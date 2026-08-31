#include "poca_console.hh"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "abi_smoke.hh"
#include "esp_console.h"
#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "poca_plugin.hh"
#include "private/elf_platform.h"
#include "sdkconfig.h"

#ifndef CONFIG_ELF_LOADER_LOAD_PSRAM
#define CONFIG_ELF_LOADER_LOAD_PSRAM 0
#endif
#ifndef CONFIG_ELF_LOADER_CACHE_OFFSET
#define CONFIG_ELF_LOADER_CACHE_OFFSET 0
#endif
#ifndef CONFIG_ELF_LOADER_SET_MMU
#define CONFIG_ELF_LOADER_SET_MMU 0
#endif
#ifndef CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
#define CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT 0
#endif

namespace frame::poca {
namespace {

const char* on_off(int flag) { return flag != 0 ? "y" : "n"; }

void print_loader_config() {
    std::printf("elf_loader: vendored espressif 1.3.3 @6526c5b1 (esp-iot-solution)\n");
    std::printf("  ELF_LOADER=y LOAD_PSRAM=%s CACHE_OFFSET=%s SET_MMU=%s DLSO=%s\n",
                on_off(CONFIG_ELF_LOADER_LOAD_PSRAM), on_off(CONFIG_ELF_LOADER_CACHE_OFFSET),
                on_off(CONFIG_ELF_LOADER_SET_MMU), on_off(CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT));
    std::printf("  esp_elf_t state size: %u bytes\n", static_cast<unsigned>(sizeof(esp_elf_t)));
    std::printf("  loader entries: esp_elf_init=%p esp_elf_relocate=%p\n",
                reinterpret_cast<void*>(&esp_elf_init), reinterpret_cast<void*>(&esp_elf_relocate));
}

void print_heap_caps(const char* label, uint32_t caps) {
    multi_heap_info_t info{};
    heap_caps_get_info(&info, caps);
    const unsigned size = static_cast<unsigned>(info.total_free_bytes + info.total_allocated_bytes);
    std::printf("heap %-9s size=%10u free=%10u largest=%10u min_ever=%10u\n", label, size,
                static_cast<unsigned>(info.total_free_bytes),
                static_cast<unsigned>(info.largest_free_block),
                static_cast<unsigned>(info.minimum_free_bytes));
}

int poca_command(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
        print_loader_config();
        std::printf("  registered host imports: poca_host_sentinel poca_host_add\n");
        std::printf("  entry queries so far: %u (execution canary)\n",
                    static_cast<unsigned>(poca_entry_queries()));
        std::printf("  iram cache syncs so far: %u (p5 counter)\n",
                    static_cast<unsigned>(esp_elf_iram_cache_sync_count()));
        print_heap_caps("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        print_heap_caps("iram-exec", MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
        print_heap_caps("psram", MALLOC_CAP_SPIRAM);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "abi") == 0) {
        return run_abi_smoke();
    }
    if ((argc == 2 || argc == 3) && std::strcmp(argv[1], "verify") == 0) {
        return cmd_poca_verify(argc == 3 ? argv[2] : "baseline");
    }
    if ((argc == 2 || argc == 3) && std::strcmp(argv[1], "load") == 0) {
        return cmd_poca_load(argc == 3 ? argv[2] : "baseline");
    }
    if (argc == 2 && std::strcmp(argv[1], "activate") == 0) {
        return cmd_poca_activate();
    }
    if (argc == 2 && std::strcmp(argv[1], "unload") == 0) {
        return cmd_poca_unload();
    }
    if ((argc == 2 || argc == 3) && std::strcmp(argv[1], "iram") == 0) {
        unsigned iterations = 1;
        if (argc == 3) {
            const int parsed = std::atoi(argv[2]);
            if (parsed < 1 || parsed > 100000) {
                std::printf("[poca-iram] FAIL iteration count out of range (1..100000)\n");
                return 1;
            }
            iterations = static_cast<unsigned>(parsed);
        }
        return cmd_poca_iram(iterations);
    }
    std::printf("usage: poca status | poca abi | poca verify [name] | poca load [name] | "
                "poca activate | poca unload | poca iram [n]\n");
    return 1;
}

} // namespace

esp_err_t start_console() {
    const esp_console_cmd_t poca_cmd{
        .command = "poca",
        .help = "poca status | abi | verify [name] | load [name] | activate | unload | iram [n]",
        .hint = nullptr,
        .func = &poca_command,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };

    esp_err_t result = esp_console_cmd_register(&poca_cmd);
    if (result != ESP_OK) {
        return result;
    }

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "poca> ";
    repl_config.max_cmdline_length = 160;
    repl_config.max_cmdline_args = 8;
    repl_config.task_stack_size = 6144;
    repl_config.task_priority = 5;
    repl_config.task_core_id = 1;
    esp_console_dev_usb_serial_jtag_config_t device_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t* repl = nullptr;
    result = esp_console_new_repl_usb_serial_jtag(&device_config, &repl_config, &repl);
    return result == ESP_OK ? esp_console_start_repl(repl) : result;
}

} // namespace frame::poca
