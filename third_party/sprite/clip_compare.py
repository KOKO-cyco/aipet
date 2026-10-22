#!/usr/bin/env python3
"""把若干动作的第一帧按**脚底对齐**并排画出来，1:1 不缩放。

    python third_party/sprite/clip_compare.py <fish_role.spf> idle,click_angry,click_wave,... <out.png>

为什么要它：各动作的角色**大小**要能直接比 —— 但直接把帧并排会被"裁剪框"
（角色 + pad + 起跳余量）和"姿势"（蹲下/抬手）干扰。把**脚底压在同一条基线**上
之后，头部高度就能直接读出来了。

图上每 10px 一条浅格线，基线是红的。左下角标了该帧的角色高（px）。

注意：这里画的是**解码后的成品**（和固件上屏的像素一模一样），不是源素材。
"""
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, __import__("os").path.dirname(__file__))
from clip_sizes import inflate        # noqa: E402

GRID = 10          # 格线间距
PAD = 24           # 画布上下留白
GAP = 12           # 两块之间的间距
BASE_Y = 0         # 由内容决定


def load_frames(path):
    buf = open(path, "rb").read()
    assert buf[:4] == b"SPF7", "不是 SPF7"
    nc, _ = struct.unpack_from("<HH", buf, 4)
    gw, gh = struct.unpack_from("<HH", buf, 8)
    p = 12
    recs = []
    for _ in range(nc):
        recs.append(struct.unpack_from("<HHHHhhH", buf, p))
        p += 14
    names = []
    for _ in range(nc):
        ln = buf[p]
        p += 1
        names.append(bytes(buf[p:p + ln]).decode("ascii"))
        p += ln
    while p % 4:
        p += 1
    total = sum(r[0] for r in recs)
    offs = struct.unpack_from("<%dI" % (total + 1), buf, p)
    starts, acc = [], 0
    for r in recs:
        starts.append(acc)
        acc += r[0]
    return buf, names, recs, starts, offs, (gw, gh)


def frame_image(buf, recs, starts, offs, gi, fi):
    n, fps, cw, ch, dx, dy, role = recs[gi]
    b = starts[gi] + (fi % n)
    px = inflate(buf, cw * ch, offs[b], offs[b + 1]).reshape(ch, cw)
    a4 = ((px >> 12) & 0xF) * 17
    rgb = np.dstack([((px >> 8) & 0xF) * 17, ((px >> 4) & 0xF) * 17,
                     (px & 0xF) * 17]).astype(np.uint8)
    return Image.fromarray(np.dstack([rgb, a4]).astype(np.uint8), "RGBA")


def main():
    path, want, out = sys.argv[1], sys.argv[2].split(","), sys.argv[3]
    buf, names, recs, starts, offs, (gw, gh) = load_frames(path)

    tiles = []
    for w in want:
        if w not in names:
            print("  [跳过] 没有这个动作: %s" % w)
            continue
        gi = names.index(w)
        im = frame_image(buf, recs, starts, offs, gi, 0)
        al = np.array(im)[:, :, 3]
        ys, xs = np.nonzero(al >= 16)
        if not len(ys):
            continue
        im = im.crop((int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1))
        tiles.append((w, im, int(ys.max() - ys.min() + 1)))

    if not tiles:
        sys.exit("一个都没画出来")

    top = max(t[1].height for t in tiles)
    W = sum(t[1].width for t in tiles) + GAP * (len(tiles) + 1)
    H = top + PAD * 2

    cv = Image.new("RGB", (W, H), (245, 245, 248))

    # 格线（每 GRID 像素一条，从基线往上数）
    for y in range(H - PAD, 0, -GRID):
        for x in range(W):
            cv.putpixel((x, y), (215, 215, 222))

    base = H - PAD
    for x in range(W):
        cv.putpixel((x, base), (230, 90, 90))          # 脚底基线

    x = GAP
    for w, im, h in tiles:
        cv.paste(im, (x, base - im.height), im)
        x += im.width + GAP

    cv.save(out)
    print("脚底基线 y=%d（红线），格线每 %dpx" % (base, GRID))
    print("%-16s %6s %6s" % ("动作", "角色高", "相对待机"))
    ref = tiles[0][2]
    for w, im, h in tiles:
        print("%-16s %6d %+6d" % (w, h, h - ref))
    print("\n写出 %s" % out)


if __name__ == "__main__":
    main()
