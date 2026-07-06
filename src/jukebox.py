#!/usr/bin/env python3
"""GPi Case "Game Music" jukebox UI/controller.

Launched by vgmplay.sh for register-format tracks (VGM/VGZ/GYM/S98/DRO).
The selected file's *folder* is treated as the album/game; all sibling tracks
become a queue. Audio is produced live by the vgmjuke engine (libvgm), one
process per track. This process owns the 640x480 framebuffer (box art + big
title + track list + status) and the gamepad.

Controls (button numbers configurable below):
    A            cycle loop mode  oo -> 0 -> 1 -> 2 -> 3 -> 4  (restarts track)
    B            exit to the menu
    Select       toggle Continuous (auto-next) vs Single (stop after track)
    L shoulder   previous track
    R shoulder   next track
"""
import os
import re
import sys
import json
import time
import select
import signal
import struct
import random
import shutil
import tempfile
import threading
import subprocess
import queue

# ----------------------------------------------------------------------------
# Config
# ----------------------------------------------------------------------------
# The engines and the saved button map live next to this script, wherever it
# is installed, so the whole thing is relocatable.
HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE_VGM = os.path.join(HERE, "vgmjuke")   # libvgm: VGM/VGZ/...
ENGINE_GME = os.path.join(HERE, "gmejuke")   # libgme: NSF/GBS/...
ENGINE_MOD = os.path.join(HERE, "modjuke")   # libopenmpt: MOD/XM/S3M/IT/...
ENGINE_SID = os.path.join(HERE, "sidjuke")   # libsidplayfp: SID/PSID
ENGINE_GM = os.path.join(HERE, "gmjuke")     # FluidSynth: MID/MIDI (GM)
ENGINE_ST = os.path.join(HERE, "stjuke")     # libsc68: Atari ST/Amiga SNDH/sc68
ENGINE_PSG = os.path.join(HERE, "psgjuke")   # libpsgplay: Atari ST/STE SNDH (STE DMA)
BTN_MAP_FILE = os.path.join(HERE, "buttons.json")
JS_DEV = "/dev/input/js0"
FB_DEV = "/dev/fb0"

# First font found is used (FreeSans/DejaVu ship on Raspbian/Debian).
FONT_CANDIDATES_BOLD = [
    "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]
FONT_CANDIDATES_REG = [
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]

# Gamepad button numbers. Captured once via the on-screen setup wizard on first
# run and saved to BTN_MAP_FILE. These are only the fallback defaults.
DEFAULT_BMAP = {"A": 0, "B": 1, "SELECT": 6, "L": 4, "R": 5}

# Loop modes shown in the UI, in cycle order. "oo" = infinite.
LOOP_MODES = ["oo", "0", "1", "2", "3", "4"]
DEFAULT_LOOP_INDEX = 3          # start at "2"

# Play modes cycled by Select. What happens when a track/album finishes:
#   SINGLE  - stop after the current track
#   ALBUM   - play the rest of this album/folder, then stop
#   ALL     - at album end, roll into the next folder and keep going
#   SHUFFLE - at album end, jump to a random folder
PLAY_MODES = ["SINGLE", "ALBUM", "ALL", "SHUFFLE"]
PLAY_LABELS = {"SINGLE": "Single", "ALBUM": "Album", "ALL": "All", "SHUFFLE": "Shuffle"}
DEFAULT_PLAY_INDEX = 1          # "ALBUM" (previous default behavior)
FADE_SECONDS = 4.0              # fade for finite loop modes

# Each engine is one of two kinds:
#   "sibling" - one song per file; the album folder's files are the queue and
#               filenames are the track names (VGM/VGZ, MOD, MIDI).
#   "subtune" - one file holds many subtunes, queried via --info/--track and
#               shown as numbered tracks (NSF/GBS/..., SID).
VGM_EXTS = (".vgz", ".vgm", ".gym", ".s98", ".dro")
GME_EXTS = (".nsf", ".nsfe", ".gbs", ".ay", ".hes", ".kss", ".sap")  # multi-subtune
SPC_EXTS = (".spc",)          # SNES; gmejuke, but one song per file -> sibling
MOD_EXTS = (".mod", ".xm", ".s3m", ".it", ".mtm", ".mptm", ".med", ".stm",
            ".okt", ".ptm", ".dbm", ".digi", ".ahx", ".hvl", ".mo3", ".umx")
SID_EXTS = (".sid", ".psid")
MIDI_EXTS = (".mid", ".midi", ".rmi")
SNDH_EXTS = (".sndh", ".sc68")               # Atari ST/Amiga; multi-subtune

# Container formats: not decoded directly - unpacked to sibling tracks first.
# .rsn = a RAR archive of .spc files (one SNES game's soundtrack).
RSN_EXTS = (".rsn",)
CONTAINER_EXTS = RSN_EXTS

# (engine binary, its extensions, kind). Order = match priority.
_ENGINE_SPECS = [
    (ENGINE_VGM, VGM_EXTS, "sibling"),
    (ENGINE_GME, GME_EXTS, "subtune"),
    (ENGINE_GME, SPC_EXTS, "sibling"),
    (ENGINE_MOD, MOD_EXTS, "sibling"),
    (ENGINE_SID, SID_EXTS, "subtune"),
    (ENGINE_GM, MIDI_EXTS, "sibling"),
    # SNDH: prefer psgjuke (psgplay - emulates STE DMA sound, so it plays the
    # STe/MaxYMiser tunes sc68 renders thin); it exec()s stjuke as a fallback for
    # .sc68 / ICE-packed / anything it can't load. stjuke listed second so that if
    # psgjuke didn't build, SNDH still routes to sc68 (fail-soft via setdefault).
    (ENGINE_PSG, SNDH_EXTS, "subtune"),
    (ENGINE_ST, SNDH_EXTS, "subtune"),
]


def _build_ext_map():
    """ext -> (engine, kind), including only engines whose binary is present.
    Fail-soft: an engine that didn't build just drops its formats."""
    m = {}
    for engine, exts, kind in _ENGINE_SPECS:
        if not os.path.exists(engine):
            continue
        for e in exts:
            m.setdefault(e, (engine, kind))
    return m


EXT_ENGINE = _build_ext_map()
AUDIO_EXTS = tuple(EXT_ENGINE.keys())

# Box art is reused from the matching console ROM system's images/ folder.
# gme category folder name -> RetroPie system name (roms/<system>/images).
# The roms root is derived from the track path at runtime, so no absolute paths.
SRC_SYSTEMS = {
    "NES": "nes",
    "Genesis": "megadrive",
    "Arcade": "arcade",
    "Master System": "mastersystem",
    "Game Gear": "gamegear",
}
# Manual overrides for names that don't match by heuristic. Keyed by the album
# folder name (preferred, unambiguous) or the paren-stripped base name. The
# value is the image stem to look for (e.g. a MAME short name for arcade art).
ART_OVERRIDES = {
    "Batman - The Video Game (NES)": "Batman",
    "Golden Axe (Sega System 16B)": "goldnaxe",   # arcade MAME name
    "Shadow Dancer (Sega System 18)": "shdancer",  # arcade MAME name
}

# Framebuffer pixel byte order. Pi 32bpp is usually BGRA; flip to "RGBA" if the
# colours come out swapped (red/blue) on the real screen.
FB_RAWMODE = "BGRA"

GRACE_SECONDS = 1.0  # ignore button presses right after launch (the launch key)

# ----------------------------------------------------------------------------
# Palette
# ----------------------------------------------------------------------------
BG = (16, 18, 24)
FG = (230, 232, 238)
DIM = (120, 126, 140)
ACCENT = (96, 200, 255)
HILITE_BG = (40, 70, 110)
HILITE_FG = (255, 255, 255)


def natural_key(s):
    return [int(t) if t.isdigit() else t.lower()
            for t in re.split(r'(\d+)', s)]


def strip_paren_suffix(name):
    """'Mega Man 2 (NES)' -> 'Mega Man 2'."""
    return re.sub(r'\s*\([^)]*\)\s*$', '', name).strip()


def normalize(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())


# ----------------------------------------------------------------------------
# Box-art matching
# ----------------------------------------------------------------------------
def candidate_targets(base):
    """Normalized name variants to try, handling 'The' article reordering."""
    out = {normalize(base)}
    low = base.lower()
    if low.startswith("the "):
        rest = base[4:]
        out.add(normalize(rest + ", The"))  # 'Legend of Zelda, The'
        out.add(normalize(rest))
    elif low.endswith(", the"):
        out.add(normalize("The " + base[:-5]))
    return {t for t in out if t}


def find_box_art(rom_path):
    """Return a path to a matching image for the selected track's album, or None.

    rom_path: .../gme/<Category>/<Album>/<track>
    """
    album_dir = os.path.dirname(rom_path)
    album = os.path.basename(album_dir)
    category = os.path.basename(os.path.dirname(album_dir))

    # 1) per-album art dropped in the album folder (self-contained; what
    #    fetch-art.py writes). Preferred so the library is portable.
    for cand in ("folder.png", "box.png", "cover.png", album + ".png"):
        p = os.path.join(album_dir, cand)
        if os.path.isfile(p):
            return p

    # 2) fall back to the matching console game system's images on the drive.
    #    Derive the roms root from the track path: <roms>/gme/<Cat>/<Album>/file
    system = SRC_SYSTEMS.get(category)
    if not system:
        return None
    roms_root = os.path.dirname(os.path.dirname(os.path.dirname(album_dir)))
    img_dir = os.path.join(roms_root, system, "images")
    if not os.path.isdir(img_dir):
        return None

    base = strip_paren_suffix(album)
    override = ART_OVERRIDES.get(album) or ART_OVERRIDES.get(base)
    if override:
        base = override
    targets = candidate_targets(base)
    if not targets:
        return None

    try:
        pngs = [f for f in os.listdir(img_dir) if f.lower().endswith(".png")]
    except OSError:
        return None

    stems = [(f, normalize(os.path.splitext(f)[0])) for f in pngs]

    # 1) exact normalized match against any target variant
    for f, n in stems:
        if n in targets:
            return os.path.join(img_dir, f)
    # 2) prefix match either direction (e.g. album subtitle vs short stem)
    cands = [f for f, n in stems
             if any(n.startswith(t) or t.startswith(n) for t in targets)]
    if cands:
        cands.sort(key=len)  # prefer the closest (shortest) stem
        return os.path.join(img_dir, cands[0])
    return None


# ----------------------------------------------------------------------------
# Framebuffer drawing
# ----------------------------------------------------------------------------
class Screen:
    def __init__(self):
        from PIL import Image, ImageDraw, ImageFont
        self._Image = Image
        self._ImageDraw = ImageDraw
        w, h = 640, 480
        try:
            with open("/sys/class/graphics/fb0/virtual_size") as fh:
                w, h = (int(x) for x in fh.read().strip().split(","))
        except Exception:
            pass
        self.w, self.h = w, h
        bold = self._font_path(FONT_CANDIDATES_BOLD)
        reg = self._font_path(FONT_CANDIDATES_REG)
        self.f_title = ImageFont.truetype(bold, 40)
        self.f_cat = ImageFont.truetype(reg, 20)
        self.f_track = ImageFont.truetype(reg, 22)
        self.f_track_b = ImageFont.truetype(bold, 22)
        self.f_status = ImageFont.truetype(bold, 22)
        self.f_hint = ImageFont.truetype(reg, 16)

    @staticmethod
    def _font_path(candidates):
        for p in candidates:
            if os.path.isfile(p):
                return p
        return candidates[0]  # let PIL raise a clear error if truly missing

    def _text_w(self, draw, s, font):
        try:
            b = draw.textbbox((0, 0), s, font=font)
            return b[2] - b[0]
        except AttributeError:
            return draw.textsize(s, font=font)[0]

    def _fit(self, draw, s, font, maxw):
        if self._text_w(draw, s, font) <= maxw:
            return s
        while s and self._text_w(draw, s + "...", font) > maxw:
            s = s[:-1]
        return s + "..."

    def load_art(self, path, box):
        if not path:
            return None
        try:
            im = self._Image.open(path).convert("RGB")
        except Exception:
            return None
        im.thumbnail((box, box))
        return im

    def render(self, st):
        Image = self._Image
        img = Image.new("RGB", (self.w, self.h), BG)
        d = self._ImageDraw.Draw(img)
        pad = 24

        # --- header: category + big game/album title ---
        d.text((pad, 14), st.category.upper(), font=self.f_cat, fill=ACCENT)
        title = self._fit(d, st.album, self.f_title, self.w - 2 * pad)
        d.text((pad, 38), title, font=self.f_title, fill=FG)
        d.line([(pad, 92), (self.w - pad, 92)], fill=DIM, width=1)

        # --- box art (left) ---
        art_box = 220
        list_x = pad
        top = 108
        if st.art is not None:
            ax = pad
            ay = top
            img.paste(st.art, (ax, ay))
            list_x = ax + st.art.width + 24
        else:
            list_x = pad

        # --- track list (windowed around current) ---
        line_h = 30
        rows = max(4, (self.h - top - 70) // line_h)
        n = len(st.tracks)
        half = rows // 2
        start = max(0, min(st.idx - half, max(0, n - rows)))
        y = top
        list_w = self.w - pad - list_x
        for i in range(start, min(n, start + rows)):
            name = self._fit(d, st.tracks[i], self.f_track, list_w - 16)
            if i == st.idx:
                d.rectangle([list_x - 6, y - 3, self.w - pad, y + line_h - 7],
                            fill=HILITE_BG)
                marker = ">" if st.playing else "|"
                d.text((list_x, y), marker + " " + name,
                       font=self.f_track_b, fill=HILITE_FG)
            else:
                d.text((list_x + 16, y), name, font=self.f_track, fill=DIM)
            y += line_h

        # --- status bar ---
        sy = self.h - 56
        d.line([(pad, sy - 8), (self.w - pad, sy - 8)], fill=DIM, width=1)
        if not st.loopable:
            loop = "No loop point"
        elif st.loop_mode == "oo":
            loop = "Loop ∞"
        else:
            loop = "Loop %s" % st.loop_mode
        mode = PLAY_LABELS[st.play_mode]
        if st.total and st.total > 0:
            tt = "%s / %s" % (fmt_time(st.cur), fmt_time(st.total))
        else:
            tt = fmt_time(st.cur)
        left = "%s   %s   %d/%d" % (loop, mode, st.idx + 1, n)
        d.text((pad, sy), left, font=self.f_status, fill=ACCENT)
        tw = self._text_w(d, tt, self.f_status)
        d.text((self.w - pad - tw, sy), tt, font=self.f_status, fill=FG)

        # --- button hints ---
        hint = "A: loop   B: exit   Select: mode   L/R: prev/next"
        d.text((pad, self.h - 24), hint, font=self.f_hint, fill=DIM)

        self._blit(img)

    def _blit(self, img):
        raw = img.convert("RGBA").tobytes("raw", FB_RAWMODE)
        with open(FB_DEV, "wb") as fb:
            fb.write(raw)

    def clear(self):
        img = self._Image.new("RGB", (self.w, self.h), (0, 0, 0))
        self._blit(img)

    def render_setup(self, heading, big, sub, captured):
        """Setup-wizard screen: heading, a big prompt, a sub line, and the list
        of buttons captured so far."""
        Image = self._Image
        img = Image.new("RGB", (self.w, self.h), BG)
        d = self._ImageDraw.Draw(img)
        pad = 24
        d.text((pad, 28), heading, font=self.f_cat, fill=ACCENT)
        big = self._fit(d, big, self.f_title, self.w - 2 * pad)
        d.text((pad, 70), big, font=self.f_title, fill=FG)
        if sub:
            d.text((pad, 128), sub, font=self.f_track, fill=DIM)
        y = 210
        for label, num in captured:
            d.text((pad, y), "%-8s -> button %d" % (label, num),
                   font=self.f_track, fill=FG)
            y += 30
        d.text((pad, self.h - 28),
               "Press each button once when asked.",
               font=self.f_hint, fill=DIM)
        self._blit(img)


def fmt_time(sec):
    if sec is None or sec < 0:
        return "--:--"
    sec = int(sec)
    return "%d:%02d" % (sec // 60, sec % 60)


# ----------------------------------------------------------------------------
# Player state
# ----------------------------------------------------------------------------
class State:
    def __init__(self, tracks, idx, category, album, art, source_rom=None):
        self.tracks = tracks            # display names
        self.idx = idx
        self.category = category
        self.album = album
        self.art = art
        self.source_rom = source_rom    # the file this album was launched from
        self.loop_index = DEFAULT_LOOP_INDEX
        self.play_index = DEFAULT_PLAY_INDEX
        self.cur = 0.0
        self.total = None
        self.playing = False
        self.loopable = True            # engine reports LOOP 0 for no-loop tracks

    @property
    def loop_mode(self):
        return LOOP_MODES[self.loop_index]

    @property
    def play_mode(self):
        return PLAY_MODES[self.play_index]


# ----------------------------------------------------------------------------
# Gamepad reader
# ----------------------------------------------------------------------------
def joystick_thread(evq, stop_evt):
    JS_EVENT_BUTTON = 0x01
    JS_EVENT_INIT = 0x80
    try:
        f = open(JS_DEV, "rb")
    except Exception:
        return
    start = time.time()
    while not stop_evt.is_set():
        ev = f.read(8)
        if not ev or len(ev) < 8:
            break
        _t, value, typ, number = struct.unpack("IhBB", ev)
        if typ & JS_EVENT_INIT:
            continue
        if (typ & ~JS_EVENT_INIT) != JS_EVENT_BUTTON:
            continue
        if value != 1:  # only button-down
            continue
        if time.time() - start < GRACE_SECONDS:
            continue
        evq.put(number)


# ----------------------------------------------------------------------------
# Jukebox engine wrapper
# ----------------------------------------------------------------------------
class Jukebox:
    def __init__(self, entries, state, screen=None):
        # each entry: {"engine": path, "file": path, "track": int|None, "name": str}
        self.entries = entries
        self.state = state
        self.screen = screen            # for reloading box art on album change
        self.proc = None
        self._killed = False
        self._reader = None

    def load_source(self, rom):
        """Swap the whole queue to a different album/file and start playing it."""
        entries, idx, category, album, art_path = describe(rom)
        if not entries:
            return False
        self.stop_proc()
        self.entries = entries
        st = self.state
        st.source_rom = rom
        st.tracks = [e["name"] for e in entries]
        st.idx = idx
        st.category = category
        st.album = album
        if self.screen is not None:
            st.art = self.screen.load_art(art_path, 220)
        self.start_track()
        return True

    def advance_album(self, shuffle=False):
        """Move to the next (or a random) sibling album. Returns False if there
        is nowhere to go (end of a sequential scope)."""
        src = self.state.source_rom
        if not src:
            return False
        albums = sibling_albums(src)
        if not albums:
            return False
        if album_is_file(src):
            cur = src
        else:
            cur_dir = os.path.dirname(src)
            cur = next((a for a in albums if os.path.dirname(a) == cur_dir), None)
        try:
            i = albums.index(cur)
        except ValueError:
            i = -1
        if shuffle:
            pool = [a for a in albums if a != cur] or albums
            nxt = random.choice(pool)
        else:
            if i + 1 >= len(albums):
                return False
            nxt = albums[i + 1]
        return self.load_source(nxt)

    def engine_args(self):
        mode = self.state.loop_mode
        if mode == "oo":
            return ["--infinite", "--fade", str(FADE_SECONDS)]
        return ["--loops", mode, "--fade", str(FADE_SECONDS)]

    def send_mode(self):
        """Tell the running engine to change loop mode live (no restart)."""
        mode = self.state.loop_mode
        cmd = "inf\n" if mode == "oo" else ("loops %s\n" % mode)
        try:
            self.proc.stdin.write(cmd)
            self.proc.stdin.flush()
        except Exception:
            self.start_track()

    def start_track(self):
        self.stop_proc()
        self.state.cur = 0.0
        self.state.total = None
        self.state.loopable = True       # until the engine says otherwise
        self.state.playing = True
        entry = self.entries[self.state.idx]
        cmd = [entry["engine"]] + self.engine_args()
        if entry.get("track"):
            cmd += ["--track", str(entry["track"])]
        cmd += [entry["file"]]
        self._killed = False
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            bufsize=1, universal_newlines=True,
        )
        self._reader = threading.Thread(target=self._read_pos, daemon=True)
        self._reader.start()

    def _read_pos(self):
        proc = self.proc
        for line in proc.stdout:
            if proc is not self.proc:
                return
            line = line.strip()
            if line.startswith("POS "):
                parts = line.split()
                try:
                    self.state.cur = float(parts[1])
                    tot = float(parts[2])
                    self.state.total = None if tot < 0 else tot
                except (IndexError, ValueError):
                    pass
            elif line.startswith("LOOP "):
                self.state.loopable = line.split()[1] != "0"

    def stop_proc(self):
        if self.proc and self.proc.poll() is None:
            self._killed = True
            try:
                self.proc.terminate()
                self.proc.wait(timeout=2)
            except Exception:
                try:
                    self.proc.kill()
                except Exception:
                    pass
        self.proc = None

    def track_ended_naturally(self):
        """True if the engine exited on its own (song finished), not a kill."""
        if self.proc is None:
            return False
        if self.proc.poll() is None:
            return False
        return not self._killed

    def next_track(self, wrap_exits=True):
        if self.state.idx + 1 >= len(self.entries):
            return False if wrap_exits else self._set(0)
        return self._set(self.state.idx + 1)

    def prev_track(self):
        return self._set((self.state.idx - 1) % len(self.entries))

    def _set(self, i):
        self.state.idx = i
        self.start_track()
        return True

    def cycle_loop(self):
        if not self.state.loopable:
            return                       # no loop point: A does nothing
        self.state.loop_index = (self.state.loop_index + 1) % len(LOOP_MODES)
        # apply live so the song keeps playing; only restart if no live process
        if self.proc is not None and self.proc.poll() is None:
            self.send_mode()
        else:
            self.start_track()


# ----------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------
def subtunes(engine, rom_path):
    """Query an engine (--info) for a multi-subtune file (NSF/GBS/SID/...).
    Returns entries or None."""
    try:
        out = subprocess.check_output(
            [engine, "--info", rom_path],
            universal_newlines=True, stderr=subprocess.DEVNULL)
    except Exception:
        return None
    count = 0
    names = {}
    for line in out.splitlines():
        if line.startswith("TRACKS "):
            try:
                count = int(line.split()[1])
            except (IndexError, ValueError):
                pass
        elif line.startswith("TRK "):
            parts = line.split(" ", 3)
            try:
                i = int(parts[1])
            except (IndexError, ValueError):
                continue
            nm = parts[3].strip() if len(parts) > 3 else ""
            if nm:
                names[i] = nm
    if count <= 0:
        return None
    return [{"engine": engine, "file": rom_path, "track": i,
             "name": names.get(i, "Track %d" % i)} for i in range(1, count + 1)]


def spc_song_title(path):
    """Song title from an .spc file's ID666 tag (text form, offset 0x2E, 32
    bytes). Returns '' if absent - caller falls back to the filename."""
    try:
        with open(path, "rb") as f:
            f.seek(0x2E)
            raw = f.read(32)
    except Exception:
        return ""
    t = raw.split(b"\x00")[0].decode("latin-1", "ignore")
    return "".join(c for c in t if c.isprintable()).strip()


def track_name(path):
    """Display name for a sibling track: SPC song title if tagged, else filename."""
    if path.lower().endswith(".spc"):
        title = spc_song_title(path)
        if title:
            return title
    return os.path.splitext(os.path.basename(path))[0]


def rsn_playlist(rom_path):
    """Unpack a .rsn (a RAR archive of .spc files, one SNES game's soundtrack)
    to a temp dir and return gmejuke sibling entries for its SPCs, or None."""
    if not os.path.exists(ENGINE_GME):
        return None
    dest = os.path.join(tempfile.gettempdir(), "rsnplay")
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest, exist_ok=True)
    # extractor preference: unar (best RAR support), then libarchive/7z/unrar
    attempts = [
        ["unar", "-quiet", "-force-overwrite", "-no-directory", "-output-directory", dest, rom_path],
        ["bsdtar", "-xf", rom_path, "-C", dest],
        ["7z", "x", "-y", "-o" + dest, rom_path],
        ["unrar", "x", "-y", "-inul", rom_path, dest + "/"],
    ]
    for cmd in attempts:
        if not shutil.which(cmd[0]):
            continue
        try:
            subprocess.check_call(cmd, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
            break
        except Exception:
            continue
    # collect .spc (some tools nest into a subfolder)
    spcs = []
    for root, _dirs, files in os.walk(dest):
        for f in files:
            if f.lower().endswith(".spc"):
                spcs.append(os.path.join(root, f))
    if not spcs:
        return None
    spcs.sort(key=natural_key)
    return [{"engine": ENGINE_GME, "file": p, "track": None,
             "name": track_name(p)} for p in spcs]


def _subtune_count(path):
    """Best-effort subtune count from the file header - fast, no subprocess.
    Returns 1 for plain single-song files and for anything we can't parse, so a
    folder album treats those as a single track."""
    ext = os.path.splitext(path)[1].lower()
    try:
        with open(path, "rb") as f:
            head = f.read(2048)
    except OSError:
        return 1
    try:
        if ext in (".sid", ".psid") and head[:4] in (b"PSID", b"RSID"):
            return max(1, struct.unpack_from(">H", head, 0x0E)[0])  # PSID songs
        if ext == ".nsf" and head[:4] == b"NESM":
            return max(1, head[0x06])                                # NSF songs
        if ext == ".sap":
            m = re.search(rb"SONGS\s+(\d+)", head)                   # SAP header tag
            if m:
                return max(1, int(m.group(1)))
        if ext in (".sndh", ".sc68"):
            j = head.find(b"SNDH")                                   # SNDH subtune count
            if j >= 0:
                m = re.search(rb"##(\d+)", head[j:j + 2048])
                if m:
                    return max(1, int(m.group(1)))
    except Exception:
        return 1
    return 1


def _subtune_siblings(rom_path, engine):
    """Sorted subtune files sharing rom_path's folder that use the same engine
    (e.g. every .sap in one composer's folder)."""
    sib_exts = tuple(e for e, (eng, k) in EXT_ENGINE.items()
                     if eng == engine and k == "subtune")
    try:
        files = [f for f in os.listdir(os.path.dirname(rom_path))
                 if f.lower().endswith(sib_exts)]
    except OSError:
        files = []
    return sorted(files, key=natural_key)


def build_playlist(rom_path):
    """Return (entries, start_index). Container formats (.rsn) unpack to sibling
    tracks; sibling-file engines (VGM/MOD/MIDI/SPC) queue every file in the album
    folder; subtune engines (NSF/SID/...) queue the file's subtunes - unless many
    subtune files share the folder (a composer collection), in which case the
    folder is the album, one track per file."""
    ext = os.path.splitext(rom_path)[1].lower()

    if ext in CONTAINER_EXTS:
        entries = rsn_playlist(rom_path)
        return (entries, 0) if entries else ([], 0)

    spec = EXT_ENGINE.get(ext)
    if spec is None:
        return [], 0
    engine, kind = spec

    if kind == "subtune":
        album_dir = os.path.dirname(rom_path)
        sibs = _subtune_siblings(rom_path, engine)
        if len(sibs) > 1 and _subtune_count(rom_path) <= 1:
            # The selected file is a single song sharing its folder with others
            # (a composer/demo collection, e.g. ZX Spectrum Demos or a C64
            # composer's standalone tunes): the album is the folder's *single-
            # song* files, one track each. Multi-subtune files (game soundtracks,
            # sound-effect collections) are their own albums, so they don't
            # appear here as entries that would only play one of their subtunes.
            # Counts come from headers (no --info), so this is fast for hundreds
            # of files.
            singles = [f for f in sibs
                       if _subtune_count(os.path.join(album_dir, f)) <= 1]
            if os.path.basename(rom_path) not in singles:
                singles.append(os.path.basename(rom_path))
                singles.sort(key=natural_key)
            entries = [{"engine": engine, "file": os.path.join(album_dir, f),
                        "track": None,
                        "name": track_name(os.path.join(album_dir, f))}
                       for f in singles]
            idx = next((k for k, f in enumerate(singles)
                        if f == os.path.basename(rom_path)), 0)
            return entries, idx
        # A multi-song file (NSF/AY game, a C64 game soundtrack) or a lone file:
        # the album is this file's own subtunes.
        entries = subtunes(engine, rom_path)
        if entries:
            return entries, 0
        # fallback: play the file as a single track
        name = os.path.splitext(os.path.basename(rom_path))[0]
        return [{"engine": engine, "file": rom_path,
                 "track": 1, "name": name}], 0

    # sibling: queue every file in the album folder with the same engine + kind
    album_dir = os.path.dirname(rom_path)
    sib_exts = tuple(e for e, (eng, k) in EXT_ENGINE.items()
                     if eng == engine and k == kind)
    files = [f for f in os.listdir(album_dir) if f.lower().endswith(sib_exts)]
    files.sort(key=natural_key)
    entries = [{"engine": engine, "file": os.path.join(album_dir, f),
                "track": None, "name": track_name(os.path.join(album_dir, f))}
               for f in files]
    sel = os.path.basename(rom_path)
    idx = next((k for k, f in enumerate(files) if f == sel), 0)
    return entries, idx


def describe(rom):
    """Everything the UI needs for a launched file: (entries, start_idx,
    category, album, art_path). Containers/one-file albums sit directly under
    their category and *are* the album; folder-albums use their folder."""
    entries, idx = build_playlist(rom)
    ext = os.path.splitext(rom)[1].lower()
    spec = EXT_ENGINE.get(ext)
    # A flat multi-song game (an .ay/.nsf sitting among sibling games) is its own
    # album: name it after the file, with its folder as the category - like a
    # container. Wrapped/lone files and folder-albums use their folder instead.
    flat_file_album = (spec and spec[1] == "subtune"
                       and len(_subtune_siblings(rom, spec[0])) > 1
                       and album_is_file(rom))
    if ext in CONTAINER_EXTS or flat_file_album:
        album = strip_paren_suffix(os.path.splitext(os.path.basename(rom))[0])
        category = os.path.basename(os.path.dirname(rom))
    else:
        album_dir = os.path.dirname(rom)
        album = strip_paren_suffix(os.path.basename(album_dir))
        category = os.path.basename(os.path.dirname(album_dir))
    return entries, idx, category, album, find_box_art(rom)


def album_is_file(rom):
    """True if the album is a single file (subtune or container) rather than a
    folder of tracks - decides how we find sibling albums. A single-song subtune
    file that shares its folder with others is part of a folder album (a
    composer/demo collection); a multi-song subtune file (an NSF/AY game) is its
    own album even when it sits flat among sibling games."""
    ext = os.path.splitext(rom)[1].lower()
    if ext in CONTAINER_EXTS:
        return True
    spec = EXT_ENGINE.get(ext)
    if not spec or spec[1] != "subtune":
        return False
    if len(_subtune_siblings(rom, spec[0])) <= 1:
        return True
    return _subtune_count(rom) > 1


def first_playable(dirpath):
    """A representative playable file inside dirpath (recursively), or None."""
    for root, _dirs, files in os.walk(dirpath):
        for f in sorted(files, key=natural_key):
            ext = os.path.splitext(f)[1].lower()
            if ext in AUDIO_EXTS or ext in CONTAINER_EXTS:
                return os.path.join(root, f)
    return None


def sibling_albums(rom):
    """Ordered list of album 'rom' paths in the same scope as `rom`: sibling
    files for one-file albums, sibling folders (a representative file each) for
    folder-albums."""
    if album_is_file(rom):
        d = os.path.dirname(rom)
        ext = os.path.splitext(rom)[1].lower()
        out = []
        for f in sorted(os.listdir(d), key=natural_key):
            p = os.path.join(d, f)
            if os.path.isfile(p) and os.path.splitext(f)[1].lower() == ext:
                out.append(p)
        return out
    album_dir = os.path.dirname(rom)
    parent = os.path.dirname(album_dir)
    out = []
    for sub in sorted(os.listdir(parent), key=natural_key):
        subp = os.path.join(parent, sub)
        if os.path.isdir(subp):
            rep = first_playable(subp)
            if rep:
                out.append(rep)
    return out


def load_button_map():
    try:
        with open(BTN_MAP_FILE) as fh:
            m = json.load(fh)
        if all(k in m for k in ("A", "B", "SELECT", "L", "R")):
            return m
    except Exception:
        pass
    return None


def _drain_js(jsf):
    while select.select([jsf], [], [], 0)[0]:
        if not jsf.read(8):
            break


def _wait_button(jsf):
    """Block until a fresh button-down; return its number (or None on EOF)."""
    while True:
        ev = jsf.read(8)
        if not ev or len(ev) < 8:
            return None
        _t, value, typ, number = struct.unpack("IhBB", ev)
        if typ & 0x80:                  # ignore synthetic INIT events
            continue
        if (typ & ~0x80) == 0x01 and value == 1:
            return number


def calibrate(screen):
    """On-screen button setup wizard. Returns the button map and saves it."""
    prompts = [
        ("A", "cycles loop mode  oo/0/1/2/3/4"),
        ("B", "exits to the menu"),
        ("SELECT", "cycles play mode  Single/Album/All/Shuffle"),
        ("L", "previous track  (left shoulder)"),
        ("R", "next track  (right shoulder)"),
    ]
    try:
        jsf = open(JS_DEV, "rb")
    except Exception:
        return dict(DEFAULT_BMAP)

    mapping = {}
    captured = []
    time.sleep(1.0)
    _drain_js(jsf)
    for key, desc in prompts:
        screen.render_setup("CONTROLLER SETUP",
                            "Press  %s" % key, desc, captured)
        _drain_js(jsf)
        num = _wait_button(jsf)
        if num is None:
            break
        mapping[key] = num
        captured.append((key, num))
        screen.render_setup("CONTROLLER SETUP",
                            "%s = button %d" % (key, num),
                            "release the button...", captured)
        time.sleep(0.7)
        _drain_js(jsf)
    jsf.close()

    screen.render_setup("CONTROLLER SETUP", "All set!",
                        "starting the player...", captured)
    time.sleep(1.0)
    try:
        with open(BTN_MAP_FILE, "w") as fh:
            json.dump(mapping, fh)
    except Exception:
        pass
    # fill any gaps from defaults just in case
    for k, v in DEFAULT_BMAP.items():
        mapping.setdefault(k, v)
    return mapping


def main():
    if len(sys.argv) < 2:
        print("usage: jukebox.py <track>")
        return 1
    rom = sys.argv[1]
    entries, idx, category, album, art_path = describe(rom)
    if not entries:
        return 1
    names = [e["name"] for e in entries]

    screen = Screen()
    # hide console cursor / clear before we draw to the framebuffer
    sys.stdout.write("\033[2J\033[?25l")
    sys.stdout.flush()

    # first run: walk the user through button setup on the GPi screen
    bmap = load_button_map()
    if bmap is None:
        bmap = calibrate(screen)

    art = screen.load_art(art_path, 220)
    state = State(names, idx, category, album, art, source_rom=rom)
    jb = Jukebox(entries, state, screen=screen)

    evq = queue.Queue()
    stop_evt = threading.Event()
    jt = threading.Thread(target=joystick_thread, args=(evq, stop_evt), daemon=True)
    jt.start()

    jb.start_track()
    screen.render(state)
    last_draw = time.time()
    running = True
    try:
        while running:
            # handle input
            try:
                btn = evq.get(timeout=0.1)
            except queue.Empty:
                btn = None
            if btn is not None:
                if btn == bmap["B"]:
                    running = False
                elif btn == bmap["A"]:
                    jb.cycle_loop()
                elif btn == bmap["SELECT"]:
                    state.play_index = (state.play_index + 1) % len(PLAY_MODES)
                elif btn == bmap["L"]:
                    jb.prev_track()
                elif btn == bmap["R"]:
                    jb.next_track(wrap_exits=False)
                screen.render(state)
                last_draw = time.time()

            # track finished on its own?
            if jb.track_ended_naturally():
                state.playing = False
                mode = state.play_mode
                if mode == "SINGLE":
                    running = False
                elif jb.next_track(wrap_exits=True):
                    pass                      # advanced within this album
                elif mode == "ALL":
                    if not jb.advance_album(shuffle=False):
                        running = False       # end of the sequential scope
                elif mode == "SHUFFLE":
                    if not jb.advance_album(shuffle=True):
                        running = False
                else:                         # ALBUM: stop at album end
                    running = False
                screen.render(state)
                last_draw = time.time()

            # periodic redraw for the clock
            if time.time() - last_draw > 0.5:
                screen.render(state)
                last_draw = time.time()
    finally:
        stop_evt.set()
        jb.stop_proc()
        try:
            screen.clear()
        except Exception:
            pass
        sys.stdout.write("\033[?25h\033[2J")
        sys.stdout.flush()
        os.system("stty sane 2>/dev/null")
    return 0


if __name__ == "__main__":
    sys.exit(main())
