# ADR-0001: PoC Hardware Baseline

Status: Blocked

## Context

PoC-B through PoC-E require a reproducible ESP32-S3 board, display, SD bus, and power-cut setup. No concrete hardware has been selected yet. Results collected before this record is accepted cannot be used to pass an architecture gate.

## Required Decision

- ESP32-S3 board model and hardware revision
- Flash size/mode/frequency and PSRAM size/mode/frequency
- 300x400 panel model, controller, pixel format, SPI host, pins, frequency, and DMA limits
- SD card model/capacity, SDMMC or SPI mode, pins, and card-detect behavior
- UART adapter and stable board identifier
- External power-cut controller and measurement method
- Test equipment firmware and configuration revisions

## Acceptance Criteria

- [ ] The selected board exposes 16 MiB Flash and 16 MiB PSRAM as required.
- [ ] The display and SD wiring are recorded in a machine-readable lab configuration.
- [ ] Flash, PSRAM, display, and SD settings build under the exact ESP-IDF v6.0.2 tag.
- [ ] The power-cut fixture removes board power; `esp_restart()` is not used as a substitute.
- [ ] At least one spare board is reserved for destructive Secure Boot, Flash Encryption, and eFuse tests.

## Gate Impact

- PoC-A may run without the final display panel, but its board and memory revision must be recorded.
- PoC-B is blocked until this ADR is Accepted.
- PoC-D and PoC-E are blocked until the SD and power-cut sections are Accepted.
