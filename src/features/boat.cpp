// The airboat's driver aims and fires (user, 2026-10-28, chapter 3-1, the marshes: "the player driving the boat can do
// nothing else, it is the other one who aims and fires"; checked by default). The airboat (uOm1306 under uOmBoatAir)
// carries both players: P+0x18 = the boat, P+0x364 = 1 (0x101 on foot). The driver stays in action 0x6A
// cPlAction_pl_act_boatair_wait (update VA 0xC74BE0: only the steering motions, from P+0x2E38 / +0x2E3C / +0x2E40),
// which never looks at the aim button; the passenger stands in the usual wait 0 and aims as on foot. With the option on,
// the driver holding the aim button is asked the aim 0x1B (weapon_ready_begin) like the wait's own, and once back in
// the wait 0 with the button let go, the driving 0x6A again.
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace features {
bool EnsurePadQueryHooks();                // quick_knife.cpp: BoatTick runs on the game thread
bool GameButtonHeld(int button, int pad);  // quick_knife.cpp
int Player1Pad();                          // movement.cpp: the pad index player 1's buttons are read with

namespace {
constexpr uint32_t kDrive = 0x6A;     // cPlAction_pl_act_boatair_wait
constexpr uint32_t kAimBegin = 0x1B;  // cPlAction_pl_act_weapon_ready_begin
constexpr int kAimButton = 0x0C;
std::atomic<bool> g_on{ false };
bool g_aimed = false;   // game thread: the driver was taken out of the driving to aim
DWORD g_lastAsk = 0;

// The driver as he was driving (user, 2026-10-28: aiming he came down from his platform, Sheva in front hid
// everything; back at the wheel he kept the aim's facing, askew). His position +0x30 and rotation +0x40 are local to
// the boat (P+0x18, a root unit: its +0x30 / quaternion +0x40 are world ones); the driving motion holds his hips about
// 163 cm above the boat's origin and 90 cm behind it (standing on the platform), his origin staying at 0.
struct Driving {
    float pos[3]{}, rot[4]{}, yaw = 0.0f, aimYaw = 0.0f;
    float hips[3]{};  // his hips in the boat's frame
};
Driving g_driving;

struct Vec {
    float x, y, z;
};

// `v` turned by the inverse of the unit quaternion (x, y, z, w).
Vec Unrotate(const float (&q)[4], Vec v) {
    const float qx = -q[0], qy = -q[1], qz = -q[2], qw = q[3];
    const Vec t{ 2.0f * (qy * v.z - qz * v.y), 2.0f * (qz * v.x - qx * v.z), 2.0f * (qx * v.y - qy * v.x) };
    return { v.x + qw * t.x + (qy * t.z - qz * t.y), v.y + qw * t.y + (qz * t.x - qx * t.z),
             v.z + qw * t.z + (qx * t.y - qy * t.x) };
}

// His hips (joint 0's world matrix row +0x80 of [P+0x318]) in the boat's frame.
bool HipsInBoat(uintptr_t p, uintptr_t boat, float (&out)[3]) {
    uintptr_t joints = 0;
    float hips[3]{}, at[3]{}, rot[4]{};
    if (!mem::Read(p + 0x318, joints) || !joints || !mem::SafeRead(reinterpret_cast<void*>(joints + 0x80), hips, sizeof(hips)) ||
        !mem::SafeRead(reinterpret_cast<void*>(boat + 0x30), at, sizeof(at)) ||
        !mem::SafeRead(reinterpret_cast<void*>(boat + 0x40), rot, sizeof(rot)))
        return false;
    const Vec local = Unrotate(rot, { hips[0] - at[0], hips[1] - at[1], hips[2] - at[2] });
    if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || std::fabs(local.x) > 500.0f ||
        std::fabs(local.y) > 500.0f || std::fabs(local.z) > 500.0f)
        return false;
    out[0] = local.x;
    out[1] = local.y;
    out[2] = local.z;
    return true;
}

bool SaveDriving(uintptr_t p, uintptr_t boat) {
    Driving d;
    if (!HipsInBoat(p, boat, d.hips) || !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), d.pos, sizeof(d.pos)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p + 0x40), d.rot, sizeof(d.rot)) || !mem::Read(p + 0x2A10, d.yaw) ||
        !mem::Read(p + 0x2A24, d.aimYaw))
        return false;
    g_driving = d;
    return true;
}

// Aiming: his origin moved so that his hips stay where the driving held them (on the platform, turning about them).
// The aim's walk (movement.cpp) is off aboard a vehicle (user: no moving while aiming in the boat).
// The hips' offset from his origin is taken in one frame's pose (joint 0's world row and his own world matrix row
// +0xD0 of +0xA0, both made by the same update: the tick runs many times a frame, his +0x30 may be newer), turned
// into the boat's frame (his rotation is local to it).
void KeepOnPlatform(uintptr_t p, uintptr_t boat) {
    uintptr_t joints = 0;
    float hips[3]{}, origin[3]{}, rot[4]{}, pos[3]{};
    if (!mem::Read(p + 0x318, joints) || !joints || !mem::SafeRead(reinterpret_cast<void*>(joints + 0x80), hips, sizeof(hips)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p + 0xD0), origin, sizeof(origin)) ||
        !mem::SafeRead(reinterpret_cast<void*>(boat + 0x40), rot, sizeof(rot)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos)))
        return;
    const Vec offset = Unrotate(rot, { hips[0] - origin[0], hips[1] - origin[1], hips[2] - origin[2] });
    if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z) || std::fabs(offset.x) > 200.0f ||
        std::fabs(offset.y) > 250.0f || std::fabs(offset.z) > 200.0f)
        return;
    const float wanted[3] = { g_driving.hips[0] - offset.x, g_driving.hips[1] - offset.y, g_driving.hips[2] - offset.z };
    for (int i = 0; i < 3; ++i)
        if (std::fabs(wanted[i] - pos[i]) > 0.5f) mem::Write<float>(p + 0x30 + 4 * i, wanted[i]);
}

void RestoreDriving(uintptr_t p) {
    for (int i = 0; i < 3; ++i) mem::Write<float>(p + 0x30 + 4 * i, g_driving.pos[i]);
    for (int i = 0; i < 4; ++i) mem::Write<float>(p + 0x40 + 4 * i, g_driving.rot[i]);
    mem::Write<float>(p + 0x2A10, g_driving.yaw);
    mem::Write<float>(p + 0x2A24, g_driving.aimYaw);
}

// A class chain holding uOmBoatAir (GetDTI = vtable slot 4, `mov eax, dti; ret`; DTI +4 name, +0x10 parent), by vtable.
bool IsBoat(uintptr_t unit) {
    static uintptr_t vtables[4] = {};
    static bool results[4] = {};
    static int next = 0;
    uintptr_t vtable = 0;
    if (!unit || !mem::Read(unit, vtable) || !vtable) return false;
    for (int i = 0; i < 4; ++i)
        if (vtables[i] == vtable) return results[i];
    bool boat = false;
    uintptr_t fn = 0, dti = 0;
    uint8_t code[6]{};
    if (mem::Read(vtable + 0x10, fn) && mem::SafeRead(reinterpret_cast<void*>(fn), code, sizeof(code)) && code[0] == 0xB8 &&
        code[5] == 0xC3) {
        std::memcpy(&dti, code + 1, 4);
        for (int depth = 0; dti && depth < 8 && !boat; ++depth) {
            uintptr_t name = 0;
            char text[32]{};
            if (!mem::Read(dti + 4, name) || !name || !mem::SafeRead(reinterpret_cast<void*>(name), text, sizeof(text) - 1))
                break;
            boat = std::strcmp(text, "uOmBoatAir") == 0;
            if (!mem::Read(dti + 0x10, dti)) break;
        }
    }
    vtables[next] = vtable;
    results[next] = boat;
    next = (next + 1) % 4;
    return boat;
}

void Request(uintptr_t p, uint32_t action) {
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(p, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return;
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(p), nullptr, static_cast<int>(action), 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Tick() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t boat = 0;
    uint32_t action = 0;
    if (!p || actors::OnFoot(p) || !mem::Read(p + 0x18, boat) || !IsBoat(boat) || !mem::Read(p + 0x10E0, action)) {
        g_aimed = false;
        return;
    }
    if (g_aimed && action != kDrive && action < 0x80) KeepOnPlatform(p, boat);  // not in a damage / event action
    const DWORD now = GetTickCount();
    if (now - g_lastAsk < 150) return;  // one request at a time
    const bool aim = GameButtonHeld(kAimButton, Player1Pad());
    if (action == kDrive) {
        if (g_aimed) {  // back at the wheel: as he was before the aim
            RestoreDriving(p);
            g_aimed = false;
        }
        if (!aim || !SaveDriving(p, boat)) return;
        // The camera turned while driving (movement.cpp: the free camera sets P+0x2A15, its yaw P+0x2A10 local to the
        // boat): he aims where it looks, as on foot.
        uint8_t turned = 0;
        if (mem::Read(p + 0x2A15, turned) && turned) {
            const float yaw = g_driving.yaw, rot[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
            for (int i = 0; i < 4; ++i) mem::Write<float>(p + 0x40 + 4 * i, rot[i]);
            mem::Write<float>(p + 0x1024, yaw);
            mem::Write<float>(p + 0x2A24, yaw);
        }
        Request(p, kAimBegin);
        g_aimed = true;
        g_lastAsk = now;
        mem::Read(p + 0x10E0, action);
        LOG("boat: the driver aims (action now %X; his hips at %.0f %.0f %.0f in the boat)", action, g_driving.hips[0],
            g_driving.hips[1], g_driving.hips[2]);
        return;
    }
    if (g_aimed && action == 0 && !aim) {
        Request(p, kDrive);
        g_lastAsk = now;
        mem::Read(p + 0x10E0, action);
        if (action == kDrive) RestoreDriving(p);
        LOG("boat: the driver drives again (action now %X)", action);
    }
}
}  // namespace

// quick_knife.cpp's pad query hook (game thread). The button query below goes through that hook again.
void BoatTick() {
    static bool inside = false;
    if (inside || !g_on.load(std::memory_order_relaxed)) return;
    inside = true;
    Tick();
    inside = false;
}

void RegisterBoat() {
    Add({ .id = "player.boat_driver_aim", .name = "Bateau : le pilote vise et tire",
          .help = "Dans le bateau des marais, le joueur qui pilote peut aussi viser et tirer : en tenant la visée il "
                  "lâche le volant pour viser, et reprend le pilotage dès qu'il relâche la visée.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsurePadQueryHooks()) return false;
              g_on = on;
              return true;
          },
          .available = [] { return game::IsSupported(); }, .defaultOn = true });
}
}  // namespace features
