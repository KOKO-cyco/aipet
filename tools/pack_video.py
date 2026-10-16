#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把一段视频切成「JPEG 序列 + WAV」两个文件，供 heikesong 设备播放。

    python3 pack_video.py -i 测试视频.mp4 -t 60          # 取前 60 秒
    python3 pack_video.py -i in.mp4 -t 60 --name movie    # 指定输出名

产出两个文件（放到 SD 卡的 /sdcard/movie/ 下即可，名字必须成对）：

    <name>.anim   画面：MVJ1 容器，里面是 baseline JPEG 帧
    <name>.wav    声音：16kHz / 单声道 / 16bit PCM（设备侧零解码、零重采样）

=============================================================================
一、画面为什么是"躺着"的（重要，改素材前先看这段）
=============================================================================
面板是 320x480 的**竖屏**，而电影是 16:9 横构图。竖着放只能用到 320x180 那么
点地方，所以设备是**横过来拿**着看的（和手机看片一样）：这一步旋转在 PC 上做
掉，写进文件的每一帧都已经转好 90°，设备只是 1:1 往面板上贴，运行时**没有任何
旋转开销**。

定义"观看坐标系" (u,v)：拿设备横过来看时，u 向右、v 向下，尺寸 480x320。
面板坐标系 (x,y) 是设备正着拿时的 320x480。两者关系（把设备逆时针转 90°）：

    u = y            v = 319 - x
    x = 319 - v      y = u

于是：观看坐标系里 u∈[U0, U0+PW)、v∈[V0, V0+PH) 这块画面，落到面板上就是

    帧尺寸  PH 宽 x PW 高
    位置    x0 = 320 - V0 - PH ,  y0 = U0

而帧内容 S(x,y) = P(u=y, v=319-x)，也就是把观看画面 P 顺时针旋转 90° —— 正好
是 ffmpeg 的 transpose=1。容器头里 flags bit0=1 就是告诉设备"这是转过的素材"。

画面摆位：观看坐标系里画面上方留 TOP_PAD=8，下方 CTRL_H=84 给时间 / 进度条 /
按键，左右各 SIDE_PAD=48（画面水平居中）。改 --pic 时记住预算：

    PW（视窗宽 = 旋转后的帧高） <= VIEW_W - 2*SIDE_PAD   = 480 - 96  = 384
    PH（视窗高 = 旋转后的帧宽） <= VIEW_H - CTRL_H - TOP_PAD = 320-84-8 = 228
                                   再取 16 的倍数 ⇒ 最大 224

超出预算只警告不拒绝（设备侧同理），但控件区会被挤掉。

**片源比例 != 视窗比例时怎么摆（三种模式，默认 fit）**：

    fit（默认）  按片源比例缩到最大，四周用 pad 补黑边填满视窗。**不会变形**；
                 黑边在 JPEG 里几乎不占体积；fw/fh 仍是 PW/PH ⇒ 设备布局一动不动。
                 2.35:1 电影塞进 1.71:1 视窗、画面只有 384x160，上下各 32px 黑边。
    --fill       裁剪填满：先放大到铺满视窗，再把溢出部分居中裁掉。**带"烧进画面的
                 硬字幕"的片子首选** —— 384x224 比 384x160 大 40%，字幕同比变大；
                 代价是左右各裁掉一部分（2.35:1 裁 ~13.5%/边）。
    --stretch    强制拉伸到 PWxPH（旧行为，**会把人压扁**）。

**整部电影（帧数上万）的两个前提**：
1. 脚本必须走流式 —— 帧先落 `<name>.anim.frames` 临时件，最后再拼容器（偏移表
   紧跟在头后面、长度依赖 n，只能等全部编完）。**峰值磁盘 ≈ 2x 成品体积**。
2. 设备侧上限：`n > 200000` 或单帧 `> MV_MAX_FRAME(256KB)` 直接拒载；偏移表
   4n 字节走 `tal_psram_malloc` 一次性分配（90 分钟 @24fps ≈ 13 万帧 = 507KB，够）。
   实测 30 分钟片（--fill 384x224 @24fps）平均 18KB/帧、735MB。

=============================================================================
二、JPEG 编码器：必须用 PIL，不能用 ffmpeg 的 mjpeg（踩过的大坑）
=============================================================================
设备端能播的素材是 third_party/resources/tools/anim_pack.py 用 **Pillow** 编的
（quality=88 / subsampling=1），段结构是：

    FFC0  SOF0   Y=0x21  Cb=0x11  Cr=0x11        （4:2:2）
    FFDB  DQT    表 0（亮度）
    FFDB  DQT    表 1（色度）     <- 两个独立段，天然成对

改用 ffmpeg -c:v mjpeg 编出来的是 Y=0x22 + **只有一张量化表**，设备上第一帧就
"硬解报告失败"（app_movie.c 的 bk_jpeg_dec_hw_start 返回非 0），整部片播不出来。
所以本脚本的链路是：

    ffmpeg  只负责解码 / 缩放 / 旋转，输出 raw rgb24
    Pillow  逐帧 save('JPEG', quality=88, subsampling=1)

三条硬性约束（违反的后果都实测过）：
1) 必须 baseline（SOF0）。Pillow 默认就是，progressive=False 显式关掉。
2) 采样因子：亮度 Y 只能 0x11/0x22/0x21，色度 Cb、Cr **必须都是 0x11**。
   Pillow subsampling: 0=4:4:4(Y=0x11) 1=4:2:2(Y=0x21) 2=4:2:0(Y=0x22)。
   **默认 1**，跟现有能播的 anim 一致。
   注意 ffmpeg 写不出合法的 4:2:2（yuvj422p 实际是 Y=0x22/C=0x12，会被拒）。
3) **量化表必须成对**（表 0 和表 1 都要在），否则整机 MemFault 重启。
   jpeg_dec_hal.c:906 无条件读 qttbl[1] 写寄存器，而那张表只在文件里真出现过
   才分配 —— 缺表就对 NULL 取址、地址 0 数据访问违例直接重启。
   Pillow 天然写两张，所以**这里不需要任何补丁**。设备侧 app_movie.c 还会扫
   一遍先拒，双保险：最坏是丢一部片，不是重启。
4) **帧宽必须是 16 的倍数**：4:2:2 的 MCU 是 16x8，宽度不是整数个 MCU 就解不出。
   注意"帧宽"是**旋转之后**的宽，也就是 --pic 里的 **PH**（不是 PW）。
   脚本按 ALIGN_W=32 对齐 PH（比硬约束更保守，无成本）；PW（= 帧高）按
   ALIGN_H=16 对齐就够。

   [2026-09-24 更正] 曾把"密密麻麻的彩色小矩形"归因于"帧宽必须是 32 的倍数 /
   YUYV 行距按 64 字节补齐"，**该结论已被设备实测证伪**：192 与 208 宽的四种
   组合（192x336、192x384、208x336、208x368）**全部正常播放**。所以 32 不是
   硬约束；保留它只是为了把默认路径停在已验证过的尺寸上（PH 落在 192）。

=============================================================================
三、容器格式 MVJ1（小端）
=============================================================================
    off 0   char[4] "MVJ1"
    off 4   u16     fw          帧宽（面板坐标系里的存放尺寸）
    off 6   u16     fh          帧高
    off 8   u16     fps
    off 10  u16     flags       bit0 = 帧已预旋转 90°（横过来看）
    off 12  u32     n           帧数
    off 16  u32     data_start  帧数据起点 = 32 + 4n
    off 20  u32     max_frame   最大单帧字节数（设备按它开解码缓冲）
    off 24  u32     dur_ms      音轨时长（毫秒；0 = 按 n/fps 算）
    off 28  u32     rsv
    off 32  u32[n]  第 i 帧的**结束**偏移（相对 data_start）
                    第 i 帧 = [ data_start + (i ? off[i-1] : 0) , data_start + off[i] )
                    最后一帧的结束偏移 = 帧数据总长

=============================================================================
四、音画同步：起点对齐（脚本自动做，不用管）
=============================================================================
设备上「音轨是主时钟」，画面跟着音轨走，所以只要**画面内容和声音内容在同一个
时刻起跑**，全片就是同步的。但两条 ffmpeg 命令各自 `-ss S` 时，起跑线并不一样：

    ffmpeg -ss S -i src ...        # 音频：能精确落在 S（样本级）
    ffmpeg -ss S -i src -vf fps=N  # 视频：只能落在「第一个 pts >= S 的帧」

源的视频流起点经常比音频流晚 —— 实测《战狼1.mp4》视频从 0.2067s 才有帧、音频
从 0.0033s 就有。从 0 开始切时，`fps` 滤镜为了凑恒定帧率，会**把第一帧复制好几
份**去填满 0 ~ 0.2067s 这段空档（帧哈希完全相同）。后果：那一帧的内容（本该出现
在 0.2067s）被搬到了 0s 显示，**画面整体提前 203ms，听起来就是"声音比画面慢"**，
而且怎么调固件都补不平 —— 因为错的是素材本身。

所以脚本转码前先测一次两侧的真实起点（probe_av_start），然后**统一用视频的
起点 T0 作为 -ss**，两条流从同一个绝对时刻起跑：

    -ss T0  视频给 T0（无空档，不再填充）；音频也给 T0（丢掉开头那段没画面的声音）

代价是开头那 200ms 有声无画的片段会被丢掉 —— 本来也没内容，丢掉更干净。
源本身音画对齐的片子（起点差 < 1ms）T0 就等于 S，行为不变。

转码结束会打印一行体检报告（视频秒数 / 音频秒数 / 残差毫秒），残差超过 1.5 帧
会告警。**看到那行是 ~0ms 就说明这条素材起跑线是齐的。**

=============================================================================
五、怎么放到设备上
=============================================================================
    cp movie.anim movie.wav  <SD 卡>/sdcard/movie/
设备开机进「电影」页就能看到它。缺 .wav 只是没声音，画面照播。
"""

import argparse
import io
import os
import re
import shutil
import struct
import subprocess
import sys

from PIL import Image

MAGIC = b'MVJ1'
HEADER_LEN = 32

# 观看坐标系（设备横过来拿）：480x320
VIEW_W = 480
VIEW_H = 320

# 控件区在观看坐标系里占的垂直空间：画面下方到屏幕底边
#   = 48（进度条/时间/按键的起画偏移）+ 36（MV_BTN_H，已缩过一轮）+ 余量
CTRL_H = 84
# 画面上方留白
TOP_PAD = 8
# 左右留白（每侧）—— 画面宽上限 = VIEW_W - 2*SIDE_PAD = 384
SIDE_PAD = 48

# 面板（竖屏 320x480）—— 只用来算落点，和固件里的 UI_PANEL_W/H 必须一致
PANEL_W = 320
PANEL_H = 480

# 设备采样率（app_speaker 实测 16000），音轨必须对齐
AUDIO_RATE = 16000

# 对齐（两个方向的要求不一样，别混）：
#   「帧宽」= --pic 的 PH —— 4:2:2 的 MCU 是 16x8，**硬约束是 16 的倍数**；
#   这里取 32 只是更保守（让默认路径落在已验证过的 192 上）。
#   注意曾把彩块归因于"32 倍数"，已被实测证伪（208 宽播放正常），见第二节第 4 条。
#   「帧高」= --pic 的 PW —— 同样按 MCU 的 8/16 倍数，取 16 就够。
ALIGN_W = 32                # 帧宽（PH）；硬约束 16
ALIGN_H = 16                # 帧高（PW）

# 与 anim_pack.py 完全一致的编码参数（设备实测能播）
JPEG_QUALITY = 88
JPEG_SUBSAMPLING = 1        # 0=4:4:4  1=4:2:2  2=4:2:0


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def align_down(v, a=ALIGN_H):
    return max(a, (int(v) // a) * a)


def probe_video(path):
    r = run(['ffprobe', '-v', 'error',
             '-select_streams', 'v:0',
             '-show_entries', 'stream=width,height,r_frame_rate',
             '-show_entries', 'format=duration',
             '-of', 'default=noprint_wrappers=1', path])
    if r.returncode != 0:
        sys.exit('ffprobe 失败: %s' % r.stderr.decode('utf-8', 'replace'))
    w = h = 0
    fps = 30.0
    dur = 0.0
    for line in r.stdout.decode().splitlines():
        k, _, v = line.partition('=')
        if k == 'width':
            w = int(v)
        elif k == 'height':
            h = int(v)
        elif k == 'r_frame_rate' and '/' in v:
            a, _, b = v.partition('/')
            fps = float(a) / float(b) if float(b) else 30.0
        elif k == 'duration':
            dur = float(v)
    return w, h, fps, dur


def has_audio(path):
    r = run(['ffprobe', '-v', 'error', '-select_streams', 'a:0',
             '-show_entries', 'stream=codec_type', '-of', 'csv=p=0', path])
    return b'audio' in r.stdout


def probe_av_start(path, start, want_audio=True):
    """测「-ss start 之后，音视频各自第一个真实内容」在**源时间轴**上的绝对时刻。

    返回 (视频起始秒 或 None, 音频起始秒 或 None)。

    两处 `-copyts` 是必须的：不加的话 ffmpeg 会把输出时间戳归零（每条命令各自
    从 0 起算），正好把要测的那个偏移抹掉 —— 当年就是栽在这里，以为"两条命令
    都 -ss 同一个值，自然就对齐了"。加了之后 showinfo / framecrc 报的就是源
    时间轴上的真实位置。

    视频只解 1~5 帧，长片也只是一次关键帧 seek，开销可忽略。
    """
    t_v = None
    r = run(['ffmpeg', '-v', 'info', '-copyts', '-ss', '%.6f' % start,
             '-t', '0.2', '-i', path, '-an', '-vf', 'showinfo',
             '-f', 'null', '-'])
    m = re.search(r'pts_time:([0-9.]+)', r.stderr.decode('utf-8', 'replace'))
    if m:
        t_v = float(m.group(1))

    t_a = None
    if want_audio:
        # framecrc 行格式：stream, dts, pts, duration, size, hash —— 时间单位就是
        # stream 的 time_base，对 pcm_s16le 输出等于 1/采样率，所以 pts/16000 = 秒。
        r = run(['ffmpeg', '-v', 'info', '-copyts', '-ss', '%.6f' % start,
                 '-t', '0.2', '-i', path, '-vn', '-ac', '1',
                 '-ar', str(AUDIO_RATE), '-f', 'framecrc', '-'])
        for line in r.stdout.decode('utf-8', 'replace').splitlines():
            if line.startswith('0,'):
                t_a = int(line.split(',')[2]) / float(AUDIO_RATE)
                break
    return t_v, t_a


def scan_markers(data):
    """扫到 SOS 为止，返回 (DQT 表号集合, SOF 分量[(id, 采样因子, 表号)], 是否 SOF0)"""
    dqts = set()
    comps = []
    sof0 = True
    i = 2
    while i + 4 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        m = data[i + 1]
        if m == 0xFF:
            i += 1
            continue
        if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        if m == 0xDA:
            break
        seg = (data[i + 2] << 8) | data[i + 3]
        if seg < 2:
            break
        if m == 0xDB:
            k = i + 4
            end = i + 2 + seg
            while k + 1 <= end:
                pq = data[k] >> 4
                tq = data[k] & 0x0F
                dqts.add(tq)
                k += 1 + (64 if pq == 0 else 128)
        elif m in (0xC0, 0xC1, 0xC2):
            sof0 = (m == 0xC0)
            ncomp = data[i + 9]
            for j in range(ncomp):
                base = i + 10 + 3 * j
                comps.append((data[base], data[base + 1], data[base + 2]))
        i += 2 + seg
    return dqts, comps, sof0


def main():
    ap = argparse.ArgumentParser(description='把视频打包成设备可播的 .anim + .wav')
    ap.add_argument('-i', '--input', default='测试视频.mp4')
    ap.add_argument('-o', '--out', default='_movie_out', help='输出目录')
    ap.add_argument('-n', '--name', default='movie', help='输出文件名（不含后缀）')
    ap.add_argument('-s', '--start', type=float, default=0.0, help='起始秒')
    ap.add_argument('-t', '--dur', type=float, default=60.0, help='截取时长（秒）')
    ap.add_argument('--fps', type=int, default=30)
    ap.add_argument('--pic', default='', help='画面尺寸 PWxPH（观看坐标系），默认按片源比例算')
    ap.add_argument('--no-rotate', action='store_true',
                    help='不预旋转（设备竖着拿，画面在上、控件在下）')
    ap.add_argument('-q', '--quality', type=int, default=JPEG_QUALITY,
                    help='JPEG 质量 1-95（Pillow 语义，默认 88，和 anim_pack 一致）')
    ap.add_argument('--subs', type=int, default=JPEG_SUBSAMPLING,
                    help='色度抽样 0=4:4:4 1=4:2:2 2=4:2:0（默认 1）')
    ap.add_argument('--no-align', action='store_true',
                    help='不做 MCU 对齐（调试用：验证"非 16 倍数宽"会不会被硬解拒）')
    ap.add_argument('--stretch', action='store_true',
                    help='强制拉伸填满 PWxPH。默认保持片源比例，画面在视窗里居中、'
                         '四周留边（黑边由固件按背景色补，不占素材体积）')
    ap.add_argument('--fill', action='store_true',
                    help='裁剪填满视窗（不补黑边）。2.35:1 电影塞进 1.71:1 视窗时，'
                         '画面和烧进画面的硬字幕都会大一圈，代价是左右各裁掉一部分')
    ap.add_argument('--no-audio', action='store_true')
    args = ap.parse_args()

    if not os.path.isfile(args.input):
        sys.exit('找不到片源: %s' % args.input)
    for tool in ('ffmpeg', 'ffprobe'):
        if shutil.which(tool) is None:
            sys.exit('没找到 %s（本机没有的话用 pack_video_wsl.sh 跑）' % tool)

    sw, sh, sfps, sdur = probe_video(args.input)
    if args.start >= sdur > 0:
        sys.exit('起始 %ss 超过片长 %.1fs' % (args.start, sdur))
    print('片源 %s  %ux%u @%.3ffps  时长 %.1fs' % (args.input, sw, sh, sfps, sdur))

    # ---- 音画起点对齐（原理见文件头第四节）---------------------------------
    # 视频受帧量化限制，只能从"第一个 pts >= start 的帧"起跑；音频能精确落在
    # start。两条流起跑线不同时，fps 滤镜会用首帧复制填掉那段空档 ⇒ 画面整体
    # 提前 ⇒ 听感"声音慢"。统一按**画面**的起点切，两边就从同一时刻起跑。
    aud = (not args.no_audio) and has_audio(args.input)
    t_v, t_a = probe_av_start(args.input, args.start, aud)
    start = args.start
    if t_v is not None:
        # 往回让 1ms：-ss 与帧的 pts 是浮点比较，T0 恰好等于那一帧的 pts 时会
        # 被判成"还没到"而**整帧丢掉**（实测 -ss 0.206667 之后首帧变成 0.248375，
        # 又白白落后一帧 42ms）。1ms 远小于帧间隔（24fps 是 41.7ms、30fps 33.3ms），
        # 不会退到上一帧去，最坏就是音频早 1ms —— 远在可感知范围之外。
        start = t_v - 0.001
    if (t_v is not None) and (t_a is not None):
        gap = (t_a - t_v) * 1000.0
        if abs(gap) >= 1.0:
            print('源音画起点相差 %+.0f ms（音频%s）-> 统一按画面起点 %.6fs 切，'
                  '丢掉开头 %.0fms 的%s'
                  % (gap, '晚' if gap > 0 else '早', start,
                     abs(t_a - start) * 1000.0,
                     '有声无画' if gap < 0 else '有画无声'))
        else:
            print('源音画起点相差 %+.1f ms，本来就是对齐的' % gap)
    else:
        print('注意: 测不到音画起点（源%s），按原起点 %.3fs 切'
              % ('无音轨' if not aud else '探测失败', start))

    dur = args.dur
    if sdur > 0:
        dur = min(dur, sdur - start)
    print('截取 %.3fs .. %.3fs（共 %.3fs）' % (start, start + dur, dur))

    # ---- 画面尺寸（观看坐标系） --------------------------------------------
    # 旋转后 JPEG 的宽 = PH、高 = PW：所以"帧宽"的对齐约束落在 PH 上
    # （硬约束 16，脚本按 ALIGN_W=32 更保守）。
    if args.pic:
        pw, _, ph = args.pic.lower().partition('x')
        PW, PH = int(pw), int(ph)
        if not args.no_align:
            if PW % ALIGN_H or PH % ALIGN_W:
                print('注意: %ux%u 未对齐（帧高 PW 需 %u 倍数、帧宽 PH 需 %u 倍数），'
                      '向下对齐到 %ux%u'
                      % (PW, PH, ALIGN_H, ALIGN_W,
                         align_down(PW, ALIGN_H), align_down(PH, ALIGN_W)))
            PW, PH = align_down(PW, ALIGN_H), align_down(PH, ALIGN_W)
    else:
        PH = VIEW_H - CTRL_H - TOP_PAD                   # 预算 216
        PH = PH if args.no_align else align_down(PH, ALIGN_W)    # -> 192
        PW = min(VIEW_W - 2 * SIDE_PAD, int(round(PH * (sw / max(sh, 1)))))
        PW = PW if args.no_align else align_down(PW, ALIGN_H)    # -> 336
    if PH > VIEW_H - CTRL_H - TOP_PAD or PW > VIEW_W - 2 * SIDE_PAD:
        print('警告: 画面 %ux%u 超出预算，控件区会被挤掉（PH<=%d, PW<=%d）'
              % (PW, PH, VIEW_H - CTRL_H - TOP_PAD, VIEW_W - 2 * SIDE_PAD))

    # ---- 片源比例 != 视窗比例 时怎么办 -------------------------------------
    # `scale=PW:PH` 是**强制拉伸**：2.35:1 的电影塞进 1.71:1 的视窗会把人物压扁。
    # 默认改成 fit —— 按片源比例缩到最大（AW x AH），再用 pad 补黑边填满 PWxPH。
    # 黑边在 JPEG 里几乎不占体积，但 fw/fh 保持 PW/PH 不变 ⇒ 固件布局一动不动。
    # 想回到旧的拉伸行为加 --stretch。
    src_ar = sw / float(max(sh, 1))
    if args.fill:
        AW, AH = PW, PH                        # 裁剪填满：输出就是整个视窗
    elif args.stretch:
        AW, AH = PW, PH
    elif PW / float(max(PH, 1)) > src_ar:
        AH = PH                                # 片源比视窗更高 → 以高为准，左右留边
        AW = int(round(AH * src_ar))
    else:
        AW = PW                                # 片源比视窗更宽（2.35:1 电影）→ 上下留边
        AH = int(round(AW / src_ar))
    if not args.no_align and not (args.stretch or args.fill):
        AW = align_down(AW, ALIGN_H)           # AW 是视窗宽 -> 旋转后帧高
        AH = align_down(AH, ALIGN_W)           # AH 是视窗高 -> 旋转后帧宽
    if AW < ALIGN_H or AH < ALIGN_W:
        sys.exit('画面算出来太小（%ux%u），检查 --pic 与片源比例 %.3f' % (AW, AH, src_ar))
    if args.fill:
        print('按视窗裁剪填满 %ux%u：源比例 %.3f -> 视窗 %.3f，%s各裁掉一部分'
              '（画面更大、硬字幕更清楚）'
              % (PW, PH, src_ar, PW / float(max(PH, 1)),
                 '左右' if src_ar > PW / float(max(PH, 1)) else '上下'))
    elif not args.stretch and (AW != PW or AH != PH):
        print('片源比例 %.3f 与视窗 %ux%u（%.3f）不一致：画面按比例缩到 %ux%u，'
              '四周补黑边（不变形）' % (src_ar, PW, PH, PW / float(max(PH, 1)), AW, AH))

    U0 = (VIEW_W - AW) // 2
    V0 = TOP_PAD + (PH - AH) // 2
    if args.no_rotate:
        fw, fh, flags = PW, PH, 0
        px, py = U0, V0
    else:
        fw, fh, flags = PH, PW, 1
        px, py = PANEL_W - V0 - PH, U0
    print('观看画面 %ux%u（u0=%d v0=%d）  面板落点 %ux%u @(%d,%d)  %s'
          % (PW, PH, U0, V0, fw, fh, px, py,
             '横过来看' if flags else '竖持'))
    print('JPEG %ux%u  quality=%d subsampling=%d（%s）  帧宽%%16=%d 帧高%%16=%d'
          % (fw, fh, args.quality, args.subs,
             {0: '4:4:4', 1: '4:2:2', 2: '4:2:0'}.get(args.subs, '?'),
             fw % 16, fh % 16))
    if fw % 16:
        print('警告: 帧宽 %u 不是 16 的倍数（4:2:2 的 MCU 宽 16），设备上解不出来！'
              % fw)

    os.makedirs(args.out, exist_ok=True)

    # ---- 1) ffmpeg 解码 + 缩放 + 旋转 -> raw rgb24；Pillow 逐帧编 JPEG ------
    if args.fill:
        # 先放大到"填满"，再把溢出部分居中裁掉 —— crop 出来正好 PW x PH。
        # 注意 crop 的 x/y 写成 iw/2-N 这种加减式，别用括号（某些调用链会吃掉引号）。
        if src_ar > PW / float(max(PH, 1)):
            vf = ('fps=%d,scale=-2:%d:flags=lanczos,crop=%d:%d:iw/2-%d:0'
                  % (args.fps, PH, PW, PH, PW // 2))
        else:
            vf = ('fps=%d,scale=%d:-2:flags=lanczos,crop=%d:%d:0:ih/2-%d'
                  % (args.fps, PW, PW, PH, PH // 2))
    elif args.stretch:
        vf = 'fps=%d,scale=%d:%d:flags=lanczos' % (args.fps, PW, PH)
    else:
        vf = ('fps=%d,scale=%d:%d:flags=lanczos,'
              'pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black'
              % (args.fps, AW, AH, PW, PH))
    if not args.no_rotate:
        vf += ',transpose=1'                     # 顺时针 90°，输出 PH x PW
    dec = ['ffmpeg', '-v', 'error', '-ss', '%.6f' % start, '-t', '%.6f' % dur,
           '-i', args.input, '-an', '-vf', vf,
           '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-']

    print('转码中（约 %d 帧 @%dfps）...' % (int(round(dur * args.fps)), args.fps))
    p = subprocess.Popen(dec, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    fsz = fw * fh * 3

    # 边解码边落盘：60 秒（1800 帧）全攒内存无所谓，但 90 分钟 ~13 万帧会吃
    # 掉几个 GB。帧先写临时文件，内存里只留偏移表（4 字节/帧）；容器在最后拼。
    anim_path = os.path.join(args.out, args.name + '.anim')
    raw_path  = anim_path + '.frames'
    max_frame   = 0
    offs        = []
    first_frame = None
    total = 0
    n = 0
    with open(raw_path, 'wb') as f:
        while True:
            buf = p.stdout.read(fsz)
            if len(buf) < fsz:
                break
            im = Image.frombytes('RGB', (fw, fh), buf)
            o = io.BytesIO()
            im.save(o, 'JPEG', quality=args.quality, subsampling=args.subs,
                    optimize=False, progressive=False)
            b = o.getvalue()
            if first_frame is None:
                first_frame = b
            f.write(b)
            total += len(b)
            offs.append(total)
            max_frame = max(max_frame, len(b))
            n += 1
            if (n % 1000) == 0:
                print('  ... %d 帧（%.0fMB）' % (n, f.tell() / 1048576.0), flush=True)
    err = p.stderr.read()
    p.stdout.close()
    if p.wait() != 0:
        try:
            os.remove(raw_path)
        except OSError:
            pass
        sys.exit('解码失败: %s' % err.decode('utf-8', 'replace')[:2000])

    if not n:
        try:
            os.remove(raw_path)
        except OSError:
            pass
        sys.exit('一帧都没编出来（检查 ffmpeg 输出）')
    print('  %d 帧   %.1fMB' % (n, total / 1048576.0))

    # ---- 2) 首帧自检（把设备最挑的三件事打出来） ---------------------------
    dqts, comps, sof0 = scan_markers(first_frame)
    subs_name = {0x11: '4:4:4', 0x21: '4:2:2', 0x22: '4:2:0'}.get(
        next((c[1] for c in comps if c[0] == 1), 0), '?')
    print('  首帧: SOF0=%s  采样=%s %s  量化表=%s'
          % (sof0, subs_name, comps, sorted(dqts)))
    if not sof0:
        print('  警告: 不是 baseline，硬解会拒')
    if {0, 1} - dqts:
        print('  警告: 量化表不全 %s，硬解会 MemFault' % sorted(dqts))
    if any(c[1] != 0x11 for c in comps if c[0] != 1):
        print('  警告: 色度采样因子不是 0x11，硬解会拒')

    # ---- 3) 拼容器：头 + 偏移表 + 帧数据（帧数据从临时文件拷入）------------
    # 偏移表必须在头正后面（固件校验 data_start == 32 + 4n），而 n 要全部编完
    # 才知道，所以帧数据只能先落在临时文件里、最后拼一次。
    data_start = HEADER_LEN + 4 * n
    with open(anim_path, 'wb') as f:
        f.write(MAGIC
                + struct.pack('<HHHH', fw, fh, args.fps, flags)
                + struct.pack('<IIII', n, data_start, max_frame, 0)
                + struct.pack('<I', 0))
        f.write(b''.join(struct.pack('<I', o) for o in offs))
        if f.tell() != data_start:
            sys.exit('内部错误：偏移表长度 %d != data_start %d' % (f.tell(), data_start))
        with open(raw_path, 'rb') as g:
            shutil.copyfileobj(g, f, 1 << 20)
    os.remove(raw_path)

    # ---- 4) 音轨：16kHz 单声道 16bit ---------------------------------------
    audio_ms = 0
    if aud:
        wav_path = os.path.join(args.out, args.name + '.wav')
        r = run(['ffmpeg', '-v', 'error', '-y',
                 '-ss', '%.6f' % start, '-t', '%.6f' % dur,
                 '-i', args.input, '-vn',
                 '-ac', '1', '-ar', str(AUDIO_RATE), '-c:a', 'pcm_s16le',
                 '-f', 'wav', wav_path])
        if r.returncode != 0 or not os.path.isfile(wav_path):
            print('警告: 音轨提取失败，只有画面')
        else:
            import wave
            with wave.open(wav_path) as w:
                audio_ms = int(w.getnframes() * 1000 / w.getframerate())
                ok = (w.getnchannels() == 1 and w.getsampwidth() == 2
                      and w.getframerate() == AUDIO_RATE)
            if not ok:
                print('警告: 音轨规格不对（要 %dHz/单声道/16bit）' % AUDIO_RATE)
            # 时长写回容器头
            with open(anim_path, 'r+b') as f:
                f.seek(24)
                f.write(struct.pack('<I', audio_ms))
    elif not args.no_audio:
        print('片源没有音轨，只有画面')

    # ---- 5) 汇总 -----------------------------------------------------------
    total_ms = audio_ms if audio_ms else int(round(n * 1000.0 / args.fps))
    anim_sz = os.path.getsize(anim_path)
    print('')
    print('== 完成 ==')
    print('  %s   %ux%u  %d 帧 @%dfps  %.1f 秒  %.1fMB（平均 %uB/帧，最大 %uB）'
          % (anim_path, fw, fh, n, args.fps, n / float(args.fps),
             anim_sz / 1048576.0, anim_sz // max(n, 1), max_frame))
    if audio_ms:
        wav_sz = os.path.getsize(os.path.join(args.out, args.name + '.wav'))
        print('  %s   %dHz 单声道 16bit  %.1f 秒  %.2fMB'
              % (os.path.join(args.out, args.name + '.wav'), AUDIO_RATE,
                 audio_ms / 1000.0, wav_sz / 1048576.0))
    print('  总时长 %.1fs' % (total_ms / 1000.0))

    # ---- 6) 音画对齐体检 ---------------------------------------------------
    # 画面长度 = 帧数 / 帧率（设备上"帧号 = 音轨位置 x fps"就是按这个走的），
    # 声音长度 = WAV 实际样本数。起点已经统一（上一节），所以这里比的是**末端**：
    # 两条 ffmpeg 各自的 -t 取整 + fps 滤镜的格点会让末尾差出 1~2 帧，3 帧以内
    # 都算正常。真有错位会远超这个数（实测源那个起点差就是 203ms = 5 帧）。
    v_ms = n * 1000.0 / args.fps
    print('')
    print('== 音画对齐 ==  两侧起点都用 %.6fs（源起点差已消除）' % start)
    if audio_ms:
        res = audio_ms - v_ms
        flag = '   <-- 超过 3 帧，回去查片源的音画起点' \
            if abs(res) > (1000.0 / args.fps) * 3.0 else '   OK'
        print('   画面 %.3fs（%d 帧 @%dfps）  声音 %.3fs  末端差 %+.0fms%s'
              % (v_ms / 1000.0, n, args.fps, audio_ms / 1000.0, res, flag))
    else:
        print('   画面 %.3fs（%d 帧 @%dfps）  无音轨'
              % (v_ms / 1000.0, n, args.fps))

    print('')
    print('拷贝到 SD 卡: 两个文件一起放进 /sdcard/movie/ （名字必须成对）')


if __name__ == '__main__':
    main()
