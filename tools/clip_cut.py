#!/usr/bin/env python3
"""裁 + 挖 + 抽帧变速：从一段素材里挑出若干帧段拼成新的，并可隔 N 帧取一帧（= 加速 N 倍）。

    # 只留 18~71 帧（挖掉后面 5 秒死区）
    python tools/clip_cut.py in.webm --out out.webm --keep 18:72

    # 挖掉中间 62~140 的"干站着"，前后拼起来
    python tools/clip_cut.py in.webm --out out.webm --keep 0:62 --keep 140:241

    # 起手：只留 19~35 帧，隔一帧取一帧 -> 0.33s
    python tools/clip_cut.py in.webm --out out.webm --keep 19:36 --step 2

参数用**源帧号**（0 起，`A:B` 含 A 不含 B），不是秒 —— 挑切点时看的是
tools/clip_probe.py 输出的帧号，换算成秒容易错一位。`--keep` 可以给多次，
按给的顺序首尾相接。

为什么用 `setpts=N/(24*TB)` 而不是 `-r 24`
------------------------------------------------
抽帧之后留下来的帧还带着**原来的时间戳**（0, 2/24, 4/24…）。这时候再加 `-r 24`
是"补帧"—— 它会把稀疏的帧**复制**到 24fps，画面变成慢动作，跟"抽帧加速"正好相反。
`setpts=N/(24*TB)` 把输出帧从 0 开始重新编号，第 N 帧就是 N/24 秒，抽掉帧之后
自然就快了 N 倍。

为什么用 `trim` + `concat` 而不是 `select`
----------------------------------------
`select` 的表达式要写成一串 `between(n,a,b)+between(n,c,d)`，逗号还得转义，
段一多就没法看。`trim=start_frame=..:end_frame=..` 一段一个标签，`concat` 拼起来，
清楚得多。

**必须 `-c:v libvpx-vp9`**：素材的 alpha 在 VP9 侧数据里，原生 vp9 解码器不读
（见 tools/fish_extract.py 的文件头）。输出也必须是 yuva420p + 关掉 alt-ref。
"""

import argparse
import os
import shutil
import subprocess
import sys

FPS = 24


def ffmpeg_bin():
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit("找不到 ffmpeg")
    return exe


def count_frames(ff, path):
    # 解一遍数帧 —— nb_frames 对这些 webm 常常是 N/A，信不过
    r = subprocess.run([ff, "-v", "error", "-c:v", "libvpx-vp9", "-i", path,
                        "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                       capture_output=True)
    pr = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                         "-show_entries", "stream=width,height",
                         "-of", "csv=p=0", path], capture_output=True, text=True)
    w, h = [int(x) for x in pr.stdout.strip().split(",")[:2]]
    return len(r.stdout) // (w * h)


def parse_ranges(args, n):
    raw = []
    for k in args.keep:
        a, _, b = k.partition(":")
        raw.append((int(a), int(b) if b else n))
    if not raw:
        raw = [(args.f0 or 0, args.f1 if args.f1 is not None else n)]

    out = []
    for a, b in raw:
        a = max(0, min(n, a))
        b = max(a, min(n, b))
        if a < b:
            out.append((a, b))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--out", required=True)
    ap.add_argument("--keep", action="append", default=[],
                    help="保留的帧段 A:B（可重复，按给顺序拼接）")
    ap.add_argument("--from", dest="f0", type=int, default=None,
                    help="（不写 --keep 时的单段写法）起始帧，含")
    ap.add_argument("--to", dest="f1", type=int, default=None,
                    help="（不写 --keep 时的单段写法）结束帧，不含")
    ap.add_argument("--step", type=int, default=1, help="每隔几帧取一帧（= 加速几倍）")
    ap.add_argument("--vf", default=None,
                    help="额外的滤镜，接在末尾（在 [out] 之前）。用来做校色这类"
                         "收尾处理，例：--vf \"eq=saturation=0.90\"")
    args = ap.parse_args()

    ff = ffmpeg_bin()
    n = count_frames(ff, args.src)
    rng = parse_ranges(args, n)
    if not rng:
        sys.exit("帧段全空（源共 %d 帧）" % n)
    if args.step < 1:
        sys.exit("--step 至少 1")

    parts, labels = [], []
    for i, (a, b) in enumerate(rng):
        parts.append("[0:v]trim=start_frame=%d:end_frame=%d,setpts=PTS-STARTPTS[s%d]"
                     % (a, b, i))
        labels.append("[s%d]" % i)

    extra = ("," + args.vf) if args.vf else ""

    if len(labels) == 1:
        tail = labels[0]
    else:
        parts.append("".join(labels) + "concat=n=%d:v=1:a=0[cat]" % len(labels))
        tail = "[cat]"

    if args.step > 1:
        parts.append("%sselect='not(mod(n\\,%d))',setpts=N/(%d*TB)%s[out]"
                     % (tail, args.step, FPS, extra))
    else:
        parts.append("%ssetpts=N/(%d*TB)%s[out]" % (tail, FPS, extra))

    cmd = [ff, "-v", "warning", "-y", "-c:v", "libvpx-vp9", "-i", args.src,
           "-filter_complex", ";".join(parts), "-map", "[out]",
           "-fps_mode", "cfr", "-r", str(FPS), "-an",
           "-c:v", "libvpx-vp9", "-pix_fmt", "yuva420p",
           "-auto-alt-ref", "0", "-b:v", "0", "-crf", "28", "-row-mt", "1",
           args.out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("ffmpeg 失败:\n%s" % r.stderr.strip())

    out_n = count_frames(ff, args.out)
    print("%-28s %s step %d -> %d 帧 %.2fs"
          % (os.path.basename(args.src),
             "+".join("%d:%d" % (a, b) for a, b in rng), args.step,
             out_n, out_n / float(FPS)))


if __name__ == "__main__":
    main()
