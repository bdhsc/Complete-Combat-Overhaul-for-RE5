// The melee swap page per character (user, 2026-11-11: "I would like the melee swap, as the free melee, to have a sub-tab per character:
// for example swap the Head Flash of Chris only and not Josh's, accept the beta animations on Sheva only and not Chris, or allow the solo
// combos with Wesker only - so every option of the melee swap page duplicated per character, applied to the played character only;
// create the tabs of RE6's characters too, since we said we would make them playable next").
//
// Every option of the page (category "Mêlée") gets a copy per character ("<id>@<character>", category "Mêlée · <character>"). The
// original stays the one that acts on the game, hidden ("Mêlée (global)"): when player 1's character changes (Player1CharacterIndex),
// his copies' states are given to the originals (toggles set, choices chosen, sliders' values); a copy of the character playing now
// acts at once when it is changed, the others are only kept. The copies start from the original's saved state (settings.ini), so
// nothing changes until the user sets one. "Animations de X" (already one per character) go to X's tab. Wesker's dash options are copied
// per character like the others (user, 2026-11-11: "the free melee can start the dash whatever the character").
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/hotkeys.hpp"
#include "common/log.hpp"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace features {
int Player1CharacterIndex();  // swaps.cpp: 0 Chris, 1 Sheva, 2 Jill, 3 Wesker, 4 Josh, 5 Excella, 6 Barry, 7 Rebecca

namespace {
struct Who {
    const char* key;
    const char* name;
};
// The game's character rows (Player1CharacterIndex). RE6's characters left out of the release (user, 2026-11-12: "not ready").
constexpr Who kWho[] = { { "chris", "Chris" },   { "sheva", "Sheva" },   { "jill", "Jill" },     { "wesker", "Wesker" },
                         { "josh", "Josh" },     { "excella", "Excella" }, { "barry", "Barry" }, { "rebecca", "Rebecca" } };
constexpr int kWhoCount = static_cast<int>(std::size(kWho));
constexpr int kPlayable = 8;

struct Group {
    Feature* orig = nullptr;
    Feature* copies[kWhoCount]{};
};
std::vector<Group> g_groups;
int g_current = -1;  // the character whose copies the originals hold

std::string IniGet(const wchar_t* section, const std::string& key, const char* fallback) {
    const std::wstring path = logx::DataDir() + L"\\settings.ini";
    wchar_t wkey[256]{}, wfallback[64]{}, buf[256]{};
    MultiByteToWideChar(CP_UTF8, 0, key.c_str(), -1, wkey, 256);
    MultiByteToWideChar(CP_UTF8, 0, fallback, -1, wfallback, 64);
    GetPrivateProfileStringW(section, wkey, wfallback, buf, 256, path.c_str());
    char out[512]{};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

bool Available(const Feature& f) { return !f.available || f.available(); }

// The copy `c` of `g` given to its original (the character playing now).
void Apply(Group& g, const Feature& c) {
    Feature& o = *g.orig;
    if (o.valueMax > o.valueMin && o.kind != Kind::Choice) o.value = c.value;
    if (o.kind == Kind::Toggle) {
        o.wanted = c.wanted;
        if (o.enabled != c.wanted && Available(o)) {
            const bool ok = o.set ? o.set(c.wanted) : true;
            if (ok) o.enabled = c.wanted;
        }
    } else if (o.kind == Kind::Choice) {
        const int want = static_cast<int>(c.value);
        o.value = c.value;
        if (o.applied != want && Available(o) && o.choose && o.choose(want)) o.applied = want;
    }
}

int Current() { return actors::InGame() ? Player1CharacterIndex() : -1; }

void Tick() {
    const int now = Current();
    if (now >= 0 && now < kPlayable && now != g_current) {
        g_current = now;
        for (Group& g : g_groups) Apply(g, *g.copies[now]);
        LOG("melee swaps: %s's settings applied (%zu options)", kWho[now].name, g_groups.size());
    }
    if (g_current < 0) return;
    // The sliders of the character playing now follow at once (the options read their own value).
    for (Group& g : g_groups) {
        Feature& o = *g.orig;
        const Feature& c = *g.copies[g_current];
        if (o.valueMax > o.valueMin && o.kind != Kind::Choice && o.value != c.value) o.value = c.value;
        if (o.holdKey) {  // a key held, read by the option from its own binding
            o.hotkey = c.hotkey;
            o.hotkey2 = c.hotkey2;
        }
    }
}
}  // namespace

void RegisterPerCharacterMelee() {
    // The originals of the page, as registered (copies are added below: take the list first).
    std::vector<Feature*> page;
    for (Feature* f : List())
        if (f->category == "Mêlée") page.push_back(f);
    int moved = 0;
    for (Feature* f : page) {
        // Already one per character: to its character's tab.
        bool placed = false;
        for (int w = 0; w < kPlayable && !placed; ++w)
            if (f->id == std::string("char.anims_") + kWho[w].key) {
                f->category = std::string("Mêlée · ") + kWho[w].name;
                placed = true;
            }
        if (placed) {
            ++moved;
            continue;
        }
        if (f->kind == Kind::Action || f->id.rfind("test.", 0) == 0) continue;  // no state to keep per character: left where it is
        Group g;
        g.orig = f;
        // Its saved state, the copies' start.
        const bool savedOn = IniGet(L"enabled", f->id, f->defaultOn ? "1" : "0") == "1";
        char def[32];
        sprintf_s(def, "%g", f->value);
        const float savedValue = static_cast<float>(atof(IniGet(L"values", f->id, def).c_str()));
        for (int w = 0; w < kWhoCount; ++w) {
            Feature c;
            c.id = f->id + "@" + kWho[w].key;
            c.name = f->name;
            c.help = f->help;
            c.category = std::string("Mêlée · ") + kWho[w].name;
            c.kind = f->kind;
            c.available = f->available;
            c.value = savedValue >= f->valueMin && savedValue <= f->valueMax ? savedValue : f->value;
            c.valueMin = f->valueMin;
            c.valueMax = f->valueMax;
            c.valueFormat = f->valueFormat;
            c.negativeText = f->negativeText;
            c.valueStep = f->valueStep;
            c.color = f->color;
            c.choices = f->choices;
            c.choiceOrder = f->choiceOrder;
            c.defaultOn = savedOn;
            c.holdKey = f->holdKey;
            c.hotkeyIgnored = f->hotkeyIgnored;
            if (f->holdKey) c.hotkey = hotkeys::Parse(IniGet(L"hotkeys", f->id, hotkeys::Serialize(f->defaultHotkey).c_str()));
            Feature* orig = f;
            const int who = w;
            if (f->kind == Kind::Toggle) {
                c.set = [orig, who](bool on) {
                    if (who != g_current) return true;  // kept for that character
                    orig->wanted = on;
                    if (orig->enabled == on) return true;
                    const bool ok = orig->set ? orig->set(on) : true;
                    if (ok) orig->enabled = on;
                    return ok;
                };
            } else {
                c.choose = [orig, who](int index) {
                    if (who != g_current) return true;
                    orig->value = static_cast<float>(index);
                    if (orig->applied == index) return true;
                    const bool ok = orig->choose && orig->choose(index);
                    if (ok) orig->applied = index;
                    if (!ok && !orig->lastError.empty()) LOG("melee swaps: %s refused for %s: %s", orig->id.c_str(), kWho[who].name,
                                                            orig->lastError.c_str());
                    return ok;
                };
            }
            g.copies[w] = &Add(std::move(c));
        }
        f->category = "Mêlée (global)";  // hidden: the copies show it
        g_groups.push_back(g);
    }
    OnFrame(Tick);
    LOG("melee swaps: %zu options per character (%d characters), %d moved to their character's tab", g_groups.size(), kWhoCount, moved);
}
}  // namespace features
