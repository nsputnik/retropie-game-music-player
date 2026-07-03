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
- **stdout (optional, once):** `LOOP 0` = this track has no loop point → jukebox shows
  **"No loop point"** and the loop (A) button does nothing. Emit `LOOP 1` or nothing when
  loopable (the default). Only `vgmjuke` emits it (from the VGM loop offset); formats that
  play forever or whole-file-repeat (NSF/SAP/SID/MOD/MIDI/SNDH) stay loopable.
- **stdin (live):** `inf\n` or `loops N\n` — change loop mode without restarting
- **`--info <file>`:** print `TRACKS <n>` then `TRK <i> <len_ms> <name>` per subtune (multi-subtune formats only)

`jukebox.py` routes by file extension via `EXT_ENGINE` (built from `_ENGINE_SPECS`).
Two engine "kinds":
- **sibling** — one song per file; the album folder's files are the queue, filenames are track names (VGM, MOD, MIDI, SPC).
- **subtune** — one file, many subtunes; queried with `--info`/`--track` (NSF, SID, SAP, AY, SNDH). How its *album* is built depends on the folder — see below.

**Fail-soft:** an engine is only routable if its binary exists on disk, so a
missing/failed engine silently drops its formats instead of crashing.

### Subtune album shapes (how a subtune file becomes an album)
`build_playlist()` decides **per selected file** using a fast, header-based
subtune count — `_subtune_count()` reads the file header (**no subprocess**):
PSID songs @0x0E, NSF @0x06, SAP `SONGS` tag, SNDH `##` tag; unknown → 1.
- **Lone subtune file in its folder** (wrapped `Game (NES)/Game.nsf`, or a game SAP) →
  album = that file's own subtunes.
- **Multi-song file among siblings** (a flat game: ZX `.ay`, a C64/ST game soundtrack) →
  its own subtune album; `describe()` names it after the *file* (folder = category), like a container.
- **Single-song file among siblings** (composer/demo collections — Pokey SAP, C64 SID,
  ZX/CPC demos) → the *folder* is the album, one track per single-song file. Multi-song
  siblings are **excluded** here (they're their own albums), so a composer list never
  contains an entry that would only play one of a multi-tune file's subtunes.
This is why C64/Pokey/ST browse by composer while ZX/Amstrad games each open as their own
multi-tune album — same code, decided by the header count.

## Engines
| Engine | Library | Formats | Build |
|---|---|---|---|
| `vgmjuke` | libvgm (source) | VGM/VGZ/GYM/S98/DRO | source, core |
| `gmejuke` | libgme (source) | NSF/NSFE/GBS/SPC/AY/HES/KSS/SAP | source, core |
| `modjuke` | libopenmpt (apt) | MOD/XM/S3M/IT/… | apt, fail-soft |
| `sidjuke` | libsidplayfp (apt) | SID/PSID | apt, fail-soft |
| `gmjuke` | FluidSynth (apt) | MID/MIDI (General MIDI) | apt, fail-soft |
| `stjuke` | sc68/libsc68 (prebuilt) | SNDH/.sc68 (Atari ST/Amiga) | prebuilt libs, fail-soft |

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
`tools/rename-rsn.py` renames abbreviated packs (smw.rsn) to the title in each
archive's info.txt line 1 ("Super Mario World (SNES).rsn"); the .rsn album name
is what the jukebox shows (main() strips the trailing "(SNES)" for display).

### Atari ST / Amiga: SNDH via sc68 (`stjuke`)
`stjuke` wraps **sc68 / libsc68** (Benjamin Gerard, GPLv3 — 68000 + YM2149/Paula
emulation) for `.sndh`/`.sc68` (**subtune** kind). Unlike the other engines,
libsc68 is **not apt-packaged** and its 2016 SVN source tree won't build as-is
(missing `vcversion.sh`, broken meta-package bootstrap, needs `as68` first). It's
built once and the **prebuilt static libs live in a separate repo,
[sc68-buildkit](https://github.com/nsputnik/sc68-buildkit)** (armv7 set staged on
the Pi at `~/src/sc68-armv7/{lib,include}`). Full recipe: `docs/BUILD-stjuke.md`.
- **Link:** `lib{sc68,dial68,io68,emu68,file68,unice68}.a` (start/end-group) +
  `-lao -lz -lm -lpthread -ldl -lasound`. Runtime dep: **libao** (`libao4`).
- **Length:** the SNDH's own duration (`sc68_music_info().trk.time_ms`), else
  `DEFAULT_LEN`; loops the subtune with `SC68_INF_LOOP` and bounds length itself
  (like `sidjuke`). Reports subtune count/durations via `--info`.
- **Per-subtune names:** sc68 does **not** decode the SNDH `!#SN` name tag, so
  `stjuke` parses it from the raw file — validating the offset table structurally
  (offsets start at the table end and never decrease) so **malformed tables fall
  back to "Track N"** instead of garbage. Only ~45 of the ~5,900-tune SNDH
  archive actually carry names; most show Track N.
- Music: the [SNDH Archive](https://sndh.atari.org/download.php), organised by
  composer — deployed as `gme/Atari ST/<Composer>/*.sndh`.

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

## New categories seeded
**2026-06**
- `C64/` — curated HVSC top-composer albums (SID). Full HVSC is 61k tunes — curate!
- `Amiga/` — tracker `.mod` albums.

**2026-07** (chip formats — all play on existing engines except Atari ST)
- `Pokey/` — Atari 800 POKEY `.sap` (`gmejuke`), from ASMA: `Games/` + top `Composers/`.
- `ZX Spectrum/`, `Amstrad CPC/` — AY-3-8910/YM2149 `.ay` (`gmejuke`), from Project AY.
  Flattened to `Games/*.ay` (multi-subtune, one level) + `Demos/*.ay` (single-song, one list).
- `Apple IIgs ES5503/` — vgmrips ES5503 VGM game packs (`vgmjuke`). NOTE: native IIgs
  SoundSmith/NoiseTracker-GS songs are **not** playable (no lib decodes them; libxmp & libopenmpt fail).
- `Atari ST/` — the full **SNDH archive**, 5,897 tunes by composer (`stjuke`; see above).
- Formats supported but not yet seeded: **Game Boy `.gbs`**, **PC Engine `.hes`** (both
  `gmejuke`, already routed) — just need content. Modland (huge, by-composer) for `modjuke`.

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
- ~~`stjuke` — sc68 for Atari ST `.sndh`~~ **BUILT** (see the sc68/SNDH section above).

## Play modes (Select cycles) & album navigation
`State.play_index` over `PLAY_MODES = [SINGLE, ALBUM, ALL, SHUFFLE]` (default ALBUM).
On natural track end: SINGLE stops; otherwise `next_track` advances within the
album; at album end, ALL calls `advance_album()` (next sibling), SHUFFLE calls it
with a random pick, ALBUM stops. `describe(rom)` builds (entries, idx, category,
album, art) for any launched file; `Jukebox.load_source(rom)` swaps the whole
queue+state and restarts; `sibling_albums(rom)` lists albums in scope — sibling
*folders* for folder-albums, sibling *files* for one-file albums (`album_is_file()`
= subtune/container). `State.source_rom` tracks the current album for sequencing.

## Art (fetch-art.py)
Drops `folder.png` per album from libretro-thumbnails (keyless). Handles flat
categories (`REPOS`) and nested ones (`NESTED`, e.g. `AdLib/Games` tried against
DOS → MAME → FBNeo). `--category X` restricts the run. Loose name matching is
length-guarded (>=5) to avoid junk like "Z" matching "Zoop". AdLib hit ~203/245.

## Gotchas
- Content-prep scripts run on macOS: `/bin/bash` is 3.2 (no `mapfile`/assoc arrays);
  the shell is **zsh** (unmatched globs error out) — prefer `find`, run scripts with `bash`.
- ES rewrites gamelists on exit; kill ES before editing gamelists, then restart.
