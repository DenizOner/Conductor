/**
 * @file z906_fsm.h
 * @brief Finite State Machine engine coordinating Z906 operational states and commands.
 */

#ifndef Z906_FSM_H
#define Z906_FSM_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FSM_STATE_UNINITIALIZED = 0,
    FSM_STATE_BOOTING,
    FSM_STATE_STANDBY,
    FSM_STATE_ACTIVE,
    FSM_STATE_MUTED,
    FSM_STATE_SPEAKER_TEST,
    FSM_STATE_FAULT
} z906_fsm_state_t;

/**
 * @brief Initializes the Z906 state machine engine.
 * @return true on success.
 */
bool z906_fsm_init(void);

/**
 * @brief Spawns the static FSM task and waits for it to signal STARTED.
 *
 * Preconditions: z906_fsm_init(), event_bus_init(), z906_state_init() must succeed first.
 * The static task TCB and stack live inside z906_fsm.c (single ownership).
 *
 * Uses an internal StaticSemaphore_t handshake: returns only after the task
 * has signalled that it entered its main loop (STARTED state confirmed).
 *
 * @return ESP_OK if task started, ESP_ERR_NO_MEM if creation failed,
 *         ESP_ERR_TIMEOUT if task did not signal within startup window.
 */
esp_err_t z906_fsm_start(void);

/**
 * @brief Main FSM processing step: consumes one event from the event bus and transitions.
 * @param timeout_ticks Maximum ticks to block waiting for an event.
 */
void z906_fsm_step(TickType_t timeout_ticks);

/**
 * @brief Gets the FSM state reconciled with the latest verified physical state.
 */
z906_fsm_state_t z906_fsm_get_state(void);

/**
 * @brief Returns human-readable state string.
 */
const char *z906_fsm_state_str(z906_fsm_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* Z906_FSM_H */
