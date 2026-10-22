/**
 * @file pui.h
 * @brief The component library: LVGL widget classes that own their own geometry.
 *
 * Every page in this app used to lay itself out in pixels. The gallery computed its
 * own column width from the panel's (`(PET_PANEL_W - 2*PET_PAD_PAGE - 2*GAL_GAP)/3`),
 * the home page computed its own tile height from the icon size, the note page derived
 * a sheet's inner width from the card's padding. That is what made the UI rigid: a page
 * knew the panel was 260 px wide and that a card is padded by 10, which is none of its
 * business, and changing a spacing meant editing five files.
 *
 * This header is the other arrangement. A page composes components and hands them data;
 * a component decides how big it is, how it is padded and what colour it is. The three
 * mechanisms that make that work:
 *
 *   - `LV_SIZE_CONTENT` plus, where a component's size is not a function of its
 *     children, `LV_EVENT_GET_SELF_SIZE` — so a tile reports its own height and no
 *     caller writes `PUI_TILE_ICON + 6 + PET_LINE_H + 2`.
 *   - `lv_grid` with `LV_GRID_FR(1)` columns — so "three across" is `pui_grid_create(p,
 *     3, gap)` and not a division against a width the caller had to know.
 *   - `PUI_TONE_E` / `PUI_FILL_E` — so a colour is a role, not a literal at the call
 *     site. One component wears six hues in four fills without a single
 *     `lv_obj_set_style_*` outside this library.
 *
 * ## The classes
 *
 * These are real `lv_obj_class_t` widgets, in the style this build of LVGL requires
 * (9.1.0 has no `LV_OBJ_CLASS_DECLARE`; see the note further down). Each one declares
 * its instance struct here, so `lv_obj_check_type(obj, &pui_card_class)` works and a
 * component can find its own parts after the fact.
 *
 * ## Reading order
 *
 * `pui_init()` first — it fills the tone table, and `lv_color_hex()` is a function in
 * this build, so nothing can be a static initialiser. Then the three axes below, then
 * the components.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __PUI_H__
#define __PUI_H__

#include "lvgl.h"

#include "pet_theme.h"
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/* Component geometry that no page should ever name. These are the numbers the pages
   used to compute for themselves — a tile's icon square, a button's height per size,
   the gap between a card's rows. They live here rather than in pet_theme.h because
   they are the components' business, and because a page that can see them is a page
   that will eventually hard-code them. */

/* The icon square inside a tile, and the gap under it before the caption. */
#define PUI_TILE_ICON 44
#define PUI_TILE_GAP  6

/* The block on a wide tile — the one standing in for a photograph. Taller than it is
   square because a photo block is read as a picture and a square is read as an icon,
   and because the caption underneath it has to stay outside the block.

   58 rather than 76: a block this size holds a 20 px glyph and nothing else, so every
   pixel past the glyph plus its breathing room is a pixel of empty pastel, and empty
   pastel is what a placeholder looks like. At 58 a three-column grid of them shows two
   full rows in the gallery's well instead of one and a half. */
#define PUI_TILE_PHOTO_H 58

/* A shorter wide block, for a grid of thumbnails that has to share a sheet with a
   heading and a pair of buttons rather than fill a page. */
#define PUI_TILE_PICK_H 48

/* A card's inner padding, and the gap between the children it stacks.

   The padding is a width budget, not just air. The tightest card in the app is one of
   the home page's two-up summary cards, and what it has to hold is worked out from the
   panel: 480 px of screen less PET_PAD_PAGE on each side, less the pet's 190 px column
   and PET_GAP_PANEL leaves PET_PANEL_W = 256; less a page margin and PUI_GRID_GAP the
   two tracks come to 111 each; less this padding twice and the 1 px border, a card's
   content is 93 px wide.

   8 rather than 10 for that reason. The card's rows carry this padding themselves — a
   row's hairline runs the card's full width, so it cannot sit inside the card's own
   padding — and a row therefore has 77 px for its text, which is the four characters of
   购物清单 in the 18 px face plus five pixels. At 10 it was 73 and the fourth character
   elided, which is not a design.

   The heading is the other half of the budget, and it now has the whole of it: the
   chevron that used to sit beside it is gone, because the card's own footer capsule
   already says the card is a target. That is what lets the home page's headings be the
   four characters they want to be — 今日待办 is about 64 px in the 16 px card face, which
   is a comfortable two thirds of the 98 px the heading row gives it. At the 20 px face it
   was 80 and filled the row, which is why the heading came down a size and the chevron
   went. A heading that does not fit does not clip, it wraps, and the wrapped heading row
   is 22 px of page height the home page does not have. Raising this number or
   PUI_GRID_GAP spends that slack, and the failure is not a clipped title — it is a grid
   whose children overflow their tracks and run off the panel. */
#define PUI_CARD_PAD 8
#define PUI_CARD_GAP 6

/* The gap between two tiles in a row, and between two columns of a grid. */
#define PUI_TILE_GAP_X 8
#define PUI_GRID_GAP   10

/* Button heights. The capsule radius is half the height, so these three numbers are
   also the three radii — see __button in pui_button.c. */
#define PUI_BTN_H_SM 28
#define PUI_BTN_H_MD 36
#define PUI_BTN_H_LG 44

/* The round icon button. 32 px is about 4.8 mm on this panel, already at the low end
   of a comfortable touch target; do not shrink it to make a row fit. */
#define PUI_ICON_BTN 32

/* A list row and a stat row. Both are content-sized; these are the floors that keep a
   row with a short label from collapsing into a hairline. */
#define PUI_ROW_H_MIN 24

/* The icon square at the head of a list row. 22 rather than the tile's 44, because a row
   is one line tall and the square has to fit inside it without becoming the row.

   It costs 22 px plus the row's own 6 px gap, which leaves 61 of a card's 93 px content
   width — and the four characters of 购物清单 are 56 in the 14 px face. A left stripe
   would have cost 3 and left the text 80, but a stripe is not what the reference draws,
   and the 25 px it saves are 25 px of white nothing else on the row wants. */
#define PUI_ROW_ICON 22

/* The selection ring a tile draws when it is selected. The caption changes colour with
   it; there is no badge, because a badge on a 44 px square either covers the glyph or
   hangs outside the square and gets clipped. */
#define PUI_TILE_RING 2

/* Page chrome. A page's header and action bar are fixed heights so the layout does not
   jump between pages; the content between them takes whatever is left.

   The status strip is 26 rather than 20: at 20 the 18 px face's 20 px line box had no
   air above or below it, so the strip read as text with a coloured line under it rather
   than as a bar. 26 gives the line its own margin, and the hairline under the strip then
   reads as the bar's edge.

   The header is 36 and has no fill. The page's name is set in the 20 px title face, whose
   line box is 22, so 36 leaves it 7 px of air above and below — a title row rather than a
   title squeezed into a strip. It was a 44 px white bar while the title was set at 24 px;
   the bar went when the title came down, and the height stayed where it was because the
   content below it is measured from it. */
#define PUI_HEADER_H     36
#define PUI_ACTION_H     48
#define PUI_STATUS_H     20
#define PUI_STATUS_BADGE 22

/* How far a photo tile's block grades from its light wash toward the full hue, of 255.
   Small on purpose: enough to read as a lit surface rather than a flat swatch. */
#define PUI_TILE_DEEP 40

/* Motion. Four transitions, and one rule that decides which of them a component may have:
   scale is for a leaf and translate-and-fade is for anything holding children, because
   scaling an object re-renders its whole subtree offscreen every frame. The rule and its
   cost are set out at the top of pui_anim.c; these are the numbers.

   A band's arrival. 8 px is about a line of the working face, so a card travels far enough
   to be seen moving and not far enough to look like it came from somewhere else. The
   stagger is what makes a page read as assembling rather than as appearing, and 40 ms is
   short enough that five bands are all in place 340 ms after the page opens. */
#define PUI_RISE_MS   180
#define PUI_RISE_PX   8
#define PUI_RISE_STEP 40

/* The press squash, of 256. Four per cent: enough to be felt at a fingertip's scale on a
   36 px capsule, little enough that a row of them does not ripple when a thumb crosses it. */
#define PUI_SQUASH 246

/* The press shadow. A solid button rests a hair above the page and its shadow collapses as
   it is pressed, so the squash reads as the button sinking into the surface rather than as
   it shrinking where it stands. Only solid fills carry the resting shadow — a ghost or an
   outline has no fill for a shadow to sit under, and tinted cards are washed into the page
   on purpose. */
#define PUI_PRESS_SHADOW_W  4
#define PUI_PRESS_SHADOW_DY 1

/* A value changing. The badge and the camera's mode chip grow an eighth and come back —
   short, and small enough that a label does not grow into the text beside it. */
#define PUI_POP_SCALE 288
#define PUI_POP_MS    110

/* A sheet opening. The card rises further than a page band does, because it is arriving
   over the page rather than with it, and the scrim fades under it on the same clock. */
#define PUI_SHEET_MS   160
#define PUI_SHEET_RISE 12

/***********************************************************
************************type define*************************
***********************************************************/

/**
 * @brief The colour role a component plays.
 *
 * Six roles rather than a palette of literals: a call site names what a thing *is*
 * (this is the delete action) and the library decides what that looks like. Adding a
 * seventh hue or re-tinting all of them is then one edit in pui_tone.c.
 */
typedef enum {
    PUI_TONE_NEUTRAL = 0, /**< Secondary, inactive, and the plain white card. */
    PUI_TONE_ACCENT,      /**< The app's blue. The camera, the note page, the primary action. */
    PUI_TONE_INDIGO,      /**< Settings, and the second half of the design. */
    PUI_TONE_AMBER,       /**< The to-do family. */
    PUI_TONE_GREEN,       /**< Done, and anything that is working. */
    PUI_TONE_RED,         /**< Destructive actions, and nothing else. */
    PUI_TONE_MAX,
} PUI_TONE_E;

/**
 * @brief How a component is painted in its tone.
 *
 * The four fills a mobile design system has, and the reason a button and a card and a
 * tile can share one colour vocabulary: the tone says which hue, the fill says how much
 * of it.
 */
typedef enum {
    PUI_FILL_SOLID = 0, /**< Saturated fill, white foreground. The loudest. */
    PUI_FILL_SOFT,      /**< A wash of the hue, with the hue itself as the foreground. */
    PUI_FILL_OUTLINE,   /**< Card-coloured fill, a hairline of the hue, hue foreground. */
    PUI_FILL_GHOST,     /**< No fill, no line, hue foreground. */
    PUI_FILL_MAX,
} PUI_FILL_E;

/**
 * @brief How big a component is.
 *
 * Three steps, and the step is a real thing in the layout: a SM button is furniture
 * inside a card, MD is the page's own action, LG is a sheet's primary. Sizes are not
 * per-component numbers because two buttons of the same size on different pages have
 * to be the same height or the UI stops reading as one system.
 */
typedef enum {
    PUI_SIZE_SM = 0,
    PUI_SIZE_MD,
    PUI_SIZE_LG,
    PUI_SIZE_MAX,
} PUI_SIZE_E;

/**
 * @brief A resolved tone and fill: everything a component needs to paint itself.
 *
 * Returned by pui_paint() from a table built at init. A component reads this and never
 * asks what colour anything is.
 */
typedef struct {
    lv_color_t bg;     /**< The fill, or a wash of it for SOFT. */
    lv_color_t fg;     /**< What to draw on top. Never the hue itself on a light wash. */
    lv_color_t line;   /**< The hairline, for OUTLINE. */
    lv_opa_t   bg_opa; /**< LV_OPA_TRANSP for GHOST, LV_OPA_COVER otherwise. */
    lv_coord_t line_w; /**< 0 unless the fill draws a line. */
} PUI_PAINT_T;

/**
 * @brief Which way a container lays its children out.
 */
typedef enum {
    PUI_FLOW_ROW = 0,
    PUI_FLOW_COLUMN,
    PUI_FLOW_ROW_WRAP,
} PUI_FLOW_E;

/***********************************************************
********************class declaration***********************
***********************************************************/

/* This build of LVGL is 9.1.0, which predates LV_OBJ_CLASS_DECLARE / LV_OBJ_CLASS_DEFINE
   (those arrive in 9.2). A class is therefore a plain `const lv_obj_class_t` in the .c
   file and one `LV_ATTRIBUTE_EXTERN_DATA extern` line here — the same shape lv_button
   and lv_label use. See src/widgets/button/lv_button.h:29.

   The instance structs are declared even where nothing outside the .c reads a field:
   the class needs the size, and a caller that has found a component with
   lv_obj_check_type() should be able to reach into it without a cast to a private type. */

/** @brief A transparent container that arranges its children. */
typedef struct {
    lv_obj_t obj;
} pui_box_t;
LV_ATTRIBUTE_EXTERN_DATA extern const lv_obj_class_t pui_box_class;

/**
 * @brief A container that divides its width into equal columns.
 *
 * The two track arrays are this component's own heap. LVGL's grid stores the arrays by
 * pointer — `lv_obj_set_grid_dsc_array()` does not copy — and the row count is not
 * known until the children are, so the arrays cannot be a caller's stack buffer and
 * cannot be static. They are rebuilt whenever a child appears or goes away, and freed
 * with the grid.
 *
 * LVGL's grid also does not place children by itself: `lv_obj_set_grid_cell()` is
 * required per child, and a child that never gets one reads position 0 and stacks in
 * the first cell with every other unplaced child. The grid therefore walks its own
 * children in index order on every change, which is what makes "just add them and they
 * flow" true for a caller.
 */
typedef struct {
    lv_obj_t    obj;
    lv_coord_t *col_dsc;
    lv_coord_t *row_dsc;
    uint8_t     cols;
    uint8_t     rows;
} pui_grid_t;
LV_ATTRIBUTE_EXTERN_DATA extern const lv_obj_class_t pui_grid_class;

/** @brief A rounded surface: white and lifted, or a wash of a tone with a heading. */
typedef struct {
    lv_obj_t   obj;
    lv_obj_t  *band;  /**< The full-bleed heading strip, or NULL for a plain card. */
    lv_obj_t  *head;  /**< The row holding the heading, inside the band. */
    lv_obj_t  *title; /**< The heading label, or NULL for a plain card. */
    lv_obj_t  *body;  /**< Where a card's contents go. See pui_card_body(). */
    PUI_TONE_E tone;
} pui_card_t;
LV_ATTRIBUTE_EXTERN_DATA extern const lv_obj_class_t pui_card_class;

/** @brief A coloured icon square over a caption. */
typedef struct {
    lv_obj_t   obj;
    lv_obj_t  *box; /**< The square, which a caller may draw into. */
    lv_obj_t  *cap; /**< The caption under it, hidden when the tile has none. */
    PUI_TONE_E tone;
    PUI_FILL_E fill;
    uint8_t    graded : 1; /**< Grade the square's fill, for a photo stand-in. */
    uint8_t    wide : 1;   /**< Fill the parent across, for a photo stand-in. */
    uint8_t    selected : 1;
    lv_coord_t box_h; /**< The wide block's height; PUI_TILE_PHOTO_H by default. */
} pui_tile_t;
LV_ATTRIBUTE_EXTERN_DATA extern const lv_obj_class_t pui_tile_class;

/** @brief A line with a coloured figure at its head: "3 项未完成". */
typedef struct {
    lv_obj_t   obj;
    lv_obj_t  *value; /**< The figure, in the large face. */
    lv_obj_t  *text;
    PUI_TONE_E tone;
} pui_stat_row_t;
LV_ATTRIBUTE_EXTERN_DATA extern const lv_obj_class_t pui_stat_row_class;

/**
 * @brief A line with a coloured icon square at its head: a note in a list.
 *
 * The trailing label is what a list of notes carries that a list of settings does not:
 * a time. It sits at the row's far edge, hidden until pui_list_row_set_trailing() gives
 * it something to say, and the text between the square and it is grown to take whatever
 * is left — so a long name shortens against the time rather than pushing it off the row.
 */
typedef struct {
    lv_obj_t   obj;
    lv_obj_t  *box;   /**< The icon square, which a caller may draw into. */
    lv_obj_t  *glyph; /**< The glyph inside it, or NULL while the square is empty. */
    lv_obj_t  *text;
    lv_obj_t  *trail; /**< The right-aligned meta label, hidden until it is given one. */
    PUI_TONE_E tone;
} pui_list_row_t;
LV_ATTRIBUTE_EXTERN_DATA extern const lv_obj_class_t pui_list_row_class;

/** @brief A capsule button. */
typedef struct {
    lv_obj_t   obj;
    lv_obj_t  *label;
    PUI_SIZE_E size;
} pui_button_t;
LV_ATTRIBUTE_EXTERN_DATA extern const lv_obj_class_t pui_button_class;

/** @brief A round button holding one glyph. */
typedef struct {
    lv_obj_t  obj;
    lv_obj_t *glyph;
} pui_icon_button_t;
LV_ATTRIBUTE_EXTERN_DATA extern const lv_obj_class_t pui_icon_button_class;

/** @brief The strip across the top of the screen: clock, badge, link and charge. */
typedef struct {
    lv_obj_t  obj;
    lv_obj_t *badge; /**< The to-do pill, which pui_status_set_badge() retitles. */
    lv_obj_t *clock;
} pui_status_t;
LV_ATTRIBUTE_EXTERN_DATA extern const lv_obj_class_t pui_status_class;

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Build the tone table and the shared styles. Call once, before any component.
 *
 * Everything here is idempotent, and every component constructor calls it, so a
 * component built before the app's own init still gets a working table rather than one
 * built from zeroed memory.
 */
void pui_init(void);

/**
 * @brief Push a colour most of the way toward white, for a tinted surface.
 *
 * The general form, for a caller with a colour that is not one of the six tones — the
 * gallery grades its photo stand-ins from hues the tone table has no name for.
 */
lv_color_t pui_tint(lv_color_t c);

/**
 * @brief The same push, one step less far, for a fill rather than a surface.
 *
 * A soft fill has to sit on a card already washed with its own hue, so at the surface's
 * own value it would be the same colour as what it is on. This is the deeper step, and
 * it is what makes a soft button visible on a tinted card.
 */
lv_color_t pui_tint_fill(lv_color_t c);

/**
 * @brief A tone's colour mixed a given distance toward white. 0 is the hue, 255 is white.
 */
lv_color_t pui_tone_wash(PUI_TONE_E tone, uint8_t mix);

/**
 * @brief The full-strength colour of a tone.
 */
lv_color_t pui_tone_hue(PUI_TONE_E tone);

/**
 * @brief A tone's colour darkened enough to read as text on its own wash.
 *
 * Not the hue itself: amber at full strength on a cream wash is about 1.8:1, which is
 * legible as a shape and not as a word. Each tone carries a hand-picked deep value
 * rather than one mixed down from the hue, because mixing toward black drains the hue
 * and mixing toward a neutral turns amber into mud.
 */
lv_color_t pui_tone_deep(PUI_TONE_E tone);

/**
 * @brief Resolve a tone and a fill to the colours a component paints with.
 *
 * @return A pointer into a table built by pui_init(). Never NULL, even for an
 *         out-of-range tone or fill — the call is clamped, because a wrong colour is a
 *         better failure than a null dereference on a device with no console.
 */
const PUI_PAINT_T *pui_paint(PUI_TONE_E tone, PUI_FILL_E fill);

/**
 * @brief Strip a hand-built container down to what every container here is.
 *
 * Transparent, unpadded, borderless, unscrollable, and taking no input. The last of those
 * is the one that matters: LVGL v9 gives every `lv_obj` LV_OBJ_FLAG_CLICKABLE, and a press
 * is delivered to the *deepest* clickable object under the finger, so a decorative
 * container is an invisible shield over the card that contains it.
 *
 * Every component in this library does this to itself in its constructor — that is a
 * large part of why they exist, because it is a rule that has to hold at every call site.
 * This is the same strip for the case where the constructor is a page: the camera's
 * viewfinder is the app's one dark surface and its sticky note the one yellow one, and
 * neither colour is a tone.
 *
 * Call it first. It zeroes the radius, the border and the padding, so a style set before
 * it is overwritten.
 */
void pui_passthrough(lv_obj_t *obj);

/**
 * @brief Give a hand-built object the library's press feedback.
 *
 * The components here arm their own feedback in their constructors, but a page sometimes
 * builds a control straight from LVGL — a check row, a segmented control, a slider — and
 * those would otherwise be the only things on the page that do not answer a finger. This
 * is the same style the constructors apply, for that case.
 *
 * Two variants, for the same reason there are two constructors: @ref pui_pressable dims and
 * squashes a leaf, @ref pui_pressable_flat only dims, and is what a surface that holds
 * children wants — squashing a row would re-render everything in it offscreen every frame.
 *
 * The object must already be clickable for a press to reach it; both of these leave that
 * flag alone.
 */
void pui_pressable(lv_obj_t *obj);
void pui_pressable_flat(lv_obj_t *obj);

/* ---- Text ------------------------------------------------------------------ */

/**
 * @brief What a piece of text is for, which is all a page should have to say.
 *
 * The face and the colour both come from here, so no page names a font or a colour.
 * Four Chinese faces are in play, which is the whole of the type scale: 32 px semibold
 * for a figure, 24 px medium for a page's name, 20 px medium for a card's, and 18 px
 * regular for everything else. BODY and SUB share the last of those and are told apart
 * by colour alone.
 */
typedef enum {
    PUI_TEXT_BODY = 0, /**< The default: a sentence the user is meant to read. */
    PUI_TEXT_SUB,      /**< A step back: captions, units, the words beside a figure. */
    PUI_TEXT_HINT,     /**< Two steps back: placeholders, and anything not yet set. */
    PUI_TEXT_TITLE,    /**< A page's own name, in the header. */
    PUI_TEXT_CARD,     /**< A card's title, inside the card. */
    PUI_TEXT_GLYPH,    /**< A `FONT_AWESOME_*` macro, at the small icon size. */
    PUI_TEXT_GLYPH_LG, /**< A `FONT_AWESOME_*` macro, at the large icon size. */
    PUI_TEXT_FIGURE,   /**< The 32 px face, for a figure that carries a card. */
    PUI_TEXT_MAX,
} PUI_TEXT_E;

/** @brief A run of text in one of the app's roles. */
typedef struct {
    /* lv_label_t, not lv_obj_t: this class is built on lv_label_class, so the label's own
       fields have to be inside the instance or the base constructor writes past the end of
       the allocation. lv_spinbox does the same with lv_textarea_t. */
    lv_label_t label;
    PUI_TEXT_E role;
} pui_text_t;
LV_ATTRIBUTE_EXTERN_DATA extern const lv_obj_class_t pui_text_class;

/**
 * @brief A run of text.
 *
 * Labels already drop LV_OBJ_FLAG_CLICKABLE in this build, but a label inside a
 * clickable component must never be the object that owns the press, and the call here
 * is what guarantees it.
 *
 * @param[in] text  Copied by lv_label_set_text(), so a stack buffer is fine.
 * @param[in] role  The face and colour. See PUI_TEXT_E.
 */
lv_obj_t *pui_text_create(lv_obj_t *parent, const char *text, PUI_TEXT_E role);

/** @brief Retitle a run of text. NULL is treated as an empty string. */
void pui_text_set(lv_obj_t *text, const char *str);

/**
 * @brief Put the text in a tone's own colour, keeping its face.
 *
 * For the one glyph in a row that is meant to be seen — the bell beside a reminder, the
 * battery beside a charge reading. A tone is not a role: a role is what the text *is*,
 * a tone is which family it belongs to, and the two vary independently.
 */
void pui_text_set_tone(lv_obj_t *text, PUI_TONE_E tone);

/**
 * @brief A label with a face and a colour named outright, rather than through a role.
 *
 * The escape hatch. A role covers a line of body text, a caption and a glyph, and those
 * are almost everything; this is for the labels that are not any of them — a widget whose
 * font and colour are its own parameters, a label on a surface no tone describes. It is
 * the same object pui_text_create() returns, so pui_text_set() and pui_text_set_tone()
 * work on it.
 *
 * A page should reach for a role first. A page that finds itself passing PET_FONT_CJK and
 * PET_COLOR_TEXT is asking for PUI_TEXT_BODY, and one that names a font of its own is
 * usually describing a component that has not been written yet.
 *
 * @param[in] font   Must contain the text: an uncovered glyph renders as a placeholder
 *                   box, not as a blank.
 */
lv_obj_t *pui_label_create(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);

/* ---- Containers ------------------------------------------------------------ */

/**
 * @brief A transparent container that fills its parent and lines its children up.
 *
 * Grows to fill, is unpadded, and passes taps through — the three things a plain
 * `lv_obj_create` gets wrong for this job.
 *
 * @param[in] gap  Spacing between children, in the flow's direction.
 */
lv_obj_t *pui_box_create(lv_obj_t *parent, PUI_FLOW_E flow, lv_coord_t gap);

/**
 * @brief A container that divides its width into @p cols equal columns.
 *
 * The replacement for every "divide the panel by three" the pages used to do: the
 * columns are LV_GRID_FR(1), so they are equal whatever the parent turns out to be
 * wide, and a caller never learns the width at all.
 *
 * Children are placed in reading order; wrap to the next row happens automatically.
 *
 * @param[in] cols  Number of columns. 0 is treated as 1.
 */
lv_obj_t *pui_grid_create(lv_obj_t *parent, uint8_t cols, lv_coord_t gap);

/**
 * @brief A zero-size object that takes the free space in a flex container.
 *
 * What pins a card's button to its foot when the card is taller than its contents —
 * which happens whenever a grid stretches two cards to a shared row height and one of
 * them has less in it than the other.
 *
 * Not the same as LV_FLEX_ALIGN_SPACE_BETWEEN, which distributes the free space between
 * *all* the gaps rather than holding the trailing items at the far edge.
 */
lv_obj_t *pui_spacer(lv_obj_t *parent);

/* ---- Card ------------------------------------------------------------------ */

/**
 * @brief A rounded surface with an optional heading.
 *
 * Two looks in one component, which is what the reference design uses, and the difference
 * between them is *what the card is*:
 *
 *   - with a title, it is an object on the page. White, a hairline, a shadow, and its
 *     tone carried by a full-bleed band across the top — the heading strip. Everything
 *     below the band is the highest-contrast text on the surface.
 *   - with no title, it is a region of the page. Neutral is the plain white card; any
 *     other tone is a wash of it, separated by its own fill rather than by a shadow.
 *
 * Contents are added to the card's body, which is what pui_card_body() returns; the card
 * stacks them in a column with PUI_CARD_GAP between them and pads them by PUI_CARD_PAD.
 * The band is not inside that padding — a heading strip that stopped short of the card's
 * edges would be a label in a box rather than the top of the card — which is also why the
 * card clips its corners: LVGL clips a child to its parent's box and not to its corners,
 * so a square band would sit on top of the card's curve.
 *
 * @param[in] title  The heading, or NULL for a plain card.
 * @param[in] tone   PUI_TONE_NEUTRAL gives the white card whatever the title is.
 */
lv_obj_t *pui_card_create(lv_obj_t *parent, const char *title, PUI_TONE_E tone);

/**
 * @brief Where a card's contents go.
 *
 * Not the card itself: the card's own padding is zero so that its heading band can run
 * edge to edge, and the body is the object that carries the padding instead. A row added
 * to the body and a row added to the card are therefore not the same thing — the row
 * would be flush against the card's border.
 *
 * @return NULL when @p card is not a card.
 */
lv_obj_t *pui_card_body(lv_obj_t *card);

/**
 * @brief Re-tint a card after it was built.
 */
void pui_card_set_tone(lv_obj_t *card, PUI_TONE_E tone);

/**
 * @brief Make the whole card a target.
 *
 * The alternative — a button at the foot of the card — costs a 28 px capsule plus the
 * gap above it, which is 38 px of a card that is only about 150 px tall to begin with.
 * A whole-object target costs nothing vertically; the entry tiles above are whole-object
 * targets for the same reason.
 *
 * This used to hang a chevron at the end of the heading row as the affordance. It does
 * not any more: both cards that use it already carry a footer capsule (查看全部 /
 * 全部笔记) that says the same thing, and the heading is what needs the row's width.
 */
void pui_card_set_action(lv_obj_t *card, lv_event_cb_t cb, void *user_data);

/* ---- Tile ------------------------------------------------------------------ */

/**
 * @brief A coloured icon square over a caption.
 *
 * One component for the four places this shape appeared: the home page's three entries,
 * the gallery's photo thumbnails, the gallery's "add" slot, and the note page's image
 * picker. They were four copies of the same forty lines with different numbers in them.
 *
 * The tile sizes itself — the square is PUI_TILE_ICON and the tile is whatever the
 * square, the gap and the caption come to — so no caller writes a tile height.
 *
 * @param[in] glyph  A `FONT_AWESOME_*` macro. Pass NULL to leave the square empty for
 *                   the caller to fill; the camera entry needs that, because the icon
 *                   face has no camera (see pui_camera_glyph).
 * @param[in] fill   PUI_FILL_SOFT is the app's usual treatment — a wash with the glyph
 *                   in the hue. PUI_FILL_OUTLINE is the empty "add" slot.
 * @param[in] label  The caption. Pass NULL for a tile with no caption.
 */
lv_obj_t *pui_tile_create(lv_obj_t *parent, const char *glyph, PUI_TONE_E tone, PUI_FILL_E fill, const char *label);

/**
 * @brief The tile's coloured square, for a caller that wants to draw into it.
 *
 * @return NULL when @p tile is not a tile.
 */
lv_obj_t *pui_tile_box(lv_obj_t *tile);

/**
 * @brief Make the square a wide block instead, and let the tile fill its parent across.
 *
 * The gallery's thumbnails are this: the same block-over-caption as an entry tile, but
 * the block is a picture rather than an icon, so it takes the cell's whole width and
 * PUI_TILE_PHOTO_H of height, and the caption centres under it. Off by default, because
 * an entry tile's square is PUI_TILE_ICON on both axes and a row of three of them is
 * what the home page's card is.
 *
 * The tile's own width stops being LV_SIZE_CONTENT here, which is why this has to be
 * called rather than a percentage passed to pui_tile_create(): a content-sized tile
 * cannot hold a percentage-width child, and the two would resolve in a circle.
 */
void pui_tile_set_wide(lv_obj_t *tile, BOOL_T on);

/**
 * @brief The wide block's height. PUI_TILE_PHOTO_H unless this says otherwise.
 *
 * Ignored on a square tile, whose block is PUI_TILE_ICON on both axes. The image picker
 * wants PUI_TILE_PICK_H: its sheet shares the panel with a heading and two buttons, and
 * two rows of full-height blocks would push it off the bottom.
 */
void pui_tile_set_box_h(lv_obj_t *tile, lv_coord_t h);

/**
 * @brief Grade the square's fill, for a tile standing in for a photograph.
 *
 * Off by default and deliberately so: pui_camera_glyph punches the lens by painting it
 * in the square's own fill colour, and a graded square has no single fill colour to
 * read back — lv_obj_get_style_bg_color() would hand back the top of the gradient.
 */
void pui_tile_set_graded(lv_obj_t *tile, BOOL_T on);

/**
 * @brief Show or hide the tile's selection ring.
 *
 * A ring on the square and a caption in the tone's deep colour. Nothing else: the two
 * obvious alternatives both fail at this size — a check badge covers the glyph on a
 * 44 px square, and a badge hung off the corner is outside the square it belongs to and
 * gets clipped.
 */
void pui_tile_set_selected(lv_obj_t *tile, BOOL_T on);

/**
 * @brief Replace the tile's caption.
 */
void pui_tile_set_label(lv_obj_t *tile, const char *label);

/**
 * @brief Draw a camera into @p box: a body, a viewfinder bump, and a lens.
 *
 * The icon face has no camera, and the camera is the entry the design leans on, so the
 * shape is assembled rather than substituted. Everything is positioned in fractions of
 * @p size, so the same call works at any scale.
 *
 * @param[in] size   The width to draw into; the caller has already sized @p box.
 * @param[in] color  The glyph's colour, drawn on the box's own fill.
 */
void      pui_camera_glyph(lv_obj_t *box, lv_coord_t size, lv_color_t color);
void      pui_note_glyph(lv_obj_t *box, lv_coord_t size, lv_color_t color);
void      pui_gear_glyph(lv_obj_t *box, lv_coord_t size, lv_color_t color);
lv_obj_t *pui_tool_button_create(lv_obj_t *parent, const char *glyph, const char *text, PUI_TONE_E tone,
                                 PUI_FILL_E fill);

/**
 * @brief Stand the tool button up: the glyph over the label instead of beside it.
 *
 * A taller, squarer button — 46 px against 34 — for a row that has the height to give,
 * and the treatment the reference design uses for the camera's controls, where the
 * buttons share the line with a 56 px shutter and read as three peers rather than as two
 * captions flanking a disc. The note page's toolbar keeps the flat form: it is a bar of
 * four equal actions under a card, where a taller button would eat the page.
 *
 * Off by default, so a caller that wants the flat one writes nothing.
 */
void pui_tool_button_set_stacked(lv_obj_t *btn, BOOL_T on);

/* ---- Rows ------------------------------------------------------------------ */

/**
 * @brief A line with a coloured figure at its head: "3 项未完成".
 *
 * The figure is drawn one size above the card's heading and the description one below it,
 * which is what makes a two-line card read as a summary rather than as a paragraph. The
 * description takes whatever width the row has left over and elides if that is not enough,
 * so a caller can pass a long string without the row spilling out of its card.
 *
 * @param[in] value  The figure. Digits only: the figure's face carries no Chinese.
 */
lv_obj_t *pui_stat_row_create(lv_obj_t *parent, const char *value, const char *text, PUI_TONE_E tone);

/**
 * @brief A line with a coloured icon square at its head, for a list of things.
 *
 * The square is PUI_ROW_ICON, filled with a wash of the tone and edged in a step of the
 * same hue, with the glyph in the tone's deep colour on top — the same figure-and-ground
 * the entry tiles use, one size down. That is what makes a list read as a list of things
 * rather than as more lines of text, and it is the shape the reference design draws.
 *
 * It used to be a bare stripe on the row's leading edge, on the argument that a stripe
 * costs 3 px where a square costs 22 and the row could not afford the difference. It can:
 * see PUI_ROW_ICON. A stripe is also the one leading mark that says nothing about what it
 * marks — it is a colour with no shape, which reads as a highlighter rather than as an
 * icon.
 *
 * The text takes whatever width the row has left and elides into it, so a long name
 * shortens rather than pushing the row out of its card.
 *
 * @param[in] glyph  A `FONT_AWESOME_*` macro, or NULL to leave the square empty.
 */
lv_obj_t *pui_list_row_create(lv_obj_t *parent, const char *glyph, PUI_TONE_E tone, const char *text);

/**
 * @brief Put a short meta string at the row's far edge: the time beside a note's name.
 *
 * It is set in the muted hint colour and one size down, so it reads as a footnote to the
 * name rather than as a second name — the name is what the row is, the time is when it
 * happened. Pass NULL or an empty string to hide it again; a row built without one keeps
 * its text taking the whole width, because a hidden label takes no space in the row.
 */
void pui_list_row_set_trailing(lv_obj_t *row, const char *text);

/* ---- Buttons --------------------------------------------------------------- */

/**
 * @brief A capsule button.
 *
 * One component for the three the theme used to carry (primary, ghost, pill). The tone
 * and the fill say what it looks like and the size says how big it is, so
 * `pui_button_create(p, "删除", PUI_TONE_RED, PUI_FILL_OUTLINE, PUI_SIZE_MD)` and
 * `pui_button_create(p, "保存", PUI_TONE_ACCENT, PUI_FILL_SOLID, PUI_SIZE_MD)` are the
 * same widget in two costumes rather than two widgets.
 *
 * The width is LV_SIZE_CONTENT; use pui_button_set_grow() for a button that shares a
 * row with others.
 */
lv_obj_t *pui_button_create(lv_obj_t *parent, const char *text, PUI_TONE_E tone, PUI_FILL_E fill, PUI_SIZE_E size);

/**
 * @brief Let a button take an equal share of its row.
 *
 * The four note-page toolbar buttons are one of these each.
 */
void pui_button_set_grow(lv_obj_t *btn, BOOL_T on);

/**
 * @brief Let a button span its container instead of sizing to its label.
 *
 * For a button that is the foot of a card rather than an item in a row. It is also what
 * keeps a card narrow: a card is content-sized, so a button left at LV_SIZE_CONTENT sets
 * the card's *minimum* width from its own label plus padding, and in a two-up grid that
 * minimum can be wider than the track — the tracks then grow past the grid and the second
 * card runs off the panel. Full width takes the button out of that sum.
 */
void pui_button_set_full(lv_obj_t *btn, BOOL_T on);

/**
 * @brief Set the button's side padding.
 *
 * Only worth setting on a grown button in a bar that shares its width: there the padding
 * is not the space around the label but the button's *share* of the free space, and a
 * bar of four two-character labels has none to give. The height, the radius and the
 * font are the size step's and stay that way.
 */
void pui_button_set_pad(lv_obj_t *btn, lv_coord_t pad);

/**
 * @brief Replace a button's label.
 */
void pui_button_set_text(lv_obj_t *btn, const char *text);

/**
 * @brief The button's label, for a caller that wants to restyle it.
 */
lv_obj_t *pui_button_label(lv_obj_t *btn);

/**
 * @brief A round button holding a single glyph.
 *
 * @param[in] glyph  A `FONT_AWESOME_*` macro with PET_FONT_ICON, or ASCII with
 *                   PET_FONT_CJK — the icon face has no plus, minus or clock, so the
 *                   stepper and "new note" still come from the CJK face.
 * @param[in] fill   PUI_FILL_SOLID draws the glyph in white; anything else draws it in
 *                   the tone, which is what a light fill needs.
 */
lv_obj_t *pui_icon_button_create(lv_obj_t *parent, const char *glyph, PUI_TONE_E tone, PUI_FILL_E fill);

/* ---- Status bar ------------------------------------------------------------ */

/**
 * @brief The strip across the top of the screen.
 *
 * It belongs to the screen rather than to a page — it is the one piece of chrome that
 * must not move when the panel changes — which is why it is a component with its own
 * layout rather than sixty lines inside the shell.
 *
 * The values are the demo model's, not a clock's or a battery's: there is no RTC in
 * this round and the board registers no power device.
 */
lv_obj_t *pui_status_create(lv_obj_t *parent);

/**
 * @brief Retitle the to-do badge from the model.
 */
void pui_status_set_badge(lv_obj_t *status, uint32_t pending);

/* ---- Page skeleton --------------------------------------------------------- */

/**
 * @brief Apply the page background and the zero padding to a screen object.
 */
void pui_screen_apply(lv_obj_t *scr);

/**
 * @brief A page's header: the title row, with optionally a back arrow.
 *
 * A row of chrome laid on the page's own background — no fill, no rule under it. It was a
 * white bar; on a cream page that made it a second surface the page had not asked for,
 * and it read as a screen sitting on top of the page rather than as the page's own first
 * line. The height is unchanged, because a page's chrome is a fixed height so the content
 * below it does not jump between pages.
 *
 * The arrow is built but does nothing: the library has no idea what going back means in
 * this app. The caller attaches the handler to the object pui_screen_header_back()
 * returns.
 *
 * @param[in] back  TRUE to prepend the arrow.
 * @return The header; append action buttons to it.
 */
lv_obj_t *pui_screen_header(lv_obj_t *page, const char *title, BOOL_T back);

/**
 * @brief The header's back arrow, or NULL if the header was built without one.
 */
lv_obj_t *pui_screen_header_back(lv_obj_t *header);

/**
 * @brief Push the header's action buttons to the right edge.
 *
 * Call it after pui_screen_header() and before adding the buttons; without it they
 * queue up against the title.
 */
void pui_screen_header_push(lv_obj_t *header);

/**
 * @brief The scrolling content area between the header and the action bar.
 *
 * Sized to whatever vertical space is left, so a page only has to fill it.
 */
lv_obj_t *pui_screen_content(lv_obj_t *page);

/**
 * @brief The action bar along the foot of a page.
 *
 * @param[in] gap  Spacing between the buttons.
 */
lv_obj_t *pui_screen_actions(lv_obj_t *page, lv_coord_t gap);

/**
 * @brief Raise a full-height scrim over a page, for a modal or a sheet.
 *
 * The returned object is the scrim: build the card into it and align it. It swallows
 * the tap that dismisses it, so a caller that wants tap-to-close only has to attach a
 * CLICKED handler.
 *
 * @return NULL when @p page is NULL.
 */
lv_obj_t *pui_screen_scrim(lv_obj_t *page);

/**
 * @brief Make a container a scroll region, and give it the app's slim scrollbar.
 *
 * This is the whole of what "this scrolls" means in this library: the SCROLLABLE flag,
 * the CLICKABLE flag that lets the region take the press which starts a scroll, and the
 * bar's styling. Use it instead of setting the flag by hand — a region that is
 * scrollable but not clickable still scrolls under a control, which is easy to mistake
 * for working.
 */
void pui_screen_scrollable(lv_obj_t *obj);

/**
 * @brief Give a scrollable container the app's slim scrollbar.
 *
 * Styling only. LVGL's default scrollbar is a wide opaque bar that fights a card layout;
 * this thins it down and tints it so it reads as an affordance rather than as a border.
 */
void pui_screen_scrollbar(lv_obj_t *obj);

/**
 * @brief Bring a page band up into place: it fades in from nothing while rising the last
 * few pixels into position.
 *
 * This is what a page uses instead of fading the whole page in. A page that fades as one
 * surface has one thing happening; a page whose bands arrive in order has the layout
 * explaining itself as it appears, and the stagger is what carries that — the eye follows
 * the cards down the page rather than watching the whole thing resolve at once.
 *
 * @param[in] index  Position in the page's sequence, 0 for the first. The delay is
 *                   `index * PUI_RISE_STEP`; a page with one card passes 0.
 *
 * A band is a card, a grid, an action bar — a child of the page's content well, not the
 * well itself and not a tile inside a grid. Nothing cleans up: the animation is bound to
 * the object, so a page the user leaves mid-stagger takes its pending arrivals with it.
 */
void pui_anim_rise_in(lv_obj_t *obj, uint8_t index);

/**
 * @brief Mark a value as having just changed: the object grows an eighth and comes back.
 *
 * For a small leaf whose text is set from state — a badge, a chip. Scale is the one
 * transform this library animates, and it is only ever put on a leaf: see pui_anim.c.
 */
void pui_anim_pop(lv_obj_t *obj);

/**
 * @brief Open a sheet: the scrim fades in and the card inside it rises into place.
 *
 * Call once, after the sheet is built. @p scrim is what pui_screen_scrim() returned, and
 * the card is its first child — the sheet does not have to be handed over separately.
 *
 * Only the opening is animated. A closing sheet is deleted outright: its animation would
 * have to keep the object alive until it finished, and the page stack already owns when a
 * sheet goes away.
 */
void pui_anim_sheet_in(lv_obj_t *scrim);

/**
 * @brief Slide an object sideways, for a page transition.
 *
 * The panel is a stack of pages pinned to the same corner, so a page change is expressed as
 * travel: the arriving page enters from the edge the navigation came from and the leaving
 * page exits toward the opposite edge, both on the same clock, so one gesture reads as one
 * movement of the panel rather than two objects doing separate things.
 *
 * Translate, not scale, because a page holds children — see pui_anim.c for that rule. The
 * slide owns the object's `translate_x` for its duration, so do not start two on one object.
 *
 * @param[in] obj       The page to move.
 * @param[in] from      X offset to start at, in pixels. For a push, +PET_PANEL_W.
 * @param[in] to        X offset to finish at, normally 0.
 * @param[in] ms        Duration, both directions of the pair the same.
 * @param[in] ease_in   `true` for the exit curve (accelerating away), `false` for the enter
 *                      curve (arriving fast and settling).
 * @param[in] done      Run when the object reaches @p to, or NULL. What the page stack uses
 *                      to know a leaving page is offscreen and can be retired.
 */
void pui_anim_translate_x(lv_obj_t *obj, lv_coord_t from, lv_coord_t to, uint32_t ms, BOOL_T ease_in,
                          lv_anim_completed_cb_t done);

/**
 * @brief Shake an object horizontally, to refuse something.
 *
 * A short side-to-side wobble, for the one case where a press is not accepted — an empty
 * field, a wrong code. It reads as "no" without a message. Amplitudes here are 3-5 px;
 * larger than that stops being a refusal and starts being a bounce.
 *
 * Uses `translate_x`, so it does not fight a `translate_y` on the same object.
 */
void pui_anim_shake(lv_obj_t *obj, lv_coord_t px, uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* __PUI_H__ */
