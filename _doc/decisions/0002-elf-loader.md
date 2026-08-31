# ADR-0002: Dynamic ELF Loader Selection

Status: Blocked

## Context

The architecture requires signed `.mpb` packages containing Xtensa ESP32-S3 ELF payloads whose verified bytes are loaded into PSRAM/IRAM. No loader or commit is selected. PoC-A must compare candidates before implementation details become product dependencies.

## Candidate Evaluation

Candidate | Repository | Commit | License | ESP-IDF v6.0.2 | Xtensa relocations | Dynamic PSRAM text | `.plugin_iram` | Cache synchronization | Result
--- | --- | --- | --- | --- | --- | --- | --- | --- | ---
TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | Pending

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
