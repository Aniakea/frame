# MPB host test corpus (T3)

Deterministic fixtures consumed by `mpb_parser_test` in `poc/host`. Binary
corpus in git is intentional: the builder is fully deterministic (RFC 6979
signing), so regeneration reproduces byte-identical files.

## Regeneration

```
uv run python poc/host/tests/gen_mpb_corpus.py
```

The generator calls the worktree's python oracle
`tools/frame_tools/manifest_builder.py` (pinned at commit `4793cc2`) and
re-verifies every fixture expectation against `verify_mpb` before writing.
Run it from the worktree after any builder change; commit the `.mpb` files
together with `corpus_manifest.inc`.

## Contents

2 positives (`pos_code`, `pos_resource_only` — the resource package carries
two payloads with alignments 4 and 16 so alignment padding inside the payload
window is exercised) and 16 negatives covering the 13 required tamper
classes plus two extras (`neg_double_fault` proves signature is reported
before payload hash, `neg_features` proves required-features policy), plus
the wrong-key variant inside the signature class (`neg_key_id`, signed by a
deterministically derived second key; the KAT context only trusts key
`e5677a1c`).

Expected error codes are `frame_err_t` values; see `corpus_manifest.inc`.

## Host signature known-answer strategy

The host `ecdsa_verify_fn` test double accepts exactly the
`(key_id, digest, signature)` triple the builder produced, recorded in
`corpus_manifest.h`. Equality stands in for the P-256 math: tampering the
signed prefix changes the recomputed digest, tampering the record changes
the signature bytes, and the wrong-key fixture fails the key comparison.
The target build performs real ECDSA P-256 verification through PSA crypto
(`mpb_mbedtls_glue.c`).

Each fixture records its whole-file SHA-256; the test re-hashes every file
with the reference SHA-256 before parsing, so a stale or partially
regenerated corpus fails loudly instead of silently drifting.

All key material here is TEST-ONLY (SEC-003/SEC-005 dev default).
