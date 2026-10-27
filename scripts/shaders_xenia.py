"""The community shader pack of a ReXGlue port (a 360 game): the shareable storage files players' launchers sent,
merged with the current pack into shaders.zip (with shaders.zip.sha256 beside it) for the port's release "shaders".

Each storage file is <TITLEID>.xsh (guest shaders) or <TITLEID>.<rtv|rov>.d3d12.xpso (pipelines). ReXGlue truncates a
file to nothing when its header is not its own, and stops at the first record whose XXH3 is wrong, dropping
everything after it: so files are grouped by name and exact header, every record is checked, and only whole, good
records go in, once each (by hash). The header kept for a name is the newest version among the inputs.

  python shaders_xenia.py <out.zip> <input> [<input> ...]
An input is a storage file, a folder holding them (searched below), or a shaders.zip made before. Needs xxhash
(pip install xxhash)."""
import hashlib
import io
import os
import re
import struct
import sys
import zipfile

import xxhash

NAME = re.compile(r'^[0-9A-Fa-f]{8}(\.xsh|\.(rtv|rov)\.d3d12\.xpso)$')


def head_size(name):
    return 8 if name.lower().endswith('.xsh') else 12


def version(name, head):
    # the version is stored byte-swapped (big-endian) in the last 4 bytes of the header
    return struct.unpack('>I', head[-4:])[0]


def records_xsh(data):
    """{u64 XXH3 of the ucode, u32 dwords:31 | type:1} + ucode; the good ones until the first bad (as ReXGlue)"""
    out, at = [], 8
    while at + 12 <= len(data):
        h, w = struct.unpack_from('<QI', data, at)
        size = 12 + (w & 0x7FFFFFFF) * 4
        if at + size > len(data):
            break
        if xxhash.xxh3_64_intdigest(data[at + 12:at + size]) != h:
            break
        out.append((h, data[at:at + size]))
        at += size
    return out


def record_size_xpso(data):
    """the pipelines' record size: the one whose first record's description hashes to its stored hash"""
    if len(data) < 12 + 16:
        return 0
    h = struct.unpack_from('<Q', data, 12)[0]
    for size in range(16, min(len(data) - 12, 65536) + 1, 4):
        if (len(data) - 12) % size == 0 and xxhash.xxh3_64_intdigest(data[20:12 + size]) == h:
            return size
    return 0


def records_xpso(data, size):
    out = []
    for at in range(12, len(data) - size + 1, size):
        h = struct.unpack_from('<Q', data, at)[0]
        if xxhash.xxh3_64_intdigest(data[at + 8:at + size]) != h:
            break
        out.append((h, data[at:at + size]))
    return out


def inputs(paths):
    """(name, bytes) of every storage file in the inputs"""
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for f in files:
                    if NAME.match(f):
                        yield f, open(os.path.join(root, f), 'rb').read()
        elif p.lower().endswith('.zip'):
            with zipfile.ZipFile(p) as z:
                for n in z.namelist():
                    f = n.rsplit('/', 1)[-1]
                    if NAME.match(f):
                        yield f, z.read(n)
        elif NAME.match(os.path.basename(p)):
            yield os.path.basename(p), open(p, 'rb').read()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = sys.argv[1]
    groups = {}  # name -> header -> [bytes]
    for name, data in inputs(sys.argv[2:]):
        hs = head_size(name)
        if len(data) < hs:
            continue
        groups.setdefault(name.upper().replace('.XSH', '.xsh').replace('.D3D12.XPSO', '.d3d12.xpso')
                          .replace('.RTV', '.rtv').replace('.ROV', '.rov'), {}).setdefault(data[:hs], []).append(data)
    files, index, bad_total = {}, {}, 0
    for name, by_head in sorted(groups.items()):
        head = max(by_head, key=lambda hd: version(name, hd))  # the newest version: the older ones ReXGlue drops
        seen, body, size = set(), [], 0
        for data in by_head[head]:
            if name.endswith('.xsh'):
                recs = records_xsh(data)
                total = len(data) > 8
            else:
                s = record_size_xpso(data)
                if not s or (size and s != size):
                    bad_total += 1
                    continue
                size = s
                recs = records_xpso(data, s)
            for h, rec in recs:
                if h not in seen:
                    seen.add(h)
                    body.append(rec)
        if not body:
            continue
        files[name] = head + b''.join(body)
        index[name] = {'registros': len(body), 'registro': size}
        skipped = sum(len(v) for hd, v in by_head.items() if hd != head)
        print(f'{name}: {len(body)} registros{f" (registro {size} bytes)" if size else ""}'
              f'{f", {skipped} arquivo(s) de outra versão ignorado(s)" if skipped else ""}')
    if not files:
        sys.exit('nenhum registro bom nas entradas')
    toml = '# o pacote de shaders da comunidade: registros por arquivo e, nos .xpso, o tamanho de cada registro\n'
    for name, v in index.items():
        toml += f'\n[arquivos."{name}"]\nregistros = {v["registros"]}\nregistro = {v["registro"]}\n'
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(name, data)
        z.writestr('indice.toml', toml)
    open(out, 'wb').write(buf.getvalue())
    sha = hashlib.sha256(buf.getvalue()).hexdigest()
    open(out + '.sha256', 'w').write(f'{sha}  {os.path.basename(out)}\n')
    print(f'{out}: {len(files)} arquivo(s), {len(buf.getvalue())} bytes, sha256 {sha}'
          f'{f" ({bad_total} .xpso sem registro reconhecível)" if bad_total else ""}')


if __name__ == '__main__':
    main()
