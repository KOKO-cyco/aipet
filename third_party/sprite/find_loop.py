#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
在一条动作的帧序列里，找"首尾能接上"的循环区间。

为什么需要它
------------
AI 说话/思考的时长是不可控的（说 20 秒很正常），而 `talk` / `think` 这种
"持续动作"只能循环播。如果整条循环（0..N-1），最后那帧和第 0 帧的姿态差得远，
接缝处就会**瞬移/抽动**。

解法是只循环其中一段 [a, b]：让第 b 帧和第 a 帧的姿态足够接近，接回去就像
正常播下一帧。这个脚本负责把 (a, b) **算出来** —— 手调帧号不可复现，换素材就废。

判据
----
两帧的距离用**下采样灰度图的平均绝对差**（对像素级噪声不敏感，看的是姿态）。

- `seam = d(b, a)`：接缝处跳多大。**要和"正常播下一帧"的 `d(i, i+1)` 比**，
  而不是和 0 比 —— 动作本身每帧都在变，接缝只要不比正常帧步大就行。
- `seam2 = d(b-1, a-1)`：**只看姿态会漏掉"速度不接"**。两帧姿态一样、但一个在
  往左动一个在往右动，接回去照样抽一下。多比一帧就能看出局部运动方向是否一致：
  两对都小 = 姿态和速度都接得上。
- `motion`：区间内部的最大帧间距离。太小说明这段几乎是静止的，循环起来像卡住。

挑 `seam + seam2` 最小、且 `motion` 够大的区间。

用法
----
    python third_party/sprite/find_loop.py third_party/resources/fish_seq/talk
    python third_party/sprite/find_loop.py third_party/resources/fish_seq/think --top 10

输出每个候选的 (a, b)（**0 基，含两端**）、帧数、seam / seam2 和正常帧步的比值、
内部动作幅度。拿到的帧号写进固件（见 page_pet.c 的 sg_loop_range[]）。
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image

# 下采样边长。64 够看清姿态，又让 205 帧的距离矩阵算得飞快。
SIZE = 64


def frames_of(d):
    """目录下的帧文件，按文件名排序（零填充，字典序 = 帧序）。"""
    fs = sorted(f for f in os.listdir(d) if f.lower().endswith((".png", ".bmp")))
    return [os.path.join(d, f) for f in fs]


def load(path):
    """RGBA -> 合成到黑底 -> 灰度 -> 缩放。返回 (SIZE*SIZE,) 的 uint8。

    合成到黑底是为了把 alpha 也算进"姿态"：角色是浅色、底是透明，合成后
    剪影清晰，比只看 RGB 稳。"""
    im = Image.open(path).convert("RGBA")
    bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
    im = Image.alpha_composite(bg, im).convert("L")
    im = im.resize((SIZE, SIZE), Image.BILINEAR)
    return np.asarray(im, dtype=np.float32).reshape(-1)


def main():
    ap = argparse.ArgumentParser(description="找一条动作里可无缝循环的帧区间")
    ap.add_argument("dir", help="帧序列目录（如 third_party/resources/fish_seq/talk）")
    ap.add_argument("--top", type=int, default=8, help="打印前 N 个候选（默认 8）")
    ap.add_argument("--min-frames", type=int, default=24,
                    help="循环区间最少多少帧（默认 24 = 24fps 下 1 秒）")
    ap.add_argument("--min-motion", type=float, default=0.35,
                    help="区间内部动作幅度至少要达到整条的这个比例（默认 0.35）")
    ap.add_argument("--sample", type=int, default=0, metavar="N",
                    help="先按打包器的方式均匀抽成 N 帧再分析（0 = 不抽）。"
                         "**待机组必须给**：固件里播的是抽过帧的版本"
                         "（build_fish.sh 的 --idle-frames），在源帧上找循环点对不上。")
    args = ap.parse_args()

    files = frames_of(args.dir)
    n = len(files)
    if n < args.min_frames * 2:
        print("帧太少（%d），没必要找循环点" % n)
        return 1

    # 复刻 pack_seq.py 的 sample_evenly()：np.linspace(0, n-1, N).round()
    # 抽完之后报出的帧号就是**固件里的组内帧号**，可以直接抄进 sg_loop[]。
    if args.sample and args.sample < n:
        idx = np.linspace(0, n - 1, args.sample).round().astype(int)
        idx = sorted(set(idx.tolist()))
        print("按打包器的方式抽帧：%d -> %d 帧（linspace(0, %d, %d)）"
              % (n, len(idx), n - 1, args.sample))
        files = [files[i] for i in idx]
        n = len(files)

    print("读 %d 帧 ..." % n)
    F = np.stack([load(p) for p in files])          # (n, D)
    print("  下采样 %dx%d，距离矩阵 %d x %d" % (SIZE, SIZE, n, n))

    # 相邻帧差 = "正常播下一帧"的基线
    adj = np.mean(np.abs(F[1:] - F[:-1]), axis=1)
    base = float(np.median(adj))

    # 距离矩阵（逐行算，避免 n x n x D 的大广播）
    D = np.empty((n, n), dtype=np.float32)
    for a in range(n):
        D[a] = np.mean(np.abs(F - F[a]), axis=1)

    total_motion = float(D.max())

    # 候选：长度 >= min_frames 的所有 (a, b)，b = a + L（含两端，共 L+1 帧）
    cands = []
    for L in range(args.min_frames, n):
        for a in range(0, n - L):
            b = a + L
            seam = float(D[a][b])
            # 速度连续性：再接一帧看看局部运动方向对不对得上。
            # a == 0 时"前一帧"就是 b 自己，比不出东西 —— 退回只按姿态算
            # （整条循环 [0, n-1] 就是这种，不能把它排除掉）。
            seam2 = float(D[a - 1][b - 1]) if a >= 1 else seam
            # 区间内部的动作幅度：这一段里两两距离的最大值
            sub = D[a:b + 1, a:b + 1]
            motion = float(sub.max())
            if motion < args.min_motion * total_motion:
                continue                    # 几乎是静止的一段，循环起来像卡住
            cands.append((seam + seam2, seam, seam2, a, b, motion))

    if not cands:
        print("\n没有满足条件的区间 —— 放宽 --min-motion 或 --min-frames")
        return 1

    cands.sort(key=lambda c: c[0])

    print("\n正常帧步 d(i,i+1) 中位数 = %.2f（接缝比它小就看不出接缝）" % base)
    print("整条最大帧距 = %.2f\n" % total_motion)
    print("  #   a    b   帧数   seam  seam2  (都比帧步)  内部幅度")
    print("  " + "-" * 58)
    for i, (cost, seam, seam2, a, b, motion) in enumerate(cands[:args.top]):
        print("  %-3d %-4d %-4d %-5d %-5.2f %-5.2f  (%.2fx %.2fx)  %.0f%%"
              % (i + 1, a, b, b - a + 1, seam, seam2,
                 seam / base, seam2 / base, 100.0 * motion / total_motion))

    cost, seam, seam2, a, b, motion = cands[0]
    print("\n接缝最小的候选：第 %d..%d 帧（%d 帧 = %.1f 秒），姿态 %.2fx / 速度 %.2fx 帧步"
          % (a, b, b - a + 1, (b - a + 1) / 24.0, seam / base, seam2 / base))

    # 但"接缝最小"不等于"最适合循环"：持续动作（说话/思考）要循环几十秒，
    # 一个 2 秒的循环重复十几遍，一眼就看出在循环。所以在**接缝够好**的候选里
    # 挑最长的 —— 接缝 1.5 倍和 1.9 倍肉眼没区别，循环长度区别很大。
    best_cost = cost
    ok = [c for c in cands if c[0] <= best_cost * 1.25]
    ok.sort(key=lambda c: -(c[4] - c[3]))          # 帧数多的排前面

    print("\n接缝够好（<= 最优的 1.25 倍）里**最长的几个**：")
    print("  #   a    b   帧数  时长    姿态    速度   内部幅度")
    print("  " + "-" * 52)
    for i, (cost, seam, seam2, a, b, motion) in enumerate(ok[:4]):
        print("  %-3d %-4d %-4d %-5d %-6.1fs %.2fx  %.2fx  %.0f%%"
              % (i + 1, a, b, b - a + 1, (b - a + 1) / 24.0,
                 seam / base, seam2 / base, 100.0 * motion / total_motion))

    cost, seam, seam2, a, b, motion = ok[0]
    print("\n推荐（接缝够好里挑最长）：第 %d..%d 帧（%d 帧 = %.1f 秒），"
          "姿态 %.2fx / 速度 %.2fx 帧步，内部幅度 %.0f%%"
          % (a, b, b - a + 1, (b - a + 1) / 24.0,
             seam / base, seam2 / base, 100.0 * motion / total_motion))
    return 0


if __name__ == "__main__":
    sys.exit(main())
