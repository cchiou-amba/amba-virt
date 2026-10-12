#!/usr/bin/env python3
# Copyright (C) 2026, Ambarella International LLC
"""Negative controls for the support table check.

Each mutant is a broken copy of one input (the document, the review record, or the broker's dump). The check
(gen_iav_support_table.py --check) must fail on every one of them; a mutant that passes is reported as "survived"
and the script exits 1. The real inputs are never modified.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, 'gen_iav_support_table.py')


def run_check(dump, review, doc):
    p = subprocess.run([sys.executable, GEN, '--dump', dump, '--review', review, '--doc', doc, '--check'],
                       capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dump', required=True)
    ap.add_argument('--review', required=True)
    ap.add_argument('--doc', required=True)
    a = ap.parse_args()

    rc, out = run_check(a.dump, a.review, a.doc)
    if rc != 0:
        print('the unmodified inputs do not pass the check: %s' % out)
        return 1
    print('baseline: pass')

    tmp = tempfile.mkdtemp(prefix='iav-support-negctl-')
    survived = 0
    try:
        dump_lines = open(a.dump).read().split('\n')
        review_lines = open(a.review).read().split('\n')
        doc = open(a.doc).read()

        def served_row(lines):
            return next(i for i, l in enumerate(lines) if l.startswith('IAV_IOC_GET_IAV_STATE\t'))

        def mutants():
            # 1. the document says a refused call is served
            i = doc.index('| `IAV_IOC_RESET` |')
            j = doc.index('\n', i)
            yield 'doc_wrong_support', None, None, doc[:i] + doc[i:j].replace('| refused |', '| every guest |') + doc[j:]
            # 2. the document is missing a row
            yield 'doc_row_deleted', None, None, doc[:i] + doc[j + 1:]
            # 3. the broker serves a call the document says is refused
            d = list(dump_lines)
            k = next(n for n, l in enumerate(d) if l.startswith('IAV_IOC_RESET\t'))
            f = d[k].split('\t'); f[7] = '0x7'; d[k] = '\t'.join(f)
            yield 'broker_serves_more', '\n'.join(d), None, None
            # 4. the broker refuses a call the document says is served
            d = list(dump_lines)
            k = served_row(d)
            f = d[k].split('\t'); f[7] = '0x0'; d[k] = '\t'.join(f)
            yield 'broker_serves_less', '\n'.join(d), None, None
            # 5. a refused call loses its reason
            r = list(review_lines)
            k = next(n for n, l in enumerate(r) if l.startswith('IAV_IOC_RESET\t'))
            f = r[k].split('\t'); f[2] = ''; r[k] = '\t'.join(f)
            yield 'reason_missing', None, '\n'.join(r), None
            # 6. a served call keeps a stale reason
            r = list(review_lines)
            k = served_row(r)
            f = r[k].split('\t'); f[2] = 'stale reason'; r[k] = '\t'.join(f)
            yield 'stale_reason', None, '\n'.join(r), None
            # 7. the review record has an ioctl the broker does not
            yield 'review_extra_ioctl', None, '\n'.join(review_lines + ['IAV_IOC_NO_SUCH_CALL\tx\ty']), None
            # 8. the review record lacks an ioctl the broker has
            r = [l for l in review_lines if not l.startswith('IAV_IOC_RESET\t')]
            yield 'review_missing_ioctl', None, '\n'.join(r), None
            # 9. a description is empty
            r = list(review_lines)
            k = next(n for n, l in enumerate(r) if l.startswith('IAV_IOC_RESET\t'))
            f = r[k].split('\t'); f[1] = ''; r[k] = '\t'.join(f)
            yield 'description_empty', None, '\n'.join(r), None

        for n, (name, d_txt, r_txt, doc_txt) in enumerate(mutants()):
            dump_p, review_p, doc_p = a.dump, a.review, a.doc
            if d_txt is not None:
                dump_p = os.path.join(tmp, 'dump%d' % n); open(dump_p, 'w').write(d_txt)
            if r_txt is not None:
                review_p = os.path.join(tmp, 'review%d' % n); open(review_p, 'w').write(r_txt)
            if doc_txt is not None:
                doc_p = os.path.join(tmp, 'doc%d' % n); open(doc_p, 'w').write(doc_txt)
            rc, out = run_check(dump_p, review_p, doc_p)
            if rc == 0:
                print('mutant %-22s survived (the check passed)' % name)
                survived += 1
            else:
                print('mutant %-22s caught: %s' % (name, out.split('\n')[0][:100]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if survived:
        print('%d mutant(s) survived' % survived)
        return 1
    print('all mutants caught')
    return 0


if __name__ == '__main__':
    sys.exit(main())
