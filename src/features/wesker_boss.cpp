// Jill and Wesker's fight (chapter 5-3, user 2026-10-24: the Ultimate Trainer's "Wesker Never Gives up" and "Wesker
// Boss No Time Limit", and a display to follow them). Research in MODLOG (2026-10-24, UT's Wesker options):
// - Wesker (uEm80, vtable RVA 0xB8BE60) +0x1FA8 = his give-up gauge (3000 at the fight start): at 0 he leaves. Lowered
//   by RVA 0x508860 (thiscall, the amount as argument) and in RVA 0x50D1A2 (a hit's damage), each store followed by
//   "below 0 -> 0". UT NOPs the two stores and keeps that one: a hit crossing 0 still made him leave. Here the jle that
//   skips the subtraction when the gauge is empty becomes a jmp: nothing lowers it. His health makes him leave too
//   (user, 2026-10-24: gauge kept, he left hurt at 5:12): "Wesker n'abandonne jamais" also refuses his SubHealth.
// - His time limit: UT rewrites the inits of two AI FSM parameters, cAIFSM::ParamConstS32 (RVA 0x7D2FF0: +0x10 = the
//   constant +0x1C) and ParamConstF32 (RVA 0x7D3020: +0x14 = the float constant +0x1C): 400 -> 0x7FFFFFFF and 180.0 ->
//   +inf, for every FSM of the fight's rooms (its room word). Here the same values while Wesker is in the enemy list;
//   which parameter is his limit is not checked yet (diag.wesker_fsm_log lists the inits). No init ran during the
//   fight (5 s watched): they run when an FSM starts. His gauge went up too (3000 at a start, 3140 later).
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

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <set>
#include <vector>

namespace features {
bool PauseMenuIsShown();       // pickup.cpp: the pause menu unit's slot 9 ran within 50 ms
bool EnsurePauseMenuWatch();   // pickup.cpp
bool EnsureActorDamageHook();  // player.cpp: the actor SubHealth hook (RVA 0x44F7C0)
int FinalQteHealth();           // final_fight.cpp: the health launching the volcano Wesker's finish, -1 off
bool EnsureEnemyActionHook();  // action_log.cpp: the enemies' request core hook (RVA 0x759030)

namespace {
constexpr uintptr_t kWeskerVtable = 0xB8BE60;  // uEm80 (RVA)
constexpr uintptr_t kJillVtable = 0xB8EA40;    // uEm83 (RVA)
constexpr uintptr_t kGiveUp = 0x1FA8;          // int32

uintptr_t FindWesker() {
    if (!actors::InGame()) return 0;
    const uintptr_t vtable = reinterpret_cast<uintptr_t>(mem::Rva(kWeskerVtable));
    for (uintptr_t e : actors::Enemies()) {
        uintptr_t v = 0;
        if (mem::Read(e, v) && v == vtable) return e;
    }
    return 0;
}

// ---- "Jauge d'abandon de Wesker": his gauge, health and the fight's time, top left. Once he left, "Parti à" for 5 s
// only (user, 2026-10-31: it stayed over the device gauge of Jill's second part, drawn at the same place).
std::atomic<bool> g_gaugeShown{ false };
constexpr DWORD kGoneShownMs = 5000;
struct Fight {
    uintptr_t wesker = 0;  // the unit followed (kept after he left, until the game is left)
    bool gone = false;
    DWORD goneTick = 0;
    int32_t gauge = 0, gaugeMax = 0;
    int16_t health = 0, healthMax = 0;
    double seconds = 0;  // since he appeared, the pause menu excluded (it also opens when the game loses the focus)
    LARGE_INTEGER last{};
};
Fight g_fight;  // render thread

void TrackFight() {
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double dt = g_fight.last.QuadPart ? static_cast<double>(now.QuadPart - g_fight.last.QuadPart) / freq.QuadPart : 0.0;
    g_fight.last = now;
    if (dt > 0.1) dt = 0.1;
    if (!actors::InGame()) {
        g_fight = Fight{};
        g_fight.last = now;
        return;
    }
    const uintptr_t w = FindWesker();
    if (w && w != g_fight.wesker) {
        g_fight = Fight{};
        g_fight.wesker = w;
        g_fight.last = now;
        LOG("wesker boss: Wesker %08X followed", static_cast<unsigned>(w));
    }
    if (!g_fight.wesker) return;
    if (!w) {
        if (!g_fight.gone) {
            LOG("wesker boss: Wesker gone after %.1f s (gauge %d, health %d / %d)", g_fight.seconds, g_fight.gauge,
                g_fight.health, g_fight.healthMax);
            g_fight.goneTick = GetTickCount();
        }
        g_fight.gone = true;
        return;
    }
    if (mem::Read(w + kGiveUp, g_fight.gauge) && g_fight.gauge > g_fight.gaugeMax) g_fight.gaugeMax = g_fight.gauge;
    mem::Read(w + actors::off::Health, g_fight.health);
    mem::Read(w + actors::off::HealthMax, g_fight.healthMax);
    if (!PauseMenuIsShown()) g_fight.seconds += dt;
}

void DrawFight() {
    if (!g_gaugeShown.load(std::memory_order_relaxed)) return;
    TrackFight();
    if (!g_fight.wesker || (g_fight.gone && GetTickCount() - g_fight.goneTick > kGoneShownMs)) return;
    const int total = static_cast<int>(g_fight.seconds);
    char lines[3][64];
    snprintf(lines[0], sizeof(lines[0]), i18n::Tr("Wesker - abandon : %d / %d"), g_fight.gauge, g_fight.gaugeMax);
    snprintf(lines[1], sizeof(lines[1]), i18n::Tr("PV : %d / %d"), g_fight.health, g_fight.healthMax);
    snprintf(lines[2], sizeof(lines[2]), g_fight.gone ? i18n::Tr("Parti à %d:%02d") : i18n::Tr("Combat : %d:%02d"), total / 60, total % 60);
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float size = screen.y * 0.028f;
    ImVec2 at(screen.x * 0.025f, screen.y * 0.03f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (int i = 0; i < 3; ++i) {
        dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), lines[i]);
        dl->AddText(ImGui::GetFont(), size, at, IM_COL32(255, 235, 200, 255), lines[i]);
        at.y += size * 1.15f;
        if (i == 0) {  // the gauge as a bar
            const float width = screen.x * 0.18f, height = size * 0.35f;
            const float filled = g_fight.gaugeMax > 0 ? width * g_fight.gauge / g_fight.gaugeMax : 0.0f;
            dl->AddRectFilled(at, ImVec2(at.x + width, at.y + height), IM_COL32(0, 0, 0, 160));
            dl->AddRectFilled(at, ImVec2(at.x + filled, at.y + height), IM_COL32(230, 160, 40, 230));
            at.y += height + size * 0.35f;
        }
    }
}

// ---- "PV de Wesker" (user, 2026-10-26: "his health shown as with Jill and the other Wesker"): the final fight's Wesker
// (uEm81, vtable RVA 0xB8C300, 30000 health) and Lost in Nightmares' (uEmA2, RVA 0xB8C0B0, 8500), top left as above.
constexpr uintptr_t kFinalWeskerVtable = 0xB8C300, kLinWeskerVtable = 0xB8C0B0;
constexpr uintptr_t kVolcanoWeskerVtable = 0xB8D878;  // uEm82, the volcano's (2026-11-02)
std::atomic<bool> g_healthShown{ false };

// ---- Lost in Nightmares' Wesker (uEmA2, user 2026-10-26: "he fled with health left: a give-up bar blocked by a DLC
// 'Wesker s'entête', and an option to set his health, he has little"). No gauge of his makes him leave (uEm80's +0x1FA8
// patches were on): the full combo's end (AtkSyutoRevEnd 0x50, its exit RVA 0x510EC0) in the fight's room 0x2C2 asks
// Standby_ready 0x95 (logged: then the end production 0x7E, he flees) once the scenario flag 3 is set (RVA 0x37E460(3),
// stdcall: bit 3 of the profile's flags +0x23214); at 1 health his AI asks his defeat LoseBgn 0x94 (forced_death.cpp).
// "Wesker s'entête (Lost in Nightmares)" refuses 0x95 (not the defeat at 1 health); "PV de Wesker (Lost in
// Nightmares)" sets his health and its maximum to the slider's value (again when it changes or a new Wesker comes).
std::atomic<bool> g_linStubborn{ false };
uintptr_t g_linHealthFor = 0;  // render thread
int16_t g_linHealthSet = 0;
// Refusing his way out froze him (user, 2026-10-26: after a full combo, his health high; logged: his FSM asked
// Standby_ready 0x95 again and again once the flag 3 was set - it was not at the fight's start). With "s'entête" the
// flag test RVA 0x37E460 (stdcall(index), the profile's flags) answers 0 for index 3 in his room, so neither his FSM
// nor the combo's end decides to leave (the flag itself, saved data, untouched); a 0x95 still refused gives him his
// Wait 0x00 back (TickLinStubborn).
using FlagTestFn = int(__stdcall*)(int index);
FlagTestFn g_flagTest = nullptr;
std::atomic<uintptr_t> g_linUnfreeze{ 0 };
// At 1 health the flag is his again (user, 2026-10-26: his defeat and Standby_ready went through but the end production,
// which tests the flag too, never started: he came back to fight, again at each hit).
std::atomic<bool> g_linBeaten{ false };
bool InLinFightRoom();                    // below
bool IsLinWesker(uintptr_t unit);         // below
bool IsLinWesker(uintptr_t unit);         // below
bool RequestUnit(uintptr_t unit, int action);  // below

int __stdcall HookFlagTest(int index) {
    if (index == 3 && g_linStubborn.load(std::memory_order_relaxed) && !g_linBeaten.load(std::memory_order_relaxed) &&
        InLinFightRoom())
        return 0;
    return g_flagTest(index);
}

bool EnsureFlagTestHook() {
    static bool hooked = false;
    if (hooked) return true;
    // mov eax, [0x11B2158]; mov ecx, [eax+0x20]
    const uint8_t expected[] = { 0xA1, 0x58, 0x21, 0x1B, 0x01, 0x8B, 0x48, 0x20 };
    uint8_t* target = mem::Rva(0x37E460);
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("wesker boss: flag test, unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookFlagTest), reinterpret_cast<void**>(&g_flagTest));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("wesker boss: flag test hook installed at %p", target);
    return true;
}

void TickLinStubborn() {
    bool beaten = false;
    for (uintptr_t u : actors::InGame() ? actors::Enemies() : std::vector<uintptr_t>{}) {
        int16_t health = 0;
        if (IsLinWesker(u) && mem::Read(u + actors::off::Health, health) && health <= 1) beaten = true;
    }
    if (beaten != g_linBeaten.exchange(beaten) && beaten) LOG("wesker boss: Lost in Nightmares' Wesker at 1 health, the flag is his again");
    const uintptr_t e = g_linUnfreeze.exchange(0);
    if (!e || !actors::InGame()) return;
    for (uintptr_t u : actors::Enemies())
        if (u == e) LOG("wesker boss: Lost in Nightmares' Wesker %08X's Wait %s (unfrozen)", static_cast<unsigned>(e),
                        RequestUnit(e, 0x00) ? "asked" : "refused");
}

bool IsLinWesker(uintptr_t unit) {
    uintptr_t vtable = 0;
    return unit && mem::Read(unit, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(kLinWeskerVtable));
}

bool LinWeskerThere() {
    if (!actors::InGame()) return false;
    for (uintptr_t e : actors::Enemies())
        if (IsLinWesker(e)) return true;
    return false;
}

// The scenario flag that lets the combo's end send him away (the game's own answer, past the hook).
bool LinFleeArmed() {
    __try {
        const FlagTestFn test = g_flagTest ? g_flagTest : reinterpret_cast<FlagTestFn>(mem::Rva(0x37E460));
        return test(3) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TickLinHealth(float value) {
    uintptr_t w = 0;
    for (uintptr_t e : actors::InGame() ? actors::Enemies() : std::vector<uintptr_t>{})
        if (IsLinWesker(e)) w = e;
    const int16_t health = static_cast<int16_t>(value);
    if (!w || (w == g_linHealthFor && health == g_linHealthSet)) return;
    int16_t was = 0;
    mem::Read(w + actors::off::Health, was);
    if (was <= 0) return;
    mem::Write(w + actors::off::HealthMax, health);
    mem::Write(w + actors::off::Health, health);
    g_linHealthFor = w;
    g_linHealthSet = health;
    LOG("wesker boss: Lost in Nightmares' Wesker %08X health %d -> %d", static_cast<unsigned>(w), was, health);
}

bool LinNoTimeLimitOn();  // below

// Lost in Nightmares' fight time since he came, the pause menu excluded (it also opens when the game loses the focus).
double LinFightSeconds(uintptr_t w) {
    static uintptr_t timedFor = 0;
    static double seconds = 0;
    static LARGE_INTEGER last{};
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double dt = last.QuadPart ? static_cast<double>(now.QuadPart - last.QuadPart) / freq.QuadPart : 0.0;
    last = now;
    if (w != timedFor) {
        timedFor = w;
        seconds = 0;
        dt = 0;
    }
    if (dt < 0.1 && !PauseMenuIsShown()) seconds += dt;
    return seconds;
}

void DrawOtherWesker() {
    if (!g_healthShown.load(std::memory_order_relaxed) || !actors::InGame()) return;
    const uintptr_t final = reinterpret_cast<uintptr_t>(mem::Rva(kFinalWeskerVtable));
    const uintptr_t lin = reinterpret_cast<uintptr_t>(mem::Rva(kLinWeskerVtable));
    const uintptr_t volcano = reinterpret_cast<uintptr_t>(mem::Rva(kVolcanoWeskerVtable));
    uintptr_t w = 0, vtable = 0;
    for (uintptr_t e : actors::Enemies())
        if (mem::Read(e, vtable) && (vtable == final || vtable == lin || vtable == volcano)) {
            w = e;
            break;
        }
    int16_t health = 0, healthMax = 0;
    if (!w || !mem::Read(w + actors::off::Health, health) || !mem::Read(w + actors::off::HealthMax, healthMax)) return;
    char line[64];
    snprintf(line, sizeof(line), i18n::Tr("Wesker - PV : %d / %d"), health, healthMax);
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float size = screen.y * 0.028f;
    ImVec2 at(screen.x * 0.025f, screen.y * 0.03f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), line);
    dl->AddText(ImGui::GetFont(), size, at, IM_COL32(255, 235, 200, 255), line);
    at.y += size * 1.15f;
    const float width = screen.x * 0.18f, height = size * 0.35f;
    const float filled = healthMax > 0 ? width * std::clamp<float>(static_cast<float>(health) / healthMax, 0.0f, 1.0f) : 0.0f;
    dl->AddRectFilled(at, ImVec2(at.x + width, at.y + height), IM_COL32(0, 0, 0, 160));
    dl->AddRectFilled(at, ImVec2(at.x + filled, at.y + height), IM_COL32(200, 40, 40, 230));
    if (vtable == volcano && FinalQteHealth() > 0) {  // final_fight.cpp: the health that launches his finish
        at.y += height + size * 0.35f;
        snprintf(line, sizeof(line), i18n::Tr("Prise finale automatique : %d PV"), FinalQteHealth());
        dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), line);
        dl->AddText(ImGui::GetFont(), size, at, IM_COL32(255, 235, 200, 255), line);
    }
    if (!IsLinWesker(w)) return;
    // His way out: the next full combo once the scenario allows it.
    at.y += height + size * 0.35f;
    const char* flee = g_linStubborn.load(std::memory_order_relaxed) ? i18n::Tr("Fuite : bloquée (s'entête)")
                       : LinFleeArmed()                              ? i18n::Tr("Fuite : au prochain combo complet")
                                                                     : i18n::Tr("Fuite : pas encore possible");
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), flee);
    dl->AddText(ImGui::GetFont(), size, at, IM_COL32(255, 235, 200, 255), flee);
    // His time limit: 180 s of his FSM (made infinite by "Wesker sans limite de temps" when set before he comes).
    at.y += size * 1.15f;
    const int total = static_cast<int>(LinFightSeconds(w));
    snprintf(line, sizeof(line), LinNoTimeLimitOn() ? i18n::Tr("Combat : %d:%02d (sans limite)") : i18n::Tr("Combat : %d:%02d / 3:00"), total / 60,
             total % 60);
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), line);
    dl->AddText(ImGui::GetFont(), size, at, IM_COL32(255, 235, 200, 255), line);
}

// ---- "Wesker s'entête" (his gauge) and "Wesker n'abandonne jamais" (his gauge and his health: user, 2026-10-24,
// with the gauge kept he still left at 5:12 of the fight, hurt). Each option holds its own references on the sites.
std::deque<mem::Patch> g_stubbornPatches, g_neverPatches;
std::atomic<bool> g_keepHealth{ false };

void MakeGaugePatches(std::deque<mem::Patch>& list, const char* id) {
    list.emplace_back(id, 0x508866, std::vector<uint8_t>{ 0x85, 0xC0, 0x7E, 0x16 }, std::vector<uint8_t>{ 0x85, 0xC0, 0xEB, 0x16 });
    list.emplace_back(id, 0x50D1A2, std::vector<uint8_t>{ 0x85, 0xC0, 0x7E, 0x17 }, std::vector<uint8_t>{ 0x85, 0xC0, 0xEB, 0x17 });
}

bool SetGaugePatches(std::deque<mem::Patch>& list, bool on) {
    if (!on) {
        bool ok = true;
        for (auto it = list.rbegin(); it != list.rend(); ++it) ok = it->Restore() && ok;
        return ok;
    }
    for (mem::Patch& p : list)
        if (!p.IsRecognised()) return false;
    for (mem::Patch& p : list)
        if (!p.Apply()) {
            for (mem::Patch& q : list) q.Restore();
            return false;
        }
    return true;
}

// ---- "Wesker sans limite de temps" and the research log of the FSM parameter inits. Lost in Nightmares' fight has the
// same 180 s (user, 2026-10-26: he left with little damage taken and no full combo; logged: Standby_ready 0x95 asked by
// the FSM's action order RVA 0x6753E7, 185 s after he came, 180 s without the two pauses): the inits are changed while
// that Wesker is there with its own option ("Wesker sans limite de temps (Lost in Nightmares)", user: apart from 5-3's).
using ParamInitFn = void(__fastcall*)(void* param, void* edx, uint32_t arg);
ParamInitFn g_initS32 = nullptr, g_initF32 = nullptr;
std::atomic<bool> g_noTimeLimit{ false }, g_noTimeLimitLin{ false }, g_fsmLog{ false };
bool LinNoTimeLimitOn() { return g_noTimeLimitLin.load(std::memory_order_relaxed); }

// Lost in Nightmares' fight room ([profile +0x5940 * idx +0x273D8] = 0x2C2): his FSM may read its constants before he is
// in the enemy list (user, 2026-10-26: the option on, he left after his 3 minutes).
bool InLinFightRoom() {
    uintptr_t profile = 0;
    uint32_t index = 0, room = 0;
    return mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), profile) && profile && mem::Read(profile + 0x20, index) &&
           index < 4 && mem::Read(profile + index * 0x5940 + 0x273D8, room) && room == 0x2C2;
}

// A time limit init to make infinite: the 5-3 Wesker's or Lost in Nightmares' option, its Wesker (or his room) there.
bool TimeLimitOff() {
    return (g_noTimeLimit.load(std::memory_order_relaxed) && FindWesker()) ||
           (g_noTimeLimitLin.load(std::memory_order_relaxed) && (LinWeskerThere() || InLinFightRoom()));
}
std::mutex g_fsmLock;
std::set<uintptr_t> g_fsmSeen;  // parameters already logged
std::set<uintptr_t> g_madeInfinite;  // logged once each (the game reads some every frame)

void LogInfinite(uintptr_t param, const char* text) {
    std::lock_guard lock(g_fsmLock);
    if (g_madeInfinite.size() < 2000 && g_madeInfinite.insert(param).second)
        LOG("wesker boss: FSM time constant %s made infinite (%08X)", text, static_cast<unsigned>(param));
}

// Every parameter once, Wesker there or not (his FSM may start before he is in the enemy list).
void NoteParam(uintptr_t param, bool isFloat, uint32_t raw, uintptr_t caller, bool changed) {
    if (!g_fsmLog.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_fsmLock);
    if (g_fsmSeen.size() >= 2000 || !g_fsmSeen.insert(param).second) return;
    float f = 0.0f;
    std::memcpy(&f, &raw, sizeof(f));
    const unsigned rva = static_cast<unsigned>(caller - reinterpret_cast<uintptr_t>(mem::Base()));
    const char* there = FindWesker() || LinWeskerThere() ? ", Wesker there" : "";
    if (isFloat)
        LOG("wesker boss: FSM F32 %08X = %.3f (caller RVA %06X%s)%s", static_cast<unsigned>(param), f, rva, there,
            changed ? " -> infinite" : "");
    else
        LOG("wesker boss: FSM S32 %08X = %d (caller RVA %06X%s)%s", static_cast<unsigned>(param), static_cast<int32_t>(raw),
            rva, there, changed ? " -> infinite" : "");
}

void __fastcall HookInitS32(void* param, void* edx, uint32_t arg) {
    g_initS32(param, edx, arg);
    const uintptr_t p = reinterpret_cast<uintptr_t>(param);
    int32_t value = 0;
    if (!mem::Read(p + 0x1C, value)) return;
    const bool change = value == 400 && TimeLimitOff();
    if (change) {
        mem::Write(p + 0x10, std::numeric_limits<int32_t>::max());
        LogInfinite(p, "400");
    }
    NoteParam(p, false, static_cast<uint32_t>(value), reinterpret_cast<uintptr_t>(_ReturnAddress()), change);
}

void __fastcall HookInitF32(void* param, void* edx, uint32_t arg) {
    g_initF32(param, edx, arg);
    const uintptr_t p = reinterpret_cast<uintptr_t>(param);
    float value = 0.0f;
    if (!mem::Read(p + 0x1C, value)) return;
    const bool change = value == 180.0f && TimeLimitOff();
    if (change) {
        mem::Write(p + 0x14, std::numeric_limits<float>::infinity());
        LogInfinite(p, "180");
    }
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    NoteParam(p, true, raw, reinterpret_cast<uintptr_t>(_ReturnAddress()), change);
}

bool HookAt(uintptr_t rva, const uint8_t* expected, size_t size, void* detour, void** original, const char* what) {
    uint8_t* target = mem::Rva(rva);
    if (!mem::IsGameCode(target, size) || std::memcmp(target, expected, size) != 0) {
        LOG("wesker boss: %s, unexpected bytes %s", what, mem::Hex(target, size).c_str());
        return false;
    }
    mem::EnsureExecutable(target, size);
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    LOG("wesker boss: %s hook installed at %p", what, target);
    return true;
}

// ---- "Jill reste au sol tant que Wesker est là" (user, 2026-10-25: "when I want to focus on Wesker her getup gets in
// the way"): thrown (Caught 0x27 / 0x2A ending on her back, em83dmg 0x25), her requests are refused while Wesker is
// in the enemy list (her deaths and another throw by player 1 excepted: after her lying motion she stays on its last
// frame); once he is gone (phase 2) or the option is off, her getup DmgGetup 0x21 is asked if she still lies 0.5 s later.
std::atomic<bool> g_jillStaysDown{ false };
std::atomic<uintptr_t> g_jillHeld{ 0 };   // the Jill kept down (game thread sets, render thread releases)
DWORD g_jillReleaseTick = 0;              // render thread

bool IsUnit(uintptr_t unit, uintptr_t vtableRva) {
    uintptr_t vtable = 0;
    return unit && mem::Read(unit, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(vtableRva));
}

// Her layer 0 motion is the lying one of em83dmg (bank found by its list's path) in a Caught action.
bool JillLying(uintptr_t jill) {
    uint32_t action = 0, motion = 0;
    if (!mem::Read(jill + 0x10E0, action) || (action != 0x27 && action != 0x2A) || !mem::Read(jill + 0x384, motion) ||
        (motion & 0xFF) != 0x25)
        return false;
    uintptr_t list = 0;
    char path[64]{};
    const uint32_t bank = (motion >> 8) & 0xFF;
    return bank < 16 && mem::Read(jill + 0xA88 + 4 * bank, list) && list &&
           mem::SafeRead(reinterpret_cast<void*>(list + 4), path, sizeof(path) - 1) && std::strstr(path, "em83dmg");
}

bool RequestUnit(uintptr_t unit, int action) {
    using RequestFn = bool(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(unit, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return false;
    __try {
        return reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(unit), nullptr, action, 1, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TickJillHeld() {
    const uintptr_t jill = g_jillHeld.load(std::memory_order_relaxed);
    if (!jill) return;
    bool alive = false;
    for (uintptr_t e : actors::Enemies()) alive = alive || e == jill;
    if (!alive) {
        g_jillHeld = 0;
        g_jillReleaseTick = 0;
        return;
    }
    if (g_jillStaysDown.load(std::memory_order_relaxed) && FindWesker() && JillLying(jill)) {
        g_jillReleaseTick = 0;
        return;
    }
    if (!JillLying(jill)) {  // up again (or thrown again)
        g_jillHeld = 0;
        g_jillReleaseTick = 0;
        return;
    }
    if (!g_jillReleaseTick) g_jillReleaseTick = GetTickCount() | 1;
    if (static_cast<int32_t>(GetTickCount() - g_jillReleaseTick) < 500) return;
    g_jillHeld = 0;
    g_jillReleaseTick = 0;
    LOG("wesker boss: Jill %08X released, getup %s", static_cast<unsigned>(jill), RequestUnit(jill, 0x21) ? "asked" : "refused");
}

bool EnsureFsmHooks() {
    static bool hooked = false;
    if (hooked) return true;
    // mov edx, [ecx+0x1C]; mov eax, 0xA; mov [ecx+8], eax; mov [ecx+0x10], edx; ret 4
    const uint8_t s32[] = { 0x8B, 0x51, 0x1C, 0xB8, 0x0A, 0x00, 0x00, 0x00, 0x89, 0x41, 0x08, 0x89, 0x51, 0x10, 0xC2, 0x04, 0x00 };
    // fld [ecx+0x1C]; mov eax, 0xC; fstp [ecx+0x14]; mov [ecx+8], eax; ret 4
    const uint8_t f32[] = { 0xD9, 0x41, 0x1C, 0xB8, 0x0C, 0x00, 0x00, 0x00, 0xD9, 0x59, 0x14, 0x89, 0x41, 0x08, 0xC2, 0x04, 0x00 };
    if (!HookAt(0x7D2FF0, s32, sizeof(s32), reinterpret_cast<void*>(&HookInitS32), reinterpret_cast<void**>(&g_initS32),
                "ParamConstS32 init") ||
        !HookAt(0x7D3020, f32, sizeof(f32), reinterpret_cast<void*>(&HookInitF32), reinterpret_cast<void**>(&g_initF32),
                "ParamConstF32 init"))
        return false;
    hooked = true;
    return true;
}
}  // namespace

// The actor SetMotion hook (swaps.cpp): her Caught goes on by itself after lying, on em83dmg 0x1E (802 frames, no request;
// logged 2026-10-25: 0x25 held about 20 s, then 0x1E and she stood): while she is kept down it plays the lying 0x25 again.
uint32_t JillDownMotion(uintptr_t unit, uint32_t layer, uint32_t motion) {
    if (layer != 0 || (motion & 0xFFFF) != 0x1E || !g_jillStaysDown.load(std::memory_order_relaxed) ||
        !IsUnit(unit, kJillVtable) || !JillLying(unit) || !FindWesker())
        return motion;
    if (g_jillHeld.exchange(unit) != unit)
        LOG("wesker boss: Jill %08X kept down (her motion %X stays the lying one)", static_cast<unsigned>(unit), motion);
    return (motion & ~0xFFFFu) | 0x25;
}

// The enemies' request hook (action_log.cpp, game thread): Jill's requests refused while she is kept down, her deaths
// and a throw by player 1 excepted. Player 2 holding her (Caught 0x27 / 0x2A, Pinioned 0x28 / 0x29 with her as the
// partner, from RVA 0x52FFB3) stood her up too (user, 2026-10-25: lying 20 s, then Sheva's hold, motion em83dmg 0x1E).
bool JillKeptDown(uintptr_t enemy, int action, uintptr_t partner) {
    if (!g_jillStaysDown.load(std::memory_order_relaxed) || !IsUnit(enemy, kJillVtable)) return false;
    if (action >= 0x24 && action <= 0x26) return false;  // her deaths
    if ((action == 0x27 || action == 0x2A) && partner && partner == actors::Player(0)) return false;  // thrown again
    if (!JillLying(enemy) || !FindWesker()) return false;
    if (g_jillHeld.exchange(enemy) != enemy)
        LOG("wesker boss: Jill %08X kept down (her %X refused while Wesker is there)", static_cast<unsigned>(enemy), action);
    return true;
}

// "Dégâts reçus par Wesker (Lost in Nightmares)" (user, 2026-10-26: "his health falls too fast, the combo's last blow
// takes 5000"; his health is an int16, 30000 already near its limit): every damage he takes times the slider's factor
// (at least 1): the game's (player.cpp's SubHealth hook) and the trainer's own blows (wesker_moves.cpp, parry.cpp).
namespace {
std::atomic<bool> g_linDamageOn{ false };
std::atomic<float> g_linDamageScale{ 0.25f };
}  // namespace

int16_t LinWeskerDamage(uintptr_t enemy, int16_t amount) {
    if (amount <= 0 || !g_linDamageOn.load(std::memory_order_relaxed) || !IsLinWesker(enemy)) return amount;
    const float scaled = amount * g_linDamageScale.load(std::memory_order_relaxed);
    return static_cast<int16_t>(scaled < 1.0f ? 1.0f : scaled);
}

// The enemies' request hook (action_log.cpp, game thread): Lost in Nightmares' Wesker does not leave with "Wesker
// s'entête (Lost in Nightmares)": Standby_ready 0x95 (his time or the combo) refused. His defeat LoseBgn 0x94 at 1 health
// goes through (user, 2026-10-26: refused, his AI asked it again and again and attacked no more after a neck break).
bool LinWeskerStays(uintptr_t enemy, int action) {
    if (action != 0x95 || !g_linStubborn.load(std::memory_order_relaxed) || !IsLinWesker(enemy))
        return false;
    // His defeat ends with the same Standby_ready (logged: LoseBgn 0x94 kneels 0x72 / 0x73, stands 0x75, then 0x95): at
    // 1 health it goes through too.
    int16_t health = 0;
    if (mem::Read(enemy + actors::off::Health, health) && health <= 1) return false;
    // His FSM asks again after its time limit (logged once in 2 s); his Wait given back.
    g_linUnfreeze = enemy;
    static DWORD logged = 0;
    if (GetTickCount() - logged > 2000)
        LOG("wesker boss: Lost in Nightmares' Wesker %08X's Standby_ready refused (he stays)", static_cast<unsigned>(enemy));
    logged = GetTickCount();
    return true;
}

// player.cpp's actor damage hook: Wesker loses no health with "Wesker n'abandonne jamais".
bool WeskerKeepsHealth(uintptr_t enemy) {
    if (!g_keepHealth.load(std::memory_order_relaxed)) return false;
    uintptr_t vtable = 0;
    return mem::Read(enemy, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(kWeskerVtable));
}

void RegisterWeskerBoss() {
    Add({ .id = "enemy.wesker_gauge", .name = "Jauge d'abandon de Wesker",
          .help = "Pendant le combat contre Jill et Wesker, affiche en haut à gauche la jauge d'abandon de Wesker (il part "
                  "quand elle est vide), ses PV et le temps du combat depuis son arrivée (arrêté pendant le menu pause).",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsurePauseMenuWatch()) return false;
              g_gaugeShown = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnHud(DrawFight);
    Add({ .id = "enemy.wesker_health", .name = "PV de Wesker (combat final, Lost in Nightmares)",
          .help = "Pendant le combat final contre Wesker et celui de Lost in Nightmares, affiche en haut à gauche ses PV "
                  "avec une barre.",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsurePauseMenuWatch()) return false;
              g_healthShown = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnHud(DrawOtherWesker);
    Add({ .id = "enemy.lin_wesker_stubborn", .name = "Wesker s'entête (Lost in Nightmares)",
          .help = "Dans Lost in Nightmares, Wesker ne s'enfuit plus : ni après un combo complet de Chris et Jill (ce qui le "
                  "fait partir quand le jeu l'autorise), ni au bout de ses 3 minutes. À 1 PV (un neck break par exemple) il "
                  "perd quand même.",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureEnemyActionHook() || !EnsureFlagTestHook())) return false;
              g_linStubborn = on;
              return true;
          },
          .tick = TickLinStubborn,
          .available = [] { return game::IsSupported(); } });
    Feature& linHealth = Add({ .id = "enemy.lin_wesker_health", .name = "PV de Wesker (Lost in Nightmares)",
                               .help = "Dans Lost in Nightmares, Wesker reçoit ces PV (8500 dans le jeu), son maximum aussi. "
                                       "Appliqué à son arrivée et à chaque changement du curseur.",
                               .category = "Ennemis", .kind = Kind::Toggle,
                               .set = [](bool) {
                                   g_linHealthFor = 0;
                                   return true;
                               },
                               .available = [] { return game::IsSupported(); },
                               .value = 20000, .valueMin = 1000, .valueMax = 30000, .valueFormat = "%.0f PV" });
    Feature* const linHealthFeature = &linHealth;
    Feature& linDamage = Add({ .id = "enemy.lin_wesker_damage", .name = "Dégâts reçus par Wesker (Lost in Nightmares)",
                               .help = "Dans Lost in Nightmares, chaque dégât que Wesker reçoit est multiplié par le curseur "
                                       "(x0,25 = quatre fois moins) : coups du jeu comme ceux du trainer. Ses PV ne peuvent "
                                       "pas dépasser 32767, ce curseur fait durer le combat au-delà.",
                               .category = "Ennemis", .kind = Kind::Toggle,
                               .set = [](bool on) {
                                   if (on && !EnsureActorDamageHook()) return false;
                                   g_linDamageOn = on;
                                   return true;
                               },
                               .available = [] { return game::IsSupported(); },
                               .value = 0.25f, .valueMin = 0.05f, .valueMax = 1.0f, .valueFormat = "x%.2f" });
    Feature* const linDamageFeature = &linDamage;
    linDamage.tick = [linDamageFeature] { g_linDamageScale.store(linDamageFeature->value, std::memory_order_relaxed); };
    linHealth.tick = [linHealthFeature] { TickLinHealth(linHealthFeature->value); };

    MakeGaugePatches(g_stubbornPatches, "enemy.wesker_stubborn");
    Add({ .id = "enemy.wesker_stubborn", .name = "Wesker s'entête",
          .help = "Pendant le combat contre Jill et Wesker, sa jauge d'abandon ne descend plus. Ses PV baissent encore : "
                  "assez blessé, il finit par partir (bien plus tard).",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) { return SetGaugePatches(g_stubbornPatches, on); },
          .available = [] { return game::IsSupported(); } });
    MakeGaugePatches(g_neverPatches, "enemy.wesker_never_gives_up");
    Add({ .id = "enemy.wesker_never_gives_up", .name = "Wesker n'abandonne jamais",
          .help = "Pendant le combat contre Jill et Wesker, ni sa jauge d'abandon ni ses PV ne descendent : il ne part pas "
                  "à cause des coups reçus (il réagit toujours aux coups).",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureActorDamageHook()) return false;
              if (!SetGaugePatches(g_neverPatches, on)) return false;
              g_keepHealth = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });

    Add({ .id = "enemy.wesker_no_time_limit", .name = "Wesker sans limite de temps",
          .help = "Comme l'Ultimate Trainer : pendant le combat contre Jill et Wesker, ses durées de 400 et 180 deviennent "
                  "infinies. Elles sont lues quand il arrive : activer avant le combat (ou recommencer au point de "
                  "contrôle).",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureFsmHooks()) return false;
              g_noTimeLimit = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "enemy.lin_wesker_no_time_limit", .name = "Wesker sans limite de temps (Lost in Nightmares)",
          .help = "Dans Lost in Nightmares, Wesker part au bout de 3 minutes de combat : cette durée (et celle de 400) "
                  "devient infinie. Elle est lue quand il arrive : activer avant le combat (ou recommencer au point de "
                  "contrôle).",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureFsmHooks()) return false;
              g_noTimeLimitLin = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "enemy.jill_stays_down", .name = "Jill reste au sol tant que Wesker est là",
          .help = "Pendant la première phase du combat contre Jill et Wesker, Jill projetée reste allongée tant que Wesker "
                  "est là : tu peux te concentrer sur lui. Quand il part (deuxième phase) ou si tu décoches l'option, "
                  "elle se relève. La cinématique où Wesker projette Chris à travers la porte la relève quand même.",
          .category = "Ennemis", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureEnemyActionHook()) return false;
              g_jillStaysDown = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnFrame(TickJillHeld);
    Add({ .id = "diag.wesker_fsm_log", .name = "Journal des paramètres IA pendant Wesker (recherche)",
          .help = "Écrit dans trainer.log chaque paramètre constant de l'IA (entier ou réel) initialisé, une fois chacun : "
                  "sa valeur, qui l'a lu et si Wesker était là. Sert à trouver sa limite de temps.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureFsmHooks()) return false;
              if (on) {
                  std::lock_guard lock(g_fsmLock);
                  g_fsmSeen.clear();
              }
              g_fsmLog = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
}
}  // namespace features
