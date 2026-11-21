# CODEBASE.md — Living Technical Documentation

## 1. Project Overview

### 1.1 Purpose & Scope
This project provides a **Native Zigbee 3.0 Embedded Controller** for the **Logitech Z906 5.1 Surround Sound System**. It interfaces inline between the original physical Logitech Z906 Control Console and the Subwoofer/Amplifier mainboard via dual hardware UART channels, intercepting commands, synchronizing device state, and exposing full remote control capabilities over a native Zigbee 3.0 mesh network.

The system integrates directly with standard Zigbee coordinators (such as Home Assistant ZHA and Zigbee2MQTT) without requiring intermediate bridges, WiFi networks, or proprietary hubs.

### 1.2 Target Hardware & Platform Configuration
The hardware and software configuration defined in [platformio.ini](platformio.ini) and [sdkconfig.defaults](sdkconfig.defaults):

| Parameter | Specification | Details |
|---|---|---|
| **Microcontroller (SoC)** | Espressif ESP32-C6 | High-performance 32-bit RISC-V core (up to 160 MHz) |
| **Radio Subsystem** | Native IEEE 802.15.4 | Hardware 2.4 GHz Zigbee 3.0 & Thread radio |
| **Target Board** | `esp32-c6-devkitc-1` | Official Espressif ESP32-C6-DevKitC-1 development board |
| **Physical Flash Memory** | 8 MB (8,388,608 bytes) | Quad/Dual SPI NOR Flash, configured in DIO mode @ 80 MHz |
| **SRAM** | 320 KB | Internal high-speed SRAM |
| **Development Framework** | `espidf` (ESP-IDF v6.1.0) | Official Espressif IoT Development Framework |
| **Operating System** | FreeRTOS | Native preemptive real-time operating system (1000 Hz tick) |
| **Zigbee Stack** | ESP-Zigbee-SDK | Official Espressif ZCL/ZDO Zigbee 3.0 stack (`esp-zigbee-lib`) |
| **Zigbee Device Type** | Zigbee Router (ZR) | Mains-powered mesh router with continuous listening |
| **Language & Standard** | C / C++17, JavaScript (Node.js) | Bounded memory, strict types, zero runtime heap allocations |

### 1.3 Key Functional Capabilities
- **Canonical Protocol Compliance:** Fully audited against the foundational open-source reverse-engineering specification (`nomis/logitech-z906`), implementing canonical framing, opcode definitions, multi-stage power sequences, and verified 20-byte amplifier telemetry.
- **Strict Longitudinal Redundancy Check (LRC):** Full checksum verification `(sum(type..chk)) & 0xFF == 0` on every packet. Corrupted or invalid frames are strictly rejected before affecting state.
- **Single Source of Truth Command Architecture:** All 8 Logitech Z906 control commands are governed from a centralized schema ([tools/z906_commands_metadata.json](tools/z906_commands_metadata.json)) and registered in firmware via [src/core/cmd_verifier/z906_cmd_registry.h](src/core/cmd_verifier/z906_cmd_registry.h) and [src/core/cmd_verifier/z906_cmd_registry.c](src/core/cmd_verifier/z906_cmd_registry.c).
- **Strict Separation of Concerns:** The central command registry manages static metadata, parameter bounds, and interface mappings; physical execution and hardware verification are strictly delegated to the state-based verifier engine ([src/core/cmd_verifier/z906_cmd_verifier.c](src/core/cmd_verifier/z906_cmd_verifier.c)).
- **Preserved State-Based Command Verifier:** The verified physical state engine (`z906_cmd_verifier`) is 100% preserved. It enforces DE-15 cable preconditions, Standby power locks, UART transmission, telemetry polling, and FreeRTOS mutex synchronization (`s_verifier_mutex`). No bypass paths exist.
- **Lockstep Command Parity:** 100% parity across CLI console, Zigbee 3.0 clusters, and Home Assistant / Zigbee2MQTT External Converter for Power, Volume (0-43), Mute, Inputs (6-Channel Direct, RCA Stereo, Optical 1, Optical 2, Digital Coaxial, Auxiliary/AUX), Effects (3D, 2.1, 4.1, None), and Channel Trims (Bass, Center, Rear).
- **Hardware Telemetry & Zero Optimistic Assumptions:** Device power is grounded exclusively in confirmed Byte 17 (`standby`: 0=ON, 1=Standby) of the 20-byte `AA 0A 14` amplifier status frame. The firmware never assumes power states optimistically.
- **Bi-directional Physical Status Synchronization:** Physical console/remote control button presses (input, effect, volume, mute, power) are intercepted via amplifier echo bytes (`0x02`–`0x07` inputs, `0x12`–`0x16` effects, `0x08`/`0x09` volume, `0x36` standby) decoded in `z906_fsm.c:handle_amp_byte`. Echo opcodes map to canonical `z906_input_t` enum values (`Z906_INPUT_SIX_CHANNEL_DIRECT` ... `Z906_INPUT_AUXILIARY`). State changes trigger immediate delta-driven Zigbee attribute reports without polling.
- **Full Internal Telemetry & Health Diagnostics Transfer to Home Assistant (F001 Option A + Health):** Temperature sensors `temperature_1/2/3` (°C) are exposed via standard `ZCL_CLUSTER_TEMP_MEASUREMENT` (`0x0402`) on dedicated Endpoints 11/12/13 respectively. Non-temperature telemetry (`input_signal_level`, `idle_time`, `digital_signal_status`, `auto_standby`), feature toggles (`built_in_led_enabled`, `command_verifier_enabled`), and selective health diagnostics (`device_health`, `health_primary_component`, `health_primary_reason`, `soc_temperature`) are exposed via `ZCL_CLUSTER_Z906_TELEMETRY` (`0xFC03`) on Endpoint 4 — exactly 10 canonical attributes (21 total reportable attributes across the data model), legacy temperature attributes 0x0004-0x0006 permanently absent.
- **Collision-Free Sequential Telemetry Polling:** Telemetry module queries (`0x25` temp, `0x2F` input_vol, `0x31` idle, `0x34` status) cycle one-at-a-time every 5 seconds using non-blocking `z906_cmd_verifier_try_lock()`. User control commands maintain absolute priority; concurrent queries are skipped rather than interleaved.
- **System Metrics & Health Manager Subsystems:** Clear architectural bifurcation:
  - **`System Metrics` (MEASURE):** Samples raw runtime metrics (SoC die temperature via on-chip sensor, O(1) CPU load delta via FreeRTOS idle counters, 8-bit internal heap capacity, uptime, reset reason) with zero business logic.
  - **`Health Manager` (INTERPRET):** Evaluates system metrics and subsystem operational states against strict hysteresis and debounce policies, selecting primary degraded components and fault reasons without executing automated recoveries.
- **Modern & Modular Debug Console:** Centralized diagnostic management service ([src/management/debug_console/debug_console.h](src/management/debug_console/debug_console.h)) providing runtime global log level switching (`log_level`), static module/tag filtering (`log_filter`), heap/uptime monitoring (`debug_status`), and deep Watchdog inspection (`wdt_status`).
- **Watchdog Diagnostic Integration:** Enhanced Task Watchdog Timer manager ([src/management/watchdog/wdt_manager.h](src/management/watchdog/wdt_manager.h)) with non-invasive hardware reset reason inspection (`esp_reset_reason()`), timeout reporting, and boot event analysis without compromising fault detection or task starvation safeguards.
- **Automated Parity Validator:** Dedicated test script ([tools/validate_command_parity.js](tools/validate_command_parity.js)) continuously verifies cross-layer synchronization between metadata JSON, converter JS, and firmware C headers.
- **Zigbee2MQTT External Converter (Phase 8 ESM):** Production converter ([tools/conductor_converter.mjs](tools/conductor_converter.mjs)) — ESM format (`import`/`export default`), fingerprint-based device detection, correct F001-migrated FC03 attribute mapping, EP11/12/13 temperature handlers with `0x8000` sentinel suppression, and `ota: true` for firmware update support.
- **Persistent Pairing & Auto-Rejoin Lifecycle:** Zigbee network metadata (PAN ID, Channel, Extended PAN ID) is persisted to NVS in the `zb_store` namespace. On reboot or power loss, the device automatically restores credentials and initiates non-blocking reconnection (`ZB_STATE_REJOINING`) without wiping data.
- **Interactive USB CLI (Bifurcated Architecture):** On-device command line interface accessible via USB Serial/JTAG strictly decoupled into User Commands (`power`, `volume`, `trim`, `input`, `effect`, `mute`, `status`, `health`, `metrics`, `devmode`) and Developer Tools (`dev log_level`, `dev log_filter`, `dev debug_status`, `dev wdt_status`, `dev temp`, `dev idle`, `dev zb_status`, `dev zb_pair`, `dev zb_leave`, `dev zb_reset`). Developer Tools are gated behind session-scoped Developer Mode (`Default: OFF`).

---

## 2. Canonical Protocol Reference & Technical Baseline

The serial communication implementation is rigorously audited against and governed by the foundational open-source Logitech Z906 reverse-engineering documentation (`nomis/logitech-z906`):

1. **`README.rst`**: Overview of the DE-15 connector, physical pinouts, and hardware signal lines.
2. **`firmware.rst`**: Microcontroller architecture, memory map, bootloader behavior, and EEPROM layout of the original console and mainboard.
3. **`interface.rst`**: Physical layer specifications, voltage levels, line sense pins, and exact serial transmission parameters.
4. **`protocol.rst`**: Complete packet structure, command opcodes, framing bytes, LRC checksum algorithm, amplifier status frame schema (`AA 0A 14`), and telemetry queries.

### 2.1 Canonical Framing & LRC Checksum
Every serial packet conforms to the standard packet format:
```text
+--------------+-------------+----------------+------------------+--------------+
| Start Byte 1 | Type / Dst  | Length (bytes) | Payload (0-N)    | Checksum     |
| 0xAA / 0xFF  | 1 Byte      | 1 Byte (0-20)  | N Bytes          | 1 Byte (LRC) |
+--------------+-------------+----------------+------------------+--------------+
```
- **Packet Types:**
  - `0x0A`: Console to Amplifier command packet.
  - `0x0B`: Console to Amplifier secondary command / data packet.
  - `0x05`: Amplifier to Console general response packet.
  - `0x0C`: Temperature telemetry response packet (5 bytes).
  - `0x0F`: Idle timer telemetry response packet (3 bytes).
  - `0x08`: Input volume telemetry response packet (3 bytes).
- **LRC Checksum Formula:**
  ```c
  uint8_t sum = type + len;
  for (size_t i = 0; i < len; i++) {
      sum += payload[i];
  }
  uint8_t lrc = (uint8_t)(0x100 - sum); // sum + lrc == 0 mod 256
  ```

### 2.2 Canonical Amplifier Status Frame (`AA 0A 14`)
When queried with `Z906_CMD_STATUS_REQ` (`0x34`), the amplifier responds with a 20-byte status frame (`AA 0A 14 ... [LRC]`):
- **Byte 0:** Input channel index (`0`=6-Channel Direct, `1`=RCA Stereo, `2`=Optical 1, `3`=Optical 2, `4`=Digital Coaxial, `5`=Auxiliary/AUX). Maps to `z906_input_t` values 1-6 via `z906_cmd_from_input()`.
- **Byte 1:** Effect mode (`0 = 3D, 1 = 4.1, 2 = 2.1, 3 = No Effect`).
- **Byte 2:** Master Volume level (`0 - 43`).
- **Byte 3:** Rear Volume trim (`0 - 43`).
- **Byte 4:** Center Volume trim (`0 - 43`).
- **Byte 5:** Subwoofer (Bass) Volume trim (`0 - 43`).
- **Byte 11:** Mute status (`0 = Unmuted, 1 = Muted`).
- **Byte 12:** Digital stream status (`0x00 = Not streaming, 0x01 = Streaming, 0x0C = Unknown, 0x0E = DTS/Bitstream`).
- **Byte 17:** Standby status (`0 = System Active / Powered ON, 1 = Standby Mode`).
- **Byte 19:** Auto-standby enabled status (`0 = Enabled, 1 = Disabled`).

---

## 3. System Architecture

### 3.1 Layered Architectural Model
The project architecture strictly decouples hardware access, protocol serialization, application state, and wireless network transport across five distinct layers:

```text
+-----------------------------------------------------------------------------------+
|               MERKEZİ DOĞRULUK KAYNAĞI (Single Source of Truth)                  |
|   1. tools/z906_commands_metadata.json (Portable JSON Schema & Interface Map)     |
|   2. src/core/cmd_verifier/z906_cmd_registry.h & .c (Firmware Static Registry)    |
|   3. Canonical Protocol Reference (nomis/logitech-z906 protocol.rst)              |
+-----------------------------------------------------------------------------------+
             |                                    |                        |
             v                                    v                        v
+------------------------+      +------------------------+      +-------------------+
|      CLI Konsolu       |      |     Zigbee Handlers    |      | External Converter|
| (cli_console.c)        |      | (zb_handlers.c)        |      | (converter.mjs)   |
| - Registry lookup      |      | - ZCL Endpoint/Cluster |      | - HA Exposes      |
| - Bounds validation    |      | - Attr ID & Types      |      | - to/fromZigbee   |
| - Debug Console & WDT  |      +------------------------+      +-------------------+
| - Health & Metrics     |                                 
+------------------------+                                 
             |                                    |
             +------------------+-----------------+
                                |
                                v  (z906_cmd_registry_build_request -> cmd_request_t)
+-----------------------------------------------------------------------------------+
|       PRESERVED STATE-BASED VERIFICATION ENGINE (z906_cmd_verifier)              |
| - Mutex Concurrency Lock (s_verifier_mutex prevents CLI vs Zigbee race conditions)|
| - Pre-State Read (z906_state_get) & Preconditions (DE-15 cable, Standby mode)     |
| - Target State Calculation & Canonical Serial Sequencing                          |
| - Physical UART Transmission (send_physical_command)                              |
| - Confirmed Hardware Telemetry Polling (Byte 17 standby, volume, mute, etc.)      |
| - Outcome Classification (SUCCESS, PRECONDITION_FAILED, TIMEOUT, BUSY, etc.)      |
+-----------------------------------------------------------------------------------+
                                │
                                ▼
+-----------------------------------------------------------------------------------+
|                     MANAGEMENT & PLATFORM DIAGNOSTICS                             |
|  - System Metrics (src/management/system_metrics): Raw SoC & FreeRTOS Telemetry   |
|  - Health Manager (src/management/health_manager): Policy Evaluation & Snapshots  |
|  - Debug Console (src/management/debug_console): Tag Filters & Dynamic Log Levels |
|  - Task Watchdog (src/management/watchdog): TWDT Health & Reset Reason Analysis   |
|  - NVS Storage (src/management/nvs): Persistent Zigbee Credentials ('zb_store')   |
+-----------------------------------------------------------------------------------+
```

---

## 4. Project Directory Structure

Every path listed below is relative to the project root and reflects the verified filesystem:

```text
.
├── .github/                        # GitHub Actions CI/CD workflows, community standards & issue forms
│   ├── CODEOWNERS                  # Primary maintainer ownership mapping
│   ├── CODE_OF_CONDUCT.md          # Contributor Covenant Code of Conduct v2.1
│   ├── CONTRIBUTING.md             # Developer contribution guidelines and PR lifecycle
│   ├── SECURITY.md                 # Security policy & Private Vulnerability Reporting instructions
│   ├── dependabot.yml              # Dependabot configuration (github-actions & pip schedules)
│   ├── release.yml                 # Automated release notes changelog categories
│   ├── requirements.txt            # Pinned build-time Python dependencies (platformio==6.1.16)
│   ├── ISSUE_TEMPLATE/             # Structured GitHub Issue forms
│   │   ├── bug_report.yml          # Bug report form with component classification
│   │   ├── config.yml              # Security redirection & discussions contact links
│   │   └── feature_request.yml     # Feature enhancement request form
│   ├── PULL_REQUEST_TEMPLATE.md    # Standard PR checklist & hardware verification notes
│   └── workflows/                  # GitHub Actions workflow definitions
│       ├── ci.yml                  # PR & push CI pipeline (host validators, PlatformIO build, dependency review)
│       └── release_ota.yml         # Tagged release pipeline (build, package, attest provenance, publish)
├── .gitignore                      # Git ignore rules for build artifacts and IDE cache
├── AGENTS.md                       # AI coding agent development guidelines and living documentation rules
├── CODEBASE.md                     # Living technical documentation of architecture and system specification
├── CMakeLists.txt                  # Root CMake project configuration for ESP-IDF
├── dependencies.lock               # Espressif IDF component manager lockfile
├── eim_config.toml                 # ESP-IDF Image Manager partition configuration
├── partitions.csv                  # Dual OTA partition table (ota_0 / ota_1 1792 KB slots)
├── platformio.ini                  # PlatformIO configuration (esp32-c6-devkitc-1, espidf framework)
├── README.md                       # Primary project documentation, hardware specs, and quickstart guide
├── sdkconfig.defaults              # Default ESP-IDF configuration (USB-JTAG console, 8MB flash @ 80MHz DIO)
├── sdkconfig.esp32-c6-devkitc-1    # Generated target-specific SDK configuration cache
├── include/                        # Public configuration headers
│   ├── README                      # PlatformIO header usage instructions
│   └── config.h                    # Global constants, pin definitions, timing, and Zigbee parameters
├── lib/                            # Project-specific private libraries (if any)
│   └── README                      # PlatformIO private library directory documentation
├── src/                            # Application source tree (Multi-tier nested architecture)
│   ├── CMakeLists.txt              # Component registration and nested include discovery
│   ├── main.c                      # Thin system entry point (app_main) — delegates entirely to system_initializer_run()
│   │
│   ├── system/                     # System lifecycle orchestration layer
│   │   └── initializer/            # Deterministic boot sequencer and readiness authority
│   │       ├── system_initializer.h # Boot phases, state model, report struct, and public API
│   │       └── system_initializer.c # Phased boot, failure policy, and FreeRTOS task spawning
│   │
│   ├── core/                       # Core system architecture & state management
│   │   ├── cmd_verifier/           # State-based command verification & central command registry
│   │   │   ├── z906_cmd_registry.h # Central command table declarations & parameter validators
│   │   │   ├── z906_cmd_registry.c # Central command table definitions & request builders
│   │   │   ├── z906_cmd_verifier.h # State verifier engine header & request data structures
│   │   │   └── z906_cmd_verifier.c # Preserved state verifier implementation & canonical sequencing
│   │   ├── event_bus/              # Asynchronous thread-safe event dispatcher
│   │   │   ├── event_bus.h
│   │   │   └── event_bus.c
│   │   ├── service_locator/        # Centralized dependency injection & service registry
│   │   │   ├── service_locator.h
│   │   │   └── service_locator.c
│   │   ├── state_machine/          # Z906 Finite State Machine (FSM) engine & telemetry tracker
│   │   │   ├── z906_fsm.h
│   │   │   └── z906_fsm.c
│   │   └── state_manager/          # Verified hardware state store and telemetry cache
│   │       ├── z906_state.h
│   │       └── z906_state.c
│   │
│   ├── hardware/                   # Hardware Abstraction Layer (HAL) implementations
│   │   ├── adc/                    # ADC One-Shot driver & voltage calibration
│   │   │   ├── adc_hal.h
│   │   │   └── adc_hal.c
│   │   ├── gpio/                   # GPIO pin initialization and safe state driver
│   │   │   ├── gpio_hal.h
│   │   │   └── gpio_hal.c
│   │   ├── led/                    # Onboard WS2812 RGB LED hardware driver
│   │   │   ├── led_driver.h
│   │   │   └── led_driver.c
│   │   └── uart/                   # Pure UART HAL — low-level FIFO read/write only (no task ownership)
│   │       ├── uart_hal.h
│   │       └── uart_hal.c
│   │
│   ├── protocol/                   # Z906 serial protocol codec and framing
│   │   ├── codec/                  # Strict LRC serializer, packet parser, and telemetry decoders
│   │   │   ├── z906_codec.h
│   │   │   └── z906_codec.c
│   │   └── commands/               # Command opcodes, headers, decode states, and scale converters
│   │       ├── z906_commands.h
│   │       └── z906_commands.c
│   │
│   ├── communication/              # Wireless, interactive & transport communication interfaces
│   │   ├── z906_transport/         # UART-to-EventBus adapter (owns uart_rx_task, private semaphore startup)
│   │   │   ├── z906_transport.h
│   │   │   └── z906_transport.c
│   │   ├── cli/                    # USB Serial/JTAG diagnostic console interface
│   │   │   ├── cli_console.h
│   │   │   ├── cli_console.c       # Main Menu & User Commands dispatcher (includes 'health' and 'metrics')
│   │   │   ├── cli_dev_tools.h     # Developer Tools header & declarations
│   │   │   ├── cli_dev_tools.c     # Developer diagnostic tools & subsystem commands
│   │   │   ├── cli_led_commands.h  # Onboard LED CLI command declarations
│   │   │   └── cli_led_commands.c  # Onboard LED CLI commands implementation
│   │   └── zigbee/                 # ESP-Zigbee-SDK integration & cluster definitions
│   │       ├── clusters/           # ZCL clusters (Basic, On/Off, Level, Media Trims, Sources)
│   │       │   ├── zcl_clusters.h
│   │       │   └── zcl_clusters.c
│   │       ├── handlers/           # ZCL command & ZDO network steering handlers
│   │       │   ├── zb_handlers.h
│   │       │   └── zb_handlers.c
│   │       ├── zigbee_app.h        # Zigbee stack lifecycle & pairing FSM
│   │       ├── zigbee_app.c
│   │       ├── zigbee_native_stack.h # Native ESP Zigbee SDK v2 stack lifecycle & platform management
│   │       ├── zigbee_native_stack.c
│   │       ├── zigbee_device_model.h # Native ESP Zigbee SDK v2 descriptor tree & attribute data model
│   │       ├── zigbee_device_model.c
│   │       ├── zigbee_command_ingress.h # Non-blocking ZCL command ingress, bounded queuing & hardware bridge
│   │       ├── zigbee_command_ingress.c
│   │       ├── zigbee_state_publisher.h # Verified state publishing & changed-only ZCL reporting
│   │       └── zigbee_state_publisher.c
│   │
│   └── management/                 # System life-cycle, monitoring & storage
│       ├── debug_console/          # Centralized Debug Console & tag-based logging filter
│       │   ├── debug_console.h
│       │   └── debug_console.c
│       ├── system_metrics/         # ESP32-C6 internal metrics capture (SoC temp, CPU load delta, heap)
│       │   ├── system_metrics.h
│       │   └── system_metrics.c
│       ├── health_manager/         # Unified system health evaluation engine & snapshot cache
│       │   ├── health_policy.h     # Health thresholds, sustained sample limits & hysteresis
│       │   ├── health_manager.h
│       │   └── health_manager.c
│       ├── led_notification/       # Modular LED notification system & priority engine
│       │   ├── Animations/         # Discrete animation implementations
│       │   │   ├── rainbow_anim.h
│       │   │   ├── rainbow_anim.c
│       │   │   ├── white_double_blink_anim.h
│       │   │   ├── white_double_blink_anim.c
│       │   │   ├── red_sos_anim.h
│       │   │   ├── red_sos_anim.c
│       │   │   ├── green_blink_anim.h
│       │   │   ├── green_blink_anim.c
│       │   │   ├── police_flasher_anim.h
│       │   │   └── police_flasher_anim.c
│       │   ├── led_animation.h     # Common animation interface contract
│       │   ├── led_animation_registry.h
│       │   ├── led_animation_registry.c
│       │   ├── led_nvs_storage.h
│       │   ├── led_nvs_storage.c
│       │   ├── led_notification_manager.h
│       │   └── led_notification_manager.c
│       ├── nvs/                    # Non-Volatile Storage (NVS) manager
│       │   ├── nvs_manager.h
│       │   └── nvs_manager.c
│       ├── telemetry/              # Periodic ADC telemetry and stack watermark tracker
│       │   ├── telemetry.h
│       │   └── telemetry.c
│       └── watchdog/               # Task Watchdog Timer (TWDT) manager & reset diagnostics
│           ├── wdt_manager.h
│           └── wdt_manager.c
│
├── test/                           # Nested unit test suites
│   ├── README                      # PlatformIO unit test documentation
│   ├── communication/              # Communication and wireless unit tests
│   │   ├── test_cli/               # CLI User Commands, Developer Mode gating, and hierarchy tests
│   │   │   └── test_cli_console.c
│   │   ├── test_zigbee/            # Zigbee pairing FSM, NVS persistence, and ZCL command parity tests
│   │   │   └── test_zigbee_app.c
│   │   └── test_zigbee_publisher/  # Native Zigbee state publisher unit tests
│   │       └── test_zigbee_state_publisher.c
│   ├── core/                       # Core engine unit tests
│   │   ├── test_cmd_verifier/      # Command verification pipeline & 11 mandated integration scenarios
│   │   │   └── test_cmd_verifier.c
│   │   ├── test_service_locator/   # Service locator registration and boundary tests
│   │   │   └── test_service_locator.c
│   │   └── test_state_manager/     # State manager defaults and telemetry boundary tests
│   │       └── test_state_manager.c
│   ├── management/                 # Management & diagnostic unit tests
│   │   ├── test_debug_console/     # Debug Console log filtering & Watchdog diagnostic tests
│   │   │   └── test_debug_console.c
│   │   ├── test_system_metrics/    # System Metrics snapshot, hooks, and timing tests
│   │   │   └── test_system_metrics.c
│   │   ├── test_health_manager/    # Health Manager policy, debounce, and hysteresis tests
│   │   │   └── test_health_manager.c
│   │   ├── test_led_notification/  # LED animation, registry, NVS persistence & priority tests
│   │   │   └── test_led_notification.c
│   │   └── test_ota/               # Zigbee OTA client, header validation, health evaluation & rollback safety
│   │       └── test_ota.c
│   ├── protocol/                   # Protocol and framing tests
│   │   ├── test_codec/             # Multi-byte parsing and literal reference LRC validation
│   │   │   └── test_codec.c
│   │   └── test_commands/          # Opcode mappings, scale conversions, and literal reference tests
│   │       └── test_commands.c
│   └── system/                     # System lifecycle unit tests
│       ├── test_initializer/       # Boot sequence, state model, and boot report validation
│       │   └── test_system_initializer.c
│       ├── test_initializer_failure/ # Fatal component failure injection tests (AC-01..AC-04)
│       │   └── test_system_initializer_failure.c
│       └── test_initializer_degraded/ # Degraded component failure injection tests (AC-05..AC-11)
│           └── test_system_initializer_degraded.c
│
└── tools/                               # External tools, metadata schemas, and validators
    ├── z906_commands_metadata.json      # Centralized JSON command metadata (Single Source of Truth)
    ├── validate_command_parity.js       # Cross-layer parity verification (metadata ↔ firmware ↔ converter)
    ├── conductor_converter.mjs          # Phase 8 ESM Zigbee2MQTT External Converter (CANONICAL — ACTIVE)
    ├── test_converter.js                # Canonical suite for conductor_converter.mjs
    ├── build_ota_image.py               # ZCL OTA container packer (header + UPGRADE_IMAGE sub-element)
    ├── generate_ota_index.js            # Zigbee2MQTT OTA index.json generator
    ├── test_ota_package.js              # OTA round-trip + index parity suite
    ├── test_zigbee_ingress.js           # Command ingress unit tests
    ├── test_zigbee_lifecycle.js         # Zigbee pairing lifecycle unit tests
    ├── test_zigbee_ota.js               # OTA lifecycle and partition boundary tests
    └── test_zigbee_state_publisher.js   # State publisher unit tests
```

---

## 5. Hardware Architecture & Pinout (ESP32-C6)

### 5.1 Physical Pinout & DE-15 Interconnection Map
The ESP32-C6 is interposed inline between the original Logitech Z906 Control Console (female DE-15) and the Z906 Subwoofer/Amplifier mainboard (male DE-15).

Pin configuration defined in [include/config.h](include/config.h):

| DE-15 Pin | Amplifier Connector | Console Connector | ESP32-C6 GPIO | Direction / Functional Signal |
|:---:|:---:|:---:|:---:|---|
| **1** | Connected | Connected | — | Passthrough (Direct connection) |
| **2** | Connected | Connected | — | Passthrough (Direct connection) |
| **3** | Connected | Connected | — | Passthrough (Direct connection) |
| **4** | Connected | Connected | — | Passthrough (Direct connection) |
| **5** | Connected | Connected | — | Passthrough (Direct connection) |
| **6** | GND | GND | **GND** | Common System Ground |
| **7** | Connected | Connected | — | Passthrough (Direct connection) |
| **8** | — | Connected | **GPIO 2** (ADC1_CH2) | Console Boot Voltage Sense (`ADC_CON_BOOT`) |
| **9** | — | Connected | **GPIO 3** (ADC1_CH3) | Cable Connected Detection (`ADC_CABLE_DETECT`) |
| **10** | Connected | Connected | — | Passthrough (Direct connection) |
| **11** | Connected | Connected | — | Passthrough (Direct connection) |
| **12** | Connected (TX) | — | **GPIO 22** (UART0 RX) | Amplifier Serial Telemetry Input (`RX_AMP_GPIO`) |
| **13** | Connected (RX) | — | **GPIO 23** (UART0 TX) | Amplifier Serial Command Output (`TX_AMP_GPIO`) |
| **14** | Connected | Connected | — | Passthrough (Direct connection) |
| **15** | — | Connected | **GPIO 1** (ADC1_CH1) | Console Power Level Sense (`ADC_PIN_LOW`) |
| **—** | — | Connected (TX) | **GPIO 16** (UART1 RX) | Console Serial Command Input (`RX_CONSOLE_GPIO`) |
| **—** | — | Connected (RX) | **GPIO 17** (UART1 TX) | Console Serial Response Output (`TX_CONSOLE_GPIO`) |

### 5.2 Serial Communication Specs
- **Baud Rate & Framing:** 57,600 baud, 8 data bits, **Odd Parity**, 1 stop bit (`57600 8O1`). Governed strictly by `interface.rst` and implemented in `src/hardware/uart/uart_hal.c`.
- **Buffer Allocation:** Static 512-byte software ring buffer for both RX and TX channels.
- **Framing Timeout:** 500 ms frame assembly timeout.

---

## 6. Software Architecture & Module Responsibilities

### 6.0 System Lifecycle Layer (`src/system/`)

#### `src/system/initializer/system_initializer.[ch]` — Pure Boot Orchestration Authority
The System Initializer is the **single orchestration authority** for Conductor's boot sequence. It is a **pure orchestrator** — it calls subsystem APIs but owns no task body loops, no hardware register details, and no protocol logic.

**Boot Phases (in execution order):**

| Phase | Components Initialized | Fatal on Failure? |
|---|---|:---:|
| `EARLY` | NVS Flash, Task Watchdog (10s timeout), Debug Console | NVS=YES; WDT/DC=NO |
| `CORE` | Service Locator, Event Bus (static queue), State Manager, FSM, Cmd Verifier | EventBus/FSM=YES; Verifier=NO |
| `HARDWARE` | GPIO (line stabilization), ADC (cable sense), UART (57600 8O1 both channels) | GPIO/UART=YES; ADC=NO |
| `MANAGEMENT` | System Metrics, Health Manager, LED Notification, OTA Client + OTA Health init | All=NO (DEGRADED) |
| `CLI` | CLI Console (registers user command and dev tools tables) | NO |
| `ACTIVATE` | Spawns and starts FreeRTOS tasks via private semaphore handshake | FATAL=transport/fsm/zigbee/telemetry; DEGRADED=sys_metrics/led/cli |
| `READY` | OTA health signalled, s_state=READY (all tasks already running) | — |

**Degraded Activation Gating:**
- If `system_metrics_init()` failed → `system_metrics_start()` is NOT called.
- If `health_manager_init()` failed → `health_manager_start()` is NOT called.
- If `led_notification_init()` failed → `led_notification_start()` is NOT called.
- If `cli_console_init()` failed → `cli_console_start()` is NOT called.

---

## 7. Management Subsystems: System Metrics & Health Manager

### 7.1 Architecture & Separation of Concerns (MEASURE vs INTERPRET vs EXPOSE)
```text
System Metrics (src/management/system_metrics/)
[MEASURE: Raw ESP32-C6 internal metrics, O(1) CPU load delta, SoC die temp, heap]
                     │
                     ▼ (system_metrics_get_snapshot)
Health Manager (src/management/health_manager/)
[INTERPRET: Evaluates metrics + subsystem signals, applies hysteresis & debounce]
                     │
                     ▼ (health_manager_get_snapshot)
CLI / Diagnostics / Presentation
[EXPOSE: Formats for human inspection or reports via CLI 'health' & 'metrics']
```

### 7.2 System Metrics (`src/management/system_metrics/`)
- **Role:** Pure low-level measurement engine. Samples hardware/RTOS metrics and publishes immutable snapshots.
- **O(1) CPU Load Delta Calculation:**
  Uses FreeRTOS runtime statistics enabled in ESP-IDF (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` and `CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y`).
  ```c
  uint32_t idle_delta = now_idle - prev_idle;
  uint32_t total_delta = (uint32_t)(now_time_us - prev_time_us);
  float idle_pct = ((float)idle_delta / (float)total_delta) * 100.0f;
  float cpu_load = 100.0f - idle_pct;
  ```
  - Strictly O(1): Does not traverse task lists (`uxTaskGetSystemState()` is forbidden).
  - First sample after boot/reset is marked invalid (`cpu_load_valid = false`) to establish baseline.
  - Rollover and divide-by-zero protected; clamped strictly to `[0.0f, 100.0f]`.
- **SoC Die Temperature:**
  - Uses ESP32-C6 internal on-chip sensor (`temperature_sensor` driver from `esp_driver_tsens`).
  - Range: `-10 °C` to `+80 °C`.
  - Terminology is strictly **"SoC Die Temperature"** (`soc_temp_c`, `soc_temp_valid`). Never labeled "CPU Temperature".
- **Internal 8-Bit Heap Semantics:**
  - Queries `heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`.
  - Measures total internal heap size, currently used heap, free heap, minimum ever free heap (watermark), largest free block, and percent heap used.
  - Heap percent used is strictly labeled as **Heap %**, never as system "RAM %".
- **Execution & Storage:**
  - Dedicated static task `sys_metrics` (Priority 1, 3072 B stack, 1000ms period).
  - Mutex-protected snapshot buffer (`s_metrics_mutex`). Zero runtime heap allocation.

### 7.3 Health Manager (`src/management/health_manager/`)
- **Role:** Pure policy evaluation and interpretation engine. Evaluates telemetry inputs without performing I/O or automatic recovery.
- **Health States:**
  - `HEALTH_STATUS_UNKNOWN`: Bootstrapping or uninitialized.
  - `HEALTH_STATUS_HEALTHY`: All monitored subsystems operating normally within thresholds.
  - `HEALTH_STATUS_DEGRADED`: One or more subsystems experiencing non-critical strain (e.g. elevated temp, sustained CPU load, degraded optional component).
  - `HEALTH_STATUS_FAULT`: Critical condition requiring operator attention (e.g. critical overheating, memory exhaustion, TWDT reset).
- **Hysteresis & Debounce Thresholds (`health_policy.h`):**
  - **SoC Die Temperature:**
    - Warning Entry: `75.0 °C` | Warning Recovery: `< 70.0 °C`
    - Critical Entry: `85.0 °C` | Critical Recovery: `< 80.0 °C`
  - **CPU Load (Sustained):**
    - Warning Entry: `>= 85.0 %` for 5 consecutive samples (5s) | Exit: `< 75.0 %`
    - Critical Entry: `>= 95.0 %` for 10 consecutive samples (10s) | Exit: `< 85.0 %`
  - **Heap Memory:**
    - Warning: Heap used `>= 85.0 %` OR Min free heap `< 32 KB`
    - Critical: Heap used `>= 95.0 %` OR Min free heap `< 16 KB`
    - Fragmentation Warning: Largest block `< 8 KB`
  - **Data Staleness:**
    - Metrics staleness: Snapshot age `> 5000 ms`
    - Z906 telemetry staleness: Last update `> 10000 ms`
- **Component Priority Ranking:**
  When multiple subsystems report issues, the primary component is deterministically selected by severity:
  `TWDT` > `NVS` > `MEMORY` > `SOC_TEMP` > `CPU_LOAD` > `UART_TRANSPORT` > `Z906_AMPLIFIER` > `ZIGBEE` > `OTA` > `LED` > `CLI` > `SYSTEM_INITIALIZER`.
- **Zero Automatic Recovery Guarantee:**
  Health Manager strictly observes and reports. It never triggers `esp_restart()`, never manipulates GPIOs, and never resets tasks. Watchdog recovery remains the sole domain of the hardware TWDT.
- **Change-Only Diagnostic Logging:**
  Health evaluations run silently during normal operation. Log messages are emitted only upon a confirmed state change, preventing periodic log spam.

---

## 8. FreeRTOS Task Matrix & Resource Budget

The firmware enforces static memory allocation for all tasks, queues, and synchronization primitives:

| Task Name | Priority | Stack Depth | Allocation | Monitored Subsystem | TWDT Policy |
|---|:---:|:---:|---|---|---|
| `uart_rx_task` | 5 (High) | 4096 B | Static / ESP32-C6 | UART0/UART1 packet stream ingestion & EventBus feed | REGISTERED (10s) |
| `z906_fsm_task` | 4 (Med-High) | 4096 B | Static / ESP32-C6 | Protocol FSM state transitions & observer dispatches | REGISTERED (10s) |
| `zigbee_task` | 3 (Medium) | 4096 B | Static / ESP32-C6 | ESP-Zigbee-SDK v2 radio event loop & attribute reporting | REGISTERED (10s) |
| `zb_state_pub` | 3 (Medium) | 4096 B | Static / ESP32-C6 | Changed-only ZCL attribute publishing to coordinator | REGISTERED (10s) |
| `cli_task` | 2 (Med-Low) | 3072 B | Static / ESP32-C6 | Interactive USB Serial/JTAG CLI line reader & dispatcher | EXEMPT (Blocking I/O) |
| `telemetry_task` | 1 (Low) | 2048 B | Static / ESP32-C6 | ADC DE-15 cable sensing, Z906 sequential queries | REGISTERED (10s) |
| `sys_metrics` | 1 (Low) | 3072 B | Static / ESP32-C6 | ESP32-C6 internal metrics snapshot (CPU load, heap, SoC temp) | REGISTERED (10s) |
| `led_task` | 1 (Low) | 2048 B | Static / ESP32-C6 | Non-blocking WS2812 RGB LED animation & notification player | EXEMPT (Low latency) |

---

## 9. Verification & Testing

### 9.1 Verified Test Suites
The project includes 12 embedded C unit test suites verified directly on physical ESP32-C6 hardware (COM6) and 4 host-based Node.js test scripts:

1. **`test/management/test_system_metrics/test_system_metrics.c` (6 Tests - PASSED):**
   - Verifies snapshot generation, SoC die temperature validity, O(1) CPU load delta computation, memory metrics integrity, uptime progression, and test hook overrides.
2. **`test/management/test_health_manager/test_health_manager.c` (6 Tests - PASSED):**
   - Verifies healthy baseline evaluation, SoC die temp warning/critical thresholds and hysteresis, CPU load sustained sample debounce, memory pressure detection, and staleness marking.
3. **`test/system/test_initializer_degraded/test_system_initializer_degraded.c` (10 Tests - PASSED):**
   - Verifies degraded failure injection and gating across WDT, Debug Console, Command Verifier, ADC, System Metrics, Health Manager, LED Notification, OTA Client, and CLI Console.
4. **`test/system/test_initializer/test_system_initializer.c` (12 Tests - PASSED):**
   - Verifies boot sequence phasing, state machine transitions, re-entry safety, and boot report correctness.
5. **`test/system/test_initializer_failure/test_system_initializer_failure.c` (9 Tests - PASSED):**
   - Verifies fatal failure handling for NVS, EventBus, FSM, GPIO, UART HAL, transport start, FSM start, Zigbee start, and reboot-reset.
6. **`test/core/test_state_manager/test_state_manager.c` (13 Tests - PASSED):**
   - Verifies state cache defaults, boundary conditions, telemetry fields (`temp_celsius[3]`, `idle_seconds`), and delta observer notifications.
7. **`test/core/test_cmd_verifier/test_cmd_verifier.c` (20 Tests - PASSED):**
   - Comprehensive test suite covering 11 mandated integration scenarios, mutex locking, preconditions, and timeout handling.
8. **`test/management/test_debug_console/test_debug_console.c` (7 Tests - PASSED):**
   - Verifies log level switching, tag filtering, and Watchdog diagnostic inspection.
9. **`test/management/test_led_notification/test_led_notification.c` (17 Tests - PASSED):**
   - Verifies discrete animations, animation registry, priority engine, NVS persistence, and 5 physical OFF invariants.
10. **`test/management/test_ota/test_ota.c` (12 Tests - PASSED):**
    - Verifies canonical Zigbee OTA header parsing, magic, layout, downgrade rejection, and 15-second health window rollback triggers.
11. **`test/communication/test_zigbee/test_zigbee_app.c` (17 Tests - PASSED):**
    - Verifies Zigbee pairing FSM, NVS retention across reboot, auto-rejoin resilience, and ZCL parity.
12. **`test/communication/test_zigbee_publisher/test_zigbee_state_publisher.c` (9 Tests - PASSED):**
    - Verifies FD-03 desired vs. applied state semantics, `EZB_ERR_INV_STATE` deferral, health diagnostic attributes, temperature encoding, and network reconciliation.
13. **`tools/validate_command_parity.js` (Node.js - 8 Tests - PASSED):**
    - Automated cross-layer parity check across metadata JSON, converter JS, and firmware C headers.
14. **`tools/test_converter.js` (Node.js - 65 Tests - PASSED):**
    - Validates device metadata, endpoint mappings, volume scale conversions, null-return policy, canonical input keys, and 10 FC03 attributes including selective health diagnostics.
15. **`tools/test_zigbee_ingress.js` (Node.js - 38 Tests - PASSED):**
    - Validates cluster dispatch, bounded ingress queuing, and Level/OnOff command handling.
16. **`tools/test_zigbee_state_publisher.js` (Node.js - 40 Tests - PASSED):**
    - Validates state publication logic, 21-attribute coordinate alignment, changed-only reporting, SoC temp rate limiting, and critical health bypass.

### 9.2 Clean Build Verification
- **Command:** `pio run -e esp32-c6-devkitc-1 -t clean && pio run -e esp32-c6-devkitc-1`
- **Result:** **SUCCESS (0 warnings, 0 errors)**
- **Binary Footprint:**
  - **RAM (DRAM):** `18.2%` (59,580 bytes used out of 327,680 bytes)
  - **Flash:** `39.5%` (724,426 bytes used out of 1,835,008 bytes OTA application slot)
  - **OTA Headroom:** `1,110,582 bytes` (60.5% headroom remains in the active OTA partition)

---

## 10. Build, Test & Execution Commands

Execute all commands from the project root using relative paths:

- **Build project:**
  ```sh
  pio run -e esp32-c6-devkitc-1
  ```
- **Clean build:**
  ```sh
  pio run -t clean
  ```
- **Upload firmware:**
  ```sh
  pio run -e esp32-c6-devkitc-1 -t upload
  ```
- **Execute unit tests on hardware:**
  ```sh
  pio test -e esp32-c6-devkitc-1
  ```
- **Build unit tests without hardware upload:**
  ```sh
  pio test -e esp32-c6-devkitc-1 --without-uploading --without-testing
  ```
- **Run Command Parity Validator:**
  ```sh
  node tools/validate_command_parity.js
  ```
- **Run External Converter unit tests:**
  ```sh
  node tools/test_converter.js
  ```

---

## 11. Temperature Telemetry Pipeline

### 11.1 Protocol Reference (protocol.rst — Read Temperature 0x25)
Full 10-byte transaction:
| Byte | Value | Role |
|:---:|:---:|---|
| 0 | `0x25` | Console → Amp: query command (single-byte echo back) |
| 1 | `0xAA` | Multi-byte packet header |
| 2 | `0x0C` | Packet type (`Z906_MB_AMP_TEMP`) |
| 3 | `0x05` | Payload length (5 bytes, excludes checksum) |
| 4 | `0x00` | Padding byte (placeholder) |
| 5 | `TT` | **Temperature 1** (°C, signed) |
| 6 | `TT` | **Temperature 2** (°C, signed) |
| 7 | `TT` | **Temperature 3** (°C, signed — Power Supply Transformer) |
| 8 | `0x00` | Padding byte (placeholder) |
| 9 | `XX` | Checksum (LRC: `sum(type..chk) & 0xFF == 0`) |

### 11.2 Codec Parsing & Channel Mapping
The `z906_codec_ctx_t` payload mapping:
- `payload[1]` → `temp_celsius[0]` (Temperature 1)
- `payload[2]` → `temp_celsius[1]` (Temperature 2)
- `payload[3]` → `temp_celsius[2]` (Temperature 3 — PSU Transformer)

`temp_valid` in `z906_state_t` acts as the authoritative gate between uninitialized boot state (`false`) and verified measurement (`true`). `0 °C` with `temp_valid = true` is a valid physical measurement and is never treated as an error sentinel.

---

## 12. Native Zigbee OTA Firmware Upgrade System

### 12.1 Architectural Flow
Firmware updates use the ZCL OTA Upgrade Cluster specification (`0x0019`) on Client Endpoint 1:
- Transported exclusively over IEEE 802.15.4 Zigbee radio frames (zero direct Wi-Fi/HTTP).
- Dual symmetrical 1,792 KB application partitions (`ota_0` and `ota_1`) prevent in-place overwrites.
- **AUD-P02-01 / release fail-closed gate:** `release_ota.yml` builds exactly four assets (`firmware.bin`, the canonical `.ota`, `index.json`, `conductor_converter.mjs`). `tools/verify_release_assets.js` checks each regular file, OTA header/identity/version/payload parity against the firmware binary, index release URL and SHA-512, and the OTA slot limit before attestation and after artifact transfer. Upgraded `attest-build-provenance@v4.2.2`, `upload-artifact@v7.0.2` and `download-artifact@v8.0.2` participate; missing uploads and digest mismatches fail the job. The tag-only publish job refuses an existing Release, verifies all four downloaded attestations, uploads to a **draft** using `action-gh-release@v3.0.4` with `fail_on_unmatched_files: true`, and compares every remote asset's name, size and SHA-256 digest with local files before a separate, final publish command. Failed verification leaves the Release as a draft; recovery/retry needs an explicit operator decision rather than automatic deletion. No publish step runs on branch-only `workflow_dispatch`.
- **AUD-P02-02 / nonpublishing integrated smoke:** `.github/workflows/release_asset_smoke.yml` runs only on a same-repo pull request from `fix/release-ota-failclosed` with **read-only contents permission and no tag, dispatch or release creation**. It builds the actual firmware, packages/indexes it, exercises provenance v4 + upload v7 + download v8, validates all four transferred assets and their composite SHA-256 checksum, and checks each downloaded file's attestation. `tools/test_release_assets.js` exercises the gate with missing/corrupt/incorrect remote-asset negative cases. The smoke intentionally **does not execute `softprops/action-gh-release`**; creating a real GitHub Release and final URL availability remain NOT TESTED until a separate authorized tag/release acceptance. It is not physical OTA verification.
- 15-second post-update observation window (`ota_health.c`) monitors 5 criteria (scheduler, watchdog, NVS, Zigbee stack, Z906 comm).
- Automatic rollback to previous workable slot on failure via `esp_ota_mark_app_invalid_rollback_and_reboot()`.

---

## 13. System Initializer & Deterministic Boot Lifecycle

Boot sequence phases:
1. `BOOT_PHASE_STORAGE` [FATAL]: Default NVS flash partition.
2. `BOOT_PHASE_CORE` [FATAL / DEGRADED]: WDT (DEGRADED), Debug Console (DEGRADED), EventBus (FATAL), FSM (FATAL), Cmd Verifier (DEGRADED), Service Locator (FATAL).
3. `BOOT_PHASE_HARDWARE` [FATAL / DEGRADED]: GPIO (FATAL), ADC HAL (DEGRADED), UART HAL (FATAL).
4. `BOOT_PHASE_MANAGEMENT` [DEGRADED]: System Metrics, Health Manager, LED Notification, OTA Client.
5. `BOOT_PHASE_CLI` [DEGRADED]: CLI Console.
6. `BOOT_PHASE_ACTIVATE` [FATAL / DEGRADED GATED]: Spawns and starts FreeRTOS tasks via private semaphore handshake.
7. `BOOT_PHASE_VERIFY`: Finalizes boot report and sets `SYSTEM_INIT_READY`.

---

## 14. Interactive CLI Console & User Commands

Accessible over USB Serial/JTAG at 115,200 baud. Decoupled into User Commands (main menu) and Developer Tools (`devmode on`):

### 14.1 User Commands Table
| Command | Arguments | Description |
|---|---|---|
| `power` | `[on\|off]` | View or control main power state (canonical sequences) |
| `volume` | `[0-43]` | View or set master volume (discrete hardware steps) |
| `trim` | `<channel> [0-43]` | View or set trim for `sub` / `center` / `rear` |
| `input` | `[1-6]` | View or switch audio input (1=Direct, 2=RCA, 3=Opt1, 4=Opt2, 5=Coax, 6=Aux) |
| `effect` | `[3d\|2.1\|4.1\|none]` | View or change spatial effect mode (analog inputs only) |
| `mute` | `[on\|off]` | View or toggle audio mute |
| `status` | — | Display verified physical device state & telemetry summary |
| `health` | — | Display overall system health, primary degraded component, and reason |
| `metrics` | — | Display ESP32-C6 runtime metrics (SoC die temp, O(1) CPU load, heap, uptime) |
| `devmode` | `[on\|off]` | Toggle session-scoped Developer Mode (Default: OFF) |
| `help` | — | Display available commands |

### 14.2 Developer Tools (`dev <tool>`)
Available only when `devmode` is `ON`:
- `dev log_level [level]`: View or modify ESP-IDF log level.
- `dev log_filter [tag] [level|clear]`: Apply per-tag logging overrides.
- `dev debug_status`: Deep inspection of FreeRTOS tasks and heap.
- `dev wdt_status`: Task Watchdog Timer health, timeouts, and reset reason.
- `dev temp`: Raw amplifier temperatures (°C).
- `dev idle`: Z906 idle timer telemetry.
- `dev zb_status`, `dev zb_pair`, `dev zb_leave`, `dev zb_reset`: Zigbee network diagnostics and commissioning.

---

## 15. System Metrics Module (Measurement Authority)

### 15.1 Architectural Role
The System Metrics module (`src/management/system_metrics/`) is the pure runtime measurement authority for ESP32-C6 / ESP-IDF / FreeRTOS. It owns measurement, sampling timing, and atomic snapshot provisioning. It contains zero health policy or failure classification.

### 15.2 Specifications & Sensor Range
- **SoC Temperature Sensor:** Calibrated via ESP-IDF `temperature_sensor` driver configured for `[20°C, 100°C]` (matches hardware attribute table `{-1, 7, 20, 100, 2}`, error < 2°C).
- **CPU Load:** O(1) mathematical calculation derived from `ulTaskGetIdleRunTimeCounter()` delta against 1 MHz ESP Timer. First sample marked invalid (`cpu_load_valid = false`) to establish baseline.
- **Heap Accounting:** Internal 8-bit heap capacity, allocated bytes, free bytes, percentage usage, lifetime minimum free watermark (`heap_min_free_bytes`), and largest free block.
- **Task Lifecycle:** Dedicated static FreeRTOS task `sys_metrics` (Priority 1, 3072 B stack, 1000 ms periodic interval).
- **Measured Execution Profiling & Overhead:**
  - `min_sample_time_us`: ~141 µs
  - `avg_sample_time_us`: ~149 µs
  - `max_sample_time_us`: ~480 µs
  - `measured_duty_cycle_pct`: ~0.0149%
  - `task_stack_min_free_bytes`: 2284 B free / 3072 B allocated (margin: 2284 B, 74.3% headroom)

---

## 16. Health Manager Module (Interpretation & Central Diagnostic Authority)

### 16.1 Architectural Role
The Health Manager (`src/management/health_manager/`) interprets telemetry snapshots and subsystem operational states to classify system health into a unified state model. It performs no hardware register I/O and no automatic recovery.

### 16.2 Health State Model & Deterministic Lifecycle
- **Status Hierarchy:**
  1. `HEALTH_STATUS_FAULT`: System integrity threatened (e.g. SoC temp ≥ 85.0°C, Z906 serial comm broken, OTA rollback).
  2. `HEALTH_STATUS_DEGRADED`: Subsystem or metric threshold exceeded (e.g. SoC temp ≥ 75.0°C, sustained CPU ≥ 85%, heap free < 32 KB, metric staleness).
  3. `HEALTH_STATUS_UNKNOWN`: Baseline pending, component uninitialized, or first sample cycle not yet ingested. Deterministic aggregation: if no FAULT/DEGRADED and required baseline components are UNKNOWN, overall health is UNKNOWN.
  4. `HEALTH_STATUS_HEALTHY`: All evaluated dimensions nominal.
- **Memory Health Semantics:** Current memory health is decoupled from the historical watermark `heap_min_free_bytes`. A historical dip does not lock memory health in FAULT/DEGRADED forever; current health evaluates `heap_free_bytes` (< 32 KB warn / < 16 KB crit), `heap_usage_percent`, and largest block fragmentation (< 8 KB).
- **Continuous Monitoring & Autonomous Staleness:**
  - Trigger 1: `health_manager_process_metrics(&snap)` callback on every sample pass.
  - Trigger 2: 2000 ms periodic `esp_timer` (`hm_stale`). If metric producer goes silent for > 5000 ms, system automatically transitions to `HEALTH_STATUS_DEGRADED` (`HEALTH_REASON_METRIC_STALE`).
  - Trigger 3: Automatic recovery when fresh metrics resume.
- **Change-Only Diagnostic Logging:** Emits transition log events only when overall status, primary component, or primary reason changes (zero log spam on steady states).

### 16.3 Selective Diagnostic Exposure to Zigbee (Endpoint 4 / Cluster 0xFC03)
To enable Home Assistant / Zigbee2MQTT diagnostics without congesting IEEE 802.15.4 bandwidth, four high-value diagnostic attributes are exposed on manufacturer cluster `0xFC03`:

| Attr ID | Name | Type | Scaling / Representation | Reporting Policy |
|:---:|---|:---:|---|---|
| `0x0009` | `device_health` | `UINT8` | `0=unknown`, `1=healthy`, `2=degraded`, `3=fault` | Changed-only (event-driven) |
| `0x000A` | `health_primary_component` | `UINT8` | `0=system`, `1=system_metrics`, `2=soc_temp`, `3=cpu`, `4=memory`, `5=wdt`... | Changed-only (event-driven) |
| `0x000B` | `health_primary_reason` | `UINT8` | `0=none`, `1=not_init`, `2=metric_not_ready`, `4=soc_temp_warn`, `9=heap_warn`... | Changed-only (event-driven) |
| `0x000C` | `soc_temperature` | `INT16` | `0.01 °C` signed (`0x8000` = invalid sensor sentinel) | `dt >= 60s && delta >= 1.0°C`, heartbeat `900s`, crit bypass |

- **Zero Flood Guarantee:** Detailed telemetry (`cpu_load`, heap counters, uptime) remains strictly local on CLI.
- **Sentinel Suppression:** When `soc_temperature` reading is invalid, `0x8000` (`-32768`) is suppressed by the external converter; no fake 0°C readings are shown.
- **Architectural Decoupling:** `health_manager.c` contains zero Zigbee SDK includes; notifications route via neutral observer callback registration (`health_manager_register_change_observer`).

