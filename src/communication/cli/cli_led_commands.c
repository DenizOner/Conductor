/**
 * @file cli_led_commands.c
 * @brief Implementation of CLI LED notification commands.
 */

#include "cli_led_commands.h"
#include "led_notification_manager.h"
#include "led_animation_registry.h"
#include "led_nvs_storage.h"
#include "led_driver.h"
#include "zigbee_state_publisher.h"
#include <stdio.h>
#include <string.h>

void cli_led_show_help(void)
{
    printf("\n=== Onboard LED Notification Commands ===\n");
    printf("  led on                                - Enable LED notifications (persisted)\n");
    printf("  led off                               - Disable all LED notifications and turn LED OFF\n");
    printf("  led status                            - Show global enable state, active animation & mappings\n");
    printf("  led animations                        - List all registered animation IDs and descriptions\n");
    printf("  led mapping show                      - Display event-to-animation mappings and policies\n");
    printf("  led mapping set <event> <anim_id>     - Set animation for specific event (persisted)\n");
    printf("  led mapping reset <event|all>         - Reset event mapping(s) to factory defaults\n");
    printf("  led preview <anim_id>                 - Preview animation on LED (temporary, non-blocking)\n");
    printf("  led help                              - Show this help summary\n");
    printf("\nAvailable Events: boot, pairing, normal, command_failure, command_success\n");
    printf("=========================================\n\n");
}

static void cmd_led_status(void)
{
    bool enabled = led_notification_is_enabled();
    const char *active = led_notification_get_active_animation_id();
    led_color_t drv_color = led_driver_get_current_color();

    /* Determine driver output state label: OFF if all channels are 0, else show RGB values */
    bool driver_off = (drv_color.r == 0 && drv_color.g == 0 && drv_color.b == 0);

    printf("\n=== Onboard LED Notification Status ===\n");
    printf("Global Enabled:   %s\n", enabled ? "ON (Enabled)" : "OFF (Disabled)");
    printf("Active Animation: %s\n", active);
    printf("Boot Window:      %s\n", led_notification_is_boot_active() ? "ACTIVE (<= 10s)" : "EXPIRED");
    /* Driver Output reflects the last GRB frame transmitted to the WS2812 hardware.
     * This is software-cached state — not a physical photon measurement. */
    if (driver_off) {
        printf("Driver Output:    OFF (last transmitted: R=0 G=0 B=0)\n");
    } else {
        printf("Driver Output:    ON  (last transmitted: R=%u G=%u B=%u)\n",
               drv_color.r, drv_color.g, drv_color.b);
    }
    printf("Mappings:\n");
    for (int i = 0; i < LED_EVENT_COUNT; i++) {
        led_event_type_t evt = (led_event_type_t)i;
        printf("  - %-16s -> %-18s [default: %s]\n",
               led_event_to_string(evt),
               led_notification_get_mapping(evt),
               led_get_default_animation_for_event(evt));
    }
    printf("=======================================\n\n");
}

static void cmd_led_animations(void)
{
    printf("\n=== Registered LED Animations ===\n");
    size_t count = led_animation_registry_count();
    for (size_t i = 0; i < count; i++) {
        const led_registered_anim_t *reg = led_animation_registry_get_at(i);
        if (reg && reg->anim) {
            printf("  [%zu] ID: %-18s  Name: %s\n", i + 1, reg->anim->id, reg->anim->display_name);
        }
    }
    printf("=================================\n\n");
}

static void cmd_led_mapping_show(void)
{
    printf("\n=== LED Event-to-Animation Mappings ===\n");
    printf("%-16s %-18s %-18s %-12s %-16s\n", "Event", "Active Animation", "Default Animation", "Mode", "Duration");
    printf("--------------------------------------------------------------------------------\n");

    for (int i = 0; i < LED_EVENT_COUNT; i++) {
        led_event_type_t evt = (led_event_type_t)i;
        const char *mode = (evt == LED_EVENT_CMD_FAILURE || evt == LED_EVENT_CMD_SUCCESS) ? "Single sequence" : "Continuous loop";
        const char *dur = (evt == LED_EVENT_BOOT) ? "10s window" : "Event duration";

        printf("%-16s %-18s %-18s %-12s %-16s\n",
               led_event_to_string(evt),
               led_notification_get_mapping(evt),
               led_get_default_animation_for_event(evt),
               mode, dur);
    }
    printf("================================================================================\n\n");
}

static void cmd_led_mapping_set(char *event_str, char *anim_str)
{
    if (event_str == NULL || anim_str == NULL) {
        printf("Error: Missing parameters. Usage: led mapping set <event> <anim_id>\n");
        printf("Type 'led animations' to see available animation IDs.\n");
        return;
    }

    led_event_type_t evt;
    if (!led_event_from_string(event_str, &evt)) {
        printf("Error: Unknown event '%s'. Allowed: boot, pairing, normal, command_failure, command_success\n", event_str);
        return;
    }

    if (led_animation_registry_find_by_id(anim_str) == NULL) {
        printf("Error: Unknown animation ID '%s'. Type 'led animations' for list.\n", anim_str);
        return;
    }

    if (led_notification_set_mapping(evt, anim_str)) {
        printf("SUCCESS: Event '%s' mapped to animation '%s' (saved to NVS).\n", event_str, anim_str);
    } else {
        printf("Error: Failed to save mapping to NVS.\n");
    }
}

static void cmd_led_mapping_reset(char *arg)
{
    if (arg == NULL) {
        printf("Error: Missing parameter. Usage: led mapping reset <event|all>\n");
        return;
    }

    if (strcmp(arg, "all") == 0) {
        if (led_notification_reset_mappings(-1)) {
            printf("SUCCESS: All 5 event mappings reset to factory defaults (global enable unaffected).\n");
        } else {
            printf("Error: Failed to reset mappings in NVS.\n");
        }
    } else {
        led_event_type_t evt;
        if (!led_event_from_string(arg, &evt)) {
            printf("Error: Unknown event '%s'. Usage: led mapping reset <boot|pairing|normal|command_failure|command_success|all>\n", arg);
            return;
        }

        if (led_notification_reset_mappings((int)evt)) {
            printf("SUCCESS: Event '%s' reset to factory default '%s' (saved to NVS).\n",
                   arg, led_get_default_animation_for_event(evt));
        } else {
            printf("Error: Failed to reset mapping in NVS.\n");
        }
    }
}

void cli_led_process(const char *line)
{
    if (line == NULL || strlen(line) == 0) {
        cmd_led_status();
        return;
    }

    char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *subcmd = strtok(buf, " ");
    if (!subcmd) {
        cmd_led_status();
        return;
    }

    if (strcmp(subcmd, "on") == 0) {
        led_notification_set_enabled(true);
        zigbee_state_publisher_notify(ZB_PUB_NOTIF_BUILT_IN_LED);
        printf("SUCCESS: LED notifications ENABLED (ON, saved to NVS).\n");
    } else if (strcmp(subcmd, "off") == 0) {
        led_notification_set_enabled(false);
        zigbee_state_publisher_notify(ZB_PUB_NOTIF_BUILT_IN_LED);
        printf("SUCCESS: LED notifications DISABLED (OFF, physical LED turned OFF, saved to NVS).\n");
    } else if (strcmp(subcmd, "status") == 0) {
        cmd_led_status();
    } else if (strcmp(subcmd, "animations") == 0) {
        cmd_led_animations();
    } else if (strcmp(subcmd, "preview") == 0) {
        char *anim_id = strtok(NULL, " ");
        if (!anim_id) {
            printf("Usage: led preview <animation_id>\n");
        } else {
            if (led_notification_preview(anim_id)) {
                printf("Previewing animation '%s' on LED...\n", anim_id);
            } else {
                if (!led_notification_is_enabled()) {
                    printf("Notice: LED is currently OFF globally. Enable it first with 'led on'.\n");
                } else {
                    printf("Error: Unknown animation '%s'. Type 'led animations' for list.\n", anim_id);
                }
            }
        }
    } else if (strcmp(subcmd, "mapping") == 0) {
        char *action = strtok(NULL, " ");
        if (!action || strcmp(action, "show") == 0) {
            cmd_led_mapping_show();
        } else if (strcmp(action, "set") == 0) {
            char *ev = strtok(NULL, " ");
            char *an = strtok(NULL, " ");
            cmd_led_mapping_set(ev, an);
        } else if (strcmp(action, "reset") == 0) {
            char *target = strtok(NULL, " ");
            cmd_led_mapping_reset(target);
        } else {
            printf("Unknown mapping action '%s'. Usage: led mapping <show|set|reset>\n", action);
        }
    } else if (strcmp(subcmd, "help") == 0) {
        cli_led_show_help();
    } else {
        printf("Unknown LED command: '%s'. Type 'led help' for available commands.\n", subcmd);
    }
}
