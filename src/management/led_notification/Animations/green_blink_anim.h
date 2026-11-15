/**
 * @file green_blink_anim.h
 * @brief Green Blink animation for command success notification.
 */

#ifndef GREEN_BLINK_ANIM_H
#define GREEN_BLINK_ANIM_H

#include "led_animation.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t phase; /**< 0 = ON, 1 = OFF, 2 = Finished */
} green_blink_state_t;

void green_blink_anim_reset(void *state);
led_anim_step_result_t green_blink_anim_step(void *state, uint32_t elapsed_ms);

extern const led_animation_t g_anim_green_blink;

#ifdef __cplusplus
}
#endif

#endif /* GREEN_BLINK_ANIM_H */
