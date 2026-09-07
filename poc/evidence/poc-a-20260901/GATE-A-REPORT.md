# Gate-A Report — PoC-A Dynamic ELF (PENDING OWNER CONFIRMATION)

Gate: **A** (PoC-A: toolchain, dynamic ELF, scheduler, cancellation, memory behavior;
`_doc/development-plan.md` §4, `_doc/requirements-v4.2.md` TEST-003).
Evidence root: `poc/evidence/poc-a-20260901/` — run id `poc-a-20260901T015343Z`, manifest
verdict **PASS** (`manifest.json`, 67 artifacts, every sha256 recomputed at assembly;
`uv run frame-evidence` green).
Tree: worktree `poc-a-dynamic-elf` @ `f34dc72537912e9629dad482222e2159c98988ea`
(T1..T15 = 15 of 17 plan todos complete; T16 = this assembly; T17 = ADR backfill, not in scope here).
Environment: ESP-IDF **v6.0.2** exact tag, **xtensa-esp-elf gcc-15.2.0**
(`esp-15.2.0_20251204`), target esp32s3; loader = **espressif/elf_loader v1.3.3** vendored from
`espressif/esp-iot-solution` @ `6526c5b18e156cfbda2c7ce48e282e384c43b485` + local patches
**p1..p5** (`poc/apps/poc_a/components/elf_loader/PATCHES.md`; tree.sha256 offline
self-consistency re-verified at assembly — 26 upstream files).
Hardware: dev board USB id **c1810c76790e** (Waveshare ESP32-S3-RLCD-4.2, module
ESP32-S3-WROOM-1-**N16R8**: 16 MiB flash, 8 MiB octal PSRAM — same board as M1-20260831;
identity photos `poc/evidence/m1-20260831/photos/board_front_20260831.jpg`,
`board_back_module_20260831.jpg`, `status_page_20260831.jpg`; no new photos — display is
PoC-B scope).
Operator: orchestrator-session. Evidence window 2026-08-31T12:38:25Z → 2026-09-01T02:4xZ
(per-task session stamps inside each transcript).

Citation convention: `task-N-….log:LINE` refers to the verbatim transcript copies in this
directory (byte-identical to `.omo/evidence/poca/` originals; sha256 in `manifest.json`).
All line numbers are spot-checkable with `sed -n 'LINEp' task-N-….log`.

---

## (a) v4.2 traceability (draft `.omo/drafts/poc-a-dynamic-elf.md` table, Result filled)

Interpretation rule (GOV-001): v4.2 requirements and project-v4.2-draft govern; v4.1
(appendix A byte layout, §4.1 strand protocol text, appendix C compile rules) is base
spec only where v4.2 is silent.

| v4.2 authoritative clause | harness landing point | Result (evidence) |
|---|---|---|
| TEST-003 全项 | C1/C3/C4/C5/C6/C7/C8 item-by-item (Gate-A report maps each) | **PASS 10/10 clauses** — section (c) below |
| PLUG-001/008 strict single-type + anti-overflow/overlap/duplicate/unknown-field rejection + same staging buffer | C2 parser + C8 corpus (v4.1 appendix A layout consumed under single-type semantics) | **PASS** — host: MpbParser 28/28 incl. all negative families (`task-3-poc-a-dynamic-elf.log:158`); board: 14/14 tamper classes rejected pre-exec, both transports (`task-14-poc-a-dynamic-elf.log:2792`); mixed-payload both directions rejected (embedded union + `neg_mixed_*` uploads) |
| PLUG-004 single candidate + single transaction | C6 coexistence test = exactly 1 old ACTIVE + 1 candidate | **PASS** — `[PASS-single-candidate-guard]` third load → FRAME_ERR_BUSY before allocation, canary unchanged (`task-8-poc-a-dynamic-elf.log:243`) |
| PLUG-009 + MEM-007 PoC-derived output freeze | Gate-A report output list: CONFIG_FRAME_MAX_MPB_BYTES, total PSRAM budget, relocation/import allowlist set, capacity 8→N lowest pass → ADR-0002 amendment | **DELIVERED** — section (d) below + `poc/apps/poc_a/plugins/ALLOWLIST.md` + `ADMISSION-BUDGET.md` (signed capacity line); ADR-0002 backfill is T17 |
| MEM-001 8MiB admission (minus core/network/display/fs/DMA/staging/coexistence/headroom) | C6 measured the harness baseline before the ladder; harness runs no Wi-Fi (max plugin budget), product admission re-measured at PoC-B | **PASS with frozen caveat** — baseline psram free 8385920 B measured fresh-boot (`task-8-poc-a-dynamic-elf.log:21` context; `ADMISSION-BUDGET.md` MEM-001 section); Wi-Fi-off caveat recorded (section (f)) |
| MEM-003 arena 256KiB default / 512KiB hard cap | test plugins max_memory_bytes ≤256KiB; hard cap enforced by manifest_builder | **PASS** — builder refuses >512 KiB declarations (T2 pytest, `task-2-poc-a-dynamic-elf.log:24`); device-side `neg_maxmem` (hostile 600 KiB, signature-valid) rejected pre-init canary unchanged + `baseline_300k` (300 KiB) loads/activates/unloads clean (`ADMISSION-BUDGET.md` rules table) |
| MEM-008 1000 cycles no monotonic largest-block degradation | C5 judge script with trend regression | **PASS 18/18** — `SOAK VERDICT: PASS (18/18 criteria)` (`task-9-poc-a-dynamic-elf.log:1264`, repro judge `:2168`); decline 0.00 %, psram min_ever delta 0 B; raw samples `poc/apps/poc_a/plugins/soak_samples_1000.csv` |
| LIFE-001/002 post-only, 1ms cooperation, overrun counted not preempted | C7 prototype implements overrun counter and is tested | **PASS** — host suite incl. strand FIFO/no-concurrency/dup-detector 24/24 ×3 (`task-11-12-poc-a-dynamic-elf.log:450,504,558`); on-target `SCHED CONSISTENCY host==target : PASS` (`task-13-poc-a-dynamic-elf.log:988`) |
| LIFE-005 reserve-completion-before-success + current-boot exactly-once | C7 CAS tests cover reserve-before-success | **PASS** — completion/cancel CAS vectors both outcomes at full count on board (`task-13-poc-a-dynamic-elf.log:988` consistency row `completion`); TSan clean host-side (`task-11-12-….log:504`) |
| LIFE-007 un-executed POST in-place CLEANUP exactly-once destroy | C7 QUIESCING transition tests | **PASS** — board vector `quiesce` (16 cleanups + one §4.1.4 revival delivery on a STOPPED strand, destroyed=17) in the consistency table (`task-13-poc-a-dynamic-elf.log:988`; markers `[PASS-quiesce]` `:297`, `[PASS-completion]` `:310`) |
| SEC-002/004 P-256/P1363/verification order (structure→signature→hash→epoch/target/ABI→ELF) | C2/C8 strict order, negative proof order is not bypassable | **PASS** — order proof `[FC-ORDER-DOUBLE-FAULT]`: bad-sig+bad-hash reports -17 SIGNATURE_INVALID first (`task-14-poc-a-dynamic-elf.log:1216`); builder round-trip + resource-only self-verify (`task-2-poc-a-dynamic-elf.log:135,142`) |
| SEC-005 epoch floor (eFuse at PoC-E) | dev uses compile-time virtual floor constant, recorded as test strategy | **PASS (dev policy)** — epoch floor = 1 (section (d)); `neg_epoch` class rejected pre-exec on board (`task-14-poc-a-dynamic-elf.log:2792` 14/14 incl. epoch) |
| GH-003 CI must include mpb negative/fuzz corpus + ABI/export/import/relocation policy checks | new host corpus/policy CI lane | **PASS** — `mpb-corpus-gates` job in ci.yml (job list `task-15-poc-a-dynamic-elf.log:4`): 28 ctest corpus cases + regen determinism + policy pytest 37; lane-actually-red negative control verified (`task-15-….log:104`) |
| TEST-002 evidence field-complete manifest | C9 manifest per field (M1 system reused) | **PASS** — `manifest.json` in this dir (gate A; frame-evidence green; board_id, UTC stamps, operator, IDF tag/toolchain/loader commit+patches, dependency commit, sdkconfig, partition table, linker map, firmware + all-.mpb sha256, per-evidence path+hash, verdict) |
| project-v4.2-draft §2 topology + §6 coding gates | harness pipeline = staging→verify→loader→arena→strand→entry table; PoC code not promoted to product components | **PASS** — staging→verify→relocate→entry chain proven on board (`task-4-poc-a-dynamic-elf.log:18-38`); loader entry called ON a worker task via strand (`task-13-…:988` `loader` vector, activate at 0x420612f0); all PoC code under `poc/`+`tools/`+ci.yml (T15/F4 scope audit) |
| development-plan §4 external blocker check | loader candidate availability cleared (ADR-0002 matrix); "lock IDF support for dynamic executable PSRAM / cache sync / WDT hook" = first verification of C1/C4 | **PASS** — vendored loader pinned (upstream fetch+verbatim-verify `task-1-poc-a-dynamic-elf.log:7,35`, tree.sha256 `:66`); PSRAM exec proven (`task-4-…:23`); cache sync observable (sync counter `>0=PASS`, `task-9-poc-a-dynamic-elf.log:534`); 1000-cycle REPL hold + 305 s soak with zero TWDT (`task-9-…:1115`) |

## (b) development-plan §4 — nine exact acceptance items

| # | Item (§4 "Exact acceptance") | Todo(s) | Verdict | Evidence pointer |
|---|---|---|---|---|
| 1 | Fixed loader repository URL, full commit SHA, license, bundled licenses, all local patch hashes | T1 (+T5/T6/T7 for later patches) | **PASS** | PATCHES.md: URL espressif/esp-iot-solution, commit `6526c5b18e156cfbda2c7ce48e282e384c43b485`, Apache-2.0 (license.txt sha in PATCHES.md), patches p1..p5 each with diff + post-patch tree.sha256; verbatim vendor proof `task-1-….log:7,35,66`; per-file shas re-verified at assembly (manifest notes) |
| 2 | Fixed ESP-IDF/toolchain/C++ mode, ELF class/machine/section/Program-Header/relocation/import/export allowlists; positive/negative test per entry | T5 + T14 | **PASS** | IDF v6.0.2 + gcc-15.2.0 pinned (this report header); reloc allowlist pos+neg on board `task-5-….log:396` (FULL MATRIX PASS 3 pos + 7 neg, canary unchanged), GLOB_DAT pointer-equality `:102`, `neg_r32`/`neg_s0op` byte-patched type flips rejected; import closed-world `-ENOSYS` (`ALLOWLIST.md` §2); phdr policy `neg_phspan` pre-load reject `task-5-….log:646`; ELF identity (class/data/machine) loader-side p2 `neg_phbe/ph64/phmach` (`ALLOWLIST.md` §3) + T14 class-14 uploads (`task-14-….log:2792`) |
| 3 | From immutable verified staging bytes: function-pointer call, dynamic PSRAM `.text`, `.plugin_iram` copy/relocate/call/free, cache synchronization | T4 + T7 | **PASS** | fp call + PSRAM text first run `task-4-….log:23-32` (addr in PSRAM window, consumed 3324 B, MAGIC MATCH, 10/10 cycles `:38`); `.plugin_iram` p5: 100-cycle load/prove/unload SOAK PASS `task-7-….log:758-769`, IRAM watermark delta +0 (`:509,541`), budget-consistency negative `iram_mismatch` → -EINVAL pre-allocation (`:386`); cache sync counter advances per IRAM load (`task-9-….log:534`, counter=11>0) |
| 4 | `.init_array`/ctor/dtor/TLS/exception/RTTI/unwind: support-or-reject with executable evidence; unknown items fail before execute | T6 | **PASS (all six REJECTED)** | REJECT-MATRIX.md (6 variants, layers L1+L2); pre-p4 crash proof `task-6-….log:2023` (LoadProhibited — the fail-open gap); post-p4 board rejections `:2474,2489`; full matrix PASS 3 pos + 10 neg canary unchanged `:2554`; TLS reloc-layer isolation via `neg_tls_nosect` |
| 5 | Code MPB and resource-only MPB strict single-type positive/negative corpus; mixed payload rejected before execute or install | T2/T3/T14 | **PASS** | resource-only round-trip build+parse+self-verify `task-2-….log:135,142`; host corpus 28/28 `task-3-….log:158`; board: mixed-both-directions among the 14/14 pre-exec rejections `task-14-….log:2792` (fixtures `neg_mixed_elf_in_res`/`neg_mixed_res_in_elf`, sha in manifest) |
| 6 | 1000× load/prepare/activate/quiesce/unload: no leak, crash, or monotonic largest-block degradation; raw heap/IRAM/stack samples kept | T9 | **PASS 18/18** | 1000-cycle DONE `task-9-….log:1115` (+second judge-reproduced run `:1637,2168`); verdict `:1264`; decline 0.00 %, min_ever delta 0 B psram / 88 B iram, integrity 20/20, 1030/1030 value matches; raw samples committed `poc/apps/poc_a/plugins/soak_samples_1000.csv` (101 rows) |
| 7 | Old ACTIVE + sole candidate pass admission on 8 MiB PSRAM; default arena 256 KiB, hard max 512 KiB; FIFO/lost-wakeup/fairness/completion-cancel tests pass | T8 + T13 (host T11/T12) | **PASS** | 60/60 old-gen calls during candidate staging, independent arenas, both generations answer, swap clean `task-8-….log:239-278` (summary `:21`); measured peak 6776 B; scheduler: host TSan 24/24 ×3 `task-11-12-….log:450,504,558`, board 3×10 vectors ALL GREEN `task-13-….log:573,946`, host==target consistency `:988` |
| 8 | Capacity 8 ACTIVE first; on failure descend 7/6/5, lowest pass into signed budget; 5 fails → Gate A fail | T10 | **PASS at 8 (descent unexercised)** | rung-8 attempt-1 `[PASS-capacity-8]` `task-10-….log:226` (load/respond/unload `:195,212,223`), per-instance 3356 B `:150`, 8-peak 26848 B `:213`, ×3 identical runs; signed budget line frozen in `ADMISSION-BUDGET.md`; honest note: rungs 7/6/5 + retry path never ran on hardware (section (f)) |
| 9 | Failed packages, wrong relocations, hash/signature/package-type mismatches execute zero payload bytes | T14 | **PASS (zero bytes executed)** | 14/14 classes rejected pre-exec on BOTH transports (36 uploaded attacker fixtures + 24 embedded negatives per 2-round run) `task-14-….log:2792-2794`; execution sentinel: canary advances only on the per-round positive control (+1), +0 across all negatives `:2790,5519,5566`; verification-order proof `:1216` |

## (c) TEST-003 — ten clauses (requirements-v4.2.md:174)

| # | TEST-003 clause | Verdict | Evidence |
|---|---|---|---|
| 1 | loader/toolchain | **PASS** | vendored elf_loader 1.3.3@6526c5b1 verified byte-verbatim (`task-1-….log:35`), tree.sha256 (`:66`), CI lane `firmware-poc-a` (`:72`); IDF v6.0.2/gcc-15.2.0; assembly rebuild 0 warnings (manifest note) |
| 2 | C ABI | **PASS** | entry table query→prepare→activate→unload round-trip with magic+CRC checks (`task-4-….log:32,38`); host ABI contract suite 52/52 (`task-4-….log:43`) |
| 3 | relocation allowlist | **PASS** | full matrix 3 pos + 7 neg, canary unchanged (`task-5-….log:396`); frozen set = types {0,2,3,4,5} (`ALLOWLIST.md` §1) |
| 4 | immutable staging | **PASS** | loads parse/verify from the immutable staging copy, never a file path (`task-4-….log:18-31`); T14 re-hashes uploaded bytes on device and asserts == committed file sha before parsing (upload-integrity closed loop, `task-14-….log` [FC-HOST] lines) |
| 5 | dynamic PSRAM text | **PASS** | `[PASS-load-psram-addr]` + per-load heap accounting (`task-4-….log:23-31`); executed pointers observed in both the 0x3C.. data window and the +0x06000000 0x42.. instruction mirror (T4/T8 logs) |
| 6 | `.plugin_iram` | **PASS** | patch p5; on-device value-from-IRAM proof + window assert + 100-cycle no-leak soak (`task-7-….log:758-769`); manifest/section budget equality enforced both directions (`:386`) |
| 7 | cache sync | **PASS** | sync counter increments exactly once per IRAM load, observed >0 on every session (`task-9-….log:534`); full-writeback + cross-core guard convention justified in PATCHES.md p5 |
| 8 | 1000-cycle lifecycle | **PASS** | `task-9-….log:1115,1264` (18/18 criteria; second run `:1637`; judge reproducibility `:2168`) |
| 9 | old/new coexistence | **PASS** | 6/6 transcript markers ×2 runs (`task-8-….log:21,239-278`); PLUG-004 single-candidate BUSY guard included |
| 10 | scheduler FIFO/cancel | **PASS** | host TSan-clean suite (`task-11-12-….log:450-558`) + on-target 10 vectors incl. strict ABABABAB fairness and both CAS outcomes, 3× stability ALL GREEN, host==target consistency PASS (`task-13-….log:573,946,988`) |

## (d) PoC-derived frozen outputs (PLUG-009 / MEM-007 — for ADR-0002 via T17)

| Frozen output | Value | Basis |
|---|---|---|
| `CONFIG_FRAME_MAX_MPB_BYTES` (PoC policy) | **512 KiB (524288 B) hard max**; default arena 256 KiB, 256–512 KiB requestable | enforced twice: `manifest_builder.py` refuses to package larger declarations (T2 pytest) AND device admission rejects signature-valid hostile 600 KiB declarations pre-init (`neg_maxmem`, `ADMISSION-BUDGET.md`) |
| PSRAM cost per instance | **3356 B** (capacity class: staging 1984 B + loader arena 1352 B); baseline-class 3388 B (longer manifest name) | measured ×3 identical ladder runs (`task-10-….log:150`); cross-checked by the 0xd1c text-base stride between instances |
| 8-instance peak consumption | **26848 B = 0.32 % of the 8 MiB pool**; largest_free_block unchanged (8257536 B); psram exactly restored after unload | `task-10-….log:213,226`; `ADMISSION-BUDGET.md` capacity table |
| Capacity value | **8 ACTIVE generations per distinct plugin name** (PASS at 8, attempt 1; MEM-007 floor 5 never reached) | signed budget line in `ADMISSION-BUDGET.md`; proposal value for ADR-0002 |
| Relocation/import allowlist (full set) | dynamic reloc types **{R_XTENSA_NONE=0, RTLD=2, GLOB_DAT=3, JMP_SLOT=4, RELATIVE=5}**; closed-world imports (declared+registered only); program-header budget admission | frozen in `poc/apps/poc_a/plugins/ALLOWLIST.md` (positives value-asserted, negatives byte-patched-and-rejected; link-flag contract §4) |
| Epoch floor | **virtual floor = 1** (compile-time constant, dev test policy per SEC-005) | real eFuse floor deferred to PoC-E (plan); `neg_epoch` rollback class rejected on board (`task-14-….log:2792`) |
| MEMPROT trade-off | `CONFIG_ESP_SYSTEM_MEMPROT=n` required for the IDF exec heap (`MALLOC_CAP_EXEC`) that `.plugin_iram` needs | software-only PMP config, **no eFuse burned**; documented in PATCHES.md p5 + `sdkconfig.defaults` comment — production must re-decide (security vs plugin IRAM) |

ADR-0002 amendment note (carried for T17): worst-case 8 × (512 KiB arena + 512 KiB container)
≈ 8 MiB ≱ pool → the product admission controller must gate the **global sum** of resident
declarations, not only the per-plugin hard max (`ADMISSION-BUDGET.md` capacity section).

## (e) Upstream loader defects found and patched (Gate-A highlights)

All registered in `PATCHES.md` with diff + sha256; each was **proven on the board** before its fix:

1. **p1 — fail-open relocation (crash/fail-open class)**: upstream `esp_elf_relocate()` ignored
   `esp_elf_arch_relocate()`'s `-EINVAL`; out-of-allowlist relocation types only logged and the
   image kept loading with unrelocated words. Fixed: error propagates, load aborts. Board proof:
   `neg_r32`/`neg_s0op` rejected (`task-5-….log:396`).
2. **p2 — no ELF identity validation (fail-open class)**: without magic/class/data/machine checks
   an ELF64-/BE-/ARM-marked payload was **loaded and executed** on the S3 mirror path. Fixed:
   `-EINVAL` before any header-driven access. Board proof: `neg_phbe/ph64/phmach` + T14 class 14.
3. **p3 — rounded `.text` window swallows `.rodata` (crash class)**: upstream
   `ELF_ALIGN(size,4)` made the map_sym window cover the first `.rodata` vaddr(s) → data pointer
   mirrored into the exec alias → **LoadStoreError on target** (observed `task-5-….log:110`,
   diagnosis `:394`). Fixed: record the true section size.
4. **p4 — forbidden C++ feature sections ignored (crash class)**: a signed `.ctors` probe (only
   allowlisted relocs) wrote through `esp_elf_map_sym()==0` → **Guru Meditation LoadProhibited,
   reboot** (pre-p4 board run `task-6-….log:2023`) — a fail-open DoS, not fail-before-execute.
   Fixed: section-name prefix scan pre-allocation, `-EINVAL` (`:2474,2489`).
5. **p5 — `.plugin_iram` support (feature patch)**: IRAM section scan/alloc/copy/relocate/free +
   budget-consistency hook + observable cache-sync counter; includes the S3 word-access-only
   IRAM copy (byte stores fault — first board run crash, learnings T7) and the
   `esp_elf_print_sec()` undercount drive-by fix.

## (f) Honest limitations (owner should read before confirming)

1. **Capacity descent path 7-5 unexercised**: rung 8 passed first attempt, so rungs 7/6/5 and the
   retry-once logic never ran on hardware; that fail-closed code exists only in the committed
   runner (`capacity_ladder.py`) and is untested against a real OOM (`task-10` issues note,
   `ADMISSION-BUDGET.md`).
2. **No Wi-Fi in the harness (MEM-001 caveat)**: all PSRAM/IRAM budget numbers are harness-class
   (radio off). Product admission budgets must be re-measured with the network stack active
   (next gate per plan).
3. **On-target scheduler vectors are a subset of the host suite** (T13 consistency table): board
   node pool 256-node credit + QUEUE_FULL retry vs host 31000-node pool; storm 2e4 posts vs host
   1e5; the host-vs-target delta list is pinned in `poca_sched.hh` (SMP spinlock correction etc.).
   The consistency declaration covers the vectors actually run (`task-13-….log:988`).
4. **Quiesce is a no-op by ABI semantics**: the plugin ABI 1.2 entry table has no quiesce slot;
   the NULL entry is defined as trivial success (frozen in `poca_plugin.cc` + the soak banner
   `task-9-….log:365`; 1000-cycle SOAKHDR `:883`). Real quiesce coordination is later-gate work.
5. **Display/screen unused**: no new photos for Gate-A (PoC-B scope); hardware identity rides on
   the M1 photo set (manifest notes).
6. **C++ matrix board rows**: only v1 (ctor) and v3 (TLS) have on-board corpus rows; v2/v4/v6
   share p4's proven scan mechanism and v5's loader layer is the `-ENOSYS` import path proven by
   T5 `import_neg`; all six have verbatim L1 gate-error evidence (`REJECT-MATRIX.md` coverage note).
7. **Binary provenance**: the fresh assembly rebuild (map/size artifacts, bin sha in manifest
   notes) is from the identical tracked tree; each board session ran its task-era build and its
   per-artifact sha256 lines appear verbatim inside the transcripts (per-.mpb device-printed ==
   build-time hash, closed loop).

## (g) Verdict

```
GATE-A: PASS (all nine development-plan §4 items evidenced; TEST-003 10/10 clauses;
frame-evidence green; 67-artifact manifest, every sha256 recomputed at assembly)
— pending owner sign-off below. T17 records the ADR-0002 backfill and does NOT sign this.
```

### PENDING OWNER CONFIRMATION (mirror of the M1-008 owner-record pattern)

Per GOV-001/005/006: this report and `manifest.json` assemble the repository evidence for the
Gate-A owner decision; per development-plan §4 the ADR can only move to Accepted after required
evidence passes **and** the owner records the decision. Until then Gate-A stays unsigned.

```
Repository Owner: Aniakea
Decision:         Gate-A Pass recorded (owner confirmed in work session, 2026-09-07 UTC)
Date:             2026-09-07 (UTC)
Notes:            read section (f) limitations before confirming; capacity value 8 and the
                  global-sum admission note are PoC-derived proposals for ADR-0002 (T17).
```

## Raw evidence index

- `manifest.json` — full 67-artifact hash index (14 transcripts + fresh map/size outputs +
  in-repo frozen references: `ALLOWLIST.md`, `REJECT-MATRIX.md`, `ADMISSION-BUDGET.md`,
  `soak_samples_1000.csv`, `PATCHES.md`, `tree.sha256`, 33 committed `.mpb` corpus containers,
  runner/generator scripts, TEST-ONLY key pair).
- `task-1…task-15-poc-a-dynamic-elf.log` — verbatim per-task board/host transcripts
  (TEST-008); rerun commands per task in `.omo/notepads/poc-a-dynamic-elf/issues.md`.
- `build/frame_poc_a.map`, `build/frame_poc_a_size_components.txt` — fresh assembly-time build.
- Board session live-capture conventions (RTS pulse, quiesce prompt detect, per-command
  deadlines) are documented in the runner scripts themselves (`plugins/*_runner.py`).
