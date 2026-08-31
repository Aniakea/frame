# v4.2 Development Plan

状态: Draft execution plan；严格 gate，未声明任何阶段通过

规范输入: [requirements-v4.2.md](requirements-v4.2.md)、[project-v4.2-draft.md](project-v4.2-draft.md)、[ADR-0003](decisions/0003-waveshare-rlcd42-hardware.md)、[ADR-0002](decisions/0002-elf-loader.md)

## 1. Sequence And Stop Rule

执行顺序固定为：

```text
D0 docs/bootstrap
  -> M1 hardware status
  -> L1 Loader ADR + PoC-A
  -> PoC-B
  -> PoC-C
  -> PoC-D
  -> PoC-E
  -> S1 architecture sign-off
  -> P1 product components
```

前一阶段 exit criteria 未全部满足时，下一阶段状态只能是 `Blocked`。允许提前采购设备和做无代码的实验排期；不得提前实现依赖未通过 gate 的产品组件。任何 required test 失败都停止序列并进入 architecture review，不自动选择替代 loader、静态 plugin 或降低 SLA。

## 2. D0: Docs And Bootstrap

范围:

- 冻结 v4.2 requirements、architecture delta、ADR 和本计划，保留 v4.1 历史文档。
- 建立 reproducible ESP-IDF/toolchain bootstrap、dev/prod configuration separation、host-test harness 和 evidence schema。
- 建立 strict single-type MPB、partition exactness、Markdown internal link、format/static、host test 和 `git diff --check` 的 CI checks。
- 仅建立 PoC/M1 所需的目录和 reusable test utilities；不建立 product service skeleton 来绕过 coding gate。

Exit criteria:

1. `GOV-001` 权威顺序、stable requirement IDs 和 status vocabulary 经 review。
2. Clean checkout 可用 pinned ESP-IDF/toolchain configure；输出 exact version 和 SHA-256。
3. Host bootstrap tests 全绿，evidence schema 能拒绝缺少 board_id、commit、config 或 hash 的记录。
4. PART-001 的 `nvs/nvs_keys/otadata/coredump/ota_0/ota_1/plugin_fs/system_fs` exact table 有 machine check，明确拒绝 `assets_fs`；当前实现 gap 未被误报为 pass。
5. Personal public GitHub repo 的 `main` 规则为 PR + strict required CI + squash-only + zero required approvals + admin no bypass；PR、push main、nightly triggers 和 no-production-key policy 可检查。

External blockers:

- Personal public GitHub repository 的 branch-rules/required-check 管理权限。
- 可固定并长期获取的 ESP-IDF release tag 和 matching toolchain。

## 3. M1: Hardware Status

范围严格限于 `M1-001`：hardware-status firmware、static core status UI、lab scripts 和 local raw evidence；没有 dynamic plugin。

Exact acceptance:

1. 启动自动断言 ESP32-S3 dual core、16 MiB Flash、8 MiB PSRAM 和 PART-001 identity，并输出 ESP-IDF/toolchain/firmware hash、board_id、reset reason。
2. `SPI3_HOST` mode 0、10 MHz、指定 pin 驱动 ST7305 physical 300x400 / logical landscape 400x300；黑、白、checkerboard、1-pixel border 和 static core status page 全部正确。
3. PCF85063 read/write/reset-retention、SHTC3 CRC-valid reading、KEY active-low press/release/short/long 通过；normal cadence 为 10 秒，快速样本只出现在显式 selftest。
4. FAT32 TF mount `/frame` 完成四转换：boot absent、insert to READY、runtime remove、reinsert to READY；每次 READY 通过 write + fsync + read + SHA-256 health check。
5. UART command 为当前启动配置/清除 Wi-Fi STA；credential 只驻留 RAM 且所有输出脱敏，重启后丢失；encrypted NVS 持久化等待专用 security board。
6. UART 和 TF `/frame/logs/` 同时产生逐行可解析 JSONL；TF 缺失时 UART 继续且 TF sink 恢复后生成连续、有 sequence 的新记录。
7. 一小时 soak 使用 normal 10 秒 RTC/SHTC3 cadence，执行 TF 四转换、KEY 和 Wi-Fi reconnect；无 panic、WDT、heap corruption、status UI/UART 停更。
8. `M1-008` local evidence manifest 及其 sdkconfig、partition、map、size、heap、stack、JSONL、photo、hash 齐全，由 repository owner 记录 Pass；不要求 approving review。

不属于 M1:

- 不选择或实现 ELF loader。
- 不构建 `.mpb` parser、plugin ABI、hot update 或 product clock plugin。
- 不构建 clock page，不以 static status page 测值宣称 PoC-B SLA pass。

External blockers:

- 至少一块目标 Waveshare ESP32-S3-RLCD-4.2 N16R8、数据 USB cable、指定 FAT32 TF。
- 可确认 pin waveform/TE 的 logic analyzer，以及可拍摄显示和模组丝印的设备。
- 若 primary target board 尚不可用，M1 保持 Blocked；模拟器和其他 ESP32-S3 board 不可替代。

## 4. L1: Loader ADR And PoC-A

顺序要求：M1 sign-off 后才能给 loader candidate 加入目标板 runtime evidence。更新 [ADR-0002](decisions/0002-elf-loader.md) 的 candidate matrix，先评审选择规则，再运行 PoC-A；ADR 只有全部 required evidence 通过后才能 Accepted。

Exact acceptance:

1. 固定 loader repository URL、full commit SHA、license、bundled licenses 和全部 local patch hash。
2. 固定 ESP-IDF/toolchain/C++ mode、ELF class/machine、section、Program Header、relocation、import/export allowlist；每项有 positive/negative test。
3. 从 immutable verified staging bytes 完成 function pointer 调用、dynamic PSRAM `.text`、`.plugin_iram` copy/relocate/call/free 和 cache synchronization。
4. `.init_array`、constructor/destructor、TLS、exception、RTTI、unwind 的支持或拒绝均有可执行证据；未知项目 fail before execute。
5. Code MPB 与 resource-only MPB strict single-type positive/negative corpus 通过；mixed payload 在执行或安装前拒绝。
6. 1000 次 load/prepare/activate/quiesce/unload，无 leak、crash 或 largest-block 单调恶化；保存 raw heap/IRAM/stack samples。
7. Old ACTIVE + 唯一 candidate 在 8 MiB PSRAM 上通过 admission；default arena 256 KiB、hard max 512 KiB；FIFO/lost-wakeup/fairness/completion-cancel tests 通过。
8. Capacity 先验证 8 ACTIVE；失败可依次验证 7/6/5 并把最低通过值写入 signed budget，5 失败则 Gate A fail。
9. 失败包、错误 relocation、hash/signature/package type mismatch 不执行任何 payload byte。

External blockers:

- Loader candidate maintainer/source availability 和明确 license。
- 锁定 ESP-IDF 对 dynamic executable PSRAM、cache sync 和 WDT hook 的真实支持。
- 若无 candidate 满足全部项，ADR-0002 保持 Blocked，序列停止；不在本 gate 内临时开发 product loader。

## 5. PoC-B: ST7305 UI SLA

开始条件：PoC-A pass；ADR-0002 Accepted；[ADR-0003](decisions/0003-waveshare-rlcd42-hardware.md) 已解除 fixture/spare-board/identity blockers 并 Accepted。

Exact acceptance:

1. 使用 logical landscape 400x300 packed monochrome，并由 driver 转换到 ST7305 原生列顺序；不运行 RGB565 compatibility benchmark 作为产品指标。
2. 10 MHz baseline 下采集 high-priority command-start 和 verified hot-cache page switch 各不少于 10000 samples、至少 10 分钟。
3. Command-start p99 <= 1 ms，hot-cache switch p99 <= 500 ms；报告 p50/p99/p99.9/max/drop，不删除 outlier。
4. Full 15000-byte refresh、TE、DMA、dirty-region alignment 和更高 SPI frequency 只报告 measurement；不存在 50 ms full-refresh requirement。
5. 叠加 Wi-Fi transfer、TLS handshake、TF read/write、4 KiB/s JSONL logging 和 signed capacity limit 的持续 ready strands/pages。
6. 验证 page order、KEY short next、long action、startup status -> READY two-second hold -> working clock，以及 clock 不可用时保持 status。
7. 验证 clock white-background original flower/explosion theme、`HH:mm`、standard-font `YYYY-MM-DD weekday`、11 bitmap exact dimensions、无 seconds/character assets。

External blockers:

- Logic analyzer channel/bandwidth、可重复 combined-load network endpoint、稳定供电。
- ADR-0003 未 Accepted 时 PoC-B 不得开始正式计数。

## 6. PoC-C: Fault, Lifecycle And Rollback

Exact acceptance:

1. 注入 infinite handler、finite wait failure、interrupt disable、spinlock >1 ms、late generation callback、queue exhaustion 和 shutdown race。
2. 合作实例在 1 秒内退出，accepted async callback exactly once，CLEANUP/destroy 和 resource release exactly once。
3. 非合作路径不调用 `vTaskDelete/vTaskSuspend`、plugin unload 或 unsafe reclaim；写 fault journal/RTC best effort 后 controlled restart。
4. 重启后隔离 fault generation，并激活同 `security_epoch` known-good package。
5. 覆盖 prepare/import/activate/PAUSING failure、backlog overflow、state schema mismatch、stateless/stateful migration 和 commit boundary recovery。
6. 独立 core Worker 在 plugin fault 后保持运行到 restart；raw trace 可归因 instance/generation/operation。

External blockers:

- 锁定 ESP-IDF Task WDT first-stage notification 能力；不安全时只能验收 panic/restart profile，不得伪称 cooperative recovery。
- 可自动复位、采集 boot-to-boot log 的 lab runner。

## 7. PoC-D: TF Failure And Storage Isolation

Exact acceptance:

1. 注入 boot no-TF、insert、runtime removal、reinsertion、broken individual resource、CRC error、command timeout、queue full、mount/recovery fail 和 non-returning driver call。
2. No-TF 时全局零 dynamic plugin，包括 no-resource plugin；core status/update UI、network、UpdateManager、UART 和 `/system` requests 继续 degraded operation。
3. Runtime removal 触发所有 plugin 安全 lifecycle stop；非合作实例 controlled restart，且 reboot 后仍不从 internal code store 启动。
4. TF recovery 只由 SDStorageWorker 在 in-flight driver call 返回后执行 close -> unmount -> deinit -> init -> mount -> `/frame` health check；trace 中无 concurrent deinit。
5. Reinsertion health check 后按 internal registry 自动启动；current whole combination 失败则尝试 previous-good whole combination，禁止 code/resource cross-mix。
6. 启动、重插或运行期发现单个 plugin current resource 损坏时只停止该 plugin 并尝试 previous-good whole combination；previous 也失败才 disable，其他 plugin 继续，rollback/disable result durable 记录。
7. `/frame/import` 与 `/frame/archive` 不作为 discovery source；import 和 network pull 都经 strict type/hash/signature 验证及 registry transaction。
8. Accepted request 在当前 boot exactly-once complete；driver 永不返回时 WDT restart，不声称 timer 可取消同步 I/O。

External blockers:

- 可控 TF removal/fault injection adapter、至少一种固定 model/capacity TF 和可复现实验介质错误的方法。

## 8. PoC-E: Durability And Production Security

Exact acceptance:

1. Current board 最多执行 20 次 preliminary cuts，只 shake down fixture/script，不计入 final evidence。
2. Spare board 完成 final 1000 total cuts：session BEGIN、session END、JSONL batch fsync/rotation、code MPB install + registry、resource MPB install + whole-combination registry、firmware OTA pending/confirm 六类各 100，加 400 seeded random cuts。
3. 返回 success 的 durable record 每次 reboot 可恢复；DURING loss 不超过 1 秒或 4 KiB；journal 无伪 commit、错误 whole combination、CRC-valid torn record 或不可解释 state。
4. malformed/overlap/mixed-type/TLV/signature/hash/key_id/target/ABI/epoch corpus 全部在 ELF execute/resource commit 前拒绝。
5. Spare board 实证 Secure Boot V2、Flash Encryption release mode、encrypted NVS/data readback、低 epoch reject、同 epoch rollback 和 epoch exhaustion policy。
6. Firmware A/B pending-verify self-test、confirm 和 rollback 全路径通过；OTA/update 对 UI SLA 的影响有 maintenance-mode 决策证据。
7. Production private key 不进入 repository、GitHub Actions、log 或 artifact；offline signing provenance 可关联 unsigned digest 和最终 image/package hash。

External blockers:

- 能真正切断 board input power 的 programmable fixture、voltage measurement 和独立 fixture firmware/config revision。
- 一块永久标识的 destructive security/eFuse spare board。
- 受控 offline signing environment 和 production-like test keys。

当前这些条件未完成，因此 PoC-E 和最终 sign-off 保持 Blocked；`esp_restart()`、USB reset 或只切 peripheral power 均不能替代。

## 9. S1: Architecture Sign-Off

Exact acceptance:

1. M1、PoC-A 至 PoC-E 全部 Pass，raw evidence hash 可重算，所有外部 blocker 清零。
2. ADR-0002、ADR-0003 和 gate 中产生的其他 normative ADR 均 Accepted；Accepted 内容与 requirements 无冲突。
3. 全部 `PoC-derived` 数值已进入 Accepted ADR 或 signed release configuration；Draft 中无被实现默认为真的 TBD。
4. GOV 至 GH requirement traceability review 完成，partition/no-TF/signing/epoch/UI SLA/quota/lifecycle 与 tests 一致。
5. GitHub owner sign-off issue/release 列出 local evidence manifest digest、accepted ADR、known limitations 和 timestamp；不要求 approving review。

任何 exception 必须先形成 ADR 并重新运行受影响 gate；会议口头批准不算 exit。

## 10. P1: Product Components

只有 S1 complete 后才开始。推荐依赖顺序：

1. Public C ABI、error/status、instance/generation identity、bounded pools 和 metrics。
2. PluginScheduler strand/timer/managed task 与 WatchdogManager。
3. DisplayService、LifecycleManager、SystemStorageWorker、SDStorageWorker、NetProxy、LogGateway、UpdateManager core Workers。
4. EventBus、MemoryService generation arena、ResourceLease 和 storage/network service tables。
5. `.mpb` parser/signature/epoch policy、PluginLoader 和 transactional hot update。
6. Product clock plugin、strict signed TF resource-only MPB、host/board regression suite。
7. Production provisioning、offline signing、OTA release 和 manufacturing evidence。

每个 component PR 必须引用稳定 requirement ID，执行 host + applicable hardware regression，并证明没有扩大 plugin trust/capability。PoC code 只有经过 normal PR scrutiny、license audit、tests 和需求追踪后才能复用；branch 仍为 zero required approvals，PoC pass 本身也不是 production quality approval。
