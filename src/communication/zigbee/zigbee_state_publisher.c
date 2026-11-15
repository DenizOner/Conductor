/**
 * @file zigbee_state_publisher.c
 * @brief Implementation of Native ESP Zigbee SDK v2 Verified State Publishing & Changed-Only ZCL Reporting.
 *
 * Directives & Concurrency Rules (Phase 5 & Selective Health Diagnostics):
 *   1. Authoritative Snapshot:
 *      - Reads confirmed physical telemetry via z906_state_get(), canonical application settings
 *        (led_notification_is_enabled(), z906_cmd_verifier_is_enabled()), and authoritative diagnostic
 *        snapshots (health_manager_get_snapshot(), system_metrics_get_snapshot()).
 *      - Zero optimistic or unverified states are ever published.
 *   2. Validity Gate:
 *      - Hardware amplifier attributes require (cable_connected && main_state_valid).
 *      - Canonical settings (built-in LED, cmd verifier) and health fields (device_health,
 *        health_primary_component, health_primary_reason) are always valid.
 *      - External temperatures use 0x8000 when !temp_valid.
 *      - Internal SoC die temperature is valid only when soc_temperature_valid is true (0x8000 sentinel suppressed).
 *   3. Changed-Only Discrete State & Continuous Telemetry Coalescing:
 *      - Discrete attributes (power, volume, trims, mute, input, effect, digital status, auto standby,
 *        LED, verifier, device_health, primary component, primary reason) trigger immediate report upon value delta.
 *      - High-rate continuous telemetry (input_signal_level, idle_time, temperatures) has minimum 10s interval.
 *      - SoC die temperature continuous telemetry has minimum 60s interval, 15m heartbeat, and 1.0°C delta threshold,
 *        with immediate bypass on critical health transitions.
 *   4. Decoupled Backing Sync & Report Dirty Retries:
 *      - ezb_zcl_attr_desc_set_value() updates descriptor backing values under SDK lock.
 *      - ezb_zcl_report_attr_cmd_req() submits one-shot reports using binding destination (EZB_ADDR_MODE_NONE).
 *      - If network_ready is false, reports are suppressed but backing values sync and dirty flags remain set.
 *      - If report submission fails or lock acquisition times out (500 ms), the dirty flag remains set
 *        for retry in the next cycle.
 *   5. Static Memory Budgeting & Task WDT:
 *      - Task runs as static zb_state_pub with 4096-byte stack, static TCB, and zero dynamic heap allocation.
 *      - Registers with Task Watchdog Timer (TWDT) and resets watchdog each cycle.
 */

#include "zigbee_state_publisher.h"
#include <string.h>
#include <math.h>
#include <limits.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "zigbee_device_model.h"
#include "z906_state.h"
#include "z906_commands.h"
#include "led_notification_manager.h"
#include "z906_cmd_verifier.h"
#include "health_manager.h"
#include "system_metrics.h"
#include "wdt_manager.h"
#include "clusters/zcl_clusters.h"

#include "esp_zigbee.h"
#include <ezbee/af.h>
#include <ezbee/zcl/zcl_type.h>
#include <ezbee/zcl/zcl_desc.h>
#include <ezbee/zcl/zcl_common.h>
#include <ezbee/zcl/zcl_general_cmd.h>

static const char *TAG = "ZB_STATE_PUB";

/* ========================================================================= */
/* Static Memory Allocation & Task Configuration                             */
/* ========================================================================= */

#define ZB_PUB_TASK_STACK_SIZE      4096
#define ZB_PUB_TASK_PRIORITY        3
#define ZB_PUB_LOCK_TIMEOUT_MS      500
#define ZB_PUB_RECONCILE_INTERVAL_MS 5000 /* Low-frequency periodic check for pending dirty work */

/* Rate limiting for high-rate continuous telemetry (10 seconds minimum between reports) */
#define ZB_PUB_TELEMETRY_MIN_INTERVAL_US (10000000LL)

/* Rate limiting for SoC Die Temperature continuous telemetry (Section 14: 60s min, 15 min heartbeat, 1.0°C delta) */
#define ZB_PUB_SOC_TEMP_MIN_INTERVAL_US  (60000000LL)  /* 60 seconds minimum */
#define ZB_PUB_SOC_TEMP_MAX_INTERVAL_US  (900000000LL) /* 15 minutes heartbeat */
#define ZB_PUB_SOC_TEMP_DELTA_MIN        (100)         /* 1.0 °C in 0.01 °C units */

static StackType_t  s_pub_task_stack[ZB_PUB_TASK_STACK_SIZE / sizeof(StackType_t)];
static StaticTask_t s_pub_task_tcb;
static TaskHandle_t s_pub_task_handle = NULL;

/* Network ready gate (false at boot; enabled via Phase 6 lifecycle API) */
static bool s_network_ready = false;

/* Diagnostics */
static zb_pub_diagnostics_t s_diag = { 0 };

/* ========================================================================= */
/* Canonical Attribute Mapping Table & Descriptor Handle Cache               */
/* ========================================================================= */

typedef enum {
    PUB_POLICY_IMMEDIATE_DISCRETE = 0,
    PUB_POLICY_RATE_LIMITED_TELEMETRY
} pub_report_policy_t;

typedef struct {
    uint8_t             ep_id;
    uint16_t            cluster_id;
    uint16_t            attr_id;
    uint8_t             attr_type;
    pub_report_policy_t policy;
    const char         *name;
} attr_mapping_t;

static const attr_mapping_t s_attr_mappings[ZB_PUB_ATTR_COUNT] = {
    [ZB_PUB_ATTR_POWER] = {
        .ep_id = Z906_ZB_EP_MAIN, .cluster_id = ZCL_CLUSTER_ON_OFF, .attr_id = 0x0000,
        .attr_type = EZB_ZCL_ATTR_TYPE_BOOL, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "power"
    },
    [ZB_PUB_ATTR_VOLUME] = {
        .ep_id = Z906_ZB_EP_MAIN, .cluster_id = ZCL_CLUSTER_LEVEL_CONTROL, .attr_id = 0x0000,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "volume"
    },
    [ZB_PUB_ATTR_TRIM_BASS] = {
        .ep_id = Z906_ZB_EP_TRIMS, .cluster_id = ZCL_CLUSTER_Z906_TRIMS, .attr_id = ZCL_ATTR_TRIM_BASS,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "trim_bass"
    },
    [ZB_PUB_ATTR_TRIM_CENTER] = {
        .ep_id = Z906_ZB_EP_TRIMS, .cluster_id = ZCL_CLUSTER_Z906_TRIMS, .attr_id = ZCL_ATTR_TRIM_CENTER,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "trim_center"
    },
    [ZB_PUB_ATTR_TRIM_REAR] = {
        .ep_id = Z906_ZB_EP_TRIMS, .cluster_id = ZCL_CLUSTER_Z906_TRIMS, .attr_id = ZCL_ATTR_TRIM_REAR,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "trim_rear"
    },
    [ZB_PUB_ATTR_MUTE] = {
        .ep_id = Z906_ZB_EP_TRIMS, .cluster_id = ZCL_CLUSTER_Z906_TRIMS, .attr_id = ZCL_ATTR_AUDIO_MUTE,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "mute"
    },
    [ZB_PUB_ATTR_INPUT] = {
        .ep_id = Z906_ZB_EP_SOURCES, .cluster_id = ZCL_CLUSTER_Z906_SOURCES, .attr_id = ZCL_ATTR_SOURCE_INPUT,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "input"
    },
    [ZB_PUB_ATTR_EFFECT] = {
        .ep_id = Z906_ZB_EP_SOURCES, .cluster_id = ZCL_CLUSTER_Z906_SOURCES, .attr_id = ZCL_ATTR_SOURCE_EFFECT,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "effect"
    },
    [ZB_PUB_ATTR_INPUT_SIGNAL_LEVEL] = {
        .ep_id = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY, .attr_id = ZCL_ATTR_INPUT_SIGNAL_LEVEL,
        .attr_type = EZB_ZCL_ATTR_TYPE_DATA24, .policy = PUB_POLICY_RATE_LIMITED_TELEMETRY, .name = "input_signal_level"
    },
    [ZB_PUB_ATTR_IDLE_TIME] = {
        .ep_id = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY, .attr_id = ZCL_ATTR_IDLE_TIME,
        .attr_type = EZB_ZCL_ATTR_TYPE_DATA32, .policy = PUB_POLICY_RATE_LIMITED_TELEMETRY, .name = "idle_time"
    },
    [ZB_PUB_ATTR_DIGITAL_SIGNAL_STATUS] = {
        .ep_id = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY, .attr_id = ZCL_ATTR_DIGITAL_SIGNAL_STATUS,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "digital_signal_status"
    },
    [ZB_PUB_ATTR_AUTO_STANDBY] = {
        .ep_id = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY, .attr_id = ZCL_ATTR_AUTO_STANDBY,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "auto_standby"
    },
    [ZB_PUB_ATTR_BUILT_IN_LED] = {
        .ep_id = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY, .attr_id = ZCL_ATTR_BUILT_IN_LED_ENABLED,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "built_in_led_enabled"
    },
    [ZB_PUB_ATTR_CMD_VERIFIER] = {
        .ep_id = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY, .attr_id = ZCL_ATTR_CMD_VERIFIER_ENABLED,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "cmd_verifier_enabled"
    },
    [ZB_PUB_ATTR_TEMP1] = {
        .ep_id = Z906_ZB_EP_TEMP1, .cluster_id = ZCL_CLUSTER_TEMP_MEASUREMENT, .attr_id = ZCL_ATTR_TEMP_MEASURED_VALUE,
        .attr_type = EZB_ZCL_ATTR_TYPE_INT16, .policy = PUB_POLICY_RATE_LIMITED_TELEMETRY, .name = "temp1"
    },
    [ZB_PUB_ATTR_TEMP2] = {
        .ep_id = Z906_ZB_EP_TEMP2, .cluster_id = ZCL_CLUSTER_TEMP_MEASUREMENT, .attr_id = ZCL_ATTR_TEMP_MEASURED_VALUE,
        .attr_type = EZB_ZCL_ATTR_TYPE_INT16, .policy = PUB_POLICY_RATE_LIMITED_TELEMETRY, .name = "temp2"
    },
    [ZB_PUB_ATTR_TEMP3] = {
        .ep_id = Z906_ZB_EP_TEMP3, .cluster_id = ZCL_CLUSTER_TEMP_MEASUREMENT, .attr_id = ZCL_ATTR_TEMP_MEASURED_VALUE,
        .attr_type = EZB_ZCL_ATTR_TYPE_INT16, .policy = PUB_POLICY_RATE_LIMITED_TELEMETRY, .name = "temp3"
    },
    [ZB_PUB_ATTR_DEVICE_HEALTH] = {
        .ep_id = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY, .attr_id = ZCL_ATTR_DEVICE_HEALTH,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "device_health"
    },
    [ZB_PUB_ATTR_HEALTH_PRIMARY_COMP] = {
        .ep_id = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY, .attr_id = ZCL_ATTR_HEALTH_PRIMARY_COMPONENT,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "health_primary_component"
    },
    [ZB_PUB_ATTR_HEALTH_PRIMARY_REAS] = {
        .ep_id = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY, .attr_id = ZCL_ATTR_HEALTH_PRIMARY_REASON,
        .attr_type = EZB_ZCL_ATTR_TYPE_UINT8, .policy = PUB_POLICY_IMMEDIATE_DISCRETE, .name = "health_primary_reason"
    },
    [ZB_PUB_ATTR_SOC_TEMP] = {
        .ep_id = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY, .attr_id = ZCL_ATTR_SOC_TEMPERATURE,
        .attr_type = EZB_ZCL_ATTR_TYPE_INT16, .policy = PUB_POLICY_RATE_LIMITED_TELEMETRY, .name = "soc_temperature"
    },
};

/* Cached Attribute Descriptor Handles (resolved once upon registration) */
static ezb_zcl_attr_desc_t s_cached_attr_descs[ZB_PUB_ATTR_COUNT] = { NULL };

/* ========================================================================= */
/* Value Representation & State Tracking Containers                          */
/* ========================================================================= */

typedef struct {
    union {
        bool     val_bool;
        uint8_t  val_u8;
        uint32_t val_u32;
        int16_t  val_i16;
        uint8_t  val_data24[3];
    } v;
    bool is_valid;
} pub_value_t;

/*
 * State Semantics — Three Distinct Concepts:
 *
 *   s_current_values[]  = DESIRED STATE   — authoritative, re-sampled from
 *                           z906_state_get() + canonical app settings +
 *                           health_manager_get_snapshot() + system_metrics_get_snapshot().
 *
 *   s_backing_values[]  = APPLIED STATE   — value that was last SUCCESSFULLY
 *                           written to the ZCL attribute descriptor via
 *                           ezb_zcl_attr_desc_set_value(). Advances ONLY on
 *                           EZB_ERR_NONE. A failed write MUST NOT advance this.
 *
 *   s_submitted_values[] = REPORTED STATE  — value for which a ZCL network
 *                           report was last successfully dispatched. Used to
 *                           determine whether a fresh report must be sent.
 *
 *   s_dirty_flags[]     = PENDING NETWORK REPORT — set when desired != reported.
 *                           Cleared only after a successful ZCL report dispatch.
 *
 *   s_deferred_on_inv_state[] = DEFERRED DESCRIPTOR WRITE — records the exact
 *                           desired value for which EZB_ERR_INV_STATE was last
 *                           received. Suppresses blind periodic retry of the same
 *                           write. Cleared by force_full_sync() on network join.
 *                           is_valid=false means no active deferral.
 */
static pub_value_t s_current_values[ZB_PUB_ATTR_COUNT]          = { { { .val_u32 = 0 }, false } };
static pub_value_t s_backing_values[ZB_PUB_ATTR_COUNT]          = { { { .val_u32 = 0 }, false } };
static pub_value_t s_submitted_values[ZB_PUB_ATTR_COUNT]        = { { { .val_u32 = 0 }, false } };
static pub_value_t s_deferred_on_inv_state[ZB_PUB_ATTR_COUNT]   = { { { .val_u32 = 0 }, false } };
static bool        s_dirty_flags[ZB_PUB_ATTR_COUNT]             = { false };
static int64_t     s_last_report_time_us[ZB_PUB_ATTR_COUNT]     = { 0 };

/*
 * Test seam: allows test binaries to inject controlled ezb_zcl_attr_desc_set_value
 * return codes (e.g. EZB_ERR_INV_STATE, EZB_ERR_INV_ARG) without linking against
 * the full Zigbee SDK. The portable pub_set_value_fn_t type (int(*)(void*,const void*))
 * is defined in zigbee_state_publisher.h. The real SDK function is cast to this type
 * at assignment; the call site casts back to the real arg types.
 * ezb_err_t == int and ezb_zcl_attr_desc_t is a pointer type, so the casts are safe.
 */
static pub_set_value_fn_t s_set_value_fn = (pub_set_value_fn_t)ezb_zcl_attr_desc_set_value;

typedef bool (*pub_lock_acquire_fn_t)(uint32_t timeout_ms);
typedef void (*pub_lock_release_fn_t)(void);

static bool default_lock_acquire(uint32_t timeout_ms)
{
    return esp_zigbee_lock_acquire(pdMS_TO_TICKS(timeout_ms));
}

static void default_lock_release(void)
{
    esp_zigbee_lock_release();
}

static pub_lock_acquire_fn_t s_lock_acquire_fn = default_lock_acquire;
static pub_lock_release_fn_t s_lock_release_fn = default_lock_release;

/* ========================================================================= */
/* Helper Functions: Encoding & Comparison                                   */
/* ========================================================================= */

void zigbee_state_publisher_encode_data24(uint32_t signal_level, uint8_t out_bytes[3])
{
    if (out_bytes == NULL) {
        return;
    }
    /* Standard ZCL DATA24 format: little-endian byte ordering */
    out_bytes[0] = (uint8_t)(signal_level & 0xFF);
    out_bytes[1] = (uint8_t)((signal_level >> 8) & 0xFF);
    out_bytes[2] = (uint8_t)((signal_level >> 16) & 0xFF);
}

int16_t zigbee_state_publisher_encode_temperature(float temp_celsius, bool is_valid)
{
    /* 1. Validity gate: If sensor uninitialized or query failed, return ZCL invalid sentinel */
    if (!is_valid) {
        return ZCL_TEMP_INVALID_VALUE;
    }

    /* 2. Finite check: NaN, +Inf, -Inf must never produce arbitrary or corrupted integers */
    if (!isfinite(temp_celsius)) {
        return ZCL_TEMP_INVALID_VALUE;
    }

    /* 3. Scale by 100 with double precision to prevent intermediate float truncation */
    double scaled = (double)temp_celsius * 100.0;

    /* 4. Deterministic nearest rounding: rounds half away from zero */
    double rounded = (scaled >= 0.0) ? floor(scaled + 0.5) : ceil(scaled - 0.5);

    /* 5. Int16 range safety check (-32767 .. 32767) */
    if (rounded < -32767.0 || rounded > 32767.0) {
        return ZCL_TEMP_INVALID_VALUE;
    }

    int16_t encoded = (int16_t)rounded;

    /* 6. Sentinel collision prevention: 0x8000 (-32768) is reserved for invalid measurement */
    if (encoded == ZCL_TEMP_INVALID_VALUE) {
        return ZCL_TEMP_INVALID_VALUE;
    }

    return encoded;
}

static bool values_equal(zb_pub_attr_key_t key, const pub_value_t *a, const pub_value_t *b)
{
    if (a->is_valid != b->is_valid) {
        return false;
    }
    if (!a->is_valid && !b->is_valid) {
        return true;
    }

    switch (s_attr_mappings[key].attr_type) {
        case EZB_ZCL_ATTR_TYPE_BOOL:
            return (a->v.val_bool == b->v.val_bool);
        case EZB_ZCL_ATTR_TYPE_UINT8:
            return (a->v.val_u8 == b->v.val_u8);
        case EZB_ZCL_ATTR_TYPE_DATA32:
        case EZB_ZCL_ATTR_TYPE_UINT32:
            return (a->v.val_u32 == b->v.val_u32);
        case EZB_ZCL_ATTR_TYPE_INT16:
            return (a->v.val_i16 == b->v.val_i16);
        case EZB_ZCL_ATTR_TYPE_DATA24:
            return (memcmp(a->v.val_data24, b->v.val_data24, 3) == 0);
        default:
            return (a->v.val_u32 == b->v.val_u32);
    }
}

/**
 * @brief Samples authoritative system telemetry and populates s_current_values.
 *
 * Implements strict validity gates:
 *   - Only settings (LED, Verifier) and Health Manager diagnostics are valid when hardware is uninitialized.
 *   - Hardware amplifier attributes become valid only when (cable_connected && main_state_valid).
 *   - External temperatures use 0x8000 when !temp_valid.
 *   - Internal SoC die temperature is marked valid only when confirmed by System Metrics.
 */
static void capture_authoritative_snapshot(void)
{
    z906_state_t st;
    z906_state_get(&st);

    bool led_on = led_notification_is_enabled();
    bool verif_on = z906_cmd_verifier_is_enabled();

    /* Strict Hardware Validity Gate:
     * Physical hardware control attributes (power, volume, trims, mute, input, effect, digital status,
     * auto standby) are valid ONLY when the cable is physically connected AND the first complete
     * status/config response (0x34 / CONFIG) has been parsed and verified. */
    bool hw_valid = (st.cable_connected && st.main_state_valid);

    /* 1. EP1 OnOff */
    s_current_values[ZB_PUB_ATTR_POWER].is_valid = hw_valid;
    s_current_values[ZB_PUB_ATTR_POWER].v.val_bool = st.power;

    /* 2. EP1 Level Control (ZCL 0..254) */
    s_current_values[ZB_PUB_ATTR_VOLUME].is_valid = hw_valid;
    s_current_values[ZB_PUB_ATTR_VOLUME].v.val_u8 = z906_cmd_volume_to_zcl(st.volume);

    /* 3..5. EP2 Trims (ZCL 0..254) */
    s_current_values[ZB_PUB_ATTR_TRIM_BASS].is_valid = hw_valid;
    s_current_values[ZB_PUB_ATTR_TRIM_BASS].v.val_u8 = z906_cmd_volume_to_zcl(st.volume_bass);

    s_current_values[ZB_PUB_ATTR_TRIM_CENTER].is_valid = hw_valid;
    s_current_values[ZB_PUB_ATTR_TRIM_CENTER].v.val_u8 = z906_cmd_volume_to_zcl(st.volume_center);

    s_current_values[ZB_PUB_ATTR_TRIM_REAR].is_valid = hw_valid;
    s_current_values[ZB_PUB_ATTR_TRIM_REAR].v.val_u8 = z906_cmd_volume_to_zcl(st.volume_rear);

    /* 6. EP2 Mute (0/1) */
    s_current_values[ZB_PUB_ATTR_MUTE].is_valid = hw_valid;
    s_current_values[ZB_PUB_ATTR_MUTE].v.val_u8 = st.mute ? 1 : 0;

    /* 7..8. EP3 Input & Effect */
    s_current_values[ZB_PUB_ATTR_INPUT].is_valid = hw_valid;
    s_current_values[ZB_PUB_ATTR_INPUT].v.val_u8 = (uint8_t)st.input;

    s_current_values[ZB_PUB_ATTR_EFFECT].is_valid = hw_valid;
    s_current_values[ZB_PUB_ATTR_EFFECT].v.val_u8 = (uint8_t)st.effect;

    /* 9. EP4 Input Signal Level (DATA24 wire format) */
    s_current_values[ZB_PUB_ATTR_INPUT_SIGNAL_LEVEL].is_valid = hw_valid;
    zigbee_state_publisher_encode_data24(st.input_signal_level,
                                         s_current_values[ZB_PUB_ATTR_INPUT_SIGNAL_LEVEL].v.val_data24);

    /* 10. EP4 Idle Time (seconds) */
    s_current_values[ZB_PUB_ATTR_IDLE_TIME].is_valid = hw_valid;
    s_current_values[ZB_PUB_ATTR_IDLE_TIME].v.val_u32 = st.idle_seconds;

    /* 11. EP4 Digital Signal Status */
    s_current_values[ZB_PUB_ATTR_DIGITAL_SIGNAL_STATUS].is_valid = hw_valid;
    s_current_values[ZB_PUB_ATTR_DIGITAL_SIGNAL_STATUS].v.val_u8 = st.digital_signal_status;

    /* 12. EP4 Auto Standby */
    s_current_values[ZB_PUB_ATTR_AUTO_STANDBY].is_valid = hw_valid;
    s_current_values[ZB_PUB_ATTR_AUTO_STANDBY].v.val_u8 = st.auto_standby_enabled ? 1 : 0;

    /* 13. EP4 Built-In LED (canonical app setting - always valid) */
    s_current_values[ZB_PUB_ATTR_BUILT_IN_LED].is_valid = true;
    s_current_values[ZB_PUB_ATTR_BUILT_IN_LED].v.val_u8 = led_on ? 1 : 0;

    /* 14. EP4 Cmd Verifier (canonical app setting - always valid) */
    s_current_values[ZB_PUB_ATTR_CMD_VERIFIER].is_valid = true;
    s_current_values[ZB_PUB_ATTR_CMD_VERIFIER].v.val_u8 = verif_on ? 1 : 0;

    /* 15..17. EP11, 12, 13 Amplifier Temperatures */
    bool temp_overall_valid = st.cable_connected && st.temp_valid;
    for (int i = 0; i < 3; i++) {
        zb_pub_attr_key_t tkey = (zb_pub_attr_key_t)(ZB_PUB_ATTR_TEMP1 + i);
        s_current_values[tkey].is_valid = st.cable_connected;
        s_current_values[tkey].v.val_i16 = zigbee_state_publisher_encode_temperature((float)st.temp_celsius[i], temp_overall_valid);
    }

    /* 18..20. EP4 Health Manager Diagnostics (always valid authoritative snapshot) */
    health_snapshot_t h_snap;
    if (health_manager_get_snapshot(&h_snap) == ESP_OK) {
        s_current_values[ZB_PUB_ATTR_DEVICE_HEALTH].is_valid = true;
        s_current_values[ZB_PUB_ATTR_DEVICE_HEALTH].v.val_u8 = (uint8_t)h_snap.overall_status;

        s_current_values[ZB_PUB_ATTR_HEALTH_PRIMARY_COMP].is_valid = true;
        s_current_values[ZB_PUB_ATTR_HEALTH_PRIMARY_COMP].v.val_u8 = (uint8_t)h_snap.primary_component;

        s_current_values[ZB_PUB_ATTR_HEALTH_PRIMARY_REAS].is_valid = true;
        s_current_values[ZB_PUB_ATTR_HEALTH_PRIMARY_REAS].v.val_u8 = (uint8_t)h_snap.primary_reason;
    }

    /* 21. EP4 SoC Die Temperature (Rate-limited continuous telemetry from System Metrics) */
    system_metrics_snapshot_t m_snap;
    if (system_metrics_get_snapshot(&m_snap) == ESP_OK) {
        if (m_snap.soc_temperature_valid) {
            s_current_values[ZB_PUB_ATTR_SOC_TEMP].is_valid = true;
            s_current_values[ZB_PUB_ATTR_SOC_TEMP].v.val_i16 =
                zigbee_state_publisher_encode_temperature(m_snap.soc_temperature_c, true);
        } else {
            /* Invalid sensor sample must NOT publish fake numeric */
            s_current_values[ZB_PUB_ATTR_SOC_TEMP].is_valid = false;
            s_current_values[ZB_PUB_ATTR_SOC_TEMP].v.val_i16 = ZCL_TEMP_INVALID_VALUE;
        }
    }
}

/**
 * @brief Resolves descriptor handles from device model once upon registration.
 */
static void resolve_descriptor_handles(void)
{
    for (int i = 0; i < ZB_PUB_ATTR_COUNT; i++) {
        if (s_cached_attr_descs[i] == NULL || s_cached_attr_descs[i] == EZB_INVALID_ZCL_ATTR_DESC) {
            s_cached_attr_descs[i] = zigbee_device_model_get_attr_desc(s_attr_mappings[i].ep_id,
                                                                       s_attr_mappings[i].cluster_id,
                                                                       s_attr_mappings[i].attr_id);
            if (s_cached_attr_descs[i] == EZB_INVALID_ZCL_ATTR_DESC) {
                ESP_LOGW(TAG, "Descriptor handle for %s (EP%u, Cluster 0x%04X, Attr 0x%04X) unresolved.",
                         s_attr_mappings[i].name, s_attr_mappings[i].ep_id,
                         s_attr_mappings[i].cluster_id, s_attr_mappings[i].attr_id);
            }
        }
    }
}

/**
 * @brief Checks if current health state represents a critical SoC temperature condition.
 */
static bool is_soc_temp_critical_transition(void)
{
    health_snapshot_t cur_h;
    if (health_manager_get_snapshot(&cur_h) == ESP_OK) {
        if (cur_h.primary_component == HEALTH_COMP_SOC_TEMPERATURE ||
            cur_h.primary_reason == HEALTH_REASON_SOC_TEMP_WARNING ||
            cur_h.primary_reason == HEALTH_REASON_SOC_TEMP_CRITICAL) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Synchronizes descriptor backing storage and submits ZCL reports.
 */
static void process_attribute_reports(bool force_sync)
{
    int64_t now = esp_timer_get_time();
    bool temp_critical = is_soc_temp_critical_transition();

    /* 1. Evaluate change detection against last submitted value */
    for (int i = 0; i < ZB_PUB_ATTR_COUNT; i++) {
        zb_pub_attr_key_t key = (zb_pub_attr_key_t)i;
        if (!s_current_values[key].is_valid) {
            continue;
        }

        if (force_sync) {
            s_dirty_flags[key] = true;
            continue;
        }

        /* Check if authoritative value has changed compared to last successfully submitted value */
        if (!values_equal(key, &s_current_values[key], &s_submitted_values[key])) {
            if (s_attr_mappings[key].policy == PUB_POLICY_IMMEDIATE_DISCRETE) {
                s_dirty_flags[key] = true;
            } else if (key == ZB_PUB_ATTR_SOC_TEMP) {
                /* SoC Die Temperature Policy (Section 14 & 15):
                 * Report on meaningful delta (>= 1.0°C) AND min interval (>= 60s),
                 * OR heartbeat timeout (15 min),
                 * OR critical temperature transition bypass (coalesced with health fields). */
                int64_t elapsed = now - s_last_report_time_us[key];
                int16_t cur_t = s_current_values[key].v.val_i16;
                int16_t sub_t = s_submitted_values[key].is_valid ? s_submitted_values[key].v.val_i16 : 0;
                int32_t delta = (int32_t)cur_t - (int32_t)sub_t;
                if (delta < 0) delta = -delta;

                if (temp_critical ||
                    s_last_report_time_us[key] == 0 ||
                    (delta >= ZB_PUB_SOC_TEMP_DELTA_MIN && elapsed >= ZB_PUB_SOC_TEMP_MIN_INTERVAL_US) ||
                    (elapsed >= ZB_PUB_SOC_TEMP_MAX_INTERVAL_US)) {
                    s_dirty_flags[key] = true;
                } else {
                    /* Coalesce: keep latest value, mark dirty for deferred dispatch */
                    s_diag.coalesced_updates++;
                }
            } else {
                /* General continuous telemetry rate limiting policy */
                int64_t elapsed = now - s_last_report_time_us[key];
                if (elapsed >= ZB_PUB_TELEMETRY_MIN_INTERVAL_US || s_last_report_time_us[key] == 0) {
                    s_dirty_flags[key] = true;
                } else {
                    /* Coalesce: keep latest value, mark dirty for deferred dispatch */
                    s_diag.coalesced_updates++;
                    s_dirty_flags[key] = true;
                }
            }
        }
    }

    /* 2. Process all attributes requiring descriptor backing sync or network report */
    for (int i = 0; i < ZB_PUB_ATTR_COUNT; i++) {
        zb_pub_attr_key_t key = (zb_pub_attr_key_t)i;
        if (!s_current_values[key].is_valid) {
            continue;
        }

        /* Check whether backing storage differs from desired state (descriptor needs update) */
        bool need_backing_sync = !values_equal(key, &s_current_values[key], &s_backing_values[key]);

        /* Suppress blind periodic retry of the same write that previously received
         * EZB_ERR_INV_STATE. If the desired value has not changed since the deferred
         * attempt, skip this cycle — reconciliation fires on the next force_full_sync(). */
        if (need_backing_sync && !force_sync &&
            s_deferred_on_inv_state[key].is_valid &&
            values_equal(key, &s_current_values[key], &s_deferred_on_inv_state[key])) {
            need_backing_sync = false;
        }

        bool need_report_send  = s_dirty_flags[key] && s_network_ready;

        /* Telemetry rate limiting check for network send */
        if (need_report_send && !force_sync) {
            if (key == ZB_PUB_ATTR_SOC_TEMP) {
                int64_t elapsed = now - s_last_report_time_us[key];
                if (!temp_critical && elapsed < ZB_PUB_SOC_TEMP_MIN_INTERVAL_US && s_last_report_time_us[key] != 0) {
                    need_report_send = false; /* Defer to reconciliation tick */
                }
            } else if (s_attr_mappings[key].policy == PUB_POLICY_RATE_LIMITED_TELEMETRY) {
                int64_t elapsed = now - s_last_report_time_us[key];
                if (elapsed < ZB_PUB_TELEMETRY_MIN_INTERVAL_US && s_last_report_time_us[key] != 0) {
                    need_report_send = false; /* Defer to reconciliation tick */
                }
            }
        }

        if (!need_backing_sync && !need_report_send) {
            continue;
        }

        /* 3. Acquire Zigbee SDK lock with bounded timeout */
        if (!s_lock_acquire_fn(ZB_PUB_LOCK_TIMEOUT_MS)) {
            s_diag.lock_timeouts++;
            ESP_LOGW(TAG, "Lock timeout (500 ms) updating %s. Retaining dirty flag.", s_attr_mappings[key].name);
            continue;
        }

        /* 4. Synchronize descriptor backing value */
        ezb_zcl_attr_desc_t desc = s_cached_attr_descs[key];
        if (desc != NULL && desc != EZB_INVALID_ZCL_ATTR_DESC) {
            const void *val_ptr = NULL;
            switch (s_attr_mappings[key].attr_type) {
                case EZB_ZCL_ATTR_TYPE_BOOL:   val_ptr = &s_current_values[key].v.val_bool; break;
                case EZB_ZCL_ATTR_TYPE_UINT8:  val_ptr = &s_current_values[key].v.val_u8;   break;
                case EZB_ZCL_ATTR_TYPE_DATA32:
                case EZB_ZCL_ATTR_TYPE_UINT32: val_ptr = &s_current_values[key].v.val_u32;  break;
                case EZB_ZCL_ATTR_TYPE_INT16:  val_ptr = &s_current_values[key].v.val_i16;  break;
                case EZB_ZCL_ATTR_TYPE_DATA24: val_ptr = s_current_values[key].v.val_data24; break;
                default: break;
            }

            if (val_ptr != NULL) {
                /* Use s_set_value_fn (test seam; defaults to real SDK call). */
                ezb_err_t set_err = (ezb_err_t)s_set_value_fn((void *)desc, val_ptr);

                if (set_err == EZB_ERR_NONE) {
                    /* SUCCESS: s_backing_values advances ONLY here */
                    s_backing_values[key] = s_current_values[key];
                    s_deferred_on_inv_state[key].is_valid = false; /* Clear prior deferral */

                } else if (set_err == EZB_ERR_INV_STATE) {
                    /* TRANSIENT: stack not ready for descriptor writes (before joined) */
                    s_deferred_on_inv_state[key] = s_current_values[key];
                    ESP_LOGD(TAG,
                             "attr_desc_set_value: %s deferred (EZB_ERR_INV_STATE; backing "
                             "unchanged; awaiting force_full_sync on network join)",
                             s_attr_mappings[key].name);

                } else {
                    ESP_LOGW(TAG, "ezb_zcl_attr_desc_set_value failed for %s (err: %d)",
                             s_attr_mappings[key].name, (int)set_err);
                }
            }
        }

        /* 5. Dispatch ZCL Report if network is ready */
        if (need_report_send) {
            ezb_zcl_report_attr_cmd_t cmd_req;
            memset(&cmd_req, 0, sizeof(cmd_req));

            /* Binding-based destination targeting coordinator */
            cmd_req.cmd_ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_NONE;
            cmd_req.cmd_ctrl.dst_ep = 0;
            cmd_req.cmd_ctrl.src_ep = s_attr_mappings[key].ep_id;
            cmd_req.cmd_ctrl.cluster_id = s_attr_mappings[key].cluster_id;
            cmd_req.cmd_ctrl.manuf_code = EZB_ZCL_STD_MANUF_CODE;
            cmd_req.cmd_ctrl.fc.direction = EZB_ZCL_CMD_DIRECTION_TO_CLI;
            cmd_req.cmd_ctrl.fc.manuf_specific = 0;
            cmd_req.cmd_ctrl.fc.dis_default_rsp = 1;
            cmd_req.payload.attr_id = s_attr_mappings[key].attr_id;

            s_diag.report_attempts++;
            ezb_err_t rep_err = ezb_zcl_report_attr_cmd_req(&cmd_req);

            if (rep_err == EZB_ERR_NONE) {
                s_diag.report_submit_success++;
                s_submitted_values[key] = s_current_values[key];
                s_dirty_flags[key] = false;
                s_last_report_time_us[key] = now;
                ESP_LOGD(TAG, "Report submitted: %s (EP%u, Cluster 0x%04X, Attr 0x%04X)",
                         s_attr_mappings[key].name, s_attr_mappings[key].ep_id,
                         s_attr_mappings[key].cluster_id, s_attr_mappings[key].attr_id);
            } else {
                s_diag.report_submit_fail++;
                /* Retain dirty flag for retry in next reconciliation / wake cycle */
                ESP_LOGW(TAG, "ezb_zcl_report_attr_cmd_req rejected for %s (err: %d). Retaining dirty.",
                         s_attr_mappings[key].name, (int)rep_err);
            }
        }

        /* Release SDK lock */
        s_lock_release_fn();
    }
}

/* ========================================================================= */
/* Publisher FreeRTOS Task Routine                                          */
/* ========================================================================= */

static void zb_state_publisher_task(void *pvParameters)
{
    (void)pvParameters;
    wdt_manager_register_task();
    ESP_LOGI(TAG, "Zigbee state publisher task started (Priority: %d, Stack: %d B)",
             ZB_PUB_TASK_PRIORITY, ZB_PUB_TASK_STACK_SIZE);

    /* Wait briefly for device model registration to complete if starting up concurrently */
    while (!zigbee_device_model_is_registered()) {
        wdt_manager_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    resolve_descriptor_handles();

    uint32_t notif_bits = 0;

    while (1) {
        wdt_manager_reset();

        /* Wait for state change notification or periodic reconciliation timeout (5 seconds) */
        BaseType_t notified = xTaskNotifyWait(0, ULONG_MAX, &notif_bits, pdMS_TO_TICKS(ZB_PUB_RECONCILE_INTERVAL_MS));
        wdt_manager_reset();

        if (notified == pdTRUE) {
            s_diag.notif_count++;
        }

        bool force_sync = ((notif_bits & ZB_PUB_NOTIF_FORCE_FULL_SYNC) != 0);

        /* 1. Capture atomic authoritative snapshot */
        capture_authoritative_snapshot();

        /* 2. Process descriptor synchronization and ZCL reporting */
        process_attribute_reports(force_sync);
    }
}

/**
 * @brief Neutral callback for Health Manager transitions (No reverse dependency).
 */
static void prv_health_change_observer(const health_snapshot_t *snapshot, void *user_ctx)
{
    (void)snapshot;
    (void)user_ctx;
    /* Post non-blocking notification to publisher task */
    zigbee_state_publisher_notify(ZB_PUB_NOTIF_HEALTH | ZB_PUB_NOTIF_SOC_TEMP);
}

/* ========================================================================= */
/* Public API Implementation                                                 */
/* ========================================================================= */

esp_err_t zigbee_state_publisher_init(void)
{
    if (s_pub_task_handle != NULL) {
        ESP_LOGI(TAG, "State publisher already initialized (idempotent).");
        return ESP_OK;
    }

    /* Register as the single state-change observer in the state manager */
    z906_state_register_change_observer(
        (z906_state_change_cb_t)zigbee_state_publisher_notify);

    /* Register as neutral health change observer in Health Manager */
    health_manager_register_change_observer(prv_health_change_observer, NULL);

    s_pub_task_handle = xTaskCreateStatic(zb_state_publisher_task,
                                          "zb_state_pub",
                                          ZB_PUB_TASK_STACK_SIZE,
                                          NULL,
                                          ZB_PUB_TASK_PRIORITY,
                                          s_pub_task_stack,
                                          &s_pub_task_tcb);

    if (s_pub_task_handle == NULL) {
        /* Deregister observers if task creation failed */
        z906_state_register_change_observer(NULL);
        health_manager_register_change_observer(NULL, NULL);
        ESP_LOGE(TAG, "Failed to create static zb_state_pub task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "State publisher initialized and observers registered (State Manager & Health Manager).");
    return ESP_OK;
}

void zigbee_state_publisher_notify(uint32_t change_mask)
{
    if (s_pub_task_handle != NULL) {
        xTaskNotify(s_pub_task_handle, change_mask, eSetBits);
    }
}

void zigbee_state_publisher_set_network_ready(bool ready)
{
    if (s_network_ready != ready) {
        s_network_ready = ready;
        ESP_LOGI(TAG, "Network-ready reporting gate updated: %s", ready ? "READY" : "NOT READY");

        if (ready) {
            /* Trigger full resync when transitioning from false -> true */
            zigbee_state_publisher_force_full_sync();
        }
    }
}

bool zigbee_state_publisher_is_network_ready(void)
{
    return s_network_ready;
}

void zigbee_state_publisher_force_full_sync(void)
{
    s_diag.full_sync_count++;
    zigbee_state_publisher_notify(ZB_PUB_NOTIF_ALL_ATTRS | ZB_PUB_NOTIF_FORCE_FULL_SYNC);
}

void zigbee_state_publisher_set_value_fn(pub_set_value_fn_t fn)
{
    /* NULL restores the default (real SDK) implementation cast to portable type. */
    s_set_value_fn = (fn != NULL) ? fn : (pub_set_value_fn_t)ezb_zcl_attr_desc_set_value;
}

void zigbee_state_publisher_get_diagnostics(zb_pub_diagnostics_t *out_diag)
{
    if (out_diag != NULL) {
        *out_diag = s_diag;
    }
}

/* ========================================================================= */
/* Test Helper Implementations (FD-03 Seams)                                 */
/* ========================================================================= */

void zigbee_state_publisher_test_step(bool force_sync)
{
    process_attribute_reports(force_sync);
}

void zigbee_state_publisher_test_reset(void)
{
    for (int i = 0; i < ZB_PUB_ATTR_COUNT; i++) {
        s_current_values[i].is_valid = false;
        s_current_values[i].v.val_u32 = 0;
        s_backing_values[i].is_valid = false;
        s_backing_values[i].v.val_u32 = 0;
        s_submitted_values[i].is_valid = false;
        s_submitted_values[i].v.val_u32 = 0;
        s_deferred_on_inv_state[i].is_valid = false;
        s_deferred_on_inv_state[i].v.val_u32 = 0;
        s_dirty_flags[i] = false;
        s_last_report_time_us[i] = 0;
        s_cached_attr_descs[i] = NULL;
    }
    memset(&s_diag, 0, sizeof(s_diag));
    s_network_ready = false;
    s_set_value_fn = (pub_set_value_fn_t)ezb_zcl_attr_desc_set_value;
    s_lock_acquire_fn = default_lock_acquire;
    s_lock_release_fn = default_lock_release;
}

static bool mock_lock_acquire(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;
}

static void mock_lock_release(void)
{
}

void zigbee_state_publisher_test_set_mock_descs(void)
{
    s_lock_acquire_fn = mock_lock_acquire;
    s_lock_release_fn = mock_lock_release;
    for (int i = 0; i < ZB_PUB_ATTR_COUNT; i++) {
        /* Non-null dummy handle recognized as valid cached descriptor handle */
        s_cached_attr_descs[i] = (ezb_zcl_attr_desc_t)(uintptr_t)(0x1000 + i * 4);
    }
}

void zigbee_state_publisher_test_set_desired(zb_pub_attr_key_t key, uint32_t val)
{
    if (key >= ZB_PUB_ATTR_COUNT) return;
    s_current_values[key].is_valid = true;
    switch (s_attr_mappings[key].attr_type) {
        case EZB_ZCL_ATTR_TYPE_BOOL:
            s_current_values[key].v.val_bool = (val != 0);
            break;
        case EZB_ZCL_ATTR_TYPE_UINT8:
            s_current_values[key].v.val_u8 = (uint8_t)val;
            break;
        case EZB_ZCL_ATTR_TYPE_INT16:
            s_current_values[key].v.val_i16 = (int16_t)val;
            break;
        default:
            s_current_values[key].v.val_u32 = val;
            break;
    }
}

bool zigbee_state_publisher_test_get_applied(zb_pub_attr_key_t key, uint32_t *out_val)
{
    if (key >= ZB_PUB_ATTR_COUNT || !s_backing_values[key].is_valid) return false;
    if (out_val != NULL) {
        switch (s_attr_mappings[key].attr_type) {
            case EZB_ZCL_ATTR_TYPE_BOOL:
                *out_val = s_backing_values[key].v.val_bool ? 1 : 0;
                break;
            case EZB_ZCL_ATTR_TYPE_UINT8:
                *out_val = s_backing_values[key].v.val_u8;
                break;
            case EZB_ZCL_ATTR_TYPE_INT16:
                *out_val = (uint32_t)(uint16_t)s_backing_values[key].v.val_i16;
                break;
            default:
                *out_val = s_backing_values[key].v.val_u32;
                break;
        }
    }
    return true;
}

bool zigbee_state_publisher_test_is_pending_deferred(zb_pub_attr_key_t key, uint32_t *out_val)
{
    if (key >= ZB_PUB_ATTR_COUNT || !s_deferred_on_inv_state[key].is_valid) return false;
    if (out_val != NULL) {
        switch (s_attr_mappings[key].attr_type) {
            case EZB_ZCL_ATTR_TYPE_BOOL:
                *out_val = s_deferred_on_inv_state[key].v.val_bool ? 1 : 0;
                break;
            case EZB_ZCL_ATTR_TYPE_UINT8:
                *out_val = s_deferred_on_inv_state[key].v.val_u8;
                break;
            case EZB_ZCL_ATTR_TYPE_INT16:
                *out_val = (uint32_t)(uint16_t)s_deferred_on_inv_state[key].v.val_i16;
                break;
            default:
                *out_val = s_deferred_on_inv_state[key].v.val_u32;
                break;
        }
    }
    return true;
}

bool zigbee_state_publisher_test_is_dirty(zb_pub_attr_key_t key)
{
    if (key >= ZB_PUB_ATTR_COUNT) return false;
    return s_dirty_flags[key];
}
