#!/usr/bin/env python3
"""把某几帧拼成一张白底联络表，方便肉眼核对动作。

    python tools/clip_frames.py <clip.webm> --at 0.7,1.0,1.3,2.5 --out sheet.png

为什么不用 ffmpeg 的 filter_complex 拼：`color=` 源是无限长的，配 `overlay` 很容易
不收敛（本项目踩过，跑成了死循环）。这里直接解码成 numpy 再拼，稳。

为什么要白底：素材是透明背景，直接看 PNG 的话透明区在多数查看器里是黑/棋盘格，
角色本身的深色描边会糊掉。
"""

import argparse
import io
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

ALPHA_MIN = 32


def ffmpeg_bin():
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit("找不到 ffmpeg")
    return exe


def decode(ff, path):
    cmd = [ff, "-v", "error", "-c:v", "libvpx-vp9", "-i", path,
           "-pix_fmt", "rgba", "-f", "rawvideo", "-"]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        sys.exit("解码失败: %s" % r.stderr.decode("utf-8", "replace"))
    # 尺寸从首帧猜不了，用 ffprobe
    pr = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                         "-show_entries", "stream=width,height",
                         "-of", "csv=p=0", path], capture_output=True, text=True)
    w, h = [int(x) for x in pr.stdout.strip().split(",")[:2]]
    n = len(r.stdout) // (w * h * 4)
    return np.frombuffer(r.stdout, dtype=np.uint8)[:n * w * h * 4].reshape(n, h, w, 4), w, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("clip")
    ap.add_argument("--at", required=True, help="秒，逗号分隔")
    ap.add_argument("--out", required=True)
    ap.add_argument("--cols", type=int, default=4)
    ap.add_argument("--scale", type=float, default=1.0, help="每格再放大几倍")
    args = ap.parse_args()

    ff = ffmpeg_bin()
    a, W, H = decode(ff, args.clip)
    n = a.shape[0]
    print("共 %d 帧 %dx%d" % (n, W, H))

    # 按**全片**的 alpha 并集裁一个框，这样每格取景一致，好对比
    al = a[:, :, :, 3] > ALPHA_MIN
    cols = np.nonzero(al.any(axis=(0, 1)))[0]
    rows = np.nonzero(al.any(axis=(0, 2)))[0]
    x0, x1 = int(cols[0]), int(cols[-1]) + 1
    y0, y1 = int(rows[0]), int(rows[-1]) + 1
    cw, ch = x1 - x0, y1 - y0
    print("内容框 %d,%d..%d,%d  %dx%d" % (x0, y0, x1 - 1, y1 - 1, cw, ch))

    ts = [float(t) for t in args.at.split(",")]
    tiles = []
    for t in ts:
        f = min(n - 1, int(round(t * 24)))
        crop = a[f, y0:y1, x0:x1]
        rgb = crop[:, :, :3].astype(np.float32)
        al_ = (crop[:, :, 3:4].astype(np.float32)) / 255.0
        white = rgb * al_ + 255.0 * (1.0 - al_)          # 合成到白底
        img = Image.fromarray(white.astype(np.uint8))
        if args.scale != 1.0:
            img = img.resize((int(cw * args.scale), int(ch * args.scale)), Image.LANCZOS)
        tiles.append((t, f, img))

    tw, th = tiles[0][2].size
    rows_n = (len(tiles) + args.cols - 1) // args.cols
    sheet = Image.new("RGB", (tw * args.cols, th * rows_n), (255, 255, 255))
    for i, (t, f, img) in enumerate(tiles):
        sheet.paste(img, ((i % args.cols) * tw, (i // args.cols) * th))
    sheet.save(args.out)
    print("帧: " + "  ".join("%.2fs(#%d)" % (t, f) for t, f, _ in tiles))
    print("写出", args.out, sheet.size)


if __name__ == "__main__":
    main()
