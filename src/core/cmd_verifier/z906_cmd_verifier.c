/**
 * @file z906_cmd_verifier.c
 * @brief Implementation of the centralized state-based command verification engine.
 *
 * Fully compliant with the canonical reverse engineering reference documentation
 * (nomis/logitech-z906): protocol.rst, interface.rst, firmware.rst, README.rst.
 */

#include "z906_cmd_verifier.h"
#include "config.h"
#include "uart_hal.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_notification_manager.h"
#include "nvs_manager.h"

static const char *TAG = "CMD_VERIFY";

/** @brief Maximum duration in milliseconds to acquire the command execution mutex */
#define Z906_VERIFY_MUTEX_WAIT_MS       1000

/** @brief Maximum duration in milliseconds to await verified physical state change */
#define Z906_VERIFY_TIMEOUT_MS          500

/** @brief Polling interval in milliseconds while waiting for amplifier telemetry */
#define Z906_VERIFY_POLL_INTERVAL_MS    25

/** @brief Concurrency lock preventing race conditions between CLI and Zigbee tasks */
static SemaphoreHandle_t s_verifier_mutex = NULL;

/** @brief Static memory storage for the FreeRTOS mutex primitive */
static StaticSemaphore_t s_verifier_mutex_buffer;

/** @brief Runtime enabled flag for command state verification (default: true) */
static bool s_verifier_enabled = true;

bool z906_cmd_verifier_init(void)
{
    /* Initialize static mutex if not already created */
    if (s_verifier_mutex == NULL) {
        s_verifier_mutex = xSemaphoreCreateMutexStatic(&s_verifier_mutex_buffer);
        if (s_verifier_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create command verifier static mutex");
            return false;
        }
    }

    /* Load persistent enabled state from NVS (defaults to true if unset) */
    nvs_manager_get_cmd_verifier_enabled(&s_verifier_enabled);

    ESP_LOGI(TAG, "Command verifier service initialized successfully (enabled: %s)",
             s_verifier_enabled ? "ON" : "OFF");
    return true;
}

const char *z906_cmd_verify_result_str(cmd_verify_result_t result)
{
    switch (result) {
        case CMD_VERIFY_SUCCESS:
            return "SUCCESS";
        case CMD_VERIFY_SENT_UNVERIFIED:
            return "SENT_UNVERIFIED";
        case CMD_VERIFY_ERR_SEND_FAILED:
            return "UART_SEND_FAILED";
        case CMD_VERIFY_ERR_PRE_STATE_FAILED:
            return "PRE_STATE_READ_FAILED";
        case CMD_VERIFY_ERR_POST_STATE_FAILED:
            return "POST_STATE_READ_FAILED";
        case CMD_VERIFY_ERR_TARGET_NOT_REACHED:
            return "TARGET_NOT_REACHED";
        case CMD_VERIFY_ERR_UNVERIFIABLE:
            return "UNVERIFIABLE_COMMAND";
        case CMD_VERIFY_ERR_TIMEOUT:
            return "VERIFICATION_TIMEOUT";
        case CMD_VERIFY_ERR_PRECONDITION_FAILED:
            return "PRECONDITION_FAILED";
        case CMD_VERIFY_ERR_BUSY:
            return "VERIFIER_BUSY";
        default:
            return "UNKNOWN_RESULT";
    }
}

const char *z906_cmd_verify_type_str(verify_cmd_type_t type)
{
    switch (type) {
        case VERIFY_CMD_POWER:        return "POWER";
        case VERIFY_CMD_VOLUME:       return "VOLUME";
        case VERIFY_CMD_INPUT:        return "INPUT";
        case VERIFY_CMD_EFFECT:       return "EFFECT";
        case VERIFY_CMD_MUTE:         return "MUTE";
        case VERIFY_CMD_TRIM:         return "TRIM";
        case VERIFY_CMD_UNVERIFIABLE: return "UNVERIFIABLE";
        default:                      return "UNKNOWN";
    }
}

const char *z906_cmd_verify_source_str(cmd_source_t source)
{
    switch (source) {
        case CMD_SOURCE_CLI:    return "CLI";
        case CMD_SOURCE_ZIGBEE: return "ZIGBEE";
        default:                return "UNKNOWN";
    }
}

/**
 * @brief Computes the expected target state given the current state and requested command.
 *
 * @param req Pointer to the command request.
 * @param pre Pointer to the confirmed pre-execution state.
 * @param target Pointer to store the calculated target state.
 */
static void calculate_expected_target(const cmd_request_t *req, const z906_state_t *pre, z906_state_t *target)
{
    /* Initialize target state with current pre-execution state */
    *target = *pre;

    switch (req->type) {
        case VERIFY_CMD_POWER:
            target->power = req->param.power;
            break;

        case VERIFY_CMD_VOLUME: {
            uint8_t vol = req->param.volume;
            if (vol > Z906_MAX_VOLUME_LEVEL) {
                vol = Z906_MAX_VOLUME_LEVEL;
            }
            target->volume = vol;
            break;
        }

        case VERIFY_CMD_INPUT:
            target->input = req->param.input;
            /* When input switches, spatial effect may be forced to NONE if input is digital */
            target->effect = z906_cmd_validate_effect(req->param.input, target->effect);
            break;

        case VERIFY_CMD_EFFECT:
            /* Validate if spatial effect is permissible on active audio input */
            target->effect = z906_cmd_validate_effect(pre->input, req->param.effect);
            break;

        case VERIFY_CMD_MUTE:
            target->mute = req->param.mute;
            break;

        case VERIFY_CMD_TRIM: {
            uint8_t level = req->param.trim.level;
            if (level > Z906_MAX_VOLUME_LEVEL) {
                level = Z906_MAX_VOLUME_LEVEL;
            }
            switch (req->param.trim.channel) {
                case TRIM_CHANNEL_BASS:
                    target->volume_bass = level;
                    break;
                case TRIM_CHANNEL_CENTER:
                    target->volume_center = level;
                    break;
                case TRIM_CHANNEL_REAR:
                    target->volume_rear = level;
                    break;
                default:
                    break;
            }
            break;
        }

        case VERIFY_CMD_UNVERIFIABLE:
        default:
            break;
    }
}

/**
 * @brief Checks whether the given state matches the expected target for the active command type.
 *
 * @param req Pointer to the command request.
 * @param current Pointer to the state to evaluate.
 * @param target Pointer to the expected target state.
 * @return true if state matches expected target, false otherwise.
 */
static bool is_state_at_target(const cmd_request_t *req, const z906_state_t *current, const z906_state_t *target)
{
    switch (req->type) {
        case VERIFY_CMD_POWER:
            return (current->power == target->power);

        case VERIFY_CMD_VOLUME:
            return (current->volume == target->volume);

        case VERIFY_CMD_INPUT:
            return (current->input == target->input);

        case VERIFY_CMD_EFFECT:
            return (current->effect == target->effect);

        case VERIFY_CMD_MUTE:
            return (current->mute == target->mute);

        case VERIFY_CMD_TRIM:
            switch (req->param.trim.channel) {
                case TRIM_CHANNEL_BASS:
                    return (current->volume_bass == target->volume_bass);
                case TRIM_CHANNEL_CENTER:
                    return (current->volume_center == target->volume_center);
                case TRIM_CHANNEL_REAR:
                    return (current->volume_rear == target->volume_rear);
                default:
                    return false;
            }

        case VERIFY_CMD_UNVERIFIABLE:
        default:
            return false;
    }
}

/**
 * @brief Validates operating preconditions before executing a command.
 *
 * @param req Pointer to the command request.
 * @param pre Pointer to confirmed pre-command state.
 * @param err_reason Output pointer receiving static error explanation string on failure.
 * @return true if preconditions are satisfied, false otherwise.
 */
static bool check_preconditions(const cmd_request_t *req, const z906_state_t *pre, const char **err_reason)
{
    /* 1. DE-15 cable must be physically connected and system operational */
    if (!pre->cable_connected || !pre->is_operational) {
        if (err_reason) *err_reason = "DE-15 cable disconnected or console in FAULT state";
        return false;
    }

    /* 2. Audio control commands (Volume, Input, Effect, Mute, Trim) require the system to be ON */
    if (req->type != VERIFY_CMD_POWER && !pre->power) {
        if (err_reason) *err_reason = "System is in STANDBY mode (power is OFF)";
        return false;
    }

    /* 3. Input range boundaries: valid values are SIX_CHANNEL_DIRECT (1) through AUXILIARY (6) */
    if (req->type == VERIFY_CMD_INPUT) {
        if (req->param.input < Z906_INPUT_SIX_CHANNEL_DIRECT || req->param.input > Z906_INPUT_AUXILIARY) {
            if (err_reason) *err_reason = "Input selection out of valid range (1 - 6)";
            return false;
        }
    }

    /* 4. Effect range boundaries */
    if (req->type == VERIFY_CMD_EFFECT) {
        if (req->param.effect > Z906_EFFECT_NONE) {
            if (err_reason) *err_reason = "Audio effect selection out of valid range";
            return false;
        }
    }

    return true;
}

/**
 * @brief Transmits physical UART bytes to the amplifier corresponding to the command request.
 *
 * Fully compliant with protocol.rst power and audio sequences.
 *
 * @param req Pointer to the command request.
 * @param pre Pointer to pre-command state snapshot.
 * @return true if transmission succeeded, false otherwise.
 */
static bool send_physical_command(const cmd_request_t *req, const z906_state_t *pre)
{
    switch (req->type) {
        case VERIFY_CMD_POWER: {
            if (req->param.power) {
                /* Canonical Power On sequence: 11 11 <effect> 39 38 30 39 */
                uint8_t eff_byte = (uint8_t)z906_cmd_from_effect(pre->effect);
                uint8_t pwr_on_seq[] = {
                    0x11, 0x11,
                    eff_byte,
                    0x39, 0x38, 0x30, 0x39
                };
                if (z906_uart_hal_write_amp(pwr_on_seq, sizeof(pwr_on_seq)) != sizeof(pwr_on_seq)) {
                    return false;
                }
            } else {
                /* Canonical Power Off sequence: 30 37 36 (0x36 saves settings and enters standby) */
                uint8_t pwr_off_seq[] = { 0x30, 0x37, 0x36 };
                if (z906_uart_hal_write_amp(pwr_off_seq, sizeof(pwr_off_seq)) != sizeof(pwr_off_seq)) {
                    return false;
                }
            }

            /* Request refreshed status configuration frame */
            uint8_t status_req = Z906_CMD_READ_STATUS;
            z906_uart_hal_write_amp(&status_req, 1);
            return true;
        }

        case VERIFY_CMD_MUTE: {
            if (req->param.mute) {
                /* Turn volume down to 0 */
                uint8_t cur_vol = pre->volume;
                while (cur_vol > 0) {
                    uint8_t cmd = Z906_CMD_VOLUME_DOWN;
                    if (z906_uart_hal_write_amp(&cmd, 1) != 1) return false;
                    cur_vol--;
                }
            } else {
                /* Unmute: send 0x38, restore volume, send 0x39 */
                uint8_t unmute_start = Z906_CMD_UNMUTE_START;
                if (z906_uart_hal_write_amp(&unmute_start, 1) != 1) return false;
                uint8_t target_vol = pre->volume > 0 ? pre->volume : Z906_DEFAULT_VOLUME_LEVEL;
                for (uint8_t i = 0; i < target_vol; i++) {
                    uint8_t cmd = Z906_CMD_VOLUME_UP;
                    if (z906_uart_hal_write_amp(&cmd, 1) != 1) return false;
                }
                uint8_t unmute_end = Z906_CMD_UNMUTE_END;
                if (z906_uart_hal_write_amp(&unmute_end, 1) != 1) return false;
            }

            /* Request refreshed status configuration frame */
            uint8_t status_req = Z906_CMD_READ_STATUS;
            z906_uart_hal_write_amp(&status_req, 1);
            return true;
        }

        case VERIFY_CMD_VOLUME: {
            uint8_t target_vol = req->param.volume;
            if (target_vol > Z906_MAX_VOLUME_LEVEL) {
                target_vol = Z906_MAX_VOLUME_LEVEL;
            }

            uint8_t cur_vol = pre->volume;
            while (cur_vol < target_vol) {
                uint8_t seq[] = { Z906_CMD_RESET_IDLE, Z906_CMD_VOLUME_UP, Z906_CMD_RESET_IDLE };
                if (z906_uart_hal_write_amp(seq, sizeof(seq)) != sizeof(seq)) return false;
                cur_vol++;
            }
            while (cur_vol > target_vol) {
                uint8_t seq[] = { Z906_CMD_RESET_IDLE, Z906_CMD_VOLUME_DOWN, Z906_CMD_RESET_IDLE };
                if (z906_uart_hal_write_amp(seq, sizeof(seq)) != sizeof(seq)) return false;
                cur_vol--;
            }
            return true;
        }

        case VERIFY_CMD_INPUT: {
            /* Input select command byte */
            uint8_t in_cmd = (uint8_t)z906_cmd_from_input(req->param.input);
            if (z906_uart_hal_write_amp(&in_cmd, 1) != 1) return false;

            /* Request refreshed status configuration frame */
            uint8_t status_req = Z906_CMD_READ_STATUS;
            z906_uart_hal_write_amp(&status_req, 1);
            return true;
        }

        case VERIFY_CMD_EFFECT: {
            z906_effect_t valid = z906_cmd_validate_effect(pre->input, req->param.effect);
            uint8_t eff_cmd = (uint8_t)z906_cmd_from_effect(valid);
            if (z906_uart_hal_write_amp(&eff_cmd, 1) != 1) return false;

            /* Request refreshed status configuration frame */
            uint8_t status_req = Z906_CMD_READ_STATUS;
            z906_uart_hal_write_amp(&status_req, 1);
            return true;
        }

        case VERIFY_CMD_TRIM: {
            uint8_t target_lvl = req->param.trim.level;
            if (target_lvl > Z906_MAX_VOLUME_LEVEL) target_lvl = Z906_MAX_VOLUME_LEVEL;

            uint8_t cur_lvl = 0;
            z906_cmd_header_t up_cmd, down_cmd;

            switch (req->param.trim.channel) {
                case TRIM_CHANNEL_BASS:
                    cur_lvl = pre->volume_bass;
                    up_cmd = Z906_CMD_VOLUME_BASS_UP;
                    down_cmd = Z906_CMD_VOLUME_BASS_DOWN;
                    break;
                case TRIM_CHANNEL_CENTER:
                    cur_lvl = pre->volume_center;
                    up_cmd = Z906_CMD_VOLUME_CENTER_UP;
                    down_cmd = Z906_CMD_VOLUME_CENTER_DOWN;
                    break;
                case TRIM_CHANNEL_REAR:
                    cur_lvl = pre->volume_rear;
                    up_cmd = Z906_CMD_VOLUME_REAR_UP;
                    down_cmd = Z906_CMD_VOLUME_REAR_DOWN;
                    break;
                default:
                    return false;
            }

            while (cur_lvl < target_lvl) {
                uint8_t seq[] = { Z906_CMD_RESET_IDLE, (uint8_t)up_cmd, Z906_CMD_RESET_IDLE };
                if (z906_uart_hal_write_amp(seq, sizeof(seq)) != sizeof(seq)) return false;
                cur_lvl++;
            }
            while (cur_lvl > target_lvl) {
                uint8_t seq[] = { Z906_CMD_RESET_IDLE, (uint8_t)down_cmd, Z906_CMD_RESET_IDLE };
                if (z906_uart_hal_write_amp(seq, sizeof(seq)) != sizeof(seq)) return false;
                cur_lvl--;
            }

            uint8_t status_req = Z906_CMD_READ_STATUS;
            z906_uart_hal_write_amp(&status_req, 1);
            return true;
        }

        case VERIFY_CMD_UNVERIFIABLE:
        default:
            return false;
    }
}

/**
 * @brief Logs diagnostic details when a command verification attempt fails.
 *
 * @param info Pointer to verification telemetry bundle.
 * @param reason Human-readable failure explanation string.
 */
static void log_verification_failure(const z906_cmd_verify_info_t *info, const char *reason)
{
    ESP_LOGE(TAG, "=== Command Verification FAILED ===");
    ESP_LOGE(TAG, "  Source:       %s", z906_cmd_verify_source_str(info->request.source));
    ESP_LOGE(TAG, "  Command:      %s", z906_cmd_verify_type_str(info->request.type));
    ESP_LOGE(TAG, "  Result Code:  %s (%d)", z906_cmd_verify_result_str(info->result), (int)info->result);
    ESP_LOGE(TAG, "  Reason:       %s", reason ? reason : "Target state verification mismatch");
    ESP_LOGE(TAG, "  Pre-State:    Pwr=%d, Mute=%d, Vol=%d, In=%d, Eff=%d, Cable=%d",
             info->pre_state.power, info->pre_state.mute, info->pre_state.volume,
             (int)info->pre_state.input, (int)info->pre_state.effect, info->pre_state.cable_connected);
    ESP_LOGE(TAG, "  Target-State: Pwr=%d, Mute=%d, Vol=%d, In=%d, Eff=%d",
             info->target_state.power, info->target_state.mute, info->target_state.volume,
             (int)info->target_state.input, (int)info->target_state.effect);
    ESP_LOGE(TAG, "  Post-State:   Pwr=%d, Mute=%d, Vol=%d, In=%d, Eff=%d",
             info->post_state.power, info->post_state.mute, info->post_state.volume,
             (int)info->post_state.input, (int)info->post_state.effect);
    ESP_LOGE(TAG, "  Duration:     %lu ms", (unsigned long)info->duration_ms);
    ESP_LOGE(TAG, "===================================");
}

cmd_verify_result_t z906_cmd_verifier_execute(const cmd_request_t *req, z906_cmd_verify_info_t *out_info)
{
    if (req == NULL) {
        return CMD_VERIFY_ERR_PRE_STATE_FAILED;
    }

    int64_t start_time_us = esp_timer_get_time();

    /* Prepare diagnostic telemetry bundle */
    z906_cmd_verify_info_t info;
    info.request = *req;
    info.duration_ms = 0;
    info.result = CMD_VERIFY_SUCCESS;

    /* 1. Unverifiable command check */
    if (req->type == VERIFY_CMD_UNVERIFIABLE) {
        z906_state_get(&info.pre_state);
        info.target_state = info.pre_state;
        info.post_state = info.pre_state;
        info.result = CMD_VERIFY_ERR_UNVERIFIABLE;
        info.duration_ms = 0;
        log_verification_failure(&info, "Command has no feedback channel on Logitech Z906 hardware");
        if (out_info) *out_info = info;
        return CMD_VERIFY_ERR_UNVERIFIABLE;
    }

    /* 2. Acquire concurrency mutex to prevent overlapping command verifications */
    if (s_verifier_mutex == NULL) {
        z906_cmd_verifier_init();
    }

    if (xSemaphoreTake(s_verifier_mutex, pdMS_TO_TICKS(Z906_VERIFY_MUTEX_WAIT_MS)) != pdTRUE) {
        z906_state_get(&info.pre_state);
        info.target_state = info.pre_state;
        info.post_state = info.pre_state;
        info.result = CMD_VERIFY_ERR_BUSY;
        info.duration_ms = (uint32_t)((esp_timer_get_time() - start_time_us) / 1000);
        log_verification_failure(&info, "Concurrency lock timeout: another verification cycle is active");
        if (out_info) *out_info = info;
        return CMD_VERIFY_ERR_BUSY;
    }

    /* 3. Read confirmed pre-command state */
    z906_state_get(&info.pre_state);

    /* 4. Validate operating preconditions */
    const char *precond_reason = NULL;
    if (!check_preconditions(req, &info.pre_state, &precond_reason)) {
        calculate_expected_target(req, &info.pre_state, &info.target_state);
        info.post_state = info.pre_state;
        info.result = CMD_VERIFY_ERR_PRECONDITION_FAILED;
        info.duration_ms = (uint32_t)((esp_timer_get_time() - start_time_us) / 1000);
        log_verification_failure(&info, precond_reason);
        xSemaphoreGive(s_verifier_mutex);
        if (out_info) *out_info = info;
        return CMD_VERIFY_ERR_PRECONDITION_FAILED;
    }

    /* 5. Determine expected target state */
    calculate_expected_target(req, &info.pre_state, &info.target_state);

    /* 6. Redundant command optimization: if already in target state, avoid redundant UART traffic */
    if (is_state_at_target(req, &info.pre_state, &info.target_state)) {
        info.post_state = info.pre_state;
        info.result = CMD_VERIFY_SUCCESS;
        info.duration_ms = (uint32_t)((esp_timer_get_time() - start_time_us) / 1000);
        xSemaphoreGive(s_verifier_mutex);
        if (out_info) *out_info = info;
        return CMD_VERIFY_SUCCESS;
    }

    /* 7. Dispatch physical bytes via UART to amplifier */
    if (!send_physical_command(req, &info.pre_state)) {
        z906_state_get(&info.post_state);
        info.result = CMD_VERIFY_ERR_SEND_FAILED;
        info.duration_ms = (uint32_t)((esp_timer_get_time() - start_time_us) / 1000);
        log_verification_failure(&info, "Failed to write command bytes to hardware UART port");
        xSemaphoreGive(s_verifier_mutex);
        if (out_info) *out_info = info;
        return CMD_VERIFY_ERR_SEND_FAILED;
    }

    /* Check snapshot of verifier enabled state */
    bool verif_enabled = s_verifier_enabled;
    if (!verif_enabled) {
        /* Verifier is disabled: command was physically dispatched, but post-state
         * verification loop is bypassed. Result is explicitly CMD_VERIFY_SENT_UNVERIFIED.
         * Real physical state will be updated purely when asynchronous telemetry arrives.
         * No optimistic state assumption; no green blink confirmation. */
        z906_state_get(&info.post_state);
        info.result = CMD_VERIFY_SENT_UNVERIFIED;
        info.duration_ms = (uint32_t)((esp_timer_get_time() - start_time_us) / 1000);
        ESP_LOGI(TAG, "Command dispatched physically over UART in Unverified Mode (Latency: %lu ms)",
                 (unsigned long)info.duration_ms);
        xSemaphoreGive(s_verifier_mutex);
        if (out_info) *out_info = info;
        return CMD_VERIFY_SENT_UNVERIFIED;
    }

    /* 8. Wait for confirmed telemetry state update from amplifier (up to Z906_VERIFY_TIMEOUT_MS) */
    uint32_t elapsed_ms = 0;
    bool verified = false;

    while (elapsed_ms < Z906_VERIFY_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(Z906_VERIFY_POLL_INTERVAL_MS));
        elapsed_ms += Z906_VERIFY_POLL_INTERVAL_MS;

        z906_state_get(&info.post_state);
        if (is_state_at_target(req, &info.post_state, &info.target_state)) {
            verified = true;
            break;
        }
    }

    info.duration_ms = (uint32_t)((esp_timer_get_time() - start_time_us) / 1000);

    /* 9. Final outcome evaluation */
    if (verified) {
        info.result = CMD_VERIFY_SUCCESS;
        led_notification_post_cmd_result(true);
    } else {
        info.result = (elapsed_ms >= Z906_VERIFY_TIMEOUT_MS) ?
                      CMD_VERIFY_ERR_TIMEOUT : CMD_VERIFY_ERR_TARGET_NOT_REACHED;
        log_verification_failure(&info, "Timeout expired without receiving confirmed target state");
        led_notification_post_cmd_result(false);
    }

    /* Release concurrency lock */
    xSemaphoreGive(s_verifier_mutex);

    if (out_info) {
        *out_info = info;
    }

    return info.result;
}

bool z906_cmd_verifier_is_busy(void)
{
    if (s_verifier_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_verifier_mutex, 0) == pdTRUE) {
        xSemaphoreGive(s_verifier_mutex);
        return false;
    }
    return true;
}

bool z906_cmd_verifier_try_lock(void)
{
    if (s_verifier_mutex == NULL) {
        z906_cmd_verifier_init();
    }
    return (xSemaphoreTake(s_verifier_mutex, pdMS_TO_TICKS(50)) == pdTRUE);
}

void z906_cmd_verifier_unlock(void)
{
    if (s_verifier_mutex != NULL) {
        xSemaphoreGive(s_verifier_mutex);
    }
}

bool z906_cmd_verifier_is_enabled(void)
{
    return s_verifier_enabled;
}

bool z906_cmd_verifier_set_enabled(bool enabled)
{
    if (s_verifier_enabled == enabled) {
        return true;
    }

    if (s_verifier_mutex == NULL) {
        z906_cmd_verifier_init();
    }

    /* Acquire mutex to safely update the enabled flag */
    if (xSemaphoreTake(s_verifier_mutex, pdMS_TO_TICKS(Z906_VERIFY_MUTEX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire verifier mutex to update enabled flag");
        return false;
    }

    s_verifier_enabled = enabled;
    nvs_manager_set_cmd_verifier_enabled(enabled);
    xSemaphoreGive(s_verifier_mutex);

    ESP_LOGI(TAG, "Command Verifier runtime state updated: %s", enabled ? "ENABLED" : "DISABLED");
    return true;
}

static const z906_cmd_verifier_service_t s_verifier_service = {
    .execute = z906_cmd_verifier_execute,
    .is_busy = z906_cmd_verifier_is_busy,
    .try_lock = z906_cmd_verifier_try_lock,
    .unlock = z906_cmd_verifier_unlock
};

const z906_cmd_verifier_service_t *z906_cmd_verifier_get_service(void)
{
    return &s_verifier_service;
}

