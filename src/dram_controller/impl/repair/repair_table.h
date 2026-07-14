#pragma once
// repair_table.h
// Loads 4-layer HBM repair tables from JSON produced by ra_algorithm.cpp.
//
// JSON field mapping (new pch/bg/ba format):
//   "layer_d_bad_banks": [{"ch","pch","bg","ba"}, ...]          -> Table 1 (Layer D)
//   "layer_a_sram":      [{"ch","pch","bg","ba","row","sram_slot"},...] -> Table 2 (Layer A)
//   "banks": [{
//     "ch","pch","bg","ba",
//     "layer_b_ded_rows": [row,...],                             -> Table 3 (Layer B)
//     "layer_c_burst": [{"row","col_start","length","target_slot"},...] -> Table 4 (Layer C)
//   }]
//
// Ramulator addr_vec layout (HBM3, ChRaBaRoCo):
//   [0]=ch  [1]=pch  [2]=bg  [3]=ba  [4]=row  [5]=col

#include <map>
#include <set>
#include <tuple>
#include <string>
#include <iostream>

namespace Ramulator {

// 4-component bank key: (ch, pch, bg, ba)
using BankKey  = std::tuple<int,int,int,int>;
// 5-component row key:  (ch, pch, bg, ba, row)
using RowKey   = std::tuple<int,int,int,int,int>;
// 6-component burst key:(ch, pch, bg, ba, row, col_start)
using BurstKey = std::tuple<int,int,int,int,int,int>;

struct BurstEntry {
  int row;
  int col_start;
  int length;
  int target_slot;  // index into burst spare storage (0-based)
};

struct RepairConfig {
  int total_spare_rows = 4;
  int bursts_per_row   = 32;
  int ded_count        = 2;  // total_spare_rows / 2
  int frag_count       = 2;  // total_spare_rows - ded_count
  int sram_slots       = 16;
  int rows_per_bank    = 16384;
  int vacuum_limit     = 32;

  // Bank geometry (needed by Layer D to enumerate live banks).
  // A die's banks are (ch, pch, bg, ba); ly = bg*num_pch + pch matches the
  // offline tool's ordering. Defaults are HBM3 (16 ch x 2 pch x 4 bg x 4 ba = 512).
  int num_channels = 16;
  int num_pch      = 2;
  int num_bg       = 4;
  int num_ba       = 4;

  int banks_per_channel() const { return num_pch * num_bg * num_ba; }
  int total_banks()       const { return num_channels * banks_per_channel(); }

  // Physical row addresses for spare rows (appended above normal rows)
  int ded_spare_base()   const { return rows_per_bank; }             // 16384
  int burst_spare_base() const { return rows_per_bank + ded_count; } // 16386
};

struct HbmRepairTable {
  int hbm_id = -1;
  int K      = 0;   // Vacuum: rows reserved at top of each good bank

  RepairConfig cfg;

  
  std::set<BankKey> bad_bank_set;

  
  std::map<RowKey, int> sram_full_map;

  
  std::map<RowKey, int> ded_row_map;

  
  std::map<BurstKey, BurstEntry> burst_map;

  
  static bool load_from_json(const std::string& path, HbmRepairTable& out);

  
  int ded_spare_row_addr(int offset) const {
    return cfg.ded_spare_base() + offset;
  }

  // Physical {row, col} for a Layer-C target_slot
  std::pair<int,int> burst_slot_to_addr(int target_slot) const {
    int frag = target_slot / cfg.bursts_per_row;
    int col  = target_slot % cfg.bursts_per_row;
    int row  = cfg.burst_spare_base() + frag;
    return {row, col};
  }

  void print_summary(std::ostream& os = std::cout) const;
  void print_detail (std::ostream& os = std::cout) const;
};

} // namespace Ramulator
