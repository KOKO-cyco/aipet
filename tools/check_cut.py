#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_cut.py —— 量「素材内部时刻」到底对应「片源哪一刻」。

用途：验切段和对齐有没有错位。取素材 T 秒处的 3 秒音频包络，在片源标称位置
(O+T) 前后各 1 秒里滑动，相关系数最大的位置就是这段素材真正的来源时刻。

    python3 check_cut.py 战狼1-2.anim /片源/战狼1.mp4 60 1800
    -> 素材第 60s <-> 片源第 1860.000s   偏差 +0ms   r=0.972

第 4 个参数 O 是该段起点在**片源时间轴**上的位置：-1 段 0、-2 段 1800、-3 段 3600。

实测基准值（2026-09-24）：
  * `战狼1-2` 偏差 **0ms** —— 切点严丝合缝；
  * `战狼1-1` 偏差 **+205ms，而这是对的**：素材第 0 秒故意锚在片源 0.2067s
    （源视频流起点），整段后移了 203ms 去咬住画面。别把它当错位。

纯标准库实现（WSL 的 python3 没有 numpy）：1ms 包络，2001 个滞后 x 3000 点，
sum(map(mul, ...)) 一两秒跑完。
"""
import array
import math
import operator
import subprocess
import sys
import wave

SR    = 16000
HOP   = 16          # 16000/16 = 1000Hz 包络（1ms 一个点）
WIN   = 3.0         # 比对窗口（秒）
SLACK = 1.0         # 允许的偏差范围（秒，正负各这么多）


def to_samples(raw):
    a = array.array('h')
    a.frombytes(raw)
    return a


def envelope(a):
    """每 HOP 个采样取绝对值最大值 -> 1ms 包络。"""
    out = []
    for i in range(0, len(a) - HOP + 1, HOP):
        m = 0
        for k in range(i, i + HOP):
            v = a[k]
            if v < 0:
                v = -v
            if v > m:
                m = v
        out.append(float(m))
    return out


def decode(path, ss, t):
    r = subprocess.run(['ffmpeg', '-v', 'error', '-ss', '%.3f' % ss, '-t', '%.3f' % t,
                        '-i', path, '-vn', '-ac', '1', '-ar', str(SR),
                        '-f', 's16le', '-'],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.exit('ffmpeg 失败: %s' % r.stderr.decode('utf-8', 'replace'))
    return to_samples(r.stdout)


def pearson(a, b):
    n = len(a)
    sa = sum(a)
    sb = sum(b)
    saa = sum(v * v for v in a)
    sbb = sum(v * v for v in b)
    sab = sum(map(operator.mul, a, b))
    va = saa - sa * sa / n
    vb = sbb - sb * sb / n
    if va <= 0 or vb <= 0:
        return 0.0
    return (sab - sa * sb / n) / math.sqrt(va * vb)


def main():
    anim, src, t_in, off = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
    wav = anim[:-5] + '.wav' if anim.lower().endswith('.anim') else anim

    with wave.open(wav, 'rb') as w:
        rate = w.getframerate()
        w.setpos(int(round(t_in * rate)))
        raw = w.readframes(int(round(WIN * rate)))
    a = envelope(to_samples(raw))
    print('素材 %s 第 %.3fs 起 %.1fs：%d 点包络' % (wav, t_in, WIN, len(a)))

    nominal = off + t_in
    ss = nominal - SLACK - 0.5
    b = envelope(decode(src, ss, WIN + 2 * SLACK + 1.0))
    print('片源第 %.3fs 起 %.1fs：%d 点包络' % (ss, WIN + 2 * SLACK + 1.0, len(b)))

    m = len(a)
    off0 = int(round((nominal - ss) * 1000))          # a[0] 在 b 里的标称下标
    best, best_r = None, -2.0
    for lag in range(-int(SLACK * 1000), int(SLACK * 1000) + 1):
        j = off0 + lag
        if j < 0 or j + m > len(b):
            continue
        r = pearson(a, b[j:j + m])
        if r > best_r:
            best_r, best = r, lag

    real = nominal + best / 1000.0
    print('')
    print('素材第 %.3fs  <->  片源第 %.3fs     偏差 %+.0f ms    r=%.3f'
          % (t_in, real, (real - nominal) * 1000.0, best_r))
    print('（r 越接近 1 越可信。偏差为负 = 素材这段其实来自片源更早的位置。）')


if __name__ == '__main__':
    main()
