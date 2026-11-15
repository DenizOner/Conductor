/**
 * @file led_notification_manager.c
 * @brief Central LED notification manager, adaptive scheduler, and non-blocking player.
 *
 * ## Physical OFF Contract (Invariants)
 *
 * This module enforces the following invariants at all times:
 *
 *   INV-1: active_animation == NONE  ->  physical LED must be OFF.
 *   INV-2: global_led_enabled == false  ->  physical LED must be OFF.
 *   INV-3: Animation completes + no replacement  ->  explicit physical OFF applied once.
 *   INV-4: Animation cancelled + no replacement  ->  explicit physical OFF applied once.
 *   INV-5: Driver init  ->  first safe state is physical OFF.
 *
 * ## Key Design Rule
 *
 * "No animation" is a SOFTWARE state. "LED physically off" is a HARDWARE state.
 * They are NOT synonymous. The WS2812 addressable LED holds its last color in its
 * internal shift register until a new GRB frame is clocked in. Setting the animation
 * pointer to NULL does NOT transmit a black frame to the hardware. This manager MUST
 * explicitly call led_driver_clear() (which sends {G=0,R=0,B=0}) to achieve real
 * physical OFF.
 *
 * The s_physical_off_sent flag tracks whether the explicit OFF frame has been
 * transmitted since the last transition to the idle/off target state. This prevents
 * anti-polling (repeated OFF writes every tick) while guaranteeing exactly ONE OFF
 * frame per idle transition.
 */

#include "led_notification_manager.h"
#include "led_animation_registry.h"
#include "led_driver.h"
#include "zigbee_app.h"
#include "z906_state.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "LED_NOTIF";

#define BOOT_WINDOW_MS 10000

/* Internal player context */
static bool s_initialized   = false;
/** @brief Tracks whether led_notification_init() succeeded; gate for led_notification_start() */
static bool s_led_init_ok   = false;
static led_config_bundle_t s_config;

/* FreeRTOS task handle for event-driven waking */
static TaskHandle_t s_led_task_handle = NULL;

/* Boot tracking */
static int64_t s_boot_start_time_us = 0;
static bool s_boot_active = true;

/* One-shot notification queue / state */
typedef enum {
    ONESHOT_NONE = 0,
    ONESHOT_CMD_SUCCESS,
    ONESHOT_CMD_FAILURE,
    ONESHOT_PREVIEW
} oneshot_type_t;

static oneshot_type_t s_current_oneshot = ONESHOT_NONE;
static char s_preview_anim_id[LED_ANIM_ID_MAX_LEN] = {0};

/* Currently executing animation tracking */
static const led_registered_anim_t *s_active_registered_anim = NULL;
static uint8_t s_anim_state_buffer[64]; /* Static memory for animation state struct */
static uint32_t s_hold_remaining_ms = 0;
static const char *s_current_anim_id = "none";

/**
 * @brief Physical-OFF sent flag.
 *
 * WHY THIS EXISTS: The WS2812 is a state-retaining addressable LED. It holds the
 * last transmitted GRB color in its internal shift register until a new frame is
 * clocked in. If the animation manager sets s_active_registered_anim = NULL without
 * transmitting a {0,0,0} frame, the LED remains physically lit at its last color.
 *
 * This flag ensures exactly ONE black frame is sent per idle transition, preventing:
 *   - Continuous per-tick OFF polling (wasteful, anti-pattern).
 *   - Missing OFF frame on idle transition (physical LED stays lit).
 *
 * LIFECYCLE:
 *   - Initialized to false at startup.
 *   - Reset to false by switch_to_animation() when a new animation starts.
 *   - Set to true by apply_physical_off() after led_driver_clear() succeeds.
 *   - Reset to false by set_enabled(true) to force re-evaluation on next step.
 */
static bool s_physical_off_sent = false;

void led_notification_register_task_handle(void *task_handle)
{
    s_led_task_handle = (TaskHandle_t)task_handle;
}

void led_notification_wake_task(void)
{
    if (s_led_task_handle != NULL) {
        xTaskNotifyGive(s_led_task_handle);
    }
}

/**
 * @brief Applies an explicit physical OFF to the WS2812 hardware.
 *
 * Transmits a black {G=0,R=0,B=0} frame and marks the physical state as off.
 * This is the ONLY correct way to achieve physical LED off — setting animation
 * pointers to NULL is insufficient because the WS2812 retains its last color.
 *
 * Idempotent: safe to call multiple times. Only transmits if not already done
 * (guarded by s_physical_off_sent flag to prevent anti-polling).
 */
static void apply_physical_off(void)
{
    if (!s_physical_off_sent) {
        /* Transmit an explicit {G=0,R=0,B=0} frame to the WS2812 hardware latch.
         * This is the only operation that truly extinguishes the physical LED. */
        led_driver_clear();
        s_physical_off_sent = true;
        ESP_LOGD(TAG, "Physical OFF frame transmitted to WS2812");
    }
}

/**
 * @brief Switches the currently active animation, resetting animation state.
 *
 * If anim_id is NULL: clears active animation and applies physical OFF via
 * apply_physical_off() (which is idempotent and guarded by s_physical_off_sent).
 * If same animation already playing: no-op to avoid restart glitch.
 * On new animation: resets s_physical_off_sent so next idle transition sends OFF.
 *
 * @param anim_id Animation ID string, or NULL to go idle/off.
 */
static void switch_to_animation(const char *anim_id)
{
    if (anim_id == NULL) {
        /* Target state is idle/off — must ensure hardware is physically off */
        s_active_registered_anim = NULL;
        s_current_anim_id = "none";
        apply_physical_off();
        return;
    }

    /* No-op if same animation is already playing (avoids restart glitch) */
    if (s_active_registered_anim != NULL && strcmp(s_current_anim_id, anim_id) == 0) {
        return;
    }

    const led_registered_anim_t *reg = led_animation_registry_find_by_id(anim_id);
    if (reg == NULL) {
        ESP_LOGW(TAG, "Animation '%s' not found — applying physical OFF", anim_id);
        s_active_registered_anim = NULL;
        s_current_anim_id = "none";
        apply_physical_off();
        return;
    }

    /* Starting a new animation: clear the off-sent flag so the next idle
     * transition will transmit a fresh OFF frame after this animation ends. */
    s_physical_off_sent = false;

    s_active_registered_anim = reg;
    s_current_anim_id = reg->anim->id;
    memset(s_anim_state_buffer, 0, sizeof(s_anim_state_buffer));

    if (reg->anim->reset) {
        reg->anim->reset(s_anim_state_buffer);
    }

    s_hold_remaining_ms = 0; /* Step immediately on next tick */
}

bool led_notification_init(void)
{
    if (s_initialized) {
        return true;
    }

    led_animation_registry_init();
    led_driver_init();

    /* Load persistent config */
    led_nvs_storage_load(&s_config);

    s_boot_start_time_us = esp_timer_get_time();
    s_boot_active = true;
    s_current_oneshot = ONESHOT_NONE;
    s_active_registered_anim = NULL;
    s_hold_remaining_ms = 0;

    s_physical_off_sent = false;
    s_initialized = true;

    if (!s_config.enabled) {
        /* INV-2: Global disabled on boot — apply physical OFF immediately */
        apply_physical_off();
        ESP_LOGI(TAG, "LED notification initialized in DISABLED (OFF) state");
    } else {
        /* switch_to_animation resets s_physical_off_sent, starts boot animation */
        ESP_LOGI(TAG, "LED notification initialized in ENABLED state, starting boot animation");
        switch_to_animation(s_config.mapping[LED_EVENT_BOOT]);
    }

    /* Mark init success so led_notification_start() is permitted to spawn the task */
    s_led_init_ok = true;
    return true;
}

bool led_notification_is_enabled(void)
{
    return s_config.enabled;
}

bool led_notification_set_enabled(bool enabled)
{
    if (s_config.enabled == enabled) {
        return true;
    }

    s_config.enabled = enabled;
    led_nvs_storage_save_enabled(enabled);

    if (!enabled) {
        /* INV-2: Immediate physical shutoff — cancel animations and one-shots */
        s_current_oneshot = ONESHOT_NONE;
        s_active_registered_anim = NULL;
        s_current_anim_id = "off";
        /* Reset flag so apply_physical_off() will transmit the OFF frame */
        s_physical_off_sent = false;
        apply_physical_off();
        ESP_LOGI(TAG, "LED notification turned OFF globally");
    } else {
        /* Re-enabling: clear off-sent flag so idle path re-evaluates correctly */
        s_physical_off_sent = false;
        s_current_anim_id = "none";
        s_hold_remaining_ms = 0;
        ESP_LOGI(TAG, "LED notification turned ON globally, resuming animation evaluation");
        /* Next step() will evaluate current priority and start the appropriate animation */
    }

    led_notification_wake_task();
    return true;
}

const char *led_notification_get_mapping(led_event_type_t event)
{
    if ((int)event >= 0 && event < LED_EVENT_COUNT) {
        return s_config.mapping[event];
    }
    return "unknown";
}

bool led_notification_set_mapping(led_event_type_t event, const char *anim_id)
{
    if ((int)event < 0 || event >= LED_EVENT_COUNT || anim_id == NULL) {
        return false;
    }

    if (led_animation_registry_find_by_id(anim_id) == NULL) {
        return false;
    }

    strncpy(s_config.mapping[event], anim_id, LED_ANIM_ID_MAX_LEN - 1);
    s_config.mapping[event][LED_ANIM_ID_MAX_LEN - 1] = '\0';

    bool ok = led_nvs_storage_save_mapping(event, anim_id);
    if (ok) {
        led_notification_wake_task();
    }
    return ok;
}

bool led_notification_reset_mappings(int event)
{
    bool ok = led_nvs_storage_reset_mappings(event, &s_config);
    if (ok) {
        led_notification_wake_task();
    }
    return ok;
}

void led_notification_post_cmd_result(bool success)
{
    if (!s_config.enabled) {
        /* Absolute OFF rule: drops notifications when disabled */
        return;
    }

    /* Do not allow low-priority success to override an active failure notification */
    if (s_current_oneshot == ONESHOT_CMD_FAILURE && success) {
        return;
    }

    s_current_oneshot = success ? ONESHOT_CMD_SUCCESS : ONESHOT_CMD_FAILURE;

    /* Switch immediately to the mapped animation */
    led_event_type_t evt = success ? LED_EVENT_CMD_SUCCESS : LED_EVENT_CMD_FAILURE;
    switch_to_animation(s_config.mapping[evt]);

    led_notification_wake_task();
}

bool led_notification_preview(const char *anim_id)
{
    if (!s_config.enabled || anim_id == NULL) {
        return false;
    }

    if (led_animation_registry_find_by_id(anim_id) == NULL) {
        return false;
    }

    strncpy(s_preview_anim_id, anim_id, sizeof(s_preview_anim_id) - 1);
    s_preview_anim_id[sizeof(s_preview_anim_id) - 1] = '\0';

    s_current_oneshot = ONESHOT_PREVIEW;
    switch_to_animation(s_preview_anim_id);

    led_notification_wake_task();
    return true;
}

const char *led_notification_get_active_animation_id(void)
{
    if (!s_config.enabled) {
        return "off";
    }
    return s_current_anim_id;
}

bool led_notification_is_boot_active(void)
{
    return s_boot_active;
}

void led_notification_set_boot_active(bool active)
{
    s_boot_active = active;
}

uint32_t led_notification_get_next_delay_ms(void)
{
    /* Globally disabled or idle with OFF already sent: deep sleep to conserve CPU */
    if (!s_config.enabled || (s_active_registered_anim == NULL && s_physical_off_sent)) {
        return 500;
    }

    /* Idle but OFF frame not yet transmitted: wake quickly to send it */
    if (s_active_registered_anim == NULL && !s_physical_off_sent) {
        return 10;
    }

    /* Active animation with remaining hold: bounded wait between 10ms and 50ms */
    if (s_hold_remaining_ms > 0) {
        uint32_t wait_ms = s_hold_remaining_ms;
        if (wait_ms > 50) {
            wait_ms = 50;
        }
        if (wait_ms < 10) {
            wait_ms = 10;
        }
        return wait_ms;
    }

    return 20;
}

/**
 * @brief Main step/tick function — called periodically from the LED task.
 *
 * ## Physical OFF Guarantee
 *
 * IDLE path: when no animation resolves, apply_physical_off() is ALWAYS called.
 * This is NOT conditional on whether an animation was previously active.
 * The s_physical_off_sent flag prevents redundant writes while ensuring
 * exactly one {0,0,0} GRB frame is transmitted per idle transition.
 *
 * ## Previous Bug (FIXED)
 *
 * The old code only called led_driver_clear() when:
 *   (s_active_registered_anim != NULL || oneshot_finished)
 * This meant: if the animation had already been set to NULL in a prior tick,
 * the clear was NEVER called again. The WS2812 kept its last physical color
 * (e.g. blue from police_flasher or rainbow) indefinitely.
 *
 * @param elapsed_ms Milliseconds since last call.
 */
void led_notification_step(uint32_t elapsed_ms)
{
    if (!s_initialized) {
        led_notification_init();
    }

    /* INV-2: Master absolute off check — if globally disabled, always apply OFF */
    if (!s_config.enabled) {
        apply_physical_off();
        return;
    }

    /* Update boot window flag based on wall-clock elapsed time or simulated step */
    int64_t now_us = esp_timer_get_time();
    if (s_boot_active) {
        if ((now_us - s_boot_start_time_us) / 1000 >= BOOT_WINDOW_MS || elapsed_ms >= BOOT_WINDOW_MS) {
            s_boot_active = false;
        }
    }

    /* Advance hold timer for current animation frame */
    if (s_hold_remaining_ms > elapsed_ms) {
        s_hold_remaining_ms -= elapsed_ms;
    } else {
        s_hold_remaining_ms = 0;
    }

    /* Determine highest priority required state */
    bool is_pairing = (zigbee_app_get_state() == ZB_STATE_PAIRING);

    /* Track whether the active one-shot just completed its full sequence */
    bool oneshot_finished = false;

    /* Step the active animation frame if hold duration elapsed */
    if (s_hold_remaining_ms == 0 && s_active_registered_anim != NULL) {
        led_anim_step_result_t res = s_active_registered_anim->anim->step(
            s_anim_state_buffer, elapsed_ms);

        /* Write new color frame to WS2812 hardware */
        led_driver_set_color(res.color);
        s_hold_remaining_ms = res.hold_duration_ms;

        if (res.sequence_finished) {
            if (s_current_oneshot != ONESHOT_NONE) {
                oneshot_finished = true;
                s_current_oneshot = ONESHOT_NONE;
            }
        }
    }

    /* =========================================================================
     * Priority Resolution:
     *   1. Zigbee Pairing (Highest — overrides everything)
     *   2. Active One-Shot (Cmd Failure, Cmd Success, Preview)
     *   3. Boot Window (first 10 seconds)
     *   4. Normal Operational (Zigbee joined + Z906 amp operational)
     *   5. Idle/Off fallback (no active condition)
     * ========================================================================= */
    (void)oneshot_finished; /* Resolved implicitly: s_current_oneshot cleared above */

    const char *target_anim_id = NULL;

    if (is_pairing) {
        target_anim_id = s_config.mapping[LED_EVENT_PAIRING];
    } else if (s_current_oneshot != ONESHOT_NONE) {
        if (s_current_oneshot == ONESHOT_CMD_FAILURE) {
            target_anim_id = s_config.mapping[LED_EVENT_CMD_FAILURE];
        } else if (s_current_oneshot == ONESHOT_CMD_SUCCESS) {
            target_anim_id = s_config.mapping[LED_EVENT_CMD_SUCCESS];
        } else if (s_current_oneshot == ONESHOT_PREVIEW) {
            target_anim_id = s_preview_anim_id;
        }
    } else if (s_boot_active) {
        target_anim_id = s_config.mapping[LED_EVENT_BOOT];
    } else {
        /* Check if device is in normal working state */
        z906_state_t st;
        z906_state_get(&st);
        bool zb_joined = (zigbee_app_get_state() == ZB_STATE_JOINED);

        if (zb_joined && st.is_operational) {
            target_anim_id = s_config.mapping[LED_EVENT_NORMAL];
        } else {
            /* No active condition resolves — target is idle/off */
            target_anim_id = NULL;
        }
    }

    /* =========================================================================
     * ACTIVE vs IDLE dispatch with Physical OFF guarantee.
     *
     * ACTIVE: switch_to_animation() handles startup and no-op-if-same logic.
     *
     * IDLE: No animation resolved.
     *   INV-1/INV-3/INV-4: physical LED MUST be off.
     *   apply_physical_off() is called unconditionally — it is idempotent and
     *   guarded by s_physical_off_sent to prevent per-tick polling.
     *
     *   IMPORTANT: Do NOT use the old conditional pattern:
     *     if (s_active_registered_anim != NULL || oneshot_finished) { clear(); }
     *   That pattern fails when s_active_registered_anim is already NULL from a
     *   prior tick — the clear is never called and the WS2812 retains its last color.
     * ========================================================================= */
    if (target_anim_id != NULL) {
        switch_to_animation(target_anim_id);
    } else {
        /* No animation required — enforce physical OFF invariant */
        s_active_registered_anim = NULL;
        s_current_anim_id = "off";

        /* INV-1 / INV-3 / INV-4: Guarantee one explicit black frame to WS2812 hardware.
         * apply_physical_off() is idempotent: only transmits if not already done. */
        apply_physical_off();
    }
}

/* =========================================================================
 * LED Task — owned by management/led_notification (led_notification_manager.c)
 *
 * Ownership rationale: The LED animation task drives led_notification_step()
 * and led_notification_get_next_delay_ms() — both are private concerns of this
 * module. Lifecycle (create/start/run) lives here. The system initializer only
 * calls led_notification_start().
 *
 * Degraded gating: If led_notification_init() did not succeed, start() returns
 * false immediately without spawning a task that would call uninitialized state.
 * ========================================================================= */

#include "freertos/task.h"
#include "freertos/event_groups.h"

/** @brief Stack depth in words for the LED animation player task */
#define LED_TASK_STACK_WORDS    (2048u / sizeof(StackType_t))

/** @brief FreeRTOS priority for the LED player task (lowest, non-critical) */
#define LED_TASK_PRIORITY       1u

/** @brief Max ms to wait for the task to signal STARTED before timeout */
#define LED_STARTUP_TIMEOUT_MS  500u

/* s_led_init_ok is declared at module scope above led_notification_init() */

static StackType_t  s_led_stack[LED_TASK_STACK_WORDS];
static StaticTask_t s_led_tcb;

/** @brief Private startup binary semaphore — given by task once it enters main loop */
static SemaphoreHandle_t s_started_sem    = NULL;
static StaticSemaphore_t s_started_sem_buf;

/**
 * @brief LED animation player task body.
 *
 * Event-driven: sleeps up to the next suggested frame interval or wakes
 * immediately via ulTaskNotifyTake on command result events / enable/disable.
 *
 * Does NOT register with TWDT because ulTaskNotifyTake blocking is incompatible
 * with periodic wdt_manager_reset() patterns. LED animation failure is non-fatal.
 *
 * No EventGroup gate — startup handled by private semaphore handshake in
 * led_notification_start(). Task signals STARTED before entering its loop.
 *
 * @param pvParameters Unused.
 */
static void prv_led_task(void *pvParameters)
{
    (void)pvParameters; /* No gate handle — startup handled by private semaphore */

    ESP_LOGI(TAG, "led_task running (Priority %u, event-driven)", LED_TASK_PRIORITY);

    /* Signal led_notification_start() that this task is alive and entering its loop */
    xSemaphoreGive(s_started_sem);

    TickType_t last_wake_time = xTaskGetTickCount();

    while (1) {
        TickType_t now        = xTaskGetTickCount();
        uint32_t   elapsed_ms = pdTICKS_TO_MS(now - last_wake_time);

        /* Guarantee non-zero delta to prevent division-by-zero in animation math */
        if (elapsed_ms == 0) {
            elapsed_ms = 1;
        }
        last_wake_time = now;

        /* Advance current animation by elapsed milliseconds */
        led_notification_step(elapsed_ms);

        /* Sleep for the exact frame interval or wake instantly on notification */
        uint32_t   next_delay_ms = led_notification_get_next_delay_ms();
        TickType_t wait_ticks    = pdMS_TO_TICKS(next_delay_ms);
        if (wait_ticks == 0) {
            wait_ticks = 1;
        }

        ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

bool led_notification_start(void)
{
    /* Guard: do not spawn a LED task when init failed. The animation registry
     * and NVS config are not populated — calling led_notification_step() would
     * operate on uninitialized state. */
    if (!s_led_init_ok) {
        ESP_LOGW(TAG, "led_notification_start skipped: init did not succeed");
        return false;
    }

    /* Create the binary startup semaphore — starts empty (task must give it) */
    s_started_sem = xSemaphoreCreateBinaryStatic(&s_started_sem_buf);

    TaskHandle_t handle = xTaskCreateStatic(
        prv_led_task,
        "led_task",
        LED_TASK_STACK_WORDS,
        NULL,              /* pvParameters: no gate handle needed */
        LED_TASK_PRIORITY,
        s_led_stack,
        &s_led_tcb
    );

    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to create led_task");
        return false;
    }

    /* Register the task handle immediately so notification-based wakeup works.
     * This replaces the separate led_notification_register_task_handle() call
     * that was previously in the system initializer after task spawn. */
    led_notification_register_task_handle((void *)handle);

    /* Block until task signals STARTED or timeout */
    BaseType_t got = xSemaphoreTake(s_started_sem,
                                    pdMS_TO_TICKS(LED_STARTUP_TIMEOUT_MS));
    if (got != pdTRUE) {
        ESP_LOGE(TAG, "led_task did not signal STARTED within %u ms",
                 LED_STARTUP_TIMEOUT_MS);
        return false;
    }

    ESP_LOGI(TAG, "led_task STARTED (Priority %u, stack %u words)",
             LED_TASK_PRIORITY, LED_TASK_STACK_WORDS);
    return true;
}
