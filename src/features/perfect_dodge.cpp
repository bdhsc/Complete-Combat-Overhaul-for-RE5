// "Esquive parfaite" (user, 2026-10-29: "un curseur de timing comme pour les parades"): an enemy attack that would reach
// player 1 within the slider's seconds after one of the trainer's dodges was asked (swaps.cpp DodgeAsked:
// NoteDodgeStarted) is dodged: a sound (one of Wesker's of RE4 Remake, sounds\wesker\W<n>.wav) and, if player 1 aims
// soon after it, a slow motion (x0.25 for 4 s by default, time_speed.cpp StartDodgeSlowMotion) and his aim turned to the
// dodged one (his facing, P+0x1024, the aim yaw P+0x2A10 and the aim camera's P+0x2A24, as the RE6 quick shot of
// quick_knife.cpp) in 0.2 s, once, the mouse free again after that; the slow motion starts with that aim only.
// The hit shapes: every unit has a list at +0x1290 (vtable RVA 0xC413E8; +4 shapes, +0x14 capacity, +0x18 count) of
// hurt shapes (type +0xC4 == 1: 13 on a Majini or Chris, on his head, torso, arms and legs, their world box +0x10..+0x28
// following the body) and attack shapes (type 0: 11 on a Majini, all off and boxless out of its blows); +0xBD is a
// shape's "on" (cleared shapes leave the collision world, VA 0xC485A0). An attack's shapes (sampled 2026-10-29 on a
// Majini's lunge 0x97: its four arm shapes 13..16) have +0xBE = 1 and their box for the whole blow (about 0.2 s), their
// +0xBD only for a moment of each frame (the collision test: seen at 1 one sample in five), so the sphere reads +0xBE
// (the first version read +0xBD and never caught one: user, "les ennemis ne la détectent pas"). The game's own dodges (its avoid actions
// 0x4F..0x55: avoid_crouch_in / _out, _front, _back, _left, _right, _side) make him invulnerable that way: their init
// (cPlAction_pl_act_avoid_front VA 0xC8E6A0) calls VA 0xB5C870(0, 0), which clears his hurt shapes' +0xBD through VA
// 0xC46C70 (the store the Ultimate Trainer took out for its "successive dodges", RVA 0x846C9D).
// A first version let his hurt shapes take the blow and refused it in his reaction: the blow still connected (user:
// its impact sound, a Majini's bottle broke on him, "pas très convaincant"). The user's idea instead: "une sphère de
// collision sur Chris", touched before him, him invincible meanwhile. So during the window his hurt shapes are off and
// hidden from the enemies as by the "Invincible" option (player.cpp SetDodgeInvincible: their number 0; off alone, the
// game switched them back during its dodges and the blow still sounded on him: "insensible, pas invincible") - no
// contact at all, a borrowed dodge included - and the trainer looks for an enemy's attack shape that is on and whose box
// meets a box around him (the slider's radius around his position, 20 cm below his feet to 2 m up; user, 2026-10-29:
// the strikes were not caught, "agrandis la sphère"), or whose held weapon's attack shape does (a Majini's axe,
// sickle or bottle is its own unit, +0x1B6C; the units an enemy points to that have a shape list - AttachedUnits - for
// the other enemies' weapons; the first version looked at the enemies alone and caught only their lunges and grabs,
// arm shapes 13 / 14): that attack is dodged. A roll that
// carries him out of the box before the attack is a plain dodge.
// "Esquives du jeu : invincible seulement sur une esquive parfaite" (user, 2026-10-29: the Quick Duck dodged the blows
// coming down, "ça m'obligerait à esquiver avec les directions dédiées"; on by default): after the window the trainer's
// game dodges get his hurt shapes back on, a blow lands; after a perfect dodge they stay off for the rest of the dodge.
// The game's own dodges (its prompts: no trainer dodge asked just before) are left alone. A blow that reaches him in
// the window anyway (a grab...) is a perfect dodge too (player.cpp asks PerfectDodgeHit), without blood
// (PerfectDodgeSparesBlood, quick_knife.cpp's blood hook, as the parries).
// The automatic responses (user, 2026-10-29: "5 options d'automatisation qui utiliseront aussi la sphère"), any enemy
// attack in the sphere (or a bullet / rocket) answered by the trainer, out of a dodge or a riposte already playing:
// - "Esquive automatique" (its own style): a dodge to the side away from the attacker (or the direction held), Chris
//   invincible from the request to its end (before it starts too: kAutoGraceMs);
// - "Esquive parfaite automatique": the same with the perfect dodge's effects (its sound, slow motion and aim);
// - "Parade automatique" (knife slash, Wesker's guard or Jill's throw): the knife parry's window opened at each frame
//   the attack is in the sphere (parry.cpp AutoParryPress), the parry taking the blow as after a press; never with a
//   broken / no knife for the slash (user);
// - "Contre RE6 automatique": the RE6 parry's window the same way (AutoRe6Press);
// - "Esquive automatique des roquettes et des balles" (its own style): an enemy rocket within kRocketReach of his chest
//   (rocket_catch.cpp IncomingRocket), or a bullet that reached him (refused in PerfectDodgeHit, its blood spared,
//   then the dodge), a dodge to the side away from it.
// First the parries, then the dodges.
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "common/wav.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace features {
uintptr_t HitAttacker(uintptr_t hit);                  // parry.cpp: a hit's attacker
const char* UnitClassName(uintptr_t object);           // parry.cpp
bool EnsurePlayerDamageHook();                         // player.cpp
bool EnsurePlayerHitReactionHook();                    // player.cpp
bool EnsureCommonEffectsHook();                        // quick_knife.cpp: the hit blood spawner VA 0x7757F0
bool CameraRay(float* eye, float& yaw, float& pitch);  // movement.cpp: the normal / aim camera
void StartDodgeSlowMotion(float speed, float seconds, bool playerKeepsSpeed);  // time_speed.cpp
bool SetDodgeInvincible(bool on);  // player.cpp: his shapes' number 0, as the "Invincible" option (no contact at all)
std::vector<std::string> DodgeStyleNames();   // swaps.cpp
bool PlayAutoDodge(int style, uint8_t dodge);  // swaps.cpp: a style's dodge (0x51 forward ... 0x54 right)
uint8_t HeldDodgeDirection();                  // swaps.cpp: the dodge of the direction held, 0x50 none
bool SetAutoParry(bool knife, bool re6);       // parry.cpp
bool AutoParryPress(int kind);                 // parry.cpp: the knife parry's window, its riposte `kind`
bool AutoRe6Press();                           // parry.cpp
bool ParriedWithin(DWORD ms);                  // parry.cpp
bool EnsureRocketTimeHooks();                  // rocket_catch.cpp: the rockets' updates followed
bool IncomingRocket(float* at, float& distance);  // rocket_catch.cpp
bool StaminaAllows();                          // stamina.cpp
bool ParrySparesBlood(uintptr_t player);       // parry.cpp: a parry's window open (the parry takes the blow)
bool Player1BlowTaken(uintptr_t attacker);     // parry.cpp: that enemy's blow just taken by player 1
bool Player1InNeckBreak();                     // wesker_moves.cpp: his neck broken from behind (no dodge)

namespace {
constexpr uint32_t kBlastHit = 0x00200000;  // blasts and fire (parry.cpp)
constexpr DWORD kSameBlowMs = 400;          // the dodged blow's other call (its SubHealth / reaction), or another at once
constexpr DWORD kTurnMs = 200;
constexpr int kWeskerSounds = 20;           // research\re4r_snd\wesker\W1..W20
constexpr uint32_t kCarrier = 0x81;         // the borrowed moves' action (wesker_moves.cpp)
constexpr uintptr_t kShapeListVtable = 0xC413E8;  // (RVA)
constexpr float kSphereHeight = 200.0f, kSphereBelow = 20.0f;  // cm, from his feet
constexpr float kSearch = 600.0f;           // enemies looked at within (cm)

std::atomic<bool> g_on{ false };
std::atomic<float> g_window{ 0.3f };        // seconds after the dodge
std::atomic<DWORD> g_dodgeTick{ 0 };        // the last dodge asked, 0 = none (or already used)
std::atomic<DWORD> g_dodgedTick{ 0 };       // the last perfect dodge
std::atomic<uintptr_t> g_dodged{ 0 };       // its enemy, for the aim
std::atomic<bool> g_eventPending{ false };  // its sound and slow motion, started by the render thread
std::atomic<int16_t> g_lastHealth{ 0 };     // player 1's health at the last frame
// 0 none, n = Wn. 0 at first: the saved choice is applied at the start unless it is 0 (index 0 is never re-applied,
// so a starting 12 played W12 again after a reload with "Aucun" saved; user, 2026-10-29).
std::atomic<int> g_sound{ 0 };
std::atomic<float> g_gain{ 2.0f };
std::atomic<bool> g_slowOn{ true };
std::atomic<float> g_slowSpeed{ 0.25f }, g_slowSeconds{ 4.0f };
std::atomic<bool> g_slowPlayer{ false };
std::atomic<bool> g_aimOn{ true };
std::atomic<float> g_aimWindow{ 1.5f };
std::atomic<float> g_sphereRadius{ 80.0f };   // cm ("Esquive parfaite : taille de la sphère")
// The automatic dodges' reach (user, 2026-10-30: "Chris réagit après l'impact, on dirait pas vraiment qu'il esquive"):
// an enemy within it, facing player 1, that starts a melee attack is dodged at once, at the attack's start (its wind-up);
// a weapon's attack shape is armed only in the frames of the blow itself, so the sphere saw it at the impact (a larger
// sphere changed nothing). The armed shapes in the perfect dodge's sphere remain a late fallback.
std::atomic<float> g_autoSphereRadius{ 170.0f };  // user: a larger sphere dodged blows that would have missed
// Each enemy's action and layer 0 motion seen last frame (render thread): a new attack is a new action, or the same
// attack action on another motion or its motion started again (user, 2026-10-30: chained blows were missed).
struct SeenAction {
    uintptr_t unit = 0;
    uint32_t action = 0, motion = 0;
    float frame = 0.0f;
};
std::vector<SeenAction> g_lastEnemyAction;
// The dodge a little after the attack's start (user, 2026-10-30: "presque trop tôt, entre maintenant et avant"), player
// 1 hidden from the blows from the start already (AutoPending: g_delayedPending).
std::atomic<float> g_autoDelay{ 0.15f };  // s
// The Wesker style's own, later (user, 2026-10-30: his dodges bring him back where he stood, unlike the rolls; started
// too early he came back right into the blow): its index in the styles (DodgeStyleNames, "Dodge Wesker").
std::atomic<float> g_autoDelayWesker{ 0.3f };  // s
std::atomic<int> g_weskerStyle{ -1 };
constexpr float kAutoDelayMax = 0.6f;
struct DelayedDodge {
    uintptr_t attacker = 0;
    DWORD at = 0;
    int style = 0;
    char how[64] = {};
};
DelayedDodge g_delayed;  // render thread
std::atomic<bool> g_delayedPending{ false };  // g_delayed set (read by the game thread too)
std::atomic<bool> g_strict{ true };           // the trainer's game dodges invulnerable only after a perfect dodge
std::atomic<bool> g_perfectInDodge{ false };  // a perfect dodge in the dodge in progress
// An automatic dodge asked or playing: player 1 hidden from the blows from its request to its end (user, 2026-10-29: the
// blood and the impact were still there, "invincible, pas insensible": hidden only from the next frame and for the 0.4 s
// grace once the dodge had started - the frames between its request and its start cleared g_perfectInDodge).
std::atomic<bool> g_autoInDodge{ false };
std::atomic<bool> g_ownDodge{ false };        // player 1 in a dodge the trainer asked (render thread writes)
std::atomic<DWORD> g_lastAsk{ 0 };            // the last dodge asked (kept: tells the trainer's dodges from the game's)
bool g_inDodge = false;   // render thread: player 1 in a dodge action (avoid or a borrowed move)
int g_written = -1;       // render thread: the hurt shapes' state the trainer holds (-1 none)
wav::Sound g_wave;        // render thread
DWORD g_turnStart = 0;    // render thread: the turn in progress
float g_turnFrom = 0.0f;
bool g_outOfAim = false;  // render thread: player 1 seen out of the aim since the perfect dodge (the aim after it counts)
bool g_armed = false;     // render thread: a perfect dodge waits for player 1's aim (slow motion, turn)
constexpr uint32_t kBulletHit = 0x00080000;  // a bullet's hit (parry.cpp)
constexpr DWORD kAutoGraceMs = 400;          // invincible from an automatic dodge's request until it plays
constexpr float kRocketReach = 700.0f;       // cm from his chest
std::atomic<bool> g_autoDodge{ false }, g_autoPerfect{ false }, g_autoParry{ false }, g_autoRe6{ false }, g_autoShots{ false };
std::atomic<int> g_autoDodgeStyle{ 0 }, g_autoShotStyle{ 0 }, g_autoParryKind{ 0 };
std::atomic<DWORD> g_autoTick{ 0 };          // an automatic dodge asked
std::atomic<DWORD> g_autoNext{ 0 };          // no new automatic dodge before
// Two automatic dodges at least this far apart, one of them cut by the next (user, 2026-10-30: "il n'esquive pas le
// suivant car il est déjà dans l'anim"; was 600 ms and nothing during a dodge): enough for a dodge asked to start.
constexpr DWORD kAutoGapMs = 250;
std::atomic<DWORD> g_rocketNext{ 0 };        // the rocket dodged: not again before
std::atomic<uintptr_t> g_shotFrom{ 0 };      // a bullet refused (game thread): its shooter, dodged at the next frame
std::atomic<DWORD> g_shotTick{ 0 };
std::atomic<uintptr_t> g_grabFrom{ 0 };      // an easier grab refused (grabs.cpp): its Majini, dodged at the next frame
// A blow that reached player 1 anyway with an automatic dodge ready (user, 2026-10-30: "toujours le sang et le son, tu
// rends Chris insensible mais pas invincible"): refused in his reaction as a parry refuses it (no reaction, no impact nor
// pain sound), his blood spared (the blood comes about 9 ms before the reaction: spared while a dodge is ready), and its
// attacker dodged at the next frame.
std::atomic<uintptr_t> g_blowFrom{ 0 };
// parry.cpp: a bolt dodged before its contact (TickBoltDodge): player 1 hidden from the blows until then, so it flies
// through him without breaking (user, 2026-10-30).
std::atomic<DWORD> g_projectileHideUntil{ 0 };
bool ProjectileHidden(DWORD now) {
    const DWORD until = g_projectileHideUntil.load();
    return until && static_cast<int32_t>(until - now) > 0;
}

// "Invincible tout court" (user, 2026-10-30: "vu que tu utilises des sphères de détection, tu peux rendre Chris
// invincible tant que l'option est activée"): with the automatic dodge (or perfect one) on, player 1 is hidden from the
// blows as by "Invincible" the whole time (his shapes' number 0) and any damage refused; the attacks are seen by their
// start and the sphere, never by their contact.
bool AlwaysInvincible() {
    return g_autoDodge.load(std::memory_order_relaxed) || g_autoPerfect.load(std::memory_order_relaxed);
}

bool AutoDodgeOn() {
    return g_autoDodge.load(std::memory_order_relaxed) || g_autoPerfect.load(std::memory_order_relaxed) ||
           g_autoShots.load(std::memory_order_relaxed);
}

// The dodges' machinery runs for the perfect dodge or an automatic dodge.
bool EngineOn() { return g_on.load(std::memory_order_relaxed) || AutoDodgeOn(); }

DWORD Elapsed(DWORD tick, DWORD now);  // below

bool AutoPending(DWORD now) {
    const DWORD tick = g_autoTick.load();
    return (tick && Elapsed(tick, now) < kAutoGraceMs) || g_delayedPending.load();
}
std::string g_soundError;

DWORD Elapsed(DWORD tick, DWORD now) { return static_cast<int32_t>(now - tick) > 0 ? now - tick : 0; }

bool WindowOpen(DWORD now) {
    const DWORD asked = g_dodgeTick.load();
    return asked && Elapsed(asked, now) <= static_cast<DWORD>(g_window.load(std::memory_order_relaxed) * 1000.0f);
}

float Wrap(float a) {
    constexpr float kPi = 3.14159265f;
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

float FacingYaw(uintptr_t p) {
    float q[4]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x40), q, sizeof(q))) return 0.0f;
    return Wrap(2.0f * std::atan2(q[1], q[3]));
}

void FaceYaw(uintptr_t p, float yaw) {
    yaw = Wrap(yaw);
    mem::Write(p + 0x2A10, yaw);
    mem::Write(p + 0x1024, yaw);
    mem::Write(p + 0x2A24, yaw);
    const float quat[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x40), quat, sizeof(quat));
}

// The aim actions (firearm 0x1B / 0x1C / 0x1E, knife 0x22..0x27 but 0x24), as the aim dodge.
bool Aiming(uint32_t action) {
    return action == 0x1B || action == 0x1C || action == 0x1E || (action >= 0x22 && action <= 0x27 && action != 0x24);
}

// The game's dodges (avoid actions).
bool AvoidAction(uint32_t action) { return action >= 0x4F && action <= 0x55; }

// A unit's shape list: its shapes and their number; false if it has none.
bool ShapeList(uintptr_t unit, uintptr_t& shapes, uint32_t& count) {
    uintptr_t vtable = 0;
    uint32_t capacity = 0;
    if (!mem::Read(unit + 0x1290, vtable) || vtable != reinterpret_cast<uintptr_t>(mem::Rva(kShapeListVtable)) ||
        !mem::Read(unit + 0x1290 + 4, shapes) || !shapes || !mem::Read(unit + 0x1290 + 0x14, capacity) ||
        !mem::Read(unit + 0x1290 + 0x18, count))
        return false;
    count = std::min({ capacity, count, 64u });
    return true;
}

// Player 1's hurt shapes on (1) or off (0), as VA 0xC46C70 sets them.
void SetHurtShapes(uintptr_t p, uint8_t on) {
    uintptr_t shapes = 0;
    uint32_t count = 0;
    if (!ShapeList(p, shapes, count)) return;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t shape = 0;
        uint32_t type = 0;
        if (mem::Read(shapes + 4 * i, shape) && shape && mem::Read(shape + 0xC4, type) && type == 1)
            mem::Write<uint8_t>(shape + 0xBD, on);
    }
}

// The trainer lets go of his hurt shapes: as the game wants them now (off in a game dodge, on otherwise); still hidden
// from the blows with `keepHidden` (AlwaysInvincible).
void ReleaseHurtShapes(uintptr_t p, uint32_t action, bool keepHidden = false) {
    SetDodgeInvincible(keepHidden);
    if (g_written < 0) return;
    if (p && g_written == 0 && !AvoidAction(action)) SetHurtShapes(p, 1);
    if (p && g_written == 1 && AvoidAction(action)) SetHurtShapes(p, 0);
    g_written = -1;
}

// The option off or the trainer unloaded during a dodge.
void ReleaseHurtShapesNow() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (p) mem::Read(p + 0x10E0, action);
    ReleaseHurtShapes(p, action);
    g_ownDodge = false;
    g_inDodge = false;
}

bool EnemyAlive(uintptr_t enemy) {
    int16_t health = 0;
    const auto enemies = actors::Enemies();
    return enemy && std::find(enemies.begin(), enemies.end(), enemy) != enemies.end() && mem::Read(enemy + 0x1364, health) &&
           health > 0;
}

// The "sphere" (a box around player 1): the first enemy whose attack shape is on and meets it.
// One of `unit`'s attack shapes armed (+0xBE) whose box meets [lo, hi]: its index, else -1.
int ArmedShapeIn(uintptr_t unit, const float* lo, const float* hi) {
    uintptr_t shapes = 0;
    uint32_t count = 0;
    if (!unit || !ShapeList(unit, shapes, count)) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t shape = 0;
        uint32_t type = 1;
        uint8_t on = 0;
        float box[6]{};
        if (!mem::Read(shapes + 4 * i, shape) || !shape || !mem::Read(shape + 0xC4, type) || type != 0 ||
            !mem::Read(shape + 0xBE, on) || !on || !mem::SafeRead(reinterpret_cast<void*>(shape + 0x10), box, 12) ||
            !mem::SafeRead(reinterpret_cast<void*>(shape + 0x20), box + 3, 12))
            continue;
        bool meets = true;
        for (int c = 0; c < 3; ++c)
            meets = meets && std::isfinite(box[c]) && std::isfinite(box[c + 3]) && box[c] <= box[c + 3] &&
                    box[c + 3] - box[c] < 1000.0f && box[c] <= hi[c] && box[c + 3] >= lo[c];
        if (meets) return static_cast<int>(i);
    }
    return -1;
}

// The units an enemy points to that have a shape list of their own (its weapons: a Majini's held weapon +0x1B6C,
// shape 0 its blow, radius 25-40; user, 2026-10-29: "le bourreau, la Gatling, la tronçonneuse ont des armes aussi"),
// looked for in its first kAttachedScan bytes, neither players nor enemies; again every kAttachedMs (render thread).
constexpr size_t kAttachedScan = 0x3000;
constexpr DWORD kAttachedMs = 1000;
struct Attached {
    uintptr_t enemy = 0;
    DWORD tick = 0;
    std::vector<uintptr_t> units;
};
std::vector<Attached> g_attached;

const std::vector<uintptr_t>& AttachedUnits(uintptr_t enemy, uintptr_t p, const std::vector<uintptr_t>& enemies, DWORD now) {
    Attached* entry = nullptr;
    for (Attached& a : g_attached)
        if (a.enemy == enemy) entry = &a;
    if (entry && Elapsed(entry->tick, now) < kAttachedMs) return entry->units;
    if (!entry) entry = &g_attached.emplace_back();
    entry->enemy = enemy;
    entry->tick = now ? now : 1;
    entry->units.clear();
    static uint32_t words[kAttachedScan / 4];
    if (!mem::SafeRead(reinterpret_cast<void*>(enemy), words, sizeof(words))) return entry->units;
    const uintptr_t listVtable = reinterpret_cast<uintptr_t>(mem::Rva(kShapeListVtable));
    for (uint32_t w : words) {
        const uintptr_t unit = w;
        uintptr_t vtable = 0;
        // Units are 16-byte aligned heap blocks (the alignment also skips most floats, unmapped as addresses).
        if (unit < 0x10000 || (unit & 0xF) || unit == enemy || unit == p || unit == actors::Player(1) ||
            std::find(entry->units.begin(), entry->units.end(), unit) != entry->units.end() ||
            !mem::SafeRead(reinterpret_cast<void*>(unit + 0x1290), &vtable, sizeof(vtable)) || vtable != listVtable ||
            std::find(enemies.begin(), enemies.end(), unit) != enemies.end())
            continue;
        entry->units.push_back(unit);
    }
    return entry->units;
}

// A Majini's parasite out (host +0x1F9C): its shapes are armed while it moves alone, no attack action (user,
// 2026-10-30: near one the automatic dodges looped "sans fin", Chris unable to move with the Wesker style).
bool IsParasiteOf(uintptr_t unit, const std::vector<uintptr_t>& enemies) {
    for (uintptr_t host : enemies) {
        uintptr_t plaga = 0;
        if (mem::Read(host + 0x1F9C, plaga) && plaga && plaga == unit) return true;
    }
    return false;
}

// The "sphere" (a box around player 1): the first enemy whose attack shape, or one of its attached units' (weapons),
// is armed and meets it; the parasites out left aside without `parasites`. An attached unit is the enemy's while it
// holds it (its parent +0x18, joint +0x1C) or once thrown (no parent, joint -1: a bottle, an axe in flight, then
// `thrown`; user, 2026-10-31: a bottle parried with Jill's throw brought its Majini from 5 m); one held by another enemy
// (the pointer an enemy keeps to a weapon it met) is that enemy's, not this one's.
uintptr_t AttackInSphere(uintptr_t p, int& shapeIndex, bool& weapon, float r, bool parasites = true,
                         uintptr_t* thrown = nullptr) {
    if (thrown) *thrown = 0;
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return 0;
    const float lo[3] = { pos[0] - r, pos[1] - kSphereBelow, pos[2] - r };
    const float hi[3] = { pos[0] + r, pos[1] + kSphereHeight, pos[2] + r };
    const auto enemies = actors::Enemies();
    const DWORD now = GetTickCount();
    if (g_attached.size() > 64) g_attached.clear();  // enemies gone
    for (uintptr_t e : enemies) {
        float ep[3]{};
        int16_t health = 0;
        if (!mem::Read(e + 0x1364, health) || health <= 0 || !mem::SafeRead(reinterpret_cast<void*>(e + 0x30), ep, sizeof(ep)) ||
            std::hypot(ep[0] - pos[0], ep[2] - pos[2]) > kSearch || (!parasites && IsParasiteOf(e, enemies)))
            continue;
        if ((shapeIndex = ArmedShapeIn(e, lo, hi)) >= 0) {
            weapon = false;
            return e;
        }
        uintptr_t plaga = 0;
        if (!parasites) mem::Read(e + 0x1F9C, plaga);
        for (uintptr_t unit : AttachedUnits(e, p, enemies, now)) {
            uintptr_t parent = 0;
            if (!mem::Read(unit + 0x18, parent) || (parent && parent != e && std::find(enemies.begin(), enemies.end(), parent) != enemies.end()))
                continue;
            if ((!plaga || unit != plaga) && (shapeIndex = ArmedShapeIn(unit, lo, hi)) >= 0) {
                weapon = true;
                if (thrown && !parent) *thrown = unit;
                return e;
            }
        }
    }
    return 0;
}

// The name of a unit's action after its "Act" ("" if none): cEm10ActAtkOneHand -> AtkOneHand.
const char* ActionTail(uintptr_t unit, uint32_t action) {
    uint32_t count = 0;
    uintptr_t table = 0, object = 0;
    if (!mem::Read(unit + 0x10D0, count) || action >= count || count > 0x200 || !mem::Read(unit + 0x10DC, table) || !table ||
        !mem::Read(table + 4 * action, object) || !object)
        return "";
    const char* name = UnitClassName(object);
    const char* tail = name ? std::strstr(name, "Act") : nullptr;
    return tail ? tail + 3 : "";
}

// A melee attack by its action's name: "Atk..." (cEm10ActAtkOneHand, AtkKick, AtkDashBothHand, the Executioner's
// AtkTate...) or a catch, not the shots, bows, throws and bombs (the bullets / rockets have their own option).
bool MeleeAttackName(const char* tail) {
    if (std::strncmp(tail, "Catch", 5) == 0 || std::strncmp(tail, "DashCatch", 9) == 0) return true;
    if (std::strncmp(tail, "Atk", 3) != 0) return false;
    for (const char* ranged : { "Gun", "Bow", "Throw", "Bomb", "Missile" })
        if (std::strstr(tail, ranged)) return false;
    return true;
}

// A shot at player 1 (user, 2026-10-30: the bullets were dodged, but after their blood and impact): a gun's shot, a
// bowgun's or a bow's, not the warning shots nor the aims and reloads.
bool ShotName(const char* tail) {
    if (std::strncmp(tail, "Atk", 3) != 0 || std::strstr(tail, "Warn") || std::strstr(tail, "Reload")) return false;
    return std::strstr(tail, "GunShot") || std::strstr(tail, "Bowgun") || std::strcmp(tail, "AtkBow") == 0;
}
constexpr float kShotReach = 3000.0f;   // cm
constexpr float kShotCone = 0.35f;      // rad: aimed at him

// The first enemy within `reach` (cm, on the ground), facing player 1 (within 70 degrees), that entered a melee attack
// since the last frame, and in `shooter` the first one that started a shot aimed at him; every enemy's action
// remembered.
uintptr_t AttackStarting(uintptr_t p, float reach, uintptr_t* shooter) {
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return 0;
    std::vector<SeenAction> seen;
    uintptr_t starting = 0;
    if (shooter) *shooter = 0;
    for (uintptr_t e : actors::Enemies()) {
        uint32_t action = 0;
        int16_t health = 0;
        float ep[3]{};
        if (!mem::Read(e + 0x10E0, action) || !mem::Read(e + 0x1364, health) || health <= 0 ||
            !mem::SafeRead(reinterpret_cast<void*>(e + 0x30), ep, sizeof(ep)))
            continue;
        uint32_t motion = 0;
        float frame = 0.0f;
        mem::Read(e + 0x384, motion);
        mem::Read(e + 0x398, frame);
        motion &= 0xFFFF;
        seen.push_back({ e, action, motion, frame });
        const auto last = std::find_if(g_lastEnemyAction.begin(), g_lastEnemyAction.end(),
                                       [e](const SeenAction& entry) { return entry.unit == e; });
        if (last != g_lastEnemyAction.end() && last->action == action && last->motion == motion && frame + 2.0f >= last->frame)
            continue;  // not a new attack
        const float dx = pos[0] - ep[0], dz = pos[2] - ep[2];
        const char* tail = ActionTail(e, action);
        if (shooter && !*shooter && ShotName(tail) && std::hypot(dx, dz) <= kShotReach &&
            std::fabs(Wrap(std::atan2(dx, dz) - FacingYaw(e))) <= kShotCone)
            *shooter = e;
        if (starting) continue;
        const float within = std::strstr(tail, "Dash") ? 2.0f * reach : reach;  // a charge starts from further
        if (std::hypot(dx, dz) > within || std::fabs(pos[1] - ep[1]) > kSphereHeight) continue;
        if (std::fabs(Wrap(std::atan2(dx, dz) - FacingYaw(e))) > 1.22f) continue;  // not towards him
        if (MeleeAttackName(tail)) starting = e;
    }
    g_lastEnemyAction.swap(seen);
    return starting;
}

// A perfect dodge of `attacker`'s attack.
void Dodged(uintptr_t attacker, DWORD now, const char* how) {
    const DWORD asked = g_dodgeTick.exchange(0);  // one per dodge
    g_dodgedTick = now ? now : 1;
    g_perfectInDodge = true;
    g_dodged = attacker && EnemyAlive(attacker) ? attacker : 0;
    g_eventPending = true;
    LOG("perfect dodge: %s %08X %s, %u ms after the dodge", attacker ? UnitClassName(attacker) : "rocket",
        static_cast<unsigned>(attacker), how, asked ? Elapsed(asked, now) : 0);
}

void PlayDodgeSound() {
    const int n = g_sound.load(std::memory_order_relaxed);
    if (n <= 0 || n > kWeskerSounds) return;
    const std::wstring file = L"wesker\\W" + std::to_wstring(n) + L".wav";
    std::string error;
    if (g_wave.Load(file.c_str(), g_gain.load(std::memory_order_relaxed), error)) {
        g_wave.Play();
        g_soundError.clear();
    } else if (error != g_soundError) {
        g_soundError = error;
        LOG("perfect dodge: %s", error.c_str());
        Toast(i18n::Tr("Esquive parfaite : ") + error, false);
    }
}

// After a perfect dodge, the first frame player 1 aims (out of the aim first) within the aim window: the slow motion
// starts (user, 2026-10-29: "le ralenti ne doit se déclencher que si je vise, comme le recentrage") and his aim turns
// to the dodged enemy in kTurnMs (smoothstep, the enemy followed), from the camera eye once the camera faces it (the
// shoulder offset, as the quick shot).
void TickAim(uintptr_t p, DWORD now) {
    if (g_armed) {
        uint32_t action = 0;
        if (Elapsed(g_dodgedTick.load(), now) > static_cast<DWORD>(g_aimWindow.load(std::memory_order_relaxed) * 1000.0f)) {
            g_armed = false;
            g_dodged = 0;
            return;
        }
        if (!mem::Read(p + 0x10E0, action)) return;
        if (!Aiming(action)) g_outOfAim = true;
        if (!g_outOfAim || !Aiming(action)) return;
        g_armed = false;
        const uintptr_t enemy = g_dodged.load(std::memory_order_relaxed);
        LOG("perfect dodge: aiming after it%s%s", g_slowOn.load() ? ", slow motion" : "",
            g_aimOn.load() && EnemyAlive(enemy) ? ", turned to the enemy" : "");
        if (g_slowOn.load(std::memory_order_relaxed))
            StartDodgeSlowMotion(g_slowSpeed.load(std::memory_order_relaxed), g_slowSeconds.load(std::memory_order_relaxed),
                                 g_slowPlayer.load(std::memory_order_relaxed));
        if (g_aimOn.load(std::memory_order_relaxed) && EnemyAlive(enemy)) {
            g_turnStart = now ? now : 1;
            g_turnFrom = FacingYaw(p);
        } else {
            g_dodged = 0;
        }
    }
    const uintptr_t enemy = g_dodged.load(std::memory_order_relaxed);
    if (!g_turnStart || !enemy) {
        g_turnStart = 0;
        return;
    }
    float e[3]{}, pos[3]{};
    if (!EnemyAlive(enemy) || !mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), e, sizeof(e)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) {
        g_dodged = 0;
        g_turnStart = 0;
        return;
    }
    float yaw = std::atan2(e[0] - pos[0], e[2] - pos[2]);
    float eye[3]{}, camera = 0, pitch = 0;
    if (CameraRay(eye, camera, pitch) && std::fabs(Wrap(camera - yaw)) < 0.5f) yaw = std::atan2(e[0] - eye[0], e[2] - eye[2]);
    const float t = std::min(1.0f, static_cast<float>(Elapsed(g_turnStart, now)) / kTurnMs);
    FaceYaw(p, g_turnFrom + Wrap(yaw - g_turnFrom) * t * t * (3.0f - 2.0f * t));
    if (t >= 1.0f) {
        g_dodged = 0;
        g_turnStart = 0;
    }
}

// The side away from `from` (or the direction held but forward: running at the enemy rolled into it): 0x53 left, 0x54
// right.
uint8_t DodgeAway(uintptr_t p, const float* from) {
    const uint8_t held = HeldDodgeDirection();
    if (held != 0x50 && held != 0x51) return held;
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return 0x53;
    const float yaw = FacingYaw(p), dx = from[0] - pos[0], dz = from[2] - pos[2];
    const float left = dx * std::cos(yaw) - dz * std::sin(yaw);  // its side, + = his left (as DodgeOfDirection)
    return left > 0.0f ? 0x54 : 0x53;
}

// An automatic dodge of `attacker` (0: a rocket) coming from `from`: invincible through it, the perfect dodge's
// effects with "Esquive parfaite automatique".
bool AutoDodge(uintptr_t p, uintptr_t attacker, const float* from, int style, DWORD now, const char* what) {
    const uint8_t dodge = DodgeAway(p, from);
    if (!PlayAutoDodge(style, dodge)) return false;
    g_autoTick = now ? now : 1;
    g_autoNext = now + kAutoGapMs;
    g_autoInDodge = true;
    // Hidden at once: the blow in the sphere may touch him in this very frame.
    SetHurtShapes(p, 0);
    g_written = 0;
    SetDodgeInvincible(true);
    if (g_autoPerfect.load(std::memory_order_relaxed)) {
        Dodged(attacker, now, what);
    } else {
        g_dodgeTick = 0;  // not a perfect dodge
        g_perfectInDodge = true;  // invincible for the rest of it
        LOG("auto dodge: %X from %s %08X (%s)", dodge, attacker ? UnitClassName(attacker) : "rocket",
            static_cast<unsigned>(attacker), what);
    }
    return true;
}

// The automatic responses, each frame (render thread), out of a riposte. In a dodge the attacks that start and the
// bullets are dodged again (the new dodge cuts the one playing, kAutoGapMs after it); the parries and the late
// fallbacks wait for its end.
void TickAuto(uintptr_t p, uint32_t action, bool dodging, DWORD now) {
    const bool parries = g_autoParry.load(std::memory_order_relaxed) || g_autoRe6.load(std::memory_order_relaxed);
    if ((!parries && !AutoDodgeOn()) || Player1InNeckBreak()) return;
    if (ParriedWithin(1500)) return;
    const int shotStyle = g_autoShotStyle.load(std::memory_order_relaxed);
    float from[3]{}, distance = 0.0f;
    // A bullet refused by PerfectDodgeHit (him invincible meanwhile: AutoPending): its dodge now.
    if (const uintptr_t shooter = g_shotFrom.exchange(0); shooter && g_shotTick.exchange(0) &&
                                                          mem::SafeRead(reinterpret_cast<void*>(shooter + 0x30), from, sizeof(from))) {
        AutoDodge(p, shooter, from, shotStyle, now, "bullet");
        return;
    }
    // A blow refused (PerfectDodgeHit): its dodge now.
    if (const uintptr_t striker = g_blowFrom.exchange(0);
        striker && mem::SafeRead(reinterpret_cast<void*>(striker + 0x30), from, sizeof(from))) {
        AutoDodge(p, striker, from, g_autoDodgeStyle.load(std::memory_order_relaxed), now, "blow that reached him");
        return;
    }
    // An easier grab refused (AutoAnswerGrab, him ungrabbable meanwhile): its dodge now.
    if (const uintptr_t grabber = g_grabFrom.exchange(0);
        grabber && mem::SafeRead(reinterpret_cast<void*>(grabber + 0x30), from, sizeof(from))) {
        AutoDodge(p, grabber, from, g_autoDodgeStyle.load(std::memory_order_relaxed), now, "easier grab");
        return;
    }
    // A dodge delayed after an attack's start: now, from where its attacker stands.
    if (g_delayed.attacker) {
        if (static_cast<int32_t>(now - g_delayed.at) < 0) return;
        const DelayedDodge d = g_delayed;
        g_delayed = {};
        g_delayedPending = false;
        if (mem::SafeRead(reinterpret_cast<void*>(d.attacker + 0x30), from, sizeof(from)))
            AutoDodge(p, d.attacker, from, d.style, now, d.how);
        return;
    }
    if (AutoPending(now) && !dodging) return;  // asked, not started yet
    const DWORD next = g_autoNext.load();
    const bool dodgeReady = !next || static_cast<int32_t>(now - next) >= 0;
    // An attack's start: its dodge after the delay (and after the pause between two automatic dodges: an attack started
    // in it was lost), him hidden meanwhile.
    const auto later = [now, dodgeReady, next](uintptr_t attacker, int style, const char* how) {
        const float seconds = (style == g_weskerStyle.load(std::memory_order_relaxed) ? g_autoDelayWesker : g_autoDelay)
                                  .load(std::memory_order_relaxed);
        const DWORD delay = static_cast<DWORD>(std::clamp(seconds, 0.0f, kAutoDelayMax) * 1000.0f);
        DWORD at = now + delay;
        if (!dodgeReady && static_cast<int32_t>(next - at) > 0) at = next;
        if (at == now) return false;  // at once
        g_delayed.attacker = attacker;
        g_delayed.at = at;
        g_delayed.style = style;
        std::snprintf(g_delayed.how, sizeof(g_delayed.how), "%s", how);
        g_delayedPending = true;  // hidden from the blows until the dodge (Tick: AutoPending)
        return true;
    };
    const DWORD rocketNext = g_rocketNext.load();
    if (g_autoShots.load(std::memory_order_relaxed) && dodgeReady && (!rocketNext || static_cast<int32_t>(now - rocketNext) >= 0) &&
        IncomingRocket(from, distance) && distance < kRocketReach && AutoDodge(p, 0, from, shotStyle, now, "rocket")) {
        g_rocketNext = now + 1500;  // that rocket
        return;
    }
    int shape = -1;
    bool weapon = false;
    // Parries first, in the perfect dodge's sphere: their window opened again at each frame the attack is in it.
    if (parries && !dodging && AttackInSphere(p, shape, weapon, g_sphereRadius.load(std::memory_order_relaxed))) {
        if (g_autoParry.load(std::memory_order_relaxed) && AutoParryPress(g_autoParryKind.load(std::memory_order_relaxed)))
            return;
        if (g_autoRe6.load(std::memory_order_relaxed) && AutoRe6Press()) return;
    }
    const bool dodges = g_autoDodge.load(std::memory_order_relaxed) || g_autoPerfect.load(std::memory_order_relaxed);
    const bool shots = g_autoShots.load(std::memory_order_relaxed);
    // An attack's start (its wind-up), not while an automatic parry waits for its blow (it goes first); a shot's start.
    uintptr_t shooting = 0;
    const uintptr_t starting =
        dodges || shots ? AttackStarting(p, g_autoSphereRadius.load(std::memory_order_relaxed), shots ? &shooting : nullptr) : 0;
    if (shooting && mem::SafeRead(reinterpret_cast<void*>(shooting + 0x30), from, sizeof(from))) {
        uint32_t action = 0;
        mem::Read(shooting + 0x10E0, action);
        char how[64];
        std::snprintf(how, sizeof(how), "its shot %s starting (auto)", ActionTail(shooting, action));
        if (later(shooting, shotStyle, how) || AutoDodge(p, shooting, from, shotStyle, now, how)) return;
    }
    if (!dodges) return;
    if (starting && !parries && mem::SafeRead(reinterpret_cast<void*>(starting + 0x30), from, sizeof(from))) {
        uint32_t action = 0;
        mem::Read(starting + 0x10E0, action);
        char how[64];
        std::snprintf(how, sizeof(how), "its attack %s starting (auto)", ActionTail(starting, action));
        const int style = g_autoDodgeStyle.load(std::memory_order_relaxed);
        if (!later(starting, style, how)) AutoDodge(p, starting, from, style, now, how);
        return;
    }
    if (!dodgeReady || dodging) return;
    // Late: an armed attack shape in the perfect dodge's sphere, not a parasite's (armed while it moves: Chris invincible,
    // its attacks are seen by the host's actions).
    const uintptr_t attacker = AttackInSphere(p, shape, weapon, g_sphereRadius.load(std::memory_order_relaxed), false);
    if (!attacker || Player1BlowTaken(attacker) || !mem::SafeRead(reinterpret_cast<void*>(attacker + 0x30), from, sizeof(from)))
        return;
    char how[64];
    std::snprintf(how, sizeof(how), "%sattack shape %d in the sphere (auto)", weapon ? "weapon's " : "", shape);
    AutoDodge(p, attacker, from, g_autoDodgeStyle.load(std::memory_order_relaxed), now, how);
}

// Every frame while the option is on (render thread).
void Tick() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p) {
        SetDodgeInvincible(false);
        g_written = -1;
        g_inDodge = false;
        g_ownDodge = false;
        return;
    }
    const DWORD now = GetTickCount();
    int16_t health = 0;
    if (mem::Read(p + 0x1364, health)) g_lastHealth = health;
    // A dodge action (the game's, or a borrowed move's carrier) started just after one of the trainer's dodge keys.
    uint32_t action = 0;
    mem::Read(p + 0x10E0, action);
    const bool dodging = AvoidAction(action) || action == kCarrier;
    if (dodging && !g_inDodge) {
        const DWORD ask = g_lastAsk.load();
        g_ownDodge = ask && Elapsed(ask, now) < 500;
    }
    g_inDodge = dodging;
    if (!dodging) {
        g_ownDodge = false;
        // An automatic dodge asked keeps its marks until it starts (then until it ends).
        if (!AutoPending(now)) {
            g_perfectInDodge = false;
            g_autoInDodge = false;
        }
    }
    const bool window = WindowOpen(now) && g_on.load(std::memory_order_relaxed);
    const bool autoDodge = g_autoInDodge.load();
    const bool always = AlwaysInvincible() || ProjectileHidden(now);
    // Managed: the trainer's dodges with the perfect dodge on, and the automatic ones (from their request to their end).
    if (dodging && (g_ownDodge.load() || autoDodge) &&
        (g_on.load(std::memory_order_relaxed) || g_perfectInDodge.load() || autoDodge)) {
        // Off in the window (the sphere watches), after a perfect dodge and through an automatic dodge; after the window
        // on when strict, else the game's (off in its dodges, on in a borrowed move).
        const int want = window || g_perfectInDodge.load() || autoDodge || always ? 0
                         : g_strict.load(std::memory_order_relaxed) ? 1
                         : AvoidAction(action)                       ? 0
                                                                     : 1;
        SetHurtShapes(p, static_cast<uint8_t>(want));
        g_written = want;
        // Off is not enough: the game switches them back during its dodges (the blow still sounded on him); hidden from
        // the enemies as by "Invincible".
        if (!SetDodgeInvincible(want == 0) && want == 0) LOG("perfect dodge: player 1 not made invincible");
        int shape = -1;
        bool weapon = false;
        if (window && !g_perfectInDodge.load() && g_on.load(std::memory_order_relaxed))
            if (const uintptr_t attacker = AttackInSphere(p, shape, weapon, g_sphereRadius.load(std::memory_order_relaxed));
                attacker && !Player1BlowTaken(attacker)) {  // not a blow he already took (its shape still armed)
                char how[64];
                std::snprintf(how, sizeof(how), "%sattack shape %d in the sphere", weapon ? "weapon's " : "", shape);
                Dodged(attacker, now, how);
            }
    } else if (AutoPending(now)) {
        SetDodgeInvincible(true);  // an automatic dodge asked, not started yet
    } else {
        ReleaseHurtShapes(p, action, always);
    }
    TickAuto(p, action, dodging, now);
    if (g_eventPending.exchange(false)) {
        PlayDodgeSound();
        g_armed = g_slowOn.load(std::memory_order_relaxed) || g_aimOn.load(std::memory_order_relaxed);
        g_turnStart = 0;
        g_outOfAim = false;
    }
    TickAim(p, now);
}
}  // namespace

// swaps.cpp: one of the trainer's dodges was asked now.
void NoteDodgeStarted() {
    const DWORD now = GetTickCount();
    if (g_on.load(std::memory_order_relaxed)) g_dodgeTick = now ? now : 1;
    g_lastAsk = now ? now : 1;
    g_perfectInDodge = false;
}

// grabs.cpp ("Saisies améliorées"): a Majini about to take its hold in its own decision, no attack shape lit: the
// automatic parries' window opened for ParryGrab to decide (false), else an automatic dodge asked for the next frame,
// him ungrabbable meanwhile (true: the hold not given).
bool AutoAnswerGrab(uintptr_t enemy) {
    const DWORD now = GetTickCount();
    if (ParriedWithin(1500)) return false;
    if (g_autoParry.load(std::memory_order_relaxed) && AutoParryPress(g_autoParryKind.load(std::memory_order_relaxed)))
        return false;
    if (g_autoRe6.load(std::memory_order_relaxed) && AutoRe6Press()) return false;
    if (!(g_autoDodge.load(std::memory_order_relaxed) || g_autoPerfect.load(std::memory_order_relaxed)) || AutoPending(now))
        return false;
    const DWORD next = g_autoNext.load();
    if ((next && static_cast<int32_t>(now - next) < 0) || !StaminaAllows()) return false;
    g_grabFrom = enemy;
    g_autoTick = now ? now : 1;
    return true;
}

// parry.cpp: an enemy's armed attack shape (or its weapon's) in the perfect dodge's sphere around player 1 (render
// thread), `grab` for a catch (its action's name); what it threw (a projectile) in place of a thrower.
uintptr_t EnemyAttackInSphere(uintptr_t p, bool& grab) {
    int shape = -1;
    bool weapon = false;
    uintptr_t thrown = 0;
    grab = false;
    const uintptr_t attacker = AttackInSphere(p, shape, weapon, g_sphereRadius.load(std::memory_order_relaxed), true, &thrown);
    if (thrown) return thrown;
    uint32_t action = 0;
    if (attacker && mem::Read(attacker + 0x10E0, action)) grab = std::strstr(ActionTail(attacker, action), "Catch") != nullptr;
    return attacker;
}

// An automatic dodge (or perfect one) could start now: on, out of its pause, the stamina allowing it.
bool AutoMeleeReady(DWORD now) {
    if (!(g_autoDodge.load(std::memory_order_relaxed) || g_autoPerfect.load(std::memory_order_relaxed))) return false;
    const DWORD next = g_autoNext.load();
    return (!next || static_cast<int32_t>(now - next) >= 0) && StaminaAllows();
}

// grabs.cpp: an automatic dodge asked and not started yet (him ungrabbable).
bool AutoDodgeHolds() { return AlwaysInvincible() || (AutoDodgeOn() && AutoPending(GetTickCount())); }

// parry.cpp: player 1 hidden from the blows for `ms` (a bolt dodged before it touches him).
void HideFromProjectiles(DWORD ms) {
    const DWORD now = GetTickCount();
    g_projectileHideUntil = (now + ms) | 1;
    SetDodgeInvincible(true);
}

// parry.cpp: "Parade automatique : riposte" (0 knife slash, 1 Wesker's guard, 2 Jill's throw).
int AutoParryKind() { return g_autoParryKind.load(std::memory_order_relaxed); }

// player.cpp, before the other refusals (a borrowed dodge is invulnerable): the hit pending on player 1 is dodged - a
// blow in the window (his hurt shapes are off then, but a grab...), or any blow in a dodge after a perfect dodge.
// A bullet of an enemy pending on player 1, free to dodge it ("Esquive automatique des roquettes et des balles").
uintptr_t BulletToDodge(uintptr_t player) {
    if (!g_autoShots.load(std::memory_order_relaxed)) return 0;
    uintptr_t hit = 0;
    uint8_t valid = 0;
    uint32_t flags = 0, action = 0;
    int16_t health = 0;
    if (!mem::Read(player + 0x129C, hit) || !hit || !mem::Read(hit + 0x48, valid) || !valid || !mem::Read(hit + 0x3C, flags) ||
        !(flags & kBulletHit) || !mem::Read(player + 0x10E0, action) || action >= 0x80 || !mem::Read(player + 0x1364, health) ||
        health <= 1)
        return 0;
    const DWORD now = GetTickCount(), next = g_autoNext.load();
    if (next && static_cast<int32_t>(now - next) < 0) return 0;
    const uintptr_t shooter = HitAttacker(hit);
    return shooter && shooter != actors::Player(0) && shooter != actors::Player(1) && StaminaAllows() ? shooter : 0;
}

bool PerfectDodgeHit(uintptr_t player, bool fromReaction) {
    if (!EngineOn() || !player || player != actors::Player(0)) return false;
    const DWORD now = GetTickCount();
    if (const DWORD dodged = g_dodgedTick.load(); dodged && Elapsed(dodged, now) < kSameBlowMs) return true;
    if ((g_perfectInDodge.load() && g_ownDodge.load()) || g_autoInDodge.load() || AutoPending(now)) return true;
    int16_t health = 0;
    const int16_t before = g_lastHealth.load(std::memory_order_relaxed);
    if (const uintptr_t shooter = BulletToDodge(player)) {  // refused, dodged at the next frame
        if (fromReaction && mem::Read(player + 0x1364, health) && health < before && health > 0) mem::Write(player + 0x1364, before);
        g_shotFrom = shooter;
        g_shotTick = now ? now : 1;
        g_autoTick = now ? now : 1;  // invincible until the dodge
        return true;
    }
    const bool window = g_on.load(std::memory_order_relaxed) && WindowOpen(now);
    const bool always = AlwaysInvincible();
    if (!window && !always) return false;
    uintptr_t hit = 0;
    uint8_t valid = 0;
    uint32_t flags = 0;
    const bool pending = mem::Read(player + 0x129C, hit) && hit && mem::Read(hit + 0x48, valid) && valid;
    if (pending) mem::Read(hit + 0x3C, flags);
    const uintptr_t attacker = pending ? HitAttacker(hit) : 0;
    const char* attackerClass = attacker ? UnitClassName(attacker) : nullptr;
    const bool enemyBlow = attacker && attacker != actors::Player(0) && attacker != actors::Player(1) && !(flags & kBlastHit) &&
                           !(attackerClass && std::strncmp(attackerClass, "uOm", 3) == 0);
    if (!enemyBlow && !always) return false;
    if (fromReaction && mem::Read(player + 0x1364, health) && health < before && health > 0)
        mem::Write(player + 0x1364, before);  // the blow was already taken off before its reaction
    if (!window) {
        // Invincible tout court: refused; an enemy's blow that reached him anyway dodged at the next frame (not a bullet:
        // their own option, above; not with a parry's window open: the sphere parries it, parry.cpp).
        if (enemyBlow && !(flags & kBulletHit) && AutoMeleeReady(now) && !ParrySparesBlood(player)) {
            g_blowFrom = attacker;
            g_autoTick = now ? now : 1;
        }
        return true;
    }
    if (!enemyBlow) return true;
    char how[48];
    std::snprintf(how, sizeof(how), "blow (flags %08X)", flags);
    Dodged(attacker, now, how);
    return true;
}

// quick_knife.cpp's blood hook: a blow now would be dodged (the window open after a dodge, a dodge after a perfect
// dodge), or one was just dodged.
bool PerfectDodgeSparesBlood(uintptr_t player) {
    if (!EngineOn() || !player || player != actors::Player(0)) return false;
    const DWORD now = GetTickCount();
    if (const DWORD dodged = g_dodgedTick.load(); dodged && Elapsed(dodged, now) < kSameBlowMs) return true;
    return (g_on.load(std::memory_order_relaxed) && WindowOpen(now)) || (g_perfectInDodge.load() && g_ownDodge.load()) ||
           g_autoInDodge.load() || AutoPending(now) || AlwaysInvincible() || BulletToDodge(player);
}

void RegisterPerfectDodge() {
    Feature& dodge = Add({ .id = "combat.perfect_dodge", .name = "Esquive parfaite",
                           .help = "Une esquive du trainer (esquive, esquive en visant, roulades à touche fixe) faite juste "
                                   "avant qu'un ennemi frappe : Chris est invincible pendant la fenêtre et une attaque qui "
                                   "entre dans la sphère autour de lui est esquivée (aucun contact : ni son de coup, ni "
                                   "sang) ; un son est joué, le jeu passe au ralenti et, si tu vises juste après, la visée "
                                   "se tourne vers l'ennemi esquivé. Le curseur règle la fenêtre après l'appui, comme pour "
                                   "les parades. Une esquive qui t'emmène hors de portée avant l'attaque reste une esquive "
                                   "normale.",
                           .category = "Dodge", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = 0.3f, .valueMin = 0.05f, .valueMax = 0.8f, .valueFormat = "%.2f s" });
    Feature* const dp = &dodge;
    dodge.set = [dp](bool on) {
        if (on && (!EnsurePlayerDamageHook() || !EnsurePlayerHitReactionHook())) return false;
        if (on && !EnsureCommonEffectsHook()) LOG("perfect dodge: no blood hook, a dodged blow may draw blood");
        g_window = dp->value;
        g_dodgeTick = 0;
        g_dodged = 0;
        if (!on) ReleaseHurtShapesNow();
        g_on = on;
        return true;
    };
    dodge.tick = [dp] { g_window.store(dp->value, std::memory_order_relaxed); };
    // The machinery for the perfect dodge and the automatic dodges; let go once they are all off.
    OnFrame([] {
        static bool ran = false;
        if (EngineOn() || g_autoParry.load() || g_autoRe6.load() || ProjectileHidden(GetTickCount())) {
            Tick();
            ran = true;
        } else if (ran) {
            ReleaseHurtShapesNow();
            ran = false;
        }
    });
    Add({ .id = "combat.perfect_dodge_strict", .name = "Esquives du jeu : invincible seulement sur une esquive parfaite",
          .help = "Les esquives du jeu lancées par le trainer (Quick Duck, roulades) ne rendent plus Chris invincible : un "
                  "coup hors du timing le touche (se baisser n'évite plus les coups qui descendent). Sur une esquive "
                  "parfaite il est invincible jusqu'à la fin de l'esquive. Les esquives des prompts du jeu ne changent pas.",
          .category = "Dodge", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_strict = on;
              return true;
          },
          .available = [] { return game::IsSupported(); },
          .defaultOn = true });
    std::vector<std::string> sounds = { "Aucun" };
    for (int n = 1; n <= kWeskerSounds; ++n) sounds.push_back("W" + std::to_string(n) + (n == 12 ? " (par défaut)" : ""));
    Add({ .id = "combat.perfect_dodge_sound", .name = "Esquive parfaite : son",
          .help = "Un des sons de Wesker de Resident Evil 4 Remake (W1 à W20, ceux de la page d'écoute ; fichiers "
                  "sounds\\wesker du dossier du trainer).",
          .category = "Dodge", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_sound = std::clamp(index, 0, kWeskerSounds);
              return true;
          },
          .value = 12, .valueMin = 0, .valueMax = kWeskerSounds, .valueFormat = "%.0f", .choices = sounds });
    Feature& volume = Add({ .id = "combat.perfect_dodge_volume", .name = "Esquive parfaite : volume du son",
                            .help = "100 % = celui du fichier ; au-delà, un limiteur évite la saturation.",
                            .category = "Dodge", .kind = Kind::Toggle,
                            .set = [](bool) { return true; },
                            .available = [] { return game::IsSupported(); },
                            .value = 200.0f, .valueMin = 0.0f, .valueMax = 800.0f, .valueFormat = "%.0f %%",
                            .defaultOn = true });
    Feature* const vp = &volume;
    volume.tick = [vp] { g_gain.store(vp->value / 100.0f, std::memory_order_relaxed); };
    Feature& slow = Add({ .id = "combat.perfect_dodge_slow", .name = "Esquive parfaite : ralenti",
                          .help = "Si tu vises (arme ou couteau) juste après une esquive parfaite (dans la fenêtre de "
                                  "« visée vers l'ennemi esquivé »), le jeu passe à la vitesse du curseur (0,25 = quatre "
                                  "fois plus lent) pendant la durée réglée ci-dessous.",
                          .category = "Dodge", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = 0.25f, .valueMin = 0.05f, .valueMax = 1.0f, .valueFormat = "x%.2f", .defaultOn = true });
    Feature* const sp = &slow;
    slow.set = [sp](bool on) {
        g_slowSpeed = sp->value;
        g_slowOn = on;
        return true;
    };
    slow.tick = [sp] { g_slowSpeed.store(sp->value, std::memory_order_relaxed); };
    Feature& slowTime = Add({ .id = "combat.perfect_dodge_slow_time", .name = "Esquive parfaite : durée du ralenti",
                              .help = "En secondes réelles.",
                              .category = "Dodge", .kind = Kind::Toggle,
                              .set = [](bool) { return true; },
                              .available = [] { return game::IsSupported(); },
                              .value = 4.0f, .valueMin = 0.5f, .valueMax = 10.0f, .valueFormat = "%.1f s",
                              .defaultOn = true });
    Feature* const tp = &slowTime;
    slowTime.tick = [tp] { g_slowSeconds.store(tp->value, std::memory_order_relaxed); };
    Add({ .id = "combat.perfect_dodge_slow_player", .name = "Esquive parfaite : Chris garde sa vitesse pendant le ralenti",
          .help = "Coché : seuls les ennemis et le reste du jeu ralentissent, Chris bouge et vise à vitesse normale.",
          .category = "Dodge", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_slowPlayer = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Feature& aim = Add({ .id = "combat.perfect_dodge_aim", .name = "Esquive parfaite : visée vers l'ennemi esquivé",
                        .help = "Si tu vises (arme ou couteau) dans les secondes du curseur après une esquive parfaite, la "
                                "visée se tourne vers l'ennemi esquivé (en 0,2 s, une fois ; la souris reprend la main "
                                "ensuite). Le curseur règle aussi la fenêtre du ralenti.",
                        .category = "Dodge", .kind = Kind::Toggle,
                        .available = [] { return game::IsSupported(); },
                        .value = 1.5f, .valueMin = 0.2f, .valueMax = 4.0f, .valueFormat = "%.1f s", .defaultOn = true });
    Feature* const ap = &aim;
    {
        Feature& sphere = Add({ .id = "combat.perfect_dodge_sphere", .name = "Esquive parfaite : taille de la sphère",
                                .help = "Le rayon (cm) de la zone autour de Chris où une attaque ennemie (ou l'arme qu'il "
                                        "tient) compte pour une esquive parfaite pendant la fenêtre ; de 20 cm sous ses "
                                        "pieds à 2 m de haut. Plus grand = plus facile.",
                                .category = "Dodge", .kind = Kind::Toggle,
                                .set = [](bool) { return true; },
                                .available = [] { return game::IsSupported(); },
                                .value = 80.0f, .valueMin = 30.0f, .valueMax = 200.0f, .valueFormat = "%.0f cm",
                                .defaultOn = true });
        Feature* const rp = &sphere;
        sphere.tick = [rp] { g_sphereRadius.store(rp->value, std::memory_order_relaxed); };
    }
    aim.set = [ap](bool on) {
        g_aimWindow = ap->value;
        g_aimOn = on;
        return true;
    };
    aim.tick = [ap] { g_aimWindow.store(ap->value, std::memory_order_relaxed); };
    // The automatic responses (user, 2026-10-29), in Mêlée > Automatisation.
    const auto supported = [] { return game::IsSupported(); };
    const auto dodgeHooks = [] {
        if (!EnsurePlayerDamageHook() || !EnsurePlayerHitReactionHook()) return false;
        if (!EnsureCommonEffectsHook()) LOG("auto dodge: no blood hook");
        return true;
    };
    const std::vector<std::string> styles = DodgeStyleNames();
    for (size_t i = 0; i < styles.size(); ++i)
        if (styles[i] == "Dodge Wesker") g_weskerStyle = static_cast<int>(i);
    Add({ .id = "auto.dodge", .name = "Esquive automatique",
          .help = "Chris esquive tout seul chaque attaque ennemie au corps à corps dès son élan (« Esquive automatique : "
                  "distance de l'ennemi »), du côté opposé à l'ennemi (ou dans la direction que tu tiens), avec le style "
                  "choisi juste en dessous, même s'il est déjà en train d'esquiver. Tant que l'option est cochée, Chris est "
                  "invincible (aucun contact, comme « Invincible ») : les attaques sont repérées par leur élan et la sphère.",
          .category = "Automatisation", .kind = Kind::Toggle,
          .set = [dodgeHooks](bool on) {
              if (on && !dodgeHooks()) return false;
              g_autoDodge = on;
              return true;
          },
          .available = supported });
    {
        Feature& sphere = Add({ .id = "auto.dodge_sphere", .name = "Esquive automatique : distance de l'ennemi",
                                .help = "Un ennemi tourné vers Chris à cette distance ou moins (cm) qui commence une attaque "
                                        "au corps à corps est esquivé dès son élan, avant l'impact (le double pour une charge ; esquive automatique et "
                                        "esquive parfaite automatique). Un coup qui arrive quand même dans la sphère de "
                                        "l'esquive parfaite est esquivé aussi. Avec la parade ou le contre RE6 automatiques "
                                        "cochés, les parades passent d'abord.",
                                .category = "Automatisation", .kind = Kind::Toggle,
                                .set = [](bool) { return true; },
                                .available = supported,
                                .value = 170.0f, .valueMin = 50.0f, .valueMax = 600.0f, .valueFormat = "%.0f cm",
                                .defaultOn = true });
        Feature* const rp = &sphere;
        sphere.tick = [rp] { g_autoSphereRadius.store(rp->value, std::memory_order_relaxed); };
    }
    {
        Feature& delay = Add({ .id = "auto.dodge_delay", .name = "Esquive automatique : délai",
                               .help = "Le temps entre le début de l'attaque (son élan, ou le début d'un tir) et l'esquive "
                                       "automatique : 0 = tout de suite. Chris est déjà invincible pendant ce délai.",
                               .category = "Automatisation", .kind = Kind::Toggle,
                               .set = [](bool on) {
                                   if (!on) g_autoDelay = 0.0f;
                                   return true;
                               },
                               .available = supported,
                               .value = 0.15f, .valueMin = 0.0f, .valueMax = 0.35f, .valueFormat = "%.2f s",
                               .valueStep = 0.05f, .defaultOn = true });
        Feature* const dp = &delay;
        delay.tick = [dp] { g_autoDelay.store(dp->value, std::memory_order_relaxed); };
        Feature& wesker = Add({ .id = "auto.dodge_delay_wesker", .name = "Esquive automatique : délai du style Wesker",
                                .help = "Le délai de l'esquive automatique (à la place du précédent) quand son style est « Dodge "
                                        "Wesker » : ses esquives le ramènent sur place à la fin, plus tard évite qu'il revienne "
                                        "pile sur l'attaque. 0 = tout de suite.",
                                .category = "Automatisation", .kind = Kind::Toggle,
                                .set = [](bool on) {
                                    if (!on) g_autoDelayWesker = 0.0f;
                                    return true;
                                },
                                .available = supported,
                                .value = 0.3f, .valueMin = 0.0f, .valueMax = kAutoDelayMax, .valueFormat = "%.2f s",
                                .valueStep = 0.05f, .defaultOn = true });
        Feature* const wp = &wesker;
        wesker.tick = [wp] { g_autoDelayWesker.store(wp->value, std::memory_order_relaxed); };
    }
    Add({ .id = "auto.dodge_style", .name = "Esquive automatique : style",
          .help = "Les esquives jouées par l'esquive automatique et l'esquive parfaite automatique (les mêmes styles que "
                  "l'esquive).",
          .category = "Automatisation", .kind = Kind::Choice, .available = supported,
          .choose = [](int index) {
              g_autoDodgeStyle = index;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = static_cast<float>(styles.size() - 1), .valueFormat = "%.0f",
          .choices = styles });
    Add({ .id = "auto.perfect_dodge", .name = "Esquive parfaite automatique",
          .help = "Comme l'esquive automatique (avec son style, Chris invincible tant que l'option est cochée), mais chaque "
                  "esquive est une esquive parfaite : son son, et le ralenti et la visée vers l'ennemi si tu vises juste "
                  "après (réglages de l'esquive parfaite).",
          .category = "Automatisation", .kind = Kind::Toggle,
          .set = [dodgeHooks](bool on) {
              if (on && !dodgeHooks()) return false;
              g_autoPerfect = on;
              return true;
          },
          .available = supported });
    Add({ .id = "auto.parry", .name = "Parade automatique",
          .help = "Chris pare tout seul chaque attaque qui entre dans la sphère, comme une parade au couteau faite au bon "
                  "moment, avec la riposte choisie juste en dessous. Avec le couteau : rien si le couteau est cassé ou si "
                  "l'emplacement du couteau est vide. Passe avant le contre RE6 et les esquives automatiques.",
          .category = "Automatisation", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (!SetAutoParry(on, g_autoRe6.load())) return false;
              g_autoParry = on;
              return true;
          },
          .available = supported });
    Add({ .id = "auto.parry_riposte", .name = "Parade automatique : riposte",
          .help = "La riposte de la parade automatique (celle de chaque personnage, onglet Parade, reste pour tes parades à "
                  "toi ; un personnage réglé sur « Aucune » n'a pas non plus de parade automatique).",
          .category = "Automatisation", .kind = Kind::Choice, .available = supported,
          .choose = [](int index) {
              g_autoParryKind = std::clamp(index, 0, 3);
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 3, .valueFormat = "%.0f",
          .choices = { "Couteau (RE4 Remake)", "Garde de Wesker", "Projection de Chris", "Projection de Josh" } });
    Add({ .id = "auto.re6_counter", .name = "Contre RE6 automatique",
          .help = "Chris fait tout seul la parade façon RE6 (son contre) sur chaque attaque qui entre dans la sphère. Après "
                  "la parade automatique, avant les esquives automatiques.",
          .category = "Automatisation", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (!SetAutoParry(g_autoParry.load(), on)) return false;
              g_autoRe6 = on;
              return true;
          },
          .available = supported });
    Add({ .id = "auto.dodge_shots", .name = "Esquive automatique des roquettes et des balles",
          .help = "Chris esquive tout seul, sur le côté, une roquette ennemie qui arrive à moins de 7 m et chaque balle qui "
                  "l'atteint (la balle ne le touche pas), avec le style choisi juste en dessous ; il est invincible "
                  "pendant l'esquive. Pas plus d'une esquive toutes les 0,6 s.",
          .category = "Automatisation", .kind = Kind::Toggle,
          .set = [dodgeHooks](bool on) {
              if (on && (!dodgeHooks() || !EnsureRocketTimeHooks())) return false;
              g_autoShots = on;
              return true;
          },
          .available = supported });
    Add({ .id = "auto.dodge_shots_style", .name = "Esquive des roquettes et des balles : style",
          .help = "Les esquives jouées contre les roquettes et les balles (les mêmes styles que l'esquive).",
          .category = "Automatisation", .kind = Kind::Choice, .available = supported,
          .choose = [](int index) {
              g_autoShotStyle = index;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = static_cast<float>(styles.size() - 1), .valueFormat = "%.0f",
          .choices = styles });
    OnRestore([] {
        g_attached.clear();
        ReleaseHurtShapesNow();
        g_wave.Close();
    });
}
}  // namespace features
