# Frame Requirements v4.2

版本: `4.2-draft`

状态: Draft；冻结已达成的产品决策，尚未批准完整产品组件编码，且不表示任何 PoC 已通过

目标硬件: Waveshare ESP32-S3-RLCD-4.2，ESP32-S3-WROOM-1-N16R8，16 MiB Flash，8 MiB PSRAM

关联文档: [v4.2 架构差异](project-v4.2-draft.md)、[v4.1 历史架构](project.md)、[开发计划](development-plan.md)、[ADR-0003](decisions/0003-waveshare-rlcd42-hardware.md)

## 1. 约束语言与状态

本文使用稳定 domain ID；后续修订不得复用或重新解释已发布 ID。删除的需求保留 ID 并标记 `Retired`。

状态 | 含义
--- | ---
`Frozen` | 已冻结的 v4.2 决策。只有更高优先级的 Accepted ADR 或明确发布的新需求版本可以改变
`PoC-derived` | 需求和验收方法已冻结，但数值、机制或可行性结论必须来自指定 PoC；在证据签署前不得填入乐观值
`Blocked` | 已知外部条件不具备；解除阻塞前不得声明满足，也不得用模拟结果代替板级结果

`MUST`、`MUST NOT`、`SHOULD` 分别表示必须、禁止和建议。每条需求的状态不代表完成状态；实现完成度由可追溯矩阵和证据记录。

### 1.1 Correction Note

本次修订纠正了同一 `v4.2-draft` 早期文本中误保留的 v4.1 假设。为保持追踪稳定，`PART-001..004`、`M1-001..008`、`UI-001..009`、`PLUG-001..009`、`STOR-001..007`、`MEM-001..008`、`TEST-001..009`、`GH-001..007` 的 ID 保留，但其下列旧语义被明确 supersede，且不再是 v4.2 requirement：含 `assets_fs` 的分区、从 `/sd/plugins` 扫描代码、code/resource 混合 MPB、TF 缺失时只禁用有资源插件、M1 clock scene、常规 1 秒传感采样、5 ms command SLA、50 ms full-refresh SLA、KEY 切主题、强制 reviewer approval 和假定存在 hardware runner。

## 2. Governance (`GOV`)

- **GOV-001 [Frozen]** 规范冲突时的权威顺序为：Accepted ADR > 本 `v4.2` requirements > [v4.2 architecture delta](project-v4.2-draft.md) > [v4.1](project.md)。Proposed 或 Blocked ADR 仅记录候选决策，不覆盖本需求。
- **GOV-002 [Frozen]** [project.md](project.md) 是 `v4.1-draft` 历史文档，MUST 保持原样；v4.2 通过新文件描述差异，不就地改写历史假设。
- **GOV-003 [Frozen]** 本文状态为 `v4.2-draft`。只有 `PoC-A` 至 `PoC-E` 均通过、外部 blocker 清零、逐项需求有证据链接并完成 sign-off 后，才能发布非 Draft 基线。
- **GOV-004 [Frozen]** 允许在 sign-off 前编写文档、构建 bootstrap、host test、M1 hardware-status firmware 和隔离 PoC。MUST NOT 在 S1 architecture sign-off 前把 PoC 代码提升为产品 `PluginLoader`、`LifecycleManager`、服务组件或产品插件。
- **GOV-005 [Frozen]** 失败的准入项触发架构复审；MUST NOT 静默降级为静态插件、脚本 VM、未签名资源或不同硬件并继续宣称符合 v4.2。
- **GOV-006 [Frozen]** 每个 PR MUST 标明影响的 requirement ID、ADR 和证据。实现与需求不一致时先更新并评审文档，不得让实现成为事实规范。

## 3. Hardware (`HW`)

- **HW-001 [Frozen]** 唯一产品基线为 Waveshare `ESP32-S3-RLCD-4.2`，模组 MUST 精确为 `ESP32-S3-WROOM-1-N16R8`：16 MiB Flash、8 MiB octal PSRAM。不得按 v4.1 的 16 MiB PSRAM 做容量推导。
- **HW-002 [Frozen]** 显示控制器为 `ST7305`，物理面板为 `300x400`，产品逻辑方向固定为 landscape `400x300`；单色 packed framebuffer 为 120000 bit，即 15000 byte。产品显示路径 MUST NOT 使用 `RGB565` framebuffer、先画 portrait 整帧再做运行时图像旋转，或承诺 60 fps 动画。本条 supersede 本草案先前的 portrait 决策。
- **HW-003 [Frozen]** 显示总线使用 `SPI3_HOST`、mode 0，初始频率 10 MHz；引脚为 `SCK=GPIO11`、`MOSI=GPIO12`、`DC=GPIO5`、`CS=GPIO40`、`RESET=GPIO41`、`TE=GPIO6`。提高频率只能作为 PoC-B 的独立变量，不能改变 10 MHz 安全启动基线。
- **HW-004 [Frozen]** 板载 I2C 使用 `SDA=GPIO13`、`SCL=GPIO14`；`PCF85063` 地址为 `0x51`，`SHTC3` 地址为 `0x70`。总线扫描不是设备身份校验的替代品。
- **HW-005 [Frozen]** TF 使用 SDMMC 1-bit：`CLK=GPIO38`、`CMD=GPIO21`、`D0=GPIO39`，没有 card-detect。软件 MUST 通过 mount/I/O 结果建立 `ABSENT`、`HEALTH_CHECKING`、`READY`、`FAILED`、`RECOVERING` 状态，不得根据不存在的 detect pin 判断。
- **HW-006 [Frozen]** 用户 `KEY` 为 `GPIO18`、active-low、input pull-up。M1 必须验证按下和释放；产品层只接收去抖后的语义事件。
- **HW-007 [Frozen]** 官方资料未提供可冻结的 PCB revision。证据使用产品型号、模组丝印 `N16R8`、板卡照片、采购 SKU 和实验室唯一 `board_id` 标识；不得编造 revision。
- **HW-008 [PoC-derived]** Flash mode/frequency、PSRAM mode/frequency、可用 internal SRAM/IRAM、显示最高稳定 SPI 频率和 TE 语义由锁定 ESP-IDF 版本上的 M1/PoC 证据确定。在此之前只可使用安全配置，不可声称官方示例数值已被本项目验证。
- **HW-009 [Blocked]** ADR-0003 当前因完整 runtime evidence、真正切断板级电源的 power-cut fixture 和 destructive security test spare board 不齐全而不能 Accepted。详见 [ADR-0003](decisions/0003-waveshare-rlcd42-hardware.md)。

## 4. Flash Partition (`PART`)

- **PART-001 [Frozen]** 16 MiB Flash 的产品分区 MUST 精确采用下表。Offset 和 Size 均为 byte address；最后一个分区 MUST 严格结束于 `0x1000000`。

Name | Type/SubType | Offset | Size | End | Mount/用途
--- | --- | --- | --- | --- | ---
`nvs` | `data/nvs` | `0x009000` | `0x014000` | `0x01D000` | encrypted NVS data
`nvs_keys` | `data/nvs_keys` | `0x01D000` | `0x001000` | `0x01E000` | NVS encryption keys
`otadata` | `data/ota` | `0x01E000` | `0x002000` | `0x020000` | OTA selection
`coredump` | `data/coredump` | `0x020000` | `0x020000` | `0x040000` | crash evidence
`ota_0` | `app/ota_0` | `0x040000` | `0x500000` | `0x540000` | firmware slot A
`ota_1` | `app/ota_1` | `0x540000` | `0x500000` | `0xA40000` | firmware slot B
`plugin_fs` | `data/littlefs` | `0xA40000` | `0x4C0000` | `0xF00000` | `/plugins`, 4.75 MiB
`system_fs` | `data/littlefs` | `0xF00000` | `0x100000` | `0x1000000` | `/system`, 1 MiB

- **PART-002 [Frozen]** production 中 `nvs_keys`、`plugin_fs` 和 `system_fs` MUST 受 Flash Encryption 保护，Wi-Fi/UART 配置 MUST 使用 NVS encryption；Flash Encryption 不替代 MPB signature。v4.2 没有 `assets_fs`，实现和文档均不得重新引入。
- **PART-003 [Frozen]** 构建 MUST 校验分区无 overlap、全部对齐、两个 OTA slot 均为 `0x500000`、`plugin_fs` 恰为 `0x4C0000`、`system_fs` 恰为 `0x100000`、最后地址恰为 `0x1000000`，且 app image 不超过 slot。
- **PART-004 [Frozen]** 当前仓库分区文件尚不等于 PART-001；在获准的 bootstrap 阶段同步之前，MUST 将其视为 implementation gap，而不是修改本需求以迁就旧文件。本条 supersede 先前错误的 `system_fs@0xC40000` 和 `assets_fs` 布局。

## 5. Milestone M1 (`M1`)

- **M1-001 [Frozen]** M1 仅交付可重复的 hardware-status firmware、静态 core status UI、实验室配置和原始证据；不包含动态 ELF、`.mpb` parser/loader、plugin lifecycle、热更新或任何动态 plugin。
- **M1-002 [Frozen]** 启动日志 MUST 输出并由测试断言 target 为 `esp32s3`、双核、实际 Flash 为 16 MiB、实际 PSRAM 为 8 MiB，以及完整 ESP-IDF tag、toolchain version、firmware SHA-256、`board_id`、reset reason 和 PART-001 partition identity。任一不符即 M1 fail。
- **M1-003 [Frozen]** ST7305 MUST 以 HW-002/HW-003 的 10 MHz 基线完成 init、黑/白/checkerboard/边框图样和 static core status page；状态页至少显示 board、display、TF、RTC、SHTC3、KEY、Wi-Fi、logging 和 health。M1 MUST NOT 用 clock plugin/page 代替 core status UI。
- **M1-004 [Frozen]** PCF85063 MUST 完成读写及跨软件复位保持；SHTC3 MUST 返回 CRC 有效且物理合理的样本；KEY MUST 记录去抖后的 active-low press/release/short/long。normal mode 中 RTC/SHTC3 cadence 固定为 10 秒；仅明确标记、有限时长的 `selftest` 可更快采样并记录 `mode=selftest`。
- **M1-005 [Frozen]** Dedicated FAT32 TF mount 为 `/frame`。M1 MUST 覆盖四个明确转换：(1) boot without card -> `ABSENT`，(2) insert -> `HEALTH_CHECKING` -> `READY`，(3) remove from `READY` -> `ABSENT/FAILED`，(4) reinsert -> `HEALTH_CHECKING` -> `READY`。每次 READY health check 包含 write + `fsync` + read + SHA-256；任一阶段不得 panic、无限等待或阻塞 core UI/UART。
- **M1-006 [Frozen]** M1 soak MUST 连续一小时，以 normal 10 秒 cadence 更新 RTC/SHTC3/status，期间执行 KEY、Wi-Fi reconnect 和 TF 四转换脚本；不得发生 panic、WDT reset、heap integrity failure、status UI 停更或 UART logging 中断。TF 缺失窗口只允许 TF sink 暂停。
- **M1-007 [Frozen]** 当前唯一开发板不烧写 Flash Encryption 或 HMAC eFuse；M1 通过 UART 为每次启动配置 Wi-Fi STA，credentials 只驻留 RAM，断电/重启后必须丢失，且不得写入普通 NVS、TF、UART log 或 status。`wifi clear` 必须清零 RAM 配置。持久 encrypted NVS provisioning 在专用 security board 上验证前保持 Blocked。M1 event/measurement logs MUST 是逐行有效 JSONL，同时输出 UART，并在 TF READY 时写 `/frame/logs/`；每条含 monotonic sequence、uptime/UTC validity、source、event、mode 和 status。
  - 开发板例外 (2026-08)：经用户批准的例外决策（依据 SEC-010：NVS encryption key protection 依赖 Flash Encryption 或 HMAC eFuse 的不可逆 provisioning，单块开发板不可用），当前唯一开发板把最后一次 Wi-Fi credentials 以 PLAINTEXT 存于 internal `system_fs` 的 `/system/wifi.json`，替代本条 RAM-only 约束。该例外仅限本开发板；production board 出厂前 MUST 改用 NVS-encrypted credential storage（解除 SEC-010 Block 的前提下）。无论何种存储，password 永不出现于 UART、log 或 status。
- **M1-008 [Blocked]** M1 通过记录 MUST 包含 local evidence manifest、sdkconfig、partition table、linker map、`idf.py size-components`、heap/stack 指标、完整 UART JSONL、TF JSONL/hash、status page 照片、board/module 照片和全部输入/固件 SHA-256。在真实目标板完成并由 repository owner 记录 Pass 前为 Blocked/Not run；无需 GitHub approving review，官方示例也不构成 pass。
- **M1-009 [Frozen]** M1 的快速 RTC/SHTC3/KEY sample collection 只能在显式 selftest 中执行；退出 selftest 后 MUST 恢复 10 秒 cadence。早期 draft 中“一小时每秒采样/clock dirty update”的表述被 supersede。

## 6. UI And Clock Theme (`UI`)

- **UI-001 [Frozen]** Page order 固定为 core status page、core update page、随后是 registry 中可运行 plugin pages。Core 两页始终存在且不依赖 TF/plugin。`KEY` short press 切到 next page 并 wrap；long press 执行当前 page 声明的 page action，不用于切 theme。
- **UI-002 [Frozen]** 所有 page 使用逻辑 landscape `400x300` monochrome、高对比、white background，不依赖灰阶、透明或颜色语义。ST7305 driver 负责把 400x300 row-major 逻辑像素打包到面板原生存储顺序，不通过中间 portrait scene 旋转实现。Core firmware 内建 standard fonts 16/25/40；没有 `assets_fs` 或 loose core font file。
- **UI-003 [Frozen]** 一个时刻最多一个 plugin page generation 提交 active scene。只有 core `DisplayService` 可访问 ST7305/LVGL/U8g2 或底层 SPI；plugin 只能提交有界、不可变 monochrome display commands。Core status/update page 可在 plugin 停止时继续工作。
- **UI-004 [Frozen]** Clock main 使用 project-original flower/explosion contrast theme：white background 上显示 stylized `HH:mm`，禁止秒显示。其 dedicated resource-only MPB MUST 恰好提供 11 个所需 bitmap：数字 `0..9` 各 `48x80`，colon 一个 `20x80`；不得包含 copyrighted character assets 或第三方角色形象。
- **UI-005 [Frozen]** Clock 的下一行以 core standard font 显示 `YYYY-MM-DD weekday`；date/weekday MUST NOT 使用 stylized digit bitmap。其他状态信息属于 core status page，不挤入 clock main contract。
- **UI-006 [Frozen]** 时间优先级为有效 PCF85063 time -> 最近一次 core NTP 校时写回 PCF85063 -> 明确的 unset-time 状态。plugin 不直接设置 RTC 或发起 NTP；时区配置存于 `/system/config/time`，内部时间戳使用 UTC，normal sensor cadence 为 10 秒。
- **UI-007 [Frozen]** 高优 display command 从 accepted enqueue 到开始处理 p99 MUST <= 1 ms。已验证且 code/resource 均在 hot cache 的 page switch，从 KEY/event accepted 到目标 page 首次完整 commit 的 p99 MUST <= 500 ms。没有 60 fps、秒 tick、full-refresh 50 ms 或 command 5 ms 产品要求；早期 draft 中后两项被 supersede，而非待满足 SLA。
- **UI-008 [PoC-derived]** PoC-B 必须验证 UI-007 的两个 SLA，并把 full-refresh throughput、TE edge、dirty-region alignment、DMA size 和最高稳定 SPI clock 作为 measurement 报告；measurement 不自动成为 requirement，也不得用于放宽 1 ms/500 ms。
- **UI-009 [Frozen]** 无 TF 或 runtime TF loss 时零动态 plugin page；core status/update page、network、UpdateManager 和 UART 继续 degraded operation。Core status page显示 TF/plugin stop 原因，update page 可以检查并显示可用更新，但不得下载或安装不完整的 code/resource 组合；完整事务等待 TF READY 后由用户再次确认。
- **UI-010 [Frozen]** Startup 立即显示 core status page。Core 达到 `READY` 后仍保持 status 两秒；仅当 clock plugin 及其 current code/resource combination 已验证并成功 ACTIVE 时自动切到 clock main，否则保持 status。不得用超时后空白页或不完整 clock 代替。

## 7. Plugin Package And Update (`PLUG`)

- **PLUG-001 [Frozen]** MPB 是 signed strict single-type container。Code MPB MUST 含恰好一个 required Xtensa ESP32-S3 ELF payload且零 RESOURCE payload；resource-only MPB MUST 含一个或多个 RESOURCE payload 且零 ELF payload。Loader MUST 拒绝 mixed、empty、loose `.elf`、loose bitmap/font 或旁路 manifest。
- **PLUG-002 [Frozen]** Installed registry 是 plugin identity 到 current/previous-good code digest、current/previous-good resource digest（可为 none）及已验证 whole combination 的内部权威 mapping。启动 MUST 按 registry 解析 content-addressed objects，MUST NOT 通过扫描 `/sd/plugins`、`/frame/import` 或目录顺序发现 installed plugin。
- **PLUG-003 [Frozen]** Code MPB current/previous-good 位于 internal `/plugins/objects/sha256/<digest>.mpb`。Resource-only MPB current/previous-good 位于 dedicated FAT32 TF `/frame/objects/sha256/<digest>.mpb`。文件名 digest 必须等于完整 MPB SHA-256；registry 未引用的 object 不可执行。
- **PLUG-004 [Frozen]** Capacity target 为 8 个同时 ACTIVE instance、每个 plugin name 最多一个 ACTIVE generation。若 PoC resource budget 不能支持 8，可把 signed release limit 降到 7、6 或 5，但 MUST NOT 低于 5；若连 5 都不能满足，gate fail 并回到 architecture review。全系统最多一个 candidate generation 和一个 update transaction，UI 同时最多一个 active plugin page。
- **PLUG-005 [Frozen]** 热更新允许 target 的 old ACTIVE 与唯一 candidate generation 共存；candidate 完成 code/resource strict-type、signature/hash、whole-combination compatibility、epoch、ABI、依赖、内存预检、`prepare`、state import 和 staged activation 后，才能 durable commit 和原子切换 ingress。
- **PLUG-006 [Frozen]** 提交前任何失败 MUST 保持 old generation ACTIVE；提交后旧 generation 只有在 cancellation fence、`quiesce`、`unload` 和资源证明完成后才可释放。非合作代码进入 `FAILED_RESTART_REQUIRED`，不得 `vTaskDelete` 或回收仍可能被引用的代码。
- **PLUG-007 [Frozen]** 同一 `security_epoch` 可回滚到 registry 的 previous-good whole combination；code 和 resource version/digest 必须作为一组回滚，MUST NOT 把 current code 与不匹配的 previous resource 临时拼接。低于 eFuse floor 的包不可加载，candidate 必须重建网络状态。
- **PLUG-008 [Frozen]** 两种 `.mpb` parser 都 MUST 使用防溢出边界检查、拒绝 overlap/重复 required payload/未知 required field/错误 package type，并从同一 immutable verified staging buffer 安装或加载，消除 reopen path 造成的 TOCTOU。
- **PLUG-009 [PoC-derived]** `CONFIG_FRAME_MAX_MPB_BYTES`、总 plugin PSRAM budget、ELF relocation/import allowlist、动态 PSRAM text、`.plugin_iram` 和 1000 次 load/unload 稳定性由 PoC-A 决定。Capacity 先以 8 为目标，只有 signed PoC budget 可依次降至不低于 5。
- **PLUG-010 [Frozen]** TF 缺失时 MUST 全局激活零个 dynamic plugin，包括 registry 中 resource digest 为 none 的 plugin；internal code availability 不改变该 safety gate。Core UI、network、update 和 UART 必须继续 degraded operation。
- **PLUG-011 [Frozen]** Runtime TF loss MUST 关闭 plugin ingress 并按 lifecycle protocol 安全停止全部 plugin；非合作实例走 `FAILED_RESTART_REQUIRED`，重启后仍以零 plugin degraded mode 启动。TF reinsertion 先执行 filesystem/object/registry health check，再自动启动 eligible plugins。
- **PLUG-012 [Frozen]** Startup、reinsertion 和 runtime object validation 对每个 plugin 独立处理 broken resource object。先验证 current whole combination；失败时安全停止该 plugin 并尝试其 previous-good whole combination；两者都失败只 disable 该 plugin 并记录原因，不得停止其他健康 plugin。若 rollback 成功，registry 以 durable transaction 记录所选整组 digest。
- **PLUG-013 [Frozen]** `/frame/import/` 仅是 removable-media import ingress，`/frame/archive/` 保存已处理 import 的 content-addressed archive/result；二者都不是 installed discovery source。Import 和 authenticated network pull 均可提供 code MPB 或 resource-only MPB，验证后分别安装到 internal `/plugins` 或 TF `/frame` object store，并由内部 registry 事务提交。

## 8. Storage And Resource Location (`STOR`)

- **STOR-001 [Frozen]** Internal mount 只有 `/plugins` 和 `/system`，对应 PART-001 的 LittleFS；dedicated FAT32 TF mount 固定为 `/frame`。v4.2 没有 `/assets`、`assets_fs` 或 `/sd` mount，首版不依赖 exFAT。
- **STOR-002 [Frozen]** `/system` 保存 `/system/config/{ui,time,network}`、`/system/session`、`/system/plugin_registry`、`/system/update`、`/system/faults` 和容量受限的 `/system/logs`。Registry 是 current/previous-good code/resource whole-combination 的内部权威记录。Plugin 私有数据路由到 `/system/plugins/{instance-name}/` 并做 instance/generation 授权。
- **STOR-003 [Frozen]** `/plugins/objects/sha256/` 仅保存 signed code MPB；`/frame/objects/sha256/` 仅保存 signed resource-only MPB；`/frame/import/` 是 import ingress，`/frame/archive/` 是 import archive，`/frame/logs/` 是 TF JSONL/log sink。所有 object 使用 lowercase 64-hex complete-file SHA-256 content address。
- **STOR-004 [Frozen]** Core status/update pages、fonts 16/25/40 和最小 glyph/icon 编入 Secure Boot protected firmware。Plugin bitmaps/fonts/layout 只来自 signed resource-only MPB；不存在内部 product-theme assets partition 或 loose trusted resource path。
- **STOR-005 [Frozen]** SystemStorageWorker 与 SDStorageWorker MUST 独立；SD `ABSENT`、I/O hang 或 recovery 不得占用 `/system` queue/lock。SD 同步驱动调用未返回前不得并发 deinit；无法返回时走 WDT controlled restart。
- **STOR-006 [Frozen]** successful durable API 只有在 write、flush 和 `fsync` 成功后返回。Registry、update、session 和 fault record 使用 CRC + monotonic sequence 的 append journal 或 A/B slot；reboot recovery 必须幂等。
- **STOR-007 [Frozen]** `/system/logs` 默认 quota 为 256 KiB，按完整 batch 淘汰；不得淘汰 config、NVS keys、registry、update、session 或 fault journal。TF READY 时 JSONL 首选 `/frame/logs/`，TF 不可用时 UART 和 internal critical journal 继续。
- **STOR-008 [Frozen]** Content-addressed current 与 previous-good objects 在 registry durable commit 前不得 GC。GC 只能删除无任何 current/previous-good/candidate/update journal 引用的 object，并保存删除证据。
- **STOR-009 [Frozen]** TF health check 必须验证 FAT32 mount、`/frame` required directories、read/write/`fsync`、object filename/hash 和 registry references；只有通过后状态为 `READY` 并允许 plugin auto-start。

## 9. Security, Signing And Epoch (`SEC`)

- **SEC-001 [Frozen]** production MUST 使用 ESP32-S3 Secure Boot V2 和 Flash Encryption release mode；dev 不烧写不可逆 production eFuse，并使用独立 test key。
- **SEC-002 [Frozen]** plugin signing 使用独立于 Secure Boot 的单一长期 ECDSA P-256 release key；digest 为 SHA-256，signature encoding 为 64-byte IEEE P1363 `r||s`。受信 public key 编入 core firmware。
- **SEC-003 [Frozen]** production private key MUST NOT 出现在 repository、firmware image、普通 developer machine default、GitHub Actions secret、CI log 或 artifact。production signing 是受控 offline release step。
- **SEC-004 [Frozen]** Code 和 resource-only `.mpb` signed bytes MUST 绑定 package_type、fixed header、manifest、payload table；每个 payload 由 signed table 中的 SHA-256 绑定。验证顺序为 structure/single-type -> signature -> payload hash -> epoch/target/ABI/features -> ELF parse 或 resource schema parse。
- **SEC-005 [Frozen]** `security_epoch` floor 存于 eFuse monotonic bits。低于 floor 拒绝，同 epoch 允许 known-good rollback；只有 core UpdateManager 在 package 已验证、稳定供电确认和显式 release approval 后可提升，plugin 永远无此 capability。
- **SEC-006 [Frozen]** 制造记录 MUST 保存 epoch 编码、总 bit、剩余次数和 exhaustion policy。bit 耗尽不能通过换 key 或 firmware 恢复；设备停止接受需要更高 epoch 的 package 或按发布策略退役。
- **SEC-007 [Frozen]** native ELF 与 core 同地址空间，只信任项目签名但可能有缺陷的 plugin。Quota、opaque handle、generation 和 DI 不是恶意代码隔离；不允许宣传运行 untrusted third-party native code。
- **SEC-008 [PoC-derived]** production eFuse、Secure Boot、Flash Encryption、encrypted partition readback 和 epoch destructive cases 只能由保留的 spare board 在 PoC-E 验证。
- **SEC-009 [Blocked]** destructive security spare board 和可证明稳定供电/断电的 fixture 未到位，因此 SEC-008 和 ADR-0003 acceptance 被阻塞。
- **SEC-010 [Blocked]** ESP-IDF v6.0.2 在 ESP32-S3 上的 NVS encryption key protection 依赖 Flash Encryption 或 HMAC eFuse。两者都涉及不可逆 eFuse，因此当前唯一开发板只允许 RAM-only Wi-Fi credentials；持久 encrypted credential storage 必须等专用 security board，并在 PoC-E 前保持 Blocked。

## 10. Network And Update (`NET`)

- **NET-001 [Frozen]** product network mode 为 Wi-Fi STA。首次 credential provisioning 通过本地受信控制面完成；production credential 存于 encrypted internal storage，MUST NOT 写入 TF、UART log 或 plugin-visible path。当前唯一 dev board 根据 M1-007 只使用 RAM-only credential，不得以 plaintext NVS 代替。
- **NET-002 [Frozen]** NTP/TLS/update 由 core NetProxy/UpdateManager 执行，plugin 不获得 raw credential、trust store、eFuse 或 OTA partition capability。CA/trust metadata 位于 `/system/config/network`，更新必须受 core release policy 约束。
- **NET-003 [Frozen]** NTP 成功后 core 使用 UTC 校准 system clock 并写回 PCF85063；网络失败不得阻止离线 RTC clock、KEY、display 或 TF plugin 启动。
- **NET-004 [Frozen]** firmware OTA 使用 `ota_0`/`ota_1`，写 non-active slot，验证 ESP-IDF image/signature 后进入 pending-verify；boot self-test 成功才 confirm，否则 bootloader rollback。
- **NET-005 [Frozen]** Authenticated network pull 支持分别获取 code MPB 和 resource-only MPB。TF READY 时，下载先进入唯一 non-executable PSRAM staging，完整验证后 code 安装到 internal content-addressed `/plugins`，resource 安装到 TF content-addressed `/frame`；使用 temp + `fsync` + atomic rename + journal，最后更新内部 registry。TF 不可用时只允许检查发布清单，不得预下载、安装或提交任一 code/resource 部分；core update page 和 network control 继续工作。
- **NET-006 [Frozen]** update request 仅来自 local trusted control plane 或 authenticated release channel；UpdateManager 不进入 plugin ABI。TLS/download completion 不在 plugin callback 上执行。
- **NET-007 [PoC-derived]** Wi-Fi/TLS/OTA 与 display、storage、log 的组合峰值内存和 latency 由 PoC-B/PoC-E 测量。若 background OTA 不满足 UI-007，可显式进入 maintenance mode；不得宣称无扰动后台更新。

## 11. Memory And Quota (`MEM`)

- **MEM-001 [Frozen]** 所有容量模型以实际 8 MiB PSRAM 为上限。MUST 在 runtime admission 中先扣除 core、Wi-Fi/LwIP、TLS、display、filesystem、DMA、staging、old/new generation 共存和 safety margin，禁止保留 v4.1 的 11.5 MiB plugin quota。Capacity gate 目标为 8 ACTIVE，最低可接受为 5。
- **MEM-002 [Frozen]** plugin 每 generation 必须声明并执行 `max_memory_bytes`、`iram_required_bytes`、`strand_queue_capacity`、`max_outstanding_operations`、`max_timers`、`max_managed_tasks`、`managed_task_stack_size`、EventBus/display/storage/network/log/resource/update-backlog quotas。
- **MEM-003 [Frozen]** 每个 plugin generation 的 default PSRAM arena 为 256 KiB，manifest 可请求但 hard maximum 为 512 KiB。Code/data/generic/resource working memory 计入该 arena；internal SRAM、IRAM 和 DMA-capable memory 只通过受限 core service 分配，每个 allocation/handle 绑定 instance + generation。
- **MEM-004 [Frozen]** loader 在执行任何 candidate code 前解析 ELF Program Header/section/relocation，计算 segment alignment、BSS、loader metadata、stack、IRAM、staging 和 old/new coexistence；quota 或 largest contiguous block 不足时 fail closed。
- **MEM-005 [Frozen]** system 必须有独立 lifecycle/control reserve、completion reserve 和 core fault/log reserve，plugin user traffic 不得耗尽。Queue/pool full 同步返回 `FRAME_ERR_QUEUE_FULL` 或 `FRAME_ERR_CAPACITY`，失败时不接管 caller ownership。
- **MEM-006 [Frozen]** hot path metadata、FreeRTOS task stacks、queue control、SPI DMA buffer、ISR pool 和 critical core state 留在 internal memory；大 immutable buffer 和 generation arena 优先 PSRAM。不得使用标称 SRAM 容量推导可用 heap。
- **MEM-007 [PoC-derived]** `plugin_psram_budget_total`、safety margin、network/TLS pool、display buffer strategy、IRAM pool 和实际并发 limit 必须由 M1 与 PoC-A/B 的 peak/minimum/largest-block 证据冻结到 Accepted ADR 或 release configuration；不得改变 256 KiB default/512 KiB hard max，也不得把并发 limit 降到 5 以下。
- **MEM-008 [PoC-derived]** PoC-A 必须在 1000 次 load/update/unload 后证明无 leak 且 `largest_free_block` 无单调恶化；否则动态 native plugin gate fail。

## 12. Scheduler And Lifecycle (`LIFE`)

- **LIFE-001 [Frozen]** Core 0/1 各一个 fixed-affinity `plugin_io_context` Worker；每 instance 一个固定 core 的 FIFO strand。同 strand handler 不并发，每轮每 ready strand 最多执行一个 operation。
- **LIFE-002 [Frozen]** 首版只提供 asynchronous `post`，不提供 inline `dispatch/defer`。普通 handler MUST 合作式在 1 ms 内返回；overrun 记录 metric，不尝试抢占或强删调用栈。
- **LIFE-003 [Frozen]** plugin 不得直接 `xTaskCreate`。额外工作通过 quota-controlled managed task，持有 stop token/task token，priority 不高于 plugin Worker，并在 1 秒 shutdown deadline 内退出。
- **LIFE-004 [Frozen]** lifecycle 主状态为 `STAGED -> LOADING -> PREPARED -> ACTIVE -> QUIESCING -> UNLOADING -> UNLOADED`；热更新增加 `PAUSING/PAUSED`，不能安全推进时进入 `FAILED_RESTART_REQUIRED`。
- **LIFE-005 [Frozen]** 生命周期 entry 和 plugin callback 只在 instance strand 调用。Core service 捕获 strand + generation；async operation 返回 success 前预留 completion，success 后在当前 boot address space exactly once complete/destroy。
- **LIFE-006 [Frozen]** shutdown 先带外关闭 ingress 和 managed task，再对 EventBus、NetProxy、StorageService、steady_timer 执行 `cancel_generation`。四个 logical source ACK 后插入 `CANCELLATION_FENCE`；fence 前 completion/cleanup 全部在原 strand 结算。
- **LIFE-007 [Frozen]** 未开始的 ordinary POST 在原 strand 原位转为 CLEANUP 并恰好 destroy 一次。旧 generation callback、handle 和 resource MUST NOT 在新 generation 执行或释放新资源。
- **LIFE-008 [Frozen]** Display、Lifecycle、Update、SystemStorage、SDStorage、Net、Log 是独立 core Worker，不进入 plugin strand。一个卡死 plugin Worker 可阻塞同 core plugin，但关键 core Worker 必须保持到 controlled restart。
- **LIFE-009 [Frozen]** Interrupt WDT 直接走 panic/restart，不在 panic context 调 lifecycle/fsync；Task WDT 只有在锁定 ESP-IDF 证明 first-stage hook 安全时才走限时协调。不得以 `vTaskSuspend`、`vTaskDelete` 或强制 heap reclaim 恢复。
- **LIFE-010 [PoC-derived]** FIFO linearization、lost-wakeup、fairness、completion/cancel CAS、WDT attribution、PAUSE/RESUME、state schema migration 和 failure injection 由 PoC-A/PoC-C 验证后才可视为实现基线。
- **LIFE-011 [Frozen]** TF physical removal、unmount 或 media-level loss 是 global plugin stop trigger；单个 resource object 的 hash/schema/read failure 不是。Global stop 时 LifecycleManager 必须关闭全部 plugin ingress、停止 managed tasks、取消四类 async source、执行 fence/quiesce/unload；若不能安全完成则 controlled restart。Core status/update/network/UART 不参与该 stop，并保持服务。
- **LIFE-012 [Frozen]** TF reinsertion 只有在 STOR-009 health check 完成后才触发 registry reconciliation 和 plugin auto-start；每个 plugin 的 current/previous-good combination 独立决策，启动顺序仍服从 dependency DAG。

## 13. Testing And Evidence (`TEST`)

- **TEST-001 [Frozen]** 准入顺序严格为 M1、PoC-A、PoC-B、PoC-C、PoC-D、PoC-E、sign-off；某 gate 未通过时不得开始依赖该 gate 的下一阶段或 product component。
- **TEST-002 [Frozen]** 每次 board run MUST 在本地保存 raw evidence，并生成 local evidence manifest：requirement/gate ID、UTC timestamp、operator、board_id、硬件照片/SKU、ESP-IDF exact tag、toolchain、dependency commit、sdkconfig、partition table、linker map、binary/package SHA-256、test script commit、每个本地 evidence path/hash 和 Pass/Fail。当前不把 raw hardware evidence 上传 GitHub artifact。
- **TEST-003 [Frozen]** `PoC-A` 验证 loader/toolchain、C ABI、relocation allowlist、immutable staging、dynamic PSRAM text、`.plugin_iram`、cache sync、1000 次 lifecycle、old/new coexistence、scheduler FIFO/cancel。任一 required item fail 即 Gate A fail。
- **TEST-004 [Frozen]** `PoC-B` 在 ST7305 native monochrome 下验证 high-priority command-start p99 <= 1 ms 和 hot-cache switch p99 <= 500 ms，并叠加 Wi-Fi/TLS、TF I/O、4 KiB/s JSONL log 与 capacity target load。报告 p50/p99/p99.9/max/drop；full-refresh throughput 只报告 measurement，不设 50 ms requirement。
- **TEST-005 [Frozen]** `PoC-C` 注入 stuck handler、interrupt disabled、spinlock overrun、late callback、shutdown/update race、state schema mismatch 和 restart；验证 exactly-once、no unsafe reclaim、journal attribution 与同 epoch rollback。
- **TEST-006 [Frozen]** `PoC-D` 注入 boot no-TF、insert、runtime removal、reinsertion、broken single-plugin resource、CRC/command timeout、queue full、recovery fail 和 non-returning SD call；验证 global plugin stop、core degraded continuity、single-owner recovery、health-check auto-start、per-plugin disable 和 whole-combination rollback。
- **TEST-007 [Frozen]** `PoC-E` final evidence 恰为 1000 次真实 board power cut：六个 critical class 各 100 次，加 400 次跨完整流程的 seeded random cut。六类为 session BEGIN commit、session END commit、JSONL batch `fsync`/rotation、code MPB install + registry switch、resource MPB install + whole-combination registry switch、firmware OTA pending-verify/confirm。每类和 random set 均保存 seed/window/outcome；1000 是总数，不是每类数量。
- **TEST-008 [Frozen]** summary、照片或录屏不能替代 raw evidence；simulator/host test 不能替代标记为 board/power-cut/eFuse 的验收。证据缺文件、hash 不可重算或环境不可追溯均为 Fail。
- **TEST-009 [Blocked]** Current board 只允许最多 20 次 preliminary power cut，用于 fixture/script shake-down，且不计入 final 1000。Final 1000 和 destructive Secure Boot/Flash Encryption/eFuse evidence 必须在专用 spare board 上完成。Fixture、measurement method 和 spare board 当前未齐，不能以 `esp_restart()` 替代。

## 14. GitHub And CI (`GH`)

- **GH-001 [Frozen]** Repository 是个人账户下的 public GitHub repository，default/protected branch 为 `main`。合并必须经 PR、branch 必须与 `main` 最新状态一致且 required CI 全绿，但 required approving review count 为 0；不得要求非作者 approval。Admin/owner 同样 MUST NOT bypass branch rules 或 required checks。
- **GH-002 [Frozen]** 只允许 squash merge，禁用 merge commit 和 rebase merge。PR checklist MUST 要求 requirement ID、ADR、risk、exact test command、local evidence manifest digest（如适用）和 Pass/Fail/Blocked；硬件 raw evidence URL 不是必填项。
- **GH-003 [Frozen]** Strict CI MUST 在 `pull_request`、push to `main` 和 nightly schedule 运行适用 lane；执行 configure/build、C/C++ host tests、format/static analysis、两种 strict single-type `.mpb` negative/fuzz corpus、ABI/export/import/relocation policy、PART-001 exactness、Markdown internal-link 和 `git diff --check`。
- **GH-004 [Frozen]** 当前没有 self-hosted hardware runner，也没有 GitHub hardware lane。Cloud CI/mock MUST NOT 把 M1/PoC board gate 标绿；hardware result 只由本地运行及 TEST-002 local evidence manifest 表示为 Pass/Fail/Blocked，不上传 raw board evidence artifact。
- **GH-005 [Frozen]** GitHub Actions third-party action MUST pin full commit SHA，workflow 使用 least-privilege `permissions`，fork PR 不接触 secrets。CI artifact 不包含 Wi-Fi credential、production signing key、eFuse secret 或未脱敏 device data。
- **GH-006 [Frozen]** production `.mpb`/firmware signing 不在普通 GitHub-hosted runner 执行。CI 只产出 unsigned digest、SBOM、build provenance 和待签名 artifact；offline release 将签名结果和 digest 关联。
- **GH-007 [Frozen]** sign-off issue/release MUST 列出 GOV 至 GH 全部适用 ID、Accepted ADR、local PoC evidence manifest digest、known limitation、repository owner 和 timestamp。它是 owner gate record，不新增 approving-review requirement；只有关闭后才允许进入 product components。

## 15. Draft Exit Criteria

v4.2 退出 Draft 必须同时满足：

1. [ADR-0003](decisions/0003-waveshare-rlcd42-hardware.md) 与 Loader ADR 已 Accepted。
2. M1 和 PoC-A 至 PoC-E 全部有可重算 raw evidence 且结论为 Pass。
3. 所有 `Blocked` 条目解除；所有 `PoC-derived` 数值进入 Accepted ADR 或签署的 release configuration。
4. PART-001、security policy、no-TF 行为、UI SLA、quota 与 lifecycle contract 在实现、测试和本文之间一致。
5. GitHub owner sign-off 记录满足 GH-007；branch protection 保持零 approval requirement、strict CI 和 no admin bypass。
