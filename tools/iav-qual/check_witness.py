#!/usr/bin/env python3
#
# Copyright (C) 2026, Ambarella International LLC.
#
# Check that frames a guest read are the frames the DSP wrote.
#
# The guest suite (group IavWitness) records, for each frame it read, the frame timestamp
# (pts_<i>) and a hash of its Y plane (hash_<i>). The Dom0 witness logs "witness: pts=<n>
# hash=<h>" for every frame it saw through its own mapping. For each guest frame whose timestamp the
# witness also logged, the two hashes must be equal; the gate requires a minimum number of such
# frames, a minimum share of them equal, and more than one distinct hash among the equal ones (so a
# window that shows one constant frame fails).
#
#   check_witness.py --guest <results.jsonl> --witness <serve.log> [--min-match N] [--min-share P]
#
# Exit 0 when the check holds, 1 when it does not, 2 on a usage or input error.

import argparse
import json
import re
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--guest", required=True)
    ap.add_argument("--witness", required=True)
    ap.add_argument("--min-match", type=int, default=5)
    ap.add_argument("--min-share", type=float, default=0.8)
    a = ap.parse_args()

    witness = {}
    with open(a.witness, errors="replace") as f:
        for line in f:
            m = re.search(r"witness: pts=(\d+) hash=(-?\d+)", line)
            if m:
                witness[int(m.group(1))] = int(m.group(2))
    obs = None
    with open(a.guest) as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if r.get("group") == "IavWitness":
                obs = r.get("obs", {})
    if obs is None:
        print("check_witness: no IavWitness row in %s" % a.guest, file=sys.stderr)
        return 2

    seen = equal = 0
    hashes = set()
    for i in range(64):
        pts, h = obs.get("pts_%d" % i), obs.get("hash_%d" % i)
        if pts is None or h is None:
            continue
        if pts in witness:
            seen += 1
            if witness[pts] == h:
                equal += 1
                hashes.add(h)
            print("frame %d pts=%d guest=%d witness=%d %s" % (i, pts, h, witness[pts],
                  "equal" if witness[pts] == h else "DIFFERENT"))
        else:
            print("frame %d pts=%d not logged by the witness" % (i, pts))
    share = (equal / seen) if seen else 0.0
    print("witness_frames=%d guest_frames_seen=%d equal=%d share=%.2f distinct_equal_hashes=%d"
          % (len(witness), seen, equal, share, len(hashes)))
    ok = equal >= a.min_match and share >= a.min_share and len(hashes) >= 2
    print("check_witness: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
