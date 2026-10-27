"""The shaders players' launchers shared, from the server's database (through wrangler, already logged in) into
<out>/<game>/<sender>/<file>, to be checked and merged into each game's pack (scripts/shaders_xenia.py for the 360
ports; the GoW's own scripts/pacote_shaders.py). Only what came after the last run (<out>/ultimo_id.txt); a storage
file a sender sent again, grown, replaces its older copy.

  python shaders_coletar.py <out> [--jogo <id>]"""
import json
import os
import subprocess
import sys

SERVIDOR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'servidor')


def consulta(sql):
    p = subprocess.run(['npx.cmd', 'wrangler', 'd1', 'execute', 'otavix-launcher', '--remote', '--json', '--command', sql],
                       cwd=SERVIDOR, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=300)
    if p.returncode != 0:
        sys.exit('falhou a consulta ao banco: ' + (p.stderr or p.stdout)[-400:])
    return json.loads(p.stdout)[0]['results']


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    jogo = sys.argv[sys.argv.index('--jogo') + 1] if '--jogo' in sys.argv else ''
    os.makedirs(out, exist_ok=True)
    marca = os.path.join(out, 'ultimo_id.txt')
    desde = int(open(marca).read().strip() or 0) if os.path.exists(marca) else 0
    filtro = f" AND jogo = '{jogo}'" if jogo and jogo.replace('-', '').isalnum() else ''
    linhas = consulta(f'SELECT id, jogo, arquivo, install, tamanho FROM shaders WHERE id > {desde}{filtro} ORDER BY id')
    novos = 0
    ultimo = desde
    for r in linhas:
        # a few at a time: each answer carries the file as hex
        dados = consulta(f"SELECT hex(dados) AS h FROM shaders WHERE id = {int(r['id'])}")
        if not dados:
            continue
        pasta = os.path.join(out, r['jogo'], r['install'][:12])
        os.makedirs(pasta, exist_ok=True)
        nome = os.path.basename(r['arquivo'])
        open(os.path.join(pasta, nome), 'wb').write(bytes.fromhex(dados[0]['h']))
        novos += 1
        ultimo = max(ultimo, int(r['id']))
    open(marca, 'w').write(str(ultimo))
    jogos = sorted({r['jogo'] for r in linhas})
    print(f'{novos} arquivo(s) novo(s) em {out}' + (f' ({", ".join(jogos)})' if jogos else '') + f'; último id {ultimo}')


if __name__ == '__main__':
    main()
