#!/usr/bin/env python3
"""把处理过的动作片段拼成一个预览视频，方便肉眼过一遍。

    # 所有片段同时循环，一眼看全（默认 6 列）
    python tools/clip_preview.py --mode grid --out preview/fish_grid.webm

    # 顺序各播一遍，看**时长和节奏**（grid 里短片段会循环，看不出快慢）
    python tools/clip_preview.py --mode reel --out preview/fish_reel.webm

素材默认取 tools/fish_extract.py 的 CLIPS 表（就是打包用的那批）。

为什么要合成白底：素材是透明背景，直接播的话透明区在播放器里是黑块，角色的
深色描边会糊掉。合成白底之后任何播放器都能正常看。

为什么不用 ffmpeg 的 `color=` 源直接 overlay：那是**无限长**的流，配 overlay
不收敛（本项目踩过，跑成了死循环）。这里一律用 `-t` 把输出钉死。
"""

import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fish_extract import CLIPS, SRC_ROOT  # noqa: E402

# 网格预览的显示名（顺序和 fish_extract.CLIPS 一致）
ORDER = [c[0] for c in CLIPS]

# fish_extract.SRC_ROOT 是**相对 tools/ 的**（它自己就在那个目录里跑），
# 这里从项目根跑，所以要先按脚本位置解成绝对路径。
ASSET_ROOT = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), SRC_ROOT))


def ffmpeg_bin():
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit("找不到 ffmpeg")
    return exe


def src_of(name):
    for n, rel in CLIPS:
        if n == name:
            return os.path.join(ASSET_ROOT, rel)
    return None


def build_grid(ff, paths, out, cols, tw, th, dur):
    rows = (len(paths) + cols - 1) // cols
    W, H = tw * cols, th * rows

    # **白底 + 逐个 overlay**，不用 xstack。
    # xstack 的 `fill=none` 在这版 ffmpeg 上保不住 alpha（实测输出背景是黑的），
    # 而且它一次要把所有输入拼起来，不好查。逐个 overlay 虽然链长，但每一环的
    # 行为都是确定的：白底在下，每格按偏移盖上去。
    parts = ["color=white:r=24:s=%dx%d[base]" % (W, H)]
    prev = "[base]"
    for i in range(len(paths)):
        parts.append("[%d:v]scale=%d:%d[t%d]" % (i, tw, th, i))
        cur = "[o%d]" % i
        parts.append("%s[t%d]overlay=%d:%d:format=auto%s"
                     % (prev, i, (i % cols) * tw, (i // cols) * th, cur))
        prev = cur
    parts.append("%sformat=yuv420p[out]" % prev)

    cmd = [ff, "-v", "warning", "-y"]
    for p in paths:
        cmd += ["-c:v", "libvpx-vp9", "-stream_loop", "-1", "-i", p]      # 短的循环，别定格
    cmd += ["-filter_complex", ";".join(parts), "-map", "[out]",
            "-t", "%.3f" % dur, "-r", "24", "-an",
            "-c:v", "libvpx-vp9", "-b:v", "0", "-crf", "34", "-row-mt", "1",
            out]
    return cmd


def build_reel(ff, paths, out):
    # **每段先各自压到白底，再 concat。**
    # 先 concat 再 overlay 是不行的：concat 之后 alpha 会丢（实测输出全黑），
    # 大概是它在输出格式协商时选了不带 alpha 的那种。逐段处理就没这个问题 ——
    # 和 grid 用的是同一套做法。
    parts, labels = [], []
    for i in range(len(paths)):
        parts.append("[%d:v]scale=640:360[t%d]" % (i, i))
        parts.append("color=white:r=24:s=640x360[bg%d]" % i)
        parts.append("[bg%d][t%d]overlay=format=auto:shortest=1,format=rgba[o%d]"
                     % (i, i, i))
        labels.append("[o%d]" % i)
    parts.append("%sconcat=n=%d:v=1:a=0,format=yuv420p[out]"
                 % ("".join(labels), len(paths)))

    cmd = [ff, "-v", "warning", "-y"]
    for p in paths:
        cmd += ["-c:v", "libvpx-vp9", "-i", p]
    cmd += ["-filter_complex", ";".join(parts), "-map", "[out]",
            "-r", "24", "-an",
            "-c:v", "libvpx-vp9", "-b:v", "0", "-crf", "34", "-row-mt", "1",
            out]
    return cmd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["grid", "reel"], default="grid")
    ap.add_argument("--out", required=True)
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--tile", default="320x180", help="每格尺寸 WxH")
    ap.add_argument("--only", default=None, help="只放这些（逗号分隔）")
    ap.add_argument("--dur", type=float, default=None, help="grid 模式时长（默认取最长的那段）")
    args = ap.parse_args()

    ff = ffmpeg_bin()
    names = args.only.split(",") if args.only else ORDER
    paths = []
    for n in names:
        p = src_of(n)
        if p and os.path.isfile(p):
            paths.append(p)
        else:
            print("  跳过 %-16s（找不到源）" % n)
    if not paths:
        sys.exit("没有可用的片段")

    tw, th = [int(x) for x in args.tile.lower().split("x")]
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    if args.mode == "grid":
        dur = args.dur or 10.5
        cmd = build_grid(ff, paths, args.out, args.cols, tw, th, dur)
    else:
        cmd = build_reel(ff, paths, args.out)

    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("ffmpeg 失败:\n%s" % r.stderr.strip()[-2000:])

    print("写出 %s（%.1f MB）" % (args.out, os.path.getsize(args.out) / 1048576.0))
    if args.mode == "grid":
        for i, p in enumerate(paths):
            print("  %2d) %s" % (i + 1, os.path.basename(p)))
    else:
        print("顺序: " + " -> ".join(os.path.basename(p) for p in paths))


if __name__ == "__main__":
    main()
