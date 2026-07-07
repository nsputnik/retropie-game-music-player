#!/usr/bin/env python3
"""Refresh the EmulationStation gamelist for the 'gme' system so box art shows
while browsing the Game Music menu.

Sets <image> for every track to its album art, resolved exactly the way the
jukebox does (per-album folder.png, else the matching console images/ folder).
Existing <playcount>/<lastplayed>/<favorite> values are preserved.

EmulationStation rewrites this file when it exits, so run this with ES stopped
(the installer / a wrapper does: stop ES -> run this -> restart ES).
"""
import os
import sys
import xml.etree.ElementTree as ET

GME_ROOT = os.path.expanduser("~/RetroPie/roms/gme")
# ~/.emulationstation is usually a symlink to /opt/retropie/configs/all/
# emulationstation on RetroPie; override with $GME_GAMELIST if yours differs.
GAMELIST = os.environ.get(
    "GME_GAMELIST",
    os.path.expanduser("~/.emulationstation/gamelists/gme/gamelist.xml"))
JUKEBOX_DIR = os.environ.get("GME_PLAYER_DIR", "/opt/retropie/emulators/gamemusic")

sys.path.insert(0, JUKEBOX_DIR)
import jukebox  # noqa: E402  (for find_box_art / AUDIO_EXTS)


def load_existing():
    meta = {}
    if os.path.isfile(GAMELIST):
        try:
            for g in ET.parse(GAMELIST).getroot().findall("game"):
                p = g.findtext("path")
                if p:
                    meta[p] = g
        except Exception:
            pass
    return meta


def main():
    existing = load_existing()
    root = ET.Element("gameList")
    total = withart = 0
    for dp, _, files in os.walk(GME_ROOT):
        for f in sorted(files):
            if not f.lower().endswith(jukebox.AUDIO_EXTS):
                continue
            full = os.path.join(dp, f)
            rel = "./" + os.path.relpath(full, GME_ROOT)
            g = ET.SubElement(root, "game")
            ET.SubElement(g, "path").text = rel
            ET.SubElement(g, "name").text = os.path.splitext(f)[0]
            art = jukebox.find_box_art(full)
            if art:
                ET.SubElement(g, "image").text = art
                withart += 1
            old = existing.get(rel)
            if old is not None:
                for tag in ("desc", "playcount", "lastplayed", "favorite"):
                    v = old.findtext(tag)
                    if v:
                        ET.SubElement(g, tag).text = v
            total += 1
    # ES derives a folder's display name with getStem() - it strips everything
    # after the last dot - so album folders whose name contains a dot show up
    # truncated in the browser: "Super Mario Bros. 2 (NES)" -> "Super Mario Bros",
    # "R.C. Pro-Am (NES)" -> "R.C". Emit an explicit <folder><name> for those so
    # ES shows the real name. Dot-free folder names already display fine, so we
    # skip them to keep the gamelist lean.
    folders = 0
    for dp, dirs, _files in os.walk(GME_ROOT):
        for d in sorted(dirs):
            if os.path.splitext(d)[0] == d:
                continue  # no dot -> ES displays it correctly already
            rel = "./" + os.path.relpath(os.path.join(dp, d), GME_ROOT)
            fo = ET.SubElement(root, "folder")
            ET.SubElement(fo, "path").text = rel
            ET.SubElement(fo, "name").text = d
            folders += 1
    os.makedirs(os.path.dirname(GAMELIST), exist_ok=True)
    ET.ElementTree(root).write(GAMELIST, encoding="utf-8", xml_declaration=True)
    print("wrote %d entries (%d with art, %d named folders) -> %s"
          % (total, withart, folders, GAMELIST))


if __name__ == "__main__":
    main()
