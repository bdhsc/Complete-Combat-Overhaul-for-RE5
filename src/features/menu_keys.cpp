// Menus with the movement keys (user, 2026-10-21: in the game's menus the keyboard navigates with the arrows, far from
// the hand, or the wheel, reserved for the trainer). The menus do not go through the player's sBioPad queries (nothing
// in diag.pad_log while navigating): they read sKeyboard [RVA 0xE49C74] (DTI class sKeyboard, vtable RVA 0xCB8CF8)
// directly. Its update VA 0x665030 (vtable slot 6) moves the held bits +0x120 (8 dwords, one bit per VK) to +0x140,
// clears them, calls the key read VA 0x664DD0 (eax = sKeyboard, esi = the held array: DirectInput GetDeviceState
// through the DIK -> VK table +0x20, filled for the keyboard layout at start; GetKeyboardState without a device) and then
// derives +0x160 pressed, +0x180 released, +0x1A0 changed and +0x1C0 auto-repeat (timers +0x1E0) from them. The key
// read is hooked: right after it, with no player loaded (main menu, chapter select, options), or in a game while the
// pause menu is shown or player 1's inventory is open (user, 2026-10-21; pickup.cpp watches both), the keys bound to the
// direction buttons 0x00-0x03 (forward, back, left, right) also hold the arrows (VK_UP / DOWN / LEFT / RIGHT, and the
// keys bound to buttons 0x22-0x25, the arrows by default), and the key bound to the action button 0x0F (E for the
// user; 0x10 is the partner action) holds Enter, the menus' confirm key, so the game makes the presses and the repeat
// itself.
// Key bindings (read live, 2026-10-21): config [RVA 0xDB2158], set [config+0x20], button b's key code at
// config + set * 0x8C74 + 0xA8 + 4b (VA 0x7B6E10); codes 1..0x61 are keyboard keys, VK = dword [RVA 0xCE5EF0 + 4 code]
// (VA 0x79A240; 0x62.. the mouse buttons).
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstring>

namespace features {
bool PauseMenuIsShown();      // pickup.cpp
bool Player1InventoryOpen();  // pickup.cpp
bool EnsurePauseMenuWatch();  // pickup.cpp

namespace {
std::atomic<bool> g_on{ false };
void* g_read = nullptr;  // trampoline of the key read VA 0x664DD0

int KeyVk(int button) {
    uintptr_t config = 0;
    uint32_t set = 0, code = 0, vk = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), config) || !config || !mem::Read(config + 0x20, set) ||
        set > 3 || !mem::Read(config + set * 0x8C74 + 0xA8 + 4 * button, code) || code < 1 || code > 0x61 ||
        !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xCE5EF0)) + 4 * code, vk) || vk > 0xFF)
        return -1;
    return static_cast<int>(vk);
}

struct MenuKey {
    int button;       // the game button whose key navigates
    int vk;           // the menu key it holds
    int menuButton;   // the game button whose key it also holds (-1 none)
};
constexpr MenuKey kMenuKeys[] = {
    { 0x00, VK_UP, 0x22 }, { 0x01, VK_DOWN, 0x23 }, { 0x02, VK_LEFT, 0x24 }, { 0x03, VK_RIGHT, 0x25 },
    { 0x0F, VK_RETURN, -1 },
};

void Hold(uint32_t* held, int vk) { held[vk >> 5] |= 1u << (vk & 31); }

void __cdecl AfterKeyRead(uint32_t* held) {
    if (!g_on.load(std::memory_order_relaxed) || !held ||
        (actors::InGame() && !PauseMenuIsShown() && !Player1InventoryOpen()))
        return;
    __try {
        for (const MenuKey& key : kMenuKeys) {
            const int from = KeyVk(key.button);
            if (from < 0 || !(held[from >> 5] & (1u << (from & 31)))) continue;
            Hold(held, key.vk);
            if (const int other = key.menuButton >= 0 ? KeyVk(key.menuButton) : -1; other >= 0) Hold(held, other);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(naked) void KeyReadStub() {
    __asm {
        push eax                 // sKeyboard: the original takes it in eax, the held array in esi
        call dword ptr [g_read]
        pop eax
        pushad
        push esi
        call AfterKeyRead
        add esp, 4
        popad
        ret
    }
}

bool EnsureKeyReadHook() {
    static bool hooked = false;
    if (hooked) return true;
    void* target = mem::Rva(0x264DD0);
    const uint8_t expected[] = { 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x53, 0x57, 0x8B, 0xF8 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("menu keys: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&KeyReadStub), &g_read);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("menu keys: key read hook installed at %p (forward %X, back %X, left %X, right %X, action %X)", target, KeyVk(0),
        KeyVk(1), KeyVk(2), KeyVk(3), KeyVk(0x0F));
    return true;
}
}  // namespace

void RegisterMenuKeys() {
    Add({ .id = "game.menu_move_keys", .name = "Menus avec les touches de déplacement",
          .help = "Hors partie (menu principal, choix du chapitre, options...), dans le menu pause et dans "
                  "l'inventaire, les touches de déplacement réglées dans le jeu (avant, arrière, gauche, droite) naviguent "
                  "comme les flèches et la touche d'action valide comme Entrée.",
          .category = "Jeu", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureKeyReadHook() || !EnsurePauseMenuWatch())) return false;
              g_on = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
}
}  // namespace features
