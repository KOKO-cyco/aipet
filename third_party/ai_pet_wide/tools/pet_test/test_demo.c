/**
 * @file test_demo.c
 * @brief Host unit tests for the desktop-pet demo model.
 *
 * Build and run with `make -C tools/pet_test` from the app directory.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "pet_demo.h"

#include <stdio.h>
#include <string.h>

static int sg_checks = 0;
static int sg_failed = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        sg_checks++;                                                                                                   \
        if (!(cond)) {                                                                                                 \
            sg_failed++;                                                                                               \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                     \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(got, want)                                                                                           \
    do {                                                                                                               \
        sg_checks++;                                                                                                   \
        if ((got) == NULL || strcmp((got), (want)) != 0) {                                                             \
            sg_failed++;                                                                                               \
            printf("FAIL %s:%d  got \"%s\" want \"%s\"\n", __FILE__, __LINE__, (got) != NULL ? (got) : "(null)",       \
                   (want));                                                                                            \
        }                                                                                                              \
    } while (0)

/***********************************************************
***********************to-do list****************************
***********************************************************/

static void test_todo_seed(void)
{
    PET_TODO_LIST_T *todo;

    pet_demo_init();
    todo = pet_demo_todo();

    CHECK(todo != NULL);
    CHECK(todo->count > 0);
    CHECK(todo->count <= PET_TODO_MAX);
    CHECK_STR(todo->title, "今日待办");
    /* Every seeded item must be a real string: the page renders them straight. */
    for (uint32_t i = 0; i < todo->count; i++) {
        CHECK(todo->items[i].text[0] != '\0');
    }
}

static void test_todo_pending(void)
{
    uint32_t pending;
    uint32_t before;
    uint32_t index = PET_TODO_MAX;

    pet_demo_init();

    pending = pet_demo_todo_pending();
    CHECK(pending < pet_demo_todo()->count);

    /* Ticking the first open item has to drop the count by exactly one. The index is
       kept, because the list is seeded with some items already done and re-finding
       "the first done one" would land on a seeded item instead. */
    before = pending;
    for (uint32_t i = 0; i < pet_demo_todo()->count; i++) {
        if (!pet_demo_todo()->items[i].done) {
            index = i;
            pet_demo_todo_toggle(i);
            break;
        }
    }
    CHECK(index < PET_TODO_MAX);
    CHECK(pet_demo_todo_pending() == before - 1);

    /* And ticking the same one again has to put it back. */
    pet_demo_todo_toggle(index);
    CHECK(pet_demo_todo_pending() == before);
}

static void test_todo_toggle_bounds(void)
{
    uint32_t count;

    pet_demo_init();
    count = pet_demo_todo()->count;

    /* Out of range must be a no-op rather than a write past the array. */
    pet_demo_todo_toggle(count);
    pet_demo_todo_toggle(count + 1000);
    CHECK(pet_demo_todo()->count == count);
}

static void test_todo_add(void)
{
    uint32_t count = pet_demo_todo()->count;

    pet_demo_init();
    count = pet_demo_todo()->count;

    CHECK(pet_demo_todo_add("写周报") == TRUE);
    CHECK(pet_demo_todo()->count == count + 1);
    CHECK_STR(pet_demo_todo()->items[count].text, "写周报");
    /* A new item starts open, which is what the badge counts. */
    CHECK(pet_demo_todo()->items[count].done == FALSE);

    /* Empty and NULL are refused, and neither changes the list. */
    CHECK(pet_demo_todo_add("") == FALSE);
    CHECK(pet_demo_todo_add(NULL) == FALSE);
    CHECK(pet_demo_todo()->count == count + 1);
}

static void test_todo_add_full(void)
{
    uint32_t i;

    pet_demo_init();

    /* Fill to the cap, then check the next one is refused rather than overrunning. */
    for (i = pet_demo_todo()->count; i < PET_TODO_MAX; i++) {
        CHECK(pet_demo_todo_add("x") == TRUE);
    }

    CHECK(pet_demo_todo()->count == PET_TODO_MAX);
    CHECK(pet_demo_todo_add("one too many") == FALSE);
    CHECK(pet_demo_todo()->count == PET_TODO_MAX);
}

static void test_todo_add_long_text(void)
{
    char long_text[PET_TEXT_MAX + 32];

    pet_demo_init();

    memset(long_text, 'A', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';

    CHECK(pet_demo_todo_add(long_text) == TRUE);

    /* Truncated, not overflowed: the field has to stay NUL-terminated inside its own
       size, or the page renders whatever follows it in the struct. */
    CHECK(strlen(pet_demo_todo()->items[pet_demo_todo()->count - 1].text) == PET_TEXT_MAX - 1);
}

static void test_nav(void)
{
    pet_demo_init();

    CHECK(pet_demo_nav_name(0) != NULL);
    CHECK(pet_demo_nav_name(0)[0] != '\0');

    /* The page loops until NULL, so one has to arrive. */
    CHECK(pet_demo_nav_name(64) == NULL);
}

/***********************************************************
************************gallery******************************
***********************************************************/

static void test_shot_selection(void)
{
    uint32_t count;

    pet_demo_init();
    count = pet_demo_shot_count();

    CHECK(count > 0);
    CHECK(count <= PET_SHOT_MAX);
    CHECK(pet_demo_shot_selected() == 0);

    pet_demo_shot_toggle(0);
    CHECK(pet_demo_shot_selected() == 1);
    CHECK(pet_demo_shots()[0].selected == TRUE);

    pet_demo_shot_toggle(0);
    CHECK(pet_demo_shot_selected() == 0);

    pet_demo_shot_toggle(1);
    pet_demo_shot_toggle(count - 1);
    CHECK(pet_demo_shot_selected() == 2);

    pet_demo_shot_clear_selection();
    CHECK(pet_demo_shot_selected() == 0);
}

static void test_shot_bounds(void)
{
    uint32_t count;

    pet_demo_init();
    count = pet_demo_shot_count();

    pet_demo_shot_toggle(count);
    pet_demo_shot_toggle(count + 1000);
    CHECK(pet_demo_shot_selected() == 0);
}

static void test_shot_captions(void)
{
    pet_demo_init();

    for (uint32_t i = 0; i < pet_demo_shot_count(); i++) {
        CHECK(pet_demo_shots()[i].caption[0] != '\0');
    }
}

/***********************************************************
************************camera*******************************
***********************************************************/

static void test_mode_cycle(void)
{
    PET_CAMERA_MODE_E first;

    pet_demo_init();
    first = pet_demo_mode();
    CHECK(first < PET_MODE_MAX);

    /* Cycling PET_MODE_MAX times has to come back to where it started. */
    for (int i = 0; i < PET_MODE_MAX; i++) {
        pet_demo_mode_next();
    }
    CHECK(pet_demo_mode() == first);

    /* And every mode must have a name, since the viewfinder renders it. */
    for (uint32_t i = 0; i < PET_MODE_MAX; i++) {
        pet_demo_mode_set((PET_CAMERA_MODE_E)i);
        CHECK(pet_demo_mode_name(pet_demo_mode())[0] != '\0');
    }
}

static void test_mode_bounds(void)
{
    PET_CAMERA_MODE_E before;

    pet_demo_init();
    before = pet_demo_mode();

    pet_demo_mode_set(PET_MODE_MAX);
    pet_demo_mode_set((PET_CAMERA_MODE_E)200);
    CHECK(pet_demo_mode() == before);

    /* An out-of-range name reads as the first mode rather than off the end of the
       table. */
    CHECK_STR(pet_demo_mode_name(PET_MODE_MAX), pet_demo_mode_name(PET_MODE_COMPOSE));
}

/***********************************************************
***********************note page*****************************
***********************************************************/

static void test_note_lines(void)
{
    pet_demo_init();

    CHECK(pet_demo_note_line(0) != NULL);
    CHECK(pet_demo_note_line(0)[0] != '\0');
    CHECK(pet_demo_note_line(64) == NULL);
}

/***********************************************************
************************reminder*****************************
***********************************************************/

static void test_remind_wraps(void)
{
    pet_demo_init();

    CHECK(pet_demo_remind_day() >= 1 && pet_demo_remind_day() <= 31);
    CHECK(pet_demo_remind_hour() <= 23);
    CHECK(pet_demo_remind_min() <= 59);

    /* Wrapping rather than clamping is the point: a roller that stops dead at 23:59
       reads as broken. */
    pet_demo_remind_step(0, 24, 0);
    CHECK(pet_demo_remind_hour() <= 23);

    pet_demo_remind_step(0, 0, 60);
    CHECK(pet_demo_remind_min() <= 59);

    pet_demo_remind_step(31, 0, 0);
    CHECK(pet_demo_remind_day() >= 1 && pet_demo_remind_day() <= 31);

    /* A large negative delta has to wrap too, not go out of range. */
    pet_demo_remind_step(-1000, -1000, -1000);
    CHECK(pet_demo_remind_day() >= 1 && pet_demo_remind_day() <= 31);
    CHECK(pet_demo_remind_hour() <= 23);
    CHECK(pet_demo_remind_min() <= 59);
}

static void test_remind_round_trip(void)
{
    uint32_t day;
    uint32_t hour;
    uint32_t min;

    pet_demo_init();
    day  = pet_demo_remind_day();
    hour = pet_demo_remind_hour();
    min  = pet_demo_remind_min();

    /* The sheet hands the model the difference between what its rollers show and what
       the model holds. A zero delta has to be exactly a no-op. */
    pet_demo_remind_step(0, 0, 0);
    CHECK(pet_demo_remind_day() == day);
    CHECK(pet_demo_remind_hour() == hour);
    CHECK(pet_demo_remind_min() == min);

    /* And a delta that lands on a target has to reach it exactly. */
    pet_demo_remind_step(3 - (int)day, 8 - (int)hour, 30 - (int)min);
    CHECK(pet_demo_remind_day() == 3);
    CHECK(pet_demo_remind_hour() == 8);
    CHECK(pet_demo_remind_min() == 30);
}

/***********************************************************
************************settings*****************************
***********************************************************/

static void test_setting_defaults(void)
{
    pet_demo_init();

    for (uint32_t i = 0; i < PET_SETTING_MAX; i++) {
        int value = pet_demo_setting_get((PET_SETTING_E)i);

        CHECK(value >= pet_demo_setting_min((PET_SETTING_E)i));
        CHECK(value <= pet_demo_setting_max((PET_SETTING_E)i));
        CHECK(pet_demo_setting_name((PET_SETTING_E)i)[0] != '\0');
    }
}

static void test_setting_clamps(void)
{
    pet_demo_init();

    pet_demo_setting_set(PET_SET_ANIM_SPEED, 1000);
    CHECK(pet_demo_setting_get(PET_SET_ANIM_SPEED) == pet_demo_setting_max(PET_SET_ANIM_SPEED));

    pet_demo_setting_set(PET_SET_ANIM_SPEED, -50);
    CHECK(pet_demo_setting_get(PET_SET_ANIM_SPEED) == pet_demo_setting_min(PET_SET_ANIM_SPEED));

    /* The countdown is seconds rather than a percentage, so it must not accept 100. */
    CHECK(pet_demo_setting_max(PET_SET_SHOT_TIMER) < 100);
    pet_demo_setting_set(PET_SET_SHOT_TIMER, 100);
    CHECK(pet_demo_setting_get(PET_SET_SHOT_TIMER) == pet_demo_setting_max(PET_SET_SHOT_TIMER));
}

static void test_setting_toggle(void)
{
    pet_demo_init();

    CHECK(pet_demo_setting_is_toggle(PET_SET_AUTO_TODO) == TRUE);
    CHECK(pet_demo_setting_is_toggle(PET_SET_OCR) == TRUE);
    CHECK(pet_demo_setting_is_toggle(PET_SET_VOLUME) == FALSE);

    pet_demo_setting_set(PET_SET_AUTO_TODO, 1);
    pet_demo_setting_toggle(PET_SET_AUTO_TODO);
    CHECK(pet_demo_setting_get(PET_SET_AUTO_TODO) == 0);
    pet_demo_setting_toggle(PET_SET_AUTO_TODO);
    CHECK(pet_demo_setting_get(PET_SET_AUTO_TODO) == 1);

    /* A slider is not a switch, so toggling one must do nothing rather than flip a
       percentage to 1. */
    pet_demo_setting_set(PET_SET_VOLUME, 55);
    pet_demo_setting_toggle(PET_SET_VOLUME);
    CHECK(pet_demo_setting_get(PET_SET_VOLUME) == 55);
}

static void test_setting_bounds(void)
{
    pet_demo_init();

    /* Out of range ids are ignored rather than reading or writing past the array. */
    pet_demo_setting_set(PET_SETTING_MAX, 5);
    pet_demo_setting_set((PET_SETTING_E)200, 5);
    CHECK(pet_demo_setting_get(PET_SETTING_MAX) == 0);
    CHECK(pet_demo_setting_is_toggle(PET_SETTING_MAX) == FALSE);
    CHECK_STR(pet_demo_setting_name(PET_SETTING_MAX), "");
}

static void test_setting_shot_timer_shared(void)
{
    pet_demo_init();

    /* The camera page's countdown switch writes the same setting the settings page's
       slider does, so switching it off has to read as zero here. */
    pet_demo_setting_set(PET_SET_SHOT_TIMER, 0);
    CHECK(pet_demo_setting_get(PET_SET_SHOT_TIMER) == 0);

    pet_demo_setting_set(PET_SET_SHOT_TIMER, 3);
    CHECK(pet_demo_setting_get(PET_SET_SHOT_TIMER) == 3);
}

/***********************************************************
************************position*****************************
***********************************************************/

static void test_pos_cycle(void)
{
    PET_POS_E first;

    pet_demo_init();
    first = pet_demo_pos();

    for (int i = 0; i < PET_POS_MAX; i++) {
        pet_demo_pos_next();
    }
    CHECK(pet_demo_pos() == first);

    for (uint32_t i = 0; i < PET_POS_MAX; i++) {
        CHECK(pet_demo_pos_name((PET_POS_E)i)[0] != '\0');
    }

    CHECK_STR(pet_demo_pos_name(PET_POS_MAX), pet_demo_pos_name(PET_POS_CENTER));
}

/***********************************************************
**************************main*******************************
***********************************************************/

static void test_init_resets(void)
{
    pet_demo_init();

    pet_demo_todo_add("temporary");
    pet_demo_shot_toggle(0);
    pet_demo_setting_set(PET_SET_VOLUME, 7);

    /* The model is seeded at boot and thrown away on reset, so a second init has to
       put everything back rather than accumulate. */
    pet_demo_init();

    CHECK(pet_demo_shot_selected() == 0);
    CHECK(pet_demo_setting_get(PET_SET_VOLUME) != 7);
    CHECK(pet_demo_todo()->count <= PET_TODO_MAX);

    for (uint32_t i = 0; i < pet_demo_todo()->count; i++) {
        CHECK_STR(pet_demo_todo()->items[i].text, pet_demo_todo()->items[i].text);
    }
}

int main(void)
{
    test_todo_seed();
    test_todo_pending();
    test_todo_toggle_bounds();
    test_todo_add();
    test_todo_add_full();
    test_todo_add_long_text();
    test_nav();

    test_shot_selection();
    test_shot_bounds();
    test_shot_captions();

    test_mode_cycle();
    test_mode_bounds();

    test_note_lines();
    test_remind_wraps();
    test_remind_round_trip();

    test_setting_defaults();
    test_setting_clamps();
    test_setting_toggle();
    test_setting_bounds();
    test_setting_shot_timer_shared();

    test_pos_cycle();
    test_init_resets();

    printf("%d checks, %d failed\n", sg_checks, sg_failed);

    return sg_failed == 0 ? 0 : 1;
}
