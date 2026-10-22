#!/usr/bin/env python3
"""量每个动作**实际渲染出来的角色高度**（不是裁剪框）。

    python third_party/sprite/clip_sizes.py third_party/sprite/my_pet/output_spf/fish_elf.spf

为什么要它：pack_seq.py 用**一个全局 scale** 缩放所有片段，而各片段源素材里角色
本身的高度并不一样（东张西望 145、待机 133）—— 上机表现就是"切到那组时角色忽然
变大/变小"。裁剪框量不出来这个（框 = 角色 + pad + 起跳余量），得量解码后每帧的
alpha 包围盒。

输出：每组的 逐帧高 min/中位/max + 并集高。**中位数**才是"这个动作看着多大"，
max 是跳起来那一下，不算数。
"""
import struct
import sys

import numpy as np

ALPHA_MIN = 0x1000        # ARGB4444 的 alpha 半字节非 0（和固件 prv_blit 一致）


def inflate(buf, n, off0, off1):
    out = np.empty(n, dtype=np.uint16)
    o, i = 0, off0
    while i < off1 and o < n:
        c = buf[i]
        i += 1
        if c < 128:
            k = min(c + 1, n - o)
            out[o:o + k] = np.frombuffer(buf, dtype="<u2", count=k, offset=i)
            i += 2 * k
            o += k
        else:
            k = min(257 - c, n - o)
            out[o:o + k] = np.frombuffer(buf, dtype="<u2", count=1, offset=i)[0]
            i += 2
            o += k
    return out


def main():
    path = sys.argv[1]
    buf = open(path, "rb").read()
    assert buf[:4] == b"SPF7", "不是 SPF7"
    nc, _ = struct.unpack_from("<HH", buf, 4)
    gw, gh = struct.unpack_from("<HH", buf, 8)
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

    starts, acc = [], 0
    for r in recs:
        starts.append(acc)
        acc += r[0]

    print("锚点框 %dx%d   共 %d 组 %d 帧" % (gw, gh, nc, total))
    print("%-16s %5s %6s %6s %6s %8s" %
          ("动作", "帧数", "最小", "中位", "最大", "并集"))
    meds = []
    for i in range(nc):
        n, fps, w, h, dx, dy, role = recs[i]
        hs = []
        for j in range(n):
            b = starts[i] + j
            px = inflate(buf, w * h, offs[b], offs[b + 1])
            al = px & 0xF000
            rows = np.nonzero(al.reshape(h, w).any(axis=1))[0]
            hs.append(int(rows.max() - rows.min() + 1) if len(rows) else 0)
        hs = np.array(hs)
        meds.append(int(np.median(hs)))
        print("%-16s %5d %6d %6d %6d %8d%s" %
              (names[i], n, hs.min(), int(np.median(hs)), hs.max(),
               hs.max() - hs.min() + 1,
               "   <- 明显偏大" if np.median(hs) > np.median(meds) + 4 else ""))
    print("\n中位高度的范围：%d ~ %d（差 %d px）"
          % (min(meds), max(meds), max(meds) - min(meds)))


if __name__ == "__main__":
    main()
