// Munitions.
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstring>

namespace features {
namespace {

// --- Reload without reserve -------------------------------------------------------------
// Same three code changes as the Codex RE5SoloTrainer (user-confirmed working on 1.0.0.129):
//  1. reload availability gate: `call reserveCount; test eax,eax; jg` -> `jge` (reload allowed with 0 reserve)
//  2. reload transfer: `cmp eax,ebx; jge +0x11; ...` -> jump straight to the magazine update at +0x25
//     (EBX = missing rounds; skips the clamp to the reserve and the removal from inventory).
//     Codex jumped through an allocated thunk to the same target; a direct jmp is equivalent.
//  3. reserve decrement `sub [esi+8], ebp` -> nops.
mem::Patch g_gate("reload.gate", 0x84DEA2,
                  { 0xE8, 0xE9, 0xFE, 0xFF, 0xFF, 0x85, 0xC0, 0x7F, 0x0C },
                  { 0xE8, 0xE9, 0xFE, 0xFF, 0xFF, 0x85, 0xC0, 0x7D, 0x0C });
mem::Patch g_transfer("reload.transfer", 0x872996,
                      { 0x3B, 0xC3, 0x7D, 0x11, 0x8B, 0x4F, 0x18 },
                      { 0xE9, 0x20, 0x00, 0x00, 0x00, 0x90, 0x18 });
mem::Patch* g_decrement = nullptr;

bool ResolveDecrement() {
    if (g_decrement) return true;
    constexpr uintptr_t kKnownRva = 0x84DCCB;
    uint8_t* site = mem::FindUnique("29 6E 08");
    if (!site) {
        uint8_t expected[3] = { 0x29, 0x6E, 0x08 };
        if (mem::IsGameCode(mem::Rva(kKnownRva), 3) && memcmp(mem::Rva(kKnownRva), expected, 3) == 0) site = mem::Rva(kKnownRva);
    }
    if (!site) {
        LOG("reload: reserve decrement instruction not found");
        return false;
    }
    LOG("reload: reserve decrement at RVA 0x%X", static_cast<unsigned>(site - mem::Base()));
    g_decrement = new mem::Patch("reload.decrement", site, { 0x29, 0x6E, 0x08 }, { 0x90, 0x90, 0x90 });
    return true;
}

bool SetReloadNoReserve(bool on) {
    if (!ResolveDecrement()) return false;
    return on ? mem::ApplyAll({ &g_gate, &g_transfer, g_decrement })
              : mem::RestoreAll({ g_decrement, &g_transfer, &g_gate });
}

// The game's own "infinite ammo" choice on its start screen is ON every time the game is launched, once unlocked
// (user). Found with tools/memscan.py (toggled ON/OFF 4 times on that screen): byte +0x198 of the start-screen unit
// uIdStartUp04 (vtable RVA 0xBA8A7C; +0x18C is its array of option values, +0x198 = option 3). When the screen
// appears, the trainer sets it to OFF once; the player can still switch it back on.
// The screen used to be found through sParameter [RVA 0xDB27B4] + 0x10E8: that pointer stays null when the screen
// is reached through "Continuer" (first start of the game: the choice stayed ON, user 2026-10-14). The screen's
// per-frame function (RVA 0x637BD0, thiscall, no argument; reads +0x18C.. every frame) is hooked instead: it
// hands over the screen itself, whatever menu led there.
std::atomic<bool> g_ammoMenuOff{ false };
uintptr_t g_ammoMenuDone = 0;   // start-screen instance already handled
uintptr_t g_ammoMenuSeen = 0;   // start-screen instance being watched
ULONGLONG g_ammoMenuSince = 0;  // when it appeared

using ScreenFrameFn = void(__fastcall*)(void* screen, void* edx);
ScreenFrameFn g_screenFrame = nullptr;
bool g_screenHooked = false;

// The value may still be unset on the first frames the screen exists: for 5 s the trainer waits for the game to put
// it ON, sets it OFF once, then leaves the player free to change it.
void StartScreenAmmoOff(uintptr_t screen) {
    if (!g_ammoMenuOff.load(std::memory_order_relaxed) || screen == g_ammoMenuDone) return;
    const ULONGLONG now = GetTickCount64();
    uint8_t value = 0;
    if (screen != g_ammoMenuSeen) {
        g_ammoMenuSeen = screen;
        g_ammoMenuSince = now;
        mem::Read(screen + 0x198, value);
        LOG("start screen %08X seen (game infinite ammo = %u)", static_cast<unsigned>(screen), value);
    }
    if (mem::Read(screen + 0x198, value) && value == 1) {
        mem::Write<uint8_t>(screen + 0x198, 0);
        LOG("start screen: game infinite ammo set to OFF");
        g_ammoMenuDone = screen;
    } else if (now - g_ammoMenuSince > 5000) {
        g_ammoMenuDone = screen;
    }
}

void __fastcall HookScreenFrame(void* screen, void* edx) {
    StartScreenAmmoOff(reinterpret_cast<uintptr_t>(screen));
    g_screenFrame(screen, edx);
}

bool EnsureScreenHook() {
    if (g_screenHooked) return true;
    uint8_t* target = mem::Rva(0x637BD0);
    const uint8_t expected[] = { 0x83, 0xEC, 0x34, 0x56, 0x57, 0x33, 0xFF, 0x57, 0x68, 0xFF };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("start screen hook: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookScreenFrame), reinterpret_cast<void**>(&g_screenFrame));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    g_screenHooked = true;
    LOG("start screen hook installed at %p", target);
    return true;
}
}

void RegisterAmmo() {
    Add({
        .id = "ammo.reload_no_reserve",
        .name = "Recharger sans réserve",
        .help = "Recharge le chargeur complet même sans munitions en réserve ; la réserve n'est jamais consommée.",
        .category = "Munitions",
        .kind = Kind::Toggle,
        .set = SetReloadNoReserve,
        .available = [] { return game::IsSupported(); },
    });
    Add({ .id = "ammo.start_screen_off", .name = "Munitions illimitées du jeu sur OFF au démarrage",
          .help = "Sur l'écran de démarrage de la partie, le choix « munitions illimitées » du jeu est mis sur OFF "
                  "(le jeu le propose sur ON à chaque lancement). Tu peux toujours le remettre sur ON à la main ; les "
                  "munitions infinies du trainer restent disponibles par raccourci.",
          .category = "Munitions", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureScreenHook()) return false;
              g_ammoMenuDone = g_ammoMenuSeen = 0;
              g_ammoMenuOff = on;
              return true;
          },
          .available = [] { return game::IsSupported(); },
          .defaultOn = true });
}
}
