/**
 * @file pet_theme.h
 * @brief The design tokens: every colour, measurement and face the UI is built from.
 *
 * Tokens only. This file used to declare the constructors as well — pet_theme_card(),
 * pet_theme_button(), pet_theme_tile() and the rest — and a page called them and then
 * adjusted what came back. The constructors moved to the pui component library (pui.h),
 * where a component owns its own geometry and a page names a tone and a fill instead of
 * a colour and a padding; what is left here is the vocabulary they share.
 *
 * All visual constants live here so the appearance can be tuned in one place
 * without touching page logic. The style is a warm card layout: a cream page, white
 * rounded cards that lift on a soft shadow, one blue accent, and an indigo reserved
 * for the note page and the settings groups.
 *
 * Typography is six faces, all of them the app's own copies under `src/fonts/`
 * (see that directory's README):
 *
 *   - `PET_FONT_CJK` — 18 px regular. Every Chinese glyph, and every string that
 *     mixes scripts. The model's own text: a note's title, a note's lines, the
 *     to-do entries. Nothing the app writes itself uses it — it is the largest
 *     face in the app and only the one page that shows prose can afford it.
 *   - `PET_FONT_CJK_SM` — 14 px regular, and the app's working face: every row
 *     label, button caption, tile caption, settings group heading and the status
 *     strip. All of GB2312, so unlike the two chrome faces below it is safe for a
 *     string that might carry a character the app did not write.
 *   - `PET_FONT_CJK_TITLE` — 20 px *medium*, the same typeface at the next weight
 *     up. The page's own name in the header, and nothing else.
 *   - `PET_FONT_CJK_CARD` — 16 px medium. A card's title. A size down from the
 *     header's, and a subset: see the rule below.
 *   - `PET_FONT_CLOCK` — 32 px *semibold*. The two figures in the home page's
 *     to-do card. A subset too: `. / 0-9 :`, A-Z, a-z, and ten CJK glyphs —
 *     一二三四五六日星月期年月分秒. It was 40 px until the home page turned out to
 *     be 18 px taller than the panel had room for; at 40 the two figures were
 *     82 px of a 157 px card, which is a poster rather than a summary.
 *   - `PET_FONT_ICON` / `PET_FONT_ICON_LG` — FontAwesome, for buttons. It has no
 *     plus, minus or clock glyph, so those still come from the CJK face as ASCII.
 *
 * The two chrome faces are subsets rather than all of GB2312, so they are for chrome
 * the app writes itself, never for text that arrives from the model — see
 * src/fonts/README.md. PET_FONT_CJK and PET_FONT_CJK_SM are the two that are safe.
 *
 * Five Chinese faces is what gives the panel a scale: 32 / 20 / 18 / 16 / 14, and
 * medium and semibold against regular. Before them there was one size and one weight,
 * and the only hierarchy a page could state was colour.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __PET_THEME_H__
#define __PET_THEME_H__

#include "lvgl.h"

/* BOOL_T, and the model types the page builders hand to the components. */
#include "pet_types.h"

/* The FONT_AWESOME_* glyph macros. The file is the SDK's own, copied alongside
   the fonts so the app does not reach into src/ai_components. */
#include "font_awesome_symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
***********************variable define**********************
***********************************************************/

/* The app's own copies of the SDK's faces, so the build does not depend on
   ENABLE_AI_COMPONENTS. See src/fonts/README.md. */
LV_FONT_DECLARE(AlibabaPuHuiTi3_Regular18_Static);
LV_FONT_DECLARE(AlibabaPuHuiTi3_Regular14);
LV_FONT_DECLARE(AlibabaPuHuiTi3_Medium16);
LV_FONT_DECLARE(AlibabaPuHuiTi3_Medium20);
LV_FONT_DECLARE(AlibabaPuHuiTi3_Medium24);
LV_FONT_DECLARE(AlibabaPuHuiTi3_SemiBold32);
LV_FONT_DECLARE(font_awesome_16_4);
LV_FONT_DECLARE(font_awesome_20_4);

/***********************************************************
************************macro define************************
***********************************************************/

/* Surfaces. The page is a warm off-white and the cards on it are pure white, so a card
   is told from the page by its own brightness before its hairline or its shadow has to
   be read. NEUTRAL is the second surface — the panel a paper-like region sits on, and
   the fill behind a secondary button's dark glyph — and ROW is the inset strip inside a
   card.

   Warm, where this started cool: the reference design's page is a cream, and the accents
   (a muted blue, an indigo, an amber, a peach) read as objects laid on it rather than as
   a second scheme fighting the first. One family, low chroma, warm-shifted. */
#define PET_COLOR_BG      lv_color_hex(0xFAF7F2)
#define PET_COLOR_CARD    lv_color_hex(0xFFFFFF)
#define PET_COLOR_NEUTRAL lv_color_hex(0xEAE6E1)

#define PET_COLOR_ROW lv_color_hex(0xFFFEFC)

/* The pet's card sits a shade deeper than the page, so the character has a room of
   its own rather than floating on the same surface as everything else. */
#define PET_COLOR_PET_COLUMN lv_color_hex(0xFFFEFD)

/* The indigo family. It carries the second half of the design: the note page's
   surface, the settings groups' headers, and the AI action. Kept separate from the
   blue accent so the two never have to be told apart by shade alone. */

/* The home page's entry tiles and the camera's shutter. Each tile owns a colour so
   they are told apart at a glance rather than by reading them. These are the *icon*
   colours: the square behind the glyph is a tint of the same hue, not a fill of it —
   see pui_tint(). */

#define PET_COLOR_ORANGE lv_color_hex(0xF4A18C)

/* The card the 今日待办 summary sits on, a shade off the page so it reads as a
   different kind of thing from the white cards beside it. */

/* The viewfinder, and the sticky note pinned into it. The preview is the only dark
   surface in the app — it is standing in for a camera image, and a light one would
   read as another card. The chip is the preview lifted a step, for the one control
   that sits on top of it: it has to read against the dark without competing with the
   note. */
#define PET_COLOR_PREVIEW      lv_color_hex(0x30313A)
#define PET_COLOR_PREVIEW_CHIP lv_color_hex(0x4D505F)
#define PET_COLOR_STICKY       lv_color_hex(0xFBE8B8)

/* Text */
#define PET_COLOR_TEXT      lv_color_hex(0x302E34)
#define PET_COLOR_TEXT_SUB  lv_color_hex(0x746E78)
#define PET_COLOR_TEXT_HINT lv_color_hex(0x817A84)
#define PET_COLOR_ON_ACCENT lv_color_hex(0xFFFFFF)

/* Lines and accents. LINE is the hairline inside a white card; LINE_STRONG is the
   card outline, which has to read against both the page and the white card.

   LINE is deeper than it looks like it needs to be. A 1 px hairline is the only thing
   separating a white card from a near-white page, and at the previous value it was
   invisible on this panel — the cards read as regions of the page rather than as
   objects on it. */
#define PET_COLOR_LINE        lv_color_hex(0xE9E3DD)
#define PET_COLOR_LINE_STRONG lv_color_hex(0xD7D0CB)
#define PET_COLOR_ACCENT      lv_color_hex(0x638FC4)
#define PET_COLOR_ACCENT_SOFT lv_color_hex(0xE6EFFA)
#define PET_COLOR_SUCCESS     lv_color_hex(0x459B7E)

#define PET_COLOR_WARN lv_color_hex(0xF5DFB5)

/* Overlays. A near-black with a touch of warmth, so the scrim sits with the page. */
#define PET_COLOR_SCRIM lv_color_hex(0x302E34)

/* What a card casts. Warm for the same reason. Deep and opaque enough to be seen: the
   previous value was a tint of the page itself at a fifth of an alpha, which on this
   panel is no shadow at all, and a card with no shadow and no border has no edge. */
#define PET_COLOR_SHADOW lv_color_hex(0xB6A99D)

/* How far a tint of a tone is pushed toward white. Two steps, because a surface and a
   fill are different jobs:

   - PET_TINT_MIX is what a *card* is washed with. At 200 of 255 the hue is about a
     fifth of the way up from white — a colour you can name, that still lets black text
     on it be the highest-contrast thing in the card.
   - PET_TINT_FILL is what a *fill* is made of — a soft button, a tile's icon square.
     It has to sit on a card that is already washed with the same hue, so it is a step
     deeper. At the card's own value a soft button on a tinted card was the same colour
     as the card and therefore invisible; that is what this second number fixes. */
#define PET_TINT_MIX  224
#define PET_TINT_FILL 196

/* Geometry. The two softened a step in the last pass: the reference design's surfaces are
   more rounded than the 18/14 the app shipped with, and the difference is legible at this
   size without the cards starting to read as lozenges. The 36 px button stays a full
   capsule below, so this only moves the big surfaces. */
#define PET_RADIUS_CARD 20
#define PET_RADIUS_PILL 16

/* A 36 px button at this radius is a full capsule — the radius is half the height, so
   the ends come out as semicircles whatever the label's width. Every button in the app
   is 36 tall, which is what lets one number do for all of them. */
#define PET_RADIUS_BTN 18

#define PET_PAD_PAGE 12
#define PET_PAD_CARD 10
#define PET_GAP_CARD 10

/* What a card casts. Kept at 14: LV_DRAW_SW_SHADOW_CACHE_SIZE is 0 in this build
   (src/liblvgl/v9/conf/lv_conf.h), so a shadowed card recomputes its blur on every
   frame it is redrawn with nothing amortising it. A static card only ever repaints its
   dirty area, so the cost is invisible there; a scrolling column of them is not. This
   is the width at which the shadow is actually legible, so it is the width, and the
   number of shadowed surfaces is what gets kept down instead. */
#define PET_SHADOW_W  8
#define PET_SHADOW_DY 3
/* LV_OPA_40 rather than LV_OPA_20, which is where this started: on a page this light a
   20 % shadow of a blue-grey is a smudge you have to look for, and the card then reads as
   a rectangle of white rather than as a surface with an edge. LVGL 9.1's opacity scale is
   in tens, so this is the next step up and not an arbitrary 35. */
#define PET_SHADOW_OPA LV_OPA_20

/* A card no longer draws a hairline — it is separated from the page by its shadow — but
   this reserve is kept, because LVGL draws a border *inside* the object's box and the
   one place in the app that derived a column width from a card's width was written
   against that accounting. Keeping the term leaves that derivation two pixels
   conservative rather than two pixels optimistic, which is the safe direction: a grid
   that comes out one pixel too wide wraps its last column onto a second row.

   That derivation is gone — the sheets it belonged to are built from pui_grid, which
   divides whatever the card has rather than being told — but the reserve stays, because
   a border still costs a pixel on each side and the next derivation will want it. */

/* The screen, and the two bands it is cut into. 480x320 landscape, the reference
   design's split: the pet keeps a card on the left and the function panel gets the
   rest.

   The board registers the 3.5" 35565 LCD at its native BOARD_LCD_WIDTH 320 /
   HEIGHT 480 with TUYA_DISPLAY_ROTATION_0, and the touch panel's x_max is the same
   320. Those are the *physical* panel, so they are not touched here — tuya_main.c
   turns the display at runtime instead, with lv_display_set_rotation() right after
   lv_vendor_init(). LVGL's software flush rotates the pixels on the way out and its
   indev rotates the pointer on the way in, so this file only has to name the logical
   size the UI is laid out in. See the comment in tuya_main.c for the 90-vs-270 caveat
   and see ai_pet/ for the portrait arrangement. */
#define PET_SCREEN_W 480
#define PET_SCREEN_H 320

/* The pet's card, the gap beside it, and the panel. The card is sized around the
   character rather than by ratio: the artwork is 188 px wide and is drawn at 0.742 of
   that (PET_CHAR_SCALE), which leaves 140 px of image and 24 px of margin on either
   side inside the card's border. Everything left over goes to the panel, which is where
   the room is wanted — the panel is the whole app. */
#define PET_GAP_PANEL 10
#define PET_COLUMN_W  190
#define PET_PANEL_W   (PET_SCREEN_W - 2 * PET_PAD_PAGE - PET_COLUMN_W - PET_GAP_PANEL)

/* The gap between the status strip and everything below it. The strip's own height is
   PUI_STATUS_H, next to the component that draws it; this one is left here because the
   page shell in pet_ui.c pads the body down by it, and the shell is not a component. */
#define PET_STATUS_GAP 4

/* The character, drawn from a 188x268 sprite. LVGL's scale is 8.8 fixed point, so 256
   is 1:1; LV_USE_FLOAT is off in this build, which is why it is written this way.

   0.742 is set by height rather than by width. The column is 284 px, the bubble holder
   takes 71 of it, and what is left for the stage is 211 — against the 199 the sprite
   comes to at this scale plus the 6 it is inset by (PET_CHAR_TOP), which leaves the
   stand at its feet clear of the bottom edge. It is the tightest constraint in the
   card, so raising the bubble's line ceiling means lowering this.

   The drawn size is kept as its own pair of constants because the image is placed by
   its top-left corner with the scale pivot moved there (see __build_character): the
   object's box stays 188x268 whatever the scale, so the box cannot be used to centre
   the drawing. */
#define PET_CHAR_SCALE 190 /* 0.7421875 */
#define PET_CHAR_W     140
#define PET_CHAR_H     199

/* A settings group's heading bar, and the small filled button that sits at the foot of
   a home-page card (查看全部 / 全部笔记). Both are deliberately shorter than the 36 px
   primary button — they are furniture inside a card, not the page's own action. */
#define PET_GROUP_H    30
#define PET_PILL_BTN_H 28

/* Height of the action bar at the foot of a page. Buttons are 36 px tall inside it,
   so this is 36 plus the padding above and below. */

/* How long a button takes to dim when pressed. Short enough to feel like a response
   to the finger rather than an animation in its own right. */
#define PET_PRESS_MS 120

/* Page transition. The right panel slides sideways when the stack is pushed or popped,
   the outgoing page leaving in the same direction the new one arrives from. 300 ms is the
   panel-scale figure from the motion rules: long enough to read as travel, short enough
   that a back tap does not feel like waiting. The enter curve is an expo-out (fast, then
   settling); the exit is a softer ease-in so the leaving page does not outrun the eye. */
#define PET_NAV_MS 300

/* Ambient room behind the pet. The light sweep drifts over a long cycle so it reads as
   daylight rather than an animation; the motes are the few specks of dust that catch it.
   Both are deliberately slow and low-contrast — the point is that the screen is never
   quite still, not that the user notices them. Parallax is how far the room shifts against
   a page transition; a handful of pixels is enough to separate the layers. */
#define PET_AMBIENT_SWEEP_MS 9000
#define PET_AMBIENT_MOTES    6
#define PET_AMBIENT_PARALLAX 6

/* Fonts. Both of these carry the full GB2312 set as well as ASCII, so either is safe for
   text that arrived from the model; the two chrome faces below are subsets and are not.
   CJK is the 18 px face the note page's prose is set in, CJK_SM the 14 px working face
   every row label, button caption and group heading is set in. */
#define PET_FONT_CJK    (&AlibabaPuHuiTi3_Regular18_Static)
#define PET_FONT_CJK_SM (&AlibabaPuHuiTi3_Regular14)

/* Chrome. Two subsets of the same typeface, the header's and a card's. Both carry every
   character this app writes into a header or a card title and nothing else — safe for the
   strings the pages spell out, not for a value that came from the model. */
#define PET_FONT_CJK_TITLE (&AlibabaPuHuiTi3_Medium20)
#define PET_FONT_CJK_CARD  (&AlibabaPuHuiTi3_Medium16)

/* Figures. 32 px semibold, a subset: `. / 0-9 :`, A-Z, a-z, and the ten date glyphs
   一二三四五六日星月期 plus 年月分秒. Not safe for arbitrary Chinese. line_height 34. */
#define PET_FONT_CLOCK (&AlibabaPuHuiTi3_SemiBold32)

/* Icons. 16 px is the working size (line_height 18); 20 px is for the one large
   glyph in an empty state (line_height 21). */
#define PET_FONT_ICON    (&font_awesome_16_4)
#define PET_FONT_ICON_LG (&font_awesome_20_4)

/* The CJK face reports line_height = 20, so anything that wraps text needs at least
   this much vertical room. */
#define PET_LINE_H 20

#ifdef __cplusplus
}
#endif

#endif /* __PET_THEME_H__ */
