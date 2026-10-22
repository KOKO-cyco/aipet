#!/usr/bin/env python3
"""third_party/新动作视频 的 WebM 素材 -> 带 alpha 的 PNG 序列，供 pack_seq.py 打包。

**必须用 `-c:v libvpx-vp9` 解码（这一项永远不能省）。**
ffmpeg 的原生 `vp9` 解码器**不读 alpha 平面** —— 用默认解码器解出来的帧没有
alpha，`ffprobe` 也把 `pix_fmt` 报成 `yuv420p`。这会让人以为"素材根本没透明
通道"（2026-10-17 就在这上面判断错过一次：抽帧命令漏了 `-c:v libvpx-vp9`，
把 native 解码器填的假 alpha=255 当成了无 alpha）。实际是：alpha 好好地在文件
里，只要显式指定 libvpx 解码器。

VP9 alpha 编码时**透明区的 RGB 就是纯黑 (0,0,0)**，所以系统自带播放器
（照片查看器 / Windows Media Player / QuickTime）播这类 webm 会显示黑底，而不是
透明—— 这也不能反证文件没有 alpha。Chrome/Edge 直接拖进去才看得到透明。

另一个容易看走眼的点：这些片段的**背景 alpha 是 1 而不是 0**。1/255 和全透明
肉眼没区别，但按"alpha 是不是 0"去判断会觉得蒙版是坏的。pack_seq.py 的
ALPHA_MIN=16 天然把 1 当透明，不用特殊处理。

**静止帧裁剪（2026-10-18 用户要求）**：这批素材里动作演完后画面会僵住
（相邻帧几乎完全一致），实测 16 条尾部静止 0.25~1.29s、中段最长 2.12s。
抽帧时自动按 TRIM_* 常量裁掉多余静止帧：

    尾部静止段只保留最后 1 帧（2026-10-18 用户改主意：末尾重复只留一帧）；
    中段静止段超过 TRIM_MID_MAX 对的裁到只保留 TRIM_MID_MAX 对（<=0.5s）；
    **开头部的静止段一律不裁**（用户只点名了尾段和中段）。

删的是完全相同的帧，不改变任何一帧的内容，也不碰第 1 帧（pack_seq 的
`--align first` 对齐基准、锚点框都不变）。待机组（第 0 组）同样适用。

输出布局（pack_seq.py 要的样子）：

    <out>/<clip>/0001.png, 0002.png, ...

用法：

    sh third_party/sprite/build_fish.sh          # 一步到位：webm -> PNG 序列 -> fish.spf

或者手动两步：

    python third_party/sprite/fish_extract.py --out third_party/resources/fish_seq
    python third_party/sprite/pack_seq.py --src third_party/resources/fish_seq \
        --out third_party/fish/fish_elf.spf \
        --height 156 --ref-action idle --src-fps 24 --fps 24 --idle-frames 48 \
        --order idle,tap_proud,tap_shy,tap_happy,tap_tickle,fn_photo,fn_note,emo_sad,emo_happy,emo_think,rnd_gomoku,rnd_pigeon,rnd_yawn,rnd_cat,rnd_lion,rnd_scare

（完整的两个文件、各自该用哪些参数，见 `third_party/sprite/build_fish.sh` —— 那里是唯一权威。）

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

from PIL import Image

# 源素材根目录（相对本脚本）
SRC_ROOT = "my_pet/豆包视频"      # 相对本脚本（third_party/sprite/）

# 中间分辨率。源是 640x360，角色本体约 216x270；目标成品高 120px，所以按
# **一半分辨率**取中间帧就够了（角色约 108x135 -> 缩到 120 高是 0.89 倍），
# 再大只是白白吃磁盘。
INTER_W, INTER_H = 554, 312

# ---- 静止帧裁剪参数（2026-10-18 用户定：末尾重复只留一帧、中段最多 0.5s）----
# 检测在读好的 PNG 上做（缩到 64x36 灰度，与打包同一条像素路径）。
TRIM_W, TRIM_H = 64, 36
TRIM_MAD_TH = 0.6        # 相邻帧平均绝对差（0-255）低于此值算"没动"
TRIM_RATIO_TH = 0.002    # 且 |d|>8 的像素占比低于此值
TRIM_TAIL_KEEP = 1       # 尾部静止段只保留最后 1 帧（用户 2026-10-18 改主意）
TRIM_MID_MAX = 12        # 中段静止段超过 12 对（0.5s）就裁到 12 对

# 片段表：输出名 -> 源 .webm（相对 SRC_ROOT）。
#
# **第一个是待机组**：pack_seq.py 把它当常驻动作，按 --idle-frames 抽帧、
# 并降 fps 保住原时长。所以顺序不能随便调。
#
# 名字用 ASCII：它们会进 SD 卡路径和 C 代码里的映射表。
CLIPS = [
    # 素材根 = third_party/新动作视频/（用户 2026-10-18 更新的第四套动作集，
    # 共 16 条，**文件名本身就说明了用途**）：
    #
    #   待机              x1   常驻 PSRAM
    #   点击回应-*        x4   点宠物时随机演
    #   功能-*            x2   拍照 / 记笔记（AI 工具或流程触发）
    #   表情-*            x3   情绪驱动（伤心 / 开心 / 思考）
    #   随机-*            x6   待机时随机插播（照镜子、东张西望已按用户要求删掉）
    #
    # 组名前缀就是用途编码（tap_ / fn_ / emo_ / rnd_），固件按前缀分组取用；
    # 顺序无所谓（固件按名字找组），但**第 0 组必须是 idle**（常驻）。
    #
    # **这批素材自带 alpha**，但抽帧必须带 `-c:v libvpx-vp9`（VP9 的 alpha 在
    # 侧数据里，native vp9 解码器不读，见文件头）。
    #
    # 加新动作：webm 丢进 third_party/新动作视频/，这里加一行，再同步
    # build_fish.sh 的 ELF_ORDER 和固件里的几张表。

    # ---- 待机（常驻）----
    # **第一个必须是待机组**（pack_seq 按 --idle-frames 抽帧、降 fps 保时长）。
    # 单周期由 build_fish.sh 第 0 步现量（find_loop.py），产物是 待机_单周期.webm。
    ("idle",            "待机_单周期.webm"),

    # ---- 点击回应 x4 ----
    ("tap_proud",       "点击回应-傲娇生气.webm"),
    ("tap_shy",         "点击回应-害羞惊讶.webm"),
    ("tap_happy",       "点击回应-开心跃动.webm"),
    ("tap_tickle",      "点击回应-挠痒咯咯笑.webm"),

    # ---- 功能 x2 ----
    ("fn_photo",        "功能-拍照.webm"),
    ("fn_note",         "功能-记笔记.webm"),

    # ---- 表情 x3 ----
    ("emo_sad",         "表情-伤心.webm"),
    ("emo_happy",       "表情-开心.webm"),
    ("emo_think",       "表情-思考.webm"),

    # ---- 随机表演 x6 ----
    ("rnd_gomoku",      "随机-下五子棋.webm"),
    ("rnd_pigeon",      "随机-变鸽子.webm"),
    ("rnd_yawn",        "随机-哈欠连天.webm"),
    ("rnd_cat",         "随机-撸猫.webm"),
    ("rnd_lion",        "随机-舞狮头.webm"),
    ("rnd_scare",       "随机-被吓一跳.webm"),
]


def ffmpeg_bin():
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit("找不到 ffmpeg（PATH 里没有）")
    return exe


def is_static(a, b):
    """两帧是否"几乎没动"：MAD 与变化像素占比双低于阈值。"""
    n = len(a)
    tot = 0
    ch = 0
    for x, y in zip(a, b):
        d = x - y if x > y else y - x
        tot += d
        if d > 8:
            ch += 1
    return tot / n < TRIM_MAD_TH and ch / n < TRIM_RATIO_TH


def png_gray(path):
    """读一张已抽好的 PNG，缩到 TRIM 尺寸转灰度字节。"""
    im = Image.open(path).convert("RGBA").resize((TRIM_W, TRIM_H))
    return im.convert("L").tobytes()


def trim_plan(out_dir):
    """算裁剪方案（迭代到不动点）。返回 (删除帧号集合, 说明串)。

    帧号 1-based。两个要点：

    **在抽好的 PNG 上检测**（与打包输入同一条像素路径，避免"webm 直接缩小"
    和"PNG 再缩小"在段边界帧上的判定分歧 —— 2026-10-18 实测 emo_happy
    两条路径差 1 对）。

    **为什么要迭代**：删帧会改变邻接关系。run 中间常有一帧"和两边都差一点"
    的孤帧，删掉它以后前后帧成了新邻居且彼此几乎一致，静止段反而变长
    （2026-10-18 实测 tap_tickle：删 1 帧后尾部静止从 5 帧变成 7 帧）。
    所以每删一轮都要在**幸存序列**上重新检测，直到没有可删的为止。
    灰帧只读一遍，迭代全在内存里做（上限 64 轮，每轮至少删 1 帧，
    帧数单调递减，绝不会跑满）。

    每轮规则相同：尾部连续相同帧只保留最后 TRIM_TAIL_KEEP 帧；
    非头非尾的静止段超过 TRIM_MID_MAX 对的裁到只留 TRIM_MID_MAX 对；
    开头部一律不裁。
    """
    files = sorted(f for f in os.listdir(out_dir) if f.endswith(".png"))
    n = len(files)
    if n < 2:
        return set(), ""
    frames = [png_gray(os.path.join(out_dir, f)) for f in files]

    alive = list(range(1, n + 1))       # 幸存帧的原始帧号（升序）
    dead = set()
    tail_rounds = 0
    tail_last = 0
    mid_notes = []

    for _ in range(64):
        m = len(alive)
        if m < 2:
            break
        sub = [frames[k - 1] for k in alive]
        # ds[k]（0-based）= 幸存序列第(k+1)帧 -> 第(k+2)帧 是否静止
        ds = [is_static(sub[k], sub[k + 1]) for k in range(m - 1)]

        dels = set()                    # alive 空间里的位置（1-based）

        # ---- 尾部：从后往前数连续静止对数 t；相同帧数 = t+1 ----
        t = 0
        for s in reversed(ds):
            if s:
                t += 1
            else:
                break
        # t < m-1 是保险：整片全静止时不动它（只可能是坏素材，不能裁成 1 帧）
        tail_fired = t + 1 > TRIM_TAIL_KEEP and t < m - 1
        if tail_fired:
            # 相同帧占末尾 t+1 帧，保留最后 KEEP 帧，删前面的
            dels.update(range(m - t, m - TRIM_TAIL_KEEP + 1))

        # ---- 中段：扫描所有连续静止段；跳过头段（位置 1）和尾段 ----
        i = 0
        while i < len(ds):
            if ds[i]:
                j = i
                while j < len(ds) and ds[j]:
                    j += 1
                pairs = j - i            # 该段的对数（相同帧数 = pairs+1）
                if pairs > TRIM_MID_MAX and i != 0 and j != len(ds):
                    first = i + 1        # 段内第 1 个相同帧（alive 位置）
                    last = j + 1         # 段内最后 1 个相同帧
                    keep_until = first + TRIM_MID_MAX   # 保留前 MID_MAX+1 帧
                    dels.update(range(keep_until + 1, last + 1))
                    mid_notes.append("中段 %d..%d 帧（%d 对）-> 留 %d 对"
                                     % (alive[first - 1], alive[last - 1],
                                        pairs, TRIM_MID_MAX))
                i = j
            else:
                i += 1

        if not dels:
            break
        if tail_fired:
            tail_rounds += 1
            tail_last = t + 1
        for p in dels:
            dead.add(alive[p - 1])
        alive = [k for k in alive if k not in dead]

    notes = []
    if tail_rounds:
        notes.append("尾部 %d 帧 -> %d 帧（%d 轮）"
                     % (tail_last, TRIM_TAIL_KEEP, tail_rounds))
    merged = []
    for x in mid_notes:
        if merged and merged[-1][0] == x:
            merged[-1][1] += 1
        else:
            merged.append([x, 1])
    notes.extend(("%s x%d" % (t, c)) if c > 1 else t for t, c in merged)
    return dead, "; ".join(notes)


def apply_trim(out_dir, dels):
    """按删除帧号集合删帧并把剩余帧重连续号。返回实际删除数。"""
    files = sorted(f for f in os.listdir(out_dir) if f.endswith(".png"))
    keep = [f for idx, f in enumerate(files, start=1) if idx not in dels]
    for idx, f in enumerate(files, start=1):
        if idx in dels:
            os.remove(os.path.join(out_dir, f))
    if len(keep) == len(files):
        return 0
    # 两步 rename：先集体挪到临时名，再按序命名（避免 0005->0004 覆盖串号）
    tmp = []
    for idx, f in enumerate(keep, start=1):
        src = os.path.join(out_dir, f)
        dst = os.path.join(out_dir, "tmp_%04d.png" % idx)
        os.rename(src, dst)
        tmp.append(dst)
    for idx, t in enumerate(tmp, start=1):
        os.rename(t, os.path.join(out_dir, "%04d.png" % idx))
    return len(files) - len(keep)


def extract(ff, webm, out_dir):
    """一个 .webm -> out_dir/0001.png ... 返回 (帧数, 删除数, 裁剪说明)。"""
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

    n = len([f for f in os.listdir(out_dir) if f.endswith(".png")])

    # 静止帧裁剪（trim_plan 内部已迭代到不动点，这里只应用一次）
    dels, why = trim_plan(out_dir)
    removed = apply_trim(out_dir, dels) if dels else 0
    return n, removed, why


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
    total_removed = 0
    print("素材根: %s\n中间分辨率: %dx%d\n输出: %s\n" % (root, INTER_W, INTER_H, args.out))

    for name, rel in CLIPS:
        if only and name not in only:
            continue
        webm = os.path.join(root, rel)
        if not os.path.isfile(webm):
            print("  [跳过] %-14s 源文件不存在: %s" % (name, rel))
            continue

        out_dir = os.path.join(args.out, name)
        n, removed, why = extract(ff, webm, out_dir)
        total += n - removed
        total_removed += removed
        if removed:
            print("  %-14s %3d -> %3d 帧  <- %s  [裁 %d: %s]"
                  % (name, n, n - removed, rel, removed, why))
        else:
            print("  %-14s %3d 帧  <- %s" % (name, n, rel))

    print("\n合计 %d 帧（静止裁剪共删除 %d 帧）" % (total, total_removed))


if __name__ == "__main__":
    main()
