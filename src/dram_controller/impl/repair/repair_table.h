#pragma once
// repair_table.h
//   Table 1 (Layer D): bad_bank_set       <- layer_d_bad_banks
//   Table 2 (Layer A): sram_full_map      <- layer_a_sram
//   Table 3 (Layer B): ded_row_map        <- banks[].layer_b_ded_rows
//   Table 4 (Layer C): burst_map          <- banks[].layer_c_burst

#include <map>
#include <set>
#include <tuple>
#include <vector>
#include <string>
#include <iostream>

namespace Ramulator {

// ─── Key types ────────────────────────────────────────────────────────────────
// (channel, layer, bank)
using BankKey  = std::tuple<int,int,int>;
// (channel, layer, bank, row)
using RowKey   = std::tuple<int,int,int,int>;
// (channel, layer, bank, row, col_start)
using BurstKey = std::tuple<int,int,int,int,int>;

// ─── Per-entry structures ─────────────────────────────────────────────────────
struct BurstEntry {
  int row;
  int col_start;
  int length;
  int target_slot;   // index into burst spare rows (0-based)
};

// ─── Hardware config（與 ra_algorithm.cpp 預設對齊）───────────────────────────
struct RepairConfig {
  int total_spare_rows = 4;
  int bursts_per_row   = 64;
  int ded_count        = 2;   // total_spare_rows / 2
  int frag_count       = 2;   // total_spare_rows - ded_count
  int sram_slots       = 16;
  int rows_per_bank    = 16384;

  int ded_spare_base()   const { return rows_per_bank; }           // 16384
  int burst_spare_base() const { return rows_per_bank + ded_count; } // 16386
};

// ─── Main table ───────────────────────────────────────────────────────────────
struct HbmRepairTable {
  int hbm_id = -1;
  int K      = 0;    // Vacuum: rows reserved at top of each good bank

  RepairConfig cfg;

  // Table 1: Layer D — bad banks (整顆 bank 壞掉 → Vacuum remap)
  // Key: (ch, ly, bk)
  std::set<BankKey> bad_bank_set;

  // Table 2: Layer A — SRAM overflow rows
  // Key: (ch, ly, bk, row)  →  sram_slot index
  std::map<RowKey, int> sram_full_map;

  // Table 3: Layer B — DED whole-row replacement
  // Key: (ch, ly, bk, row)  →  spare_row_offset (0 or 1)
  std::map<RowKey, int> ded_row_map;

  // Table 4: Layer C — Burst col-range remap
  // Key: (ch, ly, bk, row, col_start)  →  BurstEntry
  std::map<BurstKey, BurstEntry> burst_map;

  // ─── Loader ───────────────────────────────────────────────────────────────
  // Returns false and prints error if JSON cannot be opened/parsed.
  static bool load_from_json(const std::string& path, HbmRepairTable& out);

  // ─── Debug ────────────────────────────────────────────────────────────────
  void print_summary(std::ostream& os = std::cout) const;
  void print_detail(std::ostream& os = std::cout) const;

  // ─── Address helpers ──────────────────────────────────────────────────────
  // Physical DRAM row addr for Layer-B spare row (offset = 0 or 1)
  int ded_spare_row_addr(int offset) const {
    return cfg.ded_spare_base() + offset;
  }

  // Physical {row, col} for Layer-C burst target_slot
  std::pair<int,int> burst_slot_to_addr(int target_slot) const {
    int frag = target_slot / cfg.bursts_per_row;
    int col  = target_slot % cfg.bursts_per_row;
    int row  = cfg.burst_spare_base() + frag;
    return {row, col};
  }
};

} // namespace Ramulator