/**
 * @file adc_hal.h
 * @brief ADC Hardware Abstraction Layer for Z906 line voltages and cable presence.
 */

#ifndef ADC_HAL_H
#define ADC_HAL_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes ESP32-C6 ADC1 in One-Shot mode for channels 1, 2, and 3.
 * @return true on success.
 */
bool z906_adc_hal_init(void);

/**
 * @brief Reads calibrated voltage from a specified ADC1 channel in millivolts.
 * @param channel The ADC1 channel.
 * @param out_mv Pointer to receive the voltage in millivolts.
 * @return true on success.
 */
bool z906_adc_hal_read_voltage(adc_channel_t channel, int *out_mv);

/**
 * @brief Evaluates hardware lines to verify if UART communication is reliable.
 * @return true if console and cable are in stable operational states.
 */
bool z906_adc_hal_is_serial_possible(void);

/**
 * @brief Checks if DE-15 cable is physically plugged in.
 */
bool z906_adc_hal_is_cable_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* ADC_HAL_H */
