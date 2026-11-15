/**
 * @file service_locator.c
 * @brief Implementation of the central Service Locator registry.
 */

#include "service_locator.h"
#include "esp_log.h"

static const char *TAG = "SERVICE_LOCATOR";

static void *s_services[SERVICE_ID_MAX];

void service_locator_init(void)
{
    for (int i = 0; i < SERVICE_ID_MAX; i++) {
        s_services[i] = NULL;
    }
    ESP_LOGI(TAG, "Service locator registry initialized");
}

bool service_locator_register(service_id_t id, void *service_ptr)
{
    if ((int)id < 0 || id >= SERVICE_ID_MAX || service_ptr == NULL) {
        ESP_LOGE(TAG, "Invalid service registration (id: %d)", id);
        return false;
    }

    s_services[id] = service_ptr;
    ESP_LOGD(TAG, "Registered service id %d", id);
    return true;
}

void *service_locator_get(service_id_t id)
{
    if ((int)id < 0 || id >= SERVICE_ID_MAX) {
        ESP_LOGE(TAG, "Attempt to query out-of-bounds service id: %d", id);
        return NULL;
    }

    return s_services[id];
}

bool service_locator_has(service_id_t id)
{
    if ((int)id < 0 || id >= SERVICE_ID_MAX) {
        return false;
    }
    return (s_services[id] != NULL);
}
