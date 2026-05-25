#pragma once
// repair_translator.h
// Address translation through the 4-layer HBM repair hierarchy.
//
// When Ramulator::g_debug_address is true (--debug-address flag),
// translate() prints for every request:
//   1. Raw physical address + bit-level decomposition into ch/pch/bg/ba/row/col
//   2. Table 1 (Layer D) bad-bank lookup result
//   3. Table 2 (Layer A) SRAM overflow row lookup result
//   4. Table 3 (Layer B) DED whole-row lookup result
//   5. Table 4 (Layer C) burst col-range scan result
//   6. Final translated address or NONE

#include <iomanip>
#include <iostream>
#include <string>

#include "dram_controller/impl/repair/repair_table.h"
#include "dram_controller/impl/repair/repair_debug.h"
#include "base/base.h"   // AddrVec_t, Addr_t

namespace Ramulator {

// HBM3 addr_vec index  (ChRaBaRoCo mapper output)
// [0]=ch  [1]=pch  [2]=bg  [3]=ba  [4]=row  [5]=col
static constexpr int AIDX_CH  = 0;
static constexpr int AIDX_PCH = 1;
static constexpr int AIDX_BG  = 2;
static constexpr int AIDX_BA  = 3;
static constexpr int AIDX_ROW = 4;
static constexpr int AIDX_COL = 5;

enum class RepairType { NONE, LAYER_A, LAYER_B, LAYER_C, LAYER_D };

inline const char* repair_type_name(RepairType t) {
    switch (t) {
        case RepairType::NONE:    return "NONE (pass-through)";
        case RepairType::LAYER_A: return "LAYER_A (SRAM overflow row)";
        case RepairType::LAYER_B: return "LAYER_B (DED whole-row spare)";
        case RepairType::LAYER_C: return "LAYER_C (burst col-range SRAM)";
        case RepairType::LAYER_D: return "LAYER_D (bad-bank vacuum remap)";
    }
    return "UNKNOWN";
}

class RepairTranslator {
public:
    explicit RepairTranslator(const HbmRepairTable& tbl) : m_tbl(tbl) {}

    // raw_addr = original physical address before addr_mapper decomposed it.
    // Pass 0 if unavailable; the hex field will still print as 0x000000000000.
    RepairType translate(AddrVec_t& av, Addr_t raw_addr = 0) const {
        int ch  = av[AIDX_CH];
        int pch = av[AIDX_PCH];
        int bg  = av[AIDX_BG];
        int ba  = av[AIDX_BA];
        int row = av[AIDX_ROW];
        int col = av[AIDX_COL];

        const bool dbg = g_debug_address.load(std::memory_order_relaxed);

        // ¢w¢w Debug header ¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w
        if (dbg) {
            std::cerr
                << "\n[ADDR-DBG] ==========================================\n"
                << "  raw addr : 0x" << std::hex << std::setw(12)
                << std::setfill('0') << raw_addr
                << std::dec << std::setfill(' ')
                << "  (" << raw_addr << ")\n"
                << "  decomposed:\n"
                << "    ch  = " << ch  << "\n"
                << "    pch = " << pch << "\n"
                << "    bg  = " << bg  << "\n"
                << "    ba  = " << ba  << "\n"
                << "    row = " << row << "\n"
                << "    col = " << col << "\n";
        }

        // ¢w¢w STEP 0: Layer D ¡X bad bank (vacuum remap) ¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w
        bool hit_d = m_tbl.bad_bank_set.count(
            std::make_tuple(ch, pch, bg, ba));
        if (dbg) {
            std::cerr << "  [Table 1 / Layer D] lookup (ch=" << ch
                << " pch=" << pch << " bg=" << bg << " ba=" << ba
                << ") -> " << (hit_d ? "HIT" : "miss") << "\n";
        }
        if (hit_d) {
            int new_row = m_tbl.cfg.rows_per_bank - m_tbl.K + (row % m_tbl.K);
            if (dbg) {
                std::cerr
                    << "    vacuum remap: row " << row
                    << " -> spare row " << new_row
                    << "  (rows_per_bank=" << m_tbl.cfg.rows_per_bank
                    << "  K=" << m_tbl.K << ")\n"
                    << "  RESULT: " << repair_type_name(RepairType::LAYER_D)
                    << "  row=" << new_row << "\n"
                    << "[ADDR-DBG] ==========================================\n";
            }
            av[AIDX_ROW] = new_row;
            return RepairType::LAYER_D;
        }

        // ¢w¢w STEP 1: Layer A ¡X SRAM overflow row ¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w
        {
            auto it = m_tbl.sram_full_map.find(
                std::make_tuple(ch, pch, bg, ba, row));
            bool hit = (it != m_tbl.sram_full_map.end());
            if (dbg) {
                std::cerr << "  [Table 2 / Layer A] lookup (ch=" << ch
                    << " pch=" << pch << " bg=" << bg << " ba=" << ba
                    << " row=" << row << ") -> "
                    << (hit ? "HIT slot=" + std::to_string(it->second)
                            : "miss")
                    << "\n";
            }
            if (hit) {
                int new_row = m_tbl.cfg.rows_per_bank
                            + m_tbl.cfg.total_spare_rows
                            + it->second;
                if (dbg) {
                    std::cerr
                        << "    SRAM slot " << it->second
                        << " -> physical row " << new_row << "\n"
                        << "  RESULT: " << repair_type_name(RepairType::LAYER_A)
                        << "  row=" << new_row << "\n"
                        << "[ADDR-DBG] ==========================================\n";
                }
                av[AIDX_ROW] = new_row;
                return RepairType::LAYER_A;
            }
        }

        // ¢w¢w STEP 2: Layer B ¡X DED whole-row spare ¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w
        {
            auto it = m_tbl.ded_row_map.find(
                std::make_tuple(ch, pch, bg, ba, row));
            bool hit = (it != m_tbl.ded_row_map.end());
            if (dbg) {
                std::cerr << "  [Table 3 / Layer B] lookup (ch=" << ch
                    << " pch=" << pch << " bg=" << bg << " ba=" << ba
                    << " row=" << row << ") -> "
                    << (hit ? "HIT offset=" + std::to_string(it->second)
                            : "miss")
                    << "\n";
            }
            if (hit) {
                int new_row = m_tbl.ded_spare_row_addr(it->second);
                if (dbg) {
                    std::cerr
                        << "    spare offset " << it->second
                        << " -> physical row " << new_row
                        << "  (ded_spare_base=" << m_tbl.cfg.ded_spare_base()
                        << ")\n"
                        << "  RESULT: " << repair_type_name(RepairType::LAYER_B)
                        << "  row=" << new_row << "\n"
                        << "[ADDR-DBG] ==========================================\n";
                }
                av[AIDX_ROW] = new_row;
                return RepairType::LAYER_B;
            }
        }

        // ¢w¢w STEP 3: Layer C ¡X burst col-range remap ¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w
        {
            auto it = m_tbl.burst_map.lower_bound(
                std::make_tuple(ch, pch, bg, ba, row, 0));
            if (dbg) {
                std::cerr << "  [Table 4 / Layer C] scanning burst entries for"
                    << " (ch=" << ch << " pch=" << pch
                    << " bg=" << bg << " ba=" << ba
                    << " row=" << row << " col=" << col << ")\n";
            }
            int checked = 0;
            while (it != m_tbl.burst_map.end()) {
                auto& [key, be] = *it;
                auto& [kch, kpch, kbg, kba, krow, kcol] = key;
                if (kch != ch || kpch != pch || kbg != bg ||
                    kba != ba  || krow != row) break;
                ++checked;
                bool col_hit = (col >= be.col_start &&
                                col < be.col_start + be.length);
                if (dbg) {
                    std::cerr
                        << "    entry[" << (checked - 1)
                        << "] col_start=" << be.col_start
                        << " len=" << be.length
                        << " slot=" << be.target_slot
                        << " -> col " << col << " "
                        << (col_hit ? "IN RANGE" : "out of range") << "\n";
                }
                if (col_hit) {
                    auto [new_row, new_col] =
                        m_tbl.burst_slot_to_addr(be.target_slot);
                    if (dbg) {
                        std::cerr
                            << "    slot " << be.target_slot
                            << " -> (burst_spare_base="
                            << m_tbl.cfg.burst_spare_base()
                            << ") row=" << new_row
                            << " col=" << new_col << "\n"
                            << "  RESULT: "
                            << repair_type_name(RepairType::LAYER_C)
                            << "  row=" << new_row << " col=" << new_col << "\n"
                            << "[ADDR-DBG] ==========================================\n";
                    }
                    av[AIDX_ROW] = new_row;
                    av[AIDX_COL] = new_col;
                    return RepairType::LAYER_C;
                }
                ++it;
            }
            if (dbg && checked == 0) {
                std::cerr << "    (no burst entries for this row)\n";
            }
        }

        // ¢w¢w No repair needed ¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w¢w
        if (dbg) {
            std::cerr
                << "  RESULT: " << repair_type_name(RepairType::NONE) << "\n"
                << "[ADDR-DBG] ==========================================\n";
        }
        return RepairType::NONE;
    }

private:
    const HbmRepairTable& m_tbl;
};

} // namespace Ramulator