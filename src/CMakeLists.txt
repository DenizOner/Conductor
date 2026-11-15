# CMakeLists.txt for src/ component in multi-tier nested architecture

file(GLOB_RECURSE COMPONENT_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/*.c"
)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../include"
        "${CMAKE_CURRENT_LIST_DIR}/core/event_bus"
        "${CMAKE_CURRENT_LIST_DIR}/core/service_locator"
        "${CMAKE_CURRENT_LIST_DIR}/core/state_machine"
        "${CMAKE_CURRENT_LIST_DIR}/core/state_manager"
        "${CMAKE_CURRENT_LIST_DIR}/core/cmd_verifier"
        "${CMAKE_CURRENT_LIST_DIR}/hardware/uart"
        "${CMAKE_CURRENT_LIST_DIR}/hardware/adc"
        "${CMAKE_CURRENT_LIST_DIR}/hardware/gpio"
        "${CMAKE_CURRENT_LIST_DIR}/hardware/led"
        "${CMAKE_CURRENT_LIST_DIR}/protocol/codec"
        "${CMAKE_CURRENT_LIST_DIR}/protocol/commands"
        "${CMAKE_CURRENT_LIST_DIR}/communication/zigbee"
        "${CMAKE_CURRENT_LIST_DIR}/communication/zigbee/clusters"
        "${CMAKE_CURRENT_LIST_DIR}/communication/zigbee/handlers"
        "${CMAKE_CURRENT_LIST_DIR}/communication/cli"
        "${CMAKE_CURRENT_LIST_DIR}/communication/z906_transport"
        "${CMAKE_CURRENT_LIST_DIR}/management/nvs"
        "${CMAKE_CURRENT_LIST_DIR}/management/watchdog"
        "${CMAKE_CURRENT_LIST_DIR}/management/telemetry"
        "${CMAKE_CURRENT_LIST_DIR}/management/debug_console"
        "${CMAKE_CURRENT_LIST_DIR}/management/led_notification"
        "${CMAKE_CURRENT_LIST_DIR}/management/led_notification/Animations"
        "${CMAKE_CURRENT_LIST_DIR}/management/ota"
        "${CMAKE_CURRENT_LIST_DIR}/management/system_metrics"
        "${CMAKE_CURRENT_LIST_DIR}/management/health_manager"
        "${CMAKE_CURRENT_LIST_DIR}/system/initializer"
    REQUIRES
        driver
        esp_driver_tsens
        esp_adc
        nvs_flash
        esp_timer
        console
        freertos
        app_update
        esp_partition
        esp_app_format
        bootloader_support
)
