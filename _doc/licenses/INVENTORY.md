# License Inventory

Scope: the two sources [ADR-0003](../decisions/0003-waveshare-rlcd42-hardware.md) requires an
inventory for — (a) the official Waveshare repository (design input, reference-only) and (b) the
third-party code actually vendored into this repository. All license statements below were read
from the cited `LICENSE`/`LICENCE` files at the pinned commits; the license families are recorded
as documented upstream.

Status: reference-only for (a). This repository does **not** vendor, copy, or link any Waveshare
official code today; the inventory exists so future adoption of official examples re-checks each
bundled component instead of relying on the root license.

## (a) Waveshare official repository (not vendored)

Upstream: <https://github.com/waveshareteam/ESP32-S3-RLCD-4.2> at commit
`eb1f63427d735a22b9c30e22fa63ebddae1834d3` ([root LICENSE](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2/blob/eb1f63427d735a22b9c30e22fa63ebddae1834d3/LICENSE)).
The root Apache-2.0 (copyright Waveshare) covers only the root work; bundled components carry
their own licenses.

| Component | Source + commit | License | Vendored? | Path / evidence |
| --- | --- | --- | --- | --- |
| Root work (examples, firmware, docs) | waveshareteam/ESP32-S3-RLCD-4.2 @ `eb1f6342` | Apache-2.0 | No | root `LICENSE` |
| SensorLib | bundled @ `eb1f6342` (Arduino + ESP-IDF ExternLib copies) | MIT | No | `01_Arduino_Libraries/SensorLib/LICENSE` (Copyright (c) 2022 lewis he) |
| U8g2 | bundled @ `eb1f6342` (two copies) | BSD-3-Clause ("new-bsd / two-clause bsd") | No | `01_Arduino_Libraries/U8g2/LICENSE`; `02_Example/ESP-IDF/11_U8G2_Test/components/u8g2/LICENSE` |
| LVGL v9 | bundled @ `eb1f6342` | MIT | No | `01_Arduino_Libraries/lvgl9/lvgl/LICENCE.txt` (Copyright (c) 2025 LVGL Kft) |
| LVGL v8 | bundled @ `eb1f6342` | MIT | No | `01_Arduino_Libraries/lvgl8/lvgl/LICENCE.txt` (Copyright (c) 2021 LVGL Kft) |
| codec_board | bundled @ `eb1f6342` | Espressif Modified MIT | No | `02_Example/ESP-IDF/07_Audio_Test/components/ExternLib/codec_board/LICENSE` (also in `02_Example/Arduino/07_Audio_Test`, `10_FactoryProgram`) |
| esp_codec_dev | bundled @ `eb1f6342` | Apache-2.0 | No | `02_Example/Arduino/07_Audio_Test/src/ExternLib/esp_codec_dev/LICENSE` |
| XiaoZhi example firmware | bundled @ `eb1f6342` | MIT | No | `02_Example/XiaoZhi/XiaoZhiCode_V2.1.0/LICENSE` (Copyright (c) 2025 Shenzhen Xinzhi Future Technology) |
| LVGL bundled libs | inside lvgl9 @ `eb1f6342` | mixed, per file | No | `lvgl9/lvgl/src/libs/*/LICENSE.txt`: expat (Expat/MIT), freetype (FreeType Project License), gif, lodepng (Zlib), lz4 (BSD-2), qrcode (MIT, Project Nayuki), thorvg (MIT), tiny_ttf (dual MIT-style), tjpgd (ChaN BSD-style), barcode (MIT, LKC Technologies) |
| LVGL bundled fonts | inside lvgl9 @ `eb1f6342` | mixed, per file | No | `lvgl9/lvgl/scripts/built_in_font/font_license/`: Montserrat (OFL), FontAwesome5 (Font Awesome Free License), DejaVuSans (Bitstream Vera license), SourceHanSansSC (OFL), unscii; plus `src/stdlib/builtin/LICENSE_SPRINTF.txt` / `LICENSE_TLSF.txt` |

Adoption rule (from ADR-0003): before any Waveshare code is adopted, copied, or modified, this
table must be refreshed against the then-pinned commit and each bundled component's terms
followed individually; the root Apache-2.0 must not be assumed to cover bundled files.

## (b) Vendored third-party code (in this repository)

| Component | Source + commit | License | Vendored? | Path / evidence |
| --- | --- | --- | --- | --- |
| espressif/elf_loader v1.3.3 | espressif/esp-iot-solution `components/elf_loader` @ `6526c5b18e156cfbda2c7ce48e282e384c43b485` | Apache-2.0 | Yes | [`poc/apps/poc_a/components/elf_loader/`](../../poc/apps/poc_a/components/elf_loader/) — `license.txt` sha256 `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30` (recomputed 2026-09-07); provenance + local patches p1–p5 in `PATCHES.md`; upstream-file manifest `tree.sha256` |
| espressif/cmake_utilities 0.5.3 | components.espressif.com registry (managed dependency of elf_loader) | Apache-2.0 | Managed (build-time fetch, not committed) | pinned at [`poc/apps/poc_a/dependencies.lock`](../../poc/apps/poc_a/dependencies.lock) (version 0.5.3, component_hash `3513506…bb18f`); license verified 2026-09-07 from the local component cache `~/.espressif/tools/components/espressif/cmake_utilities/0.5.3/espressif__cmake_utilities-v0.5.3.zip` → `license.txt` (Apache License, Version 2.0) |

Notes:

- The elf_loader vendoring is PoC-A scope only; its `idf_component.yml` declares no other
  dependency (no mbedtls). Local deviations from upstream are recorded patch-by-patch in
  `PATCHES.md`; the license file itself is unmodified (sha256 above matches the canonical
  Apache-2.0 text).
- Managed components of the M1 firmware (`espressif__cjson`, `joltwallet__littlefs`,
  `lvgl__lvgl`) are not vendored source and are enumerated per-build by the SBOM gate
  (`uv run frame-sbom --output .build/reports/sbom.cdx.json`); they are out of this table's
  ADR-0003 scope.
