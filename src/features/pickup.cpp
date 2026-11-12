// Pickup without its animation (user, 2026-10-20). The game's pickup (MODLOG session 14): the item system calls the
// player's item_get handler VA 0xB6D310 (P+0x2E30 = item id, RequestAction(0x57, 0, 0, 1, 0), return RVA 0x76D38F,
// then VA 0xB6AF90: P+0x2E34 = 1 pending, the item system told when the action really started) -> item_get_first 0x57
// reaches for it -> the second handler VA 0xB6D3A0 requests item_get_second 0x58, whose first update (VA 0xC7FE20)
// does all the work: type = item unit +0x76 (sItem [RVA 0xDB2200] lookup VA 0x7BEA20), room left
// P.VA 0xB65BF0(type, 0, 1) -> TakeItem P.VA 0xB5A880(id) (sPlayer VA 0x7EA610 -> VA 0x7E9DF0: adds it by category and
// removes the world item), the HUD notice ([P+0x2F30] VA 0xC64F80), a record at grid position 0xA -> P.VA 0xB73B50(0xA,
// 0); no room -> the inventory menu (VA 0x792610). Its later states only animate.
// Here, for player 1 when there is room: the 0x57 request is refused (VA 0xB6AF90 then only leaves P+0x2E34 = 1, as
// for player 2's refused pickups) and the same work is done right after (the overlay's frame, same thread), then
// P+0x2E34 = 0. No room: the game's own pickup, animation and menu.
// A first try sped the two actions up (unit time scale +0xFD4 x50): 2 frames of the pickup pose stayed visible and the
// scale saved during a sprint was given back after it (Chris left at x1.8).
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace features {
bool EnsurePlayerRequestHook();             // action_log.cpp
bool RawButtonHeld(int button, int pad);   // quick_knife.cpp: as the player holds it, no trainer override
int Player1Pad();  // movement.cpp: the pad index player 1's buttons are read with
bool EnsurePadQueryHooks();                // quick_knife.cpp
bool Player1Dying();                       // dying.cpp

namespace {
constexpr int kGetFirst = 0x57;
constexpr uintptr_t kGetFirstCaller = 0x76D38F;  // return RVA of the item_get handler's request

std::atomic<bool> g_on{ false };
uint32_t g_pending = 0;  // item id taken at the next overlay frame
bool g_hasPending = false;

uintptr_t ItemUnit(uint32_t id) {
    using LookupFn = uintptr_t(__fastcall*)(void* items, void* edx, uint32_t id);
    uintptr_t items = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2200)), items) || !items) return 0;
    __try {
        return reinterpret_cast<LookupFn>(mem::Rva(0x3BEA20))(reinterpret_cast<void*>(items), nullptr, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool HasRoom(uintptr_t player, uint32_t type) {
    using RoomFn = bool(__fastcall*)(void* player, void* edx, uint32_t type, int a, int b);
    __try {
        return reinterpret_cast<RoomFn>(mem::Rva(0x765BF0))(reinterpret_cast<void*>(player), nullptr, type, 0, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Take(uintptr_t player, uint32_t id) {
    using TakeFn = uintptr_t(__fastcall*)(void* player, void* edx, uint32_t id);
    using NoticeFn = void(__fastcall*)(void* hud, void* edx);
    using EquipFn = void(__fastcall*)(void* player, void* edx, int slot, int b);
    uintptr_t record = 0, hud = 0;
    __try {
        record = reinterpret_cast<TakeFn>(mem::Rva(0x75A880))(reinterpret_cast<void*>(player), nullptr, id);
        if (mem::Read(player + 0x2F30, hud) && hud)
            reinterpret_cast<NoticeFn>(mem::Rva(0x864F80))(reinterpret_cast<void*>(hud), nullptr);
        uint32_t grid = 0;
        if (record && mem::Read(record + 0x18, grid) && grid == 0xA)
            reinterpret_cast<EquipFn>(mem::Rva(0x773B50))(reinterpret_cast<void*>(player), nullptr, 0xA, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("quick pickup: exception while taking item %08X", id);
    }
    mem::Write(player + 0x2E34, static_cast<uint8_t>(0));
    LOG("quick pickup: item %08X taken (record %08X)", id, static_cast<unsigned>(record));
}

void Tick() {
    if (!g_hasPending) return;
    g_hasPending = false;
    const uintptr_t p1 = actors::Player(0);
    if (p1) Take(p1, g_pending);
}

// ---- Healing without its animation (user, 2026-10-20: an equipped healing item or one used from the inventory).
// The game: the use handler VA 0xB788F0 (cdecl(?, player index, ?, ?, float)) reads the grid position to use at
// input ([P+0x2D64] + 0x220) + 0x434: 0xD (none) or the equipped one (cItemManager P+0x21A0 VA 0xC4BB00(0xE)) ->
// RequestAction(0x5F, 0, 0, -1, 0) (return RVA 0x77898B) then VA 0xB6AF90; another one -> weapon change to it first
// (VA 0xB782E0, the action 0x5F queued at P+0x1B3C). item_use_my_first 0x5F only animates; item_use_my_second 0x60
// (update VA 0xC80A80) uses it: P.VA 0xB5A8C0(P, grid, 0) (sPlayer VA 0x7E9B70: health and the stack), and on exit
// (VA 0xC80D40): result 0 and the record (VA 0xC4B700(key)) at count <= 0 -> cItem VA 0xC4C4E0(1) (emptied),
// sPlayer VA 0xB7EF70(2) ([sPlayer+0x5E0] &= ~2), inventory menu VA 0x7932E0(P, grid, -1). Here, for player 1, the
// handler does nothing and that work is done at the next overlay frame on the chosen grid position (no weapon change).
// From the inventory the use is a pending request ([P+0x2654] +0x5C = grid position, read every frame by the input
// update VA 0xB8AAF3 into input +0x434 with the use buttons +0x63 / +0x65 pressed); 0x5F's setup (VA 0xC8EC80) clears it
// ([P+0x2654] VA 0xC69B70(P): +0x5C = 0xD). Not cleared, the handler came again every ~350 ms (user, 2026-10-20: a
// second herb used, then the game's handler changing weapon to the emptied slot in a loop): cleared here too, and a
// second call while one heal is pending is ignored.
using HealHandlerFn = void(__cdecl*)(uint32_t a, uint32_t index, uint32_t c, uint32_t d, float e);
HealHandlerFn g_healHandler = nullptr;
std::atomic<bool> g_healOn{ false };
int g_healGrid = -1;  // grid position used at the next overlay frame
// ---- No attack from the inventory (user, 2026-10-20: the click confirming a heal or a weapon in the inventory became a
// knife stab; never time based, the player must be able to fight as soon as he is out). The inventory is
// [P+0x2654] (requests +0x58 weapon / +0x5C use): +0x68 bit 1 while it is open (0x101 opening, 0 from the close on),
// +0x04 state 0x1000003 open, 1 closing, 2 closed (recorded). Player 1's fire (0x1E) and knife actions (0x22..0x27) are
// refused while it is open and while a press that started in it is still held (fire 0x0E / quick knife 0x27, as
// really held: the quick knife played as the knife aim holds knife aim + fire itself until its stab, which the guard
// refused - user: no attack for 2 s after equipping; that emulation no longer starts on an inventory click).
// The pause menu too (user, 2026-10-21: clicking "Continuer" fired the quick shot back in game): while it is shown (the
// game also opens it when its window loses the focus) its unit's slot 9 runs every frame (PauseMenuShown, below); a
// press that begins then, or within 300 ms of it closing (the click that closes it), counts as made in a menu. One
// already held before the menu opened does not.
std::atomic<bool> g_invGuardOn{ false };
bool g_pressInInventory = false;  // the fire / quick knife press began with the inventory open, or in the pause menu
bool g_invOpenLastFrame = false, g_heldLastFrame = false, g_menuLastFrame = false;
DWORD g_menuSeen = 0;             // last overlay frame that saw the pause menu shown
bool PauseMenuShown();

bool AttackHeld() {
    return RawButtonHeld(0x0E, -1) || RawButtonHeld(0x0E, Player1Pad()) || RawButtonHeld(0x27, -1) || RawButtonHeld(0x27, Player1Pad());
}

bool InventoryOpen(uintptr_t player) {
    uintptr_t ui = 0;
    uint32_t flags = 0;
    return mem::Read(player + 0x2654, ui) && ui && mem::Read(ui + 0x68, flags) && (flags & 1);
}

void TickInventoryGuard() {
    const uintptr_t p1 = actors::Player(0);
    const DWORD now = GetTickCount();
    const bool shown = PauseMenuShown();
    if (shown && !g_menuSeen) LOG("menu guard: pause menu shown");
    if (shown) g_menuSeen = now | 1;
    else if (g_menuSeen && now - g_menuSeen >= 300) {
        LOG("menu guard: pause menu closed");
        g_menuSeen = 0;
    }
    const bool menu = g_menuSeen != 0;
    const bool held = AttackHeld(), open = p1 && InventoryOpen(p1);
    if (!held) g_pressInInventory = false;
    else if (open || ((g_invOpenLastFrame || menu) && !g_heldLastFrame)) {
        if (!g_pressInInventory && menu && !open) LOG("menu guard: attack press made in a menu, ignored until released");
        g_pressInInventory = true;
    }
    g_invOpenLastFrame = open;
    g_menuLastFrame = menu;
    g_heldLastFrame = held;
}

// The stab can be requested in the frame the click closes the inventory, before the overlay frame saw the press: a
// press newer than the last overlay frame, which saw the inventory open (or a menu), counts as made in it.
bool InventoryAttack(uintptr_t player) {
    if (InventoryOpen(player) || g_pressInInventory) return true;
    if ((g_invOpenLastFrame || g_menuLastFrame) && !g_heldLastFrame && AttackHeld()) {
        g_pressInInventory = true;
        return true;
    }
    return false;
}

void ClearUseRequest(uintptr_t player) {
    using ClearFn = void(__fastcall*)(void* ui, void* edx, uintptr_t player);
    uintptr_t ui = 0;
    if (!mem::Read(player + 0x2654, ui) || !ui) return;
    __try {
        reinterpret_cast<ClearFn>(mem::Rva(0x869B70))(reinterpret_cast<void*>(ui), nullptr, player);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

uintptr_t PlayerOf(uint32_t index) {
    using PlayerFn = uintptr_t(__fastcall*)(void* players, void* edx, uint32_t index);
    uintptr_t players = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB27DC)), players) || !players) return 0;
    __try {
        return reinterpret_cast<PlayerFn>(mem::Rva(0x3E6220))(reinterpret_cast<void*>(players), nullptr, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// cItemManager (P+0x21A0) lookups: VA 0xC4BB00(key) = grid position of the record (key 0xE = equipped), VA 0xC4B700(key)
// = the record (key = 0xE or a grid position).
int GridOf(uintptr_t player, int key) {
    using GridFn = int(__fastcall*)(void* items, void* edx, int key);
    __try {
        return reinterpret_cast<GridFn>(mem::Rva(0x84BB00))(reinterpret_cast<void*>(player + 0x21A0), nullptr, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xD;
    }
}

uintptr_t RecordOf(uintptr_t player, int key) {
    using RecordFn = uintptr_t(__fastcall*)(void* items, void* edx, int key);
    __try {
        return reinterpret_cast<RecordFn>(mem::Rva(0x84B700))(reinterpret_cast<void*>(player + 0x21A0), nullptr, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void __cdecl HookHealHandler(uint32_t a, uint32_t index, uint32_t c, uint32_t d, float e) {
    const uintptr_t p1 = actors::Player(0);
    if (g_healOn.load(std::memory_order_relaxed) && p1 && PlayerOf(index) == p1) {
        uintptr_t input = 0;
        int grid = 0xD;
        if (mem::Read(p1 + 0x2D64, input) && input) mem::Read(input + 0x220 + 0x434, grid);
        if (grid == 0xD) grid = GridOf(p1, 0xE);
        if (g_healGrid >= 0) {  // one already pending
            ClearUseRequest(p1);
            return;
        }
        if (grid >= 0 && grid < 0xD && RecordOf(p1, grid)) {
            ClearUseRequest(p1);
            g_healGrid = grid;
            return;
        }
        ClearUseRequest(p1);  // an emptied slot: the game's handler would change weapon to it
        LOG("quick heal: nothing to use (grid %d), request cleared", grid);
        return;
    }
    g_healHandler(a, index, c, d, e);
}

// The heal's feedback (user, 2026-10-21: the green wave from the health gauge and the heal sound were gone without the
// animation). It is the HUD's: the player's AddHealth (vfunc, VA 0xB7C8D0) sets the event bit P+0x2D8C |= 2 when the
// health rises (|= 1 when it falls); the HUD update (VA 0x9F8150) calls VA 0x9EF900 (thiscall(HUD, player index),
// ret 4) for each player every frame, which on that bit starts the wave (HUD +0x218 + index = 1,
// +0x21C + 4i += 1, timer +0x224 + 4i = 0, cleared after its duration by VA 0x9EBDE0) and, for player 1, the sound
// (VA 0x9ED980(index, 0x27)). The bit is rewritten by the player update (VA 0xB66DE0 / 0xB78C80) before the HUD runs,
// so a heal done at the overlay frame never showed: the check is hooked and, after a quick heal, sees the bit set.
// (A first try spawned 0x60's item effect, core cr0008 / cr0020 = the spray's droplets and "pshit": not this.)
using HudHealCheckFn = void(__fastcall*)(void* hud, void* edx, int index);
HudHealCheckFn g_hudHealCheck = nullptr;
std::atomic<bool> g_healFeedback{ false };  // a quick heal waits for the HUD's next check

void __fastcall HookHudHealCheck(void* hud, void* edx, int index) {
    if (actors::SlotIs(index, 0) && g_healFeedback.exchange(false, std::memory_order_relaxed)) {
        const uintptr_t p1 = actors::Player(0);
        uint32_t flags = 0;
        if (p1 && mem::Read(p1 + 0x2D8C, flags)) mem::Write(p1 + 0x2D8C, flags | 2u);
    }
    g_hudHealCheck(hud, edx, index);
}

bool EnsureHudHealHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x5EF900);
    const uint8_t expected[] = { 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x85, 0xFF, 0x8B, 0xF1 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("quick heal: HUD check unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookHudHealCheck), reinterpret_cast<void**>(&g_hudHealCheck));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("quick heal: HUD heal check hook installed at %p", target);
    return true;
}

void Heal(uintptr_t player, int grid) {
    using UseFn = uintptr_t(__fastcall*)(void* player, void* edx, uintptr_t self, int grid, int z);
    using ClearFn = void(__fastcall*)(void* record, void* edx, int b);
    using FlagFn = void(__fastcall*)(void* players, void* edx, uint32_t mask);
    using MenuFn = void(__fastcall*)(void* menu, void* edx, uintptr_t player, int grid, int c);
    int16_t before = 0, after = 0;
    mem::Read(player + 0x1364, before);
    uintptr_t result = 0, players = 0, menu = 0;
    __try {
        result = reinterpret_cast<UseFn>(mem::Rva(0x75A8C0))(reinterpret_cast<void*>(player), nullptr, player, grid, 0);
        int32_t count = 0;
        const uintptr_t record = RecordOf(player, grid);
        if (!result && record && mem::Read(record + 8, count) && count <= 0)
            reinterpret_cast<ClearFn>(mem::Rva(0x84C4E0))(reinterpret_cast<void*>(record), nullptr, 1);
        if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB27DC)), players) && players)
            reinterpret_cast<FlagFn>(mem::Rva(0x77EF70))(reinterpret_cast<void*>(players), nullptr, 2);
        if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C1C)), menu) && menu)
            reinterpret_cast<MenuFn>(mem::Rva(0x3932E0))(reinterpret_cast<void*>(menu), nullptr, player, grid, -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("quick heal: exception (grid %d)", grid);
    }
    mem::Read(player + 0x1364, after);
    if (after > before) g_healFeedback = true;  // the HUD's wave and sound at its next check
    LOG("quick heal: grid %d used, health %d -> %d (result %08X)", grid, before, after, static_cast<unsigned>(result));
}

// ---- Smart heal shortcut (user, 2026-10-20). The healing item is chosen by player 1's health (P+0x1364 / max
// +0x1366): above 60 % green herb, then G+G, then a full heal; 20-60 % G+G, then a full heal, then green; below 20 % a
// full heal, then G+G, then green. Full heal = G+R or first aid spray (same effect): the smaller stack first, to free a
// slot sooner (G+R on a tie); several stacks of one item: the smaller one. Items (item_names.inc): green 0x0301 /
// 0x0305, G+G 0x0307, G+R 0x0309, spray 0x0304. Eggs (user, 2026-10-21: same levels): white 0x013C = green, brown 0x013D
// = G+G, gold 0x013E = full heal (the rotten one 0x0136 does not heal); in each level the smallest stack first, herbs
// before the spray before eggs on a tie. Records: cItemManager P+0x21A0, 24 cItem of 0x30 from +4 (+0x04 id,
// +0x08 count, +0x18 grid position, +0x29 in use). With "Soin sans animation" the item is used at once (Heal above);
// otherwise the inventory's own pending use request is left ([P+0x2654]+0x5C = grid position): the game's heal.
struct Stack {
    int grid = -1;
    int32_t count = 0;
};

Stack Smallest(uintptr_t player, std::initializer_list<uint16_t> ids) {
    Stack best;
    for (int k = 0; k < 24; ++k) {
        const uintptr_t item = player + 0x21A0 + 4 + 0x30 * k;
        uint8_t inUse = 0;
        uint32_t id = 0;
        int32_t count = 0, grid = -1;
        if (!mem::Read(item + 0x29, inUse) || !inUse || !mem::Read(item + 4, id) || !mem::Read(item + 8, count) ||
            !mem::Read(item + 0x18, grid) || grid < 0 || grid > 8 || count <= 0)
            continue;
        bool wanted = false;
        for (uint16_t w : ids) wanted = wanted || (id & 0xFFFF) == w;
        if (wanted && (best.grid < 0 || count < best.count)) best = { grid, count };
    }
    return best;
}

// The smallest stack of one level; on equal counts the item listed first wins.
Stack Level(uintptr_t player, std::initializer_list<std::initializer_list<uint16_t>> items) {
    Stack best;
    for (const auto& ids : items) {
        const Stack s = Smallest(player, ids);
        if (s.grid >= 0 && (best.grid < 0 || s.count < best.count)) best = s;
    }
    return best;
}

bool SmartHeal() {
    const uintptr_t p1 = actors::Player(0);
    int16_t health = 0, max = 0;
    if (!p1 || !actors::InGame() || !mem::Read(p1 + 0x1364, health) || !mem::Read(p1 + 0x1366, max) || max <= 0 ||
        health <= 0 || Player1Dying())
        return false;
    if (health >= max) {
        LOG("smart heal: health already full");
        return false;
    }
    const Stack green = Level(p1, { { 0x0301, 0x0305 }, { 0x013C } }), greenGreen = Level(p1, { { 0x0307 }, { 0x013D } }),
                full = Level(p1, { { 0x0309 }, { 0x0304 }, { 0x013E } });
    const float ratio = static_cast<float>(health) / static_cast<float>(max);
    const Stack* order[3];
    if (ratio > 0.6f) order[0] = &green, order[1] = &greenGreen, order[2] = &full;
    else if (ratio > 0.2f) order[0] = &greenGreen, order[1] = &full, order[2] = &green;
    else order[0] = &full, order[1] = &greenGreen, order[2] = &green;
    int grid = -1;
    for (const Stack* s : order)
        if (s->grid >= 0) {
            grid = s->grid;
            break;
        }
    if (grid < 0) {
        LOG("smart heal: no healing item");
        return false;
    }
    LOG("smart heal: health %d / %d (%.0f %%) -> grid %d", health, max, ratio * 100.0f, grid);
    if (g_healOn.load(std::memory_order_relaxed)) {
        Heal(p1, grid);
        return true;
    }
    uintptr_t ui = 0;
    return mem::Read(p1 + 0x2654, ui) && ui && mem::Write<int32_t>(ui + 0x5C, grid);
}

void TickHeal() {
    if (g_healGrid < 0) return;
    const int grid = g_healGrid;
    g_healGrid = -1;
    if (const uintptr_t p1 = actors::Player(0)) Heal(p1, grid);
}

bool EnsureHealHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x7788F0);
    const uint8_t expected[] = { 0x8B, 0x0D, 0xDC, 0x27, 0x1B, 0x01, 0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("quick heal: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookHealHandler), reinterpret_cast<void**>(&g_healHandler));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("quick heal: use handler hook installed at %p", target);
    return true;
}

// ---- The pause menu unit uIdPause00 (DTI RVA 0xDF5518, vtable RVA 0xBA821C, 13 slots; slots 1, 3, 5, 6, 9 are its own,
// the others uIdBase's / cUnit's): those five slots are redirected to stubs that count their calls (diag.pause_menu_log
// logs them per second). Recorded 2026-10-21: one instance; slot 6 runs every frame (120/s) from the start, slot 9 from
// the first opening and only while the menu is shown (0 while playing, 120/s in the menu, also when the game opens it
// on losing the focus); 1, 3, 5 never ran.
constexpr uintptr_t kPauseVtable = 0xBA821C;
constexpr int kPauseSlots[5] = { 1, 3, 5, 6, 9 };
volatile LONG g_pauseCalls[5] = {};
uintptr_t g_pauseOriginal[5] = {};
mem::Patch* g_pausePatch[5] = {};

__declspec(naked) void PauseSlot0() { __asm { lock inc dword ptr [g_pauseCalls + 0]
                                              jmp dword ptr [g_pauseOriginal + 0] } }
__declspec(naked) void PauseSlot1() { __asm { lock inc dword ptr [g_pauseCalls + 4]
                                              jmp dword ptr [g_pauseOriginal + 4] } }
__declspec(naked) void PauseSlot2() { __asm { lock inc dword ptr [g_pauseCalls + 8]
                                              jmp dword ptr [g_pauseOriginal + 8] } }
__declspec(naked) void PauseSlot3() { __asm { lock inc dword ptr [g_pauseCalls + 12]
                                              jmp dword ptr [g_pauseOriginal + 12] } }
volatile LONG g_pauseMenuFrames = 0;  // slot 9 calls, never reset
__declspec(naked) void PauseSlot4() { __asm { lock inc dword ptr [g_pauseCalls + 16]
                                              lock inc dword ptr [g_pauseMenuFrames]
                                              jmp dword ptr [g_pauseOriginal + 16] } }
void (*const kPauseStubs[5])() = { PauseSlot0, PauseSlot1, PauseSlot2, PauseSlot3, PauseSlot4 };

std::vector<uint8_t> Bytes(uintptr_t value) {
    return { static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
             static_cast<uint8_t>(value >> 24) };
}

bool EnsurePauseCounters() {
    static bool done = false;
    if (done) return true;
    // The vtable must still be the one recorded: GetDTI (slot 4) at VA 0xA30940.
    uintptr_t getDti = 0;
    const uintptr_t vtable = reinterpret_cast<uintptr_t>(mem::Rva(kPauseVtable));
    if (!mem::Read(vtable + 4 * 4, getDti) || getDti != reinterpret_cast<uintptr_t>(mem::Rva(0x630940))) {
        LOG("pause menu: unexpected vtable (slot 4 %08X)", static_cast<unsigned>(getDti));
        return false;
    }
    for (int i = 0; i < 5; ++i) {
        mem::Read(vtable + 4 * kPauseSlots[i], g_pauseOriginal[i]);
        g_pausePatch[i] = new mem::Patch("pause menu slot", kPauseVtable + 4 * kPauseSlots[i], Bytes(g_pauseOriginal[i]),
                                         Bytes(reinterpret_cast<uintptr_t>(kPauseStubs[i])));
        if (!g_pausePatch[i]->Apply()) {
            LOG("pause menu: slot %d not redirected", kPauseSlots[i]);
            for (int k = 0; k <= i; ++k) g_pausePatch[k]->Restore();
            return false;
        }
    }
    OnRestore([] {
        for (mem::Patch* patch : g_pausePatch)
            if (patch) patch->Restore();
    });
    done = true;
    LOG("pause menu: slots 1 3 5 6 9 counted");
    return true;
}

std::atomic<bool> g_pauseLog{ false };

// Overlay frame: slot 9 ran since the previous check (the menu is drawn at every game frame, 120 FPS here; the overlay
// may draw faster, so a frame without a call is taken as shown for 50 ms).
// Also asked from the game thread (menu_keys.cpp, in the keyboard update). The difference is signed: the other thread
// may have stored a newer tick than the one just read (unsigned, it read as 49 days: "closed" for a frame, and every
// such flicker was a new arrow press for the menu keys, user 2026-10-21: one tap skipped several lines).
bool PauseMenuShown() {
    static std::atomic<LONG> last{ 0 };
    static std::atomic<DWORD> calledAt{ 0 };
    const DWORD tick = GetTickCount();
    const LONG frames = g_pauseMenuFrames;
    if (frames != last.load(std::memory_order_relaxed)) {
        last.store(frames, std::memory_order_relaxed);
        calledAt.store(tick | 1, std::memory_order_relaxed);
    }
    const DWORD at = calledAt.load(std::memory_order_relaxed);
    return at && static_cast<int32_t>(tick - at) < 50;
}

void TickPauseLog() {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    LONG counts[5];
    for (int i = 0; i < 5; ++i) counts[i] = InterlockedExchange(&g_pauseCalls[i], 0);
    LOG("pause menu calls in the last second: slot 1 %ld, 3 %ld, 5 %ld, 6 %ld, 9 %ld", counts[0], counts[1], counts[2],
        counts[3], counts[4]);
}

}  // namespace

// For time_speed.cpp: player 1's inventory open, the pause menu shown (its watch installed first).
bool Player1InventoryOpen() {
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    return p1 && InventoryOpen(p1);
}
bool PauseMenuIsShown() { return PauseMenuShown(); }
bool EnsurePauseMenuWatch() { return EnsurePauseCounters(); }

// For the quick knife played as the knife aim (quick_knife.cpp): its key press was made in the inventory.
bool InventoryClickHeld() {
    const uintptr_t p1 = actors::Player(0);
    return g_invGuardOn.load(std::memory_order_relaxed) && p1 && InventoryAttack(p1);
}

// Player 1's RequestAction (action_log.cpp hook), before the game takes it: true = refused.
bool PickupRefuses(uintptr_t player, int action, uintptr_t caller) {
    if (g_invGuardOn.load(std::memory_order_relaxed) && (action == 0x1E || (action >= 0x22 && action <= 0x27)) &&
        player == actors::Player(0) && InventoryAttack(player)) {
        LOG("inventory: action %02X refused (a click in the inventory)", action);
        return true;
    }
    if (!g_on.load(std::memory_order_relaxed) || action != kGetFirst || player != actors::Player(0) ||
        caller - reinterpret_cast<uintptr_t>(mem::Base()) != kGetFirstCaller)
        return false;
    uint32_t id = 0;
    uint16_t type = 0;
    const uintptr_t unit = mem::Read(player + 0x2E30, id) ? ItemUnit(id) : 0;
    if (!unit || !mem::Read(unit + 0x76, type) || type == 0xFFFF) return false;
    if (!HasRoom(player, type)) {
        LOG("quick pickup: no room for item %08X (type %04X): the game's pickup", id, type);
        return false;
    }
    g_pending = id;
    g_hasPending = true;
    return true;
}

// For the reload shortcut (quick_knife.cpp; user, 2026-10-23: with a full inventory the pickup prompt still showed and
// blocked the reload): true when the item nearest player 1 within 2 m has no room left in his inventory. The prompt
// itself does not say which item it is for (its cActionButton's owner is the player) and P+0x2E30 is only set by the
// press: the nearest unit of sItem ([RVA 0xDB2200], units +0x30, count +0x24, id +0xFD0, position +0x30), its type
// from the game's item lookup (+0x76) as for the quick pickup.
bool NearbyItemHasNoRoom() {
    const uintptr_t p = actors::Player(0);
    float pos[3]{};
    uint32_t count = 0;
    uintptr_t items = 0, array = 0;
    if (!p || !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos)) ||
        !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2200)), items) || !items || !mem::Read(items + 0x24, count) ||
        !mem::Read(items + 0x30, array) || !array || count > 4096)
        return false;
    constexpr float kRange = 200.0f;  // cm
    float best = kRange * kRange;
    uint32_t bestId = 0;
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t unit = 0;
        float at[3]{};
        uint32_t id = 0;
        if (!mem::Read(array + 4 * i, unit) || !unit || !mem::SafeRead(reinterpret_cast<void*>(unit + 0x30), at, sizeof(at)) ||
            !mem::Read(unit + 0xFD0, id))
            continue;
        const float dx = at[0] - pos[0], dy = at[1] - pos[1], dz = at[2] - pos[2], d = dx * dx + dy * dy + dz * dz;
        if (d < best) {
            best = d;
            bestId = id;
            found = true;
        }
    }
    uint16_t type = 0;
    const uintptr_t record = found ? ItemUnit(bestId) : 0;
    if (!record || !mem::Read(record + 0x76, type) || type == 0xFFFF) return false;
    const bool noRoom = !HasRoom(p, type);
    LOG("reload shortcut: item %08X (type %04X) %.0f cm away, %s", bestId, type, std::sqrt(best),
        noRoom ? "no room: reload allowed" : "room left");
    return noRoom;
}

void RegisterPickup() {
    Add({ .id = "inventory.quick_pickup", .name = "Ramassage sans animation",
          .help = "Ramasser un objet ne joue plus l'animation de ramassage : l'objet est ajouté tout de suite (nouvelle "
                  "arme équipée comme d'habitude) et Chris continue ce qu'il faisait. Inventaire plein : ramassage "
                  "normal avec son menu.",
          .category = "Inventaire", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsurePlayerRequestHook()) return false;
              g_on = on;
              g_hasPending = false;
              return true;
          },
          .tick = [] {
              if (actors::InGame()) Tick();
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "inventory.quick_heal", .name = "Soin sans animation",
          .help = "Utiliser un objet de soin (équipé ou depuis l'inventaire) ne joue plus l'animation de soin ni le "
                  "changement d'arme : l'objet est utilisé tout de suite et Chris garde son arme en main.",
          .category = "Inventaire", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureHealHook() || !EnsureHudHealHook())) return false;
              g_healOn = on;
              g_healGrid = -1;
              return true;
          },
          .tick = [] {
              if (actors::InGame()) TickHeal();
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "inventory.smart_heal", .name = "Soin intelligent (raccourci)",
          .help = "Utilise le soin le plus adapté à la vie de Chris : plus de 60 % herbe verte (ou oeuf blanc), puis "
                  "double verte (ou oeuf brun), puis verte + rouge, spray ou oeuf doré ; entre 20 et 60 % double verte "
                  "d'abord ; moins de 20 % verte + rouge, spray ou oeuf doré d'abord. Dans un même niveau, la plus "
                  "petite pile part en premier. Sans animation si « Soin sans animation » est coché.",
          .category = "Inventaire", .kind = Kind::Action,
          .run = [] { return SmartHeal(); },
          .available = [] { return actors::InGame(); } });
    Add({ .id = "diag.pause_menu_log", .name = "Journal du menu pause (recherche)",
          .help = "Écrit dans trainer.log, chaque seconde, combien de fois les fonctions du menu pause ont été appelées.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsurePauseCounters()) return false;
              g_pauseLog = on;
              return true;
          },
          .tick = TickPauseLog,
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "inventory.no_attack", .name = "Pas d'attaque depuis l'inventaire ou un menu",
          .help = "Un clic fait dans l'inventaire (valider un soin, équiper une arme...) ou dans un menu (« Continuer » "
                  "du menu pause...) ne déclenche plus de tir, de tir rapide ni de coup de couteau. Hors des menus rien "
                  "ne change : dès que ce clic est relâché, l'attaque remarche.",
          .category = "Inventaire", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsurePlayerRequestHook() || !EnsurePadQueryHooks() || !EnsurePauseCounters())) return false;
              g_invGuardOn = on;
              g_pressInInventory = false;
              return true;
          },
          .tick = [] {
              if (actors::InGame()) TickInventoryGuard();
          },
          .available = [] { return game::IsSupported(); } });
}
}  // namespace features
