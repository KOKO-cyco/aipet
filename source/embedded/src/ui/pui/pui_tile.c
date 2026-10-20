/**
 * @file pui_tile.c
 * @brief The tile — a coloured icon square over a caption — and the camera drawn into it.
 *
 * This shape appeared in four places before this file existed: the home page's three
 * entries, the gallery's photo thumbnails, the gallery's "add" slot, and the note page's
 * image picker. They were four copies of the same forty lines with different numbers in
 * them, and the numbers were the point — each copy had to be re-measured whenever the
 * panel or the padding moved.
 *
 * Here the tile sizes itself. The square is PUI_TILE_ICON, the caption is whatever the
 * text comes to, and the tile is the two plus the gap. No caller writes a tile height.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pui_priv.h"

/***********************************************************
***********************variable define**********************
***********************************************************/

#define MY_CLASS (&pui_tile_class)

/***********************************************************
********************function define**************************
***********************************************************/

/**
 * @brief Paint the square from the tile's tone and fill, plus its selection ring.
 */
static void __tile_box_apply(lv_obj_t *obj)
{
    pui_tile_t        *tile  = (pui_tile_t *)obj;
    const PUI_PAINT_T *paint = pui_paint(tile->tone, tile->fill);
    lv_color_t         deep  = pui_tone_deep(tile->tone);

    pui_priv_surface(tile->box, paint, PET_RADIUS_PILL);

    /* Every square gets a same-hue hairline, one step away from its own fill — the same
       trick a card's border plays, one size down. Without it a tinted square on a white
       card is a stain with a glyph floating in it; with it the square is a thing with an
       edge, which is most of what separates a chip from a placeholder.

       The step is *down* the hue on a solid fill and *up* it on a wash. A saturated square
       wants a deeper edge, the way the reference's icons have one; a pastel square already
       sits on white, and a deeper edge there would draw a hard outline round a soft fill. */
    if (paint->line_w == 0) {
        lv_obj_set_style_border_width(tile->box, 1, 0);
        lv_obj_set_style_border_color(tile->box, tile->fill == PUI_FILL_SOLID
                                                      ? pui_tone_deep(tile->tone)
                                                      : pui_tint_fill(pui_tone_hue(tile->tone)),
                                      0);
        lv_obj_set_style_border_opa(tile->box, LV_OPA_COVER, 0);
    }

    if (tile->graded) {
        /* A tile standing in for a photograph, graded from its light wash down to a
           deeper one so it reads as a lit surface rather than as a flat swatch. Only ever
           on a tile with no camera in it — see pui_tile_set_graded(). The grade runs the
           full span between the two tint steps: at half of it the block still read as one
           flat colour with a slightly dirty foot. */
        lv_color_t from = pui_tint(pui_tone_hue(tile->tone));
        lv_color_t to   = pui_tone_wash(tile->tone, PET_TINT_FILL - PUI_TILE_DEEP);

        lv_obj_set_style_bg_color(tile->box, from, 0);
        lv_obj_set_style_bg_grad_color(tile->box, to, 0);
        lv_obj_set_style_bg_grad_dir(tile->box, LV_GRAD_DIR_VER, 0);
    } else if (!tile->wide && tile->fill == PUI_FILL_SOFT) {
        /* **纯色，不做渐变**：这块软色块每页要画好几个（首页三个入口），渐变在
         * 软件渲染里是逐像素混合，页面切换/大范围重绘时是实打实的开销；暗色主题
         * 下两级色差本来也看不出来。 */
        lv_obj_set_style_bg_color(tile->box, pui_tone_wash(tile->tone, 132), 0);
        lv_obj_set_style_bg_grad_dir(tile->box, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_border_color(tile->box, pui_tone_wash(tile->tone, 100), 0);
    } else {
        lv_obj_set_style_bg_grad_dir(tile->box, LV_GRAD_DIR_NONE, 0);
    }

    if (tile->selected) {
        lv_obj_set_style_border_width(tile->box, PUI_TILE_RING, 0);
        lv_obj_set_style_border_color(tile->box, deep, 0);
        lv_obj_set_style_border_opa(tile->box, LV_OPA_COVER, 0);
    }

    if (tile->cap != NULL) {
        lv_obj_set_style_text_color(tile->cap, tile->selected ? deep : PET_COLOR_TEXT, 0);
    }
}

/**
 * @brief Put the tile's own size and the square's back the way the constructor left them.
 *
 * One place for both the wide and the square case, so the two can never disagree about
 * which of them owns the width.
 */
static void __tile_size_apply(lv_obj_t *obj)
{
    pui_tile_t *tile  = (pui_tile_t *)obj;
    BOOL_T      shown = (tile->cap != NULL) && !lv_obj_has_flag(tile->cap, LV_OBJ_FLAG_HIDDEN);

    if (tile->wide) {
        /* Fills whatever the parent gives it. In a grid that is the whole track, because
           the grid places its children with LV_GRID_ALIGN_STRETCH — see __grid_place() in
           pui_box.c — so the tile is never asked how wide it wants to be.

           The height is written out rather than left as LV_SIZE_CONTENT, which is the one
           place in this file where a component names its own arithmetic. A grid row of
           LV_GRID_CONTENT is measured from lv_obj_get_height() of the tiles in it
           (lv_grid.c:404), and that reads coords — which are the *previous* pass's, since
           the box is resized after the tile is built. Left to content, a picker tile in a
           sheet came out at PUI_TILE_ICON's 44 + the caption, the height it had as a
           square before pui_tile_set_box_h() ran, and the caption then hung below the tile
           and outside its touch area. The square case below has no such problem: its box
           is sized once, in the constructor, before anything measures it. */
        lv_obj_set_width(obj, LV_PCT(100));
        lv_obj_set_size(tile->box, LV_PCT(100), tile->box_h);
        lv_obj_set_height(obj,
                          shown ? tile->box_h + PUI_TILE_GAP + lv_font_get_line_height(PET_FONT_CJK_SM) : tile->box_h);
    } else {
        lv_obj_set_width(obj, LV_SIZE_CONTENT);
        lv_obj_set_size(tile->box, PUI_TILE_ICON, PUI_TILE_ICON);
        lv_obj_set_height(obj, LV_SIZE_CONTENT);
    }

    if (tile->cap != NULL) {
        /* As wide as its text, centred by the tile's own alignment — not a percentage of
           the tile. A caption given the tile's width wraps when it is a few pixels too
           long for it, and "购物小票" breaking after the third character to make room for
           the fourth reads as a mistake; the same text overflowing into the gap beside
           its cell by two pixels does not. One line always, which is also what lets the
           height above be a sum of two known terms. */
        lv_obj_set_width(tile->cap, LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(tile->cap, tile->wide ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    }
}

static void __tile_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    pui_tile_t *tile = (pui_tile_t *)obj;

    LV_UNUSED(class_p);

    pui_priv_container(obj);

    tile->tone     = PUI_TONE_NEUTRAL;
    tile->fill     = PUI_FILL_SOFT;
    tile->graded   = 0;
    tile->wide     = 0;
    tile->selected = 0;
    tile->box_h    = PUI_TILE_PHOTO_H;

    /* The whole tile is the touch target, caption included, so a tap anywhere in the
       column opens the page. The square and the caption inside it are not clickable, so
       the press lands on the tile itself rather than on whichever part was touched. */
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(obj, PUI_TILE_GAP, 0);

    /* The square is its own object so the tile's spacing does not shrink the fill: a
       background on the tile itself would sit under the caption as well, and the two
       together read as one tall pill rather than as an icon with a name under it. */
    tile->box = lv_obj_create(obj);
    pui_priv_container(tile->box);
    lv_obj_set_style_radius(tile->box, PET_RADIUS_PILL, 0);

    /* Created empty and filled in by the caller. A hidden caption is dropped from the
       flex layout entirely rather than merely made invisible, which is what makes a
       caption-less tile come out as a bare square instead of a square over a gap. */
    tile->cap = pui_label_create(obj, "", PET_FONT_CJK_SM, PET_COLOR_TEXT);
    lv_obj_add_flag(tile->cap, LV_OBJ_FLAG_HIDDEN);

    __tile_size_apply(obj);

    pui_priv_pressable(obj);
}

const lv_obj_class_t pui_tile_class = {
    .constructor_cb = __tile_constructor,
    .instance_size  = sizeof(pui_tile_t),
    .base_class     = &lv_obj_class,
    .name           = "pui_tile",
};

lv_obj_t *pui_tile_create(lv_obj_t *parent, const char *glyph, PUI_TONE_E tone, PUI_FILL_E fill, const char *label)
{
    lv_obj_t   *obj = lv_obj_class_create_obj(MY_CLASS, parent);
    pui_tile_t *tile;

    lv_obj_class_init_obj(obj);

    tile       = (pui_tile_t *)obj;
    tile->tone = tone;
    tile->fill = fill;

    if (glyph != NULL) {
        lv_obj_t *icon = pui_label_create(tile->box, glyph, PET_FONT_ICON_LG, pui_paint(tone, fill)->fg);

        lv_obj_center(icon);
    }

    pui_tile_set_label(obj, label);
    __tile_box_apply(obj);

    return obj;
}

lv_obj_t *pui_tile_box(lv_obj_t *tile)
{
    if (tile == NULL || !lv_obj_check_type(tile, MY_CLASS)) {
        return NULL;
    }

    return ((pui_tile_t *)tile)->box;
}

void pui_tile_set_graded(lv_obj_t *tile, BOOL_T on)
{
    if (tile == NULL || !lv_obj_check_type(tile, MY_CLASS)) {
        return;
    }

    ((pui_tile_t *)tile)->graded = on ? 1 : 0;
    __tile_box_apply(tile);
}

void pui_tile_set_wide(lv_obj_t *tile, BOOL_T on)
{
    if (tile == NULL || !lv_obj_check_type(tile, MY_CLASS)) {
        return;
    }

    ((pui_tile_t *)tile)->wide = on ? 1 : 0;
    __tile_size_apply(tile);
}

void pui_tile_set_box_h(lv_obj_t *tile, lv_coord_t h)
{
    if (tile == NULL || !lv_obj_check_type(tile, MY_CLASS)) {
        return;
    }

    ((pui_tile_t *)tile)->box_h = h;
    __tile_size_apply(tile);
}

void pui_tile_set_selected(lv_obj_t *tile, BOOL_T on)
{
    pui_tile_t *t;

    if (tile == NULL || !lv_obj_check_type(tile, MY_CLASS)) {
        return;
    }

    t           = (pui_tile_t *)tile;
    t->selected = on ? 1 : 0;

    /* __tile_box_apply() repaints the square from the fill before it draws the ring, so
       turning the selection off restores the fill's own line — a hairline for OUTLINE,
       nothing at all for the rest — without a second path to keep in step. */
    __tile_box_apply(tile);
}

void pui_tile_set_label(lv_obj_t *tile, const char *label)
{
    pui_tile_t *t;

    if (tile == NULL || !lv_obj_check_type(tile, MY_CLASS)) {
        return;
    }

    t = (pui_tile_t *)tile;
    if (t->cap == NULL) {
        return;
    }

    if (label == NULL || label[0] == '\0') {
        lv_obj_add_flag(t->cap, LV_OBJ_FLAG_HIDDEN);
        /* A wide tile's height is the block plus the caption, and a caption that has just
           been dropped is no longer part of that sum. */
        __tile_size_apply(tile);
        return;
    }

    lv_label_set_text(t->cap, label);
    lv_obj_remove_flag(t->cap, LV_OBJ_FLAG_HIDDEN);
    __tile_size_apply(tile);
}

static lv_obj_t *__glyph_part(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_coord_t radius, lv_color_t color)
{
    lv_obj_t *part = lv_obj_create(parent);

    pui_priv_container(part);
    lv_obj_set_size(part, w, h);
    lv_obj_set_style_radius(part, radius, 0);
    lv_obj_set_style_bg_color(part, color, 0);
    lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
    return part;
}

void pui_camera_glyph(lv_obj_t *box, lv_coord_t size, lv_color_t color)
{
    lv_obj_t *body;
    lv_obj_t *lens;
    lv_obj_t *part;
    lv_coord_t w = size * 78 / 100;
    lv_coord_t h = size * 58 / 100;

    if (box == NULL) {
        return;
    }

    part = __glyph_part(box, size / 3, size / 6, 2, color);
    lv_obj_align(part, LV_ALIGN_CENTER, 0, -h / 2);
    body = __glyph_part(box, w, h, 5, lv_color_hex(0xF5F3F0));
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, color, 0);
    lv_obj_set_style_bg_grad_color(body, lv_color_hex(0xCBCDD5), 0);
    lv_obj_set_style_bg_grad_dir(body, LV_GRAD_DIR_VER, 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 2);
    lens = __glyph_part(body, size / 2, size / 2, LV_RADIUS_CIRCLE, lv_color_hex(0x4D536A));
    lv_obj_center(lens);
    part = __glyph_part(lens, size / 3, size / 3, LV_RADIUS_CIRCLE, lv_color_hex(0x91B7DF));
    lv_obj_set_style_border_width(part, 2, 0);
    lv_obj_set_style_border_color(part, lv_color_hex(0x313748), 0);
    lv_obj_center(part);
    part = __glyph_part(lens, 3, 3, LV_RADIUS_CIRCLE, PET_COLOR_CARD);
    lv_obj_align(part, LV_ALIGN_CENTER, -2, -2);
    part = __glyph_part(body, 4, 2, 1, PET_COLOR_CARD);
    lv_obj_align(part, LV_ALIGN_TOP_LEFT, 3, 3);
}

void pui_note_glyph(lv_obj_t *box, lv_coord_t size, lv_color_t color)
{
    lv_obj_t *paper;
    lv_obj_t *part;

    if (box == NULL) {
        return;
    }

    paper = __glyph_part(box, size * 60 / 100, size * 74 / 100, 3, PET_COLOR_CARD);
    lv_obj_set_style_border_width(paper, 1, 0);
    lv_obj_set_style_border_color(paper, pui_tint_fill(color), 0);
    lv_obj_align(paper, LV_ALIGN_CENTER, -2, 0);
    for (int i = 0; i < 3; i++) {
        part = __glyph_part(paper, size / 3, 1, 0, pui_tint_fill(color));
        lv_obj_set_pos(part, 3, 7 + i * 5);
    }
    part = __glyph_part(box, 5, size * 70 / 100, 2, color);
    lv_obj_set_style_bg_grad_color(part, lv_color_hex(0xEAD29B), 0);
    lv_obj_set_style_bg_grad_dir(part, LV_GRAD_DIR_HOR, 0);
    lv_obj_align(part, LV_ALIGN_CENTER, size / 5, 0);
    lv_obj_set_style_transform_rotation(part, 300, 0);
}

void pui_gear_glyph(lv_obj_t *box, lv_coord_t size, lv_color_t color)
{
    static const int8_t offsets[8][2] = {{0, -10}, {7, -7}, {10, 0}, {7, 7}, {0, 10}, {-7, 7}, {-10, 0}, {-7, -7}};
    lv_obj_t *part;
    lv_obj_t *ring;
    lv_coord_t d = size * 60 / 100;

    if (box == NULL) {
        return;
    }
    for (int i = 0; i < 8; i++) {
        part = __glyph_part(box, 6, 8, 2, PET_COLOR_CARD);
        lv_obj_align(part, LV_ALIGN_CENTER, offsets[i][0] * size / 36, offsets[i][1] * size / 36);
        lv_obj_set_style_transform_rotation(part, i * 450, 0);
    }
    ring = __glyph_part(box, d, d, LV_RADIUS_CIRCLE, PET_COLOR_CARD);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, color, 0);
    lv_obj_center(ring);
    part = __glyph_part(ring, d / 2, d / 2, LV_RADIUS_CIRCLE, pui_tone_wash(PUI_TONE_ACCENT, 140));
    lv_obj_set_style_border_width(part, 2, 0);
    lv_obj_set_style_border_color(part, color, 0);
    lv_obj_center(part);
}
