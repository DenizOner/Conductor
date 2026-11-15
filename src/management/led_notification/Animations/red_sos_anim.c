/**
 * @file red_sos_anim.c
 * @brief Implementation of Red SOS (... --- ...) animation.
 *
 * Pattern:
 * 3 Short pulses: (150ms ON, 150ms OFF) x 3
 * Inter-letter gap: 300ms OFF
 * 3 Long pulses:  (400ms ON, 150ms OFF) x 3
 * Inter-letter gap: 300ms OFF
 * 3 Short pulses: (150ms ON, 150ms OFF) x 3
 * End of sequence: 800ms OFF
 */

#include "red_sos_anim.h"
#include <string.h>

typedef struct {
    bool on;
    uint16_t duration_ms;
} sos_step_def_t;

static const sos_step_def_t SOS_PATTERN[] = {
    /* 'S' : 3 dots */
    { true,  150 }, { false, 150 },
    { true,  150 }, { false, 150 },
    { true,  150 }, { false, 300 }, /* letter space */
    /* 'O' : 3 dashes */
    { true,  400 }, { false, 150 },
    { true,  400 }, { false, 150 },
    { true,  400 }, { false, 300 }, /* letter space */
    /* 'S' : 3 dots */
    { true,  150 }, { false, 150 },
    { true,  150 }, { false, 150 },
    { true,  150 }, { false, 800 }  /* word space */
};

#define SOS_PATTERN_LEN (sizeof(SOS_PATTERN) / sizeof(SOS_PATTERN[0]))

void red_sos_anim_reset(void *state)
{
    if (state != NULL) {
        red_sos_state_t *s = (red_sos_state_t *)state;
        s->step_index = 0;
    }
}

led_anim_step_result_t red_sos_anim_step(void *state, uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    red_sos_state_t *s = (red_sos_state_t *)state;
    if (s == NULL) {
        return (led_anim_step_result_t){LED_COLOR_BLACK, 100, true};
    }

    if (s->step_index >= SOS_PATTERN_LEN) {
        s->step_index = 0;
    }

    const sos_step_def_t *step = &SOS_PATTERN[s->step_index];
    led_color_t color = step->on ? LED_COLOR_RED : LED_COLOR_BLACK;
    uint32_t hold_ms = step->duration_ms;

    s->step_index++;
    bool finished = (s->step_index >= SOS_PATTERN_LEN);
    if (finished) {
        s->step_index = 0;
    }

    return (led_anim_step_result_t){
        .color = color,
        .hold_duration_ms = hold_ms,
        .sequence_finished = finished
    };
}

const led_animation_t g_anim_red_sos = {
    .id = "red_sos",
    .display_name = "Red S.O.S. Flasher",
    .reset = red_sos_anim_reset,
    .step = red_sos_anim_step
};
