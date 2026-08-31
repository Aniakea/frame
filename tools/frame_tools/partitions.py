"""Validate the frozen 16 MiB Frame partition layout."""

from __future__ import annotations

import argparse
import csv
import sys
from collections.abc import Sequence
from pathlib import Path

EXPECTED_HEADER = ("# Name", "Type", "SubType", "Offset", "Size", "Flags")
EXPECTED_PARTITIONS = (
    ("nvs", "data", "nvs", "0x9000", "0x14000", ""),
    ("nvs_keys", "data", "nvs_keys", "0x1D000", "0x1000", "encrypted"),
    ("otadata", "data", "ota", "0x1E000", "0x2000", ""),
    ("coredump", "data", "coredump", "0x20000", "0x20000", ""),
    ("ota_0", "app", "ota_0", "0x40000", "0x500000", ""),
    ("ota_1", "app", "ota_1", "0x540000", "0x500000", ""),
    ("plugin_fs", "data", "littlefs", "0xA40000", "0x4C0000", ""),
    ("system_fs", "data", "littlefs", "0xF00000", "0x100000", ""),
)


def read_layout(path: Path) -> tuple[tuple[str, ...], ...]:
    with path.open(encoding="utf-8", newline="") as stream:
        rows = csv.reader(stream, skipinitialspace=True)
        return tuple(
            tuple(cell.strip() for cell in row) for row in rows if any(cell.strip() for cell in row)
        )


def validate_partitions(path: Path) -> list[str]:
    try:
        rows = read_layout(path)
    except (OSError, UnicodeError, csv.Error) as error:
        return [f"{path}: cannot read partition CSV: {error}"]

    expected = (EXPECTED_HEADER, *EXPECTED_PARTITIONS)
    if rows == expected:
        return []

    errors: list[str] = []
    if not rows:
        return [f"{path}: partition CSV is empty"]
    if rows[0] != EXPECTED_HEADER:
        errors.append(f"{path}: header changed: expected {EXPECTED_HEADER!r}, got {rows[0]!r}")

    actual_partitions = rows[1:]
    for index in range(max(len(actual_partitions), len(EXPECTED_PARTITIONS))):
        expected_row = EXPECTED_PARTITIONS[index] if index < len(EXPECTED_PARTITIONS) else None
        actual_row = actual_partitions[index] if index < len(actual_partitions) else None
        if actual_row != expected_row:
            errors.append(
                f"{path}: partition row {index + 1} changed: "
                f"expected {expected_row!r}, got {actual_row!r}"
            )
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate the frozen Frame partition table")
    parser.add_argument("path", nargs="?", type=Path, default=Path("partitions.csv"))
    args = parser.parse_args(argv)

    errors = validate_partitions(args.path)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"{args.path}: partition layout matches the frozen layout")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
