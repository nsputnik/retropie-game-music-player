#!/usr/bin/env python3
"""Fetch box art for the Game Music (gme) library from libretro-thumbnails.

For each album with no art yet, match the game name against the system's
libretro-thumbnails Named_Boxarts index and download the cover to
<album>/folder.png (which the jukebox prefers). No account or API key needed --
this is the same source the rest of the RetroPie art was scraped from.

Handles both flat categories (Category/<album>) and nested ones
(Category/<sub>/<album>, e.g. AdLib/Games/<game>), and can try several thumbnail
systems per album (AdLib mixes DOS and arcade titles).

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
JUKEBOX_DIR = os.environ.get("GME_PLAYER_DIR", "/opt/retropie/emulators/vgmplay")
UA = {"User-Agent": "gme-fetch-art"}

# Flat categories: gme category folder -> a single libretro-thumbnails repo.
REPOS = {
    "NES": "Nintendo_-_Nintendo_Entertainment_System",
    "Genesis": "Sega_-_Mega_Drive_-_Genesis",
    "Master System": "Sega_-_Master_System_-_Mark_III",
    "Arcade": "MAME",
}

# Nested categories: albums live one level deeper (Category/<sub>/<album>) and
# may match several thumbnail systems, tried in order.
#   category -> (subfolder, [repo, ...])
NESTED = {
    "AdLib": ("Games", ["DOS", "MAME", "FBNeo_-_Arcade_Games"]),
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
    """Best Named_Boxart for an album within one repo (exact preferred), or None."""
    targets = candidate_targets(strip_paren_suffix(album))
    exact, loose = [], []
    for f in files:
        nb = normalize(strip_all_parens(os.path.splitext(f)[0]))
        if nb in targets:
            exact.append(f)
        elif any(len(nb) >= 5 and len(t) >= 5 and (nb.startswith(t) or t.startswith(nb))
                 for t in targets):
            loose.append(f)   # guard tiny names (e.g. "Z" vs "Zoop")
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


def pick_across(album, repo_files):
    """Choose across several repos: prefer a repo with an EXACT match (in order),
    else the first repo with any (loose) match. Returns (repo, filename)|(None,None)."""
    targets = candidate_targets(strip_paren_suffix(album))

    def is_exact(f):
        return normalize(strip_all_parens(os.path.splitext(f)[0])) in targets

    for repo, files in repo_files:                 # pass 1: exact
        f = pick(album, files)
        if f and is_exact(f):
            return repo, f
    for repo, files in repo_files:                 # pass 2: loose
        f = pick(album, files)
        if f:
            return repo, f
    return None, None


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


def iter_albums(root, only=None):
    """Yield (label, album_dir, [repo, ...]) for every album to consider.
    If `only` is set, restrict to that category folder."""
    for cat in sorted(os.listdir(root)):
        if only and cat != only:
            continue
        cp = os.path.join(root, cat)
        if not os.path.isdir(cp):
            continue
        if cat in NESTED:
            sub, repos = NESTED[cat]
            base = os.path.join(cp, sub)
            if not os.path.isdir(base):
                continue
            for album in sorted(os.listdir(base)):
                adir = os.path.join(base, album)
                if os.path.isdir(adir):
                    yield ("%s/%s/%s" % (cat, sub, album), adir, repos)
        else:
            repo = REPOS.get(cat)
            repos = [repo] if repo else []
            for album in sorted(os.listdir(cp)):
                adir = os.path.join(cp, album)
                if os.path.isdir(adir):
                    yield ("%s/%s" % (cat, album), adir, repos)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--all", action="store_true",
                        help="fetch even where console images already match")
    parser.add_argument("--root", default=DEFAULT_ROOT)
    parser.add_argument("--category", default=None,
                        help="restrict to a single gme category (e.g. AdLib)")
    args = parser.parse_args()
    if os.path.isdir(JUKEBOX_DIR):
        sys.path.insert(0, JUKEBOX_DIR)

    cache = {}
    hit = skip = 0
    misses = []
    for label, adir, repos in iter_albums(args.root, args.category):
        album = os.path.basename(adir)
        dest = os.path.join(adir, "folder.png")
        if os.path.isfile(dest):
            skip += 1
            continue
        if not args.all and has_art(adir):
            skip += 1
            continue
        if not repos:
            misses.append("%s (no repo mapping)" % label)
            continue
        rf = []
        for repo in repos:
            if repo not in cache:
                try:
                    cache[repo] = get_boxart_list(repo)
                except Exception as e:
                    print("! index fetch failed for %s: %s" % (repo, e))
                    cache[repo] = []
            rf.append((repo, cache[repo]))
        repo, f = pick_across(album, rf)
        if not f:
            misses.append(label)
            continue
        raw = ("https://raw.githubusercontent.com/libretro-thumbnails/%s"
               "/master/Named_Boxarts/%s" % (repo, urllib.parse.quote(f)))
        try:
            with urllib.request.urlopen(urllib.request.Request(raw, headers=UA), timeout=30) as r:
                blob = r.read()
            with open(dest, "wb") as out:
                out.write(blob)
            hit += 1
            print("OK   %s  <-  [%s] %s" % (label, repo, f))
        except Exception as e:
            misses.append("%s (download: %s)" % (label, e))

    print("\nfetched %d, skipped %d (already had art), missed %d"
          % (hit, skip, len(misses)))
    for m in misses:
        print("  MISS", m)


if __name__ == "__main__":
    main()
