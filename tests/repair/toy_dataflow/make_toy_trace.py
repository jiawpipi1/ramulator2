#!/usr/bin/env python3
"""
make_toy_trace.py -- build a LoadStoreTrace whose every request has a KNOWN,
predicted repair outcome, so the Ramulator2 repair dataflow can be verified
end-to-end before we point gem5 at it.

For each repair layer we emit a batch of addresses that MUST hit that layer, and
we independently re-implement the translator's precedence (D -> A -> B -> C) in
Python to predict the exact per-layer counts. Running Ramulator2 on the trace and
diffing its repair_{none,layer_a..d} stats against those predictions is a sharp,
falsifiable check: if the address decode, the JSON load, or any layer's lookup is
wrong, the counts will not line up.

ADDRESS LAYOUT (measured empirically from Ramulator2, not derived), with
`channel_width: 128` so a transaction is 32 B and the page is a JEDEC 1 KB:

    bits 32..29 ch(4) | 28 pch(1) | 27..26 bg(2) | 25..24 ba(2)
    bits 23..10 row(14)           | 9..5 col(5)  | 4..0 tx offset (32 B)

The column field is 5 bits => 32 addressable slots per row. That is deliberate
and is the WHOLE point of the granularity rule: the DRAM's smallest atomic data
group is one transaction (internal prefetch = 2 CONSECUTIVE bursts, 32 B total),
so one transaction is the finest thing an address remap can redirect. The fault
map therefore also uses 32 column slots per row -- units match by construction,
and no unit conversion is needed anywhere.
"""

import argparse
import json

def _log2(n):
    b = n.bit_length() - 1
    assert 1 << b == n, f"{n} is not a power of two"
    return b


class Geometry:
    """Address layout DERIVED FROM THE TABLE'S OWN `config` BLOCK.

    Nothing here is HBM-specific: give it any W2W stack's parameters and the
    packing follows. That is the point -- the pipeline is parameter-driven, so a
    new device is a config change, not a code change.
    """

    def __init__(self, cfg, tx_bits):
        self.ch_bits = _log2(cfg["num_channels"])
        self.pch_bits = _log2(cfg["num_pch"])
        self.bg_bits = _log2(cfg["num_bg"])
        self.ba_bits = _log2(cfg["num_ba"])
        self.row_bits = _log2(cfg["rows_per_bank"])
        self.col_bits = _log2(cfg["bursts_per_row"])
        self.tx_bits = tx_bits
        self.n_cols = cfg["bursts_per_row"]

    def addr(self, ch, pch, bg, ba, row, col):
        a = ch
        a = (a << self.pch_bits) | pch
        a = (a << self.bg_bits) | bg
        a = (a << self.ba_bits) | ba
        a = (a << self.row_bits) | row
        a = (a << self.col_bits) | col
        return a << self.tx_bits

    def describe(self):
        return (f"ch({self.ch_bits}) pch({self.pch_bits}) bg({self.bg_bits}) "
                f"ba({self.ba_bits}) row({self.row_bits}) col({self.col_bits}) "
                f"tx({self.tx_bits})")


class Table:
    """Mirror of HbmRepairTable, loaded from a repairv2 remap JSON."""

    def __init__(self, path):
        d = json.load(open(path))
        self.cfg = d["config"]
        self.bad = {(b["ch"], b["pch"], b["bg"], b["ba"]) for b in d["layer_d_bad_banks"]}
        self.sram = {(e["ch"], e["pch"], e["bg"], e["ba"], e["row"]): e["sram_slot"]
                     for e in d["layer_a_sram"]}
        self.ded, self.burst = {}, {}
        for b in d["banks"]:
            k = (b["ch"], b["pch"], b["bg"], b["ba"])
            for off, r in enumerate(b.get("layer_b_ded_rows", [])):
                self.ded[k + (r,)] = off
            for e in b.get("layer_c_burst", []):
                self.burst.setdefault(k + (e["row"],), []).append(e)

        self.live = []
        for ch in range(self.cfg["num_channels"]):
            for bg in range(self.cfg["num_bg"]):
                for pch in range(self.cfg["num_pch"]):
                    for ba in range(self.cfg["num_ba"]):
                        bank = (ch, pch, bg, ba)
                        if bank not in self.bad:
                            self.live.append(bank)
        self.dead = sorted(self.bad, key=lambda x: (x[0], x[2], x[1], x[3]))

    def classify(self, ch, pch, bg, ba, row, col):
        """Re-implement RepairTranslator::translate precedence: D, then A/B/C.
        `col` and the table use the same addressable transaction-slot units."""
        res = "none"
        if (ch, pch, bg, ba) in self.bad:
            return "layer_d"          # D relocates then falls through; it still reports D
        if (ch, pch, bg, ba, row) in self.sram:
            return "layer_a"
        if (ch, pch, bg, ba, row) in self.ded:
            return "layer_b"
        # Layer C: the translator compares addr_vec col DIRECTLY against col_start.
        for e in self.burst.get((ch, pch, bg, ba, row), []):
            if e["col_start"] <= col < e["col_start"] + e["length"]:
                return "layer_c"
        return res

    def route(self, ch, pch, bg, ba, row, col):
        """Mirror pre-controller D routing and the Layer-A SRAM bypass."""
        source = (ch, pch, bg, ba)
        result = "none"
        if source in self.bad:
            result = "layer_d"
            h = (self.cfg["rows_per_bank"] + len(self.live) - 1) // len(self.live)
            ordinal = self.dead.index(source)
            ch, pch, bg, ba = self.live[row // h]
            row = self.cfg["rows_per_bank"] - (ordinal + 1) * h + row % h
        key = (ch, pch, bg, ba, row)
        uses_sram = key in self.sram
        if result == "none":
            result = self.classify(ch, pch, bg, ba, row, col)
        return result, ch, uses_sram


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--json", required=True, help="remap_hbm_<id>.json from repairv2")
    p.add_argument("--trace", required=True)
    p.add_argument("--expect", required=True)
    p.add_argument("--n", type=int, default=8, help="requests per layer")
    p.add_argument("--tx_bits", type=int, default=5,
                   help="log2(transaction bytes); 5 = 32 B (prefetch 2 x 128b/8)")
    a = p.parse_args()

    t = Table(a.json)
    g = Geometry(t.cfg, a.tx_bits)
    N_AV_COLS = g.n_cols
    print(f"[toy] geometry from the table's own config block: {g.describe()}")
    print(f"[toy] {t.cfg}")
    reqs = []          # (addr, ch,pch,bg,ba,row,col, intent)

    def add(ch, pch, bg, ba, row, col, intent):
        reqs.append((g.addr(ch, pch, bg, ba, row, col),
                     ch, pch, bg, ba, row, col, intent))

    # -- Layer D: a dead bank. Any row must be relocated.
    for (ch, pch, bg, ba) in sorted(t.bad)[:1]:
        for i in range(a.n):
            add(ch, pch, bg, ba, i * 991 % t.cfg["rows_per_bank"], 0, "layer_d")

    # -- Layer A: channel-SRAM rows (skip banks that are dead).
    n = 0
    for (ch, pch, bg, ba, row) in sorted(t.sram):
        if n >= a.n:
            break
        if (ch, pch, bg, ba) in t.bad:
            continue
        add(ch, pch, bg, ba, row, 0, "layer_a")
        n += 1

    # -- Layer B: dedicated spare rows (must not also be a Layer A row).
    n = 0
    for (ch, pch, bg, ba, row) in sorted(t.ded):
        if n >= a.n:
            break
        if (ch, pch, bg, ba) in t.bad or (ch, pch, bg, ba, row) in t.sram:
            continue
        add(ch, pch, bg, ba, row, 0, "layer_b")
        n += 1

    # -- Layer C: burst remaps. Units now match (32 == 32), so EVERY entry is
    #    reachable; we sample across the whole column range to prove it.
    n = 0
    for key in sorted(t.burst):
        ch, pch, bg, ba, row = key
        if (ch, pch, bg, ba) in t.bad or (ch, pch, bg, ba, row) in t.sram \
           or (ch, pch, bg, ba, row) in t.ded:
            continue
        for e in t.burst[key]:
            if n >= a.n:
                break
            assert e["col_start"] < N_AV_COLS, \
                f"col_start {e['col_start']} outside addressable columns"
            # Exercise offsets inside multi-column ranges, not only col_start.
            col = e["col_start"] + (n % e["length"])
            assert col < N_AV_COLS, f"Layer C range exceeds addressable columns: {e}"
            add(ch, pch, bg, ba, row, col, "layer_c")
            n += 1
        if n >= a.n:
            break

    # -- NONE: a clean bank, rows far from any repaired row.
    clean = None
    for ch in range(t.cfg["num_channels"]):
        for pch in range(t.cfg["num_pch"]):
            for bg in range(t.cfg["num_bg"]):
                for ba in range(t.cfg["num_ba"]):
                    k = (ch, pch, bg, ba)
                    if k in t.bad:
                        continue
                    if any(x[:4] == k for x in t.sram) or any(x[:4] == k for x in t.ded):
                        continue
                    if any(x[:4] == k for x in t.burst):
                        continue
                    clean = k
                    break
                if clean: break
            if clean: break
        if clean: break
    if clean:
        ch, pch, bg, ba = clean
        for i in range(a.n):
            add(ch, pch, bg, ba, 100 + i, 0, "none")

    # -- predict the outcome of every request with the mirrored translator
    expect = {"none": 0, "layer_a": 0, "layer_b": 0, "layer_c": 0, "layer_d": 0}
    dram_channel_reads = [0] * t.cfg["num_channels"]
    detail = []
    for (addr, ch, pch, bg, ba, row, col, intent) in reqs:
        got, target_ch, uses_sram = t.route(ch, pch, bg, ba, row, col)
        expect[got] += 1
        if not uses_sram:
            dram_channel_reads[target_ch] += 1
        detail.append({"addr": addr, "ch": ch, "pch": pch, "bg": bg, "ba": ba,
                       "row": row, "col": col, "intent": intent, "predicted": got,
                       "target_ch": target_ch, "uses_layer_a_sram": uses_sram})

    with open(a.trace, "w") as f:
        for r in reqs:
            f.write(f"LD {r[0]}\n")

    json.dump({"total": len(reqs), "expect": expect,
               "dram_channel_reads": dram_channel_reads, "detail": detail},
              open(a.expect, "w"), indent=1)

    print(f"[toy] {len(reqs)} requests -> {a.trace}")
    print(f"[toy] predicted: {expect}")
    print(f"[toy] predicted DRAM-controller reads: {dram_channel_reads}")
    intents = {}
    for d in detail:
        intents.setdefault(d["intent"], []).append(d["predicted"])
    print("[toy] intent -> predicted:")
    bad = 0
    for k, v in sorted(intents.items()):
        agree = sum(1 for x in v if x == k)
        if agree != len(v):
            bad += 1
        print(f"        {k:<12} n={len(v):<3} predicted={sorted(set(v))} "
              f"({'OK' if agree == len(v) else 'MISMATCH -- layer will NOT fire'})")
    if bad:
        print(f"[toy] WARNING: {bad} intent group(s) will not reach their layer")


if __name__ == "__main__":
    main()
