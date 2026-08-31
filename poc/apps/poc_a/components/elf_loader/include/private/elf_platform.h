/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "private/elf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Notes: align_size needs to be a power of 2 */

#define ELF_ALIGN(_a, align_size) (((_a) + (align_size - 1)) & \
                                   ~(align_size - 1))

/**
 * @brief Allocate block of memory.
 *
 * @param n - Memory size in byte
 * @param exec - True: memory can run executable code; False: memory can R/W data
 *
 * @return Memory pointer if success or NULL if failed.
 */
void *esp_elf_malloc(uint32_t n, bool exec);

/**
 * @brief Free block of memory.
 *
 * @param ptr - memory block pointer allocated by "esp_elf_malloc"
 *
 * @return None
 */
void esp_elf_free(void *ptr);

/* [patch p5] Allocate n bytes of internal executable memory for the
 * .plugin_iram section copy (task T7). The regular esp_elf_malloc()
 * hard-routes to SPIRAM under CONFIG_ELF_LOADER_LOAD_PSRAM, which is right
 * for .text/.data but wrong for the IRAM budget; this helper keeps the
 * allocation policy in the adapter like every other allocation of the
 * component. */
void *esp_elf_malloc_iram(uint32_t n);

/**
 * @brief Relocates target architecture symbol of ELF
 *
 * @param elf  - ELF object pointer
 * @param rela - Relocated symbol data
 * @param sym  - ELF symbol table
 * @param addr - Jumping target address
 *
 * @return ESP_OK if success or other if failed.
 */
int esp_elf_arch_relocate(esp_elf_t *elf, const elf32_rela_t *rela,
                          const elf32_sym_t *sym, uint32_t addr);

/**
 * @brief Remap symbol from ".data" to ".text" section.
 *
 * @param elf  - ELF object pointer
 * @param sym  - ELF symbol table
 *
 * @return Remapped symbol value
 */
#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET
uintptr_t elf_remap_text(esp_elf_t *elf, uintptr_t sym);
#endif

/**
 * @brief Flush data from cache to external RAM.
 *
 * @return None
 */
#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
void esp_elf_arch_flush(void);
#endif

/* [patch p5] Number of times the IRAM copy/relocation cache-sync routine
 * has run (task T7 cache-sync observability: the counter must increment
 * with every .plugin_iram load). */
uint32_t esp_elf_iram_cache_sync_count(void);

/**
 * @brief Initialize MMU hardware remapping function.
 *
 * @param elf - ELF object pointer
 *
 * @return 0 if success or a negative value if failed.
 */
#ifdef CONFIG_ELF_LOADER_SET_MMU
int esp_elf_arch_init_mmu(esp_elf_t *elf);
#endif

/**
 * @brief De-initialize MMU hardware remapping function.
 *
 * @param elf - ELF object pointer
 *
 * @return None
 */
#ifdef CONFIG_ELF_LOADER_SET_MMU
void esp_elf_arch_deinit_mmu(esp_elf_t *elf);
#endif

#ifdef __cplusplus
}
#endif
