#!/usr/bin/env python3
"""blue_big_fat_fish 的 WebM 素材 -> 带 alpha 的 PNG 序列，供 pack_seq.py 打包。

**必须用 `-c:v libvpx-vp9` 解码。**
ffmpeg 的原生 `vp9` 解码器**不读 alpha 平面** —— 用默认解码器解出来的帧没有
alpha，`ffprobe` 也把 `pix_fmt` 报成 `yuv420p`。这会让人以为"素材根本没透明
通道"（本项目 2026-10-14 就在这上面判断错过一次）。实际是：alpha 好好地在文件
里，只是要显式指定 libvpx 解码器。

另一个容易看走眼的点：这些片段的**背景 alpha 是 1 而不是 0**。1/255 和全透明
肉眼没区别，但按"alpha 是不是 0"去判断会觉得蒙版是坏的。pack_seq.py 的
ALPHA_MIN=16 天然把 1 当透明，不用特殊处理。

输出布局（pack_seq.py 要的样子）：

    <out>/<clip>/0001.png, 0002.png, ...

用法：

    sh tools/build_fish.sh          # 一步到位：webm -> PNG 序列 -> fish.spf

或者手动两步：

    python tools/fish_extract.py --out third_party/resources/fish_seq
    python tools/pack_seq.py --src third_party/resources/fish_seq \\
        --out third_party/fish/fish_elf.spf \\
        --height 156 --ref-action idle --src-fps 24 --fps 24 --idle-frames 48 \\
        --order idle,snap,snap_l,click_angry,click_wave,click_shy,click_happy,\\
drag_up,drag_hold,drag_down,talk,rnd_lookaround,rnd_float,rnd_scare,\\
rnd_breakfast,rnd_lunch,rnd_dinner,rnd_yawn,rnd_whale

（完整的两个文件、各自该用哪些参数，见 `tools/build_fish.sh` —— 那里是唯一权威。）

**`--ref-action idle` 不能省。** pack_seq.py 默认的基准动作是 `idle_normal`，这套
素材里**没有这个名字**，于是它会悄悄退回"所有动作的并集高度"（161px 而不是 137px）
—— 结果整个文件等比缩小 1.162 倍，而且不报错。2026-10-14 就是靠 `cmp` 逐字节
对比才发现打包命令少了这个参数。

**`snap` / `snap_l`（贴边那两张）不在下面的 CLIPS 表里** —— 它们是从单张
`屏幕右边缘吸附.png` 单独做的，目录已经躺在 `fish_seq/` 里了，`--order` 里带上
就行。fish_extract.py 不会动它们。

**片段顺序即打包顺序，第 0 个是常驻 PSRAM 的待机组** —— 见 CLIPS 和 `--order`。
"""

import argparse
import os
import shutil
import subprocess
import sys

# 源素材根目录（相对本脚本）
SRC_ROOT = "../third_party/blue_big_fat_fish"

# 中间分辨率。源是 640x360，角色本体约 216x270；目标成品高 120px，所以按
# **一半分辨率**取中间帧就够了（角色约 108x135 -> 缩到 120 高是 0.89 倍），
# 再大只是白白吃磁盘。
INTER_W, INTER_H = 320, 180

# 片段表：输出名 -> 源 .webm（相对 SRC_ROOT）。
#
# **第一个是待机组**：pack_seq.py 把它当常驻动作，按 --idle-frames 抽帧、
# 并降 fps 保住原时长。所以顺序不能随便调。
#
# 名字用 ASCII：它们会进 SD 卡路径和 C 代码里的映射表。
CLIPS = [
    # 素材统一从 `动作视频优化版/` 取 —— 那是**人工剪过的版本**（去死区、砍多余循环、
    # 整体变速），已经替代了原始 webm。原始素材留在同级目录（click/ random/ move/
    # turn/ idle/ drag/）作为备份，不要再直接引用。
    #
    # ---- 待机（常驻） ----
    # **2026-10-16 换成豆包那一版**（`豆包生成动作/待机.webm`，361 帧）。
    # 旧的 `idle/待机摇摆.webm`（74 帧）是当年从 `video(1)` 抠出来、再拿
    # normalize_clip.py 对齐到旧取景的产物。豆包这一版是**一整套重新生成**的
    # （待机/开心/伤心/思考/拍照），彼此的角色大小和站位本来就一致
    # （用户的 `角色一致性检验.jpg` 量过：躯干 ±0.6%、高 ±0.4%），
    # 所以整套换成豆包版，不用再逐段手工对齐。
    #
    # **但原片 15 秒（361 帧）不能直接进** —— 待机组整组常驻 PSRAM，帧数是硬预算
    # （`--idle-frames 48`）。361 帧抽到 48 帧只剩 **3fps**，待机变成幻灯片。
    # 所以先用 clip_cut.py 裁出**一个摇摆周期**：
    #
    #     py -3 tools/clip_cut.py 豆包生成动作/待机.webm     #         --out 豆包生成动作/待机_单周期.webm --keep 50:117
    #
    # 帧号是 tools/find_loop.py 量出来的（它找接缝最小的循环段）：50..116 共
    # 67 帧 / 2.79s，接缝 0.65x 帧步、内部幅度 90%（= 完整摆了一轮）。
    # 67 帧抽到 48 帧 = 17fps，和旧的 74 帧 / 3.08s 一个量级。
    #
    # **换 idle 会动整份几何** —— 它是 `--ref-action`，pack_seq 的缩放比、
    # 锚点框都从它推。换完必须重跑打包并核对 build_fish.sh 打出来的锚点框，
    # 以及 `[sp2d] 已加载` 那条日志里的"角色高"。
    ("idle",            "豆包生成动作/待机_单周期.webm"),

    # ---- 点击回应 ×4 ----
    # **挠痒咯咯笑（click_tickle）2026-10-15 删掉了**：那一版素材头顶的呆毛是坏的
    # （细发丝抠不干净，边缘发暗、比别的动作粗一圈，放大一眼能看出来）。
    # 源文件还在 `动作视频优化版/点击回应-挠痒咯咯笑.webm`，重新生成一版干净的
    # 就能加回来 —— 加回来要同时改 build_fish.sh 的 --order、page_char.c 的情绪表、
    # app_avatar_tools.c 的 AVATAR_ANIM_NAMES。
    ("click_angry",     "动作视频优化版/点击回应-傲娇生气.webm"),
    ("click_wave",      "动作视频优化版/点击回应-元气挥手.webm"),
    ("click_shy",       "动作视频优化版/点击回应-害羞惊讶.webm"),
    ("click_happy",     "动作视频优化版/点击回应-开心跃动.webm"),

    # ---- 拖拽：**三段**，不是一段 ----
    # 拖拽是状态驱动的：按下 -> 起手 -> 悬空 -> 松手 -> 落地。
    # 三段是从 `动作视频优化版/被鼠标拖拽悬空反馈.webm`（130 帧）切出来的：
    #     0..13   起飘（底沿 328->296）
    #     13      悬空 —— **只有一帧**。源素材里 13..115 那 4 秒本来就是定住的
    #             （逐帧能量≈0），留一帧就够了；按多久飘多久由手指决定，
    #             不需要拿 100 多帧去凑时长。
    #     112..129 落地
    # 切点用 tools/clip_probe.py 的"底沿"曲线量的（底沿 = 脚的位置，越小=越高）。
    # 要重切就照上面这组帧号跑 tools/clip_cut.py --keep。
    ("drag_up",         "动作视频优化版/drag_up.webm"),
    ("drag_hold",       "动作视频优化版/drag_hold.webm"),
    ("drag_down",       "动作视频优化版/drag_down.webm"),

    # ---- AI 回复（对屏碎碎念）----
    ("talk",            "动作视频优化版/碎碎念-对屏碎碎念.webm"),

    # ---- AI 表情 ×3（开心 / 思考 / 伤心）+ 拍照 ----
    # **2026-10-16 全部换成豆包那一版**（`豆包生成动作/`）。旧的 `ai/*.webm` 是
    # 当年一条条抠出来、再拿 normalize_clip.py 对齐取景的；豆包这一版是整套
    # 重新生成的，尺寸站位天生一致（见上面 idle 那段），处理链短得多。
    # 旧的 ai/*.webm 留着做备份，不要再引用。
    #
    # 它们在 pack_seq.py 里的 role 是 ROLE_AI（不进内置随机池）。**但精灵里也要带
    # 上这三组** —— 对话时宠物要按情绪演开心/思考/伤心，见 page_pet.c 的情绪表。
    # 桌宠页用的是自己的动作状态机 + app_sprite2d_render_clip()，不走内置随机池，
    # 所以 ROLE_AI 这个标记对我们没有影响。
    #
    # **think 换了源 = 帧数变了**（163 -> 241），page_pet.c 的 `sg_loop[]` 里那条
    # `{ "think", 71, 122, 163 }` 就失效了 —— 打包时会打警告，要重跑
    #     py -3 tools/find_loop.py third_party/resources/fish_seq/think
    # 把新的循环区间抄回去。
    ("happy",           "豆包生成动作/开心.webm"),
    ("think",           "豆包生成动作/思考.webm"),
    ("sad",             "豆包生成动作/伤心.webm"),

    # 拍照：新动作（2026-10-16 加）。名字前缀 `rnd_` = 进内置随机池（role 3），
    # 也进 page_pet.c 的 sg_idle_acts[] 待机插播表。
    ("rnd_photo",       "豆包生成动作/拍照.webm"),

    # ---- 随机 ×8 ----
    ("rnd_lookaround",  "动作视频优化版/工作状态-原地踱步张望.webm"),
    ("rnd_float",       "动作视频优化版/漂浮踏步.webm"),
    ("rnd_scare",       "动作视频优化版/被吓一跳.webm"),
    ("rnd_breakfast",   "动作视频优化版/吃早餐.webm"),
    ("rnd_lunch",       "动作视频优化版/吃午餐.webm"),
    # **吃晚餐 2026-10-15 删掉了**：它是**锚点框的宽度元凶** —— 带三盘菜，
    # 裁剪框比角色本体宽一倍多，把"所有动作的并集"整个撑开，导致角色本体
    # 只能画得很小（锚点框宽 ≈ 1.75 × 角色高）。删掉它锚点框就窄下来了，
    # 同样的屏幕里角色能画大得多。源文件还在 `动作视频优化版/吃晚餐.webm`。
    ("rnd_yawn",        "动作视频优化版/哈欠连天.webm"),
    # **蓝鲸现世留着**（2026-10-15 试删过又放回来了）：它也很宽（帧 199px、
    # 偏移只到 10，删完吃晚餐之后就是它在顶并集的左右两边），但用户要先看效果
    # 再决定。真想再窄一截的时候，删它是最划算的一刀。
    ("rnd_whale",       "动作视频优化版/蓝鲸现世.webm"),
]


def ffmpeg_bin():
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit("找不到 ffmpeg（PATH 里没有）")
    return exe


def extract(ff, webm, out_dir):
    """一个 .webm -> out_dir/0001.png ... 返回帧数。"""
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)

    # -c:v libvpx-vp9 ：**唯一能解出 alpha 的方式**，见文件头。
    # -pix_fmt rgba   ：交给 png 编码器时带上 alpha 通道。
    # scale=-2:INTER_H：降到中间分辨率，省磁盘也省后面 pack_seq 的内存。
    #   **宽度写 -2（自动、取偶数）而不是固定值** —— 素材里有竖屏的
    #   （video(1) 是 720x960），固定成 320 宽会被压扁。横屏素材算出来还是
    #   320x180，和以前完全一样。
    cmd = [
        ff, "-v", "error", "-y",
        "-c:v", "libvpx-vp9",
        "-i", webm,
        "-vf", "scale=-2:%d" % INTER_H,
        "-pix_fmt", "rgba",
        os.path.join(out_dir, "%04d.png"),
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("ffmpeg 失败 (%s):\n%s" % (webm, r.stderr.strip()))

    return len([f for f in os.listdir(out_dir) if f.endswith(".png")])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="输出的 png_sequences 目录")
    ap.add_argument("--src-root", default=None,
                    help="素材根目录（默认 %s，相对本脚本）" % SRC_ROOT)
    ap.add_argument("--only", default=None,
                    help="只处理这些片段（逗号分隔），调试用")
    args = ap.parse_args()

    root = args.src_root or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         SRC_ROOT)
    if not os.path.isdir(root):
        sys.exit("找不到素材目录: %s" % root)

    ff = ffmpeg_bin()
    only = set(args.only.split(",")) if args.only else None
    os.makedirs(args.out, exist_ok=True)

    total = 0
    print("素材根: %s\n中间分辨率: %dx%d\n输出: %s\n" % (root, INTER_W, INTER_H, args.out))

    for name, rel in CLIPS:
        if only and name not in only:
            continue
        webm = os.path.join(root, rel)
        if not os.path.isfile(webm):
            print("  [跳过] %-14s 源文件不存在: %s" % (name, rel))
            continue

        out_dir = os.path.join(args.out, name)
        n = extract(ff, webm, out_dir)
        total += n
        print("  %-14s %3d 帧  <- %s" % (name, n, rel))

    print("\n合计 %d 帧" % total)


if __name__ == "__main__":
    main()
