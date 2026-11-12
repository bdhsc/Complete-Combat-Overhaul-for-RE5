// The raft of the marshes played alone (user, 2026-10-28: "a raft with a crank: Sheva must turn it while I ride the
// platform to the other side; make the raft start on the other side, let Chris use the crank (only player 2 can in
// solo) so the trip is reversed; at the end a countdown as for the bridge, and when it ends the trip the other way
// starts by itself").
// The scene (recorded): the crank uOm08a6 (vtable RVA 0xBCCDA0, the bridge's class too) is linked to the raft uOm08a3
// (uOmFollow, vtable RVA 0xBEBD00) by its +0x1A00. Its progress +0x1970 (of +0x1974 = 400) places the raft: every
// frame the raft reads it (VA 0xAB1150: progress / max * 100); 0 = the east dock (x ~7450) where Chris boards, 400 =
// the west end (x ~5030) past the crank (x ~5680), where he gets off. Sheva winds 0 -> 400 in about 27 s; at rest the
// progress runs back slowly (about 2 a second). The raft follows the crank's ratio copy +0x197C (progress / max x 100,
// written by the crank's update VA 0xAB1720), read about 3 times a frame through its getter VA 0xAB1240 (traced; the
// second and third tries reversed the reflected ratio getter VA 0xAB1150 and the progress copy +0x17FC: the raft did
// not move, the first try writing the progress itself had moved it). The crank's own code (its handle VA 0xAB1360, its
// prompts, its end at the maximum VA 0xAB1200) reads +0x1970.
// Chris's prompt on that crank (handler RVA 0x6B1E10) in this room (0xC9) with the local player 1 only plays his
// order to Sheva; the crank's turning prompt (handler RVA 0x6B19E0, shown to player 2) takes whoever presses it.
// With the option: player 1's press is the turning prompt's for him, the crank turning as the game's; the raft is
// given the reversed ratio through both ratio getters (100 - the crank's:
// at the west end at first, brought back to the east dock by his winding, drifting back if he lets go early). The
// crank wound to its end, the raft stays at the east dock; once he lets go of the handle (user: "never at the end of
// the travel, when the player lets go"), a countdown at the top of the screen (the slider), then it goes back at
// Sheva's pace (27 s) and stays at the west end; the crank itself unwinds as the game's, the raft no longer follows it. (2026-10-28, first try: the crank's progress itself set to
// its end and every change reversed - the crank took itself for wound, only its cancel prompt left, handle down.)
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <MinHook.h>
#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace features {
bool EnsurePadQueryHooks();                       // quick_knife.cpp: RaftTick runs on the game thread
bool EnsurePromptPressHook();                     // action_log.cpp: RaftRedirect is called from it
bool PressPromptFor(uintptr_t button, int slot);  // action_log.cpp: the game's OnPressed, no hook rules
bool CrankHeldByPlayer();                         // crank.cpp: a player holds a crank's handle

namespace {
constexpr uintptr_t kCrankVtable = 0xBCCDA0;  // uOm08a6 (RVA)
constexpr uintptr_t kRaftVtable = 0xBEBD00;   // uOm08a3 (RVA)
constexpr uintptr_t kOrderHandler = 0x6B1E10, kTurnHandler = 0x6B19E0;  // the crank's prompts (RVA)
constexpr float kTripSeconds = 27.0f;         // Sheva's winding, end to end

enum class Phase { None, Out, Docked, Trip, Done };
std::atomic<bool> g_on{ false };
std::atomic<float> g_seconds{ 20.0f };
std::atomic<uintptr_t> g_crank{ 0 };
std::atomic<Phase> g_phase{ Phase::None };
std::atomic<DWORD> g_dockedSince{ 0 };  // the countdown's start (render thread reads it)
std::atomic<float> g_trip{ 0.0f };      // the raft's ratio on its own trip (0..100)
DWORD g_lastTrip = 0;

uintptr_t Base() { return reinterpret_cast<uintptr_t>(mem::Base()); }

bool IsRaftCrank(uintptr_t unit) {
    uintptr_t vtable = 0, raft = 0, raftVtable = 0;
    return unit && mem::Read(unit, vtable) && vtable == Base() + kCrankVtable && mem::Read(unit + 0x1A00, raft) && raft &&
           mem::Read(raft, raftVtable) && raftVtable == Base() + kRaftVtable;
}

float Progress(uintptr_t crank) {
    float v = 0.0f;
    return mem::Read(crank + 0x1970, v) ? v : 0.0f;
}

float Max(uintptr_t crank) {
    float v = 0.0f;
    return mem::Read(crank + 0x1974, v) ? v : 0.0f;
}

// The raft's progress (0 = the east dock, max = the west end) for the crank's.
float RaftProgress(uintptr_t crank, bool& reversed) {
    const float progress = Progress(crank), max = Max(crank);
    reversed = g_on.load(std::memory_order_relaxed) && crank == g_crank.load(std::memory_order_relaxed);
    if (!reversed) return progress;
    switch (g_phase.load(std::memory_order_relaxed)) {
    case Phase::Out:
        return max - progress;  // his winding brings it back
    case Phase::Docked:
        return 0.0f;            // at the east dock
    case Phase::Trip:
        return g_trip.load(std::memory_order_relaxed) * max / 100.0f;
    case Phase::Done:
        return max;             // at the west end
    default:
        reversed = false;
        return progress;
    }
}

// The crank's ratio getters (thiscall, the result on the x87 stack).
using RatioFn = float(__fastcall*)(void* self, void* edx);

// The crank's ratio copy getter (VA 0xAB1240: fld [ecx+0x197C]), the raft's reading.
RatioFn g_ratioCopy = nullptr;

float __fastcall HookRatioCopy(void* self, void* edx) {
    const float real = g_ratioCopy(self, edx);
    bool reversed = false;
    const uintptr_t crank = reinterpret_cast<uintptr_t>(self);
    const float raft = RaftProgress(crank, reversed), max = Max(crank);
    return reversed && max > 0.0f ? raft / max * 100.0f : real;
}

bool Hook(uintptr_t rva, const uint8_t* expected, size_t size, void* detour, void** original) {
    uint8_t* target = mem::Rva(rva);
    if (!mem::IsGameCode(target, size) || std::memcmp(target, expected, size) != 0) {
        LOG("raft: unexpected bytes %s at RVA %X", mem::Hex(target, size).c_str(), static_cast<unsigned>(rva));
        return false;
    }
    mem::EnsureExecutable(target, size);
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    return st == MH_OK || st == MH_ERROR_ENABLED;
}

bool EnsureRaftHooks() {
    static bool hooked = false;
    if (hooked) return true;
    // fld [ecx+0x197C]; ret
    static const uint8_t copy[] = { 0xD9, 0x81, 0x7C, 0x19, 0x00, 0x00, 0xC3 };
    // Not the reflected ratio getter VA 0xAB1150 (hooked by the second try): reversed, Chris's winding motion no longer
    // matched the crank's handle (user, 2026-10-28) - it is his action's reading.
    if (!Hook(0x6B1240, copy, sizeof(copy), reinterpret_cast<void*>(&HookRatioCopy), reinterpret_cast<void**>(&g_ratioCopy)))
        return false;
    hooked = true;
    LOG("raft: crank ratio copy hook installed");
    return true;
}

// The crank's prompt with that handler (RVA), from sActionButton's list.
uintptr_t PromptOf(uintptr_t owner, uintptr_t handlerRva) {
    uintptr_t system = 0, array = 0;
    uint32_t count = 0;
    if (!mem::Read(Base() + 0xDB1794, system) || !system || !mem::Read(system + 0x30, count) ||
        !mem::Read(system + 0x3C, array) || !array || count > 2000)
        return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t button = 0, o = 0, handler = 0;
        if (mem::Read(array + 4 * i, button) && button && mem::Read(button + 4, o) && o == owner &&
            mem::Read(button + 0x3C, handler) && handler == Base() + handlerRva)
            return button;
    }
    return 0;
}

// The raft's crank among the prompts' owners (its order prompt), once a second.
uintptr_t FindCrank() {
    uintptr_t system = 0, array = 0;
    uint32_t count = 0;
    if (!mem::Read(Base() + 0xDB1794, system) || !system || !mem::Read(system + 0x30, count) ||
        !mem::Read(system + 0x3C, array) || !array || count > 2000)
        return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t button = 0, owner = 0, handler = 0;
        if (mem::Read(array + 4 * i, button) && button && mem::Read(button + 0x3C, handler) &&
            handler == Base() + kOrderHandler && mem::Read(button + 4, owner) && IsRaftCrank(owner))
            return owner;
    }
    return 0;
}

void DrawCountdown() {
    if (!g_on.load(std::memory_order_relaxed)) return;
    const Phase phase = g_phase.load(std::memory_order_relaxed);
    const DWORD since = g_dockedSince.load(std::memory_order_relaxed);
    if (phase != Phase::Docked || !since) return;
    const float left = g_seconds.load(std::memory_order_relaxed) - static_cast<float>(GetTickCount() - since) / 1000.0f;
    if (left <= 0.0f) return;
    char text[48];
    std::snprintf(text, sizeof(text), i18n::Tr("Radeau : départ dans %d s"), static_cast<int>(std::ceil(left)));
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float size = screen.y * 0.045f;
    const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 at((screen.x - extent.x) * 0.5f, screen.y * 0.08f);
    const ImU32 colour = left <= 5.0f ? IM_COL32(255, 90, 70, 255) : IM_COL32(255, 235, 200, 255);  // red at the end
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(ImGui::GetFont(), size, at, colour, text);
}
}  // namespace

bool IsSoloRaftCrank(uintptr_t unit) { return g_on.load(std::memory_order_relaxed) && IsRaftCrank(unit); }

// action_log.cpp's OnPressed hook, before the game's press: player 1's order to Sheva on the raft's crank becomes his
// own turn of it.
bool RaftRedirect(uintptr_t button, int slot) {
    uintptr_t owner = 0, handler = 0;
    if (!g_on.load(std::memory_order_relaxed) || !button || !actors::SlotIs(slot, 0) || !mem::Read(button + 0x3C, handler) ||
        handler != Base() + kOrderHandler || !mem::Read(button + 4, owner) || !IsRaftCrank(owner))
        return false;
    if (g_phase.load(std::memory_order_relaxed) != Phase::Out) return true;  // the raft is docked or on its way
    const uintptr_t turn = PromptOf(owner, kTurnHandler);
    if (!turn) {
        LOG("raft: no turning prompt on the crank %08X", static_cast<unsigned>(owner));
        return false;
    }
    uint8_t active = 0;
    if (mem::Read(turn + 0xA, active) && !active) mem::Write<uint8_t>(turn + 0xA, 1);
    const bool ok = PressPromptFor(turn, slot);
    uint32_t action = 0;
    mem::Read(actors::Player(0) + 0x10E0, action);
    LOG("raft: player 1 turns the crank (prompt %08X%s, his action %X)", static_cast<unsigned>(turn), ok ? "" : " FAILED", action);
    return true;
}

// quick_knife.cpp's pad query hook (game thread).
void RaftTick() {
    if (!g_on.load(std::memory_order_relaxed)) return;
    const DWORD now = GetTickCount();
    uintptr_t crank = g_crank.load(std::memory_order_relaxed);
    if (!actors::InGame() || (crank && !IsRaftCrank(crank))) {
        if (crank) LOG("raft: the crank %08X is gone", static_cast<unsigned>(crank));
        g_crank = crank = 0;
        g_phase = Phase::None;
        g_dockedSince = 0;
    }
    if (!crank) {
        static DWORD lastScan = 0;
        if (!actors::InGame() || now - lastScan < 1000) return;
        lastScan = now;
        crank = FindCrank();
        if (!crank || Max(crank) <= 0.0f) return;
        g_crank = crank;
        // Already wound to its end (the scene played, or left so by the first try): the raft stays at the west end.
        g_phase = Progress(crank) >= Max(crank) - 0.5f ? Phase::Done : Phase::Out;
        LOG("raft: crank %08X found (progress %.0f of %.0f), the raft at the west end%s", static_cast<unsigned>(crank),
            Progress(crank), Max(crank), g_phase.load() == Phase::Done ? " (the crank already wound)" : "");
        return;
    }
    const float progress = Progress(crank), max = Max(crank);
    switch (g_phase.load(std::memory_order_relaxed)) {
    case Phase::Out:
        if (progress >= max - 0.5f) {  // wound to its end: the raft at the east dock
            g_dockedSince = 0;
            g_phase = Phase::Docked;
            LOG("raft: at the east dock");
        }
        return;
    case Phase::Docked:
        if (CrankHeldByPlayer()) {  // the time runs from his letting go of the handle
            g_dockedSince = 0;
            return;
        }
        if (!g_dockedSince.load()) {
            g_dockedSince = now ? now : 1;
            LOG("raft: the handle let go, leaving in %.0f s", g_seconds.load());
            return;
        }
        if (static_cast<float>(now - g_dockedSince.load()) < g_seconds.load(std::memory_order_relaxed) * 1000.0f) return;
        g_lastTrip = now;
        g_trip = 0.0f;
        g_phase = Phase::Trip;
        LOG("raft: the trip to the west end starts");
        return;
    case Phase::Trip: {
        const float dt = static_cast<float>(now - g_lastTrip) / 1000.0f;
        if (dt < 0.01f) return;
        g_lastTrip = now;
        const float next = g_trip.load(std::memory_order_relaxed) + 100.0f / kTripSeconds * (dt > 0.1f ? 0.1f : dt);
        if (next >= 100.0f) {
            g_phase = Phase::Done;
            g_dockedSince = 0;
            LOG("raft: at the west end");
            return;
        }
        g_trip = next;
        return;
    }
    default:
        return;
    }
}

void RegisterRaft() {
    Feature& raft = Add({ .id = "partner.solo_raft", .name = "Radeau à manivelle : seul",
                          .help = "Au radeau des marais (la manivelle que seul Sheva peut tourner) : le radeau démarre "
                                  "de l'autre côté, Chris tourne lui-même la manivelle pour le faire venir, puis un compte "
                                  "à rebours s'affiche en haut de l'écran (le curseur) : le temps de monter dessus avant "
                                  "qu'il reparte tout seul de l'autre côté.",
                          .category = "Joueur", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = 20, .valueMin = 5, .valueMax = 120, .valueFormat = "%.0f s" });
    Feature* const self = &raft;
    raft.set = [self](bool on) {
        if (on && (!EnsureRaftHooks() || !EnsurePadQueryHooks() || !EnsurePromptPressHook())) return false;
        g_seconds = self->value;
        g_on = on;
        if (!on) {
            g_crank = 0;
            g_phase = Phase::None;
            g_dockedSince = 0;
        }
        return true;
    };
    raft.tick = [self] { g_seconds.store(self->value, std::memory_order_relaxed); };
    OnHud(DrawCountdown);
}
}  // namespace features
