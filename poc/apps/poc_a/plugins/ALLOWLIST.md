# PoC-A relocation / import / program-header allowlist (frozen, Gate-A output)

Task T5 of `poc-a-dynamic-elf`. This file freezes the exact dynamic-relocation
allowlist enforced by the layered pipeline (build gates + vendored-loader
patches p1/p2 + harness admission checks), with the on-board evidence
references for every row. Changes to this matrix require a Gate-A re-run.

Loader under test: vendored elf_loader 1.3.3 @6526c5b1, xtensa arch layer
(`components/elf_loader/src/arch/esp_elf_xtensa.c`), with registered patches
`p1` (fail-closed propagation of `esp_elf_arch_relocate()` errors) and `p2`
(ELF identity validation) — see `components/elf_loader/PATCHES.md`.

## 1. Dynamic relocation allowlist (Xtensa, ELF32 LE, ET_DYN)

| Type | Value | Emitted by (observed, this pipeline) | On-board positive evidence | Negative/out-of-list proof |
| --- | ----- | ------------------------------------ | --------------------------- | --------------------------- |
| `R_XTENSA_NONE` | 0 | never emitted (readelf across all plugins: 0) | n/a (accepted, no-op) | n/a |
| `R_XTENSA_RELATIVE` | 5 | every plugin: `.data.rel.ro` fn-ptr words + `.text` l32r literal slots (baseline: 8) | baseline load+activate PASS; data pointer in PSRAM resolves to sibling address (`entry sym ... mapped=... exec=... delta=0x06000000`, MAGIC MATCH) — task-5 log | type flipped to 1/20 in `neg_r32`/`neg_s0op` (see below) proves non-allowlisted types abort |
| `R_XTENSA_RTLD` | 2 | linker markers emitted automatically when the PLT literal pool is used (`plt`: 2, `import_neg`: 2, addend 1..2, no symbol) | `readelf -rW plt.elf`: `R_XTENSA_RTLD ... 1/2`; loaded and executed on board (plt activate PASS) — loader treats as no-op (`case R_XTENSA_RTLD: break;`) | n/a (no codegen effect; accepted per allowlist) |
| `R_XTENSA_GLOB_DAT` | 3 | address of a registered extern function stored in a retained const initializer → `.data.rel.ro` word; also a `.text` literal for the address load (`globdat`: 2 — one at 0x1dc in `.text`, one at 0x1320 in `.data.rel.ro`) | `load globdat` PASS with `GLOB_DAT host_sentinel=0x... expected=0x... MATCH` (exact pointer equality against the firmware's registered `&poca_host_sentinel`) + `activate` MAGIC MATCH through the relocated word | `neg_r32` flips a real `.rela.dyn` entry's type to `R_XTENSA_32` (1) → loader `-EINVAL` (patch p1) |
| `R_XTENSA_JMP_SLOT` | 4 | direct call of a registered extern function → `.rela.plt` entry patching a call literal word inside `.text` (`plt`: 1 at 0x1ec) | `load plt` PASS + `GLOB_DAT host_add=... MATCH` + `activate` MAGIC MATCH (both direct PLT calls return through the JMP_SLOT-patched literals) | `neg_s0op` flips the same entry to `R_XTENSA_SLOT0_OP` (20) → loader `-EINVAL` (patch p1); `import_neg` proves the import path refuses unknown names |

Everything else — including `R_XTENSA_32` (1), `R_XTENSA_SLOT0_OP..SLOT14_OP`
(20..34), all `R_XTENSA_TLS_*`, `ASM_EXPAND`, `DIFF*`, `GNU_VT*` — is
**rejected**: build-time by `check_plugin_elf.cmake` gate 1, load-time by
patch p1 (`-EINVAL`, before any plugin byte executes).

### Toolchain observation (why negatives are hand-patched)

xtensa ld (esp-15.2.0_20251204) cannot be coerced into emitting
out-of-allowlist *dynamic* relocations from C under the pipeline flags:

- `.reloc <loc>, R_XTENSA_32, <extern>` against a dynamic symbol is
  canonicalized to `R_XTENSA_GLOB_DAT` (addend absorbs the in-place word);
  against a section symbol or with a pure addend it becomes
  `R_XTENSA_RELATIVE`;
- `R_XTENSA_SLOT0_OP` in `.reloc` is a link-time hard error ("dangerous
  relocation: invalid relocation for dynamic symbol").

The two reloc negatives are therefore deterministic post-link byte patches of
a genuine `.rela.dyn` entry of the gated `baseline.elf` (first entry, target
0x198, `r_info 0x00000005 → 0x00000001 / 0x00000014`), produced by
`negative/build_negative_corpus.py` and re-signed with the TEST key — i.e.
exactly the artifact a hand-crafted malicious container would carry. The
pipeline gates correctly refuse to build such images; the corpus script
bypasses the gates **only** for these negative fixtures (by design; the gates
protect positive builds).

## 2. Import policy (closed world + registered-host exceptions)

- Build-time (gate 2): a plugin may leave undefined exactly the symbols
  declared in its `poca_plugin(... IMPORTS ...)` CMake argument
  (`check_plugin_elf.cmake`). Current frozen set:
  - `globdat`: `poca_host_sentinel`
  - `plt`: `poca_host_add`
  - `import_neg`: `frame_unknown_symbol` (declared to let the probe build;
    it is intentionally NOT registered on the device)
  - `baseline`: no imports
- Device-time: the loader resolves imports through
  `elf_find_sym_default()` → libc/espidf Kconfig tables →
  `esp_elf_find_symbol()` (tables registered via `esp_elf_register_symbol()`).
  The harness registers exactly `poca_host_sentinel` and `poca_host_add`.
  An unregistered import (`frame_unknown_symbol`) makes
  `esp_elf_relocate()` return `-ENOSYS` (-88; newlib-xtensa numbering) (verified on board) **before any plugin byte
  executes** (stock upstream behavior, ADR-0002; verified on board).
- `elf_set_symbol_resolver()` (v1.3.1 hook) is available but not required:
  the register/unregister API already yields fail-closed semantics for the
  PoC (registered ⇒ resolves, everything else ⇒ `-ENOSYS`).

## 3. Program-header (§4 item 2) policy

- Positive: `poca verify|load` computes, from the verified payload bytes, the
  segment budget `sum(PT_LOAD p_memsz) + sum(inter-segment padding)` and
  prints it against the manifest `max_memory_bytes`; the load aborts unless
  `budget <= max_memory_bytes` and every `PT_LOAD` span (`p_offset+p_filesz`)
  lies inside the verified payload (admission check runs after mpb
  verification, before `esp_elf_init`).
- Negative: `neg_phspan` inflates the first `PT_LOAD p_memsz` 680 →
  15,728,640 (15 MiB) → budget 15,728,732 B > 262,144 B → rejected at
  admission before any loader call (board log).
- Loader-side split (documented behavior): on ESP32-S3 the vendored loader's
  `CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR` path loads by **sections**, so phdr
  arithmetic is not consulted by `esp_elf_load_section()`; the phdr policy
  above is the admission layer. (The upstream non-mirror
  `esp_elf_load_segment()` path does check memsz<filesz / overlap / vaddr
  overflow.)
- ELF identity negatives (loader-side, patch p2): `neg_phbe`
  (`EI_DATA=big-endian`), `neg_ph64` (`EI_CLASS=ELF64`), `neg_phmach`
  (`e_machine=40/EM_ARM`) each pass mpb structure+signature+hash (signed with
  the TEST key) and are rejected by `esp_elf_relocate()` with `-EINVAL`
  before any header-driven parse or copy. This is the loader-side half of
  T14's 14th tamper class; the build side is covered by gate 3
  (check_plugin_elf.cmake) and `extract_plugin_iram_size()` in the manifest
  builder, which is why these three fixtures are packaged through
  `assemble_mpb()` + the deterministic signer (the "hostile_signed" pattern).

## 4. Link-flag contract (pipeline, mirrors vendored project_so)

Compile: `-c -std=c11 -Os -g0 -fPIC -fvisibility=hidden -fdata-sections
-ffunction-sections -ffreestanding -fno-builtin -Wall -Wextra -Werror`

Link: `-shared -fPIC -static-libgcc -nostdlib -nostartfiles
-fdata-sections -ffunction-sections -Wl,--gc-sections -fvisibility=hidden
-Wl,--allow-shlib-undefined -Wl,--build-id=none -e frame_plugin_entry`

Strip: `--strip-unneeded --keep-symbol=frame_plugin_entry
--remove-section=.comment --remove-section=.got.loc
--remove-section=.dynamic --remove-section=.note.gnu.build-id
--remove-section=.xt.lit --remove-section=.xt.prop
--remove-section=.xtensa.info`

(The only deviations from the upstream macros: deferred strip with
`--keep-symbol`, `-Wl,--build-id=none`, and the freestanding compile set —
rationale in `plugins.cmake`.)

Under these flags a closed C module emits **only** allowlisted types; every
observed count is recorded by gate 1 at build time (`R_XTENSA_* =n` in the
build log) and the on-board matrix above re-proves the behavior end-to-end.

## 5. Evidence

- Build log (gates + corpus + hashes): this task's evidence log,
  `.omo/evidence/poca/task-5-poc-a-dynamic-elf.log`.
- Board transcript (all matrix rows, canary counts, per-`.mpb` sha256):
  same evidence log, "on-board matrix" section, raw copy archived under
  `poc/evidence/poc-a-<date>/` by T16.
- Per-container sha256 at build time (stale-embed detection is closed-loop:
  the same hashes are printed by `poca load <name>` from the device):
  see `[poca-plugin]` / `[corpus]` lines in the build log.
