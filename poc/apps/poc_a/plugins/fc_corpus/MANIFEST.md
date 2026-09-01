# Fail-closed corpus (T14) — 14 tamper classes with execution sentinel

Deterministic on-board corpus proving development-plan §4 item 9
("失败包、错误 relocation、hash/signature/package type mismatch 不执行任何
payload byte"), TEST-003 and the SEC-004 verification order. Every fixture
must be rejected **before any plugin byte executes**, witnessed by the
entry-query canary (`g_entry_queries`, poca_plugin.cc) which the runner
asserts unchanged across every injection.

## Transport

The 14 classes are injected by **streaming the fixture bytes over the
console** (`poca fcbegin <n>` / `poca fcwr <hex>`) into the device's
immutable PSRAM upload staging, then running the SAME
parse+verify+admission+relocate pipeline via `poca fcgo <name> <stage>
<code>`. This proves the pipeline rejects arbitrary attacker-controlled
bytes, not only compiled-in arrays. The device re-hashes the uploaded bytes
and the runner asserts the hash equals the committed file's SHA-256
(upload-integrity + stale-state closed loop). The embedded hostile set
(T5/T6/T7/T8) is additionally re-driven through `poca load`, whose
device-side NEGATIVE assertion prints the exact errno.

## The 14 classes

| # | class            | fixture(s)                                     | src    | stage     | code |
|---|------------------|------------------------------------------------|--------|-----------|------|
| 1 | magic            | neg_magic                                      | fc     | mpb       | -18 PACKAGE_INVALID |
| 2 | total-size       | neg_total_size                                 | fc     | mpb       | -18 |
| 3 | offset-oob       | neg_offset_oob                                 | fc     | mpb       | -18 |
| 4 | overlap          | neg_overlap                                    | fc     | mpb       | -18 |
| 5 | dup-required     | neg_dup_required                               | fc     | mpb       | -18 |
| 6 | unknown-req-tlv  | neg_unknown_required_tlv                       | fc     | mpb       | -18 |
| 7 | mixed (both dir) | neg_mixed_elf_in_res + neg_mixed_res_in_elf    | fc     | mpb       | -18 |
| 8 | payload-hash     | neg_payload_hash                               | fc     | mpb       | -19 HASH_MISMATCH |
| 9 | signature        | neg_signature                                  | fc     | mpb       | -17 SIGNATURE_INVALID |
|10 | key-id           | neg_key_id (2nd deterministic key)             | fc     | mpb       | -17 |
|11 | target           | neg_target (target_id=0x53454C46)              | fc     | mpb       | -35 TARGET_MISMATCH |
|12 | abi              | neg_abi (core_abi 2.0 vs device 1.0)           | fc     | mpb       | -7 ABI_MISMATCH |
|13 | epoch            | neg_epoch (epoch 0 < floor 1)                  | fc     | mpb       | -34 EPOCH_ROLLBACK |
|14 | elf-identity     | neg_phbe / neg_ph64 / neg_phmach              | build  | relocate  | -22 EINVAL (loader patch p2) |

Order proof (SEC-004, unbypassable): `neg_double_fault` corrupts BOTH the
signature and a payload hash and must report **-17** — signature
verification precedes payload hashing.

## Fixture provenance

* `fc` (committed here): byte-copies of the deterministic T3 host corpus
  `poc/host/tests/mpb_corpus/` (generator `poc/host/tests/gen_mpb_corpus.py`,
  builder pinned `tools/frame_tools/manifest_builder.py`, RFC 6979
  deterministic signing, TEST-only key `e5677a1c`). `fc_runner.py` refuses
  to run if the copies drift from the host corpus. Verified against the
  CURRENT firmware policy: key_id e5677a1c = trusted key, epoch floor 1,
  target 0x33505345 (ESP32-S3), core ABI 1.0, device_features 0.
* `build`: generated at firmware-build time by the hostile_signed recipes
  (`plugins/negative/build_negative_corpus.py` from the gated baseline.elf,
  TEST-only key) — signature-valid containers whose rejection comes from
  the loader layers.
* `embed`: fixtures already linked into the firmware (union set): import_neg
  (-88 ENOSYS), neg_r32/neg_s0op (-22, patch p1), neg_phspan (phdr
  admission), neg_maxmem (600KiB admission, pre-init), cxx_ctor/cxx_tls
  (-22, patch p4), neg_tls_nosect (-22, patch p1), iram_mismatch (-22,
  patch p5).

## Running

```
/home/lain/Projects/esp_study/esp_study/.venv/bin/python \
  poc/apps/poc_a/plugins/fc_corpus/fc_runner.py --port /dev/ttyACM0
```

Two rounds by default; per fixture the transcript carries the local file
hash, the device-side hash, the RESULT stage/err/expected line and the
canary bracket; final markers `[FC-ALL-14-PRE-EXEC-REJECT]`,
`[FC-ORDER-DOUBLE-FAULT]`, `[FC-CANARY-SUMMARY]`. A positive control
(baseline load/activate/unload) runs each round so a corpus run can never
pass by rejecting everything.

All key material is TEST-ONLY (SEC-003/SEC-005 dev default).
