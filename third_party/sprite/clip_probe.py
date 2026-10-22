#!/usr/bin/env python3
"""给动作片段做体检：动作能量曲线 + 角色位移曲线 + 静止段 / 峰值 / 摆动周期。

    python third_party/sprite/clip_probe.py third_party/sprite/my_pet/豆包视频/*.webm --out probe.txt

输出（每个片段一段）：

    能量 |0123456789...|   每帧与前一帧的差异，归一化到 0-9 —— "这一帧动得多不多"
    纵心 |...|             角色 alpha 包围盒的垂直中心（相对首帧），看跳起/落下
    横心 |...|             同上，水平中心，看左右摆动
    静止段 / 峰值 / 周期

**为什么必须 `-c:v libvpx-vp9`**：这些素材的 alpha 在 VP9 侧数据里，ffmpeg 的原生
`vp9` 解码器不读 alpha 平面 —— 不指定的话所有帧的 alpha 全是 255，包围盒恒等于
整幅画面，位移曲线就成了一条直线。见 third_party/sprite/fish_extract.py 的文件头。

为什么要降到 160x90：体检只看"动没动、往哪动"，不需要细节；降采样让 241 帧的
解码只有 14MB，整个库跑一遍也就几分钟。
"""

import argparse
import io
import os
import shutil
import subprocess
import sys

import numpy as np

PROBE_W, PROBE_H = 160, 90

# alpha 高于这个才算角色 —— 素材背景 alpha 是 1 不是 0（见 fish_extract.py）
ALPHA_MIN = 32


def ffmpeg_bin():
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit("找不到 ffmpeg（PATH 里没有）")
    return exe


def decode(ff, path):
    """-> (n, H, W, 4) uint8 RGBA"""
    cmd = [ff, "-v", "error", "-c:v", "libvpx-vp9", "-i", path,
           "-vf", "scale=%d:%d" % (PROBE_W, PROBE_H),
           "-pix_fmt", "rgba", "-f", "rawvideo", "-"]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        sys.exit("ffmpeg 解码失败 (%s):\n%s" % (path, r.stderr.decode("utf-8", "replace")))
    n = len(r.stdout) // (PROBE_W * PROBE_H * 4)
    if n == 0:
        sys.exit("解不出帧: %s" % path)
    return np.frombuffer(r.stdout, dtype=np.uint8)[:n * PROBE_W * PROBE_H * 4] \
             .reshape(n, PROBE_H, PROBE_W, 4)


def spark(vals, lo=None, hi=None):
    """一串数 -> 0-9 的字符画。"""
    v = np.asarray(vals, dtype=np.float64)
    if lo is None:
        lo = float(v.min())
    if hi is None:
        hi = float(v.max())
    if hi - lo < 1e-9:
        return "0" * len(v)
    q = np.clip((v - lo) / (hi - lo), 0.0, 1.0)
    return "".join(str(int(round(x * 9))) for x in q)


def longest_static_prefix(v, thr):
    i = 0
    while i < len(v) and v[i] <= thr:
        i += 1
    return i


def longest_static_suffix(v, thr):
    i = len(v)
    while i > 0 and v[i - 1] <= thr:
        i -= 1
    return len(v) - i


def period_of(v, min_lag=4):
    """自相关找主周期（帧）。返回 (周期, 相关强度)。"""
    x = np.asarray(v, dtype=np.float64)
    x = x - x.mean()
    if x.std() < 1e-9:
        return 0, 0.0
    ac = np.correlate(x, x, mode="full")[len(x) - 1:]
    ac /= ac[0]
    # 找第一个"局部峰且超过 0.3"的滞后 —— 摆动类信号这里会给出周期
    best, bestv = 0, 0.0
    for lag in range(min_lag, len(x) // 2):
        if ac[lag] > ac[lag - 1] and ac[lag] >= ac[lag + 1] and ac[lag] > 0.3:
            if ac[lag] > bestv:
                best, bestv = lag, ac[lag]
            break
    return best, bestv


def probe(ff, path, out, label, dump=None):
    a = decode(ff, path).astype(np.int16)
    n, H, W = a.shape[0], a.shape[1], a.shape[2]
    rgb = a[:, :, :, :3]
    al = a[:, :, :, 3] > ALPHA_MIN

    # 动作能量：只在角色所在处比（背景 alpha=1，本来就不该算进来）
    d = np.abs(np.diff(rgb, axis=0)).astype(np.float32)              # (n-1,H,W,3)
    m = (al[1:] | al[:-1])[:, :, :, None].astype(np.float32)
    energy = (d * m).sum(axis=(1, 2, 3)) / np.maximum(m.sum(axis=(1, 2, 3)) * 3, 1)
    energy = np.concatenate([[0.0], energy])

    # 角色包围盒的中心（相对首帧），按源像素比例折算
    ys = np.arange(H)[None, :, None]
    xs = np.arange(W)[None, None, :]
    cnt = np.maximum(al.sum(axis=(1, 2)), 1)
    cy = (al * ys).sum(axis=(1, 2)) / cnt
    cx = (al * xs).sum(axis=(1, 2)) / cnt
    scale = 640.0 / PROBE_W

    # 包围盒上下沿（源像素）—— **判断"离地没有"就看底沿**：
    # 角色飘起来时底沿会明显抬高，比质心干净得多。
    row_any = al.any(axis=2)                                    # (n,H)
    top = row_any.argmax(axis=1).astype(np.float64)
    bot = (H - 1 - row_any[:, ::-1].argmax(axis=1)).astype(np.float64)
    top *= 360.0 / PROBE_H
    bot *= 360.0 / PROBE_H

    thr = max(0.5, 0.08 * float(energy.max()))
    pre = longest_static_prefix(energy, thr)
    suf = longest_static_suffix(energy, thr)
    pk = int(np.argmax(energy))
    per, corr = period_of(cx)

    out.write("### %s\n" % label)
    out.write("%d 帧  %.2fs  峰值在 %.2fs（第 %d 帧）\n"
              % (n, n / 24.0, pk / 24.0, pk))
    out.write("能量 |%s|\n" % spark(energy))
    out.write("纵心 |%s|  （峰值差 %.0fpx 源）\n"
              % (spark(cy, cy.min(), cy.max()), (cy.max() - cy.min()) * scale))
    out.write("横心 |%s|  （峰值差 %.0fpx 源）\n"
              % (spark(cx, cx.min(), cx.max()), (cx.max() - cx.min()) * scale))
    out.write("底沿 |%s|  （%.0f..%.0f 源像素，越小=越高）\n"
              % (spark(bot, bot.min(), bot.max()), bot.min(), bot.max()))
    out.write("静止: 头 %.2fs / 尾 %.2fs（阈值 %.2f）  " % (pre / 24.0, suf / 24.0, thr))
    if per:
        out.write("横心周期 ~%.2fs (%.2f)  → %.1f 个周期\n"
                  % (per / 24.0, corr, n / float(per)))
    else:
        out.write("横心无明显周期\n")
    out.write("\n")

    if dump is not None:
        dump.write("# %s\n帧,能量,质心y,质心x,底沿,顶沿\n" % label)
        for i in range(n):
            dump.write("%d,%.3f,%.2f,%.2f,%.1f,%.1f\n"
                       % (i, energy[i], cy[i] * scale, cx[i] * scale, bot[i], top[i]))
        dump.write("\n")

    return dict(n=n, pre=pre, suf=suf, peak=pk, period=per, corr=corr,
                y_amp=(cy.max() - cy.min()) * scale,
                x_amp=(cx.max() - cx.min()) * scale)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--out", default=None, help="报告写到这个文件（默认 stdout）")
    ap.add_argument("--dump", default=None,
                    help="逐帧原始数据写成 CSV（挑切点用：帧,能量,质心y,质心x,底沿,顶沿）")
    args = ap.parse_args()

    ff = ffmpeg_bin()
    buf = io.StringIO()
    dmp = io.open(args.dump, "w", encoding="utf-8") if args.dump else None
    for p in args.paths:
        probe(ff, p, buf, p.replace("\\", "/"), dmp)
        print(".", end="", file=sys.stderr, flush=True)
    print(file=sys.stderr)
    if dmp:
        dmp.close()
        print("CSV:", args.dump)

    if args.out:
        with io.open(args.out, "w", encoding="utf-8") as f:
            f.write(buf.getvalue())
        print("报告:", args.out)
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        print(buf.getvalue())


if __name__ == "__main__":
    main()
