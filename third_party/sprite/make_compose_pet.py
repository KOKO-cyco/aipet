#!/usr/bin/env python3
"""绿幕立绘 -> 合影用的 RGB565A8 常量位图，生成 source/embedded/src/modules/app_cam_pet.c

    python third_party/sprite/make_compose_pet.py third_party/my_pet/右侧半身比耶.png --height 180

为什么要"处理一下"
------------------
源图是**绿幕底、完全不透明**的 PNG（alpha 全 255）。直接贴进照片会带一块绿板，
所以这里做三件事：

1. **抠像**：按"绿通道比 R/B 高出多少"算 alpha，边缘做软过渡（硬阈值会有锯齿）；
2. **去绿边**：半透明像素的 G 压到 max(R,B)，否则边缘一圈绿；
3. **缩放**：先预乘 alpha 再 LANCZOS 缩放、再还原，避免透明区（绿色）渗到边缘。

输出格式
--------
RGB565A8：前 W*H*2 字节是 RGB565，后 W*H 字节是 alpha。拍照时由 app_camera
逐像素混合进 BMP —— 所以合影是**真透明叠加**，不是贴一块方图。

尺寸按 `--height` 定（默认 180，照片是 240x320），宽度按原图比例算。
"""

import argparse
import hashlib
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow：pip install pillow")

OUT_C = Path(__file__).resolve().parent.parent.parent / "source/embedded/src/modules/app_cam_pet.c"

# 抠像阈值：greenness = G - max(R,B)。绿幕实测约 200，角色身上最高的绿也就 40 左右。
THR_LO = 40.0
THR_HI = 90.0

TEMPLATE = """/**
 * @file app_cam_pet.c
 * @brief 合影用的角色立绘 —— **本文件由 third_party/sprite/make_compose_pet.py 生成**。
 *
 * 源图：{src}
 * 源图 md5：{srcmd5}
 * 尺寸：{w}x{h}（原图 {ow}x{oh}，抠像后裁切）
 * 格式：RGB565A8 —— 前 W*H*2 字节 RGB565，后 W*H 字节 alpha
 * 字节数：{nbytes}（在 flash 里）
 *
 * **换了源图要重新跑一次生成**（命令见 third_party/sprite/make_compose_pet.py）。别手改。
 */

#include "app_cam_pet.h"

#define PET_W {w}
#define PET_H {h}

static const uint8_t sg_pet_px[PET_W * PET_H * 3] = {{
{data}
}};

const APP_CAM_PET_T *app_cam_pet_get(void)
{{
    static const APP_CAM_PET_T t = {{
        .data = sg_pet_px,
        .w    = PET_W,
        .h    = PET_H,
    }};

    return &t;
}}
"""


def key_alpha(im):
    """按 greenness 算 alpha，附带去绿边。返回 RGBA。"""
    im = im.convert("RGBA")
    w, h = im.size
    px = im.load()
    span = THR_HI - THR_LO

    for y in range(h):
        for x in range(w):
            r, g, b, _ = px[x, y]
            green = g - max(r, b)

            if green <= THR_LO:
                a = 255
            elif green >= THR_HI:
                a = 0
            else:
                a = int(255.0 * (THR_HI - green) / span)

            # 去绿边：半透明的边缘像素把 G 压到 max(R,B)，不然缩完一圈绿。
            if a < 255:
                g = min(g, max(r, b))

            px[x, y] = (r, g, b, a)

    return im


def crop_alpha(im, margin=2):
    bbox = im.getchannel("A").getbbox()
    if bbox is None:
        sys.exit("抠完什么都不剩 —— 阈值不对？")
    x0, y0, x1, y1 = bbox
    w, h = im.size
    return im.crop((max(0, x0 - margin), max(0, y0 - margin),
                    min(w, x1 + margin), min(h, y1 + margin)))


def resize_premultiplied(im, height):
    from PIL import ImageChops

    w, h = im.size
    width = max(1, round(w * height / h))

    # 预乘 -> 缩放 -> 还原：不这么做的话，Lanczos 会把透明像素的 RGB 混进边缘。
    r, g, b, a = im.split()
    r = ImageChops.multiply(r, a)
    g = ImageChops.multiply(g, a)
    b = ImageChops.multiply(b, a)

    pm = Image.merge("RGBA", (r, g, b, a)).resize((width, height), Image.LANCZOS)

    src = pm.load()
    out = Image.new("RGBA", pm.size)
    dst = out.load()
    for y in range(height):
        for x in range(width):
            pr, pg, pb, pa = src[x, y]
            if pa == 0:
                dst[x, y] = (0, 0, 0, 0)
            elif pa == 255:
                dst[x, y] = (pr, pg, pb, 255)
            else:
                f = 255.0 / pa
                dst[x, y] = (min(255, int(pr * f + 0.5)),
                             min(255, int(pg * f + 0.5)),
                             min(255, int(pb * f + 0.5)), pa)
    return out


def pack(im):
    """RGB565 平面 + alpha 平面，拼成一个字节串。"""
    w, h = im.size
    px = im.load()
    rgb = bytearray(w * h * 2)
    alp = bytearray(w * h)

    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            rgb[i * 2] = v & 0xFF
            rgb[i * 2 + 1] = (v >> 8) & 0xFF
            alp[i] = a
            i += 1

    return bytes(rgb) + bytes(alp), w, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--height", type=int, default=180, help="合影时角色的高度（像素）")
    args = ap.parse_args()

    src = Path(args.src)
    raw = src.read_bytes()
    ow, oh = Image.open(src).size

    im = resize_premultiplied(crop_alpha(key_alpha(Image.open(src))), args.height)
    data, w, h = pack(im)

    lines = []
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join("0x%02X" % v for v in data[i:i + 16]) + ",")

    OUT_C.write_text(TEMPLATE.format(
        src=src.as_posix(), srcmd5=hashlib.md5(raw).hexdigest(),
        w=w, h=h, ow=ow, oh=oh, nbytes=len(data),
        data="\n".join(lines),
    ), encoding="utf-8")

    print(f"{OUT_C}  {w}x{h}  {len(data)} 字节")


if __name__ == "__main__":
    main()
