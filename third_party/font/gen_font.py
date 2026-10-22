#!/usr/bin/env python3
"""重新生成工程的两份中文字体（16px / 21px），保持原有字符集不变并追加新字。

为什么存在这个脚本
------------------
工程的两份 SourceHanSerifSC 字体是 lv_font_conv 生成的**手挑子集**：汉字只有
当年挑过的那些，新页面想用没挑过的字（比如"音乐"的"乐"）就会渲染成空白。
本脚本做三件事：

  1. 从现有字体 C 文件里反推出完整字符集（ASCII + 汉字 + LVGL 符号）；
  2. 追加命令行给出的新字（--add 音乐...）；
  3. 调 lv_font_conv 重新生成两份 .c，直接覆盖 source/embedded/font/ 下的文件。

字体来源
--------
  汉字/ASCII : Windows 自带的 NotoSerifSC-VF.ttf（= Source Han Serif SC，
               与原字体同一家族，外观一致）。
  LVGL 符号  : third_party/font/FontAwesome5.ttf（由 SDK 里 lvgl 的
               FontAwesome5-Solid+Brands+Regular.woff 用 fontTools 转成 ttf，
               woff 原件 opentype.js 读不了）。
  转换器     : lv_font_conv（npm 包，与生成原字体的是同一个工具）。

用法
----
  python third_party/font/gen_font.py --add 乐歌曲播放中

依赖：node（PATH 或 --node 指定）、workspace 里的 lv_font_conv、
third_party/font/FontAwesome5.ttf、Windows 的 NotoSerifSC-VF.ttf。
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "source", "embedded", "font"))

SRC_TTF = r"C:\Windows\Fonts\NotoSerifSC-VF.ttf"
AWESOME_TTF = os.path.join(HERE, "FontAwesome5.ttf")
CONV_JS = os.path.normpath(os.path.join(
    r"C:\Users\23594\.workbuddy\binaries\node\workspace",
    "node_modules", "lv_font_conv", "lv_font_conv.js"))

FONTS = [
    ("lv_font_SourceHanSerifSC_Regular_16", 16),
    ("lv_font_SourceHanSerifSC_Regular_21", 21),
]


def read_charset(path):
    """从生成的字体 C 文件里抽出全部码点（注释 /* U+XXXX */）。"""
    with open(path, encoding="utf-8", errors="ignore") as f:
        text = f.read()
    codes = sorted({int(m, 16) for m in re.findall(r"/\* U\+([0-9A-Fa-f]{4,6}) ", text)})
    if not codes:
        raise SystemExit(f"从 {path} 读不到字符集")
    ascii_part = [c for c in codes if 0x20 <= c <= 0x7E]
    other = [c for c in codes if not (0x20 <= c <= 0x7E)]
    return ascii_part, other


def fmt_range(codes):
    """码点列表 -> lv_font_conv 的 -r 参数（连续段合并）。"""
    runs, out = [], []
    for c in codes:
        if runs and c == runs[-1][1] + 1:
            runs[-1][1] = c
        else:
            runs.append([c, c])
    for a, b in runs:
        out.append(hex(a) if a == b else f"{hex(a)}-{hex(b)}")
    return ",".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--add", default="", help="要追加的汉字，直接写字符串")
    ap.add_argument("--node", default=r"C:\Users\23594\.workbuddy\binaries\node"
                                      r"\versions\22.22.2-3\node.exe")
    ap.add_argument("--keep", action="store_true", help="保留旧文件为 .bak")
    args = ap.parse_args()

    extra = [ord(c) for c in args.add if ord(c) > 0x7F]
    extra = sorted(set(extra))

    for name, size in FONTS:
        path = os.path.join(FONT_DIR, name + ".c")
        ascii_part, other = read_charset(path)
        added = [c for c in extra if c not in ascii_part and c not in other]
        charset = ascii_part + sorted(other + added)

        out = os.path.join(FONT_DIR, name + ".new.c")
        cmd = [args.node, CONV_JS,
               "--font", SRC_TTF, "-r", fmt_range(ascii_part + [c for c in charset if c < 0xF000]),
               "--font", AWESOME_TTF, "-r", fmt_range([c for c in charset if c >= 0xF000]),
               "--size", str(size), "--bpp", "4", "--no-compress", "--no-prefilter",
               "--format", "lvgl", "--lv-font-name", name, "-o", out]
        print(">>", " ".join(cmd[1:]))
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            raise SystemExit(f"{name} 生成失败")
        if args.keep and os.path.exists(path):
            os.replace(path, path + ".bak")
        os.replace(out, path)
        print(f"   {name}: 原 {len(ascii_part) + len(other)} 字 + 新 {len(added)} 字"
              f" -> {path}")


if __name__ == "__main__":
    main()
