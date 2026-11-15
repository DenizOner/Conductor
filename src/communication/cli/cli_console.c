/**
 * @file cli_console.c
 * @brief Implementation of CLI console separating User Commands from Developer Tools
 *        with runtime Developer Mode gating, debug console filters, and verification pipelines.
 *
 * Compliant with AGENTS.md, Single Responsibility Principle, and the Lossless Master Directive.
 * Developer commands are delegated to cli_dev_tools module.
 */

#include "cli_console.h"
#include "cli_dev_tools.h"
#include "cli_led_commands.h"
#include "led_notification_manager.h"
#include "z906_cmd_verifier.h"
#include "z906_cmd_registry.h"
#include "z906_state.h"
#include "z906_fsm.h"
#include "ota_client.h"
#include "zigbee_state_publisher.h"
#include "system_initializer.h"
#include "system_metrics.h"
#include "health_manager.h"
#include "config.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/** @brief Logging tag for CLI console operations */
static const char *TAG = "CLI";

/** @brief Runtime session-scoped Developer Mode state (Default: false / OFF) */
static bool s_dev_mode = false;

/** @brief Tracks whether cli_console_init() succeeded; guards cli_console_start() */
static bool s_init_ok = false;

/* ========================================================================= */
/* Developer Mode Getter & Setter                                            */
/* ========================================================================= */

bool cli_console_is_dev_mode(void)
{
    return s_dev_mode;
}

void cli_console_set_dev_mode(bool enable)
{
    s_dev_mode = enable;
    if (s_dev_mode) {
        printf("\n[DEVELOPER MODE: ENABLED]\n");
        printf("Type 'help' for User Commands or 'dev help' for Developer Tools.\n\n");
    } else {
        printf("\n[DEVELOPER MODE: DISABLED]\n");
    }
}

/* ========================================================================= */
/* 1. User Commands Implementation (Main Menu)                               */
/* ========================================================================= */

static void cmd_status(void)
{
    z906_state_t st;
    z906_state_get(&st);

    static const char *INPUT_NAMES[] = {
        "Unknown",
        "6-Channel Direct (Input 1)",
        "RCA Stereo (Input 2)",
        "Optical 1 (Input 3)",
        "Optical 2 (Input 4)",
        "Digital Coaxial (Input 5)",
        "Auxiliary / AUX (Input 6)"
    };
    const char *input_name = ((int)st.input >= 1 && (int)st.input <= 6)
                             ? INPUT_NAMES[(int)st.input]
                             : "Unknown";

    ota_diagnostics_t ota_diag;
    ota_client_get_diagnostics(&ota_diag);

    /* Read system initializer boot report for diagnostics display */
    system_init_report_t init_report;
    system_initializer_get_report(&init_report);

    printf("\n=== Logitech Z906 System Status ===\n");

    /* --- Boot / Init Section --- */
    printf("Init State:      %s\n",
           system_init_state_str(system_initializer_get_state()));
    printf("Boot Time:       %lu ms\n", (unsigned long)init_report.elapsed_ms);
    if (init_report.state == SYSTEM_INIT_FAILED) {
        printf("Failed Phase:    %s\n", boot_phase_str(init_report.failed_phase));
        printf("Failed Comp.:    %s\n",
               init_report.failed_component ? init_report.failed_component : "unknown");
        printf("Boot Error:      %s\n", esp_err_to_name(init_report.last_error));
    } else {
        printf("Failed Phase:    none\n");
    }

    /* --- Firmware / OTA Section --- */
    printf("Firmware Ver:    %s (Slot: %s, 32-bit: 0x%08lX)\n",
           Z906_FW_VERSION_STR, ota_diag.running_partition_name, (unsigned long)ota_diag.current_file_version);
    printf("OTA State:       %s (Last Result: %s)\n",
           ota_client_state_str(ota_diag.current_state), ota_client_result_str(ota_diag.last_result));

    /* --- Z906 Hardware Section --- */
    printf("FSM State:       %s\n", z906_fsm_state_str(z906_fsm_get_state()));
    printf("Power:           %s\n", st.power ? "ON" : "STANDBY");
    printf("Mute:            %s\n", st.mute ? "MUTED" : "UNMUTED");
    printf("Master Volume:   %d / 43\n", st.volume);
    printf("Trim Bass:       %d / 43\n", st.volume_bass);
    printf("Trim Center:     %d / 43\n", st.volume_center);
    printf("Trim Rear:       %d / 43\n", st.volume_rear);
    printf("Active Input:    %s\n", input_name);
    printf("Active Effect:   %d\n", (int)st.effect);
    printf("Digital Signal:  %s (0x%02X)\n",
           z906_cmd_digital_signal_status_str(st.digital_signal_status), st.digital_signal_status);
    printf("Auto-Standby:    %s\n", st.auto_standby_enabled ? "ENABLED (2h timeout)" : "DISABLED");
    if (st.temp_valid) {
        printf("Temperatures:    T1=%d C, T2=%d C, T3=%d C\n",
               (int)st.temp_celsius[0], (int)st.temp_celsius[1], (int)st.temp_celsius[2]);
    }
    if (st.idle_seconds > 0) {
        printf("Idle Duration:   %lu seconds (%lu minutes)\n",
               (unsigned long)st.idle_seconds, (unsigned long)(st.idle_seconds / 60));
    }
    printf("DE-15 Cable:     %s\n", st.cable_connected ? "PLUGGED IN" : "DISCONNECTED");
    printf("Operational:     %s\n", st.is_operational ? "YES" : "NO");

    /* --- Settings Section --- */
    printf("Cmd Verifier:    %s\n", z906_cmd_verifier_is_enabled() ? "ENABLED (State Verified)" : "DISABLED (Unverified Dispatch)");
    printf("Internal LED:    %s\n", led_notification_is_enabled() ? "ENABLED" : "DISABLED");
    printf("Developer Mode:  %s\n", s_dev_mode ? "ON" : "OFF");
    printf("===================================\n\n");
}

static void cmd_health(void)
{
    health_snapshot_t snap;
    esp_err_t err = health_manager_get_snapshot(&snap);
    if (err != ESP_OK) {
        printf("Error: Failed to retrieve health snapshot (%s)\n", esp_err_to_name(err));
        return;
    }

    printf("\n=== Conductor System Health ===\n");
    printf("Overall:         %s\n", health_status_to_str(snap.overall_status));
    if (snap.overall_status != HEALTH_STATUS_HEALTHY) {
        printf("Primary Status:  %s (%s)\n",
               health_component_to_str(snap.primary_component),
               health_reason_to_str(snap.primary_reason));
    }
    printf("----------------------------------------\n");
    for (int i = 1; i < HEALTH_COMP_COUNT; i++) {
        printf("%-18s %s\n",
               health_component_to_str((health_component_t)i),
               health_status_to_str(snap.components[i].status));
    }
    printf("========================================\n\n");
}

static void cmd_metrics(void)
{
    system_metrics_snapshot_t snap;
    esp_err_t err = system_metrics_get_snapshot(&snap);
    if (err != ESP_OK) {
        printf("Error: Failed to retrieve system metrics (%s)\n", esp_err_to_name(err));
        return;
    }

    uint32_t total_sec = (uint32_t)(snap.uptime_ms / 1000);
    uint32_t days = total_sec / 86400;
    uint32_t hours = (total_sec % 86400) / 3600;
    uint32_t minutes = (total_sec % 3600) / 60;
    uint32_t seconds = total_sec % 60;

    printf("\n=== Conductor System Metrics ===\n");
    if (snap.soc_temperature_valid) {
        printf("SoC Temperature: %.1f deg C\n", snap.soc_temperature_c);
    } else {
        printf("SoC Temperature: UNKNOWN / SENSOR_ERR\n");
    }

    if (snap.cpu_load_valid) {
        printf("CPU Load:        %.1f %%\n", snap.cpu_load_percent);
    } else {
        printf("CPU Load:        CALCULATING (Baseline pending)\n");
    }

    printf("Heap Used:       %.1f %% (%lu KB)\n",
           snap.heap_usage_percent, (unsigned long)(snap.heap_used_bytes / 1024));
    printf("Heap Free:       %lu KB\n", (unsigned long)(snap.heap_free_bytes / 1024));
    printf("Min Free Heap:   %lu KB (Historical Watermark)\n", (unsigned long)(snap.heap_min_free_bytes / 1024));
    printf("Largest Block:   %lu KB\n", (unsigned long)(snap.heap_largest_free_block_bytes / 1024));

    if (days > 0) {
        printf("Uptime:          %lud %02luh %02lum %02lus\n",
               (unsigned long)days, (unsigned long)hours, (unsigned long)minutes, (unsigned long)seconds);
    } else {
        printf("Uptime:          %02luh %02lum %02lus\n",
               (unsigned long)hours, (unsigned long)minutes, (unsigned long)seconds);
    }

    printf("Reset Reason:    %s (raw: 0x%x)\n",
           system_metrics_reset_reason_to_str(snap.reset_reason), snap.reset_reason_raw);

    /* Profiling & Overhead Statistics (Sections 24-27) */
    system_metrics_overhead_stats_t ovh;
    if (system_metrics_get_overhead_stats(&ovh) == ESP_OK && ovh.sample_count > 0) {
        printf("Sampler Timing:  min %lu us, avg %lu us, max %lu us (duty: %.4f %%)\n",
               (unsigned long)ovh.min_sample_time_us,
               (unsigned long)ovh.avg_sample_time_us,
               (unsigned long)ovh.max_sample_time_us,
               ovh.measured_duty_cycle_pct);
        printf("Sampler Stack:   %lu B free / %lu B total (margin: %lu B)\n",
               (unsigned long)ovh.task_stack_min_free_bytes,
               (unsigned long)ovh.task_stack_allocated,
               (unsigned long)ovh.task_stack_min_free_bytes);
    }

    printf("================================\n\n");
}

static void cmd_user_power(char *arg)
{
    const z906_cmd_descriptor_t *desc = z906_cmd_registry_find_by_cli_name("power", NULL);
    if (desc && arg && (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0)) {
        int target_pwr = (strcmp(arg, "on") == 0) ? 1 : 0;
        if (z906_cmd_registry_validate_param(desc->id, target_pwr)) {
            cmd_request_t req;
            z906_cmd_registry_build_request(desc->id, CMD_SOURCE_CLI, target_pwr, &req);
            z906_cmd_verify_info_t info;
            cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

            if (res == CMD_VERIFY_SUCCESS) {
                printf("[SUCCESS] Power %s verified (Latency: %lu ms)\n",
                       info.post_state.power ? "ON" : "STANDBY", (unsigned long)info.duration_ms);
            } else if (res == CMD_VERIFY_SENT_UNVERIFIED) {
                printf("[SENT] Power %s dispatched (Unverified Mode, Latency: %lu ms)\n",
                       target_pwr ? "ON" : "STANDBY", (unsigned long)info.duration_ms);
            } else {
                printf("[FAILED] Power command failed: %s (Code: %d, Latency: %lu ms)\n",
                       z906_cmd_verify_result_str(res), (int)res, (unsigned long)info.duration_ms);
            }
        } else {
            printf("Error: Power parameter validation failed\n");
        }
    } else {
        printf("Usage: power <on|off>\n");
    }
}

static void cmd_user_volume(char *arg)
{
    const z906_cmd_descriptor_t *desc = z906_cmd_registry_find_by_cli_name("volume", NULL);
    if (desc && arg) {
        int vol = atoi(arg);
        if (z906_cmd_registry_validate_param(desc->id, vol)) {
            cmd_request_t req;
            z906_cmd_registry_build_request(desc->id, CMD_SOURCE_CLI, vol, &req);
            z906_cmd_verify_info_t info;
            cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

            if (res == CMD_VERIFY_SUCCESS) {
                printf("[SUCCESS] Volume verified at %d/43 (Latency: %lu ms)\n",
                       info.post_state.volume, (unsigned long)info.duration_ms);
            } else if (res == CMD_VERIFY_SENT_UNVERIFIED) {
                printf("[SENT] Volume %d/43 dispatched (Unverified Mode, Latency: %lu ms)\n",
                       vol, (unsigned long)info.duration_ms);
            } else {
                printf("[FAILED] Volume command failed: %s (Code: %d, Latency: %lu ms)\n",
                       z906_cmd_verify_result_str(res), (int)res, (unsigned long)info.duration_ms);
            }
        } else {
            printf("Error: Volume must be between %ld and %ld\n",
                    (long)desc->min_val, (long)desc->max_val);
        }
    } else {
        printf("Usage: volume <0-43>\n");
    }
}

static void cmd_user_trim(char *chan_arg, char *val_arg)
{
    if (chan_arg && val_arg) {
        const z906_cmd_descriptor_t *desc = z906_cmd_registry_find_by_cli_name("trim", chan_arg);
        if (desc) {
            int lvl = atoi(val_arg);
            if (z906_cmd_registry_validate_param(desc->id, lvl)) {
                cmd_request_t req;
                z906_cmd_registry_build_request(desc->id, CMD_SOURCE_CLI, lvl, &req);
                z906_cmd_verify_info_t info;
                cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

                if (res == CMD_VERIFY_SUCCESS) {
                    uint8_t post_lvl = 0;
                    if (desc->trim_channel == TRIM_CHANNEL_BASS) post_lvl = info.post_state.volume_bass;
                    else if (desc->trim_channel == TRIM_CHANNEL_CENTER) post_lvl = info.post_state.volume_center;
                    else if (desc->trim_channel == TRIM_CHANNEL_REAR) post_lvl = info.post_state.volume_rear;

                    printf("[SUCCESS] Trim %s verified at %d/43 (Latency: %lu ms)\n",
                           chan_arg, post_lvl, (unsigned long)info.duration_ms);
                } else if (res == CMD_VERIFY_SENT_UNVERIFIED) {
                    printf("[SENT] Trim %s %d/43 dispatched (Unverified Mode, Latency: %lu ms)\n",
                           chan_arg, lvl, (unsigned long)info.duration_ms);
                } else {
                    printf("[FAILED] Trim command failed: %s (Code: %d, Latency: %lu ms)\n",
                           z906_cmd_verify_result_str(res), (int)res, (unsigned long)info.duration_ms);
                }
            } else {
                printf("Error: Trim level must be between %ld and %ld\n",
                       (long)desc->min_val, (long)desc->max_val);
            }
        } else {
            printf("Error: Invalid trim channel '%s'. Choices: bass, center, rear\n", chan_arg);
        }
    } else {
        printf("Usage: trim <bass|center|rear> <0-43>\n");
    }
}

static void cmd_user_input(char *arg)
{
    const z906_cmd_descriptor_t *desc = z906_cmd_registry_find_by_cli_name("input", NULL);
    if (desc && arg) {
        int in = atoi(arg);
        if (z906_cmd_registry_validate_param(desc->id, in)) {
            cmd_request_t req;
            z906_cmd_registry_build_request(desc->id, CMD_SOURCE_CLI, in, &req);
            z906_cmd_verify_info_t info;
            cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

            static const char *INPUT_NAMES[] = {
                "Unknown",
                "6-Channel Direct (Input 1)",
                "RCA Stereo (Input 2)",
                "Optical 1 (Input 3)",
                "Optical 2 (Input 4)",
                "Digital Coaxial (Input 5)",
                "Auxiliary / AUX (Input 6)"
            };

            if (res == CMD_VERIFY_SUCCESS) {
                int post_in = (int)info.post_state.input;
                const char *name = (post_in >= 1 && post_in <= 6) ? INPUT_NAMES[post_in] : "Unknown";
                printf("[SUCCESS] Audio input switched to %s (Latency: %lu ms)\n",
                       name, (unsigned long)info.duration_ms);
            } else if (res == CMD_VERIFY_SENT_UNVERIFIED) {
                const char *name = (in >= 1 && in <= 6) ? INPUT_NAMES[in] : "Unknown";
                printf("[SENT] Audio input switch to %s dispatched (Unverified Mode, Latency: %lu ms)\n",
                       name, (unsigned long)info.duration_ms);
            } else {
                printf("[FAILED] Input switch failed: %s (Code: %d, Latency: %lu ms)\n",
                       z906_cmd_verify_result_str(res), (int)res, (unsigned long)info.duration_ms);
            }
        } else {
            printf("Error: Input must be between %ld and %ld\n",
                    (long)desc->min_val, (long)desc->max_val);
        }
    } else {
        printf("Usage: input <1-6>\n");
        printf("  1: 6-Channel Direct\n  2: RCA Stereo\n  3: Optical 1\n");
        printf("  4: Optical 2\n  5: Digital Coaxial\n  6: Auxiliary / AUX\n");
    }
}

static void cmd_user_effect(char *arg)
{
    const z906_cmd_descriptor_t *desc = z906_cmd_registry_find_by_cli_name("effect", NULL);
    if (desc && arg) {
        int eff_val = -1;
        if (strcmp(arg, "3d") == 0) eff_val = (int)Z906_EFFECT_3D;
        else if (strcmp(arg, "2.1") == 0) eff_val = (int)Z906_EFFECT_2_1;
        else if (strcmp(arg, "4.1") == 0) eff_val = (int)Z906_EFFECT_4_1;
        else if (strcmp(arg, "none") == 0) eff_val = (int)Z906_EFFECT_NONE;

        if (eff_val >= 0 && z906_cmd_registry_validate_param(desc->id, eff_val)) {
            cmd_request_t req;
            z906_cmd_registry_build_request(desc->id, CMD_SOURCE_CLI, eff_val, &req);
            z906_cmd_verify_info_t info;
            cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

            if (res == CMD_VERIFY_SUCCESS) {
                printf("[SUCCESS] Audio effect %s verified (Latency: %lu ms)\n",
                       arg, (unsigned long)info.duration_ms);
            } else if (res == CMD_VERIFY_SENT_UNVERIFIED) {
                printf("[SENT] Audio effect %s dispatched (Unverified Mode, Latency: %lu ms)\n",
                       arg, (unsigned long)info.duration_ms);
            } else {
                printf("[FAILED] Audio effect failed: %s (Code: %d, Latency: %lu ms)\n",
                       z906_cmd_verify_result_str(res), (int)res, (unsigned long)info.duration_ms);
            }
        } else {
            printf("Error: Invalid effect. Choices: 3d, 2.1, 4.1, none\n");
        }
    } else {
        printf("Usage: effect <3d|2.1|4.1|none>\n");
    }
}

static void cmd_user_mute(char *arg)
{
    const z906_cmd_descriptor_t *desc = z906_cmd_registry_find_by_cli_name("mute", NULL);
    if (desc && arg && (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0)) {
        int target_mute = (strcmp(arg, "on") == 0) ? 1 : 0;
        if (z906_cmd_registry_validate_param(desc->id, target_mute)) {
            cmd_request_t req;
            z906_cmd_registry_build_request(desc->id, CMD_SOURCE_CLI, target_mute, &req);
            z906_cmd_verify_info_t info;
            cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

            if (res == CMD_VERIFY_SUCCESS) {
                printf("[SUCCESS] Mute %s verified (Latency: %lu ms)\n",
                       info.post_state.mute ? "ON" : "OFF", (unsigned long)info.duration_ms);
            } else if (res == CMD_VERIFY_SENT_UNVERIFIED) {
                printf("[SENT] Mute %s dispatched (Unverified Mode, Latency: %lu ms)\n",
                       target_mute ? "ON" : "OFF", (unsigned long)info.duration_ms);
            } else {
                printf("[FAILED] Mute command failed: %s (Code: %d, Latency: %lu ms)\n",
                       z906_cmd_verify_result_str(res), (int)res, (unsigned long)info.duration_ms);
            }
        } else {
            printf("Error: Mute parameter validation failed\n");
        }
    } else {
        printf("Usage: mute <on|off>\n");
    }
}

static void cmd_user_devmode(char *arg)
{
    if (arg == NULL) {
        printf("Developer Mode: %s\n", s_dev_mode ? "ON" : "OFF");
        printf("Usage: devmode <on|off>\n");
        return;
    }

    if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0 || strcmp(arg, "enable") == 0) {
        cli_console_set_dev_mode(true);
    } else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0 || strcmp(arg, "disable") == 0) {
        cli_console_set_dev_mode(false);
    } else {
        printf("Invalid option '%s'. Usage: devmode <on|off>\n", arg);
    }
}

static void cmd_user_verifier(char *arg)
{
    if (arg == NULL) {
        printf("Command Verifier: %s\n", z906_cmd_verifier_is_enabled() ? "ENABLED (State Verified)" : "DISABLED (Unverified Dispatch)");
        printf("Usage: verifier <on|off|status>\n");
        return;
    }

    if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0 || strcmp(arg, "enable") == 0) {
        z906_cmd_verifier_set_enabled(true);
        zigbee_state_publisher_notify(ZB_PUB_NOTIF_CMD_VERIFIER);
        printf("[SUCCESS] Command Verifier ENABLED (State Verified, saved to NVS).\n");
    } else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0 || strcmp(arg, "disable") == 0) {
        z906_cmd_verifier_set_enabled(false);
        zigbee_state_publisher_notify(ZB_PUB_NOTIF_CMD_VERIFIER);
        printf("[SUCCESS] Command Verifier DISABLED (Unverified Dispatch Mode, saved to NVS).\n");
    } else if (strcmp(arg, "status") == 0) {
        printf("Command Verifier: %s\n", z906_cmd_verifier_is_enabled() ? "ENABLED (State Verified)" : "DISABLED (Unverified Dispatch)");
    } else {
        printf("Invalid option '%s'. Usage: verifier <on|off|status>\n", arg);
    }
}

static void show_user_help(void)
{
    printf("\n=== Logitech Z906 Main Menu (User Commands) ===\n");
    printf("  status                         - Print current confirmed hardware telemetry & firmware version\n");
    printf("  health                         - Inspect overall and per-component operational health states\n");
    printf("  metrics                        - Display raw RTOS runtime measurements (SoC temp, CPU, Heap)\n");
    printf("  power <on|off>                 - Toggle power state with hardware verification\n");
    printf("  volume <0-43>                  - Set master volume level with hardware verification\n");
    printf("  trim <bass|center|rear> <0-43> - Set speaker trim level with hardware verification\n");
    printf("  input <1-6>                    - Switch active audio input with hardware verification\n");
    printf("                                   (1=Direct, 2=RCA, 3=Opt1, 4=Opt2, 5=Coax, 6=Aux)\n");
    printf("  effect <3d|2.1|4.1|none>       - Select audio processing effect with verification\n");
    printf("  mute <on|off>                  - Toggle amplifier mute with hardware verification\n");
    printf("  led <on|off|status|mappings|..>- Configure and control onboard RGB LED notifications\n");
    printf("  verifier <on|off|status>       - Enable/disable hardware state verification (Current: %s)\n",
           z906_cmd_verifier_is_enabled() ? "ON" : "OFF");
    printf("  devmode <on|off>               - Enable/disable Developer Tools (Current: %s)\n",
           s_dev_mode ? "ON" : "OFF");
    printf("  help                           - Display this user command list\n");

    if (s_dev_mode) {
        printf("\nDeveloper Tools are UNLOCKED. Type 'dev help' to view developer diagnostics.\n");
    }
    printf("===============================================\n\n");
}

/* ========================================================================= */
/* Initialization & Command Line Processing Entry Point                      */
/* ========================================================================= */

bool cli_console_init(void)
{
    s_dev_mode = false;
    s_init_ok = true;
    ESP_LOGI(TAG, "CLI console ready on USB Serial/JTAG (Developer Mode: OFF)");
    return true;
}

void cli_console_process_line(const char *cmd_line)
{
    if (cmd_line == NULL || strlen(cmd_line) == 0) {
        return;
    }

    char buf[128];
    strncpy(buf, cmd_line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Strip newline characters */
    char *eol = strpbrk(buf, "\r\n");
    if (eol) *eol = '\0';

    char *cmd = strtok(buf, " ");
    if (!cmd) return;

    /* 1. Main Menu / User Commands (Always available) */
    if (strcmp(cmd, "status") == 0) {
        cmd_status();
    } else if (strcmp(cmd, "health") == 0) {
        cmd_health();
    } else if (strcmp(cmd, "metrics") == 0) {
        cmd_metrics();
    } else if (strcmp(cmd, "power") == 0) {
        char *arg = strtok(NULL, " ");
        cmd_user_power(arg);
    } else if (strcmp(cmd, "volume") == 0) {
        char *arg = strtok(NULL, " ");
        cmd_user_volume(arg);
    } else if (strcmp(cmd, "trim") == 0) {
        char *chan_arg = strtok(NULL, " ");
        char *val_arg = strtok(NULL, " ");
        cmd_user_trim(chan_arg, val_arg);
    } else if (strcmp(cmd, "input") == 0) {
        char *arg = strtok(NULL, " ");
        cmd_user_input(arg);
    } else if (strcmp(cmd, "effect") == 0) {
        char *arg = strtok(NULL, " ");
        cmd_user_effect(arg);
    } else if (strcmp(cmd, "mute") == 0) {
        char *arg = strtok(NULL, " ");
        cmd_user_mute(arg);
    } else if (strcmp(cmd, "led") == 0) {
        char *arg = strtok(NULL, "");
        cli_led_process(arg);
    } else if (strcmp(cmd, "verifier") == 0) {
        char *arg = strtok(NULL, " ");
        cmd_user_verifier(arg);
    } else if (strcmp(cmd, "devmode") == 0) {
        char *arg = strtok(NULL, " ");
        cmd_user_devmode(arg);
    } else if (strcmp(cmd, "help") == 0) {
        show_user_help();
    }
    /* 2. Developer Tools Entrance via 'dev' prefix */
    else if (strcmp(cmd, "dev") == 0) {
        if (!s_dev_mode) {
            printf("Error: Developer Mode is currently OFF.\n");
            printf("Type 'devmode on' to enable Developer Tools.\n");
        } else {
            char *subcmd = strtok(NULL, " ");
            cli_dev_tools_process(subcmd);
        }
    }
    /* 3. Direct execution of developer commands when Developer Mode is ON */
    else if (s_dev_mode && cli_dev_tools_is_dev_command(cmd)) {
        cli_dev_tools_process(cmd);
    }
    /* 4. Developer Command attempted while Developer Mode is OFF */
    else if (!s_dev_mode && cli_dev_tools_is_dev_command(cmd)) {
        printf("Error: '%s' is a developer diagnostic tool.\n", cmd);
        printf("Developer Mode is currently OFF. Type 'devmode on' to enable.\n");
    } else {
        printf("Unknown command: '%s'. Type 'help' for available user commands.\n", cmd);
    }
}

/* =========================================================================
 * CLI Task — owned by communication/cli (cli_console.c)
 * ========================================================================= */

#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "wdt_manager.h"

/** @brief Stack depth in words for the CLI stdin reader task */
#define CLI_TASK_STACK_WORDS    (3072u / sizeof(StackType_t))

/** @brief FreeRTOS priority for the CLI task */
#define CLI_TASK_PRIORITY       2u

/** @brief Max ms to wait for the task to signal STARTED before timeout */
#define CLI_STARTUP_TIMEOUT_MS  500u

static StackType_t  s_cli_stack[CLI_TASK_STACK_WORDS];
static StaticTask_t s_cli_tcb;

/** @brief Semaphore for deterministic startup handshake in cli_console_start() */
static StaticSemaphore_t s_cli_started_sem_buffer;
static SemaphoreHandle_t s_cli_started_sem = NULL;

static void cli_task_body(void *arg)
{
    (void)arg;

    /* Register with TWDT before entering loop */
    wdt_manager_register_task();

    /* Signal that task is entering main loop (STARTED confirmed) */
    if (s_cli_started_sem != NULL) {
        xSemaphoreGive(s_cli_started_sem);
    }

    ESP_LOGI(TAG, "CLI task entered main loop (Priority %u)", CLI_TASK_PRIORITY);

    char line_buf[128];
    while (1) {
        wdt_manager_reset();

        /* Non-blocking read from console stream */
        if (fgets(line_buf, sizeof(line_buf), stdin) != NULL) {
            cli_console_process_line(line_buf);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool cli_console_start(void)
{
    if (!s_init_ok) {
        ESP_LOGE(TAG, "Cannot start CLI task: cli_console_init() failed or not called");
        return false;
    }

    s_cli_started_sem = xSemaphoreCreateBinaryStatic(&s_cli_started_sem_buffer);
    if (s_cli_started_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create static CLI started semaphore");
        return false;
    }

    TaskHandle_t h = xTaskCreateStatic(
        cli_task_body,
        "cli_task",
        CLI_TASK_STACK_WORDS,
        NULL,
        CLI_TASK_PRIORITY,
        s_cli_stack,
        &s_cli_tcb
    );

    if (h == NULL) {
        ESP_LOGE(TAG, "Failed to create static CLI task");
        return false;
    }

    /* Wait for task to signal STARTED */
    if (xSemaphoreTake(s_cli_started_sem, pdMS_TO_TICKS(CLI_STARTUP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout waiting for cli_task to signal STARTED");
        return false;
    }

    ESP_LOGI(TAG, "cli_task confirmed STARTED");
    return true;
}
