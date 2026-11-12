// "Rattraper les roquettes (parade)" (user, 2026-10-25: "Chris plays Wesker's rocket catch if he parries the rocket
// launcher"). The parry sees the rocket too late: its direct hit comes as a bullet of the Majini (flags 0x0008A690,
// refused as bullets), so the rockets are followed in flight:
// - the RPG Majini's rocket uWp83 (vtable RVA 0xC35E28: the object at +0x20 of its bullet hit on player 1), the player's
//   RPG shell uWp44 (vtable RVA 0xC33B10; both updated by RVA 0x799ED0) and uWp6c (vtable RVA 0xC2F208, the class of a
//   blast seen earlier): vtable slot 6 = the update (uWp6c RVA 0x675670:
//   vfunc +0x60 "over" -> return, else the state +0x14 / sub-state +0x15 handlers +0x170.. / +0x180..; the main loop
//   with steps at +0x16 ends by vfunc +0x1A8 = RVA 0x798380, [+0x1BD0] = 1). The slots are redirected (the pause menu's
//   way, pickup.cpp) to note each rocket.
// - A rocket first seen within 3 m of player 1 is his own (his RPG): left alone. Another one within 2 m of his chest with
//   the knife parry's window open (ParryCatchesRocket: its press only, whatever its riposte; not the RE6 parry, whose
//   attack key also fires - user) is held (below) and player 1, facing it, plays Wesker's catch (em80 0xD7 of
//   cEm80ActRokeUke 0x66, wesker_moves.cpp), invulnerable as in the guard, with the parry's sound and ring.
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace features {
bool ParryCatchesRocket(uintptr_t player, uintptr_t rocket);   // parry.cpp: the knife parry's window, sound and ring
bool PlayRocketCatch();                                        // wesker_moves.cpp
bool ParryRiposteIsSlash();                                    // parry.cpp: the knife slash is the parry's riposte
void PlayKnifeAimSlash(uintptr_t player);                      // parry.cpp: the knife aim's attack
int RocketBlast(const float* at, float radius, int damage);    // wesker_moves.cpp: player 1's blast on the enemies near
const char* UnitClassName(uintptr_t object);                   // parry.cpp: DTI class name
void FacePlayerTowards(uintptr_t player, const float* point);  // wesker_moves.cpp
bool PrepareWeskerMoves();                                     // wesker_moves.cpp
bool EnsureAttachedMatrixHook();                               // player.cpp: the attached units' world matrix
bool EnsurePlayerRequestHook();                                // action_log.cpp: player 1's shot refused over the slash
float WorldTimeSpeed();                                        // time_speed.cpp: sFps +0x2C, the world's time speed

namespace {
using UpdateFn = void(__fastcall*)(void* self, void* edx);

struct RocketClass {
    const char* name;
    uintptr_t vtable, getDti, update;  // RVA: the vtable, its slots 4 and 6
};
constexpr RocketClass kClasses[] = {
    { "uWp83", 0xC35E28, 0x7C2180, 0x799ED0 },  // the RPG Majini's rocket (logged: a bullet hit's +0x20, user's hit)
    { "uWp44", 0xC33B10, 0x7C1C90, 0x799ED0 },
    { "uWp6c", 0xC2F208, 0x7C14D0, 0x675670 },
    // A crossbow bolt (user, 2026-10-30: parried with a dodge it still broke on Chris): followed in flight for the
    // parries' dodge before its contact (parry.cpp TickBoltDodge); the same update as uWp6c.
    { "uWp81", 0xC30490, 0x7C1610, 0x675670 },
};
constexpr int kClassCount = static_cast<int>(std::size(kClasses));
UpdateFn g_original[kClassCount] = {};
mem::Patch* g_patch[kClassCount] = {};

std::atomic<bool> g_catchOn{ false };
// The game reuses one object for the RPG Majini's rockets (the same address each time): an entry is a new rocket's once
// the object was not updated for 500 ms or jumped more than 3 m in a frame (back at the launcher; user, 2026-10-25: a
// rocket lying 2.8 m from Chris when the trainer was reloaded was taken for his own, and with it every rocket after).
struct FirstSeen {
    float distance;  // to player 1
    float at[3];     // where (at the launcher: the line to the catch is its flight)
    DWORD last;      // its last update
    float lastAt[3];  // and where
};
std::unordered_map<uintptr_t, FirstSeen> g_firstSeen;  // game thread
DWORD g_catchTick = 0;                                 // game thread: one catch at a time

std::vector<uint8_t> Bytes(uintptr_t value) {
    return { static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
             static_cast<uint8_t>(value >> 24) };
}

// Gone without its blast (user, 2026-10-25: the dropped rocket exploded before it vanished): the shells' state machine
// (+0x14 state, +0x15..+0x17 sub-state and steps; handlers vtable +0x170..+0x17C, RVA 0x675670) set to state 3, whose
// handler (+0x17C -> +0x160, RVA 0x675660) removes the unit (its vfunc +0x30) at its next update. EndRocket below
// ([+0x1BD0] = 1) is its detonation: its flight state's sub-state 2 then blows it up (+0x188, RVA 0x7BF0F0).
void RemoveRocket(uintptr_t rocket) {
    const uint8_t state[4] = { 3, 0, 0, 0 };
    mem::SafeWrite(reinterpret_cast<void*>(rocket + 0x14), state, sizeof(state));
}

void EndRocket(uintptr_t rocket) {
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(rocket, vtable) || !mem::Read(vtable + 0x1A8, fn) || !fn) return;
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*)>(fn)(reinterpret_cast<void*>(rocket), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The rocket held after a catch (user, 2026-10-25: it vanished, "one does not get what Chris does": it must stay in his
// hands until its thrust stops, then fall to the ground, as Wesker's). Wesker's own catch shows a rocket prop of his
// ([his +0x1FA0], made visible by cEm80ActRokeUke's init) and the flying one ends; Chris has none, so the flying rocket
// is kept: its own update skipped (no flight, no blast), in player 1's right hand (joint 8) until he throws it in his
// catch motion (em80 0xD7, 581 frames), then it flies, lies 3 s and is removed. Its post-update (vfunc +0xB0: RVA
// 0x7955A0, the model / matrices part of the update) runs at the new place.
// Player 1's weapon is hidden meanwhile, "Arme toujours visible" or not (user).
// Its thrust flame goes out a second before the release (user: Wesker's does, "it sells why the rocket can fall"):
// ExtinguishRocket.
constexpr uint32_t kCatchMotion = 0xCD7;  // em80 0xD7 in player 1's bank C
constexpr float kFlameOutFrame = 340.0f;  // more than a second before the throw (user)
enum class Held { None, InHand, Falling, Lying };
uintptr_t g_held = 0;             // game thread
uintptr_t g_heldPlayer = 0;       // player 1 when it was caught
Held g_heldPhase = Held::None;
float g_floor = 0.0f, g_lying = 0.0f;  // g_lying: seconds on the ground, world time
// The throw (user, 2026-10-25: it fell straight down, too early; Chris flicks his hand back to throw it): held to frame
// 510 of the catch, then it flies on with the speed and spin it had in his hand, falls (980 cm/s2) to his feet's height
// and lies flat 3 s. The frame is the user's, tuned with a slider since removed ("the timing is perfect"); found on the
// way: his hand (joint 8) spins fastest at frame 498 (9.6 rad/s), a peak of the rocket's speed in the hand after frame
// 400 came at 490 with the user's pose ("a little too early").
constexpr float kThrowFrame = 510.0f;
struct Motion {
    float vel[3];   // cm/s
    float axis[3];  // spin axis (world)
    float rate;     // rad/s
};
Motion g_now{};  // its motion in his hand at the last sample
float g_samplePos[3]{}, g_sampleQuat[4]{};
bool g_haveSample = false;
float g_simPos[3]{}, g_simQuat[4]{};  // thrown: where it flies (its matrix from these, RocketLocalMatrix)
Motion g_flight{};
// The thrown rocket flies and lies apart from the held one (user, 2026-10-25: "only one catch in two": the game fires
// its two rocket objects in turn, and the next one came while the last lay on the ground, still taken as held).
std::atomic<uintptr_t> g_thrown{ 0 };
Held g_thrownPhase = Held::None;  // Falling, then Lying
LARGE_INTEGER g_thrownLast{};
// Attached to his right hand as his weapons are (user: "the hands move much more than the rocket - attach it to the
// hand as for Wesker"): a unit's parent +0x18 (player 1) and joint +0x1C (8, his right hand: every weapon unit of his has
// it), its +0x30 position / +0x40 rotation then relative to that joint (the weapon in hand: (0, 0, 0) and identity; its
// +0x60 matrix is then the local one, its world matrix +0xA0 - an unparented unit has both the same). Placed from its
// flight pose it was "held by the wrong end", then "upside down and not at the right place", its rotation never the same
// (user): its local matrix +0x60, which the world matrix is made from (RVA 0x08ABA0: +0x60 x the parent joint's), is
// written right before (player.cpp's hook of it, RocketLocalMatrix) instead of being left to what rebuilds it from +0x40.
constexpr int kHandJoint = 8;
bool g_flameOut = false;
uint32_t g_savedParent = 0, g_savedJoint = 0;  // its +0x18 / +0x1C before
std::atomic<uintptr_t> g_inHand{ 0 };           // the rocket attached to player 1's hand

// q = a then b (x y z w).
void QuatMul(const float* a, const float* b, float* q) {
    q[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    q[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    q[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    q[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// Its pose in the hand, the user's (2026-10-25, tuned live with six sliders since removed: "now well placed in Chris's
// hand", "lock the positions and rotations"): offsets (cm) and rotations (degrees, about X, then Y, then Z) in Chris's
// own axes (his world matrix +0xA0: X across, Y up, Z forward) at a moment of the catch, made a pose in the hand by kRef
// = Chris's matrix x the hand joint's inverse at that moment (the joint found by its id through the joint map [+0x31C],
// VA 0xB56DD0; the sliders were "very hard to place" along the hand's own axes).
constexpr float kPose[6] = { 22.0f, 145.0f, 92.0f, 14.0f, -84.0f, 8.0f };
constexpr float kRef[16] = { -0.937608f, -0.115321f, -0.328014f, 0.0f, -0.0222445f, 0.961357f, -0.274403f, 0.0f,
                             0.346983f, -0.249986f, -0.903941f, 0.0f, 5.91187f, -139.283f, 48.5999f, 1.0f };

void LocalPose(float* pos, float* quat) {
    const float* v = kPose;
    pos[0] = v[0], pos[1] = v[1], pos[2] = v[2];
    const float hx = v[3] * 0.00872665f, hy = v[4] * 0.00872665f, hz = v[5] * 0.00872665f;  // half angles in radians
    const float qx[4] = { std::sin(hx), 0.0f, 0.0f, std::cos(hx) }, qy[4] = { 0.0f, std::sin(hy), 0.0f, std::cos(hy) },
                qz[4] = { 0.0f, 0.0f, std::sin(hz), std::cos(hz) };
    float yx[4];
    QuatMul(qy, qx, yx);
    QuatMul(qz, yx, quat);
}

// The rows of q's matrix (each row the image of an axis: a yaw (0, sin, 0, cos) gives row 2 = (sin y, 0, cos y)).
void QuatRows(const float* q, float m[3][3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z), m[0][1] = 2 * (x * y + w * z), m[0][2] = 2 * (x * z - w * y);
    m[1][0] = 2 * (x * y - w * z), m[1][1] = 1 - 2 * (x * x + z * z), m[1][2] = 2 * (y * z + w * x);
    m[2][0] = 2 * (x * z + w * y), m[2][1] = 2 * (y * z - w * x), m[2][2] = 1 - 2 * (x * x + y * y);
}

void RowsToQuat(const float m[3][3], float* q) {
    const float c00 = m[0][0], c11 = m[1][1], c22 = m[2][2];
    const float c01 = m[1][0], c10 = m[0][1], c02 = m[2][0], c20 = m[0][2], c12 = m[2][1], c21 = m[1][2];
    const float trace = c00 + c11 + c22;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s, q[0] = (c21 - c12) / s, q[1] = (c02 - c20) / s, q[2] = (c10 - c01) / s;
    } else if (c00 > c11 && c00 > c22) {
        const float s = std::sqrt(1.0f + c00 - c11 - c22) * 2.0f;
        q[3] = (c21 - c12) / s, q[0] = 0.25f * s, q[1] = (c01 + c10) / s, q[2] = (c02 + c20) / s;
    } else if (c11 > c22) {
        const float s = std::sqrt(1.0f + c11 - c00 - c22) * 2.0f;
        q[3] = (c02 - c20) / s, q[0] = (c01 + c10) / s, q[1] = 0.25f * s, q[2] = (c12 + c21) / s;
    } else {
        const float s = std::sqrt(1.0f + c22 - c00 - c11) * 2.0f;
        q[3] = (c10 - c01) / s, q[0] = (c02 + c20) / s, q[1] = (c12 + c21) / s, q[2] = 0.25f * s;
    }
}

void Mul(const float* a, const float* b, float* out) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}

void PoseMatrix(float* m) {
    float pos[3], quat[4], rows[3][3];
    LocalPose(pos, quat);
    QuatRows(quat, rows);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = rows[i][j];
        m[i * 4 + 3] = 0.0f;
    }
    m[12] = pos[0], m[13] = pos[1], m[14] = pos[2], m[15] = 1.0f;
}

// Its matrix in the hand.
void HandLocal(float* m) {
    float pose[16];
    PoseMatrix(pose);
    Mul(pose, kRef, m);
}

// Its pose in player 1's hand, written every frame it is held.
void PoseInHand(uintptr_t rocket) {
    float m[16], rows[3][3], quat[4];
    HandLocal(m);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rows[i][j] = m[i * 4 + j];
    RowsToQuat(rows, quat);
    mem::SafeWrite(reinterpret_cast<void*>(rocket + 0x30), m + 12, 3 * sizeof(float));
    mem::SafeWrite(reinterpret_cast<void*>(rocket + 0x40), quat, sizeof(quat));
}

bool AttachToHand(uintptr_t rocket, uintptr_t p1) {
    mem::Read(rocket + 0x18, g_savedParent);
    mem::Read(rocket + 0x1C, g_savedJoint);
    PoseInHand(rocket);
    mem::Write<uint32_t>(rocket + 0x1C, kHandJoint);
    mem::Write<uint32_t>(rocket + 0x18, static_cast<uint32_t>(p1));
    g_inHand = rocket;
    g_haveSample = false;
    g_now = Motion{};
    LOG("rocket catch: rocket %08X attached to player 1's hand (its parent was %08X joint %u)", static_cast<unsigned>(rocket),
        g_savedParent, g_savedJoint);
    return true;
}

// Back in the world where it is seen: its world matrix as the engine made it (+0xA0: three axis rows, +0xD0 its
// position), not recomputed (user: recomputed, it turned upside down when falling; its +0x60, local while attached, put
// it at the world's origin: it vanished).
void DetachFromHand(uintptr_t rocket, uintptr_t) {
    g_inHand = 0;
    float m[16]{}, rows[3][3], quat[4];
    const bool known = mem::SafeRead(reinterpret_cast<void*>(rocket + 0xA0), m, sizeof(m));
    mem::Write<uint32_t>(rocket + 0x18, g_savedParent);
    mem::Write<uint32_t>(rocket + 0x1C, g_savedJoint);
    if (!known) return;
    for (int i = 0; i < 3; ++i) {
        const float length = std::sqrt(m[i * 4] * m[i * 4] + m[i * 4 + 1] * m[i * 4 + 1] + m[i * 4 + 2] * m[i * 4 + 2]);
        for (int j = 0; j < 3; ++j) rows[i][j] = length > 1e-4f ? m[i * 4 + j] / length : (i == j ? 1.0f : 0.0f);
    }
    RowsToQuat(rows, quat);
    const float pos[3] = { m[12], m[13], m[14] };
    mem::SafeWrite(reinterpret_cast<void*>(rocket + 0x30), pos, sizeof(pos));
    mem::SafeWrite(reinterpret_cast<void*>(rocket + 0x40), quat, sizeof(quat));
}

// Its flame and smoke: its handles [+0x1D04] / [+0x1D08] (its setup RVA 0x7A2EA0 spawns them attached, VA 0x7723E0)
// were 0 when caught (logged), so its effects are found in the effect manager's list instead ([VA 0x1249C5C] +0x260,
// next +0x1E4, handle +0x1E8: VA 0x770510 finds, VA 0x7712C0 releases): those whose owner +0x18 is the rocket (two,
// logged), released as its destructor's base part does with its own (RVA 0x7A2D30). Their +4 bit 0x800 (cEm80ActRokeUke's
// init shows Wesker's rocket prop by clearing it) hid nothing (user).
void ReleaseEffect(uintptr_t manager, uint32_t handle) {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, uint32_t)>(mem::Rva(0x3712C0))(reinterpret_cast<void*>(manager), nullptr,
                                                                                    handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ExtinguishRocket(uintptr_t rocket) {
    uintptr_t manager = 0, effect = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C5C)), manager) || !manager || !mem::Read(manager + 0x260, effect))
        return;
    std::vector<uint32_t> handles;
    for (int guard = 0; effect && guard < 1024 && handles.size() < 8; ++guard) {
        uintptr_t owner = 0;
        uint32_t handle = 0;
        if (mem::Read(effect + 0x18, owner) && owner == rocket && mem::Read(effect + 0x1E8, handle)) handles.push_back(handle);
        if (!mem::Read(effect + 0x1E4, effect)) break;
    }
    for (uint32_t handle : handles) ReleaseEffect(manager, handle);
    LOG("rocket catch: rocket %08X flame out (%u effects released)", static_cast<unsigned>(rocket),
        static_cast<unsigned>(handles.size()));
}
DWORD g_heldTick = 0;
LARGE_INTEGER g_heldLast{};
std::atomic<bool> g_hideWeapon{ false };  // the render thread hides player 1's weapon while it is set

void CallPostUpdate(uintptr_t rocket) {
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(rocket, vtable) || !mem::Read(vtable + 0xB0, fn) || !fn) return;
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*)>(fn)(reinterpret_cast<void*>(rocket), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void DropHeld(const char* why) {
    if (g_held) LOG("rocket catch: held rocket %08X ended (%s)", static_cast<unsigned>(g_held), why);
    if (g_held && g_heldPhase == Held::InHand) DetachFromHand(g_held, actors::Player(0));
    if (g_held) RemoveRocket(g_held);
    g_held = 0;
    g_heldPhase = Held::None;
    g_hideWeapon = false;
}

void DropThrown(const char* why) {
    const uintptr_t rocket = g_thrown.load(std::memory_order_relaxed);
    if (!rocket) return;
    LOG("rocket catch: thrown rocket %08X ended (%s)", static_cast<unsigned>(rocket), why);
    RemoveRocket(rocket);
    g_thrown = 0;
    g_thrownPhase = Held::None;
}

// The world time since the last call (seconds, at most 0.1, slowed with the world: the inventory's slow motion,
// "Vitesse du temps").
float WorldDt(LARGE_INTEGER& last) {
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
    last = now;
    return std::min(dt, 0.1f) * WorldTimeSpeed();
}

float Length(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// Its world pose as the engine made it (+0xA0: rows, translation).
bool WorldPose(uintptr_t rocket, float* pos, float* quat) {
    float m[16], rows[3][3];
    if (!mem::SafeRead(reinterpret_cast<void*>(rocket + 0xA0), m, sizeof(m))) return false;
    for (int i = 0; i < 3; ++i) {
        const float length = Length(m + i * 4);
        for (int j = 0; j < 3; ++j) rows[i][j] = length > 1e-4f ? m[i * 4 + j] / length : (i == j ? 1.0f : 0.0f);
    }
    RowsToQuat(rows, quat);
    pos[0] = m[12], pos[1] = m[13], pos[2] = m[14];
    return true;
}

// Its motion in his hand since the last sample (world time dt): speed, and spin = q_now x q_before^-1.
void SampleHand(uintptr_t rocket, float dt) {
    float pos[3], quat[4];
    if (!WorldPose(rocket, pos, quat)) return;
    if (g_haveSample && dt < 1e-4f) return;  // time stopped: nothing moved
    if (g_haveSample) {
        for (int i = 0; i < 3; ++i) g_now.vel[i] = (pos[i] - g_samplePos[i]) / dt;
        const float inverse[4] = { -g_sampleQuat[0], -g_sampleQuat[1], -g_sampleQuat[2], g_sampleQuat[3] };
        float w[4];
        QuatMul(quat, inverse, w);
        if (w[3] < 0.0f)
            for (float& c : w) c = -c;
        const float half = std::acos(std::clamp(w[3], -1.0f, 1.0f)), sinHalf = std::sin(half);
        for (int i = 0; i < 3; ++i) g_now.axis[i] = sinHalf > 1e-5f ? w[i] / sinHalf : (i == 1 ? 1.0f : 0.0f);
        g_now.rate = 2.0f * half / dt;
    }
    std::memcpy(g_samplePos, pos, sizeof(pos));
    std::memcpy(g_sampleQuat, quat, sizeof(quat));
    g_haveSample = true;
}

void WriteSimPose(uintptr_t rocket) {
    mem::SafeWrite(reinterpret_cast<void*>(rocket + 0x30), g_simPos, sizeof(g_simPos));
    mem::SafeWrite(reinterpret_cast<void*>(rocket + 0x40), g_simQuat, sizeof(g_simQuat));
}

// On the ground: flat (its long axis, local X, horizontal; up its local Y).
void LayFlat() {
    float rows[3][3];
    QuatRows(g_simQuat, rows);
    float x[3] = { rows[0][0], 0.0f, rows[0][2] };
    const float length = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (length < 1e-3f) x[0] = 1.0f, x[2] = 0.0f;
    else x[0] /= length, x[2] /= length;
    const float flat[3][3] = { { x[0], 0.0f, x[2] }, { 0.0f, 1.0f, 0.0f }, { -x[2], 0.0f, x[0] } };  // z = x cross y
    RowsToQuat(flat, g_simQuat);
}

// The held rocket's frame: true = its own update skipped.
bool TickHeld(uintptr_t rocket) {
    if (!g_held || rocket != g_held) return false;
    const float dt = WorldDt(g_heldLast);
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    if (!p1) {
        DropHeld("player 1 gone");
        return false;
    }
    {
        uint32_t motion = 0;
        float frame = 0.0f;
        mem::Read(p1 + 0x384, motion);
        mem::Read(p1 + 0x398, frame);
        const bool inCatch = (motion & 0xFFFF) == kCatchMotion;
        const bool catching = inCatch || static_cast<int32_t>(GetTickCount() - g_heldTick) < 300;  // his motion not started yet
        if (catching && !(inCatch && frame >= kThrowFrame)) {
            if (!g_flameOut && inCatch && frame >= kFlameOutFrame) {
                g_flameOut = true;
                ExtinguishRocket(rocket);
            }
            PoseInHand(rocket);
            CallPostUpdate(rocket);
            SampleHand(rocket, dt);
            return true;
        }
        if (!g_flameOut) {
            g_flameOut = true;
            ExtinguishRocket(rocket);
        }
        DropThrown("another rocket thrown");  // one in flight at a time (its pose below)
        // Tossed behind him, as his flick shows (user, 2026-10-25: with his hand's velocity at the throw it "bounced on
        // him" and fell in front of him): back along his facing and up; the hand's spin kept.
        g_flight = g_now;
        float facing[16]{}, along = 0.0f, up = g_now.vel[1];
        if (mem::SafeRead(reinterpret_cast<void*>(p1 + 0xA0), facing, sizeof(facing))) {
            float fx = facing[8], fz = facing[10];
            const float length = std::sqrt(fx * fx + fz * fz);
            if (length > 1e-3f) {
                fx /= length, fz /= length;
                along = g_now.vel[0] * fx + g_now.vel[2] * fz;  // the hand's speed forward (research)
                constexpr float kBack = 350.0f, kUp = 300.0f;  // cm/s
                g_flight.vel[0] = -fx * kBack;
                g_flight.vel[1] = kUp;
                g_flight.vel[2] = -fz * kBack;
            }
        }
        WorldPose(rocket, g_simPos, g_simQuat);
        DetachFromHand(rocket, p1);
        float feet[3]{};
        mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), feet, sizeof(feet));
        LOG("rocket catch: rocket %08X thrown at frame %.0f of motion %X, behind him (his hand: %.0f cm/s forward, %.0f up), "
            "spin %.1f rad/s", static_cast<unsigned>(rocket), frame, motion & 0xFFFF, along, up, g_flight.rate);
        g_floor = feet[1];
        g_hideWeapon = false;
        g_held = 0;
        g_heldPhase = Held::None;
        g_thrown = rocket;
        g_thrownPhase = Held::Falling;
        g_thrownLast.QuadPart = 0;
        WriteSimPose(rocket);
        CallPostUpdate(rocket);
        return true;
    }
}

// The thrown rocket's frame: true = its own update skipped.
bool TickThrown(uintptr_t rocket) {
    if (!rocket || rocket != g_thrown.load(std::memory_order_relaxed)) return false;
    const float dt = WorldDt(g_thrownLast);
    if (g_thrownPhase == Held::Falling) {
        for (int i = 0; i < 3; ++i) g_simPos[i] += g_flight.vel[i] * dt;
        g_flight.vel[1] -= 980.0f * dt;
        if (g_flight.rate > 0.0f) {
            const float half = 0.5f * g_flight.rate * dt, sinHalf = std::sin(half);
            const float step[4] = { g_flight.axis[0] * sinHalf, g_flight.axis[1] * sinHalf, g_flight.axis[2] * sinHalf, std::cos(half) };
            float q[4];
            QuatMul(step, g_simQuat, q);
            const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            for (int i = 0; i < 4; ++i) g_simQuat[i] = length > 1e-6f ? q[i] / length : (i == 3 ? 1.0f : 0.0f);
        }
        if (g_simPos[1] <= g_floor + 5.0f) {
            g_simPos[1] = g_floor + 5.0f;
            LayFlat();
            g_thrownPhase = Held::Lying;
            g_lying = 0.0f;
        }
        WriteSimPose(rocket);
        CallPostUpdate(rocket);
        return true;
    }
    g_lying += dt;
    if (g_lying < 3.0f) {
        WriteSimPose(rocket);
        CallPostUpdate(rocket);
        return true;
    }
    DropThrown("lay 3 s");
    return false;
}

// Sent back (user, 2026-10-25: with the knife slash as the parry's riposte, "a basic slash like the one of the knife
// aim, and the rocket back to its sender"; "it flies straight, so the other way round should do"): its nose (local +X)
// turned round, which turns its flight (its +0x1CF0, the player's shell's velocity in the flight step vtable +0x180 =
// RVA 0x7A2EA0, reads 0 on this one). It passed through the Majini (user): its hits take their targets from its owner
// (+0xFDC, followed up by RVA 0x7C26F0; its flight RVA 0x7B9570 builds its hit mask from the owner's player index when
// the owner is a uPlayer), so player 1 becomes its owner: it hits the enemies as his own rocket would and blows up on
// them by itself.
uintptr_t g_sentBack = 0;    // game thread
uint8_t g_sentState = 0, g_sentStep = 0;  // its state +0x14 and step +0x16 when sent back (in flight)
DWORD g_sentTick = 0;
DWORD g_divedTick = 0;  // when it was put into the ground at an enemy's feet

void SendBack(uintptr_t rocket, uintptr_t p1, const float* at, const float* launch) {
    FacePlayerTowards(p1, at);
    PlayKnifeAimSlash(p1);
    float quat[4]{};
    if (mem::SafeRead(reinterpret_cast<void*>(rocket + 0x40), quat, sizeof(quat))) {
        // A half turn about its own Y axis: q x (0, 1, 0, 0).
        const float turn[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
        float q[4];
        QuatMul(quat, turn, q);
        mem::SafeWrite(reinterpret_cast<void*>(rocket + 0x40), q, sizeof(q));
    }
    uint32_t owner = 0;
    mem::Read(rocket + 0xFDC, owner);
    mem::Write<uint32_t>(rocket + 0xFDC, static_cast<uint32_t>(p1));
    const float dx = at[0] - launch[0], dy = at[1] - launch[1], dz = at[2] - launch[2];
    g_sentBack = rocket;
    g_sentTick = GetTickCount();
    g_divedTick = 0;
    uint8_t state[3]{};
    mem::SafeRead(reinterpret_cast<void*>(rocket + 0x14), state, sizeof(state));
    g_sentState = state[0];
    g_sentStep = state[2];
    LOG("rocket catch: rocket %08X sent back (%.0f cm from its launch point, state %u / %u step %u), its owner %08X -> "
        "player 1", static_cast<unsigned>(rocket), std::sqrt(dx * dx + dy * dy + dz * dz), state[0], state[1], state[2], owner);
}

// Its blast as strong as player 1's rocket (user: the enemy rocket's blast hurts the Majini far less, its shooter not
// at all): the trainer's damage on every enemy within 4 m, blown off, besides the game's blast.
constexpr float kBlastRadius = 400.0f;
constexpr int kBlastDamage = 30000;

void Blast(uintptr_t rocket, const float* at, const char* how) {
    const int hit = RocketBlast(at, kBlastRadius, kBlastDamage);
    LOG("rocket catch: sent-back rocket %08X blown up %s: %d enem%s in its blast", static_cast<unsigned>(rocket), how, hit,
        hit == 1 ? "y" : "ies");
}

// The sent-back rocket's frame, before its update: its own blast (an enemy, a wall, the ground: its impact step +0x16 or
// its state moves on) gets player 1's rocket strength. Blown up by the trainer 80 cm before it touched the Majini, the
// trainer's blast killed him and threw him off first and the rocket passed through his body to blow up further on (user,
// 2026-10-25: "it killed him at the contact like a bullet, went on and blew up further"); its own impact cannot be
// started on demand (in flight it is in 1 / 2 step 0, gate +0x1CD8 0: writing them blew nothing up).
void TickSentBack(uintptr_t rocket) {
    if (!g_sentBack || rocket != g_sentBack) return;
    float at[3]{};
    uint8_t over = 0, state[3]{};
    const bool known = mem::SafeRead(reinterpret_cast<void*>(rocket + 0x30), at, sizeof(at)) && mem::Read(rocket + 0x1BD0, over) &&
                       mem::SafeRead(reinterpret_cast<void*>(rocket + 0x14), state, sizeof(state));
    if (!known || over || state[0] != g_sentState || state[2] != g_sentStep) {
        if (known) Blast(rocket, at, g_divedTick ? "at an enemy's feet" : "by itself");
        g_sentBack = 0;
        return;
    }
    if (g_divedTick) {
        // It should have hit the ground: the blast all the same, the rocket removed.
        if (static_cast<int32_t>(GetTickCount() - g_divedTick) > 500) {
            Blast(rocket, at, "at an enemy's feet (no impact of its own)");
            RemoveRocket(rocket);
            g_sentBack = 0;
        }
        return;
    }
    if (static_cast<int32_t>(GetTickCount() - g_sentTick) > 8000) {
        g_sentBack = 0;
        return;
    }
    // On an enemy (within 1 m of his body), its own hits passing through him even with player 1 as its owner (user,
    // 2026-10-25): put 25 cm above his feet, nose down, so that its next step hits the ground there and it blows up by
    // itself, the game's blast. It flies along its local -X: with +X down it went up to the sky (user).
    for (uintptr_t e : actors::Enemies()) {
        float pos[3]{};
        if (!mem::SafeRead(reinterpret_cast<void*>(e + 0x30), pos, sizeof(pos))) continue;
        const float height = std::clamp(at[1] - pos[1], 50.0f, 200.0f);  // the nearest point of his body
        const float ex = at[0] - pos[0], ey = at[1] - (pos[1] + height), ez = at[2] - pos[2];
        if (ex * ex + ey * ey + ez * ez >= 100.0f * 100.0f) continue;
        const float feet[3] = { pos[0], pos[1] + 25.0f, pos[2] };
        const float down[3][3] = { { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f } };  // -X down
        float quat[4];
        RowsToQuat(down, quat);
        mem::SafeWrite(reinterpret_cast<void*>(rocket + 0x30), feet, sizeof(feet));
        mem::SafeWrite(reinterpret_cast<void*>(rocket + 0x40), quat, sizeof(quat));
        g_divedTick = GetTickCount();
        LOG("rocket catch: sent-back rocket %08X on %s %08X: put into the ground at its feet", static_cast<unsigned>(rocket),
            UnitClassName(e), static_cast<unsigned>(e));
        return;
    }
}

}  // namespace

// action_log.cpp, player 1's RequestAction: his shot refused for 0.6 s after the slash that sent a rocket back (user:
// "sometimes Chris fires his weapon instead of the slash": the parry's press is also his fire key).
bool RocketSlashRefuses(uintptr_t player, int action) {
    constexpr int kWeaponFire = 0x1E;
    if (action != kWeaponFire || !g_sentTick || static_cast<int32_t>(GetTickCount() - g_sentTick) >= 600 ||
        player != actors::Player(0))
        return false;
    LOG("rocket catch: player 1's shot refused (the slash that sent a rocket back)");
    return true;
}

// movement.cpp: the knife slash that sent a rocket back keeps the camera free (user: locked during it).
bool RocketSlashPlaying() {
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    return g_sentTick && static_cast<int32_t>(GetTickCount() - g_sentTick) < 1500 && p1 && mem::Read(p1 + 0x10E0, action) &&
           action == 0x27;
}

namespace {

// true = caught now (its own update skipped from this frame).
bool CheckRocket(uintptr_t rocket, const char* name) {
    float at[3]{}, pos[3]{};
    uint8_t over = 0;
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    if (!p1 || !mem::Read(rocket + 0x1BD0, over) || over || !mem::SafeRead(reinterpret_cast<void*>(rocket + 0x30), at, sizeof(at)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), pos, sizeof(pos)))
        return false;
    const float dx = at[0] - pos[0], dy = at[1] - (pos[1] + 100.0f), dz = at[2] - pos[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (g_firstSeen.size() > 64) g_firstSeen.clear();
    const DWORD tick = GetTickCount();
    if (const auto old = g_firstSeen.find(rocket); old != g_firstSeen.end()) {
        const float* was = old->second.lastAt;
        const float jx = at[0] - was[0], jy = at[1] - was[1], jz = at[2] - was[2];
        if (static_cast<int32_t>(tick - old->second.last) > 500 || jx * jx + jy * jy + jz * jz > 300.0f * 300.0f)
            g_firstSeen.erase(old);  // a new rocket in the same object
    }
    const auto first = g_firstSeen.emplace(rocket, FirstSeen{ dist, { at[0], at[1], at[2] }, tick, { at[0], at[1], at[2] } });
    first.first->second.last = tick;
    std::memcpy(first.first->second.lastAt, at, sizeof(at));
    if (first.second)
        LOG("rocket catch: %s %08X seen %.0f cm from player 1%s", name, static_cast<unsigned>(rocket), dist,
            dist < 300.0f ? " (his own)" : "");
    if (first.first->second.distance < 300.0f) return false;  // fired by player 1
    if (rocket == g_sentBack) return false;
    if (dist >= 200.0f || g_held || static_cast<int32_t>(GetTickCount() - g_catchTick) < 1500 || !ParryCatchesRocket(p1, rocket))
        return false;
    g_catchTick = GetTickCount();
    if (ParryRiposteIsSlash()) {
        SendBack(rocket, p1, at, first.first->second.at);
        return false;  // its own update goes on, the other way
    }
    g_heldPlayer = p1;
    FacePlayerTowards(p1, at);
    const bool played = PlayRocketCatch();
    LOG("rocket catch: %s %08X caught %.0f cm away, catch %s", name, static_cast<unsigned>(rocket), dist,
        played ? "played" : "refused");
    if (!played) {
        EndRocket(rocket);
        return false;
    }
    g_flameOut = false;
    AttachToHand(rocket, p1);
    g_held = rocket;
    g_heldPhase = Held::InHand;
    g_heldTick = GetTickCount();
    g_heldLast.QuadPart = 0;
    g_hideWeapon = true;
    return true;
}

// "Esquive automatique des roquettes et des balles" (perfect_dodge.cpp, user 2026-10-29): the RPG Majini's rocket in
// flight nearest to player 1's chest, noted at each of its updates (game thread) for the render thread; not his own (a
// rocket first seen within 3 m of him, an object again "new" when not updated for 500 ms, as the catch's FirstSeen).
struct RocketOrigin {
    float distance;
    DWORD last;
};
std::unordered_map<uintptr_t, RocketOrigin> g_rocketOrigin;  // game thread
std::atomic<uintptr_t> g_incoming{ 0 };
std::atomic<DWORD> g_incomingTick{ 0 };
std::atomic<float> g_incomingAt[3]{}, g_incomingDistance{ 1e9f };

// The crossbow bolt flying nearest to player 1's chest and closing in on it (noted at its updates, game thread).
struct BoltSeen {
    float distance;
    DWORD last;
};
std::unordered_map<uintptr_t, BoltSeen> g_boltSeen;  // game thread
std::atomic<uintptr_t> g_bolt{ 0 };
std::atomic<DWORD> g_boltTick{ 0 };
std::atomic<float> g_boltDistance{ 1e9f };

void NoteBolt(uintptr_t bolt) {
    float at[3]{}, pos[3]{};
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    if (!p1 || !mem::SafeRead(reinterpret_cast<void*>(bolt + 0x30), at, sizeof(at)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), pos, sizeof(pos)))
        return;
    const float dx = at[0] - pos[0], dy = at[1] - (pos[1] + 100.0f), dz = at[2] - pos[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const DWORD now = GetTickCount();
    if (g_boltSeen.size() > 64) g_boltSeen.clear();
    BoltSeen& seen = g_boltSeen[bolt];
    const bool fresh = !seen.last || static_cast<int32_t>(now - seen.last) > 500;
    const bool closing = !fresh && seen.distance - dist > 2.0f;  // a stuck bolt does not move
    seen.distance = dist;
    seen.last = now;
    if (!closing) return;
    const DWORD noted = g_boltTick.load();
    if (g_bolt.load() != bolt && noted && static_cast<int32_t>(now - noted) < 100 && dist > g_boltDistance.load()) return;
    g_boltDistance = dist;
    g_bolt = bolt;
    g_boltTick = now ? now : 1;
}

void NoteIncoming(uintptr_t rocket) {
    float at[3]{}, pos[3]{};
    uint8_t over = 0;
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    if (!p1 || rocket == g_sentBack || !mem::Read(rocket + 0x1BD0, over) || over ||
        !mem::SafeRead(reinterpret_cast<void*>(rocket + 0x30), at, sizeof(at)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), pos, sizeof(pos)))
        return;
    const float dx = at[0] - pos[0], dy = at[1] - (pos[1] + 100.0f), dz = at[2] - pos[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const DWORD now = GetTickCount();
    if (g_rocketOrigin.size() > 64) g_rocketOrigin.clear();
    RocketOrigin& origin = g_rocketOrigin[rocket];
    if (!origin.last || static_cast<int32_t>(now - origin.last) > 500) origin.distance = dist;  // a new rocket
    origin.last = now;
    if (origin.distance < 300.0f) return;  // fired by player 1
    const DWORD noted = g_incomingTick.load();
    if (g_incoming.load() != rocket && noted && static_cast<int32_t>(now - noted) < 100 && dist > g_incomingDistance.load())
        return;  // another one nearer
    for (int c = 0; c < 3; ++c) g_incomingAt[c] = at[c];
    g_incomingDistance = dist;
    g_incoming = rocket;
    g_incomingTick = now ? now : 1;
}

// Only the RPG Majini's rocket is caught (user, 2026-10-25: a Molotov cocktail's fire started the catch: uWp6c, three of
// them at a time, caught at 2 m); the other classes stay hooked for the held one only.
bool RocketFrame(void* self, int index) {
    const uintptr_t rocket = reinterpret_cast<uintptr_t>(self);
    if (TickHeld(rocket) || TickThrown(rocket)) return true;
    TickSentBack(rocket);
    if (index == 0) NoteIncoming(rocket);
    if (index == 3) {
        NoteBolt(rocket);
        return false;
    }
    return index == 0 && g_catchOn.load(std::memory_order_relaxed) && CheckRocket(rocket, kClasses[index].name);
}

// The rockets fly a fixed step per frame, not by the world's time speed (user, 2026-10-25: in the inventory's slow
// motion the enemies froze, a rocket already fired kept going and blew Chris out of the inventory): below x1 their
// update runs that share of the frames (a debt per rocket: 0.25 = one frame in four, 0 = never). Above x1 they keep one
// update a frame (a second one could run on a rocket the first one ended).
std::unordered_map<uintptr_t, float> g_timeDebt;  // game thread

bool UpdateThisFrame(void* self) {
    const float speed = WorldTimeSpeed();
    if (speed >= 1.0f) return true;
    if (g_timeDebt.size() > 64) g_timeDebt.clear();
    float& debt = g_timeDebt[reinterpret_cast<uintptr_t>(self)];
    debt += speed;
    if (debt < 1.0f) return false;
    debt -= 1.0f;
    return true;
}

void RunUpdate(void* self, void* edx, int index) {
    if (!RocketFrame(self, index) && UpdateThisFrame(self)) g_original[index](self, edx);
}

void __fastcall HookUpdate0(void* self, void* edx) { RunUpdate(self, edx, 0); }
void __fastcall HookUpdate1(void* self, void* edx) { RunUpdate(self, edx, 1); }
void __fastcall HookUpdate2(void* self, void* edx) { RunUpdate(self, edx, 2); }
void __fastcall HookUpdate3(void* self, void* edx) { RunUpdate(self, edx, 3); }

// Player 1's weapon hidden from the catch to the end of his catch motion (user: the shotgun came back before the end of
// the animation, at the rocket's release).
bool WeaponHidden() {
    if (g_hideWeapon.load(std::memory_order_relaxed)) return true;
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    uint32_t motion = 0;
    return p1 && mem::Read(p1 + 0x384, motion) && (motion & 0xFFFF) == kCatchMotion;
}

// The hold given up without touching the rocket (user, 2026-10-25: the game was quit to its menus during the catch, with
// time stopped; the hold stayed: player 1's weapons hidden for good, no rocket caught any more): out of the game (menus,
// loads), another player 1, or the rocket no longer one of ours (its memory may be another object by then).
bool StillARocket(uintptr_t rocket) {
    uintptr_t vtable = 0;
    bool ours = false;
    if (mem::Read(rocket, vtable))
        for (const RocketClass& c : kClasses) ours |= vtable == reinterpret_cast<uintptr_t>(mem::Rva(c.vtable));
    return ours;
}

void CheckHeldStillValid() {
    if (const uintptr_t thrown = g_thrown.load(std::memory_order_relaxed);
        thrown && (!actors::InGame() || !StillARocket(thrown))) {
        LOG("rocket catch: thrown rocket %08X given up", static_cast<unsigned>(thrown));
        g_thrown = 0;
        g_thrownPhase = Held::None;
    }
    if (!g_held) return;
    const char* why = !actors::InGame()                   ? "out of the game"
                      : actors::Player(0) != g_heldPlayer ? "another player 1"
                      : !StillARocket(g_held)             ? "the rocket is gone"
                                                          : nullptr;
    if (!why) return;
    LOG("rocket catch: held rocket %08X given up (%s)", static_cast<unsigned>(g_held), why);
    g_held = 0;
    g_heldPhase = Held::None;
    g_inHand = 0;
    g_hideWeapon = false;
}

// Render thread: player 1's weapon hidden while the rocket is in his hands (byte +5 bit 0x08 of his weapon in hand
// [P+0x2634] = drawn, player.cpp), given back after.
bool g_weaponHidden = false, g_weaponWasDrawn = false;

void TickWeaponHide() {
    bool& hidden = g_weaponHidden;
    bool& wasDrawn = g_weaponWasDrawn;
    CheckHeldStillValid();
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t weapon = 0;
    uint8_t flags = 0;
    if (!p1 || !mem::Read(p1 + 0x2634, weapon) || !weapon || !mem::Read(weapon + 5, flags)) {
        hidden = false;
        return;
    }
    if (WeaponHidden()) {
        if (!hidden) wasDrawn = (flags & 0x08) != 0;
        hidden = true;
        if (flags & 0x08) mem::Write<uint8_t>(weapon + 5, flags & ~0x08);
    } else if (hidden) {
        hidden = false;
        if (wasDrawn && !(flags & 0x08)) mem::Write<uint8_t>(weapon + 5, flags | 0x08);
    }
}

bool EnsureRocketHooks() {
    static bool done = false;
    if (done) return true;
    void* const hooks[kClassCount] = { reinterpret_cast<void*>(&HookUpdate0), reinterpret_cast<void*>(&HookUpdate1),
                                       reinterpret_cast<void*>(&HookUpdate2), reinterpret_cast<void*>(&HookUpdate3) };
    for (int i = 0; i < kClassCount; ++i) {
        const RocketClass& c = kClasses[i];
        const uintptr_t vtable = reinterpret_cast<uintptr_t>(mem::Rva(c.vtable));
        uintptr_t getDti = 0, update = 0;
        if (!mem::Read(vtable + 4 * 4, getDti) || getDti != reinterpret_cast<uintptr_t>(mem::Rva(c.getDti)) ||
            !mem::Read(vtable + 4 * 6, update) || update != reinterpret_cast<uintptr_t>(mem::Rva(c.update))) {
            LOG("rocket catch: unexpected %s vtable (slot 4 %08X, slot 6 %08X)", c.name, static_cast<unsigned>(getDti),
                static_cast<unsigned>(update));
            return false;
        }
        g_original[i] = reinterpret_cast<UpdateFn>(update);
        g_patch[i] = new mem::Patch("rocket update", c.vtable + 4 * 6, Bytes(update), Bytes(reinterpret_cast<uintptr_t>(hooks[i])));
        if (!g_patch[i]->Apply()) {
            LOG("rocket catch: %s update not redirected", c.name);
            for (int k = 0; k <= i; ++k) g_patch[k]->Restore();
            return false;
        }
    }
    OnRestore([] {
        for (mem::Patch* patch : g_patch)
            if (patch) patch->Restore();
    });
    done = true;
    LOG("rocket catch: uWp83, uWp44, uWp6c and uWp81 updates redirected");
    return true;
}
}  // namespace

// time_speed.cpp: the rockets follow the world's time speed (their updates redirected).
bool EnsureRocketTimeHooks() { return EnsureRocketHooks(); }

// parry.cpp: the crossbow bolt closing in on player 1's chest (noted within 100 ms), and its distance.
bool IncomingBolt(uintptr_t& bolt, float& distance) {
    const DWORD noted = g_boltTick.load();
    if (!noted || static_cast<int32_t>(GetTickCount() - noted) > 100 || !g_bolt.load()) return false;
    bolt = g_bolt.load();
    distance = g_boltDistance.load();
    return true;
}

// The bolts followed (their update redirected).
bool EnsureBoltHooks() { return EnsureRocketHooks(); }

// perfect_dodge.cpp: the enemy rocket in flight nearest to player 1's chest (noted within 100 ms), its place and distance.
bool IncomingRocket(float* at, float& distance) {
    const DWORD noted = g_incomingTick.load();
    if (!noted || static_cast<int32_t>(GetTickCount() - noted) > 100 || !g_incoming.load()) return false;
    for (int c = 0; c < 3; ++c) at[c] = g_incomingAt[c].load();
    distance = g_incomingDistance.load();
    return true;
}

// player.cpp's hook of the units' world matrix (RVA 0x08ABA0): the rocket in player 1's hand gets its pose in the hand,
// the thrown one its flight's (rows of the rotation, then the offset).
bool RocketLocalMatrix(uintptr_t unit, float* m) {
    if (unit && unit == g_thrown.load(std::memory_order_relaxed)) {
        float rows[3][3];
        QuatRows(g_simQuat, rows);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) m[i * 4 + j] = rows[i][j];
            m[i * 4 + 3] = 0.0f;
        }
        m[12] = g_simPos[0], m[13] = g_simPos[1], m[14] = g_simPos[2], m[15] = 1.0f;
        return true;
    }
    if (!unit || unit != g_inHand.load(std::memory_order_relaxed)) return false;
    HandLocal(m);
    return true;
}

// player.cpp ("Arme toujours visible"): player 1's weapon stays hidden during the rocket catch.
bool RocketCatchHidesWeapon() { return WeaponHidden(); }

// movement.cpp: the RE6 locomotion keeps the camera free through the whole catch.
bool RocketCatchPlaying() { return WeaponHidden(); }

void RegisterRocketCatch() {
    OnFrame(TickWeaponHide);
    // Unloaded during a catch: the rocket let go and player 1's weapon given back.
    OnRestore([] {
        CheckHeldStillValid();
        if (g_held) DropHeld("trainer unloaded");
        DropThrown("trainer unloaded");
        const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
        uintptr_t weapon = 0;
        uint8_t flags = 0;
        if (g_weaponHidden && g_weaponWasDrawn && p1 && mem::Read(p1 + 0x2634, weapon) && weapon &&
            mem::Read(weapon + 5, flags))
            mem::Write<uint8_t>(weapon + 5, flags | 0x08);
        g_weaponHidden = false;
    });
    Add({ .id = "combat.parry_rocket", .name = "Rattraper les roquettes (parade)",
          .help = "Quand une roquette ennemie arrive sur Chris et que tu pares au bon moment (parade au couteau ; pas la "
                  "parade façon RE6) : avec la riposte « Coup de couteau », Chris la renvoie d'un coup de couteau vers son "
                  "tireur ; avec la garde de Wesker ou une projection (Chris, Josh), il la rattrape comme Wesker (elle n'explose "
                  "pas, sa flamme s'éteint puis il la jette derrière lui).",
          .category = "Parade", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureRocketHooks() || !EnsureAttachedMatrixHook() || !EnsurePlayerRequestHook())) return false;
              if (on && actors::InGame()) PrepareWeskerMoves();
              g_catchOn = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
}
}  // namespace features
