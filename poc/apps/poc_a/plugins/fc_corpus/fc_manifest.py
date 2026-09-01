"""Fail-closed corpus manifest (poc-a task T14).

Single source of truth for the 14 tamper classes of development-plan
section 4 item 9 / TEST-003 / SEC-004, the order-unbypassable double-fault
proof, and the consolidated union of every hostile fixture shipped by the
earlier tasks (T5 relocation/PH identity, T6 C++ sections, T7 IRAM budget,
T8 admission). ``fc_runner.py`` drives the board strictly from this table;
the firmware side asserts the exact stage+error code per fixture and the
entry-query canary (execution sentinel) across every injection.

Fixture sources:
  "fc"     committed byte-copies of the T3 deterministic host corpus
           (poc/host/tests/mpb_corpus, generator poc/host/tests/
           gen_mpb_corpus.py, TEST-only key e5677a1c) - streamed to the
           board over the console (attacker-bytes path)
  "build"  generated at firmware-build time by the hostile_signed recipes
           (plugins/negative/build_negative_corpus.py and friends) - also
           streamed over the console from the build directory
  "embed"  already linked into the firmware; driven via ``poca load``
           whose device-side NEGATIVE assertion prints the errno

Stages: "mpb" = rejected inside mpb_parse (structure -> signature -> hash
-> epoch/target/ABI/features, hardcoded order); "relocate" = signature/
hash-valid container rejected by the loader layers (patches p1/p2/p4/p5).
Codes are frame_err_t values (or loader errnos for the relocate stage).
"""

from __future__ import annotations

# (class, fixture, source, stage, expected_code)
FC_CLASSES: list[tuple[str, str, str, str, int]] = [
    ("magic", "neg_magic", "fc", "mpb", -18),
    ("total-size", "neg_total_size", "fc", "mpb", -18),
    ("offset-oob", "neg_offset_oob", "fc", "mpb", -18),
    ("overlap", "neg_overlap", "fc", "mpb", -18),
    ("dup-required", "neg_dup_required", "fc", "mpb", -18),
    ("unknown-required-tlv", "neg_unknown_required_tlv", "fc", "mpb", -18),
    ("mixed-both", "neg_mixed_elf_in_res", "fc", "mpb", -18),
    ("mixed-both", "neg_mixed_res_in_elf", "fc", "mpb", -18),
    ("payload-hash", "neg_payload_hash", "fc", "mpb", -19),
    ("signature", "neg_signature", "fc", "mpb", -17),
    ("key-id", "neg_key_id", "fc", "mpb", -17),
    ("target", "neg_target", "fc", "mpb", -35),
    ("abi", "neg_abi", "fc", "mpb", -7),
    ("epoch", "neg_epoch", "fc", "mpb", -34),
    ("elf-identity", "neg_phbe", "build", "relocate", -22),
    ("elf-identity", "neg_ph64", "build", "relocate", -22),
    ("elf-identity", "neg_phmach", "build", "relocate", -22),
]

FC_CLASS_ORDER: list[str] = [
    "magic",
    "total-size",
    "offset-oob",
    "overlap",
    "dup-required",
    "unknown-required-tlv",
    "mixed-both",
    "payload-hash",
    "signature",
    "key-id",
    "target",
    "abi",
    "epoch",
    "elf-identity",
]

# SEC-004 order proof: the double-fault fixture carries BOTH a corrupted
# payload hash AND a corrupted signature; the parser must report the
# signature error (-17) because signature verification precedes hashing.
FC_ORDER_PROOF: tuple[str, str, str, int] = ("neg_double_fault", "fc", "mpb", -17)

# Consolidated union of the embedded hostile set (device-side NEGATIVE
# errno assertions via `poca load`): (name, needle fragments).
FC_EMBEDDED: list[tuple[str, str]] = [
    ("import_neg", "errno -88, expected -88"),
    ("neg_r32", "errno -22, expected -22"),
    ("neg_s0op", "errno -22, expected -22"),
    ("neg_phspan", "phdr admission rejected before load"),
    ("neg_phbe", "errno -22, expected -22"),
    ("neg_ph64", "errno -22, expected -22"),
    ("neg_phmach", "errno -22, expected -22"),
    ("neg_maxmem", "rejected pre-init"),
    ("cxx_ctor", "errno -22, expected -22"),
    ("cxx_tls", "errno -22, expected -22"),
    ("neg_tls_nosect", "errno -22, expected -22"),
    ("iram_mismatch", "errno -22, expected -22"),
]

FC_EMBEDDED_EXTRA_NEEDLES: dict[str, list[str]] = {
    "cxx_ctor": ["Forbidden C++ feature section"],
    "cxx_tls": ["Forbidden C++ feature section"],
    "neg_tls_nosect": ["Failed to relocate type"],
}
