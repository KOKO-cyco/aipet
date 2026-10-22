#!/usr/bin/env python3
"""把 PNG **序列**（一个动作一个目录）打包成固件能直接读的 SPF7 文件。

和 pack_frames.py 的分工
------------------------
    pack_frames.py   一张网格图集（2D chibi 素材）-> .spf
    pack_seq.py      一堆 PNG 序列目录（3D 渲染产物）-> .spf

两者写出来的 .spf 格式**完全一样**（见下面），固件侧读的是同一个格式，不用改。
之所以要两个脚本：图集版靠"行内连通段 + 最小二乘拟合步距"切格，序列版每帧一个
文件、天然对齐，用不上那套。

为什么不能直接用 pack_frames.py 凑合
------------------------------------
它按"一行 = 一个动作、192px 固定步距"切。3D 每个动作 200~350 帧，拼成一行就是
几万像素宽的图 —— PIL 能开，但 16 行叠起来是几百 MB 的中间图，没必要。

两个必须守的约束
----------------
1. **缩放按角色实际高度，不按画布高度。** 源 PNG 是 315x438 的固定画布，角色
   只占其中 102x316（四周是透明留白）。按画布缩会把角色缩得比预期小一大截。
2. **抽帧在打包时做。** 3D 素材是 30fps 渲的，浮层用 15fps 足够；3866 帧全留
   会让 .spf 体积翻倍。

文件格式（小端）—— **SPF7**
---------------------------
    [magic "SPF7"][u16 clip_count][u16 reserved]
    [u16 anchor_w][u16 anchor_h]              <- 锚点框：所有动作内容框的并集
    [clip_count x {
         u16 frames, u16 fps,
         u16 w, u16 h,                        <- **本组自己的帧尺寸**
         s16 dx, s16 dy,                      <- 本组裁剪框相对锚点框的偏移
         u16 role                             <- 0待机 1点击 2拖拽 3随机 …
    }]
    [名字表：clip_count x { u8 len, ASCII 字节 }，整体补 0 到 4 字节对齐]
    [u32 offs[total_frames + 1]               <- 每帧 RLE 数据在文件里的偏移
    [total_frames 段 PackBits 数据]

逐版演进（每一版都只加不改，固件只认最新版）：
  * **SPF3** 全图集一个帧尺寸 + 一个 u8 内容框。"表演类"素材里只要有一个片段铺满
    整帧（带道具/特效的），全局框就被撑到那么大，**所有**片段一起变胖；而且那个框
    的坐标是 u8，超过 255 直接打包失败。
  * **SPF4** 改成每组各自裁剪，锚点仍是全局并集的左上角 —— 切动作时角色不会跳。
  * **SPF6** 帧数据从裸 ARGB4444 改成 PackBits RLE，并加一张偏移表。文件小一半、
    SD 读少一半，待机组才装得进 PSRAM。
  * **SPF7** 加名字表。AI 桥接和 MCP 工具是**按名字点名**播动作的，而记录里只有
    role 枚举；名字进文件之后固件按名字找组号，`--order` 改了也不会静默错位。

为什么不是 SPF3（全图集一个帧尺寸 + 一个 u8 内容框）：
  * "表演类"素材里只要有一个片段铺满整帧（带道具/特效的），全局框就被撑到那么大，
    **所有**片段一起变胖；
  * 那个框的坐标是 u8，超过 255 直接打包失败。
SPF4 改成每组各自裁剪，锚点仍然是全局并集的左上角 —— 所以切动作时角色不会跳。

用法：
    python third_party/sprite/pack_seq.py \
        --src third_party/resources/png_sequences \
        --out third_party/sprite/anim.spf \
        --height 148 --fps 15 --pad 4 --idle-frames 48
"""

import argparse
import glob
import os
import struct
import sys

import numpy as np
from PIL import Image

# ARGB4444 的 alpha 只有 4 位 —— 量化后为 0 的像素等于全透明。判定"角色在哪"
# 必须用同一个阈值，否则会把极淡的噪点算进包围盒（盒子虚胖，缩放比就偏小）。
ALPHA_MIN = 16

# 动作组在固件里扮演什么角色 —— 写进每组记录的那个 u16。
# 固件按它决定"什么时候播哪一组"：待机循环、点击回应、拖拽中、随机表演。
#
# 由**名字前缀**推出来，这样加片段只要按前缀命名就行，不用再改一张映射表：
#     idle*  -> 0 待机（第 0 组必须待机：它整组常驻 PSRAM）
#     click* -> 1 点击回应
#     drag*  -> 2 拖拽中
#     其它   -> 3 随机表演
#     snap*  -> 4 贴右边缘时显示的那张（素材已按右边缘裁好）
#     snap_l*-> 5 同上，左边缘用的镜像版
#     drag_up*  -> 6 拖拽起手：地上 -> 飘起来，**演一遍**
#     drag_down*-> 7 松手落地：飘 -> 落回地面，**演一遍**
#     drag*     -> 2 拖拽中：悬空**循环**（手按着的时候）
#
# 拖拽是**状态驱动**的，不是"播一段固定长度的片子"：按下 -> 起手 -> 悬空循环 ->
# 松手 -> 落地。所以它是三组，不是一组。名字前缀里 drag_up/drag_down 必须排在
# drag 前面判断，否则会被 drag 吃掉。
ROLE_IDLE, ROLE_CLICK, ROLE_DRAG, ROLE_RANDOM = 0, 1, 2, 3
ROLE_SNAP_R, ROLE_SNAP_L = 4, 5
ROLE_DRAG_UP, ROLE_DRAG_DOWN = 6, 7
ROLE_TALK = 8
ROLE_AI = 9     # AI 表情（开心 / 思考）：只由角色页按名字点名播，
                # **不进精灵的随机池** —— 否则桌宠自己就会演起"开心/思考"


def role_of(name):
    if name.startswith("snap_l"):
        return ROLE_SNAP_L
    if name.startswith("snap"):
        return ROLE_SNAP_R
    if name.startswith("drag_up"):
        return ROLE_DRAG_UP
    if name.startswith("drag_down"):
        return ROLE_DRAG_DOWN
    if name.startswith("idle"):
        return ROLE_IDLE
    if name.startswith("click"):
        return ROLE_CLICK
    if name.startswith("drag"):
        return ROLE_DRAG
    if name.startswith("talk"):
        return ROLE_TALK
    if name.startswith("happy") or name.startswith("think") or name.startswith("sad"):
        return ROLE_AI
    return ROLE_RANDOM


# 渲染出来就是 30fps（vrma_to_png.py 的默认值）。抽帧按它换算。
SRC_FPS_DEFAULT = 30

# 默认动作顺序。**第 0 个必须是待机** —— 固件把 clip 0 整组常驻 PSRAM
# （app_sprite2d.c 的 SP2D_IDLE_CLIP），它是读取失败时的兜底画面，也是
# 常态循环的那一组。其余按下面的顺序首尾相接循环播放。
DEFAULT_ORDER = [
    "idle_normal",
    "happy_idle",
    "joy",
    "wave",
    "excited",
    "look_around",
    "thinking",
    "standing_greeting",
    "model_pose",
    "show",
    "shoot",
    "squat",
    "crying",
    "idle_boring",
    "bier",
    "say_hello",
]


def argb4444(arr):
    """RGBA uint8 数组 -> ARGB4444 uint16 数组。"""
    r = (arr[..., 0] >> 4).astype(np.uint16)
    g = (arr[..., 1] >> 4).astype(np.uint16)
    b = (arr[..., 2] >> 4).astype(np.uint16)
    a = (arr[..., 3] >> 4).astype(np.uint16)
    return (a << 12) | (r << 8) | (g << 4) | b


def alpha_bbox(path):
    """一帧里角色（alpha >= ALPHA_MIN）的包围盒；全透明返回 None。"""
    al = np.array(Image.open(path).convert("RGBA"))[:, :, 3]
    ys, xs = np.nonzero(al >= ALPHA_MIN)
    if len(xs) == 0:
        return None
    return int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max())


def align_point(fbs, mode):
    """一个动作的"站位参考点" (中心x, 底边y)，源像素。

    pack_seq 的位置走的是**全局仿射**（源坐标 -> 输出），也就是"源里差几像素，
    上机就差几像素"—— 它**不做**跨动作的站位校正。所以各段素材要是各自把角色
    摆在画面的不同高度/左右，切动作时角色就会跳（2026-10-16 用户报的
    "待机偏向右上"就是这个）。

    这里量出每段的站位，让 --align 把它平移到基准动作的同一点上。

    median 取逐帧中位而不是并集：并集会被"动作幅度"和"起手/收势那一两帧"带偏
    （比如待机的并集底边跟它静止时的脚底一样，但"思考"的并集底边比它常态低 3px）。
    """
    if not fbs:
        return None
    if mode == "first":
        b = fbs[0]
        return (b[0] + b[1]) / 2.0, float(b[3])
    return (float(np.median([(b[0] + b[1]) / 2.0 for b in fbs])),
            float(np.median([b[3] for b in fbs])))


def frames_of(action_dir):
    """一个动作目录下的帧文件，按名字排序（0001, 0002, ...）。"""
    return sorted(glob.glob(os.path.join(action_dir, "*.png")))

def decimate(files, src_fps, dst_fps):
    """均匀抽帧：30fps -> 15fps 就是隔一个取一个。"""
    if dst_fps >= src_fps:
        return files
    step = src_fps / float(dst_fps)
    out, i, acc = [], 0.0, 0
    while int(round(acc)) < len(files):
        out.append(files[int(round(acc))])
        acc += step
        i += 1
    return out


def sample_evenly(files, n):
    """把一串帧均匀采样成 n 帧（首尾都取）。用来给常驻的待机组压帧数。"""
    if n >= len(files) or n <= 0:
        return files
    idx = np.linspace(0, len(files) - 1, n).round().astype(int)
    return [files[i] for i in sorted(set(idx.tolist()))]


def packbits(px):
    """uint16 像素数组 -> PackBits 字节流（小端）。

    控制字节 c：
        0..127   : 后面跟 c+1 个**字面**像素（2 字节/个）
        129..255 : 后面跟 1 个像素，**重复 257-c 次**
        （128 不用 —— 编码器不会产生）

    为什么值得做：帧序列里 40% 以上是"整片透明"，那是极长的同值行程，RLE 一压就没；
    角色本体是精细画面，走字面量，代价约 2.02 字节/像素（和裸存一样）。
    实测整体压到 **45%**，而且**解压就是一个循环** —— 不用引任何解码器。
    """
    out = bytearray()
    i = 0
    n = len(px)
    while i < n:
        j = i + 1
        while j < n and j - i < 128 and px[j] == px[i]:
            j += 1
        if j - i >= 3:                       # 够长才值得当行程
            out.append(257 - (j - i))
            out += px[i:i + 1].astype("<u2").tobytes()
            i = j
            continue
        k = i                                # 字面量段：一直吃到"下一个行程 >= 3"之前
        while k < n and k - i < 128:
            m = k + 1
            while m < n and m - k < 128 and px[m] == px[k]:
                m += 1
            if m - k >= 3:
                break
            k = m
        # **必须截到 128**（2026-10-14 修）：`k = m` 在"长度恰好为 2 的游程"上会
        # 一次前进 2，于是 k-i 能从 127 直接跳到 129，控制字节写出 **128** ——
        # 而 128 在协议里是保留值（字面量 ≤127、重复 ≥129），解码端会把它读成
        # "重复 129 次"，从这一帧的中段开始整段错乱。
        # 触发条件很窄（要刚好 127 个字的字面量后面接一个 2 长游程），实测
        # rnd_dinner 的 3/161 帧中招 —— 逐像素自检（--verify）就是为了抓它。
        if k - i > 128:
            k = i + 128
        out.append(k - i - 1)
        out += px[i:k].astype("<u2").tobytes()
        i = k
    return bytes(out)


def unpackbits(src, n):
    """packbits() 的对偶：解出 n 个 uint16。返回 (数组, 实际解出的个数)。"""
    out = np.empty(n, dtype=np.uint16)
    o, i = 0, 0
    ln = len(src)
    while i < ln and o < n:
        c = src[i]
        i += 1
        if c < 128:
            k = min(c + 1, n - o)
            out[o:o + k] = np.frombuffer(src, dtype="<u2", count=k, offset=i)
            i += 2 * k
            o += k
        else:
            k = min(257 - c, n - o)
            out[o:o + k] = np.frombuffer(src, dtype="<u2", count=1, offset=i)[0]
            i += 2
            o += k
    return out, o


def spf7_verify(buf, clips):
    """自检：把刚写出的文件解回来，和内存里的 clips **逐像素**比。

    为什么要有它：`unpackbits()` 和 `packbits()` 是分开写的两份实现，
    `unpackbits()` 又是固件 `spf6_inflate()` 的 Python 对照 —— 对不上就说明
    有一边错了。格式每次升版（SPF3→4→6→7）都靠它兜底，否则错误只会以
    "角色画出来不对"的形式在设备上出现。

    返回不一致的处数。
    """
    bad = 0

    if buf[:4] != b"SPF7":
        print("  !! magic 不是 SPF7")
        return 1

    nc, _ = struct.unpack_from("<HH", buf, 4)
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
    if list(offs) != sorted(offs):
        print("  !! 偏移表不是单调的")
        bad += 1
    if offs[-1] != len(buf):
        print("  !! 偏移表最后一项 %d != 文件长度 %d" % (offs[-1], len(buf)))
        bad += 1

    starts, acc = [], 0
    for r in recs:
        starts.append(acc)
        acc += r[0]

    for i, (name, fr, fps, w, h, dx, dy) in enumerate(clips):
        r = recs[i]
        if names[i] != name:
            print("  !! %s: 名字表读到 %s" % (name, names[i]))
            bad += 1
        if (r[0], r[1], r[2], r[3], r[4], r[5]) != (len(fr), fps, w, h, dx, dy):
            print("  !! %s: 记录 %s != 内存 (%d,%d,%d,%d,%d,%d)"
                  % (name, r[:6], len(fr), fps, w, h, dx, dy))
            bad += 1
            continue
        for j in range(len(fr)):
            b = starts[i] + j
            px, got = unpackbits(buf[offs[b]:offs[b + 1]], w * h)
            want = fr[j].reshape(-1)
            if got != w * h:
                print("  !! %s 帧 %d: 只解出 %d/%d 像素" % (name, j, got, w * h))
                bad += 1
            elif not np.array_equal(px, want):
                d = int((px != want).sum())
                first = int(np.nonzero(px != want)[0][0])
                stored = bytes(buf[offs[b]:offs[b + 1]])
                fresh = packbits(want)
                print("  !! %s 帧 %d: %d 个像素不一致（首个 @%d）"
                      "  存储 blob %d 字节 / 重打包 %d 字节%s"
                      % (name, j, d, first, len(stored), len(fresh),
                         "  <- 长度不同！" if len(stored) != len(fresh) else ""))
                bad += 1

    print("  自检：%d 组 %d 帧，锚点框 %dx%d，不一致 %d 处"
          % (nc, total, struct.unpack_from("<HH", buf, 8)[0],
             struct.unpack_from("<HH", buf, 8)[1], bad))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="png_sequences 目录")
    ap.add_argument("--out", required=True, help="输出的 .spf")
    ap.add_argument("--height", type=int, default=148,
                    help="角色**实际高度**（不是帧高）的目标像素数")
    ap.add_argument("--ref-action", default="idle_normal",
                    help="用哪个动作的高度当'角色高度'的基准。默认待机 —— 用户说的"
                         "'角色多高'指的是站着的高度，不是所有动作的并集"
                         "（3D 动作幅度差异大，按并集算待机会矮一截）")
    ap.add_argument("--fps", type=int, default=15, help="输出帧率")
    ap.add_argument("--src-fps", type=int, default=SRC_FPS_DEFAULT)
    ap.add_argument("--pad", type=int, default=4, help="角色四周留的透明边（像素）")
    ap.add_argument("--max-w", type=int, default=288,
                    help="单个动作的帧宽上限（固件 SP2D_MAX_W）")
    ap.add_argument("--max-h", type=int, default=208,
                    help="单个动作的帧高上限（固件 SP2D_MAX_H）")
    ap.add_argument("--idle-frames", type=int, default=48,
                    help="待机组最多保留多少帧（它整组常驻 PSRAM，帧数直接吃内存）")
    ap.add_argument("--shrink-default", type=int, default=0, metavar="像素",
                    help="所有动作的角色高再缩小这么多像素（--height 是缩小前的基准）")
    ap.add_argument("--shrink", action="append", default=[], metavar="名字=像素",
                    help="某个动作单独缩小这么多像素，覆盖 --shrink-default（可重复），"
                         "例：--shrink idle=10")
    ap.add_argument("--cap", action="append", default=[], metavar="名字=宽x高",
                    help="某个动作的裁剪窗口上限（**输出像素**，可重复），超出部分绕角色"
                         "站位点居中裁掉。给'带铺满全幅特效'的表演动作用 —— 特效会把该组"
                         "裁剪框撑大，连锚点框（屏幕画布大小）一起撑，角色本身不受影响。"
                         "例：--cap rnd_photo=160x206")
    ap.add_argument("--order", default=None,
                    help="动作顺序，逗号分隔；默认用内置表，第 0 个是待机")
    ap.add_argument("--align", choices=("off", "first", "median"), default="median",
                    help="跨动作对齐：把每个动作的'站位参考点'平移到基准动作的同一点，"
                         "切动作时角色不会左右/上下跳。median=逐帧中位（默认，抗起手帧），"
                         "first=首帧（切过去的第一眼），off=不对齐（各动作按源位置画）")
    ap.add_argument("--align-ref", default=None,
                    help="对齐到哪个动作的站位，默认 = --ref-action（待机）")
    ap.add_argument("--fit", choices=("off", "follow", "first", "median"),
                    default="follow",
                    help="跨动作尺寸归一：把每个动作的角色本体都画成 "
                         "--height - shrink 那么高。基准可选 follow（跟着 --align，"
                         "默认）/ first（首帧）/ median（逐帧中位）/ off（老行为，"
                         "所有动作共用一个缩放比）")
    ap.add_argument("--preview", default=None, help="每个动作抽一帧拼成预览 PNG")
    ap.add_argument("--verify", action="store_true",
                    help="写完之后把文件解回来逐像素自检（慢，但格式升版后必跑）")
    args = ap.parse_args()

    order = args.order.split(",") if args.order else DEFAULT_ORDER

    # ---- 挑出实际存在的动作，并抽好帧 ----
    plan = []          # [(name, [帧文件...], fps)]
    for name in order:
        d = os.path.join(args.src, name)
        files = frames_of(d)
        if not files:
            print("  跳过 %-18s （没有 PNG）" % name)
            continue

        if 0 == len(plan):
            # 待机组：抽到 idle_frames 以内，**fps 按比例降**以保住原时长
            # （整组常驻 PSRAM，帧数是硬预算；时长变了待机会看起来在快进）
            kept = sample_evenly(decimate(files, args.src_fps, args.fps),
                                 args.idle_frames)
            dur = len(files) / float(args.src_fps)
            fps = max(1, int(round(len(kept) / dur))) if dur > 0 else args.fps
        else:
            kept = decimate(files, args.src_fps, args.fps)
            fps = args.fps

        plan.append((name, kept, fps))
        print("  %-18s %4d 帧 -> 保留 %4d @%dfps" % (name, len(files), len(kept), fps))

    if not plan:
        print("没找到任何动作")
        return 1

    # ---- 第一遍：全量扫描 ----
    # 三个盒子，别混：
    #   * **全局并集**（所有动作）-> 全局锚点（缩放的零点）和夹紧框
    #   * **本动作并集**          -> 这个动作自己的裁剪框。**SPF4 的核心**：
    #                              每个动作只带自己的内容，谁也不拖累谁
    #   * **基准动作**（默认待机）-> 缩放比，用户说的"角色多高"是站着的高度
    #
    # 为什么以前是"一个全局框"：3D 角色站得稳，各动作包围盒差不多，一个框就够，
    # 而且逐帧裁剪会让角色抖动。但"表演类"素材（带道具/特效）里只要有一个片段
    # 铺满整帧，全局框就会被撑到那么大 —— 所有片段一起变胖，而且 SPF3 的框坐标
    # 是 u8，超过 255 直接打包失败。改成每动作各自裁剪之后，**锚点仍然是全局并集
    # 的左上角**，所以切动作时角色不会跳。
    gx0, gx1, gy0, gy1 = 10 ** 9, -1, 10 ** 9, -1
    rx0, rx1, ry0, ry1 = 10 ** 9, -1, 10 ** 9, -1
    boxes = {}
    fboxes = {}                 # 逐帧包围盒，跨动作对齐用（见下面的 --align）
    for name, files, _ in plan:
        cx0s, cx1s, cy0s, cy1s = 10 ** 9, -1, 10 ** 9, -1
        fbs = []
        for p in files:
            bb = alpha_bbox(p)
            if bb is None:
                continue
            x0, x1, y0, y1 = bb
            fbs.append(bb)
            cx0s, cx1s = min(cx0s, x0), max(cx1s, x1)
            cy0s, cy1s = min(cy0s, y0), max(cy1s, y1)
            gx0, gx1 = min(gx0, x0), max(gx1, x1)
            gy0, gy1 = min(gy0, y0), max(gy1, y1)
            if name == args.ref_action:
                rx0, rx1 = min(rx0, x0), max(rx1, x1)
                ry0, ry1 = min(ry0, y0), max(ry1, y1)
        boxes[name] = (cx0s, cx1s, cy0s, cy1s)
        fboxes[name] = fbs

    if gx1 < 0:
        print("所有帧都是空的")
        return 1

    if ry1 < 0:
        print("!! 找不到基准动作 %s，退回按并集高度缩放" % args.ref_action)
        rx0, rx1, ry0, ry1 = gx0, gx1, gy0, gy1

    r_h = ry1 - ry0 + 1
    scale = args.height / float(r_h)
    pad_src = args.pad / scale

    sh_map = {}
    for s in args.shrink:
        k, _, v = s.partition("=")
        sh_map[k.strip()] = int(v)

    def shrink_px(name):
        return sh_map.get(name, args.shrink_default)

    # 全局锚点 = 并集左上角，缩放后即 (0,0)。夹紧框就是并集本身。
    #
    # 注意这是**源坐标下的并集**推出来的，和各片段裁剪框（含 pad、还各自取整）
    # 的并集会有几像素出入。下面按各片段的实际矩形再归一化一次，保证锚点框
    # 严格等于所有片段的最小外接 —— 否则夹紧时会差几个像素，角色贴边时
    # 会露出/切掉一条。
    gw_src = int(round((gx1 - gx0 + 1) * scale))
    gh_src = int(round((gy1 - gy0 + 1) * scale))

    print("\n基准动作 %s 高 %d -> 缩放 %.4f（角色高 %d）"
          % (args.ref_action, r_h, scale, args.height))

    # ---- 跨动作站位对齐 + 尺寸归一 ----
    # 位置走全局仿射（见 align_point 的说明），也就是"源里差几像素，上机就差几像素" ——
    # 它**不做**跨动作的校正。素材是各段单独生成的，角色在源画面里的位置和大小并不
    # 完全一致，切动作时就会"抽一下"。用户 2026-10-16 明确要求：
    #
    #     "角色从待机切换到别的动作，角色位置、大小要完全一样"
    #
    # 所以这里做两件事：
    #   位置 —— 每个动作的站位参考点平移到基准动作的同一点；
    #   大小 —— 每个动作再乘一个系数，让**角色本体**画成同样高（见 scale_of）。
    #
    # 参考点 = 逐帧中位底边中点。不用并集：并集会被动作幅度和起手/收势那一两帧
    # 带偏（think 的并集底边比它常态低 3px，rnd_whale 的并集中心比常态偏左 6px）。
    align_ref = args.align_ref or args.ref_action

    med_h = {}                  # 逐帧中位**角色高**（源像素），尺寸归一的依据之一
    first_h = {}                # 首帧角色高 —— 切过去第一眼看到的那个
    pivot = {}                  # 站位参考点 (中心x, 底边y)，源像素
    for name, _, _ in plan:
        fbs = fboxes[name]
        bx0s, bx1s, by0s, by1s = boxes[name]
        if bx1s < 0:                        # 整段全透明：退回全局框
            bx0s, bx1s, by0s, by1s = gx0, gx1, gy0, gy1
        # alpha_bbox 给的是 (xmin, xmax, ymin, ymax) —— 高是 ymax - ymin
        med_h[name] = float(np.median([b[3] - b[2] + 1 for b in fbs])) if fbs else 0.0
        first_h[name] = float(fbs[0][3] - fbs[0][2] + 1) if fbs else 0.0
        p = align_point(fbs, args.align) if args.align != "off" else None
        pivot[name] = p if p else ((bx0s + bx1s) / 2.0, float(by1s))

    # 尺寸归一用哪套"角色高"。**默认跟着 --align** —— 两件事量的是同一个问题
    # （"切过去的那一眼对不对得上"），基准不一致的话会出现"位置按首帧对齐了、
    # 大小还按中位归一"这种自相矛盾的结果。
    #
    # 首帧 vs 中位（2026-10-16 用户选的"首帧"）：切动作时看到的是**首帧**，
    # 所以首帧对齐才能保证"切过去不跳"。中位反映的是整段的常态，对循环播的
    # 动作更稳，但会被"缩着/张开"的姿势带偏 —— 伤心那段的逐帧中位高是 128
    # （常态是缩着的），而它首帧是 133，按中位归一会把它放大 3.9%。
    fit_basis = args.align if args.fit == "follow" else args.fit
    ref_h = {"median": med_h, "first": first_h}.get(fit_basis, {})

    target = None               # 基准动作的站位参考点（源坐标）
    if args.align != "off":
        target = pivot.get(align_ref)
        if target is None:
            print("!! 对齐基准 %s 没有可用帧，退回不对齐" % align_ref)
        else:
            print("  站位对齐 + 尺寸归一（--align %s，--fit %s，基准 %s "
                  "中心x=%.1f 底边y=%.1f）"
                  % (args.align, args.fit, align_ref, target[0], target[1]))
            for name, _, _ in plan:
                pcx, pbot = pivot[name]
                h = ref_h.get(name) or 0.0
                print("    %-18s 中心x %6.1f 底边y %6.1f -> 平移 (%+6.1f,%+6.1f)  "
                      "角色高 %5.1f -> 缩放 x%.4f"
                      % (name, pcx, pbot, target[0] - pcx, target[1] - pbot,
                         h, (args.height - shrink_px(name)) / h if h else 0))

    # ---- 第二遍：按动作各自裁剪 + 缩 + 转 ARGB4444 ----
    #
    # **位置和大小分开算**（2026-10-14 加 --shrink）：
    #   位置走"未缩小的参照仿射"（就是以前那套：源坐标 -> 输出），
    #   大小走各自动作自己的 scale_c = (height - shrink) / 本动作中位角色高。
    # 并且以**站位参考点**（底边中点）为锚点 —— 缩小时角色原地变小，不会漂。
    #
    # 锚点框也仍然按**未缩小**的矩形取：固件的初始位置是
    # `panel - 锚点框 - 8`（右下角留边），锚点框一变，开机位置就跟着变。
    def scale_of(name):
        """这个动作画多大（输出像素 / 源像素）。

        两层：
          1. `--height` 是**角色在屏上的目标高度**，`--shrink` 再减几像素；
          2. 除以**本动作自己的**角色高（`--fit`，默认跟着 `--align`）。

        第 2 层就是尺寸归一。老写法是 `scale × (height - shrink) / height`，
        等价于"所有动作共用同一个缩放比" —— 素材是各段单独生成的，角色在源画面里
        本来就大小不一，共用缩放比等于把差异原样搬上屏，切过去就"胀一下"。
        改成按各自动作的角色高算，每个动作画出来的**角色本体**都是
        `--height - shrink` 像素高。

        量的是首帧还是逐帧中位由 `--fit` 定，理由见上面 ref_h 那段注释。
        """
        h = ref_h.get(name) or 0.0
        if h <= 0:
            return scale * max(0.05, (args.height - shrink_px(name)) / float(args.height))
        return max(0.05, (args.height - shrink_px(name)) / h)

    # ---- 单动作裁剪窗口上限（--cap）----------------------------------------
    # 表演素材里偶尔有"铺满整幅"的特效帧 —— 拍照那段的白色闪光星爆宽 513px
    # （源画幅 554 的 93%），它把 rnd_photo 这一组的裁剪框撑到 321x199：
    #   1) 超过固件单帧上限（SP2D_MAX_W/H），这组会被拒绝加载；
    #   2) 更麻烦的是锚点框 —— 它就是**屏幕上的画布大小**，会从 160 被撑到 352，
    #      比屏还宽，角色也跟着缩。
    # 而角色本体只有 ~190px 宽，闪光只是从角色往外辐射。所以这类动作单独限一个
    # 窗口（绕角色站位点居中），把超出的特效裁掉 —— 角色一个像素都不动，位置和
    # 大小仍然和其他动作完全一致（用户 2026-10-16 的硬要求）。
    cap_map = {}
    for s in args.cap:
        k, _, v = s.partition("=")
        cw, _, ch = v.partition("x")
        cap_map[k.strip()] = (int(cw), int(ch) if ch else args.max_h)

    for name, _, _ in plan:
        if name not in cap_map or boxes[name][1] < 0:
            continue
        cw_max, ch_max = cap_map[name]
        bx0s, bx1s, by0s, by1s = boxes[name]
        sc = scale_of(name)
        pcx, pbot = pivot[name]
        # 输出尺寸 = (源窗口 + 2*pad/scale) * sc，所以源窗口要先把 pad 让出来
        sw = (cw_max - 2 * args.pad) / sc
        sh = (ch_max - 2 * args.pad) / sc
        if bx1s - bx0s + 1 > sw:
            c0 = min(max(pcx - sw / 2.0, bx0s), bx1s - sw + 1)
            bx0s, bx1s = c0, c0 + sw - 1
            print("  %-18s 裁剪宽 %.0f -> 限到 %.0f 源px（--cap %dx%d）"
                  % (name, boxes[name][1] - boxes[name][0] + 1, sw, cw_max, ch_max))
        if by1s - by0s + 1 > sh:
            c0 = min(max(pbot - sh, by0s), by1s - sh + 1)
            by0s, by1s = c0, c0 + sh - 1
        boxes[name] = (bx0s, bx1s, by0s, by1s)

    # 第一趟（不读帧，只用已算好的包围盒）：未缩小的矩形 + 站位参考点落点。
    rect0, anchor = {}, {}
    for name, files, fps in plan:
        bx0s, bx1s, by0s, by1s = boxes[name]
        if bx1s < 0:                        # 这个动作整段全透明：退回全局框
            bx0s, bx1s, by0s, by1s = gx0, gx1, gy0, gy1

        cx0 = int(np.floor(bx0s - pad_src))
        cy0 = int(np.floor(by0s - pad_src))
        cx1 = int(np.ceil(bx1s + pad_src)) + 1
        cy1 = int(np.ceil(by1s + pad_src)) + 1
        rect0[name] = (int(round((cx1 - cx0) * scale)), int(round((cy1 - cy0) * scale)),
                       int(round((cx0 - gx0) * scale)), int(round((cy0 - gy0) * scale)))
        # 站位参考点：源坐标 (pcx, pbot)，以及它在**裁剪框内**的输出位置
        pcx, pbot = pivot[name]
        anchor[name] = ((pcx - cx0) * scale, (pbot - cy0) * scale, pcx, pbot)

    minx = min(v[2] for v in rect0.values())
    miny = min(v[3] for v in rect0.values())
    gw = max(v[2] - minx + v[0] for v in rect0.values())
    gh = max(v[3] - miny + v[1] for v in rect0.values())

    clips = []          # [(name, [帧...], fps, w, h, dx, dy)]
    for name, files, fps in plan:
        bx0s, bx1s, by0s, by1s = boxes[name]
        if bx1s < 0:                        # 这个动作整段全透明：退回全局框
            bx0s, bx1s, by0s, by1s = gx0, gx1, gy0, gy1

        sc = scale_of(name)
        ps = args.pad / sc                  # pad 是**输出**像素，换算回源

        cx0 = int(np.floor(bx0s - ps))
        cy0 = int(np.floor(by0s - ps))
        cx1 = int(np.ceil(bx1s + ps)) + 1
        cy1 = int(np.ceil(by1s + ps)) + 1
        w = int(round((cx1 - cx0) * sc))
        h = int(round((cy1 - cy0) * sc))

        # 站位参考点要落在哪（锚点框坐标）。**对齐时是全体共用的同一个目标**
        # （基准动作的参考点），不对齐时才是"本组自己的参考点在参照仿射下的位置"。
        if target is not None:
            fx = (target[0] - gx0) * scale - minx
            fy = (target[1] - gy0) * scale - miny
        else:
            fx = rect0[name][2] - minx + anchor[name][0]
            fy = rect0[name][3] - miny + anchor[name][1]
        pcx, pbot = anchor[name][2], anchor[name][3]
        dx = int(round(fx - (pcx - cx0) * sc))
        dy = int(round(fy - (pbot - cy0) * sc))

        fr = []
        for p in files:
            im = Image.open(p).convert("RGBA").crop((cx0, cy0, cx1, cy1))
            im = im.resize((w, h), Image.LANCZOS)
            fr.append(argb4444(np.array(im)))
        clips.append((name, fr, fps, w, h, dx, dy))
        print("  %-18s 帧 %3dx%-3d 偏移 (%4d,%4d)  缩放 %.3f%s  %3d 帧  角色=%s"
              % (name, w, h, dx, dy, sc,
                 ("" if 0 == shrink_px(name) else "(-%dpx)" % shrink_px(name)),
                 len(fr),
                 ("待机", "点击", "拖拽", "随机", "贴右", "贴左", "起手", "落地",
                  "说话", "表情")[role_of(name)]))

    # ---- 锚点框：做成**以枢轴为中心**的对称框 ------------------------------
    #
    # 固件原来是这样摆画布的（page_pet.c）：
    #     app_sprite2d_clip_box(0, &odx, &ody, &ow, &oh);   <- 读**第 0 组**（待机）
    #     lv_obj_set_pos(stage, PET_ORIGIN_X - (odx + ow/2),
    #                           PET_ORIGIN_Y - (ody + oh/2));
    # 也就是说**设备端也在算位置**，而且和打包耦合 —— 待机的裁剪框一变，宠物就挪窝。
    #
    # 用户 2026-10-16 要求：**几何全归打包，设备只管显示**。所以这里把锚点框撑成
    # 以枢轴为中心的对称框，枢轴 = 待机组裁剪框的中心（正好是原来那条规则用的点，
    # 所以屏幕上的落点不变）。固件于是只剩一句"把画布居中摆到原点"：
    #     lv_obj_set_pos(stage, PET_ORIGIN_X - cw/2, PET_ORIGIN_Y - ch/2);
    #
    # 代价只是锚点框略微变方（实测 218x184 -> 212x184，PSRAM 反而略降）。
    # **这条契约两边都得守**：改这里就要同步改 page_pet.c 的注释。
    pvx = clips[0][5] + clips[0][3] // 2
    pvy = clips[0][6] + clips[0][4] // 2
    hw = max(pvx - min(dx for _, _, _, _, _, dx, _ in clips),
             max(dx + w for _, _, _, w, _, dx, _ in clips) - pvx)
    hh = max(pvy - min(dy for _, _, _, _, _, _, dy in clips),
             max(dy + h for _, _, _, _, h, _, dy in clips) - pvy)
    ox, oy = pvx - hw, pvy - hh
    clips = [(nm, fr, fps, w, h, dx - ox, dy - oy)
             for nm, fr, fps, w, h, dx, dy in clips]
    gw, gh = 2 * hw, 2 * hh
    if gw > 0xFFFF or gh > 0xFFFF:
        print("\n!! 锚点框 %dx%d 超出 u16，得先删几个宽动作" % (gw, gh))
        return 1

    print("  锚点框 %dx%d，枢轴（待机裁剪框中心）在正中心 (%d,%d)"
          % (gw, gh, hw, hh))
    print("  -> 固件按**画布居中**摆，不用再读第 0 组的裁剪框（源并集推算值 %dx%d）"
          % (gw_src, gh_src))

    # ---- 帧尺寸上限（固件的流式缓冲按这个分配） ----
    bad = [(n, w, h) for n, _, _, w, h, _, _ in clips
           if w > args.max_w or h > args.max_h]
    if bad:
        print("\n!! 这些动作的帧超过固件上限 %dx%d，会被拒绝加载："
              % (args.max_w, args.max_h))
        for n, w, h in bad:
            print("     %-18s %dx%d" % (n, w, h))
        print("   —— 调小 --height，或者把该动作从 --order 里去掉")

    total = sum(len(fr) for _, fr, _, _, _, _, _ in clips)
    per_frame = sum(w * h * 2 for _, _, _, w, h, _, _ in clips) // max(1, len(clips))
    bytes_total = sum(len(fr) * w * h * 2 for _, fr, _, w, h, _, _ in clips)
    idle_bytes = len(clips[0][1]) * clips[0][3] * clips[0][4] * 2
    print("\n动作 %d 组，共 %d 帧，平均单帧 %.1f KB"
          % (len(clips), total, per_frame / 1024.0))
    print("  .spf 合计        %.2f MB" % (bytes_total / 1048576.0))
    print("  常驻（第 0 组）  %.0f KB  <- 开机就占住的 PSRAM" % (idle_bytes / 1024.0))
    print("  流式单帧缓冲     %.1f KB（按最大帧）"
          % (max(w * h * 2 for _, _, _, w, h, _, _ in clips) / 1024.0))

    # ---- 写文件（SPF7：每组记录 + 名字表 + 偏移表 + PackBits 数据） ----
    blobs = [packbits(a.reshape(-1)) for _, fr, _, _, _, _, _ in clips for a in fr]

    buf = bytearray()
    buf += b"SPF7"
    buf += struct.pack("<HH", len(clips), 0)
    buf += struct.pack("<HH", gw, gh)                 # 全局锚点框（夹紧用）
    for nm, fr, fps, w, h, dx, dy in clips:
        buf += struct.pack("<HHHHhhH", len(fr), fps, w, h, dx, dy, role_of(nm))

    # 名字表：nc × {u8 长度, ASCII 字节}，整体补 0 到 4 字节对齐。
    # **为什么要名字**：AI 桥接（page_char.c 的情绪表）和 MCP 工具都是**按名字点名**
    # 播动作的，而记录里只有 role 枚举。名字写进文件之后，固件按名字找组号，
    # `--order` 改了也不会静默错位（以前只能靠"固件里再抄一份顺序"）。
    # 补对齐是因为后面紧跟 u32 偏移表 —— M33 能容忍未对齐，但没必要赌。
    for nm, _, _, _, _, _, _ in clips:
        nb = nm.encode("ascii")
        buf += struct.pack("<B", len(nb))
        buf += nb
    while len(buf) % 4:
        buf += b"\0"

    # 偏移表：**total+1 项**（最后一项 = 数据区结束），固件靠它 O(1) 定位任意一帧，
    # 也知道这一帧的 RLE 有多长（下一项减这一项）。
    base = len(buf) + 4 * (len(blobs) + 1)
    offs, acc = [], base
    for b in blobs:
        offs.append(acc)
        acc += len(b)
    offs.append(acc)
    buf += struct.pack("<%dI" % len(offs), *offs)

    for b in blobs:
        buf += b

    raw = sum(w * h * 2 for _, fr, _, w, h, _, _ in clips for _ in fr)
    print("\n  SPF7 帧数据 %.1f MB -> RLE %.1f MB（%.0f%%）"
          % (raw / 1048576.0, sum(len(b) for b in blobs) / 1048576.0,
             100.0 * sum(len(b) for b in blobs) / max(1, raw)))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as fp:
        fp.write(buf)
    print("\n写出 %s（%.2f MB）" % (args.out, len(buf) / 1048576.0))

    # 预览：每个动作抽一帧并排贴到一条锚点框高的带子上，**位置按 dx/dy** ——
    # 这样一眼就能看出跨动作的站位/大小对不对（对齐是否生效）。
    if args.preview:
        strip = Image.new("RGBA", (gw * len(clips), gh), (32, 32, 40, 255))
        for i, (nm, fr, _, w, h, dx, dy) in enumerate(clips):
            a = fr[len(fr) // 2]
            rgba = np.zeros((h, w, 4), np.uint8)
            rgba[..., 3] = ((a >> 12) & 0xF).astype(np.uint8) * 17
            rgba[..., 0] = ((a >> 8) & 0xF).astype(np.uint8) * 17
            rgba[..., 1] = ((a >> 4) & 0xF).astype(np.uint8) * 17
            rgba[..., 2] = (a & 0xF).astype(np.uint8) * 17
            strip.alpha_composite(Image.fromarray(rgba, "RGBA"),
                                  (i * gw + dx, dy))
        strip.save(args.preview)
        print("预览 %s（%d 组并排，位置按 dx/dy）" % (args.preview, len(clips)))

    if args.verify:
        return 1 if spf7_verify(buf, clips) else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
