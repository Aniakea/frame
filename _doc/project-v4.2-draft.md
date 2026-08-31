# Frame Architecture Delta v4.2

版本: `4.2-draft`

状态: Draft；未声称 M1 或任何 PoC 通过

本文是 [v4.1 历史架构](project.md) 的 concise delta。完整可验收约束以 [v4.2 requirements](requirements-v4.2.md) 为准；冲突顺序见 `GOV-001`。

## 1. Baseline Delta

v4.1 assumption | v4.2 replacement
--- | ---
通用 ESP32-S3，16 MiB PSRAM | Waveshare ESP32-S3-RLCD-4.2，`ESP32-S3-WROOM-1-N16R8`，16 MiB Flash、8 MiB PSRAM
300x400 `RGB565`、60 fps/frame-period contract | ST7305 物理 300x400、逻辑 landscape `400x300` monochrome；高优 command-start p99 <= 1 ms，hot-cache page switch p99 <= 500 ms，无秒显示或 60 fps 承诺
产品资源来自内部 `/assets`，code/resource 可混装 | 无 `assets_fs`；code 与 resource 是严格 single-type signed MPB，code current/previous-good 在内部 `/plugins`，resource current/previous-good 在 FAT32 TF `/frame`
从 `/sd/plugins` 扫描并执行包 | 内部 installed registry 是权威 mapping；`/frame/import` 只是 import ingress，`/frame/archive` 是 archive，另支持 authenticated network pull
TF 不可用时从 `/plugins/fallback.mpb` 加载动态 fallback | no-TF 时全局零动态插件，包括 no-resource plugin；core status/update UI、network、UpdateManager、UART 继续 degraded operation
11.5 MiB plugin quota 候选值 | 以实际 8 MiB PSRAM admission；每 generation arena default 256 KiB、hard max 512 KiB；目标 8 ACTIVE，PoC 只能降到不低于 5
完成五个 PoC 前仅笼统限制完整编码 | 文档/bootstrap -> M1 -> Loader ADR/PoC-A -> PoC-B..E -> sign-off -> product components 的强制 gate

## 2. Target Topology

```text
Secure Boot protected core firmware
  -> core status page + core update page + fonts 16/25/40
  -> DisplayService -> ST7305 / SPI3_HOST / monochrome framebuffer
  -> SystemStorageWorker
       -> /system/plugin_registry (authoritative mapping)
       -> /plugins/objects/sha256/<digest>.mpb (code only)
  -> SDStorageWorker -> FAT32 /frame
       -> objects/sha256/<digest>.mpb (resource only)
       -> import/ + archive/ + logs/
  -> registry-selected code/resource whole combination
       -> PluginLoader -> 256 KiB default / 512 KiB max generation arena
       -> per-instance strand -> typed core services -> plugin pages
```

Code MPB 恰好包含一个 required ELF 且不含 RESOURCE；resource-only MPB 含 RESOURCE 且不含 ELF。两类包必须先完成 package type、structure、ECDSA P-256 signature、payload SHA-256、epoch、target/ABI 和对应 ELF/resource policy 校验，再安装到 content-addressed object store。启动不扫描目录，而是解析内部 registry 的 current/previous-good whole combination。

TF 是所有 dynamic plugin 的全局运行前提，不是 code discovery source 或 trust root。Runtime physical/media loss 关闭全部 plugin ingress 并执行安全 lifecycle stop；core UI/network/update/UART 保持。Reinsertion 经 FAT32、object hash 和 registry reference health check 后自动启动。单个 resource object 损坏不扩大成 global stop：只停止受影响 plugin，current combination 失败时整组回滚 previous-good，仍失败则只 disable 该 plugin。

## 3. Display And Clock

显示安全启动配置固定为 `SPI3_HOST`、mode 0、10 MHz，以及 [ADR-0003](decisions/0003-waveshare-rlcd42-hardware.md) 的 pin map。UI 逻辑坐标为 landscape 400x300，ST7305 driver 负责原生打包。只有 DisplayService 拥有显示 API；UI plugin 提交 immutable monochrome commands。

Page order 是 core status、core update、plugin pages。`KEY` short 切 next page，long 执行 page action。Startup 先显示 status；core READY 后保留两秒，只有 clock whole combination 验证并 ACTIVE 才自动进入 clock，否则保持 status。

Clock main 在 white background 上使用 project-original flower/explosion contrast bitmap 显示 stylized `HH:mm`，下方用 core standard font 显示 `YYYY-MM-DD weekday`。Dedicated resource-only MPB 恰好含 10 个 `48x80` digit bitmap 和一个 `20x80` colon；不含 copyrighted character assets，不显示 seconds。

v4.2 用 [requirements-v4.2.md](requirements-v4.2.md) `UI-007` 替代 v4.1 RGB565/60 fps 指标：high-priority command-start p99 <= 1 ms，verified hot-cache page switch p99 <= 500 ms。Full-refresh latency、TE 和最高 SPI clock 仅是 PoC-B measurements；先前草案的 command 5 ms/full-refresh 50 ms 不是 requirement。

## 4. Memory And Lifecycle

结构仍采用 core Worker + per-core plugin Worker + per-instance strand，但所有 admission 以 8 MiB PSRAM 实测为边界。每 generation arena default 256 KiB、hard max 512 KiB。目标最多 8 个 ACTIVE instance；若 signed PoC budget 失败可依次降到 7、6 或 5，低于 5 即 gate fail。每次 admission 仍验证 core reserve、唯一 candidate、old/new generation、network/TLS、display、stack、IRAM、largest contiguous block 和 safety margin。

热更新维持 prepare/import/staged activation/durable commit/atomic ingress switch/cancellation fence。系统同一时刻只允许一个 candidate 和一个 update transaction；不能合作停止的 native plugin 不被强删或强制回收，而是进入 `FAILED_RESTART_REQUIRED` 并 controlled restart。

## 5. Storage And Partition

v4.2 固定完整 16 MiB layout：`nvs@0x9000/0x14000`、`nvs_keys@0x1D000/0x1000`、`otadata@0x1E000/0x2000`、`coredump@0x20000/0x20000`、`ota_0@0x40000/0x500000`、`ota_1@0x540000/0x500000`、`plugin_fs@0xA40000/0x4C0000`、`system_fs@0xF00000/0x100000`。精确表和用途见 `PART-001`；没有 `assets_fs`。

`plugin_fs` 是 4.75 MiB internal code object store，`system_fs` 是 1 MiB registry/config/journal store。Production Wi-Fi/UART config 使用 encrypted NVS；当前唯一 dev board 不烧不可逆 eFuse，因此 M1 credential 只驻留 RAM，持久 encrypted NVS 保持 Blocked。TF `/frame` 保存 resource object、import/archive 和 JSONL logs；TF 故障与 internal storage Worker 隔离。

## 6. Coding Gates

sign-off 前只允许：

- requirements/ADR/development plan 和 build/test bootstrap；
- M1 hardware-status firmware、静态 core status UI、TF 四转换、RAM-only UART Wi-Fi config 和 UART+TF JSONL；
- 可丢弃、隔离的 PoC-A 至 PoC-E harness；
- 支撑上述工作的 host tests、evidence tooling 和 CI policy。

sign-off 前禁止把 PoC 路径重命名或复制为 product PluginLoader、LifecycleManager、DisplayService、UpdateManager、storage/network service 或 product clock/theme plugin。严格顺序、每阶段输入和 exit criteria 见 [development-plan.md](development-plan.md)。

## 7. Unresolved By Design

以下内容不是 Draft 中暗含的成功结论：dynamic PSRAM execution、loader relocation allowlist、最高稳定 display clock、TE semantics、8-to-5 capacity limit、实际 plugin PSRAM/IRAM budget、UI SLA、TF recovery、power-cut durability、Secure Boot/Flash Encryption/eFuse destructive flow。它们分别由 M1、PoC-A 至 PoC-E 产出；失败时回到 architecture review。
