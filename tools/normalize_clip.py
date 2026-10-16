#!/usr/bin/env python3
"""把一段素材的**取景**对齐到另一段素材 —— 缩放 + 平移，让角色的内容框落在一起。

    python tools/normalize_clip.py --src video(1).webm \\
        --ref third_party/blue_big_fat_fish/idle/待机呼吸休闲.webm \\
        --canvas 320x180 --out idle_norm.webm

为什么需要
---------
`pack_seq.py` 算每组动作的 dx/dy 时，**假定所有片段共享同一套源坐标**：每组
只带自己内容框的裁剪，位置靠 dx/dy 还原。横屏那批素材都是 640x360、角色在画面
同一个位置，所以能对齐；一旦混进一段**竖屏**（720x960）或者角色在画面里站得
不一样高的素材，算出来的 dx/dy 就对不上 —— 上机表现为"切到那组动作时角色瞬移"。
顺带还会把基准高度（= 待机组的内容高）带偏，**整套角色的渲染尺寸一起变**。

做法
----
取两段素材各自内容框的并集（alpha >= 32），算出一个"缩放 + 平移"：
* **高对齐**：把 src 的内容缩放到和 ref 一样高（顺带把基准高度拉回去）
* **底对齐**：内容底边落在 ref 的底边（脚对脚，视觉上最要紧）
* **水平居中**：内容水平中心落在 ref 的中心

然后按 `--canvas` 铺画布（透明底），输出和横屏素材**同尺寸**的中间帧。
"""

import argparse
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


def content_box(ff, path, height, align="median"):
    """一段素材的内容框。

    align="union"  —— 整段所有帧的并集（老行为）
    align="median" —— 逐帧内容框的**中位**（新默认）

    为什么默认改成 median：并集高度**包含动作幅度** —— 一段"跳一下"的动作并集
    比站着高 20px，按并集对齐就会把它缩得更小，上机表现为"切到这个动作角色变小"。
    中位数把少数几帧的大幅动作剔掉，反映的是**角色本体的典型大小和站位**，
    这才是"角色多高、脚在哪"。

    @return (w, h, (x0, y0, x1, y1))
    """
    pr = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                         "-show_entries", "stream=width,height",
                         "-of", "csv=p=0", path], capture_output=True, text=True)
    sw, sh = [int(x) for x in pr.stdout.strip().split(",")[:2]]
    tw = max(2, int(round(sw * height / float(sh))))
    tw -= tw % 2
    r = subprocess.run([ff, "-v", "error", "-c:v", "libvpx-vp9", "-i", path,
                        "-vf", "scale=%d:%d" % (tw, height),
                        "-pix_fmt", "rgba", "-f", "rawvideo", "-"],
                       capture_output=True)
    n = len(r.stdout) // (tw * height * 4)
    a = np.frombuffer(r.stdout, dtype=np.uint8)[:n * tw * height * 4] \
          .reshape(n, height, tw, 4)
    al = a[:, :, :, 3] >= ALPHA_MIN

    if align == "union":
        ys, xs = np.nonzero(al.any(axis=0))
        return tw, height, (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))

    per = []
    for i in range(n):
        ys, xs = np.nonzero(al[i])
        if len(xs):
            per.append((xs.min(), ys.min(), xs.max(), ys.max()))
    if not per:
        return tw, height, (0, 0, 0, 0)
    # 中位数按**每个边分别取**（不是挑"中位那一帧"）：对边缘抖动更稳。
    x0 = int(np.median([b[0] for b in per]))
    y0 = int(np.median([b[1] for b in per]))
    x1 = int(np.median([b[2] for b in per]))
    y1 = int(np.median([b[3] for b in per]))
    return tw, height, (x0, y0, x1, y1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--ref", required=True, help="参照素材（横屏那批里的任意一段）")
    ap.add_argument("--out", required=True)
    ap.add_argument("--canvas", default="320x180")
    ap.add_argument("--align", choices=("median", "union"), default="median",
                    help="按哪个内容框对齐（默认 median = 逐帧中位，见 content_box 的说明）")
    args = ap.parse_args()

    ff = ffmpeg_bin()
    cw, ch = [int(x) for x in args.canvas.lower().split("x")]

    rw, rh, rb = content_box(ff, args.ref, ch, args.align)
    sw, sh, sb = content_box(ff, args.src, ch, args.align)
    print("参照 %dx%d 内容 %s（高%d 底%d 中%.0f）"
          % (rw, rh, rb, rb[3] - rb[1] + 1, rb[3], (rb[0] + rb[2]) / 2.0))
    print("源   %dx%d 内容 %s（高%d 底%d 中%.0f）"
          % (sw, sh, sb, sb[3] - sb[1] + 1, sb[3], (sb[0] + sb[2]) / 2.0))

    # 源素材的原始尺寸（缩放要在它上面做，避免多一次重采样）
    pr = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                         "-show_entries", "stream=width,height",
                         "-of", "csv=p=0", args.src], capture_output=True, text=True)
    ow, oh = [int(x) for x in pr.stdout.strip().split(",")[:2]]

    # 目标内容高 / 底 / 水平中心（都在画布坐标里）
    tgt_h = rb[3] - rb[1] + 1
    tgt_bot = rb[3]
    tgt_mid = (rb[0] + rb[2]) / 2.0

    # 源内容框换算回原始分辨率
    k = oh / float(sh)
    sx0, sy0, sx1, sy1 = [v * k for v in sb]
    src_h = sy1 - sy0 + 1

    # 目标内容高 / 底 / 水平中心（都在画布坐标里）—— 这三个是"对齐到什么"的定义，
    # 由 ref 决定，自检修正循环里不变。

    def build(f, dox=0, doy=0):
        """按缩放比 f 算出滤镜串。dox/doy 是自检测出来的平移修正量。

        **位置优先、裁剪配合**：先按"内容底边要落在 tgt_bot、水平中心落在 tgt_mid"
        算出 pad 偏移，再反过来决定裁多少 —— 裁的都是**透明边**（内容已经对齐到
        ref 了，角色本体之外一定是空的）。

        为什么不能直接 `pad` 完事：pad 只能把整幅塞进画布，位置一顶到边就被夹住。
        实测那样脚底会偏高 5px（2026-10-16）。竖屏素材按内容高缩放后仍然比 16:9
        画布高（720x960 缩完还是 3:4），非裁不可 —— 不裁的话 ffmpeg 的 pad 还会
        因为"画布比源小"直接报错。
        """
        nw = max(2, int(round(ow * f)))
        nh = max(2, int(round(oh * f)))
        nw -= nw % 2
        nh -= nh % 2
        cx0, cy0 = sx0 * f, sy0 * f
        cx1, cy1 = sx1 * f, sy1 * f

        # 内容本身放不进画布就是真的出问题了（角色比目标还大，或者横向超宽）。
        if (cx1 - cx0) > cw or (cy1 - cy0) > ch:
            sys.exit("内容 %.0fx%.0f 放不进画布 %dx%d —— 调大 --canvas，或者先裁源素材"
                     % (cx1 - cx0, cy1 - cy0, cw, ch))

        # 想要的 pad 偏移（可能为负 —— 负号表示"先裁掉再放"）
        ox = int(round(tgt_mid - (cx0 + cx1) / 2.0)) + dox
        oy = int(round(tgt_bot - cy1)) + doy

        cxp = max(0, -ox)
        cxp -= cxp % 2
        cyp = max(0, -oy)
        cyp -= cyp % 2
        ox = max(0, ox + cxp)
        oy = max(0, oy + cyp)

        cw2 = min(nw - cxp, cw - ox)
        cw2 -= cw2 % 2
        ch2 = min(nh - cyp, ch - oy)
        ch2 -= ch2 % 2

        # 裁完之后内容必须还在框里，否则就是真放不下
        if (cx0 < cxp or cx1 > cxp + cw2 - 1 or cy0 < cyp or cy1 > cyp + ch2 - 1
                or cw2 < 2 or ch2 < 2):
            sys.exit("内容 x %.0f..%.0f / y %.0f..%.0f 在画布 %dx%d 里放不下"
                     "（裁到 %.0fx%.0f@(%d,%d) 装不进去）—— 调大 --canvas"
                     % (cx0, cx1, cy0, cy1, cw, ch, cw2, ch2, cxp, cyp))

        if cw2 == nw and ch2 == nh:
            return "scale=%d:%d,pad=%d:%d:%d:%d:color=black@0" % (nw, nh, cw, ch, ox, oy)

        return ("scale=%d:%d,crop=%d:%d:%d:%d,pad=%d:%d:%d:%d:color=black@0"
                % (nw, nh, cw2, ch2, cxp, cyp, cw, ch, ox, oy))

    # 量的时候按**画布高**缩放、输出的时候按**目标高**缩放 —— 两次重采样的 alpha
    # 边缘不一致，量出来的框就会偏。实测偏差不小：待机角色高大 7%、开心小 11%
    # （2026-10-16 用逐帧中位量出来的，单看一帧看不出来）。
    # 所以写完**再量一遍输出**，大小和位置都按偏差修，最多三轮。
    f = tgt_h / src_h
    dox = doy = 0

    for attempt in (1, 2, 3):
        vf = build(f, dox, doy)
        print("归一化%s: %s" % ("" if attempt == 1 else "（第 %d 轮修正）" % attempt, vf))

        cmd = [ff, "-v", "warning", "-y", "-c:v", "libvpx-vp9", "-i", args.src,
               "-vf", vf, "-an",
               "-c:v", "libvpx-vp9", "-pix_fmt", "yuva420p",
               "-auto-alt-ref", "0", "-b:v", "0", "-crf", "28", "-row-mt", "1",
               args.out]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("ffmpeg 失败:\n%s" % r.stderr.strip())

        if attempt == 3:
            break

        # 输出的高就是 ch，所以 content_box 量出来的就是**输出自己的像素**，
        # 和 tgt_* 同一套坐标，可以直接比。
        _, _, ob = content_box(ff, args.out, ch, args.align)
        got_h = ob[3] - ob[1] + 1
        dh = tgt_h - got_h
        dx = int(round(tgt_mid - (ob[0] + ob[2]) / 2.0))
        dy = int(round(tgt_bot - ob[3]))

        if abs(dh) <= 1 and abs(dx) <= 1 and abs(dy) <= 1:
            break

        print("  自检: 角色高 %+d、中心x %+d、底边y %+d px —— 修正后重写" % (dh, dx, dy))
        if got_h > 0:
            f = f * (got_h + dh) / float(got_h)     # 按比例修缩放
        dox, doy = dx, dy
        vf = ("scale=%d:%d,crop=%d:%d:%d:%d,pad=%d:%d:%d:%d:color=black@0"
              % (nw, nh, cw2, ch2, cxp, cyp, cw, ch, ox, oy))

    print("写出 %s（%.1f MB）" % (args.out, os.path.getsize(args.out) / 1048576.0))


if __name__ == "__main__":
    main()
