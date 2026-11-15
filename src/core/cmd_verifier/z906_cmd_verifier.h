/**
 * @file z906_cmd_verifier.h
 * @brief Centralized state-based command verification engine for Logitech Z906.
 *
 * Implements strict pre-condition checks, post-state assertion via confirmed hardware
 * telemetry, FreeRTOS mutex synchronization, and comprehensive verification result logging.
 *
 * Fully compliant with the canonical reverse engineering reference documentation
 * (nomis/logitech-z906): protocol.rst, interface.rst, firmware.rst, README.rst.
 */

#ifndef Z906_CMD_VERIFIER_H
#define Z906_CMD_VERIFIER_H

#include <stdint.h>
#include <stdbool.h>
#include "z906_state.h"
#include "z906_commands.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Originating source of a command execution request.
 */
typedef enum {
    CMD_SOURCE_CLI = 0,     /**< Command initiated via USB Serial/JTAG diagnostic console */
    CMD_SOURCE_ZIGBEE       /**< Command initiated via native Zigbee 3.0 cluster handler */
} cmd_source_t;

/**
 * @brief Categorized command types subject to state verification.
 */
typedef enum {
    VERIFY_CMD_POWER = 0,       /**< System power state transition (ON / Standby) */
    VERIFY_CMD_VOLUME,          /**< Master volume level adjustment (0 - 43) */
    VERIFY_CMD_INPUT,           /**< Audio input source selection (Input 1 - 6) */
    VERIFY_CMD_EFFECT,          /**< Spatial audio effect mode (3D, 2.1, 4.1, None) */
    VERIFY_CMD_MUTE,            /**< Audio mute toggle (Muted / Unmuted) */
    VERIFY_CMD_TRIM,            /**< Speaker trim level (Bass, Center, Rear) */
    VERIFY_CMD_UNVERIFIABLE     /**< Commands without hardware state confirmation (e.g. Save, Reset) */
} verify_cmd_type_t;

/**
 * @brief Exhaustive result codes produced by the command verification pipeline.
 */
typedef enum {
    CMD_VERIFY_SUCCESS = 0,             /**< Command applied and verified via confirmed hardware telemetry */
    CMD_VERIFY_SENT_UNVERIFIED,         /**< Command dispatched physically over UART without post-verification (verifier disabled) */
    CMD_VERIFY_ERR_SEND_FAILED,         /**< Hardware UART transmission failed or port unavailable */
    CMD_VERIFY_ERR_PRE_STATE_FAILED,    /**< Pre-command hardware state could not be acquired */
    CMD_VERIFY_ERR_POST_STATE_FAILED,   /**< Post-command hardware state could not be acquired */
    CMD_VERIFY_ERR_TARGET_NOT_REACHED,  /**< Command was sent, but post-state does not match expected target */
    CMD_VERIFY_ERR_UNVERIFIABLE,        /**< Command has no physical feedback mechanism on Logitech Z906 */
    CMD_VERIFY_ERR_TIMEOUT,             /**< Timed out waiting for confirmed telemetry update from amplifier */
    CMD_VERIFY_ERR_PRECONDITION_FAILED, /**< Preconditions not met (e.g., disconnected cable or standby mode) */
    CMD_VERIFY_ERR_BUSY                 /**< Concurrency lock contention; another verification is active */
} cmd_verify_result_t;

/**
 * @brief Command execution request payload.
 */
typedef struct {
    verify_cmd_type_t type;     /**< Type of command to execute */
    cmd_source_t source;        /**< Originating interface (CLI or Zigbee) */
    union {
        bool power;             /**< Desired power state (true = ON, false = Standby) */
        uint8_t volume;         /**< Desired master volume level (0 - 43) */
        z906_input_t input;     /**< Desired audio input source (1 - 6) */
        z906_effect_t effect;   /**< Desired spatial audio effect */
        bool mute;              /**< Desired mute state (true = Muted, false = Unmuted) */
        struct {
            trim_channel_t channel; /**< Trim speaker channel (Bass, Center, Rear) */
            uint8_t level;          /**< Desired trim volume level (0 - 43) */
        } trim;
    } param;                    /**< Parameter payload specific to the command type */
} cmd_request_t;

/**
 * @brief Diagnostic telemetry bundle capturing the full lifecycle of a verification attempt.
 */
typedef struct {
    cmd_request_t request;      /**< Original request executed */
    z906_state_t pre_state;     /**< Confirmed hardware state before command execution */
    z906_state_t target_state;  /**< Calculated expected target state */
    z906_state_t post_state;    /**< Confirmed hardware state after command execution */
    cmd_verify_result_t result; /**< Final outcome of the verification cycle */
    uint32_t duration_ms;       /**< Total verification latency in milliseconds */
} z906_cmd_verify_info_t;

/**
 * @brief Initializes the command verifier module and its static synchronization primitives.
 * @return true on successful initialization, false otherwise.
 */
bool z906_cmd_verifier_init(void);

/**
 * @brief Centralized entry point executing a command with pre- and post-state verification.
 *
 * This function enforces mutual exclusion, reads pre-state, evaluates preconditions,
 * computes the expected target state, sends physical bytes via UART, waits for confirmed
 * telemetry feedback, compares pre/post states against the target, and logs failures.
 *
 * @param req Pointer to the command request to execute. Must not be NULL.
 * @param out_info Optional pointer to receive comprehensive diagnostic info (may be NULL).
 * @return Result code indicating success or specific failure reason.
 */
cmd_verify_result_t z906_cmd_verifier_execute(const cmd_request_t *req, z906_cmd_verify_info_t *out_info);

/**
 * @brief Returns true if command verifier is currently busy executing a command.
 */
bool z906_cmd_verifier_is_busy(void);

/**
 * @brief Attempts non-blocking acquisition of the verifier UART mutex for telemetry polling.
 * @return true if locked, false if busy or contention.
 */
bool z906_cmd_verifier_try_lock(void);

/**
 * @brief Releases the verifier UART mutex after telemetry polling.
 */
void z906_cmd_verifier_unlock(void);

/**
 * @brief Checks whether the Command Verifier is currently enabled.
 * When disabled, commands are still dispatched via UART with mutex and preconditions,
 * but the post-command verification polling loop is skipped and result is CMD_VERIFY_SENT_UNVERIFIED.
 * @return true if enabled (default), false if disabled.
 */
bool z906_cmd_verifier_is_enabled(void);

/**
 * @brief Sets whether the Command Verifier is enabled and persists the setting to NVS.
 * @param enabled true to enable verification, false to disable.
 * @return true on success.
 */
bool z906_cmd_verifier_set_enabled(bool enabled);

/**
 * @brief Returns a human-readable description for a verification result code.
 * @param result The verification result enum value.
 * @return Constant string description.
 */
const char *z906_cmd_verify_result_str(cmd_verify_result_t result);

/**
 * @brief Returns a human-readable name for a command type.
 * @param type The command category enum value.
 * @return Constant string name.
 */
const char *z906_cmd_verify_type_str(verify_cmd_type_t type);

/**
 * @brief Returns a human-readable name for a command source.
 * @param source The command source enum value.
 * @return Constant string name.
 */
const char *z906_cmd_verify_source_str(cmd_source_t source);

/**
 * @brief Service contract interface for Command Verifier registered in ServiceLocator.
 */
typedef struct {
    cmd_verify_result_t (*execute)(const cmd_request_t *req, z906_cmd_verify_info_t *out_info);
    bool (*is_busy)(void);
    bool (*try_lock)(void);
    void (*unlock)(void);
} z906_cmd_verifier_service_t;

/**
 * @brief Returns the singleton service interface instance.
 */
const z906_cmd_verifier_service_t *z906_cmd_verifier_get_service(void);

#ifdef __cplusplus
}
#endif

#endif /* Z906_CMD_VERIFIER_H */
