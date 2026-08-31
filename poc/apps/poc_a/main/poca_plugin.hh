#ifndef POC_A_MAIN_POCA_PLUGIN_HH
#define POC_A_MAIN_POCA_PLUGIN_HH

namespace frame::poca {

/* Console subcommands implemented in poca_plugin.cc (task T4):
 *   poca verify   parse-only view of the embedded signed plugin package
 *   poca load     stage + verify + relocate + query + prepare
 *   poca activate call the plugin entry-table activate() and check the magic
 *   poca unload   esp_elf_deinit + staging release
 */
int cmd_poca_verify();
int cmd_poca_load();
int cmd_poca_activate();
int cmd_poca_unload();

} // namespace frame::poca

#endif /* POC_A_MAIN_POCA_PLUGIN_HH */
