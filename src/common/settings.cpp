#include "common/settings.hpp"
#include "common/features.hpp"
#include "common/i18n.hpp"
#include "common/log.hpp"

#include <windows.h>
#include <string>

namespace settings {
namespace {
Ui g_ui;

std::wstring Path() { return logx::DataDir() + L"\\settings.ini"; }

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Get(const wchar_t* section, const std::string& key, const char* fallback) {
    wchar_t buf[256]{};
    GetPrivateProfileStringW(section, W(key).c_str(), W(fallback).c_str(), buf, 256, Path().c_str());
    char out[512]{};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

void Set(const wchar_t* section, const std::string& key, const std::string& value) {
    WritePrivateProfileStringW(section, W(key).c_str(), W(value).c_str(), Path().c_str());
}
}

Ui& UiSettings() { return g_ui; }

void Load() {
    g_ui.menuKey = hotkeys::Parse(Get(L"ui", "menu_key", hotkeys::Serialize(g_ui.menuKey).c_str()));
    if (!g_ui.menuKey.IsBound()) g_ui.menuKey = hotkeys::Binding{ 0x2D };
    g_ui.x = static_cast<float>(atof(Get(L"ui", "x", "60").c_str()));
    g_ui.y = static_cast<float>(atof(Get(L"ui", "y", "60").c_str()));
    g_ui.w = static_cast<float>(atof(Get(L"ui", "w", "0").c_str()));
    g_ui.h = static_cast<float>(atof(Get(L"ui", "h", "0").c_str()));
    if (g_ui.w < 200 || g_ui.h < 150) g_ui.w = g_ui.h = 0;
    g_ui.scale = static_cast<float>(atof(Get(L"ui", "scale", "1.0").c_str()));
    if (g_ui.scale < 0.75f || g_ui.scale > 2.5f) g_ui.scale = 1.0f;
    g_ui.toasts = Get(L"ui", "toasts", "1") != "0";
    g_ui.autoScale = Get(L"ui", "auto_scale", "1") != "0";
    g_ui.language = i18n::Code(i18n::FromCode(Get(L"ui", "language", "en")));
    i18n::Set(i18n::FromCode(g_ui.language));

    for (features::Feature* f : features::List()) {
        f->hotkey = hotkeys::Parse(Get(L"hotkeys", f->id, hotkeys::Serialize(f->defaultHotkey).c_str()));
        f->hotkey2 = hotkeys::Parse(Get(L"hotkeys", f->id + ".2", "none"));
        if (features::HasSlider(*f)) {
            f->hotkeyUp = hotkeys::Parse(Get(L"hotkeys", f->id + ".plus", "none"));
            f->hotkeyDown = hotkeys::Parse(Get(L"hotkeys", f->id + ".minus", "none"));
        }
        if (f->kind == features::Kind::Toggle) f->wanted = Get(L"enabled", f->id, f->defaultOn ? "1" : "0") == "1";
        if (f->valueMax > f->valueMin) {
            char def[32];
            sprintf_s(def, "%g", f->value);
            float v = static_cast<float>(atof(Get(L"values", f->id, def).c_str()));
            if (v >= f->valueMin && v <= f->valueMax) f->value = v;
        } else if (f->color) {  // a colour 0xRRGGBB as an integer ("%g" would round it)
            char def[32];
            sprintf_s(def, "%.0f", f->value);
            const double v = atof(Get(L"values", f->id, def).c_str());
            if (v >= 0.0 && v <= 16777215.0) f->value = static_cast<float>(v);
        }
    }
    LOG("settings loaded from %ls", Path().c_str());
}

void SaveFeature(const features::Feature& f) {
    Set(L"hotkeys", f.id, hotkeys::Serialize(f.hotkey));
    if (f.hotkey2.IsBound()) Set(L"hotkeys", f.id + ".2", hotkeys::Serialize(f.hotkey2));
    else WritePrivateProfileStringW(L"hotkeys", W(f.id + ".2").c_str(), nullptr, Path().c_str());  // none: no line
    if (features::HasSlider(f)) {
        Set(L"hotkeys", f.id + ".plus", hotkeys::Serialize(f.hotkeyUp));
        Set(L"hotkeys", f.id + ".minus", hotkeys::Serialize(f.hotkeyDown));
    }
    if (f.kind == features::Kind::Toggle) Set(L"enabled", f.id, f.wanted ? "1" : "0");
    if (f.valueMax > f.valueMin || f.color) {
        char buf[32];
        sprintf_s(buf, f.color ? "%.0f" : "%g", f.value);
        Set(L"values", f.id, buf);
    }
}

void SaveUi() {
    char buf[32];
    Set(L"ui", "menu_key", hotkeys::Serialize(g_ui.menuKey));
    sprintf_s(buf, "%.0f", g_ui.x);
    Set(L"ui", "x", buf);
    sprintf_s(buf, "%.0f", g_ui.y);
    Set(L"ui", "y", buf);
    sprintf_s(buf, "%.0f", g_ui.w);
    Set(L"ui", "w", buf);
    sprintf_s(buf, "%.0f", g_ui.h);
    Set(L"ui", "h", buf);
    sprintf_s(buf, "%.2f", g_ui.scale);
    Set(L"ui", "scale", buf);
    Set(L"ui", "toasts", g_ui.toasts ? "1" : "0");
    Set(L"ui", "auto_scale", g_ui.autoScale ? "1" : "0");
    Set(L"ui", "language", g_ui.language);
}
}
