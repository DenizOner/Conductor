/**
 * @file z906_cmd_registry.h
 * @brief Centralized command registry and metadata table for Logitech Z906.
 *
 * This module serves as the single source of truth for all command definitions,
 * parameter validation boundaries, CLI keywords, and Zigbee ZCL mappings across
 * the firmware. It directly interfaces with the state-based command verifier.
 */

#ifndef Z906_CMD_REGISTRY_H
#define Z906_CMD_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "z906_cmd_verifier.h"
#include "zcl_clusters.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Unique identifier for every supported Logitech Z906 control command.
 */
typedef enum {
    Z906_CMD_ID_POWER = 0,       /**< Main power state control (0 = Standby, 1 = ON) */
    Z906_CMD_ID_VOLUME,          /**< Master volume level in discrete steps (0 - 43) */
    Z906_CMD_ID_MUTE,            /**< Amplifier audio mute control (0 = Unmuted, 1 = Muted) */
    Z906_CMD_ID_INPUT,           /**< Audio input source selection (1 - 6) */
    Z906_CMD_ID_EFFECT,          /**< Spatial audio processing effect (0 = 3D, 1 = 2.1, 2 = 4.1, 3 = None) */
    Z906_CMD_ID_TRIM_BASS,       /**< Subwoofer bass channel volume level (0 - 43) */
    Z906_CMD_ID_TRIM_CENTER,     /**< Center speaker channel volume level (0 - 43) */
    Z906_CMD_ID_TRIM_REAR,       /**< Rear surround speaker channels volume level (0 - 43) */
    Z906_CMD_COUNT,              /**< Total number of registered control commands */
    Z906_CMD_ID_INVALID = -1     /**< Invalid / unknown command identifier */
} z906_cmd_id_t;

/**
 * @brief Parameter data type classifications.
 */
typedef enum {
    PARAM_TYPE_BOOL = 0,         /**< Boolean value (0 or 1) */
    PARAM_TYPE_INT,              /**< Integer value within [min_value, max_value] */
    PARAM_TYPE_ENUM              /**< Discrete enumeration value within [min_value, max_value] */
} z906_param_type_t;

/**
 * @brief Comprehensive descriptor defining a single Logitech Z906 control command.
 */
typedef struct {
    z906_cmd_id_t id;            /**< Unique command identifier */
    const char *name;            /**< Canonical command name */
    const char *cli_cmd;         /**< Primary CLI command keyword */
    const char *cli_subcmd;      /**< Optional secondary CLI sub-command keyword (e.g., "bass") or NULL */
    z906_param_type_t param_type;/**< Parameter type classification */
    int32_t min_val;             /**< Minimum allowable parameter value */
    int32_t max_val;             /**< Maximum allowable parameter value */
    uint8_t zcl_endpoint;        /**< Associated Zigbee endpoint (1, 2, or 3) */
    uint16_t zcl_cluster;        /**< Associated Zigbee ZCL cluster ID */
    uint16_t zcl_attr;           /**< Associated Zigbee ZCL attribute ID */
    verify_cmd_type_t verify_type;/**< Corresponding state verifier command category */
    trim_channel_t trim_channel; /**< Trim speaker channel if verify_type == VERIFY_CMD_TRIM */
    bool is_verifiable;          /**< True if hardware telemetry can confirm command execution */
    const char *description;     /**< Human-readable description */
} z906_cmd_descriptor_t;

/**
 * @brief Returns the total number of registered commands in the centralized table.
 * @return Count of registered command descriptors.
 */
size_t z906_cmd_registry_get_count(void);

/**
 * @brief Returns the command descriptor at the specified index.
 * @param index 0-based array index (must be < z906_cmd_registry_get_count()).
 * @return Pointer to descriptor, or NULL if index is out of bounds.
 */
const z906_cmd_descriptor_t *z906_cmd_registry_get_descriptor(size_t index);

/**
 * @brief Finds a command descriptor by its unique command ID.
 * @param id The command ID enum value.
 * @return Pointer to descriptor, or NULL if not found.
 */
const z906_cmd_descriptor_t *z906_cmd_registry_find_by_id(z906_cmd_id_t id);

/**
 * @brief Finds a command descriptor matching CLI syntax.
 * @param cmd_name Primary command string (e.g., "power", "volume", "trim").
 * @param subcmd Optional sub-command string (e.g., "bass", "center", "rear"), or NULL.
 * @return Pointer to descriptor, or NULL if no match exists.
 */
const z906_cmd_descriptor_t *z906_cmd_registry_find_by_cli_name(const char *cmd_name, const char *subcmd);

/**
 * @brief Finds a command descriptor matching Zigbee ZCL coordinate triple.
 * @param endpoint Zigbee endpoint number (e.g., 1, 2, 3).
 * @param cluster Zigbee cluster ID (e.g., 0x0006, 0x0008, 0xFC01, 0xFC02).
 * @param attr Zigbee attribute ID (e.g., 0x0000, 0x0001, etc.).
 * @return Pointer to descriptor, or NULL if no matching attribute exists.
 */
const z906_cmd_descriptor_t *z906_cmd_registry_find_by_zcl(uint8_t endpoint, uint16_t cluster, uint16_t attr);

/**
 * @brief Validates a raw parameter value against the boundaries defined in the registry.
 * @param id The command ID to validate against.
 * @param value The value to validate.
 * @return true if parameter falls within [min_val, max_val], false otherwise.
 */
bool z906_cmd_registry_validate_param(z906_cmd_id_t id, int32_t value);

/**
 * @brief Constructs a typed cmd_request_t payload ready for z906_cmd_verifier_execute().
 *
 * This bridges the central registry with the state-based verification engine,
 * guaranteeing consistent parameter formatting and source assignment.
 *
 * @param id Command ID from the central registry.
 * @param source Originating source (CMD_SOURCE_CLI or CMD_SOURCE_ZIGBEE).
 * @param param_val Validated parameter value.
 * @param out_req Pointer to destination cmd_request_t structure. Must not be NULL.
 * @return true on successful construction, false if parameters or ID are invalid.
 */
bool z906_cmd_registry_build_request(z906_cmd_id_t id,
                                     cmd_source_t source,
                                     int32_t param_val,
                                     cmd_request_t *out_req);

/**
 * @brief Returns the canonical string representation for a command ID.
 * @param id The command ID enum value.
 * @return Constant string name (e.g., "power", "volume").
 */
const char *z906_cmd_registry_id_to_str(z906_cmd_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* Z906_CMD_REGISTRY_H */
