"""Emit an RGBA PNG as an LVGL v9 RGB565A8 image descriptor.

RGB565A8 is "colour array followed by alpha array" — one contiguous RGB565 plane, then
one A8 plane — which is 3 bytes a pixel and gives the character a real alpha channel
without the 4 bytes an ARGB8888 sprite would cost. Flash is the constraint here, not
decode time: the panel is 16-bit, so 565 is the native depth anyway.
"""
import sys

import numpy as np
from PIL import Image

src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]

img = Image.open(src).convert("RGBA")
w, h = img.size
a = np.asarray(img).astype(np.uint16)

r = (a[..., 0] >> 3) & 0x1F
g = (a[..., 1] >> 2) & 0x3F
b = (a[..., 2] >> 3) & 0x1F
rgb565 = ((r << 11) | (g << 5) | b).astype("<u2")

# Little-endian byte pairs, then the alpha plane.
colour = rgb565.tobytes()
alpha = a[..., 3].astype(np.uint8).tobytes()
data = colour + alpha

guard = name.upper()
lines = []
for i in range(0, len(data), 16):
    lines.append("    " + ", ".join("0x%02X" % v for v in data[i:i + 16]) + ",")
body = "\n".join(lines)

out = f"""/**
 * @file {name}.c
 * @brief The pet character sprite, generated from the reference art.
 *
 * {w}x{h}, LV_COLOR_FORMAT_RGB565A8: a 565 colour plane followed by an A8 alpha plane,
 * {len(data)} bytes in all. Generated, not hand-edited — regenerate with
 * tools/art/prepare.py + tools/art/to_c.py rather than editing a byte here.
 *
 * The alpha channel is real: the source is a screenshot of a whole page, and the card
 * background, the speech bubble, the status bar, the watermark and the sparkles were
 * keyed out of it so only the character is left.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_map[] = {{
{body}
}};

const lv_image_dsc_t {name} = {{
    .header.cf = LV_COLOR_FORMAT_RGB565A8,
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.w = {w},
    .header.h = {h},
    .data_size = {len(data)},
    .data = {name}_map,
}};
"""

with open(dst, "w", encoding="utf-8") as f:
    f.write(out)

print("wrote %s: %dx%d, %d bytes" % (dst, w, h, len(data)))
