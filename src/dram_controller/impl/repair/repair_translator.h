#pragma once
#include <tuple>
#include <set>
#include <map>
#include <vector>
#include "base/base.h"   // AddrVec_t

namespace Ramulator {

// ── HBM3 addr_vec index ────────────────────────────────
// [0]=channel  [1]=pseudochannel(ly)  [2]=bankgroup
// [3]=bank     [4]=row                [5]=column
static constexpr int AIDX_CH  = 0;
static constexpr int AIDX_PCH = 1;   // ly
static constexpr int AIDX_BK  = 3;
static constexpr int AIDX_ROW = 4;
static constexpr int AIDX_COL = 5;

// ── Layer C burst entry ─────────────────────────────────
struct BurstEntry {
    int col_start;
    int length;
    int target_slot;   // 對應 spare row 的 burst slot index
};

// ── 整張 repair table（由外部 load 進來） ──────────────
struct HbmRepairTable {
    int hbm_id = -1;
    int K      =  0;

    // Layer D: bad bank set
    std::set<std::tuple<int,int,int>> bad_bank_set;  // (ch, ly, bk)

    // Layer A: sram overflow row map
    // key=(ch, ly, bk, row), value=sram_slot
    std::map<std::tuple<int,int,int,int>, int> sram_map;

    // Layer B: ded row → spare row index (0 or 1)
    // key=(ch, ly, bk, row), value = ded_idx (0 or 1)
    std::map<std::tuple<int,int,int,int>, int> ded_row_map;

    // Layer C: burst map
    // key=(ch, ly, bk, row, col_start), value=BurstEntry
    std::map<std::tuple<int,int,int,int,int>, BurstEntry> burst_map;

    // Config（從 JSON 或 yaml 讀入）
    int rows_per_bank = 16384;
    int total_spare_rows = 4;  // ded=2, frag=2
    int bursts_per_row   = 64;
};

// ── Repair 結果類型 ─────────────────────────────────────
enum class RepairType {
    NONE,
    LAYER_D,   // bad bank vacuum remap
    LAYER_B,   // DED row → spare row 0/1
    LAYER_C,   // burst → spare row 2/3 + col remap
    LAYER_A,   // SRAM overflow row，bypass DRAM
};

// ── RepairTranslator ────────────────────────────────────
class RepairTranslator {
public:
    explicit RepairTranslator(const HbmRepairTable& tbl) : m_tbl(tbl) {}

    // 主函數：原地修改 addr_vec，回傳修復類型
    // 若回傳 LAYER_A，呼叫方需自行 bypass DRAM（設 depart）
    RepairType translate(AddrVec_t& addr_vec) const {
        int ch  = addr_vec[AIDX_CH];
        int pch = addr_vec[AIDX_PCH];
        int bk  = addr_vec[AIDX_BK];
        int row = addr_vec[AIDX_ROW];
        int col = addr_vec[AIDX_COL];

        // ── STEP 0: Layer D — bad bank vacuum remap ───────────
        if (m_tbl.bad_bank_set.count({ch, pch, bk})) {
            // bad bank 的 row 超出 [0, K-1] 就視為 DEAD（照常走，硬體層處理）
            // 這裡只把 row address 重算：映射到某個 good bank 的 top K rows
            // 最小化實作：先不換 bank，只換 row offset 到 spare 區
            // 完整實作請對應 ra_algorithm 的 Stage 3 (linear offset)
            addr_vec[AIDX_ROW] = remap_vacuum_row(row);
            return RepairType::LAYER_D;
        }

        // ── STEP 1: Layer A — SRAM overflow row ───────────────
        // 注意：Layer A 優先於 B/C，因為 Stage 2 evict 掉的 bank
        // 其 overflow row 已被放到 SRAM，但 bank 本身已是 bad_bank
        // 所以實際上 Layer A 只會在 non-bad-bank 的 overflow row 命中
        {
            auto key = std::make_tuple(ch, pch, bk, row);
            auto it  = m_tbl.sram_map.find(key);
            if (it != m_tbl.sram_map.end()) {
                // [FIX] 把 row 換成 SRAM 對應的實體位置
                // SRAM 在 chip 裡是獨立的儲存區，用 sram_slot 當作 row offset
                // 實際 row address = rows_per_bank + total_spare_rows + sram_slot
                addr_vec[AIDX_ROW] = m_tbl.rows_per_bank
                                + m_tbl.total_spare_rows
                                + it->second;  // sram_slot (0~15)
                return RepairType::LAYER_A;
            }
        }



        // ── STEP 2: Layer B — DED row → spare row 0 or 1 ─────
        {
            auto key = std::make_tuple(ch, pch, bk, row);
            auto it  = m_tbl.ded_row_map.find(key);
            if (it != m_tbl.ded_row_map.end()) {
                int ded_idx = it->second;  // 0 or 1
                addr_vec[AIDX_ROW] = spare_row_b(ded_idx);
                return RepairType::LAYER_B;
            }
        }

        // ── STEP 3: Layer C — burst → spare row 2/3 + col ────
        {
            // 搜尋同一個 (ch, pch, bk, row) 下所有 col_start
            auto it = m_tbl.burst_map.lower_bound(
                std::make_tuple(ch, pch, bk, row, 0));
            while (it != m_tbl.burst_map.end()) {
                auto& [key, be] = *it;
                auto& [kch, kpch, kbk, krow, kcol_start] = key;
                if (kch != ch || kpch != pch || kbk != bk || krow != row) break;
                if (col >= be.col_start && col < be.col_start + be.length) {
                    // target_slot → spare row 2 or 3, col = slot % 64
                    int frag_idx = be.target_slot / m_tbl.bursts_per_row;
                    addr_vec[AIDX_ROW] = spare_row_c(frag_idx);
                    addr_vec[AIDX_COL] = be.target_slot % m_tbl.bursts_per_row;
                    return RepairType::LAYER_C;
                }
                ++it;
            }
        }

        return RepairType::NONE;
    }

    // Layer A 命中時，外部用這個取得 sram_slot（用於統計或 latency 模擬）
    int get_sram_slot(const AddrVec_t& addr_vec) const {
        int ch  = addr_vec[AIDX_CH];
        int pch = addr_vec[AIDX_PCH];
        int bk  = addr_vec[AIDX_BK];
        int row = addr_vec[AIDX_ROW];
        auto it = m_tbl.sram_map.find({ch, pch, bk, row});
        return (it != m_tbl.sram_map.end()) ? it->second : -1;
    }

private:
    const HbmRepairTable& m_tbl;

    // Layer B spare row address: rows_per_bank + ded_idx (0 or 1)
    int spare_row_b(int ded_idx) const {
        return m_tbl.rows_per_bank + ded_idx;
    }

    // Layer C spare row address: rows_per_bank + 2 + frag_idx (0 or 1)
    int spare_row_c(int frag_idx) const {
        return m_tbl.rows_per_bank + 2 + frag_idx;
    }

    // Layer D vacuum row remap（簡化版）
    // bad bank 的 row i → rows_per_bank - K + i
    // 完整版需要重算目標 (ch, pch, bk)，這裡先只換 row index
    int remap_vacuum_row(int bad_row) const {
        if (bad_row >= m_tbl.K) return m_tbl.rows_per_bank - 1; // fallback
        return m_tbl.rows_per_bank - m_tbl.K + bad_row;
    }
};

} // namespace Ramulator