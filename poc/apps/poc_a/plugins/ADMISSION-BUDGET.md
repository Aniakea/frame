# ADMISSION-BUDGET — PoC-A measured admission baseline (T8 + T10 capacity, frozen Gate-A output)

Development-plan §4 item 7 / appendix D "hot-update peak" row (T8) and §4
item 8 capacity ladder (T10). All numbers are measured on the
ESP32-S3-RLCD-4.2 (N16R8) board over `/dev/ttyACM0`, poc_a harness at
commit range T1..T10 (elf_loader 1.3.3 vendored + patches p1..p5, plugin
ABI 1.2). Raw transcripts: `.omo/evidence/poca/task-8-poc-a-dynamic-elf.log`
(`poca coexist` phase snapshots and `poca status` lines) and
`.omo/evidence/poca/task-10-poc-a-dynamic-elf.log` (capacity ladder).

## MEM-001 caveat (frozen scope)

The poc_a harness boots **without Wi-Fi/BLE**: the PSRAM/IRAM/free figures
below are a *harness* baseline, not a product baseline. Product admission
budgets must be re-measured with the radio stack active (next gate, per
plan). No PoC-derived value below is a product sign-off.

## Admission rules enforced (MEM-003 / PLUG-004)

| rule | enforced where | on-board proof |
| --- | --- | --- |
| manifest `max_memory_bytes` ≤ 512 KiB hard max | `poca load`/candidate staging, before `esp_elf_init` | `neg_maxmem` (hostile 600 KiB declaration, signature-valid) → `REJECT(>hard max)` pre-init, canary unchanged |
| program-header budget (Σ p_memsz + padding) ≤ declared `max_memory_bytes` | admission, T5 | `neg_phspan` rejected before load |
| > 512 KiB declaration cannot even be packaged | `manifest_builder` cross-field validation | builder refuses; fixture built via hostile `assemble_mpb` + manual deterministic sign |
| default arena 256 KiB; 256 KiB < declaration ≤ 512 KiB is requestable | admission log line | `baseline_300k` (300 KiB declared) loads + activates + unloads clean |

## Measured PSRAM budget (8 MiB octal PSRAM pool)

Phase snapshots from `poca coexist` (v1 = baseline 1.0.0 ACTIVE with a
500 ms self-call loop; v2 = baseline 2.0.0 CANDIDATE staged in an
independent `esp_elf_t` arena):

| phase | resident | psram free | delta vs baseline |
| --- | --- | --- | --- |
| baseline (no plugins) | — | 8385920 | 0 |
| v1 ACTIVE | staging 1988 B + arenas (text/data/bss) | 8382532 | **3388 B per instance** |
| coexistence peak (v1+v2, 2 stagings) | 2 × instance | 8379144 | **6776 B measured peak** |
| v2 ACTIVE (old unloaded) | 1 × instance | 8382532 | 3388 B |
| final (all unloaded) | — | 8385920 | 0 (exactly restored) |

- Per-instance cost breakdown (baseline-class plugin): staging copy 1988 B
  (mpb container) + loader arenas ≈ 1400 B (`.text` 0xd0, data windows
  0x74+bss 0x404 rounded by TLSF) = 3388 B observed. Historical cross-check:
  T4 measured 3324 B at ABI 1.0/mpb 1964 B — the growth is the ABI 1.2
  table/entry sizes and the larger container, same order.
- Worst-case admission envelope (policy arithmetic, not measured):
  2 × 512 KiB hard-max declarations + 2 × 512 KiB staging containers +
  loader metadata ≈ 1.5 MiB ≪ 8 MiB pool. Measured hot-update peak uses
  6776 B = 0.08 % of the 8 MiB pool; `largest_free_block` stays 8257536 B
  throughout (no fragmentation pressure at this scale).
- PSRAM `min_ever` floor across the whole session: 8379144 (= the
  coexistence peak; nothing resident deeper at any other time).

## Measured IRAM-exec budget (MALLOC_CAP_EXEC|MALLOC_CAP_INTERNAL, separate pool)

| item | value |
| --- | --- |
| pool free, harness baseline | ~311–313 KiB of ~354 KiB (fresh boot: free 312876, largest 286720, min_ever 308664) |
| baseline/baseline_v2 per-instance IRAM cost | **0 B** (no `.plugin_iram` section) |
| observed iram-exec delta during coexistence (−4212 B) | attribution: the spawned `poca_gen` loop task (4096 B stack + TCB) — task stacks allocate from the same DIRAM heap; NOT plugin cost |
| per-instance IRAM cost model (plugins *with* `.plugin_iram`, from T7) | section sh_size (97 B for iram_probe) + heap stash/rounding; iram_required_bytes consistency enforced by patch p5 before any allocation; 100-cycle T7 soak drift 0 |

## Measured stack budget (uxTaskGetSystemState snapshots, plan §4 item 6)

| task | stack | high-water mark (min free) at coexistence peak |
| --- | --- | --- |
| `console_repl` (poca console, runs `poca coexist`) | 6144 B | 2120 B |
| `poca_gen` (generation self-call loop, core 0, prio 4) | 4096 B | 3460 B |
| `IDLE0` / `IDLE1` | — | 756 B / 864 B |
| `ipc0` / `ipc1` | — | 600 B / 596 B |

The appendix-D "hot-update peak" stack row = console 2120 B free +
generation loop 3460 B free (both positive margins across the full 30 s
coexistence window).

## Coexistence acceptance evidence (transcript markers)

`[PASS-v1-active-60calls]` 60/60 loop calls at 500 ms, 0 magic mismatches,
loop still running while v2 staged · `[PASS-v2-candidate-loaded]`
independent arenas (v1 text 0x3c061268 vs v2 text 0x3c061fa4) ·
`[PASS-single-candidate-guard]` third load → FRAME_ERR_BUSY before any
allocation, execution canary unchanged · `[PASS-both-generations-respond]`
old returns magic_v1 (0x1501e22e) AND new returns magic_v2 (0x771eee2c) at
distinct code addresses, simultaneously · `[PASS-swap-unload-old]` old
generation unloaded, v2 ACTIVE, magic_v2 holds · `[PASS-budget-table]`
figures above printed on-device.

Stale-state guard: every load closes the loop build→device (per-mpb
SHA-256 assert) and asserts manifest name/version identity (`name=baseline
version=1.0.0/2.0.0 MATCH`) — the two generations differ by version bytes
inside the signed manifest, so a swapped embed is caught before relocation.

## Capacity (T10, development-plan §4 item 8 / PLUG-004 / MEM-007)

Eight name-parameterized plugins `cap01..cap08` (baseline source template,
distinct manifest name `capNN` / version `1.0.N` / activate magic
0x4341503N "CAP1".."CAP8"; elf 1708 B, mpb 1984 B each). `poca capacity <n>`
loads n plugins sequentially — each a distinct `esp_elf_t` instance held
live — then a check-fn round over all n resident generations, a peak
heap/stack snapshot, and a reverse-order unload. Ladder policy (host runner
`plugins/capacity_ladder.py`): rung 8 first, retry-once per rung, descend
8→7→6→5, rung 5 failure = Gate FAIL. Raw transcript:
`.omo/evidence/poca/task-10-poc-a-dynamic-elf.log`.

**Ladder result: PASS at 8** (rung 8, attempt 1; three consecutive full
runs identical — ladder runner ×2 + post-format confirmation; rungs 7/6/5
never executed, descent path unexercised on this hardware).

| metric (measured at rung 8) | value |
| --- | --- |
| PSRAM free, harness baseline | 8385920 B (Wi-Fi off — MEM-001 caveat above) |
| per-instance cost | **3356 B** = staging container 1984 B + loader arena 1352 B |
| 8-instance peak consumption | 26848 B = **0.32 %** of the 8 MiB pool |
| per-instance PSRAM text base | 0x3c071248 … 0x3c076e0c, stride 0xd1c, all 8 pairwise distinct |
| per-instance IRAM / internal delta | **0 B / 0 B** (baseline-class plugins: no `.plugin_iram`; the 8 `esp_elf_t`+slot array lives in firmware `.bss`) |
| `largest_free_block` at peak | 8257536 B (unchanged from baseline — zero fragmentation pressure) |
| PSRAM after reverse unload | 8385920 B (exactly restored; `min_ever` floor = peak) |
| stack HWM at peak | console_repl 2040/6144 B, IDLE0 756 B, IDLE1 864 B, ipc0 568 B, ipc1 596 B |
| execution canary | +8 per rung-8 run (== 8 entry queries, no extra/missing queries) |

Cross-check vs T8: baseline-class per-instance 3388 B there (staging 1988 B)
vs 3356 B here (staging 1984 B — shorter manifest name `capNN` vs
`baseline`); same order, arena identical class. Adversarial guards: 8
DISTINCT staging SHA-256 observed on-device (matching the 8 build-time
hashes — stale-embed guard), 8 DISTINCT magics all matching at 8 DISTINCT
text bases simultaneously (misleading-success guard: one plugin called 8
times cannot produce this), transcript markers `[PASS-cap-8-load]`
`[PASS-cap-8-respond]` `[PASS-cap-8-unload]` `[PASS-capacity-8]`.

Headroom math: measured envelope 8 × 3356 B = 26848 B ≪ 8 MiB. Worst-case
*policy* envelope: 8 × (512 KiB declared arena + 512 KiB max container)
≈ 8 MiB ≱ pool — so the product admission controller must gate the SUM of
resident declarations against the pool (global budget), not only the
per-plugin 512 KiB hard max. Recorded as an ADR-0002 amendment note with
the proposal value below.

**Signed budget line:** Capacity: 8 ACTIVE verified (8 attempted),
per-instance cost 3356 B (3.28 KiB) PSRAM, 8-instance peak 26848 B =
0.32 % of the 8 MiB pool (IRAM delta 0 B, internal delta 0 B,
largest_free_block unchanged), headroom to MEM-007 floor 5: 8/5,
MEM-007 → ADR-0002 proposal value: 8 ACTIVE generations per distinct
plugin name (PSRAM class; re-measure with Wi-Fi at the next gate).
