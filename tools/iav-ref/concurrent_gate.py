#!/usr/bin/env python3
# Copyright (C) 2026, Ambarella International LLC.
"""Two guests at once, and a server restart under load (Phase E, task E2).

  concurrent_gate.py --board <dom0 alias> --primary <guest> --consumer <guest> --jpeg-dir DIR
                     [--primary-ssh-opts "..."] [--dev-server tools/iav-ref/dev_server.sh]

Part 1: the two guests work at once, within what the driver allows. The driver serves the frame descriptors of the
encoder streams to one reader at a time (the second reader is refused with EFAULT, on the bare board too), and a primary
that stops a stream ends it for the reader of that stream (a null end-of-stream descriptor). So:
  1a  the consumer reads 300 frames of stream 1 (IavRefStream) while the primary decodes JPEGs with the hardware decoder
      (IavRefDecode): both succeed and the consumer sees every frame intact;
  1b  the consumer reads 300 frames from the canvas (IavRefCapture, which has no single-reader limit) while the primary
      draws an overlay (IavRefOverlay) and changes and restores the stream quality (IavRefPipeline): all succeed;
  1c  the consumer reads 200 frames of stream 1 while the primary tries to read the same stream (IavRefOverlay): the
      primary is refused (exit 1, "Bad address") and the consumer is not disturbed (all frames, intact).

Part 2: the consumer starts a longer read and, five seconds in, the candidate server on the board is restarted
(dev_server.sh stop, then start). The consumer's read must end (a hung call is a failure) and no guest may be left
unreachable. Then both guests run again: the consumer reads 60 frames and the primary draws an overlay and decodes,
proving that a restarted server serves them as before.

Raw output of every run is printed tagged with its role. A line "CHECK <name> PASS|FAIL <detail>" is written for every
check; "RESULT PASS" is printed and the exit code is 0 only when every check held.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ref_gate as rg  # noqa: E402


def summary(res):
    return rg.summary(res)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--board', required=True)
    ap.add_argument('--primary', required=True)
    ap.add_argument('--consumer', required=True)
    ap.add_argument('--jpeg-dir', required=True)
    ap.add_argument('--primary-ssh-opts', default='')
    ap.add_argument('--consumer-ssh-opts', default='')
    ap.add_argument('--dev-server', default=os.path.join(HERE, 'dev_server.sh'))
    ap.add_argument('--camera-conf', default='/tmp/stage/camera.conf.duplex')
    ap.add_argument('--binary-dir', default=os.path.join(rg.ROOT, 'build', 'iav-ref'))
    a = ap.parse_args()
    if a.primary_ssh_opts:
        rg.HOST_OPTS[a.primary] = a.primary_ssh_opts.split()
    if a.consumer_ssh_opts:
        rg.HOST_OPTS[a.consumer] = a.consumer_ssh_opts.split()

    ok = True

    def check(name, good, detail):
        nonlocal ok
        print('CHECK %s %s %s' % (name, 'PASS' if good else 'FAIL', detail))
        ok = ok and good

    apps = ('IavRefStream', 'IavRefCapture', 'IavRefOverlay', 'IavRefDecode', 'IavRefPipeline')
    for n in apps:
        rg.stage(a.primary, os.path.join(a.binary_dir, n), n, True)
        rg.stage(a.consumer, os.path.join(a.binary_dir, n), n, True)
    for jn in ('t720.jpg', 't1080.jpg'):
        rg.scp(os.path.join(a.jpeg_dir, jn), a.primary, '/tmp/%s' % jn)

    # ---- part 1: both guests at once
    def together(label, cons_name, cons_args, prim_runs):
        res = {}

        def prim():
            for role, name, args in prim_runs:
                rg.run_side(role, a.primary, name, args, True, res)

        t_c = threading.Thread(target=rg.run_side, args=('consumer_' + label, a.consumer, cons_name, cons_args, True, res))
        t_p = threading.Thread(target=prim)
        t0 = time.time()
        t_c.start()
        t_p.start()
        t_p.join()
        t_c.join()
        print('part %s took %.1f s' % (label, time.time() - t0))
        for role in res:
            rg.emit_raw(role, res[role])
        return res

    res = together('1a', 'IavRefStream', '--stream 1 --frames 300', [('decode_1a', 'IavRefDecode', '--jpeg /tmp/t720.jpg --repeat 300')])
    s = summary(res['consumer_1a'])
    check('1a_consumer_reads_every_frame', res['consumer_1a']['rc'] == 0 and s.get('frames') == 300 and s.get('bad_order') == 0 and
          s.get('bad_jpeg') == 0 and s.get('outside') == 0, json.dumps(s))
    check('1a_primary_decodes', res['decode_1a']['rc'] == 0, 'exit %d' % res['decode_1a']['rc'])
    dec = [r for r in res['decode_1a']['rows'] if r.get('kind') == 'summary']
    check('1a_decode_passes', bool(dec) and dec[-1].get('passes') == 300 and dec[-1].get('failed') == 0, json.dumps(dec[-1] if dec else {}))

    res = together('1b', 'IavRefCapture', '--frames 300', [('overlay_1b', 'IavRefOverlay', '--stream 1 --before /tmp/cc_b.jpg --after /tmp/cc_a.jpg'),
                                                           ('pipeline_1b', 'IavRefPipeline', '--stream 1')])
    s = summary(res['consumer_1b'])
    check('1b_consumer_reads_every_frame', res['consumer_1b']['rc'] == 0 and s.get('frames') == 300 and s.get('distinct_content') == 2, json.dumps(s))
    for role in ('overlay_1b', 'pipeline_1b'):
        check('%s_succeeds' % role, res[role]['rc'] == 0, 'exit %d' % res[role]['rc'])

    res = together('1c', 'IavRefStream', '--stream 1 --frames 200', [('overlay_1c', 'IavRefOverlay', '--stream 1 --before /tmp/cc_b.jpg --after /tmp/cc_a.jpg')])
    s = summary(res['consumer_1c'])
    check('1c_consumer_undisturbed', res['consumer_1c']['rc'] == 0 and s.get('frames') == 200 and s.get('bad_order') == 0 and
          s.get('bad_jpeg') == 0 and s.get('outside') == 0, json.dumps(s))
    check('1c_second_reader_refused', res['overlay_1c']['rc'] == 1 and 'Bad address' in res['overlay_1c']['err'],
          'exit %d, %s' % (res['overlay_1c']['rc'], res['overlay_1c']['err'].strip().replace('\n', ' ')[:80]))

    # ---- part 2: restart the server under a running consumer
    res2 = {}
    t_cons = threading.Thread(target=rg.run_side, args=('consumer_cut', a.consumer, 'IavRefStream', '--stream 1 --frames 900', True, res2))
    t_cons.start()
    time.sleep(5)
    print('restarting the server')
    p1 = subprocess.run([a.dev_server, a.board, 'stop'], capture_output=True, text=True)
    p2 = subprocess.run([a.dev_server, a.board, 'start', '0', a.camera_conf], capture_output=True, text=True)
    print(p1.stdout + p1.stderr + p2.stdout + p2.stderr)
    t_cons.join(timeout=180)
    check('cut_consumer_ends', not t_cons.is_alive(), 'the read ended after the server restart' if not t_cons.is_alive()
          else 'the read is still running 180 s after the restart')
    if 'consumer_cut' in res2:
        rg.emit_raw('consumer_cut', res2['consumer_cut'])
        check('cut_consumer_fails_cleanly', res2['consumer_cut']['rc'] != 0,
              'exit %d (the read was cut off, not completed)' % res2['consumer_cut']['rc'])
    check('server_restarted', p2.returncode == 0 and 'candidate server is serving' in p2.stdout, p2.stdout.strip().splitlines()[-1] if p2.stdout.strip() else 'no output')
    for who, host in (('primary', a.primary), ('consumer', a.consumer)):
        rc, out, err = rg.ssh(host, 'echo alive; uname -r', timeout=60)
        check('%s_guest_alive' % who, rc == 0 and 'alive' in out, out.strip().replace('\n', ' '))

    # ---- after the restart, both are served as before
    res3 = {}
    rg.run_side('consumer_after', a.consumer, 'IavRefStream', '--stream 1 --frames 60', True, res3)
    rg.run_side('overlay_after', a.primary, 'IavRefOverlay', '--stream 1 --before /tmp/cc_b.jpg --after /tmp/cc_a.jpg', True, res3)
    rg.run_side('decode_after', a.primary, 'IavRefDecode', '--jpeg /tmp/t720.jpg --repeat 5', True, res3)
    for role in ('consumer_after', 'overlay_after', 'decode_after'):
        rg.emit_raw(role, res3[role])
    s = summary(res3['consumer_after'])
    check('after_restart_consumer', res3['consumer_after']['rc'] == 0 and s.get('frames') == 60 and s.get('bad_order') == 0 and s.get('bad_jpeg') == 0,
          json.dumps(s))
    check('after_restart_overlay', res3['overlay_after']['rc'] == 0, 'exit %d' % res3['overlay_after']['rc'])
    check('after_restart_decode', res3['decode_after']['rc'] == 0, 'exit %d' % res3['decode_after']['rc'])
    print('RESULT %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
