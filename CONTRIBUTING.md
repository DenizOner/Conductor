# Contributing to Conductor

Thank you for your interest in contributing to **Conductor — Smart Zigbee Console for Logitech Z906**!

Before making contributions, please review our core directives and architectural principles documented in [AGENTS.md](AGENTS.md) and [CODEBASE.md](CODEBASE.md).

---

## 1. Core Principles & Philosophy

- **Inspect Before Acting:** Always examine the existing directory structure, header interfaces, and configuration files before modifying code.
- **Zero Assumptions Policy:** Never make assumptions about hardware pinouts, peripheral registers, or communication protocols. Every technical detail must be verified.
- **Strict Relative Paths:** All file paths, references, and documentation links must originate from the project root.
- **No Scope Creep:** Keep pull requests focused, concise, and targeted.
- **Documentation & Test Sync:** Keep `CODEBASE.md`, relevant test suites, and in-line code comments synchronized with every code modification.

---

## 2. Development Workflow

### 2.1 Branching Strategy

- `main` is protected: Direct pushes and force pushes are strictly prohibited.
- Always branch off `main` using descriptive branch names:
  - `feat/feature-name`
  - `fix/bug-fix-name`
  - `chore/maintenance-name`
  - `docs/documentation-update`

### 2.2 Local Verification Before Opening a PR

Ensure all automated host validators and firmware builds pass locally:

```bash
# 1. Run Host Automated Validators
node tools/test_converter.js
node tools/validate_command_parity.js

# 2. Build Firmware Binary via PlatformIO
pio run -e esp32-c6-devkitc-1
```

### 2.3 Pull Requests & Merge Policy

1. Open a pull request against `main` using the provided PR template.
2. Ensure all automated CI checks pass (`Host Automated Validators`, `Build Firmware Binary & Validate OTA`, `CodeQL`, `Dependency Review`).
3. Resolve all conversations and reviewer comments.
4. Linear git history is enforced: Merges must be squash merges or rebase merges.

---

## 3. Embedded Coding Standards (C & ESP-IDF)

- **Language:** English for all code, comments, headers, and commit messages.
- **SOLID Principles:** Single Responsibility Principle (SRP) per module; Dependency Inversion Principle (DIP) via `ServiceLocator` and `EventBus`.
- **Zero Dynamic Allocation at Runtime:** Heap allocation (`malloc`, `free`) during steady-state operation is strictly forbidden. Use static allocation for tasks, queues, and buffers.
- **Type Safety:** Always use explicit types from `<stdint.h>` (`uint8_t`, `uint16_t`, `uint32_t`, `int32_t`). Never use unbounded types or raw `any`.
- **Buffer Safety:** Enforce bounds checking on all serial packets and buffers. Unbounded functions (`strcpy`, `sprintf`) are prohibited. Use `strncpy`, `snprintf`.

---

## 4. Hardware Verification & Telemetry

- **No Optimistic State:** Never report a hardware command as successful based on serial transmission alone. Success requires confirmed physical telemetry through `z906_cmd_verifier`.
- **Hardware-Dependent Tests:** Tests requiring physical connection to an ESP32-C6 board or Logitech Z906 amplifier must be designated as hardware-dependent and verified on hardware benches.

---

## 5. Security & Responsible Disclosure

If you discover a security vulnerability, please follow our [Security Policy](SECURITY.md) and report it privately via GitHub Private Vulnerability Reporting. Do not open public issues for security vulnerabilities.
