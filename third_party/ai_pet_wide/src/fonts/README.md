# fonts

Every face the app draws with. Five Chinese faces, all cut from the same typeface
— Alibaba PuHuiTi 3 — plus two FontAwesome sizes for glyphs.

| Face | Token | Size | Weight | Coverage |
|---|---|---|---|---|
| `AlibabaPuHuiTi3_SemiBold32` | `PET_FONT_CLOCK` | 32 px | semibold | digits, Latin, ten date glyphs |
| `AlibabaPuHuiTi3_Medium20` | `PET_FONT_CJK_TITLE` | 20 px | medium | the app's own chrome strings |
| `AlibabaPuHuiTi3_Regular18_Static` | `PET_FONT_CJK` | 18 px | regular | GB2312 + ASCII + emoji |
| `AlibabaPuHuiTi3_Medium16` | `PET_FONT_CJK_CARD` | 16 px | medium | the same chrome subset |
| `AlibabaPuHuiTi3_Regular14` | `PET_FONT_CJK_SM` | 14 px | regular | GB2312 + ASCII + emoji |
| `font_awesome_16_4` | `PET_FONT_ICON` | 16 px | — | the `FONT_AWESOME_*` macros |
| `font_awesome_20_4` | `PET_FONT_ICON_LG` | 20 px | — | the same, large |

The five sizes are the app's whole type scale — 32 / 20 / 18 / 16 / 14, semibold and
medium against regular. Before them there was one size at one weight and the only
hierarchy a page could state was colour.

`AlibabaPuHuiTi3_Medium24.c` is still in the tree and is no longer referenced by
anything: the header face came down to 20 px, which is what left the 16 px card heading a
step below it rather than two. It is kept rather than deleted because the app directory is
not tracked, so a delete is not recoverable from git.

## AlibabaPuHuiTi3_Regular18_Static.c

The body face. 18 px, 2 bpp, generated with `lv_font_conv`.

**Why this file and not LVGL's built-in CJK font.** LVGL ships `lv_font_simsun_16_cjk`,
but it contains only 1413 glyphs and is missing ordinary characters this UI needs — 备
醒 记 删 标 签 搜 索 办 详 录 设 历 闹 钟 are all absent. Text would render as blank
boxes. This font covers the full GB2312 set plus ASCII, punctuation and emoji, so any
Chinese that reaches the screen renders — including Chinese that arrives later from
cloud transcription.

**Cost.** 3.6 MB of source, about 566 KB of object code. That is affordable here because
this app deliberately does not link `src/ai_components`, which is where the multi-megabyte
Puhui font variants live.

**Provenance.** Copied verbatim from
`src/ai_components/ai_ui/font/AlibabaPuHuiTi3_Regular18_Static.c`, which is the TuyaOpen
SDK's own copy. Keeping a local copy makes the app self-contained: it builds whether or
not `ENABLE_AI_COMPONENTS` is set. Git stores identical content once, so the duplicate
costs nothing in the repository.

## AlibabaPuHuiTi3_Medium20.c / AlibabaPuHuiTi3_Medium16.c

The chrome faces: page headers in 20 px (`PET_FONT_CJK_TITLE`), card headings in 16 px
(`PET_FONT_CJK_CARD`). 4 bpp, 348 KB and 233 KB of source, about 57 KB and 38 KB of
object code, `line_height` 22 and 18.

**The 16 px face exists because of one card.** The narrowest heading in the app is one of
the home page's two-up summary cards, whose heading column is 93 px wide; `今日待办` at
20 px fills it with nothing left over, and at 16 px it leaves the row room to breathe.
The header keeps 20 px, which is what leaves the two sizes reading as a scale rather than
as one size used twice.

**Both are subsets, on purpose.** They carry the 386 characters this app's own sources
spell out — every string literal in `src/` and `include/`, plus printable ASCII and the
CJK punctuation a label might pick up. Full GB2312 at these sizes would be several MB of
source for faces that are used on two kinds of label.

**The rule that comes with them.** They are safe for chrome the app writes itself and
*not* for text that arrives from the model. A character they do not carry draws as a
placeholder box, because `LV_USE_FONT_PLACEHOLDER` is 1. **Anything new that puts a model
value into a header or a card title has to check this first.** The home page's to-do card
is the case that has already bitten: it used to set its heading from
`PET_TODO_LIST_T::title`, and now spells out `待办` instead — the model's `今日待办` is
four characters and does not fit the card. The note page, whose card is full width, still
uses the model's title.

**Regenerating.** The glyph set comes from the sources, so it moves when they do:

```bash
# 1. Collect the glyph set (see the script in this file's history, or re-derive it:
#    every double-quoted literal in src/ and include/, minus src/fonts and src/image,
#    which hold generated symbol lists, plus ASCII 0x20-0x7E).
# 2. Cut it, once per size.
for size in 20 16; do
    lv_font_conv --bpp 4 --size $size --no-compress \
        --font AlibabaPuHuiTi-3-65-Medium.ttf \
        --symbols "$(cat glyphs.txt)" \
        --format lvgl -o AlibabaPuHuiTi3_Medium$size.c
done
```

`AlibabaPuHuiTi-3-65-Medium.ttf` is not in the repository. It comes from the npm package
`@fontpkg/alibaba-pu-hui-ti-3-0`, which carries all ten weights of the family.

## AlibabaPuHuiTi3_Regular14.c

The working face. 14 px, 2 bpp, full GB2312 plus ASCII and emoji — the same coverage as
the 18 px body face, 2.9 MB of source and about 463 KB of object code.

**Why a second full-GB2312 face rather than a subset.** The 14 px face is what every row
label, button caption, tile caption and settings group heading is set in, and those labels
are the app's own strings — but the status strip and the to-do rows beside them are not,
and a settings row label can be a model value. A subset would put a placeholder box on the
one surface where the app cannot know what it will be asked to draw, so this face carries
the whole set and the two chrome faces above stay subsets.

**Why 14 px at all.** The settings page is nine rows and four group headings in a 264 px
area, and the rows are the same height as their controls (34 px). At 18 px a row label and
a slider could not both have room, and the page came to 540 px against a 246 px viewport —
the same reason it scrolls. At 14 px the rows are legible at arm's length on this panel
and the groups fit.

## AlibabaPuHuiTi3_SemiBold32.c

The figures: the two numbers in the home page's to-do card. 32 px, 4 bpp, about 150 KB of
source. `line_height` is 34.

**It was 40 px**, and the reason it is not any more is the panel, not the type. The home
page is the page with no header and no action bar — its three bands have to fit 266 px —
and at 40 px the two figures came to 82 px inside a 157 px card and pushed the page past
the bottom. 32 px semibold keeps the figures the largest thing on the page and gives the
row back the height it needed. It also sits better against the 20 px card heading: 40 was
a poster, 32 is a summary.

**Also a subset**, and a much tighter one: `. / 0-9 :`, A-Z, a-z, and the ten CJK glyphs
一二三四五六日星月期 plus 年月分秒. It has **no space and no hyphen**, and
`LV_USE_FONT_PLACEHOLDER` is 1, so `"2026-10-12 15:30"` dropped into it would show three
placeholder boxes. It is for a count, not for a date.

`PET_FONT_CLOCK`'s name is now a misnomer — the status strip draws its clock in the 14 px
working face. It is kept because renaming it would touch four files for no behaviour.

## font_awesome_16_4.c / font_awesome_20_4.c

FontAwesome 6 regular, 4 bpp, about 10 KB and 15 KB of object code. 16 px is the working
size for buttons; 20 px is for a tile's glyph and an empty state.

**What they do not have:** plus, minus, clock, microphone, camera, or any `LV_SYMBOL_*`
glyph. The stepper keeps ASCII `+`/`-` from the CJK face, and the home page's camera is
drawn as three rectangles rather than set as a glyph — see `pui_camera_glyph()`. Check
`font_awesome_symbols.h` before reaching for a glyph; an uncovered character draws as a
placeholder box.

`font_awesome_symbols.h` is the SDK's own macro list, copied alongside the fonts so the
app does not have to include anything from `src/ai_components`. Because
`include/pet_theme.h` includes it by name, `src/fonts` has to be on the compiler's include
path as well as in the source glob; see the app's `CMakeLists.txt`.

## License

Alibaba PuHuiTi 3.0 is published by Alibaba under a free license that permits commercial
use. Confirm this against Alibaba's current terms before shipping a commercial product.
FontAwesome 6's icons are CC BY 4.0 and its fonts are SIL OFL 1.1.
