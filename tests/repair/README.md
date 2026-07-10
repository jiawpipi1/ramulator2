# Repair functional-correctness tests

Standalone unit tests for the HBM repair translator (`RepairTranslator`) and the
JSON table loader (`HbmRepairTable::load_from_json`). They do **not** require a
full Ramulator2 build or gem5: `test_repair_translator` stubs `base/base.h`
(only `AddrVec_t`/`Addr_t` are needed) via `tests/repair/stubs/`.

## Run

```bash
bash tests/repair/run_tests.sh                 # uses json/remap_hbm3_404.json as the real sample
bash tests/repair/run_tests.sh path/to/remap.json
```

## What is covered

`test_repair_translator.cpp` (33 checks)
- Layer D dead-bank relocation vs an independent oracle: correct target live
  bank + vacuum-band row via the band interleave (`h=ceil(R/L)`,
  `target_row=R-(i+1)h+o`); boundaries row 0 / band-end / next-live-bank / last
  row; and D->A, D->B, D->C fall-through (relocated top row is itself faulty).
- A / B / C translation on live banks with an independent oracle; Layer C col
  boundaries (start, end-inclusive, just-below, gap, adjacent ranges); A>B>C
  priority.
- Pass-through (`NONE`) leaves the address vector unchanged.
- Structural invariants: DED / burst / SRAM spare regions pairwise disjoint and
  above normal rows; Layer D target is always a live bank and lands inside the
  `[R - F*h, R)` vacuum band.

`test_json_roundtrip.cpp` (20 checks)
- A hand-written all-layers JSON (incl. a `config` block) loads into the exact
  expected in-memory tables + geometry, and drives `translate()` correctly
  (incl. Layer D relocation from the loaded geometry, and Layer C
  `target_slot >= bursts_per_row`, i.e. frag > 0).
- A real `repairv2` output file loads without error and yields non-empty tables.

## Notes

- `repairv2` now emits a `config` block; `load_from_json` reads it so the table
  is self-describing (spare-row bases + Layer D geometry no longer depend on
  compiled defaults). Old JSONs without it fall back to HBM3 defaults.
- Still to reconcile: the offline pass/fail `K = ceil(F*R/(N-F))` slightly
  underestimates the true per-live-bank vacuum reservation `F*ceil(R/(N-F))`
  used at request time. Align these (and the OS-reported capacity) if exact
  capacity numbers are needed. See repo-root claude.md, Part I.6 / Part III.
