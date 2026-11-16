# Security Policy — Conductor

## Reporting a Vulnerability

The Conductor team takes the security of our firmware, software supply chain, and communication interfaces seriously.

### How to Report Privately

We strongly encourage responsible disclosure. **Please do NOT report potential security vulnerabilities through public GitHub issues or public discussions.**

Instead, please submit a vulnerability report through GitHub's **Private Vulnerability Reporting**:

1. Navigate to the [Security Advisories tab](https://github.com/DenizOner/Conductor/security/advisories).
2. Click **"Report a vulnerability"** to open a confidential advisory draft.
3. Provide:
   - Type of issue (e.g. buffer overflow, authentication bypass, secret exposure, denial of service).
   - Component affected (e.g. Zigbee stack, UART transport, CLI, OTA parser).
   - Detailed step-by-step reproduction instructions or proof-of-concept (PoC).
   - Expected vs. actual behavior and potential security impact.

### Response & Handling Process

- **Acknowledgement:** We aim to acknowledge receipt of your vulnerability report as promptly as possible on a best-effort basis (typically within 48 to 72 hours).
- **Assessment:** We will confirm the issue, evaluate its severity, and formulate a mitigation plan.
- **Remediation & Advisory:** Once a patch is developed and verified against hardware/host test suites, a coordinated security advisory and patched firmware release will be published.

## Supported Versions

Conductor is currently in active development. Security patches and updates are provided for the latest tagged production release and the active `main` branch.

| Version / Target | Supported          | Notes                                     |
| ---------------- | ------------------ | ----------------------------------------- |
| Latest Tagged Release | :white_check_mark: | Actively supported with security updates  |
| `main` (current development) | :white_check_mark: | Active development branch                 |
| Historical / Untagged commits | :x:                | Not maintained; upgrade to latest release |

## Security Architecture & Best Practices

- **Zero Dynamic Allocation at Runtime:** All tasks, queues, buffers, and protocol frames use deterministic static allocation to eliminate heap fragmentation and use-after-free vulnerabilities.
- **Task Watchdog Protection:** Core FreeRTOS tasks are supervised by the ESP32-C6 Task Watchdog Timer (TWDT).
- **Physical Hardware State Verification:** System commands strictly route through `z906_cmd_verifier` to prevent unverified optimistic state reporting.
- **OTA Verification & Cryptographic Integrity:** Zigbee OTA images require matching canonical File ID (`0x0BEEF11E`), header version (`0x0100`), manufacturer code (`0x131B`), image type (`0x0906`), and anti-downgrade version validation on firmware (`src/management/ota/ota_zigbee_transport.c`), complemented by SHA-512 integrity verification by the Zigbee2MQTT coordinator index prior to OTA transmission.
- **Actions & Supply Chain:** Workflows run with least-privilege permissions (`contents: read`), action dependencies are pinned to immutable commit SHAs, and release binaries are signed with GitHub Artifact Attestations.
