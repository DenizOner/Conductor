/**
 * @file adc_hal.c
 * @brief Implementation of calibrated ADC1 One-Shot line sensing.
 */

#include "adc_hal.h"
#include "config.h"
#include "esp_log.h"

static const char *TAG = "ADC_HAL";

static adc_oneshot_unit_handle_t s_adc1_handle = NULL;

bool z906_adc_hal_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    if (adc_oneshot_new_unit(&init_config, &s_adc1_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ADC1 unit");
        return false;
    }

    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };

    adc_oneshot_config_channel(s_adc1_handle, Z906_ADC_CH_LOW, &chan_config);
    adc_oneshot_config_channel(s_adc1_handle, Z906_ADC_CH_CON_BOOT, &chan_config);
    adc_oneshot_config_channel(s_adc1_handle, Z906_ADC_CH_CABLE_DETECT, &chan_config);

    ESP_LOGI(TAG, "ADC1 channels 1, 2, 3 configured with 12dB attenuation");
    return true;
}

bool z906_adc_hal_read_voltage(adc_channel_t channel, int *out_mv)
{
    if (s_adc1_handle == NULL || out_mv == NULL) {
        return false;
    }

    int raw = 0;
    if (adc_oneshot_read(s_adc1_handle, channel, &raw) != ESP_OK) {
        return false;
    }

    /* Standard 3.3V reference mapping for 12-bit ADC (4095) */
    *out_mv = (raw * 3300) / 4095;
    return true;
}

bool z906_adc_hal_is_cable_connected(void)
{
    int mv = 0;
    if (!z906_adc_hal_read_voltage(Z906_ADC_CH_CABLE_DETECT, &mv)) {
        return false;
    }
    return (mv >= 2800);
}

bool z906_adc_hal_is_serial_possible(void)
{
    int mv_con_boot = 0;
    int mv_cable = 0;
    int mv_low = 0;

    if (!z906_adc_hal_read_voltage(Z906_ADC_CH_CON_BOOT, &mv_con_boot) ||
        !z906_adc_hal_read_voltage(Z906_ADC_CH_CABLE_DETECT, &mv_cable) ||
        !z906_adc_hal_read_voltage(Z906_ADC_CH_LOW, &mv_low)) {
        return false;
    }

    /* Console transient startup or power-off phase (>3.0V) */
    if (mv_con_boot > 3000) {
        return false;
    }

    /* Cable disconnected (<2.8V) */
    if (mv_cable < 2800) {
        return false;
    }

    /* Console powered off (~0.5V on line 15) vs active (0.0V) */
    if (mv_low > 150) {
        return false;
    }

    return true;
}
