# LVGL Icon Conversion Guide

Bidirectional PNG ↔ C conversion tools for custom button icons in MKS-DLC32 firmware UI.

## Installation

Install Pillow (one-time):
```bash
pip3 install pillow
```

## Tools

### 1. PNG → C Converter
**File:** `png_to_lvgl_c.py`

Converts PNG image to LVGL C source file with embedded pixel data.

**Usage:**
```bash
python3 doc/script/png_to_lvgl_c.py <input.png> [options]
```

**Options:**
- `--output FILE` – Output C file (default: same name as input PNG)
- `--symbol NAME` – C symbol name (default: auto from filename)
- `--suffix SUFFIX` – Add suffix to symbol (e.g., `_pre` for pressed state)

**Examples:**
```bash
# Simple conversion
python3 doc/script/png_to_lvgl_c.py my_icon.png
# Output: my_icon.c with symbol my_icon_map

# With suffix (pressed state)
python3 doc/script/png_to_lvgl_c.py my_xyhome.png --suffix _pre
# Output: my_xyhome.c with symbol my_xyhome_pre_map

# Custom output and symbol
python3 doc/script/png_to_lvgl_c.py custom.png --output out.c --symbol custom_icon
```

### 2. C → PNG Converter
**File:** `lvgl_c_to_png.py`

Converts LVGL C source file back to PNG for editing.

**Usage:**
```bash
python3 doc/script/lvgl_c_to_png.py <input.c> [output.png]
```

**Examples:**
```bash
# Convert with auto output name
python3 doc/script/lvgl_c_to_png.py Grbl_Esp32/src/lv_pic/png_xyhome.c
# Output: png_xyhome.png (same directory)

# Specify output location
python3 doc/script/lvgl_c_to_png.py Grbl_Esp32/src/lv_pic/png_xyhome.c ~/Desktop/my_icon.png
```

## Icon Specifications

- **Size:** 66 × 52 pixels
- **Format:** RGB/RGBA PNG (color depth doesn't matter; script converts automatically)
- **Color Format:** RGB565 little-endian (LV_COLOR_DEPTH=16, LV_COLOR_16_SWAP=0)
- **Location:** Grbl_Esp32/src/lv_pic/*.c

## Complete Workflow: Custom Icon

### 1. Export Current Icon for Editing
```bash
python3 doc/script/lvgl_c_to_png.py Grbl_Esp32/src/lv_pic/png_xyhome.c
```
Creates `png_xyhome.png` in current directory.

### 2. Edit PNG
- Open `png_xyhome.png` in your image editor (GIMP, Photoshop, etc.)
- Keep dimensions **exactly 66×52**
- Save as PNG

### 3. Convert Back to C
```bash
python3 doc/script/png_to_lvgl_c.py png_xyhome.png --symbol png_xyhome
```
Creates `png_xyhome.c` in current directory.

### 4. Replace in Firmware
```bash
mv png_xyhome.c Grbl_Esp32/src/lv_pic/
```

### 5. Build and Upload
```bash
platformio run -e mks_dlc32_v2_1 -t upload
```

## Icon Types in Control Screen

| Button | Unpressed | Pressed |
|--------|-----------|---------|
| Hard Home XY | `png_xyhome` | `png_xyhome_pre` |
| Hard Home Z | `png_z_home` | `png_z_home_pre` |
| Soft Home XY | `X_POS` | `X_POS` (same as unpressed) |
| Soft Home Z | `Z_POS` | `Z_POS` (same as unpressed) |

## Batch Conversion: Both States

Edit both unpressed and pressed versions:

```bash
# Export both
python3 doc/script/lvgl_c_to_png.py Grbl_Esp32/src/lv_pic/png_xyhome.c
python3 doc/script/lvgl_c_to_png.py Grbl_Esp32/src/lv_pic/png_xyhome_pre.c

# Edit png_xyhome.png and png_xyhome_pre.png in your editor...

# Convert back
python3 doc/script/png_to_lvgl_c.py png_xyhome.png --symbol png_xyhome
python3 doc/script/png_to_lvgl_c.py png_xyhome_pre.png --symbol png_xyhome_pre

# Replace
mv png_xyhome.c Grbl_Esp32/src/lv_pic/
mv png_xyhome_pre.c Grbl_Esp32/src/lv_pic/
```

## Troubleshooting

**Error: "Pillow is not installed"**
```bash
pip3 install pillow
```

**Output PNG is blank or wrong colors**
- Ensure input C file has `LV_COLOR_DEPTH == 16` and `LV_COLOR_16_SWAP == 0` section
- Check file is from `Grbl_Esp32/src/lv_pic/` directory

**Symbol name doesn't match after conversion**
- Use `--symbol` option: `python3 png_to_lvgl_c.py file.png --symbol old_name`

## File Locations

- **Scripts:** `doc/script/`
  - `png_to_lvgl_c.py` – PNG → C converter
  - `lvgl_c_to_png.py` – C → PNG converter
  
- **Icons:** `Grbl_Esp32/src/lv_pic/`
  - Unpressed states: `png_xyhome.c`, `png_z_home.c`, `X_POS.c`, `Z_POS.c`
  - Pressed states: `png_xyhome_pre.c`, `png_z_home_pre.c`

- **UI Integration:** `Grbl_Esp32/src/mks/`
  - `MKS_draw_move.cpp` – Button initialization and handlers
  - `draw_ui.h` – Icon declarations
