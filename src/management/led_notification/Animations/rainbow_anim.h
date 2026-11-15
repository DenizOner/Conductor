/**
 * @file rainbow_anim.h
 * @brief Rainbow smooth color cycling animation.
 */

#ifndef RAINBOW_ANIM_H
#define RAINBOW_ANIM_H

#include "led_animation.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t hue_step; /**< Step in hue rotation (0 - 360) */
} rainbow_state_t;

void rainbow_anim_reset(void *state);
led_anim_step_result_t rainbow_anim_step(void *state, uint32_t elapsed_ms);

extern const led_animation_t g_anim_rainbow;

#ifdef __cplusplus
}
#endif

#endif /* RAINBOW_ANIM_H */
