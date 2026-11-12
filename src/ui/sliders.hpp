#pragma once
// Sliders whose number can be typed: a plain click (no drag) opens the slider's own text box, dragging still slides.
// A slider is hard to stop on one exact value (user, 2026-10-14). ImGui only offers Ctrl+click for that.
#include <imgui.h>
#include <imgui_internal.h>

namespace widgets {
namespace detail {
inline ImGuiID g_pressed = 0;  // slider pressed with the mouse, not released yet
inline float g_before = 0;     // its value before the press (the click itself moves the value)

// After the slider call: a click released without dragging puts the value back and asks ImGui to open the text box
// of that slider on the next frame (activation with "prefer input", what Enter or Ctrl+click do).
inline bool ClickToType(ImGuiID id, bool typing, float old, float& value) {
    if (typing) return false;
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_pressed = id;
        g_before = old;
    }
    if (g_pressed != id || !ImGui::IsItemDeactivated()) return false;
    g_pressed = 0;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDragMaxDistanceSqr[0] >= io.MouseDragThreshold * io.MouseDragThreshold) return false;
    value = g_before;
    ImGui::ActivateItemByID(id);
    GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
    return true;
}
}

inline bool SliderFloat(const char* label, float* v, float min, float max, const char* format, ImGuiSliderFlags flags = 0) {
    const ImGuiID id = ImGui::GetID(label);
    const bool typing = ImGui::TempInputIsActive(id);
    const float old = *v;
    bool changed = ImGui::SliderFloat(label, v, min, max, format, ImGuiSliderFlags_AlwaysClamp | flags);
    if (detail::ClickToType(id, typing, old, *v)) changed = true;
    return changed;
}

inline bool SliderInt(const char* label, int* v, int min, int max) {
    const ImGuiID id = ImGui::GetID(label);
    const bool typing = ImGui::TempInputIsActive(id);
    const int old = *v;
    bool changed = ImGui::SliderInt(label, v, min, max, "%d", ImGuiSliderFlags_AlwaysClamp);
    float value = static_cast<float>(*v);
    if (detail::ClickToType(id, typing, static_cast<float>(old), value)) {
        *v = static_cast<int>(value);
        changed = true;
    }
    return changed;
}
}
