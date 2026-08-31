# PATCHES.md — vendored elf_loader provenance and patch registry

## Upstream source

- Repository: https://github.com/espressif/esp-iot-solution
- Path: `components/elf_loader`
- Commit: `6526c5b18e156cfbda2c7ce48e282e384c43b485` (short `6526c5b1`)
  "fix(elf_loader): Use full cache flush instead of ranged API on ESP32-S31"
- Component version at that commit: `1.3.3` (per `idf_component.yml`)
- Fetch date: 2026-08-31 (partial clone `--filter=blob:none`, exact-commit
  checkout of `components/elf_loader`; commit identity verified via the GitHub
  commits API before checkout)
- Files vendored: 26 (all component files, verbatim — see verification below)

## License / NOTICE

Upstream license: **Apache License 2.0**. The full license text is vendored
verbatim as [`license.txt`](license.txt) in this directory
(sha256 `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`,
the canonical Apache-2.0 text). Every upstream source file carries the header:

> SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
>
> SPDX-License-Identifier: Apache-2.0

NOTICE: This directory contains software licensed under the Apache License,
Version 2.0, copyright Espressif Systems (Shanghai) CO LTD. It is vendored
into this repository (whose own license is GPLv3-or-later; see root LICENSE
and NOTICE) for the PoC-A dynamic-ELF proof of concept. The upstream Apache-2.0
text and copyright headers are retained in full; Apache-2.0 compatibility with
GPLv3 is documented in the GPL "License Compatibility" appendix.

## Verification

The vendored tree is byte-identical to upstream at the pinned commit:

```sh
# offline self-consistency (sufficient per plan fallback rule):
cd poc/apps/poc_a/components/elf_loader
find . -type f ! -name 'tree.sha256' ! -name 'PATCHES.md' | sort | xargs sha256sum | diff - tree.sha256

# online (enhancement): compare against the pinned commit
git clone --filter=blob:none https://github.com/espressif/esp-iot-solution.git
cd esp-iot-solution && git checkout 6526c5b1 -- components/elf_loader
diff -r components/elf_loader <this directory excluding PATCHES.md/tree.sha256>
```

`tree.sha256` (committed alongside) is the sorted sha256 manifest of the 26
upstream files only; `PATCHES.md` and `tree.sha256` itself are excluded so the
manifest stays recomputable. Manifest-file sha256:
`41874265d531ed6d95a39daca006251f3d3fa3b6ee2af220afd846e4787a8861`.

## Patch registry

All local modifications to the vendored tree MUST be registered here as
numbered patches (`p1`, `p2`, ...) with motivation, diff, and post-patch
`tree.sha256` recomputation. The tree as vendored carries **no patches**.

| Patch | Task | Scope | Diff | Post-patch tree.sha256 |
| ----- | ---- | ----- | ---- | ---------------------- |
| (none yet — T7 will add p1: `.plugin_iram` section support) | | | | |

Upstream files are NEVER edited in place without a registry entry; the
unpatched state must always be reproducible from this table.
