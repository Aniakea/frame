"""Validate PoC evidence manifests against the repository JSON Schema."""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from jsonschema import FormatChecker, SchemaError
from jsonschema.validators import validator_for


def _load_json(path: Path) -> Any:
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def _json_path(parts: Sequence[object]) -> str:
    return "$" + "".join(f"[{part!r}]" for part in parts)


def validate_evidence(schema_path: Path, manifest_paths: Sequence[Path]) -> list[str]:
    try:
        schema = _load_json(schema_path)
        validator_class = validator_for(schema)
        validator_class.check_schema(schema)
        validator = validator_class(schema, format_checker=FormatChecker())
    except (OSError, UnicodeError, json.JSONDecodeError, SchemaError) as error:
        return [f"{schema_path}: invalid evidence schema: {error}"]

    errors: list[str] = []
    for path in manifest_paths:
        try:
            manifest = _load_json(path)
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            errors.append(f"{path}: invalid JSON: {error}")
            continue

        validation_errors = sorted(
            validator.iter_errors(manifest), key=lambda item: _json_path(list(item.path))
        )
        errors.extend(
            f"{path}:{_json_path(list(error.path))}: {error.message}" for error in validation_errors
        )
    return errors


def discover_manifests(inputs: Sequence[Path], schema_path: Path) -> tuple[list[Path], list[str]]:
    candidates: list[Path] = []
    errors: list[str] = []
    sources = list(inputs) if inputs else [Path("poc/evidence")]
    schema_resolved = schema_path.resolve(strict=False)

    for source in sources:
        if source.is_dir():
            candidates.extend(sorted(source.rglob("*.json")))
        elif source.is_file():
            candidates.append(source)
        else:
            errors.append(f"{source}: evidence path does not exist")

    manifests = sorted(
        {path for path in candidates if path.resolve(strict=False) != schema_resolved},
        key=lambda path: str(path),
    )
    return manifests, errors


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate Frame evidence manifests")
    parser.add_argument("paths", nargs="*", type=Path, help="manifest files or directories")
    parser.add_argument(
        "--schema",
        type=Path,
        default=Path("poc/evidence/schema.json"),
        help="evidence JSON Schema",
    )
    args = parser.parse_args(argv)

    manifests, errors = discover_manifests(args.paths, args.schema)
    errors.extend(validate_evidence(args.schema, manifests))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"validated evidence schema and {len(manifests)} manifest(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
