#!/usr/bin/env python3
"""PNG -> LVGL v9 RGB565 常量位图，生成 source/embedded/src/pages/app_boot_logo.c

给开机 splash 用（见 app_boot_logo.h）。为什么是"编进固件"而不是放卡上现读：
它必须在系统还没准备好的时候就能显示，所以只能编进 flash。

    python tools/img_to_lvgl.py third_party/logo.png --max 100 --trim --bg "#9393C2"

--max 是长边上限，超过会等比缩小。**一个像素 2 字节**，200x200 = 78KB flash，
再大就不划算了（app 分区只剩约 800KB）。

关于透明：LVGL 的 RGB565 没有 alpha 通道。源图带透明的话，透明像素会按
--bg 指定的底色（默认黑，跟页面底色一致）合成掉 —— 不做这一步的话，透明区域
会变成源图里那些像素的原始 RGB（通常是白或黑），出现难看的方块边。
"""

import argparse
import hashlib
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow：pip install pillow")

# 生成的 .c 放哪（相对本脚本）
OUT_REL = "../source/embedded/src/pages/app_boot_logo.c"

TEMPLATE = """/**
 * @file app_boot_logo.c
 * @brief 开机 logo 的像素数据 —— **本文件由 tools/img_to_lvgl.py 生成**。
 *
 * 源图：{src}
 * 源图 md5：{srcmd5}
 * 尺寸：{w}x{h}（原图 {ow}x{oh}{trimmed}{scaled}）
 * 格式：LV_COLOR_FORMAT_RGB565，{nbytes} 字节常量（在 flash 里）
 *
 * **换了源图要重新跑一次生成**（命令见 app_boot_logo.h）。对一下上面的 md5 就知道
 * 这份是不是从当前源图来的 —— 光看图是看不出新旧版的。
 *
 * 别手改 —— 重新生成会整个覆盖。
 */

#include "app_boot_logo.h"

/* LVGL 的绘制缓冲要求按行对齐；RGB565 每像素 2 字节，宽度对齐到 2 像素即可，
 * 这里统一给 4 字节对齐（跟 LV_DRAW_BUF_STRIDE_ALIGN 一致）。 */
#define BOOT_LOGO_W        {w}
#define BOOT_LOGO_H        {h}
#define BOOT_LOGO_STRIDE   (BOOT_LOGO_W * 2)

static const uint8_t sg_boot_logo_px[BOOT_LOGO_STRIDE * BOOT_LOGO_H] = {{
{data}
}};

const lv_image_dsc_t *app_boot_logo_dsc(void)
{{
    static const lv_image_dsc_t dsc = {{
        .header = {{
            .magic      = LV_IMAGE_HEADER_MAGIC,
            .cf         = LV_COLOR_FORMAT_RGB565,
            .flags      = 0,
            .w          = BOOT_LOGO_W,
            .h          = BOOT_LOGO_H,
            .stride     = BOOT_LOGO_STRIDE,
            .reserved_2 = 0,
        }},
        .data_size = sizeof(sg_boot_logo_px),
        .data      = sg_boot_logo_px,
        .reserved  = NULL,
    }};

    return &dsc;
}}
"""


def rgb565(im, bg):
    """RGB888 -> RGB565 小端字节流。透明像素先按 bg 合成。"""
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        flat = Image.new("RGBA", im.size, bg + (255,))
        flat.alpha_composite(im)
        im = flat.convert("RGB")
    else:
        im = im.convert("RGB")

    out = bytearray()
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out += bytes((v & 0xFF, v >> 8))       # LVGL 是小端
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png")
    ap.add_argument("--max", type=int, default=200,
                    help="长边上限，超过等比缩小（默认 200）")
    ap.add_argument("--bg", default="#000000",
                    help="透明像素合成用的底色，十六进制（默认 #000000，跟页面底色一致）")
    ap.add_argument("--trim", action="store_true",
                    help="先按 alpha 把四周的透明边裁掉再缩放。"
                         "带透明的素材通常留了一圈空白，不裁的话角色在目标尺寸里会明显偏小")
    ap.add_argument("-o", "--out", default=None)
    args = ap.parse_args()

    src = Path(args.png)
    if not src.is_file():
        sys.exit(f"找不到 {src}")

    srcmd5 = hashlib.md5(src.read_bytes()).hexdigest()
    im = Image.open(src)
    ow, oh = im.size
    trimmed = ""

    # **先裁再缩**：裁在原始分辨率上做，边缘更干净；而且缩放是按"裁完的尺寸"
    # 算的，所以裁完的角色能真正填满 --max 指定的框。
    if args.trim:
        bbox = im.convert("RGBA").getchannel("A").getbbox()
        if bbox and bbox != (0, 0, ow, oh):
            im = im.crop(bbox)
            trimmed = f"，裁掉透明边 {bbox}"

    scaled = ""
    if max(im.size) > args.max:
        k = args.max / float(max(im.size))
        im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))),
                       Image.LANCZOS)
        scaled = f"，已缩到 {args.max}"
    w, h = im.size

    bg = args.bg.lstrip("#")
    if len(bg) != 6:
        sys.exit("--bg 要 6 位十六进制，例如 #101018")
    bg = tuple(int(bg[i:i + 2], 16) for i in (0, 2, 4))

    data = rgb565(im, bg)

    # 每行 12 个字节，够密又不至于一行几万字符
    lines = []
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append("    " + " ".join(f"0x{b:02X}," for b in chunk))

    out = Path(args.out) if args.out else (Path(__file__).resolve().parent / OUT_REL)
    out.write_text(TEMPLATE.format(
        src=src.name, srcmd5=srcmd5, w=w, h=h, ow=ow, oh=oh,
        scaled=scaled, trimmed=trimmed,
        nbytes=len(data), data="\n".join(lines),
    ), encoding="utf-8")

    print(f"{src.name}: {ow}x{oh} -> {w}x{h}, {len(data)} 字节 -> {out}")
    print(f"flash 增量约 {len(data) / 1024:.1f}KB")


if __name__ == "__main__":
    main()
