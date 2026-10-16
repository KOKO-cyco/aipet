#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 third_party/white/1x/<code>.png（带透明通道的彩色天气图标）转成 LVGL 内嵌
RGB565A8 图标（48x48，RGB565 平面 + A8 透明掩码平面）。

为什么内嵌、不用文件
--------------------
固件实际链接的是 LVGL **v9**（app.elf 里是 `lv_image_create` 而不是 v8 的
`lv_img_create`）。这份 v9 里：
  · `LV_USE_FS_*` 全 0 —— 没有任何文件系统驱动被注册，`lv_fs_open()` 必然失败；
  · `LV_USE_LODEPNG = 0`（v9 只认 `LV_USE_LODEPNG`，Tuya conf 里那个
    `LV_USE_PNG` 在 v9 的 lv_conf_internal 里根本没人读）—— 没有 PNG 解码器。
所以 "PNG 放 SD 卡 + lv_image_set_src(路径)" 在不改 SDK 的前提下走不通。

为什么是 RGB565A8（而不是 RGB565 或 A8）
---------------------------------------
· 图标是**彩色**的（太阳黄 / 云灰白 / 雨滴蓝），内部还有结构（"多云"是太阳+云
  两层）。只取 alpha 当 A8 掩码能省 2/3 空间，但会把这些结构压成剪影 —— 不能用。
· 早先的版本为了省空间，把 alpha **预合成到黑底**再做 RGB565（2B/px）。代价是图里
  烘死了黑底：主页底色是纯黑时看不出来，但只要背景不是黑的（比如点击时容器被按下
  染了底色），就会露出一个黑色方块。这正是实际踩到的坑。
· RGB565A8 = 3B/px（+50%），保留真透明，任何底色上都干净。40 张 48x48 约 270KB
  flash（ap 分区还剩 ~700KB，放得下）。

数据布局（LVGL v9 硬要求，见 lv_draw_sw_img.c:210）
---------------------------------------------------
    [ RGB565 平面: w*h*2 字节，stride = w*2 ]
    [ A8     平面: w*h   字节，stride = w   ]   <- 紧跟在 RGB 平面后面
LVGL 用 `src_buf + stride*h` 定位 alpha，所以两个平面必须**连续**。本生成器把
每张图写成一个含 `uint16_t rgb[]` + `uint8_t a[]` 的结构体，并加编译期断言保证
"无填充、顺序正确"。

处理方式
--------
等比缩放到最长边 48（不放大），居中放进 48x48 画布，四周 alpha=0（真透明）。
缩放用 **alpha 加权**平均，避免边缘把透明像素的杂色平均进去（否则会有暗边）。

用法
----
    python tools/gen_weather_icons.py            # 生成
    python tools/gen_weather_icons.py --check    # 只体检，不写文件
"""

import argparse
import glob
import os
import struct
import sys
import zlib

FIT = 48                        # 等比缩放到最长边（不放大）
W = H = 48                      # 输出画布
VALS_PER_LINE = 8               # RGB565 每行几个字面量
A_PER_LINE = 16                 # A8 每行几个字面量

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
SRC_DIR = os.path.join(PROJ, "third_party", "white", "1x")
OUT_C = os.path.join(PROJ, "source", "embedded", "src", "pages", "weather_icons.c")


# --------------------------------------------------------------------------- #
# 纯 python PNG 解码（只用 stdlib zlib；本机 venv 里没有 PIL）
# --------------------------------------------------------------------------- #
def read_png_rgba(path):
    """返回 (w, h, rgba_bytes)。只支持 8bit RGBA / 无隔行。"""
    raw = open(path, "rb").read()

    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: %s" % path)

    pos, idat = 8, b""
    w = h = bit_depth = color_type = interlace = 0

    while pos + 8 <= len(raw):
        (length,) = struct.unpack(">I", raw[pos:pos + 4])
        ctype = raw[pos + 4:pos + 8]
        data = raw[pos + 8:pos + 8 + length]
        pos += 12 + length                    # len + type + data + crc

        if ctype == b"IHDR":
            w, h, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", data)
        elif ctype == b"IDAT":
            idat += data
        elif ctype == b"IEND":
            break

    if bit_depth != 8 or color_type != 6 or interlace != 0:
        raise ValueError("%s: 只支持 8bit RGBA 无隔行 (bit=%d ct=%d il=%d)"
                         % (path, bit_depth, color_type, interlace))

    bpp = 4
    stride = w * bpp
    flat = zlib.decompress(idat)
    out = bytearray(stride * h)
    prev = bytearray(stride)
    p = 0

    for y in range(h):
        ft = flat[p]
        p += 1
        line = bytearray(flat[p:p + stride])
        p += stride

        if ft == 1:                                     # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:                                   # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:                                   # Average
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:                                   # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                if pa <= pb and pa <= pc:
                    pr = a
                elif pb <= pc:
                    pr = b
                else:
                    pr = c
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            raise ValueError("%s: 未知 filter %d" % (path, ft))

        out[y * stride:(y + 1) * stride] = line
        prev = line

    return w, h, bytes(out)


# --------------------------------------------------------------------------- #
# alpha 加权盒式降采样 -> 居中进 48x48 画布 -> 拆成 RGB565 / A8 两个平面
# --------------------------------------------------------------------------- #
def fit_size(w, h):
    """等比缩放到最长边 = FIT（不放大）。"""
    m = max(w, h)
    if m <= FIT:
        return w, h
    return max(1, round(w * FIT / m)), max(1, round(h * FIT / m))


def box_scale_rgba(rgba, w, h, nw, nh):
    """alpha 加权盒式平均。RGB 用 sum(c*a)/sum(a)，这样半透明的边缘像素不会
    把透明像素（RGB 往往为 0）平均进来形成暗边。"""
    out = bytearray(nw * nh * 4)

    for oy in range(nh):
        y0 = oy * h // nh
        y1 = max(y0 + 1, (oy + 1) * h // nh)
        for ox in range(nw):
            x0 = ox * w // nw
            x1 = max(x0 + 1, (ox + 1) * w // nw)

            sa = sr = sg = sb = cnt = 0
            for y in range(y0, y1):
                base = y * w * 4
                for x in range(x0, x1):
                    i = base + x * 4
                    a = rgba[i + 3]
                    sa += a
                    sr += rgba[i] * a
                    sg += rgba[i + 1] * a
                    sb += rgba[i + 2] * a
                    cnt += 1

            if sa > 0:
                o = (oy * nw + ox) * 4
                out[o] = min(255, sr // sa)
                out[o + 1] = min(255, sg // sa)
                out[o + 2] = min(255, sb // sa)
                out[o + 3] = sa // cnt
            # sa == 0：整块全透明，保持 0（RGB 与 A 都是 0）

    return out


def to_canvas(rgba, nw, nh):
    """居中放进 W x H（四周真透明）。返回 (rgb565 列表, alpha 列表)。"""
    rgb = [0] * (W * H)
    alpha = [0] * (W * H)
    ox, oy = (W - nw) // 2, (H - nh) // 2

    for y in range(nh):
        for x in range(nw):
            i = (y * nw + x) * 4
            a = rgba[i + 3]
            v = ((rgba[i] >> 3) << 11) | ((rgba[i + 1] >> 2) << 5) | (rgba[i + 2] >> 3)
            o = (oy + y) * W + ox + x
            rgb[o] = v
            alpha[o] = a

    return rgb, alpha


HEADER = """/**
 * @file weather_icons.c
 * @brief 天气图标（内嵌，LV_COLOR_FORMAT_RGB565A8，48x48）—— **自动生成，勿手改**。
 *
 * 由 tools/gen_weather_icons.py 从 third_party/white/1x/<code>.png 生成。
 * 保留原始 alpha（真透明），任何底色上都干净，不再有黑底方块。
 *
 * 数据布局（LVGL v9 要求两个平面连续）：
 *     [RGB565: WIC_W*WIC_H*2 字节，stride = WIC_W*2]
 *     [A8    : WIC_W*WIC_H   字节，stride = WIC_W  ]
 * 所以每张图是一个 prv_icon_t（rgb[] 紧跟 a[]），下面有编译期断言兜底。
 *
 * 用法：
 *     lv_image_set_src(img, app_weather_icon(code));
 */

#include "weather_icons.h"
#include "app_weather.h"   /* APP_WEATHER_CODE_UNKNOWN（未知回退用） */

"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只体检，不写文件")
    args = ap.parse_args()

    paths = glob.glob(os.path.join(SRC_DIR, "*.png"))
    if not paths:
        print("[err] 没找到 PNG：%s" % SRC_DIR)
        return 1

    icons = []
    for p in sorted(paths, key=lambda s: int(os.path.splitext(os.path.basename(s))[0])):
        code = int(os.path.splitext(os.path.basename(p))[0])
        w, h, rgba = read_png_rgba(p)
        nw, nh = fit_size(w, h)
        if (nw, nh) != (w, h):
            rgba = box_scale_rgba(rgba, w, h, nw, nh)
        rgb, alpha = to_canvas(rgba, nw, nh)
        icons.append((code, w, h, nw, nh, rgb, alpha))

        # 体检：确认图不是空的（有可见像素）
        vis = sum(1 for a in alpha if a > 8)
        if vis * 100 < len(alpha):              # <1% 可见 = 可疑
            print("[warn] %d.png 输出后几乎全透明（可见像素 %d/%d）"
                  % (code, vis, len(alpha)))

    per = W * H * 3
    print("图标 %d 张 -> 画布 %dx%d RGB565A8，%d B/张，合计 %d B (%.1f KB)"
          % (len(icons), W, H, per, per * len(icons), per * len(icons) / 1024))
    for code, w, h, nw, nh, _, _ in icons:
        mark = "  " if (nw, nh) == (w, h) else "->"
        print("  %-6d 原图 %2dx%-3d %s %2dx%-3d" % (code, w, h, mark, nw, nh))

    if args.check:
        return 0

    parts = [HEADER]
    parts.append("#define WIC_W  %d\n#define WIC_H  %d\n"
                 "#define WIC_BYTES (WIC_W * WIC_H * 3)   /* RGB565 + A8 */\n\n" % (W, H))
    parts.append("typedef struct {\n"
                 "    uint16_t rgb[WIC_W * WIC_H];   /* RGB565 平面 */\n"
                 "    uint8_t  a  [WIC_W * WIC_H];   /* A8 平面，必须紧跟在 RGB 后面 */\n"
                 "} prv_icon_t;\n\n")
    parts.append("/* 编译期兜底：结构体不能有填充，且 alpha 平面偏移必须是 w*h*2 ——\n"
                 " * LVGL 就是靠 `data + stride*h` 找 alpha 的（见 lv_draw_sw_img.c）。 */\n"
                 "typedef char prv_icon_layout_ok[\n"
                 "    (sizeof(prv_icon_t) == WIC_BYTES) ? 1 : -1];\n\n")

    for code, w, h, nw, nh, rgb, alpha in icons:
        parts.append("/* %d.png 原图 %dx%d -> %dx%d（真透明） */\n" % (code, w, h, nw, nh))
        parts.append("static const prv_icon_t s_icon_%d = {\n  {\n" % code)
        for i in range(0, len(rgb), VALS_PER_LINE):
            parts.append("    " + ",".join("0x%04x" % v for v in rgb[i:i + VALS_PER_LINE]) + ",\n")
        parts.append("  },\n  {\n")
        for i in range(0, len(alpha), A_PER_LINE):
            parts.append("    " + ",".join("%3d" % v for v in alpha[i:i + A_PER_LINE]) + ",\n")
        parts.append("  },\n};\n\n")
        parts.append("static const lv_image_dsc_t s_dsc_%d = {\n"
                     "    .header.magic  = LV_IMAGE_HEADER_MAGIC,\n"
                     "    .header.cf     = LV_COLOR_FORMAT_RGB565A8,\n"
                     "    .header.w      = WIC_W,\n"
                     "    .header.h      = WIC_H,\n"
                     "    .header.stride = WIC_W * 2,      /* 只有 RGB 平面有 stride；A8 = stride/2 */\n"
                     "    .data_size     = WIC_BYTES,\n"
                     "    .data          = (const uint8_t *)&s_icon_%d,\n"
                     "};\n\n" % (code, code))

    parts.append("typedef struct {\n    int32_t                 code;\n"
                 "    const lv_image_dsc_t   *dsc;\n} prv_icon_entry_t;\n\n")
    parts.append("static const prv_icon_entry_t s_icons[] = {\n")
    for code, _, _, _, _, _, _ in icons:
        parts.append("    { %d, &s_dsc_%d },\n" % (code, code))
    parts.append("};\n\n")

    parts.append("""#define ICON_NUM (sizeof(s_icons) / sizeof(s_icons[0]))

const lv_image_dsc_t *app_weather_icon(int32_t code)
{
    for (size_t i = 0; i < ICON_NUM; i++) {
        if (s_icons[i].code == code) {
            return s_icons[i].dsc;
        }
    }

    /* 未知代码回退到 99（心知的"未知"）；连 99 都没有就返回 NULL，
     * lv_image_set_src(img, NULL) 是"清空"，控件不会崩。 */
    for (size_t i = 0; i < ICON_NUM; i++) {
        if (APP_WEATHER_CODE_UNKNOWN == s_icons[i].code) {
            return s_icons[i].dsc;
        }
    }
    return NULL;
}
""")

    os.makedirs(os.path.dirname(OUT_C), exist_ok=True)
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(parts))

    print("[ok] 写出 %s (%d B)" % (OUT_C, os.path.getsize(OUT_C)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
