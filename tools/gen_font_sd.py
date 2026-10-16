#!/usr/bin/env python3
"""生成放 SD 卡的完整中文字库（lv_font_conv 的 bin 格式）。

为什么要有这个脚本
------------------
固件里编译的那两份字体是手挑子集（16px 只有 243 个码点、21px 185 个），新文案
用到没挑过的字就渲染成空白。本脚本生成**GB2312 全集**（汉字 6763 + 符号区）
的两份 bin，拷到 SD 卡 /sdcard/font/ 下，设备上开机后台加载成内置字体的
fallback —— 内置字库查不到的字自动去 SD 字库找，UI 代码一行都不用改。

只放汉字和中文符号，不放 ASCII / FontAwesome：内置字库里本来就有，LVGL 先查
主字体、查不到才走 fallback，重复放纯属浪费 PSRAM。

用法
----
  python tools/gen_font_sd.py                # 生成 16px + 21px 两份
  python tools/gen_font_sd.py --size 16
  python tools/gen_font_sd.py --out D:/sdcard/font

产物：cn16.bin / cn21.bin（默认落在 tools/sdcard_font/）
依赖：node（--node 指定）、workspace 里的 lv_font_conv、Windows 的
NotoSerifSC-VF.ttf（与固件内字体同源，外观一致）。
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_TTF = r"C:\Windows\Fonts\NotoSerifSC-VF.ttf"
CONV_JS = os.path.normpath(os.path.join(
    r"C:\Users\23594\.workbuddy\binaries\node\workspace",
    "node_modules", "lv_font_conv", "lv_font_conv.js"))

DEFAULT_OUT = os.path.join(HERE, "sdcard_font")


def gb2312_chars():
    """GB2312 全部字符的码点集合。

    0xA1A1-0xA9FE 是符号区（中文标点、全角数字字母、希腊字母、假名…），
    0xB0A1-0xF7FE 是一二级汉字区（6763 字）。GBK 扩展区（0x8140-0xA0FE 等）
    不属于 GB2312，这里不收 —— 那些字仍会缺，但出现概率极低。
    """
    out = set()
    for hi in range(0xA1, 0xAA):        # 符号区
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


def fmt_range(codes):
    """码点列表 -> lv_font_conv 的 -r 参数（连续段合并）。"""
    runs, out = [], []
    for c in codes:
        if runs and c == runs[-1][1] + 1:
            runs[-1][1] = c
        else:
            runs.append([c, c])
    for a, b in runs:
        out.append(hex(a) if a == b else "%s-%s" % (hex(a), hex(b)))
    return ",".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", default="16,21", help="要生成的字号，逗号分隔")
    ap.add_argument("--out", default=DEFAULT_OUT, help="输出目录")
    ap.add_argument("--node", default=r"C:\Users\23594\.workbuddy\binaries\node"
                                      r"\versions\22.22.2-3\node.exe")
    args = ap.parse_args()

    if not os.path.exists(SRC_TTF):
        raise SystemExit("找不到源字体 %s" % SRC_TTF)

    chars = gb2312_chars()
    # 用 --symbols 而不是 -r：GB2312 的汉字在 Unicode 上很分散，展开成 range 之后
    # 命令行会超过 Windows 的 CreateProcess 上限（实测 WinError 206）。
    symbols = "".join(chr(c) for c in chars)
    print("[字符集] %d 个码点（GB2312 符号区 + 一二级汉字）" % len(chars))

    os.makedirs(args.out, exist_ok=True)

    for size in [s.strip() for s in args.size.split(",") if s.strip()]:
        out = os.path.join(args.out, "cn%s.bin" % size)
        cmd = [args.node, CONV_JS,
               "--font", SRC_TTF, "--symbols", symbols,
               "--size", size, "--bpp", "4", "--no-compress", "--no-prefilter",
               "--format", "bin", "-o", out]
        print(">> 生成 %spx ..." % size)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            raise SystemExit("生成 %spx 失败" % size)
        print("   %s  %.1f KB" % (out, os.path.getsize(out) / 1024.0))

    print("\n把这两个文件拷到 SD 卡根目录的 font/ 下：/sdcard/font/cn16.bin, cn21.bin")


if __name__ == "__main__":
    main()
