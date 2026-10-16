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
  scale     a decoded 720p and 1080p JPEG is scaled by the DSP image scaler into a 640x640 buffer (window 640x360) in the
            decode partition (IavRefScale), blocking and with WAIT_IMG_SCALE: the guest's scaled picture equals the native
            one bit for bit, the pad around the window is untouched on both sides, and the scaled luma agrees with a
            software resize of the JPEG (PIL, box filter) within 30 dB PSNR (a sanity bound: the same scene, not garbage)
  scale_probe  the guest's hostile scale, GDMA and cache calls are all refused with the expected errno and a good scale
            still succeeds (IavRefScale --probe bad; not run natively, where the calls would reach the driver)
  image     one well-formed call of each image ioctl of inventory class W (IavRefImage), natively, in the guest, then
            natively again: return codes and errnos equal, and every output that did not change between the two
            native runs equals the guest's; the GDMA copy is exact on both sides; the guest sees no physical address
  arena     the Cooper allocation pattern (IavRefArena): ALLOC_ANON_MEM_PART, mmap at the returned address, GDMA_COPY of
            a decoded 720p and 1080p JPEG into the block and SET_IMG_SCALE from that block into another, blocking and
            with WAIT_IMG_SCALE, then FREE_MEM_PART: the same binary natively and in the guest; the scaled pictures are
            bit-exact between the two sides, the GDMA copies exact and the pads untouched on both
  arena_life  the bare-metal rules of anonymous partitions (IavRefArena --probe life): every case gives the same
            return code and errno natively and in the guest
  arena_probe the guest's hostile arena calls (IavRefArena --probe bad, guest only) are refused with the expected errno
            and a good call still succeeds afterwards

  The aperture applications (Envelope F). --sides native|guest|both (default both) runs one side only, for a guest
  whose image has no aperture yet or to check the server's partitions natively. Picture checks measure canvas 0 in the
  DSP partition (mask, blur, arb_blur) or the Dom0 frame tap, the 1920x1080 stream the primary guest's demo serves at
  /live/stream (canvas_overlay, yuv_remap: their effect is in the encoded stream, not the canvas), via --tap-host
  (default --guest).
  aperture_map  every aperture partition natively and in the guest (IavRefAperture --map): present with the same length,
            mapped, a word written at each end reads back, a map past twice the partition refused
  aperture_misc IMG_GET_MISC_INFO without and with the extra-raw query (IavRefAperture --misc): return codes, errnos and
            the valid flags written equal natively and in the guest, the caller's pointers kept
  aperture_probe the primary guest's hostile aperture calls (IavRefAperture --probe bad, guest only) are refused with the
            expected errno and a good call still succeeds
  aperture_consumer a consumer guest (--guest) gets EPERM for every aperture call and maps no aperture partition
            (IavRefAperture --probe consumer, guest only)
  mask      a privacy mask on canvas 0 (IavRefAperture --mask): calls succeed, the area turns uniform (variance <= 50)
            and its mean moves by at least 30 while on, and returns within 15 of the mean before after
  blur      a normal privacy blur of canvas 0 (IavRefBlur): variance of the area at most 0.9 of before while on, back to
            at least 0.9 of before after
  arb_blur  an arbitrary blur with the bitmap in IAV_PART_ARB_BLUR (IavRefBlur --arb, left-half bits set): the half
            with clear bits (right) at most 0.9 of its variance before, the other half at least 0.95
  canvas_overlay a canvas overlay box (IavRefAperture --canvas-overlay --hold-ms): in the tap the box area is uniform
            (variance <= 4) while held, and textured (variance >= 50) before and after
  yuv_remap a YUV remap of stream 1 to luma 16 (IavRefYuvRemap --hold-ms): in the tap the area is uniform (variance
            <= 4) with a mean within 4 of 16 while held, and textured (variance >= 50) before and after
  warp      the warp calls (IavRefWarp): every call's return code and errno equal natively and in the guest
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
        'decode': 'IavRefDecode', 'decode_rate': 'IavRefDecode', 'overlay': 'IavRefOverlay', 'scale': 'IavRefScale',
        'scale_probe': 'IavRefScale', 'image': 'IavRefImage', 'arena': 'IavRefArena', 'arena_life': 'IavRefArena',
        'arena_probe': 'IavRefArena', 'aperture_map': 'IavRefAperture', 'aperture_misc': 'IavRefAperture',
        'aperture_probe': 'IavRefAperture', 'aperture_consumer': 'IavRefAperture', 'mask': 'IavRefAperture',
        'canvas_overlay': 'IavRefAperture', 'blur': 'IavRefBlur', 'arb_blur': 'IavRefBlur', 'yuv_remap': 'IavRefYuvRemap',
        'warp': 'IavRefWarp'}
APERTURE_APPS = ('aperture_map', 'aperture_misc', 'aperture_probe', 'aperture_consumer', 'mask', 'canvas_overlay', 'blur',
                 'arb_blur', 'yuv_remap', 'warp')


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


def main_scale(a, name, binary):
    import tempfile
    import numpy as np
    from PIL import Image
    jdir = a.jpeg_dir
    if not jdir or not os.path.isdir(jdir):
        sys.exit('--jpeg-dir with t720.jpg and t1080.jpg is required')
    tmp = tempfile.mkdtemp(prefix='iav-scale-')
    ok = True
    records = []
    stage(a.board, binary, name, False)
    stage(a.guest, binary, name, True)
    for jn in ('t720.jpg', 't1080.jpg'):
        scp(os.path.join(jdir, jn), a.board, '/tmp/%s' % jn)
        scp(os.path.join(jdir, jn), a.guest, '/tmp/%s' % jn)
    if a.app == 'scale_probe':
        res = {}
        run_side('guest', a.guest, name, '--jpeg /tmp/t1080.jpg --probe bad', True, res)
        emit_raw('guest(probe)', res['guest'])
        probes = [r for r in res['guest']['rows'] if r.get('kind') == 'probe']
        scaled = [r for r in res['guest']['rows'] if r.get('kind') == 'scale']
        for i, r in enumerate(probes):
            records.append(dict(seq=i, call=r['call'], case=r['case'], rc=r['rc'], errno=r['errno'],
                                expected_errno=r['expected_errno'], refused_as_expected=r['refused_as_expected']))
        good = res['guest']['rc'] == 0 and len(probes) == 12 and all(r['refused_as_expected'] == 1 for r in probes) \
            and len(scaled) == 1 and scaled[0]['pad_intact'] == 1
        print('CHECK guest_hostile_calls_refused %s %d probes, %d refused as expected; the good scale after them %s' % (
            'PASS' if good else 'FAIL', len(probes), sum(r['refused_as_expected'] for r in probes),
            'succeeded' if scaled else 'did not run'))
        ok = ok and good
    else:
        sides = (('native', a.board, False), ('guest', a.guest, True))
        for jn, w, h in (('t720.jpg', 1280, 720), ('t1080.jpg', 1920, 1080)):
            ref = np.array(Image.open(os.path.join(jdir, jn)).convert('YCbCr'))[:, :, 0]
            ref = np.array(Image.fromarray(ref).resize((640, 360), Image.BOX)).astype(float)
            for mode in ('block', 'wait'):
                res = {}
                for side, host, sudo in sides:
                    out = '/tmp/scale_%s_%s_%s.nv12' % (side, jn[1:-4], mode)
                    run_side(side, host, name, '--jpeg /tmp/%s --repeat 5 --mode %s --out %s' % (jn, mode, out), sudo, res)
                    emit_raw('%s(%s,%s)' % (side, jn, mode), res[side])
                    if res[side]['rc'] != 0:
                        print('CHECK %s_%s_%s_exit FAIL exit %d' % (side, jn, mode, res[side]['rc']))
                        ok = False
                fn = os.path.join(tmp, 'native_%s_%s.nv12' % (jn, mode))
                fg = os.path.join(tmp, 'guest_%s_%s.nv12' % (jn, mode))
                fetch(a.board, '/tmp/scale_native_%s_%s.nv12' % (jn[1:-4], mode), fn)
                fetch(a.guest, '/tmp/scale_guest_%s_%s.nv12' % (jn[1:-4], mode), fg)
                bn, bg = open(fn, 'rb').read(), open(fg, 'rb').read()
                equal = bn == bg and len(bg) == 640 * 360 * 3 // 2
                print('CHECK %s_%s_bit_equal %s guest and native scaled NV12 (%d bytes) are %s' % (
                    jn, mode, 'PASS' if equal else 'FAIL', len(bg), 'identical' if equal else 'different'))
                ok = ok and equal
                y = np.frombuffer(bg, dtype=np.uint8)[:640 * 360].reshape(360, 640).astype(float)
                mse = ((y - ref) ** 2).mean()
                ps = 99.0 if mse == 0 else float(10 * np.log10(255.0 ** 2 / mse))
                good = ps >= 30.0
                print('CHECK %s_%s_vs_software %s luma PSNR %.1f dB against a software box resize of the JPEG (bound 30 dB)' % (
                    jn, mode, 'PASS' if good else 'FAIL', ps))
                ok = ok and good
                gr = [r for r in res['guest']['rows'] if r.get('kind') == 'scale']
                nr = [r for r in res['native']['rows'] if r.get('kind') == 'scale']
                if len(gr) != 5 or len(nr) != 5:
                    print('CHECK %s_%s_passes FAIL guest %d native %d scale rows (5 each expected)' % (jn, mode, len(gr), len(nr)))
                    ok = False
                for i, (g, nn) in enumerate(zip(gr, nr)):
                    records.append(dict(pass_id='%s_%s_%d' % (jn[1:-4], mode, i), seq=len(records), mode=mode,
                                        in_width=g['in_width'], in_height=g['in_height'],
                                        guest_y_hash=g['y_hash'], native_y_hash=nn['y_hash'],
                                        guest_uv_hash=g['uv_hash'], native_uv_hash=nn['uv_hash'],
                                        guest_pad_intact=g['pad_intact'], native_pad_intact=nn['pad_intact'],
                                        guest_in_y_offset=g['in_y_offset'], native_in_y_offset=nn['in_y_offset'],
                                        guest_in_uv_offset=g['in_uv_offset'], native_in_uv_offset=nn['in_uv_offset'],
                                        guest_scale_us=g['scale_us'], native_scale_us=nn['scale_us'], psnr_y=round(ps, 2)))
    with open(a.records, 'w') as f:
        for r in records:
            f.write(json.dumps(r) + '\n')
    print('RESULT %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main_image(a, name, binary):
    ok = True
    records = []
    stage(a.board, binary, name, False)
    stage(a.guest, binary, name, True)
    res1, res2, res3 = {}, {}, {}
    run_side('native', a.board, name, '--stream 1', False, res1)
    emit_raw('native(1)', res1['native'])
    run_side('guest', a.guest, name, '--stream 1', True, res2)
    emit_raw('guest', res2['guest'])
    run_side('native', a.board, name, '--stream 1', False, res3)
    emit_raw('native(2)', res3['native'])

    def calls(res):
        return {(r['name'], r['case']): r for r in res['rows'] if r.get('kind') == 'call'}
    n1, g, n2 = calls(res1['native']), calls(res2['guest']), calls(res3['native'])
    for side, res in (('native(1)', res1['native']), ('guest', res2['guest']), ('native(2)', res3['native'])):
        if res['rc'] != 0:
            print('CHECK %s_exit FAIL exit %d' % (side, res['rc']))
            ok = False
    if set(n1) != set(g) or set(n1) != set(n2) or not n1:
        print('CHECK same_calls FAIL native %d, guest %d, native again %d calls' % (len(n1), len(g), len(n2)))
        ok = False
    for key in sorted(n1):
        if key not in g or key not in n2:
            continue
        a1, gg, a2 = n1[key], g[key], n2[key]
        stable = a1['digest'] == a2['digest'] and a1['rc'] == a2['rc']
        digest_ok = 1 if (not stable or a1['digest'] == gg['digest']) else 0
        rec = dict(seq=len(records), name=key[0], case=key[1], native_rc=a1['rc'], guest_rc=gg['rc'],
                   native_errno=a1['errno'], guest_errno=gg['errno'], digest_stable=int(stable), digest_ok=digest_ok,
                   native_value=a1.get('value', -1), guest_value=gg.get('value', -1))
        records.append(rec)
        good = a1['rc'] == gg['rc'] and a1['errno'] == gg['errno'] and digest_ok == 1
        print('CHECK %s/%s %s rc native %d guest %d, errno native %d guest %d, output %s' % (
            key[0], key[1], 'PASS' if good else 'FAIL', a1['rc'], gg['rc'], a1['errno'], gg['errno'],
            ('equal' if a1['digest'] == gg['digest'] else 'DIFFERENT') if stable else 'varies between native runs, not compared'))
        ok = ok and good
    gd = g.get(('IAV_IOC_GDMA_COPY', 'decode_to_decode_2d'), {})
    nd = n1.get(('IAV_IOC_GDMA_COPY', 'decode_to_decode_2d'), {})
    exact = gd.get('value') == 1 and nd.get('value') == 1
    print('CHECK gdma_exact %s guest %s native %s' % ('PASS' if exact else 'FAIL', gd.get('value'), nd.get('value')))
    ok = ok and exact
    nn = g.get(('IAV_IOC_GET_ISP_NN_CFG', 'chan_addresses_removed'), {})
    clean = nn.get('value') == 0
    print('CHECK guest_isp_nn_no_address %s the guest saw %s physical address' % ('PASS' if clean else 'FAIL',
                                                                                 'no' if clean else 'a'))
    ok = ok and clean
    with open(a.records, 'w') as f:
        for r in records:
            f.write(json.dumps(r) + '\n')
    print('RESULT %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main_arena(a, name, binary):
    ok = True
    records = []
    stage(a.board, binary, name, False)
    stage(a.guest, binary, name, True)
    if a.app == 'arena_probe':
        res = {}
        run_side('guest', a.guest, name, '--probe bad' + (' --foreign %d' % a.foreign if a.foreign else ''), True, res)
        emit_raw('guest(probe)', res['guest'])
        probes = [r for r in res['guest']['rows'] if r.get('kind') == 'probe']
        for i, r in enumerate(probes):
            records.append(dict(seq=i, case=r['case'], rc=r['rc'], errno=r['errno'], expected_errno=r['expected_errno'],
                                as_expected=r['as_expected']))
        good = res['guest']['rc'] == 0 and len(probes) >= 16 and all(r['as_expected'] == 1 for r in probes)
        print('CHECK guest_hostile_arena_calls_refused %s %d probes, %d as expected' % (
            'PASS' if good else 'FAIL', len(probes), sum(r['as_expected'] for r in probes)))
        ok = good
    elif a.app == 'arena_life':
        res = {}
        run_side('native', a.board, name, '--probe life', False, res)
        run_side('guest', a.guest, name, '--probe life', True, res)
        for side in ('native', 'guest'):
            emit_raw(side, res[side])
        nat = {r['case']: r for r in res['native']['rows'] if r.get('kind') == 'probe'}
        gst = {r['case']: r for r in res['guest']['rows'] if r.get('kind') == 'probe'}
        same = set(nat) == set(gst) and len(nat) >= 18
        print('CHECK same_cases %s native %d guest %d' % ('PASS' if same else 'FAIL', len(nat), len(gst)))
        ok = same
        for k in sorted(gst):
            n, g = nat.get(k, {}), gst[k]
            good = n.get('rc') == g['rc'] and n.get('errno') == g['errno'] and g['as_expected'] == 1 and \
                n.get('as_expected') == 1
            print('CHECK %s %s rc native %s guest %d, errno native %s guest %d' % (
                k, 'PASS' if good else 'FAIL', n.get('rc'), g['rc'], n.get('errno'), g['errno']))
            ok = ok and good
            records.append(dict(seq=len(records), case=k, native_rc=n.get('rc', -99), guest_rc=g['rc'],
                                native_errno=n.get('errno', -99), guest_errno=g['errno'],
                                native_as_expected=n.get('as_expected', 0), guest_as_expected=g['as_expected']))
        for side in ('native', 'guest'):
            if res[side]['rc'] != 0:
                print('CHECK %s_exit FAIL exit %d' % (side, res[side]['rc']))
                ok = False
    else:
        jdir = a.jpeg_dir
        if not jdir or not os.path.isdir(jdir):
            sys.exit('--jpeg-dir with t720.jpg and t1080.jpg is required')
        for jn in ('t720.jpg', 't1080.jpg'):
            scp(os.path.join(jdir, jn), a.board, '/tmp/%s' % jn)
            scp(os.path.join(jdir, jn), a.guest, '/tmp/%s' % jn)
        for jn in ('t720.jpg', 't1080.jpg'):
            for mode in ('block', 'wait'):
                res = {}
                for side, host, sudo in (('native', a.board, False), ('guest', a.guest, True)):
                    run_side(side, host, name, '--jpeg /tmp/%s --repeat 3 --mode %s' % (jn, mode), sudo, res)
                    emit_raw('%s(%s,%s)' % (side, jn, mode), res[side])
                    if res[side]['rc'] != 0:
                        print('CHECK %s_%s_%s_exit FAIL exit %d' % (side, jn, mode, res[side]['rc']))
                        ok = False
                gr = [r for r in res['guest']['rows'] if r.get('kind') == 'arena']
                nr = [r for r in res['native']['rows'] if r.get('kind') == 'arena']
                if len(gr) != 3 or len(nr) != 3:
                    print('CHECK %s_%s_passes FAIL guest %d native %d arena rows (3 each expected)' % (jn, mode, len(gr), len(nr)))
                    ok = False
                for i, (g, n) in enumerate(zip(gr, nr)):
                    same = g['y_hash'] == n['y_hash'] and g['uv_hash'] == n['uv_hash']
                    good = same and g['gdma_exact'] == 1 and n['gdma_exact'] == 1 and g['pad_intact'] == 1 and \
                        n['pad_intact'] == 1 and g['in_y_hash'] == g['dec_y_hash']
                    print('CHECK %s_%s_%d %s scaled picture %s, GDMA copy guest %d native %d, pad guest %d native %d' % (
                        jn, mode, i, 'PASS' if good else 'FAIL', 'bit-exact' if same else 'DIFFERENT', g['gdma_exact'],
                        n['gdma_exact'], g['pad_intact'], n['pad_intact']))
                    ok = ok and good
                    records.append(dict(seq=len(records), pass_id='%s_%s_%d' % (jn[1:-4], mode, i), mode=mode,
                                        in_width=g['in_width'], guest_y_hash=g['y_hash'], native_y_hash=n['y_hash'],
                                        guest_uv_hash=g['uv_hash'], native_uv_hash=n['uv_hash'],
                                        guest_gdma_exact=g['gdma_exact'], native_gdma_exact=n['gdma_exact'],
                                        guest_pad_intact=g['pad_intact'], native_pad_intact=n['pad_intact'],
                                        guest_in_pid_is_anon=g['in_pid_is_anon'], native_in_pid_is_anon=n['in_pid_is_anon'],
                                        guest_in_offset_aligned=g['in_offset_aligned'],
                                        guest_in_length=g['in_length'], native_in_length=n['in_length'],
                                        guest_scale_us=g['scale_us'], native_scale_us=n['scale_us'],
                                        guest_gdma_us=g['gdma_us'], native_gdma_us=n['gdma_us']))
    with open(a.records, 'w') as f:
        for r in records:
            f.write(json.dumps(r) + '\n')
    print('RESULT %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def tap_region(host, x, y, w, h, frames=2):
    """Luma mean and variance of an area of the last frames of the Dom0 frame tap (the demo's /live/stream)."""
    import io
    from PIL import Image
    p = subprocess.run(['ssh'] + HOST_OPTS.get(host, []) + [host, 'curl -s -m 3 http://127.0.0.1:8080/live/stream | '
                                                                 'head -c 1500000'], capture_output=True, timeout=60)
    data, jpegs, pos = p.stdout, [], 0
    while True:
        i = data.find(b'\xff\xd8\xff', pos)
        if i < 0:
            break
        j = data.find(b'\xff\xd9', i + 2)
        if j < 0:
            break
        jpegs.append(data[i:j + 2])
        pos = j + 2
    out = []
    for jp in jpegs[-frames:]:
        im = Image.open(io.BytesIO(jp)).convert('L')
        px = im.crop((x, y, x + w, y + h))
        v = list(px.get_flattened_data()) if hasattr(px, 'get_flattened_data') else list(px.getdata())
        m = sum(v) / float(len(v))
        out.append(dict(width=im.size[0], height=im.size[1], mean=round(m, 3),
                        var=round(sum((q - m) ** 2 for q in v) / float(len(v)), 3)))
    return out


def run_held(side, host, name, args, sudo, hold_ms, tap_host, area, res):
    """Run an application that holds its effect for hold_ms, and sample the tap before, during and after."""
    taps = {}
    taps['before'] = tap_region(tap_host, *area)
    t = threading.Thread(target=run_side, args=(side, host, name, args, sudo, res))
    t.start()
    import time
    time.sleep(hold_ms / 2000.0 + 2.0)
    taps['during'] = tap_region(tap_host, *area)
    t.join()
    time.sleep(2.0)
    taps['after'] = tap_region(tap_host, *area)
    return taps


def main_aperture(a, name, binary):
    ok = True
    records = []
    sides = []
    if a.sides in ('native', 'both'):
        sides.append(('native', a.board, False))
    if a.sides in ('guest', 'both'):
        sides.append(('guest', a.guest, True))
    if a.app in ('aperture_probe', 'aperture_consumer'):
        sides = [('guest', a.guest, True)]
    for side, host, sudo in sides:
        stage(host, binary, name, sudo)
    tap_host = a.tap_host or a.guest

    def check(label, good, detail):
        print('CHECK %s %s %s' % (label, 'PASS' if good else 'FAIL', detail))
        return good

    def rows(res, kind):
        return [r for r in res['rows'] if r.get('kind') == kind]

    def region(res, phase, part=None):
        for r in rows(res, 'region'):
            if r.get('phase') == phase and (part is None or r.get('part') == part):
                return r
        return {}

    if a.app in ('aperture_probe', 'aperture_consumer'):
        res = {}
        run_side('guest', a.guest, name, '--probe ' + ('bad' if a.app == 'aperture_probe' else 'consumer'), True, res)
        emit_raw('guest(probe)', res['guest'])
        probes = rows(res['guest'], 'probe')
        for i, r in enumerate(probes):
            records.append(dict(seq=i, case=r['case'], rc=r['rc'], errno=r['errno'], expected_errno=r['expected_errno'],
                                as_expected=r['as_expected']))
        want = 21 if a.app == 'aperture_probe' else 12
        ok = check('guest_%s' % a.app, res['guest']['rc'] == 0 and len(probes) >= want and
                   all(r['as_expected'] == 1 for r in probes),
                   '%d probes (at least %d), %d as expected, exit %d' % (len(probes), want,
                                                                         sum(r['as_expected'] for r in probes),
                                                                         res['guest']['rc']))
    else:
        args = {'aperture_map': '--map', 'aperture_misc': '--misc', 'mask': '--mask', 'blur': '', 'arb_blur': '--arb',
                'warp': '', 'canvas_overlay': '--canvas-overlay --hold-ms 8000', 'yuv_remap': '--hold-ms 8000'}[a.app]
        area = {'canvas_overlay': (256, 128, 256, 128), 'yuv_remap': (256, 128, 512, 256)}.get(a.app)
        out = {}
        for side, host, sudo in sides:
            res = {}
            taps = run_held(side, host, name, args, sudo, 8000, tap_host, area, res) if area else None
            if not area:
                run_side(side, host, name, args, sudo, res)
            emit_raw(side, res[side])
            if taps is not None:
                for ph in ('before', 'during', 'after'):
                    for i, t in enumerate(taps[ph]):
                        print('%s tap %s %d %s' % (side, ph, i, json.dumps(t)))
            r = res[side]
            ok = check('%s_exit' % side, r['rc'] == 0, 'exit %d' % r['rc']) and ok
            calls = rows(r, 'call')
            if a.app not in ('aperture_misc', 'warp', 'aperture_map'):
                ok = check('%s_calls' % side, len(calls) >= 3 and all(c['rc'] == 0 for c in calls),
                           ' '.join('%s=%d/%d' % (c['case'], c['rc'], c['errno']) for c in calls)) and ok
            out[side] = (r, taps)
            if a.app == 'aperture_map':
                maps = rows(r, 'map')
                for m in maps:
                    good = m.get('present') == 1 and m.get('mapped') == 1 and m.get('readback') == 1 and \
                        m.get('over_refused') == 1
                    ok = check('%s_map_%s' % (side, m['part']), good, json.dumps(m)) and ok
                ok = check('%s_map_count' % side, len(maps) == 7, '%d partitions' % len(maps)) and ok
            elif a.app == 'mask':
                b, d, f = region(r, 'before'), region(r, 'during'), region(r, 'after')
                good = d.get('var', 1e9) <= 50 and abs(d.get('mean', 0) - b.get('mean', 0)) >= 30 and \
                    abs(f.get('mean', 1e9) - b.get('mean', 0)) <= 15
                ok = check('%s_mask_visible' % side, good, 'mean %.1f -> %.1f -> %.1f, variance %.1f -> %.1f -> %.1f' % (
                    b.get('mean', -1), d.get('mean', -1), f.get('mean', -1), b.get('var', -1), d.get('var', -1),
                    f.get('var', -1))) and ok
                records.append(dict(seq=len(records), side=side, before_mean=b.get('mean'), during_mean=d.get('mean'),
                                    after_mean=f.get('mean'), before_var=b.get('var'), during_var=d.get('var'),
                                    after_var=f.get('var'), verdict=int(good)))
            elif a.app in ('blur', 'arb_blur'):
                parts = ('area',) if a.app == 'blur' else ('left', 'right')
                rec = dict(seq=len(records), side=side)
                good = True
                for part in parts:
                    b, d, f = region(r, 'before', part), region(r, 'during', part), region(r, 'after', part)
                    rd = d.get('var', 0) / b['var'] if b.get('var') else -1
                    rf = f.get('var', 0) / b['var'] if b.get('var') else -1
                    if part in ('area', 'right'):
                        g = 0 <= rd <= 0.9 and rf >= 0.9
                    else:
                        g = rd >= 0.95
                    print('%s %s variance %.1f -> %.1f -> %.1f (during/before %.3f, after/before %.3f) %s' % (
                        side, part, b.get('var', -1), d.get('var', -1), f.get('var', -1), rd, rf,
                        'as expected' if g else 'NOT as expected'))
                    good = good and g
                    rec.update({'%s_before_var' % part: b.get('var'), '%s_during_var' % part: d.get('var'),
                                '%s_after_var' % part: f.get('var'), '%s_during_ratio' % part: round(rd, 4)})
                rec['verdict'] = int(good)
                records.append(rec)
                ok = check('%s_%s_visible' % (side, a.app), good, '') and ok
            elif area:
                b, d, f = taps['before'], taps['during'], taps['after']
                textured = lambda xs: len(xs) >= 1 and all(t['var'] >= 50 for t in xs)
                held = len(d) >= 1 and all(t['var'] <= 4 for t in d) and all(t['width'] == 1920 for t in b + d + f)
                if a.app == 'yuv_remap':
                    held = held and all(abs(t['mean'] - 16) <= 4 for t in d)
                good = held and textured(b) and textured(f)
                ok = check('%s_%s_in_stream' % (side, a.app), good, 'before %s during %s after %s' % (
                    [t['var'] for t in b], [(t['mean'], t['var']) for t in d], [t['var'] for t in f])) and ok
                records.append(dict(seq=len(records), side=side, before_var=min([t['var'] for t in b] or [-1]),
                                    during_var=max([t['var'] for t in d] or [-1]),
                                    during_mean=d[-1]['mean'] if d else -1,
                                    after_var=min([t['var'] for t in f] or [-1]), verdict=int(good)))
        if a.app in ('aperture_misc', 'warp', 'aperture_map') and len(out) == 2:
            key = 'map' if a.app == 'aperture_map' else 'call'
            n = {(x.get('case') or x.get('part')): x for x in rows(out['native'][0], key)}
            g = {(x.get('case') or x.get('part')): x for x in rows(out['guest'][0], key)}
            ok = check('same_rows', set(n) == set(g) and len(n) > 0, 'native %d guest %d' % (len(n), len(g))) and ok
            for k in sorted(g):
                nn, gg = n.get(k, {}), g[k]
                if key == 'map':
                    good = nn.get('length') == gg.get('length')
                    rec = dict(seq=len(records), case=k, native_length=nn.get('length', -1), guest_length=gg['length'],
                               native_mapped=nn.get('mapped', -1), guest_mapped=gg.get('mapped', -1),
                               native_readback=nn.get('readback', -1), guest_readback=gg.get('readback', -1))
                else:
                    good = nn.get('rc') == gg['rc'] and nn.get('errno') == gg['errno']
                    rec = dict(seq=len(records), case=k, native_rc=nn.get('rc', -99), guest_rc=gg['rc'],
                               native_errno=nn.get('errno', -99), guest_errno=gg['errno'])
                records.append(rec)
                ok = check(k, good, json.dumps(rec)) and ok
            if a.app == 'aperture_misc':
                nm, gm = rows(out['native'][0], 'misc'), rows(out['guest'][0], 'misc')
                good = len(nm) == 1 and len(gm) == 1 and nm[0]['valid_flags_written'] == gm[0]['valid_flags_written'] and \
                    nm[0]['valid_set'] == gm[0]['valid_set'] and gm[0]['pointers_kept'] == 1 and \
                    gm[0]['valid_flags_written'] >= 1
                ok = check('misc_outputs', good, 'native %s guest %s' % (nm, gm)) and ok
                if records:
                    for r in records:
                        r['native_valid_written'] = nm[0]['valid_flags_written'] if nm else -1
                        r['guest_valid_written'] = gm[0]['valid_flags_written'] if gm else -1
                        r['guest_pointers_kept'] = gm[0]['pointers_kept'] if gm else -1
    with open(a.records, 'w') as f:
        for r in records:
            f.write(json.dumps(r) + '\n')
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
    ap.add_argument('--foreign', type=int, default=0, help='arena_probe: an anonymous id another guest holds')
    ap.add_argument('--binary-dir', default=os.path.join(ROOT, 'build', 'iav-ref'))
    ap.add_argument('--sides', choices=('native', 'guest', 'both'), default='both',
                    help='aperture applications: run natively, in the guest, or both (default)')
    ap.add_argument('--tap-host', default=None, help='the guest whose demo serves the Dom0 frame tap (default --guest)')
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
    if a.app in ('scale', 'scale_probe'):
        return main_scale(a, name, binary)
    if a.app == 'image':
        return main_image(a, name, binary)
    if a.app in ('arena', 'arena_life', 'arena_probe'):
        return main_arena(a, name, binary)
    if a.app in APERTURE_APPS:
        return main_aperture(a, name, binary)
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
