# Pack the VRM PNG sequences (252x350 RGBA, transparent bg) into per-action
# .anim files for the T5AI hardware-JPEG player.
#
# Each frame is flattened onto the sky background and pasted onto the full
# 320x480 screen canvas: the 252x350 character canvas is upscaled by
# --char-scale (default 1.25x -> 315x437) and bottom-anchored with a
# configurable lift (--lift, default 25px) so the character sits optically
# centred instead of hugging the screen bottom. Frames are JPEG-encoded
# q88 baseline (--subsampling 2 = 4:2:0); the on-device libjpegturbo decoder
# accepts 4:2:0/4:2:2/4:4:4, the legacy tjpgd SW path only accepted 4:2:0.
#
# .anim v2 layout (little-endian):
#   [magic "ANMJ"][u16 w][u16 h][u16 count][u16 fps][u8 flags][u8 pad x3]
#   [u32 offsets[count+1] (absolute from file start)][jpeg blobs]
#   flags bit0 = one-shot (play once, then advance to next animation)
#
# Usage:
#   python pack_png_seq.py                       # pack everything, defaults below
#   python pack_png_seq.py --only idle_normal    # pack a single action
#
# One-shot actions are read from loop_diff.json (loop_diff present = not a
# seamless loop = play once).
import argparse
import glob
import io
import json
import os
import struct
import sys

from PIL import Image

SCREEN_W, SCREEN_H = 320, 480
CANVAS_W, CANVAS_H = 315, 438   # re-rendered final-scale canvas (1.25x baked in)
OFF_X, OFF_Y = (SCREEN_W - CANVAS_W) // 2, (SCREEN_H - CANVAS_H) // 2  # 2, 21

BASE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(os.path.dirname(BASE), "third_party", "resources")
SRC_DIR = os.path.join(RES, "png_sequences")
OUT_DIR = os.path.join(RES, "packed_anim")
LOOP_DIFF = os.path.join(RES, "loop_diff.json")
SKY_IMG = os.path.join(RES, "scenes", "sky", "pz.png")


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=SRC_DIR)
    ap.add_argument("--out", default=OUT_DIR)
    ap.add_argument("--quality", type=int, default=88)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--bg", default=None, help="flat colour RRGGBB (default when no --bg-image)")
    ap.add_argument("--bg-image", default=SKY_IMG,
                    help="background image cropped/resized to 320x480 (sky scene)")
    ap.add_argument("--char-scale", type=float, default=1.0,
                    help="upscale factor for the 252x350 character canvas")
    ap.add_argument("--lift", type=int, default=25,
                    help="lift the character canvas N px above the screen "
                         "bottom (0 = feet at the very bottom; 25 = optically "
                         "centred)")
    ap.add_argument("--subsampling", type=int, default=1,
                    help="0=4:4:4 1=4:2:2 2=4:2:0; the player's libjpegturbo "
                         "accepts all three (legacy tjpgd SW wanted 4:2:0)")
    ap.add_argument("--single-qt", action="store_true",
                    help="force identical luma/chroma quantization tables "
                         "(HW decoder DEC_ERR experiment)")
    ap.add_argument("--suffix", default="", help="output name suffix, e.g. _1qt")
    ap.add_argument("--mode", choices=("full", "patch"), default="full",
                    help="full = full-screen frames (ANMJ, legacy); "
                         "patch = static bg (bg.rgb) + per-frame character "
                         "patch JPEG (ANMP) - ~40%% less decode/SD traffic")
    ap.add_argument("--only", default=None, help="pack a single action by name")
    return ap.parse_args()


def rgb565_bytes(img):
    """RGB888 image -> little-endian RGB565 raw bytes (panel order)."""
    px = img.load()
    out = bytearray(img.width * img.height * 2)
    i = 0
    for yy in range(img.height):
        for xx in range(img.width):
            r, g, b = px[xx, yy]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out[i] = v & 0xFF
            out[i + 1] = v >> 8
            i += 2
    return bytes(out)


def canvas_bbox_union(files):
    """Union alpha bounding box (canvas coords) over the given frames."""
    x0, y0, x1, y1 = CANVAS_W, CANVAS_H, 0, 0
    for fp in files:
        bb = Image.open(fp).getchannel("A").getbbox()
        if bb is None:
            continue
        x0 = min(x0, bb[0]); y0 = min(y0, bb[1])
        x1 = max(x1, bb[2]); y1 = max(y1, bb[3])
    return x0, y0, x1, y1


def build_background(args):
    """Full-screen background layer as RGB image."""
    if args.bg_image:
        im = Image.open(args.bg_image).convert("RGB")
        # centre-crop to the screen aspect ratio, then resize
        target = SCREEN_W / SCREEN_H
        w, h = im.size
        if w / h > target:
            nw = int(h * target)
            im = im.crop(((w - nw) // 2, 0, (w + nw) // 2, h))
        else:
            nh = int(w / target)
            im = im.crop((0, (h - nh) // 2, w, (h + nh) // 2))
        return im.resize((SCREEN_W, SCREEN_H), Image.LANCZOS)
    rgb = tuple(int(args.bg[i:i + 2], 16) for i in (0, 2, 4))
    return Image.new("RGB", (SCREEN_W, SCREEN_H), rgb)


def main() -> int:
    args = parse_args()
    oneshot = set()
    if os.path.isfile(LOOP_DIFF):
        with open(LOOP_DIFF) as f:
            oneshot = set(json.load(f).keys())
    bg_img = build_background(args)

    scaled_w = int(CANVAS_W * args.char_scale)
    scaled_h = int(CANVAS_H * args.char_scale)
    paste_x = (SCREEN_W - scaled_w) // 2
    # bottom-anchored, then lifted by --lift so the character sits optically
    # centred instead of hugging the bottom edge (the character's internal
    # canvas margin already keeps its feet ~50px*scale above the canvas bottom)
    paste_y = max(SCREEN_H - scaled_h - args.lift, 0)

    acts = sorted(d for d in os.listdir(args.src)
                  if os.path.isdir(os.path.join(args.src, d)))
    if args.only:
        acts = [a for a in acts if a == args.only]
    if not acts:
        print("no action dirs found in", args.src)
        return 1
    os.makedirs(args.out, exist_ok=True)

    if args.mode == "patch":
        bg_path = os.path.join(args.out, "bg.rgb")
        with open(bg_path, "wb") as f:
            f.write(rgb565_bytes(bg_img.convert("RGB")))
        print("bg: %s (%dKB)" % (bg_path, os.path.getsize(bg_path) // 1024))

    total = 0
    print("%-20s %6s %8s %9s  %s" % ("action", "frames", "flags", "size", "jpeg avg"))
    for act in acts:
        files = sorted(glob.glob(os.path.join(args.src, act, "*.png")))
        if not files:
            print("%-20s SKIP (no frames)" % act)
            continue
        flags = 1 if act in oneshot else 0

        rect = None
        if args.mode == "patch":
            # per-action union alpha bbox (canvas coords) -> screen rect,
            # padded for resampling bleed and snapped to the hw decoder's
            # MCU grid (16 px wide for 4:2:2 h2v1, 8 px tall)
            x0, y0, x1, y1 = canvas_bbox_union(files)
            sx0 = paste_x + int(x0 * args.char_scale) - 2
            sy0 = paste_y + int(y0 * args.char_scale) - 2
            sx1 = paste_x + int(x1 * args.char_scale) + 3
            sy1 = paste_y + int(y1 * args.char_scale) + 3
            pw = min(((sx1 - sx0) + 31) // 32 * 32, SCREEN_W - sx0)
            ph = min(((sy1 - sy0) + 7) // 8 * 8, SCREEN_H - sy0)
            rect = (max(sx0, 0), max(sy0, 0), max(pw, 16), max(ph, 8))

        jpegs = []
        for fp in files:
            fg = Image.open(fp).convert("RGBA")
            if fg.size != (CANVAS_W, CANVAS_H):
                print("%-20s BAD SIZE %s %s" % (act, fp, fg.size))
                return 1
            frame = bg_img.copy()
            if args.char_scale != 1.0:
                fg = fg.resize((scaled_w, scaled_h), Image.LANCZOS)
            frame.paste(fg, (paste_x, paste_y), fg)
            if rect is not None:
                rgb = frame.crop((rect[0], rect[1],
                                  rect[0] + rect[2], rect[1] + rect[3])).convert("RGB")
            else:
                rgb = frame.convert("RGB")
            # All frames go through the same DMA2D YUV->RGB converter on the
            # device, whose fixed math expects LIMITED-range YUV (TV levels,
            # Y 16..235 / chroma 16..240). Pre-encoding that way makes the
            # hardware conversion land on the exact source RGB - otherwise
            # frames render a few percent brighter than bg.rgb.
            y, cb, cr = rgb.convert("YCbCr").split()
            y = y.point(lambda v: min(235, max(16, round(16 + v * 219 / 255))))
            cb = cb.point(lambda v: min(240, max(16, round(128 + (v - 128) * 224 / 255))))
            cr = cr.point(lambda v: min(240, max(16, round(128 + (v - 128) * 224 / 255))))
            rgb = Image.merge("YCbCr", (y, cb, cr))
            buf = io.BytesIO()
            if args.single_qt:
                # first pass to obtain the scaled tables, then re-save with
                # the luma table for both channels (one identical DQT)
                tmp = io.BytesIO()
                rgb.save(tmp, "JPEG", quality=args.quality,
                         subsampling=args.subsampling)
                tmp.seek(0)
                qt = Image.open(tmp).quantization
                rgb.save(buf, "JPEG", quality=args.quality,
                         subsampling=args.subsampling, qtables=[qt[0], qt[0]])
            else:
                rgb.save(buf, "JPEG", quality=args.quality,
                         subsampling=args.subsampling)
            jpegs.append(buf.getvalue())

        if args.mode == "patch":
            # ANMP: 28-byte header (screen-size in w/h is the PATCH size),
            # count END-offsets, then patch JPEGs. The device composites the
            # patch over the static bg.rgb framebuffer at (px, py).
            data_off = 28 + 4 * len(jpegs)
            out_path = os.path.join(args.out, act + args.suffix + ".anim")
            with open(out_path, "wb") as f:
                f.write(b"ANMP")
                f.write(struct.pack("<HHHH", rect[2], rect[3], len(jpegs), args.fps))
                f.write(struct.pack("<I", flags))
                f.write(struct.pack("<I", data_off))
                f.write(struct.pack("<HHHH", rect[0], rect[1], 0, 0))
                off = data_off
                for j in jpegs:
                    off += len(j)
                    f.write(struct.pack("<I", off))
                for j in jpegs:
                    f.write(j)
        else:
            # v2 header: 16 bytes, then absolute offset table (count+1 entries)
            data_off = 16 + 4 * (len(jpegs) + 1)
            out_path = os.path.join(args.out, act + args.suffix + ".anim")
            with open(out_path, "wb") as f:
                f.write(b"ANMJ")
                f.write(struct.pack("<HHHH", SCREEN_W, SCREEN_H, len(jpegs), args.fps))
                f.write(struct.pack("<B3x", flags))
                off = data_off
                for j in jpegs:
                    f.write(struct.pack("<I", off))
                    off += len(j)
                f.write(struct.pack("<I", off))
                for j in jpegs:
                    f.write(j)

        sz = os.path.getsize(out_path)
        avg = sum(len(j) for j in jpegs) // len(jpegs)
        total += sz
        print("%-20s %6d %8s %8dKB  %dKB" %
              (act, len(jpegs), "oneshot" if flags else "loop", sz // 1024, avg // 1024))

    print("-" * 56)
    print("total: %d files, %.1f MB -> %s" % (len(acts), total / 1048576, args.out))
    print("paste: char scale %.2fx at (%d,%d) lift=%dpx, bg=%s, q%d subsampling=%d" %
          (args.char_scale, paste_x, paste_y, args.lift,
           os.path.basename(args.bg_image or args.bg), args.quality,
           args.subsampling))
    return 0


if __name__ == "__main__":
    sys.exit(main())
