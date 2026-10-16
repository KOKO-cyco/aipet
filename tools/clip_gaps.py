#!/usr/bin/env python3
"""读 clip_probe.py --dump 出来的 CSV，找出每段里"几乎不动"的区间。

    python tools/clip_gaps.py all.csv --out gaps.txt

输出每个片段：
  * 头/尾静止多少帧
  * **中间**最长的静止区间（"干站着"）在哪
  * 粗粒度的能量轮廓（每 5 帧一格），用来判断"整体是不是太慢"

阈值取峰值能量的 8%（和 clip_probe.py 的"静止"判据一致），下限 0.5。
"""

import argparse
import io
import sys


def runs_below(v, thr):
    """所有 v[i] <= thr 的连续区间 [a, b)，按长度降序。"""
    out, i, n = [], 0, len(v)
    while i < n:
        if v[i] <= thr:
            j = i
            while j < n and v[j] <= thr:
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    out.sort(key=lambda r: r[1] - r[0], reverse=True)
    return out


def spark(v, lo, hi):
    if hi - lo < 1e-9:
        return "0" * len(v)
    return "".join(str(int(round(min(1.0, max(0.0, (x - lo) / (hi - lo))) * 9)))
                   for x in v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--out", default=None)
    ap.add_argument("--bucket", type=int, default=5, help="轮廓每几帧一格")
    args = ap.parse_args()

    buf = io.StringIO()
    name, fr, en = None, [], []

    def flush():
        if not fr:
            return
        peak = max(en) if en else 0.0
        thr = max(0.5, 0.08 * peak)
        rs = runs_below(en, thr)
        head = rs and rs[0][0] == 0
        n = len(en)

        # 中间那段：既不是从 0 开始，也不顶到末尾
        mid = [(a, b) for a, b in rs if a > 0 and b < n]
        buf.write("### %s\n" % name)
        buf.write("  %d 帧 %.2fs  峰值 %.1f  阈值 %.2f\n" % (n, n / 24.0, peak, thr))
        if rs:
            buf.write("  静止区间（前 4 长）: %s\n"
                      % "  ".join("%d..%d(%.2fs)" % (a, b - 1, (b - a) / 24.0)
                                  for a, b in rs[:4]))
        if mid:
            a, b = mid[0]
            buf.write("  **中间最长静止 %d..%d（%.2fs）** -> 挖掉后剩 %.2fs\n"
                      % (a, b - 1, (b - a) / 24.0, (n - (b - a)) / 24.0))
        # 粗轮廓
        k = args.bucket
        prof = [max(en[i:i + k]) for i in range(0, n, k)]
        buf.write("  轮廓(每%d帧取峰) |%s|\n" % (k, spark(prof, 0, peak)))
        buf.write("\n")

    for line in io.open(args.csv, encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith("# "):
            flush()
            name, fr, en = line[2:], [], []
        elif line.startswith("帧,") or not line:
            continue
        else:
            p = line.split(",")
            if len(p) >= 2:
                try:
                    fr.append(int(p[0]))
                    en.append(float(p[1]))
                except ValueError:
                    pass
    flush()

    if args.out:
        io.open(args.out, "w", encoding="utf-8").write(buf.getvalue())
        print("报告:", args.out)
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        print(buf.getvalue())


if __name__ == "__main__":
    main()
