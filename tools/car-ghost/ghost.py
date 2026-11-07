"""ghost.py <run dir> <reference dir> [--max <bias>]: the ghost behind the car, measured.

Both dirs hold the same replayed frames (check-car-ghost.sh): <run> with the RT denoisers' history as the game has it,
<reference> with none (NGG_RT_NO_HISTORY=1) - this frame's rays alone. Under the chase camera the car stands near the
middle of the screen; the road behind and beside its back (BOX) is where a history leaking from the car lands: a light
copy of the exhaust or the bumper a frame of travel behind them (view 13, the AO) or of their shadow (view 11).
The ghost is the SIGNED mean of run - reference there (0..255): the history may smooth the noise away, not shift it -
darker is the car's occlusion or shadow lagging behind it, and the road the exhaust or the bumper just uncovered,
which has no history to lag, then shows lighter in their shape. Exits 1 when the mean bias is over --max either way.
"""
import sys, glob, os
import numpy as np
from PIL import Image

BOX = (700, 930, 1220, 1075)   # x0 y0 x1 y1 at 1920x1080: below and beside the chase camera's car's bumper

def load(f):
    a = np.asarray(Image.open(f).convert('L'), dtype=np.float32)
    h, w = a.shape
    x0, y0, x1, y1 = (int(BOX[0] * w / 1920), int(BOX[1] * h / 1080), int(BOX[2] * w / 1920), int(BOX[3] * h / 1080))
    return a[y0:y1, x0:x1]

def main():
    run, ref = sys.argv[1], sys.argv[2]
    lim = float(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else None
    fs = sorted(glob.glob(os.path.join(run, 'shot-*.png')))[8:]     # the first frames: the history still filling
    bias, absd = [], []
    for f in fs:
        g = os.path.join(ref, os.path.basename(f))
        if not os.path.exists(g):
            continue
        d = load(f) - load(g)
        bias.append(float(d.mean()))
        absd.append(float(np.abs(d).mean()))
    if not bias:
        print('no frames'); return 2
    b = np.array(bias)
    print('frames %d  bias mean %+.2f  max %+.2f  |diff| %.2f  (0..255, run - no-history reference)'
          % (len(b), b.mean(), b.max(), float(np.mean(absd))))
    if float(np.mean(absd)) < 0.05:
        print('the reference IS the run: this build ignores NGG_RT_NO_HISTORY - nothing measured'); return 2
    if lim is not None and abs(b.mean()) > lim:
        print('GHOST: the bias %.2f is over %.2f' % (b.mean(), lim)); return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
