/**
 * @file z906_state.c
 * @brief Implementation of the thread-safe confirmed hardware state cache.
 *
 * ## Observer Pattern
 *
 * A single registered callback (`s_change_observer`) is invoked after the mutex
 * is RELEASED whenever any attribute changes. This keeps callbacks outside the
 * critical section (no reentrancy hazard, no mutex acquisition inside callback).
 *
 * The callback receives a bitmask identifying which attribute changed, using the
 * ZB_PUB_NOTIF_* bit constants defined in zigbee_state_publisher.h. The state
 * manager itself does NOT interpret these bits — it merely passes the value the
 * caller provided. This avoids a direct dependency from core → communication.
 *
 * The FSM does NOT call zigbee_state_publisher_notify() directly. Instead, it
 * calls z906_state_set_*() which fires the observer if the value changed.
 */

#include "z906_state.h"
#include "z906_commands.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "STATE_MGR";

/* -------------------------------------------------------------------------
 * Static state storage and synchronization primitives
 * ------------------------------------------------------------------------- */

static z906_state_t      s_current_state;
static SemaphoreHandle_t s_state_mutex  = NULL;
static StaticSemaphore_t s_mutex_buffer;
/* Protected by s_state_mutex after init; updated only by verified CONFIG frames. */
static int64_t s_last_main_status_time_us = 0;
/* Only an in-window reply to a telemetry STATUS request is OTA evidence. */
static int64_t s_last_matched_status_time_us = 0;
/* Monotonic logical cable session, protected by the existing state mutex. */
static uint32_t s_cable_epoch = 0;
/* Shared receive continuity generation, protected by the existing state mutex.
 * A failed EventBus post invalidates prior OTA proof immediately, even if no
 * further amplifier byte reaches the FSM. */
static uint32_t s_amp_rx_stream_epoch = 0;

/* Single-slot observer — NULL if no observer is registered */
static z906_state_change_cb_t s_change_observer = NULL;

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Invokes the registered observer (if any) AFTER the mutex is released.
 *
 * Must be called OUTSIDE the state mutex to avoid reentrancy deadlocks
 * (e.g., if the observer itself calls z906_state_get() inside the callback).
 *
 * @param change_mask  Attribute bitmask to pass to the observer.
 */
static inline void prv_notify_observer(uint32_t change_mask)
{
    /* Load atomically — single-slot, written only during init phase */
    z906_state_change_cb_t cb = s_change_observer;
    if (cb != NULL) {
        cb(change_mask);
    }
}

/* =========================================================================
 * Public API — Lifecycle
 * ========================================================================= */

void z906_state_init(void)
{
    if (s_state_mutex == NULL) {
        /* Create a static mutex so no dynamic allocation happens at runtime */
        s_state_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
    }

    /* Reset all fields to safe, unverified defaults */
    s_current_state.power                 = false;
    s_current_state.mute                  = false;
    s_current_state.volume                = Z906_DEFAULT_VOLUME_LEVEL;
    s_current_state.volume_bass           = Z906_DEFAULT_VOLUME_LEVEL;
    s_current_state.volume_center         = Z906_DEFAULT_VOLUME_LEVEL;
    s_current_state.volume_rear           = Z906_DEFAULT_VOLUME_LEVEL;
    s_current_state.input                 = Z906_INPUT_SIX_CHANNEL_DIRECT;
    s_current_state.effect                = Z906_EFFECT_3D;
    s_current_state.digital_signal_status = 0x00;
    s_current_state.auto_standby_enabled  = true;
    s_current_state.temp_celsius[0]       = 0;
    s_current_state.temp_celsius[1]       = 0;
    s_current_state.temp_celsius[2]       = 0;
    s_current_state.temp_valid            = false; /* Not safe to report to HA */
    s_current_state.idle_seconds          = 0;
    s_current_state.input_signal_level    = 0;
    s_current_state.cable_connected       = false;
    s_current_state.is_operational        = false;
    s_current_state.main_state_valid      = false; /* No status frame received */
    s_last_main_status_time_us             = 0;
    s_last_matched_status_time_us          = 0;
    s_cable_epoch                          = 0;
    s_amp_rx_stream_epoch                  = 0;

    ESP_LOGI(TAG, "Z906 state store initialized with defaults");
}

void z906_state_register_change_observer(z906_state_change_cb_t cb)
{
    /* Single-slot — overwrites previous registration (or NULL to deregister) */
    s_change_observer = cb;
    if (cb != NULL) {
        ESP_LOGD(TAG, "State change observer registered");
    } else {
        ESP_LOGD(TAG, "State change observer deregistered");
    }
}

/* =========================================================================
 * Public API — Read
 * ========================================================================= */

void z906_state_get(z906_state_t *out_state)
{
    if (out_state == NULL || s_state_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out_state = s_current_state;
    xSemaphoreGive(s_state_mutex);
}

/* =========================================================================
 * Public API — Write (each notifies observer on change)
 * ========================================================================= */

bool z906_state_set_volume(uint8_t volume)
{
    /* Enforce hardware bounds: 0..43 */
    if (volume > Z906_MAX_VOLUME_LEVEL) {
        volume = Z906_MAX_VOLUME_LEVEL;
    }

    bool changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_current_state.volume != volume) {
        s_current_state.volume = volume;
        changed = true;
    }
    xSemaphoreGive(s_state_mutex);

    /* Notify outside mutex to avoid reentrancy: mask bit 1 (ZB_PUB_ATTR_VOLUME) */
    if (changed) {
        prv_notify_observer(1UL << 1);
    }
    return changed;
}

bool z906_state_set_trim(uint8_t channel, uint8_t level)
{
    if (level > Z906_MAX_VOLUME_LEVEL) {
        level = Z906_MAX_VOLUME_LEVEL;
    }

    bool     changed      = false;
    uint32_t change_bit   = 0;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    switch (channel) {
        case 0: /* Bass — ZB_PUB_ATTR_TRIM_BASS = bit 2 */
            if (s_current_state.volume_bass != level) {
                s_current_state.volume_bass = level;
                changed    = true;
                change_bit = 1UL << 2;
            }
            break;
        case 1: /* Center — ZB_PUB_ATTR_TRIM_CENTER = bit 3 */
            if (s_current_state.volume_center != level) {
                s_current_state.volume_center = level;
                changed    = true;
                change_bit = 1UL << 3;
            }
            break;
        case 2: /* Rear — ZB_PUB_ATTR_TRIM_REAR = bit 4 */
            if (s_current_state.volume_rear != level) {
                s_current_state.volume_rear = level;
                changed    = true;
                change_bit = 1UL << 4;
            }
            break;
        default:
            break;
    }
    xSemaphoreGive(s_state_mutex);

    if (changed) {
        prv_notify_observer(change_bit);
    }
    return changed;
}

bool z906_state_set_input(z906_input_t input)
{
    /* Reject values outside the canonical input range */
    if (input < Z906_INPUT_SIX_CHANNEL_DIRECT || input > Z906_INPUT_AUXILIARY) {
        return false;
    }

    bool changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_current_state.input != input) {
        s_current_state.input = input;
        changed = true;
    }
    xSemaphoreGive(s_state_mutex);

    /* ZB_PUB_ATTR_INPUT = bit 6 */
    if (changed) {
        prv_notify_observer(1UL << 6);
    }
    return changed;
}

bool z906_state_set_effect(z906_effect_t effect)
{
    bool changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_current_state.effect != effect) {
        s_current_state.effect = effect;
        changed = true;
    }
    xSemaphoreGive(s_state_mutex);

    /* ZB_PUB_ATTR_EFFECT = bit 7 */
    if (changed) {
        prv_notify_observer(1UL << 7);
    }
    return changed;
}

bool z906_state_set_power(bool power)
{
    bool changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_current_state.power != power) {
        s_current_state.power = power;
        changed = true;
    }
    xSemaphoreGive(s_state_mutex);

    /* ZB_PUB_ATTR_POWER = bit 0 */
    if (changed) {
        prv_notify_observer(1UL << 0);
    }
    return changed;
}

bool z906_state_set_mute(bool mute)
{
    bool changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_current_state.mute != mute) {
        s_current_state.mute = mute;
        changed = true;
    }
    xSemaphoreGive(s_state_mutex);

    /* ZB_PUB_ATTR_MUTE = bit 5 */
    if (changed) {
        prv_notify_observer(1UL << 5);
    }
    return changed;
}

bool z906_state_set_digital_signal_status(uint8_t status)
{
    bool changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_current_state.digital_signal_status != status) {
        s_current_state.digital_signal_status = status;
        changed = true;
    }
    xSemaphoreGive(s_state_mutex);

    /* ZB_PUB_ATTR_DIGITAL_SIGNAL_STATUS = bit 10 */
    if (changed) {
        prv_notify_observer(1UL << 10);
    }
    return changed;
}

bool z906_state_set_auto_standby(bool enabled)
{
    bool changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_current_state.auto_standby_enabled != enabled) {
        s_current_state.auto_standby_enabled = enabled;
        changed = true;
    }
    xSemaphoreGive(s_state_mutex);

    /* ZB_PUB_ATTR_AUTO_STANDBY = bit 11 */
    if (changed) {
        prv_notify_observer(1UL << 11);
    }
    return changed;
}

/* Called only while s_state_mutex is held. */
static bool prv_amp_rx_generations_match(uint32_t cable_epoch, uint32_t stream_epoch)
{
    return s_current_state.cable_connected &&
           s_cable_epoch == cable_epoch &&
           s_amp_rx_stream_epoch == stream_epoch;
}

bool z906_state_apply_temperature_for_generations(uint32_t cable_epoch,
                                                   uint32_t stream_epoch,
                                                   const int8_t temps[3])
{
    if (temps == NULL || s_state_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool accepted = prv_amp_rx_generations_match(cable_epoch, stream_epoch);
    bool changed = false;
    if (accepted) {
        changed = !s_current_state.temp_valid;
        for (int i = 0; i < 3; i++) {
            if (s_current_state.temp_celsius[i] != temps[i]) {
                s_current_state.temp_celsius[i] = temps[i];
                changed = true;
            }
        }
        s_current_state.temp_valid = true;
    }
    xSemaphoreGive(s_state_mutex);
    if (accepted && changed) {
        prv_notify_observer((1UL << 14) | (1UL << 15) | (1UL << 16));
    }
    return accepted;
}

bool z906_state_apply_idle_for_generations(uint32_t cable_epoch,
                                           uint32_t stream_epoch,
                                           uint32_t seconds)
{
    if (s_state_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool accepted = prv_amp_rx_generations_match(cable_epoch, stream_epoch);
    bool changed = accepted && s_current_state.idle_seconds != seconds;
    if (changed) {
        s_current_state.idle_seconds = seconds;
    }
    xSemaphoreGive(s_state_mutex);
    if (changed) {
        prv_notify_observer(1UL << 9);
    }
    return accepted;
}

bool z906_state_apply_input_level_for_generations(uint32_t cable_epoch,
                                                  uint32_t stream_epoch,
                                                  uint32_t level)
{
    if (s_state_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool accepted = prv_amp_rx_generations_match(cable_epoch, stream_epoch);
    bool changed = accepted && s_current_state.input_signal_level != level;
    if (changed) {
        s_current_state.input_signal_level = level;
    }
    xSemaphoreGive(s_state_mutex);
    if (changed) {
        prv_notify_observer(1UL << 8);
    }
    return accepted;
}

bool z906_state_set_temperature(const int8_t temps[3])
{
    if (temps == NULL || s_state_mutex == NULL) {
        return false;
    }

    bool changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    if (s_current_state.temp_celsius[0] != temps[0] ||
        s_current_state.temp_celsius[1] != temps[1] ||
        s_current_state.temp_celsius[2] != temps[2]) {
        s_current_state.temp_celsius[0] = temps[0];
        s_current_state.temp_celsius[1] = temps[1];
        s_current_state.temp_celsius[2] = temps[2];
        changed = true;
    }

    /* Mark as valid after first integrity-verified protocol call */
    if (!s_current_state.temp_valid) {
        s_current_state.temp_valid = true;
        changed = true; /* Force report even if values happen to be 0 */
    }

    xSemaphoreGive(s_state_mutex);

    /* ZB_PUB_ATTR_TEMP1..TEMP3 = bits 14, 15, 16 */
    if (changed) {
        prv_notify_observer((1UL << 14) | (1UL << 15) | (1UL << 16));
    }
    return changed;
}

bool z906_state_set_idle_seconds(uint32_t seconds)
{
    if (s_state_mutex == NULL) {
        return false;
    }

    bool changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_current_state.idle_seconds != seconds) {
        s_current_state.idle_seconds = seconds;
        changed = true;
    }
    xSemaphoreGive(s_state_mutex);

    /* ZB_PUB_ATTR_IDLE_TIME = bit 9 */
    if (changed) {
        prv_notify_observer(1UL << 9);
    }
    return changed;
}

bool z906_state_set_input_signal_level(uint32_t level)
{
    if (s_state_mutex == NULL) {
        return false;
    }

    bool changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_current_state.input_signal_level != level) {
        s_current_state.input_signal_level = level;
        changed = true;
    }
    xSemaphoreGive(s_state_mutex);

    /* ZB_PUB_ATTR_INPUT_SIGNAL_LEVEL = bit 8 */
    if (changed) {
        prv_notify_observer(1UL << 8);
    }
    return changed;
}

void z906_state_set_cable_status(bool connected)
{
    bool changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_current_state.cable_connected != connected ||
        s_current_state.is_operational  != connected) {
        s_current_state.cable_connected = connected;
        s_current_state.is_operational  = connected;
        changed = true;
    }
    if (changed) {
        ++s_cable_epoch; /* Even if the EventBus cable event is dropped. */
        ++s_amp_rx_stream_epoch; /* Physical session also breaks RX continuity. */
    }
    if (changed || !connected) {
        /* Each physical cable epoch requires a new verified CONFIG frame.
         * Reset both on disconnect and reconnect, never reuse old state. */
        s_current_state.main_state_valid = false;
        s_current_state.temp_valid       = false;
        s_last_main_status_time_us       = 0;
        s_last_matched_status_time_us    = 0;
    }
    xSemaphoreGive(s_state_mutex);

    /* Notify all hardware attributes — coordinator needs fresh state */
    if (changed) {
        prv_notify_observer((1UL << 17) - 1UL); /* All 17 attribute bits */
    }
}

uint32_t z906_state_get_amp_rx_stream_epoch(void)
{
    if (s_state_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    uint32_t epoch = s_amp_rx_stream_epoch;
    xSemaphoreGive(s_state_mutex);
    return epoch;
}

uint32_t z906_state_note_amp_rx_loss(uint32_t expected_cable_epoch)
{
    if (s_state_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool affected = s_current_state.cable_connected &&
                    s_cable_epoch == expected_cable_epoch;
    /* Temperature is an independent Zigbee validity channel. */
    bool was_valid = affected && (s_current_state.main_state_valid ||
                                  s_current_state.temp_valid);
    if (affected) {
        ++s_amp_rx_stream_epoch;
        s_current_state.main_state_valid = false;
        s_current_state.temp_valid = false;
        s_last_main_status_time_us = 0;
        s_last_matched_status_time_us = 0;
    }
    uint32_t current = s_amp_rx_stream_epoch;
    xSemaphoreGive(s_state_mutex);
    if (was_valid) {
        /* Notify even when ONLY temperature was valid: the publisher must
         * replace a stale temperature with its invalid sentinel immediately.
         * Observer is always invoked outside the state mutex. */
        prv_notify_observer((1UL << 17) - 1UL);
    }
    return current;
}

uint32_t z906_state_get_cable_epoch(void)
{
    if (s_state_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    uint32_t epoch = s_cable_epoch;
    xSemaphoreGive(s_state_mutex);
    return epoch;
}

/**
 * @brief Applies one amplifier echo atomically against the cable and RX
 *        continuity generations. No new mutex: the existing state lock
 *        prevents a cable transition or loss event between check and write.
 */
bool z906_state_apply_amp_echo_for_generations(uint32_t cable_epoch,
                                               uint32_t rx_stream_epoch,
                                               uint8_t opcode)
{
    if (s_state_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_current_state.cable_connected || s_cable_epoch != cable_epoch ||
        s_amp_rx_stream_epoch != rx_stream_epoch) {
        xSemaphoreGive(s_state_mutex);
        return false;
    }

    z906_state_t next = s_current_state;
    uint32_t changed_bit = 0;
    switch (opcode) {
        case Z906_CMD_STANDBY_SAVE:
            next.power = false; changed_bit = 1UL << 0; break;
        case Z906_CMD_AMP_MUTED:
            next.mute = true; changed_bit = 1UL << 5; break;
        case Z906_CMD_AMP_UNMUTED:
            next.mute = false; changed_bit = 1UL << 5; break;
        case Z906_CMD_VOLUME_UP:
            if (next.volume < Z906_MAX_VOLUME_LEVEL) ++next.volume;
            changed_bit = 1UL << 1; break;
        case Z906_CMD_VOLUME_DOWN:
            if (next.volume > 0) --next.volume;
            changed_bit = 1UL << 1; break;
        case Z906_CMD_VOLUME_BASS_UP:
            if (next.volume_bass < Z906_MAX_VOLUME_LEVEL) ++next.volume_bass;
            changed_bit = 1UL << 2; break;
        case Z906_CMD_VOLUME_BASS_DOWN:
            if (next.volume_bass > 0) --next.volume_bass;
            changed_bit = 1UL << 2; break;
        case Z906_CMD_VOLUME_CENTER_UP:
            if (next.volume_center < Z906_MAX_VOLUME_LEVEL) ++next.volume_center;
            changed_bit = 1UL << 3; break;
        case Z906_CMD_VOLUME_CENTER_DOWN:
            if (next.volume_center > 0) --next.volume_center;
            changed_bit = 1UL << 3; break;
        case Z906_CMD_VOLUME_REAR_UP:
            if (next.volume_rear < Z906_MAX_VOLUME_LEVEL) ++next.volume_rear;
            changed_bit = 1UL << 4; break;
        case Z906_CMD_VOLUME_REAR_DOWN:
            if (next.volume_rear > 0) --next.volume_rear;
            changed_bit = 1UL << 4; break;
        case Z906_CMD_SELECT_INPUT_1:
            next.input = Z906_INPUT_SIX_CHANNEL_DIRECT; changed_bit = 1UL << 6; break;
        case Z906_CMD_SELECT_INPUT_2:
            next.input = Z906_INPUT_RCA_STEREO; changed_bit = 1UL << 6; break;
        case Z906_CMD_SELECT_INPUT_3:
            next.input = Z906_INPUT_OPTICAL_1; changed_bit = 1UL << 6; break;
        case Z906_CMD_SELECT_INPUT_4:
            next.input = Z906_INPUT_OPTICAL_2; changed_bit = 1UL << 6; break;
        case Z906_CMD_SELECT_INPUT_5:
            next.input = Z906_INPUT_DIGITAL_COAXIAL; changed_bit = 1UL << 6; break;
        case Z906_CMD_SELECT_INPUT_6:
            next.input = Z906_INPUT_AUXILIARY; changed_bit = 1UL << 6; break;
        case Z906_CMD_SELECT_EFFECT_3D:
            next.effect = Z906_EFFECT_3D; changed_bit = 1UL << 7; break;
        case Z906_CMD_SELECT_EFFECT_2_1:
            next.effect = Z906_EFFECT_2_1; changed_bit = 1UL << 7; break;
        case Z906_CMD_SELECT_EFFECT_4_1:
            next.effect = Z906_EFFECT_4_1; changed_bit = 1UL << 7; break;
        case Z906_CMD_SELECT_EFFECT_NONE:
            next.effect = Z906_EFFECT_NONE; changed_bit = 1UL << 7; break;
        default:
            if (opcode >= Z906_DECODE_5_1 && opcode <= Z906_DECODE_UNKNOWN) {
                next.digital_signal_status = opcode;
                changed_bit = 1UL << 10;
            }
            break;
    }

    bool recognized = changed_bit != 0;
    bool changed = false;
    switch (changed_bit) {
        case 1UL << 0: changed = next.power != s_current_state.power; break;
        case 1UL << 1: changed = next.volume != s_current_state.volume; break;
        case 1UL << 2: changed = next.volume_bass != s_current_state.volume_bass; break;
        case 1UL << 3: changed = next.volume_center != s_current_state.volume_center; break;
        case 1UL << 4: changed = next.volume_rear != s_current_state.volume_rear; break;
        case 1UL << 5: changed = next.mute != s_current_state.mute; break;
        case 1UL << 6: changed = next.input != s_current_state.input; break;
        case 1UL << 7: changed = next.effect != s_current_state.effect; break;
        case 1UL << 10:
            changed = next.digital_signal_status != s_current_state.digital_signal_status;
            break;
        default: break;
    }
    if (changed) {
        s_current_state = next;
    }
    xSemaphoreGive(s_state_mutex);
    if (changed) {
        prv_notify_observer(changed_bit);
    }
    return recognized;
}

bool z906_state_apply_verified_config_for_epoch(uint32_t epoch,
                                                uint32_t rx_stream_epoch,
                                                const z906_state_t *verified)
{
    if (s_state_mutex == NULL || verified == NULL ||
        verified->volume > Z906_MAX_VOLUME_LEVEL ||
        verified->volume_bass > Z906_MAX_VOLUME_LEVEL ||
        verified->volume_center > Z906_MAX_VOLUME_LEVEL ||
        verified->volume_rear > Z906_MAX_VOLUME_LEVEL ||
        verified->input < Z906_INPUT_SIX_CHANNEL_DIRECT ||
        verified->input > Z906_INPUT_AUXILIARY ||
        verified->effect < Z906_EFFECT_3D ||
        verified->effect > Z906_EFFECT_NONE) {
        return false;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool accepted = s_current_state.cable_connected && s_cable_epoch == epoch &&
                    s_amp_rx_stream_epoch == rx_stream_epoch;
    if (accepted) {
        /* One commit: no reader, cable transition or dropped RX byte can observe a partial
         * CONFIG field update or a validity flag for another cable epoch. */
        s_current_state.volume = verified->volume;
        s_current_state.volume_bass = verified->volume_bass;
        s_current_state.volume_center = verified->volume_center;
        s_current_state.volume_rear = verified->volume_rear;
        s_current_state.input = verified->input;
        s_current_state.effect = verified->effect;
        s_current_state.mute = verified->mute;
        s_current_state.digital_signal_status = verified->digital_signal_status;
        s_current_state.auto_standby_enabled = verified->auto_standby_enabled;
        s_current_state.power = verified->power;
        s_current_state.main_state_valid = true;
        s_last_main_status_time_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_state_mutex);

    if (accepted) {
        /* Previously each field setter notified, followed by the full
         * main-state notification. Keep the full refresh, but publish only
         * the coherent confirmed snapshot after releasing the state mutex. */
        prv_notify_observer((1UL << 17) - 1UL);
    }
    return accepted;
}

bool z906_state_confirm_main_for_epoch(uint32_t epoch)
{
    if (s_state_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool accepted = s_current_state.cable_connected && s_cable_epoch == epoch;
    if (accepted) {
        s_current_state.main_state_valid = true;
        s_last_main_status_time_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_state_mutex);
    if (accepted) {
        prv_notify_observer((1UL << 17) - 1UL);
    }
    return accepted;
}

void z906_state_set_main_valid(bool valid)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    /* Queued amplifier bytes must not resurrect validity while unplugged. */
    bool accepted = valid && s_current_state.cable_connected;
    s_current_state.main_state_valid = accepted;
    s_last_main_status_time_us = accepted ? esp_timer_get_time() : 0;
    if (!accepted) {
        s_last_matched_status_time_us = 0;
    }
    xSemaphoreGive(s_state_mutex);

    /* Notify all hardware attributes — first verified frame is special */
    prv_notify_observer((1UL << 17) - 1UL);
}

void z906_state_note_matched_status_reply(int64_t response_time_us)
{
    if (s_state_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_current_state.main_state_valid && s_current_state.cable_connected &&
        response_time_us > 0 &&
        response_time_us >= s_last_main_status_time_us) {
        s_last_matched_status_time_us = response_time_us;
    }
    xSemaphoreGive(s_state_mutex);
}

bool z906_state_get_ota_status_snapshot(z906_ota_status_snapshot_t *out)
{
    if (out == NULL || s_state_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    out->main_valid = s_current_state.main_state_valid &&
                      s_current_state.cable_connected;
    out->cable_epoch = s_cable_epoch;
    out->rx_stream_epoch = s_amp_rx_stream_epoch;
    out->matched_status_us = out->main_valid ? s_last_matched_status_time_us : 0;
    xSemaphoreGive(s_state_mutex);
    return true;
}

int64_t z906_state_get_matched_status_reply_time_us(void)
{
    if (s_state_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int64_t confirmed = (s_current_state.main_state_valid && s_current_state.cable_connected)
        ? s_last_matched_status_time_us : 0;
    xSemaphoreGive(s_state_mutex);
    return confirmed;
}

int64_t z906_state_get_main_status_time_us(void)
{
    if (s_state_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int64_t timestamp = (s_current_state.main_state_valid && s_current_state.cable_connected)
        ? s_last_main_status_time_us : 0;
    xSemaphoreGive(s_state_mutex);
    return timestamp;
}

bool z906_state_is_main_valid(void)
{
    if (s_state_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool valid = s_current_state.main_state_valid && s_current_state.cable_connected;
    xSemaphoreGive(s_state_mutex);
    return valid;
}
