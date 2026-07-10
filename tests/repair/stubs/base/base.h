#pragma once
// Minimal stub of Ramulator's base/base.h for standalone unit testing of the
// repair translator. The real base/base.h pulls in spdlog + yaml-cpp and the
// whole Implementation/Factory machinery, none of which repair_translator.h
// actually needs -- it only uses AddrVec_t and Addr_t. Put this dir FIRST on
// the include path so it shadows src/base/base.h.
#include <vector>
#include <cstdint>

namespace Ramulator {
using Addr_t    = int64_t;
using AddrVec_t = std::vector<int>;
}
