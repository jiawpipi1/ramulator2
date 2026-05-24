// repair_table.cpp
// 從 ra_algorithm.cpp 吐出的 JSON 載入 4 張修復表並印出摘要確認。
//
// JSON 結構（你的 remap_hbm_1335.json）：
// {
//   "hbm_id": 1335,
//   "K": 4,
//   "layer_d_bad_banks": [[ch, ly, bk], ...],
//   "layer_a_sram": [{"ch":…, "ly":…, "bk":…, "row":…, "sram_slot":…}, …],
//   "banks": [
//     { "ch":…, "ly":…, "bk":…,
//       "layer_b_ded_rows": [row, …],
//       "layer_c_burst": [{"row":…,"col_start":…,"length":…,"target_slot":…}, …]
//     }, …
//   ]
// }

#include "dram_controller/impl/repair/repair_table.h"

#include <fstream>
#include <iomanip>
#include <stdexcept>

// nlohmann/json 已在 ramulator2 extern/ 裡
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace Ramulator {

bool HbmRepairTable::load_from_json(const std::string& path, HbmRepairTable& out) {

  std::ifstream f(path);
  if (!f.is_open()) {
    std::cerr << "[RepairTable] ERROR: cannot open \"" << path << "\"\n";
    return false;
  }

  json j;
  try {
    f >> j;
  } catch (const json::parse_error& e) {
    std::cerr << "[RepairTable] JSON parse error: " << e.what() << "\n";
    return false;
  }

  out.hbm_id = j.value("hbm_id", -1);
  out.K      = j.value("K",      0);

  // ── Table 1: Layer D bad banks ─────────────────────────────────────────────
  // JSON: "layer_d_bad_banks": [[ch, ly, bk], ...]
  if (j.contains("layer_d_bad_banks")) {
    for (auto& entry : j["layer_d_bad_banks"]) {
        int ch = entry["ch"].get<int>();   // ← 改成用 key 取值
        int ly = entry["ly"].get<int>();
        int bk = entry["bk"].get<int>();
        out.bad_bank_set.insert({ch, ly, bk});
    }
  }

  // ── Table 2: Layer A SRAM full-row map ────────────────────────────────────
  // JSON: "layer_a_sram": [{"ch":…,"ly":…,"bk":…,"row":…,"sram_slot":…}, …]
  if (j.contains("layer_a_sram")) {
    for (auto& entry : j["layer_a_sram"]) {
      int ch   = entry["ch"].get<int>();
      int ly   = entry["ly"].get<int>();
      int bk   = entry["bk"].get<int>();
      int row  = entry["row"].get<int>();
      int slot = entry["sram_slot"].get<int>();
      out.sram_full_map[{ch, ly, bk, row}] = slot;
    }
  }

  // ── Tables 3 & 4: per-bank Layer B + Layer C ──────────────────────────────
  // JSON: "banks": [{"ch":…,"ly":…,"bk":…,
  //                  "layer_b_ded_rows":[row,…],
  //                  "layer_c_burst":[{…},…]}, …]
  if (j.contains("banks")) {
    for (auto& bank_entry : j["banks"]) {
      int ch = bank_entry["ch"].get<int>();
      int ly = bank_entry["ly"].get<int>();
      int bk = bank_entry["bk"].get<int>();

      // Table 3: Layer B — DED rows
      // offset 0 → spare row 0, offset 1 → spare row 1
      if (bank_entry.contains("layer_b_ded_rows")) {
        int offset = 0;
        for (auto& row_val : bank_entry["layer_b_ded_rows"]) {
          int row = row_val.get<int>();
          out.ded_row_map[{ch, ly, bk, row}] = offset;
          offset++;
        }
      }

      // Table 4: Layer C — Burst col-range remap
      if (bank_entry.contains("layer_c_burst")) {
        for (auto& burst_val : bank_entry["layer_c_burst"]) {
          BurstEntry be;
          be.row         = burst_val["row"].get<int>();
          be.col_start   = burst_val["col_start"].get<int>();
          be.length      = burst_val["length"].get<int>();
          be.target_slot = burst_val["target_slot"].get<int>();
          out.burst_map[{ch, ly, bk, be.row, be.col_start}] = be;
        }
      }
    }
  }

  return true;
}

void HbmRepairTable::print_summary(std::ostream& os) const {
  os << "========================================\n";
  os << "[RepairTable] LOAD SUMMARY\n";
  os << "  hbm_id           = " << hbm_id << "\n";
  os << "  K (vacuum rows)  = " << K      << "\n";
  os << "----------------------------------------\n";
  os << "  Table 1 | Layer D bad banks  : " << bad_bank_set.size()  << " banks\n";
  os << "  Table 2 | Layer A SRAM rows  : " << sram_full_map.size() << " rows\n";
  os << "  Table 3 | Layer B DED rows   : " << ded_row_map.size()   << " rows\n";
  os << "  Table 4 | Layer C burst segs : " << burst_map.size()     << " segments\n";
  os << "========================================\n";
}

void HbmRepairTable::print_detail(std::ostream& os) const {
  print_summary(os);  // 先印摘要

  // ── Table 1: Layer D bad banks ──────────────────────────────────
  os << "\n[Table 1] Layer D — Bad Banks (" << bad_bank_set.size() << " total)\n";
  os << "  idx |  ch   ly   bk\n";
  os << "  ----+---------------\n";
  int i = 0;
  for (auto& [ch, ly, bk] : bad_bank_set) {
    os << "  " << std::setw(3) << i++ << " | "
       << std::setw(3) << ch << "  "
       << std::setw(3) << ly << "  "
       << std::setw(3) << bk << "\n";
  }

  // ── Table 2: Layer A SRAM ────────────────────────────────────────
  os << "\n[Table 2] Layer A — SRAM Map (" << sram_full_map.size() << " total)\n";
  os << "  idx |  ch   ly   bk    row  slot\n";
  os << "  ----+-----------------------------\n";
  i = 0;
  for (auto& [key, slot] : sram_full_map) {
    auto& [ch, ly, bk, row] = key;
    os << "  " << std::setw(3) << i++ << " | "
       << std::setw(3) << ch << "  "
       << std::setw(3) << ly << "  "
       << std::setw(3) << bk << "  "
       << std::setw(6) << row << "  "
       << std::setw(3) << slot << "\n";
  }

  // ── Table 3: Layer B DED rows ────────────────────────────────────
  os << "\n[Table 3] Layer B — DED Rows (" << ded_row_map.size() << " total)\n";
  os << "  idx |  ch   ly   bk    row  spare_offset\n";
  os << "  ----+-------------------------------------\n";
  i = 0;
  for (auto& [key, offset] : ded_row_map) {
    auto& [ch, ly, bk, row] = key;
    os << "  " << std::setw(3) << i++ << " | "
       << std::setw(3) << ch << "  "
       << std::setw(3) << ly << "  "
       << std::setw(3) << bk << "  "
       << std::setw(6) << row << "  "
       << std::setw(3) << offset << "\n";
  }

  // ── Table 4: Layer C burst ───────────────────────────────────────
  os << "\n[Table 4] Layer C — Burst Segments (" << burst_map.size() << " total)\n";
  os << "  idx |  ch   ly   bk    row  col_start  len  slot\n";
  os << "  ----+--------------------------------------------\n";
  i = 0;
  for (auto& [key, be] : burst_map) {
    auto& [ch, ly, bk, row, col] = key;
    os << "  " << std::setw(3) << i++ << " | "
       << std::setw(3) << ch << "  "
       << std::setw(3) << ly << "  "
       << std::setw(3) << bk << "  "
       << std::setw(6) << row << "  "
       << std::setw(6) << be.col_start << "  "
       << std::setw(3) << be.length << "  "
       << std::setw(4) << be.target_slot << "\n";
  }
  os << "========================================\n";
}

} // namespace Ramulator