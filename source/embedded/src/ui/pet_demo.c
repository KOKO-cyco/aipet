/**
 * @file pet_demo.c
 * @brief The in-memory model behind the desktop-pet UI. See pet_demo.h.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_demo.h"

#include <string.h>

/***********************************************************
***********************variable define**********************
***********************************************************/

/* One seeded list each for the home page and the gallery, plus the scalar state
   the settings and camera pages read. All of it is file-static because the UI is
   single-threaded: pages are only ever built from the LVGL task. */
static PET_TODO_LIST_T sg_todo;

static PET_SHOT_T sg_shots[PET_SHOT_MAX];
static uint32_t   sg_shot_count;

static PET_CAMERA_MODE_E sg_mode = PET_MODE_SELFIE;
static PET_POS_E         sg_pos  = PET_POS_CENTER;

static int sg_settings[PET_SETTING_MAX];
static int sg_remind_day  = 15;
static int sg_remind_hour = 19;
static int sg_remind_min  = 0;

/* The notes the home card previews and the notes list page shows in full. The names are
   the gallery's captions so the two never disagree about what a note is called. Each is
   paired with the time it was last touched, which is what the row's trailing label shows:
   the model has no clock, so these are fixed demo strings. */
static const char *const sg_nav[] = {"购物清单", "会议记录", "灵感记录", "待办事项", "旅行计划"};
static const char *const sg_nav_time[] = {"10:20", "昨天", "昨天", "周一", "上周"};

/* The note page's body, one line per row. */
static const char *const sg_note_lines[] = {
    "拍照记录便签",
    "提取待办事项",
    "周三买电池",
    "晚上7点开会",
};

static const char *const sg_mode_names[PET_MODE_MAX] = {
    "准景合物",
    "实景合影",
    "素材拍摄",
    "截取屏幕",
};

static const char *const sg_setting_names[PET_SETTING_MAX] = {
    "动画速度", "宠物音量", "自动提取待办", "拍照倒计时", "拍照&AI识别", "OCR识别功能", "照片质量", "亮度",
};

static const char *const sg_pos_names[PET_POS_MAX] = {"左侧", "居中", "右侧"};

/***********************************************************
********************function define**************************
***********************************************************/

/**
 * @brief Copy @p src into a fixed-size field, always NUL-terminated.
 *
 * strncpy alone leaves the field unterminated when the source is exactly the
 * buffer size, which is how a 64-byte caption turns into a read past the end of
 * the struct.
 */
static void __copy(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

/**
 * @brief Step @p value by @p delta within [lo, hi], wrapping rather than clamping.
 */
static int __wrap(int value, int lo, int hi)
{
    int span = hi - lo + 1;

    value -= lo;
    value %= span;
    if (value < 0) {
        value += span;
    }

    return value + lo;
}

static void __seed_todo(void)
{
    static const struct {
        const char *text;
        BOOL_T      done;
    } rows[] = {
        {"整理桌面", TRUE},
        {"回复邮件", TRUE},
        {"项目讨论", FALSE},
        {"买咖啡", FALSE},
    };

    memset(&sg_todo, 0, sizeof(sg_todo));
    __copy(sg_todo.title, sizeof(sg_todo.title), "今日待办");

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]) && i < PET_TODO_MAX; i++) {
        __copy(sg_todo.items[i].text, PET_TEXT_MAX, rows[i].text);
        sg_todo.items[i].done = rows[i].done;
        sg_todo.count++;
    }
}

static void __seed_shots(void)
{
    /* Five, not four: the gallery grid is three columns wide and puts an 添加笔记 tile
       in the last cell, so five photos fill it exactly. Four would leave a hole. */
    static const char *const captions[] = {"合影", "便签", "购物条", "购物小票", "会议记录"};

    memset(sg_shots, 0, sizeof(sg_shots));
    sg_shot_count = 0;

    for (size_t i = 0; i < sizeof(captions) / sizeof(captions[0]) && i < PET_SHOT_MAX; i++) {
        __copy(sg_shots[i].caption, PET_TEXT_MAX, captions[i]);
        sg_shots[i].selected = FALSE;
        sg_shot_count++;
    }
}

static void __seed_settings(void)
{
    memset(sg_settings, 0, sizeof(sg_settings));

    sg_settings[PET_SET_ANIM_SPEED] = 60;
    sg_settings[PET_SET_VOLUME]     = 50;
    sg_settings[PET_SET_AUTO_TODO]  = 1;
    sg_settings[PET_SET_SHOT_TIMER] = 3;
    sg_settings[PET_SET_SHOT_AI]    = 50;
    sg_settings[PET_SET_OCR]        = 1;
    sg_settings[PET_SET_QUALITY]    = 70;
    sg_settings[PET_SET_BRIGHTNESS] = 80;
}

void pet_demo_init(void)
{
    __seed_todo();
    __seed_shots();
    __seed_settings();

    sg_mode        = PET_MODE_SELFIE;
    sg_pos         = PET_POS_CENTER;
    sg_remind_day  = 15;
    sg_remind_hour = 19;
    sg_remind_min  = 0;
}

/* ---- To-do ---------------------------------------------------------------- */

PET_TODO_LIST_T *pet_demo_todo(void)
{
    return &sg_todo;
}

uint32_t pet_demo_todo_pending(void)
{
    uint32_t pending = 0;

    for (uint32_t i = 0; i < sg_todo.count; i++) {
        if (!sg_todo.items[i].done) {
            pending++;
        }
    }

    return pending;
}

void pet_demo_todo_toggle(uint32_t index)
{
    if (index >= sg_todo.count) {
        return;
    }

    sg_todo.items[index].done = sg_todo.items[index].done ? FALSE : TRUE;
}

BOOL_T pet_demo_todo_add(const char *text)
{
    if (sg_todo.count >= PET_TODO_MAX || text == NULL || text[0] == '\0') {
        return FALSE;
    }

    __copy(sg_todo.items[sg_todo.count].text, PET_TEXT_MAX, text);
    sg_todo.items[sg_todo.count].done = FALSE;
    sg_todo.count++;

    return TRUE;
}

const char *pet_demo_nav_name(uint32_t index)
{
    if (index >= sizeof(sg_nav) / sizeof(sg_nav[0])) {
        return NULL;
    }

    return sg_nav[index];
}

const char *pet_demo_nav_time(uint32_t index)
{
    if (index >= sizeof(sg_nav_time) / sizeof(sg_nav_time[0])) {
        return NULL;
    }

    return sg_nav_time[index];
}

/* ---- Gallery -------------------------------------------------------------- */

const PET_SHOT_T *pet_demo_shots(void)
{
    return sg_shots;
}

uint32_t pet_demo_shot_count(void)
{
    return sg_shot_count;
}

void pet_demo_shot_toggle(uint32_t index)
{
    if (index >= sg_shot_count) {
        return;
    }

    sg_shots[index].selected = sg_shots[index].selected ? FALSE : TRUE;
}

uint32_t pet_demo_shot_selected(void)
{
    uint32_t selected = 0;

    for (uint32_t i = 0; i < sg_shot_count; i++) {
        if (sg_shots[i].selected) {
            selected++;
        }
    }

    return selected;
}

void pet_demo_shot_clear_selection(void)
{
    for (uint32_t i = 0; i < sg_shot_count; i++) {
        sg_shots[i].selected = FALSE;
    }
}

/* ---- Camera --------------------------------------------------------------- */

PET_CAMERA_MODE_E pet_demo_mode(void)
{
    return sg_mode;
}

void pet_demo_mode_set(PET_CAMERA_MODE_E mode)
{
    if (mode >= PET_MODE_MAX) {
        return;
    }

    sg_mode = mode;
}

void pet_demo_mode_next(void)
{
    sg_mode = (PET_CAMERA_MODE_E)__wrap((int)sg_mode + 1, 0, PET_MODE_MAX - 1);
}

const char *pet_demo_mode_name(PET_CAMERA_MODE_E mode)
{
    if (mode >= PET_MODE_MAX) {
        mode = PET_MODE_COMPOSE;
    }

    return sg_mode_names[mode];
}

/* ---- Note page ------------------------------------------------------------ */

const char *pet_demo_note_line(uint32_t index)
{
    if (index >= sizeof(sg_note_lines) / sizeof(sg_note_lines[0])) {
        return NULL;
    }

    return sg_note_lines[index];
}

/* ---- Reminder picker ------------------------------------------------------ */

uint32_t pet_demo_remind_day(void)
{
    return (uint32_t)sg_remind_day;
}

uint32_t pet_demo_remind_hour(void)
{
    return (uint32_t)sg_remind_hour;
}

uint32_t pet_demo_remind_min(void)
{
    return (uint32_t)sg_remind_min;
}

void pet_demo_remind_step(int day_delta, int hour_delta, int min_delta)
{
    sg_remind_day  = __wrap(sg_remind_day + day_delta, 1, 31);
    sg_remind_hour = __wrap(sg_remind_hour + hour_delta, 0, 23);
    sg_remind_min  = __wrap(sg_remind_min + min_delta, 0, 59);
}

/* ---- Settings ------------------------------------------------------------- */

int pet_demo_setting_min(PET_SETTING_E id)
{
    if (id >= PET_SETTING_MAX) {
        return 0;
    }

    /* The countdown is a number of seconds, not a percentage, so it gets its own
       range. Every other slider is 0..100. */
    if (id == PET_SET_SHOT_TIMER) {
        return 0;
    }

    return 0;
}

int pet_demo_setting_max(PET_SETTING_E id)
{
    if (id >= PET_SETTING_MAX) {
        return 0;
    }

    if (pet_demo_setting_is_toggle(id)) {
        return 1;
    }

    if (id == PET_SET_SHOT_TIMER) {
        return 10;
    }

    return 100;
}

BOOL_T pet_demo_setting_is_toggle(PET_SETTING_E id)
{
    return (id == PET_SET_AUTO_TODO || id == PET_SET_OCR) ? TRUE : FALSE;
}

int pet_demo_setting_get(PET_SETTING_E id)
{
    if (id >= PET_SETTING_MAX) {
        return 0;
    }

    return sg_settings[id];
}

void pet_demo_setting_set(PET_SETTING_E id, int value)
{
    int lo;
    int hi;

    if (id >= PET_SETTING_MAX) {
        return;
    }

    lo = pet_demo_setting_min(id);
    hi = pet_demo_setting_max(id);

    if (value < lo) {
        value = lo;
    }
    if (value > hi) {
        value = hi;
    }

    sg_settings[id] = value;
}

void pet_demo_setting_toggle(PET_SETTING_E id)
{
    if (!pet_demo_setting_is_toggle(id)) {
        return;
    }

    pet_demo_setting_set(id, pet_demo_setting_get(id) ? 0 : 1);
}

const char *pet_demo_setting_name(PET_SETTING_E id)
{
    if (id >= PET_SETTING_MAX) {
        return "";
    }

    return sg_setting_names[id];
}

/* ---- Composite position --------------------------------------------------- */

PET_POS_E pet_demo_pos(void)
{
    return sg_pos;
}

void pet_demo_pos_next(void)
{
    sg_pos = (PET_POS_E)__wrap((int)sg_pos + 1, 0, PET_POS_MAX - 1);
}

const char *pet_demo_pos_name(PET_POS_E pos)
{
    if (pos >= PET_POS_MAX) {
        pos = PET_POS_CENTER;
    }

    return sg_pos_names[pos];
}
