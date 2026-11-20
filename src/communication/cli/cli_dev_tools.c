/**
 * @file cli_dev_tools.c
 * @brief Implementation of Developer Tools diagnostic commands and dispatching logic.
 *
 * Encapsulates low-level hardware diagnostics, Zigbee network testing,
 * Task Watchdog inspection, logging filter management, OTA lifecycle inspection,
 * and factory reset commands.
 *
 * All UART telemetry requests route through the centralized telemetry service
 * and state manager, eliminating direct UART race conditions.
 */

#include "cli_dev_tools.h"
#include "z906_state.h"
#include "z906_codec.h"
#include "uart_hal.h"
#include "nvs_manager.h"
#include "zigbee_native_stack.h"
#include "debug_console.h"
#include "ota_client.h"
#include "ota_zigbee_transport.h"
#include "ota_health.h"
#include "telemetry.h"
#include "config.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_zigbee.h"
#include <stdio.h>
#include <string.h>

/**
 * @brief Displays the Developer Tools hierarchy and available commands.
 */
void cli_dev_tools_show_help(void)
{
    printf("\n=== Developer Tools Hierarchy ===\n");
    printf("Logging Subsystem:\n");
    printf("  dev log_level [level]              - View or set global ESP-IDF log level\n");
    printf("  dev log_filter [tag] [level|clear] - Manage per-module tag log overrides\n");
    printf("\nDiagnostics Subsystem:\n");
    printf("  dev debug_status                   - Print uptime, heap statistics & system health\n");
    printf("  dev wdt_status                     - Print TWDT task health & reset reason analysis\n");
    printf("\nZigbee OTA Firmware Subsystem:\n");
    printf("  dev ota_status                     - Inspect running/target slots, OTA state & health\n");
    printf("  dev ota_query                      - Dispatch Query Next Image Request to coordinator\n");
    printf("  dev ota_rollback                   - Manually trigger immediate rollback to previous slot\n");
    printf("\nProtocols & Telemetry Subsystem:\n");
    printf("  dev temp                           - Direct query of amplifier temperature (0x25)\n");
    printf("  dev idle                           - Direct query of amplifier idle timer (0x31)\n");
    printf("  dev zb_status                      - View native Zigbee lifecycle & RAM diagnostic snapshot\n");
    printf("  dev zb_pair                        - Start Zigbee pairing / steering (re-pairs if joined)\n");
    printf("\nCritical Controls (Destructive):\n");
    printf("  dev zb_reset                       - Clears Zigbee pairing/network and restarts device (preserves non-Zigbee settings)\n");
    printf("  dev factory_reset CONFIRM          - FULL factory reset: erases ALL persistent settings (app NVS + Zigbee datasets)\n");
    printf("    (running 'dev factory_reset' without CONFIRM will show this safety warning)\n");
    printf("=================================\n\n");
}

/**
 * @brief Displays detailed Zigbee OTA diagnostics, partition slots, and rollback status.
 */
static void cmd_dev_ota_status(void)
{
    ota_diagnostics_t diag;
    ota_client_get_diagnostics(&diag);

    ota_health_criteria_t crit;
    ota_health_get_criteria(&crit);

    printf("\n=== Zigbee OTA Upgrade Diagnostics ===\n");
    printf("Firmware Version:    %s (32-bit: 0x%08lX)\n", Z906_FW_VERSION_STR, (unsigned long)diag.current_file_version);
    printf("Manufacturer Code:   0x%04X (Espressif CSA)\n", Z906_OTA_MANUFACTURER_CODE);
    printf("Image Type:          0x%04X\n", Z906_OTA_IMAGE_TYPE);
    printf("Hardware Version:    absent (no canonical source; fc=0x00 in Query Next Image)\n");
    printf("--------------------------------------\n");
    printf("OTA State:           %s\n", ota_client_state_str(diag.current_state));
    printf("Last Result:         %s\n", ota_client_result_str(diag.last_result));
    printf("Running Partition:   %s\n", diag.running_partition_name);
    printf("Target A/B Slot:     %s\n", diag.target_partition_name);
    printf("Transfer Progress:   %d%% (%lu / %lu bytes)\n",
           diag.progress_percent, (unsigned long)diag.bytes_downloaded, (unsigned long)diag.total_image_size);
    printf("Target File Version: 0x%08lX\n", (unsigned long)diag.target_file_version);
    ota_health_recovery_state_t recovery = ota_health_get_recovery_state();
    printf("Pending Validation:  %s\n", diag.is_pending_verify ? "YES (not yet confirmed)" : "NO");
    printf("Recovery State:      %s\n", ota_health_recovery_state_str(recovery));
    if (recovery == OTA_HEALTH_RECOVERY_NO_FALLBACK ||
        recovery == OTA_HEALTH_RECOVERY_ROLLBACK_ERROR) {
        printf("WARNING: OTA image is unconfirmed; manual recovery decision required.\n");
    } else if (recovery == OTA_HEALTH_RECOVERY_ROLLBACK_QUEUED) {
        printf("WARNING: Rollback queued; firmware validation will abort before SDK commit.\n");
    } else if (recovery == OTA_HEALTH_RECOVERY_REQUEST_TOO_LATE) {
        printf("WARNING: Rollback requested after software OTA admission; in-flight SDK commit cannot be undone.\n");
    }
    printf("Rollback Possible:   %s\n", diag.is_rollback_possible ? "YES" : "NO");
    printf("Previous Rollback:   %s\n", ota_health_was_rollback_detected() ? "DETECTED ON BOOT" : "NONE");
    printf("Health Criteria:     Sched=%d, WDT=%d, NVS=%d, ZB=%d, Z906=%d\n",
           crit.scheduler_healthy, crit.watchdog_healthy, crit.nvs_healthy, crit.zigbee_healthy, crit.z906_comm_healthy);
    printf("======================================\n\n");
}

/**
 * @brief Thread-safe amplifier temperature query command (0x25).
 * Dispatches query via telemetry manager and observes confirmed state store.
 */
static void cmd_dev_temp(void)
{
    printf("[INFO] Querying amplifier temperature telemetry (0x25)...\n");

    telemetry_stat_t stat_before;
    telemetry_get_stats(TELEMETRY_QUERY_TEMP, &stat_before);

    if (!telemetry_dispatch_query(Z906_CMD_READ_TEMP)) {
        printf("[WARNING] UART bus busy or verification in progress. Reading from state store:\n");
        z906_state_t st;
        z906_state_get(&st);
        if (st.temp_valid) {
            printf("[INFO] Confirmed Temperatures: T1=%d C, T2=%d C, T3=%d C\n",
                   (int)st.temp_celsius[0], (int)st.temp_celsius[1], (int)st.temp_celsius[2]);
        } else {
            printf("[WARNING] No temperature telemetry available in state cache yet.\n");
        }
        return;
    }

    /* Wait up to 500 ms for uart_rx_task / FSM to ingest and decode the response */
    int waited_ms = 0;
    bool updated = false;
    while (waited_ms < 500) {
        vTaskDelay(pdMS_TO_TICKS(25));
        waited_ms += 25;
        telemetry_stat_t stat_now;
        telemetry_get_stats(TELEMETRY_QUERY_TEMP, &stat_now);
        if (stat_now.success_count > stat_before.success_count) {
            updated = true;
            break;
        }
    }

    z906_state_t st;
    z906_state_get(&st);

    if (updated && st.temp_valid) {
        printf("[SUCCESS] Confirmed Amplifier Temperatures (Latency: %d ms):\n", waited_ms);
        printf("  Sensor 1: %d C\n", (int)st.temp_celsius[0]);
        printf("  Sensor 2: %d C\n", (int)st.temp_celsius[1]);
        printf("  Sensor 3: %d C (Power Supply Transformer)\n", (int)st.temp_celsius[2]);
    } else if (st.temp_valid) {
        printf("[INFO] Cached confirmed temperatures: T1=%d C, T2=%d C, T3=%d C\n",
               (int)st.temp_celsius[0], (int)st.temp_celsius[1], (int)st.temp_celsius[2]);
    } else {
        printf("[WARNING] No verified temperature response received within timeout.\n");
    }
}

/**
 * @brief Thread-safe amplifier idle timer query command (0x31).
 * Dispatches query via telemetry manager and observes confirmed state store.
 */
static void cmd_dev_idle(void)
{
    printf("[INFO] Querying amplifier idle timer telemetry (0x31)...\n");

    telemetry_stat_t stat_before;
    telemetry_get_stats(TELEMETRY_QUERY_IDLE, &stat_before);

    if (!telemetry_dispatch_query(Z906_CMD_READ_IDLE)) {
        printf("[WARNING] UART bus busy or verification in progress. Reading from state store:\n");
        z906_state_t st;
        z906_state_get(&st);
        if (st.idle_seconds > 0) {
            printf("[INFO] Confirmed Idle Time: %lu seconds (%lu minutes)\n",
                   (unsigned long)st.idle_seconds, (unsigned long)(st.idle_seconds / 60));
        } else {
            printf("[WARNING] No idle telemetry available in state cache yet.\n");
        }
        return;
    }

    /* Wait up to 500 ms for uart_rx_task / FSM to ingest and decode the response */
    int waited_ms = 0;
    bool updated = false;
    while (waited_ms < 500) {
        vTaskDelay(pdMS_TO_TICKS(25));
        waited_ms += 25;
        telemetry_stat_t stat_now;
        telemetry_get_stats(TELEMETRY_QUERY_IDLE, &stat_now);
        if (stat_now.success_count > stat_before.success_count) {
            updated = true;
            break;
        }
    }

    z906_state_t st;
    z906_state_get(&st);

    if (updated) {
        printf("[SUCCESS] Confirmed Amplifier Idle Time (Latency: %d ms):\n", waited_ms);
        printf("  Duration: %lu seconds (%lu minutes)\n",
               (unsigned long)st.idle_seconds, (unsigned long)(st.idle_seconds / 60));
    } else if (st.idle_seconds > 0) {
        printf("[INFO] Cached confirmed idle duration: %lu seconds (%lu minutes)\n",
               (unsigned long)st.idle_seconds, (unsigned long)(st.idle_seconds / 60));
    } else {
        printf("[WARNING] No verified idle duration response received within timeout.\n");
    }
}

/**
 * @brief Displays detailed native Zigbee network status from authoritative RAM diagnostic snapshot.
 *
 * In accordance with Phase 6:
 *   - Displays native stack lifecycle state.
 *   - Displays factory-new, network-ready, and commissioning state.
 *   - Displays confirmed PAN ID, Channel, Short Address, Ext PAN ID (or indicates None).
 *   - Strictly redacts all network security keys / secrets.
 */
static void cmd_dev_zb_status(void)
{
    zb_network_diagnostics_t diag;
    zigbee_native_get_diagnostics(&diag);

    printf("\n=== Zigbee Subsystem Status (Native SDK v2.0.4) ===\n");
    printf("Stack Lifecycle: %s\n", zigbee_native_stack_state_to_str(diag.stack_state));
    printf("Network State:   %s\n", zigbee_native_net_state_to_str(diag.net_state));
    printf("Factory New:     %s\n", diag.factory_new ? "YES" : "NO");
    printf("SDK Joined:      %s\n", diag.sdk_joined ? "YES (ezb_bdb_dev_joined)" : "NO");
    printf("Network Ready:   %s\n", diag.network_ready ? "YES (ZCL Reporting Enabled)" : "NO (Reports Suppressed)");
    printf("Last BDB Status: %s (0x%02X)\n", zigbee_native_bdb_status_to_str(diag.last_bdb_status), diag.last_bdb_status);
    if (diag.last_nwk_status != 0xFF) {
        printf("Last NWK Status: 0x%02X\n", diag.last_nwk_status);
    }

    if (diag.metadata_valid) {
        printf("PAN ID:          0x%04X\n", diag.pan_id);
        printf("Channel:         %d\n", diag.channel);
        printf("Short Address:   0x%04X\n", diag.short_addr);
        printf("Ext PAN ID:      %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
               diag.ext_pan_id[0], diag.ext_pan_id[1],
               diag.ext_pan_id[2], diag.ext_pan_id[3],
               diag.ext_pan_id[4], diag.ext_pan_id[5],
               diag.ext_pan_id[6], diag.ext_pan_id[7]);
    } else {
        printf("Network Params:  None (No active network joined / metadata invalid)\n");
    }

    if (diag.net_state == ZB_NET_STATE_REJOINING) {
        printf("Rejoin Attempts: %d\n", diag.rejoin_attempt_count);
    }
    printf("===================================================\n\n");
}

/**
 * @brief Native Zigbee pairing trigger command (`dev zb_pair`).
 *
 * Initiates network steering. If already joined or has existing datasets,
 * automatically executes deterministic re-pair (Strategy B: one-shot intent + factory reset + restart).
 */
static void cmd_dev_zb_pair(void)
{
    zb_network_diagnostics_t diag;
    zigbee_native_get_diagnostics(&diag);

    if (diag.net_state == ZB_NET_STATE_STEERING || diag.net_state == ZB_NET_STATE_LEAVING_RESET) {
        printf("[WARNING] Pairing/steering is already in progress (State: %s).\n",
               zigbee_native_net_state_to_str(diag.net_state));
        return;
    }

    if (!diag.factory_new || diag.sdk_joined) {
        printf("[NOTICE] Device has existing network history (FactoryNew=%s, Joined=%s).\n",
               diag.factory_new ? "YES" : "NO", diag.sdk_joined ? "YES" : "NO");
        printf("[INFO] Executing deterministic re-pair (clearing Zigbee datasets and restarting into pairing mode)...\n");
    } else {
        printf("[INFO] Starting Zigbee network steering (pairing mode)...\n");
    }

    zb_request_status_t status = zigbee_native_request_pair();
    switch (status) {
        case ZB_REQ_ACCEPTED:
            printf("[SUCCESS] Pairing request accepted. Observe 'dev zb_status' or logs for completion.\n");
            break;
        case ZB_REQ_BUSY:
            printf("[ERROR] Zigbee stack is busy with another commissioning operation.\n");
            break;
        case ZB_REQ_FAILED_TO_START:
        default:
            printf("[FAILED] Failed to schedule pairing request on Zigbee task.\n");
            break;
    }
}

/**
 * @brief Global logging level configuration command.
 */
static void cmd_dev_log_level(char *arg)
{
    if (arg) {
        esp_log_level_t lvl = debug_console_str_to_level(arg);
        debug_console_set_global_level(lvl);
        printf("[SUCCESS] Global log level set to: %s\n", debug_console_level_to_str(lvl));
    } else {
        printf("Current global log level: %s\n",
               debug_console_level_to_str(debug_console_get_global_level()));
        printf("Usage: dev log_level <none|error|warn|info|debug|verbose>\n");
    }
}

/**
 * @brief Tag-based logging filter configuration command.
 */
static void cmd_dev_log_filter(char *tag_arg, char *lvl_arg)
{
    if (tag_arg == NULL) {
        size_t count = debug_console_get_filter_count();
        printf("\nActive Module Log Filters (%zu / %d):\n", count, DEBUG_CONSOLE_MAX_TAG_FILTERS);
        if (count > 0) {
            for (size_t i = 0; i < count; i++) {
                char tag_buf[DEBUG_CONSOLE_TAG_MAX_LEN];
                esp_log_level_t lvl;
                if (debug_console_get_filter(i, tag_buf, sizeof(tag_buf), &lvl)) {
                    printf("  [%zu] %-18s -> %s\n", i + 1, tag_buf, debug_console_level_to_str(lvl));
                }
            }
        } else {
            printf("  (No tag overrides active; all tags follow global level: %s)\n",
               debug_console_level_to_str(debug_console_get_global_level()));
        }
        printf("Usage: dev log_filter <tag> <level> | dev log_filter clear\n\n");
    } else if (strcmp(tag_arg, "clear") == 0) {
        debug_console_clear_tag_filters();
        printf("[SUCCESS] Cleared all module tag filters. Restored global level: %s\n",
               debug_console_level_to_str(debug_console_get_global_level()));
    } else if (lvl_arg != NULL) {
        esp_log_level_t lvl = debug_console_str_to_level(lvl_arg);
        if (debug_console_set_tag_level(tag_arg, lvl)) {
            printf("[SUCCESS] Tag filter set: %s -> %s\n", tag_arg, debug_console_level_to_str(lvl));
        } else {
            printf("[FAILED] Could not set tag filter (table full or invalid tag)\n");
        }
    } else {
        esp_log_level_t lvl = debug_console_get_tag_level(tag_arg);
        printf("Log filter for [%s]: %s\n", tag_arg, debug_console_level_to_str(lvl));
    }
}

/**
 * @brief Checks if a given command name matches any known developer diagnostic tool.
 */
bool cli_dev_tools_is_dev_command(const char *cmd)
{
    if (!cmd) return false;
    return (strcmp(cmd, "temp") == 0 ||
            strcmp(cmd, "idle") == 0 ||
            strcmp(cmd, "zb_status") == 0 ||
            strcmp(cmd, "zb_pair") == 0 ||
            strcmp(cmd, "zb_reset") == 0 ||
            strcmp(cmd, "ota_status") == 0 ||
            strcmp(cmd, "ota_query") == 0 ||
            strcmp(cmd, "ota_rollback") == 0 ||
            strcmp(cmd, "log_level") == 0 ||
            strcmp(cmd, "log_filter") == 0 ||
            strcmp(cmd, "wdt_status") == 0 ||
            strcmp(cmd, "debug_status") == 0);
}

/**
 * @brief Processes a developer subcommand.
 */
void cli_dev_tools_process(char *subcmd)
{
    if (subcmd == NULL || strcmp(subcmd, "help") == 0) {
        cli_dev_tools_show_help();
        return;
    }

    if (strcmp(subcmd, "temp") == 0) {
        cmd_dev_temp();
    } else if (strcmp(subcmd, "idle") == 0) {
        cmd_dev_idle();
    } else if (strcmp(subcmd, "zb_status") == 0) {
        cmd_dev_zb_status();
    } else if (strcmp(subcmd, "zb_pair") == 0) {
        cmd_dev_zb_pair();
    } else if (strcmp(subcmd, "ota_status") == 0) {
        cmd_dev_ota_status();
    } else if (strcmp(subcmd, "ota_query") == 0) {
        printf("[INFO] Initiating OTA check with Zigbee coordinator...\n");
        esp_err_t query_err = ota_zigbee_transport_query_next_image();
        if (query_err != ESP_OK) {
            printf("[ERROR] Failed to dispatch OTA query: %s (0x%x)\n", esp_err_to_name(query_err), (int)query_err);
        } else {
            printf("[SUCCESS] Query Next Image Request dispatched to coordinator.\n");
        }
    } else if (strcmp(subcmd, "ota_rollback") == 0) {
        printf("[WARNING] Requesting manual rollback (may be queued during OTA admission)...\n");
        ota_health_trigger_manual_rollback("Manual CLI rollback request");
        printf("[INFO] Rollback request state: %s\n",
               ota_health_recovery_state_str(ota_health_get_recovery_state()));
    } else if (strcmp(subcmd, "wdt_status") == 0) {
        debug_console_print_wdt_status();
    } else if (strcmp(subcmd, "debug_status") == 0) {
        debug_console_print_status();
    } else if (strcmp(subcmd, "log_level") == 0) {
        char *arg = strtok(NULL, " ");
        cmd_dev_log_level(arg);
    } else if (strcmp(subcmd, "log_filter") == 0) {
        char *tag_arg = strtok(NULL, " ");
        char *lvl_arg = strtok(NULL, " ");
        cmd_dev_log_filter(tag_arg, lvl_arg);
    } else if (strcmp(subcmd, "zb_reset") == 0) {
        /* Zigbee-only reset: clears Zigbee datasets and restarts device.
         * Preserves non-Zigbee settings (LED, verifier, etc.). */
        printf("\n[INFO] Zigbee Reset: Clearing Zigbee network datasets in zb_storage...\n");
        printf("[INFO] Non-Zigbee settings (LED, verifier) are preserved.\n");
        zigbee_native_request_reset();
    } else if (strcmp(subcmd, "factory_reset") == 0) {
        /* Full factory reset: erases entire application NVS partition FIRST,
         * then clears Zigbee datasets via esp_zigbee_factory_reset().
         * Requires explicit CONFIRM argument to prevent accidental data loss.
         * Usage: dev factory_reset CONFIRM */
        char *confirm_arg = strtok(NULL, " ");
        if (confirm_arg != NULL && strcmp(confirm_arg, "CONFIRM") == 0) {
            printf("\n[CRITICAL] FULL FACTORY RESET: Erasing ALL persistent application NVS "
                   "(LED configuration + verifier + user preferences)...\n");

            if (!nvs_manager_factory_reset()) {
                printf("[ERROR] Failed to erase application NVS partition! Full factory reset aborted.\n");
                return;
            }

            printf("[CRITICAL] Application NVS erased successfully.\n");
            printf("[CRITICAL] Clearing Zigbee datasets and restarting via esp_zigbee_factory_reset()...\n");
            esp_zigbee_factory_reset();
        } else {
            printf("\n[WARNING] Full factory reset requires explicit confirmation.\n");
            printf("          This will permanently erase ALL persistent settings:\n");
            printf("            - Zigbee network datasets\n");
            printf("            - LED enabled preference\n");
            printf("            - LED animation mappings\n");
            printf("            - Command verifier preference\n");
            printf("            - All other application NVS preferences\n");
            printf("          To proceed, run: dev factory_reset CONFIRM\n");
            printf("          For Zigbee-only reset (LED preserved): dev zb_reset\n");
        }
    } else {
        printf("Unknown developer tool: '%s'. Type 'dev help' for options.\n", subcmd);
    }
}
