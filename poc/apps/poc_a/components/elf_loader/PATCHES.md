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
| p4 forbidden C++ feature sections | T6 | `src/esp_elf.c` (1 hunk) | below | manifest sha256 `1602131050e4667d48b4bee7ec3a80464f091af5eaaaf72b7ff7ffe96999f336`; `src/esp_elf.c` sha256 `63162dbce031de712fd06f449a4f0b05a697c30354329cf5845490205438711d` (current, p1..p4) |
| p5 `.plugin_iram` IRAM section | T7 | `include/private/elf_types.h`, `include/private/elf_platform.h`, `src/esp_elf_adapter.c`, `src/esp_elf.c` | below | manifest sha256 `3c7706cbec751d5dce06bd733d1b3452b5be712e9123b02f413e666936b49a81`; per-file sha256 (current, p1..p5): `src/esp_elf.c` `e3aecd2dccd963440b28cee811158e917825bd59683898df0815014c529df40c`, `src/esp_elf_adapter.c` `76bc197b51844110e73962c4a0be98965a0ed9028f4a27725185eabb332dcb2c`, `include/private/elf_types.h` `b1e946cbb34dad91aa8913724022577154806ad32291d1ed4888e579d2897292`, `include/private/elf_platform.h` `4dcdcb6a11aaf5156ea978019acc484f687234bedaa32fb2fececc652cac3cfe` |

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


### p4 — forbidden C++ feature sections (T6)

**Rationale:** upstream `esp_elf_relocate()` parses the section table but
silently ignores every section name it does not know. A signed container
carrying C++ runtime feature sections therefore reaches the copy/relocation
path unchallenged (appendix C 11.2 default-forbidden features; the PoC-A
loader has no runner for static constructors/destructors, no TLS block and
no unwinder). Consequences observed on target **before** this patch (T6
pre-patch board run, signed `cxx_ctor` probe with a `.ctors` section whose
only dynamic relocations are allowlisted `R_XTENSA_RELATIVE`):

- the `.ctors` function-pointer relocation makes
  `esp_elf_arch_relocate()` write through `esp_elf_map_sym(0x12c4) == 0`
  (the section was never loaded, so no recorded window covers the offset)
  -> `Guru Meditation LoadProhibited, EXCVADDR=0x0`, firmware reboot -
  a fail-open crash, not the TEST-003-required fail-before-execute
  rejection;
- the build-time gate (check_plugin_elf.cmake gate 4) and the manifest
  builder never see the artifact because a hostile producer bypasses both
  (the T5 `hostile_signed` lesson: loader-side checks are the last layer).

The patch scans section names (prefix match, so numbered subsections like
`.init_array.00001` stay covered) immediately after the `shstrab` pointer
is computed - before any allocation or copy - and returns `-EINVAL`. The
list mirrors gate 4 exactly: `.init_array/.fini_array/.preinit_array/
.ctors/.dtors/.tdata/.tbss/.eh_frame/.eh_frame_hdr/.gcc_except_table`.
TLS relocation types remain independently fatal through patch p1 (proven
on board by the `neg_tls_nosect` row, whose sections were renamed away).

```diff
@@ -562,6 +562,34 @@ int esp_elf_relocate(esp_elf_t *elf, const uint8_t *pbuf)
     shdr    = (const elf32_shdr_t *)(pbuf + ehdr->shoff);
     shstrab = (const char *)pbuf + shdr[ehdr->shstrndx].offset;
 
+    /* [patch p4] Reject C++ runtime feature sections before any section is
+     * allocated, copied or relocated (appendix C 11.2 default-forbidden
+     * features; task T6). The entry-point model has no runner for static
+     * constructors/destructors (.init_array/.fini_array, spelled .ctors/
+     * .dtors by the xtensa toolchain), no TLS block support (.tdata/.tbss)
+     * and no unwinder (.eh_frame/.eh_frame_hdr/.gcc_except_table).
+     * Upstream silently ignored these sections, and their relocations then
+     * write through esp_elf_map_sym()==0 (observed: LoadProhibited crash
+     * on a signed .ctors probe, T6 pre-patch board run) - fail closed. */
+    {
+        static const char *const forbidden[] = {
+            ".init_array", ".fini_array", ".preinit_array",
+            ".ctors", ".dtors",
+            ".tdata", ".tbss",
+            ".eh_frame", ".eh_frame_hdr", ".gcc_except_table",
+        };
+        for (uint32_t i = 0; i < ehdr->shnum; i++) {
+            const char *name = shstrab + shdr[i].name;
+            for (size_t f = 0; f < sizeof(forbidden) / sizeof(forbidden[0]); f++) {
+                if (strncmp(name, forbidden[f], strlen(forbidden[f])) == 0) {
+                    ESP_LOGE(TAG, "Forbidden C++ feature section '%s' "
+                             "(appendix C 11.2); rejecting image before load", name);
+                    return -EINVAL;
+                }
+            }
+        }
+    }
+
     /* Load section or segment to memory space */
```

(the hunk carries the `[patch p4]` comment block verbatim from the source)

### p5 — `.plugin_iram` IRAM section support (T7)

**Rationale:** requirements 4.10.3/4.10.4 budget plugin hot functions into
internal executable RAM; upstream `esp_elf_load_section()` knows only
`.text/.data/.rodata/.data.rel.ro/.bss` (all SPIRAM under
`CONFIG_ELF_LOADER_LOAD_PSRAM`) and silently ignores every other ALLOC
section — so a `.plugin_iram` section's relocations would write through
`esp_elf_map_sym()==0` (the patch p4 crash pattern). The patch, in scan
order inside `esp_elf_load_section()`:

1. **scan** section names for `.plugin_iram` (PROGBITS+ALLOC+EXECINSTR
   required; missing EXECINSTR or a duplicate section is `-EINVAL`) and
   record v_addr/size/offset with the **TRUE size** (patch p3 rationale:
   the recorded size is the `map_sym()`/`elf_remap_text()` window);
2. **budget consistency** (PLUG-009 / MEM 4.2.4): the caller copies the
   verified manifest's `iram_required_bytes` into `elf->iram_required_bytes`
   between `esp_elf_init()` and `esp_elf_relocate()`; the load aborts with
   `-EINVAL` **before any allocation** unless it equals the measured
   `sh_size` exactly, in both directions (0 is required for images without
   the section). Proven on board by the `iram_mismatch` negative
   (manifest 97+16 vs section 97 → `-EINVAL`, canary untouched);
3. **allocate** the copy via the new adapter helper `esp_elf_malloc_iram()`
   — explicit `MALLOC_CAP_EXEC|MALLOC_CAP_INTERNAL` (the analogue of the
   adapter's own internal-exec branch used when PSRAM loading is disabled),
   because `esp_elf_malloc()` hard-routes to SPIRAM. On S3 the IDF exec
   heap returns the IRAM-alias pointer (native `0x4037..` window, no bus
   mirror) with a stash word that `heap_caps_free()` unconverts, so
   `esp_elf_deinit()` releasing `piram` through the existing
   `esp_elf_free()` is exact;
4. **copy word-wise**: the S3 data bus can touch the instruction-bus SRAM
   window only with 32-bit accesses — the first board run crashed in ROM
   `memcpy`'s tail byte store (`s8i` to `0x403dbf54`, LoadStoreError,
   EXCCAUSE=3) after 96 bytes of word stores had succeeded; the same rule
   is why IDF provides the separate `MALLOC_CAP_IRAM_8BIT` cap. The copy
   loop assembles each word from source bytes (no source alignment
   assumption, no out-of-bounds read for byte-granular `sh_size`), and the
   recorded window keeps the true size so the zero padding in the final
   word is never mapped;
5. **relocations** need no new code: recording `sec[ELF_SEC_IRAM].addr`
   extends `esp_elf_map_sym()`, so the existing arch relocate loop covers
   `r_offsets` inside the section, and value relocations whose addend is an
   IRAM vaddr produce the native `0x40..` address because
   `elf_remap_text()` only remaps the PSRAM `.text` window (IRAM needs no
   `+0x06000000` mirror);
6. **cache sync**: `esp_elf_iram_cache_sync()` after the relocation loop
   uses the component's **full-writeback + cross-core-guard convention**
   (`Cache_WriteBack_All` via `esp_elf_arch_flush()`), NOT the ranged
   `esp_cache_msync()` on the IRAM span — justified by the pinned upstream
   commit itself (`6526c5b1` "Use full cache flush instead of ranged API on
   ESP32-S31": ranged cache APIs fault intermittently on unaligned spans,
   and a section copy is byte-granular by construction). On ESP32-S3
   internal SRAM sits behind no cache (the caches front external memory
   only; `SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE` is not defined), so the sync
   is defense-in-depth for the same load's PSRAM `.text` side and the
   required mechanism on L1-internal parts. A counter
   (`esp_elf_iram_cache_sync_count()`) makes the sync observable: it must
   advance by exactly one per IRAM load (T7 acceptance);
7. **deinit** frees `piram` mirroring `ptext`/`pdata` (100-cycle board
   soak: IRAM free watermark flat, drift +0);
8. drive-by fix in the same patch: `esp_elf_print_sec()`'s name array
   under-counted `ELF_SECS` upstream (read past the array for index ≥ 4);
   now lists all six slots.

Harness-side integration (poc_a app, outside the vendored tree): the mpb
view's `iram_required_bytes` is copied into the elf struct after
`esp_elf_init()`; `sdkconfig.defaults` sets `CONFIG_ESP_SYSTEM_MEMPROT=n`
because the IDF Kconfig for it states `MALLOC_CAP_EXEC` allocation is
impossible while memory protection is enabled (software-only PMP config,
no eFuse; required by 4.10.3/4.10.4).

Diff (paths relative to the component root; applies on top of p1..p4):

```diff
diff --git a/include/private/elf_platform.h b/include/private/elf_platform.h
index b346737..9ee3a2a 100644
--- a/include/private/elf_platform.h
+++ b/include/private/elf_platform.h
@@ -36,6 +36,14 @@ void *esp_elf_malloc(uint32_t n, bool exec);
  */
 void esp_elf_free(void *ptr);
 
+/* [patch p5] Allocate n bytes of internal executable memory for the
+ * .plugin_iram section copy (task T7). The regular esp_elf_malloc()
+ * hard-routes to SPIRAM under CONFIG_ELF_LOADER_LOAD_PSRAM, which is right
+ * for .text/.data but wrong for the IRAM budget; this helper keeps the
+ * allocation policy in the adapter like every other allocation of the
+ * component. */
+void *esp_elf_malloc_iram(uint32_t n);
+
 /**
  * @brief Relocates target architecture symbol of ELF
  *
@@ -70,6 +78,11 @@ uintptr_t elf_remap_text(esp_elf_t *elf, uintptr_t sym);
 void esp_elf_arch_flush(void);
 #endif
 
+/* [patch p5] Number of times the IRAM copy/relocation cache-sync routine
+ * has run (task T7 cache-sync observability: the counter must increment
+ * with every .plugin_iram load). */
+uint32_t esp_elf_iram_cache_sync_count(void);
+
 /**
  * @brief Initialize MMU hardware remapping function.
  *
diff --git a/include/private/elf_types.h b/include/private/elf_types.h
index c9b4f7a..0ac8022 100644
--- a/include/private/elf_types.h
+++ b/include/private/elf_types.h
@@ -119,6 +119,11 @@ extern "C" {
 #define ELF_PLT         ".plt"          /*!< procedure linkage table. */
 #define ELF_GOT_PLT     ".got.plt"      /*!< a table where resolved addresses from external functions are stored */
 
+/* [patch p5] Plugin hot-function section (task T7 of poc-a-dynamic-elf,
+ * requirements 4.10.3/4.10.4): plugin code the manifest budgets into
+ * internal executable RAM. Parsed only on the BUS_ADDRESS_MIRROR path. */
+#define ELF_IRAM        ".plugin_iram"  /*!< plugin IRAM-resident code */
+
 /** @brief ELF section and symbol operation */
 
 #define ELF_SEC_TEXT            0
@@ -126,7 +131,8 @@ extern "C" {
 #define ELF_SEC_DATA            2
 #define ELF_SEC_RODATA          3
 #define ELF_SEC_DRLRO           4
-#define ELF_SECS                5
+#define ELF_SEC_IRAM            5 /* [patch p5] */
+#define ELF_SECS                6
 
 #define ELF_ST_BIND(_i)         ((_i) >> 4)
 #define ELF_ST_TYPE(_i)         ((_i) & 0xf)
@@ -241,6 +247,7 @@ typedef struct esp_elf {
 #ifdef CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR
     unsigned char   *ptext;             /*!< instruction buffer pointer */
     unsigned char   *pdata;             /*!< data buffer pointer */
+    unsigned char   *piram;             /*!< [patch p5] .plugin_iram buffer (internal EXEC) */
 #else
     unsigned char   *psegment;          /*!< segment buffer pointer */
     uint32_t         svaddr;            /*!< start virtual address of segment */
@@ -250,6 +257,15 @@ typedef struct esp_elf {
 
     int (*entry)(int argc, char *argv[]);               /*!< Entry pointer of ELF */
 
+    /* [patch p5] Manifest-declared .plugin_iram budget (PLUG-009 / MEM
+     * 4.2.4): the caller copies iram_required_bytes from the verified mpb
+     * manifest view into this field between esp_elf_init() and
+     * esp_elf_relocate(); the load aborts with -EINVAL before any
+     * allocation unless it equals the section's measured sh_size. Zero
+     * (the esp_elf_init() reset) is the correct value for images without
+     * a .plugin_iram section. */
+    uint32_t        iram_required_bytes;
+
 #ifdef CONFIG_ELF_LOADER_SET_MMU
     uint32_t        text_off;           /*!< .text symbol offset */
 
diff --git a/src/esp_elf_adapter.c b/src/esp_elf_adapter.c
index 36a17bc..6b4c08f 100644
--- a/src/esp_elf_adapter.c
+++ b/src/esp_elf_adapter.c
@@ -70,6 +70,29 @@ void esp_elf_free(void *ptr)
     heap_caps_free(ptr);
 }
 
+/* [patch p5] Internal-EXEC allocation for the .plugin_iram section copy
+ * (task T7). Mirrors the adapter's own internal-exec selection used when
+ * PSRAM loading is disabled (esp_elf_malloc()'s exec branch): explicit
+ * EXEC|INTERNAL caps, so the section copy lands in the native IRAM
+ * execution window (0x4037_0000..0x403E_0000 on ESP32-S3) instead of the
+ * PSRAM data window. MALLOC_CAP_EXEC exists only when the IDF exec heap is
+ * available (CONFIG_HEAP_HAS_EXEC_HEAP, i.e. CONFIG_ESP_SYSTEM_MEMPROT=n;
+ * the IDF Kconfig help states EXEC allocation is impossible under memory
+ * protection), EXEC cannot be combined with 8BIT/DMA, and heap_caps
+ * transparently returns the IRAM-alias pointer with a stash word that
+ * heap_caps_free() unconverts - so esp_elf_free() stays correct. Fail
+ * closed (NULL) when the exec heap is not configured. */
+void *esp_elf_malloc_iram(uint32_t n)
+{
+#ifdef MALLOC_CAP_EXEC
+    return heap_caps_malloc(n, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
+#else
+    ESP_LOGE("elf_adapter", "MALLOC_CAP_EXEC unavailable (enable by setting "
+             "CONFIG_ESP_SYSTEM_MEMPROT=n); cannot load .plugin_iram");
+    return NULL;
+#endif
+}
+
 /**
  * @brief Remap symbol from ".data" to ".text" section.
  *
diff --git a/src/esp_elf.c b/src/esp_elf.c
index 0c03f2f..401b776 100644
--- a/src/esp_elf.c
+++ b/src/esp_elf.c
@@ -43,6 +43,41 @@ static const char *TAG = "ELF";
 static esp_elf_symbol_table_t *g_symbol_tables[SYMBOL_TABLES_NO];
 static _Atomic(symbol_resolver) current_resolver = elf_find_sym_default;
 
+/* [patch p5] Cache-sync observability counter (task T7): incremented every
+ * time the IRAM copy/relocation cache-sync routine runs; must advance with
+ * every .plugin_iram load. */
+static uint32_t g_iram_cache_syncs;
+
+uint32_t esp_elf_iram_cache_sync_count(void)
+{
+    return g_iram_cache_syncs;
+}
+
+/* [patch p5] Cache sync for the .plugin_iram copy + its relocations.
+ *
+ * Choice: the component's full-writeback convention (Cache_WriteBack_All +
+ * cross-core guard via esp_elf_arch_flush()), NOT the ranged
+ * esp_cache_msync() on the IRAM span. Justification from the pinned
+ * upstream evidence (commit 6526c5b1 "Use full cache flush instead of
+ * ranged API on ESP32-S31", the ADR-0002 archive note): the ranged cache
+ * APIs fault intermittently on unaligned addr/size spans, and a section
+ * copy is byte-granular by construction (sh_size is not line-aligned), so
+ * the IRAM span is exactly the hazard class that commit reverted away
+ * from. On ESP32-S3 internal SRAM sits behind no cache (the caches front
+ * external memory only), so D-side stores are already coherent with
+ * I-fetch and this sync is defense-in-depth for the same load's PSRAM
+ * .text side; on parts whose internal memory IS L1-cached the full
+ * writeback is the required sync. Called once per load after the
+ * relocation loop so both the copy and any relocations into the window
+ * are covered. */
+static void esp_elf_iram_cache_sync(void)
+{
+#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
+    esp_elf_arch_flush();
+#endif
+    g_iram_cache_syncs++;
+}
+
 /**
  * @brief Open and load an ELF file into memory.
  *
@@ -218,6 +253,31 @@ static int esp_elf_load_section(esp_elf_t *elf, const uint8_t *pbuf)
                 ESP_LOGD(TAG, ".data   offset is 0x%lx size is 0x%x",
                          elf->sec[ELF_SEC_DATA].offset,
                          elf->sec[ELF_SEC_DATA].size);
+            } else if (!strcmp(ELF_IRAM, name)) {
+                /* [patch p5] .plugin_iram (task T7, requirements 4.10.3/
+                 * 4.10.4): recorded like every other section with its TRUE
+                 * size - the recorded size is the map_sym()/elf_remap_text()
+                 * window and must never swallow the neighbouring section's
+                 * first vaddr(s) (patch p3 rationale). Duplicates and
+                 * non-executable spellings are rejected: the IRAM window is
+                 * only meaningful for code, and a silently ignored alias
+                 * would leave its relocations writing through
+                 * esp_elf_map_sym()==0 (the patch p4 crash pattern). */
+                if (!sflags(&shdr[i], SHF_EXECINSTR)) {
+                    ESP_LOGE(TAG, "Section %s lacks SHF_EXECINSTR; rejecting image", name);
+                    return -EINVAL;
+                }
+                if (elf->sec[ELF_SEC_IRAM].size) {
+                    ESP_LOGE(TAG, "Duplicate %s section; rejecting image", name);
+                    return -EINVAL;
+                }
+
+                ESP_LOGD(TAG, ".plugin_iram sec addr=0x%08x size=0x%08x offset=0x%08x",
+                         shdr[i].addr, shdr[i].size, shdr[i].offset);
+
+                elf->sec[ELF_SEC_IRAM].v_addr  = shdr[i].addr;
+                elf->sec[ELF_SEC_IRAM].size    = shdr[i].size;
+                elf->sec[ELF_SEC_IRAM].offset  = shdr[i].offset;
             } else if (!strcmp(ELF_RODATA, name)) {
                 ESP_LOGD(TAG, ".rodata sec addr=0x%08x size=0x%08x offset=0x%08x",
                          shdr[i].addr, shdr[i].size, shdr[i].offset);
@@ -263,6 +323,20 @@ static int esp_elf_load_section(esp_elf_t *elf, const uint8_t *pbuf)
         return -EINVAL;
     }
 
+    /* [patch p5] IRAM budget consistency (PLUG-009 / MEM 4.2.4): the
+     * manifest-declared iram_required_bytes (caller-copied from the
+     * verified mpb manifest view) must equal the section's measured sh_size
+     * exactly, in BOTH directions, checked before any allocation. An
+     * under-declared section would silently spend internal EXEC memory the
+     * admission layer never granted; an over-declared one hides the true
+     * footprint. Images without .plugin_iram must carry budget 0. */
+    if (elf->iram_required_bytes != elf->sec[ELF_SEC_IRAM].size) {
+        ESP_LOGE(TAG, ".plugin_iram budget mismatch: manifest=%" PRIu32
+                 " section=%" PRIu32 "; rejecting before allocation",
+                 elf->iram_required_bytes, (uint32_t)elf->sec[ELF_SEC_IRAM].size);
+        return -EINVAL;
+    }
+
     elf->ptext = esp_elf_malloc(elf->sec[ELF_SEC_TEXT].size, true);
     if (!elf->ptext) {
         ESP_LOGE(TAG, "Failed to malloc %"PRIu32" bytes for text section",
@@ -283,6 +357,23 @@ static int esp_elf_load_section(esp_elf_t *elf, const uint8_t *pbuf)
         }
     }
 
+    /* [patch p5] .plugin_iram copy target: internal EXEC memory (native
+     * 0x4037.. instruction window on ESP32-S3), NOT the PSRAM text window.
+     * Allocated through the adapter's explicit EXEC|INTERNAL selection
+     * because esp_elf_malloc() hard-routes to SPIRAM under
+     * CONFIG_ELF_LOADER_LOAD_PSRAM (the adapter's own internal-exec path,
+     * used upstream when PSRAM loading is disabled, is the analogue). */
+    if (elf->sec[ELF_SEC_IRAM].size) {
+        elf->piram = esp_elf_malloc_iram((uint32_t)elf->sec[ELF_SEC_IRAM].size);
+        if (!elf->piram) {
+            ESP_LOGE(TAG, "Failed to malloc %"PRIu32" bytes for plugin_iram section",
+                     (uint32_t)elf->sec[ELF_SEC_IRAM].size);
+            esp_elf_free(elf->pdata);
+            esp_elf_free(elf->ptext);
+            return -ENOMEM;
+        }
+    }
+
     /* Dump ".text" from ELF to executable space memory */
 
     elf->sec[ELF_SEC_TEXT].addr = (Elf32_Addr)elf->ptext;
@@ -339,6 +430,38 @@ static int esp_elf_load_section(esp_elf_t *elf, const uint8_t *pbuf)
         }
     }
 
+    /* [patch p5] Dump ".plugin_iram" to the internal executable window.
+     * Recording sec[ELF_SEC_IRAM].addr extends esp_elf_map_sym() so the
+     * generic relocation loop resolves r_offsets inside the section's
+     * vaddr window to the IRAM copy, and value relocations whose addend is
+     * an IRAM vaddr produce the native 0x40.. address - elf_remap_text()
+     * correctly leaves those untouched (they are outside the PSRAM .text
+     * window, and the IRAM window needs no +0x06000000 bus mirror).
+     * The copy itself is WORD-WISE: on ESP32-S3 the data bus can touch the
+     * instruction-bus SRAM window only with 32-bit accesses (byte stores
+     * raise LoadStoreError - the same rule that makes IDF provide the
+     * separate MALLOC_CAP_IRAM_8BIT cap). Verified on target: ROM memcpy's
+     * tail s8i to the alias faulted with EXCCAUSE=3. Source bytes are read
+     * individually so no source alignment is assumed. */
+    if (elf->sec[ELF_SEC_IRAM].size) {
+        const uint8_t *src = pbuf + elf->sec[ELF_SEC_IRAM].offset;
+        volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)elf->piram;
+        const uint32_t total = (uint32_t)elf->sec[ELF_SEC_IRAM].size;
+        elf->sec[ELF_SEC_IRAM].addr = (Elf32_Addr)elf->piram;
+
+        for (uint32_t off = 0; off < total; off += 4u) {
+            uint32_t word = 0;
+            const uint32_t remain = total - off;
+            for (uint32_t b = 0; b < 4u && b < remain; b++) {
+                word |= (uint32_t)src[off + b] << (8u * b);
+            }
+            dst[off >> 2] = word;
+        }
+
+        ESP_LOGI(TAG, ".plugin_iram copied to 0x%08x size %"PRIu32,
+                 (uint32_t)(uintptr_t)elf->piram, (uint32_t)elf->sec[ELF_SEC_IRAM].size);
+    }
+
     /* Set ELF entry */
 
     entry = ehdr->entry + elf->sec[ELF_SEC_TEXT].addr -
@@ -742,8 +865,17 @@ int esp_elf_relocate(esp_elf_t *elf, const uint8_t *pbuf)
         }
     }
 
+    /* [patch p5] IRAM loads sync through esp_elf_iram_cache_sync() (which
+     * performs the same full writeback + cross-core guard and counts it);
+     * the plain flush stays on the no-IRAM path so its behaviour is
+     * unchanged. */
+    if (elf->piram) {
+        esp_elf_iram_cache_sync();
+    } else
 #ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
-    esp_elf_arch_flush();
+    {
+        esp_elf_arch_flush();
+    }
 #endif
 
     return 0;
@@ -792,6 +924,13 @@ void esp_elf_deinit(esp_elf_t *elf)
         elf->pdata = NULL;
     }
 
+    /* [patch p5] release the .plugin_iram copy (mirrors ptext/pdata) so
+     * load/unload cycles return the internal EXEC watermark to baseline */
+    if (elf->piram) {
+        esp_elf_free(elf->piram);
+        elf->piram = NULL;
+    }
+
     if (elf->ptext) {
         esp_elf_free(elf->ptext);
         elf->ptext = NULL;
@@ -946,8 +1085,11 @@ void esp_elf_print_shdr(const uint8_t *pbuf)
  */
 void esp_elf_print_sec(esp_elf_t *elf)
 {
+    /* [patch p5] ELF_SECS grew to 6 (ELF_SEC_IRAM); the name array must
+     * cover every slot or the loop below reads past it (it already
+     * under-counted ELF_SEC_DRLRO upstream). */
     const char *sec_names[ELF_SECS] = {
-        "text", "bss", "data", "rodata"
+        "text", "bss", "data", "rodata", "drlro", "iram"
     };
 
     for (int i = 0; i < ELF_SECS; i++) {
```

Upstream files are NEVER edited in place without a registry entry; the
unpatched state must always be reproducible from this table (apply the
hunks above in reverse to `git show 6526c5b1:components/elf_loader/src/esp_elf.c`).
