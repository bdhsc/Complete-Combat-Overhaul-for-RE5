// The trainer's languages (user, 2026-11-02: "an option in Réglages to change the trainer's language, its menu and the
// messages on screen - Wesker's health, the custom prompts' texts: French (as now), English (the default, to reach the
// most people), Spanish"). The source strings stay the French ones; each is looked up at display time in the table
// src/i18n/strings.inc ({ French, English, Spanish }), itself when missing.
#pragma once

#include <string>

namespace i18n {
enum class Lang { Fr = 0, En = 1, Es = 2 };
constexpr int kLangCount = 3;

Lang Current();
void Set(Lang lang);
const char* Code(Lang lang);           // "fr" / "en" / "es" (settings.ini)
Lang FromCode(const std::string& code);  // English when unknown
const char* Name(Lang lang);           // "Français" / "English" / "Español"

// The text in the current language: a French source string from the table, itself when missing (noted for the export).
const char* Tr(const char* french);
std::string Tr(const std::string& french);

// DataDir\i18n_strings.txt: every option's name, help, tab, choices and value format, and the strings asked of Tr that
// the table lacks (the list to translate).
void ExportStrings();
void Register();  // the export as a Diagnostic action (dllmain.cpp)
}  // namespace i18n
