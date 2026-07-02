#!/usr/bin/env python3
"""Rename SNES .rsn sets to their real game titles.

SNESmusic.org packs are named with abbreviations (smw.rsn, sf2.rsn, mk2.rsn),
but each archive's info.txt carries the full game title on line 1. This reads
that title and renames the file to "<Game Title> (SNES).rsn", which the jukebox
shows as the album name.

Reads info.txt without unpacking the whole archive (bsdtar/libarchive, falling
back to unar or 7z) -- the .rsn files themselves are left untouched.

Dry-run by default; pass --apply to actually rename.

Usage:
  rename-rsn.py DIR                 # preview
  rename-rsn.py --apply DIR         # do it
  rename-rsn.py --apply --suffix "" DIR   # no "(SNES)" suffix
"""
import os
import re
import argparse
import shutil
import tempfile
import subprocess


def _first_line(data):
    for line in data.replace(b"\r", b"").split(b"\n"):
        s = line.decode("latin-1", "ignore").strip()
        if s:
            return s
    return ""


def read_info_title(rsn):
    """First non-empty line of info.txt inside the .rsn, or ''."""
    # 1) libarchive streams a single member to stdout (reads RAR4 rsn packs)
    if shutil.which("bsdtar"):
        try:
            out = subprocess.check_output(["bsdtar", "-xOf", rsn, "info.txt"],
                                          stderr=subprocess.DEVNULL)
            t = _first_line(out)
            if t:
                return t
        except Exception:
            pass
    # 2) fall back: extract info.txt to a temp dir with unar or 7z
    tmp = tempfile.mkdtemp()
    try:
        for cmd in (["unar", "-quiet", "-force-overwrite", "-no-directory",
                     "-output-directory", tmp, rsn],
                    ["7z", "x", "-y", "-o" + tmp, rsn, "info.txt"]):
            if not shutil.which(cmd[0]):
                continue
            try:
                subprocess.check_call(cmd, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL)
            except Exception:
                continue
            p = os.path.join(tmp, "info.txt")
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    return _first_line(f.read())
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ""


def sanitize(title):
    """Make a title safe as a filename (/, : are illegal/awkward)."""
    return re.sub(r'\s+', ' ', re.sub(r'[/:]', ' - ', title)).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", help="folder of .rsn files")
    ap.add_argument("--apply", action="store_true",
                    help="actually rename (default: dry-run preview)")
    ap.add_argument("--suffix", default="(SNES)",
                    help='appended before .rsn (default "(SNES)"; "" for none)')
    args = ap.parse_args()

    rsns = sorted(f for f in os.listdir(args.dir) if f.lower().endswith(".rsn"))
    renamed = unchanged = missed = 0
    for fn in rsns:
        src = os.path.join(args.dir, fn)
        title = read_info_title(src)
        if not title:
            print("SKIP (no info.txt title): %s" % fn)
            missed += 1
            continue
        clean = sanitize(title)
        new = ("%s %s.rsn" % (clean, args.suffix)) if args.suffix else "%s.rsn" % clean
        if new == fn:
            unchanged += 1
            continue
        if os.path.exists(os.path.join(args.dir, new)):
            # collision (e.g. a duplicate download): keep the abbreviation to disambiguate
            base = os.path.splitext(fn)[0]
            new = ("%s [%s] %s.rsn" % (clean, base, args.suffix)) if args.suffix \
                else "%s [%s].rsn" % (clean, base)
        if args.apply:
            os.rename(src, os.path.join(args.dir, new))
            print("renamed: %s  ->  %s" % (fn, new))
            renamed += 1
        else:
            print("%-18s ->  %s" % (fn, new))

    if args.apply:
        print("\nrenamed %d, unchanged %d, missed %d" % (renamed, unchanged, missed))
    else:
        print("\n(dry-run) re-run with --apply to rename")


if __name__ == "__main__":
    main()
