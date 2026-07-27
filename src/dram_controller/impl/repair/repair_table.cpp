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

  try {
    json j;
    f >> j;
    HbmRepairTable tmp;
    auto fail = [](const std::string& msg) -> void { throw std::runtime_error(msg); };
    auto power2 = [](int v) { return v > 0 && (v & (v - 1)) == 0; };

    tmp.hbm_id = j.at("hbm_id").get<int>();
    tmp.K      = j.at("K").get<int>();
    const auto& c = j.at("config");
    RepairConfig& cfg = tmp.cfg;
    cfg.total_spare_rows = c.at("total_spare_rows").get<int>();
    cfg.bursts_per_row   = c.at("bursts_per_row").get<int>();
    cfg.transaction_bytes = c.value("transaction_bytes", 32);
    cfg.sram_slots       = c.at("sram_slots").get<int>();
    const std::string layer_a_granularity = c.value("layer_a_granularity", "row");
    if (layer_a_granularity != "row" && layer_a_granularity != "transaction")
      fail("unsupported Layer A granularity");
    cfg.layer_a_transaction_granularity = layer_a_granularity == "transaction";
    cfg.rows_per_bank    = c.at("rows_per_bank").get<int>();
    cfg.vacuum_limit     = c.at("vacuum_limit").get<int>();
    cfg.num_channels     = c.at("num_channels").get<int>();
    cfg.num_pch          = c.at("num_pch").get<int>();
    cfg.num_bg           = c.at("num_bg").get<int>();
    cfg.num_ba           = c.at("num_ba").get<int>();
    cfg.ded_count        = cfg.total_spare_rows / 2;
    cfg.frag_count       = cfg.total_spare_rows - cfg.ded_count;
    if (tmp.hbm_id < 0 || tmp.K < 0 || cfg.total_spare_rows < 0 ||
        cfg.sram_slots < 0 || cfg.vacuum_limit < 0 ||
        !power2(cfg.bursts_per_row) || !power2(cfg.transaction_bytes) ||
        !power2(cfg.rows_per_bank) ||
        !power2(cfg.num_channels) || !power2(cfg.num_pch) ||
        !power2(cfg.num_bg) || !power2(cfg.num_ba))
      fail("invalid config or negative repair-table metadata");

    auto valid_bank = [&](int ch, int pch, int bg, int ba) {
      return ch >= 0 && ch < cfg.num_channels && pch >= 0 && pch < cfg.num_pch &&
             bg >= 0 && bg < cfg.num_bg && ba >= 0 && ba < cfg.num_ba;
    };
    const auto& bad_json = j.at("layer_d_bad_banks");
    if (!bad_json.is_array()) fail("layer_d_bad_banks must be an array");
    for (const auto& e : bad_json) {
      int ch=e.at("ch").get<int>(), pch=e.at("pch").get<int>();
      int bg=e.at("bg").get<int>(), ba=e.at("ba").get<int>();
      if (!valid_bank(ch,pch,bg,ba)) fail("Layer D bank is out of range");
      if (!tmp.bad_bank_set.insert({ch,pch,bg,ba}).second)
        fail("duplicate Layer D bank");
    }

    std::set<std::pair<int,int>> used_sram_slots;
    std::map<RowKey,std::set<int>> used_sram_source_cols;
    const auto& sram_json = j.at("layer_a_sram");
    if (!sram_json.is_array()) fail("layer_a_sram must be an array");
    for (const auto& e : sram_json) {
      int ch=e.at("ch").get<int>(), pch=e.at("pch").get<int>();
      int bg=e.at("bg").get<int>(), ba=e.at("ba").get<int>();
      int row=e.at("row").get<int>();
      BankKey bk{ch,pch,bg,ba};
      RowKey rk{ch,pch,bg,ba,row};
      if (!valid_bank(ch,pch,bg,ba) || row < 0 || row >= cfg.rows_per_bank ||
          tmp.bad_bank_set.count(bk))
        fail("Layer A entry is out of range or belongs to a dead bank");
      if (cfg.layer_a_transaction_granularity) {
        BurstEntry be;
        be.row = row;
        be.col_start = e.at("col_start").get<int>();
        be.length = e.at("length").get<int>();
        be.target_slot = e.at("target_slot").get<int>();
        BurstKey key{ch,pch,bg,ba,row,be.col_start};
        if (be.col_start < 0 || be.length <= 0 ||
            be.col_start + be.length > cfg.bursts_per_row ||
            be.target_slot < 0 || be.target_slot + be.length > cfg.sram_slots ||
            !tmp.sram_tx_map.emplace(key, be).second)
          fail("invalid or duplicate transaction-granular Layer A range");
        for (int col=be.col_start; col<be.col_start+be.length; ++col)
          if (!used_sram_source_cols[rk].insert(col).second)
            fail("Layer A source ranges overlap within a row");
        for (int slot=be.target_slot; slot<be.target_slot+be.length; ++slot)
          if (!used_sram_slots.insert({ch,slot}).second)
            fail("Layer A SRAM slot reused within a channel");
      } else {
        int slot=e.at("sram_slot").get<int>();
        if (slot < 0 || slot >= cfg.sram_slots ||
            !tmp.sram_full_map.emplace(rk, slot).second)
          fail("invalid or duplicate legacy Layer A row");
        if (!used_sram_slots.insert({ch,slot}).second)
          fail("Layer A SRAM slot reused within a channel");
      }
    }

    std::set<BankKey> seen_bank_records;
    std::map<BankKey,std::set<int>> used_burst_slots;
    std::map<RowKey,std::set<int>> used_source_cols;
    const auto& banks_json = j.at("banks");
    if (!banks_json.is_array()) fail("banks must be an array");
    for (const auto& bank_entry : banks_json) {
      int ch=bank_entry.at("ch").get<int>(), pch=bank_entry.at("pch").get<int>();
      int bg=bank_entry.at("bg").get<int>(), ba=bank_entry.at("ba").get<int>();
      BankKey bk{ch,pch,bg,ba};
      if (!valid_bank(ch,pch,bg,ba) || tmp.bad_bank_set.count(bk))
        fail("Layer B/C bank is out of range or dead");
      if (!seen_bank_records.insert(bk).second)
        fail("duplicate Layer B/C bank record");
      int offset = 0;
      for (const auto& row_val : bank_entry.at("layer_b_ded_rows")) {
        int row = row_val.get<int>();
        RowKey rk{ch,pch,bg,ba,row};
        if (row < 0 || row >= cfg.rows_per_bank || offset >= cfg.ded_count ||
            !tmp.ded_row_map.emplace(rk, offset).second)
          fail("invalid or duplicate Layer B row");
        ++offset;
      }
      for (const auto& burst_val : bank_entry.at("layer_c_burst")) {
        BurstEntry be;
        be.row=burst_val.at("row").get<int>();
        be.col_start=burst_val.at("col_start").get<int>();
        be.length=burst_val.at("length").get<int>();
        be.target_slot=burst_val.at("target_slot").get<int>();
        BurstKey key{ch,pch,bg,ba,be.row,be.col_start};
        if (be.row < 0 || be.row >= cfg.rows_per_bank || be.col_start < 0 ||
            be.length <= 0 || be.col_start + be.length > cfg.bursts_per_row ||
            be.target_slot < 0 ||
            be.target_slot + be.length > cfg.frag_count * cfg.bursts_per_row ||
            !tmp.burst_map.emplace(key, be).second)
          fail("invalid or duplicate Layer C range");
        for (int slot=be.target_slot; slot<be.target_slot+be.length; ++slot)
          if (!used_burst_slots[bk].insert(slot).second)
            fail("Layer C target slot reused within a bank");
        RowKey rk{ch,pch,bg,ba,be.row};
        for (int col=be.col_start; col<be.col_start+be.length; ++col)
          if (!used_source_cols[rk].insert(col).second)
            fail("Layer C source ranges overlap within a row");
      }
    }

    // A/B/C allocation remains mutually exclusive at source-row granularity,
    // even though Layer A stores only the faulty transaction units of its rows.
    std::set<RowKey> layer_a_rows;
    for (const auto& [rk, slot] : tmp.sram_full_map) layer_a_rows.insert(rk);
    for (const auto& [key, be] : tmp.sram_tx_map) {
      auto [ch,pch,bg,ba,row,col] = key;
      layer_a_rows.insert({ch,pch,bg,ba,row});
    }
    for (const auto& rk : layer_a_rows)
      if (tmp.ded_row_map.count(rk)) fail("row appears in both Layer A and B");
    for (const auto& [key, be] : tmp.burst_map) {
      auto [ch,pch,bg,ba,row,col] = key;
      RowKey rk{ch,pch,bg,ba,row};
      if (layer_a_rows.count(rk) || tmp.ded_row_map.count(rk))
        fail("row appears in Layer C and a whole-row repair layer");
    }

    const int F = static_cast<int>(tmp.bad_bank_set.size());
    const int L = cfg.total_banks() - F;
    if (L <= 0) fail("repair table has no live banks");
    const int h = (cfg.rows_per_bank + L - 1) / L;
    const long long required = 1LL * F * h;
    if (required != tmp.K) fail("K does not equal F*ceil(rows_per_bank/live_banks)");
    if (required > cfg.vacuum_limit) fail("repair table exceeds vacuum_limit");

    out = std::move(tmp);
    return true;
  } catch (const std::exception& e) {
    std::cerr << "[RepairTable] invalid JSON/table: " << e.what() << "\n";
    return false;
  }
}

void HbmRepairTable::print_summary(std::ostream& os) const {
  os << "========================================\n"
     << "[RepairTable] LOAD SUMMARY\n"
     << "  hbm_id           = " << hbm_id << "\n"
     << "  K (vacuum rows)  = " << K      << "\n"
     << "----------------------------------------\n"
     << "  Table 1 | Layer D bad banks  : " << bad_bank_set.size()  << " banks\n"
     << "  Table 2 | Layer A SRAM ranges: "
     << (sram_tx_map.size() + sram_full_map.size()) << " entries\n"
     << "  Table 3 | Layer B DED rows   : " << ded_row_map.size()   << " rows\n"
     << "  Table 4 | Layer C burst segs : " << burst_map.size()     << " segments\n"
     << "========================================\n";
}

void HbmRepairTable::print_detail(std::ostream& os) const {
  print_summary(os);

  os << "\n[Table 1] Layer D Bad Banks (" << bad_bank_set.size() << " total)\n";
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

  os << "\n[Table 2] Layer A SRAM Map ("
     << (sram_tx_map.size() + sram_full_map.size()) << " total)\n";
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
  for (auto& [key, be] : sram_tx_map) {
    auto& [ch, pch, bg, ba, row, col] = key;
    os << "  " << std::setw(3) << i++ << " | "
       << std::setw(3) << ch  << "  "
       << std::setw(3) << pch << "  "
       << std::setw(3) << bg  << "  "
       << std::setw(3) << ba  << "  "
       << std::setw(6) << row << "  col=" << std::setw(2) << col
       << " len=" << std::setw(2) << be.length
       << " slot=" << std::setw(3) << be.target_slot << "\n";
  }

  os << "\n[Table 3] Layer B DED Rows (" << ded_row_map.size() << " total)\n";
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

  os << "\n[Table 4] Layer C Burst Segments (" << burst_map.size() << " total)\n";
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
