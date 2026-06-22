#!/usr/bin/env python3
"""Restore PNG from an LVGL C image file (RGB565, LV_COLOR_16_SWAP=0).

This script is tailored for image files generated like:
- Grbl_Esp32/src/lv_pic/png_*.c
- lv_img_dsc_t with LV_IMG_CF_TRUE_COLOR
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def _scale_5_to_8(v: int) -> int:
    return (v << 3) | (v >> 2)


def _scale_6_to_8(v: int) -> int:
    return (v << 2) | (v >> 4)


def _extract_descriptor(text: str, symbol: str | None) -> tuple[str, int, int, str]:
    if symbol is None:
        m = re.search(r"const\s+lv_img_dsc_t\s+(\w+)\s*=\s*\{", text)
        if not m:
            raise ValueError("Cannot find lv_img_dsc_t descriptor in file")
        symbol = m.group(1)

    desc_re = re.compile(
        rf"const\s+lv_img_dsc_t\s+{re.escape(symbol)}\s*=\s*\{{(?P<body>.*?)\}};",
        re.S,
    )
    m = desc_re.search(text)
    if not m:
        raise ValueError(f"Descriptor '{symbol}' not found")

    body = m.group("body")

    mw = re.search(r"\.header\.w\s*=\s*(\d+)", body)
    mh = re.search(r"\.header\.h\s*=\s*(\d+)", body)
    md = re.search(r"\.data\s*=\s*(\w+)", body)

    if not (mw and mh and md):
        raise ValueError(f"Descriptor '{symbol}' is missing width/height/data fields")

    w = int(mw.group(1))
    h = int(mh.group(1))
    map_symbol = md.group(1)
    return symbol, w, h, map_symbol


def _extract_rgb565_bytes(text: str, map_symbol: str) -> bytes:
    # Keep only the RGB565 no-swap section that matches this firmware config.
    sec = re.search(
        r"#if\s+LV_COLOR_DEPTH\s*==\s*16\s*&&\s*LV_COLOR_16_SWAP\s*==\s*0(?P<body>.*?)#endif",
        text,
        re.S,
    )
    if not sec:
        raise ValueError("Cannot find LV_COLOR_DEPTH==16 && LV_COLOR_16_SWAP==0 section")

    body = sec.group("body")

    # Validate that we are in the expected map declaration context.
    if map_symbol not in text:
        raise ValueError(f"Map symbol '{map_symbol}' not found")

    hex_vals = re.findall(r"0x([0-9a-fA-F]{2})", body)
    if not hex_vals:
        raise ValueError("No byte data found in RGB565 section")

    return bytes(int(v, 16) for v in hex_vals)


def _rgb565_to_rgb888(raw: bytes, w: int, h: int) -> bytes:
    expected = w * h * 2
    if len(raw) < expected:
        raise ValueError(f"Not enough bytes for image: got {len(raw)}, need {expected}")
    if len(raw) > expected:
        raw = raw[:expected]

    out = bytearray(w * h * 3)
    oi = 0
    for i in range(0, len(raw), 2):
        v = raw[i] | (raw[i + 1] << 8)  # LV_COLOR_16_SWAP == 0
        r5 = (v >> 11) & 0x1F
        g6 = (v >> 5) & 0x3F
        b5 = v & 0x1F
        out[oi] = _scale_5_to_8(r5)
        out[oi + 1] = _scale_6_to_8(g6)
        out[oi + 2] = _scale_5_to_8(b5)
        oi += 3
    return bytes(out)


def _save_png(rgb: bytes, w: int, h: int, out_path: Path) -> None:
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError(
            "Pillow is not installed. Install with: pip install pillow"
        ) from exc

    img = Image.frombytes("RGB", (w, h), rgb)
    img.save(out_path, format="PNG")


def main() -> int:
    p = argparse.ArgumentParser(description="Convert LVGL C image back to PNG")
    p.add_argument("input", type=Path, help="Path to LVGL .c image file")
    p.add_argument("output", type=Path, nargs="?", help="Output PNG path")
    p.add_argument(
        "--symbol",
        help="Descriptor symbol name, e.g. png_xyhome_pre (auto-detected if omitted)",
    )

    args = p.parse_args()

    text = args.input.read_text(encoding="utf-8", errors="ignore")
    symbol, w, h, map_symbol = _extract_descriptor(text, args.symbol)
    raw = _extract_rgb565_bytes(text, map_symbol)
    rgb = _rgb565_to_rgb888(raw, w, h)

    out = args.output
    if out is None:
        out = args.input.with_suffix(".png")

    _save_png(rgb, w, h, out)
    print(f"OK: {symbol} -> {out} ({w}x{h})")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        raise SystemExit(1)
