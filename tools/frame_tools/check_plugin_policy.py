"""Static plugin-ELF policy gate for PoC-A (GH-003, appendix C 11.3 subset).

Host-side re-implementation of the build-time gates in
``poc/apps/poc_a/plugins/check_plugin_elf.cmake`` and loader patch p4, so the
policy stays enforced (and testable) without the xtensa toolchain:

* shape  - ELF32, little-endian, Xtensa, ET_DYN (gate 3 subset);
* reloc  - every dynamic relocation type must be one of the five types the
  vendored elf_loader implements on xtensa (gate 1 / patch p1; this also
  rejects every ``R_XTENSA_TLS_*`` type, so the ``neg_tls_nosect`` shape - TLS
  relocations whose sections were renamed away - is still caught);
* section - no C++ runtime feature section (gate 4 / patch p4; the stem list
  is pinned to a single source by ``tests/test_check_plugin_policy.py``,
  which asserts equality with both the cmake gate and PATCHES.md);
* import - closed world: every undefined global must be declared (gate 2);
* export - exactly one exported query entry symbol, a GLOBAL FUNC;
* allocator - no direct malloc/free/operator-new/delete references.

Usage:

    uv run python -m frame_tools.check_plugin_policy \
        --allow-import poca_host_add --allow-import poca_host_sentinel \
        plugin.elf [more.elf ...]
    uv run python -m frame_tools.check_plugin_policy \
        --allow-imports-file ALLOWLIST.txt plugin.elf

Exit status is 0 when every ELF satisfies the policy, 1 otherwise.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

ELFCLASS32 = 1
ELFDATA2LSB = 1
ET_DYN = 3
EM_XTENSA = 94
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_REL = 9
SHN_UNDEF = 0
SHN_LORESERVE = 0xFF00
STB_GLOBAL = 1
STT_FUNC = 2

DEFAULT_ENTRY = "frame_plugin_entry"

ALLOWED_RELOC_TYPES: dict[int, str] = {
    0: "R_XTENSA_NONE",
    2: "R_XTENSA_RTLD",
    3: "R_XTENSA_GLOB_DAT",
    4: "R_XTENSA_JMP_SLOT",
    5: "R_XTENSA_RELATIVE",
}

RELOC_TYPE_NAMES: dict[int, str] = {
    **ALLOWED_RELOC_TYPES,
    1: "R_XTENSA_32",
    6: "R_XTENSA_PLT",
    8: "R_XTENSA_OP0",
    9: "R_XTENSA_OP1",
    10: "R_XTENSA_OP2",
    11: "R_XTENSA_ASM_EXPAND",
    12: "R_XTENSA_ASM_SIMPLIFY",
    15: "R_XTENSA_GNU_VTINHERIT",
    16: "R_XTENSA_GNU_VTENTRY",
    50: "R_XTENSA_TLSDESC_ARG",
    51: "R_XTENSA_TLSDESC_FN",
    **{20 + slot: f"R_XTENSA_SLOT{slot}_OP" for slot in range(15)},
}

FORBIDDEN_SECTION_STEMS = (
    "init_array",
    "fini_array",
    "preinit_array",
    "ctors",
    "dtors",
    "tdata",
    "tbss",
    "eh_frame",
    "eh_frame_hdr",
    "gcc_except_table",
)

ALLOCATOR_IMPORTS = frozenset(
    {"malloc", "free", "calloc", "realloc", "posix_memalign", "memalign", "aligned_alloc"}
)
OPERATOR_ALLOC_PREFIXES = ("_Zna", "_Zda", "_Zdl")

_REL_WRAPPER = re.compile(r"^\.(rela?)?\.")


def section_stem(name: str) -> str:
    """Normalize a section name to its stem, mirroring cmake gate 4.

    Strips an optional ``.rel``/``.rela`` wrapper and the leading dot, then
    keeps everything up to the next dot, so ``.rela.init_array.00001``
    normalizes to ``init_array`` exactly like the build-time gate.
    """
    stripped = _REL_WRAPPER.sub("", name, count=1).lstrip(".")
    return stripped.split(".", 1)[0] if stripped else name


@dataclass(frozen=True)
class Section:
    name: str
    sh_type: int
    offset: int
    size: int
    link: int
    entsize: int


@dataclass(frozen=True)
class Symbol:
    name: str
    bind: int
    sym_type: int
    shndx: int


class PolicyError(ValueError):
    """The file is not a parsable plugin ELF; the policy fails closed."""


def _string_at(table: bytes, offset: int) -> str:
    end = table.find(b"\x00", offset)
    if end < 0:
        end = len(table)
    return table[offset:end].decode("utf-8", errors="replace")


def parse_elf32(data: bytes) -> tuple[list[Section], list[Symbol]]:
    """Parse the section and symbol tables of an ELF32 LE Xtensa ET_DYN image."""
    if len(data) < 52:
        raise PolicyError("truncated ELF header")
    (
        ident,
        e_type,
        e_machine,
        _e_version,
        _e_entry,
        _e_phoff,
        e_shoff,
        _e_flags,
        _e_ehsize,
        _e_phentsize,
        _e_phnum,
        e_shentsize,
        e_shnum,
        e_shstrndx,
    ) = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
    if ident[:4] != b"\x7fELF":
        raise PolicyError("bad ELF magic")
    if ident[4] != ELFCLASS32:
        raise PolicyError("expected ELF32 image")
    if ident[5] != ELFDATA2LSB:
        raise PolicyError("expected little-endian image")
    if e_type != ET_DYN:
        raise PolicyError(f"expected ET_DYN shared image, got e_type {e_type}")
    if e_machine != EM_XTENSA:
        raise PolicyError(f"expected Xtensa machine, got e_machine {e_machine}")
    if e_shoff == 0 or e_shnum == 0 or e_shentsize != 40:
        raise PolicyError("missing or malformed section header table")
    if e_shstrndx >= e_shnum:
        raise PolicyError("section name string table index out of range")
    raw_shdrs: list[tuple[int, ...]] = []
    for index in range(e_shnum):
        start = e_shoff + index * e_shentsize
        if start + 40 > len(data):
            raise PolicyError("truncated section header table")
        raw_shdrs.append(struct.unpack_from("<10I", data, start))
    shstr = raw_shdrs[e_shstrndx]
    if shstr[4] + shstr[5] > len(data):
        raise PolicyError("truncated section name string table")
    shstrtab = data[shstr[4] : shstr[4] + shstr[5]]
    sections = [
        Section(_string_at(shstrtab, sh[0]), sh[1], sh[4], sh[5], sh[6], sh[9]) for sh in raw_shdrs
    ]
    symbols: list[Symbol] = []
    for section in sections:
        if section.sh_type != SHT_SYMTAB:
            continue
        if section.link >= len(sections):
            raise PolicyError("symbol table link out of range")
        strtab_section = sections[section.link]
        if strtab_section.offset + strtab_section.size > len(data):
            raise PolicyError("truncated symbol string table")
        strtab = data[strtab_section.offset : strtab_section.offset + strtab_section.size]
        entsize = section.entsize or 16
        if entsize != 16 or section.offset + section.size > len(data):
            raise PolicyError("malformed symbol table")
        for offset in range(0, section.size - entsize + 1, entsize):
            name_off, _, _, info, _, shndx = struct.unpack_from(
                "<IIIBBH", data, section.offset + offset
            )
            if offset == 0:
                continue
            symbols.append(Symbol(_string_at(strtab, name_off), info >> 4, info & 0xF, shndx))
    return sections, symbols


def _reloc_type_names(data: bytes, sections: Sequence[Section]) -> dict[str, int]:
    """Count dynamic relocation types from every SHT_REL/SHT_RELA section."""
    counts: dict[str, int] = {}
    for section in sections:
        if section.sh_type not in (SHT_REL, SHT_RELA):
            continue
        entsize = section.entsize or (8 if section.sh_type == SHT_REL else 12)
        if entsize not in (8, 12) or section.offset + section.size > len(data):
            raise PolicyError(f"malformed relocation section {section.name!r}")
        for offset in range(0, section.size - entsize + 1, entsize):
            info = struct.unpack_from("<I", data, section.offset + offset + 4)[0]
            name = RELOC_TYPE_NAMES.get(info >> 8, f"R_XTENSA_{info >> 8}")
            counts[name] = counts.get(name, 0) + 1
    return counts


def _evaluate(data: bytes, entry: str, allowed_imports: frozenset[str]) -> tuple[list[str], str]:
    violations: list[str] = []
    try:
        sections, symbols = parse_elf32(data)
        reloc_counts = _reloc_type_names(data, sections)
    except PolicyError as error:
        return [f"shape: {error}"], "unparsable"
    for name, count in sorted(reloc_counts.items()):
        if name not in ALLOWED_RELOC_TYPES.values():
            violations.append(f"reloc: forbidden relocation type {name} x{count}")
    for section in sections:
        if section_stem(section.name) in FORBIDDEN_SECTION_STEMS:
            violations.append(f"section: forbidden C++ feature section {section.name!r}")
    reloc_summary = (
        "relocs("
        + " ".join(f"{name}={count}" for name, count in sorted(reloc_counts.items()))
        + ")"
    )
    if not any(s.sh_type == SHT_SYMTAB for s in sections):
        violations.append("shape: no symbol table (entry lookup impossible)")
    else:
        defined = [
            s for s in symbols if s.bind == STB_GLOBAL and SHN_UNDEF < s.shndx < SHN_LORESERVE
        ]
        exports = sorted(s.name for s in defined)
        if exports != [entry]:
            violations.append(f"export: exported globals must be exactly [{entry}], got {exports}")
        elif defined[0].sym_type != STT_FUNC:
            violations.append(f"export: entry {entry} must be a FUNC symbol")
        undefined = [s.name for s in symbols if s.bind == STB_GLOBAL and s.shndx == SHN_UNDEF]
        for name in undefined:
            if name.startswith(OPERATOR_ALLOC_PREFIXES) or name in ALLOCATOR_IMPORTS:
                violations.append(f"allocator: direct allocation import {name!r}")
            elif name not in allowed_imports:
                violations.append(f"import: undeclared undefined symbol {name!r}")
        reloc_summary += f" exports={exports} imports={len(undefined)}"
    return violations, reloc_summary


def check_policy(
    data: bytes, entry: str = DEFAULT_ENTRY, allowed_imports: frozenset[str] = frozenset()
) -> list[str]:
    """Return the policy violations for one plugin ELF (empty list = pass)."""
    return _evaluate(data, entry, allowed_imports)[0]


def _load_imports_file(path: str) -> list[str]:
    names: list[str] = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        stripped = line.split("#", 1)[0].strip()
        if stripped:
            names.append(stripped)
    return names


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="check-plugin-policy",
        description="Enforce the appendix C 11.3 plugin policy subset on ELF32 "
        "Xtensa plugin images (see module docstring).",
    )
    parser.add_argument("--entry", default=DEFAULT_ENTRY, help="the single exported query entry")
    parser.add_argument(
        "--allow-import", action="append", default=[], help="declared import symbol (repeatable)"
    )
    parser.add_argument(
        "--allow-imports-file",
        help="file with one declared import symbol per line (# comments allowed)",
    )
    parser.add_argument("elfs", nargs="+", help="plugin ELF files to check")
    args = parser.parse_args(argv)

    declared = list(args.allow_import)
    if args.allow_imports_file:
        declared += _load_imports_file(args.allow_imports_file)
    allowed = frozenset(declared)

    failed = False
    for elf in args.elfs:
        violations = check_policy(Path(elf).read_bytes(), args.entry, allowed)
        if violations:
            failed = True
            for violation in violations:
                print(f"check-plugin-policy: {elf}: {violation}")
        else:
            print(f"check-plugin-policy: {elf}: policy OK (entry={args.entry})")
    if failed:
        print("check-plugin-policy: FAILED", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
