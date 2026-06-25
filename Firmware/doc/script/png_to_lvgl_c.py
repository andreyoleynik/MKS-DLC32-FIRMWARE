#!/usr/bin/env python3
"""Convert PNG image to LVGL C header file (RGB565, LV_COLOR_16_SWAP=0).

Output format matches the style used in Grbl_Esp32/src/lv_pic/*.c
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from datetime import datetime
from pathlib import Path


def _get_symbol_name(path: Path, suffix: str | None = None) -> str:
    """Extract a valid C symbol name from filename."""
    name = path.stem
    if suffix:
        name = f"{name}{suffix}"
    name = re.sub(r"[^a-zA-Z0-9_]", "_", name)
    name = re.sub(r"^[0-9]", "_", name)  # C identifiers can't start with digit
    return name


def _rgb888_to_rgb565(rgb: bytes, w: int, h: int) -> bytes:
    """Convert RGB888 to RGB565 (LV_COLOR_16_SWAP=0), little-endian."""
    if len(rgb) < w * h * 3:
        raise ValueError(f"Not enough bytes: got {len(rgb)}, need {w * h * 3}")

    out = bytearray()
    for i in range(0, len(rgb), 3):
        r8 = rgb[i]
        g8 = rgb[i + 1]
        b8 = rgb[i + 2]

        r5 = (r8 >> 3) & 0x1F
        g6 = (g8 >> 2) & 0x3F
        b5 = (b8 >> 3) & 0x1F

        v = (r5 << 11) | (g6 << 5) | b5
        out.append(v & 0xFF)
        out.append((v >> 8) & 0xFF)

    return bytes(out)


def _load_png(path: Path) -> tuple[bytes, int, int]:
    """Load PNG and return (RGBA data, width, height)."""
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError(
            "Pillow is not installed. Install with: pip install pillow"
        ) from exc

    img = Image.open(path)
    if img.mode != "RGBA":
        img = img.convert("RGBA")

    w, h = img.size
    rgb = bytes(img.convert("RGB").tobytes())
    return rgb, w, h


def _generate_c_file(
    rgb: bytes, w: int, h: int, symbol: str, map_symbol: str
) -> str:
    """Generate C file content matching LVGL project style."""

    rgb565 = _rgb888_to_rgb565(rgb, w, h)

    lines = ["/*", "*---------------------------------------------------------------"]
    lines.append("*                        Lvgl Img Tool")
    lines.append("*")
    lines.append(f"* Generated: {datetime.now().isoformat()}")
    lines.append("*---------------------------------------------------------------")
    lines.append("*/")
    lines.append("")
    lines.append("")
    lines.append("#include \"lvgl.h\"")
    lines.append("")
    lines.append("#ifndef LV_ATTRIBUTE_MEM_ALIGN")
    lines.append("#define LV_ATTRIBUTE_MEM_ALIGN")
    lines.append("#endif")
    lines.append("")
    lines.append("")
    lines.append(f"const LV_ATTRIBUTE_MEM_ALIGN uint8_t {map_symbol}[] = {{")
    lines.append("#if LV_COLOR_DEPTH == 1 || LV_COLOR_DEPTH == 8")
    lines.append(" /*Pixel format: Red: 3 bit, Green: 3 bit, Blue: 2 bit*/")

    # Stub for 8-bit (not used by this project, but include for compatibility)
    lines.append("  // 8-bit format not generated; use 16-bit")
    lines.append("#endif")

    lines.append("#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0")
    lines.append(
        " /*Pixel format: Red: 5 bit, Green: 6 bit, Blue: 5 bit (RGB565)*/"
    )

    # Format bytes with 32 bytes per line for readability
    bytes_per_line = 32
    for chunk_start in range(0, len(rgb565), bytes_per_line):
        chunk = rgb565[chunk_start : chunk_start + bytes_per_line]
        hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"  {hex_str},")

    lines.append("#endif")
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_img_dsc_t {symbol} = {{")
    lines.append("    .header.always_zero = 0,")
    lines.append(f"    .header.w = {w},")
    lines.append(f"    .header.h = {h},")
    lines.append(f"    .data_size = {w * h * 2} * LV_COLOR_SIZE / 8,")
    lines.append("    .header.cf = LV_IMG_CF_TRUE_COLOR,")
    lines.append(f"    .data = {map_symbol},")
    lines.append("};")
    lines.append("")
    lines.append("//end of file")

    return "\n".join(lines)


def main() -> int:
    p = argparse.ArgumentParser(
        description="Convert PNG image to LVGL C file (RGB565, LV_COLOR_16_SWAP=0)"
    )
    p.add_argument("input", type=Path, help="Input PNG file")
    p.add_argument("--output", type=Path, help="Output C file (default: same as input)")
    p.add_argument("--symbol", help="C symbol name (default: auto from filename)")
    p.add_argument(
        "--suffix", help="Suffix for symbol name, e.g. '_pre' for pressed state"
    )

    args = p.parse_args()

    if not args.input.exists():
        print(f"ERROR: Input file not found: {args.input}", file=sys.stderr)
        return 1

    rgb, w, h = _load_png(args.input)

    symbol = args.symbol or _get_symbol_name(args.input, args.suffix)
    map_symbol = f"{symbol}_map"

    c_content = _generate_c_file(rgb, w, h, symbol, map_symbol)

    out = args.output
    if out is None:
        out = args.input.with_suffix(".c")

    out.write_text(c_content, encoding="utf-8")
    print(f"OK: {args.input} ({w}x{h}) -> {out}")
    print(f"   Symbol: {symbol}")
    print(f"   Map: {map_symbol}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        raise SystemExit(1)
