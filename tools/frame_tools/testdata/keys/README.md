# Test-only ECDSA P-256 signing keys

**TEST-ONLY. NEVER USE FOR PRODUCTION SIGNING.** (SEC-003)

Production private keys must never exist in the repository, firmware images,
CI, or logs; production signing is a controlled offline release step. The pair
committed here is the sanctioned PoC-A development test key (plan SEC-005
default) and exists only so the deterministic MPB builder, the python reference
verifier, and the T3 C parser tests can round-trip signed containers without
touching real key material.

- `poc_a_test_signing_key.pem` — private key (secp256r1, PKCS#8, unencrypted)
- `poc_a_test_signing_key.pub.pem` — public key (SPKI)

## key_id

`key_id` is the first 4 bytes of SHA-256 over the public key DER (SPKI),
read big-endian:

- key_id = `e5677a1c` (u32 3848763932; little-endian bytes in the fixed header: `1c 7a 67 e5`)

Regenerate the value with:

```sh
uv run python -c "from frame_tools.manifest_builder import derive_key_id, load_public_key; print(f'{derive_key_id(load_public_key(__import__("pathlib").Path("tools/frame_tools/testdata/keys/poc_a_test_signing_key.pub.pem"))):08x}')"
```
