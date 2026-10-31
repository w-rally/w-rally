"""The car's lamps on a wall 10 m ahead - a headlamp aiming screen, drawn without the engine.

The beam model is the renderer's (lights.cpp, LightIrradiance): each lamp a sum of lobes, elliptical peaks in candela
in the lamp's own frame, I = sum candela * 2^-(q^(sharpness/2)), q = ((tx - tan yaw) / tan halfWidth)^2 +
((ty - tan pitch) / tan halfHeight)^2, tx, ty the ray's tangents across and up; lux on the wall = I / d^2 * cos.
The presets and the test layouts are a copy of game/src/assets/car_lights.cpp's (kBeams, TestLayout) - tune them
here, then copy them there; keep the two in step.

    python wall.py <layout> <out.png> [distance]
"""
import math
import sys
from PIL import Image, ImageDraw

# Presets: kelvin, lobes (candela, halfWidth, halfHeight, yaw, pitch, sharpness).
BEAMS = {
    'halogen-low':  (3200, [(16000, 22.0, 3.5, 0.0, -2.0, 4.0), (5000, 45.0, 8.0, 0.0, -5.0, 2.0)]),
    'halogen-high': (3300, [(80000, 7.0, 3.5, 0.0, 0.0, 2.0), (18000, 22.0, 7.0, 0.0, -1.0, 2.0)]),
    'halogen-pencil':  (3300, [(350000, 2.5, 1.8, 0.0, 0.0, 3.0), (15000, 6.0, 3.5, 0.0, 0.0, 2.0),
                            (25000, 25.0, 10.0, 0.0, -1.0, 2.0)]),
    'halogen-driving': (3300, [(150000, 5.0, 3.0, 0.0, 0.0, 3.0), (15000, 12.0, 5.0, 0.0, -1.0, 2.0),
                            (12000, 30.0, 12.0, 0.0, -1.0, 2.0)]),
    # A combo bar's smooth beam: a spot in the middle, a wide flood round it, no cut-off (not a halogen's).
    'led-bar':      (5700, [(150000, 6.0, 3.5, 0.0, 0.0, 2.0), (30000, 18.0, 6.0, 0.0, -0.5, 2.0),
                            (10000, 40.0, 10.0, 0.0, -1.0, 2.0)]),
    'ditch':        (5800, [(40000, 20.0, 6.0, 0.0, -2.0, 2.0)]),
}

# Layouts: (beam, x LEFT, y (-forward), z up from the centre of gravity, yaw right, pitch up).
HEAD = [('halogen-high', 0.65, -2.0, 0.05, 0.0, -0.5), ('halogen-high', -0.65, -2.0, 0.05, 0.0, -0.5)]
# The ramp's main beams: turned 10 degrees out, each to its side.
HEAD_RAMP = [('halogen-high', 0.65, -2.0, 0.05, -10.0, -0.5), ('halogen-high', -0.65, -2.0, 0.05, 10.0, -0.5)]
LAYOUTS = {
    # The pods each to their own spot: the outer ones 20 degrees out to their corners, the inner ones 5 degrees out
    # and up (yaw is to the right, so the left side's are negative).
    'ramp': HEAD_RAMP + [('halogen-pencil', 0.38, -1.9, 0.3, -20.0, -0.8), ('halogen-pencil', 0.12, -1.9, 0.3, -5.0, 4.0),
                         ('halogen-pencil', -0.12, -1.9, 0.3, 5.0, 4.0), ('halogen-pencil', -0.38, -1.9, 0.3, 20.0, -0.8)],
    # The i20 R5's own six pods (its .ini's Point_light_bon1..6, the centre of gravity as HEAD's), aimed as the game's
    # AimRamp aims six: the outer pair 20 degrees out, the middle pair 13 and 2 up, the inner pair 5 out and 4 up;
    # all six pencils.
    'ramp6': HEAD_RAMP + [('halogen-pencil', 0.456, -2.27, 0.15, -20.0, -0.8), ('halogen-pencil', 0.316, -2.27, 0.18, -13.0, 2.0),
                          ('halogen-pencil', 0.126, -2.27, 0.18, -5.0, 4.0), ('halogen-pencil', -0.126, -2.27, 0.18, 5.0, 4.0),
                          ('halogen-pencil', -0.316, -2.27, 0.18, 13.0, 2.0), ('halogen-pencil', -0.486, -2.27, 0.15, 20.0, -0.8)],
    # Two pods: driving beams 5 degrees out and 3 up.
    'pods2': HEAD_RAMP + [('halogen-driving', 0.25, -1.9, 0.3, -5.0, 3.0), ('halogen-driving', -0.25, -1.9, 0.3, 5.0, 3.0)],
    # The LED bars: the main beams straight on (the bars' flood covers the corners).
    'bars': HEAD + [('led-bar', 0.3, -1.8, 0.32, 0.0, -0.8), ('led-bar', -0.3, -1.8, 0.32, 0.0, -0.8)],
    'low': [('halogen-low', 0.65, -2.0, 0.05, 0.0, 0.0), ('halogen-low', -0.65, -2.0, 0.05, 0.0, 0.0)],
}
GROUND = -0.45   # the ground under the centre of gravity
D2R = math.pi / 180.0
EXPOSE = 1500.0   # lux at half white in the 'as seen' panel


def kelvin_rgb(k):
    t = min(max(k, 1667.0), 25000.0)
    x = (-0.2661239e9 / t**3 - 0.2343589e6 / t**2 + 0.8776956e3 / t + 0.179910) if t <= 4000 else \
        (-3.0258469e9 / t**3 + 2.1070379e6 / t**2 + 0.2226347e3 / t + 0.240390)
    y = (-1.1063814 * x**3 - 1.34811020 * x**2 + 2.18555832 * x - 0.20219683) if t <= 2222 else \
        (-0.9549476 * x**3 - 1.37418593 * x**2 + 2.09137015 * x - 0.16748867) if t <= 4000 else \
        (3.0817580 * x**3 - 5.87338670 * x**2 + 3.75112997 * x - 0.37001483)
    X, Y, Z = x / y, 1.0, (1 - x - y) / y
    r = max(3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z, 0)
    g = max(-0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z, 0)
    b = max(0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z, 0)
    lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
    return r / lum, g / lum, b / lum


def lamp_frame(yaw, pitch):
    cy, sy, cp, sp = math.cos(yaw * D2R), math.sin(yaw * D2R), math.cos(pitch * D2R), math.sin(pitch * D2R)
    axis = (-sy * cp, -cy * cp, sp)        # x left: a yaw to the right turns the axis towards -x
    up = (sy * sp, cy * sp, cp)
    right = (-cy, sy, 0.0)
    return axis, up, right


def lux_at(lamp, p):
    beam, lx, ly, lz, yaw, pitch = lamp
    axis, up, right = lamp_frame(yaw, pitch)
    d = (p[0] - lx, p[1] - ly, p[2] - lz)
    d2 = d[0] ** 2 + d[1] ** 2 + d[2] ** 2
    inv = 1.0 / math.sqrt(d2)
    t = (d[0] * inv, d[1] * inv, d[2] * inv)
    cd = t[0] * axis[0] + t[1] * axis[1] + t[2] * axis[2]
    if cd <= 1e-3:
        return 0.0
    tx = (t[0] * right[0] + t[1] * right[1] + t[2] * right[2]) / cd
    ty = (t[0] * up[0] + t[1] * up[1] + t[2] * up[2]) / cd
    intensity = 0.0
    for cand, hw, hh, oy, op, sharp in BEAMS[beam][1]:
        qx = (tx - math.tan(oy * D2R)) / math.tan(hw * D2R)
        qy = (ty - math.tan(op * D2R)) / math.tan(hh * D2R)
        intensity += cand * 2.0 ** -(max(qx * qx + qy * qy, 1e-12) ** (0.5 * sharp))
    cosw = -t[1]   # onto the wall, its normal +y (towards the car)
    return intensity / d2 * cosw


def main():
    name, out = sys.argv[1], sys.argv[2]
    dist = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
    lamps = LAYOUTS[name]
    nose = min(l[2] for l in lamps)
    wall_y = nose - dist
    half = dist * math.tan(45 * D2R)                 # +-45 degrees across
    top = GROUND + 4.5
    W = 900
    H = int(W * (top - GROUND) / (2 * half))
    hues = [(255, 80, 80), (80, 160, 255), (80, 230, 120), (255, 210, 60), (230, 100, 255), (60, 230, 230),
            (255, 150, 60), (200, 200, 200)]
    tints = [kelvin_rgb(BEAMS[l[0]][0]) for l in lamps]
    # lux per lamp per pixel
    planes = [[0.0] * (W * H) for _ in lamps]
    for j in range(H):
        z = top + (GROUND - top) * (j + 0.5) / H
        for i in range(W):
            x = half + (-2 * half) * (i + 0.5) / W
            for k, lamp in enumerate(lamps):
                planes[k][j * W + i] = lux_at(lamp, (x, wall_y, z))

    def to_px(x, z):
        return (half - x) / (2 * half) * W, (top - z) / (top - GROUND) * H

    # Panel 1: as the eye sees it (light x tint, a soft shoulder: 300 lux mid-grey-ish).
    p1 = Image.new('RGB', (W, H))
    px1 = p1.load()
    for j in range(H):
        for i in range(W):
            rgb = [0.0, 0.0, 0.0]
            for k in range(len(lamps)):
                e = planes[k][j * W + i]
                for c in range(3):
                    rgb[c] += e * tints[k][c]
            px1[i, j] = tuple(int(255 * ((v / EXPOSE) / (1 + v / EXPOSE)) ** (1 / 2.2)) for v in rgb)
    # Panel 2: each lamp in its own colour, its isolux contours (30, 100, 300, 1000, 3000 lux).
    p2 = Image.new('RGB', (W, H), (12, 12, 16))
    px2 = p2.load()
    levels = [30, 100, 300, 1000, 3000]
    for k in range(len(lamps)):
        pl = planes[k]
        for j in range(H - 1):
            for i in range(W - 1):
                a, b, c = pl[j * W + i], pl[j * W + i + 1], pl[(j + 1) * W + i]
                for li, lv in enumerate(levels):
                    if (a >= lv) != (b >= lv) or (a >= lv) != (c >= lv):
                        f = 0.45 + 0.55 * li / (len(levels) - 1)
                        px2[i, j] = tuple(int(h * f) for h in hues[k])
    for img in (p1, p2):
        d = ImageDraw.Draw(img)
        for m in range(-int(half), int(half) + 1):          # 1 m grid
            x0, _ = to_px(m, 0)
            d.line([(x0, 0), (x0, H)], fill=(60, 60, 60) if m else (120, 120, 120))
        for zz in range(0, int(top - GROUND) + 1):
            _, y0 = to_px(0, GROUND + zz)
            d.line([(0, y0), (W, y0)], fill=(60, 60, 60))
        lamp_z = sum(l[3] for l in lamps) / len(lamps)
        _, yl = to_px(0, lamp_z)
        d.line([(0, yl), (W, yl)], fill=(170, 140, 60))
        d.text((4, yl - 12), "lamps' height", fill=(200, 170, 80))
        for deg in range(-40, 41, 10):                       # angles as seen from the lamps
            x0, _ = to_px(-dist * math.tan(deg * D2R), 0)
            d.text((x0 - 8, 2), '%+d' % deg if deg else '0', fill=(200, 200, 200))
        d.text((4, H - 14), 'ground', fill=(150, 150, 150))
    d2 = ImageDraw.Draw(p2)
    for k, lamp in enumerate(lamps):
        d2.text((W - 230, 18 + 13 * k), '%s  %s' % (lamp[0], 'left' if lamp[1] > 0 else 'right'), fill=hues[k])
    out_img = Image.new('RGB', (W, 2 * H + 40), (0, 0, 0))
    out_img.paste(p1, (0, 18))
    out_img.paste(p2, (0, H + 38))
    d = ImageDraw.Draw(out_img)
    d.text((4, 3), '%s on a wall %.0f m ahead - as seen (half white at %.0f lux), 1 m grid, degrees from the car' % (name, dist, EXPOSE),
           fill=(230, 230, 230))
    d.text((4, H + 22), 'each lamp: isolux 30 / 100 / 300 / 1000 / 3000 lux (dim -> bright)', fill=(230, 230, 230))
    out_img.save(out)
    print('peak lux:', ', '.join('%s %.0f' % (l[0], max(pl)) for l, pl in zip(lamps, planes)))


if __name__ == '__main__':
    main()
