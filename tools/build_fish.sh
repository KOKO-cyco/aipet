#!/bin/sh
# 2D 宠物：素材 webm -> PNG 序列 -> fish_elf.spf，一步到位。
#
#     sh tools/build_fish.sh
#
# 产出（拷到 SD 卡的 /sprite/ 下）：
#   third_party/fish/fish_elf.spf   桌宠精灵（app_sprite2d.c + page_pet.c）
#
# 老产品还有一个 fish_role.spf（角色页的大角色，app_role2d.c），那个页面删了，
# 所以这一步也去掉了 —— 只剩精灵。
#
# **`--ref-action idle` 千万别省** —— 见 tools/fish_extract.py 的用法说明。
# pack_seq.py 默认基准动作 `idle_normal` 在这套素材里不存在，会静默退回
# "所有动作的并集高度"，整个文件等比缩小 1.162 倍且不报错。
#
# **`--order` 里的顺序 = 固件里的组号**，第 0 组（idle）整组常驻 PSRAM，必须排第一。
# 固件是按**名字**找组号的（SPF7 名字表），所以顺序改了不会静默错位。
#
# `--shrink-default N`：所有动作的角色高都比 `--height` 矮 N 像素。**位置和大小是
# 分开算的** —— 缩小只改"画多大"，角色以自己的站位参考点（底边中点）为锚原地变小。
#
# **不要给单个动作加 `--shrink 名字=M`。** 2026-10-16 之前这里有一行 `--shrink idle=10`，
# 结果待机比别的动作高 4px，切动作时头会跳一下 —— 用户的原话是"角色位置、大小要完全
# 一样，不然看着像是抽动"。要调大小就调 `--shrink-default`，全体一起动。
#
# 尺寸已经由 pack_seq.py 自动归一了（`--fit follow` -> `--align first`，按各动作的
# 首帧角色高算缩放），所以这里不需要再逐动作微调。
#
# **`--order` 里一动就重看锚点框**：锚点框 = 所有动作内容框的并集，加一组宽的
# 动作会把整套撑小（角色能画多大由它决定）。脚本末尾会打出来。
#
# 用哪个 python：需要 numpy + PIL。本机 `py -3` 有（项目 venv 里没有），
# 要换就 `PY=python3 sh tools/build_fish.sh`。
set -e

cd "$(dirname "$0")/.."
PY="${PY:-py -3}"

# AI 表情（happy/think/sad）也在精灵里 —— 对话时宠物要按情绪演开心/思考/伤心
# （见 page_pet.c 的情绪表和 app_avatar_tools.c 的动作白名单）。
# 它们在 pack_seq.py 里的 role 是 ROLE_AI（不进内置随机池），但桌宠页用的是
# 自己的动作状态机 + app_sprite2d_render_clip()，所以 role 字段不影响我们。
#
# **rnd_whale（蓝鲸现世）留着** —— 2026-10-15 试删过（它是锚点框宽度的元凶：
# 帧宽 199px、偏移只到 10，把并集左右两边都顶开；删掉锚点框从 218x195 降到
# 171x195）。但**删了角色并不会变大**：去掉宽度之后瓶颈变成高度（SP2D_MAX_H=208），
# 角色高只能从 156 提到 ~166（+6%），代价是少一个动作 —— 不划算，所以放回来了。
# 真想再窄一截的时候，删它仍然是最划算的一刀（那时要同时抬 SP2D_MAX_H）。
ELF_ORDER="idle,snap,snap_l,click_angry,click_wave,click_shy,click_happy,drag_up,drag_hold,drag_down,talk,happy,think,sad,rnd_lookaround,rnd_float,rnd_scare,rnd_breakfast,rnd_lunch,rnd_yawn,rnd_whale,rnd_photo"

echo "== 1/3 webm -> PNG 序列 =="
$PY tools/fish_extract.py --out third_party/resources/fish_seq

echo
echo "== 2/3 贴边那两张（snap / snap_l） =="
# **必须在打包前跑**：这两张不在 fish_extract.py 的 CLIPS 表里（不是从 webm 来的），
# 少了它们 --order 里的 snap/snap_l 会被静默跳过 —— 固件里没有 SNAP 组，贴边时
# 角色就不换成"半藏"那张图（2026-10-14 就是这么坏的）。
$PY tools/make_snap_seq.py

echo
echo "== 3/3 精灵（fish_elf.spf）：待机常驻 + 跨动作位置/尺寸归一 =="
# `--align first --fit follow`：位置和尺寸都按**首帧**归一。
# 用户 2026-10-16 选的"首帧" —— 切动作时看到的就是新动作的首帧，所以只有首帧
# 对齐才能保证"切过去不跳"。他自己的 `角色一致性检验.jpg` 也是按首帧量的。
# 改成 `--align median` 会按整段常态对齐，循环动作更稳，但会被"缩着/张开"的
# 姿势带偏（伤心那段的逐帧中位高只有 128）。
$PY tools/pack_seq.py \
    --src third_party/resources/fish_seq \
    --out third_party/fish/fish_elf.spf \
    --height 156 --ref-action idle --src-fps 24 --fps 24 --idle-frames 48 \
    --shrink-default 14 \
    --align first --fit follow \
    --verify \
    --order "$ELF_ORDER"

echo
echo "== 背景 =="
# bg_afternoon.rgb 是原来 packed_anim/bg.rgb 的原样拷贝（逐字节一致）；
# bg_morning.rgb 是孤儿文件，仓库里没有生成它的脚本、源图也不在（见 make_bg.py）。
# 换背景才需要 make_bg.py。
ls -l third_party/fish/bg_*.rgb

echo
echo "== 拷到 SD 卡（/sprite/ 下） =="
echo "  fish_elf.spf       精灵（待机常驻）"
echo "  bg_morning.rgb     上午背景（12:00 前用）"
echo "  bg_afternoon.rgb   下午背景"
