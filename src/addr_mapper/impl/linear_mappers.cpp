#include <vector>

#include "base/base.h"
#include "dram/dram.h"
#include "addr_mapper/addr_mapper.h"
#include "memory_system/memory_system.h"

namespace Ramulator {

class LinearMapperBase : public IAddrMapper {
  public:
    IDRAM* m_dram = nullptr;

    int m_num_levels = -1;          // How many levels in the hierarchy?
    std::vector<int> m_addr_bits;   // How many address bits for each level in the hierarchy?
    Addr_t m_tx_offset = -1;

    int m_col_bits_idx = -1;
    int m_row_bits_idx = -1;


  protected:
    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) {
      m_dram = memory_system->get_ifce<IDRAM>();

      // Populate m_addr_bits vector with the number of address bits for each level in the hierachy
      const auto& count = m_dram->m_organization.count;
      m_num_levels = count.size();
      m_addr_bits.resize(m_num_levels);
      for (size_t level = 0; level < m_addr_bits.size(); level++) {
        if (count[level] <= 0 || (count[level] & (count[level] - 1)) != 0) {
          throw ConfigurationError(
              "Linear address mappers require power-of-two organization counts; "
              "level {} has count {}!", level, count[level]);
        }
        m_addr_bits[level] = calc_log2(count[level]);
      }

      // Last (Column) address have the granularity of the prefetch size
      m_addr_bits[m_num_levels - 1] -= calc_log2(m_dram->m_internal_prefetch_size);

      int tx_bytes = m_dram->m_internal_prefetch_size * m_dram->m_channel_width / 8;
      if (tx_bytes <= 0 || (tx_bytes & (tx_bytes - 1)) != 0) {
        throw ConfigurationError(
            "Linear address mappers require a power-of-two transaction size; got {} bytes!",
            tx_bytes);
      }
      m_tx_offset = calc_log2(tx_bytes);

      // Determine where are the row and col bits for ChRaBaRoCo and RoBaRaCoCh
      try {
        m_row_bits_idx = m_dram->m_levels("row");
      } catch (const std::out_of_range& r) {
        throw std::runtime_error(fmt::format("Organization \"row\" not found in the spec, cannot use linear mapping!"));
      }

      // Assume column is always the last level
      m_col_bits_idx = m_num_levels - 1;
    }

};


class ChRaBaRoCo final : public LinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IAddrMapper, ChRaBaRoCo, "ChRaBaRoCo", "Applies a trival mapping to the address.");

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      LinearMapperBase::setup(frontend, memory_system);
    }

    void apply(Request& req) override {
      req.addr_vec.resize(m_num_levels, -1);
      Addr_t addr = req.addr >> m_tx_offset;
      for (int i = m_addr_bits.size() - 1; i >= 0; i--) {
        req.addr_vec[i] = slice_lower_bits(addr, m_addr_bits[i]);
      }
    }
};


class RoBaRaCoCh final : public LinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IAddrMapper, RoBaRaCoCh, "RoBaRaCoCh", "Applies a RoBaRaCoCh mapping to the address.");

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      LinearMapperBase::setup(frontend, memory_system);
    }

    void apply(Request& req) override {
      req.addr_vec.resize(m_num_levels, -1);
      Addr_t addr = req.addr >> m_tx_offset;
      req.addr_vec[0] = slice_lower_bits(addr, m_addr_bits[0]);
      req.addr_vec[m_addr_bits.size() - 1] = slice_lower_bits(addr, m_addr_bits[m_addr_bits.size() - 1]);
      for (int i = 1; i <= m_row_bits_idx; i++) {
        req.addr_vec[i] = slice_lower_bits(addr, m_addr_bits[i]);
      }
    }
};


class MOP4CLXOR final : public LinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IAddrMapper, MOP4CLXOR, "MOP4CLXOR", "Applies a MOP4CLXOR mapping to the address.");

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      LinearMapperBase::setup(frontend, memory_system);
    }

    void apply(Request& req) override {
      req.addr_vec.resize(m_num_levels, -1);
      Addr_t addr = req.addr >> m_tx_offset;
      req.addr_vec[m_col_bits_idx] = slice_lower_bits(addr, 2);
      for (int lvl = 0 ; lvl < m_row_bits_idx ; lvl++)
          req.addr_vec[lvl] = slice_lower_bits(addr, m_addr_bits[lvl]);
      req.addr_vec[m_col_bits_idx] += slice_lower_bits(addr, m_addr_bits[m_col_bits_idx]-2) << 2;
      req.addr_vec[m_row_bits_idx] = (int) addr;

      int row_xor_index = 0; 
      for (int lvl = 0 ; lvl < m_col_bits_idx ; lvl++){
        if (m_addr_bits[lvl] > 0){
          int mask = (req.addr_vec[m_col_bits_idx] >> row_xor_index) & ((1<<m_addr_bits[lvl])-1);
          req.addr_vec[lvl] = req.addr_vec[lvl] xor mask;
          row_xor_index += m_addr_bits[lvl];
        }
      }
    }
};

class LineRoBaRaCoCh final : public LinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IAddrMapper, LineRoBaRaCoCh, "LineRoBaRaCoCh",
      "RoBaRaCoCh with the channel interleaved at CACHE-LINE granularity.");

  // Plain RoBaRaCoCh puts the channel bits immediately above the transaction
  // offset, so the channel changes every transaction (e.g. every 32 B on HBM3).
  // A 64 B cache line then STRADDLES TWO CHANNELS, and its two halves can never
  // be a row-buffer hit for each other. Real memory controllers interleave
  // channels at cache-line granularity so a whole line stays in one channel:
  //
  //   plain RoBaRaCoCh : [row|ba|ra|col_hi........|ch|tx_offset]
  //   LineRoBaRaCoCh   : [row|ba|ra|col_hi|ch|col_lo|tx_offset]
  //
  // where col_lo = log2(line_size / tx_size) column bits (the transactions
  // WITHIN one line). Both halves of a line share (ch, bank, row) and differ
  // only in the column LSB -> the second half is a guaranteed row hit.
  //
  // line_size == tx_size degenerates to plain RoBaRaCoCh (col_lo = 0 bits).

  private:
    int m_line_size = 64;   // cache-line size in bytes
    int m_line_bits = 0;    // log2(line_size / transaction_size)

  public:
    void init() override {
      m_line_size = param<int>("line_size").default_val(64)
          .desc("Cache-line size in bytes; the channel interleaving granularity.");
    };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      LinearMapperBase::setup(frontend, memory_system);

      int tx_bytes = 1 << m_tx_offset;
      if (m_line_size < tx_bytes || m_line_size % tx_bytes != 0) {
        throw ConfigurationError(
            "LineRoBaRaCoCh: line_size ({}) must be a multiple of the "
            "transaction size ({})!", m_line_size, tx_bytes);
      }
      if ((m_line_size & (m_line_size - 1)) != 0) {
        throw ConfigurationError(
            "LineRoBaRaCoCh: line_size ({}) must be a power of two!", m_line_size);
      }
      m_line_bits = calc_log2(m_line_size / tx_bytes);
      if (m_line_bits > m_addr_bits[m_col_bits_idx]) {
        throw ConfigurationError(
            "LineRoBaRaCoCh: line_size ({}) exceeds a whole row!", m_line_size);
      }
    }

    void apply(Request& req) override {
      req.addr_vec.resize(m_num_levels, -1);
      Addr_t addr = req.addr >> m_tx_offset;
      Addr_t col_lo = slice_lower_bits(addr, m_line_bits);
      req.addr_vec[0] = slice_lower_bits(addr, m_addr_bits[0]);
      Addr_t col_hi =
          slice_lower_bits(addr, m_addr_bits[m_col_bits_idx] - m_line_bits);
      req.addr_vec[m_col_bits_idx] = (col_hi << m_line_bits) | col_lo;
      for (int i = 1; i <= m_row_bits_idx; i++) {
        req.addr_vec[i] = slice_lower_bits(addr, m_addr_bits[i]);
      }
    }
};

}   // namespace Ramulator
