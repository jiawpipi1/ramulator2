#pragma once
// repair_translator.h
// Address translation through the 4-layer HBM repair hierarchy.
//
// Layer priority / flow for one request:
//   Front gate          : check the tiny dead-bank membership structure and the
//                         A/B/C Bloom filter together. A live-bank Bloom reject
//                         is the common fast path and returns immediately.
//   Layer D (dead bank) : RELOCATE the access to a live bank's reserved top-row
//                         band, then re-check Bloom and FALL THROUGH to A/B/C on
//                         the relocated address (the target row may be faulty).
//   Layer A (SRAM)      : overflow row -> SRAM replacement row.
//   Layer B (DED)       : faulty row   -> dedicated whole-row spare.
//   Layer C (burst)     : faulty col range -> burst spare slot.
// A/B/C are mutually exclusive for a given (bank,row). Layer D cannot re-fire
// after relocation because the target is a live bank (never in bad_bank_set).
//
// Layer D interleave (combinational, no per-row table):
//   Let R  = rows_per_bank, F = #dead banks, L = #live banks (all banks - dead).
//       h  = ceil(R / L)                    (band height: rows one dead bank
//                                            places in each live bank)
//   For a dead bank with ordinal i (its rank among dead banks in canonical
//   (ch, ly=bg*num_pch+pch, ba) order) and incoming row r:
//       j    = r / h            -> index into the ordered live-bank list
//       o    = r % h            -> offset within the band
//       tgt  = live_banks[j]
//       trow = R - (i+1)*h + o  -> dead bank i occupies band [R-(i+1)h, R-i*h)
//   So dead bank ordinal 0 takes the topmost band of every live bank, ordinal 1
//   the next band down, etc. The union of bands is the per-live-bank vacuum
//   region [R - F*h, R) that the OS is told not to use.
//
// When Ramulator::g_debug_address is true (--debug-address) translate() prints
// a per-request trace of every layer lookup.
//
// addr_vec layout (HBM3, ChRaBaRoCo): [0]=ch [1]=pch [2]=bg [3]=ba [4]=row [5]=col

#include <iomanip>
#include <iostream>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include "dram_controller/impl/repair/repair_table.h"
#include "dram_controller/impl/repair/repair_debug.h"
#include "base/base.h"   // AddrVec_t, Addr_t

namespace Ramulator {

// -- Blocked Bloom filter ------------------------------------------------
// Screens the fine-grained (bank,row) A/B/C repair keys. Remapping is sparse
// (almost every request is a pass-through), so a request whose (bank,row) is
// "definitely not" in the repair set can skip all the A/B/C table lookups --
// saving lookup energy. No false negatives: a real A/B/C key always tests
// "maybe present", so translation stays exact. False positives just fall back
// to the full (correct) lookup. All k probes for one key occupy one 64-bit word,
// so hardware needs one small-SRAM read rather than k random reads/ports.
class BloomFilter {
public:
    // A "block" is the unit the hardware reads in ONE SRAM access. All k probes
    // for a key live inside one block, so a lookup is a single read -- never k
    // random reads and never a k-ported memory. That is the whole point of a
    // blocked filter.
    //
    // BLOCK SIZE IS 512 BITS (64 B), NOT 64. This was measured, not guessed. A
    // 64-bit block made the filter 7x worse than the textbook formula predicts:
    //
    //   block   bits/key  k   size     measured FP   (worst die 193, 1,972 keys)
    //     64       12     8   2,960 B     2.178%     <- the old configuration
    //    512       12     8   3,008 B     0.517%
    //    512       16     8   4,032 B     0.1747%    <- current runtime
    //   (textbook non-blocked, m/n=12, k=8:  0.31%)
    //
    // WHY: keys land in blocks by a hash, so occupancy is Poisson-spread. The
    // false-positive rate of a block is convex in its occupancy, so the few
    // overloaded blocks dominate the average -- E[FP] >> FP(E[occupancy]). With
    // 512-bit blocks the relative spread is far smaller and the filter behaves
    // close to theory. A 512-bit block is still ONE SRAM row read (contiguous
    // words), so the fast path costs the same access it always did.
    //
    // This matters more than it looks: with the unified front gate, a Bloom
    // false positive is now essentially the ONLY thing left on the slow path.
    static constexpr int WORDS_PER_BLOCK = 8;                    // 8 x 64 bits
    static constexpr int BLOCK_BITS      = WORDS_PER_BLOCK * 64; // = 512

    void build(size_t n_keys, int bits_per_key, int k) {
        m_k = (k > 0) ? k : 1;
        size_t nbits = (n_keys ? n_keys : 1) * (size_t)(bits_per_key > 0 ? bits_per_key : 16);
        if (nbits < 1024) nbits = 1024;
        m_nblocks = (nbits + BLOCK_BITS - 1) / BLOCK_BITS;
        if (m_nblocks == 0) m_nblocks = 1;
        m_words.assign(m_nblocks * WORDS_PER_BLOCK, 0);
        m_nbits = m_words.size() * 64;
    }
    bool built() const { return m_nbits > 0; }
    void insert(uint64_t key) {
        size_t base;
        uint64_t masks[WORDS_PER_BLOCK];
        location(key, base, masks);
        for (int w = 0; w < WORDS_PER_BLOCK; ++w) m_words[base + w] |= masks[w];
    }
    // true  => key MIGHT be present (do the full lookup)
    // false => key is DEFINITELY absent (safe to skip A/B/C)
    bool maybe_contains(uint64_t key) const {
        if (!built()) return true;               // disabled -> never screens
        size_t base;
        uint64_t masks[WORDS_PER_BLOCK];
        location(key, base, masks);
        for (int w = 0; w < WORDS_PER_BLOCK; ++w)
            if ((m_words[base + w] & masks[w]) != masks[w]) return false;
        return true;
    }
    size_t num_bits()   const { return m_nbits; }
    int    num_hashes() const { return m_k; }
    size_t num_blocks() const { return m_nblocks; }
private:
    static uint64_t mix(uint64_t x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x;
    }
    // One hash selects the block; two more spread k bits inside it.
    void location(uint64_t key, size_t& base, uint64_t (&masks)[WORDS_PER_BLOCK]) const {
        base = (mix(key ^ 0x243f6a8885a308d3ULL) % m_nblocks) * WORDS_PER_BLOCK;
        uint64_t first = mix(key ^ 0x9e3779b97f4a7c15ULL);
        uint64_t step  = mix(key ^ 0xc2b2ae3d27d4eb4fULL) | 1ULL;
        for (int w = 0; w < WORDS_PER_BLOCK; ++w) masks[w] = 0;
        for (int i = 0; i < m_k; ++i) {
            uint64_t bit = (first + (uint64_t)i * step) & (uint64_t)(BLOCK_BITS - 1);
            masks[bit >> 6] |= 1ULL << (bit & 63ULL);
        }
    }
    std::vector<uint64_t> m_words;
    size_t m_nbits   = 0;
    size_t m_nblocks = 0;
    int    m_k = 0;
};

// Pack (ch,pch,bg,ba,row) into one 64-bit Bloom key.
inline uint64_t bank_row_key(int ch, int pch, int bg, int ba, int row) {
    return ((uint64_t)(uint32_t)ch  << 44) | ((uint64_t)(uint32_t)pch << 42)
         | ((uint64_t)(uint32_t)bg  << 38) | ((uint64_t)(uint32_t)ba  << 32)
         | (uint64_t)(uint32_t)row;
}

// HBM3 addr_vec index  (ChRaBaRoCo mapper output)
static constexpr int AIDX_CH  = 0;
static constexpr int AIDX_PCH = 1;
static constexpr int AIDX_BG  = 2;
static constexpr int AIDX_BA  = 3;
static constexpr int AIDX_ROW = 4;
static constexpr int AIDX_COL = 5;

enum class RepairType { NONE, LAYER_A, LAYER_B, LAYER_C, LAYER_D };

inline const char* repair_type_name(RepairType t) {
    switch (t) {
        case RepairType::NONE:    return "NONE (pass-through)";
        case RepairType::LAYER_A: return "LAYER_A (SRAM overflow row)";
        case RepairType::LAYER_B: return "LAYER_B (DED whole-row spare)";
        case RepairType::LAYER_C: return "LAYER_C (burst col-range SRAM)";
        case RepairType::LAYER_D: return "LAYER_D (dead-bank relocate + A/B/C)";
    }
    return "UNKNOWN";
}

class RepairTranslator {
public:
    // Conservative repair-entry sizing with 512-bit blocks measures 0.1747% FP
    // on worst die 193 (4,032 B, 300k live-bank negative probes). See
    // BloomFilter for why the block size, not the bit budget, was
    // the thing that mattered.
    explicit RepairTranslator(const HbmRepairTable& tbl, bool enable_bloom = true,
                              int bloom_bits_per_key = 16, int bloom_k = 8,
                              bool unified_gate = true)
        : m_tbl(tbl), m_bloom_enabled(enable_bloom), m_unified_gate(unified_gate) {
        build_bank_lists();
        if (m_bloom_enabled) build_bloom(bloom_bits_per_key, bloom_k);
    }

    // Number of live/dead banks and the band height (exposed for tests/introspection).
    int num_live()  const { return m_num_live; }
    int num_dead()  const { return m_num_dead; }
    int band_h()    const { return m_band_h; }
    // Rows reserved per live bank as the Layer D vacuum region.
    int vacuum_rows_per_bank() const { return m_num_dead * m_band_h; }

    // Bloom introspection / stats (cumulative across translate() calls).
    bool   bloom_enabled()   const { return m_bloom_enabled; }
    bool   unified_gate()    const { return m_unified_gate; }
    size_t bloom_num_bits()  const { return m_bloom.num_bits(); }
    int    bloom_num_hashes()const { return m_bloom.num_hashes(); }
    size_t bloom_rejects()   const { return m_bloom_reject; }   // fast path taken
    size_t bloom_maybes()    const { return m_bloom_maybe;  }   // full A/B/C lookup done
    bool   last_bloom_reject() const { return m_last_reject; }  // for the most recent translate()
    bool   last_fast_path() const { return m_last_fast; }       // live bank + Bloom reject
    bool   last_uses_layer_a_sram() const { return m_last_layer_a; }
    // Array-activity flags for the most recent translate().  These describe
    // logical metadata accesses, independent of the configured latency model.
    // The memory-system counts them only after the request is accepted, so a
    // backpressured retry cannot inflate workload energy.
    bool   last_source_bank_dead() const { return m_last_source_dead; }
    bool   last_abc_table_lookup() const { return m_last_abc_lookup; }
    // True when the request was relocated by Layer D AND the relocated row then
    // ALSO needed an A/B/C repair. The vacuum band is made of ordinary DRAM rows
    // in live banks, so a relocated access can land on a row that is itself
    // faulty. That request does real table work and is charged the slow path.
    // translate() returns LAYER_D for it (D is the outermost layer), so without
    // this flag the per-layer counters would hide the case entirely.
    bool   last_layer_d_then_abc() const { return m_last_d_abc; }

    RepairType translate(AddrVec_t& av, Addr_t raw_addr = 0) const {
        m_last_layer_a = false;
        m_last_d_abc = false;
        m_last_source_dead = false;
        m_last_abc_lookup = false;
        int ch  = av[AIDX_CH];
        int pch = av[AIDX_PCH];
        int bg  = av[AIDX_BG];
        int ba  = av[AIDX_BA];
        int row = av[AIDX_ROW];
        int col = av[AIDX_COL];

        const bool dbg = g_debug_address.load(std::memory_order_relaxed);
        if (dbg) {
            std::cerr
                << "\n[ADDR-DBG] ==========================================\n"
                << "  raw addr : 0x" << std::hex << std::setw(12)
                << std::setfill('0') << raw_addr
                << std::dec << std::setfill(' ') << "  (" << raw_addr << ")\n"
                << "  decomposed: ch=" << ch << " pch=" << pch << " bg=" << bg
                << " ba=" << ba << " row=" << row << " col=" << col << "\n";
        }

        m_last_reject = false;
        m_last_fast = false;
        const BankKey4 source_bank{ch, pch, bg, ba};
        const bool source_dead = m_tbl.bad_bank_set.count(source_bank) != 0;
        m_last_source_dead = source_dead;

        // -- Front gate: exact dead-bank membership ---------------------------
        // source_dead is a 512-bit bank bitmap (64 B for HBM3): one bit indexed
        // by the 9-bit bank key. Exact, no false positives. Layer D is NOT in
        // the Bloom filter and must not be: a dead bank kills ALL its rows, so
        // representing it as (bank,row) keys would mean inserting 16384 keys per
        // dead bank (245,760 for worst die 193) and would swamp a filter sized
        // for 1,990 A/B/C keys. The bitmap is smaller, exact, and can be read in
        // parallel with the Bloom.
        //
        // Layer D relocation therefore runs BEFORE the Bloom probe, because D
        // changes the (bank,row) that A/B/C are keyed on. There is exactly ONE
        // probe per request, on the post-D key.

        RepairType d_result = RepairType::NONE;

        // -- Layer D: dead bank -> relocate to a live bank's reserved band -----
        if (source_dead) {
            auto it = m_dead_ordinal.find(source_bank);
            bool ok = (it != m_dead_ordinal.end()) && m_num_live > 0 && m_band_h > 0;
            int j = ok ? (row / m_band_h) : -1;
            if (ok && j < m_num_live) {
                int i = it->second;
                int o = row % m_band_h;
                const auto& tgt = m_live_banks[j];
                int tch  = std::get<0>(tgt);
                int tpch = std::get<1>(tgt);
                int tbg  = std::get<2>(tgt);
                int tba  = std::get<3>(tgt);
                int trow = m_R - (i + 1) * m_band_h + o;
                if (dbg) {
                    std::cerr << "  [Layer D] dead ord=" << i << " row=" << row
                        << " -> live[" << j << "]=(" << tch << "," << tpch << ","
                        << tbg << "," << tba << ") row=" << trow
                        << "  (h=" << m_band_h << " F=" << m_num_dead
                        << " L=" << m_num_live << ")  -> fall through to A/B/C\n";
                }
                av[AIDX_CH] = ch = tch;
                av[AIDX_PCH] = pch = tpch;
                av[AIDX_BG] = bg = tbg;
                av[AIDX_BA] = ba = tba;
                av[AIDX_ROW] = row = trow;
                d_result = RepairType::LAYER_D;
            } else if (dbg) {
                std::cerr << "  [Layer D] dead bank but relocation unavailable "
                    << "(ord_found=" << (it != m_dead_ordinal.end())
                    << " L=" << m_num_live << " j=" << j << ") -> pass-through\n";
            }
        }

        // -- The single Bloom probe, on the POST-D key ------------------------
        // ch/pch/bg/ba/row are the relocated values if Layer D fired, else the
        // originals (for a live bank the post-D key IS the original key). A
        // reject proves no A/B/C entry can apply, so the request is done.
        //
        // FAST-PATH CLASSIFICATION. m_unified_gate = true means a Bloom reject
        // takes the fast path EVEN IF Layer D relocated it. That is the point of
        // this structure: Layer D is combinational (a bitmap bit, an ordinal, a
        // live-bank select and a constant-divisor divide) and touches no repair
        // table, so a relocated request whose target row has no A/B/C entry does
        // no more table work than a plain pass-through. Previously every
        // dead-bank access was charged the slow latency, even though ~2.93% of
        // all banks are dead -- that alone put most of the slow traffic on the
        // slow path for no lookup reason.
        //
        // The hardware assumption this buys: to keep the fast path at one cycle,
        // the Bloom must be probed with the post-D key without serialising
        // bitmap -> relocate -> Bloom. Probe BOTH the original and the relocated
        // key in parallel and let the dead bit mux the result. The non-RTL power
        // model therefore reports two Bloom copies for F1; one copy is only the
        // serialized area lower bound.
        // Set m_unified_gate = false to charge dead banks the slow latency and
        // measure the conservative bracket.
        if (m_bloom_enabled) {
            if (!m_bloom.maybe_contains(bank_row_key(ch, pch, bg, ba, row))) {
                ++m_bloom_reject;
                m_last_reject = true;
                m_last_fast = m_unified_gate || !source_dead;
                if (dbg) std::cerr << "  [Bloom] reject -> skip A/B/C  RESULT: "
                                   << repair_type_name(d_result)
                                   << (m_last_fast ? "  (fast path)" : "  (slow path)")
                                   << "\n";
                return d_result;                 // LAYER_D if relocated, else NONE
            }
            ++m_bloom_maybe;                      // fall through to full lookup
        }

        // A Bloom maybe (or Bloom disabled) performs the packed A/B/C metadata
        // lookup.  This is one logical table access even if all three maps miss.
        m_last_abc_lookup = true;

        // -- Layer A: SRAM overflow row ---------------------------------------
        {
            auto it = m_tbl.sram_full_map.find(std::make_tuple(ch, pch, bg, ba, row));
            if (it != m_tbl.sram_full_map.end()) {
                int new_row = m_tbl.cfg.rows_per_bank
                            + m_tbl.cfg.total_spare_rows + it->second;
                if (dbg) std::cerr << "  [Layer A] slot " << it->second
                                   << " -> row " << new_row << "\n";
                av[AIDX_ROW] = new_row;
                m_last_layer_a = true;
                m_last_d_abc = (d_result == RepairType::LAYER_D);
                return d_result == RepairType::LAYER_D ? RepairType::LAYER_D
                                                       : RepairType::LAYER_A;
            }
        }

        // -- Layer B: DED whole-row spare -------------------------------------
        {
            auto it = m_tbl.ded_row_map.find(std::make_tuple(ch, pch, bg, ba, row));
            if (it != m_tbl.ded_row_map.end()) {
                int new_row = m_tbl.ded_spare_row_addr(it->second);
                if (dbg) std::cerr << "  [Layer B] offset " << it->second
                                   << " -> row " << new_row << "\n";
                av[AIDX_ROW] = new_row;
                m_last_d_abc = (d_result == RepairType::LAYER_D);
                return d_result == RepairType::LAYER_D ? RepairType::LAYER_D
                                                       : RepairType::LAYER_B;
            }
        }

        // -- Layer C: burst col-range remap -----------------------------------
        {
            auto it = m_tbl.burst_map.lower_bound(
                std::make_tuple(ch, pch, bg, ba, row, 0));
            while (it != m_tbl.burst_map.end()) {
                auto& [key, be] = *it;
                auto& [kch, kpch, kbg, kba, krow, kcol] = key;
                if (kch != ch || kpch != pch || kbg != bg ||
                    kba != ba  || krow != row) break;
                if (col >= be.col_start && col < be.col_start + be.length) {
                    int target = be.target_slot + (col - be.col_start);
                    auto [new_row, new_col] = m_tbl.burst_slot_to_addr(target);
                    if (dbg) std::cerr << "  [Layer C] slot " << be.target_slot
                                       << " -> row " << new_row << " col " << new_col << "\n";
                    av[AIDX_ROW] = new_row;
                    av[AIDX_COL] = new_col;
                    m_last_d_abc = (d_result == RepairType::LAYER_D);
                    return d_result == RepairType::LAYER_D ? RepairType::LAYER_D
                                                           : RepairType::LAYER_C;
                }
                ++it;
            }
        }

        if (dbg) std::cerr << "  RESULT: " << repair_type_name(d_result) << "\n"
                           << "[ADDR-DBG] ==========================================\n";
        return d_result;   // LAYER_D if it relocated (A/B/C all missed), else NONE
    }

private:
    using BankKey4 = std::tuple<int,int,int,int>;   // (ch, pch, bg, ba)

    // Enumerate all banks in canonical (ch, ly=bg*num_pch+pch, ba) order,
    // splitting into the ordered live-bank list and dead-bank ordinals. This
    // ordering matches the offline tool's bad-bank emission order.
    void build_bank_lists() {
        const RepairConfig& c = m_tbl.cfg;
        m_R = c.rows_per_bank;
        int ord = 0;
        const int num_ly = c.num_pch * c.num_bg;
        for (int ch = 0; ch < c.num_channels; ++ch) {
            for (int ly = 0; ly < num_ly; ++ly) {
                int pch = ly % c.num_pch;
                int bg  = ly / c.num_pch;
                for (int ba = 0; ba < c.num_ba; ++ba) {
                    BankKey4 k{ch, pch, bg, ba};
                    if (m_tbl.bad_bank_set.count(k)) m_dead_ordinal[k] = ord++;
                    else                             m_live_banks.push_back(k);
                }
            }
        }
        m_num_dead = ord;
        m_num_live = static_cast<int>(m_live_banks.size());
        m_band_h   = (m_num_live > 0) ? (m_R + m_num_live - 1) / m_num_live : 0;
    }

    // Insert every fine-grained A/B/C (bank,row) repair key. Layer D is NOT
    // inserted -- it is a coarse per-bank check against bad_bank_set.
    void build_bloom(int bits_per_key, int k) {
        // Capacity is conservatively based on repair entries. Several Layer-C
        // segments can share one tested (bank,row) key, so this can allocate a
        // few more bits than unique-key sizing; the CACTI capacity model uses
        // the same rule. Preserve it because the published Claim-2 matrix used
        // this organization.
        size_t n = m_tbl.sram_full_map.size() + m_tbl.ded_row_map.size()
                 + m_tbl.burst_map.size();
        m_bloom.build(n, bits_per_key, k);
        for (auto& [key, v] : m_tbl.sram_full_map) {
            auto& [ch, pch, bg, ba, row] = key;
            m_bloom.insert(bank_row_key(ch, pch, bg, ba, row));
        }
        for (auto& [key, v] : m_tbl.ded_row_map) {
            auto& [ch, pch, bg, ba, row] = key;
            m_bloom.insert(bank_row_key(ch, pch, bg, ba, row));
        }
        for (auto& [key, be] : m_tbl.burst_map) {
            auto& [ch, pch, bg, ba, row, cs] = key;
            m_bloom.insert(bank_row_key(ch, pch, bg, ba, row));
        }
    }

    const HbmRepairTable&  m_tbl;
    std::vector<BankKey4>  m_live_banks;
    std::map<BankKey4,int> m_dead_ordinal;
    int m_R = 0, m_num_live = 0, m_num_dead = 0, m_band_h = 0;

    bool           m_bloom_enabled = false;
    // true  = a Bloom reject is fast even when Layer D relocated the request
    // false = legacy: every dead-bank access pays the slow latency
    bool           m_unified_gate  = true;
    BloomFilter    m_bloom;
    mutable size_t m_bloom_reject = 0;
    mutable size_t m_bloom_maybe  = 0;
    mutable bool   m_last_reject  = false;
    mutable bool   m_last_fast    = false;
    mutable bool   m_last_layer_a = false;
    mutable bool   m_last_d_abc   = false;
    mutable bool   m_last_source_dead = false;
    mutable bool   m_last_abc_lookup = false;
};

} // namespace Ramulator
