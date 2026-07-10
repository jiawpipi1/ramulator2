#pragma once
// repair_translator.h
// Address translation through the 4-layer HBM repair hierarchy.
//
// Layer priority / flow for one request:
//   Layer D (dead bank) : RELOCATE the access to a live bank's reserved top-row
//                         band, then FALL THROUGH to A/B/C on the relocated
//                         address (the target top row may itself be faulty).
//   Layer A (SRAM)      : overflow row -> SRAM replacement row.
//   Layer B (DED)       : faulty row   -> dedicated whole-row spare.
//   Layer C (burst)     : faulty col range -> burst spare slot.
// A/B/C are mutually exclusive for a given (bank,row). Layer D cannot re-fire
// after relocation because the target is a live bank (never in bad_bank_set).
//
// Layer D interleave (combinational, no per-row table):
//   Let R  = rows_per_bank, F = #dead banks, L = #live banks (all banks - dead).
//       h  = ceil(R / L)                    (band height: rows one dead bank
//                                            places in each live bank)
//   For a dead bank with ordinal i (its rank among dead banks in canonical
//   (ch, ly=bg*num_pch+pch, ba) order) and incoming row r:
//       j    = r / h            -> index into the ordered live-bank list
//       o    = r % h            -> offset within the band
//       tgt  = live_banks[j]
//       trow = R - (i+1)*h + o  -> dead bank i occupies band [R-(i+1)h, R-i*h)
//   So dead bank ordinal 0 takes the topmost band of every live bank, ordinal 1
//   the next band down, etc. The union of bands is the per-live-bank vacuum
//   region [R - F*h, R) that the OS is told not to use.
//
// When Ramulator::g_debug_address is true (--debug-address) translate() prints
// a per-request trace of every layer lookup.
//
// addr_vec layout (HBM3, ChRaBaRoCo): [0]=ch [1]=pch [2]=bg [3]=ba [4]=row [5]=col

#include <iomanip>
#include <iostream>
#include <map>
#include <tuple>
#include <vector>

#include "dram_controller/impl/repair/repair_table.h"
#include "dram_controller/impl/repair/repair_debug.h"
#include "base/base.h"   // AddrVec_t, Addr_t

namespace Ramulator {

// HBM3 addr_vec index  (ChRaBaRoCo mapper output)
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
        case RepairType::LAYER_D: return "LAYER_D (dead-bank relocate + A/B/C)";
    }
    return "UNKNOWN";
}

class RepairTranslator {
public:
    explicit RepairTranslator(const HbmRepairTable& tbl) : m_tbl(tbl) {
        build_bank_lists();
    }

    // Number of live/dead banks and the band height (exposed for tests/introspection).
    int num_live()  const { return m_num_live; }
    int num_dead()  const { return m_num_dead; }
    int band_h()    const { return m_band_h; }
    // Rows reserved per live bank as the Layer D vacuum region.
    int vacuum_rows_per_bank() const { return m_num_dead * m_band_h; }

    RepairType translate(AddrVec_t& av, Addr_t raw_addr = 0) const {
        int ch  = av[AIDX_CH];
        int pch = av[AIDX_PCH];
        int bg  = av[AIDX_BG];
        int ba  = av[AIDX_BA];
        int row = av[AIDX_ROW];
        int col = av[AIDX_COL];

        const bool dbg = g_debug_address.load(std::memory_order_relaxed);
        if (dbg) {
            std::cerr
                << "\n[ADDR-DBG] ==========================================\n"
                << "  raw addr : 0x" << std::hex << std::setw(12)
                << std::setfill('0') << raw_addr
                << std::dec << std::setfill(' ') << "  (" << raw_addr << ")\n"
                << "  decomposed: ch=" << ch << " pch=" << pch << " bg=" << bg
                << " ba=" << ba << " row=" << row << " col=" << col << "\n";
        }

        RepairType d_result = RepairType::NONE;

        // -- Layer D: dead bank -> relocate to a live bank's reserved band -----
        if (m_tbl.bad_bank_set.count(std::make_tuple(ch, pch, bg, ba))) {
            auto it = m_dead_ordinal.find(std::make_tuple(ch, pch, bg, ba));
            bool ok = (it != m_dead_ordinal.end()) && m_num_live > 0 && m_band_h > 0;
            int j = ok ? (row / m_band_h) : -1;
            if (ok && j < m_num_live) {
                int i = it->second;
                int o = row % m_band_h;
                const auto& tgt = m_live_banks[j];
                int tch  = std::get<0>(tgt);
                int tpch = std::get<1>(tgt);
                int tbg  = std::get<2>(tgt);
                int tba  = std::get<3>(tgt);
                int trow = m_R - (i + 1) * m_band_h + o;
                if (dbg) {
                    std::cerr << "  [Layer D] dead ord=" << i << " row=" << row
                        << " -> live[" << j << "]=(" << tch << "," << tpch << ","
                        << tbg << "," << tba << ") row=" << trow
                        << "  (h=" << m_band_h << " F=" << m_num_dead
                        << " L=" << m_num_live << ")  -> fall through to A/B/C\n";
                }
                av[AIDX_CH] = ch = tch;
                av[AIDX_PCH] = pch = tpch;
                av[AIDX_BG] = bg = tbg;
                av[AIDX_BA] = ba = tba;
                av[AIDX_ROW] = row = trow;
                d_result = RepairType::LAYER_D;
            } else if (dbg) {
                std::cerr << "  [Layer D] dead bank but relocation unavailable "
                    << "(ord_found=" << (it != m_dead_ordinal.end())
                    << " L=" << m_num_live << " j=" << j << ") -> pass-through\n";
            }
        }

        // -- Layer A: SRAM overflow row ---------------------------------------
        {
            auto it = m_tbl.sram_full_map.find(std::make_tuple(ch, pch, bg, ba, row));
            if (it != m_tbl.sram_full_map.end()) {
                int new_row = m_tbl.cfg.rows_per_bank
                            + m_tbl.cfg.total_spare_rows + it->second;
                if (dbg) std::cerr << "  [Layer A] slot " << it->second
                                   << " -> row " << new_row << "\n";
                av[AIDX_ROW] = new_row;
                return d_result == RepairType::LAYER_D ? RepairType::LAYER_D
                                                       : RepairType::LAYER_A;
            }
        }

        // -- Layer B: DED whole-row spare -------------------------------------
        {
            auto it = m_tbl.ded_row_map.find(std::make_tuple(ch, pch, bg, ba, row));
            if (it != m_tbl.ded_row_map.end()) {
                int new_row = m_tbl.ded_spare_row_addr(it->second);
                if (dbg) std::cerr << "  [Layer B] offset " << it->second
                                   << " -> row " << new_row << "\n";
                av[AIDX_ROW] = new_row;
                return d_result == RepairType::LAYER_D ? RepairType::LAYER_D
                                                       : RepairType::LAYER_B;
            }
        }

        // -- Layer C: burst col-range remap -----------------------------------
        {
            auto it = m_tbl.burst_map.lower_bound(
                std::make_tuple(ch, pch, bg, ba, row, 0));
            while (it != m_tbl.burst_map.end()) {
                auto& [key, be] = *it;
                auto& [kch, kpch, kbg, kba, krow, kcol] = key;
                if (kch != ch || kpch != pch || kbg != bg ||
                    kba != ba  || krow != row) break;
                if (col >= be.col_start && col < be.col_start + be.length) {
                    auto [new_row, new_col] = m_tbl.burst_slot_to_addr(be.target_slot);
                    if (dbg) std::cerr << "  [Layer C] slot " << be.target_slot
                                       << " -> row " << new_row << " col " << new_col << "\n";
                    av[AIDX_ROW] = new_row;
                    av[AIDX_COL] = new_col;
                    return d_result == RepairType::LAYER_D ? RepairType::LAYER_D
                                                           : RepairType::LAYER_C;
                }
                ++it;
            }
        }

        if (dbg) std::cerr << "  RESULT: " << repair_type_name(d_result) << "\n"
                           << "[ADDR-DBG] ==========================================\n";
        return d_result;   // LAYER_D if it relocated (A/B/C all missed), else NONE
    }

private:
    using BankKey4 = std::tuple<int,int,int,int>;   // (ch, pch, bg, ba)

    // Enumerate all banks in canonical (ch, ly=bg*num_pch+pch, ba) order,
    // splitting into the ordered live-bank list and dead-bank ordinals. This
    // ordering matches the offline tool's bad-bank emission order.
    void build_bank_lists() {
        const RepairConfig& c = m_tbl.cfg;
        m_R = c.rows_per_bank;
        int ord = 0;
        const int num_ly = c.num_pch * c.num_bg;
        for (int ch = 0; ch < c.num_channels; ++ch) {
            for (int ly = 0; ly < num_ly; ++ly) {
                int pch = ly % c.num_pch;
                int bg  = ly / c.num_pch;
                for (int ba = 0; ba < c.num_ba; ++ba) {
                    BankKey4 k{ch, pch, bg, ba};
                    if (m_tbl.bad_bank_set.count(k)) m_dead_ordinal[k] = ord++;
                    else                             m_live_banks.push_back(k);
                }
            }
        }
        m_num_dead = ord;
        m_num_live = static_cast<int>(m_live_banks.size());
        m_band_h   = (m_num_live > 0) ? (m_R + m_num_live - 1) / m_num_live : 0;
    }

    const HbmRepairTable&  m_tbl;
    std::vector<BankKey4>  m_live_banks;
    std::map<BankKey4,int> m_dead_ordinal;
    int m_R = 0, m_num_live = 0, m_num_dead = 0, m_band_h = 0;
};

} // namespace Ramulator
