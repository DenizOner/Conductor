# AGENTS.md — AI Coding Agent Guidelines & Living Documentation Protocol for "Conductor - Smart Zigbee Console for Logitech z906".

## Configurable Preferences & Language Settings
The user can customize the language and behavior preferences by modifying the values below:

- **Coding & Implementation Language:** `English`
- **Docstrings, Comments & In-line Notes Language:** `English`
- **Implementation Plan Language:** `Turkish`
- **Task List Language:** `Turkish`
- **Walkthrough Artifacts Language:** `Turkish`
- **Agent-to-User Communication Language:** `Turkish`

*Note for AI Agents:* Always inspect the configurable preferences above before generating source code, comments, planning artifacts (`implementation_plan.md`), task lists, or walkthrough summaries (`walkthrough.md`).

---

## 1. Core Directives & Working Principles

### 1.1 Primary Directive
**Write code so clearly that the most tired, hurried, and lazy person who reads it can immediately understand it — because that person will most likely be your future self.**

Every AI coding agent and human contributor operating within this repository must strictly adhere to this fundamental directive across all design, code, and documentation activities.

### 1.2 Mandatory Operational Rules
Every agent must observe the following fifteen non-negotiable rules throughout the entire lifecycle of the project:

1. **Inspect Before Acting:** Always examine the existing directory structure, source code, and configuration files before modifying or generating code and documentation.
2. **Zero Assumptions Policy:** Never make assumptions about the target hardware, framework, libraries, architecture, pinouts, or design intent. Every technical detail must be verified against actual workspace files, explicit user instructions, or confirmed telemetry.
3. **No Unverified Facts:** Never present unverified technical claims, hypothetical components, uncreated files, or planned features as existing facts.
4. **Preserve User Decisions:** Do not alter established architectural decisions (ESP32-C6, ESP-IDF framework, native Zigbee 3.0 stack, nested HAL + FSM + ServiceLocator architecture, 8 MB physical flash) without explicit user authorization.
5. **Strict Relative Paths:** All file paths, references, code snippets, documentation links, and commands must use relative paths originating from the project root (e.g., `src/main.c`, `platformio.ini`, `src/hardware/uart/uart_hal.h`, `test/core/test_service_locator/test_service_locator.c`, `tools/logitech_z906_converter.js`). Absolute paths are strictly prohibited.
6. **No Scope Creep:** Never introduce unsolicited features, speculative abstractions, or unused dependencies that the user did not request.
7. **Impact Assessment:** Thoroughly evaluate the scope, hardware impact, memory consumption, timing implications, and dependency chain before implementing changes.
8. **Documentation & Test Sync:** Keep `CODEBASE.md`, relevant test suites, and in-line code comments synchronized with every code modification. A task is incomplete until documentation is updated.
9. **Zero Falsification:** Never report tasks, test runs, or verification checks as successful if they were not genuinely executed and validated.
10. **Hardware State Integrity:** Never use optimistic, speculative, or assumed states for hardware status or Zigbee attribute reporting. The system must always rely on real, confirmed physical hardware telemetry.
11. **Token Efficiency & Semi-Reactive Execution Protocol:** When running builds, compilation jobs, test suites, flash uploads, or long-running background tasks, AI agents must never repeatedly poll status or query task progress in rapid loops (wasting AI tokens), nor rely on purely unmonitored reactive waiting (which risks indefinite deadlocks if a task hangs, crashes silently, or prompts for interactive input). Instead, agents must enforce a **Semi-Reactive Strategy**:
    - Whenever an asynchronous task is launched, schedule a watchdog timer of at least 360 seconds (`DurationSeconds >= 360`) with early termination linked to the task ID (`TimerCondition=<task-id>`), and wait reactively.
    - If the task finishes early, the reactive completion notification resumes execution immediately.
    - If the 360s timer expires, the agent wakes up, inspects task health/logs via `manage_task(action='status')`. If progressing normally, it schedules another >= 360s timer and returns to reactive waiting; if blocked or hung, it intervenes immediately.
12. **No Code Truncation:** Never use placeholder comments such as `// Remaining code here...`, `// ... existing code ...`, or `/* TODO: preserve old logic */` inside generated code blocks. Every modified or newly introduced function and file must be written in its entirety and without omissions.
13. **Single Task Per Response:** If a requested change spans multiple files or involves complex logic, present a clear implementation plan first. Never attempt to compress everything into a single response at the cost of producing incomplete code.
14. **Strict Type Safety:** The `any` type (or its equivalent in any language) is strictly forbidden. All function arguments, return types, and API response shapes must be explicitly and fully typed using the language's native type system (e.g., C structs/enums, TypeScript interfaces/types). Before writing code, always read the relevant type definition files (`types.ts`, structs in `*.h` headers, etc.) to avoid assumption-driven implementations.
15. **Change Summary & Critical Approval Gate:** Before writing a large code block, briefly summarize in 2–3 bullet points what is being changed, why, and how. When a decision could impact performance, data integrity, or hardware safety, halt code generation and request explicit user approval before proceeding.

---

## 2. Living Documentation Protocol & Continuous Synchronization

### 2.1 The Living Documentation Principle
`AGENTS.md` and `CODEBASE.md` are living documents that must stay perpetually synchronized with the actual codebase. They are not static artifacts written once at project inception.

**Non-Negotiable Rule:** If a change made to the project necessitates a documentation update, the task **MUST NOT** be reported as complete until all relevant documentation is fully updated, verified, and reported to the user.

### 2.2 Documentation Responsibilities & Separation of Concerns
`AGENTS.md` and `CODEBASE.md` complement each other but serve distinct purposes:

- **`AGENTS.md` (Governing Rules & Developer Directives):**
  - Defines how AI agents and developers must operate.
  - Development constraints, coding standards, architecture guidelines (e.g., SOLID principles).
  - Autonomous engineering, CLI architecture, diagnostics, and developer tools protocol.
  - Command parity rules and External Converter synchronization protocols.
  - Central Command Registry governance and Command Verifier preservation rules.
  - Canonical Z906 serial protocol governance and reverse-engineering reference preservation.
  - Debug Console, Watchdog, System Metrics, and Health Manager governance protocols.
  - Memory budgeting, hardware rules, FreeRTOS task design, and testing protocols.
  - AI token efficiency and semi-reactive asynchronous execution protocols.
  - Living documentation maintenance procedures.

- **`CODEBASE.md` (Technical Reality & System Specification):**
  - The single source of truth for the project's current, verified technical state.
  - Verified hardware specifications, development board, and flash memory configuration.
  - PlatformIO and ESP-IDF settings.
  - Exact directory tree matching the real filesystem.
  - Implemented software architecture, module responsibilities, and inter-component dependencies.
  - Canonical protocol definitions, LRC checksum algorithm, and status frame structures.
  - Command Parity matrix, GPIO assignments, and serial communication parameters (`57600 8O1`).
  - FreeRTOS task matrix and application lifecycle.
  - Verified unit test suites and known limitations / technical debt.

---

## 3. SOLID Principles in Embedded C & ESP-IDF

All new and refactored code must adhere to the SOLID engineering principles adapted to embedded C and the ESP-IDF framework:

- **Single Responsibility Principle (SRP):** Each module, file, and function must have exactly one reason to change. Functions must remain focused and concise (ideally 25 to 50 lines of code).
- **Open/Closed Principle (OCP):** Modules should be open for extension but closed for modification via table-driven command handlers and EventBus events.
- **Liskov Substitution Principle (LSP):** Abstract drivers (e.g., UART HAL, ADC HAL) must honor identical return codes and non-blocking contracts.
- **Interface Segregation Principle (ISP):** Avoid omnibus header files. Keep header files granular and focused (`uart_hal.h`, `adc_hal.h`, `z906_codec.h`, `z906_cmd_registry.h`, `z906_cmd_verifier.h`, `debug_console.h`, `wdt_manager.h`, `system_metrics.h`, `health_manager.h`, `zb_handlers.h`).
- **Dependency Inversion Principle (DIP):** High-level modules (FSM, Zigbee application, CLI, Command Verifier) depend on abstractions managed via the centralized `ServiceLocator` and communicate through asynchronous events on the `EventBus`.

---

## 4. Code Organization & Directory Structure

Source code must be strictly organized into feature-isolated nested directories adhering to PlatformIO and GCC conventions:

- `platformio.ini`: PlatformIO configuration targeting `esp32-c6-devkitc-1` with `framework = espidf`.
- `CMakeLists.txt`: Root CMake project definition.
- `sdkconfig.defaults`: Default ESP-IDF configuration (USB-JTAG console, 8MB flash).
- `README.md`: Primary project documentation, hardware specs, and quickstart guide.
- `include/`:
  - `README`: ESP-IDF / PlatformIO guidelines for shared headers.
  - `config.h`: Public configuration header reserved for global constants, pin definitions, and timing parameters.
- `lib/`:
  - `README`: PlatformIO private library directory documentation.
- `src/`:
  - `CMakeLists.txt`: ESP-IDF component registration discovering nested include paths and sources.
  - `main.c`: Thin application entry point (`app_main`) — delegates entirely to `system_initializer_run()`.
  - `system/`:
    - `initializer/`: Deterministic boot dependency graph, lifecycle phasing, failure policy, and FreeRTOS task spawning authority (`system_initializer.h`, `system_initializer.c`).
  - `core/`:
    - `cmd_verifier/`: Centralized state-based command verification engine & command registry (`z906_cmd_registry.h`, `z906_cmd_registry.c`, `z906_cmd_verifier.h`, `z906_cmd_verifier.c`).
    - `event_bus/`: Thread-safe FreeRTOS publish/subscribe event engine.
    - `service_locator/`: Central dependency registry for system services.
    - `state_machine/`: Z906 FSM state transition rules.
    - `state_manager/`: Verified physical state store and telemetry cache.
  - `hardware/`:
    - `uart/`: Pure low-level UART HAL (Console & Amp) — no task ownership, no EventBus dependency.
    - `adc/`: Calibrated ADC1 One-Shot driver for pin sensing.
    - `gpio/`: GPIO configuration and bootline stabilization.
  - `protocol/`:
    - `codec/`: Frame serialization, multi-byte parsing, strict LRC checksum validation.
    - `commands/`: Protocol opcodes, command tables, decode states, volume scale converters.
  - `communication/`:
    - `z906_transport/`: UART-to-EventBus adapter — owns `uart_rx_task` with private semaphore startup handshake (`z906_transport.h`, `z906_transport.c`).
    - `zigbee/`: ESP-Zigbee-SDK integration, ZCL clusters, and ZDO handlers.
      - `clusters/`: Standard and custom ZCL cluster definitions (`zcl_clusters.h`, `zcl_clusters.c`).
      - `handlers/`: Attribute read/write and network event handlers (`zb_handlers.h`, `zb_handlers.c`).
      - `zigbee_app.h`, `zigbee_app.c`: Zigbee pairing FSM, non-blocking timers, and state reporting.
    - `cli/`: Interactive command line over USB Serial/JTAG, bifurcated into User Commands (main menu) and Developer Tools (`cli_console.[ch]`, `cli_dev_tools.[ch]`).
  - `management/`:
    - `debug_console/`: Centralized Debug Console service and static tag filters (`debug_console.h`, `debug_console.c`).
    - `system_metrics/`: ESP32-C6 internal metrics capture (`system_metrics.h`, `system_metrics.c`).
    - `health_manager/`: System health evaluation & snapshot engine (`health_policy.h`, `health_manager.h`, `health_manager.c`).
    - `led_notification/`: Modular WS2812 LED notification system and priority engine.
    - `nvs/`: Non-Volatile Storage manager for persistent Zigbee credentials (`zb_store` namespace).
    - `watchdog/`: Task Watchdog Timer (TWDT) manager and reset diagnostics (`wdt_manager.h`, `wdt_manager.c`).
    - `telemetry/`: Hardware line voltage monitor and stack watermarking.
- `test/`:
  - `README`: PlatformIO test runner documentation.
  - `communication/`:
    - `test_zigbee/`: Unit tests for Zigbee pairing management, NVS retention, reboot rejoining, and ZCL command parity.
  - `core/`:
    - `test_cmd_verifier/`: Unit tests for command verification engine and central registry integration.
    - `test_service_locator/`: Unit tests for ServiceLocator.
    - `test_state_manager/`: Unit tests for Z906 verified state manager and telemetry cache.
  - `management/`:
    - `test_debug_console/`: Unit tests for Debug Console log filtering and Watchdog diagnostic inspection.
    - `test_system_metrics/`: Unit tests for internal metrics snapshot, task timing, and test hooks.
    - `test_health_manager/`: Unit tests for health policies, hysteresis, debounce, and component ranking.
    - `test_led_notification/`: Unit tests for LED animations, priority engine, and OFF invariants.
    - `test_ota/`: Unit tests for Zigbee OTA upgrade client and rollback safety.
  - `protocol/`:
    - `test_codec/`: Unit tests for framing, byte parsing, and literal reference LRC checksums.
    - `test_commands/`: Unit tests for opcode mappings, decode states, and volume scale conversions.
  - `system/`:
    - `test_initializer/`: Unit tests for boot sequence, state model, re-entry protection, and boot report validation (`test_system_initializer.c`).
    - `test_initializer_failure/`: Unit tests for fatal failure injections.
    - `test_initializer_degraded/`: Unit tests for degraded failure injections and gating.
- `tools/`:
  - `z906_commands_metadata.json`: Single Source of Truth JSON metadata for all Z906 commands.
  - `validate_command_parity.js`: Automated cross-layer parity verification script.
  - `conductor_converter.mjs`: ESM Zigbee2MQTT External Converter for Home Assistant.
  - `test_converter.js`: Automated Node.js unit tests for External Converter.

---

## 5. Central Command Registry & State-Based Verifier Protocol

To govern commands consistently and protect the state-based verification engine across all control interfaces, every AI agent and contributor must adhere to the following ten non-negotiable rules:

### 5.1 Ten Permanent Directives for Command Governance & Verification
1. **Central Command Definition:** All new Logitech Z906 control commands must be defined in the central metadata source (`tools/z906_commands_metadata.json`) and registered in firmware via `src/core/cmd_verifier/z906_cmd_registry.h` and `z906_cmd_registry.c`.
2. **Unified Execution Pathway:** Commands originating from CLI or Zigbee must strictly route through the same shared execution and verification engine (`z906_cmd_verifier_execute()`).
3. **Mandatory State Verification:** Every new command must be paired with an appropriate `verify_cmd_type_t` and target state comparison logic in the command verifier.
4. **Strict Separation of Concerns:** Command definition (metadata, syntax, parameter bounds, ZCL mapping in `z906_cmd_registry`) and state verification (pre/post physical telemetry, UART byte dispatch, mutex locking in `z906_cmd_verifier`) must remain separated.
5. **No Verification Bypass:** Creating alternative execution paths, direct UART register writes, or bypassing `z906_cmd_verifier_execute()` from CLI or Zigbee is strictly forbidden.
6. **Synchronous Schema Maintenance:** Whenever the central metadata schema changes, corresponding mappings in `z906_cmd_registry.[ch]`, `tools/conductor_converter.mjs`, and `tools/validate_command_parity.js` must be updated simultaneously.
7. **No Optimistic Assumptions:** The transmission of a serial byte or Zigbee ACK must never be interpreted as verified physical execution. Only confirmed telemetry feedback constitutes success.
8. **Unverifiable Commands Handling:** Commands lacking physical feedback channels on the Logitech Z906 hardware must be marked as `VERIFY_CMD_UNVERIFIABLE` and must never be reported as verified success (`CMD_VERIFY_SUCCESS`).
9. **Lockstep Test Parity:** Whenever commands are added or updated, test suites across CLI, Zigbee, External Converter, and Command Verifier must be expanded to cover the new commands.
10. **Preservation of Existing Verifier Behavior:** The core behavior, mutex synchronization, timeout parameters, and external API of `z906_cmd_verifier` must not be altered without explicit user authorization.

### 5.2 Parameter Consistency, Scaling Standards & Canonical Input Naming
- Parameter names, ranges, and semantics must remain identical across CLI and Zigbee.
- Master Volume and Channel Trims use native discrete steps `0 - 43`.
- Conversions between hardware volume (`0 - 43`) and Zigbee ZCL levels (`0 - 254`) must strictly use the standard converter functions (`z906_cmd_volume_to_zcl` / `z906_cmd_zcl_to_volume` in C and `volumeToZcl` / `zclToVolume` in JavaScript).

#### Canonical Audio Input Identifiers
The six physical audio inputs of the Logitech Z906 must be referenced exclusively by their canonical connection-type identifiers. The `z906_input_t` enum uses the following authoritative names:

| Enum (C firmware)              | Input # | Converter String              | Connection Type                               |
|--------------------------------|:-------:|-------------------------------|-----------------------------------------------|
| `Z906_INPUT_SIX_CHANNEL_DIRECT`| 1       | `6-Channel Direct (Input 1)`  | Three 3.5mm TRS jacks — 6-channel analog      |
| `Z906_INPUT_RCA_STEREO`        | 2       | `RCA Stereo (Input 2)`        | Red/white RCA connectors — stereo analog       |
| `Z906_INPUT_OPTICAL_1`         | 3       | `Optical 1 (Input 3)`         | First TOSLINK connector — digital optical      |
| `Z906_INPUT_OPTICAL_2`         | 4       | `Optical 2 (Input 4)`         | Second TOSLINK connector — digital optical     |
| `Z906_INPUT_DIGITAL_COAXIAL`   | 5       | `Digital Coaxial (Input 5)`   | 75-ohm S/PDIF coaxial connector               |
| `Z906_INPUT_AUXILIARY`         | 6       | `Auxiliary / AUX (Input 6)`   | 3.5mm TRS on console side panel — AUX         |

**Non-Negotiable Rules for Input Naming:**
1. **Always use canonical enum names** in C firmware: `Z906_INPUT_SIX_CHANNEL_DIRECT`, `Z906_INPUT_RCA_STEREO`, `Z906_INPUT_OPTICAL_1`, `Z906_INPUT_OPTICAL_2`, `Z906_INPUT_DIGITAL_COAXIAL`, `Z906_INPUT_AUXILIARY`. The generic aliases `Z906_INPUT_1`..`Z906_INPUT_6` **do not exist** and must not be used.
2. **Always use canonical string keys** in the JavaScript converter: `'6-Channel Direct (Input 1)'`, `'RCA Stereo (Input 2)'`, `'Optical 1 (Input 3)'`, `'Optical 2 (Input 4)'`, `'Digital Coaxial (Input 5)'`, `'Auxiliary / AUX (Input 6)'`. Legacy strings `'Input 1 (Direct)'` etc. **do not exist** as accepted values.
3. **No backward-compatibility shims:** This codebase does not maintain deprecated aliases, legacy string mappings, or transitional `#define` bridges for input identifiers. When renaming, update all usages completely and break cleanly.
4. **Integer values are protocol-stable:** The numeric values `1`–6 assigned to each input must never be changed. They map directly to the CLI parameter, ZCL attribute, and codec decode output.
5. **Effect compatibility is input-type-dependent:** Analog inputs (6-Channel Direct, RCA Stereo, Auxiliary/AUX) support all spatial effects. Digital inputs (Optical 1, Optical 2, Digital Coaxial) force effect to `Z906_EFFECT_NONE` via `z906_cmd_validate_effect()`.

---

## 6. Debug Console & Watchdog Integration Protocol

To ensure reliable, non-blocking diagnostics without compromising system watchdog integrity, all AI agents must enforce the following six mandatory directives:

1. **Leverage Native ESP-IDF Logging:** Do not create parallel, redundant logging engines or custom formatted print buffers. Always use ESP-IDF's native `esp_log.h` and `esp_log_level_set()`.
2. **Preserve Watchdog Protection:** Debug Console operations must never disable the Task Watchdog Timer, inflate timeouts arbitrarily, or suppress panic triggers.
3. **No Fraudulent Watchdog Petting:** Debug Console, CLI, or other services must **NEVER** pet/reset the Task Watchdog on behalf of another task. Each monitored task (`uart_rx_task`, `z906_fsm_task`, `zigbee_task`, `telemetry_task`, `sys_metrics`) must autonomously feed the watchdog to prove it is not deadlocked or starved.
4. **Non-Blocking Diagnostic Output:** Logging and status printing must remain strictly non-blocking. Diagnostic operations must never stall tasks monitored by the TWDT, which would otherwise trigger artificial watchdog panics.
5. **Truthful Reset Reason Reporting:** Always query confirmed hardware reset causes via ESP-IDF `esp_reset_reason()`. Never hypothesize or fabricate reset reasons.
6. **Zero Runtime Heap Allocation:** All Debug Console structures, tag filter tables, and mutex synchronization primitives must use static memory allocation (`StaticSemaphore_t`, static arrays).

---

## 7. Embedded Systems Engineering Rules

### 7.1 Memory Safety & Static Resource Allocation
- **Zero Dynamic Allocation at Runtime:** Calls to dynamic heap allocation (`malloc()`, `calloc()`, `new`, `free()`) are strictly forbidden during runtime operation. Static allocation is required for all tasks, queues, ring buffers, and contexts.
- **Stack Budgeting & Watermarking:** Every FreeRTOS task must have a strictly calculated stack depth. Continuously monitor stack watermarks (`uxTaskGetStackHighWaterMark()`) during testing to prevent overflows.
- **Buffer Boundary Safety:** Always enforce bounds checking on serial packet buffers and Zigbee payload arrays. Never use unbounded string functions (`strcpy`, `sprintf`); strictly enforce bounded alternatives (`strncpy`, `snprintf`).

### 7.2 Flash Memory & Partition Boundary Safeguards
- **Physical Flash Capacity:** The hardware ESP32-C6 board features 8 MB (8,388,608 bytes) physical SPI NOR flash.
- **Partition Verification:** Any changes affecting firmware binaries, partition tables, NVS structures, or OTA slots must verify physical capacity, active partition table offsets, and application partition limits against `CODEBASE.md`.
- **Application Slot Allocation:** The active partition table (`partitions.csv`) configures dual symmetrical OTA application slots (`ota_0` and `ota_1`) of 1,792 KB (`0x1C0000`, 1,835,008 bytes) each. Firmware binary size must remain strictly within this 1,792 KB slot ceiling.

### 7.3 Concurrency, Timing & FreeRTOS Task Design
- **Non-Blocking Architecture:** Never use busy-waits or blocking delays inside event processors or communication handlers. Use FreeRTOS queues, event groups, and notifications.
- **Task Prioritization Matrix:**
  1. `uart_rx_task` (Priority 5, High): Owned by `z906_transport.c` — drains UART FIFOs into EventBus. Pure data forwarding, no protocol knowledge.
  2. `z906_fsm_task` (Priority 4, Med-High): Evaluates EventBus queue, executes state transitions, notifies Zigbee publisher via observer pattern.
  3. `zigbee_task` (Priority 3, Medium): Handles Zigbee 3.0 stack events, pairing timers, and attribute reporting. Init confirmed before STARTED.
  4. `cli_task` (Priority 2, Med-Low): Processes interactive commands over USB Serial/JTAG.
  5. `telemetry_task` (Priority 1, Low): Periodic ADC monitoring, TWDT watchdog service.
  6. `sys_metrics` (Priority 1, Low): Periodic ESP32-C6 internal metrics snapshot (CPU load delta, heap, die temp).
  7. `led_task` (Priority 1, Low): Event-driven LED animation player (DEGRADED if init failed).
- **Startup Handshake Contract:** Every `_start()` API uses a private `StaticSemaphore_t` — it returns only after the task signals STARTED. No `EventGroupHandle_t` is passed between modules. `zigbee_native_stack_start()` additionally gives the semaphore only after `zigbee_native_stack_init()` succeeds.

### 7.4 Robustness & Fault Tolerance
- **Hardware Telemetry vs. Optimistic Assumptions:** Always query, read, and verify actual physical pins, return packets, or ACK frames before updating internal or reported states.
- **Hardware Watchdog Timer (TWDT):** Register all primary FreeRTOS tasks with the ESP-IDF Task Watchdog Timer. Any hung task must trigger a controlled system restart.

---

## 8. Verification, Testing & Static Analysis

### 8.1 PlatformIO & Host Verification Commands
Execute from project root using relative paths:

- **Target environment build:**
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
- **Execute unit tests (with physical upload to board):**
  ```sh
  pio test -e esp32-c6-devkitc-1
  ```
- **Build unit tests (verification without physical hardware upload):**
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

## 9. AI Token Efficiency & Semi-Reactive Long-Running Operations Protocol

To eliminate wasteful token consumption during lengthy build, compilation, test, and upload operations while preventing indefinite hangs or silent deadlocks, all AI agents must strictly observe the **Semi-Reactive Execution Strategy**:

### 9.1 The Deadlock & Token Waste Dilemma
1. **The Polling Pitfall:** Constantly polling task status or reading terminal output every few seconds consumes thousands of unnecessary AI tokens.
2. **The Purely Reactive Pitfall:** Relying solely on asynchronous completion events without time boundaries causes permanent agent paralysis if a process deadlocks, freezes, crashes silently, or halts awaiting interactive console input.

### 9.2 The Semi-Reactive Watchdog Protocol
AI agents must manage asynchronous background tasks through the following standard operational sequence:

1. **Launch Asynchronously:**
   Launch the command using `run_command` with an appropriate synchronous buffer (e.g., `WaitMsBeforeAsync: 10000`). If the command completes quickly within this buffer, proceed immediately.
2. **Arm Periodic Watchdog Timer:**
   If the command is sent to the background as a task (`task-id`), immediately call the `schedule` tool to arm a one-shot watchdog timer of **no less than 360 seconds (6 minutes)**:
   ```json
   {
     "DurationSeconds": "360",
     "TimerCondition": "<task-id>",
     "Prompt": "Watchdog timer expired: inspect health and status of background task <task-id>"
   }
   ```
3. **Await Notification Reactively:**
   Stop calling tools and yield turn. The system will resume the agent in one of two situations:
   - **Case A — Early Task Completion:** The background task completes before 360 seconds. The system delivers the task exit status and output, automatically canceling the timer. Proceed immediately.
   - **Case B — Watchdog Expiration:** 360 seconds elapse without completion. The timer fires and wakes the agent.
4. **Periodic Health Inspection Upon Expiry:**
   Upon waking from the watchdog timer:
   - Check process state via `manage_task(Action='status', TaskId='<task-id>')`.
   - Inspect recent log lines to determine if the task is actively progressing or stuck (e.g., waiting for user input, error prompt, or hung).
   - If the task is blocked, in error, or prompting for input: resolve it, abort it (`manage_task(Action='kill')`), or report the issue to the user immediately.
   - If the task is actively progressing normally: schedule a new watchdog timer of at least 360 seconds (`DurationSeconds >= 360`, `TimerCondition=<task-id>`) and yield turn to resume reactive waiting.
5. **Zero Token Waste & Zero Hang Guarantee:**
   This semi-reactive architecture ensures zero token consumption during quiet execution windows while strictly bounding maximum undetected deadlock latency to 5 minutes.

---

## 10. Canonical Logitech Z906 Protocol Governance Protocol

To preserve protocol integrity and maintain strict compliance with the foundational reverse-engineering documentation (`nomis/logitech-z906`), every AI agent and contributor must adhere to the following seven non-negotiable protocol rules:

1. **Canonical Reference Documentation as Single Source of Truth:**
   The 4 foundational documents (`README.rst`, `firmware.rst`, `interface.rst`, `protocol.rst`) are the authoritative technical reference for the Z906 serial protocol. AI agents must never modify protocol opcodes, framing structures, packet schemas, or timing rules contrary to these specifications, nor demand repeated physical reverse-engineering for established documented facts.
2. **Strict Serial Interface Configuration (57600 8O1):**
   The serial link between the console/ESP32-C6 and the subwoofer/amplifier operates strictly at `57600 baud, 8 data bits, Odd Parity, 1 stop bit`. All UART driver configurations (`uart_hal.c`) and documentation must strictly reflect Odd Parity.
3. **Mandatory Strict Longitudinal Redundancy Check (LRC):**
   Every transmitted and received packet must strictly validate the Longitudinal Redundancy Check:
   `(packet_type + length + sum(payload) + lrc) & 0xFF == 0`.
   Malformed frames with invalid checksums must be rejected without altering device state. Bypassing or disabling LRC checks is strictly prohibited.
4. **Canonical Power Sequencing & Physical Telemetry Grounding:**
   - Power ON must execute the canonical multi-stage sequence: `11 11 <effect> 39 38 30 39` followed by `0x34` status poll.
   - Power OFF must execute the canonical sequence: `30 37 36` (`0x36` Standby Save) followed by `0x34` status poll.
   - Power state must NEVER be optimistically assumed. Power state is authoritatively determined by Byte 17 (`standby`: `0 = Active / ON`, `1 = Standby`) of the 20-byte `AA 0A 14` amplifier status frame.
5. **Canonical Mute Sequencing:**
   Muting and unmuting operations must follow canonical bracket sequences (`0x38` - `0x39`) and volume ramping to protect amplifier circuitry and prevent audio pops.
6. **Telemetry & Sensor Grounding:**
   - Temperature telemetry (`0x25` query) decodes Type `0x0C` (5 bytes: heatsink, DSP, PSU temperatures in °C).
   - Idle telemetry (`0x31` query) decodes Type `0x0F` (3 bytes: elapsed idle seconds).
   - Digital stream decoding status is tracked via Byte 12 of `AA 0A 14` (`0x00` Not streaming, `0x01` Streaming, `0x0C` Unknown, `0x0E` DTS / Bitstream) and decode states (`0x17` - `0x1F`).
7. **Literal Test Fixture Standards:**
   Protocol unit tests must use independent literal test vectors directly extracted from `protocol.rst` rather than circular tautological assertions comparing internal enums against themselves.

---

## 11. Zigbee2MQTT External Converter Development Directives

To ensure the `tools/conductor_converter.mjs` External Converter remains correct, secure, and aligned with current Zigbee2MQTT API standards, every AI agent and contributor must adhere to the following nine non-negotiable directives:

### 11.1 Architecture — Why Not modernExtend
1. **`modernExtend` is explicitly excluded** from this project's converter. The Logitech Z906 uses three manufacturer-specific ZCL clusters (`0xFC01`, `0xFC02`, `0xFC03`) that have no equivalent modules in `zigbee-herdsman-converters/lib/modernExtend`. Any attempt to replace the existing `fromZigbee`/`toZigbee` handlers with `modernExtend` will silently drop all custom cluster support (trims, input, telemetry).
2. **ESM format (`.mjs`)** is the canonical standard for Zigbee2MQTT v2.x external converters (`conductor_converter.mjs`). Imports use explicit `.js` extension (`lib/exposes.js`, `lib/ota.js`) required by ZHC 21.x.
3. **Deployment path (Z2M v2.x+):** The converter file must be placed at `<z2m-data-dir>/external_converters/conductor_converter.mjs`. Set `advanced: enable_external_js: true` in `configuration.yaml`.
4. **Security gate:** `advanced: enable_external_js: true` must be set in `configuration.yaml` for Z2M v2.11.0+. AI agents must document this requirement in any deployment guide they produce but must never modify the user's `configuration.yaml` directly.

### 11.2 No-Optimistic-State Policy (Non-Negotiable)
5. **All `convertSet` handlers must return `null`** — never `{ state: { ... } }`. Returning an optimistic state causes Home Assistant to display the intended state before the firmware has physically verified it. The firmware's `z906_cmd_verifier_execute()` is the sole authority for state confirmation. HA updates only after receiving a real ZCL attribute report.
6. **Never infer success from ZCL dispatch.** The transmission of a `genOnOff on/off` command or a ZCL cluster write is not evidence of physical success. Only confirmed telemetry feedback (Zigbee attribute report published by firmware post-verification) constitutes success.

### 11.3 Strict Input Validation
7. **`z906_input` canonical-only validation:** The `z906_input.convertSet` handler must accept only canonical connection-type string keys from `INPUT_MAP_TO_NUM`. Numeric integer fallback (e.g., accepting `'3'` as `Optical 1`) is permanently prohibited. This mirrors the no-backward-compatibility policy for input identifiers documented in `AGENTS.md` Section 5.2.

### 11.4 Resilience & Security
8. **All `fromZigbee` handlers must be wrapped in `try/catch`.** A single malformed Zigbee message from a misbehaving coordinator or corrupted OTA frame must never crash the Zigbee2MQTT Node.js process. Every `convert()` function must return `{}` on any parse error.
9. **No new runtime dependencies.** The converter file must depend only on `zigbee-herdsman-converters` modules. No `require()` or `import` of external npm packages, filesystem modules (`fs`, `path`), or network modules is permitted.

### 11.5 Test Obligations
Whenever the converter is modified, all of the following must pass before the task is reported complete:
```sh
node tools/test_converter.js           # All test suites
node tools/validate_command_parity.js  # 8/8 command parity
```
If new `toZigbee` handlers are added, a corresponding null-return test must be added to `test_converter.js`.

---

## 12. Living Documentation Pre-Completion Checklist

Before reporting any development task as complete, every AI agent must execute this mandatory verification checklist:

- [x] **Filesystem Audit:** Did I create, rename, or delete any files? If so, is the `CODEBASE.md` file tree updated to match reality exactly?
- [x] **Relative Paths:** Are all file paths, links, and command examples strictly relative to the project root?
- [x] **Zero Assumptions:** Are all claims in `CODEBASE.md` backed by verified files, hardware configs, or executed tests?
- [x] **Command Parity Verified:** Are all Z906 control commands synchronized across CLI, Zigbee handlers, and External Converter?
- [x] **Canonical Input Names:** Are all audio input references using the canonical enum names (`Z906_INPUT_SIX_CHANNEL_DIRECT` etc.) and canonical string keys (`'6-Channel Direct (Input 1)'` etc.)? No generic aliases (`Z906_INPUT_1..6`) or legacy strings (`'Input 1 (Direct)'`) exist in the codebase.
- [x] **Protocol Compliance Verified:** Are all opcode definitions, LRC checksum validations, and serial configurations strictly conforming to `nomis/logitech-z906` specifications?
- [x] **Semi-Reactive Protocol Compliance:** Were long-running commands managed using the semi-reactive watchdog protocol (>= 360s timer) to avoid polling loops and deadlocks?
- [x] **Converter Compliance:** Does `conductor_converter.mjs` follow the directives in Section 11? (null returns, strict validation, try/catch, no optimistic state, no new dependencies)
- [x] **Temperature Integrity Compliance:** Is `temp_valid` set to `false` at boot and only promoted to `true` after a verified LRC-checked temperature response? Are temperatures never reported to Home Assistant while `temp_valid = false`?
- [x] **Mutual Consistency:** Are `AGENTS.md` and `CODEBASE.md` completely consistent with each other without contradictions?
- [x] **User Reporting:** Did I summarize all documentation updates in the final report to the user?

---

## 13. Temperature Telemetry Integrity Directives

To prevent communication errors from being misrepresented as valid physical temperature measurements, every AI agent and contributor must adhere to the following nine non-negotiable directives:

1. **Valid measurement and communication error are always distinct representations.**
   `0 °C` must never be used as an error sentinel. A valid `0 °C` reading (produced from a LRC-verified packet) and a communication failure (no response, timeout, checksum mismatch) must be stored and reported through separate mechanisms. The `z906_state_t.temp_valid` flag is the authoritative separator.

2. **`temp_valid` starts `false` at boot and transitions to `true` only after the first integrity-verified temperature response.**
   Any code that resets `z906_state_t` (e.g., `z906_state_init()`) must explicitly set `temp_valid = false`. A response from the amplifier that fails LRC checksum, has the wrong type byte, or has an insufficient payload length must never trigger a `temp_valid = true` transition.

3. **Temperatures are never reported to Zigbee or Home Assistant while `temp_valid = false`.**
   The `has_state_changed()` function in `zigbee_app.c` gates temperature delta detection on `temp_valid`. Any refactor of this function must preserve this gate. Boot-time `0 °C` values must never reach Home Assistant as if they were measured readings.

4. **Invalid or malformed protocol messages never produce temperature values.**
   The LRC checksum (`z906_codec_verify_lrc()`) is mandatory before any temperature data is extracted. A packet failing checksum, type check (`Z906_MB_AMP_TEMP = 0x0C`), or length check (`bytes_read >= 5`) must be discarded in full. No partial extraction of temperature bytes from a failed packet is permitted.

5. **All three temperature channels must be verified simultaneously.**
   Temperature 1, Temperature 2, and Temperature 3 are always updated atomically inside a single mutex lock in `z906_state_set_temperature()`. No code must update individual channels independently. A packet that passes full integrity checks updates all three channels together or none.

6. **Temperature channel indices must match the verified protocol layout.**
   Per `CODEBASE.md` Section 11.2: `temp_celsius[0]` ← `payload[1]`, `temp_celsius[1]` ← `payload[2]`, `temp_celsius[2]` ← `payload[3]`. `payload[0]` and `payload[4]` are padding bytes. This mapping must not be changed without re-verifying against the `protocol.rst` canonical reference.

7. **Failed or skipped telemetry queries must be logged at `LOGW` level.**
   When `telemetry_dispatch_query(Z906_CMD_READ_TEMP)` returns `false`, a `ESP_LOGW` message must be emitted. Silent failures are prohibited because they hide communication problems that cause `temp_valid` to remain `false` indefinitely.

8. **Stale temperatures from a previous successful query must not be re-published as new measurements.**
   If a subsequent temperature query fails, the cached values in `z906_state_t.temp_celsius[]` are preserved unchanged and `temp_valid` remains `true`. However, they must not trigger a new Zigbee attribute report (delta detection returns `false` for unchanged values). Never mark stale data as freshly measured.

9. **New temperature-related features require tests covering both Test A and Test B:**
   - **Test A — Valid 0 °C:** A LRC-verified packet with `0x00` in temperature bytes must be accepted as a valid measurement. `temp_valid` must become `true`. The reading must reach Home Assistant.
   - **Test B — Communication Error:** A failed, timed-out, or malformed query must not update `temp_celsius[]` and must not trigger a Zigbee report. `temp_valid` must not transition from `false` to `true` due to a failed query.

---

## 14. Autonomous Development, CLI Architecture, Diagnostics and Developer Tools

To empower AI agents and developers to operate autonomously as senior embedded systems engineers while maintaining pristine user interfaces and system reliability, all agents must adhere to the following directives governing CLI design, observability instrumentation, and diagnostic workflows:

### 14.1 Core Design Principle & CLI Separation of Concerns
The fundamental principle governing all user interfaces and CLI systems in this project is:
> **The commands required by an end-user to operate the product's primary functions must be strictly and cleanly decoupled from development, instrumentation, diagnostic, and debugging tools.**

Commands are bifurcated into two logical tiers:
1. **User Commands (`Main Menu`):** Essential end-user product functionality directly reflecting the primary operational purpose of the device.
   - In this project: Main Power (`power`), Master Volume (`volume`), Speaker Trims (`trim`), Audio Input (`input`), Audio Processing Effect (`effect`), Mute (`mute`), Confirmed Status Telemetry (`status`), Overall System Health (`health`), System Metrics (`metrics`), and Developer Mode toggle (`devmode`).
2. **Developer Tools (`Developer Tools`):** Internal diagnostics, instrumentation, telemetry probes, runtime state dumps, logging controls, network debugging, and low-level subsystem tests.
   - Normal users must never be exposed to developer tools in the default CLI view.

### 14.2 Developer Mode Operational Lifecycle
1. **Default State:** `Developer Mode = OFF` upon boot and reset.
2. **Non-Persistent by Default:** Developer Mode is session-scoped (volatile RAM). It automatically resets to `OFF` across system reboots or watchdogs to prevent accidental exposure of privileged or disruptive tools.
3. **CLI Presentation:**
   - When `OFF`: The root CLI `help` and command dispatcher present only User Commands and the command to toggle Developer Mode (`devmode on`). Low-level diagnostic commands are hidden.
   - When `ON`: The root CLI exposes `Developer Tools` submenus or namespaced command trees, enabling full developer inspection capabilities.
4. **Purpose:** Complexity management and accident prevention. It is not an enterprise security sandbox, but an engineering interface barrier protecting normal operational simplicity.

### 14.3 Developer Tools Hierarchy & Subsystem Alignment
Developer Tools must not degenerate into a flat, disorganized list of ad-hoc commands. They must follow a structured hierarchy reflecting the firmware architecture:

```text
Developer Tools
├── 1. Logging
│   ├── log_level [level]              # View or set global ESP-IDF log level
│   ├── log_filter [tag] [level|clear] # Per-module tag log level overrides
│   └── log_reset                      # Restore default log filter state
├── 2. Diagnostics (Runtime & Platform)
│   ├── debug_status                   # Uptime, free heap, min free heap, stack watermarks
│   ├── wdt_status                     # TWDT task health, configured timeouts, reset reason
│   ├── tasks                          # Task scheduler statistics, run times, and states
│   └── telemetry_status               # Raw ADC line voltage, cable sense, polling counters
├── 3. Protocols (Hardware & Wireless)
│   ├── uart_status                    # UART ring buffer depths, parity errors, drop counters
│   ├── z906_raw_query <cmd_hex>       # Low-level protocol query (e.g. 0x25 temp, 0x31 idle)
│   ├── zb_status                      # Zigbee PAN ID, channel, extended PAN, pairing state
│   ├── zb_pair [seconds]              # Trigger commissioning window
│   └── zb_leave                       # Leave Zigbee network and clear NVS credentials
├── 4. Testing & Verification
│   ├── self_test                      # Autonomous multi-subsystem integrity check
│   ├── verify_stats                   # Command verifier success/failure latency metrics
│   └── test_inject_event <evt_type>   # Dispatch simulated EventBus event (controlled test)
└── 5. Critical Controls (State-Altering / Destructive)
    └── zb_reset                       # Full factory reset: erase NVS data and reboot
```

### 14.4 Safe vs. State-Altering Developer Commands
- **Informational Commands (`show`, `status`, `dump`):** Read-only operations that query telemetry or metrics without altering device hardware or state.
- **State-Altering Commands (`zb_reset`, raw packet injections, overrides):** Commands that mutate persistent memory or force hardware states must enforce strict input validation, distinct semantic naming, and appropriate confirmation prompts or clear warnings.

### 14.5 Autonomous Observability & Instrumentation Mandate
1. **Measure, Don't Guess:** When diagnosing bugs, timing anomalies, or protocol errors, agents must never rely on unverified assumptions. Agents are explicitly authorized to introduce runtime instrumentation:
   - Dynamic logging via `ESP_LOG_LEVEL_SET` and module tags.
   - Execution timing probes and latency watermarks (`esp_timer_get_time()`).
   - Monotonic error and event counters.
   - FreeRTOS stack depth watermarks (`uxTaskGetStackHighWaterMark()`).
2. **Autonomous Engineering Enhancements:** The agent has full authority to fix obvious defects, missing validations, unhandled edge cases, and inadequate diagnostic telemetry within the scope of work without awaiting explicit permission. Workarounds are strictly forbidden; always implement the clean, ideal engineering solution.
3. **Ephemeral vs. Permanent Instrumentation:**
   - One-off diagnostic probes created solely to debug an isolated issue must be cleanly removed once the root cause is resolved and verified.
   - Subsystem diagnostics that provide general, reusable engineering value must be formalized, documented, and integrated cleanly into the `Developer Tools` structure.

### 14.6 Embedded Resource & Real-Time Constraints
1. **Zero Runtime Allocation:** All instrumentation, metric counters, diagnostic buffers, and CLI state structures must use static memory allocation. Dynamic heap calls (`malloc`, `free`) during runtime are strictly prohibited.
2. **Watchdog & Timing Non-Interference:** Diagnostic commands and logging operations must remain strictly non-blocking. Heavy diagnostic printing must never stall tasks monitored by the TWDT or alter real-time UART/Zigbee scheduling deadlines. Rate-limiting, conditional logging, and ring buffers must be used where necessary.

---

## 15. Full Autonomous Embedded SDLC Constitution & Engineering Governance

To govern all engineering tasks with high technical autonomy, architectural integrity, and evidence-based validation, all AI agents and contributors must strictly enforce the following SDLC directives across the entire lifecycle of the repository:

### 15.1 Core Operating Philosophy
Prefer:
- Evidence over assumption.
- Root cause over symptom suppression.
- Correct architecture over minimal diff size.
- Cohesive modules over monolithic convenience.
- Durable solutions over temporary workarounds.
- Explicit trust over implicit trust.
- Deterministic enforcement over repeated prose.
- Reversible operations over irreversible ones.
- Standards over vendor-specific duplication.
- Portable core behavior over harness lock-in.
- Project-local configuration over hidden machine state.
- Actual validation over optimistic claims.
- Focused context over context dumping.
- Independent review over self-confidence.
- Canonical artifacts over hidden memory.
- Truthful incomplete status over false completion.

Autonomy means completing engineering work without unnecessary supervision. Autonomy does not mean bypassing boundaries where product judgment, security, destructive risk, external authority, or irreversible consequences genuinely matter.

### 15.2 Mandatory Task Inception Workflow
Before substantial implementation on any meaningful task:
1. Inspect repository structure and `git status`.
2. Inspect relevant diffs and existing uncommitted work (preserve user changes).
3. Identify build system, toolchain versions, and dependency lockfiles.
4. Identify test infrastructure and CI/CD workflows.
5. Identify relevant hardware targets (ESP32-C6) and device configuration.
6. Identify relevant architecture, `AGENTS.md`, and `CODEBASE.md`.
7. Identify the actual requirement and observable acceptance criteria.
8. Search for existing related implementations and trace callers, state, config, tests, and interfaces.
9. Determine whether the reported issue is local or evidence of a deeper subsystem flaw.
10. Inspect the structural quality and cohesion of source files that the task will materially touch.
11. For substantial work, establish an implementation and validation plan before making broad edits.

### 15.3 Technical Autonomy vs. Product & Approval Boundaries
- **Technical Decisions (High Autonomy):** When the technically preferable solution is clear, decide and execute autonomously: internal implementation details, safe refactoring, decomposition of overgrown modules, test suites, build/tooling configuration, bug fixes, and testability improvements.
- **Product Decisions:** When multiple legitimate interpretations affect intended behavior, user experience, persistent project policy, or product semantics: explain the decision, identify realistic alternatives with trade-offs, recommend the technically most reasonable option with rationale, and ask the user to decide.
- **Explicit Approval Boundaries:** Explicit user approval is mandatory prior to:
  - Destructive Git history rewriting, force-pushing, deleting meaningful commits.
  - Repository-level governance/security policy changes or branch protection changes.
  - Destructive flash erase where meaningful state may be lost, eFuse programming, or factory reset with real consequences.
  - Irreversible hardware operations or unsafe electrical/thermal testing.
  - Secret creation/rotation/revocation where the user must authorize or provide material.
  - Consequential administrator/root elevation or risky machine-wide changes.

### 15.4 Root-Cause-First Engineering
Do not stop at symptom suppression. When debugging:
1. Reproduce or characterize the failure with evidence.
2. Trace execution/data/state path to identify the root cause.
3. Fix the problem at the correct architectural layer.
4. Validate both direct resolution and plausible regressions.
5. If several defects share the same root cause or broken abstraction, fix the entire related set. Do not expand into unrelated cleanup.

### 15.5 Responsibility-Driven Modularity & Structural Decomposition
Large source files must not grow indefinitely into multi-purpose monoliths. The primary criterion for decomposition is **responsibility**, not raw line count.
- **Decomposition Signals:** Multiple feature domains coexisting in one file; unrelated changes repeatedly modifying the same file; mixing parsing, orchestration, transport, and hardware control; accumulating unrelated dependencies; difficult test isolation; oversized functions.
- **Size-Based Review Triggers:**
  - `~250–300 LOC`: Trigger explicit cohesion/responsibility review.
  - `~400–500 LOC`: Explicitly determine whether the file still represents one cohesive responsibility.
  - `~700 LOC+`: Presume decomposition is appropriate unless a clear technical reason justifies the monolith (e.g. data tables, generated code).
- **Touched Overgrown Files:** If the current task materially touches a source file that is already overgrown or structurally confused, decompose it by domain before or as part of the implementation. Putting new behavior into an existing overgrown file without decomposing it is strictly forbidden.
- **Domain Decomposition Standard:** Decompose by domain responsibility (`parser`, `registry`, `commands`), never by arbitrary numbered slices (`part1`, `part2`) or generic dumping grounds (`misc`, `utils`, `helpers`). Avoid over-fragmentation; optimize for clear, durable responsibility boundaries.
- **Backward Compatibility Standard:** Backward compatibility is **not** a default objective. Obsolete APIs, legacy internal behaviors, and transitional shims must be aggressively removed once replacement behavior is implemented and validated.

### 15.6 Clean Workspace, Temporary Files & Disposable Artifacts
- **Repository Root Scratchpad Banned:** Never use repository root as a temporary scratchpad.
- **Temporary Artifacts Area:** Disposable scripts, captures, traces, debug logs, and temporary resource locks must reside strictly under:
  ```text
  .temporary/
  ```
  Ensure `.temporary/` is present in `.gitignore`.
- **Promotion vs. Disposal:** Artifacts that prove reusable regression or operational value must be promoted to permanent locations (`test/`, `tools/`), cleaned, and documented. Purely disposable artifacts must be removed before declaring completion.

### 15.7 Deferred Technical Debt Governance (`TODO.md`)
- Incomplete work, workarounds, or stubs must never be silently buried in source code.
- If an external constraint genuinely requires deferring work:
  1. Record it in root `TODO.md` with: ID, Title, Subsystem, Status, Reason, Evidence/Context, Why Not Now, Next Action, Unblock Condition.
  2. If a comment must remain in code, reference the central `TODO.md` ID. Orphan `TODO: fix later` comments are prohibited.
  3. `TODO.md` contains open actionable work only; remove completed items rather than accumulating historical changelog logs.

### 15.8 Quality Gates, Deterministic Enforcement & Zero Warnings
- **Deterministic Tools First:** Prefer compiler diagnostics, linters, formatters, and static analyzers over relying solely on prose.
- **No Weakening of Gates:** Never disable rules, lower warning severity, skip tests, or exclude files merely to turn a pipeline green.
- **Zero Meaningful Warnings:** Target zero warnings across the repository. Address compiler and linter warnings in all touched areas.
- **Meaningful Comments:** Write comments explaining *why*, invariants, constraints, hardware assumptions, and protocol specifics. Outdated comments are defects; update or remove them immediately.

### 15.9 Proportional Evidence Standards & Definition of Done (DoD)
All completion claims must be traceable to directly observed evidence.
- A task is **DONE** only when all applicable criteria are satisfied:
  1. Requirement correctly understood and root cause addressed.
  2. Architecture sound; touched files reviewed for responsibility creep; overgrown files decomposed.
  3. All new files follow established project naming conventions.
  4. PlatformIO target build succeeds (`pio run -e esp32-c6-devkitc-1`).
  5. All applicable unit tests pass (`pio test -e esp32-c6-devkitc-1 ...`, `tools/test_converter.js`).
  6. 100% command parity validated (`tools/validate_command_parity.js`).
  7. Hardware/protocol invariants verified (zero optimistic state, LRC checksums, confirmed physical telemetry).
  8. Security and resource impact (RAM/Flash headroom) verified.
  9. Final complete diff reviewed (`git status`, `git diff`) — no unintended files, temporary artifacts, or lingering debt.
  10. User work strictly preserved; `CODEBASE.md` and `AGENTS.md` fully synchronized.
  11. Final engineering report delivered with observed evidence, measured metrics, and any remaining blockers.

---

## 16. Modular LED Notification System & Animation Governance

To govern the onboard WS2812 RGB LED notification subsystem, preserve strict state priority rules, and ensure non-blocking deterministic behavior across CLI and Zigbee interfaces, every AI agent and contributor must adhere to the following directives:

### 16.1 Hardware Isolation & Low-Level Driver Decoupling
1. **Dedicated Hardware Driver:** Physical interaction with the onboard WS2812 LED on GPIO 8 is exclusively routed through `src/hardware/led/led_driver.[ch]`. Direct manipulation of RMT or GPIO registers outside this driver is strictly prohibited.
2. **Deterministic Color & State Caching:** The LED driver caches the active RGB color and state to avoid redundant hardware bus writes when the output color has not changed.
3. **Zero Dynamic Allocation:** The driver, animations, registry, and notification manager must never perform heap allocation (`malloc`, `calloc`) at runtime. All contexts, state machines, and buffers must be statically allocated.

### 16.2 Animation Modularity & Separation of Concerns
1. **Discrete Animation Architecture:** Every animation is an isolated module implementing the standard contract (`led_animation_t`, `led_anim_step_result_t`) residing strictly in `src/management/led_notification/Animations/`.
2. **Non-Blocking Step Contract:** Animations execute in discrete time steps (20ms standard cadence). Blocking loops (`vTaskDelay`, busy-wait) inside animation steps are forbidden.
3. **Animation Registry:** Animations are registered statically in `led_animation_registry.[ch]` and referenced by stable string IDs (`rainbow`, `white_double_blink`, `red_sos`, `green_blink`, `police_flasher`).

### 16.3 Strict Priority Engine & Absolute Disable Override
1. **Deterministic Priority Ranking:** Active LED presentation strictly enforces priority from highest to lowest:
   - Priority 1: `LED_EVENT_ZIGBEE_PAIRING` (Commissioning active)
   - Priority 2: `LED_EVENT_CMD_FAILURE` (Red SOS / One-shot command failure)
   - Priority 3: `LED_EVENT_CMD_SUCCESS` (Green Blink / One-shot command success)
   - Priority 4: `LED_EVENT_BOOT_WINDOW` (10-second boot indicator)
   - Priority 5: `LED_EVENT_NORMAL_OPERATION` (System running normally)
   - Priority 6: `LED_EVENT_OFF` (Explicitly turned off)
2. **Absolute Disable Override:** When `enabled == false` (via `led off` CLI or Zigbee `built_in_led_enabled: OFF`), the physical LED must be turned off immediately, all ongoing animations stopped, and one-shot queues suppressed. No event or command result may turn the LED on while disabled.

### 16.4 Non-Volatile Storage (NVS) Persistence
1. **Storage Namespace:** Persistent state is managed in the `led_store` NVS namespace via `led_nvs_storage.[ch]`.
2. **Deduplicated Writes & Corruption Recovery:** NVS writes are executed only when the value has changed. Corrupted or missing NVS records automatically fall back to factory default mappings with explicit warning logs.

### 16.5 Verification Integration & Zigbee Contract
1. **Confirmed Verification Triggering Only:** Command success/failure notifications (`led_notification_post_cmd_result()`) are triggered strictly from the final outcome of `z906_cmd_verifier_execute()`. They must never be triggered speculatively upon UART packet dispatch or Zigbee command receipt.
2. **No Optimistic State in Zigbee:** In `tools/conductor_converter.mjs`, the `z906_built_in_led` converter must return `null` in `convertSet` to ensure Home Assistant reflects only confirmed physical telemetry.

### 16.6 Adaptive Task Scheduling & CPU Conservation
1. **Adaptive Dynamic Polling:** When the onboard LED is globally disabled or idle/off, `led_task` must sleep using long timeouts (e.g. 500 ms) instead of busy 20 ms polling loops.
2. **Event-Driven Waking:** The LED notification manager exposes FreeRTOS task notification hooks (`led_notification_register_task_handle()`, `led_notification_wake_task()`) to wake `led_task` immediately upon state changes, one-shot triggers, or CLI configuration updates.

---

## 17. Native Zigbee OTA Architecture & Rollback Verification Protocol

To govern Over-The-Air firmware updates, maintain hardware partition safety, and enforce zero-downtime automated rollback, all AI agents must adhere to the following directives:

1. **Standard ZCL OTA Cluster (`0x0019`):** Firmware updates are delivered exclusively via native Zigbee 3.0 IEEE 802.15.4 radio frames. Direct internet/Wi-Fi/HTTP access from the controller is strictly forbidden.
2. **Dual Symmetrical Partition Allocation:** The active partition table allocates dual 1,792 KB application partitions (`ota_0` and `ota_1`). Firmware binary size must remain strictly within this 1,792 KB ceiling.
3. **Mandatory Post-Update Self-Validation Window:** Upon first boot into a new firmware image (`ESP_OTA_IMG_PENDING_VERIFY`), a 15-second health evaluation window must be enforced.
4. **Five Subsystem Health Criteria:**
   - Scheduler health (FreeRTOS scheduler running)
   - Watchdog health (TWDT initialized, no resets detected)
   - NVS health (Persistent storage partitions operational)
   - Zigbee health (Stack active, endpoint clusters bound)
   - Z906 communication health (UART HAL operational, verified amplifier telemetry)
5. **Automated Rollback Triggering:** If any criterion fails or the 15-second window expires without all criteria passing, `ota_health_trigger_rollback()` must immediately mark the partition invalid and reboot into the previous workable firmware slot.
6. **Permanent Validation Commit:** Only after all 5 criteria pass continuously throughout the observation window may `esp_ota_mark_app_valid_cancel_rollback()` be called to finalize the upgrade.

---

## 18. Code Generation & Quality Standards

These rules govern **how** AI agents produce, modify, and communicate code changes across all layers of the project (C firmware, JavaScript converter, Python tools, test suites). They complement the operational rules in Section 1.2 with detailed, actionable sub-directives.

### 18.1 Code Completeness & Non-Truncation
1. **Zero Placeholder Policy:** Every code block delivered by an AI agent must be complete and immediately usable. The following patterns are permanently forbidden inside any generated file or code block:
   - `// Remaining code here...`
   - `// ... existing code ...`
   - `/* TODO: preserve old logic */`
   - `# ... (rest unchanged)`
   - Any similar ellipsis or hand-wave substitute.
2. **Full Function & File Output:** When a function or file is modified, the entire function or file must be emitted — not just the changed lines surrounded by implicit gaps. Partial outputs require the user to mentally merge diffs, which introduces integration errors.
3. **Modify Only What Is Requested:** When updating a specific function, do not silently delete, reorder, or refactor unrelated functions in the same file. Unrelated code must be preserved verbatim.

### 18.2 Single-Task-Per-Response Discipline
1. **Scope Evaluation Before Writing:** Before producing code, evaluate the full scope of the requested change:
   - How many files are affected?
   - Does the change require new types, structs, or header updates?
   - Are there dependent test files that must be updated in lock-step?
2. **Plan-First Threshold:** If the answer to any of the above implies changes to **three or more files**, or any structural/architectural modification, the agent must present an `implementation_plan.md` artifact first and await explicit user confirmation before writing code.
3. **No Compression Under Pressure:** Attempting to fit a multi-file change into a single response by truncating code is strictly forbidden. Correctness and completeness take priority over response brevity.

### 18.3 Architecture Integrity & Type Safety
1. **Read Before Writing:** Before adding a new feature, the agent must inspect:
   - The target module's header file and all files that `#include` it.
   - Relevant type definitions (`types.ts`, C `enum`/`struct` definitions in `*.h`).
   - The existing design pattern used by the nearest similar module.
2. **No `any` or Equivalent:** The use of `any` (TypeScript), `void *` without a documented cast rationale (C), or untyped dictionaries (Python `dict` without type hints) is forbidden. All interfaces must be explicitly and fully typed.
3. **Pattern Conformance:** New code must follow the design patterns already present in the codebase:
   - New commands → registered in `z906_cmd_registry` and dispatched via `z906_cmd_verifier_execute()`.
   - New events → published through the `EventBus`, never via direct function calls across modules.
   - New persistent state → stored and retrieved through the `nvs` manager, never raw `nvs_set_*` calls in application code.
4. **No Unsolicited Dependencies:** New third-party libraries, npm packages, or ESP-IDF components must not be introduced without explicit user authorization. Document the justification and impact if a new dependency is unavoidable.

### 18.4 Error Handling & Edge Case Coverage
1. **Beyond the Happy Path:** Every new function must handle, at minimum:
   - Null / uninitialized pointer inputs.
   - Out-of-range parameter values.
   - Allocation or initialization failures (where applicable).
   - Mutex acquisition timeouts (for any function acquiring a FreeRTOS mutex).
2. **Meaningful Error Propagation:** Functions must return typed error codes (e.g., `esp_err_t`, custom `conductor_err_t`) rather than silently swallowing failures. Callers must check and log every non-`ESP_OK` return.
3. **Structured Logging at Critical Points:** Use ESP-IDF's `ESP_LOGE` / `ESP_LOGW` / `ESP_LOGI` at every critical operation boundary:
   - UART packet dispatch and reception.
   - Zigbee attribute report publication.
   - NVS read/write operations.
   - Command verification outcomes (`CMD_VERIFY_SUCCESS`, `CMD_VERIFY_FAILURE`, `CMD_VERIFY_TIMEOUT`).

### 18.5 Change Summary & Critical Approval Gate
1. **Pre-Code Summary (Mandatory):** Before producing any code block that modifies existing logic, the agent must state in 2–3 bullet points:
   - **What** is being changed.
   - **Why** the change is necessary.
   - **How** the implementation achieves the goal without breaking existing contracts.
2. **Architectural Decision Gate:** If a code change requires any of the following, the agent must **stop, explain the trade-offs, and wait for explicit user approval** before writing code:
   - Changing the public API of an existing module (function signatures, return types, enum values).
   - Altering FreeRTOS task priorities, stack depths, or watchdog registration.
   - Modifying the partition table, flash layout, or NVS namespace schema.
   - Introducing a new inter-task communication channel (queue, event group, semaphore).
   - Changing the Zigbee ZCL cluster layout or attribute IDs.
3. **No Silent Architectural Drift:** An agent must never silently refactor, rename, or reorganize code beyond the immediate task scope — even if the refactor appears beneficial. All structural changes require a plan artifact and user sign-off.

---

## 19. Project Directory Hygiene Protocol

The project root is a curated space. Its cleanliness directly reflects the discipline of every contributor — human or AI. A cluttered root is the first sign of an undisciplined engineering process.

### 19.1 Canonical Root-Level Structure
Only the following entries are permitted at the project root. Any file or directory not listed here requires explicit user approval before being created:

```text
/
├── .github/            # CI/CD workflow definitions
├── .pio/               # PlatformIO build cache (gitignored)
├── .vscode/            # Editor settings (partially tracked)
├── dist/               # Release OTA artifacts (tracked)
├── include/            # Shared public headers (config.h)
├── lib/                # PlatformIO private libraries
├── logs/               # Temporary session logs (gitignored — see §19.2)
├── managed_components/ # ESP-IDF component manager cache (gitignored)
├── src/                # All firmware source code
├── test/               # All unit and HIL test suites
├── tools/              # Developer scripts and tooling
├── .gitignore          # ← tracked
├── AGENTS.md           # ← tracked
├── CODEBASE.md         # ← tracked
├── CMakeLists.txt      # ← tracked
├── dependencies.lock   # ← tracked
├── eim_config.toml     # ← tracked
├── partitions.csv      # ← tracked
├── platformio.ini      # ← tracked
├── README.md           # ← tracked
├── sdkconfig.defaults  # ← tracked
├── sdkconfig.<env>     # ← tracked (per-env IDF config snapshot)
└── TODO.md             # ← tracked
```

**Non-negotiable rule:** No `.log`, `.tmp`, `.bak`, `.txt`, `.json`, `.py`, or any other ad-hoc file may be created directly in the project root. If a script needs to write output, it must target `logs/` or `tools/`.

### 19.2 Log File Policy
All log files produced by AI agents, developer scripts, and PlatformIO commands must be redirected to the `logs/` directory. This directory is:
- **Gitignored** — never committed to the repository.
- **Ephemeral** — may be deleted at any time without data loss.
- **Writable by AI agents** — agents that need to capture command output must use `logs/<descriptive-name>.log` as the target path.

### 19.3 Temporary / Scratch File Policy
Temporary files generated during a work session (JSON dumps, intermediate outputs, test captures, Python scratch scripts) must be placed in:
- **`logs/`** — for command output and serial captures
- **`tools/`** — only if the script is reusable and correctly documented
- **Nowhere else**

Scratch files that served a one-off debugging purpose must be deleted or moved to `logs/` **before the session's git commit**. Leaving debugging artifacts in `src/`, `test/`, or the project root is a policy violation.

### 19.4 Pre-Commit Cleanliness Gate
Before every `git commit`, an AI agent MUST run and verify both checks pass:
```powershell
# 1. Confirm only expected entries exist in the project root
Get-ChildItem -Path "." | Where-Object { !$_.PSIsContainer } | Select-Object Name

# 2. Confirm no stray log/temp files exist outside logs/ and .pio/
Get-ChildItem -Path "." -Recurse -Include "*.log","*.tmp","*.bak" |
    Where-Object { $_.FullName -notmatch "\\logs\\" -and $_.FullName -notmatch "\\.pio\\" }
```
If either check reveals unexpected files, the agent must clean them up before committing. A commit that introduces root-level clutter is a failed commit.

### 19.5 `tools/` Directory Hygiene
Scripts placed in `tools/` must meet the following bar before being committed:
- **Named clearly** — `validate_command_parity.js`, not `test2_final_v3.js`
- **Documented** — a top-of-file comment explaining purpose, usage, and expected output
- **Self-contained or explicitly dependent** — external dependencies must be documented in `README.md` or the script itself
- **Not a debugging one-off** — if the script was written to diagnose a specific bug and has no reuse value, it belongs in `logs/` (or deleted), not `tools/`

### 19.6 Agent Memory: Lessons Learned
The following failures occurred in this project and must not be repeated:

| Failure | Root Cause | Prevention |
|---------|-----------|------------|
| 20 `.log` files created in project root | AI agent redirected `pio run`/`pio test` output to CWD without an explicit path | Always specify `logs/<name>.log` as the target |
| 10 MB `clean_build.log` committed to git | `*.log` was not in `.gitignore` at that point | `.gitignore` now covers `*.log` and the `logs/` directory |
| PowerShell regex bulk-patching corrupted C source files | `$1` back-reference + `Set-Content` changed line endings silently | Use `write_to_file` tool for surgical edits; never use PowerShell regex replace for multi-file source patching |
| `vTaskDelay` added without FreeRTOS include | Patch applied without verifying `#include` dependencies | After adding any new API call, always verify the required header is present |

---

## 20. System Metrics & Health Manager Governance Protocol

To maintain clear separation of responsibilities, low computational overhead, and zero architectural ambiguity between raw telemetry measurements, health state interpretations, and external presentation, all AI agents must strictly enforce the following seven mandatory directives:

### 20.1 Core Separation of Concerns (Measure → Interpret → Expose)
- **`System Metrics` (MEASURE):** Pure low-level hardware and runtime metric acquisition (`src/management/system_metrics/`). Samples raw internal sensors, FreeRTOS idle counters, and heap capabilities. It contains zero threshold evaluations, zero health policies, and zero business logic.
- **`Health Manager` (INTERPRET):** Pure state classification and policy engine (`src/management/health_manager/`). Consumes `system_metrics_snapshot_t` and subsystem health statuses, applies hysteresis and sustained-sample filters, and produces a single unified health snapshot. It executes zero hardware driver calls, initiates zero I/O polling, and initiates zero automated recovery actions.
- **`CLI / Telemetry` (EXPOSE):** Pure presentation layer (`cli_console.c`, `telemetry.c`). Renders human-readable summaries or marshals data to external protocols without evaluating health rules.

### 20.2 Low Overhead & O(1) CPU Load Computation
- CPU load calculation must execute in **O(1) time complexity** by computing the delta between consecutive FreeRTOS idle task runtime counters (`ulTaskGetIdleRunTimeCounter()`) and the high-resolution hardware timer (`esp_timer_get_time()`).
- Iterating through FreeRTOS task lists (`uxTaskGetSystemState()`, `vTaskList()`) is strictly prohibited in periodic measurement routines to avoid unbounded CPU and stack overhead.
- Delta computations must enforce strict divide-by-zero protection, 32-bit counter rollover compensation, and bounded output constraints `[0.0f, 100.0f]`. The initial sample following boot or reset must be marked invalid (`cpu_load_valid = false`) to establish an accurate baseline.

### 20.3 SoC Die Temperature Terminology Standard
- The internal ESP32-C6 thermal sensor monitors the silicon substrate temperature.
- The terminology **"SoC Die Temperature"** (`soc_temp_c`, `soc_temp_valid`) must be strictly used across all C source code, headers, comments, documentation, and CLI interfaces.
- The phrase **"CPU Temperature" is strictly prohibited** to prevent technical inaccuracies and confusion with external amplifier heatsink temperatures (`temperature_1/2/3`).

### 20.4 Memory Semantics & Heap Representation
- Microcontroller memory statistics must query internal 8-bit capable memory (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`) via `heap_caps_get_info()`.
- Snapshots must expose total internal heap size, currently used heap, free heap, minimum ever free heap (watermark), largest free block, and percent heap used.
- **Heap utilization percentage must NEVER be presented as "RAM %" or "System Memory %"**, as this conflates dynamic heap allocation with static data, BSS segments, and task stack reservations.

### 20.5 No God Object & No Duplicate Source of Truth
- System Metrics and Health Manager must remain lightweight, decoupled modules.
- Subsystem health states must be queried directly from their authoritative source modules (`z906_state_t` for amplifier telemetry, `wdt_manager` for TWDT health, `zigbee_app` for network status). Health Manager must never duplicate or maintain parallel state stores.

### 20.6 No Automatic Recovery Policy
- **Health Manager is strictly an observer and interpreter.** It evaluates and classifies system health (`HEALTH_STATUS_HEALTHY`, `HEALTH_STATUS_DEGRADED`, `HEALTH_STATUS_FAULT`).
- Under no circumstances may Health Manager initiate automatic reboots (`esp_restart()`), reset hardware pins, restart FreeRTOS tasks, or trigger automated recovery actions. System watchdog enforcement remains the exclusive domain of the hardware Task Watchdog Timer (`wdt_manager`).

### 20.7 Change-Only Diagnostic Logging
- Periodic health evaluations must execute silently during steady-state operation.
- Diagnostic log messages (`ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`) may be emitted **only upon a confirmed change in overall health status, primary degraded component, or primary fault reason**. Periodic log spam is strictly prohibited.

### 20.8 Temperature Sensor Contract & Health Threshold Bounds
- The internal ESP32-C6 temperature sensor driver must be configured to the hardware calibrated range `[20°C, 100°C]` (matches hardware attribute table `{-1, 7, 20, 100, 2}`).
- Warning (75.0°C) and Critical (85.0°C) health policy thresholds must remain strictly within this measurable driver range.
- Critical health thresholds must never exceed the sensor driver's configured maximum range.

### 20.9 `UNKNOWN` Health State Lifecycle Semantics
- `HEALTH_STATUS_UNKNOWN` represents genuine lifecycle phases (baseline pending, uninitialized subsystem, or pre-telemetry boot).
- Overall health aggregation is deterministic: FAULT > DEGRADED > UNKNOWN (if any required baseline component is UNKNOWN) > HEALTHY.
- Systems must never synthesize speculative `HEALTHY` states prior to confirmed telemetry ingest.

### 20.10 Memory Health Semantics & Historical Watermark Separation
- `heap_min_free_bytes` represents a lifetime historical watermark; it must not be used as the exclusive criterion for current operational memory health.
- Current memory health must evaluate `heap_free_bytes` (< 32 KB warn / < 16 KB crit), `heap_usage_percent`, and largest allocatable block fragmentation (< 8 KB).
- Historical memory pressure is recorded as a diagnostic reason without locking operational health into permanent degradation after memory is recovered.

### 20.11 Autonomous Continuous Monitoring & Staleness Detection
- Health Manager evaluations are triggered directly upon completion of each `system_metrics` sampling pass.
- An autonomous low-frequency timer (`esp_timer`, 2000 ms period) evaluates staleness deadlines. If metric producers become completely silent for > 5000 ms, the system automatically transitions to `HEALTH_STATUS_DEGRADED` (`HEALTH_REASON_METRIC_STALE`).
- Automatic recovery occurs immediately when fresh metrics are processed. Health Manager must never devolve into an on-demand diagnostic tool that only evaluates when queried via CLI.

### 20.12 Selective Diagnostic Exposure to Zigbee (No Flood, Event-Driven)
To expose meaningful health indicators to Zigbee2MQTT / Home Assistant without flooding the 802.15.4 mesh network with noisy telemetry, all AI agents must enforce the following six rules:

1. **Selective Diagnostic Set Only:** Only four diagnostic attributes are exposed on Endpoint 4 (Cluster `0xFC03`):
   - `0x0009` `device_health` (UINT8: `0=unknown`, `1=healthy`, `2=degraded`, `3=fault`)
   - `0x000A` `health_primary_component` (UINT8: matches `health_component_t`)
   - `0x000B` `health_primary_reason` (UINT8: matches `health_reason_t`)
   - `0x000C` `soc_temperature` (INT16: 0.01 °C units)
2. **Prohibited Diagnostic Exposure:** Detailed operational metrics (`cpu_load`, `heap_usage`, `free_heap`, `minimum_free_heap`, `largest_free_block`, `uptime`, `reset_reason`) must NOT be added to Zigbee clusters. They remain strictly on-device CLI diagnostics.
3. **Strict Event-Driven Reporting:** Health status, primary component, and primary reason attributes are reported only when a state transition occurs (overall status, primary component, or primary reason code changes). Steady states produce zero reports.
4. **SoC Temperature Rate Limiting:** SoC die temperature telemetry reports only if:
   - Minimum interval of 60 seconds has elapsed (`dt >= 60s`) **AND** absolute temperature change is at least 1.0°C (`delta >= 100` in 0.01°C units), OR
   - Maximum heartbeat interval of 15 minutes has elapsed (`dt >= 900s`), OR
   - A critical health state transition occurs (bypasses rate limiter immediately for coalesced diagnostic dispatch).
5. **Invalid Sensor Sentinel (0x8000) Suppression:** When the internal temperature sensor reading is invalid (`!is_valid` or out of range), the firmware publishes standard ZCL invalid sentinel `0x8000` (`-32768`). The Zigbee2MQTT external converter must drop/suppress this value so that fake values (`0°C` or `-327.68°C`) are never presented in Home Assistant.
6. **Zero Reverse Dependency & Memory Invariant:** Health Manager must never `#include` Zigbee or ZCL headers (`esp_zigbee.h`, `zcl_clusters.h`). Decoupled notifications must route through neutral observer callbacks (`health_manager_register_change_observer`). The `esp_timer` staleness mechanism operates within FreeRTOS timer task bounds, guaranteeing zero runtime dynamic heap allocation after startup.

