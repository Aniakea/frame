#ifndef POC_A_MAIN_POCA_PLUGIN_HH
#define POC_A_MAIN_POCA_PLUGIN_HH

#include <cstdint>

namespace frame::poca {

/* Console subcommands implemented in poca_plugin.cc (task T4, extended in
 * T5 with the named-package relocation/import allowlist matrix, in T7 with
 * the IRAM probe, in T8 with generation coexistence, in T9 with the
 * lifecycle soak):
 *   poca verify [name]   parse-only view of an embedded signed package
 *   poca load [name]     stage + verify + admission + relocate + query
 *                        + prepare; negatives assert fail-before-execute
 *   poca activate        call the loaded plugin's activate() (magic check)
 *   poca unload          esp_elf_deinit + staging release
 *   poca iram [n]        T7: n x (load iram_probe -> IRAM fn proofs ->
 *                        unload); IRAM watermark every 20 iterations
 *   poca coexist         T8: v1 ACTIVE + 500ms self-call loop, v2 staged as
 *                        CANDIDATE in an independent arena, single-candidate
 *                        guard, both generations callable, swap+unload old,
 *                        per-phase heap/stack snapshots + budget summary
 *   poca soak <n> [mix]  T9: n lifecycle cycles (load->verify->relocate->
 *                        prepare->activate+value assert->quiesce(no-op;
 *                        NULL entry = trivial success)->unload); schedule:
 *                        mix=default cycles%3==0 iram_probe else baseline,
 *                        mix=baseline / mix=iram force one plugin; every
 *                        100th cycle a v1/v2 coexistence swap; heap+stack
 *                        sample ring every 10 cycles printed as a
 *                        machine-parsable SOAKSMP table; heap integrity
 *                        probe every 50 cycles; SOAKSUM summary line (the
 *                        PASS/FAIL verdict authority is the host judge)
 *   poca capacity <n>    T10: capacity ladder step - sequentially load n
 *                        distinct-name plugins cap01..cap0n (each its own
 *                        esp_elf_t instance held live), check-fn round over
 *                        all n resident generations (n distinct magics +
 *                        pairwise-distinct PSRAM text bases), peak heap/stack
 *                        snapshot + CAPBUD cost line, reverse-order unload;
 *                        markers [PASS-cap-N-load|respond|unload] and
 *                        [PASS-capacity-N] (the 8->7->6->5 descent with the
 *                        retry-once fail-closed policy lives in the host
 *                        runner plugins/capacity_ladder.py)
 * Package names: baseline baseline_v2 baseline_300k globdat plt import_neg
 * iram_probe iram_mismatch neg_maxmem neg_r32 neg_s0op neg_phspan neg_phbe
 * neg_ph64 neg_phmach cap01..cap08 (default baseline).
 */
int cmd_poca_verify(const char* name);
int cmd_poca_load(const char* name);
int cmd_poca_activate();
int cmd_poca_unload();
int cmd_poca_iram(unsigned iterations);
int cmd_poca_coexist();
int cmd_poca_soak(unsigned cycles, const char* mix);
int cmd_poca_capacity(unsigned count);
uint32_t poca_entry_queries();

// T13 loader-strand hookup: activate() pointer of the ACTIVE plugin slot
// (nullptr when nothing is loaded) and the firmware-side expected activate
// value, so a strand handler can call plugin entry code and check it.
const void* poca_active_activate_fn();
int32_t poca_active_expected_activate();

} // namespace frame::poca

#endif /* POC_A_MAIN_POCA_PLUGIN_HH */
