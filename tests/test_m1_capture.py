import hashlib
import importlib.util
import json
from pathlib import Path

import pytest
from frame_tools.m1_capture import (
    CaptureSettings,
    CaptureWriter,
    LineKind,
    classify_payload,
    main,
    parse_device_json,
    run_capture,
    sequence_of,
)

BOOT_LINE = "Frame M1 0.1.0-dev.1 boot complete"
JSONL_LINE = (
    '{"schema_version":1,"sequence":7,"uptime_us":1000,"source":"m1","event":"status",'
    '"mode":"idle","crc32c":"deadbeef"}'
)
STATUS_LINE = '{"schema_version":1,"firmware":"0.1.0-dev.1","display":"OK","partitions_ok":true}'


def test_parse_device_json_returns_none_for_plain_text() -> None:
    assert parse_device_json(BOOT_LINE) is None
    assert parse_device_json("{not json") is None


def test_parse_device_json_decodes_device_lines() -> None:
    payload = parse_device_json(JSONL_LINE)

    assert payload is not None
    assert payload["schema_version"] == 1


def test_classify_payload_distinguishes_jsonl_from_status() -> None:
    jsonl = parse_device_json(JSONL_LINE)
    status = parse_device_json(STATUS_LINE)

    assert jsonl is not None and classify_payload(jsonl) is LineKind.JSONL
    assert status is not None and classify_payload(status) is LineKind.STATUS
    assert classify_payload({"schema_version": 2}) is LineKind.RAW
    assert classify_payload({"other": 1}) is LineKind.RAW


def test_sequence_of_rejects_non_integers() -> None:
    assert sequence_of({"sequence": 12}) == 12
    assert sequence_of({}) is None
    assert sequence_of({"sequence": "12"}) is None
    assert sequence_of({"sequence": True}) is None


def test_capture_writer_routes_lines(tmp_path: Path) -> None:
    with CaptureWriter(tmp_path) as writer:
        writer.process_line(f"{BOOT_LINE}\r\n")
        writer.process_line(JSONL_LINE)
        writer.process_line(STATUS_LINE)

    assert (tmp_path / "soak.log").read_text(encoding="utf-8").splitlines() == [
        BOOT_LINE,
        JSONL_LINE,
        STATUS_LINE,
    ]
    assert (tmp_path / "jsonl.ndjson").read_text(encoding="utf-8").splitlines() == [JSONL_LINE]
    assert (tmp_path / "status.json").read_text(encoding="utf-8").splitlines() == [STATUS_LINE]
    assert writer.stats.jsonl_count == 1
    assert writer.stats.sequence_min == 7
    assert writer.stats.sequence_max == 7


def test_capture_writer_tracks_sequence_bounds(tmp_path: Path) -> None:
    with CaptureWriter(tmp_path) as writer:
        for sequence in (5, 9, 3):
            writer.process_line(f'{{"schema_version":1,"sequence":{sequence}}}')

    assert writer.stats.jsonl_count == 3
    assert writer.stats.sequence_min == 3
    assert writer.stats.sequence_max == 9
    assert writer.stats.as_metrics() == {
        "jsonl_count": 3,
        "sequence_min": 3,
        "sequence_max": 9,
    }


def test_write_manifest_records_gate_and_stats(tmp_path: Path) -> None:
    with CaptureWriter(tmp_path) as writer:
        writer.process_line(BOOT_LINE)
        writer.process_line(JSONL_LINE)
        writer.process_line(STATUS_LINE)
        manifest_path = writer.write_manifest(
            "m1-test", "2026-08-30T00:00:00Z", "2026-08-30T01:00:00Z"
        )

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["gate"] == "M1"
    assert manifest["verdict"] == "pending"
    assert manifest["run_id"] == "m1-test"
    assert manifest["started_at"] == "2026-08-30T00:00:00Z"
    assert manifest["ended_at"] == "2026-08-30T01:00:00Z"
    assert manifest["metrics"] == {"jsonl_count": 1, "sequence_min": 7, "sequence_max": 7}
    artifacts = {entry["name"]: entry for entry in manifest["artifacts"]}
    assert set(artifacts) == {"soak.log", "jsonl.ndjson", "status.json"}
    for name, entry in artifacts.items():
        digest = hashlib.sha256((tmp_path / name).read_bytes()).hexdigest()
        assert entry["sha256"] == digest
        assert entry["uri"] == name


class FakeSerialPort:
    def __init__(self, lines: list[bytes]) -> None:
        self._lines = list(lines)
        self.written: list[bytes] = []
        self.closed = False

    def readline(self) -> bytes:
        return self._lines.pop(0) if self._lines else b""

    def write(self, data: bytes) -> int:
        self.written.append(data)
        return len(data)

    def close(self) -> None:
        self.closed = True


def test_run_capture_collects_artifacts_and_polls_status(tmp_path: Path) -> None:
    port_obj = FakeSerialPort(
        [f"{BOOT_LINE}\r\n".encode(), JSONL_LINE.encode(), STATUS_LINE.encode()]
    )
    settings = CaptureSettings(
        port="/dev/fake",
        baud=115200,
        duration=0.05,
        out_dir=tmp_path / "run",
        status_interval=0.01,
    )

    def open_port(port: str, baudrate: int, timeout: float) -> FakeSerialPort:
        assert port == "/dev/fake"
        assert baudrate == 115200
        return port_obj

    manifest_path = run_capture(settings, open_port)

    assert manifest_path == tmp_path / "run" / "manifest.json"
    assert port_obj.written.count(b"status --json\r\n") >= 1
    assert port_obj.closed
    assert (tmp_path / "run" / "jsonl.ndjson").read_text(encoding="utf-8") == f"{JSONL_LINE}\n"
    assert (tmp_path / "run" / "status.json").read_text(encoding="utf-8") == f"{STATUS_LINE}\n"


def test_run_capture_sends_periodic_command(tmp_path: Path) -> None:
    port_obj = FakeSerialPort([])
    settings = CaptureSettings(
        port="/dev/fake",
        baud=115200,
        duration=0.05,
        out_dir=tmp_path / "run",
        status_interval=0.01,
        command="wifi reconnect",
        command_interval=0.01,
    )

    def open_port(port: str, baudrate: int, timeout: float) -> FakeSerialPort:
        return port_obj

    run_capture(settings, open_port)

    assert port_obj.written.count(b"wifi reconnect\r\n") >= 1


def test_run_capture_without_command_sends_only_status(tmp_path: Path) -> None:
    port_obj = FakeSerialPort([])
    settings = CaptureSettings(
        port="/dev/fake",
        baud=115200,
        duration=0.05,
        out_dir=tmp_path / "run",
        status_interval=0.01,
    )

    def open_port(port: str, baudrate: int, timeout: float) -> FakeSerialPort:
        return port_obj

    run_capture(settings, open_port)

    assert port_obj.written
    assert all(chunk == b"status --json\r\n" for chunk in port_obj.written)


def test_main_requires_command_interval_with_command(tmp_path: Path) -> None:
    with pytest.raises(SystemExit) as excinfo:
        main(["--duration", "1", "--out", str(tmp_path), "--command", "wifi reconnect"])
    assert excinfo.value.code == 2


def test_main_requires_duration_and_out(tmp_path: Path) -> None:
    with pytest.raises(SystemExit) as excinfo:
        main(["--out", str(tmp_path)])
    assert excinfo.value.code == 2

    with pytest.raises(SystemExit) as excinfo:
        main(["--duration", "1"])
    assert excinfo.value.code == 2


def test_main_reports_missing_pyserial(tmp_path: Path) -> None:
    if importlib.util.find_spec("serial") is not None:
        pytest.skip("pyserial is installed; the missing-dependency path cannot be exercised")
    with pytest.raises(SystemExit, match="pyserial is required"):
        main(["--duration", "1", "--out", str(tmp_path)])


def test_capture_writer_persists_lines_written_before_error(tmp_path: Path) -> None:
    class Boom(Exception):
        pass

    with pytest.raises(Boom), CaptureWriter(tmp_path) as writer:
        writer.process_line(BOOT_LINE)
        raise Boom

    assert (tmp_path / "soak.log").read_text(encoding="utf-8") == f"{BOOT_LINE}\n"
