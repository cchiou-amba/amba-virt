#!/usr/bin/env python3
#
# Copyright (C) 2026, Ambarella International LLC.
#
# Check a results file of the IAV qualification suite against a stated expectation: at least
# --min-pass tests passed, and every test that did not pass is named with --allow-fail. Used where
# a documented limitation makes named tests fail, so the gate states which ones and no others.
#
#   check_suite_results.py --results <file.jsonl> --min-pass N [--allow-fail Test1,Test2]
#
# Exit 0 when the expectation holds, 1 when it does not, 2 on an input error.

import argparse
import json
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--min-pass", type=int, required=True)
    ap.add_argument("--allow-fail", default="")
    a = ap.parse_args()
    allowed = set(x for x in a.allow_fail.split(",") if x)
    passed, failed = [], []
    with open(a.results) as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if "test" not in r:
                continue
            (passed if r.get("status") == "pass" else failed).append(r["test"])
    if not passed and not failed:
        print("check_suite_results: no test rows in %s" % a.results, file=sys.stderr)
        return 2
    unexpected = [t for t in failed if t not in allowed]
    print("passed=%d failed=%d (%s) unexpected_failures=%s" % (len(passed), len(failed),
          ",".join(failed) or "none", ",".join(unexpected) or "none"))
    ok = len(passed) >= a.min_pass and not unexpected
    print("check_suite_results: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
