# PoC-A C++ feature rejection matrix (frozen, Gate-A output)

Task T6 of `poc-a-dynamic-elf`. Appendix C 11.2 declares these C++ runtime
features **default-forbidden** for loadable plugins; this matrix proves, per
feature, that the artifact is rejected at every layer that can see it. PoC-A
proves REJECTION only — no row is whitelisted, and no row may ever be.

Layers:

- **L1 pipeline/CI** — `check_plugin_elf.cmake` runs inside every positive
  `poca_plugin()` build AND inside the poc_a CI lane. Gate 1 = dynamic
  relocation allowlist (T5), gate 4 = forbidden C++ feature sections (T6,
  new), gate 2 = closed-world imports (T5), gate 3 = ELF shape (T4). The
  probe variants are built by `poca_cxx_probe()`
  (plugins/cxx_probes/*.cpp) and each one is run through the REAL gate
  script by the expected-failure wrapper `check_probe_rejection.cmake`:
  acceptance of any probe fails the build ("NOT REJECTED" is fatal), and
  the verbatim gate error text lands in the build log.
- **L2 loader (fail-before-execute)** — vendored elf_loader with registered
  patches: p1 (relocation allowlist propagation, `-EINVAL`), p2 (ELF
  identity), **p4 (forbidden-section scan, T6, new — see decision below)**.
  p4 mirrors the gate 4 section list on target so a hand-crafted signed
  container (the T5 `hostile_signed` path) cannot slip a feature section
  past the build layers.

| # | Feature | Emitter (this toolchain) | Forbidden artifact(s) | Rejection layer(s) | Evidence (task-6 log) | Whitelisted? |
| - | ------- | ------------------------ | --------------------- | ------------------ | --------------------- | ------------ |
| v1 | static constructor | file-scope object w/ non-trivial ctor (no extra flag) | `.ctors` section (this gcc's `.init_array` spelling) + its `R_XTENSA_RELATIVE` entries | L1 gate 4 (`.ctors`); L2 p4 (section scan, `-EINVAL` before copy) | build: "forbidden C++ feature section(s) [.ctors]"; board: `cxx_ctor` row, `Forbidden C++ feature section '.ctors'`, errno -22, canary unchanged | **NO** |
| v2 | static destructor | object w/ non-trivial dtor + `__attribute__((destructor))`, `-fno-use-cxa-atexit` | `.dtors` section (this gcc's `.fini_array` spelling) | L1 gate 4 (`.dtors`) | build: "forbidden C++ feature section(s) [.dtors]" (verbatim gate output) | **NO** |
| v3 | dynamic TLS | `__thread` vars written+read at runtime (never-written thread-locals are constant-folded away by gcc — the runtime store is load-bearing for the probe) | `.tdata`/`.tbss` sections + `R_XTENSA_TLSDESC_FN`/`R_XTENSA_TLSDESC_ARG` dynamic relocs in `.rela.dyn` | L1 gate 1 (TLS reloc types out of allowlist); L1 gate 4 (sections, latent — gate 1 fires first); L2 p4 (`.tdata` scan, `-EINVAL`); L2 p1 (TLSDESC reloc types, `-EINVAL` — proven independently by the `neg_tls_nosect` row whose sections were renamed away) | build: "relocation allowlist violated (R_XTENSA_TLSDESC_ARG;R_XTENSA_TLSDESC_FN)"; board: `cxx_tls` (p4 log line) + `neg_tls_nosect` ("Failed to relocate type=51"), both errno -22, canary unchanged | **NO** |
| v4 | exceptions | `-fexceptions -funwind-tables` (toolchain needs the unwind-tables flag to emit landing pads at -Os) | `.eh_frame` + `.gcc_except_table` sections; undeclared EH runtime imports (`__cxa_*`, `_Unwind_Resume`, `__gxx_personality_v0`, `_ZTIi`) | L1 gate 4 (`.eh_frame`/`.gcc_except_table`); L2 p4 (same list); (gate 2 / loader `-ENOSYS` would additionally catch the imports) | build: "forbidden C++ feature section(s) [.eh_frame;.gcc_except_table] present" | **NO** |
| v5 | RTTI | `-frtti` (probe baseline keeps `-fno-rtti`), polymorphic `typeid` + `dynamic_cast` | no dedicated section on this toolchain: local typeinfo in `.data.rel.ro` (allowlisted relocs) + undeclared C++ ABI imports `__dynamic_cast`, `_ZTVN10__cxxabiv1...`, `_ZdlPvj` | L1 gate 2 (closed-world imports); L2 loader `-ENOSYS` for unregistered imports (same mechanism proven on board by T5 `import_neg`) | build: "import gate failed, undeclared undefined symbols: __dynamic_cast; _ZdlPvj; _ZTVN10__cxxabiv117__class_type_infoE; _ZTVN10__cxxabiv120__si_class_type_infoE" | **NO** |
| v6 | unwind tables | `-funwind-tables -fasynchronous-unwind-tables`, `noinline` function | `.eh_frame` section (pure emitter: no exception code, no EH imports) | L1 gate 4 (`.eh_frame`); L2 p4 | build: "forbidden C++ feature section(s) [.eh_frame]" | **NO** |

On-board corpus rows (packaged via `cxx_probes/build_cxx_probes.py`, the T5
hostile_signed pattern — structure/signature/hash all PASS on device, so the
rejection necessarily comes from the loader): `cxx_ctor` (v1) and `cxx_tls`
(v3) must fail `esp_elf_relocate` with `-EINVAL` **before any plugin byte
executes** (entry-query canary unchanged); `neg_tls_nosect` isolates the p1
relocation-layer rejection by renaming `.tdata`/`.tbss` to `.xdata`/`.xbss`
in `.shstrtab` (same-length byte patch).

## p4 decision — ADD (loader-side section rejection): justification

The plan text for T6 mandates the loader half ("loader 侧 fail-before-execute
（若链接器仍产出则解析 ELF section 于执行前拒绝）"), and TEST-003 / §4 item 4
("未知项目 fail before execute") is only satisfiable with it. The empirical
pre-p4 board run (task-6 evidence log, phase 2) settles the policy question:

- A signed `cxx_ctor` container (valid mpb structure, signature, hash; only
  allowlisted `R_XTENSA_RELATIVE` relocations) made the unpatched loader
  write through `esp_elf_map_sym()==0` for the never-loaded `.ctors`
  pointer: `Guru Meditation Error: Core  1 panic'ed (LoadProhibited)`,
  `EXCVADDR: 0x00000000`, firmware reboot. That is a fail-open crash (DoS),
  not a fail-before-execute rejection — a hostile producer with any valid
  signing key path (or a leaked CI key) crashes the device.
- "Constructor never runs" silent misbehavior is the *benign* branch of the
  same gap; the observed branch was worse.
- Build gates cannot be the last line: the T5 corpus methodology (and this
  task's packaging) demonstrates signed containers bypass builder/pipeline
  checks by construction. p2 established the same lesson for ELF identity.
- With p4, the same container is rejected pre-allocation with
  `-EINVAL` ("Forbidden C++ feature section '.ctors'"), canary unchanged.

Registered as patch p4 in `components/elf_loader/PATCHES.md` (diff + sha256
+ rationale). The section list mirrors gate 4 exactly (prefix match covers
numbered subsections like `.init_array.00001`).

## v2/v4/v5/v6 board coverage note

By design (and per the task contract) only v1 and v3 have on-board corpus
rows: v2 (`.dtors`), v4 (`.eh_frame`/`.gcc_except_table`) and v6
(`.eh_frame`) share p4's section-scan mechanism, whose on-target behavior is
proven by v1's row; v5's loader-side layer is the import `-ENOSYS` path
proven on board by T5's `import_neg` row. Their L1 gate rejection is proven
in the build log (verbatim gate error for each). If Gate-A review wants belt
and braces, packaging them is a 5-line extension of `build_cxx_probes.py`.

## Notes for T15 (CI corpus lane)

- The expected-failure probes already run inside the poc_a CI build lane
  (`poca_cxx_probe_*` targets are dependencies of the main component): each
  lane build fails unless all six probes are rejected with the recorded
  reason. The lane log carries each rejection verbatim.
- To script appendix C 11.3 static checks in the host corpus job: gate 4's
  forbidden-stem list (`check_plugin_elf.cmake`) and p4's loader list
  (`esp_elf.c` patch p4) must stay in sync — both are single sources,
  mirror by convention; a corpus test should assert the two lists are
  identical (parse both) so drift is caught.
- `neg_tls_nosect` shows the pattern for relocation-layer-only probes:
  rename the section names in `.shstrtab` (same-length overwrite) and the
  reloc allowlist is the only remaining layer.

## Evidence pointers

- Build-side rejection (verbatim gate errors, all six), determinism hashes,
  probe ELF sha256s, artifact map: `.omo/evidence/poca/task-6-poc-a-dynamic-elf.log`.
- Pre-p4 gap demo (LoadProhibited crash + reboot): same log, phase 2.
- Post-p4 matrix (3 positives + 10 negatives, canary unchanged, runs
  1/4/5 PASS; runner-side flakes of runs 2/3 disambiguated in-log): same
  log, phase 3. Raw board output archived under `poc/evidence/poc-a-<date>/`
  by T16.
