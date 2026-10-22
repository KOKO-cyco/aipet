#!/usr/bin/env python
"""多动作统一工具 v3: 大小/长宽比/位置一致 (豆包同来源素材版)。

流程:
  1. 测每个视频第一帧(基础姿势)的角色: 躯干宽%/总高%/脚底%/中心x%
     (测量口径: 处理版测第一轮抠像输出 豆包/已抠像/<名>.webm —— 与 old_fit 同缩放;
      prekeyed 版测源, 源即 1280x720 画布)
  2. 以 REFS(同来源基准组)的中位数为基准:
     宽归一 s_w = 目标躯干/实测躯干
     高修正 K   = 参考长宽比/实测长宽比   (总高/躯干宽 对齐, K=1 即不修)
     锚点    = (640, 参考脚底中位)        (角色 bbox 底中钉在画布固定点)
  3. --run: 调 key_greenscreen.py 重出到 豆包/统一动作集/
     (待机从 mp4 重抠单次插值; 同来源 7 个 --prekeyed 只缩放+锚点)
  4. --verify: 复测输出(第一帧一致性)并出对比图

尾帧衔接(交叉溶解回待机)能力保留, 本次未启用: USE_TAIL=False。
启用后每个动作结尾加 N 帧溶解(末端与待机首帧逐像素一致), 待机自循环用 --tail-self。

用法:
  python unify_actions.py                 # 只出计划(测量+计算, 不动文件)
  python unify_actions.py --run           # 按计划重出
  python unify_actions.py --verify        # 验校 并出对比图
  python unify_actions.py --run --verify  # 一条龙

依赖: pip install imageio imageio-ffmpeg numpy pillow
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent          # 待处理动作/
KEY = Path(__file__).resolve().parent / "key_greenscreen.py"
PY = sys.executable
OUT_DIR = ROOT / "豆包" / "统一动作集"
CANVAS = (1280, 720)
IDLE = "待机"
USE_TAIL = False          # 尾帧衔接开关(本次统一不启用)

# ---------------- 动作表(换素材改这里) ----------------
# (名称, 渲染源, 是否已抠好, 旧fit缩放, 覆盖物矩形[源坐标])
_P_WM = [(0, 0, 345, 68), (560, 860, 720, 960)]       # 待机(720x960 竖屏源坐标)
ACTIONS = [
    # 待机: 唯一旧来源竖屏, 从 mp4 重抠(scale=old_fit*s_w, 单次插值)
    ("待机",   "豆包/待机.mp4",   False, 0.75, _P_WM),
    # 同来源 7 个: 已抠像输出即 1280x720 画布, 只缩放+锚点
    ("开心",   "豆包/已抠像/开心.webm",   True, 1.0, []),
    ("伤心",   "豆包/已抠像/伤心.webm",   True, 1.0, []),
    ("傲娇",   "豆包/已抠像/傲娇.webm",   True, 1.0, []),
    ("思考",   "豆包/已抠像/思考.webm",   True, 1.0, []),
    ("拍照",   "豆包/已抠像/拍照.webm",   True, 1.0, []),
    ("舞狮",   "豆包/已抠像/舞狮.webm",   True, 1.0, []),
    ("记笔记", "豆包/已抠像/记笔记.webm", True, 1.0, []),
]
# 基准组 = 同来源 7 个(比例一致性好); 待机是旧来源, 放大/缩小去凑基准
REFS = [n for n, s, pk, *_ in ACTIONS if n != IDLE]

# ---------------- 尾帧长度(仅 USE_TAIL=True 时生效) ----------------
# 姿势差(IoU) -> 溶解帧数; 未实测的走 TAIL_DEFAULT, 批量后按 verify 起点IoU 复核
TAIL_IOU = {}
TAIL_DEFAULT = 12

A_TH = 128        # alpha 阈值: 角色可视边缘(与 key_greenscreen --place 同标准)
TORSO_FRAC = 0.35 # 躯干测量高度: 冠部往下 35%(避开发型/手臂)


# ---------------- 帧计数 / 测量 ----------------

def _ffmpeg():
    import imageio_ffmpeg
    return imageio_ffmpeg.get_ffmpeg_exe()


def count_frames(p):
    """精确帧数: 解一遍到 null, 解析 stderr 的 frame= 计数。"""
    r = subprocess.run([_ffmpeg(), "-v", "info", "-i", str(p), "-f", "null", "-"],
                       capture_output=True, text=True)
    m = re.findall(r"frame=\s*(\d+)", r.stderr)
    return int(m[-1]) if m else None


def decode_frame(webm, idx):
    """按帧号抽 1 帧(PNG, 保 alpha), 返回 RGBA ndarray 或 None"""
    import tempfile
    from PIL import Image
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td) / "f.png"
        subprocess.run([_ffmpeg(), "-y", "-c:v", "libvpx-vp9", "-i", str(webm),
                        "-vf", f"select=eq(n\\,{idx})", "-frames:v", "1", str(tmp)],
                       capture_output=True)
        if not tmp.exists() or tmp.stat().st_size < 1000:
            return None
        im = Image.open(tmp)
        arr = np.frombuffer(im.tobytes(), np.uint8).copy()
        w, h = im.size
        im.close()
    if arr.size != w * h * 4:
        return None
    return arr.reshape(h, w, 4)


def _run_width_row(row):
    best = cur = 0
    for v in row:
        cur = cur + 1 if v else 0
        if cur > best:
            best = cur
    return best


def measure(webm):
    """前 3 帧中位: 躯干宽%/总高%/脚底%/中心x% (画布百分比, 分辨率无关)"""
    ts, hs, bs, cxs = [], [], [], []
    for idx in (0, 1, 2):
        rgba = decode_frame(webm, idx)
        if rgba is None:
            continue
        a = rgba[..., 3]
        m = a > A_TH
        ys, xs = np.where(m)
        if not len(ys):
            continue
        y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
        H = y1 - y0 + 1
        W, CH = a.shape[1], a.shape[0]
        ny = y0 + int(H * TORSO_FRAC)
        torso = max(_run_width_row(m[r]) for r in range(ny, min(ny + 3, m.shape[0])))
        ts.append(torso / W * 100)
        hs.append(H / CH * 100)
        bs.append(y1 / CH * 100)
        cxs.append((x0 + x1) / 2 / W * 100)
    if not ts:
        return None
    return dict(torso=float(np.median(ts)), H=float(np.median(hs)),
                bottom=float(np.median(bs)), cx=float(np.median(cxs)))


def measure_all():
    """prekeyed 版测源(已抠 1280x720 画布);
    处理版(待机)测第一轮抠像输出 豆包/已抠像/<名>.webm —— 与 old_fit 同缩放口径。"""
    out = {}
    for name, src, prekeyed, old_fit, wms in ACTIONS:
        if prekeyed:
            p = ROOT / src
        else:
            p = ROOT / "豆包" / "已抠像" / f"{name}.webm"
        if not p.exists():
            print(f"!! 缺文件: {p}")
            continue
        out[name] = measure(p)
        print(f"  {name}: 躯干={out[name]['torso']:.1f}% 总高={out[name]['H']:.1f}% "
              f"脚底={out[name]['bottom']:.1f}% cx={out[name]['cx']:.1f}%", flush=True)
    return out


def cmp2(a, b):
    """两帧轮廓比较: IoU + 上/下沿差(px)"""
    ma, mb = a[..., 3] > A_TH, b[..., 3] > A_TH
    inter = (ma & mb).sum(); union = (ma | mb).sum()
    ya, xa = np.where(ma); yb, xb = np.where(mb)
    return dict(iou=float(inter / max(union, 1)),
                top=int(ya.min() - yb.min()), bot=int(ya.max() - yb.max()))


# ---------------- 计划 ----------------

def tail_n(name, total):
    if not USE_TAIL:
        return 0
    iou = TAIL_IOU.get(name)
    if iou is None:
        n = TAIL_DEFAULT
    elif iou >= 0.95:
        n = 8
    elif iou >= 0.93:
        n = 10
    elif iou >= 0.90:
        n = 12
    else:
        n = 14
    return max(4, min(n, int(total * 0.25)))     # 尾帧不超过本体 1/4


def plan(metrics, counts):
    cw, ch = CANVAS
    t_torso = float(np.median([metrics[r]["torso"] for r in REFS]))
    t_H = float(np.median([metrics[r]["H"] for r in REFS]))
    t_bottom = float(np.median([metrics[r]["bottom"] for r in REFS]))
    t_aspect = t_H / t_torso
    print(f"\n基准({len(REFS)} 个同来源中位): 躯干={t_torso:.2f}%  总高={t_H:.2f}%  脚底={t_bottom:.2f}%"
          f"({t_bottom/100*ch:.0f}px)  长宽比={t_aspect:.3f}")
    rows = []
    print(f"\n{'动作':<10} {'躯干%':>6} {'总高%':>6} {'宽缩放':>7} {'高修正K':>7} {'新fit':>7}  说明")
    for name, src, prekeyed, old_fit, wms in ACTIONS:
        mt = metrics[name]
        s_w = t_torso / mt["torso"]
        K = t_aspect / (mt["H"] / mt["torso"])
        new_fit = old_fit * s_w
        rows.append(dict(name=name, src=src, prekeyed=prekeyed, wm=wms,
                         scale=new_fit, stretch=K, place=(cw / 2, t_bottom / 100 * ch),
                         tail=tail_n(name, counts.get(name, 0)),
                         total=counts.get(name, 0), cur=mt))
        note = []
        if abs(s_w - 1) > 0.02:
            note.append(("放大" if s_w > 1 else "缩小") + f"宽{abs(s_w-1)*100:.0f}%")
        if abs(K - 1) > 0.02:
            note.append(("拉高" if K > 1 else "压低") + f"高{abs(K-1)*100:.0f}%")
        if not note:
            note.append("基本不动")
        print(f"{name:<10} {mt['torso']:>5.1f}% {mt['H']:>5.1f}% {s_w:>7.3f} {K:>7.3f} {new_fit:>7.4f}  "
              + " ".join(note), flush=True)
    return rows, dict(torso=t_torso, H=t_H, bottom=t_bottom, aspect=t_aspect)


# ---------------- 执行 ----------------

def run(rows, only=None):
    from PIL import Image
    OUT_DIR.mkdir(exist_ok=True)
    if only:
        rows = [r for r in rows if r["name"] in only]
    idle_png = ROOT / "_tail" / "idle_first.png"
    need_idle_png = USE_TAIL and any(r["name"] != IDLE and r["tail"] > 0 for r in rows)
    if need_idle_png:
        idle_png.parent.mkdir(exist_ok=True)
    for r in rows:
        out = OUT_DIR / f"{r['name']}.webm"
        cmd = [PY, str(KEY), str(ROOT / r["src"]), str(out),
               "--scale", f"{r['scale']:.4f}",
               "--stretch", f"{r['stretch']:.4f}",
               "--place", f"{r['place'][0]:.0f},{r['place'][1]:.0f}",
               "--canvas", f"{CANVAS[0]}x{CANVAS[1]}"]
        if r["prekeyed"]:
            cmd.append("--prekeyed")
        for (x0, y0, x1, y1) in r["wm"]:
            cmd += ["--wm", f"{x0},{y0},{x1},{y1}"]
        if r["tail"] > 0:
            cmd += ["--tail", str(r["tail"])]
            if r["name"] == IDLE:
                cmd.append("--tail-self")       # 待机: 溶到自己的首帧(平滑自循环)
            else:
                cmd += ["--tail-target", str(idle_png)]
        print(f"\n=== {r['name']} -> {out}  (tail={r['tail']}) ===", flush=True)
        rc = subprocess.run(cmd).returncode
        if rc != 0:
            print(f"!! {r['name']} 失败 rc={rc}")
            sys.exit(1)
        if r["name"] == IDLE and need_idle_png:
            rgba = decode_frame(out, 0)
            if rgba is not None:
                Image.fromarray(rgba).save(idle_png)
                print(f"idle target png: {idle_png}", flush=True)


# ---------------- 验校 + 对比图 ----------------

def verify(rows, target, only=None):
    from PIL import Image, ImageDraw, ImageFont
    cw, ch = CANVAS
    FONT = ImageFont.truetype("C:\\Windows\\Fonts\\msyh.ttc", 13)
    FONT_S = ImageFont.truetype("C:\\Windows\\Fonts\\msyh.ttc", 11)
    if only:
        rows = [r for r in rows if r["name"] in only]
    base_y = target["bottom"] / 100 * ch
    ok = True

    # ---- 1) 第一帧一致性 ----
    print(f"\n{'动作':<10} {'躯干%':>6} {'偏差':>6} {'总高%':>6} {'偏差':>6} {'脚底%':>6} {'中心x%':>6}  判定")
    mets = {}
    for r in rows:
        p = OUT_DIR / f"{r['name']}.webm"
        mt = measure(p)
        if mt is None:
            print(f"{r['name']:<10} 测量失败!")
            ok = False
            continue
        mets[r["name"]] = mt
        d1 = (mt["torso"] - target["torso"]) / target["torso"] * 100
        d2 = (mt["H"] - target["H"]) / target["H"] * 100
        verdict = "OK" if abs(d1) <= 2.5 and abs(d2) <= 2.5 else "!! 超差"
        if "!!" in verdict:
            ok = False
        print(f"{r['name']:<10} {mt['torso']:>5.1f}% {d1:>+5.1f}% {mt['H']:>5.1f}% {d2:>+5.1f}% "
              f"{mt['bottom']:>5.1f}% {mt['cx']:>5.1f}%  {verdict}", flush=True)

    # ---- 2) 尾帧验校(仅 USE_TAIL) ----
    if USE_TAIL and any(r["tail"] > 0 for r in rows):
        tgt = decode_frame(OUT_DIR / f"{IDLE}.webm", 0)
        print(f"\n{'动作':<10} {'尾帧N':>5} {'末端IoU':>7} {'末端上/下':>9} {'起点IoU':>7} {'起点上/下':>9}  判定")
        for r in rows:
            p = OUT_DIR / f"{r['name']}.webm"
            nf = count_frames(p)
            N = r["tail"]
            if tgt is None or nf is None:
                print(f"{r['name']:<10} 尾帧验校失败(解码/计数)")
                ok = False
                continue
            last = decode_frame(p, nf - 1)
            c1 = cmp2(last, tgt)
            v_end = c1["iou"] >= 0.995 and abs(c1["top"]) <= 1 and abs(c1["bot"]) <= 1
            note = "OK" if v_end else "!! 末端不一致"
            if not v_end:
                ok = False
            print(f"{r['name']:<10} {N:>5} {c1['iou']:>7.3f} {c1['top']:>+4d}/{c1['bot']:>+4d}  {note}",
                  flush=True)
    else:
        print("\n(USE_TAIL=False, 跳过尾帧验校)")

    # ---- 3) 第一帧一致性对比图(带基准线) ----
    cells = [(r["name"], decode_frame(OUT_DIR / f"{r['name']}.webm", 0)) for r in rows]
    tw, th = 240, 135
    ncol = 4
    nrow = (len(cells) + ncol - 1) // ncol
    sheet = Image.new("RGB", (tw * ncol + 60, th * nrow + 130), (26, 28, 32))
    d = ImageDraw.Draw(sheet)
    d.text((12, 8), f"统一验校(第一帧)  基准: 躯干{target['torso']:.1f}%  总高{target['H']:.1f}%  "
                    f"脚底{base_y:.0f}px  中心x640  (白线=基准, 全部动作应重合)", font=FONT, fill=(235, 235, 235))
    for i, (name, rgba) in enumerate(cells):
        if rgba is None:
            continue
        im = Image.fromarray(rgba).resize((tw, th), Image.LANCZOS)
        bg = Image.new("RGBA", (tw, th), (105, 108, 115, 255))
        bg.alpha_composite(im)
        x = 12 + (i % ncol) * (tw + 8)
        y = 34 + (i // ncol) * (th + 32)
        sheet.paste(bg.convert("RGB"), (x, y))
        ly = y + th * target["bottom"] / 100
        lx = x + tw * 0.5
        d.line([(x, ly), (x + tw, ly)], fill=(255, 255, 255), width=1)
        d.line([(lx, y), (lx, y + th)], fill=(255, 255, 255), width=1)
        mt = mets.get(name)
        d1 = (mt["torso"] - target["torso"]) / target["torso"] * 100 if mt else 0
        d2 = (mt["H"] - target["H"]) / target["H"] * 100 if mt else 0
        d.text((x + 4, y + th + 3), f"{name} 躯干{d1:+.1f}% 高{d2:+.1f}%" if mt else name,
               font=FONT_S, fill=(220, 220, 220))
    sheet.save(OUT_DIR / "一致性验校.png")

    print(f"\n对比图: {OUT_DIR/'一致性验校.png'}")
    print("验校结果:", "全部通过" if ok else "有超差, 看下表")
    return ok


def main():
    ap = argparse.ArgumentParser(description="多动作大小/长宽比/位置统一(豆包版)")
    ap.add_argument("--run", action="store_true", help="按计划重出视频")
    ap.add_argument("--verify", action="store_true", help="验校输出并出对比图")
    ap.add_argument("--only", default=None, help="只跑指定动作, 逗号分隔")
    a = ap.parse_args()
    only = set(a.only.split(",")) if a.only else None

    print("测量各动作第一帧(基础姿势; 处理版测第一轮抠像输出, 已抠版测源) ...", flush=True)
    metrics = measure_all()
    if len(metrics) < len(ACTIONS):
        print("!! 有文件缺失, 中止")
        sys.exit(1)
    counts = {}
    for name, src, *_ in ACTIONS:
        n = count_frames(ROOT / src)
        counts[name] = n or 0
        if not n:
            print(f"!! {name} 源帧数探测失败")
    rows, target = plan(metrics, counts)
    (ROOT / "_unify").mkdir(exist_ok=True)
    (ROOT / "_unify" / "plan.json").write_text(json.dumps(
        {"target": target,
         "rows": [{k: (list(v) if isinstance(v, tuple) else v) for k, v in r.items()} for r in rows]},
        ensure_ascii=False, indent=1))
    if a.run:
        run(rows, only)
    if a.verify:
        verify(rows, target, only)
    if not a.run and not a.verify:
        print("\n(以上为计划, 加 --run 执行重出, --verify 验校)")


if __name__ == "__main__":
    main()
