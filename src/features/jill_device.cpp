// Jill's device in the second part of her fight (uEm84, vtable RVA 0xB90728; user 2026-10-26: the Ultimate Trainer's
// "Jill's Device" - its health shown, set and frozen - and in solo the device is hard to damage). MODLOG 2026-10-26:
// - Jill +0x1FCC = the device's health (int32): 10000 at her start (RVA 0x547CB9), -1500 for each hit on its part in
//   her damage reaction (RVA 0x538EF5; none while she reels from such a hit, DmgCore 0x59 / 0x5A: the bullets that
//   "bounced"), -30 for each press of the rip-off prompt (RVA 0x53C28C: at 0 the device comes off). Below 1000 a game
//   flag (0x11, RVA 0x54E339).
// - At the end of a throw (her Caught sets [Jill+0x1BA0] = 1) her AI's state RVA 0x5CD6F0 compares it with the float
//   at VA 0xF98678 (7000.0, also read at RVA 0x799989): at or below she stays down (Sleep 0x5C, where the rip-off
//   prompt comes), above she gets up (DmgGetup 0x60). 50 throws at 10000 never kept her down, 3333 did at once (user).
//   The threshold option points that comparison (its operand, RVA 0x5CD714) at the trainer's own value.
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace features {
bool EnsureActorDamageHook();  // player.cpp: the actor SubHealth hook (RVA 0x44F7C0)

namespace {
constexpr uintptr_t kJillVtable = 0xB8EA40;   // uEm83, the first part (RVA)
constexpr uintptr_t kJill2Vtable = 0xB90728;  // uEm84 (RVA)
constexpr uintptr_t kDevice = 0x1FCC;         // int32
constexpr int32_t kDeviceMax = 10000;
constexpr float kGamePinThreshold = 7000.0f;

uintptr_t FindJill2() {
    if (!actors::InGame()) return 0;
    const uintptr_t vtable = reinterpret_cast<uintptr_t>(mem::Rva(kJill2Vtable));
    for (uintptr_t e : actors::Enemies()) {
        uintptr_t v = 0;
        if (mem::Read(e, v) && v == vtable) return e;
    }
    return 0;
}

bool ReadDevice(uintptr_t jill, int32_t& device) { return jill && mem::Read(jill + kDevice, device); }

// ---- "Jill reste au sol sous" (the threshold after a throw).
float g_pinThreshold = kGamePinThreshold;  // read by the game through the patched operand
std::atomic<bool> g_pinOn{ false };

std::vector<uint8_t> AddressBytes(const void* p) {
    const uint32_t a = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    return { static_cast<uint8_t>(a), static_cast<uint8_t>(a >> 8), static_cast<uint8_t>(a >> 16), static_cast<uint8_t>(a >> 24) };
}
// movss xmm1, [0x00F98678] at RVA 0x5CD710: its address operand.
mem::Patch g_pinPatch("jill pin threshold", 0x5CD714, { 0x78, 0x86, 0xF9, 0x00 }, AddressBytes(&g_pinThreshold));

// ---- "Figer le device de Jill".
uintptr_t g_frozenJill = 0;  // render thread
int32_t g_frozenDevice = 0;

// ---- "Corps à corps libre : le device de Jill à la place de sa vie".
std::atomic<int> g_meleeDevice{ 0 };  // device damage per hit, 0 = off

// ---- "Figer la vie de Jill" (user, 2026-10-26: a bullet that misses the device must not kill her): her SubHealth
// refused (player.cpp's damage hook), the trainer's moves leave her health too (wesker_moves.cpp).
std::atomic<bool> g_keepJillHealth{ false };

// ---- "Jauge du device de Jill": its health top left, as Wesker's gauge, and Jill's own (user, 2026-10-26).
std::atomic<bool> g_gaugeShown{ false };

void DrawDevice() {
    if (!g_gaugeShown.load(std::memory_order_relaxed)) return;
    const uintptr_t jill = FindJill2();
    int32_t device = 0;
    int16_t health = 0, healthMax = 0;
    if (!ReadDevice(jill, device) || !mem::Read(jill + 0x1364, health) || !mem::Read(jill + 0x1366, healthMax)) return;
    char lines[3][64];
    snprintf(lines[0], sizeof(lines[0]), i18n::Tr("Device de Jill : %d / %d"), device, kDeviceMax);
    const float threshold = g_pinOn.load(std::memory_order_relaxed) ? g_pinThreshold : kGamePinThreshold;
    snprintf(lines[1], sizeof(lines[1]), i18n::Tr("Reste au sol (projetée) à %.0f ou moins"), threshold);
    snprintf(lines[2], sizeof(lines[2]), i18n::Tr("PV de Jill : %d / %d%s"), health, healthMax,
             g_keepJillHealth.load(std::memory_order_relaxed) ? " (figés)" : "");
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float size = screen.y * 0.028f;
    ImVec2 at(screen.x * 0.025f, screen.y * 0.03f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (int i = 0; i < 3; ++i) {
        dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), lines[i]);
        dl->AddText(ImGui::GetFont(), size, at, IM_COL32(255, 235, 200, 255), lines[i]);
        at.y += size * 1.15f;
        if (i == 0) {  // the device as a bar, the threshold as a mark
            const float width = screen.x * 0.18f, height = size * 0.35f;
            const float filled = width * (device < 0 ? 0 : device > kDeviceMax ? kDeviceMax : device) / kDeviceMax;
            const float mark = width * threshold / kDeviceMax;
            const bool pinned = static_cast<float>(device) <= threshold;
            dl->AddRectFilled(at, ImVec2(at.x + width, at.y + height), IM_COL32(0, 0, 0, 160));
            dl->AddRectFilled(at, ImVec2(at.x + filled, at.y + height),
                              pinned ? IM_COL32(220, 70, 50, 230) : IM_COL32(80, 170, 230, 230));
            dl->AddLine(ImVec2(at.x + mark, at.y - 2), ImVec2(at.x + mark, at.y + height + 2), IM_COL32(255, 255, 255, 230), 2.0f);
            at.y += height + size * 0.35f;
        }
    }
}
}  // namespace

// player.cpp / wesker_moves.cpp: Jill (either part of her fight) loses no health with "Figer la vie de Jill".
bool JillKeepsHealth(uintptr_t enemy) {
    uintptr_t vtable = 0;
    return g_keepJillHealth.load(std::memory_order_relaxed) && mem::Read(enemy, vtable) &&
           (vtable == reinterpret_cast<uintptr_t>(mem::Rva(kJill2Vtable)) || vtable == reinterpret_cast<uintptr_t>(mem::Rva(kJillVtable)));
}

// wesker_moves.cpp: the trainer's moves on Jill (second part) hit her device instead of her health (0 = her health).
int JillDeviceMeleeDamage(uintptr_t enemy) {
    const int damage = g_meleeDevice.load(std::memory_order_relaxed);
    uintptr_t vtable = 0;
    return damage > 0 && mem::Read(enemy, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(kJill2Vtable)) ? damage : 0;
}

// The device lowered by `damage`, never to 0 (the rip-off prompt takes it off there).
void DamageJillDevice(uintptr_t jill, int damage, const char* by) {
    int32_t device = 0;
    if (!ReadDevice(jill, device)) return;
    const int32_t left = device - damage > 1 ? device - damage : 1;
    mem::Write<int32_t>(jill + kDevice, left);
    LOG("jill device: %s hits it: %d -> %d", by, device, left);
}

void RegisterJillDevice() {
    Add({ .id = "enemy.jill_device_gauge", .name = "Jauge du device de Jill",
          .help = "Pendant la deuxième partie du combat contre Jill, affiche en haut à gauche la vie du device sur sa poitrine "
                  "(10000 au départ, -1500 par balle qui le touche), le seuil sous lequel elle reste au sol après une "
                  "projection (barre rouge : elle restera au sol, de quoi tenter d'arracher le device) et ses PV.",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_gaugeShown = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnHud(DrawDevice);
    Add({ .id = "enemy.jill_keeps_health", .name = "Figer la vie de Jill",
          .help = "Jill (les deux parties du combat) ne perd plus de vie : une balle qui rate le device ne la tue pas, ni les "
                  "coups du trainer. Elle réagit toujours aux coups, et son device s'abîme normalement.",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureActorDamageHook()) return false;
              g_keepJillHealth = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Feature& set = Add({ .id = "enemy.jill_device_set", .name = "Régler la vie du device de Jill",
                         .help = "Met la vie du device de Jill (deuxième partie du combat) à la valeur du curseur (10000 = "
                                 "intact). À 7000 ou moins (seuil du jeu), elle reste au sol après une projection.",
                         .category = "Ennemis", .kind = Kind::Action,
                         .available = [] { return FindJill2() != 0; },
                         .value = 5000, .valueMin = 1, .valueMax = 10000, .valueFormat = "%.0f" });
    Feature* const ps = &set;
    set.run = [ps] {
        const uintptr_t jill = FindJill2();
        if (!jill) return false;
        const int32_t value = ps->value > 1.0f ? static_cast<int32_t>(ps->value) : 1;
        mem::Write<int32_t>(jill + kDevice, value);
        if (jill == g_frozenJill) g_frozenDevice = value;
        LOG("jill device: set to %d", value);
        return true;
    };
    Add({ .id = "enemy.jill_device_freeze", .name = "Figer la vie du device de Jill",
          .help = "La vie du device de Jill (deuxième partie du combat) reste celle qu'il avait quand l'option a été cochée "
                  "(ou à l'arrivée de Jill) : ni les balles ni l'arrachage ne la baissent. Décoche-la pour pouvoir "
                  "l'arracher.",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_frozenJill = 0;
              return true;
          },
          .tick = [] {
              const uintptr_t jill = FindJill2();
              int32_t device = 0;
              if (!ReadDevice(jill, device)) {
                  g_frozenJill = 0;
                  return;
              }
              if (jill != g_frozenJill) {
                  g_frozenJill = jill;
                  g_frozenDevice = device;
              } else if (device != g_frozenDevice) {
                  mem::Write<int32_t>(jill + kDevice, g_frozenDevice);
              }
          },
          .available = [] { return game::IsSupported(); } });
    Feature& pin = Add({ .id = "enemy.jill_pin_threshold", .name = "Jill reste au sol sous ce seuil de device",
                         .help = "Après une projection, Jill (deuxième partie du combat) reste au sol si la vie de son "
                                 "device est à ce seuil ou en dessous, au lieu de 7000 dans le jeu (deux balles qui le "
                                 "touchent). 10000 = toujours, même device intact : chaque projection te laisse tenter de "
                                 "l'arracher.",
                         .category = "Ennemis", .kind = Kind::Toggle,
                         .available = [] { return game::IsSupported(); },
                         .value = 10000, .valueMin = 0, .valueMax = 10000, .valueFormat = "%.0f" });
    Feature* const pp = &pin;
    pin.set = [pp](bool on) {
        g_pinThreshold = pp->value;
        if (on ? !g_pinPatch.Apply() : !g_pinPatch.Restore()) return false;
        g_pinOn = on;
        return true;
    };
    pin.tick = [pp] { g_pinThreshold = pp->value; };
    Feature& melee = Add({ .id = "enemy.jill_device_melee", .name = "Corps à corps libre : le device de Jill au lieu de sa vie",
                           .help = "Les coups de la mêlée libre du trainer (et le coup de poing de la parade RE6) ne "
                                   "retirent plus de vie à Jill (deuxième partie du combat) : ils abîment son device du "
                                   "montant du curseur (une balle = 1500). Pour qu'elle reste au sol, projette-la (sa "
                                   "projection, la parade au couteau, la garde) une fois le device sous le seuil.",
                           .category = "Ennemis", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = 1500, .valueMin = 100, .valueMax = 10000, .valueFormat = "%.0f par coup" });
    Feature* const pm = &melee;
    melee.set = [pm](bool on) {
        g_meleeDevice = on ? static_cast<int>(pm->value) : 0;
        return true;
    };
    melee.tick = [pm] { g_meleeDevice = static_cast<int>(pm->value); };
}
}  // namespace features
