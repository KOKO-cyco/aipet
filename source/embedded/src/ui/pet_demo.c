/**
 * @file pet_demo.c
 * @brief The in-memory model behind the desktop-pet UI. See pet_demo.h.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_demo.h"

#include <string.h>

#include "tal_kv.h"

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
static PET_POS_E         sg_pos  = PET_POS_RIGHT;

static int sg_settings[PET_SETTING_MAX];

/* Notes are their own list now; the UI only shows and deletes them. Seeded newest
   first, like the list page renders them. */
static PET_NOTE_T sg_notes[PET_NOTE_MAX];
static uint32_t   sg_note_count;

/* Monotonic id source for memos and notes. 0 is "no item" — the AI's snapshot
   uses real ids, and 0 would make "unset" and "item zero" collide. */
static uint32_t sg_next_id = 1;

static const char *const sg_mode_names[PET_MODE_MAX] = {
    "准景合物",
    "实景合影",
    "素材拍摄",
    "截取屏幕",
};

static const char *const sg_setting_names[PET_SETTING_MAX] = {
    "动画速度", "宠物音量", "自动提取待办", "拍照倒计时", "角色合影", "OCR识别功能", "照片质量", "亮度",
};

static const char *const sg_pos_names[PET_POS_MAX] = {"左侧", "右侧"};

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

/***********************************************************
*********************persist define*************************
***********************************************************/

/* 备忘录/笔记落 flash。分三块存：一张 meta 头（计数 + id 游标）+ 两张表各存一块
 * 原始结构体数组。之所以不拼成一个大结构再写，是为了 save 时不需要 6KB 连续临时
 * 缓冲 —— AI 栈起来后 SRAM 只剩十几 KB，凑不出这么大的块。
 * 写入顺序：先表、后 meta，meta 相当于提交标记；中途掉电时 meta 还是旧的，
 * 下次加载长度对不上就整份作废、回落到演示数据。
 * 键名受 TAL_LV_KEY_LEN(16) 限制。 */
#define PET_KV_META    "pet_meta"
#define PET_KV_MEMO    "pet_memo"
#define PET_KV_NOTE    "pet_note"
#define PET_KV_MAGIC   0x5045544DU /* 'P','E','T','M'，用来识别坏数据 */
#define PET_KV_VERSION 1U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t todo_count;
    uint32_t note_count;
    uint32_t next_id;
} PET_KV_META_T;

static void __persist_save(void)
{
    PET_KV_META_T meta;

    if (sg_todo.count > 0) {
        tal_kv_set(PET_KV_MEMO, (const uint8_t *)sg_todo.items, sg_todo.count * sizeof(sg_todo.items[0]));
    } else {
        tal_kv_del(PET_KV_MEMO);
    }

    if (sg_note_count > 0) {
        tal_kv_set(PET_KV_NOTE, (const uint8_t *)sg_notes, sg_note_count * sizeof(sg_notes[0]));
    } else {
        tal_kv_del(PET_KV_NOTE);
    }

    meta.magic      = PET_KV_MAGIC;
    meta.version    = PET_KV_VERSION;
    meta.todo_count = (uint16_t)sg_todo.count;
    meta.note_count = sg_note_count;
    meta.next_id    = sg_next_id;

    /* KV 没就绪时静默放弃：内存里的功能照常，最多丢这次持久化。 */
    (void)tal_kv_set(PET_KV_META, (const uint8_t *)&meta, sizeof(meta));
}

static BOOL_T __persist_load(void)
{
    uint8_t      *buf = NULL;
    size_t        len = 0;
    PET_KV_META_T meta;

    if (0 != tal_kv_get(PET_KV_META, &buf, &len) || NULL == buf) {
        return FALSE;
    }
    if (len != sizeof(meta)) {
        tal_kv_free(buf);
        return FALSE;
    }
    memcpy(&meta, buf, sizeof(meta));
    tal_kv_free(buf);

    if (PET_KV_MAGIC != meta.magic || PET_KV_VERSION != meta.version || meta.todo_count > PET_TODO_MAX ||
        meta.note_count > PET_NOTE_MAX) {
        return FALSE;
    }

    memset(&sg_todo, 0, sizeof(sg_todo));
    memset(sg_notes, 0, sizeof(sg_notes));

    buf = NULL;
    if (meta.todo_count > 0) {
        if (0 != tal_kv_get(PET_KV_MEMO, &buf, &len) || NULL == buf ||
            len != (size_t)meta.todo_count * sizeof(sg_todo.items[0])) {
            if (NULL != buf) {
                tal_kv_free(buf);
            }
            return FALSE;
        }
        memcpy(sg_todo.items, buf, len);
        tal_kv_free(buf);
        sg_todo.count = meta.todo_count;
    }

    buf = NULL;
    if (meta.note_count > 0) {
        if (0 != tal_kv_get(PET_KV_NOTE, &buf, &len) || NULL == buf ||
            len != (size_t)meta.note_count * sizeof(sg_notes[0])) {
            if (NULL != buf) {
                tal_kv_free(buf);
            }
            return FALSE;
        }
        memcpy(sg_notes, buf, len);
        tal_kv_free(buf);
        sg_note_count = meta.note_count;
    }

    __copy(sg_todo.title, sizeof(sg_todo.title), "今日备忘录");

    /* id 游标兜底：即使 meta 里的值偏小，也不能和现有 id 撞车。 */
    sg_next_id = (meta.next_id > 0) ? meta.next_id : 1;
    for (uint32_t i = 0; i < sg_todo.count; i++) {
        if (sg_todo.items[i].id >= sg_next_id) {
            sg_next_id = sg_todo.items[i].id + 1;
        }
    }
    for (uint32_t i = 0; i < sg_note_count; i++) {
        if (sg_notes[i].id >= sg_next_id) {
            sg_next_id = sg_notes[i].id + 1;
        }
    }

    return TRUE;
}

static void __seed_todo(void)
{
    static const struct {
        const char *text;
        const char *when;
        BOOL_T      done;
    } rows[] = {
        {"买咖啡", "09:30", FALSE},
        {"回复邮件", "10:00", TRUE},
        {"项目讨论", "19:00", FALSE},
        {"整理桌面", "昨天", FALSE},
    };

    memset(&sg_todo, 0, sizeof(sg_todo));
    __copy(sg_todo.title, sizeof(sg_todo.title), "今日备忘录");

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]) && i < PET_TODO_MAX; i++) {
        sg_todo.items[i].id = sg_next_id++;
        __copy(sg_todo.items[i].text, PET_TEXT_MAX, rows[i].text);
        __copy(sg_todo.items[i].when, sizeof(sg_todo.items[i].when), rows[i].when);
        sg_todo.items[i].done = rows[i].done;
        sg_todo.count++;
    }
}

static void __seed_notes(void)
{
    static const struct {
        const char *title;
        const char *time;
        const char *lines[3];
    } rows[] = {
        {"购物清单", "10:20", {"牛奶", "电池", "周末采购"}},
        {"会议记录", "昨天", {"周三 10 点", "项目进度", "需要带电脑"}},
        {"灵感记录", "昨天", {"宠物动作", "语音提醒", "加一个转圈动画"}},
        {"旅行计划", "上周", {"杭州两天一夜", "西湖", "带上相机"}},
    };

    memset(sg_notes, 0, sizeof(sg_notes));
    sg_note_count = 0;

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]) && i < PET_NOTE_MAX; i++) {
        PET_NOTE_T *note = &sg_notes[sg_note_count];

        note->id = sg_next_id++;
        __copy(note->title, sizeof(note->title), rows[i].title);
        __copy(note->time, sizeof(note->time), rows[i].time);

        for (size_t j = 0; j < sizeof(rows[i].lines) / sizeof(rows[i].lines[0]); j++) {
            if (rows[i].lines[j] == NULL) {
                break;
            }
            __copy(note->lines[note->line_count], PET_TEXT_MAX, rows[i].lines[j]);
            note->line_count++;
        }

        sg_note_count++;
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
    sg_settings[PET_SET_COMPOSE]    = 1;
    sg_settings[PET_SET_OCR]        = 1;
    sg_settings[PET_SET_QUALITY]    = 1;
    sg_settings[PET_SET_BRIGHTNESS] = 80;
}

void pet_demo_init(void)
{
    memset(&sg_todo, 0, sizeof(sg_todo));
    memset(sg_notes, 0, sizeof(sg_notes));
    sg_note_count = 0;
    sg_next_id    = 1;

    /* 有落盘数据就恢复；没有（首次启动 / 数据损坏）就种一份演示数据并立刻写回，
     * 这样用户第一次删改之后，重启看到的是他自己的清单，而不是重新长出来的四条。 */
    if (TRUE != __persist_load()) {
        __seed_todo();
        __seed_notes();
        __persist_save();
    }

    __seed_shots();
    __seed_settings();

    sg_mode = PET_MODE_SELFIE;
    sg_pos  = PET_POS_RIGHT;
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

    __persist_save();
}

void pet_demo_todo_delete(uint32_t index)
{
    if (index >= sg_todo.count) {
        return;
    }

    if (index + 1 < sg_todo.count) {
        memmove(&sg_todo.items[index], &sg_todo.items[index + 1],
                (sg_todo.count - index - 1) * sizeof(sg_todo.items[0]));
    }

    sg_todo.count--;
    memset(&sg_todo.items[sg_todo.count], 0, sizeof(sg_todo.items[0]));

    __persist_save();
}

void pet_demo_todo_clear_done(void)
{
    uint32_t write = 0;

    for (uint32_t read = 0; read < sg_todo.count; read++) {
        if (sg_todo.items[read].done) {
            continue;
        }

        if (write != read) {
            sg_todo.items[write] = sg_todo.items[read];
        }
        write++;
    }

    if (write < sg_todo.count) {
        memset(&sg_todo.items[write], 0, (sg_todo.count - write) * sizeof(sg_todo.items[0]));
    }

    sg_todo.count = write;

    __persist_save();
}

/* ---- Memos / notes by id -------------------------------------------------- */

static int __memo_find(uint32_t id)
{
    for (uint32_t i = 0; i < sg_todo.count; i++) {
        if (sg_todo.items[i].id == id) {
            return (int)i;
        }
    }

    return -1;
}

static int __note_find(uint32_t id)
{
    for (uint32_t i = 0; i < sg_note_count; i++) {
        if (sg_notes[i].id == id) {
            return (int)i;
        }
    }

    return -1;
}

/* Fill a note's body from newline-separated text. Shared by add and update so
   the two never disagree about what a body looks like. */
static void __note_fill_body(PET_NOTE_T *note, const char *body)
{
    const char *line = body;

    note->line_count = 0;
    memset(note->lines, 0, sizeof(note->lines));

    while (line != NULL && line[0] != '\0' && note->line_count < PET_NOTE_LINES_MAX) {
        const char *nl  = strchr(line, '\n');
        size_t      len = (nl != NULL) ? (size_t)(nl - line) : strlen(line);

        while (len > 0 && line[len - 1] == '\r') {
            len--;
        }

        if (len > 0) {
            size_t copy = (len < PET_TEXT_MAX) ? len : (PET_TEXT_MAX - 1);

            memcpy(note->lines[note->line_count], line, copy);
            note->lines[note->line_count][copy] = '\0';
            note->line_count++;
        }

        line = (nl != NULL) ? nl + 1 : NULL;
    }
}

uint32_t pet_demo_memo_add(const char *text, const char *when)
{
    if (sg_todo.count >= PET_TODO_MAX || text == NULL || text[0] == '\0') {
        return 0;
    }

    /* Newest first, like the list page renders: what the user just asked for
       should be visible at the top without scrolling. */
    if (sg_todo.count > 0) {
        memmove(&sg_todo.items[1], &sg_todo.items[0], sg_todo.count * sizeof(sg_todo.items[0]));
    }

    memset(&sg_todo.items[0], 0, sizeof(sg_todo.items[0]));
    sg_todo.items[0].id = sg_next_id++;
    __copy(sg_todo.items[0].text, PET_TEXT_MAX, text);
    __copy(sg_todo.items[0].when, sizeof(sg_todo.items[0].when), when);
    sg_todo.items[0].done = FALSE;
    sg_todo.count++;

    __persist_save();

    return sg_todo.items[0].id;
}

BOOL_T pet_demo_memo_update(uint32_t id, const char *text, const char *when, int done)
{
    int i = __memo_find(id);

    if (i < 0) {
        return FALSE;
    }

    if (text != NULL && text[0] != '\0') {
        __copy(sg_todo.items[i].text, PET_TEXT_MAX, text);
    }
    if (when != NULL && when[0] != '\0') {
        __copy(sg_todo.items[i].when, sizeof(sg_todo.items[i].when), when);
    }
    if (done >= 0) {
        sg_todo.items[i].done = done ? TRUE : FALSE;
    }

    __persist_save();

    return TRUE;
}

BOOL_T pet_demo_memo_delete_id(uint32_t id)
{
    int i = __memo_find(id);

    if (i < 0) {
        return FALSE;
    }

    pet_demo_todo_delete((uint32_t)i);
    return TRUE;
}

uint32_t pet_demo_note_add(const char *title, const char *body)
{
    PET_NOTE_T *note;

    if (sg_note_count >= PET_NOTE_MAX || title == NULL || title[0] == '\0') {
        return 0;
    }

    if (sg_note_count > 0) {
        memmove(&sg_notes[1], &sg_notes[0], sg_note_count * sizeof(sg_notes[0]));
    }

    note = &sg_notes[0];
    memset(note, 0, sizeof(*note));
    note->id = sg_next_id++;
    __copy(note->title, sizeof(note->title), title);
    __copy(note->time, sizeof(note->time), "刚刚");
    __note_fill_body(note, body);
    sg_note_count++;

    __persist_save();

    return note->id;
}

BOOL_T pet_demo_note_update(uint32_t id, const char *title, const char *body)
{
    int i = __note_find(id);

    if (i < 0) {
        return FALSE;
    }

    if (title != NULL && title[0] != '\0') {
        __copy(sg_notes[i].title, sizeof(sg_notes[i].title), title);
    }
    if (body != NULL && body[0] != '\0') {
        __note_fill_body(&sg_notes[i], body);
    }

    __persist_save();

    return TRUE;
}

BOOL_T pet_demo_note_delete_id(uint32_t id)
{
    int i = __note_find(id);

    if (i < 0) {
        return FALSE;
    }

    pet_demo_note_delete((uint32_t)i);
    return TRUE;
}

/* ---- Notes ---------------------------------------------------------------- */

uint32_t pet_demo_note_count(void)
{
    return sg_note_count;
}

const PET_NOTE_T *pet_demo_note_at(uint32_t index)
{
    if (index >= sg_note_count) {
        return NULL;
    }

    return &sg_notes[index];
}

void pet_demo_note_delete(uint32_t index)
{
    if (index >= sg_note_count) {
        return;
    }

    if (index + 1 < sg_note_count) {
        memmove(&sg_notes[index], &sg_notes[index + 1], (sg_note_count - index - 1) * sizeof(sg_notes[0]));
    }

    sg_note_count--;
    memset(&sg_notes[sg_note_count], 0, sizeof(sg_notes[0]));

    __persist_save();
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

    /* 照片质量只有两档：0 标准（16 位 BMP）/ 1 高清（24 位 BMP）。 */
    if (id == PET_SET_QUALITY) {
        return 1;
    }

    return 100;
}

BOOL_T pet_demo_setting_is_toggle(PET_SETTING_E id)
{
    return (id == PET_SET_AUTO_TODO || id == PET_SET_OCR || id == PET_SET_COMPOSE) ? TRUE : FALSE;
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
        pos = PET_POS_RIGHT;
    }

    return sg_pos_names[pos];
}
