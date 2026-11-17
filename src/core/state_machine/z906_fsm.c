/**
 * @file z906_fsm.c
 * @brief Implementation of the Z906 Finite State Machine and stream decoding coordination.
 *
 * Fully compliant with the canonical reverse engineering reference documentation
 * (nomis/logitech-z906): protocol.rst, interface.rst, firmware.rst, README.rst.
 *
 * All control commands are routed exclusively through z906_cmd_verifier_execute()
 * to enforce verified physical state transitions.
 */

#include "z906_fsm.h"
#include "config.h"
#include "event_bus.h"
#include "service_locator.h"
#include "uart_hal.h"
#include "adc_hal.h"
#include "z906_state.h"
#include "z906_codec.h"
#include "z906_commands.h"
#include "telemetry.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "Z906_FSM";

static z906_fsm_state_t s_fsm_state = FSM_STATE_UNINITIALIZED;
static z906_codec_ctx_t s_amp_codec_ctx;
/* First byte time, captured at UART ingress, not the later FSM queue drain. */
static int64_t s_amp_frame_started_us = 0;
static uint32_t s_fsm_cable_epoch = 0;
static uint32_t s_fsm_rx_stream_epoch = 0;

static void fsm_set_state(z906_fsm_state_t new_state)
{
    if (s_fsm_state != new_state) {
        ESP_LOGI(TAG, "State transition: %s -> %s",
                 z906_fsm_state_str(s_fsm_state), z906_fsm_state_str(new_state));
        s_fsm_state = new_state;

        /* Broadcast state change event to notification subscribers */
        z906_event_t evt = { .type = EVENT_STATE_CHANGED };
        event_bus_post(&evt, 0);
    }
}

bool z906_fsm_init(void)
{
    z906_codec_init(&s_amp_codec_ctx);
    s_amp_frame_started_us = 0;
    s_fsm_cable_epoch = z906_state_get_cable_epoch();
    s_fsm_rx_stream_epoch = 0;
    fsm_set_state(FSM_STATE_BOOTING);
    return true;
}

z906_fsm_state_t z906_fsm_get_state(void)
{
    return s_fsm_state;
}

const char *z906_fsm_state_str(z906_fsm_state_t state)
{
    switch (state) {
        case FSM_STATE_UNINITIALIZED: return "UNINITIALIZED";
        case FSM_STATE_BOOTING:       return "BOOTING";
        case FSM_STATE_STANDBY:       return "STANDBY";
        case FSM_STATE_ACTIVE:        return "ACTIVE";
        case FSM_STATE_MUTED:         return "MUTED";
        case FSM_STATE_SPEAKER_TEST:  return "SPEAKER_TEST";
        case FSM_STATE_FAULT:         return "FAULT";
        default:                      return "UNKNOWN";
    }
}

static void handle_console_byte(uint8_t byte)
{
    /* Forward directly to physical amplifier */
    z906_uart_hal_write_amp(&byte, 1);
}

static void handle_amp_byte(uint8_t byte, int64_t received_at_us, uint32_t cable_epoch)
{
    /* 1. Forward raw byte to physical console */
    z906_uart_hal_write_console(&byte, 1);

    /* 2. A queued old frame cannot be given the time it was processed.
     * Anchor complete CONFIG/telemetry replies to their first byte at RX. */
    if (!z906_codec_is_frame_in_progress(&s_amp_codec_ctx) && byte == 0xAA) {
        s_amp_frame_started_us = received_at_us;
    }
    z906_decoded_packet_t pkt;
    z906_parse_result_t parse_res = z906_codec_feed_byte(&s_amp_codec_ctx, byte, &pkt);
    if (parse_res == Z906_PARSE_RESULT_RESYNC_DROP) {
        s_amp_frame_started_us = 0;
    }

    if (parse_res == Z906_PARSE_RESULT_FRAME_COMPLETE) {
        switch (pkt.type) {
            case Z906_PACKET_CONFIG: {
                z906_state_t cable_state;
                z906_state_get(&cable_state);
                if (!cable_state.cable_connected ||
                    z906_state_get_cable_epoch() != cable_epoch) {
                    /* Preserve raw passthrough; discard stale physical state. */
                    ESP_LOGW(TAG, "Discarding CONFIG from stale/unplugged cable epoch");
                    break;
                }
                ESP_LOGI(TAG, "Confirmed Amp Config: Vol=%d, Input=%d, Effect=%d, Mute=%d, Standby=%d",
                         pkt.data.config.volume, pkt.data.config.input, pkt.data.config.effect,
                         pkt.data.config.mute, pkt.data.config.standby);
                z906_state_set_volume(pkt.data.config.volume);
                z906_state_set_trim(0, pkt.data.config.volume_bass);
                z906_state_set_trim(1, pkt.data.config.volume_center);
                z906_state_set_trim(2, pkt.data.config.volume_rear);
                z906_state_set_input(pkt.data.config.input);
                z906_state_set_effect(pkt.data.config.effect);
                z906_state_set_mute(pkt.data.config.mute);
                z906_state_set_digital_signal_status(pkt.data.config.digital_signal_status);
                z906_state_set_auto_standby(pkt.data.config.auto_standby_enabled);

                /* Set confirmed physical power state directly from Byte 17 (0 = ON, 1 = Standby) */
                z906_state_set_power(!pkt.data.config.standby);
                if (!z906_state_confirm_main_for_epoch(cable_epoch)) {
                    /* Cable changed during CONFIG state updates. No OTA proof. */
                    break;
                }
                if (pkt.data.config.standby) {
                    fsm_set_state(FSM_STATE_STANDBY);
                } else if (pkt.data.config.mute) {
                    fsm_set_state(FSM_STATE_MUTED);
                } else {
                    fsm_set_state(FSM_STATE_ACTIVE);
                }
                telemetry_notify_response_received(TELEMETRY_QUERY_STATUS, s_amp_frame_started_us);
                break;
            }

            case Z906_PACKET_TEMP:
                if (!pkt.data.temp.is_valid) {
                    ESP_LOGW(TAG, "Temperature packet decoded but is_valid=false — discarding");
                    break;
                }
                ESP_LOGI(TAG, "Confirmed Amp Temperatures: [%d, %d, %d] \xc2\xb0""C",
                         pkt.data.temp.temp_celsius[0], pkt.data.temp.temp_celsius[1], pkt.data.temp.temp_celsius[2]);
                z906_state_set_temperature(pkt.data.temp.temp_celsius);
                telemetry_notify_response_received(TELEMETRY_QUERY_TEMP, s_amp_frame_started_us);
                break;

            case Z906_PACKET_IDLE:
                ESP_LOGD(TAG, "Confirmed Amp Idle Duration: %lu s", (unsigned long)pkt.data.idle.idle_seconds);
                z906_state_set_idle_seconds(pkt.data.idle.idle_seconds);
                telemetry_notify_response_received(TELEMETRY_QUERY_IDLE, s_amp_frame_started_us);
                break;

            case Z906_PACKET_INPUT_VOL:
                ESP_LOGD(TAG, "Confirmed Amp Input Signal Level: %lu", (unsigned long)pkt.data.input_vol.input_volume);
                z906_state_set_input_signal_level(pkt.data.input_vol.input_volume);
                telemetry_notify_response_received(TELEMETRY_QUERY_INPUT_VOL, s_amp_frame_started_us);
                break;

            default:
                break;
        }
        return;
    }

    /* If the byte was consumed as part of an in-flight multi-byte frame, or was dropped
     * during a malformed frame resynchronization, DO NOT dispatch to single-byte handlers!
     * This eliminates any risk of payload opcode collision (e.g. 0x20 mute, 0x36 standby). */
    if (parse_res == Z906_PARSE_RESULT_IN_PROGRESS || parse_res == Z906_PARSE_RESULT_RESYNC_DROP) {
        return;
    }

    /* 3. Process standalone single-byte telemetry confirmations & physical button echoes per protocol.rst */
    switch ((z906_cmd_header_t)byte) {
        case Z906_CMD_STANDBY_SAVE: /* 0x36: Saves settings & transitions to standby */
            z906_state_set_power(false);
            fsm_set_state(FSM_STATE_STANDBY);
            break;

        case Z906_CMD_AMP_MUTED: /* 0x20: Amplifier reports muted status */
            z906_state_set_mute(true);
            fsm_set_state(FSM_STATE_MUTED);
            break;

        case Z906_CMD_AMP_UNMUTED: /* 0x21: Amplifier reports unmuted status */
            z906_state_set_mute(false);
            fsm_set_state(FSM_STATE_ACTIVE);
            break;

        /* Physical Master Volume Up / Down Echoes */
        case Z906_CMD_VOLUME_UP: {
            z906_state_t cur;
            z906_state_get(&cur);
            z906_state_set_volume(cur.volume + 1);
            break;
        }
        case Z906_CMD_VOLUME_DOWN: {
            z906_state_t cur;
            z906_state_get(&cur);
            if (cur.volume > 0) z906_state_set_volume(cur.volume - 1);
            break;
        }

        /* Physical Channel Trim Volume Echoes */
        case Z906_CMD_VOLUME_BASS_UP: {
            z906_state_t cur;
            z906_state_get(&cur);
            z906_state_set_trim(0, cur.volume_bass + 1);
            break;
        }
        case Z906_CMD_VOLUME_BASS_DOWN: {
            z906_state_t cur;
            z906_state_get(&cur);
            if (cur.volume_bass > 0) z906_state_set_trim(0, cur.volume_bass - 1);
            break;
        }
        case Z906_CMD_VOLUME_CENTER_UP: {
            z906_state_t cur;
            z906_state_get(&cur);
            z906_state_set_trim(1, cur.volume_center + 1);
            break;
        }
        case Z906_CMD_VOLUME_CENTER_DOWN: {
            z906_state_t cur;
            z906_state_get(&cur);
            if (cur.volume_center > 0) z906_state_set_trim(1, cur.volume_center - 1);
            break;
        }
        case Z906_CMD_VOLUME_REAR_UP: {
            z906_state_t cur;
            z906_state_get(&cur);
            z906_state_set_trim(2, cur.volume_rear + 1);
            break;
        }
        case Z906_CMD_VOLUME_REAR_DOWN: {
            z906_state_t cur;
            z906_state_get(&cur);
            if (cur.volume_rear > 0) z906_state_set_trim(2, cur.volume_rear - 1);
            break;
        }

        /* Physical Input Selection Echoes (protocol.rst Section: Input selection) */
        case Z906_CMD_SELECT_INPUT_1: z906_state_set_input(Z906_INPUT_SIX_CHANNEL_DIRECT); break;
        case Z906_CMD_SELECT_INPUT_2: z906_state_set_input(Z906_INPUT_RCA_STEREO);          break;
        case Z906_CMD_SELECT_INPUT_3: z906_state_set_input(Z906_INPUT_OPTICAL_1);           break;
        case Z906_CMD_SELECT_INPUT_4: z906_state_set_input(Z906_INPUT_OPTICAL_2);           break;
        case Z906_CMD_SELECT_INPUT_5: z906_state_set_input(Z906_INPUT_DIGITAL_COAXIAL);     break;
        case Z906_CMD_SELECT_INPUT_6: z906_state_set_input(Z906_INPUT_AUXILIARY);           break;

        /* Physical Effect Selection Echoes (protocol.rst Section: Effect selection) */
        case Z906_CMD_SELECT_EFFECT_3D:   z906_state_set_effect(Z906_EFFECT_3D); break;
        case Z906_CMD_SELECT_EFFECT_2_1:  z906_state_set_effect(Z906_EFFECT_2_1); break;
        case Z906_CMD_SELECT_EFFECT_4_1:  z906_state_set_effect(Z906_EFFECT_4_1); break;
        case Z906_CMD_SELECT_EFFECT_NONE: z906_state_set_effect(Z906_EFFECT_NONE); break;

        default:
            /* Decode state check (0x17 to 0x1F, protocol.rst Section: Decode state) */
            if (byte >= Z906_DECODE_5_1 && byte <= Z906_DECODE_UNKNOWN) {
                ESP_LOGI(TAG, "Amplifier decode state: %s (0x%02X)",
                         z906_cmd_decode_state_str(byte), byte);
                z906_state_set_digital_signal_status(byte);
            }
            break;
    }
}

static void fsm_sync_cable_epoch(void)
{
    uint32_t current = z906_state_get_cable_epoch();
    if (current != s_fsm_cable_epoch) {
        /* Direct state generation, never a lossy EventBus cable notification. */
        z906_codec_init(&s_amp_codec_ctx);
        s_amp_frame_started_us = 0;
        s_fsm_cable_epoch = current;
    }
}

void z906_fsm_step(TickType_t block_time)
{
    z906_event_t event;
    bool received = event_bus_receive(&event, block_time);
    fsm_sync_cable_epoch();
    if (!received) {
        return;
    }

    switch (event.type) {
        case EVENT_CONSOLE_RX_BYTE:
            handle_console_byte(event.data.raw_byte);
            break;

        case EVENT_AMP_RX_BYTE:
            if (event.cable_epoch != s_fsm_cable_epoch) {
                /* Passthrough byte is preserved, but never decoded as new state. */
                z906_uart_hal_write_console(&event.data.raw_byte, 1);
                break;
            }
            if (event.rx_stream_epoch != s_fsm_rx_stream_epoch) {
                /* The UART-to-EventBus bridge dropped a byte or flushed RX. */
                z906_codec_init(&s_amp_codec_ctx);
                s_amp_frame_started_us = 0;
                s_fsm_rx_stream_epoch = event.rx_stream_epoch;
            }
            handle_amp_byte(event.data.raw_byte, event.received_at_us,
                            event.cable_epoch);
            break;

        case EVENT_CABLE_STATUS_CHANGED: {
            /* Advisory only: a stale queued cable event must not reset a valid
             * newer frame or override the current physical connection state. */
            z906_state_t physical;
            z906_state_get(&physical);
            if (event.data.cable_connected != physical.cable_connected) {
                break;
            }
            if (!physical.cable_connected) {
                fsm_set_state(FSM_STATE_FAULT);
            } else if (s_fsm_state == FSM_STATE_FAULT) {
                fsm_set_state(FSM_STATE_STANDBY);
            }
            break;
        }

        default:
            break;
    }
}

/* =========================================================================
 * FSM Task — owned by core/state_machine (z906_fsm.c)
 *
 * Ownership rationale: The FSM task is the runtime of the state machine. Its
 * lifecycle (create/start/run) belongs in the same module as the FSM logic
 * itself. No boot orchestration logic lives here.
 * ========================================================================= */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "wdt_manager.h"
/* NOTE: zigbee_state_publisher.h is intentionally NOT included here.
 * State changes drive the observer registered in z906_state via
 * z906_state_register_change_observer() — no direct core→communication
 * dependency is needed or allowed. (FSR-06 fix) */

/** @brief Stack depth in words for the FSM task */
#define FSM_TASK_STACK_WORDS    (4096u / sizeof(StackType_t))

/** @brief FreeRTOS priority for the FSM task */
#define FSM_TASK_PRIORITY       4u

/** @brief Max ms to wait for the task to signal STARTED before declaring timeout */
#define FSM_STARTUP_TIMEOUT_MS  500u

static StackType_t  s_fsm_stack[FSM_TASK_STACK_WORDS];
static StaticTask_t s_fsm_tcb;

/** @brief Private startup binary semaphore — given by task once it enters main loop */
static SemaphoreHandle_t s_started_sem    = NULL;
static StaticSemaphore_t s_started_sem_buf;

/**
 * @brief FSM task body.
 *
 * No EventGroup gate — startup is handled by the private semaphore handshake
 * in z906_fsm_start(). The task signals STARTED immediately before
 * entering its main loop.
 *
 * @param pvParameters Unused.
 */
static void prv_fsm_task(void *pvParameters)
{
    (void)pvParameters; /* No gate handle — startup handled by private semaphore */

    wdt_manager_register_task();
    ESP_LOGI(TAG, "z906_fsm_task running (Priority %u)", FSM_TASK_PRIORITY);

    /* Signal z906_fsm_start() that this task is alive and entering its loop */
    xSemaphoreGive(s_started_sem);

    while (1) {
        wdt_manager_reset();
        /* Consume one event from the EventBus and run state transition logic.
         * State changes are propagated to the Zigbee publisher via the observer
         * callback registered in z906_state (z906_state_register_change_observer).
         * The FSM does NOT call the publisher directly. */
        z906_fsm_step(pdMS_TO_TICKS(50));
    }
}

esp_err_t z906_fsm_start(void)
{
    /* Create the binary startup semaphore — starts empty (task must give it) */
    s_started_sem = xSemaphoreCreateBinaryStatic(&s_started_sem_buf);

    TaskHandle_t handle = xTaskCreateStatic(
        prv_fsm_task,
        "z906_fsm_task",
        FSM_TASK_STACK_WORDS,
        NULL,              /* pvParameters: no gate handle needed */
        FSM_TASK_PRIORITY,
        s_fsm_stack,
        &s_fsm_tcb
    );

    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to create z906_fsm_task");
        return ESP_ERR_NO_MEM;
    }

    /* Block until task signals STARTED or timeout */
    BaseType_t got = xSemaphoreTake(s_started_sem,
                                    pdMS_TO_TICKS(FSM_STARTUP_TIMEOUT_MS));
    if (got != pdTRUE) {
        ESP_LOGE(TAG, "z906_fsm_task did not signal STARTED within %u ms",
                 FSM_STARTUP_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "z906_fsm_task STARTED (Priority %u, stack %u words)",
             FSM_TASK_PRIORITY, FSM_TASK_STACK_WORDS);
    return ESP_OK;
}
