#!/usr/bin/env python3
"""Fetch box art for the Game Music (gme) library from libretro-thumbnails.

For each album with no art yet, match the game name against the system's
libretro-thumbnails Named_Boxarts index and download the cover to
<album>/folder.png (which the jukebox prefers). No account or API key needed --
this is the same source the rest of the RetroPie art was scraped from.

Usage: fetch-art.py [--all] [--root DIR]
  --all   also write folder.png for albums that already resolve art from a
          console images/ folder, making the gme library fully self-contained.
"""
import os
import re
import sys
import json
import argparse
import urllib.request
import urllib.parse

DEFAULT_ROOT = os.path.expanduser("~/RetroPie/roms/gme")
JUKEBOX_DIR = os.environ.get("GME_PLAYER_DIR", "/opt/retropie/emulators/gamemusic")
UA = {"User-Agent": "gme-fetch-art"}

# gme category folder -> libretro-thumbnails per-system repository
REPOS = {
    "NES": "Nintendo_-_Nintendo_Entertainment_System",
    "Genesis": "Sega_-_Mega_Drive_-_Genesis",
    "Master System": "Sega_-_Master_System_-_Mark_III",
    "Arcade": "MAME",
}


def normalize(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())


def strip_paren_suffix(s):
    return re.sub(r'\s*\([^)]*\)\s*$', '', s).strip()


def strip_all_parens(s):
    return re.sub(r'\s*\([^)]*\)', '', s).strip()


def candidate_targets(base):
    """Normalized name variants, handling 'The' article reordering."""
    out = {normalize(base)}
    low = base.lower()
    if low.startswith("the "):
        out.add(normalize(base[4:] + ", The"))
        out.add(normalize(base[4:]))
    elif low.endswith(", the"):
        out.add(normalize("The " + base[:-5]))
    return {t for t in out if t}


def get_boxart_list(repo):
    """All Named_Boxarts filenames for a libretro-thumbnails system (1 request)."""
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


def pick(album, files):
    """Choose the best Named_Boxart for an album, preferring USA/Europe English."""
    targets = candidate_targets(strip_paren_suffix(album))
    exact, loose = [], []
    for f in files:
        nb = normalize(strip_all_parens(os.path.splitext(f)[0]))
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
        if "(en" in s or ",en" in s or " en)" in s:
            r -= 1
        if "beta" in s or "proto" in s or "demo" in s:
            r += 4
        if "(japan" in s and "en" not in s:
            r += 2
        return (r, len(f))

    return sorted(pool, key=score)[0]


def has_art(album_dir):
    for c in ("folder.png", "box.png", "cover.png"):
        if os.path.isfile(os.path.join(album_dir, c)):
            return True
    try:
        import jukebox
        for f in sorted(os.listdir(album_dir)):
            return jukebox.find_box_art(os.path.join(album_dir, f)) is not None
    except Exception:
        pass
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--all", action="store_true",
                        help="fetch even where console images already match")
    parser.add_argument("--root", default=DEFAULT_ROOT)
    args = parser.parse_args()
    if os.path.isdir(JUKEBOX_DIR):
        sys.path.insert(0, JUKEBOX_DIR)

    cache = {}
    hit = skip = 0
    misses = []
    for cat in sorted(os.listdir(args.root)):
        cp = os.path.join(args.root, cat)
        if not os.path.isdir(cp):
            continue
        repo = REPOS.get(cat)
        for album in sorted(os.listdir(cp)):
            adir = os.path.join(cp, album)
            if not os.path.isdir(adir):
                continue
            dest = os.path.join(adir, "folder.png")
            if os.path.isfile(dest):
                skip += 1
                continue
            if not args.all and has_art(adir):
                skip += 1
                continue
            if not repo:
                misses.append("%s/%s (no repo mapping)" % (cat, album))
                continue
            if repo not in cache:
                try:
                    cache[repo] = get_boxart_list(repo)
                except Exception as e:
                    print("! index fetch failed for %s: %s" % (repo, e))
                    cache[repo] = []
            f = pick(album, cache[repo])
            if not f:
                misses.append("%s/%s" % (cat, album))
                continue
            raw = ("https://raw.githubusercontent.com/libretro-thumbnails/%s"
                   "/master/Named_Boxarts/%s" % (repo, urllib.parse.quote(f)))
            try:
                with urllib.request.urlopen(urllib.request.Request(raw, headers=UA), timeout=30) as r:
                    blob = r.read()
                with open(dest, "wb") as out:
                    out.write(blob)
                hit += 1
                print("OK   %s/%s  <-  %s" % (cat, album, f))
            except Exception as e:
                misses.append("%s/%s (download: %s)" % (cat, album, e))

    print("\nfetched %d, skipped %d (already had art), missed %d"
          % (hit, skip, len(misses)))
    for m in misses:
        print("  MISS", m)


if __name__ == "__main__":
    main()
