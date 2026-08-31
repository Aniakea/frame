# Frame

[![CI](https://github.com/Aniakea/frame/actions/workflows/ci.yml/badge.svg)](https://github.com/Aniakea/frame/actions/workflows/ci.yml)
[![Nightly](https://github.com/Aniakea/frame/actions/workflows/nightly.yml/badge.svg)](https://github.com/Aniakea/frame/actions/workflows/nightly.yml)
[![CodeQL](https://github.com/Aniakea/frame/actions/workflows/codeql.yml/badge.svg)](https://github.com/Aniakea/frame/actions/workflows/codeql.yml)

> **Draft / PoC:** the v4.2 architecture and hardware gates are not signed off. Green cloud CI
> does not mean M1, PoC-A through PoC-E, power-cut durability, or production security has passed.

Frame is an ESP32-S3 firmware project exploring an embedded plugin architecture. The repository
contains the product firmware skeleton, isolated architecture proofs of concept, host-side contract
tests, and repository validation tools.

The firmware currently targets the Waveshare ESP32-S3-RLCD-4.2 (N16R8) with ESP-IDF `v6.0.2`
and requires a toolchain that provides C++26 mode. The custom 16 MiB flash layout contains two
OTA slots, core-dump storage, encrypted NVS, internal plugin storage, and system storage. Signed
plugin resources live on a dedicated FAT32 TF card; dynamic plugins do not start without it.

## Repository Layout

- `main/` contains the firmware entry point.
- `components/` contains product firmware components.
- `poc/` contains isolated architecture proofs of concept and host contract tests.
- `plugins/` is reserved for plugin build inputs; installed code is content-addressed at runtime.
- `tools/frame_tools/` contains local repository gate commands.

## Firmware Build

Install CMake 3.25 or newer and Ninja, then export an ESP-IDF checkout at the exact `v6.0.2` tag.
The checked-in presets target `esp32s3` and keep generated configuration under `.build/`.

```sh
. "$IDF_PATH/export.sh"
cmake --preset dev
cmake --build --preset dev
```

Use the production defaults with:

```sh
. "$IDF_PATH/export.sh"
cmake --preset prod
cmake --build --preset prod
```

Production builds are build artifacts, not release attestations. Hardware validation and signing
remain separate release concerns.

## Host Tests

The host project is a plain CMake/CTest suite. It does not build ESP32 firmware.

```sh
cmake -S poc/host -B .build/poc-host -DENABLE_SANITIZERS=ON
cmake --build .build/poc-host
ctest --test-dir .build/poc-host --output-on-failure
```

## Repository Gates

Python 3.11 or newer and [uv](https://docs.astral.sh/uv/) are required.

```sh
uv sync --frozen
uv run ruff format --check tools tests
uv run ruff check tools tests
uv run pyright
uv run pytest
uv run frame-partitions
uv run frame-evidence
uv run frame-doc-links
uv run frame-sbom --output .build/reports/sbom.cdx.json
```

CI also runs an independent ThreadSanitizer build and enforces at least 90% LLVM branch coverage
for the shared ABI, handle, and lifecycle contract headers.

Evidence manifests placed in `poc/evidence/` are validated against the existing
`poc/evidence/schema.json`. The schema itself is always checked, including when no manifests have
been collected yet.

## Contributing And Security

Read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting a change. Report security issues through
GitHub private vulnerability reporting as described in [SECURITY.md](SECURITY.md). Community
participation is governed by [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## License

Except where a file or third-party component states otherwise, this project is licensed under the
GNU General Public License, version 3 or later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
