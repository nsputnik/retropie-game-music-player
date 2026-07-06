/* psgjuke - libpsgplay engine for the jukebox: Atari ST / STE SNDH via PSG Play.
 *
 * Sixth engine (alongside vgmjuke/gmejuke/modjuke/sidjuke/stjuke). Plays .sndh
 * via psgplay (Motorola 68000 + YM2149 PSG + *STE DMA sound* + LMC1992 mixer
 * emulation). Unlike sc68 (the stjuke engine), psgplay emulates the STE DMA
 * sound hardware, so it plays the large class of STe tunes - and DMA-sample
 * tracks - that sc68 renders silent.
 *
 * Preferred over stjuke for .sndh. On any failure - the data is not SNDH (e.g.
 * ICE-packed or a .sc68 file), or psgplay cannot initialise it - psgjuke exec()s
 * the sc68 stjuke engine sitting next to it, which depacks ICE and covers
 * .sc68 / Amiga and anything psgplay rejects. Same CLI + POS protocol, so the
 * hand-off is transparent to jukebox.py.
 *
 * psgplay (C) Fredrik Noring, GPL-2.0 - https://github.com/frno7/psgplay
 * This file is a thin wrapper, like sidjuke wraps libsidplayfp. libpsgplay is
 * built separately (see docs/BUILD-psgjuke.md). psgplay exposes native SNDH
 * subtune count / duration / names, so - unlike stjuke - no hand-rolled tag
 * parsing is needed here.
 *
 * Modes:
 *   psgjuke --info <file>   -> "TRACKS n", "TITLE <album>", one TRK line each
 *   psgjuke [--track T] [--loops N|--infinite] [--fade F] <file>
 *       stdin: inf | loops N       stdout: POS <cur> <total>
 *
 * SNDH subtunes loop forever; we bound the length from the SNDH's own TIME tag
 * (per subtune) or a default, applying a fade for finite loop modes - the same
 * shape as stjuke.
 *
 * Link: psgplay + asound (alsa_out.h)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include <string>
#include <vector>

extern "C" {
#include <psgplay/psgplay.h>
#include <psgplay/stereo.h>
#include <psgplay/sndh.h>
}
#include "alsa_out.h"

static const int RATE = 44100;
static const double DEFAULT_LEN = 180.0;   // fallback when SNDH carries no length

static volatile sig_atomic_t stopReq = 0;
static void onSig(int sig) { (void)sig; stopReq = 1; }

static bool gModeInf = false;
static int gModeLoops = 2;
static std::string gInbuf;

static void handle_cmd(const std::string& line) {
    if (line.empty()) return;
    if (line[0] == 'i') gModeInf = true;               // "inf"
    else if (line[0] == 'l') {                          // "loops N"
        const char* p = line.c_str();
        while (*p && (*p < '0' || *p > '9')) p++;
        gModeInf = false;
        gModeLoops = (int)strtol(p, NULL, 10);
    }
}

static void poll_stdin(void) {
    fd_set rf; struct timeval tv;
    for (;;) {
        FD_ZERO(&rf); FD_SET(0, &rf);
        tv.tv_sec = 0; tv.tv_usec = 0;
        if (select(1, &rf, NULL, NULL, &tv) <= 0) break;
        char b[256];
        ssize_t nr = read(0, b, sizeof b);
        if (nr <= 0) break;
        gInbuf.append(b, (size_t)nr);
        size_t pos;
        while ((pos = gInbuf.find('\n')) != std::string::npos) {
            handle_cmd(gInbuf.substr(0, pos));
            gInbuf.erase(0, pos + 1);
        }
    }
}

// Read the whole file into buf (psgplay_init needs the data in memory, and it
// must be uncompressed). Returns false on error.
static bool read_file(const char* path, std::vector<unsigned char>& buf) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(f); return false; }
    buf.resize((size_t)sz);
    size_t rd = fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);
    return rd == (size_t)sz;
}

// Hand off to the sc68 engine (stjuke) sitting next to us. It depacks ICE and
// covers .sc68 / Amiga and any tune psgplay can't load. Same args + protocol.
static void exec_stjuke(char** argv) {
    std::string self = argv[0];
    size_t s = self.find_last_of('/');
    std::string dir = (s == std::string::npos) ? std::string(".") : self.substr(0, s);
    std::string st = dir + "/stjuke";
    execv(st.c_str(), argv);       // next to us (install dir)
    execvp("stjuke", argv);        // else via PATH
    fprintf(stderr, "psgjuke: fallback to stjuke failed\n");
    _exit(1);
}

static int do_info(const unsigned char* d, size_t sz, char** argv) {
    if (!sndh_identify(d, sz)) exec_stjuke(argv);      // not SNDH -> sc68 info
    int n = 0;
    if (!sndh_tag_subtune_count(&n, d, sz) || n < 1) n = 1;
    char album[256] = {0};
    bool has_album = sndh_tag_title(album, sizeof album, d, sz) && album[0];
    printf("TRACKS %d\n", n);
    if (has_album) printf("TITLE %s\n", album);
    for (int i = 1; i <= n; i++) {
        int len = -1; float dur = 0;
        if (sndh_tag_subtune_time(&dur, i, d, sz) && dur > 0)
            len = (int)(dur * 1000.0f);
        // psgplay decodes the SNDH "!#SN" per-subtune names natively; emit one
        // only when it's a real per-subtune title (differs from the game title),
        // else the jukebox shows "Track N".
        char name[256] = {0};
        std::string nm;
        if (sndh_tag_subtune_name(name, sizeof name, i, d, sz) &&
            name[0] && strcmp(name, album) != 0)
            nm = name;
        printf("TRK %d %d %s\n", i, len, nm.c_str());
    }
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

    std::vector<unsigned char> data;
    if (!read_file(file, data)) exec_stjuke(argv);     // unreadable -> let sc68 try

    if (info_mode) return do_info(data.data(), data.size(), argv);

    // Not SNDH (e.g. .sc68 or ICE-packed) -> hand to the sc68 engine.
    if (!sndh_identify(data.data(), data.size())) exec_stjuke(argv);

    signal(SIGTERM, onSig);
    signal(SIGINT, onSig);

    struct psgplay* pp = psgplay_init(data.data(), data.size(), track, RATE);
    if (!pp) exec_stjuke(argv);      // psgplay couldn't load it -> sc68 fallback

    // Natural length from the SNDH TIME tag (per subtune), else default.
    double natural = DEFAULT_LEN;
    { float dur = 0;
      if (sndh_tag_subtune_time(&dur, track, data.data(), data.size()) && dur > 0)
          natural = dur; }

    AlsaOut out;
    if (out.open(RATE, 2)) { fprintf(stderr, "psgjuke: ALSA open failed\n"); psgplay_free(pp); return 1; }

    std::vector<struct psgplay_stereo> buf(1024);
    long long rendered = 0;
    int tick = 0;

    while (!stopReq) {
        long long total_frames = gModeInf ? -1 : (long long)(natural * RATE);
        ssize_t r = psgplay_read_stereo(pp, buf.data(), buf.size());
        if (r <= 0) break;
        size_t got = (size_t)r;

        if (total_frames > 0) {
            long long fade_frames = (long long)(fade * RATE);
            long long fade_start = total_frames - fade_frames;
            for (size_t f = 0; f < got; f++) {
                long long fpos = rendered + (long long)f;
                if (fpos >= total_frames) { got = f; break; }
                if (fade_frames > 0 && fpos > fade_start) {
                    double g = (double)(total_frames - fpos) / (double)fade_frames;
                    if (g < 0) g = 0;
                    buf[f].left  = (int16_t)(buf[f].left  * g);
                    buf[f].right = (int16_t)(buf[f].right * g);
                }
            }
        }

        if (got > 0 && out.write((const short*)buf.data(), got)) break;
        rendered += (long long)got;

        poll_stdin();
        if (++tick >= 20) {
            tick = 0;
            double cur = (double)rendered / RATE;
            double tot = gModeInf ? -1.0 : natural;
            printf("POS %.2f %.2f\n", cur, tot);
            fflush(stdout);
        }
        if (total_frames > 0 && rendered >= total_frames) break;
    }

    out.drain();
    out.close();
    psgplay_free(pp);
    return 0;
}
