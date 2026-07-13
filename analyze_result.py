#!/usr/bin/env python3
import os
import re
import json

RESULTS_DIR = "./results"

def parse_result(filepath):
    with open(filepath) as f:
        text = f.read()
    
    d = {}
    
    # hbm_id
    m = re.search(r'hbm_id\s*=\s*(\d+)', text)
    d['hbm_id'] = int(m.group(1)) if m else -1
    
    # cycles
    m = re.search(r'cycles_recorded_core_0:\s*([\d.]+)', text)
    d['cycles'] = float(m.group(1)) if m else None
    
    # memory_system_cycles
    m = re.search(r'memory_system_cycles:\s*([\d.]+)', text)
    d['mem_cycles'] = float(m.group(1)) if m else None
    
    # avg_read_latency 
    latencies = [float(x) for x in re.findall(r'avg_read_latency_\d+:\s*([\d.]+)', text)]
    d['avg_latency'] = sum(latencies)/len(latencies) if latencies else None
    
    # repair counts
    d['repair_none']    = sum(int(x) for x in re.findall(r'repair_none_\d+:\s*(\d+)', text))
    d['repair_layer_a'] = sum(int(x) for x in re.findall(r'repair_layer_a_\d+:\s*(\d+)', text))
    d['repair_layer_b'] = sum(int(x) for x in re.findall(r'repair_layer_b_\d+:\s*(\d+)', text))
    d['repair_layer_c'] = sum(int(x) for x in re.findall(r'repair_layer_c_\d+:\s*(\d+)', text))
    d['repair_layer_d'] = sum(int(x) for x in re.findall(r'repair_layer_d_\d+:\s*(\d+)', text))
    d['total_repaired'] = d['repair_layer_a'] + d['repair_layer_b'] + d['repair_layer_c'] + d['repair_layer_d']

    # fault table summary
    m = re.search(r'K \(vacuum rows\)\s*=\s*(\d+)', text)
    d['K'] = int(m.group(1)) if m else 0
    m = re.search(r'Layer D bad banks\s*:\s*(\d+)', text)
    d['bad_banks'] = int(m.group(1)) if m else 0
    m = re.search(r'Layer B DED rows\s*:\s*(\d+)', text)
    d['ded_rows'] = int(m.group(1)) if m else 0
    
    return d

all_data = []
for fname in sorted(os.listdir(RESULTS_DIR), key=lambda x: int(re.search(r'\d+', x).group())):
    if not fname.startswith("result_") or not fname.endswith(".txt"):
        continue
    fpath = os.path.join(RESULTS_DIR, fname)
    try:
        d = parse_result(fpath)
        all_data.append(d)
    except Exception as e:
        print(f"Skip {fname}: {e}")



print(f"\n  {len(all_data)} data\n")


total_reqs_list = []
for fname in sorted(os.listdir(RESULTS_DIR), key=lambda x: int(re.search(r'\d+', x).group())):
    if not fname.startswith("result_") or not fname.endswith(".txt"):
        continue
    with open(os.path.join(RESULTS_DIR, fname)) as f:
        text = f.read()
    m = re.search(r'total_num_read_requests:\s*(\d+)', text)
    if m:
        total_reqs_list.append(int(m.group(1)))
        break
TOTAL_REQS = total_reqs_list[0] if total_reqs_list else 586973
print(f"  total_num_read_requests = {TOTAL_REQS}\n")

def summarize(data, key, is_latency=False):
    vals = [(d['hbm_id'], d[key]) for d in data if d[key] is not None]
    if not vals: return
    ids, vs = zip(*vals)
    avg = sum(vs) / len(vs)
    best_i  = vs.index(min(vs)) if is_latency else vs.index(min(vs))
    worst_i = vs.index(max(vs))
    print(f"  average: {avg:.4f}")
    print(f"  best: {vs[best_i]:.4f}  (hbm_id={ids[best_i]})")
    print(f"  worse: {vs[worst_i]:.4f}  (hbm_id={ids[worst_i]})")

def summarize_repair(data, key, total_reqs):
    vals = [(d['hbm_id'], d[key]) for d in data if d.get(key) is not None]
    if not vals: return
    ids, vs = zip(*vals)
    avg     = sum(vs) / len(vs)
    avg_pct = avg / total_reqs * 100
    best_i  = vs.index(min(vs))
    worst_i = vs.index(max(vs))
    print(f"  average: {avg:.2f}  ({avg_pct:.4f}% of all requests)")
    print(f"  less: {vs[best_i]}  ({vs[best_i]/total_reqs*100:.4f}%)  (hbm_id={ids[best_i]})")
    print(f"  most: {vs[worst_i]}  ({vs[worst_i]/total_reqs*100:.4f}%)  (hbm_id={ids[worst_i]})")

print("=" * 55)


print("\n  cycles_recorded_core_0")
summarize(all_data, 'cycles', is_latency=True)

print("\n  avg_read_latency (cycles)")
summarize(all_data, 'avg_latency', is_latency=True)

print("\n  memory_system_cycles")
summarize(all_data, 'mem_cycles', is_latency=True)


print("\n" + "=" * 55)
print(" REPAIR hit (all 16 channels )")
print("=" * 55)

for layer, key in [("Layer A (SRAM rows)",  "repair_layer_a"),
                   ("Layer B (DED rows)",    "repair_layer_b"),
                   ("Layer C (burst segs)",  "repair_layer_c"),
                   ("Layer D (bad banks)",   "repair_layer_d"),
                   ("Total repaired",        "total_repaired"),
                   ("NONE (no repair)",      "repair_none")]:
    print(f"\n {layer}")
    summarize_repair(all_data, key, TOTAL_REQS)


print("\n" + "=" * 55)
print(" each Layer hit(>0) HBM number")
print("=" * 55)
total_hbm = len(all_data)
for layer, key in [("Layer A", "repair_layer_a"),
                   ("Layer B", "repair_layer_b"),
                   ("Layer C", "repair_layer_c"),
                   ("Layer D", "repair_layer_d")]:
    count = sum(1 for d in all_data if d.get(key, 0) > 0)
    print(f"  {layer}: {count}/{total_hbm} HBMs ({count/total_hbm*100:.1f}%) have actual hit")

print("\n" + "=" * 55)


import csv
out_csv = "summary_all.csv"
fields = ['hbm_id','cycles','mem_cycles','avg_latency',
          'repair_none','repair_layer_a','repair_layer_b',
          'repair_layer_c','repair_layer_d','total_repaired',
          'K','bad_banks','ded_rows']
with open(out_csv, 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    w.writerows(all_data)
print(f"\n CSV: {out_csv}")