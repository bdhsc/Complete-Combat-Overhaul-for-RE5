// Fire rate and reload speed per weapon (user, 2026-10-21: "like RE4's upgrades"; RE5 upgrades firepower, reload and
// capacity only, and the reload as one level for every weapon).
// Fire: the fire action's update (VA 0xC89F30, this = action, +0x18 = player) asks VA 0xC708A0 (cdecl(item, rapid),
// float in st0: table VA 0x11C0628 + ((item & 0xFF) * 0x7A + rapid) * 4) for the weapon's fire interval in frames
// (M92F 26, SMGs and rifles 6, shotguns and grenade launcher 73, Hydra 54, magnums 45 / 47 / 71, S75 104, sniper rifles
// 900, bow 999; the inventory record's rapid byte +0x21 picks entry 1 = 1 frame, the bursts of "Tir rapide"). Two
// callers:
// - return VA 0xC8A018: compared to the action's timer +0x20 (frames, advanced by the frame time x the unit time scale
//   +0xFD4): past it the next shot may start (player flag 0x20000000 through VA 0xB5C350);
// - return VA 0xC8A989: the weapon parameter +0x90 (table VA 0x11B5170 + (item & 0xFF) * 0x140) plus it, compared to
//   the main motion's frame P+0x398, sets P+0x2760.
// Found with a read trace on the M92F record's rapid byte (two readers, 2026-10-21). For player 1 and a weapon whose
// slider is above x1 (rapid byte 0): the first answer is divided by the slider, and while he fires (action 0x1E) the
// main motion's speed P+0x3A4 (layer 0 record P+0x380, +0x24) is multiplied by it (the shot and the pump play at the
// new pace; the motion check, the second answer, follows the faster motion by itself).
// The fire action's timer counts real time (VA 0xC8A04C: sFps +0x28, the frame time before the time speed, x the unit's
// +0xFD4) while the shot and the pump play at the world's: in a slow motion the next shot came before the pump ended
// (user, 2026-10-29, the perfect dodge's x0.25: "Chris peut l'interrompre"; the trainer's fire rate made it worse). So
// the first answer is also divided by the shooter's time speed (time_speed.cpp: the world's, or player 1's own while
// he keeps his speed), with or without the fire rate; time_speed.cpp installs the hook when it slows the game.
// Reload: the reload action 0x1F (update VA 0xC8AC30) has no timer of its own: its steps wait for the main motion's
// frame P+0x398 (thresholds from VA 0x770380) and its end (P+0x388 bit 0), and it plays motions on other layers too
// (VA 0xC77290: layers 2-4). Every motion start writes its layer's speed (the SetMotion speed argument, VA 0x59E8F5;
// write trace on P+0x3A4 during reloads, 2026-10-21), so while he reloads the speeds of layers 0-6 are multiplied by
// the reload slider, each from the game's own value (re-read whenever the game set a new motion). Layer 7 is left
// alone (the legs of "Marcher pendant les actions", else the head).
// Walking while firing / reloading is unchanged: the trainer writes the position (movement.cpp) at its own speed.
// Power, critical, capacity (user, 2026-10-27: "a power, critical and ammo column too"; every column for players 1 and
// 2: "player 2 must apply the same values"). Every slider is a share of the weapon as it is now, upgrades included (user:
// "x1 is not the default value but the current upgrade's: the M92F 150, upgraded to 200, x1 = 200"): each multiplies the
// game's own value of the moment, never sets one.
// - power: the players' hits multiplied in the enemies' ApplyDamage (knife.cpp, like the knife's power): his bullets (hit
//   flag 0x00080000) with the weapon in hand, and every hit but the knife's and his melee moves' while he holds a
//   launcher, the bow or the flamethrower (their hit flags were never recorded);
// - critical: the hit's critical value +0x38 (the weapon's, upgrades included: pistol 4, knife 0) multiplied in
//   knife.cpp's OnHit (a damaging head hit on a Majini is critical when random % 8 < (int) value, VA 0x860CA9; the
//   fraction left by the product is rolled, so x1.5 on 4 gives 6, on 1 gives 1 or 2 half the time each); x0 = never;
// - capacity: the magazine size (cItemManager getter RVA 0x84C050, thiscall(record) ret 4: a weapon's
//   [VA 0x11C06A0 + ((item & 0xFF) * 0x7A + capacity level record +0x23) * 4], read by the reloads, 12 callers),
//   multiplied for the players' records (P+0x21A4 + 0x30 * k).
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "ui/sliders.hpp"

#include <windows.h>
#include <MinHook.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace features {
uint16_t PlayerHeldItem(uintptr_t player);  // knife.cpp
std::string ItemDisplayName(uint16_t id);   // inventory.cpp
bool EnsureEnemyDamageHook();               // knife.cpp: the power is applied in it
bool EnsureEnemyOnHitHook();                // knife.cpp: the critical value is set in it
float Player1TimeSpeed();                   // time_speed.cpp: the time speed player 1 lives at
float WorldTimeSpeed();                     // time_speed.cpp
bool ShellReloadWeapon(uint16_t item);      // shell_reload.cpp: reloaded shell by shell
bool EnsureReloadStepHook();                // shell_reload.cpp: the reload's fill step hooked

namespace {
constexpr uint32_t kFire = 0x1E, kReload = 0x1F;
constexpr int kSpedLayers = 7;             // fire and reload: layers 0..6

// The four knives (user, 2026-11-05: "each knife its own power, criticals, range, stagger, strike rate and durability,
// all in the weapon table; no global knife power"), then the firearms (the grenade launcher's loaded variants share its
// sliders; 0x0112 Barry's Samurai Edge, user 2026-10-27).
constexpr uint16_t kKnives[] = { 0x0101, 0x010E, 0x0123, 0x0124 };
constexpr uint16_t kWeapons[] = { 0x0101, 0x010E, 0x0123, 0x0124, 0x0102, 0x0110, 0x0111, 0x011E, 0x011F, 0x0129, 0x0112, 0x0103, 0x0113, 0x0109, 0x011D,
                                  0x0104, 0x0116, 0x0117, 0x0119, 0x010B, 0x011A, 0x011B, 0x0105, 0x011C, 0x0120, 0x010C,
                                  0x010D, 0x0135, 0x010F, 0x0115, 0x0121 };

// One value per item & 0xFF (`fallback` = the game's), saved in its own file. Every multiplier from x0.05 to x20 (user,
// 2026-11-04: "everything from 0.05, up to 20 instead of 10"; the critical keeps x0 = never), the shells per push 1 to
// 20 (a count); the sliders are logarithmic, x1 in their middle.
struct Table {
    std::atomic<float> values[0x40];
    const wchar_t* file;
    float min, max, fallback;
};
Table g_fire{ {}, L"fire_rate.ini", 0.05f, 20.0f, 1.0f }, g_reload{ {}, L"reload_speed.ini", 0.05f, 20.0f, 1.0f };
Table g_power{ {}, L"weapon_power.ini", 0.05f, 20.0f, 1.0f };
Table g_critical{ {}, L"weapon_critical.ini", 0.0f, 20.0f, 1.0f };
Table g_capacity{ {}, L"weapon_capacity.ini", 0.05f, 20.0f, 1.0f };
// The shells each push of a shell-by-shell reload puts in (user, 2026-11-01: "in the weapon table, so the Ithaca
// takes them two by two and the revolver one by one"), shell_reload.cpp.
Table g_shells{ {}, L"shells_per_push.ini", 1.0f, 20.0f, 1.0f };
// The knives' own columns (their options were in the Couteau tab; parries stay in Parade): a critical head hit's chance
// (%), the range (x the hitting shape's size), the chance a hit in each zone (head, arms, legs, body) staggers an ordinary
// Majini as a bullet would (above 100 %: every hit), the chance a hit on the plaga out of a Majini's neck gives it the
// head flash, the units of a new knife with "Durabilité du couteau".
Table g_knifeCritical{ {}, L"knife_critical.ini", 0.0f, 100.0f, 0.0f };
Table g_knifeRange{ {}, L"knife_range.ini", 0.2f, 5.0f, 1.0f };
Table g_knifeStagger[4]{ { {}, L"knife_stagger_head.ini", 0.0f, 1000.0f, 0.0f },
                         { {}, L"knife_stagger_arms.ini", 0.0f, 1000.0f, 0.0f },
                         { {}, L"knife_stagger_legs.ini", 0.0f, 1000.0f, 0.0f },
                         { {}, L"knife_stagger_body.ini", 0.0f, 1000.0f, 0.0f } };
Table g_knifePlaga{ {}, L"knife_plaga.ini", 0.0f, 100.0f, 0.0f };
Table g_knifeResistance{ {}, L"knife_resistance.ini", 10.0f, 500.0f, 100.0f };
Table g_knifePrice{ {}, L"knife_price.ini", 0.0f, 50000.0f, 0.0f };  // what a sale gives (the game's: 0)

int KnifeRow(uint16_t item) {
    for (int k = 0; k < 4; ++k)
        if (kKnives[k] == item) return k;
    return -1;
}

// A row's value before the user touches it: the game's (x1, 0 %), but the knives' power and units (their options'
// values of 2026-10-28: Chris's knife x0.8 / 120, Sheva's x1.2 / 50, the variants x1 / 100).
float DefaultRate(const Table& table, uint16_t item) {
    static constexpr float kPower[4] = { 0.8f, 1.2f, 1.0f, 1.0f }, kUnits[4] = { 120.0f, 50.0f, 100.0f, 100.0f };
    const int k = KnifeRow(item);
    if (k >= 0 && &table == &g_power) return kPower[k];
    if (k >= 0 && &table == &g_knifeResistance) return kUnits[k];
    return table.fallback;
}
std::atomic<bool> g_fireOn{ false }, g_reloadOn{ false }, g_powerOn{ false }, g_criticalOn{ false }, g_capacityOn{ false };
uintptr_t g_fireReturn[2] = {};

uint16_t SliderItem(uint16_t item) {
    if ((item >= 0x0125 && item <= 0x0127) || (item >= 0x0139 && item <= 0x013B)) return 0x010C;
    return item;
}

float Rate(const Table& table, uint16_t item) {
    item = SliderItem(item);
    if ((item & 0xFF00) != 0x0100 || (item & 0xFF) >= 0x40) return table.fallback;
    const float rate = table.values[item & 0xFF].load(std::memory_order_relaxed);
    return rate >= table.min && rate <= table.max ? rate : table.fallback;
}

bool InTable(uint16_t item) {
    item = SliderItem(item);
    for (uint16_t weapon : kWeapons)
        if (weapon == item) return true;
    return false;
}

// The magazine size of the players' weapons.
using CapacityFn = int(__fastcall*)(void* manager, void* edx, uintptr_t record);
CapacityFn g_capacityGame = nullptr;

bool PlayerRecord(uintptr_t record) {
    for (int i = 0; i < 2; ++i) {
        const uintptr_t p = actors::Player(i), first = p + 0x21A4;
        if (p && record >= first && record < first + 24 * 0x30 && (record - first) % 0x30 == 0) return true;
    }
    return false;
}

// shell_reload.cpp: one shell per fill - this record's capacity read as at most the cap meanwhile.
std::atomic<uintptr_t> g_capRecord{ 0 };
std::atomic<int> g_capValue{ 0 };

// "Balle dans la chambre (+1)" (user, 2026-11-04: "a weapon of 10 emptied and reloaded gets 10; one round fired, one is
// in the chamber, so reloading gives 11 - for every automatic weapon, the partner too"): the magazine-fed weapons with a
// closed bolt. A players' record of one of them holding a round when its reload fills (shell_reload.cpp's step hook:
// the fill reads the capacity, VA 0xC728C0) has its capacity read one higher meanwhile, and its reload is allowed up to
// that (HookCanReload).
std::atomic<bool> g_chamberOn{ false };
std::atomic<uintptr_t> g_chamberRecord{ 0 };  // the record being filled with its chambered round

bool ChamberWeapon(uint16_t item) {
    switch (item) {
    case 0x0102: case 0x0110: case 0x0111: case 0x011E: case 0x011F: case 0x0129: case 0x0112:  // handguns
    case 0x011A:                                                                                 // Lightning Hawk
    case 0x0103: case 0x0113: case 0x0109:                                                       // submachine guns
    case 0x011D:                                                                                 // AK-74
    case 0x011C: case 0x0120:                                                                    // PSG1, Dragunov
        return true;
    default:
        return false;
    }
}

// 1 when the record holds a round of a chambered weapon (its reload may give one more), else 0.
int ChamberBonus(uintptr_t record) {
    uint32_t item = 0;
    int32_t count = 0;
    if (!g_chamberOn.load(std::memory_order_relaxed) || !record || !PlayerRecord(record) || !mem::Read(record + 4, item) ||
        !ChamberWeapon(static_cast<uint16_t>(item)) || !mem::Read(record + 8, count))
        return 0;
    return count >= 1 ? 1 : 0;
}

int ScaledCapacity(uintptr_t record, int game) {
    if (!g_capacityOn.load(std::memory_order_relaxed) || game <= 0) return game;
    uint32_t item = 0;
    if (!PlayerRecord(record) || !mem::Read(record + 4, item) || !InTable(static_cast<uint16_t>(item))) return game;
    const float rate = Rate(g_capacity, static_cast<uint16_t>(item));
    return rate == 1.0f ? game : std::max(1, static_cast<int>(static_cast<float>(game) * rate + 0.5f));
}

// Mercenaries (user, 2026-11-09: No Mercy's Ithaca reloaded shell by shell to 6 of its 20): the mode sets its weapons' magazine in
// the record's max +0xC (the Ithaca 20, its capacity level +0x23 0x0B), the getter reads the upgrade table (6). The record's max,
// through the weapon table, when it is bigger (in the story they are the same).
int RecordCapacity(uintptr_t record, int getter) {
    int32_t max = 0;
    if (!record || !PlayerRecord(record) || !mem::Read(record + 0xC, max) || max <= 0) return getter;
    return std::max(getter, ScaledCapacity(record, max));
}

int __fastcall HookCapacity(void* manager, void* edx, uintptr_t record) {
    int capacity = ScaledCapacity(record, g_capacityGame(manager, edx, record));
    if (record && record == g_chamberRecord.load(std::memory_order_relaxed)) capacity += 1;
    if (record && record == g_capRecord.load(std::memory_order_relaxed))  // a shell reload's push: up to its cap, past the table's
        return std::min(std::max(capacity, RecordCapacity(record, capacity)), g_capValue.load(std::memory_order_relaxed));
    return capacity;
}

// Reload allowed (cItemManager RVA 0x84DE50, thiscall(record) ret 4; read trace on the record's max, 2026-10-27): its
// last test is the count +8 against the record's max +0xC, the native magazine (user: no reload while the bigger
// magazine held more than the game's). During the call the max is the multiplied magazine, then put back; one more
// with a round in the chamber.
using CanReloadFn = bool(__fastcall*)(void* manager, void* edx, uintptr_t record);
CanReloadFn g_canReloadGame = nullptr;

bool __fastcall HookCanReload(void* manager, void* edx, uintptr_t record) {
    int32_t max = 0;
    if ((!g_capacityOn.load(std::memory_order_relaxed) && !g_chamberOn.load(std::memory_order_relaxed)) ||
        !PlayerRecord(record) || !mem::Read(record + 0xC, max) || max <= 0)
        return g_canReloadGame(manager, edx, record);
    const int bigger = HookCapacity(manager, edx, record) + ChamberBonus(record);
    if (bigger <= max) return g_canReloadGame(manager, edx, record);
    mem::Write<int32_t>(record + 0xC, bigger);
    const bool result = g_canReloadGame(manager, edx, record);
    mem::Write<int32_t>(record + 0xC, max);
    return result;
}

bool HookGame(uintptr_t rva, const uint8_t* expected, size_t size, void* detour, void** original, const char* what) {
    uint8_t* target = mem::Rva(rva);
    if (!mem::IsGameCode(target, size) || std::memcmp(target, expected, size) != 0) {
        LOG("weapon capacity: unexpected bytes for the %s %s", what, mem::Hex(target, size).c_str());
        return false;
    }
    mem::EnsureExecutable(target, size);
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    LOG("weapon capacity: %s hook installed at %p", what, target);
    return true;
}

bool EnsureCapacityHook() {
    static bool hooked = false;
    if (hooked) return true;
    const uint8_t getter[] = { 0x8B, 0x4C, 0x24, 0x04, 0x85, 0xC9, 0x75, 0x05, 0x33, 0xC0 };
    const uint8_t canReload[] = { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x46, 0x04, 0x57, 0x8B, 0xF9 };
    hooked = HookGame(0x84C050, getter, sizeof(getter), reinterpret_cast<void*>(&HookCapacity),
                      reinterpret_cast<void**>(&g_capacityGame), "getter") &&
             HookGame(0x84DE50, canReload, sizeof(canReload), reinterpret_cast<void*>(&HookCanReload),
                      reinterpret_cast<void**>(&g_canReloadGame), "can-reload");
    return hooked;
}

using IntervalFn = float(__cdecl*)(uint32_t item, uint32_t rapid);
IntervalFn g_interval = nullptr;

float __cdecl FireInterval(uint32_t item, uint32_t rapid, uintptr_t action, uintptr_t from) {
    const float game = g_interval(item, rapid);
    if (from != g_fireReturn[0] || !action) return game;
    uintptr_t player = 0;
    const int index = mem::Read(action + 0x18, player) && player ? actors::PlayerIndex(player) : -1;
    if (index < 0) return game;
    float interval = game;
    if (g_fireOn.load(std::memory_order_relaxed) && rapid == 0) interval /= Rate(g_fire, static_cast<uint16_t>(item));
    const float speed = index == 0 ? Player1TimeSpeed() : WorldTimeSpeed();
    if (speed > 0.01f && speed < 0.999f) interval /= speed;  // its timer counts real time
    return interval;
}

// cdecl(item, rapid) entered from the fire action: the action is the caller's esi; the return address tells the two
// calls apart.
__declspec(naked) void FireIntervalStub() {
    __asm {
        push dword ptr [esp]         // from
        push esi                     // action
        push dword ptr [esp + 0x10]  // rapid
        push dword ptr [esp + 0x10]  // item
        call FireInterval
        add esp, 0x10
        ret
    }
}

bool EnsureHook() {
    static bool hooked = false;
    if (hooked) return true;
    for (int i = 0; i < 2; ++i) {
        const uintptr_t call = reinterpret_cast<uintptr_t>(mem::Rva(i == 0 ? 0x88A013 : 0x88A984));
        uint8_t op = 0;
        int32_t rel = 0;
        if (!mem::Read(call, op) || op != 0xE8 || !mem::Read(call + 1, rel) ||
            call + 5 + rel != reinterpret_cast<uintptr_t>(mem::Rva(0x8708A0))) {
            LOG("fire rate: unexpected call site %s", mem::Hex(reinterpret_cast<void*>(call), 5).c_str());
            return false;
        }
        g_fireReturn[i] = call + 5;
    }
    uint8_t* target = mem::Rva(0x8708A0);
    const uint8_t expected[] = { 0x8B, 0x44, 0x24, 0x04, 0x3D, 0x50, 0x01, 0x00, 0x00 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("fire rate: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&FireIntervalStub), reinterpret_cast<void**>(&g_interval));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("fire rate: fire interval hook installed at %p", target);
    return true;
}

}  // namespace

// time_speed.cpp: the fire interval follows the time speed (installed when the game slows down).
bool EnsureFireIntervalHook() { return EnsureHook(); }

// shell_reload.cpp: the shells a push puts in for this weapon (the table's column, 1 to 20).
int ShellsPerPush(uint16_t item) { return static_cast<int>(Rate(g_shells, item) + 0.5f); }

namespace {
// The HUD (uIdCockpit00): VA 0x9EDA80 (thiscall(cockpit, arg) ret 4 - hooked first without its argument, the game
// crashed at once, 2026-11-04; every frame) fills a block of 0x54 per player at +0xEC (its
// player +0x1A0, the partner +0x1A4): +0x18 the weapon, +0x1C and +0x28 its count (VA 0xC4C030: the record's +8), +0x2C
// its capacity (VA 0xC4C050). The number is drawn from +0x28 (VA 0x9F1E90: three digit elements, green when it equals
// +0x2C, grey otherwise; read traces of 2026-11-04). With the extra chambered round player 1's block shows the
// magazine (count - 1, the capacity: user, "it must stay at 300 and write +1"), and the overlay draws "+1".
using GaugesFn = void(__fastcall*)(void* cockpit, void* edx, uintptr_t arg);
GaugesFn g_gaugesGame = nullptr;
std::atomic<DWORD> g_chamberShownTick[2] = {};  // the HUD showed player 1's / the partner's extra round then (the "+1")

// The record the HUD shows (VA 0x9EDC1C: cItemManager VA 0xC4B700(0xE), the equipped weapon - 0xF when that one's
// +0x18 is 9), not the unit in hand (user, 2026-11-04: "aiming the knife, 10 +1 becomes 11": the knife in hand).
uintptr_t ShownRecordOf(uintptr_t p) {
    using RecordFn = uintptr_t(__fastcall*)(void* manager, void* edx, int key);
    if (!p) return 0;
    __try {
        const auto get = reinterpret_cast<RecordFn>(mem::Rva(0x84B700));
        uintptr_t record = get(reinterpret_cast<void*>(p + 0x21A0), nullptr, 0xE);
        uint32_t kind = 0;
        if (record && mem::Read(record + 0x18, kind) && kind == 9) record = get(reinterpret_cast<void*>(p + 0x21A0), nullptr, 0xF);
        return record;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The partner's gauge too (user, 2026-11-06: "Sheva's VZ61 at 301 on the HUD instead of 300 +1"): player 1's cockpit
// (its +0x1A0 player 1) has the partner's block at +0x140 (its player +0x1A4; read live: VZ61 0x103, count, 300). The
// partner's own cockpit (+0x1A0 the partner, its block frozen in solo) is left alone.
void __fastcall HookGauges(void* cockpit, void* edx, uintptr_t arg) {
    g_gaugesGame(cockpit, edx, arg);
    if (!g_chamberOn.load(std::memory_order_relaxed)) return;
    const uintptr_t c = reinterpret_cast<uintptr_t>(cockpit);
    uintptr_t own = 0;
    if (!mem::Read(c + 0x1A0, own) || !own || own != actors::Player(0)) return;
    for (int k = 0; k < 2; ++k) {
        uintptr_t player = 0;
        if (!mem::Read(c + 0x1A0 + 4 * k, player) || !player || player != actors::Player(k)) continue;
        const uintptr_t block = c + 0xEC + 0x54 * k, record = ShownRecordOf(player);
        int32_t count = 0, capacity = 0;
        if (!record || !ChamberBonus(record) || !mem::Read(block + 0x28, count) || !mem::Read(block + 0x2C, capacity) ||
            capacity <= 0 || count <= capacity)
            continue;
        mem::Write<int32_t>(block + 0x28, count - 1);
        mem::Write<int32_t>(block + 0x1C, count - 1);
        const DWORD now = GetTickCount();
        g_chamberShownTick[k] = now ? now : 1;
    }
}

bool EnsureGaugesHook() {
    static bool hooked = false;
    if (hooked) return true;
    // sub esp, 10h; push ebx; push ebp; push esi; push edi; mov edi, ecx
    const uint8_t expected[] = { 0x83, 0xEC, 0x10, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9 };
    hooked = HookGame(0x5EDA80, expected, sizeof(expected), reinterpret_cast<void*>(&HookGauges),
                      reinterpret_cast<void**>(&g_gaugesGame), "HUD gauges");
    return hooked;
}
}  // namespace

// The overlay: player 1's HUD shows his (index 0) or the partner's (1) magazine with its extra chambered round (updated
// in the last 200 ms).
bool ChamberPlusOneShown(int index) {
    if (index < 0 || index > 1) return false;
    const DWORD tick = g_chamberShownTick[index].load(std::memory_order_relaxed);
    return g_chamberOn.load(std::memory_order_relaxed) && tick && GetTickCount() - tick < 200;
}

// shell_reload.cpp's reload step hook, around the game's fill of player p's held record: its chambered round.
bool ChamberFillBegin(uintptr_t record) {
    if (!ChamberBonus(record)) return false;
    g_chamberRecord = record;
    return true;
}
void ChamberFillEnd() { g_chamberRecord = 0; }

// shell_reload.cpp: the capacity getter hooked, and a record's capacity read as at most `cap` (record 0: none).
bool EnsureMagazineCapHook() { return EnsureCapacityHook(); }
void SetMagazineCap(uintptr_t record, int cap) {
    g_capValue = cap;
    g_capRecord = record;
}
// shell_reload.cpp: a record's magazine as the shell reload fills it (the getter's, or the record's max when bigger: Mercenaries).
int MagazineOfRecord(uintptr_t record, int getter) { return RecordCapacity(record, getter); }

namespace {
// The layer speeds while a player fires or reloads, his and his held weapon's (user, 2026-11-04: with the S75 fired
// faster the rifle stayed in the right hand while the right hand worked the bolt - the left hand's hold and the rifle's
// own motion (the weapon unit's layers, its bolt) played at the game's pace while only Chris's layer 0 was sped up; the
// fire now speeds layers 0-6 as the reload, and both speed the weapon's layers too). Per layer the game's value and
// what the trainer wrote over it.
struct LayerSpeeds {
    uintptr_t unit = 0;
    float base[8] = {}, written[8] = {};
};
struct ActionSpeeds {
    LayerSpeeds player, weapon;
};
ActionSpeeds g_fireSpeed[2], g_reloadSpeed[2];

uintptr_t LayerSpeed(uintptr_t unit, int layer) { return unit + 0x380 + layer * 0xE0 + 0x24; }

void RestoreLayers(LayerSpeeds& l, bool alive) {
    if (l.unit && alive)
        for (int k = 0; k < kSpedLayers; ++k) {
            float speed = 0;
            if (mem::Read(LayerSpeed(l.unit, k), speed) && speed == l.written[k]) mem::Write(LayerSpeed(l.unit, k), l.base[k]);
        }
    l.unit = 0;
}

void SpeedLayers(LayerSpeeds& l, uintptr_t unit, float rate) {
    if (l.unit != unit) {
        l.unit = unit;
        for (int k = 0; k < kSpedLayers; ++k) l.written[k] = -1.0f;  // nothing written yet: every value is the game's
    }
    for (int k = 0; k < kSpedLayers; ++k) {
        float speed = 0;
        if (!mem::Read(LayerSpeed(unit, k), speed)) continue;
        if (speed != l.written[k]) {  // a new motion: the game's own speed
            if (speed < 0.0f || speed > 3.0f) continue;
            l.base[k] = speed;
        }
        l.written[k] = l.base[k] * rate;
        mem::Write(LayerSpeed(unit, k), l.written[k]);
    }
}

void RestoreActionSpeeds(ActionSpeeds& a, int i) {
    const uintptr_t p = actors::Player(i);
    uintptr_t held = 0;
    if (p) mem::Read(p + 0x2634, held);
    RestoreLayers(a.weapon, a.weapon.unit && a.weapon.unit == held);
    RestoreLayers(a.player, a.player.unit && a.player.unit == p);
}

void RestoreFireSpeed() {
    for (int i = 0; i < 2; ++i) RestoreActionSpeeds(g_fireSpeed[i], i);
}

void RestoreReloadSpeed() {
    for (int i = 0; i < 2; ++i) RestoreActionSpeeds(g_reloadSpeed[i], i);
}

// While player i is in `action` with a held weapon whose slider is not x1: his layers and the weapon's at its pace.
// `weaponAction`: another action that counts only with a weapon of the table in hand (the knife aim's shot, below).
void TickActionSpeed(ActionSpeeds& a, const Table& table, uint32_t action, int i, uint32_t weaponAction = 0xFFFFFFFF) {
    const uintptr_t p = actors::InGame() ? actors::Player(i) : 0;
    uint32_t now = 0;
    uintptr_t held = 0;
    const uint16_t item = p ? PlayerHeldItem(p) : 0;
    const float rate = p && mem::Read(p + 0x10E0, now) && (now == action || (now == weaponAction && InTable(item)))
                           ? Rate(table, item)
                           : 1.0f;
    if (std::fabs(rate - 1.0f) < 0.001f) {
        RestoreActionSpeeds(a, i);
        return;
    }
    SpeedLayers(a.player, p, rate);
    if (mem::Read(p + 0x2634, held) && held) {
        uintptr_t parent = 0;
        if (a.weapon.unit && a.weapon.unit != held) RestoreLayers(a.weapon, false);  // another weapon now
        if (mem::Read(held + 0x18, parent) && parent == p) SpeedLayers(a.weapon, held, rate);
    } else {
        RestoreLayers(a.weapon, false);
    }
}

// A firearm of the knife slot fired in the knife aim plays its shot (and a shotgun's pump) in knife_attack 0x27 (user,
// 2026-11-05: its fire interval followed the slider, not its motions: the pump could be skipped).
constexpr uint32_t kKnifeAttack = 0x27;

void TickFire() {
    for (int i = 0; i < 2; ++i) TickActionSpeed(g_fireSpeed[i], g_fire, kFire, i, kKnifeAttack);
}

void TickReload() {
    for (int i = 0; i < 2; ++i) TickActionSpeed(g_reloadSpeed[i], g_reload, kReload, i);
}

std::wstring TablePath(const Table& table) { return logx::DataDir() + L"\\" + table.file; }

void Save(const Table& table) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, TablePath(table).c_str(), L"w") || !file) return;
    for (uint16_t item : kWeapons)
        if (const float rate = Rate(table, item); rate != DefaultRate(table, item)) std::fprintf(file, "%04X=%.2f\n", item, rate);
    std::fclose(file);
}

void Load(Table& table) {
    for (std::atomic<float>& rate : table.values) rate.store(table.fallback, std::memory_order_relaxed);
    for (uint16_t item : kWeapons) table.values[item & 0xFF].store(DefaultRate(table, item), std::memory_order_relaxed);
    FILE* file = nullptr;
    if (_wfopen_s(&file, TablePath(table).c_str(), L"r") || !file) return;
    unsigned item = 0;
    float rate = 1.0f;
    while (fscanf_s(file, "%x=%f\n", &item, &rate) == 2)
        if ((item & 0xFF00) == 0x0100 && (item & 0xFF) < 0x40 && rate >= table.min && rate <= table.max)
            table.values[item & 0xFF].store(rate, std::memory_order_relaxed);
    std::fclose(file);
}

Table* const kAllTables[] = { &g_fire,           &g_reload,         &g_power,          &g_critical,
                              &g_capacity,       &g_shells,         &g_knifeCritical,  &g_knifeRange,
                              &g_knifeStagger[0], &g_knifeStagger[1], &g_knifeStagger[2], &g_knifeStagger[3],
                              &g_knifePlaga,     &g_knifeResistance, &g_knifePrice };

// An option of settings.ini as it was ([enabled] "id=1", [values] "id=value"): the knife options of the Couteau tab
// moved into the table (2026-11-05) give their values once.
bool OldOption(const char* id, bool& on, float& value) {
    std::ifstream file(logx::DataDir() + L"\\settings.ini");
    std::string line, section;
    const std::string key = std::string(id) + "=";
    bool found = false;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '[') {
            section = line;
            continue;
        }
        if (line.compare(0, key.size(), key) != 0) continue;
        const std::string text = line.substr(key.size());
        if (section == "[enabled]") {
            on = text == "1";
            found = true;
        } else if (section == "[values]") {
            value = std::strtof(text.c_str(), nullptr);
            found = true;
        }
    }
    return found;
}

// Once (no knife_resistance.ini yet): the knives' rows from the options they replace - "Puissance du couteau" (all four,
// player 1's) times each "Puissance du couteau de X", "Durabilité du couteau de X" (else "Durabilité du couteau"'s
// units), "Couteau : coups critiques à 100 %", "Portée du couteau", "Couteau : faire vaciller (zone)", "Parasite : vacille
// au couteau"; an option unchecked gives the game's value.
void MoveKnifeOptions() {
    if (GetFileAttributesW(TablePath(g_knifeResistance).c_str()) != INVALID_FILE_ATTRIBUTES) return;
    static constexpr float kTypePower[4] = { 0.8f, 1.2f, 1.0f, 1.0f }, kTypeUnits[4] = { 120.0f, 50.0f, 100.0f, 100.0f };
    static const char* const kStaggerIds[4] = { "combat.knife_stagger", "combat.knife_stagger_arms", "combat.knife_stagger_legs",
                                                "combat.knife_stagger_body" };
    bool on = false;
    float value = 3.0f;
    const float global = OldOption("combat.knife_power", on, value) && on ? value : 1.0f;
    on = false;
    value = 100.0f;
    OldOption("combat.knife_durability", on, value);
    const float units = value;
    on = false;
    const float critical = OldOption("combat.critical_knife", on, value) && on ? 100.0f : 0.0f;
    on = false;
    value = 2.0f;
    const float range = OldOption("combat.knife_range", on, value) && on ? value : 1.0f;
    float stagger[4];
    for (int r = 0; r < 4; ++r) {
        on = false;
        value = 100.0f;
        stagger[r] = OldOption(kStaggerIds[r], on, value) && on ? value : 0.0f;
    }
    on = false;
    value = 100.0f;
    const float plaga = OldOption("combat.plaga_flash_knife", on, value) && on ? value : 0.0f;
    for (int k = 0; k < 4; ++k) {
        const unsigned low = kKnives[k] & 0xFF;
        char id[64];
        std::snprintf(id, sizeof(id), "combat.knife_type_power_%04x", kKnives[k]);
        on = true;  // those options were checked by default
        value = kTypePower[k];
        OldOption(id, on, value);
        g_power.values[low].store(std::clamp(global * (on ? value : 1.0f), g_power.min, g_power.max), std::memory_order_relaxed);
        std::snprintf(id, sizeof(id), "combat.knife_type_durability_%04x", kKnives[k]);
        on = true;
        value = kTypeUnits[k];
        OldOption(id, on, value);
        g_knifeResistance.values[low].store(std::clamp(on ? value : units, g_knifeResistance.min, g_knifeResistance.max),
                                            std::memory_order_relaxed);
        g_knifeCritical.values[low].store(critical, std::memory_order_relaxed);
        g_knifeRange.values[low].store(std::clamp(range, g_knifeRange.min, g_knifeRange.max), std::memory_order_relaxed);
        for (int r = 0; r < 4; ++r)
            g_knifeStagger[r].values[low].store(std::clamp(stagger[r], 0.0f, 1000.0f), std::memory_order_relaxed);
        g_knifePlaga.values[low].store(std::clamp(plaga, 0.0f, 100.0f), std::memory_order_relaxed);
    }
    for (Table* table : kAllTables) Save(*table);
    LOG("weapon table: the knife options moved in (power x%.2f, critical %.0f %%, range x%.2f, stagger %.0f / %.0f / %.0f / "
        "%.0f %%, plaga %.0f %%)", global, critical, range, stagger[0], stagger[1], stagger[2], stagger[3], plaga);
}

// A slider of `table` for `item`, saved once the edit is over; logarithmic with x1 / 100 % in its middle part.
void SliderCell(Table& table, uint16_t item, const char* id, float width, const char* format, bool logarithmic) {
    float rate = Rate(table, item);
    ImGui::SetNextItemWidth(width);
    if (widgets::SliderFloat(id, &rate, table.min, table.max, format, logarithmic ? ImGuiSliderFlags_Logarithmic : 0))
        table.values[item & 0xFF].store(rate, std::memory_order_relaxed);
    if (ImGui::IsItemDeactivatedAfterEdit()) Save(table);
}

const char* KnifeRowName(int k) {
    switch (k) {
    case 0: return i18n::Tr("Couteau de Chris");
    case 1: return i18n::Tr("Couteau de Sheva");
    case 2: return i18n::Tr("Couteau de Chris (variante)");
    default: return i18n::Tr("Couteau de Sheva (variante)");
    }
}

void DrawRateTable() {
    const float scale = ImGui::GetFontSize() / 13.0f;
    ImGui::TextUnformatted(i18n::Tr("Par arme (joueurs 1 et 2 ; chaque colonne agit quand son option est cochée ci-dessus) :"));
    ImGui::SameLine();
    if (ImGui::SmallButton(i18n::Tr("Tout comme le jeu"))) {
        for (Table* table : kAllTables) {
            for (std::atomic<float>& rate : table->values) rate.store(table->fallback, std::memory_order_relaxed);
            if (table == &g_knifeResistance)  // a knife's units stay its own (the game has none)
                for (uint16_t item : kKnives)
                    table->values[item & 0xFF].store(DefaultRate(*table, item), std::memory_order_relaxed);
            Save(*table);
        }
    }
    static const char* const kColumns[] = { "Cadence",           "Rechargement",       "Puissance",          "Coup critique",
                                            "Munitions",         "Cartouches par geste", "Portée",           "Vacillement tête",
                                            "Vacillement bras",  "Vacillement jambes", "Vacillement corps",  "Parasite",
                                            "Résistance",        "Prix de vente" };
    if (ImGui::BeginTable("fire_rate", 1 + static_cast<int>(std::size(kColumns)),
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                              ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0, 300 * scale))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn(i18n::Tr("Arme"));
        for (const char* column : kColumns) ImGui::TableSetupColumn(i18n::Tr(column));
        ImGui::TableHeadersRow();
        const float width = 120 * scale;
        for (uint16_t item : kWeapons) {
            const int knife = KnifeRow(item);
            ImGui::PushID(item);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(knife >= 0 ? KnifeRowName(knife) : ItemDisplayName(item).c_str());
            const auto none = [] { ImGui::TextDisabled("-"); };
            for (int c = 0; c < static_cast<int>(std::size(kColumns)); ++c) {
                ImGui::TableSetColumnIndex(c + 1);
                switch (c) {
                case 0: SliderCell(g_fire, item, "##rate", width, "x%.2f", true); break;
                case 1: knife >= 0 ? none() : SliderCell(g_reload, item, "##reload", width, "x%.2f", true); break;
                case 2: SliderCell(g_power, item, "##power", width, "x%.2f", true); break;
                case 3:
                    // x0 = never: a logarithmic slider cannot reach 0. The knives': a chance (their own is 0).
                    if (knife >= 0) SliderCell(g_knifeCritical, item, "##kcritical", width, "%.0f %%", false);
                    else SliderCell(g_critical, item, "##critical", width, "x%.2f", false);
                    break;
                case 4: knife >= 0 ? none() : SliderCell(g_capacity, item, "##capacity", width, "x%.2f", true); break;
                case 5: {
                    if (!ShellReloadWeapon(item)) {
                        none();
                        break;
                    }
                    float shells = Rate(g_shells, item);
                    ImGui::SetNextItemWidth(width);
                    if (widgets::SliderFloat("##shells", &shells, g_shells.min, g_shells.max, "%.0f"))
                        g_shells.values[item & 0xFF].store(std::floor(shells + 0.5f), std::memory_order_relaxed);
                    if (ImGui::IsItemDeactivatedAfterEdit()) Save(g_shells);
                    break;
                }
                case 6: knife < 0 ? none() : SliderCell(g_knifeRange, item, "##range", width, "x%.2f", true); break;
                case 7: case 8: case 9: case 10: {
                    static const char* const kIds[4] = { "##head", "##arms", "##legs", "##body" };
                    knife < 0 ? none() : SliderCell(g_knifeStagger[c - 7], item, kIds[c - 7], width, "%.0f %%", true);
                    break;
                }
                case 11: knife < 0 ? none() : SliderCell(g_knifePlaga, item, "##plaga", width, "%.0f %%", false); break;
                case 12: knife < 0 ? none() : SliderCell(g_knifeResistance, item, "##units", width, "%.0f", true); break;
                default: knife < 0 ? none() : SliderCell(g_knifePrice, item, "##price", width, "%.0f", true); break;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled(i18n::Tr("x1 = l'arme telle qu'elle est en jeu, améliorations comprises (un 92F amélioré à 200 de puissance : "
                        "x1 = 200, x1.5 = 300) ; x2 = deux fois plus (vite, fort, de chances de critique, de balles). "
                        "« Cartouches par geste » : avec « Rechargement cartouche par cartouche », les cartouches mises à "
                        "chaque geste (jamais plus qu'il n'en manque). Clic simple sur un curseur pour taper la valeur."));
    ImGui::TextDisabled(i18n::Tr("Couteaux (le couteau en main, sinon celui de l'étui ; joueurs 1 et 2) : « Cadence » accélère "
                        "ses coups, « Puissance » multiplie ses dégâts, « Coup critique » est la chance qu'un coup dans la tête "
                        "d'un Majini soit critique (la tête explose). « Portée » agrandit la zone qui touche. « Vacillement » "
                        "est la chance qu'un coup dans cette zone fasse réagir un Majini ordinaire comme une balle (tête : il "
                        "vacille ; jambes : il peut tomber à genoux ; au-delà de 100 % : à chaque coup). « Parasite » est la "
                        "chance qu'un coup sur le parasite sorti d'un Majini le fasse vaciller (« head flash »). "
                        "« Résistance » : ses unités neuf avec « Durabilité du couteau » (onglet Couteau). « Prix de "
                        "vente » : ce que sa vente rapporte dans l'Organiser (rouvrir l'Organiser pour le voir changer). "
                        "Portée, vacillement, parasite, résistance et prix agissent sans option à cocher."));
}

// The weapon table in a tab of its own with the options of its columns (user, 2026-11-05: "out of Munitions, its own tab
// Réglages des armes"); Munitions keeps the rest.
void DrawWeaponPanel() {
    DrawOptions("Réglages des armes");
    ImGui::Separator();
    DrawRateTable();
}

void RestoreSpeeds() {
    RestoreFireSpeed();
    RestoreReloadSpeed();
}
}  // namespace

// knife.cpp: a player's power and critical multipliers for the weapon in hand (1 = the game's, also with their options
// off).
float WeaponPower(uint16_t item) {
    return g_powerOn.load(std::memory_order_relaxed) && InTable(item) ? Rate(g_power, item) : 1.0f;
}
float WeaponCritical(uint16_t item) {
    return g_criticalOn.load(std::memory_order_relaxed) && InTable(item) ? Rate(g_critical, item) : 1.0f;
}
// knife.cpp / knife_icon.cpp: the knives' rows (the knife a hit was made with; anything else: the game's).
float KnifePower(uint16_t item) {
    return g_powerOn.load(std::memory_order_relaxed) && KnifeRow(item) >= 0 ? Rate(g_power, item) : 1.0f;
}
float KnifeCriticalChance(uint16_t item) {
    return g_criticalOn.load(std::memory_order_relaxed) && KnifeRow(item) >= 0 ? Rate(g_knifeCritical, item) : 0.0f;
}
float KnifeRange(uint16_t item) { return KnifeRow(item) >= 0 ? Rate(g_knifeRange, item) : 1.0f; }
float KnifeStaggerChance(uint16_t item, int region) {
    return KnifeRow(item) >= 0 && region >= 0 && region < 4 ? Rate(g_knifeStagger[region], item) : 0.0f;
}
float KnifePlagaChance(uint16_t item) { return KnifeRow(item) >= 0 ? Rate(g_knifePlaga, item) : 0.0f; }
float KnifeResistance(uint16_t item) { return KnifeRow(item) >= 0 ? Rate(g_knifeResistance, item) : g_knifeResistance.fallback; }
bool AnyKnife(const Table& table, float neutral) {
    for (uint16_t item : kKnives)
        if (Rate(table, item) != neutral) return true;
    return false;
}
bool AnyKnifeStagger() {
    for (const Table& table : g_knifeStagger)
        if (AnyKnife(table, 0.0f)) return true;
    return false;
}
bool AnyKnifePlaga() { return AnyKnife(g_knifePlaga, 0.0f); }
bool AnyKnifeRange() { return AnyKnife(g_knifeRange, 1.0f); }

// The knives' sale price (user, 2026-11-05: "they sell for 0"): the items' base price VA 0xA356B0 (cdecl(id): the table
// VA 0x112C4E8, 12-byte entries u16 id / price, 100 items, no knife) answers ten times the knife row's "Prix de vente"
// - a sale gives a tenth of it (the Organiser's cell fill at VA 0xA35B5C: base / 10 + upgrades / 4). Its 9 callers
// are the Organiser's and the shops' screens.
using PriceFn = int(__cdecl*)(int id);
PriceFn g_price = nullptr;

int __cdecl HookItemPrice(int id) {
    if (KnifeRow(static_cast<uint16_t>(id)) >= 0)
        if (const float sale = Rate(g_knifePrice, static_cast<uint16_t>(id)); sale > 0.0f)
            return static_cast<int>(sale * 10.0f + 0.5f);
    return g_price(id);
}

bool EnsurePriceHook() {
    static bool hooked = false;
    if (hooked) return true;
    const uint8_t expected[] = { 0x8B, 0x54, 0x24, 0x04, 0x8D, 0x82, 0xDA, 0xFE, 0xFF, 0xFF };
    hooked = HookGame(0x6356B0, expected, sizeof(expected), reinterpret_cast<void*>(&HookItemPrice),
                      reinterpret_cast<void**>(&g_price), "knife price");
    return hooked;
}

// The weapons whose hits are not bullets (grenade launchers, rocket launchers, bow, flamethrower).
bool WeaponHitsNotBullets(uint16_t item) {
    item = SliderItem(item);
    return item == 0x010C || item == 0x010D || item == 0x0135 || item == 0x010F || item == 0x0121;
}

void RegisterFireRate() {
    for (Table* table : kAllTables) Load(*table);
    MoveKnifeOptions();
    OnFrame([] {  // the knives' prices once one is set
        if (AnyKnife(g_knifePrice, 0.0f)) EnsurePriceHook();
    });
    Add({ .id = "ammo.fire_rate", .name = "Cadence de tir par arme",
          .help = "Chaque arme tire plus vite selon son curseur « Cadence » dans le tableau ci-dessous (comme les "
                  "améliorations de cadence de RE4) : le tir suivant part plus tôt et l'animation de tir (pompe comprise) "
                  "va au même rythme. Tes déplacements en tirant ne changent pas. Les couteaux : leurs coups vont plus "
                  "vite. Joueurs 1 et 2 ; sans effet sur une arme en « tir rapide ».",
          .category = "Réglages des armes", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureHook()) return false;
              g_fireOn = on;
              if (!on) RestoreFireSpeed();
              return true;
          },
          .tick = TickFire,
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "ammo.reload_speed", .name = "Vitesse de rechargement par arme",
          .help = "Chaque arme recharge plus vite selon son curseur « Rechargement » dans le tableau ci-dessous, en plus de "
                  "l'amélioration de rechargement du jeu : toute l'animation de rechargement va plus vite et les balles "
                  "arrivent plus tôt. Tes déplacements en rechargeant ne changent pas. Joueurs 1 et 2.",
          .category = "Réglages des armes", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_reloadOn = on;
              if (!on) RestoreReloadSpeed();
              return true;
          },
          .tick = TickReload,
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "ammo.weapon_power", .name = "Puissance par arme",
          .help = "Multiplie les dégâts de chaque arme, tels qu'ils sont avec ses améliorations, selon son curseur "
                  "« Puissance » dans le tableau ci-dessous : balles, plombs, et pour les lance-grenades, lance-roquettes, "
                  "l'arc et le lance-flammes tout ce qui touche pendant qu'on les tient ; chaque couteau ses coups (pas "
                  "les coups au corps à corps). Joueurs 1 et 2.",
          .category = "Réglages des armes", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureEnemyDamageHook()) return false;
              g_powerOn = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "ammo.weapon_critical", .name = "Coups critiques par arme",
          .help = "Multiplie la chance de coup critique (la tête qui explose) de chaque arme, telle qu'elle est avec ses "
                  "améliorations, selon son curseur « Coup critique » dans le tableau ci-dessous : x0 = jamais, x2 = deux "
                  "fois plus de chances. Une arme sans critique dans le jeu n'en a pas plus. Passe avant « Armes à feu : "
                  "coups critiques à 100 % » pour une arme réglée autrement qu'à x1. Les couteaux (qui n'en ont pas dans "
                  "le jeu) : la chance en %. Ennemis qui ont des coups critiques seulement (Majini...). Joueurs 1 et 2.",
          .category = "Réglages des armes", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureEnemyOnHitHook()) return false;
              g_criticalOn = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "ammo.chamber", .name = "Balle dans la chambre (+1)",
          .help = "Les armes à chargeur (pistolets, Lightning Hawk, mitraillettes, fusils d'assaut, fusils de précision "
                  "semi-automatiques) gardent une balle dans la chambre : recharger une arme qui n'est pas vide donne le "
                  "chargeur plein plus une balle (10 + 1 pour un chargeur de 10). Vide, elle se recharge à 10 comme dans le "
                  "jeu. Joueurs 1 et 2 (le partenaire aussi). Le compteur du joueur 1 affiche « +1 » tant que la balle en plus "
                  "est là.",
          .category = "Munitions", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureCapacityHook() || !EnsureReloadStepHook() || !EnsureGaugesHook())) return false;
              g_chamberOn = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "ammo.weapon_capacity", .name = "Munitions par arme",
          .help = "Multiplie la taille du chargeur de chaque arme, telle qu'elle est avec ses améliorations de capacité, "
                  "selon son curseur « Munitions » dans le tableau ci-dessous : le rechargement remplit le nouveau "
                  "chargeur. Joueurs 1 et 2.",
          .category = "Réglages des armes", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureCapacityHook()) return false;
              g_capacityOn = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    AddPanel("Réglages des armes", DrawWeaponPanel, { "Réglages des armes" });
    OnRestore(RestoreSpeeds);
}
}  // namespace features
