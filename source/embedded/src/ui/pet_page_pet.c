/**
 * @file pet_page_pet.c
 * @brief The character's card: the speech bubble above, the sprite below.
 *
 *     +-------------+
 *     | +---------+ |
 *     | |  说……   | |  bubble
 *     | +----\----+ |
 *     |      \      |  tail
 *     |   (character)|  stage
 *     |             |
 *     +-------------+
 *
 * Built once, into the shell's left card, and never torn down — the panel beside it is
 * what changes. That is why nothing here has to survive being deleted and rebuilt: the
 * bubble keeps its line across every navigation because the object carrying it is still
 * the same one.
 *
 * The status strip is not here. It belongs to the screen rather than to this card, and
 * lives in pet_ui.c with the rest of the shell.
 *
 * The bubble fades in and out rather than appearing and disappearing, and pet_say() is
 * what drives it.
 *
 * It also comes in one shape per tone: a plain bubble, a rounder one for a pleased line,
 * one trailing bubbles for a thinking line, and one sharpened to a barb for a refusal.
 * See PET_BUBBLE_SHAPE_E.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_pages.h"

#include "pet_demo.h"
#include "pet_pet.h"
#include "pui.h"

#include <string.h>

/***********************************************************
************************macro define************************
***********************************************************/

/* Room for the longest bubble in the UI, with headroom. Not PET_TEXT_MAX: the
   bubbles are prose, and 64 bytes is only about twenty characters. */
#define PET_SAY_MAX 96

/* The bubble spans the card with a margin either side, which is 170 px of the card's
   196. It is sized to the line it is showing rather than to a fixed box: a fixed
   rectangle leaves a band of dead fill under every one-line line, which is most of
   what made it read as a slab rather than as something the character had just said.

   Two lines is the ceiling, down from three in the portrait layout. At 18 px a Chinese
   glyph advances a full 18 px, so the 150 px the padding leaves fits eight characters
   and the longest line in the UI — fourteen of them — takes exactly two. A third would
   have to grow the holder below, and the card is 286 px tall: the stage is what is left
   over, and the sprite's 199 px anchor box already needs almost all of it. */
#define PET_BUBBLE_W      170
#define PET_BUBBLE_PAD_X  10
#define PET_BUBBLE_PAD_Y  8
#define PET_BUBBLE_RADIUS 14

/* No outline, and a shadow instead.

   The first version of this was a white box inside a 2 px PET_COLOR_TEXT border,
   which is the comic-sticker bubble: right for a chat app, wrong on this page.
   Everything else here is a white card on warm cream separated by a hairline whose
   only job is to divide two whites, and the single hard black edge on the whole
   screen was this one. It read as pasted on.

   The soft bubbles are all flat fills with no outline, and they lift off the
   background with a shadow instead — that is what Noto's, Twemoji's and Material's
   chat-bubble-rounded have in common. LVGL fades the shadow along with the object
   (lv_obj_init_draw_rect_dsc() mixes the recursive opacity into shadow_opa), so it
   arrives and leaves with the bubble rather than popping.

   The blur is not free: the software renderer redraws it every frame and
   LV_DRAW_SW_SHADOW_CACHE_SIZE is 0 in this build, so nothing amortises it. It only
   redraws while the 200 ms fade runs. Keep the two numbers under about 13 together:
   the bubble is bottom-anchored PET_BUBBLE_TAIL_H above the holder's bottom, and
   anything deeper than that is clipped away by the holder. */
#define PET_BUBBLE_SHADOW_W 8
#define PET_BUBBLE_SHADOW_Y 2

/* Filled rather than white. Swap in PET_COLOR_ACCENT and PET_COLOR_ON_ACCENT for the
   solid chat-app bubble — same geometry, white text on blue. */
#define PET_BUBBLE_FILL PET_COLOR_CARD
#define PET_BUBBLE_INK  PET_COLOR_TEXT

#define PET_BUBBLE_LINE_H     18
#define PET_BUBBLE_TEXT_H_MAX (2 * PET_BUBBLE_LINE_H)

/* The width the label actually gets. LVGL draws a border inside the object, so it
   would come out of the content box along with the padding. There is no border now,
   but the subtraction is kept so the two stay in step if one comes back. */
#define PET_BUBBLE_TEXT_W (PET_BUBBLE_W - 2 * PET_BUBBLE_PAD_X)

#define PET_BUBBLE_MAX_H (PET_BUBBLE_TEXT_H_MAX + 2 * PET_BUBBLE_PAD_Y)

/* The tail: a drip off the bottom edge, its tip on the page's centre line, which is
   where the character's head is.

   Curved, not a triangle. The first version was a right triangle with a vertical
   right edge, and a straight vertical edge under a 18 px corner reads as a paper
   aeroplane rather than as part of the bubble. Both edges here are quadratic and
   both are concave, so the shape necks in at the mouth the way every reference tail
   does.

   SEAM is how far the polygon reaches up inside the bubble. Without it the tail's
   antialiased top edge meets the bubble's antialiased bottom edge and the
   half-covered pixels either side of the join blend against the cream, which shows
   as a hairline drawn across the mouth. */
#define PET_BUBBLE_TAIL_W     26
#define PET_BUBBLE_TAIL_H     13
#define PET_BUBBLE_TAIL_SEAM  3
#define PET_BUBBLE_TAIL_TIP   42 /* percent of the width, measured from the left */
#define PET_BUBBLE_TAIL_STEPS 8  /* segments per curved edge */

/* The holder is the fixed box the character's room is carved out of. The bubble is
   bottom-anchored inside it, so the tail is always in the same place: the character
   does not move when the line gets longer, and does not move when the bubble comes
   or goes. */
#define PET_BUBBLE_HOLDER_H (PET_BUBBLE_MAX_H + PET_BUBBLE_TAIL_H + 6)

/* How long the bubble takes to arrive and to leave, and how far it travels. Same
   order as PET_FADE_MS: long enough to read as movement, short enough not to be in
   the way of the line it is carrying. */
#define PET_BUBBLE_MS   200
#define PET_BUBBLE_RISE 10

/* How far an ALERT line shakes, in pixels. Three to five is the range that reads as a
   refusal; more than that turns into a bounce and stops meaning "no". */
#define PET_BUBBLE_SHAKE 4

/* The typewriter. A line lands a character at a time rather than all at once, which is
   what makes the bubble feel like it is being spoken. 30 ms a character is about the
   pace of the model that will eventually feed it, and short enough that even a full
   line is on screen in well under half a second. The ellipsis a THINK line grows ticks
   far slower — it is punctuation, not speech. */
#define PET_TYPE_MS 30
#define PET_DOTS_MS 340

/* Where the character sits inside the stage it grows into. PET_CHAR_TOP lives in
 * pet_theme.h now — the sprite canvas is the page's to place, and the bubble only
 * builds the holder and the stage. */

/***********************************************************
***********************type define*************************
***********************************************************/

/**
 * @brief The bubble's silhouette, one per tone.
 *
 * The body is a single rounded rectangle whatever the shape; what changes is its corner
 * radius and what the tail below it draws. That is enough for the four to read apart:
 * ROUND is the plain bubble every line used to get, PUFF rounds its corners further for
 * a pleased line, THOUGHT hangs two shrinking bubbles where the tail would be, and SHARP
 * squares its corners and tapers to a barbed point for a refusal.
 */
typedef enum {
    PET_BUBBLE_ROUND = 0, /**< CALM: the plain rounded bubble. */
    PET_BUBBLE_PUFF,      /**< HAPPY: rounder corners. */
    PET_BUBBLE_THOUGHT,   /**< THINK: trailing bubbles instead of a tail. */
    PET_BUBBLE_SHARP,     /**< ALERT: square corners and a barb. */
} PET_BUBBLE_SHAPE_E;

/***********************************************************
***********************variable define**********************
***********************************************************/

static lv_obj_t *sg_stage;
static lv_obj_t *sg_bubble_holder;
static lv_obj_t *sg_bubble;
static lv_obj_t *sg_say_label;

/* The bubble is on screen (shown, or on its way in). pet_say_stream() uses this to
   replace the line in place instead of replaying the fade-and-rise for every page of
   one AI reply, which would read as a blink between pages. */
static BOOL_T sg_bubble_on;

/* The shape the current line is being said in. Read by the tail's draw callback, which
   has no other way to know which silhouette to paint. Static rather than per-record: the
   card is built once and there is only ever one bubble. */
static PET_BUBBLE_SHAPE_E sg_bubble_shape;

/* The last line handed to pet_say(). Kept so the character is already saying the
   right thing the moment the page is rebuilt — which now happens on every trip back
   from a function page. */
static char sg_say[PET_SAY_MAX];

/* The tone the current line is being said in. Remembered alongside the text so a line
   that arrives while the card is off screen still lands the right way when it is built. */
static PET_SAY_MOOD_E sg_mood;

/* The typewriter. sg_shown is the prefix of sg_say revealed so far and is what the label
   actually shows; sg_type_len is how many bytes of sg_say that is (never a partial UTF-8
   character — see __utf8_next). The timer is NULL whenever nothing is typing, which is
   what __type_stop() checks. */
static char        sg_shown[PET_SAY_MAX];
static uint32_t    sg_type_len;
static uint32_t    sg_type_total;
static lv_timer_t *sg_type_timer;

/* The ellipsis a THINK line grows once it has finished typing. sg_dots is its position in
   a fixed 0-3 cycle, so it never allocates and never has to be bounded by the clock. */
static lv_timer_t *sg_dots_timer;
static uint32_t    sg_dots;

/* The four states of the thinking ellipsis. A zero-width string first, so the pulse
   starts from the bare line rather than from one dot. */
/* clang-format off */
static const char *const sg_dots_text[] = { "", "…", "……", "………" };
/* clang-format on */

/***********************************************************
********************function define**************************
***********************************************************/

/* ---- The bubble ------------------------------------------------------------ */

/**
 * @brief Append one quadratic bezier to a chain, as straight segments.
 *
 * Fixed point, because lv_value_precise_t is int32_t in this build (LV_USE_FLOAT is
 * 0) and there is no floating point in the draw path. t runs 0..256, so the two
 * divisions are the only rounding, and they happen once per point.
 *
 * The start point is not emitted: the caller puts it in, and every curve here starts
 * where the previous one ended.
 */
static void __tail_curve(lv_point_precise_t *out, int32_t *n, lv_point_precise_t p0, lv_point_precise_t p1,
                         lv_point_precise_t p2)
{
    for (int32_t i = 1; i <= PET_BUBBLE_TAIL_STEPS; i++) {
        int32_t t = (i * 256) / PET_BUBBLE_TAIL_STEPS;
        int32_t u = 256 - t;

        out[*n].x = (u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x) / (256 * 256);
        out[*n].y = (u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y) / (256 * 256);
        (*n)++;
    }
}

/**
 * @brief Where one of the tail's edges crosses a scanline, in 8.8 fixed point.
 *
 * The chain runs from the mouth down to the tip with y rising along it, so this is a
 * straight walk with at most one crossing per scanline. A scanline past the end of the
 * chain clamps to it, which is what closes the shape off at the tip.
 */
static int32_t __tail_edge_x(const lv_point_precise_t *chain, int32_t n, int32_t y)
{
    for (int32_t i = 0; i + 1 < n; i++) {
        int32_t y0 = chain[i].y;
        int32_t dy = chain[i + 1].y - y0;

        if (dy > 0 && y >= y0 && y <= chain[i + 1].y) {
            return chain[i].x * 256 + ((chain[i + 1].x - chain[i].x) * 256 * (y - y0)) / dy;
        }
    }

    return chain[n - 1].x * 256;
}

/**
 * @brief Fill the polygon between two edges, a scanline at a time.
 *
 * A filled shape rather than another rounded box: the 18x9 nub the tail used to be read
 * as a stray dot, because a shape that small and that round has no direction in it.
 *
 * It is painted rather than assembled out of objects because the shapes are curved and
 * LV_USE_VECTOR_GRAPHIC is off, so there is no path API to hand an lv_obj. What is
 * left is lv_draw_triangle, and it will not do these shapes. It builds its coverage
 * mask out of three lines (see lv_draw_sw_triangle.c), and a mask line that is nearly
 * horizontal comes out dashed rather than smooth — so a polygon filled as a fan, or
 * as a strip between its two edges, comes out hatched all over. Every triangle that
 * approximates a curve has a nearly horizontal edge somewhere.
 *
 * So the shape is filled a scanline at a time with lv_draw_rect, which is exact. The
 * two edges are sampled per row and the half-covered column at each end is blended at
 * the coverage the edge actually gives it, so a curved outline stays smooth without
 * ever asking for a mask — and a straight one, the barb, comes out of the same loop
 * with its corners exact.
 *
 * Both chains run from the mouth down to the tip with y rising along them, so the loop
 * walks the shared rows once. The opacity is passed in rather than read here: the
 * caller reads lv_obj_get_style_opa_recursive(), because a custom draw callback does
 * not go through lv_obj_init_draw_rect_dsc() and so does not inherit the holder's fade
 * for free. It is the same recursive form the bubble beside it gets, so the two fade
 * together at the same rate instead of the tail lagging behind.
 */
static void __tail_fill(lv_layer_t *layer, lv_opa_t opa, const lv_point_precise_t *left, int32_t nl,
                        const lv_point_precise_t *right, int32_t nr)
{
    lv_area_t          row;
    lv_draw_rect_dsc_t rect;
    int32_t            y_top = left[0].y;
    int32_t            y_tip = left[nl - 1].y;

    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = PET_BUBBLE_FILL;
    row.y1 = row.y2 = y_top;

    for (int32_t y = y_top; y <= y_tip; y++) {
        int32_t lo = __tail_edge_x(left, nl, y);
        int32_t hi = __tail_edge_x(right, nr, y);
        int32_t c_lo, c_hi, cov;

        row.y1 = row.y2 = y;

        if (hi <= lo) {
            continue;
        }

        /* The columns this scanline covers whole. */
        c_lo = (lo + 255) >> 8;
        c_hi = (hi >> 8) - 1;

        if (c_lo > c_hi) {
            /* Narrower than one whole column — the last row or two at the tip. */
            cov = hi - lo;
            if (cov > 0) {
                rect.bg_opa = (lv_opa_t)cov;
                row.x1 = row.x2 = lo >> 8;
                lv_draw_rect(layer, &rect, &row);
            }
            continue;
        }

        rect.bg_opa = opa;
        row.x1      = c_lo;
        row.x2      = c_hi;
        lv_draw_rect(layer, &rect, &row);

        /* The column the left edge cuts through, blended at what is left of it, and
           the same at the right. */
        cov = (c_lo << 8) - lo;
        if (cov > 0) {
            rect.bg_opa = (lv_opa_t)cov;
            row.x1 = row.x2 = c_lo - 1;
            lv_draw_rect(layer, &rect, &row);
        }

        cov = hi - ((c_hi + 1) << 8);
        if (cov > 0) {
            rect.bg_opa = (lv_opa_t)cov;
            row.x1 = row.x2 = c_hi + 1;
            lv_draw_rect(layer, &rect, &row);
        }
    }
}

/**
 * @brief The plain tail: a curved drip whose tip sits under the character's head.
 *
 * Curved, not a triangle. The first version was a right triangle with a vertical right
 * edge, and a straight vertical edge under an 18 px corner reads as a paper aeroplane
 * rather than as part of the bubble. Both edges here are quadratic and both are
 * concave, so the shape necks in at the mouth the way every reference tail does.
 *
 * The mouth is the top of the tail object, which is PET_BUBBLE_TAIL_SEAM above the
 * bubble's own bottom edge. The tail is a sibling created after the bubble, so the top
 * rows that overlap the join paint over it instead of meeting it edge to edge.
 */
static void __tail_draw_drip(lv_obj_t *tail, lv_layer_t *layer, lv_opa_t opa)
{
    lv_area_t box;
    /* The mouth corner, plus PET_BUBBLE_TAIL_STEPS points along the curve, which ends
       on the tip — so both chains are the same length. */
    lv_point_precise_t left[PET_BUBBLE_TAIL_STEPS + 1];
    lv_point_precise_t right[PET_BUBBLE_TAIL_STEPS + 1];
    lv_point_precise_t a, tip, b, ctrl;
    int32_t            nl = 0;
    int32_t            nr = 0;
    int32_t            mouth_y;
    int32_t            drop;

    lv_obj_get_coords(tail, &box);

    mouth_y = box.y1;
    drop    = box.y2 - box.y1;

    a.x   = box.x1;
    a.y   = mouth_y;
    b.x   = box.x2;
    b.y   = mouth_y;
    tip.x = box.x1 + (PET_BUBBLE_TAIL_W * PET_BUBBLE_TAIL_TIP) / 100;
    tip.y = mouth_y + drop;

    /* The outer edge, from the mouth's left corner round to the tip. Its control point
       sits down at the tip's own line, so the curve leaves the mouth going straight
       down and only then sweeps across into the tip — the convex side. Putting the
       control point anywhere nearer the mouth line instead curls this edge inward and
       the pair together come out as a chevron rather than as a tail. */
    left[nl++] = a;
    ctrl.x     = box.x1;
    ctrl.y     = tip.y;
    __tail_curve(left, &nl, a, ctrl, tip);

    /* The inner edge, from the mouth's right corner round to the same tip. Its control
       point sits just under the mouth line, which is what necks the shape in instead of
       letting it flare back out into a triangle — the concave side. It is the longer of
       the two, so the tail reads as leaning left of its tip. */
    right[nr++] = b;
    ctrl.x      = box.x1 + (PET_BUBBLE_TAIL_W * 60) / 100;
    ctrl.y      = mouth_y + (drop * 15) / 100;
    __tail_curve(right, &nr, b, ctrl, tip);

    __tail_fill(layer, opa, left, nl, right, nr);
}

/**
 * @brief The barbed tail: the drip's silhouette with its curves taken out.
 *
 * The curve is what makes the plain tail soft, so the one shape for a refusal is the
 * same drip drawn with straight edges and a notch in its inner side. It runs from the
 * mouth's right corner in toward the centre, then back out to the tip, so the tail
 * still comes to a point the way the drip does but arrives with a corner in it.
 */
static void __tail_draw_barb(lv_obj_t *tail, lv_layer_t *layer, lv_opa_t opa)
{
    lv_area_t          box;
    lv_point_precise_t left[2];
    lv_point_precise_t right[3];
    lv_point_precise_t tip, notch;
    int32_t            mouth_y;
    int32_t            drop;

    lv_obj_get_coords(tail, &box);

    mouth_y = box.y1;
    drop    = box.y2 - box.y1;

    tip.x = box.x1 + (PET_BUBBLE_TAIL_W * PET_BUBBLE_TAIL_TIP) / 100;
    tip.y = mouth_y + drop;

    left[0].x = box.x1;
    left[0].y = mouth_y;
    left[1]   = tip;

    right[0].x = box.x2;
    right[0].y = mouth_y;
    notch.x    = box.x1 + (PET_BUBBLE_TAIL_W * 62) / 100;
    notch.y    = mouth_y + (drop * 40) / 100;
    right[1]   = notch;
    right[2]   = tip;

    __tail_fill(layer, opa, left, 2, right, 3);
}

/**
 * @brief The trailing bubbles of a thinking line: two shrinking circles in place of a
 *        tail.
 *
 * This is the one shape that is not a polygon, so it is not drawn by __tail_fill: a
 * circle is a rectangle with its corner radius set to LV_RADIUS_CIRCLE, and going
 * through lv_draw_rect() also gets it a soft shadow for free — which the scanline fill
 * deliberately does without. That shadow is not decoration here: a white circle alone
 * on the cream page is only a shade lighter than the page, and it is the shadow that
 * makes the bubbles read as bubbles at all.
 *
 * The two are pinned inside the tail object's own box, which is why the smaller one is
 * positioned from the box's bottom: the box is bottom-anchored to the holder and the
 * object clips its drawing to its coordinates, so anything meant to be seen has to fit
 * above the holder's bottom edge.
 */
static void __tail_draw_dots(lv_obj_t *tail, lv_layer_t *layer, lv_opa_t opa)
{
    /* Diameter, and how far below the mouth each circle's top sits. The first starts at
       the mouth — it hangs off the bubble's bottom edge — and the second is set from the
       first's foot so the pair just touch. */
    static const int32_t dia[]  = {9, 5};
    static const int32_t down[] = {PET_BUBBLE_TAIL_SEAM, PET_BUBBLE_TAIL_SEAM + 9};
    lv_area_t            box;
    int32_t              cx;

    lv_obj_get_coords(tail, &box);
    cx = box.x1 + PET_BUBBLE_TAIL_W / 2;

    for (size_t i = 0; i < sizeof(dia) / sizeof(dia[0]); i++) {
        lv_draw_rect_dsc_t d;
        lv_area_t          area;
        int32_t            r = dia[i] / 2;

        lv_draw_rect_dsc_init(&d);
        d.radius          = LV_RADIUS_CIRCLE;
        d.bg_color        = PET_BUBBLE_FILL;
        d.bg_opa          = opa;
        d.shadow_color    = PET_COLOR_SHADOW;
        d.shadow_width    = PET_BUBBLE_SHADOW_W;
        d.shadow_offset_y = PET_BUBBLE_SHADOW_Y;
        d.shadow_opa      = (lv_opa_t)((LV_OPA_20 * opa) / LV_OPA_COVER);

        area.x1 = cx - r;
        area.y1 = box.y1 + down[i];
        area.x2 = area.x1 + dia[i] - 1;
        area.y2 = area.y1 + dia[i] - 1;
        lv_draw_rect(layer, &d, &area);
    }
}

/**
 * @brief Draw the tail in whichever shape the current line is being said in.
 *
 * The opacity is read here, once, and handed on: a custom draw callback does not go
 * through lv_obj_init_draw_rect_dsc() and so does not inherit the holder's fade for
 * free, and the recursive form is exactly what the bubble beside it gets — so the two
 * fade together at the same rate instead of the tail lagging behind.
 */
static void __tail_draw_cb(lv_event_t *e)
{
    lv_obj_t   *tail  = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_opa_t    opa   = lv_obj_get_style_opa_recursive(tail, LV_PART_MAIN);

    if (opa <= LV_OPA_MIN) {
        return;
    }

    switch (sg_bubble_shape) {
    case PET_BUBBLE_THOUGHT:
        __tail_draw_dots(tail, layer, opa);
        break;
    case PET_BUBBLE_SHARP:
        __tail_draw_barb(tail, layer, opa);
        break;
    default:
        __tail_draw_drip(tail, layer, opa);
        break;
    }
}

/**
 * @brief The shape a tone is spoken in.
 *
 * The four tones map one to one: CALM is the plain bubble, and each of the other three
 * is the one shape that says what it is — pleased rounds off, thinking trails bubbles, a
 * refusal sharpens. Nothing else chooses a shape, so a line's look and its motion always
 * agree.
 */
static PET_BUBBLE_SHAPE_E __mood_shape(PET_SAY_MOOD_E mood)
{
    switch (mood) {
    case PET_SAY_HAPPY:
        return PET_BUBBLE_PUFF;
    case PET_SAY_THINK:
        return PET_BUBBLE_THOUGHT;
    case PET_SAY_ALERT:
        return PET_BUBBLE_SHARP;
    default:
        return PET_BUBBLE_ROUND;
    }
}

/**
 * @brief The body's corner radius for a shape.
 *
 * PET_BUBBLE_RADIUS is the plain bubble; the others move around it — rounder for a
 * pleased line, squarer for a refusal. THOUGHT keeps close to the plain radius, because
 * what a thinking line is read by is the bubbles under it, not its corners.
 */
static int32_t __shape_radius(PET_BUBBLE_SHAPE_E shape)
{
    switch (shape) {
    case PET_BUBBLE_PUFF:
        return 24;
    case PET_BUBBLE_THOUGHT:
        return 18;
    case PET_BUBBLE_SHARP:
        return 8;
    default:
        return PET_BUBBLE_RADIUS;
    }
}

/**
 * @brief Put the bubble and its tail into the shape a tone is spoken in.
 *
 * Two things move: the body's corner radius, and what the tail draws, which the draw
 * callback reads back off sg_bubble_shape. The tail's box does not — it is the same
 * 26 x 16 window under the mouth whatever the shape — so the tail never has to be
 * re-aligned and the bubble never has to move. The radius is re-invalidated rather than
 * animated: a shape is what a line arrives as, not something to watch change.
 */
static void __bubble_apply_shape(PET_BUBBLE_SHAPE_E shape)
{
    sg_bubble_shape = shape;

    if (sg_bubble != NULL) {
        lv_obj_set_style_radius(sg_bubble, __shape_radius(shape), 0);
        lv_obj_invalidate(sg_bubble);
        if (sg_bubble_holder != NULL) {
            lv_obj_invalidate(sg_bubble_holder);
        }
    }
}

/**
 * @brief Size the bubble to the line it is carrying.
 *
 * Measured up front rather than left to LV_SIZE_CONTENT, because the bubble is
 * bottom-anchored: a height that arrived a layout pass late would show as the bubble
 * jumping into place. lv_text_get_size() is the same measurement lv_label's own wrap
 * mode makes, with the same width, so the two agree.
 */
static void __bubble_fit(const char *text)
{
    lv_point_t size;

    if (sg_bubble == NULL) {
        return;
    }

    lv_text_get_size(&size, text, PET_FONT_CJK_SM, 0, 0, PET_BUBBLE_TEXT_W, LV_TEXT_FLAG_NONE);

    if (size.y < PET_BUBBLE_LINE_H) {
        size.y = PET_BUBBLE_LINE_H;
    }
    if (size.y > PET_BUBBLE_TEXT_H_MAX) {
        size.y = PET_BUBBLE_TEXT_H_MAX;
    }

    lv_obj_set_height(sg_bubble, size.y + 2 * PET_BUBBLE_PAD_Y);
}

/**
 * @brief The speech bubble: a shadowed rounded box, a wrapping label, and a tail.
 */
static void __build_bubble(lv_obj_t *holder)
{
    lv_obj_t *bubble = lv_obj_create(holder);
    lv_obj_t *tail   = lv_obj_create(holder);

    pui_passthrough(bubble);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(bubble, PET_BUBBLE_W);
    lv_obj_set_height(bubble, PET_BUBBLE_MAX_H);
    lv_obj_set_style_bg_color(bubble, PET_BUBBLE_FILL, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    /* A warm shadow rather than a grey one: the page is cream, and a neutral grey
       under a white card on cream reads as dirt. */
    lv_obj_set_style_shadow_width(bubble, PET_BUBBLE_SHADOW_W, 0);
    lv_obj_set_style_shadow_offset_y(bubble, PET_BUBBLE_SHADOW_Y, 0);
    lv_obj_set_style_shadow_color(bubble, PET_COLOR_SHADOW, 0);
    lv_obj_set_style_shadow_opa(bubble, LV_OPA_20, 0);
    lv_obj_set_style_pad_hor(bubble, PET_BUBBLE_PAD_X, 0);
    lv_obj_set_style_pad_ver(bubble, PET_BUBBLE_PAD_Y, 0);
    /* Bottom-anchored, so the tail — which is positioned off the bubble's bottom
       edge — stays put however tall the bubble turns out to be. */
    lv_obj_align(bubble, LV_ALIGN_BOTTOM_MID, 0, -PET_BUBBLE_TAIL_H);
    sg_bubble = bubble;
    /* The radius is the shape's to set, so it is set here rather than above. */
    __bubble_apply_shape(PET_BUBBLE_ROUND);

    sg_say_label = pui_label_create(bubble, sg_say, PET_FONT_CJK_SM, PET_BUBBLE_INK);
    lv_obj_set_style_text_color(sg_say_label, PET_BUBBLE_INK, 0);
    lv_obj_set_style_text_align(sg_say_label, LV_TEXT_ALIGN_CENTER, 0);
    /* Pinned to the same width __bubble_fit() measures against, rather than left as
       LV_PCT(100) — the two have to agree exactly or a line measured as one line
       wraps to two and loses its second half off the bottom. */
    lv_obj_set_width(sg_say_label, PET_BUBBLE_TEXT_W);
    lv_label_set_long_mode(sg_say_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(sg_say_label, LV_ALIGN_TOP_LEFT, 0, 0);

    /* The tail is its own object so it can be built after the bubble and so paint
       over the join; its box is the seam overlap plus the drop below the bubble. */
    pui_passthrough(tail);
    lv_obj_remove_flag(tail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(tail, PET_BUBBLE_TAIL_W, PET_BUBBLE_TAIL_SEAM + PET_BUBBLE_TAIL_H);
    lv_obj_set_style_bg_opa(tail, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tail, 0, 0);
    lv_obj_set_style_pad_all(tail, 0, 0);
    lv_obj_align(tail, LV_ALIGN_BOTTOM_MID, -PET_BUBBLE_TAIL_W / 2, 0);
    lv_obj_add_event_cb(tail, __tail_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
}

/**
 * @brief Animation trampolines.
 *
 * lv_obj_set_style_*() take a selector as their third argument, so they cannot be
 * cast to lv_anim_exec_xcb_t and handed to lv_anim_set_exec_cb() the way LVGL's own
 * two-argument setters can. These forward with the selector pinned to the main part.
 */
static void __bubble_opa_anim(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void __bubble_rise_anim(void *var, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)value, 0);
}

/**
 * @brief Show or hide the bubble and its tail, without touching the holder.
 *
 * The holder must stay in the layout at all times. It is a flex child of the card, and
 * lv_flex.c skips any child carrying LV_OBJ_FLAG_HIDDEN — so hiding the holder collapses
 * it to nothing, the stage below grows by the holder's 83 px, and the character jumps to
 * the top of the card. That is what this used to do, and it is why the card looked like
 * it had two different layouts.
 *
 * Hiding the holder's children instead costs nothing: the holder is transparent and the
 * bubble is bottom-aligned inside it, so the box keeps its height and the character
 * keeps its place. Both children go together — the tail is a sibling of the bubble, not
 * a child of it, so hiding one alone leaves a wedge floating under nothing.
 */
static void __bubble_children_hide(BOOL_T hide)
{
    uint32_t n;

    if (sg_bubble_holder == NULL) {
        return;
    }

    n = lv_obj_get_child_count(sg_bubble_holder);

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(sg_bubble_holder, i);

        if (hide) {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ---- The typewriter and the thinking ellipsis ----------------------------- */

/**
 * @brief The index just past the character starting at @p i.
 *
 * A line here is mostly three-byte CJK, so revealing it a *byte* at a time would show a
 * broken glyph for two frames out of every three. This walks whole characters: the lead
 * byte says how many bytes follow. A byte that is not a valid lead is treated as one
 * character long, so a malformed line still reaches its end instead of the timer spinning
 * on the same spot.
 */
static int __utf8_next(const char *s, int i)
{
    unsigned char c = (unsigned char)s[i];

    if ((c & 0x80) == 0x00) {
        return i + 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return i + 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return i + 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return i + 4;
    }
    return i + 1;
}

static void __type_stop(void)
{
    if (sg_type_timer != NULL) {
        lv_timer_delete(sg_type_timer);
        sg_type_timer = NULL;
    }
}

static void __dots_stop(void)
{
    if (sg_dots_timer != NULL) {
        lv_timer_delete(sg_dots_timer);
        sg_dots_timer = NULL;
    }
    sg_dots = 0;
}

/** @brief One tick of the thinking ellipsis: the line, then one more dot each time. */
static void __dots_cb(lv_timer_t *t)
{
    (void)t;

    lv_snprintf(sg_shown, sizeof(sg_shown), "%s%s", sg_say, sg_dots_text[sg_dots]);
    sg_dots = (sg_dots + 1) % (sizeof(sg_dots_text) / sizeof(sg_dots_text[0]));
    lv_label_set_text(sg_say_label, sg_shown);
}

/** @brief One tick of the typewriter: reveal the next character; when the line is whole,
 *         hand over to the ellipsis if this is a THINK line. */
static void __type_cb(lv_timer_t *t)
{
    (void)t;

    int next = __utf8_next(sg_say, (int)sg_type_len);

    if (next >= (int)sg_type_total) {
        next = (int)sg_type_total;
    }
    sg_type_len = (uint32_t)next;

    memcpy(sg_shown, sg_say, sg_type_len);
    sg_shown[sg_type_len] = '\0';
    lv_label_set_text(sg_say_label, sg_shown);

    if (sg_type_len >= sg_type_total) {
        __type_stop();
        if (sg_mood == PET_SAY_THINK) {
            sg_dots_timer = lv_timer_create(__dots_cb, PET_DOTS_MS, NULL);
        }
    }
}

/**
 * @brief Start the typewriter for the current sg_say.
 *
 * The first character is drawn here rather than waiting for the first tick, so the bubble
 * never arrives blank and then fills. If the line is a single character (or empty) there
 * is nothing to type and the timer is not created at all.
 */
static void __type_start(void)
{
    __type_stop();
    __dots_stop();

    sg_type_len   = 0;
    sg_type_total = (uint32_t)strlen(sg_say);
    sg_shown[0]   = '\0';

    if (sg_type_total == 0) {
        return;
    }

    sg_type_len = (uint32_t)__utf8_next(sg_say, 0);
    if (sg_type_len > sg_type_total) {
        sg_type_len = sg_type_total;
    }
    memcpy(sg_shown, sg_say, sg_type_len);
    sg_shown[sg_type_len] = '\0';
    lv_label_set_text(sg_say_label, sg_shown);

    if (sg_type_len < sg_type_total) {
        sg_type_timer = lv_timer_create(__type_cb, PET_TYPE_MS, NULL);
    } else if (sg_mood == PET_SAY_THINK) {
        sg_dots_timer = lv_timer_create(__dots_cb, PET_DOTS_MS, NULL);
    }
}

/**
 * @brief Drop everything the bubble is running.
 *
 * Every animation on the holder goes at once — the opacity, the rise, and the side-shake
 * a mood may have added. LVGL keys animations by (variable, callback), and the shake's
 * callback lives in pui_anim.c, so the all-on-this-object form is the one that is sure to
 * catch all three. The two timers are not animations and have to be cancelled by hand.
 */
static void __bubble_anim_stop(void)
{
    lv_anim_delete(sg_bubble_holder, NULL);
    __type_stop();
    __dots_stop();
}

/**
 * @brief Fade the bubble up and slide it down into place, in the tone @p mood asks for.
 *
 * Fade and translate only. A scale-in would read as more of a pop, but
 * transform_scale forces LV_LAYER_TYPE_TRANSFORM, which allocates a buffer over the
 * object's whole area for the length of the animation — about 50 KB at this size, on
 * a board with a 3.67 MB app partition. Opacity and translate are both drawn in
 * place, so they cost nothing but the redraw. The mood is spent in the *course* of those
 * two: HAPPY overshoots the resting place on the way in, ALERT adds a side-shake. Both
 * are translate, so neither costs a layer either.
 */
static void __bubble_show(PET_SAY_MOOD_E mood)
{
    lv_anim_t         a;
    lv_anim_path_cb_t rise_path = lv_anim_path_ease_out;

    __bubble_anim_stop();
    __bubble_children_hide(FALSE);
    sg_bubble_on = TRUE;

    /* The silhouette is the mood's as well, and it is set before the fade starts so the
       line arrives already the right shape rather than morphing into it mid-arrival. */
    __bubble_apply_shape(__mood_shape(mood));

    /* The overshoot path runs past the end value and settles back, so a rise from -RISE
       to 0 dips a few pixels below the resting line before returning — a small bounce,
       made of the same translate the plain rise uses. */
    if (mood == PET_SAY_HAPPY) {
        rise_path = lv_anim_path_overshoot;
    }

    lv_anim_init(&a);
    lv_anim_set_var(&a, sg_bubble_holder);
    lv_anim_set_time(&a, PET_BUBBLE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, __bubble_opa_anim);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, sg_bubble_holder);
    lv_anim_set_time(&a, PET_BUBBLE_MS);
    lv_anim_set_path_cb(&a, rise_path);
    lv_anim_set_exec_cb(&a, __bubble_rise_anim);
    lv_anim_set_values(&a, -PET_BUBBLE_RISE, 0);
    lv_anim_start(&a);

    /* The one mood with more than one movement: a refusal is the bubble shaking its head.
       On translate_x, so it does not fight the rise on translate_y. */
    if (mood == PET_SAY_ALERT) {
        pui_anim_shake(sg_bubble_holder, PET_BUBBLE_SHAKE, PET_BUBBLE_MS);
    }
}

/**
 * @brief Take the bubble out of the draw once its fade has finished.
 *
 * The holder, not the animation's own variable: the fade runs on the holder's opacity
 * and the holder has to stay in the layout. See __bubble_children_hide().
 */
static void __bubble_hide_done(lv_anim_t *a)
{
    (void)a;

    __bubble_children_hide(TRUE);
}

/**
 * @brief Fade the bubble out and slide it away.
 *
 * The bubble is only hidden once the fade is over. Hiding it up front — which is what
 * the page used to do — is why the disappear could not be seen at all.
 */
static void __bubble_hide(void)
{
    lv_anim_t a;

    if (sg_bubble == NULL || lv_obj_has_flag(sg_bubble, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    sg_bubble_on = FALSE;
    __bubble_anim_stop();

    lv_anim_init(&a);
    lv_anim_set_var(&a, sg_bubble_holder);
    lv_anim_set_time(&a, PET_BUBBLE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, __bubble_opa_anim);
    /* From wherever the fade in left it, so a line replaced mid-arrival does not
       jump back to full opacity first. */
    lv_anim_set_values(&a, lv_obj_get_style_opa(sg_bubble_holder, 0), LV_OPA_TRANSP);
    lv_anim_set_completed_cb(&a, __bubble_hide_done);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, sg_bubble_holder);
    lv_anim_set_time(&a, PET_BUBBLE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, __bubble_rise_anim);
    lv_anim_set_values(&a, 0, -PET_BUBBLE_RISE);
    lv_anim_start(&a);
}

/**
 * @brief The character's opening line: how much is still open today.
 *
 * Derived from the model rather than written down, so the bubble cannot contradict the
 * badge in the status strip above it.
 *
 * **Two lines of seven is the ceiling** (PET_BUBBLE_TEXT_H_MAX), so the line is kept
 * to ten characters — a longer one loses its tail off the bottom of the bubble
 * rather than wrapping.
 */
static void __say_pending(void)
{
    char     text[PET_SAY_MAX];
    uint32_t pending = pet_demo_todo_pending();

    if (pending == 0) {
        pet_say("今天的待办都完成啦");
        return;
    }

    lv_snprintf(text, sizeof(text), "今天还有%u件待办", (unsigned)pending);
    pet_say(text);
}

/* ---- Card ------------------------------------------------------------------ */

/**
 * @brief Build the character's card.
 *
 * Called once, from the shell, and never again: the card outlives every navigation and
 * so do the four pointers above, which is what lets pet_say() write into the bubble
 * from a page that is nowhere near it.
 *
 * The stage is left empty. The sprite is an lv_canvas with a PSRAM buffer, and both
 * belong to the page that owns the animation state (app_page_pet), which adds it to
 * this stage through pet_pet_stage().
 */
void pet_page_pet_build(lv_obj_t *column)
{
    lv_obj_t *holder;

    sg_stage         = NULL;
    sg_bubble_holder = NULL;
    sg_bubble        = NULL;
    sg_say_label     = NULL;
    sg_bubble_on     = FALSE;

    /* The holder is a fixed-height box so the character below it never moves when the
       bubble's line changes. It starts at zero opacity with its contents hidden, so the
       first line — the opening one, or whatever the user was doing on the page they came
       back from — fades in rather than already being there when the card appears.

       The holder itself is never hidden. It is a flex child, and lv_flex.c skips hidden
       children, so hiding it would collapse the box and pull the character up the card.
       See __bubble_children_hide(). */
    holder = lv_obj_create(column);
    pui_passthrough(holder);
    lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(holder, LV_PCT(100), PET_BUBBLE_HOLDER_H);
    lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(holder, 0, 0);
    lv_obj_set_style_pad_all(holder, 0, 0);
    lv_obj_set_style_opa(holder, LV_OPA_TRANSP, 0);
    /* Children are clipped to their parent's extent by default, and the bubble's
       shadow is drawn below the bubble — outside this box. Nothing else in the app
       overflows its parent, so this is the one place the flag is needed. */
    lv_obj_add_flag(holder, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    sg_bubble_holder = holder;

    __build_bubble(holder);
    __bubble_children_hide(TRUE);

    sg_stage = lv_obj_create(column);
    pui_passthrough(sg_stage);
    lv_obj_remove_flag(sg_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(sg_stage, LV_PCT(100));
    lv_obj_set_flex_grow(sg_stage, 1);
    lv_obj_set_style_bg_opa(sg_stage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sg_stage, 0, 0);
    lv_obj_set_style_pad_all(sg_stage, 0, 0);
    /* The canvas is clipped to this box. The anchor box is 1 px wider than the stage's
       content width at the current asset set, and that 1 px is transparent padding on
       each side — but a future asset that grows the frame would be cut, so the
       overhang is logged by the page rather than silently swallowed. */

    /* sg_say is already set — either from the seed line below or from whatever the
       user was doing before they came back here. The mood travels with it: a line that
       was said HAPPY is still HAPPY when the card is rebuilt, not reset to CALM. */
    if (sg_say[0] == '\0') {
        __say_pending();
    } else {
        pet_say_mood(sg_say, sg_mood);
    }
}

lv_obj_t *pet_pet_stage(void)
{
    return sg_stage;
}

void pet_page_pet_reset(void)
{
    /* The two timers are LVGL timers, not children of the card, so deleting the
       objects does not stop them. Left running they would write the next character
       into a label that no longer exists. */
    __type_stop();
    __dots_stop();

    sg_stage         = NULL;
    sg_bubble_holder = NULL;
    sg_bubble        = NULL;
    sg_say_label     = NULL;
    sg_bubble_on     = FALSE;
}

void pet_say_mood(const char *text, PET_SAY_MOOD_E mood)
{
    if (text == NULL) {
        text = "";
    }

    /* pet_pet_reflow() hands back the buffer it is stored in; copying a buffer onto
       itself with strncpy() is undefined. */
    if (text != sg_say) {
        strncpy(sg_say, text, sizeof(sg_say) - 1);
        sg_say[sizeof(sg_say) - 1] = '\0';
    }
    sg_mood = mood;

    if (sg_say_label == NULL || sg_bubble_holder == NULL) {
        /* The card is not on screen. The line — and its tone — are remembered and shown
           when it is built again. */
        return;
    }

    /* An empty line is the pet going quiet, which is the one path that takes the
       bubble away — so it animates out rather than simply not being there. None of the
       per-mood machinery runs for it: there is nothing to type or shake. */
    if (sg_say[0] == '\0') {
        __type_stop();
        __dots_stop();
        __bubble_hide();
        return;
    }

    /* Sized from the whole line, then typed into: the bubble takes its final height at
       once and the characters fill it, so the box never grows a line at a time. */
    __bubble_fit(sg_say);
    __bubble_show(mood);
    __type_start();
}

void pet_say(const char *text)
{
    pet_say_mood(text, PET_SAY_CALM);
}

void pet_say_stream(const char *text)
{
    if (text == NULL) {
        text = "";
    }

    if (text != sg_say) {
        strncpy(sg_say, text, sizeof(sg_say) - 1);
        sg_say[sizeof(sg_say) - 1] = '\0';
    }
    sg_mood = PET_SAY_CALM;

    if (sg_say_label == NULL || sg_bubble_holder == NULL) {
        return;
    }

    if (sg_say[0] == '\0') {
        __type_stop();
        __dots_stop();
        __bubble_hide();
        return;
    }

    __bubble_fit(sg_say);

    /* 已经是 AI 回复的下一页：只换字，不重播淡入/上浮。整段回复每页都闪一下
       会让气泡看起来在眨眼；第一页（气泡还没出现）才走完整动画。 */
    if (!sg_bubble_on) {
        __bubble_show(PET_SAY_CALM);
    } else {
        __bubble_apply_shape(__mood_shape(PET_SAY_CALM));
    }

    __type_start();
}

void pet_pet_reflow(void)
{
    if (sg_say_label == NULL || sg_say[0] == '\0') {
        return;
    }

    /* Same text, re-measured against the face that is loaded now. pet_say_mood()
       skips the copy when the pointer is the buffer itself, so this is safe. */
    pet_say_mood(sg_say, sg_mood);
}
