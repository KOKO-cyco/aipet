/**
 * @file pet_page_gallery.c
 * @brief The gallery: a grid of thumbnails, each tap selecting one.
 *
 *     +---------------------------------+
 *     | <  相册                [删除选中] |  header
 *     +---------------------------------+
 *     |  +------+ +------+ +------+     |
 *     |  | (img)| | (img)| | (img)|     |
 *     |  +------+ +------+ +------+     |
 *     |    合影     便签    购物条       |
 *     |  +------+ +------+ +------+     |
 *     |  | (img)| | (img)| |  [+] |     |
 *     |  +------+ +------+ +------+     |
 *     |   购物小票  会议记录  添加笔记    |
 *     +---------------------------------+
 *     | [   加备忘录   ] [   AI解读   ]  |  action bar
 *     +---------------------------------+
 *
 * Three columns, and the page does not say how wide one is: the grid divides the panel
 * and the tiles fill whatever cell they land in. The version before this one derived a
 * tile width from PET_PANEL_W and three subtractions, which meant that changing the page
 * margin silently moved the third column off the edge.
 *
 * Reached from the camera page rather than from the home page: photos come from the
 * camera, and 相册 is the one thing a user wants after a shot.
 *
 * A thumbnail is a graded colour block with an image glyph on it, over its caption.
 * There is no image data this round — no JPEG decoder in this build and no filesystem
 * mounted — so the block is a stand-in, and the caption is what the layout needs to be
 * judged. The block is the seam where a real thumbnail goes.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_pages.h"

#include "pet_demo.h"
#include "pet_pet.h"
#include "pui.h"

/***********************************************************
************************macro define************************
***********************************************************/

#define GAL_GAP 8

/* The grid's columns, and how many thumbnails it holds before the add slot. Six tiles
   over three columns is exactly two rows; the panel's content well is 198 px and two
   rows of PUI_TILE_PHOTO_H plus a caption and a gap come to 208, so the grid scrolls by
   ten pixels — which is what a sixth shot needs, and what a seventh would use properly. */
#define GAL_COLS 3

/* The hues the placeholder blocks cycle through. There is no image data to take a colour
   from, and a grid of five identical grey blocks says nothing about being a grid of five
   different photos — variety is the whole job of a placeholder here.

   Four tones and the neutral: RED is deliberately not among them, because it is the
   destructive colour and a red thumbnail would read as one that had been marked. */
static const PUI_TONE_E sg_photo_tones[] = {PUI_TONE_ACCENT, PUI_TONE_INDIGO, PUI_TONE_AMBER, PUI_TONE_GREEN,
                                            PUI_TONE_NEUTRAL};

#define GAL_SAY_MAX 64

/***********************************************************
********************function define**************************
***********************************************************/

static void __goto_note_cb(lv_event_t *e);

static void __say_selected(void)
{
    char     text[GAL_SAY_MAX];
    uint32_t selected = pet_demo_shot_selected();

    if (selected == 0) {
        pet_say("想处理哪一张？点一下选上我吧");
        return;
    }

    lv_snprintf(text, sizeof(text), "选好了%u张，要我帮你做什么？", (unsigned)selected);
    pet_say(text);
}

static void __tile_cb(lv_event_t *e)
{
    lv_obj_t *tile = lv_event_get_current_target(e);
    uint32_t  idx  = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    pet_demo_shot_toggle(idx);

    /* Repainted in place rather than by rebuilding the page: a tile that jumps under the
       finger reads as a mis-tap. */
    pui_tile_set_selected(tile, pet_demo_shots()[idx].selected);

    __say_selected();
}

/**
 * @brief One thumbnail: a graded colour block with an image glyph on it, over its caption.
 *
 * The block is a wash of one hue, graded a little deeper at its foot so it reads as a lit
 * surface rather than as a flat swatch, with the glyph in the tone's deep colour on top.
 */
static lv_obj_t *__build_tile(lv_obj_t *parent, uint32_t index)
{
    const PET_SHOT_T *shot = &pet_demo_shots()[index];
    PUI_TONE_E        tone = sg_photo_tones[index % (sizeof(sg_photo_tones) / sizeof(sg_photo_tones[0]))];
    lv_obj_t         *tile = pui_tile_create(parent, FONT_AWESOME_IMAGE, tone, PUI_FILL_SOFT, shot->caption);

    /* Wide before graded: the gradient is computed from the tone, not from the block's
       size, but the two together are what makes the block a picture. */
    pui_tile_set_wide(tile, TRUE);
    pui_tile_set_graded(tile, TRUE);
    pui_tile_set_selected(tile, shot->selected);

    lv_obj_add_event_cb(tile, __tile_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

    return tile;
}

/**
 * @brief The last cell of the grid: a way out to the note page.
 *
 * The same shape as a photo tile — a block over a caption — with the block outlined and
 * empty instead of filled. Matching the structure matters more than matching the fill:
 * a differently-shaped cell in the last slot reads as a stray object rather than as the
 * empty slot it is.
 */
static lv_obj_t *__build_add_tile(lv_obj_t *parent)
{
    lv_obj_t *tile = pui_tile_create(parent, FONT_AWESOME_EDIT, PUI_TONE_NEUTRAL, PUI_FILL_OUTLINE, "添加笔记");

    pui_tile_set_wide(tile, TRUE);
    lv_obj_add_event_cb(tile, __goto_note_cb, LV_EVENT_CLICKED, NULL);

    return tile;
}

/* ---- Actions --------------------------------------------------------------- */

static void __goto_note_cb(lv_event_t *e)
{
    (void)e;

    pet_ui_go(PET_PAGE_NOTE);
}

/**
 * @brief The delete action, on the header's trailing button.
 *
 * It clears the selection and rebuilds the page so every tile repaints. The photos
 * themselves stay: this round has no storage, so there is nothing to delete them
 * from, and a grid that empties itself would misrepresent what the model holds.
 *
 * This is the page's only delete. The action bar used to carry a second one under the
 * label 删除图片, wired to this same handler — and three grown buttons in a 240 px bar
 * left the longest label two pixels of air on each side, which is the kind of thing that
 * only holds while the font does.
 */
static void __delete_cb(lv_event_t *e)
{
    uint32_t selected = pet_demo_shot_selected();

    (void)e;

    if (selected == 0) {
        pet_say_mood("先点一张照片选上，我再删", PET_SAY_ALERT);
        return;
    }

    pet_demo_shot_clear_selection();
    pet_say("先取消选中，删除等SD卡");
    pet_ui_refresh();
}

static void __to_memo_cb(lv_event_t *e)
{
    (void)e;

    pet_say(pet_demo_shot_selected() > 0 ? "好的，这几张我写进备忘录里了" : "先选一张，我才知道写哪张");
}

static void __ai_read_cb(lv_event_t *e)
{
    (void)e;

    pet_say("读完了，是张购物清单");
}

/* ---- Page ------------------------------------------------------------------ */

void pet_page_gallery_build(lv_obj_t *panel)
{
    lv_obj_t *header;
    lv_obj_t *body;
    lv_obj_t *grid;
    lv_obj_t *bar;
    lv_obj_t *btn;
    uint32_t  count = pet_demo_shot_count();

    header = pet_page_header(panel, "相册", TRUE);
    pet_page_header_push(header);

    btn = pui_button_create(header, "删除选中", PUI_TONE_RED, PUI_FILL_GHOST, PUI_SIZE_SM);
    lv_obj_add_event_cb(btn, __delete_cb, LV_EVENT_CLICKED, NULL);

    body = pet_page_content(panel);

    grid = pui_grid_create(body, GAL_COLS, GAL_GAP);
    /* Kept scrollable: the model's five shots and the add slot fit exactly, but a
       sixth shot would need a third row and the grid is where it would go. */
    pui_screen_scrollable(grid);
    lv_obj_set_flex_grow(grid, 1);

    for (uint32_t i = 0; i < count; i++) {
        __build_tile(grid, i);
    }

    __build_add_tile(grid);

    bar = pet_page_action_bar(panel, PUI_GRID_GAP);
    btn = pui_button_create(bar, "加备忘录", PUI_TONE_ACCENT, PUI_FILL_SOLID, PUI_SIZE_MD);
    /* Grown rather than sized to their labels: three of them share the bar's width
       whatever it turns out to be, so neither the labels nor the panel have to be
       measured. */
    pui_button_set_grow(btn, TRUE);
    lv_obj_add_event_cb(btn, __to_memo_cb, LV_EVENT_CLICKED, NULL);

    btn = pui_button_create(bar, "AI解读", PUI_TONE_NEUTRAL, PUI_FILL_GHOST, PUI_SIZE_MD);
    pui_button_set_grow(btn, TRUE);
    lv_obj_add_event_cb(btn, __ai_read_cb, LV_EVENT_CLICKED, NULL);

    /* Two bands: the grid, then the bar. The tiles inside the grid arrive with it rather
       than one after another — a six-tile cascade is six animations to say what one
       already says, and the grid is read as a block. */
    pui_anim_rise_in(grid, 0);
    pui_anim_rise_in(bar, 1);

    __say_selected();
}
