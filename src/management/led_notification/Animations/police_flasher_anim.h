/**
 * @file police_flasher_anim.h
 * @brief Police Flasher (Red & Blue alternating rapid strobes) animation for Zigbee pairing.
 */

#ifndef POLICE_FLASHER_ANIM_H
#define POLICE_FLASHER_ANIM_H

#include "led_animation.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t step_index; /**< Step index in the alternating red/blue cycle */
} police_flasher_state_t;

void police_flasher_anim_reset(void *state);
led_anim_step_result_t police_flasher_anim_step(void *state, uint32_t elapsed_ms);

extern const led_animation_t g_anim_police_flasher;

#ifdef __cplusplus
}
#endif

#endif /* POLICE_FLASHER_ANIM_H */
