"""make_pbr.py - drafts a car's PBR maps from its RBR textures: an ORM map per atlas (assets/car_model.h CarMap).

    car-uv-dump <car .sgc> > uv.txt
    python make_pbr.py <car folder> uv.txt [--out DIR] [--preview DIR] [--force]

RBR's car textures are colour alone, and one atlas serves paint, tyres, plastic and glass alike. The dump says which
texels which parts sample - every exterior mesh group's UV triangles with its shader class (<car>.ini
FaceBlock<g>_Shader) - so the atlas is painted class by class (FINISH below), the paint winning where parts share
texels. Inside the paint, what is dark and grey - a grille, black trim, a rubber seal drawn on the body - goes matte
(DARK), judged on the texture blurred (DARK_BLUR): a decal's black lettering averages out bright and keeps the
lacquer, an area of grille stays dark. Empty texels take their neighbours' finish, so the mips do not bleed the default into the edges.

Written beside each texture as <texture>_orm.dds (DXT5 with mips; r occlusion 1, g roughness, b metallic, a the clear
coat); a map already there is kept unless --force. A DRAFT, to correct by hand: a black-painted car would lose its
lacquer to DARK. No normal or base colour map: from colour alone they emboss every decal. --preview writes the class
map and the ORM's channels as PNGs.
"""
import argparse
import io
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

# Class (lower case, by prefix) -> (priority, roughness, metallic, coat). The higher priority wins a shared texel.
FINISH = [
    ('carpaint',    6, 0.20, 0.0, 1.0),
    ('carrim',      5, 0.35, 0.8, 0.0),
    ('cartire',     4, 0.85, 0.0, 0.0),
    ('carplastic',  3, 0.55, 0.0, 0.0),
    ('cartexture',  2, 0.60, 0.0, 0.0),
    ('carcolor',    2, 0.60, 0.0, 0.0),
    ('texture',     2, 0.60, 0.0, 0.0),
    ('carglass',    1, 0.05, 0.0, 0.0),
    ('carintglass', 1, 0.05, 0.0, 0.0),
    ('carlight',    1, 0.05, 0.0, 0.0),
]
UNKNOWN = (0, 0.60, 0.0, 0.0)          # what no part samples, or an unknown class
DARK_LUM = (0.02, 0.06)                # linear luminance: matte below the first, lacquered above the second
DARK_SAT = (0.15, 0.35)                # ... and only where this grey (saturation)
DARK_BLUR = 4.0                        # texels: the scale it is judged at (thin lettering below it)


def finish_of(cls):
    c = cls.lower()
    for name, *f in FINISH:
        if c.startswith(name):
            return tuple(f)
    return UNKNOWN


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def class_map(size, tris):
    """Each texel's finish index into `finishes` (0: none), painted lowest priority first."""
    w, h = size
    finishes = [UNKNOWN]
    index = {}
    img = Image.new('L', size, 0)
    draw = ImageDraw.Draw(img)
    for f, uv in sorted(tris, key=lambda t: t[0][0]):
        if f not in index:
            index[f] = len(finishes)
            finishes.append(f)
        u = [uv[0], uv[2], uv[4]]
        v = [uv[1], uv[3], uv[5]]
        du, dv = np.floor(min(u)), np.floor(min(v))   # a tile's worth of wrap
        pts = [((u[k] - du) * w, (v[k] - dv) * h) for k in range(3)]
        draw.polygon(pts, fill=index[f])
    # Empty texels: their neighbours' (the highest priority among them: indices follow priority order), a few times.
    lab = np.asarray(img).copy()
    order = np.argsort([f[0] for f in finishes])            # index -> rank by priority
    rank = np.empty_like(order)
    rank[order] = np.arange(len(order))
    ranked = Image.fromarray(rank[lab].astype(np.uint8))
    for _ in range(12):
        grown = np.asarray(ranked.filter(ImageFilter.MaxFilter(3)))
        cur = np.asarray(ranked)
        ranked = Image.fromarray(np.where(cur == 0, grown, cur).astype(np.uint8))
    lab = order[np.asarray(ranked)]
    return lab, finishes


def dds_dxt5_with_mips(rgba):
    """A DXT5 DDS of `rgba` (H x W x 4 uint8) with its mip chain down to 4x4 (Pillow encodes each level)."""
    levels = []
    img = Image.fromarray(rgba, 'RGBA')
    while True:
        buf = io.BytesIO()
        img.save(buf, 'DDS', pixel_format='DXT5')
        levels.append(buf.getvalue())
        if img.width <= 4 or img.height <= 4:
            break
        img = img.resize((max(4, img.width // 2), max(4, img.height // 2)), Image.BOX)
    head = bytearray(levels[0][:128])
    struct.pack_into('<I', head, 8, struct.unpack_from('<I', head, 8)[0] | 0x20000)      # DDSD_MIPMAPCOUNT
    struct.pack_into('<I', head, 28, len(levels))
    struct.pack_into('<I', head, 108, struct.unpack_from('<I', head, 108)[0] | 0x400008)  # COMPLEX | MIPMAP
    return bytes(head) + b''.join(l[128:] for l in levels)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('car')
    ap.add_argument('dump')
    ap.add_argument('--out', help='where the maps go (default: the car\'s textures folder)')
    ap.add_argument('--preview', help='PNGs of the class map and the ORM channels here')
    ap.add_argument('--force', action='store_true', help='replace an ORM map already there')
    a = ap.parse_args()

    tex_dir = os.path.join(a.car, 'textures')
    out_dir = a.out or tex_dir
    by_tex = {}
    for line in open(a.dump, encoding='latin-1'):
        p = line.split()
        if len(p) != 10 or p[0] != 'tri':
            continue
        by_tex.setdefault(p[1], []).append((finish_of(p[2]), [float(x) for x in p[4:]]))

    for tex, tris in sorted(by_tex.items()):
        src = os.path.join(tex_dir, tex)
        dst = os.path.join(out_dir, os.path.splitext(tex)[0] + '_orm.dds')
        if os.path.exists(dst) and not a.force:
            print('%s: %s already there (kept; --force replaces it)' % (tex, os.path.basename(dst)))
            continue
        try:
            col = Image.open(src).convert('RGB')
        except OSError as e:
            print('%s: cannot read (%s)' % (tex, e))
            continue
        lab, finishes = class_map(col.size, tris)
        f = np.array([fi[1:] for fi in finishes], np.float32)   # roughness, metallic, coat
        rough, metal, coat = f[lab, 0], f[lab, 1], f[lab, 2]

        # Inside the paint: dark and grey is a grille, black trim, a seal - matte.
        srgb = np.asarray(col.filter(ImageFilter.GaussianBlur(DARK_BLUR))).astype(np.float32) / 255.0
        lin = np.where(srgb <= 0.04045, srgb / 12.92, ((srgb + 0.055) / 1.055) ** 2.4)
        lum = lin @ np.array([0.2126, 0.7152, 0.0722], np.float32)
        mx, mn = srgb.max(axis=2), srgb.min(axis=2)
        sat = np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0.0)
        dark = (1.0 - smoothstep(*DARK_LUM, lum)) * (1.0 - smoothstep(*DARK_SAT, sat)) * (coat > 0.5)
        coat = coat * (1.0 - dark)
        rough = rough + (0.6 - rough) * dark

        orm = np.zeros(lab.shape + (4,), np.uint8)
        orm[..., 0] = 255
        orm[..., 1] = np.clip(rough * 255 + 0.5, 0, 255)
        orm[..., 2] = np.clip(metal * 255 + 0.5, 0, 255)
        orm[..., 3] = np.clip(coat * 255 + 0.5, 0, 255)
        img = Image.fromarray(orm, 'RGBA').filter(ImageFilter.GaussianBlur(0.7))   # (no hard texel steps)
        os.makedirs(out_dir, exist_ok=True)
        open(dst, 'wb').write(dds_dxt5_with_mips(np.asarray(img)))
        share = {}
        for i, fi in enumerate(finishes):
            share[fi] = share.get(fi, 0) + int((lab == i).sum())
        print('%s -> %s: paint %.0f%% (%.0f%% of it made matte), coat on %.0f%% of the atlas' % (
            tex, os.path.basename(dst), 100.0 * (coat > -1).mean() * (f[lab, 2] > 0.5).mean(),
            100.0 * dark[f[lab, 2] > 0.5].mean() if (f[lab, 2] > 0.5).any() else 0.0, 100.0 * (coat > 0.5).mean()))
        if a.preview:
            os.makedirs(a.preview, exist_ok=True)
            stem = os.path.join(a.preview, os.path.splitext(tex)[0])
            palette = np.array([[40, 40, 40]] + [[int(255 * fi[3]), int(255 * fi[1]), int(255 * fi[2])]
                                                 for fi in finishes[1:]], np.uint8)
            Image.fromarray(palette[lab]).save(stem + '_classes.png')
            o = np.asarray(img)
            Image.fromarray(np.concatenate([o[..., 1], o[..., 2], o[..., 3]], axis=1)).save(stem + '_orm_g_b_a.png')
    return 0


if __name__ == '__main__':
    sys.exit(main())
