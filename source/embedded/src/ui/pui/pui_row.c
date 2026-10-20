/**
 * @file pui_row.c
 * @brief The two one-line components: a figure with a label, and an icon with a label.
 *
 * These are what a card is usually full of. The reference design's summary cards are a
 * heading band, two of these, and a button, and the reason they read as a summary rather
 * than as a paragraph is the mark at the head of each line.
 *
 * The stat row's figure is drawn in the app's 20 px medium face and its label in the 14 px
 * regular one, which is the hierarchy the card's 16 px heading band sits between. The 20 px
 * face carries digits, ASCII and ten Chinese characters; a value that is anything else
 * renders as placeholder boxes, so pass digits. The list row's mark is a square instead of
 * a figure, and carries a glyph the caller chooses.
 *
 * Both rows end in a label that grows to the width the row has left and elides into it.
 * That is not decoration: a card here is about 77 px of usable width, and a label whose
 * minimum width is its own text width makes the row wider than the card, at which point
 * the card's edge clips the text instead of the label's own ellipsis. A flex item with
 * `flex_grow` set makes the track take exactly the container's width (lv_flex.c:299), so
 * with the grow in place the row cannot overflow whatever the text says.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pui_priv.h"

/***********************************************************
***********************variable define**********************
***********************************************************/

#define MY_STAT (&pui_stat_row_class)
#define MY_LIST (&pui_list_row_class)

/* The face the figure is drawn in, and the gap between it and its label. Kept as names
   because the row's height is decided by this font's line height (26) and the whole home
   page's vertical budget is spent against it.

   24 rather than the 32 px clock face it used to be: the card is 111 px wide and the
   figure was taking a third of the row's height for one digit. At 24 the figure is still
   the largest thing on the card — one size above the heading band's 20 — and the 8 px a
   row it gives back is what pays for the band. */
#define STAT_VALUE_FONT PET_FONT_CJK_TITLE
#define STAT_GAP        6

/* A row's own vertical padding, above and below its line. The rows of a card are stacked
   with the card's own PUI_CARD_GAP between them and each row's hairline is drawn on its
   top edge, so this is the air under that hairline: with 4 the rule sits 8 px below the
   line above it and 5 px above the line below, which is as close to centred as a
   border-side divider gets. */
#define ROW_PAD_V 4

/* A list row's leading icon square, and its corner. The radius is the tile's PET_RADIUS_PILL
   taken down with the square: 14 on a 44 px square and 14 on a 22 px one are not the same
   shape, and the smaller one rounds all the way into a circle. */
#define LIST_ICON_R 7

/***********************************************************
********************stat row implementation*****************
***********************************************************/

static void __stat_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    pui_stat_row_t *row = (pui_stat_row_t *)obj;

    LV_UNUSED(class_p);

    pui_priv_container(obj);

    row->tone = PUI_TONE_NEUTRAL;

    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    /* Bottom-aligned: the two faces have different line heights and the figure's
       baseline is what the label should sit on, which at these sizes means aligning the
       two boxes' feet. */
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(obj, STAT_GAP, 0);
    lv_obj_set_style_pad_hor(obj, 2, 0);
    lv_obj_set_style_pad_ver(obj, ROW_PAD_V, 0);

    /* The divider, as a top-only border — the hairline that turns two lines of text into
       two rows of a table. Drawn on the top rather than the bottom edge because a row is
       built before whatever follows it, so at construction time it is always the last
       child; the *first* child is the one position that is known and does not change, and
       that is the position a divider is wrong for. Everything below the card's band is a
       row, so the first child is the first row, and a rule above it would only draw a
       second line under the band.

       The list row draws the same one in the same place. The two used to differ — a list
       row's separator was its own leading stripe — and the difference showed the moment two
       cards of different row types sat side by side. */
    if (lv_obj_get_index(obj) != 0) {
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_color(obj, PET_COLOR_LINE, 0);
        lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    }

    row->value = pui_label_create(obj, "", STAT_VALUE_FONT, PET_COLOR_TEXT);
    row->text  = pui_label_create(obj, "", PET_FONT_CJK_SM, PET_COLOR_TEXT);

    /* The figure is the row's point and keeps its full width; the label takes what is left
       and elides into it. Without this the label's minimum width is its text width, so a
       long one pushes the row past its card and gets clipped by the card's edge instead of
       by its own. */
    lv_obj_set_flex_grow(row->text, 1);
    lv_label_set_long_mode(row->text, LV_LABEL_LONG_DOT);
}

const lv_obj_class_t pui_stat_row_class = {
    .constructor_cb = __stat_constructor,
    .instance_size  = sizeof(pui_stat_row_t),
    .base_class     = &lv_obj_class,
    .name           = "pui_stat_row",
};

lv_obj_t *pui_stat_row_create(lv_obj_t *parent, const char *value, const char *text, PUI_TONE_E tone)
{
    lv_obj_t       *obj = lv_obj_class_create_obj(MY_STAT, parent);
    pui_stat_row_t *row;

    lv_obj_class_init_obj(obj);

    row       = (pui_stat_row_t *)obj;
    row->tone = tone;

    lv_label_set_text(row->value, value != NULL ? value : "");
    lv_label_set_text(row->text, text != NULL ? text : "");

    /* The figure carries the tone and the label does not: at this size a coloured label
       would be a second thing competing with the number, and the number is the point. The
       label is in the full text colour rather than the muted one it used to be — on a
       white card with a rule between the rows, a grey label and a grey rule are the same
       weight and the card reads as one grey mass. */
    lv_obj_set_style_text_color(row->value, PET_COLOR_TEXT, 0);

    return obj;
}

/***********************************************************
********************list row implementation*****************
***********************************************************/

static void __list_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    pui_list_row_t *row = (pui_list_row_t *)obj;

    LV_UNUSED(class_p);

    pui_priv_container(obj);

    row->tone  = PUI_TONE_NEUTRAL;
    row->glyph = NULL;
    row->trail = NULL;

    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, STAT_GAP, 0);
    lv_obj_set_style_pad_hor(obj, 2, 0);
    lv_obj_set_style_pad_ver(obj, ROW_PAD_V, 0);

    /* The same hairline the stat row draws, in the same place. A list row used to carry
       none, because its leading stripe was a border and an object has one border with one
       colour — the stripe *was* the separator. The square is a child object and leaves the
       border free, so the two row types now separate their rows the same way, which is
       what makes the two cards beside each other on the home page share a rhythm. */
    if (lv_obj_get_index(obj) != 0) {
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_color(obj, PET_COLOR_LINE, 0);
        lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    }

    /* The square is created empty and filled in by the caller, the way a tile's is: the
       drawn glyphs have no font and go in with lv_obj_create(). */
    row->box = lv_obj_create(obj);
    pui_priv_container(row->box);
    lv_obj_set_size(row->box, PUI_ROW_ICON, PUI_ROW_ICON);
    lv_obj_set_style_radius(row->box, LIST_ICON_R, 0);

    row->text = pui_label_create(obj, "", PET_FONT_CJK_SM, PET_COLOR_TEXT);

    /* Same reason as the stat row's label: grown to the leftover width and elided into it,
       so a long note name shortens instead of pushing the row out of its card. */
    lv_obj_set_flex_grow(row->text, 1);
    lv_label_set_long_mode(row->text, LV_LABEL_LONG_DOT);

    /* The time, at the far edge. Built hidden rather than on demand so a row that never
       gets one costs only the object, and so the text's grow sees the final arrangement —
       a label created later would reflow the whole row. Hidden labels take no space in a
       flex container, so a row without a time looks exactly as it did before. */
    row->trail = pui_label_create(obj, "", PET_FONT_CJK_SM, PET_COLOR_TEXT_HINT);
    lv_obj_add_flag(row->trail, LV_OBJ_FLAG_HIDDEN);
}

const lv_obj_class_t pui_list_row_class = {
    .constructor_cb = __list_constructor,
    .instance_size  = sizeof(pui_list_row_t),
    .base_class     = &lv_obj_class,
    .name           = "pui_list_row",
};

lv_obj_t *pui_list_row_create(lv_obj_t *parent, const char *glyph, PUI_TONE_E tone, const char *text)
{
    lv_obj_t       *obj = lv_obj_class_create_obj(MY_LIST, parent);
    pui_list_row_t *row;

    lv_obj_class_init_obj(obj);

    row       = (pui_list_row_t *)obj;
    row->tone = tone;

    /* The wash-and-hairline treatment, not the tile's gradient: a 22 px square is read at
       a glance and a gradient in one is just a smudge. The edge is a step up the hue rather
       than down it, the same call the tile makes on a pastel fill — a deeper edge on a pale
       square draws a hard outline round a soft thing. */
    lv_obj_set_style_bg_color(row->box, pui_tint(pui_tone_hue(tone)), 0);
    lv_obj_set_style_bg_opa(row->box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row->box, 1, 0);
    lv_obj_set_style_border_color(row->box, pui_tint_fill(pui_tone_hue(tone)), 0);
    lv_obj_set_style_border_opa(row->box, LV_OPA_COVER, 0);

    if (glyph != NULL) {
        row->glyph = pui_label_create(row->box, glyph, PET_FONT_ICON, pui_tone_deep(tone));
        lv_obj_center(row->glyph);
    }

    lv_label_set_text(row->text, text != NULL ? text : "");

    return obj;
}

void pui_list_row_set_trailing(lv_obj_t *row, const char *text)
{
    pui_list_row_t *r;

    if (row == NULL || !lv_obj_check_type(row, MY_LIST)) {
        return;
    }

    r = (pui_list_row_t *)row;

    if (r->trail == NULL) {
        return;
    }

    /* Empty means "no time", not "a time of zero width": hiding the label is what returns
       the width it would have taken to the name beside it. */
    if (text == NULL || text[0] == '\0') {
        lv_obj_add_flag(r->trail, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(r->trail, text);
    lv_obj_remove_flag(r->trail, LV_OBJ_FLAG_HIDDEN);
}
