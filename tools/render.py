# -*- coding: utf-8 -*-
"""152x152 square e-paper layout preview, pixel-faithful to firmware rendering.

Replicates exactly what epd.c draws with OneBitDisplay (obdWriteStringCustom):
 - GFXfont glyphs: top-left = (x + xOffset, baseline + yOffset), MSB-first
   bitmaps packed continuously across rows, x += xAdvance per char.
 - color 1 -> black ink, canvas fill 0 -> white.
 - model 3 glass: canvas is 152 wide x 160 tall, glass shows canvas rows
   SRC_OFFSET(8)..159, i.e. all y below are glass coords; +8 applied here.

Usage: python tools/render.py   (needs Pillow; font data is parsed from the
firmware sources, so run from a fresh checkout without compiling anything)
"""
import re, os, sys
from PIL import Image, ImageDraw, ImageFont

SRC = os.path.join(os.path.dirname(__file__), "..", "ATC_TLSR_Paper", "Firmware", "src")
OUT = os.path.dirname(os.path.abspath(__file__))
W, H, YOFF = 152, 152, 8  # glass size and source-axis offset

def parse_font(stem, name):
    txt = open(os.path.join(SRC, stem + ".h"), encoding="utf-8", errors="ignore").read()
    bm = re.search(re.escape(name) + r"Bitmaps\[\][^=]*=\s*\{(.*?)\};", txt, re.S).group(1)
    bitmaps = bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", bm))
    gl = re.search(re.escape(name) + r"Glyphs\[\][^=]*=\s*\{(.*?)\};", txt, re.S).group(1)
    glyphs = [tuple(int(x) for x in m.groups())
              for m in re.finditer(r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}", gl)]
    return {"bitmaps": bitmaps, "glyphs": glyphs}

F = {
    "d16":  parse_font("font16", "Dialog_plain_16"),
    "se30": parse_font("font30", "Special_Elite_Regular_30"),
    "dseg": parse_font("font_60", "DSEG14_Classic_Mini_Regular_40"),
    "rb80": parse_font("Roboto_Black_80", "Roboto_Black_80"),
}

def glyph(font, ch):
    o = ord(ch) - 0x20
    return font["glyphs"][o] if 0 <= o < len(font["glyphs"]) else None

def string_box(font, s):
    """like obdGetStringBox: (advance_width, min_yOffset, max_yOffset+height) rel. baseline"""
    cx, miny, maxy = 0, 999, -999
    for ch in s:
        g = glyph(font, ch)
        if not g: continue
        off, w, h, adv, xo, yo = g
        cx += adv
        miny = min(miny, yo); maxy = max(maxy, yo + h)
    return cx, miny, maxy

class Glass:
    def __init__(self):
        self.px = set()          # glass coords (x, y), y in 0..151
        self.clipped = 0
        self.boxes = []          # (name, x1, y1, x2, y2) for overlap check

    def _set(self, x, y):
        if 0 <= x < W and 0 <= y < H:
            self.px.add((x, y))
        else:
            self.clipped += 1

    def box(self, name, x, y, w, h):
        self.boxes.append((name, x, y, x + w - 1, y + h - 1))

    def check(self, title):
        for i in range(len(self.boxes)):
            for j in range(i + 1, len(self.boxes)):
                (n1, a1, b1, a2, b2), (n2, c1, d1, c2, d2) = self.boxes[i], self.boxes[j]
                if a1 <= c2 and c1 <= a2 and b1 <= d2 and d1 <= b2:
                    print(f"  !! OVERLAP in {title}: {n1} {self.boxes[i][1:]} vs {n2} {self.boxes[j][1:]}")

    def text(self, fname, x, baseline, s):
        """obdWriteStringCustom semantics; x = left edge, baseline = y"""
        font = F[fname]
        for ch in s:
            g = glyph(font, ch)
            if g:
                off, w, h, adv, xo, yo = g
                dx, dy = x + xo, baseline + yo
                for py in range(h):
                    for pxi in range(w):
                        bit = off * 8 + py * w + pxi
                        if font["bitmaps"][bit >> 3] & (0x80 >> (bit & 7)):
                            self._set(dx + pxi, dy + py)
                x += adv
            else:
                x += 6  # undefined char skipped (cursor unmoved in fw, keep simple)

    def hline(self, x1, x2, y):
        for x in range(min(x1, x2), max(x1, x2) + 1):
            self._set(x, y)

    def vline(self, y1, y2, x):
        for y in range(min(y1, y2), max(y1, y2) + 1):
            self._set(x, y)

    def rect(self, x1, y1, x2, y2):
        self.hline(x1, x2, y1); self.hline(x1, x2, y2)
        self.vline(y1, y2, x1); self.vline(y1, y2, x2)

    def fillrect(self, x1, y1, x2, y2):
        for y in range(y1, y2 + 1):
            for x in range(x1, x2 + 1):
                self._set(x, y)

# ---- layout helpers (same math the firmware will do with obdGetStringBox) ----
def centered(g, fname, s, baseline, cx=W // 2):
    w, _, _ = string_box(F[fname], s)
    g.text(fname, cx - w // 2, baseline, s)

def right_align(g, fname, s, baseline, x_end):
    w, _, _ = string_box(F[fname], s)
    g.text(fname, x_end - w, baseline, s)

# ---- shared sample data ----
MAC = "ESL_A1B2C3"; MODEL = "BWR154"; TIME = "12:34"; TEMP = "23'C"; BATT = "2.98V"; BLE = "B"

def ble_dot(g, x, y, size=8):
    g.fillrect(x, y, x + size - 1, y + size - 1)
    g.box("ble", x, y, size, size)

def text_boxed(g, fname, x, base, s, name):
    w, top, bot = string_box(F[fname], s)
    g.text(fname, x, base, s)
    g.box(name, x, base + top, w, bot - top)

def centered_boxed(g, fname, s, base, name, cx=W // 2):
    w, _, _ = string_box(F[fname], s)
    text_boxed(g, fname, cx - w // 2, base, s, name)

def right_align_boxed(g, fname, s, base, x_end, name):
    w, _, _ = string_box(F[fname], s)
    text_boxed(g, fname, x_end - w, base, s, name)

# ---------------- L0: current firmware layout (faithful reproduction) ----------------
def layout_current():
    g = Glass()
    g.text("d16", 1, 17, MAC + " " + MODEL)
    g.text("d16", 132, 20, BLE)
    g.text("dseg", 50, 65, TIME)
    g.text("se30", 10, 95, TEMP)
    g.text("d16", 10, 120, "Battery 2985mV")
    return g

# ---------------- L1: dashboard — header / hero clock / footer + dividers ----------------
def layout_dashboard():
    g = Glass()
    text_boxed(g, "d16", 7, 20, MAC, "mac")
    ble_dot(g, 138, 10)
    g.hline(6, 146, 30)
    w, top, bot = string_box(F["dseg"], TIME)
    base = 69 - (top + bot) // 2          # optically centered in band 34..104
    text_boxed(g, "dseg", (W - w) // 2, base, TIME, "time")
    g.hline(6, 146, 104)
    text_boxed(g, "se30", 12, 128, TEMP, "temp")
    right_align_boxed(g, "d16", BATT, 128, 140, "batt")
    centered_boxed(g, "d16", MODEL, 146, "model")
    return g

def ink_box(font, s):
    """ink (bitmap) left/right extent relative to pen origin, for optical centering"""
    pen, l, r = 0, 10**9, -10**9
    for ch in s:
        g = glyph(font, ch)
        if g:
            off, w, h, adv, xo, yo = g
            if w:
                l = min(l, pen + xo); r = max(r, pen + xo + w)
            pen += adv
    return l, r

def ink_centered(g, fname, s, base, name):
    """place string so its INK box is centered on the glass"""
    font = F[fname]
    l, r = ink_box(font, s)
    text_boxed(g, fname, 76 - (l + r) // 2, base, s, name)

def ink_right(g, fname, s, base, x_end, name):
    """place string so its INK right edge lands on x_end"""
    font = F[fname]
    l, r = ink_box(font, s)
    text_boxed(g, fname, x_end - r, base, s, name)

# ---------------- L1v2: dashboard final — vision-review fixes applied ----------------
def layout_dashboard_v2():
    g = Glass()
    text_boxed(g, "d16", 8, 20, MAC, "mac")
    ble_dot(g, 136, 10)
    g.hline(8, 143, 30)
    ink_centered(g, "dseg", TIME, 84, "time")     # ink-box centered, band 32..96
    g.hline(8, 143, 96)
    text_boxed(g, "se30", 8, 124, TEMP, "temp")
    ink_right(g, "d16", BATT, 124, 143, "batt")
    ink_centered(g, "d16", MODEL, 144, "model")
    return g

# ---------------- L2: minimal airy — no rules, three quiet zones ----------------
def layout_minimal():
    g = Glass()
    text_boxed(g, "d16", 8, 18, MAC, "mac")
    ble_dot(g, 139, 8)
    w, top, bot = string_box(F["dseg"], TIME)
    base = 76 - (top + bot) // 2          # optically centered on the glass
    text_boxed(g, "dseg", (W - w) // 2, base, TIME, "time")
    text_boxed(g, "se30", 12, 128, TEMP, "temp")
    right_align_boxed(g, "d16", BATT, 128, 144, "batt")
    centered_boxed(g, "d16", MODEL, 146, "model")
    return g

# ---------------- L3: corner brackets — square-screen corner ticks ----------------
def layout_card():
    g = Glass()
    L, T = 16, 2              # arm length, thickness, inset 4
    for (cx, cy, sx, sy) in [(4, 4, 1, 1), (147, 4, -1, 1), (4, 147, 1, -1), (147, 147, -1, -1)]:
        g.fillrect(min(cx, cx + sx * (L - 1)), min(cy, cy + sy * (T - 1)),
                   max(cx, cx + sx * (L - 1)), max(cy, cy + sy * (L - 1)))
        g.fillrect(min(cx, cx + sx * (T - 1)), min(cy, cy + sy * (L - 1)),
                   max(cx, cx + sx * (T - 1)), max(cy, cy + sy * (L - 1)))
        g.box("bracket", min(cx, cx + sx * (L - 1)), min(cy, cy + sy * (L - 1)), L, L)
    text_boxed(g, "d16", 24, 22, MAC[4:], "mac")   # short id, brackets eat the corners
    ble_dot(g, 116, 14)
    w, top, bot = string_box(F["dseg"], TIME)
    base = 80 - (top + bot) // 2          # optically centered on the glass
    text_boxed(g, "dseg", (W - w) // 2, base, TIME, "time")
    text_boxed(g, "se30", 12, 128, TEMP, "temp")
    right_align_boxed(g, "d16", BATT, 128, 140, "batt")
    return g

# ---------------- render to PNG ----------------
def render(g, title, fname, scale=4):
    S = scale
    img = Image.new("L", (W * S + 48, H * S + 64), 0xB8BCBE)  # bezel gray
    d = ImageDraw.Draw(img)
    d.rectangle([24 - 6, 28 - 6, 24 + W * S + 5, 28 + H * S + 5], fill=0x50545A)  # shell
    d.rectangle([24, 28, 24 + W * S - 1, 28 + H * S - 1], fill=255)               # glass
    for (x, y) in g.px:
        d.rectangle([24 + x * S, 28 + y * S, 24 + x * S + S - 1, 28 + y * S + S - 1], fill=0)
    try:
        fnt = ImageFont.truetype("consola.ttf", 15)
    except Exception:
        fnt = ImageFont.load_default()
    d.text((24, 6), title + ("   [CLIPPED %d px]" % g.clipped if g.clipped else ""), font=fnt, fill=0x202020)
    img.save(os.path.join(OUT, fname))
    print(f"{fname}: {title}, clipped={g.clipped}")

if __name__ == "__main__":
    layouts = [
        (layout_current,      "L0 current (as in v5 firmware)", "preview_L0_current.png"),
        (layout_dashboard_v2, "L1v2 dashboard final",           "preview_L1v2_final.png"),
        (layout_minimal,      "L2 minimal (airy)",              "preview_L2_minimal.png"),
        (layout_card,         "L3 corner brackets",             "preview_L3_brackets.png"),
    ]
    for fn, title, fname in layouts:
        g = fn()
        g.check(title)
        render(g, title, fname)
    # metrics dump for firmware constants
    for k, s in [("d16", MAC), ("d16", MODEL), ("dseg", TIME), ("se30", TEMP), ("d16", BATT)]:
        print(k, repr(s), string_box(F[k], s))
