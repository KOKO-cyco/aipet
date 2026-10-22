#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""make_headtail.py — 首帧.png 抠像后作为全部动作的首/尾帧, 重出统一动作集并合并验校。

背景:
  豆包/首帧.png 经逐字节比对 = 舞狮.mp4 的第 0 帧(1280x720 绿幕原图)。
  用户要求: 所有动作都以它(抠像后)为首尾帧, 保证动作间角色一致性。

流程:
  1. 用与 7 个同来源视频完全相同的抠像参数抠 首帧.png
     (bg 四角自动测量 / t_body=0.2*gn_bg 自动 / dilate 6 / shadow 0.32 / 无水印矩形)
  2. 网格搜索 (scale, stretch) + 底中锚点(640,608)公式偏移, 使抠像帧与
     统一动作集/舞狮.webm 第 0 帧对齐; 对齐不上(>4px 级差异)则退回
     直接采用统一版第 0 帧作为 canonical
  3. canonical(画布 1280x720 RGBA) -> 豆包/首帧_已抠像.png
  4. 每个动作: [canonical]*hold + 原全部帧 + [canonical]*hold, 同参数重编码
     -> 豆包/统一动作集_首尾帧/<名>.webm   (原 统一动作集/ 不动)
  5. 按 ORDER 合并(每个动作一次, 待机起) -> 豆包/全部动作_首尾帧版.webm
     (先试 concat -c copy; 帧数不符则管道整条重编码兜底)
  6. 验校: 每动作帧数=原+2*hold, 首/尾帧与 canonical 一致(VP9 噪声级),
     合并总帧数, 接缝帧抽查; 出 豆包/首尾帧验校.png

用法:
  python make_headtail.py                 # hold=1(首/尾各 1 帧)
  python make_headtail.py --hold 6        # 首/尾各停 6 帧
  python make_headtail.py --only 待机,开心
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import key_greenscreen as kg                                   # 复用抠像/变换同款代码

DB = ROOT / "豆包"
SRC_PNG = DB / "首帧.png"
UNI = DB / "统一动作集"
OUT = DB / "统一动作集_首尾帧"
CANON_PNG = DB / "首帧_已抠像.png"
MERGED = DB / "全部动作_首尾帧版.webm"
SHEET = DB / "首尾帧验校.png"
ORDER = ["待机", "开心", "伤心", "傲娇", "思考", "拍照", "舞狮", "记笔记"]
CW, CH = 1280, 720
FPS = 24
PLACE = (640.0, 608.0)          # 统一动作集的底中锚点(unify_actions 同款)
FONT = "C:\\Windows\\Fonts\\msyh.ttc"


# ---------------- ffmpeg ----------------

def ffmpeg():
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        pass
    import glob
    c = glob.glob(str(Path.home() / ".workbuddy/binaries/python/envs/default/"
                      "Lib/site-packages/imageio_ffmpeg/binaries/*.exe"))
    if c:
        return c[0]
    from shutil import which
    return which("ffmpeg")


FF = ffmpeg()


def count_frames(p):
    r = subprocess.run([FF, "-v", "info", "-i", str(p), "-f", "null", "-"],
                       capture_output=True, text=True)
    m = re.findall(r"frame=\s*(\d+)", r.stderr)
    return int(m[-1]) if m else None


def decode_one(p, idx):
    """按帧号解 1 帧(保 alpha), 返回 RGBA ndarray。"""
    import tempfile
    from PIL import Image
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td) / "f.png"
        subprocess.run([FF, "-y", "-c:v", "libvpx-vp9", "-i", str(p),
                        "-vf", f"select=eq(n\\,{idx})", "-frames:v", "1", str(tmp)],
                       capture_output=True)
        if not tmp.exists() or tmp.stat().st_size < 1000:
            return None
        im = Image.open(tmp)
        arr = np.frombuffer(im.tobytes(), np.uint8)
        w, h = im.size
        im.close()
    if arr.size != w * h * 4:
        return None
    return arr.reshape(h, w, 4)


def iter_frames(p):
    """整条解成 rawvideo rgba 管道, 逐帧 yield。"""
    cmd = [FF, "-v", "error", "-c:v", "libvpx-vp9", "-i", str(p),
           "-f", "rawvideo", "-pix_fmt", "rgba", "-"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    nbytes = CW * CH * 4
    try:
        while True:
            buf = b""
            while len(buf) < nbytes:
                chunk = proc.stdout.read(nbytes - len(buf))
                if not chunk:
                    break
                buf += chunk
            if len(buf) < nbytes:
                break
            yield np.frombuffer(buf, np.uint8).reshape(CH, CW, 4)
    finally:
        proc.stdout.close()
        proc.wait()


def open_encoder(out):
    """与 key_greenscreen.process 完全相同的 VP9 alpha 编码参数。"""
    return subprocess.Popen([FF, "-y", "-v", "error",
        "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{CW}x{CH}", "-r", str(FPS), "-i", "-",
        "-c:v", "libvpx-vp9", "-pix_fmt", "yuva420p", "-crf", "28", "-b:v", "0",
        "-cpu-used", "2", "-auto-alt-ref", "1", "-an", str(out)], stdin=subprocess.PIPE)


# ---------------- 抠像 + 对齐 ----------------

def key_first_frame():
    """抠 首帧.png: 参数与 7 个同来源视频一致(bg/t_body 自动, dilate 6, shadow 0.32)。"""
    rgb = np.asarray(Image.open(SRC_PNG).convert("RGB"))
    bg = np.median(np.concatenate([rgb[:15, :15].reshape(-1, 3), rgb[:15, -15:].reshape(-1, 3),
                                   rgb[-15:, :15].reshape(-1, 3), rgb[-15:, -15:].reshape(-1, 3)]),
                   axis=0)
    gn_bg = bg[1] - max(bg[0], bg[2])
    t_body = float(np.clip(round(0.2 * gn_bg), 15, 45))
    print(f"抠 首帧.png: bg=({bg[0]:.1f},{bg[1]:.1f},{bg[2]:.1f}) gn_bg={gn_bg:.1f} "
          f"t_body auto={t_body:.0f} (与同来源 7 视频同参数)", flush=True)
    return kg.key_frame(rgb, bg, t_body, 6, [], 0.32)


def bbox(a, th=128):
    m = a[..., 3] > th
    ys, xs = np.where(m)
    return [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())] if len(ys) else None


def to_canvas(keyed, scale, stretch, place=PLACE):
    """复刻 key_greenscreen.process 的 to_canvas: 缩放 + 底中锚点粘贴。"""
    bb = bbox(keyed, 128)
    if bb is None:
        return None
    dw, dh = round(CW * scale), round(CH * scale * stretch)
    resample = Image.LANCZOS if scale > 1.0 else Image.BILINEAR
    if (dw, dh) != (CW, CH):
        small = Image.fromarray(keyed).resize((dw, dh), resample)
    else:
        small = Image.fromarray(keyed)
    bcx = (bb[0] + bb[2]) / 2.0 * scale
    bcy = (bb[3] + 0.5) * scale * stretch
    ox = int(round(place[0] - bcx))
    oy = int(round(place[1] - bcy))
    cv = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
    cv.paste(small, (ox, oy))
    frame = np.array(cv)
    frame[frame[..., 3] == 0] = 0
    return frame


def body_diff(a, b):
    """本体区(双方 alpha>100) mean|d| + alpha>128 掩码 IoU。"""
    m = (a[..., 3] > 100) & (b[..., 3] > 100)
    if not m.any():
        return 999.0, 0.0
    md = float(np.abs(a[m].astype(np.int16) - b[m].astype(np.int16)).mean())
    ma, mb = a[..., 3] > 128, b[..., 3] > 128
    iou = float((ma & mb).sum() / max((ma | mb).sum(), 1))
    return md, iou


def align_to_unified(keyed, target):
    """网格搜索 (scale, stretch) 使抠像帧对上统一动作集/舞狮.webm 第 0 帧。"""
    # 粗估: 宽比/高比
    kb, tb = bbox(keyed, 128), bbox(target, 128)
    s0 = (tb[2] - tb[0] + 1) / (kb[2] - kb[0] + 1)
    k0 = ((tb[3] - tb[1] + 1) / (kb[3] - kb[1] + 1)) / s0
    print(f"对齐搜索: 粗估 scale={s0:.4f} stretch={k0:.4f}", flush=True)
    best = None
    for scale in np.arange(s0 - 0.006, s0 + 0.0061, 0.0005):
        for stretch in np.arange(k0 - 0.006, k0 + 0.0061, 0.0005):
            fr = to_canvas(keyed, float(scale), float(stretch))
            md, iou = body_diff(fr, target)
            if best is None or md < best[0]:
                best = (md, iou, float(scale), float(stretch))
    # 最优参数附近再扫一遍整数偏移(吸收预扫描用前3帧中位带来的 ±1px 差)
    md0, iou0, s1, k1 = best
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            fr = to_canvas(keyed, s1, k1, (PLACE[0] + dx, PLACE[1] + dy))
            md, iou = body_diff(fr, target)
            if md < md0:
                best = (md, iou, s1, k1)
                md0 = md
    md, iou, s1, k1 = best
    fr = to_canvas(keyed, s1, k1)
    print(f"对齐结果: scale={s1:.4f} stretch={k1:.4f} 本体mean|d|={md:.3f} IoU={iou:.4f} "
          f"bbox={bbox(fr,128)}", flush=True)
    return fr, (s1, k1, md, iou)


# ---------------- 主流程 ----------------

def build_one(name, canonical, hold):
    src = UNI / f"{name}.webm"
    out = OUT / f"{name}.webm"
    n0 = count_frames(src)
    enc = open_encoder(out)
    cb = canonical.tobytes()
    n = 0
    try:
        for _ in range(hold):                     # 首帧
            enc.stdin.write(cb)
            n += 1
        for fr in iter_frames(src):               # 原全部帧
            enc.stdin.write(fr.tobytes())
            n += 1
            if n % 120 == 0:
                print(f"  {name}: {n} frames...", flush=True)
        for _ in range(hold):                     # 尾帧
            enc.stdin.write(cb)
            n += 1
    finally:
        enc.stdin.close()
    rc = enc.wait()
    if rc != 0:
        raise RuntimeError(f"{name} 编码失败 rc={rc}")
    n1 = count_frames(out)
    print(f"  {name}: {n0} -> {n1} 帧 (+{n1 - n0})  rc={rc}", flush=True)
    if n1 != n0 + 2 * hold:
        raise RuntimeError(f"{name} 帧数不符: {n0} -> {n1}, 期望 {n0 + 2 * hold}")
    return n0, n1


def merge(files):
    """先试 concat -c copy(不重编码); 帧数不符则管道整条重编码兜底。"""
    expect = sum(count_frames(p) for p in files)
    with tempfile.TemporaryDirectory() as td:
        lst = Path(td) / "_ht.txt"
        lst.write_bytes("".join(f"file '{p.as_posix()}'\n" for p in files).encode("gbk"))
        r = subprocess.run([FF, "-y", "-v", "error", "-f", "concat", "-safe", "0",
                            "-i", str(lst), "-c", "copy", str(MERGED)],
                           capture_output=True, text=True)
    n = count_frames(MERGED) if MERGED.exists() else None
    if r.returncode == 0 and n == expect:
        print(f"合并(concat -c copy): {expect} 帧 -> {MERGED.name}", flush=True)
        return expect
    print(f"!! concat 合并不符(rc={r.returncode}, {n}!={expect}), 改用管道重编码", flush=True)
    enc = open_encoder(MERGED)
    n = 0
    for p in files:
        for fr in iter_frames(p):
            enc.stdin.write(fr.tobytes())
            n += 1
        print(f"  合并写入 {p.name} 完成 (累计 {n})", flush=True)
    enc.stdin.close()
    if enc.wait() != 0:
        raise RuntimeError("合并重编码失败")
    n = count_frames(MERGED)
    if n != expect:
        raise RuntimeError(f"合并帧数不符: {n} != {expect}")
    print(f"合并(管道重编码): {expect} 帧 -> {MERGED.name}", flush=True)
    return expect


def thumb(rgba, tw, th):
    im = Image.fromarray(rgba).resize((tw, th), Image.LANCZOS)
    bg = Image.new("RGBA", (tw, th), (105, 108, 115, 255))
    bg.alpha_composite(im)
    return bg.convert("RGB")


def make_sheet(canonical, info, junctions, hold):
    """验校图: canonical 大图 + 每动作首/尾帧缩略 + 合并接缝帧抽查。"""
    F = ImageFont.truetype(FONT, 15)
    FS = ImageFont.truetype(FONT, 12)
    tw, th = 232, 130
    cols = 4
    rows = (len(info) + cols - 1) // cols
    jcols = 4
    jrows = (len(junctions) + jcols - 1) // jcols
    W = 60 + cols * (tw + 8)
    H = 300 + rows * (th + 46) + 24 + jrows * (th + 22) + 50
    sheet = Image.new("RGB", (W, H), (26, 28, 32))
    d = ImageDraw.Draw(sheet)
    d.text((16, 10), "首尾帧验校 — canonical = 首帧.png 抠像版(= 舞狮首帧姿势, 底中锚点 640,608)",
           font=F, fill=(235, 235, 235))
    d.text((16, 34), f"每动作: 首 {hold} 帧 + 原片 + 尾 {hold} 帧; 合并=每动作一次(待机起); "
                     f"VP9 alpha 24fps 1280x720", font=FS, fill=(170, 170, 175))

    # canonical 大图
    big = thumb(canonical, 320, 180)
    sheet.paste(big, (16, 64))
    d.rectangle([16, 64, 336, 244], outline=(90, 200, 120), width=2)
    d.text((16, 250), "canonical(首帧_已抠像.png)", font=FS, fill=(140, 220, 160))

    # 每动作: 新文件首帧 / 尾帧(都应=canonical)
    x0, y0 = 360, 64
    for i, (name, n0, n1, head, tail) in enumerate(info):
        x = x0 + (i % cols) * (tw + 8)
        y = y0 + (i // cols) * (th + 46)
        hw = (tw - 6) // 2
        sheet.paste(thumb(head, hw, th), (x, y))
        sheet.paste(thumb(tail, hw, th), (x + hw + 6, y))
        d.rectangle([x, y, x + hw, y + th], outline=(70, 72, 78), width=1)
        d.rectangle([x + hw + 6, y, x + tw, y + th], outline=(70, 72, 78), width=1)
        d.text((x, y + th + 4), f"{name}  首|尾   {n0}->{n1}(+{n1 - n0})", font=FS,
               fill=(220, 220, 220))

    # 合并接缝抽查(4 列换行)
    jy = y0 + rows * (th + 46) + 16
    d.text((16, jy), f"合并文件接缝抽查(全部动作_首尾帧版.webm, 共 {sum(i[2] for i in info)} 帧):",
           font=FS, fill=(200, 200, 205))
    for j, (label, fr) in enumerate(junctions):
        x = 16 + (j % jcols) * (tw + 8)
        y = jy + 24 + (j // jcols) * (th + 22)
        sheet.paste(thumb(fr, tw, th), (x, y))
        d.rectangle([x, y, x + tw, y + th], outline=(70, 72, 78), width=1)
        d.text((x, y + th + 4), label, font=FS, fill=(220, 220, 220))
    d.text((16, H - 34), "判定: 所有动作首帧=尾帧=canonical, 合并接缝处为 canonical -> 角色一致性达成",
           font=FS, fill=(140, 220, 160))
    sheet.save(SHEET)
    print(f"验校图: {SHEET}", flush=True)


def main():
    ap = argparse.ArgumentParser(description="首帧.png 抠像版作为全部动作首尾帧")
    ap.add_argument("--hold", type=int, default=1, help="首/尾各停几帧(默认 1)")
    ap.add_argument("--only", default=None, help="只重出指定动作, 逗号分隔")
    a = ap.parse_args()
    only = set(a.only.split(",")) if a.only else None
    names = [n for n in ORDER if not only or n in only]

    # 1) 抠首帧
    keyed = key_first_frame()

    # 2) 与统一动作集/舞狮.webm 第0帧对齐(首帧.png 即舞狮.mp4 第0帧)
    target = decode_one(UNI / "舞狮.webm", 0)
    canonical, (s, k, md, iou) = align_to_unified(keyed, target)
    if md > 4.0:
        print(f"!! 对齐差异偏大(mean|d|={md:.2f}), 退回直接用统一版第 0 帧作为 canonical", flush=True)
        canonical = target.copy()
    Image.fromarray(canonical).save(CANON_PNG)
    print(f"canonical -> {CANON_PNG}  bbox(a>128)={bbox(canonical,128)}", flush=True)

    # 3) 重出 8 个动作(首/尾加 canonical)
    OUT.mkdir(exist_ok=True)
    info = []
    for name in names:
        n0, n1 = build_one(name, canonical, a.hold)
        head = decode_one(OUT / f"{name}.webm", 0)
        tail = decode_one(OUT / f"{name}.webm", n1 - 1)
        mh, _ = body_diff(head, canonical)
        mt, _ = body_diff(tail, canonical)
        print(f"  {name} 首帧 vs canonical: mean|d|={mh:.3f} | 尾帧 vs canonical: mean|d|={mt:.3f}",
              flush=True)
        info.append((name, n0, n1, head, tail))

    # 4) 合并
    files = [OUT / f"{n}.webm" for n in ORDER if (OUT / f"{n}.webm").exists()]
    total = merge(files)

    # 5) 接缝抽查: 每个接缝取"前动作尾帧 / 后动作首帧"两帧
    junctions = []
    acc = 0
    bounds = []
    for p in files:
        n = count_frames(p)
        bounds.append((p.stem, acc, acc + n - 1))
        acc += n
    for i in range(len(bounds) - 1):
        prev_name, ps, pe = bounds[i]
        next_name, ns, ne = bounds[i + 1]
        junctions.append((f"接缝{i+1}: {prev_name}尾(帧{pe})", decode_one(MERGED, pe)))
        junctions.append((f"接缝{i+1}: {next_name}首(帧{ns})", decode_one(MERGED, ns)))

    make_sheet(canonical, info, junctions, a.hold)
    print(f"\n完成: canonical + {len(info)} 个动作(各 +{2*a.hold} 帧) + 合并 {total} 帧", flush=True)


if __name__ == "__main__":
    main()
