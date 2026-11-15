/**
 * @file test_zigbee_publisher.c
 * @brief FD-03 Production C Verification for Native Zigbee State Publisher.
 *
 * Verifies desired vs applied state semantics directly against the production C
 * implementation in src/communication/zigbee/zigbee_state_publisher.c.
 *
 * Covers:
 *   - TC-01 / Test A: Successful apply advances applied, clears pending/dirty.
 *   - TC-02 / Test B: EZB_ERR_INV_STATE preserves old applied, records pending.
 *   - TC-03 / Test C: Same-value deferred suppresses blind periodic retry.
 *   - TC-04 / Test D: Desired value change while pending updates pending to latest.
 *   - TC-05 / Test E: Reconcile (force_sync) success clears pending and applies state.
 *   - TC-06 / Test F: Reconcile failure preserves pending and old applied (no premature clear).
 *   - TC-07 / Test G: Multiple pending attributes with partial failure handling.
 *   - TC-08 / Test H: Health diagnostic attributes apply and defer properly.
 *   - TC-09 / Test I: Temperature encoding precision, range limits, and 0x8000 sentinel.
 */

#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>
#include "esp_err.h"
#include "esp_zigbee.h"
#include <ezbee/af.h>
#include "zigbee_state_publisher.h"

/* ========================================================================= */
/* Test State & Mock SDK Setter Seam                                         */
/* ========================================================================= */

static int s_setter_call_count = 0;
static int s_injected_error_code = EZB_ERR_NONE;

/* Per-attribute error injection for partial failure tests */
static int s_attr_injected_error[ZB_PUB_ATTR_COUNT];

static int mock_attr_desc_set_value(void *desc, const void *val_ptr)
{
    (void)val_ptr;
    s_setter_call_count++;

    /* If per-attribute error is configured, use it */
    for (int i = 0; i < ZB_PUB_ATTR_COUNT; i++) {
        if (desc == (void *)(uintptr_t)(0x1000 + i * 4)) {
            if (s_attr_injected_error[i] != -1) {
                return s_attr_injected_error[i];
            }
            break;
        }
    }

    return s_injected_error_code;
}

void setUp(void)
{
    zigbee_state_publisher_test_reset();
    zigbee_state_publisher_test_set_mock_descs();
    s_setter_call_count = 0;
    s_injected_error_code = EZB_ERR_NONE;
    for (int i = 0; i < ZB_PUB_ATTR_COUNT; i++) {
        s_attr_injected_error[i] = -1; /* Unset */
    }
    zigbee_state_publisher_set_value_fn(mock_attr_desc_set_value);
}

void tearDown(void)
{
    zigbee_state_publisher_set_value_fn(NULL);
    zigbee_state_publisher_test_reset();
}

/* ========================================================================= */
/* TC-01 / Test A: Successful Apply                                          */
/* ========================================================================= */

void test_fd03_a_successful_apply(void)
{
    /* Desired = 30, SDK setter returns EZB_ERR_NONE */
    s_injected_error_code = EZB_ERR_NONE;
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_VOLUME, 30);

    zigbee_state_publisher_test_step(false);

    uint32_t applied = 0;
    bool has_applied = zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_VOLUME, &applied);
    TEST_ASSERT_TRUE_MESSAGE(has_applied, "Applied state must be valid on success");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(30, applied, "Applied state must equal desired value 30");

    uint32_t pending = 0;
    bool is_pending = zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_VOLUME, &pending);
    TEST_ASSERT_FALSE_MESSAGE(is_pending, "Pending/deferred state must be cleared on success");
    TEST_ASSERT_EQUAL_INT(1, s_setter_call_count);
}

/* ========================================================================= */
/* TC-02 / Test B: EZB_ERR_INV_STATE Preserves Old Applied                   */
/* ========================================================================= */

void test_fd03_b_inv_state_preserves_old_applied(void)
{
    /* 1. First establish initial applied value = 20 */
    s_injected_error_code = EZB_ERR_NONE;
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_VOLUME, 20);
    zigbee_state_publisher_test_step(false);

    uint32_t applied = 0;
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_VOLUME, &applied));
    TEST_ASSERT_EQUAL_UINT32(20, applied);

    /* 2. Now desire = 30, but SDK setter returns EZB_ERR_INV_STATE */
    s_injected_error_code = EZB_ERR_INV_STATE;
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_VOLUME, 30);
    zigbee_state_publisher_test_step(false);

    /* INVARIANT: Applied state must NOT advance */
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_VOLUME, &applied));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(20, applied, "Failed write must NOT advance applied state");

    /* Pending state must record desired value 30 */
    uint32_t pending = 0;
    bool is_pending = zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_VOLUME, &pending);
    TEST_ASSERT_TRUE_MESSAGE(is_pending, "Pending state must be marked true on EZB_ERR_INV_STATE");
    TEST_ASSERT_EQUAL_UINT32(30, pending);
}

/* ========================================================================= */
/* TC-03 / Test C: Repeated Temporary Failure Suppresses Blind Retry         */
/* ========================================================================= */

void test_fd03_c_repeated_inv_state_suppresses_blind_retry(void)
{
    s_injected_error_code = EZB_ERR_INV_STATE;
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_VOLUME, 30);

    /* Attempt 1 */
    zigbee_state_publisher_test_step(false);
    TEST_ASSERT_EQUAL_INT(1, s_setter_call_count);

    /* Subsequent periodic cycles with same desired value */
    zigbee_state_publisher_test_step(false);
    zigbee_state_publisher_test_step(false);
    zigbee_state_publisher_test_step(false);

    /* Call count must remain 1: blind retries are suppressed */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_setter_call_count,
        "Periodic cycles must not blindly re-attempt write while value is unchanged and deferred");

    /* Pending value remains valid */
    uint32_t pending = 0;
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_VOLUME, &pending));
    TEST_ASSERT_EQUAL_UINT32(30, pending);
}

/* ========================================================================= */
/* TC-04 / Test D: Desired Value Changes While Pending                       */
/* ========================================================================= */

void test_fd03_d_desired_change_while_pending_updates_pending(void)
{
    s_injected_error_code = EZB_ERR_INV_STATE;
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_VOLUME, 30);
    zigbee_state_publisher_test_step(false);
    TEST_ASSERT_EQUAL_INT(1, s_setter_call_count);

    /* Desired value changes to 35 while stack is still in invalid state */
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_VOLUME, 35);
    zigbee_state_publisher_test_step(false);

    /* Setter must be called again because desired value changed */
    TEST_ASSERT_EQUAL_INT(2, s_setter_call_count);

    /* Pending state must now record the latest desired value (35) */
    uint32_t pending = 0;
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_VOLUME, &pending));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(35, pending, "Pending state must reflect latest desired value");

    /* Applied state still has not advanced */
    uint32_t applied = 0;
    TEST_ASSERT_FALSE_MESSAGE(zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_VOLUME, &applied),
        "Applied state must not exist if write never succeeded");
}

/* ========================================================================= */
/* TC-05 / Test E: Real Reconciliation Success                               */
/* ========================================================================= */

void test_fd03_e_reconcile_success_clears_pending(void)
{
    /* 1. Defer write with EZB_ERR_INV_STATE */
    s_injected_error_code = EZB_ERR_INV_STATE;
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_VOLUME, 30);
    zigbee_state_publisher_test_step(false);

    uint32_t pending = 0;
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_VOLUME, &pending));

    /* 2. Network ready event -> force_full_sync trigger -> SDK setter now succeeds */
    s_injected_error_code = EZB_ERR_NONE;
    zigbee_state_publisher_test_step(true /* force_sync */);

    /* Applied must advance to 30 */
    uint32_t applied = 0;
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_VOLUME, &applied));
    TEST_ASSERT_EQUAL_UINT32(30, applied);

    /* Pending must be cleared */
    TEST_ASSERT_FALSE_MESSAGE(zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_VOLUME, &pending),
        "Pending state must be cleared after successful reconciliation");
}

/* ========================================================================= */
/* TC-06 / Test F: Reconciliation STILL Fails (Critical Anti-Pattern Check)  */
/* ========================================================================= */

void test_fd03_f_reconcile_failure_preserves_pending(void)
{
    /* 1. Set initial applied = 10 */
    s_injected_error_code = EZB_ERR_NONE;
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_VOLUME, 10);
    zigbee_state_publisher_test_step(false);

    /* 2. Desired = 30, deferred on EZB_ERR_INV_STATE */
    s_injected_error_code = EZB_ERR_INV_STATE;
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_VOLUME, 30);
    zigbee_state_publisher_test_step(false);

    uint32_t pending = 0;
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_VOLUME, &pending));
    TEST_ASSERT_EQUAL_UINT32(30, pending);

    /* 3. Reconcile runs (force_sync=true), but setter STILL returns EZB_ERR_INV_STATE */
    s_injected_error_code = EZB_ERR_INV_STATE;
    zigbee_state_publisher_test_step(true /* force_sync */);

    /* INVARIANT: Applied state remains old (10) */
    uint32_t applied = 0;
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_VOLUME, &applied));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(10, applied, "Applied state must remain old value on failed reconcile");

    /* INVARIANT: Pending MUST NOT be cleared before actual successful write */
    TEST_ASSERT_TRUE_MESSAGE(zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_VOLUME, &pending),
        "Pending state MUST NOT be cleared when reconciliation write fails");
    TEST_ASSERT_EQUAL_UINT32(30, pending);
}

/* ========================================================================= */
/* TC-07 / Test G: Multiple Pending Attributes with Partial Success          */
/* ========================================================================= */

void test_fd03_g_multiple_pending_attributes_partial_failure(void)
{
    /* 1. Both Volume (desired=30) and TrimBass (desired=25) deferred */
    s_injected_error_code = EZB_ERR_INV_STATE;
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_VOLUME, 30);
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_TRIM_BASS, 25);
    zigbee_state_publisher_test_step(false);

    uint32_t pending_vol = 0;
    uint32_t pending_bass = 0;
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_VOLUME, &pending_vol));
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_TRIM_BASS, &pending_bass));

    /* 2. Reconcile with partial success: Volume succeeds, TrimBass still fails */
    s_attr_injected_error[ZB_PUB_ATTR_VOLUME]    = EZB_ERR_NONE;
    s_attr_injected_error[ZB_PUB_ATTR_TRIM_BASS] = EZB_ERR_INV_STATE;

    zigbee_state_publisher_test_step(true /* force_sync */);

    /* Volume: applied=30, pending cleared */
    uint32_t applied_vol = 0;
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_VOLUME, &applied_vol));
    TEST_ASSERT_EQUAL_UINT32(30, applied_vol);
    TEST_ASSERT_FALSE(zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_VOLUME, &pending_vol));

    /* TrimBass: applied not set (or old), pending still true = 25 */
    uint32_t applied_bass = 0;
    TEST_ASSERT_FALSE(zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_TRIM_BASS, &applied_bass));
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_TRIM_BASS, &pending_bass));
    TEST_ASSERT_EQUAL_UINT32(25, pending_bass);
}

/* ========================================================================= */
/* TC-08 / Test H: Health Diagnostics Attributes Apply & Defer               */
/* ========================================================================= */

void test_fd03_h_health_attributes_apply_and_defer(void)
{
    /* Set desired for all 4 health attributes:
     * Device Health = 2 (degraded)
     * Primary Component = 4 (memory)
     * Primary Reason = 9 (heap_warning)
     * SoC Temperature = 4550 (45.50 °C)
     */
    s_injected_error_code = EZB_ERR_NONE;
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_DEVICE_HEALTH, 2);
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_HEALTH_PRIMARY_COMP, 4);
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_HEALTH_PRIMARY_REAS, 9);
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_SOC_TEMP, 4550);

    zigbee_state_publisher_test_step(false);

    uint32_t app_health = 0, app_comp = 0, app_reas = 0, app_temp = 0;
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_DEVICE_HEALTH, &app_health));
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_HEALTH_PRIMARY_COMP, &app_comp));
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_HEALTH_PRIMARY_REAS, &app_reas));
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_SOC_TEMP, &app_temp));

    TEST_ASSERT_EQUAL_UINT32(2, app_health);
    TEST_ASSERT_EQUAL_UINT32(4, app_comp);
    TEST_ASSERT_EQUAL_UINT32(9, app_reas);
    TEST_ASSERT_EQUAL_UINT32(4550, app_temp);

    /* Reason changes to 10 (heap_critical) while stack in EZB_ERR_INV_STATE */
    s_injected_error_code = EZB_ERR_INV_STATE;
    zigbee_state_publisher_test_set_desired(ZB_PUB_ATTR_HEALTH_PRIMARY_REAS, 10);
    zigbee_state_publisher_test_step(false);

    /* Applied remains 9 */
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_get_applied(ZB_PUB_ATTR_HEALTH_PRIMARY_REAS, &app_reas));
    TEST_ASSERT_EQUAL_UINT32(9, app_reas);

    /* Pending records 10 */
    uint32_t pending_reas = 0;
    TEST_ASSERT_TRUE(zigbee_state_publisher_test_is_pending_deferred(ZB_PUB_ATTR_HEALTH_PRIMARY_REAS, &pending_reas));
    TEST_ASSERT_EQUAL_UINT32(10, pending_reas);
}

/* ========================================================================= */
/* TC-09 / Test I: Temperature Encoding Precision & Sentinel Handling        */
/* ========================================================================= */

void test_fd03_i_encode_temperature_sentinel_and_precision(void)
{
    /* Valid cases with 0.01 °C resolution */
    TEST_ASSERT_EQUAL_INT16(2500, zigbee_state_publisher_encode_temperature(25.0f, true));
    TEST_ASSERT_EQUAL_INT16(2550, zigbee_state_publisher_encode_temperature(25.5f, true));
    TEST_ASSERT_EQUAL_INT16(0, zigbee_state_publisher_encode_temperature(0.0f, true));
    TEST_ASSERT_EQUAL_INT16(-525, zigbee_state_publisher_encode_temperature(-5.25f, true));

    /* Invalid cases return 0x8000 (-32768) */
    TEST_ASSERT_EQUAL_INT16(-32768, zigbee_state_publisher_encode_temperature(25.0f, false));
    TEST_ASSERT_EQUAL_INT16(-32768, zigbee_state_publisher_encode_temperature(0.0f, false));
    TEST_ASSERT_EQUAL_INT16(-32768, zigbee_state_publisher_encode_temperature(NAN, true));
    TEST_ASSERT_EQUAL_INT16(-32768, zigbee_state_publisher_encode_temperature(INFINITY, true));
    TEST_ASSERT_EQUAL_INT16(-32768, zigbee_state_publisher_encode_temperature(-INFINITY, true));
    TEST_ASSERT_EQUAL_INT16(-32768, zigbee_state_publisher_encode_temperature(350.0f, true));  /* Overflow */
}

/* ========================================================================= */
/* Test Runner Entry Point                                                   */
/* ========================================================================= */

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow serial monitor to open */
    UNITY_BEGIN();

    RUN_TEST(test_fd03_a_successful_apply);
    RUN_TEST(test_fd03_b_inv_state_preserves_old_applied);
    RUN_TEST(test_fd03_c_repeated_inv_state_suppresses_blind_retry);
    RUN_TEST(test_fd03_d_desired_change_while_pending_updates_pending);
    RUN_TEST(test_fd03_e_reconcile_success_clears_pending);
    RUN_TEST(test_fd03_f_reconcile_failure_preserves_pending);
    RUN_TEST(test_fd03_g_multiple_pending_attributes_partial_failure);
    RUN_TEST(test_fd03_h_health_attributes_apply_and_defer);
    RUN_TEST(test_fd03_i_encode_temperature_sentinel_and_precision);

    UNITY_END();
}
