// Enemy grabs on player 1 (user, 2026-10-21; research with diag.player_hit_log, MODLOG session 14).
// A Majini grab: its Catch action 0x97 (update VA 0xC1A8F0) turns to its target and, during the motion's attack window
// (enemy +0x44B bit 0), arms its hand shapes 0xD..0x10 of the hit list +0x1290 with the grab hit 0x10003. When they
// touch the target, the collision stores it as the list's contact (+0x1290 +0x20 = enemy +0x12B0, a unit whose +4 & 7
// is 1 or 2) and queues a grab hit on it (+0x3C 0x40000000: the player's hit reaction requests 0x84 setEventAct_wait,
// RVA 0x77C6F6). The AI state's decision (RVA 0x575210, and its twin RVA 0x575390: thiscall(ai), ai +0x280 = enemy,
// +0x2C = target) then asks "can grab" (RVA 0x563440, thiscall(ai, enemy, target) ret 8: contact == target, a player
// with 1 HP or dead is refused, then the grab manager [RVA 0xDB1FCC] claims the target, RVA 0x3AEE90: refused when it
// is already held) and picks the hold by the facings (same direction = from behind BackHold 0x9B, else NeckHang 0x99);
// without it the player's 0x84 ends on its own the next frame (callback VA 0xB816D0 -> RVA 0x7781C0, back to wait).
// - "Saisie en étant mourant": the "can grab" answer without its 1 HP refusal for player 1.
// - "Saisies améliorées": the hands had to touch player 1 exactly (user: moving or at an angle the grab missed); during
//   the attack window, player 1 within reach in front of the Majini counts as the contact for its decision, and when it
//   takes the hold player 1 gets the grab hit's 0x84 like a real touch.
// - "Une autre saisie ne te libère pas": held by a Majini, an Executioner's grab (uEm20, its action 0x17, hit +0x3C
//   0x40400010, +0x9C 0x00050004) did nothing to player 1 but also touched the Majini, whose reaction 0x7C let him go
//   (0x83 from its hold, RVA 0x827C3E). The hits of another enemy's grab on the Majini holding player 1 are ignored:
//   the Executioner's in 0x17 (its hit on the Majini has +0x3C 0x01460010, no grab flag), or any hit with the grab flag.
#include "features/dying.hpp"

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

namespace features {
bool EnsureEnemyActionHook();          // action_log.cpp: the enemies' RequestAction core (calls OnEnemyGrabRequest)
bool EnsureEnemyOnHitHook();           // knife.cpp: enemy OnHit (calls GrabKeepsHolder)
uintptr_t HitAttacker(uintptr_t hit);  // parry.cpp
bool PlayerInvincible(int index);      // player.cpp
bool WeskerMoveInvulnerable(uintptr_t player);  // wesker_moves.cpp
bool ParryGrab(uintptr_t enemy);       // parry.cpp: an easier grab parried (the parry window open)
bool AutoAnswerGrab(uintptr_t enemy);  // perfect_dodge.cpp: the automatic parry's window, or an automatic dodge asked
bool AutoDodgeHolds();                 // perfect_dodge.cpp: an automatic dodge asked, not started yet

namespace {
constexpr uint32_t kNeckHang = 0x99, kBackHold = 0x9B;  // cEm10 actions
constexpr uint32_t kGrabWait = 0x84;                     // player setEventAct_wait
constexpr uint32_t kGrabHit = 0x40000000;                // hit +0x3C: a grab
constexpr uintptr_t kExecutionerVtable = 0xB748A8;       // uEm20 (RVA)
constexpr uint32_t kExecutionerGrab = 0x17;

std::atomic<bool> g_dyingOn{ false };
std::atomic<bool> g_easyOn{ false };
std::atomic<bool> g_keepHoldOn{ false };
std::atomic<float> g_reach{ 130.0f };  // cm
uintptr_t g_forcedEnemy = 0;           // game thread
DWORD g_forcedTick = 0;

// The hit list's contact as the game reads it (RVA 0x563440): a unit of kind 1 or 2, else none.
uintptr_t Contact(uintptr_t enemy) {
    uintptr_t unit = 0;
    uint32_t kind = 0;
    if (!mem::Read(enemy + 0x12B0, unit) || !unit || !mem::Read(unit + 4, kind)) return 0;
    kind &= 7;
    return kind == 1 || kind == 2 ? unit : 0;
}

bool IsDead(uintptr_t unit) {
    using IsDeadFn = bool(__fastcall*)(void* self, void* edx);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(unit, vtable) || !mem::Read(vtable + 0x15C, fn) || !fn) return true;
    __try {
        return reinterpret_cast<IsDeadFn>(fn)(reinterpret_cast<void*>(unit), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

// The grab manager's claim on the target (marks it held, P+0x18DC bit 0).
bool Claim(uintptr_t target) {
    using ClaimFn = bool(__fastcall*)(void* manager, void* edx, uintptr_t target);
    uintptr_t manager = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1FCC)), manager) || !manager) return false;
    __try {
        return reinterpret_cast<ClaimFn>(mem::Rva(0x3AEE90))(reinterpret_cast<void*>(manager), nullptr, target);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Player 1 may not be grabbed (user, 2026-10-23: with "Saisies améliorées" Majini grabbed him during melee moves,
// dodges, and a few times through "Invincible - Joueur 1"): invincible, in a Wesker / Jill move, in a melee move (actions
// 0x30-0x4D, 0x6F, 0xAC, 0xAD as swaps.cpp) or a dodge (Quick Duck 0x50, rolls 0x51-0x55), or no damage shape of his
// carries his number (the enemies only hit shapes holding it: shape list P+0x1290, array [+4], count [+0x18], type-1
// shapes +0xC4 == 1 with the number at +0xD0; how invincibility hides him, MODLOG session 4).
bool DamageShapesArmed(uintptr_t player) {
    uintptr_t list = 0, array = 0;
    uint32_t count = 0;
    if (!mem::Read(player + 0x1290, list) || !list || !mem::Read(list + 4, array) || !array || !mem::Read(list + 0x18, count) ||
        count == 0 || count > 64)
        return true;
    bool any = false;
    for (uint32_t k = 0; k < count; ++k) {
        uintptr_t shape = 0;
        uint32_t type = 0, number = 0;
        if (!mem::Read(array + 4 * k, shape) || !shape || !mem::Read(shape + 0xC4, type) || type != 1) continue;
        any = true;
        if (mem::Read(shape + 0xD0, number) && number) return true;
    }
    return !any;
}

// Player 1 in a melee move or a dodge.
bool InMoveOrDodge(uintptr_t p1) {
    uint32_t action = 0;
    if (!mem::Read(p1 + 0x10E0, action)) return false;
    return (action >= 0x30 && action <= 0x4D) || action == 0x6F || action == 0xAC || action == 0xAD ||
           (action >= 0x50 && action <= 0x55);
}

bool Player1Ungrabbable(uintptr_t p1) {
    if (PlayerInvincible(0) || WeskerMoveInvulnerable(p1) || AutoDodgeHolds() || InMoveOrDodge(p1)) return true;
    return !DamageShapesArmed(p1);
}

using CanGrabFn = bool(__fastcall*)(void* ai, void* edx, uintptr_t enemy, uintptr_t target);
CanGrabFn g_canGrab = nullptr;

bool __fastcall HookCanGrab(void* ai, void* edx, uintptr_t enemy, uintptr_t target) {
    // Player 1 in a Wesker / Jill move (user 2026-10-22: invincible during Jill's throw yet still grabbed), invincible,
    // in a melee move or a dodge: refused before the game's test, whose claim would mark him held.
    if (target && target == actors::Player(0) && Player1Ungrabbable(target)) return false;
    const bool game = g_canGrab(ai, edx, enemy, target);
    if (game || !g_dyingOn.load(std::memory_order_relaxed) || !enemy || !target || target != actors::Player(0) ||
        !Player1Dying())
        return game;
    // The game's test without its "1 HP" refusal (it stops there, before the claim).
    if (Contact(enemy) != target || IsDead(target) || !Claim(target)) return false;
    LOG("grab while dying: %08X may grab player 1", static_cast<unsigned>(enemy));
    return true;
}

// Player 1 within reach of the Majini's arms: in front of it (half circle), close enough, about the same height, and
// free (the claim refuses a held player anyway).
bool InReach(uintptr_t enemy, uintptr_t player) {
    uint8_t window = 0;
    uint32_t action = 0;
    if (!mem::Read(enemy + 0x44B, window) || !(window & 1)) return false;
    if (!mem::Read(player + 0x10E0, action) || action == 0x80000000 || action == kGrabWait || action == 0x82) return false;
    float e[3]{}, q[4]{}, p[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), e, sizeof(e)) ||
        !mem::SafeRead(reinterpret_cast<void*>(enemy + 0x40), q, sizeof(q)) ||
        !mem::SafeRead(reinterpret_cast<void*>(player + 0x30), p, sizeof(p)))
        return false;
    const float dx = p[0] - e[0], dz = p[2] - e[2], reach = g_reach.load(std::memory_order_relaxed);
    if (dx * dx + dz * dz > reach * reach || std::fabs(p[1] - e[1]) > 100.0f) return false;
    constexpr float kPi = 3.14159265f;
    float side = 2.0f * std::atan2(q[1], q[3]) - std::atan2(dx, dz);
    while (side > kPi) side -= 2 * kPi;
    while (side < -kPi) side += 2 * kPi;
    return std::fabs(side) <= kPi / 2;
}

using DecideFn = bool(__fastcall*)(void* ai, void* edx);
DecideFn g_decide[2] = {};

bool Decide(int which, void* ai, void* edx) {
    const DecideFn original = g_decide[which];
    if (!g_easyOn.load(std::memory_order_relaxed)) return original(ai, edx);
    const uintptr_t brain = reinterpret_cast<uintptr_t>(ai), p1 = actors::Player(0);
    uintptr_t enemy = 0, target = 0;
    if (!p1 || !mem::Read(brain + 0x280, enemy) || !enemy || !mem::Read(brain + 0x2C, target) || target != p1 ||
        Contact(enemy) || !InReach(enemy, p1))
        return original(ai, edx);
    // An invincible player 1 is never touched (his hit shapes are hidden from the enemies): no contact given either
    // (user, 2026-10-22: Majini grabbed him through "Invincible"). He still parries or dodges the grab (user,
    // 2026-10-30: invincible with the automatic dodges, the parries no longer came): no attack shape to see there.
    if (Player1Ungrabbable(p1)) {
        if ((PlayerInvincible(0) || AutoDodgeHolds()) && !WeskerMoveInvulnerable(p1) && !InMoveOrDodge(p1) &&
            !AutoAnswerGrab(enemy))
            ParryGrab(enemy);
        return original(ai, edx);
    }
    // No grab hit reaches player 1 on this path: the parry is decided here (parry.cpp), the contact then not given; the
    // automatic answers too (user, 2026-10-29: these grabs light no attack shape, the sphere never saw them).
    if (AutoAnswerGrab(enemy) || ParryGrab(enemy)) return original(ai, edx);
    // Only for this decision: the collision sets the contact again by itself.
    mem::Write<uint32_t>(enemy + 0x12B0, static_cast<uint32_t>(p1));
    g_forcedEnemy = enemy;
    g_forcedTick = GetTickCount();
    const bool result = original(ai, edx);
    uintptr_t contact = 0;
    if (mem::Read(enemy + 0x12B0, contact) && contact == p1) mem::Write<uint32_t>(enemy + 0x12B0, 0);
    g_forcedEnemy = 0;
    return result;
}

bool __fastcall HookDecideA(void* ai, void* edx) { return Decide(0, ai, edx); }
bool __fastcall HookDecideB(void* ai, void* edx) { return Decide(1, ai, edx); }

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

bool EnsureCanGrabHook() {
    static bool hooked = false;
    if (!hooked) {
        const uint8_t prologue[] = { 0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x56 };
        hooked = InstallHook(0x563440, prologue, sizeof(prologue), reinterpret_cast<void*>(&HookCanGrab),
                             reinterpret_cast<void**>(&g_canGrab), "enemy can grab");
    }
    return hooked;
}

bool EnsureDecideHooks() {
    static bool hooked = false;
    if (!hooked) {
        // Both decisions start alike: push ebp; mov ebp, esp; and esp, -16; sub esp, 0x1C; push esi; mov esi, ecx.
        const uint8_t prologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x1C, 0x56, 0x8B, 0xF1 };
        hooked = EnsureEnemyActionHook() && EnsureCanGrabHook() &&
                 InstallHook(0x575210, prologue, sizeof(prologue), reinterpret_cast<void*>(&HookDecideA),
                             reinterpret_cast<void**>(&g_decide[0]), "Majini grab decision") &&
                 InstallHook(0x575390, prologue, sizeof(prologue), reinterpret_cast<void*>(&HookDecideB),
                             reinterpret_cast<void**>(&g_decide[1]), "Majini grab decision 2");
    }
    return hooked;
}
}  // namespace

// wesker_moves.cpp: no grab on player 1 during its moves.
bool EnsureGrabRefusalHook() { return EnsureCanGrabHook(); }

// From the enemies' OnHit (knife.cpp), before it: true = the hit is ignored.
bool GrabKeepsHolder(uintptr_t enemy, void* hit) {
    if (!g_keepHoldOn.load(std::memory_order_relaxed) || !hit) return false;
    const uintptr_t p1 = actors::Player(0);
    uint32_t held = 0, holding = 0, flags = 0, attackerAction = 0;
    if (!p1 || !mem::Read(p1 + 0x10E0, held) || held != 0x80000000 || !mem::Read(enemy + 0x10E0, holding) ||
        (holding != kNeckHang && holding != kBackHold))
        return false;
    const uintptr_t h = reinterpret_cast<uintptr_t>(hit);
    const uintptr_t attacker = HitAttacker(h);
    if (!attacker || attacker == enemy || attacker == p1 || attacker == actors::Player(1)) return false;
    uintptr_t vtable = 0;
    mem::Read(h + 0x3C, flags);
    mem::Read(attacker + 0x10E0, attackerAction);
    const bool executionerGrab = mem::Read(attacker, vtable) &&
                                 vtable == reinterpret_cast<uintptr_t>(mem::Rva(kExecutionerVtable)) &&
                                 attackerAction == kExecutionerGrab;
    if (!(flags & kGrabHit) && !executionerGrab) return false;
    static DWORD lastLog = 0;
    if (GetTickCount() - lastLog > 1000) {
        lastLog = GetTickCount();
        LOG("grab kept: a grab of %08X (action %X, +3C %08X) on the Majini %08X holding player 1 ignored",
            static_cast<unsigned>(attacker), attackerAction, flags, static_cast<unsigned>(enemy));
    }
    return true;
}

// From the enemies' RequestAction core (action_log.cpp), after the request: a Majini whose contact the trainer gave
// takes its hold, player 1 gets what the grab hit would have given him.
void OnEnemyGrabRequest(uintptr_t enemy, int action) {
    if (!g_forcedEnemy || enemy != g_forcedEnemy || (action != static_cast<int>(kNeckHang) && action != static_cast<int>(kBackHold)))
        return;
    g_forcedEnemy = 0;
    if (GetTickCount() - g_forcedTick > 250) return;
    const uintptr_t p1 = actors::Player(0);
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    if (!p1 || !mem::Read(p1, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return;
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(p1), nullptr, static_cast<int>(kGrabWait), 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    LOG("easier grab: %08X holds player 1 (%s)", static_cast<unsigned>(enemy), action == static_cast<int>(kNeckHang) ? "front" : "back");
}

void RegisterGrabs() {
    Add({ .id = "enemies.grab_dying", .name = "Saisie en étant mourant",
          .help = "Les ennemis peuvent te saisir quand tu es mourant (dans le jeu leur prise rate toujours). Une morsure "
                  "fait alors vite descendre la barre de l'état mourant (et tue d'un coup avec « Mourant façon RE6 ») : "
                  "coche « Se débattre en étant mourant » pour pouvoir te libérer.",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureCanGrabHook()) return false;
              g_dyingOn = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });

    Feature& easy = Add({ .id = "enemies.grab_easier", .name = "Saisies améliorées",
                          .help = "Un Majini qui tend les bras pour te saisir réussit dès que tu es à portée devant lui "
                                  "pendant son geste, même si tu bouges ou que l'angle n'est pas parfait (dans le jeu ses "
                                  "mains doivent te toucher exactement). Le curseur règle la portée.",
                          .category = "Ennemis", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = 130, .valueMin = 60, .valueMax = 250, .valueFormat = "%.0f cm" });
    Feature* const pe = &easy;
    easy.set = [pe](bool on) {
        if (on && !EnsureDecideHooks()) return false;
        g_reach = pe->value;
        g_easyOn = on;
        return true;
    };
    easy.tick = [pe] { g_reach.store(pe->value, std::memory_order_relaxed); };

    Add({ .id = "enemies.grab_kept", .name = "Une autre saisie ne te libère pas",
          .help = "Quand un Majini te tient, la saisie d'un autre ennemi (le bourreau qui tente de t'attraper) est "
                  "ignorée : dans le jeu son geste touchait le Majini, qui te lâchait.",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureEnemyOnHitHook()) return false;
              g_keepHoldOn = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
}
}  // namespace features
