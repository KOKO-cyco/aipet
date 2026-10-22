"""Turn the pet mockup into a keyed, trimmed, scaled RGBA sprite.

The mockup is a screenshot of the whole page, so the character arrives surrounded by
card background, the status bar, the speech bubble, a floating button and a watermark.
This isolates the character: flood-fill the connected background from the border, keep
only the largest remaining blob, then un-blend that blob's edge against the background
so it does not carry a white fringe when it is composited on the app's cream page.
"""
import sys
from collections import deque

import numpy as np
from PIL import Image, ImageFilter

src, out_png = sys.argv[1], sys.argv[2]
target_h = int(sys.argv[3]) if len(sys.argv) > 3 else 268

im = Image.open(src).convert("RGB")
a = np.asarray(im).astype(np.float64)
h, w, _ = a.shape

# --- 1. Flood fill the background -------------------------------------------------
ring = np.concatenate([a[0:6].reshape(-1, 3), a[-6:].reshape(-1, 3),
                       a[:, 0:6].reshape(-1, 3), a[:, -6:].reshape(-1, 3)])
bg_ref = np.median(ring, axis=0)

near = (np.abs(a - bg_ref).max(axis=2) <= 22)
seen = np.zeros((h, w), bool)
q = deque()
for x in range(w):
    for y in (0, h - 1):
        if near[y, x] and not seen[y, x]:
            seen[y, x] = True
            q.append((y, x))
for y in range(h):
    for x in (0, w - 1):
        if near[y, x] and not seen[y, x]:
            seen[y, x] = True
            q.append((y, x))
while q:
    y, x = q.popleft()
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        ny, nx = y + dy, x + dx
        if 0 <= ny < h and 0 <= nx < w and near[ny, nx] and not seen[ny, nx]:
            seen[ny, nx] = True
            q.append((ny, nx))
fg = ~seen

# --- 2. Keep only the largest blob, which is the character ------------------------
# Everything else the fill left behind is page furniture: the bubble's text, the
# status bar's digits, the watermark, the sparkles. They are all smaller and all
# disconnected from the character.
label = np.zeros((h, w), np.int32)
best, best_n, cur = 0, 0, 0
for sy in range(h):
    for sx in range(w):
        if not fg[sy, sx] or label[sy, sx]:
            continue
        cur += 1
        n = 0
        q = deque([(sy, sx)])
        label[sy, sx] = cur
        while q:
            y, x = q.popleft()
            n += 1
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny, nx = y + dy, x + dx
                if 0 <= ny < h and 0 <= nx < w and fg[ny, nx] and not label[ny, nx]:
                    label[ny, nx] = cur
                    q.append((ny, nx))
        if n > best_n:
            best_n, best = n, cur
mask = label == best
ys, xs = np.nonzero(mask)
y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
print("character blob %d px, bbox x %d..%d y %d..%d (%dx%d)"
      % (best_n, x0, x1, y0, y1, x1 - x0 + 1, y1 - y0 + 1))

# --- 3. Un-blend the edge ---------------------------------------------------------
# The illustration's own anti-aliasing blended it toward the card's near-white, so a
# hard mask would leave a pale halo. Recovering alpha and then dividing the background
# back out is what removes it.
alpha = Image.fromarray((mask * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.7))
alpha = np.asarray(alpha).astype(np.float64) / 255.0

# The background the edge was blended against: the median of the ring just outside the
# blob, which is card interior rather than the cream page.
dil = np.asarray(Image.fromarray((mask * 255).astype(np.uint8))
                 .filter(ImageFilter.MaxFilter(9))).astype(bool) & ~mask
bg_local = np.median(a[dil].reshape(-1, 3), axis=0) if dil.any() else bg_ref
print("local background", bg_local)

al = alpha[..., None]
rgb = np.where(al > 0.004, (a - (1.0 - al) * bg_local) / np.maximum(al, 1e-6), a)
rgb = np.clip(rgb, 0, 255)

rgba = np.dstack([rgb, alpha * 255.0]).astype(np.uint8)
crop = rgba[y0:y1 + 1, x0:x1 + 1]
print("trimmed", crop.shape[1], "x", crop.shape[0])

# --- 4. Scale ---------------------------------------------------------------------
img = Image.fromarray(crop, "RGBA")
scale = target_h / img.height
size = (max(1, round(img.width * scale)), target_h)
# Premultiplied while resampling, or the transparent surround bleeds into the edges.
arr = np.asarray(img).astype(np.float64)
pm = np.dstack([arr[..., :3] * arr[..., 3:4] / 255.0, arr[..., 3]])
pm = np.asarray(Image.fromarray(pm.astype(np.uint8), "RGBA").resize(size, Image.LANCZOS)).astype(np.float64)
al2 = pm[..., 3:4]
out = np.dstack([np.clip(np.where(al2 > 0, pm[..., :3] * 255.0 / np.maximum(al2, 1e-6), 0), 0, 255), al2])
Image.fromarray(out.astype(np.uint8), "RGBA").save(out_png)
print("scaled to", size)
