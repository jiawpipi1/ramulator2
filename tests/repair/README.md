# Repair functional-correctness tests

Standalone unit tests for the HBM repair translator (`RepairTranslator`) and the JSON table loader (`HbmRepairTable::load_from_json`). No full Ramulator2 build or gem5 is needed: `test_repair_translator` stubs `base/base.h` (only `AddrVec_t`/`Addr_t` are required) via `tests/repair/stubs/`.

## Run

```bash
bash tests/repair/run_tests.sh                 # hand-written fixtures
bash tests/repair/run_tests.sh path/to/remap.json
```

## What is covered

`test_repair_translator.cpp` (64 checks)
- Layer D dead-bank relocation vs an independent oracle: correct target live bank and vacuum-band row via the band interleave (`h=ceil(R/L)`, `target_row=R-(i+1)h+o`); boundaries row 0 / band-end / next-live-bank / last row; D->A, D->B, D->C fall-through (relocated top row is itself faulty).
- Spread placement remains the default. Opt-in clustered placement is checked at chunk boundaries and last row: exclusive, evenly balanced live targets, preserved D->A/B/C fall-through, exhaustive/injective for the fixture, and rejection of geometries with too few exclusive targets.
- Transaction-granular A / B / C translation on live banks vs an independent oracle: Layer A row/column range hits and same-row clean-column misses; Layer C column boundaries (start, end-inclusive, just-below, gap, adjacent ranges); A>B>C priority. Legacy full-row Layer-A JSON remains covered for compatibility.
- Pass-through (`NONE`) leaves the address vector unchanged.
- Structural invariants: DED / burst / SRAM spare regions pairwise disjoint and above normal rows; Layer D target is always a live bank and lands inside the `[R - F*h, R)` vacuum band.

`test_json_roundtrip.cpp` (26 checks)
- A hand-written all-layers JSON (incl. a `config` block) loads into the exact expected in-memory tables + geometry and drives `translate()` correctly (incl. Layer D relocation from the loaded geometry, and Layer C `target_slot >= bursts_per_row`, i.e. frag > 0).
- Transaction-granular Layer-A source ranges and SRAM target slots load exactly; target-slot overflow is rejected.
- A real `repairv2` output file loads without error and yields non-empty tables.

`test_interposer_contract.py`
- Pins the timing order that advances native DRAM state while a request waits in the repair pipeline, so a pre-existing issue constraint ages during a 12-cycle slow lookup instead of restarting when the request reaches the MC.
- Pins preservation of the original arrival timestamp through pipeline backpressure and FR-FCFS age comparison.
- Guards the additive integration boundary: the repair translator/pipeline stays in `GenericDRAMSystem`; native FR-FCFS and HBM3 timing contain no repair policy.

## Notes

- `repairv2` now emits a `config` block; `load_from_json` reads it so the table is self-describing (spare-row bases + Layer D geometry no longer depend on compiled defaults). Old JSONs without it fall back to HBM3 defaults.
- Offline and runtime spread feasibility both use exact `K=F*ceil(R/(N-F))`. Clustered reuses the same configured `vacuum_limit` capacity contract and additionally requires `F*ceil(R/vacuum_limit) <= live_banks`.
- End-to-end oracle usage: `bash tests/repair/toy_dataflow/run_toy_dataflow.sh path/to/remap.json spread` or replace the last argument with `clustered`.
