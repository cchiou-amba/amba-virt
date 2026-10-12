#!/usr/bin/env python3
# Copyright (C) 2026, Ambarella International LLC.
"""Run one IAV reference application natively (Dom0) and in a guest, and compare the two.

  ref_gate.py --app capture|events|stream|pipeline --board <dom0 alias> --guest <guest alias> --records <file>
              [--frames N] [--binary-dir DIR]

Every raw output row of both runs is printed to stdout tagged with its side ("native" or "guest"), so a gate keeps the
raw evidence. The comparison rows go to --records (one JSON object per line) for the gate's record rules; the exit
code is 0 only when every check of the application held. The applications are the static binaries built by
`make iav-ref`; the same file runs on both sides.

  capture   frames with the same time stamp read natively and in the guest at the same time have the same hash
  events    the guest sees frames at the camera's rate; call and wait costs are recorded for the cost report
  stream    both sides read a valid MJPEG stream (the frame tap must not be running); sizes and rates compare
  pipeline  the guest changes the stream quality and restores it; its step sequence equals the native run's
  decode    the hardware JPEG decoder is driven from the guest (IavRefDecode): the decoded 720p and 1080p frames equal the
            native decode bit for bit and agree with an independent software decode (PIL) within 45 dB PSNR on luma
  overlay   the overlay is blended into the stream from the guest (IavRefOverlay): a frame saved before and one after the
            overlay are decoded here; the box is solid white with a black border after, the rest of the frame is unchanged,
            natively and in the guest; the guest's out-of-window overlays are all refused (--probe bad)
  decode_rate  decode rate and per-pass cost (copy in, decode, read out) natively and in the guest, with and without the
            read out; the passes must all succeed (the numbers go into the cost report)
"""
import argparse
import json
import os
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
APPS = {'capture': 'IavRefCapture', 'events': 'IavRefEvents', 'stream': 'IavRefStream', 'pipeline': 'IavRefPipeline',
        'decode': 'IavRefDecode', 'decode_rate': 'IavRefDecode', 'overlay': 'IavRefOverlay'}


# Extra ssh options per host, for a guest whose port forward is stale after its instance was replaced (set by
# --guest-ssh-opts; the guest is then named user@address and reached through Dom0).
HOST_OPTS = {}


def ssh(host, command, timeout=300):
    p = subprocess.run(['ssh'] + HOST_OPTS.get(host, []) + [host, command], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def scp(src, host, dst):
    subprocess.run(['scp', '-q'] + HOST_OPTS.get(host, []) + [src, '%s:%s' % (host, dst)], check=True)


def stage(host, binary, name, sudo):
    scp(binary, host, '/tmp/%s' % name)
    rc, _, err = ssh(host, 'chmod +x /tmp/%s' % name)
    if rc != 0:
        sys.exit('cannot stage %s on %s: %s' % (name, host, err))


def run_side(side, host, name, args, sudo, results):
    cmd = ('if [ $(id -u) -eq 0 ]; then /tmp/%s %s; else sudo -n /tmp/%s %s; fi' % (name, args, name, args)) if sudo \
        else '/tmp/%s %s' % (name, args)
    rc, out, err = ssh(host, cmd)
    rows = []
    for line in out.splitlines():
        line = line.strip()
        if line.startswith('{'):
            try:
                rows.append(json.loads(line))
            except ValueError:
                pass
    results[side] = dict(rc=rc, rows=rows, out=out, err=err)


def emit_raw(side, res):
    for line in res['out'].splitlines():
        if line.strip():
            print('%s %s' % (side, line))
    for line in res['err'].splitlines():
        if line.strip():
            print('%s stderr: %s' % (side, line))
    print('%s exit=%d' % (side, res['rc']))


def frames(res):
    return [r for r in res['rows'] if r.get('kind') == 'frame']


def summary(res):
    s = [r for r in res['rows'] if r.get('kind') == 'summary']
    return s[-1] if s else {}


def timing(res, what):
    for r in res['rows']:
        if r.get('kind') == 'timing' and r.get('what') == what:
            return r
    return {}


def fetch(host, remote, local):
    subprocess.run(['scp', '-q'] + HOST_OPTS.get(host, []) + ['%s:%s' % (host, remote), local], check=True)


def psnr_y(nv12_path, jpeg_path, w, h):
    import numpy as np
    from PIL import Image
    y = np.fromfile(nv12_path, dtype=np.uint8)[:w * h].reshape(h, w).astype(float)
    ref = np.array(Image.open(jpeg_path).convert('YCbCr'))[:, :, 0].astype(float)
    mse = ((y - ref) ** 2).mean()
    return 99.0 if mse == 0 else float(10 * np.log10(255.0 ** 2 / mse))


def main_decode(a, name, binary):
    import hashlib, tempfile
    jdir = a.jpeg_dir
    if not jdir or not os.path.isdir(jdir):
        sys.exit('--jpeg-dir with t720.jpg and t1080.jpg is required')
    tmp = tempfile.mkdtemp(prefix='iav-decode-')
    ok = True
    records = []
    stage(a.board, binary, name, False)
    stage(a.guest, binary, name, True)
    for jn in ('t720.jpg', 't1080.jpg'):
        scp(os.path.join(jdir, jn), a.board, '/tmp/%s' % jn)
        scp(os.path.join(jdir, jn), a.guest, '/tmp/%s' % jn)
    sides = (('native', a.board, False), ('guest', a.guest, True))
    if a.app == 'decode':
        for jn, w, h in (('t720.jpg', 1280, 720), ('t1080.jpg', 1920, 1080)):
            res = {}
            for side, host, sudo in sides:
                out = '/tmp/dec_%s_%s.nv12' % (side, jn[1:-4])
                run_side(side, host, name, '--jpeg /tmp/%s --repeat 5 --out %s' % (jn, out), sudo, res)
                emit_raw('%s(%s)' % (side, jn), res[side])
                if res[side]['rc'] != 0:
                    print('CHECK %s_%s_exit FAIL exit %d' % (side, jn, res[side]['rc']))
                    ok = False
            fn, fg = os.path.join(tmp, 'native_%s.nv12' % jn), os.path.join(tmp, 'guest_%s.nv12' % jn)
            fetch(a.board, '/tmp/dec_native_%s.nv12' % jn[1:-4], fn)
            fetch(a.guest, '/tmp/dec_guest_%s.nv12' % jn[1:-4], fg)
            equal = open(fn, 'rb').read() == open(fg, 'rb').read() and os.path.getsize(fn) == w * h * 3 // 2
            print('CHECK %s_bit_equal %s guest and native NV12 of %s (%d bytes) are %s' % (
                jn, 'PASS' if equal else 'FAIL', jn, os.path.getsize(fg), 'identical' if equal else 'different'))
            ok = ok and equal
            ps = psnr_y(fg, os.path.join(jdir, jn), w, h)
            good = ps >= 45.0
            print('CHECK %s_vs_software %s luma PSNR %.1f dB against the software decode (limit 45 dB)' % (jn, 'PASS' if good else 'FAIL', ps))
            ok = ok and good
            gr = [r for r in res['guest']['rows'] if r.get('kind') == 'decode']
            nr = [r for r in res['native']['rows'] if r.get('kind') == 'decode']
            for i, (g, nn) in enumerate(zip(gr, nr)):
                records.append(dict(pass_id='%s_%d' % (jn[1:-4], i), seq=len(records), guest_y_hash=g['y_hash'], native_y_hash=nn['y_hash'],
                                    guest_uv_hash=g['uv_hash'], native_uv_hash=nn['uv_hash'], width=g['width'], height=g['height'],
                                    psnr_y=round(ps, 2)))
    else:
        for jn in ('t720.jpg', 't1080.jpg'):
            for readout in (1, 0):
                res = {}
                for side, host, sudo in sides:
                    run_side(side, host, name, '--jpeg /tmp/%s --repeat 200 --readout %d' % (jn, readout), sudo, res)
                    emit_raw('%s(%s,readout=%d)' % (side, jn, readout), res[side])
                    s_ = summary(res[side])
                    good = res[side]['rc'] == 0 and s_.get('passes') == 200 and s_.get('failed') == 0
                    print('CHECK %s_%s_readout%d %s %s' % (side, jn, readout, 'PASS' if good else 'FAIL', json.dumps(s_)))
                    ok = ok and good
                    records.append(dict(seq=len(records), side=side, jpeg=jn, readout=readout, passes=s_.get('passes', 0),
                                        decode_p50_us=s_.get('decode_p50_us', 0), decode_p99_us=s_.get('decode_p99_us', 0),
                                        copy_p50_us=s_.get('copy_p50_us', 0), read_p50_us=s_.get('read_p50_us', 0)))
    with open(a.records, 'w') as f:
        for r in records:
            f.write(json.dumps(r) + '\n')
    print('RESULT %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main_overlay(a, name, binary):
    import tempfile
    import numpy as np
    from PIL import Image
    tmp = tempfile.mkdtemp(prefix='iav-overlay-')
    ok = True
    records = []
    box = dict(x=64, y=64, w=128, h=64)
    stage(a.board, binary, name, False)
    stage(a.guest, binary, name, True)
    for side, host, sudo in (('native', a.board, False), ('guest', a.guest, True)):
        res = {}
        run_side(side, host, name, '--stream 1 --x %d --y %d --w %d --h %d --before /tmp/ov_before.jpg --after /tmp/ov_after.jpg'
                 % (box['x'], box['y'], box['w'], box['h']), sudo, res)
        emit_raw(side, res[side])
        if res[side]['rc'] != 0:
            print('CHECK %s_exit FAIL exit %d' % (side, res[side]['rc']))
            ok = False
            continue
        for tag in ('before', 'after'):
            fetch(host, '/tmp/ov_%s.jpg' % tag, os.path.join(tmp, '%s_%s.jpg' % (side, tag)))
        # the luma of the pictures: the stream's chroma is not what the check is about, the box is white and black
        b = np.array(Image.open(os.path.join(tmp, '%s_before.jpg' % side)).convert('L')).astype(float)
        f = np.array(Image.open(os.path.join(tmp, '%s_after.jpg' % side)).convert('L')).astype(float)
        x, y, w, h = box['x'], box['y'], box['w'], box['h']
        inner = (slice(y + 8, y + h - 8), slice(x + 8, x + w - 8))
        white = float(f[inner].mean())
        scene = float(b[inner].mean())
        edge = float(f[y + 1:y + 3, x + 16:x + w - 16].mean())
        mask = np.ones(f.shape, bool)
        mask[max(y - 16, 0):y + h + 16, max(x - 16, 0):x + w + 16] = False
        outside = float(np.abs(f - b)[mask].mean())
        inside = float(np.abs(f[inner] - b[inner]).mean())
        is_white = white > 215
        is_edge = edge < 40
        quiet = outside < 12.0
        print('CHECK %s_box_white %s mean luma inside the box %.1f after, %.1f before (white is 235)' % (
            side, 'PASS' if is_white else 'FAIL', white, scene))
        print('CHECK %s_box_border %s mean luma of the border %.1f (black is 16)' % (side, 'PASS' if is_edge else 'FAIL', edge))
        print('CHECK %s_outside_unchanged %s mean abs difference outside the box %.2f (limit 12), inside %.1f' % (
            side, 'PASS' if quiet else 'FAIL', outside, inside))
        ok = ok and is_white and is_edge and quiet
        records.append(dict(side=side, box_luma=white, scene_luma=scene, border_luma=edge, outside_diff=outside,
                            inside_diff=inside))
    res = {}
    run_side('guest', a.guest, name, '--stream 1 --probe bad', True, res)
    emit_raw('guest(probe)', res['guest'])
    probes = [r for r in res['guest']['rows'] if r.get('kind') == 'probe']
    refused = all(r['rc'] == -1 and r['errno'] in (1, 22) for r in probes) and len(probes) == 6
    good = res['guest']['rc'] == 0 and refused
    print('CHECK guest_bad_overlays_refused %s %d probes, all refused with EINVAL or EPERM; a good overlay still accepted' % (
        'PASS' if good else 'FAIL', len(probes)))
    ok = ok and good
    with open(a.records, 'w') as fh:
        for r in records:
            fh.write(json.dumps(r) + '\n')
    print('RESULT %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--app', required=True, choices=sorted(APPS))
    ap.add_argument('--jpeg-dir', default=None, help='directory with t720.jpg and t1080.jpg (decode apps)')
    ap.add_argument('--board', required=True)
    ap.add_argument('--guest', required=True)
    ap.add_argument('--records', required=True)
    ap.add_argument('--guest-ssh-opts', default='', help='extra ssh options for the guest, for example "-J <board>"')
    ap.add_argument('--frames', type=int, default=40)
    ap.add_argument('--binary-dir', default=os.path.join(ROOT, 'build', 'iav-ref'))
    a = ap.parse_args()

    if a.guest_ssh_opts:
        HOST_OPTS[a.guest] = a.guest_ssh_opts.split()
    name = APPS[a.app]
    binary = os.path.join(a.binary_dir, name)
    if not os.path.exists(binary):
        sys.exit('missing %s; run make iav-ref' % binary)
    if a.app in ('decode', 'decode_rate'):
        return main_decode(a, name, binary)
    if a.app == 'overlay':
        return main_overlay(a, name, binary)
    stage(a.board, binary, name, False)
    stage(a.guest, binary, name, True)
    args = {'capture': '--frames %d' % a.frames, 'events': '--events 60 --waits 300 --calls 1000 --verbose 1',
            'stream': '--stream 1 --frames %d' % a.frames, 'pipeline': '--stream 1'}[a.app]

    res = {}
    ok = True
    records = []
    if a.app == 'capture':
        # both at once: canvas descriptors have no single-user limit, so the native copy is the witness
        t = [threading.Thread(target=run_side, args=('native', a.board, name, args, False, res)),
             threading.Thread(target=run_side, args=('guest', a.guest, name, args, True, res))]
        for x in t:
            x.start()
        for x in t:
            x.join()
    else:
        run_side('native', a.board, name, args, False, res)
        run_side('guest', a.guest, name, args, True, res)
    for side in ('native', 'guest'):
        emit_raw(side, res[side])
        if res[side]['rc'] != 0:
            print('CHECK %s_exit FAIL exit %d' % (side, res[side]['rc']))
            ok = False

    if a.app == 'capture':
        nat = {f['mono_pts']: f['hash'] for f in frames(res['native'])}
        n = 0
        for f in frames(res['guest']):
            if f['mono_pts'] in nat:
                records.append(dict(frame_seq=n, mono_pts=f['mono_pts'], guest_hash=f['hash'], native_hash=nat[f['mono_pts']]))
                n += 1
        print('CHECK common_frames %s %d frames with the same time stamp on both sides'
              % ('PASS' if n >= 8 else 'FAIL', n))
        ok = ok and n >= 8
        bad = [r for r in records if r['guest_hash'] != r['native_hash']]
        print('CHECK hashes_equal %s %d of %d differ' % ('PASS' if not bad else 'FAIL', len(bad), len(records)))
        ok = ok and not bad
        sg = summary(res['guest'])
        print('CHECK guest_content %s distinct_content=%s' % ('PASS' if sg.get('distinct_content') == 2 else 'FAIL',
                                                              sg.get('distinct_content')))
        ok = ok and sg.get('distinct_content') == 2
    elif a.app == 'events':
        rg = [r for r in res['guest']['rows'] if r.get('kind') == 'rate']
        rn = [r for r in res['native']['rows'] if r.get('kind') == 'rate']
        if not rg or not rn:
            print('CHECK rate FAIL no rate rows')
            ok = False
        else:
            g, nn = rg[0]['events_per_s'], rn[0]['events_per_s']
            close = abs(g - nn) <= 0.05 * nn
            print('CHECK rate %s guest %.3f native %.3f events per second' % ('PASS' if close else 'FAIL', g, nn))
            ok = ok and close
            row = dict(sample=0, guest_events_per_s=g, native_events_per_s=nn)
            for what in ('get_iav_state_call', 'wait_next_idsp_frame', 'poll_ready', 'event_gap'):
                for side in ('guest', 'native'):
                    t_ = timing(res[side], what)
                    for k in ('p50_us', 'p99_us', 'min_us', 'max_us'):
                        if k in t_:
                            row['%s_%s_%s' % (side, what, k)] = t_[k]
            records.append(row)
        # B4 (finding F13): after a poll phase, no wait for the next frame may exceed twice the frame interval plus
        # one poll slice (2 x 33.4 ms + 20 ms, rounded to 90 ms), on either side.
        for side in ('guest', 'native'):
            w = [r['us'] / 1000.0 for r in res[side]['rows'] if r.get('kind') == 'wait']
            worst = max(w) if w else 1e9
            good = len(w) >= 300 and worst <= 90.0
            print('CHECK %s_wait_tail %s %d waits, worst %.1f ms (limit 90 ms)' % (side, 'PASS' if good else 'FAIL', len(w), worst))
            ok = ok and good
    elif a.app == 'stream':
        sg, sn = summary(res['guest']), summary(res['native'])
        for side, s in (('guest', sg), ('native', sn)):
            good = s.get('frames') == a.frames and s.get('bad_order') == 0 and s.get('bad_jpeg') == 0 and s.get('outside') == 0
            print('CHECK %s_stream %s %s' % (side, 'PASS' if good else 'FAIL', json.dumps(s)))
            ok = ok and good
        for i, (g, nn) in enumerate(zip(frames(res['guest']), frames(res['native']))):
            records.append(dict(frame_seq=i, guest_width=g['width'], native_width=nn['width'], guest_height=g['height'],
                                native_height=nn['height'], guest_jpeg_ok=g['jpeg_ok'], native_jpeg_ok=nn['jpeg_ok']))
        fps_ok = sg.get('pts_fps', 0) > 0 and abs(sg.get('pts_fps', 0) - sn.get('pts_fps', 0)) <= 0.05 * max(sn.get('pts_fps', 1), 1)
        print('CHECK pts_fps %s guest %.2f native %.2f frames per second from the camera time stamps' % ('PASS' if fps_ok else 'FAIL', sg.get('pts_fps', 0), sn.get('pts_fps', 0)))
        ok = ok and fps_ok
    elif a.app == 'pipeline':
        def steps(r):
            return [(s['name'], s['rc'], s['state']) for s in r['rows'] if s.get('kind') == 'step']
        sg, sn = steps(res['guest']), steps(res['native'])
        same = sg == sn and len(sg) > 0
        print('CHECK step_sequence %s guest %d steps, native %d steps' % ('PASS' if same else 'FAIL', len(sg), len(sn)))
        ok = ok and same
        for i, s in enumerate(sg):
            records.append(dict(step=i, name=s[0], guest_rc=s[1], guest_state=s[2],
                                native_state=sn[i][2] if i < len(sn) else -99))

    with open(a.records, 'w') as f:
        for r in records:
            f.write(json.dumps(r) + '\n')
    print('RESULT %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
