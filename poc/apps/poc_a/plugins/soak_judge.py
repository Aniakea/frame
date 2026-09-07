#!/usr/bin/env python3
"""T9 thousand-cycle lifecycle soak: board runner + judgment gate.

Two modes:

  run:    soak_judge.py --port /dev/ttyACM0 --cycles 1000 [--mix default]
              [--raw-out FILE] [--csv-out FILE]
          Flashes nothing; drives the poc_a console: captures `poca status`,
          runs `poca soak <n> [mix]` to completion (progress lines keep the
          session observable; idle/total timeouts guard a hung command),
          then a post-soak positive regression (load/activate/unload +
          `poca iram 3`). Every serial byte lands in --raw-out verbatim.

  judge:  soak_judge.py --transcript FILE [--csv-out FILE]
          Parses the SOAKHDR/SOAKTASKS/SOAKTASKSZ/SOAKSMP/SOAKSUM lines and
          applies the development-plan section 4 item 6 / MEM-008 gate:
            (a) psram largest_free_block: first- vs last-quartile mean
                decline < 15% AND no monotonic decline outside a noise band
            (b) psram + iram min_ever stable (delta < one default plugin
                arena, 256 KiB)
            (c) internal free bytes: no trend (same quartile + mono test)
            (d) stack high-water: no task within 10% of its stack size at
                the final sample (known sizes; unknown tasks get a 512 B
                floor and are reported)
            (e) heap integrity probes all OK (every 50 cycles)
            (f) zero errors, completed == cycles, value-match count equals
                the expected count (cycles - swaps + 4*swaps), swaps all ok,
                canary delta == expected entry queries
          plus crash/WDT needle scan and (when the transcript carries the
          regression section) the post-soak positive markers.

The verdict authority for task T9 is THIS SCRIPT over the raw samples, not
the device-side summary alone. Exit code 0 iff SOAK VERDICT: PASS.
"""

from __future__ import annotations

import argparse
import math
import sys
import time
from pathlib import Path

PROMPT = "poca> "
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
DEFAULT_ARENA_BYTES = 256 * 1024
DECLINE_LIMIT = 0.15
NOISE_FLOOR_BYTES = 4096
NOISE_RATIO = 0.005
MONO_MIN_STEPS = 3
UNKNOWN_TASK_FLOOR_BYTES = 512


def parse_kv_csv(line: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for field in line.split(","):
        if "=" in field:
            key, value = field.split("=", 1)
            out[key.strip()] = value.strip()
    return out


class SoakData:
    def __init__(self) -> None:
        self.header: dict[str, str] = {}
        self.task_names: list[str] = []
        self.task_sizes: list[int] = []
        self.samples: list[dict[str, int]] = []
        self.summary: dict[str, str] = {}
        self.prog_lines = 0
        self.swap_ok_lines = 0
        self.fail_lines = 0
        self.done_line = False
        self.regression: list[str] = []

    def sample_columns(self) -> list[str]:
        return ["psram_free", "psram_largest", "psram_min_ever", "iram_free", "iram_largest",
                "iram_min_ever", "int_free", "int_largest", "int_min_ever"]


def parse_transcript(text: str) -> SoakData:
    data = SoakData()
    in_regression = False
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line == "--- REGRESSION-BEGIN ---":
            in_regression = True
            continue
        if line == "--- REGRESSION-END ---":
            in_regression = False
            continue
        if in_regression:
            data.regression.append(line)
        if line.startswith("SOAKHDR,"):
            data.header = parse_kv_csv(line[len("SOAKHDR,"):])
        elif line.startswith("SOAKTASKSZ,"):
            data.task_sizes = [int(v) for v in line[len("SOAKTASKSZ,"):].split(",")]
        elif line.startswith("SOAKTASKS,"):
            data.task_names = line[len("SOAKTASKS,"):].split(",")
        elif line.startswith("SOAKSMP,"):
            fields = [int(v) for v in line[len("SOAKSMP,"):].split(",")]
            row: dict[str, int] = {"idx": fields[0], "cycle": fields[1]}
            for col, value in zip(data.sample_columns(), fields[2:11], strict=False):
                row[col] = value
            for name, value in zip(data.task_names, fields[11:], strict=False):
                row[f"hwm_{name}"] = value
            data.samples.append(row)
        elif line.startswith("SOAKSUM,"):
            data.summary = parse_kv_csv(line[len("SOAKSUM,"):])
        elif line.startswith("[poca-soak] prog "):
            data.prog_lines += 1
        elif line.startswith("[poca-soak] swap cycle ") and "ok=yes" in line:
            data.swap_ok_lines += 1
        elif line.startswith("[poca-soak] FAIL"):
            data.fail_lines += 1
        elif line.startswith("[poca-soak] DONE"):
            data.done_line = True
    return data


def trend(values: list[int]) -> tuple[float, float, int, int, int]:
    """Return (q1_mean, q4_mean, decline_ratio, n_dec, n_inc) for a series."""
    n = len(values)
    quart = max(1, n // 4)
    q1 = sum(values[:quart]) / quart
    q4 = sum(values[-quart:]) / quart
    decline = (q1 - q4) / q1 if q1 > 0 else math.inf
    noise = max(NOISE_FLOOR_BYTES, NOISE_RATIO * q1)
    dec = inc = 0
    for prev, cur in zip(values, values[1:]):
        delta = cur - prev
        if delta < -noise:
            dec += 1
        elif delta > noise:
            inc += 1
    return q1, q4, decline, dec, inc


def judge(data: SoakData, text: str) -> tuple[bool, list[str], list[str]]:
    checks: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str) -> None:
        checks.append((name, ok, detail))

    if not data.header or not data.summary or not data.samples:
        print("SOAK VERDICT: FAIL")
        print("  parse: missing SOAKHDR/SOAKSMP/SOAKSUM (incomplete transcript?)")
        return False, [], []

    cycles = int(data.header["cycles"])
    sample_every = int(data.header["sample_every"])
    swap_every = int(data.header["swap_every"])
    integrity_every = int(data.header["integrity_every"])
    s = data.summary
    completed = int(s["completed"])
    errors = int(s["errors"])
    matches = int(s["value_matches"])
    expected_matches = int(s["expected_value_matches"])
    swaps = int(s["swaps"])
    swap_ok = int(s["swap_ok"])
    probes = int(s["integrity_probes"])
    probes_ok = int(s["integrity_ok"])
    canary_delta = int(s["canary_after"]) - int(s["canary_before"])
    expected_queries = int(s["expected_queries"])

    # (f) completion, error, value-match and swap accounting.
    check("f.completed", completed == cycles, f"completed={completed}/{cycles}")
    check("f.errors", errors == 0 and data.fail_lines == 0,
          f"errors={errors} fail_lines={data.fail_lines}")
    computed_matches = (cycles - swaps) + 4 * swaps
    check("f.matches", matches == expected_matches == computed_matches,
          f"value_matches={matches} expected={expected_matches} computed={computed_matches}")
    check("f.swaps", swaps == cycles // swap_every, f"swaps={swaps} (cycles/{swap_every})")
    check("f.swap_ok", swap_ok == swaps and data.swap_ok_lines == swap_ok,
          f"swap_ok={swap_ok}/{swaps} lines={data.swap_ok_lines}")
    check("f.canary", canary_delta == expected_queries == cycles + swaps,
          f"canary_delta={canary_delta} expected_queries={expected_queries} "
          f"(cycles+swaps={cycles + swaps})")
    check("f.samples", len(data.samples) == 1 + cycles // sample_every,
          f"sample_rows={len(data.samples)} expected={1 + cycles // sample_every}")
    check("f.prog", data.prog_lines == cycles // sample_every,
          f"prog_lines={data.prog_lines} expected={cycles // sample_every}")
    check("f.done", data.done_line, "device DONE line present")

    # (e) integrity probes.
    expected_probes = cycles // integrity_every
    check("e.integrity", probes == expected_probes and probes_ok == probes,
          f"integrity_ok={probes_ok}/{probes} expected_probes={expected_probes}")

    # (a) psram largest-block trend.
    largest = [row["psram_largest"] for row in data.samples]
    q1, q4, decline, dec, inc = trend(largest)
    check("a.psram_largest_decline", decline < DECLINE_LIMIT,
          f"q1_mean={q1:.0f} q4_mean={q4:.0f} decline={decline * 100:.2f}% "
          f"(limit {DECLINE_LIMIT * 100:.0f}%)")
    check("a.psram_largest_monotone", not (inc == 0 and dec >= MONO_MIN_STEPS),
          f"outside-noise steps dec={dec} inc={inc} "
          f"(noise={max(NOISE_FLOOR_BYTES, NOISE_RATIO * q1):.0f}B, "
          f"mono-fail iff inc==0 and dec>={MONO_MIN_STEPS})")

    # (b) min_ever stability.
    pm_first, pm_last = data.samples[0]["psram_min_ever"], data.samples[-1]["psram_min_ever"]
    im_first, im_last = data.samples[0]["iram_min_ever"], data.samples[-1]["iram_min_ever"]
    pm_delta = abs(pm_last - pm_first)
    im_delta = abs(im_last - im_first)
    check("b.psram_min_ever", pm_delta < DEFAULT_ARENA_BYTES,
          f"psram min_ever first={pm_first} last={pm_last} delta={pm_delta}B "
          f"(<arena {DEFAULT_ARENA_BYTES}B)")
    check("b.iram_min_ever", im_delta < DEFAULT_ARENA_BYTES,
          f"iram min_ever first={im_first} last={im_last} delta={im_delta}B")

    # (c) internal free trend.
    internal = [row["int_free"] for row in data.samples]
    iq1, iq4, idecline, idec, iinc = trend(internal)
    check("c.internal_free_trend",
          idecline < DECLINE_LIMIT and not (iinc == 0 and idec >= MONO_MIN_STEPS),
          f"q1_mean={iq1:.0f} q4_mean={iq4:.0f} decline={idecline * 100:.2f}% "
          f"dec={idec} inc={iinc}")

    # (d) stack high-water margins at the final sample.
    final = data.samples[-1]
    stack_details = []
    stack_ok = True
    for name, size in zip(data.task_names, data.task_sizes, strict=False):
        hwm = final.get(f"hwm_{name}")
        if hwm is None:
            continue
        if size > 0:
            ok = hwm > 0.10 * size
            stack_details.append(f"{name} hwm={hwm}B/{size}B (used "
                                 f"{100 - 100 * hwm / size:.1f}%)")
        else:
            ok = hwm >= UNKNOWN_TASK_FLOOR_BYTES
            stack_details.append(f"{name} hwm={hwm}B/size-unknown "
                                 f"(floor {UNKNOWN_TASK_FLOOR_BYTES}B)")
        stack_ok = stack_ok and ok
    check("d.stack_margin", stack_ok, "; ".join(stack_details))

    # crash / WDT needles anywhere in the raw transcript.
    hits = [needle for needle in CRASH_NEEDLES if needle in text]
    check("x.no_crash_wdt", not hits, f"needle hits={hits or 'none'}")

    # post-soak positive regression (only when the runner captured it).
    if data.regression:
        blob = "\n".join(data.regression)
        reg_ok = ("[poca-load] PASS" in blob and "MAGIC MATCH" in blob
                  and "[poca-unload] PASS" in blob and "[poca-iram] PASS" in blob
                  and "[poca-iram] final iram free=" in blob and "no leak" in blob)
        check("x.regression", reg_ok, "post-soak load/activate/unload + iram 3")

    passed = [c for c in checks if c[1]]
    failed = [c for c in checks if not c[1]]
    print(f"SOAK VERDICT: {'PASS' if not failed else 'FAIL'} "
          f"({len(passed)}/{len(checks)} criteria)")
    for name, ok, detail in checks:
        marker = "PASS" if ok else "FAIL"
        print(f"  [{marker}] {name}: {detail}")
    if "elapsed_ms" in s:
        print(f"  wall time on device: {int(s['elapsed_ms']) / 1000:.1f}s "
              f"(mix={data.header.get('mix')}, samples={len(data.samples)})")
    return not failed, [c[2] for c in checks], [f"{c[0]}: {c[2]}" for c in failed]


def write_csv(data: SoakData, path: Path) -> None:
    cols = ["idx", "cycle"] + data.sample_columns() + [f"hwm_{n}" for n in data.task_names]
    with path.open("w", encoding="ascii", newline="\n") as handle:
        handle.write(",".join(cols) + "\n")
        for row in data.samples:
            handle.write(",".join(str(row.get(c, "")) for c in cols) + "\n")
    print(f"csv written: {path} ({len(data.samples)} rows)")


class Board:
    def __init__(self, port: str, baud: int = 115200) -> None:
        import serial

        self.serial = serial.Serial(port, baud, timeout=1.0)
        self.serial.setRTS(True)
        time.sleep(0.1)
        self.serial.setRTS(False)
        time.sleep(0.1)
        self.serial.reset_input_buffer()

    def wait_prompt(self, deadline_s: float = 10.0) -> str:
        end = time.monotonic() + deadline_s
        buf = b""
        while time.monotonic() < end:
            chunk = self.serial.read(4096)
            if chunk:
                buf += chunk
                text = buf.decode(errors="replace")
                if text.rstrip().endswith(PROMPT.rstrip()):
                    return text
            else:
                self.serial.write(b"\r")
                self.serial.flush()
        return buf.decode(errors="replace")

    def command(self, line: str, settle: float = 0.6, deadline_s: float = 30.0,
                sink=None) -> str:
        self.serial.write((line + "\r").encode())
        self.serial.flush()
        time.sleep(settle)
        end = time.monotonic() + deadline_s
        buf = b""
        while time.monotonic() < end:
            chunk = self.serial.read(4096)
            if chunk:
                buf += chunk
                if sink is not None:
                    sink.write(chunk)
                    sink.flush()
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


def run_on_board(port: str, cycles: int, mix: str, raw_out: Path, idle_timeout: float,
                 total_timeout: float) -> None:
    board = Board(port)
    with raw_out.open("wb") as sink:
        def note(line: str) -> None:
            sink.write((line + "\n").encode())
            sink.flush()
            print(line)

        note(f"=== T9 soak raw capture {time.strftime('%Y-%m-%dT%H:%M:%S')} "
             f"cycles={cycles} mix={mix} port={port} ===")
        board.wait_prompt()
        board.command("poca status", sink=sink)

        note("--- SOAK-BEGIN ---")
        board.serial.write(f"poca soak {cycles} {mix}\r".encode())
        board.serial.flush()
        end_total = time.monotonic() + total_timeout
        last_line_at = time.monotonic()
        buf = b""
        done_seen = False
        while time.monotonic() < end_total:
            chunk = board.serial.read(4096)
            now = time.monotonic()
            if chunk:
                buf += chunk
                sink.write(chunk)
                sink.flush()
                sys.stdout.write(chunk.decode(errors="replace"))
                sys.stdout.flush()
                if b"\n" in chunk or b"\r" in chunk:
                    last_line_at = now
                text = buf.decode(errors="replace")
                if not done_seen and ("[poca-soak] DONE" in text
                                      or "[poca-soak] FAIL (" in text):
                    done_seen = True
                if done_seen and text.rstrip().endswith(PROMPT.rstrip()):
                    break
                if "[poca-soak] FAIL slots busy" in text:
                    break
            elif now - last_line_at > idle_timeout:
                note(f"--- SOAK-IDLE-TIMEOUT after {now - last_line_at:.0f}s "
                     f"without output ---")
                break
        note("--- SOAK-END ---")

        note("--- REGRESSION-BEGIN ---")
        board.wait_prompt()
        board.command("poca status", sink=sink)
        board.command("poca load baseline", sink=sink)
        board.command("poca activate", sink=sink)
        board.command("poca unload", sink=sink)
        board.command("poca iram 3", deadline_s=60.0, sink=sink)
        board.command("poca status", sink=sink)
        note("--- REGRESSION-END ---")
    board.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transcript", help="judge an existing raw capture")
    parser.add_argument("--port", help="run the soak on this serial port")
    parser.add_argument("--cycles", type=int, default=1000)
    parser.add_argument("--mix", default="default", choices=["default", "baseline", "iram"])
    parser.add_argument("--raw-out", type=Path, default=Path("/tmp/soak_raw.txt"))
    parser.add_argument("--csv-out", type=Path, default=None)
    parser.add_argument("--idle-timeout", type=float, default=180.0,
                        help="abort if no output line for this many seconds")
    parser.add_argument("--total-timeout", type=float, default=2700.0)
    args = parser.parse_args()

    if args.transcript:
        text = Path(args.transcript).read_text(encoding="utf-8", errors="replace")
    elif args.port:
        run_on_board(args.port, args.cycles, args.mix, args.raw_out,
                     args.idle_timeout, args.total_timeout)
        text = args.raw_out.read_text(encoding="utf-8", errors="replace")
    else:
        parser.error("need --transcript FILE or --port PORT")
        return 2

    data = parse_transcript(text)
    ok, _, _ = judge(data, text)
    if args.csv_out:
        write_csv(data, args.csv_out)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
