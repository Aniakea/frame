import json
from pathlib import Path

from frame_tools.evidence import discover_manifests, validate_evidence

SCHEMA = Path("poc/evidence/schema.json")


def valid_manifest() -> dict[str, object]:
    return {
        "schema_version": 1,
        "gate": "A",
        "run_id": "test-run",
        "verdict": "BLOCKED",
        "started_at": "2026-08-30T00:00:00Z",
        "finished_at": "2026-08-30T00:01:00Z",
        "source": {
            "repository_commit": "0" * 40,
            "dirty": False,
            "project_document_sha256": "0" * 64,
        },
        "platform": {
            "idf_tag": "v6.0.2",
            "compiler": "test compiler",
            "target": "esp32s3",
            "sdkconfig_sha256": "0" * 64,
        },
        "artifacts": [{"name": "log", "uri": "log.txt", "sha256": "0" * 64}],
    }


def test_valid_manifest_passes(tmp_path: Path) -> None:
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(valid_manifest()), encoding="utf-8")

    assert validate_evidence(SCHEMA, [path]) == []


def test_m1_gate_manifest_passes(tmp_path: Path) -> None:
    manifest = valid_manifest()
    manifest["gate"] = "M1"
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")

    assert validate_evidence(SCHEMA, [path]) == []


def test_invalid_manifest_reports_json_path(tmp_path: Path) -> None:
    manifest = valid_manifest()
    manifest["gate"] = "F"
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")

    errors = validate_evidence(SCHEMA, [path])

    assert errors
    assert "['gate']" in errors[0]


def test_invalid_date_time_is_rejected(tmp_path: Path) -> None:
    manifest = valid_manifest()
    manifest["started_at"] = "not-a-date"
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")

    errors = validate_evidence(SCHEMA, [path])

    assert errors
    assert "['started_at']" in errors[0]


def test_schema_is_not_discovered_as_a_manifest() -> None:
    manifests, errors = discover_manifests([SCHEMA.parent], SCHEMA)

    assert errors == []
    assert SCHEMA not in manifests
