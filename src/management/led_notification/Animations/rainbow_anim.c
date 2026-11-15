/**
 * @file rainbow_anim.c
 * @brief Implementation of Rainbow smooth color cycling animation.
 */

#include "rainbow_anim.h"
#include <string.h>

#define HUE_MAX 360
#define HUE_INCREMENT 10
#define STEP_DURATION_MS 50

static led_color_t hue_to_rgb(uint16_t hue)
{
    /* Simple HSV with S=1, V=1 to RGB conversion */
    uint8_t region = hue / 60;
    uint8_t remainder = (hue - (region * 60)) * 255 / 60;

    uint8_t p = 0;
    uint8_t q = 255 - remainder;
    uint8_t t = remainder;

    switch (region) {
        case 0: return (led_color_t){255, t, p};
        case 1: return (led_color_t){q, 255, p};
        case 2: return (led_color_t){p, 255, t};
        case 3: return (led_color_t){p, q, 255};
        case 4: return (led_color_t){t, p, 255};
        default: return (led_color_t){255, p, q};
    }
}

void rainbow_anim_reset(void *state)
{
    if (state != NULL) {
        rainbow_state_t *s = (rainbow_state_t *)state;
        s->hue_step = 0;
    }
}

led_anim_step_result_t rainbow_anim_step(void *state, uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    rainbow_state_t *s = (rainbow_state_t *)state;
    if (s == NULL) {
        return (led_anim_step_result_t){LED_COLOR_BLACK, 100, true};
    }

    led_color_t c = hue_to_rgb(s->hue_step);
    s->hue_step += HUE_INCREMENT;

    bool finished = false;
    if (s->hue_step >= HUE_MAX) {
        s->hue_step = 0;
        finished = true; /* One full 360 degree rainbow cycle complete */
    }

    return (led_anim_step_result_t){
        .color = c,
        .hold_duration_ms = STEP_DURATION_MS,
        .sequence_finished = finished
    };
}

const led_animation_t g_anim_rainbow = {
    .id = "rainbow",
    .display_name = "Rainbow Smooth Cycle",
    .reset = rainbow_anim_reset,
    .step = rainbow_anim_step
};
