# Logitech Z906 Native Zigbee 3.0 Controller

A high-performance, deterministic **Native Zigbee 3.0 Embedded Controller** for the **Logitech Z906 5.1 Surround Sound System**, built on the **Espressif ESP-IDF** framework targeting the **ESP32-C6** SoC (`esp32-c6-devkitc-1`).

---

## 1. Project Overview

This project provides an inline, transparent bridge between the physical Logitech Z906 Control Console (female DE-15) and the Subwoofer/Amplifier mainboard (male DE-15). By interposing between the two devices via dual hardware UART channels, it intercepts serial packets, synchronizes device state in real time, and exposes full audio control capabilities over a native Zigbee 3.0 mesh network.

### Key Capabilities
- **Original Console Preservation:** The original physical control console remains 100% operational alongside Zigbee network commands.
- **Native Zigbee 3.0 Router (ZR):** Direct mesh connectivity with standard Zigbee coordinators (Home Assistant ZHA, Zigbee2MQTT) without external bridges or WiFi networks.
- **Strict Hardware Telemetry:** Reported Zigbee attributes always reflect verified physical amplifier state, never speculative or assumed states.
- **Zero Runtime Dynamic Allocation:** All FreeRTOS task stacks, queues, and buffers are statically allocated for maximum determinism and memory safety.
- **Diagnostic USB CLI:** Interactive diagnostic console accessible over USB Serial/JTAG for real-time monitoring and manual debugging.

---

## 2. Target Hardware & Specifications

| Parameter | Specification | Details |
|---|---|---|
| **Microcontroller (SoC)** | Espressif ESP32-C6 | High-performance 32-bit RISC-V core (up to 160 MHz) |
| **Radio Subsystem** | Native IEEE 802.15.4 | Hardware 2.4 GHz Zigbee 3.0 & Thread radio |
| **Target Board** | `esp32-c6-devkitc-1` | Official Espressif ESP32-C6-DevKitC-1 development board |
| **Physical Flash Memory** | 8 MB (8,388,608 bytes) | Quad/Dual SPI NOR Flash, configured in DIO mode @ 80 MHz |
| **SRAM** | 320 KB | Internal high-speed SRAM |
| **Development Framework** | `espidf` (ESP-IDF v6.1.0) | Official Espressif IoT Development Framework via PlatformIO |
| **Operating System** | FreeRTOS | Native preemptive real-time operating system (1000 Hz tick) |
| **Zigbee Stack** | ESP-Zigbee-SDK | Official Espressif ZCL/ZDO Zigbee 3.0 stack (`esp-zigbee-lib`) |
| **Zigbee Device Type** | Zigbee Router (ZR) | Mains-powered mesh router with continuous listening |
| **Baud Rate** | 57,600 baud | 8 Data bits, Odd parity, 1 Stop bit (`8O1`) |

---

## 3. Hardware Interconnection & Pinout Map (DE-15 VGA)

The ESP32-C6 is interposed inline between the original Logitech Z906 Control Console and the Subwoofer/Amplifier mainboard:

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
| **—** | — | Connected (RX) | **GPIO 17** (UART1 TX) | Console Serial Feedback Output (`TX_CONSOLE_GPIO`) |

---

## 4. Multi-Tier Nested Architecture

The firmware is structured into feature-isolated nested layers adhering to the Single Responsibility Principle (SRP) and Dependency Inversion Principle (DIP):

```
[ src/communication/ ] <-- Zigbee 3.0 stack (ZCL/ZDO) and interactive USB Serial/JTAG CLI
           |
    [ src/core/ ]      <-- ServiceLocator, FreeRTOS EventBus, Finite State Machine & State Cache
           |
  [ src/protocol/ ]    <-- Z906 UART framing codec, packet parsing, LRC checksum, and opcodes
           |
  [ src/hardware/ ]    <-- Hardware Abstraction Layer (HAL) for Dual UART, ADC1 OneShot, and GPIO
           |
 [ src/management/ ]   <-- System NVS manager, Task Watchdog Timer (TWDT), and Line Telemetry
```

### FreeRTOS Task Priority Matrix
All tasks are statically allocated at startup in `src/main.c`:

| Task Name | Priority | Stack Depth | Core | Primary Responsibility |
|---|:---:|:---:|:---:|---|
| `uart_rx_task` | **5** (High) | 4096 bytes | Core 0 | Drains hardware UART FIFOs into static ring buffers; detects packet framing |
| `z906_fsm_task` | **4** (Med-High) | 4096 bytes | Core 0 | Evaluates EventBus queue, executes state transitions, dispatches Amp commands |
| `zigbee_task` | **3** (Medium) | 4096 bytes | Core 0 | Runs the official ESP-Zigbee-SDK event loop, processes ZCL frames, handles steering |
| `cli_task` | **2** (Med-Low) | 3072 bytes | Core 0 | Processes USB Serial/JTAG diagnostic console commands |
| `telemetry_task`| **1** (Low) | 2048 bytes | Core 0 | Performs periodic ADC voltage checks (cable presence, boot state) and TWDT resets |

---

## 5. Build, Flash & Monitor Instructions

All commands are executed using PlatformIO CLI from the project root:

- **Build firmware:**
  ```sh
  pio run -e esp32-c6-devkitc-1
  ```
- **Clean build artifacts:**
  ```sh
  pio run -t clean
  ```
- **Flash target device:**
  ```sh
  pio run -e esp32-c6-devkitc-1 -t upload
  ```
- **Open serial monitor:**
  ```sh
  pio device monitor -b 115200
  ```
- **Execute unit tests on target board:**
  ```sh
  pio test -e esp32-c6-devkitc-1
  ```

---

## 6. Native Zigbee Over-The-Air (OTA) Updates

The controller supports standard Zigbee OTA firmware updates over the Zigbee 3.0 mesh network via the ZCL OTA Upgrade Cluster (`0x0019`) and Zigbee2MQTT:

- **Zero Direct Internet:** The device does not connect to Wi-Fi or download from GitHub directly; the coordinator feeds blocks over Zigbee.
- **Fail-Safe A/B Partitioning:** Flashes to the alternate partition (`ota_0` / `ota_1`), leaving the active image intact until verified.
- **15-Second Post-Update Health Check:** Self-tests FreeRTOS scheduler, TWDT, NVS, Zigbee stack, and Z906 communication before permanent commit.
- **Automatic Rollback:** Any fault, panic, or unfulfilled criteria triggers an automated fallback to the previous known good firmware slot.

### Packaging & Deploying OTA Updates
1. **Compile Firmware:**
   ```sh
   pio run -e esp32-c6-devkitc-1
   ```
2. **Package into `.ota` Container:**
   ```sh
   python tools/build_ota_image.py --input .pio/build/esp32-c6-devkitc-1/firmware.bin --output dist/131b-0906-010400aa-LogitechZ906.ota
   ```
3. **Generate Zigbee2MQTT Index:**
   ```sh
   node tools/generate_ota_index.js dist/131b-0906-010400aa-LogitechZ906.ota "https://github.com/<owner>/Logitech_z906_Zigbee_Console/releases/download/v1.4.0/131b-0906-010400aa-LogitechZ906.ota" dist/index.json
   ```
4. **Validate Artifacts:**
   ```sh
   node tools/validate_ota_image.js dist/131b-0906-010400aa-LogitechZ906.ota dist/index.json
   ```

---

## 7. Living Documentation & Guidelines

This project maintains two continuously synchronized living documentation files:
- **[CODEBASE.md](CODEBASE.md):** The single source of truth for current technical reality, complete directory tree, flash partition tables, hardware pinouts, and test suites.
- **[AGENTS.md](AGENTS.md):** Governing rules for AI coding agents and contributors, covering embedded C SOLID principles, memory budgeting, and the living documentation protocol.

