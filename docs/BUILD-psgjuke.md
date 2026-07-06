# Building `psgjuke` (Atari ST / STE SNDH engine, STE DMA sound)

`psgjuke` is a thin wrapper (like `sidjuke`/`stjuke`) around **psgplay / libpsgplay**
— Fredrik Noring's Atari ST & STE music player. It emulates the **68000 + YM2149
PSG + STE DMA sound + LMC1992 mixer**, so it plays the large class of **STe /
MaxYMiser** tunes that sc68 (`stjuke`) renders thin or silent — the DMA sample
layer (sampled drums/bass) that sc68 doesn't emulate.

- Player: **psgplay**, © Fredrik Noring — <https://github.com/frno7/psgplay>, GPL-2.0
- Music: the **[SNDH Archive](https://sndh.atari.org/)** (~9,900 tunes).

## Unlike `stjuke`, this is built automatically

`install.sh` builds `libpsgplay` and compiles `psgjuke` for you — the same way it
builds libvgm and libgme. **No prebuilt binary, no buildkit.** That is the key
difference from `stjuke`/libsc68 (see [BUILD-stjuke.md](BUILD-stjuke.md)), which
needs source fixes and is too heavy to compile on a Pi Zero 2 W, so it ships as a
separate prebuilt buildkit. psgplay compiles cleanly **on the Pi itself in ~1
minute (~90 MB RAM)**, so there is nothing extra for a casual installer to do.

The `install.sh` step is FAIL-SOFT: if libpsgplay doesn't build, the core player
still installs and `.sndh` falls back to `stjuke` (if present).

## Manual build (what `install.sh` does)

```sh
git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/frno7/psgplay ~/src/psgplay
cd ~/src/psgplay
make -j"$(nproc)" lib/psgplay/libpsgplay.a          # STATIC lib only - see note
g++ -O2 -I include src/psgjuke.cpp \
    -o "$INSTALL_DIR/psgjuke" lib/psgplay/libpsgplay.a -lasound -lm
```

**Build the static target (`lib/psgplay/libpsgplay.a`), not `make install-lib`.**
`install-lib` also builds and installs a *shared* library, whose install step
trips over a self-referential symlink on a **shallow clone** (the version string
becomes a git hash, so the `.so` filename and soname collide → `ln: File exists`).
It's harmless — the jukebox links the static `.a` — but building the static
target directly avoids the error.

`libpsgplay.a` is self-contained (it bundles the Musashi 68000 core, the cf2149
PSG, cf68901 MFP and cf300588 STE DMA-sound emulators), so the only extra link
libs are `-lasound` (via `alsa_out.h`) and `-lm`.

### Cross-compiling for the Pi (optional)

Building on the Pi is the norm, but you can cross-build in an armv7 / glibc-2.28
(Debian Buster) container the same way as sc68:

```sh
docker run --rm --platform linux/arm/v7 -v "$PWD":/src debian:buster-slim bash -c '
  apt-get update && apt-get install -y build-essential git
  cd /src && make -j4 install-lib prefix=/src/inst'   # then copy inst/lib + inst/include
```

## Music layout & routing

SNDH is multi-subtune; the jukebox routes `.sndh` to `psgjuke` as a subtune
engine. psgplay parses SNDH natively, so `psgjuke --info` reports the subtune
count, per-subtune duration (from the `TIME` tag) and per-subtune names (from the
`!#SN` tag) directly — no hand-rolled parsing needed (contrast `stjuke`).

`psgjuke` is the **preferred** SNDH engine; `jukebox.py` lists it ahead of
`stjuke`. On any file it can't load — not SNDH, ICE-packed, or a `.sc68` file —
`psgjuke` `exec()`s `stjuke` next to it as a fallback (same CLI + `POS` protocol),
so sc68 still covers ICE-packed SNDH and `.sc68`/Amiga. See
[BUILD-stjuke.md](BUILD-stjuke.md).
