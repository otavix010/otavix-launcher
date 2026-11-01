#!/bin/bash
# Monta o pacote Linux / SteamOS do launcher: compila no Legion (contêiner distrobox "shadbuild", Ubuntu 24.04), junta com o
# conteúdo de out/pacote (hub.toml, perfis, catálogo, licenças: rode scripts/release.cmd antes) e traz de volta
# out/Otavix-Launcher-<versão>-linux-x64.tar.gz. Uso (no Git Bash do PC): scripts/pacote_linux.sh <versão>
set -e
v="$1"; [ -n "$v" ] || { echo "uso: pacote_linux.sh <versão>"; exit 1; }
cd "$(dirname "$0")/.."
tar -cf - src CMakeLists.txt profiles hub docs/notas assets | ssh legion 'rm -rf ~/launcher-linux/src ~/launcher-linux/profiles && mkdir -p ~/launcher-linux && cd ~/launcher-linux && tar -xf -'
ssh legion 'cd ~/launcher-linux && distrobox enter shadbuild -- bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && ninja -C build" 2>&1 | tail -3'
ssh legion 'rm -rf ~/launcher-pacote && mkdir -p ~/launcher-pacote'
tar -C out/pacote --exclude='*.exe' -cf - . | ssh legion 'cd ~/launcher-pacote && tar -xf -'
cat > out/instalar.sh <<'SH'
#!/bin/bash
# Cria a entrada do Otavix Launcher no menu (modo desktop). No Modo Jogo do SteamOS: adicione "Otavix Launcher" como jogo fora da Steam.
d="$(cd "$(dirname "$0")" && pwd)"
mkdir -p ~/.local/share/applications
cat > ~/.local/share/applications/otavix-launcher.desktop <<DESK
[Desktop Entry]
Type=Application
Name=Otavix Launcher
Exec="$d/Otavix Launcher"
Path=$d
Icon=$d/launcher/icone.png
Categories=Game;
Terminal=false
DESK
echo "Pronto: Otavix Launcher está no menu de aplicativos."
SH
scp -q out/instalar.sh legion:~/launcher-pacote/instalar.sh
ssh legion "set -e; cd ~; rm -rf Otavix-Launcher-stage; mkdir -p Otavix-Launcher-stage/Otavix-Launcher; cd Otavix-Launcher-stage/Otavix-Launcher; cp -a ~/launcher-pacote/. .; cp ~/launcher-linux/build/'Otavix Launcher' .; cp -a ~/launcher-linux/build/launcher/. launcher/ 2>/dev/null || true; chmod +x 'Otavix Launcher' instalar.sh; strip 'Otavix Launcher' 2>/dev/null || true; cd ..; tar -czf ~/Otavix-Launcher-$v-linux-x64.tar.gz Otavix-Launcher; ls -la ~/Otavix-Launcher-$v-linux-x64.tar.gz"
scp -q legion:~/Otavix-Launcher-$v-linux-x64.tar.gz out/
ls -la out/Otavix-Launcher-$v-linux-x64.tar.gz
