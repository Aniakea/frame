#!/usr/bin/env python3
"""On-board runner for the T5 relocation/import allowlist matrix.

Drives the poc_a console over /dev/ttyACM0 and asserts, per matrix row, the
explicit PASS/NEGATIVE markers printed by the firmware plus the
entry-query canary count (which must stay unchanged across all negatives —
nothing may execute). Exits non-zero on the first violated assertion.

Serial session conventions learned in T4: "\r" line endings, quiesce-based
prompt detection (log lines interleave after the first prompt), RTS pulse
to reboot after opening the port.
"""

from __future__ import annotations

import argparse
import sys
import time

import serial

PROMPT = "poca> "
POSITIVE_CYCLE = ["baseline", "globdat", "plt"]
NEGATIVES = [
    ("import_neg", ["NEGATIVE import_neg PASS (errno -88, expected -88)"]),
    ("neg_r32", ["NEGATIVE neg_r32 PASS (errno -22, expected -22)"]),
    ("neg_s0op", ["NEGATIVE neg_s0op PASS (errno -22, expected -22)"]),
    ("neg_phbe", ["NEGATIVE neg_phbe PASS (errno -22, expected -22)"]),
    ("neg_ph64", ["NEGATIVE neg_ph64 PASS (errno -22, expected -22)"]),
    ("neg_phmach", ["NEGATIVE neg_phmach PASS (errno -22, expected -22)"]),
    ("neg_phspan", ["NEGATIVE neg_phspan PASS (phdr admission rejected before load)"]),
    ("cxx_ctor", ["NEGATIVE cxx_ctor PASS (errno -22, expected -22)",
                  "Forbidden C++ feature section"]),
    ("cxx_tls", ["NEGATIVE cxx_tls PASS (errno -22, expected -22)",
                 "Forbidden C++ feature section"]),
    ("neg_tls_nosect", ["NEGATIVE neg_tls_nosect PASS (errno -22, expected -22)",
                        "Failed to relocate type"]),
]


class Board:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 1.0) -> None:
        self.serial = serial.Serial(port, baud, timeout=timeout)
        self.serial.setRTS(True)
        time.sleep(0.1)
        self.serial.setRTS(False)
        time.sleep(0.1)
        self.serial.reset_input_buffer()

    def command(self, line: str, settle: float = 0.6) -> str:
        self.serial.write((line + "\r").encode())
        self.serial.flush()
        time.sleep(settle)
        deadline = time.monotonic() + 8.0
        buf = b""
        while time.monotonic() < deadline:
            chunk = self.serial.read(4096)
            if chunk:
                buf += chunk
                tail = buf.decode(errors="replace")
                if tail.rstrip().endswith(PROMPT.rstrip()):
                    break
            else:
                time.sleep(0.1)
        text = buf.decode(errors="replace")
        print(f"--- poca> {line}")
        print(text, end="" if text.endswith("\n") else "\n")
        return text

    def close(self) -> None:
        self.serial.close()


def entry_queries(board: Board) -> int:
    text = board.command("poca status")
    marker = "entry queries so far: "
    for line in text.splitlines():
        if marker in line:
            return int(line.split(marker)[1].split()[0])
    raise AssertionError("entry queries marker not found in status output")


def run(port: str) -> int:
    board = Board(port)
    failures = []

    def expect(what: str, text: str, needles: list[str]) -> None:
        for needle in needles:
            if needle not in text:
                failures.append(f"{what}: missing '{needle}'")

    expect("status", board.command("poca status"),
           ["registered host imports: poca_host_sentinel poca_host_add"])
    canary_start = entry_queries(board)

    for name in POSITIVE_CYCLE:
        expect(f"verify {name}", board.command(f"poca verify {name}"),
               [f"[poca-verify] {name} size=", "phdr loads=", "[poca-verify] PASS"])
        load = board.command(f"poca load {name}")
        expect(f"load {name}", load,
               ["[poca-load] mpb size=", "sha256=", "[poca-load] PASS"])
        if name == "globdat":
            expect("globdat value", load, ["GLOB_DAT host_sentinel=0x", " MATCH"])
        if name == "plt":
            expect("plt value", load, ["GLOB_DAT host_add=0x", " MATCH"])
        expect(f"activate {name}", board.command("poca activate"),
               ["MAGIC MATCH", "[poca-activate] PASS"])
        expect(f"unload {name}", board.command("poca unload"), ["[poca-unload] PASS"])

    canary = entry_queries(board)
    if canary != canary_start + len(POSITIVE_CYCLE):
        failures.append(f"canary after positives: {canary} != "
                        f"{canary_start}+{len(POSITIVE_CYCLE)}")

    for name, needles in NEGATIVES:
        expect(f"load {name}", board.command(f"poca load {name}"),
               needles + ["entry_queries=", "(unchanged=yes)"])

    canary_neg = entry_queries(board)
    if canary_neg != canary:
        failures.append(f"canary after negatives: {canary_neg} != {canary} (executed bytes!)")

    expect("re-load baseline", board.command("poca load baseline"), ["[poca-load] PASS"])
    expect("re-activate", board.command("poca activate"), ["MAGIC MATCH"])
    expect("re-unload", board.command("poca unload"), ["[poca-unload] PASS"])

    board.close()

    if failures:
        print("\nMATRIX FAILURES:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(f"\nMATRIX PASS (positives={len(POSITIVE_CYCLE)}, negatives={len(NEGATIVES)}, "
          f"canary={canary_neg} unchanged across all negatives, "
          f"canary_start={canary_start})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    args = parser.parse_args()
    return run(args.port)


if __name__ == "__main__":
    sys.exit(main())
