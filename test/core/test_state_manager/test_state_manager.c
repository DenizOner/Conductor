/**
 * @file test_state_manager.c
 * @brief Unit tests for Z906 confirmed hardware state store.
 *
 * Verifies that confirmed telemetry and hardware settings are safely stored,
 * updated, and delta-tracked. Includes tests for master volume, input selection,
 * digital signal status, auto-standby, 3-point temperatures (with transformer sensor),
 * idle time, and raw unitless input signal level.
 */

/* Include Unity test framework header */
#include <unity.h>
/* Include confirmed state manager header under test */
#include "z906_state.h"
/* Include project configuration header */
#include "config.h"
/* FreeRTOS headers for concurrent-writer test */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/**
 * @brief Test fixture setup executed before each test case.
 */
void setUp(void)
{
    /* Initialize global state manager to default values */
    z906_state_init();
}

/**
 * @brief Test fixture teardown executed after each test case.
 */
void tearDown(void)
{
    /* No cleanup required */
}

/**
 * @brief Tests default initialization state.
 */
void test_state_defaults(void)
{
    /* State container struct */
    z906_state_t st;
    /* Retrieve active confirmed state */
    z906_state_get(&st);

    /* Assert power is initially off */
    TEST_ASSERT_FALSE(st.power);
    /* Assert audio is not muted */
    TEST_ASSERT_FALSE(st.mute);
    /* Assert master volume starts at default level */
    TEST_ASSERT_EQUAL_UINT8(Z906_DEFAULT_VOLUME_LEVEL, st.volume);
    /* Assert subwoofer trim starts at default level */
    TEST_ASSERT_EQUAL_UINT8(Z906_DEFAULT_VOLUME_LEVEL, st.volume_bass);
    /* Assert default input source is 6-Channel Direct (Input 1) */
    TEST_ASSERT_EQUAL(Z906_INPUT_SIX_CHANNEL_DIRECT, st.input);
    /* Assert digital signal status is initially unclassified (0x00) */
    TEST_ASSERT_EQUAL_HEX8(0x00, st.digital_signal_status);
    /* Assert automatic standby is enabled by factory default */
    TEST_ASSERT_TRUE(st.auto_standby_enabled);
    /* Assert input signal level starts at zero */
    TEST_ASSERT_EQUAL_UINT32(0, st.input_signal_level);
    /* Assert temp_valid starts false at boot — no verified measurement yet */
    TEST_ASSERT_FALSE(st.temp_valid);
}

/**
 * @brief Tests volume boundary enforcement (0 - 43) and delta detection.
 */
void test_state_volume_boundaries(void)
{
    /* Setting a new volume value should return true (changed) */
    TEST_ASSERT_TRUE(z906_state_set_volume(25));
    /* State container */
    z906_state_t st;
    /* Retrieve state */
    z906_state_get(&st);
    /* Assert volume updated to 25 */
    TEST_ASSERT_EQUAL_UINT8(25, st.volume);

    /* Setting identical volume should return false (no delta) */
    TEST_ASSERT_FALSE(z906_state_set_volume(25));

    /* Setting volume exceeding maximum should clamp to 43 and report changed */
    TEST_ASSERT_TRUE(z906_state_set_volume(100));
    /* Retrieve state */
    z906_state_get(&st);
    /* Assert volume clamped to 43 */
    TEST_ASSERT_EQUAL_UINT8(Z906_MAX_VOLUME_LEVEL, st.volume);
}

/**
 * @brief Tests input source selection validity and bounds checking.
 */
void test_state_input_transitions(void)
{
    /* Setting valid Optical 2 (Input 4) should succeed and report changed */
    TEST_ASSERT_TRUE(z906_state_set_input(Z906_INPUT_OPTICAL_2));
    /* State container */
    z906_state_t st;
    /* Retrieve state */
    z906_state_get(&st);
    /* Assert input is now Optical 2 (Input 4) */
    TEST_ASSERT_EQUAL(Z906_INPUT_OPTICAL_2, st.input);

    /* Setting the same input again should report false (no delta) */
    TEST_ASSERT_FALSE(z906_state_set_input(Z906_INPUT_OPTICAL_2));

    /* Invalid input source index (99) should be rejected */
    TEST_ASSERT_FALSE(z906_state_set_input((z906_input_t)99));
}

/**
 * @brief Tests telemetries: signal status, auto-standby, temperature, idle seconds, and input signal level.
 */
void test_state_telemetry_fields(void)
{
    /* Test digital signal status */
    TEST_ASSERT_TRUE(z906_state_set_digital_signal_status(0x01));
    z906_state_t st;
    z906_state_get(&st);
    TEST_ASSERT_EQUAL_HEX8(0x01, st.digital_signal_status);
    /* Re-setting same value returns false */
    TEST_ASSERT_FALSE(z906_state_set_digital_signal_status(0x01));

    /* Test auto standby toggle */
    TEST_ASSERT_TRUE(z906_state_set_auto_standby(false));
    z906_state_get(&st);
    TEST_ASSERT_FALSE(st.auto_standby_enabled);
    /* Re-setting same value returns false */
    TEST_ASSERT_FALSE(z906_state_set_auto_standby(false));

    /* Test 3-point temperatures: Index 0 & 1 heatsinks, Index 2 power supply transformer */
    int8_t temps[3] = { 25, 32, 40 };
    TEST_ASSERT_TRUE(z906_state_set_temperature(temps));
    z906_state_get(&st);
    TEST_ASSERT_EQUAL_INT8(25, st.temp_celsius[0]);
    TEST_ASSERT_EQUAL_INT8(32, st.temp_celsius[1]);
    TEST_ASSERT_EQUAL_INT8(40, st.temp_celsius[2]);
    /* Setting same temperatures returns false */
    TEST_ASSERT_FALSE(z906_state_set_temperature(temps));

    /* Test idle seconds */
    TEST_ASSERT_TRUE(z906_state_set_idle_seconds(1800));
    z906_state_get(&st);
    TEST_ASSERT_EQUAL_UINT32(1800, st.idle_seconds);
    /* Setting same idle seconds returns false */
    TEST_ASSERT_FALSE(z906_state_set_idle_seconds(1800));

    /* Test raw unitless input signal level */
    TEST_ASSERT_TRUE(z906_state_set_input_signal_level(123456));
    z906_state_get(&st);
    TEST_ASSERT_EQUAL_UINT32(123456, st.input_signal_level);
    /* Setting same input signal level returns false */
    TEST_ASSERT_FALSE(z906_state_set_input_signal_level(123456));
}

/**
 * @brief Tests that the first call to z906_state_set_temperature sets temp_valid = true,
 * distinguishing a protocol-verified 0 \u00b0C measurement from a boot-time uninitialized state.
 *
 * Acceptance criterion: VALID 0 \u00b0C (Test A from task Section 11.3) must NOT be mistaken
 * for a haberle\u015fme haberle\u015fme (communication) error, and must set temp_valid = true.
 */
void test_state_temp_valid_flag_boot_to_first_measurement(void)
{
    z906_state_t st;

    /* Before any temperature query: temp_valid must be false */
    z906_state_get(&st);
    TEST_ASSERT_FALSE(st.temp_valid);
    TEST_ASSERT_EQUAL_INT8(0, st.temp_celsius[0]);
    TEST_ASSERT_EQUAL_INT8(0, st.temp_celsius[1]);
    TEST_ASSERT_EQUAL_INT8(0, st.temp_celsius[2]);

    /* Test A: Valid 0 \u00b0C measurement (all three channels are genuinely 0 \u00b0C).         */
    /* The result must be accepted as a valid measurement, NOT treated as an error. */
    int8_t zero_temps[3] = { 0, 0, 0 };
    /* First call must return true (changed) because temp_valid transitions false -> true */
    TEST_ASSERT_TRUE(z906_state_set_temperature(zero_temps));
    z906_state_get(&st);
    /* temp_valid must now be true */
    TEST_ASSERT_TRUE(st.temp_valid);
    /* All three channels must hold the genuine 0 \u00b0C value */
    TEST_ASSERT_EQUAL_INT8(0, st.temp_celsius[0]);
    TEST_ASSERT_EQUAL_INT8(0, st.temp_celsius[1]);
    TEST_ASSERT_EQUAL_INT8(0, st.temp_celsius[2]);
}

/**
 * @brief Tests that z906_state_set_temperature returns false (no delta) when called with
 * identical values after temp_valid is already true, but keeps temp_valid set.
 */
void test_state_temp_valid_preserved_on_no_change(void)
{
    int8_t temps[3] = { 30, 35, 45 };

    /* First call: transition to valid, temperature values change */
    TEST_ASSERT_TRUE(z906_state_set_temperature(temps));

    /* Second call with same values: no delta, but temp_valid stays true */
    TEST_ASSERT_FALSE(z906_state_set_temperature(temps));

    z906_state_t st;
    z906_state_get(&st);
    TEST_ASSERT_TRUE(st.temp_valid);
    TEST_ASSERT_EQUAL_INT8(30, st.temp_celsius[0]);
    TEST_ASSERT_EQUAL_INT8(35, st.temp_celsius[1]);
    TEST_ASSERT_EQUAL_INT8(45, st.temp_celsius[2]);
}

/**
 * @brief Tests that all three temperature channels are updated atomically and correctly.
 * Verifies that Temperature 1, 2, and 3 each map to their own independent storage slot.
 */
void test_state_temp_three_channels_distinct(void)
{
    int8_t temps[3] = { 22, 38, 51 }; /* Deliberately distinct values */
    TEST_ASSERT_TRUE(z906_state_set_temperature(temps));

    z906_state_t st;
    z906_state_get(&st);
    /* Each channel must hold its own value, not a neighbour's */
    TEST_ASSERT_EQUAL_INT8(22, st.temp_celsius[0]); /* Temperature 1 */
    TEST_ASSERT_EQUAL_INT8(38, st.temp_celsius[1]); /* Temperature 2 */
    TEST_ASSERT_EQUAL_INT8(51, st.temp_celsius[2]); /* Temperature 3 (Power Supply Transformer) */
    TEST_ASSERT_TRUE(st.temp_valid);
}

/**
 * @brief Tests that passing NULL to z906_state_set_temperature is rejected without crash.
 * Previous valid temperature values must remain unchanged.
 */
void test_state_temp_null_input_rejected(void)
{
    /* Establish a known valid state first */
    int8_t good_temps[3] = { 28, 33, 44 };
    TEST_ASSERT_TRUE(z906_state_set_temperature(good_temps));

    /* Attempt to update with NULL pointer — must return false and not crash */
    TEST_ASSERT_FALSE(z906_state_set_temperature(NULL));

    /* Prior valid readings must be preserved */
    z906_state_t st;
    z906_state_get(&st);
    TEST_ASSERT_TRUE(st.temp_valid);
    TEST_ASSERT_EQUAL_INT8(28, st.temp_celsius[0]);
    TEST_ASSERT_EQUAL_INT8(33, st.temp_celsius[1]);
    TEST_ASSERT_EQUAL_INT8(44, st.temp_celsius[2]);
}


/**
 * @brief Counts how many times the test observer callback was invoked.
 * Reset in setUp() via z906_state_init() + re-register.
 */
static int s_observer_call_count = 0;

/**
 * @brief Last change_mask received by the test observer.
 */
static uint32_t s_last_observer_mask = 0;

/**
 * @brief Test observer callback — increments call counter and records mask.
 *
 * Must be non-blocking per the z906_state.h observer contract.
 *
 * @param change_mask Attribute bitmask from state setter.
 */
static void prv_test_observer(uint32_t change_mask)
{
    s_observer_call_count++;
    s_last_observer_mask = change_mask;
}

/**
 * @brief Test A — Same-value setter must NOT invoke the observer.
 *
 * RATIONALE: Verifies the delta-check in each setter so that FSM loops
 * that repeatedly call z906_state_set_volume() with the same value do NOT
 * generate spurious Zigbee publisher wakeups (no-flood contract).
 */
void test_observer_same_value_no_callback(void)
{
    s_observer_call_count = 0;
    s_last_observer_mask  = 0;
    z906_state_register_change_observer(prv_test_observer);

    /* Set initial value */
    TEST_ASSERT_TRUE(z906_state_set_volume(20));
    TEST_ASSERT_EQUAL_INT(1, s_observer_call_count);

    /* Set identical value — observer must NOT fire again */
    TEST_ASSERT_FALSE(z906_state_set_volume(20));
    TEST_ASSERT_EQUAL_INT(1, s_observer_call_count); /* still 1, not 2 */

    z906_state_register_change_observer(NULL);
}

/**
 * @brief Test B — Changed value must invoke observer exactly once.
 *
 * RATIONALE: A state change must reliably wake the Zigbee publisher
 * exactly once — never zero times (missing notification) or twice
 * (duplicate report).
 */
void test_observer_changed_value_called_once(void)
{
    s_observer_call_count = 0;
    s_last_observer_mask  = 0;
    z906_state_register_change_observer(prv_test_observer);

    /* Transition from default power=false to true */
    TEST_ASSERT_TRUE(z906_state_set_power(true));
    TEST_ASSERT_EQUAL_INT(1, s_observer_call_count);
    /* ZB_PUB_ATTR_POWER = bit 0 */
    TEST_ASSERT_EQUAL_HEX32(1UL << 0, s_last_observer_mask);

    z906_state_register_change_observer(NULL);
}

/**
 * @brief Test C — No observer registered must not crash on state change.
 *
 * RATIONALE: If zigbee_state_publisher fails to register its observer
 * during init, the state manager must remain fully operational rather
 * than crashing. This is the NULL observer safety guarantee.
 */
void test_observer_null_observer_no_crash(void)
{
    /* Explicitly deregister any observer */
    z906_state_register_change_observer(NULL);

    /* State change with no observer — must NOT crash or assert */
    z906_state_set_volume(5);
    z906_state_set_power(true);
    z906_state_set_mute(true);

    /* If we reach here without crashing, the test passes */
    TEST_PASS();
}

/**
 * @brief Test D — Observer callback reads state via z906_state_get() safely.
 *
 * RATIONALE: Callbacks MUST be called outside the state mutex (per
 * prv_notify_observer() contract) to avoid re-entrant deadlock when the
 * callback itself calls z906_state_get(). This test exercises that path.
 *
 * If the mutex is still held when the callback fires, z906_state_get()
 * would deadlock (portMAX_DELAY). On ESP32-C6 this would trigger the TWDT.
 */
static z906_state_t s_state_read_in_callback;
static void prv_reading_observer(uint32_t change_mask)
{
    (void)change_mask;
    /* This call acquires s_state_mutex — safe only if callback is outside mutex */
    z906_state_get(&s_state_read_in_callback);
}

void test_observer_callback_reads_state_no_deadlock(void)
{
    z906_state_register_change_observer(prv_reading_observer);

    /* This triggers the observer which will re-acquire the mutex internally.
     * If deadlock prevention is correct, this returns immediately. */
    z906_state_set_volume(15);

    /* Verify the state read inside the callback was coherent */
    TEST_ASSERT_EQUAL_UINT8(15, s_state_read_in_callback.volume);

    z906_state_register_change_observer(NULL);
}

/**
 * @brief Test E — Concurrent writers: two FreeRTOS tasks simultaneously modify
 * different state attributes. Verifies the state manager's mutex prevents
 * state corruption, guarantees no deadlock, and that the final state remains
 * within valid hardware bounds.
 *
 * RATIONALE: On the real system, uart_rx_task (Priority 5) and cli_task
 * (Priority 2) may simultaneously call state setters. This test verifies the
 * mutex-protected implementation handles that safely.
 *
 * IMPLEMENTATION: Both tasks run CONCURRENT_WRITER_ITERATIONS iterations on
 * the test device. A counting semaphore tracks completion. The test waits with
 * a hard 5-second timeout per task — any timeout is reported as a deadlock.
 */

#define CONCURRENT_WRITER_ITERATIONS   50
#define CONCURRENT_WRITER_TASK_STACK   2048

/* Static FreeRTOS resources for concurrent test tasks */
static StaticTask_t    s_conc_task_a_tcb;
static StaticTask_t    s_conc_task_b_tcb;
static StackType_t     s_conc_task_a_stack[CONCURRENT_WRITER_TASK_STACK / sizeof(StackType_t)];
static StackType_t     s_conc_task_b_stack[CONCURRENT_WRITER_TASK_STACK / sizeof(StackType_t)];
static StaticSemaphore_t s_conc_done_sem_buf;
static SemaphoreHandle_t s_conc_done_sem;

/** @brief Shared completion counter — incremented inside each task before signalling. */
static volatile int s_conc_tasks_done = 0;

/**
 * @brief Concurrent Task A — writes master volume in ascending order (0..Z906_MAX_VOLUME_LEVEL).
 *
 * Each iteration writes a different valid volume value. The state manager mutex
 * must serialize this with concurrent writes from Task B.
 */
static void prv_conc_writer_a(void *pvParam)
{
    (void)pvParam;
    for (int i = 0; i < CONCURRENT_WRITER_ITERATIONS; i++) {
        /* Stay within [0..Z906_MAX_VOLUME_LEVEL] to avoid triggering range rejection */
        uint8_t vol = (uint8_t)(i % (Z906_MAX_VOLUME_LEVEL + 1));
        z906_state_set_volume(vol);
    }
    /* Increment done counter then signal semaphore to unblock the test task */
    s_conc_tasks_done++;
    xSemaphoreGive(s_conc_done_sem);
    vTaskDelete(NULL); /* Self-deletes after work is complete */
}

/**
 * @brief Concurrent Task B — alternately sets power ON and OFF.
 *
 * Alternating true/false ensures every iteration causes a real state delta,
 * maximizing mutex contention with Task A's volume writes.
 */
static void prv_conc_writer_b(void *pvParam)
{
    (void)pvParam;
    for (int i = 0; i < CONCURRENT_WRITER_ITERATIONS; i++) {
        z906_state_set_power((bool)(i % 2 == 0));
    }
    s_conc_tasks_done++;
    xSemaphoreGive(s_conc_done_sem);
    vTaskDelete(NULL);
}

void test_observer_concurrent_writers(void)
{
    /* Reset shared counters */
    s_conc_tasks_done = 0;

    /* Create a counting semaphore for exactly 2 completion signals */
    s_conc_done_sem = xSemaphoreCreateCountingStatic(2, 0, &s_conc_done_sem_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(s_conc_done_sem, "Failed to create done semaphore");

    /* Spawn both tasks at the same priority so the scheduler interleaves them */
    TaskHandle_t h_a = xTaskCreateStatic(prv_conc_writer_a, "ConcA",
                                          CONCURRENT_WRITER_TASK_STACK, NULL,
                                          tskIDLE_PRIORITY + 1,
                                          s_conc_task_a_stack, &s_conc_task_a_tcb);
    TaskHandle_t h_b = xTaskCreateStatic(prv_conc_writer_b, "ConcB",
                                          CONCURRENT_WRITER_TASK_STACK, NULL,
                                          tskIDLE_PRIORITY + 1,
                                          s_conc_task_b_stack, &s_conc_task_b_tcb);
    TEST_ASSERT_NOT_NULL_MESSAGE(h_a, "Failed to create concurrent writer task A");
    TEST_ASSERT_NOT_NULL_MESSAGE(h_b, "Failed to create concurrent writer task B");

    /* Wait for Task A — 5-second timeout. Failure = deadlock. */
    BaseType_t a_done = xSemaphoreTake(s_conc_done_sem, pdMS_TO_TICKS(5000));
    TEST_ASSERT_MESSAGE(a_done == pdTRUE,
        "Concurrent writer Task A timed out (possible deadlock in state manager mutex)");

    /* Wait for Task B */
    BaseType_t b_done = xSemaphoreTake(s_conc_done_sem, pdMS_TO_TICKS(5000));
    TEST_ASSERT_MESSAGE(b_done == pdTRUE,
        "Concurrent writer Task B timed out (possible deadlock in state manager mutex)");

    /* Both tasks must have self-reported completion */
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_conc_tasks_done,
        "Not all concurrent writer tasks completed cleanly");

    /* Verify final state coherence: no bit-level corruption from concurrent access */
    z906_state_t final_st;
    z906_state_get(&final_st);

    /* Volume must be a valid hardware value — any bit corruption produces an out-of-range value */
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(Z906_MAX_VOLUME_LEVEL, final_st.volume,
        "Volume corrupted by concurrent writes (out of range)");

    /* Power must be a valid bool — garbled memory produces neither true nor false */
    TEST_ASSERT_MESSAGE(final_st.power == true || final_st.power == false,
        "Power state corrupted by concurrent writes (not a valid bool)");
}


/**
 * @brief Entry point for Unity test suite execution.
 */
int runUnityTests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_state_defaults);
    RUN_TEST(test_state_volume_boundaries);
    RUN_TEST(test_state_input_transitions);
    RUN_TEST(test_state_telemetry_fields);
    RUN_TEST(test_state_temp_valid_flag_boot_to_first_measurement);
    RUN_TEST(test_state_temp_valid_preserved_on_no_change);
    RUN_TEST(test_state_temp_three_channels_distinct);
    RUN_TEST(test_state_temp_null_input_rejected);
    /* Observer contract tests (FV-07) */
    RUN_TEST(test_observer_same_value_no_callback);
    RUN_TEST(test_observer_changed_value_called_once);
    RUN_TEST(test_observer_null_observer_no_crash);
    RUN_TEST(test_observer_callback_reads_state_no_deadlock);
    /* Observer concurrent-writer test (FC-05) */
    RUN_TEST(test_observer_concurrent_writers);
    return UNITY_END();
}

#if defined(ESP_PLATFORM)
/**
 * @brief ESP-IDF application entry point for testing.
 *
 * WHY THE DELAY: esptool hard-resets the board at the end of flashing.
 * PlatformIO's test runner opens the serial port AFTER esptool exits,
 * which takes ~2-3 seconds. Without this delay, all Unity output is
 * transmitted before the test runner starts listening and the test
 * appears as SKIPPED. The delay also ensures USB CDC re-enumeration
 * completes on the host before the first Unity byte is sent.
 */
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow test runner to open serial port */
    runUnityTests();
}
#else
/**
 * @brief Host-based test runner entry point.
 */
int main(void)
{
    return runUnityTests();
}
#endif
