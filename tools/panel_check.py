#!/usr/bin/env python3
"""按**设备上的真实几何**把每个动作合成到 320x480 面板上，量最终位置和大小。

    python tools/panel_check.py <fish_role.spf> <out.png> [每块取第几帧]

为什么不能直接量 .spf：屏幕上看到的位置 = 锚点框位置 + 本组 dx/dy，而锚点框位置
是**固件运行时**从待机的脚底推出来的（app_role2d.c 的 prv_place）：

    sg_ax = (面板宽 - 锚点框宽) / 2
    sg_ay = R2D_FEET_Y - (待机的 dy + 待机第一帧脚底在裁剪框内的行号)

本脚本把这两步**照抄一遍**，所以量出来的就是设备上真实的脚底 y / 头顶 y / 角色高。

最后一列"脚底偏差"才是要看的：它不为 0 就说明切到那一组时角色会上下跳。
"""
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, __import__("os").path.dirname(__file__))
from clip_sizes import inflate        # noqa: E402

PANEL_W, PANEL_H = 320, 480
FEET_Y = 417          # 必须和 app_role2d.c 的 R2D_FEET_Y 一致


def load(path):
    buf = open(path, "rb").read()
    assert buf[:4] == b"SPF7"
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
    return buf, names, recs, starts, offs, gw, gh


def frame_rgba(buf, recs, starts, offs, gi, fi):
    n, fps, cw, ch, dx, dy, role = recs[gi]
    b = starts[gi] + (fi % n)
    px = inflate(buf, cw * ch, offs[b], offs[b + 1]).reshape(ch, cw)
    a4 = ((px >> 12) & 0xF) * 17
    rgb = np.dstack([((px >> 8) & 0xF) * 17, ((px >> 4) & 0xF) * 17,
                     (px & 0xF) * 17]).astype(np.uint8)
    return np.dstack([rgb, a4]).astype(np.uint8)


def content_rows(fr):
    al = fr[:, :, 3]
    rows = np.nonzero((al >= 16).any(axis=1))[0]
    return (int(rows.min()), int(rows.max())) if len(rows) else (0, 0)


def main():
    path, out = sys.argv[1], sys.argv[2]
    fi = int(sys.argv[3]) if len(sys.argv) > 3 else 0

    buf, names, recs, starts, offs, gw, gh = load(path)

    # ---- 锚点框位置：照抄 prv_place() ----
    n, fps, cw, ch, dx, dy, role = recs[0]
    fr0 = frame_rgba(buf, recs, starts, offs, 0, 0)
    t, b = content_rows(fr0)
    feet_row = b                       # 待机第一帧脚底在裁剪框内的行号
    ax = (PANEL_W - gw) // 2
    ay = FEET_Y - (dy + feet_row)
    print("锚点框 %dx%d @ (%d,%d)   待机脚底 = %d（目标 %d）"
          % (gw, gh, ax, ay, ay + dy + feet_row, FEET_Y))
    print()
    print("%-16s %7s %7s %7s   %s" % ("动作", "头顶y", "脚底y", "角色高", "脚底偏差"))

    tiles, ref_feet = [], None
    for i in range(len(names)):
        fr = frame_rgba(buf, recs, starts, offs, i, fi)
        t, b = content_rows(fr)
        h = b - t + 1
        ox = ax + recs[i][4]
        oy = ay + recs[i][5]
        feet = oy + b
        head = oy + t
        if ref_feet is None:
            ref_feet = feet
        print("%-16s %7d %7d %7d   %+d" % (names[i], head, feet, h, feet - ref_feet))

        panel = Image.new("RGBA", (PANEL_W, PANEL_H), (30, 60, 110, 255))
        im = Image.fromarray(fr, "RGBA")
        panel.alpha_composite(im, (ox, oy))
        # 脚底参考线（红）
        d = np.array(panel)
        if 0 <= ref_feet < PANEL_H:
            d[ref_feet, :, :3] = (230, 90, 90)
        tiles.append(Image.fromarray(d, "RGBA"))

    gap = 6
    cv = Image.new("RGB", (PANEL_W * len(tiles) + gap * (len(tiles) + 1),
                           PANEL_H + gap * 2), (70, 70, 80))
    for i, t in enumerate(tiles):
        cv.paste(t.convert("RGB"), (gap + i * (PANEL_W + gap), gap))
    cv.save(out)
    print("\n写出 %s（每块一帧，红线 = 第一个动作的脚底）" % out)


if __name__ == "__main__":
    main()
