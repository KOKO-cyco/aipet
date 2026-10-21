/**
 * @file pet_ui.c
 * @brief The shell: the status strip, the character's card, and the stack of pages
 *        that fills the panel beside it.
 *
 *     +-----------------------------------------------+
 *     | 10:30            待办 3  (bt)已连接 (bat)电量 80% |  status
 *     +-----------------+-----------------------------+
 *     | +-------------+ | < 备忘录         [ 保存 ]    |  header
 *     | |  说……       | +-----------------------------+
 *     | +---\---------+ |                             |
 *     |     \           |  page content               |  content
 *     |   (character)   |                             |
 *     | +-------------+ +-----------------------------+
 *     |                 | [ 待办 ][ 提醒 ][ 图片 ][AI] |  action bar
 *     +-----------------+-----------------------------+
 *
 * The status strip belongs to the screen, not to a page: it is the one thing that must
 * not move when the panel changes, so it is a sibling of both columns rather than
 * living inside either.
 *
 * The character's card is built once, at start-up, and never torn down. That is what
 * makes a line set from a function page visible at the moment it is set rather than on
 * the way back, and it is also why pet_say() has no "is the character on screen" case
 * here — it always is.
 *
 * The panel is a stack. 主页 is the root; its three tiles open 拍照, 备忘录 and 设置, and
 * 相册 hangs off the camera page. Each of those carries a back arrow that pops it, so
 * the stack is what remembers how the user got somewhere.
 *
 * A page that has just been replaced is hidden and kept for one more navigation rather
 * than deleted on the spot. See __trash_add() — freeing an object the pointer still
 * names costs the user their next tap.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_ui.h"

#include "app_statusbar.h"

#include "pet_demo.h"
#include "pet_pages.h"
#include "pet_pet.h"
#include "pet_widgets.h"
#include "pui.h"

/* PR_WARN, for the one diagnostic in this file. */
#include "tal_log.h"

/***********************************************************
************************macro define************************
***********************************************************/

/* How deep the stack can get. 主页 is the root, 拍照 hangs off it and 相册 off 拍照, so
   three would do; four leaves room for a sub-page when one arrives, and a fixed array
   means navigation never allocates. */
#define PET_PAGE_STACK_MAX 4

/* 切页动画开关。
 *
 * 1 = 面板左右滑动（demo 原样，300ms）；0 = 直接换页。
 * 这块屏是软件渲染 480x320：滑动时**每一帧都要重绘新旧两页**（各 250x286），
 * 实测整机只有 ~10fps，宠物 42ms 的拍子也被拖到 100ms 左右。默认关掉换性能；
 * 想要滑动观感把它改回 1（代价就是切页那 300ms 掉帧）。 */
#define PET_NAV_ANIM 0

/* How many replaced objects are held before they are freed. Two is what normal use
   needs — a page that was just left, and a sheet that was just dismissed — and the
   rest is headroom for a navigation that happens while a sheet is still fading. */
#define PET_TRASH_MAX 4

/* The badge's text, "待办 32" at the very most. */
#define PET_BADGE_MAX 24

/***********************************************************
***********************variable define**********************
***********************************************************/

static lv_obj_t *sg_shell;  /* the status strip over the two columns */
static lv_obj_t *sg_column; /* the character's card, built once */
static lv_obj_t *sg_host;   /* the panel's, which pages come and go inside */
static lv_obj_t *sg_page;   /* the page currently in the panel */
static lv_obj_t *sg_sheet;  /* the scrim over it, when one is raised */

/* Which way the last navigation went, so __rebuild() knows which edge to slide from:
   +1 a push (new page from the right), -1 a pop (from the left), 0 no travel — a
   refresh, or the first build. Not cleared here; it is set by the call that causes the
   rebuild and read once, so it does not have to be reset afterwards. */
static int sg_nav_dir;

/* The page that is sliding out, if one is. It is not in the trash yet — it has to stay
   alive and drawn until its exit finishes — so it is named here instead, and moved to
   the trash by the animation's completed callback. A navigation that arrives before the
   slide is over retires it early, and cancels the animation so the callback cannot fire
   on it afterwards. */
static lv_obj_t *sg_pending;

static lv_obj_t *sg_status; /* the strip across the top, built once */

/* Objects that have left the screen but are not safe to free yet. See __trash_add(). */
static lv_obj_t *sg_trash[PET_TRASH_MAX];
static uint32_t  sg_trash_n;

static PET_PAGE_E sg_stack[PET_PAGE_STACK_MAX];
static int        sg_depth; /* number of live entries; top is depth-1 */

/* 页面构建完成后的回调（宿主页注册，用来触发进页面动作）。 */
static PET_UI_PAGE_HOOK sg_page_hook;

void pet_ui_set_page_hook(PET_UI_PAGE_HOOK cb)
{
    sg_page_hook = cb;
}

BOOL_T pet_ui_sheet_active(void)
{
    return (BOOL_T)(sg_sheet != NULL);
}

void pet_ui_close_sheet(void)
{
    if (NULL != sg_sheet) {
        pet_page_sheet_close();
    }
}

/***********************************************************
********************function define**************************
***********************************************************/

static void __rebuild(void);

/* ---- The delayed free ----------------------------------------------------- */

/**
 * @brief Take an object off the screen now and free it on the next navigation.
 *
 * Deleting an object that the input device still names costs the user their next tap.
 * obj_delete_core() walks every pointer indev and calls obj_indev_reset() when
 * `pointer.last_obj` is the object being freed (lv_obj_tree.c:527), which arms
 * wait_until_release; indev_proc_press() then returns early for as long as that is set
 * (lv_indev.c:1169), and it is only cleared by a release (lv_indev.c:1330). So the
 * press half of the next tap is dropped and the user has to tap twice.
 *
 * `last_obj` is not cleared on release unless that flag is already set, which is what
 * makes this reachable at all: after any ordinary tap it still names the object that
 * was tapped. The page that was just navigated away from is therefore exactly the
 * object the pointer is holding — the tap that caused the navigation landed in it.
 *
 * Holding it for one more navigation sidesteps the whole thing. By the time the entry
 * at the head of the list is freed, several taps have gone by and every one of them
 * landed somewhere else. The object is hidden so it neither draws nor takes input in
 * the meantime; the cost is one page's worth of memory, for one navigation.
 */
static void __trash_add(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);

    if (sg_trash_n >= PET_TRASH_MAX) {
        /* The list only fills if the user navigates several times without a tap in
           between, so the oldest entry is by then safe to free outright. */
        lv_obj_delete(sg_trash[0]);
        for (uint32_t i = 1; i < PET_TRASH_MAX; i++) {
            sg_trash[i - 1] = sg_trash[i];
        }
        sg_trash_n--;
    }

    sg_trash[sg_trash_n] = obj;
    sg_trash_n++;
}

/**
 * @brief Free everything held by __trash_add().
 *
 * Called when a new page is about to be built, which is only ever in response to a tap
 * that landed in the page being replaced — so nothing in the list can be what the
 * pointer is naming.
 */
static void __trash_empty(void)
{
    for (uint32_t i = 0; i < sg_trash_n; i++) {
        lv_obj_delete(sg_trash[i]);
    }

    sg_trash_n = 0;
}

/* ---- Page stack ----------------------------------------------------------- */

static PET_PAGE_E __top(void)
{
    return sg_stack[sg_depth - 1];
}

void pet_ui_go(PET_PAGE_E page)
{
    if (page >= PET_PAGE_MAX || sg_depth <= 0) {
        return;
    }

    /* Already there. Pushing a second copy would leave the user two backs away from
       where they started. */
    if (__top() == page) {
        return;
    }

    if (sg_depth >= PET_PAGE_STACK_MAX) {
        /* Dropping the navigation is better than dropping the page the user is
           looking at, which is what replacing the top would do. */
        PR_WARN("pet: page stack full, ignoring navigation");
        return;
    }

    sg_stack[sg_depth] = page;
    sg_depth++;
    sg_nav_dir = 1;

    __rebuild();
}

void pet_ui_back(void)
{
    /* 主页 is the root and has no back arrow; a back that arrived from somewhere else
       has nothing to pop and is ignored rather than treated as a request to leave. */
    if (sg_depth <= 1) {
        return;
    }

    sg_depth--;
    sg_nav_dir = -1;
    __rebuild();
}

void pet_ui_refresh(void)
{
    if (sg_depth <= 0) {
        return;
    }

    /* No travel: a refresh is the same page with new data, and sliding it would say the
       user went somewhere when they did not. */
    sg_nav_dir = 0;
    __rebuild();
}

void pet_ui_badge_refresh(void)
{
    app_statusbar_set_todo(pet_demo_todo_pending());
}

lv_obj_t *pet_ui_panel(void)
{
    return sg_page;
}

/* ---- Page lifecycle ------------------------------------------------------- */

/**
 * @brief Retire the page that has just finished sliding off the panel.
 *
 * The exit is what keeps a leaving page alive after it has been replaced, so this is where
 * it finally joins the trash — by which point the slide is over and the pointer is
 * nowhere near it. The object is carried in the animation's user data rather than read
 * from a file-static, so a callback that somehow outlives its navigation still retires the
 * right thing. See pui_anim_translate_x().
 */
static void __page_out_done(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)lv_anim_get_user_data(a);

    if (obj == sg_pending) {
        sg_pending = NULL;
    }

    __trash_add(obj);
}

static void __rebuild(void)
{
    PET_PAGE_E kind;
    lv_obj_t  *old = sg_page;

    __trash_empty();

    /* A slide still running when the next navigation arrives: stop it (so its completed
       callback cannot fire against a page we are about to retire) and put the page it was
       moving into the trash, which we just emptied. */
    if (sg_pending != NULL) {
        lv_anim_delete(sg_pending, NULL);
        __trash_add(sg_pending);
        sg_pending = NULL;
    }

    sg_page  = NULL;
    sg_sheet = NULL;

    if (sg_depth <= 0) {
        __trash_add(old);
        return;
    }

    kind = __top();
    lv_obj_set_style_bg_opa(sg_column, kind == PET_PAGE_HOME ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(sg_column, kind == PET_PAGE_HOME ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(sg_column, kind == PET_PAGE_HOME ? PET_SHADOW_OPA : LV_OPA_TRANSP, 0);
    sg_page = lv_obj_create(sg_host);
    pui_screen_apply(sg_page);

    switch (kind) {
    case PET_PAGE_CAMERA:
        pet_page_camera_build(sg_page);
        break;
    case PET_PAGE_GALLERY:
        pet_page_gallery_build(sg_page);
        break;
    case PET_PAGE_NOTES:
        pet_page_notes_build(sg_page);
        break;
    case PET_PAGE_NOTE:
        pet_page_note_build(sg_page);
        break;
    case PET_PAGE_SETTINGS:
        pet_page_settings_build(sg_page);
        break;
    case PET_PAGE_HOME:
    default:
        pet_page_home_build(sg_page);
        break;
    }

    /* 页面已经建好，宿主页可以据此放"进页面动作"（拍照 / 记笔记）。
     * **只在 push 时触发**：回退到某一页、以及刷新重建（字库就绪）都不重放动作。 */
    if (sg_page_hook != NULL && sg_nav_dir > 0) {
        sg_page_hook(kind);
    }

    /* A push or a pop slides; a refresh, the first build, and the empty-stack case do not.
       The two pages move as a pair on the same clock and the same width, so the panel
       reads as one surface travelling rather than as one page arriving over another. The
       new page was created after the old, so it is drawn on top and the overlap during the
       slide shows the page that is leaving, not the host behind it. */
    if (PET_NAV_ANIM && old != NULL && sg_nav_dir != 0) {
        lv_coord_t w = PET_PANEL_W;

        pui_anim_translate_x(sg_page, sg_nav_dir > 0 ? w : -w, 0, PET_NAV_MS, FALSE, NULL);
        pui_anim_translate_x(old, 0, sg_nav_dir > 0 ? -w : w, PET_NAV_MS, TRUE, __page_out_done);
        /* No room parallax: the character's card is a canvas of the real sprite on
           this device, so there is no separate background plane to lean against the
           page. The panel alone carries the travel. */
        sg_pending = old;
    } else {
        __trash_add(old);
    }
}

/* ---- Shared page furniture ------------------------------------------------ */

static void __back_click_cb(lv_event_t *e)
{
    (void)e;

    pet_ui_back();
}

lv_obj_t *pet_page_header(lv_obj_t *panel, const char *title, BOOL_T back)
{
    lv_obj_t *header = pui_screen_header(panel, title, back);

    /* The arrow is the library's to build and this file's to wire: going back is a
       statement about this app's page stack, and pui has no idea there is one. */
    if (back) {
        lv_obj_add_event_cb(pui_screen_header_back(header), __back_click_cb, LV_EVENT_CLICKED, NULL);
    }

    return header;
}

void pet_page_header_push(lv_obj_t *header)
{
    pui_screen_header_push(header);
}

/**
 * @brief A transparent container that fills its parent and lays its children out in
 *        a line.
 *
 * A thin alias over pui_box_create(): the two are the same box, but a page's row is
 * always its parent's full width and its parent's to size, which is what the box
 * already does.
 */
lv_obj_t *pet_page_row(lv_obj_t *parent, lv_coord_t gap)
{
    return pui_box_create(parent, PUI_FLOW_ROW, gap);
}

lv_obj_t *pet_page_col(lv_obj_t *parent, lv_coord_t gap)
{
    return pui_box_create(parent, PUI_FLOW_COLUMN, gap);
}

lv_obj_t *pet_page_content(lv_obj_t *panel)
{
    return pui_screen_content(panel);
}

lv_obj_t *pet_page_action_bar(lv_obj_t *panel, lv_coord_t gap)
{
    return pui_screen_actions(panel, gap);
}

lv_obj_t *pet_page_sheet(void)
{
    if (sg_page == NULL) {
        return NULL;
    }

    /* One at a time: a second sheet would stack over the first with no way back to
       it, and every caller here replaces rather than nests. Emptying the trash first
       is safe — the tap that got here landed on the page, not on anything in it. */
    __trash_empty();

    if (sg_sheet != NULL) {
        __trash_add(sg_sheet);
        sg_sheet = NULL;
    }

    sg_sheet = pui_screen_scrim(sg_page);

    /* The page lays out in a flex column, and without this the scrim becomes the next
       item in it — placed below the action bar and scrolled off the bottom, instead of
       covering the page. IGNORE_LAYOUT takes it out of the flow and leaves it at its
       own coordinates, which are the page's origin. */
    lv_obj_add_flag(sg_sheet, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(sg_sheet, 0, 0);
    lv_obj_move_foreground(sg_sheet);

    return sg_sheet;
}

void pet_page_sheet_close(void)
{
    if (sg_sheet == NULL) {
        return;
    }

    /* Held rather than freed, and hidden immediately so the modal is gone from the
       screen at once. The usual caller is a button inside the sheet, so the pointer is
       naming an object that is about to be freed — see __trash_add(). */
    __trash_add(sg_sheet);
    sg_sheet = NULL;
}

BOOL_T pet_page_sheet_open(void)
{
    return (NULL != sg_sheet) ? TRUE : FALSE;
}

/* ---- Shell ---------------------------------------------------------------- */

void pet_ui_init(lv_obj_t *screen)
{
    lv_obj_t *body;

    if (screen == NULL) {
        screen = lv_screen_active();
    }

    pui_init();
    pet_demo_init();

    pui_screen_apply(screen);

    sg_trash_n = 0;
    sg_status  = NULL;

    /* The shell is a column: the status strip at its own height, then everything else.
       It never changes, so neither does the character. */
    sg_shell = lv_obj_create(screen);
    pui_passthrough(sg_shell);
    lv_obj_set_size(sg_shell, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(sg_shell, PET_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(sg_shell, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(sg_shell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sg_shell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* The real strip, not the demo's: clock, weather, wifi, volume and charge come
       from the device's services (see app_statusbar.c). It is still a flex child of
       the shell at its own fixed height, so nothing below it moves. */
    sg_status = app_statusbar_create(sg_shell);

    /* Everything under the strip. Its own padding is what carves the page margin out
       of the screen, so a page is built against this box rather than against a number
       the theme worked out for it.

       The bottom margin is 8 rather than the page's 12: the pet card has to be tall
       enough for the sprite's 199 px anchor box plus the bubble holder above it, and
       four pixels of page margin are worth less than four pixels of character. */
    body = lv_obj_create(sg_shell);
    pui_passthrough(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_hor(body, PET_PAD_PAGE, 0);
    lv_obj_set_style_pad_top(body, PET_STATUS_GAP, 0);
    lv_obj_set_style_pad_bottom(body, 8, 0);
    lv_obj_set_style_pad_column(body, PET_GAP_PANEL, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* The character's card. A rounded surface a shade deeper than the page, so the
       sprite composites onto a room of its own rather than onto the same surface as
       everything else. */
    sg_column = lv_obj_create(body);
    lv_obj_remove_flag(sg_column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(sg_column, PET_COLUMN_W, LV_PCT(100));
    lv_obj_set_flex_grow(sg_column, 0);
    lv_obj_set_style_bg_color(sg_column, PET_COLOR_PET_COLUMN, 0);
    lv_obj_set_style_bg_opa(sg_column, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sg_column, 1, 0);
    lv_obj_set_style_border_color(sg_column, PET_COLOR_LINE, 0);
    lv_obj_set_style_shadow_width(sg_column, PET_SHADOW_W, 0);
    lv_obj_set_style_shadow_offset_y(sg_column, PET_SHADOW_DY, 0);
    lv_obj_set_style_shadow_color(sg_column, PET_COLOR_SHADOW, 0);
    lv_obj_set_style_shadow_opa(sg_column, PET_SHADOW_OPA, 0);
    lv_obj_set_style_radius(sg_column, PET_RADIUS_CARD, 0);
    /* The sprite canvas is a rectangle at the card's foot; without this its square
       corners paint over the card's rounded bottom corners. */
    lv_obj_set_style_clip_corner(sg_column, true, 0);
    lv_obj_set_style_pad_all(sg_column, 0, 0);
    lv_obj_set_style_pad_row(sg_column, 0, 0);
    lv_obj_set_flex_flow(sg_column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sg_column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Built once. Everything the character needs is inside, and pet_say() writes into
       it from any page without having to check whether it is on screen. */
    pet_page_pet_build(sg_column);

    sg_host = lv_obj_create(body);
    pui_passthrough(sg_host);
    lv_obj_set_size(sg_host, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(sg_host, 1);

    sg_page  = NULL;
    sg_sheet = NULL;

    sg_stack[0] = PET_PAGE_HOME;
    sg_depth    = 1;

    /* The badge was built with an empty label; the model is seeded by now, so this is
       the first and only time it needs telling what to say. */
    pet_ui_badge_refresh();

    __rebuild();
}

void pet_ui_deinit(void)
{
    /* Everything — the strip, the card, the pages in the trash — is a child of the
       shell, so one delete takes it all. The pointers are cleared first: an event or a
       timer that fires between the delete and the next init must not dereference
       anything that is about to go. */
    sg_page    = NULL;
    sg_sheet   = NULL;
    sg_pending = NULL;
    sg_status  = NULL;
    sg_trash_n = 0;
    sg_depth   = 0;

    /* The bubble's four statics live in pet_page_pet.c and are not children of the
       shell's object tree. Reset them before the objects go away. */
    pet_page_pet_reset();

    /* LVGL does not cancel animations when their object is deleted: an arrival fade
       still running on a card inside a page we are about to free would write into
       freed memory on its next tick. Nothing on this screen needs an animation after
       this point, so drop them all — `lv_anim_delete_all()` removes the entries
       without firing completed callbacks. */
    lv_anim_delete_all();

    if (sg_shell != NULL) {
        lv_obj_delete(sg_shell);
        sg_shell = NULL;
    }

    sg_host   = NULL;
    sg_column = NULL;
}
