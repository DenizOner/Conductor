/**
 * @file green_blink_anim.c
 * @brief Implementation of Green Blink animation.
 *
 * Sequence: 250ms Green ON -> 150ms OFF -> Finished.
 */

#include "green_blink_anim.h"
#include <string.h>

void green_blink_anim_reset(void *state)
{
    if (state != NULL) {
        green_blink_state_t *s = (green_blink_state_t *)state;
        s->phase = 0;
    }
}

led_anim_step_result_t green_blink_anim_step(void *state, uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    green_blink_state_t *s = (green_blink_state_t *)state;
    if (s == NULL) {
        return (led_anim_step_result_t){LED_COLOR_BLACK, 100, true};
    }

    led_color_t color = LED_COLOR_BLACK;
    uint32_t hold_ms = 100;
    bool finished = false;

    if (s->phase == 0) {
        /* Pulse ON */
        color = LED_COLOR_GREEN;
        hold_ms = 250;
        s->phase = 1;
    } else {
        /* Tail OFF */
        color = LED_COLOR_BLACK;
        hold_ms = 150;
        s->phase = 0;
        finished = true;
    }

    return (led_anim_step_result_t){
        .color = color,
        .hold_duration_ms = hold_ms,
        .sequence_finished = finished
    };
}

const led_animation_t g_anim_green_blink = {
    .id = "green_blink",
    .display_name = "Green Success Blink",
    .reset = green_blink_anim_reset,
    .step = green_blink_anim_step
};
