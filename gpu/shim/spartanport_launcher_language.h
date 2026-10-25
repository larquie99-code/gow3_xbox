// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <string_view>

// App UI only. The guest game's SPARTAN_LANGUAGE setting is independent.
namespace SpartanLauncher {
inline constexpr std::array Names{"English", "Português (Brasil)", "Español"};
inline constexpr std::array Codes{"en", "pt-BR", "es"};
inline int Normalize(int language) {
    return language >= 0 && language < int(Names.size()) ? language : 0;
}
inline int Parse(std::string_view code) {
    for (int i = 0; i < int(Codes.size()); ++i)
        if (code == Codes[i]) return i;
    return 0;
}
// PollChoice returns negative values for an open/cancelled menu; Back is index 3.
inline int SelectedLanguage(int current, int choice) {
    return choice >= 0 && choice < int(Names.size()) ? choice : Normalize(current);
}

struct Text {
    const char *start, *precompile, *folder, *language, *back, *browse, *current;
    const char *folder_help, *internal, *usb, *controls;
    const char *scanning, *vertex, *fragment, *prepared, *failed_variants, *failed, *no_folder;
};
inline constexpr std::array<Text, 3> Translations{{
    {"Start game", "Precompile shaders", "Choose game folder", "Language", "Back",
     "Browse USB drive...", "current",
     "Choose the game folder\nInternal storage: put the dump anywhere under D:\\DevelopmentFiles.\n"
     "USB drive: any folder; choose it with Browse USB drive.",
     "internal storage", "USB drive",
     "D-pad / Up / Down: choose   Enter / A: OK   Esc / B: cancel",
     "Scanning game files...", "Precompiling vertex shaders...", "Precompiling fragment shaders...",
     "{} shader entries prepared. Others compile during gameplay.{}",
     " Some variants could not be prepared.", "Precompilation could not finish: ",
     "Game folder is not configured"},
    {"Iniciar jogo", "Pré-compilar shaders", "Escolher pasta do jogo", "Idioma", "Voltar",
     "Procurar na unidade USB...", "atual",
     "Escolha a pasta do jogo\nArmazenamento interno: coloque os arquivos em D:\\DevelopmentFiles.\n"
     "Unidade USB: qualquer pasta; selecione com Procurar na unidade USB.",
     "armazenamento interno", "unidade USB",
     "Direcional / Cima / Baixo: escolher   Enter / A: OK   Esc / B: cancelar",
     "Verificando arquivos do jogo...", "Pré-compilando shaders de vértices...",
     "Pré-compilando shaders de fragmentos...",
     "{} entradas de shaders preparadas. As demais serão compiladas durante o jogo.{}",
     " Algumas variantes não puderam ser preparadas.", "Não foi possível concluir a pré-compilação: ",
     "A pasta do jogo não está configurada"},
    {"Iniciar juego", "Precompilar shaders", "Elegir carpeta del juego", "Idioma", "Volver",
     "Buscar en la unidad USB...", "actual",
     "Elige la carpeta del juego\nAlmacenamiento interno: coloca los archivos en D:\\DevelopmentFiles.\n"
     "Unidad USB: cualquier carpeta; selecciónala con Buscar en la unidad USB.",
     "almacenamiento interno", "unidad USB",
     "Cruceta / Arriba / Abajo: elegir   Enter / A: OK   Esc / B: cancelar",
     "Comprobando archivos del juego...", "Precompilando shaders de vértices...",
     "Precompilando shaders de fragmentos...",
     "{} entradas de shaders preparadas. Las demás se compilarán durante el juego.{}",
     " No se pudieron preparar algunas variantes.", "No se pudo completar la precompilación: ",
     "La carpeta del juego no está configurada"}
}};
inline const Text& Strings(int language) {
    return Translations[Normalize(language)];
}
inline const char* Place(int language, std::string_view place) {
    const auto& text = Strings(language);
    if (place == "internal storage") return text.internal;
    if (place == "USB drive") return text.usb;
    return "";
}
} // namespace SpartanLauncher
