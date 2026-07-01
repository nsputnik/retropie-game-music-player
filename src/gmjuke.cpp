/* gmjuke - FluidSynth player engine for the GPi Case jukebox (General MIDI).
 *
 * Fifth engine. Plays .mid/.midi through FluidSynth using a General MIDI
 * SoundFont (SC-55 style). A MIDI file carries only note/program-change events,
 * so a SoundFont is required to make sound; the file is routed here by its
 * category folder (roms/gme/GM/...), the same way box art is resolved.
 *
 * SoundFont lookup (first that exists):
 *   --soundfont PATH  |  $GMJUKE_SF2  |  <dir-of-binary>/gm.sf2
 *   |  first *.sf2 in <dir-of-binary>/soundfonts/
 * AWE32 pair bonus: if "<midi-basename>.sf2" sits next to the .mid, it is loaded
 * on top (bank offset 1) so AWE32 MIDI+SF2 pairs voice their custom bank.
 *
 * One file = one song (sibling-file album, filename track names). MIDI length is
 * awkward to know up front, so POS total = -1 (elapsed only) and the engine
 * exits when playback finishes.
 *
 * Modes:
 *   gmjuke --info <file>   -> "TRACKS 1"
 *   gmjuke [--loops N|--infinite] [--fade F] [--soundfont SF2] <file>
 *       stdin: inf | loops N     stdout: POS <cur> -1
 *
 * Link: -lfluidsynth -lasound
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <libgen.h>
#include <dirent.h>
#include <sys/select.h>
#include <string>
#include <vector>

#include <fluidsynth.h>
#include "alsa_out.h"

static const int RATE = 48000;

static volatile sig_atomic_t stopReq = 0;
static void onSig(int sig) { (void)sig; stopReq = 1; }

static bool gModeInf = false;
static int gModeLoops = 2;
static std::string gInbuf;
static fluid_player_t* gPlayer = NULL;

static int loop_count_for(bool inf, int loops) {
    return inf ? -1 : (loops + 1);   // play (loops+1) times, -1 = forever
}

static void handle_cmd(const std::string& line) {
    if (line.empty() || !gPlayer) return;
    if (line[0] == 'i') { gModeInf = true; }
    else if (line[0] == 'l') {
        const char* p = line.c_str();
        while (*p && (*p < '0' || *p > '9')) p++;
        gModeInf = false;
        gModeLoops = (int)strtol(p, NULL, 10);
    }
    fluid_player_set_loop(gPlayer, loop_count_for(gModeInf, gModeLoops));
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

static std::string dir_of(const char* path) {
    std::vector<char> tmp(path, path + strlen(path) + 1);
    return std::string(dirname(tmp.data()));
}

static std::string base_noext(const char* path) {
    std::vector<char> tmp(path, path + strlen(path) + 1);
    std::string b = basename(tmp.data());
    size_t dot = b.rfind('.');
    if (dot != std::string::npos) b.erase(dot);
    return b;
}

static bool exists(const std::string& p) { return access(p.c_str(), R_OK) == 0; }

// Resolve the default GM SoundFont path.
static std::string resolve_soundfont(const char* argSf, const char* selfPath) {
    if (argSf && exists(argSf)) return argSf;
    const char* env = getenv("GMJUKE_SF2");
    if (env && exists(env)) return env;
    std::string here = dir_of(selfPath);
    if (exists(here + "/gm.sf2")) return here + "/gm.sf2";
    std::string sfdir = here + "/soundfonts";
    DIR* d = opendir(sfdir.c_str());
    if (d) {
        struct dirent* e;
        std::string found;
        while ((e = readdir(d)) != NULL) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.substr(n.size() - 4) == ".sf2") { found = sfdir + "/" + n; break; }
        }
        closedir(d);
        if (!found.empty()) return found;
    }
    return "";
}

int main(int argc, char* argv[]) {
    double fade = 4.0;
    bool info_mode = false;
    const char* file = NULL;
    const char* argSf = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--loops") && i + 1 < argc)
            gModeLoops = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--fade") && i + 1 < argc)
            fade = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--soundfont") && i + 1 < argc)
            argSf = argv[++i];
        else if (!strcmp(argv[i], "--infinite"))
            gModeInf = true;
        else if (!strcmp(argv[i], "--track") && i + 1 < argc)
            ++i;                          // MIDI has no subtunes; ignore
        else if (!strcmp(argv[i], "--info"))
            info_mode = true;
        else
            file = argv[i];
    }
    (void)fade;
    if (!file) {
        fprintf(stderr, "Usage: %s [--info] [--loops N|--infinite] [--fade F] [--soundfont SF2] <file>\n", argv[0]);
        return 1;
    }
    if (info_mode) { printf("TRACKS 1\n"); fflush(stdout); return 0; }

    std::string sf = resolve_soundfont(argSf, argv[0]);
    if (sf.empty()) {
        fprintf(stderr, "gmjuke: no GM SoundFont found (set --soundfont or $GMJUKE_SF2)\n");
        return 1;
    }

    signal(SIGTERM, onSig);
    signal(SIGINT, onSig);

    fluid_settings_t* settings = new_fluid_settings();
    fluid_settings_setnum(settings, "synth.sample-rate", (double)RATE);
    fluid_settings_setint(settings, "synth.audio-channels", 1);   // one stereo pair
    fluid_synth_t* synth = new_fluid_synth(settings);

    if (fluid_synth_sfload(synth, sf.c_str(), 1) == FLUID_FAILED) {
        fprintf(stderr, "gmjuke: failed to load SoundFont %s\n", sf.c_str());
        delete_fluid_synth(synth); delete_fluid_settings(settings);
        return 1;
    }
    // AWE32 pair: same-named sidecar .sf2 -> bank offset 1.
    std::string sidecar = dir_of(file) + "/" + base_noext(file) + ".sf2";
    if (exists(sidecar)) {
        int sid = fluid_synth_sfload(synth, sidecar.c_str(), 1);
        if (sid != FLUID_FAILED)
            fluid_synth_set_bank_offset(synth, sid, 1);
    }

    gPlayer = new_fluid_player(synth);
    if (fluid_player_add(gPlayer, file) == FLUID_FAILED) {
        fprintf(stderr, "gmjuke: cannot load MIDI %s\n", file);
        delete_fluid_player(gPlayer); delete_fluid_synth(synth); delete_fluid_settings(settings);
        return 1;
    }
    fluid_player_set_loop(gPlayer, loop_count_for(gModeInf, gModeLoops));
    fluid_player_play(gPlayer);

    AlsaOut out;
    if (out.open(RATE, 2)) {
        fprintf(stderr, "gmjuke: ALSA open failed\n");
        delete_fluid_player(gPlayer); delete_fluid_synth(synth); delete_fluid_settings(settings);
        return 1;
    }

    const int FRAMES = 1024;
    std::vector<short> buf(FRAMES * 2);
    long long rendered = 0;
    int tick = 0;

    while (!stopReq && fluid_player_get_status(gPlayer) == FLUID_PLAYER_PLAYING) {
        // interleaved stereo into one buffer
        if (fluid_synth_write_s16(synth, FRAMES,
                                  buf.data(), 0, 2,
                                  buf.data(), 1, 2) != FLUID_OK)
            break;
        if (out.write(buf.data(), FRAMES)) break;
        rendered += FRAMES;

        poll_stdin();
        if (++tick >= 20) {
            tick = 0;
            printf("POS %.2f -1\n", (double)rendered / RATE);
            fflush(stdout);
        }
    }

    fluid_player_stop(gPlayer);
    out.drain();
    out.close();
    delete_fluid_player(gPlayer);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);
    return 0;
}
