/**
 * @file led_animation_registry.c
 * @brief Implementation of central animation registry.
 */

#include "led_animation_registry.h"
#include "Animations/rainbow_anim.h"
#include "Animations/white_double_blink_anim.h"
#include "Animations/red_sos_anim.h"
#include "Animations/green_blink_anim.h"
#include "Animations/police_flasher_anim.h"
#include <string.h>

static const led_registered_anim_t S_REGISTRY[] = {
    {
        .anim = &g_anim_rainbow,
        .state_size = sizeof(rainbow_state_t)
    },
    {
        .anim = &g_anim_white_double_blink,
        .state_size = sizeof(white_double_blink_state_t)
    },
    {
        .anim = &g_anim_red_sos,
        .state_size = sizeof(red_sos_state_t)
    },
    {
        .anim = &g_anim_green_blink,
        .state_size = sizeof(green_blink_state_t)
    },
    {
        .anim = &g_anim_police_flasher,
        .state_size = sizeof(police_flasher_state_t)
    }
};

#define REGISTRY_COUNT (sizeof(S_REGISTRY) / sizeof(S_REGISTRY[0]))

void led_animation_registry_init(void)
{
    /* Static table - no runtime allocation needed */
}

size_t led_animation_registry_count(void)
{
    return REGISTRY_COUNT;
}

const led_registered_anim_t *led_animation_registry_get_at(size_t index)
{
    if (index >= REGISTRY_COUNT) {
        return NULL;
    }
    return &S_REGISTRY[index];
}

const led_registered_anim_t *led_animation_registry_find_by_id(const char *id)
{
    if (id == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < REGISTRY_COUNT; i++) {
        if (strcmp(S_REGISTRY[i].anim->id, id) == 0) {
            return &S_REGISTRY[i];
        }
    }
    return NULL;
}
