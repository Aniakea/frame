# Contributing To Frame

Contributions are welcome through GitHub issues and pull requests. Keep changes focused and do not
combine unrelated refactors with behavioral changes.

## Before Opening A Change

1. Search existing issues and pull requests for related work.
2. For architecture or hardware changes, open an issue before investing in a large implementation.
3. Do not include credentials, signing keys, device secrets, private evidence, or proprietary
   firmware in an issue or pull request.
4. Follow the [Code of Conduct](CODE_OF_CONDUCT.md).

Security vulnerabilities must not be filed as public issues. Follow [SECURITY.md](SECURITY.md).

## Pull Requests

Use a title in this form:

```text
<type>(optional-scope): concise summary
```

Allowed types are `build`, `chore`, `ci`, `docs`, `feat`, `fix`, `perf`, `refactor`, `revert`,
`style`, and `test`. Breaking changes may add `!`, for example `feat(runtime)!: change plugin ABI`.

Describe the motivation, behavior change, validation performed, and any hardware dependencies.
Update tests and documentation when behavior changes. A pull request should be reviewable as a
single coherent change.

## Local Validation

Install Python 3.11 or newer and uv, then run:

```sh
uv sync --frozen
uv run ruff format --check tools tests
uv run ruff check tools tests
uv run pyright
uv run pytest
uv run frame-partitions
uv run frame-evidence
uv run frame-doc-links
```

For host-contract changes, run both a sanitizer build and a GCC compatibility build:

```sh
CC=clang CXX=clang++ cmake -S poc/host -B .build/host-clang -DENABLE_SANITIZERS=ON
cmake --build .build/host-clang
ctest --test-dir .build/host-clang --output-on-failure

CC=gcc CXX=g++ cmake -S poc/host -B .build/host-gcc
cmake --build .build/host-gcc
ctest --test-dir .build/host-gcc --output-on-failure

CC=clang CXX=clang++ cmake -S poc/host -B .build/host-tsan -DENABLE_TSAN=ON
cmake --build .build/host-tsan
ctest --test-dir .build/host-tsan --output-on-failure
```

For product firmware changes, build the affected preset using the exact ESP-IDF `v6.0.2`
environment. CI builds `dev`; the scheduled workflow builds both `dev` and `prod`.

## Evidence

Architecture-gate evidence must conform to `poc/evidence/schema.json`. Evidence must identify its
source commit and platform and must carry digests for referenced artifacts. Do not mark a gate as
passing without the required hardware run and retained artifacts.

## Licensing

By submitting a contribution, you agree that it may be distributed under this repository's
GPL-3.0-or-later license. Identify any third-party material and preserve its applicable notices and
license terms.
