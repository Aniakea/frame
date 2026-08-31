#!/usr/bin/env python3
"""Convert a TEST-ONLY ECDSA P-256 public key PEM into a C header.

Emits the 65-byte uncompressed point (0x04 || X || Y) that
mpb_mbedtls_crypto_make() expects, plus the frame key_id (first 4 bytes of
SHA-256 over the SPKI DER, big-endian - same derivation as
frame_tools.manifest_builder.derive_key_id). Standard library only so the
step also runs inside the plain ESP-IDF CI container python.

TEST-ONLY key material (SEC-003): the committed PEM under
tools/frame_tools/testdata/keys/ must never be used for production signing.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import sys
from pathlib import Path

GUARD = "POCA_GENERATED_PUBKEY_H"
KEY_ID = 0xE5677A1C


def pem_to_der(pem: str) -> bytes:
    body = "".join(
        line for line in pem.splitlines() if line and not line.startswith("-----")
    )
    return base64.b64decode(body, validate=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pem", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--expect-key-id", type=lambda v: int(v, 0), default=KEY_ID)
    args = parser.parse_args()

    der = pem_to_der(args.pem.read_text(encoding="ascii"))
    if len(der) < 70:
        print(f"error: {args.pem}: too short for a P-256 SPKI", file=sys.stderr)
        return 1
    point = der[-65:]
    if point[0] != 0x04:
        print(f"error: {args.pem}: SPKI does not end in an uncompressed point", file=sys.stderr)
        return 1
    key_id = int.from_bytes(hashlib.sha256(der).digest()[:4], "big")
    if key_id != args.expect_key_id:
        print(
            f"error: {args.pem}: derived key_id {key_id:08x} != expected {args.expect_key_id:08x}",
            file=sys.stderr,
        )
        return 1

    rows = [", ".join(f"0x{b:02x}" for b in point[i : i + 8]) for i in range(0, 65, 8)]
    body = "\n".join(f"    {row}," for row in rows)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(
        "/* Generated from "
        f"{args.pem.name} (TEST-ONLY, SEC-003) by gen_pubkey_header.py - do not edit. */\n"
        "#include <stdint.h>\n"
        "\n"
        f"#define POCA_TRUSTED_KEY_ID 0x{key_id:08x}u\n"
        "static const uint8_t POCA_TRUSTED_KEY_POINT[65] = {\n"
        f"{body}\n"
        "};\n",
        encoding="ascii",
    )
    print(f"key_id={key_id:08x} point=0x04||X||Y(64B) -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
