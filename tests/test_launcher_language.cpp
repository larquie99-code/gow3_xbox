// SPDX-License-Identifier: GPL-2.0-or-later
#include "spartanport_launcher_language.h"
#include "spartanport_settings.h"

#include "test_environment.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

int main() {
    namespace L = SpartanLauncher;
    auto& language = SpartanSettings::Get().launcher_language;
    assert(language == 0); // Existing installations keep English.
    assert(std::string(L::Strings(language).start) == "Start game");
    const char* path = "launcher-language-test.ini";
    SetTestEnvironment("SPARTAN_CONFIG", path);
    SetTestEnvironment("SPARTAN_LANGUAGE", "17");
    for (int selected = 0; selected < int(L::Names.size()); ++selected) {
        language = L::SelectedLanguage(language, selected);
        SpartanSettings::Save();
        language = (selected + 1) % int(L::Names.size());
        SpartanSettings::Load();
        assert(language == selected); // Disk roundtrip for each supported language.
        assert(L::Parse(L::Codes[selected]) == selected);
        // Back, cancel and an unfinished dialog preserve the selection.
        for (int dismissed : {-2, -1, 3, 99}) {
            language = L::SelectedLanguage(language, dismissed);
            assert(language == selected);
        }
        // Launcher localization must not override the guest game's language.
        assert(std::string(std::getenv("SPARTAN_LANGUAGE")) == "17");
    }
    assert(std::string(L::Strings(1).start) == "Iniciar jogo");
    assert(std::string(L::Strings(2).start) == "Iniciar juego");
    assert(std::string(L::Place(1, "internal storage")) == "armazenamento interno");
    assert(std::string(L::Place(2, "USB drive")) == "unidad USB");
    for (const char* invalid : {"unknown", "", "-1", "999", "pt"}) {
        std::ofstream(path) << "launcher_language=" << invalid << '\n';
        language = 2;
        SpartanSettings::Load();
        assert(language == 0); // Invalid saved preferences safely fall back to English.
    }
    assert(&L::Strings(-1) == &L::Strings(0));
    assert(&L::Strings(99) == &L::Strings(0));
    std::remove(path);
    SetTestEnvironment("SPARTAN_CONFIG", "");
    SetTestEnvironment("SPARTAN_LANGUAGE", "");
}
