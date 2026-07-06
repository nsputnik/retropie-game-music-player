# Building `stjuke` (Atari ST / SNDH engine)

`stjuke` is a thin wrapper (like `sidjuke`) around **sc68 / libsc68** — Benjamin
Gerard's Atari ST & Amiga music player (68000 + YM-2149 / Paula emulation).

> **`stjuke` is now the fallback, not the primary SNDH engine.** `psgjuke`
> (psgplay — see [BUILD-psgjuke.md](BUILD-psgjuke.md)) is preferred for `.sndh`
> because it emulates the **STE DMA sound**, which sc68 does not — so it plays
> the STe / MaxYMiser tunes sc68 renders thin. `jukebox.py` lists `psgjuke`
> ahead of `stjuke`, and `psgjuke` `exec()`s `stjuke` for anything it can't load.
> That leaves sc68 one genuinely useful niche psgplay doesn't cover:
>
> - **ICE-packed SNDH** — `psgplay_init()` needs *uncompressed* SNDH, and refuses
>   packed files; libsc68 depacks ICE (via unice68) transparently.
> - **`.sc68` files and Amiga (Paula) tunes** — psgplay is Atari ST/STE only.
>
> So keep `stjuke` installed as the fallback. If you only have plain (unpacked)
> `.sndh` — as the vgmrips/SNDH-archive rips generally are — psgplay handles
> everything and sc68 simply never gets invoked.

- Player: **sc68**, © Benjamin Gerard — <http://sc68.atari.org>, GPL
  Source: <https://sourceforge.net/p/sc68> (canonical) · mirror
  <https://github.com/Zeinok/sc68>
- Music: the **[SNDH Archive](https://sndh.atari.org/)** (~9,900 tunes).

> **Just want the libraries?** They're packaged, prebuilt, in a standalone repo:
> **[sc68-buildkit](https://github.com/nsputnik/sc68-buildkit)** — grab
> `prebuilt/linux-armv7-glibc2.28/` and skip straight to step 4. The kit also has
> a one-command cross-build for other targets. The rest of this doc is a summary;
> sc68-buildkit is the source of truth for the build.
>
> **Why a separate repo?** Getting `libsc68` to compile again is generic work —
> useful to *anyone* playing SNDH / `.sc68` on modern Linux or ARM, not just this
> jukebox. Keeping it in its own reusable project (prebuilt libs + recipe) means
> nobody has to repeat the fight, and this repo just consumes it as a dependency
> — the same way it depends on libvgm, libgme, etc.

Unlike libvgm/libgme, `install.sh` does **not** build libsc68 automatically —
its source tree needs a few fixes and the build is heavy, so it's done once (via
sc68-buildkit) and the resulting static libs are kept on the Pi.

## 1. Get the source

```sh
curl -L https://github.com/Zeinok/sc68/archive/refs/heads/master.tar.gz | tar xz
cd sc68-master
```

## 2. Fixes the SVN/mirror tree needs

- **`tools/vcversion.sh` is missing** (it's SVN-generated). `configure.ac` runs
  it via `esyscmd`, so stub it — and emit **no trailing newline** (a newline
  corrupts the version string → "missing terminating `"`" in configure):
  ```sh
  printf '#!/bin/sh\nprintf 690\n' > tools/vcversion.sh && chmod +x tools/vcversion.sh
  ```
- The **meta-package** bootstrap fails (`SOURCE_UNICE68 ... AM_CONDITIONAL`), so
  build the sub-packages **standalone** in order: `as68 → unice68 → file68 →
  libsc68` (each `autoreconf -fi -I ../aclocal68 && ./configure --enable-static
  --disable-shared --prefix=$PWD/../inst && make && make install`).
  `as68` (sc68's 68k assembler) must be built first and be on `$PATH` — libsc68
  assembles its TOS trap emulator (`trapfunc.s → trap68.h`) with it.
- Build deps: `build-essential libtool automake autoconf pkg-config zlib1g-dev
  libao-dev bsdmainutils` (`bsdmainutils` provides `hexdump`).

## 3. Cross-compiling for the Pi (recommended)

The Pi Zero 2 W is too small for this build. Build it on a bigger machine in a
container matching the Pi's target (**armv7 / glibc 2.28 = Debian Buster**):

```sh
docker run --rm --platform linux/arm/v7 -v "$PWD":/src debian:buster-slim bash -c '
  # Buster is EOL: point apt at archive.debian.org first
  sed -i "s|deb.debian.org|archive.debian.org|g" /etc/apt/sources.list
  echo "Acquire::Check-Valid-Until \"false\";" > /etc/apt/apt.conf.d/99no-check
  apt-get update && apt-get install -y build-essential libtool automake autoconf \
    pkg-config zlib1g-dev libao-dev bsdmainutils
  cd /src/sc68-master && ...build the 4 sub-packages as above...'
```

Copy the resulting static libs + headers to the Pi (kept at
`~/src/sc68-armv7/{lib,include}`):
`libsc68.a libdial68.a libio68.a libemu68.a libfile68.a libunice68.a`,
plus headers `sc68/` and `unice68.h`.

## 4. Compile the engine on the Pi

```sh
L=~/src/sc68-armv7
g++ -O2 src/stjuke.cpp -I"$L/include" -o "$INSTALL_DIR/stjuke" \
  -Wl,--start-group "$L"/lib/lib{sc68,dial68,io68,emu68,file68,unice68}.a -Wl,--end-group \
  -lao -lz -lm -lpthread -ldl -lasound
```

Runtime dep: **`libao4`** (`sudo apt-get install libao-dev`) — file68's audio
backend links it. (A future cleanup could rebuild file68 `--without-ao` to drop
this.)

## 5. Music layout

SNDH is multi-subtune; the jukebox routes `.sndh`/`.sc68` to `stjuke` as a
subtune engine and reads the subtune count from the `##` header tag. Per-subtune
names come from the optional `!#SN` tag (parsed in `stjuke`, since sc68 doesn't
expose it); most files have none and show *Track N*.
