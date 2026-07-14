#include "memory_system/memory_system.h"
#include "translation/translation.h"
#include "dram_controller/controller.h"
#include "addr_mapper/addr_mapper.h"
#include "dram/dram.h"
#include "dram_controller/impl/repair/repair_debug.h"
#include "dram_controller/impl/repair/repair_table.h"
#include "dram_controller/impl/repair/repair_translator.h"

#include <deque>
#include <memory>
#include <stdexcept>

namespace Ramulator {

class GenericDRAMSystem final : public IMemorySystem, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IMemorySystem, GenericDRAMSystem, "GenericDRAM", "A generic DRAM-based memory system.");

  protected:
    Clk_t m_clk = 0;
    IDRAM*  m_dram;
    IAddrMapper*  m_addr_mapper;
    std::vector<IDRAMController*> m_controllers;
    HbmRepairTable m_repair_table;
    std::unique_ptr<RepairTranslator> m_repair_translator;
    struct PendingRepair {
      Clk_t ready;
      Request req;
      bool layer_a_sram;
    };
    std::deque<PendingRepair> m_repair_pipeline;
    int m_repair_lookup_latency = 0;
    int m_layer_a_data_latency = 6;
    size_t m_repair_pipeline_size = 64;
    std::vector<size_t> s_repair_none, s_repair_layer_a, s_repair_layer_b;
    std::vector<size_t> s_repair_layer_c, s_repair_layer_d;
    std::vector<size_t> s_bloom_reject, s_bloom_maybe;

  public:
    int s_num_read_requests = 0;
    int s_num_write_requests = 0;
    int s_num_other_requests = 0;


  public:
    void init() override { 
      // Create device (a top-level node wrapping all channel nodes)
      m_dram = create_child_ifce<IDRAM>();
      m_addr_mapper = create_child_ifce<IAddrMapper>();

      int num_channels = m_dram->get_level_size("channel");

      YAML::Node controller_cfg = m_config["Controller"];
      if (controller_cfg && controller_cfg["repair_table_path"]) {
        std::string path = controller_cfg["repair_table_path"].as<std::string>();
        if (!HbmRepairTable::load_from_json(path, m_repair_table))
          throw std::runtime_error("Failed to load/validate repair table: " + path);
        const auto& tc = m_repair_table.cfg;
        const int addressable_cols = m_dram->get_level_size("column") /
                                     m_dram->m_internal_prefetch_size;
        if (tc.num_channels != num_channels ||
            tc.num_pch != m_dram->get_level_size("pseudochannel") ||
            tc.num_bg != m_dram->get_level_size("bankgroup") ||
            tc.num_ba != m_dram->get_level_size("bank") ||
            tc.rows_per_bank != m_dram->get_level_size("row") ||
            tc.bursts_per_row != addressable_cols)
          throw std::runtime_error(
              "Repair-table geometry does not match the DRAM organization");
        bool bloom = controller_cfg["repair_enable_bloom"]
                   ? controller_cfg["repair_enable_bloom"].as<bool>() : true;
        m_repair_lookup_latency = controller_cfg["repair_lookup_latency"]
                                ? controller_cfg["repair_lookup_latency"].as<int>() : 0;
        m_layer_a_data_latency = controller_cfg["layer_a_data_latency"]
                               ? controller_cfg["layer_a_data_latency"].as<int>() : 6;
        m_repair_pipeline_size = controller_cfg["repair_pipeline_size"]
                               ? controller_cfg["repair_pipeline_size"].as<size_t>() : 64;
        if (m_repair_lookup_latency < 0 || m_layer_a_data_latency < 0 ||
            m_repair_pipeline_size == 0)
          throw std::runtime_error("repair latencies must be non-negative and pipeline size positive");
        if (controller_cfg["repair_debug_addr"] &&
            controller_cfg["repair_debug_addr"].as<bool>())
          g_debug_address.store(true, std::memory_order_relaxed);
        m_repair_translator = std::make_unique<RepairTranslator>(m_repair_table, bloom);
      }

      // Create memory controllers
      for (int i = 0; i < num_channels; i++) {
        IDRAMController* controller = create_child_ifce<IDRAMController>();
        controller->m_impl->set_id(fmt::format("Channel {}", i));
        controller->m_channel_id = i;
        m_controllers.push_back(controller);
      }

      m_clock_ratio = param<uint>("clock_ratio").required();

      register_stat(m_clk).name("memory_system_cycles");
      register_stat(s_num_read_requests).name("total_num_read_requests");
      register_stat(s_num_write_requests).name("total_num_write_requests");
      register_stat(s_num_other_requests).name("total_num_other_requests");
      s_repair_none.resize(num_channels); s_repair_layer_a.resize(num_channels);
      s_repair_layer_b.resize(num_channels); s_repair_layer_c.resize(num_channels);
      s_repair_layer_d.resize(num_channels); s_bloom_reject.resize(num_channels);
      s_bloom_maybe.resize(num_channels);
      for (int ch = 0; ch < num_channels; ++ch) {
        register_stat(s_repair_none[ch]).name("repair_none_{}", ch);
        register_stat(s_repair_layer_a[ch]).name("repair_layer_a_{}", ch);
        register_stat(s_repair_layer_b[ch]).name("repair_layer_b_{}", ch);
        register_stat(s_repair_layer_c[ch]).name("repair_layer_c_{}", ch);
        register_stat(s_repair_layer_d[ch]).name("repair_layer_d_{}", ch);
        register_stat(s_bloom_reject[ch]).name("bloom_reject_{}", ch);
        register_stat(s_bloom_maybe[ch]).name("bloom_maybe_{}", ch);
      }
    };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override { }


    bool send(Request req) override {
      m_addr_mapper->apply(req);
      RepairType repair_type = RepairType::NONE;
      bool layer_a_sram = false;
      bool bloom_reject = false;
      if (m_repair_translator) {
        repair_type = m_repair_translator->translate(req.addr_vec, req.addr);
        layer_a_sram = m_repair_translator->last_uses_layer_a_sram();
        bloom_reject = m_repair_translator->last_bloom_reject();
      }
      int channel_id = req.addr_vec[0];  // route AFTER repair translation
      bool needs_pipeline = m_repair_translator &&
                            (m_repair_lookup_latency > 0 || layer_a_sram);
      bool is_success = false;
      if (needs_pipeline) {
        if (m_repair_pipeline.size() >= m_repair_pipeline_size) return false;
        req.arrive = m_clk;
        Clk_t ready = m_clk + m_repair_lookup_latency +
                      (layer_a_sram ? m_layer_a_data_latency : 0);
        m_repair_pipeline.push_back({ready, req, layer_a_sram});
        is_success = true;
      } else {
        is_success = m_controllers[channel_id]->send(req);
      }

      if (is_success) {
        switch (req.type_id) {
          case Request::Type::Read: {
            s_num_read_requests++;
            break;
          }
          case Request::Type::Write: {
            s_num_write_requests++;
            break;
          }
          default: {
            s_num_other_requests++;
            break;
          }
        }
        if (m_repair_translator) {
          switch (repair_type) {
            case RepairType::NONE:    ++s_repair_none[channel_id]; break;
            case RepairType::LAYER_A: ++s_repair_layer_a[channel_id]; break;
            case RepairType::LAYER_B: ++s_repair_layer_b[channel_id]; break;
            case RepairType::LAYER_C: ++s_repair_layer_c[channel_id]; break;
            case RepairType::LAYER_D: ++s_repair_layer_d[channel_id]; break;
          }
          if (m_repair_translator->bloom_enabled()) {
            if (bloom_reject) ++s_bloom_reject[channel_id];
            else              ++s_bloom_maybe[channel_id];
          }
        }
      }

      return is_success;
    };

    bool is_empty() override {
      if (!m_repair_pipeline.empty()) return false;
      for (auto& ctrl : m_controllers) {
        if (!ctrl->is_empty()) return false;
      }
      return true;
    }
    
    void tick() override {
      m_clk++;
      m_dram->tick();
      for (auto it = m_repair_pipeline.begin(); it != m_repair_pipeline.end(); ) {
        if (it->ready > m_clk) { ++it; continue; }
        if (it->layer_a_sram) {
          if (it->req.type_id == Request::Type::Read && it->req.callback)
            it->req.callback(it->req);
          it = m_repair_pipeline.erase(it);
          continue;
        }
        int target_ch = it->req.addr_vec[0];
        // The controller stamps req.arrive = -1 on a failed enqueue. This request
        // was already accepted by the memory system when it entered the repair
        // pipeline, so restore its true arrival cycle; otherwise a retry under
        // backpressure would re-stamp a later arrive and under-report read latency
        // by exactly the delay the repair pipeline introduced.
        const Clk_t pipeline_arrive = it->req.arrive;
        if (m_controllers[target_ch]->send(it->req)) {
          it = m_repair_pipeline.erase(it);
        } else {
          it->req.arrive = pipeline_arrive;
          ++it;
        }
      }
      for (auto controller : m_controllers) {
        controller->tick();
      }
    };

    float get_tCK() override {
      return m_dram->m_timing_vals("tCK_ps") / 1000.0f;
    }

    int get_transaction_size() override {
      return m_dram->m_internal_prefetch_size * m_dram->m_channel_width / 8;
    }

    // const SpecDef& get_supported_requests() override {
    //   return m_dram->m_requests;
    // };
};
  
}   // namespace 

