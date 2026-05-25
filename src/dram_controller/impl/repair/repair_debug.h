#pragma once
// repair_debug.h
// Global flag for --debug-address.
// Include this wherever you need to check the flag.

#include <atomic>

namespace Ramulator {

inline std::atomic<bool> g_debug_address{false};

} // namespace Ramulator