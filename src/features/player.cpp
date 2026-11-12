// Joueur / Ennemis / Combat options built on live player data and the shared damage routine.
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/hotkeys.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "features/melee_tuning.hpp"
#include "common/settings.hpp"

#include <windows.h>
#include <intrin.h>
#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace features {
void StaminaTrainerHeal();     // stamina.cpp: "Soigner le joueur 1" fills it
void StaminaHealthRescaled();  // stamina.cpp: "Vie max" changed his health, no heal

uintptr_t CameraBlock(int slot);
uintptr_t CameraOverride(int k);  // camera.cpp: copies built at camera setup (normal 0..2, aiming 3..5)
bool IsCameraParams(uintptr_t block);  // camera.cpp: still valid camera values
uint32_t ActorCharacterInfo(void* actor);  // swaps.cpp: (character << 8) | costume
bool BlockPlayer2Actions(int category, bool on);  // action_log.cpp: 0 attack, 1 pickup, 2 revive, 3 grab rescue
bool GrabGraceRefusesDamage();  // licker.cpp: start of a grab with a struggle prompt, damage refused
bool GrabRescueActive();        // dying.cpp: a grab made player 1 fall dying, damage refused until free
bool Re6DyingHitKills();        // dying.cpp: "Mourant façon RE6", a hit while dying kills
bool ParryHit(uintptr_t player, bool fromReaction);  // parry.cpp: the hit pending on player 1 is parried (no reaction, no damage)
uintptr_t HitAttacker(uintptr_t hit);         // parry.cpp: the unit a hit comes from (RVA 0x849530)
const char* UnitClassName(uintptr_t object);  // parry.cpp: DTI class name
bool EnsurePlayerRequestHook();               // action_log.cpp
bool EnsureEnemyActionHook();                 // action_log.cpp
void NoteEnemySubHealth(uintptr_t enemy, int amount);  // knife.cpp: the quick shot's damage of a hit, kills included
bool WeskerMoveInvulnerable(uintptr_t player);         // wesker_moves.cpp: player 1 in one of Wesker's attacks
bool PerfectDodgeHit(uintptr_t player, bool fromReaction);  // perfect_dodge.cpp: the pending hit dodged
void NotePlayer1BlowTaken(uintptr_t player);                // parry.cpp: a blow let through (no late parry of it)
bool BossMoveSilencesVoices();                         // wesker_moves.cpp: a borrowed boss move just started
bool Player1InventoryOpen();                           // pickup.cpp: shortcuts kept for the inventory's
uintptr_t WeskerThrownUnit();                          // wesker_moves.cpp: the Majini in Jill's throw
bool BetaBackThrowOn();                                // beta_anims.cpp: "Projection de dos (animation beta)"
bool WeskerKeepsHealth(uintptr_t enemy);               // wesker_boss.cpp: "Wesker n'abandonne jamais"
bool JillKeepsHealth(uintptr_t enemy);                 // jill_device.cpp: "Figer la vie de Jill"
int16_t LinWeskerDamage(uintptr_t enemy, int16_t amount);  // wesker_boss.cpp: "Dégâts reçus par Wesker (Lost in Nightmares)"
bool RocketCatchHidesWeapon();                         // rocket_catch.cpp: player 1's weapon hidden in the rocket catch
bool CrankHidesWeapon(int index);                      // crank.cpp: the weapon hidden while turning a crank
bool PianoHidesWeapon(int index);                      // coop_solo.cpp: hidden at the piano and the fire
bool BorrowedMoveHidesWeapon();  // swaps.cpp: a borrowed move with a tool in the hand
bool TrainerMoveHidesWeapon();   // wesker_moves.cpp: a trainer move played without the weapon (RE6's machete disarm)
bool RocketLocalMatrix(uintptr_t unit, float* m);      // rocket_catch.cpp: the caught rocket's pose in the hand
bool FinalFightPartnerFree();                          // final_fight.cpp: the partner's options stand aside there
int16_t FinalQteDamage(uintptr_t enemy, int16_t amount);  // final_fight.cpp: the volcano's Wesker kept for his finish

uint16_t PlayerHeldItem(uintptr_t player);  // knife.cpp: the item in a player's hand
const char* UnitClassName(uintptr_t object);  // parry.cpp: DTI class name
bool Re6WeaponJointAt(int index, uint32_t motion, float frame, float (&q)[4], float (&t)[3]);  // wesker_moves.cpp: RE6's bone 9
bool Player1MovesetLent();                   // swaps.cpp: another character's moveset lent to player 1 for a move

namespace {

// ---- Player objects ------------------------------------------------------------------------
// [re5dx9+0xDB27DC] = player manager; players at +0x24 (P1) +0x28 (P2) +0x2C (P3) +0x30 (P4).
constexpr uintptr_t kHealth = 0x1364;      // int16 current
constexpr uintptr_t kHealthMax = 0x1366;   // int16 max
constexpr uintptr_t kWeaponAnim = 0x1AF8;  // byte: 0 male, 1 female
constexpr uintptr_t kHandedness = 0x1AF9;  // byte: 0 right, 1 left
constexpr uintptr_t kCollision = 0x1B08;   // u32: 1 normal, 0 no collision
constexpr uintptr_t kHeldWeapon = 0x2634;  // item object of the weapon in hand (one of the inventory objects)

// Player 1 = the local player, player 2 = the partner, whatever the game's slots (actors.cpp).
uintptr_t Player(int index) { return actors::Player(index); }
int PlayerIndex(uintptr_t actor) { return actors::PlayerIndex(actor); }

bool InGame() { return game::IsSupported() && Player(0) != 0; }

// ---- Damage routine (RVA 0x44F7C0) ---------------------------------------------------------
// void __thiscall Actor::SubHealth(int16 amount): health -= amount, clamped to [0, max]. Used by every
// actor, which is why patching its body (as the Ultimate Trainer does) affects players AND enemies.
// We hook it and decide per actor.
std::atomic<bool> g_godPlayer[4];
std::atomic<bool> g_invinciblePlayer[4];  // no hit reaction at all (not just health)
// "Esquive parfaite" (perfect_dodge.cpp, SetDodgeInvincible): player 1's shapes hidden from the enemies the same way
// (number 0, below) for the window after a dodge and the rest of a perfect one - no contact at all (user, 2026-10-29:
// with his hurt shapes merely switched off he was "insensible, pas invincible": the blow still sounded on him, the game
// switches the shapes back during its dodges, from seven writers).
std::atomic<bool> g_dodgeInvincible{ false };
std::atomic<bool> g_enemyNeverDie{ false };
std::atomic<bool> g_oneHitKill{ false };
bool g_damageHookInstalled = false;

using SubHealthFn = void(__fastcall*)(void* self, void* edx, int16_t amount);
SubHealthFn g_subHealth = nullptr;        // base actor version (enemies...), RVA 0x44F7C0
SubHealthFn g_playerSubHealth = nullptr;  // uPlayer override, RVA 0x77C950 (same contract)

// Research (diag.damage_log): each distinct call stack that damages an enemy, with what player 1 is doing
// (action P+0x10E0, body motion bank/index P+0x1948/+0x194C) - compares a melee move that hits with one that
// passes through (Reunion duo moves, 2026-10-13).
std::atomic<bool> g_damageLog{ false };

void LogEnemyDamage(void* enemy, int16_t amount) {
    static std::atomic<int> lines{ 0 };
    if (lines.fetch_add(1) >= 400) return;  // every hit, not only new call stacks
    void* frames[8]{};
    const USHORT n = RtlCaptureStackBackTrace(1, 8, frames, nullptr);
    uint32_t action = 0, bank = 0, index = 0;
    if (uintptr_t p1 = Player(0)) {
        mem::Read(p1 + 0x10E0, action);
        mem::Read(p1 + 0x1948, bank);
        mem::Read(p1 + 0x194C, index);
    }
    char line[640];
    int len = sprintf_s(line, "enemy damage %p amount %d (P1 action 0x%X motion %X%02X) stack:", enemy, amount, action, bank, index);
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    for (USHORT i = 0; i < n && len < 600; ++i) {
        const uintptr_t f = reinterpret_cast<uintptr_t>(frames[i]);
        len += sprintf_s(line + len, sizeof(line) - len, f >= base && f < base + 0x1000000 ? " %06X" : " (%p)",
                         f >= base && f < base + 0x1000000 ? static_cast<unsigned>(f - base) : f);
    }
    LOG("%s", line);
}

// ---- Vehicle health (truck of chapter 2-3, uOm1304): the vehicle is an actor like the enemies, health at +0x1364
// (int16, max +0x1366 = 3500), damaged through the same SubHealth (2026-10-14: 3500 -> 2625 -> 1375, the amounts
// logged by diag.damage_log). It is recognised as the vehicle carrying a mounted gun of one of the players (the
// mounted gun units, items 0x012C-0x012F / 0x0131, are attached to it: unit +0x18). The enemy options (never die,
// one-hit kill) leave it alone.
std::atomic<bool> g_vehicleInvincible{ false };

bool IsPlayersVehicle(uintptr_t actor) {
    for (int index = 0; index < 2; ++index) {
        const uintptr_t p = Player(index);
        if (!p) continue;
        for (int k = 0; k < 24; ++k) {
            const uintptr_t record = p + 0x21A4 + 0x30 * k;
            uint32_t item = 0;
            uintptr_t unit = 0, parent = 0;
            if (!mem::Read(record + 4, item)) continue;
            item &= 0xFFFF;
            if (item != 0x012C && item != 0x012D && item != 0x012E && item != 0x012F && item != 0x0131) continue;
            if (mem::Read(record + 0x2C, unit) && unit && mem::Read(unit + 0x18, parent) && parent == actor) return true;
        }
    }
    return false;
}

void __fastcall HookSubHealth(void* self, void* edx, int16_t amount) {
    const int index = PlayerIndex(reinterpret_cast<uintptr_t>(self));
    if (index < 0 && amount > 0 && g_damageLog.load(std::memory_order_relaxed)) LogEnemyDamage(self, amount);
    if (index < 0 && amount > 0) NoteEnemySubHealth(reinterpret_cast<uintptr_t>(self), amount);  // knife.cpp
    if (index >= 0) {
        if (g_godPlayer[index].load(std::memory_order_relaxed) && amount > 0) return;
        if (index == 0 && amount > 0 && GrabRescueActive()) return;
    } else if (amount > 0 && IsPlayersVehicle(reinterpret_cast<uintptr_t>(self))) {
        if (g_vehicleInvincible.load(std::memory_order_relaxed)) return;
    } else if (amount > 0) {
        if (g_enemyNeverDie.load(std::memory_order_relaxed)) return;
        if (WeskerKeepsHealth(reinterpret_cast<uintptr_t>(self))) return;
        if (JillKeepsHealth(reinterpret_cast<uintptr_t>(self))) return;
        amount = LinWeskerDamage(reinterpret_cast<uintptr_t>(self), amount);
        amount = FinalQteDamage(reinterpret_cast<uintptr_t>(self), amount);
        MeleeEnemySubHealth(reinterpret_cast<uintptr_t>(self), amount);  // melee_tuning.cpp: the back escape's damage
        if (g_oneHitKill.load(std::memory_order_relaxed)) {
            int16_t health = 0;
            if (mem::Read(reinterpret_cast<uintptr_t>(self) + kHealth, health) && health > amount) amount = health;
        }
    }
    g_subHealth(self, edx, amount);
}

// Research: log each distinct call stack that damages a player (to find the hit handler for invincibility).
void LogDamageStack(int index, int16_t amount) {
    static uint32_t seen[32]{};
    static int seenCount = 0;
    void* frames[10]{};
    const USHORT n = RtlCaptureStackBackTrace(1, 10, frames, nullptr);
    uint32_t hash = 0;
    for (USHORT i = 0; i < n; ++i) hash = hash * 31 + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(frames[i]));
    for (int i = 0; i < seenCount; ++i)
        if (seen[i] == hash) return;
    if (seenCount < 32) seen[seenCount++] = hash;
    char line[512];
    int len = sprintf_s(line, "damage P%d amount %d stack:", index + 1, amount);
    for (USHORT i = 0; i < n && len < 480; ++i) len += sprintf_s(line + len, sizeof(line) - len, " %p", frames[i]);
    LOG("%s", line);
}

// Research (diag.player_hit_log, 2026-10-21: grabs while dying never happen; held, the Executioner's downward swing
// does no damage and its grab frees player 1): in trainer.log, in order, each hit pending on player 1 when his hit
// reaction runs (the attacker and its action, the hit's fields), what he does after it, his requested actions, his
// losses of health, and the actions requested by the enemies within 8 m.
std::atomic<bool> g_hitLog{ false };

void LogPendingHit(uintptr_t player) {
    uintptr_t hit = 0;
    uint8_t valid = 0;
    if (!mem::Read(player + 0x129C, hit) || !hit || !mem::Read(hit + 0x48, valid) || !valid) {
        LOG("hit log: reaction without a pending hit");
        return;
    }
    const uintptr_t attacker = HitAttacker(hit);
    uint32_t attackerAction = 0, action = 0, flags3C = 0, flags40 = 0, flags44 = 0, x9C = 0, xA0 = 0, power = 0, s2D7C = 0,
             s2D8C = 0;
    float x34 = 0, left = 0;
    int16_t health = 0;
    if (attacker) mem::Read(attacker + 0x10E0, attackerAction);
    mem::Read(player + 0x10E0, action);
    mem::Read(player + kHealth, health);
    mem::Read(player + 0x1BAC, left);
    mem::Read(player + 0x2D7C, s2D7C);
    mem::Read(player + 0x2D8C, s2D8C);
    mem::Read(hit + 0x28, power);
    mem::Read(hit + 0x34, x34);
    mem::Read(hit + 0x3C, flags3C);
    mem::Read(hit + 0x40, flags40);
    mem::Read(hit + 0x44, flags44);
    mem::Read(hit + 0x9C, x9C);
    mem::Read(hit + 0xA0, xA0);
    LOG("hit log: hit on P1 (action %X, HP %d, dying %.1f s, +2D7C %08X +2D8C %08X) by %s %08X (action %X): power %d "
        "+34 %.2f +3C %08X +40 %08X +44 %08X +9C %08X +A0 %08X",
        action, health, left, s2D7C, s2D8C, attacker ? UnitClassName(attacker) : "-", static_cast<unsigned>(attacker),
        attackerAction, power, x34, flags3C, flags40, flags44, x9C, xA0);
}

void LogHealthLoss(uintptr_t player, int16_t amount, uintptr_t caller) {
    static uintptr_t lastCaller = 0;
    static DWORD lastTick = 0;
    static int skipped = 0;
    const DWORD now = GetTickCount();
    if (caller == lastCaller && now - lastTick < 300) {
        skipped += amount;  // a bite drains every frame
        return;
    }
    uint32_t action = 0;
    int16_t health = 0;
    mem::Read(player + 0x10E0, action);
    mem::Read(player + kHealth, health);
    LOG("hit log: P1 loses %d (HP %d, action %X) from RVA %06X%s", amount, health, action,
        static_cast<unsigned>(caller - reinterpret_cast<uintptr_t>(mem::Base())), skipped ? " (more before)" : "");
    lastCaller = caller;
    lastTick = now;
    skipped = 0;
}

void __fastcall HookPlayerSubHealth(void* self, void* edx, int16_t amount) {
    const int index = PlayerIndex(reinterpret_cast<uintptr_t>(self));
    if (index == 0 && amount > 0 && g_hitLog.load(std::memory_order_relaxed))
        LogHealthLoss(reinterpret_cast<uintptr_t>(self), amount, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if (index >= 0 && amount > 0) LogDamageStack(index, amount);
    if (index >= 0 && amount > 0 &&
        (g_godPlayer[index].load(std::memory_order_relaxed) || g_invinciblePlayer[index].load(std::memory_order_relaxed)))
        return;
    if (index == 0 && amount > 0 && (GrabGraceRefusesDamage() || GrabRescueActive())) return;
    if (index == 0 && amount > 0 && PerfectDodgeHit(reinterpret_cast<uintptr_t>(self), false)) return;
    if (index == 0 && amount > 0 && WeskerMoveInvulnerable(reinterpret_cast<uintptr_t>(self))) return;
    if (index == 0 && amount > 0 && ParryHit(reinterpret_cast<uintptr_t>(self), false)) return;
    const bool kills = index == 0 && amount > 0 && Re6DyingHitKills();
    g_playerSubHealth(self, edx, amount);
    if (kills) mem::Write(reinterpret_cast<uintptr_t>(self) + 0x1BAC, 0.0f);  // no time left: the update kills
}

// uPlayer hit reaction, RVA 0x77B730 (void __thiscall, no stack args). Called from the per-frame player
// update when a hit is pending; picks the damage action (0x85, 0x86, 0x89...) — the same code the
// Ultimate Trainer's auto-dodge rewrites. Skipping it for a player = the hit is ignored.
bool HookAt(uintptr_t rva, const uint8_t* expected, size_t n, void* detour, void** original, const char* what);
bool EnsureDamageHook();
bool EnsureHitHook();

using HitReactionFn = void(__fastcall*)(void* self, void* edx);
HitReactionFn g_hitReaction = nullptr;
std::atomic<uint32_t> g_hitsIgnored{ 0 };

void __fastcall HookHitReaction(void* self, void* edx) {
    const int index = PlayerIndex(reinterpret_cast<uintptr_t>(self));
    if (index >= 0 && g_invinciblePlayer[index].load(std::memory_order_relaxed)) {
        if (g_hitsIgnored.fetch_add(1) < 20) LOG("hit ignored for P%d", index + 1);
        return;
    }
    const bool log = index == 0 && g_hitLog.load(std::memory_order_relaxed);
    if (log) LogPendingHit(reinterpret_cast<uintptr_t>(self));
    if (index == 0 && PerfectDodgeHit(reinterpret_cast<uintptr_t>(self), true)) {
        if (log) LOG("hit log: perfect dodge");
        return;
    }
    if (index == 0 && WeskerMoveInvulnerable(reinterpret_cast<uintptr_t>(self))) {
        if (log) LOG("hit log: ignored (Wesker move)");
        return;
    }
    if (index == 0 && ParryHit(reinterpret_cast<uintptr_t>(self), true)) {
        if (log) LOG("hit log: parried");
        return;
    }
    g_hitReaction(self, edx);
    if (index == 0) NotePlayer1BlowTaken(reinterpret_cast<uintptr_t>(self));  // no late parry of it (parry.cpp)
    uint32_t action = 0;
    if (log && mem::Read(reinterpret_cast<uintptr_t>(self) + 0x10E0, action)) LOG("hit log: after the reaction, action %X", action);
}

// ---- Invincibility: hide the player number from the enemy-facing components --------------------
// Every frame the player copies team/number (P+0x1358/+0x135C) into two components that enemies use to
// target and hit it: [P+0x2770]+0xCC/+0xD0 (RVA 0x767701..0x76771F) and [P+0x2890]+0xCC/+0xD0
// (RVA 0x768BB1..0x768BC9). The Ultimate Trainer zeroes P+0x135C itself, which also breaks the owner of
// the player's own attacks (enemies become invincible). We only make those two copies read 0 for an
// invincible player (user-verified that number 0 removes impact, blood/sound and grabs).
//
// Two 6-byte loads are redirected to small code caves:
//   mov eax,[esi+0x135C] @0x767713  /  mov ecx,[esi+0x135C] @0x768BBD
// cave: original load; if esi == slot0 or esi == slot1 -> reg = 0; jmp back.
// The slots live inside the cave allocation (not in the DLL), so the caves stay valid if the DLL unloads.
struct InvincibilityCaves {
    uint8_t* block = nullptr;
    volatile uint32_t* slot = nullptr;  // slot[0], slot[1]: invincible player objects (0 = none)
    mem::Patch* site1 = nullptr;
    mem::Patch* site2 = nullptr;
} g_caves;

void EmitCave(uint8_t* at, uint8_t loadModrm, uint8_t xorOpcode2, uintptr_t back, volatile uint32_t* slots) {
    std::vector<uint8_t> c = { 0x8B, loadModrm, 0x5C, 0x13, 0x00, 0x00 };  // mov r32,[esi+0x135C]
    auto cmpSlot = [&](volatile uint32_t* a) {                           // cmp esi,[abs32]
        uint32_t v = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a));
        c.insert(c.end(), { 0x3B, 0x35 });
        c.insert(c.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4);
    };
    cmpSlot(&slots[0]);
    c.insert(c.end(), { 0x74, 0x08 });  // je zero
    cmpSlot(&slots[1]);
    c.insert(c.end(), { 0x75, 0x02 });  // jne back
    c.insert(c.end(), { 0x33, xorOpcode2 });  // zero: xor r32,r32
    c.push_back(0xE9);                        // back: jmp original continuation
    int32_t rel = static_cast<int32_t>(back - (reinterpret_cast<uintptr_t>(at) + c.size() + 4));
    c.insert(c.end(), reinterpret_cast<uint8_t*>(&rel), reinterpret_cast<uint8_t*>(&rel) + 4);
    memcpy(at, c.data(), c.size());
}

std::vector<uint8_t> JumpTo(uintptr_t from, uint8_t* to) {
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (from + 5));
    std::vector<uint8_t> j = { 0xE9 };
    j.insert(j.end(), reinterpret_cast<uint8_t*>(&rel), reinterpret_cast<uint8_t*>(&rel) + 4);
    j.push_back(0x90);
    return j;
}

bool EnsureInvincibilityCaves() {
    if (g_caves.site1) return true;
    auto* block = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!block) return false;
    g_caves.block = block;
    g_caves.slot = reinterpret_cast<volatile uint32_t*>(block + 0xF0);
    g_caves.slot[0] = g_caves.slot[1] = 0;
    uint8_t* a1 = mem::Rva(0x767713);
    uint8_t* a2 = mem::Rva(0x768BBD);
    EmitCave(block, 0x86, 0xC0, reinterpret_cast<uintptr_t>(a1) + 6, g_caves.slot);         // eax
    EmitCave(block + 0x40, 0x8E, 0xC9, reinterpret_cast<uintptr_t>(a2) + 6, g_caves.slot);  // ecx
    FlushInstructionCache(GetCurrentProcess(), block, 0x100);
    static mem::Patch p1("invincible.copy1", a1, { 0x8B, 0x86, 0x5C, 0x13, 0x00, 0x00 }, JumpTo(reinterpret_cast<uintptr_t>(a1), block));
    static mem::Patch p2("invincible.copy2", a2, { 0x8B, 0x8E, 0x5C, 0x13, 0x00, 0x00 },
                         JumpTo(reinterpret_cast<uintptr_t>(a2), block + 0x40));
    g_caves.site1 = &p1;
    g_caves.site2 = &p2;
    LOG("invincibility caves at %p", block);
    return true;
}

// The damage hit-shapes: P+0x1290 is a shape list (array [list+4], count [list+0x18]). 0xC43D80(team, number)
// (RVA 0x843D80, 5 callers incl. inline copies of SetDamageCollision) writes team into type-0 shapes
// (+0xCC) and the player number into type-1 shapes (+0xD0). Enemy attacks test those numbers: 0 = not
// hittable (no impact, no blood/sound, no grab). Only the shapes get 0; the player keeps its number.
constexpr uintptr_t kHitShapes = 0x1290;
using SetShapeGroupFn = void(__fastcall*)(void* list, void* edx, int team, int number);
SetShapeGroupFn g_setShapeGroup = nullptr;

void __fastcall HookSetShapeGroup(void* list, void* edx, int team, int number) {
    for (int i = 0; i < 2; ++i)
        if ((g_invinciblePlayer[i].load(std::memory_order_relaxed) || (i == 0 && g_dodgeInvincible.load(std::memory_order_relaxed))) &&
            list && reinterpret_cast<uintptr_t>(list) == Player(i) + kHitShapes)
            number = 0;
    g_setShapeGroup(list, edx, team, number);
}

void WriteShapeNumbers(uintptr_t player, uint32_t number) {
    if (!player) return;
    const uintptr_t list = player + kHitShapes;
    uintptr_t array = 0;
    uint32_t count = 0;
    if (!mem::Read(list + 4, array) || !mem::Read(list + 0x18, count) || !array || count > 256) return;
    for (uint32_t k = 0; k < count; ++k) {
        uintptr_t shape = 0;
        uint32_t type = 0;
        if (mem::Read(array + 4 * k, shape) && shape && mem::Read(shape + 0xC4, type) && type == 1)
            mem::Write(shape + 0xD0, number);
    }
}

void UpdateInvincibleSlots() {
    if (!g_caves.slot) return;
    for (int i = 0; i < 2; ++i) {
        const bool on = g_invinciblePlayer[i].load() || (i == 0 && g_dodgeInvincible.load());
        g_caves.slot[i] = on ? static_cast<uint32_t>(Player(i)) : 0;
        if (on) WriteShapeNumbers(Player(i), 0);
    }
}

bool SetInvincible(int i, bool on) {
    if (on) {
        if (!EnsureDamageHook() || !EnsureHitHook() || !EnsureInvincibilityCaves()) return false;
        if (!mem::ApplyAll({ g_caves.site1, g_caves.site2 })) return false;
    }
    g_invinciblePlayer[i] = on;
    UpdateInvincibleSlots();
    if (!on && !(i == 0 && g_dodgeInvincible.load())) {  // give the shapes their real number back
        uint32_t number = 0;
        if (uintptr_t p = Player(i); p && mem::Read(p + 0x135C, number)) WriteShapeNumbers(p, number);
    }
    if (!g_invinciblePlayer[0] && !g_invinciblePlayer[1] && !g_dodgeInvincible.load() && g_caves.site1)
        mem::RestoreAll({ g_caves.site2, g_caves.site1 });
    return true;
}

// Player 1 hidden from the enemies' blows (on) or back as the "Invincible" option leaves him (SetDodgeInvincible).
bool DodgeInvincible(bool on) {
    if (on == g_dodgeInvincible.load()) {
        // Held for long (the automatic dodges): the player object changes on a level load.
        if (on && g_caves.slot && g_caves.slot[0] != static_cast<uint32_t>(Player(0))) UpdateInvincibleSlots();
        return true;
    }
    if (on) {
        if (!EnsureHitHook() || !EnsureInvincibilityCaves()) return false;
        if (!mem::ApplyAll({ g_caves.site1, g_caves.site2 })) return false;
    }
    g_dodgeInvincible = on;
    UpdateInvincibleSlots();
    if (!on && !g_invinciblePlayer[0].load()) {
        uint32_t number = 0;
        if (uintptr_t p = Player(0); p && mem::Read(p + 0x135C, number)) WriteShapeNumbers(p, number);
    }
    if (!g_invinciblePlayer[0] && !g_invinciblePlayer[1] && !g_dodgeInvincible.load() && g_caves.site1)
        mem::RestoreAll({ g_caves.site2, g_caves.site1 });
    return true;
}

bool g_hitHookInstalled = false;

bool EnsureHitHook() {
    if (g_hitHookInstalled) return true;
    const uint8_t reaction[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00 };
    const uint8_t group[] = { 0x56, 0x33, 0xF6, 0x39, 0x71, 0x18, 0x76, 0x3C };
    if (!HookAt(0x77B730, reaction, sizeof(reaction), &HookHitReaction, reinterpret_cast<void**>(&g_hitReaction), "hit reaction") ||
        !HookAt(0x843D80, group, sizeof(group), &HookSetShapeGroup, reinterpret_cast<void**>(&g_setShapeGroup), "hit-shape group"))
        return false;
    g_hitHookInstalled = true;
    return true;
}

bool HookAt(uintptr_t rva, const uint8_t* expected, size_t n, void* detour, void** original, const char* what) {
    uint8_t* target = mem::Rva(rva);
    if (!mem::IsGameCode(target, n) || memcmp(target, expected, n) != 0) {
        LOG("%s hook refused: unexpected bytes %s", what, mem::Hex(target, n).c_str());
        return false;
    }
    // Another trainer may have left this code page PAGE_READWRITE; MinHook only hooks executable pages.
    mem::EnsureExecutable(target, n);
    MH_STATUS created = MH_CreateHook(target, detour, original);
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) {
        LOG("%s hook: MH_CreateHook %s", what, MH_StatusToString(created));
        return false;
    }
    MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
        LOG("%s hook: MH_EnableHook %s", what, MH_StatusToString(enabled));
        return false;
    }
    LOG("%s hook installed at %p", what, target);
    return true;
}

bool EnsureDamageHook() {
    if (g_damageHookInstalled) return true;
    const uint8_t base[] = { 0x66, 0x8B, 0x44, 0x24, 0x04, 0x66, 0x29, 0x81, 0x64, 0x13, 0x00, 0x00 };
    const uint8_t player[] = { 0x56, 0x8B, 0xF1, 0x66, 0x8B, 0x4C, 0x24, 0x08, 0x57, 0x0F, 0xBF, 0xBE, 0x64, 0x13, 0x00, 0x00 };
    if (!HookAt(0x44F7C0, base, sizeof(base), &HookSubHealth, reinterpret_cast<void**>(&g_subHealth), "actor damage") ||
        !HookAt(0x77C950, player, sizeof(player), &HookPlayerSubHealth, reinterpret_cast<void**>(&g_playerSubHealth), "player damage"))
        return false;
    g_damageHookInstalled = true;
    return true;
}

// ---- Player 2 does not shoot (user, 2026-10-14, first on the truck turret). A gun fires in two steps, both virtual:
// Fire (vtable slot 111, thiscall with 3 arguments, called by the weapon's update when a shot request is pending,
// RVA 0x795FA4) creates the bullet/shell, then calls Shot (slot 113, RVA 0x7980E0 for every gun but the Gatling:
// effects and the firing flag +0x1C41). Blocking Shot alone left the bullets (truck turret, 2026-10-14: 147 shots
// refused, Sheva still hit the Gigante). Fire has 12 implementations (weapon families, turrets RVA 0x7B9450); each
// gets its own detour. A mounted gun's unit is attached to the vehicle, not to the player: the weapons of player 2
// are recognised by its inventory records (+0x2C unit). Two options (user): hand weapons (with the melee block) and
// mounted guns (items 0x012C-0x012F / 0x0131), kept apart because Chris alone cannot stagger the truck Gigante.
using ShotFn = void(__fastcall*)(void* unit, void* edx);
using FireFn = void(__fastcall*)(void* unit, void* edx, uintptr_t a, uintptr_t b, uintptr_t c);

struct FireSite {
    uintptr_t rva;
    uint8_t prologue[6];
};
// Fire implementations found through the vtables whose slot 113 is a Shot (the stub "ret 0Ch" of the knives excluded).
constexpr FireSite kFireSites[] = {
    { 0x797BA0, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } },  // common (handguns, rifles, shotguns...)
    { 0x798F70, { 0xF3, 0x0F, 0x10, 0x05, 0xA8, 0xCB } },
    { 0x79B0B0, { 0x8B, 0x01, 0x8B, 0x90, 0xC4, 0x01 } },
    { 0x7B61F0, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } },
    { 0x7B6E90, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } },
    { 0x7B82C0, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } },
    { 0x7B8D30, { 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0xBE } },
    { 0x7B9450, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } },  // mounted guns (truck, jeep, emplacement)
    { 0x7BBB20, { 0x56, 0x8B, 0xF1, 0x8B, 0x0D, 0xDC } },
    { 0x7BBB60, { 0x83, 0xA1, 0x50, 0x01, 0x00, 0x00 } },
    { 0x7BC770, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } },
    { 0x7BDE20, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } },
};
constexpr size_t kFireCount = sizeof(kFireSites) / sizeof(kFireSites[0]);

ShotFn g_shot = nullptr;
FireFn g_fire[kFireCount] = {};
std::atomic<bool> g_p2NoShot{ false };    // hand weapons
std::atomic<bool> g_p2NoTurret{ false };  // mounted guns

bool IsMountedGun(uint32_t item) {
    return item == 0x012C || item == 0x012D || item == 0x012E || item == 0x012F || item == 0x0131;
}

// Item id of one of the player's weapon units, 0 when it is the held weapon without a record, -1 when not its own.
int ItemOfPlayerUnit(uintptr_t unit, int index) {
    const uintptr_t p = Player(index);
    if (!p || !unit) return -1;
    for (int k = 0; k < 24; ++k) {
        uintptr_t recordUnit = 0;
        uint32_t item = 0;
        if (mem::Read(p + 0x21A4 + 0x30 * k + 0x2C, recordUnit) && recordUnit == unit)
            return mem::Read(p + 0x21A4 + 0x30 * k + 4, item) ? static_cast<int>(item & 0xFFFF) : 0;
    }
    uintptr_t held = 0;
    return mem::Read(p + kHeldWeapon, held) && held == unit ? 0 : -1;
}

// (2026-11-06: a log of her shots showed only some of them going through these functions - her fire action 0x1E is
// refused in action_log.cpp too.)
bool ShotRefused(void* unit) {
    const bool free = FinalFightPartnerFree();  // final_fight.cpp
    const bool hand = g_p2NoShot.load(std::memory_order_relaxed) && !free,
               mounted = g_p2NoTurret.load(std::memory_order_relaxed) && !free;
    if (!hand && !mounted) return false;
    const int item = ItemOfPlayerUnit(reinterpret_cast<uintptr_t>(unit), 1);
    if (item < 0) return false;
    return IsMountedGun(static_cast<uint32_t>(item)) ? mounted : hand;
}

void __fastcall HookShot(void* unit, void* edx) {
    if (ShotRefused(unit)) return;
    g_shot(unit, edx);
}

template <size_t I>
void __fastcall HookFire(void* unit, void* edx, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (ShotRefused(unit)) return;
    g_fire[I](unit, edx, a, b, c);
}

template <size_t... I>
void* FireDetour(size_t index, std::index_sequence<I...>) {
    void* const detours[] = { reinterpret_cast<void*>(&HookFire<I>)... };
    return detours[index];
}

bool EnsureShotHooks() {
    static bool hooked = false;
    if (hooked) return true;
    const uint8_t shot[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x24, 0x53 };
    if (!HookAt(0x7980E0, shot, sizeof(shot), reinterpret_cast<void*>(&HookShot), reinterpret_cast<void**>(&g_shot),
                "weapon shot"))
        return false;
    int count = 0;
    for (size_t i = 0; i < kFireCount; ++i) {
        char what[32];
        sprintf_s(what, "weapon fire %06X", static_cast<unsigned>(kFireSites[i].rva));
        count += HookAt(kFireSites[i].rva, kFireSites[i].prologue, sizeof(kFireSites[i].prologue),
                        FireDetour(i, std::make_index_sequence<kFireCount>{}), reinterpret_cast<void**>(&g_fire[i]), what);
    }
    LOG("weapon fire: %d of %d implementations hooked", count, static_cast<int>(kFireCount));
    hooked = count > 0;
    return hooked;
}

// ---- Truck turret power (user, 2026-10-14): with Sheva's turret shots blocked, Chris alone could not stagger the
// Gigante of the truck (chapter 2-3, uEm45 + its parasites uEm4502), the only hard lock of the game in that setup.
// The weapon table power (Chris 150, Sheva 300) is not read when shooting; the Gigante takes damage through its own
// virtual Damage (vtable slot 129, thiscall(int shape, int amount), ret 8): body RVA 0x4E19E0 (hits on the body:
// 88 per turret bullet), parasite RVA 0x4E14F0 (110 per bullet: parasite health int +0x1F50 = 8000, and the body's
// health float +0x1F50 / gauge +0x1F54 too). The body staggers each time its health passes the next threshold (gauge =
// health - threshold, e.g. 39698 - 33808 = 5890): multiplying the amounts multiplies the stopping power. Applied only
// while a player holds a truck turret (items 0x012D / 0x012E).
using EnemyDamageFn = int(__fastcall*)(void* self, void* edx, int shape, int amount);
EnemyDamageFn g_giganteDamage = nullptr;
EnemyDamageFn g_parasiteDamage = nullptr;
std::atomic<bool> g_truckPowerOn{ false };
std::atomic<float> g_truckPower{ 2.0f };

bool HoldsTruckTurret() {
    for (int index = 0; index < 2; ++index) {
        const uintptr_t p = Player(index);
        uintptr_t held = 0;
        if (!p || !mem::Read(p + kHeldWeapon, held) || !held) continue;
        for (int k = 0; k < 24; ++k) {
            uint32_t item = 0;
            uintptr_t unit = 0;
            if (mem::Read(p + 0x21A4 + 0x30 * k + 0x2C, unit) && unit == held && mem::Read(p + 0x21A4 + 0x30 * k + 4, item) &&
                ((item & 0xFFFF) == 0x012D || (item & 0xFFFF) == 0x012E))
                return true;
        }
    }
    return false;
}

int TruckDamage(int amount) {
    if (!g_truckPowerOn.load(std::memory_order_relaxed) || amount <= 0 || !HoldsTruckTurret()) return amount;
    const float scaled = static_cast<float>(amount) * g_truckPower.load(std::memory_order_relaxed);
    return scaled > 1e6f ? 1000000 : static_cast<int>(scaled);
}

int __fastcall HookGiganteDamage(void* self, void* edx, int shape, int amount) {
    return g_giganteDamage(self, edx, shape, TruckDamage(amount));
}

int __fastcall HookParasiteDamage(void* self, void* edx, int shape, int amount) {
    return g_parasiteDamage(self, edx, shape, TruckDamage(amount));
}

bool EnsureGiganteHooks() {
    static bool hooked = false;
    if (hooked) return true;
    const uint8_t body[] = { 0x0F, 0xB7, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x18, 0x57 };
    const uint8_t parasite[] = { 0xF6, 0x81, 0xB4, 0x1B, 0x00, 0x00, 0x01, 0x74, 0x5F };
    hooked = HookAt(0x4E19E0, body, sizeof(body), reinterpret_cast<void*>(&HookGiganteDamage),
                    reinterpret_cast<void**>(&g_giganteDamage), "gigante damage") &&
             HookAt(0x4E14F0, parasite, sizeof(parasite), reinterpret_cast<void*>(&HookParasiteDamage),
                    reinterpret_cast<void**>(&g_parasiteDamage), "gigante parasite damage");
    return hooked;
}

// ---- Invisible player 2 (user, 2026-10-14): unit flags +5 bit 0x08 = drawn (the same bit the weapons use: a held
// weapon has it, a holstered one not). Cleared on Sheva (verified live: invisible, still moving and acting; the
// game does not set it back) and on her drawn weapon units (held, on the back) every frame while on; switching off
// draws her again and the weapons she holds or carries on her back.
std::vector<uintptr_t> g_hiddenUnits;
std::atomic<bool> g_p2Invisible{ false };
// "Combat final : Jill en partenaire" (final_fight.cpp): the partner shown in the final fight whatever this option.
bool P2Hidden() { return g_p2Invisible.load(std::memory_order_relaxed) && !FinalFightPartnerFree(); }
bool g_p2HiddenNow = false;  // HidePlayer2 ran on the players of now

// Her health gauge (user): each player has a HUD uIdCockpit00 (vtable RVA 0xB9E480); RVA 0x5ED920 (thiscall(player),
// called every frame) sets +0x1A0 = the player and +0x1A4 = its partner when valid (gauge blocks of 0x54 at +0xEC,
// RVA 0x5EDA80). Every partner element of the HUD checks +0x1A4 first (RVA 0x3E6370): emptied on player 1's HUD
// while she is invisible.
using CockpitPlayersFn = void(__fastcall*)(void* cockpit, void* edx, uintptr_t player);
CockpitPlayersFn g_cockpitPlayers = nullptr;

// Player 1's HUD (stamina.cpp reads its pulse counter +0x1BC).
std::atomic<uintptr_t> g_p1Cockpit{ 0 };
std::atomic<DWORD> g_p1CockpitTick{ 0 };  // its last update (knife_icon.cpp: no HUD, no icon)

void __fastcall HookCockpitPlayers(void* cockpit, void* edx, uintptr_t player) {
    g_cockpitPlayers(cockpit, edx, player);
    if (player && player == Player(0)) {
        g_p1Cockpit = reinterpret_cast<uintptr_t>(cockpit);
        const DWORD now = GetTickCount();
        g_p1CockpitTick = now ? now : 1;
    }
    if (P2Hidden() && player && player == Player(0))
        mem::Write<uintptr_t>(reinterpret_cast<uintptr_t>(cockpit) + 0x1A4, 0);
}

// She still blocked Chris (user, 2026-10-15). Characters are kept apart by the component cObjAdj at P+0x11E0 (vtable
// RVA 0xC433F8): its resolve (RVA 0x866AA0) and the movement code (RVA 0x8E4540) walk the contacts of its sensors
// through GetContact (RVA 0x867F10, thiscall(index) ret 4: sensor + (index + 3) * 0x40, null past 8); a contact holds
// the actor touched at +0x30 (copied to cObjAdj +0xA0) and its cObjAdj at +0x34. Both callers skip a null contact:
// contacts with player 2 come back null while it is invisible, so nobody is pushed by it (it still steps aside).
// Also the Majini in Jill's throw (wesker_moves.cpp): put on player 1's origin as Jill is in her Caught, it pushed him
// 40-80 cm away at once and the throw played apart (user 2026-10-22); and with the beta back throw on (beta_anims.cpp)
// any enemy in the back escape BackHoldDmg 0x9C (user: that throw jammed too).
using GetContactFn = void*(__fastcall*)(void* sensor, void* edx, uint32_t index);
GetContactFn g_getContact = nullptr;

// The thrown unit's own sensors (user, 2026-10-31: HUNK's neck break - "the Majini backs off as if pushed, it must stay
// in place from the start of Chris's motion"): put on player 1's origin, its sensors saw him and pushed it away. A
// cObjAdj (unit +0x11E0) keeps its sensors at +0x8C (count +0x98; resolve VA 0xC66AA0), each its contacts (+0x2C0).
bool OwnSensor(uintptr_t unit, const void* sensor) {
    uintptr_t sensors = 0;
    uint32_t count = 0;
    if (!mem::Read(unit + 0x11E0 + 0x8C, sensors) || !sensors || !mem::Read(unit + 0x11E0 + 0x98, count)) return false;
    for (uint32_t i = 0; i < std::min<uint32_t>(count, 16); ++i) {
        uintptr_t s = 0;
        if (mem::Read(sensors + 4 * i, s) && s == reinterpret_cast<uintptr_t>(sensor)) return true;
    }
    return false;
}

void* __fastcall HookGetContact(void* sensor, void* edx, uint32_t index) {
    void* contact = g_getContact(sensor, edx, index);
    if (!contact) return contact;
    const uintptr_t thrown = WeskerThrownUnit();
    const bool beta = BetaBackThrowOn(), p2 = P2Hidden();
    if (!thrown && !beta && !p2) return contact;
    if (thrown && OwnSensor(thrown, sensor)) return nullptr;  // nobody pushes it
    uintptr_t actor = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(contact) + 0x30, actor) || !actor) return contact;
    if (actor == thrown) return nullptr;
    if (p2 && actor == Player(1)) return nullptr;
    uint32_t action = 0;
    if (beta && mem::Read(actor + 0x10E0, action) && action == 0x9C && PlayerIndex(actor) < 0) return nullptr;
    return contact;
}

bool EnsureContactHook() {
    static bool hooked = false;
    if (!hooked) {
        const uint8_t prologue[] = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xF8, 0x08 };
        hooked = HookAt(0x867F10, prologue, sizeof(prologue), reinterpret_cast<void*>(&HookGetContact),
                        reinterpret_cast<void**>(&g_getContact), "cObjAdj contact");
    }
    return hooked;
}

// Its voice (user, 2026-10-26: "mute the partner when the option makes it invisible"): a player's lines are played by
// Player::PlayVoice (RVA 0x777220, thiscall(voice bank, line, priority, flags) ret 0x10; the bank P+0x2EA8, e.g.
// sound\bgm_dc\emo_vo_17_j for Jill; called by RVA 0x77AB60, the players' manager's dispatch RVA 0x3E72B6, and RVA
// 0x3D9E62), which returns 0 for a line it skips: player 2's lines are skipped while it is invisible.
using PlayVoiceFn = int(__fastcall*)(void* player, void* edx, void* bank, int line, int priority, int flags);
PlayVoiceFn g_playVoice = nullptr;

int __fastcall HookPlayVoice(void* player, void* edx, void* bank, int line, int priority, int flags) {
    if (P2Hidden() && player && reinterpret_cast<uintptr_t>(player) == Player(1)) return 0;
    // A borrowed boss move's carrier is the speech gesture (wesker_moves.cpp): neither the call nor the answer.
    if (BossMoveSilencesVoices() && player &&
        (reinterpret_cast<uintptr_t>(player) == Player(0) || reinterpret_cast<uintptr_t>(player) == Player(1)))
        return 0;
    return g_playVoice(player, edx, bank, line, priority, flags);
}

bool EnsureVoiceHook() {
    static bool hooked = false;
    if (!hooked) {
        // push ebp; mov ebp, esp; and esp, -16; sub esp, 0x34
        const uint8_t prologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x34 };
        hooked = HookAt(0x777220, prologue, sizeof(prologue), reinterpret_cast<void*>(&HookPlayVoice),
                        reinterpret_cast<void**>(&g_playVoice), "player voice");
    }
    return hooked;
}

bool EnsureCockpitHook() {
    static bool hooked = false;
    if (!hooked) {
        const uint8_t prologue[] = { 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x33, 0xC0 };
        hooked = HookAt(0x5ED920, prologue, sizeof(prologue), reinterpret_cast<void*>(&HookCockpitPlayers),
                        reinterpret_cast<void**>(&g_cockpitPlayers), "cockpit players");
    }
    return hooked;
}

void HidePlayer2() {
    const uintptr_t p = Player(1);
    if (!p) return;
    uint8_t flags = 0;
    if (mem::Read(p + 5, flags) && (flags & 0x08)) mem::Write<uint8_t>(p + 5, flags & ~0x08);
    for (int k = 0; k < 24; ++k) {
        uintptr_t unit = 0;
        if (!mem::Read(p + 0x21A4 + 0x30 * k + 0x2C, unit) || !unit) continue;
        if (mem::Read(unit + 5, flags) && (flags & 0x08)) {
            mem::Write<uint8_t>(unit + 5, flags & ~0x08);
            if (std::find(g_hiddenUnits.begin(), g_hiddenUnits.end(), unit) == g_hiddenUnits.end()) g_hiddenUnits.push_back(unit);
        }
    }
}

void ShowPlayer2() {
    const uintptr_t p = Player(1);
    if (p) {
        uint8_t flags = 0;
        if (mem::Read(p + 5, flags)) mem::Write<uint8_t>(p + 5, flags | 0x08);
        uintptr_t held = 0;
        mem::Read(p + kHeldWeapon, held);
        for (int k = 0; k < 24; ++k) {
            uintptr_t unit = 0;
            if (!mem::Read(p + 0x21A4 + 0x30 * k + 0x2C, unit) || !unit) continue;
            if (std::find(g_hiddenUnits.begin(), g_hiddenUnits.end(), unit) == g_hiddenUnits.end()) continue;
            uint32_t joint = 0;
            mem::Read(unit + 0x1C, joint);
            // held weapon, or a weapon on the back (joint 0x75 male / 2 female): drawn again
            if ((unit == held || joint == 0x75 || joint == 2) && mem::Read(unit + 5, flags)) mem::Write<uint8_t>(unit + 5, flags | 0x08);
        }
    }
    g_hiddenUnits.clear();
}

Feature& AddFlagToggle(const char* id, const char* name, const char* help, const char* category, std::atomic<bool>& flag) {
    return Add({
        .id = id, .name = name, .help = help, .category = category, .kind = Kind::Toggle,
        .set = [&flag](bool on) {
            if (on && !EnsureDamageHook()) return false;
            flag = on;
            return true;
        },
        .available = [] { return game::IsSupported(); },
    });
}

// "Vie max du joueur 1 / 2" (user, 2026-10-26: "a slider like the stamina's"): the player's max health +0x1366 (int16,
// the game's 1000; the HUD ring is health / it, VA 0x9EDB5F) set to the slider's value every frame in play. The ring is
// a share that never moves without damage (user): a change of ours (slider, tick, untick) scales his health with it;
// the game putting its own max back (18:13:20, a checkpoint or menu: his 600 of 600 was then read as 600 of 1000 and
// cut to 360) only gets the max rewritten, his health kept, or full again if the game filled it. A dying 1 stays 1.
struct MaxHealth {
    std::atomic<bool> on{ false };
    std::atomic<float> wanted{ 1000.0f };
    uintptr_t actor = 0;   // render thread
    int16_t gameMax = 0;   // the game's own max for that actor
    int16_t written = 0;   // the max we last wrote on it (0: none)
};
MaxHealth g_maxHealth[2];

void ScaleHealth(uintptr_t p, int16_t from, int16_t to) {
    int16_t health = 0;
    if (from <= 0 || !mem::Read(p + kHealth, health) || health <= 1) return;
    const int scaled = static_cast<int>((static_cast<long long>(health) * to + from / 2) / from);
    mem::Write<int16_t>(p + kHealth, static_cast<int16_t>(std::clamp(scaled, 2, static_cast<int>(to))));
    if (p == actors::Player(0)) StaminaHealthRescaled();
}

void TickMaxHealth(int index) {
    MaxHealth& m = g_maxHealth[index];
    const uintptr_t p = actors::InGame() ? Player(index) : 0;
    int16_t max = 0, health = 0;
    if (!p || !mem::Read(p + kHealthMax, max) || max <= 0) return;
    if (p != m.actor) {
        m.actor = p;
        m.gameMax = max;
        m.written = 0;
    }
    if (!m.on.load(std::memory_order_relaxed)) return;
    const int16_t wanted = static_cast<int16_t>(std::clamp(m.wanted.load(std::memory_order_relaxed), 100.0f, 30000.0f));
    if (max == wanted) {
        m.written = wanted;
        return;
    }
    if (!m.written || max == m.written) {
        ScaleHealth(p, max, wanted);  // ours: the slider moved or the option starts
        LOG("max health: player %d %d -> %d (health scaled)", index + 1, max, wanted);
    } else if (mem::Read(p + kHealth, health)) {
        // The game's own max back: his health is ours still, unless the game filled it.
        if (health >= max && health > 1) mem::Write<int16_t>(p + kHealth, wanted);
        else if (health > wanted) mem::Write<int16_t>(p + kHealth, wanted);
        if (index == 0) StaminaHealthRescaled();
        LOG("max health: the game put player %d's max to %d, %d again (health %d kept%s)", index + 1, max, wanted, health,
            health >= max ? ", full" : "");
    }
    mem::Write<int16_t>(p + kHealthMax, wanted);
    m.written = wanted;
}

// Unticked or at unload (a reloaded trainer would take ours for the game's): the game's max back, his health scaled
// (ticks only run while a feature is on).
void RestoreMaxHealth(int index) {
    MaxHealth& m = g_maxHealth[index];
    int16_t max = 0;
    const uintptr_t p = actors::InGame() ? Player(index) : 0;
    m.written = 0;
    if (!p || p != m.actor || m.gameMax <= 0 || !mem::Read(p + kHealthMax, max) || max == m.gameMax) return;
    ScaleHealth(p, max, m.gameMax);
    mem::Write<int16_t>(p + kHealthMax, m.gameMax);
    LOG("max health: player %d back to the game's %d", index + 1, m.gameMax);
}

// "Replacer les accessoires" (user, 2026-10-26: "after an RE7 parry the rifle on his back and the knife sheath sat
// lower and further forward, in his body; put them back automatically"). The motions the trainer borrows (Wesker's
// guard and moves, Jill's: other skeletons' lists) also drive joints the character's own motions never animate -
// on Chris the back weapon's 117, the holsters and pouches 102-122, two on the chest 56 / 57 - so they kept that
// pose until the stage reloaded (read live: joint 117's local translation (3.0, -3.7, 6.8) for its bind (0, -1.5, 0);
// the same leftovers as RE6's finger tracks, MODLOG session 16). uModel::Joint [P+0x318], 0x90 each: +5 parent index,
// +7 id, +0x10 bind offset, +0x20 local rotation (quaternion), +0x30 scale, +0x40 local translation, +0x50 world
// matrix. Each motion layer also keeps the last evaluated pose of each joint in its slot (layer record +0xD8, 0x60
// per joint: three track entries, pointer at +0 and weight +0xC; rotation +0x30, translation +0x40, scale +0x50) and
// applies it every frame: with no track it is never evaluated again (Wesker's pose in layers 0, 6 and 7 for 117).
// Per character (user: "with Jill it must be Jill's, not Chris's"): while player 1 plays his own motions his joints'
// pose is copied every frame; when a borrowed move ends (the carrier action 0x81, layer 0 banks 7 / B-F), for a
// second (the blend out, its other layer still driving them), the joints no layer has a track for get that pose
// back, in the joint and in the layers' slots. Not every frame (some may be driven by the game's physics).
std::atomic<bool> g_accessoryFix{ true };
int g_accessoryFrames = 0;       // render thread: frames left to put them back
bool g_accessoryForeign = false;
uintptr_t g_accessoryJoints = 0; // the joint array the pose below is of
std::vector<uint8_t> g_accessoryPose;  // its joints' +0x20..+0x4F (rotation, scale, translation), 0x30 each

void TickAccessoryJoints() {
    const uintptr_t p = actors::InGame() ? Player(0) : 0;
    uintptr_t joints = 0;
    uint32_t action = 0, motion = 0, count = 0;
    if (!p || !mem::Read(p + 0x10E0, action) || !mem::Read(p + 0x384, motion) || !mem::Read(p + 0x318, joints) ||
        !joints || !mem::Read(p + 0x314, count) || count == 0 || count > 256)
        return;
    const uint32_t bank = (motion >> 8) & 0xFF;
    // Wesker's / Jill's lists (B-E), the DLC's (7) and the beta grabs' (F): the moves the trainer borrows. Also the knife
    // variants' own motions (0x0123 / 0x0124's lists; user, 2026-11-05: "Chris's sheath moved forward into his body
    // once I used the variant": read live, joints 102 / 103 / 108-115 / 118-122 far from their binds, no layer's track
    // on them): their knife actions (0x22-0x27) count as borrowed.
    const uint16_t held = PlayerHeldItem(p);
    const bool variantKnife = action >= 0x22 && action <= 0x27 && (held == 0x0123 || held == 0x0124);
    // And any move with another character's moveset lent (user, 2026-11-06: "Wesker's dash moves Chris's sheath like the
    // parry"): the dash (knee bazooka 0x47-0x4D) plays Wesker's list from bank 1.
    const bool foreign = action == 0x81 || bank == 7 || (bank >= 0xB && bank <= 0xF) || variantKnife ||
                         (action >= 0x47 && action <= 0x4D) || Player1MovesetLent();
    if (g_accessoryForeign && !foreign) g_accessoryFrames = 60;
    g_accessoryForeign = foreign;
    if (foreign) return;
    std::vector<uint8_t> now(static_cast<size_t>(count) * 0x90);
    if (!mem::SafeRead(reinterpret_cast<const void*>(joints), now.data(), now.size())) return;
    const bool known = g_accessoryJoints == joints && g_accessoryPose.size() == static_cast<size_t>(count) * 0x30;
    if (g_accessoryFrames <= 0 || !known) {  // his own motions: his pose
        g_accessoryFrames = known ? 0 : 60;  // a new pose (start, reload, level): checked against the rest places below
        g_accessoryJoints = joints;
        g_accessoryPose.resize(static_cast<size_t>(count) * 0x30);
        for (uint32_t i = 0; i < count; ++i) std::memcpy(&g_accessoryPose[i * 0x30], &now[i * 0x90 + 0x20], 0x30);
        return;
    }
    --g_accessoryFrames;
    uintptr_t slots[8] = {};
    for (uint32_t layer = 0; layer < 8; ++layer) mem::Read(p + 0x380 + 0xE0 * layer + 0xD8, slots[layer]);
    if (!slots[0]) return;
    int fixed = 0, rest = 0;
    for (uint32_t i = 1; i < count; ++i) {  // not the root
        float* was = reinterpret_cast<float*>(&g_accessoryPose[i * 0x30]);  // rotation 4, scale 4, translation 4
        const float* cur = reinterpret_cast<const float*>(&now[i * 0x90 + 0x20]);
        const float* bind = reinterpret_cast<const float*>(&now[i * 0x90 + 0x10]);
        // A pose already left by a borrowed move (user, 2026-11-06: the knife sheath moved again - the trainer was reloaded
        // during Wesker's guard, the new one took the guard's leftovers for his pose): a joint no motion animates rests at
        // its bind offset (Chris's untouched accessories 121 / 123-130 and all of Sheva's, read live), so one more than
        // 0.5 cm from it goes back there.
        const bool offRest = std::fabs(was[8] - bind[0]) > 0.5f || std::fabs(was[9] - bind[1]) > 0.5f ||
                             std::fabs(was[10] - bind[2]) > 0.5f;
        bool moved = offRest;
        for (int k = 0; k < 12 && !moved; ++k) moved = std::fabs(was[k] - cur[k]) > (k >= 8 ? 0.05f : 0.001f);
        if (!moved) continue;
        bool driven = false;  // a track of any layer evaluates it: the motion's to place
        for (uint32_t layer = 0; layer < 8 && !driven; ++layer) {
            uint32_t tracks[3] = {};
            if (slots[layer] && mem::Read(slots[layer] + 0x60 * i, tracks[0]) &&
                mem::Read(slots[layer] + 0x60 * i + 0x10, tracks[1]) && mem::Read(slots[layer] + 0x60 * i + 0x20, tracks[2]))
                driven = tracks[0] || tracks[1] || tracks[2];
        }
        if (driven) continue;
        if (offRest) {
            for (int k = 0; k < 3; ++k) was[8 + k] = bind[k];
            ++rest;
        }
        const uintptr_t joint = joints + 0x90 * i;
        mem::SafeWrite(reinterpret_cast<void*>(joint + 0x20), was, 0x30);
        for (uint32_t layer = 0; layer < 8; ++layer) {
            if (!slots[layer]) continue;
            const uintptr_t slot = slots[layer] + 0x60 * i;
            mem::SafeWrite(reinterpret_cast<void*>(slot + 0x30), was, 0x10);      // rotation
            mem::SafeWrite(reinterpret_cast<void*>(slot + 0x40), was + 8, 0x10);  // translation
            mem::SafeWrite(reinterpret_cast<void*>(slot + 0x50), was + 4, 0x10);  // scale
        }
        ++fixed;
    }
    static ULONGLONG lastLog = 0;
    if (fixed && GetTickCount64() - lastLog > 2000) {
        lastLog = GetTickCount64();
        LOG("accessories: %d joint(s) of player 1 back to his pose before the move, %d of them to their rest place "
            "(action %X, motion %X)", fixed, rest, action, motion & 0xFFFF);
    }
}

}  // namespace

// The unload waits for this (dllmain.cpp; user, 2026-11-06: the sheath stuck through the harness until a room change -
// the trainer was reloaded during Wesker's guard, the leftovers never put back and the accessories' own physics, the
// pouches' and the sheath's end joints, stayed wrong after them): a borrowed move playing, or its pose being put back.
bool AccessoryRestorePending() {
    return g_accessoryFix.load(std::memory_order_relaxed) && actors::InGame() &&
           (g_accessoryForeign || g_accessoryFrames > 0);
}

namespace {
bool Heal(int index) {
    uintptr_t p = Player(index);
    int16_t max = 0;
    if (!p || !mem::Read(p + kHealthMax, max) || max <= 0) return false;
    if (p == actors::Player(0)) StaminaTrainerHeal();
    return mem::Write(p + kHealth, max);
}

// Per-frame field freeze for one player; restores the value it found when switched off.
struct FieldFreeze {
    int player;
    uintptr_t offset;
    uint8_t value;
    uint8_t saved = 0;
    uintptr_t savedFor = 0;
};

// Weapon motions follow P+0x1AF8 (male / female) each time a weapon is taken in hand: Player::SetWeaponMotion
// (RVA 0x75A7A0, thiscall(record key, motion slot), ret 8) takes the weapon unit's list +0xA8C (pl00wpXX) or +0xA90
// (pl01wpXX; both are in the weapon's archive) and installs it with uModel::SetMotionList (RVA 0x1CCDD0) in slot 5
// (P+0xA9C). Calling it for the weapon in hand (key 0xE) right after the flag changes makes the switch live; the
// other weapons follow when they are drawn (it only worked after a reload before, user 2026-10-14).
using SetWeaponMotionFn = void(__fastcall*)(void* player, void* edx, int key, int slot);

void RefreshWeaponMotion(int index) {
    const uintptr_t p = InGame() ? Player(index) : 0;
    uintptr_t held = 0;
    if (!p || !mem::Read(p + 0x2634, held) || !held) return;
    __try {
        reinterpret_cast<SetWeaponMotionFn>(mem::Rva(0x75A7A0))(reinterpret_cast<void*>(p), nullptr, 0xE, 5);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("weapon motion refresh failed for P%d", index + 1);
    }
}

Feature& AddFieldFreeze(const char* id, const char* name, const char* help, const char* category, FieldFreeze* ff) {
    return Add({
        .id = id, .name = name, .help = help, .category = category, .kind = Kind::Toggle,
        .set = [ff](bool on) {
            uintptr_t p = Player(ff->player);
            if (on) {
                if (!p) return false;
                mem::Read(p + ff->offset, ff->saved);
                ff->savedFor = p;
                return mem::Write(p + ff->offset, ff->value);
            }
            if (p && p == ff->savedFor) mem::Write(p + ff->offset, ff->saved);
            return true;
        },
        .tick = [ff] {
            uintptr_t p = Player(ff->player);
            uint8_t v = 0;
            if (p && mem::Read(p + ff->offset, v) && v != ff->value) mem::Write(p + ff->offset, ff->value);
        },
        .available = InGame,
    });
}

FieldFreeze g_leftyP1{ 0, kHandedness, 1 }, g_leftyP2{ 1, kHandedness, 1 };
bool NativeLeftHanded(uintptr_t player);
bool NativeFemaleAnims(uintptr_t player);
FieldFreeze g_femaleP1{ 0, kWeaponAnim, 1 }, g_femaleP2{ 1, kWeaponAnim, 1 };

// ---- No collision: code patch + per-player flag (as observed), flags restored on disable ----
mem::Patch g_noCollisionCode("collision.off", 0x772FA0, { 0x55 }, { 0xC3 });
std::atomic<bool> g_noCollisionOn{ false };

// Switched back on he went back to the room he switched it off in (user, 2026-10-26; logged: off at a door's far side, back
// on he stood in front of the door, on the near side): P+0x1170 and P+0x11D0 (float3) keep the position of the last
// collision update, and the next one moves him from there to his position, stopped by the walls in between. Both are
// set to his position just before the collision update comes back ("Rester sur place en réactivant les collisions",
// on by default: user).
constexpr uintptr_t kCollisionFrom[2] = { 0x1170, 0x11D0 };
std::atomic<bool> g_keepPlace{ false };
int g_watchFrames = 0;

bool SetNoCollision(bool on) {
    g_noCollisionOn = false;
    if (on) {
        if (!g_noCollisionCode.Apply()) return false;
        g_noCollisionOn = true;
    } else {
        for (int i = 0; i < 2 && g_keepPlace.load(std::memory_order_relaxed); ++i) {
            const uintptr_t p = Player(i);
            float at[3]{};
            if (!p || !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), at, sizeof(at))) continue;
            for (uintptr_t field : kCollisionFrom) mem::SafeWrite(reinterpret_cast<void*>(p + field), at, sizeof(at));
            LOG("no collision: off, player %d's collision starts again from (%.0f %.0f %.0f)", i + 1, at[0], at[1], at[2]);
        }
        g_watchFrames = 30;
        for (int i = 0; i < 2; ++i)
            if (uintptr_t p = Player(i)) mem::Write<uint32_t>(p + kCollision, 1);
        return g_noCollisionCode.Restore();
    }
    return true;
}

void TickNoCollision() {
    for (int i = 0; i < 2; ++i)
        if (uintptr_t p = Player(i)) mem::Write<uint32_t>(p + kCollision, 0);
}

// "Monter / Descendre (sans collision)" (user, 2026-10-26: UT's Increase / Decrease Z): player 1's height P+0x34 moved
// while the key is held and "Pas de collision" is on, at the slider's speed (cm/s); the frozen height follows.
Feature* g_flyUp = nullptr;
Feature* g_flyDown = nullptr;
void FollowFrozenHeight(uintptr_t p, float height);  // below

void TickFly() {
    if (g_watchFrames > 0) {  // player 1's position at the first and 30th frames after collision came back
        --g_watchFrames;
        float at[3]{};
        const uintptr_t p = Player(0);
        if ((g_watchFrames == 29 || g_watchFrames == 0) && p && mem::SafeRead(reinterpret_cast<void*>(p + 0x30), at, sizeof(at)))
            LOG("no collision: back on, frame %d at (%.0f %.0f %.0f)", 30 - g_watchFrames, at[0], at[1], at[2]);
    }
    static LARGE_INTEGER last{};
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? static_cast<float>(now.QuadPart - last.QuadPart) / freq.QuadPart : 0.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    const uintptr_t p = Player(0);
    if (!p || !g_noCollisionOn.load(std::memory_order_relaxed)) return;
    auto held = [](const Feature* f) {
        return f && f->enabled && (hotkeys::Held(f->hotkey) || hotkeys::Held(f->hotkey2));
    };
    const float up = held(g_flyUp) ? g_flyUp->value : 0.0f, down = held(g_flyDown) ? g_flyDown->value : 0.0f;
    float height = 0;
    if ((up == 0.0f && down == 0.0f) || !mem::Read(p + 0x34, height)) return;
    height += (up - down) * dt;
    mem::Write(p + 0x34, height);
    FollowFrozenHeight(p, height);
}

// ---- Freeze height (UT "Freeze Z Coordinate", Only You / AI Player): P+0x34 = vertical position, kept at the
// value it had when switched on (UT wrote it back the same way). Captured again when the player object changes
// (level load), so a new level does not teleport the player to the old height.
constexpr uintptr_t kHeight = 0x34;
struct HeightFreeze {
    int player;
    float height = 0;
    uintptr_t capturedFor = 0;
};
HeightFreeze g_heightP1{ 0 }, g_heightP2{ 1 };

void FollowFrozenHeight(uintptr_t p, float height) {
    if (g_heightP1.capturedFor == p) g_heightP1.height = height;
}

Feature& AddHeightFreeze(const char* id, const char* name, const char* help, HeightFreeze* hf) {
    return Add({
        .id = id, .name = name, .help = help, .category = "Joueur", .kind = Kind::Toggle,
        .set = [hf](bool on) {
            hf->capturedFor = 0;
            if (!on) return true;
            uintptr_t p = Player(hf->player);
            if (!p || !mem::Read(p + kHeight, hf->height)) return false;
            hf->capturedFor = p;
            return true;
        },
        .tick = [hf] {
            uintptr_t p = Player(hf->player);
            if (!p) return;
            if (p != hf->capturedFor) {
                if (mem::Read(p + kHeight, hf->height)) hf->capturedFor = p;
                return;
            }
            mem::Write(p + kHeight, hf->height);
        },
        .available = InGame,
    });
}

// ---- Uncap FPS: [re5dx9+0xE340A4]+0x3C float frame cap (120 by default) ----
float g_savedFps = 0;

float* FpsCap() {
    uintptr_t obj = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE340A4)), obj) || !obj) return nullptr;
    return reinterpret_cast<float*>(obj + 0x3C);
}

// The game's limit (120) is replaced by the slider's value while the option is on (user, 2026-10-15: a slider instead
// of a fixed 240; at ~215 FPS a plaga froze mid-attack in the mines, MODLOG).
float g_appliedFps = 0;

bool SetFpsCap(bool on, float value) {
    float* cap = FpsCap();
    if (!cap) return false;
    if (on) {
        float current = 0;
        if (!mem::Read(reinterpret_cast<uintptr_t>(cap), current) || current < 10 || current > 1000) return false;
        if (current != g_appliedFps) g_savedFps = current;  // the game's own value, not ours
        g_appliedFps = value;
        return mem::Write(reinterpret_cast<uintptr_t>(cap), value);
    }
    g_appliedFps = 0;
    return mem::Write(reinterpret_cast<uintptr_t>(cap), g_savedFps > 0 ? g_savedFps : 120.0f);
}

// The soft parasite's freeze (user, 2026-11-10: "it freezes above 120, in the mines, No Mercy, now Mercenaries and the campaign"; "only
// if it comes out at 240"): its class's own update (uEm0d vtable slot 23, VA 0x85EDE0, in no other vtable) rounds its layer 0 frame
// to the nearest whole frame after the motion update (+0x398 and +0x3AC; the frame before kept at +0x1FB0; not its motions 0x0E /
// 0x0F). Its frames advance 60 a second: a whole frame an update at 60 FPS (the rounding changes nothing), half a frame at 120 (rounded
// up to a whole one: twice too fast), under half from 150 on (rounded back: frozen; its Majini waits in cEm10ActAtkPlagaSmash 0x63 for
// its motion's end). With the option on, that rounding is skipped (addss / cvttss2si / cvtsi2ss nopped): its own speed at any rate.
// Found by the session of 2026-11-10 (a write trace on its frame, the writer at VA 0x85EE23), its frozen state read live (Majini 0x63
// motion 0x851, its plaga +0x1F9C in motion 1 at a whole frame, its step +0x10 0.125).
mem::Patch g_parasiteFrame("parasite: animation frame not rounded", 0x45EE0B,
                           { 0xF3, 0x0F, 0x58, 0x05, 0x98, 0xB4, 0x0B, 0x01, 0xF3, 0x0F, 0x2C, 0xC0, 0xF3, 0x0F, 0x2A, 0xC0 },
                           std::vector<uint8_t>(16, 0x90));
// The slider's value everywhere (user, 2026-11-10: "lift the limits, I will test"): the mines (room 0x6B) and Mercenaries' No Mercy were
// held at 120 (a plaga froze there at 150-240), the soft parasite's frame rounding being the cause (g_parasiteFrame).
float FpsCapHere(float value) { return value; }

// ---- Lefty P1 + camera on the left shoulder ----------------------------------------------------
// Camera blocks (camera.cpp): float [0] = horizontal distance, [4] = horizontal adjustment. Chris (right-
// handed) has them negative; a left-handed player gets them positive. Restored on disable. The camera also
// keeps copies built at level load (CameraOverride, side chosen from +0x1AF9 at that moment): the aiming
// camera uses them, so it stayed on the right shoulder after a live switch (user, 2026-10-13). Flipped too.
// The parameter blocks are shared by the game's cameras: values flipped while one camera was active stayed on the
// left under another one (the knife-slot weapon's camera, user 2026-10-13). Flipped values are remembered by
// address and put back by address, whatever camera is active; forgotten when the level changes.
struct FlippedValue {
    uintptr_t block;
    int index;
    bool left;  // flipped to the left shoulder (positive), else to the right
};
std::vector<FlippedValue> g_leftyCamFlipped;
bool NativeLeftHanded(uintptr_t player);
// After an off, the blocks the camera only takes later (user, 2026-10-31: the knife aim's camera stayed on the left
// shoulder with "Main inversée" switched off, the normal aim on the right - the knife slot has its own blocks, not the
// camera's at the off, still holding the other side): each one put once on the character's own side when it comes,
// until a shoulder is asked again.
bool g_ownSidePending = false;
std::vector<uintptr_t> g_ownSideDone;

void PutOwnSide(uintptr_t block, bool nativeLeft) {
    for (int index : { 0, 4 }) {
        float v = 0;
        if (mem::Read(block + 4 * index, v) && (nativeLeft ? v < 0 : v > 0)) mem::Write(block + 4 * index, -v);
    }
}

// The camera on one shoulder: values of the other sign flipped (left: positive; right: negative), remembered by
// address. Off: the flipped ones back, and the setup copies on the character's own side (after a load done with the
// other hand the game built them on that side).
void ShoulderCamera(bool on, bool left) {
    if (on) {
        g_ownSidePending = false;
        for (int slot = 0; slot < 12; ++slot) {
            const uintptr_t block = slot < 6 ? CameraBlock(slot) : CameraOverride(slot - 6);
            if (!block) continue;
            for (int index : { 0, 4 }) {
                float v = 0;
                if (!mem::Read(block + 4 * index, v) || (left ? v >= 0 : v <= 0)) continue;
                mem::Write(block + 4 * index, -v);
                bool known = false;
                for (const FlippedValue& f : g_leftyCamFlipped) known = known || (f.block == block && f.index == index);
                if (!known) g_leftyCamFlipped.push_back({ block, index, left });
            }
        }
        return;
    }
    for (const FlippedValue& f : g_leftyCamFlipped) {
        float v = 0;
        if (IsCameraParams(f.block) && mem::Read(f.block + 4 * f.index, v) && (f.left ? v > 0 : v < 0))
            mem::Write(f.block + 4 * f.index, -v);
    }
    g_leftyCamFlipped.clear();
    g_ownSidePending = true;
    g_ownSideDone.clear();
    const uintptr_t p = Player(0);
    if (!p) return;
    // Every block on the character's own side, the parameter blocks too (user, 2026-10-27: switched on, then a checkpoint
    // reload: the flipped values were forgotten with the level but the blocks kept them, and off left the camera on the
    // left shoulder for good).
    const bool nativeLeft = NativeLeftHanded(p);
    for (int k = 0; k < 12; ++k)
        if (const uintptr_t block = k < 6 ? CameraBlock(k) : CameraOverride(k - 6)) {
            PutOwnSide(block, nativeLeft);
            g_ownSideDone.push_back(block);
        }
}

// The bow / Gatling hand (UpdateWeaponHand): the left shoulder, or the character's own side back.
void LeftyCamera(bool on) { ShoulderCamera(on, true); }

void ForgetLeftyCamera() {
    if (!InGame()) {
        g_leftyCamFlipped.clear();  // level change: the camera parameters are rebuilt
        g_ownSideDone.clear();      // (the blocks may keep their values: checked once more)
        return;
    }
    const uintptr_t p = g_ownSidePending ? Player(0) : 0;
    if (!p) return;
    const bool nativeLeft = NativeLeftHanded(p);
    for (int k = 0; k < 12; ++k) {
        const uintptr_t block = k < 6 ? CameraBlock(k) : CameraOverride(k - 6);
        if (!block || std::find(g_ownSideDone.begin(), g_ownSideDone.end(), block) != g_ownSideDone.end()) continue;
        PutOwnSide(block, nativeLeft);
        g_ownSideDone.push_back(block);
        LOG("lefty camera: block %08X put on player 1's own side", static_cast<unsigned>(block));
    }
    if (g_ownSideDone.size() > 64) g_ownSidePending = false;  // blocks of many levels: enough
}

// ---- Weapon in the left hand of a right-handed character ----------------------------------------
// A lefty player's weapon is attached to joint 13 (left hand) with an identity local transform. Sheva's rig is
// made for that; on a right-handed rig (every other character: record +0x1C = 0) joint 13 is the mirror of joint
// 8 with X and Y reversed (measured on Chris, both hands on the pistol), so the weapon sat turned over in his
// hand (user, 2026-10-13). The world matrix of an attached object is computed by RVA 0x08ABA0 (thiscall, no
// argument): local matrix +0x60 times the parent's joint matrix when +0x1C (joint number) >= 0. While it runs for
// that weapon, the local matrix gets a half turn about the joint's Z axis (x and y of every row negated), then
// is put back. The local quaternion +0x40 cannot be changed from outside: the motion update rewrites it.
constexpr uintptr_t kWeaponParent = 0x18;  // item object: actor it is attached to
constexpr uintptr_t kWeaponJoint = 0x1C;   // item object: joint number on that actor (8 right hand, 13 left)
constexpr uintptr_t kLocalMatrix = 0x60;
using CoordWorldFn = void(__fastcall*)(void* self, void* edx);
CoordWorldFn g_coordWorld = nullptr;
bool g_coordWorldHooked = false;
std::atomic<uintptr_t> g_turnedWeapon[2] = {};
// Fine adjustment chosen by the user (sliders), added to the translation of that local matrix (joint-space
// units, about 1 cm): the left hand of the lefty motions does not grip exactly like the right one.
std::atomic<float> g_turnedOffset[3] = {};
// A weapon moved in the hand by camera.cpp's iron sights (user, 2026-11-04: the Hydra sat too low in Chris's hands in
// the aim it borrows): added to its local matrix's translation while its world matrix is made.
std::atomic<uintptr_t> g_liftUnit{ 0 };
// A knife held reversed (wesker_moves.cpp: RE6's knife Ground and throat cut; user, 2026-11-10): its local matrix turned half a turn
// about its own Y and moved back into the hand while its world matrix is made - set by the user with research sliders (Z 180 pointed
// the blade at Chris, X +-90 up / to the right; Y -180 "the right way round but shifted, not in Chris's hand"; then X -13.3, Y 1.1 cm:
// "the orientation and the offset are good, remove the sliders").
std::atomic<uintptr_t> g_reverseGrip{ 0 };
// A weapon held as RE6's weapon joint holds it (wesker_moves.cpp: bone 9 by frame of the RE6 moves that move it; user, 2026-11-10: "Chris
// slides the rifle to strike with the stock"): the move's index, its unit; the joint's frame read from the unit's actor (+0x18).
std::atomic<uintptr_t> g_weaponJointUnit{ 0 };
std::atomic<int> g_weaponJointIndex{ -1 };
// A Majini's weapon player 1 took (wesker_moves.cpp: RE6's weapon steals; user, 2026-11-10): while its world matrix is made, its parent is
// player 1 instead of its Majini (its joint, 8 or 13, kept: the pair is mirrored for a left-handed Majini) - nothing else of it changed.
std::atomic<uintptr_t> g_stolenWeapon{ 0 }, g_stolenFrom{ 0 }, g_stolenHolder{ 0 };
// Player 1's hand holding it (8 right, 13 left; 0: the one the Majini held it with). In the other hand than the Majini's, its local
// matrix gets the half turn about the joint's Z a hand's mirror needs (x and y of every row negated, as "Main inversée" does).
std::atomic<uint32_t> g_stolenJoint{ 0 };
// A stolen weapon let go at the move's end (user, 2026-11-11: "it should stay detached so Chris lets it fall to the ground, as when I
// shoot an enemy's hand and it drops its weapon"). The game's own release (VA 0x85DFB0) drops only uWp5d / uWp62 and the emplacement
// guns: for the others it clears the Majini's pointers and the weapon stays on its hand joint ("it teleported back into its hand when it
// got up"). Its fall is made here: from where player 1's hand left it, down with gravity to his floor, its most upright axis laid flat
// on the way, then left lying - its world matrix written after the game made it, as long as its Majini is still its parent.
struct DroppedWeapon {
    uintptr_t unit = 0, parent = 0;
    float m[16]{};       // the world matrix where it was let go (rows: its axes, then its place)
    float axis[3]{};     // the world axis it turns about to lie down
    float angle = 0.0f;  // by how much (radians)
    float floor = 0.0f;  // its place's height once lying
    float fall = 0.3f;   // the fall's duration (s)
    ULONGLONG start = 0;
};
constexpr int kDroppedMax = 8;
DroppedWeapon g_dropped[kDroppedMax];
std::atomic<int> g_droppedCount{ 0 };
std::atomic<bool> g_droppedBusy{ false };

void Rotate(const float axis[3], float angle, const float v[3], float out[3]) {
    const float c = std::cos(angle), sn = std::sin(angle), d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
    const float cr[3] = { axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0] };
    for (int i = 0; i < 3; ++i) out[i] = v[i] * c + cr[i] * sn + axis[i] * d * (1.0f - c);
}

// The dropped weapon's world matrix now (after the game made its own from its Majini's hand).
bool DroppedWorld(uintptr_t s) {
    for (int i = 0; i < kDroppedMax; ++i) {
        DroppedWeapon& d = g_dropped[i];
        if (d.unit != s) continue;
        uintptr_t parent = 0;
        if (!mem::Read(s + kWeaponParent, parent) || parent != d.parent) {
            d.unit = 0;  // another unit there now, or let go by the game
            return false;
        }
        const float t = static_cast<float>(GetTickCount64() - d.start) / 1000.0f;
        const float k = std::min(1.0f, t / d.fall);
        float* w = reinterpret_cast<float*>(s + 0xA0);
        for (int row = 0; row < 3; ++row) Rotate(d.axis, d.angle * k, &d.m[row * 4], &w[row * 4]);
        w[12] = d.m[12];
        w[14] = d.m[14];
        w[13] = std::max(d.floor, d.m[13] - 0.5f * 980.0f * t * t);
        w[15] = 1.0f;
        return true;
    }
    return false;
}
constexpr float kReverseGripAngles[3] = { 0.0f, -180.0f, 0.0f };  // degrees about its own X, Y, Z
constexpr float kReverseGripOffset[3] = { -13.3f, 1.1f, 0.0f };   // cm along its own axes as they were before the turn
std::atomic<float> g_lift[3] = {};
// The bow on a man's back (user, 2026-11-04: "on Chris the bow's angle on the back is crooked"): Sheva's weapon, its
// model laid for the women's back joint (id 2); on a man it hangs from joint id 117 (0x75) like his long guns, its local
// transform identity: lying across the shoulders. Its local matrix gets a quarter turn about its X (its limbs along Z
// stand up) and 25 cm down in the joint's frame (Y up there) while its world matrix is made - tuned live by the user
// with six sliders (since removed).
std::atomic<uintptr_t> g_backBow{ 0 };
constexpr float kBackBowTurn[3] = { 90.0f, 0.0f, 0.0f }, kBackBowShift[3] = { 0.0f, -25.0f, 0.0f };

// m (rows: the local axes, then the place) turned by the angles about its own axes and moved by the offset.
void TurnLocal(float* m, const float degrees[3], const float offset[3]) {
    float r[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int axis = 0; axis < 3; ++axis) {
        const float a = degrees[axis] * 3.14159265f / 180.0f, c = std::cos(a), sn = std::sin(a);
        float q[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        const int i = (axis + 1) % 3, j = (axis + 2) % 3;
        q[i][i] = c;
        q[i][j] = sn;
        q[j][i] = -sn;
        q[j][j] = c;
        float t[3][3];
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col) t[row][col] = q[row][0] * r[0][col] + q[row][1] * r[1][col] + q[row][2] * r[2][col];
        std::memcpy(r, t, sizeof(r));
    }
    float out[3][3];
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) out[row][col] = r[row][0] * m[col] + r[row][1] * m[4 + col] + r[row][2] * m[8 + col];
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) m[4 * row + col] = out[row][col];
    for (int axis = 0; axis < 3; ++axis) m[12 + axis] += offset[axis];
}

void __fastcall HookCoordWorld(void* self, void* edx) {
    const uintptr_t s = reinterpret_cast<uintptr_t>(self);
    if (s && g_droppedCount.load(std::memory_order_relaxed) > 0 && !g_droppedBusy.load(std::memory_order_acquire)) {
        bool dropped = false;
        for (int i = 0; i < kDroppedMax && !dropped; ++i) dropped = g_dropped[i].unit == s;
        if (dropped) {
            g_coordWorld(self, edx);
            DroppedWorld(s);
            return;
        }
    }
    if (RocketLocalMatrix(s, reinterpret_cast<float*>(s + kLocalMatrix))) {
        g_coordWorld(self, edx);
        return;
    }
    if (s && (s == g_turnedWeapon[0].load(std::memory_order_relaxed) || s == g_turnedWeapon[1].load(std::memory_order_relaxed))) {
        float* m = reinterpret_cast<float*>(s + kLocalMatrix);
        float saved[16];
        std::memcpy(saved, m, sizeof(saved));
        for (int row = 0; row < 4; ++row) {
            m[row * 4 + 0] = -m[row * 4 + 0];
            m[row * 4 + 1] = -m[row * 4 + 1];
        }
        for (int axis = 0; axis < 3; ++axis) m[12 + axis] += g_turnedOffset[axis].load(std::memory_order_relaxed);
        if (s == g_liftUnit.load(std::memory_order_relaxed))
            for (int axis = 0; axis < 3; ++axis) m[12 + axis] += g_lift[axis].load(std::memory_order_relaxed);
        g_coordWorld(self, edx);
        std::memcpy(m, saved, sizeof(saved));
        return;
    }
    if (s && s == g_backBow.load(std::memory_order_relaxed)) {
        float* m = reinterpret_cast<float*>(s + kLocalMatrix);
        float saved[16];
        std::memcpy(saved, m, sizeof(saved));
        TurnLocal(m, kBackBowTurn, kBackBowShift);
        g_coordWorld(self, edx);
        std::memcpy(m, saved, sizeof(saved));
        return;
    }
    if (s && s == g_stolenWeapon.load(std::memory_order_relaxed)) {
        const uintptr_t holder = g_stolenHolder.load(std::memory_order_relaxed);
        uintptr_t parent = 0;
        uint32_t joint = 0;
        const bool ok = holder && mem::Read(s + kWeaponParent, parent) && parent == g_stolenFrom.load(std::memory_order_relaxed) &&
                        mem::Read(s + kWeaponJoint, joint) && (joint == 8 || joint == 13);
        // Research (user, 2026-11-10: "the weapon stays in the enemy's hand"): what the hook sees, once a second.
        static ULONGLONG told = 0;
        if (GetTickCount64() - told > 1000) {
            told = GetTickCount64();
            LOG("stolen weapon research: %08X parent %08X (its Majini %08X) joint %u: %s", static_cast<unsigned>(s), static_cast<unsigned>(parent),
                static_cast<unsigned>(g_stolenFrom.load()), joint, ok ? "drawn from player 1's hand" : "left alone");
        }
        if (ok) {
            const uint32_t hand = g_stolenJoint.load(std::memory_order_relaxed);
            const bool other = hand && hand != joint;
            float* m = reinterpret_cast<float*>(s + kLocalMatrix);
            float saved[16];
            std::memcpy(saved, m, sizeof(saved));
            if (other) {
                mem::Write(s + kWeaponJoint, hand);
                for (int row = 0; row < 4; ++row) {
                    m[row * 4 + 0] = -m[row * 4 + 0];
                    m[row * 4 + 1] = -m[row * 4 + 1];
                }
            }
            mem::Write(s + kWeaponParent, holder);
            g_coordWorld(self, edx);
            mem::Write(s + kWeaponParent, parent);
            if (other) {
                mem::Write(s + kWeaponJoint, joint);
                std::memcpy(m, saved, sizeof(saved));
            }
            return;
        }
    }
    if (s && s == g_weaponJointUnit.load(std::memory_order_relaxed)) {
        // RE6's bone 9 (rotation q, position t in the hand's frame) between the hand and the weapon: the weapon's local matrix (rows: its
        // axes, then its place, in the hand's frame; world = local x parent) becomes local x [R(q) | t], R(q) in the import tools' row
        // convention (tools/import_re6_motions.py _matrix). The right hand only (joint 8: a left-handed player's mirror is not done).
        uintptr_t actor = 0;
        uint32_t motion = 0, joint = 0;
        float frame = 0.0f, q[4]{}, t[3]{};
        if (mem::Read(s + 0x18, actor) && actor && mem::Read(s + 0x1C, joint) && joint == 8 && mem::Read(actor + 0x384, motion) &&
            mem::Read(actor + 0x398, frame) &&
            Re6WeaponJointAt(g_weaponJointIndex.load(std::memory_order_relaxed), motion, frame, q, t)) {
            float* m = reinterpret_cast<float*>(s + kLocalMatrix);
            float saved[16];
            std::memcpy(saved, m, sizeof(saved));
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            const float r[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w) },
                                    { 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w) },
                                    { 2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y) } };
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 3; ++col) {
                    float v = 0.0f;
                    for (int k = 0; k < 3; ++k) v += saved[4 * row + k] * r[k][col];
                    m[4 * row + col] = v + (row == 3 ? t[col] : 0.0f);
                }
            g_coordWorld(self, edx);
            std::memcpy(m, saved, sizeof(saved));
            return;
        }
    }
    if (s && s == g_reverseGrip.load(std::memory_order_relaxed)) {
        float* m = reinterpret_cast<float*>(s + kLocalMatrix);
        float saved[16];
        std::memcpy(saved, m, sizeof(saved));
        // Its turn about its own axes, then moved back into the hand (it turned about its own origin, not about the grip): along its own
        // axes as they were (rows 0-2 of the matrix before the turn).
        const float none[3] = {};
        TurnLocal(m, kReverseGripAngles, none);
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j) m[12 + k] += kReverseGripOffset[j] * saved[4 * j + k];
        g_coordWorld(self, edx);
        std::memcpy(m, saved, sizeof(saved));
        return;
    }
    if (s && s == g_liftUnit.load(std::memory_order_relaxed)) {
        float* m = reinterpret_cast<float*>(s + kLocalMatrix);
        float saved[3];
        std::memcpy(saved, m + 12, sizeof(saved));
        for (int axis = 0; axis < 3; ++axis) m[12 + axis] += g_lift[axis].load(std::memory_order_relaxed);
        g_coordWorld(self, edx);
        std::memcpy(m + 12, saved, sizeof(saved));
        return;
    }
    g_coordWorld(self, edx);
}

bool EnsureCoordWorldHook() {
    if (g_coordWorldHooked) return true;
    const uint8_t prologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x3C, 0x56, 0x8B, 0xF1 };
    g_coordWorldHooked = HookAt(0x08ABA0, prologue, sizeof(prologue), &HookCoordWorld,
                                reinterpret_cast<void**>(&g_coordWorld), "coord world matrix");
    return g_coordWorldHooked;
}

// Handedness of the character record (table at RVA 0xCFC128, 0x28 bytes each: +4 character, +8 costume,
// +0x1C 1 = left-handed; only Sheva). Unknown character: treated as left-handed, i.e. left alone.
bool NativeLeftHanded(uintptr_t player) {
    static uintptr_t cachedFor[2] = {};
    static bool cached[2] = {};
    const int slot = player == Player(1) ? 1 : 0;
    if (cachedFor[slot] == player) return cached[slot];
    bool left = true;
    const uint32_t info = ActorCharacterInfo(reinterpret_cast<void*>(player));
    if (info != 0xFFFFFFFF) {
        const uint32_t character = (info >> 8) & 0xFFFF, costume = info & 0xFF;
        const uintptr_t first = reinterpret_cast<uintptr_t>(mem::Rva(0xCFC128));
        for (uintptr_t r = first; r < first + 31 * 0x28; r += 0x28) {
            uint32_t c = 0, co = 0;
            uint8_t hand = 1;
            if (mem::Read(r + 4, c) && mem::Read(r + 8, co) && c == character && co == costume) {
                left = mem::Read(r + 0x1C, hand) && hand != 0;
                break;
            }
        }
    }
    cachedFor[slot] = player;
    cached[slot] = left;
    return left;
}

// The hand of the character record (same table): 1 left, 0 right, -1 not found.
int CharacterHand(uintptr_t player) {
    const uint32_t info = ActorCharacterInfo(reinterpret_cast<void*>(player));
    if (info == 0xFFFFFFFF) return -1;
    const uint32_t character = (info >> 8) & 0xFFFF, costume = info & 0xFF;
    const uintptr_t first = reinterpret_cast<uintptr_t>(mem::Rva(0xCFC128));
    for (uintptr_t r = first; r < first + 31 * 0x28; r += 0x28) {
        uint32_t c = 0, co = 0;
        uint8_t hand = 1;
        if (mem::Read(r + 4, c) && mem::Read(r + 8, co) && c == character && co == costume)
            return mem::Read(r + 0x1C, hand) ? (hand != 0 ? 1 : 0) : -1;
    }
    return -1;
}

// Weapon-animation set of the character record (same table, +0x14: 1 = female weapon motions; Sheva, Jill...).
// Unknown character: male.
bool NativeFemaleAnims(uintptr_t player) {
    static uintptr_t cachedFor[2] = {};
    static bool cached[2] = {};
    const int slot = player == Player(1) ? 1 : 0;
    if (cachedFor[slot] == player) return cached[slot];
    bool female = false;
    const uint32_t info = ActorCharacterInfo(reinterpret_cast<void*>(player));
    if (info != 0xFFFFFFFF) {
        const uint32_t character = (info >> 8) & 0xFFFF, costume = info & 0xFF;
        const uintptr_t first = reinterpret_cast<uintptr_t>(mem::Rva(0xCFC128));
        for (uintptr_t r = first; r < first + 31 * 0x28; r += 0x28) {
            uint32_t c = 0, co = 0;
            uint8_t anims = 0;
            if (mem::Read(r + 4, c) && mem::Read(r + 8, co) && c == character && co == costume) {
                female = mem::Read(r + 0x14, anims) && anims != 0;
                break;
            }
        }
    }
    cachedFor[slot] = player;
    cached[slot] = female;
    return female;
}

// Every frame: the held weapon of a right-handed player switched to the left hand gets the half turn.
// ---- Bow / Gatling: each archive holds only one character's player motions (uWp0f: Sheva's pl01wp0f, uWp15:
// Chris's pl00wp15). Chris aims the bow cleanly only in lefty mode (user, 2026-10-13), Sheva the Gatling only
// right-handed. While one of them is in hand, the handedness follows the weapon (the player's own lefty choice wins
// over the Gatling), then goes back to the player's setting.
bool g_weaponHand = false;
int g_weaponHandForced[2] = { -1, -1 };  // handedness written by this option, -1 = none

uint16_t EquippedItem(uintptr_t player) {
    for (int k = 0; k < 24; ++k) {
        const uintptr_t item = player + 0x21A4 + 0x30 * k;  // cItem k of the item manager at P+0x21A0
        uint8_t inUse = 0;
        uint32_t state = 0, id = 0;
        if (mem::Read(item + 0x29, inUse) && inUse && mem::Read(item + 0x1C, state) && state == 1 && mem::Read(item + 4, id))
            return static_cast<uint16_t>(id);
    }
    return 0;
}

// The prompts' side (user, 2026-10-27: "Main inversée" moved the pickup prompt to the other side while the health
// ring stays on the character's): sActionButton's two drawers (RVA 0x364B20, RVA 0x365A74; the player of the prompt's
// slot) pick the side with `cmp byte [edi+0x1AF9], 0` (cl / al zeroed just before) and setne. Those two reads become
// the character's own hand (the record's, as the ring) while a hand option is on.
uint8_t __cdecl PromptHand(uintptr_t player) {
    const int hand = CharacterHand(player);
    if (hand >= 0) return static_cast<uint8_t>(hand);
    uint8_t live = 0;
    mem::Read(player + kHandedness, live);
    return live;
}

__declspec(naked) void PromptHandStub() {
    __asm {
        push eax
        push ecx
        push edx
        sub esp, 0x80
        movups xmmword ptr [esp], xmm0
        movups xmmword ptr [esp + 0x10], xmm1
        movups xmmword ptr [esp + 0x20], xmm2
        movups xmmword ptr [esp + 0x30], xmm3
        movups xmmword ptr [esp + 0x40], xmm4
        movups xmmword ptr [esp + 0x50], xmm5
        movups xmmword ptr [esp + 0x60], xmm6
        movups xmmword ptr [esp + 0x70], xmm7
        push edi
        call PromptHand
        add esp, 4
        movups xmm0, xmmword ptr [esp]
        movups xmm1, xmmword ptr [esp + 0x10]
        movups xmm2, xmmword ptr [esp + 0x20]
        movups xmm3, xmmword ptr [esp + 0x30]
        movups xmm4, xmmword ptr [esp + 0x40]
        movups xmm5, xmmword ptr [esp + 0x50]
        movups xmm6, xmmword ptr [esp + 0x60]
        movups xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        cmp al, 0  // replaced: cmp byte [edi+0x1AF9], 0 (the flags for its setne)
        pop edx
        pop ecx
        pop eax
        ret
    }
}

// The inventory's mouse (user, 2026-11-04: with "Main inversée" in the story, the mouse over Chris's first slot lit the
// partner's first one, and the other way round): VA 0xC69F00 (the inventory's per-player pointing, player in edi) takes
// a left-handed player's grid for the right one (slots 9-17: Sheva's place as player 1) with `cmp byte [edi+0x1AF9],
// 1` at VA 0xC69F24; it gets the character's own hand too.
__declspec(naked) void InventoryHandStub() {
    __asm {
        push eax
        push ecx
        push edx
        sub esp, 0x80
        movups xmmword ptr [esp], xmm0
        movups xmmword ptr [esp + 0x10], xmm1
        movups xmmword ptr [esp + 0x20], xmm2
        movups xmmword ptr [esp + 0x30], xmm3
        movups xmmword ptr [esp + 0x40], xmm4
        movups xmmword ptr [esp + 0x50], xmm5
        movups xmmword ptr [esp + 0x60], xmm6
        movups xmmword ptr [esp + 0x70], xmm7
        push edi
        call PromptHand
        add esp, 4
        movups xmm0, xmmword ptr [esp]
        movups xmm1, xmmword ptr [esp + 0x10]
        movups xmm2, xmmword ptr [esp + 0x20]
        movups xmm3, xmmword ptr [esp + 0x30]
        movups xmm4, xmmword ptr [esp + 0x40]
        movups xmm5, xmmword ptr [esp + 0x50]
        movups xmm6, xmmword ptr [esp + 0x60]
        movups xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        cmp al, 1  // replaced: cmp byte [edi+0x1AF9], 1 (the flags for its jne)
        pop edx
        pop ecx
        pop eax
        ret
    }
}

mem::Patch* g_promptHand[3] = {};

void SetPromptHand(bool on) {
    static constexpr uint32_t kSites[2] = { 0x364B60, 0x365AA0 };
    static constexpr uint8_t kRegister[2] = { 0x8F, 0x87 };  // cmp [edi+0x1AF9], cl / al
    if (!g_promptHand[2] && on) {
        uint8_t* site = mem::Rva(0x869F24);
        const std::vector<uint8_t> original = { 0x80, 0xBF, 0xF9, 0x1A, 0x00, 0x00, 0x01 };
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&InventoryHandStub) - reinterpret_cast<uintptr_t>(site + 5));
        std::vector<uint8_t> patched = { 0xE8 };
        for (int b = 0; b < 4; ++b) patched.push_back(static_cast<uint8_t>(rel >> (8 * b)));
        patched.push_back(0x90);
        patched.push_back(0x90);
        g_promptHand[2] = new mem::Patch("inventory pointing: the character's hand", site, original, patched);
    }
    if (g_promptHand[2] && on != g_promptHand[2]->IsApplied()) on ? g_promptHand[2]->Apply() : g_promptHand[2]->Restore();
    for (int i = 0; i < 2; ++i) {
        if (!g_promptHand[i]) {
            if (!on) continue;
            uint8_t* site = mem::Rva(kSites[i]);
            const std::vector<uint8_t> original = { 0x38, kRegister[i], 0xF9, 0x1A, 0x00, 0x00 };
            const int32_t rel =
                static_cast<int32_t>(reinterpret_cast<uintptr_t>(&PromptHandStub) - reinterpret_cast<uintptr_t>(site + 5));
            std::vector<uint8_t> patched = { 0xE8 };
            for (int b = 0; b < 4; ++b) patched.push_back(static_cast<uint8_t>(rel >> (8 * b)));
            patched.push_back(0x90);
            g_promptHand[i] = new mem::Patch("prompt side: the character's hand", site, original, patched);
        }
        if (on != g_promptHand[i]->IsApplied()) on ? g_promptHand[i]->Apply() : g_promptHand[i]->Restore();
    }
}

// Handedness set while a player is being created freezes left-handed motions for the whole level (switching back
// live keeps them): the option acts only once the player has existed for 3 s.
ULONGLONG g_weaponHandSince[2] = {};

void UpdateWeaponHand() {
    {
        const Feature* p1 = Find("player.lefty_p1");
        const Feature* p2 = Find("player.lefty_p2");
        SetPromptHand(g_weaponHand || (p1 && p1->enabled) || (p2 && p2->enabled));
    }
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 2; ++i) {
        const uintptr_t p = InGame() ? Player(i) : 0;
        if (!p) {
            g_weaponHandForced[i] = -1;
            g_weaponHandSince[i] = 0;
            continue;
        }
        if (!g_weaponHandSince[i]) g_weaponHandSince[i] = now;
        if (now - g_weaponHandSince[i] < 3000) continue;
        const Feature* lefty = Find(i == 0 ? "player.lefty_p1" : "player.lefty_p2");
        const bool userLefty = lefty && lefty->enabled;
        int want = -1;
        if (g_weaponHand) {
            const uint16_t id = EquippedItem(p);
            if (id == 0x010F) want = 1;
            else if (id == 0x0115 && !userLefty) want = 0;
        }
        if (want >= 0) {
            mem::Write<uint8_t>(p + kHandedness, static_cast<uint8_t>(want));
            if (i == 0 && want == 1) LeftyCamera(true);
            g_weaponHandForced[i] = want;
        } else if (g_weaponHandForced[i] >= 0) {
            const bool left = userLefty || NativeLeftHanded(p);
            mem::Write<uint8_t>(p + kHandedness, left ? 1 : 0);
            if (i == 0 && g_weaponHandForced[i] == 1 && !userLefty) LeftyCamera(false);
            g_weaponHandForced[i] = -1;
        }
    }
}

// The half turn is the male skeleton's (Chris: joints 8 / 13 bound alike); a female one (Sheva, Jill...) holds a weapon
// on joint 13 the way Sheva does natively, unturned (user, 2026-11-02: Jill with "Main inversée" aimed her VZ61 laser the
// other way and never hit Wesker).
void UpdateTurnedWeapons() {
    for (int i = 0; i < 2; ++i) {
        uintptr_t target = 0, p = g_coordWorldHooked && InGame() ? Player(i) : 0, weapon = 0, parent = 0;
        uint8_t hand = 0;
        uint32_t joint = 0;
        if (p && mem::Read(p + kHandedness, hand) && hand == 1 && mem::Read(p + kHeldWeapon, weapon) && weapon &&
            mem::Read(weapon + kWeaponParent, parent) && parent == p && mem::Read(weapon + kWeaponJoint, joint) &&
            joint == 13 && !NativeLeftHanded(p) && !NativeFemaleAnims(p))
            target = weapon;
        g_turnedWeapon[i].store(target, std::memory_order_relaxed);
    }
}

// ---- Size / speed per player (UT "Set Size" / "Set Speed") -------------------------------------
constexpr uintptr_t kScale = 0x50;       // float x, y, z
constexpr uintptr_t kBaseScale = 0x1E10; // character-record scale
constexpr uintptr_t kSpeed = 0x3A4;

struct SavedSize {
    float scale[3] = { 1, 1, 1 };
    float base = 1;
    float speed = 1;
} g_savedSize[2], g_savedSpeed[2];

// ---- Rapid fire on every weapon of a player: inventory records P+0x21A0 + k*0x30, +0x25 = 1 -------
constexpr uintptr_t kInventory = 0x21A0;
uint8_t g_savedRapid[2][9] = {};

uintptr_t Profile() {
    uintptr_t p = 0;
    mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), p);
    return p;
}
}

bool PlayerHitLogOn() { return g_hitLog.load(std::memory_order_relaxed); }

bool EnsurePlayerVoiceHook() { return EnsureVoiceHook(); }  // wesker_moves.cpp

bool Player2Invisible() { return P2Hidden(); }  // action_log.cpp

// stamina.cpp: player 1's HUD (uIdCockpit00), 0 until its players were set once.
uintptr_t Player1Cockpit() { return actors::InGame() ? g_p1Cockpit.load(std::memory_order_relaxed) : 0; }
// knife_icon.cpp: player 1's HUD updated within `ms` (the game skips it while no HUD is shown).
bool Player1CockpitUpdated(DWORD ms) {
    const DWORD tick = g_p1CockpitTick.load(std::memory_order_relaxed);
    return tick && static_cast<int32_t>(GetTickCount() - tick) < static_cast<int32_t>(ms);
}
bool EnsureCockpitWatch() { return EnsureCockpitHook(); }
// knife_icon.cpp / stamina.cpp: player 1's health ring on screen (user, 2026-10-29: the trainer's HUD stayed in the pause
// menu and in-game cutscenes): his HUD updated within 300 ms and drawn - its draw (uIdCockpit00 vtable slot 9, VA
// 0x9F8EF0) draws nothing while its byte +0xC3 is 0 (0 with the pause menu open, read live; +0x14 stayed 2 then).
bool Player1HudShown() {
    const uintptr_t cockpit = Player1Cockpit();
    uint8_t drawn = 0;
    return cockpit && mem::Read(cockpit + 0xC3, drawn) && drawn && Player1CockpitUpdated(300);
}

bool EnsureActorDamageHook() { return EnsureDamageHook(); }  // wesker_boss.cpp

bool EnsureAttachedMatrixHook() { return EnsureCoordWorldHook(); }  // rocket_catch.cpp

// Every frame: player 1's bow when it hangs on a man's back (joint id 117).
void UpdateBackBow() {
    uintptr_t target = 0;
    const uintptr_t p = InGame() ? Player(0) : 0;
    uint8_t female = 0;
    if (p && mem::Read(p + 0x1AF8, female) && !female) {
        for (int k = 0; k < 24 && !target; ++k) {
            uint32_t id = 0, joint = 0;
            uintptr_t unit = 0, parent = 0;
            if (mem::Read(p + 0x21A4 + 0x30 * k + 4, id) && (id & 0xFFFF) == 0x010F && mem::Read(p + 0x21A4 + 0x30 * k + 0x2C, unit) &&
                unit && mem::Read(unit + kWeaponParent, parent) && parent == p && mem::Read(unit + kWeaponJoint, joint) && joint == 117)
                target = unit;
        }
    }
    if (target && !EnsureCoordWorldHook()) target = 0;
    g_backBow.store(target, std::memory_order_relaxed);
}

void SetReverseGrip(uintptr_t unit) { g_reverseGrip.store(unit, std::memory_order_relaxed); }
// The Majini moves its weapon by its own correction every frame (VA 0x85DA30: its +0x1B6C, +0x1F58, +0x1F84... shifted by its +0x1400
// through VA 0xB540D0, thiscall(vector), ret 4 - the world place and every joint; traced 2026-11-11 on an axe's world place, the only
// writers being it and the hooked VA 0x48ABA0): the stolen weapon, made from player 1's hand, was pushed up to 140 cm off it during the
// pair (logged; user: "the weapon is 50 cm to Chris's left"). That shift is skipped for it.
using UnitShiftFn = void(__fastcall*)(void* self, void* edx, const float* delta);
UnitShiftFn g_unitShift = nullptr;
bool g_unitShiftHooked = false;
void __fastcall HookUnitShift(void* self, void* edx, const float* delta) {
    if (self && reinterpret_cast<uintptr_t>(self) == g_stolenWeapon.load(std::memory_order_relaxed)) return;
    g_unitShift(self, edx, delta);
}
bool EnsureUnitShiftHook() {
    if (g_unitShiftHooked) return true;
    const uint8_t prologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x55, 0x08 };
    g_unitShiftHooked = HookAt(0x7540D0, prologue, sizeof(prologue), &HookUnitShift, reinterpret_cast<void**>(&g_unitShift), "unit shift");
    return g_unitShiftHooked;
}

void SetStolenWeaponHand(uint32_t joint) { g_stolenJoint.store(joint, std::memory_order_relaxed); }

// wesker_moves.cpp: the stolen weapon let go where its world matrix is now (player 1's hand), to fall to `floor` (his feet's height).
void DropStolenWeapon(uintptr_t unit, uintptr_t majini, float floor) {
    float m[16]{};
    if (!unit || !mem::SafeRead(reinterpret_cast<void*>(unit + 0xA0), m, sizeof(m)) || !EnsureCoordWorldHook()) return;
    g_droppedBusy.store(true, std::memory_order_release);
    int slot = 0;
    for (int i = 0; i < kDroppedMax; ++i)
        if (!g_dropped[i].unit || g_dropped[i].unit == unit) {
            slot = i;
            break;
        } else if (g_dropped[i].start < g_dropped[slot].start) {
            slot = i;  // the oldest given up
        }
    DroppedWeapon& d = g_dropped[slot];
    d = {};
    std::memcpy(d.m, m, sizeof(m));
    // Its most upright axis (a held club, axe or wrench hangs along its handle) laid flat: turned about the horizontal axis across it.
    int up = 0;
    for (int row = 1; row < 3; ++row)
        if (std::fabs(m[row * 4 + 1]) > std::fabs(m[up * 4 + 1])) up = row;
    const float* a = &m[up * 4];
    float across[3] = { a[2], 0.0f, -a[0] };  // up x a, horizontal
    const float len = std::hypot(across[0], across[2]);
    if (len > 1e-3f) {
        across[0] /= len;
        across[2] /= len;
        const float alen = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        d.angle = alen > 1e-3f ? std::asin(std::max(-1.0f, std::min(1.0f, a[1] / alen))) : 0.0f;
        std::memcpy(d.axis, across, sizeof(across));
    } else {
        d.axis[0] = 1.0f;  // straight up or down: about the world's X
        d.angle = 1.5707963f * (a[1] > 0.0f ? 1.0f : -1.0f);
    }
    d.floor = floor + 3.0f;
    d.fall = std::max(0.25f, std::sqrt(2.0f * std::max(0.0f, m[13] - d.floor) / 980.0f));
    d.parent = majini;
    d.start = GetTickCount64();
    d.unit = unit;
    int count = 0;
    for (const DroppedWeapon& x : g_dropped) count += x.unit ? 1 : 0;
    g_droppedCount.store(count, std::memory_order_relaxed);
    g_droppedBusy.store(false, std::memory_order_release);
    LOG("stolen weapon: %08X let go at (%.0f %.0f %.0f), falls %.2f s to %.0f, axis %d laid by %.0f deg", static_cast<unsigned>(unit), m[12],
        m[13], m[14], d.fall, d.floor, up, d.angle * 57.2958f);
}

void SetStolenWeapon(uintptr_t unit, uintptr_t from, uintptr_t holder) {
    if (unit && !EnsureCoordWorldHook()) unit = 0;
    if (unit && !EnsureUnitShiftHook()) LOG("stolen weapon: no unit shift hook, the Majini's correction will move it");
    g_stolenWeapon.store(0, std::memory_order_relaxed);
    if (!unit) g_stolenJoint.store(0, std::memory_order_relaxed);
    g_stolenFrom.store(from, std::memory_order_relaxed);
    g_stolenHolder.store(holder, std::memory_order_relaxed);
    g_stolenWeapon.store(unit, std::memory_order_relaxed);
}

void SetWeaponJointUnit(uintptr_t unit, int index) {
    g_weaponJointIndex.store(index, std::memory_order_relaxed);
    g_weaponJointUnit.store(unit, std::memory_order_relaxed);
}

// camera.cpp's iron sights: a held weapon moved in the hand (its local frame, cm); 0 = none.
void SetWeaponLift(uintptr_t unit, const float offset[3]) {
    for (int axis = 0; axis < 3; ++axis) g_lift[axis].store(offset ? offset[axis] : 0.0f, std::memory_order_relaxed);
    g_liftUnit.store(unit, std::memory_order_relaxed);
}

// "Invincible" for a player (grabs.cpp: the easier grabs give no contact to an invincible player 1).
bool PlayerInvincible(int index) {
    return index >= 0 && index < 4 && g_invinciblePlayer[index].load(std::memory_order_relaxed);
}

// perfect_dodge.cpp: player 1 invincible for the window after a dodge and the rest of a perfect one.
bool SetDodgeInvincible(bool on) { return DodgeInvincible(on); }

// swaps.cpp: player 1 using the other hand than his character's ("Main inversée", the bow's hand): his motions mirrored.
bool Player1HandInverted() {
    const uintptr_t p = InGame() ? Player(0) : 0;
    uint8_t hand = 0;
    return p && mem::Read(p + kHandedness, hand) && (hand != 0) != NativeLeftHanded(p);
}

// swaps.cpp: player 1's character left-handed by the game (his own attack shapes made for that hand).
bool Player1NativeLeftHanded() {
    const uintptr_t p = InGame() ? Player(0) : 0;
    return p && NativeLeftHanded(p);
}

// dying.cpp ("Me mettre mourant", user 2026-10-30, for the tests): player 1's health taken off through his own
// SubHealth past the trainer's refusals (the original when hooked: invincibility, perfect dodges...); a killing amount
// puts him dying as a killing hit does.
bool HurtPlayer1Raw(int16_t amount) {
    const uintptr_t p = Player(0);
    if (!p || amount <= 0) return false;
    const SubHealthFn fn = g_playerSubHealth ? g_playerSubHealth : reinterpret_cast<SubHealthFn>(mem::Rva(0x77C950));
    __try {
        fn(reinterpret_cast<void*>(p), nullptr, amount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void RegisterPlayerFeatures() {
    Add({ .id = "diag.player_hit_log", .name = "Journal des coups reçus (recherche)",
          .help = "Écrit dans trainer.log, dans l'ordre, chaque coup reçu par le joueur 1 (attaquant, type de coup), ce qu'il "
                  "fait ensuite, ses pertes de vie et les actions des ennemis proches. Sert à comprendre les saisies.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureDamageHook() || !EnsureHitHook() || !EnsurePlayerRequestHook() || !EnsureEnemyActionHook()))
                  return false;
              g_hitLog = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "diag.damage_log", .name = "Journal des dégâts aux ennemis (recherche)",
          .help = "Écrit dans trainer.log chaque façon dont un ennemi perd de la vie (chemin dans le code) avec ce que fait le "
                  "joueur 1. Sert à comprendre pourquoi un coup ne touche pas.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureDamageHook()) return false;
              g_damageLog = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    AddFlagToggle("player.god_p1", "Santé infinie — Joueur 1",
                  "Le joueur 1 ne perd jamais de vie (les autres personnages et les ennemis ne sont pas affectés).", "Joueur",
                  g_godPlayer[0]);
    AddFlagToggle("player.god_p2", "Santé infinie — Joueur 2 / partenaire",
                  "Le joueur 2 (Sheva/IA ou 2e joueur) ne perd jamais de vie.", "Joueur", g_godPlayer[1]);
    for (int i = 0; i < 2; ++i) {
        static const char* ids[] = { "player.invincible_p1", "player.invincible_p2" };
        static const char* names[] = { "Invincible — Joueur 1", "Invincible — Joueur 2 / partenaire" };
        Add({ .id = ids[i], .name = names[i],
              .help = "Le joueur ne peut plus être touché : pas d'impact, pas de sang ni de son, pas de saisie, pas de dégâts. "
                      "Ses propres attaques comptent toujours (les ennemis restent tuables).",
              .category = "Joueur", .kind = Kind::Toggle,
              .set = [i](bool on) { return SetInvincible(i, on); },
              .tick = [] { UpdateInvincibleSlots(); },  // player objects change on level load
              .available = [] { return game::IsSupported(); } });
    }
    Add({ .id = "player.heal_p1", .name = "Soigner le joueur 1", .help = "Remet la vie au maximum.", .category = "Joueur",
          .kind = Kind::Action, .run = [] { return Heal(0); }, .available = InGame });
    Add({ .id = "player.heal_p2", .name = "Soigner le joueur 2", .help = "Remet la vie au maximum.", .category = "Joueur",
          .kind = Kind::Action, .run = [] { return Heal(1); }, .available = InGame });
    for (int i = 0; i < 2; ++i) {
        static const char* const ids[2] = { "player.max_health_p1", "player.max_health_p2" };
        static const char* const names[2] = { "Vie max du joueur 1", "Vie max du joueur 2" };
        Feature& f = Add({ .id = ids[i], .name = names[i],
                           .help = "Le maximum de vie du joueur devient la valeur du curseur (1000 dans le jeu) ; sa vie "
                                   "suit dans la même proportion (pleine reste pleine). Décochée : le maximum du jeu "
                                   "revient.",
                           .category = "Joueur", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = 1000, .valueMin = 100, .valueMax = 30000, .valueFormat = "%.0f PV" });
        Feature* const fp = &f;
        f.set = [i, fp](bool on) {
            g_maxHealth[i].wanted = fp->value;
            g_maxHealth[i].on = on;
            if (!on) RestoreMaxHealth(i);
            return true;
        };
        f.tick = [i, fp] {
            g_maxHealth[i].wanted.store(fp->value, std::memory_order_relaxed);
            TickMaxHealth(i);
        };
    }
    Add({ .id = "player.max_health_default", .name = "Vie max : valeur du jeu",
          .help = "Remet les curseurs « Vie max du joueur 1 / 2 » sur la valeur du jeu (1000). Les cases restent comme "
                  "elles sont.",
          .category = "Joueur", .kind = Kind::Action,
          .run = [] {
              for (const char* id : { "player.max_health_p1", "player.max_health_p2" }) {
                  Feature* f = Find(id);
                  if (!f) continue;
                  f->value = 1000.0f;
                  settings::SaveFeature(*f);
              }
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnRestore([] {
        RestoreMaxHealth(0);
        RestoreMaxHealth(1);
    });
    Add({ .id = "player.fix_accessories", .name = "Replacer les accessoires",
          .help = "Les mouvements de Wesker et de Jill joués par le joueur 1 (garde RE7, coups...) déplacent aussi ses "
                  "accessoires (arme dans le dos, étui du couteau, sacoches), que ses propres animations ne remettent "
                  "jamais. Cochée : ils reprennent la place qu'ils avaient avant le mouvement, celle du personnage "
                  "joué.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_accessoryFix = on;
              g_accessoryFrames = 0;
              g_accessoryJoints = 0;
              return true;
          },
          .tick = TickAccessoryJoints,
          .available = [] { return game::IsSupported(); },
          .defaultOn = true });
    Add({ .id = "player.no_collision", .name = "Pas de collision",
          .help = "Traverse les murs et décors. Désactiver vous ramène au sol.", .category = "Joueur", .kind = Kind::Toggle,
          .set = SetNoCollision, .tick = TickNoCollision, .available = InGame });
    Add({ .id = "player.no_collision_keep_place", .name = "Rester sur place en réactivant les collisions",
          .help = "En décochant « Pas de collision », le joueur reste là où il est au lieu d'être ramené vers l'endroit "
                  "où il l'avait cochée (le jeu reprenait le trajet depuis cet endroit et s'arrêtait au premier mur).",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_keepPlace = on;
              return true;
          },
          .available = [] { return game::IsSupported(); }, .defaultOn = true });
    g_flyUp = &Add({ .id = "player.no_collision_up", .name = "Monter (sans collision)",
                     .help = "Avec « Pas de collision », le joueur 1 monte tant que la touche est tenue (Increase Z de "
                             "l'Ultimate Trainer). Choisis la touche dans la colonne Raccourci ; le curseur règle la vitesse "
                             "(cm/s).",
                     .category = "Joueur", .kind = Kind::Toggle, .set = [](bool) { return true; },
                     .available = [] { return game::IsSupported(); },
                     .value = 300, .valueMin = 50, .valueMax = 1500, .valueFormat = "%.0f cm/s",
                     .defaultOn = true, .holdKey = true });
    g_flyDown = &Add({ .id = "player.no_collision_down", .name = "Descendre (sans collision)",
                       .help = "Avec « Pas de collision », le joueur 1 descend tant que la touche est tenue (Decrease Z de "
                               "l'Ultimate Trainer). Choisis la touche dans la colonne Raccourci ; le curseur règle la "
                               "vitesse (cm/s).",
                       .category = "Joueur", .kind = Kind::Toggle, .set = [](bool) { return true; },
                       .available = [] { return game::IsSupported(); },
                       .value = 300, .valueMin = 50, .valueMax = 1500, .valueFormat = "%.0f cm/s",
                       .defaultOn = true, .holdKey = true });
    OnFrame(TickFly);
    AddHeightFreeze("player.freeze_height_p1", "Figer la hauteur — Joueur 1",
                    "Le joueur 1 reste à sa hauteur actuelle : il ne tombe plus et marche dans le vide au-dessus des trous "
                    "(Freeze Z de l'Ultimate Trainer). Avec « Pas de collision », permet de se déplacer à l'horizontale.",
                    &g_heightP1);
    AddHeightFreeze("player.freeze_height_p2", "Figer la hauteur — Joueur 2",
                    "Le joueur 2 reste à sa hauteur actuelle (Freeze Z « AI Player » de l'Ultimate Trainer).", &g_heightP2);
    // Handedness applies live. The weapon-anim set (+0x1AF8) needs a checkpoint/chapter reload: the female
    // weapon motion list is only loaded at player init (RVA 0x77E4E6) when the flag is set (user-verified).
    // The game picks a motion's hand (+0x1AF9) when an action starts (user, 2026-10-27: standing, running or aiming the
    // motions only changed at the next action): the current action is asked again when the hand changes, if it is one
    // that may start over - wait, walks, run, turns (0x00-0x08), aim loop 0x1C, knife aim loop 0x23.
    static auto restartHanded = [](uintptr_t p, uint8_t hand) {
        uint32_t action = 0;
        uintptr_t vtable = 0, fn = 0;
        if (!p || !mem::Write<uint8_t>(p + kHandedness, hand) || !mem::Read(p + 0x10E0, action)) return;
        if (action > 0x08 && action != 0x1C && action != 0x23) return;
        if (!mem::Read(p, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return;
        using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
        __try {
            reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(p), nullptr, static_cast<int>(action), 0, 0, -1, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    };
    {
        // "Main inversée" (user, 2026-10-26: Sheva, left-handed by the game, did not change - "Gaucher" put her left
        // and off put her own hand, left again; promised 2026-10-14: "Chris left, Sheva right"): the other hand than
        // the character's own (record +0x1C, NativeLeftHanded), the camera on that shoulder.
        Feature& lefty = AddFieldFreeze("player.lefty_p1", "Main inversée — Joueur 1",
                                        "Le joueur 1 tient ses armes de l'autre main que la sienne (Chris de la main gauche, "
                                        "Sheva de la main droite), et la caméra passe sur l'autre épaule (effet immédiat).",
                                        "Personnage", &g_leftyP1);
        auto baseSet = lefty.set;
        auto baseTick = lefty.tick;
        // Its key may serve the inventory's shortcuts too (user, 2026-10-27): ignored while the inventory is open.
        lefty.hotkeyIgnored = [] { return Player1InventoryOpen(); };
        lefty.set = [baseSet](bool on) {
            const uintptr_t p = Player(0);
            if (on && p) g_leftyP1.value = NativeLeftHanded(p) ? 0 : 1;
            if (on) EnsureCoordWorldHook();
            if (!baseSet(on)) return false;
            // Off: the character's own handedness, not the value captured when switched on (after trainer reloads
            // it could already be "left": the player stayed lefty, user 2026-10-13).
            if (p) restartHanded(p, on ? static_cast<uint8_t>(g_leftyP1.value) : (NativeLeftHanded(p) ? 1 : 0));
            ShoulderCamera(on, g_leftyP1.value == 1);
            return true;
        };
        lefty.tick = [baseTick] {
            if (const uintptr_t p = Player(0)) g_leftyP1.value = NativeLeftHanded(p) ? 0 : 1;  // a new level, a new character
            baseTick();
            ShoulderCamera(true, g_leftyP1.value == 1);  // the game may reload camera values (weapon change...)
        };
    }
    {
        Feature& lefty = AddFieldFreeze("player.lefty_p2", "Main inversée — Joueur 2",
                                        "Le joueur 2 tient ses armes de l'autre main que la sienne (Chris de la main gauche, "
                                        "Sheva de la main droite ; effet immédiat). Sans joueur 2 en jeu, refusée.",
                                        "Personnage", &g_leftyP2);
        auto baseSet = lefty.set;
        auto baseTick = lefty.tick;
        lefty.hotkeyIgnored = [] { return Player1InventoryOpen(); };
        lefty.tick = [baseTick] {
            if (const uintptr_t p = Player(1)) g_leftyP2.value = NativeLeftHanded(p) ? 0 : 1;
            baseTick();
        };
        lefty.set = [baseSet](bool on) {
            if (on)
                if (const uintptr_t p = Player(1)) g_leftyP2.value = NativeLeftHanded(p) ? 0 : 1;
            if (on) EnsureCoordWorldHook();
            if (!baseSet(on)) return false;
            if (const uintptr_t p = Player(1))
                restartHanded(p, on ? static_cast<uint8_t>(g_leftyP2.value) : (NativeLeftHanded(p) ? 1 : 0));
            return true;
        };
    }
    OnFrame(UpdateTurnedWeapons);
    OnFrame(UpdateBackBow);
    OnFrame(UpdateWeaponHand);
    // The prompts' calls into this DLL go before it is unloaded (user, 2026-10-27: left after a hot reload, they jumped
    // into the next instance's code and the game crashed drawing a prompt).
    OnRestore([] { SetPromptHand(false); });
    OnFrame(ForgetLeftyCamera);
    Add({ .id = "char.weapon_hand", .name = "Main adaptée à l'arc et à la Gatling",
          .help = "Quand l'arc (arme de Sheva, gauchère) est en main, le personnage passe gaucher, caméra comprise ; avec "
                  "la Gatling (arme de Chris), il passe droitier. Retour à la main habituelle en changeant d'arme. "
                  "« Main inversée » cochée garde la priorité pour la Gatling.",
          .category = "Personnage", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on) EnsureCoordWorldHook();  // the bow turned into Chris's left hand
              g_weaponHand = on;
              return true;
          },
          .defaultOn = true });
    {
        static const char* ids[3] = { "player.lefty_weapon_x", "player.lefty_weapon_y", "player.lefty_weapon_z" };
        static const char* names[3] = { "Arme en gaucher : avant / arrière", "Arme en gaucher : côté",
                                         "Arme en gaucher : haut / bas" };
        // Tuned by the user on Chris with the pistol (2026-10-13). The side value is about twice the lateral
        // offset of the pistol model's bounding-sphere centre (-2.3): the half turn mirrors it to the other side.
        static const float defaults[3] = { 1.4f, -4.8f, 1.6f };
        for (int axis = 0; axis < 3; ++axis) {
            Feature& f = Add({ .id = ids[axis], .name = names[axis],
                               .help = "Ajuste la position de l'arme dans la main gauche (option Gaucher, personnages "
                                       "droitiers). Cocher pour appliquer la valeur du curseur, effet immédiat.",
                               .category = "Personnage", .kind = Kind::Toggle,
                               .set = [axis](bool on) {
                                   if (!on) g_turnedOffset[axis] = 0.0f;
                                   return true;
                               },
                               .value = defaults[axis], .valueMin = -10, .valueMax = 10, .valueFormat = "%.1f",
                               .defaultOn = true });
            Feature* self = &f;
            f.tick = [axis, self] { g_turnedOffset[axis] = self->value; };
        }
    }
    // Inverted, not "female": Chris takes Sheva's weapon motions, Sheva Chris's (user, 2026-10-14: the option did
    // nothing for Sheva, whose own set is already the female one). Off = the character's own set.
    for (int i = 0; i < 2; ++i) {
        FieldFreeze* ff = i == 0 ? &g_femaleP1 : &g_femaleP2;
        Feature& inverted = AddFieldFreeze(i == 0 ? "player.female_anim_p1" : "player.female_anim_p2",
                                           i == 0 ? "Animations d'armes inversées — J1" : "Animations d'armes inversées — J2",
                                           "Le personnage prend les animations d'armes de l'autre sexe : Chris celles de "
                                           "Sheva, Sheva celles de Chris. Tout de suite pour l'arme en main, les autres "
                                           "armes dès qu'elles sont sorties.",
                                           "Personnage", ff);
        auto baseSet = inverted.set;
        auto baseTick = inverted.tick;
        inverted.set = [baseSet, ff, i](bool on) {
            const uintptr_t p = Player(i);
            if (p) ff->value = NativeFemaleAnims(p) ? 0 : 1;
            if (!baseSet(on)) return false;
            if (!on && p) mem::Write<uint8_t>(p + kWeaponAnim, NativeFemaleAnims(p) ? 1 : 0);
            RefreshWeaponMotion(i);
            return true;
        };
        inverted.tick = [baseTick, ff, i] {
            if (const uintptr_t p = Player(i)) ff->value = NativeFemaleAnims(p) ? 0 : 1;
            baseTick();
        };
    }
    Add({ .id = "combat.p2_no_attack", .name = "Le joueur 2 n'attaque pas",
          .help = "Le partenaire (joueur 2) n'attaque plus : pas de tir avec ses armes (il vise sans tirer), pas de "
                  "couteau ni de coup au corps à corps. Les tourelles et les libérations quand tu es saisi ont leur "
                  "propre case.",
          .category = "Combat", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureShotHooks()) return false;
              if (!BlockPlayer2Actions(0, on)) return false;
              g_p2NoShot = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "combat.p2_no_turret", .name = "Le joueur 2 ne tire pas à la tourelle",
          .help = "Le partenaire (joueur 2) ne tire plus avec les armes montées : tourelle du camion, mitrailleuses fixes. "
                  "Il reste à son poste et vise sans tirer.",
          .category = "Combat", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureShotHooks()) return false;
              g_p2NoTurret = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    {
        struct P2Option {
            const char* id;
            const char* name;
            const char* help;
            int category;
        };
        static const P2Option options[] = {
            { "combat.p2_no_pickup", "Le joueur 2 ne ramasse pas les objets",
              "Le partenaire (joueur 2) ne ramasse plus rien : munitions, soins, trésors. Exception : la lampe des "
              "mines, qu'il peut toujours prendre pour éclairer (une fois en main, plus rien d'autre), sauf quand une "
              "« Lampe à la ceinture » est allumée.", 1 },
            { "combat.p2_no_revive", "Le joueur 2 ne te réanime pas",
              "Le partenaire ne vient plus te relever quand tu es mourant et ne t'applique plus de soins.", 2 },
            { "combat.p2_no_grab_rescue", "Le joueur 2 ne te libère pas des prises",
              "Le partenaire ne te libère plus quand un ennemi te saisit (Majini, licker, parasite).", 3 },
        };
        for (const P2Option& option : options) {
            const int category = option.category;
            Add({ .id = option.id, .name = option.name, .help = option.help, .category = "Combat", .kind = Kind::Toggle,
                  .set = [category](bool on) { return BlockPlayer2Actions(category, on); },
                  .available = [] { return game::IsSupported(); } });
        }
    }
    AddFlagToggle("enemies.never_die", "Ennemis immortels", "Les ennemis ne perdent plus de vie (les joueurs ne sont pas affectés).",
                  "Ennemis", g_enemyNeverDie);
    AddFlagToggle("enemies.one_hit_kill", "Tuer en un coup", "Chaque coup porté à un ennemi le tue (les joueurs ne sont pas affectés).",
                  "Combat", g_oneHitKill);
    {
        Feature& power = Add({ .id = "ammo.truck_turret_power", .name = "Puissance des tourelles du camion",
                               .help = "Multiplie les dégâts des tourelles du camion sur le Gigante (chapitre 2-3) : il "
                                       "plie (stagger) plus vite, de quoi le battre sans l'aide de Sheva. Actif seulement "
                                       "quand un joueur tient une tourelle du camion.",
                               .category = "Munitions", .kind = Kind::Toggle,
                               .available = [] { return game::IsSupported(); },
                               .value = 2.0f, .valueMin = 1.0f, .valueMax = 10.0f, .valueFormat = "x%.1f" });
        Feature* const pf = &power;
        power.set = [pf](bool on) {
            if (on && !EnsureGiganteHooks()) return false;
            g_truckPower = pf->value;
            g_truckPowerOn = on;
            return true;
        };
        power.tick = [pf] { g_truckPower.store(pf->value, std::memory_order_relaxed); };
    }
    Add({ .id = "char.p2_invisible", .name = "Joueur 2 invisible",
          .help = "Le partenaire (joueur 2), ses armes et sa jauge de vie ne sont plus affichés, on ne l'entend plus parler "
                  "et il ne bloque plus ton passage ; il agit normalement.",
          .category = "Personnage", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureCockpitHook() || !EnsureContactHook() || !EnsureVoiceHook())) return false;
              g_p2Invisible = on;
              if (P2Hidden()) {
                  HidePlayer2();
                  g_p2HiddenNow = true;
              } else {
                  ShowPlayer2();
                  g_p2HiddenNow = false;
              }
              return true;
          },
          .tick = [] {
              if (!InGame()) {
                  g_hiddenUnits.clear();  // level change: the units are rebuilt
                  g_p2HiddenNow = false;
              } else if (P2Hidden()) {
                  HidePlayer2();
                  g_p2HiddenNow = true;
              } else if (g_p2HiddenNow) {  // the final fight began without a load: shown once
                  ShowPlayer2();
                  g_p2HiddenNow = false;
              }
          },
          .available = InGame });
    AddFlagToggle("vehicle.invincible", "Véhicule indestructible",
                  "Le véhicule où sont montées vos tourelles (camion du chapitre 2-3) ne perd plus de vie.", "Joueur",
                  g_vehicleInvincible);

    Add({ .id = "game.wheel_for_trainer", .name = "Molette réservée au trainer",
          .help = "Le jeu ne voit plus la molette de la souris (plus de changement d'arme à la molette) : elle sert aux "
                  "raccourcis « Molette haut » / « Molette bas » des options du trainer. Dans les menus du jeu (aucun "
                  "personnage chargé), la molette reste au jeu.",
          .category = "Jeu", .kind = Kind::Toggle,
          .set = [](bool on) {
              hotkeys::SetWheelBlocked(on);
              return true;
          } });
    Feature& fps = Add({ .id = "display.uncap_fps", .name = "Limite d'images par seconde",
                         .help = "Remplace la limite d'images par seconde du jeu (120) par celle du curseur. Les parasites souples gardent leur vitesse à toute fréquence (le jeu arrondissait leur animation : deux fois trop rapides à 120, figés au-delà).",
                         .category = "Graphismes", .kind = Kind::Toggle, .available = InGame,
                         .value = 144, .valueMin = 30, .valueMax = 240, .valueFormat = "%.0f FPS" });
    Feature* const pf = &fps;
    fps.set = [pf](bool on) {
        if (on ? !g_parasiteFrame.Apply() : !g_parasiteFrame.Restore()) LOG("FPS limit: the soft parasite's frame patch not %s", on ? "applied" : "restored");
        return SetFpsCap(on, FpsCapHere(pf->value));
    };
    fps.tick = [pf] {
        const float wanted = FpsCapHere(pf->value);
        // The game puts its own 120 back (user, 2026-11-10, Mercenaries: the slider at 240, the game at 120 - read live, the trainer still
        // believing its 240 was there): the live value checked too.
        float live = 0.0f;
        const float* cap = FpsCap();
        const bool reset = cap && mem::Read(reinterpret_cast<uintptr_t>(cap), live) && live != wanted && live >= 10.0f && live <= 1000.0f;
        if (wanted == g_appliedFps && !reset) return;
        const bool changed = wanted != g_appliedFps;
        SetFpsCap(true, wanted);  // its own value kept as the one to give back (SetFpsCap: it differs from ours)
        static ULONGLONG told = 0;  // a game that puts it back again and again: logged once in 10 s
        if (changed || GetTickCount64() - told > 10000) {
            told = GetTickCount64();
            LOG("FPS limit: %.0f (slider %.0f)%s", wanted, pf->value, changed ? "" : ", the game had put its own limit back");
        }
    };

    for (int i = 0; i < 2; ++i) {
        static const char* sizeIds[] = { "player.size_p1", "player.size_p2" };
        static const char* sizeNames[] = { "Taille — Joueur 1", "Taille — Joueur 2" };
        static const char* speedIds[] = { "player.speed_p1", "player.speed_p2" };
        static const char* speedNames[] = { "Vitesse — Joueur 1", "Vitesse — Joueur 2" };
        Feature& size = Add({ .id = sizeIds[i], .name = sizeNames[i],
                              .help = "Taille du personnage (1 = normale). Avec « Taille fixe » pour garder la taille en sautant "
                                      "ou sur les échelles. Pour un joueur que tu ne contrôles pas, rester à 1 évite des ralentissements.",
                              .category = "Joueur", .kind = Kind::Toggle, .available = InGame,
                              .value = 1.0f, .valueMin = 0.2f, .valueMax = 3.0f });
        Feature* sizeSelf = &size;
        size.set = [i](bool on) {
            uintptr_t p = Player(i);
            if (!p) return false;
            SavedSize& sv = g_savedSize[i];
            if (on) {
                mem::SafeRead(reinterpret_cast<void*>(p + kScale), sv.scale, sizeof(sv.scale));
                mem::Read(p + kBaseScale, sv.base);
            } else {
                mem::SafeWrite(reinterpret_cast<void*>(p + kScale), sv.scale, sizeof(sv.scale));
                mem::Write(p + kBaseScale, sv.base);
            }
            return true;
        };
        size.tick = [i, sizeSelf] {
            if (uintptr_t p = Player(i)) {
                const float v = sizeSelf->value;
                for (int k = 0; k < 3; ++k) mem::Write(p + kScale + 4 * k, v);
                mem::Write(p + kBaseScale, v);
            }
        };
        Feature& speed = Add({ .id = speedIds[i], .name = speedNames[i],
                               .help = "Vitesse du personnage (1 = normale), animations comprises.", .category = "Joueur",
                               .kind = Kind::Toggle, .available = InGame, .value = 1.0f, .valueMin = 0.1f, .valueMax = 3.0f });
        Feature* speedSelf = &speed;
        speed.set = [i](bool on) {
            uintptr_t p = Player(i);
            if (!p) return false;
            if (on) mem::Read(p + kSpeed, g_savedSpeed[i].speed);
            else mem::Write(p + kSpeed, g_savedSpeed[i].speed > 0 ? g_savedSpeed[i].speed : 1.0f);
            return true;
        };
        speed.tick = [i, speedSelf] {
            if (uintptr_t p = Player(i)) mem::Write(p + kSpeed, speedSelf->value);
        };

        static const char* rapidIds[] = { "inventory.rapid_fire_p1", "inventory.rapid_fire_p2" };
        static const char* rapidNames[] = { "Tir rapide — Joueur 1", "Tir rapide — Joueur 2" };
        Add({ .id = rapidIds[i], .name = rapidNames[i],
              .help = "Toutes les armes de l'inventaire tirent en rafale. Éviter avec les armes explosives.",
              .category = "Inventaire", .kind = Kind::Toggle,
              .set = [i](bool on) {
                  uintptr_t p = Player(i);
                  if (!p) return false;
                  for (int k = 0; k < 9; ++k) {
                      const uintptr_t flag = p + kInventory + 0x30 * k + 0x25;
                      if (on) mem::Read(flag, g_savedRapid[i][k]);
                      else mem::Write(flag, g_savedRapid[i][k]);
                  }
                  return true;
              },
              .tick = [i] {
                  if (uintptr_t p = Player(i))
                      for (int k = 0; k < 9; ++k) mem::Write<uint8_t>(p + kInventory + 0x30 * k + 0x25, 1);
              },
              .available = InGame });
    }

    Add({ .id = "player.teleport_p2_to_p1", .name = "Amener le joueur 2 à moi",
          .help = "Téléporte le joueur 2 (Sheva/IA) à la position du joueur 1.", .category = "Joueur", .kind = Kind::Action,
          .run = [] {
              uintptr_t a = Player(0), b = Player(1);
              float pos[3]{};
              return a && b && mem::SafeRead(reinterpret_cast<void*>(a + 0x30), pos, sizeof(pos)) &&
                     mem::SafeWrite(reinterpret_cast<void*>(b + 0x30), pos, sizeof(pos));
          },
          .available = InGame });
    Add({ .id = "player.teleport_p1_to_p2", .name = "Aller vers le joueur 2",
          .help = "Téléporte le joueur 1 à la position du joueur 2.", .category = "Joueur", .kind = Kind::Action,
          .run = [] {
              uintptr_t a = Player(0), b = Player(1);
              float pos[3]{};
              return a && b && mem::SafeRead(reinterpret_cast<void*>(b + 0x30), pos, sizeof(pos)) &&
                     mem::SafeWrite(reinterpret_cast<void*>(a + 0x30), pos, sizeof(pos));
          },
          .available = InGame });

    // TPS mode (UT's: action-state byte P+0x2D7F |= 0x20, actions almost instant) removed (user, 2026-11-05: "nul").
    // Friendly fire (UT): team byte P+0x1358, 0x70 for players; P1 0x7E / P2 0x7D make them hit each other.
    static uint8_t savedTeam[2] = { 0x70, 0x70 };
    Add({ .id = "combat.friendly_fire", .name = "Tir allié",
          .help = "Les joueurs peuvent se blesser entre eux (Chris peut toucher Sheva et inversement).",
          .category = "Combat", .kind = Kind::Toggle,
          .set = [](bool on) {
              for (int i = 0; i < 2; ++i)
                  if (uintptr_t p = Player(i)) {
                      if (on) mem::Read(p + 0x1358, savedTeam[i]);
                      else mem::Write<uint8_t>(p + 0x1358, savedTeam[i] == 0x7E || savedTeam[i] == 0x7D ? 0x70 : savedTeam[i]);
                  }
              return true;
          },
          .tick = [] {
              const uint8_t team[2] = { 0x7E, 0x7D };
              for (int i = 0; i < 2; ++i)
                  if (uintptr_t p = Player(i)) mem::Write<uint8_t>(p + 0x1358, team[i]);
          },
          .available = InGame });

    // UT "Disable Rapid Fire" / "Disable Infinite Ammo": inventory records P+0x21A0 + k*0x30, +0x25 / +0x27 = 0.
    for (int field = 0; field < 2; ++field) {
        static const char* ids[] = { "inventory.disable_rapid_fire", "inventory.disable_infinite_ammo" };
        static const char* names[] = { "Désactiver le tir rapide des armes", "Désactiver les munitions infinies des armes" };
        static const char* helps[] = {
            "Retire le tir rapide de toutes les armes des deux joueurs (bonus du jeu ou tir rapide du trainer).",
            "Retire les munitions infinies de toutes les armes des deux joueurs (bonus du jeu). Les munitions infinies "
            "du trainer restent utilisables." };
        const uintptr_t off = field == 0 ? 0x25 : 0x27;
        Add({ .id = ids[field], .name = names[field], .help = helps[field], .category = "Inventaire", .kind = Kind::Toggle,
              .set = [](bool) { return true; },
              .tick = [off] {
                  for (int i = 0; i < 2; ++i)
                      if (uintptr_t p = Player(i))
                          for (int k = 0; k < 9; ++k) mem::Write<uint8_t>(p + kInventory + 0x30 * k + off, 0);
              },
              .available = InGame });
    }


    // UT "Freeze Stage" / "Freeze Checkpoint": profile [0xDB2158] +0x70 chapter, +0x78 stage, +0x7C checkpoint
    // (UT wrote 5 / 0xC8 / 0 while the user was in chapter 5; 3 / 0x64 in chapter 3). Values captured when the
    // option is switched on, then imposed: the game stays on that stage / checkpoint.
    static uint32_t frozenStage[2] = {}, frozenCheckpoint = 0;
    Add({ .id = "game.freeze_stage", .name = "Figer l'étape",
          .help = "Le jeu reste sur le chapitre et l'étape actuels : en finissant l'étape ou en rechargeant, on recommence "
                  "la même.",
          .category = "Jeu", .kind = Kind::Toggle,
          .set = [](bool on) {
              uintptr_t profile = Profile();
              if (!profile) return false;
              if (on) {
                  mem::Read(profile + 0x70, frozenStage[0]);
                  mem::Read(profile + 0x78, frozenStage[1]);
              }
              return true;
          },
          .tick = [] {
              if (uintptr_t profile = Profile()) {
                  mem::Write(profile + 0x70, frozenStage[0]);
                  mem::Write(profile + 0x78, frozenStage[1]);
              }
          },
          .available = InGame });
    Add({ .id = "game.freeze_checkpoint", .name = "Figer le point de passage",
          .help = "Le jeu garde le point de passage actuel : en mourant ou en recommençant, on repart toujours du même.",
          .category = "Jeu", .kind = Kind::Toggle,
          .set = [](bool on) {
              uintptr_t profile = Profile();
              if (!profile) return false;
              if (on) mem::Read(profile + 0x7C, frozenCheckpoint);
              return true;
          },
          .tick = [] {
              if (uintptr_t profile = Profile()) mem::Write(profile + 0x7C, frozenCheckpoint);
          },
          .available = InGame });

    // The weapon in hand is the item object [P+0x2634]; bit 0x08 of its byte +5 = drawn. The game clears it during
    // melee moves (action 0x31...) and sets it back afterwards (actsnap capture, 2026-10-13). The Ultimate Trainer
    // set that bit on P+0x2FA5 = byte +5 of whatever object follows the player in memory (inventory slot 1's item):
    // pistol rounds appeared in Chris's hand during a punch (user). Here: only the held weapon, kept drawn.
    Add({ .id = "char.always_show_weapon", .name = "Arme toujours visible",
          .help = "L'arme tenue reste visible dans la main pendant les attaques au corps à corps.", .category = "Personnage",
          .kind = Kind::Toggle,
          .set = [](bool) { return true; },
          .tick = [] {
              for (int i = 0; i < 2; ++i) {
                  uintptr_t p = Player(i), weapon = 0;
                  if (i == 0 && RocketCatchHidesWeapon()) continue;  // rocket_catch.cpp: his hands hold the rocket
                  if (i == 0 && BorrowedMoveHidesWeapon()) continue;  // swaps.cpp: a tool in his hand (spray, taser...)
                  if (i == 0 && TrainerMoveHidesWeapon()) continue;   // wesker_moves.cpp: the enemy's weapon in his hands
                  if (CrankHidesWeapon(i)) continue;  // crank.cpp: both hands on a crank
                  if (PianoHidesWeapon(i)) continue;  // coop_solo.cpp: playing the piano, burning the paper
                  // "Joueur 2 invisible" hides it (user, 2026-10-26: the partner's weapon stayed visible, this option
                  // drawing it again every frame).
                  if (i == 1 && P2Hidden()) continue;
                  if (!p || !mem::Read(p + kHeldWeapon, weapon) || !weapon) continue;
                  bool owned = false;  // never write through a stale pointer: it must be one of the player's items
                  for (int k = 1; k <= 9 && !owned; ++k) {  // slot k-1's item object: next record's first dword
                      uintptr_t obj = 0;
                      owned = mem::Read(p + kInventory + 0x30 * k, obj) && obj == weapon;
                  }
                  uint8_t flags = 0;
                  if (owned && mem::Read(weapon + 5, flags) && !(flags & 0x08)) mem::Write<uint8_t>(weapon + 5, flags | 0x08);
              }
          },
          .available = InGame });

    static Feature* money = nullptr;
    money = &Add({ .id = "inventory.set_money", .name = "Appliquer l'argent",
                   .help = "Remplace l'argent de la campagne par la valeur du curseur.", .category = "Inventaire",
                   .kind = Kind::Action,
                   .run = [] {
                       uintptr_t profile = Profile();
                       return profile && mem::Write<int32_t>(profile + 0x1C0, static_cast<int32_t>(money->value));
                   },
                   .available = InGame, .value = 100000, .valueMin = 0, .valueMax = 9999999, .valueFormat = "%.0f" });
    static Feature* points = nullptr;
    points = &Add({ .id = "inventory.set_exchange_points", .name = "Appliquer les points d'échange",
                    .help = "Remplace les points d'échange (boutique des bonus) par la valeur du curseur.", .category = "Inventaire",
                    .kind = Kind::Action,
                    .run = [] {
                        uintptr_t profile = Profile();
                        return profile && mem::Write<int32_t>(profile + 0x1C4, static_cast<int32_t>(points->value));
                    },
                    .available = InGame, .value = 100000, .valueMin = 0, .valueMax = 9999999, .valueFormat = "%.0f" });
}

bool EnsurePlayerDamageHook() { return EnsureDamageHook(); }
bool EnsurePlayerHitReactionHook() { return EnsureHitHook(); }
bool EnsurePlayerContactHook() { return EnsureContactHook(); }

// knife_icon.cpp: player `index`'s character is left-handed by the game (Sheva): the inventory screen shows its grid on
// the right (the screen places the grids by the character's hand, VA 0xC69F00's `cmp byte [edi+0x1AF9], 1`; user,
// 2026-11-04: playing Sheva, her knife's units were drawn in Chris's grid).
bool PlayerCharacterLeftHanded(int index) {
    const uintptr_t p = actors::InGame() ? Player(index) : 0;
    if (!p) return false;
    const int hand = CharacterHand(p);
    if (hand >= 0) return hand == 1;
    uint8_t live = 0;
    return mem::Read(p + kHandedness, live) && live == 1;
}

// stamina.cpp: player 1's health ring is on the left of the screen: his character is left-handed by the game (Sheva),
// whatever hand he holds his weapons with now ("Main inversée", the bow); not found, his hand now.
bool Player1RingOnLeft() {
    static uintptr_t cachedFor = 0;
    static int cached = -1;
    const uintptr_t p = actors::InGame() ? Player(0) : 0;
    if (!p) return false;
    if (p != cachedFor) {
        cachedFor = p;
        cached = CharacterHand(p);
    }
    if (cached >= 0) return cached == 1;
    uint8_t hand = 0;
    return mem::Read(p + kHandedness, hand) && hand == 1;
}
}
