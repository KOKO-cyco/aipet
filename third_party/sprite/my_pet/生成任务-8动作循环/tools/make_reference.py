# -*- coding: utf-8 -*-
"""生成豆包图生视频用的「首尾帧参考图」（绿幕规范版）。

链路（对应 docs/ASSET-PIPELINE.md §二 的素材生成链）:
    源 webm（VP9 alpha，动作视频优化版/待机呼吸休闲.webm）
      -> ffmpeg 强制 libvpx-vp9 解码抽第 0 帧（原生解码器会静默丢 alpha）
      -> 按绿幕规范定位合成到纯 #00FF00 画布

规范（docs/ASSET-PIPELINE.md §①，硬性）:
    - 画面 16:9；背景纯绿幕色 #00FF00，无阴影、杂物、渐变、边框
    - 头顶约画幅垂直 20%，脚底约 85%；左右边缘约 25% / 75%
    - 第一帧 = 干净的标准正面站立（本图同时用作每个动作的尾帧：
      每个动作结束都恢复这一初始状态，首尾帧闭环）

输出（写到本脚本所在目录）:
    首尾帧参考图_绿幕_1920x1080.png   豆包首帧/尾帧上传用
    角色_透明底_640x360.png           原生分辨率抠像（备用/管线对齐用）

用法:
    python make_reference.py [--source webm|ico]

    --source webm（默认）: 从「动作视频优化版/待机呼吸休闲.webm」抽第 0 帧。
        与库内现有 98 段素材同渲染同色彩，无色差——首选。
    --source ico: 用 assets/icon.ico 的 256x256 入口。兜底选项，仅当没有
        可用的视频源时使用。实测短板：① 容器上限 256x256，需放大 ~2.8x；
        ② 与视频源存在平均 29/255 的色差（另一次渲染），与库内旧素材并排
        播放会有可感知偏色；姿势本身一致（alpha 掩码 IoU 0.96）。
        清晰度指标反而略高于视频源（图标入口是无损 PNG，未经 VP9 有损压缩）。
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]  # .workbuddy/my_pet/生成任务-8动作循环/tools -> 仓库根
SRC_WEBM = REPO / "assets" / "characters" / "shenshen" / "动作视频优化版" / "待机呼吸休闲.webm"
SRC_ICO = REPO / "assets" / "icon.ico"
FFMPEG = next(
    (p for p in [
        Path(sys.executable).parent / "Lib" / "site-packages" / "imageio_ffmpeg" / "binaries",
    ] if p.is_dir()),
    None,
)

GREEN = (0, 255, 0)
CANVAS_W, CANVAS_H = 1920, 1080   # 16:9，豆包图生视频输入
HEAD_RATIO = 0.20                  # 头顶 = 画幅垂直 20%
FEET_RATIO = 0.85                  # 脚底 = 画幅垂直 85%
SIDE_LO, SIDE_HI = 0.25, 0.75      # 左右边缘约 25% / 75%


def find_ffmpeg() -> str:
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception as exc:  # noqa: BLE001
        raise SystemExit(f"找不到 ffmpeg（需要 imageio-ffmpeg）: {exc}")


def extract_frame0(webm: Path, out_png: Path) -> None:
    # 必须 -c:v libvpx-vp9：原生 vp9 解码器会静默丢弃 VP9 alpha 位流
    cmd = [
        find_ffmpeg(), "-hide_banner", "-loglevel", "error",
        "-c:v", "libvpx-vp9", "-i", str(webm),
        "-pix_fmt", "rgba", "-frames:v", "1", "-y", str(out_png),
    ]
    subprocess.run(cmd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="生成绿幕首尾帧参考图")
    parser.add_argument("--source", choices=["webm", "ico"], default="webm",
                        help="角色底图来源：webm=视频源第0帧（默认，无色差）；"
                             "ico=assets/icon.ico 256px 入口（兜底，有 ~29/255 色差）")
    args = parser.parse_args()

    if args.source == "webm":
        if not SRC_WEBM.is_file():
            raise SystemExit(f"源视频不存在: {SRC_WEBM}")
        with tempfile.TemporaryDirectory() as td:
            frame0 = Path(td) / "frame0.png"
            extract_frame0(SRC_WEBM, frame0)
            im = Image.open(frame0).convert("RGBA")
    else:
        if not SRC_ICO.is_file():
            raise SystemExit(f"ico 不存在: {SRC_ICO}")
        ico = Image.open(SRC_ICO)
        ico.seek(0)  # 首帧即最大入口（256x256）
        im = ico.convert("RGBA")

    a = np.array(im)
    alpha = a[..., 3]
    ys, xs = np.where(alpha > 10)
    if not len(xs):
        raise SystemExit("源图没有有效角色像素（alpha 全透明）")
    x0, x1, y0, y1 = int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max())
    char_w, char_h = x1 - x0 + 1, y1 - y0 + 1
    cut = im.crop((x0, y0, x1 + 1, y1 + 1))
    suffix = "_ico源" if args.source == "ico" else ""
    cut.save(HERE / f"角色_透明底_640x360{suffix}.png")

    # 规范定位：头顶 20% / 脚底 85% => 角色高 = 65% 画幅高；水平居中
    target_h = round((FEET_RATIO - HEAD_RATIO) * CANVAS_H)
    scale = target_h / char_h
    target_w = round(char_w * scale)
    cut_big = cut.resize((target_w, target_h), Image.LANCZOS)

    canvas = Image.new("RGBA", (CANVAS_W, CANVAS_H), GREEN + (255,))
    px = (CANVAS_W - target_w) // 2
    py = round(HEAD_RATIO * CANVAS_H)
    canvas.alpha_composite(cut_big, (px, py))
    out = HERE / f"首尾帧参考图_绿幕_1920x1080{suffix}.png"
    canvas.convert("RGB").save(out)

    # 自检：把规范比例打出来，人工核对
    print(f"角色原生 bbox: x {x0}-{x1}  y {y0}-{y1}  ({char_w}x{char_h})")
    print(f"缩放: x{scale:.3f} -> {target_w}x{target_h}")
    print(f"头顶 y = {py} ({py / CANVAS_H:.1%}, 规范 {HEAD_RATIO:.0%})")
    print(f"脚底 y = {py + target_h} ({(py + target_h) / CANVAS_H:.1%}, 规范 {FEET_RATIO:.0%})")
    print(f"左缘 x = {px} ({px / CANVAS_W:.1%}), 右缘 x = {px + target_w} "
          f"({(px + target_w) / CANVAS_W:.1%}, 规范带 {SIDE_LO:.0%}-{SIDE_HI:.0%})")
    bg = np.array(canvas.convert("RGB"))
    greenish = ((bg[..., 1] > 200) & (bg[..., 0] < 60) & (bg[..., 2] < 60)).mean()
    print(f"绿幕像素占比: {greenish:.1%}")
    print(f"输出: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
