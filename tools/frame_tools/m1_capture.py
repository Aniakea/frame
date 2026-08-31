"""Collect M1 hardware-validation evidence from the device serial console.

The collector passively records everything on the console into ``soak.log``,
splits device JSON lines into ``jsonl.ndjson`` (telemetry) and ``status.json``
(``status --json`` responses), and writes a draft ``manifest.json`` for the M1
gate whose verdict is completed later by the soak judgment step.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import sys
import time
from collections.abc import Sequence
from dataclasses import dataclass
from datetime import UTC, datetime
from enum import Enum
from pathlib import Path
from typing import Protocol, cast

SOAK_LOG = "soak.log"
JSONL_LOG = "jsonl.ndjson"
STATUS_LOG = "status.json"
MANIFEST_LOG = "manifest.json"
STATUS_COMMAND = b"status --json\r\n"


class LineKind(Enum):
    RAW = "raw"
    JSONL = "jsonl"
    STATUS = "status"


def parse_device_json(line: str) -> dict[str, object] | None:
    """Return the decoded device JSON object, or None for plain console text."""
    text = line.strip()
    if not text.startswith("{"):
        return None
    try:
        payload = json.loads(text)
    except json.JSONDecodeError:
        return None
    return payload if isinstance(payload, dict) else None


def classify_payload(payload: dict[str, object]) -> LineKind:
    """Telemetry lines carry ``sequence``; status responses do not."""
    if payload.get("schema_version") != 1:
        return LineKind.RAW
    return LineKind.JSONL if "sequence" in payload else LineKind.STATUS


def sequence_of(payload: dict[str, object]) -> int | None:
    value = payload.get("sequence")
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value


@dataclass(slots=True)
class CaptureStats:
    jsonl_count: int = 0
    sequence_min: int | None = None
    sequence_max: int | None = None

    def record(self, payload: dict[str, object]) -> None:
        self.jsonl_count += 1
        sequence = sequence_of(payload)
        if sequence is not None:
            self.sequence_min = (
                sequence if self.sequence_min is None else min(self.sequence_min, sequence)
            )
            self.sequence_max = (
                sequence if self.sequence_max is None else max(self.sequence_max, sequence)
            )

    def as_metrics(self) -> dict[str, int]:
        metrics = {"jsonl_count": self.jsonl_count}
        if self.sequence_min is not None:
            metrics["sequence_min"] = self.sequence_min
        if self.sequence_max is not None:
            metrics["sequence_max"] = self.sequence_max
        return metrics


class SerialPort(Protocol):
    def readline(self) -> bytes: ...

    def write(self, data: bytes) -> int: ...

    def close(self) -> None: ...


class SerialFactory(Protocol):
    def __call__(self, port: str, baudrate: int, timeout: float) -> SerialPort: ...


def load_serial() -> SerialFactory:
    """Import pyserial lazily; it is an optional, capture-only dependency."""
    try:
        module = importlib.import_module("serial")
    except ImportError as error:
        raise SystemExit(
            "pyserial is required for serial capture but is not installed; "
            "install it into the active environment with `uv pip install pyserial`"
        ) from error
    # pyserial is absent from the checked-in dependency set, so bind it to a
    # Protocol at this boundary instead of importing it for type checking.
    return cast(SerialFactory, module.Serial)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _utc_now() -> str:
    return datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


class CaptureWriter:
    """Route decoded serial lines into the soak, JSONL, and status artifacts."""

    def __init__(self, out_dir: Path) -> None:
        out_dir.mkdir(parents=True, exist_ok=True)
        self._out_dir = out_dir
        self._soak = (out_dir / SOAK_LOG).open("w", encoding="utf-8")
        self._jsonl = (out_dir / JSONL_LOG).open("w", encoding="utf-8")
        self._status = (out_dir / STATUS_LOG).open("w", encoding="utf-8")
        self.stats = CaptureStats()

    def __enter__(self) -> CaptureWriter:
        return self

    def __exit__(self, exc_type: object, exc_value: object, traceback: object) -> None:
        self.close()

    def close(self) -> None:
        for stream in (self._soak, self._jsonl, self._status):
            stream.close()

    def process_line(self, line: str) -> None:
        text = line.rstrip("\r\n")
        self._soak.write(f"{text}\n")
        payload = parse_device_json(text)
        if payload is None:
            return
        match classify_payload(payload):
            case LineKind.JSONL:
                self.stats.record(payload)
                self._jsonl.write(f"{text}\n")
            case LineKind.STATUS:
                self._status.write(f"{text}\n")
            case LineKind.RAW:
                pass

    def write_manifest(self, run_id: str, started_at: str, ended_at: str) -> Path:
        """Write the collector draft manifest; the soak step completes the verdict."""
        for stream in (self._soak, self._jsonl, self._status):
            stream.flush()
        manifest = {
            "schema_version": 1,
            "gate": "M1",
            "run_id": run_id,
            "verdict": "pending",
            "started_at": started_at,
            "ended_at": ended_at,
            "metrics": self.stats.as_metrics(),
            "artifacts": [
                {"name": name, "uri": name, "sha256": _sha256(self._out_dir / name)}
                for name in (SOAK_LOG, JSONL_LOG, STATUS_LOG)
            ],
            "notes": "collector draft; verdict, source, and platform are completed later",
        }
        path = self._out_dir / MANIFEST_LOG
        path.write_text(f"{json.dumps(manifest, indent=2)}\n", encoding="utf-8")
        return path


@dataclass(frozen=True, slots=True)
class CaptureSettings:
    port: str
    baud: int
    duration: float
    out_dir: Path
    status_interval: float
    command: str | None = None
    command_interval: float = 0.0


def _pump(port: SerialPort, writer: CaptureWriter, settings: CaptureSettings) -> None:
    deadline = time.monotonic() + settings.duration
    next_status = time.monotonic() + settings.status_interval
    command_bytes = (
        settings.command.encode("ascii") + b"\r\n" if settings.command is not None else None
    )
    next_command = (
        time.monotonic() + settings.command_interval if command_bytes is not None else None
    )
    while time.monotonic() < deadline:
        if time.monotonic() >= next_status:
            port.write(STATUS_COMMAND)
            next_status += settings.status_interval
        if (
            command_bytes is not None
            and next_command is not None
            and time.monotonic() >= next_command
        ):
            port.write(command_bytes)
            next_command += settings.command_interval
        line = port.readline()
        if line:
            writer.process_line(line.decode("utf-8", errors="replace"))


def run_capture(settings: CaptureSettings, open_port: SerialFactory) -> Path:
    """Capture for ``settings.duration`` seconds and return the manifest path."""
    started_at = _utc_now()
    run_id = f"m1-{datetime.now(UTC).strftime('%Y%m%dT%H%M%SZ')}"
    port = open_port(settings.port, baudrate=settings.baud, timeout=1.0)
    try:
        with CaptureWriter(settings.out_dir) as writer:
            try:
                _pump(port, writer, settings)
            finally:
                manifest_path = writer.write_manifest(run_id, started_at, _utc_now())
        return manifest_path
    finally:
        port.close()


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Collect M1 gate evidence from the serial console")
    parser.add_argument(
        "--port", default="/dev/ttyACM0", help="serial device (default: %(default)s)"
    )
    parser.add_argument("--baud", type=int, default=115200, help="baud rate (default: %(default)s)")
    parser.add_argument(
        "--duration", type=float, required=True, help="capture length in seconds (soak mode)"
    )
    parser.add_argument(
        "--out", type=Path, required=True, help="output directory for captured artifacts"
    )
    parser.add_argument(
        "--status-interval",
        type=float,
        default=120.0,
        help="seconds between status --json polls (default: %(default)s)",
    )
    parser.add_argument(
        "--command",
        help="extra console command sent periodically during the soak (e.g. 'wifi reconnect')",
    )
    parser.add_argument(
        "--command-interval",
        type=float,
        default=0.0,
        help="seconds between --command sends; required and >0 when --command is set",
    )
    args = parser.parse_args(argv)
    if args.command is not None and args.command_interval <= 0:
        parser.error("--command requires --command-interval > 0")

    settings = CaptureSettings(
        port=args.port,
        baud=args.baud,
        duration=args.duration,
        out_dir=args.out,
        status_interval=args.status_interval,
        command=args.command,
        command_interval=args.command_interval,
    )
    try:
        manifest_path = run_capture(settings, load_serial())
    except OSError as error:
        print(f"{settings.port}: cannot open serial port: {error}", file=sys.stderr)
        return 1
    print(f"{manifest_path}: capture complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
