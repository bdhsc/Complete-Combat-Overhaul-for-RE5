// Ennemis: operate on every live enemy of the enemy array ([[0xDB27DC]+0x650]).
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace features {
bool FinalFightPartnerFree();  // final_fight.cpp: "Ne pas attaquer le joueur 2" stands aside in the final fight

namespace {
using actors::Enemies;
namespace off = actors::off;

void ForEachEnemy(void (*fn)(uintptr_t, float), float arg) {
    for (uintptr_t e : Enemies()) fn(e, arg);
}

// Per-frame writers (size, speed) leave dead enemies alone and only write a value that changed (2026-10-20: a crash
// in the game's heap free, VA 0x6DB6F7, a free-list link holding 1.2f = the enemy speed slider, right after a grenade
// killed an enemy: the speed went on being written into the enemy the game was freeing).
bool Alive(uintptr_t e) {
    int16_t health = 0;
    return mem::Read(e + off::Health, health) && health > 0;
}

void WriteIfChanged(uintptr_t at, float v) {
    float now = 0.0f;
    if (mem::Read(at, now) && now != v) mem::Write(at, v);
}

void SetScale(uintptr_t e, float s) {
    if (!Alive(e)) return;
    for (int k = 0; k < 3; ++k) WriteIfChanged(e + off::Scale + 4 * k, s);
}

void SetSpeed(uintptr_t e, float s) {
    if (Alive(e)) WriteIfChanged(e + off::Speed, s);
}

void RefillHealth(uintptr_t e, float) {
    int16_t max = 0;
    if (mem::Read(e + off::HealthMax, max) && max > 0) mem::Write(e + off::Health, max);
}

void KillEnemy(uintptr_t e, float) { mem::Write<int16_t>(e + off::Health, 0); }

Feature* g_size = nullptr;
Feature* g_speed = nullptr;

// "Tronçonneuse rapide" (user, 2026-10-15: swings as fast as RE4's): the chainsaw Majini (uEm22, vtable RVA 0xB75B20;
// actions 0x11 AtkCut, 0x12 AtkTurnCut, 0x13 AtkCutting, 0x14 AtkCritical (the grab kill, sped up too: user), 0x15
// AtkCutsDown; not 0x16 AtkRanbu, the rampage, fast enough: user) gets the unit time scale +0xFD4 (1.0, a multiplier
// of its frame time, see licker.cpp) = slider during its chainsaw attacks only; its own value is written back when the attack ends or the option is switched off.
struct SpedSaw {
    uintptr_t enemy;
    float original;
};
std::vector<SpedSaw> g_spedSaws;  // render thread
Feature* g_sawSpeed = nullptr;

bool IsSawSwing(uint32_t action) { return action >= 0x11 && action <= 0x15; }

void RestoreSaws() {
    const std::vector<uintptr_t> live = Enemies();
    for (const SpedSaw& saw : g_spedSaws)
        if (std::find(live.begin(), live.end(), saw.enemy) != live.end()) mem::Write(saw.enemy + 0xFD4, saw.original);
    g_spedSaws.clear();
}

void TickSawSpeed() {
    if (!actors::InGame()) {  // loading: the units are being freed, nothing is written
        g_spedSaws.clear();
        return;
    }
    const float factor = g_sawSpeed->value;
    const std::vector<uintptr_t> live = Enemies();
    std::erase_if(g_spedSaws, [&](const SpedSaw& saw) { return std::find(live.begin(), live.end(), saw.enemy) == live.end(); });
    for (uintptr_t e : live) {
        uintptr_t vtable = 0;
        uint32_t action = 0;
        if (!mem::Read(e, vtable) || vtable - reinterpret_cast<uintptr_t>(mem::Base()) != 0xB75B20 ||
            !mem::Read(e + 0x10E0, action))
            continue;
        auto sped = std::find_if(g_spedSaws.begin(), g_spedSaws.end(), [e](const SpedSaw& saw) { return saw.enemy == e; });
        if (IsSawSwing(action)) {
            if (sped == g_spedSaws.end()) {
                float scale = 0;
                if (!mem::Read(e + 0xFD4, scale) || scale < 0.5f || scale > 2.0f) continue;  // not the plain 1.0 expected
                g_spedSaws.push_back({ e, scale });
                LOG("chainsaw %08X: attack 0x%X sped up x%.1f", static_cast<unsigned>(e), action, factor);
                mem::Write(e + 0xFD4, scale * factor);
            } else {
                mem::Write(e + 0xFD4, sped->original * factor);  // slider moved during the swing
            }
        } else if (sped != g_spedSaws.end()) {
            mem::Write(e + 0xFD4, sped->original);
            g_spedSaws.erase(sped);
        }
    }
}

// "Ne pas attaquer" (UT "Do Not Attack"): player field +0x1BB4 = 15.0 (0 by default).
constexpr uintptr_t kDoNotAttack = 0x1BB4;
// The Weskers ignored it (user, 2026-10-26: Lost in Nightmares' Wesker went on attacking Jill; read live: her +0x1BB4 at
// 15, his target +0x1BA4 her): the enemies' own AI code reads that field, Wesker's (VA 0x908000-0x925000) never does.
// His target comes from his AI unit uEmIntel80 (vtable RVA 0xB98038, at the enemy +0x1B68, owner at its +0x280): its
// SetTarget (vtable +0x44, RVA 0x563E60, thiscall(target) ret 4: +0x2C = target), copied to the enemy's +0x1BA4 (RVA
// 0x5D3EB0). Hooked: a target the option spares becomes the other player.
std::atomic<bool> g_spared[2]{};
bool Spared(int i) { return g_spared[i].load(std::memory_order_relaxed) && !(i == 1 && FinalFightPartnerFree()); }
using SetTargetFn = void(__fastcall*)(void* intel, void* edx, uintptr_t target);
SetTargetFn g_setTarget = nullptr;

void __fastcall HookSetTarget(void* intel, void* edx, uintptr_t target) {
    for (int i = 0; i < 2; ++i) {
        const uintptr_t other = actors::Player(1 - i);
        if (target && target == actors::Player(i) && Spared(i) &&
            !Spared(1 - i) && other) {
            static DWORD logged = 0;
            if (GetTickCount() - logged > 2000)
                LOG("do not attack: an AI's target player %d -> player %d", i + 1, 2 - i);
            logged = GetTickCount();
            target = other;
            break;
        }
    }
    g_setTarget(intel, edx, target);
}

// His own target was still Jill (user, 2026-10-26: his lunge on her again; logged: his AI's +0x2C player 1 after the
// redirect, his +0x1BA4 player 2 in Turn, AtkSH_LR...): set when he came, not copied from his AI since. A Wesker whose
// target is a spared player is given the other one (his AI's target too), except in his holds and kills
// (Pinion 0x6A-0x6C, AtkSyutoDie 0x4A, Kata 0x52-0x54, the DLC's FirstAtk 0x80 / 0x87, Syuto_OK 0x83, HumithukeOk
// 0x8A) and events.
bool HoldsHisTarget(uint32_t action) {
    return action >= 0x80000000u || action == 0x4A || (action >= 0x52 && action <= 0x54) || (action >= 0x6A && action <= 0x6C) ||
           action == 0x80 || action == 0x83 || action == 0x87 || action == 0x8A;
}

void TickSparedTarget() {
    if (!actors::InGame() || (!Spared(0) && !Spared(1)))
        return;
    for (uintptr_t e : Enemies()) {
        uintptr_t vtable = 0, target = 0, intel = 0, intelTarget = 0;
        uint32_t action = 0;
        if (!mem::Read(e, vtable)) continue;
        const uintptr_t rva = vtable - reinterpret_cast<uintptr_t>(mem::Base());
        if ((rva != 0xB8BE60 && rva != 0xB8C300 && rva != 0xB8C0B0) || !mem::Read(e + 0x1BA4, target)) continue;
        const int player = actors::PlayerIndex(target);
        if (player < 0 || !Spared(player) || !mem::Read(e + 0x10E0, action) ||
            HoldsHisTarget(action) || !mem::Read(e + 0x1B68, intel) || !intel || !mem::Read(intel + 0x2C, intelTarget))
            continue;
        // The other player (his AI's too when it still names the spared one).
        const int other = 1 - player;
        const uintptr_t to = actors::Player(other);
        if (!to || Spared(other)) continue;
        if (intelTarget != to) mem::Write(intel + 0x2C, to);
        mem::Write(e + 0x1BA4, to);
        LOG("do not attack: Wesker %08X's target player %d -> player %d (in his action %X%s)", static_cast<unsigned>(e),
            player + 1, other + 1, action, intelTarget != to ? ", his AI's too" : "");
    }
}

bool EnsureSetTargetHook() {
    static bool hooked = false;
    if (hooked) return true;
    // mov eax, [esp+4]; mov [ecx+0x2C], eax; ret 4
    const uint8_t expected[] = { 0x8B, 0x44, 0x24, 0x04, 0x89, 0x41, 0x2C, 0xC2, 0x04, 0x00 };
    uint8_t* target = mem::Rva(0x563E60);
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("enemy SetTarget hook: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookSetTarget), reinterpret_cast<void**>(&g_setTarget));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("enemy SetTarget hook installed at %p", target);
    return true;
}

Feature& AddDoNotAttack(const char* id, const char* name, int player) {
    return Add({
        .id = id, .name = name,
        .help = "Les ennemis ne visent plus ce joueur : ils restent autour sans l'attaquer.",
        .category = "Ennemis", .kind = Kind::Toggle,
        .set = [player](bool on) {
            if (on && !EnsureSetTargetHook()) LOG("do not attack: no SetTarget hook, the Weskers may still attack");
            g_spared[player] = on;
            uintptr_t p = actors::Player(player);
            if (!on) {
                if (p) mem::Write(p + kDoNotAttack, 0.0f);
                return true;
            }
            return p && mem::Write(p + kDoNotAttack, 15.0f);
        },
        .tick = [player] {
            if (uintptr_t p = actors::Player(player)) mem::Write(p + kDoNotAttack, Spared(player) ? 15.0f : 0.0f);
        },
        .available = actors::InGame,
    });
}
}

void RegisterEnemyFeatures() {
    OnFrame(TickSparedTarget);
    Add({ .id = "enemies.freeze_health", .name = "Santé des ennemis figée",
          .help = "La vie des ennemis est remise au maximum en continu (tirs à la tête et explosions peuvent encore tuer).",
          .category = "Ennemis", .kind = Kind::Toggle,
          .tick = [] { ForEachEnemy(RefillHealth, 0); }, .available = actors::InGame });
    Add({ .id = "enemies.kill_all", .name = "Tuer tous les ennemis",
          .help = "Met la vie de tous les ennemis présents à 0 (certains boss ont besoin d'un coup de plus).",
          .category = "Ennemis", .kind = Kind::Action,
          .run = [] { ForEachEnemy(KillEnemy, 0); return true; }, .available = actors::InGame });
    Add({ .id = "enemies.move_to_player", .name = "Amener les ennemis à moi",
          .help = "Téléporte tous les ennemis à la position du joueur 1.", .category = "Ennemis", .kind = Kind::Action,
          .run = [] {
              uintptr_t p = actors::Player(0);
              float pos[3]{};
              if (!p || !mem::SafeRead(reinterpret_cast<void*>(p + off::Position), pos, sizeof(pos))) return false;
              for (uintptr_t e : Enemies()) mem::SafeWrite(reinterpret_cast<void*>(e + off::Position), pos, sizeof(pos));
              return true;
          },
          .available = actors::InGame });
    g_size = &Add({ .id = "enemies.size", .name = "Taille des ennemis",
                    .help = "Multiplie la taille de tous les ennemis (1 = normal). Trop petits, certains n'arrivent plus à franchir les obstacles.",
                    .category = "Ennemis", .kind = Kind::Toggle,
                    .set = [](bool on) { if (!on) ForEachEnemy(SetScale, 1.0f); return true; },
                    .tick = [] { ForEachEnemy(SetScale, g_size->value); },
                    .available = actors::InGame, .value = 1.0f, .valueMin = 0.1f, .valueMax = 5.0f });
    g_speed = &Add({ .id = "enemies.speed", .name = "Vitesse des ennemis",
                     .help = "Vitesse de tous les ennemis (1 = normal, 0 = figés ; chiens, araignées et lickers ignorent 0).",
                     .category = "Ennemis", .kind = Kind::Toggle,
                     .set = [](bool on) { if (!on) ForEachEnemy(SetSpeed, 1.0f); return true; },
                     .tick = [] { ForEachEnemy(SetSpeed, g_speed->value); },
                     .available = actors::InGame, .value = 1.0f, .valueMin = 0.0f, .valueMax = 5.0f });
    g_sawSpeed = &Add({ .id = "enemies.chainsaw_speed", .name = "Tronçonneuse rapide",
                        .help = "Le Majini à la tronçonneuse donne ses coups de tronçonneuse plus vite (comme dans RE4) : le "
                                "curseur multiplie sa vitesse pendant ses attaques (décapitation comprise, pas sa série "
                                "enragée) ; le reste du temps, il garde sa vitesse normale.",
                        .category = "Ennemis", .kind = Kind::Toggle,
                        .set = [](bool on) { if (!on) RestoreSaws(); return true; },
                        .tick = TickSawSpeed,
                        .available = actors::InGame, .value = 1.5f, .valueMin = 1.0f, .valueMax = 3.0f,
                        .valueFormat = "x%.1f" });
    AddDoNotAttack("enemies.ignore_p1", "Ne pas attaquer le joueur 1", 0);
    AddDoNotAttack("enemies.ignore_p2", "Ne pas attaquer le joueur 2", 1);
}
}
