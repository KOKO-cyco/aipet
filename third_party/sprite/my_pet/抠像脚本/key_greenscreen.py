#!/usr/bin/env python
"""绿幕视频抠像 v3: 分区域硬色键 + 边缘颜色借用(去粉紫边) + 缩放/锚点统一, 输出 WebM(VP9 alpha)。

适用场景(2026-10-15 桌宠实战方案):
  - 纯色绿幕素材, 角色与背景/阴影/水印颜色完全不重叠
  - 角色本体含白/黑像素(r≈g≈b, gn≈0), 单阈值 gn<0 会把它们切成黑洞 -> 分区域阈值
  - 边缘半透明像素是 "角色色+绿幕" 混合色: 自行反推色相会把 G 减成负值截断,
    G<<R,B 出品红/粉紫边(2026-10-17 诊断) -> 改为颜色借用: 半透明像素直接取
    最近不透明本体像素的颜色, 软边手感由 alpha 决定, 不受影响
  - 多动作统一: --scale 统一角色大小, --place 把角色底中对齐到固定锚点

用法:
  python key_greenscreen.py <input.mp4|input.webm> <output.webm> [选项]

选项:
  --wm x0,y0,x1,y1   覆盖物矩形(源坐标系, 可重复): 水印/数字, 直接置零
  --bg r,g,b         背景绿 RGB; 缺省自动测量(四角 15x15 多帧中位)
  --t-body           本体区保留阈值; 缺省按背景饱和度自动标定 ≈0.2*gn_bg(15~45)
  --dilate 6         本体区 = gn<0 核心膨胀半径
  --canvas 1280x720  输出画布
  --scale S          源->画布缩放; 缺省等比适配(竖屏 720x960 -> 0.75)
  --stretch K        垂直修正系数(乘在 scale 上): 多动作统一"长宽比"用。
                     K = 参考长宽比/本片长宽比, 总高/躯干宽 与参考一致即 K=1
  --place cx,cy      角色 bbox 底中对齐到画布(cx, cy)。自动抽样(前3帧)测角色 bbox,
                     全视频用同一偏移(不逐帧重锚, 保留动作自身节奏, 防抽动)
  --tail N           结尾加 N 帧"交叉溶解"过渡: 把本动作最后 N 帧逐帧溶到目标首帧,
                     尾帧末端与目标逐像素一致 -> 播放器切到目标动作时无跳变
  --tail-target F    过渡目标: .png(画布尺寸 RGBA) 或 .webm(取第 0 帧)
  --tail-self        过渡目标 = 本视频自身的第 0 帧(待机自循环接缝平滑用)
  --prekeyed         输入已是带 alpha 的视频(.webm): 跳过抠像, 只做缩放/锚点/编码
  --shadow 0.32      脚底阴影保留为半透明的 alpha(0=完全抠掉阴影)。
                     豆包绿幕素材脚底有淡影, 阴影/水印的色键距离都 < T_body 会被当背景,
                     此选项把"本体下方 36px 内、暗于背景"的像素救回为半透明影;
                     水印(dist 更小且不暗)仍自动抠掉
  --resample MODE    缩放滤波器 auto|bilinear|lanczos。
                     auto(默认): 放大用 LANCZOS, 缩小用 BILINEAR(旧行为)。
                     缩小到 640x360 时 BILINEAR 会明显发虚(实测梯度能量 -30%),
                     要成品锐利用 --resample lanczos(预乘 alpha 缩放, 无黑边)。
  --curve "r5,r50,r95,g5,g50,g95,b5,b50,b95"
                     颜色对齐: 目标素材本体(alpha>=230)的 p5/p50/p95 逐通道分位数,
                     9 个数。脚本自动抽样测本片对应分位数, 建逐通道分段线性 LUT
                     在编码前套上去 —— 一次编码, 不二次转码。
                     豆包不同批次源片曝光不一样(实测 luma 中位 79 vs 92), 用同一个
                     --curve 就能把多个动作拉到同一基准。缺省不校准。
  --fps              输出帧率(默认取源帧率)
  --mov              额外输出 MOV(qtrle) 供 AE/FCP
  --crf 28           VP9 质量

依赖: pip install imageio imageio-ffmpeg numpy pillow
注意: 验证输出时 ffmpeg 解码必须 `-c:v libvpx-vp9` 放在 `-i` 前, 否则 alpha 静默变全 255。
"""
import argparse
import re
import subprocess
import sys

import numpy as np


# ---------- 形态学 / 模糊（无 scipy，numpy pad+移位）----------

def erode(m, n=1):
    for _ in range(n):
        p = np.pad(m, 1, constant_values=False)
        out = np.ones_like(m)
        h, w = m.shape
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                out &= p[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
        m = out
    return m


def dilate(m, n=1):
    for _ in range(n):
        p = np.pad(m, 1, constant_values=False)
        out = np.zeros_like(m)
        h, w = m.shape
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                out |= p[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
        m = out
    return m


def boxblur(f, n=2):
    for _ in range(n):
        p = np.pad(f, 1, mode="edge")
        s = np.zeros_like(f)
        h, w = f.shape
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                s += p[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
        f = s / 9.0
    return f


# ---------- 参数常量（实战调优值，改天换素材先跑 verify_alpha.py 再决定要不要动）----------
T_BODY = None      # 本体区阈值: None=按背景饱和度自动标定 ≈0.2*gn_bg (裁剪 15~45);
                   # 原理: 阈值边界对应的"真实不透明度"应≈0.8, 与背景深浅无关。
                   #   混合像素 gn ≈ (1-α)*gn_bg  →  α=0.8  ⟹  T = 0.2*gn_bg
                   # 纯绿幕 gn_bg≈218 → T=44; 低饱和绿(98,186,71) gn_bg=88 → T=18。
                   # T 偏大会把半混合像素判全不透明→绿晕(反推救不回); 偏小则边缘发虚。
                   # 洞像素(白围裙/黑相机 gn 0~15)必须被 T 覆盖; 手动覆盖用 --t-body
ERODE = 1          # 去孤立毛刺
BLUR_PASS = 2      # 软边: 3x3 box blur x2
GAIN = 1.15        # blur 衰减补偿
A_FULL = 0.85      # alpha>=此值用原图色; 以下的半透明像素借用本体色(bleed_color 外播)


# ---------- 单帧抠像（预扫描和主循环共用，保证 bbox 测量与实际输出一致）----------

SHADOW_PX = 36      # 脚底阴影带宽度: 本体最低行以下这么多像素内算阴影候选


def add_shadow(rgb, bg, alpha_f, shadow_alpha):
    """保留脚底阴影为半透明: 本体每列最低行下方 SHADOW_PX 内、暗于背景、且非本体的像素。
    阴影跟随角色逐帧重算(角色跳起时脚下无暗区 -> 不加, 不会糊出假影)。"""
    bodym = alpha_f > 0.5
    H, W = alpha_f.shape
    has = bodym.any(axis=0)
    low = np.where(has, H - bodym[::-1].argmax(axis=0) - 1, -1)   # 每列本体最低行
    yy = np.arange(H)[:, None]
    band = (yy > low[None, :]) & (yy <= low[None, :] + SHADOW_PX) & (low[None, :] >= 0)
    lum = rgb.astype(np.float32).mean(axis=2)
    darker = lum < float(np.asarray(bg).mean()) - 4.0
    sh = band & darker & (alpha_f < 0.5)
    if not sh.any():
        return alpha_f
    shf = boxblur(sh.astype(np.float32), 1)
    return np.maximum(alpha_f, np.clip(shf * shadow_alpha, 0.0, 1.0))


def bleed_color(rgb, known, iters=6, ds=4):
    """颜色借用: 把 known 区(不透明本体)的颜色向外迭代外播(3x3 均值),
    半透明边缘像素直接取最近本体色, 不再自行反推色相。

    为什么替掉颜色反推(unmix=(obs-(1-a)*B)/a): 反推假设 像素=a*前景+(1-a)*背景,
    高饱和绿幕(gn_bg≈219)下边缘像素 (1-a)*bg 的 G 项可达 150+, 把 G 减成负值
    截断为 0, 而 R/B 无背景贡献可减 -> G<<R,B = 品红/粉紫边
    (2026-10-17 诊断: 首帧_已抠像 8% 的半透明边缘像素中招)。
    借用本体色后 fringe 色相=本体色, 软边手感不变。

    降采样 1/ds 运算后双线性放大: 色场平滑, 取色不需要像素级精度, 速度快 ~16 倍。
    """
    from PIL import Image
    h, w = known.shape
    lh, lw = max(1, round(h / ds)), max(1, round(w / ds))
    kn = np.asarray(Image.fromarray((known * 255).astype(np.uint8))
                    .resize((lw, lh), Image.BILINEAR)) > 127
    col = np.asarray(Image.fromarray(rgb.astype(np.uint8))
                     .resize((lw, lh), Image.BILINEAR), np.float32)
    for _ in range(iters):
        pk = np.pad(kn, 1, constant_values=False)
        pc = np.pad(col, ((1, 1), (1, 1), (0, 0)), mode="edge")
        cnt = np.zeros((lh, lw), np.float32)
        acc = np.zeros((lh, lw, 3), np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                m = pk[1 + dy:1 + dy + lh, 1 + dx:1 + dx + lw]
                cnt += m
                acc += m[..., None] * pc[1 + dy:1 + dy + lh, 1 + dx:1 + dx + lw]
        new = (cnt > 0) & ~kn
        if not new.any():
            break
        col[new] = acc[new] / cnt[new, None]
        kn |= new
    return np.asarray(Image.fromarray(np.clip(col, 0, 255).astype(np.uint8))
                      .resize((w, h), Image.BILINEAR), np.float32)


def key_frame(rgb, bg, t_body, dilate_r, wm_rects, shadow=0.0):
    """输入源分辨率 RGB(uint8), 输出源分辨率 RGBA(uint8)。"""
    im = rgb.astype(np.int16)
    gn = np.minimum(im[..., 1] - im[..., 0], im[..., 1] - im[..., 2])

    # 1) 分区域 alpha: 本体区宽松阈值救白/黑像素, 本体外仍 gn<0
    core = gn < 0
    body = dilate(core, dilate_r)
    sel = ((gn < t_body) & body) | core
    sel = erode(sel, ERODE)
    alpha_f = np.clip(boxblur(sel.astype(np.float32), BLUR_PASS) * GAIN, 0.0, 1.0)

    # 1.5) 脚底阴影保留为半透明(水印/背景渐变 dist<T_body 仍被当背景抠掉)
    if shadow > 0:
        alpha_f = add_shadow(rgb, bg, alpha_f, shadow)

    # 2) 边缘颜色借用: 半透明像素不自行反推色相(会把 G 减负截断 -> 品红/粉紫边),
    #    直接取最近不透明本体像素的颜色; 软边手感由 alpha 决定, 不受影响
    borrowed = bleed_color(rgb, alpha_f >= A_FULL)
    F = np.where((alpha_f < A_FULL)[..., None], borrowed, rgb.astype(np.float32))
    F = np.clip(F, 0, 255)
    F[..., 1] = np.minimum(F[..., 1], np.maximum(F[..., 0], F[..., 2]) + 8)  # despill 兜底(只压绿)

    rgba = np.dstack([F.astype(np.uint8), (alpha_f * 255.0 + 0.5).astype(np.uint8)])

    # 3) 覆盖物矩形置零(水印/数字, 源坐标系) + 透明区 RGB 清零
    for (x0, y0, x1, y1) in wm_rects:
        rgba[y0:y1, x0:x1] = 0
    rgba[rgba[..., 3] == 0] = 0
    return rgba


# ---------- 探测 / 背景测量 / bbox 预扫描 ----------

def measure_bg(src, step=30, maxf=8):
    """背景绿: 四角 15x15 中位, 多帧取中位。"""
    import imageio.v3 as iio
    cols = []
    for i, fr in enumerate(iio.imiter(src, plugin="FFMPEG")):
        if i % step:
            continue
        im = fr[..., :3]
        h, w = im.shape[:2]
        c = np.concatenate([im[:15, :15].reshape(-1, 3), im[:15, -15:].reshape(-1, 3),
                            im[-15:, :15].reshape(-1, 3), im[-15:, -15:].reshape(-1, 3)])
        cols.append(np.median(c, axis=0))
        if len(cols) >= maxf:
            break
    return np.median(np.array(cols), axis=0)


def probe(src):
    """返回 (w, h, fps)。imageio-ffmpeg 只带 ffmpeg 不带 ffprobe, 用 imageio 元数据接口。"""
    import imageio.v3 as iio
    m = iio.immeta(src, plugin="FFMPEG")
    w, h = m["size"]
    return int(w), int(h), float(m["fps"])


def decode_png_frame(ff, src, t, prekeyed, tmp):
    """按时间点抽 1 帧存 PNG 再读回(带不带 alpha 都能读)。"""
    from PIL import Image
    cmd = [ff, "-y"]
    if prekeyed:
        cmd += ["-c:v", "libvpx-vp9"]      # 必须在 -i 前, 否则 alpha 静默丢失
    cmd += ["-ss", f"{t:.3f}", "-i", str(src), "-frames:v", "1", str(tmp)]
    subprocess.run(cmd, capture_output=True)
    if not tmp.exists() or tmp.stat().st_size < 1000:
        return None
    im = Image.open(tmp)
    arr = np.frombuffer(im.tobytes(), np.uint8)
    w, h = im.size
    im.close()
    tmp.unlink()
    return arr.reshape(h, w, -1)


def measure_char_bbox(ff, src, prekeyed, key_fn, tmpdir):
    """抽样前 3 帧测角色 bbox(alpha>128), 返回中位 (x0,y0,x1,y1) 源坐标系。
    用前 3 帧(基础姿势): 与 unify_actions 的目标值同口径, 且切换动作时从第 0 帧对齐。"""
    from PIL import Image
    boxes = []
    for k, idx in enumerate((0, 1, 2)):
        p = tmpdir / f"_bb_{k}.png"
        cmd = [ff, "-y"]
        if prekeyed:
            cmd += ["-c:v", "libvpx-vp9"]      # 必须在 -i 前, 否则 alpha 静默丢失
        cmd += ["-i", str(src), "-vf", f"select=eq(n\\,{idx})", "-frames:v", "1", str(p)]
        subprocess.run(cmd, capture_output=True)
        if not p.exists() or p.stat().st_size < 1000:
            continue
        im = Image.open(p)
        arr = np.frombuffer(im.tobytes(), np.uint8)
        w, h = im.size
        im.close()
        p.unlink()
        arr = arr.reshape(h, w, -1)
        if prekeyed:
            a = arr[..., 3]
        else:
            a = key_fn(arr[..., :3])[..., 3]
        m = a > 128
        ys, xs = np.where(m)
        if len(ys):
            boxes.append((xs.min(), ys.min(), xs.max(), ys.max()))
    if not boxes:
        return None
    b = np.array(boxes, dtype=np.float64)
    return tuple(int(np.median(b[:, i])) for i in range(4))


# ---------- 尾帧用: 帧计数 / 精确帧号解码 / 画布目标帧加载 ----------

def count_frames(ff, src):
    """精确帧数: 解一遍到 null, 解析 stderr 的 frame= 计数。"""
    r = subprocess.run([ff, "-v", "info", "-i", str(src), "-f", "null", "-"],
                       capture_output=True, text=True)
    m = re.findall(r"frame=\s*(\d+)", r.stderr)
    return int(m[-1]) if m else None


def decode_src_frame(ff, src, prekeyed, idx):
    """按帧号解 1 帧(源分辨率 RGBA/RGB)。"""
    import tempfile
    from pathlib import Path
    from PIL import Image
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td) / "f.png"
        cmd = [ff, "-y"]
        if prekeyed:
            cmd += ["-c:v", "libvpx-vp9"]      # 必须在 -i 前, 否则 alpha 静默丢失
        cmd += ["-i", str(src), "-vf", f"select=eq(n\\,{idx})", "-frames:v", "1", str(tmp)]
        subprocess.run(cmd, capture_output=True)
        if not tmp.exists() or tmp.stat().st_size < 1000:
            return None
        im = Image.open(tmp)
        arr = np.frombuffer(im.tobytes(), np.uint8)
        w, h = im.size
        im.close()
    return arr.reshape(h, w, -1)


def decode_src_indices(ff, src, prekeyed, idxs, size):
    """一次解多帧(精确帧号 select), 管道 rawvideo rgba 读回; 解不够返回已有。"""
    w, h = size
    expr = "+".join(f"eq(n\\,{i})" for i in idxs)
    cmd = [ff, "-v", "error"]
    if prekeyed:
        cmd += ["-c:v", "libvpx-vp9"]          # 必须在 -i 前, 否则 alpha 静默丢失
    cmd += ["-i", str(src), "-vf", f"select={expr}", "-frames:v", str(len(idxs)),
            "-f", "rawvideo", "-pix_fmt", "rgba", "-"]
    dec = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    nbytes = w * h * 4
    out = []
    for _ in idxs:
        buf = b""
        while len(buf) < nbytes:
            chunk = dec.stdout.read(nbytes - len(buf))
            if not chunk:
                break
            buf += chunk
        if len(buf) < nbytes:
            break
        out.append(np.frombuffer(buf, np.uint8).reshape(h, w, 4))
    dec.stdout.close()
    dec.wait()
    return out


def load_canvas_frame(ff, path, canvas):
    """加载画布坐标的目标帧: .png 直接读, .webm 解第 0 帧(保 alpha)。"""
    from pathlib import Path
    from PIL import Image
    p = Path(path)
    if p.suffix.lower() == ".png":
        im = Image.open(p).convert("RGBA")
        if im.size != canvas:
            print(f"!! 尾帧目标尺寸 {im.size} != 画布 {canvas}", flush=True)
            return None
        return np.array(im)
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td) / "t.png"
        subprocess.run([ff, "-y", "-c:v", "libvpx-vp9", "-i", str(p),
                        "-frames:v", "1", str(tmp)], capture_output=True)
        if not tmp.exists() or tmp.stat().st_size < 1000:
            return None
        im = Image.open(tmp)
        arr = np.frombuffer(im.tobytes(), np.uint8)
        w, h = im.size
        im.close()
    if (w, h) != canvas:
        print(f"!! 尾帧目标尺寸 {(w, h)} != 画布 {canvas}", flush=True)
        return None
    return arr.reshape(h, w, 4)


# ---------- 颜色对齐(跨动作曝光统一) ----------

def measure_source_pct(ff, src, key_fn, prekeyed, n=9):
    """抽样 n 帧, 抠像后取 alpha>=230 的主体像素, 返回每通道 (p5,p50,p95) 数组(3x3)。"""
    nf = count_frames(ff, src) or 0
    if nf <= 0:
        return None
    idxs = sorted(set(int(round(x)) for x in np.linspace(0, nf - 1, min(n, nf))))
    px = []
    for i in idxs:
        fr = decode_src_frame(ff, src, prekeyed, i)
        if fr is None:
            continue
        rgba = fr if prekeyed else key_fn(fr[..., :3])
        m = rgba[..., 3] >= 230
        if m.any():
            px.append(rgba[..., :3][m])
    if not px:
        return None
    px = np.concatenate(px).astype(np.float32)
    return np.array([[float(np.percentile(px[:, c], q)) for q in (5, 50, 95)] for c in range(3)])


def build_curve_lut(src_pct, dst_pct):
    """逐通道分段线性 LUT: 0->0, p5->t5, p50->t50, p95->t95, 255->255。
    只动亮度分布, 不改几何; 分段点在分位数上, 所以暗部不会被拉偏。"""
    lut = []
    grid = np.arange(256, dtype=np.float32)
    for c in range(3):
        xs = np.concatenate(([0.0], src_pct[c], [255.0]))
        ys = np.concatenate(([0.0], dst_pct[c], [255.0]))
        for i in range(1, len(xs)):          # 保证严格递增, 防 np.interp 退化
            if xs[i] <= xs[i - 1]:
                xs[i] = xs[i - 1] + 1e-3
            if ys[i] < ys[i - 1]:
                ys[i] = ys[i - 1]
        lut.append(np.interp(grid, xs, ys).round().clip(0, 255).astype(np.uint8))
    return lut


# ---------- 主处理 ----------

def process(src, out, wm_rects, bg, t_body, dilate_r, canvas, fps, mov, crf,
            scale=None, stretch=1.0, place=None, prekeyed=False,
            tail_n=0, tail_target=None, tail_self=False, shadow=0.32, resample_mode="auto",
            curve_targets=None):
    import imageio_ffmpeg
    from PIL import Image
    ff = imageio_ffmpeg.get_ffmpeg_exe()

    w, h, src_fps = probe(src)
    fps = fps or src_fps
    cw, ch = canvas
    fit = min(cw / w, ch / h)
    scale = fit if scale is None else float(scale)
    stretch = float(stretch)
    dw, dh = round(w * scale), round(h * scale * stretch)
    need_resize = (dw, dh) != (w, h)
    # 缩放滤波器: auto=旧行为(放大 LANCZOS/缩小 BILINEAR); lanczos=预乘 alpha 的 LANCZOS
    if resample_mode == "lanczos":
        resample, premul = Image.LANCZOS, True
    elif resample_mode == "bilinear":
        resample, premul = Image.BILINEAR, False
    else:
        resample, premul = (Image.LANCZOS if scale > 1.0 else Image.BILINEAR), False
    print(f"src {w}x{h}@{src_fps}fps -> canvas {cw}x{ch}, scale={scale:.4f}, stretch={stretch:.4f}, "
          f"frame={dw}x{dh}, resample={'LANCZOS' if resample == Image.LANCZOS else 'BILINEAR'}"
          f"{'(premul)' if premul else ''}", flush=True)

    if prekeyed:
        bg = t_body = None
    else:
        if bg is None:
            bg = measure_bg(src)
        print(f"bg green = ({bg[0]:.1f}, {bg[1]:.1f}, {bg[2]:.1f})", flush=True)
        if t_body is None:  # 按背景饱和度自动标定, 使阈值边界对应真实不透明度≈0.8
            gn_bg = bg[1] - max(bg[0], bg[2])
            t_body = float(np.clip(round(0.2 * gn_bg), 15, 45))
            print(f"t-body auto = {t_body:.0f} (0.2*gn_bg = {0.2 * gn_bg:.1f})", flush=True)

    # 颜色对齐: 抽样测本片主体分位数, 建 LUT(编码前套, 不二次转码)
    curve_lut = None
    if curve_targets is not None:
        kf = None if prekeyed else (lambda rgb: key_frame(rgb, bg, t_body, dilate_r, wm_rects, shadow))
        sp = measure_source_pct(ff, src, kf, prekeyed)
        if sp is None:
            print("!! 颜色对齐: 测不到本源分位数, 跳过", flush=True)
        else:
            curve_lut = build_curve_lut(sp, np.asarray(curve_targets, dtype=np.float32))
            print("calib src pct = " + " ".join(
                f"{ch}({sp[c][0]:.0f},{sp[c][1]:.0f},{sp[c][2]:.0f})" for c, ch in enumerate("RGB")), flush=True)
            print("calib dst pct = " + " ".join(
                f"{ch}({curve_targets[c][0]:.0f},{curve_targets[c][1]:.0f},{curve_targets[c][2]:.0f})"
                for c, ch in enumerate("RGB")), flush=True)

    # --place: 预扫描角色 bbox(源坐标), 全视频用同一偏移; 否则等比居中(旧行为)
    if place is not None:
        import tempfile
        from pathlib import Path
        key_fn = (lambda rgb: key_frame(rgb, bg, t_body, dilate_r, wm_rects, shadow)) if not prekeyed else None
        with tempfile.TemporaryDirectory() as td:
            bb = measure_char_bbox(ff, src, prekeyed, key_fn, Path(td))
        if bb is None:
            print("!! 预扫描失败: 测不到角色 bbox, 退回居中", flush=True)
            ox, oy = (cw - dw) // 2, (ch - dh) // 2
        else:
            bcx = (bb[0] + bb[2]) / 2.0 * scale            # 角色底中(缩放后, 源坐标->画布)
            bcy = (bb[3] + 0.5) * scale * stretch
            ox = int(round(place[0] - bcx))
            oy = int(round(place[1] - bcy))
            print(f"char bbox(src) = {bb} -> paste offset = ({ox},{oy})", flush=True)
    else:
        ox, oy = (cw - dw) // 2, (ch - dh) // 2

    outs = {
        "webm": subprocess.Popen([ff, "-y", "-v", "error",
            "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{cw}x{ch}", "-r", str(fps), "-i", "-",
            "-c:v", "libvpx-vp9", "-pix_fmt", "yuva420p", "-crf", str(crf), "-b:v", "0",
            "-cpu-used", "2", "-auto-alt-ref", "1", "-an", out], stdin=subprocess.PIPE),
    }
    if mov:
        outs["mov"] = subprocess.Popen([ff, "-y", "-v", "error",
            "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{cw}x{ch}", "-r", str(fps), "-i", "-",
            "-c:v", "qtrle", "-pix_fmt", "rgba", "-an", out.rsplit(".", 1)[0] + ".mov"],
            stdin=subprocess.PIPE)

    def to_canvas(rgba_src):
        """源分辨率 RGBA -> 缩放/锚点 -> 画布 RGBA(尾帧构造与主循环共用, 保证像素一致)"""
        if need_resize:
            if premul:
                # 预乘 alpha 缩放: 透明区的 RGB 是 0, 直接 LANCZOS 会把黑渗进边缘
                arr = rgba_src.astype(np.float32)
                av = arr[..., 3:4] / 255.0
                pm = np.dstack([arr[..., :3] * av, arr[..., 3]]).astype(np.uint8)
                s = np.array(Image.fromarray(pm).resize((dw, dh), resample), np.float32)
                av2 = s[..., 3:4] / 255.0
                rgb = np.where(av2 > 1e-4, s[..., :3] / np.maximum(av2, 1e-4), 0.0)
                small = Image.fromarray(
                    np.dstack([np.clip(rgb, 0, 255), s[..., 3:4]]).astype(np.uint8))
            else:
                small = Image.fromarray(rgba_src).resize((dw, dh), resample)
        else:
            small = Image.fromarray(rgba_src)
        if place is not None or need_resize:
            cv = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
            cv.paste(small, (ox, oy))
            frame = np.array(cv)          # np.array(拷贝): asarray 对 PIL 图是只读视图
            if curve_lut is not None:
                for c in range(3):
                    frame[..., c] = curve_lut[c][frame[..., c]]
            frame[frame[..., 3] == 0] = 0
        else:
            frame = rgba_src if rgba_src.flags.writeable else rgba_src.copy()
            if curve_lut is not None:
                for c in range(3):
                    frame[..., c] = curve_lut[c][frame[..., c]]
            frame[frame[..., 3] == 0] = 0
        return frame

    def emit(rgba_src):
        frame = to_canvas(rgba_src)
        for p in outs.values():
            p.stdin.write(frame.tobytes())

    # ---------- 衔接尾帧: 结尾 N 帧交叉溶解到目标首帧(画布坐标, 如待机第 0 帧) ----------
    tail_frames = []
    if tail_n > 0:
        T = None
        if tail_self:                      # 目标 = 自身第 0 帧(同一套变换)
            f0 = decode_src_frame(ff, src, prekeyed, 0)
            if f0 is not None:
                T = to_canvas(key_frame(f0[..., :3], bg, t_body, dilate_r, wm_rects, shadow)
                              if not prekeyed else f0)
        elif tail_target:
            T = load_canvas_frame(ff, tail_target, (cw, ch))
        if T is None:
            print("!! 尾帧目标不可用, 跳过尾帧", flush=True)
            tail_n = 0
        else:
            nf = count_frames(ff, src)
            if nf is None:
                print("!! 源帧数探测失败, 跳过尾帧", flush=True)
                tail_n = 0
            else:
                cap = max(0, nf - 3)       # 至少留 3 帧本体
                if tail_n > cap:
                    print(f"!! 源仅 {nf} 帧, 尾帧截到 {cap}", flush=True)
                    tail_n = cap
                if tail_n > 0:
                    idxs = list(range(nf - tail_n, nf))
                    srcs = decode_src_indices(ff, src, prekeyed, idxs, (w, h))
                    if len(srcs) < tail_n:
                        print(f"!! 尾帧源只解出 {len(srcs)}/{tail_n} 帧, 跳过尾帧", flush=True)
                        tail_frames = []
                    else:
                        Tf = T.astype(np.float32)
                        for k, fr in enumerate(srcs):
                            rgba = (key_frame(fr[..., :3], bg, t_body, dilate_r, wm_rects, shadow)
                                    if not prekeyed else fr)
                            A = to_canvas(rgba).astype(np.float32)
                            a = (k + 1) / tail_n
                            tail_frames.append(((1.0 - a) * A + a * Tf + 0.5).astype(np.uint8))
                        print(f"tail: {tail_n} 帧交叉溶解 -> 目标(末端与目标逐像素一致)", flush=True)

    n = 0
    if prekeyed:
        # 已抠好的输入: ffmpeg 解成 rawvideo rgba 管道
        dec = subprocess.Popen([ff, "-v", "error", "-c:v", "libvpx-vp9", "-i", str(src),
                                "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        nbytes = w * h * 4
        while True:
            buf = b""
            while len(buf) < nbytes:
                chunk = dec.stdout.read(nbytes - len(buf))
                if not chunk:
                    break
                buf += chunk
            if len(buf) < nbytes:
                break
            emit(np.frombuffer(buf, np.uint8).reshape(h, w, 4))
            n += 1
            if n % 90 == 0:
                print(f"  {n} frames...", flush=True)
        dec.stdout.close()
        dec.wait()
    else:
        import imageio.v3 as iio
        for fr in iio.imiter(src, plugin="FFMPEG"):
            emit(key_frame(fr[..., :3], bg, t_body, dilate_r, wm_rects, shadow))
            n += 1
            if n % 90 == 0:
                print(f"  {n} frames...", flush=True)

    for f in tail_frames:
        for p in outs.values():
            p.stdin.write(f.tobytes())
    n += len(tail_frames)

    for p in outs.values():
        p.stdin.close()
    rcs = {k: p.wait() for k, p in outs.items()}
    print(f"frames={n} rc={rcs}", flush=True)
    if any(rcs.values()):
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description="绿幕抠像 v3: 分区域硬色键 + 边缘颜色借用 + 缩放锚点统一")
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--wm", action="append", default=[],
                    help="覆盖物矩形 x0,y0,x1,y1 (源坐标系, 可重复)")
    ap.add_argument("--bg", help="背景绿 r,g,b; 缺省自动测量")
    ap.add_argument("--t-body", type=float, default=T_BODY,
                    help="本体区色键阈值; 缺省按背景饱和度自动标定 0.2*gn_bg(15~45)")
    ap.add_argument("--dilate", type=int, default=6)
    ap.add_argument("--canvas", default="1280x720")
    ap.add_argument("--scale", type=float, default=None,
                    help="源->画布缩放; 缺省等比适配")
    ap.add_argument("--stretch", type=float, default=1.0,
                    help="垂直修正系数(乘在 scale 上); 统一长宽比用, 1=不修")
    ap.add_argument("--place", default=None,
                    help="角色底中对齐到画布 cx,cy (多动作统一位置)")
    ap.add_argument("--tail", type=int, default=0,
                    help="结尾交叉溶解帧数: 把最后 N 帧逐帧溶到目标首帧, 无缝衔接下一动作")
    ap.add_argument("--tail-target", default=None,
                    help="过渡目标 .png(画布尺寸 RGBA)/.webm(取第0帧); 与 --tail-self 二选一")
    ap.add_argument("--tail-self", action="store_true",
                    help="过渡目标=本视频第 0 帧(待机自循环接缝平滑)")
    ap.add_argument("--prekeyed", action="store_true",
                    help="输入已是带 alpha 的 webm, 跳过抠像")
    ap.add_argument("--shadow", type=float, default=0.32,
                    help="脚底阴影保留为半透明的 alpha 值; 0=完全抠掉阴影(缺省 0.32)")
    ap.add_argument("--resample", default="auto", choices=["auto", "bilinear", "lanczos"],
                    help="缩放滤波器; auto=旧行为(缩小用 BILINEAR, 会发虚), "
                         "缩小到 640x360 想要锐利请用 lanczos")
    ap.add_argument("--curve", default=None,
                    help="颜色对齐: 目标素材本体(alpha>=230)的 p5,p50,p95 x RGB 共 9 个数, "
                         "如 28,75.5,236,33,92.5,220,52.5,144.5,222")
    ap.add_argument("--fps", type=float, default=None)
    ap.add_argument("--mov", action="store_true")
    ap.add_argument("--crf", type=int, default=28)
    a = ap.parse_args()

    wm_rects = [tuple(int(x) for x in s.split(",")) for s in a.wm]
    bg = tuple(float(x) for x in a.bg.split(",")) if a.bg else None
    bg = np.array(bg, dtype=np.float32) if bg else None
    cw, ch = (int(x) for x in a.canvas.split("x"))
    place = tuple(float(x) for x in a.place.split(",")) if a.place else None
    curve_targets = None
    if a.curve:
        vals = [float(x) for x in a.curve.split(",")]
        if len(vals) != 9:
            ap.error("--curve 需要 9 个数: r5,r50,r95,g5,g50,g95,b5,b50,b95")
        curve_targets = np.array(vals, dtype=np.float32).reshape(3, 3)
    process(a.input, a.output, wm_rects, bg, a.t_body, a.dilate, (cw, ch), a.fps, a.mov, a.crf,
            scale=a.scale, stretch=a.stretch, place=place, prekeyed=a.prekeyed,
            tail_n=a.tail, tail_target=a.tail_target, tail_self=a.tail_self, shadow=a.shadow,
            resample_mode=a.resample, curve_targets=curve_targets)


if __name__ == "__main__":
    main()
