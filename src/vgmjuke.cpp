/* vgmjuke - minimal live libvgm player for the GPi Case jukebox.
 *
 * Plays a single VGM/VGZ/GYM/S98/DRO file live to ALSA with a caller-chosen
 * loop count and fade, exits 0 when the track ends naturally, and stops
 * cleanly on SIGTERM (used by the Python jukebox for stop / skip / loop change).
 *
 * It also prints machine-readable status lines to stdout for the UI:
 *   GAME  <game tag>
 *   TITLE <title tag>
 *   POS   <cur_seconds> <total_seconds>   (total = -1 when looping forever)
 *
 * And accepts commands on stdin (one per line) to change loop mode *live*,
 * without restarting the track:
 *   inf        -> loop forever
 *   loops N    -> N loops then fade   (also: "l N")
 *
 * Usage: vgmjuke [--loops N] [--fade F] [--infinite] <file>
 *   --infinite => loop forever (jukebox kills us on navigation); POS total = -1
 *   --loops N  => literal loop count (N=0 means zero loops of the looped section)
 *
 * Link: vgm-player vgm-emu vgm-utils vgm-audio asound z pthread dl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include <string>

#include "common_def.h"
#include "utils/DataLoader.h"
#include "utils/FileLoader.h"
#include "player/playerbase.hpp"
#include "player/vgmplayer.hpp"
#include "player/s98player.hpp"
#include "player/droplayer.hpp"
#include "player/gymplayer.hpp"
#include "player/playera.hpp"
#include "audio/AudioStream.h"

static PlayerA mainPlr;
static void* audDrv = NULL;
static UINT32 sampleRate = 44100;
static volatile UINT8 playEnd = 0;
static volatile sig_atomic_t stopReq = 0;

// live-control state (loop mode can change mid-track via stdin commands)
static PlayerBase* gPlayer = NULL;
static bool gIsVgm = false;
static double gFade = 4.0;
static volatile bool gInfinite = false;
static std::string gInbuf;

static const UINT8 PBTIME_CUR = PLAYTIME_LOOP_INCL | PLAYTIME_TIME_FILE;
static const UINT8 PBTIME_TOT = PLAYTIME_LOOP_INCL | PLAYTIME_TIME_FILE | PLAYTIME_WITH_FADE;

static void onSig(int sig)
{
	(void)sig;
	stopReq = 1;
}

// pull-style render callback invoked by the ALSA driver thread
static UINT32 FillBuffer(void* drvStruct, void* userParam, UINT32 bufSize, void* data)
{
	(void)drvStruct;
	PlayerA& plr = *(PlayerA*)userParam;
	if (!(plr.GetState() & PLAYSTATE_PLAY))
	{
		memset(data, 0x00, bufSize);
		return bufSize;
	}
	return plr.Render(bufSize, data);
}

static UINT8 EventCallback(PlayerBase* player, void* userParam, UINT8 evtType, void* evtParam)
{
	(void)player; (void)userParam; (void)evtParam;
	if (evtType == PLREVT_END)
		playEnd = 1;
	return 0x00;
}

static DATA_LOADER* RequestFileCallback(void* userParam, PlayerBase* player, const char* fileName)
{
	(void)userParam; (void)player;
	DATA_LOADER* loader = FileLoader_Init(fileName);
	if (loader == NULL)
		return NULL;
	if (!DataLoader_Load(loader))
		return loader;
	DataLoader_Deinit(loader);
	return NULL;
}

static UINT32 GetNthAudioDriver(UINT8 adrvType, INT32 drvNumber)
{
	// -1 = none, -2 = last found (we use -2 to grab ALSA, the last OUT driver)
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
	if (idWavOut == (UINT32)-1)
	{
		Audio_Deinit();
		return 1;
	}
	if (AudioDrv_Init(idWavOut, &audDrv))
	{
		Audio_Deinit();
		return 1;
	}
	AUDIO_OPTS* opts = AudioDrv_GetOptions(audDrv);
	opts->sampleRate = sampleRate;
	opts->numChannels = 2;
	opts->numBitsPerSmpl = 16;
	UINT32 smplSize = opts->numChannels * opts->numBitsPerSmpl / 8;
	if (AudioDrv_Start(audDrv, 0))
	{
		AudioDrv_Deinit(&audDrv);
		Audio_Deinit();
		return 1;
	}
	UINT32 smplAlloc = AudioDrv_GetBufferSize(audDrv) / smplSize;
	mainPlr.SetOutputSettings(opts->sampleRate, opts->numChannels, opts->numBitsPerSmpl, smplAlloc);
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

static void PrintTag(const char* label, const char* const* tagList, const char* key)
{
	for (const char* const* t = tagList; *t; t += 2)
	{
		if (!strcmp(t[0], key) && t[1] != NULL && t[1][0] != '\0')
		{
			printf("%s %s\n", label, t[1]);
			return;
		}
	}
}

// Change loop mode live (no restart). inf = loop forever; otherwise n loops.
static void apply_mode(bool inf, UINT32 n)
{
	if (inf)
	{
		mainPlr.SetLoopCount(1000000u);
		mainPlr.SetFadeSamples(0);
		gInfinite = true;
	}
	else
	{
		UINT32 lc = n;
		if (gIsVgm && gPlayer != NULL)
			lc = dynamic_cast<VGMPlayer*>(gPlayer)->GetModifiedLoopCount(n);
		mainPlr.SetLoopCount(lc);
		mainPlr.SetFadeSamples((UINT32)(sampleRate * gFade));
		gInfinite = false;
	}
}

static void handle_cmd(const std::string& line)
{
	if (line.empty())
		return;
	if (line[0] == 'i')            // "inf"
		apply_mode(true, 0);
	else if (line[0] == 'l')       // "loops N" or "l N"
	{
		const char* p = line.c_str();
		while (*p && (*p < '0' || *p > '9'))
			p++;
		apply_mode(false, (UINT32)strtoul(p, NULL, 10));
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

int main(int argc, char* argv[])
{
	UINT32 loops = 2;
	double fade = 4.0;
	bool infinite = false;
	const char* file = NULL;

	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--loops") && i + 1 < argc)
			loops = (UINT32)strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--fade") && i + 1 < argc)
			fade = strtod(argv[++i], NULL);
		else if (!strcmp(argv[i], "--infinite"))
			infinite = true;
		else
			file = argv[i];
	}
	if (file == NULL)
	{
		fprintf(stderr, "Usage: %s [--loops N] [--fade F] [--infinite] <file>\n", argv[0]);
		return 1;
	}

	gFade = fade;  // remembered finite-mode fade (kept across live mode changes)
	if (infinite)
		loops = 1000000u;

	signal(SIGTERM, onSig);
	signal(SIGINT, onSig);

	if (InitAudio())
	{
		fprintf(stderr, "vgmjuke: audio init failed\n");
		return 1;
	}

	mainPlr.RegisterPlayerEngine(new VGMPlayer);
	mainPlr.RegisterPlayerEngine(new S98Player);
	mainPlr.RegisterPlayerEngine(new DROPlayer);
	mainPlr.RegisterPlayerEngine(new GYMPlayer);
	mainPlr.SetEventCallback(EventCallback, NULL);
	mainPlr.SetFileReqCallback(RequestFileCallback, NULL);

	{
		PlayerA::Config cfg = mainPlr.GetConfiguration();
		cfg.masterVol = 0x10000;
		cfg.loopCount = loops;
		cfg.fadeSmpls = infinite ? 0 : (UINT32)(sampleRate * fade);
		cfg.endSilenceSmpls = infinite ? 0 : sampleRate / 2;
		cfg.pbSpeed = 1.0;
		mainPlr.SetConfiguration(cfg);
	}

	DATA_LOADER* dLoad = FileLoader_Init(file);
	if (dLoad == NULL)
	{
		fprintf(stderr, "vgmjuke: cannot open %s\n", file);
		DeinitAudio();
		return 1;
	}
	DataLoader_SetPreloadBytes(dLoad, 0x100);
	if (DataLoader_Load(dLoad))
	{
		fprintf(stderr, "vgmjuke: load error\n");
		DataLoader_Deinit(dLoad);
		DeinitAudio();
		return 1;
	}
	if (mainPlr.LoadFile(dLoad))
	{
		fprintf(stderr, "vgmjuke: unsupported file\n");
		DataLoader_Deinit(dLoad);
		DeinitAudio();
		return 1;
	}

	PlayerBase* player = mainPlr.GetPlayer();
	gPlayer = player;
	gIsVgm = (player->GetPlayerType() == FCC_VGM);
	gInfinite = infinite;
	mainPlr.SetLoopCount(loops);
	if (gIsVgm)
	{
		VGMPlayer* vgmplay = dynamic_cast<VGMPlayer*>(player);
		mainPlr.SetLoopCount(vgmplay->GetModifiedLoopCount(loops));
	}

	const char* const* tags = player->GetTags();
	PrintTag("GAME", tags, "GAME");
	PrintTag("TITLE", tags, "TITLE");
	// Report whether this file carries a loop point (0 loop ticks = none, e.g.
	// many Genesis VGM rips). The jukebox shows "No loop point" and disables the
	// loop button for such tracks.
	printf("LOOP %d\n", player->GetLoopTicks() > 0 ? 1 : 0);
	fflush(stdout);

	mainPlr.Start();

	if (AudioDrv_SetCallback(audDrv, FillBuffer, &mainPlr))
	{
		fprintf(stderr, "vgmjuke: cannot set audio callback\n");
		mainPlr.Stop();
		mainPlr.UnloadFile();
		DataLoader_Deinit(dLoad);
		DeinitAudio();
		return 1;
	}

	int tick = 0;
	while (!playEnd && !stopReq)
	{
		usleep(100 * 1000);
		poll_stdin();  // live loop-mode changes
		if (++tick >= 2)  // ~200ms
		{
			tick = 0;
			double cur = mainPlr.GetCurTime(PBTIME_CUR);
			double tot = gInfinite ? -1.0 : mainPlr.GetTotalTime(PBTIME_TOT);
			printf("POS %.2f %.2f\n", cur, tot);
			fflush(stdout);
		}
	}

	// remove callback first - this blocks until the render thread is idle,
	// so no Render() runs concurrently with teardown
	AudioDrv_SetCallback(audDrv, NULL, NULL);
	mainPlr.Stop();
	mainPlr.UnloadFile();
	DataLoader_Deinit(dLoad);
	mainPlr.UnregisterAllPlayers();
	DeinitAudio();

	// exit 0 on natural end (jukebox advances) or clean SIGTERM stop
	return 0;
}
