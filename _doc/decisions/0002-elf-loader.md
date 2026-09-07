# ADR-0002: Dynamic ELF Loader Selection

Status: Accepted (2026-09-01)

Per development-plan §4（"ADR 只有全部 required evidence 通过后才能 Accepted"）：全部 required evidence 已按 [GATE-A-REPORT](../../poc/evidence/poc-a-20260901/GATE-A-REPORT.md) 通过（`GATE-A: PASS`，nine §4 items + TEST-003 10/10）。Owner sign-off of the report's PENDING OWNER CONFIRMATION block completes the record（与 M1-008 owner-record pattern 同构，见 `poc/evidence/m1-20260831/`）；在该 sign-off 落笔前 Gate-A 视为 unsigned。

## Context

The architecture requires signed `.mpb` packages containing Xtensa ESP32-S3 ELF payloads whose verified bytes are loaded into PSRAM/IRAM. No loader or commit is selected. PoC-A must compare candidates before implementation details become product dependencies.

## Candidate Evaluation

Candidate | Repository | Commit | License | ESP-IDF v6.0.2 | Xtensa relocations | Dynamic PSRAM text | `.plugin_iram` | Cache synchronization | Result
--- | --- | --- | --- | --- | --- | --- | --- | --- | ---
espressif/elf_loader | [esp-iot-solution/components/elf_loader](https://github.com/espressif/esp-iot-solution/tree/master/components/elf_loader) + registry `espressif/elf_loader` | v1.3.3 (`6526c5b1`, 2026-07-28) | Apache-2.0 | ≥4.4.3; v6.x picolibc fixed in v1.3.2 (`ff8c8663`) — use ≥1.3.2 | `R_XTENSA_RELATIVE`/`RTLD`/`GLOB_DAT`/`JMP_SLOT` only; others `-EINVAL`（硬编码 allowlist，与 project_so 链接参数耦合） | ✅ S3 via I/D-bus mirror offset（`SOC_IROM_LOW`−`SOC_DROM_LOW`），`heap_caps` SPIRAM 8BIT | ❌ 无 per-section IRAM（仅 internal EXEC fallback） | full `Cache_WriteBack_All` + `spi_flash_disable_interrupts_caches_and_other_cpu`（无 `esp_cache_msync`；v1.3.3 因 unaligned faults 在 S3 上移除 ranged API） | Primary — **PoC-A PASS 2026-09-01**（GATE-A-REPORT: nine items evidenced; 1000-cycle soak 18/18; capacity 8/8（PASS at rung 8, attempt 1）; 14-class fail-closed zero-bytes-executed; 4 upstream defects found + fixed p1–p4 + `.plugin_iram` feature patch p5, per [PATCHES.md](../../poc/apps/poc_a/components/elf_loader/PATCHES.md) @`887d0f7` with diff + tree.sha256）
igrr/hotreload | [igrr/hotreload](https://github.com/igrr/hotreload)（registry `igrr/hotreload`） | 0.10.0 (`4acb68d`, 2026-02-25) | MIT | idf ≥5.0；v6 untested | `RELATIVE`/`32`/`JMP_SLOT`/`PLT` + 完整 `SLOT0_OP` operand patcher（CLAUDE.md 声称支持 `ASM_EXPAND` 但 switch 中缺失 — discrepancy） | ✅ S3 PSRAM；`esp_cache_msync` `DIR_C2M\|UNALIGNED` w/ fallback（最干净的参考实现） | ❌ | `esp_cache_msync` | Not evaluated on target — reference/patterns role confirmed（esp_cache_msync usage、SLOT0_OP patcher；未集成）
Self-built minimal route | n/a（IDF v6.0.2 natives：`esp_mmu_map` `MMU_MEM_CAP_EXEC` + `esp_cache_msync` + S3 bus-mirror） | n/a | project | full control | 自选 allowlist（在插件链接参数受约束的前提下，elf_loader 的 4-reloc 集是最小可行集；`SLOT0_OP` family 是难点） | ✅ `esp_mmu_map` per-arena | ✅ `esp_cache_msync` aligned | `esp_cache_msync` | Not needed (contingency) — Gate A PASS 使其无需启用（Decision Rule 仍不授权 self-built fallback）

Candidate Priority (2026-08-31)：Primary = espressif/elf_loader ≥v1.3.3（pending PoC-A）；Secondary = igrr/hotreload，仅作为 pattern/reference 来源（esp_cache_msync 用法、SLOT0_OP patcher），不直接集成；Contingency = self-built minimal route，仅在 Gate A 判定"无 candidate 通过"并回到架构评审后才可考虑（见 Decision Rule：本 ADR 不授权 automatic fallback 或新的 loader 实现）。

PoC-A outcome (2026-09-01)：Primary 通过 Gate-A（见 matrix Result 与 PoC-A Frozen Outputs 节）；Secondary 的 reference/patterns 角色经评审确认（msync 模式用于 p5 的 cache-sync 设计参照）；Contingency 未启用。

### espressif/elf_loader — Evaluation Evidence (2026-08-31)

核心 API 为 buffer 模型：`esp_elf_relocate(elf, buf)` 直接从已验证的 staging buffer 加载，其上的 dlopen 风格 layer（可选组件）建议保持关闭，以满足 immutable staging buffer 约束。未解析 import 默认 hard-fail `-ENOSYS`；v1.3.1 起可用 `elf_set_symbol_resolver` 提供 host-side 覆盖。PSRAM arena 为 per-instance（`esp_elf_init`/`esp_elf_deinit` 配对），支持多实例隔离。无 `.init_array` 处理——采用 entry-point 模型，构造函数语义需在 PoC-A 按 Required Evidence 单独裁决。已知 open issues（影响 PoC-A 观察）：#776（2026-08-30，`.so` 调用崩溃）、#686（`.got.loc`）、#497（symbol tables 仅子集）、#560（C++ 支持）。

Citations:

- Component tree: https://github.com/espressif/esp-iot-solution/tree/master/components/elf_loader
- Component registry: https://components.espressif.com/components/espressif/elf_loader
- v1.3.3 commit: https://github.com/espressif/esp-iot-solution/commit/6526c5b1 ; picolibc/v6 fix v1.3.2: https://github.com/espressif/esp-iot-solution/commit/ff8c8663
- Issues: [#776](https://github.com/espressif/esp-iot-solution/issues/776), [#686](https://github.com/espressif/esp-iot-solution/issues/686), [#497](https://github.com/espressif/esp-iot-solution/issues/497), [#560](https://github.com/espressif/esp-iot-solution/issues/560)

### igrr/hotreload — Evaluation Evidence (2026-08-31)

作者 Ivan Grokhotkov。定位是 dev-time 指针交换式 reload（single-generation），非生产级多代共存模型。提供 `hotreload_load_from_buffer`（对 PoC-A 的 immutable staging 有参考价值）。已知限制：unload 后旧 stub 悬挂（dangling stubs）；未解析的 `JMP_SLOT`/`PLT` 仅告警（是否 hard-fail 未验证——与 Frame 的 fail-before-execute 要求可能冲突）；固定地址 symbol table 导致固件更新后插件必须重建。价值在于 patterns：`esp_cache_msync` 的正确用法（本 matrix 中最干净的参考）与 `SLOT0_OP` operand patcher 实现。

Citations:

- Repository: https://github.com/igrr/hotreload
- Component registry: https://components.espressif.com/components/igrr/hotreload
- 0.10.0 commit: https://github.com/igrr/hotreload/commit/4acb68d

### Self-built minimal route — Evaluation Evidence (2026-08-31)

ESP-IDF v6.0.2 树内没有 dynamic loader（espcoredump 仅含私有 `elf.h` struct 定义，无 relocate 能力）；self-built 即在 IDF natives（`esp_mmu_map` + `esp_cache_msync` + S3 bus-mirror）之上重写 relocation/映射/同步层。`R_XTENSA_REGISTER` 并非真实存在的 dynamic relocation（GitHub 全域 0 命中；windowed-register 语义实际编码在 `SLOT*_OP` 系列中，是 self-built 路线的主要难点）。Canonical prior art 为 glibc xtensa `dl-machine.h`。

Citations:

- IDF v6.0.2 memory management (`esp_mmu_map`): https://docs.espressif.com/projects/esp-idf/en/v6.0.2/api-reference/system/mm.html
- IDF v6.0.2 cache (`esp_cache_msync`): https://docs.espressif.com/projects/esp-idf/en/v6.0.2/api-reference/system/cache.html
- glibc xtensa prior art: https://sourceware.org/git/glibc.git/tree/sysdeps/xtensa/dl-machine.h

### Correction Log (2026-08)

- 仓库 `espressif/esp-elf-loader` 不存在（GitHub 404）；elf_loader 组件实际位于 `espressif/esp-iot-solution` 仓库 `components/elf_loader` 目录并以 registry `espressif/elf_loader` 发布。
- "ARMAnt" 全网无结果，不是真实 candidate。
- `R_XTENSA_REGISTER` 不是真实的 dynamic relocation（见 self-built evidence）。

### Selection Rule Review (2026-08)

现有 Decision Rule（必须满足全部 required PoC-A item、仅加载 immutable verified staging buffer、无 automatic fallback）经评审后 **UNCHANGED**。新增一条 clarification：elf_loader 的窄 relocation allowlist 隐含插件必须经其 `project_so` pipeline（或等价的受约束链接参数）构建——该耦合作为 PoC-A probe item（TEST-003 relocation allowlist）验证，不构成规则变更。

## Required Evidence

- Exact repository URL, commit, license, and local patches
- Accepted and rejected ELF class, machine, section, relocation, and import types
- Program-header and relocation arithmetic with overflow checks
- Function-pointer calls from dynamically allocated PSRAM
- `.plugin_iram` copy, relocation, invocation, and release
- Instruction/data cache synchronization APIs for the locked ESP-IDF tag
- `.init_array`, static constructor/destructor, TLS, exception, RTTI, and unwind behavior
- One thousand load/unload cycles with raw heap and largest-block samples

## PoC-A Frozen Outputs (2026-09-01)

PLUG-009 / MEM-007 PoC-derived 值由 Gate-A 冻结（来源：[GATE-A-REPORT](../../poc/evidence/poc-a-20260901/GATE-A-REPORT.md) section (d)）。按 GOV-001 顺序，本 Accepted ADR 中的冻结值高于 v4.2 仍标 `PoC-derived` 的条目（后者保持原值待 sign-off，不在本次回填中改写）。Product admission 需在 PoC-B 以 Wi-Fi 激活态复测（harness 无线电关闭，MEM-001 caveat）。

| Frozen output | Value | Basis / evidence |
| --- | --- | --- |
| `CONFIG_FRAME_MAX_MPB_BYTES`（PoC policy） | **512 KiB（524288 B）hard max**；default arena 256 KiB，256–512 KiB requestable | 双重强制：`manifest_builder.py` 拒绝打包更大声明（T2 pytest）+ 板上 admission 对签名有效的 600 KiB 敌意声明 pre-init 拒绝（`neg_maxmem`） |
| PSRAM cost per instance | **3356 B**（capacity class：staging 1984 B + loader arena 1352 B）；baseline class 3388 B（manifest 名更长） | ×3 次 identical ladder runs 实测；text-base stride 交叉验证 |
| 8-instance peak consumption | **26848 B = 0.32 % of 8 MiB pool**；largest_free_block 不变（8257536 B）；unload 后 PSRAM 精确复原 | capacity rung 8 attempt 1，×3 runs |
| Capacity value | **8 ACTIVE per distinct plugin name**（MEM-007 floor 5 从未触达；下降路径 7/6/5 未在硬件运行——honest limitation (f)1） | signed budget line（[ADMISSION-BUDGET.md](../../poc/apps/poc_a/plugins/ADMISSION-BUDGET.md)） |
| Relocation + import allowlist | dynamic reloc types **{`R_XTENSA_NONE`=0, `RTLD`=2, `GLOB_DAT`=3, `JMP_SLOT`=4, `RELATIVE`=5}**；closed-world imports（declared + registered only，否则 `-ENOSYS`）；program-header budget admission | 全集冻结于 [ALLOWLIST.md](../../poc/apps/poc_a/plugins/ALLOWLIST.md)（正例 value-asserted、负例 byte-patched-and-rejected、link-flag contract） |
| C++ feature policy | 全六项 REJECT before execute：static ctor/`.init_array`、dtor/`fini_array`、dynamic TLS、`-fexceptions`、RTTI、unwind tables（pipeline gate + loader section-scan 双层） | [REJECT-MATRIX.md](../../poc/apps/poc_a/plugins/REJECT-MATRIX.md)（每特性拒绝层 + 证据引用） |
| Epoch floor | **virtual floor = 1**（编译期常量，SEC-005 dev test policy）；真实 eFuse floor 延后至 PoC-E | `neg_epoch` rollback 类板上 pre-exec 拒绝（task-14） |
| MEMPROT trade-off | `CONFIG_ESP_SYSTEM_MEMPROT=n`（`.plugin_iram` 所需的 IDF exec heap `MALLOC_CAP_EXEC` 的前提）；software-only PMP config，**未烧 eFuse**；production 必须重新裁决 security vs plugin IRAM | [PATCHES.md](../../poc/apps/poc_a/components/elf_loader/PATCHES.md) p5 + `sdkconfig.defaults` 注释 |
| Local patches | **p1–p5**，each with diff + post-patch `tree.sha256`（最终 manifest sha256 `3c7706cb…49a81`，26 upstream files） | [PATCHES.md](../../poc/apps/poc_a/components/elf_loader/PATCHES.md) @`887d0f7`；assembly 时离线自洽复验 |

ADR amendment（承 GATE-A-REPORT (d) note）：worst-case 8 ×（512 KiB arena + 512 KiB container）≈ 8 MiB ≱ pool —— product admission controller 必须对驻留声明的 **global sum** 设闸，而非仅 per-plugin hard max。

### PoC-A Evidence Links (2026-09-01)

- [GATE-A-REPORT.md](../../poc/evidence/poc-a-20260901/GATE-A-REPORT.md) — verdict `GATE-A: PASS`（nine §4 items、TEST-003 10/10、§(e) defect 清单、§(f) limitations、PENDING OWNER block）
- [manifest.json](../../poc/evidence/poc-a-20260901/manifest.json) — 67-artifact hash index（gate A；`uv run frame-evidence` green；每 sha256 assembly 时重算）
- Key transcripts（同目录 verbatim 副本，sha256 见 manifest）：[task-4 first PSRAM execution](../../poc/evidence/poc-a-20260901/task-4-poc-a-dynamic-elf.log)、[task-5 relocation matrix](../../poc/evidence/poc-a-20260901/task-5-poc-a-dynamic-elf.log)、[task-7 .plugin_iram patch](../../poc/evidence/poc-a-20260901/task-7-poc-a-dynamic-elf.log)、[task-8 coexistence/admission](../../poc/evidence/poc-a-20260901/task-8-poc-a-dynamic-elf.log)、[task-9 1000-cycle soak 18/18](../../poc/evidence/poc-a-20260901/task-9-poc-a-dynamic-elf.log)、[task-10 capacity 8](../../poc/evidence/poc-a-20260901/task-10-poc-a-dynamic-elf.log)、[task-13 scheduler host==target](../../poc/evidence/poc-a-20260901/task-13-poc-a-dynamic-elf.log)、[task-14 fail-closed 14/14](../../poc/evidence/poc-a-20260901/task-14-poc-a-dynamic-elf.log)

## Decision Rule

The selected candidate must satisfy every required PoC-A item without loading bytes other than the immutable, verified staging buffer. If no candidate passes, Gate A fails and the architecture returns to review; this ADR does not authorize an automatic fallback or a new loader implementation.

## Gate Impact

PoC-A 于 2026-09-01 通过 Gate-A（GATE-A-REPORT PENDING owner sign-off 仍开放）；本 ADR Accepted 后，development-plan §5 的 "ADR-0002 Accepted" 前置条件在证据上已满足（以该 sign-off 落笔为准）。Product PluginLoader 实现本身仍受 GOV-004 约束：S1 architecture sign-off 前不得把 PoC 代码提升为产品组件。

Selected: espressif/elf_loader v1.3.3 vendored + patches p1–p5（`poc/apps/poc_a/components/elf_loader/`）；hotreload confirmed as pattern source only；self-built contingency not needed.
