/**
 * @file white_double_blink_anim.h
 * @brief White Double Blink animation for normal operational state.
 */

#ifndef WHITE_DOUBLE_BLINK_ANIM_H
#define WHITE_DOUBLE_BLINK_ANIM_H

#include "led_animation.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t phase; /**< Phase within the double blink sequence (0 - 4) */
} white_double_blink_state_t;

void white_double_blink_anim_reset(void *state);
led_anim_step_result_t white_double_blink_anim_step(void *state, uint32_t elapsed_ms);

extern const led_animation_t g_anim_white_double_blink;

#ifdef __cplusplus
}
#endif

#endif /* WHITE_DOUBLE_BLINK_ANIM_H */
