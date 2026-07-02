#!/usr/bin/env bash
#
# Installer for RetroPie Game Music Player.
# Builds the libvgm + libgme engines, installs the player, and registers the
# "Game Music" (gme) system in EmulationStation.
#
# Tested on RetroPie (Raspbian) on a Pi Zero 2 W (GPi Case 2W) and standard
# RPi 3/4/5 builds. Re-runnable.
#
set -euo pipefail

# --- paths (override via environment if your setup differs) -------------------
RP="${RP:-/opt/retropie}"
INSTALL_DIR="${INSTALL_DIR:-$RP/emulators/gamemusic}"
CONFIG_DIR="${CONFIG_DIR:-$RP/configs/gme}"
ROMS="${ROMS:-$HOME/RetroPie/roms}"
GME_ROMS="$ROMS/gme"
SRC="${SRC:-$HOME/src}"
ES_SYSTEMS="${ES_SYSTEMS:-/etc/emulationstation/es_systems.cfg}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="$(nproc 2>/dev/null || echo 2)"

echo "==> RetroPie Game Music Player installer"
echo "    install dir : $INSTALL_DIR"
echo "    roms        : $GME_ROMS"
echo "    build dir   : $SRC"

# --- 1. dependencies ----------------------------------------------------------
echo "==> Installing build + runtime dependencies (sudo apt)"
sudo apt-get install -y build-essential cmake git libasound2-dev zlib1g-dev \
    python3 python3-pil fonts-freefont-ttf || {
    echo "!! apt install failed. On EOL Raspbian Buster the mirror may have moved;"
    echo "   point /etc/apt/sources.list at http://legacy.raspbian.org/raspbian/ and retry."
    exit 1
}

mkdir -p "$SRC"

# --- 2. build libvgm (VGM/VGZ/GYM/S98/DRO) ------------------------------------
echo "==> Building libvgm"
if [ ! -d "$SRC/libvgm" ]; then
    git clone --depth 1 https://github.com/ValleyBell/libvgm "$SRC/libvgm"
fi
mkdir -p "$SRC/libvgm/build"
( cd "$SRC/libvgm/build"
  cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBAUDIO=ON -DBUILD_LIBEMU=ON \
        -DBUILD_PLAYER=ON ..
  make -j"$JOBS" )

# --- 3. build libgme (NSF/GBS/SPC/AY/HES/KSS/SAP) -----------------------------
echo "==> Building libgme (game-music-emu)"
if [ ! -d "$SRC/game-music-emu" ]; then
    git clone --depth 1 https://github.com/libgme/game-music-emu "$SRC/game-music-emu"
fi
mkdir -p "$SRC/game-music-emu/build"
( cd "$SRC/game-music-emu/build"
  cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF ..
  make -j"$JOBS" )

# --- 4. compile the engines ---------------------------------------------------
echo "==> Compiling engines (vgmjuke, gmejuke)"
mkdir -p "$INSTALL_DIR"
g++ -O2 -I"$SRC/libvgm" "$HERE/src/vgmjuke.cpp" -o "$INSTALL_DIR/vgmjuke" \
    -L"$SRC/libvgm/build/bin" \
    -Wl,--start-group -lvgm-player -lvgm-emu -lvgm-utils -lvgm-audio -Wl,--end-group \
    -lasound -lz -lpthread -ldl
g++ -O2 -I"$SRC/libvgm" -I"$SRC/game-music-emu" "$HERE/src/gmejuke.cpp" \
    -o "$INSTALL_DIR/gmejuke" \
    -L"$SRC/game-music-emu/build/gme" -L"$SRC/libvgm/build/bin" \
    -Wl,--start-group -lgme -lvgm-audio -lvgm-utils -Wl,--end-group \
    -lasound -lpthread -lz

# --- 4b. optional extra engines (MOD / SID / GM-MIDI) -------------------------
# These wrap apt-packaged libraries (no source build) and render PCM straight to
# ALSA. Each is FAIL-SOFT: if its dev package is missing or it won't compile, we
# skip just that engine (the jukebox then drops those file types) and keep going.
echo "==> Installing extra engine deps (sudo apt; non-fatal)"
sudo apt-get install -y libopenmpt-dev libsidplayfp-dev libfluidsynth-dev \
    pkg-config || echo "!! some extra engine dev packages unavailable; those engines will be skipped"

# .rsn (SNES SPC sets) are RAR archives; the jukebox unpacks them to .spc at
# play time via 'unar'. Optional: without it, .rsn won't open (raw .spc still works).
echo "==> Installing unar for .rsn (SNES) support (sudo apt; non-fatal)"
sudo apt-get install -y unar || echo "!! unar unavailable - .rsn files won't unpack; use extracted .spc instead"

build_extra_engine() {  # name  source.cpp  pkgconfig-name
    local name="$1" src="$2" pkg="$3" flags
    flags="$(pkg-config --cflags --libs "$pkg" 2>/dev/null)" || flags=""
    [ -z "$flags" ] && flags="-l${pkg#lib}"   # fallback if no .pc file
    echo "==> Compiling $name ($pkg)"
    if g++ -O2 "$HERE/src/$src" -o "$INSTALL_DIR/$name" $flags -lasound; then
        chmod +x "$INSTALL_DIR/$name"
        echo "    $name OK"
    else
        rm -f "$INSTALL_DIR/$name"
        echo "!! $name failed to build - its formats will be unavailable (non-fatal)"
    fi
}

build_extra_engine modjuke modjuke.cpp libopenmpt      # MOD/XM/S3M/IT/...
build_extra_engine sidjuke sidjuke.cpp libsidplayfp    # SID/PSID
build_extra_engine gmjuke  gmjuke.cpp  fluidsynth      # MID/MIDI (General MIDI)

# gmjuke needs a General MIDI SoundFont (SC-55 style). Drop a .sf2 in here (or
# set $GMJUKE_SF2); MIDI playback is skipped until one is present.
mkdir -p "$INSTALL_DIR/soundfonts"
if ! ls "$INSTALL_DIR/soundfonts/"*.sf2 >/dev/null 2>&1 && [ ! -f "$INSTALL_DIR/gm.sf2" ]; then
    echo "!! no GM SoundFont in $INSTALL_DIR/soundfonts/ - MIDI stays silent until you add one"
fi

# --- 5. install the player ----------------------------------------------------
echo "==> Installing player files into $INSTALL_DIR"
cp "$HERE/src/jukebox.py" "$INSTALL_DIR/jukebox.py"
cp "$HERE/src/launch.sh" "$INSTALL_DIR/launch.sh"
chmod +x "$INSTALL_DIR/launch.sh" "$INSTALL_DIR/vgmjuke" "$INSTALL_DIR/gmejuke"

# --- 6. EmulationStation 'gme' system -----------------------------------------
echo "==> Registering the Game Music (gme) system"
mkdir -p "$GME_ROMS" "$CONFIG_DIR"

cat > "$CONFIG_DIR/emulators.cfg" <<EOF
gamemusic = "$INSTALL_DIR/launch.sh %ROM%"
default = "gamemusic"
EOF

if ! grep -q "<name>gme</name>" "$ES_SYSTEMS" 2>/dev/null; then
    echo "    adding <system> entry to $ES_SYSTEMS (sudo)"
    SNIPPET="$(sed "s#@ROMS@#$GME_ROMS#" "$HERE/setup/es_systems-gme.xml")"
    # insert before the closing </systemList>
    sudo python3 - "$ES_SYSTEMS" "$SNIPPET" <<'PY'
import sys
path, snippet = sys.argv[1], sys.argv[2]
with open(path) as f:
    data = f.read()
if "</systemList>" in data:
    data = data.replace("</systemList>", snippet + "\n</systemList>", 1)
else:
    data = data.rstrip() + "\n" + snippet + "\n"
with open(path, "w") as f:
    f.write(data)
print("  inserted gme system")
PY
else
    echo "    gme system already present, leaving es_systems.cfg alone"
fi

echo
echo "==> Done. Put music under: $GME_ROMS/<Category>/<Album>/*.vgz|*.nsf|..."
echo "    e.g. $GME_ROMS/NES/Mega Man 2 (NES)/01 Title.vgz"
echo "    Then restart EmulationStation. The first launch runs the on-screen"
echo "    controller setup. Optional art: see tools/ (fetch-art, make-gamelist)."
