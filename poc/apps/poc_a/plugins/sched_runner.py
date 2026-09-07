#!/usr/bin/env python3
"""On-board runner for the T13 scheduler semantics vectors (`poca sched`).

Drives the poc_a console over /dev/ttyACM0 and asserts, per run, the
[PASS-<vector>] markers emitted by the on-target code after its own
programmatic counter/continuity checks (misleading_success guard: the
firmware verdict is authoritative, this runner only re-verifies markers and
rejects any [FAIL- marker). Default: 3 full-suite runs for flake detection
(T13 adversarial: all runs must be green).

Consistency: pair each target vector with its host-suite result (the host
suite is run separately by the task gates) to build the host==target table
in the task evidence log.

Serial session conventions learned in T4: "\r" line endings, quiesce-based
prompt detection, RTS pulse to reboot after opening the port.
"""

from __future__ import annotations

import argparse
import sys
import time

import serial

PROMPT = "poca> "
VECTORS = [
    "fifo",
    "fair",
    "noconc",
    "dupclear",
    "storm",
    "qfull",
    "completion",
    "quiesce",
    "stalegen",
    "loader",
]


class Board:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 1.0) -> None:
        self.serial = serial.Serial(port, baud, timeout=timeout)
        self.serial.setRTS(True)
        time.sleep(0.1)
        self.serial.setRTS(False)
        time.sleep(0.1)
        self.serial.reset_input_buffer()

    def command(self, line: str, deadline_s: float = 300.0) -> str:
        self.serial.write((line + "\r").encode())
        self.serial.flush()
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


def check_run(text: str, run_index: int) -> list[str]:
    failures: list[str] = []
    for vector in VECTORS:
        if f"[PASS-{vector}]" not in text:
            failures.append(f"run{run_index}: missing [PASS-{vector}]")
    if "[FAIL-" in text:
        failures.append(f"run{run_index}: firmware emitted a [FAIL- marker")
    if "SCHED SUITE PASS" not in text:
        failures.append(f"run{run_index}: missing SCHED SUITE PASS summary")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--repeat-prompt-wait", type=float, default=300.0)
    args = parser.parse_args()

    board = Board(args.port)
    failures: list[str] = []
    per_run_vectors: dict[int, set[str]] = {}
    try:
        boot = board.command("", deadline_s=8.0)
        if "poca> " not in boot:
            # First prompt interleave: send a no-op and re-check.
            boot = board.command("poca status", deadline_s=30.0)
            if "poca> " not in boot:
                print("FATAL: no console prompt after reboot")
                return 2
        for run in range(1, args.runs + 1):
            print(f"===== SCHED RUN {run}/{args.runs} =====")
            text = board.command("poca sched all", deadline_s=args.repeat_prompt_wait)
            per_run_vectors[run] = {vector for vector in VECTORS if f"[PASS-{vector}]" in text}
            failures.extend(check_run(text, run))
    finally:
        board.close()

    print("===== STABILITY SUMMARY =====")
    for run in range(1, args.runs + 1):
        ok = per_run_vectors.get(run) == set(VECTORS)
        print(
            f"run {run}: {len(per_run_vectors.get(run, set()))}/{len(VECTORS)} vectors "
            f"{'GREEN' if ok else 'INCOMPLETE'}"
        )
    if failures:
        for failure in failures:
            print(f"VIOLATION: {failure}")
        return 1
    print(f"SCHED STABILITY {args.runs}x{len(VECTORS)} : ALL GREEN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
