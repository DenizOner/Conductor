/**
 * @file led_notification_manager.h
 * @brief Central LED notification manager, priority engine, non-blocking player.
 *
 * Priorities:
 * 1 (Highest): Zigbee Pairing
 * 2: Command Failure (Red SOS default)
 * 3: Command Success (Green Blink default)
 * 4: Boot Window (10 seconds)
 * 5: Normal Operational State
 * 6: Off (Black)
 *
 * Overriding Master Rule:
 * enabled == false: Instant physical LED off, no animation runs, queues cancelled.
 */

#ifndef LED_NOTIFICATION_MANAGER_H
#define LED_NOTIFICATION_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "led_nvs_storage.h"
#include "led_animation.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the LED notification manager and loads persistent configuration.
 * Safe to call multiple times (idempotent).
 * @return true on success.
 */
bool led_notification_init(void);

/**
 * @brief Non-blocking step/tick function called periodically (e.g. from timer or task cycle).
 * @param elapsed_ms Milliseconds elapsed since last call or tick.
 */
void led_notification_step(uint32_t elapsed_ms);

/**
 * @brief Returns the suggested sleep/delay in milliseconds until the next frame update.
 * When LED is disabled or in a steady idle/off state, returns a power-saving delay (e.g. 500 ms).
 * When actively animating, returns min(s_hold_remaining_ms, 50) with a floor of 10 ms.
 * @return Recommended wait delay in milliseconds before next step.
 */
uint32_t led_notification_get_next_delay_ms(void);

/**
 * @brief Posts a confirmed Logitech Z906 command execution result.
 * Wakes up the LED player task if registered.
 * @param success true if confirmed success, false if final failure.
 */
void led_notification_post_cmd_result(bool success);

/**
 * @brief Gets global enabled state.
 * @return true if enabled, false if disabled.
 */
bool led_notification_is_enabled(void);

/**
 * @brief Sets global enabled state, saves to NVS atomically, and updates LED immediately.
 * @param enabled true to enable, false to disable.
 * @return true on success.
 */
bool led_notification_set_enabled(bool enabled);

/**
 * @brief Retrieves the active animation ID mapped to a given event.
 * @param event Event type enum.
 * @return String pointer to animation ID.
 */
const char *led_notification_get_mapping(led_event_type_t event);

/**
 * @brief Updates and persists mapping for a specific event.
 * @param event Event type enum.
 * @param anim_id Target animation ID.
 * @return true on success, false if invalid event or unregistered animation.
 */
bool led_notification_set_mapping(led_event_type_t event, const char *anim_id);

/**
 * @brief Resets event mapping(s) to factory defaults in NVS and memory.
 * Does not touch global enabled state.
 * @param event Specific event or -1 for all events.
 * @return true on success.
 */
bool led_notification_reset_mappings(int event);

/**
 * @brief Starts a temporary preview of an animation.
 * Ignored if enabled == false.
 * @param anim_id Animation ID to preview.
 * @return true if preview started.
 */
bool led_notification_preview(const char *anim_id);

/**
 * @brief Returns the ID of the currently playing animation (or "none" / "off").
 */
const char *led_notification_get_active_animation_id(void);

/**
 * @brief Checks if the 10-second boot window is currently active.
 */
bool led_notification_is_boot_active(void);

/**
 * @brief Manually sets or overrides the boot window active flag (useful for testing).
 * @param active Desired boot active status.
 */
void led_notification_set_boot_active(bool active);

/**
 * @brief Registers a FreeRTOS task handle to be notified whenever an LED event occurs
 * (e.g. command result, preview trigger, enable/disable toggle).
 * @param task_handle Task handle (or NULL to deregister).
 */
void led_notification_register_task_handle(void *task_handle);

/**
 * @brief Wake up the registered LED player task immediately via FreeRTOS task notification.
 */
void led_notification_wake_task(void);

/**
 * @brief Spawns the static LED player task and waits for it to signal STARTED.
 *
 * Must only be called after led_notification_init() succeeds. If init failed,
 * do NOT call start() — the animation registry is unpopulated.
 *
 * The static task TCB, stack, and task handle live inside led_notification_manager.c.
 * After task creation, led_notification_register_task_handle() is called internally
 * so the LED manager can post direct-task notifications.
 *
 * Uses an internal StaticSemaphore_t handshake: returns only after the task
 * has signalled that it entered its main loop (STARTED state confirmed).
 *
 * @return true if task started, false if creation failed or timeout elapsed.
 */
bool led_notification_start(void);

#ifdef __cplusplus
}
#endif

#endif /* LED_NOTIFICATION_MANAGER_H */
