#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turns Eglantine texture dumps into PNGs and edited PNGs back into texture replacements.

Dumps come from EDEN_TEXTURE_DUMP=<folder> (src/video_core/texture_cache/texture_mods.h): each
texture as <hash>_<w>x<h>.bin (every level and layer, rows of blocks as uploaded) and .json.
A replacement is the same .bin with new contents, in <load>/<title id>/<mod>/textures/.

  python -I texture_mod_tool.py png  <dump folder> <png folder> [--min 256]
      level 0, layer 0 of every dumped texture as <hash>_<w>x<h>.png (textures smaller than
      --min on both sides skipped)
  python -I texture_mod_tool.py pack <dump .json> <edited .png> <mod folder>
      encodes the PNG into every level (smaller levels box-filtered) of layer 0, keeps the other
      layers, and writes <mod folder>/textures/<hash>_<w>x<h>.bin
  python -I texture_mod_tool.py icons <prompt_icons.pak> <png folder>
      the keyboard and mouse icons of Recompia's prompt pack (Kenney Input Prompts, CC0) as PNGs

Needs: pip install texture2ddecoder etcpak pillow numpy
"""
import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

BLOCK_BYTES = {"BC1": 8, "BC4": 8, "BC2": 16, "BC3": 16, "BC5": 16, "BC7": 16}


def kind(fmt: str) -> str:
    for k in BLOCK_BYTES:
        if fmt.startswith(k + "_"):
            return k
    if fmt.startswith("A8B8G8R8"):
        return "RGBA8"
    return ""


def decode(k: str, data: bytes, w: int, h: int) -> Image.Image:
    import texture2ddecoder as t
    if k == "RGBA8":
        return Image.frombytes("RGBA", (w, h), data[: w * h * 4])
    fn = {"BC1": t.decode_bc1, "BC3": t.decode_bc3, "BC4": t.decode_bc4, "BC5": t.decode_bc5,
          "BC7": t.decode_bc7}[k]
    bw, bh = (w + 3) // 4 * 4, (h + 3) // 4 * 4
    return Image.frombytes("RGBA", (bw, bh), fn(data, bw, bh), "raw", "BGRA").crop((0, 0, w, h))


def encode(k: str, img: Image.Image) -> bytes:
    import etcpak
    w, h = img.size
    if k == "RGBA8":
        return img.convert("RGBA").tobytes()
    bw, bh = (w + 3) // 4 * 4, (h + 3) // 4 * 4
    canvas = Image.new("RGBA", (bw, bh))
    canvas.paste(img.convert("RGBA"), (0, 0))
    rgba = np.asarray(canvas).tobytes()
    fn = {"BC1": etcpak.compress_bc1, "BC3": etcpak.compress_bc3, "BC4": etcpak.compress_bc4,
          "BC5": etcpak.compress_bc5}.get(k)
    if k == "BC7":
        return etcpak.compress_bc7(rgba, bw, bh, None)
    if fn is None:
        raise SystemExit(f"cannot encode {k}")
    return fn(rgba, bw, bh)


def layer_size(meta: dict, level: dict) -> int:
    return level["size"] // max(1, meta["layers"])


def cmd_png(args) -> None:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    n = 0
    for js in sorted(Path(args.dump).glob("*.json")):
        meta = json.loads(js.read_text())
        k = kind(meta["format"])
        if not k or (meta["width"] < args.min and meta["height"] < args.min):
            continue
        data = js.with_suffix(".bin").read_bytes()
        lv = meta["levels"][0]
        level0 = data[lv["offset"]: lv["offset"] + layer_size(meta, lv)]
        try:
            decode(k, level0, lv["width"], lv["height"]).save(out / (js.stem + ".png"))
            n += 1
        except Exception as e:  # noqa: BLE001 - one bad dump must not stop the rest
            print(f"{js.name}: {e}", file=sys.stderr)
    print(f"{n} PNGs in {out}")


def cmd_pack(args) -> None:
    js = Path(args.json)
    meta = json.loads(js.read_text())
    k = kind(meta["format"])
    if not k:
        raise SystemExit(f"format {meta['format']} is not supported")
    data = bytearray(js.with_suffix(".bin").read_bytes())
    img = Image.open(args.png).convert("RGBA")
    if img.size != (meta["width"], meta["height"]):
        raise SystemExit(f"the PNG is {img.size}, the texture {meta['width']}x{meta['height']}")
    for lv in meta["levels"]:
        size = layer_size(meta, lv)
        level_img = img.resize((lv["width"], lv["height"]), Image.BOX) if lv is not meta["levels"][0] else img
        blocks = encode(k, level_img)
        if len(blocks) != size:
            raise SystemExit(f"level {lv['width']}x{lv['height']}: encoded {len(blocks)} bytes, need {size}")
        data[lv["offset"]: lv["offset"] + size] = blocks
    dest = Path(args.mod) / "textures"
    dest.mkdir(parents=True, exist_ok=True)
    (dest / (js.stem + ".bin")).write_bytes(bytes(data))
    print(f"wrote {dest / (js.stem + '.bin')} ({len(data)} bytes, {len(meta['levels'])} levels)")


def cmd_icons(args) -> None:
    data = Path(args.pak).read_bytes()
    magic, _ver, count, w, h, _ = struct.unpack_from("<4sIIIII", data, 0)
    if magic != b"MPIP":
        raise SystemExit("not an MPIP pack")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    n = 0
    for i in range(count):
        raw, off, size = struct.unpack_from("<40sII", data, 24 + 48 * i)
        name = raw.split(b"\0", 1)[0].decode()
        if not name.startswith("kbm/") or name.endswith("_pressed"):
            continue
        # SDL key names ("left shift", "mouse1", "#"): spaces to _, other symbols by their code
        safe = "".join(c if c.isalnum() or c == "_" else ("_" if c == " " else f"x{ord(c):02x}")
                       for c in name[4:])
        Image.frombytes("RGBA", (w, h), data[off: off + size]).save(out / (safe + ".png"))
        n += 1
    print(f"{n} keyboard and mouse icons in {out}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("png")
    p.add_argument("dump")
    p.add_argument("out")
    p.add_argument("--min", type=int, default=256)
    p.set_defaults(fn=cmd_png)
    p = sub.add_parser("pack")
    p.add_argument("json")
    p.add_argument("png")
    p.add_argument("mod")
    p.set_defaults(fn=cmd_pack)
    p = sub.add_parser("icons")
    p.add_argument("pak")
    p.add_argument("out")
    p.set_defaults(fn=cmd_icons)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
