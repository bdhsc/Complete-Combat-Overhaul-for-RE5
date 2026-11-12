#pragma once
#include <cstdint>

namespace features {
// env_exec.cpp: one sweep of the game's ledge probe (VA 0x76FF80).
struct EnvHit {
    bool hit = false;
    uint32_t flags = 0, bits = 0;
    float point[3]{};   // where it met (or its end)
    float top[3]{};     // +0x70: a point 40 cm inside (on a low top)
    float normal[3]{};  // the obstacle's plane, its normal towards the side swept from
    float d = 0.0f;     // n . x + d = 0
};
bool EnvProbe(const float* origin, float yaw, float height, float reach, float radius, EnvHit& hit);
bool EnvSweep(const float* origin, float yaw, const float* from, const float* to, float radius, EnvHit& hit);
}  // namespace features
