// RE6's environmental executions (user, 2026-11-11: "the environmental executions next: the jump from above, smashing the head on a wall
// or on a ledge - a table, a barrier"; "normally it is from this kind of height that I can trigger the execution from below if an enemy
// is staggered on the ground under me" - a 100 cm ledge). Their pairs are in re6_melee.lmt already (docs/RE6_MELEE.md: n° 17 / 18 the
// jump from above, 19 / 20 the walls, 21-24 the tables and low barriers, with the J'avo's sides em1000CoC 3A-48 at 152-159).
//
// The level is read with the game's own ledge probe (the one the players' crossings use: VA 0x76FF80, thiscall on the collision
// system [VA 0x1249C64], 11 arguments, ret 0x2C; it fills a 0xE0 block - +0x2AB0 of a player, every frame straight ahead of him from VA
// 0xB78E28, MODLOG "Obstacles : grimper seul"). First its arguments are logged on the players' own call (research).
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "features/env_exec.hpp"

#include <windows.h>
#include <MinHook.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace features {
namespace {

bool HookAt(uintptr_t rva, const std::vector<uint8_t>& expected, void* detour, void** original) {
    uint8_t* target = mem::Rva(rva);
    if (!mem::IsGameCode(target, expected.size()) || std::memcmp(target, expected.data(), expected.size()) != 0) {
        LOG("environmental executions: unexpected bytes %s at RVA %X", mem::Hex(target, expected.size()).c_str(), static_cast<unsigned>(rva));
        return false;
    }
    mem::EnsureExecutable(target, expected.size());
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    return st == MH_OK || st == MH_ERROR_ENABLED;
}

// The ledge probe: thiscall(collision, a, b, c, f, out, i1, mask, i2, k, z, last), ret 0x2C.
using ProbeFn = uint32_t(__fastcall*)(void* self, void* edx, const float* a, const float* b, const float* c, float f, uint8_t* out,
                                      int i1, uint32_t mask, int i2, float k, float z, int last);
ProbeFn g_probe = nullptr;
std::atomic<bool> g_probeLog{ false };

void LogVec(char* buf, size_t n, const char* name, const float* v) {
    float x[4]{};
    if (v && mem::SafeRead(const_cast<float*>(v), x, sizeof(x)))
        std::snprintf(buf, n, "%s (%.1f %.1f %.1f %.2f)", name, x[0], x[1], x[2], x[3]);
    else
        std::snprintf(buf, n, "%s -", name);
}

uint32_t __fastcall HookProbe(void* self, void* edx, const float* a, const float* b, const float* c, float f, uint8_t* out, int i1,
                              uint32_t mask, int i2, float k, float z, int last) {
    const uint32_t result = g_probe(self, edx, a, b, c, f, out, i1, mask, i2, k, z, last);
    if (!g_probeLog.load(std::memory_order_relaxed)) return result;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p || reinterpret_cast<uintptr_t>(out) != p + 0x2AB0) return result;
    static ULONGLONG told = 0;
    if (GetTickCount64() - told < 1000) return result;
    told = GetTickCount64();
    char va[64], vb[64], vc[64];
    LogVec(va, sizeof(va), "a", a);
    LogVec(vb, sizeof(vb), "b", b);
    LogVec(vc, sizeof(vc), "c", c);
    float pos[3]{}, quat[4]{}, blk[0x38]{};
    mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos));
    mem::SafeRead(reinterpret_cast<void*>(p + 0x40), quat, sizeof(quat));
    mem::SafeRead(out, blk, sizeof(blk));
    uint32_t flags[4]{}, bits = 0;
    mem::SafeRead(out, flags, sizeof(flags));
    mem::Read(reinterpret_cast<uintptr_t>(out) + 0xA0, bits);
    LOG("env probe research: ret %X; %s %s %s f %.2f i1 %d mask %08X i2 %d k %.3f z %.3f last %d; player (%.1f %.1f %.1f) yaw %.1f; out "
        "%X %X %X %X bits %08X, +20 (%.1f %.1f %.1f) +30 (%.1f %.1f %.1f) +70 (%.1f %.1f %.1f) +80 (%.2f %.2f %.2f %.1f)",
        result, va, vb, vc, f, i1, mask, i2, k, z, last, pos[0], pos[1], pos[2], 2.0f * std::atan2(quat[1], quat[3]) * 57.2958f,
        flags[0], flags[1], flags[2], flags[3], bits, blk[8], blk[9], blk[10], blk[12], blk[13], blk[14], blk[28], blk[29], blk[30],
        blk[32], blk[33], blk[34], blk[35]);
    return result;
}

// One sweep of the probe for the trainer (the environmental executions: walls and low obstacles round a staggered enemy). Only plain data
// here (a fault in the game's code is caught).
uint32_t CallProbe(uintptr_t collision, const float* a, const float* b, const float* c, float yaw, uint8_t* out, float radius) {
    const ProbeFn fn = reinterpret_cast<ProbeFn>(mem::Rva(0x36FF80));
    __try {
        return fn(reinterpret_cast<void*>(collision), nullptr, a, b, c, yaw, out, 1, 0x3FFFFF00, 1, radius, 0.0f, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFFFFFF;
    }
}

bool EnsureProbeHook() {
    static bool hooked = false;
    if (hooked) return true;
    // push ebx; push ebp; push esi; mov esi, [esp+0x20]; push edi
    hooked = HookAt(0x36FF80, { 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x20, 0x57 }, reinterpret_cast<void*>(&HookProbe),
                    reinterpret_cast<void**>(&g_probe));
    LOG("environmental executions: ledge probe hook %s", hooked ? "installed" : "refused");
    return hooked;
}

}  // namespace

// The game's ledge probe swept from `origin` (a unit's feet) along `yaw` (radians, +z ahead = (sin, cos)) at `height` above it, `reach` cm,
// a sphere of `radius`. A hit gives the obstacle's plane: its normal (towards the side swept from) and d (n . x + d = 0), and the point
// met. Read live 2026-11-11: the game's own sweep (0, 40, 0) -> (0, 40, 150) from player 1's feet, r 35; returns 0 when nothing is met
// (its end point at +0x20), else flags (4 a wall: bits +0xA0 0x100; 0x18 a low top, +0x70 on it).
bool EnvProbe(const float* origin, float yaw, float height, float reach, float radius, EnvHit& hit) {
    const float from[3] = { 0.0f, height, 0.0f }, to[3] = { 0.0f, height, reach };
    return EnvSweep(origin, yaw, from, to, radius, hit);
}

// Any segment, in the frame of `origin` turned by `yaw` (x, height, ahead): a sweep down finds a top (its plane's normal up).
bool EnvSweep(const float* origin, float yaw, const float* from, const float* to, float radius, EnvHit& hit) {
    hit = {};
    uintptr_t collision = 0;
    if (!game::IsSupported() || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C64)), collision) || !collision) return false;
    if (!mem::IsGameCode(mem::Rva(0x36FF80), 8)) return false;
    alignas(16) float a[4] = { from[0], from[1], from[2], 0.0f };
    alignas(16) float b[4] = { to[0], to[1], to[2], 0.0f };
    alignas(16) float c[4] = { origin[0], origin[1], origin[2], 0.0f };
    alignas(16) uint8_t out[0x100]{};
    const uint32_t result = CallProbe(collision, a, b, c, yaw, out, radius);
    if (result == 0xFFFFFFFF) {
        LOG("environmental executions: the ledge probe faulted");
        return false;
    }
    hit.flags = result;
    std::memcpy(&hit.bits, out + 0xA0, sizeof(hit.bits));
    std::memcpy(hit.point, out + 0x20, sizeof(hit.point));
    std::memcpy(hit.top, out + 0x70, sizeof(hit.top));
    float plane[4]{};
    std::memcpy(plane, out + 0x80, sizeof(plane));
    if (!result) return false;
    for (float v : plane)
        if (!std::isfinite(v)) return false;
    const float len = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    if (len < 0.5f) return false;
    for (int i = 0; i < 3; ++i) hit.normal[i] = plane[i] / len;
    hit.d = plane[3] / len;
    hit.hit = true;
    return true;
}

void RegisterEnvExec() {
    Add({ .id = "diag.env_probe_log", .name = "Journal de la sonde de rebord (recherche)",
          .help = "Écrit dans trainer.log, une fois par seconde, les arguments et le résultat de la sonde de rebord que le jeu lance "
                  "devant le joueur 1 (recherche pour les exécutions environnementales de RE6).",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureProbeHook()) return false;
              g_probeLog = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
}
}  // namespace features
