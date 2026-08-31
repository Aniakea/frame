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
`tree.sha256` recomputation.

| Patch | Task | Scope | Diff | Post-patch tree.sha256 |
| ----- | ---- | ----- | ---- | ---------------------- |
| p1 fail-closed arch relocate | T5 | `src/esp_elf.c` (1 hunk) | below | manifest sha256 `16f67a5112e08e40cda6db67873dd5ce8c5c2b82fe24580aa3a32caada3a6524` (p1+p2 only) |
| p2 ELF header validation | T5 | `src/esp_elf.c` (1 hunk) | below | (as above; both patches touch only `src/esp_elf.c`) |
| p3 true .text window size | T5 | `src/esp_elf.c` (1 hunk) | below | manifest sha256 `77a95ceccd65bcdf773d0f415f5136d17e2d9382f9b04fe005545c02d5547637`; `src/esp_elf.c` sha256 `c13652c1f2921bb516d6dc206816d20b59d21c7e2771738f4da9a4ae4040f024` (current, p1+p2+p3) |

### p1 — fail-closed relocation (T5)

**Rationale:** upstream `esp_elf_relocate()` called
`esp_elf_arch_relocate()` and **ignored its return value**; the arch layer
returns `-EINVAL` for every relocation type outside
{RELATIVE, RTLD, GLOB_DAT, JMP_SLOT}, but the load only logged
`elf_arch: info=N is not supported` and continued with unrelocated words
(FAIL-OPEN, verified during T4). TEST-003 requires "unknown relocation
types are rejected before any plugin code executes", so the error must
propagate. Cleanup mirrors the existing `-ENOSYS` failure path in the same
loop (no internal free; the caller releases via `esp_elf_deinit()`, which
the poc_a harness does on every load failure).

```diff
@@ -620,7 +620,17 @@ int esp_elf_relocate(esp_elf_t *elf, const uint8_t *pbuf)
                     ESP_LOGD(TAG, "Find function %s addr=%x", func_name, addr);
                 }
 
-                esp_elf_arch_relocate(elf, &rela_buf, sym, addr);
+                /* [patch p1] Fail closed on unsupported relocations: upstream
+                 * ignored esp_elf_arch_relocate()'s -EINVAL, so out-of-allowlist
+                 * relocation types only logged an error and loading continued
+                 * with unrelocated words. Abort the load instead (TEST-003:
+                 * unknown types are rejected before any plugin code runs). */
+                ret = esp_elf_arch_relocate(elf, &rela_buf, sym, addr);
+                if (ret) {
+                    ESP_LOGE(TAG, "Failed to relocate type=%d offset=0x%x ret=%d",
+                             ELF_R_TYPE(rela_buf.info), rela_buf.offset, ret);
+                    return ret;
+                }
             }
 #if CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
         } else {
```

### p2 — ELF header validation before parse/copy (T5)

**Rationale:** upstream `esp_elf_relocate()` cast the payload to
`elf32_hdr_t` and started parsing with **no** validation of `e_ident`
magic, EI_CLASS, EI_DATA or `e_machine`. On ESP32-S3 (BUS_ADDRESS_MIRROR
section path) a big-endian-marked, ELF64-marked or non-Xtensa payload is
indistinguishable from a valid image at the byte level the loader reads
(LE field reads are unaffected by the EI_DATA flag), so such images were
**loaded and executed** (verified by construction in T5: the build-time
gates and the manifest builder's `extract_plugin_iram_size()` both reject
these images, but a hand-crafted signed container reaches the loader with
them). Minimal fail-closed gate before any header-driven access. This is
also the loader-side half of T14's 14th tamper class (ELF class/machine).

```diff
@@ -531,6 +531,23 @@ int esp_elf_relocate(esp_elf_t *elf, const uint8_t *pbuf)
     }
 
     ehdr    = (const elf32_hdr_t *)pbuf;
+
+    /* [patch p2] Fail closed on malformed or foreign-architecture images
+     * before any header-driven parsing or segment copy: ELF magic,
+     * ELFCLASS32, little-endian data encoding and an Xtensa machine type
+     * are required. Without this check the loader happily interprets
+     * arbitrary bytes as section/program headers (verified in T5). */
+    if (ehdr->ident[0] != 0x7f || ehdr->ident[1] != 'E' ||
+            ehdr->ident[2] != 'L' || ehdr->ident[3] != 'F' ||
+            ehdr->ident[4] != 1 /* ELFCLASS32 */ ||
+            ehdr->ident[5] != 1 /* ELFDATA2LSB */ ||
+            ehdr->machine != 94 /* EM_XTENSA */) {
+        ESP_LOGE(TAG, "Invalid ELF image: magic/class/data/machine mismatch "
+                 "(class=%u data=%u machine=%u)",
+                 ehdr->ident[4], ehdr->ident[5], ehdr->machine);
+        return -EINVAL;
+    }
+
     shdr    = (const elf32_shdr_t *)(pbuf + ehdr->shoff);
     shstrab = (const char *)pbuf + shdr[ehdr->shstrndx].offset;
```

### p3 — record true .text window size (T5)

**Rationale:** upstream recorded `sec[TEXT].size = ELF_ALIGN(shdr.size, 4)`.
That recorded size is also the `esp_elf_map_sym()` / `elf_remap_text()`
window. Xtensa places `.rodata` (alignment 1) directly after `.text`, so
when the true `.text` size is not a multiple of 4 the rounded-up window
swallows the first vaddr(s) of `.rodata`: a data pointer with such a vaddr
is mapped into the `.text` window and then mirrored into the instruction
alias (`+0x06000000`), and the first data load through it takes a
LoadStoreError. Observed on target with the T5 `globdat` probe (`.text`
size 0xd3 → `.rodata` vaddr 0x2ab inside the aligned window [0x1d8,
0x2ac)); T4's baseline survived only because its `.text` size happened to
be 4-aligned. Every other recorded section already uses the true size;
allocation is unaffected (the allocator returns aligned blocks regardless
of requested size). Note for T7: `.plugin_iram` handling must copy this
pattern (true size, never a rounded window).

```diff
@@ -190,7 +190,7 @@ static int esp_elf_load_section(esp_elf_t *elf, const uint8_t
                 elf->sec[ELF_SEC_TEXT].v_addr  = shdr[i].addr;
-                elf->sec[ELF_SEC_TEXT].size    = ELF_ALIGN(shdr[i].size, 4);
+                elf->sec[ELF_SEC_TEXT].size    = shdr[i].size;
                 elf->sec[ELF_SEC_TEXT].offset  = shdr[i].offset;
```

(the hunk carries the `[patch p3]` comment block verbatim from the source)

Upstream files are NEVER edited in place without a registry entry; the
unpatched state must always be reproducible from this table (apply the
hunks above in reverse to `git show 6526c5b1:components/elf_loader/src/esp_elf.c`).
