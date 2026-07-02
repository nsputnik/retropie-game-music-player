# CLAUDE.md — dev notes for retropie-game-music-player

Working notes for anyone (human or Claude) extending this player. Keep this
current as the architecture evolves.

## What it is
A framebuffer + gamepad **music jukebox** for RetroPie/EmulationStation. It adds
a `gme` ("Game Music") system and plays game-music rips through per-format engine
binaries. `src/jukebox.py` owns the screen, pad, playlist, box art and loop
control; each engine is a tiny CLI that decodes one library and streams PCM.

## The engine contract (match this for any new engine)
Every engine binary honors the same stdin/stdout protocol so `jukebox.py` stays
format-agnostic:

- **Invoke:** `engine [--infinite | --loops N] --fade <sec> [--track N] <file>`
- **stdout:** `POS <cur> <total>` lines (`total < 0` = unknown/looping forever) — drives the clock
- **stdin (live):** `inf\n` or `loops N\n` — change loop mode without restarting
- **`--info <file>`:** print `TRACKS <n>` then `TRK <i> <len_ms> <name>` per subtune (multi-subtune formats only)

`jukebox.py` routes by file extension via `EXT_ENGINE` (built from `_ENGINE_SPECS`).
Two engine "kinds":
- **sibling** — one song per file; the album folder's files are the queue, filenames are track names (VGM, MOD, MIDI).
- **subtune** — one file, many subtunes; queried with `--info`/`--track`, shown as numbered tracks (NSF, SID).

**Fail-soft:** an engine is only routable if its binary exists on disk, so a
missing/failed engine silently drops its formats instead of crashing.

## Engines
| Engine | Library | Formats | Build |
|---|---|---|---|
| `vgmjuke` | libvgm (source) | VGM/VGZ/GYM/S98/DRO | source, core |
| `gmejuke` | libgme (source) | NSF/NSFE/GBS/SPC/AY/HES/KSS/SAP | source, core |
| `modjuke` | libopenmpt (apt) | MOD/XM/S3M/IT/… | apt, fail-soft |
| `sidjuke` | libsidplayfp (apt) | SID/PSID | apt, fail-soft |
| `gmjuke` | FluidSynth (apt) | MID/MIDI (General MIDI) | apt, fail-soft |

- `vgmjuke`/`gmejuke` output audio through libvgm's `AudioStream` driver.
- `modjuke`/`sidjuke`/`gmjuke` render PCM and write straight to ALSA via
  `src/alsa_out.h` (self-contained, `-lasound` only) — no libvgm coupling.
- SID has no length/loop points → `--infinite` plays forever; finite modes play a
  fixed length (`DEFAULT_LEN`) with a fade.
- **gmjuke needs a GM SoundFont** (SC-55 style). Resolves in order:
  `--soundfont` → `$GMJUKE_SF2` → `<binary-dir>/gm.sf2` → first `.sf2` in
  `<binary-dir>/soundfonts/`. If a `.mid` has an identically-named `.sf2` beside
  it, it's loaded into bank 1 (AWE32 MIDI+SF2 pairs).

### SNES: SPC (sibling) + RSN containers
SNES music is `.spc` (decoded by `gmejuke`) but one song per file — so SPC is a
**sibling** kind (album = folder of `.spc`), not subtune. SNES sets are almost
always distributed as **`.rsn`** (a RAR of `.spc` + `info.txt`). `.rsn` is a
**container**: `jukebox.py` unpacks it (via `unar`; falls back to bsdtar/7z/unrar)
to `/tmp/rsnplay` on launch and plays the SPCs as a sibling album. A `.rsn` sits
directly under its category and *is* the album (main() special-cases album/category
for containers). `install.sh` installs `unar` (fail-soft). TODO: prettier names
come from the `.rsn` filename — rename files to full titles or parse `info.txt`;
box art for containers isn't wired (find_box_art still assumes Category/Album/track).
SPC track names come from each file's **ID666 song title** (text tag @ offset 0x2E)
via `track_name()`, falling back to the filename — so SNESmusic.org sets show real
song names (e.g. "Yoshi's Island") without renaming/repacking the archives.

## Deployment reality (IMPORTANT — differs from install.sh defaults)
`install.sh` defaults `INSTALL_DIR=/opt/retropie/emulators/gamemusic`, **but the
live GPi Case Pi was installed under the older name**:

- **Install dir:** `/opt/retropie/emulators/vgmplay/` (holds `jukebox.py`,
  `vgmjuke`, `gmejuke`, launcher `vgmplay.sh`)
- **gme emulators.cfg:** `vgmplay = ".../vgmplay/vgmplay.sh %ROM%"`, `default = vgmplay`
- **ES command:** `runcommand.sh 0 _SYS_ gme %ROM%`
- Core lib build dirs on the Pi: `/home/pi/src/libvgm/build/bin`, `/home/pi/src/game-music-emu/build`

So when deploying the new engines to *that* Pi, target `.../vgmplay/`, not
`gamemusic`. Drop the SoundFont at `.../vgmplay/soundfonts/*.sf2`.

## Music library layout
`roms/gme/<Category>/<Album>/…`. Category → box art system is `SRC_SYSTEMS` in
`jukebox.py` (NES→nes, Genesis→megadrive, …); new categories (C64/Amiga/DOS) have
no console-art fallback, so they show art only if an album `folder.png` is present.
- **sibling** albums: multiple files, e.g. `NN Title.vgz` / `*.mod` / `*.mid`.
- **subtune** albums: one file per album folder (e.g. `Game (NES)/Game (NES).nsf`,
  or one `.sid` per folder).

## New categories seeded (2026-06)
- `C64/` — curated HVSC top-composer albums (SID). Full HVSC is 61k tunes — curate!
- `Amiga/` — tracker `.mod` albums.

### PC/DOS is organized by SYNTH, not platform (categories map to engines)
"DOS" was too broad — it spans three different synthesis models needing three
engines. So the top-level category IS the synth:

| Category | Files | Engine |
|---|---|---|
| `General MIDI/` | `.mid` | `gmjuke` (FluidSynth + SC-55 SF2) — **live** |
| `MT-32/` | `.mid` | `mt32juke` (Munt + ROMs) — **not built yet** |
| `AdLib/` | `.dro` / `.vgm` | `vgmjuke` — works today (OPL2 is FM register data, **not MIDI**) |

- MT-32 vs GM are both `.mid`; distinguish by SysEx if needed (MT-32: `F0 41 10 16 12…`;
  GM/GS reset: `F0 7E 7F 09 01` / `F0 41 10 42 12…`) — but **route by folder**, not sniffing.
- **TODO — category-aware `.mid` routing:** `jukebox.py` currently routes `.mid` →
  `gmjuke` by extension only. When `mt32juke` lands, route by category: a MIDI whose
  category folder is `MT-32` → `mt32juke`, else → `gmjuke`. Until then MT-32 content is
  kept OUT of the Pi (it would play wrong through the GM SoundFont).

## Planned next engines (design agreed, not built)
- `mt32juke` — Munt / libmt32emu (source build, user-supplied MT-32 ROMs) for the
  MT-32 DOS corpus. Category-routed: `gme/MT-32/…` → mt32juke, `gme/DOS|GM/…` → gmjuke.
- `stjuke` — sc68 (source build, gated/fail-soft) for Atari ST `.sndh`/`.ym`.

## Gotchas
- Content-prep scripts run on macOS: `/bin/bash` is 3.2 (no `mapfile`/assoc arrays);
  the shell is **zsh** (unmatched globs error out) — prefer `find`, run scripts with `bash`.
- ES rewrites gamelists on exit; kill ES before editing gamelists, then restart.
