#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pokémon Pokopia 2.0.1: redraws the Switch button glyphs of the game's prompt atlas as the keys
and mouse buttons of mods/.../Keyboard and mouse/Pokopia keyboard and mouse.ini.

The atlas is the 1024x1024 BC7 texture abafc68a5cb224c9 of an Eglantine texture dump
(EDEN_TEXTURE_DUMP, see README.md). Each glyph is cleared and redrawn in the game's style (a white
cap, a 75% dark outline, dark lettering), at the glyph's own place and size.

The title screen's "PRESS (A) BUTTON" is part of the title logo texture (eef4c6cb94636c58): its
A disc becomes an E key, cut out of a white cap like the original.

  python -I pokopia_kbm_glyphs.py <dump folder> <out folder> [--font <bold .ttf>]
then pack both PNGs it writes:
  python -I texture_mod_tool.py pack <dump folder>/<name>.json <out folder>/<name>.png \
      "<load folder>/0400A3A01EE4C000/Keyboard and mouse glyphs"
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ATLAS = "abafc68a5cb224c9_1024x1024"
TITLE = "eef4c6cb94636c58_1024x1024"
TITLE_DISC = (262, 631, 309, 678)  # the A of "PRESS (A) BUTTON"
DARK = (51, 51, 51)
RING_ALPHA = 191
SS = 4  # supersampling

# The glyph boxes in the atlas (x0, y0, x1, y1), found from its alpha, and what each becomes.
GLYPHS = {
    "A": ((249, 345, 315, 411), ("key", "E")),
    "B": ((937, 417, 1003, 483), ("key", "SHIFT")),
    "X": ((713, 605, 779, 671), ("key", "TAB")),
    "Y": ((9, 609, 75, 675), ("key", "F")),
    "L": ((271, 520, 341, 580), ("key", "Q")),
    "R": ((535, 524, 605, 584), ("key", "SPACE")),
    "ZL": ((95, 612, 165, 672), ("mouse", "right")),
    "ZR": ((183, 612, 253, 672), ("mouse", "left")),
    "Plus": ((9, 521, 75, 587), ("key", "M")),
    "Minus": ((777, 517, 843, 583), ("key", "C")),
    "DLeft": ((361, 517, 427, 583), ("key", "3")),
    "DRight": ((625, 521, 691, 587), ("key", "4")),
    "DUp": ((449, 605, 515, 671), ("key", "1")),
    "DDown": ((425, 429, 491, 495), ("key", "2")),
    "LStick": ((449, 517, 515, 583), ("wasd", None)),
    "RStick": ((865, 593, 931, 659), ("mouse", "move")),
    "LStickPress": ((97, 513, 163, 587), ("key", "CTRL")),
    "RStickPress": ((185, 513, 251, 587), ("key", "V")),
}


def font(path: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(path, size)


def fit_text(draw, text, font_path, max_w, max_h):
    size = max_h
    while size > 6:
        f = font(font_path, size)
        l, t, r, b = draw.textbbox((0, 0), text, font=f)
        if r - l <= max_w and b - t <= max_h:
            return f, (l, t, r, b)
        size -= 1
    f = font(font_path, 6)
    return f, draw.textbbox((0, 0), text, font=f)


def cap(w, h):
    """A key cap filling w x h: outline ring, white body (supersampled)."""
    W, H = w * SS, h * SS
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    ring = 5 * SS
    radius = min(W, H) // 4
    d.rounded_rectangle((0, 0, W - 1, H - 1), radius=radius, fill=DARK + (RING_ALPHA,))
    d.rounded_rectangle((ring, ring, W - 1 - ring, H - 1 - ring), radius=max(1, radius - ring),
                        fill=(255, 255, 255, 255))
    return img, d, ring


def centered(d, text, font_path, box):
    x0, y0, x1, y1 = box
    f, (l, t, r, b) = fit_text(d, text, font_path, x1 - x0, y1 - y0)
    d.text((x0 + (x1 - x0 - (r - l)) / 2 - l, y0 + (y1 - y0 - (b - t)) / 2 - t), text, font=f,
           fill=DARK + (255,))


def draw_key(w, h, label, font_path):
    img, d, ring = cap(w, h)
    W, H = img.size
    pad = ring + (3 if len(label) == 1 else 2) * SS
    # single characters large, words fitted to the width
    box = (pad, pad + (2 * SS if len(label) == 1 else H // 6),
           W - pad, H - pad - (2 * SS if len(label) == 1 else H // 6))
    centered(d, label, font_path, box)
    return img


def draw_wasd(w, h, font_path):
    img, d, ring = cap(w, h)
    W, H = img.size
    pad = ring + 3 * SS
    mid = H // 2
    centered(d, "W", font_path, (pad, pad, W - pad, mid - SS))
    centered(d, "ASD", font_path, (pad, mid + SS, W - pad, H - pad))
    return img


def draw_mouse(w, h, which):
    W, H = w * SS, h * SS
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    ring = 5 * SS
    mw = int(min(W, H) * (0.62 if which == "move" else 0.8))
    mh = int(H * 0.98)
    x0 = (W - mw) // 2
    y0 = (H - mh) // 2
    body = (x0, y0, x0 + mw - 1, y0 + mh - 1)
    d.rounded_rectangle(body, radius=mw // 2, fill=DARK + (RING_ALPHA,))
    inner = (body[0] + ring, body[1] + ring, body[2] - ring, body[3] - ring)
    d.rounded_rectangle(inner, radius=mw // 2 - ring, fill=(255, 255, 255, 255))
    split_y = inner[1] + (inner[3] - inner[1]) * 42 // 100
    cx = (inner[0] + inner[2]) // 2
    line = max(SS, 2 * SS)
    # the pressed button filled dark, clipped to the body
    if which in ("left", "right"):
        mask = Image.new("L", img.size, 0)
        md = ImageDraw.Draw(mask)
        md.rounded_rectangle(inner, radius=mw // 2 - ring, fill=255)
        half = (inner[0], inner[1], cx, split_y) if which == "left" else (cx, inner[1], inner[2], split_y)
        fill = Image.new("RGBA", img.size, DARK + (255,))
        clip = Image.new("L", img.size, 0)
        ImageDraw.Draw(clip).rectangle(half, fill=255)
        img.paste(fill, (0, 0), Image.fromarray(np.minimum(np.asarray(mask), np.asarray(clip))))
        d = ImageDraw.Draw(img)
    d.line((inner[0], split_y, inner[2], split_y), fill=DARK + (255,), width=line)
    d.line((cx, inner[1], cx, split_y), fill=DARK + (255,), width=line)
    if which == "move":
        # small arrows either side: the mouse moves the camera
        a = 7 * SS
        my = (inner[1] + inner[3]) // 2 + 4 * SS
        for sx in (-1, 1):
            tip = cx + sx * (mw // 2 + a + 2 * SS)
            base = tip - sx * a
            d.polygon([(tip, my), (base, my - a), (base, my + a)], fill=DARK + (255,))
    return img


def title_key(pixels, font_path):
    """The title's A disc as an E key: white cap, the letter cut out to the art's translucent
    dark, and the art's soft dark glow around it."""
    from PIL import ImageFilter
    x0, y0, x1, y1 = TITLE_DISC
    m = 10  # the glow's reach
    pixels[y0 - m: y1 + m, x0 - m + 4: x1 + m - 4, 3] = 0
    w, h = x1 - x0, y1 - y0
    W, H = (w + 2 * m) * SS, (h + 2 * m) * SS
    shape = Image.new("L", (W, H), 0)
    sd = ImageDraw.Draw(shape)
    box = (m * SS, m * SS, (m + w) * SS - 1, (m + h) * SS - 1)
    sd.rounded_rectangle(box, radius=w * SS // 4, fill=255)
    glow = shape.filter(ImageFilter.GaussianBlur(4 * SS)).point(lambda v: v * 56 // 255)
    out = Image.new("RGBA", (W, H), (13, 11, 9, 0))
    out.putalpha(glow)
    cap_img = Image.new("RGBA", (W, H), (255, 255, 255, 0))
    cap_img.putalpha(shape)
    out.alpha_composite(cap_img)
    letter = Image.new("L", (W, H), 0)
    ld = ImageDraw.Draw(letter)
    pad = 7 * SS
    f, (l, t, r, b) = fit_text(ld, "E", font_path, w * SS - 2 * pad, h * SS - 2 * pad)
    ld.text((box[0] + (w * SS - (r - l)) / 2 - l, box[1] + (h * SS - (b - t)) / 2 - t), "E", font=f,
            fill=255)
    hole = np.asarray(out).copy()
    lm = np.asarray(letter) > 0
    hole[lm] = (12, 12, 12, 77)
    key = Image.fromarray(hole).resize((w + 2 * m, h + 2 * m), Image.LANCZOS)
    region = Image.fromarray(pixels[y0 - m: y1 + m, x0 - m: x1 + m])
    region.alpha_composite(key)
    pixels[y0 - m: y1 + m, x0 - m: x1 + m] = np.asarray(region)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("out")
    ap.add_argument("--font", default="/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf")
    args = ap.parse_args()

    import json
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent))  # texture_mod_tool.py (-I leaves it out)
    from texture_mod_tool import decode, kind
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    def load(name):
        meta = json.loads((Path(args.dump) / (name + ".json")).read_text())
        data = (Path(args.dump) / (name + ".bin")).read_bytes()
        lv = meta["levels"][0]
        return np.array(decode(kind(meta["format"]), data[: lv["size"]], lv["width"], lv["height"]).convert("RGBA"))

    pixels = load(ATLAS)

    for name, ((x0, y0, x1, y1), (what, arg)) in GLYPHS.items():
        # clear the old glyph with a 2-pixel margin for its soft edge
        pixels[max(0, y0 - 2): y1 + 2, max(0, x0 - 2): x1 + 2, 3] = 0
        w, h = x1 - x0, y1 - y0
        if what == "key":
            glyph = draw_key(w, h, arg, args.font)
        elif what == "wasd":
            glyph = draw_wasd(w, h, args.font)
        else:
            glyph = draw_mouse(w, h, arg)
        glyph = glyph.resize((w, h), Image.LANCZOS)
        region = Image.fromarray(pixels[y0:y1, x0:x1])
        region.alpha_composite(glyph)
        pixels[y0:y1, x0:x1] = np.asarray(region)
    Image.fromarray(pixels).save(out / (ATLAS + ".png"))
    print(f"wrote {out / (ATLAS + '.png')}: {len(GLYPHS)} glyphs redrawn")
    if (Path(args.dump) / (TITLE + ".json")).exists():
        title = load(TITLE)
        title_key(title, args.font)
        Image.fromarray(title).save(out / (TITLE + ".png"))
        print(f"wrote {out / (TITLE + '.png')}: PRESS (E) BUTTON")


if __name__ == "__main__":
    main()
