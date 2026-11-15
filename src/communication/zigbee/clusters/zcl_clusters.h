/**
 * @file zcl_clusters.h
 * @brief Zigbee Cluster Library (ZCL) attribute and endpoint declarations for Logitech Z906.
 *
 * Temperature Architecture (Seçenek C / Option C — standard 0x0402 cluster):
 *   Each of the three Z906 internal temperatures is reported on its own dedicated
 *   sub-endpoint using the STANDARD ZCL Temperature Measurement cluster (0x0402).
 *   This allows ZHA and other standard Zigbee coordinators to auto-discover the
 *   sensors without requiring the external converter.
 *
 *   Endpoint layout:
 *     EP 1  (main)      — genOnOff + genLevelCtrl + genOtaUpgrade (Client)
 *     EP 2  (trims)     — 0xFC01 Trims & Mute
 *     EP 3  (sources)   — 0xFC02 Input & Effect
 *     EP 4  (telemetry) — 0xFC03 Telemetry, Feature Toggles & System Diagnostics
 *     EP 11 (temp1)     — msTemperatureMeasurement (Temperature 1)
 *     EP 12 (temp2)     — msTemperatureMeasurement (Temperature 2)
 *     EP 13 (temp3)     — msTemperatureMeasurement (Temperature 3, Power Supply Transformer)
 *
 *   Standard ZCL 0x0402 encoding:
 *     measuredValue (int16_t) = temperature_celsius × 100
 *     Invalid / unavailable sentinel = 0x8000 (-32768)
 */

#ifndef ZCL_CLUSTERS_H
#define ZCL_CLUSTERS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Standard ZCL Cluster Identifiers                                          */
/* ========================================================================= */

#define ZCL_CLUSTER_BASIC                   0x0000
#define ZCL_CLUSTER_ON_OFF                  0x0006
#define ZCL_CLUSTER_LEVEL_CONTROL           0x0008
#define ZCL_CLUSTER_OTA_UPGRADE             0x0019 /**< Standard ZCL OTA Upgrade cluster */
#define ZCL_CLUSTER_TEMP_MEASUREMENT        0x0402 /**< Standard ZCL Temperature Measurement cluster */

/* ========================================================================= */
/* Standard ZCL OTA Upgrade Cluster (0x0019) Command & Status Identifiers     */
/* ========================================================================= */

#define ZCL_CMD_OTA_IMAGE_NOTIFY            0x00
#define ZCL_CMD_OTA_QUERY_NEXT_IMAGE_REQ    0x01
#define ZCL_CMD_OTA_QUERY_NEXT_IMAGE_RESP   0x02
#define ZCL_CMD_OTA_IMAGE_BLOCK_REQ         0x03
#define ZCL_CMD_OTA_IMAGE_BLOCK_RESP        0x05
#define ZCL_CMD_OTA_UPGRADE_END_REQ         0x06
#define ZCL_CMD_OTA_UPGRADE_END_RESP        0x07

#define ZCL_STATUS_OTA_SUCCESS              0x00
#define ZCL_STATUS_OTA_NO_IMAGE_AVAILABLE   0x98

/* ========================================================================= */
/* Logitech Z906 Manufacturer-Specific Cluster Identifiers                   */
/* ========================================================================= */

#define ZCL_CLUSTER_Z906_TRIMS              0xFC01 /**< Channel trims & mute audio control */
#define ZCL_CLUSTER_Z906_SOURCES            0xFC02 /**< Input source & spatial effect */
#define ZCL_CLUSTER_Z906_TELEMETRY          0xFC03 /**< Telemetry, toggles & diagnostic exposure */

/* ========================================================================= */
/* Endpoint Number Constants                                                  */
/* ========================================================================= */

#define Z906_ZB_EP_MAIN                     1  /**< genOnOff + genLevelCtrl + genOtaUpgrade (Client) */
#define Z906_ZB_EP_TRIMS                    2  /**< 0xFC01 Trims & Mute */
#define Z906_ZB_EP_SOURCES                  3  /**< 0xFC02 Input & Effect */
#define Z906_ZB_EP_TELEMETRY                4  /**< 0xFC03 Telemetry & Diagnostics */
#define Z906_ZB_EP_TEMP1                    11 /**< Standard 0x0402 — Temperature 1 */
#define Z906_ZB_EP_TEMP2                    12 /**< Standard 0x0402 — Temperature 2 */
#define Z906_ZB_EP_TEMP3                    13 /**< Standard 0x0402 — Temperature 3 (Power Supply Transformer) */

/* ========================================================================= */
/* Standard ZCL 0x0402 Temperature Measurement Attribute & Constants         */
/* ========================================================================= */

/** @brief Standard ZCL measuredValue attribute ID (int16_t, °C × 100). */
#define ZCL_ATTR_TEMP_MEASURED_VALUE        0x0000

/**
 * @brief ZCL Temperature Measurement cluster invalid/unavailable sentinel.
 *
 * Per ZCL spec section 4.4.2.1: when no valid measurement is available,
 * the measuredValue attribute must be set to 0x8000.
 * This replaces the firmware-level temp_valid=false state transparently at
 * the protocol level — ZHA and HA display "unavailable" instead of 0 °C.
 */
#define ZCL_TEMP_INVALID_VALUE              ((int16_t)(-32768)) /* 0x8000 */

/* ========================================================================= */
/* Custom Cluster 0xFC01 — Trims & Audio Control Attribute IDs               */
/* ========================================================================= */

#define ZCL_ATTR_TRIM_BASS                  0x0000
#define ZCL_ATTR_TRIM_CENTER                0x0001
#define ZCL_ATTR_TRIM_REAR                  0x0002
#define ZCL_ATTR_AUDIO_MUTE                 0x0003

/* ========================================================================= */
/* Custom Cluster 0xFC02 — Source Selection Attribute IDs                    */
/* ========================================================================= */

#define ZCL_ATTR_SOURCE_INPUT               0x0000
#define ZCL_ATTR_SOURCE_EFFECT              0x0001

/* ========================================================================= */
/* Custom Cluster 0xFC03 — Telemetry, Toggles & System Diagnostics           */
/*                                                                           */
/* NOTE: Amplifier heatsink temperatures (Temp 1/2/3) reside on standard     */
/* 0x0402 sub-endpoints 11, 12, 13. Attributes 0x0004-0x0006 remain absent.  */
/* ========================================================================= */

#define ZCL_ATTR_INPUT_SIGNAL_LEVEL         0x0000  /**< Raw U24BE input volume/signal amplitude */
#define ZCL_ATTR_IDLE_TIME                  0x0001  /**< U24BE idle time in seconds */
#define ZCL_ATTR_DIGITAL_SIGNAL_STATUS      0x0002  /**< Byte 12 / Decode state code */
#define ZCL_ATTR_AUTO_STANDBY               0x0003  /**< 1 if auto-standby enabled, 0 if disabled */
#define ZCL_ATTR_BUILT_IN_LED_ENABLED       0x0007  /**< 1 if onboard LED notifications enabled, 0 if disabled */
#define ZCL_ATTR_CMD_VERIFIER_ENABLED       0x0008  /**< 1 if state-based command verification enabled, 0 if disabled */

/* Health Manager Selective Diagnostics (Cluster 0xFC03) */
#define ZCL_ATTR_DEVICE_HEALTH              0x0009  /**< Overall system health: uint8 (0=unknown, 1=healthy, 2=degraded, 3=fault) */
#define ZCL_ATTR_HEALTH_PRIMARY_COMPONENT   0x000A  /**< Primary failing/degraded component: uint8 enum */
#define ZCL_ATTR_HEALTH_PRIMARY_REASON      0x000B  /**< Primary root cause reason code: uint8 enum */
#define ZCL_ATTR_SOC_TEMPERATURE            0x000C  /**< ESP32-C6 internal die temp: int16_t (°C × 100, 0x8000=invalid) */

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CLUSTERS_H */
