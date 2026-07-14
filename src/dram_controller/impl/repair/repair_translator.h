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
    void build(size_t n_keys, int bits_per_key, int k) {
        m_k = (k > 0) ? k : 1;
        size_t nbits = (n_keys ? n_keys : 1) * (size_t)(bits_per_key > 0 ? bits_per_key : 12);
        if (nbits < 1024) nbits = 1024;
        m_words.assign((nbits + 63) / 64, 0);
        m_nbits = m_words.size() * 64;
    }
    bool built() const { return m_nbits > 0; }
    void insert(uint64_t key) {
        size_t word;
        uint64_t mask;
        location(key, word, mask);
        m_words[word] |= mask;
    }
    // true  => key MIGHT be present (do the full lookup)
    // false => key is DEFINITELY absent (safe to skip A/B/C)
    bool maybe_contains(uint64_t key) const {
        if (!built()) return true;               // disabled -> never screens
        size_t word;
        uint64_t mask;
        location(key, word, mask);
        return (m_words[word] & mask) == mask;
    }
    size_t num_bits() const { return m_nbits; }
    int    num_hashes() const { return m_k; }
private:
    static uint64_t mix(uint64_t x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x;
    }
    void location(uint64_t key, size_t& word, uint64_t& mask) const {
        word = mix(key ^ 0x243f6a8885a308d3ULL) % m_words.size();
        uint64_t first = mix(key ^ 0x9e3779b97f4a7c15ULL);
        uint64_t step = mix(key ^ 0xc2b2ae3d27d4eb4fULL) | 1ULL;
        mask = 0;
        for (int i = 0; i < m_k; ++i)
            mask |= 1ULL << ((first + (uint64_t)i * step) & 63ULL);
    }
    std::vector<uint64_t> m_words;
    size_t m_nbits = 0;
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
    explicit RepairTranslator(const HbmRepairTable& tbl, bool enable_bloom = true,
                              int bloom_bits_per_key = 12, int bloom_k = 8)
        : m_tbl(tbl), m_bloom_enabled(enable_bloom) {
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
    size_t bloom_num_bits()  const { return m_bloom.num_bits(); }
    int    bloom_num_hashes()const { return m_bloom.num_hashes(); }
    size_t bloom_rejects()   const { return m_bloom_reject; }   // fast path taken
    size_t bloom_maybes()    const { return m_bloom_maybe;  }   // full A/B/C lookup done
    bool   last_bloom_reject() const { return m_last_reject; }  // for the most recent translate()
    bool   last_fast_path() const { return m_last_fast; }       // live bank + Bloom reject
    bool   last_uses_layer_a_sram() const { return m_last_layer_a; }

    RepairType translate(AddrVec_t& av, Addr_t raw_addr = 0) const {
        m_last_layer_a = false;
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

        // -- Common front gate ------------------------------------------------
        // In hardware, source_dead is a tiny bank bitmap/CAM check that can run
        // beside the Bloom probes. A live-bank Bloom reject proves that neither
        // D nor A/B/C can apply, so the request returns without exercising the
        // relocation or full tables. A dead-bank request cannot use this reject:
        // D changes its bank/row, and the translated key must be checked again.
        if (m_bloom_enabled && !source_dead) {
            if (!m_bloom.maybe_contains(bank_row_key(ch, pch, bg, ba, row))) {
                ++m_bloom_reject;
                m_last_reject = true;
                m_last_fast = true;
                if (dbg) std::cerr << "  [Front gate] live bank + Bloom reject"
                                   << " -> fast pass-through\n";
                return RepairType::NONE;
            }
            ++m_bloom_maybe;
        }

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

        // A live-bank Bloom maybe was already counted by the front gate. For a
        // dead source bank, query the relocated key now; querying only the
        // original key would introduce false negatives after Layer D.
        if (m_bloom_enabled && source_dead) {
            if (!m_bloom.maybe_contains(bank_row_key(ch, pch, bg, ba, row))) {
                ++m_bloom_reject;
                m_last_reject = true;
                if (dbg) std::cerr << "  [Bloom] reject -> skip A/B/C  RESULT: "
                                   << repair_type_name(d_result) << "\n";
                return d_result;                 // LAYER_D if relocated, else NONE
            }
            ++m_bloom_maybe;                      // fall through to full lookup
        }

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
    BloomFilter    m_bloom;
    mutable size_t m_bloom_reject = 0;
    mutable size_t m_bloom_maybe  = 0;
    mutable bool   m_last_reject  = false;
    mutable bool   m_last_fast    = false;
    mutable bool   m_last_layer_a = false;
};

} // namespace Ramulator
