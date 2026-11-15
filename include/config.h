/**
 * @file config.h
 * @brief Global hardware configuration and pin assignments for
 * "Conductor - Smart Zigbee Console for Logitech z906".
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Firmware Version & Zigbee OTA Identity Standards                          */
/* ========================================================================= */

#define Z906_FW_VERSION_MAJOR 1
#define Z906_FW_VERSION_MINOR 4
#define Z906_FW_VERSION_PATCH 0
#define Z906_FW_VERSION_STR "1.4.0"

/**
 * @brief Canonical 32-bit Zigbee OTA fileVersion encoding formula:
 * (MAJOR << 24) | (MINOR << 16) | (PATCH << 8) | RELEASE_TYPE
 * RELEASE_TYPE: 0x00=dev/prerelease, 0xAA=stable release
 * For 1.4.0 stable: (1 << 24) | (4 << 16) | (0 << 8) | 0xAA = 0x010400AA
 * (17039530)
 */
#define Z906_OTA_FILE_VERSION 0x010400AA

/**
 * @brief Zigbee OTA Manufacturer Code (Espressif CSA Manufacturer Code 0x131B)
 * Used as standard for ESP32-based native Zigbee devices to prevent collision.
 */
#define Z906_OTA_MANUFACTURER_CODE 0x131B

/**
 * @brief Zigbee OTA Image Type identifier for Logitech Z906 controller
 */
#define Z906_OTA_IMAGE_TYPE 0x0906

/* Z906_HARDWARE_VERSION is intentionally undefined.
 * The ESP32-C6 board has no canonical physical hardware revision register or
 * manufacturing metadata that can serve as a hardware version source of truth.
 * All OTA Query Next Image Requests are sent with fc=0x00 (hw_version absent). */

/* ========================================================================= */
/* Hardware GPIO & Peripheral Pin Assignments (ESP32-C6)                     */
/* ========================================================================= */

/** @brief Console power sense / low voltage ADC pin (DE-15 Pin 15) */
#define Z906_PIN_ADC_LOW GPIO_NUM_1
#define Z906_ADC_CH_LOW ADC_CHANNEL_1

/** @brief Console boot sense ADC pin (DE-15 Pin 8) */
#define Z906_PIN_ADC_CON_BOOT GPIO_NUM_2
#define Z906_ADC_CH_CON_BOOT ADC_CHANNEL_2

/** @brief Cable presence detection ADC pin (DE-15 Pin 9) */
#define Z906_PIN_ADC_CABLE_DETECT GPIO_NUM_3
#define Z906_ADC_CH_CABLE_DETECT ADC_CHANNEL_3

/** @brief Console UART Interface (UART1) */
#define Z906_CONSOLE_UART_PORT UART_NUM_1
#define Z906_PIN_CONSOLE_RX GPIO_NUM_16
#define Z906_PIN_CONSOLE_TX GPIO_NUM_17

/** @brief Amplifier UART Interface (UART0) */
#define Z906_AMP_UART_PORT UART_NUM_0
#define Z906_PIN_AMP_RX GPIO_NUM_22
#define Z906_PIN_AMP_TX GPIO_NUM_23

/** @brief Onboard Addressable WS2812 RGB LED (ESP32-C6-DevKitC-1) */
#define Z906_PIN_WS2812_LED GPIO_NUM_8

/* ========================================================================= */
/* Serial Communication Parameters                                           */
/* ========================================================================= */

/** @brief Fixed baud rate for Logitech Z906 protocol */
#define Z906_UART_BAUD_RATE 57600

/** @brief Size of UART software ring buffers in bytes */
#define Z906_UART_RX_BUFFER_SIZE 512
#define Z906_UART_TX_BUFFER_SIZE 512

/** @brief Multi-byte command framing timeout in milliseconds */
#define Z906_SERIAL_TIMEOUT_MS 500

/* ========================================================================= */
/* Audio & Volume Limits                                                     */
/* ========================================================================= */

/** @brief Maximum discrete volume steps supported by Logitech Z906 amplifier */
#define Z906_MAX_VOLUME_LEVEL 43

/** @brief Safe default startup volume level (~20%) */
#define Z906_DEFAULT_VOLUME_LEVEL 9

/* ========================================================================= */
/* Zigbee Device & Endpoint Identification                                   */
/* ========================================================================= */

#define Z906_ZB_MANUFACTURER_NAME "Logitech"
#define Z906_ZB_MODEL_IDENTIFIER "Z-906-Zigbee"

/** @brief Primary audio endpoint (On/Off, Master Level Control, OTA Upgrade
 * Client) */
#define Z906_ZB_ENDPOINT_MAIN 1

/** @brief Secondary trims endpoint (Subwoofer Bass, Center, Rear channels) */
#define Z906_ZB_ENDPOINT_TRIMS 2

/** @brief Source selection endpoint (Audio Inputs 1-6, Stereo 3D Effects) */
#define Z906_ZB_ENDPOINT_SOURCES 3

/* ========================================================================= */
/* Zigbee Commissioning & Network Parameters                                 */
/* ========================================================================= */

/** @brief Zigbee network pairing/steering window timeout in seconds */
#define Z906_ZB_PAIRING_TIMEOUT_S 180

/** @brief Zigbee network rejoin retry period in seconds when coordinator is
 * unreachable */
#define Z906_ZB_REJOIN_INTERVAL_S 30

/* ========================================================================= */
/* Native Zigbee SDK v2 Persistence & Stack Limits                          */
/* ========================================================================= */

/** @brief Dedicated NVS partition name for native Zigbee stack datasets */
#define Z906_ZB_STORAGE_PARTITION_NAME "zb_storage"

/** @brief Maximum child devices supported as Zigbee Router (Espressif v2
 * example baseline) */
#define Z906_ZB_MAX_CHILDREN 10

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
