#!/usr/bin/env bash
# Functional-correctness tests for the HBM repair translator/table.
# Run from the ramulator2/ repo root:  bash tests/repair/run_tests.sh
set -euo pipefail
cd "$(dirname "$0")/../.."   # -> ramulator2/

CXX=${CXX:-g++}
STD="-std=c++17"
INC_STUB="-Itests/repair/stubs"
INC_SRC="-Isrc"
INC_EXT="-Iext"

echo "== building test_repair_translator =="
$CXX $STD $INC_STUB $INC_SRC \
    tests/repair/test_repair_translator.cpp \
    -o tests/repair/test_repair_translator

echo "== building test_json_roundtrip =="
$CXX $STD $INC_STUB $INC_SRC $INC_EXT \
    tests/repair/test_json_roundtrip.cpp \
    src/dram_controller/impl/repair/repair_table.cpp \
    -o tests/repair/test_json_roundtrip

rc=0
echo; echo "== running test_repair_translator =="
./tests/repair/test_repair_translator || rc=1
echo; echo "== running test_json_roundtrip =="
# optional arg: path to a real repairv2 sample (defaults to json/remap_hbm3_404.json)
./tests/repair/test_json_roundtrip "${1:-json/remap_hbm3_404.json}" || rc=1

echo
if [ "$rc" -eq 0 ]; then echo "ALL REPAIR TESTS PASSED"; else echo "SOME REPAIR TESTS FAILED"; fi
exit $rc
