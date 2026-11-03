"""scan_transparent.py <Cars folder>: every car's transparent exterior groups by shader class, and the ones W-Rally draws
as holes, not glass (docs/car-pbr.md section 1) - what the glass rule changes, over all the cars at hand."""
import os, re, sys, collections
root = sys.argv[1]
def ini(p):
    d, sec = {}, None
    try: lines = open(p, encoding='latin-1').read().splitlines()
    except OSError: return d
    for l in lines:
        l = l.strip()
        if l.startswith('[') and l.endswith(']'): sec = l[1:-1]; d.setdefault(sec, {})
        elif '=' in l and sec is not None:
            k, v = l.split('=', 1); d[sec][k.strip()] = v.strip()
    return d
classes = collections.Counter()
changed = collections.defaultdict(set)
cars = 0
for car in sorted(os.listdir(root)):
    cd = os.path.join(root, car)
    if not os.path.isdir(cd): continue
    sgc = [f for f in os.listdir(cd) if f.lower().endswith('.sgc')]
    if not sgc: continue
    base = sgc[0][:-4]
    shaders = ini(os.path.join(cd, base + '_shaders.ini'))
    carini = ini(os.path.join(cd, base + '.ini'))
    if not carini: continue
    cars += 1
    def transparent(mat):
        s = shaders.get(mat, {})
        if s.get('OpacityChannel', '') == 'true': return True
        return 'transparent' in mat.lower()
    for mesh, keys in carini.items():
        if not mesh.lower().startswith('e_'): continue
        for k, v in keys.items():
            m = re.match(r'FaceBlock(\d+)_Material', k)
            if not m: continue
            sh = keys.get('FaceBlock%s_Shader' % m.group(1), '')
            if transparent(v):
                classes[sh] += 1
                if not sh.lower().startswith(('carglass', 'carintglass')):
                    changed[sh].add('%s:%s' % (car, mesh))
print(cars, 'cars; transparent e_ groups by shader class:', dict(classes))
for sh, s in sorted(changed.items()):
    print('\n%s (%d): %s' % (sh or '(none)', len(s), ' '.join(sorted(s))))
