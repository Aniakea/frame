from pathlib import Path

from frame_tools.partitions import validate_partitions


def test_repository_partition_layout_is_frozen() -> None:
    assert validate_partitions(Path("partitions.csv")) == []


def test_changed_partition_is_rejected(tmp_path: Path) -> None:
    changed = Path("partitions.csv").read_text(encoding="utf-8").replace("0x4C0000", "0x4B0000")
    path = tmp_path / "partitions.csv"
    path.write_text(changed, encoding="utf-8")

    errors = validate_partitions(path)

    assert errors
    assert "plugin_fs" in errors[0]
