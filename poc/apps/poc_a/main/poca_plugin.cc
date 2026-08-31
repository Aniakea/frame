#include "poca_plugin.hh"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "frame_poc/frame_abi.h"
#include "frame_poc/mpb_mbedtls_glue.h"
#include "frame_poc/mpb_parser.h"
#include "plugin_abi.h"
#include "poca_baseline_meta.h"
#include "poca_pubkey.h"
#include "psa/crypto.h"

/* Embedded signed plugin container, produced by the plugins.cmake pipeline
 * and linked in via target_add_binary_data(). The IDF embed wrapper names
 * the symbols from the file name: baseline.mpb -> _binary_baseline_mpb_*. */
extern "C" const uint8_t _binary_baseline_mpb_start[];
extern "C" const uint8_t _binary_baseline_mpb_end[];

namespace frame::poca {
namespace {

/* ESP32-S3 address windows: PSRAM data-bus mapping 0x3C000000..0x3E000000
 * (drom) and its instruction-bus alias used for execution 0x42000000..
 * 0x44000000 (irom = data window + 0x06000000 with ELF_LOADER_CACHE_OFFSET). */
constexpr uint32_t kPsramDataLow = 0x3C000000u;
constexpr uint32_t kPsramDataHigh = 0x3E000000u;
constexpr uint32_t kPsramExecLow = 0x42000000u;
constexpr uint32_t kPsramExecHigh = 0x44000000u;
constexpr uint32_t kPsramExecMirrorOffset = 0x06000000u;

struct PluginRuntime {
    bool loaded = false;
    uint8_t* staging = nullptr;
    size_t staging_size = 0;
    uint32_t psram_free_before_load = 0;
    esp_elf_t elf{};
    const poca_plugin_table_t* table = nullptr;
};

PluginRuntime g_plugin;
mpb_crypto_t g_crypto{};
bool g_crypto_ready = false;

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

int check_embedded_digest(size_t size, const char* hex) {
    const int digest_ok = std::strcmp(hex, POCA_BASELINE_MPB_SHA256) == 0 &&
                          size == static_cast<size_t>(POCA_BASELINE_MPB_SIZE);
    std::printf("[poca] mpb digest %s\n", digest_ok ? "MATCH" : "MISMATCH");
    return digest_ok ? 0 : 1;
}

void release_plugin(bool free_staging) {
    esp_elf_deinit(&g_plugin.elf);
    if (free_staging && g_plugin.staging != nullptr) {
        heap_caps_free(g_plugin.staging);
        g_plugin.staging = nullptr;
        g_plugin.staging_size = 0;
    }
    g_plugin.table = nullptr;
    g_plugin.loaded = false;
}

} // namespace

int cmd_poca_verify() {
    if (g_plugin.loaded) {
        std::printf("[poca-verify] FAIL plugin loaded; unload first\n");
        return 1;
    }
    const size_t size = static_cast<size_t>(_binary_baseline_mpb_end - _binary_baseline_mpb_start);
    char hex[65];
    if (!sha256_hex(_binary_baseline_mpb_start, size, hex)) {
        std::printf("[poca-verify] FAIL sha256\n");
        return 1;
    }
    std::printf("[poca-verify] mpb size=%zu sha256=%s\n", size, hex);
    std::printf("[poca-verify] meta size=%u sha256=%s (build)\n",
                static_cast<unsigned>(POCA_BASELINE_MPB_SIZE), POCA_BASELINE_MPB_SHA256);
    if (check_embedded_digest(size, hex) != 0) {
        std::printf("[poca-verify] FAIL stale embed\n");
        return 1;
    }
    mpb_view_t view{};
    if (parse_mpb(_binary_baseline_mpb_start, size, &view) != 0) {
        std::printf("[poca-verify] FAIL\n");
        return 1;
    }
    print_view(view);
    std::printf("[poca-verify] PASS\n");
    return 0;
}

int cmd_poca_load() {
    if (g_plugin.loaded) {
        std::printf("[poca-load] FAIL plugin already loaded; run poca unload first\n");
        return 1;
    }
    std::printf("[poca-load] begin\n");
    const size_t embed_size =
        static_cast<size_t>(_binary_baseline_mpb_end - _binary_baseline_mpb_start);
    g_plugin.psram_free_before_load = psram_free_bytes();
    std::printf("[poca-load] psram free before=%" PRIu32 "\n", g_plugin.psram_free_before_load);

    /* Immutable staging copy of the embedded container (PLUG-008: never load
     * from the flash mapping directly, never mutate after this memcpy). */
    g_plugin.staging =
        static_cast<uint8_t*>(heap_caps_malloc(embed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_plugin.staging == nullptr) {
        std::printf("[poca-load] FAIL staging alloc %zu bytes\n", embed_size);
        return 1;
    }
    g_plugin.staging_size = embed_size;
    std::memcpy(g_plugin.staging, _binary_baseline_mpb_start, embed_size);

    char hex[65];
    if (!sha256_hex(g_plugin.staging, g_plugin.staging_size, hex)) {
        std::printf("[poca-load] FAIL sha256\n");
        release_plugin(true);
        return 1;
    }
    std::printf("[poca-load] mpb size=%zu sha256=%s\n", g_plugin.staging_size, hex);
    std::printf("[poca-load] meta size=%u sha256=%s (build)\n",
                static_cast<unsigned>(POCA_BASELINE_MPB_SIZE), POCA_BASELINE_MPB_SHA256);
    if (check_embedded_digest(g_plugin.staging_size, hex) != 0) {
        std::printf("[poca-load] FAIL stale staging copy\n");
        release_plugin(true);
        return 1;
    }

    /* Verification order is fixed inside mpb_parse: structure -> signature ->
     * payload hash -> policy (SEC-004); only then is any ELF byte touched. */
    mpb_view_t view{};
    if (parse_mpb(g_plugin.staging, g_plugin.staging_size, &view) != 0) {
        std::printf("[poca-load] FAIL verify\n");
        release_plugin(true);
        return 1;
    }
    print_view(view);
    if (view.package_kind != MPB_PACKAGE_CODE || view.payload_count != 1 ||
        view.payloads[0].length == 0) {
        std::printf("[poca-load] FAIL not a single-ELF code package\n");
        release_plugin(true);
        return 1;
    }

    const uint8_t* elf_base = g_plugin.staging + view.payloads[0].offset;
    const size_t elf_size = view.payloads[0].length;
    if (esp_elf_init(&g_plugin.elf) != 0) {
        std::printf("[poca-load] FAIL esp_elf_init\n");
        release_plugin(true);
        return 1;
    }
    /* Relocate from the same immutable staging buffer (PLUG-008). */
    const int relocate_err = esp_elf_relocate(&g_plugin.elf, elf_base);
    if (relocate_err != 0) {
        std::printf("[poca-load] FAIL esp_elf_relocate err=%d\n", relocate_err);
        release_plugin(true);
        return 1;
    }

    elf32_hdr_t ehdr{};
    std::memcpy(&ehdr, elf_base, sizeof(ehdr));
    const uintptr_t entry_vaddr_mapped = esp_elf_map_sym(&g_plugin.elf, ehdr.entry);
    if (entry_vaddr_mapped == 0) {
        std::printf("[poca-load] FAIL entry vaddr 0x%08" PRIx32 " outside loadable sections\n",
                    static_cast<uint32_t>(ehdr.entry));
        release_plugin(true);
        return 1;
    }

    /* Entry-table query: find the exported symbol by name, then confirm the
     * loader's entry pointer is exactly the mirrored executable alias of it. */
    uint32_t sym_value = 0;
    const int sym_matches =
        find_exported_symbol(elf_base, elf_size, POCA_PLUGIN_ENTRY_NAME, &sym_value);
    if (sym_matches != 1) {
        std::printf("[poca-load] FAIL expected exactly 1 exported %s, found %d\n",
                    POCA_PLUGIN_ENTRY_NAME, sym_matches);
        release_plugin(true);
        return 1;
    }
    const uintptr_t sym_mapped = esp_elf_map_sym(&g_plugin.elf, sym_value);
    const uintptr_t entry_exec = reinterpret_cast<uintptr_t>(g_plugin.elf.entry);
    const uintptr_t mirror_delta = entry_exec - entry_vaddr_mapped;
    std::printf("[poca-load] entry sym %s vaddr=0x%08" PRIx32 " mapped=0x%08" PRIx32
                " exec=0x%08" PRIx32 " delta=0x%08" PRIx32 "\n",
                POCA_PLUGIN_ENTRY_NAME, sym_value, static_cast<uint32_t>(sym_mapped),
                static_cast<uint32_t>(entry_exec), static_cast<uint32_t>(mirror_delta));
    if (sym_mapped == 0 || sym_mapped + mirror_delta != entry_exec) {
        std::printf("[poca-load] FAIL entry symbol is not the linked entry point\n");
        release_plugin(true);
        return 1;
    }
    if (!(entry_exec >= kPsramExecLow && entry_exec < kPsramExecHigh)) {
        std::printf("[poca-load] FAIL entry exec addr outside PSRAM exec window\n");
        release_plugin(true);
        return 1;
    }

    /* two-step cast: -Werror=cast-function-type rejects direct fn-to-fn */
    const auto query =
        reinterpret_cast<poca_plugin_query_fn>(reinterpret_cast<void*>(g_plugin.elf.entry));
    std::printf("[poca-load] query fn ptr=0x%08" PRIx32 "\n", reinterpret_cast<uint32_t>(query));
    const poca_plugin_table_t* table = query();
    if (table == nullptr) {
        std::printf("[poca-load] FAIL query returned null\n");
        release_plugin(true);
        return 1;
    }
    std::printf("[poca-load] table ptr=0x%08" PRIx32 " struct_size=%u abi=%u.%u\n",
                reinterpret_cast<uint32_t>(table), static_cast<unsigned>(table->struct_size),
                static_cast<unsigned>(table->abi_major), static_cast<unsigned>(table->abi_minor));
    if (table->struct_size != sizeof(poca_plugin_table_t) ||
        table->abi_major != POCA_PLUGIN_ABI_MAJOR || table->abi_minor != POCA_PLUGIN_ABI_MINOR ||
        table->prepare == nullptr || table->activate == nullptr || table->unload == nullptr) {
        std::printf("[poca-load] FAIL entry table contract\n");
        release_plugin(true);
        return 1;
    }
    const int32_t prepare_err = table->prepare();
    if (prepare_err != 0) {
        std::printf("[poca-load] FAIL prepare err=%d (%s)\n", static_cast<int>(prepare_err),
                    frame_err_name(prepare_err));
        release_plugin(true);
        return 1;
    }
    std::printf("[poca-load] prepare()=0 OK\n");

    /* Loaded image address evidence: text/data windows live in the PSRAM data
     * mapping; entry pointers run on the mirrored exec window. */
    std::printf("[poca-load] text [0x%08" PRIx32 " .. +0x%zx) data [0x%08" PRIx32
                " .. +0x%zx) bss 0x%zx rodata 0x%zx drlro 0x%zx\n",
                reinterpret_cast<uint32_t>(g_plugin.elf.ptext), g_plugin.elf.sec[ELF_SEC_TEXT].size,
                reinterpret_cast<uint32_t>(g_plugin.elf.pdata),
                g_plugin.elf.sec[ELF_SEC_DATA].size + g_plugin.elf.sec[ELF_SEC_RODATA].size +
                    g_plugin.elf.sec[ELF_SEC_DRLRO].size,
                g_plugin.elf.sec[ELF_SEC_BSS].size, g_plugin.elf.sec[ELF_SEC_RODATA].size,
                g_plugin.elf.sec[ELF_SEC_DRLRO].size);
    const uint32_t text_addr = reinterpret_cast<uint32_t>(g_plugin.elf.ptext);
    if (!(text_addr >= kPsramDataLow && text_addr < kPsramDataHigh)) {
        std::printf("[poca-load] FAIL text addr outside PSRAM data window\n");
        release_plugin(true);
        return 1;
    }
    if (mirror_delta != kPsramExecMirrorOffset) {
        std::printf("[poca-load] FAIL unexpected exec mirror delta\n");
        release_plugin(true);
        return 1;
    }

    g_plugin.table = table;
    g_plugin.loaded = true;
    const uint32_t free_after = psram_free_bytes();
    std::printf("[poca-load] psram free after=%" PRIu32 " consumed=%" PRIu32 "\n", free_after,
                g_plugin.psram_free_before_load - free_after);
    std::printf("[poca-load] PASS\n");
    return 0;
}

int cmd_poca_activate() {
    if (!g_plugin.loaded || g_plugin.table == nullptr) {
        std::printf("[poca-activate] FAIL no loaded plugin; run poca load first\n");
        return 1;
    }
    const poca_plugin_table_t* table = g_plugin.table;
    const auto activate = table->activate;
    std::printf("[poca-activate] activate fn ptr=0x%08" PRIx32 "\n",
                reinterpret_cast<uint32_t>(activate));
    const uint32_t fn_addr = reinterpret_cast<uint32_t>(activate);
    if (!(fn_addr >= kPsramExecLow && fn_addr < kPsramExecHigh)) {
        std::printf("[poca-activate] FAIL activate fn outside PSRAM exec window\n");
        return 1;
    }
    /* The value is computed by plugin code executing from PSRAM over the
     * plugin's own pattern bytes; expected is computed here over the flash
     * copy of the identical constants. */
    const int32_t returned = activate();
    const int32_t expected =
        static_cast<int32_t>(POCA_PLUGIN_MAGIC ^ poca_crc32(POCA_BASELINE_PATTERN, 64u));
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
    if (!g_plugin.loaded) {
        std::printf("[poca-unload] FAIL no loaded plugin\n");
        return 1;
    }
    const int32_t unload_err = g_plugin.table->unload();
    if (unload_err != 0) {
        std::printf("[poca-unload] plugin unload err=%d (%s); continuing teardown\n",
                    static_cast<int>(unload_err), frame_err_name(unload_err));
    }
    release_plugin(true);
    const uint32_t free_after = psram_free_bytes();
    std::printf(
        "[poca-unload] psram free after=%" PRIu32 " (before-load=%" PRIu32 " delta=%+" PRId32 ")\n",
        free_after, g_plugin.psram_free_before_load,
        static_cast<int32_t>(free_after) - static_cast<int32_t>(g_plugin.psram_free_before_load));
    std::printf("[poca-unload] PASS\n");
    return 0;
}

} // namespace frame::poca
