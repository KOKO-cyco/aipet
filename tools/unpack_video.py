#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
unpack_video.py —— 把设备素材（MVJ1 画面 + WAV 音轨）反向重组回一个能播的 MP4。

为什么要这个
------------
设备上"声音慢"，到底是**素材**带的还是**固件**算错的？把素材单独还原成 MP4，
扔进任意播放器，用眼睛和耳朵就能把这两种情况分开：

    python3 unpack_video.py -i 战狼1-1.anim -o /tmp/战狼1-1.mp4

MP4 的时间轴是**按素材内容重建**的 —— 第 i 帧出现在 t = i/fps，音轨第 0 个
样本出现在 t = 0。所以它忠实反映素材本身的对齐状态：

  * PC 上听着同步  -> 素材没问题，偏差在设备侧（去对固件日志 `[movie] drift=`）
  * PC 上也慢      -> 素材/打包的问题（回去看 pack_video.py 打印的对齐报告）

想知道"具体慢多少"，用 --audio-shift 把音轨平移后重出，**听到对齐的那个数
就是真实偏差量**：

    python3 unpack_video.py -i 战狼1-1.anim -o t.mp4 --audio-shift -800

（-800 = 音轨整体提前 800ms。如果这样听就齐了，说明原来是"声音慢 800ms"。）

只取中段（段开头因为"打包时用首帧填空档"，往往是最特殊的一段，中段才代表常态）：

    python3 unpack_video.py -i 战狼1-2.anim -o t.mp4 --start-sec 60 --limit-sec 60

（从素材第 60s 起取 60s。音轨按同一时刻做输入级 seek —— 两边必须同时起跑，
否则测出来的只是"你切歪了"，跟同步无关。）

顺带做的三项体检
----------------
1. 容器字段自检（magic / data_start / 帧数上限），和固件加载时用的判据一致；
2. 两边**名义时长**的差（帧数/fps 对 音轨样本数/采样率）—— 就是设备上 pos
   能走到的上限与画面末尾的错位量；
3. 开头连续重复帧的个数 —— **这是"打包时用首帧填空档"的签名**，那会让整幅
   画面提前（源首帧 pts 比音频起点晚多少，就提前多少）。

运行环境：WSL（需要 ffmpeg；不要用 Windows 的 python，那边多半没有 ffmpeg）
    wsl -e bash -c "python3 <此文件> -i /mnt/c/.../战狼1-1.anim -o /tmp/a.mp4"
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import wave

HDR_LEN = 32


def die(msg):
    sys.stderr.write('错误：%s\n' % msg)
    sys.exit(1)


def parse_anim(path):
    """解析 MVJ1 头 + 末偏移表。格式见 tools/pack_video.py 的容器说明。"""
    with open(path, 'rb') as f:
        hdr = f.read(HDR_LEN)
        if len(hdr) < HDR_LEN:
            die('容器头不完整（文件只有 %d 字节？）' % len(hdr))
        if hdr[:4] != b'MVJ1':
            die('不是 MVJ1 容器（magic=%r）' % hdr[:4])

        fw, fh, fps, flags, n, ds, max_frame, dur_ms = \
            struct.unpack('<HHHHIIII', hdr[4:28])

        if fps == 0 or n == 0:
            die('容器头不合理：fps=%u n=%u' % (fps, n))
        if ds != HDR_LEN + 4 * n:
            die('data_start=%u 与帧数 %u 对不上（应为 %u）'
                % (ds, n, HDR_LEN + 4 * n))

        f.seek(HDR_LEN)
        blob = f.read(4 * n)
        if len(blob) < 4 * n:
            die('偏移表读不全（要 %u 字节，只有 %u）' % (4 * n, len(blob)))
        offs = struct.unpack('<%dI' % n, blob)

    size = os.path.getsize(path)
    if size != ds + offs[-1]:
        sys.stderr.write('提示：文件实际 %u 字节，容器声称 %u 字节（多出 %d）\n'
                         % (size, ds + offs[-1], size - ds - offs[-1]))

    return {'fw': fw, 'fh': fh, 'fps': fps, 'flags': flags, 'n': n,
            'ds': ds, 'max_frame': max_frame, 'dur_ms': dur_ms, 'offs': offs}


def wav_info(path):
    with wave.open(path, 'rb') as w:
        return {'rate': w.getframerate(), 'ch': w.getnchannels(),
                'bits': w.getsampwidth() * 8, 'frames': w.getnframes(),
                'sec': w.getnframes() / float(w.getframerate())}


def head_dup_frames(path, info, probe=12):
    """开头 probe 帧里，有多少帧和它前一帧**字节完全相同**（重复帧签名）。"""
    md5s = []
    with open(path, 'rb') as f:
        f.seek(info['ds'])
        prev = 0
        for end in info['offs'][:probe]:
            f.seek(info['ds'] + prev)
            md5s.append(hashlib.md5(f.read(end - prev)).hexdigest()[:8])
            prev = end
    dup = sum(1 for i in range(1, len(md5s)) if md5s[i] == md5s[i - 1])
    return dup, md5s


def encode(anim, info, wav_path, out, upright, shift_ms, vcopy,
           start_sec=0.0, limit_sec=0):
    """把帧序列通过管道喂给 ffmpeg，和音轨一起封装。返回 ffmpeg 退出码。

    start_sec 是**素材内部**的相对时刻：画面从第 round(start_sec*fps) 帧开始喂，
    音轨在输入侧 seek 同一时刻。两边同时起跑，切中段才不会切出个假的错位。
    """
    fps = info['fps']
    n   = info['n']

    i0 = max(0, min(n - 1, int(round(start_sec * fps)))) if start_sec > 0 else 0
    n_use = n - i0
    if limit_sec > 0:
        n_use = min(n_use, max(1, int(round(limit_sec * fps))))
    i1 = i0 + n_use

    cmd = ['ffmpeg', '-v', 'error', '-y',
           '-f', 'image2pipe', '-framerate', str(fps), '-c:v', 'mjpeg', '-i', '-']
    if wav_path:
        if i0 > 0:
            # PCM 的输入级 seek 精确到样本；只有放在 -i 之前才算输入选项。
            cmd += ['-ss', '%.3f' % (i0 / float(fps))]
        cmd += ['-i', wav_path]

    cmd += ['-map', '0:v:0']
    if wav_path:
        cmd += ['-map', '1:a:0']

    if upright:
        # 观看坐标是 u = y、v = (panel_w-1) - x，反解出来正好是**逆时针 90°**，
        # 也就是 ffmpeg 的 transpose=2 —— 正好是打包时 transpose=1 的逆操作
        # （见 tools/pack_video.py 的 vf 拼装处）。这是重编码，只在需要
        # "看图方向正"时用。
        cmd += ['-vf', 'transpose=2',
                '-c:v', 'libx264', '-preset', 'veryfast', '-crf', '18',
                '-pix_fmt', 'yuv420p']
    elif vcopy:
        cmd += ['-c:v', 'copy']
    else:
        cmd += ['-c:v', 'libx264', '-preset', 'veryfast', '-crf', '18',
                '-pix_fmt', 'yuv420p']

    if wav_path:
        if shift_ms > 0:
            cmd += ['-af', 'adelay=%d' % shift_ms]
        elif shift_ms < 0:
            cmd += ['-af', 'atrim=start=%.3f,asetpts=PTS-STARTPTS' % (-shift_ms / 1000.0)]
        cmd += ['-c:a', 'aac', '-b:a', '128k']

    if limit_sec > 0:
        cmd += ['-t', '%.3f' % (n_use / float(fps))]

    cmd += ['-movflags', '+faststart', out]

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    prev = info['offs'][i0 - 1] if i0 > 0 else 0
    try:
        with open(anim, 'rb') as f:
            f.seek(info['ds'] + prev)        # 直接跳到第 i0 帧的起点，不空转读盘
            for end in info['offs'][i0:i1]:
                proc.stdin.write(f.read(end - prev))
                prev = end
    except BrokenPipeError:
        pass        # ffmpeg 提前退出（参数问题），退出码会说明
    finally:
        try:
            proc.stdin.close()
        except BrokenPipeError:
            pass
    return proc.wait()


def ffprobe_streams(path):
    r = subprocess.run(
        ['ffprobe', '-v', 'error', '-show_entries',
         'stream=codec_type,codec_name,width,height,r_frame_rate,duration',
         '-of', 'default=noprint_wrappers=1', path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.stdout.decode('utf-8', 'replace').strip()


def main():
    ap = argparse.ArgumentParser(
        description='把 MVJ1 素材 + WAV 反向重组为 MP4（用于交叉验证音画同步）')
    ap.add_argument('-i', '--input', required=True, help='输入 .anim')
    ap.add_argument('-o', '--output', required=True, help='输出 .mp4')
    ap.add_argument('-a', '--audio', default=None,
                    help='音轨 .wav（默认取同目录同名换后缀；传 none 表示不要音轨）')
    ap.add_argument('--upright', action='store_true',
                    help='把画面转成正立方向（重编码，慢；默认原样复制，快且无损）')
    ap.add_argument('--audio-shift', type=int, default=0, metavar='MS',
                    help='音轨相对画面平移 MS 毫秒（正=声音更慢）。'
                         '用来二分"到底慢多少"：听到齐了，这个数就是偏差量')
    ap.add_argument('--probe', type=int, default=12,
                    help='体检开头多少帧的重复情况（默认 12）')
    ap.add_argument('--start-sec', type=float, default=0, metavar='SEC',
                    help='从素材第 SEC 秒开始（画面跳过前面的帧，音轨同步 seek）。'
                         '用来切中段试听 —— 段开头往往是最特殊的一段')
    ap.add_argument('--limit-sec', type=float, default=0, metavar='SEC',
                    help='从起点再取 SEC 秒（0 = 一直到末尾）')
    args = ap.parse_args()

    if not os.path.exists(args.input):
        die('找不到 %s' % args.input)

    info = parse_anim(args.input)
    n, fps = info['n'], info['fps']
    v_sec = n / float(fps)

    print('== 容器 ==')
    print('   %s' % args.input)
    print('   帧 %ux%u  %ufps  flags=0x%X（bit0=%s）  共 %u 帧  最大单帧 %uB'
          % (info['fw'], info['fh'], fps, info['flags'],
             '已预旋转' if (info['flags'] & 1) else '未旋转',
             n, info['max_frame']))
    print('   容器声明时长 %.3fs（%u ms）' % (info['dur_ms'] / 1000.0, info['dur_ms']))

    # ---- 音轨 -------------------------------------------------------------
    wav_path = args.audio
    if wav_path is None:
        base = args.input[:-5] if args.input.lower().endswith('.anim') else args.input
        cand = base + '.wav'
        wav_path = cand if os.path.exists(cand) else ''
    elif wav_path.strip().lower() == 'none':
        wav_path = ''

    a = None
    if wav_path:
        if not os.path.exists(wav_path):
            die('找不到音轨 %s（不要音轨请传 --audio none）' % wav_path)
        a = wav_info(wav_path)
        print('== 音轨 ==')
        print('   %s' % wav_path)
        print('   %uHz %uch %ubit  共 %u 样本  %.3fs'
              % (a['rate'], a['ch'], a['bits'], a['frames'], a['sec']))

    # ---- 静态体检 ---------------------------------------------------------
    print('== 名义时长 ==')
    print('   画面 %.3fs（%u 帧 / %ufps）' % (v_sec, n, fps))
    if a:
        res = a['sec'] - v_sec
        frames = abs(res) * fps
        note = 'OK' if frames <= 1.5 else '偏大，查片源音画起点'
        print('   音轨 %.3fs   残差 %+.0fms（%.1f 帧）  %s'
              % (a['sec'], res * 1000.0, frames, note))
        print('   （残差是"音轨末尾 vs 画面末尾"，不是起点偏差。要测起点只能靠'
              '下面输出的 MP4 自己去听/看。）')

    dup, md5s = head_dup_frames(args.input, info, args.probe)
    print('== 开头重复帧 ==')
    print('   前 %d 帧 md5: %s' % (len(md5s), ' '.join(md5s)))
    if dup >= 2:
        print('   ** 有 %d 处连续重复帧 —— 这是"打包时用首帧填空档"的签名，'
              '画面会整体提前，声音听着就慢 **' % dup)
    else:
        print('   重复 %d 处（帧率转换本身的产物，正常）' % dup)

    # ---- 重组 -------------------------------------------------------------
    print('== 重组 ==')
    if args.start_sec > 0:
        print('   从素材 %.3fs 起（第 %d 帧）'
              % (args.start_sec, int(round(args.start_sec * fps))))
    if args.limit_sec:
        print('   取 %gs 到 %.3fs' % (args.limit_sec, args.start_sec + args.limit_sec))
    elif args.start_sec > 0:
        print('   取到素材末尾')
    if args.audio_shift:
        print('   音轨平移 %+dms' % args.audio_shift)
    print('   画面: %s%s' % ('转正立（重编码）' if args.upright else '原样复制（无损）',
                             '' if a else '，无音轨'))

    if os.path.dirname(args.output):
        os.makedirs(os.path.dirname(args.output), exist_ok=True)

    rc = encode(args.input, info, wav_path, args.output, args.upright,
                args.audio_shift, vcopy=True,
                start_sec=args.start_sec, limit_sec=args.limit_sec)
    if rc != 0 or not os.path.exists(args.output) or os.path.getsize(args.output) == 0:
        if not args.upright:
            sys.stderr.write('   （mjpeg 直接封装失败，回退到重编码）\n')
            rc = encode(args.input, info, wav_path, args.output, args.upright,
                        args.audio_shift, vcopy=False,
                        start_sec=args.start_sec, limit_sec=args.limit_sec)
    if rc != 0 or not os.path.exists(args.output):
        die('ffmpeg 重组失败（退出码 %d）' % rc)

    print('   写出 %s  %.1f MB' % (args.output, os.path.getsize(args.output) / 1048576.0))
    print('== ffprobe 复核 ==')
    for line in ffprobe_streams(args.output).splitlines():
        print('   %s' % line)
    print('')
    print('把 %s 拖进任意播放器：画面和声音同时到位 = 素材是齐的。' % args.output)
    print('如果不齐，用 --audio-shift 二分出毫秒数，再决定是改素材还是改固件。')


if __name__ == '__main__':
    main()
