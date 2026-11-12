// Mêlée / Personnage swaps observed with UTSpy (UT session 3, MODLOG "UT session 3 results"):
//  - melee swap: each slot is the action-id immediate of a `push <id>` before RequestAction;
//  - anim sets per character: motion/soft paths of the character's resource list, swapped at load;
//  - character / costume swap for player 1 (next load);
//  - Reunion moves (needs a Reunion anim set).
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "features/melee_tuning.hpp"

#include <windows.h>
#include <MinHook.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace features {
bool StaminaAllows();  // stamina.cpp: the red arc not filling
void StaminaSpend();   // stamina.cpp: a free melee move or dodge started
void StaminaSpendDodge(float cost);  // stamina.cpp: a dodge started (its option's cost, "Endurance stricte")

bool MotionLogEnabled();
void LogMotion(void* self, uint32_t layer, uint32_t motion, uintptr_t caller);
bool EnsureSetMotionHook();
bool GameButtonHeld(int button, int pad);  // quick_knife.cpp
int Player1Pad();  // movement.cpp: the pad index player 1's buttons are read with
bool EnsurePlayerRequestHook();            // action_log.cpp
bool EnsurePlayerVoiceHook();              // player.cpp: Player::PlayVoice
bool EnsureGrabRefusalHook();              // grabs.cpp: no grab on player 1 during a move
bool EnsurePlayerContactHook();            // player.cpp: cObjAdj contacts
uint32_t WeskerMoveMotion(uintptr_t unit, uint32_t layer, uint32_t motion);  // wesker_moves.cpp
uint32_t ObstacleClimbMotion(uintptr_t unit, uint32_t layer, uint32_t motion);  // obstacle_climb.cpp
uint32_t JillDownMotion(uintptr_t unit, uint32_t layer, uint32_t motion);    // wesker_boss.cpp: Jill kept down
uint32_t InvisibleHolderMotion(uintptr_t unit, uint32_t layer, uint32_t motion);  // action_log.cpp: Wesker kneels
void ForcedDeathMotion(uintptr_t unit, uint32_t layer, uint32_t& motion, uint32_t& blend, uint32_t& start);  // forced_death.cpp
void JillVictimMotion(uintptr_t unit, uint32_t layer, uint32_t& motion, uint32_t& blend, uint32_t& start);  // wesker_moves.cpp
void PairCorpseMotion(uintptr_t unit, uint32_t layer, uint32_t& motion, uint32_t& blend, uint32_t& start);  // wesker_moves.cpp
const char* UnitClassName(uintptr_t object);  // parry.cpp: its DTI class name
bool Player1HandInverted();                   // player.cpp: player 1 with the other hand than his character's
bool Player1NativeLeftHanded();               // player.cpp: his character left-handed by the game (record +0x1C)
void PairLyingRealign(uintptr_t unit, uint32_t layer, uint32_t motion, uint32_t& blend);                     // wesker_moves.cpp
void GroundRollStart(uintptr_t unit, uint32_t layer, uint32_t motion, uint32_t& blend, uint32_t& start);  // wesker_moves.cpp
void MoveBeginStart(uintptr_t unit, uint32_t layer, uint32_t& start);       // wesker_moves.cpp: a move's start frame
bool Player1Sprinting();  // movement.cpp
bool StartRe6AimSlide();  // wesker_moves.cpp
bool Player1SprintKeyHeld();  // movement.cpp: the run key really held
bool Re6OnGround();       // wesker_moves.cpp: RE6's ground: no trainer move
bool StartDodgeMove(const char* id);                                         // wesker_moves.cpp: a free melee move by id
bool Re6Ducked();                         // wesker_moves.cpp: in "Se baisser en visant (pose figée)"'s frozen duck
void SetSlideState(bool staggered, int state);  // wesker_moves.cpp: the slide's blow, the Majini's reaction (ForceMajiniState)
void SetRe6MeleeChoice(int kind, int index, int value);  // wesker_moves.cpp: the RE6 melee's per-character choices
bool StartDodgeFromDuck(const char* id);  // wesker_moves.cpp: the duck left for that dodge move (nullptr: only left)
bool Player1MovePlaying();                                                   // wesker_moves.cpp: a move of his playing
void DrawMeleeTable(const std::string& category);                            // melee_tuning.cpp: a free melee sub-tab
void NoteDodgeStarted();                                                     // perfect_dodge.cpp: its window opens
bool DyingActionsAllowed();                                                  // dying.cpp: "Actions en étant mourant"
uint32_t Re6SprintRemap(uintptr_t unit, uint32_t layer, uint32_t motion);    // re6_motions.cpp
uint32_t Re6GroundMotion(uintptr_t unit, uint32_t layer, uint32_t motion, uint32_t& start);  // wesker_moves.cpp
uint32_t LowReadyLayerMotion(uintptr_t unit, uint32_t layer, uint32_t motion, uint32_t& start, uint32_t& speed, uint32_t& mode);  // movement.cpp
void NoteMotionStart(uintptr_t unit);                                         // time_speed.cpp: layer 0 starts
void LogBossMotion(void* self, uint32_t layer, uint32_t motion);              // action_log.cpp: Jill / Wesker research
void Player1BodyDirection(float& forward, float& left);                      // movement.cpp: held, relative to his body
void Player1MoveDirection(float& forward, float& left);                      // wesker_moves.cpp: the side a move plays (left-handed: mirrored)
void* LoadMotionList(const char* archive, const char* path);                 // wesker_moves.cpp: kept referenced
bool LoadWeaponArchive(const char* name);                                    // inventory.cpp: Image\Archive\<name>
bool FinalFightJill();  // final_fight.cpp: player 2 is Jill with her own motions during the volcano's loads
namespace {
bool EnsureMeleeSetHook();  // below: MeleeSetFor hooked
}  // namespace
bool SetMotionBank(uintptr_t unit, void* list, int bank);                    // wesker_moves.cpp

namespace {

bool Supported() { return game::IsSupported(); }

// ---- Melee swap ------------------------------------------------------------------------------
struct Action {
    const char* name;
    uint8_t id;
    int combat = -1;  // a free melee move (wesker_moves.cpp kCombat order) played through the marker action
    int lender = -1;    // the game's move `id` with this character's moveset borrowed (kLoanSets)
};

// Order and names as in the Ultimate Trainer list (ids measured one by one, research/melee_action_ids.txt).
const Action kActions[] = {
    { "Move Forward", 0x02 }, { "Move Backwards", 0x06 }, { "Move Left", 0x03 }, { "Move Right", 0x04 },
    { "Run", 0x05 }, { "Quick Turn", 0x09 }, { "Reload", 0x1F }, { "Taunt", 0x2F }, { "Knife", 0x22 },
    { "Partner Action", 0x81 }, { "Leg Front", 0x31 }, { "Leg Back", 0x39 }, { "Arm Front", 0x30 },
    { "Arm Back", 0x33 }, { "Finisher Front", 0x35 }, { "Finisher Back", 0x34 }, { "Ground", 0x38 },
    { "Help", 0x37 }, { "Head Flash", 0x32 }, { "Stab", 0x3B }, { "Jump", 0x28 }, { "Give", 0x63 },
    { "Discard Item", 0x5D }, { "Put down", 0x5B }, { "Pick up", 0x59 }, { "Knife Slash1", 0x3E },
    { "Knife Slash2", 0x3A }, { "Knife Horizontal", 0x60 }, { "Knife Vertical", 0x3E }, { "Dash Start", 0x47 },
    { "Dash", 0x48 }, { "Dash Turn Left", 0x4C }, { "Dash Turn Right", 0x4B }, { "Dash Knee", 0x49 },
    { "Roll Forward (Dodge)", 0x51 }, { "Roll Back (Dodge)", 0x52 }, { "Roll Left (Dodge)", 0x53 },
    { "Roll Right (Dodge)", 0x54 }, { "Duck", 0x4E }, { "Quick Duck", 0x50 }, { "Stall 1", 0x71 },
    { "Stall 2", 0x74 }, { "Licker Finisher Counter", 0x3D }, { "Draw weapon", 0x62 }, { "Stand up", 0x57 },
    { "Roll left/right", 0x55 },
    // Jill's throw (user, 2026-10-23: it was only in the free melee tab) is no player action: the site requests 0 (wait,
    // harmless without the trainer) and MeleeSwapThrow, from the player's request hook, refuses it and starts the
    // throw (wesker_moves.cpp) on the move's target. Wesker's moves the same way (user, 2026-10-23), the slot knowing
    // which one.
    { "Chris RE5 Projection", 0x00, 5 },
    { "Wesker RE5 Coup de pied 1", 0x00, 0 }, { "Wesker RE5 Coup de pied 2", 0x00, 1 },
    { "Wesker RE5 Coup de pied retourné", 0x00, 2 }, { "Wesker RE5 Balayage", 0x00, 3 },
    { "Wesker RE5 Série de coups de pied", 0x00, 4 }, { "Wesker RE5 Esquive gauche", 0x00, 6 },
    { "Wesker RE5 Esquive droite", 0x00, 7 }, { "Wesker RE5 Esquive arrière", 0x00, 8 }, { "Wesker RE5 Esquive avant", 0x00, 9 },
    { "Wesker RE5 Esquive 5", 0x00, 10 }, { "Wesker RE5 Esquive basse", 0x00, 11 },
    // Jill's, second part of her fight (user, 2026-10-25).
    { "Jill RE5 Projection (2e phase)", 0x00, 12 }, { "Jill RE5 Coup de pied", 0x00, 13 }, { "Jill RE5 Roue 1", 0x00, 14 },
    { "Jill RE5 Roue 2", 0x00, 15 }, { "Jill RE5 Rondade", 0x00, 16 }, { "Jill RE5 Flip arrière", 0x00, 17 },
    { "Jill RE5 Balayage", 0x00, 18 }, { "Jill RE5 Glissade", 0x00, 19 },
    { "Jill RE5 Salto (coup de pied)", 0x00, 21 },  // her first part's somersault kick (wesker_moves.cpp kCombat 21)
    // The Mercenaries Reunion characters' moves (user, 2026-10-27): the game's moves played with their moveset borrowed
    // (kLoanSets index), named after the character then the game's move (user). Josh's first: his choices came first.
    { "Josh RE5 Arm Front", 0x30, -1, 3 }, { "Josh RE5 Leg Front", 0x31, -1, 3 }, { "Josh RE5 Head Flash", 0x32, -1, 3 },
    { "Josh RE5 Arm Back", 0x33, -1, 3 }, { "Josh RE5 Finisher Back", 0x34, -1, 3 }, { "Josh RE5 Finisher Front", 0x35, -1, 3 },
    { "Josh RE5 Help", 0x37, -1, 3 }, { "Josh RE5 Ground", 0x38, -1, 3 }, { "Josh RE5 Leg Back", 0x39, -1, 3 },
    { "Sheva RE5 Arm Front", 0x30, -1, 0 }, { "Sheva RE5 Leg Front", 0x31, -1, 0 }, { "Sheva RE5 Head Flash", 0x32, -1, 0 },
    { "Sheva RE5 Arm Back", 0x33, -1, 0 }, { "Sheva RE5 Finisher Back", 0x34, -1, 0 }, { "Sheva RE5 Finisher Front", 0x35, -1, 0 },
    { "Sheva RE5 Help", 0x37, -1, 0 }, { "Sheva RE5 Ground", 0x38, -1, 0 }, { "Sheva RE5 Leg Back", 0x39, -1, 0 },
    { "Jill RE5 Arm Front", 0x30, -1, 1 }, { "Jill RE5 Leg Front", 0x31, -1, 1 }, { "Jill RE5 Head Flash", 0x32, -1, 1 },
    { "Jill RE5 Arm Back", 0x33, -1, 1 }, { "Jill RE5 Finisher Back", 0x34, -1, 1 }, { "Jill RE5 Finisher Front", 0x35, -1, 1 },
    { "Jill RE5 Help", 0x37, -1, 1 }, { "Jill RE5 Ground", 0x38, -1, 1 }, { "Jill RE5 Leg Back", 0x39, -1, 1 },
    { "Wesker RE5 Arm Front", 0x30, -1, 2 }, { "Wesker RE5 Leg Front", 0x31, -1, 2 }, { "Wesker RE5 Head Flash", 0x32, -1, 2 },
    { "Wesker RE5 Arm Back", 0x33, -1, 2 }, { "Wesker RE5 Finisher Back", 0x34, -1, 2 }, { "Wesker RE5 Finisher Front", 0x35, -1, 2 },
    { "Wesker RE5 Help", 0x37, -1, 2 }, { "Wesker RE5 Ground", 0x38, -1, 2 }, { "Wesker RE5 Leg Back", 0x39, -1, 2 },
    { "Barry RE5 Arm Front", 0x30, -1, 4 }, { "Barry RE5 Leg Front", 0x31, -1, 4 }, { "Barry RE5 Head Flash", 0x32, -1, 4 },
    { "Barry RE5 Arm Back", 0x33, -1, 4 }, { "Barry RE5 Finisher Back", 0x34, -1, 4 }, { "Barry RE5 Finisher Front", 0x35, -1, 4 },
    { "Barry RE5 Help", 0x37, -1, 4 }, { "Barry RE5 Ground", 0x38, -1, 4 }, { "Barry RE5 Leg Back", 0x39, -1, 4 },
    { "Rebecca RE5 Arm Front", 0x30, -1, 5 }, { "Rebecca RE5 Leg Front", 0x31, -1, 5 }, { "Rebecca RE5 Head Flash", 0x32, -1, 5 },
    { "Rebecca RE5 Arm Back", 0x33, -1, 5 }, { "Rebecca RE5 Finisher Back", 0x34, -1, 5 }, { "Rebecca RE5 Finisher Front", 0x35, -1, 5 },
    { "Rebecca RE5 Help", 0x37, -1, 5 }, { "Rebecca RE5 Ground", 0x38, -1, 5 }, { "Rebecca RE5 Leg Back", 0x39, -1, 5 },
    { "Excella RE5 Arm Front", 0x30, -1, 6 }, { "Excella RE5 Leg Front", 0x31, -1, 6 }, { "Excella RE5 Head Flash", 0x32, -1, 6 },
    { "Excella RE5 Arm Back", 0x33, -1, 6 }, { "Excella RE5 Finisher Back", 0x34, -1, 6 }, { "Excella RE5 Finisher Front", 0x35, -1, 6 },
    { "Excella RE5 Help", 0x37, -1, 6 }, { "Excella RE5 Ground", 0x38, -1, 6 }, { "Excella RE5 Leg Back", 0x39, -1, 6 },
    { "Chris RE5 Arm Front", 0x30, -1, 7 }, { "Chris RE5 Leg Front", 0x31, -1, 7 }, { "Chris RE5 Head Flash", 0x32, -1, 7 },
    { "Chris RE5 Arm Back", 0x33, -1, 7 }, { "Chris RE5 Finisher Back", 0x34, -1, 7 }, { "Chris RE5 Finisher Front", 0x35, -1, 7 },
    { "Chris RE5 Help", 0x37, -1, 7 }, { "Chris RE5 Ground", 0x38, -1, 7 }, { "Chris RE5 Leg Back", 0x39, -1, 7 },
    // The Mercenaries Reunion duo moves (user, 2026-11-05: "add them to the free melee and the melee swap, I will put them
    // in place of the moves myself"), trainer moves of wesker_moves.cpp (kCombat 54-60). Appended: the slots' saved
    // choices are indices of this list.
    { "Chris RE5 Coup duo (TAG UPPER, Reunion)", 0x00, 54 }, { "Chris RE5 Coup duo (TAG NECK, Reunion)", 0x00, 55 },
    { "Sheva RE5 Coup duo (TAG ADD, Reunion)", 0x00, 56 }, { "Josh RE5 Coup duo (TAG STRAIGHT, Reunion)", 0x00, 57 },
    { "Excella RE5 Coup duo (TAG HOOK, Reunion)", 0x00, 58 }, { "Barry RE5 Coup duo (TAG HOOK, Reunion)", 0x00, 59 },
    { "Rebecca RE5 Coup duo (TAG HOOK, Reunion)", 0x00, 60 },
    // RE6's characters' moves (user, 2026-11-09: "add it to the free melee under him and to the melee swap"), trainer moves of
    // wesker_moves.cpp (kCombat 85-88, asserted there). Appended.
    // Named as the later RE6 entries (user, 2026-11-10: "I do not see Leon's Ground in the melee swap list"; it was "Ground de Leon (RE6)").
    { "Leon RE6 Arm Front", 0x00, 85 }, { "Jake RE6 Arm Front", 0x00, 86 },
    { "Ada RE6 Arm Front", 0x00, 87 }, { "Helena RE6 Arm Front", 0x00, 88 },
    { "Leon RE6 Ground", 0x00, 89 }, { "RE6 Ground (arme à deux mains)", 0x00, 90 },
    { "RE6 Ground (couteau)", 0x00, 91 },
    // RE6's characters' moves without a victim (kCombat 112-129, asserted in wesker_moves.cpp).
    { "Chris RE6 Arm Front", 0x00, 112 },
    { "Chris RE6 Finisher Front", 0x00, 113 },
    { "Leon RE6 Finisher Front", 0x00, 114 },
    { "Helena RE6 Finisher Front", 0x00, 115 },
    { "Piers RE6 Finisher Front", 0x00, 116 },
    { "Jake RE6 Finisher Front", 0x00, 117 },
    { "Sherry RE6 Finisher", 0x00, 118 },
    { "Ada RE6 Finisher", 0x00, 119 },
    { "Agent RE6 Finisher", 0x00, 120 },
    { "Jake RE6 Help", 0x00, 121 },
    { "Jake RE6 Leg Front", 0x00, 122 },
    { "Jake RE6 Crochet droit", 0x00, 123 },
    { "Jake RE6 Crochet gauche", 0x00, 124 },
    { "Jake RE6 Coup de pied", 0x00, 125 },
    { "Jake RE6 Frappe", 0x00, 126 },
    { "Jake RE6 Balayage", 0x00, 127 },
    { "Jake RE6 Dernière frappe", 0x00, 128 },
    { "Jake RE6 Coup de pied dégagé", 0x00, 129 },
    // RE6's synchronised moves (kCombat 130-144, asserted in wesker_moves.cpp; 140 is Jake's Ground's face-down variant).
    { "Leon RE6 Leg Back", 0x00, 130 },
    { "Leon RE6 Head Flash", 0x00, 131 },
    { "Helena RE6 Leg Back", 0x00, 132 },
    { "Helena RE6 Head Flash", 0x00, 133 },
    { "Chris RE6 Head Flash", 0x00, 134 },
    { "Piers RE6 Leg Back", 0x00, 135 },
    { "Piers RE6 Head Flash", 0x00, 136 },
    { "Jake RE6 Leg Back", 0x00, 137 },
    { "Jake RE6 Head Flash", 0x00, 138 },
    { "Jake RE6 Ground", 0x00, 139 },
    { "Sherry RE6 Leg Back", 0x00, 141 },
    { "Sherry RE6 Head Flash", 0x00, 142 },
    { "Ada RE6 Head Flash", 0x00, 143 },
    { "Ada RE6 Leg Back", 0x00, 144 },
    { "Chris RE6 Leg Back", 0x00, 153 },  // n° 46, AcA 80 with em1000CoC 80 (user, 2026-11-10)
    { "RE6 Contre au couteau", 0x00, 154 }, { "RE6 Égorgement au couteau", 0x00, 155 },  // AcA 5A / 5C with their J'avo sides
};
constexpr int kThrowCombat = 5;
constexpr uint8_t kThrowMarker = 0x00;

// One immediate byte in game code. Refuses to write if something else changed it.
struct ByteSite {
    uintptr_t rva;
    uint8_t original;
    uint8_t current;
    bool Set(uint8_t value) {
        uint8_t* p = mem::Rva(rva);
        if (!mem::IsGameCode(p - 1, 2)) return false;
        // `push imm8` (6A xx) sign-extends: an action id from 0x80 would be pushed as a negative id.
        if (p[-1] == 0x6A && value >= 0x80) {
            LOG("melee site %06X: action %02X does not fit a push imm8", static_cast<unsigned>(rva), value);
            return false;
        }
        if (*p != current && *p != original) {
            LOG("melee site %06X holds %02X (expected %02X)", static_cast<unsigned>(rva), *p, current);
            return false;
        }
        if (*p != value && !mem::WriteCode(p, &value, 1)) return false;
        current = value;
        return true;
    }
};

struct Slot {
    const char* id;
    const char* name;
    std::vector<ByteSite> sites;
    bool swapped = false;
    int combat = -1;  // swapped to a free melee move (its marker action)
    int lender = -1;    // swapped to a Reunion character's move: its moveset borrowed when its site requests it
};

std::deque<Slot> g_slots;
mem::Patch g_meleeCompat("melee.compat", 0x87E625, { 0xE0 }, { 0xE9 });  // set by UT while Melee Swap is on

void UpdateCompat() {
    bool any = false;
    for (const Slot& s : g_slots) any = any || s.swapped;
    if (any) g_meleeCompat.Apply();
    else g_meleeCompat.Restore();
}

bool AnySwapped() {
    for (const Slot& slot : g_slots)
        if (slot.swapped) return true;
    return false;
}

const Slot* FirstSwapped() {
    for (const Slot& s : g_slots)
        if (s.swapped) return &s;
    return nullptr;
}

// Melee target selection (user, 2026-10-15): synchronized moves (neck breaker, Leg Back, Sheva's Finisher Front...)
// only work on a real enemy, so a move started without a prompt (swapped or free) takes the nearest living enemy
// within 3 m (300 units, any direction) as its target, P+0x2DA4 (the game's handlers, VA 0xB5D9F0..., set it with
// the point P+0x2E10 = one of the target's joints, VA 0xB56DD0). The point and angle stay Chris's own (his reach point
// in front, his yaw + pi), as for the virtual enemy (user: natively Chris keeps his orientation and the enemy is
// placed along it; with the point on the enemy he turned and moved to it). None within 3 m: the virtual enemy in front
// (no teleport across the map).
constexpr float kMeleeReach = 300.0f;
// Not more than 1.5 m above or below him (another floor), a flying enemy up to 4 m above (forced_death.cpp).
constexpr float kMeleeHeight = 150.0f, kFlyerHeight = 400.0f;

}  // namespace
bool FlyingEnemy(uintptr_t enemy);  // forced_death.cpp
namespace {

uintptr_t NearestEnemy(uintptr_t p, float out[3]) {
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return 0;
    uintptr_t best = 0;
    float bestDist = kMeleeReach * kMeleeReach;
    for (uintptr_t enemy : actors::Enemies()) {
        int16_t health = 0;
        float e[3]{};
        if (!mem::Read(enemy + 0x1364, health) || health <= 0 || !mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), e, sizeof(e)))
            continue;
        const float dx = e[0] - pos[0], dz = e[2] - pos[2], dist = dx * dx + dz * dz;
        const float up = e[1] - pos[1];
        if (dist >= bestDist || up < -kMeleeHeight || up > (FlyingEnemy(enemy) ? kFlyerHeight : kMeleeHeight)) continue;
        best = enemy;
        bestDist = dist;
        std::memcpy(out, e, sizeof(e));
    }
    return best;
}

void AimVirtual(uintptr_t p, bool setAngle, uintptr_t target = 0) {
    constexpr float kPi = 3.14159265f;
    float reach[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x2AD0), reach, sizeof(reach))) return;
    mem::Write<uint32_t>(p + 0x2DA4, static_cast<uint32_t>(target));
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x2E10), reach, sizeof(reach));
    float yaw = 0;
    if (setAngle && mem::Read(p + 0x2A10, yaw)) {
        float facing = yaw + kPi;
        if (facing > kPi) facing -= 2 * kPi;
        mem::Write(p + 0x2E20, facing);
    }
}

// Targets set by the trainer (free melee, swapped moves, the parry's riposte) are "claimed" and taken back once the
// move is over (user, 2026-10-20: drawing the knife sometimes locked Chris and the camera for up to a second - the knife
// draw turns the player to a living melee target, which the game itself only leaves behind dead). A game prompt's
// target is never touched. The swapped moves pick theirs when the move is requested (OnPlayerAction), not every frame.
uintptr_t g_ourTarget[4] = {};
DWORD g_claimTick[4] = {};
// A target the trainer chose on purpose (ClaimMeleeTarget: the head flash after a front escape, the parry's riposte):
// the swapped melee's re-aim at the nearest enemy left it alone for a second (user, 2026-10-23: the head flash after
// an escape went to the nearest enemy, not the Majini that had just let Chris go).
DWORD g_explicitTick[4] = {};

bool IsMeleeMove(uint32_t action) { return (action >= 0x30 && action <= 0x4D) || action == 0x6F || action == 0xAC || action == 0xAD; }

int PlayerSlot(uintptr_t p) { return actors::SlotOf(p); }

void Claim(uintptr_t p, uintptr_t enemy) {
    const int i = PlayerSlot(p);
    if (i < 0) return;
    g_ourTarget[i] = enemy;
    g_claimTick[i] = GetTickCount();
}

// Every frame: a claimed target the player is done with goes back to 0 (as if no prompt had set one).
void ReleaseMeleeTargets() {
    const DWORD now = GetTickCount();
    for (int i = 0; i < 4; ++i) {
        if (!g_ourTarget[i]) continue;
        const uintptr_t p = actors::Slot(i);
        uint32_t target = 0, action = 0;
        if (!p || !mem::Read(p + 0x2DA4, target) || target != g_ourTarget[i]) {  // replaced by the game
            g_ourTarget[i] = 0;
            continue;
        }
        if (!mem::Read(p + 0x10E0, action) || IsMeleeMove(action) || now - g_claimTick[i] < 400) continue;
        mem::Write<uint32_t>(p + 0x2DA4, 0);
        g_ourTarget[i] = 0;
    }
}

// A living listed enemy within melee reach.
bool IsRealTarget(uintptr_t p, uint32_t target) {
    if (!target) return false;
    for (uintptr_t enemy : actors::Enemies())
        if (enemy == target) {
            int16_t health = 0;
            float enemyPos[3]{}, playerPos[3]{};
            if (!mem::Read(enemy + 0x1364, health) || health <= 0 ||
                !mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), enemyPos, sizeof(enemyPos)) ||
                !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), playerPos, sizeof(playerPos)))
                return false;
            const float dx = enemyPos[0] - playerPos[0], dz = enemyPos[2] - playerPos[2];
            return dx * dx + dz * dz < kMeleeReach * kMeleeReach;
        }
    return false;
}

uintptr_t AimMelee(uintptr_t p, bool setAngle) {
    float e[3]{};
    const uintptr_t enemy = NearestEnemy(p, e);
    Claim(p, enemy);
    if (!enemy) {
        AimVirtual(p, setAngle);
        return 0;
    }
    AimVirtual(p, setAngle, enemy);
    return enemy;
}

// The other half of UT's "Melee Swap" checkbox (found with UTSpy during swapped knife slashes, 2026-10-13):
// UT rewrites at 10 Hz, for each player, the melee target P+0x2DA4 = 0 and the point melee moves align on,
// P+0x2E10 xyz = P+0x2AD0 xyz (the point at arm's reach in front of the player); for player 1 also the
// angle P+0x2E20 = yaw (P+0x2A10) + pi, i.e. a virtual enemy right in front, facing the player. Without it, a
// move started outside its usual context (no prompt) aligns on a stale point: the player teleports.
// Done every frame here while any slot is swapped: the virtual point only (no target); the target of a swapped move is
// chosen when it is requested (OnPlayerAction).
void AlignSwappedMelee() {
    for (int i = 0; i < 4; ++i) {
        uintptr_t p = actors::Slot(i);
        if (!p) continue;
        // A real melee target (prompt on a staggered enemy): leave the game's alignment alone, otherwise a swapped
        // finisher aims at the virtual point and misses the enemy (user, 2026-10-13). The game never clears the
        // target: a finished licker stayed there (health 0) and a swapped head flash without target took Chris back
        // to where he had killed it (2026-10-14). Real = an enemy of the list, alive, within melee reach; anything
        // else is cleared like the Ultimate Trainer does.
        uint32_t target = 0, action = 0;
        if (!mem::Read(p + 0x2DA4, target) || !mem::Read(p + 0x10E0, action)) continue;
        // During a melee move nothing is touched (a finisher kills its target before it ends): the point is set
        // between moves, before the next one starts.
        if (IsMeleeMove(action)) continue;
        if (target != 0 && target == g_ourTarget[i]) continue;  // ours, given back by ReleaseMeleeTargets
        if (IsRealTarget(p, target)) continue;                    // a game prompt's target: its own point
        AimVirtual(p, i == 0);                                    // virtual enemy in front, no target
    }
}

bool AnySwapped();

}  // namespace

// Called by the player's RequestAction hook (action_log.cpp) before the game takes the request: a melee move requested
// while a slot is swapped gets the nearest enemy within 3 m as its target, unless a game prompt already set a real one.
void OnPlayerAction(uintptr_t p, int action) {
    if (!IsMeleeMove(static_cast<uint32_t>(action)) || !AnySwapped()) return;
    const int i = PlayerSlot(p);
    uint32_t target = 0;
    if (i < 0 || !mem::Read(p + 0x2DA4, target)) return;
    if (target != 0 && target != g_ourTarget[i] && IsRealTarget(p, target)) return;
    int16_t health = 0;
    if (target != 0 && target == g_ourTarget[i] && g_explicitTick[i] && GetTickCount() - g_explicitTick[i] < 1000 &&
        mem::Read(target + 0x1364, health) && health > 0)
        return;
    AimMelee(p, i == 0);
}

// The parry's riposte (parry.cpp) claims its attacker the same way.
void ClaimMeleeTarget(uintptr_t p, uintptr_t enemy) {
    Claim(p, enemy);
    const int i = PlayerSlot(p);
    if (i >= 0) g_explicitTick[i] = GetTickCount();
}

void RequestThrowFromMelee(uintptr_t target);  // wesker_moves.cpp: Jill's throw at the next frame
void RequestCombatMove(int k, uintptr_t target);  // wesker_moves.cpp: a free melee move at the next frame
int KnifeLegBackMove(uintptr_t p);               // wesker_moves.cpp: the throat cut with the knife in hand, else -1
int WeaponGroundMove(uintptr_t p);               // wesker_moves.cpp: RE6's Ground by the weapon in hand, else -1

// Player's request hook (action_log.cpp), before the game takes it: player 1's request of a slot swapped to Jill's throw
// (its marker action 0, from just after that slot's site: the push of the id precedes the call) is refused and the
// throw started on the move's target (the prompt's enemy, else the nearest within 3 m).
bool MeleeSwapThrow(uintptr_t p, int action, uintptr_t caller) {
    if (action != kThrowMarker || !p || p != actors::Player(0)) return false;
    for (const Slot& slot : g_slots) {
        if (!slot.swapped) continue;
        for (const ByteSite& site : slot.sites) {
            const uintptr_t at = reinterpret_cast<uintptr_t>(mem::Rva(site.rva));
            if (site.current != kThrowMarker || caller <= at || caller - at > 0x40) continue;
            uint32_t target = 0;
            mem::Read(p + 0x2DA4, target);
            if (!IsRealTarget(p, target)) target = 0;
            // The Leg Back's slot with the knife in hand: the throat cut; the Ground's with the knife or a two-handed weapon: RE6's Ground
            // for it - whatever their swap (user, 2026-11-10).
            const int byWeapon = std::strcmp(slot.id, "melee.leg_back") == 0 ? KnifeLegBackMove(p)
                                 : std::strcmp(slot.id, "melee.ground") == 0 ? WeaponGroundMove(p)
                                                                              : -1;
            if (byWeapon >= 0) {
                LOG("melee swap: %s with that weapon in hand -> free melee move %d instead (target %08X)", slot.name, byWeapon,
                    static_cast<unsigned>(target));
                RequestCombatMove(byWeapon, target);
                return true;
            }
            LOG("melee swap: %s -> free melee move %d (target %08X)", slot.name, slot.combat, static_cast<unsigned>(target));
            if (slot.combat == kThrowCombat || slot.combat < 0) RequestThrowFromMelee(target);
            else RequestCombatMove(slot.combat, target);
            return true;
        }
    }
    return false;
}

bool MajiniHoldsStealable(uintptr_t target);  // wesker_moves.cpp: a Majini holding a weapon to strike with
bool PlayWeaponSteal(uintptr_t target);       // wesker_moves.cpp: RE6's weapon steal on it at the next frame
bool PrepareWeskerMoves();                    // wesker_moves.cpp

// "Vol d'arme (RE6) sur les ennemis armés" (user, 2026-11-10: "in the melee swap, an option that replaces every melee prompt with the
// weapon steal when the enemy is armed and staggered, as in RE6"): player 1's request from a site of the staggered enemies' prompts
// (Head Flash, Leg Front / Back, Arm Front / Back, Finisher Front / Back - not Ground, the rescues, the licker's) on a Majini holding a
// melee weapon is refused and RE6's weapon steal played on it, whatever the slot's swap.
std::atomic<bool> g_weaponSteal{ false };
bool MeleeWeaponSteal(uintptr_t p, int action, uintptr_t caller) {
    if (!g_weaponSteal.load(std::memory_order_relaxed) || !p || p != actors::Player(0)) return false;
    static const char* const kStaggerSlots[] = { "melee.head_flash", "melee.leg_front", "melee.leg_back", "melee.arm_front",
                                                 "melee.arm_back", "melee.finisher_front", "melee.finisher_back" };
    for (const Slot& slot : g_slots) {
        if (std::none_of(std::begin(kStaggerSlots), std::end(kStaggerSlots), [&](const char* id) { return std::strcmp(id, slot.id) == 0; }))
            continue;
        for (const ByteSite& site : slot.sites) {
            const uintptr_t at = reinterpret_cast<uintptr_t>(mem::Rva(site.rva));
            if (site.current != static_cast<uint8_t>(action) || caller <= at || caller - at > 0x40) continue;
            uint32_t target = 0;
            mem::Read(p + 0x2DA4, target);
            if (!IsRealTarget(p, target) || !MajiniHoldsStealable(target)) return false;
            LOG("melee swap: %s on an armed Majini %08X -> RE6's weapon steal", slot.name, static_cast<unsigned>(target));
            return PlayWeaponSteal(target);
        }
    }
    return false;
}

bool EnsurePadQueryHooks();             // quick_knife.cpp: the game thread's tick
bool PlayEnvExecution(uintptr_t target);  // wesker_moves.cpp: RE6's execution against an obstacle near it, at the next frame

// "Exécutions contre le décor (RE6) : murs, tables et barrières" (user, 2026-11-11: "replace the attacks by the executions against a wall";
// "for the walls and the barriers the enemy must be staggered"; "a tolerance slider"): player 1's request from a site of the staggered
// enemies' prompts (as the weapon steal) on a Majini with a wall, a table or a low barrier within the tolerance is refused and RE6's
// execution against it played (wesker_moves.cpp EnvExecutionFor); the RE6 melee's finisher too.
std::atomic<bool> g_envWall{ false };
Feature* g_envWallFeature = nullptr;
bool MeleeEnvExecution(uintptr_t p, int action, uintptr_t caller) {
    if (!g_envWall.load(std::memory_order_relaxed) || !p || p != actors::Player(0)) return false;
    static const char* const kStaggerSlots[] = { "melee.head_flash", "melee.leg_front", "melee.leg_back", "melee.arm_front",
                                                 "melee.arm_back", "melee.finisher_front", "melee.finisher_back" };
    for (const Slot& slot : g_slots) {
        if (std::none_of(std::begin(kStaggerSlots), std::end(kStaggerSlots), [&](const char* id) { return std::strcmp(id, slot.id) == 0; }))
            continue;
        for (const ByteSite& site : slot.sites) {
            const uintptr_t at = reinterpret_cast<uintptr_t>(mem::Rva(site.rva));
            if (site.current != static_cast<uint8_t>(action) || caller <= at || caller - at > 0x40) continue;
            uint32_t target = 0;
            mem::Read(p + 0x2DA4, target);
            if (!IsRealTarget(p, target) || !PlayEnvExecution(target)) return false;
            LOG("melee swap: %s on %08X -> RE6's execution against an obstacle", slot.name, static_cast<unsigned>(target));
            return true;
        }
    }
    return false;
}

// wesker_moves.cpp: the option on, its tolerance.
bool EnvWallTolerance(float& tolerance) {
    if (!g_envWall.load(std::memory_order_relaxed) || !g_envWallFeature) return false;
    tolerance = g_envWallFeature->value;
    return true;
}

// "Exécutions d'une hauteur (RE6) : par le dessus et par le dessous" (user, 2026-11-11: "no stagger: the enemy and the player close
// enough to the edge"; "use the jump and the climb prompts, on the attack key with the attack prompt option"): player 1's climb up
// (0x2B) or jump down (0x2A / 0x2C) request with a Majini on the ledge above him or below it within the tolerance is refused and RE6's
// execution played (wesker_moves.cpp EnvHeightFor).
std::atomic<bool> g_envHeight{ false };
Feature* g_envHeightFeature = nullptr;
bool PlayEnvHeightExecution();  // wesker_moves.cpp
bool EnvHeightTolerance(float& tolerance) {
    if (!g_envHeight.load(std::memory_order_relaxed) || !g_envHeightFeature) return false;
    tolerance = g_envHeightFeature->value;
    return true;
}
bool EnvHeightRequest(uintptr_t p, int action) {
    if (!g_envHeight.load(std::memory_order_relaxed) || !p || p != actors::Player(0) || (action != 0x2A && action != 0x2B && action != 0x2C))
        return false;
    const bool played = PlayEnvHeightExecution();
    if (played) LOG("melee swap: crossing %X -> RE6's execution from a height", action);
    return played;
}

bool BorrowFor(uintptr_t p, int set, uint32_t action, bool lendList = true);  // below

// Player's request hook (action_log.cpp), before the game takes it: player 1's request from the site of a slot swapped
// to a Reunion character's move borrows its moveset for it (its list in bank 1, its melee row and motion slots).
void MeleeSwapBorrow(uintptr_t p, int action, uintptr_t caller) {
    if (!p || p != actors::Player(0)) return;
    for (const Slot& slot : g_slots) {
        if (!slot.swapped || slot.lender < 0) continue;
        for (const ByteSite& site : slot.sites) {
            const uintptr_t at = reinterpret_cast<uintptr_t>(mem::Rva(site.rva));
            if (site.current != static_cast<uint8_t>(action) || caller <= at || caller - at > 0x40) continue;
            LOG("melee swap: %s -> lender %d's move %X", slot.name, slot.lender, action);
            BorrowFor(p, slot.lender, static_cast<uint32_t>(action));
            return;
        }
    }
}

namespace {

void AddSlot(const char* id, const char* name, std::vector<ByteSite> sites, const char* note = nullptr) {
    g_slots.push_back({ id, name, std::move(sites) });
    Slot* slot = &g_slots.back();
    std::vector<std::string> choices = { "Par défaut" };
    for (const Action& a : kActions) choices.push_back(a.name);
    // Shown grouped by character (user, 2026-11-10: "you renamed them but did not group them by character as Josh's were: the RE6 ones are
    // mixed, Leon, Chris, Jake, Leon again"): the game's own actions first, then each character's RE5 moves and RE6 moves, the generic RE6
    // Grounds last, a separator between groups; the stored index stays the list's (the saved choices unchanged).
    static const char* const kWho[] = { "Chris", "Sheva", "Jill", "Wesker", "Josh", "Barry", "Rebecca", "Excella",
                                        "Leon", "Helena", "Piers", "Jake", "Sherry", "Ada", "Agent", "RE6" };
    auto groupOf = [](const std::string& name) {
        for (int g = 0; g < static_cast<int>(std::size(kWho)); ++g) {
            const std::string who = kWho[g];
            if (name.compare(0, who.size() + 1, who + " ") == 0) return (g + 1) * 2 + (name.find(" RE6 ") != std::string::npos ? 1 : 0);
        }
        return 0;  // the game's own actions
    };
    std::vector<int> order(choices.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin() + 1, order.end(), [&](int a, int b) { return groupOf(choices[a]) < groupOf(choices[b]); });
    std::vector<int> shown = { 0 };
    for (size_t i = 1; i < order.size(); ++i) {
        if (i > 1 && groupOf(choices[order[i]]) / 2 != groupOf(choices[order[i - 1]]) / 2) shown.push_back(-1);
        shown.push_back(order[i]);
    }
    std::string help = "Remplace ce mouvement par un autre (Chris et Sheva). « Par défaut » remet le mouvement d'origine.\n"
                       "Tant qu'un mouvement est échangé, un ennemi virtuel est placé devant chaque joueur pour que les "
                       "mouvements lancés hors contexte ne téléportent pas (case « Melee Swap » de l'Ultimate Trainer).";
    if (note) help += std::string("\n") + note;
    Add({ .id = id, .name = name,
          .help = help,
          .category = "Mêlée", .kind = Kind::Choice,
          .tick = [slot] {
              if (FirstSwapped() == slot) AlignSwappedMelee();  // once per frame
          },
          .available = Supported,
          .choose = [slot](int index) {
              bool ok = true;
              for (ByteSite& site : slot->sites)
                  ok = site.Set(index == 0 ? site.original : kActions[index - 1].id) && ok;
              slot->swapped = index != 0;
              slot->combat = index == 0 ? -1 : kActions[index - 1].combat;
              slot->lender = index == 0 ? -1 : kActions[index - 1].lender;
              UpdateCompat();
              if (slot->swapped) EnsurePlayerRequestHook();  // swapped moves pick their target when requested
              if (slot->lender >= 0) EnsureMeleeSetHook();   // the lender's row while its move plays
              return ok;
          },
          .value = 0, .valueMin = 0, .valueMax = static_cast<float>(std::size(kActions)), .valueFormat = "%.0f",
          .choices = choices, .choiceOrder = shown });
}

// ---- Character anim sets ---------------------------------------------------------------------
// A character loads its resources from a static list of { path, resource type } (player init calls
// LoadResourceList, RVA 0x7C2810, with the list of its costume). Two paths of that list choose the
// animations: the motion list ("pawn\pl\pl00\motion\pl00action") and the "soft" files (hit shapes...,
// "pawn\pl\pl00\soft\pl00"). The Ultimate Trainer rewrites these strings in the game image; if the other
// character's archive is not loaded in the level (Wesker's only exist in uPl03Wesker*.arc), the game then
// asks for a loose file that does not exist and crashes. Here the strings are never modified: the list is
// copied during the load and a path is replaced only if that resource is already loaded.
struct AnimSet {
    const char* name;
    const char* motion;  // main motion list ("action")
    const char* soft;    // hit shapes...
    int meleeSet;        // row of the melee-move table (VA 0x10FE430, 27 moves x 0xA0 bytes) matching these motions
    bool female;         // pl01 motion family (female weapon / damage motions)
    const char* sound;   // se bank (3 resource types)
    const char* sound2;  // second se type when it differs (Jill: se_pl0200), else nullptr = sound
    const char* adh;     // effects used by the moves
    const char* cef;
    bool ownOnly;        // needs extra resources only its own list has (Excella: em14 effects, em40 path)
    const char* code;    // folder code of its effects (effect\efl\pl\<code>\<code>NN)
};
// Rows from the table read by MeleeSetFor (RVA 0x3E6030): Chris 0, Sheva 1, Jill 2, Wesker 3, Josh
// Reunion/DEC 4 (plain Josh uses Chris's), Excella 5, Barry 6, Rebecca 7, Chris/Sheva in mode 5: 8/9.
// Moveset resources from each character's own list (tools: scratch reslists dump, MODLOG session 5).
const AnimSet kAnimSets[] = {
    { "Chris", "pawn\\pl\\pl00\\motion\\pl00action", "pawn\\pl\\pl00\\soft\\pl00", 0, false, "sound\\se\\pl\\se_pl00",
      nullptr, "effect\\adh\\pl\\pl00", "effect\\cef\\pl\\pl00", false, "pl00" },
    { "Chris Reunion", "pawn\\pl\\pl00\\motion\\pl0Daction", "pawn\\pl\\pl00\\soft\\pl0D", 8, false,
      "sound\\se\\pl\\se_pl00", nullptr, "effect\\adh\\pl\\pl00", "effect\\cef\\pl\\pl00", false, "pl00" },
    { "Sheva", "pawn\\pl\\pl01\\motion\\pl01action", "pawn\\pl\\pl01\\soft\\pl01", 1, true, "sound\\se\\pl\\se_pl01",
      nullptr, "effect\\adh\\pl\\pl01", "effect\\cef\\pl\\pl01", false, "pl01" },
    { "Sheva Reunion", "pawn\\pl\\pl01\\motion\\pl1Naction", "pawn\\pl\\pl01\\soft\\pl1W", 9, true,
      "sound\\se\\pl\\se_pl01", nullptr, "effect\\adh\\pl\\pl01", "effect\\cef\\pl\\pl01", false, "pl01" },
    { "Jill", "pawn\\pl\\pl01\\motion\\pl02action", "pawn\\pl\\pl02\\soft\\pl02", 2, true, "sound\\se\\pl\\se_pl02",
      "sound\\se\\pl\\se_pl0200", "effect\\adh\\pl\\pl00", "effect\\cef\\pl\\pl01", false, "pl02" },
    { "Jill LIN/DEC", "pawn\\pl\\pl01\\motion\\pl09action", "pawn\\pl\\pl02\\soft\\pl2D", 2, true,
      "sound\\se\\pl\\se_pl02", "sound\\se\\pl\\se_pl0200", "effect\\adh\\pl\\pl00", "effect\\cef\\pl\\pl01", false, "pl02" },
    { "Wesker", "pawn\\pl\\pl00\\motion\\pl03action", "pawn\\pl\\pl15\\soft\\pl15", 3, false, "sound\\se\\pl\\se_pl03",
      nullptr, "effect\\adh\\pl\\pl00", "effect\\cef\\pl\\pl15", false, "pl15" },
    { "Josh", "pawn\\pl\\pl00\\motion\\pl00action", "pawn\\pl\\pl00\\soft\\pl0J", 0, false, "sound\\se\\pl\\se_pl00",
      nullptr, "effect\\adh\\pl\\pl00", "effect\\cef\\pl\\pl00", false, "pl00" },
    { "Josh Reunion/DEC", "pawn\\pl\\pl00\\motion\\pl04action", "pawn\\pl\\pl18\\soft\\pl18", 4, false,
      "sound\\se\\pl\\se_pl18", nullptr, "effect\\adh\\em\\em88", "effect\\cef\\pl\\pl18", false, "pl18" },
    { "Barry", "pawn\\pl\\pl00\\motion\\pl06action", "pawn\\pl\\pl1c\\soft\\pl1c", 6, false, "sound\\se\\pl\\se_pl1c",
      nullptr, "effect\\adh\\em\\em88", "effect\\cef\\pl\\pl1c", false, "pl1c" },
    { "Rebecca", "pawn\\pl\\pl01\\motion\\pl07action", "pawn\\pl\\pl1d\\soft\\pl1d", 7, true, "sound\\se\\pl\\se_pl1d",
      nullptr, "effect\\adh\\pl\\pl00", "effect\\cef\\pl\\pl1d", false, "pl1d" },
    { "Excella", "pawn\\pl\\pl01\\motion\\pl05action", "pawn\\pl\\pl1e\\soft\\pl1e", 5, true, "sound\\se\\pl\\se_pl1e",
      nullptr, "effect\\adh\\pl\\pl00", "effect\\cef\\pl\\pl1e", true, "pl1e" },
};

// Each character's own motion / soft path strings (RVA in .rdata), as in UT's 8 combo boxes.
struct CharAnims {
    const char* id;
    const char* name;
    int charId;  // game character id (profile, melee-set table key)
    bool female;  // native motion family
    const char* code;  // its effect folder code
    uintptr_t motionRva, softRva;
    const char* defaultMotion;      // expected content, checked before hooking
    std::atomic<int> set{ 0 };      // chosen: 0 = unchanged, else 1 + index in kAnimSets
    std::atomic<int> applied{ 0 };  // what the last load of this character really used (same encoding)
};
CharAnims g_charAnims[] = {
    { "char.anims_chris", "Animations de Chris", 0, false, "pl00", 0xB990A8, 0xB990FC, "pawn\\pl\\pl00\\motion\\pl00action" },
    { "char.anims_sheva", "Animations de Sheva", 1, true, "pl01", 0xC18FA4, 0xC18FF8, "pawn\\pl\\pl01\\motion\\pl01action" },
    { "char.anims_jill", "Animations de Jill", 2, true, "pl02", 0xC18BE8, 0xC18C34, "pawn\\pl\\pl01\\motion\\pl02action" },
    { "char.anims_wesker", "Animations de Wesker", 3, false, "pl15", 0xC18A0C, 0xC18A40, "pawn\\pl\\pl00\\motion\\pl03action" },
    { "char.anims_josh", "Animations de Josh", 4, false, "pl18", 0xC188A8, 0xC188DC, "pawn\\pl\\pl00\\motion\\pl04action" },
    { "char.anims_barry", "Animations de Barry", 6, false, "pl1c", 0xC186D0, 0xC18704, "pawn\\pl\\pl00\\motion\\pl06action" },
    { "char.anims_rebecca", "Animations de Rebecca", 7, true, "pl1d", 0xC18608, 0xC1863C, "pawn\\pl\\pl01\\motion\\pl07action" },
    { "char.anims_excella", "Animations d'Excella", 5, true, "pl1e", 0xC187F8, 0xC1882C, "pawn\\pl\\pl01\\motion\\pl05action" },
};

// sResource = [re5dx9+0xE34580]. Load(type, path, flags) RVA 0x48B70 (thiscall, ret 0xC): flags 0x80 =
// only look among the resources already loaded (the level's archives), never open a loose file. A found
// resource is add-ref'd: Release RVA 0x4F3E0 (thiscall, no argument).
using ResLoadFn = void*(__fastcall*)(void* mgr, void* edx, void* type, const char* path, unsigned flags);
using ResReleaseFn = void(__fastcall*)(void* res, void* edx);
constexpr unsigned kLookupOnly = 0x80;
constexpr uintptr_t kTypeMotionList = 0xE4B574;  // resource type of the motion-list entries

bool ResourceLoaded(void* type, const char* path) {
    void* mgr = nullptr;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE34580)), mgr) || !mgr || !type) return false;
    auto load = reinterpret_cast<ResLoadFn>(mem::Rva(0x48B70));
    void* res = load(mgr, nullptr, type, path, kLookupOnly);
    if (!res) return false;
    reinterpret_cast<ResReleaseFn>(mem::Rva(0x4F3E0))(res, nullptr);
    return true;
}

struct ResEntry {
    const char* path;
    void* type;
};
using LoadListFn = void(__fastcall*)(void* self, void* edx, ResEntry* list, unsigned count);
LoadListFn g_loadList = nullptr;
std::atomic<bool> g_movesetActive{ false };  // some character currently plays another one's moveset
bool g_loadListHooked = false;

// Resource types (DTI objects, RVA) of the moveset entries of a player's resource list.
constexpr uintptr_t kTypeSoftHit = 0xDE9058;
constexpr uintptr_t kTypeSound[3] = { 0xE4A898, 0xDE98A4, 0xDE973C };
constexpr uintptr_t kTypeAdh = 0xDE8898, kTypeCef = 0xDE8FA4;

bool EndsWith(const char* s, const char* suffix) {
    const size_t a = std::strlen(s), b = std::strlen(suffix);
    return a >= b && std::strcmp(s + a - b, suffix) == 0;
}

// The path the anim set uses for the same role as this entry of the owner's list, or nullptr (keep).
// Model, face, voice and the shared soft entries stay the character's own.
const char* MovesetPath(const AnimSet& set, const ResEntry& e) {
    static const char* const kFamily[2][3] = {
        { "pawn\\pl\\pl00\\motion\\pl00damage", "pawn\\pl\\pl00\\motion\\pl00wp06", "pawn\\pl\\pl00\\motion\\pl00wpdm" },
        { "pawn\\pl\\pl01\\motion\\pl01damage", "pawn\\pl\\pl01\\motion\\pl01wp06", "pawn\\pl\\pl01\\motion\\pl01wpdm" },
    };
    const int fam = set.female ? 1 : 0;
    const auto type = reinterpret_cast<uintptr_t>(e.type);
    if (type == reinterpret_cast<uintptr_t>(mem::Rva(kTypeMotionList))) {
        if (EndsWith(e.path, "action")) return set.motion;
        if (EndsWith(e.path, "damage")) return kFamily[fam][0];
        if (EndsWith(e.path, "wp06")) return kFamily[fam][1];
        if (EndsWith(e.path, "wpdm")) return kFamily[fam][2];
        return nullptr;  // face
    }
    if (type == reinterpret_cast<uintptr_t>(mem::Rva(kTypeSoftHit))) return set.soft;
    for (int k = 0; k < 3; ++k)
        if (type == reinterpret_cast<uintptr_t>(mem::Rva(kTypeSound[k])))
            return k == 1 && set.sound2 ? set.sound2 : set.sound;
    if (type == reinterpret_cast<uintptr_t>(mem::Rva(kTypeAdh))) return set.adh;
    if (type == reinterpret_cast<uintptr_t>(mem::Rva(kTypeCef))) return set.cef;
    return nullptr;
}

// "Reunion moves" checkbox for any Chris / Sheva costume: their long motion lists (with the Reunion moves) are
// only inside the Reunion costume archives, under the same name as the short ones. tools/extract_reunion_motions.py
// puts copies next to the game data under new names; the game loads such loose files itself when they are not
// in an archive (rMotionList -> ".lmt").
const char* const kReunionMotions[2] = { "pawn\\pl\\pl00\\motion\\pl00action_reunion",
                                         "pawn\\pl\\pl01\\motion\\pl01action_reunion" };
// The checkbox "Mouvements Reunion partout" (char.reunion_moves) was removed (user, 2026-11-05: its aim was the duo moves,
// now in the free melee and the melee swap - wesker_moves.cpp kPlayerLists loads these long lists itself): never set.
std::atomic<bool> g_reunionMotionsWanted{ false };
bool g_longMotions[2] = {};  // Chris / Sheva got the long motion list (Reunion moves) at their last load

void __fastcall HookLoadList(void* self, void* edx, ResEntry* list, unsigned count) {
    if (!list || count == 0 || count > 512) return g_loadList(self, edx, list, count);
    // A player's list: first entry = its model ("pawn\pl\..."), and it contains the character's own
    // motion-list path (an enemy using Chris's motions, em85, is left alone).
    if (!list[0].path || std::strncmp(list[0].path, "pawn\\pl\\", 8) != 0) return g_loadList(self, edx, list, count);
    CharAnims* owner = nullptr;
    for (unsigned i = 0; i < count && !owner; ++i)
        for (CharAnims& c : g_charAnims)
            if (list[i].path == reinterpret_cast<const char*>(mem::Rva(c.motionRva))) {
                owner = &c;
                break;
            }
    if (!owner) return g_loadList(self, edx, list, count);

    const int set = owner->charId == 2 && FinalFightJill() ? 0 : owner->set.load();  // final_fight.cpp: her own
    std::vector<ResEntry> copy;
    int replaced = 0, kept = 0;
    bool motionSwapped = false;
    if (set != 0) {
        const AnimSet& as = kAnimSets[set - 1];
        for (unsigned i = 0; i < count; ++i) {
            if (!list[i].path) continue;
            const char* wanted = MovesetPath(as, list[i]);
            if (!wanted) continue;
            const bool motion = EndsWith(list[i].path, "action") &&
                                list[i].type == mem::Rva(kTypeMotionList);
            if (std::strcmp(wanted, list[i].path) == 0) {
                motionSwapped = motionSwapped || motion;
                continue;
            }
            if (!ResourceLoaded(list[i].type, wanted)) {
                ++kept;  // not in this level's archives: keep the character's own file
                continue;
            }
            if (copy.empty()) copy.assign(list, list + count);
            copy[i].path = wanted;  // static strings of the DLL, the game copies the path into the resource
            ++replaced;
            motionSwapped = motionSwapped || motion;
        }
    }
    if (set == 0 && owner->charId <= 1) {
        g_longMotions[owner->charId] = false;
        const char* longList = kReunionMotions[owner->charId];
        if (g_reunionMotionsWanted.load() && ResourceLoaded(mem::Rva(kTypeMotionList), longList)) {
            for (unsigned i = 0; i < count; ++i) {
                if (list[i].path && list[i].type == mem::Rva(kTypeMotionList) && EndsWith(list[i].path, "action")) {
                    if (copy.empty()) copy.assign(list, list + count);
                    copy[i].path = longList;
                    g_longMotions[owner->charId] = true;
                    LOG("reunion moves: %s loads %s", owner->id, longList);
                    break;
                }
            }
        }
    }
    owner->applied = motionSwapped ? set : 0;  // melee row and weapon family follow the motions really loaded
    {
        bool any = false;
        for (const CharAnims& c : g_charAnims) any = any || c.applied.load() > 0;
        g_movesetActive = any;
    }
    if (replaced || kept)
        LOG("anim swap %s: %d path(s) replaced, %d kept (not loaded in this level), melee set %d", owner->id, replaced,
            kept, motionSwapped ? kAnimSets[set - 1].meleeSet : -1);
    g_loadList(self, edx, copy.empty() ? list : copy.data(), count);
}

// MeleeSetFor(character, costume), RVA 0x3E6030 (cdecl), picks the row of the melee-move table: which
// animation numbers and sync data each prompt move (Head Flash, Leg Back...) uses. Chris with Sheva's
// motions must use Sheva's row, otherwise Head Flash plays her Arm Front, Leg Back her Ground move...
using MeleeSetFn = int(__cdecl*)(int character, int costume);
MeleeSetFn g_meleeSetFor = nullptr;
bool g_meleeSetHooked = false;

// ---- Anim sets applied live (user, 2026-10-27: "no restart when I swap a character's combat anims") ------------
// The players of that character get the set's action list in their bank 1 at once (from its archive, loaded when it is
// not: the lists of tools/arc_list.py), and its melee row (applied, read by MeleeSetFor at each move's start); the
// set's sounds and effects at once too (below; user, 2026-10-27); its hit shapes at the next load. "Par défaut" gives the list they
// had when first changed back. No archive for the Reunion variants of Chris / Sheva / Jill (not in this version).
const char* const kAnimSetArchives[] = { "uPl00ChrisNormal", nullptr, "uPl01ShebaNormal", nullptr, "uPl02JillCos1", nullptr,
                                         "uPl03WeskerCos1", "uPl04Josh", "uPl04JoshCos2", "uPl06BarryNormal",
                                         "uPl07RebeccaNormal", "uPl05ExcellaNormal" };
// Their effects archives (the effect folders the resource load hook takes the missing effects from: user, 2026-10-27:
// "the effects are live in the free melee, why not here").
const char* const kAnimSetEffects[] = { "uPl00Eff", "uPl00Eff", "uPl01Eff", "uPl01Eff", "uPl02Eff", "uPl02Eff",
                                        "uPl03Eff", "uPl04Eff", "uPl18Eff", "uPl06Eff", "uPl07Eff", "uPl05Eff" };
void* g_ownActionList[2] = {};
uintptr_t g_ownActionFor[2] = {};
uint32_t CharacterInfo(void* actor);  // below: (character << 8) | costume of an actor's class

// ---- A player's sounds swapped live (user, 2026-10-27: "the sounds and effects too, at least") -------------------
// A player holds its se resources (scanned live on Barry): P+0x1454 the bank (vt RVA 0xCB0FFC, the one the sound requests
// take: read by ~400 sites of the players' code, each move playing its ids into it), P+0x1484 / P+0x1488 the two other se
// types (vt RVA 0xB6A428 / 0xB69FC0, also copied at P+0x1F88 / P+0x1F8C), all three "sound\se\pl\se_plXX" (the
// anim swap's three sound types at load, MovesetPath). Another character's are loaded from its archive (already in) and
// written there while its moves play; his own back after. Our loads stay referenced.
constexpr uintptr_t kSoundFields[3] = { 0x1454, 0x1484, 0x1488 };
constexpr uintptr_t kSoundVtables[3] = { 0xCB0FFC, 0xB6A428, 0xB69FC0 };  // RVA, in kSoundFields' order
constexpr uintptr_t kSoundCopies[3] = { 0, 0x1F88, 0x1F8C };
struct SoundSet {
    void* res[3] = {};
};

// The three se resources of `path` (`path2` for the second type, Jill's se_pl0200), by their vtables.
bool LoadSoundSet(const char* path, const char* path2, SoundSet& out) {
    void* mgr = nullptr;
    if (!path || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE34580)), mgr) || !mgr) return false;
    auto load = reinterpret_cast<ResLoadFn>(mem::Rva(0x48B70));
    out = {};
    for (int k = 0; k < 3; ++k) {
        const char* wanted = k == 1 && path2 ? path2 : path;
        void* type = mem::Rva(kTypeSound[k]);
        if (!ResourceLoaded(type, wanted)) continue;  // only what an archive already holds (no loose file asked)
        void* res = load(mgr, nullptr, type, wanted, 1);
        uintptr_t vtable = 0;
        if (!res || !mem::Read(reinterpret_cast<uintptr_t>(res), vtable)) continue;
        for (int f = 0; f < 3; ++f)
            if (vtable == reinterpret_cast<uintptr_t>(mem::Rva(kSoundVtables[f]))) out.res[f] = res;
    }
    const bool all = out.res[0] && out.res[1] && out.res[2];
    if (!all) LOG("sounds: %s not all loaded (%p %p %p)", path, out.res[0], out.res[1], out.res[2]);
    return all;
}

bool ReadSoundSet(uintptr_t p, SoundSet& out) {
    for (int f = 0; f < 3; ++f) {
        uintptr_t v = 0, vtable = 0;
        if (!mem::Read(p + kSoundFields[f], v) || !v || !mem::Read(v, vtable) ||
            vtable != reinterpret_cast<uintptr_t>(mem::Rva(kSoundVtables[f])))
            return false;  // not the layout seen: left alone
        out.res[f] = reinterpret_cast<void*>(v);
    }
    return true;
}

// `to` in place of `from` (each field still holding its `from`; the copies too).
void WriteSoundSet(uintptr_t p, const SoundSet& from, const SoundSet& to) {
    for (int f = 0; f < 3; ++f) {
        uintptr_t v = 0;
        if (!from.res[f] || !to.res[f]) continue;
        if (mem::Read(p + kSoundFields[f], v) && v == reinterpret_cast<uintptr_t>(from.res[f]))
            mem::Write(p + kSoundFields[f], reinterpret_cast<uintptr_t>(to.res[f]));
        if (kSoundCopies[f] && mem::Read(p + kSoundCopies[f], v) && v == reinterpret_cast<uintptr_t>(from.res[f]))
            mem::Write(p + kSoundCopies[f], reinterpret_cast<uintptr_t>(to.res[f]));
    }
}

SoundSet g_ownSounds[2] = {};
uintptr_t g_ownSoundsFor[2] = {};
SoundSet g_setSounds[std::size(kAnimSets)] = {};

bool ApplyAnimSetLive(CharAnims& c, int index) {
    void* list = nullptr;
    if (index > 0) {
        const char* archive = kAnimSetArchives[index - 1];
        if (!archive) return false;
        LoadWeaponArchive(kAnimSetEffects[index - 1]);
        list = LoadMotionList(archive, kAnimSets[index - 1].motion);
        if (!list) return false;
    }
    bool any = false;
    for (int slot = 0; slot < 2; ++slot) {
        const uintptr_t p = actors::Slot(slot);
        uintptr_t current = 0;
        if (!p || ((CharacterInfo(reinterpret_cast<void*>(p)) >> 8) & 0xFFFF) != static_cast<uint32_t>(c.charId) ||
            !mem::Read(p + 0xA88 + 4, current) || !current)
            continue;
        if (g_ownActionFor[slot] != p) {
            g_ownActionFor[slot] = p;
            g_ownActionList[slot] = reinterpret_cast<void*>(current);
        }
        void* wanted = index > 0 ? list : g_ownActionList[slot];
        if (!wanted || !SetMotionBank(p, wanted, 1)) return false;
        any = true;
        // Its sounds: the set's in place of whatever is there now, or his own back.
        SoundSet now;
        if (g_ownSoundsFor[slot] != p && ReadSoundSet(p, g_ownSounds[slot])) g_ownSoundsFor[slot] = p;
        if (g_ownSoundsFor[slot] == p && ReadSoundSet(p, now)) {
            if (index == 0) {
                WriteSoundSet(p, now, g_ownSounds[slot]);
            } else {
                SoundSet& set = g_setSounds[index - 1];
                if ((set.res[0] && set.res[1] && set.res[2]) ||
                    LoadSoundSet(kAnimSets[index - 1].sound, kAnimSets[index - 1].sound2, set))
                    WriteSoundSet(p, now, set);
            }
        }
        LOG("anim swap %s: player slot %d's action list %s now (live)", c.id, slot,
            index > 0 ? kAnimSets[index - 1].motion : "its own");
    }
    if (!any) return false;
    c.applied = index;
    bool active = false;
    for (const CharAnims& other : g_charAnims) active = active || other.applied.load() > 0;
    g_movesetActive = active;
    return true;
}

// ---- Reunion moves -------------------------------------------------------------------------------------
// In Mercenaries Reunion (mode 5) Reunion characters request the Reunion variants of Leg Front / Head Flash
// themselves (actions 0xAC / 0xAD -> moves 0x17 / 0x18; Barry's headbutt), and MoveIndex (VA 0xC77D20)
// redirects some moves to the duo moves 0x19 / 0x1A when ReunionCheck (RVA 0x3E6820) is true: mode 5 AND the
// player + its partner form a pair of the duo table VA 0x10E9DB0 (Chris+Barry, Chris+Rebecca, Sheva+Josh(Reunion),
// Sheva+Excella, Josh(Reunion)+Barry, Excella+Rebecca). The Ultimate Trainer forced these redirections for
// everybody (Barry's Arm Front became his duo move instead of his elbow jab - user, untouched game 2026-10-13).
// Here, outside mode 5, for Reunion players only: the action is shown to MoveIndex as its Reunion variant and
// ReunionCheck does the native duo test without the mode condition. Chris / Sheva get their Reunion rows (8 / 9).
// Automatic for Reunion characters / costumes; the checkbox adds every Chris / Sheva costume (long motion lists).
bool IsReunionCostume(uint32_t character, uint32_t costume) {
    switch (character) {
    case 0: return costume == 7 || costume == 9;  // Chris Warrior (Cos3), Heavy Metal (Cos4)
    case 1: return costume == 7 || costume == 8;  // Sheva Cos3, Cos4
    case 2: return costume == 3;                  // Jill Cos4
    case 4: return costume == 1;                  // Josh Cos2 (Reunion)
    case 5: case 6: case 7: return true;          // Excella, Barry, Rebecca
    default: return false;
    }
}

std::atomic<bool> g_reunionChecked{ false };    // the checkbox: Reunion moves for every Chris / Sheva costume
std::atomic<bool> g_reunionAutoActive{ false };  // a Reunion character / costume is loaded
std::atomic<bool> g_duoAnyPartner{ false };      // checkbox: duo moves whatever the partner, not only the game's pairs
uint32_t g_loadedChar[2] = { 0xFF, 0xFF }, g_loadedCostume[2] = { 0, 0 };  // sampled when the level loaded

bool MercenariesReunionMode() {
    uintptr_t main = 0, merc = 0;
    uint32_t mode = 0;
    return mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE340A4)), main) && main && mem::Read(main + 0x1042C, merc) &&
           merc && mem::Read(merc + 0x58, mode) && mode == 5;
}

// Other costumes whose archive carries the long motion list with the Reunion moves (tools/arcsizes: pl00action
// 1765760 bytes in Cos3/Cos4/Ep1, pl01action 1791896 in Cos3/Cos4/Plain): Chris Flashback, Sheva Plain. The
// other costumes have the short list: Reunion moves there do not come out (user), so the checkbox skips them.
bool HasReunionMotions(uint32_t character, uint32_t costume) {
    return IsReunionCostume(character, costume) || (character == 0 && costume == 8) || (character == 1 && costume == 4);
}

// Characters that only exist in Mercenaries Reunion: always their Reunion moves (Josh, Excella, Barry, Rebecca).
bool IsReunionCharacter(uint32_t character) { return character >= 4 && character <= 7; }

// Reunion moves wanted for this character / costume (outside mode 5)? Reunion characters always; Chris, Sheva and
// Jill only with the checkbox, whatever the costume (user, 2026-10-13), and only if their motions have the moves.
bool ReunionWanted(uint32_t character, uint32_t costume) {
    if (IsReunionCharacter(character)) return true;
    if (!g_reunionChecked.load() || character > 2) return false;
    return HasReunionMotions(character, costume) || (character <= 1 && g_longMotions[character]) || character == 2;
}

// ---- Borrowed movesets (user, 2026-10-27: "all of Josh's moves in the free melee and the melee swap", then every
// Mercenaries Reunion character) -------------------------------------------------------------------------------------
// A character's melee moves are rows of the melee table (VA 0x10FE430, 0xA0 per move, row * 0x1B + move: +4 its name,
// +0x18 its motion slot in the character's action list (bank 1), +0x1C a second one or 0; dumped live: Chris 0, Sheva 1,
// Jill 2, Wesker 3, Josh Reunion 4, Excella 5, Barry 6, Rebecca 7). The melee move's init (VA 0xC8DA00) reads MoveIndex
// and MeleeSetFor when it starts (the sync data); the character's class then plays its own motion numbers (immediates:
// Chris's are his row's, motions.log). A borrowed move gives player 1's bank 1 the lender's action list (loaded from its
// archive), MeleeSetFor the lender's row for player 1's character, the lender's motion slot in place of player 1's own
// for that move (SetMotion hook: player 1's row's slot -> the lender's row's slot) and the lender's effects folder for
// the effects player 1's own folder lacks (resource load hook), from just before the request until the move's action is
// over (then his own list back). Josh's slots happen to be Chris's numbers (his own animations in pl04action).
struct LoanSet {
    const char* who;      // tab and names
    const char* key;      // feature ids: melee.free_<key>_<move>
    const char* archive;  // Image\Archive\<archive>.arc holding the list
    const char* list;     // the action list
    int row;              // melee table row
    const char* code;     // effect folder (effect\efl\pl\<code>)
    const char* effects;  // its effects archive
    const char* sound;    // its se (sound\\se\\pl\\se_plXX)
    const char* sound2;   // the second se type when it differs (Jill), else nullptr
    // A Reunion character (Josh, Excella, Barry, Rebecca: user, 2026-10-27: "Barry's Head Flash is his headbutt, not
    // his tornado"): its Leg Front / Head Flash are the Reunion variants it requests itself in Mercenaries Reunion
    // (0xAC / 0xAD -> moves 0x17 / 0x18: Barry's HEAD BUTT, Excella's SLAP GRASP), as the anim swap does (HookMoveIndex).
    bool reunion;
    const char* hit;  // its hit shapes: DataDir\hit\<hit>.hit (pawn\pl\..\soft\<hit>, tools/extract_hit_shapes.py)
};
const LoanSet kLoanSets[] = {
    { "Sheva", "sheva", "uPl01ShebaNormal", "pawn\\pl\\pl01\\motion\\pl01action", 1, "pl01", "uPl01Eff", "sound\\se\\pl\\se_pl01", nullptr, false, "pl01" },
    { "Jill", "jill", "uPl02JillCos1", "pawn\\pl\\pl01\\motion\\pl02action", 2, "pl02", "uPl02Eff", "sound\\se\\pl\\se_pl02", "sound\\se\\pl\\se_pl0200", false, "pl02" },
    { "Wesker", "wesker", "uPl03WeskerCos1", "pawn\\pl\\pl00\\motion\\pl03action", 3, "pl03", "uPl03Eff", "sound\\se\\pl\\se_pl03", nullptr, false, "pl15" },
    { "Josh", "josh", "uPl04JoshCos2", "pawn\\pl\\pl00\\motion\\pl04action", 4, "pl18", "uPl18Eff", "sound\\se\\pl\\se_pl18", nullptr, true, "pl18" },
    { "Barry", "barry", "uPl06BarryNormal", "pawn\\pl\\pl00\\motion\\pl06action", 6, "pl1c", "uPl06Eff", "sound\\se\\pl\\se_pl1c", nullptr, true, "pl1c" },
    { "Rebecca", "rebecca", "uPl07RebeccaNormal", "pawn\\pl\\pl01\\motion\\pl07action", 7, "pl1d", "uPl07Eff", "sound\\se\\pl\\se_pl1d", nullptr, true, "pl1d" },
    { "Excella", "excella", "uPl05ExcellaNormal", "pawn\\pl\\pl01\\motion\\pl05action", 5, "pl1e", "uPl05Eff", "sound\\se\\pl\\se_pl1e", nullptr, true, "pl1e" },
    // Chris (user, 2026-10-27: "you forgot Chris: another character may want his moves").
    { "Chris", "chris", "uPl00ChrisNormal", "pawn\\pl\\pl00\\motion\\pl00action", 0, "pl00", "uPl00Eff", "sound\\se\\pl\\se_pl00", nullptr, false, "pl00" },
};
constexpr int kJosh = 3;  // index in kLoanSets
void* g_loanLists[std::size(kLoanSets)] = {};

struct Loan {
    std::atomic<bool> active{ false };
    std::atomic<int> character{ -1 };  // player 1's character id (his class: CharacterInfo >> 8): the row is his
    std::atomic<int> row{ 0 };
    std::atomic<int> ownRow{ 0 };      // player 1's own row (the game's MeleeSetFor for his character)
    std::atomic<int> set{ -1 };        // index in kLoanSets
    uintptr_t player = 0;  // render / game thread: set before the request, ended by TickLoan
    void* own = nullptr;   // his own bank 1 list
    void* lent = nullptr;  // the lender's list in his bank 1
    std::atomic<uint32_t> action{ 0 };
    DWORD since = 0;
    bool started = false;
    // His model's part mask P+0x150 (16 bytes of visibility bits) when the loan started: the lender's motions switch
    // parts of the lender's model on (Rebecca's spray can is a piece of her model: user, 2026-10-27, Barry's magnum, the
    // same piece number of his, came out instead), meaningless on his: kept as it was while another's moveset plays.
    uint8_t parts[16] = {};
    bool partsKept = false;
    SoundSet ownSounds, lentSounds;  // his sounds and the lender's while its move plays
    bool soundsLent = false;
};
Loan g_loan;
SoundSet g_lenderSounds[std::size(kLoanSets)] = {};

uint32_t CharacterInfo(void* actor);  // below

// The melee move index of a player action (MoveIndex VA 0xC77D20, without its Reunion remaps).
int MoveOfAction(uint32_t action) {
    if (action >= 0x30 && action <= 0x46) return static_cast<int>(action - 0x30);
    if (action == 0xAC) return 0x17;
    if (action == 0xAD) return 0x18;
    return -1;
}

// The move the lender plays for `action`: its Reunion Leg Front / Head Flash for a Reunion lender.
int LenderMove(int set, uint32_t action) {
    if (set >= 0 && kLoanSets[set].reunion && (action == 0x31 || action == 0x32)) return action == 0x31 ? 0x17 : 0x18;
    return MoveOfAction(action);
}

void MeleeEntry(int row, int move, uint32_t& slot, uint32_t& second) {
    const uintptr_t entry = reinterpret_cast<uintptr_t>(mem::Rva(0xCFE430)) + (static_cast<uintptr_t>(row) * 0x1B + move) * 0xA0;
    slot = second = 0;
    mem::Read(entry + 0x18, slot);
    mem::Read(entry + 0x1C, second);
}

const char* MeleeName(int row, int move) {
    const uintptr_t entry = reinterpret_cast<uintptr_t>(mem::Rva(0xCFE430)) + (static_cast<uintptr_t>(row) * 0x1B + move) * 0xA0;
    uintptr_t name = 0;
    return mem::Read(entry + 4, name) && name ? reinterpret_cast<const char*>(name) : "";
}

// His model's parts as they were (Borrow), if the lender's motion switched one.
void KeepParts(uintptr_t p) {
    uint8_t now[16] = {};
    if (!g_loan.partsKept || !p || !mem::SafeRead(reinterpret_cast<const void*>(p + 0x150), now, sizeof(now)) ||
        std::memcmp(now, g_loan.parts, sizeof(now)) == 0)
        return;
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x150), g_loan.parts, sizeof(g_loan.parts));
    static int logged = 0;
    if (logged++ < 10) LOG("borrowed moveset: model parts put back (%02X%02X%02X%02X -> %02X%02X%02X%02X)", now[3], now[2],
                           now[1], now[0], g_loan.parts[3], g_loan.parts[2], g_loan.parts[1], g_loan.parts[0]);
}

// ---- A borrowed move's attack shapes (user, 2026-10-31: Sheva's head flash borrowed by Chris never hit, "check every
// character"). A melee move arms the shape its melee row names (the entry +0x60, VA 0xC7E09B: SetAttackShape VA 0xC442D0),
// the lender's row while a loan lasts - a number of the lender's shape list, another limb on the borrower: every
// character has the same numbers (0x10..0x17, 0x1E, 0x1F) on its own bones (Chris's 0x11 = his right hand, bone 9;
// Sheva's = her left foot, bone 0x16, her spin kick). So while a loan lasts player 1's attack shapes (+0xC4 type 0)
// take the lender's geometry from its HIT file (DataDir\hit\<hit>.hit: kind +0x94, bones +0xB8 / +0xBA, radius +0xA8,
// points +0x70 / +0x80, all read again every frame - VA 0xBEFFC0 takes the bones there), his own given back at its end.
struct ShapeGeometry {
    uint32_t kind = 0, bones = 0;
    float radius = 0, a[3]{}, b[3]{};
    bool operator==(const ShapeGeometry& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
struct HitFile {
    bool tried = false;
    std::vector<ShapeGeometry> shapes;
};
HitFile g_hitFiles[std::size(kLoanSets)];
uintptr_t g_shapesPlayer = 0;
int g_shapesSet = -1;  // the lender whose geometry player 1's shapes have (index in kLoanSets)
std::vector<std::pair<uintptr_t, ShapeGeometry>> g_ownShapes;  // the borrower's shapes changed, their own geometry

bool LoadHitFile(int set) {
    HitFile& file = g_hitFiles[set];
    if (file.tried) return !file.shapes.empty();
    file.tried = true;
    const std::string name(kLoanSets[set].hit);
    std::ifstream in(logx::DataDir() + L"\\hit\\" + std::wstring(name.begin(), name.end()) + L".hit", std::ios::binary);
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    uint32_t count = 0;
    if (d.size() < 0x10 || std::memcmp(d.data(), "HIT ", 4) != 0) {
        LOG("borrowed moveset: no hit shapes for %s (DataDir\\hit\\%s.hit: tools/extract_hit_shapes.py)", kLoanSets[set].who,
            kLoanSets[set].hit);
        return false;
    }
    std::memcpy(&count, d.data() + 8, 4);
    for (uint32_t i = 0; i < count && 0x10 + (i + 1) * 0x2A <= d.size(); ++i) {
        const uint8_t* r = d.data() + 0x10 + i * 0x2A;
        ShapeGeometry g;
        int16_t a = 0, b = 0;
        g.kind = r[0];
        std::memcpy(&a, r + 1, 2);
        std::memcpy(&b, r + 3, 2);
        g.bones = static_cast<uint16_t>(a) | (static_cast<uint32_t>(static_cast<uint16_t>(b)) << 16);
        std::memcpy(&g.radius, r + 0x0A, 4);
        std::memcpy(g.a, r + 0x0E, 12);
        std::memcpy(g.b, r + 0x1A, 12);
        file.shapes.push_back(g);
    }
    LOG("borrowed moveset: %s's hit shapes loaded (%u)", kLoanSets[set].who, static_cast<unsigned>(file.shapes.size()));
    return !file.shapes.empty();
}

ShapeGeometry ReadShape(uintptr_t shape) {
    ShapeGeometry g;
    mem::Read(shape + 0x94, g.kind);
    mem::Read(shape + 0xB8, g.bones);
    mem::Read(shape + 0xA8, g.radius);
    mem::SafeRead(reinterpret_cast<void*>(shape + 0x70), g.a, sizeof(g.a));
    mem::SafeRead(reinterpret_cast<void*>(shape + 0x80), g.b, sizeof(g.b));
    return g;
}

void WriteShape(uintptr_t shape, const ShapeGeometry& g) {
    mem::Write(shape + 0x94, g.kind);
    mem::Write(shape + 0xB8, g.bones);
    mem::Write(shape + 0xA8, g.radius);
    mem::SafeWrite(reinterpret_cast<void*>(shape + 0x70), g.a, sizeof(g.a));
    mem::SafeWrite(reinterpret_cast<void*>(shape + 0x80), g.b, sizeof(g.b));
}

// ---- Player 1's attack shapes mirrored with his hand (user, 2026-10-31: "Sheva's Arm Front does not hit right-handed, a
// zone problem as with the borrowed moves"): with the other hand than his character's ("Main inversée") the game plays his
// motions mirrored - the other fist, the other foot - but his attack shapes stay on his own limbs (his HIT file: Sheva's
// 0x11 on her left foot). Meanwhile their bones are swapped right / left (arms 6-9 / 0xB-0xE, legs 0x10-0x13 /
// 0x14-0x17, clavicles 5 / 0xA) and their points' x negated; his own back with his own hand, and around a loan's
// changes (the next frame mirrors the lent ones).
// Not every motion (user, 2026-11-01: "Sheva's Ground missed right-handed too, check every move": her blade's capsule
// lay sideways): a left-handed player's requests (VA 0xB7C9D0, kind bit 2) toggle the layer's mirror bit 0x100 unless
// they are bank 1's 0x0F / 0x10 / 0x11 / 0x5F / 0x60 (VA 0xB5B850: Sheva's Arm Back 0x10F and Ground 0x110 play as
// made in either hand, her wrists recorded at the same places). So the shapes are mirrored while layer 0's mirror bit
// is not the one their author's own play of that motion has: left-handed (Sheva's HIT file, or a loan of hers) and
// not one of those five.
std::vector<std::pair<uintptr_t, ShapeGeometry>> g_mirrored;  // render thread: shapes mirrored, their geometry before
uintptr_t g_mirrorPlayer = 0;

bool MirroredForLeftHand(uint16_t motion) {
    if ((motion >> 8) != 1) return true;
    switch (motion & 0xFF) {
    case 0x0F: case 0x10: case 0x11: case 0x5F: case 0x60: return false;
    default: return true;
    }
}

// Layer 0 (+0x380): motion +4, mode +0xA.
bool ShapesWantMirror(uintptr_t p) {
    uint16_t motion = 0, mode = 0;
    if (!mem::Read(p + 0x380 + 4, motion) || !mem::Read(p + 0x380 + 0xA, mode)) return Player1HandInverted();
    const bool authorLeft = g_shapesPlayer == p && g_shapesSet >= 0 ? std::strcmp(kLoanSets[g_shapesSet].key, "sheva") == 0
                                                                     : Player1NativeLeftHanded();
    return ((mode & 0x100) != 0) != (authorLeft && MirroredForLeftHand(motion));
}

uint16_t MirrorBone(uint16_t bone) {
    static constexpr uint8_t kPairs[][2] = { { 5, 0xA },     { 6, 0xB },     { 7, 0xC },     { 8, 0xD },    { 9, 0xE },
                                             { 0x10, 0x14 }, { 0x11, 0x15 }, { 0x12, 0x16 }, { 0x13, 0x17 } };
    for (const auto& pair : kPairs) {
        if (bone == pair[0]) return pair[1];
        if (bone == pair[1]) return pair[0];
    }
    return bone;
}

void UnmirrorShapes() {
    if (g_mirrorPlayer && g_mirrorPlayer == actors::Player(0))
        for (const auto& [shape, before] : g_mirrored) WriteShape(shape, before);
    g_mirrored.clear();
    g_mirrorPlayer = 0;
}

void TickShapeMirror() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p || p != g_mirrorPlayer) {  // a new level, a new player: his shapes are his own
        g_mirrored.clear();
        g_mirrorPlayer = 0;
    }
    const bool want = p && ShapesWantMirror(p);
    if (!want) {
        if (g_mirrorPlayer) {
            UnmirrorShapes();
            uint16_t motion = 0;
            mem::Read(p + 0x380 + 4, motion);
            LOG("attack shapes: player 1's shapes as made (motion %X)", motion);
        }
        return;
    }
    if (g_mirrorPlayer) return;
    uintptr_t shapes = 0;
    uint32_t count = 0, capacity = 0;
    if (!mem::Read(p + 0x1290 + 4, shapes) || !shapes || !mem::Read(p + 0x1290 + 0x14, capacity) ||
        !mem::Read(p + 0x1290 + 0x18, count))
        return;
    count = std::min({ count, capacity, 64u });
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t shape = 0;
        uint32_t type = 1;
        if (!mem::Read(shapes + 4 * i, shape) || !shape || !mem::Read(shape + 0xC4, type) || type != 0) continue;
        const ShapeGeometry before = ReadShape(shape);
        ShapeGeometry g = before;
        g.bones = MirrorBone(static_cast<uint16_t>(g.bones & 0xFFFF)) |
                  (static_cast<uint32_t>(MirrorBone(static_cast<uint16_t>(g.bones >> 16))) << 16);
        g.a[0] = -g.a[0];
        g.b[0] = -g.b[0];
        if (g == before) continue;
        g_mirrored.push_back({ shape, before });
        WriteShape(shape, g);
    }
    g_mirrorPlayer = p;
    uint16_t motion = 0;
    mem::Read(p + 0x380 + 4, motion);
    LOG("attack shapes: player 1's motion %X mirrored from its author's, %u attack shapes mirrored", motion,
        static_cast<unsigned>(g_mirrored.size()));
}

void RestoreShapes() {
    UnmirrorShapes();  // the loan's geometry as it was given
    if (g_shapesPlayer && g_shapesPlayer == actors::Player(0))
        for (const auto& [shape, own] : g_ownShapes) WriteShape(shape, own);
    if (!g_ownShapes.empty()) LOG("borrowed moveset: player 1's own attack shapes back (%u)", static_cast<unsigned>(g_ownShapes.size()));
    g_ownShapes.clear();
    g_shapesPlayer = 0;
    g_shapesSet = -1;
}

void LendShapes(uintptr_t p, int set) {
    RestoreShapes();
    if (!LoadHitFile(set)) return;
    const std::vector<ShapeGeometry>& lent = g_hitFiles[set].shapes;
    uintptr_t shapes = 0;
    uint32_t count = 0, capacity = 0;
    if (!mem::Read(p + 0x1290 + 4, shapes) || !shapes || !mem::Read(p + 0x1290 + 0x14, capacity) ||
        !mem::Read(p + 0x1290 + 0x18, count))
        return;
    count = std::min({ count, capacity, static_cast<uint32_t>(lent.size()), 64u });
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t shape = 0;
        uint32_t type = 1;
        if (!mem::Read(shapes + 4 * i, shape) || !shape || !mem::Read(shape + 0xC4, type) || type != 0) continue;
        const ShapeGeometry own = ReadShape(shape);
        if (own == lent[i]) continue;
        g_ownShapes.push_back({ shape, own });
        WriteShape(shape, lent[i]);
    }
    g_shapesPlayer = p;
    g_shapesSet = set;
    LOG("borrowed moveset: %u attack shapes of player 1 given %s's", static_cast<unsigned>(g_ownShapes.size()), kLoanSets[set].who);
}

// ---- A borrowed move's voice (user, 2026-10-31: "les coups échangés jouent la voix du personnage en plus des sons des
// frappes ... remplace la voix par celle du perso joué"). The lender's se bank (P+0x1454, lent with the moves' sounds)
// plays its grunts too. A bank's requests (srq records of 0x9C bytes, live at +0x68, count +0x64: VA 0x497A40 searches
// them by their u16 id) each name a wave package (+0x1E): package 1 is the character's voice (none of its waves alike
// between Chris, Sheva and Jill, the blows' and clothes' packages mostly the same waves). Every sound request is queued by
// VA 0x497780 (stdcall(system, bank, id, unit, ...), ret 0x28): one of player 1's in the lent bank asking a voice goes to
// his own bank (the same id is the same line in every bank: 191 of their 222 ids in the same package), or is dropped when
// his bank has no voice under that id.
constexpr int16_t kVoicePackage = 1;
std::atomic<uintptr_t> g_voiceLent{ 0 }, g_voiceOwn{ 0 }, g_voicePlayer{ 0 };
// A trainer move's loan (a duo move: Borrow with lendList false; user, 2026-11-06: "I hear Rebecca's voice playing
// Chris"): every request of the lender's bank whose id player 1's own bank has in the same package is played from his
// bank - her breaths and efforts are not all in the voice package (her duo move asks 80 in package 10, 26 / 27 in
// package 3, both in Chris's bank too); what only she has (her spray, 225 in package 20) stays hers.
std::atomic<bool> g_voiceAllShared{ false };
std::atomic<int> g_voiceLogs{ 0 };

// The wave package of request `id` in a bank, -1 none (or an empty record).
int RequestPackage(uintptr_t bank, uint32_t id) {
    uint32_t count = 0;
    uintptr_t records = 0;
    if (!bank || !mem::Read(bank + 0x64, count) || !mem::Read(bank + 0x68, records) || !records) return -1;
    count = std::min<uint32_t>(count, 4096);
    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t r = records + i * 0x9C;
        uint16_t rid = 0;
        if (!mem::Read(r, rid)) return -1;
        if (rid != id) continue;
        uint32_t kind = 0;
        int16_t package = -1;
        return mem::Read(r + 8, kind) && kind && mem::Read(r + 0x1E, package) ? package : -1;
    }
    return -1;
}

using SoundRequestFn = void(__stdcall*)(void* system, uintptr_t bank, uint32_t id, uintptr_t unit, void* a5, uint32_t a6,
                                         uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10);
SoundRequestFn g_soundRequest = nullptr;
bool g_soundRequestHooked = false;

// "Journal des sons des personnages (recherche)" (user, 2026-10-31: the trainer's moves with the native sounds - the neck
// break's crack, Jill's and Wesker's kicks): every request of player 1 and the enemies (bank path, id, unit, its action
// and layer 0 motion at that frame), the same one once in 300 ms.
std::atomic<bool> g_soundLog{ false };
std::atomic<int> g_soundLogLines{ 0 };

void LogSoundRequest(uintptr_t bank, uint32_t id, uintptr_t unit) {
    if (g_soundLogLines.load(std::memory_order_relaxed) >= 20000 || !unit) return;
    const uintptr_t p1 = actors::Player(0);
    bool ours = unit == p1;
    if (!ours)
        for (uintptr_t e : actors::Enemies()) ours = ours || e == unit;
    if (!ours) return;
    // The enemies near him only (the melee's: the whole stage's footsteps filled the log at once).
    float a[3]{}, b[3]{};
    if (unit != p1 && (!mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), a, sizeof(a)) ||
                       !mem::SafeRead(reinterpret_cast<void*>(unit + 0x30), b, sizeof(b)) ||
                       std::hypot(a[0] - b[0], a[2] - b[2]) > 400.0f))
        return;
    static uintptr_t lastBank = 0, lastUnit = 0;  // game thread
    static uint32_t lastId = 0;
    static DWORD lastTick = 0;
    const DWORD now = GetTickCount();
    if (bank == lastBank && id == lastId && unit == lastUnit && now - lastTick < 300) return;
    lastBank = bank;
    lastId = id;
    lastUnit = unit;
    lastTick = now;
    char path[48]{};
    mem::SafeRead(reinterpret_cast<void*>(bank + 4), path, sizeof(path) - 1);
    const char* tail = std::strrchr(path, '\\');
    uint32_t action = 0, motion = 0;
    float frame = 0.0f;
    mem::Read(unit + 0x10E0, action);
    mem::Read(unit + 0x384, motion);
    mem::Read(unit + 0x398, frame);
    g_soundLogLines.fetch_add(1);
    LOG("sound: %s %u (package %d) by %s %08X%s, action %X motion %X frame %.0f", tail ? tail + 1 : path, id,
        RequestPackage(bank, id), unit == p1 ? "player 1" : UnitClassName(unit), static_cast<unsigned>(unit),
        unit == p1 ? "" : " (enemy)", action, motion & 0xFFFF, frame);
}

void __stdcall HookSoundRequest(void* system, uintptr_t bank, uint32_t id, uintptr_t unit, void* a5, uint32_t a6, uint32_t a7,
                                uint32_t a8, uint32_t a9, uint32_t a10) {
    if (g_soundLog.load(std::memory_order_relaxed)) LogSoundRequest(bank, id, unit);
    const uintptr_t lent = g_voiceLent.load(std::memory_order_relaxed);
    const int package = lent && bank == lent ? RequestPackage(bank, id) : -1;
    if (package >= 0 && package != kVoicePackage && g_voiceAllShared.load(std::memory_order_relaxed) &&
        unit == g_voicePlayer.load(std::memory_order_relaxed)) {
        const uintptr_t own = g_voiceOwn.load(std::memory_order_relaxed);
        if (own && RequestPackage(own, id) == package) {
            if (g_voiceLogs.fetch_add(1) < 40)
                LOG("borrowed moveset: the lender's sound %u (package %d) played from player 1's own bank", id, package);
            bank = own;
        }
    } else if (package == kVoicePackage) {
        const uintptr_t player = g_voicePlayer.load(std::memory_order_relaxed);
        const uintptr_t own = g_voiceOwn.load(std::memory_order_relaxed);
        if (unit == player && own) {
            const bool mine = RequestPackage(own, id) == kVoicePackage;
            if (g_voiceLogs.fetch_add(1) < 40)
                LOG("borrowed moveset: the lender's voice %u %s", id, mine ? "played in player 1's own voice" : "dropped");
            if (!mine) return;
            bank = own;
        } else if (g_voiceLogs.fetch_add(1) < 40) {
            LOG("borrowed moveset: voice %u of the lender's bank for unit %08X (player 1 %08X): kept", id,
                static_cast<unsigned>(unit), static_cast<unsigned>(player));
        }
    }
    g_soundRequest(system, bank, id, unit, a5, a6, a7, a8, a9, a10);
}

bool EnsureSoundRequestHook() {
    if (g_soundRequestHooked) return true;
    uint8_t* target = mem::Rva(0x97780);
    const uint8_t expected[] = { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("sound request hook: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookSoundRequest), reinterpret_cast<void**>(&g_soundRequest));
    if (st != MH_OK || !g_soundRequest) {  // the research call log "sound" (diagnostics.cpp) may hold the site
        LOG("sound request hook: not installed (%d)", static_cast<int>(st));
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    g_soundRequestHooked = true;
    LOG("sound request hook installed at %p", target);
    return true;
}

void LendVoice(uintptr_t p) {
    const uintptr_t lent = reinterpret_cast<uintptr_t>(g_loan.lentSounds.res[0]);
    const uintptr_t own = reinterpret_cast<uintptr_t>(g_loan.ownSounds.res[0]);
    g_voiceLent = 0;
    if (!g_loan.soundsLent || !lent || !own || lent == own || !EnsureSoundRequestHook()) return;
    g_voiceOwn = own;
    g_voicePlayer = p;
    g_voiceLent = lent;
}

void EndLoan(const char* why) {
    if (!g_loan.active.load()) return;
    g_voiceLent = 0;
    g_voiceAllShared = false;
    RestoreShapes();
    const uintptr_t p = g_loan.player;
    if (p && p == actors::Player(0)) KeepParts(p);
    if (p && p == actors::Player(0) && g_loan.soundsLent) WriteSoundSet(p, g_loan.lentSounds, g_loan.ownSounds);
    g_loan.soundsLent = false;
    uintptr_t current = 0;
    if (p && p == actors::Player(0) && g_loan.own && g_loan.lent && mem::Read(p + 0xA88 + 4, current) &&
        current == reinterpret_cast<uintptr_t>(g_loan.lent))
        SetMotionBank(p, g_loan.own, 1);
    g_loan.active = false;
    g_loan.player = 0;
    LOG("borrowed moveset: %s's ended (%s)", kLoanSets[g_loan.set.load()].who, why);
}

// A character's moveset lent to player 1 for the move `action` he is about to request. `lendList` false: his own list
// kept in bank 1, only the rest lent (effects, sounds, model parts, voice, shapes) - a trainer move playing the lender's
// motion from its own bank (wesker_moves.cpp): its carrier action reads its own motion in bank 1 (slot 0x13), whose
// length in another list cut Excella's duo move at once and held Rebecca's carrier 13 s (2026-11-05).
bool Borrow(uintptr_t p, int set, uint32_t action, bool lendList = true) {
    if (!p || set < 0 || set >= static_cast<int>(std::size(kLoanSets))) return false;
    const LoanSet& ls = kLoanSets[set];
    if (!g_loanLists[set]) {
        LoadWeaponArchive(ls.effects);  // its effects, for the moves that spawn some (sprays, shots...)
        g_loanLists[set] = LoadMotionList(ls.archive, ls.list);
    }
    uintptr_t own = 0;
    if (!g_loanLists[set] || !mem::Read(p + 0xA88 + 4, own) || !own) {
        LOG("borrowed moveset: %s's list not available", ls.who);
        return false;
    }
    if (g_loan.active.load() && g_loan.player == p && g_loan.set.load() != set) EndLoan("another lender");
    if (!g_loan.active.load() || g_loan.player != p) {
        g_loan.lent = g_loanLists[set];
        if (own == reinterpret_cast<uintptr_t>(g_loanLists[set])) {
            g_loan.own = nullptr;  // the lender himself, or his set already there: nothing to give back
            g_loan.partsKept = false;
        } else {
            g_loan.own = lendList ? reinterpret_cast<void*>(own) : nullptr;
            if (lendList && !SetMotionBank(p, g_loanLists[set], 1)) return false;
            g_loan.partsKept = mem::SafeRead(reinterpret_cast<const void*>(p + 0x150), g_loan.parts, sizeof(g_loan.parts));
            // The lender's sounds (user, 2026-10-27).
            SoundSet& lent = g_lenderSounds[set];
            g_loan.soundsLent = ReadSoundSet(p, g_loan.ownSounds) &&
                                ((lent.res[0] && lent.res[1] && lent.res[2]) || LoadSoundSet(ls.sound, ls.sound2, lent));
            if (g_loan.soundsLent) {
                g_loan.lentSounds = lent;
                WriteSoundSet(p, g_loan.ownSounds, lent);
            }
        }
    }
    const uint32_t info = CharacterInfo(reinterpret_cast<void*>(p));
    const int character = info == 0xFFFFFFFF ? -1 : static_cast<int>((info >> 8) & 0xFFFF);
    g_loan.character = character;
    g_loan.ownRow = character >= 0 && g_meleeSetFor ? g_meleeSetFor(character, static_cast<int>(info & 0xFF)) : 0;
    g_loan.row = ls.row;
    g_loan.set = set;
    g_loan.player = p;
    g_loan.action = action;
    g_loan.since = GetTickCount();
    g_loan.started = false;
    g_loan.active = true;
    LOG("borrowed moveset: %s's for action %X (character %d, own row %d)", ls.who, action, character, g_loan.ownRow.load());
    LendShapes(p, set);
    LendVoice(p);
    g_voiceAllShared = !lendList;
    return true;
}

// SetMotion hook: player 1's own slot for the borrowed move -> the lender's (Barry's head flash is his tornado 0x10E
// where Chris's code plays 0x112).
uint32_t LoanMotion(uintptr_t unit, uint32_t layer, uint32_t motion) {
    if (layer != 0 || !g_loan.active.load(std::memory_order_relaxed) || unit != g_loan.player || (motion & 0xFF00) != 0x100)
        return motion;
    uint32_t action = 0;
    if (!mem::Read(unit + 0x10E0, action) || action != g_loan.action.load(std::memory_order_relaxed)) return motion;
    const int move = MoveOfAction(action);
    if (move < 0) return motion;
    uint32_t own = 0, ownSecond = 0, lent = 0, lentSecond = 0;
    MeleeEntry(g_loan.ownRow.load(), move, own, ownSecond);
    MeleeEntry(g_loan.row.load(), LenderMove(g_loan.set.load(), action), lent, lentSecond);
    const uint32_t slot = motion & 0xFF;
    uint32_t wanted = slot;
    if (slot == own && lent) wanted = lent;
    else if (ownSecond && slot == ownSecond && lentSecond) wanted = lentSecond;
    if (wanted == slot) return motion;
    LOG("borrowed moveset: motion %X -> %X", motion, 0x100 | wanted);
    return 0x100 | wanted;
}

// Wesker's dash (knee bazooka): its actions follow one another (first 0x47 -> second 0x48 -> third 0x49 / fail 0x4A,
// turns 0x4B-0x4D): one loan for all of them.
bool KneeBazookaAction(uint32_t action) { return action >= 0x47 && action <= 0x4D; }

// Every frame: the loan ends once its move is over (or never started within a second).
void TickLoan() {
    if (!g_loan.active.load()) return;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || p != g_loan.player || !mem::Read(p + 0x10E0, action)) {
        g_voiceLent = 0;
        RestoreShapes();        // its shapes given back only to the same player
        g_loan.active = false;  // a new level, a new player: his own lists
        g_loan.player = 0;
        return;
    }
    KeepParts(p);
    if (action == g_loan.action.load() || (KneeBazookaAction(g_loan.action.load()) && KneeBazookaAction(action))) {
        g_loan.started = true;
        return;
    }
    if (g_loan.started) EndLoan("move over");
    else if (GetTickCount() - g_loan.since > 1000) EndLoan("move never started");
}

int __cdecl HookMeleeSetFor(int character, int costume) {
    if (g_loan.active.load(std::memory_order_relaxed) &&
        (g_loan.character.load(std::memory_order_relaxed) < 0 || character == g_loan.character.load(std::memory_order_relaxed)))
        return g_loan.row.load(std::memory_order_relaxed);
    for (const CharAnims& c : g_charAnims) {
        if (c.charId != character) continue;
        const int applied = c.applied.load();
        if (applied > 0) return kAnimSets[applied - 1].meleeSet;
        break;
    }
    if ((character == 0 || character == 1) && (g_reunionAutoActive.load() || g_reunionChecked.load()) &&
        !MercenariesReunionMode() && ReunionWanted(static_cast<uint32_t>(character), static_cast<uint32_t>(costume)))
        return character + 8;  // Chris / Sheva Reunion rows
    return g_meleeSetFor(character, costume);
}

// MoveIndex(player), VA 0xC77D20 (cdecl): action -> melee move index.
using MoveIndexFn = int(__cdecl*)(void* player);
MoveIndexFn g_moveIndex = nullptr;
bool g_moveIndexHooked = false;

bool PlayerWantsReunion(void* player) {
    if (!player || !(g_reunionAutoActive.load() || g_reunionChecked.load()) || MercenariesReunionMode()) return false;
    const int index = actors::SlotOf(reinterpret_cast<uintptr_t>(player));  // the profile's player block
    return index >= 0 && index < 2 && ReunionWanted(g_loadedChar[index], g_loadedCostume[index]);
}

int __cdecl HookMoveIndex(void* player) {
    // A borrowed move of a Reunion lender: its Leg Front / Head Flash are moves 0x17 / 0x18.
    if (g_loan.active.load(std::memory_order_relaxed) && reinterpret_cast<uintptr_t>(player) == g_loan.player) {
        uint32_t action = 0;
        mem::Read(reinterpret_cast<uintptr_t>(player) + 0x10E0, action);
        const int set = g_loan.set.load();
        if (action == g_loan.action.load() && set >= 0 && kLoanSets[set].reunion && (action == 0x31 || action == 0x32))
            return LenderMove(set, action);
    }
    // A character with a Reunion character's moveset ("Animations de X"): its Leg Front / Head Flash variants.
    if (g_movesetActive.load(std::memory_order_relaxed)) {
        uint32_t action = 0;
        const uint32_t info = CharacterInfo(player);
        if ((mem::Read(reinterpret_cast<uintptr_t>(player) + 0x10E0, action) && (action == 0x31 || action == 0x32)) &&
            info != 0xFFFFFFFF)
            for (const CharAnims& c : g_charAnims)
                if (c.charId == static_cast<int>((info >> 8) & 0xFFFF)) {
                    const int applied = c.applied.load();
                    const int row = applied > 0 ? kAnimSets[applied - 1].meleeSet : -1;
                    if (row >= 4 && row <= 7) return action == 0x31 ? 0x17 : 0x18;
                    break;
                }
    }
    if (!PlayerWantsReunion(player)) return g_moveIndex(player);
    const uintptr_t actionAt = reinterpret_cast<uintptr_t>(player) + 0x10E0;
    uint32_t action = 0;
    mem::Read(actionAt, action);
    const uint32_t variant = action == 0x31 ? 0xAC : action == 0x32 ? 0xAD : action;  // what mode 5 requests
    if (variant != action) mem::Write(actionAt, variant);
    const int move = g_moveIndex(player);  // native: variant index + duo redirection (ReunionCheck below)
    if (variant != action) mem::Write(actionAt, action);
    return move;
}

// ReunionCheck(player), thiscall on the player manager, ret 4. Outside mode 5, for Reunion players: the native
// duo test without the mode condition.
using ReunionCheckFn = bool(__fastcall*)(void* mgr, void* edx, void* player);
ReunionCheckFn g_reunionCheck = nullptr;
bool g_reunionCheckHooked = false;

uint32_t CharacterInfo(void* actor) {  // (character << 8) | costume, from [vtable+0x10]() + 0x20
    using InfoFn = void*(__fastcall*)(void* self, void* edx);
    uintptr_t vtable = 0, fn = 0;
    if (!actor || !mem::Read(reinterpret_cast<uintptr_t>(actor), vtable) || !mem::Read(vtable + 0x10, fn) || !fn) return 0xFFFFFFFF;
    void* info = reinterpret_cast<InfoFn>(fn)(actor, nullptr);
    uint32_t value = 0xFFFFFFFF;
    if (info) mem::Read(reinterpret_cast<uintptr_t>(info) + 0x20, value);
    return value;
}

bool DuoCheck(void* mgr, void* player) {
    using IsPlayerFn = bool(__fastcall*)(void* mgr, void* edx, void* actor);
    using LinkedFn = void*(__fastcall*)(void* player, void* edx, int which);
    auto isPlayer = reinterpret_cast<IsPlayerFn>(mem::Rva(0x3E6370));
    if (!isPlayer(mgr, nullptr, player)) return false;
    void* other = reinterpret_cast<LinkedFn>(mem::Rva(0x75BED0))(player, nullptr, 1);
    if (!other || !isPlayer(mgr, nullptr, other)) return false;
    if (g_duoAnyPartner.load()) return true;  // option: duo moves with whoever the partner is
    const uint32_t a = CharacterInfo(player), b = CharacterInfo(other);
    if (a == 0xFFFFFFFF || b == 0xFFFFFFFF) return false;
    const uint32_t ac = (a >> 8) & 0xFFFF, aco = a & 0xFF, bc = (b >> 8) & 0xFFFF, bco = b & 0xFF;
    auto fits = [](uint32_t c, uint32_t co, uint32_t wc, uint32_t wco) { return c == wc && (wco == 0xFF || co == wco); };
    for (uintptr_t e = reinterpret_cast<uintptr_t>(mem::Rva(0xCE9DB0));; e += 16) {
        uint32_t row[4] = {};
        if (!mem::SafeRead(reinterpret_cast<void*>(e), row, sizeof(row)) || row[0] == 0xFFFF) return false;
        if ((fits(ac, aco, row[0], row[1]) && fits(bc, bco, row[2], row[3])) ||
            (fits(ac, aco, row[2], row[3]) && fits(bc, bco, row[0], row[1])))
            return true;
    }
}

// Duo moves are disabled for now (user, 2026-10-13): outside Mercenaries Reunion their animation plays but
// never hits (no enemy damage; the costume archives only differ by the motion list and the sound bank name,
// so the hit is gated somewhere in code). To revisit: DuoCheck() below gives the native duo test.
constexpr bool kDuoMoves = false;

bool __fastcall HookReunionCheck(void* mgr, void* edx, void* player) {
    if (PlayerWantsReunion(player)) return kDuoMoves && DuoCheck(mgr, player);
    return g_reunionCheck(mgr, edx, player);
}

bool EnsureReunionCheckHook() {
    if (g_reunionCheckHooked) return true;
    uint8_t* target = mem::Rva(0x3E6820);
    const uint8_t expected[] = { 0xA1, 0xA4, 0x40, 0x23, 0x01, 0x83, 0xEC, 0x0C, 0x53 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("reunion check hook: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, &HookReunionCheck, reinterpret_cast<void**>(&g_reunionCheck));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    g_reunionCheckHooked = true;
    LOG("reunion check hook installed at %p", target);
    return true;
}

bool EnsureMoveIndexHook() {
    if (g_moveIndexHooked) return true;
    uint8_t* target = mem::Rva(0x877D20);
    const uint8_t expected[] = { 0x57, 0x8B, 0x7C, 0x24, 0x08, 0x85, 0xFF, 0x75, 0x04 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("move index hook: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, &HookMoveIndex, reinterpret_cast<void**>(&g_moveIndex));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    g_moveIndexHooked = true;
    LOG("move index hook installed at %p", target);
    return true;
}

bool EnsureMeleeSetHook() {
    if (g_meleeSetHooked) return true;
    uint8_t* target = mem::Rva(0x3E6030);
    const uint8_t expected[] = { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55, 0x8B, 0x2D, 0xA4, 0x40, 0x23, 0x01 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("melee set hook: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS s = MH_CreateHook(target, &HookMeleeSetFor, reinterpret_cast<void**>(&g_meleeSetFor));
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) return false;
    s = MH_EnableHook(target);
    if (s != MH_OK && s != MH_ERROR_ENABLED) return false;
    g_meleeSetHooked = true;
    LOG("melee set hook installed at %p", target);
    return true;
}

// Moves spawn their effects by number from the player's own folder: with Barry's moves, Chris asked for
// "effect\efl\pl\pl00\pl002a", which does not exist -> loose-file load -> crash in the allocator
// (2026-10-13; Barry's is "pl1c\pl1c2a"). sResource::Load is hooked: a missing effect of a character playing
// another one's moveset is taken from that character's folder, when that one is loaded.
ResLoadFn g_resLoad = nullptr;
bool g_resLoadHooked = false;

// An effect of player 1's own folder `from` that does not exist taken from the folder `to` (nullptr: none of that).
// `lenderFirst`: the folder `to`'s whenever it has it (a duo move plays the lender's motion: user, 2026-11-06, Rebecca's
// fire spray had no flame - an effect number Chris also has may have stayed his).
void* RedirectEffect(void* mgr, void* edx, void* type, const char* path, unsigned flags, const char* from, const char* to,
                     const char* who, bool lenderFirst = false) {
    char folder[16];
    sprintf_s(folder, "\\%s\\", from);
    if (std::strcmp(from, to) == 0 || !std::strstr(path, folder)) return nullptr;
    auto release = reinterpret_cast<ResReleaseFn>(mem::Rva(0x4F3E0));
    if (void* own = lenderFirst ? nullptr : g_resLoad(mgr, edx, type, path, flags | kLookupOnly)) {  // it exists: nothing to do
        release(own, nullptr);
        return nullptr;
    }
    std::string alt(path);
    for (size_t at = alt.find(from); at != std::string::npos; at = alt.find(from, at + 4)) alt.replace(at, 4, to);
    void* other = g_resLoad(mgr, edx, type, alt.c_str(), flags | kLookupOnly);
    if (!other) return nullptr;
    release(other, nullptr);
    static std::atomic<int> logged{ 0 };
    if (logged.fetch_add(1) < 30) LOG("%s: effect %s -> %s", who, path, alt.c_str());
    return g_resLoad(mgr, edx, type, alt.c_str(), flags);
}

void* __fastcall HookResLoad(void* mgr, void* edx, void* type, const char* path, unsigned flags) {
    if (!(flags & kLookupOnly) && path && std::strncmp(path, "effect\\", 7) == 0) {
        // A borrowed move: the lender's effects (Rebecca's sprays, Barry's shot...).
        if (g_loan.active.load(std::memory_order_relaxed)) {
            const int set = g_loan.set.load(), character = g_loan.character.load();
            const bool duo = g_voiceAllShared.load(std::memory_order_relaxed);  // a trainer move's loan (a duo move)
            static std::atomic<int> logged{ 0 };
            // Research (2026-11-06, Rebecca's fire spray: which list her FIRE SPRAY asks): every loan's effect requests but
            // the stage's.
            if (!std::strstr(path, "\\stage\\") && logged.fetch_add(1) < 120)
                LOG("borrowed moveset: effect requested %s (%s)", path, duo ? "duo move" : "borrowed move");
            for (const CharAnims& c : g_charAnims)
                if (c.charId == character && set >= 0) {
                    // The lender's list first whenever it has it (user, 2026-11-06: Rebecca's FIRE SPRAY asks pl0014,
                    // which Chris has too - his played, no flame): the motion and the move are the lender's.
                    if (void* res = RedirectEffect(mgr, edx, type, path, flags, c.code, kLoanSets[set].code,
                                                   "borrowed moveset", true))
                        return res;
                    break;
                }
        }
        for (const CharAnims& c : g_charAnims) {
            const int applied = c.applied.load();
            if (applied <= 0) continue;
            if (void* res = RedirectEffect(mgr, edx, type, path, flags, c.code, kAnimSets[applied - 1].code, c.id)) return res;
            break;
        }
    }
    return g_resLoad(mgr, edx, type, path, flags);
}

bool EnsureResLoadHook() {
    if (g_resLoadHooked) return true;
    uint8_t* target = mem::Rva(0x48B70);
    const uint8_t expected[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x0C, 0x8B, 0x45, 0x08 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("resource load hook: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, &HookResLoad, reinterpret_cast<void**>(&g_resLoad));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    g_resLoadHooked = true;
    LOG("resource load hook installed at %p", target);
    return true;
}

// ---- Moveset motion remap ------------------------------------------------------------------------
// Each character's code plays its melee moves with fixed motion numbers (layer 0), so Chris with Barry's
// motion lists played the wrong animation for some moves (Head Flash -> other move, Arm Front -> crash).
// The slots recorded by hand for Chris / Wesker / Barry (motions.log, 2026-10-13) were the melee table's: read there now.

// A character playing another's moveset ("Animations de X", live or loaded): his class plays its own motion slots (his
// row's), the set's row has the other's (the melee table, as the borrowed moves; user, 2026-10-27: no more only Chris /
// Wesker / Barry recorded). A Reunion set's Leg Front / Head Flash are its moves 0x17 / 0x18 (as HookMoveIndex).
uint32_t RemapMotion(uintptr_t player, uint32_t layer, uint32_t motion) {
    if (layer != 0 || !g_movesetActive.load(std::memory_order_relaxed) || (motion & 0xFF00) != 0x100) return motion;
    uint32_t action = 0;
    const uint32_t info = CharacterInfo(reinterpret_cast<void*>(player));
    if (info == 0xFFFFFFFF || !mem::Read(player + 0x10E0, action)) return motion;
    const int character = static_cast<int>((info >> 8) & 0xFFFF);
    for (const CharAnims& c : g_charAnims) {
        if (c.charId != character) continue;
        const int applied = c.applied.load();
        const int move = MoveOfAction(action);
        if (applied <= 0 || move < 0 || !g_meleeSetFor) return motion;
        const int row = kAnimSets[applied - 1].meleeSet;
        const int ownRow = g_meleeSetFor(character, static_cast<int>(info & 0xFF));
        const int setMove = row >= 4 && row <= 7 && (action == 0x31 || action == 0x32) ? (action == 0x31 ? 0x17 : 0x18) : move;
        uint32_t own = 0, ownSecond = 0, other = 0, otherSecond = 0;
        MeleeEntry(ownRow, move, own, ownSecond);
        MeleeEntry(row, setMove, other, otherSecond);
        const uint32_t slot = motion & 0xFF;
        if (slot == own && other) return 0x100 | other;
        if (ownSecond && slot == ownSecond && otherSecond) return 0x100 | otherSecond;
        return motion;
    }
    return motion;
}

// Actor SetMotion, RVA 0x44CFF0 (thiscall(layer, motion, 4 more args), ret 0x18): remap + motion log.
using SetMotionFn = uint32_t(__fastcall*)(void* self, void* edx, uint32_t layer, uint32_t motion, uint32_t a, uint32_t b,
                                          uint32_t c, uint32_t d);
SetMotionFn g_setMotion = nullptr;
bool g_setMotionHooked = false;

uint32_t __fastcall HookSetMotion(void* self, void* edx, uint32_t layer, uint32_t motion, uint32_t a, uint32_t b,
                                  uint32_t c, uint32_t d) {
    if (MotionLogEnabled()) {
        LogMotion(self, layer, motion, reinterpret_cast<uintptr_t>(_ReturnAddress()));
        // Research (move while aiming): the other arguments the game passes, player 1, first 80 calls.
        static int lines = 0;
        if (reinterpret_cast<uintptr_t>(self) == actors::Player(0) && lines < 80) {
            ++lines;
            float fa = 0, fb = 0, fc = 0;
            std::memcpy(&fa, &a, 4);
            std::memcpy(&fb, &b, 4);
            std::memcpy(&fc, &c, 4);
            LOG("SetMotion P1 layer %u motion %X start %.2f blend %.2f speed %.2f mode %X caller RVA %06X", layer, motion,
                fa, fb, fc, d,
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(mem::Base())));
        }
    }
    if (layer == 0) NoteMotionStart(reinterpret_cast<uintptr_t>(self));         // time_speed.cpp
    LogBossMotion(self, layer, motion);                                           // action_log.cpp
    motion = Re6SprintRemap(reinterpret_cast<uintptr_t>(self), layer, motion);    // re6_motions.cpp
    {
        const uint32_t asked = motion;
        motion = Re6GroundMotion(reinterpret_cast<uintptr_t>(self), layer, motion, b);  // wesker_moves.cpp
        // The game's own request on the leg layer under RE6's ground keeps the game's mode: a left-handed player's legs played right-handed
        // under the aim (user, 2026-11-10: "with the aim held the legs stay right-handed"; a tap's slide was right). Its mirror bit from his
        // handedness byte P+0x1AF9.
        if (layer == 7 && motion != asked && reinterpret_cast<uintptr_t>(self) == actors::Player(0)) {
            uint8_t hand = 0;
            mem::Read(reinterpret_cast<uintptr_t>(self) + 0x1AF9, hand);
            d = (d & ~0x100u) | (hand == 1 ? 0x100u : 0u);
        }
    }
    motion = LowReadyLayerMotion(reinterpret_cast<uintptr_t>(self), layer, motion, b, c, d);  // movement.cpp: the low ready / shot kept
    if (motion == 0xFFFFFFFFu) return 1;  // its layer left alone: the game's request not made
    motion = WeskerMoveMotion(reinterpret_cast<uintptr_t>(self), layer, motion);  // wesker_moves.cpp
    motion = ObstacleClimbMotion(reinterpret_cast<uintptr_t>(self), layer, motion);  // obstacle_climb.cpp
    motion = JillDownMotion(reinterpret_cast<uintptr_t>(self), layer, motion);    // wesker_boss.cpp
    motion = InvisibleHolderMotion(reinterpret_cast<uintptr_t>(self), layer, motion);  // action_log.cpp
    ForcedDeathMotion(reinterpret_cast<uintptr_t>(self), layer, motion, a, b);     // forced_death.cpp
    JillVictimMotion(reinterpret_cast<uintptr_t>(self), layer, motion, a, b);      // wesker_moves.cpp
    PairCorpseMotion(reinterpret_cast<uintptr_t>(self), layer, motion, a, b);      // wesker_moves.cpp
    PairLyingRealign(reinterpret_cast<uintptr_t>(self), layer, motion, a);         // wesker_moves.cpp
    GroundRollStart(reinterpret_cast<uintptr_t>(self), layer, motion, a, b);       // wesker_moves.cpp
    MoveBeginStart(reinterpret_cast<uintptr_t>(self), layer, b);                  // wesker_moves.cpp
    motion = LoanMotion(reinterpret_cast<uintptr_t>(self), layer, motion);  // a borrowed move: the lender's slot
    return g_setMotion(self, edx, layer, RemapMotion(reinterpret_cast<uintptr_t>(self), layer, motion), a, b, c, d);
}

bool EnsureLoadListHook() {
    if (g_loadListHooked) return true;
    uint8_t* target = mem::Rva(0x7C2810);
    const uint8_t expected[] = { 0x53, 0x55, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x33, 0xDB };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("anim swap: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    for (const CharAnims& c : g_charAnims) {
        auto* s = reinterpret_cast<const char*>(mem::Rva(c.motionRva));
        if (!mem::IsReadable(s, 32) || std::memcmp(s, c.defaultMotion, std::strlen(c.defaultMotion)) != 0) {
            LOG("anim swap: %s path string changed (another trainer?)", c.id);
            return false;
        }
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS s = MH_CreateHook(target, &HookLoadList, reinterpret_cast<void**>(&g_loadList));
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) return false;
    s = MH_EnableHook(target);
    if (s != MH_OK && s != MH_ERROR_ENABLED) return false;
    g_loadListHooked = true;
    LOG("anim swap hook installed at %p", target);
    return true;
}

bool InLevel() { return actors::Player(0) != 0; }

// A character playing motions of the other family needs the matching weapon-animation flag (P+0x1AF8,
// 1 = female set): the game picks weapon / damage motion numbers with it.
uintptr_t ProfileForAnims() {
    uintptr_t p = 0;
    mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), p);
    return p;
}

void AnimFamilyFlags() {
    uintptr_t profile = ProfileForAnims();
    if (!profile) return;
    for (int player = 0; player < 2; ++player) {
        uintptr_t p = actors::Slot(player);
        uint32_t ch = 0xFFFFFFFF;
        if (!p || !mem::Read(profile + 0x23318 + 0x50 * player, ch)) continue;
        for (const CharAnims& c : g_charAnims) {
            if (static_cast<uint32_t>(c.charId) != ch) continue;
            const int applied = c.applied.load();
            if (applied > 0 && kAnimSets[applied - 1].female != c.female)
                mem::Write<uint8_t>(p + 0x1AF8, kAnimSets[applied - 1].female ? 1 : 0);
            break;
        }
    }
}


// ---- Character / costume swap, players 1 and 2 (taken into account at the next load) -----------
// Profile object [re5dx9+0xDB2158]: one block per player, 0x50 bytes apart: P1 character +0x23318,
// costume +0x2331C (mirror +0x248A8/+0x248AC); P2 +0x23368/+0x2336C (mirror +0x248F8/+0x248FC); the
// blocks also hold health/max (+8/+0xC) and the checkpoint position.
// The Ultimate Trainer's "Player N Freeze" also applies the code patches below while a swap is active.
const char* kCharNames[] = { "Chris", "Sheva", "Jill", "Wesker", "Josh", "Excella", "Barry", "Rebecca", "Irving" };
const uint32_t kCharIds[] = { 0, 1, 2, 3, 4, 5, 6, 7, 0x86 };
constexpr uintptr_t kCharSelect = 0x23318, kCharSelectMirror = 0x248A8, kPlayerBlock = 0x50;

uintptr_t Profile() {
    uintptr_t p = 0;
    mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), p);
    return p;
}

struct CharSwap {
    int character = 0;  // choice index, 0 = no swap
    int costume = 0;
    bool saved = false;
    bool pendingRestore = false;  // player 2 set back to default inside a level
    uint32_t origChar = 0, origCostume = 0;
    // One set of patch objects per player: mem::Patch counts the users of each site.
    mem::Patch p1{ "charswap.1", 0x20E1FF, { 0x75, 0x04 }, { 0xEB, 0x04 } };
    mem::Patch p2{ "charswap.2", 0x20DF27, { 0x75, 0x04 }, { 0xEB, 0x04 } };
    mem::Patch p3{ "charswap.3", 0x32A8EB, { 0x89, 0x57, 0x0C }, { 0x90, 0x90, 0x90 } };
    mem::Patch p4{ "charswap.4", 0x892B8C, { 0x14, 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00, 0x00 } };
} g_charSwap[2];

// The Ultimate Trainer's default-tab fixes, applied when it attaches (tools/ut_attach_diff.py: the only 3
// attach writes that differ from the game, 2026-10-13). A Wesker partner only worked with UT attached:
// one of them is its "multiplayer model swap fix". 0x869287: cmp [player+0x2DA8] (1 = AI partner), 1 -> 0.
mem::Patch g_utFix1("ut.default_fix1", 0x869287, { 0x01 }, { 0x00 });
mem::Patch g_utFix2("ut.default_fix2", 0x77000A, { 0x89 }, { 0x01 });
mem::Patch g_utFix3("ut.default_fix3", 0x00ED09, { 0x02 }, { 0x03 });

// final_fight.cpp's "Combat final : Jill en partenaire": player 2 imposed as Jill (her first costume) during the
// volcano's loads, whatever "Personnage du joueur 2", with its own patch objects (mem::Patch counts each site's users);
// the partner of before (the trainer's choice, else the profile's) put back at the next load elsewhere.
struct ForcedPartner {
    bool on = false;
    uint32_t character = 1, costume = 0;  // the profile's partner before
    CharSwap patches;                      // its p1..p4
    mem::Patch fix1{ "ut.default_fix1", 0x869287, { 0x01 }, { 0x00 } };
    mem::Patch fix2{ "ut.default_fix2", 0x77000A, { 0x89 }, { 0x01 } };
    mem::Patch fix3{ "ut.default_fix3", 0x00ED09, { 0x02 }, { 0x03 } };
} g_forcedPartner;
constexpr uint32_t kJillCharacter = 2;
// Her costume (user, 2026-11-02: "the Battle Suit, not the S.T.A.R.S. one"): the game registers costume n as archive
// uPl02JillCos(n + 1) (codes 0x01000200 + n pushed with each name): Cos1 / Cos4 hold the pl02 model (her BSAA uniform and
// beret, the one seen), Cos2 pl14 (the Battle Suit), Cos3 pl17 (Lost in Nightmares).
constexpr uint32_t kJillBattleSuit = 1;

uintptr_t CharField(int player, bool mirror, bool costume) {
    return (mirror ? kCharSelectMirror : kCharSelect) + kPlayerBlock * player + (costume ? 4 : 0);
}

// Player 2 (partner): its profile values are only written while no player exists (main menu, or the load
// of a checkpoint restart), never in the middle of a level. Verified: Wesker as partner after a checkpoint
// restart (2026-10-13). The crashes first blamed on the partner swap came from the camera sliders writing
// into the destroyed camera during loads (fixed in camera.cpp).
bool CanWriteProfile(int player) { return player == 0 || !InLevel(); }

// Third copy of the player blocks (+0x274E8 / +0x27538): the checkpoint data. A checkpoint restart copies it
// back over the two others, so a character imposed long enough to be saved there came back after "Par
// défaut" (Barry, 2026-10-13). All three copies are written.
constexpr uintptr_t kCharSelectCheckpoint = 0x274E8;

void WriteCharSelect(int player, uint32_t ch, uint32_t co) {
    uintptr_t profile = Profile();
    if (!profile) return;
    for (uintptr_t base : { kCharSelect, kCharSelectMirror, kCharSelectCheckpoint }) {
        uint32_t v = 0;
        const uintptr_t chAt = profile + base + kPlayerBlock * player, coAt = chAt + 4;
        if (mem::Read(chAt, v) && v != ch) mem::Write(chAt, ch);
        if (mem::Read(coAt, v) && v != co) mem::Write(coAt, co);
    }
}

void FreezeCharSwap(int player);

bool ApplyCharSwap(int player) {
    CharSwap& cs = g_charSwap[player];
    uintptr_t profile = Profile();
    if (!profile) return false;
    const bool active = cs.character != 0 || cs.costume != 0;
    if (active && !cs.saved) {
        if (!cs.pendingRestore) {  // a pending restore means the profile still holds our values
            mem::Read(profile + CharField(player, false, false), cs.origChar);
            mem::Read(profile + CharField(player, false, true), cs.origCostume);
            // After a trainer reload the profile already holds the imposed character: fall back to the
            // story defaults (Chris / Sheva, first costume) so that "Par défaut" still means something.
            if (cs.character && cs.origChar == kCharIds[cs.character - 1]) cs.origChar = player == 0 ? 0 : 1;
            if (cs.costume && cs.origCostume == static_cast<uint32_t>(cs.costume - 1)) cs.origCostume = 0;
        }
        cs.saved = true;
        cs.pendingRestore = false;
    }
    if (!active) {
        if (cs.saved) {  // put the original character back (UT left it stuck sometimes)
            if (CanWriteProfile(player)) WriteCharSelect(player, cs.origChar, cs.origCostume);
            else cs.pendingRestore = true;
            cs.saved = false;
        }
        if (player == 1) mem::RestoreAll({ &g_utFix3, &g_utFix2, &g_utFix1 });
        return mem::RestoreAll({ &cs.p4, &cs.p3, &cs.p2, &cs.p1 });
    }
    if (!mem::ApplyAll({ &cs.p1, &cs.p2, &cs.p3, &cs.p4 })) return false;
    if (player == 1 && !mem::ApplyAll({ &g_utFix1, &g_utFix2, &g_utFix3 })) return false;
    FreezeCharSwap(player);
    return true;
}

// "Freeze": the game writes its own character/costume back when a level loads, so the values are imposed
// every frame while a swap is active (like the Ultimate Trainer's "Player N Freeze").
void FreezeCharSwap(int player) {
    const CharSwap& cs = g_charSwap[player];
    if (!cs.saved || !CanWriteProfile(player) || (player == 1 && g_forcedPartner.on)) return;
    WriteCharSelect(player, cs.character ? kCharIds[cs.character - 1] : cs.origChar,
                    cs.costume ? static_cast<uint32_t>(cs.costume - 1) : cs.origCostume);
}

// Every frame: Jill imposed while final_fight.cpp asks (written only while no player exists, as FreezeCharSwap).
void ForcedPartnerTick() {
    ForcedPartner& f = g_forcedPartner;
    if (FinalFightJill()) {
        if (!f.on) {
            static DWORD tried = 0;  // a refused patch set tried again every 5 s, not every frame
            const uintptr_t profile = Profile();
            if (!profile || GetTickCount() - tried < 5000) return;
            tried = GetTickCount();
            if (!mem::ApplyAll({ &f.patches.p1, &f.patches.p2, &f.patches.p3, &f.patches.p4, &f.fix1, &f.fix2, &f.fix3 }))
                return;
            static bool restoreRegistered = false;
            if (!restoreRegistered) {  // the trainer unloaded in the volcano: the game's bytes given back
                restoreRegistered = true;
                OnRestore([] {
                    ForcedPartner& fp = g_forcedPartner;
                    mem::RestoreAll({ &fp.fix3, &fp.fix2, &fp.fix1, &fp.patches.p4, &fp.patches.p3, &fp.patches.p2, &fp.patches.p1 });
                });
            }
            mem::Read(profile + CharField(1, false, false), f.character);
            mem::Read(profile + CharField(1, false, true), f.costume);
            if (f.character == kJillCharacter) f.character = 1, f.costume = 0;  // a trainer reload in the volcano: Sheva
            f.on = true;
            LOG("final fight: player 2 is Jill (Battle Suit) at the volcano's loads (partner before: character %u, costume %u)", f.character,
                f.costume);
        }
        if (CanWriteProfile(1)) WriteCharSelect(1, kJillCharacter, kJillBattleSuit);
        return;
    }
    if (!f.on || !CanWriteProfile(1)) return;  // put back at the next load
    f.on = false;
    if (g_charSwap[1].saved) FreezeCharSwap(1);  // the trainer's partner
    else WriteCharSelect(1, f.character, f.costume);
    mem::RestoreAll({ &f.fix3, &f.fix2, &f.fix1, &f.patches.p4, &f.patches.p3, &f.patches.p2, &f.patches.p1 });
    LOG("final fight: player 2 given back (%s)", g_charSwap[1].saved ? "the trainer's partner" : "the partner before");
}

// A partner restored to default inside a level is written back once back at the menu.
void FinishPendingRestores() {
    for (int player = 0; player < 2; ++player) {
        CharSwap& cs = g_charSwap[player];
        if (!cs.pendingRestore || !CanWriteProfile(player)) continue;
        WriteCharSelect(player, cs.origChar, cs.origCostume);
        cs.pendingRestore = false;
    }
}

// Every frame: sample the characters once their level has loaded; the Reunion rules (hooks above) are on
// while a Reunion character / costume is loaded.
void UpdateReunionAuto() {
    const bool inLevel = InLevel();
    uintptr_t profile = Profile();
    for (int player = 0; player < 2; ++player) {
        if (!inLevel) {  // forget them during loads and menus
            g_loadedChar[player] = 0xFF;
            g_loadedCostume[player] = 0;
        } else if (g_loadedChar[player] == 0xFF && profile && actors::Slot(player)) {  // once per level and player
            mem::Read(profile + kCharSelect + kPlayerBlock * player, g_loadedChar[player]);
            mem::Read(profile + kCharSelect + kPlayerBlock * player + 4, g_loadedCostume[player]);
        }
    }
    bool need = false;
    for (int player = 0; player < 2 && inLevel; ++player) need = need || IsReunionCharacter(g_loadedChar[player]);
    if (need == g_reunionAutoActive.load()) return;
    if (need && (!EnsureMeleeSetHook() || !EnsureMoveIndexHook() || !EnsureReunionCheckHook())) return;
    g_reunionAutoActive = need;
    LOG("reunion moves: automatic %s (loaded %u/%u, %u/%u)", need ? "on" : "off", g_loadedChar[0], g_loadedCostume[0],
        g_loadedChar[1], g_loadedCostume[1]);
}

mem::Patch* g_reunion[4] = {};

// Costume names per character, in costume-index order (archive table VA 0x12033B0: Chris Normal, ArmorA,
// ArmorB, ArmorF, Plain, Cos1, Cos2, Cos3, Ep1, Cos4; Sheva Normal .. Plain, Cos1..Cos4; Jill Cos1..4...).
// Chris's names are the Ultimate Trainer's; the others follow the archives and the Mercenaries Reunion
// lineup (VA 0xFACAD8) - to confirm in game.
std::vector<std::string> CostumeList(uint32_t character) {
    static const std::vector<std::vector<const char*>> kNames = {
        { "BSAA", "Melee", "Bulletproof", "Full Armor", "Plain", "Safari", "S.T.A.R.S.", "Warrior (Reunion)", "Flashback",
          "Heavy Metal (Reunion)" },
        { "BSAA", "Melee", "Bulletproof", "Full Armor", "Plain", "Clubbin'", "Tribal", "Business (Reunion)",
          "Fairy Tale (Reunion)" },
        { "BSAA", "Battle Suit", "Lost in Nightmares", "BSAA (Reunion)" },  // Cos1 pl02, Cos2 pl14, Cos3 pl17, Cos4 pl02
        { "Midnight", "S.T.A.R.S." },
        { "Josh (histoire)", "Josh (Reunion)" },
        { "Excella" },
        { "Barry" },
        { "Rebecca" },
    };
    std::vector<std::string> out = { "Par défaut" };
    if (character < kNames.size())
        for (const char* n : kNames[character]) out.push_back(n);
    else
        out.push_back("Costume 1");
    return out;
}

Feature* g_costumeFeature[2] = {};
uint32_t g_costumeListFor[2] = { 0xFFFFFFFF, 0xFFFFFFFF };

// The costume lists show the names of the character chosen for that player (or the current one).
void UpdateCostumeLists() {
    uintptr_t profile = Profile();
    for (int player = 0; player < 2; ++player) {
        Feature* f = g_costumeFeature[player];
        if (!f) continue;
        uint32_t ch = 0xFFFFFFFF;
        const int chosen = g_charSwap[player].character;
        if (chosen) ch = kCharIds[chosen - 1];
        else if (profile) mem::Read(profile + CharField(player, false, false), ch);
        if (ch == g_costumeListFor[player]) continue;
        g_costumeListFor[player] = ch;
        std::vector<std::string> list = CostumeList(ch);
        while (static_cast<int>(list.size()) <= static_cast<int>(f->value))  // keep the selected index valid
            list.push_back("Costume " + std::to_string(list.size()));
        f->choices = std::move(list);
        f->valueMax = static_cast<float>(f->choices.size() - 1);
    }
}
}

bool EnsureSetMotionHook() {
    if (g_setMotionHooked) return true;
    uint8_t* target = mem::Rva(0x44CFF0);
    const uint8_t expected[] = { 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x74, 0x19, 0x00, 0x00 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("set motion hook: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, &HookSetMotion, reinterpret_cast<void**>(&g_setMotion));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    g_setMotionHooked = true;
    LOG("set motion hook installed at %p", target);
    return true;
}

uint32_t ActorCharacterInfo(void* actor) { return CharacterInfo(actor); }

// Leg Front / Head Flash as a Reunion character (Josh, Excella, Barry, Rebecca) requests them in Mercenaries Reunion:
// 0xAC / 0xAD (moves 0x17 / 0x18; user, 2026-10-27: Barry's head flash after a front escape played his tornado, move 2,
// instead of his headbutt; Excella's leg front her slap grasp). Other characters and moves: the action itself.
uint8_t ReunionVariantOf(uintptr_t p, uint8_t action) {
    if (action != 0x31 && action != 0x32) return action;
    const uint32_t info = CharacterInfo(reinterpret_cast<void*>(p));
    const uint32_t character = (info >> 8) & 0xFFFF;
    if (info == 0xFFFFFFFF || character < 4 || character > 7) return action;
    return action == 0x31 ? 0xAC : 0xAD;
}

int ReunionVariant(uintptr_t p, int action) {
    return action >= 0 && action <= 0xFF ? ReunionVariantOf(p, static_cast<uint8_t>(action)) : action;
}
// player.cpp ("Arme toujours visible"): a melee move in which the character takes a tool in the hand, its weapon left
// hidden by the game (user, 2026-10-27): Rebecca's sprays (Arm Front TEAR GAS SPRAY, Head Flash FIRE SPRAY, also as
// her Reunion 0xAD) and stun gun (both finishers), Barry's magnum (Ground: GUN SHOT, its butt) - borrowed by player 1
// or his own character's.
bool ToolMove(int character, uint32_t action) {
    if (character == 7) return action == 0x30 || action == 0x32 || action == 0xAD || action == 0x34 || action == 0x35;
    if (character == 6) return action == 0x38;
    return false;
}

bool BorrowedMoveHidesWeapon() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action)) return false;
    if (g_loan.active.load(std::memory_order_relaxed) && p == g_loan.player) {
        const int set = g_loan.set.load();
        return set >= 0 && action == g_loan.action.load() && ToolMove(kLoanSets[set].row, action);  // rows 6 / 7 = ids
    }
    const uint32_t info = CharacterInfo(reinterpret_cast<void*>(p));
    return info != 0xFFFFFFFF && ToolMove(static_cast<int>((info >> 8) & 0xFFFF), action);
}

bool BorrowFor(uintptr_t p, int set, uint32_t action, bool lendList) {
    return EnsureMeleeSetHook() && EnsureMoveIndexHook() && EnsureResLoadHook() && Borrow(p, set, action, lendList);
}

// ---- Free melee (user, 2026-10-15): a hotkey per melee move of the swap list, played at once by player 1 (its
// RequestAction, vtable +0xDC: (action, 0, 0, -1, 0) like the game). Melee moves align the player on the melee target
// P+0x2DA4 / point P+0x2E10 / angle P+0x2E20: AimMelee, the nearest living enemy within 3 m, otherwise the virtual
// enemy at arm's reach in front (no target), so the move is played on the spot - no teleport.
struct FreeMove {
    const char* id;
    const char* name;
    uint8_t action;
};
// No "Finisher 2" (attack_finish_2 0x36, move 6; user, 2026-10-27: "the same as Finisher Back?"): its entry of the
// melee table (VA 0x10FE430, 0xA0 each, row * 0x1B + move) is Finisher Front's byte for byte but the name (Chris:
// "FINISH BLOW 2" = "FINISH BLOW", motion 0x0F), Finisher Back's for Josh ("DROP KICK 2" = "DROP KICK"); the three
// actions share every function of their class (init VA 0xC8DA00).
const FreeMove kFreeMoves[] = {
    { "melee.free_arm_front", "Arm Front", 0x30 },     { "melee.free_leg_front", "Leg Front", 0x31 },
    { "melee.free_head_flash", "Head Flash", 0x32 },   { "melee.free_arm_back", "Arm Back", 0x33 },
    { "melee.free_finisher_back", "Finisher Back", 0x34 }, { "melee.free_finisher_front", "Finisher Front", 0x35 },
    { "melee.free_help", "Help", 0x37 },
    { "melee.free_ground", "Ground", 0x38 },           { "melee.free_leg_back", "Leg Back", 0x39 },
    { "melee.free_knife_slash2", "Knife Slash2", 0x3A }, { "melee.free_stab", "Stab", 0x3B },
    { "melee.free_licker_tongue", "Knife Licker Tongue", 0x3C }, { "melee.free_licker_counter", "Licker Finisher Counter", 0x3D },
    { "melee.free_knife_vertical", "Knife Vertical", 0x3E },
};

// Player 1, free to act (not dying, held, or in a damage / grab action 0x80 and up); its current action. On foot only:
// Jill's slide played in the boat took Chris off it and locked him (user, 2026-10-28).
uintptr_t FreePlayer1(uint32_t& current, bool ducked = false) {  // ducked: the frozen duck accepted
    if (Re6OnGround() && !(ducked && Re6Ducked())) return 0;
    const uintptr_t p = actors::Player(0);
    int16_t health = 0;
    if (!p || !actors::InGame() || !actors::OnFoot(p) || !mem::Read(p + 0x1364, health) ||
        health < 1 || (health == 1 && !DyingActionsAllowed()) ||
        !mem::Read(p + 0x10E0, current) || current >= 0x80)
        return 0;
    return p;
}

bool RequestPlayer(uintptr_t p, uint8_t action) {
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(p, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return false;
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(p), nullptr, action, 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool RequestFreeMove(uint8_t action) {
    uint32_t current = 0;
    const uintptr_t p = FreePlayer1(current);
    if (!p || !StaminaAllows()) return false;
    AimMelee(p, true);
    const bool asked = RequestPlayer(p, action);
    if (asked) StaminaSpend();
    return asked;
}

uint8_t ReunionVariantOf(uintptr_t p, uint8_t action);  // below

// The game's move as player 1's character plays it: a Reunion character's Leg Front / Head Flash variants.
bool PlayFreeMove(uint8_t action) {
    const uintptr_t p = actors::Player(0);
    return RequestFreeMove(p ? ReunionVariantOf(p, action) : action);
}

// The Reunion characters' moves (user, 2026-10-27: Josh first, then each of them, a sub-tab each): the game's moves with
// their moveset borrowed for it (a Reunion lender's Leg Front / Head Flash its Reunion variants, LenderMove).
struct BorrowedMove {
    const char* key;
    const char* name;
    uint8_t action;
};
const BorrowedMove kBorrowedMoves[] = {
    { "arm_front", "Arm Front", 0x30 }, { "leg_front", "Leg Front", 0x31 }, { "head_flash", "Head Flash", 0x32 },
    { "arm_back", "Arm Back", 0x33 }, { "finisher_back", "Finisher Back", 0x34 }, { "finisher_front", "Finisher Front", 0x35 },
    { "help", "Help", 0x37 }, { "ground", "Ground", 0x38 }, { "leg_back", "Leg Back", 0x39 },
};

bool PlayBorrowedMove(int set, uint8_t action) {
    uint32_t current = 0;
    const uintptr_t p = FreePlayer1(current);
    if (!p || !StaminaAllows() || !BorrowFor(p, set, action)) return false;
    if (RequestFreeMove(action)) return true;  // the loan's own action (its Reunion variant through MoveIndex)
    EndLoan("request refused");
    return false;
}

// "Josh Contre" (user, 2026-11-01), a trainer move of wesker_moves.cpp: his Finisher Front (HEAD CRUSH, a synchronised
// move without attack shape whose victim dies by the game's own finisher) played as a pair, his side 0x0F and the
// victim's 0x1D of his action list, death only from its table line. The list loaded ahead (parry.cpp when "Projection
// de Josh" is chosen) and kept referenced; a motion entry of it (the list in memory: its offsets are addresses).
bool PrepareJoshCounter() {
    if (!g_loanLists[kJosh]) {
        LoadWeaponArchive(kLoanSets[kJosh].effects);
        g_loanLists[kJosh] = LoadMotionList(kLoanSets[kJosh].archive, kLoanSets[kJosh].list);
    }
    return g_loanLists[kJosh] != nullptr;
}

void* JoshActionList() { return PrepareJoshCounter() ? g_loanLists[kJosh] : nullptr; }

uint32_t JoshMotionEntry(uint16_t slot) {
    uintptr_t data = 0;
    uint16_t count = 0;
    uint32_t entry = 0;
    const uintptr_t list = reinterpret_cast<uintptr_t>(JoshActionList());
    if (!list || !mem::Read(list + 0x60, data) || !data || !mem::Read(data + 6, count) || slot >= count ||
        !mem::Read(data + 8 + 4u * slot, entry))
        return 0;
    return entry;
}

// wesker_moves.cpp: another character's duo move played by player 1 as a trainer move (its carrier action): that
// character's moveset lent for the carrier action's length as for a borrowed move - its effects, sounds and model parts,
// its voice muted (user, 2026-11-05: Rebecca's duo move without her spray). Nothing when it is his own character's.
// parry.cpp (PlayerBankName: the trainer's grunts and impacts): player 1's own se bank while another character's is
// lent to him (user, 2026-11-06: Rebecca's voice heard on Chris in her duo move - the grunt read P+0x1454, hers then),
// else 0.
uintptr_t Player1OwnSoundBank() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p || !g_loan.active.load() || g_loan.player != p || !g_loan.soundsLent) return 0;
    return reinterpret_cast<uintptr_t>(g_loan.ownSounds.res[0]);
}

// player.cpp ("Replacer les accessoires"): player 1 plays a move with another character's moveset lent (Wesker's dash
// in bank 1, a borrowed move, a duo move): its motions may drive joints his own never animate (the sheath).
bool Player1MovesetLent() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    return p && g_loan.active.load() && g_loan.player == p;
}

bool LendMovesetForTrainerMove(int set, uint32_t carrier) {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p || set < 0 || set >= static_cast<int>(std::size(kLoanSets))) return false;
    const uint32_t info = CharacterInfo(reinterpret_cast<void*>(p));
    if (info != 0xFFFFFFFF && kLoanSets[set].row == static_cast<int>((info >> 8) & 0xFFFF)) return true;
    return BorrowFor(p, set, carrier, false);  // his own list kept in bank 1: the carrier reads it
}

// ---- The free melee table (melee_tuning.cpp): the settings a game move of player 1 uses. A move is a lender's
// (Chris, Sheva... sub-tabs: melee.free_<lender>_<move>) when its moveset is borrowed, else the played character's own;
// the Général sub-tab's standard moves are the played character's (Chris's row 0 / 8, Sheva's 1 / 9...).
namespace {
const char* StandardKey(uint32_t action) {
    if (action == 0x36) action = 0x35;  // Finisher 2 is Finisher Front
    for (const BorrowedMove& m : kBorrowedMoves)
        if (m.action == action) return m.key;
    return nullptr;
}

int SetOfRow(int row) {
    if (row == 8) row = 0;  // Chris Reunion
    if (row == 9) row = 1;  // Sheva Reunion
    for (int k = 0; k < static_cast<int>(std::size(kLoanSets)); ++k)
        if (kLoanSets[k].row == row) return k;
    return -1;
}

int CallMeleeSet(int character, int costume) {
    const MeleeSetFn original = g_meleeSetFor ? g_meleeSetFor : reinterpret_cast<MeleeSetFn>(mem::Rva(0x3E6030));
    __try {
        return g_meleeSetHooked ? HookMeleeSetFor(character, costume) : original(character, costume);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Player 1's own melee row (his character's, his anim set's), as MeleeSetFor answers without a loan.
int OwnRow(uintptr_t p) {
    if (!p) return 0;
    if (g_loan.active.load() && g_loan.player == p) return g_loan.ownRow.load();
    const uint32_t info = CharacterInfo(reinterpret_cast<void*>(p));
    if (info == 0xFFFFFFFF) return 0;
    return CallMeleeSet(static_cast<int>((info >> 8) & 0xFFFF), static_cast<int>(info & 0xFF));
}

std::string StandardId(int set, const char* key) {
    return set >= 0 && key ? std::string("melee.free_") + kLoanSets[set].key + "_" + key : std::string();
}

uintptr_t Player1InGame() { return actors::InGame() ? actors::Player(0) : 0; }
}  // namespace

std::string Player1NativeMoveId(uintptr_t p, uint32_t action) {
    for (const FreeMove& m : kFreeMoves)
        if (m.action >= 0x3A && m.action == action) return m.id;  // the knife moves: the Général sub-tab's
    if (action == 0xAC) action = 0x31;
    else if (action == 0xAD) action = 0x32;
    const char* key = StandardKey(action);
    if (!key || !p) return {};
    const int set = g_loan.active.load() && g_loan.player == p ? g_loan.set.load() : SetOfRow(OwnRow(p));
    return StandardId(set, key);
}

std::string TuningIdOf(const std::string& featureId) {
    for (const FreeMove& m : kFreeMoves)
        if (m.action <= 0x39 && featureId == m.id) {
            const std::string id = StandardId(SetOfRow(OwnRow(Player1InGame())), StandardKey(m.action));
            return id.empty() ? featureId : id;
        }
    return featureId;
}

// melee_tuning.cpp: a game move's power (user, 2026-10-31: "write the damage the multiplier gives, to set the Head Flash
// as strong as the Finisher Front"): the attack parameter of its melee row (the entry's +0x64, a u16 pushed to
// SetAttackShape at VA 0xC7E085; RVA 0x4237D0: [RVA 0xD15280 + (id >> 16) * 8] + (id & 0xFFFF) * 28, power its dword 1:
// Chris's hook 300, straight 400, finish blow 3000), the hit's damage before the multiplier; -1 if none.
int NativeMovePower(const std::string& featureId) {
    int row = -1, move = -1;
    for (const FreeMove& m : kFreeMoves)
        if (featureId == m.id) {
            if (m.action <= 0x39) return NativeMovePower(TuningIdOf(m.id) == m.id ? std::string() : TuningIdOf(m.id));
            row = OwnRow(Player1InGame());  // the knife moves: the played character's
            move = MoveOfAction(m.action);
        }
    for (int set = 0; set < static_cast<int>(std::size(kLoanSets)) && move < 0; ++set)
        for (const BorrowedMove& m : kBorrowedMoves)
            if (featureId == StandardId(set, m.key)) {
                row = kLoanSets[set].row;
                move = LenderMove(set, m.action);
            }
    if (row < 0 || move < 0) return -1;
    const uintptr_t entry = reinterpret_cast<uintptr_t>(mem::Rva(0xCFE430)) + (static_cast<uintptr_t>(row) * 0x1B + move) * 0xA0;
    uint16_t id = 0;
    uintptr_t table = 0;
    int32_t power = -1;
    if (!mem::Read(entry + 0x64, id) || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xD15280)), table) || !table ||
        !mem::Read(table + id * 28u + 4, power) || power < 0 || power > 100000)
        return -1;
    return power;
}

// wesker_moves.cpp: the played character's melee row (the melee table's, his own even while he borrows a moveset).
int Player1MeleeRow() { return OwnRow(Player1InGame()); }

// ---- Per-character choices (user, 2026-11-01: "a dropdown per character: Wesker dodges as Wesker, Chris as RE5, Sheva
// as Leon...; the same for the parries, with None so that character cannot do it at all"): player 1's character by his
// own melee row (his own during a loan; 8 / 9 the Reunion Chris / Sheva), looked up twice a second at most (the parries
// ask it from their hooks).
constexpr int kCharacterCount = 8;
const char* const kCharacterKeys[kCharacterCount] = { "chris", "sheva", "jill", "wesker", "josh", "excella", "barry", "rebecca" };
const char* const kCharacterNames[kCharacterCount] = { "Chris", "Sheva", "Jill", "Wesker", "Josh", "Excella", "Barry", "Rebecca" };

int Player1CharacterIndex() {
    static std::atomic<uintptr_t> cachedPlayer{ 0 };
    static std::atomic<DWORD> cachedTick{ 0 };
    static std::atomic<int> cached{ 0 };
    const uintptr_t p = Player1InGame();
    const DWORD now = GetTickCount();
    if (p && p == cachedPlayer.load() && now - cachedTick.load() < 500) return cached.load();
    const int row = OwnRow(p);
    const int character = row == 8 ? 0 : row == 9 ? 1 : row >= 0 && row < kCharacterCount ? row : 0;
    cached = character;
    cachedTick = now;
    cachedPlayer = p;
    return character;
}

const char* CharacterName(int character) { return kCharacterNames[std::clamp(character, 0, kCharacterCount - 1)]; }

// The per-character choices as a table (Dodge, Parade): a row per character in the free melee tabs' order (the played
// one marked), a column per family of options (the features <prefix><key>).
void DrawCharacterChoices(const char* table, const std::vector<std::pair<std::string, std::string>>& columns) {
    static constexpr int kOrder[kCharacterCount] = { 0, 1, 2, 3, 4, 6, 7, 5 };
    const float scale = ImGui::GetFontSize() / 13.0f;
    if (!ImGui::BeginTable(table, 1 + static_cast<int>(columns.size()),
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
        return;
    ImGui::TableSetupColumn(i18n::Tr("Personnage"));
    for (const auto& column : columns) ImGui::TableSetupColumn(column.first.c_str());
    ImGui::TableHeadersRow();
    const int played = actors::InGame() ? Player1CharacterIndex() : -1;
    for (const int c : kOrder) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (c == played) ImGui::Text(i18n::Tr("%s (joué)"), kCharacterNames[c]);
        else ImGui::TextUnformatted(kCharacterNames[c]);
        for (size_t k = 0; k < columns.size(); ++k) {
            ImGui::TableSetColumnIndex(static_cast<int>(k) + 1);
            Feature* f = Find(columns[k].second + kCharacterKeys[c]);
            if (!f) continue;
            if (f->kind == Kind::Toggle) {  // a check box ("Aléatoire")
                ImGui::PushID(f->id.c_str());
                bool on = f->enabled;
                if (ImGui::Checkbox("##on", &on)) SetEnabled(*f, on);
                if (!f->help.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f->help.c_str());
                ImGui::PopID();
                continue;
            }
            if (f->choices.empty()) continue;
            int index = static_cast<int>(f->value);
            if (index < 0 || index >= static_cast<int>(f->choices.size())) index = 0;
            ImGui::PushID(f->id.c_str());
            ImGui::SetNextItemWidth(230 * scale);
            if (ImGui::BeginCombo("##choice", f->choices[index].c_str(), ImGuiComboFlags_HeightLarge)) {
                for (int i = 0; i < static_cast<int>(f->choices.size()); ++i) {
                    ImGui::PushID(i);
                    if (ImGui::Selectable(f->choices[i].c_str(), i == index)) Choose(*f, i);
                    ImGui::PopID();
                    if (i == index) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (!f->help.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f->help.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

std::string Player1MovesetName() {
    const int set = SetOfRow(OwnRow(Player1InGame()));
    return set >= 0 ? kLoanSets[set].who : "Chris";
}

namespace {
// "Utiliser les back-up" (melee_tuning.cpp): the shortcut's move refused on the enemy it would meet (the one AimMelee
// takes), its back-up played instead; true with `played` when a back-up answered.
bool ShortcutBackup(const std::string& id, bool& played) {
    const uintptr_t p = Player1InGame();
    float e[3]{};
    const std::string backup = p ? BackupFor(id, NearestEnemy(p, e)) : std::string();
    if (backup.empty()) return false;
    played = RunBackup(backup);
    return true;
}
}  // namespace

// The dodge of the direction held (Player1BodyDirection, relative to his body): rolls forward 0x51 / back 0x52 / left
// 0x53 / right 0x54 by the dominant axis, Quick Duck 0x50 without a direction.
uint8_t DodgeOfDirection() {
    float forward = 0.0f, left = 0.0f;
    Player1MoveDirection(forward, left);  // a left-handed player's dodges play mirrored
    if (std::fabs(forward) < 0.25f && std::fabs(left) < 0.25f) return 0x50;
    if (std::fabs(forward) >= std::fabs(left)) return forward > 0.0f ? 0x51 : 0x52;
    return left > 0.0f ? 0x53 : 0x54;
}

// "Dodge : temps de recharge" (user, 2026-10-27: dodges chained too fast cut the one playing): the trainer's dodges
// (the one of the direction, the aim one, the fixed ones) wait the slider's seconds after the last one.
std::atomic<float> g_dodgeCooldown{ 1.5f };
DWORD g_lastDodge = 0;  // render thread (hotkeys)

bool DodgeReady() {
    const float wait = g_dodgeCooldown.load(std::memory_order_relaxed);
    return !g_lastDodge || wait <= 0.0f || static_cast<float>(GetTickCount() - g_lastDodge) >= wait * 1000.0f;
}

bool DodgeAsked(bool asked) {
    if (asked) {
        const DWORD now = GetTickCount();
        g_lastDodge = now ? now : 1;
        NoteDodgeStarted();  // "Esquive parfaite"
    }
    return asked;
}

// The dodge styles (user, 2026-10-28: a choice for the dodge and one for the aim dodge): each direction's move, the
// game's (Quick Duck 0x50, rolls forward 0x51 / back 0x52 / left 0x53 / right 0x54) or a free melee move of
// wesker_moves.cpp (its option id). RE6 and step: the game's for now, until RE6's knife dodges are in (user).
struct DodgeStyle {
    const char* name;
    const char* moves[5];  // in place, forward, back, left, right; nullptr: the game's
};
const DodgeStyle kDodgeStyles[] = {
    { "Dodge RE5", {} },
    { "Dodge RE6", { "melee.free_re6_dodge_duck", "melee.free_re6_dodge_forward", "melee.free_re6_dodge_back",
                     "melee.free_re6_dodge_left", "melee.free_re6_dodge_right" } },
    { "Dodge step (pour l'instant celui de RE5)", {} },
    // Leon: RE5's, his RE4 back handspring for the back roll (cut before its long wait).
    { "Dodge Leon", { nullptr, nullptr, "melee.free_leon_dodge_handspring", nullptr, nullptr } },
    // Jill: her round-off forward, back flip back, cartwheels to the sides (em84nml 0x1E goes 2 m to her left, 0x1F 2.3 m
    // to her right: the roots' x, +x = left as DodgeOfDirection), her guard pose in place.
    { "Dodge Jill", { "melee.free_jill2_guard_pose", "melee.free_jill2_roundoff", "melee.free_jill2_backflip",
                      "melee.free_jill2_cartwheel1", "melee.free_jill2_cartwheel2" } },
    // Wesker: forward his boss kick series that ends his dodges (user, 2026-10-29, in place of his forward dodge).
    { "Dodge Wesker", { "melee.free_wesker_dodge6", "melee.free_wesker_riposte", "melee.free_wesker_dodge3",
                        "melee.free_wesker_dodge1", "melee.free_wesker_dodge2" } },
};
// Per character (the Dodge sub-tab's table, DrawCharacterChoices): the style of his dodge and of his dodge while aiming,
// -1 "Aucune" (his dodge keys refused); set by the choices from their registration on.
std::atomic<int> g_dodgeStyleOf[kCharacterCount], g_aimDodgeStyleOf[kCharacterCount];

// Player 1's character's style (-1 none: refused, logged once in two seconds).
int DodgeStyleNow(bool aim, const char* what) {
    const int character = Player1CharacterIndex();
    const int style = (aim ? g_aimDodgeStyleOf : g_dodgeStyleOf)[character].load(std::memory_order_relaxed);
    static DWORD logged = 0;
    if (style < 0 && GetTickCount() - logged > 2000) {
        logged = GetTickCount();
        LOG("dodge: %s refused, %s's set to none", what, CharacterName(character));
    }
    return style;
}

// The style's dodge for the game's dodge `dodge`; a move that cannot play (its motions not loaded) falls back on the
// game's.
// The game's dodges asked by the trainer: the stamina cost of their own option ("Esquive sur place (Quick Duck)", "Roulade avant"...).
float FixedDodgeCost(uint8_t dodge) {
    static constexpr const char* kIds[] = { "melee.free_dodge_duck", "melee.free_dodge_forward", "melee.free_dodge_back",
                                            "melee.free_dodge_left", "melee.free_dodge_right" };
    const Feature* f = dodge >= 0x50 && dodge <= 0x54 ? Find(kIds[dodge - 0x50]) : nullptr;
    return f ? f->value : -1.0f;
}

bool PlayStyledDodge(uintptr_t p, uint8_t dodge, int style) {
    const char* move = kDodgeStyles[std::clamp(style, 0, static_cast<int>(std::size(kDodgeStyles)) - 1)].moves[dodge - 0x50];
    if (move) {
        if (DodgeAsked(StartDodgeMove(move))) return true;  // its stamina spent by the move
        LOG("dodge: %s refused, the game's %X instead", move, dodge);
    }
    // Rolls align on the melee target like the melee moves; the game never clears it, so a dodge without a fresh one
    // turned to the last enemy a prompt had targeted, even dead (user, 2026-10-18; only while no swap slot refreshed the
    // virtual enemy every frame): the virtual enemy in front, no target.
    AimVirtual(p, true);
    const bool asked = DodgeAsked(RequestPlayer(p, dodge));
    if (asked) StaminaSpendDodge(FixedDodgeCost(dodge));
    return asked;
}

// Dodge (user, 2026-10-15): one key, Quick Duck (0x50) standing still, a roll along the movement otherwise. From the
// direction held, not from his movement action (user, 2026-10-27: in an animation that does not move him the quick duck
// came instead of the roll).
bool PlayDodge() {
    uint32_t current = 0;
    const uintptr_t p = FreePlayer1(current);
    if (!p) return false;
    const int style = DodgeStyleNow(false, "the dodge");
    if (style < 0 || !StaminaAllows() || !DodgeReady()) return false;
    return PlayStyledDodge(p, DodgeOfDirection(), style);
}

// Dodge while aiming (user, 2026-10-15: "aim + E"): only while aiming, the aim (game button 0x0C) or the knife aim
// (0x0D) held or Chris in an aim action (a pad's buttons never go through the button queries: user, 2026-10-23, nothing
// happened on a pad); the direction held (DodgeOfDirection: the left stick on a pad, the direction buttons otherwise,
// read by the player even while aiming): rolls 0x51 / 0x52 / 0x53 / 0x54, Quick Duck 0x50 without a direction.
bool AimingWith(uint32_t current) {
    const bool aimAction =
        current == 0x1B || current == 0x1C || current == 0x1E || (current >= 0x22 && current <= 0x27 && current != 0x24);
    return aimAction || GameButtonHeld(0x0C, Player1Pad()) || GameButtonHeld(0x0D, -1);
}

// From the frozen duck too (user, 2026-11-09: "from the frozen pose the dives on the other key, since I am aiming").
bool PlayAimDodge() {
    uint32_t current = 0;
    const bool ducked = Re6Ducked();
    const uintptr_t p = FreePlayer1(current, ducked);
    if (!p || !AimingWith(current)) return false;
    const int style = DodgeStyleNow(true, "the dodge while aiming");
    if (style < 0 || !StaminaAllows() || !DodgeReady()) return false;
    const uint8_t dodge = DodgeOfDirection();
    if (ducked) {
        const char* move = kDodgeStyles[std::clamp(style, 0, static_cast<int>(std::size(kDodgeStyles)) - 1)].moves[dodge - 0x50];
        if (move && DodgeAsked(StartDodgeFromDuck(move))) return true;
        StartDodgeFromDuck(nullptr);  // the game's dodge, or the move refused: the duck left first
    }
    return PlayStyledDodge(p, dodge, style);
}

// One key per dodge (user, 2026-10-23: beside the dodge that follows the movement): Quick Duck 0x50, rolls forward 0x51,
// back 0x52, left 0x53, right 0x54, aligned as the other dodges. None for a character whose dodge is "Aucune".
bool PlayFixedDodge(uint8_t dodge) {
    uint32_t current = 0;
    const uintptr_t p = FreePlayer1(current);
    if (!p || DodgeStyleNow(false, "the fixed dodge") < 0 || !StaminaAllows() || !DodgeReady()) return false;
    AimVirtual(p, true);
    const bool asked = DodgeAsked(RequestPlayer(p, dodge));
    if (asked) StaminaSpendDodge(FixedDodgeCost(dodge));
    return asked;
}

// The slide: the aim's press (the held mask input +0x51C bit 0x800, keyboard and pad) during the sprint, or just after it ended
// (the game drops the sprint as soon as the aim is pressed).
void TickRe6Slide() {
    static bool wasAim = false;
    static ULONGLONG sprinted = 0;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t block = 0;
    uint32_t held = 0;
    if (!p || !mem::Read(p + 0x2D64, block) || !block || !mem::Read(block + 0x220 + 0x51C, held)) {
        wasAim = false;
        return;
    }
    if (Player1Sprinting()) sprinted = GetTickCount64();
    const bool aim = (held & 0x800) != 0, pressed = aim && !wasAim;
    wasAim = aim;
    // The run key still held too (user, 2026-11-10: "aiming quickly after I stopped the sprint, the slide starts though the sprint is
    // over: as soon as the sprint key is no longer held, the aim is a normal aim"): the 250 ms only cover the game dropping the sprint
    // at the aim's press.
    if (!pressed || !sprinted || GetTickCount64() - sprinted > 250 || !Player1SprintKeyHeld()) return;
    sprinted = 0;
    if (!StaminaAllows()) {
        LOG("re6 slide: refused, the stamina's red arc");
        return;
    }
    const bool started = StartRe6AimSlide();
    if (const Feature* f = Find("player.re6_slide"); started && f) StaminaSpendDodge(f->value);
    LOG("re6 slide: %s", started ? "started" : "refused");
}

void DrawMeleePanel() {
    if (!ImGui::BeginTabBar("melee")) return;
    // The sub-tabs stay in view, each one scrolls below them (user, 2026-10-23).
    const auto tab = [](const char* label, const char* note, const char* category) {
        if (!ImGui::BeginTabItem(label)) return;
        ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_None);
        if (note) ImGui::TextDisabled("%s", note);
        DrawOptions(category);
        ImGui::EndChild();
        ImGui::EndTabItem();
    };
    // Per character (user, 2026-11-11, per_character.cpp): one sub-tab each, RE5's playable characters then RE6's; the options that stay
    // common (the tests) under the first.
    if (ImGui::BeginTabItem(i18n::Tr("Échanges"))) {
        ImGui::TextDisabled("%s", i18n::Tr("Chaque personnage a ses propres échanges et réglages de corps à corps : seuls ceux du personnage joué par le joueur 1 s'appliquent. Les onglets des personnages de RE6 attendent qu'ils soient jouables."));
        if (ImGui::BeginTabBar("swaps per character")) {
            static const char* const kWhoTabs[] = { "Chris", "Sheva", "Jill", "Wesker", "Josh", "Excella", "Barry", "Rebecca" };
            for (const char* who : kWhoTabs) {
                if (!ImGui::BeginTabItem(who)) continue;
                ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_None);
                DrawOptions(std::string("Mêlée · ") + who);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            tab("Commun", nullptr, "Mêlée");
            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }
    // Its attacks as a table (user, 2026-10-30, melee_tuning.cpp: shortcuts, reaction, damage, critical, kill, back-up).
    // `settings`: options drawn above the table (the RE6 melee's per character, user 2026-11-09: "an option in each character's tab").
    const auto table = [](const char* label, const char* note, const char* category, const char* settings = nullptr) {
        if (!ImGui::BeginTabItem(label)) return;
        ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        if (note) ImGui::TextDisabled("%s", note);
        if (settings) {
            DrawOptions(settings);
            // The RE6 melee's reactions (common to every character; user, 2026-11-09: "I do not see the dropdown").
            if (std::strncmp(settings, "RE6 · ", std::strlen("RE6 · ")) == 0) DrawOptions("Réglages RE6");
            ImGui::Separator();
        }
        DrawMeleeTable(category);
        ImGui::EndChild();
        ImGui::EndTabItem();
    };
    // The free melee in four sub-tabs (user, 2026-10-27: "so I do not search for three hours").
    if (ImGui::BeginTabItem(i18n::Tr("Mêlée libre"))) {
        ImGui::TextDisabled(i18n::Tr("Un raccourci par mouvement : Chris le joue tout de suite sur l'ennemi le plus proche (3 m), "
                            "sinon sur place."));
        if (ImGui::BeginTabBar("free melee")) {
            table("Général", nullptr, "Mêlée libre");
            table("Chris", "Ses mouvements, quel que soit le personnage joué.", "Mêlée libre Chris", "RE6 · Chris");
            table("Sheva", "Ses mouvements de Mercenaries, quel que soit le personnage joué.", "Mêlée libre Sheva", "RE6 · Sheva");
            table("Jill", "Ses mouvements de Mercenaries (Jill ...) et ceux de son combat (projections, coups de pied...).",
                "Mêlée libre Jill", "RE6 · Jill");
            table("Wesker", "Ses mouvements de Mercenaries (Wesker ...) et ceux de ses combats (coups de pied...).",
                "Mêlée libre Wesker", "RE6 · Wesker");
            table("Josh", "Ses mouvements de Mercenaries Reunion, quel que soit le personnage joué.", "Mêlée libre Josh", "RE6 · Josh");
            table("Barry", "Ses mouvements de Mercenaries Reunion, quel que soit le personnage joué.", "Mêlée libre Barry", "RE6 · Barry");
            table("Rebecca", "Ses mouvements de Mercenaries Reunion, quel que soit le personnage joué.", "Mêlée libre Rebecca",
                "RE6 · Rebecca");
            table("Excella", "Ses mouvements de Mercenaries Reunion, quel que soit le personnage joué (certains effets "
                "peuvent manquer).", "Mêlée libre Excella", "RE6 · Excella");
            table("Leon (RE4)", "Les mouvements de Leon dans Resident Evil 4 (2005), joués par Chris (sans sons ni effets "
                "pour l'instant ; « test » : joué seul, sans l'ennemi synchronisé).", "Mêlée libre Leon");
            table("Leon (Remake)", "Les parades et les mouvements au couteau de Leon dans Resident Evil 4 Remake, joués par "
                "Chris (« test » : joué seul, sans l'ennemi synchronisé).", "Mêlée libre Leon Remake");
            // RE6's characters (user, 2026-11-09: their moves named in the RE6 test bank, "under each one").
            table("RE6 (commun)", "Les mouvements de Resident Evil 6 de tous les personnages (contres, coups d'arme à deux mains, "
                "attaques en sprint, coups de coude, caisses...) et les réactions de la mêlée RE6. Le combo de chaque personnage se "
                "choisit dans son onglet.", "Mêlée libre RE6", "Réglages RE6");
            table("Chris (RE6)", "Ses mouvements de Resident Evil 6, joués par le personnage (animations de tes fichiers de RE6).",
                "Mêlée libre Chris RE6");
            table("Piers (RE6)", "Ses mouvements de Resident Evil 6, joués par le personnage (animations de tes fichiers de RE6).",
                "Mêlée libre Piers RE6");
            table("Sherry (RE6)", "Ses mouvements de Resident Evil 6, joués par le personnage (animations de tes fichiers de RE6).",
                "Mêlée libre Sherry RE6");
            table("Leon (RE6)", "Ses mouvements de Resident Evil 6, joués par Chris (animations de tes fichiers de RE6).",
                "Mêlée libre Leon RE6");
            table("Helena (RE6)", "Ses mouvements de Resident Evil 6, joués par Chris (animations de tes fichiers de RE6).",
                "Mêlée libre Helena RE6");
            table("Jake (RE6)", "Ses mouvements de Resident Evil 6, joués par Chris (animations de tes fichiers de RE6).",
                "Mêlée libre Jake RE6");
            table("Ada (RE6)", "Ses mouvements de Resident Evil 6, joués par Chris (animations de tes fichiers de RE6).",
                "Mêlée libre Ada RE6");
            table("HUNK", "Ses mouvements des Mercenaires de Resident Evil 4, joués par Chris (« test » : joué seul, sans "
                "l'ennemi synchronisé).", "Mêlée libre HUNK");
            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(i18n::Tr("Dodge"))) {
        ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_None);
        ImGui::SeparatorText(i18n::Tr("Par personnage"));
        ImGui::TextDisabled(i18n::Tr("Le style des esquives selon le personnage joué (« Aucune » : il ne peut pas esquiver)."));
        DrawCharacterChoices("dodge_styles", { { "Esquive", "melee.dodge_style_" }, { "Esquive en visant", "melee.aim_dodge_style_" } });
        ImGui::SeparatorText(i18n::Tr("Esquives"));
        ImGui::TextDisabled(i18n::Tr("Un raccourci par esquive : Chris la fait tout de suite."));
        DrawOptions("Dodge");
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    // Research (user, 2026-11-09): RE6's melee motions played one by one to name them (wesker_moves.cpp).
    tab("Test RE6", "Recherche : les mouvements de corps à corps de RE6 (la liste commune AcA puis ceux de chaque personnage), "
        "à jouer un par un sur Chris pour les "
        "reconnaître. Le curseur choisit le numéro, le bouton ou son raccourci le joue.", "Test RE6");
    tab("Automatisation", "Chris répond tout seul aux attaques qui entrent dans la sphère de l'esquive parfaite (sa taille "
        "se règle dans Dodge).", "Automatisation");
    ImGui::EndTabBar();
}

// perfect_dodge.cpp's automatic dodges (user, 2026-10-29): the styles' names, a style's dodge in a direction (0x50 in
// place, 0x51 forward, 0x52 back, 0x53 left, 0x54 right; no key nor cooldown), the direction held (0x50 none).
// wesker_moves.cpp's RE6 melee ("Finisher Front du jeu"): the game's melee move on the nearest enemy, as the free melee plays it.
bool PlayNativeMeleeMove(uint8_t action) { return RequestFreeMove(action); }

std::vector<std::string> DodgeStyleNames() {
    std::vector<std::string> names;
    for (const DodgeStyle& style : kDodgeStyles) names.push_back(style.name);
    return names;
}

bool PlayAutoDodge(int style, uint8_t dodge) {
    uint32_t current = 0;
    const uintptr_t p = FreePlayer1(current);
    if (!p || !StaminaAllows() || dodge < 0x50 || dodge > 0x54) return false;
    return PlayStyledDodge(p, dodge, style);
}

uint8_t HeldDodgeDirection() { return DodgeOfDirection(); }

void RegisterSwapFeatures() {
    Add({ .id = "melee.re6_weapon_steal", .name = "Vol d'arme (RE6) sur les ennemis armés",
          .help = "Sur un Majini armé d'une arme de corps à corps (hache, massue, clé, couteau, faucille, machette, bouteille...) et étourdi, chaque prompt de corps à corps (Head Flash, Leg Front / Back, Arm Front / Back, Finisher Front / Back) fait le vol d'arme de RE6 à la place, quel que soit son échange : Chris lui arrache son arme et le frappe avec. Pas sur un ennemi au sol (Ground).",
          .category = "Mêlée", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && actors::InGame() && !PrepareWeskerMoves()) return false;
              if (on && !EnsurePlayerRequestHook()) return false;
              g_weaponSteal = on;
              return true;
          },
          .available = Supported });
    Feature& envWall = Add({ .id = "melee.re6_env_wall", .name = "Exécutions contre le décor (RE6) : murs, tables et barrières",
          .help = "Sur un Majini étourdi près d'un mur, d'une table ou d'une barrière basse, chaque prompt de corps à corps (Head Flash, "
                  "Leg Front / Back, Arm Front / Back, Finisher Front / Back) et le coup final de la mêlée RE6 font l'exécution de "
                  "RE6 contre l'obstacle à la place : Chris lui écrase la tête contre le mur, ou le plaque sur la table ou la barrière "
                  "(de face ou de côté). Le curseur est la tolérance : jusqu'où Chris et l'ennemi peuvent être déplacés vers leurs "
                  "places (ils y glissent au début du mouvement) ; plus il est grand, plus on peut être loin de l'obstacle.",
          .category = "Mêlée", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && actors::InGame() && !PrepareWeskerMoves()) return false;
              if (on && (!EnsurePlayerRequestHook() || !EnsurePadQueryHooks())) return false;  // the checks on the game thread
              g_envWall = on;
              return true;
          },
          .available = Supported, .value = 80.0f, .valueMin = 10.0f, .valueMax = 300.0f, .valueFormat = "%.0f cm" });
    g_envWallFeature = &envWall;
    Feature& envHeight = Add({ .id = "melee.re6_env_height", .name = "Exécutions d'une hauteur (RE6) : par le dessus et par le dessous",
          .help = "Un Majini près du bord d'un rebord (70 cm à 1,40 m), au-dessus ou en dessous de Chris, lui-même près du bord : le prompt "
                  "pour grimper ou pour sauter / descendre fait l'exécution de RE6 à la place, sans avoir à étourdir l'ennemi. Par le "
                  "dessous Chris le tire du rebord et l'écrase au sol (de face ou de dos), par le dessus il lui saute dessus. Avec « Corps "
                  "à corps sur la touche de tir », ce prompt passe sur la touche d'attaque tant qu'une exécution est possible. Le curseur "
                  "est la tolérance : jusqu'où Chris et l'ennemi peuvent être déplacés vers leurs places. Seulement là où le jeu "
                  "propose de grimper ou de descendre.",
          .category = "Mêlée", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && actors::InGame() && !PrepareWeskerMoves()) return false;
              if (on && (!EnsurePlayerRequestHook() || !EnsurePadQueryHooks())) return false;  // the checks on the game thread
              g_envHeight = on;
              return true;
          },
          .available = Supported, .value = 80.0f, .valueMin = 10.0f, .valueMax = 300.0f, .valueFormat = "%.0f cm" });
    g_envHeightFeature = &envHeight;
    // Melee Swap (input actions) then Melee Moves Swap (prompts). Sites: research/melee_slots.txt.
    AddSlot("melee.move_forward", "Échange : Avancer (Move Forward)", { { 0x75D437, 0x02, 0x02 } });
    AddSlot("melee.move_back", "Échange : Reculer (Move Backwards)", { { 0x75D61D, 0x06, 0x06 } });
    AddSlot("melee.move_left", "Échange : Gauche (Move Left)", { { 0x75D4B7, 0x03, 0x03 } });
    AddSlot("melee.move_right", "Échange : Droite (Move Right)", { { 0x75D537, 0x04, 0x04 } });
    AddSlot("melee.run", "Échange : Courir (Run)", { { 0x75D5B7, 0x05, 0x05 } });
    AddSlot("melee.quick_turn", "Échange : Demi-tour (Quick Turn)", { { 0x75D792, 0x09, 0x09 } });
    AddSlot("melee.reload", "Échange : Recharger (Reload)", { { 0x76C472, 0x1F, 0x1F } });
    AddSlot("melee.taunt", "Échange : Provocation (Taunt)", { { 0x75D9E7, 0x2F, 0x2F } });
    AddSlot("melee.knife", "Échange : Couteau (Knife)", { { 0x77856D, 0x22, 0x22 } });
    AddSlot("melee.partner_action", "Échange : Action partenaire", { { 0x75F91D, 0x81, 0x81 } });
    // + the Reunion variants requested by Reunion characters (Barry in Mercenaries Reunion: push 0xAC / 0xAD,
    // found with the action log 2026-10-13): one byte of a push imm32, the other bytes stay 0.
    // UT grouped the licker counter (attack_upper_licker, push 0x3D at RVA 0x75DF72) with Leg Front: its own slot here
    // (user, 2026-10-14).
    AddSlot("melee.leg_front", "Échange : Leg Front",
            { { 0x75DAF3, 0x31, 0x31 }, { 0x75E093, 0x3F, 0x3F }, { 0x75DB83, 0xAC, 0xAC } });
    AddSlot("melee.licker_counter", "Échange : Contre licker", { { 0x75DF73, 0x3D, 0x3D } },
            "Déconseillé : seul le contre d'origine est assez rapide pour toucher le licker et le faire tomber ; les "
            "coups plus lents le ratent (testé le 15/10).");
    AddSlot("melee.leg_back", "Échange : Leg Back", { { 0x75DE53, 0x39, 0x39 } });
    AddSlot("melee.arm_front", "Échange : Arm Front", { { 0x75DA63, 0x30, 0x30 } });
    AddSlot("melee.arm_back", "Échange : Arm Back", { { 0x75DD33, 0x33, 0x33 } });
    AddSlot("melee.finisher_front", "Échange : Finisher Front", { { 0x76CB3F, 0x35, 0x35 }, { 0x75E243, 0x42, 0x42 } });
    AddSlot("melee.finisher_back", "Échange : Finisher Back", { { 0x76CA7F, 0x34, 0x34 } });
    AddSlot("melee.ground", "Échange : Ground (ennemi au sol)", { { 0x76CD1B, 0x38, 0x38 } });
    AddSlot("melee.help", "Échange : Help (aider le partenaire)", { { 0x75DDC3, 0x37, 0x37 } });
    AddSlot("melee.head_flash", "Échange : Head Flash", { { 0x75DC13, 0x32, 0x32 }, { 0x75DCA3, 0xAD, 0xAD } });
    // Partner rescues seen in the action log (2026-10-14): Sheva frees Chris from a parasite with attack_knife_flying
    // (targeted handler RVA 0x75DF90, push 0x3E at RVA 0x75E002) and from a licker's tongue with
    // attack_knife_licker_tongue (handler RVA 0x76CE40, push 0x3C at RVA 0x76CEDB).
    AddSlot("melee.rescue_parasite", "Échange : Libération parasite (couteau)", { { 0x75E003, 0x3E, 0x3E } },
            "Déconseillé avec les mouvements synchronisés avec un ennemi (Finisher Front, prises) : ils figent les "
            "personnages. Les frappes simples sont à préférer.");
    AddSlot("melee.rescue_licker_tongue", "Échange : Libération langue de licker", { { 0x76CEDC, 0x3C, 0x3C } },
            "Frappes simples et Finisher Back : marchent très bien. Déconseillé avec les mouvements synchronisés avec "
            "un ennemi : Finisher Front (ciseau) a figé Chris et le licker (testé le 15/10).");
    // Licker knocked down by the counter (attack_upper_licker 0x3D, in the Leg Front slot as in UT) then finished with
    // the knife: attack_knife_licker (handler RVA 0x76CD70, push 0x3B at RVA 0x76CE2B; user, 2026-10-14).
    AddSlot("melee.licker_knife", "Échange : Finisher licker au couteau", { { 0x76CE2C, 0x3B, 0x3B } },
            "« Ground » marche très bien sur le licker à terre (testé le 15/10).");

    std::vector<std::string> sets = { "Par défaut" };
    for (const AnimSet& s : kAnimSets) sets.push_back(s.name);
    static_assert(std::size(kAnimSetArchives) == std::size(kAnimSets) && std::size(kAnimSetEffects) == std::size(kAnimSets));
    for (CharAnims& c : g_charAnims) {
        CharAnims* anims = &c;
        Feature& feature = Add({ .id = c.id, .name = c.name,
                                 .help = "Ce personnage (joueur 1 ou 2) prend le jeu de mouvements d'un autre : animations et coups au corps "
                                         "à corps tout de suite, sans recharger (l'archive de l'autre personnage est chargée au "
                                         "besoin), sons et effets des coups au prochain chargement (son modèle, son visage et sa "
                                         "voix restent). Les "
                                         "variantes Reunion de Chris, Sheva et Jill demandent leurs fichiers chargés dans le "
                                         "niveau. Excella : impossible sur un autre personnage.",
                                 // In Mêlée > Échanges (user, 2026-10-27: "more about the swaps").
                                 .category = "Mêlée", .kind = Kind::Choice, .available = Supported,
                                 .value = 0, .valueMin = 0, .valueMax = static_cast<float>(sets.size() - 1),
                                 .valueFormat = "%.0f", .choices = sets });
        Feature* self = &feature;
        feature.choose = [anims, self](int index) {
            if (index != 0) {
                if (!EnsureLoadListHook() || !EnsureMeleeSetHook() || !EnsureResLoadHook() || !EnsureSetMotionHook() ||
                    !EnsureMoveIndexHook())
                    return false;
                const AnimSet& s = kAnimSets[index - 1];
                if (s.ownOnly && std::strcmp(s.name, "Excella") == 0 && anims->charId != 5) {
                    self->lastError = "les coups d'Excella demandent ses propres effets : impossible sur un autre personnage";
                    return false;
                }
                if (InLevel() && !ResourceLoaded(mem::Rva(kTypeMotionList), s.motion) && !kAnimSetArchives[index - 1]) {
                    self->lastError = std::string("animations de ") + s.name + " absentes de ce niveau";
                    static int lastLogged = -1;
                    if (lastLogged != index) LOG("anim swap %s: %s not loaded in this level", anims->id, s.motion);
                    lastLogged = index;
                    return false;
                }
            }
            anims->set = index;
            if (InLevel() && !ApplyAnimSetLive(*anims, index)) {
                self->lastError = "pris en compte au prochain chargement seulement";
                LOG("anim swap %s: set %d for the next load only", anims->id, index);
            }
            return true;
        };
    }

    OnFrame(UpdateReunionAuto);
    OnFrame(TickLoan);
    // The hooks a first borrowed move or trainer move installed when it started, each freezing the game about 0.2 s while
    // MinHook suspends its threads (user, 2026-11-06: "a small freeze at each first free melee since the trainer's
    // launch"; about a second for a borrowed move: melee set, move index, resource load, sound request, player voice):
    // installed with the others at the trainer's start. They stay passive until a move uses them.
    OnFrame([] {
        static bool done = false;
        if (done || !game::IsSupported()) return;
        done = true;
        const bool all = EnsureMeleeSetHook() & EnsureMoveIndexHook() & EnsureResLoadHook() & EnsureSoundRequestHook() &
                         EnsurePlayerVoiceHook() & EnsureGrabRefusalHook() & EnsurePlayerContactHook();
        LOG("moves: the borrowed and trainer moves' hooks installed at the start%s", all ? "" : " (not all)");
    });
    OnRestore([] { EndLoan("trainer unloaded"); });  // the borrower's own list, sounds and attack shapes back
    OnFrame(TickShapeMirror);
    OnRestore(UnmirrorShapes);  // his shapes on his own limbs again
    Add({ .id = "diag.sound_request_log", .name = "Journal des sons des personnages (recherche)",
          .help = "Écrit dans trainer.log chaque son demandé par le joueur 1 et par les ennemis (banque, numéro, action et "
                  "animation à ce moment-là). Sert à retrouver les sons natifs (craquement du neck break, impacts des coups "
                  "de pied de Jill et de Wesker) pour les mouvements du trainer. À couper ensuite.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureSoundRequestHook()) return false;
              if (on) g_soundLogLines = 0;
              g_soundLog = on;
              return true;
          },
          .available = Supported });
    OnFrame(ReleaseMeleeTargets);

    OnFrame(FinishPendingRestores);
    OnFrame(AnimFamilyFlags);
    OnFrame(ForcedPartnerTick);
    OnFrame(UpdateCostumeLists);
    std::vector<std::string> chars = { "Par défaut" };
    for (const char* c : kCharNames) chars.push_back(c);
    std::vector<std::string> costumes = { "Par défaut" };
    for (int i = 1; i <= 10; ++i) costumes.push_back("Costume " + std::to_string(i));  // replaced by names in game
    for (int player = 0; player < 2; ++player) {
        static const char* swapIds[] = { "char.swap_p1", "char.swap_p2" };
        static const char* swapNames[] = { "Personnage du joueur 1", "Personnage du joueur 2 (partenaire)" };
        static const char* costumeIds[] = { "char.costume_p1", "char.costume_p2" };
        static const char* costumeNames[] = { "Costume du joueur 1", "Costume du joueur 2 (partenaire)" };
        Add({ .id = swapIds[player], .name = swapNames[player],
              .help = player == 0
                          ? "Change le personnage du joueur 1 au prochain chargement (recommencer au point de passage). "
                            "Activer aussi « Ignorer les fichiers manquants ». « Par défaut » remet le personnage d'origine."
                          : "Change le partenaire (joueur 2, IA en solo), par exemple Wesker à la place de Sheva, au "
                            "prochain chargement (recommencer au point de passage ou Continuer depuis le menu). "
                            "« Par défaut » remet le partenaire d'origine.",
              .category = "Personnage", .kind = Kind::Choice,
              .tick = [player] { FreezeCharSwap(player); },
              .available = Supported,
              .choose = [player](int index) {
                  g_charSwap[player].character = index;
                  return ApplyCharSwap(player);
              },
              .value = 0, .valueMin = 0, .valueMax = static_cast<float>(chars.size() - 1), .valueFormat = "%.0f",
              .choices = chars });
        g_costumeFeature[player] = &Add({ .id = costumeIds[player], .name = costumeNames[player],
              .help = "Costume au prochain chargement (recommencer au point de passage). La liste montre les tenues du "
                      "personnage choisi juste au-dessus (ou du personnage actuel). Les tenues « Reunion » contiennent "
                      "les animations des coups de Mercenaries Reunion.",
              .category = "Personnage", .kind = Kind::Choice,
              .tick = [player] { FreezeCharSwap(player); },
              .available = Supported,
              .choose = [player](int index) {
                  g_charSwap[player].costume = index;
                  return ApplyCharSwap(player);
              },
              .value = 0, .valueMin = 0, .valueMax = static_cast<float>(costumes.size() - 1), .valueFormat = "%.0f",
              .choices = costumes });
    }

    for (const FreeMove& move : kFreeMoves) {
        const uint8_t action = move.action;
        Add({ .id = move.id, .name = move.name,
              .help = "Chris joue ce mouvement de corps à corps dès qu'on appuie sur le raccourci, sans prompt : sur "
                      "l'ennemi le plus proche (3 m, coups synchronisés compris), sinon sur place. Certains mouvements ne "
                      "touchent que dans leur situation d'origine (achèvement sur un ennemi au sol...).",
              .category = "Mêlée libre", .kind = Kind::Action,
              .run = [action, id = std::string(move.id)] {
                  if (Player1MovePlaying()) return false;
                  bool played = false;
                  if (ShortcutBackup(id, played)) return played;
                  return PlayFreeMove(action);
              },
              .available = [] { return actors::InGame(); } });
    }
    for (int set = 0; set < static_cast<int>(std::size(kLoanSets)); ++set) {
        const LoanSet& ls = kLoanSets[set];
        for (const BorrowedMove& move : kBorrowedMoves) {
            const uint8_t action = move.action;
            static std::deque<std::string> texts;  // ids and names live as long as the features
            texts.push_back(std::string("melee.free_") + ls.key + "_" + move.key);
            const char* id = texts.back().c_str();
            texts.push_back(std::string(ls.who) + " " + move.name);
            const char* name = texts.back().c_str();
            std::string game = MeleeName(ls.row, LenderMove(set, action));
            while (!game.empty() && (game.back() == '\t' || game.back() == ' ')) game.pop_back();
            for (char& c : game)
                if (c == '\t') c = ' ';
            Add({ .id = id, .name = name,
                  .help = std::string("Le mouvement de ") + ls.who + " (Mercenaries Reunion" +
                          (game.empty() ? "" : " : « " + game + " »") +
                          "), joué par le joueur 1 quel que soit son personnage, dès qu'on appuie sur le raccourci : son "
                          "animation et sa synchronisation avec l'ennemi, sur l'ennemi le plus proche (3 m), sinon sur "
                          "place. Ses fichiers sont chargés au premier coup.",
                  .category = std::string("Mêlée libre ") + ls.who, .kind = Kind::Action,
                  .run = [set, action, key = std::string(id)] {
                      if (Player1MovePlaying()) return false;
                      bool played = false;
                      if (ShortcutBackup(key, played)) return played;
                      return PlayBorrowedMove(set, action);
                  },
                  .available = [] { return actors::InGame(); } });
        }
    }
    // Wesker's dash (user, 2026-11-05: "a shortcut that starts Wesker's dash"; the UT's "Dash Start / Dash / Knee" swap
    // slots): his knee bazooka of Mercenaries, the player actions 0x47-0x4D (DTI classes cPlAction_pl_act_attack_
    // knee_bazooka_first, _second, _third, _fail, _turn_r, _turn_l, _turn_b) whose motions are slots 0x16.. of the
    // character's action list - Chris's own are his duo moves (TAG UPPER / TAG NECK), Wesker's (pl03action) his dash:
    // 0x16 the crouch before it, 0x17 the dash (18 frames, 5.5 m), 0x18 its stop, 0x19 a flying knee, 0x1A-0x1C turns.
    // Played with Wesker's moveset lent, the loan kept through the whole chain (KneeBazookaAction).
    Feature& dash = Add({ .id = "melee.free_wesker_dash", .name = "Dash de Wesker (Knee Bazooka)",
          .help = "Le dash de Wesker en Mercenaries (Knee Bazooka : il fonce droit devant et finit par un coup de genou), "
                  "joué par le joueur 1 quel que soit son personnage, avec les animations de Wesker, dès qu'on appuie sur "
                  "le raccourci.",
          .category = "Mêlée libre Wesker", .kind = Kind::Action,
          .run = [] {
              if (Player1MovePlaying()) return false;
              bool played = false;
              if (ShortcutBackup("melee.free_wesker_dash", played)) return played;
              return PlayBorrowedMove(2, 0x47);  // kLoanSets[2]: Wesker
          },
          .available = [] { return actors::InGame(); } });
    // "Dash infini" (UT; user, 2026-11-05: "it stays as long as the key is held"): the dash itself, knee_bazooka_second
    // (action 0x48, vtable RVA 0xC44CA4, update VA 0xC7EA50), runs on a countdown at its action object +0x1C (P+0x10F8)
    // set to 1.5 s by its init (VA 0xC8E290, constant VA 0x10BCD64) and lowered by the frame's time; at 1.3 s it sets a
    // flag (0x20000000), at 0 it ends (the stop 0x4A, or the knee 0x49 on an enemy). Held at 1 s while the dash's key is
    // down. (Its start, knee_bazooka_first VA 0xC8E210, costs the player [[0x11B27B4]+0x80] = 100 health, not the AI's.)
    static Feature* dashFeature = nullptr;
    dashFeature = &dash;
    static std::atomic<bool> dashHold{ false };
    Add({ .id = "melee.wesker_dash_hold", .name = "Dash de Wesker : continue tant que la touche est tenue",
          .help = "Le dash de Wesker (raccourci « Dash de Wesker » de la Mêlée libre Wesker) continue tant que sa touche "
                  "reste enfoncée, au lieu de s'arrêter au bout d'un instant ; relâcher la touche le termine comme dans le "
                  "jeu.",
          // Mêlée > Échanges (user, 2026-11-06: the free melee's sub-tabs show their moves' table only).
          .category = "Mêlée", .kind = Kind::Toggle,
          .set = [](bool on) {
              dashHold = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnFrame([] {
        if (!dashHold.load(std::memory_order_relaxed) || !dashFeature) return;
        const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
        uint32_t action = 0;
        uintptr_t object = 0, vtable = 0;
        if (!p || !mem::Read(p + 0x10E0, action) || action != 0x48 || !mem::Read(p + 0x10F8, object) || !object ||
            !mem::Read(object, vtable) || vtable != reinterpret_cast<uintptr_t>(mem::Rva(0xC44CA4)))
            return;
        if (!hotkeys::Held(dashFeature->hotkey) && !hotkeys::Held(dashFeature->hotkey2)) return;
        float left = 0.0f;
        if (mem::Read(object + 0x1C, left) && left < 1.0f) mem::Write(object + 0x1C, 1.0f);
    });
    // "Dash gratuit" (user, 2026-11-06: "an option that stops the health loss"): knee_bazooka_first's init (VA 0xC8E210)
    // charges the player [[0x11B27B4]+0x80] = 100 health (VA 0xB56FB0) unless P+0x2DA8 == 2 (VA 0xC8E25B cmp, then
    // je 0xC8E27C over the charge): the je made a jmp.
    static mem::Patch dashFree("melee.wesker_dash_free", 0x88E262, { 0x74, 0x18 }, { 0xEB, 0x18 });
    Add({ .id = "melee.wesker_dash_free", .name = "Dash de Wesker gratuit (sans perte de vie)",
          .help = "Le dash de Wesker (raccourci « Dash de Wesker » de la Mêlée libre Wesker) ne coûte plus de vie : dans le "
                  "jeu, chaque dash retire 100 points de vie au joueur.",
          .category = "Mêlée", .kind = Kind::Toggle,
          .set = [](bool on) { return on ? dashFree.IsRecognised() && dashFree.Apply() : dashFree.Restore(); },
          .available = [] { return game::IsSupported(); } });
    std::vector<std::string> styles;
    for (const DodgeStyle& style : kDodgeStyles) styles.push_back(style.name);
    const char* const styleHelp =
        "Les esquives jouées dans chaque direction. Dodge RE5 : celles du jeu (Quick Duck sur place, roulades). Dodge "
        "RE6 et Dodge step : celles de RE5 pour l'instant. Dodge Leon : celles de RE5, avec le saut arrière de Leon (RE4) "
        "vers l'arrière. Dodge Jill : la rondade en avant, le flip arrière en arrière, ses roues sur les côtés et sa pose "
        "de garde sur place. Dodge Wesker : sa série de coups de pied de fin d'esquive en avant, ses esquives arrière, "
        "gauche et droite, et son esquive basse sur place.";
    // The styles per character (user, 2026-11-01), "Aucune" first: that character cannot dodge with the trainer's keys.
    // Defaults from the user's examples: Chris RE5, Sheva Leon, Jill Jill, Wesker Wesker, the others RE5; while aiming
    // RE5 for all, as before.
    std::vector<std::string> styleChoices = { "Aucune" };
    styleChoices.insert(styleChoices.end(), styles.begin(), styles.end());
    static constexpr int kDefaultStyle[kCharacterCount] = { 0, 3, 4, 5, 0, 0, 0, 0 };
    for (int c = 0; c < kCharacterCount; ++c) {
        g_dodgeStyleOf[c] = -1;  // as the choice's index 0 until it is applied
        g_aimDodgeStyleOf[c] = -1;
        const std::string who = kCharacterNames[c];
        Add({ .id = std::string("melee.dodge_style_") + kCharacterKeys[c], .name = "Esquive de " + who,
              .help = "Le style de l'esquive (raccourcis « Esquive » et touches fixes) quand le joueur 1 joue " + who +
                      ". « Aucune » : " + who + " ne peut pas esquiver avec les raccourcis du trainer. " + styleHelp,
              .category = "Dodge par personnage", .kind = Kind::Choice, .available = Supported,
              .choose = [c](int index) {
                  g_dodgeStyleOf[c] = index - 1;
                  return true;
              },
              .value = static_cast<float>(kDefaultStyle[c] + 1), .valueMin = 0,
              .valueMax = static_cast<float>(styleChoices.size() - 1), .valueFormat = "%.0f", .choices = styleChoices });
        Add({ .id = std::string("melee.aim_dodge_style_") + kCharacterKeys[c], .name = "Esquive en visant de " + who,
              .help = "Le style de l'esquive en visant quand le joueur 1 joue " + who + ". « Aucune » : " + who +
                      " ne peut pas esquiver en visant. " + styleHelp,
              .category = "Dodge par personnage", .kind = Kind::Choice, .available = Supported,
              .choose = [c](int index) {
                  g_aimDodgeStyleOf[c] = index - 1;
                  return true;
              },
              .value = 1, .valueMin = 0, .valueMax = static_cast<float>(styleChoices.size() - 1), .valueFormat = "%.0f",
              .choices = styleChoices });
    }
    // The RE6 melee per character (user, 2026-11-09: "not a general option in the free melee tab but one in each character's tab, so I
    // tick it for each one and give each one his combo: Chris Chris's, Sheva none, Jill Helena's, Wesker Jake's"; the sprint attack
    // "a choice between the shoulder and the jump kick so I can give it to anyone", the elbow "choose between the two or randomise").
    for (int c = 0; c < kCharacterCount; ++c) {
        const std::string who = kCharacterNames[c];
        const std::string category = "RE6 · " + who;
        const bool woman = c == 1 || c == 2 || c == 5 || c == 7;
        Add({ .id = std::string("melee.re6_combo_") + kCharacterKeys[c], .name = "Corps à corps RE6 de " + who,
              .help = "La touche d'attaque sans viser, un ennemi proche : 1er coup (il sursaute, sans prompt), appuyer encore : 2e coup, "
                      "plus fort, qui ouvre le prompt choisi (RE6 (commun)) ; sur un ennemi déjà sonné, un finisher. En reculant : "
                      "le coup de coude arrière ; en sprint : l'attaque en sprint ; sans endurance : un coup sans prompt ; avec une "
                      "arme à deux mains : ses coups ; une caisse devant au lieu d'un ennemi : la casser. Sans ennemi proche, la "
                      "touche garde ses autres usages (tir rapide, tir sans viser...). Aucun : la mêlée RE6 est désactivée pour " + who + ".",
              .category = category, .kind = Kind::Choice, .available = Supported,
              .choose = [c](int index) {
                  SetRe6MeleeChoice(0, c, index);
                  return true;
              },
              .value = 0, .valueMin = 0, .valueMax = 5, .valueFormat = "%.0f",
              .choices = { "Aucun (corps à corps du jeu)", "Combo de Chris (RE6)", "Combo de Leon (RE6)", "Combo d'Helena (RE6)",
                           "Combo de Jake (RE6)", "Combo de Sherry (RE6)" } });
        Add({ .id = std::string("melee.re6_finisher_") + kCharacterKeys[c], .name = "Finisher RE6 de " + who,
              .help = "La touche d'attaque sur un Majini déjà sonné (le 2e coup, une glissade, un tir rapide, un tir qui le fait "
                      "vaciller...) : ce finisher. « Celui du combo » : le finisher du personnage de RE6 dont " + who +
                      " a le combo.",
              .category = category, .kind = Kind::Choice, .available = Supported,
              .choose = [c](int index) {
                  SetRe6MeleeChoice(6, c, index);
                  return true;
              },
              .value = 0, .valueMin = 0, .valueMax = 9, .valueFormat = "%.0f",
              .choices = { "Celui du combo", "Finisher Front du jeu", "Finisher de Chris (RE6)", "Finisher de Leon (RE6)",
                           "Finisher d'Helena (RE6)", "Finisher de Piers (RE6)", "Finisher de Jake (RE6)",
                           "Finisher de Sherry (RE6)", "Finisher d'Ada (RE6)", "Finisher de l'agent (RE6)" } });
        Add({ .id = std::string("melee.re6_sprint_") + kCharacterKeys[c], .name = "Attaque en sprint RE6 de " + who,
              .help = "Pendant le sprint, la touche d'attaque près d'un ennemi (4 m) : ce coup.",
              .category = category, .kind = Kind::Choice, .available = Supported,
              .choose = [c](int index) {
                  SetRe6MeleeChoice(1, c, index);
                  return true;
              },
              .value = woman ? 1.0f : 0.0f, .valueMin = 0, .valueMax = 2, .valueFormat = "%.0f",
              .choices = { "Coup d'épaule", "Coup de pied sauté", "Variante (n° 36)" } });
        Add({ .id = std::string("melee.re6_sprint_random_") + kCharacterKeys[c], .name = "Attaque en sprint RE6 de " + who + " : aléatoire",
              .help = "Cochée : à chaque attaque en sprint, l'un des trois coups au hasard (épaule, coup de pied sauté, variante).",
              .category = category, .kind = Kind::Toggle,
              .set = [c](bool on) {
                  SetRe6MeleeChoice(4, c, on ? 1 : 0);
                  return true;
              },
              .available = Supported });
        Add({ .id = std::string("melee.re6_elbow_") + kCharacterKeys[c], .name = "Coup de coude arrière RE6 de " + who,
              .help = "La touche d'attaque en reculant (direction arrière tenue) : ce coup de coude vers l'arrière.",
              .category = category, .kind = Kind::Choice, .available = Supported,
              .choose = [c](int index) {
                  SetRe6MeleeChoice(2, c, index);
                  return true;
              },
              .value = 0, .valueMin = 0, .valueMax = 1, .valueFormat = "%.0f",
              .choices = { "Coup de coude (n° 30)", "Variante (n° 31)" } });
        Add({ .id = std::string("melee.re6_elbow_random_") + kCharacterKeys[c], .name = "Coup de coude arrière RE6 de " + who + " : aléatoire",
              .help = "Cochée : à chaque attaque en reculant, l'un des deux coups de coude au hasard.",
              .category = category, .kind = Kind::Toggle,
              .set = [c](bool on) {
                  SetRe6MeleeChoice(5, c, on ? 1 : 0);
                  return true;
              },
              .available = Supported });
    }
    // RE6's slide (user, 2026-11-08): the aim pressed during the sprint.
    Add({ .id = "player.re6_slide", .name = "Glissade (RE6)",
          .help = "Comme dans RE6 : appuyer sur la touche de visée pendant le sprint déclenche la glissade ; ses jambes "
                  "frappent les ennemis (petits dégâts, réactions ci-dessous). Le curseur : son coût avec « Endurance "
                  "stricte » (tout à gauche, « blocage seul » : elle arrête seulement le remplissage).",
          .category = "Dodge", .kind = Kind::Toggle,
          .set = [](bool) { return true; },
          .tick = TickRe6Slide,
          .available = Supported,
          .value = 200, .valueMin = -100, .valueMax = 2000, .valueFormat = "%.0f pts (stricte)", .negativeText = "blocage seul" });
    // Its blow (user, 2026-11-09, wesker_moves.cpp TickSlideBlow): the Majini's reaction and its prompt, chosen as the parry's ("let me choose
    // the reaction too"), on a normal Majini (the arm hit + Arm by default) and on one already staggered (kneeling + Finisher: "on their
    // knees as after a leg shot, as RE6's enemies").
    for (int staggered = 0; staggered < 2; ++staggered) {
        Add({ .id = staggered ? "player.re6_slide_state_staggered" : "player.re6_slide_state",
              .name = staggered ? "Glissade (RE6) : réaction d'un Majini déjà sonné" : "Glissade (RE6) : réaction d'un Majini normal",
              .help = staggered
                          ? "La glissade fait de petits dégâts (comme un Arm Front). Un Majini déjà sonné (tir rapide, tir qui le "
                            "fait vaciller, combo avec un Arm Front...) qu'elle touche prend cette réaction, avec ce prompt de "
                            "corps à corps."
                          : "La glissade fait de petits dégâts (comme un Arm Front). Un Majini qui n'est pas sonné prend cette "
                            "réaction, avec ce prompt de corps à corps.",
              .category = "Dodge", .kind = Kind::Choice,
              .available = Supported,
              .choose = [staggered](int index) {
                  SetSlideState(staggered != 0, index);
                  return true;
              },
              .value = staggered ? 18.0f : 10.0f, .valueMin = 0, .valueMax = 19, .valueFormat = "%.0f",
              .choices = { "Head flash", "Recul + head flash", "Arm", "Leg", "Finisher",
                           "Recul + Arm (Arm Front de face, Arm Back de dos)", "Recul + Arm Front (natif : head flash de dos)",
                           "Recul + Leg (Leg Front de face, Leg Back de dos)", "Recul + Leg Front (de face comme de dos)",
                           "Bras + head flash (Arm Back de dos)", "Bras + Arm (Arm Front de face, Arm Back de dos)",
                           "Bras + Leg (Leg Front de face, Leg Back de dos)", "Bras + Leg Front (de face comme de dos)",
                           "Bras + Finisher", "Genou + head flash (Arm Back de dos)",
                           "Genou + Arm (Arm Front de face, Arm Back de dos)", "Genou + Leg (Leg Front de face, Leg Back de dos)",
                           "Genou + Leg Front (de face comme de dos)", "Genou + Finisher", "Chute (tir dans la jambe en courant)" } });
    }
    // "Esquive (sur place / roulade) : ignorée en visant" (user, 2026-11-09: "ignore the dodge shortcut while I aim, so that "Se baisser en
    // visant (pose figée)" can share its key and only counts while aiming"): its shortcut skipped, no toast, while player 1 aims.
    static std::atomic<bool> s_dodgeNotAiming{ false };
    Add({ .id = "melee.free_dodge", .name = "Esquive (sur place / roulade)",
          .help = "Un seul raccourci : immobile, Chris esquive sur place (Quick Duck) ; en avançant ou en courant, roulade "
                  "avant ; en reculant, roulade arrière ; en se déplaçant à gauche ou à droite, roulade de ce côté (ou "
                  "les esquives du style de son personnage, tableau ci-dessus). Avec « Endurance stricte », elle coûte le "
                  "curseur de l'esquive jouée.",
          .category = "Dodge", .kind = Kind::Action,
          .run = [] { return PlayDodge(); },
          .available = [] { return actors::InGame(); },
          .hotkeyIgnored = [] {
              if (!s_dodgeNotAiming) return false;
              const uintptr_t p = actors::Player(0);
              uint32_t current = 0;
              return p && mem::Read(p + 0x10E0, current) && AimingWith(current);
          } });
    Add({ .id = "melee.free_dodge_not_aiming", .name = "Esquive (sur place / roulade) : ignorée en visant",
          .help = "Le raccourci « Esquive (sur place / roulade) » ne fait rien pendant la visée (arme ou couteau) : sa touche "
                  "peut alors servir à une option qui ne marche qu'en visant, comme « Se baisser en visant (pose figée) ». "
                  "En visant, les esquives restent sur « Esquive en visant ».",
          .category = "Dodge", .kind = Kind::Toggle,
          .set = [](bool on) {
              s_dodgeNotAiming = on;
              return true;
          },
          .available = Supported });
    Add({ .id = "melee.free_aim_dodge", .name = "Esquive en visant",
          .help = "Ne marche qu'en visant (arme ou couteau) : sans direction, Chris esquive sur place (Quick Duck) ; avec "
                  "une direction tenue (avant, arrière, gauche, droite), roulade de ce côté (ou les esquives du style "
                  "de son personnage, tableau ci-dessus). Avec « Endurance stricte », elle coûte le curseur de l'esquive "
                  "jouée.",
          .category = "Dodge", .kind = Kind::Action,
          .run = [] { return PlayAimDodge(); },
          .available = [] { return actors::InGame(); } });
    static constexpr struct { const char* id; const char* name; uint8_t action; } kDodges[] = {
        { "melee.free_dodge_duck", "Esquive sur place (Quick Duck)", 0x50 },
        { "melee.free_dodge_forward", "Roulade avant", 0x51 },
        { "melee.free_dodge_back", "Roulade arrière", 0x52 },
        { "melee.free_dodge_left", "Roulade à gauche", 0x53 },
        { "melee.free_dodge_right", "Roulade à droite", 0x54 },
    };
    for (const auto& dodge : kDodges) {
        const uint8_t action = dodge.action;
        Add({ .id = dodge.id, .name = dodge.name,
              .help = "Chris fait cette esquive tout de suite, quelle que soit sa direction de déplacement. Le curseur : son "
                      "coût avec « Endurance stricte » (tout à gauche, « blocage seul » : elle arrête seulement le remplissage).",
              .category = "Dodge", .kind = Kind::Action,
              .run = [action] { return PlayFixedDodge(action); },
              .available = [] { return actors::InGame(); },
              .value = 200, .valueMin = -100, .valueMax = 2000, .valueFormat = "%.0f pts (stricte)", .negativeText = "blocage seul" });
    }
    Feature& cooldown = Add({ .id = "melee.dodge_cooldown", .name = "Esquives : temps de recharge",
                              .help = "Temps minimum entre deux esquives du trainer (esquive selon la direction, esquive en "
                                      "visant, esquives à touche fixe) : une esquive trop tôt est refusée, celle en cours n'est "
                                      "plus coupée. 0 = sans attente.",
                              .category = "Dodge", .kind = Kind::Toggle,
                              .set = [](bool on) {
                                  if (!on) g_dodgeCooldown = 0.0f;  // unchecked: no wait
                                  return true;
                              },
                              .value = 1.5f, .valueMin = 0.0f, .valueMax = 5.0f, .valueFormat = "%.1f s",
                              .defaultOn = true });
    {
        Feature* const fp = &cooldown;
        cooldown.tick = [fp] { g_dodgeCooldown.store(fp->value, std::memory_order_relaxed); };
    }
    AddPanel("Mêlée", DrawMeleePanel,
             { "Mêlée", "Mêlée libre", "Mêlée libre Chris", "Mêlée libre Sheva", "Mêlée libre Jill", "Mêlée libre Wesker", "Mêlée libre Josh",
               "Mêlée libre Barry", "Mêlée libre Rebecca", "Mêlée libre Excella", "Mêlée libre Leon",
               "Mêlée libre Leon Remake", "Mêlée libre HUNK", "Dodge", "Dodge par personnage", "Automatisation", "Test RE6",
               "Mêlée libre Leon RE6", "Mêlée libre Helena RE6", "Mêlée libre Jake RE6", "Mêlée libre Ada RE6",
               "Mêlée libre Chris RE6", "Mêlée libre Piers RE6", "Mêlée libre Sherry RE6", "Mêlée libre RE6", "Réglages RE6", "RE6 · Chris", "RE6 · Sheva", "RE6 · Jill", "RE6 · Wesker",
               "RE6 · Josh", "RE6 · Excella", "RE6 · Barry", "RE6 · Rebecca",
               // per_character.cpp: the swap page per character, the originals hidden
               "Mêlée · Chris", "Mêlée · Sheva", "Mêlée · Jill", "Mêlée · Wesker", "Mêlée · Josh", "Mêlée · Excella", "Mêlée · Barry",
               "Mêlée · Rebecca", "Mêlée · Leon", "Mêlée · Helena", "Mêlée · Piers", "Mêlée · Jake", "Mêlée · Sherry", "Mêlée · Ada",
               "Mêlée · Agent", "Mêlée (global)" },
             true);
}
}
