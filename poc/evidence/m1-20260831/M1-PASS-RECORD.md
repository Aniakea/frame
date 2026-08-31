# M1 Pass Record — Draft (PENDING OWNER CONFIRMATION)

Milestone: **M1 hardware-status vertical slice** (requirement IDs M1-001..M1-008,
`_doc/requirements-v4.2.md`). This record assembles the in-repository evidence for the
M1-008 owner decision. Per M1-008, the milestone stays Blocked until the repository owner
records Pass on the real target board; this document ships as the completeness draft and
**does not** constitute a Pass.

Evidence root: `poc/evidence/m1-20260831/` (run id `m1-20260831T021427Z`, verdict PASS).
Firmware: `poc/apps/m1_hardware` @ `0.1.0-dev.1`, ESP-IDF v6.0.2, xtensa-esp-elf gcc-15.2.0,
esp32s3. Hardware: Waveshare ESP32-S3-RLCD-4.2, module ESP32-S3-WROOM-1-N16R8 (16 MiB flash,
8 MiB PSRAM), device id `c1810c76790e`.

## M1-008 Required Artifact Completeness

| Required artifact | Path (relative to repo root) | SHA-256 | Status |
|---|---|---|---|
| Local evidence manifest | `poc/evidence/m1-20260831/manifest.json` (47 artifacts, verdict PASS) | see file (self-referential; tracked in git) | ✅ |
| sdkconfig | `poc/apps/m1_hardware/sdkconfig.defaults` (committed source) + generated `build/default/sdkconfig` of the M1-008 instrumented build | generated: `083523994a42cc07910e9d585a31fb1d464fb8f89fc0695dc0ad7ab0cbbc41bc` (soak-era sdkconfig recorded in `manifest.json` → `platform.sdkconfig_sha256` = `7d094856…cbf`) | ✅ |
| Partition table | `partitions.csv` (frozen; boot-log partition identity asserted on-device) | `261b5d3338a7785d048f12eedd26bbab535f6d8b273f29e529d196a04515e3a3` | ✅ |
| Linker map | `poc/evidence/m1-20260831/build/m1_frame_m1_hardware.map` | `a5d01bfa35c0def56d350aaf8c203d8d4bca13372ee83c4aa0aa43d094cf43fb` | ✅ |
| `idf.py size-components` | `poc/evidence/m1-20260831/build/m1_size_components.txt` (includes `idf.py size` summary) | `85281cdf6aa2ae986e3eeb89832c03caf6b6583880487c3ea124e0924adeb64d` | ✅ |
| Heap/stack metrics | `poc/evidence/m1-20260831/build/m1_metrics_samples.txt` (3 on-device `metrics` samples: boot-ish / post-selftest / post-60s-idle) | `cf7e4c280250111876693276a5deb3ca3ceb256c91f3faccf73913bf4613a257` | ✅ |
| Complete UART JSONL | `poc/evidence/m1-20260831/soak.log` (1 h raw UART) | `dc568ea538fb91767adffef8a02f78f73672f906050c3a1a36e9d0bf66d6a78f` | ✅ |
| TF JSONL + hash | `poc/evidence/m1-20260831/jsonl.ndjson` (358 lines, sequence 25..384 monotonic, per-line crc32c) | `2908825353a77bace2b0d733bf837897131efe31312b787c53c419b7ff0a9ce9` | ✅ |
| Status-page photo | `poc/evidence/m1-20260831/photos/status_page_20260831.jpg` | `03817e0424452eba786768960645aa9e369bdfef08fe1da9883a7b9b035e82ec` | ✅ |
| Board photo | `poc/evidence/m1-20260831/photos/board_front_20260831.jpg` | `87d232943d4328a15f3d6047fc25d2acd53fea210e29889836df47c44b8c9dcf` | ✅ |
| Module marking photo | `poc/evidence/m1-20260831/photos/board_back_module_20260831.jpg` (N16R8 ↔ boot-log 16 MiB/8 MiB cross-check) | `6576c98d54097f85554f013c34f96922082de96cb7413f654d074d04fe376b60` | ✅ |
| Input/firmware SHA-256 set | `manifest.json` → `source.project_document_sha256` (`7c2dc241…dff76`); M1-008 build binary `frame_m1_hardware.bin` sha256 `a0e937ba7e4a0bd51b9c74f6e6ed25acb8fc42d78087da2ed2810085a3f2c4bc` + device-reported `elf_sha256`/`image_sha256` inside the metrics transcript; soak-era image sha in `raw-history/*` and manifest notes | see cited files | ✅ |

Note: the generated sdkconfig, `.bin`, and `.elf` of the M1-008 instrumented build remain
build outputs outside the repository (gitignored by policy); their SHA-256 values are pinned
in the table above and in the manifest artifact notes, and the device-reported
`elf_sha256`/`image_sha256` appear verbatim inside `m1_metrics_samples.txt`.

## M1-001 .. M1-007 Acceptance Summary

- **M1-001 (scope)** — PASS: deliverable is the repeatable hardware-status firmware + static
  core status page only (`poc/apps/m1_hardware`, initial publish commit `5f7c60c`; app
  `README.md`); no dynamic ELF, plugin loader, or hot update anywhere in the M1 tree.
- **M1-002 (boot identity)** — PASS: boot banner asserts target esp32s3, 2 cores, flash
  16777216, PSRAM 8388608, IDF v6.0.2, toolchain gcc-15.2.0, image sha, board/device id,
  reset reason and partition identity (`raw-history/boot.log`, `raw-history/verify.log`;
  8/8 on-device assertions); module marking N16R8 photo corroborates flash/PSRAM sizes.
- **M1-003 (display)** — PASS: ST7305 10 MHz init + patterns + native-landscape 400x300
  static status page showing board/display/TF/RTC/SHTC3/KEY/Wi-Fi/logging/health
  (`photos/status_page_20260831.jpg`; display "OK" in every status round; PR #5 responsive
  rendering fix).
- **M1-004 (RTC/SHTC3/KEY)** — PASS: PCF85063 read/write + keep-across-soft-reset with SNTP
  resync (PR #5; `rtc set`/`rtc status` round-trips in `raw-history/`), SHTC3 CRC-valid
  plausible samples (27.7–27.8 °C / ~37.8 % across the soak), debounced KEY/BOOT
  short/long counting exactly +2/+2 with no spurious presses (PR #6; soak manifest metrics).
- **M1-005 (TF transitions)** — PASS: dedicated FAT32 `/frame` mount with all four
  transitions observed (boot-without-card → ABSENT, insert → HEALTH_CHECKING → READY, remove
  → ABSENT, reinsert → READY), write+fsync+read+SHA-256 health checks, no panic/blocking;
  absent-boot classified in ≤0.9 s device time (`soak.log`, manifest notes; PR #3).
- **M1-006 (1 h soak)** — PASS: 3601 s soak, 10 s RTC/SHTC3/status cadence (358 JSONL lines,
  zero `status:FAILED`, sequence strictly monotonic, no panic/WDT/heap-integrity failure,
  UART logging uninterrupted); sole failing criterion (Wi-Fi reconnect) root-caused and fixed
  in PR #4, re-verified 3/3 cycles — manifest verdict PASS with FAIL history preserved.
  Heap/stack instrumentation added for M1-008 (`build/m1_metrics_samples.txt`) closes the
  soak's "internal_free not evaluable" gap: internal min_ever 123295 B, PSRAM min_ever
  8362016 B, ≥60 s idle stable.
- **M1-007 (JSONL + credential handling)** — PASS: per-line valid JSONL with monotonic
  sequence, uptime/UTC validity, source/event/mode/status, mirrored to UART and to
  `/frame/logs/` when TF READY (`jsonl.ndjson`, `soak.log`); credentials persist as
  plaintext `/system/wifi.json` on `system_fs` under the documented, user-approved SEC-010
  development-board exception (app `README.md` and `_doc/requirements-v4.2.md` M1-007 note;
  encrypted-NVS provisioning remains Blocked pending the dedicated security board), password
  never appears in UART output, logs, or status.

## Known Limitations (owner should read before confirming)

1. **SEC-010 development-board exception** — Wi-Fi credentials are stored as plaintext JSON
   on the internal `system_fs` partition because Flash Encryption / HMAC eFuse burn-in is
   irreversible on the single dev board. Production must move to encrypted NVS before ship.
2. **PCF85063 coin cell** — RTC time does not survive full power loss without a CR1220 cell
   (soft resets retain it; SNTP re-syncs after Wi-Fi join). A sporadic
   `ESP_ERR_INVALID_CRC` on the shared I2C bus was observed at some USB-initiated boots
   (suspected electrical/seating, watch item in session issues log).
3. **BOOT long-press unexercised** — BOOT ≥1 s hold was never physically exercised by a
   human; the code path is identical to the device-proven KEY long band (same constants,
   same debounce machine). BOOT short ×2 is hardware-proven.
4. **Provisioning portal OTA rows are code-inspection only** — POST /connect from CONNECTED
   state (web edit) and POST /clear → AP return were build-verified and code-inspected but
   never driven over the air (bench host is polkit-blocked from joining the AP; wrong-password
   and console paths are device-proven).
5. **JSONL 640 B theoretical budget** — measured line ~594 B with ~46 B headroom, but the
   u64-digit worst case computes ~795 B > the 640 B buffer; any new JSONL field must
   re-measure or grow the buffer (silent-truncation guard history in session issues log).
6. **TF `RECOVERING` display width (cosmetic)** — "TF: RECOVERING / log:OK" (~207 px) and
   HEALTH_CHECKING (~230 px) exceed the 179 px left column and overlap toward the right
   column instead of clipping; pre-existing, visual-only.
7. **First-boot `system_fs` policy undecided** — a factory-blank littlefs partition cannot
   mount with `format_if_mount_failed=false`; this board was pre-formatted once. Fresh boards
   need a factory image or an explicit first-boot format decision before provisioning can
   persist.

## Decision

```
Repository Owner: Aniakea
Decision: M1 Pass recorded (owner confirmed in work session, 2026-08-31 UTC)
Date: 2026-08-31 (UTC)
```
