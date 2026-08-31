"""Generate a deterministic CycloneDX inventory from Frame lock files."""

from __future__ import annotations

import argparse
import json
import sys
import tomllib
from collections.abc import Sequence
from pathlib import Path

import yaml


def _component(
    name: str, version: str, ecosystem: str, digest: str | None = None
) -> dict[str, object]:
    item: dict[str, object] = {
        "type": "library",
        "bom-ref": f"pkg:{ecosystem}/{name}@{version}",
        "name": name,
        "version": version,
        "purl": f"pkg:{ecosystem}/{name}@{version}",
    }
    if digest is not None:
        item["hashes"] = [{"alg": "SHA-256", "content": digest}]
    return item


def locked_components(root: Path) -> list[dict[str, object]]:
    components: dict[str, dict[str, object]] = {}
    uv_data = tomllib.loads((root / "uv.lock").read_text(encoding="utf-8"))
    for package in uv_data.get("package", []):
        name = package.get("name")
        version = package.get("version")
        if not isinstance(name, str) or not isinstance(version, str) or name == "frame-tools":
            continue
        item = _component(name, version, "pypi")
        components[str(item["bom-ref"])] = item

    lock_files = [root / "dependencies.lock", root / "poc/apps/m1_hardware/dependencies.lock"]
    for path in lock_files:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
        for name, package in data.get("dependencies", {}).items():
            if name == "idf":
                continue
            version = str(package["version"])
            digest = package.get("component_hash")
            item = _component(name, version, "espressif", digest)
            components[str(item["bom-ref"])] = item

    idf = _component("esp-idf", "6.0.2", "github/espressif")
    components[str(idf["bom-ref"])] = idf
    googletest = _component(
        "google/googletest", "52eb8108c5bdec04579160ae17225d66034bd723", "github"
    )
    components[str(googletest["bom-ref"])] = googletest
    return [components[key] for key in sorted(components)]


def generate_sbom(root: Path) -> dict[str, object]:
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": "urn:uuid:ba788506-18cf-5a4e-ae35-38d141e6db90",
        "version": 1,
        "metadata": {
            "component": {
                "type": "application",
                "bom-ref": "pkg:github/Aniakea/frame",
                "name": "Frame",
                "version": "0.1.0-dev.1",
            }
        },
        "components": locked_components(root),
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate the Frame CycloneDX SBOM")
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)

    try:
        output = json.dumps(generate_sbom(args.root.resolve()), indent=2, sort_keys=True) + "\n"
    except (
        OSError,
        UnicodeError,
        ValueError,
        KeyError,
        TypeError,
        tomllib.TOMLDecodeError,
    ) as error:
        print(error, file=sys.stderr)
        return 1
    if args.output is None:
        sys.stdout.write(output)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
