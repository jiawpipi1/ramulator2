#!/usr/bin/env bash
# run_toy_dataflow.sh -- end-to-end check of the Ramulator2 repair dataflow.
#
# Builds a trace whose every request has an independently PREDICTED repair
# outcome, runs Ramulator2 on it, and diffs the predicted per-layer counts
# against Ramulator's repair_{none,layer_a..d} stats. Any drift in the address
# decode, the JSON table load, or a layer's lookup shows up as a count mismatch.
#
# Run this after ANY change to: the fault-map schema, the address mapping, the
# DRAM org (esp. channel_width / column count), or repair_translator.h.
#
#   bash tests/repair/toy_dataflow/run_toy_dataflow.sh [remap_hbm_<id>.json]

set -euo pipefail
cd "$(dirname "$0")/../../.."          # -> ramulator2/

RAM=./build_new/ramulator2
JSON="${1:-/home/pitsaiyang/work/my_work/Fault_yield/remap_json_hbm3/remap_hbm_1001.json}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

[ -x "$RAM" ] || { echo "FAIL: $RAM not built (cmake --build build_new -j)"; exit 1; }
[ -f "$JSON" ] || { echo "FAIL: remap JSON not found: $JSON"; exit 1; }

# The DRAM org is DERIVED ENTIRELY from the table's own config block, so this test
# works for ANY device -- HBM3, HBM4, or an arbitrary W2W stack -- with no edits.
# NOTE: Ramulator ships no HBM4 DRAM impl, so non-HBM3 geometries are instantiated
# on the HBM3 model. That gives correct ADDRESSING (what this test checks) but
# HBM3 TIMINGS. Timings do not affect the repair dataflow.
eval "$(python3 - "$JSON" <<'PYEOF'
import json, sys
c = json.load(open(sys.argv[1]))["config"]
ba = c["num_banks"] if "num_banks" in c else c["num_ba"]
print(f'ORG_CH={c["num_channels"]}')
print(f'ORG_PCH={c["num_pch"]}')
print(f'ORG_BG={c["num_bg"]}')
print(f'ORG_BA={ba}')
print(f'ORG_ROW={c["rows_per_bank"]}')
org_col = c["bursts_per_row"] * 2            # x prefetch(2): addressable = col/2
dq = 128                                      # HBM3 preset DQ
# Ramulator VALIDATES channel density against the org. A non-HBM3 geometry has a
# different density, so it must be declared or HBM3.cpp aborts.
density = c["num_pch"] * c["num_bg"] * ba * c["rows_per_bank"] * org_col * dq // (1024 * 1024)
print(f'ORG_COL={org_col}')
print(f'ORG_DENSITY={density}')
print(f'DESC={c["num_channels"]}ch_{c["num_pch"]}pch_{c["num_bg"]}bg_{ba}ba_'
      f'{c["rows_per_bank"]}row_{c["bursts_per_row"]}col')
PYEOF
)"
echo "[toy] table  : $JSON"
echo "[toy] device : $DESC   (org derived from the table's config block)"

python3 tests/repair/toy_dataflow/make_toy_trace.py \
  --json "$JSON" --trace "$TMP/toy.trace" --expect "$TMP/expect.json" --n 8

cat > "$TMP/toy.yaml" <<EOF
Frontend:
  impl: LoadStoreTrace
  path: $TMP/toy.trace
  clock_ratio: 4
MemorySystem:
  impl: GenericDRAM
  clock_ratio: 2
  DRAM:
    impl: HBM3
    org:
      preset: HBM3_4Gb
      channel: $ORG_CH
      pseudochannel: $ORG_PCH
      bankgroup: $ORG_BG
      bank: $ORG_BA
      row: $ORG_ROW
      column: $ORG_COL
      density: $ORG_DENSITY
      channel_width: 128
    timing: { preset: HBM3_2Gbps }
  Controller:
    impl: Generic
    Scheduler: { impl: FRFCFS }
    RefreshManager: { impl: NoRefresh }
    RowPolicy: { impl: OpenRowPolicy }
    repair_table_path: $JSON
    plugins:
  AddrMapper:
    impl: ChRaBaRoCo
EOF

"$RAM" -f "$TMP/toy.yaml" 2>/dev/null > "$TMP/out.txt"

TMP="$TMP" python3 - <<'PY'
import json, os, re, sys
tmp = os.environ["TMP"]
out = open(f"{tmp}/out.txt").read()
exp = json.load(open(f"{tmp}/expect.json"))["expect"]
got = {k: sum(int(m.group(1))
              for m in re.finditer(rf"repair_{k}_\d+:\s*(\d+)", out)) for k in exp}

print(f"\n{'layer':<10}{'expected':>10}{'ramulator':>11}   verdict")
print("-" * 44)
ok = True
for k in ("none", "layer_a", "layer_b", "layer_c", "layer_d"):
    match = exp[k] == got[k]
    ok &= match
    print(f"{k:<10}{exp[k]:>10}{got[k]:>11}   {'MATCH' if match else '*** MISMATCH ***'}")
print("-" * 44)
if ok:
    print("PASS: repair dataflow verified end-to-end")
else:
    print("FAIL: Ramulator does not match the independent prediction")
    sys.exit(1)
PY
