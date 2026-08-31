# ADMISSION-BUDGET — PoC-A measured admission baseline (T8, frozen Gate-A output)

Development-plan §4 item 7 / appendix D "hot-update peak" row. All numbers
are measured on the ESP32-S3-RLCD-4.2 (N16R8) board over `/dev/ttyACM0`,
poc_a harness at commit range T1..T8 (elf_loader 1.3.3 vendored + patches
p1..p5, plugin ABI 1.2). Raw transcript: `.omo/evidence/poca/task-8-poc-a-dynamic-elf.log`
(`poca coexist` phase snapshots and `poca status` lines).

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
