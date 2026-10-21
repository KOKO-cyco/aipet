#!/usr/bin/env python3
"""生成放 SD 卡的中文字库 bin（lv_font_conv 的 --format bin，按需读字形用）。

    python tools/gen_font_sd.py                          # 六档全量，默认路径
    python tools/gen_font_sd.py --size 16,24
    python tools/gen_font_sd.py --node E:/node/node.exe --conv <lv_font_conv.js>

为什么是"全 GB2312"
-------------------
界面里的字分两种：一种是代码写死的（改代码就能跟着改子集），另一种是**运行时
才出现的** —— 天气描述、城市名、AI 回复、用户写的备忘录。后者用什么都可能，
静态子集永远会缺字，缺字就渲染成空白。所以 SD 上这份必须是全集。

为什么要有这个脚本（而不是编进固件）
------------------------------------
全集 × 六个字号 ≈ 5MB，编进固件放不下，全读进 PSRAM 也放不下。所以放在 SD 上，
`app_font.c` 只把索引（cmap + 字形描述）读进 PSRAM，**字形位图按需读**，读过的
进 24 行缓存。这个脚本负责把全集生出来。

字符集
------
GB2312 全部（符号区 + 一级/二级汉字 6763 个）+ ASCII 可见区。汉字在 Unicode 上
是散的，展开成 range 会超过 Windows 的命令行长度上限，所以用 `--symbols`。

依赖：node + lv_font_conv（`npm install lv_font_conv`）、字体包里的
AlibabaPuHuiTi-3-55-Regular.ttf（和现有 SD 字库同一款字，换字体会让整界面变样）。
"""

import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DEF_TTF = os.path.join(ROOT, "third_party/font/fontpkg-alibaba-pu-hui-ti-3-0-3.1.2",
                       "package", "AlibabaPuHuiTi-3-55-Regular.ttf")
DEF_OUT = os.path.join(ROOT, "third_party/font/sdcard_font")
DEF_SIZES = "14,16,18,20,24,32"


def find_conv():
    """lv_font_conv.js 的常见位置（npm 全局 / 本地装）。"""
    cands = [
        os.path.join(os.path.expanduser("~"), "lvfont", "node_modules",
                     "lv_font_conv", "lv_font_conv.js"),
        os.path.join(ROOT, "tools", "node_modules", "lv_font_conv", "lv_font_conv.js"),
    ]
    for c in cands:
        if os.path.exists(c):
            return c
    return None


def gb2312_chars():
    """GB2312 全部字符的码点集合。"""
    out = set(range(0x20, 0x7F))
    for hi in range(0xA1, 0xF8):        # 符号区
        for lo in range(0xA1, 0xFF):
            try:
                out.add(ord(bytes([hi, lo]).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    for hi in range(0xB0, 0xF8):        # 汉字区
        for lo in range(0xA1, 0xFF):
            try:
                out.add(ord(bytes([hi, lo]).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    return sorted(out)


def patch_metrics(path):
    """把表头的 ascent / max_y 改成 typo_ascent。

    为什么：lv_font_conv 1.5.x 的 ascent 取的是 hhea 的 ascender，比旧版（取
    typo ascender）高 2px，行高会跟着变 —— 整个界面的文字行距都会松一截。
    这里改回旧口径，字形本身不动，只是行盒回到原来的高度。
    """
    d = bytearray(open(path, "rb").read())
    typo_a = struct.unpack_from("<H", d, 8 + 12)[0]
    struct.pack_into("<H", d, 8 + 8, typo_a)      # ascent
    struct.pack_into("<H", d, 8 + 18, typo_a)     # max_y
    open(path, "wb").write(d)
    return typo_a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", default=DEF_SIZES, help="要生成的字号，逗号分隔")
    ap.add_argument("--out", default=DEF_OUT, help="输出目录")
    ap.add_argument("--node", default=None, help="node 可执行文件（默认取 PATH）")
    ap.add_argument("--conv", default=None, help="lv_font_conv.js 路径")
    ap.add_argument("--ttf", default=DEF_TTF, help="源字体")
    args = ap.parse_args()

    node = args.node or os.environ.get("NODE") or "node"
    conv = args.conv or find_conv()
    if conv is None:
        sys.exit("找不到 lv_font_conv.js —— 先 `npm install lv_font_conv`，"
                 "或者用 --conv 指过去")
    if not os.path.exists(args.ttf):
        sys.exit("找不到源字体 %s" % args.ttf)

    chars = gb2312_chars()
    symbols = "".join(chr(c) for c in chars)
    print("[字符集] %d 个码点（GB2312 全集 + ASCII）" % len(chars))

    os.makedirs(args.out, exist_ok=True)

    for size in [s.strip() for s in args.size.split(",") if s.strip()]:
        out = os.path.join(args.out, "puhui%s.bin" % size)
        cmd = [node, conv,
               "--font", args.ttf, "--symbols", symbols,
               "--size", size, "--bpp", "4", "--no-compress", "--no-prefilter",
               "--format", "bin", "-o", out]
        print(">> 生成 %spx ..." % size)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            sys.exit("生成 %spx 失败" % size)
        asc = patch_metrics(out)
        print("   %s  %.1f KB（ascent 校正为 %d）"
              % (out, os.path.getsize(out) / 1024.0, asc))

    print("\n把 %s 下的 puhui*.bin 拷到 SD 卡 /sdcard/font/ 下。" % args.out)


if __name__ == "__main__":
    main()
