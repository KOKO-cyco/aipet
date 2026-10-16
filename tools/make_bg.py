#!/usr/bin/env python3
"""源图 -> 320x480 小端 RGB565 背景（角色页的底图）。

    python tools/make_bg.py <源图.png> <输出.rgb>

做法和 `tools/pack_png_seq.py` 的 `build_background()` **一致**：先按 320:480 的
宽高比居中裁剪，再 LANCZOS 缩到 320x480 —— 直接拉伸会把天空压扁。

产物是**裸 RGB565，没有头**：320*480*2 = 307200 字节，固件按面板几何直接读。

现有的两张是怎么来的
--------------------
    bg_afternoon.rgb   <- third_party/resources/scenes/sky/pz.png（立方体贴图的正面）
                          pack_png_seq.py --mode patch 顺手写过这一张
    bg_morning.rgb     **孤儿**：仓库里没有任何脚本生成过它，源图也不在仓库里。
                          格式没问题（同样 320x480 RGB565），但它**无法从仓库重建** ——
                          要重建得先找到当初那张上午的天空源图。

> **本脚本复现出来的是"看起来一样"，不是"逐字节一样"。** 拿 pz.png 重跑，
> 95% 的像素有 1~2 级的色差（重采样的浮点细节），肉眼完全看不出。
> `third_party/fish/bg_afternoon.rgb` 目前放的是**原来那份原文件**（逐字节一致），
> 本脚本是给以后换背景用的。
"""

import argparse
import os
import sys

from PIL import Image

W, H = 320, 480


def build_background(path):
    """源图 -> 320x480 RGB。按 320:480 居中裁剪再缩放（不拉伸）。"""
    im = Image.open(path).convert("RGB")
    sw, sh = im.size
    tgt = W / float(H)

    if sw / float(sh) > tgt:            # 源图更宽：裁两边
        nw = int(round(sh * tgt))
        x0 = (sw - nw) // 2
        im = im.crop((x0, 0, x0 + nw, sh))
    else:                               # 源图更高：裁上下
        nh = int(round(sw / tgt))
        y0 = (sh - nh) // 2
        im = im.crop((0, y0, sw, y0 + nh))

    return im.resize((W, H), Image.LANCZOS)


def rgb565_bytes(im):
    """RGB -> 小端 RGB565 字节串（和固件的 uint16 像素一致）。"""
    px = im.load()
    out = bytearray(W * H * 2)

    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            i = (y * W + x) * 2
            out[i] = v & 0xFF
            out[i + 1] = (v >> 8) & 0xFF

    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", help="源图（任意尺寸，会被居中裁剪 + 缩放）")
    ap.add_argument("out", help="输出的 .rgb")
    args = ap.parse_args()

    if not os.path.isfile(args.src):
        sys.exit("找不到源图: %s" % args.src)

    im = build_background(args.src)
    data = rgb565_bytes(im)

    if len(data) != W * H * 2:
        sys.exit("内部错误：产出 %d 字节，应为 %d" % (len(data), W * H * 2))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(data)

    print("写出 %s（%dx%d RGB565，%d 字节）<- %s"
          % (args.out, W, H, len(data), args.src))


if __name__ == "__main__":
    main()
