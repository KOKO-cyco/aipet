#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把打好的 .spf 解回来，量**每一组在锚点框里的实际落点**。

为什么需要
----------
`pack_seq.py` 的对齐（`--align`）和尺寸归一（`--fit`）都是**在源 PNG 上**算的，
而固件真正画的是打包之后的 ARGB4444 帧 —— 中间还隔着 LANCZOS 缩放和 4 位 alpha
量化，两者都会让边缘挪 ±1px。所以"源里对齐了"不等于"屏上对齐了"。

这个脚本直接解 .spf（不碰源素材），量每组**逐帧中位**的底边 y / 中心 x / 角色高。
这三列的**跨度**就是切动作时角色会跳多少：

    底边y 跨度   → 上下跳
    中心x 跨度   → 左右跳
    角色高 跨度  → 胀缩

判据：都在 ±1px 内 = 对齐到位。**不可能全 0** —— 输出是整数像素栅格，
`dx = round(...)` 本身就带 ±0.5px，再加 alpha 量化的 ±1px 边缘抖动。

用法
----
    py -3 third_party/sprite/check_spf.py third_party/sprite/my_pet/output_spf/fish_elf.spf
"""

import os
import struct
import sys

import numpy as np

ALPHA_MIN4 = 1      # 4 位 alpha 的有效阈值（0 = 全透明）


def unpackbits(data, count):
    """PackBits -> count 个 uint16。和 pack_seq.packbits 互逆。"""
    out = np.empty(count, np.uint16)
    i, k, n = 0, 0, len(data)
    while k < count and i < n:
        c = data[i]
        i += 1
        if c < 128:
            m = c + 1
            out[k:k + m] = np.frombuffer(data[i:i + 2 * m], dtype="<u2")
            i += 2 * m
            k += m
        else:
            m = 257 - c
            out[k:k + m] = struct.unpack("<H", data[i:i + 2])[0]
            i += 2
            k += m
    return out


def load(path):
    """-> (原始字节, 锚点框, [(名字, 记录...)], 偏移表)"""
    b = open(path, "rb").read()
    if b[:4] != b"SPF7":
        sys.exit("不是 SPF7（magic = %r）" % b[:4])
    nc, _ = struct.unpack_from("<HH", b, 4)
    gw, gh = struct.unpack_from("<HH", b, 8)
    recs, off = [], 12
    for _ in range(nc):
        recs.append(struct.unpack_from("<HHHHhhH", b, off))   # n,fps,w,h,dx,dy,role
        off += 14
    names = []
    for _ in range(nc):
        ln = b[off]
        names.append(b[off + 1:off + 1 + ln].decode("ascii"))
        off += 1 + ln
    while off % 4:
        off += 1
    total = sum(r[0] for r in recs)
    offs = list(struct.unpack_from("<%dI" % (total + 1), b, off))
    return b, (gw, gh), list(zip(names, recs)), offs


def main(path, basis):
    b, (gw, gh), clips, offs = load(path)
    print("%s" % path)
    print("锚点框 %dx%d（固件拿它当画布，并且按**画布居中**摆到宠物原点）" % (gw, gh))
    print()
    print("判据按**首帧**（= pack_seq 的 --align first / --fit follow 用的基准）：")
    print("切动作时看到的就是新动作的首帧，所以只有首帧对齐才保证'切过去不跳'。")
    print("中位列是参考 —— 它反映整段常态，会被'缩着/张开'的姿势带偏，不算判据。")
    print()
    print("%-16s %4s | %-22s | %-22s" % ("动作", "帧", "首帧 底边y/中心x/高",
                                        "中位 底边y/中心x/高"))
    print("-" * 74)

    rows, f = [], 0
    for name, (n, fps, w, h, dx, dy, role) in clips:
        fb, fc, fh = None, None, None
        bots, cxs, hts = [], [], []
        for k in range(n):
            px = unpackbits(b[offs[f + k]:offs[f + k + 1]], w * h)
            a = ((px >> 12) & 0xF).reshape(h, w)
            ys, xs = np.nonzero(a >= ALPHA_MIN4)
            if len(xs) == 0:
                continue
            bot = dy + int(ys.max())
            cx = dx + (int(xs.min()) + int(xs.max())) / 2.0
            ht = int(ys.max() - ys.min() + 1)
            if fb is None:                  # 首帧（第一个有内容的帧）
                fb, fc, fh = bot, cx, ht
            bots.append(bot)
            cxs.append(cx)
            hts.append(ht)
        f += n
        if fb is None:
            continue
        rows.append((name, n, fb, fc, fh,
                     int(np.median(bots)), float(np.median(cxs)), int(np.median(hts))))
        print("%-16s %4d | %3d %6.1f %4d      | %3d %6.1f %4d"
              % (name, n, fb, fc, fh, rows[-1][5], rows[-1][6], rows[-1][7]))

    print()
    for i, (label, cols) in enumerate((("首帧", (2, 3, 4)), ("中位", (5, 6, 7)))):
        bs = [r[cols[0]] for r in rows]
        cs = [r[cols[1]] for r in rows]
        hs = [r[cols[2]] for r in rows]
        print("  %s  底边y %3d..%3d (跨%d)   中心x %.1f..%.1f (跨%.1f)   高 %d..%d (跨%d)"
              % (label, min(bs), max(bs), max(bs) - min(bs),
                 min(cs), max(cs), max(cs) - min(cs), min(hs), max(hs), max(hs) - min(hs)))

    c = (2, 3, 4) if basis == "first" else (5, 6, 7)
    bs = [r[c[0]] for r in rows]
    cs = [r[c[1]] for r in rows]
    hs = [r[c[2]] for r in rows]
    # 阈值 2px 是**整数栅格 + 4 位 alpha 量化的下限**，不是"放宽到能过"：
    #   * 摆放量 `dx = round(...)` 本身带 ±0.5px；
    #   * 帧里的 alpha 只有 4 位，LANCZOS 缩出来的软边量化后，边界落在
    #     alpha≈8.5/255 那条线上，两个动作的边缘梯度不同就会差 1px。
    # 两者叠起来，两组之间差 2px 是理论下限。要更小只能不量化 alpha（帧体积翻倍）。
    ok = (max(bs) - min(bs)) <= 2 and (max(cs) - min(cs)) <= 2.0 \
        and (max(hs) - min(hs)) <= 2
    print()
    if ok:
        print("  ✓ 按%s判：全体一致（跨度 ≤2px = 整数栅格 + 4 位 alpha 量化的下限）"
              % basis)
    else:
        print("  ✗ 按%s判：还有动作对不齐（位置看 --align，大小看 --fit）" % basis)
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("用法：py -3 third_party/sprite/check_spf.py <文件.spf> [first|median]")
    sys.exit(main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "first"))
