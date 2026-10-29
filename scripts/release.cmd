@echo off
rem Monta a versao publica: compila, gera os catalogos, junta out\pacote (exe, fontes, hub.toml, perfis, catalogo,
rem licencas) e cria out\Instalar-Otavix-Launcher-<versao>.exe com o Inno Setup.
rem Uso: scripts\release.cmd 1.0.0
setlocal
if "%~1"=="" (echo Uso: release.cmd ^<versao^>& exit /b 1)
cd /d "%~dp0.."
call scripts\build.cmd || exit /b 1
rem a regra: todo jogo tem a sua foto para qualquer jogador ([art] web dos perfis)
py -3 -I scripts\conferir_arte.py || exit /b 1
set "P=out\pacote"
rd /s /q "%P%" 2>nul
mkdir "%P%\perfis" "%P%\catalogo"
copy /y "out\Otavix Launcher.exe" "%P%\" >nul || exit /b 1
xcopy /y /e /i /q "out\launcher" "%P%\launcher" >nul
copy /y profiles\*.toml "%P%\perfis\" >nul
copy /y LICENSE "%P%\LICENSE.txt" >nul
copy /y THIRD_PARTY.md "%P%\THIRD_PARTY.txt" >nul
rem the public hub.toml, with this release's version
powershell -NoProfile -Command "(Get-Content -Raw hub\hub.toml) -replace '(?m)^version = \".*\"', 'version = \"%~1\"' | Set-Content -NoNewline -Encoding utf8 '%P%\hub.toml'"
rem the catalogs: fresh from libretro, GameTDB and Redump
py -3 -I scripts\catalogo.py "%P%\catalogo" || exit /b 1
set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
"%ISCC%" /Q /DVersao=%~1 installer\otavix-launcher.iss || exit /b 1
echo pronto: out\Instalar-Otavix-Launcher-%~1.exe
