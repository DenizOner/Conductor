/**
 * @file led_animation.h
 * @brief Common animation interface contract for LED notification system.
 *
 * Every animation implements this interface independently in its own file
 * under Animations/ directory without cross-animation coupling.
 */

#ifndef LED_ANIMATION_H
#define LED_ANIMATION_H

#include <stdint.h>
#include <stdbool.h>
#include "led_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Animation step result.
 */
typedef struct {
    led_color_t color;           /**< RGB Color to display for this step/frame */
    uint32_t hold_duration_ms;   /**< How long to hold this color before calling next step */
    bool sequence_finished;      /**< True if one complete logical sequence/cycle just completed */
} led_anim_step_result_t;

/**
 * @brief Generic animation interface table.
 */
typedef struct led_animation_interface {
    const char *id;              /**< Stable CLI machine identifier (e.g. "rainbow") */
    const char *display_name;    /**< Human-readable name (e.g. "Rainbow Smooth Cycle") */
    void (*reset)(void *state);  /**< Resets animation internal state machine to start */
    led_anim_step_result_t (*step)(void *state, uint32_t elapsed_ms); /**< Calculates next frame */
} led_animation_t;

#ifdef __cplusplus
}
#endif

#endif /* LED_ANIMATION_H */
