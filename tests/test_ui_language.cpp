// SPDX-License-Identifier: GPL-2.0-or-later
#include "spartanport_ui_widgets.h"
#include "imgui_internal.h"
#include <cassert>
#include <string>
#include <sstream>

int main() {
    std::istringstream config("# saved UI preference\nlauncher_language=en\nlauncher_language=pt-BR \r\n");
    assert(SpartanUi::ReadLanguage(config) == 1);
    std::istringstream invalid("launcher_language=es\nlauncher_language=invalid\n");
    assert(SpartanUi::ReadLanguage(invalid) == 0);
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto& language = SpartanSettings::Get().launcher_language;
    ImGuiID id = 0;
    for (int selected : {0, 1, 2, 0}) {
        language = selected;
        ImGui::NewFrame();
        ImGui::Begin("UI language test");
        const auto label = SpartanUi::Label(selected, "Close");
        if (!id) id = ImGui::GetID(label.c_str());
        assert(id == ImGui::GetID(label.c_str()));
        assert(SpartanUi::Label(selected, "##value") == "##value");
        ImGui::LogToBuffer();
        SpartanUi::SeparatorText("Performance");
        SpartanUi::TextDisabled("Loading shader cache %u%%", 75u);
        SpartanUi::Button("Close");
        const char* choices[] = {"Custom", "Balanced", "Lower latency"};
        int profile = 1;
        SpartanUi::Combo("Profile", &profile, choices, 3);
        assert(profile == 1);
        const char* balanced[] = {"Balanced", "Equilibrado", "Equilibrado"};
        assert(std::string(GImGui->LogBuffer.c_str()).find(balanced[selected]) != std::string::npos);
        const std::string rendered = GImGui->LogBuffer.c_str();
        const char* names[] = {"Performance", "Desempenho", "Rendimiento"};
        const char* loading[] = {"Loading shader cache 75%", "Carregando cache de shaders 75%", "Cargando caché de shaders 75%"};
        assert(rendered.find(names[selected]) != std::string::npos);
        assert(rendered.find(loading[selected]) != std::string::npos);
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
    }
    for (auto& e : SpartanUi::Catalog) {
        assert(std::string(SpartanUi::Translate(0, e.en)) == e.en);
        assert(std::string(SpartanUi::Translate(1, e.en)) == e.pt);
        assert(std::string(SpartanUi::Translate(2, e.en)) == e.es);
        assert(std::string(SpartanUi::Translate(-1, e.en)) == e.en);
    }
    assert(std::string(SpartanUi::Translate(2, "E:\\My game\\eboot.bin")) == "E:\\My game\\eboot.bin");
    ImGui::DestroyContext();
}
