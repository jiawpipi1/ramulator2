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

echo "== building validate_repair_directory =="
$CXX $STD $INC_STUB $INC_SRC $INC_EXT \
    tests/repair/validate_repair_directory.cpp \
    src/dram_controller/impl/repair/repair_table.cpp \
    -o tests/repair/validate_repair_directory

echo "== building test_req_buffer =="
$CXX $STD $INC_STUB $INC_SRC \
    tests/repair/test_req_buffer.cpp src/base/request.cpp \
    -o tests/repair/test_req_buffer

rc=0
echo; echo "== running test_repair_translator =="
./tests/repair/test_repair_translator || rc=1
echo; echo "== running test_json_roundtrip =="
# optional arg: path to a freshly generated repairv2 sample
if [ "$#" -gt 0 ]; then
  ./tests/repair/test_json_roundtrip "$1" || rc=1
else
./tests/repair/test_json_roundtrip || rc=1
fi

echo; echo "== running offline pipeline regressions =="
python3 tests/repair/test_offline_pipeline.py || rc=1

echo; echo "== running ReqBuffer capacity regression =="
./tests/repair/test_req_buffer || rc=1

echo; echo "== auditing additive interposer and lookup-overlap contract =="
python3 tests/repair/test_interposer_contract.py || rc=1

echo
if [ "$rc" -eq 0 ]; then echo "ALL REPAIR TESTS PASSED"; else echo "SOME REPAIR TESTS FAILED"; fi
exit $rc
