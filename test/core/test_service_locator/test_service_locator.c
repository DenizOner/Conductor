/**
 * @file test_service_locator.c
 * @brief Unit tests for ServiceLocator dependency registry.
 */

#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "service_locator.h"

void setUp(void)
{
    service_locator_init();
}

void tearDown(void) {}

/**
 * @brief Tests registering and querying valid service pointers.
 */
void test_service_locator_register_and_get(void)
{
    int dummy_service = 42;

    /* Prior to registration, query must return NULL */
    TEST_ASSERT_NULL(service_locator_get(SERVICE_ID_UART_HAL));

    /* Register service */
    bool ok = service_locator_register(SERVICE_ID_UART_HAL, &dummy_service);
    TEST_ASSERT_TRUE(ok);

    /* Retrieve service */
    void *retrieved = service_locator_get(SERVICE_ID_UART_HAL);
    TEST_ASSERT_NOT_NULL(retrieved);
    TEST_ASSERT_EQUAL_PTR(&dummy_service, retrieved);
    TEST_ASSERT_EQUAL_INT(42, *(int *)retrieved);
}

/**
 * @brief Tests out of bounds service IDs and NULL registrations.
 */
void test_service_locator_boundary_checks(void)
{
    TEST_ASSERT_FALSE(service_locator_register(SERVICE_ID_MAX, (void *)0x1234));
    TEST_ASSERT_FALSE(service_locator_register(SERVICE_ID_FSM, NULL));
    TEST_ASSERT_NULL(service_locator_get(SERVICE_ID_MAX));
}

int runUnityTests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_service_locator_register_and_get);
    RUN_TEST(test_service_locator_boundary_checks);
    return UNITY_END();
}

#if defined(ESP_PLATFORM)
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow test runner to open serial port */
    runUnityTests();
}
#else
int main(void)
{
    return runUnityTests();
}
#endif
