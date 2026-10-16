#!/usr/bin/env python3
"""绿幕 MP4 -> 带 alpha 的 VP9 WebM（和 blue_big_fat_fish 现有素材同规格）。

    python tools/green_key.py third_party/blue_big_fat_fish/待处理动作/思考.mp4

默认输出到源文件旁边（同名换 .webm），也可以 --out 指定。

为什么是 WebM/VP9
-----------------
VP9 有 alpha 侧数据，WebM 能装 —— 这是"视频带透明通道"目前唯一顺手的组合
（H.264 没有 alpha）。现有素材都是这么来的，所以这里产出同规格的东西，
fish_extract.py 那边一行不用改。

**解码时必须 `-c:v libvpx-vp9`**，否则 ffmpeg 的原生 vp9 解码器不读 alpha
平面 —— 详见 tools/fish_extract.py 的文件头。

抠像参数怎么定的
---------------
`--key` 是绿幕色。不传就从首帧自动估：取四角一圈像素的均值。实测素材的绿幕
不是纯 0x00FF00 而是约 RGB(98,184,71)，写死纯绿会抠不干净。

`--similarity` 是"多接近算背景"，默认 0.12。太小会留下绿边，太大会啃掉角色
身上的青绿（这条鱼的头发就是蓝绿系，所以别调大）。

`--despill` 把边缘残留的绿压掉（去溢色）。关掉的话，半透明的边缘像素会泛绿光。
"""

import argparse
import os
import shutil
import subprocess
import sys

# 输出规格 —— 与现有素材一致
OUT_W, OUT_H = 640, 360


def ffmpeg_bin():
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit("找不到 ffmpeg（PATH 里没有）")
    return exe


def guess_key_color(ff, src):
    """从首帧四角估绿幕色（十六进制 0xRRGGBB）。"""
    cmd = [ff, "-v", "error", "-i", src, "-frames:v", "1",
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0 or len(r.stdout) < 12:
        return None

    # 源尺寸未知，取前若干像素里最"绿"的那几个的平均 —— 绿幕一定占大片，
    # 首行前几十个像素基本都是背景。
    px = r.stdout[:64 * 3]
    n = len(px) // 3
    rs = sum(px[i * 3] for i in range(n)) / n
    gs = sum(px[i * 3 + 1] for i in range(n)) / n
    bs = sum(px[i * 3 + 2] for i in range(n)) / n
    return (int(round(rs)) << 16) | (int(round(gs)) << 8) | int(round(bs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", help="绿幕 mp4")
    ap.add_argument("--out", default=None, help="输出 .webm（默认同名换后缀）")
    ap.add_argument("--key", default=None,
                    help="绿幕色 0xRRGGBB；不传就从首帧自动估")
    ap.add_argument("--similarity", type=float, default=0.12,
                    help="多接近算背景（默认 0.12）。太大会啃掉角色身上的青绿")
    ap.add_argument("--blend", type=float, default=0.04,
                    help="边缘过渡宽度（默认 0.04），让抠像边不硬")
    ap.add_argument("--no-despill", action="store_true",
                    help="关掉去溢色（默认开）")
    ap.add_argument("--fps", type=int, default=24, help="输出帧率（默认 24）")
    ap.add_argument("--patch", action="append", default=[],
                    help="水印之类：先用底色盖掉一块矩形 X,Y,W,H（可重复）。"
                         "坐标是**当前分辨率**（用了 --pre 之后就是 --pre 输出那个尺寸）")
    ap.add_argument("--pre", default=None,
                    help="**抠像之前**插的滤镜。常用：把\"缩放\"放这里 —— "
                         "抠像必须放在缩放之后，否则缩放会把绿边重采样成光晕。"
                         "例：--pre \"scale=126:170\"")
    ap.add_argument("--post", default=None,
                    help="**抠像之后**插的滤镜。`pad` 铺画布必须放这里 —— "
                         "chromakey 会覆盖 alpha，pad 放前面会被它设成不透明。"
                         "例：--post \"pad=320:180:96:16:color=black@0\"")
    ap.add_argument("--size", default=None,
                    help="输出尺寸 WxH。不写按默认 %dx%d —— **竖屏素材一定要写**，"
                         "否则会被压扁" % (OUT_W, OUT_H))
    args = ap.parse_args()

    if not os.path.isfile(args.src):
        sys.exit("找不到 %s" % args.src)

    ff = ffmpeg_bin()

    key = args.key
    if key is None:
        c = guess_key_color(ff, args.src)
        if c is None:
            sys.exit("估不出绿幕色，用 --key 手动给一个（如 0x62B847）")
        key = "0x%06X" % c
        print("自动估出的绿幕色: %s" % key)

    out = args.out or (os.path.splitext(args.src)[0] + ".webm")

    vf = []
    # **抠像之前**先插的滤镜（`--pre`）。
    # 最常用的用法是把"缩放 + 铺画布"放这里 —— **抠像必须放在缩放之后**：
    # 反过来的话，缩放会把已经抠成半透明的绿边重采样，把绿糊进更多像素，
    # 上机就是角色周围一圈绿光晕（2026-10-14 实测：先抠后缩 边缘绿度 +177，
    # 先缩后抠 −5）。
    if args.pre:
        vf.append(args.pre)

    # 补丁（在**当前**分辨率上打）—— 用在"水印"这种"压在背景上、但角色够不到"
    # 的东西：直接拿底色盖掉，后面的 chromakey 一并抠走。比 delogo 稳 ——
    # delogo 是从四周插值，万一边缘有角色的像素就会被拖成一片糊。
    for r in args.patch:
        try:
            x, y, w, h = [int(v) for v in r.split(",")]
        except ValueError:
            sys.exit("--patch 要写成 X,Y,W,H，收到 %r" % r)
        vf.append("drawbox=x=%d:y=%d:w=%d:h=%d:color=%s@1:t=fill" % (x, y, w, h, key))

    if args.size:
        vf.append("scale=%s" % args.size)
    else:
        vf.append("scale=%d:%d" % (OUT_W, OUT_H))

    # 滤镜链顺序有讲究：
    #   --pre     先做"缩放"这类准备（**抠像必须在缩放之后**，见上面的注释）
    #   patch     用底色盖掉水印那类东西
    #   scale     缩到目标尺寸
    #   chromakey 抠像（在 YUV 域做，比 RGB 的 colorkey 稳）
    #   despill   压掉边缘残留的绿
    #   format    交给 vp9 编码器时带上 alpha
    #   --post    **铺画布（pad）必须放这里** —— 见下面的注释
    vf.append("chromakey=%s:%.3f:%.3f" % (key, args.similarity, args.blend))
    if not args.no_despill:
        vf.append("despill=type=green")
    vf.append("format=yuva420p")

    # **抠像之后**插的滤镜（`--post`）。
    #
    # `pad` 铺画布**必须放这里**：`chromakey` **会覆盖 alpha**（不是"在已有 alpha
    # 上再抠"）—— pad 出来的透明黑不是绿的，会被它强行设成不透明，整幅变成
    # "角色 + 一个不透明的大方块"（2026-10-14 实测：pad 放前面 76.8% 不透明，
    # 放后面才对）。
    if args.post:
        vf.append(args.post)

    # 曾经试过"把抠掉的地方 RGB 也刷黑"（好让不认 alpha 的播放器看到剪影而不是
    # 绿幕）。**放弃了**：那条链（lutyuv 压黑 + overlay + alphamerge 合回 alpha）
    # 在这版 ffmpeg 上产出的是垃圾 —— 角色变成绿色剪影、外面套一个白框
    # （2026-10-14 实测）。要看效果就用白底预览视频，别在这个文件上做文章。
    cmd = [ff, "-v", "warning", "-y", "-i", args.src,
           "-vf", ",".join(vf),
           "-r", str(args.fps),
           "-an",                                  # 不要音轨
           "-c:v", "libvpx-vp9",
           "-pix_fmt", "yuva420p",
           "-auto-alt-ref", "0",                   # **alpha 与 alt-ref 冲突**，必须关
           "-b:v", "0", "-crf", "30",
           "-row-mt", "1",
           out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("ffmpeg 失败:\n%s" % r.stderr.strip())

    print("写出 %s（%.1f MB）" % (out, os.path.getsize(out) / 1048576.0))
    print("自检: python tools/fish_extract.py --only <片段名>  然后用 pack_seq.py 打包")


if __name__ == "__main__":
    main()
