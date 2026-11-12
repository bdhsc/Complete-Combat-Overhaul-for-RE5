// Dying state (user, 2026-10-15). When a hit would kill, the player falls with 1 HP (P+0x1364 == 1) and P+0x1BAC
// (seconds left, float) counts down by one per second (player update RVA 0x77CDD0); at 0 the player dies.
// - Its starting value comes from RVA 0x765170 (thiscall, float): 30 s Amateur, 15 s Normal / Veteran, 3 s Pro (game
//   parameters [RVA 0xDB27B4] +0x3B8 / +0x3BC / +0x3C0), 15 s in Mercenaries without a partner. The HUD bar is the time
//   left over that value: it starts full on every difficulty.
// - The dying entry (RVA 0x779CE0) sets it; in Mercenaries solo it starts lower instead: 13 s, then 6 s, then 2 s
//   (+0x3F4 / +0x3F8 / +0x3FC) when the player falls again without healing (count P+0x1BC8, reset above 3 HP).
// - Mercenaries solo: the player's cActionButton (P+0x219C) shows the "mash" prompt; its activation callback RVA
//   0x76BF40 returns "HP == 1" except in story (mode 0, 3, 4) or with a partner. Each press of its button (+0xB set for
//   one frame) adds +0x400 (13) / 30 = 0.43 s in the player update, behind the game-mode test RVA 0x315B20 (called
//   from RVA 0x77CF60); above the starting value of RVA 0x765170 the player gets up with 3 HP.
// "Se relever seul (mourant)" lifts these two mode tests for player 1 in story, starts the bar like Mercenaries solo
// (13/15 of it, then 6/15, then 2/15) and scales each press by its slider. "Durée de l'état mourant" replaces the
// difficulty's value of RVA 0x765170 (story only: Mercenaries starts at 13 s whatever the maximum).
#include "features/dying.hpp"

#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <imgui.h>

#include <atomic>
#include <cstring>

namespace features {
bool EnsurePlayerDamageHook();  // player.cpp
bool HurtPlayer1Raw(int16_t amount);  // player.cpp: his own SubHealth, past the trainer's refusals

namespace {

std::atomic<bool> g_timeOn{ false };
std::atomic<float> g_timeValue{ 15.0f };
std::atomic<bool> g_mashOn{ false };
std::atomic<float> g_mashFactor{ 1.0f };
std::atomic<bool> g_struggleOn{ false };
std::atomic<bool> g_rescueOn{ false };
std::atomic<bool> g_rescue{ false };
std::atomic<bool> g_re6On{ false };
std::atomic<float> g_re6Seconds{ 15.0f };
std::atomic<float> g_re6Progress{ 0.0f };  // recovery bar 0..1 (game thread)
std::atomic<bool> g_re6Shown{ false };     // player 1 dying in this mode (render thread, for the HUD)
std::atomic<bool> g_dyingSwap{ false };     // "Mourant : changer d'arme" (off at first: a toggle saved off is not re-applied)
std::atomic<bool> g_normalSpeed{ false };   // "Vitesse normale en étant mourant"
std::atomic<bool> g_dyingActions{ false };  // "Actions en étant mourant"

bool InMercenaries() {
    uint32_t mode = 0;
    const uintptr_t at = mem::Chain(reinterpret_cast<uintptr_t>(mem::Rva(0xE340A4)), { 0, 0x1042C, 0x58 });
    return at && mem::Read(at, mode) && (mode == 1 || mode == 2 || mode == 5);  // as RVA 0x315B20
}

float GameParam(uintptr_t offset) {
    float value = 0;
    const uintptr_t at = mem::Chain(reinterpret_cast<uintptr_t>(mem::Rva(0xDB27B4)), { 0, offset });
    return at && mem::Read(at, value) ? value : 0.0f;
}

// Starting value of the dying countdown.
using DyingTimeFn = float(__fastcall*)(void* player, void* edx);
DyingTimeFn g_dyingTime = nullptr;

float __fastcall HookDyingTime(void* player, void* edx) {
    const float game = g_dyingTime(player, edx);
    if (!g_timeOn.load(std::memory_order_relaxed) || InMercenaries()) return game;
    const uintptr_t p = reinterpret_cast<uintptr_t>(player);
    if (p != actors::Player(0) && p != actors::Player(1)) return game;
    // 0 = immediate death: a hundredth of a second rather than 0, the HUD divides by this value.
    const float value = g_timeValue.load(std::memory_order_relaxed);
    return value < 0.01f ? 0.01f : value;
}

// Dying entry: start player 1 below the top like Mercenaries solo, otherwise the first press would get him up.
using DyingEntryFn = int(__fastcall*)(void* player, void* edx, int a, int b);
DyingEntryFn g_dyingEntry = nullptr;

int __fastcall HookDyingEntry(void* player, void* edx, int a, int b) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(player);
    const bool mine = g_mashOn.load(std::memory_order_relaxed) && p && p == actors::Player(0);
    uint8_t countBefore = 0;
    if (mine) mem::Read(p + 0x1BC8, countBefore);
    const int result = g_dyingEntry(player, edx, a, b);
    uint8_t countAfter = countBefore;
    int16_t health = 0;
    float left = 0;
    if (!mine || InMercenaries() || !mem::Read(p + 0x1BC8, countAfter) || countAfter == countBefore) return result;
    if (!mem::Read(p + 0x1364, health) || health != 1 || !mem::Read(p + 0x1BAC, left) || left <= 0) return result;
    const float top = GameParam(0x3BC);
    float start = GameParam(0x3F4 + 4 * (countBefore < 2 ? countBefore : 2));
    float fraction = top > 0 && start > 0 ? start / top : (countBefore == 0 ? 13.0f : countBefore == 1 ? 6.0f : 2.0f) / 15.0f;
    if (fraction > 0.95f) fraction = 0.95f;
    mem::Write(p + 0x1BAC, left * fraction);
    LOG("dying: player 1 falls (time %d), %.2f s of %.2f", countBefore + 1, left * fraction, left);
    return result;
}

// The game clears the fall count only in the dying entry, when a hit leaves more than 3 HP: a heal followed by a hit
// that takes player 1 straight from full health to dying kept it (2026-10-15: third fall at 2/15 after a herb). Healed
// above 3 HP (herb, partner revive) = count cleared at once; getting up alone (3 HP) keeps it, as in Mercenaries.
void ClearFallCountWhenHealed() {
    if (!actors::InGame() || InMercenaries()) return;
    const uintptr_t p1 = actors::Player(0);
    int16_t health = 0;
    uint8_t count = 0;
    if (p1 && mem::Read(p1 + 0x1364, health) && health > 3 && mem::Read(p1 + 0x1BC8, count) && count != 0) {
        mem::Write(p1 + 0x1BC8, static_cast<uint8_t>(0));
        LOG("dying: player 1 healed (%d HP), fall count %d cleared", health, count);
    }
}

// Activation of the player's "mash" prompt (cActionButton callback, cdecl(player, player index, context)).
using ButtonActiveFn = bool(__cdecl*)(void* player, int index, void* context);
ButtonActiveFn g_buttonActive = nullptr;

bool __cdecl HookDyingButton(void* player, int index, void* context) {
    if (actors::SlotIs(index, 0) && g_mashOn.load(std::memory_order_relaxed) && !InMercenaries()) {
        const uintptr_t p1 = actors::Player(0);
        int16_t health = 0;
        float left = 0;
        if (p1 && mem::Read(p1 + 0x1364, health) && mem::Read(p1 + 0x1BAC, left)) return health == 1 && left > 0;
    }
    return g_buttonActive(player, index, context);
}

// Game-mode test RVA 0x315B20 (thiscall on the mode object, no argument). Only its call from the player update
// (return address RVA 0x77CF65, player in esi) is answered "Mercenaries" for player 1. On a press the update adds its
// gain to the time it read before this test (its [esp+0x10] = our [esp+0x14] on entry), not to P+0x1BAC: the scaled
// part is added to that copy, otherwise the game's write erases it (2026-10-15, measured: x5 gave 0.43 s per press).
void* g_modeCheck = nullptr;
uintptr_t g_mashReturn = 0;

bool PadTriggered(int button, int pad) {
    using TriggeredFn = bool(__fastcall*)(void* bioPad, void* edx, int button, int pad);
    uintptr_t bioPad = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C40)), bioPad) || !bioPad) return false;
    __try {
        return reinterpret_cast<TriggeredFn>(mem::Rva(0x39A3E0))(reinterpret_cast<void*>(bioPad), nullptr, button, pad);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// "Mourant façon RE6" (user, 2026-10-15). While player 1 is dying the game's time is
// frozen (countdown paused, see HookDrainGate), any hit kills (the player SubHealth hook empties the time, the update
// then kills as when the time runs out), and a recovery bar drawn by the trainer fills in the slider's seconds; full,
// the time is set above the maximum and this test lets the game's own "got up" code run (3 HP). With "Se relever
// seul", each press of the prompt's button adds 0.43 s x its multiplier to the bar instead of the game's time. Weapons
// stay usable (the 1 HP weapon checks RVA 0x452C62 / 0x76A89B / 0x76A8FB taken out, three of the five patches of the
// Ultimate Trainer's "Not So Dying"); the dying walk and the absence of action prompts (can-act test RVA 0x76A820) are
// kept.
// Answers of DyingMashAllowed to the mode test: 0 = the game's own, 1 = "Mercenaries" (the block runs), 2 = "not
// Mercenaries" (skipped, needed in Mercenaries where the game's answer runs it).
int Re6Update(uintptr_t player, float* saved) {
    float left = 0;
    if (!mem::Read(player + 0x1BAC, left) || *saved != left) return 2;
    // The bar fills only while he stands free (user, 2026-10-30): not while thrown down by the blow that made him fall
    // dying and getting up (the damage actions 0x85 and up), nor while held (0x80000000, the grab waits 0x82..0x84), until
    // he is free again; the trainer's moves (carrier 0x81) count as standing.
    uint32_t action = 0;
    if (!mem::Read(player + 0x10E0, action) || (action >= 0x80 && action != 0x81)) return 2;
    uintptr_t clock = 0;
    float dt = 0, scale = 1.0f;
    mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB209C)), clock);
    if (clock) mem::Read(clock + 0x24, dt);
    mem::Read(player + 0xFD4, scale);
    if (dt < 0 || dt > 0.25f) dt = 0;
    const float seconds = g_re6Seconds.load(std::memory_order_relaxed) > 0.5f ? g_re6Seconds.load(std::memory_order_relaxed) : 0.5f;
    float progress = g_re6Progress.load(std::memory_order_relaxed) + dt * scale / seconds;
    if (g_mashOn.load(std::memory_order_relaxed) || InMercenaries()) {  // Mercenaries: its own prompt, always
        uintptr_t button = 0;
        uint8_t accepted = 0, pad = 0;
        bool pressed = mem::Read(player + 0x219C, button) && button && mem::Read(button + 0xB, accepted) && accepted == 1;
        if (!pressed && mem::Read(player + 0x2D9C, pad)) pressed = PadTriggered(0x10, pad);
        if (pressed) {
            float perSecond = 0;
            mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xC18560)), perSecond);  // 1/30
            const float gain = GameParam(0x400) * perSecond;
            if (gain > 0 && gain < 5) progress += gain * g_mashFactor.load(std::memory_order_relaxed) / seconds;
        }
    }
    if (progress < 1.0f) {
        g_re6Progress = progress;
        return 2;  // nothing of the Mercenaries block: the game's time stays as it is
    }
    g_re6Progress = 1.0f;
    using DyingTimeFn = float(__fastcall*)(void* player, void* edx);
    const float top = reinterpret_cast<DyingTimeFn>(mem::Rva(0x765170))(reinterpret_cast<void*>(player), nullptr);
    *saved = top + 1.0f;
    mem::Write(player + 0x1BAC, *saved);
    LOG("dying (RE6): recovery bar full, player 1 gets up");
    return 1;
}

int __cdecl DyingMashAllowed(uintptr_t player, float* saved) {
    if (!player || player != actors::Player(0)) return 0;
    if (g_re6On.load(std::memory_order_relaxed)) return Re6Update(player, saved);  // story and Mercenaries
    if (InMercenaries() || !g_mashOn.load(std::memory_order_relaxed)) return 0;
    float left = 0;
    if (!mem::Read(player + 0x1BAC, left) || *saved != left) return 1;
    // A press the prompt accepted this frame (+0xB): the game adds its gain right after this test, to *saved.
    uintptr_t button = 0;
    uint8_t accepted = 0;
    const bool native = mem::Read(player + 0x219C, button) && button && mem::Read(button + 0xB, accepted) && accepted == 1;
    // The prompt (type 5, RVA 0x363B60) ignores the first press after a pause or after another prompt took its slot
    // (it only arms it): such a press of its button (game button 0x10 = partner, mask 0x2000) is added here. Not while
    // an enemy holds the player (its own struggle prompt is shown).
    bool missed = false;
    if (!native) {
        uint32_t action = 0;
        uint8_t pad = 0;
        missed = mem::Read(player + 0x10E0, action) && action != 0x80000000 && mem::Read(player + 0x2D9C, pad) &&
                 PadTriggered(0x10, pad);
    }
    if (!native && !missed) return 1;
    float perSecond = 0;
    mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xC18560)), perSecond);  // 1/30
    const float gain = GameParam(0x400) * perSecond;
    if (gain <= 0 || gain >= 5) return 1;
    const float factor = g_mashFactor.load(std::memory_order_relaxed);
    if (native) {
        *saved = left + gain * (factor - 1.0f);
    } else {
        // Without an accepted press the game neither adds nor writes its copy back: the time itself is raised (the
        // "got up" test that follows reads it).
        *saved = left + gain * factor;
        mem::Write(player + 0x1BAC, *saved);
    }
    return 1;
}

__declspec(naked) void HookModeCheck() {
    __asm {
        mov eax, [esp]
        cmp eax, g_mashReturn
        jne original
        lea eax, [esp + 0x14]
        push ecx
        push eax
        push esi
        call DyingMashAllowed
        add esp, 8
        pop ecx
        cmp eax, 1
        je mercs
        cmp eax, 2
        je story
        jmp dword ptr [g_modeCheck]
    mercs:
        mov eax, 1
        ret
    story:
        xor eax, eax
        ret
    original:
        jmp dword ptr [g_modeCheck]
    }
}

// Prompts while dying: the sActionButton request (RVA 0x364690, button in ebp) asks the player "can act" (RVA
// 0x76A820: false at 1 HP) and then only lets the dying prompts through (+0x1C 0x3C / 0x3F): the struggle prompt of a
// grab (licker tongue: type +0x18 6 "left/right", Majini: 4 "mash") vanishes the frame the player falls to 1 HP
// (2026-10-15, recorded). "Se débattre en étant mourant" answers "can act" for player 1's struggle prompts there.
void* g_canAct = nullptr;
uintptr_t g_requestReturn = 0;
uintptr_t g_itemReturn[3] = {};  // "can act" from the item code wrappers RVA 0x869840 / 0x8698B0 / 0x869920

// "Mourant façon RE6": the inventory and the weapon change ask "can act" through three small wrappers of the item
// code (RVA 0x869840, 0x8698B0, 0x869920, each also testing RVA 0x75AB30); only those get "yes" for player 1 while
// dying. The other callers (movement, prompts) keep the game's answer: dying walk, no action prompts. Not with
// "Mourant : changer d'arme" unticked (user, 2026-10-30: a weapon equipped from the inventory, its change refused, left
// Chris stuck): no inventory nor quick slots then, as in the game.
int __cdecl Re6ItemsAllowed(uintptr_t player) {
    return g_re6On.load(std::memory_order_relaxed) && g_dyingSwap.load(std::memory_order_relaxed) && player &&
                   player == actors::Player(0) && Player1Dying()
               ? 1
               : 0;
}

// "Vitesse normale en étant mourant" (user, 2026-10-30: the only useful part of the Ultimate Trainer's "Not So Dying",
// whose NOP of the 1 HP "no" in can-act, RVA 0x76A82D, let everyone act normally at 1 HP). The limp is not can-act's
// (answering "yes" to the walk / run input handlers alone changed nothing, to every caller but the prompts and items
// Chris could not move at all; logged while dying: can-act from RVA 0x36489A prompts, 0x78AFF6 input decoder, 0x8698FA
// inventory, 0x7840D8 walk / run): the locomotion motion is adjusted by RVA 0x77AD.. (its HP test RVA 0x77AEA9, from
// the motions seen: at low health bank 3 - the hurt set, 0x300 wait / 0x304 run - posture P+0x1E2D 1; at 1 HP index
// +0x28 - 0x328 wait / 0x329 walk / 0x32C run - posture 2), its "posture changed" test RVA 0x77AFB0 (HP test 0x77AFE6),
// and the walk / run updates take their dying speed factors (params +0x3C / +0x34 for the walk at RVA 0x87A4F2, +0x4C
// for the run at 0x87AB62). Those four jumps are taken as for a living player (both players, as the Ultimate Trainer's
// patch); the walk / run input handlers still get "yes" from can-act for player 1 (the run asked while dying).
mem::Patch g_normalMotion("dying.normal_motion", 0x77AEA9, { 0x74, 0x21 }, { 0x90, 0x90 });
mem::Patch g_normalPosture("dying.normal_posture", 0x77AFE6, { 0x74, 0x1D }, { 0x90, 0x90 });
mem::Patch g_normalWalkSpeed("dying.normal_walk_speed", 0x87A4F2, { 0x75, 0x16 }, { 0xEB, 0x16 });
mem::Patch g_normalRunSpeed("dying.normal_run_speed", 0x87AB62, { 0x75, 0x0B }, { 0xEB, 0x0B });
constexpr int kMoveCalls = 3;
uintptr_t g_moveReturn[kMoveCalls] = {};

int __cdecl NormalSpeedWhileDying(uintptr_t player) {
    return g_normalSpeed.load(std::memory_order_relaxed) && player && player == actors::Player(0) && Player1Dying() ? 1 : 0;
}

// "Actions en étant mourant" (user, 2026-10-30: "the jumps, the melee"): every prompt of player 1 while dying, not only
// the dying ones (the request's can-act, below; the trainer's moves through DyingActionsAllowed).
int __cdecl StruggleWhileDying(uintptr_t player, uintptr_t button) {
    if (g_dyingActions.load(std::memory_order_relaxed) && player && player == actors::Player(0) && Player1Dying()) return 1;
    if ((!g_struggleOn.load(std::memory_order_relaxed) && !g_rescue.load(std::memory_order_relaxed)) || !player ||
        player != actors::Player(0) || !button)
        return 0;
    uint32_t type = 0;
    return mem::Read(button + 0x18, type) && (type == 4 || type == 6) ? 1 : 0;
}

__declspec(naked) void HookCanAct() {
    __asm {
        mov eax, [esp]
        cmp eax, g_itemReturn[0]
        je items
        cmp eax, g_itemReturn[4]
        je items
        cmp eax, g_itemReturn[8]
        je items
        cmp eax, g_moveReturn[0]
        je move
        cmp eax, g_moveReturn[4]
        je move
        cmp eax, g_moveReturn[8]
        je move
        cmp eax, g_requestReturn
        jne original
        push ecx
        push ebp
        push ecx
        call StruggleWhileDying
        add esp, 8
        pop ecx
        test eax, eax
        jz original
        mov eax, 1
        ret
    items:
        push ecx
        push ecx
        call Re6ItemsAllowed
        add esp, 4
        pop ecx
        test eax, eax
        jz original
        mov eax, 1
        ret
    move:
        push ecx
        push ecx
        call NormalSpeedWhileDying
        add esp, 4
        pop ecx
        test eax, eax
        jz original
        mov eax, 1
        ret
    original:
        jmp dword ptr [g_canAct]
    }
}

// "Mourant : changer d'arme" (user, 2026-10-30: the RE6 mode allowed it, "impossible in RE6"): every weapon change of a
// player goes through RVA 0x7782E0 (cdecl: player, pad, two ints, a float; it takes the slot asked from the input block
// +0x430..+0x43C into P+0x2DD8 and asks the weapon change action 0x20; recorded on a quick slot change, the inventory's
// equip asks 0x20 too): unticked, player 1's are skipped while he is dying.
using WeaponChangeFn = void(__cdecl*)(uintptr_t player, int pad, int a, int b, float f);
WeaponChangeFn g_weaponChange = nullptr;

void __cdecl HookWeaponChange(uintptr_t player, int pad, int a, int b, float f) {
    if (!g_dyingSwap.load(std::memory_order_relaxed) && player && player == actors::Player(0) && Player1Dying()) {
        static DWORD logged = 0;
        if (GetTickCount() - logged > 2000) LOG("dying: player 1's weapon change refused");
        logged = GetTickCount();
        return;
    }
    g_weaponChange(player, pad, a, b, f);
}

// Saved by the grab (user, 2026-10-15): when a grab takes player 1 from health to dying (not when he was already
// dying), the struggle is won at once (struggle prompt let through while dying and filled by the struggle hooks,
// licker pounce released) and the dying time stays still until he is free (out of the hold and of the getting-up
// animations, action < 0x10): damage refused (hits while dying go through the player SubHealth to the dying entry,
// 0.06 s per point) and the countdown skipped through its "paused" test RVA 0x75C490, answered from its two calls in
// the player update (return RVA 0x77CEDD / 0x77CF14). Then the countdown resumes.
using DrainGateFn = bool(__fastcall*)(void* player, void* edx);
DrainGateFn g_drainGate = nullptr;
uintptr_t g_drainReturn[2] = {};

bool __fastcall HookDrainGate(void* player, void* edx) {
    const bool frozen = g_rescue.load(std::memory_order_relaxed) || g_re6On.load(std::memory_order_relaxed);
    if (frozen && player && reinterpret_cast<uintptr_t>(player) == actors::Player(0)) {
        const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (from == g_drainReturn[0] || from == g_drainReturn[1]) return true;
    }
    return g_drainGate(player, edx);
}

bool g_wasDying = false;  // render thread
DWORD g_rescueSince = 0, g_heldSince = 0, g_lastLickerTry = 0;

void TickRescue() {
    const uintptr_t p1 = actors::Player(0);
    uint32_t action = 0;
    if (!actors::InGame() || !p1 || !mem::Read(p1 + 0x10E0, action)) {
        g_rescue = false;
        g_wasDying = false;
        g_heldSince = 0;
        return;
    }
    const bool held = action == 0x80000000 || action == 0x84;
    const bool dying = Player1Dying();
    const DWORD now = GetTickCount();
    if (!held) g_heldSince = 0;
    else if (!g_heldSince) g_heldSince = now;
    if (!g_rescue.load() && held && dying && !g_wasDying) {
        g_rescue = true;
        g_rescueSince = now;
        LOG("grab rescue: player 1 fell dying while held (action %X)", action);
    }
    g_wasDying = dying;
    if (!g_rescue.load()) return;
    if (!dying || (!held && action < 0x10) || now - g_rescueSince > 20000) {
        g_rescue = false;
        LOG("grab rescue: over after %.1f s (action %X, dying %d)", (now - g_rescueSince) / 1000.0f, action, dying);
        return;
    }
    // The licker pounce has no game struggle: ask the licker to let go (it must be settled in its hold first).
    if (held && now - g_heldSince >= 600 && now - g_lastLickerTry >= 250) {
        g_lastLickerTry = now;
        FreePlayer1FromLicker();
    }
}

bool InstallHook(uintptr_t rva, const uint8_t* expected, size_t n, void* detour, void** original, const char* what) {
    uint8_t* target = mem::Rva(rva);
    if (!mem::IsGameCode(target, n) || std::memcmp(target, expected, n) != 0) {
        LOG("%s: unexpected bytes %s", what, mem::Hex(target, n).c_str());
        return false;
    }
    mem::EnsureExecutable(target, n);
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    LOG("%s hook installed at %p", what, target);
    return true;
}

bool EnsureWeaponChangeHook() {
    static bool hooked = false;
    if (!hooked) {
        const uint8_t prologue[] = { 0x8B, 0x0D, 0xDC, 0x27, 0x1B, 0x01, 0x56, 0x57 };  // mov ecx, [actors]; push esi; push edi
        hooked = InstallHook(0x7782E0, prologue, sizeof(prologue), reinterpret_cast<void*>(&HookWeaponChange),
                             reinterpret_cast<void**>(&g_weaponChange), "dying weapon change");
    }
    return hooked;
}

bool EnsureTimeHook() {
    static bool hooked = false;
    if (!hooked) {
        const uint8_t prologue[] = { 0x51, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x74, 0x2A, 0x00, 0x00 };
        hooked = InstallHook(0x765170, prologue, sizeof(prologue), reinterpret_cast<void*>(&HookDyingTime),
                             reinterpret_cast<void**>(&g_dyingTime), "dying time");
    }
    return hooked;
}

bool EnsureStruggleHook() {
    static bool hooked = false;
    if (!hooked) {
        const uint8_t call[] = { 0x8B, 0xCE, 0xE8 };  // mov ecx, esi; call RVA 0x76A820
        const uint8_t* site = mem::Rva(0x364893);
        int32_t rel = 0;
        if (std::memcmp(site, call, sizeof(call)) != 0 || !mem::Read(reinterpret_cast<uintptr_t>(site + 3), rel) ||
            reinterpret_cast<uintptr_t>(site + 7) + rel != reinterpret_cast<uintptr_t>(mem::Rva(0x76A820))) {
            LOG("dying struggle: unexpected call site %s", mem::Hex(site, 7).c_str());
            return false;
        }
        g_requestReturn = reinterpret_cast<uintptr_t>(site + 7);
        const uintptr_t itemCalls[3] = { 0x86988E, 0x8698F5, 0x869963 };  // mov ecx, esi; call RVA 0x76A820 (5 bytes)
        for (int i = 0; i < 3; ++i) {
            const uint8_t* call = mem::Rva(itemCalls[i]);
            int32_t to = 0;
            if (call[0] != 0xE8 || !mem::Read(reinterpret_cast<uintptr_t>(call + 1), to) ||
                reinterpret_cast<uintptr_t>(call + 5) + to != reinterpret_cast<uintptr_t>(mem::Rva(0x76A820))) {
                LOG("dying: unexpected item call site %s", mem::Hex(call, 5).c_str());
                return false;
            }
            g_itemReturn[i] = reinterpret_cast<uintptr_t>(call + 5);
        }
        const uintptr_t moveCalls[kMoveCalls] = { 0x7898BC, 0x789BDC, 0x7840D1 };  // mov ecx, esi; call RVA 0x76A820
        for (int i = 0; i < kMoveCalls; ++i) {
            const uint8_t* at = mem::Rva(moveCalls[i]);
            int32_t to = 0;
            if (std::memcmp(at, call, sizeof(call)) != 0 || !mem::Read(reinterpret_cast<uintptr_t>(at + 3), to) ||
                reinterpret_cast<uintptr_t>(at + 7) + to != reinterpret_cast<uintptr_t>(mem::Rva(0x76A820))) {
                LOG("dying: unexpected move call site %s", mem::Hex(at, 7).c_str());
                return false;
            }
            g_moveReturn[i] = reinterpret_cast<uintptr_t>(at + 7);
        }
        const uint8_t prologue[] = { 0x56, 0x8B, 0xF1, 0x66, 0x83, 0xBE, 0x64, 0x13, 0x00, 0x00, 0x01 };
        hooked = InstallHook(0x76A820, prologue, sizeof(prologue), reinterpret_cast<void*>(&HookCanAct), &g_canAct,
                             "dying struggle prompts");
    }
    return hooked;
}

bool EnsureDrainGateHook() {
    static bool hooked = false;
    if (!hooked) {
        for (int i = 0; i < 2; ++i) {
            const uintptr_t callRva = i == 0 ? 0x77CED8 : 0x77CF0F;
            const uint8_t* site = mem::Rva(callRva);
            int32_t rel = 0;
            if (site[0] != 0xE8 || !mem::Read(reinterpret_cast<uintptr_t>(site + 1), rel) ||
                reinterpret_cast<uintptr_t>(site + 5) + rel != reinterpret_cast<uintptr_t>(mem::Rva(0x75C490))) {
                LOG("grab rescue: unexpected call site %s", mem::Hex(site, 5).c_str());
                return false;
            }
            g_drainReturn[i] = reinterpret_cast<uintptr_t>(site + 5);
        }
        const uint8_t prologue[] = { 0x8B, 0x81, 0x64, 0x2D, 0x00, 0x00 };
        hooked = InstallHook(0x75C490, prologue, sizeof(prologue), reinterpret_cast<void*>(&HookDrainGate),
                             reinterpret_cast<void**>(&g_drainGate), "dying countdown pause");
    }
    return hooked;
}

bool EnsureRescueHooks() {
    return EnsureDrainGateHook() && EnsureStruggleHook() && EnsureGrabStruggleHooks() && EnsurePlayerDamageHook();
}

mem::Patch g_re6WeaponUnit("re6.weapon_unit", 0x452C62, { 0x75 }, { 0xEB });
mem::Patch g_re6CanWeapon("re6.can_weapon", 0x76A89B, { 0x75 }, { 0xEB });
mem::Patch g_re6CanKnife("re6.can_knife", 0x76A8FB, { 0x75 }, { 0xEB });

void TickRe6() {
    const bool dying = actors::InGame() && Player1Dying();
    if (!dying) g_re6Progress = 0.0f;
    g_re6Shown = dying;
}

void DrawRe6() {
    if (!g_re6On.load(std::memory_order_relaxed) || !g_re6Shown.load(std::memory_order_relaxed)) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const char* title = i18n::Tr("Tiens bon !");
    const char* hint = g_mashOn.load(std::memory_order_relaxed) ? i18n::Tr("Martèle la touche partenaire pour te relever plus vite") : "";
    const ImVec2 titleSize = ImGui::CalcTextSize(title);
    const ImVec2 hintSize = ImGui::CalcTextSize(hint);
    const float width = (hintSize.x > 300 ? hintSize.x : 300) + 40;
    const float height = titleSize.y + (hint[0] ? hintSize.y + 8 : 0) + 46;
    const ImVec2 box(screen.x * 0.5f - width * 0.5f, screen.y * 0.62f);
    dl->AddRectFilled(box, ImVec2(box.x + width, box.y + height), IM_COL32(15, 10, 10, 200), 6);
    dl->AddText(ImVec2(box.x + (width - titleSize.x) * 0.5f, box.y + 10), IM_COL32(255, 230, 200, 255), title);
    const ImVec2 bar(box.x + 20, box.y + titleSize.y + 20);
    const float barWidth = width - 40;
    const float progress = g_re6Progress.load(std::memory_order_relaxed);
    dl->AddRectFilled(bar, ImVec2(bar.x + barWidth, bar.y + 12), IM_COL32(60, 40, 35, 255), 3);
    dl->AddRectFilled(bar, ImVec2(bar.x + barWidth * (progress < 1 ? progress : 1), bar.y + 12), IM_COL32(200, 30, 30, 255), 3);
    if (hint[0]) dl->AddText(ImVec2(box.x + (width - hintSize.x) * 0.5f, bar.y + 20), IM_COL32(220, 210, 200, 255), hint);
}

bool EnsureMashHooks() {
    static bool hooked = false;
    if (!hooked) {
        // The call the mode-test detour answers: E8 rel32 to RVA 0x315B20, right after "mov ecx, [edi+0x1042C]".
        const uint8_t call[] = { 0x8B, 0x8F, 0x2C, 0x04, 0x01, 0x00, 0xE8 };
        const uint8_t* site = mem::Rva(0x77CF5A);
        int32_t rel = 0;
        if (std::memcmp(site, call, sizeof(call)) != 0 || !mem::Read(reinterpret_cast<uintptr_t>(site + 7), rel) ||
            reinterpret_cast<uintptr_t>(site + 11) + rel != reinterpret_cast<uintptr_t>(mem::Rva(0x315B20))) {
            LOG("dying mash: unexpected call site %s", mem::Hex(site, 11).c_str());
            return false;
        }
        g_mashReturn = reinterpret_cast<uintptr_t>(site + 11);
        const uint8_t entry[] = { 0x51, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x74, 0x2A, 0x00, 0x00 };
        const uint8_t active[] = { 0xA1 };
        const uint8_t mode[] = { 0x8B, 0x41, 0x58, 0x83, 0xF8, 0x02 };
        hooked = InstallHook(0x779CE0, entry, sizeof(entry), reinterpret_cast<void*>(&HookDyingEntry),
                             reinterpret_cast<void**>(&g_dyingEntry), "dying entry") &&
                 InstallHook(0x76BF40, active, sizeof(active), reinterpret_cast<void*>(&HookDyingButton),
                             reinterpret_cast<void**>(&g_buttonActive), "dying prompt") &&
                 InstallHook(0x315B20, mode, sizeof(mode), reinterpret_cast<void*>(&HookModeCheck), &g_modeCheck,
                             "dying mode test");
    }
    return hooked;
}
}  // namespace

bool Player1Dying() {
    const uintptr_t p1 = actors::Player(0);
    int16_t health = 0;
    float left = 0;
    return p1 && mem::Read(p1 + 0x1364, health) && health == 1 && mem::Read(p1 + 0x1BAC, left) && left > 0;
}

bool DyingStruggleAllowed() { return g_struggleOn.load(std::memory_order_relaxed); }

// swaps.cpp / wesker_moves.cpp: the trainer's moves and dodges for player 1 at 1 HP ("Actions en étant mourant").
bool DyingActionsAllowed() { return g_dyingActions.load(std::memory_order_relaxed) && Player1Dying(); }

bool GrabRescueActive() { return g_rescue.load(std::memory_order_relaxed); }

bool Re6DyingHitKills() {
    return (g_re6On.load(std::memory_order_relaxed) || FatalGrabHolds()) && !g_rescue.load(std::memory_order_relaxed) &&
           Player1Dying();
}

namespace {
void AskWalk(uintptr_t p) {
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(p, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return;
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(p), nullptr, 0x02, 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A run going on when player 1 falls dying (user, 2026-10-30: "Me mettre mourant" while sprinting, Chris stood frozen
// until the run key was let go): the game never lets a dying player start a run, and the run's dying motion (0x32C, see
// "Vitesse normale en étant mourant") stands at its frame 1. Without that option the run becomes his dying walk 0x02.
void TickDyingRun() {
    static DWORD asked = 0;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || g_normalSpeed.load(std::memory_order_relaxed) || !mem::Read(p + 0x10E0, action) || action != 0x05 ||
        GetTickCount() - asked < 250 || !Player1Dying())
        return;
    asked = GetTickCount();
    LOG("dying: player 1's run turned into the dying walk");
    AskWalk(p);
}
}  // namespace

void RegisterDying() {
    Feature& time = Add({ .id = "combat.dying_time", .name = "Durée de l'état mourant",
                          .help = "Remplace le temps de l'état mourant donné par la difficulté (Amateur 30 s, Normal et "
                                  "Vétéran 15 s, Pro 3 s) par la valeur du curseur, pour Chris et Sheva, en histoire. "
                                  "La barre part toujours pleine. 0 s = mort immédiate quand la vie est vide.",
                          .category = "Combat", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = 15, .valueMin = 0, .valueMax = 60, .valueFormat = "%.0f s" });
    Feature* const pt = &time;
    time.set = [pt](bool on) {
        if (on && !EnsureTimeHook()) return false;
        g_timeValue = pt->value;
        g_timeOn = on;
        return true;
    };
    time.tick = [pt] { g_timeValue.store(pt->value, std::memory_order_relaxed); };

    Feature& mash = Add({ .id = "combat.dying_mash", .name = "Se relever seul (mourant)",
                          .help = "Comme en Mercenaries solo : quand tu es mourant, le prompt du jeu apparaît et chaque appui "
                                  "sur la touche indiquée remonte la barre ; au-dessus du maximum, tu te relèves (avec très "
                                  "peu de vie). La barre part plus bas (13/15, puis 6/15 et 2/15 si tu retombes sans t'être "
                                  "soigné : un soin ou Sheva qui te relève remet à 13/15). Le curseur multiplie ce que rapporte chaque appui (x1 = Mercenaries : 0,43 s "
                                  "par appui). Conseil : chaque appui rapporte toujours le même temps, mais plus l'état "
                                  "mourant dure longtemps (« Durée de l'état mourant »), plus il faut en remonter : pour "
                                  "retrouver la sensation de Mercenaries (15 s), multiplie par la durée / 15 (30 s : x2, "
                                  "60 s : x4). Sheva peut toujours te relever.",
                          .category = "Combat", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = 1, .valueMin = 0.2f, .valueMax = 50, .valueFormat = "x%.1f" });
    Feature* const pm = &mash;
    mash.set = [pm](bool on) {
        if (on && !EnsureMashHooks()) return false;
        g_mashFactor = pm->value;
        g_mashOn = on;
        return true;
    };
    mash.tick = [pm] {
        g_mashFactor.store(pm->value, std::memory_order_relaxed);
        ClearFallCountWhenHealed();
    };

    Add({ .id = "combat.dying_struggle", .name = "Se débattre en étant mourant",
          .help = "Quand tu es mourant, les prompts pour te débattre d'une prise (langue de licker, Majini, chien...) "
                  "restent affichés et tu peux te libérer. Décochée : comme dans le jeu, pas de lutte en étant mourant, "
                  "et le saut d'un licker ne peut pas être repoussé seul (« Se libérer seul d'un licker » et les "
                  "échappements automatiques ne marchent pas en étant mourant).",
          .category = "Combat", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureStruggleHook()) return false;
              g_struggleOn = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });

    Feature& re6 = Add({ .id = "combat.dying_re6", .name = "Mourant façon RE6",
                         .help = "Quand tu es mourant, la barre du jeu ne "
                                 "descend plus toute seule mais le moindre coup reçu te tue ; une barre de rétablissement "
                                 "se remplit au milieu de l'écran en N secondes (curseur) et tu te relèves quand elle est "
                                 "pleine : il faut survivre jusque-là. Elle ne se remplit que quand Chris est debout et "
                                 "libre (pas pendant sa chute et son relevé, ni pendant une saisie). Avec « Se relever seul (mourant) », marteler la "
                                 "touche partenaire la remplit plus vite (selon son multiplicateur ; en Mercenaries, "
                                 "toujours). Tu peux utiliser tes armes (en changer et ouvrir l'inventaire : « Mourant : "
                                 "changer d'arme ») ; tu gardes la démarche du mourant (sauf avec « Vitesse normale en "
                                 "étant mourant ») et aucun prompt d'action n'apparaît. Marche aussi en Mercenaries.",
                         .category = "Combat", .kind = Kind::Toggle,
                         .available = [] { return game::IsSupported(); },
                         .value = 15, .valueMin = 3, .valueMax = 60, .valueFormat = "%.0f s" });
    Feature* const pr = &re6;
    re6.set = [pr](bool on) {
        if (on) {
            if (!EnsureMashHooks() || !EnsureDrainGateHook() || !EnsurePlayerDamageHook() || !EnsureStruggleHook() ||
                !EnsureWeaponChangeHook())
                return false;
            if (!mem::ApplyAll({ &g_re6WeaponUnit, &g_re6CanWeapon, &g_re6CanKnife })) return false;
        } else {
            mem::RestoreAll({ &g_re6CanKnife, &g_re6CanWeapon, &g_re6WeaponUnit });
        }
        g_re6Seconds = pr->value;
        g_re6Progress = 0.0f;
        g_re6On = on;
        return true;
    };
    re6.tick = [pr] {
        g_re6Seconds.store(pr->value, std::memory_order_relaxed);
        TickRe6();
    };
    OnHud(DrawRe6);
    Add({ .id = "combat.dying_weapon_change", .name = "Mourant : changer d'arme",
          .help = "Quand tu es mourant (avec « Mourant façon RE6 », qui te laisse tes armes), tu peux changer d'arme "
                  "(raccourcis) et ouvrir l'inventaire. Décochée : comme dans RE6, tu gardes l'arme que tu avais en "
                  "tombant et l'inventaire ne s'ouvre pas jusqu'à te relever.",
          .category = "Combat", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (!on && !EnsureWeaponChangeHook()) return false;  // also with the RE6 mode (saved off: never set)
              g_dyingSwap = on;
              return true;
          },
          .available = [] { return game::IsSupported(); },
          .defaultOn = true });
    Add({ .id = "combat.dying_now", .name = "Me mettre mourant",
          .help = "Pour les tests : retire toute la vie du joueur 1 d'un coup, comme un coup mortel, et le met donc en "
                  "état mourant (même invincible ou avec la vie infinie).",
          .category = "Combat", .kind = Kind::Action,
          .run = [] {
              const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
              int16_t health = 0;
              if (!p || !mem::Read(p + 0x1364, health)) return false;
              if (Player1Dying()) {
                  Toast("Déjà mourant", false);
                  return false;
              }
              const bool done = health > 0 && HurtPlayer1Raw(health);
              LOG("dying: player 1 put dying for a test (health %d) %s", health, done ? "" : "- failed");
              return done;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "combat.dying_actions", .name = "Actions en étant mourant",
          .help = "Quand tu es mourant, les prompts d'action restent affichés comme en pleine forme (sauts, corps à "
                  "corps, portes, objets...) et les mouvements et esquives du trainer (mêlée libre, esquives) marchent.",
          .category = "Combat", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureStruggleHook()) return false;
              g_dyingActions = on;
              return true;
          },
          .available = [] { return game::IsSupported(); },
          .defaultOn = true });
    Add({ .id = "combat.dying_normal_speed", .name = "Vitesse normale en étant mourant",
          .help = "Quand tu es mourant, Chris marche et court à sa vitesse normale au lieu de la démarche du mourant. "
                  "Les prompts d'action restent cachés comme dans le jeu.",
          .category = "Combat", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on) {
                  if (!EnsureStruggleHook() ||
                      !mem::ApplyAll({ &g_normalMotion, &g_normalPosture, &g_normalWalkSpeed, &g_normalRunSpeed }))
                      return false;
              } else {
                  mem::RestoreAll({ &g_normalRunSpeed, &g_normalWalkSpeed, &g_normalPosture, &g_normalMotion });
              }
              g_normalSpeed = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });

    Add({ .id = "combat.grab_rescue", .name = "Sauvé si une prise te met mourant",
          .help = "Si un ennemi qui te tient te fait passer mourant alors que tu avais encore de la vie, tu te libères "
                  "automatiquement (saut de licker compris) et la barre de l'état mourant reste figée, sans plus aucun "
                  "dégât, jusqu'à ce que tu sois libre et relevé ; ensuite elle descend normalement. Si tu étais déjà "
                  "mourant avant la prise, rien ne change.",
          .category = "Combat", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureRescueHooks()) return false;
              g_rescueOn = on;
              if (!on) g_rescue = false;
              return true;
          },
          .tick = TickRescue,
          .available = [] { return game::IsSupported(); } });
    OnFrame(TickDyingRun);
}
}  // namespace features
