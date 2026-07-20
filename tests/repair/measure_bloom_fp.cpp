// Measure the runtime blocked-Bloom false-positive rate on live-bank negative keys.
// Usage: measure_bloom_fp TABLE.json [NEGATIVE_SAMPLES]

#include <cstdint>
#include <iostream>
#include <random>
#include <set>
#include <string>

#include "dram_controller/impl/repair/repair_table.h"
#include "dram_controller/impl/repair/repair_translator.h"

using namespace Ramulator;

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "usage: " << argv[0] << " TABLE.json [NEGATIVE_SAMPLES]\n";
    return 2;
  }
  const uint64_t wanted = argc == 3 ? std::stoull(argv[2]) : 300000;
  HbmRepairTable table;
  if (!HbmRepairTable::load_from_json(argv[1], table)) return 1;

  std::set<RowKey> exact;
  for (const auto& [key, value] : table.sram_full_map) exact.insert(key);
  for (const auto& [key, value] : table.ded_row_map) exact.insert(key);
  for (const auto& [key, value] : table.burst_map) {
    const auto& [ch, pch, bg, ba, row, col] = key;
    exact.insert({ch, pch, bg, ba, row});
  }

  RepairTranslator translator(table, true, 16, 8, true);
  std::mt19937_64 rng(0x575257424c4f4f4dULL);
  uint64_t negatives = 0, false_positives = 0;
  while (negatives < wanted) {
    int ch = rng() % table.cfg.num_channels;
    int pch = rng() % table.cfg.num_pch;
    int bg = rng() % table.cfg.num_bg;
    int ba = rng() % table.cfg.num_ba;
    int row = rng() % table.cfg.rows_per_bank;
    BankKey bank{ch, pch, bg, ba};
    RowKey key{ch, pch, bg, ba, row};
    if (table.bad_bank_set.count(bank) || exact.count(key)) continue;
    AddrVec_t av{ch, pch, bg, ba, row, 0};
    translator.translate(av);
    if (!translator.last_bloom_reject()) ++false_positives;
    ++negatives;
  }

  const size_t capacity_entries = table.sram_full_map.size()
                                + table.ded_row_map.size()
                                + table.burst_map.size();
  std::cout << "table=" << table.hbm_id
            << " unique_keys=" << exact.size()
            << " capacity_entries=" << capacity_entries
            << " bloom_bytes=" << translator.bloom_num_bits() / 8
            << " negatives=" << negatives
            << " false_positives=" << false_positives
            << " fp_pct=" << (100.0 * false_positives / negatives) << "\n";
  return 0;
}
