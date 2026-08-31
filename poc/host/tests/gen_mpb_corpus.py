"""Generate the deterministic host test corpus for the T3 MPB C parser.

Usage (repository root or any directory):

    uv run python poc/host/tests/gen_mpb_corpus.py

Calls the pinned worktree builder ``tools/frame_tools/manifest_builder.py``
(commit 4793cc2) to emit every fixture into ``poc/host/tests/mpb_corpus/``
together with ``corpus_manifest.inc`` (the data table the GoogleTest binary
walks) and this directory's README. The python oracle re-verifies each
expectation before it is written, so a regenerated corpus that does not
reproduce the recorded expectations fails loudly instead of silently
drifting. The ``.inc`` extension keeps generated data outside the
clang-format gate's source globs.

Host known-answer (KAT) signature strategy, documented here because it is a
deliberate simplification: host tests inject an ``ecdsa_verify_fn`` test
double that accepts exactly the (key_id, digest, signature) triple the
builder produced, as recorded in this corpus. Equality stands in for the
P-256 math: any tamper of the signed prefix changes the digest side, any
tamper of the signature record changes the signature side, and the wrong-key
fixture fails the key_id comparison. The target build performs real ECDSA
verification through the PSA crypto glue instead.
"""

from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path
from typing import Any, Callable

from cryptography.hazmat.primitives.asymmetric import ec

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from frame_tools.manifest_builder import (  # noqa: E402
    FLAG_RESOURCE_ONLY,
    FRAME_ERR_ABI_MISMATCH,
    FRAME_ERR_EPOCH_ROLLBACK,
    FRAME_ERR_HASH_MISMATCH,
    FRAME_ERR_PACKAGE_INVALID,
    FRAME_ERR_SIGNATURE_INVALID,
    FRAME_ERR_TARGET_MISMATCH,
    MpbError,
    PAYLOAD_ENTRY_SIZE,
    PAYLOAD_TYPE_ELF,
    PAYLOAD_TYPE_RESOURCE,
    SIGNATURE_RECORD_SIZE,
    TLV_FLAG_REQUIRED,
    Manifest,
    Payload,
    assemble_mpb,
    build_mpb,
    derive_key_id,
    load_private_key,
    load_public_key,
    sign_digest_deterministic,
    verify_mpb,
)

CORPUS_DIR = Path(__file__).resolve().parent / "mpb_corpus"
KEYS = REPO_ROOT / "tools/frame_tools/testdata/keys"
PRIVATE_PEM = KEYS / "poc_a_test_signing_key.pem"
PUBLIC_PEM = KEYS / "poc_a_test_signing_key.pub.pem"
IRAM_SIZE = 0x1234
IRAM_ADDR = 0x40378000
KEY_B_SEED = b"poc-a-mpb-corpus-key-b"
EPOCH_FLOOR = 1


def synthetic_elf(iram_size: int = IRAM_SIZE, iram_addr: int = IRAM_ADDR) -> bytes:
    """Same minimal ELF32 LE Xtensa image as tests/test_manifest_builder.py."""
    shstrtab = b"\x00" + b".plugin_iram\x00" + b".shstrtab\x00"
    body = b"\x00" * 8
    shoff = 52 + len(body) + len(shstrtab)
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\x00" * 8
    ehdr = ident + struct.pack(
        "<HHIIIIIHHHHHH", 2, 94, 1, iram_addr, 0, shoff, 0, 52, 0, 0, 40, 3, 2
    )
    iram_name = 1
    shstr_name = 1 + len(b".plugin_iram\x00")
    shdrs = (
        b"\x00" * 40
        + struct.pack("<10I", iram_name, 1, 0x6, iram_addr, 52, iram_size, 0, 0, 16, 0)
        + struct.pack("<10I", shstr_name, 3, 0, 0, 52 + len(body), len(shstrtab), 0, 0, 1, 0)
    )
    return ehdr + body + shstrtab + shdrs


def tlv(field_id: int, flags: int, value: bytes) -> bytes:
    return struct.pack("<HHI", field_id, flags, len(value)) + value


def header_u32(container: bytes, offset: int) -> int:
    return struct.unpack_from("<I", container, offset)[0]


def header_u16(container: bytes, offset: int) -> int:
    return struct.unpack_from("<H", container, offset)[0]


def sign_crafted(container: bytes, key: ec.EllipticCurvePrivateKey) -> bytes:
    """Sign a container whose structure the oracle would reject (hostile path)."""
    table_end = header_u32(container, 52) + header_u16(container, 56) * PAYLOAD_ENTRY_SIZE
    digest = hashlib.sha256(container[:table_end]).digest()
    signature = sign_digest_deterministic(key, digest)
    out = bytearray(container)
    offset = header_u32(container, 60)
    out[offset + 8 : offset + SIGNATURE_RECORD_SIZE] = signature
    return bytes(out)


def hostile_unsigned(
    payloads: list[Payload],
    *,
    flags: int,
    key: ec.EllipticCurvePrivateKey,
    manifest_bytes: bytes,
    **kwargs: Any,
) -> bytes:
    return assemble_mpb(
        manifest_bytes=manifest_bytes,
        payloads=payloads,
        key_id=derive_key_id(key.public_key()),
        flags=flags,
        **kwargs,
    )


def payload_spans_and_hashes(container: bytes) -> list[tuple[int, int, str]]:
    table_offset = header_u32(container, 52)
    count = header_u16(container, 56)
    spans = []
    for index in range(count):
        entry = table_offset + index * PAYLOAD_ENTRY_SIZE
        offset, length = struct.unpack_from("<II", container, entry + 4)
        spans.append(
            (offset, length, hashlib.sha256(container[offset : offset + length]).hexdigest())
        )
    return spans


def kat_of(container: bytes, signature_override: bytes | None = None) -> tuple[str, str]:
    """Known-answer (digest, signature) of a fixture.

    digest is always recomputed from the fixture's final bytes (that is what
    the C parser will hash). signature is the legitimate builder signature
    for fixtures that were not tampered inside the signature record itself;
    for signature-tamper fixtures the caller passes the pre-tamper value so
    the known-answer double still detects the tamper.
    """
    table_end = header_u32(container, 52) + header_u16(container, 56) * PAYLOAD_ENTRY_SIZE
    digest = hashlib.sha256(container[:table_end]).hexdigest()
    signature_offset = header_u32(container, 60)
    if signature_override is not None:
        signature = signature_override
    else:
        signature = container[signature_offset + 8 : signature_offset + SIGNATURE_RECORD_SIZE]
    return digest, signature.hex()


def expect_oracle(
    code: int,
    data: bytes,
    key: ec.EllipticCurvePublicKey,
    *,
    expect_ok: bool = False,
) -> None:
    try:
        verify_mpb(
            data,
            key,
            epoch_floor=EPOCH_FLOOR,
            device_abi=(1, 0),
            device_features=0,
        )
    except MpbError as error:
        if expect_ok or error.code != code:
            raise AssertionError(f"oracle returned {error}, expected {code}") from error
    else:
        if not expect_ok:
            raise AssertionError(f"oracle accepted a fixture expected to fail with {code}")


def flip(data: bytes, offset: int) -> bytes:
    out = bytearray(data)
    out[offset] ^= 0x01
    return bytes(out)


def entry() -> dict[str, Any]:
    return {
        "name": "",
        "version": "",
        "epoch": 0,
        "iram": 0,
        "max_memory": 0,
        "payloads": [],
        "digest": "",
        "signature": "",
        "kind": 0,
    }


def main() -> int:
    key_a = load_private_key(PRIVATE_PEM)
    pub_a = load_public_key(PUBLIC_PEM)
    key_b = ec.derive_private_key(
        int.from_bytes(hashlib.sha256(KEY_B_SEED).digest(), "big"), ec.SECP256R1()
    )

    code_manifest = Manifest("poc-a-baseline", "1.2.3")
    code_manifest.set("max_memory_bytes", 262144)
    code_manifest.set("requires", ("frame/core>=1.0.0",))
    resource_manifest = Manifest("poc-a-assets", "2.0.0")
    resource_manifest.set("max_memory_bytes", 65536)

    pos_code = build_mpb(
        code_manifest,
        [Payload(PAYLOAD_TYPE_ELF, synthetic_elf(), alignment=4)],
        signing_key=key_a,
        security_epoch=1,
    )
    pos_resource = build_mpb(
        resource_manifest,
        [
            Payload(PAYLOAD_TYPE_RESOURCE, b"alpha-asset-bytes", alignment=4),
            Payload(PAYLOAD_TYPE_RESOURCE, b"omega!" * 3, alignment=16),
        ],
        signing_key=key_a,
        security_epoch=1,
    )
    assert verify_mpb(pos_code, pub_a, epoch_floor=EPOCH_FLOOR).package_type == "code"
    assert (
        verify_mpb(pos_resource, pub_a, epoch_floor=EPOCH_FLOOR).package_type
        == "resource-only"
    )

    sig_offset = header_u32(pos_code, 60)
    untampered_signature = pos_code[sig_offset + 8 : sig_offset + SIGNATURE_RECORD_SIZE]
    elf_table = header_u32(pos_code, 52)
    elf_offset, elf_length = struct.unpack_from("<II", pos_code, elf_table + 4)

    neg_magic = flip(pos_code, 0)
    neg_total_size = pos_code[:-1]
    neg_payload_hash = flip(pos_code, elf_offset + elf_length // 2)
    neg_signature = flip(pos_code, sig_offset + 8 + 10)
    neg_double_fault = flip(neg_payload_hash, sig_offset + 8 + 10)

    def signed_patch(
        payloads: list[Payload],
        flags: int,
        manifest_bytes: bytes,
        patch: Callable[[bytearray], None],
    ) -> bytes:
        crafted = bytearray(hostile_unsigned(payloads, flags=flags, key=key_a,
                                             manifest_bytes=manifest_bytes))
        patch(crafted)
        return sign_crafted(bytes(crafted), key_a)

    def patch_offset_oob(buf: bytearray) -> None:
        window_end = header_u32(bytes(buf), 60)
        struct.pack_into("<I", buf, header_u32(bytes(buf), 52) + 4, window_end)

    def patch_overlap(buf: bytearray) -> None:
        table = header_u32(bytes(buf), 52)
        first_offset = struct.unpack_from("<I", buf, table + 4)[0]
        struct.pack_into("<I", buf, table + PAYLOAD_ENTRY_SIZE + 4, first_offset)

    neg_offset_oob = signed_patch(
        [Payload(PAYLOAD_TYPE_RESOURCE, b"r", alignment=4)],
        FLAG_RESOURCE_ONLY,
        Manifest("oob", "0.0.1").encode(),
        patch_offset_oob,
    )
    neg_overlap = signed_patch(
        [
            Payload(PAYLOAD_TYPE_RESOURCE, b"aaaa", alignment=4),
            Payload(PAYLOAD_TYPE_RESOURCE, b"bbbb", alignment=4),
        ],
        FLAG_RESOURCE_ONLY,
        Manifest("overlap", "0.0.2").encode(),
        patch_overlap,
    )
    neg_dup_required = sign_crafted(
        hostile_unsigned(
            [Payload(PAYLOAD_TYPE_RESOURCE, b"dup", alignment=4)],
            flags=FLAG_RESOURCE_ONLY,
            key=key_a,
            manifest_bytes=tlv(1, TLV_FLAG_REQUIRED, b"dup-name")
            + tlv(1, TLV_FLAG_REQUIRED, b"dup-name"),
        ),
        key_a,
    )
    neg_unknown_required_tlv = sign_crafted(
        hostile_unsigned(
            [Payload(PAYLOAD_TYPE_RESOURCE, b"unk", alignment=4)],
            flags=FLAG_RESOURCE_ONLY,
            key=key_a,
            manifest_bytes=tlv(1, TLV_FLAG_REQUIRED, b"unknown-field")
            + tlv(2, TLV_FLAG_REQUIRED, b"1.0.0")
            + tlv(99, TLV_FLAG_REQUIRED, b"x"),
        ),
        key_a,
    )

    def mixed_hostile(flags: int, name: str) -> bytes:
        return sign_crafted(
            hostile_unsigned(
                [
                    Payload(PAYLOAD_TYPE_ELF, synthetic_elf(), alignment=4),
                    Payload(PAYLOAD_TYPE_RESOURCE, b"stowaway", alignment=4),
                ],
                flags=flags,
                key=key_a,
                manifest_bytes=Manifest(name, "0.1.0").encode(),
            ),
            key_a,
        )

    neg_mixed_elf_in_res = mixed_hostile(FLAG_RESOURCE_ONLY, "mixed-a")
    neg_mixed_res_in_elf = mixed_hostile(0, "mixed-b")

    neg_key_id = build_mpb(
        Manifest("wrong-key", "0.9.0"),
        [Payload(PAYLOAD_TYPE_RESOURCE, b"signed-by-b", alignment=4)],
        signing_key=key_b,
        security_epoch=1,
    )
    assert verify_mpb(neg_key_id, key_b.public_key(), epoch_floor=EPOCH_FLOOR)
    neg_target = sign_crafted(
        hostile_unsigned(
            [Payload(PAYLOAD_TYPE_ELF, synthetic_elf(), alignment=4)],
            flags=0,
            key=key_a,
            manifest_bytes=Manifest("wrong-target", "0.9.1").encode(),
            target_id=0x53454C46,
        ),
        key_a,
    )
    neg_abi = build_mpb(
        Manifest("abi-future", "0.9.2"),
        [Payload(PAYLOAD_TYPE_ELF, synthetic_elf(), alignment=4)],
        signing_key=key_a,
        security_epoch=1,
        core_abi_major=2,
    )
    neg_features = build_mpb(
        Manifest("features-beyond", "0.9.3"),
        [Payload(PAYLOAD_TYPE_ELF, synthetic_elf(), alignment=4)],
        signing_key=key_a,
        security_epoch=1,
        required_features=0x3,
    )
    neg_epoch = build_mpb(
        Manifest("stale-epoch", "0.9.4"),
        [Payload(PAYLOAD_TYPE_ELF, synthetic_elf(), alignment=4)],
        signing_key=key_a,
        security_epoch=0,
    )

    expect_oracle(0, pos_code, pub_a, expect_ok=True)
    expect_oracle(0, pos_resource, pub_a, expect_ok=True)
    expect_oracle(FRAME_ERR_PACKAGE_INVALID, neg_magic, pub_a)
    expect_oracle(FRAME_ERR_PACKAGE_INVALID, neg_total_size, pub_a)
    expect_oracle(FRAME_ERR_PACKAGE_INVALID, neg_offset_oob, pub_a)
    expect_oracle(FRAME_ERR_PACKAGE_INVALID, neg_overlap, pub_a)
    expect_oracle(FRAME_ERR_PACKAGE_INVALID, neg_dup_required, pub_a)
    expect_oracle(FRAME_ERR_PACKAGE_INVALID, neg_unknown_required_tlv, pub_a)
    expect_oracle(FRAME_ERR_PACKAGE_INVALID, neg_mixed_elf_in_res, pub_a)
    expect_oracle(FRAME_ERR_PACKAGE_INVALID, neg_mixed_res_in_elf, pub_a)
    expect_oracle(FRAME_ERR_HASH_MISMATCH, neg_payload_hash, pub_a)
    expect_oracle(FRAME_ERR_SIGNATURE_INVALID, neg_signature, pub_a)
    expect_oracle(FRAME_ERR_SIGNATURE_INVALID, neg_double_fault, pub_a)
    expect_oracle(FRAME_ERR_TARGET_MISMATCH, neg_target, pub_a)
    expect_oracle(FRAME_ERR_ABI_MISMATCH, neg_abi, pub_a)
    expect_oracle(FRAME_ERR_ABI_MISMATCH, neg_features, pub_a)
    expect_oracle(FRAME_ERR_EPOCH_ROLLBACK, neg_epoch, pub_a)

    def positive(file_: str, klass: str, kind: int, name: str, version: str, epoch: int,
                 iram: int, max_memory: int, container: bytes) -> dict[str, Any]:
        digest, signature = kat_of(container)
        item = entry()
        item.update(
            file=file_,
            corpus_class=klass,
            sha256=hashlib.sha256(container).hexdigest(),
            expected=0,
            kind=kind,
            name=name,
            version=version,
            epoch=epoch,
            iram=iram,
            max_memory=max_memory,
            payloads=payload_spans_and_hashes(container),
            digest=digest,
            signature=signature,
        )
        return item

    def negative(file_: str, klass: str, expected: int, container: bytes,
                 signature_override: bytes | None = None) -> dict[str, Any]:
        item = entry()
        digest, signature = kat_of(container, signature_override)
        item.update(
            file=file_,
            corpus_class=klass,
            sha256=hashlib.sha256(container).hexdigest(),
            expected=expected,
            digest=digest,
            signature=signature,
        )
        return item

    entries = [
        positive("pos_code.mpb", "positive-code", 1, "poc-a-baseline", "1.2.3", 1,
                 IRAM_SIZE, 262144, pos_code),
        positive("pos_resource_only.mpb", "positive-resource-only", 2, "poc-a-assets",
                 "2.0.0", 1, 0, 65536, pos_resource),
        negative("neg_magic.mpb", "negative-magic", FRAME_ERR_PACKAGE_INVALID, neg_magic),
        negative("neg_total_size.mpb", "negative-total-size", FRAME_ERR_PACKAGE_INVALID,
                 neg_total_size, untampered_signature),
        negative("neg_offset_oob.mpb", "negative-offset-oob", FRAME_ERR_PACKAGE_INVALID,
                 neg_offset_oob),
        negative("neg_overlap.mpb", "negative-overlap", FRAME_ERR_PACKAGE_INVALID,
                 neg_overlap),
        negative("neg_dup_required.mpb", "negative-dup-required",
                 FRAME_ERR_PACKAGE_INVALID, neg_dup_required),
        negative("neg_unknown_required_tlv.mpb", "negative-unknown-required-tlv",
                 FRAME_ERR_PACKAGE_INVALID, neg_unknown_required_tlv),
        negative("neg_mixed_elf_in_res.mpb", "negative-mixed-elf-in-res",
                 FRAME_ERR_PACKAGE_INVALID, neg_mixed_elf_in_res),
        negative("neg_mixed_res_in_elf.mpb", "negative-mixed-res-in-elf",
                 FRAME_ERR_PACKAGE_INVALID, neg_mixed_res_in_elf),
        negative("neg_payload_hash.mpb", "negative-payload-hash",
                 FRAME_ERR_HASH_MISMATCH, neg_payload_hash),
        negative("neg_signature.mpb", "negative-signature",
                 FRAME_ERR_SIGNATURE_INVALID, neg_signature, untampered_signature),
        negative("neg_double_fault.mpb", "negative-double-fault",
                 FRAME_ERR_SIGNATURE_INVALID, neg_double_fault, untampered_signature),
        negative("neg_key_id.mpb", "negative-key-id", FRAME_ERR_SIGNATURE_INVALID,
                 neg_key_id),
        negative("neg_target.mpb", "negative-target", FRAME_ERR_TARGET_MISMATCH,
                 neg_target),
        negative("neg_abi.mpb", "negative-abi", FRAME_ERR_ABI_MISMATCH, neg_abi),
        negative("neg_features.mpb", "negative-features", FRAME_ERR_ABI_MISMATCH,
                 neg_features),
        negative("neg_epoch.mpb", "negative-epoch", FRAME_ERR_EPOCH_ROLLBACK, neg_epoch),
    ]

    binaries = {
        "pos_code.mpb": pos_code,
        "pos_resource_only.mpb": pos_resource,
        "neg_magic.mpb": neg_magic,
        "neg_total_size.mpb": neg_total_size,
        "neg_offset_oob.mpb": neg_offset_oob,
        "neg_overlap.mpb": neg_overlap,
        "neg_dup_required.mpb": neg_dup_required,
        "neg_unknown_required_tlv.mpb": neg_unknown_required_tlv,
        "neg_mixed_elf_in_res.mpb": neg_mixed_elf_in_res,
        "neg_mixed_res_in_elf.mpb": neg_mixed_res_in_elf,
        "neg_payload_hash.mpb": neg_payload_hash,
        "neg_signature.mpb": neg_signature,
        "neg_double_fault.mpb": neg_double_fault,
        "neg_key_id.mpb": neg_key_id,
        "neg_target.mpb": neg_target,
        "neg_abi.mpb": neg_abi,
        "neg_features.mpb": neg_features,
        "neg_epoch.mpb": neg_epoch,
    }
    CORPUS_DIR.mkdir(parents=True, exist_ok=True)
    for name, data in binaries.items():
        (CORPUS_DIR / name).write_bytes(data)

    key_id_a = derive_key_id(pub_a)
    lines = [
        "/* GENERATED by poc/host/tests/gen_mpb_corpus.py - do not edit.",
        " * Builder pin: tools/frame_tools/manifest_builder.py @4793cc2.",
        " * expected codes are frame_err_t values from frame_poc/frame_abi.h.",
        " */",
        "#ifndef MPB_TEST_CORPUS_MANIFEST_H",
        "#define MPB_TEST_CORPUS_MANIFEST_H",
        "",
        "#include <stdint.h>",
        "",
        f'#define MPB_CORPUS_KEY_ID_A UINT32_C(0x{key_id_a:08x})',
        "#define MPB_CORPUS_EPOCH_FLOOR UINT32_C(1)",
        "",
        "typedef struct {",
        "    const char* file;",
        "    const char* corpus_class;",
        "    const char* file_sha256_hex;",
        "    int32_t expected_error;",
        "    int16_t package_kind; /* 0 n/a, 1 code, 2 resource-only */",
        "    const char* name;",
        "    const char* version;",
        "    uint32_t epoch;",
        "    uint32_t iram_required_bytes;",
        "    uint32_t max_memory_bytes;",
        "    int16_t payload_count;",
        "    const char* payload_sha256_hex[16];",
        "    const char* kat_digest_hex;",
        "    const char* kat_signature_hex;",
        "} mpb_corpus_entry_t;",
        "",
        "static const mpb_corpus_entry_t k_mpb_corpus[] = {",
    ]
    for item in entries:
        payloads = item["payloads"]
        hexes = [f'"{span[2]}"' for span in payloads] + ["0"] * (16 - len(payloads))
        lines.append(
            "    {"
            f'"{item["file"]}", "{item["corpus_class"]}", "{item["sha256"]}", '
            f'{item["expected"]}, {item["kind"]}, "{item["name"]}", '
            f'"{item["version"]}", UINT32_C({item["epoch"]}), '
            f'UINT32_C({item["iram"]}), UINT32_C({item["max_memory"]}), '
            f'{len(payloads)}, {{{", ".join(hexes)}}}, '
            f'"{item["digest"]}", "{item["signature"]}"'
            "},"
        )
    lines += ["};", "", "#endif", ""]
    (CORPUS_DIR / "corpus_manifest.inc").write_text("\n".join(lines), encoding="utf-8")

    print(f"wrote {len(binaries)} fixtures + corpus_manifest.inc to {CORPUS_DIR}")
    for item in entries:
        print(f'  {item["file"]:28s} expected={item["expected"]:4d} {item["corpus_class"]}')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
