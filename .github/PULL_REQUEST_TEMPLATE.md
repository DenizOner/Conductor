## Description

Briefly describe the purpose of this PR, what is being changed, why, and how.

## Type of Change

- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality to not work as expected)
- [ ] Documentation update
- [ ] CI/CD or tooling enhancement

## Quality & Security Checklist

- [ ] My code strictly adheres to the architecture directives in `AGENTS.md` and `CODEBASE.md`.
- [ ] I have not used dynamic runtime memory allocation (`malloc`, `free`, `new`).
- [ ] All functions, variables, and comments are fully typed (no unbounded `any` or placeholder comments).
- [ ] I have run local host automated validators (`node tools/test_converter.js`, `node tools/validate_command_parity.js`).
- [ ] Firmware builds successfully via PlatformIO (`pio run -e esp32-c6-devkitc-1`).
- [ ] No secrets, tokens, credentials, or private keys are introduced or logged.
- [ ] Living documentation (`CODEBASE.md`) and in-line code comments are updated in lockstep.
- [ ] Hardware state verification: No optimistic state assumption is used without confirmed physical telemetry.

## Hardware Verification Notes

If this change affects hardware drivers, serial protocol, or Zigbee networking, please describe physical hardware test results (or specify if verified via host emulator / mock):
