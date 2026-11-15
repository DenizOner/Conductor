/**
 * @file cli_led_commands.h
 * @brief Declarations of CLI command handlers for LED notification management.
 *
 * Implements:
 *   led on | off | status | help
 *   led animations
 *   led mapping show
 *   led mapping set <boot|pairing|normal|command_failure|command_success> <animation_id>
 *   led mapping reset <boot|pairing|normal|command_failure|command_success|all>
 *   led preview <animation_id>
 */

#ifndef CLI_LED_COMMANDS_H
#define CLI_LED_COMMANDS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Processes subcommands following the 'led' root CLI command.
 * @param line The argument string following 'led ' (e.g. "on", "mapping show", etc.)
 */
void cli_led_process(const char *line);

/**
 * @brief Displays help documentation for all LED commands.
 */
void cli_led_show_help(void);

#ifdef __cplusplus
}
#endif

#endif /* CLI_LED_COMMANDS_H */
