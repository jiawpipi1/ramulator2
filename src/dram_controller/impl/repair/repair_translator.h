#pragma once
// repair_translator.h

#include "dram_controller/impl/repair/repair_table.h"
#include "base/base.h"   // AddrVec_t

namespace Ramulator {

// HBM3 addr_vec index（ChRaBaRoCo mapper 輸出）
// [0]=ch  [1]=pch  [2]=bg  [3]=ba  [4]=row  [5]=col
static constexpr int AIDX_CH  = 0;
static constexpr int AIDX_PCH = 1;
static constexpr int AIDX_BG  = 2;   // ← 新增 bankgroup
static constexpr int AIDX_BA  = 3;   // ← 原來的 AIDX_BK 改名
static constexpr int AIDX_ROW = 4;
static constexpr int AIDX_COL = 5;

enum class RepairType { NONE, LAYER_A, LAYER_B, LAYER_C, LAYER_D };

class RepairTranslator {
public:
    explicit RepairTranslator(const HbmRepairTable& tbl) : m_tbl(tbl) {}

    RepairType translate(AddrVec_t& av) const {
        int ch  = av[AIDX_CH];
        int pch = av[AIDX_PCH];
        int bg  = av[AIDX_BG];   // ← 拆成兩個
        int ba  = av[AIDX_BA];   // ← 拆成兩個
        int row = av[AIDX_ROW];
        int col = av[AIDX_COL];

        // STEP 0: Layer D — bad bank vacuum remap
        if (m_tbl.bad_bank_set.count(std::make_tuple(ch, pch, bg, ba))) {
            av[AIDX_ROW] = m_tbl.cfg.rows_per_bank - m_tbl.K + (row % m_tbl.K);
            return RepairType::LAYER_D;
        }

        // STEP 1: Layer A — SRAM overflow row
        {
            auto it = m_tbl.sram_full_map.find(
                std::make_tuple(ch, pch, bg, ba, row));
            if (it != m_tbl.sram_full_map.end()) {
                av[AIDX_ROW] = m_tbl.cfg.rows_per_bank
                             + m_tbl.cfg.total_spare_rows
                             + it->second;
                return RepairType::LAYER_A;
            }
        }

        // STEP 2: Layer B — DED whole row → spare row 0 or 1
        {
            auto it = m_tbl.ded_row_map.find(
                std::make_tuple(ch, pch, bg, ba, row));
            if (it != m_tbl.ded_row_map.end()) {
                av[AIDX_ROW] = m_tbl.ded_spare_row_addr(it->second);
                return RepairType::LAYER_B;
            }
        }

        // STEP 3: Layer C — burst col-range remap
        {
            auto it = m_tbl.burst_map.lower_bound(
                std::make_tuple(ch, pch, bg, ba, row, 0));
            while (it != m_tbl.burst_map.end()) {
                auto& [key, be] = *it;
                auto& [kch, kpch, kbg, kba, krow, kcol] = key;   // ← 6 元素解包
                if (kch != ch || kpch != pch || kbg != bg ||
                    kba != ba || krow != row) break;
                if (col >= be.col_start && col < be.col_start + be.length) {
                    auto [new_row, new_col] = m_tbl.burst_slot_to_addr(be.target_slot);
                    av[AIDX_ROW] = new_row;
                    av[AIDX_COL] = new_col;
                    return RepairType::LAYER_C;
                }
                ++it;
            }
        }

        return RepairType::NONE;
    }

private:
    const HbmRepairTable& m_tbl;
};

} // namespace Ramulator