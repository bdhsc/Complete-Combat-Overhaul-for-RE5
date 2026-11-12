#include "ui/overlay.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/hotkeys.hpp"
#include "common/i18n.hpp"
#include "hooks/hooks.hpp"
#include "common/log.hpp"
#include "common/settings.hpp"

#include <windows.h>
#include <d3d9.h>
#include <imgui.h>
#include "ui/sliders.hpp"
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay {
namespace {
using i18n::Tr;

// A tab's label in the current language, its ImGui id the French name (the tab stays selected across languages).
std::string TabLabel(const std::string& french) { return Tr(french) + "###" + french; }
IDirect3DDevice9* g_device = nullptr;  // the device ImGui is bound to
HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;
bool g_open = false;
bool g_initFailed = false;
volatile bool g_shutdownRequested = false;
volatile bool g_shutdownDone = false;
hotkeys::Binding* g_captureTarget = nullptr;  // binding being rebound
bool g_captureHold = false;                    // its key to hold (else its key)
features::Feature* g_captureFeature = nullptr;

bool IsInputMessage(UINT msg) {
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEWHEEL && !g_open && hotkeys::WheelBlocked()) return 0;  // counted in the message loop hook
    if (g_open) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        if (IsInputMessage(msg)) return msg == WM_INPUT ? DefWindowProcW(hwnd, msg, wp, lp) : 0;
        if (msg == WM_SETCURSOR) return TRUE;
    }
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wp, lp);
}

// Pick the device that renders the game window (RE5 can create auxiliary devices).
bool IsGameDevice(IDirect3DDevice9* device, HWND window) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (FAILED(device->GetCreationParameters(&cp))) return false;
    IDirect3DSwapChain9* sc = nullptr;
    HWND presentWindow = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &sc)) && sc) {
        D3DPRESENT_PARAMETERS pp{};
        if (SUCCEEDED(sc->GetPresentParameters(&pp))) presentWindow = pp.hDeviceWindow;
        sc->Release();
    }
    return cp.hFocusWindow == window || presentWindow == window;
}

bool Init(IDirect3DDevice9* device) {
    HWND window = static_cast<HWND>(game::MainWindow());
    if (!window || !IsGameDevice(device, window)) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // our own settings.ini
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6;
    style.FrameRounding = 4;
    style.GrabRounding = 4;
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.45f, 0.12f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.30f, 0.10f, 0.09f, 1.0f);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.60f, 0.16f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.75f, 0.22f, 0.16f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.55f, 0.35f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.35f, 0.12f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.60f, 0.18f, 0.14f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.22f, 0.10f, 0.09f, 1.0f);

    if (!ImGui_ImplWin32_Init(window) || !ImGui_ImplDX9_Init(device)) {
        LOG("ImGui init failed");
        g_initFailed = true;
        return false;
    }
    g_window = window;
    g_device = device;
    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
    LOG("overlay bound to device %p, window %p", device, window);
    features::Toast(std::string(Tr("RE5 Trainer chargé — ")) + hotkeys::ToString(settings::UiSettings().menuKey) +
                    Tr(" pour le menu"));
    return true;
}

// The other options using the same shortcut (user, 2026-10-23: one key for several options is allowed, e.g. A for the
// traversal shortcut and the aim dodge; it is shown, not refused).
std::string SharedWith(const hotkeys::Binding& binding) {
    std::string names;
    if (!binding.IsBound()) return names;
    for (features::Feature* f : features::List())
        for (const hotkeys::Binding* other : { &f->hotkey, &f->hotkey2, &f->hotkeyUp, &f->hotkeyDown })
            if (other != &binding && *other == binding) {
                if (!names.empty()) names += ", ";
                names += f->name;
                break;
            }
    return names;
}

// A shortcut = an optional key to hold first ("Tenir : ...", the prerequisite) and the key itself (user, 2026-10-20).
// Each is its own button; the hold button always says "Tenir" so the two cannot be mixed up.
void KeyButton(const char* id, hotkeys::Binding& binding, bool hold, features::Feature* owner, const char* prefix,
               float width) {
    const bool capturingThis = hotkeys::Capturing() && g_captureTarget == &binding && g_captureHold == hold;
    const std::string shared = hold ? std::string() : SharedWith(binding);
    std::string label = prefix;
    if (hold) label += Tr("Tenir : ");
    label += capturingThis ? Tr("Appuyez…") : hotkeys::KeyName(hold ? binding.hold : binding.key);
    label += "##";
    label += id;
    label += hold ? "_hold" : "_key";
    if (hold && !binding.hold) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (!shared.empty()) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.45f, 0.05f, 1.0f));
    if (ImGui::Button(label.c_str(), ImVec2(width * settings::UiSettings().scale, 0))) {
        g_captureTarget = &binding;
        g_captureHold = hold;
        g_captureFeature = owner;
        hotkeys::StartCapture();
    }
    if (!shared.empty()) ImGui::PopStyleColor();
    if (hold && !binding.hold) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        if (hold)
            ImGui::SetTooltip("%s", Tr("Touche à maintenir avant le raccourci (facultative).\nVide : le raccourci marche même si "
                                     "d'autres touches sont tenues (Maj pour courir...).\nCliquer puis appuyer sur une touche. "
                                     "Échap = annuler, Retour arrière = effacer."));
        else if (!shared.empty())
            ImGui::SetTooltip(Tr("Le raccourci lui-même.\nAussi utilisé par : %s (toutes ces options se déclenchent "
                              "ensemble).\nCliquer puis appuyer sur une touche ou un bouton de souris (clic gauche compris : dans une zone vide du menu). Échap = annuler, "
                              "Retour arrière = effacer."),
                              shared.c_str());
        else
            ImGui::SetTooltip("%s", Tr("Le raccourci lui-même.\nCliquer puis appuyer sur une touche ou un bouton de souris (clic gauche compris : dans une zone vide du menu). "
                                     "Échap = annuler, Retour arrière = effacer."));
    }
}

// Both buttons of a shortcut on one line: [Tenir : ...] [key].
void HotkeyButton(const char* id, hotkeys::Binding& binding, features::Feature* owner, const char* prefix = "",
                  float width = 110) {
    KeyButton(id, binding, true, owner, prefix, width);
    ImGui::SameLine(0, 2);
    KeyButton(id, binding, false, owner, prefix, width);
}

void PollCapture() {
    uint16_t key = 0;
    bool cancelled = false;
    if (!g_captureTarget || !hotkeys::PollCapture(key, cancelled)) return;
    if (!cancelled) {
        hotkeys::Binding result = *g_captureTarget;
        if (g_captureHold) result.hold = key;
        else if (key) result.key = key;
        else result = hotkeys::Binding{};  // the key cleared: no shortcut left
        if (result.hold == result.key) result.hold = 0;
        // A shortcut may serve several options (user, 2026-10-23: it used to be taken from the other one): a warning.
        *g_captureTarget = result;
        const std::string shared = SharedWith(result);
        if (!shared.empty()) features::Toast(Tr("Raccourci partagé avec : ") + shared, false);
        if (g_captureFeature) settings::SaveFeature(*g_captureFeature);
        else settings::SaveUi();
    }
    g_captureTarget = nullptr;
    g_captureFeature = nullptr;
}

void FeatureRow(features::Feature& f) {
    ImGui::PushID(f.id.c_str());
    const bool available = !f.available || f.available();
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (!available) ImGui::BeginDisabled();
    if (f.kind == features::Kind::Toggle) {
        bool on = f.enabled;
        if (ImGui::Checkbox(Tr(f.name).c_str(), &on)) features::SetEnabled(f, on);
    } else if (f.kind == features::Kind::Choice) {
        int index = static_cast<int>(f.value);
        if (index < 0 || index >= static_cast<int>(f.choices.size())) index = 0;
        ImGui::SetNextItemWidth(220 * settings::UiSettings().scale);
        if (ImGui::BeginCombo("##choice", Tr(f.choices[index]).c_str(), ImGuiComboFlags_HeightLarge)) {
            const int shown = f.choiceOrder.empty() ? static_cast<int>(f.choices.size()) : static_cast<int>(f.choiceOrder.size());
            for (int n = 0; n < shown; ++n) {
                const int k = f.choiceOrder.empty() ? n : f.choiceOrder[n];
                if (k < 0 || k >= static_cast<int>(f.choices.size())) {  // a separator between groups
                    ImGui::Separator();
                    continue;
                }
                const bool selected = k == index;
                ImGui::PushID(k);  // entries are identified by index: identical labels stay selectable
                if (ImGui::Selectable(Tr(f.choices[k]).c_str(), selected)) features::Choose(f, k);
                ImGui::PopID();
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(Tr(f.name).c_str());
    } else {
        if (ImGui::Button(Tr(f.name).c_str())) features::Trigger(f);
    }
    if (!available) ImGui::EndDisabled();
    if (!f.help.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", Tr(f.help).c_str());
    if (f.kind != features::Kind::Choice && f.valueMax > f.valueMin) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160 * settings::UiSettings().scale);
        widgets::SliderFloat("##value", &f.value, f.valueMin, f.valueMax,
                             Tr(f.negativeText && f.value < 0.0f ? f.negativeText : f.valueFormat));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Glisser pour régler, clic simple pour taper la valeur."));
        if (ImGui::IsItemDeactivatedAfterEdit()) settings::SaveFeature(f);
        ImGui::SameLine();
        HotkeyButton("up", f.hotkeyUp, &f, "+ ", 95);
        ImGui::SameLine();
        HotkeyButton("down", f.hotkeyDown, &f, "- ", 95);
    }
    // A colour option (user, 2026-10-24: any colour, not only the game's): RGB picker with hue bar and values, saved
    // once the edit is over (no ini write per frame while dragging).
    if (f.color) {
        static features::Feature* dirty = nullptr;
        ImGui::SameLine();
        const uint32_t rgb = static_cast<uint32_t>(f.value + 0.5f);
        float col[3] = { static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                         static_cast<float>(rgb & 0xFF) / 255.0f };
        ImGui::SetNextItemWidth(260 * settings::UiSettings().scale);
        if (ImGui::ColorEdit3("##color", col, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_PickerHueBar)) {
            const auto byte = [](float v) {
                return static_cast<uint32_t>((v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v) * 255.0f + 0.5f);
            };
            f.value = static_cast<float>((byte(col[0]) << 16) | (byte(col[1]) << 8) | byte(col[2]));
            if (dirty && dirty != &f) settings::SaveFeature(*dirty);
            dirty = &f;
        }
        if (dirty == &f && !ImGui::IsAnyItemActive()) {
            settings::SaveFeature(f);
            dirty = nullptr;
        }
    }
    if (!f.lastError.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "(%s)", f.lastError.c_str());
    }
    ImGui::TableSetColumnIndex(1);
    KeyButton("hk", f.hotkey, true, &f, "", 115);
    ImGui::TableSetColumnIndex(2);
    KeyButton("hk", f.hotkey, false, &f, "", 115);
    // A second shortcut (user, 2026-10-23: a key for the keyboard and a pad button for the same option).
    ImGui::TableSetColumnIndex(3);
    KeyButton("hk2", f.hotkey2, true, &f, "", 115);
    ImGui::TableSetColumnIndex(4);
    KeyButton("hk2", f.hotkey2, false, &f, "", 115);
    ImGui::PopID();
}

// A category's options, or one option when `category` is '\x01' + its id (features::DrawOption).
void DrawOptionTable(const std::string& category) {
    const bool single = !category.empty() && category[0] == '\x01';
    const std::string id = single ? category.substr(1) : std::string();
    if (ImGui::BeginTable("rows", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(Tr("Option"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(Tr("À maintenir (facultatif)"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(Tr("Raccourci"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(Tr("2e : à maintenir"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(Tr("2e raccourci (manette...)"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (features::Feature* f : features::List())
            if (single ? f->id == id : f->category == category) FeatureRow(*f);
        ImGui::EndTable();
    }
}

void DrawMenu() {
    static const bool optionsDrawer = (features::SetOptionsDrawer(DrawOptionTable), true);
    (void)optionsDrawer;
    static const bool keyButtonDrawer = (features::SetKeyButtonDrawer([](features::Feature& f, bool second, bool hold) {
                                             KeyButton(second ? "hk2" : "hk", second ? f.hotkey2 : f.hotkey, hold, &f, "", 100);
                                         }),
                                         true);
    (void)keyButtonDrawer;
    settings::Ui& ui = settings::UiSettings();
    ImGui::SetNextWindowPos(ImVec2(ui.x, ui.y), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ui.w > 0 ? ImVec2(ui.w, ui.h) : ImVec2(620 * ui.scale, 520 * ui.scale), ImGuiCond_Once);
    if (ImGui::Begin("RE5 Trainer", &g_open, ImGuiWindowFlags_NoCollapse)) {
        if (!game::IsSupported())
            ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1), Tr("Version du jeu %s non validée (attendue %s) : options désactivées."),
                               game::Version().c_str(), game::kSupportedVersion);

        // Categories hosted by a panel (the inventory options in the inventory editor) get no tab of their own; an
        // "in place" panel (Combat, user 2026-10-28: its knife sub-tab) takes the place of its first category's tab.
        std::vector<std::string> categories;
        for (const features::Panel& panel : features::Panels())
            for (const std::string& hosted : panel.categories) categories.push_back(hosted);
        const size_t hosted = categories.size();
        std::vector<const features::Panel*> inPlace(hosted, nullptr);  // by index in `categories`
        for (const features::Panel& panel : features::Panels())
            if (panel.inPlace)
                for (const std::string& name : panel.categories)
                    inPlace[std::find(categories.begin(), categories.end(), name) - categories.begin()] = &panel;
        std::vector<const features::Panel*> tabPanels;  // nullptr: the category tab of the same index in `categories`
        for (features::Feature* f : features::List()) {
            const auto it = std::find(categories.begin(), categories.end(), f->category);
            if (it == categories.end()) {
                categories.push_back(f->category);
                tabPanels.push_back(nullptr);
            } else if (const size_t index = static_cast<size_t>(it - categories.begin()); index < hosted && inPlace[index]) {
                const features::Panel* panel = inPlace[index];
                for (const std::string& name : panel->categories)
                    inPlace[std::find(categories.begin(), categories.end(), name) - categories.begin()] = nullptr;
                categories.push_back(panel->tab);
                tabPanels.push_back(panel);
            }
        }
        categories.erase(categories.begin(), categories.begin() + static_cast<std::ptrdiff_t>(hosted));

        // The tabs stay in view (user, 2026-10-23): each tab's content scrolls in its own region below them; a panel
        // with sub-tabs keeps them in view the same way (Panel::ownScroll).
        const auto beginScroll = [] { ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_None); };
        const auto drawPanel = [&beginScroll](const features::Panel& panel) {
            if (panel.ownScroll) {
                panel.draw();
            } else {
                beginScroll();
                panel.draw();
                ImGui::EndChild();
            }
        };
        if (ImGui::BeginTabBar("tabs")) {
            for (size_t i = 0; i < categories.size(); ++i) {
                const std::string& cat = categories[i];
                if (!ImGui::BeginTabItem(TabLabel(cat).c_str())) continue;
                if (tabPanels[i]) {
                    drawPanel(*tabPanels[i]);
                } else {
                    beginScroll();
                    DrawOptionTable(cat);
                    ImGui::EndChild();
                }
                ImGui::EndTabItem();
            }
            for (const features::Panel& panel : features::Panels()) {
                if (panel.inPlace || !ImGui::BeginTabItem(TabLabel(panel.tab).c_str())) continue;
                if (panel.ownScroll) {
                    panel.draw();
                } else {
                    beginScroll();
                    panel.draw();
                    ImGui::EndChild();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(TabLabel("Raccourcis").c_str())) {
                beginScroll();
                ImGui::TextDisabled("%s", Tr("Tous les raccourcis actifs"));
                if (ImGui::BeginTable("all", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
                    ImGui::TableSetupColumn(Tr("À maintenir"));
                    ImGui::TableSetupColumn(Tr("Touche"));
                    ImGui::TableSetupColumn(Tr("Option"));
                    ImGui::TableSetupColumn(Tr("Onglet"));
                    ImGui::TableHeadersRow();
                    for (features::Feature* f : features::List()) {
                        const std::pair<const hotkeys::Binding*, const char*> keys[] = {
                            { &f->hotkey, "" }, { &f->hotkey2, Tr(" (2e)") }, { &f->hotkeyUp, " (+)" }, { &f->hotkeyDown, " (-)" } };
                        for (const auto& [binding, suffix] : keys) {
                            if (!binding->IsBound()) continue;
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            if (binding->hold) ImGui::TextUnformatted(hotkeys::KeyName(binding->hold).c_str());
                            else ImGui::TextDisabled("—");
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(hotkeys::KeyName(binding->key).c_str());
                            ImGui::TableSetColumnIndex(2);
                            ImGui::Text("%s%s", Tr(f->name).c_str(), suffix);
                            ImGui::TableSetColumnIndex(3);
                            ImGui::TextDisabled("%s", Tr(f->category).c_str());
                        }
                    }
                    ImGui::EndTable();
                }
                if (ImGui::Button(Tr("Remettre les raccourcis par défaut"))) {
                    for (features::Feature* f : features::List()) {
                        f->hotkey = f->defaultHotkey;
                        f->hotkey2 = f->hotkeyUp = f->hotkeyDown = hotkeys::Binding{};
                        settings::SaveFeature(*f);
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(TabLabel("Réglages").c_str())) {
                beginScroll();
                // The trainer's language (user, 2026-11-02; i18n.hpp): English by default.
                ImGui::TextUnformatted("Langue / Language / Idioma :");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160 * ui.scale);
                if (ImGui::BeginCombo("##language", i18n::Name(i18n::Current()))) {
                    for (int k = 0; k < i18n::kLangCount; ++k) {
                        const auto lang = static_cast<i18n::Lang>(k);
                        if (ImGui::Selectable(i18n::Name(lang), lang == i18n::Current())) {
                            i18n::Set(lang);
                            ui.language = i18n::Code(lang);
                            settings::SaveUi();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::Text("%s", Tr("Ouvrir / fermer le menu :"));
                ImGui::SameLine();
                HotkeyButton("menu", ui.menuKey, nullptr);
                if (ImGui::Checkbox(Tr("Taille automatique (selon la résolution)"), &ui.autoScale)) settings::SaveUi();
                if (ui.autoScale) ImGui::BeginDisabled();
                if (widgets::SliderFloat(Tr("Taille du menu"), &ui.scale, 0.75f, 2.5f, "%.2f")) settings::SaveUi();
                if (ui.autoScale) ImGui::EndDisabled();
                if (ImGui::Checkbox(Tr("Messages à l'écran quand une option change"), &ui.toasts)) settings::SaveUi();
                ImGui::Separator();
                ImGui::TextDisabled(Tr("Jeu %s  |  réglages : %%LOCALAPPDATA%%\\RE5TrainerPlus\\settings.ini"), game::Version().c_str());
                ImGui::TextDisabled("%s", Tr("Journal : %LOCALAPPDATA%\\RE5TrainerPlus\\trainer.log"));
                if (ImGui::Button(Tr("Exporter les textes à traduire"))) i18n::ExportStrings();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        // Position and size are saved once the mouse button is released after a move or a resize.
        static bool unsaved = false;
        const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        if (pos.x != ui.x || pos.y != ui.y || size.x != ui.w || size.y != ui.h) {
            ui.x = pos.x;
            ui.y = pos.y;
            ui.w = size.x;
            ui.h = size.y;
            unsaved = true;
        }
        if (unsaved && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            settings::SaveUi();
            unsaved = false;
        }
    }
    ImGui::End();
}

void DrawToasts() {
    if (!settings::UiSettings().toasts) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float y = 20;
    for (const auto& t : features::Toasts()) {
        ImVec2 size = ImGui::CalcTextSize(t.text.c_str());
        ImVec2 p(20, y);
        dl->AddRectFilled(ImVec2(p.x - 8, p.y - 4), ImVec2(p.x + size.x + 8, p.y + size.y + 4), IM_COL32(15, 10, 10, 200), 4);
        dl->AddText(p, t.ok ? IM_COL32(255, 230, 200, 255) : IM_COL32(255, 110, 90, 255), t.text.c_str());
        y += size.y + 12;
    }
}
}

bool IsOpen() { return g_open; }
bool BlocksGameInput() { return g_open && g_window && GetForegroundWindow() == g_window && !g_shutdownDone; }

void RequestShutdown() { g_shutdownRequested = true; }
bool ShutdownDone() { return g_shutdownDone; }

namespace {
// Runs on the render thread: undo every game change and release ImGui so the DLL can be unloaded.
void Shutdown() {
    features::RestoreAll();  // `wanted` / chosen values stay saved: the next instance re-applies them
    if (g_device) {
        if (g_originalWndProc) SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_device = nullptr;
    }
    g_open = false;
    g_shutdownDone = true;
    LOG("overlay shut down");
}

float AutoScale() {
    RECT r{};
    if (!g_window || !GetClientRect(g_window, &r)) return 1.0f;
    float s = static_cast<float>(r.bottom - r.top) / 1080.0f;
    return s < 1.0f ? 1.0f : (s > 2.5f ? 2.5f : s);
}
}

void OnPresent(IDirect3DDevice9* device) {
    if (g_shutdownDone) return;
    if (g_shutdownRequested) {
        Shutdown();
        return;
    }
    if (g_initFailed) return;
    if (!g_device) {
        static ULONGLONG nextTry = 0;
        if (GetTickCount64() < nextTry) return;
        nextTry = GetTickCount64() + 500;
        if (!Init(device)) return;
    }
    if (device != g_device) return;

    const bool focused = GetForegroundWindow() == g_window;
    hotkeys::BeginFrame(focused);
    PollCapture();
    if (!hotkeys::Capturing() && hotkeys::Pressed(settings::UiSettings().menuKey)) g_open = !g_open;
    hotkeys::SetMenuOpen(g_open);
    features::Frame();

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().MouseDrawCursor = g_open;
    settings::Ui& ui = settings::UiSettings();
    if (ui.autoScale) ui.scale = AutoScale();
    ImGui::GetStyle().FontScaleMain = ui.scale;
    ImGui::NewFrame();
    if (g_open) DrawMenu();
    features::DrawHud();
    DrawToasts();
    ImGui::EndFrame();

    if (SUCCEEDED(device->BeginScene())) {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device->EndScene();
    }
}

void OnReset(IDirect3DDevice9* device, bool before) {
    if (!g_device || device != g_device) return;
    if (before) ImGui_ImplDX9_InvalidateDeviceObjects();
    else ImGui_ImplDX9_CreateDeviceObjects();
}
}
