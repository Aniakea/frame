# ADR-0003: Waveshare ESP32-S3-RLCD-4.2 Hardware Baseline

Status: Blocked

## Context

[ADR-0001](0001-poc-hardware.md) 要求选择可重复的 board/display/SD/power-cut baseline，但其 16 MiB PSRAM acceptance criterion 与实际目标板不一致。v4.2 已选择 Waveshare ESP32-S3-RLCD-4.2；官方产品页、文档、原理图和固定 commit 可验证型号、N16R8 容量、ST7305 及主要 pin map。

本记录尚不能 Accepted。官方示例是设计输入，不是本项目 runtime evidence；当前还缺完整板级采样、真正移除 board power 的 fixture，以及用于不可逆 Secure Boot、Flash Encryption 和 eFuse test 的 spare board。

## Proposed Decision

当本 ADR Accepted 时，以 Waveshare ESP32-S3-RLCD-4.2 作为 v4.2 唯一硬件 baseline，并在硬件选型方面 supersede ADR-0001。ADR-0001 的证据纪律继续有效；其中“16 MiB PSRAM”条件由本记录的精确 N16R8 条件替代。

### Board Identity

- Product: `Waveshare ESP32-S3-RLCD-4.2`
- Module: `ESP32-S3-WROOM-1-N16R8`
- SoC: ESP32-S3, dual-core Xtensa LX7, up to 240 MHz
- Flash: 16 MiB
- PSRAM: 8 MiB
- Display: ST7305, fully reflective monochrome LCD, physical 300x400; product logical landscape 400x300
- PCB revision: 官方资料未提供可冻结的 revision；不得填写推测值
- Lab identity: 每块实物用唯一 `board_id`、采购 SKU、模组丝印照片和整板正反面照片记录

### Display Interface

Signal | Value
--- | ---
Host | `SPI3_HOST`
Mode | 0
Safe start clock | 10 MHz
SCK | GPIO11
MOSI | GPIO12
DC | GPIO5
CS | GPIO40
RESET | GPIO41
TE | GPIO6

10 MHz 是项目安全启动值。官方固定 commit 中常规 ESP-IDF display path 使用 SPI3 mode 0、10 MHz；较新的 U8g2 example 出现更高 clock，只能作为 PoC-B candidate，不覆盖安全基线。TE edge semantics、dirty-region alignment、DMA size 和最高稳定 clock 需要 logic analyzer + runtime evidence。

### I2C And Onboard Devices

Bus | Device | Address/Pin
--- | --- | ---
I2C | SDA | GPIO13
I2C | SCL | GPIO14
I2C | PCF85063 RTC | `0x51`
I2C | SHTC3 | `0x70`
Button | KEY | GPIO18, active-low, pull-up

### TF Interface

- Controller: SDMMC
- Width: 1-bit
- `CLK=GPIO38`
- `CMD=GPIO21`
- `D0=GPIO39`
- Card detect: none
- Baseline filesystem: FAT32/FatFs

缺少 card-detect 是硬件事实。软件通过 mount、status 和真实 I/O 结果区分 `ABSENT/HEALTH_CHECKING/READY/FAILED/RECOVERING`，不得虚构 GPIO detect。

## Official Sources Verified For This Proposal

- [Waveshare product page](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm)
- [Waveshare product documentation](https://docs.waveshare.com/ESP32-S3-RLCD-4.2)
- [Waveshare resources and datasheets](https://docs.waveshare.com/ESP32-S3-RLCD-4.2/Resources-And-Documents)
- [Official schematic PDF](https://files.waveshare.com/wiki/ESP32-S3-RLCD-4.2/ESP32-S3-RLCD-4.2-schematic.pdf)
- [Official ST7305 datasheet](https://files.waveshare.com/wiki/common/ST_7305_V0_2.pdf)
- [Official PCF85063 datasheet](https://files.waveshare.com/wiki/common/Pcf85063atl1118-NdPQpTGE-loeW7GbZ7.pdf)
- [Official SHTC3 datasheet](https://files.waveshare.com/wiki/common/SHTC3_Datasheet.pdf)
- [Official GitHub repository at commit eb1f63427d735a22b9c30e22fa63ebddae1834d3](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2/tree/eb1f63427d735a22b9c30e22fa63ebddae1834d3)
- [Root LICENSE at that commit](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2/blob/eb1f63427d735a22b9c30e22fa63ebddae1834d3/LICENSE)

核对结论：root `LICENSE` 是 Apache-2.0，copyright 2026 Waveshare。该结论只覆盖 root work 的声明；仓库 bundled components 包含各自的 `LICENSE`（例如 SensorLib、U8g2、codec components、LVGL fonts/libs）。采用、复制或修改官方代码前必须生成 dependency inventory 并逐项遵守 bundled license，不能用 root Apache-2.0 概括全部文件。

## Acceptance Evidence

- [ ] 实机启动记录确认 target、dual core、16 MiB Flash、8 MiB PSRAM 和 exact module marking。
- [ ] machine-readable lab config 固定上述 pin、SPI/I2C/SDMMC 参数、board_id、ESP-IDF tag 和 firmware hash。
- [ ] ST7305 在 SPI3 mode 0、10 MHz 完成 init 和 landscape 400x300 black/white/checkerboard/border/static core status page；driver 按面板原生列顺序打包，logic analyzer 保存 SCK/MOSI/DC/CS/RESET/TE trace。
- [ ] PCF85063 `0x51`、SHTC3 `0x70`、KEY GPIO18 active-low 分别通过 [M1](../requirements-v4.2.md) 规定的 normal 10 秒 cadence 与显式 selftest 样本。
- [ ] SDMMC 1-bit/FAT32 `/frame` 完成 boot absent、insert to READY、runtime remove、reinsert to READY 四转换和每次 READY read/write/fsync/hash health check；记录 TF model/capacity。
- [ ] 当前开发板的 Wi-Fi UART configuration 只驻留 RAM 并脱敏；专用 security board 上另行验证 encrypted NVS。UART 与 TF `/frame/logs` JSONL 在一小时 M1 soak 中符合连续性要求。
- [ ] Flash/PSRAM mode/frequency 由 exact sdkconfig 和 runtime log 证明，不由 N16R8 名称推断。
- [ ] power-cut fixture 能由外部 controller 真正移除并恢复 board power，记录切断点、off duration、voltage measurement 和 fixture firmware/config revision；`esp_restart()` 不算。Current board preliminary cuts 不超过 20。
- [ ] 至少一块 spare board 被保留并标识为 final 1000 power-cut 与 destructive security/eFuse board，不与日常开发板混用。
- [ ] 保存 schematic/repository commit、root 与 bundled license inventory。

## Current Blockers

- 完整 runtime evidence 尚未归档，不能确认 M1 和组合外设行为。
- power-cut fixture、外部控制器和 measurement method 尚未完成，阻塞 PoC-E。
- destructive Secure Boot V2、Flash Encryption release mode 和 eFuse epoch 使用的 spare board 尚未保留，阻塞 PoC-E。
- 没有官方 PCB revision；接受时必须显式批准以 `board_id + SKU + N16R8 marking + photos` 作为替代识别，不得伪造 revision。

## Consequences

- v4.1 的 16 MiB PSRAM、RGB565/60 fps 和未知 pin assumptions 被明确淘汰。
- 所有 memory budget 必须按 8 MiB PSRAM 重算。
- 显示测试改为 ST7305 packed monochrome SLA；10 MHz 是安全起点，不是已证实性能上限。
- no-card 必须是正常可恢复状态，因为硬件没有 card detect。
- Runtime TF loss 是全局 dynamic-plugin stop trigger；reinsertion health check 后才允许 registry-driven auto-start。
- Hardware evidence 只保存在本地 raw files 和 local manifest；当前没有 self-hosted GitHub hardware runner。
- 本 ADR 保持 Blocked 期间可执行 M1 和不依赖最终 fixture 的 PoC-A 准备，但不得据此声称 hardware baseline 已 Accepted，也不得开始 PoC-B 产品准入。
