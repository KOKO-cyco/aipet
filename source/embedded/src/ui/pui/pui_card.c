/**
 * @file pui_card.c
 * @brief The card: the surface everything in this app sits on.
 *
 * Two looks in one component, and the difference is *what the card is*. With a heading it
 * is an object on the page — white, hairline, shadow, and its tone carried by a full-bleed
 * band across its top. Without one it is a region of the page — the plain white card, or,
 * if it is given a tone, a wash of that tone that is separated by its own fill instead of
 * by a shadow.
 *
 * The card owns its own padding, its own corner radius and its own internal spacing, so a
 * page that puts something in a card never learns any of those numbers. That is what
 * retires `NOTE_SHEET_INNER` and the two other widths derived from a card's padding.
 *
 * Its own padding is zero and its body's is not, which is the one non-obvious thing here:
 * a heading band has to reach the card's edges, and a child of a padded object starts at
 * the padding. See pui_card_body().
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pui_priv.h"

/***********************************************************
***********************variable define**********************
***********************************************************/

#define MY_CLASS (&pui_card_class)

/* The gap inside the heading row. Nothing sits beside the heading any more, so this is
   only the row's own spacing — kept because the row is a box, and a box with no gap is a
   box that has to be changed the day something does go beside the heading. */
#define HEAD_GAP 6

/* The band's own vertical padding. The heading's line box is 22, so the strip is 32 —
   which is what a heading strip is: the text plus enough air to read as a strip rather
   than as a line of text with a coloured rectangle behind it. */
#define BAND_PAD_V 7

/***********************************************************
********************function define**************************
***********************************************************/

/**
 * @brief Put the card's fill, its edge and its band on, from its tone.
 *
 * A headed card is white and its tone is the band; a headless one takes the tone itself.
 * The two used to be one rule — a toned card was a wash of the tone, heading and all —
 * and the wash was the problem: a card the same colour as its own heading has no figure
 * and no ground, so the heading read as the card's first line of text rather than as its
 * name. The band gives the heading its own surface, which is most of what makes the card
 * read as a group of things rather than as a paragraph.
 *
 * The edge is a hairline rather than a wider shadow, and the two elevations are told apart
 * by *what* the edge is made of, not by whether there is one:
 *
 *   - a white card is an object on the page. It gets a cool hairline and the shadow.
 *   - a tinted card is a region of the page. It gets a hairline in its own hue, one step
 *     deeper than its own wash, and no shadow.
 *
 * A tinted card still takes no shadow on purpose — LV_DRAW_SW_SHADOW_CACHE_SIZE is 0 in
 * this build, so a blur is recomputed on every repaint with nothing amortising it, and the
 * settings page scrolls three of these at once.
 */
static void __card_apply(lv_obj_t *obj)
{
    pui_card_t *card = (pui_card_t *)obj;

    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);

    if (card->band != NULL || card->tone == PUI_TONE_NEUTRAL) {
        lv_obj_set_style_bg_color(obj, PET_COLOR_CARD, 0);
        lv_obj_set_style_border_color(obj, PET_COLOR_LINE, 0);
        lv_obj_set_style_shadow_width(obj, PET_SHADOW_W, 0);
        lv_obj_set_style_shadow_offset_y(obj, PET_SHADOW_DY, 0);
        lv_obj_set_style_shadow_color(obj, PET_COLOR_SHADOW, 0);
        lv_obj_set_style_shadow_opa(obj, PET_SHADOW_OPA, 0);
    } else {
        lv_obj_set_style_bg_color(obj, pui_tint(pui_tone_hue(card->tone)), 0);
        lv_obj_set_style_border_color(obj, pui_tint_fill(pui_tone_hue(card->tone)), 0);
        lv_obj_set_style_shadow_width(obj, 0, 0);
    }

    if (card->band != NULL) {
        lv_obj_set_style_bg_color(card->band, pui_tint(pui_tone_hue(card->tone)), 0);
        lv_obj_set_style_bg_opa(card->band, LV_OPA_COVER, 0);
    }
}

static void __card_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    pui_card_t *card = (pui_card_t *)obj;

    LV_UNUSED(class_p);

    pui_priv_container(obj);

    card->tone  = PUI_TONE_NEUTRAL;
    card->band  = NULL;
    card->head  = NULL;
    card->title = NULL;

    /* A card is a surface, not a target. A caller that wants the whole card to take a tap
       calls pui_card_set_action(), which adds LV_OBJ_FLAG_CLICKABLE back on. */
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(obj, PET_RADIUS_CARD, 0);
    /* No padding here. The heading band runs edge to edge, and a child of a padded object
       starts at the padding — so the padding lives on the body instead, and the band is
       the card's own child. See pui_card_body(). */
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_pad_row(obj, 0, 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    card->body = lv_obj_create(obj);
    pui_priv_container(card->body);
    lv_obj_set_width(card->body, LV_PCT(100));
    lv_obj_set_height(card->body, LV_SIZE_CONTENT);
    /* Grown so that a card told to fill its parent — the note page's surface — passes the
       extra height on to its contents rather than leaving a band of empty card under
       them. A no-op on a card that is only as tall as what is in it. */
    lv_obj_set_flex_grow(card->body, 0);
    lv_obj_set_style_radius(card->body, 0, 0);
    lv_obj_set_style_pad_all(card->body, PUI_CARD_PAD, 0);
    lv_obj_set_style_pad_row(card->body, PUI_CARD_GAP, 0);
    lv_obj_set_flex_flow(card->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card->body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
}

const lv_obj_class_t pui_card_class = {
    .constructor_cb = __card_constructor,
    .instance_size  = sizeof(pui_card_t),
    .base_class     = &lv_obj_class,
    .name           = "pui_card",
};

lv_obj_t *pui_card_create(lv_obj_t *parent, const char *title, PUI_TONE_E tone)
{
    lv_obj_t   *obj = lv_obj_class_create_obj(MY_CLASS, parent);
    pui_card_t *card;

    lv_obj_class_init_obj(obj);

    card       = (pui_card_t *)obj;
    card->tone = tone;

    if (title != NULL) {
        /* The band is the card's first child, and the body was created in the
           constructor, so the two have to be swapped. */
        card->band = lv_obj_create(obj);
        pui_priv_container(card->band);
        lv_obj_set_size(card->band, LV_PCT(100), LV_SIZE_CONTENT);

        card->head = pui_box_create(card->band, PUI_FLOW_ROW, HEAD_GAP);
        lv_obj_set_flex_align(card->head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* After the box, not before: pui_box_create() puts its own container defaults on
           the parent it is given, and those include pad_all(0). */
        lv_obj_set_style_pad_hor(card->band, PUI_CARD_PAD, 0);
        lv_obj_set_style_pad_ver(card->band, BAND_PAD_V, 0);

        /* The heading is dark, not muted, and that is the band's whole point: on a tinted
           card the heading and the contents used to be the same weight of grey, and the
           card read as five lines of text. Against the band the heading is the darkest
           thing on the card and the reader's eye lands on the name before the rows. */
        card->title = pui_text_create(card->head, title, PUI_TEXT_CARD);
        lv_obj_set_flex_grow(card->title, 1);
        lv_label_set_long_mode(card->title, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(card->title, PET_COLOR_TEXT, 0);
        lv_obj_set_style_text_letter_space(card->title, 0, 0);

        /* LVGL clips a child to its parent's *box*, not to its corners, so without this the
           band's square top corners sit on top of the card's curve and the card reads as a
           square with a rounded bottom. Only worth its cost on a card that has a band; a
           plain card's children are inside its padding and never reach the curve. */
        lv_obj_set_style_clip_corner(obj, TRUE, 0);

        lv_obj_move_to_index(card->band, 0);
    }

    __card_apply(obj);

    return obj;
}

lv_obj_t *pui_card_body(lv_obj_t *card)
{
    if (card == NULL || !lv_obj_check_type(card, MY_CLASS)) {
        return NULL;
    }

    return ((pui_card_t *)card)->body;
}

void pui_card_set_action(lv_obj_t *card, lv_event_cb_t cb, void *user_data)
{
    if (card == NULL || !lv_obj_check_type(card, MY_CLASS)) {
        return;
    }

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    /* Flat, not the shared pressable: a card is a surface with contents on it, and the
       squash is for leaves. See pui_priv_pressable_flat(). */
    pui_priv_pressable_flat(card);

    if (cb != NULL) {
        lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, user_data);
    }
}

void pui_card_set_tone(lv_obj_t *card, PUI_TONE_E tone)
{
    if (card == NULL || !lv_obj_check_type(card, MY_CLASS)) {
        return;
    }

    ((pui_card_t *)card)->tone = tone;
    __card_apply(card);
}
