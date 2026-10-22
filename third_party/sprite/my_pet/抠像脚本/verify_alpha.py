#!/usr/bin/env python
"""验证绿幕抠像输出: 黑洞(闪烁块)/边缘绿晕/覆盖物残留/透明通道有效性。

用法:
  python verify_alpha.py <output.webm> [--frames 5] [--wm x0,y0,x1,y1 ...]
      [--save-dir ./verify_out] [--no-images]

指标解读:
  holes       本体空洞数(alpha=0 且 8 邻域全不透明) —— 必须为 0, 否则白/黑像素被误抠, 会闪烁
  greenish    边缘环带(alpha 20~235) G-max(R,B)>3 的像素占比 —— 中高 alpha 区应 <0.5%
  tint        上述绿色像素合成到白/黑底的可见色偏 —— p99<=10 / max<=15 可接受
  overlay     覆盖物矩形内 max alpha —— 必须为 0
依赖: pip install imageio-ffmpeg numpy pillow
"""
import argparse
import subprocess
from pathlib import Path

import numpy as np


def get_ffmpeg():
    import imageio_ffmpeg
    return imageio_ffmpeg.get_ffmpeg_exe()


def decode_frame_pil(webm, idx, tmp_png):
    """解码第 idx 帧为 RGBA（ffmpeg 出 PNG, PIL 原生读 alpha）。tmp_png: 临时 png 路径。"""
    ff = get_ffmpeg()
    cmd = [ff, "-y", "-c:v", "libvpx-vp9", "-i", str(webm),   # 注意: -c:v 必须在 -i 前!
           "-vf", f"select=eq(n\\,{idx})", "-frames:v", "1",
           "-c:v", "png", str(tmp_png)]
    subprocess.run(cmd, capture_output=True)
    if not tmp_png.exists():
        return None
    from PIL import Image
    rgba = np.asarray(Image.open(tmp_png).convert("RGBA"))
    tmp_png.unlink(missing_ok=True)
    return rgba


def surround_opaque(alpha):
    """alpha=0 且 8 邻域全不透明 => 本体空洞"""
    op = (alpha > 128).astype(np.float32)
    p = np.pad(op, 1, mode="edge")
    h, w = op.shape
    s = np.zeros_like(op)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            s += p[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
    return (alpha < 8) & (s >= 8)


def count_frames(path):
    """帧数: 先试 imageio 元数据, 失败则解码数数(慢但可靠)。"""
    import imageio.v3 as iio
    try:
        p = iio.improps(path, plugin="FFMPEG")
        if p.shape and p.shape[0]:
            return int(p.shape[0])
    except Exception:
        pass
    n = 0
    for _ in iio.imiter(path, plugin="FFMPEG"):
        n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("webm")
    ap.add_argument("--frames", type=int, default=5)
    ap.add_argument("--wm", action="append", default=[], help="覆盖物矩形 x0,y0,x1,y1 (画布坐标系)")
    ap.add_argument("--save-dir", default=None)
    ap.add_argument("--no-images", action="store_true")
    a = ap.parse_args()

    webm = Path(a.webm)
    total = count_frames(webm)
    if total <= 0:
        print("无法读取帧数, 请检查文件"); return
    idxs = [int(total * (i + 0.5) / a.frames) for i in range(a.frames)]
    print(f"{webm.name}: {total} frames, 抽样 {idxs}")

    wm_rects = [tuple(int(x) for x in s.split(",")) for s in a.wm]
    save = Path(a.save_dir) if a.save_dir else None
    if save and not a.no_images:
        save.mkdir(parents=True, exist_ok=True)

    ok = True
    for k, idx in enumerate(idxs):
        tmp_png = (save / f"_tmp_{k}.png") if save else Path("_tmp_v.png")
        rgba = decode_frame_pil(webm, idx, tmp_png)
        if rgba is None:
            print(f"  f{idx}: DECODE FAILED"); ok = False; continue
        alpha = rgba[..., 3]
        rgb = rgba[..., :3].astype(np.int16)
        corners = (int(alpha[0, 0]), int(alpha[0, -1]), int(alpha[-1, 0]), int(alpha[-1, -1]))
        holes = int(surround_opaque(alpha).sum())
        ring = (alpha >= 20) & (alpha <= 235)
        grn = rgb[..., 1] - np.maximum(rgb[..., 0], rgb[..., 2])
        g = grn[ring]
        if len(g):
            gp = f"greenish>3: {(g > 3).sum()} ({(g > 3).mean() * 100:.1f}%) p99={np.percentile(g, 99):.1f}"
        else:
            gp = "no edge px"
        omax = max((int(alpha[y0:y1, x0:x1].max()) for (x0, y0, x1, y1) in wm_rects), default=0)
        core_min = int(alpha[alpha > 0].min()) if (alpha > 0).any() else 0
        print(f"  f{idx}: corners={corners} holes={holes} overlay_max={omax} | {gp}")
        if holes > 0 or omax > 0 or max(corners) > 0:
            ok = False
        # 合成色偏 + 目视图
        if save and not a.no_images and k == len(idxs) // 2:
            af = alpha[..., None].astype(np.float32) / 255.0
            rgbf = rgba[..., :3].astype(np.float32)
            from PIL import Image
            for label, bgv in (("red", (255, 0, 0)), ("white", (255, 255, 255)),
                               ("gray", (128, 128, 128)), ("black", (0, 0, 0))):
                bg = np.full_like(rgbf, 0.0)
                bg[..., 0], bg[..., 1], bg[..., 2] = bgv
                comp = (rgbf * af + bg * (1 - af)).astype(np.uint8)
                Image.fromarray(comp).save(save / f"{webm.stem}_{label}_f{idx}.png")
            # 绿色 fringe 像素的实际合成色偏
            ringg = ring & (grn > 3)
            if ringg.any():
                for label, bgv in (("white", 255.0), ("black", 0.0)):
                    comp = rgbf * af + bgv * (1 - af)
                    cg = comp[..., 1] - np.maximum(comp[..., 0], comp[..., 2])
                    v = cg[ringg]
                    print(f"    tint over {label}: p99={np.percentile(v, 99):.1f} max={v.max():.1f} (<=10 可接受)")
    print("RESULT:", "PASS" if ok else "FAIL(见上行标注)")


if __name__ == "__main__":
    main()
