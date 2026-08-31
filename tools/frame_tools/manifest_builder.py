"""Deterministic little-endian MPB container builder and reference verifier.

Implements appendix A of _doc/project.md (section 9) with the v4.2 strict
single-type semantics of PLUG-001. This module is the reference oracle that
the PoC-A C parser (task T3) must match byte for byte. Run
``layout_tables()`` to print the frozen container layout contract.

Container region order (section 9.1): fixed_header | manifest | payload_table |
payloads | signature_record. The manifest immediately follows the fixed
header, the payload table immediately follows the manifest, payloads live in
the window bounded by the payload table end and the signature offset, and the
signature record ends exactly at total_size. Undescribed trailing bytes are
rejected. Alignment padding inside the payload window is not covered by any
digest by design: payloads are authenticated through the signed table's
per-entry SHA-256, and parsers must never interpret padding bytes.

Signing input is SHA-256 over the concatenated raw bytes of fixed_header +
manifest + payload_table (section 9.5). The signature is ECDSA P-256 over
that digest, encoded as IEEE P1363 r||s (64 bytes). Signing uses RFC 6979
deterministic nonces so that identical inputs produce byte-identical
containers; every produced signature is re-checked with the `cryptography`
verifier before the container is emitted.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac as hmac_mod
import json
import re
import struct
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import Prehashed, encode_dss_signature

MAGIC = 0x4D504246
FORMAT_MAJOR = 1
FORMAT_MINOR = 0

TARGET_ESP32S3 = 0x33505345
FRAME_ABI_MAJOR = 1
FRAME_ABI_MINOR = 0

FLAG_RESOURCE_ONLY = 0x00000001
KNOWN_HEADER_FLAGS = FLAG_RESOURCE_ONLY

SIG_ALGORITHM_ECDSA_P256_SHA256 = 1
SIG_RECORD_VERSION = 1

PAYLOAD_TYPE_ELF = 1
PAYLOAD_TYPE_RESOURCE = 2
PAYLOAD_FLAG_REQUIRED = 0x0001
PAYLOAD_FLAG_COMPRESSED = 0x0002
KNOWN_PAYLOAD_FLAGS = PAYLOAD_FLAG_REQUIRED | PAYLOAD_FLAG_COMPRESSED

MAX_PAYLOAD_COUNT = 16
MAX_PLUGIN_MEMORY_BYTES = 512 * 1024
MAX_MANAGED_TASKS = 8
MAX_MANAGED_TASK_PRIORITY = 4
MAX_NAME_BYTES = 64
MAX_VERSION_BYTES = 32
MAX_REQUIRES = 32
MAX_REQUIRE_BYTES = 96

DEFAULT_ELF_ALIGNMENT = 4
DEFAULT_RESOURCE_ALIGNMENT = 4
MAX_ALIGNMENT = 4096

FRAME_ERR_ABI_MISMATCH = -7
FRAME_ERR_SIGNATURE_INVALID = -17
FRAME_ERR_PACKAGE_INVALID = -18
FRAME_ERR_HASH_MISMATCH = -19
FRAME_ERR_INVALID_ARGUMENT = -28
FRAME_ERR_EPOCH_ROLLBACK = -34
FRAME_ERR_TARGET_MISMATCH = -35

EM_XTENSA = 94
ELF_MAGIC = b"\x7fELF"

HEADER_FIELDS: tuple[tuple[str, str], ...] = (
    ("magic", "I"),
    ("format_major", "H"),
    ("format_minor", "H"),
    ("header_size", "I"),
    ("total_size", "I"),
    ("flags", "I"),
    ("target_id", "I"),
    ("core_abi_major", "H"),
    ("core_abi_minor", "H"),
    ("required_features", "Q"),
    ("optional_features", "Q"),
    ("manifest_offset", "I"),
    ("manifest_length", "I"),
    ("payload_table_offset", "I"),
    ("payload_count", "H"),
    ("payload_entry_size", "H"),
    ("signature_offset", "I"),
    ("signature_length", "I"),
    ("key_id", "I"),
    ("security_epoch", "I"),
    ("reserved", "4I"),
)

PAYLOAD_ENTRY_FIELDS: tuple[tuple[str, str], ...] = (
    ("payload_type", "H"),
    ("flags", "H"),
    ("offset", "I"),
    ("length", "I"),
    ("unpacked_length", "I"),
    ("alignment", "I"),
    ("sha256", "32s"),
    ("reserved", "2I"),
)

SIGNATURE_RECORD_FIELDS: tuple[tuple[str, str], ...] = (
    ("algorithm", "H"),
    ("record_version", "H"),
    ("key_id", "I"),
    ("signature", "64s"),
)

_HEADER_FMT = "<" + "".join(fmt for _name, fmt in HEADER_FIELDS)
_PAYLOAD_ENTRY_FMT = "<" + "".join(fmt for _name, fmt in PAYLOAD_ENTRY_FIELDS)
_SIGNATURE_FMT = "<" + "".join(fmt for _name, fmt in SIGNATURE_RECORD_FIELDS)
HEADER_SIZE = struct.calcsize(_HEADER_FMT)
PAYLOAD_ENTRY_SIZE = struct.calcsize(_PAYLOAD_ENTRY_FMT)
SIGNATURE_RECORD_SIZE = struct.calcsize(_SIGNATURE_FMT)

TLV_HEADER_FMT = "<HHI"
TLV_HEADER_SIZE = struct.calcsize(TLV_HEADER_FMT)
TLV_FLAG_REQUIRED = 0x0001
KNOWN_TLV_FLAGS = TLV_FLAG_REQUIRED


@dataclass(frozen=True)
class MpbError(Exception):
    """Error with a frame_err_t code from _doc/project.md section 8."""

    code: int
    message: str

    def __str__(self) -> str:
        return f"[{self.code}] {self.message}"


@dataclass(frozen=True)
class _FieldDesc:
    field_id: int
    name: str
    kind: str


_FIELD_DESCS: tuple[_FieldDesc, ...] = (
    _FieldDesc(1, "name", "utf8_name"),
    _FieldDesc(2, "version", "semver"),
    _FieldDesc(3, "requires", "str_list"),
    _FieldDesc(4, "max_memory_bytes", "u32"),
    _FieldDesc(5, "iram_required_bytes", "u32"),
    _FieldDesc(6, "core_affinity", "affinity"),
    _FieldDesc(7, "strand_queue_capacity", "u32"),
    _FieldDesc(8, "max_outstanding_operations", "u32"),
    _FieldDesc(9, "max_timers", "u32"),
    _FieldDesc(10, "max_managed_tasks", "u32"),
    _FieldDesc(11, "managed_task_priority", "u32"),
    _FieldDesc(12, "managed_task_stack_size", "u32"),
    _FieldDesc(13, "heartbeat_interval_ms", "u32"),
    _FieldDesc(14, "event_rate_limit", "u32"),
    _FieldDesc(15, "max_event_subscriptions", "u32"),
    _FieldDesc(16, "max_event_message_bytes", "u32"),
    _FieldDesc(17, "max_event_buffer_bytes", "u32"),
    _FieldDesc(18, "max_outstanding_rpc", "u32"),
    _FieldDesc(19, "max_display_commands", "u32"),
    _FieldDesc(20, "max_display_bytes", "u32"),
    _FieldDesc(21, "max_storage_requests", "u32"),
    _FieldDesc(22, "max_storage_io_bytes", "u32"),
    _FieldDesc(23, "max_storage_copy_bytes", "u32"),
    _FieldDesc(24, "max_tcp_connections", "u32"),
    _FieldDesc(25, "max_udp_sockets", "u32"),
    _FieldDesc(26, "max_network_requests", "u32"),
    _FieldDesc(27, "max_network_buffer_bytes", "u32"),
    _FieldDesc(28, "max_network_io_bytes", "u32"),
    _FieldDesc(29, "max_log_records_per_second", "u32"),
    _FieldDesc(30, "max_log_buffer_bytes", "u32"),
    _FieldDesc(31, "max_resource_leases", "u32"),
    _FieldDesc(32, "max_update_backlog_events", "u32"),
    _FieldDesc(33, "max_update_backlog_bytes", "u32"),
    _FieldDesc(34, "state_schema_id", "schema_id"),
    _FieldDesc(35, "state_schema_version", "u32"),
)

_FIELDS_BY_NAME: dict[str, _FieldDesc] = {desc.name: desc for desc in _FIELD_DESCS}
_FIELDS_BY_ID: dict[int, _FieldDesc] = {desc.field_id: desc for desc in _FIELD_DESCS}

FieldValue = int | str | tuple[str, ...] | bytes
_SEMVER_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")


def _invalid(message: str) -> MpbError:
    return MpbError(FRAME_ERR_INVALID_ARGUMENT, message)


def _package_invalid(message: str) -> MpbError:
    return MpbError(FRAME_ERR_PACKAGE_INVALID, message)


def _check_value(desc: _FieldDesc, value: FieldValue) -> None:
    if desc.kind in ("utf8_name", "semver"):
        if not isinstance(value, str):
            raise _invalid(f"manifest field {desc.name} must be a string")
        encoded = value.encode("utf-8")
        if desc.kind == "utf8_name":
            if not 1 <= len(encoded) <= MAX_NAME_BYTES:
                raise _invalid(f"manifest field name must be 1..{MAX_NAME_BYTES} bytes")
        else:
            if len(encoded) > MAX_VERSION_BYTES or not _SEMVER_RE.match(value):
                raise _invalid("manifest field version must be strict major.minor.patch")
    elif desc.kind == "str_list":
        if not isinstance(value, tuple):
            raise _invalid(f"manifest field {desc.name} must be a sequence of strings")
        if len(value) > MAX_REQUIRES:
            raise _invalid(f"manifest field requires allows at most {MAX_REQUIRES} entries")
        for entry in value:
            if (
                not isinstance(entry, str)
                or not 1 <= len(entry.encode("utf-8")) <= MAX_REQUIRE_BYTES
            ):
                raise _invalid("each requires entry must be 1..96 utf-8 bytes")
    elif desc.kind == "u32":
        if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= 0xFFFFFFFF:
            raise _invalid(f"manifest field {desc.name} must be a uint32")
    elif desc.kind == "affinity":
        if not isinstance(value, int) or isinstance(value, bool) or value not in (0, 1, 2):
            raise _invalid("manifest field core_affinity must be 0, 1 or 2")
    elif desc.kind == "schema_id":
        if not isinstance(value, (bytes, bytearray)) or len(value) != 16:
            raise _invalid("manifest field state_schema_id must be 16 bytes")


def _encode_value(desc: _FieldDesc, value: FieldValue) -> bytes:
    _check_value(desc, value)
    if desc.kind in ("utf8_name", "semver"):
        if not isinstance(value, str):
            raise _invalid(f"manifest field {desc.name} must be a string")
        return value.encode("utf-8")
    if desc.kind == "str_list":
        if not isinstance(value, tuple):
            raise _invalid(f"manifest field {desc.name} must be a sequence of strings")
        out = [struct.pack("<B", len(value))]
        for entry in value:
            encoded = entry.encode("utf-8")
            out.append(struct.pack("<H", len(encoded)))
            out.append(encoded)
        return b"".join(out)
    if desc.kind == "u32":
        if not isinstance(value, int):
            raise _invalid(f"manifest field {desc.name} must be a uint32")
        return struct.pack("<I", value)
    if desc.kind == "affinity":
        if not isinstance(value, int):
            raise _invalid("manifest field core_affinity must be 0, 1 or 2")
        return struct.pack("<B", value)
    if not isinstance(value, (bytes, bytearray)):
        raise _invalid("manifest field state_schema_id must be 16 bytes")
    return bytes(value)


def _validate_cross_fields(values: Mapping[str, FieldValue], code: int) -> None:
    def fail(message: str) -> MpbError:
        return MpbError(code, message)

    max_memory = values.get("max_memory_bytes")
    if isinstance(max_memory, int) and max_memory > MAX_PLUGIN_MEMORY_BYTES:
        raise fail(
            f"max_memory_bytes {max_memory} exceeds the {MAX_PLUGIN_MEMORY_BYTES} hard maximum"
        )
    managed_tasks = values.get("max_managed_tasks")
    if isinstance(managed_tasks, int) and managed_tasks > MAX_MANAGED_TASKS:
        raise fail(f"max_managed_tasks {managed_tasks} exceeds {MAX_MANAGED_TASKS}")
    priority = values.get("managed_task_priority")
    if isinstance(priority, int) and priority > MAX_MANAGED_TASK_PRIORITY:
        raise fail(f"managed_task_priority {priority} exceeds {MAX_MANAGED_TASK_PRIORITY}")
    queue = values.get("strand_queue_capacity")
    outstanding = values.get("max_outstanding_operations")
    if isinstance(queue, int) and isinstance(outstanding, int) and outstanding > queue:
        raise fail("max_outstanding_operations must be <= strand_queue_capacity")
    for bounded in ("max_outstanding_rpc", "max_storage_requests", "max_network_requests"):
        value = values.get(bounded)
        if isinstance(value, int) and isinstance(outstanding, int) and value > outstanding:
            raise fail(f"{bounded} must be <= max_outstanding_operations")
    schema_id = values.get("state_schema_id")
    schema_version = values.get("state_schema_version")
    stateless = not isinstance(schema_id, bytes) or schema_id == b"\x00" * 16
    if stateless:
        if isinstance(schema_version, int) and schema_version != 0:
            raise fail("state_schema_version must be 0 when state_schema_id is all zero")
    elif not isinstance(schema_version, int) or schema_version < 1:
        raise fail("state_schema_version must be >= 1 when state_schema_id is set")


class Manifest:
    """Typed manifest field set for one MPB build."""

    def __init__(self, name: str, version: str) -> None:
        self._values: dict[str, FieldValue] = {}
        self.set("name", name)
        self.set("version", version)

    @classmethod
    def from_mapping(cls, data: Mapping[str, object]) -> Manifest:
        name = data.get("name")
        version = data.get("version")
        if not isinstance(name, str) or not isinstance(version, str):
            raise _invalid("manifest mapping requires string 'name' and 'version'")
        manifest = cls(name, version)
        for key, value in data.items():
            if key in ("name", "version"):
                continue
            if key not in _FIELDS_BY_NAME:
                raise _invalid(f"unknown manifest field {key!r}")
            if key == "state_schema_id":
                if not isinstance(value, str):
                    raise _invalid("state_schema_id must be a 32 char hex string")
                try:
                    value = bytes.fromhex(value)
                except ValueError as error:
                    raise _invalid(f"state_schema_id is not hex: {error}") from error
            elif key == "requires":
                if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
                    raise _invalid("requires must be a list of strings")
                value = tuple(value)
            elif not isinstance(value, int) or isinstance(value, bool):
                raise _invalid(f"manifest field {key} must be an integer")
            manifest.set(key, value)
        return manifest

    def set(self, name: str, value: FieldValue) -> None:
        desc = _FIELDS_BY_NAME.get(name)
        if desc is None:
            raise _invalid(f"unknown manifest field {name!r}")
        _check_value(desc, value)
        self._values[name] = value

    def get(self, name: str) -> FieldValue | None:
        return self._values.get(name)

    def require(self, name: str) -> FieldValue:
        value = self._values.get(name)
        if value is None:
            raise _invalid(f"manifest field {name} is required")
        return value

    @property
    def values(self) -> Mapping[str, FieldValue]:
        return self._values

    def validate(self) -> None:
        _validate_cross_fields(self._values, FRAME_ERR_INVALID_ARGUMENT)

    def encode(self) -> bytes:
        self.validate()
        chunks: list[bytes] = []
        for desc in sorted(_FIELD_DESCS, key=lambda d: d.field_id):
            value = self._values.get(desc.name)
            if value is None:
                continue
            body = _encode_value(desc, value)
            required = TLV_FLAG_REQUIRED if desc.name in ("name", "version") else 0
            chunks.append(struct.pack(TLV_HEADER_FMT, desc.field_id, required, len(body)))
            chunks.append(body)
        return b"".join(chunks)


@dataclass(frozen=True)
class Payload:
    kind: int
    data: bytes
    alignment: int = DEFAULT_ELF_ALIGNMENT

    def __post_init__(self) -> None:
        if self.kind not in (PAYLOAD_TYPE_ELF, PAYLOAD_TYPE_RESOURCE):
            raise _invalid(f"unknown payload type {self.kind}")
        if not 1 <= self.alignment <= MAX_ALIGNMENT or self.alignment & (self.alignment - 1):
            raise _invalid(f"alignment {self.alignment} must be a power of two <= {MAX_ALIGNMENT}")


@dataclass(frozen=True)
class VerifyResult:
    package_type: str
    name: str
    version: str
    key_id: int
    security_epoch: int
    iram_required_bytes: int
    payload_count: int
    payload_sha256: tuple[bytes, ...]
    fields: dict[str, FieldValue] = field(default_factory=dict)


def derive_key_id(public_key: ec.EllipticCurvePublicKey) -> int:
    der = public_key.public_bytes(
        serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo
    )
    return int.from_bytes(hashlib.sha256(der).digest()[:4], "big")


def load_private_key(path: Path) -> ec.EllipticCurvePrivateKey:
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
        raise _invalid(f"{path}: expected an ECDSA P-256 (secp256r1) private key")
    return key


def load_public_key(path: Path) -> ec.EllipticCurvePublicKey:
    key = serialization.load_pem_public_key(path.read_bytes())
    if not isinstance(key, ec.EllipticCurvePublicKey) or not isinstance(key.curve, ec.SECP256R1):
        raise _invalid(f"{path}: expected an ECDSA P-256 (secp256r1) public key")
    return key


_P256_P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
_P256_A = _P256_P - 3
_P256_N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
_P256_G = (
    0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
    0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5,
)

_Jacobian = tuple[int, int, int] | None


def _point_double(point: _Jacobian) -> _Jacobian:
    if point is None:
        return None
    x, y, z = point
    y2 = y * y % _P256_P
    s = 4 * x * y2 % _P256_P
    m = (3 * x * x + _P256_A * pow(z, 4, _P256_P)) % _P256_P
    x3 = (m * m - 2 * s) % _P256_P
    y3 = (m * (s - x3) - 8 * y2 * y2) % _P256_P
    z3 = 2 * y * z % _P256_P
    return x3, y3, z3


def _point_add(p1: _Jacobian, p2: _Jacobian) -> _Jacobian:
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1, z1 = p1
    x2, y2, z2 = p2
    z1_2 = z1 * z1 % _P256_P
    z2_2 = z2 * z2 % _P256_P
    u1 = x1 * z2_2 % _P256_P
    u2 = x2 * z1_2 % _P256_P
    s1 = y1 * z2_2 * z2 % _P256_P
    s2 = y2 * z1_2 * z1 % _P256_P
    if u1 == u2:
        if s1 != s2:
            return None
        return _point_double(p1)
    h = (u2 - u1) % _P256_P
    r = (s2 - s1) % _P256_P
    h2 = h * h % _P256_P
    h3 = h2 * h % _P256_P
    x3 = (r * r - h3 - 2 * u1 * h2) % _P256_P
    y3 = (r * (u1 * h2 - x3) - s1 * h3) % _P256_P
    z3 = h * z1 * z2 % _P256_P
    return x3, y3, z3


def _scalar_base_mult(k: int) -> tuple[int, int]:
    result: _Jacobian = None
    addend: _Jacobian = (_P256_G[0], _P256_G[1], 1)
    for bit in bin(k)[2:]:
        result = _point_double(result)
        if bit == "1":
            result = _point_add(result, addend)
    assert result is not None
    x, y, z = result
    z_inv = pow(z, -1, _P256_P)
    return x * pow(z_inv, 2, _P256_P) % _P256_P, y * pow(z_inv, 3, _P256_P) % _P256_P


def _rfc6979_nonces(private: int, digest: bytes):
    v = b"\x01" * 32
    k = b"\x00" * 32
    x = private.to_bytes(32, "big")
    k = hmac_mod.new(k, v + b"\x00" + x + digest, hashlib.sha256).digest()
    v = hmac_mod.new(k, v, hashlib.sha256).digest()
    k = hmac_mod.new(k, v + b"\x01" + x + digest, hashlib.sha256).digest()
    v = hmac_mod.new(k, v, hashlib.sha256).digest()
    while True:
        v = hmac_mod.new(k, v, hashlib.sha256).digest()
        nonce = int.from_bytes(v, "big")
        if 1 <= nonce < _P256_N:
            yield nonce
        k = hmac_mod.new(k, v + b"\x00", hashlib.sha256).digest()
        v = hmac_mod.new(k, v, hashlib.sha256).digest()


def sign_digest_deterministic(private_key: ec.EllipticCurvePrivateKey, digest: bytes) -> bytes:
    """RFC 6979 deterministic ECDSA P-256 over a 32 byte digest, P1363 r||s."""
    private = private_key.private_numbers().private_value
    z = int.from_bytes(digest, "big")
    for nonce in _rfc6979_nonces(private, digest):
        r, _y = _scalar_base_mult(nonce)
        r %= _P256_N
        if r == 0:
            continue
        s = pow(nonce, -1, _P256_N) * (z + r * private) % _P256_N
        if s == 0:
            continue
        return r.to_bytes(32, "big") + s.to_bytes(32, "big")
    raise MpbError(FRAME_ERR_SIGNATURE_INVALID, "rfc6979 nonce exhausted")


def _verify_signature(
    public_key: ec.EllipticCurvePublicKey, digest: bytes, signature: bytes
) -> bool:
    r = int.from_bytes(signature[:32], "big")
    s = int.from_bytes(signature[32:], "big")
    try:
        public_key.verify(encode_dss_signature(r, s), digest, ec.ECDSA(Prehashed(hashes.SHA256())))
    except InvalidSignature:
        return False
    return True


def extract_plugin_iram_size(elf: bytes) -> int:
    """Return the .plugin_iram section size of an ELF32 little-endian image."""
    if len(elf) < 52 or elf[:4] != ELF_MAGIC:
        raise _invalid("elf payload is not an ELF image")
    if elf[4] != 1 or elf[5] != 1 or elf[6] != 1:
        raise _invalid("elf payload must be ELF32 little-endian version 1")
    e_machine = struct.unpack_from("<H", elf, 18)[0]
    if e_machine != EM_XTENSA:
        raise _invalid(f"elf payload machine {e_machine} is not Xtensa ({EM_XTENSA})")
    shoff = struct.unpack_from("<I", elf, 32)[0]
    shentsize = struct.unpack_from("<H", elf, 46)[0]
    shnum = struct.unpack_from("<H", elf, 48)[0]
    shstrndx = struct.unpack_from("<H", elf, 50)[0]
    if shentsize != 40 or shnum == 0:
        raise _invalid("elf payload has no usable section table")
    if shoff > len(elf) or shnum > (len(elf) - shoff) // shentsize:
        raise _invalid("elf section table is out of bounds")
    if shstrndx >= shnum:
        raise _invalid("elf section string table index is out of bounds")
    headers = [struct.unpack_from("<10I", elf, shoff + index * shentsize) for index in range(shnum)]
    str_offset = headers[shstrndx][4]
    str_size = headers[shstrndx][5]
    if str_offset > len(elf) or str_size > len(elf) - str_offset:
        raise _invalid("elf section string table is out of bounds")
    string_table = elf[str_offset : str_offset + str_size]
    sizes = [
        header[5] for header in headers if _string_at(string_table, header[0]) == b".plugin_iram"
    ]
    if len(sizes) > 1:
        raise _invalid("elf payload declares .plugin_iram more than once")
    return sizes[0] if sizes else 0


def _string_at(table: bytes, offset: int) -> bytes:
    end = table.find(b"\x00", offset)
    if end < 0:
        end = len(table)
    return table[offset:end]


def _check_payload_kinds(payloads: Sequence[Payload]) -> int:
    elf_count = sum(1 for p in payloads if p.kind == PAYLOAD_TYPE_ELF)
    resource_count = sum(1 for p in payloads if p.kind == PAYLOAD_TYPE_RESOURCE)
    if not 1 <= len(payloads) <= MAX_PAYLOAD_COUNT:
        raise _invalid(f"payload count must be 1..{MAX_PAYLOAD_COUNT}")
    if elf_count > 1:
        raise _invalid("code mpb must contain exactly one elf payload")
    if elf_count and resource_count:
        raise _invalid("mixed elf and resource payloads violate strict single-type (PLUG-001)")
    if not elf_count and not resource_count:
        raise _invalid("package must contain elf or resource payloads")
    return FLAG_RESOURCE_ONLY if resource_count else 0


def build_mpb(
    manifest: Manifest,
    payloads: Sequence[Payload],
    *,
    key_id: int | None = None,
    signing_key: ec.EllipticCurvePrivateKey | None = None,
    security_epoch: int = 1,
    core_abi_major: int = FRAME_ABI_MAJOR,
    core_abi_minor: int = FRAME_ABI_MINOR,
    required_features: int = 0,
    optional_features: int = 0,
    target_id: int = TARGET_ESP32S3,
) -> bytes:
    """Assemble a deterministic MPB container; signed when a key is given."""
    if signing_key is not None:
        derived = derive_key_id(signing_key.public_key())
        if key_id is not None and key_id != derived:
            raise _invalid(f"key_id {key_id} does not match signing key {derived}")
        key_id = derived
    if key_id is None:
        raise _invalid("signing requires --key or an explicit --key-id")
    if not 0 <= key_id <= 0xFFFFFFFF:
        raise _invalid("key_id must be a uint32")
    if not 0 <= security_epoch <= 0xFFFFFFFF:
        raise _invalid("security_epoch must be a uint32")

    flags = _check_payload_kinds(payloads)
    iram_extracted: int | None = None
    for payload in payloads:
        if payload.kind == PAYLOAD_TYPE_ELF:
            iram_extracted = extract_plugin_iram_size(payload.data)
    declared = manifest.get("iram_required_bytes")
    if iram_extracted is not None:
        if declared is not None and declared != iram_extracted:
            raise _invalid(
                f"declared iram_required_bytes {declared} != extracted .plugin_iram "
                f"size {iram_extracted}"
            )
        manifest.set("iram_required_bytes", iram_extracted)
    elif declared is not None and declared != 0:
        raise _invalid("resource-only mpb cannot declare iram_required_bytes")

    return assemble_mpb(
        manifest_bytes=manifest.encode(),
        payloads=payloads,
        key_id=key_id,
        flags=flags,
        security_epoch=security_epoch,
        core_abi_major=core_abi_major,
        core_abi_minor=core_abi_minor,
        required_features=required_features,
        optional_features=optional_features,
        target_id=target_id,
        signing_key=signing_key,
    )


def assemble_mpb(
    *,
    manifest_bytes: bytes,
    payloads: Sequence[Payload],
    key_id: int,
    flags: int,
    security_epoch: int = 1,
    core_abi_major: int = FRAME_ABI_MAJOR,
    core_abi_minor: int = FRAME_ABI_MINOR,
    required_features: int = 0,
    optional_features: int = 0,
    target_id: int = TARGET_ESP32S3,
    signing_key: ec.EllipticCurvePrivateKey | None = None,
) -> bytes:
    """Low-level assembly over pre-encoded manifest bytes (hostile-input path).

    Skips manifest semantic validation so tests can craft structurally invalid
    containers; callers are responsible for what they pass.
    """
    if not 1 <= len(payloads) <= MAX_PAYLOAD_COUNT:
        raise _invalid(f"payload count must be 1..{MAX_PAYLOAD_COUNT}")
    payload_table_offset = HEADER_SIZE + len(manifest_bytes)
    table_size = len(payloads) * PAYLOAD_ENTRY_SIZE
    cursor = payload_table_offset + table_size
    aligned: list[tuple[int, int, bytes, int]] = []
    for payload in payloads:
        cursor = (cursor + payload.alignment - 1) // payload.alignment * payload.alignment
        aligned.append((payload.kind, cursor, payload.data, payload.alignment))
        cursor += len(payload.data)
    signature_offset = (cursor + 3) & ~3
    total_size = signature_offset + SIGNATURE_RECORD_SIZE

    header = struct.pack(
        _HEADER_FMT,
        MAGIC,
        FORMAT_MAJOR,
        FORMAT_MINOR,
        HEADER_SIZE,
        total_size,
        flags,
        target_id,
        core_abi_major,
        core_abi_minor,
        required_features,
        optional_features,
        HEADER_SIZE,
        len(manifest_bytes),
        payload_table_offset,
        len(payloads),
        PAYLOAD_ENTRY_SIZE,
        signature_offset,
        SIGNATURE_RECORD_SIZE,
        key_id,
        security_epoch,
        0,
        0,
        0,
        0,
    )
    rows = [
        struct.pack(
            _PAYLOAD_ENTRY_FMT,
            kind,
            PAYLOAD_FLAG_REQUIRED,
            offset,
            len(data),
            len(data),
            alignment,
            hashlib.sha256(data).digest(),
            0,
            0,
        )
        for kind, offset, data, alignment in aligned
    ]
    payload_table = b"".join(rows)
    region = bytearray(signature_offset - payload_table_offset - table_size)
    for _kind, offset, data, _alignment in aligned:
        start = offset - payload_table_offset - table_size
        region[start : start + len(data)] = data
    signature_record = struct.pack(
        _SIGNATURE_FMT,
        SIG_ALGORITHM_ECDSA_P256_SHA256,
        SIG_RECORD_VERSION,
        key_id,
        b"\x00" * 64,
    )
    container = header + manifest_bytes + payload_table + bytes(region) + signature_record
    if signing_key is None:
        return container
    return sign_mpb(container, signing_key)


def sign_mpb(container: bytes, private_key: ec.EllipticCurvePrivateKey) -> bytes:
    """Sign an unsigned container in place, returning the signed container."""
    parsed = _parse_structure(container)
    algorithm, record_version, record_key_id, signature = struct.unpack(
        _SIGNATURE_FMT, parsed.signature_record
    )
    if algorithm != SIG_ALGORITHM_ECDSA_P256_SHA256 or record_version != SIG_RECORD_VERSION:
        raise _package_invalid("signature record fields are invalid")
    if signature != b"\x00" * 64:
        raise _invalid("input container is already signed")
    digest = hashlib.sha256(
        container[:HEADER_SIZE] + parsed.manifest_bytes + parsed.payload_table_bytes
    ).digest()
    signed = sign_digest_deterministic(private_key, digest)
    if not _verify_signature(private_key.public_key(), digest, signed):
        raise MpbError(FRAME_ERR_SIGNATURE_INVALID, "self-check of deterministic signature failed")
    record = struct.pack(_SIGNATURE_FMT, algorithm, record_version, record_key_id, signed)
    out = bytearray(container)
    out[parsed.signature_offset : parsed.signature_offset + SIGNATURE_RECORD_SIZE] = record
    result = bytes(out)
    verify_mpb(result, private_key.public_key())
    return result


@dataclass(frozen=True)
class _Parsed:
    header: dict[str, int]
    manifest_bytes: bytes
    payload_table_bytes: bytes
    payload_entries: tuple[tuple[int, int, int, int, int, bytes], ...]
    signature_record: bytes
    signature_offset: int


def _parse_structure(data: bytes) -> _Parsed:
    def bad(message: str) -> MpbError:
        return _package_invalid(message)

    if len(data) < HEADER_SIZE:
        raise bad(f"container is {len(data)} bytes, shorter than the fixed header")
    values = struct.unpack(_HEADER_FMT, data[:HEADER_SIZE])
    header: dict[str, int] = {}
    index = 0
    for name, fmt in HEADER_FIELDS:
        prefix = fmt[:-1]
        count = int(prefix) if prefix.isdigit() else 1
        if count == 1:
            header[name] = values[index]
        index += count
    if len(values) != index:
        raise bad("fixed header field table does not match the struct format")
    if header["magic"] != MAGIC:
        raise bad(f"magic {header['magic']:#010x} is not MPBF {MAGIC:#010x}")
    if header["format_major"] != FORMAT_MAJOR:
        raise bad(f"unsupported format major {header['format_major']}")
    if header["header_size"] != HEADER_SIZE:
        raise bad(f"header_size {header['header_size']} != {HEADER_SIZE}")
    if header["total_size"] != len(data):
        raise bad(f"total_size {header['total_size']} != container length {len(data)}")
    if header["flags"] & ~KNOWN_HEADER_FLAGS:
        raise bad(f"unknown required header flags {header['flags']:#010x}")
    if header["payload_count"] < 1 or header["payload_count"] > MAX_PAYLOAD_COUNT:
        raise bad(f"payload_count {header['payload_count']} outside 1..{MAX_PAYLOAD_COUNT}")
    if header["payload_entry_size"] != PAYLOAD_ENTRY_SIZE:
        raise bad(f"payload_entry_size {header['payload_entry_size']} != {PAYLOAD_ENTRY_SIZE}")
    if header["signature_length"] != SIGNATURE_RECORD_SIZE:
        raise bad(f"signature_length {header['signature_length']} != {SIGNATURE_RECORD_SIZE}")
    manifest_offset = header["manifest_offset"]
    manifest_length = header["manifest_length"]
    if manifest_offset != HEADER_SIZE:
        raise bad(f"manifest_offset {manifest_offset} != {HEADER_SIZE}")
    if manifest_length < TLV_HEADER_SIZE + 1:
        raise bad(f"manifest_length {manifest_length} is too short for its required fields")
    table_offset = header["payload_table_offset"]
    if table_offset != manifest_offset + manifest_length:
        raise bad("payload_table does not directly follow the manifest")
    table_size = header["payload_count"] * header["payload_entry_size"]
    signature_offset = header["signature_offset"]
    if signature_offset < table_offset + table_size:
        raise bad("signature record overlaps the payload table")
    if signature_offset + SIGNATURE_RECORD_SIZE != len(data):
        raise bad("signature record must end exactly at total_size")
    manifest_bytes = data[manifest_offset : manifest_offset + manifest_length]
    payload_table_bytes = data[table_offset : table_offset + table_size]
    signature_record = data[signature_offset : signature_offset + SIGNATURE_RECORD_SIZE]
    payload_window_end = signature_offset

    entries: list[tuple[int, int, int, int, int, bytes]] = []
    covered: list[tuple[int, int]] = []
    for index in range(header["payload_count"]):
        row = struct.unpack_from(
            _PAYLOAD_ENTRY_FMT, payload_table_bytes, index * PAYLOAD_ENTRY_SIZE
        )
        ptype, pflags, offset, length, unpacked, alignment = row[:6]
        digest = row[6]
        if ptype not in (PAYLOAD_TYPE_ELF, PAYLOAD_TYPE_RESOURCE):
            raise bad(f"payload {index} type {ptype} is unknown")
        if pflags & ~KNOWN_PAYLOAD_FLAGS:
            raise bad(f"payload {index} flags {pflags:#06x} contain unknown bits")
        if not pflags & PAYLOAD_FLAG_REQUIRED:
            raise bad(f"payload {index} is not marked required")
        if ptype == PAYLOAD_TYPE_ELF and pflags & PAYLOAD_FLAG_COMPRESSED:
            raise bad("elf payloads must not be compressed")
        if unpacked != length:
            raise bad(f"payload {index} unpacked_length != length")
        if not 1 <= alignment <= MAX_ALIGNMENT or alignment & (alignment - 1):
            raise bad(f"payload {index} alignment {alignment} is not a power of two")
        if offset % alignment:
            raise bad(f"payload {index} offset {offset} violates alignment {alignment}")
        if offset < table_offset + table_size or offset + length > payload_window_end:
            raise bad(f"payload {index} region [{offset}, {offset + length}) is out of bounds")
        for start, end in covered:
            if offset < end and start < offset + length:
                raise bad(f"payload {index} region overlaps another payload")
        covered.append((offset, offset + length))
        entries.append((ptype, offset, length, unpacked, alignment, digest))
    return _Parsed(
        header=header,
        manifest_bytes=manifest_bytes,
        payload_table_bytes=payload_table_bytes,
        payload_entries=tuple(entries),
        signature_record=signature_record,
        signature_offset=signature_offset,
    )


def _decode_manifest(data: bytes) -> dict[str, FieldValue]:
    def bad(message: str) -> MpbError:
        return _package_invalid(message)

    values: dict[str, FieldValue] = {}
    seen: set[int] = set()
    cursor = 0
    while cursor < len(data):
        if len(data) - cursor < TLV_HEADER_SIZE:
            raise bad("manifest TLV header is truncated")
        field_id, flags, length = struct.unpack_from(TLV_HEADER_FMT, data, cursor)
        cursor += TLV_HEADER_SIZE
        if flags & ~KNOWN_TLV_FLAGS:
            raise bad(f"TLV {field_id} flags {flags:#06x} contain unknown bits")
        if length > len(data) - cursor:
            raise bad(f"TLV {field_id} value overruns the manifest region")
        body = data[cursor : cursor + length]
        cursor += length
        desc = _FIELDS_BY_ID.get(field_id)
        if desc is None:
            if flags & TLV_FLAG_REQUIRED:
                raise bad(f"unknown required manifest TLV {field_id}")
            continue
        if field_id in seen:
            raise bad(f"manifest TLV {field_id} ({desc.name}) is duplicated")
        seen.add(field_id)
        values[desc.name] = _decode_value(desc, body)
    if "name" not in values or "version" not in values:
        raise bad("manifest is missing required name or version TLV")
    _validate_cross_fields(values, FRAME_ERR_PACKAGE_INVALID)
    return values


def _decode_value(desc: _FieldDesc, body: bytes) -> FieldValue:
    def bad(message: str) -> MpbError:
        return _package_invalid(f"manifest TLV {desc.name}: {message}")

    def recheck(value: FieldValue) -> FieldValue:
        try:
            _check_value(desc, value)
        except MpbError as error:
            raise bad(str(error)) from error
        return value

    if desc.kind in ("utf8_name", "semver"):
        try:
            value = body.decode("utf-8")
        except UnicodeDecodeError as error:
            raise bad(f"value is not utf-8: {error}") from error
        return recheck(value)
    if desc.kind == "u32":
        if len(body) != 4:
            raise bad(f"expected 4 bytes, got {len(body)}")
        return struct.unpack("<I", body)[0]
    if desc.kind == "affinity":
        if len(body) != 1:
            raise bad(f"expected 1 byte, got {len(body)}")
        return recheck(body[0])
    if desc.kind == "schema_id":
        if len(body) != 16:
            raise bad(f"expected 16 bytes, got {len(body)}")
        return body
    count = body[0] if body else -1
    if len(body) < 1 or count > MAX_REQUIRES:
        raise bad("requires encoding is invalid")
    cursor = 1
    entries: list[str] = []
    for _ in range(count):
        if len(body) - cursor < 2:
            raise bad("requires entry is truncated")
        entry_len = struct.unpack_from("<H", body, cursor)[0]
        cursor += 2
        if entry_len > len(body) - cursor:
            raise bad("requires entry overruns the value")
        try:
            entry = body[cursor : cursor + entry_len].decode("utf-8")
        except UnicodeDecodeError as error:
            raise bad(f"requires entry is not utf-8: {error}") from error
        cursor += entry_len
        entries.append(entry)
    if cursor != len(body):
        raise bad("requires value has trailing bytes")
    return recheck(tuple(entries))


def verify_mpb(
    data: bytes,
    public_key: ec.EllipticCurvePublicKey,
    *,
    epoch_floor: int = 0,
    expected_target: int = TARGET_ESP32S3,
    device_abi: tuple[int, int] | None = None,
    device_features: int | None = None,
) -> VerifyResult:
    """Full structural + signature + hash + semantic verification (oracle)."""
    parsed = _parse_structure(data)
    values = _decode_manifest(parsed.manifest_bytes)

    elf_count = sum(1 for e in parsed.payload_entries if e[0] == PAYLOAD_TYPE_ELF)
    resource_count = len(parsed.payload_entries) - elf_count
    resource_only = bool(parsed.header["flags"] & FLAG_RESOURCE_ONLY)
    if elf_count == 1 and resource_count == 0 and resource_only:
        raise _package_invalid("header claims resource-only but the single elf payload says code")
    if elf_count == 0 and resource_count >= 1 and not resource_only:
        raise _package_invalid("header flags do not declare the resource-only package type")
    if elf_count > 1 or (elf_count and resource_count):
        raise _package_invalid("payload composition violates strict single-type (PLUG-001)")

    algorithm, record_version, record_key_id, signature = struct.unpack(
        _SIGNATURE_FMT, parsed.signature_record
    )
    if algorithm != SIG_ALGORITHM_ECDSA_P256_SHA256:
        raise _package_invalid(f"signature algorithm {algorithm} is not ECDSA-P256-SHA256")
    if record_version != SIG_RECORD_VERSION:
        raise _package_invalid(f"signature record version {record_version} != {SIG_RECORD_VERSION}")
    if record_key_id != parsed.header["key_id"]:
        raise _package_invalid("signature record key_id does not match the fixed header")
    if derive_key_id(public_key) != parsed.header["key_id"]:
        raise MpbError(FRAME_ERR_SIGNATURE_INVALID, "public key key_id does not match the package")
    digest = hashlib.sha256(
        data[:HEADER_SIZE] + parsed.manifest_bytes + parsed.payload_table_bytes
    ).digest()
    if not _verify_signature(public_key, digest, signature):
        raise MpbError(
            FRAME_ERR_SIGNATURE_INVALID,
            "ECDSA P-256 signature over fixed_header+manifest+payload_table is invalid",
        )

    for index, (_ptype, offset, length, _unpacked, _alignment, expected) in enumerate(
        parsed.payload_entries
    ):
        actual = hashlib.sha256(data[offset : offset + length]).digest()
        if actual != expected:
            raise MpbError(FRAME_ERR_HASH_MISMATCH, f"payload {index} sha256 does not match")

    if parsed.header["security_epoch"] < epoch_floor:
        raise MpbError(
            FRAME_ERR_EPOCH_ROLLBACK,
            f"security_epoch {parsed.header['security_epoch']} < floor {epoch_floor}",
        )
    if parsed.header["target_id"] != expected_target:
        raise MpbError(
            FRAME_ERR_TARGET_MISMATCH,
            f"target_id {parsed.header['target_id']:#010x} != {expected_target:#010x}",
        )
    if device_abi is not None:
        major, minor = device_abi
        if parsed.header["core_abi_major"] != major or parsed.header["core_abi_minor"] > minor:
            raise MpbError(FRAME_ERR_ABI_MISMATCH, "core abi requirements exceed the device abi")
    if device_features is not None and parsed.header["required_features"] & ~device_features:
        raise MpbError(FRAME_ERR_ABI_MISMATCH, "required features exceed the device features")

    iram = values.get("iram_required_bytes", 0)
    if not isinstance(iram, int):
        iram = 0
    return VerifyResult(
        package_type="resource-only" if resource_only else "code",
        name=str(values["name"]),
        version=str(values["version"]),
        key_id=parsed.header["key_id"],
        security_epoch=parsed.header["security_epoch"],
        iram_required_bytes=iram,
        payload_count=len(parsed.payload_entries),
        payload_sha256=tuple(e[5] for e in parsed.payload_entries),
        fields=dict(values),
    )


def layout_tables() -> str:
    """Print the frozen container layout contract for the T3 C parser."""

    def table(title: str, fields: Sequence[tuple[str, str]], total: int) -> list[str]:
        lines = [title, f"  total size: {total} bytes (0x{total:X})", "  offset size type   field"]
        offset = 0
        for name, fmt in fields:
            size = struct.calcsize("<" + fmt)
            lines.append(f"  {offset:5d} {size:4d} {fmt:>6s}  {name}")
            offset += size
        return lines

    lines = table("fixed_header (little-endian, struct field order):", HEADER_FIELDS, HEADER_SIZE)
    lines += [""]
    lines += table("payload_table entry:", PAYLOAD_ENTRY_FIELDS, PAYLOAD_ENTRY_SIZE)
    lines += [""]
    lines += table("signature_record:", SIGNATURE_RECORD_FIELDS, SIGNATURE_RECORD_SIZE)
    lines += [
        "",
        "manifest TLV entry: field_id u16 | flags u16 | length u32 | value length bytes",
        f"  TLV_FLAG_REQUIRED = {TLV_FLAG_REQUIRED:#06x}; unknown field_id + required -> reject",
        "",
        "signing input: sha256(fixed_header_bytes || manifest_bytes || payload_table_bytes)",
        "signature: ECDSA P-256 over that digest, IEEE P1363 r||s, 64 bytes",
        f"magic {MAGIC:#010x} serialized little-endian is bytes 46 42 50 4d",
        f"target FRAME_TARGET_ESP32S3 = {TARGET_ESP32S3:#010x}",
        f"header flags: bit0 resource-only = {FLAG_RESOURCE_ONLY:#010x}",
        f"payload types: ELF={PAYLOAD_TYPE_ELF} RESOURCE={PAYLOAD_TYPE_RESOURCE}",
        f"payload flags: REQUIRED={PAYLOAD_FLAG_REQUIRED:#06x} "
        f"COMPRESSED={PAYLOAD_FLAG_COMPRESSED:#06x} (ELF must not set COMPRESSED)",
        "region order: header | manifest | payload_table | payloads | signature_record;",
        "manifest directly after header, table directly after manifest, payloads inside the",
        "window [table_end, signature_offset) with per-entry alignment, signature ends at",
        "total_size; any other byte coverage is FRAME_ERR_PACKAGE_INVALID.",
        "error codes: PACKAGE_INVALID=-18 SIGNATURE_INVALID=-17 HASH_MISMATCH=-19",
        "EPOCH_ROLLBACK=-34 TARGET_MISMATCH=-35 ABI_MISMATCH=-7",
        "manifest TLV field ids:",
    ]
    lines += [f"  {d.field_id:3d} {d.name} ({d.kind})" for d in _FIELD_DESCS]
    return "\n".join(lines)


def _load_manifest_json(path: Path) -> dict[str, object]:
    def no_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise _invalid(f"{path}: duplicate manifest field {key!r}")
            result[key] = value
        return result

    try:
        data = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=no_duplicates)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise _invalid(f"{path}: cannot read manifest json: {error}") from error
    if not isinstance(data, dict):
        raise _invalid(f"{path}: manifest json must be an object")
    return data


def _cmd_build(args: argparse.Namespace) -> int:
    if bool(args.elf) == bool(args.resource):
        raise _invalid("pass exactly one of --elf (code mpb) or --resource (resource-only mpb)")
    payloads: list[Payload] = []
    if args.elf:
        payloads.append(
            Payload(
                PAYLOAD_TYPE_ELF,
                Path(args.elf).read_bytes(),
                alignment=args.elf_alignment,
            )
        )
    else:
        for name in args.resource:
            payloads.append(
                Payload(
                    PAYLOAD_TYPE_RESOURCE,
                    Path(name).read_bytes(),
                    alignment=args.resource_alignment,
                )
            )
    data: dict[str, object] = {"name": args.name, "version": args.version}
    if args.manifest_json:
        data.update(_load_manifest_json(Path(args.manifest_json)))
    manifest = Manifest.from_mapping(data)
    signing_key = load_private_key(Path(args.key)) if args.key else None
    container = build_mpb(
        manifest,
        payloads,
        key_id=args.key_id,
        signing_key=signing_key,
        security_epoch=args.epoch,
    )
    _write_container(Path(args.output), container)
    signed = "signed" if signing_key else "unsigned"
    print(
        f"{args.output}: {signed} {len(container)} byte mpb, "
        f"key_id={_hex_key_id(container)}, sha256={hashlib.sha256(container).hexdigest()}"
    )
    return 0


def _hex_key_id(container: bytes) -> str:
    key_id = struct.unpack_from("<I", container, 68)[0]
    return f"{key_id:08x}"


def _write_container(path: Path, container: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(container)


def _cmd_sign(args: argparse.Namespace) -> int:
    container = Path(args.input).read_bytes()
    signed = sign_mpb(container, load_private_key(Path(args.key)))
    _write_container(Path(args.output), signed)
    print(
        f"{args.output}: signed {len(signed)} byte mpb, "
        f"key_id={_hex_key_id(signed)}, sha256={hashlib.sha256(signed).hexdigest()}"
    )
    return 0


def _cmd_verify(args: argparse.Namespace) -> int:
    container = Path(args.input).read_bytes()
    result = verify_mpb(
        container,
        load_public_key(Path(args.pubkey)),
        epoch_floor=args.epoch_floor,
    )
    print(f"{args.input}: OK ({result.package_type})")
    print(
        f"  name={result.name} version={result.version} epoch={result.security_epoch} "
        f"key_id={result.key_id:08x}"
    )
    print(f"  payloads={result.payload_count} iram_required_bytes={result.iram_required_bytes}")
    for index, digest in enumerate(result.payload_sha256):
        print(f"  payload[{index}] sha256={digest.hex()}")
    return 0


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Deterministic little-endian MPB container builder and reference verifier"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build", help="build an mpb container")
    build.add_argument("--elf", help="code mpb elf payload path")
    build.add_argument("--resource", action="append", default=[], help="resource payload path")
    build.add_argument("--name", required=True, help="plugin name (1..64 utf-8 bytes)")
    build.add_argument("--version", required=True, help="strict semver major.minor.patch")
    build.add_argument("--manifest-json", help="json object with additional manifest fields")
    build.add_argument("--key", help="secp256r1 private key pem; signs the container")
    build.add_argument("--key-id", type=int, help="explicit key_id for unsigned builds")
    build.add_argument("--epoch", type=int, default=1, help="security epoch (default 1)")
    build.add_argument("--elf-alignment", type=int, default=DEFAULT_ELF_ALIGNMENT)
    build.add_argument("--resource-alignment", type=int, default=DEFAULT_RESOURCE_ALIGNMENT)
    build.add_argument("--output", required=True)
    build.set_defaults(handler=_cmd_build)

    sign = sub.add_parser("sign", help="sign an unsigned mpb container")
    sign.add_argument("--input", required=True)
    sign.add_argument("--key", required=True)
    sign.add_argument("--output", required=True)
    sign.set_defaults(handler=_cmd_sign)

    verify = sub.add_parser("verify", help="verify structure, signature and payload hashes")
    verify.add_argument("--input", required=True)
    verify.add_argument("--pubkey", required=True)
    verify.add_argument("--epoch-floor", type=int, default=0)
    verify.set_defaults(handler=_cmd_verify)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.handler(args))
    except (MpbError, OSError, InvalidSignature, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
