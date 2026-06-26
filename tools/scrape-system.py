#!/usr/bin/env python3
"""Fetch box art and build an EmulationStation gamelist for a console ROM
system, using libretro-thumbnails (no account/key). General-purpose; used here
to give the Sega Master System game browser cover art.

Games organised in per-title subfolders (./Game Name/rom (region).ext) get a
<folder> entry with art plus a <game> entry per ROM. Loose ROMs in the system
root get a <game> entry. Existing playcount/lastplayed are preserved.

Run with EmulationStation stopped, then restart ES. Example:
  scrape-system.py --roms /home/pi/RetroPie/roms/mastersystem \\
      --repo Sega_-_Master_System_-_Mark_III --exts .sms,.sg,.sc
"""
import os
import re
import sys
import json
import argparse
import urllib.request
import urllib.parse
import xml.etree.ElementTree as ET

UA = {"User-Agent": "gme-scrape"}


def normalize(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())


def clean_name(s):
    """Strip trailing (region)/[dump] tags: 'Wonder Boy (UE) [!]' -> 'Wonder Boy'."""
    return re.sub(r'\s*[\(\[][^\)\]]*[\)\]]', '', s).strip()


def candidate_targets(base):
    out = {normalize(base)}
    low = base.lower()
    if low.startswith("the "):
        out.add(normalize(base[4:] + ", The"))
        out.add(normalize(base[4:]))
    elif low.endswith(", the"):
        out.add(normalize("The " + base[:-5]))
    return {t for t in out if t}


def get_boxart_list(repo):
    url = ("https://api.github.com/repos/libretro-thumbnails/%s"
           "/git/trees/master?recursive=1" % repo)
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=30) as r:
        data = json.load(r)
    out = []
    for node in data.get("tree", []):
        p = node.get("path", "")
        if p.startswith("Named_Boxarts/") and p.lower().endswith(".png"):
            out.append(p[len("Named_Boxarts/"):])
    return out


def pick(name, files):
    targets = candidate_targets(clean_name(name))
    exact, loose = [], []
    for f in files:
        nb = normalize(clean_name(os.path.splitext(f)[0]))
        if nb in targets:
            exact.append(f)
        elif any(nb.startswith(t) or t.startswith(nb) for t in targets):
            loose.append(f)
    pool = exact or loose
    if not pool:
        return None

    def score(f):
        s = f.lower()
        r = 0
        if "(usa" in s or "(world" in s:
            r -= 4
        if "europe" in s:
            r -= 3
        if "(en" in s or " en)" in s or ",en" in s:
            r -= 1
        if "beta" in s or "proto" in s or "demo" in s or "hack" in s:
            r += 4
        if "(japan" in s and "en" not in s:
            r += 2
        return (r, len(f))

    return sorted(pool, key=score)[0]


def download(repo, fname, dest):
    raw = ("https://raw.githubusercontent.com/libretro-thumbnails/%s"
           "/master/Named_Boxarts/%s" % (repo, urllib.parse.quote(fname)))
    with urllib.request.urlopen(urllib.request.Request(raw, headers=UA), timeout=30) as r:
        blob = r.read()
    with open(dest, "wb") as out:
        out.write(blob)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roms", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--exts", default=".sms,.sg,.sc")
    ap.add_argument("--gamelist", default=None)
    args = ap.parse_args()
    exts = tuple(e if e.startswith(".") else "." + e
                 for e in args.exts.lower().split(","))
    roms = args.roms.rstrip("/")
    img_dir = os.path.join(roms, "images")
    os.makedirs(img_dir, exist_ok=True)
    gamelist = args.gamelist or os.path.join(
        os.path.expanduser("~/.emulationstation/gamelists"),
        os.path.basename(roms), "gamelist.xml")

    # preserve existing metadata
    meta = {}
    if os.path.isfile(gamelist):
        try:
            for g in ET.parse(gamelist).getroot().findall("game"):
                p = g.findtext("path")
                if p:
                    meta[p] = g
        except Exception:
            pass

    files = None  # lazy-loaded boxart index
    art_for = {}  # game-folder (or "") -> image path or None
    hit = miss = 0
    misses = []

    def resolve_art(name, key):
        nonlocal files, hit, miss
        if key in art_for:
            return art_for[key]
        dest = os.path.join(img_dir, (name if name else key) + ".png")
        if os.path.isfile(dest):
            art_for[key] = dest
            return dest
        if files is None:
            try:
                files = get_boxart_list(args.repo)
            except Exception as e:
                print("! index fetch failed:", e)
                files = []
        f = pick(name, files)
        if not f:
            art_for[key] = None
            miss += 1
            misses.append(name)
            return None
        try:
            download(args.repo, f, dest)
            hit += 1
            print("OK   %s  <-  %s" % (name, f))
            art_for[key] = dest
            return dest
        except Exception as e:
            art_for[key] = None
            misses.append("%s (dl: %s)" % (name, e))
            return None

    root = ET.Element("gameList")
    folders_done = set()
    for dp, dirs, fnames in os.walk(roms):
        if os.path.basename(dp) == "images":
            dirs[:] = []
            continue
        for f in sorted(fnames):
            if not f.lower().endswith(exts):
                continue
            full = os.path.join(dp, f)
            rel = "./" + os.path.relpath(full, roms)
            in_subfolder = os.path.dirname(os.path.relpath(full, roms)) != ""
            folder = os.path.relpath(full, roms).split(os.sep)[0] if in_subfolder else ""
            art = resolve_art(folder if folder else clean_name(os.path.splitext(f)[0]),
                              folder if folder else os.path.splitext(f)[0])

            # one <folder> entry per game subfolder (so the browse list shows art)
            if folder and folder not in folders_done:
                folders_done.add(folder)
                fe = ET.SubElement(root, "folder")
                ET.SubElement(fe, "path").text = "./" + folder
                ET.SubElement(fe, "name").text = folder
                if art:
                    ET.SubElement(fe, "image").text = art

            g = ET.SubElement(root, "game")
            ET.SubElement(g, "path").text = rel
            ET.SubElement(g, "name").text = clean_name(os.path.splitext(f)[0]) or os.path.splitext(f)[0]
            if art:
                ET.SubElement(g, "image").text = art
            old = meta.get(rel)
            if old is not None:
                for tag in ("playcount", "lastplayed", "favorite"):
                    v = old.findtext(tag)
                    if v:
                        ET.SubElement(g, tag).text = v

    os.makedirs(os.path.dirname(gamelist), exist_ok=True)
    ET.ElementTree(root).write(gamelist, encoding="utf-8", xml_declaration=True)
    print("\nart: %d fetched, %d missed. gamelist -> %s" % (hit, miss, gamelist))
    for m in misses:
        print("  MISS", m)


if __name__ == "__main__":
    main()
