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
/* First byte's UART task read time, not the later FSM queue drain time. */
static int64_t s_amp_frame_started_us = 0;
static uint32_t s_fsm_cable_epoch = 0;
static uint32_t s_fsm_rx_stream_epoch = 0;
/* After RX stream loss, ignore payload-looking standalone opcodes until AA. */
static bool s_amp_resync_required = false;

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
    s_fsm_rx_stream_epoch = z906_state_get_amp_rx_stream_epoch();
    s_amp_resync_required = false;
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

static void handle_amp_byte(uint8_t byte, int64_t received_at_us,
                            uint32_t cable_epoch, uint32_t rx_stream_epoch)
{
    /* 1. Forward raw byte to physical console */
    z906_uart_hal_write_console(&byte, 1);

    /* 2. Use the first byte's UART task read time instead of FSM handling time.
     * ESP-IDF UART ring buffers do not supply hardware byte-arrival timestamps. */
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
                /* The complete, LRC/semantics-checked CONFIG is committed
                 * in one state-mutex transaction. A concurrent cable transition
                 * rejects all fields, not merely the final validity bit. */
                z906_state_t confirmed = { 0 };
                confirmed.volume = pkt.data.config.volume;
                confirmed.volume_bass = pkt.data.config.volume_bass;
                confirmed.volume_center = pkt.data.config.volume_center;
                confirmed.volume_rear = pkt.data.config.volume_rear;
                confirmed.input = pkt.data.config.input;
                confirmed.effect = pkt.data.config.effect;
                confirmed.mute = pkt.data.config.mute;
                confirmed.digital_signal_status = pkt.data.config.digital_signal_status;
                confirmed.auto_standby_enabled = pkt.data.config.auto_standby_enabled;
                /* The amplifier's Byte 17 defines true physical power. */
                confirmed.power = !pkt.data.config.standby;
                if (!z906_state_apply_verified_config_for_epoch(cable_epoch, rx_stream_epoch, &confirmed)) {
                    ESP_LOGW(TAG, "Discarding CONFIG from stale/unplugged cable epoch");
                    break;
                }
                ESP_LOGI(TAG, "Confirmed Amp Config: Vol=%d, Input=%d, Effect=%d, Mute=%d, Standby=%d",
                         pkt.data.config.volume, pkt.data.config.input, pkt.data.config.effect,
                         pkt.data.config.mute, pkt.data.config.standby);
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
                if (z906_state_apply_temperature_for_generations(
                        cable_epoch, rx_stream_epoch, pkt.data.temp.temp_celsius)) {
                    telemetry_notify_response_received(TELEMETRY_QUERY_TEMP,
                                                       s_amp_frame_started_us);
                }
                break;

            case Z906_PACKET_IDLE:
                ESP_LOGD(TAG, "Confirmed Amp Idle Duration: %lu s", (unsigned long)pkt.data.idle.idle_seconds);
                if (z906_state_apply_idle_for_generations(
                        cable_epoch, rx_stream_epoch, pkt.data.idle.idle_seconds)) {
                    telemetry_notify_response_received(TELEMETRY_QUERY_IDLE,
                                                       s_amp_frame_started_us);
                }
                break;

            case Z906_PACKET_INPUT_VOL:
                ESP_LOGD(TAG, "Confirmed Amp Input Signal Level: %lu", (unsigned long)pkt.data.input_vol.input_volume);
                if (z906_state_apply_input_level_for_generations(
                        cable_epoch, rx_stream_epoch, pkt.data.input_vol.input_volume)) {
                    telemetry_notify_response_received(TELEMETRY_QUERY_INPUT_VOL,
                                                       s_amp_frame_started_us);
                }
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

    /* 3. Both epoch validation and any standalone echo state change occur
     * under ONE state mutex transaction, after physical raw passthrough. */
    if (!z906_state_apply_amp_echo_for_generations(cable_epoch, rx_stream_epoch,
                                                   byte)) {
        return;
    }
    switch ((z906_cmd_header_t)byte) {
        case Z906_CMD_STANDBY_SAVE: /* 0x36: Saves settings & transitions to standby */
            fsm_set_state(FSM_STATE_STANDBY);
            break;

        case Z906_CMD_AMP_MUTED: /* 0x20: Amplifier reports muted status */
            fsm_set_state(FSM_STATE_MUTED);
            break;

        case Z906_CMD_AMP_UNMUTED: /* 0x21: Amplifier reports unmuted status */
            fsm_set_state(FSM_STATE_ACTIVE);
            break;

        default:
            /* Other accepted echoes were already applied by the state store. */
            if (byte >= Z906_DECODE_5_1 && byte <= Z906_DECODE_UNKNOWN) {
                ESP_LOGI(TAG, "Amplifier decode state: %s (0x%02X)",
                         z906_cmd_decode_state_str(byte), byte);
            }
            break;
    }
}

static void fsm_sync_rx_generations(void)
{
    uint32_t cable = z906_state_get_cable_epoch();
    uint32_t stream = z906_state_get_amp_rx_stream_epoch();
    if (cable != s_fsm_cable_epoch || stream != s_fsm_rx_stream_epoch) {
        /* Shared state, not lossy EventBus delivery, owns invalidation.
         * An old queued CONFIG cannot resurrect status after a final drop. */
        z906_codec_init(&s_amp_codec_ctx);
        s_amp_frame_started_us = 0;
        s_amp_resync_required = true;
        s_fsm_cable_epoch = cable;
        s_fsm_rx_stream_epoch = stream;
    }
}

void z906_fsm_step(TickType_t block_time)
{
    z906_event_t event;
    bool received = event_bus_receive(&event, block_time);
    fsm_sync_rx_generations();
    if (!received) {
        return;
    }

    switch (event.type) {
        case EVENT_CONSOLE_RX_BYTE:
            handle_console_byte(event.data.raw_byte);
            break;

        case EVENT_AMP_RX_BYTE:
            if (event.cable_epoch != s_fsm_cable_epoch ||
                event.rx_stream_epoch != s_fsm_rx_stream_epoch) {
                /* Passthrough byte is preserved, but never decoded as new state. */
                z906_uart_hal_write_console(&event.data.raw_byte, 1);
                break;
            }
             if (s_amp_resync_required && event.data.raw_byte != 0xAA) {
                /* Preserve raw amplifier-to-console passthrough but never
                 * interpret an orphaned frame payload as a standalone echo. */
                z906_uart_hal_write_console(&event.data.raw_byte, 1);
                break;
            }
            if (s_amp_resync_required) {
                s_amp_resync_required = false;
            }
            handle_amp_byte(event.data.raw_byte, event.received_at_us,
                            event.cable_epoch, event.rx_stream_epoch);
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
