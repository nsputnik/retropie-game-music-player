/* modjuke - libopenmpt player engine for the GPi Case jukebox (Amiga/tracker).
 *
 * Third engine alongside vgmjuke (libvgm) and gmejuke (libgme). Plays tracker
 * modules - MOD/XM/S3M/IT/MTM/... - via libopenmpt. One file = one song (like
 * the VGM engine), so the jukebox routes it as a sibling-file album and shows
 * filenames as track names. Renders PCM itself and writes to ALSA (alsa_out.h).
 *
 * Modes (same contract as vgmjuke/gmejuke):
 *   modjuke --info <file>
 *       Print "TRACKS 1" and (if present) "TITLE <embedded module title>".
 *   modjuke [--track T] [--loops N|--infinite] [--fade F] <file>
 *       Play the module live to ALSA, exit 0 when it ends, stop on SIGTERM.
 *       stdin (one command per line):  inf | loops N   (live loop change)
 *       stdout: POS <cur> <total>      (total = -1 while looping forever)
 *
 * Link: -lopenmpt -lasound
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include <string>
#include <vector>

#include <libopenmpt/libopenmpt.h>
#include "alsa_out.h"

static const int RATE = 48000;
static const int CHANS = 2;

static volatile sig_atomic_t stopReq = 0;
static void onSig(int sig) { (void)sig; stopReq = 1; }

// Current loop mode, updated live from stdin.
static bool gModeInf = false;
static int gModeLoops = 2;
static std::string gInbuf;

static void handle_cmd(const std::string& line) {
    if (line.empty()) return;
    if (line[0] == 'i') {                 // inf
        gModeInf = true;
    } else if (line[0] == 'l') {          // loops N
        const char* p = line.c_str();
        while (*p && (*p < '0' || *p > '9')) p++;
        gModeInf = false;
        gModeLoops = (int)strtol(p, NULL, 10);
    }
}

static void poll_stdin() {
    fd_set rf;
    struct timeval tv;
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

static bool read_file(const char* path, std::vector<char>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

static openmpt_module* open_mod(const char* path) {
    std::vector<char> data;
    if (!read_file(path, data)) return NULL;
    return openmpt_module_create_from_memory2(
        data.data(), data.size(),
        NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

static int do_info(const char* file) {
    openmpt_module* mod = open_mod(file);
    if (!mod) { fprintf(stderr, "modjuke: open failed\n"); return 1; }
    printf("TRACKS 1\n");
    const char* title = openmpt_module_get_metadata(mod, "title");
    if (title && title[0]) printf("TITLE %s\n", title);
    if (title) openmpt_free_string(title);
    fflush(stdout);
    openmpt_module_destroy(mod);
    return 0;
}

int main(int argc, char* argv[]) {
    int track = 0;                        // subsong (0-based); default whole
    double fade = 4.0;
    bool info_mode = false;
    const char* file = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--track") && i + 1 < argc)
            track = (int)strtol(argv[++i], NULL, 10) - 1;
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

    openmpt_module* mod = open_mod(file);
    if (!mod) { fprintf(stderr, "modjuke: open failed\n"); return 1; }
    if (track > 0) openmpt_module_select_subsong(mod, track);

    double duration = openmpt_module_get_duration_seconds(mod);
    if (duration <= 0) duration = 150.0;

    AlsaOut out;
    if (out.open(RATE, CHANS)) {
        fprintf(stderr, "modjuke: ALSA open failed\n");
        openmpt_module_destroy(mod);
        return 1;
    }

    // Repeat forever; we bound finite modes ourselves so live loop changes work.
    openmpt_module_set_repeat_count(mod, -1);

    const size_t FRAMES = 1024;
    std::vector<short> buf(FRAMES * CHANS);
    long long rendered = 0;               // cumulative frames emitted
    int tick = 0;

    while (!stopReq) {
        long long total_frames = gModeInf ? -1
            : (long long)(duration * (gModeLoops + 1) * RATE);

        size_t got = openmpt_module_read_interleaved_stereo(
            mod, RATE, FRAMES, buf.data());
        if (got == 0) break;              // module ended (shouldn't with repeat -1)

        // Finite mode: fade + stop at the computed end.
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
        if (++tick >= 20) {               // ~ every 20 buffers
            tick = 0;
            double cur = (double)rendered / RATE;
            double tot = gModeInf ? -1.0 : duration * (gModeLoops + 1);
            printf("POS %.2f %.2f\n", cur, tot);
            fflush(stdout);
        }

        if (total_frames > 0 && rendered >= total_frames) break;
    }

    out.drain();
    out.close();
    openmpt_module_destroy(mod);
    return 0;
}
