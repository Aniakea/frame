#pragma once

namespace frame::poca {

// Runs the PoC-A ABI smoke checks (header layout, size negotiation, and
// feature-bit compatibility) and prints one PASS/FAIL line per check.
// Returns 0 when every check passes, 1 otherwise.
int run_abi_smoke();

} // namespace frame::poca
