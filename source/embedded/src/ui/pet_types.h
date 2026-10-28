/**
 * @file pet_types.h
 * @brief Core data model for the desktop-pet UI.
 *
 * Every record here is fixed-size and pointer-free, so the whole model lives in one
 * static array and the UI never allocates while a page is being built. This round
 * carries no persistence: the model is seeded at boot and thrown away on reset.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __PET_TYPES_H__
#define __PET_TYPES_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/* Room for a UTF-8 string. Chinese is 3 bytes a character, so 64 bytes is about
   twenty characters — longer than any label this UI shows. */
#define PET_TEXT_MAX 64

/* Cap on to-do items held in RAM at once. Overridable from the build so the app's
   memory footprint can be traded against how long a list the page shows. */
#ifndef PET_TODO_MAX
#define PET_TODO_MAX 32
#endif

/* Cap on photos in the gallery strip. */
#define PET_SHOT_MAX 8

/* Cap on notes held in RAM. Notes are free-form text; the list page is read-only. */
#define PET_NOTE_MAX 8

/* A note is a title plus a handful of lines. Six is what the 250 px panel can show
   without the list turning into a wall of text; a few lines is what the panel reads
   comfortably. */
#define PET_NOTE_LINES_MAX 6

/***********************************************************
***********************typedef define***********************
***********************************************************/

/**
 * @brief One to-do row. Plain data, safe to memcpy.
 */
typedef struct {
    char   text[PET_TEXT_MAX];
    char   when[16]; /* display time, e.g. "09:30" / "明天"; empty = no time */
    BOOL_T done;
} PET_TODO_T;

/**
 * @brief The to-do list shown on the home page.
 */
typedef struct {
    char       title[PET_TEXT_MAX];
    PET_TODO_T items[PET_TODO_MAX];
    uint32_t   count;
} PET_TODO_LIST_T;

/**
 * @brief One free-form note. Notes have no editing UI or add path on this build;
 *        the list page only shows and deletes them.
 */
typedef struct {
    char     title[PET_TEXT_MAX];
    char     time[16]; /* display time, e.g. "10:20" / "昨天" */
    char     lines[PET_NOTE_LINES_MAX][PET_TEXT_MAX];
    uint32_t line_count;
} PET_NOTE_T;

/**
 * @brief One photo in the gallery grid. Only the caption is modelled — there is no
 *        image data this round.
 */
typedef struct {
    char   caption[PET_TEXT_MAX];
    BOOL_T selected;
} PET_SHOT_T;

/**
 * @brief Which of the four capture modes the camera page is in.
 */
typedef uint8_t PET_CAMERA_MODE_E;
#define PET_MODE_COMPOSE  0 /* 准景合物 */
#define PET_MODE_SELFIE   1 /* 实景合影 */
#define PET_MODE_MATERIAL 2 /* 素材拍摄 */
#define PET_MODE_SCREEN   3 /* 截取屏幕 */
#define PET_MODE_MAX      4

/**
 * @brief The settings page's rows. Whether a row is a switch, a slider or a cycling
 *        choice is decided by the page (see pet_page_settings.c), not by the index.
 */
typedef uint8_t PET_SETTING_E;
#define PET_SET_ANIM_SPEED 0 /* 动画速度 */
#define PET_SET_VOLUME     1 /* 宠物音量 */
#define PET_SET_AUTO_TODO  2 /* 自动提取待办 */
#define PET_SET_SHOT_TIMER 3 /* 拍照倒计时（秒，0 = 关） */
#define PET_SET_COMPOSE    4 /* 角色合影（拍进照片） */
#define PET_SET_OCR        5 /* OCR识别功能 */
#define PET_SET_QUALITY    6 /* 照片质量（0 标准 / 1 高清） */
#define PET_SET_BRIGHTNESS 7 /* 亮度 */
#define PET_SETTING_MAX    8

/**
 * @brief Where the pet stands in a composite photo. A cycling choice rather than a
 *        slider, so it gets its own enum instead of a PET_SETTING_E slot.
 *
 * 只有左右：合影的角色图是一张立绘，居中会把照片里的人挡在正中间，实际没有
 * 人这么构图。
 */
typedef uint8_t PET_POS_E;
#define PET_POS_LEFT  0 /* 左侧 */
#define PET_POS_RIGHT 1 /* 右侧 */
#define PET_POS_MAX   2

#ifdef __cplusplus
}
#endif

#endif /* __PET_TYPES_H__ */
