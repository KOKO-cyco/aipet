/**
 * @file pet_page_camera.c
 * @brief The capture page: a viewfinder with the mode on it, a shutter, and the sheet
 *        that confirms a shot.
 *
 *     +---------------------------------+
 *     | +-----------------------------+ |
 *     | | (回) [ 拍照 ]                | |
 *     | |                    +-------+| |
 *     | |                    | 购物清单|| |  one card:
 *     | |                    | 牛奶  || |  the viewfinder
 *     | |                    +-------+| |  and its controls
 *     | |  演示预览                    | |
 *     | +-----------------------------+ |
 *     | [相册]        ( o )      [AI识别] |  the card's control row
 *     +---------------------------------+
 *
 * There is no header on this page. The whole of what a header carried here was the back
 * arrow — 拍照 is already written on the frame's own mode chip, and the page has no second
 * action — so the arrow floats on the preview's top-left corner instead, and the 36 px the
 * row would have cost go to the preview. The preview and the control row are one card
 * rather than three things on the page, because that is what they are: an instrument, not
 * a list of parts.
 *
 * 相册 lives here rather than on the home page: photos come from the camera, and the
 * one thing a user wants after a shot is to see the ones they already have.
 *
 * The mode is a chip on the viewfinder rather than the four-segment strip it used to
 * be. At 240 px of card width four Chinese labels come to more than the strip has, and
 * a control that has to abbreviate its own options is worse than one that cycles: the
 * chip shows the full name, and a tap moves to the next.
 *
 * The shutter raises a scrim rather than pushing a page: the confirmation belongs to
 * the shot the user just took, and coming back from it should return them to the
 * viewfinder, not to a page they navigated to.
 *
 * ## What is a component here, and what is not
 *
 * The card, the content well, the control row's two buttons and the whole confirmation
 * sheet are components: the page names a tone, a fill and a size and learns nothing else.
 * The confirmation card used to be the exception — its width was CAM_MODAL_W, which is the
 * panel less the scrim's margin worked out by hand — and it is now LV_PCT(100) of a scrim
 * that does its own insetting, which comes to the same 240 px without the page knowing why.
 *
 * Four things stay built by hand, because each of them exists exactly once and none of
 * their colours is a tone: the viewfinder, which is the app's only dark surface; the
 * sticky note pinned into it, which is the only yellow one; the shutter, which stands
 * in for a physical button and is the only place the accent orange is used; and the back
 * arrow, which is the only chrome on the page and is placed against the preview rather
 * than against a page margin. They are together at the top of the file so the colours no
 * role covers are named in one place, and each is still run through pui_passthrough() so
 * it cannot swallow a tap.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_pages.h"

#include "tal_api.h"            /* tal_psram_malloc —— 照片画布 */

#include "app_camera.h"         /* 实时预览 + 快门存卡（src/modules） */

#include "pet_demo.h"
#include "pet_pet.h"
#include "pet_snapshot.h"
#include "pui.h"

/***********************************************************
************************macro define************************
***********************************************************/

#define CAM_SAY_MAX 96

/* The three hand-built pieces. See the file comment for why they are not components. */
#define CAM_FRAME_PAD 8
#define CAM_CHIP_H    22
#define CAM_CHIP_PAD  5

/* Where the mode chip starts, and the back arrow it makes room for. Both are pinned to the
   preview's top-left corner rather than laid out, because the scene is not a flex
   container — every one of its children is aligned or positioned — so nothing here can
   move anything else. The chip's x is the arrow's own corner plus its width plus a gap,
   which is the whole of the arithmetic: the arrow is PUI_ICON_BTN across and the gap is
   the same 6 the header used between its arrow and its title. */
#define CAM_CORNER 4
#define CAM_CHIP_X (CAM_CORNER + PUI_ICON_BTN + 6)

/* The sticky note. The note is what the layout is really about — a photo of a handwritten
   list is the case the camera exists for — so it gets the corner of the frame opposite the
   arrow. Its width is named because a note is a fixed-width thing and its lines are
   shorter than it; its height is not, because the lines and their padding already say how
   tall it is. */
#define CAM_NOTE_W   60
#define CAM_NOTE_PAD 5
#define CAM_NOTE_GAP 3

/* The shutter: the disc, how far its ring is held in from the edge, and the ring's own
   width. The ring is derived from the disc rather than sized separately, so the two
   cannot drift apart. It is also the control row's height, because the shutter is what
   the row is built around and the two buttons beside it are sized against it. */
#define CAM_SHUTTER     56
#define CAM_SHUTTER_GAP 8
#define CAM_RING_W      3

/* Between the viewfinder and the control row. Tighter than the card's own gap: the two are
   one instrument rather than two rows, which is why the card is built rather than letting
   the content well's card gap do it. */
#define CAM_STAGE_GAP 6

/* Between the control row's two buttons and the shutter. */
#define CAM_ROW_GAP 12

/***********************************************************
***********************variable define**********************
***********************************************************/

/* The viewfinder's mode chip, so the mode handler can retitle it in place, and the label
   inside it, which is what carries the name. */
static lv_obj_t *sg_mode_chip;
static lv_obj_t *sg_mode_label;

/* The one-shot that turns the recognition's THINK line into its answer. The pet's card
   outlives this page, so the timer can be left to fire even if the user has moved on — it
   only ever calls pet_say_mood(), which is safe from anywhere. It is cancelled on the
   next press and on the next build so two thoughts cannot be in flight at once. */
static lv_timer_t *sg_ai_timer;

/* 取景框的 overlay 状态（声明在这里是因为快门的回调也要用到）。
 *   sg_cam_placed —— 预览是否已经接上；建页那一刻量出来的取景框偏高（下面那行按钮
 *                    还没建出来），所以落位推迟到页面建完后的第一拍；
 *   sg_cam_paused —— 弹窗盖在取景框上时让位中，见 __preview_pause。 */
static BOOL_T sg_cam_placed;
static BOOL_T sg_cam_paused;

static void __preview_pause(BOOL_T pause);

/***********************************************************
********************function define**************************
***********************************************************/

static void __say_mode(void)
{
    char text[CAM_SAY_MAX];

    /* Fourteen characters, the bubble's ceiling. The mode name is the longest part of
       it and the chip in the viewfinder already carries it, so the line leans on the
       chip rather than repeating the whole sentence. */
    lv_snprintf(text, sizeof(text), "%s，按快门就行", pet_demo_mode_name(pet_demo_mode()));
    pet_say(text);
}

/**
 * @brief Move to the next mode.
 *
 * The chip shows the full name, so it has to be retitled rather than just restyled —
 * the old strip painted its own highlight and only needed a repaint.
 */
static void __mode_cb(lv_event_t *e)
{
    (void)e;

    pet_demo_mode_next();

    if (sg_mode_label != NULL) {
        pui_text_set(sg_mode_label, pet_demo_mode_name(pet_demo_mode()));
    }

    /* The chip, not the label inside it. The label is centred in a chip sized to content,
       so it has no room to grow into: a label that scaled would be clipped by the chip
       around it and the pop would read as nothing at all. The chip is the thing whose
       value changed, and it is a leaf on a background with room to spare. */
    pui_anim_pop(sg_mode_chip);

    __say_mode();
}

/* ---- Modal ----------------------------------------------------------------- */

/* 快门抓到的照片还在内存里、等用户确认是否保存；确认卡上的路径文本。 */
static BOOL_T    sg_shot_pending;
static lv_obj_t *sg_shot_sub;

static void __modal_close_cb(lv_event_t *e)
{
    (void)e;

    /* 关掉确认卡 = 不保存：把内存里那张照片扔掉。 */
    if (sg_shot_pending) {
        app_camera_discard();
        sg_shot_pending = FALSE;
    }
    pet_page_sheet_close();
    __preview_pause(FALSE);
}

static void __modal_say_cb(lv_event_t *e)
{
    const char *line = (const char *)lv_event_get_user_data(e);

    if (sg_shot_pending) {
        app_camera_discard();
        sg_shot_pending = FALSE;
    }
    pet_page_sheet_close();
    __preview_pause(FALSE);
    pet_say(line);
}

/**
 * @brief Add one full-width row to a sheet's card.
 *
 * Outline rather than solid: the sheet is a menu of things the user might do with the
 * shot, and saturated bars would each claim to be the page's action. The tone is what
 * says which of them is the destructive one.
 *
 * @param[in] cb    clicked handler
 * @param[in] user  handler's user data (a say-line for __modal_say_cb, NULL otherwise)
 */
static void __modal_button(lv_obj_t *card, const char *text, PUI_TONE_E tone,
                           lv_event_cb_t cb, const char *user)
{
    lv_obj_t *btn = pui_button_create(card, text, tone, PUI_FILL_OUTLINE, PUI_SIZE_MD);

    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)user);
}

/**
 * @brief Confirm the shot.
 *
 * Four stacked buttons rather than the reference's two-by-two: the card is 240 px, so
 * a two-column grid leaves about 104 px a cell after its padding and the gap, and
 * AI识别图片文字 alone comes to about 128 px of text before the button's own side
 * padding. A column gives every label the full width, and the card still clears the
 * 320 px of screen height at four rows.
 */
/** 确认卡上的「保存到相册」：把内存里那张照片写进 /sdcard/pictures。 */
static void __save_shot_cb(lv_event_t *e)
{
    char path[160] = { 0 };

    (void)e;

    if (!sg_shot_pending) {
        return;
    }

    if (app_camera_save(path, sizeof(path))) {
        sg_shot_pending = FALSE;
        if (NULL != sg_shot_sub) {
            lv_label_set_text_fmt(sg_shot_sub, "已保存：\n%s", path);
        }
        pet_say_mood("存好啦", PET_SAY_HAPPY);
    } else if (NULL != sg_shot_sub) {
        lv_label_set_text(sg_shot_sub, "保存失败：卡读写异常");
    }
}

static void __ai_open_cb(lv_event_t *e);
static void __ai_pick_cb(lv_event_t *e);

/* ---- 快门：倒计时 + 拍照 --------------------------------------------------- */

static lv_timer_t *sg_count_tmr;
static int         sg_count_left;

/** 倒计时数字写在模式 chip 上（顶部那条不参与 overlay，不会被预览盖掉）。 */
static void __count_show(int left)
{
    if (NULL == sg_mode_label) {
        return;
    }

    if (left > 0) {
        char buf[8];

        lv_snprintf(buf, sizeof(buf), "%d", left);
        pui_text_set(sg_mode_label, buf);
    } else {
        pui_text_set(sg_mode_label, pet_demo_mode_name(pet_demo_mode()));
    }
}

static void __shot_now(void);

static void __count_cb(lv_timer_t *t)
{
    (void)t;

    if (sg_count_left > 0) {
        sg_count_left--;
    }
    if (sg_count_left > 0) {
        __count_show(sg_count_left);
        return;
    }

    sg_count_tmr = NULL;                /* one-shot：这一拍之后定时器自己就没了 */
    __count_show(0);
    __shot_now();
}

/**
 * @brief 按快门：先倒计时（设置里有秒数），到点再抓。
 *
 * **倒计时期间再按一律忽略。** 以前这里是"重新开始倒计时"，但连按的意思通常是
 * "怎么还没拍"，重开只会让等待更长；而且它和"到点自动拍"叠在一起，会拍出两张。
 */
static void __shutter_cb(lv_event_t *e)
{
    int secs;

    (void)e;

    if (NULL != sg_count_tmr) {
        return;                         /* 倒计时中：这一下不算 */
    }

    secs = pet_demo_setting_get(PET_SET_SHOT_TIMER);
    if (secs <= 0) {
        __shot_now();
        return;
    }
    if (secs > 10) {
        secs = 10;
    }

    sg_count_left = secs;
    __count_show(sg_count_left);
    sg_count_tmr = lv_timer_create(__count_cb, 1000, NULL);
    lv_timer_set_repeat_count(sg_count_tmr, secs);
}

/**
 * @brief 抓一张并弹确认卡。
 *
 * 合影 / 质量是拍照这一刻从设置里取的 —— 设置页只改模型，拍照时才真正生效。
 */
static void __shot_now(void)
{
    BOOL_T    ok;
    lv_obj_t *sheet;
    lv_obj_t *card;
    lv_obj_t *body;
    lv_obj_t *title;
    lv_obj_t *sub;
    lv_obj_t *close;

    app_camera_set_compose(pet_demo_setting_get(PET_SET_COMPOSE) ? TRUE : FALSE,
                           (int)pet_demo_pos());
    app_camera_set_quality(pet_demo_setting_get(PET_SET_QUALITY) ? APP_CAM_QUALITY_HD
                                                                 : APP_CAM_QUALITY_STANDARD);

    /* 先抓一张到内存（不写卡）：等用户在确认卡上点「保存到相册」再落盘。 */
    ok = app_camera_grab();
    sg_shot_pending = ok;

    sheet = pet_page_sheet();
    if (sheet == NULL) {
        if (sg_shot_pending) {
            app_camera_discard();
            sg_shot_pending = FALSE;
        }
        return;
    }

    /* 立刻让 overlay 让位（不等 200ms 那一拍），确认卡从第一帧起就是完整的。 */
    __preview_pause(TRUE);

    /* The scrim swallows the tap that dismisses it. */
    lv_obj_add_event_cb(sheet, __modal_close_cb, LV_EVENT_CLICKED, NULL);

    /* The scrim insets its sheet by the page margin and centres it, so this card comes
       out 240 px wide without the page writing the number — which is all CAM_MODAL_W was. */
    card = pui_card_create(sheet, NULL, PUI_TONE_NEUTRAL);
    body = pui_card_body(card);

    title = pui_text_create(body, ok ? "拍照成功！" : "拍照失败", PUI_TEXT_BODY);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    sub = pui_label_create(body,
                           ok ? "点「保存到相册」存进 pictures/"
                              : (app_camera_is_open() ? "没等到相机帧"
                                                      : "相机没有打开"),
                           PET_FONT_CJK_SM, PET_COLOR_TEXT_SUB);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    sg_shot_sub = sub;

    if (ok) {
        __modal_button(body, "保存到相册", PUI_TONE_ACCENT, __save_shot_cb, NULL);
    }
    __modal_button(body, "AI识别", PUI_TONE_ACCENT, __ai_open_cb, NULL);
    __modal_button(body, "新建备忘录", PUI_TONE_ACCENT, __modal_say_cb,
                   "新建了一条备忘录，随时可以改");

    close = pui_button_create(body, "关闭", PUI_TONE_RED, PUI_FILL_OUTLINE, PUI_SIZE_MD);
    lv_obj_set_width(close, LV_PCT(100));
    lv_obj_add_event_cb(close, __modal_close_cb, LV_EVENT_CLICKED, NULL);

    pui_anim_sheet_in(sheet);
}

/* ---- Back ------------------------------------------------------------------ */

/**
 * @brief Leave the page.
 *
 * The page has no header, so this is the only way off it: the arrow is the app's one pop
 * entry point wherever it is drawn, and here it is drawn on the preview's own corner.
 */
/**
 * @brief 离开这一页之前必须当场收掉的东西。
 *
 * 倒计时和 overlay 都是"这一页"的：倒计时到点会去拍，overlay 会在每次上屏前
 * 直接往 framebuffer 里写像素。只靠 200ms 的定时器兜底的话，跳转之后的那次
 * 重绘会先被 overlay 糊上一块取景画面 —— 实测就是"退出后主界面带出一块相机
 * 画面"。所以**任何**离页动作都要先走这里。
 */
static void __leave_page(void)
{
    if (NULL != sg_count_tmr) {
        lv_timer_delete(sg_count_tmr);
        sg_count_tmr = NULL;
    }
    app_camera_overlay_disable();
    if (sg_shot_pending) {
        app_camera_discard();
        sg_shot_pending = FALSE;
    }
    if (app_camera_is_open()) {
        app_camera_close();
    }
}

static void __back_cb(lv_event_t *e)
{
    (void)e;

    __leave_page();
    pet_ui_back();
}

/* ---- Viewfinder ------------------------------------------------------------ */

/**
 * @brief The sticky note pinned into the frame.
 *
 * The stand-in for the thing the camera is pointed at: a note on a wall, which is the
 * shot the AI is going to read. Its lines are fixed demo text, like every other string
 * in this round.
 *
 * The lines go straight into the note rather than into a column inside it. A
 * pet_page_col here comes back as tall as the note whatever it holds — the note is not
 * itself a flex container, so the column's own content height is never resolved — and the
 * lines then centre on that instead of on the note, which drops the last of them off the
 * bottom edge for the viewfinder to clip.
 *
 * The note is sized by its own content and pinned to the top-right corner, which is what
 * keeps it off the mode chip in the top-left. It used to be a fixed-height block centred
 * in the frame, which put its lower half behind the chip.
 */
static void __build_note(lv_obj_t *frame)
{
    lv_obj_t *note = lv_obj_create(frame);

    pui_passthrough(note);
    lv_obj_set_size(note, CAM_NOTE_W, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(note, PET_COLOR_STICKY, 0);
    lv_obj_set_style_bg_opa(note, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(note, 3, 0);
    lv_obj_set_style_pad_all(note, CAM_NOTE_PAD, 0);
    lv_obj_set_style_pad_row(note, CAM_NOTE_GAP, 0);
    lv_obj_set_flex_flow(note, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(note, LV_ALIGN_TOP_RIGHT, -6, 8);
    pui_label_create(note, "买牛奶", PET_FONT_CJK_SM, PET_COLOR_TEXT);
    pui_label_create(note, "买电池", PET_FONT_CJK_SM, PET_COLOR_TEXT);
}

/* ---- 取景框里的实时预览 ----------------------------------------------------
 *
 * 进拍照页时打开摄像头（app_camera，src/modules），预览由 app_camera 的
 * pre-flush 钩子直接写进显示 framebuffer（overlay，见 __cam_place）；页面离开
 * （被隐藏进 trash）就关 overlay、关流、停定时器 —— DVP 一直出帧会白占 PSRAM
 * 带宽和 CPU。
 *
 * 快门走 `app_camera_grab()`：把当前预览帧拼成一张 BMP 放在内存里，用户在确认卡
 * 上点"保存"才写 /sdcard/pictures（app_camera_save）。
 */

static lv_timer_t *sg_cam_tmr;
static lv_obj_t   *sg_cam_cv;
static uint16_t   *sg_cam_buf;
static int         sg_cam_w;
static int         sg_cam_h;

static void __cam_buf_free_cb(lv_event_t *e)
{
    uint16_t *buf = (uint16_t *)lv_event_get_user_data(e);

    if (NULL != buf) {
        tal_psram_free(buf);
    }
}

/**
 * @brief 让 overlay 暂停/恢复（弹窗盖住取景框时）。
 *
 * overlay 是 LVGL 画完之后直接写 framebuffer 的，看不见 LVGL 画了什么 —— 不停下
 * 来就会把确认卡整块糊掉。恢复时顺手 invalidate 一次，overlay 在紧接着的那次
 * flush 里就把画面补回去，不用等下一帧相机数据。
 */
static void __preview_pause(BOOL_T pause)
{
    pause = (TRUE == pause) ? TRUE : FALSE;
    if (pause == sg_cam_paused) {
        return;
    }
    sg_cam_paused = pause;
    app_camera_overlay_pause(pause);

    if (!pause && NULL != sg_cam_cv) {
        lv_obj_invalidate(sg_cam_cv);
    }
}

/**
 * @brief 量一次取景框，把预览接上去（overlay 优先，失败退回 LVGL 画布）。
 *
 * **只能在布局定下来之后调**。建页那一刻，取景框的兄弟节点（快门那一行）还没
 * 建出来，这时量出来的是"没有控制行"的高度 —— overlay 会多铺一条，正好压住
 * 下方"相册 / 快门 / AI识别"那一行。所以第一次量放在页面建完后的第一拍。
 */
static void __cam_place(void)
{
    lv_area_t sc;
    int       top = CAM_CORNER + CAM_CHIP_H + 8;    /* 顶部留给返回箭头 + 模式 chip */
    int       w;
    int       h;
    lv_obj_t *cv;

    lv_obj_update_layout(sg_cam_cv);
    lv_obj_get_coords(sg_cam_cv, &sc);
    w = sc.x2 - sc.x1 + 1;
    h = sc.y2 - sc.y1 + 1;

    /* 高度连顶部那条控件都放不下，说明布局还没稳 —— 下一拍再量。 */
    if (w <= 0 || h <= (top + 16)) {
        return;
    }

    sg_cam_placed = TRUE;
    if (NULL != sg_cam_tmr) {
        lv_timer_set_period(sg_cam_tmr, 200);
    }

    if (app_camera_overlay_enable(sc.x1, sc.y1 + top, w, h - top)) {
        PR_NOTICE("[cam] 取景框 overlay %dx%d（顶部 %d 留给控件）", w, h - top, top);
        return;
    }

    PR_WARN("[cam] overlay 没起来，退回 LVGL 画布");

    sg_cam_buf = (uint16_t *)tal_psram_malloc((size_t)w * (size_t)h * 2u);
    if (NULL == sg_cam_buf) {
        PR_WARN("[cam] 取景框缓冲分配失败（%dx%d）", w, h);
        return;
    }
    sg_cam_w = w;
    sg_cam_h = h;

    /* 先铺成深色底，第一帧到之前不至于是一块花屏。 */
    for (int i = 0; i < w * h; i++) {
        sg_cam_buf[i] = 0x10E5;         /* 0x141C2E */
    }

    cv = lv_canvas_create(sg_cam_cv);
    lv_canvas_set_buffer(cv, sg_cam_buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(cv, 0, 0);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(cv, 0, 0);
    lv_obj_set_style_border_width(cv, 0, 0);
    lv_obj_set_style_pad_all(cv, 0, 0);
    lv_obj_add_event_cb(cv, __cam_buf_free_cb, LV_EVENT_DELETE, sg_cam_buf);
    sg_cam_cv = cv;

    PR_NOTICE("[cam] 取景框实时预览 %dx%d", w, h);
}

static void __cam_tick_cb(lv_timer_t *t)
{
    /* 页面已经离开（隐藏进 trash）：关 overlay + 关流 + 停表。
     * 实时画面由 app_camera 的 pre-flush 钩子直接写 fb，这里不再做绘制。 */
    if (NULL == sg_cam_cv || !lv_obj_is_visible(sg_cam_cv)) {
        app_camera_overlay_disable();
        if (NULL != sg_count_tmr) {
            lv_timer_delete(sg_count_tmr);
            sg_count_tmr = NULL;
        }
        if (sg_shot_pending) {
            app_camera_discard();
            sg_shot_pending = FALSE;
        }
        if (app_camera_is_open()) {
            app_camera_close();
        }
        lv_timer_delete(t);
        sg_cam_tmr = NULL;
        return;
    }

    /* 第一拍才落位：建页时那一刻的布局还不含下面那一行按钮。 */
    if (!sg_cam_placed) {
        __cam_place();
    }

    /* 弹窗盖在取景框上时，overlay 必须让位 —— 它是 LVGL 画完之后直接写
     * framebuffer 的，否则会把确认卡整块糊掉。这里统一兜住所有打开/关闭路径
     * （点关闭、点遮罩、点某一行、页面导航），不用每处都记得调。 */
    __preview_pause(pet_page_sheet_open());
}

static void __scene_del_cb(lv_event_t *e)
{
    (void)e;

    app_camera_overlay_disable();

    if (NULL != sg_cam_tmr) {
        lv_timer_delete(sg_cam_tmr);
        sg_cam_tmr = NULL;
    }
    /* 倒计时中途离开：这一拍不能再落在已经没了的页面上。 */
    if (NULL != sg_count_tmr) {
        lv_timer_delete(sg_count_tmr);
        sg_count_tmr = NULL;
    }
    sg_cam_cv = NULL;                   /* 画布随页面一起删；它的 DELETE 回调释放缓冲 */

    if (sg_shot_pending) {
        app_camera_discard();
        sg_shot_pending = FALSE;
    }
    if (app_camera_is_open()) {
        app_camera_close();
    }
}

/**
 * @brief 建取景框（登记场景，落位交给第一拍）。
 *
 * 首选 **overlay**：不建 lv_canvas，由 app_camera 的 pre-flush 钩子把预览直接写进
 * 显示 framebuffer —— LVGL 逐像素画 236×264 的画布实测要 20~40ms，是拍照页里最贵
 * 的一块。overlay 失败才退回原来的 canvas 路径（见 __cam_place）。
 *
 * **这里不量尺寸、也不接预览**：建页顺序是先建取景框、后建下面那一行按钮，
 * 此刻量出来的高度是"没有控制行"的高度。等 100ms 后的第一拍再量。
 */
static void __build_preview(lv_obj_t *scene)
{
    sg_cam_cv     = scene;              /* 生命周期判断（可见性）用场景本身 */
    sg_cam_placed = FALSE;
    sg_cam_paused = FALSE;

    sg_cam_tmr = lv_timer_create(__cam_tick_cb, 100, NULL);
}

static void __build_viewfinder(lv_obj_t *parent)
{
    lv_obj_t *frame = lv_obj_create(parent);
    lv_obj_t *scene;
    lv_obj_t *part;
    lv_obj_t *art;
    lv_obj_t *back;
    lv_obj_t *chip;

    pui_passthrough(frame);
    lv_obj_set_width(frame, LV_PCT(100));
    lv_obj_set_flex_grow(frame, 1);
    lv_obj_set_style_bg_color(frame, PET_COLOR_PREVIEW, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    /* 取景框四角直角、零内边距：相机画面铺满整块，四周不留边。 */
    lv_obj_set_style_radius(frame, 0, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_set_style_border_width(frame, 0, 0);
    lv_obj_set_style_clip_corner(frame, FALSE, 0);

    scene = lv_obj_create(frame);
    pui_passthrough(scene);
    lv_obj_set_size(scene, LV_PCT(100), LV_PCT(100));
    /* 暗色主题下的"照片"：深蓝灰的房间。整块**纯色**（不再用渐变），这样精灵快照
     * 画布铺同一个底色时能无缝贴上去 —— 快照是 RGB565 不透明矩形。 */
    lv_obj_set_style_bg_color(scene, lv_color_hex(0x141C2E), 0);
    lv_obj_set_style_bg_grad_color(scene, lv_color_hex(0x141C2E), 0);
    lv_obj_set_style_bg_grad_dir(scene, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scene, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scene, 0, 0);
    lv_obj_set_style_clip_corner(scene, FALSE, 0);
    lv_obj_add_event_cb(scene, __scene_del_cb, LV_EVENT_DELETE, NULL);

    if (OPRT_OK == app_camera_open()) {
        /* 相机起来了：取景框里是实时画面，假布景和精灵快照全不要。 */
        __build_preview(scene);
    } else {
        /* A static composition, explicitly labelled below; this is not a camera feed. */
        part = lv_obj_create(scene);
        pui_passthrough(part);
        lv_obj_set_size(part, 32, 70);
        lv_obj_set_style_bg_color(part, lv_color_hex(0x2A3448), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_80, 0);
        lv_obj_set_style_radius(part, 3, 0);
        lv_obj_set_pos(part, 6, 18);
        part = lv_obj_create(scene);
        pui_passthrough(part);
        lv_obj_set_size(part, LV_PCT(100), 22);
        lv_obj_set_style_bg_color(part, lv_color_hex(0x0F1524), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
        lv_obj_align(part, LV_ALIGN_BOTTOM_MID, 0, 0);

        /* A snapshot of the real sprite, not the demo's still image: one frame rendered
           into its own canvas. At most 193x199, which is the viewfinder's inner width, and
           aligned to the foot so the character stands on the same floor line the demo's
           scaled image does. The background is the scene's own colour (0x141C2E), so the
           opaque canvas disappears into it. */
        art = pet_snapshot_create(scene, 0x10E5);   /* 0x141C2E */
        if (NULL != art) {
            lv_obj_align(art, LV_ALIGN_BOTTOM_MID, 0, -10);
        }
        __build_note(scene);
    }

    /* On the scene rather than on the frame: the frame is padded by CAM_FRAME_PAD, and
       lv_obj_align() measures from the parent's content area, so a corner offset here
       would come out CAM_FRAME_PAD further in than it reads. The scene is the light
       surface the chip sits on, and the arrow wears the same ghost fill the header's
       arrow did — the tone, with no disc behind it. */
    back = pui_icon_button_create(scene, FONT_AWESOME_MICARROW_LEFT, PUI_TONE_INDIGO, PUI_FILL_GHOST);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, CAM_CORNER, CAM_CORNER);
    lv_obj_add_event_cb(back, __back_cb, LV_EVENT_CLICKED, NULL);

    chip = lv_obj_create(scene);
    pui_passthrough(chip);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, CAM_CHIP_H);
    lv_obj_set_style_radius(chip, 8, 0);
    lv_obj_set_style_bg_color(chip, PET_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_90, 0);
    lv_obj_set_style_pad_hor(chip, CAM_CHIP_PAD, 0);
    lv_obj_align(chip, LV_ALIGN_TOP_LEFT, CAM_CHIP_X, CAM_CORNER);
    sg_mode_chip  = chip;
    sg_mode_label = pui_label_create(chip, pet_demo_mode_name(pet_demo_mode()), PET_FONT_CJK_SM, PET_COLOR_TEXT_SUB);
    lv_obj_center(sg_mode_label);
    lv_obj_add_event_cb(chip, __mode_cb, LV_EVENT_CLICKED, NULL);
    part = pui_label_create(scene,
                            app_camera_is_open() ? "实时预览" : "演示预览",
                            PET_FONT_CJK_SM, PET_COLOR_TEXT_SUB);
    if (app_camera_is_open()) {
        /* 实时画面由 overlay 直接写 fb，会把底部盖住 —— 标签挪到顶部右侧，
         * 和返回箭头/模式 chip 同一条（那条不参与 overlay）。 */
        lv_obj_align(part, LV_ALIGN_TOP_RIGHT, -CAM_CORNER, CAM_CORNER + 4);
    } else {
        lv_obj_align(part, LV_ALIGN_BOTTOM_LEFT, 5, -3);
    }
}

/* ---- Shutter --------------------------------------------------------------- */

static lv_obj_t *__build_shutter(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_t *ring;

    /* Not pui_passthrough(): this one is the press target, so it keeps CLICKABLE. */
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn, CAM_SHUTTER, CAM_SHUTTER);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, PET_COLOR_ORANGE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    /* A ring rather than a glyph: the icon face has no shutter, and a filled circle
       with a white outline is what the hardware button it stands in for looks like. */
    ring = lv_obj_create(btn);
    pui_passthrough(ring);
    lv_obj_set_size(ring, CAM_SHUTTER - 2 * CAM_SHUTTER_GAP, CAM_SHUTTER - 2 * CAM_SHUTTER_GAP);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, CAM_RING_W, 0);
    lv_obj_set_style_border_color(ring, PET_COLOR_ON_ACCENT, 0);
    lv_obj_center(ring);
    pui_camera_glyph(btn, 28, PET_COLOR_TEXT_SUB);

    lv_obj_add_event_cb(btn, __shutter_cb, LV_EVENT_CLICKED, NULL);

    return btn;
}

/* ---- Actions --------------------------------------------------------------- */

static void __gallery_cb(lv_event_t *e)
{
    (void)e;

    __leave_page();
    pet_ui_go(PET_PAGE_GALLERY);
}

/**
 * @brief 拍照页的「设置」：进设置页。
 *
 * 相册、拍照都在这一行上，第三个位置放的就是"这页剩下的那件事"—— 相机自己的
 * 参数、倒计时、质量、合影都在设置里。AI识别不是这一行的事，它是拍完之后对
 * 照片做的事，所以留在确认卡上。
 */
static void __settings_cb(lv_event_t *e)
{
    (void)e;

    __leave_page();                 /* 和返回一样：走之前先把取景画面收掉 */
    pet_ui_go(PET_PAGE_SETTINGS);
}

/**
 * @brief The stand-in for the recognition pass: it thinks, then answers.
 *
 * Two beats rather than one. The wait is the whole point of a THINK line — the ellipsis
 * is what says the character is still working — so the answer is held back until it has
 * run. The answer line rides on the timer's user data, which is why the same callback
 * serves both kinds of recognition.
 */
static void __ai_done_cb(lv_timer_t *t)
{
    const char *line = (const char *)lv_timer_get_user_data(t);

    /* Cleared, not deleted: the timer is one-shot and clears itself the moment this
       returns. Only the handle has to go, so a later press does not try to cancel a dead
       timer. The next press and the next build both re-check it. */
    sg_ai_timer = NULL;
    pet_say_mood(line != NULL ? line : "认出来了：牛奶、电池", PET_SAY_HAPPY);
}

/**
 * @brief 选了「AI识别图片」或「AI识别图片文字」。
 *
 * 两张卡共用一个流程：收起当前卡，想一下，再说结果。照片到这一步就用完了，
 * 所以顺手丢掉内存里的那一张。
 */
static void __ai_pick_cb(lv_event_t *e)
{
    const char *line = (const char *)lv_event_get_user_data(e);

    if (sg_shot_pending) {
        app_camera_discard();
        sg_shot_pending = FALSE;
    }
    pet_page_sheet_close();
    __preview_pause(FALSE);

    if (sg_ai_timer != NULL) {
        lv_timer_delete(sg_ai_timer);
        sg_ai_timer = NULL;
    }
    pet_say_mood("让我想想", PET_SAY_THINK);
    sg_ai_timer = lv_timer_create(__ai_done_cb, 1300, (void *)line);
    lv_timer_set_repeat_count(sg_ai_timer, 1);
}

/**
 * @brief 确认卡上的「AI识别」：换成一张二选一的卡。
 *
 * `pet_page_sheet()` 会把上一张卡顶掉（旧卡进回收站），照片仍在内存里 ——
 * 所以这里不丢 `sg_shot_pending`，什么时候真不用了由关闭回调来丢。
 */
static void __ai_open_cb(lv_event_t *e)
{
    lv_obj_t *sheet;
    lv_obj_t *card;
    lv_obj_t *body;
    lv_obj_t *title;
    lv_obj_t *close;

    (void)e;

    sheet = pet_page_sheet();
    if (NULL == sheet) {
        return;
    }

    lv_obj_add_event_cb(sheet, __modal_close_cb, LV_EVENT_CLICKED, NULL);
    __preview_pause(TRUE);

    card = pui_card_create(sheet, NULL, PUI_TONE_NEUTRAL);
    body = pui_card_body(card);

    title = pui_text_create(body, "AI识别", PUI_TEXT_BODY);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    __modal_button(body, "AI识别图片", PUI_TONE_ACCENT, __ai_pick_cb,
                   "图里有一张桌子、一杯牛奶和一节电池");
    __modal_button(body, "AI识别图片文字", PUI_TONE_ACCENT, __ai_pick_cb,
                   "认出来了：牛奶 250ml，电池 5 号");

    close = pui_button_create(body, "关闭", PUI_TONE_RED, PUI_FILL_OUTLINE, PUI_SIZE_MD);
    lv_obj_set_width(close, LV_PCT(100));
    lv_obj_add_event_cb(close, __modal_close_cb, LV_EVENT_CLICKED, NULL);

    pui_anim_sheet_in(sheet);
}

/* ---- Page ------------------------------------------------------------------ */

void pet_page_camera_build(lv_obj_t *panel)
{
    lv_obj_t *body;
    lv_obj_t *card;
    lv_obj_t *inner;
    lv_obj_t *row;
    lv_obj_t *btn;

    sg_mode_chip  = NULL;
    sg_mode_label = NULL;

    /* A thought left over from the last visit is cancelled rather than allowed to land on
       a page the user has already left. */
    if (sg_ai_timer != NULL) {
        lv_timer_delete(sg_ai_timer);
        sg_ai_timer = NULL;
    }

    /* No header. The back arrow is on the preview instead — see __build_viewfinder — and
       that arrow is the whole of what a header carried here. */
    body = pet_page_content(panel);

    /* The preview and its controls in one card, which is what they are: an instrument,
       not three things laid on a page. The card takes the whole well, so it has no shadow
       — the well's own edge would clip it — and the extra height the missing header left
       goes to the preview, which is the one thing on this page that wants to be bigger. */
    card = pui_card_create(body, NULL, PUI_TONE_NEUTRAL);
    lv_obj_set_height(card, 0);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_shadow_width(card, 0, 0);

    /* Grown for the same reason the note page's paper is: a card told to fill its parent
       passes the extra height on to its contents rather than leaving a band of empty card
       under them. */
    inner = pui_card_body(card);
    lv_obj_set_height(inner, 0);
    lv_obj_set_flex_grow(inner, 1);
    /* 相机页的卡片内边距清零：取景框直接铺到卡片边缘，四周不留边框。
     * 底部保留 PUI_CARD_PAD，快门那一行不贴边。 */
    lv_obj_set_style_pad_all(inner, 0, 0);
    lv_obj_set_style_pad_row(inner, CAM_STAGE_GAP, 0);
    lv_obj_set_style_pad_bottom(inner, PUI_CARD_PAD, 0);

    __build_viewfinder(inner);

    row = pui_box_create(inner, PUI_FLOW_ROW, CAM_ROW_GAP);
    lv_obj_set_style_pad_hor(row, PUI_CARD_PAD, 0);
    lv_obj_set_height(row, CAM_SHUTTER);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Stacked: the row is as tall as the shutter, so the two buttons beside it have the
       height to stand up. Flat, they would read as captions flanking a disc rather than
       as the other two things the camera does. */
    btn = pui_tool_button_create(row, FONT_AWESOME_IMAGE, "相册", PUI_TONE_ACCENT, PUI_FILL_OUTLINE);
    pui_tool_button_set_stacked(btn, TRUE);
    lv_obj_add_event_cb(btn, __gallery_cb, LV_EVENT_CLICKED, NULL);

    __build_shutter(row);

    btn = pui_tool_button_create(row, FONT_AWESOME_GEAR, "设置", PUI_TONE_ACCENT, PUI_FILL_OUTLINE);
    pui_tool_button_set_stacked(btn, TRUE);
    lv_obj_add_event_cb(btn, __settings_cb, LV_EVENT_CLICKED, NULL);

    /* One card, so one band: index 0. The rise is what says the page has arrived rather
       than that it was always there — the page has no header to slide in. */
    pui_anim_rise_in(card, 0);

    __say_mode();
}
