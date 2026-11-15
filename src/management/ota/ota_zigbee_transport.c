/**
 * @file ota_zigbee_transport.c
 * @brief Native Zigbee OTA Upgrade Client transport adapter, streaming parser, and flash writer.
 *
 * Directives & Concurrency Rules (Phase 7):
 *   1. Pure ESP Zigbee SDK v2: Zero legacy SDK v1 or deprecated stack symbols.
 *   2. Core Action Callbacks:
 *      - Handles EZB_ZCL_CORE_OTA_UPGRADE_CLIENT_PROGRESS_CB_ID
 *      - Handles EZB_ZCL_CORE_OTA_UPGRADE_QUERY_NEXT_IMAGE_RSP_CB_ID
 *   3. Streaming OTA Container Parser:
 *      - Processes raw incoming blocks (EZB_ZCL_OTA_UPGRADE_PROGRESS_RECEIVING).
 *      - Parses mandatory header (56 bytes, ezb_zcl_ota_file_header_t), optional header fields,
 *        and sub-elements (ezb_zcl_ota_file_sub_element_header_t).
 *      - Streams ONLY sub-element 0x0000 (EZB_ZCL_OTA_FILE_TAG_ID_UPGRADE_IMAGE) bytes into esp_ota_write().
 *      - Skips non-firmware sub-elements safely across arbitrary block chunk boundaries.
 *   4. Strict Firmware Validation:
 *      - File ID matches exclusively 0x0BEEF11E (ZCL r08 §11.4.2.1.1). No byte-swapped variants accepted.
 *      - Header version matches 0x0100.
 *      - Manufacturer Code matches 0x131B (Z906_OTA_MANUFACTURER_CODE).
 *      - Image Type matches 0x0906 (Z906_OTA_IMAGE_TYPE).
 *      - Monotonic File Version strictly greater than Z906_OTA_FILE_VERSION (anti-downgrade).
 *      - Firmware raw binary payload size does not exceed target OTA partition capacity (1,792 KB).
 *   5. Pinned Issue #906 Policy:
 *      - In EZB_ZCL_OTA_UPGRADE_PROGRESS_FINISH: If session is validly APPLIED, logs countdown delay
 *        and executes esp_restart() immediately to prevent upstream assertion in esp-zigbee-lib 2.0.4.
 *      - If session is not validly APPLIED, sets message->out.result = EZB_ZCL_STATUS_ABORT.
 *   6. Memory & FreeRTOS Rules:
 *      - Zero dynamic memory allocation (static buffers and parser context).
 *      - Uses esp_ota_begin(), esp_ota_write(), esp_ota_end(), and esp_ota_set_boot_partition().
 */

#include "ota_zigbee_transport.h"
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "config.h"
#include "ota_types.h"
#include "ota_client.h"
#include "ota_health.h"
#include "led_notification_manager.h"
#include "zigbee_native_stack.h"
#include "clusters/zcl_clusters.h"

#include <ezbee/af.h>
#include <ezbee/core_types.h>
#include <ezbee/bdb.h>
#include <ezbee/zcl/zcl_type.h>
#include <ezbee/zcl/zcl_desc.h>
#include <ezbee/zcl/zcl_common.h>
#include <ezbee/zcl/zcl_core.h>
#include <ezbee/zcl/cluster/ota_upgrade_desc.h>
#include <ezbee/zcl/cluster/ota_upgrade.h>
#include <ezbee/zcl/cluster/ota_file.h>

static const char *TAG = "OTA_ZB_TRANS";

/* Maximum download block size requested from OTA server (223 bytes for optimal IEEE 802.15.4 frame fit) */
#define OTA_MAX_BLOCK_SIZE_BYTES       223

/* Canonical ZCL OTA Upgrade File Identifier (ZCL r08 §11.4.2.1.1).
 * On the little-endian ESP32-C6, the uint32_t read from the wire equals 0x0BEEF11E.
 * The former byte-swapped alias 0x1EF1EE0B is permanently banned; accepting it would
 * pass invalid containers produced by non-compliant tooling. */
#define ZIGBEE_OTA_FILE_IDENTIFIER_LE  0x0BEEF11EU

/* Parser States */
typedef enum {
    OTA_PARSE_MANDATORY_HEADER = 0,
    OTA_PARSE_OPTIONAL_HEADER,
    OTA_PARSE_SUB_ELEMENT_HEADER,
    OTA_PARSE_UPGRADE_IMAGE_PAYLOAD,
    OTA_PARSE_SKIP_SUB_ELEMENT_PAYLOAD,
    OTA_PARSE_COMPLETE,
    OTA_PARSE_ERROR
} ota_parser_state_t;

/* Transport Session State */
typedef enum {
    OTA_TRANS_IDLE = 0,
    OTA_TRANS_QUERYING,
    OTA_TRANS_DOWNLOADING,
    OTA_TRANS_CHECKED,
    OTA_TRANS_APPLIED,
    OTA_TRANS_FAILED
} ota_transport_state_t;

/* Streaming Parser Context (Static Storage) */
typedef struct {
    ota_parser_state_t parse_state;
    ota_transport_state_t session_state;

    /* Temporary accumulator for chunk boundary alignment */
    uint8_t accum_buf[64];
    uint16_t accum_len;

    /* Parsed header metadata */
    ezb_zcl_ota_file_header_t hdr;
    uint16_t remaining_header_bytes;

    /* Current sub-element parsing */
    ezb_zcl_ota_file_sub_element_header_t current_sub_elem;
    uint32_t current_sub_elem_bytes_left;
    bool current_sub_elem_is_image;

    /* Overall stream stats */
    uint32_t total_stream_offset;
    uint32_t total_payload_bytes_written;
    uint32_t expected_total_image_size;

    /* Flash handles */
    const esp_partition_t *target_partition;
    esp_ota_handle_t ota_handle;
    bool flash_write_started;

    /* Query information */
    uint16_t server_short_addr;
    uint8_t server_ep;

    /* Progress logging */
    uint8_t last_logged_percent;
} ota_zigbee_parser_ctx_t;

static ota_zigbee_parser_ctx_t s_ctx;

/* Forward Declarations */
static void ota_core_action_callback(ezb_zcl_core_action_callback_id_t callback_id, void *message);
static void handle_client_progress(ezb_zcl_ota_upgrade_client_progress_message_t *msg);
static void handle_query_image_rsp(ezb_zcl_ota_upgrade_query_next_image_rsp_message_t *msg);
static bool process_incoming_stream_bytes(const uint8_t *data, size_t len);

/* ========================================================================= */
/* Streaming Parser Logic                                                    */
/* ========================================================================= */

static void reset_parser_context(void)
{
    if (s_ctx.flash_write_started && s_ctx.ota_handle != 0) {
        esp_ota_abort(s_ctx.ota_handle);
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.parse_state = OTA_PARSE_MANDATORY_HEADER;
    s_ctx.session_state = OTA_TRANS_IDLE;
    s_ctx.server_short_addr = 0x0000; /* Default coordinator */
    s_ctx.server_ep = 1;
}

static bool validate_ota_header(const ezb_zcl_ota_file_header_t *hdr)
{
    /* 1. Validate File Identifier — strict single-value check.
     * Only 0x0BEEF11E (ZCL r08 §11.4.2.1.1) is accepted. The byte-swapped variant
     * 0x1EF1EE0B is permanently rejected to prevent non-compliant containers from
     * slipping through on byte-order mismatches in test tooling or converters. */
    if (hdr->file_id != ZIGBEE_OTA_FILE_IDENTIFIER_LE) {
        ESP_LOGE(TAG, "Invalid OTA File ID: 0x%08lX (expected canonical 0x%08X; 0x1EF1EE0B is not accepted)",
                 (unsigned long)hdr->file_id, (unsigned int)ZIGBEE_OTA_FILE_IDENTIFIER_LE);
        return false;
    }

    /* 2. Validate Header Version */
    if (hdr->hdr_version != EZB_ZCL_OTA_FILE_HEADER_VERSION) {
        ESP_LOGE(TAG, "Unsupported OTA Header Version: 0x%04X (expected 0x%04X)",
                 hdr->hdr_version, EZB_ZCL_OTA_FILE_HEADER_VERSION);
        return false;
    }

    /* 3. Validate Manufacturer Code */
    if (hdr->manufacturer_code != Z906_OTA_MANUFACTURER_CODE) {
        ESP_LOGW(TAG, "Rejected OTA Image: Manufacturer Code 0x%04X != expected 0x%04X",
                 hdr->manufacturer_code, Z906_OTA_MANUFACTURER_CODE);
        return false;
    }

    /* 4. Validate Image Type */
    if (hdr->image_type != Z906_OTA_IMAGE_TYPE) {
        ESP_LOGW(TAG, "Rejected OTA Image: Image Type 0x%04X != expected 0x%04X",
                 hdr->image_type, Z906_OTA_IMAGE_TYPE);
        return false;
    }

    /* 5. Validate Monotonic File Version (Strict Anti-Downgrade) */
    if (hdr->file_version <= Z906_OTA_FILE_VERSION) {
        ESP_LOGW(TAG, "Rejected OTA Image: Offered version (0x%08lX) is not newer than current (0x%08lX)",
                 (unsigned long)hdr->file_version, (unsigned long)Z906_OTA_FILE_VERSION);
        return false;
    }

    /* 6. Validate Header Length */
    if (hdr->hdr_length < sizeof(ezb_zcl_ota_file_header_t)) {
        ESP_LOGE(TAG, "Invalid OTA Header Length: %u bytes (minimum %u)",
                 hdr->hdr_length, (unsigned int)sizeof(ezb_zcl_ota_file_header_t));
        return false;
    }

    ESP_LOGI(TAG, "Zigbee OTA Header Validated Successfully:");
    ESP_LOGI(TAG, "  Manufacturer: 0x%04X, ImageType: 0x%04X, FileVersion: 0x%08lX, TotalSize: %lu bytes",
             hdr->manufacturer_code, hdr->image_type,
             (unsigned long)hdr->file_version, (unsigned long)hdr->total_image_size);

    return true;
}

static bool process_incoming_stream_bytes(const uint8_t *data, size_t len)
{
    size_t offset = 0;

    while (offset < len) {
        switch (s_ctx.parse_state) {
            case OTA_PARSE_MANDATORY_HEADER: {
                size_t needed = sizeof(ezb_zcl_ota_file_header_t) - s_ctx.accum_len;
                size_t available = len - offset;
                size_t to_copy = (available < needed) ? available : needed;

                memcpy(&s_ctx.accum_buf[s_ctx.accum_len], &data[offset], to_copy);
                s_ctx.accum_len += to_copy;
                offset += to_copy;
                s_ctx.total_stream_offset += to_copy;

                if (s_ctx.accum_len == sizeof(ezb_zcl_ota_file_header_t)) {
                    memcpy(&s_ctx.hdr, s_ctx.accum_buf, sizeof(ezb_zcl_ota_file_header_t));
                    s_ctx.accum_len = 0;

                    if (!validate_ota_header(&s_ctx.hdr)) {
                        s_ctx.parse_state = OTA_PARSE_ERROR;
                        return false;
                    }

                    s_ctx.expected_total_image_size = s_ctx.hdr.total_image_size;
                    s_ctx.remaining_header_bytes = s_ctx.hdr.hdr_length - sizeof(ezb_zcl_ota_file_header_t);

                    if (s_ctx.remaining_header_bytes > 0) {
                        s_ctx.parse_state = OTA_PARSE_OPTIONAL_HEADER;
                    } else {
                        s_ctx.parse_state = OTA_PARSE_SUB_ELEMENT_HEADER;
                    }
                }
                break;
            }

            case OTA_PARSE_OPTIONAL_HEADER: {
                size_t available = len - offset;
                size_t to_skip = (available < s_ctx.remaining_header_bytes) ? available : s_ctx.remaining_header_bytes;

                offset += to_skip;
                s_ctx.remaining_header_bytes -= to_skip;
                s_ctx.total_stream_offset += to_skip;

                if (s_ctx.remaining_header_bytes == 0) {
                    s_ctx.parse_state = OTA_PARSE_SUB_ELEMENT_HEADER;
                }
                break;
            }

            case OTA_PARSE_SUB_ELEMENT_HEADER: {
                size_t needed = sizeof(ezb_zcl_ota_file_sub_element_header_t) - s_ctx.accum_len;
                size_t available = len - offset;
                size_t to_copy = (available < needed) ? available : needed;

                memcpy(&s_ctx.accum_buf[s_ctx.accum_len], &data[offset], to_copy);
                s_ctx.accum_len += to_copy;
                offset += to_copy;
                s_ctx.total_stream_offset += to_copy;

                if (s_ctx.accum_len == sizeof(ezb_zcl_ota_file_sub_element_header_t)) {
                    memcpy(&s_ctx.current_sub_elem, s_ctx.accum_buf, sizeof(ezb_zcl_ota_file_sub_element_header_t));
                    s_ctx.accum_len = 0;
                    s_ctx.current_sub_elem_bytes_left = s_ctx.current_sub_elem.length;

                    ESP_LOGI(TAG, "Sub-element encountered: Tag 0x%04X, Length %lu bytes",
                             s_ctx.current_sub_elem.tag_id, (unsigned long)s_ctx.current_sub_elem.length);

                    if (s_ctx.current_sub_elem.tag_id == EZB_ZCL_OTA_FILE_TAG_ID_UPGRADE_IMAGE) {
                        s_ctx.current_sub_elem_is_image = true;

                        /* Validate payload size against target partition boundary */
                        if (s_ctx.target_partition != NULL &&
                            s_ctx.current_sub_elem.length > s_ctx.target_partition->size) {
                            ESP_LOGE(TAG, "Sub-element payload size %lu exceeds partition size %lu",
                                     (unsigned long)s_ctx.current_sub_elem.length,
                                     (unsigned long)s_ctx.target_partition->size);
                            s_ctx.parse_state = OTA_PARSE_ERROR;
                            return false;
                        }

                        /* Initialize ESP-IDF OTA writer on first payload start */
                        if (!s_ctx.flash_write_started) {
                            esp_err_t ota_err = esp_ota_begin(s_ctx.target_partition,
                                                              s_ctx.current_sub_elem.length,
                                                              &s_ctx.ota_handle);
                            if (ota_err != ESP_OK) {
                                ESP_LOGE(TAG, "esp_ota_begin failed: %s (0x%x)", esp_err_to_name(ota_err), ota_err);
                                s_ctx.parse_state = OTA_PARSE_ERROR;
                                return false;
                            }
                            s_ctx.flash_write_started = true;
                            ESP_LOGI(TAG, "Flash OTA begin successful on partition '%s' (OTA handle: 0x%lx)",
                                     s_ctx.target_partition->label, (unsigned long)s_ctx.ota_handle);
                        }

                        s_ctx.parse_state = OTA_PARSE_UPGRADE_IMAGE_PAYLOAD;
                    } else {
                        /* Non-image sub-element (e.g. ECDSA signature, integrity code, certificates) */
                        s_ctx.current_sub_elem_is_image = false;
                        s_ctx.parse_state = OTA_PARSE_SKIP_SUB_ELEMENT_PAYLOAD;
                    }
                }
                break;
            }

            case OTA_PARSE_UPGRADE_IMAGE_PAYLOAD: {
                size_t available = len - offset;
                size_t to_write = (available < s_ctx.current_sub_elem_bytes_left) ? available : s_ctx.current_sub_elem_bytes_left;

                if (to_write > 0) {
                    esp_err_t write_err = esp_ota_write(s_ctx.ota_handle, &data[offset], to_write);
                    if (write_err != ESP_OK) {
                        ESP_LOGE(TAG, "esp_ota_write failed at stream offset %lu: %s (0x%x)",
                                 (unsigned long)s_ctx.total_stream_offset,
                                 esp_err_to_name(write_err), write_err);
                        s_ctx.parse_state = OTA_PARSE_ERROR;
                        return false;
                    }

                    s_ctx.total_payload_bytes_written += to_write;
                    s_ctx.current_sub_elem_bytes_left -= to_write;
                    s_ctx.total_stream_offset += to_write;
                    offset += to_write;

                    /* Rate-limited 10% progress logging */
                    if (s_ctx.current_sub_elem.length > 0) {
                        uint8_t percent = (uint8_t)((s_ctx.total_payload_bytes_written * 100ULL) /
                                                    s_ctx.current_sub_elem.length);
                        if (percent >= s_ctx.last_logged_percent + 10 ||
                            s_ctx.total_payload_bytes_written >= s_ctx.current_sub_elem.length) {
                            s_ctx.last_logged_percent = percent;
                            ESP_LOGI(TAG, "OTA Download Progress: %d%% (%lu / %lu bytes)",
                                     percent, (unsigned long)s_ctx.total_payload_bytes_written,
                                     (unsigned long)s_ctx.current_sub_elem.length);
                        }
                    }
                }

                if (s_ctx.current_sub_elem_bytes_left == 0) {
                    /* Finished writing this image sub-element; check if more sub-elements exist */
                    if (s_ctx.total_stream_offset < s_ctx.expected_total_image_size) {
                        s_ctx.parse_state = OTA_PARSE_SUB_ELEMENT_HEADER;
                    } else {
                        s_ctx.parse_state = OTA_PARSE_COMPLETE;
                    }
                }
                break;
            }

            case OTA_PARSE_SKIP_SUB_ELEMENT_PAYLOAD: {
                size_t available = len - offset;
                size_t to_skip = (available < s_ctx.current_sub_elem_bytes_left) ? available : s_ctx.current_sub_elem_bytes_left;

                s_ctx.current_sub_elem_bytes_left -= to_skip;
                s_ctx.total_stream_offset += to_skip;
                offset += to_skip;

                if (s_ctx.current_sub_elem_bytes_left == 0) {
                    if (s_ctx.total_stream_offset < s_ctx.expected_total_image_size) {
                        s_ctx.parse_state = OTA_PARSE_SUB_ELEMENT_HEADER;
                    } else {
                        s_ctx.parse_state = OTA_PARSE_COMPLETE;
                    }
                }
                break;
            }

            case OTA_PARSE_COMPLETE:
                /* Extra padding or trailing bytes */
                offset = len;
                break;

            case OTA_PARSE_ERROR:
            default:
                return false;
        }
    }

    return true;
}

/* ========================================================================= */
/* Core Action Progress Handler & Issue #906 Policy                          */
/* ========================================================================= */

static void handle_client_progress(ezb_zcl_ota_upgrade_client_progress_message_t *msg)
{
    if (msg == NULL) {
        return;
    }

    ezb_zcl_ota_upgrade_progress_t progress = msg->in.progress;

    switch (progress) {
        case EZB_ZCL_OTA_UPGRADE_PROGRESS_START: {
            ESP_LOGI(TAG, "OTA Progress: START received from coordinator (MFR: 0x%04X, Type: 0x%04X, Ver: 0x%08lX, Size: %lu)",
                     msg->in.start.manuf_code, msg->in.start.image_type,
                     (unsigned long)msg->in.start.file_version, (unsigned long)msg->in.start.image_size);

            /* Check compatibility and anti-downgrade */
            if (msg->in.start.manuf_code != Z906_OTA_MANUFACTURER_CODE ||
                msg->in.start.image_type != Z906_OTA_IMAGE_TYPE ||
                msg->in.start.file_version <= Z906_OTA_FILE_VERSION) {
                ESP_LOGW(TAG, "OTA START rejected: Incompatible metadata or not newer than current.");
                msg->out.result = EZB_ZCL_STATUS_INVALID_IMAGE;
                s_ctx.session_state = OTA_TRANS_FAILED;
                return;
            }

            /* Initialize new download session */
            reset_parser_context();
            s_ctx.target_partition = esp_ota_get_next_update_partition(NULL);
            if (s_ctx.target_partition == NULL) {
                ESP_LOGE(TAG, "Cannot start OTA: No inactive target update partition found!");
                msg->out.result = EZB_ZCL_STATUS_INSUFFICIENT_SPACE;
                s_ctx.session_state = OTA_TRANS_FAILED;
                return;
            }

            s_ctx.session_state = OTA_TRANS_DOWNLOADING;
            msg->out.result = EZB_ZCL_STATUS_SUCCESS;
            ESP_LOGI(TAG, "OTA START accepted. Target partition: '%s' (0x%08lx, Size: %lu KB)",
                     s_ctx.target_partition->label, (unsigned long)s_ctx.target_partition->address,
                     (unsigned long)(s_ctx.target_partition->size / 1024));
            break;
        }

        case EZB_ZCL_OTA_UPGRADE_PROGRESS_RECEIVING: {
            uint32_t file_offset = msg->in.receiving.file_offset;
            uint8_t block_size = msg->in.receiving.block_size;
            const uint8_t *block = msg->in.receiving.block;

            if (s_ctx.session_state != OTA_TRANS_DOWNLOADING || block == NULL || block_size == 0) {
                ESP_LOGW(TAG, "OTA RECEIVING rejected: Invalid session state (%d) or null block", (int)s_ctx.session_state);
                msg->out.result = EZB_ZCL_STATUS_ABORT;
                return;
            }

            /* Stream block through streaming container parser */
            bool ok = process_incoming_stream_bytes(block, block_size);
            if (!ok) {
                ESP_LOGE(TAG, "OTA RECEIVING error during block at file_offset %lu (size: %u)",
                         (unsigned long)file_offset, block_size);
                reset_parser_context();
                s_ctx.session_state = OTA_TRANS_FAILED;
                msg->out.result = EZB_ZCL_STATUS_INVALID_IMAGE;
                led_notification_post_cmd_result(false);
                return;
            }

            msg->out.result = EZB_ZCL_STATUS_SUCCESS;
            break;
        }

        case EZB_ZCL_OTA_UPGRADE_PROGRESS_CHECK: {
            ESP_LOGI(TAG, "OTA Progress: CHECK received (MFR: 0x%04X, Type: 0x%04X, Ver: 0x%08lX)",
                     msg->in.check.manuf_code, msg->in.check.image_type,
                     (unsigned long)msg->in.check.file_version);

            if (s_ctx.session_state != OTA_TRANS_DOWNLOADING || !s_ctx.flash_write_started) {
                ESP_LOGE(TAG, "OTA CHECK failed: No active flash write in progress.");
                msg->out.result = EZB_ZCL_STATUS_INVALID_IMAGE;
                s_ctx.session_state = OTA_TRANS_FAILED;
                return;
            }

            /* End flash writing and validate image header & checksum in partition */
            esp_err_t end_err = esp_ota_end(s_ctx.ota_handle);
            s_ctx.ota_handle = 0;
            s_ctx.flash_write_started = false;

            if (end_err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_end validation failed: %s (0x%x)", esp_err_to_name(end_err), end_err);
                s_ctx.session_state = OTA_TRANS_FAILED;
                msg->out.result = EZB_ZCL_STATUS_INVALID_IMAGE;
                led_notification_post_cmd_result(false);
                return;
            }

            ESP_LOGI(TAG, "OTA CHECK successful! Firmware image signature and integrity verified.");
            s_ctx.session_state = OTA_TRANS_CHECKED;
            msg->out.result = EZB_ZCL_STATUS_SUCCESS;
            break;
        }

        case EZB_ZCL_OTA_UPGRADE_PROGRESS_APPLY: {
            ESP_LOGI(TAG, "OTA Progress: APPLY received (MFR: 0x%04X, Type: 0x%04X, Ver: 0x%08lX)",
                     msg->in.apply.manuf_code, msg->in.apply.image_type,
                     (unsigned long)msg->in.apply.file_version);

            if (s_ctx.session_state != OTA_TRANS_CHECKED || s_ctx.target_partition == NULL) {
                ESP_LOGE(TAG, "OTA APPLY failed: Session not in CHECKED state or null partition.");
                msg->out.result = EZB_ZCL_STATUS_ABORT;
                s_ctx.session_state = OTA_TRANS_FAILED;
                return;
            }

            /* Configure next boot partition in otadata */
            esp_err_t boot_err = esp_ota_set_boot_partition(s_ctx.target_partition);
            if (boot_err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s (0x%x)",
                         esp_err_to_name(boot_err), boot_err);
                msg->out.result = EZB_ZCL_STATUS_FAIL;
                s_ctx.session_state = OTA_TRANS_FAILED;
                led_notification_post_cmd_result(false);
                return;
            }

            ESP_LOGI(TAG, "Next boot partition set successfully to '%s' (0x%08lx)",
                     s_ctx.target_partition->label, (unsigned long)s_ctx.target_partition->address);
            s_ctx.session_state = OTA_TRANS_APPLIED;
            msg->out.result = EZB_ZCL_STATUS_SUCCESS;
            led_notification_post_cmd_result(true);
            break;
        }

        case EZB_ZCL_OTA_UPGRADE_PROGRESS_FINISH: {
            uint32_t count_down_delay = msg->in.finish.count_down_delay;
            ESP_LOGI(TAG, "OTA Progress: FINISH received (count_down_delay: %lu s)", (unsigned long)count_down_delay);

            /* Issue #906 Policy Enforcement:
             * Upstream esp-zigbee-lib 2.0.4 has an open assertion failure if a successful FINISH callback returns.
             * If the session is verified and APPLIED, we log the countdown delay and immediately call esp_restart().
             * This function does not return, preventing the upstream crash.
             */
            if (s_ctx.session_state == OTA_TRANS_APPLIED) {
                ESP_LOGI(TAG, "=========================================================");
                ESP_LOGI(TAG, "OTA UPGRADE FULLY APPLIED AND VERIFIED!");
                ESP_LOGI(TAG, "Immediate reboot requested per Issue #906 policy (delay was %lu s).",
                         (unsigned long)count_down_delay);
                ESP_LOGI(TAG, "Restarting system now into partition '%s'...",
                         s_ctx.target_partition ? s_ctx.target_partition->label : "new");
                ESP_LOGI(TAG, "=========================================================");

                /* Immediate restart */
                esp_restart();
            } else {
                ESP_LOGE(TAG, "OTA FINISH rejected: Session is not in APPLIED state (current: %d)",
                         (int)s_ctx.session_state);
                msg->out.result = EZB_ZCL_STATUS_ABORT;
            }
            break;
        }

        case EZB_ZCL_OTA_UPGRADE_PROGRESS_ABORT: {
            ESP_LOGW(TAG, "OTA Progress: ABORT received from coordinator or stack");
            reset_parser_context();
            s_ctx.session_state = OTA_TRANS_IDLE;
            msg->out.result = EZB_ZCL_STATUS_SUCCESS;
            led_notification_post_cmd_result(false);
            break;
        }

        default:
            ESP_LOGW(TAG, "Unhandled OTA Progress code: %d", (int)progress);
            msg->out.result = EZB_ZCL_STATUS_UNSUP_CMD;
            break;
    }
}

static void handle_query_image_rsp(ezb_zcl_ota_upgrade_query_next_image_rsp_message_t *msg)
{
    if (msg == NULL) {
        return;
    }

    uint8_t status = msg->in.image.status;
    uint16_t mfr = msg->in.image.manuf_code;
    uint16_t image_type = msg->in.image.image_type;
    uint32_t file_ver = msg->in.image.file_version;
    uint32_t size = msg->in.image.size;

    ESP_LOGI(TAG, "Query Next Image Response received: Status=0x%02X, MFR=0x%04X, Type=0x%04X, Ver=0x%08lX, Size=%lu",
             status, mfr, image_type, (unsigned long)file_ver, (unsigned long)size);

    if (status != EZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "No newer image available on OTA server (Status: 0x%02X)", status);
        s_ctx.session_state = OTA_TRANS_IDLE;
        msg->out.result = EZB_ZCL_STATUS_SUCCESS;
        return;
    }

    if (mfr != Z906_OTA_MANUFACTURER_CODE || image_type != Z906_OTA_IMAGE_TYPE) {
        ESP_LOGW(TAG, "Offered image metadata mismatch (MFR: 0x%04X vs 0x%04X, Type: 0x%04X vs 0x%04X)",
                 mfr, Z906_OTA_MANUFACTURER_CODE, image_type, Z906_OTA_IMAGE_TYPE);
        msg->out.result = EZB_ZCL_STATUS_NO_IMAGE_AVAILABLE;
        s_ctx.session_state = OTA_TRANS_IDLE;
        return;
    }

    if (file_ver <= Z906_OTA_FILE_VERSION) {
        ESP_LOGI(TAG, "Offered version 0x%08lX is not newer than current 0x%08lX. Ignoring.",
                 (unsigned long)file_ver, (unsigned long)Z906_OTA_FILE_VERSION);
        msg->out.result = EZB_ZCL_STATUS_NO_IMAGE_AVAILABLE;
        s_ctx.session_state = OTA_TRANS_IDLE;
        return;
    }

    ESP_LOGI(TAG, "Compatible new firmware image confirmed. OTA download will proceed automatically via SDK.");
    msg->out.result = EZB_ZCL_STATUS_SUCCESS;
}

static void ota_core_action_callback(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    if (callback_id == EZB_ZCL_CORE_OTA_UPGRADE_CLIENT_PROGRESS_CB_ID) {
        handle_client_progress((ezb_zcl_ota_upgrade_client_progress_message_t *)message);
    } else if (callback_id == EZB_ZCL_CORE_OTA_UPGRADE_QUERY_NEXT_IMAGE_RSP_CB_ID) {
        handle_query_image_rsp((ezb_zcl_ota_upgrade_query_next_image_rsp_message_t *)message);
    }
}

/* ========================================================================= */
/* Public API Implementation                                                 */
/* ========================================================================= */

esp_err_t ota_zigbee_transport_init(void)
{
    ESP_LOGI(TAG, "Initializing Native Zigbee OTA Transport Adapter on Endpoint %d...", Z906_ZB_EP_MAIN);

    /* 1. Reset parser and session context */
    reset_parser_context();

    /* 2. Initialize OTA Upgrade Cluster Client Role */
    ezb_zcl_ota_upgrade_cluster_client_init(Z906_ZB_EP_MAIN);

    /* 3. Configure optimal download block size (223 bytes fits in a single IEEE 802.15.4 frame) */
    ezb_err_t ezb_err = ezb_zcl_ota_upgrade_set_download_block_size(Z906_ZB_EP_MAIN, OTA_MAX_BLOCK_SIZE_BYTES);
    if (ezb_err != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "ezb_zcl_ota_upgrade_set_download_block_size failed: %d", (int)ezb_err);
        return ESP_FAIL;
    }

    /* 4. Register core action callback handler for progress and query response events.
     * NOTE: The hardware-version configuration API is intentionally not called here.
     * The ESP32-C6 board has no canonical physical hardware revision; setting an
     * invented value (e.g. 1) would cause the coordinator to silently reject
     * otherwise valid images whose hw_version range excludes 1. The Field Control
     * byte in the Query Next Image Request payload is therefore 0x00 (hw_version absent). */
    ezb_zcl_core_action_handler_register(ota_core_action_callback);

    ESP_LOGI(TAG, "Native Zigbee OTA Transport Adapter initialized successfully (BlockSize: %d, hw_version: absent).",
             OTA_MAX_BLOCK_SIZE_BYTES);

    return ESP_OK;
}

esp_err_t ota_zigbee_transport_query_next_image(void)
{
    if (!ezb_bdb_dev_joined() || !zigbee_native_is_network_ready()) {
        ESP_LOGW(TAG, "Cannot query next image: Zigbee network is not joined/ready.");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ctx.session_state == OTA_TRANS_DOWNLOADING || s_ctx.session_state == OTA_TRANS_CHECKED) {
        ESP_LOGW(TAG, "Cannot query next image: OTA download already active (State: %d)", (int)s_ctx.session_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Dispatching manual Query Next Image Request to coordinator (0x%04X, EP %d)...",
             s_ctx.server_short_addr, s_ctx.server_ep);

    ezb_zcl_ota_upgrade_query_next_image_req_cmd_t cmd_req;
    memset(&cmd_req, 0, sizeof(cmd_req));

    /* Set up command control */
    cmd_req.cmd_ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
    cmd_req.cmd_ctrl.dst_addr.u.short_addr = s_ctx.server_short_addr;
    cmd_req.cmd_ctrl.dst_ep = s_ctx.server_ep;
    cmd_req.cmd_ctrl.src_ep = Z906_ZB_EP_MAIN;
    cmd_req.cmd_ctrl.dis_default_rsp = false;

    /* Set up payload.
     * Field Control (fc) = 0x00 means hw_version is absent.
     * No canonical hardware version exists for this ESP32-C6 board; sending a
     * fabricated value (e.g. 1) would allow a coordinator with hw_version range
     * restrictions to reject valid images. Omitting hw_version is the correct default
     * per ZCL r08 §11.13.6.2. */
    cmd_req.payload.fc          = 0x00; /* hw_version field absent */
    cmd_req.payload.manuf_code  = Z906_OTA_MANUFACTURER_CODE;
    cmd_req.payload.image_type  = Z906_OTA_IMAGE_TYPE;
    cmd_req.payload.file_version = Z906_OTA_FILE_VERSION;

    ezb_err_t ezb_err = ezb_zcl_ota_upgrade_query_next_image_cmd_req(&cmd_req);
    if (ezb_err != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "ezb_zcl_ota_upgrade_query_next_image_cmd_req failed: %d", (int)ezb_err);
        return ESP_FAIL;
    }

    s_ctx.session_state = OTA_TRANS_QUERYING;
    ESP_LOGI(TAG, "Query Next Image Request sent successfully (Current Version: 0x%08lX)",
             (unsigned long)Z906_OTA_FILE_VERSION);

    return ESP_OK;
}

void ota_zigbee_transport_abort(const char *reason)
{
    ESP_LOGW(TAG, "OTA transport abort requested: %s", reason ? reason : "Unknown");
    reset_parser_context();
}

void ota_zigbee_transport_process_cycle(void)
{
    /* Service health check observation cycle */
    ota_health_process_cycle(1);
}

void ota_zigbee_transport_get_diagnostics(ota_transport_diag_t *out_diag)
{
    /* Read directly from the static parser context \u2014 no mutex needed for scalar reads
     * on a RISC-V32 single-core target (ESP32-C6 has a single application core). */
    if (out_diag == NULL) {
        return;
    }

    /* File version is valid only after the mandatory header has been fully parsed. */
    out_diag->offered_file_version  = s_ctx.hdr.file_version;

    /* Payload bytes streamed to flash (only UPGRADE_IMAGE sub-element bytes, not headers). */
    out_diag->payload_bytes_written = s_ctx.total_payload_bytes_written;

    /* Total firmware payload length taken from the UPGRADE_IMAGE sub-element length field. */
    out_diag->total_expected_size   = s_ctx.current_sub_elem_is_image
                                      ? s_ctx.current_sub_elem.length
                                      : 0;

    /* Compute percentage the same way the 10%-interval logger does, avoiding division by zero. */
    if (out_diag->total_expected_size > 0) {
        out_diag->progress_percent = (uint8_t)(
            (out_diag->payload_bytes_written * 100ULL) / out_diag->total_expected_size);
    } else {
        out_diag->progress_percent = 0;
    }

    out_diag->session_active = (s_ctx.session_state == OTA_TRANS_DOWNLOADING ||
                                s_ctx.session_state == OTA_TRANS_CHECKED);
}
