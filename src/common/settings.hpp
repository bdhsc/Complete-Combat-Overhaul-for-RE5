#pragma once
#include "common/hotkeys.hpp"

namespace features { struct Feature; }

// %LOCALAPPDATA%\RE5TrainerPlus\settings.ini
namespace settings {
struct Ui {
    hotkeys::Binding menuKey{ 0x2D };  // Insert
    float x = 60, y = 60;
    float w = 0, h = 0;  // menu window size (0 = default size for the menu scale)
    float scale = 1.0f;
    bool autoScale = true;
    bool toasts = true;
    std::string language = "en";  // i18n.hpp: "fr" / "en" / "es"
};

Ui& UiSettings();
void Load();                                  // after features are registered
void SaveFeature(const features::Feature& f);
void SaveUi();
}
