#!/usr/bin/env python3
"""Source-level regression for repair timing overlap and additive integration.

This is intentionally a contract audit rather than a numerical DRAM test. It
pins the ordering that makes native timing constraints age during lookup and
guards the architecture boundary: repair is in GenericDRAMSystem, while native
FR-FCFS selection and HBM3 timing remain repair-unaware.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MEM = (ROOT / "src/memory_system/impl/generic_DRAM_system.cpp").read_text()
CTRL = (ROOT / "src/dram_controller/impl/generic_dram_controller.cpp").read_text()
SCHED = (ROOT / "src/dram_controller/impl/scheduler/generic_scheduler.cpp").read_text()
HBM3 = (ROOT / "src/dram/impl/HBM3.cpp").read_text()


def ordered(text: str, *needles: str) -> None:
    positions = [text.index(needle) for needle in needles]
    assert positions == sorted(positions), (needles, positions)


# Lookup is an upstream ready-time gate, not a replacement MC scheduler.
ordered(
    MEM,
    "ready = m_clk + lookup_latency",
    "void tick() override",
    "m_dram->tick();",
    "if (it->ready > m_clk)",
    "m_controllers[target_ch]->send(it->req)",
    "controller->tick();",
)

# The request ages from its original memory-system arrival, including lookup.
assert "req.arrive = m_clk;" in MEM
assert "const Clk_t pipeline_arrive = it->req.arrive;" in MEM
assert "it->req.arrive = pipeline_arrive;" in MEM
assert "if (req.arrive < 0) req.arrive = m_clk;" in CTRL
assert "if (req1->arrive <= req2->arrive)" in SCHED

# The native policy/timing engines know nothing about repair. The controller's
# only repair include is optional address-debug printing, not scheduling logic.
assert "RepairTranslator" not in CTRL
assert "repair_lookup" not in CTRL
assert "repair" not in SCHED.lower()
assert "repair" not in HBM3.lower()
assert "m_dram->check_ready(req_it->command, req_it->addr_vec)" in CTRL

print("Repair interposer/timing-overlap contract: PASS")
print("  native DRAM timing advances during lookup")
print("  original request age is preserved for FR-FCFS")
print("  scheduler and HBM3 timing remain repair-unaware")
