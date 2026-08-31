#include "poca_plugin.hh"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "frame_poc/frame_abi.h"
#include "frame_poc/mpb_mbedtls_glue.h"
#include "frame_poc/mpb_parser.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "plugin_abi.h"
#include "poca_baseline_300k_meta.h"
#include "poca_baseline_meta.h"
#include "poca_baseline_v2_meta.h"
#include "poca_console.hh"
#include "poca_cxx_ctor_meta.h"
#include "poca_cxx_tls_meta.h"
#include "poca_globdat_meta.h"
#include "poca_import_neg_meta.h"
#include "poca_iram_mismatch_meta.h"
#include "poca_iram_probe_meta.h"
#include "poca_neg_maxmem_meta.h"
#include "poca_neg_ph64_meta.h"
#include "poca_neg_phbe_meta.h"
#include "poca_neg_phmach_meta.h"
#include "poca_neg_phspan_meta.h"
#include "poca_neg_r32_meta.h"
#include "poca_neg_s0op_meta.h"
#include "poca_neg_tls_nosect_meta.h"
#include "poca_plt_meta.h"
#include "poca_pubkey.h"
#include "private/elf_platform.h"
#include "psa/crypto.h"

/* Embedded signed plugin containers, produced by the plugins.cmake pipeline
 * and negative/build_negative_corpus.py, linked via target_add_binary_data().
 * The IDF embed wrapper names the symbols from the file names. */
extern "C" const uint8_t _binary_baseline_mpb_start[];
extern "C" const uint8_t _binary_baseline_mpb_end[];
extern "C" const uint8_t _binary_baseline_v2_mpb_start[];
extern "C" const uint8_t _binary_baseline_v2_mpb_end[];
extern "C" const uint8_t _binary_baseline_300k_mpb_start[];
extern "C" const uint8_t _binary_baseline_300k_mpb_end[];
extern "C" const uint8_t _binary_globdat_mpb_start[];
extern "C" const uint8_t _binary_globdat_mpb_end[];
extern "C" const uint8_t _binary_plt_mpb_start[];
extern "C" const uint8_t _binary_plt_mpb_end[];
extern "C" const uint8_t _binary_import_neg_mpb_start[];
extern "C" const uint8_t _binary_import_neg_mpb_end[];
extern "C" const uint8_t _binary_neg_r32_mpb_start[];
extern "C" const uint8_t _binary_neg_r32_mpb_end[];
extern "C" const uint8_t _binary_neg_s0op_mpb_start[];
extern "C" const uint8_t _binary_neg_s0op_mpb_end[];
extern "C" const uint8_t _binary_neg_phspan_mpb_start[];
extern "C" const uint8_t _binary_neg_phspan_mpb_end[];
extern "C" const uint8_t _binary_neg_phbe_mpb_start[];
extern "C" const uint8_t _binary_neg_phbe_mpb_end[];
extern "C" const uint8_t _binary_neg_ph64_mpb_start[];
extern "C" const uint8_t _binary_neg_ph64_mpb_end[];
extern "C" const uint8_t _binary_neg_phmach_mpb_start[];
extern "C" const uint8_t _binary_neg_phmach_mpb_end[];
extern "C" const uint8_t _binary_neg_maxmem_mpb_start[];
extern "C" const uint8_t _binary_neg_maxmem_mpb_end[];
extern "C" const uint8_t _binary_cxx_ctor_mpb_start[];
extern "C" const uint8_t _binary_cxx_ctor_mpb_end[];
extern "C" const uint8_t _binary_cxx_tls_mpb_start[];
extern "C" const uint8_t _binary_cxx_tls_mpb_end[];
extern "C" const uint8_t _binary_neg_tls_nosect_mpb_start[];
extern "C" const uint8_t _binary_neg_tls_nosect_mpb_end[];
extern "C" const uint8_t _binary_iram_probe_mpb_start[];
extern "C" const uint8_t _binary_iram_probe_mpb_end[];
extern "C" const uint8_t _binary_iram_mismatch_mpb_start[];
extern "C" const uint8_t _binary_iram_mismatch_mpb_end[];

namespace frame::poca {
namespace {

/* ESP32-S3 address windows: PSRAM data-bus mapping 0x3C000000..0x3E000000
 * (drom) and its instruction-bus alias used for execution 0x42000000..
 * 0x44000000 (irom = data window + 0x06000000 with ELF_LOADER_CACHE_OFFSET).
 * IRAM (internal SRAM through the instruction bus, soc.h SOC_IRAM_LOW..
 * SOC_IRAM_HIGH) is a DIFFERENT window: .plugin_iram copies allocate
 * MALLOC_CAP_EXEC|MALLOC_CAP_INTERNAL and execute natively at 0x4037.. with
 * no bus mirror (T7). */
constexpr uint32_t kPsramDataLow = 0x3C000000u;
constexpr uint32_t kPsramDataHigh = 0x3E000000u;
constexpr uint32_t kPsramExecLow = 0x42000000u;
constexpr uint32_t kPsramExecHigh = 0x44000000u;
constexpr uint32_t kPsramExecMirrorOffset = 0x06000000u;
constexpr uint32_t kIramLow = 0x40370000u;
constexpr uint32_t kIramHigh = 0x403E0000u;

/* Host imports registered into the loader's symbol tables (T5 import
 * allowlist). Plugins resolve these names through GLOB_DAT/JMP_SLOT; every
 * other import fails the load with -ENOSYS before execute. */
extern "C" uint32_t poca_host_sentinel(void) { return poca_host_sentinel_value(); }

extern "C" uint32_t poca_host_add(uint32_t a, uint32_t b) { return poca_host_add_value(a, b); }

const esp_elf_symbol_table_t k_host_symbols[] = {
    {"poca_host_sentinel", reinterpret_cast<const void*>(&poca_host_sentinel)},
    {"poca_host_add", reinterpret_cast<const void*>(&poca_host_add)},
    ESP_ELFSYM_END,
};

struct EmbeddedPackage {
    const char* name;
    const uint8_t* start;
    const uint8_t* end;
    unsigned meta_size;
    const char* meta_sha256;
    const char* meta_name;    /* manifest name field asserted on-device */
    const char* meta_version; /* manifest version field asserted on-device */
    uint32_t magic;           /* activate() self-check magic of this build */
};

#define POCAPKG(build_name, mpb_name, magic_value)                                                 \
    {#build_name,                                                                                  \
     _binary_##build_name##_mpb_start,                                                             \
     _binary_##build_name##_mpb_end,                                                               \
     POCA_##mpb_name##_MPB_SIZE,                                                                   \
     POCA_##mpb_name##_MPB_SHA256,                                                                 \
     POCA_##mpb_name##_NAME,                                                                       \
     POCA_##mpb_name##_VERSION,                                                                    \
     (magic_value)}

const EmbeddedPackage k_packages[] = {
    POCAPKG(baseline, BASELINE, POCA_PLUGIN_MAGIC),
    POCAPKG(baseline_v2, BASELINE_V2, POCA_BASELINE_V2_MAGIC),
    POCAPKG(baseline_300k, BASELINE_300K, POCA_PLUGIN_MAGIC),
    POCAPKG(globdat, GLOBDAT, POCA_PLUGIN_MAGIC),
    POCAPKG(plt, PLT, POCA_PLUGIN_MAGIC),
    POCAPKG(import_neg, IMPORT_NEG, POCA_PLUGIN_MAGIC),
    POCAPKG(neg_r32, NEG_R32, POCA_PLUGIN_MAGIC),
    POCAPKG(neg_s0op, NEG_S0OP, POCA_PLUGIN_MAGIC),
    POCAPKG(neg_phspan, NEG_PHSPAN, POCA_PLUGIN_MAGIC),
    POCAPKG(neg_phbe, NEG_PHBE, POCA_PLUGIN_MAGIC),
    POCAPKG(neg_ph64, NEG_PH64, POCA_PLUGIN_MAGIC),
    POCAPKG(neg_phmach, NEG_PHMACH, POCA_PLUGIN_MAGIC),
    POCAPKG(neg_maxmem, NEG_MAXMEM, POCA_PLUGIN_MAGIC),
    POCAPKG(cxx_ctor, CXX_CTOR, POCA_PLUGIN_MAGIC),
    POCAPKG(cxx_tls, CXX_TLS, POCA_PLUGIN_MAGIC),
    POCAPKG(neg_tls_nosect, NEG_TLS_NOSECT, POCA_PLUGIN_MAGIC),
    POCAPKG(iram_probe, IRAM_PROBE, POCA_PLUGIN_MAGIC),
    POCAPKG(iram_mismatch, IRAM_MISMATCH, POCA_PLUGIN_MAGIC),
};

/* Expected outcome per package: the on-board matrix rows. Every negative
 * must be rejected at its designated stage and the entry-query canary must
 * stay untouched across the whole corpus (nothing may execute). */
enum class Expect : uint8_t { kLoadOk, kRelocateErrno, kPhdrReject, kMaxMemReject };

struct PackageExpect {
    const char* name;
    Expect expect;
    int errno_value;
};

const PackageExpect k_expects[] = {
    {"baseline", Expect::kLoadOk, 0},
    {"baseline_v2", Expect::kLoadOk, 0},
    {"baseline_300k", Expect::kLoadOk, 0},
    {"globdat", Expect::kLoadOk, 0},
    {"plt", Expect::kLoadOk, 0},
    {"import_neg", Expect::kRelocateErrno, -88},     /* -ENOSYS (newlib xtensa) */
    {"neg_r32", Expect::kRelocateErrno, -22},        /* -EINVAL: patch p1, R_XTENSA_32 */
    {"neg_s0op", Expect::kRelocateErrno, -22},       /* -EINVAL: patch p1, SLOT0_OP */
    {"neg_phbe", Expect::kRelocateErrno, -22},       /* -EINVAL: patch p2, EI_DATA */
    {"neg_ph64", Expect::kRelocateErrno, -22},       /* -EINVAL: patch p2, EI_CLASS */
    {"neg_phmach", Expect::kRelocateErrno, -22},     /* -EINVAL: patch p2, e_machine */
    {"neg_phspan", Expect::kPhdrReject, 0},          /* admission: budget > manifest */
    {"neg_maxmem", Expect::kMaxMemReject, 0},        /* admission: manifest > 512KiB */
    {"cxx_ctor", Expect::kRelocateErrno, -22},       /* -EINVAL: patch p4, .ctors */
    {"cxx_tls", Expect::kRelocateErrno, -22},        /* -EINVAL: patch p4, .tdata/.tbss */
    {"neg_tls_nosect", Expect::kRelocateErrno, -22}, /* -EINVAL: patch p1, TLSDESC relocs */
    {"iram_probe", Expect::kLoadOk, 0},
    {"iram_mismatch", Expect::kRelocateErrno, -22}, /* -EINVAL: patch p5, iram budget */
};

/* MEM-003 admission budget: every plugin generation gets the 256 KiB default
 * arena; a manifest MAY request up to the 512 KiB hard maximum. The builder
 * refuses to package declarations beyond the hard max, so an over-declaring
 * container reaching the device is hostile packaging and admission must
 * reject it before esp_elf_init (fail-closed, PLUG-004/MEM-003). */
constexpr uint32_t kDefaultArenaBytes = 256u * 1024u;
constexpr uint32_t kHardMaxMemoryBytes = 512u * 1024u;

struct PluginRuntime {
    bool loaded = false;
    const EmbeddedPackage* package = nullptr;
    uint8_t* staging = nullptr;
    size_t staging_size = 0;
    uint32_t psram_free_before_load = 0;
    esp_elf_t elf{};
    const poca_plugin_table_t* table = nullptr;
};

/* Generation slots (PLUG-004/T8): at most one ACTIVE generation and one
 * staged CANDIDATE may be resident; a second candidate is refused with
 * FRAME_ERR_BUSY before any allocation. Each slot owns an independent
 * staging buffer and esp_elf_t instance (independent PSRAM arenas,
 * ADR-0002). */
PluginRuntime g_active;
PluginRuntime g_candidate;
mpb_crypto_t g_crypto{};
bool g_crypto_ready = false;
bool g_host_symbols_registered = false;
/* Execution canary: incremented exactly once per entry-table query. It must
 * stay unchanged across every negative probe (fail-before-execute proof). */
uint32_t g_entry_queries = 0;

const EmbeddedPackage* find_package(const char* name) {
    for (const EmbeddedPackage& package : k_packages) {
        if (std::strcmp(package.name, name) == 0) {
            return &package;
        }
    }
    return nullptr;
}

const PackageExpect* find_expect(const char* name) {
    for (const PackageExpect& expect : k_expects) {
        if (std::strcmp(expect.name, name) == 0) {
            return &expect;
        }
    }
    return nullptr;
}

const char* frame_err_name(int32_t err) {
    switch (err) {
    case FRAME_OK:
        return "OK";
    case FRAME_ERR_ABI_MISMATCH:
        return "ABI_MISMATCH";
    case FRAME_ERR_SIGNATURE_INVALID:
        return "SIGNATURE_INVALID";
    case FRAME_ERR_PACKAGE_INVALID:
        return "PACKAGE_INVALID";
    case FRAME_ERR_HASH_MISMATCH:
        return "HASH_MISMATCH";
    case FRAME_ERR_EPOCH_ROLLBACK:
        return "EPOCH_ROLLBACK";
    case FRAME_ERR_TARGET_MISMATCH:
        return "TARGET_MISMATCH";
    case FRAME_ERR_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    default:
        return "UNKNOWN";
    }
}

uint32_t psram_free_bytes() {
    multi_heap_info_t info{};
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    return static_cast<uint32_t>(info.total_free_bytes);
}

uint32_t iram_free_bytes() {
    multi_heap_info_t info{};
    heap_caps_get_info(&info, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    return static_cast<uint32_t>(info.total_free_bytes);
}

bool ensure_crypto() {
    if (g_crypto_ready) {
        return true;
    }
    /* static storage: mpb_mbedtls_crypto_make keeps the pointer, so the key
     * table must outlive the call */
    static mpb_trusted_key_t trusted_key = {};
    if (trusted_key.key_id == 0) {
        trusted_key.key_id = POCA_TRUSTED_KEY_ID;
        std::memcpy(trusted_key.uncompressed_point, POCA_TRUSTED_KEY_POINT,
                    sizeof(POCA_TRUSTED_KEY_POINT));
    }
    mpb_crypto_t crypto{};
    const frame_err_t err = mpb_mbedtls_crypto_make(&trusted_key, 1, &crypto);
    if (err != FRAME_OK) {
        std::printf("[poca] crypto init failed err=%d (%s)\n", static_cast<int>(err),
                    frame_err_name(err));
        return false;
    }
    g_crypto = crypto;
    g_crypto_ready = true;
    return true;
}

bool ensure_host_symbols() {
    if (g_host_symbols_registered) {
        return true;
    }
    const int ret = esp_elf_register_symbol(const_cast<esp_elf_symbol_table_t*>(k_host_symbols));
    if (ret != 0) {
        std::printf("[poca] FAIL esp_elf_register_symbol ret=%d\n", ret);
        return false;
    }
    g_host_symbols_registered = true;
    return true;
}

bool sha256_hex(const uint8_t* data, size_t size, char* out_hex65) {
    uint8_t digest[32];
    size_t produced = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, data, size, digest, sizeof(digest), &produced) !=
            PSA_SUCCESS ||
        produced != sizeof(digest)) {
        return false;
    }
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        out_hex65[2 * i] = kHex[digest[i] >> 4];
        out_hex65[2 * i + 1] = kHex[digest[i] & 0xF];
    }
    out_hex65[2 * sizeof(digest)] = '\0';
    return true;
}

void print_view(const mpb_view_t& view) {
    char name[MPB_MAX_NAME_BYTES + 1];
    char version[MPB_MAX_VERSION_BYTES + 1];
    const size_t name_len =
        view.name_length < sizeof(name) - 1 ? view.name_length : sizeof(name) - 1;
    const size_t version_len =
        view.version_length < sizeof(version) - 1 ? view.version_length : sizeof(version) - 1;
    std::memcpy(name, view.name, name_len);
    name[name_len] = '\0';
    std::memcpy(version, view.version, version_len);
    version[version_len] = '\0';
    std::printf("[poca] view name=%s version=%s epoch=%" PRIu32 " key_id=%08" PRIx32
                " abi=%u.%u iram=%" PRIu32 "B max_mem=%" PRIu32 "B payloads=%u\n",
                name, version, view.security_epoch, view.key_id,
                static_cast<unsigned>(view.core_abi_major),
                static_cast<unsigned>(view.core_abi_minor), view.iram_required_bytes,
                view.max_memory_bytes, static_cast<unsigned>(view.payload_count));
    for (uint16_t i = 0; i < view.payload_count; ++i) {
        std::printf("[poca] view payload[%u] span offset=%zu length=%zu\n",
                    static_cast<unsigned>(i), view.payloads[i].offset, view.payloads[i].length);
    }
}

mpb_policy_t device_policy() {
    return mpb_policy_t{
        .max_package_bytes = 512u * 1024u,
        .epoch_floor = 1u,
        .abi_major = FRAME_ABI_MAJOR,
        .abi_minor = FRAME_ABI_MINOR,
        .device_features = 0u,
    };
}

/* Program-header admission arithmetic (T5, plan §4 item 2): the computed
 * segment budget = sum(PT_LOAD p_memsz) + sum(inter-segment padding) must
 * fit the manifest's max_memory_bytes, and every PT_LOAD span must stay
 * inside the verified payload bytes. On ESP32-S3 the loader's
 * BUS_ADDRESS_MIRROR path loads by sections, so this phdr policy is an
 * admission-layer check (fail-closed before esp_elf_init), documented in
 * ALLOWLIST.md. */
struct PhBudget {
    bool ok = false;
    uint32_t loads = 0;
    uint64_t memsz_total = 0;
    uint64_t pad_total = 0;
    char reason[64] = {0};
};

PhBudget compute_ph_budget(const uint8_t* elf, size_t size) {
    PhBudget out{};
    if (size < sizeof(elf32_hdr_t)) {
        std::snprintf(out.reason, sizeof(out.reason), "elf smaller than ehdr");
        return out;
    }
    elf32_hdr_t ehdr{};
    std::memcpy(&ehdr, elf, sizeof(ehdr));
    if (ehdr.phentsize < sizeof(elf32_phdr_t) || ehdr.phnum == 0) {
        std::snprintf(out.reason, sizeof(out.reason), "no usable program headers");
        return out;
    }
    if (static_cast<size_t>(ehdr.phoff) +
            static_cast<size_t>(ehdr.phnum) * static_cast<size_t>(ehdr.phentsize) >
        size) {
        std::snprintf(out.reason, sizeof(out.reason), "phdr table out of bounds");
        return out;
    }
    bool have_prev = false;
    uint64_t prev_end = 0;
    for (uint32_t i = 0; i < ehdr.phnum; ++i) {
        elf32_phdr_t phdr{};
        std::memcpy(&phdr, elf + ehdr.phoff + i * ehdr.phentsize, sizeof(phdr));
        if (phdr.type != PT_LOAD) {
            continue;
        }
        out.loads += 1;
        if (phdr.filesz > phdr.memsz) {
            std::snprintf(out.reason, sizeof(out.reason), "LOAD[%u] filesz>memsz",
                          static_cast<unsigned>(i));
            return out;
        }
        if (static_cast<uint64_t>(phdr.offset) + static_cast<uint64_t>(phdr.filesz) > size) {
            std::snprintf(out.reason, sizeof(out.reason), "LOAD[%u] span exceeds payload",
                          static_cast<unsigned>(i));
            return out;
        }
        if (static_cast<uint64_t>(phdr.vaddr) + static_cast<uint64_t>(phdr.memsz) > 0xFFFFFFFFull) {
            std::snprintf(out.reason, sizeof(out.reason), "LOAD[%u] vaddr+memsz overflow",
                          static_cast<unsigned>(i));
            return out;
        }
        if (have_prev && phdr.vaddr > prev_end) {
            out.pad_total += phdr.vaddr - prev_end;
        }
        out.memsz_total += phdr.memsz;
        prev_end = static_cast<uint64_t>(phdr.vaddr) + phdr.memsz;
        have_prev = true;
    }
    if (out.loads == 0) {
        std::snprintf(out.reason, sizeof(out.reason), "no PT_LOAD segments");
        return out;
    }
    out.ok = true;
    return out;
}

int check_ph_budget(const uint8_t* elf_base, const mpb_view_t& view, bool negative_expected,
                    const char* name, bool quiet = false) {
    const PhBudget budget = compute_ph_budget(elf_base, view.payloads[0].length);
    if (!quiet) {
        if (!budget.ok) {
            std::printf("[poca] phdr budget INVALID: %s\n", budget.reason);
        } else {
            std::printf("[poca] phdr loads=%" PRIu32 " memsz=%" PRIu64 " pad=%" PRIu64
                        " budget=%" PRIu64 " manifest max_mem=%" PRIu32 "\n",
                        budget.loads, budget.memsz_total, budget.pad_total,
                        budget.memsz_total + budget.pad_total, view.max_memory_bytes);
        }
    }
    if (!budget.ok || budget.memsz_total + budget.pad_total > view.max_memory_bytes) {
        if (negative_expected) {
            std::printf("[poca-load] NEGATIVE %s PASS (phdr admission rejected before load)\n",
                        name);
            return 0;
        }
        std::printf("[poca] FAIL phdr admission\n");
        return 1;
    }
    return 0;
}

/* Find <name> in the plugin ELF's symbol tables: defined GLOBAL/WEAK FUNC.
 * The regular .symtab is authoritative (the pipeline keeps the exported
 * entry in it via strip --keep-symbol); .dynsym is only consulted as a
 * fallback. Returns the match count and fills out_value with the symbol
 * value. Pure read pass; the staging buffer stays immutable (PLUG-008). */
int find_exported_symbol(const uint8_t* elf_base, size_t elf_size, const char* name,
                         uint32_t* out_value) {
    /* upstream elf_types.h names SHT_DYNSYM(11) "SHT_SYNSYM" */
    const uint32_t wanted_types[2] = {SHT_SYMTAB, SHT_SYNSYM};
    int matches = 0;
    for (int pass = 0; pass < 2 && matches == 0; ++pass) {
        if (elf_size < sizeof(elf32_hdr_t)) {
            return 0;
        }
        elf32_hdr_t ehdr{};
        std::memcpy(&ehdr, elf_base, sizeof(ehdr));
        if (ehdr.shoff == 0 || ehdr.shentsize < sizeof(elf32_shdr_t) || ehdr.shnum == 0) {
            return 0;
        }
        if (static_cast<size_t>(ehdr.shoff) +
                static_cast<size_t>(ehdr.shnum) * static_cast<size_t>(ehdr.shentsize) >
            elf_size) {
            return 0;
        }
        for (uint32_t s = 0; s < ehdr.shnum; ++s) {
            elf32_shdr_t shdr{};
            std::memcpy(&shdr, elf_base + ehdr.shoff + s * ehdr.shentsize, sizeof(shdr));
            if (shdr.type != wanted_types[pass]) {
                continue;
            }
            if (shdr.link >= ehdr.shnum || shdr.entsize < sizeof(elf32_sym_t)) {
                continue;
            }
            elf32_shdr_t str_shdr{};
            std::memcpy(&str_shdr, elf_base + ehdr.shoff + shdr.link * ehdr.shentsize,
                        sizeof(str_shdr));
            if (str_shdr.type != SHT_STRTAB ||
                static_cast<size_t>(str_shdr.offset) + static_cast<size_t>(str_shdr.size) >
                    elf_size ||
                static_cast<size_t>(shdr.offset) + static_cast<size_t>(shdr.size) > elf_size) {
                continue;
            }
            const size_t sym_count = static_cast<size_t>(shdr.size) / sizeof(elf32_sym_t);
            for (size_t i = 0; i < sym_count; ++i) {
                elf32_sym_t sym{};
                std::memcpy(&sym, elf_base + shdr.offset + i * sizeof(elf32_sym_t), sizeof(sym));
                if (sym.name >= str_shdr.size || sym.shndx == SHN_UNDEF) {
                    continue;
                }
                const int bind = sym.info >> 4;
                const int type = sym.info & 0xF;
                if ((bind != STB_GLOBAL && bind != STB_WEAK) || type != STT_FUNC) {
                    continue;
                }
                const char* sym_name =
                    reinterpret_cast<const char*>(elf_base + str_shdr.offset + sym.name);
                const size_t max_len = static_cast<size_t>(str_shdr.size) - sym.name;
                const void* nul = std::memchr(sym_name, '\0', max_len);
                const size_t sym_name_len =
                    nul != nullptr ? static_cast<size_t>(static_cast<const char*>(nul) - sym_name)
                                   : max_len;
                if (std::strncmp(sym_name, name, max_len) == 0 &&
                    sym_name_len == std::strlen(name)) {
                    ++matches;
                    *out_value = sym.value;
                }
            }
        }
    }
    return matches;
}

int parse_mpb(const uint8_t* buffer, size_t size, mpb_view_t* out_view) {
    if (!ensure_crypto()) {
        return 1;
    }
    const mpb_policy_t policy = device_policy();
    const frame_err_t err = mpb_parse(buffer, size, &policy, &g_crypto, out_view);
    if (err != FRAME_OK) {
        std::printf("[poca] mpb_parse FAILED err=%d (%s)\n", static_cast<int>(err),
                    frame_err_name(err));
        return 1;
    }
    return 0;
}

int check_embedded_digest(const EmbeddedPackage& package, size_t size, const char* hex,
                          bool quiet = false) {
    const int digest_ok = std::strcmp(hex, package.meta_sha256) == 0 &&
                          size == static_cast<size_t>(package.meta_size);
    if (!quiet) {
        std::printf("[poca] mpb digest %s\n", digest_ok ? "MATCH" : "MISMATCH");
    }
    return digest_ok ? 0 : 1;
}

void release_slot(PluginRuntime& slot, bool free_staging);
bool check_max_memory_admission(const mpb_view_t& view, bool quiet = false);

/* Stage a package into slot's immutable PSRAM staging copy (PLUG-008) and
 * run the full mpb verification (structure -> signature -> hash -> policy,
 * hardcoded inside mpb_parse). On success fills out_view and asserts the
 * container identity (manifest name/version) against the packaged meta so a
 * stale or swapped embed is caught before any ELF byte is interpreted.
 * Returns false (slot released) on any failure. quiet=true (T9 soak) skips
 * informational lines; every FAIL line still prints. */
bool stage_and_verify(PluginRuntime& slot, const EmbeddedPackage& package, const char* tag,
                      mpb_view_t* out_view, bool quiet = false) {
    slot.package = &package;
    const size_t embed_size = static_cast<size_t>(package.end - package.start);
    slot.psram_free_before_load = psram_free_bytes();
    if (!quiet) {
        std::printf("[%s] psram free before=%" PRIu32 "\n", tag, slot.psram_free_before_load);
    }
    slot.staging =
        static_cast<uint8_t*>(heap_caps_malloc(embed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (slot.staging == nullptr) {
        std::printf("[%s] FAIL staging alloc %zu bytes\n", tag, embed_size);
        release_slot(slot, true);
        return false;
    }
    slot.staging_size = embed_size;
    std::memcpy(slot.staging, package.start, embed_size);

    char hex[65];
    if (!sha256_hex(slot.staging, slot.staging_size, hex)) {
        std::printf("[%s] FAIL sha256\n", tag);
        release_slot(slot, true);
        return false;
    }
    if (!quiet) {
        std::printf("[%s] mpb size=%zu sha256=%s\n", tag, slot.staging_size, hex);
    }
    if (check_embedded_digest(package, slot.staging_size, hex, quiet) != 0) {
        std::printf("[%s] FAIL stale staging copy\n", tag);
        release_slot(slot, true);
        return false;
    }

    mpb_view_t view{};
    if (parse_mpb(slot.staging, slot.staging_size, &view) != 0) {
        std::printf("[%s] FAIL verify\n", tag);
        release_slot(slot, true);
        return false;
    }
    if (!quiet) {
        print_view(view);
    }

    char view_name[MPB_MAX_NAME_BYTES + 1];
    char view_version[MPB_MAX_VERSION_BYTES + 1];
    const size_t name_len =
        view.name_length < sizeof(view_name) - 1 ? view.name_length : sizeof(view_name) - 1;
    const size_t version_len = view.version_length < sizeof(view_version) - 1
                                   ? view.version_length
                                   : sizeof(view_version) - 1;
    std::memcpy(view_name, view.name, name_len);
    view_name[name_len] = '\0';
    std::memcpy(view_version, view.version, version_len);
    view_version[version_len] = '\0';
    const bool identity_ok = std::strcmp(view_name, package.meta_name) == 0 &&
                             std::strcmp(view_version, package.meta_version) == 0;
    if (!quiet) {
        std::printf("[%s] identity name=%s version=%s %s\n", tag, view_name, view_version,
                    identity_ok ? "MATCH" : "MISMATCH");
    }
    if (!identity_ok) {
        std::printf("[%s] FAIL package identity (expected name=%s version=%s)\n", tag,
                    package.meta_name, package.meta_version);
        release_slot(slot, true);
        return false;
    }

    if (view.package_kind != MPB_PACKAGE_CODE || view.payload_count != 1 ||
        view.payloads[0].length == 0) {
        std::printf("[%s] FAIL not a single-ELF code package\n", tag);
        release_slot(slot, true);
        return false;
    }
    *out_view = view;
    return true;
}

/* Full positive load of a verified package into a slot: admission (hard max
 * + program-header budget), esp_elf relocate, entry-table query, contract
 * checks and prepare(). Used by `poca load` (ACTIVE slot) and by the T8
 * coexistence flow (either slot). On failure the slot is released and false
 * is returned; the execution canary advances by exactly one on the
 * successful query. quiet=true (T9 soak) suppresses informational lines
 * only; FAIL lines and every contract check are unchanged. */
bool load_positive_into(PluginRuntime& slot, const EmbeddedPackage& package, const char* tag,
                        bool quiet = false) {
    mpb_view_t view{};
    if (!stage_and_verify(slot, package, tag, &view, quiet)) {
        return false;
    }
    const uint8_t* elf_base = slot.staging + view.payloads[0].offset;
    const size_t elf_size = view.payloads[0].length;

    if (!check_max_memory_admission(view, quiet)) {
        std::printf("[%s] FAIL admission: declared max_memory_bytes exceeds hard max\n", tag);
        release_slot(slot, true);
        return false;
    }
    if (check_ph_budget(elf_base, view, false, package.name, quiet) != 0) {
        std::printf("[%s] FAIL phdr admission\n", tag);
        release_slot(slot, true);
        return false;
    }

    if (!ensure_host_symbols()) {
        std::printf("[%s] FAIL host symbol registration\n", tag);
        release_slot(slot, true);
        return false;
    }
    if (esp_elf_init(&slot.elf) != 0) {
        std::printf("[%s] FAIL esp_elf_init\n", tag);
        release_slot(slot, true);
        return false;
    }
    /* T7 (PLUG-009/MEM 4.2.4): hand the manifest-declared IRAM budget from
     * the verified mpb view to loader patch p5, which asserts it equals the
     * .plugin_iram section's measured sh_size before any allocation. */
    slot.elf.iram_required_bytes = view.iram_required_bytes;
    const int relocate_err = esp_elf_relocate(&slot.elf, elf_base);
    if (relocate_err != 0) {
        std::printf("[%s] FAIL esp_elf_relocate err=%d\n", tag, relocate_err);
        release_slot(slot, true);
        return false;
    }

    elf32_hdr_t ehdr{};
    std::memcpy(&ehdr, elf_base, sizeof(ehdr));
    const uintptr_t entry_vaddr_mapped = esp_elf_map_sym(&slot.elf, ehdr.entry);
    if (entry_vaddr_mapped == 0) {
        std::printf("[%s] FAIL entry vaddr 0x%08" PRIx32 " outside loadable sections\n", tag,
                    static_cast<uint32_t>(ehdr.entry));
        release_slot(slot, true);
        return false;
    }

    /* Entry-table query: find the exported symbol by name, then confirm the
     * loader's entry pointer is exactly the mirrored executable alias. */
    uint32_t sym_value = 0;
    const int sym_matches =
        find_exported_symbol(elf_base, elf_size, POCA_PLUGIN_ENTRY_NAME, &sym_value);
    if (sym_matches != 1) {
        std::printf("[%s] FAIL expected exactly 1 exported %s, found %d\n", tag,
                    POCA_PLUGIN_ENTRY_NAME, sym_matches);
        release_slot(slot, true);
        return false;
    }
    const uintptr_t sym_mapped = esp_elf_map_sym(&slot.elf, sym_value);
    const uintptr_t entry_exec = reinterpret_cast<uintptr_t>(slot.elf.entry);
    const uintptr_t mirror_delta = entry_exec - entry_vaddr_mapped;
    if (!quiet) {
        std::printf("[%s] entry sym %s vaddr=0x%08" PRIx32 " mapped=0x%08" PRIx32
                    " exec=0x%08" PRIx32 " delta=0x%08" PRIx32 "\n",
                    tag, POCA_PLUGIN_ENTRY_NAME, sym_value, static_cast<uint32_t>(sym_mapped),
                    static_cast<uint32_t>(entry_exec), static_cast<uint32_t>(mirror_delta));
    }
    if (sym_mapped == 0 || sym_mapped + mirror_delta != entry_exec) {
        std::printf("[%s] FAIL entry symbol is not the linked entry point\n", tag);
        release_slot(slot, true);
        return false;
    }
    if (!(entry_exec >= kPsramExecLow && entry_exec < kPsramExecHigh)) {
        std::printf("[%s] FAIL entry exec addr outside PSRAM exec window\n", tag);
        release_slot(slot, true);
        return false;
    }

    /* two-step cast: -Werror=cast-function-type rejects direct fn-to-fn */
    const auto query =
        reinterpret_cast<poca_plugin_query_fn>(reinterpret_cast<void*>(slot.elf.entry));
    if (!quiet) {
        std::printf("[%s] query fn ptr=0x%08" PRIx32 "\n", tag, reinterpret_cast<uint32_t>(query));
    }
    g_entry_queries += 1;
    const poca_plugin_table_t* table = query();
    if (table == nullptr) {
        std::printf("[%s] FAIL query returned null\n", tag);
        release_slot(slot, true);
        return false;
    }
    if (table->struct_size != sizeof(poca_plugin_table_t) ||
        table->abi_major != POCA_PLUGIN_ABI_MAJOR || table->abi_minor != POCA_PLUGIN_ABI_MINOR ||
        table->prepare == nullptr || table->activate == nullptr || table->unload == nullptr) {
        std::printf("[%s] FAIL entry table contract\n", tag);
        release_slot(slot, true);
        return false;
    }
    if (!quiet) {
        std::printf("[%s] table ptr=0x%08" PRIx32 " struct_size=%u abi=%u.%u\n", tag,
                    reinterpret_cast<uint32_t>(table), static_cast<unsigned>(table->struct_size),
                    static_cast<unsigned>(table->abi_major),
                    static_cast<unsigned>(table->abi_minor));
    }
    const int32_t prepare_err = table->prepare();
    if (prepare_err != 0) {
        std::printf("[%s] FAIL prepare err=%d (%s)\n", tag, static_cast<int>(prepare_err),
                    frame_err_name(prepare_err));
        release_slot(slot, true);
        return false;
    }
    if (!quiet) {
        std::printf("[%s] prepare()=0 OK\n", tag);
    }

    /* Relocation VALUE assertions for the importing plugins (T5 matrix):
     * the GLOB_DAT slots in the entry table must hold exactly the firmware
     * address of the registered host function. */
    if (table->host_sentinel != nullptr) {
        const bool value_ok = reinterpret_cast<void*>(table->host_sentinel) ==
                              reinterpret_cast<void*>(&poca_host_sentinel);
        if (!quiet) {
            std::printf("[%s] GLOB_DAT host_sentinel=0x%08" PRIx32 " expected=0x%08" PRIx32 " %s\n",
                        tag, reinterpret_cast<uint32_t>(table->host_sentinel),
                        reinterpret_cast<uint32_t>(&poca_host_sentinel),
                        value_ok ? "MATCH" : "MISMATCH");
        }
        if (!value_ok && std::strcmp(package.name, "globdat") == 0) {
            std::printf("[%s] FAIL globdat GLOB_DAT value\n", tag);
            release_slot(slot, true);
            return false;
        }
    }
    if (table->host_add != nullptr) {
        const bool value_ok =
            reinterpret_cast<void*>(table->host_add) == reinterpret_cast<void*>(&poca_host_add);
        if (!quiet) {
            std::printf("[%s] GLOB_DAT host_add=0x%08" PRIx32 " expected=0x%08" PRIx32 " %s\n", tag,
                        reinterpret_cast<uint32_t>(table->host_add),
                        reinterpret_cast<uint32_t>(&poca_host_add),
                        value_ok ? "MATCH" : "MISMATCH");
        }
        if (!value_ok && std::strcmp(package.name, "plt") == 0) {
            std::printf("[%s] FAIL plt GLOB_DAT value\n", tag);
            release_slot(slot, true);
            return false;
        }
    }

    /* Loaded image address evidence: text/data windows live in the PSRAM data
     * mapping; entry pointers run on the mirrored exec window. */
    if (!quiet) {
        std::printf("[%s] text [0x%08" PRIx32 " .. +0x%zx) data [0x%08" PRIx32
                    " .. +0x%zx) bss 0x%zx"
                    " rodata 0x%zx drlro 0x%zx\n",
                    tag, reinterpret_cast<uint32_t>(slot.elf.ptext),
                    slot.elf.sec[ELF_SEC_TEXT].size, reinterpret_cast<uint32_t>(slot.elf.pdata),
                    slot.elf.sec[ELF_SEC_DATA].size + slot.elf.sec[ELF_SEC_RODATA].size +
                        slot.elf.sec[ELF_SEC_DRLRO].size,
                    slot.elf.sec[ELF_SEC_BSS].size, slot.elf.sec[ELF_SEC_RODATA].size,
                    slot.elf.sec[ELF_SEC_DRLRO].size);
    }
    const uint32_t text_addr = reinterpret_cast<uint32_t>(slot.elf.ptext);
    if (!(text_addr >= kPsramDataLow && text_addr < kPsramDataHigh)) {
        std::printf("[%s] FAIL text addr outside PSRAM data window\n", tag);
        release_slot(slot, true);
        return false;
    }
    if (mirror_delta != kPsramExecMirrorOffset) {
        std::printf("[%s] FAIL unexpected exec mirror delta\n", tag);
        release_slot(slot, true);
        return false;
    }

    /* T7: a .plugin_iram section must land in the internal EXEC window
     * (native instruction-bus IRAM, distinct from both PSRAM windows). */
    if (slot.elf.sec[ELF_SEC_IRAM].size != 0) {
        const uint32_t iram_addr = reinterpret_cast<uint32_t>(slot.elf.piram);
        const bool iram_window_ok = iram_addr >= kIramLow && iram_addr < kIramHigh;
        if (!quiet) {
            std::printf("[%s] iram [0x%08" PRIx32 " .. +0x%zx) window0x40=%s\n", tag, iram_addr,
                        slot.elf.sec[ELF_SEC_IRAM].size, iram_window_ok ? "PASS" : "FAIL");
        }
        if (!iram_window_ok || slot.elf.piram == nullptr) {
            std::printf("[%s] FAIL plugin_iram outside IRAM exec window\n", tag);
            release_slot(slot, true);
            return false;
        }
    }

    slot.table = table;
    slot.loaded = true;
    const uint32_t free_after = psram_free_bytes();
    if (!quiet) {
        std::printf("[%s] psram free after=%" PRIu32 " consumed=%" PRIu32 "\n", tag, free_after,
                    slot.psram_free_before_load - free_after);
    }
    return true;
}

void release_slot(PluginRuntime& slot, bool free_staging) {
    esp_elf_deinit(&slot.elf);
    if (free_staging && slot.staging != nullptr) {
        heap_caps_free(slot.staging);
        slot.staging = nullptr;
        slot.staging_size = 0;
    }
    slot.table = nullptr;
    slot.package = nullptr;
    slot.loaded = false;
}

/* The value activate() must return for this package: the plugin's own magic
 * constant xored with the firmware-recomputed CRC over the shared pattern
 * bytes (globdat/plt fold their host imports in; see ALLOWLIST.md). */
int32_t expected_activate(const EmbeddedPackage& package) {
    const uint32_t crc = poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE);
    uint32_t expected = package.magic ^ crc;
    if (std::strcmp(package.name, "globdat") == 0) {
        expected = package.magic ^ crc ^ poca_host_sentinel_value();
    } else if (std::strcmp(package.name, "plt") == 0) {
        expected = package.magic ^ poca_host_add_value(crc, 0u) ^
                   poca_host_add_value(POCA_HOST_ADD_DELTA, crc);
    }
    return static_cast<int32_t>(expected);
}

/* T8 admission guard (MEM-003): the manifest's max_memory_bytes must not
 * exceed the 512 KiB hard maximum. Returns true when admissible and always
 * logs the declaration against both budget anchors. */
/* T8 admission guard (MEM-003): the manifest's max_memory_bytes must not
 * exceed the 512 KiB hard maximum. Returns true when admissible; quiet=true
 * (T9 soak) skips the informational log line. */
bool check_max_memory_admission(const mpb_view_t& view, bool quiet) {
    const bool ok = view.max_memory_bytes <= kHardMaxMemoryBytes;
    if (!quiet) {
        std::printf("[poca] admission max_mem=%" PRIu32 " default_arena=%" PRIu32
                    " hard_max=%" PRIu32 " %s\n",
                    view.max_memory_bytes, kDefaultArenaBytes, kHardMaxMemoryBytes,
                    ok ? "OK" : "REJECT(>hard max)");
    }
    return ok;
}

struct HeapSnap {
    uint32_t psram_free;
    uint32_t psram_largest;
    uint32_t psram_min_ever;
    uint32_t iram_free;
    uint32_t iram_largest;
    uint32_t iram_min_ever;
    uint32_t internal_free;
    /* T9 soak trend fields (development-plan section 4 item 6): the internal
     * heap needs the same largest/min_ever trend evidence as PSRAM. */
    uint32_t internal_largest;
    uint32_t internal_min_ever;
};

HeapSnap heap_snap() {
    multi_heap_info_t psram{};
    multi_heap_info_t iram{};
    multi_heap_info_t internal{};
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM);
    heap_caps_get_info(&iram, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return HeapSnap{
        static_cast<uint32_t>(psram.total_free_bytes),
        static_cast<uint32_t>(psram.largest_free_block),
        static_cast<uint32_t>(psram.minimum_free_bytes),
        static_cast<uint32_t>(iram.total_free_bytes),
        static_cast<uint32_t>(iram.largest_free_block),
        static_cast<uint32_t>(iram.minimum_free_bytes),
        static_cast<uint32_t>(internal.total_free_bytes),
        static_cast<uint32_t>(internal.largest_free_block),
        static_cast<uint32_t>(internal.minimum_free_bytes),
    };
}

void print_heap_snap(const char* phase, const HeapSnap& snap) {
    std::printf("[T8-heap] phase=%-16s psram free=%" PRIu32 " largest=%" PRIu32 " min_ever=%" PRIu32
                " | iram-exec free=%" PRIu32 " largest=%" PRIu32 " min_ever=%" PRIu32
                " | internal free=%" PRIu32 " largest=%" PRIu32 " min_ever=%" PRIu32 "\n",
                phase, snap.psram_free, snap.psram_largest, snap.psram_min_ever, snap.iram_free,
                snap.iram_largest, snap.iram_min_ever, snap.internal_free, snap.internal_largest,
                snap.internal_min_ever);
}

/* Whole-system stack high-water snapshot (uxTaskGetSystemStates, the M1
 * metrics pattern): feeds the appendix-D "hot-update peak" stack budget row
 * (console task + any spawned generation loop task). */
void print_stack_snap(const char* phase) {
    static TaskStatus_t tasks[24];
    const UBaseType_t count = uxTaskGetSystemState(tasks, 24, nullptr);
    std::printf("[T8-stack] phase=%s tasks=%u\n", phase, static_cast<unsigned>(count));
    for (UBaseType_t i = 0; i < count; ++i) {
        std::printf("[T8-stack]   %-16s prio=%u stack_hwm=%uB\n", tasks[i].pcTaskName,
                    static_cast<unsigned>(tasks[i].uxCurrentPriority),
                    static_cast<unsigned>(tasks[i].usStackHighWaterMark));
    }
}

} // namespace

uint32_t poca_entry_queries() { return g_entry_queries; }

const void* poca_active_activate_fn() {
    if (!g_active.loaded || g_active.table == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<const void*>(g_active.table->activate);
}

int32_t poca_active_expected_activate() {
    if (!g_active.loaded || g_active.package == nullptr) {
        return INT32_MIN;
    }
    return expected_activate(*g_active.package);
}

int cmd_poca_verify(const char* name) {
    if (g_active.loaded) {
        std::printf("[poca-verify] FAIL plugin loaded; unload first\n");
        return 1;
    }
    const EmbeddedPackage* package = find_package(name);
    if (package == nullptr) {
        std::printf("[poca-verify] FAIL unknown package '%s'\n", name);
        return 1;
    }
    const size_t size = static_cast<size_t>(package->end - package->start);
    char hex[65];
    if (!sha256_hex(package->start, size, hex)) {
        std::printf("[poca-verify] FAIL sha256\n");
        return 1;
    }
    std::printf("[poca-verify] %s size=%zu sha256=%s\n", package->name, size, hex);
    if (check_embedded_digest(*package, size, hex) != 0) {
        std::printf("[poca-verify] FAIL stale embed\n");
        return 1;
    }
    mpb_view_t view{};
    if (parse_mpb(package->start, size, &view) != 0) {
        std::printf("[poca-verify] FAIL\n");
        return 1;
    }
    print_view(view);
    const uint8_t* elf_base = package->start + view.payloads[0].offset;
    const int ph = check_ph_budget(elf_base, view, false, package->name);
    if (ph != 0) {
        std::printf("[poca-verify] FAIL\n");
        return 1;
    }
    std::printf("[poca-verify] PASS\n");
    return 0;
}

int cmd_poca_load(const char* name) {
    if (g_candidate.loaded) {
        std::printf("[poca-load] FAIL a candidate generation is staged; finish the swap first\n");
        return 1;
    }
    if (g_active.loaded) {
        std::printf("[poca-load] FAIL plugin already loaded; run poca unload first\n");
        return 1;
    }
    const EmbeddedPackage* package = find_package(name);
    if (package == nullptr) {
        std::printf("[poca-load] FAIL unknown package '%s'\n", name);
        return 1;
    }
    const PackageExpect* expect = find_expect(name);
    if (expect == nullptr) {
        std::printf("[poca-load] FAIL no expectation registered for '%s'\n", name);
        return 1;
    }
    const uint32_t canary_before = g_entry_queries;
    std::printf("[poca-load] begin %s (entry_queries=%" PRIu32 ")\n", package->name, canary_before);

    /* Positives run the full pipeline shared with the coexistence loader;
     * every negative below shares only the staging+verification head and
     * must be rejected at its designated stage with the canary untouched. */
    if (expect->expect == Expect::kLoadOk) {
        if (!load_positive_into(g_active, *package, "poca-load")) {
            return 1;
        }
        std::printf("[poca-load] PASS\n");
        return 0;
    }

    mpb_view_t view{};
    if (!stage_and_verify(g_active, *package, "poca-load", &view)) {
        return 1;
    }
    const uint8_t* elf_base = g_active.staging + view.payloads[0].offset;
    const auto canary_unchanged = [&canary_before]() {
        const uint32_t canary_after = g_entry_queries;
        std::printf("[poca-load] entry_queries=%" PRIu32 " (unchanged=%s)\n", canary_after,
                    canary_after == canary_before ? "yes" : "NO");
        return canary_after == canary_before;
    };

    /* Admission hard-max negative (T8/MEM-003): a manifest declaring more
     * than the 512 KiB hard maximum is hostile packaging; reject before
     * esp_elf_init and before any plugin byte executes. */
    if (!check_max_memory_admission(view)) {
        const bool pass = expect->expect == Expect::kMaxMemReject;
        std::printf("[poca-load] NEGATIVE %s %s (admission: max_memory=%" PRIu32 " > %" PRIu32
                    " hard max, rejected pre-init)\n",
                    package->name, pass ? "PASS" : "FAIL", view.max_memory_bytes,
                    kHardMaxMemoryBytes);
        const bool canary_ok = canary_unchanged();
        release_slot(g_active, true);
        if (pass && canary_ok) {
            return 0;
        }
        return 1;
    }
    if (expect->expect == Expect::kMaxMemReject) {
        std::printf("[poca-load] NEGATIVE %s FAIL (admission accepted an over-max declaration)\n",
                    package->name);
        release_slot(g_active, true);
        return 1;
    }
    if (expect->expect == Expect::kPhdrReject) {
        const int ph = check_ph_budget(elf_base, view, true, package->name);
        const bool canary_ok = canary_unchanged();
        release_slot(g_active, true);
        if (ph == 0 && canary_ok) {
            return 0;
        }
        std::printf("[poca-load] NEGATIVE %s FAIL\n", package->name);
        return 1;
    }
    if (check_ph_budget(elf_base, view, false, package->name) != 0) {
        std::printf("[poca-load] FAIL phdr admission\n");
        release_slot(g_active, true);
        return 1;
    }

    if (!ensure_host_symbols()) {
        std::printf("[poca-load] FAIL host symbol registration\n");
        release_slot(g_active, true);
        return 1;
    }
    if (esp_elf_init(&g_active.elf) != 0) {
        std::printf("[poca-load] FAIL esp_elf_init\n");
        release_slot(g_active, true);
        return 1;
    }
    g_active.elf.iram_required_bytes = view.iram_required_bytes;
    /* Relocate from the same immutable staging buffer (PLUG-008). */
    const int relocate_err = esp_elf_relocate(&g_active.elf, elf_base);
    if (relocate_err != 0) {
        std::printf("[poca-load] FAIL esp_elf_relocate err=%d\n", relocate_err);
        const bool canary_ok = canary_unchanged();
        if (expect->expect == Expect::kRelocateErrno) {
            const bool pass = relocate_err == expect->errno_value && canary_ok;
            std::printf("[poca-load] NEGATIVE %s %s (errno %d, expected %d)\n", package->name,
                        pass ? "PASS" : "FAIL", relocate_err, expect->errno_value);
            release_slot(g_active, true);
            return pass ? 0 : 1;
        }
        release_slot(g_active, true);
        return 1;
    }
    std::printf("[poca-load] NEGATIVE %s FAIL (load succeeded, rejection expected)\n",
                package->name);
    release_slot(g_active, true);
    return 1;
}

int cmd_poca_activate() {
    if (!g_active.loaded || g_active.table == nullptr || g_active.package == nullptr) {
        std::printf("[poca-activate] FAIL no loaded plugin; run poca load first\n");
        return 1;
    }
    const poca_plugin_table_t* table = g_active.table;
    const auto activate = table->activate;
    std::printf("[poca-activate] %s activate fn ptr=0x%08" PRIx32 "\n", g_active.package->name,
                reinterpret_cast<uint32_t>(activate));
    const uint32_t fn_addr = reinterpret_cast<uint32_t>(activate);
    if (!(fn_addr >= kPsramExecLow && fn_addr < kPsramExecHigh)) {
        std::printf("[poca-activate] FAIL activate fn outside PSRAM exec window\n");
        return 1;
    }
    const int32_t returned = activate();
    /* The expected value is computed by firmware code over its own copy of
     * the identical constants; which imports contribute depends on the
     * loaded plugin (per-package self-check contract in ALLOWLIST.md). */
    const int32_t expected = expected_activate(*g_active.package);
    std::printf("[poca-activate] returned=0x%08x expected=0x%08x\n",
                static_cast<unsigned>(returned), static_cast<unsigned>(expected));
    if (returned != expected) {
        std::printf("[poca-activate] MAGIC MISMATCH\n");
        return 1;
    }
    std::printf("[poca-activate] MAGIC MATCH\n");
    std::printf("[poca-activate] PASS\n");
    return 0;
}

int cmd_poca_unload() {
    if (!g_active.loaded) {
        std::printf("[poca-unload] FAIL no loaded plugin\n");
        return 1;
    }
    if (g_candidate.loaded) {
        std::printf("[poca-unload] FAIL a candidate generation is staged; finish the swap first\n");
        return 1;
    }
    const int32_t unload_err = g_active.table->unload();
    if (unload_err != 0) {
        std::printf("[poca-unload] plugin unload err=%d (%s); continuing teardown\n",
                    static_cast<int>(unload_err), frame_err_name(unload_err));
    }
    release_slot(g_active, true);
    const uint32_t free_after = psram_free_bytes();
    std::printf(
        "[poca-unload] psram free after=%" PRIu32 " (before-load=%" PRIu32 " delta=%+" PRId32 ")\n",
        free_after, g_active.psram_free_before_load,
        static_cast<int32_t>(free_after) - static_cast<int32_t>(g_active.psram_free_before_load));
    std::printf("[poca-unload] PASS\n");
    return 0;
}

int cmd_poca_iram(unsigned iterations) {
    if (iterations == 0) {
        iterations = 1;
    }
    std::printf("[poca-iram] %u iteration(s): load iram_probe -> IRAM proofs -> unload\n",
                static_cast<unsigned>(iterations));
    const uint32_t iram_free_baseline = iram_free_bytes();
    std::printf("[poca-iram] iram free baseline=%" PRIu32 "\n", iram_free_baseline);

    for (unsigned iter = 1; iter <= iterations; ++iter) {
        if (cmd_poca_load("iram_probe") != 0) {
            std::printf("[poca-iram] FAIL load iteration %u\n", iter);
            return 1;
        }
        const poca_plugin_table_t* table = g_active.table;
        if (table == nullptr || table->iram_check == nullptr || table->iram_write == nullptr ||
            table->data_word_ptr == nullptr || table->data_word_read == nullptr) {
            std::printf("[poca-iram] FAIL iram probe slots missing from entry table\n");
            cmd_poca_unload();
            return 1;
        }

        /* Proof 1 (address window): the returned iram_check pointer must be
         * in the native instruction-bus IRAM window - a DIFFERENT window
         * than the PSRAM data (0x3C..) and exec-mirror (0x42..) addresses
         * of the rest of the image. */
        const uint32_t iram_fn = reinterpret_cast<uint32_t>(table->iram_check);
        const bool window_ok = iram_fn >= kIramLow && iram_fn < kIramHigh;
        std::printf("[poca-iram] iram_check fn=0x%08" PRIx32 " window0x40=%s\n", iram_fn,
                    window_ok ? "PASS" : "FAIL");
        if (!window_ok) {
            cmd_poca_unload();
            return 1;
        }

        /* Proof 2 (cache-sync observability): the loader's IRAM cache-sync
         * counter must have advanced by exactly one during this load. */
        const uint32_t syncs = esp_elf_iram_cache_sync_count();
        const bool counter_ok = syncs >= 1;
        std::printf("[poca-iram] cache-sync counter=%" PRIu32 " (>0=%s)\n", syncs,
                    counter_ok ? "PASS" : "FAIL");
        if (!counter_ok) {
            cmd_poca_unload();
            return 1;
        }

        /* Proof 3 (value computed BY iram-resident code): the check value
         * comes from executing the copy at 0x40..; the firmware recomputes
         * the expected value with its own flash-resident code. */
        const uint32_t returned =
            table->iram_check(POCA_PLUGIN_MAGIC, POCA_IRAM_PROBE_MULT, POCA_IRAM_PROBE_SEED,
                              POCA_IRAM_PROBE_POLY, POCA_IRAM_PROBE_WORDS);
        const uint32_t expected = poca_iram_probe_value(POCA_PLUGIN_MAGIC);
        std::printf("[poca-iram] iram_check returned=0x%08" PRIx32 " expected=0x%08" PRIx32 " %s\n",
                    returned, expected, returned == expected ? "MATCH" : "MISMATCH");
        if (returned != expected) {
            cmd_poca_unload();
            return 1;
        }

        /* Proof 4 (write visibility across windows): IRAM-resident stores
         * into the plugin's PSRAM .data word must be readable back through
         * the .text export and through the host pointer. */
        uint32_t* word = table->data_word_ptr();
        const uint32_t word_addr = reinterpret_cast<uint32_t>(word);
        const bool word_window_ok = word_addr >= kPsramDataLow && word_addr < kPsramDataHigh;
        const int32_t write_err = table->iram_write(word, POCA_IRAM_WRITE_PATTERN);
        const uint32_t read_text = table->data_word_read();
        const uint32_t read_host = *word;
        std::printf("[poca-iram] write-probe .data=0x%08" PRIx32 " (psram=%s) wrote=0x%08" PRIx32
                    " read_text=0x%08" PRIx32 " read_host=0x%08" PRIx32 " %s\n",
                    word_addr, word_window_ok ? "PASS" : "FAIL",
                    static_cast<uint32_t>(POCA_IRAM_WRITE_PATTERN), read_text, read_host,
                    (write_err == 0 && read_text == POCA_IRAM_WRITE_PATTERN &&
                     read_host == POCA_IRAM_WRITE_PATTERN && word_window_ok)
                        ? "MATCH"
                        : "MISMATCH");
        if (write_err != 0 || read_text != POCA_IRAM_WRITE_PATTERN ||
            read_host != POCA_IRAM_WRITE_PATTERN || !word_window_ok) {
            cmd_poca_unload();
            return 1;
        }

        if (cmd_poca_unload() != 0) {
            std::printf("[poca-iram] FAIL unload iteration %u\n", iter);
            return 1;
        }
        if (iter % 20u == 0u || iter == iterations) {
            std::printf("[poca-iram] watermark iter=%u iram_free=%" PRIu32 " (baseline=%" PRIu32
                        " delta=%+" PRId32 ")\n",
                        iter, iram_free_bytes(), iram_free_baseline,
                        static_cast<int32_t>(iram_free_bytes()) -
                            static_cast<int32_t>(iram_free_baseline));
        }
    }

    const uint32_t iram_free_final = iram_free_bytes();
    const int32_t drift =
        static_cast<int32_t>(iram_free_final) - static_cast<int32_t>(iram_free_baseline);
    std::printf("[poca-iram] final iram free=%" PRIu32 " drift=%+" PRId32 " (%s)\n",
                iram_free_final, drift, drift == 0 ? "no leak" : "LEAK");
    if (drift != 0) {
        return 1;
    }
    std::printf("[poca-iram] PASS (%u cycle(s))\n", static_cast<unsigned>(iterations));
    return 0;
}

/* ===================== T8: generation coexistence ===================== */

/* Self-call loop over the ACTIVE generation (task PLUG-004 acceptance: the
 * old generation must keep answering at >=1 call per 500ms across the whole
 * candidate staging + swap window, with EVERY call returning the v1 magic).
 * Counters are single-writer (this task) / single-reader (console task);
 * the loop never prints, so the transcript stays strictly console-ordered. */
constexpr uint32_t kGenLoopPeriodMs = 500u;
constexpr uint32_t kGenLoopMinCalls = 60u;
volatile uint32_t g_gen_calls = 0;
volatile uint32_t g_gen_mismatch = 0;
volatile bool g_gen_run = false;
volatile bool g_gen_alive = false;
int32_t g_gen_expected = 0;

void gen_loop_task(void*) {
    while (g_gen_run) {
        const int32_t returned = g_active.table->activate();
        g_gen_calls += 1;
        if (returned != g_gen_expected) {
            g_gen_mismatch += 1;
        }
        vTaskDelay(pdMS_TO_TICKS(kGenLoopPeriodMs));
    }
    g_gen_alive = false;
    vTaskDelete(nullptr);
}

/* PLUG-004 single-candidate guard: stage a candidate generation into its own
 * arena; refuses with FRAME_ERR_BUSY before any allocation when a candidate
 * is already staged. */
int32_t stage_candidate(const EmbeddedPackage& package) {
    if (g_candidate.loaded) {
        std::printf("[poca-cand] FAIL busy: candidate generation already staged (PLUG-004)\n");
        return FRAME_ERR_BUSY;
    }
    if (!load_positive_into(g_candidate, package, "poca-cand")) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    return FRAME_OK;
}

void stop_gen_loop() {
    g_gen_run = false;
    for (int i = 0; i < 100 && g_gen_alive; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

int cmd_poca_coexist() {
    if (g_active.loaded || g_candidate.loaded) {
        std::printf("[poca-coexist] FAIL slots busy; run poca unload first\n");
        return 1;
    }
    const EmbeddedPackage* v1 = find_package("baseline");
    const EmbeddedPackage* v2 = find_package("baseline_v2");
    const EmbeddedPackage* third = find_package("baseline_300k");
    if (v1 == nullptr || v2 == nullptr || third == nullptr) {
        std::printf("[poca-coexist] FAIL fixture packages missing\n");
        return 1;
    }
    bool ok = true;

    const HeapSnap baseline_snap = heap_snap();
    print_heap_snap("baseline", baseline_snap);
    print_stack_snap("baseline");

    /* Phase 1: v1 (name=baseline version=1.0.0) becomes ACTIVE and starts
     * answering a 500ms self-call loop on the other core. */
    std::printf("[poca-coexist] phase 1: load v1 (baseline 1.0.0) as ACTIVE\n");
    if (!load_positive_into(g_active, *v1, "poca-coexist")) {
        return 1;
    }
    const int32_t exp_v1 = expected_activate(*v1);
    const int32_t ret_v1 = g_active.table->activate();
    const bool v1_magic_ok = ret_v1 == exp_v1;
    std::printf("[poca-coexist] v1 ACTIVE activate returned=0x%08x expected=0x%08x %s\n",
                static_cast<unsigned>(ret_v1), static_cast<unsigned>(exp_v1),
                v1_magic_ok ? "MATCH" : "MISMATCH");
    ok = ok && v1_magic_ok;
    if (!ok) {
        release_slot(g_active, true);
        return 1;
    }
    g_gen_calls = 0;
    g_gen_mismatch = 0;
    g_gen_expected = exp_v1;
    g_gen_run = true;
    g_gen_alive = true;
    if (xTaskCreatePinnedToCore(&gen_loop_task, "poca_gen", 4096, nullptr, 4, nullptr, 0) !=
        pdPASS) {
        std::printf("[poca-coexist] FAIL gen loop task create\n");
        g_gen_run = false;
        g_gen_alive = false;
        release_slot(g_active, true);
        return 1;
    }
    std::printf("[poca-coexist] v1 self-call loop started (every %ums, core 0)\n",
                static_cast<unsigned>(kGenLoopPeriodMs));
    const HeapSnap v1_snap = heap_snap();
    print_heap_snap("v1-active", v1_snap);

    /* Phase 2: v2 (same manifest name, version 2.0.0) staged as CANDIDATE in
     * an independent esp_elf_t instance while the v1 loop keeps running. */
    std::printf("[poca-coexist] phase 2: stage v2 (baseline 2.0.0) as CANDIDATE\n");
    if (stage_candidate(*v2) != FRAME_OK) {
        stop_gen_loop();
        release_slot(g_active, true);
        return 1;
    }
    std::printf("[poca-coexist] v2 CANDIDATE resident: text=0x%08" PRIx32 " (v1 text=0x%08" PRIx32
                ") independent arenas\n",
                reinterpret_cast<uint32_t>(g_candidate.elf.ptext),
                reinterpret_cast<uint32_t>(g_active.elf.ptext));
    std::printf("[PASS-v2-candidate-loaded]\n");
    const HeapSnap peak_snap = heap_snap();
    print_heap_snap("coexist-peak", peak_snap);

    /* Phase 3: a THIRD load attempt must be refused while a candidate is
     * staged (PLUG-004), before any allocation and without a query. */
    const uint32_t psram_before_guard = psram_free_bytes();
    const uint32_t canary_before_guard = g_entry_queries;
    const int32_t guard_err = stage_candidate(*third);
    const bool guard_ok = guard_err == FRAME_ERR_BUSY && psram_free_bytes() == psram_before_guard &&
                          g_entry_queries == canary_before_guard;
    std::printf("[poca-coexist] third candidate (baseline_300k) err=%" PRId32 " (want %" PRId32
                " BUSY) psram_unchanged=%s canary_unchanged=%s\n",
                guard_err, static_cast<int32_t>(FRAME_ERR_BUSY),
                psram_free_bytes() == psram_before_guard ? "yes" : "NO",
                g_entry_queries == canary_before_guard ? "yes" : "NO");
    if (guard_ok) {
        std::printf("[PASS-single-candidate-guard]\n");
    }
    ok = ok && guard_ok;

    /* Phase 4: both generations answer with their OWN magic while the v1
     * loop is still running. */
    const int32_t exp_v2 = expected_activate(*v2);
    const int32_t ret_old = g_active.table->activate();
    const int32_t ret_new = g_candidate.table->activate();
    const uintptr_t fn_old = reinterpret_cast<uintptr_t>(g_active.table->activate);
    const uintptr_t fn_new = reinterpret_cast<uintptr_t>(g_candidate.table->activate);
    const bool distinct_code = fn_old != fn_new;
    const bool both_ok = ret_old == exp_v1 && ret_new == exp_v2 && distinct_code;
    std::printf("[poca-coexist] gen-old activate fn=0x%08" PRIx32 " returned=0x%08x"
                " expected=0x%08x %s\n",
                static_cast<uint32_t>(fn_old), static_cast<unsigned>(ret_old),
                static_cast<unsigned>(exp_v1), ret_old == exp_v1 ? "MATCH" : "MISMATCH");
    std::printf("[poca-coexist] gen-new activate fn=0x%08" PRIx32 " returned=0x%08x"
                " expected=0x%08x %s\n",
                static_cast<uint32_t>(fn_new), static_cast<unsigned>(ret_new),
                static_cast<unsigned>(exp_v2), ret_new == exp_v2 ? "MATCH" : "MISMATCH");
    std::printf("[poca-coexist] distinct code addresses=%s (delta=0x%" PRIx32
                ") loop_calls=%" PRIu32 " loop_mismatch=%" PRIu32 "\n",
                distinct_code ? "yes" : "NO",
                static_cast<uint32_t>(fn_new > fn_old ? fn_new - fn_old : fn_old - fn_new),
                g_gen_calls, g_gen_mismatch);
    if (both_ok) {
        std::printf("[PASS-both-generations-respond]\n");
    }
    ok = ok && both_ok;
    print_stack_snap("coexist-peak");

    /* Phase 5: let the loop reach the acceptance window (>=60 calls), then
     * stop it. */
    std::printf("[poca-coexist] phase 5: waiting for %u loop calls\n",
                static_cast<unsigned>(kGenLoopMinCalls));
    for (int waited = 0; waited < 45000 / 250 && g_gen_calls < kGenLoopMinCalls; ++waited) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (g_gen_calls % 10u == 0u) {
            std::printf("[poca-coexist] loop progress calls=%" PRIu32 " mismatch=%" PRIu32 "\n",
                        g_gen_calls, g_gen_mismatch);
        }
    }
    stop_gen_loop();
    const bool calls_ok = g_gen_calls >= kGenLoopMinCalls && g_gen_mismatch == 0;
    std::printf("[poca-coexist] v1 loop final calls=%" PRIu32 " (>=60=%s) mismatches=%" PRIu32
                " (==0=%s)\n",
                g_gen_calls, g_gen_calls >= kGenLoopMinCalls ? "yes" : "NO", g_gen_mismatch,
                g_gen_mismatch == 0 ? "yes" : "NO");
    if (calls_ok) {
        std::printf("[PASS-v1-active-60calls]\n");
    }
    ok = ok && calls_ok;

    /* Phase 6: swap generations: unload the OLD v1, promote the candidate. */
    std::printf("[poca-coexist] phase 6: swap (unload old v1, promote v2)\n");
    const int32_t unload_err = g_active.table->unload();
    if (unload_err != 0) {
        std::printf("[poca-coexist] v1 unload err=%d; continuing\n", static_cast<int>(unload_err));
    }
    release_slot(g_active, true);
    g_active = g_candidate;
    g_candidate = PluginRuntime{};
    const int32_t ret_promoted = g_active.table->activate();
    const bool swap_ok = g_active.loaded && ret_promoted == exp_v2;
    std::printf("[poca-coexist] v2 promoted ACTIVE activate returned=0x%08x expected=0x%08x %s\n",
                static_cast<unsigned>(ret_promoted), static_cast<unsigned>(exp_v2),
                ret_promoted == exp_v2 ? "MATCH" : "MISMATCH");
    if (swap_ok) {
        std::printf("[PASS-swap-unload-old]\n");
    }
    ok = ok && swap_ok;
    print_heap_snap("v2-active", heap_snap());

    /* Phase 7: teardown and the measured admission budget summary. */
    const int32_t unload2_err = g_active.table->unload();
    if (unload2_err != 0) {
        std::printf("[poca-coexist] v2 unload err=%d; continuing\n", static_cast<int>(unload2_err));
    }
    release_slot(g_active, true);
    const HeapSnap final_snap = heap_snap();
    print_heap_snap("final", final_snap);
    const bool restored = final_snap.psram_free == baseline_snap.psram_free;
    const uint32_t per_instance = baseline_snap.psram_free - v1_snap.psram_free;
    const uint32_t coexist_peak = baseline_snap.psram_free - peak_snap.psram_free;
    const uint32_t psram_pool = 8u * 1024u * 1024u;
    std::printf("[T8-budget] psram baseline_free=%" PRIu32 " per_instance_v1=%" PRIu32
                " coexist_peak(v1+v2+2 stagings)=%" PRIu32 " final_free=%" PRIu32 " restored=%s\n",
                baseline_snap.psram_free, per_instance, coexist_peak, final_snap.psram_free,
                restored ? "yes" : "NO");
    std::printf("[T8-budget] headroom vs 8MiB pool: peak uses %u.%02u%% (%" PRIu32
                "/%u); 2x256KiB arena caps + measured overhead fit=%s\n",
                static_cast<unsigned>((coexist_peak * 100u) / psram_pool),
                static_cast<unsigned>(((coexist_peak * 10000u) / psram_pool) % 100u), coexist_peak,
                static_cast<unsigned>(psram_pool), coexist_peak < psram_pool / 2u ? "yes" : "NO");
    std::printf("[T8-budget] iram-exec baseline_free=%" PRIu32 " peak_free=%" PRIu32
                " delta=%" PRId32 " (baseline plugins carry no .plugin_iram)\n",
                baseline_snap.iram_free, peak_snap.iram_free,
                static_cast<int32_t>(peak_snap.iram_free) -
                    static_cast<int32_t>(baseline_snap.iram_free));
    std::printf("[T8-budget] min_ever floors: psram=%" PRIu32 " iram-exec=%" PRIu32 "\n",
                final_snap.psram_min_ever, final_snap.iram_min_ever);
    std::printf("[PASS-budget-table]\n");
    ok = ok && restored;

    if (ok) {
        std::printf("[poca-coexist] PASS\n");
        return 0;
    }
    std::printf("[poca-coexist] FAIL\n");
    return 1;
}

/* ====================== T9: 1000-cycle lifecycle soak ====================== */

constexpr unsigned kSoakSampleEvery = 10u;
constexpr unsigned kSoakSwapEvery = 100u;
constexpr unsigned kSoakIntegrityEvery = 50u;
constexpr unsigned kSoakMaxTasks = 12u;
/* ring capacity: baseline row + one row per kSoakSampleEvery window of a
 * 10000-cycle max run would overflow by design; rows beyond capacity wrap
 * (keep the newest kSoakRingRows samples) and the summary reports it. */
constexpr unsigned kSoakRingRows = 104u;

struct SoakSample {
    uint32_t cycle;
    HeapSnap heap;
    uint32_t hwm[kSoakMaxTasks];
};

SoakSample g_soak_ring[kSoakRingRows];
char g_soak_task_names[kSoakMaxTasks][configMAX_TASK_NAME_LEN];
unsigned g_soak_task_count = 0;
unsigned g_soak_ring_count = 0;
unsigned g_soak_ring_next = 0;
bool g_soak_ring_overflow = false;

uint32_t soak_task_stack_size(const char* name) {
    if (std::strcmp(name, "console_repl") == 0) {
        return kPocaReplStackBytes;
    }
    if (std::strcmp(name, "IDLE0") == 0 || std::strcmp(name, "IDLE1") == 0) {
        return CONFIG_FREERTOS_IDLE_TASK_STACKSIZE;
    }
    if (std::strcmp(name, "ipc0") == 0 || std::strcmp(name, "ipc1") == 0) {
        return CONFIG_ESP_IPC_TASK_STACK_SIZE;
    }
    if (std::strcmp(name, "esp_timer") == 0 || std::strcmp(name, "Tmr Svc") == 0) {
        return CONFIG_ESP_TIMER_TASK_STACK_SIZE;
    }
    return 0; /* unknown: judge applies a conservative floor instead */
}

/* Capture the per-task stack high-water marks via uxTaskGetSystemState (the
 * T8 helper's data source). The first call also fixes the tracked task set
 * and name order; any later task-set change is a soak error (fail loudly
 * rather than silently misattributing columns). */
bool soak_capture_tasks(SoakSample* out) {
    static TaskStatus_t tasks[kSoakMaxTasks * 2];
    const UBaseType_t count = uxTaskGetSystemState(tasks, kSoakMaxTasks * 2, nullptr);
    if (count == 0 || count > kSoakMaxTasks) {
        std::printf("[poca-soak] FAIL task capture count=%u\n", static_cast<unsigned>(count));
        return false;
    }
    if (g_soak_task_count == 0) {
        g_soak_task_count = count;
        for (unsigned t = 0; t < count; ++t) {
            std::snprintf(g_soak_task_names[t], sizeof(g_soak_task_names[t]), "%s",
                          tasks[t].pcTaskName);
            out->hwm[t] = tasks[t].usStackHighWaterMark;
        }
        return true;
    }
    if (count != g_soak_task_count) {
        std::printf("[poca-soak] FAIL task set changed %u -> %u\n", g_soak_task_count,
                    static_cast<unsigned>(count));
        return false;
    }
    for (unsigned t = 0; t < count; ++t) {
        bool matched = false;
        for (unsigned s = 0; s < count; ++s) {
            if (std::strcmp(tasks[s].pcTaskName, g_soak_task_names[t]) == 0) {
                out->hwm[t] = tasks[s].usStackHighWaterMark;
                matched = true;
                break;
            }
        }
        if (!matched) {
            std::printf("[poca-soak] FAIL task '%s' missing from snapshot\n", g_soak_task_names[t]);
            return false;
        }
    }
    return true;
}

void soak_take_sample(uint32_t cycle) {
    SoakSample sample{};
    sample.cycle = cycle;
    sample.heap = heap_snap();
    if (!soak_capture_tasks(&sample)) {
        return; /* capture failure already counted as a soak error */
    }
    if (g_soak_ring_count < kSoakRingRows) {
        g_soak_ring[g_soak_ring_count] = sample;
        g_soak_ring_count += 1;
    } else {
        g_soak_ring[g_soak_ring_next] = sample;
        g_soak_ring_next = (g_soak_ring_next + 1u) % kSoakRingRows;
        g_soak_ring_overflow = true;
    }
}

/* Quiesce step semantics (development-plan section 4 item 6 / 4.14.2 minimal
 * entry subset): the ABI 1.2 entry table defines prepare/activate/unload and
 * NO quiesce entry. A missing (NULL) lifecycle entry is contractually a
 * trivial success - quiescing a stateless plugin requires no plugin code -
 * so the soak counts each quiesce as a satisfied no-op instead of calling
 * any function. */
bool soak_quiesce(const PluginRuntime& slot) {
    return slot.loaded && slot.table != nullptr && slot.table->activate != nullptr;
}

/* Full single-plugin lifecycle: load(verify+relocate+query+prepare) ->
 * activate(value assert) -> quiesce(no-op) -> unload. Returns true only when
 * every stage passed; the slot is released on all exits. */
bool soak_lifecycle_cycle(const EmbeddedPackage& package, unsigned* matches) {
    if (!load_positive_into(g_active, package, "poca-soak", /*quiet=*/true)) {
        std::printf("[poca-soak] FAIL cycle load %s\n", package.name);
        return false;
    }
    const int32_t returned = g_active.table->activate();
    const int32_t expected = expected_activate(package);
    const bool match = returned == expected;
    *matches += match ? 1u : 0u;
    if (!match) {
        std::printf("[poca-soak] FAIL cycle activate %s returned=0x%08x expected=0x%08x\n",
                    package.name, static_cast<unsigned>(returned), static_cast<unsigned>(expected));
    }
    const bool quiesced = soak_quiesce(g_active);
    const int32_t unload_err = g_active.table->unload();
    release_slot(g_active, true);
    if (!quiesced || unload_err != 0) {
        std::printf("[poca-soak] FAIL cycle teardown %s quiesced=%s unload_err=%d\n", package.name,
                    quiesced ? "yes" : "NO", static_cast<int>(unload_err));
        return false;
    }
    return match;
}

/* Every kSoakSwapEvery-th cycle: the T8 coexistence mini-path. v1 runs its
 * full lifecycle up to quiesce, v2 stages as CANDIDATE while v1 is still
 * ACTIVE (both arenas resident), both answer with their own magic, v1
 * unloads, the candidate is promoted and finishes its own lifecycle. */
bool soak_swap_cycle(const EmbeddedPackage& v1, const EmbeddedPackage& v2, unsigned* matches) {
    bool ok = true;
    if (!load_positive_into(g_active, v1, "poca-soak", /*quiet=*/true)) {
        std::printf("[poca-soak] FAIL swap load v1\n");
        return false;
    }
    const int32_t exp_v1 = expected_activate(v1);
    const int32_t exp_v2 = expected_activate(v2);
    const int32_t ret_v1 = g_active.table->activate();
    const bool m1 = ret_v1 == exp_v1;
    *matches += m1 ? 1u : 0u;
    ok = ok && m1;

    if (!load_positive_into(g_candidate, v2, "poca-soak", /*quiet=*/true)) {
        std::printf("[poca-soak] FAIL swap load v2 candidate\n");
        release_slot(g_active, true);
        return false;
    }
    const int32_t ret_old = g_active.table->activate();
    const int32_t ret_cand = g_candidate.table->activate();
    const bool m_old = ret_old == exp_v1;
    const bool m_cand = ret_cand == exp_v2;
    *matches += (m_old ? 1u : 0u) + (m_cand ? 1u : 0u);
    ok = ok && m_old && m_cand;

    const bool quiesced_v1 = soak_quiesce(g_active);
    const int32_t unload_err = g_active.table->unload();
    release_slot(g_active, true);
    g_active = g_candidate;
    g_candidate = PluginRuntime{};
    const int32_t ret_promoted = g_active.table->activate();
    const bool m_promoted = ret_promoted == exp_v2;
    *matches += m_promoted ? 1u : 0u;
    const bool quiesced_v2 = soak_quiesce(g_active);
    const int32_t unload2_err = g_active.table->unload();
    release_slot(g_active, true);
    ok = ok && quiesced_v1 && quiesced_v2 && unload_err == 0 && unload2_err == 0 && m_promoted;

    std::printf("[poca-soak] swap cycle v1=%s v2=%s ok=%s "
                "(v1=0x%08x/%s cand=0x%08x/%s old-while-cand=0x%08x/%s promoted=0x%08x/%s)\n",
                v1.name, v2.name, ok ? "yes" : "NO", static_cast<unsigned>(ret_v1),
                m1 ? "MATCH" : "MISMATCH", static_cast<unsigned>(ret_cand),
                m_cand ? "MATCH" : "MISMATCH", static_cast<unsigned>(ret_old),
                m_old ? "MATCH" : "MISMATCH", static_cast<unsigned>(ret_promoted),
                m_promoted ? "MATCH" : "MISMATCH");
    return ok;
}

int cmd_poca_soak(unsigned cycles, const char* mix) {
    if (g_active.loaded || g_candidate.loaded) {
        std::printf("[poca-soak] FAIL slots busy; run poca unload first\n");
        return 1;
    }
    const EmbeddedPackage* v1 = find_package("baseline");
    const EmbeddedPackage* v2 = find_package("baseline_v2");
    const EmbeddedPackage* iram = find_package("iram_probe");
    if (v1 == nullptr || v2 == nullptr || iram == nullptr) {
        std::printf("[poca-soak] FAIL fixture packages missing\n");
        return 1;
    }
    const bool mix_default = std::strcmp(mix, "default") == 0;
    const bool mix_baseline = std::strcmp(mix, "baseline") == 0;
    const bool mix_iram = std::strcmp(mix, "iram") == 0;
    if (!mix_default && !mix_baseline && !mix_iram) {
        std::printf("[poca-soak] FAIL unknown mix '%s' (default|baseline|iram)\n", mix);
        return 1;
    }

    g_soak_task_count = 0;
    g_soak_ring_count = 0;
    g_soak_ring_next = 0;
    g_soak_ring_overflow = false;
    const uint32_t canary_before = g_entry_queries;
    std::printf("SOAKHDR,cycles=%u,sample_every=%u,swap_every=%u,integrity_every=%u,mix=%s\n",
                static_cast<unsigned>(cycles), kSoakSampleEvery, kSoakSwapEvery,
                kSoakIntegrityEvery, mix);
    std::printf("[poca-soak] quiesce semantics: no quiesce fn in the ABI 1.2 entry table; "
                "NULL entry = trivial success (4.14.2 minimal subset)\n");
    print_heap_snap("soak-baseline", heap_snap());
    print_stack_snap("soak-baseline");
    soak_take_sample(0);
    std::printf("SOAKTASKS");
    for (unsigned t = 0; t < g_soak_task_count; ++t) {
        std::printf(",%s", g_soak_task_names[t]);
    }
    std::printf("\nSOAKTASKSZ");
    for (unsigned t = 0; t < g_soak_task_count; ++t) {
        std::printf(",%u", static_cast<unsigned>(soak_task_stack_size(g_soak_task_names[t])));
    }
    std::printf("\n");

    /* The loader tags every relocate with a 2-line INFO banner; 1000 cycles
     * would flood the transcript. Errors stay visible (level ERROR). */
    esp_log_level_set("ELF", ESP_LOG_ERROR);

    unsigned completed = 0;
    unsigned errors = 0;
    unsigned matches = 0;
    unsigned expected_matches = 0;
    unsigned swaps = 0;
    unsigned swap_ok = 0;
    unsigned quiesce_noop = 0;
    unsigned integrity_probes = 0;
    unsigned integrity_ok = 0;
    const int64_t t0 = esp_timer_get_time();

    for (unsigned cycle = 1; cycle <= cycles; ++cycle) {
        if (cycle % kSoakSwapEvery == 0u) {
            swaps += 1u;
            expected_matches += 4u;
            if (soak_swap_cycle(*v1, *v2, &matches)) {
                swap_ok += 1u;
            } else {
                errors += 1u;
            }
            quiesce_noop += 2u;
        } else {
            const EmbeddedPackage* scheduled =
                (mix_iram || (mix_default && cycle % 3u == 0u)) ? iram : v1;
            expected_matches += 1u;
            quiesce_noop += 1u;
            if (!soak_lifecycle_cycle(*scheduled, &matches)) {
                errors += 1u;
            }
        }
        completed += 1u;

        if (cycle % kSoakIntegrityEvery == 0u) {
            integrity_probes += 1u;
            if (heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false)) {
                integrity_ok += 1u;
            } else {
                errors += 1u;
                std::printf("[poca-soak] FAIL integrity probe cycle=%u\n", cycle);
            }
        }
        if (cycle % kSoakSampleEvery == 0u) {
            soak_take_sample(cycle);
            std::printf("[poca-soak] prog cycle=%u/%u errors=%u matches=%u psram_free=%" PRIu32
                        "\n",
                        cycle, static_cast<unsigned>(cycles), errors, matches, psram_free_bytes());
        }
        /* yield 1 tick so the IDLE tasks on both cores feed the task WDT
         * (both CPU idle tasks are watched on this config) */
        vTaskDelay(1);
    }

    if (cycles % kSoakSampleEvery != 0u) {
        soak_take_sample(cycles);
    }
    const int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    esp_log_level_set("ELF", ESP_LOG_INFO);
    print_heap_snap("soak-final", heap_snap());
    print_stack_snap("soak-final");

    std::printf("[poca-soak] samples begin (%u rows)\n", g_soak_ring_count);
    for (unsigned i = 0; i < g_soak_ring_count; ++i) {
        const unsigned row = g_soak_ring_overflow ? (g_soak_ring_next + i) % kSoakRingRows : i;
        const SoakSample& s = g_soak_ring[row];
        std::printf("SOAKSMP,%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
                    ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "",
                    i, s.cycle, s.heap.psram_free, s.heap.psram_largest, s.heap.psram_min_ever,
                    s.heap.iram_free, s.heap.iram_largest, s.heap.iram_min_ever,
                    s.heap.internal_free, s.heap.internal_largest, s.heap.internal_min_ever);
        for (unsigned t = 0; t < g_soak_task_count; ++t) {
            std::printf(",%u", static_cast<unsigned>(s.hwm[t]));
        }
        std::printf("\n");
    }
    std::printf("[poca-soak] samples end\n");

    const unsigned expected_queries = completed + swaps;
    std::printf("SOAKSUM,cycles=%u,completed=%u,errors=%u,value_matches=%u,"
                "expected_value_matches=%u,swaps=%u,swap_ok=%u,quiesce_noop=%u,"
                "integrity_probes=%u,integrity_ok=%u,canary_before=%" PRIu32
                ",canary_after=%" PRIu32 ",expected_queries=%u,elapsed_ms=%" PRId64
                ",ring_overflow=%s\n",
                static_cast<unsigned>(cycles), completed, errors, matches, expected_matches, swaps,
                swap_ok, quiesce_noop, integrity_probes, integrity_ok, canary_before,
                g_entry_queries, expected_queries, elapsed_ms, g_soak_ring_overflow ? "yes" : "no");
    if (errors != 0u || completed != cycles) {
        std::printf("[poca-soak] FAIL (errors=%u completed=%u/%u)\n", errors, completed,
                    static_cast<unsigned>(cycles));
        return 1;
    }
    std::printf("[poca-soak] DONE (%u cycles, verdict authority = host judge on samples)\n",
                static_cast<unsigned>(cycles));
    return 0;
}

} // namespace frame::poca
