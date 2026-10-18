#!/bin/sh
# 2D 宠物：素材 webm -> PNG 序列 -> fish_elf.spf，一步到位。
#
#     sh tools/build_fish.sh
#
# 产出（拷到 SD 卡的 /sprite/ 下）：
#   third_party/fish/fish_elf.spf   桌宠精灵（app_sprite2d.c + page_pet.c）
#
# **动作集 = third_party/新动作视频/ 下的 16 条**（2026-10-18 用户更新，第四套；
# 照镜子、东张西望已按用户要求从打包链里删掉，源 webm 仍留在目录里）：
#   待机 x1 / 点击回应 x4 / 功能 x2 / 表情 x3 / 随机 x6。文件名即用途。
# 文件名 -> 固件组名的映射在 tools/fish_extract.py 的 CLIPS 表；固件侧要认这
# 些名字的表在 page_pet.c（sg_tap_acts / sg_idle_acts / sg_loop / 情绪表 /
# 巡检表 sg_audit）和 app_avatar_tools.c（MCP 白名单）。加新动作 = 视频丢进
# third_party/新动作视频/ + CLIPS 加一行 + 下面 ELF_ORDER 追加 + 固件几张表
# 同步。
#
# **这批素材自带 alpha，但抽帧必须 `-c:v libvpx-vp9`**：VP9 的 alpha 在侧数据
# 里，native vp9 解码器不读（2026-10-17 在这误判过一次"素材无 alpha"，详见
# tools/fish_extract.py 文件头）。系统播放器显示黑底 ≠ 无 alpha。
#
# **`--ref-action idle` 千万别省** —— pack_seq.py 默认基准动作 `idle_normal`
# 在这套素材里不存在，会静默退回"所有动作的并集高度"，整个文件等比缩小且不报错。
#
# **ELF_ORDER 的顺序 = 固件里的组号**，第 0 组（idle）整组常驻 PSRAM，必须排第一。
# 固件是按**名字**找组号的（SPF7 名字表），顺序改了不会静默错位。
#
# **ELF_ORDER 一动就重看锚点框**：锚点框 = 所有动作内容框的并集，加一组宽的
# 动作会把整套撑小（角色能画多大由它决定）。pack_seq.py 末尾会打出来。
#
# 尺寸由 pack_seq.py 自动归一（`--fit follow` -> `--align first`，按各动作
# 首帧角色高算缩放），不需要逐动作微调。`--shrink-default 14` 是**全体一起缩**；
# **不要给单个动作加 `--shrink 名字=M`**（2026-10-16 之前待机单独缩过 10px，
# 切动作时头会跳一下 —— 用户的硬要求是"角色位置、大小要完全一样"）。
#
# **`--cap fn_photo=160x206` 别删**：拍照那段有个"白色闪光星爆"特效，铺满源
# 画幅的 93%（513x312），把这一组的裁剪框撑到 321x199 —— 既超过固件单帧上限
# （SP2D_MAX_W/H = 288x208，这组会被拒绝加载），又会把**锚点框（屏幕上画布
# 大小）**从 166 撑到 352（比屏还宽）。`--cap` 绕角色站位点限一个窗口，把溢出
# 的闪光裁掉，角色一个像素都不动。**换了拍照素材要重看这一项**：先不带 --cap
# 跑一遍，看它报的帧尺寸和锚点框再定。
#
# 依赖：ffmpeg（PATH 里，带 libvpx-vp9 解码；imageio-ffmpeg 的静态构建就够）
# + numpy/PIL。不要求 ffprobe —— clip_cut.py 没有它会从 ffmpeg stderr 解析尺寸。
# 用哪个 python：本机 `py -3` 有（项目 venv 里没有），要换就
# `PY=python3 sh tools/build_fish.sh`。
set -e

cd "$(dirname "$0")/.."
PY="${PY:-py -3}"

ELF_ORDER="idle,tap_proud,tap_shy,tap_happy,tap_tickle,fn_photo,fn_note,emo_sad,emo_happy,emo_think,rnd_gomoku,rnd_pigeon,rnd_yawn,rnd_cat,rnd_lion,rnd_scare"

echo "== 0/3 待机：砍出一个摇摆单周期 =="
# 原片 4.04s / 97 帧。待机组整组常驻 PSRAM、帧数是硬预算（--idle-frames 48）：
# 97 帧直接抽只剩 12fps，待机变幻灯片。所以先量出接缝够好的循环段、砍出来，
# 再抽到 48 帧才够顺。帧号由 find_loop.py 现量（别手抄旧数字）。
mkdir -p third_party/resources/fish_seq/idle
ffmpeg -v error -y -c:v libvpx-vp9 \
    -i third_party/新动作视频/待机.webm \
    -vf scale=-2:312 -pix_fmt rgba \
    third_party/resources/fish_seq/idle/%04d.png
RANGE=$($PY tools/find_loop.py third_party/resources/fish_seq/idle | awk '
    /^  -+ *$/ { intab = 1; next }          # 候选表的分隔线
    intab && $1 ~ /^[0-9]+$/ && $4 ~ /^[0-9]+$/ {
        if ($4 + 0 <= 72) { print $2, $3; exit }   # 帧数 <=72：抽到 48 帧后 >=16fps
    }')
# **不能直接取 find_loop 的"推荐"**：那是"接缝够好里挑最长"，而待机组只有
# 48 帧的常驻预算 —— 段一长就被抽成幻灯片。要的是"接缝够好里挑**短**的"：
# 一个摇摆单周期。
A=$(echo "$RANGE" | cut -d' ' -f1)
B=$(echo "$RANGE" | cut -d' ' -f2)
if [ -z "$A" ] || [ -z "$B" ]; then
    echo "find_loop.py 没给出 <=72 帧的循环段，先看一眼它的输出再手动填 --keep"
    exit 1
fi
echo "  循环段：第 $A..$B 帧（$((B - A + 1)) 帧）"
# clip_cut 的 A:B 含 A 不含 B，所以末帧 +1
$PY tools/clip_cut.py third_party/新动作视频/待机.webm \
    --out third_party/新动作视频/待机_单周期.webm --keep "$A:$((B + 1))"

echo
echo "== 1/3 webm -> PNG 序列 =="
$PY tools/fish_extract.py --out third_party/resources/fish_seq

echo
echo "== 2/3 精灵（fish_elf.spf）：待机常驻 + 跨动作位置/尺寸归一 =="
# `--align first --fit follow`：位置和尺寸都按**首帧**归一 —— 切动作时看到的
# 就是新动作的首帧，只有首帧对齐才能保证"切过去不跳"。
$PY tools/pack_seq.py \
    --src third_party/resources/fish_seq \
    --out third_party/fish/fish_elf.spf \
    --height 156 --ref-action idle --src-fps 24 --fps 24 --idle-frames 48 \
    --shrink-default 14 \
    --cap fn_photo=160x206 \
    --align first --fit follow \
    --verify \
    --order "$ELF_ORDER"

echo
echo "== 3/3 对齐校验 =="
$PY tools/check_spf.py third_party/fish/fish_elf.spf

echo
echo "== 拷到 SD 卡（/sprite/ 下）=="
echo "  fish_elf.spf       精灵（待机常驻）"
