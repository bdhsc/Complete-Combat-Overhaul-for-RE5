#include "hooks/hooks.hpp"
#include "common/hotkeys.hpp"
#include "common/log.hpp"
#include "ui/overlay.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>

// While the overlay is open the game must not see keyboard/mouse input (camera, shooting...).
namespace hooks {
namespace {
using GetStateFn = HRESULT(__stdcall*)(void*, DWORD, LPVOID);
using GetDataFn = HRESULT(__stdcall*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

GetStateFn g_stateA = nullptr, g_stateW = nullptr;
GetDataFn g_dataA = nullptr, g_dataW = nullptr;

// The mouse wheel goes to the trainer's "Molette haut / bas" hotkeys (menu closed, or while a key is being
// captured) and, when it is reserved for the trainer, never reaches the game.
std::atomic<unsigned long long> g_lastMouseRead{ 0 };

// Look frozen (the RE6-style quick shot, 2026-10-21: moving the mouse during the shot missed the enemy): the game sees
// no mouse movement (raw input and DirectInput) and a centred right stick (XInput) meanwhile.
std::atomic<bool> g_lookFrozen{ false };
bool LookFrozen() { return g_lookFrozen.load(std::memory_order_relaxed) && !overlay::IsOpen(); }

bool IsMouse(void* self) {
    static void* known[8] = {};
    static bool mouse[8] = {};
    static int count = 0;
    for (int i = 0; i < count; ++i)
        if (known[i] == self) return mouse[i];
    using CapsFn = HRESULT(__stdcall*)(void*, LPDIDEVCAPS);
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    const auto getCaps = reinterpret_cast<CapsFn>((*reinterpret_cast<void***>(self))[3]);  // GetCapabilities
    const bool isMouse = SUCCEEDED(getCaps(self, &caps)) && GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_MOUSE;
    if (count < 8) {
        known[count] = self;
        mouse[count++] = isMouse;
    }
    return isMouse;
}

void Wheel(LONG& z) {
    if (!z) return;
    if (!overlay::IsOpen() || hotkeys::Capturing()) hotkeys::AddWheel(z);
    if (hotkeys::WheelBlocked()) z = 0;
}

HRESULT FilterState(void* self, HRESULT hr, DWORD size, LPVOID data) {
    if (SUCCEEDED(hr) && data && size >= sizeof(DIMOUSESTATE) && size <= sizeof(DIMOUSESTATE2) && IsMouse(self)) {
        g_lastMouseRead = GetTickCount64();
        Wheel(static_cast<DIMOUSESTATE*>(data)->lZ);
        if (LookFrozen()) static_cast<DIMOUSESTATE*>(data)->lX = static_cast<DIMOUSESTATE*>(data)->lY = 0;
    }
    if (SUCCEEDED(hr) && data && size == 256 && !IsMouse(self)) hotkeys::NoteKeyboardState(static_cast<const unsigned char*>(data));
    if (SUCCEEDED(hr) && data && overlay::BlocksGameInput()) memset(data, 0, size);
    return hr;
}

HRESULT FilterData(void* self, HRESULT hr, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags) {
    if (SUCCEEDED(hr) && data && inOut && !(flags & DIGDD_PEEK) && size >= sizeof(DIDEVICEOBJECTDATA_DX3) && IsMouse(self)) {
        g_lastMouseRead = GetTickCount64();
        for (DWORD i = 0; i < *inOut; ++i) {
            auto* entry = reinterpret_cast<LPDIDEVICEOBJECTDATA>(reinterpret_cast<BYTE*>(data) + i * size);
            if ((entry->dwOfs == DIMOFS_X || entry->dwOfs == DIMOFS_Y) && LookFrozen()) entry->dwData = 0;
            if (entry->dwOfs != DIMOFS_Z) continue;
            LONG z = static_cast<LONG>(entry->dwData);
            Wheel(z);
            entry->dwData = static_cast<DWORD>(z);
        }
    }
    if (SUCCEEDED(hr) && inOut && !(flags & DIGDD_PEEK) && overlay::BlocksGameInput()) *inOut = 0;
    return hr;
}

HRESULT __stdcall StateA(void* self, DWORD size, LPVOID data) { return FilterState(self, g_stateA(self, size, data), size, data); }
HRESULT __stdcall StateW(void* self, DWORD size, LPVOID data) { return FilterState(self, g_stateW(self, size, data), size, data); }
HRESULT __stdcall DataA(void* self, DWORD sz, LPDIDEVICEOBJECTDATA d, LPDWORD n, DWORD f) { return FilterData(self, g_dataA(self, sz, d, n, f), sz, d, n, f); }
HRESULT __stdcall DataW(void* self, DWORD sz, LPDIDEVICEOBJECTDATA d, LPDWORD n, DWORD f) { return FilterData(self, g_dataW(self, sz, d, n, f), sz, d, n, f); }

template <class Fn>
bool Hook(void* target, void* detour, Fn* original, void* alreadyHooked) {
    if (!target || target == alreadyHooked) return true;
    return MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) == MH_OK && MH_EnableHook(target) == MH_OK;
}
}

unsigned long long LastDInputMouseRead() { return g_lastMouseRead; }

// RE5 reads the keyboard through DirectInput but the mouse through raw input (GetRawInputData, RID_INPUT: the wheel
// still switched weapons with WM_MOUSEWHEEL removed, 2026-10-13). The trainer counts the wheel from WM_MOUSEWHEEL
// in PeekMessage / GetMessage (once, when the message leaves the queue) and, when the wheel is reserved for it,
// hides it from the game in both places (WM_NULL, raw RI_MOUSE_WHEEL cleared), except while the menu is open (ImGui
// scrolls with it).
namespace {
using PeekFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using GetFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT);
PeekFn g_peekA = nullptr, g_peekW = nullptr;
GetFn g_getA = nullptr, g_getW = nullptr;

void FilterMessage(LPMSG msg, bool removed) {
    if (!msg || msg->message != WM_MOUSEWHEEL || GetTickCount64() - g_lastMouseRead < 2000) return;
    if (removed && (!overlay::IsOpen() || hotkeys::Capturing())) hotkeys::AddWheel(GET_WHEEL_DELTA_WPARAM(msg->wParam));
    if (hotkeys::WheelBlocked() && !overlay::IsOpen()) msg->message = WM_NULL;
}

BOOL WINAPI PeekA(LPMSG m, HWND h, UINT lo, UINT hi, UINT remove) {
    const BOOL ok = g_peekA(m, h, lo, hi, remove);
    if (ok) FilterMessage(m, (remove & PM_REMOVE) != 0);
    return ok;
}
BOOL WINAPI PeekW(LPMSG m, HWND h, UINT lo, UINT hi, UINT remove) {
    const BOOL ok = g_peekW(m, h, lo, hi, remove);
    if (ok) FilterMessage(m, (remove & PM_REMOVE) != 0);
    return ok;
}
BOOL WINAPI GetA(LPMSG m, HWND h, UINT lo, UINT hi) {
    const BOOL ok = g_getA(m, h, lo, hi);
    if (ok > 0) FilterMessage(m, true);
    return ok;
}
BOOL WINAPI GetW(LPMSG m, HWND h, UINT lo, UINT hi) {
    const BOOL ok = g_getW(m, h, lo, hi);
    if (ok > 0) FilterMessage(m, true);
    return ok;
}

// Raw input: the wheel flag of a mouse packet is cleared when the wheel is reserved for the trainer.
using RawDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using RawBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
RawDataFn g_rawData = nullptr;
RawBufferFn g_rawBuffer = nullptr;

// Horizontal mouse counts (relative packets), for turning the player where the game ignores the mouse (movement.cpp).
std::atomic<long> g_rawMouseX{ 0 };

void FilterRaw(RAWINPUT* raw) {
    if (raw->header.dwType != RIM_TYPEMOUSE) return;
    RAWMOUSE& mouse = raw->data.mouse;
    // The menu's clicks are not the game's (user, 2026-11-08: a left click on an option made Chris fire; the game reads the
    // mouse here, not through DirectInput). The presses only: a button held when the menu opens must still be released.
    if (overlay::BlocksGameInput())
        mouse.usButtonFlags &= ~(RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN |
                                 RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN);
    if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && LookFrozen()) mouse.lLastX = mouse.lLastY = 0;
    if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && !overlay::IsOpen()) g_rawMouseX.fetch_add(mouse.lLastX);
    if (!(mouse.usButtonFlags & RI_MOUSE_WHEEL)) return;
    if (hotkeys::WheelBlocked() && !overlay::IsOpen()) {
        mouse.usButtonFlags &= ~RI_MOUSE_WHEEL;
        mouse.usButtonData = 0;
    }
}

UINT WINAPI RawData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header) {
    const UINT result = g_rawData(input, command, data, size, header);
    if (command == RID_INPUT && data && result != static_cast<UINT>(-1) && result >= sizeof(RAWINPUTHEADER))
        FilterRaw(static_cast<RAWINPUT*>(data));
    return result;
}

UINT WINAPI RawBuffer(PRAWINPUT data, PUINT size, UINT header) {
    const UINT count = g_rawBuffer(data, size, header);
    if (data && count != static_cast<UINT>(-1)) {
        RAWINPUT* raw = data;
        for (UINT i = 0; i < count; ++i) {
            FilterRaw(raw);
            // NEXTRAWINPUTBLOCK: blocks are 8-byte aligned (also for 32-bit processes on 64-bit Windows).
            raw = reinterpret_cast<RAWINPUT*>((reinterpret_cast<uintptr_t>(raw) + raw->header.dwSize + 7) & ~uintptr_t{ 7 });
        }
    }
    return count;
}

// XInput (the game's xinput1_3, XInputGetState = ordinal 2 and its undocumented twin ordinal 100): the right stick reads
// centred while the look is frozen.
using PadStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
PadStateFn g_padState = nullptr, g_padStateEx = nullptr;
std::atomic<bool> g_padSeen{ false };

// Free camera on a pad (movement.cpp): while Chris stands, the right stick's horizontal axis is hidden from the game (it
// turned him in place) and the trainer turns the camera with its real value.
std::atomic<bool> g_padRightXHidden{ false };
std::atomic<bool> g_padRightXHiddenAim{ false };  // "Manette : visée comme à la souris" (its own source)
std::atomic<int> g_padRightX{ 0 };
std::atomic<int> g_padRightY{ 0 };
std::atomic<int> g_padLeftX{ 0 }, g_padLeftY{ 0 };
std::atomic<bool> g_padLeftForward{ false };
std::atomic<bool> g_padLeftHiddenAim{ false };  // the pad's aim through the mouse's: the game's mouse way turns the aim with it

// Pad buttons kept from the game (user, 2026-10-23: d-pad melee moves with LB held also reached the game as its item
// keys, and Chris changed weapon after the move): XINPUT_GAMEPAD bits, 0x10000 LT, 0x20000 RT, per source (0 = the
// shortcuts with a key to hold; 1 unused: the d-pad while the knife was out, dropped, the user wants to switch weapons
// while aiming; 2 the minimap / partner location buttons, quick_knife.cpp). The trainer's own XInput still reads them.
std::atomic<uint32_t> g_padHidden[3]{};
// Source 2's buttons really held while the game did not see them (user, 2026-11-06: "Tir sans viser" no longer worked
// on the pad with RT kept from the game out of aim; the trainer's readers of the game's held mask get them back).
std::atomic<uint32_t> g_padHiddenHeld{ 0 };
// Pad buttons the trainer presses for the game (user, 2026-11-07: "Mini-carte (touche)", "Localiser le partenaire
// (touche)" - RB / RT kept from the game, their functions on other buttons): after the hidden ones, same bits.
std::atomic<uint32_t> g_padInjected{ 0 };

// A and X swapped for the game (user, 2026-10-23: "all actions on A", A the action button and X the run button, RE4
// style): applied after the hidden buttons (those are the physical ones of the trainer's shortcuts).
std::atomic<bool> g_padSwapAX{ false };

void FilterPad(DWORD result, XINPUT_STATE* state) {
    if (!g_padSeen.exchange(true)) LOG("game reads XInput (xinput1_3)");
    if (result != ERROR_SUCCESS || !state) return;
    g_padRightX = state->Gamepad.sThumbRX;
    g_padRightY = state->Gamepad.sThumbRY;
    g_padLeftX = state->Gamepad.sThumbLX;
    g_padLeftY = state->Gamepad.sThumbLY;
    if (g_padLeftForward.load(std::memory_order_relaxed) && !overlay::IsOpen()) {
        const float x = state->Gamepad.sThumbLX, y = state->Gamepad.sThumbLY;
        const float tilt = std::sqrt(x * x + y * y);
        state->Gamepad.sThumbLX = 0;
        state->Gamepad.sThumbLY = static_cast<SHORT>(tilt > 32767.0f ? 32767.0f : tilt);
    }
    if (g_padLeftHiddenAim.load(std::memory_order_relaxed) && !overlay::IsOpen())
        state->Gamepad.sThumbLX = state->Gamepad.sThumbLY = 0;
    if ((g_padRightXHidden.load(std::memory_order_relaxed) || g_padRightXHiddenAim.load(std::memory_order_relaxed)) &&
        !overlay::IsOpen())
        state->Gamepad.sThumbRX = 0;
    // The pad's aim through the mouse's: both axes are the trainer's then (movement.cpp feeds the mouse's virtual stick).
    if (g_padRightXHiddenAim.load(std::memory_order_relaxed) && !overlay::IsOpen()) state->Gamepad.sThumbRY = 0;
    const uint32_t hidden = g_padHidden[0].load(std::memory_order_relaxed) | g_padHidden[1].load(std::memory_order_relaxed) |
                            g_padHidden[2].load(std::memory_order_relaxed);
    {
        const uint32_t real = state->Gamepad.wButtons | (state->Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 0x10000u : 0u) |
                              (state->Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 0x20000u : 0u);
        g_padHiddenHeld = overlay::IsOpen() ? 0u : real & g_padHidden[2].load(std::memory_order_relaxed);
    }
    if (hidden && !overlay::IsOpen()) {
        state->Gamepad.wButtons &= static_cast<WORD>(~hidden);
        if (hidden & 0x10000) state->Gamepad.bLeftTrigger = 0;
        if (hidden & 0x20000) state->Gamepad.bRightTrigger = 0;
    }
    if (const uint32_t injected = g_padInjected.load(std::memory_order_relaxed); injected && !overlay::IsOpen()) {
        state->Gamepad.wButtons |= static_cast<WORD>(injected);
        if (injected & 0x10000) state->Gamepad.bLeftTrigger = 255;
        if (injected & 0x20000) state->Gamepad.bRightTrigger = 255;
    }
    if (g_padSwapAX.load(std::memory_order_relaxed)) {
        const WORD b = state->Gamepad.wButtons;
        const WORD a = b & XINPUT_GAMEPAD_A, x = b & XINPUT_GAMEPAD_X;
        state->Gamepad.wButtons = static_cast<WORD>((b & ~(XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X)) | (a ? XINPUT_GAMEPAD_X : 0) |
                                                    (x ? XINPUT_GAMEPAD_A : 0));
    }
    if (LookFrozen()) state->Gamepad.sThumbRX = state->Gamepad.sThumbRY = 0;
}
DWORD WINAPI PadState(DWORD user, XINPUT_STATE* state) {
    const DWORD result = g_padState(user, state);
    FilterPad(result, state);
    return result;
}
DWORD WINAPI PadStateEx(DWORD user, XINPUT_STATE* state) {
    const DWORD result = g_padStateEx(user, state);
    FilterPad(result, state);
    return result;
}

bool HookPad() {
    HMODULE xinput = GetModuleHandleW(L"xinput1_3.dll");
    if (!xinput) return false;
    return Hook(reinterpret_cast<void*>(GetProcAddress(xinput, MAKEINTRESOURCEA(2))), &PadState, &g_padState, nullptr) &&
           Hook(reinterpret_cast<void*>(GetProcAddress(xinput, MAKEINTRESOURCEA(100))), &PadStateEx, &g_padStateEx, nullptr);
}

bool HookMessages() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;
    const bool raw = Hook(reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputData")), &RawData, &g_rawData, nullptr) &&
                     Hook(reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputBuffer")), &RawBuffer, &g_rawBuffer, nullptr);
    LOG("raw input hooks %s", raw ? "installed" : "FAILED");
    return Hook(reinterpret_cast<void*>(GetProcAddress(user32, "PeekMessageA")), &PeekA, &g_peekA, nullptr) &&
           Hook(reinterpret_cast<void*>(GetProcAddress(user32, "PeekMessageW")), &PeekW, &g_peekW, nullptr) &&
           Hook(reinterpret_cast<void*>(GetProcAddress(user32, "GetMessageA")), &GetA, &g_getA, nullptr) &&
           Hook(reinterpret_cast<void*>(GetProcAddress(user32, "GetMessageW")), &GetW, &g_getW, nullptr);
}
}

bool InstallDInput() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    void* vtA[11]{};
    void* vtW[11]{};
    IDirectInput8A* diA = nullptr;
    IDirectInput8W* diW = nullptr;
    if (SUCCEEDED(DirectInput8Create(inst, DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(&diA), nullptr))) {
        IDirectInputDevice8A* dev = nullptr;
        if (SUCCEEDED(diA->CreateDevice(GUID_SysKeyboard, &dev, nullptr))) {
            memcpy(vtA, *reinterpret_cast<void***>(dev), sizeof(vtA));
            dev->Release();
        }
        diA->Release();
    }
    if (SUCCEEDED(DirectInput8Create(inst, DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&diW), nullptr))) {
        IDirectInputDevice8W* dev = nullptr;
        if (SUCCEEDED(diW->CreateDevice(GUID_SysKeyboard, &dev, nullptr))) {
            memcpy(vtW, *reinterpret_cast<void***>(dev), sizeof(vtW));
            dev->Release();
        }
        diW->Release();
    }
    // IDirectInputDevice8: 9 = GetDeviceState, 10 = GetDeviceData
    bool ok = Hook(vtA[9], &StateA, &g_stateA, nullptr) && Hook(vtA[10], &DataA, &g_dataA, nullptr) &&
              Hook(vtW[9], &StateW, &g_stateW, vtA[9]) && Hook(vtW[10], &DataW, &g_dataW, vtA[10]);
    LOG("dinput hooks %s (A %p/%p, W %p/%p)", ok ? "installed" : "FAILED", vtA[9], vtA[10], vtW[9], vtW[10]);
    const bool messages = HookMessages();
    LOG("message loop hooks %s", messages ? "installed" : "FAILED");
    LOG("xinput hooks %s", HookPad() ? "installed" : "FAILED");
    return ok;
}

long TakeRawMouseX() { return g_rawMouseX.exchange(0); }
void HidePadRightX(bool on) { g_padRightXHidden = on; }
void HidePadRightXForAim(bool on) { g_padRightXHiddenAim = on; }
void HidePadLeftForAim(bool on) { g_padLeftHiddenAim = on; }
void SwapPadAX(bool on) { g_padSwapAX = on; }
void HidePadButtons(int source, uint32_t mask) {
    if (source >= 0 && source < 3) g_padHidden[source] = mask;
}
uint32_t PadHiddenButtonsHeld() { return g_padHiddenHeld.load(std::memory_order_relaxed); }
void InjectPadButtons(uint32_t mask) { g_padInjected = mask; }
uint32_t PadInjectedButtons() { return g_padInjected.load(std::memory_order_relaxed); }
float PadRightY() {  // its real value, -1 (down) .. 1 (up), dead zone removed
    const float v = static_cast<float>(g_padRightY.load(std::memory_order_relaxed));
    constexpr float kDeadZone = 8689.0f;  // XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE
    if (std::fabs(v) < kDeadZone) return 0.0f;
    return (v - std::copysign(kDeadZone, v)) / (32767.0f - kDeadZone);
}
float PadRightX() {
    const float v = static_cast<float>(g_padRightX.load(std::memory_order_relaxed));
    constexpr float kDeadZone = 8689.0f;  // XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE
    if (std::fabs(v) < kDeadZone) return 0.0f;
    return (v - std::copysign(kDeadZone, v)) / (32767.0f - kDeadZone);
}
void FreezeLook(bool on) { g_lookFrozen = on; }
void PadLeftForward(bool on) { g_padLeftForward = on; }
void PadLeftStick(float& x, float& y) {
    x = static_cast<float>(g_padLeftX.load(std::memory_order_relaxed)) / 32767.0f;
    y = static_cast<float>(g_padLeftY.load(std::memory_order_relaxed)) / 32767.0f;
}
}
