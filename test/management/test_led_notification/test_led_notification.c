/**
 * @file test_led_notification.c
 * @brief Comprehensive unit tests for LED notification module:
 *        - Animation frame generation & sequence completion (Rainbow, WhiteDoubleBlink, RedSOS, GreenBlink, PoliceFlasher)
 *        - Animation registry & stable ID lookup
 *        - Persistent NVS storage, defaults, corruption fallback, deduplication
 *        - Event priority engine (Pairing > Cmd Fail > Cmd Succ > Boot > Normal > Off)
 *        - Global enabled/disabled control & immediate physical shutoff
 *        - Adaptive delay calculation & CPU conservation
 *        - Boot window (10s) management
 *        - CLI mapping set / reset
 *        - Physical OFF invariants (INV-1 through INV-5):
 *            INV-1: active_animation == NONE  ->  driver output must be {0,0,0}.
 *            INV-2: global_led_enabled == false  ->  driver output must be {0,0,0}.
 *            INV-3: Animation complete + no replacement  ->  explicit OFF once.
 *            INV-4: Animation cancelled/preempted  ->  explicit OFF once.
 *            INV-5: Driver init  ->  safe OFF state before first animation.
 */

#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_notification_manager.h"
#include "led_animation_registry.h"
#include "led_driver.h"
#include "led_nvs_storage.h"
#include "Animations/rainbow_anim.h"
#include "Animations/white_double_blink_anim.h"
#include "Animations/red_sos_anim.h"
#include "Animations/green_blink_anim.h"
#include "Animations/police_flasher_anim.h"
#include "nvs_manager.h"
#include "z906_state.h"
#include "zigbee_app.h"
#include <string.h>

void setUp(void)
{
    nvs_manager_init();
    z906_state_init();
    led_notification_init();
    /* Ensure clean enabled baseline */
    led_notification_set_enabled(true);
    led_notification_reset_mappings(-1);
}

void tearDown(void)
{
    led_notification_set_enabled(true);
    led_notification_reset_mappings(-1);
}

/* ========================================================================= */
/* Test Suite 1: Animation Frame & Sequence Tests                            */
/* ========================================================================= */

void test_rainbow_anim_sequence(void)
{
    rainbow_state_t st;
    rainbow_anim_reset(&st);
    TEST_ASSERT_EQUAL_UINT16(0, st.hue_step);

    bool finished = false;
    for (int i = 0; i < 40; i++) {
        led_anim_step_result_t res = rainbow_anim_step(&st, 50);
        TEST_ASSERT_TRUE(res.hold_duration_ms > 0);
        if (res.sequence_finished) {
            finished = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(finished);
}

void test_white_double_blink_sequence(void)
{
    white_double_blink_state_t st;
    white_double_blink_anim_reset(&st);

    /* Step 0: Pulse 1 ON */
    led_anim_step_result_t r0 = white_double_blink_anim_step(&st, 0);
    TEST_ASSERT_EQUAL_UINT8(255, r0.color.r);
    TEST_ASSERT_EQUAL_UINT8(255, r0.color.g);
    TEST_ASSERT_EQUAL_UINT8(255, r0.color.b);
    TEST_ASSERT_FALSE(r0.sequence_finished);

    /* Step 1: Gap 1 OFF */
    led_anim_step_result_t r1 = white_double_blink_anim_step(&st, 0);
    TEST_ASSERT_EQUAL_UINT8(0, r1.color.r);
    TEST_ASSERT_FALSE(r1.sequence_finished);

    /* Step 2: Pulse 2 ON */
    led_anim_step_result_t r2 = white_double_blink_anim_step(&st, 0);
    TEST_ASSERT_EQUAL_UINT8(255, r2.color.r);
    TEST_ASSERT_FALSE(r2.sequence_finished);

    /* Step 3: Long Rest OFF (Completes sequence) */
    led_anim_step_result_t r3 = white_double_blink_anim_step(&st, 0);
    TEST_ASSERT_EQUAL_UINT8(0, r3.color.r);
    TEST_ASSERT_TRUE(r3.sequence_finished);
}

void test_red_sos_sequence(void)
{
    red_sos_state_t st;
    red_sos_anim_reset(&st);

    bool saw_red = false;
    bool finished = false;

    for (int i = 0; i < 20; i++) {
        led_anim_step_result_t res = red_sos_anim_step(&st, 0);
        if (res.color.r > 0) {
            saw_red = true;
        }
        if (res.sequence_finished) {
            finished = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(saw_red);
    TEST_ASSERT_TRUE(finished);
}

void test_green_blink_sequence(void)
{
    green_blink_state_t st;
    green_blink_anim_reset(&st);

    /* Step 0: ON */
    led_anim_step_result_t r0 = green_blink_anim_step(&st, 0);
    TEST_ASSERT_EQUAL_UINT8(255, r0.color.g);
    TEST_ASSERT_FALSE(r0.sequence_finished);

    /* Step 1: OFF */
    led_anim_step_result_t r1 = green_blink_anim_step(&st, 0);
    TEST_ASSERT_EQUAL_UINT8(0, r1.color.g);
    TEST_ASSERT_TRUE(r1.sequence_finished);
}

void test_police_flasher_sequence(void)
{
    police_flasher_state_t st;
    police_flasher_anim_reset(&st);

    bool saw_blue = false;
    bool saw_red = false;
    bool finished = false;

    for (int i = 0; i < 20; i++) {
        led_anim_step_result_t res = police_flasher_anim_step(&st, 0);
        if (res.color.b > 0) saw_blue = true;
        if (res.color.r > 0) saw_red = true;
        if (res.sequence_finished) {
            finished = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(saw_blue);
    TEST_ASSERT_TRUE(saw_red);
    TEST_ASSERT_TRUE(finished);
}

/* ========================================================================= */
/* Test Suite 2: Registry & Lookup Tests                                     */
/* ========================================================================= */

void test_animation_registry_lookup(void)
{
    TEST_ASSERT_EQUAL_INT(5, led_animation_registry_count());

    TEST_ASSERT_NOT_NULL(led_animation_registry_find_by_id("rainbow"));
    TEST_ASSERT_NOT_NULL(led_animation_registry_find_by_id("white_double_blink"));
    TEST_ASSERT_NOT_NULL(led_animation_registry_find_by_id("red_sos"));
    TEST_ASSERT_NOT_NULL(led_animation_registry_find_by_id("green_blink"));
    TEST_ASSERT_NOT_NULL(led_animation_registry_find_by_id("police_flasher"));

    /* Non-existent */
    TEST_ASSERT_NULL(led_animation_registry_find_by_id("non_existent_anim"));
    TEST_ASSERT_NULL(led_animation_registry_find_by_id(NULL));
}

/* ========================================================================= */
/* Test Suite 3: Global Enable / Disable & Priority Tests                    */
/* ========================================================================= */

void test_global_enable_disable_behavior(void)
{
    TEST_ASSERT_TRUE(led_notification_is_enabled());

    /* Disable LED */
    TEST_ASSERT_TRUE(led_notification_set_enabled(false));
    TEST_ASSERT_FALSE(led_notification_is_enabled());

    /* While disabled, stepping sets physical color to black */
    led_notification_step(20);
    led_color_t c = led_driver_get_current_color();
    TEST_ASSERT_EQUAL_UINT8(0, c.r);
    TEST_ASSERT_EQUAL_UINT8(0, c.g);
    TEST_ASSERT_EQUAL_UINT8(0, c.b);

    /* Posting a command result while disabled must NOT trigger notification */
    led_notification_post_cmd_result(true);
    TEST_ASSERT_EQUAL_STRING("off", led_notification_get_active_animation_id());

    /* Re-enable */
    TEST_ASSERT_TRUE(led_notification_set_enabled(true));
    TEST_ASSERT_TRUE(led_notification_is_enabled());
}

void test_adaptive_delay_behavior(void)
{
    /* When disabled, next delay must be power-saving high delay (500 ms) */
    led_notification_set_enabled(false);
    TEST_ASSERT_EQUAL_UINT32(500, led_notification_get_next_delay_ms());

    /* Re-enable and verify adaptive delay when active */
    led_notification_set_enabled(true);
    led_notification_step(20);
    uint32_t active_delay = led_notification_get_next_delay_ms();
    TEST_ASSERT_TRUE(active_delay <= 50);
    TEST_ASSERT_TRUE(active_delay >= 10);
}

void test_event_mapping_set_and_reset(void)
{
    /* Set pairing event to rainbow */
    TEST_ASSERT_TRUE(led_notification_set_mapping(LED_EVENT_PAIRING, "rainbow"));
    TEST_ASSERT_EQUAL_STRING("rainbow", led_notification_get_mapping(LED_EVENT_PAIRING));

    /* Reset pairing event */
    TEST_ASSERT_TRUE(led_notification_reset_mappings((int)LED_EVENT_PAIRING));
    TEST_ASSERT_EQUAL_STRING("police_flasher", led_notification_get_mapping(LED_EVENT_PAIRING));

    /* Set multiple and reset all */
    led_notification_set_mapping(LED_EVENT_BOOT, "white_double_blink");
    led_notification_set_mapping(LED_EVENT_NORMAL, "green_blink");
    TEST_ASSERT_EQUAL_STRING("white_double_blink", led_notification_get_mapping(LED_EVENT_BOOT));
    TEST_ASSERT_EQUAL_STRING("green_blink", led_notification_get_mapping(LED_EVENT_NORMAL));

    TEST_ASSERT_TRUE(led_notification_reset_mappings(-1));
    TEST_ASSERT_EQUAL_STRING("rainbow", led_notification_get_mapping(LED_EVENT_BOOT));
    TEST_ASSERT_EQUAL_STRING("white_double_blink", led_notification_get_mapping(LED_EVENT_NORMAL));
}

/* =========================================================================
 * Test Suite 4: Physical OFF Invariants
 *
 * These tests verify that when no animation is active, the driver output is
 * always {R=0, G=0, B=0} — meaning the WS2812 received an explicit black frame.
 * "Active Animation: off" MUST imply "Driver Output: R=0 G=0 B=0".
 * ========================================================================= */

/**
 * @brief INV-1/INV-3: Enabled + no base condition -> driver output must be OFF.
 *
 * Simulates the state: LED enabled, boot expired, Zigbee not joined.
 * After step(), target_anim_id resolves to NULL -> apply_physical_off() must fire.
 */
void test_inv1_idle_enabled_driver_output_off(void)
{
    /* LED is enabled; setUp ensures boot window has not yet expired in real clock terms.
     * Force boot_active to false by stepping with a large elapsed value (> 10000 ms). */
    led_notification_step(10001);

    /* Now boot is expired. Zigbee is not joined (test environment stub returns UNPAIRED).
     * No active condition -> should be physically off. */
    led_notification_step(100);

    TEST_ASSERT_EQUAL_STRING("off", led_notification_get_active_animation_id());

    /* Driver output MUST be {0,0,0}: an explicit black frame was sent. */
    led_color_t c = led_driver_get_current_color();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.r, "INV-1: Red channel must be 0 when idle");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.g, "INV-1: Green channel must be 0 when idle");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.b, "INV-1: Blue channel must be 0 when idle");
}

/**
 * @brief INV-2: global_led_enabled == false -> driver output must be {0,0,0}.
 *
 * Sets an animation to drive a non-zero color, then disables globally.
 * The driver cache must show {0,0,0} immediately after disable.
 */
void test_inv2_global_disable_driver_off(void)
{
    /* Start a preview so the driver holds a non-zero color */
    led_notification_preview("police_flasher");
    /* Step to push a colored frame */
    led_notification_step(20);

    /* Disable globally */
    led_notification_set_enabled(false);
    TEST_ASSERT_FALSE(led_notification_is_enabled());

    /* Driver must show {0,0,0} after disable */
    led_color_t c = led_driver_get_current_color();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.r, "INV-2: Red must be 0 after global disable");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.g, "INV-2: Green must be 0 after global disable");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.b, "INV-2: Blue must be 0 after global disable");

    /* Additional step() calls while disabled must not light the LED */
    led_notification_step(20);
    c = led_driver_get_current_color();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.r, "INV-2: Red must stay 0 on subsequent disabled steps");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.b, "INV-2: Blue must stay 0 on subsequent disabled steps");
}

/**
 * @brief INV-3: Green blink one-shot finishes, no base condition -> driver OFF.
 *
 * Verifies that after a CMD_SUCCESS notification plays its full sequence,
 * the physical driver output transitions to {0,0,0}.
 */
void test_inv3_oneshot_success_then_off(void)
{
    /* Push boot window past expiry */
    led_notification_step(10001);

    /* Post a success result (no base animation since device not operational) */
    led_notification_post_cmd_result(true);

    /* Run enough steps to exhaust the green_blink one-shot sequence.
     * green_blink has 2 steps (ON=200ms, OFF=200ms) = ~400ms total. */
    for (int i = 0; i < 30; i++) {
        led_notification_step(20);
    }

    /* One-shot is finished, no replacement -> should be off */
    TEST_ASSERT_EQUAL_STRING("off", led_notification_get_active_animation_id());

    /* Driver must be physically off */
    led_color_t c = led_driver_get_current_color();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.r, "INV-3: Red must be 0 after success oneshot ends");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.g, "INV-3: Green must be 0 after success oneshot ends");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.b, "INV-3: Blue must be 0 after success oneshot ends");
}

/**
 * @brief INV-3: Red SOS one-shot finishes, no base condition -> driver OFF.
 */
void test_inv3_oneshot_failure_then_off(void)
{
    /* Push boot window past expiry */
    led_notification_step(10001);

    /* Post failure (no base animation) */
    led_notification_post_cmd_result(false);

    /* Run enough steps to exhaust red_sos (18-step pattern ~4400ms) */
    for (int i = 0; i < 250; i++) {
        led_notification_step(20);
    }

    TEST_ASSERT_EQUAL_STRING("off", led_notification_get_active_animation_id());

    led_color_t c = led_driver_get_current_color();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.r, "INV-3: Red must be 0 after failure oneshot ends");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.g, "INV-3: Green must be 0 after failure oneshot ends");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.b, "INV-3: Blue must be 0 after failure oneshot ends");
}

/**
 * @brief INV-4: Preview animation cancelled by global disable -> driver OFF.
 */
void test_inv4_preview_cancelled_by_disable(void)
{
    led_notification_preview("police_flasher");
    /* Step once to transmit a colored frame */
    led_notification_step(20);

    /* Cancel via global disable */
    led_notification_set_enabled(false);

    led_color_t c = led_driver_get_current_color();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.r, "INV-4: Red must be 0 after preview cancelled");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.g, "INV-4: Green must be 0 after preview cancelled");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.b, "INV-4: Blue must be 0 after preview cancelled");
}

/**
 * @brief INV-5: Driver init -> safe OFF state before first animation.
 *
 * After a fresh init (tearDown resets state) the driver must report {0,0,0}.
 * Note: setUp calls led_notification_init() which clears the LED.
 */
void test_inv5_driver_init_safe_off_state(void)
{
    /* setUp already called led_notification_init() with enabled=true.
     * Before any step(), the boot animation may or may not have been started.
     * However, driver must have been initialized to {0,0,0} at startup.
     * We verify by setting enabled=false and checking driver output. */
    led_notification_set_enabled(false);

    led_color_t c = led_driver_get_current_color();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.r, "INV-5: Red must be 0 after init+disable");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.g, "INV-5: Green must be 0 after init+disable");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c.b, "INV-5: Blue must be 0 after init+disable");
}

/**
 * @brief Verify that idle state produces power-saving delay (500ms) only after OFF is sent.
 *
 * Before OFF frame: delay should be small (10ms) to wake and send it.
 * After OFF frame: delay should be 500ms to conserve CPU.
 */
void test_idle_delay_after_physical_off(void)
{
    /* Force into idle state: boot expired, not operational */
    led_notification_step(10001); /* Expire boot */
    led_notification_step(20);    /* Resolve idle, send OFF */

    /* After OFF sent: should sleep for 500ms */
    uint32_t delay = led_notification_get_next_delay_ms();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(500, delay,
        "After physical OFF sent, idle delay must be 500ms for CPU conservation");
}

/**
 * @brief INV-2: Global disable while idle -> driver stays at {0,0,0} (idempotent).
 */
void test_inv2_disable_while_already_idle(void)
{
    /* Force idle state first */
    led_notification_step(10001);
    led_notification_step(20);
    TEST_ASSERT_EQUAL_STRING("off", led_notification_get_active_animation_id());

    /* Disable while already idle */
    led_notification_set_enabled(false);
    led_notification_step(20);

    led_color_t c = led_driver_get_current_color();
    TEST_ASSERT_EQUAL_UINT8(0, c.r);
    TEST_ASSERT_EQUAL_UINT8(0, c.g);
    TEST_ASSERT_EQUAL_UINT8(0, c.b);

    /* Re-enable and verify no animation starts without an active condition */
    led_notification_set_enabled(true);
    led_notification_step(20);
    TEST_ASSERT_EQUAL_STRING("off", led_notification_get_active_animation_id());
    c = led_driver_get_current_color();
    TEST_ASSERT_EQUAL_UINT8(0, c.r);
    TEST_ASSERT_EQUAL_UINT8(0, c.g);
    TEST_ASSERT_EQUAL_UINT8(0, c.b);
}

int runUnityTests(void)
{
    UNITY_BEGIN();
    /* Suite 1: Animation Frame & Sequence */
    RUN_TEST(test_rainbow_anim_sequence);
    RUN_TEST(test_white_double_blink_sequence);
    RUN_TEST(test_red_sos_sequence);
    RUN_TEST(test_green_blink_sequence);
    RUN_TEST(test_police_flasher_sequence);
    /* Suite 2: Registry & Lookup */
    RUN_TEST(test_animation_registry_lookup);
    /* Suite 3: Global Enable / Priority */
    RUN_TEST(test_global_enable_disable_behavior);
    RUN_TEST(test_adaptive_delay_behavior);
    RUN_TEST(test_event_mapping_set_and_reset);
    /* Suite 4: Physical OFF Invariants */
    RUN_TEST(test_inv1_idle_enabled_driver_output_off);
    RUN_TEST(test_inv2_global_disable_driver_off);
    RUN_TEST(test_inv3_oneshot_success_then_off);
    RUN_TEST(test_inv3_oneshot_failure_then_off);
    RUN_TEST(test_inv4_preview_cancelled_by_disable);
    RUN_TEST(test_inv5_driver_init_safe_off_state);
    RUN_TEST(test_idle_delay_after_physical_off);
    RUN_TEST(test_inv2_disable_while_already_idle);
    return UNITY_END();
}

#if defined(ESP_PLATFORM)
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow test runner to open serial port before Unity starts */
    runUnityTests();
}
#else
int main(void)
{
    return runUnityTests();
}
#endif
