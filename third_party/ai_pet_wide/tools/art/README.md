# tools/art — turning the reference art into a sprite

The character in `src/image/pet_char.c` is generated, not drawn by hand. These two
scripts are how it is regenerated.

## Why two steps

The reference art arrives as a screenshot of the whole page: the character is sitting
on a card, with the status bar, the speech bubble, a floating button, a watermark and
a field of sparkles around it. `prepare.py` isolates the character; `to_c.py` encodes
it. Keeping them apart means the keying can be judged — `prepare.py` writes a PNG you
can look at — before 150 KB of bytes are baked into a C file.

## Usage

```sh
python3 tools/art/prepare.py <mockup.png> /tmp/char.png 268
python3 tools/art/to_c.py    /tmp/char.png src/image/pet_char.c pet_char
```

The `268` is the target height in pixels. The stage on the pet page is 280 px tall, so
the sprite is authored a little under that and bottom-anchored.

## What prepare.py does

1. **Flood-fills the background** from the image border rather than thresholding
   globally, so the character's own white parts — apron, ruffles, socks, cushion —
   stay opaque. Only background *connected to the edge* is removed.
2. **Keeps the largest remaining blob.** The fill leaves the page furniture behind as
   separate components, all smaller than the character.
3. **Un-blends the edge.** The illustration's anti-aliasing blended the silhouette
   toward the card's near-white, so a hard mask would leave a pale halo. Alpha is
   recovered, then the background is divided back out of the colour.
4. **Resamples premultiplied**, or the transparent surround bleeds into the edges.

## Constraints

- **`LV_USE_LZ4_*` is off** in `src/liblvgl/v9/conf/lv_conf.h`, so the sprite ships
  uncompressed at 3 bytes a pixel. That is a shared SDK header; do not flip it for one
  app. At 188x268 the sprite is 151 KB against an OTA partition with ~1.3 MB free.
- **The panel is 16-bit**, so RGB565A8 is the native depth and costs nothing in decode
  fidelity. ARGB8888 would be a third larger for no visible gain.
- `apps/tuya.ai/ai_pet/src/image` is in `.clang-format-ignore`; the generated file is
  not hand-formatted.
