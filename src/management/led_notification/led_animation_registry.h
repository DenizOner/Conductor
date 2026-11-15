/**
 * @file led_animation_registry.h
 * @brief Central animation registry and lookup engine.
 *
 * Provides single source of truth for all registered animations,
 * stable CLI identifiers, and registration without touching animation files.
 */

#ifndef LED_ANIMATION_REGISTRY_H
#define LED_ANIMATION_REGISTRY_H

#include <stddef.h>
#include <stdbool.h>
#include "led_animation.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registered animation descriptor.
 */
typedef struct {
    const led_animation_t *anim; /**< Pointer to animation interface table */
    size_t state_size;           /**< Size in bytes of internal state struct */
} led_registered_anim_t;

/**
 * @brief Initializes the central animation registry.
 */
void led_animation_registry_init(void);

/**
 * @brief Returns total number of registered animations.
 * @return Number of registered animations.
 */
size_t led_animation_registry_count(void);

/**
 * @brief Retrieves registered animation by array index.
 * @param index Index (0 to count-1).
 * @return Pointer to animation descriptor or NULL if index out of bounds.
 */
const led_registered_anim_t *led_animation_registry_get_at(size_t index);

/**
 * @brief Looks up an animation by its stable machine ID (e.g. "rainbow").
 * @param id Machine string ID.
 * @return Pointer to animation descriptor or NULL if not found.
 */
const led_registered_anim_t *led_animation_registry_find_by_id(const char *id);

#ifdef __cplusplus
}
#endif

#endif /* LED_ANIMATION_REGISTRY_H */
