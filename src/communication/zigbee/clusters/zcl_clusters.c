/**
 * @file zcl_clusters.c
 * @brief Implementation of cluster metadata and mapping helpers.
 */

#include "zcl_clusters.h"
#include "esp_log.h"

static const char *TAG = "ZCL_CLUSTERS";

void zcl_clusters_log_info(void)
{
    /* EP 1: main (genOnOff + genLevelCtrl)
     * EP 2: trims (0xFC01)
     * EP 3: sources (0xFC02)
     * EP 4: telemetry (0xFC03 — non-temperature)
     * EP 11/12/13: temperature 1/2/3 (standard 0x0402 msTemperatureMeasurement) */
    ESP_LOGI(TAG,
        "Configured ZCL Endpoints — Main:1 Trims:2 Sources:3 Telemetry:4 "
        "Temp1:11 Temp2:12 Temp3(PSU):13");
}
