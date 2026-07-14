// test_json_roundtrip.cpp
//
// Verifies HbmRepairTable::load_from_json parses a repairv2-style JSON table
// identically into the in-memory tables, that the loaded table drives
// translate() correctly, and that a real repairv2 output file is consumable.
//
// Build (from ramulator2/):
//   g++ -std=c++17 -Itests/repair/stubs -Isrc -Iext \
//       tests/repair/test_json_roundtrip.cpp \
//       src/dram_controller/impl/repair/repair_table.cpp \
//       -o tests/repair/test_json_roundtrip

#include <fstream>
#include <iostream>
#include <string>

#include "dram_controller/impl/repair/repair_table.h"
#include "dram_controller/impl/repair/repair_translator.h"

using namespace Ramulator;

static int g_pass = 0, g_fail = 0;
static void check(bool cond, const std::string& name) {
    if (cond) { ++g_pass; std::cout << "  [PASS] " << name << "\n"; }
    else      { ++g_fail; std::cout << "  [FAIL] " << name << "\n"; }
}
static AddrVec_t A(int ch,int pch,int bg,int ba,int row,int col){
    return AddrVec_t{ch,pch,bg,ba,row,col};
}

static const char* KNOWN_JSON = R"JSON(
{
  "hbm_id": 7,
  "K": 65,
  "config": {"rows_per_bank":16384,"total_spare_rows":4,"sram_slots":16,
             "vacuum_limit":128,"bursts_per_row":32,
             "num_channels":8,"num_pch":2,"num_bg":4,"num_ba":4},
  "layer_d_bad_banks": [ {"ch":0,"pch":0,"bg":0,"ba":0} ],
  "layer_a_sram": [ {"ch":3,"pch":1,"bg":0,"ba":2,"row":500,"sram_slot":6} ],
  "banks": [
    { "ch":4,"pch":0,"bg":1,"ba":3,
      "layer_b_ded_rows": [300, 305],
      "layer_c_burst": [ {"row":600,"col_start":8,"length":4,"target_slot":2},
                         {"row":600,"col_start":20,"length":2,"target_slot":40} ] }
  ]
}
)JSON";

int main(int argc, char** argv) {
    // ---- (a) known JSON round-trip -----------------------------------------
    std::string known_path = "tests/repair/_known_tmp.json";
    { std::ofstream o(known_path); o << KNOWN_JSON; }

    HbmRepairTable t;
    bool loaded = HbmRepairTable::load_from_json(known_path, t);
    std::cout << "== Known-JSON load & parse ==\n";
    check(loaded, "load_from_json returns true");
    check(t.hbm_id == 7, "hbm_id == 7");
    check(t.K == 65, "K == exact runtime reservation 65");
    check(t.cfg.num_channels==8 && t.cfg.num_pch==2 && t.cfg.num_bg==4 && t.cfg.num_ba==4
          && t.cfg.rows_per_bank==16384, "config block parsed (geometry)");
    check(t.bad_bank_set.size() == 1 && t.bad_bank_set.count({0,0,0,0})==1, "Layer D: {(0,0,0,0)}");
    check(t.sram_full_map.size()==1 && t.sram_full_map.at({3,1,0,2,500})==6, "Layer A: (3,1,0,2,500)->slot6");
    check(t.ded_row_map.size()==2 && t.ded_row_map.at({4,0,1,3,300})==0
          && t.ded_row_map.at({4,0,1,3,305})==1, "Layer B: rows 300->off0, 305->off1");
    check(t.burst_map.size()==2, "Layer C: 2 burst segments");
    if (t.burst_map.count({4,0,1,3,600,8}) && t.burst_map.count({4,0,1,3,600,20})) {
        const BurstEntry& b0 = t.burst_map.at({4,0,1,3,600,8});
        const BurstEntry& b1 = t.burst_map.at({4,0,1,3,600,20});
        check(b0.length==4 && b0.target_slot==2, "Layer C seg0 len4 slot2");
        check(b1.length==2 && b1.target_slot==40, "Layer C seg1 len2 slot40");
    } else { check(false, "Layer C keys present"); }

    // Invalid tables must fail atomically: the caller keeps the last known-good
    // table instead of receiving partially parsed state.
    std::cout << "== Invalid-table rejection ==\n";
    auto invalid_load = [&](std::string body, const std::string& from,
                            const std::string& to, const std::string& name) {
        const auto pos = body.find(from);
        if (pos == std::string::npos) { check(false, name + " fixture"); return; }
        body.replace(pos, from.size(), to);
        { std::ofstream o(known_path); o << body; }
        const int old_id = t.hbm_id;
        const auto old_bad = t.bad_bank_set;
        check(!HbmRepairTable::load_from_json(known_path, t) &&
              t.hbm_id == old_id && t.bad_bank_set == old_bad, name);
    };
    invalid_load(KNOWN_JSON, "\"K\": 65", "\"K\": 64",
                 "reject inexact K without mutating output");
    invalid_load(KNOWN_JSON, "\"col_start\":20", "\"col_start\":10",
                 "reject overlapping Layer C source ranges");
    { std::ofstream o(known_path); o << KNOWN_JSON; }

    // ---- loaded table drives translate() correctly -------------------------
    std::cout << "== Loaded table -> translate() ==\n";
    RepairTranslator T(t);
    const int RPB = t.cfg.rows_per_bank;

    auto exp = [&](AddrVec_t av, RepairType wt, AddrVec_t wav, const std::string& n){
        RepairType got = T.translate(av);
        check(got==wt && av==wav, n);
        if (!(got==wt && av==wav)) {
            std::cout << "    got " << repair_type_name(got) << " row=" << av[AIDX_ROW]
                      << " col=" << av[AIDX_COL] << "\n";
        }
    };
    // Layer D relocates dead (0,0,0,0) to live[0]=(0,0,0,1)'s vacuum band.
    // F=1, L=255, h=ceil(16384/255)=65; row 25 -> o=25 -> row RPB-65+25.
    check(T.num_dead()==1 && T.num_live()==255 && T.band_h()==65, "D geometry from JSON: F=1 L=255 h=65");
    exp(A(0,0,0,0, 25, 3), RepairType::LAYER_D, A(0,0,0,1, RPB - T.band_h() + 25, 3), "D relocate row=25");
    exp(A(3,1,0,2, 500,7), RepairType::LAYER_A, A(3,1,0,2, RPB+t.cfg.total_spare_rows+6, 7), "A slot6");
    exp(A(4,0,1,3, 300,1), RepairType::LAYER_B, A(4,0,1,3, RPB+0, 1), "B row300 off0");
    exp(A(4,0,1,3, 305,1), RepairType::LAYER_B, A(4,0,1,3, RPB+1, 1), "B row305 off1");
    exp(A(4,0,1,3, 600,9), RepairType::LAYER_C, A(4,0,1,3, RPB+t.cfg.ded_count+0, 3), "C seg0 preserves offset");
    exp(A(4,0,1,3, 600,21),RepairType::LAYER_C, A(4,0,1,3, RPB+t.cfg.ded_count+1, 9), "C seg1 preserves offset in frag1");
    std::remove(known_path.c_str());

    // ---- (b) real repairv2 sample is consumable ----------------------------
    std::string real = (argc > 1) ? argv[1] : "";
    std::cout << "== Real repairv2 sample (" << real << ") ==\n";
    std::ifstream probe(real);
    if (!probe.good()) {
        std::cout << "  [SKIP] sample not found (pass path as arg1)\n";
    } else {
        probe.close();
        HbmRepairTable r;
        bool ok = HbmRepairTable::load_from_json(real, r);
        check(ok, "real sample loads without parse error");
        check(r.hbm_id >= 0, "real sample has hbm_id");
        check(r.cfg.num_channels==16 && r.cfg.rows_per_bank==16384,
              "real sample config block parsed (HBM3 geometry)");
        check(r.bad_bank_set.size() + r.sram_full_map.size()
              + r.ded_row_map.size() + r.burst_map.size() > 0, "real sample has non-empty tables");
        r.print_summary(std::cout);
    }

    std::cout << "\n================ RESULT ================\n";
    std::cout << "  passed: " << g_pass << "   failed: " << g_fail << "\n";
    std::cout << "========================================\n";
    return g_fail == 0 ? 0 : 1;
}
