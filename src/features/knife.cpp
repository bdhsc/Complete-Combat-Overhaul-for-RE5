// Knife (user, 2026-10-15): range, power and stopping power of the knife attacks.
// Every hit on an enemy goes through its ApplyDamage, RVA 0x5DEA50 (thiscall(hit info, x) ret 8): the amount comes
// from the hit info (damage calculator [RVA 0xE34578], RVA 0x31B010), +0x49 = body part, +0x3C bit 0x10000000 = the
// whole health, then the enemy's vfunc +0x204(part, amount) and SubHealth (vfunc +0x114).
// diag.hit_log writes each hit's info with what player 1 is doing and the weapon in his hand. Compared on Majini
// (2026-10-15): power +0x28 / +0x2C (int) pistol 250-300, shotgun 400, melee 400-800, knife 50-60; +0x38 (float)
// pistol 3-4, shotgun 2, melee 1, knife 0; +0x34 pistol 0.1, melee 0.5, knife 0.08; +0x40 pistol 0x200, knife 0;
// +0x3C flags: knife 0x00400810 (guns 0x0008000x, melee 0x00800090); +0x20 the source, the attacker is resolved by
// RVA 0x849530 (thiscall(hit)). The knife of player 1 = attacker player 1 and flag 0x00400000.
// "Puissance du couteau" multiplies +0x28 / +0x2C and the amount. Stopping power, what did not work (user tests,
// 2026-10-15): +0x38 = 10 (it is the critical value: pistol 4 blew every head up, knife 0), 5x the damage, the
// pistol's reaction bits +0x40 0x200 / +0x34 0.1 (even before OnHit), asking the DmgHead action, the weapon kind and
// the stagger gauge. What does: the "light hit" bit, see KnifeStagger. Part = hit +0x49 (index of the enemy's hit shape), its kind = shape list +0x1290: +0x30
// array / +0x34 count (game lookup RVA 0x844150); Majini (uEm10 / 11 / 21, same table): head kind 2, body 1 / 6,
// arms 3 / 4 / 5, legs 7 / 8 / 9 / 0xA.
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "features/melee_tuning.hpp"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace features {
bool ParryIgnoresHit(uintptr_t enemy, void* hit);  // parry.cpp
bool GrabKeepsHolder(uintptr_t enemy, void* hit);  // grabs.cpp
bool QuickShotTarget(uintptr_t enemy);              // quick_knife.cpp: the RE6-style quick shot's enemy
void QuickShotSawHit(uintptr_t enemy);
void RecordBulletDamage(uintptr_t enemy, uint16_t item, int damage, bool head);  // quick_knife.cpp
float WeaponPower(uint16_t item);                // fire_rate.cpp: "Puissance par arme" (1 = the game's)
float WeaponCritical(uint16_t item);             // fire_rate.cpp: "Coups critiques par arme" (1 = the game's)
bool WeaponHitsNotBullets(uint16_t item);        // fire_rate.cpp: launchers, bow, flamethrower
bool MajiniHeadFlash(uintptr_t enemy);             // parry.cpp: DmgHead with a stagger motion (melee prompt)
void KnifeWearHit(int player);                     // knife_icon.cpp: a player's knife hit an enemy
const char* UnitClassName(uintptr_t object);       // parry.cpp: its DTI class name
// fire_rate.cpp: the knives' rows of the weapon table (user, 2026-11-05: "each knife its own power, criticals, range,
// stagger...; no global knife power"; their options of the Couteau tab gone).
float KnifePower(uint16_t item);
float KnifeCriticalChance(uint16_t item);
float KnifeRange(uint16_t item);
float KnifeStaggerChance(uint16_t item, int region);
float KnifePlagaChance(uint16_t item);
bool AnyKnifeStagger();
bool AnyKnifePlaga();
bool AnyKnifeRange();
uint16_t KnifeSlotItem(int playerIndex);           // inventory.cpp: the knife slot's item
bool IsKnifeItem(uint16_t id);                     // inventory.cpp

namespace {

std::atomic<bool> g_hitLog{ false };
enum Region { kHead, kArms, kLegs, kBody, kRegions };

// Player 1's hit of that kind (hit flags +0x3C: knife 0x00400000, bullet 0x00080000).
bool IsPlayer1Hit(void* hit, uint32_t kind) {
    uint32_t flags = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x3C, flags) || !(flags & kind)) return false;
    using AttackerFn = uintptr_t(__fastcall*)(void* hit, void* edx);
    __try {
        const uintptr_t attacker = reinterpret_cast<AttackerFn>(mem::Rva(0x849530))(hit, nullptr);
        return attacker && attacker == actors::Player(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The attacker of a hit with the knife flag (0x00400000), 0 otherwise.
uintptr_t KnifeHitAttacker(void* hit) {
    uint32_t flags = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x3C, flags) || !(flags & 0x00400000)) return 0;
    using AttackerFn = uintptr_t(__fastcall*)(void* hit, void* edx);
    __try {
        return reinterpret_cast<AttackerFn>(mem::Rva(0x849530))(hit, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The player (0 / 1) whose knife made the hit, -1 otherwise ("Durabilité du couteau"). The knife flag alone is not
// enough: a punch or a free melee move with the knife out carries it too (user, 2026-10-27: a melee move after a grab
// begun in the knife aim wore the knife) - the player's action must be a knife one (its DTI class: knife_attack,
// knife_ready_*, attack_knife_flying / _licker / _licker_tongue / _plaga) or the knife's slash in its aim, the weapon's
// fire weapon_fire (action 0x27 there: user, 2026-10-28, the second knife wore no more).
int KnifeHitPlayer(void* hit) {
    const uintptr_t attacker = KnifeHitAttacker(hit);
    const int index = attacker ? actors::PlayerIndex(attacker) : -1;
    uint32_t action = 0, count = 0;
    uintptr_t table = 0, object = 0;
    if (index < 0 || !mem::Read(attacker + 0x10E0, action) || !mem::Read(attacker + 0x10D0, count) || action >= count ||
        count > 0x200 || !mem::Read(attacker + 0x10DC, table) || !table || !mem::Read(table + 4 * action, object) || !object)
        return -1;
    const char* name = UnitClassName(object);
    return name && (std::strstr(name, "knife") || std::strstr(name, "weapon_fire")) ? index : -1;
}

// The weapon in a player's hand when the hit is his weapon's (fire_rate.cpp's power and critical columns, players 1 and
// 2: user, 2026-10-27): a bullet (flag 0x00080000), or any hit but the knife's (0x00400000) and his melee moves'
// (actions 0x30-0x4D, 0xAC / 0xAD) while he holds a weapon whose hits are not bullets; 0 otherwise.
uint16_t HeldItem(uintptr_t player);  // below
uint16_t PlayerWeaponHit(void* hit) {
    uint32_t flags = 0;
    if (!hit || !mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x3C, flags) || (flags & 0x00400000)) return 0;
    using AttackerFn = uintptr_t(__fastcall*)(void* hit, void* edx);
    uintptr_t p = 0;
    __try {
        p = reinterpret_cast<AttackerFn>(mem::Rva(0x849530))(hit, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!p || actors::PlayerIndex(p) < 0) return 0;
    const uint16_t item = HeldItem(p);
    if (flags & 0x00080000) return item;
    uint32_t action = 0;
    if (!WeaponHitsNotBullets(item) || !mem::Read(p + 0x10E0, action) || (action >= 0x30 && action <= 0x4D) ||
        action == 0xAC || action == 0xAD)
        return 0;
    return item;
}

// "Puissance par arme": a player's hit with that weapon, amount (and the hit's power fields, as the knife's) multiplied.
void TuneWeaponHit(void* hit, uint32_t& amount) {
    const uint16_t item = PlayerWeaponHit(hit);
    const float factor = item ? WeaponPower(item) : 1.0f;
    if (factor == 1.0f) return;
    const uintptr_t h = reinterpret_cast<uintptr_t>(hit);
    for (uintptr_t off : { 0x28u, 0x2Cu }) {
        int32_t value = 0;
        if (mem::Read(h + off, value) && value > 0 && value < 100000)
            mem::Write(h + off, std::max(1, static_cast<int32_t>(static_cast<float>(value) * factor + 0.5f)));
    }
    if (amount > 0 && amount < 100000) amount = std::max(1u, static_cast<uint32_t>(static_cast<float>(amount) * factor + 0.5f));
}

// The knife a player's knife hit was made with: the knife in his hand (an inventory knife's record, or the slot's), else
// the slot's item.
uint16_t KnifeOfHit(int player) {
    const uint16_t held = HeldItem(actors::Player(player));
    return held && IsKnifeItem(held) ? held : KnifeSlotItem(player);
}

// The amount ApplyDamage takes off is its second argument, computed by the caller (enemy OnHit RVA 0x5E0840, vfunc
// +0x20C(hit) from +0x28) before the call: multiplying +0x28 alone changed nothing (2026-10-15, knife hits still 45-60).
// The power of the knife's row in the weapon table (fire_rate.cpp, "Puissance": either player's real knife hits; the
// global "Puissance du couteau" of player 1 and the per-knife options before 2026-11-05).
void TuneKnifeHit(void* hit, uint32_t& amount) {
    if (!hit) return;
    const int who = KnifeHitPlayer(hit);
    const float factor = who >= 0 ? KnifePower(KnifeOfHit(who)) : 1.0f;
    if (factor == 1.0f) return;
    const uintptr_t h = reinterpret_cast<uintptr_t>(hit);
    for (uintptr_t off : { 0x28u, 0x2Cu }) {
        int32_t value = 0;
        if (mem::Read(h + off, value) && value > 0 && value < 100000)
            mem::Write(h + off, std::max(1, static_cast<int32_t>(static_cast<float>(value) * factor + 0.5f)));
    }
    if (amount > 0 && amount < 100000) amount = std::max(1u, static_cast<uint32_t>(static_cast<float>(amount) * factor + 0.5f));
}

// Range (user, 2026-10-15): the knife unit (the record's +0x2C of item 0x0101 / 0x010E / 0x0123 / 0x0124) keeps its
// collision shapes like an actor, list at unit +0x1290 (array +4, count +0x18, cHitTree::Node). The attack one carries
// the knife's hit flags at +0x98 (0x00400810, the hit info's +0x3C): a capsule from +0x70 (x, y, z) to +0x80 in the
// hand's frame ((40, 0, -10) to (-80, -10, 60) for uWp24) with radius +0xA8 (10). The "Portée" of the knife's row in
// the weapon table (fire_rate.cpp; "Portée du couteau" for player 1 before 2026-11-05) multiplies both points and the
// radius, both players' knives; the game's values are kept and written back once every knife is at x1. Only shapes
// found in a live knife list that frame are touched.
struct KnifeShape {
    uintptr_t shape;
    float points[6];
    float radius;
    float applied;  // factor written last
};
std::vector<KnifeShape> g_knifeShapes;  // render thread

template <class Fn> void ForEachKnifeAttackShape(Fn fn) {
    if (!actors::InGame()) return;
    for (int player = 0; player < 2; ++player)
    for (int k = 0; k < 24; ++k) {
        const uintptr_t p = actors::Player(player);
        if (!p) break;
        const uintptr_t record = p + 0x21A4 + 0x30 * k;
        uint32_t item = 0;
        uintptr_t unit = 0, array = 0;
        uint32_t count = 0;
        if (!mem::Read(record + 4, item) || !mem::Read(record + 0x2C, unit) || !unit) continue;
        item &= 0xFFFF;
        if (item != 0x0101 && item != 0x010E && item != 0x0123 && item != 0x0124) continue;
        if (!mem::Read(unit + 0x1290 + 4, array) || !array || !mem::Read(unit + 0x1290 + 0x18, count) || count > 64) continue;
        for (uint32_t i = 0; i < count; ++i) {
            uintptr_t shape = 0;
            uint32_t flags = 0;
            if (mem::Read(array + 4 * i, shape) && shape && mem::Read(shape + 0x98, flags) && (flags & 0x00400000))
                fn(shape, static_cast<uint16_t>(item));
        }
    }
}

void ApplyKnifeRanges(bool restore) {
    ForEachKnifeAttackShape([restore](uintptr_t shape, uint16_t item) {
        const float factor = restore ? 1.0f : KnifeRange(item);
        float points[6]{}, radius = 0;
        if (!mem::SafeRead(reinterpret_cast<void*>(shape + 0x70), points, 12) ||
            !mem::SafeRead(reinterpret_cast<void*>(shape + 0x80), points + 3, 12) || !mem::Read(shape + 0xA8, radius))
            return;
        KnifeShape* known = nullptr;
        for (KnifeShape& k : g_knifeShapes)
            if (k.shape == shape) known = &k;
        // New shape, or rebuilt by the game since (its radius is no longer ours): its values are the game's.
        if (!known || radius != known->radius * known->applied) {
            if (!known) {
                g_knifeShapes.push_back({});
                known = &g_knifeShapes.back();
            }
            known->shape = shape;
            std::memcpy(known->points, points, sizeof(points));
            known->radius = radius;
            known->applied = 1.0f;
        }
        if (known->applied == factor) return;
        float scaled[6];
        for (int i = 0; i < 6; ++i) scaled[i] = known->points[i] * factor;
        mem::SafeWrite(reinterpret_cast<void*>(shape + 0x70), scaled, 12);
        mem::SafeWrite(reinterpret_cast<void*>(shape + 0x80), scaled + 3, 12);
        mem::Write(shape + 0xA8, known->radius * factor);
        known->applied = factor;
        LOG("knife range: shape %08X radius %.1f -> %.1f", static_cast<unsigned>(shape), known->radius, known->radius * factor);
    });
}

void RestoreKnifeRange() {
    ApplyKnifeRanges(true);
    g_knifeShapes.clear();
}

// Every frame (game thread): the knives' ranges, while any knife's is not x1 or shapes were changed.
void TickKnifeRange() {
    const bool any = AnyKnifeRange();
    if (!any && g_knifeShapes.empty()) return;
    ApplyKnifeRanges(!any);
    if (!any) g_knifeShapes.clear();
}

using ApplyDamageFn = void(__fastcall*)(void* enemy, void* edx, void* hit, uint32_t extra);
ApplyDamageFn g_applyDamage = nullptr;
uintptr_t g_applying = 0;  // the enemy HookApplyDamage is applying a hit to (game thread)
int g_appliedAmount = 0;   // what its SubHealth received meanwhile (player.cpp, NoteEnemySubHealth)

// Executioner family (uEm20, uEm26, uEm27) and the chainsaw (uEm22, 13 parts, same layout): their own part kinds (14
// parts on uEm20): 1 body (part 0), 2 head (part 1), 3 (part 2), 4 and 5 the arms (parts 3-5, 6-8), 6 and 7 the legs (parts 9-10, 11-12), 8 (part 13: one
// pistol bullet makes it kneel, user test 2026-10-15; counted as the head).
bool IsExecutionerFamily(uintptr_t enemy) {
    uintptr_t vtable = 0;
    if (!mem::Read(enemy, vtable)) return false;
    const uintptr_t rva = vtable - reinterpret_cast<uintptr_t>(mem::Base());
    return rva == 0xB748A8 || rva == 0xB74BC0 || rva == 0xB74E38 || rva == 0xB75B20;
}

int PartRegion(uintptr_t enemy, uint8_t part) {
    uintptr_t kinds = 0;
    uint32_t count = 0, kind = 0;
    if (!mem::Read(enemy + 0x1290 + 0x30, kinds) || !kinds || !mem::Read(enemy + 0x1290 + 0x34, count) || part >= count ||
        !mem::Read(kinds + 4 * part, kind))
        return -1;
    if (IsExecutionerFamily(enemy)) {
        switch (kind) {
        case 2: case 8: return kHead;
        case 4: case 5: return kArms;
        case 6: case 7: return kLegs;
        case 1: case 3: return kBody;
        default: return -1;
        }
    }
    switch (kind) {
    case 2: return kHead;
    case 3: case 4: case 5: return kArms;
    case 7: case 8: case 9: case 0xA: return kLegs;
    case 1: case 6: return kBody;
    default: return -1;
    }
}

// The enemy OnHit (RVA 0x5E0840, thiscall(hit) ret 4).
using OnHitFn = void(__fastcall*)(void* enemy, void* edx, void* hit);
OnHitFn g_onHit = nullptr;

// Research (diag.hit_diff): the enemy object (0x2400 bytes) is copied before each hit by player 1 and compared 0.25 s
// later (render thread): the changed dwords are logged with the weapon, the part and the enemy's action then. Compares a
// pistol head shot that staggers with a knife stab that does not.
uint16_t HeldItem(uintptr_t player);
constexpr size_t kDiffSize = 0x2400;
std::atomic<bool> g_hitDiff{ false };
struct PendingDiff {
    uintptr_t enemy = 0;
    DWORD due = 0;
    uint16_t item = 0;
    uint8_t part = 0;
    uint8_t before[kDiffSize]{};
};
PendingDiff g_diff;
std::atomic<bool> g_diffPending{ false };

void TakeDiffSnapshot(uintptr_t enemy, void* hit) {
    if (g_diffPending.load()) return;  // one at a time
    using AttackerFn = uintptr_t(__fastcall*)(void* hit, void* edx);
    uintptr_t attacker = 0;
    __try {
        attacker = reinterpret_cast<AttackerFn>(mem::Rva(0x849530))(hit, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!attacker || attacker != actors::Player(0)) return;
    if (!mem::SafeRead(reinterpret_cast<void*>(enemy), g_diff.before, kDiffSize)) return;
    g_diff.enemy = enemy;
    g_diff.item = HeldItem(attacker);
    mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x49, g_diff.part);
    g_diff.due = GetTickCount() + 250;
    g_diffPending = true;
}

void TickHitDiff() {
    if (!g_diffPending.load() || static_cast<int>(GetTickCount() - g_diff.due) < 0) return;
    static uint8_t after[kDiffSize];
    if (mem::SafeRead(reinterpret_cast<void*>(g_diff.enemy), after, kDiffSize)) {
        uint32_t action = 0;
        mem::Read(g_diff.enemy + 0x10E0, action);
        std::string line;
        int n = 0;
        for (size_t o = 0; o < kDiffSize && n < 90; o += 4) {
            uint32_t a = 0, b = 0;
            std::memcpy(&a, g_diff.before + o, 4);
            std::memcpy(&b, after + o, 4);
            if (a == b) continue;
            // floats that only drift (positions, timers) are noise: keep ints and big float jumps
            float fa = 0, fb = 0;
            std::memcpy(&fa, &a, 4);
            std::memcpy(&fb, &b, 4);
            const bool floats = std::isfinite(fa) && std::isfinite(fb) && std::fabs(fa) > 1e-3f && std::fabs(fa) < 1e6f &&
                                std::fabs(fb) < 1e6f;
            if (floats && std::fabs(fb - fa) < 0.5f) continue;
            char item[40];
            sprintf_s(item, " %X:%X>%X", static_cast<unsigned>(o), a, b);
            line += item;
            ++n;
        }
        LOG("hit diff enemy %08X item %04X part %02X action now %X |%s", static_cast<unsigned>(g_diff.enemy), g_diff.item,
            g_diff.part, action, line.c_str());
    }
    g_diffPending = false;
}

// Critical hits (user, 2026-10-15: one box for the knife, one for every firearm, no slider - the game's weapon upgrades
// already raise the rate): a Majini's damage computation (vfunc +0x20C, VA 0x860450, run inside OnHit before
// ApplyDamage) rolls a head hit that does damage: critical (head blown, death) when the enemy's random +0x1034 modulo
// 8 (16 for uEm13, 12 for the class at 0x11EAA3C) is below the hit's critical value +0x38 (float, truncated; pistol 4,
// shotgun 2, melee 1, knife 0) - VA 0x860CA9. For player 1's knife (hit flag 0x00400000) or bullets (0x00080000) the
// value is set to 100 during OnHit and written back after.
// The knives: the chance of their row in the weapon table (HookOnHit; "Couteau : coups critiques à 100 %" before
// 2026-11-05).
std::atomic<bool> g_critGuns{ false };

bool WantsCritical(void* hit) {
    uint32_t flags = 0;
    if (!g_critGuns.load(std::memory_order_relaxed) || !mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x3C, flags) ||
        !(flags & 0x00080000))
        return false;
    using AttackerFn = uintptr_t(__fastcall*)(void* hit, void* edx);
    __try {
        const uintptr_t attacker = reinterpret_cast<AttackerFn>(mem::Rva(0x849530))(hit, nullptr);
        return attacker && attacker == actors::Player(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Parasites (user, 2026-10-15: a parried parasite played the head flash): a Majini whose plaga is out (sub-unit
// +0x1F9C) hit by player 1 on the plaga (hit part +0x49 >= 0x3C: the plaga's own shapes, see the reaction function VA
// 0x85FA97) or on the head gets the head flash after its OnHit - knife and firearms (bullet flag 0x00080000) each have
// their option, with a chance slider (user: 0 % = the game, 50 % = one hit in two, 100 % = every hit). Not if the hit
// killed it.
std::atomic<bool> g_plagaGuns{ false };
std::atomic<float> g_plagaGunsChance{ 100.0f };

bool Roll(float percent) {  // game thread
    static uint32_t state = GetTickCount() | 1;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state % 10000) / 100.0f < percent;
}

// On the ground, getting up or kneeling (user, 2026-10-15: a head hit stood a downed parasite up at once; 2026-10-21:
// the RE6-style quick shot hits a downed enemy without its state): cEm10 actions 0x6A / 0x6B RiseFaceUp / Down, 0x71
// DmgKnee, 0x73 DmgFlyoff, 0x75 / 0x76 DmgFall_F / B, 0x80 DmgUpper, 0x82 / 0x83 DmgAtFaceUp / Down, 0x8B and up the
// deaths.
bool DownAction(uint32_t action) {
    switch (action) {
    case 0x6A: case 0x6B: case 0x71: case 0x73: case 0x75: case 0x76: case 0x80: case 0x82: case 0x83: return true;
    default: return action >= 0x8B;
    }
}

// The knife: the "Parasite" chance of its row in the weapon table (fire_rate.cpp), either player's knife; firearms: player
// 1's, "Parasite : vacille aux armes à feu".
uintptr_t HitAttacker(void* hit) {
    using AttackerFn = uintptr_t(__fastcall*)(void* hit, void* edx);
    __try {
        return reinterpret_cast<AttackerFn>(mem::Rva(0x849530))(hit, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void PlagaFlash(uintptr_t enemy, void* hit) {
    uintptr_t plaga = 0;
    uint32_t flags = 0;
    uint8_t part = 0xFF;
    int16_t health = 0;
    const uintptr_t h = reinterpret_cast<uintptr_t>(hit);
    if (!mem::Read(h + 0x3C, flags)) return;
    float chance = 0.0f;
    if (flags & 0x00400000) {
        if (const int who = KnifeHitPlayer(hit); who >= 0) chance = KnifePlagaChance(KnifeOfHit(who));
    } else if ((flags & 0x00080000) && g_plagaGuns.load(std::memory_order_relaxed) && HitAttacker(hit) == actors::Player(0)) {
        chance = g_plagaGunsChance.load(std::memory_order_relaxed);
    }
    if (chance <= 0.0f || !mem::Read(enemy + 0x1F9C, plaga) || !plaga || !mem::Read(h + 0x49, part) ||
        !mem::Read(enemy + 0x1364, health) || health <= 0)
        return;
    if (part < 0x3C && PartRegion(enemy, part) != kHead) return;
    uint32_t action = 0;
    if (!mem::Read(enemy + 0x10E0, action) || DownAction(action)) return;
    if (!Roll(chance)) return;
    const bool flashed = MajiniHeadFlash(enemy);
    if (g_hitLog.load(std::memory_order_relaxed))
        LOG("plaga hit: enemy %08X part %02X %s", static_cast<unsigned>(enemy), part, flashed ? "head flash" : "no DmgHead");
}

// The flying parasites react to the melee blows (user, 2026-11-06: "hit in melee by Chris they took damage without
// reacting"; Sheva's Help kick swapped into the parasite rescue freed him only when it died, three kicks - the native
// rescue's knife makes it let go at once). A Kipepeo (uEm0c: Catch 0x0A, Release 0x0B, DmgBack 0x0C, DmgBlow 0x0D...)
// hit in melee (flags 0x00800000) by a player, alive and not already hurt or dying: holding a player (its Catch) it lets
// go (its Release), otherwise its DmgBlow. Its actions found by their class names (its action table +0x10DC, count
// +0x10D0).
int EnemyActionNamed(uintptr_t e, const char* suffix) {
    uint32_t count = 0;
    uintptr_t table = 0;
    if (!mem::Read(e + 0x10D0, count) || count > 0x200 || !mem::Read(e + 0x10DC, table) || !table) return -1;
    const size_t length = std::strlen(suffix);
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t object = 0;
        const char* name = mem::Read(table + 4 * i, object) && object ? UnitClassName(object) : nullptr;
        const size_t size = name ? std::strlen(name) : 0;
        if (size >= length && std::strcmp(name + size - length, suffix) == 0) return static_cast<int>(i);
    }
    return -1;
}

const char* EnemyActionNameNow(uintptr_t e) {
    uint32_t action = 0, count = 0;
    uintptr_t table = 0, object = 0;
    if (!mem::Read(e + 0x10E0, action) || !mem::Read(e + 0x10D0, count) || action >= count || count > 0x200 ||
        !mem::Read(e + 0x10DC, table) || !table || !mem::Read(table + 4 * action, object) || !object)
        return "";
    const char* name = UnitClassName(object);
    return name ? name : "";
}

bool RequestFlyerAction(uintptr_t e, int action) {
    using RequestFn = bool(__fastcall*)(void* self, void* edx, int action, int flag, int a, int b, int c);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(e, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return false;
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(e), nullptr, action, 1, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}
}  // namespace

// The reaction itself, also for the trainer's moves (wesker_moves.cpp Strike; user, 2026-11-06: "do not forget the
// bosses' blows the trainer plays, they are not native melee"): false when `e` is no Kipepeo, dead, or busy.
bool FlyingParasiteReact(uintptr_t e, const char* by) {
    const char* cls = UnitClassName(e);
    int16_t health = 0;
    if (!cls || std::strcmp(cls, "uEm0c") != 0 || !mem::Read(e + actors::off::Health, health) || health <= 0) return false;
    const char* now = EnemyActionNameNow(e);
    for (const char* busy : { "Dmg", "Die", "Release", "Erase", "Appear" })
        if (std::strstr(now, busy)) return false;
    const bool catching = std::strstr(now, "Catch") != nullptr;
    const int next = EnemyActionNamed(e, catching ? "ActRelease" : "ActDmgBlow");
    if (next < 0 || !RequestFlyerAction(e, next)) return false;
    LOG("flying parasite %08X hit by %s (%s, health %d): %s %X", static_cast<unsigned>(e), by, now, health,
        catching ? "lets go, Release" : "DmgBlow", next);
    return true;
}

namespace {
void FlyerMeleeReaction(uintptr_t e, void* hit) {
    uint32_t flags = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x3C, flags) || !(flags & 0x00800000) ||
        actors::PlayerIndex(HitAttacker(hit)) < 0)
        return;
    FlyingParasiteReact(e, "a melee blow");
}

void __fastcall HookOnHit(void* enemy, void* edx, void* hit) {
    const uintptr_t e = reinterpret_cast<uintptr_t>(enemy);
    if (hit && ParryIgnoresHit(e, hit)) return;  // the parry's riposte leaves its enemy untouched (parry.cpp)
    if (hit && GrabKeepsHolder(e, hit)) return;  // another enemy's grab on the Majini holding player 1 (grabs.cpp)
    if (hit && QuickShotTarget(e) && IsPlayer1Hit(hit, 0x00080000)) QuickShotSawHit(e);  // before its reaction
    if (g_hitDiff.load(std::memory_order_relaxed) && hit) TakeDiffSnapshot(e, hit);
    // The critical value: the weapon's own (upgrades included) times its "Coups critiques par arme" slider, the fraction
    // rolled (the game compares its int part), else the boxes' 100 %.
    float critical = 0, wanted = -1.0f;
    const bool known = hit && mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x38, critical);
    if (const uint16_t item = known ? PlayerWeaponHit(hit) : 0; item)
        if (const float rate = WeaponCritical(item); rate != 1.0f && critical > 0.0f) {
            const float scaled = critical * rate, whole = std::floor(scaled);
            wanted = whole + (Roll((scaled - whole) * 100.0f) ? 1.0f : 0.0f);
        }
    if (wanted < 0.0f && known && WantsCritical(hit)) wanted = 100.0f;
    if (wanted < 0.0f && known) {  // the knife's row: its chance, either player
        uint32_t flags = 0;
        const int who = mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x3C, flags) && (flags & 0x00400000) ? KnifeHitPlayer(hit) : -1;
        if (const float chance = who >= 0 ? KnifeCriticalChance(KnifeOfHit(who)) : 0.0f; chance > 0.0f && Roll(chance))
            wanted = 100.0f;
    }
    const bool forced = wanted >= 0.0f && mem::Write(reinterpret_cast<uintptr_t>(hit) + 0x38, wanted);
    // The free melee table (melee_tuning.cpp): player 1's game moves' critical, damage, kill and reaction.
    MeleeHitState melee;
    const bool tunedMelee = hit && MeleeHitBegin(e, hit, melee);
    g_onHit(enemy, edx, hit);
    if (tunedMelee) MeleeHitEnd(e, hit, melee);
    if (forced) mem::Write(reinterpret_cast<uintptr_t>(hit) + 0x38, critical);
    if (const int who = hit ? KnifeHitPlayer(hit) : -1; who >= 0) KnifeWearHit(who);  // knife_icon.cpp: "Durabilité du couteau"
    if (hit) PlagaFlash(e, hit);
    if (hit) FlyerMeleeReaction(e, hit);
    // Research: the big enemies (uEm20 Executioner, uEm26, uEm27, uEm23 Gatling, uEm24 / uEm12 chainsaw, uEm13, uEm14):
    // class and stagger state after each hit (+0x1E29 = the Executioner family's stagger byte, +0x1FB8 the gauge).
    uintptr_t vtable = 0;
    if (!g_hitLog.load(std::memory_order_relaxed) || !hit || !mem::Read(e, vtable)) return;
    const uintptr_t rva = vtable - reinterpret_cast<uintptr_t>(mem::Base());
    if (rva != 0xB748A8 && rva != 0xB74BC0 && rva != 0xB74E38 && rva != 0xB73068 && rva != 0xB71C38 && rva != 0xB732E0 &&
        rva != 0xB72680 && rva != 0xB71EB0 && rva != 0xB75B20)
        return;
    const uintptr_t h = reinterpret_cast<uintptr_t>(hit);
    int16_t hp = 0;
    uint32_t reactions = 0, action = 0, react40 = 0, flags3c = 0;
    int32_t power = 0;
    uint8_t stagger = 0, part = 0;
    mem::Read(e + 0x1364, hp);
    mem::Read(e + 0x1E2C, reactions);
    mem::Read(e + 0x1E29, stagger);
    mem::Read(e + 0x10E0, action);
    mem::Read(h + 0x40, react40);
    mem::Read(h + 0x3C, flags3c);
    mem::Read(h + 0x28, power);
    mem::Read(h + 0x49, part);
    int32_t gauge = 0;
    mem::Read(e + 0x1FB8, gauge);
    LOG("big enemy hit: enemy %08X vt %06X item %04X part %02X power %d +3C %08X +40 %08X -> hp %d react %08X stagger %d "
        "gauge %d action %X",
        static_cast<unsigned>(e), static_cast<unsigned>(rva), HeldItem(actors::Player(0)), part, power, flags3c, react40, hp,
        reactions, stagger, gauge, action);
}

bool EnsureOnHitHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x5E0840);
    const uint8_t expected[] = { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x85, 0xFF, 0x8B, 0xF1 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("enemy on hit: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookOnHit), reinterpret_cast<void**>(&g_onHit));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("enemy on hit hook installed at %p", target);
    return true;
}

uint16_t HeldItem(uintptr_t player) {
    uintptr_t held = 0;
    if (!player || !mem::Read(player + 0x2634, held) || !held) return 0;
    for (int k = 0; k < 24; ++k) {
        const uintptr_t record = player + 0x21A4 + 0x30 * k;
        uint32_t item = 0;
        uintptr_t unit = 0;
        if (mem::Read(record + 4, item) && mem::Read(record + 0x2C, unit) && unit == held) return static_cast<uint16_t>(item);
    }
    return 0;
}

// Stagger: a pistol head shot and a knife stab both give a Majini its DmgHead action (0x6D); the action picks its
// motion at its setup (vtable slot 6, VA 0xC21030) from the enemy's random +0x1034. Pistol: 0x50A-0x50D (the stagger,
// the action's +0x24 stays 1 = melee prompt); knife: mostly 0x501 / 0x502 (a flinch, +0x24 = 0), 0x50B-0x50D about one
// stab in three (diag.hit_diff, 2026-10-15). The difference is the "light hit" bit 0x100 of the enemy's +0x1BB4, set
// for knife hits after OnHit and consumed by the damage actions' setups (head: 8 chances in 12 of a flinch with it,
// none without; the arm and leg setups test it too). Neither the weapon kind +0x1DE0 (pistol 5, knife 3) nor the
// stagger gauge +0x1FB8 (+600 a bullet, +120 a stab) changed the motion. A stab chosen by the slider of its region
// marks the enemy; the next damage action setup of that enemy (within 0.5 s) gets the bit cleared after the common
// action setup (VA 0xC1AB30, which the damage setups call first) and before the setup reads it: the pistol's choice.
// Big Majini do not flinch per hit: every hit adds its power (hit +0x28) to the stagger gauge +0x1FB8 in their
// reaction function (vfunc +0x208, VA 0x85F7B0, shared by uEm10 / 11 / 12 / 13 / 14 / 21 / 23 / 24), half of it for a
// shotgun (+0x40 & 0x800), and they stagger (+0x1E2C |= 0x1000) when it reaches 1200 (uEm13), 1400 (uEm24) or 2800 /
// 3600 (uEm23, by difficulty); uEm14: 200. The other Majini add it twice and never test it. A stab chosen by a region
// slider counts there as a pistol bullet (power 300); +0x28 is only changed during that function (the damage is
// already done, ApplyDamage got its own amount).
// The Executioner family has no gauge: its stagger counter is the generic one of OnHit (VA 0x9DEC20, thiscall(hit),
// called when the reaction function asked a flinch): +0x1DF0 (float, falls back about 8 a second) += part value (table
// [+0x1DE8] by part kind, count +0x1DEC; uEm20: body 0.6, head 0.5, kind 3 0.4, arms 0.2, legs 0.3, kind 8 1.0) x
// weapon value (table VA 0xF9D2C8 by the weapon kind +0x1DE0: knife 3 -> 5, pistol 5 -> 100, 9 / 10 -> 80; kinds 0xC-0xF
// by distance, 200 to 50); at vfunc +0x214 (100) it sets the stagger byte +0x1E29 (the AI makes it kneel, action 0x1C)
// and resets. A stab chosen by a region slider is counted with the pistol's kind (the word is restored after); enemies
// without the table (Majini) leave the function at once.
// Dedicated sliders (user, 2026-10-15: two stabs at 100 % were too strong on a mini boss, about ten is more logical,
// keeping the pistol's randomness), per big enemy and per weapon: knife (every stab worth that percentage of a pistol
// bullet) and firearms (hit flag 0x00080000: every shot worth that percentage of its own normal shot - user: one
// pistol shot but four shotgun shots at 300 % was not coherent), in the game's own counter or gauge (part values,
// falling back with time and the stagger draw stay the game's): counter share x % / 5 (knife weapon value 5, pistol
// 100) or x % / 100 (firearms, shotguns' distance values included), gauge power the hit's own x 5 for the knife (pistol
// 250 / 150 against knife 50 / 30 on the same parts) or x 1 for a shot, x %, from the power before the "Puissance du
// couteau" multiplier. Classes (user tests): Executioner
// uEm20 (counter), Gatling uEm23 (gauge 3600, every hit counted twice, staggers into DmgGGHead 0x7A after an upper hit
// or DmgGGLeg 0x7B after a leg hit), chainsaw uEm22 (counter like the Executioner: body 0.7, head 0.5, kind 3 0.4,
// arms 0.2, legs 0.3; its own reaction function VA 0x878DB0), fat man uEm13 (gauge 1200, x2), giant uEm24 (gauge
// 1400, x2, helmeted head).
constexpr int32_t kBulletPower = 300;
enum BigEnemy { kExecutioner, kGatling, kChainsaw, kFatMan, kGiant, kBigEnemies };
enum Weapon { kKnife, kFirearm, kWeapons };
std::atomic<bool> g_bigOn[kWeapons][kBigEnemies]{};
std::atomic<float> g_bigPower[kWeapons][kBigEnemies]{};  // percent of a pistol bullet
const char* const kBigNames[kBigEnemies] = { "executioner", "gatling", "chainsaw", "fat man", "giant" };
const char* const kWeaponNames[kWeapons] = { "knife", "firearm" };

int BigEnemyOf(uintptr_t enemy) {
    uintptr_t vtable = 0;
    if (!mem::Read(enemy, vtable)) return -1;
    switch (vtable - reinterpret_cast<uintptr_t>(mem::Base())) {
    case 0xB748A8: return kExecutioner;              // uEm20
    case 0xB72680: return kGatling;                  // uEm23 (gauge 3600; uEm27 is not the Gatling)
    case 0xB75B20: return kChainsaw;                 // uEm22 (own reaction function VA 0x878DB0)
    case 0xB732E0: return kFatMan;                   // uEm13 (gauge 1200, every hit counted twice: user test)
    case 0xB73068: return kGiant;                    // uEm24 (gauge 1400, every hit counted twice, head armoured)
    default: return -1;
    }
}

// Region sliders = ordinary Majini only (user, 2026-10-15: with the dedicated box unticked a big enemy must stay the
// game's): the classes with a stagger threshold (gauge uEm13 / 23 / 24, counter uEm20 / 26 / 27 / 22) are left out.
bool IsBigEnemyClass(uintptr_t enemy) {
    uintptr_t vtable = 0;
    if (!mem::Read(enemy, vtable)) return false;
    switch (vtable - reinterpret_cast<uintptr_t>(mem::Base())) {
    case 0xB732E0: case 0xB72680: case 0xB73068: case 0xB748A8: case 0xB74BC0: case 0xB74E38: case 0xB75B20: return true;
    default: return false;
    }
}

std::atomic<uintptr_t> g_staggerEnemy{ 0 };
std::atomic<DWORD> g_staggerTick{ 0 };
void* g_gaugeHit = nullptr;  // game thread: the chosen stab, between ApplyDamage and the reaction function
void* g_accumHit = nullptr;  // game thread: the chosen stab, between ApplyDamage and the stagger counter
float g_bigPercent = 0.0f;   // game thread: > 0 = the hit is worth that percent of a bullet (dedicated slider)
float g_gaugeFactor = 1.0f;  // game thread: its power multiplier for a gauge
float g_counterFactor = 1.0f;  // game thread: its share multiplier for a counter
int32_t g_stabPower = 0;     // game thread: the hit's power before "Puissance du couteau"

bool AnyStaggerOption() {
    if (AnyKnifeStagger()) return true;
    for (const auto& weapon : g_bigOn)
        for (const auto& on : weapon)
            if (on.load(std::memory_order_relaxed)) return true;
    return false;
}

// Knife (hit flag 0x00400000) or firearm (bullet flag 0x00080000: pistols 0x00080000, shotguns 0x00080080; not the
// grenades 0x00204490) of player 1, -1 otherwise.
int HitWeapon(void* hit) {
    uint32_t flags = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x3C, flags)) return -1;
    int weapon = -1;
    if (flags & 0x00400000) weapon = kKnife;
    else if (flags & 0x00080000) weapon = kFirearm;
    else return -1;
    using AttackerFn = uintptr_t(__fastcall*)(void* hit, void* edx);
    __try {
        const uintptr_t attacker = reinterpret_cast<AttackerFn>(mem::Rva(0x849530))(hit, nullptr);
        return attacker && attacker == actors::Player(0) ? weapon : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void KnifeStagger(uintptr_t enemy, void* hit) {
    g_gaugeHit = nullptr;
    g_accumHit = nullptr;
    g_bigPercent = 0.0f;
    if (!hit || !AnyStaggerOption()) return;
    const int weapon = HitWeapon(hit);  // player 1's knife or firearm (the big enemies' sliders)
    const int big = BigEnemyOf(enemy);
    if (weapon >= 0 && big >= 0 && g_bigOn[weapon][big].load(std::memory_order_relaxed)) {
        g_bigPercent = g_bigPower[weapon][big].load(std::memory_order_relaxed);
        g_gaugeFactor = (weapon == kKnife ? 5.0f : 1.0f) * g_bigPercent / 100.0f;
        // The counter: the knife's share is its weapon value (5 for kind 3, table VA 0xF9D2C8) x part value.
        float knife = 5.0f;
        uint16_t kind = 0;
        if (weapon == kKnife && mem::Read(enemy + 0x1DE0, kind) && kind < 0xC)
            mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xB9D2C8)) + 4u * kind, knife);
        g_counterFactor = weapon == kKnife ? (knife > 0.0f ? g_bigPercent / knife : 0.0f) : g_bigPercent / 100.0f;
        g_stabPower = 0;
        mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x28, g_stabPower);
        if (g_hitLog.load(std::memory_order_relaxed))
            LOG("%s %s %08X: hit worth %.0f %% of a bullet", kWeaponNames[weapon], kBigNames[big],
                static_cast<unsigned>(enemy), g_bigPercent);
    } else {
        // An ordinary Majini: the "Vacillement" chance of the zone in the knife's row of the weapon table (fire_rate.cpp),
        // either player's knife hit (player 1's "Couteau : faire vaciller (zone)" before 2026-11-05).
        uint32_t flags = 0;
        if (big >= 0 || IsBigEnemyClass(enemy) || !mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x3C, flags) ||
            !(flags & 0x00400000))
            return;
        const int who = KnifeHitPlayer(hit);
        uint8_t part = 0xFF;
        if (who < 0 || !mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x49, part)) return;
        const int region = PartRegion(enemy, part);
        const float chance = region >= 0 ? KnifeStaggerChance(KnifeOfHit(who), region) : 0.0f;
        if (chance <= 0.0f || static_cast<float>(GetTickCount() % 1000) / 10.0f >= chance) return;
    }
    g_staggerTick = GetTickCount();
    g_staggerEnemy = enemy;
    g_gaugeHit = hit;
    g_accumHit = hit;
}

using AccumulateFn = void(__fastcall*)(void* enemy, void* edx, void* hit);
AccumulateFn g_accumulate = nullptr;

void __fastcall HookAccumulate(void* enemy, void* edx, void* hit) {
    const uintptr_t e = reinterpret_cast<uintptr_t>(enemy);
    if (!hit || hit != g_accumHit || e != g_staggerEnemy.load(std::memory_order_relaxed)) return g_accumulate(enemy, edx, hit);
    g_accumHit = nullptr;
    uint16_t kind = 0;
    uintptr_t table = 0;
    float before = 0, after = 0;
    uint8_t staggerBefore = 0, staggerAfter = 0;
    mem::Read(e + 0x1DE0, kind);
    mem::Read(e + 0x1DE8, table);
    mem::Read(e + 0x1DF0, before);
    mem::Read(e + 0x1E29, staggerBefore);
    if (g_bigPercent > 0.0f) {
        // The game counts the hit with its weapon's value; its share is scaled to the slider's.
        g_accumulate(enemy, edx, hit);
        float total = 0.0f, threshold = 100.0f;
        mem::Read(e + 0x1DF0, total);
        mem::Read(e + 0x1E29, staggerAfter);
        if (table && g_counterFactor > 0.0f && !(staggerAfter && !staggerBefore) && total > before) {
            total = before + (total - before) * g_counterFactor;
            using ThresholdFn = float(__fastcall*)(void* self, void* edx);
            uintptr_t vtable = 0, fn = 0;
            if (mem::Read(e, vtable) && mem::Read(vtable + 0x214, fn) && fn) {
                __try {
                    threshold = reinterpret_cast<ThresholdFn>(fn)(enemy, nullptr);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            if (total >= threshold) {
                mem::Write(e + 0x1E29, static_cast<uint8_t>(1));
                total = 0.0f;
            }
            mem::Write(e + 0x1DF0, total);
        }
    } else {
        if (kind == 3) mem::Write(e + 0x1DE0, static_cast<uint16_t>(5));
        g_accumulate(enemy, edx, hit);
        if (kind == 3) mem::Write(e + 0x1DE0, kind);
    }
    if (!table) return;
    mem::Read(e + 0x1DF0, after);
    mem::Read(e + 0x1E29, staggerAfter);
    if (g_hitLog.load(std::memory_order_relaxed))
        LOG("knife stagger counter: enemy %08X weapon kind %u, counter %.1f -> %.1f, stagger %d -> %d", static_cast<unsigned>(e),
            kind, before, after, staggerBefore, staggerAfter);
}

bool EnsureAccumulateHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x5DEC20);
    const uint8_t expected[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45, 0x08, 0x8A, 0x40, 0x49 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("enemy stagger counter: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookAccumulate), reinterpret_cast<void**>(&g_accumulate));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("enemy stagger counter hook installed at %p", target);
    return true;
}

using ReactionFn = bool(__fastcall*)(void* enemy, void* edx, void* hit);
ReactionFn g_reaction = nullptr;

bool __fastcall HookReaction(void* enemy, void* edx, void* hit) {
    if (hit && MeleeSkipsReaction(reinterpret_cast<uintptr_t>(enemy), hit)) return false;  // melee_tuning.cpp
    const uintptr_t e = reinterpret_cast<uintptr_t>(enemy), h = reinterpret_cast<uintptr_t>(hit);
    if (!hit || hit != g_gaugeHit || e != g_staggerEnemy.load(std::memory_order_relaxed)) return g_reaction(enemy, edx, hit);
    g_gaugeHit = nullptr;
    const int32_t wanted = g_bigPercent > 0.0f
                               ? static_cast<int32_t>(static_cast<float>(g_stabPower) * g_gaugeFactor + 0.5f)
                               : kBulletPower;
    int32_t power = 0, before = 0, after = 0;
    if (!mem::Read(h + 0x28, power) || power >= wanted) return g_reaction(enemy, edx, hit);
    mem::Read(e + 0x1FB8, before);
    mem::Write(h + 0x28, wanted);
    const bool result = g_reaction(enemy, edx, hit);
    mem::Write(h + 0x28, power);
    mem::Read(e + 0x1FB8, after);
    if (g_hitLog.load(std::memory_order_relaxed)) {
        uintptr_t vtable = 0;
        mem::Read(e, vtable);
        LOG("knife stagger gauge: enemy %08X vt %06X power %d -> %d, gauge %d -> %d", static_cast<unsigned>(e),
            static_cast<unsigned>(vtable - reinterpret_cast<uintptr_t>(mem::Base())), power, wanted, before, after);
    }
    return result;
}

bool EnsureReactionHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x45F7B0);
    const uint8_t expected[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x44, 0x53 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("enemy reaction: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookReaction), reinterpret_cast<void**>(&g_reaction));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("enemy reaction hook installed at %p", target);
    return true;
}

// cEm10ActDmg* (shared by every Majini, uEm10 / 11 / 21): Body, Head, Arm, ArmWeapon, Leg, Knee (vtable RVAs).
constexpr uintptr_t kDmgActionVtables[] = { 0xC3B198, 0xC3B1C8, 0xC3B1F8, 0xC3B228, 0xC3B254, 0xC3B284 };

// Common setup of an enemy action (VA 0xC1AB30, thiscall(action), ret): finds the owner (+0x18), +0x1B60 = +0x1B5C...
using ActionSetupFn = void(__fastcall*)(void* action, void* edx);
ActionSetupFn g_actionSetup = nullptr;

// The free melee table's flinch (parry.cpp MajiniFlinch, user 2026-10-30: "the flinch sometimes opened a second head
// flash"): its random draw is set when the game sets the DmgHead up (cEm10ActDmgHead, vtable RVA 0xC3B1C8), here,
// after the common setup and before the draw - set at the request, the draw had changed by then. The setup (VA
// 0xC21030) tests, in order: +0x1BB4 bit 0x200 (a light hit: 0x543 / 0x544, no prompt), +0x1B60 bit 0x400 (the head
// flash flag, the common setup's copy of +0x1B5C: its staggers with the prompt; user, 2026-10-31: left on a Majini out of
// a back hold, the elbow's flinch opened a head flash), +0x1F50 0x11 / 0xB / 0xE (other paths), then the 0x100 light
// hit's draw. So the head flash flag is cleared, and for the +0x1F50 cases the 0x200 path is taken.
std::atomic<uintptr_t> g_flinchEnemy{ 0 };
std::atomic<DWORD> g_flinchTick{ 0 };

bool FlinchSetup(uintptr_t a) {
    const uintptr_t enemy = g_flinchEnemy.load(std::memory_order_relaxed);
    if (!enemy) return false;
    if (GetTickCount() - g_flinchTick.load(std::memory_order_relaxed) > 500) {
        g_flinchEnemy = 0;
        return false;
    }
    uintptr_t owner = 0, vtable = 0;
    if (!mem::Read(a + 0x18, owner) || owner != enemy || !mem::Read(a, vtable) ||
        vtable != reinterpret_cast<uintptr_t>(mem::Rva(0xC3B1C8)))
        return false;
    g_flinchEnemy = 0;
    uint32_t light = 0, random = 0, previous = 0, kind = 0;
    if (mem::Read(enemy + 0x1B60, previous) && (previous & 0x400u)) mem::Write(enemy + 0x1B60, previous & ~0x400u);
    const bool otherPath = mem::Read(enemy + 0x1F50, kind) && (kind == 0x11 || kind == 0xB || kind == 0xE);
    if (mem::Read(enemy + 0x1BB4, light)) mem::Write(enemy + 0x1BB4, (light & ~0x302u) | (otherPath ? 0x200u : 0x100u));
    mem::Write<uint32_t>(enemy + 0x1DAC, 1);
    if (mem::Read(enemy + 0x1034, random)) mem::Write(enemy + 0x1034, random - random % 12 + ((random & 1) ? 4u : 8u));
    return true;
}

void __fastcall HookActionSetup(void* action, void* edx) {
    g_actionSetup(action, edx);
    if (FlinchSetup(reinterpret_cast<uintptr_t>(action))) return;
    const uintptr_t enemy = g_staggerEnemy.load(std::memory_order_relaxed);
    if (!enemy) return;
    if (GetTickCount() - g_staggerTick.load(std::memory_order_relaxed) > 500) {
        g_staggerEnemy = 0;
        return;
    }
    const uintptr_t a = reinterpret_cast<uintptr_t>(action);
    uintptr_t owner = 0, vtable = 0;
    if (!mem::Read(a + 0x18, owner) || owner != enemy || !mem::Read(a, vtable)) return;
    int index = -1;
    for (int i = 0; i < static_cast<int>(std::size(kDmgActionVtables)); ++i)
        if (vtable == reinterpret_cast<uintptr_t>(mem::Rva(kDmgActionVtables[i]))) index = i;
    if (index < 0) return;
    g_staggerEnemy = 0;
    uint32_t flags = 0;
    if (!mem::Read(enemy + 0x1BB4, flags)) return;
    if (flags & 0x100u) mem::Write(enemy + 0x1BB4, flags & ~0x100u);
    if (g_hitLog.load(std::memory_order_relaxed))
        LOG("knife stagger: enemy %08X damage action %d (Body Head Arm ArmWeapon Leg Knee), light hit bit %s",
            static_cast<unsigned>(enemy), index, (flags & 0x100u) ? "cleared" : "was not set");
}

bool EnsureActionSetupHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x81AB30);
    const uint8_t expected[] = { 0x53, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7E, 0x08, 0x33, 0xDB };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("enemy action setup: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookActionSetup), reinterpret_cast<void**>(&g_actionSetup));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("enemy action setup hook installed at %p", target);
    return true;
}

void __fastcall HookApplyDamage(void* enemy, void* edx, void* hit, uint32_t extra) {
    const uintptr_t e = reinterpret_cast<uintptr_t>(enemy);
    KnifeStagger(e, hit);
    int16_t before = 0, after = 0;
    uint32_t reactions = 0;  // +0x1E2C: reaction bits set by the enemy's damage computation (vfunc +0x20C) before
    mem::Read(e + 0x1364, before);
    mem::Read(e + 0x1E2C, reactions);
    TuneKnifeHit(hit, extra);
    TuneWeaponHit(hit, extra);
    MeleeHitAmount(reinterpret_cast<uintptr_t>(enemy), hit, extra);  // melee_tuning.cpp
    g_applying = e;
    g_appliedAmount = 0;
    g_applyDamage(enemy, edx, hit, extra);
    g_applying = 0;
    // The RE6-style quick shot's damage: what player 1's last shot with that weapon took off an enemy (each shotgun
    // pellet comes here on its own: quick_knife.cpp adds them up). The amount the enemy's SubHealth received, kills
    // included (2026-10-21: the health lost stops at the health left, so magnums and rifles, which kill a Majini at
    // once, learnt nothing); the health lost when SubHealth was not seen.
    uint8_t part = 0xFF;
    mem::Read(e + 0x1364, after);
    const int amount = g_appliedAmount > 0 ? g_appliedAmount : before > after ? before - after : 0;
    // Not on the big enemies (their parts take their own share: the quick shot's targets are the ordinary ones).
    if (hit && amount > 0 && mem::Read(reinterpret_cast<uintptr_t>(hit) + 0x49, part) && part < 0x3C &&
        IsPlayer1Hit(hit, 0x00080000) && !IsBigEnemyClass(e))
        RecordBulletDamage(e, HeldItem(actors::Player(0)), amount, PartRegion(e, part) == kHead);
    if (!g_hitLog.load(std::memory_order_relaxed)) return;
    static std::atomic<int> lines{ 0 };
    if (lines.fetch_add(1) >= 300) return;
    mem::Read(e + 0x1364, after);
    uintptr_t vtable = 0;
    mem::Read(e, vtable);
    const uintptr_t p1 = actors::Player(0);
    uint32_t action = 0;
    if (p1) mem::Read(p1 + 0x10E0, action);
    uint32_t reactionsAfter = 0;
    mem::Read(e + 0x1E2C, reactionsAfter);
    LOG("hit enemy %08X vt %06X hp %d->%d extra %X react %08X->%08X | P1 action %X item %04X | info %s",
        static_cast<unsigned>(e), static_cast<unsigned>(vtable - reinterpret_cast<uintptr_t>(mem::Base())), before, after,
        extra, reactions, reactionsAfter, action, HeldItem(p1), mem::Hex(hit, 0xA0).c_str());
}

bool EnsureHitHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x5DEA50);
    const uint8_t expected[] = { 0x55, 0x8B, 0x6C, 0x24, 0x08, 0x85, 0xED, 0x56, 0x8B, 0xF1 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("enemy apply damage: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookApplyDamage), reinterpret_cast<void**>(&g_applyDamage));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("enemy apply damage hook installed at %p", target);
    return true;
}
}  // namespace

void RegisterKnife() {
    // The knives' rows of the weapon table (fire_rate.cpp): their hooks once a value asks for them, their range every
    // frame (the options of the Couteau tab they replace installed them when checked).
    OnFrame([] {
        static DWORD tried = 0;
        static bool staggerHooks = false, onHit = false;
        if (((!staggerHooks && AnyKnifeStagger()) || (!onHit && AnyKnifePlaga())) && GetTickCount() - tried > 1000) {
            tried = GetTickCount();
            if (!staggerHooks && AnyKnifeStagger())
                staggerHooks = EnsureHitHook() && EnsureActionSetupHook() && EnsureReactionHook() && EnsureAccumulateHook();
            if (!onHit && AnyKnifePlaga()) onHit = EnsureOnHitHook();
        }
        TickKnifeRange();
    });
    OnRestore(RestoreKnifeRange);

    struct StaggerOption {
        const char* id;
        const char* name;
        const char* help;
    };
    static const StaggerOption bigOptions[kBigEnemies] = {
        { "combat.knife_fall_executioner", "Couteau : faire tomber le bourreau",
          "Chaque coup de couteau de Chris compte pour ce pourcentage d'une balle de pistolet dans ce qui fait tomber le "
          "bourreau à genoux (au pistolet : 2 balles au corps ou à la tête, une seule à un endroit précis). Le jeu garde "
          "son hasard : la zone touchée compte plus ou moins (bras et jambes peu) et l'effet retombe vite avec le temps : "
          "à 100 %, en général 4 à 10 coups selon la zone et le rythme. Comme pour une balle, un coup ne compte pas "
          "pendant ses attaques." },
        { "combat.knife_fall_gatling", "Couteau : faire tomber la gatling",
          "Chaque coup de couteau de Chris compte pour ce pourcentage d'une balle de pistolet dans ce qui fait vaciller le "
          "Majini à la gatling (au pistolet : 7 à 8 balles). Il vacille de la tête après un coup en haut (bras, corps, "
          "tête) et des jambes après un coup aux jambes, comme au pistolet. Vers 80 %, une dizaine de coups." },
        { "combat.knife_fall_chainsaw", "Couteau : faire tomber la tronçonneuse",
          "Chaque coup de couteau de Chris compte pour ce pourcentage d'une balle de pistolet dans ce qui fait vaciller le "
          "Majini à la tronçonneuse, comme pour le bourreau (au pistolet : 2 balles au corps ou à la tête). Le jeu garde "
          "son hasard : la zone touchée compte plus ou moins (bras et jambes peu) et l'effet retombe vite avec le temps. "
          "Comme pour une balle, un coup ne compte pas pendant ses attaques." },
        { "combat.knife_fall_fatman", "Couteau : faire tomber le fat man",
          "Chaque coup de couteau de Chris compte pour ce pourcentage d'une balle de pistolet dans ce qui fait vaciller le "
          "gros Majini (fat man ; au pistolet : 3 à 4 balles, au couteau du jeu : une douzaine de coups). Vers 30 %, "
          "environ 8 coups." },
        { "combat.knife_fall_giant", "Couteau : faire tomber le Majini géant",
          "Chaque coup de couteau de Chris compte pour ce pourcentage d'une balle de pistolet dans ce qui fait tomber le "
          "Majini géant (au couteau du jeu : une douzaine de coups ; sa tête casquée ne compte pas). Vers 30 %, environ 9 "
          "coups." },
    };
    // Firearms: ids kept from the handgun-only version (the user's settings).
#define FIREARM_SHOT "Chaque tir de Chris (toutes armes à feu) compte pour ce pourcentage d'un tir normal de son arme "
    static const StaggerOption pistolOptions[kBigEnemies] = {
        { "combat.pistol_fall_executioner", "Armes à feu : faire tomber le bourreau",
          FIREARM_SHOT "dans ce qui fait tomber le bourreau à genoux (100 % = le jeu : 2 balles de pistolet au corps ou à "
          "la tête). Le jeu garde son hasard (zone touchée, effet qui retombe avec le temps, pas pendant ses attaques)." },
        { "combat.pistol_fall_gatling", "Armes à feu : faire tomber la gatling",
          FIREARM_SHOT "dans ce qui fait vaciller le Majini à la gatling (100 % = le jeu : 7 à 8 balles de pistolet)." },
        { "combat.pistol_fall_chainsaw", "Armes à feu : faire tomber la tronçonneuse",
          FIREARM_SHOT "dans ce qui fait vaciller le Majini à la tronçonneuse (100 % = le jeu : 2 balles de pistolet au "
          "corps ou à la tête). Le jeu garde son hasard." },
        { "combat.pistol_fall_fatman", "Armes à feu : faire tomber le fat man",
          FIREARM_SHOT "dans ce qui fait vaciller le gros Majini (100 % = le jeu : 3 à 4 balles de pistolet)." },
        { "combat.pistol_fall_giant", "Armes à feu : faire tomber le Majini géant",
          FIREARM_SHOT "dans ce qui fait tomber le Majini géant (100 % = le jeu ; sa tête casquée ne compte pas)." },
    };
#undef FIREARM_SHOT
    // Knife defaults for about ten stabs (the Executioner's counter falls back with time, the gauges do not);
    // firearms: twice the game's.
    static const float bigDefaults[kWeapons][kBigEnemies] = { { 100, 80, 60, 30, 30 }, { 200, 200, 200, 200, 200 } };
    for (int weapon = 0; weapon < kWeapons; ++weapon)
        for (int big = 0; big < kBigEnemies; ++big) {
            const StaggerOption& option = weapon == kKnife ? bigOptions[big] : pistolOptions[big];
            Feature& fall = Add({ .id = option.id, .name = option.name, .help = option.help,
                                  .category = weapon == kKnife ? "Couteau" : "Combat",
                                  .kind = Kind::Toggle, .available = [] { return game::IsSupported(); },
                                  .value = bigDefaults[weapon][big], .valueMin = weapon == kKnife ? 1.0f : 100.0f,
                                  .valueMax = 1000, .valueFormat = "%.0f %% d'une balle" });
            Feature* const pf = &fall;
            fall.set = [pf, weapon, big](bool on) {
                if (on && (!EnsureHitHook() || !EnsureReactionHook() || !EnsureAccumulateHook())) return false;
                g_bigPower[weapon][big] = pf->value;
                g_bigOn[weapon][big] = on;
                return true;
            };
            fall.tick = [pf, weapon, big] { g_bigPower[weapon][big].store(pf->value, std::memory_order_relaxed); };
        }

    Feature& plagaGuns = Add({ .id = "combat.plaga_flash_guns", .name = "Parasite : vacille aux armes à feu",
                               .help = "Une balle de Chris (toutes armes à feu) sur le parasite sorti d'un Majini (ou sur "
                                       "sa tête) le met en vacillement « head flash », comme une parade : le corps à corps "
                                       "devient possible. Curseur : la chance (0 % = le jeu, 50 % = un coup sur deux, "
                                       "100 % = à chaque coup).",
                               .category = "Combat", .kind = Kind::Toggle,
                               .available = [] { return game::IsSupported(); },
                               .value = 100, .valueMin = 0, .valueMax = 100, .valueFormat = "%.0f %%" });
    Feature* const pg = &plagaGuns;
    plagaGuns.set = [pg](bool on) {
        if (on && !EnsureOnHitHook()) return false;
        g_plagaGunsChance = pg->value;
        g_plagaGuns = on;
        return true;
    };
    plagaGuns.tick = [pg] { g_plagaGunsChance.store(pg->value, std::memory_order_relaxed); };
    Add({ .id = "combat.critical_guns", .name = "Armes à feu : coups critiques à 100 %",
          .help = "Chaque balle de Chris (pistolets, fusils à pompe, fusils, mitraillettes, magnums) dans la tête d'un "
                  "Majini est un coup critique : la tête explose. Sans cette case, c'est le hasard du jeu (que les "
                  "améliorations d'armes augmentent). Les ennemis sans coup critique (gros ennemis) ne changent pas.",
          .category = "Combat", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureOnHitHook()) return false;
              g_critGuns = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });


    Add({ .id = "diag.hit_diff", .name = "Différences après un coup (recherche)",
          .help = "Écrit dans trainer.log ce qui change dans un ennemi 0,25 s après un coup du joueur 1.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureOnHitHook()) return false;
              g_hitDiff = on;
              return true;
          },
          .tick = TickHitDiff,
          .available = [] { return game::IsSupported(); } });

    Add({ .id = "diag.hit_log", .name = "Journal des coups (recherche)",
          .help = "Écrit dans trainer.log chaque coup reçu par un ennemi (contenu du coup, arme en main, action du joueur 1).",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureHitHook()) return false;
              g_hitLog = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
}
bool EnsureEnemyOnHitHook() { return EnsureOnHitHook(); }
bool EnsureEnemyReactionHook() { return EnsureReactionHook(); }
bool EnsureEnemyActionSetupHook() { return EnsureActionSetupHook(); }
// parry.cpp: the flinch's draw set at its setup (FlinchSetup).
void MarkMajiniFlinch(uintptr_t enemy) {
    g_flinchTick = GetTickCount();
    g_flinchEnemy = enemy;
}

bool EnsureEnemyDamageHook() { return EnsureHitHook(); }

// parry.cpp (a grab parried with the knife hurts as a knife hit): "Puissance du couteau" (when on) times the power of the
// knife in player 1's hand.
float Player1KnifeDamageFactor() { return KnifePower(KnifeOfHit(0)); }

// The actors' SubHealth (player.cpp), for an enemy: the whole amount of the hit being applied, before the health clamp.
void NoteEnemySubHealth(uintptr_t enemy, int amount) {
    if (enemy && enemy == g_applying && amount > 0) g_appliedAmount += amount;
}
uint16_t PlayerHeldItem(uintptr_t player) { return HeldItem(player); }
bool BigEnemyClass(uintptr_t enemy) { return IsBigEnemyClass(enemy); }  // mercenaries.cpp: kills reported

bool EnemyIsDown(uintptr_t enemy) {
    uint32_t action = 0;
    return mem::Read(enemy + 0x10E0, action) && DownAction(action);
}

// On the ground for the RE6-style quick shot: DownAction but kneeling 0x71 (DmgKnee: still up on a knee). RiseFaceUp /
// Down 0x6A / 0x6B are the whole time a Majini lies there before it gets up (11 s in 0x6A recorded): allowing them
// gave head flashes on downed enemies (user, 2026-10-21). DownAction counts every action from 0x8B: the deaths are
// 0x8B..0x96, then Catch 0x97 / DashCatch 0x98 reach for player 1 standing (user, 2026-10-23: quick shots on them put
// no state; cEm10Act* DTI order) and the holds 0x99.. stay excluded.
bool EnemyOnGround(uintptr_t enemy) {
    uint32_t action = 0;
    return mem::Read(enemy + 0x10E0, action) && DownAction(action) && action != 0x71 && action != 0x97 && action != 0x98;
}
}  // namespace features
