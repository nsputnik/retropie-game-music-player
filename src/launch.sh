#!/bin/bash
# Game Music launcher -> graphical jukebox (queues the whole folder / all subtunes).
# EmulationStation calls this with the selected track as $1. The jukebox picks
# its engine by file type:
#   register-log formats (VGM/VGZ/GYM/S98/DRO) -> vgmjuke (libvgm)
#   CPU-emulated formats (NSF/GBS/SPC/AY/...)   -> gmejuke (libgme)
ROM="$1"
BIN="$(dirname "$(readlink -f "$0")")"

python3 "$BIN/jukebox.py" "$ROM"
rc=$?
stty sane 2>/dev/null
clear 2>/dev/null
exit $rc
