/**
 * @file red_sos_anim.h
 * @brief Red SOS (... --- ...) animation for command failure notification.
 */

#ifndef RED_SOS_ANIM_H
#define RED_SOS_ANIM_H

#include "led_animation.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t step_index; /**< Step index in the 18-step SOS pattern */
} red_sos_state_t;

void red_sos_anim_reset(void *state);
led_anim_step_result_t red_sos_anim_step(void *state, uint32_t elapsed_ms);

extern const led_animation_t g_anim_red_sos;

#ifdef __cplusplus
}
#endif

#endif /* RED_SOS_ANIM_H */
