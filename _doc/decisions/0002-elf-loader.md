# ADR-0002: Dynamic ELF Loader Selection

Status: Blocked

## Context

The architecture requires signed `.mpb` packages containing Xtensa ESP32-S3 ELF payloads whose verified bytes are loaded into PSRAM/IRAM. No loader or commit is selected. PoC-A must compare candidates before implementation details become product dependencies.

## Candidate Evaluation

Candidate | Repository | Commit | License | ESP-IDF v6.0.2 | Xtensa relocations | Dynamic PSRAM text | `.plugin_iram` | Cache synchronization | Result
--- | --- | --- | --- | --- | --- | --- | --- | --- | ---
espressif/elf_loader | [esp-iot-solution/components/elf_loader](https://github.com/espressif/esp-iot-solution/tree/master/components/elf_loader) + registry `espressif/elf_loader` | v1.3.3 (`6526c5b1`, 2026-07-28) | Apache-2.0 | ≥4.4.3; v6.x picolibc fixed in v1.3.2 (`ff8c8663`) — use ≥1.3.2 | `R_XTENSA_RELATIVE`/`RTLD`/`GLOB_DAT`/`JMP_SLOT` only; others `-EINVAL`（硬编码 allowlist，与 project_so 链接参数耦合） | ✅ S3 via I/D-bus mirror offset（`SOC_IROM_LOW`−`SOC_DROM_LOW`），`heap_caps` SPIRAM 8BIT | ❌ 无 per-section IRAM（仅 internal EXEC fallback） | full `Cache_WriteBack_All` + `spi_flash_disable_interrupts_caches_and_other_cpu`（无 `esp_cache_msync`；v1.3.3 因 unaligned faults 在 S3 上移除 ranged API） | Primary — pending PoC-A
igrr/hotreload | [igrr/hotreload](https://github.com/igrr/hotreload)（registry `igrr/hotreload`） | 0.10.0 (`4acb68d`, 2026-02-25) | MIT | idf ≥5.0；v6 untested | `RELATIVE`/`32`/`JMP_SLOT`/`PLT` + 完整 `SLOT0_OP` operand patcher（CLAUDE.md 声称支持 `ASM_EXPAND` 但 switch 中缺失 — discrepancy） | ✅ S3 PSRAM；`esp_cache_msync` `DIR_C2M\|UNALIGNED` w/ fallback（最干净的参考实现） | ❌ | `esp_cache_msync` | Secondary — reference/patterns（single-generation、experimental、v6 untested）
Self-built minimal route | n/a（IDF v6.0.2 natives：`esp_mmu_map` `MMU_MEM_CAP_EXEC` + `esp_cache_msync` + S3 bus-mirror） | n/a | project | full control | 自选 allowlist（在插件链接参数受约束的前提下，elf_loader 的 4-reloc 集是最小可行集；`SLOT0_OP` family 是难点） | ✅ `esp_mmu_map` per-arena | ✅ `esp_cache_msync` aligned | `esp_cache_msync` | Contingency — ~600-1000 LOC security-sensitive

Candidate Priority (2026-08-31)：Primary = espressif/elf_loader ≥v1.3.3（pending PoC-A）；Secondary = igrr/hotreload，仅作为 pattern/reference 来源（esp_cache_msync 用法、SLOT0_OP patcher），不直接集成；Contingency = self-built minimal route，仅在 Gate A 判定"无 candidate 通过"并回到架构评审后才可考虑（见 Decision Rule：本 ADR 不授权 automatic fallback 或新的 loader 实现）。

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

## Decision Rule

The selected candidate must satisfy every required PoC-A item without loading bytes other than the immutable, verified staging buffer. If no candidate passes, Gate A fails and the architecture returns to review; this ADR does not authorize an automatic fallback or a new loader implementation.

## Gate Impact

PoC-A cannot pass and product PluginLoader work cannot begin while this ADR remains Blocked.

PoC-A primary candidate = espressif/elf_loader v1.3.3+; hotreload reserved as pattern source; self-built is contingency only.
