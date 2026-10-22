/**
 * @file pet_pet.h
 * @brief The character and its speech bubble.
 *
 * The character is a still sprite for now: `pet_char`, an RGB565A8 image generated
 * from the reference art. Animation and interaction are a later job, and
 * pet_pet_stage() is the seam — it returns the container the character is drawn in,
 * so swapping the still for an `lv_gif` (enabled in this build) or an animated image
 * sequence touches pet_page_pet.c only.
 *
 * The bubble does move, though: it fades and slides in when a line is set and fades
 * and slides out when the line is cleared. Tapping the character cycles through a
 * short table of lines as a stand-in for the chat that is not built yet, which is
 * also the only way to watch the animation on demand.
 *
 * Both live in the card on the left of the shell, built once and never torn down —
 * which is what lets a function page set a line with pet_say() and have it already be
 * there when the user looks over.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __PET_PET_H__
#define __PET_PET_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
***********************variable define**********************
***********************************************************/

/* The character sprite. Defined in src/image/pet_char.c, which is generated from the
   art — see that file's header for how. */
LV_IMAGE_DECLARE(pet_char);

/***********************************************************
***********************type define*************************
***********************************************************/

/**
 * @brief How the character is saying a line, which decides how the bubble arrives and
 *        whether it keeps moving once it is there.
 *
 * The words alone cannot carry tone — the bubble is the only channel the character has
 * besides its still face — so the way a line lands is the tone. A tone is spent in two
 * places: the bubble's silhouette, which is the plain rounded bubble for CALM and a shape
 * of its own for the other three, and the way it arrives. CALM is the default, and the
 * plain fade-and-rise every line used to get; the other three are for the handful of
 * moments that are not neutral: something worked, the character is working out an answer,
 * or a press was refused. The motion is small on purpose — a bubble that threw itself
 * around would be tiring by the third line, and this is a companion, not a notification.
 */
typedef enum {
    PET_SAY_CALM = 0, /**< The plain bubble, arriving plainly. The default, and pet_say(). */
    PET_SAY_HAPPY,    /**< A rounder bubble that settles with a small overshoot. */
    PET_SAY_THINK,    /**< Bubble trailed by bubbles; the line then grows an ellipsis. */
    PET_SAY_ALERT,    /**< A sharp, barbed bubble that shakes on arrival: a refusal. */
} PET_SAY_MOOD_E;

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Set the speech bubble's text.
 *
 * Pass NULL or "" to take the bubble away. The string is copied, so a caller may
 * hand over a stack buffer.
 *
 * Either way the bubble animates: it fades and slides in for a new line, and fades
 * and slides out for an empty one. The line is sized to at most two rows of the
 * 18 px face — see PET_BUBBLE_TEXT_H_MAX — so anything longer is clipped rather than
 * growing the bubble over the character.
 *
 * That makes fourteen characters the budget: the bubble's 142 px of text width holds
 * seven glyphs at the face's full 18 px advance, and the bubble cannot take a third
 * row without eating the stage the character stands in. A longer line does not wrap,
 * it loses its tail off the bottom of the bubble — so count before writing one, and
 * prefer a shorter line to a wider bubble.
 *
 * The card is always on screen, so a line set from a function page is visible at the
 * moment it is set rather than on the way back.
 */
void pet_say(const char *text);

/**
 * @brief pet_say() with a tone.
 *
 * Identical to pet_say() except that the bubble is drawn and arrives the way @p mood
 * asks — see PET_SAY_MOOD_E. Use it at the few sites that are not neutral; pet_say() is
 * PET_SAY_CALM and reads better everywhere else. A THINK line should be short, because
 * the ellipsis it grows is appended on top of it.
 */
void pet_say_mood(const char *text, PET_SAY_MOOD_E mood);

/**
 * @brief The container the character is drawn in, for the eventual art swap.
 *
 * @return NULL before the character's card has been built.
 */
lv_obj_t *pet_pet_stage(void);

#ifdef __cplusplus
}
#endif

#endif /* __PET_PET_H__ */
