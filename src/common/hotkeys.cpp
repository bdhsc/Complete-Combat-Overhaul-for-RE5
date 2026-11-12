#include "common/hotkeys.hpp"

#include "common/actors.hpp"

#include <windows.h>
#include <xinput.h>
#include <array>
#include <atomic>
#include <cstdio>

namespace hotkeys {
namespace {
std::array<bool, kKeyCount> g_now{};
std::array<bool, kKeyCount> g_prev{};

// XInput, loaded at run time (xinput1_4 / 1_3 / 9_1_0, whichever the system has). Shortcuts can be pad buttons
// (user, 2026-10-20: a pad reload key, the game's pad reload being aim + run).
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn LoadXInput() {
    for (const wchar_t* dll : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" })
        if (HMODULE m = LoadLibraryW(dll))
            if (auto f = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"))) return f;
    return nullptr;
}

void PollPad(bool gameFocused) {
    static XInputGetStateFn getState = LoadXInput();
    XINPUT_STATE state{};
    const bool ok = gameFocused && getState && getState(0, &state) == ERROR_SUCCESS;
    for (int bit = 0; bit < 16; ++bit) g_now[kPadFirst + bit] = ok && (state.Gamepad.wButtons >> bit & 1);
    g_now[kPadLT] = ok && state.Gamepad.bLeftTrigger > 60;
    g_now[kPadRT] = ok && state.Gamepad.bRightTrigger > 60;
    // Sticks past half way, up / down / left / right (this XInput is not the game's: the right stick hidden from the
    // game by the free camera still reads here).
    constexpr int kHalf = 16384;
    const SHORT sticks[2][2] = { { state.Gamepad.sThumbLX, state.Gamepad.sThumbLY },
                                 { state.Gamepad.sThumbRX, state.Gamepad.sThumbRY } };
    for (int s = 0; s < 2; ++s) {
        const uint16_t first = s == 0 ? kPadLeftStick : kPadRightStick;
        g_now[first + 0] = ok && sticks[s][1] > kHalf;
        g_now[first + 1] = ok && sticks[s][1] < -kHalf;
        g_now[first + 2] = ok && sticks[s][0] < -kHalf;
        g_now[first + 3] = ok && sticks[s][0] > kHalf;
    }
}
// The numpad's digits as the keyboard holds them (user, 2026-10-30: "Me mettre mourant" on Pavé 5 did nothing while
// sprinting): with Maj held Windows turns a numpad digit into its navigation key (5 -> Clear, 8 -> Haut...), so
// GetAsyncKeyState never sees VK_NUMPADn; the game's DirectInput keyboard (scan codes) still holds them.
constexpr unsigned char kNumpadDik[11] = { 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x53 };  // 0-9, .
std::atomic<uint16_t> g_dinputNumpad{ 0 };
std::atomic<ULONGLONG> g_dinputNumpadTick{ 0 };

bool g_capturing = false;
bool g_captureArmed = false;  // wait until every key is released before accepting one
std::atomic<int> g_wheel{ 0 };
std::atomic<bool> g_wheelBlocked{ false };
bool g_menuOpen = false;

// The left and right clicks operate the menu (user, 2026-10-23: the left click may now be a shortcut, e.g. melee).
bool MenuClick(uint16_t key) { return g_menuOpen && (key == VK_LBUTTON || key == VK_RBUTTON); }

bool IsWheel(int vk) { return vk == kWheelUp || vk == kWheelDown; }
// Either-side Ctrl / Maj / Alt: GetAsyncKeyState also reports the sided codes; a capture takes those.
bool IsGenericModifier(int vk) { return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU; }
}

std::string KeyName(uint16_t vk) {
    if (vk >= kPadFirst && vk < kKeyCount) {
        static const char* names[] = { "Manette croix haut", "Manette croix bas", "Manette croix gauche", "Manette croix droite",
                                       "Manette Start", "Manette Back", "Manette L3", "Manette R3", "Manette LB",
                                       "Manette RB", "Manette 10", "Manette 11", "Manette A", "Manette B", "Manette X",
                                       "Manette Y", "Manette LT", "Manette RT", "Stick gauche haut", "Stick gauche bas",
                                       "Stick gauche gauche", "Stick gauche droite", "Stick droit haut", "Stick droit bas",
                                       "Stick droit gauche", "Stick droit droite" };
        return names[vk - kPadFirst];
    }
    switch (vk) {
    case 0: return "—";
    case VK_SHIFT: return "Maj";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case kWheelUp: return "Molette haut";
    case kWheelDown: return "Molette bas";
    case VK_LSHIFT: return "Maj gauche";
    case VK_RSHIFT: return "Maj droite";
    case VK_LCONTROL: return "Ctrl gauche";
    case VK_RCONTROL: return "Ctrl droite";
    case VK_LMENU: return "Alt";
    case VK_RMENU: return "Alt Gr";
    case VK_LBUTTON: return "Clic gauche";
    case VK_RBUTTON: return "Clic droit";
    case VK_MBUTTON: return "Clic molette";
    case VK_XBUTTON1: return "Souris 4";
    case VK_XBUTTON2: return "Souris 5";
    case VK_INSERT: return "Inser";
    case VK_DELETE: return "Suppr";
    case VK_HOME: return "Origine";
    case VK_END: return "Fin";
    case VK_PRIOR: return "Page haut";
    case VK_NEXT: return "Page bas";
    case VK_UP: return "Haut";
    case VK_DOWN: return "Bas";
    case VK_LEFT: return "Gauche";
    case VK_RIGHT: return "Droite";
    case VK_SPACE: return "Espace";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Entrée";
    case VK_CAPITAL: return "Verr. Maj";
    case VK_MULTIPLY: return "Pavé *";
    case VK_ADD: return "Pavé +";
    case VK_SUBTRACT: return "Pavé -";
    case VK_DECIMAL: return "Pavé .";
    case VK_DIVIDE: return "Pavé /";
    }
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Pavé " + std::to_string(vk - VK_NUMPAD0);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, static_cast<char>(vk));
    // Layout-aware name for punctuation keys (AZERTY etc.).
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    char name[64]{};
    if (scan && GetKeyNameTextA(static_cast<LONG>(scan << 16), name, sizeof(name)) > 0) return name;
    char buf[16];
    sprintf_s(buf, "Touche 0x%02X", vk);
    return buf;
}

std::string ToString(const Binding& b) {
    if (!b.IsBound()) return "—";
    return b.hold ? KeyName(b.hold) + " + " + KeyName(b.key) : KeyName(b.key);
}

std::string Serialize(const Binding& b) {
    if (!b.IsBound()) return "none";
    char buf[32];
    if (b.hold) sprintf_s(buf, "0x%02X+0x%02X", b.hold, b.key);
    else sprintf_s(buf, "0x%02X", b.key);
    return buf;
}

Binding Parse(const std::string& text) {
    Binding b;
    if (text.empty() || text == "none") return b;
    // Tokens separated by '+': hex keys (held key first) or the old modifier letters C / S / A.
    uint16_t keys[2]{};
    int count = 0;
    uint16_t oldModifier = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('+', start);
        if (end == std::string::npos) end = text.size();
        const std::string token = text.substr(start, end - start);
        if (token == "C" && !oldModifier) oldModifier = VK_CONTROL;
        else if (token == "S" && !oldModifier) oldModifier = VK_SHIFT;
        else if (token == "A" && !oldModifier) oldModifier = VK_MENU;
        else if (!token.empty() && count < 2) keys[count++] = static_cast<uint16_t>(strtoul(token.c_str(), nullptr, 16));
        start = end + 1;
    }
    if (count == 2) {
        b.hold = keys[0];
        b.key = keys[1];
    } else if (count == 1) {
        b.key = keys[0];
        b.hold = oldModifier;
    }
    return b;
}

void BeginFrame(bool gameFocused) {
    g_prev = g_now;
    for (int vk = 1; vk < 256; ++vk) g_now[vk] = gameFocused && (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (gameFocused && GetTickCount64() - g_dinputNumpadTick.load(std::memory_order_relaxed) < 250) {
        const uint16_t numpad = g_dinputNumpad.load(std::memory_order_relaxed);
        for (int i = 0; i < 10; ++i)
            if (numpad >> i & 1) g_now[VK_NUMPAD0 + i] = true;
        if (numpad >> 10 & 1) g_now[VK_DECIMAL] = true;
    }
    PollPad(gameFocused);
    const int wheel = g_wheel.exchange(0);  // a notch is a one-frame "press"
    g_now[kWheelUp] = gameFocused && wheel > 0;
    g_now[kWheelDown] = gameFocused && wheel < 0;
}

void SetMenuOpen(bool open) { g_menuOpen = open; }

bool MenuOpen() { return g_menuOpen; }

void AddWheel(int delta) { g_wheel += delta; }

void NoteKeyboardState(const unsigned char* keys) {
    uint16_t numpad = 0;
    for (int i = 0; i < 11; ++i)
        if (keys[kNumpadDik[i]] & 0x80) numpad |= static_cast<uint16_t>(1u << i);
    g_dinputNumpad.store(numpad, std::memory_order_relaxed);
    g_dinputNumpadTick.store(GetTickCount64(), std::memory_order_relaxed);
}
void SetWheelBlocked(bool on) { g_wheelBlocked = on; }
// Not in the game's menus: no player loaded, the wheel is the game's (user, 2026-10-27: "handy in the menus").
bool WheelBlocked() { return g_wheelBlocked && actors::InGame(); }

bool Held(uint16_t key) { return key < kKeyCount && g_now[key] && !MenuClick(key); }

// The options' shortcuts are silent while the menu is open (user, 2026-10-29: digits typed into a slider's box moved
// player 1 up / down and switched collisions off): the menu key itself is read with Pressed by the overlay.
bool Held(const Binding& b) { return !g_menuOpen && b.IsBound() && Held(b.key) && (!b.hold || Held(b.hold)); }

bool Pressed(const Binding& b) {
    if (!b.IsBound() || g_capturing || b.key >= kKeyCount || b.hold >= kKeyCount || MenuClick(b.key)) return false;
    // Wheel notches fire every frame they happen (fast scrolling = consecutive frames).
    if (!(g_now[b.key] && (IsWheel(b.key) || !g_prev[b.key]))) return false;
    return !b.hold || g_now[b.hold];
}

void StartCapture() {
    g_capturing = true;
    g_captureArmed = false;
}

bool Capturing() { return g_capturing; }

bool PollCapture(uint16_t& out, bool& cancelled) {
    cancelled = false;
    if (!g_capturing) return false;
    bool anyDown = false;
    for (int vk = 1; vk < kKeyCount; ++vk) {
        if (!g_now[vk] || IsGenericModifier(vk)) continue;
        if (!IsWheel(vk)) anyDown = true;
        if (!g_captureArmed || (g_prev[vk] && !IsWheel(vk))) continue;
        // The left click too (user, 2026-10-23): the click that opened the capture was released first (armed).
        g_capturing = false;
        if (vk == VK_ESCAPE) {
            cancelled = true;
            return true;
        }
        out = vk == VK_BACK || vk == VK_DELETE ? 0 : static_cast<uint16_t>(vk);
        return true;
    }
    if (!anyDown) g_captureArmed = true;
    return false;
}
}
