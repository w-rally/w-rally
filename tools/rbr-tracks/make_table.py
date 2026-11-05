"""rbr-tracks.json from the onlineracing.cz tracks table (docs/content-layers.md 3): every RBR map number the table knows
to its W-Rally track id, <country>_<stagename>.

    python make_table.py <rbr_ct_tracks.csv> <rbr-tracks.json> [--rbr <RBR folder>]

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
import json
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('csv')
    ap.add_argument('out')
    ap.add_argument('--rbr')
    a = ap.parse_args()

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

    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump({str(m): ids[m] for m in sorted(ids)}, f, indent=2)
        f.write('\n')
    print(f'{len(ids)} tracks to {a.out}')

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
