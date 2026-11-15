/**
 * @file white_double_blink_anim.c
 * @brief Implementation of White Double Blink animation.
 *
 * Sequence: Pulse 1 (80ms ON) -> Gap 1 (120ms OFF) -> Pulse 2 (80ms ON) -> Rest (2000ms OFF).
 */

#include "white_double_blink_anim.h"
#include <string.h>

void white_double_blink_anim_reset(void *state)
{
    if (state != NULL) {
        white_double_blink_state_t *s = (white_double_blink_state_t *)state;
        s->phase = 0;
    }
}

led_anim_step_result_t white_double_blink_anim_step(void *state, uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    white_double_blink_state_t *s = (white_double_blink_state_t *)state;
    if (s == NULL) {
        return (led_anim_step_result_t){LED_COLOR_BLACK, 100, true};
    }

    led_color_t color = LED_COLOR_BLACK;
    uint32_t hold_ms = 100;
    bool finished = false;

    switch (s->phase) {
        case 0: /* Pulse 1: ON */
            color = LED_COLOR_WHITE;
            hold_ms = 80;
            s->phase = 1;
            break;
        case 1: /* Gap 1: OFF */
            color = LED_COLOR_BLACK;
            hold_ms = 120;
            s->phase = 2;
            break;
        case 2: /* Pulse 2: ON */
            color = LED_COLOR_WHITE;
            hold_ms = 80;
            s->phase = 3;
            break;
        case 3: /* Long rest: OFF */
        default:
            color = LED_COLOR_BLACK;
            hold_ms = 2000;
            s->phase = 0;
            finished = true; /* 1 complete double-blink sequence cycle */
            break;
    }

    return (led_anim_step_result_t){
        .color = color,
        .hold_duration_ms = hold_ms,
        .sequence_finished = finished
    };
}

const led_animation_t g_anim_white_double_blink = {
    .id = "white_double_blink",
    .display_name = "White Double Blink",
    .reset = white_double_blink_anim_reset,
    .step = white_double_blink_anim_step
};
