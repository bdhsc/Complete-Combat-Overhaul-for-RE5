#include "common/i18n.hpp"
#include "common/features.hpp"
#include "common/log.hpp"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>

namespace i18n {
namespace {
struct Entry {
    const char* text[kLangCount];
};
// { French, English, Spanish } (u8 literals: the sources are UTF-8, as ImGui wants).
const Entry kStrings[] = {
#include "i18n/strings.inc"
};

std::atomic<int> g_lang{ static_cast<int>(Lang::En) };
std::mutex g_missingLock;
std::set<std::string> g_missing;  // asked of Tr, not in the table (render thread mostly)

const std::unordered_map<std::string_view, const Entry*>& Table() {
    static const std::unordered_map<std::string_view, const Entry*> table = [] {
        std::unordered_map<std::string_view, const Entry*> t;
        t.reserve(std::size(kStrings));
        for (const Entry& e : kStrings) t.emplace(e.text[0], &e);
        return t;
    }();
    return table;
}

const char* Lookup(const char* french) {
    if (!french || !*french) return french;
    const auto& table = Table();
    const auto it = table.find(std::string_view(french));
    if (it == table.end()) {
        std::lock_guard<std::mutex> lock(g_missingLock);
        if (g_missing.size() < 5000) g_missing.emplace(french);
        return french;
    }
    const char* text = it->second->text[g_lang.load(std::memory_order_relaxed)];
    return text && *text ? text : french;
}
}  // namespace

Lang Current() { return static_cast<Lang>(g_lang.load(std::memory_order_relaxed)); }
void Set(Lang lang) { g_lang = static_cast<int>(lang); }

const char* Code(Lang lang) {
    switch (lang) {
    case Lang::Fr: return "fr";
    case Lang::Es: return "es";
    default: return "en";
    }
}

Lang FromCode(const std::string& code) {
    if (code == "fr") return Lang::Fr;
    if (code == "es") return Lang::Es;
    return Lang::En;
}

const char* Name(Lang lang) {
    switch (lang) {
    case Lang::Fr: return "Français";
    case Lang::Es: return "Español";
    default: return "English";
    }
}

// Looked up in French too: the strings the table lacks are noted for the export whatever the language.
const char* Tr(const char* french) {
    const char* text = Lookup(french);
    return Current() == Lang::Fr ? french : text;
}

std::string Tr(const std::string& french) { return Tr(french.c_str()); }

void ExportStrings() {
    std::set<std::string> all;
    for (const features::Feature* f : features::List()) {
        for (const std::string* s : { &f->name, &f->help, &f->category }) all.insert(*s);
        for (const std::string& c : f->choices) all.insert(c);
        if (f->valueFormat) all.insert(f->valueFormat);
        if (f->negativeText) all.insert(f->negativeText);
    }
    for (const features::Panel& p : features::Panels()) {
        all.insert(p.tab);
        for (const std::string& c : p.categories) all.insert(c);
    }
    {
        std::lock_guard<std::mutex> lock(g_missingLock);
        all.insert(g_missing.begin(), g_missing.end());
    }
    std::ofstream out(logx::DataDir() + L"\\i18n_strings.txt", std::ios::binary);
    int missing = 0;
    for (const std::string& s : all) {
        if (s.empty()) continue;
        const bool known = Table().count(std::string_view(s)) != 0;
        if (!known) ++missing;
        std::string line;  // one string per line: backslashes, newlines and tabs escaped as in the C sources
        for (const char c : s) line += c == '\\' ? "\\\\" : c == '\n' ? "\\n" : c == '\t' ? "\\t" : std::string(1, c);
        out << (known ? "+\t" : "-\t") << line << "\n";
    }
    LOG("i18n: %zu strings exported, %d not translated", all.size(), missing);
}
// "Exporter les textes à traduire" for the bridge too (run diag.export_strings).
void Register() {
    features::Add({ .id = "diag.export_strings", .name = "Exporter les textes à traduire",
                    .help = "Écrit dans i18n_strings.txt (dossier du trainer) tous les textes du menu et ceux affichés depuis le "
                            "lancement, marqués - quand ils n'ont pas encore de traduction.",
                    .category = "Diagnostic", .kind = features::Kind::Action,
                    .run = [] {
                        ExportStrings();
                        return true;
                    } });
}
}  // namespace i18n
