/* stjuke - libsc68 player engine for the jukebox: Atari ST / Amiga SNDH & sc68.
 *
 * Fifth engine (alongside vgmjuke/gmejuke/modjuke/sidjuke). Plays .sndh/.sc68
 * via libsc68 (Motorola 68000 CPU + YM-2149 / Paula emulation). SNDH files hold
 * multiple subtunes each with its own duration; like SID/NSF the jukebox routes
 * them with --info/--track and numbered tracks. Renders PCM to ALSA (alsa_out.h).
 *
 * Playback is done by sc68 / libsc68 (C) Benjamin Gerard - http://sc68.atari.org
 * (source: https://sourceforge.net/p/sc68, GitHub mirror github.com/Zeinok/sc68),
 * GPL. This file is only a thin wrapper around it - the same way sidjuke wraps
 * libsidplayfp. libsc68 is built separately (see docs/BUILD-stjuke.md).
 *
 * Modes:
 *   stjuke --info <file>      -> "TRACKS n", "TITLE <album>", one TRK line each
 *   stjuke [--track T] [--loops N|--infinite] [--fade F] <file>
 *       stdin: inf | loops N       stdout: POS <cur> <total>
 *
 * SNDH tunes loop internally forever, so - like sidjuke - we let sc68 loop the
 * subtune and bound the play length ourselves (from the SNDH's own duration
 * metadata, or a default), applying a fade for finite loop modes.
 *
 * Link: sc68 file68 unice68 (+ emu68 io68 dial68) + ao z m pthread + asound
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include <string>
#include <vector>
#include <cctype>
#include <cstring>

#include <sc68/sc68.h>
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

// Create an sc68 instance and load a file. Returns NULL on failure.
static sc68_t* open_sc68(const char* file) {
    sc68_init_t init; memset(&init, 0, sizeof init);
    if (sc68_init(&init)) return NULL;
    sc68_create_t cr; memset(&cr, 0, sizeof cr);
    cr.sampling_rate = RATE;
    sc68_t* s = sc68_create(&cr);
    if (!s) return NULL;
    if (sc68_load_uri(s, file)) {
        fprintf(stderr, "stjuke: %s\n", sc68_error(s));
        sc68_destroy(s);
        return NULL;
    }
    return s;
}

// Parse SNDH per-subtune names from the "!#SN" tag, which sc68 does not decode.
// Layout: "!#SN" + <ntracks> big-endian word offsets (relative to the tag) +
// null-terminated name strings. Returns a 1-based vector (index 0 unused);
// entries are empty when absent/malformed. All SNDH here are unpacked.
static std::vector<std::string> sndh_subnames(const char* file, int n) {
    std::vector<std::string> out(n + 1);
    FILE* f = fopen(file, "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(f); return out; }
    std::vector<unsigned char> d((size_t)sz);
    size_t rd = fread(d.data(), 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) return out;
    long tag = -1;
    for (long p = 0; p + 4 < sz; p++)
        if (d[p]=='!' && d[p+1]=='#' && d[p+2]=='S' && d[p+3]=='N') { tag = p; break; }
    if (tag < 0) return out;
    long table_end = 4 + (long)n * 2;              // offsets sit right after the tag
    // Validate the WHOLE offset table first: a well-formed !#SN has offsets that
    // start at table_end and never decrease. Malformed tables (offsets pointing
    // into code, e.g. Pixie & Dixie) are rejected wholesale -> "Track N".
    long minoff = table_end;
    for (int i = 0; i < n; i++) {
        long op = tag + 4 + (long)i * 2;
        if (op + 1 >= sz) return out;
        int rel = (d[op] << 8) | d[op + 1];
        if (rel < minoff || tag + rel >= sz) return out;
        minoff = rel;
    }
    for (int i = 0; i < n; i++) {
        long op = tag + 4 + (long)i * 2;
        int rel = (d[op] << 8) | d[op + 1];
        long sp = tag + rel;
        std::string s;
        while (sp < sz && d[sp] && s.size() < 63) {
            char c = (char)d[sp++];
            if (c >= 32 && c < 127) s += c;
        }
        // secondary printable-ratio check on the extracted title
        int good = 0;
        for (size_t k = 0; k < s.size(); k++) {
            unsigned char c = (unsigned char)s[k];
            if (isalnum(c) || c == ' ' || strchr(".,!?'&()-:", c)) good++;
        }
        if (s.size() >= 1 && s != "n/a" && good >= (int)(s.size() * 4 / 5))
            out[i + 1] = s;
    }
    return out;
}

static int do_info(const char* file) {
    sc68_t* s = open_sc68(file);
    if (!s) { fprintf(stderr, "stjuke: open failed\n"); return 1; }
    sc68_music_info_t info;
    if (sc68_music_info(s, &info, 0, 0)) {
        fprintf(stderr, "stjuke: no info\n"); sc68_destroy(s); return 1;
    }
    int n = info.tracks; if (n < 1) n = 1;
    // Copy the album/game title now - the string buffer is reused by the
    // per-track sc68_music_info() calls below.
    std::string album = (info.album && info.album[0]) ? info.album : "";
    std::vector<std::string> snd = sndh_subnames(file, n);   // SNDH !#SN names
    printf("TRACKS %d\n", n);
    if (!album.empty()) printf("TITLE %s\n", album.c_str());
    for (int i = 1; i <= n; i++) {
        sc68_music_info_t ti;
        std::string name; int len = -1;
        if (!sc68_music_info(s, &ti, i, 0)) {
            if (ti.trk.time_ms > 0) len = (int)ti.trk.time_ms;
            // Emit a subtune name only when it's a real per-subtune title
            // (differs from the game title); most SNDH have none, so the
            // jukebox then shows "Track N".
            if (ti.title && ti.title[0] && album != ti.title) name = ti.title;
        }
        if (i < (int)snd.size() && !snd[i].empty()) name = snd[i];  // prefer !#SN
        printf("TRK %d %d %s\n", i, len, name.c_str());
    }
    fflush(stdout);
    sc68_destroy(s);
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

    sc68_t* s = open_sc68(file);
    if (!s) { fprintf(stderr, "stjuke: open failed\n"); return 1; }

    // Natural length from the SNDH's own metadata (per subtune), else default.
    double natural = DEFAULT_LEN;
    { sc68_music_info_t ti;
      if (!sc68_music_info(s, &ti, track, 0) && ti.trk.time_ms > 0)
          natural = ti.trk.time_ms / 1000.0; }

    // Loop the subtune internally forever; we bound the length ourselves so live
    // loop-mode changes work.
    if (sc68_play(s, track, SC68_INF_LOOP) < 0) {
        fprintf(stderr, "stjuke: play failed\n"); sc68_destroy(s); return 1;
    }

    AlsaOut out;
    if (out.open(RATE, 2)) { fprintf(stderr, "stjuke: ALSA open failed\n"); sc68_destroy(s); return 1; }

    std::vector<short> buf(1024 * 2);
    long long rendered = 0;
    int tick = 0;

    while (!stopReq) {
        long long total_frames = gModeInf ? -1 : (long long)(natural * RATE);
        int n = 1024;
        int code = sc68_process(s, buf.data(), &n);
        size_t got = (size_t)n;
        if (got == 0) { if (code & SC68_END) break; else continue; }

        if (total_frames > 0) {
            long long fade_frames = (long long)(fade * RATE);
            long long fade_start = total_frames - fade_frames;
            for (size_t f = 0; f < got; f++) {
                long long fpos = rendered + (long long)f;
                if (fpos >= total_frames) { got = f; break; }
                if (fade_frames > 0 && fpos > fade_start) {
                    double g = (double)(total_frames - fpos) / (double)fade_frames;
                    if (g < 0) g = 0;
                    buf[f * 2]     = (short)(buf[f * 2] * g);
                    buf[f * 2 + 1] = (short)(buf[f * 2 + 1] * g);
                }
            }
        }

        if (got > 0 && out.write(buf.data(), got)) break;
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
        if (code & SC68_END) break;   // safety (shouldn't fire with INF_LOOP)
    }

    out.drain();
    out.close();
    sc68_destroy(s);
    return 0;
}
