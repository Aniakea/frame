"""Tests for the host plugin-policy gate (GH-003, appendix C 11.3 subset).

The policy checker is exercised on synthetic ELF32 LE Xtensa images built by
this module, so the suite runs host-only (no xtensa toolchain needed). The
stem drift guard additionally pins the forbidden-section list to a single
source: the list in ``check_plugin_policy.py`` must equal both gate 4 of
``check_plugin_elf.cmake`` and the p4 hunk recorded in ``PATCHES.md``.
"""

from __future__ import annotations

import re
import struct
from collections.abc import Sequence
from pathlib import Path

import pytest
from frame_tools.check_plugin_policy import (
    DEFAULT_ENTRY,
    FORBIDDEN_SECTION_STEMS,
    check_policy,
    section_stem,
)
from frame_tools.check_plugin_policy import (
    main as cli_main,
)

REPO = Path(__file__).resolve().parents[1]
CMAKE_GATE = REPO / "poc/apps/poc_a/plugins/check_plugin_elf.cmake"
PATCHES_MD = REPO / "poc/apps/poc_a/components/elf_loader/PATCHES.md"

STT_FUNC = 2
STT_OBJECT = 1
STB_GLOBAL = 1
SHT_PROGBITS = 1
EM_XTENSA = 94


def build_elf(
    *,
    extra_sections: Sequence[str] = (),
    undefined: Sequence[str] = (),
    defined_globals: Sequence[tuple[str, int]] | None = None,
    rela: Sequence[tuple[int, int]] = ((5, 0),),
    rel: Sequence[tuple[int, int]] = (),
    ei_class: int = 1,
    ei_data: int = 1,
    machine: int = EM_XTENSA,
    e_type: int = 3,
    bad_rela_entsize: bool = False,
) -> bytes:
    """Synthesize a minimal plugin-shaped ELF32 LE image.

    ``rela``/``rel`` entries are ``(reloc_type, symbol_index)`` pairs packed
    into ``.rela.dyn``/``.rel.dyn``; ``defined_globals`` defaults to exactly
    the query entry, mirroring the stripped positive-pipeline artifacts.
    """
    if defined_globals is None:
        defined_globals = [(DEFAULT_ENTRY, STT_FUNC)]
    section_order = [
        ".text",
        *extra_sections,
        ".rela.dyn",
        ".rel.dyn",
        ".shstrtab",
        ".strtab",
        ".symtab",
    ]
    sh_names: dict[str, int] = {}
    shstrtab = b"\x00"
    for name in section_order:
        sh_names[name] = len(shstrtab)
        shstrtab += name.encode() + b"\x00"
    header_index = {name: index + 1 for index, name in enumerate(section_order)}
    sym_names: dict[str, int] = {}
    strtab = b"\x00"
    for name in [n for n, _ in defined_globals] + list(undefined):
        if name not in sym_names:
            sym_names[name] = len(strtab)
            strtab += name.encode() + b"\x00"

    blobs: list[tuple[str, int, bytes]] = []
    cursor = 52
    for name, blob in (
        (".text", b"\x00" * 16),
        (".shstrtab", shstrtab),
        (".strtab", strtab),
    ):
        blobs.append((name, cursor, blob))
        cursor += len(blob)
    symbols = [(0, 0, 0, 0, 0, 0)]
    for name, sym_type in defined_globals:
        symbols.append((sym_names[name], 0x100, 0, (STB_GLOBAL << 4) | sym_type, 0, 1))
    for name in undefined:
        symbols.append((sym_names[name], 0, 0, STB_GLOBAL << 4, 0, 0))
    symtab = b"".join(struct.pack("<IIIBBH", *sym) for sym in symbols)
    blobs.append((".symtab", cursor, symtab))
    cursor += len(symtab)
    rela_bytes = b"".join(
        struct.pack("<IIi", 0x198, (reloc_type << 8) | sym_index, 0)
        for reloc_type, sym_index in rela
    )
    blobs.append((".rela.dyn", cursor, rela_bytes))
    cursor += len(rela_bytes)
    rel_bytes = b"".join(
        struct.pack("<II", 0x1A0, (reloc_type << 8) | sym_index) for reloc_type, sym_index in rel
    )
    blobs.append((".rel.dyn", cursor, rel_bytes))
    cursor += len(rel_bytes)
    placed = {name: (offset, len(blob)) for name, offset, blob in blobs}

    def shdr(name: str, sh_type: int, link: int = 0, entsize: int = 0) -> bytes:
        offset, size = placed[name]
        return struct.pack(
            "<10I", sh_names[name], sh_type, 0, 0x1000, offset, size, link, 0, 4, entsize
        )

    shdrs = b"\x00" * 40
    shdrs += shdr(".text", SHT_PROGBITS)
    for name in extra_sections:
        placed[name] = (placed[".text"][0], 0)
        shdrs += shdr(name, SHT_PROGBITS)
    shdrs += shdr(".rela.dyn", 4, entsize=99 if bad_rela_entsize else 12)
    shdrs += shdr(".rel.dyn", 9, entsize=8)
    shdrs += shdr(".shstrtab", 3)
    shdrs += shdr(".strtab", 3)
    shdrs += shdr(".symtab", 2, link=header_index[".strtab"], entsize=16)

    shnum = len(section_order) + 1
    ident = b"\x7fELF" + bytes([ei_class, ei_data, 1, 0]) + b"\x00" * 8
    ehdr = ident + struct.pack(
        "<HHIIIIIHHHHHH",
        e_type,
        machine,
        1,
        0x100,
        0,
        cursor,
        0,
        52,
        0,
        0,
        40,
        shnum,
        header_index[".shstrtab"],
    )
    return ehdr + b"".join(blob for _, _, blob in blobs) + shdrs


def gate4_stems() -> list[str]:
    match = re.search(
        r"set\(forbidden_stems\s+(.*?)\)", CMAKE_GATE.read_text(encoding="utf-8"), re.DOTALL
    )
    assert match, "gate 4 stem list not found in check_plugin_elf.cmake"
    return match.group(1).split()


def p4_stems() -> list[str]:
    text = PATCHES_MD.read_text(encoding="utf-8")
    match = re.search(r"static const char \*const forbidden\[\] = \{(.*?)\};", text, re.DOTALL)
    assert match, "p4 forbidden[] list not found in PATCHES.md"
    return [name.lstrip(".") for name in re.findall(r'"([^"]+)"', match.group(1))]


def test_forbidden_section_stems_have_a_single_source() -> None:
    """Drift guard: checker list == cmake gate 4 == loader patch p4."""
    assert gate4_stems() == list(FORBIDDEN_SECTION_STEMS)
    assert p4_stems() == list(FORBIDDEN_SECTION_STEMS)


@pytest.mark.parametrize(
    ("name", "stem"),
    [
        (".text", "text"),
        (".ctors", "ctors"),
        (".init_array.00001", "init_array"),
        (".rela.init_array.00001", "init_array"),
        (".rel.text", "text"),
        (".gcc_except_table", "gcc_except_table"),
        (".data.rel.ro", "data"),
    ],
)
def test_section_stem_mirrors_gate4_normalization(name: str, stem: str) -> None:
    assert section_stem(name) == stem


def test_minimal_positive_plugin_passes() -> None:
    elf = build_elf(undefined=("poca_host_add",), rela=((5, 0), (3, 1), (4, 1), (2, 0), (0, 0)))
    assert check_policy(elf, allowed_imports=frozenset({"poca_host_add"})) == []


def test_undeclared_import_is_rejected() -> None:
    elf = build_elf(undefined=("sprintf",))
    violations = check_policy(elf, allowed_imports=frozenset({"poca_host_add"}))
    assert any("import: undeclared undefined symbol 'sprintf'" in v for v in violations)


@pytest.mark.parametrize(
    "allocator", ["malloc", "free", "calloc", "realloc", "_Znam", "_ZdlPv", "_ZdaPv"]
)
def test_allocator_imports_are_rejected_even_when_declared(allocator: str) -> None:
    elf = build_elf(undefined=(allocator,))
    violations = check_policy(elf, allowed_imports=frozenset({allocator}))
    assert any("allocator: direct allocation import" in v for v in violations)


def test_extra_export_is_rejected() -> None:
    elf = build_elf(defined_globals=[(DEFAULT_ENTRY, STT_FUNC), ("sneaky_export", STT_FUNC)])
    violations = check_policy(elf)
    assert any(v.startswith("export:") and "sneaky_export" in v for v in violations)


def test_missing_entry_is_rejected() -> None:
    elf = build_elf(defined_globals=[("not_the_entry", STT_FUNC)])
    violations = check_policy(elf)
    assert any(v.startswith("export:") for v in violations)


def test_entry_must_be_a_function_symbol() -> None:
    elf = build_elf(defined_globals=[(DEFAULT_ENTRY, STT_OBJECT)])
    violations = check_policy(elf)
    assert any("must be a FUNC symbol" in v for v in violations)


@pytest.mark.parametrize(
    ("reloc", "needle"), [(1, "R_XTENSA_32"), (20, "R_XTENSA_SLOT0_OP"), (11, "ASM_EXPAND")]
)
def test_out_of_allowlist_relocations_are_named_and_rejected(reloc: int, needle: str) -> None:
    elf = build_elf(rela=((5, 0), (reloc, 0)))
    violations = check_policy(elf)
    assert any("reloc: forbidden relocation type" in v and needle in v for v in violations)


def test_tls_relocations_without_any_tls_section_are_rejected() -> None:
    """neg_tls_nosect pattern: sections renamed away, TLS reloc types remain."""
    elf = build_elf(rela=((5, 0), (51, 0), (50, 0)))
    violations = check_policy(elf)
    assert any("R_XTENSA_TLSDESC_FN" in v for v in violations)
    assert any("R_XTENSA_TLSDESC_ARG" in v for v in violations)


@pytest.mark.parametrize(
    "section", [".ctors", ".init_array.00001", ".rela.tdata", ".eh_frame", ".dtors"]
)
def test_forbidden_cpp_feature_sections_are_rejected(section: str) -> None:
    elf = build_elf(extra_sections=(section,))
    violations = check_policy(elf)
    assert any(f"forbidden C++ feature section {section!r}" in v for v in violations)


def test_sht_rel_sections_are_parsed_like_rela() -> None:
    elf = build_elf(rela=(), rel=((5, 0), (1, 0)))
    violations = check_policy(elf)
    assert any("R_XTENSA_32" in v for v in violations)


@pytest.mark.parametrize(
    "kwargs",
    [
        {"ei_class": 2},
        {"ei_data": 2},
        {"machine": 40},
        {"e_type": 2},
        {"bad_rela_entsize": True},
    ],
)
def test_shape_failures_fail_closed(kwargs: dict[str, int | bool]) -> None:
    elf = build_elf(**kwargs)  # type: ignore[arg-type]
    assert check_policy(elf)


def test_cli_accepts_a_policy_clean_elf(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    elf_path = tmp_path / "plugin.elf"
    elf_path.write_bytes(build_elf(undefined=("poca_host_add",)))
    allow = tmp_path / "imports.txt"
    allow.write_text("# declared imports\npoca_host_add\n")
    assert cli_main([f"--allow-imports-file={allow}", str(elf_path)]) == 0
    assert "policy OK" in capsys.readouterr().out


def test_cli_rejects_a_violating_elf_and_names_the_reasons(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    elf_path = tmp_path / "evil.elf"
    elf_path.write_bytes(
        build_elf(
            extra_sections=(".ctors",),
            undefined=("malloc",),
            defined_globals=[(DEFAULT_ENTRY, STT_FUNC), ("extra", STT_FUNC)],
            rela=((51, 0),),
        )
    )
    assert cli_main([str(elf_path)]) == 1
    out = capsys.readouterr().out
    for needle in (".ctors", "malloc", "extra", "TLSDESC"):
        assert needle in out
