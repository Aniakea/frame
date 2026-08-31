#ifndef POC_A_MAIN_POCA_PLUGIN_HH
#define POC_A_MAIN_POCA_PLUGIN_HH

#include <cstdint>

namespace frame::poca {

/* Console subcommands implemented in poca_plugin.cc (task T4, extended in
 * T5 with the named-package relocation/import allowlist matrix, in T7 with
 * the IRAM probe):
 *   poca verify [name]   parse-only view of an embedded signed package
 *   poca load [name]     stage + verify + phdr admission + relocate + query
 *                        + prepare; negatives assert fail-before-execute
 *   poca activate        call the loaded plugin's activate() (magic check)
 *   poca unload          esp_elf_deinit + staging release
 *   poca iram [n]        T7: n x (load iram_probe -> IRAM fn proofs ->
 *                        unload); IRAM watermark every 20 iterations
 * Package names: baseline globdat plt import_neg iram_probe iram_mismatch
 * neg_r32 neg_s0op neg_phspan neg_phbe neg_ph64 neg_phmach (default
 * baseline).
 */
int cmd_poca_verify(const char* name);
int cmd_poca_load(const char* name);
int cmd_poca_activate();
int cmd_poca_unload();
int cmd_poca_iram(unsigned iterations);
uint32_t poca_entry_queries();

} // namespace frame::poca

#endif /* POC_A_MAIN_POCA_PLUGIN_HH */
