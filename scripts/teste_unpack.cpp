// Bancada do unpack (report 1367): descompacta os arquivos dados com unpack::Extract e diz o que aconteceu.
// Linux: c++ -std=c++20 -Isrc -I<SDL3 include> scripts/teste_unpack.cpp src/unpack.cpp <libSDL3.a> -o teste_unpack
// Uso: teste_unpack <arquivo> <pasta destino>
#include <SDL3/SDL.h>

#include <cstdio>
#include <string>

#include "unpack.h"

namespace launcher {
std::string L3(const char* pt, const char*, const char*) { return pt; }
}  // namespace launcher

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    SDL_Init(0);
    std::string why;
    std::printf("tar do sistema: \"%s\"\n", unpack::Tar().c_str());
    bool ok = unpack::Extract(argv[1], argv[2], why);
    std::printf("%s: %s%s\n", argv[1], ok ? "ok" : "FALHOU: ", why.c_str());
    SDL_Quit();
    return ok ? 0 : 1;
}
