/**
 * @file pui_tone.c
 * @brief The colour system: six tones, four fills, and the table between them.
 *
 * This is the file that makes the rest of the library flexible. Everything else in
 * `src/pui/` asks pui_paint() what colour to be; nothing else names a colour. So
 * re-tinting the app is this table, and giving a component a new costume is a matter of
 * passing a different tone rather than of writing a second constructor.
 *
 * The table is filled at init and not statically, because `lv_color_hex()` is a
 * function in this build rather than a macro over a literal. A `static const
 * PUI_PAINT_T` with `lv_color_hex(...)` in its initialiser does not compile.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pui_priv.h"

/***********************************************************
***********************variable define**********************
***********************************************************/

/**
 * @brief A tone's three source colours, as plain hex.
 *
 * `hue` is what a call site means by "amber". `deep` is the same colour taken far enough
 * down that it can be read as text on `hue`'s own wash — amber at full strength on a
 * cream is about 1.8:1, legible as a shape and not as a word. `on_solid` is what goes on
 * top of a saturated fill.
 *
 * `deep` is picked by hand rather than mixed from the hue. Mixing toward black drains
 * the hue and mixing toward a neutral turns amber into mud; both were tried against the
 * reference design's numbers and both read as a different colour rather than as the same
 * one darker.
 */
typedef struct {
    uint32_t hue;
    uint32_t deep;
    uint32_t on_solid;
} TONE_SRC_T;

static const TONE_SRC_T sg_src[PUI_TONE_MAX] = {
    /* hue         deep        on_solid */
    {0x46536B, 0xC3CCDC, 0x0B101B}, /* NEUTRAL — secondary, inactive */
    {0x3D7BFF, 0x8FB4FF, 0xFFFFFF}, /* ACCENT  — the app's blue */
    {0x6C5CE7, 0xB4A8FF, 0xFFFFFF}, /* INDIGO  — settings, the note page */
    {0xF2A93B, 0xFFCC7A, 0x1A1206}, /* AMBER   — the to-do family */
    {0x2FBF8F, 0x6FE0B8, 0xFFFFFF}, /* GREEN   — done, working */
    {0xE5484D, 0xFF8F8B, 0xFFFFFF}, /* RED     — destructive, nothing else */
};

static lv_color_t  sg_hue[PUI_TONE_MAX];
static lv_color_t  sg_deep[PUI_TONE_MAX];
static lv_color_t  sg_on_solid[PUI_TONE_MAX];
static PUI_PAINT_T sg_paint[PUI_TONE_MAX][PUI_FILL_MAX];

static lv_style_t                sg_style_pressed;
static lv_style_t                sg_style_pressed_flat;
static lv_style_transition_dsc_t sg_pressed_tr;
static lv_style_transition_dsc_t sg_pressed_flat_tr;
static BOOL_T                    sg_ready;

/***********************************************************
********************function define**************************
***********************************************************/

void pui_init(void)
{
    static const lv_style_prop_t props[] = {
        LV_STYLE_OPA,        LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, LV_STYLE_SHADOW_WIDTH,
        LV_STYLE_SHADOW_OPA, LV_STYLE_SHADOW_OFFSET_Y,   LV_STYLE_PROP_INV};
    static const lv_style_prop_t props_flat[] = {LV_STYLE_OPA, LV_STYLE_SHADOW_WIDTH, LV_STYLE_SHADOW_OPA,
                                                 LV_STYLE_SHADOW_OFFSET_Y, LV_STYLE_PROP_INV};
    PUI_TONE_E                   tone;
    PUI_FILL_E                   fill;

    if (sg_ready) {
        return;
    }

    /* Set before anything else: a component constructed during a page build calls
       pui_init() itself, and two of them can arrive here in the same frame. */
    sg_ready = TRUE;

    for (tone = 0; tone < PUI_TONE_MAX; tone++) {
        sg_hue[tone]      = lv_color_hex(sg_src[tone].hue);
        sg_deep[tone]     = lv_color_hex(sg_src[tone].deep);
        sg_on_solid[tone] = lv_color_hex(sg_src[tone].on_solid);
    }

    for (tone = 0; tone < PUI_TONE_MAX; tone++) {
        lv_color_t wash = pui_tint(sg_hue[tone]);

        for (fill = 0; fill < PUI_FILL_MAX; fill++) {
            PUI_PAINT_T *p = &sg_paint[tone][fill];

            p->bg     = wash;
            p->fg     = sg_deep[tone];
            p->line   = sg_hue[tone];
            p->bg_opa = LV_OPA_COVER;
            p->line_w = 0;

            switch (fill) {
            case PUI_FILL_SOLID:
                /* The only fill where the foreground is not the deep colour: on a
                   saturated field the deep colour has no contrast left to give. */
                p->bg = sg_hue[tone];
                p->fg = sg_on_solid[tone];
                break;

            case PUI_FILL_SOFT:
                /* One step deeper than a card's own wash. A soft fill is a *thing on* a
                   tinted card — the card's footer button — and at the card's own value it
                   was the same colour as the card it sat on, which is to say invisible.
                   See PET_TINT_FILL. */
                p->bg = pui_tint_fill(sg_hue[tone]);
                break;

            case PUI_FILL_OUTLINE:
                p->bg     = PET_COLOR_CARD;
                p->line   = pui_tone_deep(tone);
                p->line_w = 1;
                break;

            case PUI_FILL_GHOST:
            default:
                p->bg     = PET_COLOR_CARD;
                p->bg_opa = LV_OPA_TRANSP;
                break;
            }
        }
    }

    lv_style_transition_dsc_init(&sg_pressed_tr, props, lv_anim_path_ease_out, PET_PRESS_MS, 0, NULL);
    lv_style_init(&sg_style_pressed);
    lv_style_set_opa(&sg_style_pressed, LV_OPA_70);
    /* The pivot has to be named. LVGL's default for it is (0, 0) — unlike transform_scale,
       whose default is LV_SCALE_NONE (lv_style_prop_get_default()) — so a squash written
       without one shrinks a button toward its own top-left corner, and what the user sees
       is the label sliding rather than the button pressing. */
    lv_style_set_transform_pivot_x(&sg_style_pressed, LV_PCT(50));
    lv_style_set_transform_pivot_y(&sg_style_pressed, LV_PCT(50));
    lv_style_set_transform_scale_x(&sg_style_pressed, PUI_SQUASH);
    lv_style_set_transform_scale_y(&sg_style_pressed, PUI_SQUASH);
    /* The shadow converges on the press. Take away a resting shadow (see
       pui_priv_press_shadow) and the button has nowhere to sink to; collapse it while
       squashing and the button reads as going *into* the surface it was floating over
       rather than as shrinking where it stood. */
    lv_style_set_shadow_width(&sg_style_pressed, 0);
    lv_style_set_shadow_opa(&sg_style_pressed, LV_OPA_TRANSP);
    lv_style_set_transition(&sg_style_pressed, &sg_pressed_tr);

    /* The dim on its own, for a component whose press must not scale it. Its own props list
       rather than the one above: a transition only animates the properties it is given, and
       handing this style the list with the scales in it would make it animate a property
       its own rule says it does not set. */
    lv_style_transition_dsc_init(&sg_pressed_flat_tr, props_flat, lv_anim_path_ease_out, PET_PRESS_MS, 0, NULL);
    lv_style_init(&sg_style_pressed_flat);
    lv_style_set_opa(&sg_style_pressed_flat, LV_OPA_70);
    /* A card is not squashed, but it does have a shadow to give up. Same convergence as
       the leaf style; the props list carries only what this style actually sets. */
    lv_style_set_shadow_width(&sg_style_pressed_flat, 0);
    lv_style_set_shadow_opa(&sg_style_pressed_flat, LV_OPA_TRANSP);
    lv_style_set_transition(&sg_style_pressed_flat, &sg_pressed_flat_tr);
}

lv_color_t pui_tint(lv_color_t c)
{
    /* Dark UI: a "wash" is the hue mixed into the **dark neutral surface**, not into
       white. 200 of 255 neutral leaves the hue about a fifth of the way up — enough to
       read as a colour, little enough that light text on it stays the highest-contrast
       thing in the tile. */
    return lv_color_mix(PET_COLOR_NEUTRAL, c, PET_TINT_MIX);
}

lv_color_t pui_tint_fill(lv_color_t c)
{
    return lv_color_mix(PET_COLOR_NEUTRAL, c, PET_TINT_FILL);
}

lv_color_t pui_tone_wash(PUI_TONE_E tone, uint8_t mix)
{
    pui_init();

    return lv_color_mix(PET_COLOR_NEUTRAL,
                        sg_hue[(tone >= 0 && tone < PUI_TONE_MAX) ? tone : PUI_TONE_NEUTRAL], mix);
}

lv_color_t pui_tone_hue(PUI_TONE_E tone)
{
    pui_init();
    return sg_hue[(tone >= 0 && tone < PUI_TONE_MAX) ? tone : PUI_TONE_NEUTRAL];
}

lv_color_t pui_tone_deep(PUI_TONE_E tone)
{
    pui_init();
    return sg_deep[(tone >= 0 && tone < PUI_TONE_MAX) ? tone : PUI_TONE_NEUTRAL];
}

const PUI_PAINT_T *pui_paint(PUI_TONE_E tone, PUI_FILL_E fill)
{
    pui_init();

    /* Clamped rather than asserted: this build has LV_USE_ASSERT_OBJ 0 and no console on
       the device, so a wrong colour is a better failure than a read past the table. */
    if (tone < 0 || tone >= PUI_TONE_MAX) {
        tone = PUI_TONE_NEUTRAL;
    }
    if (fill < 0 || fill >= PUI_FILL_MAX) {
        fill = PUI_FILL_SOFT;
    }

    return &sg_paint[tone][fill];
}

/***********************************************************
*********************priv function**************************
***********************************************************/

void pui_priv_container(lv_obj_t *obj)
{
    pui_init();

    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);

    /* pad_all() does not cover pad_row/pad_column: in a flex container those are the gaps
       between the children, and the default theme sets pad_row to 10 on every plain
       lv_obj (lv_theme_default.c, `styles.pad_gap`). A hand-made column that skipped these
       two lines therefore had a 10 px gap between its children that nothing in this
       library had asked for — which is where the space between the status strip and the
       page below it came from. A component that sets its own gap does so after calling
       this, so the ones here are defaults, not overrides. */
    lv_obj_set_style_pad_row(obj, 0, 0);
    lv_obj_set_style_pad_column(obj, 0, 0);
}

void pui_priv_pressable(lv_obj_t *obj)
{
    pui_init();
    lv_obj_add_style(obj, &sg_style_pressed, LV_STATE_PRESSED);
}

void pui_priv_pressable_flat(lv_obj_t *obj)
{
    pui_init();
    lv_obj_add_style(obj, &sg_style_pressed_flat, LV_STATE_PRESSED);
}

void pui_priv_surface(lv_obj_t *obj, const PUI_PAINT_T *paint, lv_coord_t radius)
{
    lv_obj_set_style_bg_color(obj, paint->bg, 0);
    lv_obj_set_style_bg_opa(obj, paint->bg_opa, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, paint->line_w, 0);
    lv_obj_set_style_border_color(obj, paint->line, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
}

void pui_priv_press_shadow(lv_obj_t *obj)
{
    /* The resting half of the press: a shallow, warm shadow just under a solid button, so
       it sits a hair above the page. Small and low-opacity on purpose — this is an
       affordance, not elevation. Only solid fills call it; a ghost or an outline has no
       fill for a shadow to gather under and would show a floating dark rectangle. */
    lv_obj_set_style_shadow_width(obj, PUI_PRESS_SHADOW_W, 0);
    lv_obj_set_style_shadow_offset_y(obj, PUI_PRESS_SHADOW_DY, 0);
    lv_obj_set_style_shadow_color(obj, PET_COLOR_SHADOW, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_30, 0);
}

void pui_pressable(lv_obj_t *obj)
{
    pui_priv_pressable(obj);
}

void pui_pressable_flat(lv_obj_t *obj)
{
    pui_priv_pressable_flat(obj);
}
