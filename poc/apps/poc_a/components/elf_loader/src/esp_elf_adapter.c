/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <sys/errno.h>
#include "esp_idf_version.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "soc/soc.h"
#if CONFIG_IDF_TARGET_ESP32S31
#include "esp32s31/rom/cache.h"
#include "soc/cache_reg.h"
#endif
#include "private/elf_platform.h"

#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
#ifdef CONFIG_IDF_TARGET_ESP32S3
#define OFFSET_TEXT_VALUE   (SOC_IROM_LOW - SOC_DROM_LOW)
#endif
#endif

/**
 * @brief Allocate block of memory.
 *
 * @param n - Memory size in byte
 * @param exec - True: memory can run executable code; False: memory can R/W data
 *
 * @return Memory pointer if success or NULL if failed.
 */
void *esp_elf_malloc(uint32_t n, bool exec)
{
    uint32_t caps;

#if CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR
#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
    caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
#ifdef MALLOC_CAP_EXEC
    caps = exec ? MALLOC_CAP_EXEC : MALLOC_CAP_8BIT;
#else
    caps = MALLOC_CAP_8BIT | MALLOC_CAP_32BIT;
#endif
#endif
#else
#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
    caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    caps = MALLOC_CAP_8BIT;
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    caps |= MALLOC_CAP_CACHE_ALIGNED;
#endif
#endif

    return heap_caps_malloc(n, caps);
}

/**
 * @brief Free block of memory.
 *
 * @param ptr - memory block pointer allocated by "esp_elf_malloc"
 *
 * @return None
 */
void esp_elf_free(void *ptr)
{
    heap_caps_free(ptr);
}

/* [patch p5] Internal-EXEC allocation for the .plugin_iram section copy
 * (task T7). Mirrors the adapter's own internal-exec selection used when
 * PSRAM loading is disabled (esp_elf_malloc()'s exec branch): explicit
 * EXEC|INTERNAL caps, so the section copy lands in the native IRAM
 * execution window (0x4037_0000..0x403E_0000 on ESP32-S3) instead of the
 * PSRAM data window. MALLOC_CAP_EXEC exists only when the IDF exec heap is
 * available (CONFIG_HEAP_HAS_EXEC_HEAP, i.e. CONFIG_ESP_SYSTEM_MEMPROT=n;
 * the IDF Kconfig help states EXEC allocation is impossible under memory
 * protection), EXEC cannot be combined with 8BIT/DMA, and heap_caps
 * transparently returns the IRAM-alias pointer with a stash word that
 * heap_caps_free() unconverts - so esp_elf_free() stays correct. Fail
 * closed (NULL) when the exec heap is not configured. */
void *esp_elf_malloc_iram(uint32_t n)
{
#ifdef MALLOC_CAP_EXEC
    return heap_caps_malloc(n, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
#else
    ESP_LOGE("elf_adapter", "MALLOC_CAP_EXEC unavailable (enable by setting "
             "CONFIG_ESP_SYSTEM_MEMPROT=n); cannot load .plugin_iram");
    return NULL;
#endif
}

/**
 * @brief Remap symbol from ".data" to ".text" section.
 *
 * @param elf  - ELF object pointer
 * @param sym  - ELF symbol table
 *
 * @return Remapped symbol value
 */
#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET
uintptr_t elf_remap_text(esp_elf_t *elf, uintptr_t sym)
{
    uintptr_t mapped_sym;
    esp_elf_sec_t *sec = &elf->sec[ELF_SEC_TEXT];

    if ((sym >= sec->addr) &&
            (sym < (sec->addr + sec->size))) {
#ifdef CONFIG_ELF_LOADER_SET_MMU
        mapped_sym = sym + elf->text_off;
#else
        mapped_sym = sym + OFFSET_TEXT_VALUE;
#endif
    } else {
        mapped_sym = sym;
    }

    return mapped_sym;
}
#endif

/**
 * @brief Flush data from cache to external RAM.
 *
 * @param None
 *
 * @return None
 */
#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
void IRAM_ATTR esp_elf_arch_flush(void)
{
    extern void spi_flash_disable_interrupts_caches_and_other_cpu(void);
    extern void spi_flash_enable_interrupts_caches_and_other_cpu(void);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)

#if CONFIG_IDF_TARGET_ESP32S31
    /* ESP32-S31: Ranged cache APIs (Cache_WriteBack_Addr/Cache_Invalidate_Addr)
     * cause intermittent Instruction access faults (MCAUSE=0x01,
     * MEPC=0x00000000) during long-term ELF execution from PSRAM, likely
     * related to unaligned addr/size (e.g. seg_size=0x136a8, cache_line=64B).
     * Use full D-writeback + I-invalidate like other targets. */
    Cache_WriteBack_All(CACHE_MAP_L1_DCACHE);
    spi_flash_disable_interrupts_caches_and_other_cpu();
    Cache_Invalidate_All(CACHE_MAP_L1_ICACHE_MASK);
    spi_flash_enable_interrupts_caches_and_other_cpu();
    REG_CLR_BIT(CACHE_L1_ICACHE_CTRL_REG, CACHE_L1_ICACHE_SHUT_IBUS1);
#else
    extern void Cache_WriteBack_All(void);
    Cache_WriteBack_All();
    spi_flash_disable_interrupts_caches_and_other_cpu();
    spi_flash_enable_interrupts_caches_and_other_cpu();
#endif
#else
    void esp_spiram_writeback_cache(void);

    esp_spiram_writeback_cache();
    spi_flash_disable_interrupts_caches_and_other_cpu();
    spi_flash_enable_interrupts_caches_and_other_cpu();
#endif
}
#endif
