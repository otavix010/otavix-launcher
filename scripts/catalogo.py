"""Builds the launcher's game catalogs: one line a game (its regions joined), the name to show and where its box art is.
Most consoles come from libretro's box art lists; the PlayStation 3 from GameTDB (names and covers by title id); the
Xbox 360 from Redump's disc list (names only: no free box art).

    py catalogo.py saida_dir
"""
import html
import io
import os
import re
import sys
import unicodedata
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

LIBRETRO = {
    'xbox': 'Microsoft - Xbox',
    'ps1': 'Sony - PlayStation',
    'ps2': 'Sony - PlayStation 2',
    'psp': 'Sony - PlayStation Portable',
    'vita': 'Sony - PlayStation Vita',
    'gc': 'Nintendo - GameCube',
    'wii': 'Nintendo - Wii',
    'wiiu': 'Nintendo - Wii U',
    'nds': 'Nintendo - Nintendo DS',
    '3ds': 'Nintendo - Nintendo 3DS',
}
# versions that are not the game as sold
SKIP = re.compile(r'\((?:[^)]*\b(?:Demo|Beta|Proto|Prototype|Sample|Kiosk|Preview|Debug|Trial|Promo|Taikenban|Alpha|Pirate|Unl|Aftermarket|Program|Test)\b[^)]*)\)', re.I)
REGIONS = ['USA', 'World', 'Europe', 'Brazil', 'Australia', 'UK', 'Canada', 'Japan', 'Korea', 'Asia']


def fetch(url):
    req = urllib.request.Request(url, headers={'User-Agent': 'Otavix-Launcher-catalogo'})
    with urllib.request.urlopen(req, timeout=300) as r:
        return r.read()


def base_title(name):
    t = re.sub(r'\s*[\(\[].*$', '', name).strip()
    # libretro writes &*/:`<>?\| as _
    t = t.replace(' _ ', ' & ').replace('_ ', ': ')
    # "Legend of Zelda, The - Twilight Princess" -> "The Legend of Zelda - Twilight Princess"
    m = re.match(r'^(.*?), (The|A|An)( - .*)?$', t)
    if m:
        t = f'{m.group(2)} {m.group(1)}{m.group(3) or ""}'
    return t


def rank(name):
    tags = re.findall(r'\(([^)]*)\)', name)
    region = tags[0] if tags else ''
    for i, r in enumerate(REGIONS):
        if r in region:
            return (i, len(name))
    return (len(REGIONS), len(name))


def key(t):
    t = unicodedata.normalize('NFD', t)
    t = ''.join(c for c in t if unicodedata.category(c) != 'Mn').lower()
    return re.sub(r'[^a-z0-9]+', ' ', t).strip()


def write(out, cid, source, rows):
    rows = sorted(rows, key=lambda r: key(r[0]))
    with open(os.path.join(out, cid + '.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'# {source}: {len(rows)} jogos\n')
        for t, n in rows:
            f.write(f'{t}\t{n}\n')
    print(f'{cid:5} -> {len(rows):6} jogos ({source})')


def by_names(names):
    best = {}
    for n in names:
        if SKIP.search(n):
            continue
        t = base_title(n)
        k = key(t)
        if k and (k not in best or rank(n) < rank(best[k][1])):
            best[k] = (t, n)
    return best


def libretro(out, cid, system):
    page = fetch('https://thumbnails.libretro.com/' + urllib.parse.quote(system) + '/Named_Boxarts/').decode('utf-8', 'replace')
    names = [html.unescape(urllib.parse.unquote(h))[:-4] for h in re.findall(r'<a href="([^"?/][^"]*\.png)"', page)]
    write(out, cid, f'{system} (capas thumbnails.libretro.com)', list(by_names(names).values()))


# GameTDB: the id's letters say the region, and the region its cover folder
def ps3(out):
    z = zipfile.ZipFile(io.BytesIO(fetch('https://www.gametdb.com/ps3tdb.zip?LANG=EN')))
    root = ET.fromstring(z.read('ps3tdb.xml'))
    order = ['BLUS', 'BCUS', 'NPUB', 'NPUA', 'BLES', 'BCES', 'NPEB', 'NPEA', 'BLJM', 'BLJS', 'BCJS', 'NPJB', 'NPJA']
    folder = {'U': 'US', 'E': 'EN', 'J': 'JA', 'A': 'EN', 'K': 'KO', 'H': 'EN'}
    best = {}
    for g in root.iter('game'):
        if (g.findtext('type') or '') not in ('PS3', 'SEN'):
            continue
        gid = g.findtext('id') or ''
        title = ''
        for loc in g.iter('locale'):
            if loc.get('lang') == 'EN':
                title = (loc.findtext('title') or '').strip()
        if not title or re.search(r'\b(Demo|Trial|Theme|Avatar|Beta|Soundtrack|OST|Directors Live|Trailer|Video|Wallpaper)\b', title, re.I):
            continue
        k = key(title)
        pr = order.index(gid[:4]) if gid[:4] in order else len(order)
        if k and (k not in best or pr < best[k][0]):
            best[k] = (pr, title, f'gametdb:{folder.get(gid[2:3], "EN")}/{gid}')
    write(out, 'ps3', 'GameTDB (capas art.gametdb.com)', [(t, c) for _, t, c in best.values()])


def x360(out):
    z = zipfile.ZipFile(io.BytesIO(fetch('http://redump.org/datfile/xbox360/')))
    root = ET.fromstring(z.read(z.namelist()[0]))
    names = [g.get('name') for g in root.iter('game') if g.findtext('category') == 'Games']
    write(out, 'x360', 'Redump (sem capas)', [(t, '') for t, _ in by_names(names).values()])


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    ps3(out)
    x360(out)
    for cid, system in LIBRETRO.items():
        libretro(out, cid, system)


if __name__ == '__main__':
    main()
