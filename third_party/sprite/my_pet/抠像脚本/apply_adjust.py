#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
apply_adjust.py — 按 adjust_gui.py 调好的参数渲染最终 webm

用法:
    python apply_adjust.py --params 豆包/调优参数.json [--out 豆包/最终输出] [--only 待机] [--no-merge]

流程:
  1. 读 调优参数.json（GUI 里肉眼调的 中心X/脚底Y/宽/高 + 角色 bbox）
  2. 换算成 key_greenscreen.py 的 --scale/--stretch/--place（--prekeyed 只缩放+锚点）
  3. 逐动作渲染到输出目录
  4. 合并成 豆包/全部动作_调优版.webm（流式 rawvideo rgba -> VP9 alpha，与主脚本同参数）

形变提示: 每个动作打印 缩放x/缩放y/形变%，非 0 即有形变（GUI 解锁长宽比才会出现）。
"""
import sys
import json
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from adjust_gui import ff, params_to_render, body_bbox, grab_first_frame  # noqa: E402

ROOT = HERE.parent


def find_render_python():
    """找带 imageio_ffmpeg 的 python 跑 key_greenscreen.py（managed venv 优先）。"""
    cands = [Path.home() / ".workbuddy/binaries/python/envs/default/Scripts/python.exe",
             Path(sys.executable)]
    for c in cands:
        try:
            r = subprocess.run([str(c), "-c", "import imageio_ffmpeg"],
                               capture_output=True, timeout=30)
            if r.returncode == 0:
                return str(c)
        except Exception:
            continue
    return sys.executable


def merge(outputs, out: Path, fps=24):
    enc = subprocess.Popen(
        [ff().exe, "-y", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", "1280x720",
         "-r", str(fps), "-i", "-", "-c:v", "libvpx-vp9", "-pix_fmt", "yuva420p",
         "-crf", "28", "-b:v", "0", "-cpu-used", "2", "-auto-alt-ref", "1", "-an", str(out)],
        stdin=subprocess.PIPE)
    total = 0
    for w in outputs:
        dec = subprocess.Popen(
            [ff().exe, "-v", "error", "-c:v", "libvpx-vp9", "-i", str(w),
             "-f", "rawvideo", "-pix_fmt", "rgba", "-"], stdout=subprocess.PIPE)
        n = 0
        while True:
            chunk = dec.stdout.read(1 << 20)
            if not chunk:
                break
            enc.stdin.write(chunk)
            n += len(chunk)
        dec.stdout.close()
        dec.wait()
        total += n // (1280 * 720 * 4)
        print(f"  + {w.name}: {n // (1280 * 720 * 4)} 帧", flush=True)
    enc.stdin.close()
    enc.wait()
    print(f"合并 -> {out}  ({total} 帧, {total / fps:.1f}s)")
    return out


def main():
    args = sys.argv[1:]
    params_p = Path(args[args.index("--params") + 1]) if "--params" in args \
        else ROOT / "豆包" / "调优参数.json"
    out_dir = Path(args[args.index("--out") + 1]) if "--out" in args \
        else ROOT / "豆包" / "最终输出"
    only = args[args.index("--only") + 1] if "--only" in args else None
    do_merge = "--no-merge" not in args

    data = json.loads(params_p.read_text(encoding="utf-8"))
    actions, meta = data["actions"], data.get("meta", {})
    names = [n for n in actions if only in (None, n)]
    out_dir.mkdir(parents=True, exist_ok=True)
    py = find_render_python()
    print(f"渲染 python: {py}\n输出目录: {out_dir}\n")

    outputs = []
    for name in names:
        p, m = actions[name], meta.get(name, {})
        src = Path(m.get("src") or str(ROOT / "豆包" / "统一动作集" / f"{name}.webm"))
        if not src.exists():
            print(f"!! {name}: 源不存在 {src}，跳过")
            continue
        bbox = m.get("bbox")
        if bbox is None:                       # meta 缺失时重测
            tmp = out_dir / f"_probe_{name}.png"
            grab_first_frame(src, tmp)
            bbox = body_bbox(tmp)
            tmp.unlink(missing_ok=True)
        rd = params_to_render(p, bbox)
        out = out_dir / f"{name}.webm"
        cmd = [py, str(HERE / "key_greenscreen.py"), str(src), str(out),
               "--prekeyed", "--scale", f"{rd['scale']:.4f}",
               "--stretch", f"{rd['stretch']:.4f}",
               "--place", f"{rd['place'][0]:.0f},{rd['place'][1]:.0f}"]
        print(f"=== {name}: scale={rd['scale']:.4f} stretch={rd['stretch']:.4f} "
              f"place=({rd['place'][0]:.0f},{rd['place'][1]:.0f}) "
              f"形变={(rd['stretch'] - 1) * 100:+.1f}% ===", flush=True)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not out.exists():
            print(f"!! {name} 渲染失败:\n{(r.stderr or r.stdout)[-600:]}")
            continue
        outputs.append(out)
        print(f"  -> {out.name}  OK", flush=True)

    if do_merge and outputs:
        print("\n合并 ...", flush=True)
        merge(outputs, ROOT / "豆包" / "全部动作_调优版.webm")
    print("\nDONE")


if __name__ == "__main__":
    main()
