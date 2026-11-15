/**
 * @file service_locator.h
 * @brief Central dependency injection and service registry for the Z906 embedded system.
 *
 * Provides strongly-typed registration and retrieval of core service interfaces and contexts.
 */

#ifndef SERVICE_LOCATOR_H
#define SERVICE_LOCATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Identifier tags for registered system services */
typedef enum {
    SERVICE_ID_UART_HAL = 0,
    SERVICE_ID_ADC_HAL,
    SERVICE_ID_GPIO_HAL,
    SERVICE_ID_FSM,
    SERVICE_ID_STATE_MANAGER,
    SERVICE_ID_ZIGBEE,
    SERVICE_ID_CMD_VERIFIER,
    SERVICE_ID_MAX
} service_id_t;

/**
 * @brief Typed service descriptor registered in ServiceLocator.
 */
typedef struct {
    service_id_t id;
    const char *name;
    void *instance;
    bool is_initialized;
} service_entry_t;

/**
 * @brief Initializes the service locator table.
 */
void service_locator_init(void);

/**
 * @brief Registers a service instance pointer with a given service ID.
 * @param id The service identifier.
 * @param service_ptr Pointer to the service structure or context. Must not be NULL.
 * @return true on success, false if ID is invalid or service_ptr is NULL.
 */
bool service_locator_register(service_id_t id, void *service_ptr);

/**
 * @brief Retrieves a registered service instance pointer.
 * @param id The service identifier.
 * @return Pointer to the service instance, or NULL if not registered.
 */
void *service_locator_get(service_id_t id);

/**
 * @brief Checks if a service ID has been registered.
 * @param id The service identifier.
 * @return true if service is registered and non-null.
 */
bool service_locator_has(service_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_LOCATOR_H */
