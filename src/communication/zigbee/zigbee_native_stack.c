/**
 * @file zigbee_native_stack.c
 * @brief Implementation of Native ESP Zigbee SDK v2 stack lifecycle, commissioning, and network management.
 */

#include "zigbee_native_stack.h"
#include "zigbee_device_model.h"
#include "zigbee_command_ingress.h"
#include "zigbee_state_publisher.h"
#include "ota_zigbee_transport.h"
#include "config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs_manager.h"
#include "esp_zigbee.h"
#include "ezbee/app_signals.h"
#include "ezbee/bdb.h"
#include "ezbee/core.h"
#include "ezbee/nwk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "ZB_NATIVE";

/** @brief Internal stack lifecycle state tracker */
static zb_stack_state_t s_stack_state = ZB_STACK_STATE_UNINITIALIZED;

/** @brief Authoritative network commissioning lifecycle state */
static zb_network_lifecycle_state_t s_net_state = ZB_NET_STATE_UNINITIALIZED;

/** @brief Authoritative RAM-only diagnostic telemetry snapshot */
static zb_network_diagnostics_t s_diag = {
    .stack_state = ZB_STACK_STATE_UNINITIALIZED,
    .net_state = ZB_NET_STATE_UNINITIALIZED,
    .factory_new = true,
    .sdk_joined = false,
    .network_ready = false,
    .metadata_valid = false,
    .pan_id = 0xFFFF,
    .channel = 0,
    .short_addr = 0xFFFF,
    .ext_pan_id = { 0 },
    .last_bdb_status = 0xFF,
    .last_nwk_status = 0xFF,
    .rejoin_attempt_count = 0,
};

/** @brief Maximum number of bounded rejoin attempts before backoff/idle */
#define MAX_REJOIN_ATTEMPTS 5

/** @brief FreeRTOS timer for non-blocking rejoin backoff retry */
static TimerHandle_t s_rejoin_timer = NULL;
static StaticTimer_t s_rejoin_timer_buffer;

/* Forward declarations */
static void refresh_network_metadata(void);
static void invalidate_network_metadata(void);
static void set_network_ready(bool ready);
static void start_steering_in_stack_ctx(void *ctx);
static void rejoin_retry_timer_cb(TimerHandle_t xTimer);
static void trigger_rejoin_in_stack_ctx(void *ctx);

/**
 * @brief Centralized helper to set network readiness.
 *
 * Ensures single ownership of network_ready transitions, synchronizes diagnostics,
 * enforces canonical membership truth (ezb_bdb_dev_joined()), and notifies
 * the verified state publisher.
 */
static void set_network_ready(bool ready)
{
    s_diag.sdk_joined = ezb_bdb_dev_joined();

    if (ready) {
        if (!s_diag.sdk_joined) {
            ESP_LOGW(TAG, "Inconsistency rejected: attempt to set network_ready=true while ezb_bdb_dev_joined() is false! Retaining ready=false.");
            s_diag.network_ready = false;
            zigbee_state_publisher_set_network_ready(false);
            return;
        }
    }

    s_diag.network_ready = ready;
    zigbee_state_publisher_set_network_ready(ready);
    ESP_LOGI(TAG, "Network readiness updated: %s (sdk_joined=%d)",
             ready ? "READY (ZCL Reporting Enabled)" : "NOT READY (Reports Suppressed)",
             s_diag.sdk_joined);
}

/**
 * @brief Reads network metadata from stack getters into the RAM diagnostic snapshot.
 *
 * Only reads when joined; NEVER persists these values to application NVS.
 * Dedicated zb_storage is the single authoritative source of persistence.
 */
static void refresh_network_metadata(void)
{
    s_diag.pan_id = (uint16_t)ezb_get_panid();
    s_diag.channel = ezb_get_current_channel();
    s_diag.short_addr = (uint16_t)ezb_get_short_address();

    ezb_extpanid_t ext_pan;
    ezb_get_extended_panid(&ext_pan);
    memcpy(s_diag.ext_pan_id, ext_pan.u8, sizeof(s_diag.ext_pan_id));

    s_diag.metadata_valid = true;
    s_diag.factory_new = ezb_bdb_is_factory_new();

    ESP_LOGI(TAG, "Network Diagnostic Snapshot: PAN=0x%04X, Channel=%d, Short=0x%04X, FactoryNew=%d",
             s_diag.pan_id, s_diag.channel, s_diag.short_addr, s_diag.factory_new);
}

/**
 * @brief Invalidates the RAM network diagnostic snapshot upon leave, reset, or failure.
 */
static void invalidate_network_metadata(void)
{
    s_diag.metadata_valid = false;
    s_diag.pan_id = 0xFFFF;
    s_diag.channel = 0;
    s_diag.short_addr = 0xFFFF;
    memset(s_diag.ext_pan_id, 0, sizeof(s_diag.ext_pan_id));
    s_diag.factory_new = ezb_bdb_is_factory_new();
}

/**
 * @brief Dedicated application signal handler registered with native stack via ezb_app_signal_add_handler.
 */
static bool zigbee_native_signal_handler(const ezb_app_signal_t *app_signal)
{
    if (app_signal == NULL) {
        return false;
    }

    ezb_app_signal_type_t sig_type = ezb_app_signal_get_type(app_signal);
    const char *sig_str = ezb_app_signal_to_string(sig_type);

    switch (sig_type) {
        case EZB_ZDO_SIGNAL_SKIP_STARTUP: {
            ESP_LOGI(TAG, "Signal: %s (0x%04X) -- Framework startup complete. Starting BDB initialization...",
                     sig_str ? sig_str : "SKIP_STARTUP", (unsigned int)sig_type);
            ezb_err_t ezb_err = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
            if (ezb_err != EZB_ERR_NONE) {
                ESP_LOGE(TAG, "ezb_bdb_start_top_level_commissioning(INITIALIZATION) failed: %d", (int)ezb_err);
            }
            return true;
        }

        case EZB_BDB_SIGNAL_DEVICE_FIRST_START: {
            const ezb_bdb_signal_simple_params_t *params = (const ezb_bdb_signal_simple_params_t *)ezb_app_signal_get_params(app_signal);
            uint8_t status = params ? params->status : 0xFF;
            s_diag.last_bdb_status = status;
            s_diag.factory_new = ezb_bdb_is_factory_new();
            s_diag.sdk_joined = ezb_bdb_dev_joined();

            ESP_LOGI(TAG, "Signal: %s (0x%04X) -- Device First Start (Status: %s / 0x%02X, FactoryNew: %d, Joined: %d).",
                     sig_str ? sig_str : "DEVICE_FIRST_START",
                     zigbee_native_bdb_status_to_str(status), status, s_diag.factory_new, s_diag.sdk_joined);

            set_network_ready(false);
            invalidate_network_metadata();

            /* Check one-shot user intent for re-pair after Zigbee factory reset */
            bool pair_intent = false;
            nvs_manager_get_pair_after_reset(&pair_intent);

            if (pair_intent) {
                /* Power-loss safety: Clear intent immediately before starting steering to avoid reboot loops */
                nvs_manager_clear_pair_after_reset();
                ESP_LOGI(TAG, "One-shot re-pair intent consumed and cleared. Starting network steering immediately...");
                s_net_state = ZB_NET_STATE_STEERING;
                s_diag.net_state = s_net_state;

                ezb_err_t ezb_err = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
                if (ezb_err != EZB_ERR_NONE) {
                    ESP_LOGE(TAG, "ezb_bdb_start_top_level_commissioning(STEERING) failed on intent boot: %d", (int)ezb_err);
                    s_net_state = ZB_NET_STATE_FACTORY_NEW_IDLE;
                    s_diag.net_state = s_net_state;
                }
            } else {
                /* Invariant: Conductor Router does NOT auto-steer on normal boot. Steering is CLI-controlled. */
                s_net_state = ZB_NET_STATE_FACTORY_NEW_IDLE;
                s_diag.net_state = s_net_state;
            }
            return true;
        }

        case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
            const ezb_bdb_signal_simple_params_t *params = (const ezb_bdb_signal_simple_params_t *)ezb_app_signal_get_params(app_signal);
            uint8_t status = params ? params->status : 0xFF;
            s_diag.last_bdb_status = status;
            s_diag.factory_new = ezb_bdb_is_factory_new();
            s_diag.sdk_joined = ezb_bdb_dev_joined();

            ESP_LOGI(TAG, "Signal: %s (0x%04X) -- Device Reboot with datasets (Status: %s / 0x%02X, FactoryNew: %d, Joined: %d).",
                     sig_str ? sig_str : "DEVICE_REBOOT",
                     zigbee_native_bdb_status_to_str(status), status, s_diag.factory_new, s_diag.sdk_joined);

            if (status == EZB_BDB_STATUS_SUCCESS) {
                if (s_diag.sdk_joined) {
                    /* Successfully rejoined previous network using persistent datasets from zb_storage */
                    s_net_state = ZB_NET_STATE_JOINED;
                    s_diag.net_state = s_net_state;
                    s_diag.rejoin_attempt_count = 0;
                    refresh_network_metadata();
                    set_network_ready(true);
                    ESP_LOGI(TAG, "Successfully rejoined network after reboot. Publisher full sync triggered.");
                } else {
                    /* Inconsistency guard: status success but dev_joined is false */
                    ESP_LOGW(TAG, "DEVICE_REBOOT status SUCCESS but ezb_bdb_dev_joined() is false! Retaining ready=false.");
                    s_net_state = ZB_NET_STATE_REJOINING;
                    s_diag.net_state = s_net_state;
                    set_network_ready(false);
                }
            } else {
                /* Rejoin failed or in progress */
                s_net_state = ZB_NET_STATE_REJOINING;
                s_diag.net_state = s_net_state;
                set_network_ready(false);
                ESP_LOGW(TAG, "Rejoin after reboot unsuccessful (Status: %s). Bounded retry scheduled.",
                         zigbee_native_bdb_status_to_str(status));

                /* Schedule bounded backoff retry via FreeRTOS timer */
                if (s_diag.rejoin_attempt_count < MAX_REJOIN_ATTEMPTS) {
                    s_diag.rejoin_attempt_count++;
                    if (s_rejoin_timer != NULL) {
                        xTimerStart(s_rejoin_timer, 0);
                    }
                } else {
                    ESP_LOGW(TAG, "Max rejoin attempts (%d) reached. Entering idle degraded state.", MAX_REJOIN_ATTEMPTS);
                    s_net_state = ZB_NET_STATE_COMMISSION_FAILED;
                    s_diag.net_state = s_net_state;
                }
            }
            return true;
        }

        case EZB_BDB_SIGNAL_STEERING: {
            const ezb_bdb_signal_simple_params_t *params = (const ezb_bdb_signal_simple_params_t *)ezb_app_signal_get_params(app_signal);
            uint8_t status = params ? params->status : 0xFF;
            s_diag.last_bdb_status = status;
            s_diag.factory_new = ezb_bdb_is_factory_new();
            s_diag.sdk_joined = ezb_bdb_dev_joined();

            ESP_LOGI(TAG, "Signal: %s (0x%04X) -- Network Steering finished (Status: %s / 0x%02X, Joined: %d).",
                     sig_str ? sig_str : "STEERING",
                     zigbee_native_bdb_status_to_str(status), status, s_diag.sdk_joined);

            if (status == EZB_BDB_STATUS_SUCCESS) {
                if (s_diag.sdk_joined) {
                    s_net_state = ZB_NET_STATE_JOINED;
                    s_diag.net_state = s_net_state;
                    s_diag.rejoin_attempt_count = 0;
                    refresh_network_metadata();
                    set_network_ready(true);
                    ESP_LOGI(TAG, "Successfully joined Zigbee network! Publisher full sync triggered.");
                } else {
                    ESP_LOGW(TAG, "STEERING status SUCCESS but ezb_bdb_dev_joined() is false! Retaining ready=false.");
                    s_net_state = ZB_NET_STATE_COMMISSION_FAILED;
                    s_diag.net_state = s_net_state;
                    set_network_ready(false);
                }
            } else {
                s_net_state = s_diag.factory_new ? ZB_NET_STATE_FACTORY_NEW_IDLE : ZB_NET_STATE_COMMISSION_FAILED;
                s_diag.net_state = s_net_state;
                set_network_ready(false);
                ESP_LOGW(TAG, "Network steering failed (Status: %s). No network joined.",
                         zigbee_native_bdb_status_to_str(status));
            }
            return true;
        }

        case EZB_ZDO_SIGNAL_LEAVE: {
            const ezb_zdo_signal_leave_params_t *params = (const ezb_zdo_signal_leave_params_t *)ezb_app_signal_get_params(app_signal);
            ezb_zdo_leave_type_t leave_type = params ? params->leave_type : EZB_ZDO_LEAVE_TYPE_RESET;

            ESP_LOGI(TAG, "Signal: %s (0x%04X) -- Device network leave (Type: %s).",
                     sig_str ? sig_str : "LEAVE",
                     (leave_type == EZB_ZDO_LEAVE_TYPE_RESET) ? "RESET" : "REJOIN");

            set_network_ready(false);
            invalidate_network_metadata();

            if (leave_type == EZB_ZDO_LEAVE_TYPE_RESET) {
                s_net_state = ZB_NET_STATE_FACTORY_NEW_IDLE;
                s_diag.net_state = s_net_state;
            } else if (leave_type == EZB_ZDO_LEAVE_TYPE_REJOIN) {
                s_net_state = ZB_NET_STATE_REJOINING;
                s_diag.net_state = s_net_state;
            }
            return true;
        }

        case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
            const ezb_zdo_signal_leave_indication_params_t *params = (const ezb_zdo_signal_leave_indication_params_t *)ezb_app_signal_get_params(app_signal);
            if (params != NULL) {
                ezb_shortaddr_t own_addr = ezb_get_short_address();
                if (params->short_addr != own_addr) {
                    /* Child or neighbor device left -- Conductor's own network membership is UNCHANGED */
                    ESP_LOGI(TAG, "Signal: LEAVE_INDICATION -- Peer device 0x%04X left network. Own state unchanged.",
                             params->short_addr);
                    return true;
                }
            }
            ESP_LOGW(TAG, "Signal: LEAVE_INDICATION for own address received.");
            return true;
        }

        case EZB_NWK_SIGNAL_NETWORK_STATUS: {
            const ezb_nwk_signal_network_status_params_t *params = (const ezb_nwk_signal_network_status_params_t *)ezb_app_signal_get_params(app_signal);
            uint8_t status = params ? params->status : 0xFF;
            ezb_shortaddr_t addr = params ? params->network_addr : 0xFFFF;
            s_diag.last_nwk_status = status;

            ESP_LOGW(TAG, "Signal: %s (0x%04X) -- Network status notification (Status: 0x%02X, Addr: 0x%04X). Non-destructive.",
                     sig_str ? sig_str : "NETWORK_STATUS", (unsigned int)sig_type, status, addr);
            return true;
        }

        case EZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT: {
            s_diag.sdk_joined = ezb_bdb_dev_joined();
            ESP_LOGW(TAG, "Signal: %s (0x%04X) -- Neighbor table links expired (SDK Joined: %d). Recording diagnostic; non-destructive.",
                     sig_str ? sig_str : "NO_ACTIVE_LINKS_LEFT", (unsigned int)sig_type, s_diag.sdk_joined);
            /* Conservative policy: Do NOT erase credentials, do NOT local-reset, do NOT start steering.
             * If stack reports device is still joined, do not flap network_ready. */
            if (!s_diag.sdk_joined) {
                set_network_ready(false);
                s_net_state = ZB_NET_STATE_REJOINING;
                s_diag.net_state = s_net_state;
            }
            return true;
        }

        case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
            const ezb_zdo_signal_device_annce_params_t *params = (const ezb_zdo_signal_device_annce_params_t *)ezb_app_signal_get_params(app_signal);
            if (params != NULL) {
                ESP_LOGD(TAG, "Signal: DEVICE_ANNCE -- Device 0x%04X announced on network.", params->short_addr);
            }
            return true;
        }

        default:
            ESP_LOGD(TAG, "Signal: %s (0x%04X) -- Unhandled lifecycle signal",
                     sig_str ? sig_str : "UNKNOWN", (unsigned int)sig_type);
            return false;
    }
}

/**
 * @brief Callback executed in Zigbee task context to start network steering.
 */
static void start_steering_in_stack_ctx(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Executing network steering in Zigbee task context...");
    s_net_state = ZB_NET_STATE_STEERING;
    s_diag.net_state = s_net_state;
    set_network_ready(false);

    ezb_err_t err = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    if (err != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "ezb_bdb_start_top_level_commissioning(STEERING) failed: %d", (int)err);
        s_net_state = s_diag.factory_new ? ZB_NET_STATE_FACTORY_NEW_IDLE : ZB_NET_STATE_COMMISSION_FAILED;
        s_diag.net_state = s_net_state;
    }
}

/**
 * @brief Callback executed in Zigbee task context to attempt rejoin.
 */
static void trigger_rejoin_in_stack_ctx(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Executing BDB initialization/rejoin retry in Zigbee task context (Attempt %d)...",
             s_diag.rejoin_attempt_count);
    ezb_err_t err = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
    if (err != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "ezb_bdb_start_top_level_commissioning(INITIALIZATION) retry failed: %d", (int)err);
    }
}

/**
 * @brief FreeRTOS timer callback for non-blocking rejoin backoff.
 */
static void rejoin_retry_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    esp_zigbee_task_queue_post(trigger_rejoin_in_stack_ctx, NULL);
}

esp_err_t zigbee_native_storage_init(void)
{
    ESP_LOGI(TAG, "Initializing dedicated Zigbee NVS storage partition: '%s'",
             Z906_ZB_STORAGE_PARTITION_NAME);

    esp_err_t err = nvs_flash_init_partition(Z906_ZB_STORAGE_PARTITION_NAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize partition '%s': %s (0x%x). "
                      "Credentials preserved; will NOT auto-erase partition.",
                 Z906_ZB_STORAGE_PARTITION_NAME, esp_err_to_name(err), err);
        return err;
    }

    ESP_LOGI(TAG, "Dedicated storage partition '%s' initialized successfully.",
             Z906_ZB_STORAGE_PARTITION_NAME);
    return ESP_OK;
}

esp_err_t zigbee_native_stack_init(void)
{
    s_stack_state = ZB_STACK_STATE_INITIALIZING;
    s_net_state = ZB_NET_STATE_UNINITIALIZED;
    s_diag.net_state = s_net_state;

    /* Initialize static FreeRTOS timer for bounded rejoin backoff */
    if (s_rejoin_timer == NULL) {
        s_rejoin_timer = xTimerCreateStatic(
            "zb_rejoin_tmr",
            pdMS_TO_TICKS(15000), /* 15 second backoff between attempts */
            pdFALSE,              /* One-shot timer */
            NULL,
            rejoin_retry_timer_cb,
            &s_rejoin_timer_buffer
        );
    }

    /* 1. Initialize dedicated Zigbee NVS storage */
    esp_err_t err = zigbee_native_storage_init();
    if (err != ESP_OK) {
        s_stack_state = ZB_STACK_STATE_FAILED;
        return err;
    }

    /* 2. Configure native stack platform & device parameters */
    esp_zigbee_config_t zb_config = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_ROUTER,
            .install_code_policy = false,
            .zczr_config = {
                .max_children = Z906_ZB_MAX_CHILDREN,
            },
        },
        .platform_config = {
            .storage_partition_name = Z906_ZB_STORAGE_PARTITION_NAME,
            .radio_config = {
                .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,
            },
        },
    };

    ESP_LOGI(TAG, "Zigbee Config: Role=ROUTER, Storage='%s', Radio=NATIVE, MaxChildren=%d, InstallCodePolicy=false",
             zb_config.platform_config.storage_partition_name,
             zb_config.device_config.zczr_config.max_children);

    /* 3. Initialize core Zigbee stack */
    err = esp_zigbee_init(&zb_config);
    if (err != ESP_OK) {
        s_stack_state = ZB_STACK_STATE_FAILED;
        ESP_LOGE(TAG, "esp_zigbee_init failed: %s (0x%x)", esp_err_to_name(err), err);
        return err;
    }

    /* 3.1 Explicitly configure Router rejoin required on BDB initialization (Directive 10) */
    ezb_bdb_set_router_rejoin_required(true);

    /* 4. Register complete Zigbee device model (EP 1, 2, 3, 4, 11, 12, 13) */
    err = zigbee_device_model_register();
    if (err != ESP_OK) {
        s_stack_state = ZB_STACK_STATE_FAILED;
        ESP_LOGE(TAG, "zigbee_device_model_register failed: %s (0x%x)", esp_err_to_name(err), err);
        return err;
    }

    /* 4.1 Initialize Non-Blocking ZCL Command Ingress & Hardware Bridge (Phase 4) */
    err = zigbee_command_ingress_init();
    if (err != ESP_OK) {
        s_stack_state = ZB_STACK_STATE_FAILED;
        ESP_LOGE(TAG, "zigbee_command_ingress_init failed: %s (0x%x)", esp_err_to_name(err), err);
        return err;
    }

    /* 4.2 Initialize Verified State Publishing & Changed-Only ZCL Reporting (Phase 5) */
    err = zigbee_state_publisher_init();
    if (err != ESP_OK) {
        s_stack_state = ZB_STACK_STATE_FAILED;
        ESP_LOGE(TAG, "zigbee_state_publisher_init failed: %s (0x%x)", esp_err_to_name(err), err);
        return err;
    }

    /* 4.3 Initialize Native Zigbee OTA Transport Adapter (Phase 7) */
    err = ota_zigbee_transport_init();
    if (err != ESP_OK) {
        s_stack_state = ZB_STACK_STATE_FAILED;
        ESP_LOGE(TAG, "ota_zigbee_transport_init failed: %s (0x%x)", esp_err_to_name(err), err);
        return err;
    }

    /* 5. Register application lifecycle signal handler */
    ezb_err_t ezb_err = ezb_app_signal_add_handler(zigbee_native_signal_handler);
    if (ezb_err != EZB_ERR_NONE) {
        s_stack_state = ZB_STACK_STATE_FAILED;
        ESP_LOGE(TAG, "ezb_app_signal_add_handler failed: %d", (int)ezb_err);
        return ESP_FAIL;
    }

    s_stack_state = ZB_STACK_STATE_READY;
    s_diag.stack_state = s_stack_state;
    ESP_LOGI(TAG, "Native Zigbee SDK v2 initialized successfully (State: READY).");
    return ESP_OK;
}

esp_err_t zigbee_native_stack_run(void)
{
    if (s_stack_state != ZB_STACK_STATE_READY) {
        ESP_LOGE(TAG, "Cannot run Zigbee stack in state: %s",
                 zigbee_native_stack_state_to_str(s_stack_state));
        return ESP_ERR_INVALID_STATE;
    }

    /* Start the stack with autostart=false to trigger SKIP_STARTUP signal for controlled BDB initialization */
    ESP_LOGI(TAG, "Starting native Zigbee stack (autostart=false, BDB initialization via SKIP_STARTUP)...");
    esp_err_t err = esp_zigbee_start(false);
    if (err != ESP_OK) {
        s_stack_state = ZB_STACK_STATE_FAILED;
        s_diag.stack_state = s_stack_state;
        ESP_LOGE(TAG, "esp_zigbee_start failed: %s (0x%x)", esp_err_to_name(err), err);
        return err;
    }

    s_stack_state = ZB_STACK_STATE_RUNNING;
    s_diag.stack_state = s_stack_state;
    ESP_LOGI(TAG, "Launching native Zigbee mainloop (blocking)...");

    /* Enter blocking main loop */
    err = esp_zigbee_launch_mainloop();

    /* Mainloop returned -- either stopped via esp_zigbee_stop() or stack error */
    ESP_LOGW(TAG, "Native Zigbee mainloop exited with result: %s (0x%x)", esp_err_to_name(err), err);
    s_stack_state = (err == ESP_OK) ? ZB_STACK_STATE_STOPPED : ZB_STACK_STATE_FAILED;
    s_diag.stack_state = s_stack_state;

    /* Clean deinitialization */
    esp_err_t deinit_err = esp_zigbee_deinit();
    if (deinit_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_deinit failed: %s (0x%x)", esp_err_to_name(deinit_err), deinit_err);
    } else {
        ESP_LOGI(TAG, "esp_zigbee_deinit completed cleanly.");
    }

    return err;
}

esp_err_t zigbee_native_stack_stop(void)
{
    if (s_stack_state != ZB_STACK_STATE_RUNNING) {
        ESP_LOGW(TAG, "Stack not running, cannot stop (current state: %s)",
                 zigbee_native_stack_state_to_str(s_stack_state));
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Requesting native Zigbee stack stop...");
    return esp_zigbee_stop();
}

zb_stack_state_t zigbee_native_stack_get_state(void)
{
    return s_stack_state;
}

const char *zigbee_native_stack_state_to_str(zb_stack_state_t state)
{
    switch (state) {
        case ZB_STACK_STATE_UNINITIALIZED: return "UNINITIALIZED";
        case ZB_STACK_STATE_INITIALIZING:  return "INITIALIZING";
        case ZB_STACK_STATE_READY:         return "READY";
        case ZB_STACK_STATE_RUNNING:       return "RUNNING";
        case ZB_STACK_STATE_FAILED:        return "FAILED";
        case ZB_STACK_STATE_STOPPED:       return "STOPPED";
        default:                           return "UNKNOWN";
    }
}

const char *zigbee_native_net_state_to_str(zb_network_lifecycle_state_t state)
{
    switch (state) {
        case ZB_NET_STATE_UNINITIALIZED:       return "UNINITIALIZED";
        case ZB_NET_STATE_FACTORY_NEW_IDLE:    return "FACTORY_NEW_IDLE";
        case ZB_NET_STATE_STEERING:            return "STEERING";
        case ZB_NET_STATE_JOINED:              return "JOINED";
        case ZB_NET_STATE_REJOINING:           return "REJOINING";
        case ZB_NET_STATE_LEAVING_RESET:       return "LEAVING_RESET";
        case ZB_NET_STATE_COMMISSION_FAILED:   return "COMMISSION_FAILED";
        default:                               return "UNKNOWN";
    }
}

const char *zigbee_native_bdb_status_to_str(uint8_t status)
{
    switch (status) {
        case EZB_BDB_STATUS_SUCCESS:                 return "SUCCESS";
        case EZB_BDB_STATUS_IN_PROGRESS:             return "IN_PROGRESS";
        case EZB_BDB_STATUS_NOT_AA_CAPABLE:          return "NOT_AA_CAPABLE";
        case EZB_BDB_STATUS_NO_NETWORK:              return "NO_NETWORK";
        case EZB_BDB_STATUS_TARGET_FAILURE:          return "TARGET_FAILURE";
        case EZB_BDB_STATUS_FORMATION_FAILURE:       return "FORMATION_FAILURE";
        case EZB_BDB_STATUS_NO_IDENTIFY_QUERY_RESPONSE: return "NO_IDENTIFY_QUERY_RESPONSE";
        case EZB_BDB_STATUS_BINDING_TABLE_FULL:      return "BINDING_TABLE_FULL";
        case EZB_BDB_STATUS_NO_SCAN_RESPONSE:        return "NO_SCAN_RESPONSE";
        case EZB_BDB_STATUS_NOT_PERMITTED:           return "NOT_PERMITTED";
        case EZB_BDB_STATUS_TCLK_EX_FAILURE:         return "TCLK_EX_FAILURE";
        case EZB_BDB_STATUS_NOT_ON_A_NETWORK:        return "NOT_ON_A_NETWORK";
        case EZB_BDB_STATUS_ON_A_NETWORK:            return "ON_A_NETWORK";
        case EZB_BDB_STATUS_CANCELLED:               return "CANCELLED";
        case EZB_BDB_STATUS_DEV_ANNCE_SEND_FAILURE:  return "DEV_ANNCE_SEND_FAILURE";
        default:                                     return "UNKNOWN_STATUS";
    }
}

zb_request_status_t zigbee_native_request_pair(void)
{
    if (s_stack_state != ZB_STACK_STATE_RUNNING) {
        ESP_LOGW(TAG, "Cannot start pairing: Zigbee stack is not running (Current: %s)",
                 zigbee_native_stack_state_to_str(s_stack_state));
        return ZB_REQ_FAILED_TO_START;
    }

    if (s_net_state == ZB_NET_STATE_STEERING || s_net_state == ZB_NET_STATE_LEAVING_RESET) {
        ESP_LOGW(TAG, "Commissioning or reset operation already active (Current: %s)",
                 zigbee_native_net_state_to_str(s_net_state));
        return ZB_REQ_BUSY;
    }

    bool factory_new = ezb_bdb_is_factory_new();
    bool joined = ezb_bdb_dev_joined();

    ESP_LOGI(TAG, "Pair request received. Membership truth: factory_new=%d, joined=%d",
             factory_new, joined);

    /* Membership Truth Decision Matrix:
     * 1. factory_new=true, joined=false: Clean/factory-new -> Start steering directly
     * 2. factory_new=false, joined=true: Currently joined -> Safe Re-pair Strategy B (persist intent + reset + restart)
     * 3. factory_new=false, joined=false: Old datasets/disconnected -> Strategy B (persist intent + reset + restart)
     * 4. factory_new=true, joined=true: Inconsistent -> Log error diagnostic, do not guess
     */
    if (factory_new && joined) {
        ESP_LOGE(TAG, "Inconsistent Zigbee state detected: factory_new=true AND joined=true! Rejecting pairing request.");
        return ZB_REQ_FAILED_TO_START;
    }

    if (!factory_new) {
        /* Disconnected re-pair OR Joined re-pair:
         * Strategy B (Zigbee factory reset + restart + one-shot user intent).
         * This eliminates the historical same-session local reset assertion risk (Espressif #832)
         * and guarantees deterministic dataset clearing regardless of whether coordinator is reachable.
         */
        ESP_LOGI(TAG, "Existing network history detected (factory_new=false, joined=%d). "
                      "Executing deterministic re-pair flow (Strategy B: one-shot intent + factory reset + restart)...",
                 joined);

        /* 1. Persist one-shot user intent */
        if (!nvs_manager_set_pair_after_reset(true)) {
            ESP_LOGE(TAG, "Failed to persist one-shot pair intent to NVS. Aborting re-pair.");
            return ZB_REQ_FAILED_TO_START;
        }

        /* 2. Gate reporting off immediately */
        set_network_ready(false);

        /* 3. Invoke native stack factory reset (clears zb_storage datasets and restarts board) */
        ESP_LOGI(TAG, "Calling esp_zigbee_factory_reset() to clear datasets and restart into factory-new steering...");
        esp_zigbee_factory_reset();

        /* esp_zigbee_factory_reset initiates restart and does not return */
        return ZB_REQ_ACCEPTED;
    }

    /* Device is factory-new and unjoined: start network steering directly */
    ESP_LOGI(TAG, "Posting network steering request to Zigbee task queue...");
    esp_err_t err = esp_zigbee_task_queue_post(start_steering_in_stack_ctx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post steering task: %s (0x%x)", esp_err_to_name(err), err);
        return ZB_REQ_FAILED_TO_START;
    }

    return ZB_REQ_ACCEPTED;
}

void zigbee_native_request_reset(void)
{
    ESP_LOGI(TAG, "Zigbee-only factory reset requested.");

    /* 1. Suppress reporting immediately */
    set_network_ready(false);

    /* 2. Clear legacy app Zigbee cache if present */
    nvs_manager_clear_zb_net_info();

    /* 3. Clear one-shot pair intent if present */
    nvs_manager_clear_pair_after_reset();

    /* 4. Invoke native stack factory reset (clears zb_storage datasets and restarts) */
    ESP_LOGI(TAG, "Calling esp_zigbee_factory_reset() to clear Zigbee datasets and restart...");
    esp_zigbee_factory_reset();
}

void zigbee_native_get_diagnostics(zb_network_diagnostics_t *diag)
{
    if (diag == NULL) {
        return;
    }

    /* Refresh snapshot fields */
    s_diag.stack_state = s_stack_state;
    s_diag.net_state = s_net_state;
    s_diag.factory_new = ezb_bdb_is_factory_new();
    s_diag.sdk_joined = ezb_bdb_dev_joined();

    if (s_net_state == ZB_NET_STATE_JOINED && s_diag.sdk_joined) {
        refresh_network_metadata();
    }

    memcpy(diag, &s_diag, sizeof(zb_network_diagnostics_t));
}

bool zigbee_native_is_network_ready(void)
{
    return s_diag.network_ready;
}

/* =========================================================================
 * Zigbee Task — owned by communication/zigbee (zigbee_native_stack.c)
 *
 * Ownership rationale: The Zigbee task initializes and runs the ESP Zigbee SDK
 * blocking mainloop. Both operations are Zigbee-specific concerns. Lifecycle
 * belongs here. The system initializer only calls zigbee_native_stack_start().
 *
 * WHY INIT INSIDE TASK: esp_zigbee_launch_mainloop() blocks indefinitely by
 * design in ESP Zigbee SDK v2. The function never returns under normal operation.
 * Therefore the stack must be initialized and started within this dedicated task.
 *
 * TWDT: This task does NOT register with the application TWDT because the
 * blocking mainloop cannot yield to call wdt_manager_reset(). The global TWDT
 * remains active for all other monitored tasks.
 * ========================================================================= */

#include "freertos/task.h"
#include "freertos/semphr.h"

/** @brief Stack depth in words for the blocking Zigbee mainloop task */
#define ZB_TASK_STACK_WORDS     (4096u / sizeof(StackType_t))

/** @brief FreeRTOS priority for the Zigbee task */
#define ZB_TASK_PRIORITY        3u

/** @brief Max ms to wait for zigbee_native_stack_init() to complete and signal STARTED.
 *  Zigbee init involves flash NVS reads and commissioning state machine setup.
 *  2000 ms is generous but bounded to prevent indefinite hangs. */
#define ZB_STARTUP_TIMEOUT_MS   2000u

static StackType_t  s_zb_stack[ZB_TASK_STACK_WORDS];
static StaticTask_t s_zb_tcb;

/** @brief Private startup binary semaphore — given after zigbee_native_stack_init() completes */
static SemaphoreHandle_t s_started_sem    = NULL;
static StaticSemaphore_t s_started_sem_buf;

static zb_native_init_fn_t s_test_init_fn = NULL;
static zb_native_run_fn_t  s_test_run_fn = NULL;
static uint32_t            s_startup_timeout_ms = ZB_STARTUP_TIMEOUT_MS;
static esp_err_t           s_init_result = ESP_FAIL;

static bool s_task_started_confirmed = false;

/**
 * @brief Zigbee task body (FSR-04 fix: private semaphore startup handshake).
 *
 * No EventGroup gate. Calls zigbee_native_stack_init() first.
 * Signals s_started_sem after init completes (with s_init_result recording success/failure),
 * so the caller of zigbee_native_stack_start() is guaranteed the stack init was executed
 * and can distinguish success from failure.
 *
 * @param pvParameters Unused.
 */
static void prv_zigbee_task(void *pvParameters)
{
    (void)pvParameters; /* No gate handle — startup handled by private semaphore */

    ESP_LOGI(TAG, "zigbee_task running (Priority %u, blocking mainloop)", ZB_TASK_PRIORITY);

    esp_err_t err = (s_test_init_fn != NULL) ? s_test_init_fn() : zigbee_native_stack_init();
    s_init_result = err;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "zigbee_native_stack_init failed: %s (0x%x). Task exiting.",
                 esp_err_to_name(err), err);
        /* Signal caller immediately with completion failure */
        xSemaphoreGive(s_started_sem);
        vTaskDelete(NULL);
        return;
    }

    /* Signal start() that init succeeded and the blocking mainloop is about to start */
    xSemaphoreGive(s_started_sem);

    /* Allow caller to record start confirmation or handle timeout */
    vTaskDelay(pdMS_TO_TICKS(10));

    if (!s_task_started_confirmed) {
        ESP_LOGW(TAG, "zigbee_task startup was not confirmed by caller (timed out/aborted). Exiting.");
        vTaskDelete(NULL);
        return;
    }

    /* This call blocks until esp_zigbee_stop() is called */
    err = (s_test_run_fn != NULL) ? s_test_run_fn() : zigbee_native_stack_run();
    ESP_LOGW(TAG, "zigbee_native_stack_run returned: %s (0x%x)",
             esp_err_to_name(err), err);

    vTaskDelete(NULL);
}

bool zigbee_native_stack_start(void)
{
    /* Create the binary startup semaphore — starts empty;
     * the task gives it after zigbee_native_stack_init() completes. */
    s_started_sem = xSemaphoreCreateBinaryStatic(&s_started_sem_buf);
    s_init_result = ESP_FAIL;
    s_task_started_confirmed = false;

    TaskHandle_t handle = xTaskCreateStatic(
        prv_zigbee_task,
        "zigbee_task",
        ZB_TASK_STACK_WORDS,
        NULL,               /* pvParameters: no gate handle needed */
        ZB_TASK_PRIORITY,
        s_zb_stack,
        &s_zb_tcb
    );

    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to create zigbee_task");
        return false;
    }

    /* Block until the task signals init completion or timeout expires. */
    BaseType_t got = xSemaphoreTake(s_started_sem,
                                    pdMS_TO_TICKS(s_startup_timeout_ms));
    if (got != pdTRUE) {
        ESP_LOGE(TAG, "zigbee_task did not signal STARTED within %u ms (timing out, deleting task)",
                 s_startup_timeout_ms);
        vTaskDelete(handle);
        return false;
    }

    if (s_init_result != ESP_OK) {
        ESP_LOGE(TAG, "zigbee_task init failed with error: %s (0x%x)",
                 esp_err_to_name(s_init_result), s_init_result);
        return false;
    }

    s_task_started_confirmed = true;
    ESP_LOGI(TAG, "zigbee_task STARTED (Priority %u, stack %u words, init confirmed)",
             ZB_TASK_PRIORITY, ZB_TASK_STACK_WORDS);
    return true;
}

void zigbee_native_stack_set_init_fn_for_test(zb_native_init_fn_t fn)
{
    s_test_init_fn = fn;
}

void zigbee_native_stack_set_run_fn_for_test(zb_native_run_fn_t fn)
{
    s_test_run_fn = fn;
}

void zigbee_native_stack_set_timeout_for_test(uint32_t timeout_ms)
{
    s_startup_timeout_ms = timeout_ms;
}

void zigbee_native_stack_reset_for_test(void)
{
    s_test_init_fn = NULL;
    s_test_run_fn = NULL;
    s_startup_timeout_ms = ZB_STARTUP_TIMEOUT_MS;
    s_init_result = ESP_FAIL;
    s_task_started_confirmed = false;
}
