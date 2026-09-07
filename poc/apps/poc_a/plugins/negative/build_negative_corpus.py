#!/usr/bin/env python3
"""Handcrafted negative corpus for the T5 relocation/import allowlist matrix.

The positive pipeline gates (check_plugin_elf.cmake) reject every image this
script produces, by design: the gates protect positive builds, negatives are
handcrafted. What the script fabricates, starting from the genuine gated
`baseline.elf` (each fixture is a deterministic byte patch, re-signed with
the TEST-only key, so the device-side mpb verification passes and the
rejection must come from the loader/policy layers):

  neg_r32     first .rela.dyn entry's relocation type patched to
              R_XTENSA_32 (1)  -> loader must fail with -EINVAL (patch p1)
  neg_s0op    same entry patched to R_XTENSA_SLOT0_OP (20)
              -> loader must fail with -EINVAL (patch p1)
  neg_phbe    e_ident[EI_DATA] = big-endian  -> loader must fail with
              -EINVAL (patch p2)
  neg_ph64    e_ident[EI_CLASS] = ELF64      -> loader must fail with
              -EINVAL (patch p2)
  neg_phmach  e_machine = 40 (EM_ARM)        -> loader must fail with
              -EINVAL (patch p2)
  neg_phspan  first PT_LOAD p_memsz inflated to 15 MiB -> harness
              program-header admission check must reject the load before
              esp_elf_init (segment budget > manifest max_memory_bytes)

Toolchain note (recorded in ALLOWLIST.md): xtensa ld canonicalizes or
rejects fabricated out-of-allowlist dynamic relocations at link time
(R_XTENSA_32 against a dynamic symbol becomes R_XTENSA_GLOB_DAT;
R_XTENSA_SLOT0_OP is a "dangerous relocation" hard error), so the reloc
negatives are post-link patches of a real .rela.dyn entry rather than
compiler-emitted ones.

The EI_DATA/EI_CLASS/e_machine fixtures cannot go through `build_mpb`
because the manifest builder itself validates class/endian/machine in
extract_plugin_iram_size(); those three are assembled with assemble_mpb()
plus the deterministic signer (the "hostile_signed" pattern proven in
tests/test_manifest_builder).
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from frame_tools.manifest_builder import (  # noqa: E402
    PAYLOAD_TYPE_ELF,
    Manifest,
    Payload,
    assemble_mpb,
    build_mpb,
    derive_key_id,
    load_private_key,
)

SHT_RELA = 4
PT_LOAD = 1
EM_ARM = 40
R_XTENSA_32 = 1
R_XTENSA_SLOT0_OP = 20
EI_CLASS, EI_DATA = 4, 5
MAX_MEMORY_BYTES = 262144
PHSPAN_MEMSZ = 0x00F00000


def ehdr_fields(elf: bytes) -> dict[str, int]:
    (phoff, shoff) = struct.unpack_from("<II", elf, 28)
    (phentsize, phnum, shentsize, shnum) = struct.unpack_from("<HHHH", elf, 42)
    return {
        "phoff": phoff,
        "shoff": shoff,
        "phentsize": phentsize,
        "phnum": phnum,
        "shentsize": shentsize,
        "shnum": shnum,
    }


def first_rela_entry(elf: bytes) -> int:
    """File offset of the first Elf32_Rela entry of the first SHT_RELA section."""
    fields = ehdr_fields(elf)
    for index in range(fields["shnum"]):
        shdr = struct.unpack_from("<10I", elf, fields["shoff"] + index * fields["shentsize"])
        if shdr[1] == SHT_RELA and shdr[5] >= 12:
            return shdr[4]
    raise SystemExit("baseline.elf carries no SHT_RELA section; cannot patch")


def patch_reloc_type(elf: bytes, new_type: int) -> bytes:
    entry = first_rela_entry(elf)
    r_offset, r_info = struct.unpack_from("<II", elf, entry)
    buf = bytearray(elf)
    buf[entry + 4] = new_type  # low byte of r_info on little-endian ELF32
    patched_info = (r_info & 0xFFFFFF00) | new_type
    print(f"  rela@0x{entry:04x}: r_offset=0x{r_offset:08x} r_info=0x{r_info:08x}"
          f" -> 0x{patched_info:08x} (type {r_info & 0xFF} -> {new_type})")
    return bytes(buf)


def first_pt_load_memsz_field(elf: bytes) -> int:
    fields = ehdr_fields(elf)
    for index in range(fields["phnum"]):
        phdr = struct.unpack_from("<8I", elf, fields["phoff"] + index * fields["phentsize"])
        if phdr[0] == PT_LOAD:
            return fields["phoff"] + index * fields["phentsize"] + 5 * 4
    raise SystemExit("baseline.elf carries no PT_LOAD segment")


def patch_phspan(elf: bytes) -> bytes:
    field = first_pt_load_memsz_field(elf)
    old = struct.unpack_from("<I", elf, field)[0]
    buf = bytearray(elf)
    struct.pack_into("<I", buf, field, PHSPAN_MEMSZ)
    print(f"  phdr p_memsz@0x{field:04x}: {old} -> {PHSPAN_MEMSZ}")
    return bytes(buf)


def patch_header(elf: bytes, offset: int, value: int, what: str) -> bytes:
    buf = bytearray(elf)
    old = buf[offset]
    buf[offset] = value
    print(f"  {what}@0x{offset:02x}: {old} -> {value}")
    return bytes(buf)


def sign_with_assemble(name: str, elf: bytes, key) -> bytes:
    manifest = Manifest(name, "1.0.0")
    manifest.set("max_memory_bytes", MAX_MEMORY_BYTES)
    manifest.set("iram_required_bytes", 0)
    return assemble_mpb(
        manifest_bytes=manifest.encode(),
        payloads=[Payload(kind=PAYLOAD_TYPE_ELF, data=elf)],
        key_id=derive_key_id(key.public_key()),
        flags=0,
        security_epoch=1,
        signing_key=key,
    )


def write_meta(out_dir: Path, name: str, container: bytes) -> None:
    digest = hashlib.sha256(container).hexdigest()
    upper = name.upper()
    header = (
        f"/* Generated by poc/apps/poc_a/plugins/negative/build_negative_corpus.py - do not edit. */\n"
        f"#define POCA_{upper}_MPB_SIZE {len(container)}u\n"
        f"#define POCA_{upper}_MPB_SHA256 \"{digest}\"\n"
        f"#define POCA_{upper}_NAME \"{name}\"\n"
        f"#define POCA_{upper}_VERSION \"1.0.0\"\n"
        f"#define POCA_{upper}_ENTRY \"frame_plugin_entry\"\n"
    )
    (out_dir / "generated" / f"poca_{name}_meta.h").write_text(header)
    print(f"[corpus] {name}.mpb size={len(container)} sha256={digest}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-elf", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    (out_dir / "generated").mkdir(parents=True, exist_ok=True)
    key = load_private_key(Path(args.key))
    baseline = Path(args.baseline_elf).read_bytes()

    buildable = {
        "neg_r32": lambda: patch_reloc_type(baseline, R_XTENSA_32),
        "neg_s0op": lambda: patch_reloc_type(baseline, R_XTENSA_SLOT0_OP),
        "neg_phspan": lambda: patch_phspan(baseline),
    }
    for name, make in buildable.items():
        print(f"[corpus] building {name}")
        elf = make()
        manifest = Manifest(name, "1.0.0")
        manifest.set("max_memory_bytes", MAX_MEMORY_BYTES)
        container = build_mpb(
            manifest,
            [Payload(kind=PAYLOAD_TYPE_ELF, data=elf)],
            signing_key=key,
            security_epoch=1,
        )
        (out_dir / f"{name}.mpb").write_bytes(container)
        write_meta(out_dir, name, container)

    header_only = {
        "neg_phbe": lambda: patch_header(baseline, EI_DATA, 2, "EI_DATA"),
        "neg_ph64": lambda: patch_header(baseline, EI_CLASS, 2, "EI_CLASS"),
        "neg_phmach": lambda: patch_header(
            baseline, 18, EM_ARM, "e_machine(low byte)"
        ),
    }
    for name, make in header_only.items():
        print(f"[corpus] building {name} (assemble_mpb hostile-signed path)")
        elf = make()
        container = sign_with_assemble(name, elf, key)
        (out_dir / f"{name}.mpb").write_bytes(container)
        write_meta(out_dir, name, container)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
