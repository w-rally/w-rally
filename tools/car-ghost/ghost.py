"""ghost.py <run dir> <reference dir> [--max <bias>] [--box x0,y0,x1,y1] [--blur <px> --max-abs <grey>] [--lit <percent>]:
the ghost behind the car, measured.

Both dirs hold the same replayed frames (check-car-ghost.sh): <run> with the RT denoisers' history as the game has it,
<reference> with none (NGG_RT_NO_HISTORY=1) - this frame's rays alone. Under the chase camera the car stands near the
middle of the screen; the road behind and beside its back (BOX) is where a history leaking from the car lands: a light
copy of the exhaust or the bumper a frame of travel behind them (view 13, the AO) or of their shadow (view 11).
The ghost is the SIGNED mean of run - reference there (0..255): the history may smooth the noise away, not shift it -
darker is the car's occlusion or shadow lagging behind it, and the road the exhaust or the bumper just uncovered,
which has no history to lag, then shows lighter in their shape. Exits 1 when the mean bias is over --max either way.
--blur: both pictures box-blurred first (the reference's grain gone) and the mean |run - reference| judged against
--max-abs - for copies of the car under a still TV camera, lighter and darker in turn, which a signed mean misses.
--lit: only the road the reference has lit all around (its 9x9 minimum >= 250 grey: every ray there says sun), and the
share of it over 4 grey darker in the run, judged against <percent> - for a pale copy of the shadow under the car, a
frame of travel behind it, on sunlit road (2026-11-19: 9% before the fix, 0 after); the grain and the penumbras, where
a history smooths either way, are left out.
"""
import sys, glob, os
import numpy as np
from PIL import Image, ImageFilter

BOX = (700, 930, 1220, 1075)   # x0 y0 x1 y1 at 1920x1080: below and beside the chase camera's car's bumper

BLUR = 0

def load(f):
    im = Image.open(f).convert('L')
    a = np.asarray(im.filter(ImageFilter.BoxBlur(BLUR)) if BLUR else im, dtype=np.float32)
    h, w = a.shape
    x0, y0, x1, y1 = (int(BOX[0] * w / 1920), int(BOX[1] * h / 1080), int(BOX[2] * w / 1920), int(BOX[3] * h / 1080))
    return a[y0:y1, x0:x1]

def main():
    run, ref = sys.argv[1], sys.argv[2]
    lim = float(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else None
    if '--box' in sys.argv:     # x0,y0,x1,y1 at 1920x1080 in place of BOX (another camera)
        global BOX
        BOX = tuple(int(v) for v in sys.argv[sys.argv.index('--box') + 1].split(','))
    if '--blur' in sys.argv:
        global BLUR
        BLUR = int(sys.argv[sys.argv.index('--blur') + 1])
    limAbs = float(sys.argv[sys.argv.index('--max-abs') + 1]) if '--max-abs' in sys.argv else None
    limLit = float(sys.argv[sys.argv.index('--lit') + 1]) if '--lit' in sys.argv else None
    fs = sorted(glob.glob(os.path.join(run, 'shot-*.png')))[8:]     # the first frames: the history still filling
    bias, absd, lit = [], [], []
    for f in fs:
        g = os.path.join(ref, os.path.basename(f))
        if not os.path.exists(g):
            continue
        d = load(f) - load(g)
        bias.append(float(d.mean()))
        absd.append(float(np.abs(d).mean()))
        if limLit is not None:
            r = load(g)
            m = np.asarray(Image.fromarray(r.astype(np.uint8)).filter(ImageFilter.MinFilter(9)), dtype=np.float32) >= 250
            lit.append(float((d[m] < -4).mean()) * 100 if m.any() else 0.0)
    if not bias:
        print('no frames'); return 2
    b = np.array(bias)
    print('frames %d  bias mean %+.2f  max %+.2f  |diff| %.2f  (0..255, run - no-history reference)'
          % (len(b), b.mean(), b.max(), float(np.mean(absd))))
    if float(np.mean(absd)) < 0.05:
        print('the reference IS the run: this build ignores NGG_RT_NO_HISTORY - nothing measured'); return 2
    if limLit is not None:
        print('           lit road over 4 grey darker: %.1f%% (worst frame %.1f%%)' % (np.mean(lit), np.max(lit)))
        if np.mean(lit) > limLit:
            print('GHOST: %.1f%% of the lit road is darker, over %.1f%%' % (np.mean(lit), limLit)); return 1
    if limAbs is not None and float(np.mean(absd)) > limAbs:
        print('GHOST: |diff| %.2f is over %.2f' % (float(np.mean(absd)), limAbs)); return 1
    if lim is not None and abs(b.mean()) > lim:
        print('GHOST: the bias %.2f is over %.2f' % (b.mean(), lim)); return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
