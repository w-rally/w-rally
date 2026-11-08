"""rbr-tracks.yaml from the onlineracing.cz tracks table (docs/content-layers.md 3): the W-Rally tracks, each with its
name and country and its STAGES - every RBR map number the table knows, with its stage id, name, length and surface.

    python make_table.py <rbr_ct_tracks.csv> <rbr-tracks.yaml> [--rbr <RBR folder>]
    python make_table.py --regroup <rbr-tracks.yaml>      rewrite a table (any of its forms) with the stages grouped

A track is a place: its id <country>_<place>. Its stages are the RBR maps driven there - RBR's reversed stages ("X
II", decided 2026-11-08) are the same track as X: Joux Verte (42) and Joux Verte II (45) are fr_joux_verte with the stages
joux_verte and joux_verte_ii. A map is grouped under another when its id is the other's plus _ii, _reverse, _reversed or
_rev (or _ii_a: 70, Prospect Ridge II A), or the other's _i plus _ii; the chain is followed to its root, whose id names
the track and whose name and country are the track's. A stage id is the map's own id without its country.

    fr_joux_verte:
      name: "Joux Verte"
      country: "France"
      42:
        id: joux_verte
        name: "Joux Verte"
        length_km: 7.9
        surface: tarmac
      45:
        ...

The CSV is the table's export with its header (stage_id, ingame_stage_id, name, length, ..., country_ENG, ...,
surface, ...): the map is ingame_stage_id; surface 0 tarmac, 1 gravel, 2 snow. The country code is ISO 3166-1 alpha-2
(the table's English name mapped); one with no real country (Montekland, Rally school, Neverwhere) is "xx", ISO's code
for private use. The id's name is a slug: ASCII, lowercase, a-z 0-9 and _, a release number at its end dropped
("Bergheim v1.1": bergheim). Two maps with the same id: the later ones get _<map>. The names shown are the table's, its
country names' spellings corrected (COUNTRY).

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
# The table's country names as shown: its spellings corrected.
COUNTRY = {'Itally': 'Italy', 'Sweeden': 'Sweden', 'Swiss': 'Switzerland', 'Brasil': 'Brazil', 'Rally school': 'Rally School'}
SURFACE = {'0': 'tarmac', '1': 'gravel', '2': 'snow'}
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


def group(maps):
    """{map: {'id': ..., ...}} -> {track id: [map, ...]}, the track's own stage first, the tracks in the order of their
    first map."""
    by_id = {s['id']: m for m, s in maps.items()}

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
    for m in sorted(maps):
        root = maps[m]['id']
        seen = {root}
        while base(root) and base(root) not in seen:
            root = base(root)
            seen.add(root)
        tracks.setdefault(root, []).append(m)
    # the track's own stage (the root) first - it is the track's main stage - then the others by map number
    for t, ms in tracks.items():
        ms.sort(key=lambda m: (maps[m]['id'] != t, m))
    return dict(sorted(tracks.items(), key=lambda kv: min(kv[1])))


def unquote(v):
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] == '"':
        return v[1:-1].replace('\\"', '"').replace('\\\\', '\\')
    return v


def read_table(path):
    """A table of any of its forms -> {map: {'id', 'name', 'length_km', 'surface', 'country'}}: "41: cz_test" lines,
    tracks with "<map>: <stage id>", or tracks with their fields and stages with theirs."""
    maps, track, fields, stage = {}, None, {}, None
    for line in open(path, encoding='utf-8'):
        text = line.rstrip('\n')
        if not text.strip() or text.lstrip().startswith('#'):
            continue
        indent = len(text) - len(text.lstrip(' '))
        key, _, value = text.strip().partition(':')
        key, value = key.strip(), unquote(value)
        country = lambda: track.split('_', 1)[0]
        if indent == 0 and key.isdigit():                        # the oldest form: a map, its track
            maps[int(key)] = {'id': value}
            track = stage = None
        elif indent == 0:                                         # a track
            track, fields, stage = key, {}, None
        elif indent == 2 and track and key.isdigit():           # one of its stages
            stage = int(key)
            maps[stage] = {'id': country() + '_' + value} if value else {}
            maps[stage].update({'country': fields.get('country', '')})
        elif indent == 2 and track:                               # the track's own field
            fields[key] = value
            for m, s in maps.items():                             # (fields come before the stages, but in case)
                if s.get('_track') == track and key == 'country':
                    s['country'] = value
        elif indent == 4 and stage is not None:                  # the stage's field
            maps[stage][key] = country() + '_' + value if key == 'id' else value
        if stage is not None and stage in maps:
            maps[stage]['_track'] = track
    for s in maps.values():
        s.pop('_track', None)
    return maps


def quoted(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def write(path, maps):
    tracks = group(maps)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# W-Rally tracks (<country>_<place>): a name, a country, and its stages - each an RBR map number with its stage\n')
        f.write('# id, name, length and surface. RBR\'s reversed stages are stages of the same track (docs/content-layers.md 3).\n')
        f.write('# Written by tools/rbr-tracks/make_table.py from the onlineracing.cz tracks table.\n')
        for t, ms in tracks.items():
            root = maps[ms[0]]
            f.write(f'{t}:\n')
            if root.get('name'):
                f.write(f'  name: {quoted(root["name"])}\n')
            if root.get('country'):
                f.write(f'  country: {quoted(root["country"])}\n')
            for m in ms:
                s = maps[m]
                f.write(f'  {m}:\n')
                f.write(f'    id: {s["id"].split("_", 1)[1] if "_" in s["id"] else s["id"]}\n')
                if s.get('name'):
                    f.write(f'    name: {quoted(s["name"])}\n')
                if s.get('length_km'):
                    f.write(f'    length_km: {s["length_km"]}\n')
                if s.get('surface'):
                    f.write(f'    surface: {s["surface"]}\n')
    several = sum(1 for s in tracks.values() if len(s) > 1)
    print(f'{len(maps)} maps, {len(tracks)} tracks ({several} with more than one stage) to {path}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('csv', nargs='?')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--rbr')
    ap.add_argument('--regroup', metavar='YAML')
    a = ap.parse_args()

    if a.regroup:
        write(a.regroup, read_table(a.regroup))
        return
    if not a.csv or not a.out:
        ap.error('a CSV and an output, or --regroup')

    table = {}
    for r in csv.DictReader(open(a.csv, encoding='utf-8')):
        m = int(r['ingame_stage_id'])
        table[m] = r
        r['name'] = FIX.get(m, r['name'].strip())
        r['country_ENG'] = r['country_ENG'].strip()

    maps = {}
    for m, r in sorted(table.items()):
        country = r['country_ENG']
        if country not in CC:
            print(f'map {m} {r["name"]}: country "{country}" has no code - left out', file=sys.stderr)
            continue
        s = {'id': CC[country] + '_' + slug(r['name']), 'name': r['name'], 'country': COUNTRY.get(country, country)}
        try:
            if float(r['length']) > 0:
                s['length_km'] = f'{float(r["length"]):g}'
        except ValueError:
            pass
        if r.get('surface') in SURFACE:
            s['surface'] = SURFACE[r['surface']]
        maps[m] = s
    shared = collections.defaultdict(list)
    for m, s in sorted(maps.items()):
        shared[s['id']].append(m)
    for i, ms in shared.items():
        for m in ms[1:]:
            maps[m]['id'] = f'{i}_{m}'
            print(f'map {m}: {i} is map {ms[0]}\'s too - {maps[m]["id"]}', file=sys.stderr)

    write(a.out, maps)

    if a.rbr:
        local = tracks_ini_names(a.rbr)
        here = set()
        for f in os.listdir(os.path.join(a.rbr, 'Maps')):
            mm = re.match(r'track-(\d+)[_.].*\.(trk|lbs)$', f, re.I)
            if mm:
                here.add(int(mm.group(1)))
        for m in sorted(here & set(maps)):
            if m in local and not slug(local[m]).startswith(slug(table[m]['name'])) and not slug(table[m]['name']).startswith(slug(local[m])):
                print(f'map {m}: {a.rbr} has "{local[m]}", the table "{table[m]["name"]}" ({maps[m]["id"]})')
        missing = sorted(here - set(maps))
        print(f'{len(missing)} maps in {a.rbr} the table does not know (rbr_<n>): ' +
              ', '.join(f'{m}' + (f' "{local[m]}"' if m in local else '') for m in missing))


if __name__ == '__main__':
    main()
