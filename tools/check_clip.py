#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
量一段素材的角色几何，和基准（idle）比对 —— 打包前先跑这个。

为什么需要
----------
`pack_seq.py` 的对齐规则是**两条硬的**：

  1. **缩放是全局唯一的** —— 只按基准动作（`--ref-action idle`）的并集高度算一次
     （`scale = --height / idle并集高`），所有动作共用。所以你的新素材里角色
     比 idle 大 2%，上机就大 2%。
  2. **锚点是"整段并集内容框"的底边中点** —— 并集底边在源画面里高 5px，上机就
     高 5px；水平中心偏 4px，上机就偏 4px。

新素材对不上这两条时，切动作就会**轻微跳一下**（大小、位置都对不齐）。这个脚本
把偏差量出来，让你在打包前就能改，而不是烧进去才发现。

看哪个数
--------
- **并集内容框**：pack_seq 真正用的就是它（底边 + 水平中心）。
- **逐帧中位框**：把"整段动作幅度大"造成的干扰去掉，反映**角色本体**的典型大小和
  站位。角色大小看这个，比看并集准（并集高度含动作幅度，跳一下就能高 20px）。

用法
----
    # 量基准（idle），把它的数字记下来当目标
    python tools/check_clip.py third_party/blue_big_fat_fish/idle/待机摇摆.webm

    # 量新素材，和基准比
    python tools/check_clip.py --ref third_party/blue_big_fat_fish/idle/待机摇摆.webm \
        新素材.webm

    # 也可以直接量解出来的 PNG 序列目录
    python tools/check_clip.py third_party/resources/fish_seq/talk
"""

import argparse
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

# 和 fish_extract.py 保持一致：中间分辨率按高度缩到 180（宽度自动、保比例）。
# 所有比对都在这个空间里做，因为 pack_seq 就是在这个空间里算的。
INTER_H = 180
ALPHA_MIN = 32


def ffmpeg_bin():
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit("找不到 ffmpeg")
    return exe


def frames_of(path):
    """返回 (帧数组列表 [(h,w,4) uint8], 画布描述)。webm 或 PNG 目录都吃。"""
    if os.path.isdir(path):
        fs = sorted(f for f in os.listdir(path) if f.lower().endswith((".png", ".bmp")))
        if not fs:
            sys.exit("%s 里没有帧" % path)
        arrs = [np.asarray(Image.open(os.path.join(path, f)).convert("RGBA"))
                for f in fs]
        return arrs, "PNG 目录（%d 帧）" % len(arrs)

    if not os.path.isfile(path):
        sys.exit("找不到 %s" % path)

    ff = ffmpeg_bin()
    # 先探一次源尺寸：解出来的 raw 是纯像素流，得知道 w/h 才能 reshape。
    pr = subprocess.run(["ffprobe", "-v", "error",
                         "-select_streams", "v:0",
                         "-show_entries", "stream=width,height",
                         "-of", "csv=p=0", path], capture_output=True, text=True)
    sw, sh = [int(x) for x in pr.stdout.strip().split(",")[:2]]
    # **宽度自己算、显式传给 ffmpeg**，不要写 `scale=-2:180` —— ffmpeg 的 -2 对
    # 奇数宽度的取整和 Python 的 round() 不一致（135 -> ffmpeg 给 136，我们给 134），
    # 差 2 像素就会让下面的 reshape 整个错位，量出来的内容框全是垃圾。
    tw = max(2, int(round(sw * INTER_H / float(sh))))
    tw -= tw % 2
    r = subprocess.run([ff, "-v", "error", "-c:v", "libvpx-vp9", "-i", path,
                        "-vf", "scale=%d:%d" % (tw, INTER_H), "-pix_fmt", "rgba",
                        "-f", "rawvideo", "-"], capture_output=True)
    if r.returncode != 0:
        sys.exit("ffmpeg 失败 (%s):\n%s" % (path, r.stderr.decode("utf-8", "replace")))
    n = len(r.stdout) // (tw * INTER_H * 4)
    a = np.frombuffer(r.stdout, dtype=np.uint8)[:n * tw * INTER_H * 4] \
          .reshape(n, INTER_H, tw, 4)
    return list(a), "%dx%d（%d 帧，按高缩到 %d）" % (tw, INTER_H, n, INTER_H)


def boxes(arrs):
    """(并集框, 逐帧框列表)。框 = (x0,y0,x1,y1)，没有内容的帧为 None。"""
    per = []
    u = None
    for a in arrs:
        m = a[:, :, 3] >= ALPHA_MIN
        ys, xs = np.nonzero(m)
        if len(xs) == 0:
            per.append(None)
            continue
        b = (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))
        per.append(b)
        u = b if u is None else (min(u[0], b[0]), min(u[1], b[1]),
                                 max(u[2], b[2]), max(u[3], b[3]))
    return u, per


def stats(u, per, w, h):
    """(高, 底边y, 中心x) 的并集值 + 逐帧中位值。"""
    real = [b for b in per if b]
    med_h = int(np.median([b[3] - b[1] + 1 for b in real]))
    med_b = int(np.median([b[3] for b in real]))
    med_c = float(np.median([(b[0] + b[2]) / 2.0 for b in real]))
    return {
        "canvas": (w, h),
        "uni_h": u[3] - u[1] + 1, "uni_b": u[3], "uni_c": (u[0] + u[2]) / 2.0,
        "med_h": med_h, "med_b": med_b, "med_c": med_c,
        "uni": u,
    }


def show(tag, s):
    print("  %-6s 画布 %dx%d" % (tag, s["canvas"][0], s["canvas"][1]))
    print("    并集内容框 (%3d,%3d,%3d,%3d)  高%3d  底边y=%3d  中心x=%6.1f"
          % (s["uni"] + (s["uni_h"], s["uni_b"], s["uni_c"])))
    print("    逐帧中位                  高%3d  底边y=%3d  中心x=%6.1f"
          % (s["med_h"], s["med_b"], s["med_c"]))


def main():
    ap = argparse.ArgumentParser(description="量素材的角色几何，和基准比对")
    ap.add_argument("clips", nargs="+", help="要量的 .webm 或 PNG 目录")
    ap.add_argument("--ref", default=None,
                    help="基准素材（默认拿第一个要量的当基准）")
    args = ap.parse_args()

    targets = list(args.clips)
    if args.ref:
        ref_path = args.ref
    else:
        ref_path = targets.pop(0)
        if not targets:
            print("只给了一个素材 —— 把它当基准，没有可比的。再给几个要检查的。")

    arrs, desc = frames_of(ref_path)
    u, per = boxes(arrs)
    if u is None:
        sys.exit("基准素材全是空的（alpha 全 0？）")
    rs = stats(u, per, arrs[0].shape[1], arrs[0].shape[0])

    print("=" * 72)
    print("基准  %s" % ref_path)
    print("      %s" % desc)
    show("基准", rs)
    print("=" * 72)

    # 上机像素：fish_extract 之后是 320x180，pack_seq 再乘 scale = 156 / 基准并集高
    # （见 build_fish.sh 的 --height 156）。所以"源 1px 的偏差"上机是多少，能直接算。
    scale_dev = 156.0 / rs["uni_h"]

    if not targets:
        print()
        print("要检查的素材几何应该满足（和基准一致）：")
        print("  并集底边 y = %d，并集中心 x = %.1f，角色高（逐帧中位）= %d"
              % (rs["uni_b"], rs["uni_c"], rs["med_h"]))
        print("  上机换算：这里差 1px，屏幕上差约 %.2f px" % scale_dev)
        return 0

    for path in targets:
        arrs, desc = frames_of(path)
        u, per = boxes(arrs)
        if u is None:
            print("\n%s：全是空的，跳过" % path)
            continue
        s = stats(u, per, arrs[0].shape[1], arrs[0].shape[0])

        print("\n%s" % path)
        print("      %s" % desc)
        show("本段", s)

        dh = s["med_h"] - rs["med_h"]
        db = s["uni_b"] - rs["uni_b"]
        dc = s["uni_c"] - rs["uni_c"]
        print("     ---- 与基准的差 ----")
        print("     角色高  %+4d px  -> 上机约 %+.1f px  (%+.1f%%)"
              % (dh, dh * scale_dev, 100.0 * dh / rs["med_h"]))
        print("     底边y   %+4d px  -> 上机约 %+.1f px" % (db, db * scale_dev))
        print("     中心x   %+6.1f px  -> 上机约 %+.1f px" % (dc, dc * scale_dev))

        bad = []
        if abs(dh) * scale_dev > 2.0:
            bad.append("角色大小差 %.1f px" % (dh * scale_dev))
        if abs(db) * scale_dev > 2.0:
            bad.append("脚底位置差 %.1f px" % (db * scale_dev))
        if abs(dc) * scale_dev > 2.0:
            bad.append("水平位置差 %.1f px" % (dc * scale_dev))
        if bad:
            print("     ⚠️  切动作时会看出来：" + "；".join(bad))
        else:
            print("     ✓ 和基准一致（上机误差都在 2px 内）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
