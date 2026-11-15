/**
 * @file zigbee_command_ingress.c
 * @brief Implementation of non-blocking ZCL command ingress, bounded queuing, and verified dispatch.
 *
 * Invariants & Correctness Guarantees (Phase 4 Corrective Hardening):
 *   1. Zero Optimistic State: Incoming writes or commands NEVER mutate authoritative state (z906_state).
 *   2. Non-blocking Callbacks: Inside SDK callbacks, only validation and non-blocking xQueueSend(..., 0)
 *      are performed. Zero UART I/O, zero mutex wait, zero dynamic memory allocation.
 *   3. ZCL Standard Level Control Semantics:
 *      - Plain MoveToLevel (0x00): Parses level, transition_time, options_mask, options_override.
 *        If system is currently OFF (Standby), inspects ExecuteIfOff bit (bit 0 of options).
 *        If ExecuteIfOff is 0, the command has NO EFFECT and returns SUCCESS without powering on or altering volume.
 *        If ExecuteIfOff is 1, updates volume while leaving power state untouched.
 *      - MoveToLevelWithOnOff (0x04): Compound atomic operation.
 *        If target volume > 0: Ensures Power ON is verified first (if currently in Standby), then verifies Volume.
 *        If target volume == 0: Verifies Volume 0, then enters Standby (Power OFF) per ZCL spec.
 *      - Continuous / Relative commands (Move, Step, Stop) and extended frequency commands are rejected with UNSUP_CMD.
 *      - Transition time is safely parsed but Z906 hardware executes discrete volume steps without smooth fade.
 *   4. Extended OnOff Command Rejection:
 *      - Basic On/Off/Toggle supported.
 *      - OffWithEffect (0x40), OnWithRecallGlobalScene (0x41), OnWithTimedOff (0x42) explicitly rejected with UNSUP_CMD.
 *   5. Default Response Correctness:
 *      - Checked against disableDefaultResponse bit in Frame Control.
 *      - Handled frames return true to drop from stack; unhandled return false to pass through.
 *      - Default response contains accurate TSN, command ID, endpoint, cluster, and address correlation.
 *   6. Guaranteed Capacity & Single-Producer Concurrency Invariant:
 *      - All queue producers execute serially in the same Zigbee stack task context (via callbacks).
 *      - No other task or ISR in the system ever enqueues to s_ingress_queue.
 *      - While FreeRTOS scheduler preemption may occur between check_value_cb and write_attr_cb,
 *        only the consumer task (zb_cmd_worker) or unrelated tasks can execute.
 *      - The consumer task can ONLY remove items from the queue (increasing available capacity).
 *      - Therefore, available queue capacity cannot decrease between check_value_cb and write_attr_cb,
 *        formally guaranteeing that write_attr_cb will always find sufficient space.
 *   7. Optimistic Memory Restoration:
 *      - Any backing memory updated by SDK during Write Attributes is immediately reverted to verified
 *        physical state via zigbee_device_model_restore_attr().
 */

#include "zigbee_command_ingress.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "zigbee_device_model.h"
#include "zigbee_state_publisher.h"
#include "z906_cmd_verifier.h"
#include "z906_cmd_registry.h"
#include "z906_commands.h"
#include "z906_state.h"
#include "led_notification_manager.h"
#include "clusters/zcl_clusters.h"

#include <ezbee/af.h>
#include <ezbee/zcl/zcl_type.h>
#include <ezbee/zcl/zcl_desc.h>
#include <ezbee/zcl/zcl_common.h>
#include <ezbee/zcl/zcl_core.h>
#include <ezbee/zcl/zcl_general_cmd.h>
#include <ezbee/zcl/cluster/on_off_desc.h>
#include <ezbee/zcl/cluster/level.h>
#include <ezbee/zcl/cluster/level_desc.h>
#include <ezbee/zcl/cluster/custom.h>

/* ========================================================================= */
/* Compile-Time Static Assertions: Canonical ZCL Server Command IDs          */
/* Validated against esp-zigbee-lib 2.0.4 headers (level_desc.h, on_off_desc.h)*/
/* ========================================================================= */

_Static_assert(EZB_ZCL_CMD_LEVEL_MOVE_TO_LEVEL_ID == 0x00U,
               "ZCL Command ID drift: MoveToLevel MUST be 0x00");
_Static_assert(EZB_ZCL_CMD_LEVEL_MOVE_ID == 0x01U,
               "ZCL Command ID drift: Move MUST be 0x01");
_Static_assert(EZB_ZCL_CMD_LEVEL_STEP_ID == 0x02U,
               "ZCL Command ID drift: Step MUST be 0x02");
_Static_assert(EZB_ZCL_CMD_LEVEL_STOP_ID == 0x03U,
               "ZCL Command ID drift: Stop MUST be 0x03");
_Static_assert(EZB_ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF_ID == 0x04U,
               "ZCL Command ID drift: MoveToLevelWithOnOff MUST be 0x04");
_Static_assert(EZB_ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF_ID == 0x05U,
               "ZCL Command ID drift: MoveWithOnOff MUST be 0x05");
_Static_assert(EZB_ZCL_CMD_LEVEL_STEP_WITH_ON_OFF_ID == 0x06U,
               "ZCL Command ID drift: StepWithOnOff MUST be 0x06");
_Static_assert(EZB_ZCL_CMD_LEVEL_STOP_WITH_ON_OFF_ID == 0x07U,
               "ZCL Command ID drift: StopWithOnOff MUST be 0x07");
_Static_assert(EZB_ZCL_CMD_LEVEL_MOVE_TO_CLOSEST_FREQUENCY_ID == 0x08U,
               "ZCL Command ID drift: MoveToClosestFrequency MUST be 0x08");

_Static_assert(EZB_ZCL_CMD_ON_OFF_OFF_ID == 0x00U,
               "ZCL Command ID drift: OnOff OFF MUST be 0x00");
_Static_assert(EZB_ZCL_CMD_ON_OFF_ON_ID == 0x01U,
               "ZCL Command ID drift: OnOff ON MUST be 0x01");
_Static_assert(EZB_ZCL_CMD_ON_OFF_TOGGLE_ID == 0x02U,
               "ZCL Command ID drift: OnOff TOGGLE MUST be 0x02");
_Static_assert(EZB_ZCL_CMD_ON_OFF_OFF_WITH_EFFECT_ID == 0x40U,
               "ZCL Command ID drift: OffWithEffect MUST be 0x40");
_Static_assert(EZB_ZCL_CMD_ON_OFF_ON_WITH_RECALL_GLOBAL_SCENE_ID == 0x41U,
               "ZCL Command ID drift: OnWithRecallGlobalScene MUST be 0x41");
_Static_assert(EZB_ZCL_CMD_ON_OFF_ON_WITH_TIMED_OFF_ID == 0x42U,
               "ZCL Command ID drift: OnWithTimedOff MUST be 0x42");

static const char *TAG = "ZB_INGRESS";

/* ========================================================================= */
/* Static FreeRTOS Queue & Worker Task Buffers                               */
/* ========================================================================= */

#define ZB_WORKER_STACK_SIZE 4096

static QueueHandle_t s_ingress_queue = NULL;
static StaticQueue_t s_static_queue_control;
static uint8_t s_queue_storage[ZB_INGRESS_QUEUE_DEPTH * sizeof(zb_ingress_event_t)];

static StackType_t s_worker_stack[ZB_WORKER_STACK_SIZE / sizeof(StackType_t)];
static StaticTask_t s_worker_tcb;
static TaskHandle_t s_worker_task_handle = NULL;
static bool s_ingress_initialized = false;

/* ========================================================================= */
/* Worker Task — Verified Hardware Dispatch                                  */
/* ========================================================================= */

static void zigbee_cmd_worker_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Zigbee command bridge worker task active (Priority: 3)");

    zb_ingress_event_t evt;

    while (1) {
        if (xQueueReceive(s_ingress_queue, &evt, portMAX_DELAY) == pdTRUE) {
            if (evt.type == ZB_INGRESS_TYPE_FEATURE_LED) {
                ESP_LOGI(TAG, "Executing setting change: Onboard LED -> %s",
                         evt.param_val ? "ENABLED" : "DISABLED");
                led_notification_set_enabled(evt.param_val != 0);
                zigbee_state_publisher_notify(ZB_PUB_NOTIF_BUILT_IN_LED);
            } else if (evt.type == ZB_INGRESS_TYPE_FEATURE_VERIFIER) {
                ESP_LOGI(TAG, "Executing setting change: Command Verifier -> %s",
                         evt.param_val ? "ENABLED" : "DISABLED");
                z906_cmd_verifier_set_enabled(evt.param_val != 0);
                zigbee_state_publisher_notify(ZB_PUB_NOTIF_CMD_VERIFIER);
            } else if (evt.type == ZB_INGRESS_TYPE_HARDWARE_CMD) {
                ESP_LOGI(TAG, "Dispatching verified Z906 command from Zigbee: %s (param: %ld, EP: %u, Cluster: 0x%04X)",
                         z906_cmd_registry_id_to_str(evt.cmd_id), (long)evt.param_val, evt.endpoint, evt.cluster);

                cmd_request_t req;
                if (!z906_cmd_registry_build_request(evt.cmd_id, CMD_SOURCE_ZIGBEE, evt.param_val, &req)) {
                    ESP_LOGE(TAG, "Failed to build cmd_request_t for command ID %d", evt.cmd_id);
                    continue;
                }

                z906_cmd_verify_info_t verify_info;
                cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &verify_info);

                if (res == CMD_VERIFY_SUCCESS) {
                    ESP_LOGI(TAG, "Command %s verified successfully (Latency: %lu ms)",
                             z906_cmd_registry_id_to_str(evt.cmd_id), (unsigned long)verify_info.duration_ms);
                } else {
                    ESP_LOGW(TAG, "Command %s verification failed: %s (Code: %d, Latency: %lu ms)",
                             z906_cmd_registry_id_to_str(evt.cmd_id),
                             z906_cmd_verify_result_str(res), (int)res, (unsigned long)verify_info.duration_ms);
                }
            } else if (evt.type == ZB_INGRESS_TYPE_COMPOUND_LEVEL_ONOFF) {
                /*
                 * Atomic compound sequencing for MoveToLevelWithOnOff:
                 * Case 1: Target volume > 0 -> Power ON first if in Standby, then set Volume.
                 * Case 2: Target volume == 0 -> Set Volume 0, then enter Standby (Power OFF).
                 */
                int32_t target_vol = evt.param_val;
                ESP_LOGI(TAG, "Executing compound MoveToLevelWithOnOff: target volume %ld", (long)target_vol);

                z906_state_t st;
                z906_state_get(&st);

                if (target_vol > 0) {
                    /* Step 1: Ensure system is Powered ON */
                    if (!st.power) {
                        ESP_LOGI(TAG, "MoveToLevelWithOnOff: System is in Standby; powering ON first...");
                        cmd_request_t pwr_req;
                        if (z906_cmd_registry_build_request(Z906_CMD_ID_POWER, CMD_SOURCE_ZIGBEE, 1, &pwr_req)) {
                            z906_cmd_verify_info_t pwr_info;
                            cmd_verify_result_t pwr_res = z906_cmd_verifier_execute(&pwr_req, &pwr_info);
                            if (pwr_res != CMD_VERIFY_SUCCESS) {
                                ESP_LOGW(TAG, "MoveToLevelWithOnOff: Power ON verification failed: %s; aborting volume step",
                                         z906_cmd_verify_result_str(pwr_res));
                                continue;
                            }
                        }
                    }

                    /* Step 2: Set target volume */
                    cmd_request_t vol_req;
                    if (z906_cmd_registry_build_request(Z906_CMD_ID_VOLUME, CMD_SOURCE_ZIGBEE, target_vol, &vol_req)) {
                        z906_cmd_verify_info_t vol_info;
                        cmd_verify_result_t vol_res = z906_cmd_verifier_execute(&vol_req, &vol_info);
                        if (vol_res == CMD_VERIFY_SUCCESS) {
                            ESP_LOGI(TAG, "MoveToLevelWithOnOff compound sequence succeeded (Volume: %ld)", (long)target_vol);
                        } else {
                            ESP_LOGW(TAG, "MoveToLevelWithOnOff: Volume verification failed: %s",
                                     z906_cmd_verify_result_str(vol_res));
                        }
                    }
                } else {
                    /* Target volume == 0 */
                    cmd_request_t vol_req;
                    if (z906_cmd_registry_build_request(Z906_CMD_ID_VOLUME, CMD_SOURCE_ZIGBEE, 0, &vol_req)) {
                        z906_cmd_verify_info_t vol_info;
                        z906_cmd_verifier_execute(&vol_req, &vol_info);
                    }

                    /* Turn off (Standby) per ZCL spec when moving to min level */
                    if (st.power) {
                        cmd_request_t pwr_req;
                        if (z906_cmd_registry_build_request(Z906_CMD_ID_POWER, CMD_SOURCE_ZIGBEE, 0, &pwr_req)) {
                            z906_cmd_verify_info_t pwr_info;
                            z906_cmd_verifier_execute(&pwr_req, &pwr_info);
                        }
                    }
                }
            }
        }
    }
}

/* ========================================================================= */
/* Helper: Send ZCL Default Response                                          */
/* ========================================================================= */

static void send_default_response(const ezb_zcl_cmd_hdr_t *hdr, ezb_zcl_status_t status)
{
    if (hdr == NULL) {
        return;
    }

    /*
     * ZCL Specification Default Response Rule:
     * If disableDefaultResponse == 1 AND status == SUCCESS, NO default response is generated.
     * If status != SUCCESS, default response MUST be generated regardless of disableDefaultResponse.
     */
    if (EZB_ZCL_CMD_FC_IS_DIS_DEFAULT_RSP(hdr->fc) && status == EZB_ZCL_STATUS_SUCCESS) {
        return;
    }

    ezb_zcl_default_rsp_cmd_t resp;
    memset(&resp, 0, sizeof(resp));

    resp.cmd_ctrl.dst_addr = hdr->src_addr;
    resp.cmd_ctrl.dst_ep   = hdr->src_ep;
    resp.cmd_ctrl.src_ep   = hdr->dst_ep;
    resp.cmd_ctrl.cluster_id = hdr->cluster_id;
    resp.cmd_ctrl.manuf_code = hdr->manuf_code;
    resp.cmd_ctrl.fc.manuf_specific = EZB_ZCL_CMD_FC_IS_MANUF_SPEC(hdr->fc) ? 1 : 0;
    resp.cmd_ctrl.fc.direction      = EZB_ZCL_CMD_DIRECTION_TO_CLI;
    resp.cmd_ctrl.fc.dis_default_rsp = 1;

    resp.payload.tsn         = hdr->tsn;
    resp.payload.rsp_to_cmd  = hdr->cmd_id;
    resp.payload.status_code = status;

    ezb_err_t err = ezb_zcl_default_rsp_cmd_req(&resp);
    if (err != EZB_ERR_NONE) {
        ESP_LOGD(TAG, "ezb_zcl_default_rsp_cmd_req returned %d", (int)err);
    }
}

/* ========================================================================= */
/* EP1 Standard Clusters Raw Frame Callback                                  */
/* ========================================================================= */

static bool handle_on_off_command(const ezb_zcl_cmd_hdr_t *hdr)
{
    int32_t target_power = -1;

    switch (hdr->cmd_id) {
        case EZB_ZCL_CMD_ON_OFF_OFF_ID:
            target_power = 0;
            break;
        case EZB_ZCL_CMD_ON_OFF_ON_ID:
            target_power = 1;
            break;
        case EZB_ZCL_CMD_ON_OFF_TOGGLE_ID: {
            z906_state_t st;
            z906_state_get(&st);
            target_power = st.power ? 0 : 1;
            break;
        }
        case EZB_ZCL_CMD_ON_OFF_OFF_WITH_EFFECT_ID:
        case EZB_ZCL_CMD_ON_OFF_ON_WITH_RECALL_GLOBAL_SCENE_ID:
        case EZB_ZCL_CMD_ON_OFF_ON_WITH_TIMED_OFF_ID:
            ESP_LOGW(TAG, "Unsupported extended On/Off cluster command ID: 0x%02X", hdr->cmd_id);
            send_default_response(hdr, EZB_ZCL_STATUS_UNSUP_CMD);
            return true;
        default:
            ESP_LOGW(TAG, "Unknown On/Off cluster command ID: 0x%02X", hdr->cmd_id);
            send_default_response(hdr, EZB_ZCL_STATUS_UNSUP_CMD);
            return true;
    }

    if (uxQueueSpacesAvailable(s_ingress_queue) == 0) {
        ESP_LOGW(TAG, "Ingress queue full; rejecting On/Off command");
        send_default_response(hdr, EZB_ZCL_STATUS_INSUFFICIENT_SPACE);
        return true;
    }

    zb_ingress_event_t evt = {
        .type          = ZB_INGRESS_TYPE_HARDWARE_CMD,
        .cmd_id        = Z906_CMD_ID_POWER,
        .param_val     = target_power,
        .secondary_val = 0,
        .endpoint      = hdr->dst_ep,
        .cluster       = hdr->cluster_id,
    };

    if (xQueueSend(s_ingress_queue, &evt, 0) == pdTRUE) {
        send_default_response(hdr, EZB_ZCL_STATUS_SUCCESS);
    } else {
        send_default_response(hdr, EZB_ZCL_STATUS_INSUFFICIENT_SPACE);
    }

    return true;
}

static bool handle_level_control_command(const ezb_zcl_raw_frame_t *raw_frame)
{
    const ezb_zcl_cmd_hdr_t *hdr = raw_frame->header;

    switch (hdr->cmd_id) {
        case EZB_ZCL_CMD_LEVEL_MOVE_TO_LEVEL_ID: {
            /*
             * Payload layout per ZCL spec (ezb_zcl_level_move_to_level_cmd_payload_t):
             * Byte 0:    uint8_t  level (0-254)
             * Byte 1..2: uint16_t transition_time (little endian)
             * Optional Byte 3: uint8_t options_mask
             * Optional Byte 4: uint8_t options_override
             */
            if (raw_frame->payload_length < 3 || raw_frame->payload == NULL) {
                ESP_LOGW(TAG, "Malformed MoveToLevel payload length: %u (expected >= 3)", raw_frame->payload_length);
                send_default_response(hdr, EZB_ZCL_STATUS_MALFORMED_CMD);
                return true;
            }

            uint8_t zcl_level = raw_frame->payload[0];
            /* Transition time parsed safely for standards compliance */
            uint16_t transition_time = (uint16_t)raw_frame->payload[1] | ((uint16_t)raw_frame->payload[2] << 8);
            (void)transition_time;

            /* Parse optional options mask / override if present */
            uint8_t options_mask = 0;
            uint8_t options_override = 0;
            if (raw_frame->payload_length >= 5) {
                options_mask = raw_frame->payload[3];
                options_override = raw_frame->payload[4];
            }

            /*
             * Evaluate ExecuteIfOff semantics:
             * Default Options attribute in Level Cluster is 0x00 (ExecuteIfOff = 0).
             * Effective ExecuteIfOff bit is determined by override if masked, or cluster attribute default (0).
             */
            bool execute_if_off = false;
            if (options_mask & (1 << EZB_ZCL_LEVEL_OPTIONS_EXECUTE_IF_OFF)) {
                execute_if_off = (options_override & (1 << EZB_ZCL_LEVEL_OPTIONS_EXECUTE_IF_OFF)) != 0;
            }

            z906_state_t st;
            z906_state_get(&st);

            /* If device is OFF and ExecuteIfOff is false, command has NO EFFECT per ZCL spec */
            if (!st.power && !execute_if_off) {
                ESP_LOGI(TAG, "MoveToLevel ignored: Device is in Standby and ExecuteIfOff=0 (ZCL spec compliant)");
                send_default_response(hdr, EZB_ZCL_STATUS_SUCCESS);
                return true;
            }

            uint8_t hw_volume = z906_cmd_zcl_to_volume(zcl_level);
            if (!z906_cmd_registry_validate_param(Z906_CMD_ID_VOLUME, hw_volume)) {
                ESP_LOGW(TAG, "MoveToLevel volume out of bounds: %u (ZCL: %u)", hw_volume, zcl_level);
                send_default_response(hdr, EZB_ZCL_STATUS_INVALID_VALUE);
                return true;
            }

            if (uxQueueSpacesAvailable(s_ingress_queue) == 0) {
                ESP_LOGW(TAG, "Ingress queue full; rejecting MoveToLevel command");
                send_default_response(hdr, EZB_ZCL_STATUS_INSUFFICIENT_SPACE);
                return true;
            }

            zb_ingress_event_t evt = {
                .type          = ZB_INGRESS_TYPE_HARDWARE_CMD,
                .cmd_id        = Z906_CMD_ID_VOLUME,
                .param_val     = hw_volume,
                .secondary_val = 0,
                .endpoint      = hdr->dst_ep,
                .cluster       = hdr->cluster_id,
            };

            if (xQueueSend(s_ingress_queue, &evt, 0) == pdTRUE) {
                send_default_response(hdr, EZB_ZCL_STATUS_SUCCESS);
            } else {
                send_default_response(hdr, EZB_ZCL_STATUS_INSUFFICIENT_SPACE);
            }
            return true;
        }

        case EZB_ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF_ID: {
            if (raw_frame->payload_length < 3 || raw_frame->payload == NULL) {
                ESP_LOGW(TAG, "Malformed MoveToLevelWithOnOff payload length: %u (expected >= 3)", raw_frame->payload_length);
                send_default_response(hdr, EZB_ZCL_STATUS_MALFORMED_CMD);
                return true;
            }

            uint8_t zcl_level = raw_frame->payload[0];
            uint16_t transition_time = (uint16_t)raw_frame->payload[1] | ((uint16_t)raw_frame->payload[2] << 8);
            (void)transition_time;

            uint8_t hw_volume = z906_cmd_zcl_to_volume(zcl_level);
            if (!z906_cmd_registry_validate_param(Z906_CMD_ID_VOLUME, hw_volume)) {
                ESP_LOGW(TAG, "MoveToLevelWithOnOff volume out of bounds: %u (ZCL: %u)", hw_volume, zcl_level);
                send_default_response(hdr, EZB_ZCL_STATUS_INVALID_VALUE);
                return true;
            }

            if (uxQueueSpacesAvailable(s_ingress_queue) == 0) {
                ESP_LOGW(TAG, "Ingress queue full; rejecting MoveToLevelWithOnOff command");
                send_default_response(hdr, EZB_ZCL_STATUS_INSUFFICIENT_SPACE);
                return true;
            }

            /* Single compound event guarantees atomic execution in worker without inter-task race */
            zb_ingress_event_t evt = {
                .type          = ZB_INGRESS_TYPE_COMPOUND_LEVEL_ONOFF,
                .cmd_id        = Z906_CMD_ID_VOLUME,
                .param_val     = hw_volume,
                .secondary_val = 0,
                .endpoint      = hdr->dst_ep,
                .cluster       = hdr->cluster_id,
            };

            if (xQueueSend(s_ingress_queue, &evt, 0) == pdTRUE) {
                send_default_response(hdr, EZB_ZCL_STATUS_SUCCESS);
            } else {
                send_default_response(hdr, EZB_ZCL_STATUS_INSUFFICIENT_SPACE);
            }
            return true;
        }

        case EZB_ZCL_CMD_LEVEL_MOVE_ID:
        case EZB_ZCL_CMD_LEVEL_STEP_ID:
        case EZB_ZCL_CMD_LEVEL_STOP_ID:
        case EZB_ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF_ID:
        case EZB_ZCL_CMD_LEVEL_STEP_WITH_ON_OFF_ID:
        case EZB_ZCL_CMD_LEVEL_STOP_WITH_ON_OFF_ID:
        case EZB_ZCL_CMD_LEVEL_MOVE_TO_CLOSEST_FREQUENCY_ID:
        default:
            ESP_LOGW(TAG, "Unsupported continuous/relative Level Control command ID: 0x%02X", hdr->cmd_id);
            send_default_response(hdr, EZB_ZCL_STATUS_UNSUP_CMD);
            return true;
    }
}

static bool zigbee_raw_frame_callback(const ezb_zcl_raw_frame_t *raw_frame)
{
    if (raw_frame == NULL || raw_frame->header == NULL) {
        return false;
    }

    const ezb_zcl_cmd_hdr_t *hdr = raw_frame->header;

    /* Only intercept cluster-specific commands sent to server on EP 1 */
    if (hdr->dst_ep != Z906_ZB_EP_MAIN) {
        return false;
    }

    if (EZB_ZCL_CMD_FC_GET_FRAME_TYPE(hdr->fc) != EZB_ZCL_FRAME_TYPE_CLUSTER_SPECIFIC) {
        return false;
    }

    if (EZB_ZCL_CMD_FC_IS_TO_CLI_DIRECTION(hdr->fc)) {
        return false;
    }

    if (hdr->cluster_id == ZCL_CLUSTER_ON_OFF) {
        return handle_on_off_command(hdr);
    } else if (hdr->cluster_id == ZCL_CLUSTER_LEVEL_CONTROL) {
        return handle_level_control_command(raw_frame);
    }

    /* Pass through all other clusters (e.g. Identify, OTA Upgrade) */
    return false;
}

/* ========================================================================= */
/* Custom Clusters (EP 2, EP 3, EP 4) Ingress Handlers                       */
/* ========================================================================= */

static ezb_zcl_status_t custom_check_value_cb(uint16_t attr_id, uint8_t ep_id, void *value)
{
    if (value == NULL) {
        return EZB_ZCL_STATUS_INVALID_VALUE;
    }

    /*
     * Pre-check: Ensure ingress queue has capacity.
     * Single-Producer Guarantee: Because the Zigbee stack callback context is the ONLY
     * task producing into s_ingress_queue, verifying capacity here strictly guarantees
     * that write_attr_cb will succeed in queueing the request.
     */
    if (uxQueueSpacesAvailable(s_ingress_queue) == 0) {
        ESP_LOGW(TAG, "Ingress queue full; rejecting attribute write (EP: %u, Attr: 0x%04X)", ep_id, attr_id);
        return EZB_ZCL_STATUS_INSUFFICIENT_SPACE;
    }

    if (ep_id == Z906_ZB_EP_TRIMS) {
        uint8_t val_u8 = *(uint8_t *)value;
        if (attr_id == ZCL_ATTR_AUDIO_MUTE) {
            if (!z906_cmd_registry_validate_param(Z906_CMD_ID_MUTE, val_u8)) {
                return EZB_ZCL_STATUS_INVALID_VALUE;
            }
            return EZB_ZCL_STATUS_SUCCESS;
        }

        /* Channel trims (0-43 discrete hardware steps, mapped to ZCL 0-254) */
        uint8_t hw_trim = z906_cmd_zcl_to_volume(val_u8);
        z906_cmd_id_t cmd_id;
        switch (attr_id) {
            case ZCL_ATTR_TRIM_BASS:   cmd_id = Z906_CMD_ID_TRIM_BASS;   break;
            case ZCL_ATTR_TRIM_CENTER: cmd_id = Z906_CMD_ID_TRIM_CENTER; break;
            case ZCL_ATTR_TRIM_REAR:   cmd_id = Z906_CMD_ID_TRIM_REAR;   break;
            default:
                return EZB_ZCL_STATUS_UNSUP_ATTRIB;
        }

        if (!z906_cmd_registry_validate_param(cmd_id, hw_trim)) {
            return EZB_ZCL_STATUS_INVALID_VALUE;
        }
        return EZB_ZCL_STATUS_SUCCESS;

    } else if (ep_id == Z906_ZB_EP_SOURCES) {
        uint8_t val_u8 = *(uint8_t *)value;
        if (attr_id == ZCL_ATTR_SOURCE_INPUT) {
            if (!z906_cmd_registry_validate_param(Z906_CMD_ID_INPUT, val_u8)) {
                return EZB_ZCL_STATUS_INVALID_VALUE;
            }
            return EZB_ZCL_STATUS_SUCCESS;
        } else if (attr_id == ZCL_ATTR_SOURCE_EFFECT) {
            if (!z906_cmd_registry_validate_param(Z906_CMD_ID_EFFECT, val_u8)) {
                return EZB_ZCL_STATUS_INVALID_VALUE;
            }
            return EZB_ZCL_STATUS_SUCCESS;
        }
        return EZB_ZCL_STATUS_UNSUP_ATTRIB;

    } else if (ep_id == Z906_ZB_EP_TELEMETRY) {
        /* Read-only protection for hardware telemetry attributes */
        if (attr_id <= ZCL_ATTR_AUTO_STANDBY) {
            ESP_LOGW(TAG, "Rejected write to read-only telemetry attribute 0x%04X on EP %u", attr_id, ep_id);
            return EZB_ZCL_STATUS_READ_ONLY;
        }

        uint8_t val_u8 = *(uint8_t *)value;
        if (attr_id == ZCL_ATTR_BUILT_IN_LED_ENABLED || attr_id == ZCL_ATTR_CMD_VERIFIER_ENABLED) {
            if (val_u8 > 1) {
                return EZB_ZCL_STATUS_INVALID_VALUE;
            }
            return EZB_ZCL_STATUS_SUCCESS;
        }
        return EZB_ZCL_STATUS_UNSUP_ATTRIB;
    }

    return EZB_ZCL_STATUS_UNSUP_ATTRIB;
}

static void custom_write_attr_cb(uint8_t ep_id, uint16_t attr_id, void *new_value, uint16_t manuf_code)
{
    (void)manuf_code;

    if (new_value == NULL) {
        return;
    }

    uint8_t val_u8 = *(uint8_t *)new_value;
    zb_ingress_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.endpoint = ep_id;

    if (ep_id == Z906_ZB_EP_TRIMS) {
        evt.cluster = ZCL_CLUSTER_Z906_TRIMS;
        evt.type    = ZB_INGRESS_TYPE_HARDWARE_CMD;

        if (attr_id == ZCL_ATTR_AUDIO_MUTE) {
            evt.cmd_id    = Z906_CMD_ID_MUTE;
            evt.param_val = val_u8 ? 1 : 0;
        } else {
            uint8_t hw_trim = z906_cmd_zcl_to_volume(val_u8);
            evt.param_val = hw_trim;
            switch (attr_id) {
                case ZCL_ATTR_TRIM_BASS:   evt.cmd_id = Z906_CMD_ID_TRIM_BASS;   break;
                case ZCL_ATTR_TRIM_CENTER: evt.cmd_id = Z906_CMD_ID_TRIM_CENTER; break;
                case ZCL_ATTR_TRIM_REAR:   evt.cmd_id = Z906_CMD_ID_TRIM_REAR;   break;
                default: return;
            }
        }

        /* Non-blocking enqueue guaranteed by single-producer pre-check */
        BaseType_t sent = xQueueSend(s_ingress_queue, &evt, 0);
        if (sent != pdTRUE) {
            ESP_LOGE(TAG, "FATAL: Queue full in write_attr_cb despite single-producer capacity pre-check!");
        }

        /* CRITICAL: Immediately restore backing memory to verified physical state */
        zigbee_device_model_restore_attr(ep_id, evt.cluster, attr_id);

    } else if (ep_id == Z906_ZB_EP_SOURCES) {
        evt.cluster = ZCL_CLUSTER_Z906_SOURCES;
        evt.type    = ZB_INGRESS_TYPE_HARDWARE_CMD;

        if (attr_id == ZCL_ATTR_SOURCE_INPUT) {
            evt.cmd_id    = Z906_CMD_ID_INPUT;
            evt.param_val = val_u8;
        } else if (attr_id == ZCL_ATTR_SOURCE_EFFECT) {
            evt.cmd_id    = Z906_CMD_ID_EFFECT;
            evt.param_val = val_u8;
        } else {
            return;
        }

        BaseType_t sent = xQueueSend(s_ingress_queue, &evt, 0);
        if (sent != pdTRUE) {
            ESP_LOGE(TAG, "FATAL: Queue full in write_attr_cb despite single-producer capacity pre-check!");
        }

        /* CRITICAL: Immediately restore backing memory to verified physical state */
        zigbee_device_model_restore_attr(ep_id, evt.cluster, attr_id);

    } else if (ep_id == Z906_ZB_EP_TELEMETRY) {
        evt.cluster = ZCL_CLUSTER_Z906_TELEMETRY;

        if (attr_id == ZCL_ATTR_BUILT_IN_LED_ENABLED) {
            evt.type      = ZB_INGRESS_TYPE_FEATURE_LED;
            evt.param_val = val_u8 ? 1 : 0;
            BaseType_t sent = xQueueSend(s_ingress_queue, &evt, 0);
            if (sent != pdTRUE) {
                ESP_LOGE(TAG, "FATAL: Queue full in write_attr_cb for LED setting despite capacity pre-check!");
            }
        } else if (attr_id == ZCL_ATTR_CMD_VERIFIER_ENABLED) {
            evt.type      = ZB_INGRESS_TYPE_FEATURE_VERIFIER;
            evt.param_val = val_u8 ? 1 : 0;
            BaseType_t sent = xQueueSend(s_ingress_queue, &evt, 0);
            if (sent != pdTRUE) {
                ESP_LOGE(TAG, "FATAL: Queue full in write_attr_cb for verifier setting despite capacity pre-check!");
            }
        }

        /* Restore backing memory to current toggle state */
        zigbee_device_model_restore_attr(ep_id, evt.cluster, attr_id);
    }
}

/* ========================================================================= */
/* Public API Implementation                                                 */
/* ========================================================================= */

esp_err_t zigbee_command_ingress_init(void)
{
    if (s_ingress_initialized) {
        ESP_LOGI(TAG, "Command ingress subsystem already initialized (idempotent).");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Zigbee command ingress subsystem (static queue depth: %d)...",
             ZB_INGRESS_QUEUE_DEPTH);

    /* 1. Create static FreeRTOS command queue */
    s_ingress_queue = xQueueCreateStatic(ZB_INGRESS_QUEUE_DEPTH,
                                         sizeof(zb_ingress_event_t),
                                         s_queue_storage,
                                         &s_static_queue_control);
    if (s_ingress_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create static ingress queue.");
        return ESP_FAIL;
    }

    /* 2. Spawn static command bridge worker task */
    s_worker_task_handle = xTaskCreateStatic(zigbee_cmd_worker_task,
                                             "zb_cmd_worker",
                                             ZB_WORKER_STACK_SIZE / sizeof(StackType_t),
                                             NULL,
                                             3, /* Priority 3 (Medium) */
                                             s_worker_stack,
                                             &s_worker_tcb);
    if (s_worker_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create static ingress worker task.");
        return ESP_FAIL;
    }

    /* 3. Register raw command handler for EP1 standard clusters (OnOff, Level) */
    ezb_zcl_raw_command_handler_register(zigbee_raw_frame_callback);
    ESP_LOGI(TAG, "Raw command handler registered for standard clusters (EP1 On/Off, Level).");

    /* 4. Register custom cluster handlers for EP2, EP3, and EP4 */
    static const ezb_zcl_custom_cluster_handlers_t custom_handlers_trims = {
        .cluster_id     = ZCL_CLUSTER_Z906_TRIMS,
        .cluster_role   = EZB_ZCL_CLUSTER_SERVER,
        .check_value_cb = custom_check_value_cb,
        .write_attr_cb  = custom_write_attr_cb,
        .cmd_disc_cb    = NULL,
        .process_cmd_cb = NULL,
    };
    ezb_err_t err_trims = ezb_zcl_custom_cluster_handlers_register(&custom_handlers_trims);
    if (err_trims != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "Failed to register custom cluster handlers for 0xFC01 (Trims): %d", (int)err_trims);
        return ESP_FAIL;
    }

    static const ezb_zcl_custom_cluster_handlers_t custom_handlers_sources = {
        .cluster_id     = ZCL_CLUSTER_Z906_SOURCES,
        .cluster_role   = EZB_ZCL_CLUSTER_SERVER,
        .check_value_cb = custom_check_value_cb,
        .write_attr_cb  = custom_write_attr_cb,
        .cmd_disc_cb    = NULL,
        .process_cmd_cb = NULL,
    };
    ezb_err_t err_sources = ezb_zcl_custom_cluster_handlers_register(&custom_handlers_sources);
    if (err_sources != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "Failed to register custom cluster handlers for 0xFC02 (Sources): %d", (int)err_sources);
        return ESP_FAIL;
    }

    static const ezb_zcl_custom_cluster_handlers_t custom_handlers_telemetry = {
        .cluster_id     = ZCL_CLUSTER_Z906_TELEMETRY,
        .cluster_role   = EZB_ZCL_CLUSTER_SERVER,
        .check_value_cb = custom_check_value_cb,
        .write_attr_cb  = custom_write_attr_cb,
        .cmd_disc_cb    = NULL,
        .process_cmd_cb = NULL,
    };
    ezb_err_t err_telemetry = ezb_zcl_custom_cluster_handlers_register(&custom_handlers_telemetry);
    if (err_telemetry != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "Failed to register custom cluster handlers for 0xFC03 (Telemetry): %d", (int)err_telemetry);
        return ESP_FAIL;
    }

    s_ingress_initialized = true;
    ESP_LOGI(TAG, "Zigbee command ingress subsystem initialized successfully.");
    return ESP_OK;
}

bool zigbee_command_ingress_is_initialized(void)
{
    return s_ingress_initialized;
}

size_t zigbee_command_ingress_get_queue_free_spaces(void)
{
    if (s_ingress_queue == NULL) {
        return 0;
    }
    return (size_t)uxQueueSpacesAvailable(s_ingress_queue);
}
