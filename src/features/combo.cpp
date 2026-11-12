// Solo melee combo (user, 2026-10-20): in co-op a melee blow that makes a Majini stumble opens a follow-up for the
// partner only, and the partner's follow-up opens a finisher for the first player. "Combo seul" gives player 1 both:
// his own blow opens a second Arm Front for himself, and that Arm Front opens his finisher.
// The game's chain (MODLOG session 14): the stumbles cEm10ActDmgYoro_F 0x7D / DmgYoro_B 0x7E request their prompts in
// vfunc 10 (VA 0xC0E3D0 / 0xC0EBA0) when their motion's window opens, for every player whose bit 1 << (1 + index) of
// enemy +0x1BB4 is clear - the player who hit is left out. Action +0x4C = 0 (first stumble): 0x7D offers Arm Front 0x30
// + head flash 0x32, 0x7E head flash 0x32 + Arm Back 0x33. +0x4C = 1 (the setups: the previous action was the other
// stumble, or +0x1B60 & 0x100000, i.e. +0x1B5C & 0x100000 during the previous action): Finisher Front 0x35 + Finisher
// Back 0x34. Here: player 1's bit is cleared as soon as his melee blow starts a stumble (before its window), the first
// stumble gets +0x1B5C |= 0x100000 (the next stumble offers the finishers whatever its direction), and the follow-up
// player 1 takes from a first stumble's prompt (0x32 / 0x33 / Reunion 0xAD) is played as Arm Front 0x30.
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

namespace features {
int ReunionVariant(uintptr_t p, int action);  // swaps.cpp: a Reunion character's Leg Front / Head Flash variants
bool StaminaAllows();  // stamina.cpp: the red arc not filling

bool EnsurePlayerRequestHook();                            // action_log.cpp
void RequestCombatMove(int k, uintptr_t target);           // wesker_moves.cpp: a free melee move on it, the next frame
bool PrepareWeskerMoves();                                 // wesker_moves.cpp
bool Player1Dying();                                       // dying.cpp
bool GrabRescueActive();                                   // dying.cpp
void ClaimMeleeTarget(uintptr_t player, uintptr_t enemy);  // swaps.cpp

namespace {
constexpr uint32_t kYoroF = 0x7D, kYoroB = 0x7E;
constexpr uintptr_t kYoroFVtable = 0xC3B484, kYoroBVtable = 0xC3B4B4;  // RVA
constexpr uint32_t kPlayer1Bit = 0x2, kComboFlag = 0x100000;
constexpr int kArmFront = 0x30;

std::atomic<bool> g_on{ false };
uintptr_t g_firstStumble[16] = {};  // enemies whose first stumble player 1 started (game thread only)

bool IsMeleeMove(uint32_t action) { return (action >= 0x30 && action <= 0x4D) || action == 0x6F || action == 0xAC || action == 0xAD; }

// The enemy's stumble action object (0 if it is not stumbling, or not a Majini).
uintptr_t Stumble(uintptr_t enemy) {
    uint32_t action = 0;
    uintptr_t actions = 0, object = 0, vtable = 0;
    if (!mem::Read(enemy + 0x10E0, action) || (action != kYoroF && action != kYoroB) || !mem::Read(enemy + 0x10DC, actions) ||
        !actions || !mem::Read(actions + 4 * action, object) || !object || !mem::Read(object, vtable))
        return 0;
    return vtable == reinterpret_cast<uintptr_t>(mem::Rva(action == kYoroF ? kYoroFVtable : kYoroBVtable)) ? object : 0;
}

bool FirstStumble(uintptr_t enemy) {
    uint8_t finisher = 1;
    const uintptr_t object = Stumble(enemy);
    return object && mem::Read(object + 0x4C, finisher) && !finisher;
}

bool Tracked(uintptr_t enemy) {
    for (uintptr_t e : g_firstStumble)
        if (e == enemy) return true;
    return false;
}

void Track(uintptr_t enemy) {
    if (Tracked(enemy)) return;
    for (uintptr_t& e : g_firstStumble)
        if (!e) {
            e = enemy;
            return;
        }
}

void Tick() {
    for (uintptr_t& e : g_firstStumble)
        if (e && !Stumble(e)) e = 0;  // out of its stumble: the combo is over (or became a finisher stumble)
    const uintptr_t p1 = actors::Player(0);
    uint32_t p1Action = 0;
    if (!p1 || !mem::Read(p1 + 0x10E0, p1Action) || !IsMeleeMove(p1Action)) return;
    for (uintptr_t enemy : actors::Enemies()) {
        uint32_t players = 0;
        const uintptr_t object = Stumble(enemy);
        if (!object || !mem::Read(enemy + 0x1BB4, players) || !(players & kPlayer1Bit)) continue;
        mem::Write(enemy + 0x1BB4, players & ~kPlayer1Bit);  // player 1's blow: he gets the follow-up prompts
        uint8_t finisher = 0;
        mem::Read(object + 0x4C, finisher);
        if (!finisher) {
            uint32_t flags = 0;
            if (mem::Read(enemy + 0x1B5C, flags)) mem::Write(enemy + 0x1B5C, flags | kComboFlag);
            Track(enemy);
        }
        LOG("solo combo: enemy %08X stumble %s after player 1's 0x%02X", static_cast<unsigned>(enemy),
            finisher ? "with finisher" : "1 (follow-up Arm Front)", p1Action);
    }
}
// ---- Head flash after escaping a front grab (user, 2026-10-20: like in the first trailers). Recording (Mercenaries):
// a Majini front grab is cEm10ActNeckHang 0x99 (player 1 in event wait 0x80000000, motions 0xF71 / 0xF64); escaping
// it by struggling, the Majini goes to NeckHangDmg 0x9A (its push-off motion 0x60B, player 1 0xF6F) and player 1 is
// free (action 0) ~2.15 s later. A back grab is BackHold 0x9B -> BackHoldDmg 0x9C: left out. A failed escape (the
// final bite) throws player 1 down (action 0x97), no 0x9A. "Sauvé si une prise te met mourant" also ends in 0x9A but
// with player 1 at 1 HP: only an escape with player 1 above 1 HP counts, and he must still be fine once free. Then the
// free head flash (0x32) is played on that Majini, aimed at it like a prompt (target P+0x2DA4, point = its position,
// angle towards player 1: Chris turns and steps to it).
// Waiting until player 1 was free felt slow (user): the head flash is requested during the push-off once the delay
// slider has elapsed (1.6 s, 0.55 s before he is free; the parry's riposte showed a request is taken while held).
constexpr uint32_t kNeckHang = 0x99, kNeckHangDmg = 0x9A, kHeldAction = 0x80000000;
// The move played (user, 2026-10-23: a choice instead of the head flash alone): a melee move requested with the Majini
// as its target (as a prompt's handler sets it), or Jill's throw (wesker_moves.cpp, the next frame).
// Wesker's moves too (user, 2026-10-23): the free melee moves (wesker_moves.cpp RequestCombatMove, kCombat order).
struct FollowUp {
    const char* name;
    int action;      // player action, unless a free melee move
    int combat = -1;  // free melee move index, started once player 1 is free
};
constexpr FollowUp kFollowUps[] = {
    { "Head flash (par défaut)", 0x32 }, { "Arm Front", 0x30 }, { "Leg Front", 0x31 }, { "Arm Back", 0x33 },
    { "Leg Back", 0x39 }, { "Finisher Front", 0x35 }, { "Finisher Back", 0x34 }, { "Chris RE5 Projection", 0, 5 },
    { "Wesker RE5 Coup de pied 1", 0, 0 }, { "Wesker RE5 Coup de pied 2", 0, 1 }, { "Wesker RE5 Coup de pied retourné", 0, 2 },
    { "Wesker RE5 Balayage", 0, 3 }, { "Wesker RE5 Série de coups de pied", 0, 4 },
};
std::atomic<int> g_followUp{ 0 };
constexpr DWORD kEscapeMs = 4000;   // push-off included
constexpr float kFlashReach = 350;  // units (cm); the push-off sends the Majini ~1-2 m away

std::atomic<float> g_grabFlashDelay{ 1.6f };  // seconds after the push-off starts
struct Seen {
    uintptr_t enemy;
    uint32_t action;
};
Seen g_seen[64] = {};
int g_seenCount = 0;
uintptr_t g_escaped = 0;  // the Majini player 1 just pushed off
DWORD g_escapeTick = 0;

uint32_t PreviousAction(uintptr_t enemy) {
    for (int i = 0; i < g_seenCount; ++i)
        if (g_seen[i].enemy == enemy) return g_seen[i].action;
    return 0xFFFFFFFF;
}

bool Healthy(uintptr_t p1) {
    int16_t health = 0;
    return mem::Read(p1 + 0x1364, health) && health > 1 && !Player1Dying();
}

void HeadFlashOn(uintptr_t p1, uintptr_t enemy) {
    uint32_t current = 0;
    mem::Read(p1 + 0x10E0, current);
    float enemyPos[3]{}, playerPos[3]{};
    int16_t health = 0;
    if (!mem::Read(enemy + 0x1364, health) || health <= 0 ||
        !mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), enemyPos, sizeof(enemyPos)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), playerPos, sizeof(playerPos)))
        return;
    const float dx = enemyPos[0] - playerPos[0], dz = enemyPos[2] - playerPos[2];
    if (dx * dx + dz * dz > kFlashReach * kFlashReach) {
        LOG("grab head flash: enemy %08X too far (%.0f)", static_cast<unsigned>(enemy), std::sqrt(dx * dx + dz * dz));
        return;
    }
    const int choice = g_followUp.load(std::memory_order_relaxed);
    const FollowUp& followUp = kFollowUps[choice >= 0 && choice < static_cast<int>(std::size(kFollowUps)) ? choice : 0];
    if (followUp.combat >= 0) {
        RequestCombatMove(followUp.combat, enemy);
        LOG("grab head flash: %s on enemy %08X (player 1 action was %X)", followUp.name, static_cast<unsigned>(enemy),
            static_cast<unsigned>(current));
        return;
    }
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(p1, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return;
    mem::Write<uint32_t>(p1 + 0x2DA4, static_cast<uint32_t>(enemy));
    ClaimMeleeTarget(p1, enemy);
    mem::SafeWrite(reinterpret_cast<void*>(p1 + 0x2E10), enemyPos, sizeof(enemyPos));
    mem::Write(p1 + 0x2E20, std::atan2(playerPos[0] - enemyPos[0], playerPos[2] - enemyPos[2]));
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(p1), nullptr, ReunionVariant(p1, followUp.action), 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    LOG("grab head flash: %s (0x%02X) on enemy %08X (player 1 action was %X)", followUp.name, followUp.action,
        static_cast<unsigned>(enemy), static_cast<unsigned>(current));
}

void TickGrabFlash() {
    const uintptr_t p1 = actors::Player(0);
    uint32_t p1Action = 0;
    if (!p1 || !mem::Read(p1 + 0x10E0, p1Action)) return;
    Seen now[64];
    int count = 0;
    for (uintptr_t enemy : actors::Enemies()) {
        uint32_t action = 0;
        if (count >= 64 || !mem::Read(enemy + 0x10E0, action)) continue;
        if (action == kNeckHangDmg && PreviousAction(enemy) == kNeckHang && p1Action == kHeldAction) {
            // "Endurance" empty (user, 2026-10-26): the plain escape only, no follow-up.
            const bool healthy = Healthy(p1) && !GrabRescueActive() && StaminaAllows();
            LOG("grab head flash: player 1 pushed enemy %08X off (%s)", static_cast<unsigned>(enemy),
                healthy ? "waiting until he is free" : "dying: no head flash");
            g_escaped = healthy ? enemy : 0;
            g_escapeTick = GetTickCount();
        }
        now[count++] = { enemy, action };
    }
    std::copy(now, now + count, g_seen);
    g_seenCount = count;
    if (!g_escaped) return;
    if (GetTickCount() - g_escapeTick > kEscapeMs) {
        g_escaped = 0;
        return;
    }
    const bool early = GetTickCount() - g_escapeTick >= static_cast<DWORD>(g_grabFlashDelay.load(std::memory_order_relaxed) * 1000.0f);
    // The free melee moves (Jill's throw, Wesker's) start only on a free player 1 (wesker_moves.cpp FreePlayer1: action
    // below 0x80; logged 2026-10-23: the throw asked during the push-off failed): they wait until he is free.
    const int choice = g_followUp.load(std::memory_order_relaxed);
    const bool freeMove = choice >= 0 && choice < static_cast<int>(std::size(kFollowUps)) && kFollowUps[choice].combat >= 0;
    if (p1Action == kHeldAction && (!early || freeMove)) return;  // still in the push-off
    const uintptr_t enemy = g_escaped;
    g_escaped = 0;
    if ((p1Action >= 0x10 && p1Action != kHeldAction) || !Healthy(p1)) {
        LOG("grab head flash: player 1 not free (action %02X) or dying", p1Action);
        return;
    }
    HeadFlashOn(p1, enemy);
}
}  // namespace

// Player 1's RequestAction (action_log.cpp hook): the follow-up taken from a first stumble's prompt (head flash, Arm
// Back; the handlers set the melee target P+0x2DA4 first) becomes Arm Front.
int SoloComboAction(uintptr_t player, int action) {
    if (!g_on.load(std::memory_order_relaxed) || player != actors::Player(0) || (action != 0x32 && action != 0x33 && action != 0xAD))
        return action;
    uint32_t target = 0;
    if (!mem::Read(player + 0x2DA4, target) || !target || !Tracked(target) || !FirstStumble(target)) return action;
    LOG("solo combo: follow-up 0x%02X on enemy %08X played as Arm Front", action, static_cast<unsigned>(target));
    return kArmFront;
}

void RegisterCombo() {
    Add({ .id = "melee.solo_combo", .name = "Combo seul (sans partenaire)",
          .help = "En coop, un coup qui fait tituber un Majini ouvre une suite au partenaire, puis la suite du "
                  "partenaire ouvre un finisher au premier joueur. Coché : ton coup t'ouvre à toi-même un deuxième "
                  "Arm Front (le prompt du partenaire), et ce deuxième Arm Front t'ouvre ton finisher.",
          .category = "Mêlée", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsurePlayerRequestHook()) return false;
              g_on = on;
              if (!on)
                  for (uintptr_t& e : g_firstStumble) e = 0;
              return true;
          },
          .tick = [] {
              if (actors::InGame()) Tick();
          },
          .available = [] { return game::IsSupported(); } });
    Feature& flash = Add({ .id = "melee.grab_head_flash", .name = "Coup après une désaisie de face",
                           .help = "Comme dans les premières bandes-annonces : quand tu te libères seul d'un Majini qui "
                                   "t'a saisi de face, Chris enchaîne un coup sur lui (choisi dans « Coup lancé après une "
                                   "désaisie de face »). Le curseur règle le délai après le début de la poussée (Chris "
                                   "est libre vers 2,15 s). Pas en mourant (ni la désaisie automatique quand une prise te "
                                   "met mourant), ni après une saisie de dos.",
                           .category = "Mêlée", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = 1.6f, .valueMin = 0.3f, .valueMax = 2.2f, .valueFormat = "%.2f s" });
    Feature* const fp = &flash;
    flash.set = [fp](bool) {
        g_grabFlashDelay = fp->value;
        g_escaped = 0;
        g_seenCount = 0;
        return true;
    };
    flash.tick = [fp] {
        g_grabFlashDelay.store(fp->value, std::memory_order_relaxed);
        if (actors::InGame()) TickGrabFlash();
    };
    std::vector<std::string> followUps;
    for (const FollowUp& f : kFollowUps) followUps.push_back(f.name);
    Add({ .id = "melee.grab_follow_up", .name = "Coup lancé après une désaisie de face",
          .help = "Le coup que Chris enchaîne sur le Majini qu'il vient de repousser (avec « Coup après une désaisie de "
                  "face » coché).",
          .category = "Mêlée", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              if (kFollowUps[index].combat >= 0 && actors::InGame() && !PrepareWeskerMoves()) return false;
              g_followUp = index;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = static_cast<float>(std::size(kFollowUps) - 1), .valueFormat = "%.0f",
          .choices = followUps });
}
}  // namespace features
