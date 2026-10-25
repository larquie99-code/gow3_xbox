// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "spartanport_ui_language.h"
#include "spartanport_settings.h"
#include "imgui.h"
#include <vector>

namespace SpartanUi {
inline int Language() { return SpartanSettings::Get().launcher_language.load(); }
inline const char* Tr(const char* text) { return Translate(Language(), text); }
inline void TextUnformatted(const char* text) { ImGui::TextUnformatted(Tr(text)); }
inline void SeparatorText(const char* text) { ImGui::SeparatorText(Tr(text)); }
template<class... Args> void TextDisabled(const char* format, Args... args) {
    if constexpr (sizeof...(Args) == 0) ImGui::TextDisabled("%s", Tr(format));
    else ImGui::TextDisabled(Tr(format), args...);
}
template<class... Args> void TextColored(const ImVec4& color, const char* format, Args... args) {
    ImGui::TextColored(color, Tr(format), args...);
}
inline bool Button(const char* label) { return ImGui::Button(Label(Language(), label).c_str()); }
inline bool Checkbox(const char* label, bool* value) {
    return ImGui::Checkbox(Label(Language(), label).c_str(), value);
}
inline bool BeginTabItem(const char* label, bool* open, ImGuiTabItemFlags flags) {
    return ImGui::BeginTabItem(Label(Language(), label).c_str(), open, flags);
}
inline bool Combo(const char* label, int* selected, const char* const* choices, int count) {
    std::vector<const char*> translated;
    translated.reserve(count);
    for (int i = 0; i < count; ++i) translated.push_back(Tr(choices[i]));
    return ImGui::Combo(Label(Language(), label).c_str(), selected, translated.data(), count);
}
} // namespace SpartanUi
