"""rbr-tracks.yaml from the onlineracing.cz tracks table (docs/content-layers.md 3): the W-Rally tracks, each with its
STAGES - every RBR map number the table knows, to its track and its stage id.

    python make_table.py <rbr_ct_tracks.csv> <rbr-tracks.yaml> [--rbr <RBR folder>]
    python make_table.py --regroup <rbr-tracks.yaml>      rewrite a table (either form) with the stages grouped

A track is a place: its id <country>_<stagename>. Its stages are the RBR maps driven there - RBR's reversed stages ("X
II", decided 2026-11-08) are the same track as X: Joux Verte (42) and Joux Verte II (45) are fr_joux_verte with the stages
joux_verte and joux_verte_ii. A map is grouped under another when its id is the other's plus _ii, _reverse, _reversed or
_rev (or _ii_a: 70, Prospect Ridge II A), or the other's _i plus _ii; the chain is followed to its root, whose id names
the track. A stage id is the map's own id without its country.

The CSV is the table's export with its header (stage_id, ingame_stage_id, name, ..., country_ENG, ...): the map is
ingame_stage_id. The country is ISO 3166-1 alpha-2 (the table's English name mapped, its spellings as they are); one
with no real country (Montekland, Rally school, Neverwhere) is "xx", ISO's code for private use. The name is a slug: ASCII,
lowercase, a-z 0-9 and _, a release number at its end dropped ("Bergheim v1.1": bergheim). Two maps with the same id:
the later ones get _<map>.

--rbr: the folder's Tracks.ini names checked against the table's, and the maps it has that the table does not - printed,
nothing changed (where the folder's number is another track, the table wins: decided 2026-11-05).
"""
import argparse
import collections
import csv
import os
import re
import sys
import unicodedata

CC = {'Argentina': 'ar', 'Australia': 'au', 'Austria': 'at', 'Brasil': 'br', 'Czech Republic': 'cz', 'Estonia': 'ee',
      'Finland': 'fi', 'France': 'fr', 'Germany': 'de', 'Great Britain': 'gb', 'Hungary': 'hu', 'Ireland': 'ie',
      'Itally': 'it', 'Italy': 'it', 'Japan': 'jp', 'Lithuania': 'lt', 'Netherlands': 'nl', 'New Zealand': 'nz',
      'Poland': 'pl', 'Portugal': 'pt', 'San Marino': 'sm', 'Slovakia': 'sk', 'Slovenia': 'si', 'Spain': 'es',
      'Sweeden': 'se', 'Swiss': 'ch', 'Switzerland': 'ch', 'USA': 'us', 'Ukraine': 'ua',
      'Montekland': 'xx', 'Neverwhere': 'xx', 'Rally School': 'xx', 'Rally school': 'xx'}
# The table's names corrected: RBR's own spelling.
FIX = {60: 'Fraizer Wells II'}


def slug(name):
    s = unicodedata.normalize('NFKD', name).encode('ascii', 'ignore').decode().lower().replace("'", '')
    s = re.sub(r'\s+v?\d+\.\d+(\.\d+)*$', '', s)
    return re.sub(r'_+', '_', re.sub(r'[^a-z0-9]', '_', s)).strip('_')


def tracks_ini_names(rbr):
    names, cur = {}, None
    for line in open(os.path.join(rbr, 'Maps', 'Tracks.ini'), encoding='latin-1'):
        line = line.split(';')[0].strip()
        m = re.match(r'\[Map(\d+)\]', line, re.I)
        if m:
            cur = int(m.group(1))
        elif cur is not None and '=' in line and line.split('=')[0].strip().lower() == 'stagename':
            names[cur] = line.split('=', 1)[1].strip().strip('"').strip()
    return names


SUFFIXES = ('_ii_a', '_ii', '_reversed', '_reverse', '_rev')


def group(ids):
    """{map: id} -> {track id: [(map, stage id)]}, the tracks in the order of their first map."""
    by_id = {i: m for m, i in ids.items()}

    def base(i):
        for suf in SUFFIXES:
            if i.endswith(suf):
                b = i[:-len(suf)]
                if b in by_id:
                    return b
                if suf == '_ii' and b + '_i' in by_id:   # Junior Wheels I and II
                    return b + '_i'
        return None

    tracks = {}
    for m in sorted(ids):
        root = ids[m]
        seen = {root}
        while base(root) and base(root) not in seen:
            root = base(root)
            seen.add(root)
        stage = ids[m].split('_', 1)[1] if '_' in ids[m] else ids[m]
        tracks.setdefault(root, []).append((m, stage))
    # the track's own stage (the root) first - it is the track's main stage - then the others by map number
    for t, stages in tracks.items():
        stages.sort(key=lambda ms: (ids[ms[0]] != t, ms[0]))
    return dict(sorted(tracks.items(), key=lambda kv: min(m for m, _ in kv[1])))


def read_table(path):
    """A table of either form: "41: cz_test" lines, or tracks with their stages."""
    ids, track = {}, None
    for line in open(path, encoding='utf-8'):
        text = line.split('#')[0].rstrip()
        if not text.strip():
            continue
        indented = text[0] in ' \t'
        key, _, value = text.strip().partition(':')
        key, value = key.strip(), value.strip()
        if not indented and key.isdigit():          # the old form
            ids[int(key)] = value
        elif not indented:                           # a track
            track = key
        elif track and key.isdigit():               # one of its stages: the map's id is <country>_<stage>
            ids[int(key)] = track.split('_', 1)[0] + '_' + value
    return ids


def write(path, ids):
    tracks = group(ids)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# W-Rally track id (<country>_<place>): its stages, RBR map number: stage id - tools/rbr-tracks/make_table.py\n')
        f.write('# RBR\'s reversed stages are stages of the same track (docs/content-layers.md 3).\n')
        for t, stages in tracks.items():
            f.write(f'{t}:\n')
            for m, st in stages:
                f.write(f'  {m}: {st}\n')
    several = sum(1 for s in tracks.values() if len(s) > 1)
    print(f'{len(ids)} maps, {len(tracks)} tracks ({several} with more than one stage) to {path}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('csv', nargs='?')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--rbr')
    ap.add_argument('--regroup', metavar='YAML')
    a = ap.parse_args()

    if a.regroup:
        ids = read_table(a.regroup)
        write(a.regroup, ids)
        return
    if not a.csv or not a.out:
        ap.error('a CSV and an output, or --regroup')

    table = {}
    for r in csv.DictReader(open(a.csv, encoding='utf-8')):
        m = int(r['ingame_stage_id'])
        table[m] = (FIX.get(m, r['name'].strip()), r['country_ENG'].strip())

    ids = {}
    for m, (name, country) in sorted(table.items()):
        if country not in CC:
            print(f'map {m} {name}: country "{country}" has no code - left out', file=sys.stderr)
            continue
        ids[m] = CC[country] + '_' + slug(name)
    shared = collections.defaultdict(list)
    for m, i in sorted(ids.items()):
        shared[i].append(m)
    for i, maps in shared.items():
        for m in maps[1:]:
            ids[m] = f'{i}_{m}'
            print(f'map {m}: {i} is map {maps[0]}\'s too - {ids[m]}', file=sys.stderr)

    write(a.out, ids)

    if a.rbr:
        local = tracks_ini_names(a.rbr)
        here = set()
        for f in os.listdir(os.path.join(a.rbr, 'Maps')):
            m = re.match(r'track-(\d+)[_.].*\.(trk|lbs)$', f, re.I)
            if m:
                here.add(int(m.group(1)))
        for m in sorted(here & set(ids)):
            if m in local and not slug(local[m]).startswith(slug(table[m][0])) and not slug(table[m][0]).startswith(slug(local[m])):
                print(f'map {m}: {a.rbr} has "{local[m]}", the table "{table[m][0]}" ({ids[m]})')
        missing = sorted(here - set(ids))
        print(f'{len(missing)} maps in {a.rbr} the table does not know (rbr_<n>): ' +
              ', '.join(f'{m}' + (f' "{local[m]}"' if m in local else '') for m in missing))


if __name__ == '__main__':
    main()
