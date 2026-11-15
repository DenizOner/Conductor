/**
 * @file event_bus.h
 * @brief Thread-safe FreeRTOS event distribution engine for asynchronous inter-task communication.
 */

#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief System event types processed by the FSM and communication layers */
typedef enum {
    EVENT_NONE = 0,
    EVENT_CONSOLE_RX_BYTE,
    EVENT_AMP_RX_BYTE,
    EVENT_STATE_CHANGED,
    EVENT_CABLE_STATUS_CHANGED
} event_type_t;

/** @brief Channel selectors for individual speaker trim adjustments */
typedef enum {
    TRIM_CHANNEL_BASS = 0,
    TRIM_CHANNEL_CENTER,
    TRIM_CHANNEL_REAR
} trim_channel_t;

/** @brief Unified event payload struct passed across queues */
typedef struct {
    event_type_t type;
    union {
        uint8_t raw_byte;
        bool cable_connected;
    } data;
} z906_event_t;

/**
 * @brief Initializes the EventBus static queue.
 * @return true on success.
 */
bool event_bus_init(void);

/**
 * @brief Posts an event to the EventBus queue.
 * @param event Pointer to event structure to copy into queue.
 * @param wait_ticks Maximum ticks to block waiting for free space.
 * @return true if posted successfully.
 */
bool event_bus_post(const z906_event_t *event, TickType_t wait_ticks);

/**
 * @brief Receives an event from the EventBus queue.
 * @param out_event Pointer to buffer receiving event.
 * @param wait_ticks Maximum ticks to block waiting for an event.
 * @return true if an event was dequeued.
 */
bool event_bus_receive(z906_event_t *out_event, TickType_t wait_ticks);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_H */
