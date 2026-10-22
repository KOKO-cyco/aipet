#!/usr/bin/env python3
"""把 `屏幕右边缘吸附.png` 做成 pack_seq.py 要的 PNG 序列（snap / snap_l）。

    python third_party/sprite/make_snap_seq.py

产出：
    third_party/resources/fish_seq/snap/0001.png     贴右边时显示的那张
    third_party/resources/fish_seq/snap_l/0001.png   同上，左右镜像（贴左边用）

**为什么要单独一个脚本**
-----------------------
这两张不在 third_party/sprite/fish_extract.py 的 CLIPS 表里（它们不是从 webm 来的），
所以**任何 `rm -rf fish_seq/*` 都会把它们删掉，而且重跑 fish_extract 补不回来** ——
2026-10-14 就是这么把吸附搞坏的（固件里找不到 SNAP 组，贴边时角色不换图）。
现在 third_party/sprite/build_fish.sh 每次都会调这个脚本，删了也能自己长回来。

两个参数怎么定的
---------------
* **黑底抠透明**：源图是全不透明的（alpha 全 255，背景是纯黑），不是带 alpha 的
  素材。用亮度阈值抠 —— 实测非黑内容 x 34..170 y 15..252。
* **缩一半**：源图是按 **640x360** 那套素材的尺度做的（内容高 238），而
  fish_extract.py 把 webm 抽成 **320x180**（半尺度）。238 / 2 = 119，正好等于
  旧 .spf 里 snap 的内容高（105 ÷ 0.8759 = 119.9）—— 对得上，所以就是 2 倍。
"""

import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SRC = os.path.join(ROOT, "third_party/blue_big_fat_fish/屏幕右边缘吸附.png")
OUT = os.path.join(ROOT, "third_party/resources/fish_seq")

# 亮度低于这个算背景（纯黑）。取 40 —— 实测背景是 0，角色最暗的描边也在 100 以上。
LUMA_MIN = 40

# 缩到 fish_seq 的尺度（webm 是 640x360，fish_extract 抽成 320x180）
SCALE = 0.5

# 贴到 320x180 画布上的位置（左上角）。
#
# **必须贴到 320x180 上，不能直接用那张小图自己的坐标。** pack_seq.py 把每个
# 片段目录里的 PNG 当成同一个坐标系：webm 抽出来的帧都是 320x180 对齐的，而
# 屏幕右边缘吸附.png 只有 171x282（缩完 85x141）。直接用的话它的内容会落在
# 画布左上角 —— 上机表现为"贴边时角色整个往上窜"，而且把锚点框从 207 撑到 250。
#
# 这两个数怎么来的：对齐到旧 .spf 里的相对关系
#     旧 snap 相对 idle：dx +22、dy +8（输出像素）
#     除以缩放 0.8759 -> 源坐标 +25、+9
#     idle 的源内容左上角 (106, 30) -> snap 的源内容左上角应为 (131, 39)
#     小图里内容在 (17, 7) -> 画布位置 = (131-17, 39-7) = (114, 32)
PAD_X, PAD_Y = 114, 32
CANVAS_W, CANVAS_H = 320, 180


def main():
    if not os.path.isfile(SRC):
        sys.exit("找不到 %s" % SRC)

    im = Image.open(SRC).convert("RGBA")
    a = np.array(im)
    lum = a[:, :, :3].astype(np.int32).sum(axis=2)

    # 黑底 -> 全透明
    a[:, :, 3] = np.where(lum > LUMA_MIN, 255, 0).astype(np.uint8)

    w = max(1, int(round(im.size[0] * SCALE)))
    h = max(1, int(round(im.size[1] * SCALE)))
    img = Image.fromarray(a).resize((w, h), Image.LANCZOS)

    # 贴到 320x180 画布（透明底）
    canvas = Image.new("RGBA", (CANVAS_W, CANVAS_H), (0, 0, 0, 0))
    canvas.paste(img, (PAD_X, PAD_Y))
    img = canvas

    ys, xs = np.nonzero(np.array(img)[:, :, 3] >= 16)
    print("源 %dx%d -> 内容 %dx%d，贴到 %dx%d 画布的 (%d,%d)"
          % (im.size[0], im.size[1],
             xs.max() - xs.min() + 1, ys.max() - ys.min() + 1,
             CANVAS_W, CANVAS_H, PAD_X, PAD_Y))

    for name, flip in (("snap", False), ("snap_l", True)):
        d = os.path.join(OUT, name)
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):                 # 清掉旧帧，免得残留多帧
            if f.endswith(".png"):
                os.remove(os.path.join(d, f))
        out = img.transpose(Image.FLIP_LEFT_RIGHT) if flip else img
        p = os.path.join(d, "0001.png")
        out.save(p)
        print("  写出 %s" % os.path.relpath(p, ROOT))

    print("接着跑 third_party/sprite/build_fish.sh 重新打包")


if __name__ == "__main__":
    main()
