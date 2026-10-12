#!/usr/bin/env python3
# Copyright (C) 2026, Ambarella International LLC.
"""An overlay set by the primary guest is in the stream a guest's demo serves, with the production server (Dom0 frame tap).

  overlay_prod_gate.py --board <dom0 alias> --primary <guest alias> --consumer <guest alias> [--binary-dir DIR]

With the production server the Dom0 tap reads the descriptors of stream 1, so a guest cannot read them itself
(IavRefOverlay --no-frames). The demo that runs in each guest serves the hardware-encoded MJPEG frames the tap hands
over, at http://127.0.0.1:8080/live/stream. This script takes one frame from each guest's stream before, while the
primary guest holds an overlay (a white 128x64 box with a black border at 64,64), and after it was disabled, decodes
them here, and checks in both guests' frames: the box is white with a black border while the overlay is held, the rest
of the frame is unchanged, and the box is gone afterwards. "CHECK <name> PASS|FAIL <detail>" lines; RESULT PASS when
every check held.
"""
import argparse
import io
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ref_gate as rg  # noqa: E402


def frame(host, port, tag, tmp):
    """The latest complete JPEG of three seconds of the guest demo's stream: it starts with a backlog of older frames."""
    raw = os.path.join(tmp, '%s.raw' % tag)
    p = subprocess.run(['ssh', host, 'curl -s -m 3 http://127.0.0.1:%d/live/stream | tail -c 1500000' % port], capture_output=True)
    data = p.stdout
    j = data.rfind(b'\xff\xd9')
    i = data.rfind(b'\xff\xd8\xff', 0, j) if j >= 0 else -1
    if i < 0 or j < 0:
        return None
    with open(raw, 'wb') as f:
        f.write(data[i:j + 2])
    return data[i:j + 2]


def main():
    import numpy as np
    from PIL import Image
    import tempfile
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--board', required=True)
    ap.add_argument('--primary', required=True)
    ap.add_argument('--consumer', required=True)
    ap.add_argument('--binary-dir', default=os.path.join(rg.ROOT, 'build', 'iav-ref'))
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix='iav-overlay-prod-')
    ok = True

    def check(name, good, detail):
        nonlocal ok
        print('CHECK %s %s %s' % (name, 'PASS' if good else 'FAIL', detail))
        ok = ok and good

    rg.stage(a.primary, os.path.join(a.binary_dir, 'IavRefOverlay'), 'IavRefOverlay', True)
    guests = (('primary', a.primary, 8080), ('consumer', a.consumer, 8081))
    pics = {}
    for tag, host, port in guests:
        pics[(tag, 'before')] = frame(host, port, tag + '_before', tmp)
    res = {}
    t = threading.Thread(target=rg.run_side, args=('overlay', a.primary, 'IavRefOverlay', '--stream 1 --no-frames --hold-ms 14000', True, res))
    t.start()
    time.sleep(3)
    for tag, host, port in guests:
        pics[(tag, 'during')] = frame(host, port, tag + '_during', tmp)
    t.join()
    rg.emit_raw('overlay', res['overlay'])
    check('overlay_app_exit', res['overlay']['rc'] == 0, 'exit %d' % res['overlay']['rc'])
    time.sleep(3)
    for tag, host, port in guests:
        pics[(tag, 'after')] = frame(host, port, tag + '_after', tmp)

    x, y, w, h = 64, 64, 128, 64
    for tag, _, _ in guests:
        got = [pics.get((tag, k)) for k in ('before', 'during', 'after')]
        if any(g is None for g in got):
            check('%s_frames' % tag, False, 'could not take a frame from the demo stream of the %s guest' % tag)
            continue
        b, d, f = [np.array(Image.open(io.BytesIO(g)).convert('L')).astype(float) for g in got]
        if not (b.shape == d.shape == f.shape):
            check('%s_frame_size' % tag, False, 'shapes differ: %s %s %s' % (b.shape, d.shape, f.shape))
            continue
        check('%s_frame_size' % tag, b.shape == (1080, 1920), '%dx%d' % (b.shape[1], b.shape[0]))
        inner = (slice(y + 8, y + h - 8), slice(x + 8, x + w - 8))
        white = float(d[inner].mean())
        edge = float(d[y + 1:y + 3, x + 16:x + w - 16].mean())
        mask = np.ones(d.shape, bool)
        mask[max(y - 16, 0):y + h + 16, max(x - 16, 0):x + w + 16] = False
        outside = float(np.abs(d - b)[mask].mean())
        back = float(np.abs(f[inner] - b[inner]).mean())
        check('%s_box_white' % tag, white > 215, 'mean luma inside the box %.1f while held (before %.1f)' % (white, float(b[inner].mean())))
        check('%s_box_border' % tag, edge < 40, 'mean luma of the border %.1f' % edge)
        check('%s_outside_unchanged' % tag, outside < 12.0, 'mean abs difference outside the box %.2f' % outside)
        check('%s_box_gone_after' % tag, back < 12.0, 'mean abs difference inside the box after the overlay was disabled %.2f' % back)
    print('RESULT %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
