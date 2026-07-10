// repair_table.cpp
// Loads HBM repair tables from JSON (pch/bg/ba format).
//
// Compile & link with nlohmann/json (header-only).

#include "dram_controller/impl/repair/repair_table.h"

#include <fstream>
#include <iomanip>
#include <stdexcept>

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
  out.K      = j.value("K",       0);

  // Optional "config" block. When present it makes the table self-describing
  // (spare-row base addresses and Layer D geometry no longer depend on the
  // compiled-in defaults matching the repairv2 run). Absent -> keep defaults.
  if (j.contains("config")) {
    const auto& c = j["config"];
    RepairConfig& cfg = out.cfg;
    cfg.total_spare_rows = c.value("total_spare_rows", cfg.total_spare_rows);
    cfg.bursts_per_row   = c.value("bursts_per_row",   cfg.bursts_per_row);
    cfg.sram_slots       = c.value("sram_slots",       cfg.sram_slots);
    cfg.rows_per_bank    = c.value("rows_per_bank",    cfg.rows_per_bank);
    cfg.num_channels     = c.value("num_channels",     cfg.num_channels);
    cfg.num_pch          = c.value("num_pch",          cfg.num_pch);
    cfg.num_bg           = c.value("num_bg",           cfg.num_bg);
    cfg.num_ba           = c.value("num_ba",           cfg.num_ba);
    cfg.ded_count        = cfg.total_spare_rows / 2;
    cfg.frag_count       = cfg.total_spare_rows - cfg.ded_count;
  }

  if (j.contains("layer_d_bad_banks")) {
    for (auto& e : j["layer_d_bad_banks"]) {
      int ch  = e["ch"].get<int>();
      int pch = e["pch"].get<int>();
      int bg  = e["bg"].get<int>();
      int ba  = e["ba"].get<int>();
      out.bad_bank_set.insert({ch, pch, bg, ba});
    }
  }

  if (j.contains("layer_a_sram")) {
    for (auto& e : j["layer_a_sram"]) {
      int ch   = e["ch"].get<int>();
      int pch  = e["pch"].get<int>();
      int bg   = e["bg"].get<int>();
      int ba   = e["ba"].get<int>();
      int row  = e["row"].get<int>();
      int slot = e["sram_slot"].get<int>();
      out.sram_full_map[{ch, pch, bg, ba, row}] = slot;
    }
  }

  if (j.contains("banks")) {
    for (auto& bank_entry : j["banks"]) {
      int ch  = bank_entry["ch"].get<int>();
      int pch = bank_entry["pch"].get<int>();
      int bg  = bank_entry["bg"].get<int>();
      int ba  = bank_entry["ba"].get<int>();

      
      if (bank_entry.contains("layer_b_ded_rows")) {
        int offset = 0;
        for (auto& row_val : bank_entry["layer_b_ded_rows"]) {
          int row = row_val.get<int>();
          out.ded_row_map[{ch, pch, bg, ba, row}] = offset++;
        }
      }

      
      if (bank_entry.contains("layer_c_burst")) {
        for (auto& burst_val : bank_entry["layer_c_burst"]) {
          BurstEntry be;
          be.row         = burst_val["row"].get<int>();
          be.col_start   = burst_val["col_start"].get<int>();
          be.length      = burst_val["length"].get<int>();
          be.target_slot = burst_val["target_slot"].get<int>();
          out.burst_map[{ch, pch, bg, ba, be.row, be.col_start}] = be;
        }
      }
    }
  }

  return true;
}

void HbmRepairTable::print_summary(std::ostream& os) const {
  os << "========================================\n"
     << "[RepairTable] LOAD SUMMARY\n"
     << "  hbm_id           = " << hbm_id << "\n"
     << "  K (vacuum rows)  = " << K      << "\n"
     << "----------------------------------------\n"
     << "  Table 1 | Layer D bad banks  : " << bad_bank_set.size()  << " banks\n"
     << "  Table 2 | Layer A SRAM rows  : " << sram_full_map.size() << " rows\n"
     << "  Table 3 | Layer B DED rows   : " << ded_row_map.size()   << " rows\n"
     << "  Table 4 | Layer C burst segs : " << burst_map.size()     << " segments\n"
     << "========================================\n";
}

void HbmRepairTable::print_detail(std::ostream& os) const {
  print_summary(os);

  os << "\n[Table 1] Layer D ??? Bad Banks (" << bad_bank_set.size() << " total)\n";
  os << "  idx |  ch  pch   bg   ba\n";
  os << "  ----+--------------------\n";
  int i = 0;
  for (auto& [ch, pch, bg, ba] : bad_bank_set) {
    os << "  " << std::setw(3) << i++ << " | "
       << std::setw(3) << ch  << "  "
       << std::setw(3) << pch << "  "
       << std::setw(3) << bg  << "  "
       << std::setw(3) << ba  << "\n";
  }

  os << "\n[Table 2] Layer A ??? SRAM Map (" << sram_full_map.size() << " total)\n";
  os << "  idx |  ch  pch   bg   ba    row  slot\n";
  os << "  ----+---------------------------------\n";
  i = 0;
  for (auto& [key, slot] : sram_full_map) {
    auto& [ch, pch, bg, ba, row] = key;
    os << "  " << std::setw(3) << i++ << " | "
       << std::setw(3) << ch  << "  "
       << std::setw(3) << pch << "  "
       << std::setw(3) << bg  << "  "
       << std::setw(3) << ba  << "  "
       << std::setw(6) << row << "  "
       << std::setw(3) << slot << "\n";
  }

  os << "\n[Table 3] Layer B ??? DED Rows (" << ded_row_map.size() << " total)\n";
  os << "  idx |  ch  pch   bg   ba    row  spare_offset\n";
  os << "  ----+------------------------------------------\n";
  i = 0;
  for (auto& [key, offset] : ded_row_map) {
    auto& [ch, pch, bg, ba, row] = key;
    os << "  " << std::setw(3) << i++ << " | "
       << std::setw(3) << ch  << "  "
       << std::setw(3) << pch << "  "
       << std::setw(3) << bg  << "  "
       << std::setw(3) << ba  << "  "
       << std::setw(6) << row << "  "
       << std::setw(3) << offset << "\n";
  }

  os << "\n[Table 4] Layer C ??? Burst Segments (" << burst_map.size() << " total)\n";
  os << "  idx |  ch  pch   bg   ba    row  col_start  len  slot\n";
  os << "  ----+--------------------------------------------------\n";
  i = 0;
  for (auto& [key, be] : burst_map) {
    auto& [ch, pch, bg, ba, row, col] = key;
    os << "  " << std::setw(3) << i++ << " | "
       << std::setw(3) << ch  << "  "
       << std::setw(3) << pch << "  "
       << std::setw(3) << bg  << "  "
       << std::setw(3) << ba  << "  "
       << std::setw(6) << row << "  "
       << std::setw(6) << be.col_start << "  "
       << std::setw(3) << be.length    << "  "
       << std::setw(4) << be.target_slot << "\n";
  }
  os << "========================================\n";
}

} // namespace Ramulator
