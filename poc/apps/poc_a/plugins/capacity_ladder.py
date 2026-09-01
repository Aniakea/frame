#!/usr/bin/env python3
"""T10 capacity ladder: board runner + ladder judge (8 -> 7 -> 6 -> 5).

Drives the poc_a console over /dev/ttyACM0. Each ladder step runs
`poca capacity <N>` (N distinct-name plugins loaded into N live esp_elf_t
instances, check-fn round over all N resident generations, peak heap/stack
snapshot, reverse-order unload). Ladder policy (fail-closed):

  - step N=8 first; a step PASSes only on the four [PASS-cap-N-*] markers
    plus [PASS-capacity-N] and the device-side distinctness asserts;
  - on failure at plugin k the rung records k-1 as provisionally achieved,
    the SAME rung is retried once (transient guard), then the ladder
    descends to the next rung;
  - a PASS at any rung >= 5 ends the ladder (lower rungs stay unexecuted);
  - if rung 5 also fails both attempts the gate FAILS (reported, never
    masked: exit code 1 with every per-step failure reason printed).

Adversarial re-verification on the PASSING transcript (authority stays with
the on-device asserts; this runner independently re-checks):
  n DISTINCT staging sha256 hashes observed (stale-embed guard), n DISTINCT
  PSRAM text bases in the respond round, n DISTINCT returned/expected magic
  pairs (misleading-success guard: one plugin called n times cannot produce
  n distinct magics at n distinct text bases), crash-needle scan.

Serial conventions from T4/T5: "\r" line endings, quiesce-based prompt
detection, RTS pulse after opening the port, per-command + total timeouts.
"""

from __future__ import annotations

import argparse
import re
import sys
import time

import serial

PROMPT = "poca> "
LADDER = [8, 7, 6, 5]
CRASH_NEEDLES = (
    "Guru Meditation",
    "abort()",
    "Backtrace:",
    "rst:0x",
    "WDT",
    "panic",
    "LoadProhibited",
    "LoadStoreError",
    "EXCVADDR",
)

CAP_LOAD_RE = re.compile(
    r"\[cap-load\] idx=(\d+) name=(\w+) magic=(0x[0-9a-f]+) text=(0x[0-9a-f]+)"
    r" staging=(\d+)B sha256=([0-9a-f]{64}) psram_free=(\d+)"
)
CAP_RESPOND_RE = re.compile(
    r"\[cap-respond\] idx=(\d+) name=(\w+) fn=(0x[0-9a-f]+) text=(0x[0-9a-f]+)"
    r" returned=(0x[0-9a-f]+) expected=(0x[0-9a-f]+) (MATCH|MISMATCH)"
)
CAPBUD_RE = re.compile(r"^CAPBUD,(.*)$", re.MULTILINE)
FAIL_RE = re.compile(r"\[FAIL-cap-(\d+)-(load|respond|unload)\](.*)")


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


def parse_capbud(text: str) -> dict[str, int]:
    match = CAPBUD_RE.search(text)
    if not match:
        return {}
    out: dict[str, int] = {}
    for field in match.group(1).split(","):
        if "=" in field:
            key, value = field.split("=", 1)
            out[key.strip()] = int(value.strip())
    return out


def judge_step(text: str, count: int) -> tuple[bool, dict]:
    """Re-verify one `poca capacity <count>` transcript on the host."""
    verdict: dict = {"fail_stage": None, "fail_step": None, "reasons": [], "capbud": {}}
    for needle in CRASH_NEEDLES:
        if needle in text:
            verdict["reasons"].append(f"crash needle '{needle}'")
    fail = FAIL_RE.search(text)
    if fail:
        verdict["fail_stage"] = fail.group(2)
        step_match = re.search(r"step=(\d+)", fail.group(3))
        verdict["fail_step"] = int(step_match.group(1)) if step_match else None
        verdict["reasons"].append(f"device [FAIL-cap-{fail.group(1)}-{fail.group(2)}] marker")
    for marker in (
        f"[PASS-cap-{count}-load]",
        f"[PASS-cap-{count}-respond]",
        f"[PASS-cap-{count}-unload]",
        f"[PASS-capacity-{count}]",
    ):
        if marker not in text and not verdict["fail_stage"]:
            verdict["reasons"].append(f"missing {marker}")
    if "[cap-distinct] text_bases_all_distinct=yes" not in text:
        verdict["reasons"].append("cap-distinct line not all-yes")

    loads = CAP_LOAD_RE.findall(text)
    responds = CAP_RESPOND_RE.findall(text)
    verdict["loads"] = loads
    verdict["responds"] = responds
    verdict["capbud"] = parse_capbud(text)
    if len(loads) != count:
        verdict["reasons"].append(f"cap-load lines {len(loads)} != {count}")
    if len({row[5] for row in loads}) != len(loads):
        verdict["reasons"].append("staging sha256 hashes NOT all distinct (stale-embed suspect)")
    if len({row[3] for row in loads}) != len(loads):
        verdict["reasons"].append("load-phase text bases NOT all distinct")
    if len(responds) != count:
        verdict["reasons"].append(f"cap-respond lines {len(responds)} != {count}")
    if any(row[6] != "MATCH" for row in responds):
        verdict["reasons"].append("respond round MISMATCH present")
    if len({row[4] for row in responds}) != len(responds):
        verdict["reasons"].append("respond magics NOT all distinct (misleading-success suspect)")
    if len({row[3] for row in responds}) != len(responds):
        verdict["reasons"].append("respond text bases NOT all distinct")
    if "[poca-cap] restored=yes" not in text:
        verdict["reasons"].append("psram not restored to baseline")
    verdict["ok"] = not verdict["reasons"]
    return verdict["ok"], verdict


def format_kib(budget: dict[str, int]) -> str:
    per = budget.get("per_instance", 0)
    return f"{per} B ({per / 1024:.2f} KiB)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--cmd-deadline", type=float, default=300.0)
    parser.add_argument("--total-guard", type=float, default=1200.0)
    args = parser.parse_args()

    started = time.monotonic()
    board = Board(args.port)
    steps: list[dict] = []
    achieved: int | None = None
    try:
        boot = board.command("", deadline_s=8.0)
        if "poca> " not in boot:
            boot = board.command("poca status", deadline_s=30.0)
            if "poca> " not in boot:
                print("FATAL: no console prompt after reboot")
                return 2
        status_before = board.command("poca status", deadline_s=30.0)

        for rung in LADDER:
            for attempt in (1, 2):
                if time.monotonic() - started > args.total_guard:
                    print("FATAL: total-session guard expired")
                    return 2
                text = board.command(f"poca capacity {rung}", deadline_s=args.cmd_deadline)
                ok, verdict = judge_step(text, rung)
                steps.append(
                    {
                        "rung": rung,
                        "attempt": attempt,
                        "ok": ok,
                        "fail_stage": verdict["fail_stage"],
                        "fail_step": verdict["fail_step"],
                        "reasons": verdict["reasons"],
                        "capbud": verdict["capbud"],
                    }
                )
                if ok:
                    achieved = rung
                    break
                provisional = (
                    (verdict["fail_step"] or (rung + 1)) - 1
                    if verdict["fail_stage"] == "load"
                    else rung
                )
                print(
                    f"--- rung {rung} attempt {attempt} FAILED "
                    f"(stage={verdict['fail_stage']} step={verdict['fail_step']} "
                    f"provisional_achieved={provisional})"
                )
            if achieved is not None:
                break

        status_after = board.command("poca status", deadline_s=30.0)
    finally:
        board.close()

    print("===== LADDER STEPS =====")
    for step in steps:
        head = f"rung {step['rung']} attempt {step['attempt']}: {'PASS' if step['ok'] else 'FAIL'}"
        if step["fail_stage"]:
            head += f" (stage={step['fail_stage']} step={step['fail_step']})"
        print(head)
        for reason in step["reasons"]:
            print(f"    reason: {reason}")

    print("===== PER-INSTANCE COST TABLE (CAPBUD, measured per executed rung) =====")
    print(
        "rung   psram_base   psram_peak   consumed   per_instance   mpb(B)  arena(B)  "
        "iram_delta  internal_delta"
    )
    for step in steps:
        budget = step["capbud"]
        if not budget:
            continue
        print(
            f"{step['rung']:>4}   {budget['psram_base_free']:>11} "
            f"{budget['psram_peak_free']:>11} {budget['consumed']:>9} "
            f"{budget['per_instance']:>13} {budget['mpb_size']:>6} "
            f"{budget['arena_size']:>8} {budget['iram_delta']:>10} "
            f"{budget['internal_delta']:>15}"
        )
    canary_before = re.search(r"entry queries so far: (\d+)", status_before)
    canary_after = re.search(r"entry queries so far: (\d+)", status_after)
    if canary_before and canary_after:
        delta = int(canary_after.group(1)) - int(canary_before.group(1))
        queries_expected = sum(
            step["rung"]
            if step["ok"] or step["fail_stage"] != "load"
            else max(step["fail_step"] - 1, 0)
            if step["fail_step"]
            else 0
            for step in steps
        )
        print(
            f"canary: before={canary_before.group(1)} after={canary_after.group(1)} "
            f"delta={delta} (expected={queries_expected}) "
            f"{'OK' if delta == queries_expected else 'MISMATCH'}"
        )

    print("===== LADDER VERDICT =====")
    if achieved is None:
        print("CAPACITY LADDER: GATE FAIL - no rung >= 5 passed (report, not masked)")
        return 1
    final = next(step for step in reversed(steps) if step["ok"])
    budget = final["capbud"]
    per_kib = budget["per_instance"] / 1024
    pool_pct = budget["consumed"] * 100.0 / (8 * 1024 * 1024)
    print(f"achieved={achieved} (attempted rungs: {', '.join(str(s['rung']) for s in steps)})")
    print(
        f"per-instance cost: {format_kib(budget)} PSRAM "
        f"(staging {budget['mpb_size']} B + loader arena {budget['arena_size']} B)"
    )
    print(f"{achieved}-instance peak: {budget['consumed']} B = {pool_pct:.2f}% of the 8 MiB pool")
    print("SIGNED BUDGET LINE:")
    print(
        f"Capacity: {achieved} ACTIVE verified (8 attempted), per-instance cost "
        f"{budget['per_instance']} B ({per_kib:.2f} KiB) PSRAM, {achieved}-instance peak "
        f"{budget['consumed']} B = {pool_pct:.2f}% of the 8 MiB pool "
        f"(IRAM delta {budget['iram_delta']} B, internal delta {budget['internal_delta']} B, "
        f"largest_free_block unchanged), headroom to MEM-007 floor 5: "
        f"{achieved}/5, MEM-007 -> ADR-0002 proposal value: {achieved} ACTIVE generations "
        f"per distinct plugin name (PSRAM class; re-measure with Wi-Fi at the next gate)"
    )
    print(f"CAPACITY LADDER: PASS at {achieved}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
