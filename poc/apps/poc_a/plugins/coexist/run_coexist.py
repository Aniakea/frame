#!/usr/bin/env python3
"""On-board runner for T8 generation coexistence + admission baseline.

Drives the poc_a console over /dev/ttyACM0 and asserts the explicit T8
markers printed by the firmware:
  [PASS-v2-candidate-loaded] [PASS-single-candidate-guard]
  [PASS-both-generations-respond] [PASS-v1-active-60calls]
  [PASS-swap-unload-old] [PASS-budget-table]
plus the per-phase heap snapshots, the >256KiB-requestable positive
(baseline_300k), the hostile 600KiB admission negative (neg_maxmem) and the
plain baseline_v2 generation positive. Exits non-zero on the first violated
assertion.

`poca coexist` blocks for ~35s (60 x 500ms loop window), so this command
gets a longer deadline than the matrix runner's 8s default. Serial session
conventions learned in T4/T5: "\\r" line endings, quiesce-based prompt
detection, RTS pulse to reboot after opening the port.
"""

from __future__ import annotations

import argparse
import sys
import time

import serial

PROMPT = "poca> "
COEXIST_MARKERS = [
    "[PASS-v2-candidate-loaded]",
    "[PASS-single-candidate-guard]",
    "[PASS-both-generations-respond]",
    "[PASS-v1-active-60calls]",
    "[PASS-swap-unload-old]",
    "[PASS-budget-table]",
]
HEAP_PHASES = ["baseline", "v1-active", "coexist-peak", "v2-active", "final"]


class Board:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 1.0) -> None:
        self.serial = serial.Serial(port, baud, timeout=timeout)
        self.serial.setRTS(True)
        time.sleep(0.1)
        self.serial.setRTS(False)
        time.sleep(0.1)
        self.serial.reset_input_buffer()

    def command(self, line: str, settle: float = 0.6, deadline_s: float = 8.0) -> str:
        self.serial.write((line + "\r").encode())
        self.serial.flush()
        time.sleep(settle)
        deadline = time.monotonic() + deadline_s
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
           ["registered host imports: poca_host_sentinel poca_host_add",
            "heap psram", "heap iram-exec"])
    canary_start = entry_queries(board)

    # T8 generation positive: baseline_v2 (same manifest name "baseline",
    # version 2.0.0, distinct activate magic).
    expect("verify v2", board.command("poca verify baseline_v2"),
           ["[poca-verify] baseline_v2 size=", "sha256=", "version=2.0.0",
            "[poca-verify] PASS"])
    expect("load v2", board.command("poca load baseline_v2"),
           ["[poca-load] mpb size=", "[poca-load] identity name=baseline version=2.0.0 MATCH",
            "[poca] admission max_mem=262144", "[poca-load] PASS"])
    expect("activate v2", board.command("poca activate"), ["MAGIC MATCH"])
    expect("unload v2", board.command("poca unload"), ["[poca-unload] PASS"])

    # T8 requestable positive: 300KiB declaration (default arena is 256KiB;
    # hard max is 512KiB -> admissible, MEM-003).
    expect("verify 300k", board.command("poca verify baseline_300k"),
           ["[poca-verify] baseline_300k size=", "version=1.0.1", "[poca-verify] PASS"])
    expect("load 300k", board.command("poca load baseline_300k"),
           ["[poca] admission max_mem=307200 default_arena=262144 hard_max=524288 OK",
            "[poca-load] PASS"])
    expect("activate 300k", board.command("poca activate"), ["MAGIC MATCH"])
    expect("unload 300k", board.command("poca unload"), ["[poca-unload] PASS"])

    # T8 admission negative: hostile 600KiB declaration rejected pre-init.
    expect("load neg_maxmem", board.command("poca load neg_maxmem"),
           ["[poca] admission max_mem=614400 default_arena=262144 hard_max=524288 "
            "REJECT(>hard max)",
            "NEGATIVE neg_maxmem PASS (admission: max_memory=614400 > 524288 hard max, "
            "rejected pre-init)",
            "entry_queries=", "(unchanged=yes)"])

    canary_mid = entry_queries(board)
    if canary_mid != canary_start + 2:
        failures.append(f"canary after v2/300k loads + neg_maxmem: {canary_mid} != "
                        f"{canary_start}+2 (two queries; negative must not execute)")

    # T8 coexistence scenario (long: ~35s blocking).
    coexist = board.command("poca coexist", settle=1.0, deadline_s=120.0)
    expect("coexist", coexist, COEXIST_MARKERS + [
        "[poca-coexist] phase 1: load v1 (baseline 1.0.0) as ACTIVE",
        "[poca-coexist] identity name=baseline version=1.0.0 MATCH",
        "[poca-cand] identity name=baseline version=2.0.0 MATCH",
        "independent arenas",
        "v1 ACTIVE activate returned=0x",
        "gen-old activate fn=0x",
        "gen-new activate fn=0x",
        "distinct code addresses=yes",
        "final calls=", "loop final calls=",
        "[T8-budget] psram baseline_free=",
        "[T8-budget] headroom vs 8MiB pool:",
        "[T8-budget] iram-exec baseline_free=",
        "[poca-coexist] PASS",
    ])
    for phase in HEAP_PHASES:
        expect("coexist heap", coexist, [f"[T8-heap] phase={phase} "])
    expect("coexist stacks", coexist,
           ["[T8-stack] phase=baseline", "[T8-stack] phase=coexist-peak", "poca_gen",
            "[T8-stack]   "])

    # Post-scenario clean state: plain load/activate/unload still works.
    expect("post load", board.command("poca load baseline"), ["[poca-load] PASS"])
    expect("post activate", board.command("poca activate"), ["MAGIC MATCH"])
    expect("post unload", board.command("poca unload"), ["[poca-unload] PASS"])

    final_status = board.command("poca status")
    expect("final status", final_status, ["heap psram", "heap iram-exec"])

    board.close()

    if failures:
        print("\nCOEXIST FAILURES:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(f"\nCOEXIST PASS (6/6 markers, {len(HEAP_PHASES)} heap phases, "
          f"300KiB requestable + 600KiB rejected, canary start={canary_start})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    args = parser.parse_args()
    return run(args.port)


if __name__ == "__main__":
    sys.exit(main())
