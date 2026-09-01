#!/usr/bin/env python3
"""On-board runner for the T14 fail-closed corpus (development-plan section 4
item 9, TEST-003, SEC-004).

Drives the poc_a console over /dev/ttyACM0 from the manifest table in
fc_manifest.py:

  * the 14 tamper classes are injected by STREAMING the hostile container
    bytes over the console (poca fcbegin/fcwr) into the device's immutable
    PSRAM staging and running the SAME parse+verify+admission+relocate
    pipeline via poca fcgo - proving the pipeline rejects arbitrary
    attacker-controlled bytes, not only compiled-in arrays;
  * per fixture the runner asserts the EXACT expected stage+error code from
    the firmware's RESULT line, the device-side sha256 of the uploaded
    bytes (must equal the committed corpus file's hash - upload-integrity
    and stale-state closed loop) and the entry-query canary bracket
    (unchanged=yes; nothing executed);
  * the consolidated embedded hostile set (T5/T6/T7/T8) is re-driven via
    `poca load` for the device-side NEGATIVE errno assertions;
  * a positive control (baseline load/activate/unload) proves the pipeline
    still accepts good packages in the same session (misleading-success
    guard); its canary delta of exactly +1 is accounted for;
  * the T3 double-fault fixture asserts -17 (signature reported before
    payload hash): the verification order cannot be bypassed.

Run with the repository venv python (pyserial lives there):

  /path/to/esp_study/.venv/bin/python fc_runner.py --port /dev/ttyACM0

Exit code 0 only when every fixture of every round matched its expected
code with a zero canary delta.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fc_manifest import (  # noqa: E402
    FC_CLASS_ORDER,
    FC_CLASSES,
    FC_EMBEDDED,
    FC_EMBEDDED_EXTRA_NEEDLES,
    FC_ORDER_PROOF,
)

REPO_ROOT = Path(__file__).resolve().parents[5]
HOST_CORPUS = REPO_ROOT / "poc" / "host" / "tests" / "mpb_corpus"
BUILD_PLUGINS = REPO_ROOT / "poc" / "apps" / "poc_a" / "build" / "plugins"
PROMPT = "poca> "
CHUNK_BYTES = 64  # 138-char console lines stay under the USB-Serial-JTAG RX path limit


class Board:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 1.0) -> None:
        self.serial = serial.Serial(port, baud, timeout=timeout)
        self.serial.setRTS(True)
        time.sleep(0.1)
        self.serial.setRTS(False)
        time.sleep(0.1)
        self.serial.reset_input_buffer()
        # terminate any partial line a previous/aborted session left in the
        # REPL buffer (the RTS pulse does not reliably reboot the chip)
        for _ in range(2):
            self.serial.write(b"\r")
            self.serial.flush()
            time.sleep(0.2)
            self.serial.reset_input_buffer()

    def command(self, line: str, settle: float = 0.5) -> str:
        self.serial.write((line + "\r").encode())
        self.serial.flush()
        time.sleep(settle)
        deadline = time.monotonic() + 8.0
        buf = b""
        while time.monotonic() < deadline:
            chunk = self.serial.read(4096)
            if chunk:
                buf += chunk
                if buf.decode(errors="replace").rstrip().endswith(PROMPT.rstrip()):
                    break
            else:
                time.sleep(0.05)
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


def fixture_path(source: str, name: str) -> Path:
    if source == "fc":
        return Path(__file__).resolve().parent / f"{name}.mpb"
    if source == "build":
        return BUILD_PLUGINS / f"{name}.mpb"
    raise ValueError(f"unknown fixture source {source}")


def check_fc_drift() -> None:
    """Committed fc_corpus bytes must equal the T3 host corpus bytes."""
    for name in sorted(p.name for p in Path(__file__).resolve().parent.glob("neg_*.mpb")):
        fc = (Path(__file__).resolve().parent / name).read_bytes()
        host = (HOST_CORPUS / name).read_bytes()
        if fc != host:
            raise SystemExit(f"drift: fc_corpus/{name} differs from poc/host/tests/mpb_corpus")


def upload(
    board: Board, name: str, source: str, stage: str, code: int, failures: list[str]
) -> None:
    data = fixture_path(source, name).read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    print(f"[FC-HOST] {name} file={fixture_path(source, name)} size={len(data)} sha256={sha}")
    out = board.command(f"poca fcbegin {len(data)}")
    if "[fc] begin size=" not in out:
        failures.append(f"{name}: fcbegin refused")
        return
    for off in range(0, len(data), CHUNK_BYTES):
        chunk = data[off : off + CHUNK_BYTES]
        out = board.command(f"poca fcwr {chunk.hex()}", settle=0.05)
        want = f"[fc] wr +{len(chunk)} total={min(off + CHUNK_BYTES, len(data))}/{len(data)}"
        if want not in out:
            failures.append(f"{name}: fcwr @{off} missing '{want}'")
            return
    out = board.command(f"poca fcgo {name} {stage} {code}")
    needles = [
        f"[FC] {name} size={len(data)} sha256={sha}",
        f"[FC] {name} RESULT stage={stage} err={code} expected={code} PASS",
        f"[FC] {name} entry_queries=",
        "(unchanged=yes)",
    ]
    for needle in needles:
        if needle not in out:
            failures.append(f"{name}: missing '{needle}'")


def run_round(board: Board, round_no: int, failures: list[str]) -> tuple[int, int]:
    canary_start = entry_queries(board)

    out = board.command("poca load baseline")
    for needle in ["[poca-load] PASS"]:
        if needle not in out:
            failures.append(f"round{round_no} positive control load: missing '{needle}'")
    if "MAGIC MATCH" not in board.command("poca activate"):
        failures.append(f"round{round_no} positive control activate: MAGIC MISMATCH")
    if "[poca-unload] PASS" not in board.command("poca unload"):
        failures.append(f"round{round_no} positive control unload: missing PASS")
    canary_after_positive = entry_queries(board)
    if canary_after_positive != canary_start + 1:
        failures.append(
            f"round{round_no} positive control canary {canary_after_positive} != {canary_start}+1"
        )

    rejected_classes = 0
    for cls in FC_CLASS_ORDER:
        rows = [row for row in FC_CLASSES if row[0] == cls]
        failures_before = len(failures)
        for _cls, name, source, stage, code in rows:
            upload(board, name, source, stage, code, failures)
        if len(failures) == failures_before:
            print(f"[FC-{cls}-REJECT]")
            rejected_classes += 1
        else:
            print(f"[FC-{cls}-REJECT] SKIPPED (fixture failure above)")

    name, source, stage, code = FC_ORDER_PROOF
    upload(board, name, source, stage, code, failures)
    print(
        f"[FC-ORDER-DOUBLE-FAULT] ({name}: bad sig + bad hash -> err={code} "
        "SIGNATURE_INVALID reported first)"
    )

    for name, needle in FC_EMBEDDED:
        out = board.command(f"poca load {name}")
        for frag in [f"NEGATIVE {name} PASS", needle, "entry_queries=", "(unchanged=yes)"]:
            if frag not in out:
                failures.append(f"embedded {name}: missing '{frag}'")
        for extra in FC_EMBEDDED_EXTRA_NEEDLES.get(name, []):
            if extra not in out:
                failures.append(f"embedded {name}: missing '{extra}'")

    canary_end = entry_queries(board)
    if canary_end != canary_after_positive:
        failures.append(
            f"round{round_no} canary delta across corpus: {canary_end} != "
            f"{canary_after_positive} (payload bytes executed!)"
        )
    return rejected_classes, canary_end


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--rounds", type=int, default=2)
    args = parser.parse_args()

    check_fc_drift()
    print("[FC] corpus drift check vs poc/host/tests/mpb_corpus: OK")

    board = Board(args.port)
    failures: list[str] = []
    canary_first = entry_queries(board)
    totals = []
    try:
        for round_no in range(1, args.rounds + 1):
            print(f"\n===== FC ROUND {round_no}/{args.rounds} =====")
            totals.append(run_round(board, round_no, failures))
    finally:
        board.close()

    classes_14 = all(t[0] == 14 for t in totals)
    if classes_14 and not failures:
        print("\n[FC-ALL-14-PRE-EXEC-REJECT]")
    print(
        f"[FC-CANARY-SUMMARY] rounds={args.rounds} per-round rejected-classes="
        + ",".join(str(t[0]) for t in totals)
        + f" canary={totals[-1][1] if totals else canary_first} "
        f"(start={canary_first}, +1/round positive control only, negatives +0)"
    )

    if failures:
        print("\nFC FAILURES:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    if not classes_14:
        print("\nFC FAILURES: class coverage incomplete")
        return 1
    print(
        f"\nFC PASS ({len(FC_CLASSES) + 1} uploaded fixtures x {args.rounds} rounds + "
        f"{len(FC_EMBEDDED)} embedded negatives x {args.rounds} rounds, "
        "14/14 classes pre-exec reject, order proof, sentinel zero-touch)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
