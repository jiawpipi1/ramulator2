// test_repair_translator.cpp
//
// Standalone functional-correctness unit test for RepairTranslator.
// Builds an HbmRepairTable in memory (no JSON, no full Ramulator build) and
// checks every layer's translation, boundary conditions, the Layer D
// dead-bank relocation under both spread and clustered placement,
// fall-through to A/B/C, and structural invariants.
//
// Build (from ramulator2/):
//   g++ -std=c++17 -Itests/repair/stubs -Isrc \
//       tests/repair/test_repair_translator.cpp -o tests/repair/test_repair_translator
//
// Addr_vec layout: [0]=ch [1]=pch [2]=bg [3]=ba [4]=row [5]=col

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

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

static void dump_av(const AddrVec_t& av){
    std::cout << "[";
    for (size_t i=0;i<av.size();++i) std::cout << av[i] << (i+1<av.size()?",":"");
    std::cout << "]";
}

// Expect a specific repair type and resulting address vector.
static void expect(RepairTranslator& T, AddrVec_t av,
                   RepairType want_type, AddrVec_t want_av, const std::string& name) {
    RepairType got = T.translate(av);
    bool ok = (got == want_type) && (av == want_av);
    if (!ok) {
        std::cout << "    got type=" << repair_type_name(got) << " av="; dump_av(av);
        std::cout << "  want type=" << repair_type_name(want_type) << " av="; dump_av(want_av);
        std::cout << "\n";
    }
    check(ok, name);
}

// Expect a Layer D relocation resulting in a specific address vector.
static void expect_D(RepairTranslator& T, AddrVec_t av, AddrVec_t want_av, const std::string& name) {
    expect(T, av, RepairType::LAYER_D, want_av, name);
}

int main() {
    HbmRepairTable t;
    t.hbm_id = 0;
    t.K      = 530;

    // Geometry: 8 ch x 1 pch x 1 bg x 8 ba = 64 banks. Enumeration order is
    // (ch, ly=0, ba) so global index = ch*8 + ba.
    t.cfg.num_channels = 8;
    t.cfg.num_pch      = 1;
    t.cfg.num_bg       = 1;
    t.cfg.num_ba       = 8;
    t.cfg.vacuum_limit = 1024;
    t.cfg.layer_a_transaction_granularity = true;
    const RepairConfig& c = t.cfg;      // rpb=16384, tsr=4, ded=2, frag=2, bpr=32, sram=32
    const int RPB = c.rows_per_bank;    // 16384

    // Layer D: dead banks (0,0,0,0)=ord0 and (0,0,0,1)=ord1.
    // -> F=2, L=62, band h=ceil(16384/62)=265. live[0]=(0,0,0,2), live[1]=(0,0,0,3),
    //    live[61]=global idx 63=(7,0,0,7). dead0 band base=RPB-265=16119; dead1=RPB-530=15854.
    t.bad_bank_set.insert({0,0,0,0});
    t.bad_bank_set.insert({0,0,0,1});

    // Layer A: transaction-granular ranges on normal (live) banks.
    auto addA = [&](int ch,int pch,int bg,int ba,int row,int cs,int len,int slot){
        t.sram_tx_map[{ch,pch,bg,ba,row,cs}] = BurstEntry{row,cs,len,slot};
    };
    addA(1,0,0,3,512,5,2,4);
    addA(3,0,0,1,60,4,1,2);   // beats B only on the faulty transaction
    // fall-through target: dead1 row2 relocates to (0,0,0,2,15856)
    addA(0,0,0,2,15856,7,1,3);

    // Layer B
    t.ded_row_map[{1,0,0,3,800}] = 0;
    t.ded_row_map[{1,0,0,3,801}] = 1;
    t.ded_row_map[{3,0,0,1,60}]  = 0;     // shadowed by A
    t.ded_row_map[{3,0,0,2,70}]  = 0;     // beats C
    // fall-through target: dead1 row0 relocates to (0,0,0,2,15854)
    t.ded_row_map[{0,0,0,2,15854}] = 1;

    // Layer C
    auto addC = [&](int ch,int pch,int bg,int ba,int row,int cs,int len,int slot){
        t.burst_map[{ch,pch,bg,ba,row,cs}] = BurstEntry{row,cs,len,slot};
    };
    addC(1,0,0,3, 900, 8,4,0);
    addC(1,0,0,3, 900,12,4,4);
    addC(1,0,0,3, 900,20,2,8);
    addC(3,0,0,2, 70, 8,4,3);             // shadowed by B
    // fall-through target: dead1 row1 relocates to (0,0,0,2,15855)
    addC(0,0,0,2, 15855, 8,4,0);

    RepairTranslator T(t);

    const int A_base = RPB + c.total_spare_rows;   // 16388
    const int B_base = RPB;                         // 16384
    const int C_base = RPB + c.ded_count;           // 16386

    std::cout << "== Layer D (dead-bank relocation, then fall-through) ==\n";
    check(T.num_dead()==2 && T.num_live()==62 && T.band_h()==265, "D geometry: F=2 L=62 h=265");
    check(T.vacuum_rows_per_bank()==530, "D vacuum region = F*h = 530 rows/live bank");
    // plain relocation (target top row is clean -> A/B/C miss -> returns LAYER_D)
    expect_D(T, A(0,0,0,0, 0,   5), A(0,0,0,2, 16119, 5), "D dead0 row0 -> live0 top band");
    expect_D(T, A(0,0,0,0, 264, 5), A(0,0,0,2, 16383, 5), "D dead0 row264 -> band end");
    expect_D(T, A(0,0,0,0, 265, 5), A(0,0,0,3, 16119, 5), "D dead0 row265 -> next live bank");
    expect_D(T, A(0,0,0,0,16383,5), A(7,0,0,7, 16337, 5), "D dead0 last row -> live[61]");
    expect_D(T, A(0,0,0,1, 3,   5), A(0,0,0,2, 15857, 5), "D dead1 row3 -> lower band");
    // fall-through: relocated top row is itself faulty -> A/B/C also applied, still reports LAYER_D
    expect_D(T, A(0,0,0,1, 0,   5), A(0,0,0,2, B_base+1, 5), "D->B fall-through (dead1 row0)");
    expect_D(T, A(0,0,0,1, 1,   9), A(0,0,0,2, C_base,   1), "D->C fall-through preserves range offset");
    expect_D(T, A(0,0,0,1, 2,   7), A(0,0,0,2, A_base, 3), "D->A fall-through (dead1 row2)");

    std::cout << "== Layer D clustered placement (opt-in; spread remains default) ==\n";
    HbmRepairTable tc = t;
    // Balanced exclusive target selection: F=2, L=62, q=16, M=32.
    // dead1 row0 -> slot1 -> floor(1*62/32)=live[1]=(0,0,0,3).
    // Make that target faulty to prove clustered D->A/B/C fall-through too.
    tc.ded_row_map[{0,0,0,3,RPB-1024}] = 0;
    RepairTranslator TC(tc, true, 16, 8, true, LayerDMapping::CLUSTERED);
    check(T.layer_d_mapping() == LayerDMapping::SPREAD,
          "spread remains the constructor default");
    check(TC.layer_d_mapping() == LayerDMapping::CLUSTERED,
          "clustered placement is explicitly selectable");
    check(TC.cluster_banks_per_dead()==16,
          "clustered geometry: ceil(16384/1024)=16 target banks/dead bank");
    check(TC.vacuum_rows_per_bank()==1024,
          "clustered target chunk uses vacuum_limit=1024 rows");
    expect_D(TC, A(0,0,0,0,0,5),
             A(0,0,0,2,RPB-1024,5), "clustered dead0 row0 -> live[0] chunk base");
    expect_D(TC, A(0,0,0,0,1023,5),
             A(0,0,0,2,RPB-1,5), "clustered dead0 row1023 -> first chunk end");
    expect_D(TC, A(0,0,0,0,1024,5),
             A(0,0,0,5,RPB-1024,5), "clustered dead0 row1024 -> next exclusive target");
    expect_D(TC, A(0,0,0,0,RPB-1,5),
             A(7,0,0,4,RPB-1,5), "clustered dead0 last row -> balanced live[58]");
    expect_D(TC, A(0,0,0,1,0,5),
             A(0,0,0,3,B_base,5), "clustered D->B fall-through remains active");
    { AddrVec_t v=A(0,0,0,1,0,5); TC.translate(v);
      check(TC.last_layer_d_then_abc() && !TC.last_fast_path(),
            "clustered D->A/B/C is visible and stays on slow path"); }
    bool clustered_injective = true;
    std::set<std::tuple<int,int,int,int,int>> clustered_targets;
    for (int db = 0; db < 2; ++db) {
        for (int r = 0; r < RPB; ++r) {
            AddrVec_t v=A(0,0,0,db,r,0); TC.translate(v);
            auto target=std::make_tuple(v[0],v[1],v[2],v[3],v[4]);
            if (!clustered_targets.insert(target).second) clustered_injective=false;
        }
    }
    check(clustered_injective,
          "clustered Layer-D mapping is injective across both dead banks");
    bool rejected_bad_cluster_geometry = false;
    try {
        HbmRepairTable too_small = t;
        too_small.cfg.vacuum_limit = 128; // 2*128 target banks > 62 live banks
        RepairTranslator bad(too_small, true, 16, 8, true,
                             LayerDMapping::CLUSTERED);
    } catch (const std::invalid_argument&) {
        rejected_bad_cluster_geometry = true;
    }
    check(rejected_bad_cluster_geometry,
          "clustered placement rejects insufficient exclusive target banks");

    std::cout << "== Layer A / B / C on live banks ==\n";
    expect(T, A(1,0,0,3, 512, 5), RepairType::LAYER_A, A(1,0,0,3, A_base, 4), "A transaction hit slot=4");
    expect(T, A(1,0,0,3, 512, 7), RepairType::NONE, A(1,0,0,3, 512, 7), "A clean transaction in same row misses");
    expect(T, A(1,0,0,3, 513, 5), RepairType::NONE,    A(1,0,0,3, 513,      5), "A miss -> NONE");
    expect(T, A(1,0,0,3, 800, 1), RepairType::LAYER_B, A(1,0,0,3, B_base+0, 1), "B offset0");
    expect(T, A(1,0,0,3, 801, 1), RepairType::LAYER_B, A(1,0,0,3, B_base+1, 1), "B offset1");
    expect(T, A(1,0,0,3, 900,  8), RepairType::LAYER_C, A(1,0,0,3, C_base, 0), "C slot0 start");
    expect(T, A(1,0,0,3, 900, 11), RepairType::LAYER_C, A(1,0,0,3, C_base, 3), "C range offset preserved");
    expect(T, A(1,0,0,3, 900,  7), RepairType::NONE,    A(1,0,0,3, 900,   7), "C below range");
    expect(T, A(1,0,0,3, 900, 12), RepairType::LAYER_C, A(1,0,0,3, C_base, 4), "C adjacent range slot4");
    expect(T, A(1,0,0,3, 900, 16), RepairType::NONE,    A(1,0,0,3, 900,  16), "C gap -> NONE");
    expect(T, A(1,0,0,3, 900, 20), RepairType::LAYER_C, A(1,0,0,3, C_base, 8), "C 3rd entry slot8");

    std::cout << "== Layer priority on a live bank (A>B>C) ==\n";
    expect(T, A(3,0,0,1, 60, 4), RepairType::LAYER_A, A(3,0,0,1, A_base, 2), "A beats B on matching transaction");
    expect(T, A(3,0,0,2, 70, 9), RepairType::LAYER_B, A(3,0,0,2, B_base+0, 9), "B beats C");

    std::cout << "== Pass-through (NONE) ==\n";
    expect(T, A(5,0,0,0, 123, 45), RepairType::NONE, A(5,0,0,0, 123, 45), "clean addr unchanged");

    std::cout << "== Structural invariants ==\n";
    int A_lo=A_base, A_hi=A_base+(c.sram_slots+c.bursts_per_row-1)/c.bursts_per_row;
    int B_lo=B_base, B_hi=B_base+c.ded_count;
    int C_lo=C_base, C_hi=C_base+c.frag_count;
    auto disjoint=[](int l1,int h1,int l2,int h2){ return h1<=l2||h2<=l1; };
    check(B_lo>=RPB && C_lo>=RPB && A_lo>=RPB, "spare regions above normal rows");
    check(disjoint(B_lo,B_hi,C_lo,C_hi) && disjoint(B_lo,B_hi,A_lo,A_hi)
          && disjoint(C_lo,C_hi,A_lo,A_hi), "DED/burst/SRAM regions pairwise disjoint");
    // Layer D never relocates onto a dead bank, and lands within the bank's own rows.
    for (int r : {0, 264, 265, 16383}) {
        AddrVec_t v=A(0,0,0,0,r,0); T.translate(v);
        bool tgt_dead = t.bad_bank_set.count({v[AIDX_CH],v[AIDX_PCH],v[AIDX_BG],v[AIDX_BA]})>0;
        check(!tgt_dead, "D target bank is live (row="+std::to_string(r)+")");
        check(v[AIDX_ROW]>=RPB-T.vacuum_rows_per_bank() && v[AIDX_ROW]<RPB,
              "D target row in vacuum band (row="+std::to_string(r)+")");
    }

    std::cout << "== Bloom pre-filter ==\n";
    check(T.bloom_enabled() && T.bloom_num_bits() > 0, "bloom built (enabled, non-empty)");
    // Bloom must NOT change any translation result: compare against a bloom-off twin.
    RepairTranslator T_nb(t, /*enable_bloom=*/false);
    bool equal_all = true;
    auto same = [&](AddrVec_t v){
        AddrVec_t a=v,b=v; RepairType ra=T.translate(a), rb=T_nb.translate(b);
        if (ra!=rb || a!=b) equal_all=false;
    };
    // real A/B/C keys (must be looked up, i.e. bloom "maybe"), dead-bank rows, and clean rows
    for (int col : {5,8,9,11,12,20,31}) {
        same(A(1,0,0,3,512,col)); same(A(1,0,0,3,800,col)); same(A(1,0,0,3,900,col));
        same(A(3,0,0,1,60,col));  same(A(3,0,0,2,70,col));
        same(A(0,0,0,0,col*37,col)); same(A(0,0,0,1,col*11,col));
    }
    for (int r=0; r<4000; r+=7) { same(A(5,0,0,0,r,3)); same(A(2,0,0,1,r,9)); }
    check(equal_all, "bloom on == bloom off for all sampled addresses");
    // Real A/B/C keys must never be screened out (no false negatives).
    { AddrVec_t v=A(1,0,0,3,512,5); T.translate(v); check(!T.last_bloom_reject(), "A key -> bloom maybe (not screened)"); }
    { AddrVec_t v=A(1,0,0,3,800,5); T.translate(v); check(!T.last_bloom_reject(), "B key -> bloom maybe"); }
    { AddrVec_t v=A(1,0,0,3,900,8); T.translate(v); check(!T.last_bloom_reject(), "C key -> bloom maybe"); }
    { AddrVec_t v=A(1,0,0,3,512,5); T.translate(v); check(!T.last_fast_path(), "A key -> slow lookup path"); }
    { AddrVec_t v=A(1,0,0,3,512,5); T.translate(v);
      check(!T.last_source_bank_dead() && T.last_abc_table_lookup(),
            "live A hit reports exact-table activity without Layer D"); }
    { AddrVec_t v=A(6,0,0,2,123,1); T.translate(v);
      check(!T.last_source_bank_dead() && !T.last_abc_table_lookup(),
            "Bloom reject reports no exact-table activity"); }
    { AddrVec_t v=A(0,0,0,0,3,5); T.translate(v);
      check(T.last_source_bank_dead(), "dead source reports Layer-D metadata activity"); }
    // Clean rows should mostly take the fast (reject) path -> that is the power win.
    { size_t fast=0,tot=0; for (int r=0;r<2000;++r){ AddrVec_t v=A(6,0,0,2,r,1); T.translate(v); ++tot; if(T.last_fast_path())++fast; }
      check(fast > tot*9/10, "clean addrs mostly bloom-rejected (fast path >90%)"); }

    // -- The unified front gate ------------------------------------------------
    // There is ONE Bloom probe per request, on the POST-Layer-D key. A dead-bank
    // request whose relocated row has no A/B/C entry gets a Bloom reject and does
    // no repair-table work at all -- Layer D is a bank bitmap plus combinational
    // address math. So it takes the FAST path. The legacy gate charges every
    // dead-bank access the slow latency; keep it as the conservative bracket.
    //
    // Both gates MUST produce identical addresses and identical RepairTypes --
    // only the fast/slow classification differs. That is the safety property.
    {
        RepairTranslator T_uni(t, true, 12, 8, /*unified_gate=*/true);
        RepairTranslator T_leg(t, true, 12, 8, /*unified_gate=*/false);
        check(T_uni.unified_gate() && !T_leg.unified_gate(), "gate flag plumbed through");

        bool same_result = true, uni_fast_seen = false, leg_fast_on_dead = false;
        for (int r = 0; r < 4000; r += 3) {
            for (int col : {1, 5, 17}) {
                AddrVec_t vu = A(0,0,0,0,r,col);   // ch0/pch0/bg0/ba0 is a DEAD bank
                AddrVec_t vl = vu;
                RepairType ru = T_uni.translate(vu);
                RepairType rl = T_leg.translate(vl);
                if (ru != rl || vu != vl) same_result = false;   // addresses must match
                if (T_uni.last_fast_path()) uni_fast_seen = true;
                if (T_leg.last_fast_path()) leg_fast_on_dead = true;
            }
        }
        check(same_result,
              "unified and legacy gates translate dead-bank addresses IDENTICALLY");
        check(uni_fast_seen,
              "unified gate: a dead-bank Bloom reject takes the FAST path");
        check(!leg_fast_on_dead,
              "legacy gate: every dead-bank access stays on the SLOW path");

        // A dead bank whose relocated row DOES hit an A/B/C key must stay slow in
        // both gates -- the Bloom cannot reject it, so real table work happens.
        size_t uni_slow = 0;
        for (int r = 0; r < 16384; ++r) {
            AddrVec_t v = A(0,0,0,0,r,5);
            T_uni.translate(v);
            if (!T_uni.last_fast_path()) ++uni_slow;
        }
        check(uni_slow > 0,
              "unified gate: dead-bank rows landing on A/B/C keys still go slow");
    }

    // -- Layer D -> A/B/C fall-through must be VISIBLE and charged SLOW --------
    // The vacuum band is made of ordinary DRAM rows in live banks, so a relocated
    // access can land on a row that is ITSELF faulty and needs A/B/C. translate()
    // reports LAYER_D for those (D is the outermost layer), which would hide them
    // in the per-layer counters -- hence last_layer_d_then_abc().
    //
    // This is NOT hypothetical: on the real worst die 193, 58 of its 1,990 A/B/C
    // keys sit inside the vacuum band, and all 844 dead-bank dies have at least one.
    {
        RepairTranslator T2(t);
        // dead1 rows 0/1/2 relocate onto rows that carry a B / C / A entry.
        struct { AddrVec_t v; const char* what; } fall[] = {
            { A(0,0,0,1, 0, 5), "D->B" },
            { A(0,0,0,1, 1, 9), "D->C" },
            { A(0,0,0,1, 2, 7), "D->A" },
        };
        bool all_flagged = true, all_slow = true;
        for (auto& f : fall) {
            AddrVec_t v = f.v;
            RepairType r = T2.translate(v);
            if (r != RepairType::LAYER_D)      all_flagged = false;
            if (!T2.last_layer_d_then_abc())   all_flagged = false;
            if (T2.last_fast_path())           all_slow = false;   // must be SLOW
        }
        check(all_flagged, "D->A/B/C fall-through is reported by last_layer_d_then_abc()");
        check(all_slow,    "D->A/B/C fall-through is charged the SLOW path");

        // A plain relocation (clean target row) must NOT be flagged, and under the
        // unified gate it is allowed to take the fast path.
        AddrVec_t v = A(0,0,0,1, 3, 5);        // relocates to a clean row
        RepairType r = T2.translate(v);
        check(r == RepairType::LAYER_D && !T2.last_layer_d_then_abc(),
              "plain Layer D (clean target row) is NOT flagged as D->A/B/C");
    }

    std::cout << "\n================ RESULT ================\n";
    std::cout << "  passed: " << g_pass << "   failed: " << g_fail << "\n";
    std::cout << "========================================\n";
    return g_fail == 0 ? 0 : 1;
}
