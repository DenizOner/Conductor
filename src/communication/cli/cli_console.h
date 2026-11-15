/**
 * @file cli_console.h
 * @brief Interactive command-line interface over USB Serial/JTAG bifurcated into
 *        User Commands and Developer Tools with Developer Mode runtime management.
 *
 * Compliant with AGENTS.md Section 14: Autonomous Development, CLI Architecture,
 * Diagnostics and Developer Tools.
 */

#ifndef CLI_CONSOLE_H
#define CLI_CONSOLE_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the interactive CLI console and sets Developer Mode to default (OFF).
 * @return true on success.
 */
bool cli_console_init(void);

/**
 * @brief Spawns the static CLI task that reads stdin lines and dispatches commands.
 *
 * Must only be called after cli_console_init() succeeds. If init failed,
 * do NOT call start() — the CLI handler table is unpopulated.
 *
 * The static task TCB and stack live inside cli_console.c (single ownership).
 * Uses an internal StaticSemaphore_t handshake: returns only after the task
 * has signalled that it entered its main loop (STARTED state confirmed).
 *
 * @return true if task started, false if creation failed or timeout elapsed.
 */
bool cli_console_start(void);

/**
 * @brief Returns the current state of Developer Mode.
 * @return true if Developer Mode is enabled, false if disabled.
 */
bool cli_console_is_dev_mode(void);

/**
 * @brief Enables or disables Developer Mode.
 * @param enable true to enable Developer Tools, false to restrict to User Commands.
 */
void cli_console_set_dev_mode(bool enable);

/**
 * @brief Processes one interactive line from the console input stream.
 * @param cmd_line Input command string received over serial console.
 */
void cli_console_process_line(const char *cmd_line);

#ifdef __cplusplus
}
#endif

#endif /* CLI_CONSOLE_H */
