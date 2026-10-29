r"""REGRA DO LAUNCHER: todo jogo que sobe para o launcher tem a sua foto (capa e fundo) para QUALQUER jogador, tenha ele
baixado o jogo ou nao. A arte da propria copia do jogador ([art] folder) e so um extra; o perfil precisa de [art] web
com enderecos publicos (loja, GameTDB, libretro) ou do nosso servidor (/v1/arquivos/arte/<jogo>/<arquivo>, enviado
pelo espelho.py na lista ARTE).

    py -3 -I scripts\conferir_arte.py [--rede]

Sem --rede confere so que cada jogo do hub.toml tem [art] web; com --rede abre cada endereco (200 e imagem).
Sai com erro (1) se algum jogo ficou sem foto; o release.cmd chama sem --rede e o espelho depois de subir, com --rede."""
import sys
import tomllib
import urllib.request
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent


def main():
    hub = tomllib.loads((RAIZ / 'hub' / 'hub.toml').read_text(encoding='utf-8'))
    rede = '--rede' in sys.argv
    faltam = []
    for jogo in hub.get('games', []):
        nome = Path(jogo['profile']).name
        perfil = RAIZ / 'profiles' / nome
        if not perfil.exists():
            faltam.append(f"{jogo['id']}: perfil {nome} nao existe")
            continue
        art = tomllib.loads(perfil.read_text(encoding='utf-8')).get('art', {})
        web = art.get('web', [])
        if not web:
            faltam.append(f"{jogo['id']}: sem [art] web (foto publica para quem nao tem o jogo)")
            continue
        if rede:
            for url in web:
                try:
                    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
                    with urllib.request.urlopen(req, timeout=30) as r:
                        tipo = r.headers.get('Content-Type', '')
                        inicio = r.read(8)  # the server answers octet-stream: the picture is told by its first bytes
                        if r.status != 200 or not (tipo.startswith('image') or inicio[:4] == b'\x89PNG' or inicio[:2] == b'\xff\xd8'):
                            faltam.append(f"{jogo['id']}: {url} -> {r.status} {tipo}")
                except Exception as e:
                    faltam.append(f"{jogo['id']}: {url} -> {e}")
    for f in faltam:
        print('SEM FOTO:', f)
    print(f"{len(hub.get('games', []))} jogos conferidos, {len(faltam)} problema(s)")
    sys.exit(1 if faltam else 0)


main()
