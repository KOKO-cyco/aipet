#!/usr/bin/env bash
#
# Record the speech bubble arriving and leaving, as a filmstrip and a GIF.
#
# The bubble is driven by tapping the character, so this replays a few taps through
# the simulator's synthetic pointer and samples the frame counter while the 200 ms
# fade is running. See src/port/pet_sim_shot.c for the click script's syntax.
#
#     apps/tuya.ai/ai_pet_wide/tools/bubble_demo.sh [outdir]
#
# Needs the PC simulator to be configured for ai_pet_wide already — the script will
# build it but will not reconfigure, because reconfiguring wants the SDL2 dev tree that
# sim_config.cmake points at. Output defaults to /tmp.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sim="${here}/../../../../examples/graphics/lvgl_simulator"
out="${1:-/tmp}"

# Where the character's clickable box is, and what to crop the captures to.
#
# Landscape, so these are not the portrait numbers. The pet's card is x 10..199,
# y 24..309 and the whole card takes the press; the sprite is drawn 126x180 from the top
# of the stage, which starts at y 111, so its middle is (105, 201). The bubble sits in
# the holder above the stage — x 10..199, y 24..106 — and the crop is that plus a few
# pixels of margin, so the frame reads as the card's top rather than as a loose text box.
poke="105,201"
crop="0,20,210,112"

cmake --build "${sim}/.build" -j"$(nproc)" >/dev/null
mkdir -p "${out}"

shoot() { # shoot <file> <settle cycles> <click script>
    SDL_VIDEODRIVER=dummy PET_SIM_SHOT="$1" PET_SIM_SETTLE="$2" PET_SIM_CLICK="$3" \
        "${sim}/dist/lvgl_sim" >/dev/null 2>&1
}

# The appear: the page opens with the greeting and the bubble fades up over 50
# cycles. 4 ms a cycle, so these are roughly 0, 16, 32, 56, 88 and 200 ms in.
appear=(0 4 8 14 22 50)
for n in "${appear[@]}"; do shoot "${out}/bub_in_${n}.ppm" "${n}" ""; done

# The disappear: four taps walk the poke table to the empty entry, which is the one
# that takes the bubble away, then the last tap is left un-waited so the settle lands
# inside its fade. The samples crowd the back half of the 200 ms because this one runs
# on ease_in — the first half of it barely moves.
walks=""
for _ in 1 2 3 4; do walks="${walks}${poke};90;"; done
vanish=(1 20 28 36 44 50)
for n in "${vanish[@]}"; do shoot "${out}/bub_out_${n}.ppm" "${n}" "${walks}${poke}"; done

# One tap for each line, settled, so the filmstrip also shows the bubble resizing to
# its text — one row for a short line, two for a long one.
for n in 1 2 3 4; do
    taps=""
    for _ in $(seq 1 "${n}"); do taps="${taps}${poke};90;"; done
    shoot "${out}/bub_line_${n}.ppm" 70 "${taps%;}"
done

python3 - "${out}" "${crop}" <<'PY'
import sys, glob, re
from PIL import Image

out = sys.argv[1]
box = tuple(int(v) for v in sys.argv[2].split(","))
def shot(path):
    return Image.open(path).convert("RGB").crop(box)
def frames(prefix):
    fs = sorted(glob.glob(f"{out}/{prefix}_*.ppm"), key=lambda p: int(re.search(r"_(\d+)\.ppm$", p).group(1)))
    return [shot(p) for p in fs]

rows = [("in", frames("bub_in"))] + [(f"line {i}", [shot(f"{out}/bub_line_{i}.ppm")]) for i in (1, 2, 3, 4)] + [("out", frames("bub_out"))]

strip = [(lbl, im) for lbl, ims in rows for im in ims]
w, h = strip[0][1].size
sheet = Image.new("RGB", (w, h * len(strip) + 3 * (len(strip) - 1)), (255, 0, 255))
for i, (_, im) in enumerate(strip):
    sheet.paste(im, (0, i * (h + 3)))
sheet.save(f"{out}/pet_bubble_strip.png")

# The GIF is the appear followed by the disappear, so the loop reads as one bubble
# coming and going.
seq = frames("bub_in") + frames("bub_out")
seq[0].save(f"{out}/pet_bubble.gif", save_all=True, append_images=seq[1:], duration=60, loop=0)

print(f"wrote {out}/pet_bubble_strip.png and {out}/pet_bubble.gif")
PY
