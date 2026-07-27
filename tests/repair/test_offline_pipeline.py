#!/usr/bin/env python3
"""Focused regressions for bugs that translator-only tests cannot catch."""

import json
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path.cwd()
FAULT = ROOT.parent / "Fault_yield"
HEADER = "BankFail HBMID ChannelID LayerID BankID RowID ColID RowLen ColLen\n"


def build(src, out):
    subprocess.run(
        ["g++", "-O2", "-std=c++17", "-o", str(out), str(FAULT / src)],
        check=True,
    )


def run(binary, args, text):
    return subprocess.run(
        [str(binary), *args], input=text, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )


def faults(groups):
    lines = [HEADER]
    for row, cols in groups.items():
        for col in cols:
            lines.append(f"0 0 0 0 0 {row} {col} 1 1\n")
    return "".join(lines)


def main():
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        repair = td / "repairv2"
        trad = td / "repair_trad"
        micro = td / "micro_fault_gen"
        build("repair_algo_final_v2.cpp", repair)
        build("repair_algo_trad.cpp", trad)
        build("my_fault_gen/fault_generator.cpp", micro)

        # Layer C must choose the subset that covers the most faulty transaction
        # units. Here rows 0,1,3 exactly fill its 32 registers/slots, leaving the
        # ten isolated transactions of row 2 for transaction-granular Layer A.
        groups = {
            100: range(32), 101: range(32),
            0: range(0, 30, 2), 1: range(0, 24, 2),
            2: range(0, 20, 2), 3: range(0, 10, 2),
        }
        json_dir = td / "json"
        p = run(repair, [
            "--input", "-", "--rs", "4", "--sram_slots", "10",
            "--vacuum_limit", "4096", "--num_channels", "1",
            "--num_banks", "8", "--bursts", "32", "--num_dies", "2",
            "--dump-json", "--json-dir", str(json_dir),
        ], faults(groups))
        assert p.returncode == 0, p.stderr
        assert "Total dies        : 2" in p.stdout
        table = json.loads((json_dir / "remap_hbm_0.json").read_text())
        assert table["config"]["layer_a_granularity"] == "transaction"
        assert table["config"]["transaction_bytes"] == 32
        assert len(table["layer_a_sram"]) == 10
        assert sum(e["length"] for e in table["layer_a_sram"]) == 10
        assert {e["row"] for e in table["layer_a_sram"]} == {2}
        assert {e["target_slot"] for e in table["layer_a_sram"]} == set(range(10))
        assert not table["layer_d_bad_banks"]
        a_rows = {(e["ch"], e["pch"], e["bg"], e["ba"], e["row"])
                  for e in table["layer_a_sram"]}
        c_rows = {(b["ch"], b["pch"], b["bg"], b["ba"], e["row"])
                  for b in table["banks"] for e in b["layer_c_burst"]}
        assert not (a_rows & c_rows)
        assert table["K"] == 0 and table["config"]["vacuum_limit"] == 4096

        # Counterexample for the old two-order greedy baseline. Exact cover:
        # rows {0,4}, columns {2,3}.
        graph = {0: [0, 1], 1: [2, 3], 2: [2, 3], 3: [2, 3], 4: [0]}
        p = run(trad, [
            "--input", "-", "--sr", "2", "--sc", "2", "--cols", "32",
            "--num_channels", "1", "--num_pch", "1", "--num_bg", "1",
            "--num_ba", "1", "--pass-only", "--output", str(td / "trad.txt"),
        ], faults(graph))
        assert p.returncode == 0, p.stderr
        assert "PASS  : 1" in p.stdout and "Yield : 100.00%" in p.stdout

        # Truncated/malformed records must fail rather than silently ending input.
        p = run(repair, ["--input", "-"], HEADER + "0 0 0 0 0 1 2 1\n")
        assert p.returncode != 0 and "exactly nine" in p.stderr

        # Generator defaults must obey the 32-slot repair granularity.
        out = td / "micro.txt"
        p = subprocess.run([
            str(micro), "--hbm", "1", "--stack", "1", "--channels", "1",
            "--banks", "1", "--ly_slots", "1", "--rows", "1",
            "--fixed", "32", "--output_file", str(out),
        ], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert p.returncode == 0, p.stderr
        records = [list(map(int, line.split())) for line in out.read_text().splitlines()[1:]]
        assert len(records) == 32 and {r[-1] for r in records} == set(range(32))

    print("offline pipeline regressions: PASS")


if __name__ == "__main__":
    main()
