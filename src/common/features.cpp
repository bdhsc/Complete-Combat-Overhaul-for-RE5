#include "common/features.hpp"
#include "common/actors.hpp"
#include "common/tracer.hpp"
#include "common/i18n.hpp"
#include "common/log.hpp"
#include "common/settings.hpp"
#include "hooks/hooks.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace features {
namespace {
std::deque<Feature> g_storage;  // stable addresses
std::vector<Feature*> g_list;
std::vector<ToastLine> g_toasts;
ULONGLONG g_nextRetry = 0;

bool IsAvailable(const Feature& f) { return !f.available || f.available(); }
}

Feature& Add(Feature f) {
    f.defaultHotkey = f.hotkey;
    g_storage.push_back(std::move(f));
    g_list.push_back(&g_storage.back());
    return g_storage.back();
}

Feature* Find(const std::string& id) {
    for (Feature* f : g_list)
        if (f->id == id) return f;
    return nullptr;
}

const std::vector<Feature*>& List() { return g_list; }

// A toast in the current language: a whole French message of the table, else as built (its parts translated by the
// caller, i18n.hpp).
unsigned g_toastCount = 0;  // toasts made so far (an action that showed its own message gets no generic one)

void Toast(const std::string& text, bool ok) {
    // In the log too (user, 2026-11-05: a shortcut "did nothing" and nothing told why): every shortcut's result and message.
    LOG("toast: %s%s", text.c_str(), ok ? "" : " (failed)");
    ++g_toastCount;
    g_toasts.push_back({ i18n::Tr(text), ok, GetTickCount64() + 2500 });
    if (g_toasts.size() > 6) g_toasts.erase(g_toasts.begin());
}

const std::vector<ToastLine>& Toasts() {
    const ULONGLONG now = GetTickCount64();
    std::erase_if(g_toasts, [now](const ToastLine& t) { return t.until < now; });
    return g_toasts;
}

bool SetEnabled(Feature& f, bool on) {
    if (f.kind != Kind::Toggle) return false;
    f.wanted = on;
    if (f.enabled == on) return true;
    if (on && !IsAvailable(f)) {
        f.lastError = "Indisponible pour l'instant";
        Toast(i18n::Tr(f.name) + i18n::Tr(" : indisponible"), false);
        settings::SaveFeature(f);
        return false;
    }
    bool ok = f.set ? f.set(on) : true;
    if (ok) {
        f.enabled = on;
        f.lastError.clear();
        Toast(i18n::Tr(f.name) + i18n::Tr(on ? " : ACTIVÉ" : " : désactivé"), true);
    } else {
        f.lastError = on ? "Activation refusée (voir trainer.log)" : "Restauration refusée (voir trainer.log)";
        Toast(i18n::Tr(f.name) + i18n::Tr(" : échec"), false);
    }
    LOG("feature %s -> %s : %s", f.id.c_str(), on ? "on" : "off", ok ? "ok" : "FAILED");
    settings::SaveFeature(f);
    return ok;
}

bool Choose(Feature& f, int index) {
    if (f.kind != Kind::Choice || index < 0 || index >= static_cast<int>(f.choices.size())) return false;
    f.value = static_cast<float>(index);
    settings::SaveFeature(f);
    if (!IsAvailable(f)) {
        f.lastError = "Appliqué dès que possible";
        return false;
    }
    f.lastError.clear();  // choose() may set a precise reason
    const bool ok = f.choose ? f.choose(index) : false;
    if (ok) {
        f.applied = index;
        f.lastError.clear();
        Toast(i18n::Tr(f.name) + " : " + i18n::Tr(f.choices[index]), true);
    } else {
        if (f.lastError.empty()) f.lastError = "Refusé (voir trainer.log)";
        Toast(i18n::Tr(f.name) + " : " + i18n::Tr(f.lastError), false);
        f.value = static_cast<float>(f.applied);  // the list shows what is really applied
        settings::SaveFeature(f);
    }
    LOG("choice %s -> %d (%s) : %s", f.id.c_str(), index, f.choices[index].c_str(), ok ? "ok" : "FAILED");
    return ok;
}

namespace {
std::vector<std::function<void()>> g_restoreTasks;
}

void OnRestore(std::function<void()> task) { g_restoreTasks.push_back(std::move(task)); }

namespace {
std::vector<Panel> g_panels;
}

void AddPanel(const std::string& tab, std::function<void()> draw, std::vector<std::string> categories, bool ownScroll,
              bool inPlace) {
    g_panels.push_back({ tab, std::move(draw), std::move(categories), ownScroll, inPlace });
}
const std::vector<Panel>& Panels() { return g_panels; }

namespace {
std::function<void(const std::string&)> g_optionsDrawer;
}
void SetOptionsDrawer(std::function<void(const std::string& category)> drawer) { g_optionsDrawer = std::move(drawer); }
void DrawOptions(const std::string& category) {
    if (g_optionsDrawer) g_optionsDrawer(category);
}
void DrawOption(const std::string& id) {
    if (g_optionsDrawer) g_optionsDrawer(std::string(1, '\x01') + id);  // the drawer's mark: one option by its id
}

namespace {
std::function<void(Feature&, bool, bool)> g_keyButtonDrawer;
}
void SetKeyButtonDrawer(std::function<void(Feature& f, bool second, bool hold)> drawer) { g_keyButtonDrawer = std::move(drawer); }
void DrawKeyButton(Feature& f, bool second, bool hold) {
    if (g_keyButtonDrawer) g_keyButtonDrawer(f, second, hold);
}

void RestoreAll() {
    for (auto& task : g_restoreTasks) task();
    for (Feature* f : g_list) {
        if (f->kind == Kind::Toggle && f->enabled && f->set) {
            f->set(false);
            f->enabled = false;
        } else if (f->kind == Kind::Choice && f->applied != 0 && f->choose) {
            f->choose(0);
            f->applied = 0;
        }
    }
}

bool Trigger(Feature& f) {
    if (f.kind == Kind::Choice) return Choose(f, (static_cast<int>(f.value) + 1) % static_cast<int>(f.choices.size()));
    if (f.kind == Kind::Toggle) return SetEnabled(f, !f.enabled);
    if (!IsAvailable(f)) {
        Toast(i18n::Tr(f.name) + i18n::Tr(" : indisponible"), false);
        return false;
    }
    const unsigned before = g_toastCount;
    bool ok = f.run ? f.run() : false;
    // An action that showed its own message (a refusal and its reason, user 2026-11-05) gets no "name : échec" after it.
    if (!ok || g_toastCount == before) Toast(i18n::Tr(f.name) + i18n::Tr(ok ? "" : " : échec"), ok);
    return ok;
}

namespace {
// Test bridge: %LOCALAPPDATA%\RE5TrainerPlus\bridge_in.txt, one command per line:
//   on <id> | off <id> | run <id> | status
// Results are appended to bridge_out.txt. Lets an agent exercise every option without clicking.
ULONGLONG g_nextBridgePoll = 0;

void PollBridge() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextBridgePoll) return;
    g_nextBridgePoll = now + 300;
    const std::wstring in = logx::DataDir() + L"\\bridge_in.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, in.c_str(), L"r") != 0 || !f) return;
    std::vector<std::string> lines;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (!s.empty()) lines.push_back(s);
    }
    fclose(f);
    DeleteFileW(in.c_str());
    FILE* out = nullptr;
    if (_wfopen_s(&out, (logx::DataDir() + L"\\bridge_out.txt").c_str(), L"a") != 0 || !out) return;
    for (const std::string& line : lines) {
        const size_t sp = line.find(' ');
        const std::string verb = line.substr(0, sp);
        const std::string id = sp == std::string::npos ? "" : line.substr(sp + 1);
        if (verb == "trace") {  // trace <p1|p2|hex>[+hexoff] <len> <seconds> <r|rw|w> <label>
            char target[64]{}, mode[8]{}, label[64]{};
            int len = 4, seconds = 20;
            if (sscanf_s(id.c_str(), "%63s %d %d %7s %63s", target, (unsigned)sizeof(target), &len, &seconds, mode,
                         (unsigned)sizeof(mode), label, (unsigned)sizeof(label)) == 5) {
                std::string t(target);
                uintptr_t base = 0, offset = 0;
                const size_t plus = t.find('+');
                const std::string head = t.substr(0, plus);
                if (plus != std::string::npos) offset = strtoul(t.c_str() + plus + 1, nullptr, 16);
                if (head == "p1" || head == "p2") base = actors::Player(head == "p1" ? 0 : 1);
                else base = strtoul(head.c_str(), nullptr, 16);
                const bool ok = base && tracer::Start(base + offset, len, mode, seconds, label);
                fprintf(out, "trace %s -> %08X : %s\n", target, static_cast<unsigned>(base + offset), ok ? "started" : "FAILED");
            } else {
                fprintf(out, "trace : bad arguments\n");
            }
            continue;
        }
        if (verb == "shot") {  // shot <label>
            hooks::RequestShot(id.empty() ? "now" : id);
            fprintf(out, "shot %s : requested\n", id.c_str());
            continue;
        }
        if (verb == "status") {
            for (Feature* x : g_list)
                fprintf(out, "status %s %s avail=%d %s\n", x->id.c_str(), x->kind == Kind::Toggle ? (x->enabled ? "on" : "off") : "action",
                        IsAvailable(*x) ? 1 : 0, x->lastError.c_str());
            continue;
        }
        if (verb == "choose") {  // choose <id> <index>
            const size_t sp2 = id.find(' ');
            Feature* c = sp2 == std::string::npos ? nullptr : Find(id.substr(0, sp2));
            const bool chosen = c && Choose(*c, atoi(id.c_str() + sp2 + 1));
            fprintf(out, "choose %s : %s %s\n", id.c_str(), chosen ? "ok" : "FAILED", c ? c->lastError.c_str() : "unknown");
            continue;
        }
        if (verb == "value") {  // value <id> <number>: a slider, clamped to its range and saved
            const size_t sp2 = id.find(' ');
            Feature* c = sp2 == std::string::npos ? nullptr : Find(id.substr(0, sp2));
            if (c) {
                // A colour option takes 0xRRGGBB as a decimal integer (0..16777215).
                c->value = std::clamp(static_cast<float>(atof(id.c_str() + sp2 + 1)), c->color ? 0.0f : c->valueMin,
                                      c->color ? 16777215.0f : c->valueMax);
                settings::SaveFeature(*c);
            }
            fprintf(out, "value %s : %s %.3f\n", id.c_str(), c ? "ok" : "unknown", c ? c->value : 0.0f);
            continue;
        }
        Feature* x = Find(id);
        if (!x) {
            fprintf(out, "%s %s : unknown\n", verb.c_str(), id.c_str());
            continue;
        }
        bool ok = false;
        if (verb == "on") ok = SetEnabled(*x, true);
        else if (verb == "off") ok = SetEnabled(*x, false);
        else if (verb == "run") ok = Trigger(*x);
        fprintf(out, "%s %s : %s %s\n", verb.c_str(), id.c_str(), ok ? "ok" : "FAILED", x->lastError.c_str());
    }
    fclose(out);
}
}

std::vector<std::function<void()>> g_frameTasks;

void OnFrame(std::function<void()> task) { g_frameTasks.push_back(std::move(task)); }
void OnFrameFirst(std::function<void()> task) { g_frameTasks.insert(g_frameTasks.begin(), std::move(task)); }

std::vector<std::function<void()>> g_hudDrawers;

void OnHud(std::function<void()> draw) { g_hudDrawers.push_back(std::move(draw)); }

void DrawHud() {
    for (auto& draw : g_hudDrawers) draw();
}

bool HasSlider(const Feature& f) { return f.kind != Kind::Choice && f.valueMax > f.valueMin; }

namespace {
float AutoStep(float range) {
    const float raw = range / 50.0f;
    const float power = std::pow(10.0f, std::floor(std::log10(raw)));
    const float m = raw / power;
    return (m < 1.5f ? 1.0f : m < 3.5f ? 2.0f : m < 7.5f ? 5.0f : 10.0f) * power;
}
}

void StepValue(Feature& f, int direction) {
    if (!HasSlider(f)) return;
    const float step = f.valueStep > 0 ? f.valueStep : AutoStep(f.valueMax - f.valueMin);
    f.value = std::clamp(f.value + direction * step, f.valueMin, f.valueMax);
    if (std::fabs(f.value) < step * 1e-3f) f.value = 0;  // no "-0.00" after going back and forth
    settings::SaveFeature(f);
    if (f.kind == Kind::Toggle && !f.enabled && IsAvailable(f)) SetEnabled(f, true);
    char text[48];
    sprintf_s(text, f.negativeText && f.value < 0.0f ? f.negativeText : f.valueFormat, f.value);
    Toast(i18n::Tr(f.name) + " : " + i18n::Tr(text));
}

// A pad button bound with a key to hold belongs to the trainer while that key is down (user, 2026-10-23: d-pad melee
// moves with LB held, the game also took the d-pad as its item keys and changed the weapon), and until it is released.
void HideHeldPadShortcuts() {
    static uint32_t latched = 0;
    uint32_t hide = 0, down = 0;
    const auto bit = [](uint16_t key) -> uint32_t {
        if (key == hotkeys::kPadLT) return 0x10000;
        if (key == hotkeys::kPadRT) return 0x20000;
        return key >= hotkeys::kPadFirst && key < hotkeys::kPadLT ? 1u << (key - hotkeys::kPadFirst) : 0;
    };
    for (uint16_t key = hotkeys::kPadFirst; key <= hotkeys::kPadRT; ++key)
        if (hotkeys::Held(key)) down |= bit(key);
    for (Feature* f : g_list)
        for (const hotkeys::Binding* b : { &f->hotkey, &f->hotkey2, &f->hotkeyUp, &f->hotkeyDown })
            if (b->hold && hotkeys::Held(b->hold)) hide |= bit(b->key);
    latched = hide | (latched & down);
    hooks::HidePadButtons(0, latched);
}

std::atomic<bool> g_stopping{ false };
void RequestStop() { g_stopping = true; }

void Frame() {
    PollBridge();
    for (auto& task : g_frameTasks) task();
    HideHeldPadShortcuts();
    // A binding with a held key wins over the same key alone: with "C" on one option and "Maj + C" on another, Maj + C
    // only runs the second one.
    std::vector<uint16_t> specific;
    for (Feature* f : g_list)
        for (const hotkeys::Binding* b : { &f->hotkey, &f->hotkey2, &f->hotkeyUp, &f->hotkeyDown })
            if (b->hold && hotkeys::Pressed(*b)) specific.push_back(b->key);
    const auto fires = [&specific](const hotkeys::Binding& b) {
        return hotkeys::Pressed(b) && (b.hold || std::find(specific.begin(), specific.end(), b.key) == specific.end());
    };
    for (Feature* f : g_list) {
        if (hotkeys::MenuOpen()) break;  // no shortcut while the menu is open (digits typed into a slider's box)
        if (!f->holdKey && (fires(f->hotkey) || fires(f->hotkey2)) && !(f->hotkeyIgnored && f->hotkeyIgnored())) Trigger(*f);
        if (HasSlider(*f)) {
            if (fires(f->hotkeyUp)) StepValue(*f, 1);
            if (fires(f->hotkeyDown)) StepValue(*f, -1);
        }
    }

    // Persisted toggles that could not be applied yet (e.g. trainer loaded at the title screen). Stopped by an unload
    // request (2026-10-27: the start-up's batch, ~0.2 s per hook, ran on for seconds after it and the DLL was freed
    // under it - two crashes).
    const ULONGLONG now = GetTickCount64();
    if (now >= g_nextRetry) {
        g_nextRetry = now + 1000;
        for (Feature* f : g_list) {
            if (g_stopping.load()) break;
            // A toggle without set (a tick alone) is applied as it is (2026-10-24: melee.no_hand_shake stayed off).
            if (f->kind == Kind::Toggle && f->wanted && !f->enabled && IsAvailable(*f) && (!f->set || f->set(true))) {
                f->enabled = true;
                LOG("feature %s re-applied", f->id.c_str());
            }
            const int want = static_cast<int>(f->value);
            if (f->kind == Kind::Choice && want != f->applied && IsAvailable(*f) && f->choose && f->choose(want)) {
                f->applied = want;
                f->lastError.clear();
                LOG("choice %s re-applied (%d)", f->id.c_str(), want);
            }
        }
    }

    for (Feature* f : g_list) {
        const bool active = (f->kind == Kind::Toggle && f->enabled) || (f->kind == Kind::Choice && f->applied != 0);
        if (!active || !IsAvailable(*f)) continue;
        if (f->tick) f->tick();
    }
}
}
