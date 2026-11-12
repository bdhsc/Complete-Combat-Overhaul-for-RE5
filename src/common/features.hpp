#pragma once
#include "common/hotkeys.hpp"

#include <functional>
#include <string>
#include <vector>

namespace features {

enum class Kind {
    Toggle,  // on/off: set(bool) once, tick() every frame while on
    Action,  // one-shot: run()
    Choice,  // pick one of `choices` (combo box); choose(index) applies it, index 0 = game default
};

struct Feature {
    std::string id;        // stable key for the ini ("ammo.reload_no_reserve")
    std::string name;      // shown in the overlay (French)
    std::string help;      // tooltip
    std::string category;  // overlay tab
    Kind kind = Kind::Toggle;

    std::function<bool(bool)> set;   // Toggle: apply/restore, return success
    std::function<void()> tick;      // Toggle: per-frame work while enabled (freezes)
    std::function<bool()> run;       // Action
    std::function<bool()> available; // false = greyed out (unsupported build, not in game...)
    std::function<bool(int)> choose; // Choice: apply choice index (0 = restore the game default)
    // The shortcut does nothing (no toast) while this answers true: one key shared by two options used in different
    // places (user, 2026-10-27: the belt lamp out of the inventory, the knife to the sheath in it).
    std::function<bool()> hotkeyIgnored;

    // Optional value shown as a slider next to the option (size, speed, score...). Persisted.
    float value = 0;
    float valueMin = 0;
    float valueMax = 0;  // valueMax > valueMin enables the slider
    const char* valueFormat = "%.2f";
    // Shown instead of the value while it is below 0 (user, 2026-11-09: a dodge's stamina slider at its lowest = "the refill held only").
    const char* negativeText = nullptr;
    float valueStep = 0;  // step of the +/- hotkeys (0 = automatic: about 1/50 of the range, rounded to 1-2-5)
    bool color = false;   // `value` is a colour 0xRRGGBB, shown as a colour picker (no slider); persisted exactly

    std::vector<std::string> choices;  // Choice: labels; the selected index is stored in `value`
    // Choice: the order the list shows them in (indices into `choices`, -1 a separator); empty = as listed. The stored index stays
    // the label's own (the melee swaps grouped by character without changing the saved choices).
    std::vector<int> choiceOrder;

    hotkeys::Binding hotkey;
    hotkeys::Binding hotkey2;  // a second shortcut (user, 2026-10-23: one for the keyboard, one for the pad)
    hotkeys::Binding hotkeyUp, hotkeyDown;  // sliders: raise / lower the value by one step
    hotkeys::Binding defaultHotkey;
    int applied = 0;          // Choice: index currently applied in the game
    bool enabled = false;
    bool wanted = false;      // persisted state; re-applied when it becomes available
    std::string lastError;
    bool defaultOn = false;   // Toggle enabled when settings.ini has no entry yet
    bool holdKey = false;     // the hotkey is a key held to act (read by the feature), not a toggle
};

const std::vector<Feature*>& List();  // registration order
Feature& Add(Feature f);
Feature* Find(const std::string& id);

// Called from the render thread.
bool SetEnabled(Feature& f, bool on);   // shows a toast
bool Trigger(Feature& f);               // Action, toggle flip, or next choice; shows a toast
bool Choose(Feature& f, int index);     // Choice: select + apply (or defer until available)
bool HasSlider(const Feature& f);       // a value slider with +/- hotkeys (not the combo-box index of a Choice)
void StepValue(Feature& f, int direction);  // +/- hotkey: one step, enables a toggle, shows a toast
void RestoreAll();                      // undo every toggle and choice (before unloading)
void RequestStop();                     // any thread: an unload is coming, nothing more is applied
void Frame();                           // hotkeys, ticks, deferred re-apply
void OnFrame(std::function<void()> task);  // extra work run every frame (always, not tied to an option)
void OnFrameFirst(std::function<void()> task);  // the same, before every other frame task
void OnHud(std::function<void()> draw);    // drawn every frame inside the ImGui frame, menu open or not
void DrawHud();                            // overlay: runs the OnHud callbacks
void OnRestore(std::function<void()> task);  // extra cleanup run by RestoreAll (unload)

// Custom overlay tab drawn with ImGui by a feature module (inventory editor...), after the option tabs.
struct Panel {
    std::string tab;
    std::function<void()> draw;
    std::vector<std::string> categories;  // options of these categories are shown inside the panel, no tab of their own
    bool ownScroll = false;  // the panel scrolls its content itself, below the sub-tabs it keeps in view
    bool inPlace = false;    // its tab where its first category's tab was (not after the category tabs)
};
void AddPanel(const std::string& tab, std::function<void()> draw, std::vector<std::string> categories = {},
              bool ownScroll = false, bool inPlace = false);
const std::vector<Panel>& Panels();
// The option table of one category, drawn by the overlay (for panels that host a category).
void SetOptionsDrawer(std::function<void(const std::string& category)> drawer);
void DrawOptions(const std::string& category);
void DrawOption(const std::string& id);  // one option's row, in the same table (a page that shows it once more)
// One button of a feature's shortcut (`second`: its 2nd shortcut; `hold`: the key to hold, else the key), drawn by the
// overlay, for panels with a table of their own (the free melee's).
void SetKeyButtonDrawer(std::function<void(Feature& f, bool second, bool hold)> drawer);
void DrawKeyButton(Feature& f, bool second, bool hold);

// Short on-screen messages ("Munitions sans réserve : ACTIVÉ").
void Toast(const std::string& text, bool ok = true);
struct ToastLine { std::string text; bool ok; unsigned long long until; };
const std::vector<ToastLine>& Toasts();

void RegisterAll();  // defined in features/*.cpp aggregator (dllmain)
}
