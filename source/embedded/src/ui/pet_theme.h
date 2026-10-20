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

/* 字体来自 SD 卡（app_font_puhui），不再编进固件 —— 六个字号的 C 源加起来
 * 7.5MB，比整个 app 分区还大。见 app_font.h。 */
#include "app_font.h"

/* The FONT_AWESOME_* glyph macros, and the two icon faces. These stay compiled
   in: 149KB together, and the whole UI's iconography is in them. */
#include "font_awesome_symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
***********************variable define**********************
***********************************************************/

/* 图标字体编进固件（149KB）；中文字体全部走 SD 卡。 */
LV_FONT_DECLARE(font_awesome_16_4);
LV_FONT_DECLARE(font_awesome_20_4);

/***********************************************************
************************macro define************************
***********************************************************/

/* Surfaces. Dark UI（参考 third_party/ai_pet_wide/4页面暗色UI.png）：深海军蓝的
   页面、略亮一档的卡片、再亮一档的次级表面。文字全部提亮，强调色是亮蓝。

   light 那套的命名保留不变，只换值 —— 所有组件都只读这些 token。 */
#define PET_COLOR_BG      lv_color_hex(0x0B101B)
#define PET_COLOR_CARD    lv_color_hex(0x151C2A)
#define PET_COLOR_NEUTRAL lv_color_hex(0x1B2434)

#define PET_COLOR_ROW lv_color_hex(0x1A2130)

/* 宠物卡比页面亮一点点（并偏蓝），角色像站在一个自己的房间里。 */
#define PET_COLOR_PET_COLUMN lv_color_hex(0x0E1626)

/* The indigo family. It carries the second half of the design: the note page's
   surface, the settings groups' headers, and the AI action. Kept separate from the
   blue accent so the two never have to be told apart by shade alone. */

/* The home page's entry tiles and the camera's shutter. Each tile owns a colour so
   they are told apart at a glance rather than by reading them. These are the *icon*
   colours: the square behind the glyph is a tint of the same hue, not a fill of it —
   see pui_tint(). */

#define PET_COLOR_ORANGE lv_color_hex(0xF4A18C)

/* The viewfinder, and the sticky note pinned into it. The preview is standing in for a
   camera image — on a dark page it is the *darkest* surface, and its chip is lifted a
   step for the one control that sits on top of it. The sticky note stays paper-yellow:
   it is a physical object in the picture, not part of the UI's dark scheme. */
#define PET_COLOR_PREVIEW      lv_color_hex(0x0E131C)
#define PET_COLOR_PREVIEW_CHIP lv_color_hex(0x232C3E)
#define PET_COLOR_STICKY       lv_color_hex(0xFBE8B8)

/* Text */
#define PET_COLOR_TEXT      lv_color_hex(0xE9EEF8)
#define PET_COLOR_TEXT_SUB  lv_color_hex(0x9AA6BC)
#define PET_COLOR_TEXT_HINT lv_color_hex(0x6F7A90)
#define PET_COLOR_ON_ACCENT lv_color_hex(0xFFFFFF)

/* Lines and accents. On the dark page a hairline has to be *lighter* than the card it
   divides, or it disappears; LINE_STRONG is the outline that also sits against the page. */
#define PET_COLOR_LINE        lv_color_hex(0x263146)
#define PET_COLOR_LINE_STRONG lv_color_hex(0x33415C)
#define PET_COLOR_ACCENT      lv_color_hex(0x3D7BFF)
#define PET_COLOR_ACCENT_SOFT lv_color_hex(0x16294D)
#define PET_COLOR_SUCCESS     lv_color_hex(0x35D0A0)

#define PET_COLOR_WARN lv_color_hex(0x4A3A1E)

/* Overlays. A near-black scrim, a bit colder than the page so it reads as shade. */
#define PET_COLOR_SCRIM lv_color_hex(0x05080F)

/* What a card casts: black. On a dark page a shadow is almost invisible by nature, so
   it is kept for the little separation it still gives at the card's edge. */
#define PET_COLOR_SHADOW lv_color_hex(0x000000)

/* How far a tint of a tone is pushed toward the **dark surface** (PET_COLOR_NEUTRAL),
   not toward white. Two steps, because a surface and a fill are different jobs:

   - PET_TINT_MIX is what a *card* is washed with: mostly the dark neutral, a hint of
     the hue — a colour you can name that stays dark enough for light text on it.
   - PET_TINT_FILL is what a *fill* is made of — a soft button, a tile's icon square.
     It carries more of the hue, so a soft fill does not vanish into the card it sits on. */
#define PET_TINT_MIX  200
#define PET_TINT_FILL 160

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

/* What a card casts. **Off (0) on this build**：LV_DRAW_SW_SHADOW_CACHE_SIZE 是 0，
   软件渲染器每重绘一次卡片就把整块阴影模糊重算一遍 —— 宠物每 42ms 脏一块画布，
   底下那张卡的阴影就跟着重算一次，实测把刷新从 8~10ms 顶到 20ms 以上。
   暗色主题下黑色阴影本来也几乎看不见，卡片靠 1px 细边和色差分界就够了。
   想要回阴影就把 W 改回 8、OPA 改成 LV_OPA_20。 */
#define PET_SHADOW_W  0
#define PET_SHADOW_DY 3
#define PET_SHADOW_OPA LV_OPA_TRANSP

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
   real sprite now, not around the demo's still: the sprite engine's anchor box is
   193x199 (see app_sprite2d_size()), and the card is 196 wide so the canvas fits
   inside its 1 px border with nothing clipped.

   Everything left over goes to the panel, which is where the room is wanted — the
   panel is the whole app. 480 - 2*12 - 196 - 10 = 250. */
#define PET_GAP_PANEL 10
#define PET_COLUMN_W  196
#define PET_PANEL_W   (PET_SCREEN_W - 2 * PET_PAD_PAGE - PET_COLUMN_W - PET_GAP_PANEL)

/* The gap between the status strip and everything below it. The strip's own height is
   APP_STATUSBAR_H, next to the component that draws it; this one is left here because
   the page shell in pet_ui.c pads the body down by it, and the shell is not a component.

   2 rather than 4: the pet card has to be tall enough for the sprite's 199 px anchor
   box plus the bubble holder above it, and the strip has no fill to separate it from
   the page — the extra two pixels were not buying anything. */
#define PET_STATUS_GAP 2

/* The character is the real animated sprite (app_sprite2d), drawn into an
   lv_canvas inside the card's stage. The stage is what is left of the column after
   the bubble holder; the canvas is the sprite's own anchor box, centred.

   PET_CHAR_TOP is the gap between the top of the stage and the canvas. The canvas
   itself is 193x199 at the current asset set — read at runtime, never assumed. */
#define PET_CHAR_TOP 2

/* 角色在卡里的**水平位置**：0 = 居中，负值向左。
 *
 * 画布（193）比舞台（194）窄一点，角色内容只有 ~129 宽，所以画布左右各有约 32px
 * 的透明边。左移只是把左边的透明边推出舞台（被舞台裁掉），右边露出的那条舞台
 * 底色和画布底色同色，看不出来 —— 角色本体不会被裁。想再往左就继续减。 */
#define PET_CHAR_DX (-18)

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

/* 六个字号，全部从 SD 卡加载（/sdcard/font/puhui<px>.bin）。这些宏展开成
   **函数调用**，返回的是 app_font.c 里地址恒定的代理字体 —— 所以字库后加载
   完成时，已经建好的界面会自动用上新字体，不用重建。

   app_font_puhui() 在对应字号缺失时会退到最接近的一档，所以少放一两份文件
   界面也不会崩，只是那几处字号不对。缺哪些、生成什么范围，见 app_font.h。 */
#define PET_FONT_CJK    app_font_puhui(18)   /* 正文：便签正文、列表长句 */
#define PET_FONT_CJK_SM app_font_puhui(14)   /* 工作字号：行标签、按钮、状态栏 */

/* Chrome：标题和卡片头。子集字号，只覆盖界面自己写死的那几个字。 */
#define PET_FONT_CJK_TITLE app_font_puhui(20)
#define PET_FONT_CJK_CARD  app_font_puhui(16)

/* 数字：卡片里那个大数字。子集字号（数字 + 冒号 + 日期用字）。 */
#define PET_FONT_CLOCK app_font_puhui(32)

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
