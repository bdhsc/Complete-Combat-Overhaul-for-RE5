// Mercenaires. Data located with UTSpy in a Mercenaries session (MODLOG.md):
//   merc object  = [[re5dx9+0xE340A4]+0x1042C]: +0x58C kill counter, +0x698 combo flag, +0x6A4 float combo
//                  timer, +0x6B8 score, +0x79E "No Mercy" byte
//   round timer  = [[re5dx9+0xE34578]+0x382C]+0x4DC (float)
//   unlocked characters = [re5dx9+0xDB2158]+0x1AC / +0x1B0 (u16 bitmask, 0x3FF = all)
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <intrin.h>
#include <MinHook.h>

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace features {
bool IsMajiniEnemy(uintptr_t enemy);  // parry.cpp
bool BigEnemyClass(uintptr_t enemy);  // knife.cpp: Executioners, chainsaw, Gatling, fat man, giant

namespace {

uintptr_t Deref(uintptr_t rva, uintptr_t offset) {
    uintptr_t obj = 0, out = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(rva)), obj) || !obj) return 0;
    if (!mem::Read(obj + offset, out)) return 0;
    return out;
}

uintptr_t Merc() { return Deref(0xE340A4, 0x1042C); }

uintptr_t TimerAddress() {
    uintptr_t t = Deref(0xE34578, 0x382C);
    return t ? t + 0x4DC : 0;
}

// The merc object also exists in the story (user, 2026-10-22: the timer freeze ran in the Jill fight); its mode +0x58
// tells (1, 2, 5 = Mercenaries, as RVA 0x315B20; same test as dying.cpp).
bool InMercenaries() {
    uint32_t mode = 0;
    const uintptr_t m = actors::InGame() ? Merc() : 0;
    return m && mem::Read(m + 0x58, mode) && (mode == 1 || mode == 2 || mode == 5);
}

// Freeze a value at whatever it was when the option was switched on.
template <class T>
struct Freeze {
    uintptr_t (*address)();
    T saved{};
    bool Capture() {
        uintptr_t a = address();
        return a && mem::Read(a, saved);
    }
    void Hold() {
        if (uintptr_t a = address()) mem::Write(a, saved);
    }
};

// The time left when the option was switched on, or the first time left above 0: re-applied at the start of a
// Mercenaries session (option saved on), it captured the timer before the round set it and held 0 - the round ended at
// once (user, 2026-10-21). The capture belongs to one timer (aGame object): another one (a new stage or round) takes
// its own; a value captured in the story was held in the next Mercenaries round (2026-10-22).
struct TimerFreeze {
    float saved = 0;
    uintptr_t at = 0;
    void Capture() {
        saved = 0;
        at = TimerAddress();
        if (at) mem::Read(at, saved);
    }
    void Hold() {
        const uintptr_t a = TimerAddress();
        if (a != at) {
            at = a;
            saved = 0;
        }
        if (!a) {
            saved = 0;
        } else if (saved > 0) {
            mem::Write(a, saved);
        } else {
            mem::Read(a, saved);
        }
    }
};
TimerFreeze g_timer;
Freeze<int32_t> g_kills{ [] { uintptr_t m = Merc(); return m ? m + 0x58C : uintptr_t(0); } };

// Research (user, 2026-10-23: 150 enemies spawned, 107 kills counted, the round stuck - kills by the trainer's own
// moves and quick shots are not counted): the kill count is the merc object +0x58C, added to at VA 0x7243B9 in RVA
// 0x3243A0 (thiscall(int player, const void* record, int flag) ret 0xC; flag 0 = a kill), called by RVA 0x325C90
// (record built from its arguments) and by the kill queue RVA 0x386010. Logged with the stack, to find the game's own
// path for a Majini's death (diag.merc_kill_log).
using KillFn = void(__fastcall*)(void* self, void* edx, int player, const uint8_t* record, int flag);
KillFn g_kill = nullptr;
std::atomic<bool> g_killLog{ false };

void __fastcall HookKill(void* self, void* edx, int player, const uint8_t* record, int flag) {
    if (g_killLog.load(std::memory_order_relaxed)) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
        const uint32_t* stack = static_cast<const uint32_t*>(_AddressOfReturnAddress());
        char chain[256] = {};
        int n = 0, used = 0;
        for (int k = 0; k < 64 && n < 12; ++k) {
            uint32_t word = 0;
            if (!mem::Read(reinterpret_cast<uintptr_t>(stack + k), word)) break;
            if (word < base + 0x1000 || word >= base + 0x9C0000) continue;
            used += snprintf(chain + used, sizeof(chain) - used, " %06X", static_cast<unsigned>(word - base));
            ++n;
        }
        uint8_t rec[8]{};
        mem::SafeRead(record, rec, sizeof(rec));
        LOG("merc kill: player %d flag %d record %02X %02X %02X %02X %02X %02X %02X %02X | stack%s", player, flag, rec[0],
            rec[1], rec[2], rec[3], rec[4], rec[5], rec[6], rec[7], chain);
    }
    g_kill(self, edx, player, record, flag);
}

bool EnsureKillHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x3243A0);
    const uint8_t expected[] = { 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x1C };
    if (!mem::IsGameCode(target, sizeof(expected)) || memcmp(target, expected, sizeof(expected)) != 0) return false;
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookKill), reinterpret_cast<void**>(&g_kill));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("merc kill hook installed at %p", target);
    return true;
}

// The trainer's own kills (health written to 0: the RE6 quick shot's misses, Jill's throw, Wesker's kicks...) were never
// counted: 150 enemies spawned, 107 kills, no enemy left and the round stuck (user, 2026-10-23). A hit's kill is
// reported by the enemy OnHit (RVA 0x5E0840: after ApplyDamage, when dead, VA 0x9E08FB) with the kill report RVA
// 0x321C50 (thiscall on [aGame]'s RVA 0x368C10() object, (enemy, hit) ret 8): it marks the enemy +0x191C, then the
// kill goes to RVA 0x3916A0 -> RVA 0x386010 -> the count RVA 0x3243A0 (+0x58C, combo, score; logged: every pistol kill
// came this way). Without a hit its record names no player (0x0F: the active ones, player 1 alone in solo). So in
// Mercenaries an enemy (a Majini or a big one) dead for 0.5 s and not reported is reported here.
struct DeadEnemy {
    uintptr_t enemy;
    DWORD since;
};
std::vector<DeadEnemy> g_dead;  // render thread

void ReportKill(uintptr_t enemy) {
    using ManagerFn = void*(__fastcall*)(void* game, void* edx);
    using ReportFn = void(__fastcall*)(void* manager, void* edx, uintptr_t enemy, void* hit);
    void* game = nullptr;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE34578)), game) || !game) return;
    __try {
        void* manager = reinterpret_cast<ManagerFn>(mem::Rva(0x368C10))(game, nullptr);
        if (manager) reinterpret_cast<ReportFn>(mem::Rva(0x321C50))(manager, nullptr, enemy, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("merc: kill report of %08X failed", static_cast<unsigned>(enemy));
    }
}

// The trainer's bare-hand kills (user, 2026-10-31: "no +5 s with the trainer's moves, as with a native bare-hand kill"):
// the count (RVA 0x3243A0) gives a kill whose record's byte 4 is 0 or 0x18 five seconds through RVA 0x31CBF0
// (thiscall(seconds) ret 4 on the round timer's object [[aGame]+0x382C]: its +0x4DC and the bonus' message); a kill
// reported without a hit has 0xFF there. An enemy the trainer's moves killed gets them once its kill is counted (its
// +0x191C set, by the game or by TickKillReports), within 10 s.
struct MeleeKill {
    uintptr_t enemy;
    DWORD tick;
};
std::mutex g_meleeKillLock;
std::vector<MeleeKill> g_meleeKills;

void GiveMeleeBonus(uintptr_t enemy) {
    const uintptr_t timer = Deref(0xE34578, 0x382C);
    float before = 0.0f, after = 0.0f;
    if (!timer || !mem::Read(timer + 0x4DC, before)) return;
    using BonusFn = void(__fastcall*)(void* timer, void* edx, int seconds);
    __try {
        reinterpret_cast<BonusFn>(mem::Rva(0x31CBF0))(reinterpret_cast<void*>(timer), nullptr, 5);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    mem::Read(timer + 0x4DC, after);
    LOG("merc: bare-hand kill of %08X by the trainer, time %.1f -> %.1f", static_cast<unsigned>(enemy), before, after);
}

void TickMeleeBonuses() {
    std::vector<uintptr_t> due;
    {
        std::lock_guard<std::mutex> lock(g_meleeKillLock);
        const DWORD now = GetTickCount();
        for (auto it = g_meleeKills.begin(); it != g_meleeKills.end();) {
            uint8_t reported = 0;
            if (now - it->tick > 10000) {
                it = g_meleeKills.erase(it);
            } else if (mem::Read(it->enemy + 0x191C, reported) && reported) {
                due.push_back(it->enemy);
                it = g_meleeKills.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (uintptr_t e : due) GiveMeleeBonus(e);
}

void TickKillReports() {
    if (!InMercenaries()) {
        g_dead.clear();
        std::lock_guard<std::mutex> lock(g_meleeKillLock);
        g_meleeKills.clear();
        return;
    }
    const DWORD now = GetTickCount();
    const auto enemies = actors::Enemies();
    std::vector<DeadEnemy> still;
    for (uintptr_t e : enemies) {
        int16_t health = 0;
        uint8_t reported = 0;
        if (!mem::Read(e + 0x1364, health) || health > 0 || !mem::Read(e + 0x191C, reported) || reported) continue;
        if (!IsMajiniEnemy(e) && !BigEnemyClass(e)) continue;  // not a parasite unit nor an object of the list
        DWORD since = now;
        for (const DeadEnemy& d : g_dead)
            if (d.enemy == e) since = d.since;
        if (static_cast<int32_t>(now - since) < 500) {
            still.push_back({ e, since });
            continue;
        }
        int32_t before = 0, after = 0;
        const uintptr_t m = Merc();
        if (m) mem::Read(m + 0x58C, before);
        ReportKill(e);
        if (m) mem::Read(m + 0x58C, after);
        LOG("merc: kill of %08X reported by the trainer (kills %d -> %d)", static_cast<unsigned>(e), before, after);
    }
    g_dead.swap(still);
    TickMeleeBonuses();
}

// "Compteur d'ennemis tués" (user, 2026-10-22): the round's kill counter (merc object +0x58C) in the top left corner.
std::atomic<bool> g_killCounter{ false };

void DrawKillCounter() {
    if (!g_killCounter.load(std::memory_order_relaxed) || !InMercenaries()) return;
    const uintptr_t m = Merc();
    int32_t kills = 0;
    if (!m || !mem::Read(m + 0x58C, kills)) return;
    char text[48];
    snprintf(text, sizeof(text), i18n::Tr("Ennemis tués : %d"), kills);
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float size = screen.y * 0.032f;
    const ImVec2 at(screen.x * 0.025f, screen.y * 0.03f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(ImGui::GetFont(), size, at, IM_COL32(255, 235, 200, 255), text);
}

std::deque<mem::Patch> g_patches;

std::vector<mem::Patch*> MakeSites(std::initializer_list<std::tuple<uintptr_t, std::vector<uint8_t>, std::vector<uint8_t>>> sites,
                                   const char* id) {
    std::vector<mem::Patch*> list;
    for (auto& [rva, original, patched] : sites) {
        g_patches.emplace_back(id, rva, original, patched);
        list.push_back(&g_patches.back());
    }
    return list;
}

std::vector<mem::Patch*> g_enhanced;
Feature* g_score = nullptr;
}

// wesker_moves.cpp / parry.cpp: an enemy a trainer's bare-hand move killed (its Mercenaries time bonus once counted).
void NoteTrainerMeleeKill(uintptr_t enemy) {
    if (!enemy) return;
    std::lock_guard<std::mutex> lock(g_meleeKillLock);
    for (const MeleeKill& k : g_meleeKills)
        if (k.enemy == enemy) return;
    g_meleeKills.push_back({ enemy, GetTickCount() });
}

void RegisterMercenariesFeatures() {
    Add({ .id = "merc.freeze_timer", .name = "Figer le chrono",
          .help = "Le temps restant ne descend plus (valeur au moment de l'activation).", .category = "Mercenaires",
          .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on) g_timer.Capture();
              return true;
          },
          .tick = [] { g_timer.Hold(); },
          .available = InMercenaries });
    Add({ .id = "merc.infinite_combo", .name = "Combo infini",
          .help = "Le compteur de combo ne retombe jamais.", .category = "Mercenaires", .kind = Kind::Toggle,
          .tick = [] { if (uintptr_t m = Merc()) mem::Write(m + 0x6A4, 15.0f); }, .available = InMercenaries });
    // Combo bonus: +0x6A8 = 1 while active, +0x6AC = float seconds left (found by snapshot diff during a
    // bonus; UT writes +0x698/+0x6A4 instead, which does nothing on 1.0.0.129).
    Add({ .id = "merc.freeze_combo_bonus", .name = "Bonus de combo permanent",
          .help = "Quand un bonus de combo démarre, il ne se termine plus (minuteur bloqué à 30 s).", .category = "Mercenaires",
          .kind = Kind::Toggle,
          .tick = [] {
              uintptr_t m = Merc();
              uint32_t active = 0;
              if (m && mem::Read(m + 0x6A8, active) && active == 1) mem::Write(m + 0x6AC, 30.0f);
          },
          .available = InMercenaries });
    Add({ .id = "merc.kill_counter", .name = "Compteur d'ennemis tués",
          .help = "Affiche le nombre d'ennemis tués pendant la partie de Mercenaires, en haut à gauche de l'écran.",
          .category = "Mercenaires", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_killCounter = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnHud(DrawKillCounter);
    OnFrame(TickKillReports);
    Add({ .id = "diag.merc_kill_log", .name = "Journal des kills Mercenaires (recherche)",
          .help = "Écrit dans trainer.log chaque kill compté par Mercenaires, avec qui l'a compté.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureKillHook()) return false;
              g_killLog = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "merc.freeze_kills", .name = "Figer le compteur de kills",
          .help = "Le nombre d'ennemis tués ne change plus.", .category = "Mercenaires", .kind = Kind::Toggle,
          .set = [](bool on) { return !on || g_kills.Capture(); }, .tick = [] { g_kills.Hold(); }, .available = InMercenaries });
    g_score = &Add({ .id = "merc.set_score", .name = "Appliquer le score",
                     .help = "Remplace le score actuel par la valeur du curseur.", .category = "Mercenaires", .kind = Kind::Action,
                     .run = [] {
                         uintptr_t m = Merc();
                         return m && mem::Write<int32_t>(m + 0x6B8, static_cast<int32_t>(g_score->value));
                     },
                     .available = InMercenaries, .value = 100000, .valueMin = 0, .valueMax = 9999999, .valueFormat = "%.0f" });
    Add({ .id = "merc.no_mercy", .name = "Mode No Mercy (duo)", .help = "Active le mode No Mercy en Mercenaires duo.",
          .category = "Mercenaires", .kind = Kind::Toggle,
          .set = [](bool on) { uintptr_t m = Merc(); return !m || mem::Write<uint8_t>(m + 0x79E, on ? 1 : 0); },
          .tick = [] { if (uintptr_t m = Merc()) mem::Write<uint8_t>(m + 0x79E, 1); }, .available = InMercenaries });
    Add({ .id = "merc.unlock_characters", .name = "Débloquer tous les personnages",
          .help = "Rend tous les personnages des Mercenaires disponibles (à laisser actif si certains restent verrouillés).",
          .category = "Mercenaires", .kind = Kind::Toggle,
          .tick = [] {
              uintptr_t p = 0;
              if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), p) && p) {
                  mem::Write<uint16_t>(p + 0x1AC, 0x3FF);
                  mem::Write<uint16_t>(p + 0x1B0, 0x3FF);
              }
          },
          .available = [] { return game::IsSupported(); } });

    // Enhanced No Mercy: up to 700 enemies (enemy cap 150 -> 700) and the related mode checks.
    g_enhanced = MakeSites({
        { 0x3D50F3, { 0x05 }, { 0x02 } },
        { 0x32B857, { 0x05 }, { 0x7F } },
        { 0x32D7DC, { 0x05 }, { 0x7F } },
        { 0x32DFD1, { 0x05 }, { 0x7F } },
        { 0x32DF54, { 0x0F, 0x85, 0x75, 0x00, 0x00, 0x00 }, { 0xEB, 0x2F, 0x90, 0x90, 0x90, 0x90 } },
        { 0x32B7B0, { 0x02 }, { 0x7F } },
        { 0x32D763, { 0x02 }, { 0x7F } },
        { 0x32E8B7, { 0x05 }, { 0x02 } },
        { 0x5F8F88, { 0x01 }, { 0x02 } },
        { 0x317583, { 0x01 }, { 0x02 } },
        { 0x3178F9, { 0x96, 0x00, 0x00, 0x00 }, { 0xBC, 0x02, 0x00, 0x00 } },
        { 0x3F3029, { 0x75 }, { 0xEB } },
        { 0x5DEBA0, { 0x01 }, { 0x00 } },
    }, "merc.enhanced_no_mercy");
    Add({ .id = "merc.enhanced_no_mercy", .name = "Enhanced No Mercy (700 ennemis)",
          .help = "Hybride Reunion / No Mercy : jusqu'à 700 ennemis. À activer avant de lancer la partie.",
          .category = "Mercenaires", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (!on) {
                  bool ok = true;
                  for (auto it = g_enhanced.rbegin(); it != g_enhanced.rend(); ++it) ok = (*it)->Restore() && ok;
                  return ok;
              }
              for (mem::Patch* p : g_enhanced)
                  if (!p->IsRecognised()) return false;
              for (mem::Patch* p : g_enhanced)
                  if (!p->Apply()) return false;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
}
}
