/**
 * @file cli_dev_tools.h
 * @brief Developer Tools commands and diagnostics module for CLI interface.
 *
 * Provides diagnostic handlers, developer help display, and command dispatcher
 * for Developer Mode operations, adhering to SRP and modularity directives.
 */

#ifndef CLI_DEV_TOOLS_H
#define CLI_DEV_TOOLS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/**
 * @brief Displays the Developer Tools hierarchy and available commands.
 */
void cli_dev_tools_show_help(void);

/**
 * @brief Processes a developer subcommand (e.g. temp, idle, zb_status, wdt_status, ota_status, ota_query, ota_rollback).
 *
 * @param subcmd First token of the developer tool command name.
 */
void cli_dev_tools_process(char *subcmd);

/**
 * @brief Checks if a given command name matches any known developer diagnostic tool.
 *
 * @param cmd Command name string to check.
 * @return true if command is a developer tool, false otherwise.
 */
bool cli_dev_tools_is_dev_command(const char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* CLI_DEV_TOOLS_H */
