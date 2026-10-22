/**
 * @file pui_box.c
 * @brief The two containers: pui_box, which arranges, and pui_grid, which divides.
 *
 * Between them they replace every container a page used to build by hand, and with them
 * every width a page used to compute. `(PET_PANEL_W - 2 * PET_PAD_PAGE - 2 * GAL_GAP) / 3`
 * is pui_grid_create(parent, 3, gap); a column with a gap is pui_box_create(parent,
 * PUI_FLOW_COLUMN, gap).
 *
 * The file also carries the two pieces that are not classes: pui_spacer(), and
 * pui_passthrough() — the strip every container here applies to itself, published for the
 * page that has to build one by hand.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pui_priv.h"

/***********************************************************
***********************variable define**********************
***********************************************************/

#define MY_BOX  (&pui_box_class)
#define MY_GRID (&pui_grid_class)

/***********************************************************
*********************box implementation*********************
***********************************************************/

/**
 * @brief The public face of the rule every container in this library follows on itself.
 *
 * A page occasionally has to build a container by hand — the camera's viewfinder is the
 * app's one dark surface and its sticky note the one yellow one, and neither colour is a
 * tone. Such a container still has to be transparent, unpadded, unscrollable and unable to
 * swallow a tap, or it becomes an invisible shield over the card that holds it. This is
 * that strip, for the case where the constructor is the page.
 *
 * Call it first, then set whatever makes the surface special: it zeroes the radius, the
 * border and the padding, so anything set before it is overwritten.
 */
void pui_passthrough(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }

    pui_priv_container(obj);
}

/**
 * @brief An empty container that takes up whatever space its siblings did not.
 *
 * The one thing a content-sized component cannot do by itself: a card whose contents are
 * shorter than the row it sits in needs something to hold the slack, or its trailing
 * button floats up to meet the text instead of lining up with the button in the card
 * beside it. Zero-sized and grown, it costs nothing where there is no slack.
 */
lv_obj_t *pui_spacer(lv_obj_t *parent)
{
    lv_obj_t *spacer = lv_obj_create(parent);

    pui_priv_container(spacer);
    lv_obj_set_size(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    return spacer;
}

static void __box_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    LV_UNUSED(class_p);

    pui_priv_container(obj);

    /* Fills its parent across, and is as tall as what is in it. A container that took
       its parent's height too would stretch the card around it. */
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
}

const lv_obj_class_t pui_box_class = {
    .constructor_cb = __box_constructor,
    .instance_size  = sizeof(pui_box_t),
    .base_class     = &lv_obj_class,
    .name           = "pui_box",
};

lv_obj_t *pui_box_create(lv_obj_t *parent, PUI_FLOW_E flow, lv_coord_t gap)
{
    lv_obj_t *obj = lv_obj_class_create_obj(MY_BOX, parent);

    lv_obj_class_init_obj(obj);

    switch (flow) {
    case PUI_FLOW_COLUMN:
        lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(obj, gap, 0);
        break;

    case PUI_FLOW_ROW_WRAP:
        lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(obj, gap, 0);
        lv_obj_set_style_pad_row(obj, gap, 0);
        break;

    case PUI_FLOW_ROW:
    default:
        lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
        /* Spread, not packed left. Three entry tiles across a card is the shape this
           exists for, and three tiles queued against the left edge with a gap between
           them is not the same picture — the reference design has them sharing the card's
           width. A row that wants to pack left sets its own alignment. */
        lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(obj, gap, 0);
        break;
    }

    return obj;
}

/***********************************************************
********************grid implementation*********************
***********************************************************/

/**
 * @brief Rebuild the row tracks for @p count children and place all of them.
 *
 * Called on every change to the child list, so the row count is always exactly what the
 * children need: a grid that allocated a fixed number of rows would silently drop the
 * items past the end, because lv_grid's item_repos() indexes its row arrays by the
 * position it was given and the arrays are only as long as the template.
 */
static void __grid_place(lv_obj_t *obj)
{
    pui_grid_t *grid = (pui_grid_t *)obj;
    uint32_t    count;
    uint32_t    rows;
    uint32_t    i;

    if (grid->col_dsc == NULL) {
        return;
    }

    count = lv_obj_get_child_count(obj);
    rows  = (count + grid->cols - 1) / grid->cols;
    if (rows == 0) {
        rows = 1;
    }

    if (rows != grid->rows) {
        lv_coord_t *row_dsc = lv_malloc(sizeof(lv_coord_t) * (rows + 1));

        if (row_dsc == NULL) {
            return;
        }

        for (i = 0; i < rows; i++) {
            /* Content, not FR: a row is as tall as the tallest tile in it, and the grid
               is then exactly as tall as its rows — which is what lets a page put a grid
               in a column and get the right height without measuring anything. */
            row_dsc[i] = LV_GRID_CONTENT;
        }
        row_dsc[rows] = LV_GRID_TEMPLATE_LAST;

        lv_free(grid->row_dsc);
        grid->row_dsc = row_dsc;
        grid->rows    = (uint8_t)rows;

        /* The array is stored by pointer, so it has to be handed over again every time
           it is replaced. */
        lv_obj_set_grid_dsc_array(obj, grid->col_dsc, grid->row_dsc);
    }

    for (i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);

        /* Reading order, one cell each. A child that is not placed reads position 0 and
           shares the first cell with every other unplaced child, which is why this walk
           is not optional. */
        lv_obj_set_grid_cell(child, LV_GRID_ALIGN_STRETCH, (int32_t)(i % grid->cols), 1, LV_GRID_ALIGN_STRETCH,
                             (int32_t)(i / grid->cols), 1);
    }
}

static void __grid_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    LV_UNUSED(class_p);

    pui_priv_container(obj);

    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, LV_SIZE_CONTENT);

    /* START, not STRETCH. LV_GRID_ALIGN_STRETCH means "make the cell fill its track",
       which is a statement about a *cell* and is handled in lv_grid.c's item_repos().
       The grid's own alignment goes through grid_align() instead, and that function has
       no case for STRETCH — it falls through to an empty default and never writes
       pos_array[0], which is lv_malloc'd and therefore whatever was on the heap. The
       tracks then come out the right *size* at a garbage origin. The columns here are
       LV_GRID_FR(1) and already fill the width, so START is both correct and safe. */
    lv_obj_set_grid_align(obj, LV_GRID_ALIGN_START, LV_GRID_ALIGN_START);
}

static void __grid_destructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    pui_grid_t *grid = (pui_grid_t *)obj;

    LV_UNUSED(class_p);

    lv_free(grid->col_dsc);
    lv_free(grid->row_dsc);
    grid->col_dsc = NULL;
    grid->row_dsc = NULL;
}

static void __grid_event(const lv_obj_class_t *class_p, lv_event_t *e)
{
    lv_event_code_t code;

    lv_obj_event_base(class_p, e);

    code = lv_event_get_code(e);

    /* CHILD_CHANGED is not handled: it fires when a child resizes, which happens on every
       layout pass of a content-sized tile, and the answer is always the same one. Only a
       change to the *list* can change which cell a child belongs in.

       The current target, not the target. CHILD_CREATED and CHILD_DELETED are two of the
       event codes LVGL always bubbles (event_is_bubbled() in lv_obj_event.c), so this
       handler runs for every object created anywhere beneath the grid as well — with the
       original target still naming that descendant. lv_event_get_target() would hand back
       the descendant, and casting a tile to a grid to read its column count is reading a
       field that is not there. The current target is the object whose class chain is being
       walked, which is the grid, and it is the only one of the two that is this class. */
    if (code == LV_EVENT_CHILD_CREATED || code == LV_EVENT_CHILD_DELETED) {
        lv_obj_t *self = lv_event_get_current_target(e);

        if (lv_obj_check_type(self, MY_GRID)) {
            __grid_place(self);
        }
    }
}

const lv_obj_class_t pui_grid_class = {
    .constructor_cb = __grid_constructor,
    .destructor_cb  = __grid_destructor,
    .event_cb       = __grid_event,
    .instance_size  = sizeof(pui_grid_t),
    .base_class     = &lv_obj_class,
    .name           = "pui_grid",
};

lv_obj_t *pui_grid_create(lv_obj_t *parent, uint8_t cols, lv_coord_t gap)
{
    pui_grid_t *grid;
    lv_obj_t   *obj;
    uint32_t    i;

    if (cols == 0) {
        cols = 1;
    }

    obj  = lv_obj_class_create_obj(MY_GRID, parent);
    grid = (pui_grid_t *)obj;

    /* Initialised first, and parameterised after: the constructor runs inside init_obj()
       and lv_theme_apply() runs inside it too, so anything set before it would be
       writing into a style list the theme is about to add to. */
    lv_obj_class_init_obj(obj);

    grid->cols = cols;
    grid->rows = 0;

    /* Equal shares whatever the parent turns out to be wide — this is the whole reason
       the pages stop needing to know PET_PANEL_W. */
    grid->col_dsc = lv_malloc(sizeof(lv_coord_t) * (cols + 1));
    if (grid->col_dsc == NULL) {
        /* Nothing to lay out with. The grid stays a plain container: children will
           stack, which is visible and recoverable, rather than the object failing to
           exist at all. __grid_place() reads the NULL and does nothing. */
        return obj;
    }

    for (i = 0; i < cols; i++) {
        grid->col_dsc[i] = LV_GRID_FR(1);
    }
    grid->col_dsc[cols] = LV_GRID_TEMPLATE_LAST;

    lv_obj_set_style_pad_column(obj, gap, 0);
    lv_obj_set_style_pad_row(obj, gap, 0);

    __grid_place(obj);

    return obj;
}
