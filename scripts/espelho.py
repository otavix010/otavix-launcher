"""The launcher's files on its own server (Cloudflare R2, bucket otavix-arquivos), since the GitHub account was
suspended on 28/10/2026. The server answers the launcher's GitHub questions from "espelho/<path>.json" with
GitHub's own shape, and serves the files under /v1/arquivos/ (servidor/worker.js, "arquivo" and "github").

    py -3 -I scripts\\espelho.py [--so-gerar]

What goes up (edit the lists below for each new version):
- ports made from code ("branch"): GitHub's zip of origin/main (git archive, same top folder), its NOVIDADES.md and
  espelho/repos/<repo>/commits/main.json = {"sha", "zip_url"};
- ports shipped as a package ("release"): the zip and espelho/repos/<repo>/releases/latest.json;
- the launcher: its installer, releases/latest.json and the list the "Novidades" page reads (releases.lista.json);
- the community shader packs;
- fixed files the ports' generators download (FIXOS).
--so-gerar writes everything to out\\espelho without sending."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
SERVIDOR = 'https://otavix-launcher.otavix-launcher-servidor.workers.dev'
BUCKET = 'otavix-arquivos'
DONO = 'otavix010'
TMP = RAIZ / 'out' / 'espelho'
SCRATCH = Path(r'C:\Users\otavi\AppData\Local\Temp\claude\C--Users-otavi')

BRANCH = {  # repository: local clone (origin/main is what was published)
    'gow-ascension-pc': r'C:\Users\otavi\gow-ascension-pc',
    'asuras-wrath-pc': r'C:\Users\otavi\x360-ports\asuras-wrath',
    'dantes-inferno-pc': r'C:\Users\otavi\x360-ports\dantes-inferno',
    'dirt-2-pc': r'C:\Users\otavi\x360-ports\dirt-2',
    'saints-row-pc': r'C:\Users\otavi\x360-ports\saints-row',
    'silent-hill-pc': r'E:\RecompDev\silent-hill-pc',
}
RELEASE = {  # repository: (tag, package)
    'fifa-street-2-pc': ('v1.0.14', r'C:\Users\otavi\fifast2-port\fifast2\FIFAStreet2.zip'),
    'gow-betrayal-pc': ('v1.0.1', r'C:\Users\otavi\gowb-port\gowb-port-windows-x64.zip', r'C:\Users\otavi\gowb-port\gowb-port-linux-x64.tar.gz'),
    'infamous-second-son-pc': ('v1.0.10', r'C:\infamous\pacote-v1010\infamous-second-son-windows-x64.zip', r'C:\infamous\pacote-v1010\infamous-second-son-linux-x64.tar.gz'),
}
FIXOS = {  # key: local file; files the ports' generators download (the 360 ports' comum/gerar.ps1: the SDK)
    'releases/otavix010/rexglue-sdk-ffx/v0.10.0.24-ffx2/rexglue-sdk-0.10.0.24-ffx2-win-amd64.zip':
        r'C:\Users\otavi\x360-ports\rexglue-sdk-0.10.0.24-ffx2-win-amd64.zip',
    # inFAMOUS Second Son: the Linux/SteamOS package (a manual download; the launcher takes the .zip) and the build's source (GPL)
    'releases/otavix010/infamous-second-son-pc/v1.0.1/infamous-second-son-linux-x64.tar.gz':
        r'C:\infamous\pacote-linux\infamous-second-son-linux-x64.tar.gz',
    # the PS4 .pkg extractor the launcher downloads once (ShadPs4Plus PkgExtractor 1.1, built from shadPS4's package code)
    'ferramentas/ps4-pkg-extractor.zip': r'C:/infamous/tools/ps4-pkg-extractor.zip',
    'arquivos/infamous-second-son-fonte-6cec83cc.zip': r'C:\infamous\pacote-windows\infamous-second-son-fonte-6cec83cc.zip',
    'arquivos/infamous-second-son-fonte-ifpc-v1.0.1.zip': r'C:\infamous\pacote-windows-v2\infamous-second-son-fonte-ifpc-v1.0.1.zip',
    'arquivos/infamous-second-son-fonte-ifpc-v1.0.2.zip': r'C:\infamous\pacote-windows-v3\infamous-second-son-fonte-ifpc-v1.0.2.zip',
    'arquivos/infamous-second-son-fonte-ifpc-v1.0.3.zip': r'C:\infamous\pacote-windows-v4\infamous-second-son-fonte-ifpc-v1.0.3.zip',
    'arquivos/infamous-second-son-fonte-ifpc-v1.0.4.zip': r'C:\infamous\pacote-windows-v5\infamous-second-son-fonte-ifpc-v1.0.4.zip',
    'arquivos/infamous-second-son-fonte-ifpc-v1.0.5.zip': r'C:\infamous\pacote-windows-v6\infamous-second-son-fonte-ifpc-v1.0.5.zip',
    'arquivos/infamous-second-son-fonte-ifpc-v1.0.6.zip': r'C:\infamous\pacote-windows-v7\infamous-second-son-fonte-ifpc-v1.0.6.zip',
    'arquivos/infamous-second-son-fonte-ifpc-v1.0.8.zip': r'C:\infamous\pacote-windows-v10\infamous-second-son-fonte-ifpc-v1.0.8.zip',
    'arquivos/infamous-second-son-fonte-ifpc-v1.0.9.zip': r'C:\infamous\pacote-v109\infamous-second-son-fonte-ifpc-v1.0.9.zip',
    'arquivos/infamous-second-son-fonte-ifpc-v1.0.10.zip': r'C:\infamous\pacote-v1010\infamous-second-son-fonte-ifpc-v1.0.10.zip',
    'arquivos/fsr-vulkan-fonte.zip': r'C:\infamous\pacote-windows-v10\fsr-vulkan-fonte.zip',
}
ARTE = {  # game: its pictures on the server (arte/<game>/<file>), the profiles' [art] web: every player sees the game's photo
    'infamous-second-son': ['icon0.png', 'pic1.png'],
    'gow-betrayal': ['capa.png', 'fundo.png'],
}
ARTE_LOCAL = Path(r'C:\Users\otavi\Otavix Launcher\arte')
LAUNCHER = ['1.0.76', '1.0.75', '1.0.74', '1.0.73', '1.0.72', '1.0.71', '1.0.70', '1.0.69', '1.0.68', '1.0.67', '1.0.66', '1.0.65', '1.0.64', '1.0.63', '1.0.62', '1.0.61', '1.0.60', '1.0.59', '1.0.58', '1.0.57', '1.0.56', '1.0.55', '1.0.54', '1.0.53', '1.0.52', '1.0.51', '1.0.50']  # newest first; notes in docs/notas/<version>.md, installer in out
SHADERS = SCRATCH / 'f30c7acd-0366-4290-94ef-e4dec4da0c98' / 'scratchpad' / 'pacotes'
SHADERS_REPOS = {'gow-ascension': 'gow-ascension-pc', 'asuras-wrath': 'asuras-wrath-pc', 'dantes-inferno': 'dantes-inferno-pc',
                 'dirt-2': 'dirt-2-pc', 'saints-row': 'saints-row-pc'}

# shadPS4 ports: the community packs, one zip a kind of graphics card plus shaders-index.txt (made and checked by
# C:\infamous	ools\pacote_shaders_ps4.py on the owner's PC), in a folder a game; they go up as they are
SHADPS4 = {'infamous-second-son-pc': Path(r'C:\infamous\shaders-comunidade\CUSA00004')}

envios = []  # (key, local file)


def url(key):
    return f'{SERVIDOR}/v1/arquivos/{key}'


def guardar(key, dados):
    p = TMP / key
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(dados if isinstance(dados, bytes) else dados.encode('utf-8'))
    envios.append((key, p))


def git(pasta, *a, binario=False):
    return subprocess.run(['git', '-C', pasta, *a], check=True, capture_output=True).stdout if binario else \
        subprocess.run(['git', '-C', pasta, *a], check=True, capture_output=True, text=True, encoding='utf-8').stdout.strip()


def asset(nome, arquivo, key):
    dados = Path(arquivo).read_bytes()
    envios.append((key, Path(arquivo)))
    # the launcher reads name, size and digest BEFORE browser_download_url: keep this order
    return {'name': nome, 'size': len(dados), 'digest': 'sha256:' + hashlib.sha256(dados).hexdigest(),
            'browser_download_url': url(key)}


def main():
    for repo, pasta in BRANCH.items():
        sha = git(pasta, 'rev-parse', 'origin/main')
        zkey = f'ports/{DONO}/{repo}/{sha}.zip'
        guardar(zkey, git(pasta, 'archive', '--format=zip', f'--prefix={repo}-{sha}/', 'origin/main', binario=True))
        try:
            guardar(f'ports/{DONO}/{repo}/main/NOVIDADES.md', git(pasta, 'show', 'origin/main:NOVIDADES.md', binario=True))
        except subprocess.CalledProcessError:
            pass
        guardar(f'espelho/repos/{DONO}/{repo}/commits/main.json', json.dumps({'sha': sha, 'zip_url': url(zkey)}))
        print(f'{repo}: {sha[:12]}')
    for repo, (tag, pacote, *linux) in RELEASE.items():
        a = asset(Path(pacote).name, pacote, f'releases/{DONO}/{repo}/{tag}/{Path(pacote).name}')
        # the Linux / SteamOS package, when there is one (its name carries "linux": each system takes its own)
        assets = [a] + [asset(Path(p).name, p, f'releases/{DONO}/{repo}/{tag}/{Path(p).name}') for p in linux if p and Path(p).exists()]
        # the ports' notes (docs/notas-portes/<repo>/<tag>.md, as the launcher's): the "What's new" page reads them from here
        notas = {f.stem: f.read_text(encoding='utf-8') for f in (RAIZ / 'docs' / 'notas-portes' / repo).glob('v*.md')}
        ordem = sorted(notas, key=lambda v: [int(x) for x in v.lstrip('v').split('.')], reverse=True)
        guardar(f'espelho/repos/{DONO}/{repo}/releases/latest.json', json.dumps({'tag_name': tag, 'name': tag, 'body': notas.get(tag, ''), 'published_at': '', 'html_url': '', 'assets': assets}, ensure_ascii=False))
        if notas:
            guardar(f'espelho/repos/{DONO}/{repo}/releases.lista.json', json.dumps([{'tag_name': v, 'name': v, 'published_at': '', 'body': notas[v], 'html_url': ''} for v in ordem], ensure_ascii=False))
        print(f'{repo}: {tag}')
    lista = []
    for i, versao in enumerate(LAUNCHER):
        notas = RAIZ / 'docs' / 'notas' / f'{versao}.md'
        exe = RAIZ / 'out' / f'Instalar-Otavix-Launcher-{versao}.exe'
        corpo = notas.read_text(encoding='utf-8') if notas.exists() else ''
        rel = {'tag_name': 'v' + versao, 'name': versao, 'published_at': '', 'body': corpo, 'html_url': ''}
        if i == 0:
            rel['assets'] = [asset(exe.name, exe, f'releases/{DONO}/otavix-launcher/v{versao}/{exe.name}')]
            tgz = RAIZ / 'out' / f'Otavix-Launcher-{versao}-linux-x64.tar.gz'   # built on the Legion (scripts/pacote_linux.sh)
            if tgz.exists():
                rel['assets'].append(asset(tgz.name, tgz, f'releases/{DONO}/otavix-launcher/v{versao}/{tgz.name}'))
                guardar(f'launcher/{tgz.name}', tgz.read_bytes())
            guardar(f'espelho/repos/{DONO}/otavix-launcher/releases/latest.json', json.dumps(rel, ensure_ascii=False))
            guardar(f'launcher/{exe.name}', exe.read_bytes())   # the download page's link
        lista.append({k: v for k, v in rel.items() if k != 'assets'})
    guardar(f'espelho/repos/{DONO}/otavix-launcher/releases.lista.json', json.dumps(lista, ensure_ascii=False))
    print(f'launcher: {LAUNCHER[0]}')
    for key, arquivo in FIXOS.items():
        envios.append((key, Path(arquivo)))
    for jogo, arquivos in ARTE.items():
        for nome in arquivos:
            envios.append((f'arte/{jogo}/{nome}', ARTE_LOCAL / jogo / nome))
    for jogo, repo in ({} if '--sem-shaders' in sys.argv else SHADERS_REPOS).items():  # local packs may be older than the server's
        for nome in ('shaders.zip', 'shaders.zip.sha256'):
            f = SHADERS / jogo / nome
            if f.exists():
                envios.append((f'shaders/{DONO}/{repo}/{nome}', f))
    for repo, pasta in ({} if '--sem-shaders' in sys.argv else SHADPS4).items():
        if (pasta / 'shaders-index.txt').exists():
            for f in sorted(pasta.iterdir()):
                if f.is_file() and (f.name == 'shaders-index.txt' or (f.name.startswith('shaders-') and f.suffix == '.zip')):
                    envios.append((f'shaders/{DONO}/{repo}/{f.name}', f))
    if '--so-gerar' in sys.argv:
        print(f'{len(envios)} arquivos em {TMP}')
        return
    falhas = 0
    for key, arquivo in envios:
        p = subprocess.run(['npx.cmd', 'wrangler', 'r2', 'object', 'put', f'{BUCKET}/{key}', '--file', str(arquivo), '--remote', *(['--content-type', 'image/png' if str(arquivo).lower().endswith('.png') else 'image/jpeg'] if str(arquivo).lower().endswith(('.png', '.jpg')) and key.startswith('arte/') else [])],
                           cwd=RAIZ / 'servidor', capture_output=True, text=True, encoding='utf-8', errors='replace')
        if p.returncode:
            falhas += 1
            print(f'FALHOU {key}: {(p.stdout + p.stderr)[-300:]}')
    print(f'{len(envios) - falhas}/{len(envios)} enviados')


main()
