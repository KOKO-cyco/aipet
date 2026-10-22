#!/usr/bin/env python3
"""检查 SD 卡字库 bin（/sdcard/font/cn16.bin、cn21.bin）能不能被设备端加载。

为什么要有这个脚本
------------------
设备端的 app_font.c 是自己写的 bin 解析（LVGL 自带的 lv_binfont_create 会把整个
字形位图 malloc 到 SRAM，2MB 级字库放不下）。其中唯一"自己发明"的部分是位图重排：

  bin 里每个字形 = [描述符 nbits 位][位图]，而 nbits 是 26（16px）/ 28（21px），
  **不是 8 的倍数** —— 位图在文件里是错开的位流，每个字节都横跨两个文件字节。
  官方 loader 是逐 bit 读（每字节 8 次函数调用），设备端改成一次拼一个字节，
  快一个量级。算法对不对，在真机上要等字形画出来才知道，太慢；这里先在 PC 上
  跑一遍两种算法，逐个字形比对。

顺带报告表结构、字形数、抽样几何，拷卡之前先跑它比烧一次固件快得多。

用法
----
  python third_party/font/font_bin_probe.py third_party/font/sdcard_font/puhui14.bin
  python third_party/font/font_bin_probe.py third_party/font/sdcard_font/*.bin -c 中文字库音乐
"""

import argparse
import struct
import sys

HEAD_FMT = "<IHHHhHhHhhHHBBBBBBBBbbH"
HEAD_NAMES = [
    "version", "tables_count", "font_size", "ascent", "descent",
    "typo_ascent", "typo_descent", "typo_line_gap", "min_y", "max_y",
    "def_adv_w", "kern_scale", "idx2loc", "glyph_id_fmt", "adv_w_fmt",
    "bpp", "xy_bits", "wh_bits", "adv_w_bits", "compress", "subpx",
    "pad", "ul_pos", "ul_thick",
]


class Bits:
    """位读取器：每字节从 MSB 到 LSB（对应设备端的 prv_bit_get）。"""

    def __init__(self, buf, pos):
        self.b = buf
        self.p = pos
        self.bit = 0

    def read(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.b[self.p] >> (7 - self.bit)) & 1)
            self.bit += 1
            if self.bit == 8:
                self.bit = 0
                self.p += 1
        return v

    def signed(self, n):
        v = self.read(n)
        if n and (v >> (n - 1)) & 1:
            v -= 1 << n
        return v


def bits_to_bytes(src, bitpos, n):
    """设备端 prv_bits_to_bytes 的 Python 版：一次拼一个字节。"""
    out = bytearray(n)
    for k in range(n):
        bp = bitpos + 8 * k
        bi = bp >> 3
        sh = bp & 7
        hi = src[bi]
        lo = 0 if (sh == 0 or bi + 1 >= len(src)) else src[bi + 1]
        out[k] = ((hi << sh) | (lo >> (8 - sh))) & 0xFF
    return bytes(out)


def official_extract(buf, start, nbits, bmp_size):
    """官方 loader 的逐 bit 路径（含最后那个字节的特殊处理）。"""
    it = Bits(buf, start)
    it.read(nbits)
    out = bytearray(bmp_size)
    for k in range(bmp_size - 1):
        out[k] = it.read(8)
    if bmp_size:
        tail = it.read(8 - nbits % 8) if (8 - nbits % 8) else 0
        out[bmp_size - 1] = (tail << (nbits % 8)) & 0xFF
    return bytes(out)


def read_label(buf, start, label):
    """表的 8 字节前缀 [u32 length][char[4] label]，length 含这 8 字节。"""
    if start + 8 > len(buf):
        return -1
    ln = struct.unpack_from("<I", buf, start)[0]
    if buf[start + 4:start + 8] != label.encode():
        return -1
    return ln


def probe(path, sample, check_bytes):
    buf = open(path, "rb").read()
    print("%s   %.1f KB" % (path, len(buf) / 1024.0))

    hl = read_label(buf, 0, "head")
    if hl < 0:
        print("  !! 不是 lv_font_conv 的 bin（找不到 head 表）")
        return
    h = dict(zip(HEAD_NAMES, struct.unpack_from(HEAD_FMT, buf, 8)))
    print("  version=%d 字号=%d ascent=%d descent=%d bpp=%d 压缩=%d"
          % (h["version"], h["font_size"], h["ascent"], h["descent"],
             h["bpp"], h["compress"]))

    nbits = h["adv_w_bits"] + 2 * h["xy_bits"] + 2 * h["wh_bits"]
    print("  描述符 %d 位（%%8=%d）-> %s"
          % (nbits, nbits % 8,
             "字节对齐" if nbits % 8 == 0 else "错位，位图要重排"))

    cl = read_label(buf, hl, "cmap")
    loca_start = hl + cl
    ll = read_label(buf, loca_start, "loca")
    glyph_start = loca_start + ll
    gl = read_label(buf, glyph_start, "glyf")
    if min(cl, ll, gl) < 0:
        print("  !! 表目录不全（cmap=%d loca=%d glyf=%d）" % (cl, ll, gl))
        return
    print("  表：head@0 len=%d / cmap@%d len=%d / loca@%d len=%d / glyf@%d len=%d"
          % (hl, hl, cl, loca_start, ll, glyph_start, gl))

    p = loca_start + 8
    loca_count = struct.unpack_from("<I", buf, p)[0]
    p += 4
    if h["idx2loc"] == 0:
        off = list(struct.unpack_from("<%dH" % loca_count, buf, p))
    else:
        off = list(struct.unpack_from("<%dI" % loca_count, buf, p))
    print("  字形数 %d，loca 格式 %s" % (loca_count,
                                    "u16" if h["idx2loc"] == 0 else "u32"))
    if h["idx2loc"] == 0 and max(off) >= 0xFFFE:
        print("  !! uint16 loca 已逼近上限，glyf 可能溢出")

    # 位图长度：相邻偏移之差 - 描述符占的整字节数（向上取整）。
    # 官方 loader 用的是向下取整，每个字形会多算一个字节（那是下一个字形的头部）。
    desc_bytes = (nbits + 7) // 8

    # 位图重排：设备端算法 vs 官方逐 bit 算法
    bad = 0
    checked = 0
    total_bmp = 0
    for i in range(1, loca_count):
        next_off = off[i + 1] if i + 1 < loca_count else gl
        size = next_off - off[i] - desc_bytes
        it = Bits(buf, glyph_start + off[i])
        # 描述符字段顺序：adv_w / ofs_x / ofs_y / box_w / box_h
        if h["adv_w_bits"]:
            it.read(h["adv_w_bits"])
        it.signed(h["xy_bits"])
        it.signed(h["xy_bits"])
        bw, bh = it.read(h["wh_bits"]), it.read(h["wh_bits"])
        if bw * bh == 0:
            continue
        total_bmp += size
        if checked < check_bytes:
            checked += 1
            mine = bits_to_bytes(buf, (glyph_start + off[i]) * 8 + nbits, size)
            offi = official_extract(buf, glyph_start + off[i], nbits, size)
            # 最后一个字节官方只读了有效位再左移，padding 位可能不同，不参与比对
            if mine[:size - 1] != offi[:size - 1]:
                bad += 1
    print("  位图重排校验：比对了 %d 个字形，%s（位图合计 %.1f KB）"
          % (checked, "全部一致" if bad == 0 else "!! %d 个不一致" % bad,
             total_bmp / 1024.0))

    # cmap 里查几个字，看几何是否合理
    if sample:
        sub = struct.unpack_from("<I", buf, hl + 8)[0]
        tables = []
        for k in range(sub):
            tables.append(struct.unpack_from("<IIHHHBB", buf, hl + 12 + k * 16))
        print("  抽样：")
        for ch in sample:
            cp = ord(ch)
            gid = None
            for (doff, rs, rlen, gstart, cnt, fmt, _pad) in tables:
                if rs <= cp < rs + rlen:
                    gid = gstart + (cp - rs)
                    break
            if gid is None or gid >= loca_count:
                print("    %s U+%04X  不在字库里" % (ch, cp))
                continue
            it = Bits(buf, glyph_start + off[gid])
            adv = h["def_adv_w"] if h["adv_w_bits"] == 0 else it.read(h["adv_w_bits"])
            if h["adv_w_fmt"] == 0:
                adv *= 16
            ox, oy = it.signed(h["xy_bits"]), it.signed(h["xy_bits"])
            bw, bh = it.read(h["wh_bits"]), it.read(h["wh_bits"])
            ok = "OK" if (bw > 0 and bh > 0 and adv > 0) else "!! 异常"
            print("    %s U+%04X glyph#%d adv=%d(%.1fpx) box=%dx%d ofs=(%d,%d) %s"
                  % (ch, cp, gid, adv, adv / 16.0, bw, bh, ox, oy, ok))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bins", nargs="+", help="要检查的 .bin")
    ap.add_argument("-c", "--chars", default="中文字库音乐播放器", help="抽样检查的字")
    ap.add_argument("-n", "--check", type=int, default=300,
                    help="比对多少个字形的位图（默认 300）")
    args = ap.parse_args()
    for p in args.bins:
        try:
            probe(p, args.chars, args.check)
        except Exception as e:  # noqa: BLE001
            print("  %s 解析失败: %s" % (p, e))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
