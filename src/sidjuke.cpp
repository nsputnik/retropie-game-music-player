/* sidjuke - libsidplayfp player engine for the GPi Case jukebox (C64 SID).
 *
 * Fourth engine (alongside vgmjuke/gmejuke/modjuke). Plays .sid/.psid via the
 * reSIDfp core. SID files hold multiple subtunes with no per-subtune length, so
 * - like NSF - the jukebox routes them with --info/--track and numbered tracks.
 * Renders PCM and writes to ALSA (alsa_out.h).
 *
 * SID has no natural end or loop points, so: --infinite plays forever
 * (POS total = -1); a finite --loops mode plays a fixed length with a fade,
 * i.e. the loop-count control acts as "let it run then fade" for SID.
 *
 * Modes:
 *   sidjuke --info <file>            -> "TRACKS n", "TITLE <name>", TRK lines
 *   sidjuke [--track T] [--loops N|--infinite] [--fade F] <file>
 *       stdin: inf | loops N         stdout: POS <cur> <total>
 *
 * Link: -lsidplayfp -lasound
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include <string>
#include <vector>

#include <sidplayfp/sidplayfp.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidTuneInfo.h>
#include <sidplayfp/SidInfo.h>
#include <sidplayfp/builders/residfp.h>
#include "alsa_out.h"

static const int RATE = 48000;
static const double DEFAULT_LEN = 180.0;   // finite-mode play length (SID has none)

static volatile sig_atomic_t stopReq = 0;
static void onSig(int sig) { (void)sig; stopReq = 1; }

static bool gModeInf = false;
static int gModeLoops = 2;
static std::string gInbuf;

static void handle_cmd(const std::string& line) {
    if (line.empty()) return;
    if (line[0] == 'i') gModeInf = true;
    else if (line[0] == 'l') {
        const char* p = line.c_str();
        while (*p && (*p < '0' || *p > '9')) p++;
        gModeInf = false;
        gModeLoops = (int)strtol(p, NULL, 10);
    }
}

static void poll_stdin() {
    fd_set rf; struct timeval tv;
    for (;;) {
        FD_ZERO(&rf); FD_SET(0, &rf);
        tv.tv_sec = 0; tv.tv_usec = 0;
        if (select(1, &rf, NULL, NULL, &tv) <= 0) break;
        char buf[256];
        ssize_t nr = read(0, buf, sizeof(buf));
        if (nr <= 0) break;
        gInbuf.append(buf, (size_t)nr);
        size_t pos;
        while ((pos = gInbuf.find('\n')) != std::string::npos) {
            handle_cmd(gInbuf.substr(0, pos));
            gInbuf.erase(0, pos + 1);
        }
    }
}

static int do_info(const char* file) {
    SidTune tune(file);
    if (!tune.getStatus()) { fprintf(stderr, "sidjuke: bad tune\n"); return 1; }
    const SidTuneInfo* info = tune.getInfo();
    unsigned n = info ? info->songs() : 0;
    if (n == 0) return 1;
    printf("TRACKS %u\n", n);
    if (info && info->numberOfInfoStrings() > 0 && info->infoString(0))
        printf("TITLE %s\n", info->infoString(0));
    // SID subtunes carry no individual names -> empty; UI shows "Track i".
    for (unsigned i = 1; i <= n; i++)
        printf("TRK %u -1 \n", i);
    fflush(stdout);
    return 0;
}

int main(int argc, char* argv[]) {
    int track = 1;
    double fade = 4.0;
    bool info_mode = false;
    const char* file = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--track") && i + 1 < argc)
            track = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--loops") && i + 1 < argc)
            gModeLoops = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--fade") && i + 1 < argc)
            fade = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--infinite"))
            gModeInf = true;
        else if (!strcmp(argv[i], "--info"))
            info_mode = true;
        else
            file = argv[i];
    }
    if (!file) {
        fprintf(stderr, "Usage: %s [--info] [--track T] [--loops N|--infinite] [--fade F] <file>\n", argv[0]);
        return 1;
    }
    if (info_mode) return do_info(file);

    signal(SIGTERM, onSig);
    signal(SIGINT, onSig);

    SidTune tune(file);
    if (!tune.getStatus()) { fprintf(stderr, "sidjuke: bad tune\n"); return 1; }
    tune.selectSong((unsigned)track);

    sidplayfp engine;
    ReSIDfpBuilder builder("residfp");
    builder.create(engine.info().maxsids());
    if (!builder.getStatus()) { fprintf(stderr, "sidjuke: builder failed\n"); return 1; }
    builder.filter(true);

    SidConfig cfg = engine.config();
    cfg.frequency = RATE;
    cfg.playback = SidConfig::STEREO;
    cfg.samplingMethod = SidConfig::INTERPOLATE;
    cfg.sidEmulation = &builder;
    if (!engine.config(cfg)) { fprintf(stderr, "sidjuke: %s\n", engine.error()); return 1; }

    if (!engine.load(&tune)) { fprintf(stderr, "sidjuke: %s\n", engine.error()); return 1; }

    AlsaOut out;
    if (out.open(RATE, 2)) { fprintf(stderr, "sidjuke: ALSA open failed\n"); return 1; }

    const size_t FRAMES = 1024;
    std::vector<short> buf(FRAMES * 2);
    long long rendered = 0;
    int tick = 0;

    while (!stopReq) {
        long long total_frames = gModeInf ? -1 : (long long)(DEFAULT_LEN * RATE);

        // engine.play fills `count` shorts (stereo => 2 per frame)
        uint_least32_t shorts = engine.play(buf.data(), (uint_least32_t)(FRAMES * 2));
        size_t got = shorts / 2;
        if (got == 0) break;

        if (total_frames > 0) {
            long long fade_frames = (long long)(fade * RATE);
            long long fade_start = total_frames - fade_frames;
            for (size_t s = 0; s < got; s++) {
                long long fpos = rendered + (long long)s;
                if (fpos >= total_frames) { got = s; break; }
                if (fade_frames > 0 && fpos > fade_start) {
                    double g = (double)(total_frames - fpos) / (double)fade_frames;
                    if (g < 0) g = 0;
                    buf[s * 2]     = (short)(buf[s * 2] * g);
                    buf[s * 2 + 1] = (short)(buf[s * 2 + 1] * g);
                }
            }
        }

        if (got > 0 && out.write(buf.data(), got)) break;
        rendered += (long long)got;

        poll_stdin();
        if (++tick >= 20) {
            tick = 0;
            double cur = (double)rendered / RATE;
            double tot = gModeInf ? -1.0 : DEFAULT_LEN;
            printf("POS %.2f %.2f\n", cur, tot);
            fflush(stdout);
        }

        if (total_frames > 0 && rendered >= total_frames) break;
    }

    out.drain();
    out.close();
    return 0;
}
