#pragma once
#include <cstdint>
#include <string>

namespace hotkeys {

// Mouse wheel notches as pseudo keys (VK 0x0A / 0x0B are reserved by Windows, never reported by GetAsyncKeyState).
constexpr uint16_t kWheelUp = 0x0A;
constexpr uint16_t kWheelDown = 0x0B;
// Gamepad buttons (XInput pad 0) as pseudo keys after the 256 VKs: 0x100 + the XINPUT_GAMEPAD bit index (0 croix haut,
// 1 bas, 2 gauche, 3 droite, 4 Start, 5 Back, 6 L3, 7 R3, 8 LB, 9 RB, 12 A, 13 B, 14 X, 15 Y), 0x110 LT, 0x111 RT,
// then the sticks pushed past half way (user, 2026-10-23: "shortcuts must read every pad input"): 0x112..0x115 left
// stick up / down / left / right, 0x116..0x119 right stick up / down / left / right.
constexpr uint16_t kPadFirst = 0x100;
constexpr uint16_t kPadLT = 0x110;
constexpr uint16_t kPadRT = 0x111;
constexpr uint16_t kPadLeftStick = 0x112;
constexpr uint16_t kPadRightStick = 0x116;
constexpr uint16_t kKeyCount = 0x11A;

// A key (keyboard VK or mouse button VK) and an optional key to hold first (user, 2026-10-20: with exact Ctrl / Maj /
// Alt combinations, a key did nothing while Maj was held to run). Without a held key the key works whatever else is
// held; with one, only while it is held. Any key can be either (Maj droite alone, Souris 5 held + E...).
struct Binding {
    uint16_t key = 0;   // 0 = unbound
    uint16_t hold = 0;  // 0 = none

    bool IsBound() const { return key != 0; }
    bool operator==(const Binding&) const = default;
};

std::string KeyName(uint16_t vk);               // "F1", "Souris 5", "—" for 0
std::string ToString(const Binding& b);         // "Maj gauche + C", "Souris 5", "—"
std::string Serialize(const Binding& b);        // "0x43", "0xA0+0x43" (hold first), "none"
Binding Parse(const std::string& text);         // also the old "C+S+A+0x70" (Ctrl / Maj / Alt become the held key)

// Polling (call once per frame from the render hook).
void BeginFrame(bool gameFocused);
void SetMenuOpen(bool open);                    // the shortcuts stay silent while the menu is open
bool MenuOpen();
bool Pressed(const Binding& b);                 // edge of the key this frame, its held key (if any) down
bool Held(uint16_t key);
bool Held(const Binding& b);                    // the key and its held key (if any) down

// Mouse wheel from the input hooks (positive = forward). Blocked = the game no longer sees it (the trainer does).
void AddWheel(int delta);
// The game's DirectInput keyboard state (256 scan code bytes), read by the input hooks: its numpad digits.
void NoteKeyboardState(const unsigned char* keys);
void SetWheelBlocked(bool on);
bool WheelBlocked();

// Rebinding one key: PollCapture returns true once a key was pressed (out = it); Escape cancels (*cancelled),
// Backspace / Delete clears (out = 0).
void StartCapture();
bool Capturing();
bool PollCapture(uint16_t& out, bool& cancelled);
}
