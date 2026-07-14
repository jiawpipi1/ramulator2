#!/bin/bash
# run_all.sh
# 用法：bash run_all.sh

RAMULATOR="./build/ramulator2"
YAML="hbm3.yaml"
JSON_DIR="/home/pitsaiyang/work/Fault_yield/remap_json"
OUT_DIR="./results"
mkdir -p "$OUT_DIR"

for i in $(seq 0 1335); do
    JSON="${JSON_DIR}/remap_hbm_${i}.json"
    
    # 跳過不存在的 json
    [ -f "$JSON" ] || continue
    
    OUT="${OUT_DIR}/result_${i}.txt"
    
    # 已跑過就跳過（方便中斷後續跑）
    [ -f "$OUT" ] && continue
    
    "$RAMULATOR" -f "$YAML" \
        -p MemorySystem.Controller.repair_table_path="$JSON" \
        > "$OUT" 2>&1
    
    echo "Done: hbm_id=$i"
done

echo "All done!"