"""Enforce LLVM branch coverage for the shared Frame contract headers."""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Sequence
from pathlib import Path


def branch_percentage(report: Path) -> float:
    data = json.loads(report.read_text(encoding="utf-8"))
    try:
        return float(data["data"][0]["totals"]["branches"]["percent"])
    except (KeyError, IndexError, TypeError, ValueError) as error:
        raise ValueError(f"{report}: invalid llvm-cov export") from error


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check Frame LLVM branch coverage")
    parser.add_argument("report", type=Path)
    parser.add_argument("--minimum", type=float, default=90.0)
    args = parser.parse_args(argv)

    try:
        percentage = branch_percentage(args.report)
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    if percentage < args.minimum:
        print(
            f"branch coverage {percentage:.2f}% is below {args.minimum:.2f}%",
            file=sys.stderr,
        )
        return 1
    print(f"branch coverage {percentage:.2f}% meets {args.minimum:.2f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
