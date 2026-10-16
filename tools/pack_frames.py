#!/usr/bin/env python3
"""把姿势图集切成对齐的帧序列，打包成固件能直接读的 ARGB4444 多动作文件。

切格：按角色**中心**对齐，不是按左边缘
--------------------------------------
图集里角色是按固定步距（192px）横向排列的，步距锚在**角色中心**上。如果拿
"该行第一个非透明像素"当格子原点，格子就整体左移约 22px，后果有两个：

  * 每格右边缘吃到**下一帧角色的左边缘** —— 播放时角色右侧有一条细竖线
  * 越靠后的格子偏得越多，角色**左边缘被切掉**几个像素

所以这里先按 alpha 分出行内每个角色的连通段，取中心，再令
    X0 = 中心 - 格宽/2
步距用最小二乘拟合（图集不总是精确的 192），避免行尾累积几像素漂移 ——
那会表现为播放时角色左右轻微滑动。

文件格式（小端）
----------------
    [magic "SPF3"][u16 w][u16 h][u16 clip_count][u16 reserved]
    [u8 bx0][u8 bx1][u8 by0][u8 by1]          <- 所有帧的内容框并集
    [clip_count x { u16 frames, u16 fps, u32 reserved }]
    [所有帧的 ARGB4444 数据，按 clip 顺序、帧顺序连续排列]

内容框（bx0..bx1, by0..by1）是**角色真正有像素的范围**，帧内其余部分是透明留白。
浮层夹紧位置时用它，而不是用帧尺寸 —— 否则角色会停在离屏幕边缘十几像素的地方。

ARGB4444：每像素 2 字节，高 4 位 alpha。浮层合成只需要一次"带 alpha 的顺序拷贝"，
2 字节/像素把带宽和体积都压到最小。

用法：
    python pack_frames.py <图集> --out 输出.spf [--preview 预览.png] [--scale 0.752]
    python pack_frames.py <图集> --rows 0,1,5 --out x.spf      # 只取指定行
"""
import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image

CELL_W, CELL_H = 192, 208
GRID_COLS, GRID_ROWS = 8, 11

# .spf 存的是 ARGB4444，alpha 低 4 位被丢掉 —— 量化后为 0 的像素等于全透明。
# 判定"角色在哪"必须用同一个阈值，否则会把极淡的噪点算成内容，包围盒虚胖。
ALPHA_MIN = 16

# 行内分段时，短于这个宽度的段当噪点丢掉
MIN_SEG_W = 20


def argb4444(arr):
    """RGBA uint8 数组 -> ARGB4444 uint16 数组。"""
    r = (arr[..., 0] >> 4).astype(np.uint16)
    g = (arr[..., 1] >> 4).astype(np.uint16)
    b = (arr[..., 2] >> 4).astype(np.uint16)
    a = (arr[..., 3] >> 4).astype(np.uint16)
    return (a << 12) | (r << 8) | (g << 4) | b


def row_segments(alpha, row):
    """切出该行里每个角色的横向连通段 [(x0, x1), ...]（含端点）。"""
    band = alpha[row * CELL_H:(row + 1) * CELL_H]
    has = band.any(axis=0)

    segs, s = [], None
    for i, v in enumerate(has):
        if v and s is None:
            s = i
        elif not v and s is not None:
            segs.append((s, i - 1))
            s = None
    if s is not None:
        segs.append((s, len(has) - 1))

    return [q for q in segs if q[1] - q[0] + 1 >= MIN_SEG_W]


def row_grid(alpha, row):
    """求该行的格子布局。

    返回 (x0, pitch, segs)：第 c 格的左边界 = x0 + c * pitch。
    没有内容返回 None。
    """
    segs = row_segments(alpha, row)
    if not segs:
        return None

    n = len(segs)
    cen = np.array([(a + b) / 2.0 for a, b in segs])

    if n >= 3:
        pitch, c0 = np.polyfit(np.arange(n, dtype=float), cen, 1)
        # 拟合出的步距应该贴着格宽；差太多说明分段数不对（粘连/漏检），
        # 退回固定步距 —— 宁可留几像素漂移，也不要把格子切歪。
        if not (0.9 * CELL_W <= pitch <= 1.1 * CELL_W):
            pitch, c0 = float(CELL_W), cen[0]
    else:
        pitch, c0 = float(CELL_W), cen[0]

    return c0 - CELL_W / 2.0, pitch, segs


def extract_row(im, alpha, row, scale, tw, th):
    """切出一行里所有非空的帧。返回 [np.uint16 数组, ...]。"""
    g = row_grid(alpha, row)
    if g is None:
        return [], 0.0

    x0, pitch, segs = g
    nseg = len(segs)
    out = []

    for c in range(nseg):
        left = int(round(x0 + c * pitch))
        cell = im.crop((left, row * CELL_H, left + CELL_W, (row + 1) * CELL_H))

        if cell.getchannel("A").getbbox() is None:      # 空格子
            continue

        out.append(argb4444(np.array(cell.resize((tw, th), Image.LANCZOS))))

    return out, pitch


def content_box(frames, tw, th):
    """一段帧的内容框并集（缩放后的坐标）。"""
    x0, x1, y0, y1 = tw, -1, th, -1

    for fr in frames:
        a = (fr >> 12) & 0xF
        ys, xs = np.nonzero(a)
        if len(xs) == 0:
            continue
        x0 = min(x0, int(xs.min())); x1 = max(x1, int(xs.max()))
        y0 = min(y0, int(ys.min())); y1 = max(y1, int(ys.max()))

    return None if x1 < 0 else (x0, x1, y0, y1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--rows", default=None, help="只取这些行（逗号分隔）；默认全部非空行")
    ap.add_argument("--scale", type=float, default=0.752, help="缩放（默认 0.752 -> 角色约 112px 宽）")
    ap.add_argument("--fps", type=int, default=12)
    ap.add_argument("--min-frames", type=int, default=2, help="少于此帧数的行丢弃")
    ap.add_argument("--out", required=True)
    ap.add_argument("--preview", default=None, help="横向拼一张所有动作的预览")
    args = ap.parse_args()

    im = Image.open(args.src).convert("RGBA")
    alpha = np.array(im)[:, :, 3] >= ALPHA_MIN
    tw, th = int(CELL_W * args.scale), int(CELL_H * args.scale)

    rows = ([int(x) for x in args.rows.split(",")] if args.rows
            else list(range(GRID_ROWS)))

    clips = []
    for r in rows:
        frames, pitch = extract_row(im, alpha, r, args.scale, tw, th)
        if len(frames) >= args.min_frames:
            clips.append(frames)
            print("  行 %2d: %2d 帧，步距 %.1f" % (r, len(frames), pitch))
        else:
            print("  行 %2d 只有 %d 帧，跳过" % (r, len(frames)))

    if not clips:
        print("没提取到任何动作")
        return 1

    total_frames = sum(len(c) for c in clips)
    per_frame = tw * th * 2
    print("动作 %d 组，共 %d 帧，单帧 %dx%d = %d 字节，合计 %.2f MB"
          % (len(clips), total_frames, tw, th, per_frame,
             total_frames * per_frame / 1048576))

    box = content_box([f for c in clips for f in c], tw, th)
    if box is None:
        print("所有帧都是空的")
        return 1
    bx0, bx1, by0, by1 = box
    print("内容框 x %d..%d (宽%d)  y %d..%d (高%d)   帧 %dx%d"
          % (bx0, bx1, bx1 - bx0 + 1, by0, by1, by1 - by0 + 1, tw, th))
    print("  -> 浮层夹紧范围: x 下限 %d, x 上限 %d（按内容而不是帧）"
          % (-bx0, 320 - 1 - bx1))

    # ---- 写文件 ----
    HDR = 16
    off = HDR + 8 * len(clips)
    frames_off = []
    for c in clips:
        frames_off.append(off)
        off += len(c) * per_frame

    with open(args.out, "wb") as f:
        f.write(b"SPF3")
        f.write(struct.pack("<HHHH", tw, th, len(clips), 0))
        f.write(bytes((bx0, bx1, by0, by1)))
        for c in clips:
            f.write(struct.pack("<HHI", len(c), args.fps, 0))
        for c in clips:
            for fr in c:
                f.write(fr.astype("<u2").tobytes())

    print("已写出 %s  (%.2f MB)" % (args.out, os.path.getsize(args.out) / 1048576))
    for i, c in enumerate(clips):
        print("  动作 %2d: %d 帧  @%dfps  %.1f 秒" % (i, len(c), args.fps, len(c) / args.fps))

    # ---- 预览 ----
    if args.preview:
        gap = 6
        W = sum(len(c) * tw for c in clips) + gap * (len(clips) - 1)
        strip = Image.new("RGBA", (W, th), (40, 40, 50, 255))
        x = 0
        for c in clips:
            for fr in c:
                a = ((fr >> 12) & 0xF).astype(np.uint8) * 17
                r = ((fr >> 8) & 0xF).astype(np.uint8) * 17
                g = ((fr >> 4) & 0xF).astype(np.uint8) * 17
                b = (fr & 0xF).astype(np.uint8) * 17
                strip.alpha_composite(Image.fromarray(np.dstack([r, g, b, a])), (x, 0))
                x += tw
            x += gap
        strip.save(args.preview)
        print("已写出预览", args.preview, strip.size)

    return 0


if __name__ == "__main__":
    sys.exit(main())
