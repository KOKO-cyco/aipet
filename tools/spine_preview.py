#!/usr/bin/env python3
"""把 Spine 的 setup pose 合成成一张预览图，用来核对坐标约定。

为什么先做这个
--------------
Spine 的 attachment x/y 到底是"图片左下角"还是"图片中心"，直接决定我们转换器里
的轴心(pivot)怎么算 —— 错了角色会整体错位。与其翻文档，不如合成出来看一眼：
约定错了，图会明显歪掉。

用法：
    python spine_preview.py <spineboy 目录> [输出 png]

    python spine_preview.py third_party/spineboy-4.3 out.png
"""
import json
import math
import os
import sys

from PIL import Image

# 只画角色本体，跳过传送门/悬浮板那些特效槽位
CHAR_SLOT_FROM = 19


def build_world(bones):
    """按 setup pose 求每根骨骼的世界变换。

    返回 name -> (x, y, a, b, c, d)，矩阵约定与 Spine 一致（y 轴向上，逆时针为正）：
        [ a  b ]   [ cos*sx  -sin*sx ]
        [ c  d ] = [ sin*sy   cos*sy ]
    骨骼局部 +x 轴在世界里的方向是矩阵第一列 (a, c)。
    """
    by_name = {b["name"]: b for b in bones}
    world = {}

    for b in bones:                      # json 里父骨骼总排在子骨骼之前
        rot = math.radians(b.get("rotation", 0.0))
        cos, sin = math.cos(rot), math.sin(rot)
        sx = b.get("scaleX", 1.0)
        sy = b.get("scaleY", 1.0)

        la = cos * sx
        lb = -sin * sx
        lc = sin * sy
        ld = cos * sy

        x, y = b.get("x", 0.0), b.get("y", 0.0)
        p = by_name.get(b.get("parent"))

        if p is None:
            world[b["name"]] = (x, y, la, lb, lc, ld)
        else:
            px, py, pa, pb, pc, pd = world[p["name"]]
            # 世界矩阵 = 父矩阵 × 局部矩阵。注意是**加号**：
            #   [pa pb]   [la lb]   [pa·la + pb·lc   pa·lb + pb·ld]
            #   [pc pd] × [lc ld] = [pc·la + pd·lc   pc·lb + pd·ld]
            # 写成减号的话，旋转会沿层级累积偏移 —— 头骨会转到 160° 去，
            # 整个角色看起来是斜的、部件散开。
            world[b["name"]] = (
                pa * x + pb * y + px,
                pc * x + pd * y + py,
                pa * la + pb * lc,
                pa * lb + pb * ld,
                pc * la + pd * lc,
                pc * lb + pd * ld,
            )
    return world


def place(img, att, bone_xf, anchor, out, ox, oy, scale):
    """把一张部件图按 attachment 的局部变换贴到 out 上。

    用仿射变换而不是 Image.rotate：后者不 expand 会把旋转后的内容裁掉，
    部件一转到斜角就缺一块，看起来像"坐标约定错了"。

    anchor: 'center' -> 图片中心落在 attachment 原点
            'bl'     -> 图片左下角落在 attachment 原点

    推导（Spine 是 y 向上的坐标系，画布 y 向下）：
        正向   X = Cx + (u-px)cos - (py-v)sin
               Y = Cy - ((u-px)sin + (py-v)cos)
        逆解   u = cos·X - sin·Y + (px - cos·Cx + sin·Cy)
               v = sin·X + cos·Y + (py - sin·Cx - cos·Cy)
    """
    ax = att.get("x", 0.0)
    ay = att.get("y", 0.0)
    arot = math.radians(att.get("rotation", 0.0))

    bx, by, ba, bb, bc, bd = bone_xf

    # attachment 原点在骨骼空间里旋转后 + 平移到世界
    wx = bx + ba * ax + bb * ay
    wy = by + bc * ax + bd * ay

    # 骨骼局部 +x 轴在世界里的方向 = 矩阵第一列 (a, c)
    rot = math.atan2(bc, ba) + arot
    cos, sin = math.cos(rot), math.sin(rot)

    w, h = img.width, img.height
    px = w / 2.0 if anchor == "center" else 0.0
    py = h / 2.0 if anchor == "center" else float(h)

    # 世界 -> 画布（画布 y 向下）
    Cx = ox + wx * scale
    Cy = oy - wy * scale
    pxs, pys = px * scale, py * scale

    # 部件在画布上的包围盒。局部 (lx,ly) -> 世界 (cos·lx - sin·ly, sin·lx + cos·ly)，
    # 取负到画布 y 向下之后：
    #     X = Cx + cos·lx - sin·ly
    #     Y = Cy - sin·lx - cos·ly
    xs, ys = [], []
    for (u, v) in ((0, 0), (w * scale, 0), (w * scale, h * scale), (0, h * scale)):
        lx, ly = u - pxs, pys - v
        xs.append(Cx + cos * lx - sin * ly)
        ys.append(Cy - sin * lx - cos * ly)

    x0 = max(int(min(xs)) - 2, 0)
    y0 = max(int(min(ys)) - 2, 0)
    x1 = min(int(max(xs)) + 3, out.width)
    y1 = min(int(max(ys)) + 3, out.height)
    if x0 >= x1 or y0 >= y1:
        return

    # 逆解：
    #     u =  cos·X - sin·Y + (pxs - cos·Cx + sin·Cy)
    #     v =  sin·X + cos·Y + (pys - sin·Cx - cos·Cy)
    # 再叠加子区域原点的偏移。
    c = pxs - cos * Cx + sin * Cy
    f = pys - sin * Cx - cos * Cy
    c += cos * x0 - sin * y0
    f += sin * x0 + cos * y0

    warped = img.convert("RGBA").transform(
        (x1 - x0, y1 - y0), Image.AFFINE, (cos, -sin, c, sin, cos, f),
        resample=Image.BICUBIC)

    out.alpha_composite(warped, (x0, y0))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    root = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "spine_preview.png"

    with open(os.path.join(root, "export", "spineboy-pro.json"), encoding="utf-8") as f:
        d = json.load(f)

    sk = d["skins"][0]
    world = build_world(d["bones"])

    # 先算一遍包围盒，确定画布尺寸
    xs, ys = [], []
    for slot in d["slots"][CHAR_SLOT_FROM:]:
        att_name = slot.get("attachment")
        if not att_name:
            continue
        att = sk["attachments"].get(slot["name"], {}).get(att_name)
        if not att:
            continue
        bx, by, ba, bb, bc, bd = world[slot["bone"]]
        wx = bx + ba * att.get("x", 0) + bb * att.get("y", 0)
        wy = by + bc * att.get("x", 0) + bd * att.get("y", 0)
        xs += [wx - att["width"], wx + att["width"]]
        ys += [wy - att["height"], wy + att["height"]]

    pad = 40
    minx, maxx = min(xs) - pad, max(xs) + pad
    miny, maxy = min(ys) - pad, max(ys) + pad
    W = int(maxx - minx)
    H = int(maxy - miny)
    ox, oy = -minx, maxy          # Spine 世界坐标 -> 画布坐标

    print("世界包围盒: x %.0f..%.0f  y %.0f..%.0f  -> 画布 %dx%d" % (minx, maxx, miny, maxy, W, H))

    for anchor in ("center", "bl"):
        canvas = Image.new("RGBA", (W, H), (30, 30, 40, 255))

        for slot in d["slots"][CHAR_SLOT_FROM:]:
            att_name = slot.get("attachment")
            if not att_name:
                continue
            att = sk["attachments"].get(slot["name"], {}).get(att_name)
            if not att or att.get("type") not in (None, "region"):
                continue

            p = os.path.join(root, "images", att_name + ".png")
            if not os.path.exists(p):
                continue
            place(Image.open(p).convert("RGBA"), att, world[slot["bone"]],
                  anchor, canvas, ox, oy, 1.0)

        name = out_path.replace(".png", "_%s.png" % anchor)
        canvas.save(name)
        print("已写出", name, canvas.size)

    return 0


if __name__ == "__main__":
    sys.exit(main())
