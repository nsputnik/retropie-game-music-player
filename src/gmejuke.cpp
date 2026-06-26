/* gmejuke - libgme player engine for the GPi Case jukebox (NSF/GBS/SPC/...).
 *
 * Twin of vgmjuke: same control protocol and the same libvgm-audio ALSA output,
 * but audio is synthesized by Game Music Emu (libgme), which handles the
 * CPU-emulated formats libvgm can't (NSF/NSFE/GBS/SPC/AY/HES/KSS/SAP).
 *
 * One subtune per process (libgme files hold many subtunes). Plays subtune
 * --track T live to ALSA, exits 0 when it ends, stops cleanly on SIGTERM.
 *
 * Modes:
 *   gmejuke --info <file>
 *       Print "TRACKS <n>" then one "TRK <i> <len_ms> <name>" line per subtune
 *       and exit. Used by the jukebox to build the playlist.
 *   gmejuke [--track T] [--loops N|--infinite] [--fade F] <file>
 *       Play subtune T (1-based, default 1). stdin commands (one per line):
 *         inf / loops N   change loop mode live (no restart)
 *       stdout: GAME/TITLE/POS lines (POS total = -1 when looping forever).
 *
 * Link: gme (static) + vgm-audio asound + pthread z stdc++
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>
#include <sys/select.h>
#include <pthread.h>
#include <string>

#include "common_def.h"
#include "audio/AudioStream.h"
#include "gme/gme.h"

static Music_Emu* gEmu = NULL;
static void* audDrv = NULL;
static UINT32 sampleRate = 44100;
static volatile sig_atomic_t stopReq = 0;
static volatile int gEnded = 0;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;

static int gTrack = 0;             // 0-based current subtune
static int gTrackCount = 1;
static bool gModeInf = false;      // current loop mode
static int gModeLoops = 2;
static long gTotalMs = -1;         // computed total for the current mode
static std::string gInbuf;

static void onSig(int sig) { (void)sig; stopReq = 1; }

// ---------------------------------------------------------------------------
// libvgm-audio (ALSA) - identical to vgmjuke
// ---------------------------------------------------------------------------
static UINT32 FillBuffer(void* drvStruct, void* userParam, UINT32 bufSize, void* data)
{
	(void)drvStruct; (void)userParam;
	int shorts = (int)(bufSize / sizeof(short));
	pthread_mutex_lock(&gLock);
	if (gEmu != NULL && !gme_track_ended(gEmu))
	{
		gme_play(gEmu, shorts, (short*)data);
		if (gme_track_ended(gEmu))
			gEnded = 1;
	}
	else
	{
		memset(data, 0x00, bufSize);
	}
	pthread_mutex_unlock(&gLock);
	return bufSize;
}

static UINT32 GetNthAudioDriver(UINT8 adrvType, INT32 drvNumber)
{
	if (drvNumber == -1)
		return (UINT32)-1;
	UINT32 drvCount = Audio_GetDriverCount();
	UINT32 lastDrv = (UINT32)-1;
	INT32 typedDrv = 0;
	for (UINT32 curDrv = 0; curDrv < drvCount; curDrv++)
	{
		AUDDRV_INFO* drvInfo;
		Audio_GetDriverInfo(curDrv, &drvInfo);
		if (drvInfo->drvType == adrvType)
		{
			lastDrv = curDrv;
			if (typedDrv == drvNumber)
				return curDrv;
			typedDrv++;
		}
	}
	if (drvNumber == -2)
		return lastDrv;
	return (UINT32)-1;
}

static int InitAudio(void)
{
	if (Audio_Init() == AERR_NODRVS)
		return 1;
	UINT32 idWavOut = GetNthAudioDriver(ADRVTYPE_OUT, -2);
	if (idWavOut == (UINT32)-1) { Audio_Deinit(); return 1; }
	if (AudioDrv_Init(idWavOut, &audDrv)) { Audio_Deinit(); return 1; }
	AUDIO_OPTS* opts = AudioDrv_GetOptions(audDrv);
	opts->sampleRate = sampleRate;
	opts->numChannels = 2;
	opts->numBitsPerSmpl = 16;
	if (AudioDrv_Start(audDrv, 0)) { AudioDrv_Deinit(&audDrv); Audio_Deinit(); return 1; }
	return 0;
}

static void DeinitAudio(void)
{
	if (audDrv != NULL)
	{
		AudioDrv_Stop(audDrv);
		AudioDrv_Deinit(&audDrv);
		audDrv = NULL;
	}
	Audio_Deinit();
}

// ---------------------------------------------------------------------------
// playback length / loop handling
// ---------------------------------------------------------------------------
static void apply_mode_locked(void)
{
	int start;
	gme_info_t* info = NULL;
	if (gme_track_info(gEmu, &info, gTrack) == 0 && info != NULL)
	{
		int intro = info->intro_length;
		int loop = info->loop_length;
		int length = info->length;
		int base = info->play_length > 0 ? info->play_length : 150000;
		gme_free_info(info);
		if (gModeInf)
			start = INT_MAX / 2;
		else if (loop > 0)
			start = intro + (gModeLoops < 0 ? 0 : gModeLoops) * loop;
		else if (length > 0)
			start = length;
		else
			start = (gModeLoops <= 0) ? base / 4 : gModeLoops * base;
	}
	else
	{
		start = gModeInf ? INT_MAX / 2 : 150000;
	}
	gme_set_fade(gEmu, start);
	gTotalMs = gModeInf ? -1 : (long)start + 8000;  // +gme default fade
}

static void print_tags(void)
{
	gme_info_t* info = NULL;
	if (gme_track_info(gEmu, &info, gTrack) == 0 && info != NULL)
	{
		if (info->game && info->game[0])
			printf("GAME %s\n", info->game);
		if (info->song && info->song[0])
			printf("TITLE %s\n", info->song);
		gme_free_info(info);
		fflush(stdout);
	}
}

static void handle_cmd(const std::string& line)
{
	if (line.empty())
		return;
	if (line[0] == 'i')            // inf
	{
		pthread_mutex_lock(&gLock);
		gModeInf = true;
		apply_mode_locked();
		pthread_mutex_unlock(&gLock);
	}
	else if (line[0] == 'l')       // loops N
	{
		const char* p = line.c_str();
		while (*p && (*p < '0' || *p > '9')) p++;
		pthread_mutex_lock(&gLock);
		gModeInf = false;
		gModeLoops = (int)strtol(p, NULL, 10);
		apply_mode_locked();
		pthread_mutex_unlock(&gLock);
	}
}

static void poll_stdin(void)
{
	fd_set rf;
	struct timeval tv;
	for (;;)
	{
		FD_ZERO(&rf); FD_SET(0, &rf);
		tv.tv_sec = 0; tv.tv_usec = 0;
		if (select(1, &rf, NULL, NULL, &tv) <= 0)
			break;
		char buf[256];
		ssize_t nr = read(0, buf, sizeof(buf));
		if (nr <= 0)
			break;
		gInbuf.append(buf, (size_t)nr);
		size_t pos;
		while ((pos = gInbuf.find('\n')) != std::string::npos)
		{
			handle_cmd(gInbuf.substr(0, pos));
			gInbuf.erase(0, pos + 1);
		}
	}
}

// ---------------------------------------------------------------------------
static int do_info(const char* file)
{
	Music_Emu* emu = NULL;
	gme_err_t err = gme_open_file(file, &emu, 44100);
	if (err != NULL || emu == NULL)
	{
		fprintf(stderr, "gmejuke: %s\n", err ? err : "open failed");
		return 1;
	}
	int n = gme_track_count(emu);
	printf("TRACKS %d\n", n);
	for (int i = 0; i < n; i++)
	{
		gme_info_t* info = NULL;
		const char* name = "";
		int len = -1;
		if (gme_track_info(emu, &info, i) == 0 && info != NULL)
		{
			name = (info->song && info->song[0]) ? info->song : "";
			len = info->length;
		}
		printf("TRK %d %d %s\n", i + 1, len, name);
		if (info) gme_free_info(info);
	}
	fflush(stdout);
	gme_delete(emu);
	return 0;
}

int main(int argc, char* argv[])
{
	int track = 1;
	bool infinite = false;
	int loops = 2;
	double fade = 4.0;
	bool info_mode = false;
	const char* file = NULL;

	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--track") && i + 1 < argc)
			track = (int)strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--loops") && i + 1 < argc)
			loops = (int)strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--fade") && i + 1 < argc)
			fade = strtod(argv[++i], NULL);
		else if (!strcmp(argv[i], "--infinite"))
			infinite = true;
		else if (!strcmp(argv[i], "--info"))
			info_mode = true;
		else
			file = argv[i];
	}
	if (file == NULL)
	{
		fprintf(stderr, "Usage: %s [--info] [--track T] [--loops N|--infinite] [--fade F] <file>\n", argv[0]);
		return 1;
	}
	if (info_mode)
		return do_info(file);

	(void)fade;
	signal(SIGTERM, onSig);
	signal(SIGINT, onSig);

	gme_err_t err = gme_open_file(file, &gEmu, sampleRate);
	if (err != NULL || gEmu == NULL)
	{
		fprintf(stderr, "gmejuke: %s\n", err ? err : "open failed");
		return 1;
	}
	gTrackCount = gme_track_count(gEmu);
	gTrack = track - 1;
	if (gTrack < 0) gTrack = 0;
	if (gTrack >= gTrackCount) gTrack = gTrackCount - 1;
	gModeInf = infinite;
	gModeLoops = loops;

	if (gme_start_track(gEmu, gTrack) != NULL)
	{
		fprintf(stderr, "gmejuke: start_track failed\n");
		gme_delete(gEmu);
		return 1;
	}
	apply_mode_locked();
	print_tags();

	if (InitAudio())
	{
		fprintf(stderr, "gmejuke: audio init failed\n");
		gme_delete(gEmu);
		return 1;
	}
	if (AudioDrv_SetCallback(audDrv, FillBuffer, NULL))
	{
		fprintf(stderr, "gmejuke: cannot set audio callback\n");
		DeinitAudio();
		gme_delete(gEmu);
		return 1;
	}

	int tick = 0;
	while (!stopReq && !gEnded)
	{
		usleep(100 * 1000);
		poll_stdin();
		if (++tick >= 2)  // ~200ms
		{
			tick = 0;
			pthread_mutex_lock(&gLock);
			double cur = gme_tell(gEmu) / 1000.0;
			pthread_mutex_unlock(&gLock);
			double tot = gModeInf ? -1.0 : gTotalMs / 1000.0;
			printf("POS %.2f %.2f\n", cur, tot);
			fflush(stdout);
		}
	}

	AudioDrv_SetCallback(audDrv, NULL, NULL);  // blocks until render thread idle
	DeinitAudio();
	gme_delete(gEmu);
	gEmu = NULL;
	return 0;
}
