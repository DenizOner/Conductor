/**
 * @file zigbee_device_model.h
 * @brief Native ESP Zigbee SDK v2 descriptor tree and attribute data model.
 *
 * Responsibilities:
 *   - Creates and configures the single native device descriptor (ezb_af_device_desc_t).
 *   - Retains the device descriptor handle for the lifetime of the Zigbee stack.
 *   - Builds Endpoint 1 (Main: Basic, Identify, On/Off, Level Control, OTA Upgrade Client).
 *   - Builds Endpoint 2 (Trims: Custom Cluster 0xFC01).
 *   - Builds Endpoint 3 (Sources: Custom Cluster 0xFC02).
 *   - Builds Endpoint 4 (Telemetry: Custom Cluster 0xFC03 - non-temperature telemetry only).
 *   - Builds Endpoints 11, 12, 13 (Temperature Measurement: Standard Cluster 0x0402).
 *   - Provides static backing storage for all cluster attributes.
 *   - Exposes minimal registration API (zigbee_device_model_register) called before esp_zigbee_start.
 */

#ifndef ZIGBEE_DEVICE_MODEL_H
#define ZIGBEE_DEVICE_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include <ezbee/af.h>
#include <ezbee/zcl/zcl_desc.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registers the complete Conductor Zigbee device model with the native stack.
 *
 * Constructs 7 endpoints (EP 1, 2, 3, 4, 11, 12, 13) with all associated standard
 * and custom cluster descriptors and adds them to a single device descriptor,
 * then registers the device descriptor via ezb_af_device_desc_register().
 *
 * This function MUST be called after esp_zigbee_init() and before esp_zigbee_start().
 * It is idempotent at the application level; calling it more than once returns ESP_OK.
 *
 * @return ESP_OK on successful registration, ESP_FAIL otherwise.
 */
esp_err_t zigbee_device_model_register(void);

/**
 * @brief Checks whether the device model has been registered.
 *
 * @return true if successfully registered, false otherwise.
 */
bool zigbee_device_model_is_registered(void);

/**
 * @brief Retrieves the registered native device descriptor handle.
 *
 * @return ezb_af_device_desc_t handle if registered, or EZB_INVALID_AF_DEVICE_DESC otherwise.
 */
ezb_af_device_desc_t zigbee_device_model_get_device_desc(void);

/**
 * @brief Restores the static backing variable of an attribute to its authoritative physical value.
 *
 * Used by command ingress to immediately revert optimistic memory updates performed by the SDK
 * during Write Attributes before physical confirmation arrives from the Logitech Z906.
 *
 * @param ep_id Endpoint identifier.
 * @param cluster_id Cluster identifier.
 * @param attr_id Attribute identifier.
 */
void zigbee_device_model_restore_attr(uint8_t ep_id, uint16_t cluster_id, uint16_t attr_id);

/**
 * @brief Retrieves the registered native attribute descriptor for a given endpoint, cluster, and attribute.
 *
 * Used by the state publisher to obtain direct attribute descriptor handles for ezb_zcl_attr_desc_set_value().
 *
 * @param ep_id Endpoint identifier (1, 2, 3, 4, 11, 12, 13).
 * @param cluster_id Cluster identifier.
 * @param attr_id Attribute identifier.
 * @return ezb_zcl_attr_desc_t handle if found, EZB_INVALID_ZCL_ATTR_DESC otherwise.
 */
ezb_zcl_attr_desc_t zigbee_device_model_get_attr_desc(uint8_t ep_id, uint16_t cluster_id, uint16_t attr_id);

#ifdef __cplusplus
}
#endif

#endif /* ZIGBEE_DEVICE_MODEL_H */
