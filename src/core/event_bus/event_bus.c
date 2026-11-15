/**
 * @file event_bus.c
 * @brief Implementation of the static FreeRTOS event bus queue.
 */

#include "event_bus.h"
#include "esp_log.h"

#define EVENT_BUS_QUEUE_LENGTH   32
#define EVENT_BUS_ITEM_SIZE      sizeof(z906_event_t)

static const char *TAG = "EVENT_BUS";

static QueueHandle_t s_event_queue = NULL;
static StaticQueue_t s_static_queue_struct;
static uint8_t s_queue_storage[EVENT_BUS_QUEUE_LENGTH * EVENT_BUS_ITEM_SIZE];

bool event_bus_init(void)
{
    if (s_event_queue != NULL) {
        return true;
    }

    s_event_queue = xQueueCreateStatic(
        EVENT_BUS_QUEUE_LENGTH,
        EVENT_BUS_ITEM_SIZE,
        s_queue_storage,
        &s_static_queue_struct
    );

    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create static event queue");
        return false;
    }

    ESP_LOGI(TAG, "Static event bus initialized (Capacity: %d items)", EVENT_BUS_QUEUE_LENGTH);
    return true;
}

bool event_bus_post(const z906_event_t *event, TickType_t timeout_ticks)
{
    if (s_event_queue == NULL || event == NULL) {
        return false;
    }

    if (xQueueSend(s_event_queue, event, timeout_ticks) != pdPASS) {
        ESP_LOGW(TAG, "Event queue full, dropped event: %d", event->type);
        return false;
    }

    return true;
}

bool event_bus_post_from_isr(const z906_event_t *event, BaseType_t *pxHigherPriorityTaskWoken)
{
    if (s_event_queue == NULL || event == NULL) {
        return false;
    }

    return (xQueueSendFromISR(s_event_queue, event, pxHigherPriorityTaskWoken) == pdPASS);
}

bool event_bus_receive(z906_event_t *event, TickType_t timeout_ticks)
{
    if (s_event_queue == NULL || event == NULL) {
        return false;
    }

    return (xQueueReceive(s_event_queue, event, timeout_ticks) == pdTRUE);
}
