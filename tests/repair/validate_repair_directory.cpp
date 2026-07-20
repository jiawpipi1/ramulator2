// Validate every production repair table in a directory with the runtime loader.
// Usage: validate_repair_directory DIR EXPECTED_COUNT EXPECTED_CHANNELS [EXPECTED_SLOTS]

#include <filesystem>
#include <iostream>
#include <set>
#include <string>

#include "dram_controller/impl/repair/repair_table.h"

namespace fs = std::filesystem;
using Ramulator::HbmRepairTable;

int main(int argc, char** argv) {
  if (argc != 4 && argc != 5) {
    std::cerr << "usage: " << argv[0]
              << " DIR EXPECTED_COUNT EXPECTED_CHANNELS [EXPECTED_SLOTS]\n";
    return 2;
  }
  const fs::path directory = argv[1];
  const int expected_count = std::stoi(argv[2]);
  const int expected_channels = std::stoi(argv[3]);
  const int expected_slots = argc == 5 ? std::stoi(argv[4]) : 16;
  if (!fs::is_directory(directory)) {
    std::cerr << "not a directory: " << directory << "\n";
    return 2;
  }

  int files = 0;
  std::set<int> ids;
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json")
      continue;
    HbmRepairTable table;
    if (!HbmRepairTable::load_from_json(entry.path().string(), table)) {
      std::cerr << "runtime loader rejected: " << entry.path() << "\n";
      return 1;
    }
    if (table.cfg.num_channels != expected_channels ||
        table.cfg.rows_per_bank != 16384 || table.cfg.bursts_per_row != 32 ||
        table.cfg.vacuum_limit != 512 || table.cfg.sram_slots != expected_slots ||
        table.cfg.total_spare_rows != 4) {
      std::cerr << "unexpected config in: " << entry.path() << "\n";
      return 1;
    }
    const std::string expected_name =
        "remap_hbm_" + std::to_string(table.hbm_id) + ".json";
    if (entry.path().filename() != expected_name || !ids.insert(table.hbm_id).second) {
      std::cerr << "filename/id mismatch or duplicate id: " << entry.path() << "\n";
      return 1;
    }
    ++files;
  }
  if (files != expected_count) {
    std::cerr << "expected " << expected_count << " tables, found " << files << "\n";
    return 1;
  }
  std::cout << "PASS: runtime loader accepted " << files << " tables from "
            << directory << " (" << expected_channels << " channels)\n";
  return 0;
}
