#!/usr/bin/env python3
#
# Copyright (C) 2026, Ambarella International LLC.
#
# Compare the IAV qualification results of a native run (Dom0, real driver) with a guest run (the
# same suite through the virtual /dev/iav), from the recorded results files only.
#
#   compare_results.py --native-tests N.jsonl --guest-tests G.jsonl \
#       --native-bench N1.jsonl [N2.jsonl ...] --guest-bench G1.jsonl [G2.jsonl ...] \
#       [--expect-diff TestName:reason ...] [--title TEXT] [--out report.md]
#
# Correctness: every test that has a different status natively and in the guest is listed. A
# difference not named with --expect-diff makes the exit code 1. Performance: for each benchmark and
# workload, the median, 95th percentile, sample count and run count of each side and the ratio of the
# medians (guest over native), with the spread of the per-run medians when several runs are given.
# The tool states nothing about stability; it reports the samples it was given.

import argparse
import json
import statistics
import sys
from collections import OrderedDict, defaultdict


def rows(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    out.append(json.loads(line))
                except ValueError:
                    pass
    return out


def pct(vals, p):
    s = sorted(vals)
    if not s:
        return float("nan")
    k = min(len(s) - 1, max(0, int(round(p / 100.0 * (len(s) - 1)))))
    return s[k]


def bench_stats(paths):
    """{(bench, workload): [samples per run]} from one or more results files."""
    runs = defaultdict(list)
    unit = {}
    for p in paths:
        per = defaultdict(list)
        for r in rows(p):
            if "bench" in r:
                per[(r["bench"], r["workload"])].append(r["value"])
                unit[(r["bench"], r["workload"])] = r.get("unit", "")
        for k, v in per.items():
            runs[k].append(v)
    return runs, unit


def fmt(v, unit):
    if v != v:
        return "-"
    if unit == "ns":
        if v >= 1e6:
            return "%.2f ms" % (v / 1e6)
        if v >= 1e3:
            return "%.1f us" % (v / 1e3)
        return "%d ns" % v
    return "%g %s" % (v, unit)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--native-tests", required=True)
    ap.add_argument("--guest-tests", required=True)
    ap.add_argument("--native-bench", nargs="+", required=True)
    ap.add_argument("--guest-bench", nargs="+", required=True)
    ap.add_argument("--expect-diff", action="append", default=[])
    ap.add_argument("--title", default="IAV native versus guest")
    ap.add_argument("--out")
    a = ap.parse_args()

    expected = dict(x.split(":", 1) for x in a.expect_diff)
    nt = OrderedDict((r["test"], r) for r in rows(a.native_tests) if "test" in r)
    gt = OrderedDict((r["test"], r) for r in rows(a.guest_tests) if "test" in r)
    names = list(nt.keys()) + [k for k in gt.keys() if k not in nt]

    lines = ["# %s" % a.title, ""]
    lines.append("Generated from the recorded results files only: `%s`, `%s`, and the benchmark files "
                 "named below." % (a.native_tests, a.guest_tests))
    lines.append("")
    lines.append("## Correctness parity")
    lines.append("")
    lines.append("| Test | Native | Guest | Result |")
    lines.append("|---|---|---|---|")
    unexpected = 0
    diffs = 0
    for n in names:
        ns = nt[n]["status"] if n in nt else "absent"
        gs = gt[n]["status"] if n in gt else "absent"
        if ns == gs:
            res = "same"
        else:
            diffs += 1
            if n in expected:
                res = "different (expected: %s)" % expected[n]
            else:
                res = "**DIFFERENT**"
                unexpected += 1
        lines.append("| %s | %s | %s | %s |" % (n, ns, gs, res))
    lines.append("")
    lines.append("Tests: native %d, guest %d. Differences: %d (%d not expected)."
                 % (len(nt), len(gt), diffs, unexpected))
    lines.append("")

    nr, nu = bench_stats(a.native_bench)
    gr, gu = bench_stats(a.guest_bench)
    lines.append("## Performance")
    lines.append("")
    lines.append("Native files: %s. Guest files: %s." % (", ".join("`%s`" % p for p in a.native_bench),
                                                       ", ".join("`%s`" % p for p in a.guest_bench)))
    lines.append("")
    lines.append("| Benchmark | Workload | Native median | Native p95 | Guest median | Guest p95 | Ratio | "
                 "Samples (native/guest) | Runs (native/guest) | Spread of run medians (native/guest) |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|")
    for k in sorted(set(nr) | set(gr)):
        n_all = [x for run in nr.get(k, []) for x in run]
        g_all = [x for run in gr.get(k, []) for x in run]
        unit = nu.get(k) or gu.get(k) or ""
        nm = statistics.median(n_all) if n_all else float("nan")
        gm = statistics.median(g_all) if g_all else float("nan")
        ratio = (gm / nm) if n_all and g_all and nm else float("nan")

        def spread(runs):
            if len(runs) < 2:
                return "n/a"
            ms = [statistics.median(r) for r in runs]
            return "%s to %s" % (fmt(min(ms), unit), fmt(max(ms), unit))

        lines.append("| %s | %s | %s | %s | %s | %s | %s | %d/%d | %d/%d | %s / %s |" % (
            k[0], k[1], fmt(nm, unit), fmt(pct(n_all, 95), unit), fmt(gm, unit), fmt(pct(g_all, 95), unit),
            ("%.1fx" % ratio) if ratio == ratio else "-", len(n_all), len(g_all),
            len(nr.get(k, [])), len(gr.get(k, [])), spread(nr.get(k, [])), spread(gr.get(k, []))))
    lines.append("")
    lines.append("Ratio is guest over native, from the medians. The tool reports the samples it was given and "
                 "makes no claim about stability beyond the spread shown.")
    text = "\n".join(lines) + "\n"
    if a.out:
        with open(a.out, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 1 if unexpected else 0


if __name__ == "__main__":
    sys.exit(main())
