/**
 * @file police_flasher_anim.c
 * @brief Implementation of Police Flasher alternating strobes.
 *
 * Pattern:
 * Red burst: (60ms ON, 50ms OFF) x 3
 * Inter-color gap: 100ms OFF
 * Blue burst: (60ms ON, 50ms OFF) x 3
 * Cycle gap: 200ms OFF
 */

#include "police_flasher_anim.h"
#include <string.h>

typedef struct {
    led_color_t color;
    uint16_t duration_ms;
} strobe_step_t;

static const strobe_step_t POLICE_PATTERN[] = {
    /* 3 Red strobes */
    { {255, 0, 0}, 60 }, { {0, 0, 0}, 50 },
    { {255, 0, 0}, 60 }, { {0, 0, 0}, 50 },
    { {255, 0, 0}, 60 }, { {0, 0, 0}, 100 },
    /* 3 Blue strobes */
    { {0, 0, 255}, 60 }, { {0, 0, 0}, 50 },
    { {0, 0, 255}, 60 }, { {0, 0, 0}, 50 },
    { {0, 0, 255}, 60 }, { {0, 0, 0}, 200 }
};

#define POLICE_PATTERN_LEN (sizeof(POLICE_PATTERN) / sizeof(POLICE_PATTERN[0]))

void police_flasher_anim_reset(void *state)
{
    if (state != NULL) {
        police_flasher_state_t *s = (police_flasher_state_t *)state;
        s->step_index = 0;
    }
}

led_anim_step_result_t police_flasher_anim_step(void *state, uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    police_flasher_state_t *s = (police_flasher_state_t *)state;
    if (s == NULL) {
        return (led_anim_step_result_t){LED_COLOR_BLACK, 100, true};
    }

    if (s->step_index >= POLICE_PATTERN_LEN) {
        s->step_index = 0;
    }

    const strobe_step_t *step = &POLICE_PATTERN[s->step_index];
    led_color_t color = step->color;
    uint32_t hold_ms = step->duration_ms;

    s->step_index++;
    bool finished = (s->step_index >= POLICE_PATTERN_LEN);
    if (finished) {
        s->step_index = 0;
    }

    return (led_anim_step_result_t){
        .color = color,
        .hold_duration_ms = hold_ms,
        .sequence_finished = finished
    };
}

const led_animation_t g_anim_police_flasher = {
    .id = "police_flasher",
    .display_name = "Police Flasher (Red/Blue Strobe)",
    .reset = police_flasher_anim_reset,
    .step = police_flasher_anim_step
};
