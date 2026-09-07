import hashlib
import json
import struct
from pathlib import Path

import pytest
from cryptography.hazmat.primitives.asymmetric import ec
from frame_tools.manifest_builder import (
    FLAG_RESOURCE_ONLY,
    FRAME_ERR_EPOCH_ROLLBACK,
    FRAME_ERR_HASH_MISMATCH,
    FRAME_ERR_INVALID_ARGUMENT,
    FRAME_ERR_PACKAGE_INVALID,
    FRAME_ERR_SIGNATURE_INVALID,
    FRAME_ERR_TARGET_MISMATCH,
    HEADER_SIZE,
    PAYLOAD_ENTRY_SIZE,
    PAYLOAD_TYPE_ELF,
    PAYLOAD_TYPE_RESOURCE,
    SIGNATURE_RECORD_SIZE,
    TLV_FLAG_REQUIRED,
    Manifest,
    MpbError,
    Payload,
    _load_manifest_json,
    assemble_mpb,
    build_mpb,
    derive_key_id,
    extract_plugin_iram_size,
    load_private_key,
    load_public_key,
    sign_digest_deterministic,
    sign_mpb,
    verify_mpb,
)
from frame_tools.manifest_builder import (
    main as cli_main,
)

KEYS = Path(__file__).resolve().parents[1] / "tools/frame_tools/testdata/keys"
PRIVATE_PEM = KEYS / "poc_a_test_signing_key.pem"
PUBLIC_PEM = KEYS / "poc_a_test_signing_key.pub.pem"
IRAM_ADDR = 0x40378000
IRAM_SIZE = 0x1234


def synthetic_elf(iram_size: int = IRAM_SIZE, iram_addr: int = IRAM_ADDR) -> bytes:
    """Minimal ELF32 LE Xtensa image whose section table holds .plugin_iram."""
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


@pytest.fixture(scope="module")
def signing_key() -> ec.EllipticCurvePrivateKey:
    return load_private_key(PRIVATE_PEM)


@pytest.fixture(scope="module")
def public_key() -> ec.EllipticCurvePublicKey:
    return load_public_key(PUBLIC_PEM)


@pytest.fixture()
def code_manifest() -> Manifest:
    manifest = Manifest("poc-a-demo", "1.2.3")
    manifest.set("max_memory_bytes", 262144)
    manifest.set("max_managed_tasks", 2)
    manifest.set("requires", ("frame/core>=1.0.0",))
    return manifest


def build_code(m: Manifest, key: ec.EllipticCurvePrivateKey) -> bytes:
    return build_mpb(m, [Payload(PAYLOAD_TYPE_ELF, synthetic_elf())], signing_key=key)


def hostile_signed(
    manifest_bytes: bytes, key: ec.EllipticCurvePrivateKey, *, flags: int = FLAG_RESOURCE_ONLY
) -> bytes:
    """Sign a container over arbitrary manifest bytes (invalid structures included)."""
    payloads = [Payload(PAYLOAD_TYPE_RESOURCE, b"resource-bytes")]
    container = assemble_mpb(
        manifest_bytes=manifest_bytes,
        payloads=payloads,
        key_id=derive_key_id(key.public_key()),
        flags=FLAG_RESOURCE_ONLY if flags else 0,
    )
    table_end = struct.unpack_from("<I", container, 52)[0] + PAYLOAD_ENTRY_SIZE
    digest = hashlib.sha256(container[:table_end]).digest()
    signature = sign_digest_deterministic(key, digest)
    offset = struct.unpack_from("<I", container, 60)[0]
    container = bytearray(container)
    container[offset + 8 : offset + SIGNATURE_RECORD_SIZE] = signature
    return bytes(container)


def test_extracted_iram_matches_synthetic_section_table() -> None:
    assert extract_plugin_iram_size(synthetic_elf()) == IRAM_SIZE


def test_deterministic_build_is_byte_identical(signing_key: ec.EllipticCurvePrivateKey) -> None:
    manifest_a = Manifest("poc-a-demo", "1.2.3")
    manifest_b = Manifest("poc-a-demo", "1.2.3")
    first = build_code(manifest_a, signing_key)
    second = build_code(manifest_b, signing_key)
    assert first == second


def test_code_mpb_roundtrip_verify(
    code_manifest: Manifest,
    signing_key: ec.EllipticCurvePrivateKey,
    public_key: ec.EllipticCurvePublicKey,
) -> None:
    container = build_code(code_manifest, signing_key)
    result = verify_mpb(container, public_key, epoch_floor=1)
    assert result.package_type == "code"
    assert result.name == "poc-a-demo"
    assert result.version == "1.2.3"
    assert result.iram_required_bytes == IRAM_SIZE
    assert result.key_id == derive_key_id(public_key)
    assert result.payload_sha256[0] == hashlib.sha256(synthetic_elf()).digest()


def test_resource_only_roundtrip_verify(
    signing_key: ec.EllipticCurvePrivateKey, public_key: ec.EllipticCurvePublicKey
) -> None:
    payloads = [
        Payload(PAYLOAD_TYPE_RESOURCE, b"font-bitmap"),
        Payload(PAYLOAD_TYPE_RESOURCE, b"image-data-8", alignment=8),
    ]
    container = build_mpb(Manifest("res-pack", "0.1.0"), payloads, signing_key=signing_key)
    result = verify_mpb(container, public_key)
    assert result.package_type == "resource-only"
    assert result.payload_count == 2
    assert result.iram_required_bytes == 0


def test_mixed_elf_and_resource_rejected(
    signing_key: ec.EllipticCurvePrivateKey,
) -> None:
    payloads = [
        Payload(PAYLOAD_TYPE_ELF, synthetic_elf()),
        Payload(PAYLOAD_TYPE_RESOURCE, b"loose-asset"),
    ]
    with pytest.raises(MpbError) as info:
        build_mpb(Manifest("mixed", "1.0.0"), payloads, signing_key=signing_key)
    assert "single-type" in str(info.value)


def test_two_elf_payloads_rejected(signing_key: ec.EllipticCurvePrivateKey) -> None:
    payloads = [
        Payload(PAYLOAD_TYPE_ELF, synthetic_elf()),
        Payload(PAYLOAD_TYPE_ELF, synthetic_elf(0x2000)),
    ]
    with pytest.raises(MpbError, match="exactly one elf"):
        build_mpb(Manifest("two-elfs", "1.0.0"), payloads, signing_key=signing_key)


def test_duplicate_manifest_tlv_rejected(
    code_manifest: Manifest, signing_key: ec.EllipticCurvePrivateKey
) -> None:
    name = tlv(1, TLV_FLAG_REQUIRED, b"dup-name")
    version = tlv(2, TLV_FLAG_REQUIRED, b"1.0.0")
    container = hostile_signed(name + name + version, signing_key)
    with pytest.raises(MpbError) as info:
        verify_mpb(container, signing_key.public_key())
    assert info.value.code == FRAME_ERR_PACKAGE_INVALID
    assert "duplicated" in info.value.message


def test_iram_mismatch_between_declared_and_extracted_rejected(
    signing_key: ec.EllipticCurvePrivateKey,
) -> None:
    manifest = Manifest("bad-iram", "1.0.0")
    manifest.set("iram_required_bytes", IRAM_SIZE + 0x40)
    with pytest.raises(MpbError) as info:
        build_mpb(manifest, [Payload(PAYLOAD_TYPE_ELF, synthetic_elf())], signing_key=signing_key)
    assert info.value.code == FRAME_ERR_INVALID_ARGUMENT


def test_one_byte_header_tamper_fails_signature(
    code_manifest: Manifest,
    signing_key: ec.EllipticCurvePrivateKey,
    public_key: ec.EllipticCurvePublicKey,
) -> None:
    container = bytearray(build_code(code_manifest, signing_key))
    epoch_offset = HEADER_SIZE - 16 - 4
    container[epoch_offset] ^= 0x01
    with pytest.raises(MpbError) as info:
        verify_mpb(bytes(container), public_key)
    assert info.value.code == FRAME_ERR_SIGNATURE_INVALID


def test_one_byte_payload_tamper_fails_hash(
    code_manifest: Manifest,
    signing_key: ec.EllipticCurvePrivateKey,
    public_key: ec.EllipticCurvePublicKey,
) -> None:
    container = bytearray(build_code(code_manifest, signing_key))
    signature_offset = struct.unpack_from("<I", container, 60)[0]
    container[signature_offset - 1] ^= 0x01
    with pytest.raises(MpbError) as info:
        verify_mpb(bytes(container), public_key)
    assert info.value.code == FRAME_ERR_HASH_MISMATCH


def test_max_memory_bytes_over_512kib_rejected(signing_key: ec.EllipticCurvePrivateKey) -> None:
    over = Manifest("mem-over", "1.0.0")
    over.set("max_memory_bytes", 512 * 1024 + 1)
    with pytest.raises(MpbError, match="hard maximum"):
        build_mpb(over, [Payload(PAYLOAD_TYPE_ELF, synthetic_elf())], signing_key=signing_key)
    at_limit = Manifest("mem-limit", "1.0.0")
    at_limit.set("max_memory_bytes", 512 * 1024)
    build_mpb(at_limit, [Payload(PAYLOAD_TYPE_ELF, synthetic_elf())], signing_key=signing_key)


def test_unknown_required_tlv_rejected(
    signing_key: ec.EllipticCurvePrivateKey,
) -> None:
    name = tlv(1, TLV_FLAG_REQUIRED, b"unknown-probe")
    version = tlv(2, TLV_FLAG_REQUIRED, b"1.0.0")
    unknown_required = tlv(0x4242, TLV_FLAG_REQUIRED, b"\x01\x02\x03\x04")
    container = hostile_signed(name + version + unknown_required, signing_key)
    with pytest.raises(MpbError) as info:
        verify_mpb(container, signing_key.public_key())
    assert info.value.code == FRAME_ERR_PACKAGE_INVALID
    assert "unknown required" in info.value.message

    unknown_optional = tlv(0x4243, 0x0000, b"\x00\x00\x00\x00")
    container = hostile_signed(name + version + unknown_optional, signing_key)
    assert verify_mpb(container, signing_key.public_key()).package_type == "resource-only"


def test_resource_count_above_sixteen_rejected(signing_key: ec.EllipticCurvePrivateKey) -> None:
    payloads = [Payload(PAYLOAD_TYPE_RESOURCE, b"x") for _ in range(17)]
    with pytest.raises(MpbError, match="1..16"):
        build_mpb(Manifest("too-many", "1.0.0"), payloads, signing_key=signing_key)


def test_non_xtensa_elf_rejected(signing_key: ec.EllipticCurvePrivateKey) -> None:
    wrong_machine = bytearray(synthetic_elf())
    struct.pack_into("<H", wrong_machine, 18, 40)
    with pytest.raises(MpbError, match="Xtensa"):
        build_mpb(
            Manifest("wrong-machine", "1.0.0"),
            [Payload(PAYLOAD_TYPE_ELF, bytes(wrong_machine))],
            signing_key=signing_key,
        )


def test_epoch_rollback_and_target_mismatch_detected(
    code_manifest: Manifest,
    signing_key: ec.EllipticCurvePrivateKey,
    public_key: ec.EllipticCurvePublicKey,
) -> None:
    container = build_code(code_manifest, signing_key)
    with pytest.raises(MpbError) as info:
        verify_mpb(container, public_key, epoch_floor=2)
    assert info.value.code == FRAME_ERR_EPOCH_ROLLBACK
    with pytest.raises(MpbError) as info:
        verify_mpb(container, public_key, expected_target=0xDEADBEEF)
    assert info.value.code == FRAME_ERR_TARGET_MISMATCH


def test_unsigned_build_then_sign_equals_direct_build(
    code_manifest: Manifest, signing_key: ec.EllipticCurvePrivateKey
) -> None:
    direct = build_code(code_manifest, signing_key)
    unsigned = build_mpb(
        code_manifest,
        [Payload(PAYLOAD_TYPE_ELF, synthetic_elf())],
        key_id=derive_key_id(signing_key.public_key()),
    )
    assert sign_mpb(unsigned, signing_key) == direct


def test_manifest_json_duplicate_keys_rejected(tmp_path: Path) -> None:
    payload = tmp_path / "dup.json"
    payload.write_text(
        json.dumps({"name": "x", "version": "1.0.0", "max_timers": 1}).replace(
            "}", ', "max_timers": 2}'
        ),
        encoding="utf-8",
    )
    with pytest.raises(MpbError, match="duplicate manifest field"):
        _load_manifest_json(payload)


def test_cli_build_and_verify_roundtrip(tmp_path: Path) -> None:
    elf = tmp_path / "plugin.elf"
    elf.write_bytes(synthetic_elf())
    extra = tmp_path / "manifest.json"
    extra.write_text(
        json.dumps(
            {
                "name": "cli-demo",
                "version": "2.0.1",
                "max_memory_bytes": 131072,
                "strand_queue_capacity": 32,
                "max_outstanding_operations": 16,
                "state_schema_id": "00" * 16,
                "state_schema_version": 0,
            }
        ),
        encoding="utf-8",
    )
    container = tmp_path / "plugin.mpb"
    assert (
        cli_main(
            [
                "build",
                "--elf",
                str(elf),
                "--name",
                "cli-demo",
                "--version",
                "2.0.1",
                "--manifest-json",
                str(extra),
                "--key",
                str(PRIVATE_PEM),
                "--output",
                str(container),
            ]
        )
        == 0
    )
    assert (
        cli_main(
            ["verify", "--input", str(container), "--pubkey", str(PUBLIC_PEM), "--epoch-floor", "1"]
        )
        == 0
    )
    result = verify_mpb(container.read_bytes(), load_public_key(PUBLIC_PEM))
    assert result.package_type == "code"
    assert result.fields["strand_queue_capacity"] == 32
