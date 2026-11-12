// Scenario steps waiting for both players, played alone (user, 2026-10-28, chapter 1-1: "after the cutscene both
// characters must pick up or equip their weapon; Sheva has none in my solo games: the game should be satisfied as soon
// as player 1 equipped one").
// Found (2026-10-29): the step waits for player 2 to EQUIP a weapon - an M92F given to Sheva through the trainer
// (inventory.give_p2), her AI equipped it at once (action 0x20) and the next cutscene started. The check is not in the
// automaton (none of the logs below) but in the stage's own script, VA 0x73A070: "partner.weapons_step_solo" at the end
// of this file. The automaton's conditions read FreeFlag / RSF / SceNo (the scenario number) / mCntState (seconds in
// the state) / mbGotoArrival; the stage's automatons wait for scenario numbers (1174, 1175...).
// Research (this file's diagnostic, off by default): the scenario does not read player 2's weapon in hand [P+0x2634]
// (traced: only the player update VA 0xB70529 / 0xB705B7); her item records' states are read by the item manager's
// GetItem (VA 0xC4B700, thiscall(manager = P+0x21A0, key) ret 4: the record whose state +0x1C is the key, 0xE = the
// item equipped; 251 callers) and VA 0xC4BD00 (the player update). The log names GetItem's callers on player 2.
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <MinHook.h>
#include <intrin.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace features {
bool SetUnitTimeBoost(uintptr_t unit, float factor);  // time_speed.cpp: one unit's frame time multiplied
bool FinalFightPartnerFree();  // final_fight.cpp: "Zones à deux" stands aside in the volcano fight
namespace {
std::atomic<bool> g_log{ false };

// Return addresses up the stack: words in the game's code just after a call (E8 rel32, FF /2 forms).
void Callers(const uint32_t* stack, char* out, size_t size) {
    out[0] = 0;
    size_t used = 0;
    int found = 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    for (int i = 0; i < 96 && found < 7; ++i) {
        uint32_t v = 0;
        if (!mem::Read(reinterpret_cast<uintptr_t>(stack + i), v) || !mem::IsGameCode(reinterpret_cast<void*>(v - 6), 6)) continue;
        uint8_t code[6]{};
        if (!mem::SafeRead(reinterpret_cast<void*>(v - 6), code, sizeof(code))) continue;
        const bool call = code[1] == 0xE8 || (code[4] == 0xFF && (code[5] & 0x38) == 0x10) ||
                          (code[0] == 0xFF && (code[1] & 0x38) == 0x10) || (code[3] == 0xFF && (code[4] & 0x38) == 0x10);
        if (!call) continue;
        used += static_cast<size_t>(std::snprintf(out + used, size - used, " %06X", static_cast<unsigned>(v - base)));
        if (used >= size - 8) break;
        ++found;
    }
}

using GetItemFn = uintptr_t(__fastcall*)(void* self, void* edx, int key);
GetItemFn g_getItem = nullptr;

uintptr_t __fastcall HookGetItem(void* self, void* edx, int key) {
    const uintptr_t result = g_getItem(self, edx, key);
    if (g_log.load(std::memory_order_relaxed)) {
        static uintptr_t seen[64] = {};
        static int count = 0;
        const uintptr_t p2 = actors::Player(1);
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (p2 && reinterpret_cast<uintptr_t>(self) == p2 + 0x21A0 && count < 64) {
            bool known = false;
            for (int i = 0; i < count && !known; ++i) known = seen[i] == caller;
            if (!known) {
                seen[count++] = caller;
                char chain[96];
                Callers(static_cast<const uint32_t*>(_AddressOfReturnAddress()), chain, sizeof(chain));
                LOG("solo scenario: GetItem(%X) on player 2 from RVA %06X -> %08X (callers%s)", key,
                    static_cast<unsigned>(caller - reinterpret_cast<uintptr_t>(mem::Base())), static_cast<unsigned>(result), chain);
            }
        }
    }
    return result;
}

// The automaton's flag setter (VA 0xC29920, thiscall(order, type 0..9, index, value) ret 0xC: type 1 = the scenario's
// flags (set VA 0x77FCE0 / clear VA 0x77FD50 on [RVA 0xE340A4])): every check order writes its result through it,
// each time it is evaluated (the player check VA 0xC2C200 does). Logged once per caller / type / index / value.
using SetFlagFn = void(__fastcall*)(void* self, void* edx, int type, int index, int value);
SetFlagFn g_setFlag = nullptr;
std::atomic<bool> g_flagLog{ false };

// The checks' latest answers (game thread writes, render thread reads for the summary).
struct Answer {
    uintptr_t caller, order;
    int type, index, value;
    DWORD tick;
    unsigned count;
};
Answer g_answers[64] = {};
int g_answerCount = 0;

void NoteAnswer(uintptr_t caller, uintptr_t order, int type, int index, int value) {
    for (int i = 0; i < g_answerCount; ++i) {
        Answer& a = g_answers[i];
        if (a.caller == caller && a.order == order) {
            a.type = type;
            a.index = index;
            a.value = value;
            a.tick = GetTickCount();
            ++a.count;
            return;
        }
    }
    if (g_answerCount < 64) g_answers[g_answerCount++] = { caller, order, type, index, value, GetTickCount(), 1 };
}

// Every 5 s: the checks still asked (within the last second) that answer false.
void TickAnswerSummary() {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (!g_flagLog.load(std::memory_order_relaxed) || now - last < 5000) return;
    last = now;
    for (int i = 0; i < g_answerCount; ++i) {
        const Answer a = g_answers[i];
        if (a.value || now - a.tick > 1000) continue;
        LOG("solo scenario: still false: RVA %06X order %08X flag %d / %d (%u answers)",
            static_cast<unsigned>(a.caller - reinterpret_cast<uintptr_t>(mem::Base())), static_cast<unsigned>(a.order), a.type,
            a.index, a.count);
    }
}

void __fastcall HookSetFlag(void* self, void* edx, int type, int index, int value) {
    if (g_flagLog.load(std::memory_order_relaxed))
        NoteAnswer(reinterpret_cast<uintptr_t>(_ReturnAddress()), reinterpret_cast<uintptr_t>(self), type, index, value & 0xFF);
    if (g_flagLog.load(std::memory_order_relaxed)) {
        struct Seen {
            uintptr_t caller;
            int type, index, value;
        };
        static Seen seen[256] = {};
        static int count = 0;
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const int v = value & 0xFF;
        bool known = false;
        for (int i = 0; i < count && !known; ++i)
            known = seen[i].caller == caller && seen[i].type == type && seen[i].index == index && seen[i].value == v;
        if (!known && count < 256) {
            seen[count++] = { caller, type, index, v };
            char chain[96];
            Callers(static_cast<const uint32_t*>(_AddressOfReturnAddress()), chain, sizeof(chain));
            LOG("solo scenario: flag type %d index %d = %d from RVA %06X (order %08X; callers%s)", type, index, v,
                static_cast<unsigned>(caller - reinterpret_cast<uintptr_t>(mem::Base())), static_cast<unsigned>(reinterpret_cast<uintptr_t>(self)), chain);
        }
    }
    g_setFlag(self, edx, type, index, value);
}

// The scenario flags themselves (profile +0x23214, bit 0x80000000 >> (index & 31) of word index >> 5): set VA 0x77FCE0,
// clear VA 0x77FD50 (thiscall([RVA 0xE340A4], index) ret 4). The automaton's conditions read only flags (its property
// parameters name FreeFlag, RSF, SceNo, mCntState, mbGotoArrival), so whatever "both armed" is sets one of them.
using ScenarioFlagFn = void(__fastcall*)(void* self, void* edx, int index);
ScenarioFlagFn g_flagSet = nullptr, g_flagClear = nullptr;

void LogScenarioFlag(const char* what, int index) {
    char chain[96];
    Callers(static_cast<const uint32_t*>(_AddressOfReturnAddress()), chain, sizeof(chain));
    LOG("solo scenario: scenario flag %d %s (callers%s)", index, what, chain);
}

void __fastcall HookFlagSet(void* self, void* edx, int index) {
    if (g_flagLog.load(std::memory_order_relaxed)) LogScenarioFlag("set", index);
    g_flagSet(self, edx, index);
}

void __fastcall HookFlagClear(void* self, void* edx, int index) {
    if (g_flagLog.load(std::memory_order_relaxed)) LogScenarioFlag("cleared", index);
    g_flagClear(self, edx, index);
}

bool EnsureScenarioFlagHooks() {
    static bool hooked = false;
    if (hooked) return true;
    // push edi; mov edi, ecx; cmp byte [edi+0x1C], 0
    const uint8_t expected[] = { 0x57, 0x8B, 0xF9, 0x80, 0x7F, 0x1C, 0x00 };
    for (uintptr_t rva : { 0x37FCE0u, 0x37FD50u }) {
        uint8_t* target = mem::Rva(rva);
        if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
            LOG("solo scenario: unexpected bytes %s at RVA %X", mem::Hex(target, sizeof(expected)).c_str(), static_cast<unsigned>(rva));
            return false;
        }
        mem::EnsureExecutable(target, sizeof(expected));
        void* detour = rva == 0x37FCE0u ? reinterpret_cast<void*>(&HookFlagSet) : reinterpret_cast<void*>(&HookFlagClear);
        void** original = rva == 0x37FCE0u ? reinterpret_cast<void**>(&g_flagSet) : reinterpret_cast<void**>(&g_flagClear);
        MH_STATUS st = MH_CreateHook(target, detour, original);
        if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
        st = MH_EnableHook(target);
        if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    }
    hooked = true;
    LOG("solo scenario: scenario flag hooks installed");
    return true;
}

bool EnsureSetFlagHook() {
    if (!EnsureScenarioFlagHooks()) return false;
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x829920);
    // mov eax, [esp+4]; cmp eax, 9; ja
    const uint8_t expected[] = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xF8, 0x09, 0x0F, 0x87 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("solo scenario: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookSetFlag), reinterpret_cast<void**>(&g_setFlag));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("solo scenario: flag setter hook installed at %p", target);
    return true;
}

// The automaton's area check (cFSMOrder function VA 0xC340C0, its parameter (FuncParamAreaHit) type +0x14 4): every
// pawn (uPawn) of a list inside an area (mode +0x3C 0: "all", 1: "any"), the area's vfunc +0x24 (thiscall(area, unit)
// ret 4) asked per pawn; its result written to a flag (VA 0xC34232). Chapter 1-1's man (automaton s100_butch_all):
// "single" (his talk area 2 boots butcherTalk_Single, "I am waiting for your partner") until both players are in area 9
// ("all" mode), then "double" (area 5 boots butcherTalk_Double, the cutscene). With "Zones à deux : le joueur 1 suffit"
// player 2 counts as inside when player 1 is (the "all" mode's call VA 0xC341EC: mov eax, [edx+0x24]; mov ecx, ebp;
// call eax): user-verified on the man (2026-10-29).
std::atomic<bool> g_areaSolo{ false };
// "Combat final : Jill en partenaire": not in the volcano fight (user, 2026-11-01: as Chris fell, Wesker jumped onto the
// platform as if the rock scene was done and Jill appeared by him - logged: an area needing players 1 and 2 with player
// 1 alone in it, player 2 counted with him).
bool AreaSolo() { return g_areaSolo.load(std::memory_order_relaxed) && !FinalFightPartnerFree(); }

bool AskInside(uintptr_t fn, uintptr_t area, uintptr_t unit) {
    using InsideFn = bool(__fastcall*)(void* self, void* edx, uintptr_t unit);
    __try {
        return reinterpret_cast<InsideFn>(fn)(reinterpret_cast<void*>(area), nullptr, unit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

extern "C" bool __cdecl AreaAllInside(uintptr_t area, uintptr_t unit) {
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(area, vtable) || !mem::Read(vtable + 0x24, fn) || !fn) return false;
    bool inside = AskInside(fn, area, unit);
    const uintptr_t p1 = actors::Player(0), p2 = actors::Player(1);
    bool lent = false;
    if (!inside && AreaSolo() && unit && unit == p2 && p1) lent = inside = AskInside(fn, area, p1);
    if (g_flagLog.load(std::memory_order_relaxed)) {
        static uintptr_t lastUnit[8] = {};
        static int lastResult[8] = {};
        int slot = -1;
        for (int i = 0; i < 8 && slot < 0; ++i)
            if (lastUnit[i] == unit || !lastUnit[i]) slot = i;
        if (slot >= 0 && (lastUnit[slot] != unit || lastResult[slot] != (inside ? 1 : 0) + (lent ? 2 : 0))) {
            lastUnit[slot] = unit;
            lastResult[slot] = (inside ? 1 : 0) + (lent ? 2 : 0);
            const int index = actors::PlayerIndex(unit);
            LOG("solo scenario: area %08X: %s %08X %s%s", static_cast<unsigned>(area), index >= 0 ? "player" : "pawn",
                static_cast<unsigned>(unit), inside ? "inside" : "outside", lent ? " (player 1's answer)" : "");
        }
    }
    return inside;
}

__declspec(naked) void AreaAllStub() {
    __asm {
        mov eax, [esp + 4]  // the pawn pushed for the area's call
        push eax
        push ebp            // the area
        call AreaAllInside
        add esp, 8
        ret 4               // the area's call popped its argument
    }
}

// The area tests of the check's other types (user's wait: type 1, VA 0xCBFD50; and VA 0xCBFD90): the players inside
// as bits of their game slots ([area+8] +0x10, or [area+0x20] for the second) against the required players
// ([area+4] +0x3C), mode [area+4] +0xC: 0 any of them, 1 all of them. Player 2's bit counts as set when player 1's is.
using AreaTestFn = bool(__fastcall*)(void* self, void* edx);
AreaTestFn g_areaTest[2] = {};

bool LentAll(uintptr_t area, uint32_t bits) {
    uintptr_t data = 0;
    uint32_t mode = 0, mask = 0;
    const int s1 = actors::SlotOf(actors::Player(0)), s2 = actors::SlotOf(actors::Player(1));
    if (!mem::Read(area + 4, data) || !data || !mem::Read(data + 0xC, mode) || mode != 1 || !mem::Read(data + 0x3C, mask) ||
        s1 < 0 || s2 < 0 || !(bits & (1u << s1)))
        return false;
    const bool lent = ((bits | (1u << s2)) & mask) == mask;
    if (lent) {
        static uintptr_t logged = 0;
        if (logged != area) {
            logged = area;
            LOG("solo scenario: area %08X needs players %X, inside %X: player 2 counted with player 1", static_cast<unsigned>(area),
                mask, bits);
        }
    }
    return lent;
}

bool __fastcall HookAreaTest0(void* self, void* edx) {
    const bool real = g_areaTest[0](self, edx);
    uintptr_t hit = 0;
    uint32_t bits = 0;
    const uintptr_t area = reinterpret_cast<uintptr_t>(self);
    if (real || !AreaSolo() || !mem::Read(area + 8, hit) || !hit || !mem::Read(hit + 0x10, bits))
        return real;
    return LentAll(area, bits);
}

bool __fastcall HookAreaTest1(void* self, void* edx) {
    const bool real = g_areaTest[1](self, edx);
    uint32_t bits = 0;
    const uintptr_t area = reinterpret_cast<uintptr_t>(self);
    if (real || !AreaSolo() || !mem::Read(area + 0x20, bits)) return real;
    return LentAll(area, bits);
}

// The area check order itself (VA 0xC340C0, thiscall(order, arg) ret 4): its parameter [[arg+4] + [arg+0xC] x 0x14
// + 0x10], type +0x14, the area from VA 0xC2AEF0 (thiscall(parameter)). Logged once per parameter.
using OrderFn = void(__fastcall*)(void* self, void* edx, uintptr_t arg);
OrderFn g_areaOrder = nullptr;

uintptr_t AreaOf(uintptr_t param) {
    using AreaFn = uintptr_t(__fastcall*)(void* self, void* edx);
    __try {
        return reinterpret_cast<AreaFn>(mem::Rva(0x82AEF0))(reinterpret_cast<void*>(param), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void __fastcall HookAreaOrder(void* self, void* edx, uintptr_t arg) {
    if (g_flagLog.load(std::memory_order_relaxed)) {
        uintptr_t list = 0, param = 0;
        uint32_t index = 0, type = 0, flags = 0;
        if (mem::Read(arg + 4, list) && list && mem::Read(arg + 0xC, index) && mem::Read(list + index * 0x14 + 0x10, param) &&
            param && mem::Read(param + 0x14, type)) {
            static uintptr_t seen[32] = {};
            static int count = 0;
            bool known = false;
            for (int i = 0; i < count && !known; ++i) known = seen[i] == param;
            if (!known && count < 32) {
                seen[count++] = param;
                const uintptr_t area = AreaOf(param);
                if (area) mem::Read(area + 0x1C, flags);
                LOG("solo scenario: area order %08X parameter %08X type %u area %08X flags +1C %X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(self)),
                    static_cast<unsigned>(param), type, static_cast<unsigned>(area), flags);
            }
        }
    }
    g_areaOrder(self, edx, arg);
}

bool HookAt(uintptr_t rva, const std::vector<uint8_t>& expected, void* detour, void** original) {
    uint8_t* target = mem::Rva(rva);
    if (!mem::IsGameCode(target, expected.size()) || std::memcmp(target, expected.data(), expected.size()) != 0) {
        LOG("solo scenario: unexpected bytes %s at RVA %X", mem::Hex(target, expected.size()).c_str(), static_cast<unsigned>(rva));
        return false;
    }
    mem::EnsureExecutable(target, expected.size());
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    return st == MH_OK || st == MH_ERROR_ENABLED;
}

bool EnsureAreaTestHooks() {
    static bool hooked = false;
    if (hooked) return true;
    // push esi; mov esi, [ecx+8]; test esi, esi / mov edx, [ecx+4]; mov eax, [edx+0xC]
    // sub esp, 8; mov eax, [esp+0xC]; push ebx
    if (!HookAt(0x8340C0, { 0x83, 0xEC, 0x08, 0x8B, 0x44, 0x24, 0x0C, 0x53 }, reinterpret_cast<void*>(&HookAreaOrder),
                reinterpret_cast<void**>(&g_areaOrder)))
        return false;
    if (!HookAt(0x8BFD50, { 0x56, 0x8B, 0x71, 0x08, 0x85, 0xF6 }, reinterpret_cast<void*>(&HookAreaTest0),
                reinterpret_cast<void**>(&g_areaTest[0])) ||
        !HookAt(0x8BFD90, { 0x8B, 0x51, 0x04, 0x8B, 0x42, 0x0C }, reinterpret_cast<void*>(&HookAreaTest1),
                reinterpret_cast<void**>(&g_areaTest[1])))
        return false;
    hooked = true;
    LOG("solo scenario: area test hooks installed");
    return true;
}

// The single-unit types (3: a player by index, 5: the order's own unit) ask at VA 0xC3429D (mov eax, [edx+0x24];
// mov ecx, edi; call eax), the area in edi.
__declspec(naked) void AreaOneStub() {
    __asm {
        mov eax, [esp + 4]  // the pawn pushed for the area's call
        push eax
        push edi            // the area
        call AreaAllInside
        add esp, 8
        ret 4
    }
}

mem::Patch* g_areaOneCall = nullptr;
mem::Patch* g_areaCall = nullptr;

bool EnsureAreaOnePatch() {
    if (g_areaOneCall) return g_areaOneCall->IsApplied() || g_areaOneCall->Apply();
    uint8_t* site = mem::Rva(0x83429D);
    const std::vector<uint8_t> original = { 0x8B, 0x42, 0x24, 0x8B, 0xCF, 0xFF, 0xD0 };
    if (!mem::IsGameCode(site, original.size()) || std::memcmp(site, original.data(), original.size()) != 0) {
        LOG("solo scenario: unexpected area call %s", mem::Hex(site, original.size()).c_str());
        return false;
    }
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&AreaOneStub) - reinterpret_cast<uintptr_t>(site + 5));
    std::vector<uint8_t> patched = { 0xE8, 0, 0, 0, 0, 0x90, 0x90 };
    std::memcpy(patched.data() + 1, &rel, 4);
    g_areaOneCall = new mem::Patch("solo scenario: area check (one unit)", site, original, patched);
    OnRestore([] {
        if (g_areaOneCall) g_areaOneCall->Restore();  // it calls into this DLL
    });
    return g_areaOneCall->Apply();
}

bool EnsureAreaPatch() {
    if (!EnsureAreaOnePatch()) return false;
    if (g_areaCall) return g_areaCall->IsApplied() || g_areaCall->Apply();
    uint8_t* site = mem::Rva(0x8341EC);
    const std::vector<uint8_t> original = { 0x8B, 0x42, 0x24, 0x8B, 0xCD, 0xFF, 0xD0 };
    if (!mem::IsGameCode(site, original.size()) || std::memcmp(site, original.data(), original.size()) != 0) {
        LOG("solo scenario: unexpected area call %s", mem::Hex(site, original.size()).c_str());
        return false;
    }
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&AreaAllStub) - reinterpret_cast<uintptr_t>(site + 5));
    std::vector<uint8_t> patched = { 0xE8, 0, 0, 0, 0, 0x90, 0x90 };
    std::memcpy(patched.data() + 1, &rel, 4);
    g_areaCall = new mem::Patch("solo scenario: area check", site, original, patched);
    OnRestore([] {
        if (g_areaCall) g_areaCall->Restore();  // it calls into this DLL
    });
    return g_areaCall->Apply();
}

bool EnsureGetItemHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x84B700);
    // push ecx; push ebp; push esi; mov esi, [esp+0x10]; cmp esi, 0xE
    const uint8_t expected[] = { 0x51, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x83, 0xFE, 0x0E };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("solo scenario: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookGetItem), reinterpret_cast<void**>(&g_getItem));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("solo scenario: GetItem hook installed at %p", target);
    return true;
}

// The weapons step itself (found 2026-10-29): stage s115's script VA 0x73A070 (every frame while the scenario number
// [[0x11B2158] + slot x 0x5940 + 0x273E0] is below 1175) counts the players ready - a player controlled by a person
// (+0x2DA8 0) with an equipped weapon (GetItem(0xE) not the bare hands 0x137), the AI partner (+0x2DA8 1) with an item
// in its first inventory slot (GetItem(0)) - and at two raises the scenario number to 1175 (the automaton s115_1175
// jumps to the next area 2.5 s later). Its two GetItem calls (VA 0x73A2DB key 0xE, VA 0x73A368 key 0) come here: a
// player who fails counts with player 1's weapon in hand when player 1 has one.
std::atomic<bool> g_weaponsSolo{ false };

uintptr_t __fastcall WeaponsStepGetItem(void* manager, void* edx, int key) {
    const auto getItem = reinterpret_cast<GetItemFn>(mem::Rva(0x84B700));
    const uintptr_t record = getItem(manager, edx, key);
    uint32_t id = 0;
    if (!g_weaponsSolo.load(std::memory_order_relaxed) || (record && (key != 0xE || (mem::Read(record + 4, id) && id != 0x137))))
        return record;
    const uintptr_t p1 = actors::Player(0);
    if (!p1 || reinterpret_cast<uintptr_t>(manager) == p1 + 0x21A0) return record;
    const uintptr_t own = getItem(reinterpret_cast<void*>(p1 + 0x21A0), nullptr, 0xE);
    if (!own || !mem::Read(own + 4, id) || id == 0x137) return record;
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG("solo scenario: weapons step: player 2 counted with player 1's weapon %X", id);
    }
    return own;
}

mem::Patch* g_weaponsCall[2] = {};

bool EnsureWeaponsPatches() {
    if (!g_weaponsCall[0]) {
        // push 0xE; mov ecx, edi; call GetItem / push esi; mov ecx, edi; call GetItem
        const struct {
            uintptr_t rva;
            std::vector<uint8_t> original;
        } sites[2] = { { 0x33A2D7, { 0x6A, 0x0E, 0x8B, 0xCF, 0xE8, 0x20, 0x14, 0x51, 0x00 } },
                       { 0x33A365, { 0x56, 0x8B, 0xCF, 0xE8, 0x93, 0x13, 0x51, 0x00 } } };
        for (int i = 0; i < 2; ++i) {
            uint8_t* site = mem::Rva(sites[i].rva);
            const size_t call = sites[i].original.size() - 5;
            std::vector<uint8_t> patched = sites[i].original;
            const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&WeaponsStepGetItem) -
                                                     reinterpret_cast<uintptr_t>(site + call + 5));
            std::memcpy(patched.data() + call + 1, &rel, 4);
            g_weaponsCall[i] = new mem::Patch(i ? "solo scenario: weapons step (AI partner)" : "solo scenario: weapons step",
                                              site, sites[i].original, patched);
        }
        OnRestore([] {
            mem::RestoreAll({ g_weaponsCall[0], g_weaponsCall[1] });  // they call into this DLL
        });
    }
    return mem::ApplyAll({ g_weaponsCall[0], g_weaponsCall[1] });
}

// The house of chapter 1-1 (stage s116): the closing scene starts once both players are inside (scenario area 3,
// "all", mask 0x13; scenario number 1278) and the last one in closes the door. The partner's automaton NpcKeepArea
// waits at the door in state "PL_House" until the other player is in the entrance (its FuncParamAreaHitPl
// "PL_House_p0000": FreeFlag 0 when player "who 1" (the other) is in area 5), then "IN_House" walks it in - so the AI
// always enters second and closes. The players' area check (cFSMOrderPlayer vtable slot 97, VA 0xC40830,
// thiscall(order, arg) ret 4; its parameter [[arg+4] + [arg+0xC] x 0x14 + 0x10]) answers true there for the AI
// partner (+0x2DA8 1): it goes in first and player 1 closes. Also its earlier states "RUN_Tuto" / "PL_RUN" (their
// "RUN_Tuto_p0002" / "PL_RUN_p0001": FreeFlag 1 when the other is in area 5 leads straight to "IN_House"): without
// them it went through "Keep_03" and Sheva's line "MESS_42" first and waited at the door (user, 2026-10-29). Only
// during the attack (scenario numbers 1240 to 1277).
const char* const kHouseChecks[] = { "PL_House_p0000", "RUN_Tuto_p0002", "PL_RUN_p0001" };
std::atomic<bool> g_houseFirst{ false };
using AreaHitPlFn = void(__fastcall*)(void* order, void* edx, uintptr_t arg);
AreaHitPlFn g_areaHitPl = nullptr;

// An automaton object whose +4 is its name (MtString: refs, length, characters): parameters and states.
bool NameIs(uintptr_t object, const char* name) {
    uintptr_t text = 0;
    uint32_t length = 0;
    char buffer[32] = {};
    const size_t wanted = std::strlen(name);
    return mem::Read(object + 4, text) && text && mem::Read(text + 4, length) && length == wanted && wanted < sizeof(buffer) &&
           mem::SafeRead(reinterpret_cast<void*>(text + 8), buffer, wanted) && std::memcmp(buffer, name, wanted) == 0;
}

uint32_t ScenarioNumber();

bool HouseCheck(uintptr_t param) {
    for (const char* name : kHouseChecks)
        if (NameIs(param, name)) return true;
    return false;
}

void __fastcall HookAreaHitPl(void* order, void* edx, uintptr_t arg) {
    if (g_houseFirst.load(std::memory_order_relaxed)) {
        uintptr_t pawn = 0, list = 0, param = 0;
        uint32_t index = 0, controller = 0, type = 0, flag = 0;
        uint8_t value = 0;
        const uintptr_t self = reinterpret_cast<uintptr_t>(order);
        if (mem::Read(self + 8, pawn) && actors::PlayerIndex(pawn) >= 0 && mem::Read(pawn + 0x2DA8, controller) && controller == 1 &&
            mem::Read(arg + 4, list) && list && mem::Read(arg + 0xC, index) && mem::Read(list + index * 0x14 + 0x10, param) && param &&
            HouseCheck(param) && ScenarioNumber() >= 1240 && ScenarioNumber() < 1278 && mem::Read(param + 8, type) && mem::Read(param + 0xC, flag) && mem::Read(param + 0x10, value)) {
            static uintptr_t logged = 0;
            if (logged != param) {
                logged = param;
                LOG("solo scenario: house: the AI partner %08X goes in first", static_cast<unsigned>(pawn));
            }
            reinterpret_cast<SetFlagFn>(mem::Rva(0x829920))(order, nullptr, static_cast<int>(type), static_cast<int>(flag), value);
            return;
        }
    }
    g_areaHitPl(order, edx, arg);
}

// From the Majini's arrival (the camera shot cam02EV at scenario number 1239, then 1240) to the closing scene (1278)
// the AI partner's time runs 3 times faster (user, 2026-10-29: Sheva reacts slower than him, he still went in first;
// its motion speed +0x3A4 at 3 changed nothing for the AI, and started at the door it came too late): time_speed.cpp's
// unit frame time hook. Its own speed ("Vitesse - Joueur N" or the game's) is untouched, back when the boost ends.
std::atomic<int> g_boostIndex{ -1 };
constexpr float kHouseTime = 3.0f;

uint32_t ScenarioNumber() {
    uintptr_t profile = 0;
    uint32_t slot = 0, number = 0;
    if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), profile) && profile && mem::Read(profile + 0x20, slot))
        mem::Read(profile + slot * 0x5940 + 0x273E0, number);
    return number;
}

void HouseSpeedTick() {
    int index = -1;
    const uint32_t scenario = actors::InGame() ? ScenarioNumber() : 0;
    if (g_houseFirst.load(std::memory_order_relaxed) && scenario >= 1240 && scenario < 1278) {
        for (int i = 0; i < 2 && index < 0; ++i) {
            const uintptr_t p = actors::Player(i);
            uint32_t controller = 0;
            if (p && mem::Read(p + 0x2DA8, controller) && controller == 1) index = i;
        }
    }
    const int previous = g_boostIndex.exchange(index);
    if (index == previous) return;
    if (index >= 0 && SetUnitTimeBoost(actors::Player(index), kHouseTime)) {
        LOG("solo scenario: house: partner %d's time x%.0f", index + 1, kHouseTime);
    } else {
        SetUnitTimeBoost(0, 1.0f);
        LOG("solo scenario: house: partner's time back to normal");
    }
}

bool EnsureAreaHitPlHook() {
    static bool hooked = false;
    if (hooked) return true;
    // push ebx; mov ebx, ecx; push edi; mov edi, [ebx+8]; mov eax, [edi]
    if (!HookAt(0x840830, { 0x53, 0x8B, 0xD9, 0x57, 0x8B, 0x7B, 0x08, 0x8B, 0x07 }, reinterpret_cast<void*>(&HookAreaHitPl),
                reinterpret_cast<void**>(&g_areaHitPl)))
        return false;
    hooked = true;
    LOG("solo scenario: player area check hook installed");
    return true;
}

// Chapter 1-1's last fight, survived until help comes (user, 2026-10-29: "a hidden timer"): three automatons count it,
// "1322" (100 s), "1330" (200 s) and "1345" (50 s), the radio calls in between. Each waits in its first state (named
// after it) for its scenario number, then in "Timer_on" until mCntState (order +0x14C, seconds in the state) >= its
// cAIFSM::ParamConstS32 (value +0x10, copy +0x1C). The automaton agents' update (cAIFSMAgent vtable slot 6, VA
// 0xBD36A0, thiscall(agent); a cFSMOrder's agent at +0xC, its core +4: +4 the cAIFSM, +8 the state) finds them each
// frame: the time left is shown (the calls' few seconds not counted) and each timer's length set by its own slider
// (user: "one slider per part of the fight": 100 s until the Executioner breaks the gate, 200 s until the helicopter
// pilot's call, 50 s until he comes).
// cAIFSM: +4 name, +0xC / +0x18 states; a state +4 name, +0xC / +0x18 transitions; a transition +0x14 / +0x20
// conditions; a condition +8 / +0xC left / right parameters, +0x10 operator (8: >=); a ParamProperty +0x1C its name.
struct SurvivalTimer {
    const char* name;
    std::atomic<uintptr_t> param{ 0 };  // its ParamConstS32
    std::atomic<int> original{ 0 };     // the game's seconds
    std::atomic<float> left{ 0.0f };
    std::atomic<int> status{ 0 };       // 1 waiting, 2 counting, 3 done
    std::atomic<DWORD> tick{ 0 };
};
SurvivalTimer g_timers[3] = { { "1322" }, { "1330" }, { "1345" } };
std::atomic<bool> g_survivalOn{ false };
std::atomic<float> g_phaseSeconds[3] = { 100.0f, 200.0f, 50.0f };  // 0 = the game's (its slider off)
using AgentUpdateFn = void(__fastcall*)(void* agent, void* edx);
AgentUpdateFn g_agentUpdate = nullptr;

template <typename T>
T At(uintptr_t address) {
    T value{};
    mem::Read(address, value);
    return value;
}

// The "Timer_on" state of an automaton and its mCntState >= N parameter.
uintptr_t TimerParam(uintptr_t fsm, uintptr_t& timerState) {
    const uint32_t states = At<uint32_t>(fsm + 0xC);
    const uintptr_t stateList = At<uintptr_t>(fsm + 0x18);
    for (uint32_t s = 0; stateList && s < states && s < 64; ++s) {
        const uintptr_t state = At<uintptr_t>(stateList + 4 * s);
        if (!state || !NameIs(state, "Timer_on")) continue;
        const uint32_t transitions = At<uint32_t>(state + 0xC);
        const uintptr_t transitionList = At<uintptr_t>(state + 0x18);
        for (uint32_t t = 0; transitionList && t < transitions && t < 16; ++t) {
            const uintptr_t transition = At<uintptr_t>(transitionList + 4 * t);
            const uint32_t conditions = transition ? At<uint32_t>(transition + 0x14) : 0;
            const uintptr_t conditionList = transition ? At<uintptr_t>(transition + 0x20) : 0;
            for (uint32_t c = 0; conditionList && c < conditions && c < 8; ++c) {
                const uintptr_t condition = At<uintptr_t>(conditionList + 4 * c);
                const uintptr_t left = condition ? At<uintptr_t>(condition + 8) : 0, right = condition ? At<uintptr_t>(condition + 0xC) : 0;
                if (!left || !right || At<uint32_t>(condition + 0x10) != 8 ||
                    At<uintptr_t>(right) != reinterpret_cast<uintptr_t>(mem::Rva(0xC384E8)))
                    continue;
                const uintptr_t property = At<uintptr_t>(left + 0x1C);
                char text[12]{};
                if (!property || At<uint32_t>(property + 4) != 9 || !mem::SafeRead(reinterpret_cast<void*>(property + 8), text, 9) ||
                    std::strcmp(text, "mCntState") != 0)
                    continue;
                timerState = state;
                return right;
            }
        }
    }
    return 0;
}

void WriteSeconds(uintptr_t param, int seconds) {
    if (At<int>(param + 0x10) != seconds) mem::Write<int>(param + 0x10, seconds);
    if (At<int>(param + 0x1C) != seconds) mem::Write<int>(param + 0x1C, seconds);
}

void __fastcall HookAgentUpdate(void* agent, void* edx) {
    g_agentUpdate(agent, edx);
    if (!g_survivalOn.load(std::memory_order_relaxed)) return;
    const uintptr_t a = reinterpret_cast<uintptr_t>(agent);
    if (At<uintptr_t>(a - 0xC) != reinterpret_cast<uintptr_t>(mem::Rva(0xC3D7A8))) return;  // a plain cFSMOrder's agent
    const uintptr_t core = At<uintptr_t>(a + 4);
    const uintptr_t fsm = core ? At<uintptr_t>(core + 4) : 0, state = core ? At<uintptr_t>(core + 8) : 0;
    if (!fsm || !state) return;
    for (int i = 0; i < 3; ++i) {
        SurvivalTimer& t = g_timers[i];
        if (!NameIs(fsm, t.name)) continue;
        uintptr_t timerState = 0;
        const uintptr_t param = TimerParam(fsm, timerState);
        if (!param) return;
        if (t.param.load() != param) {
            t.original = At<int>(param + 0x1C);
            t.param = param;
            LOG("survival timer: %s found, %d s", t.name, t.original.load());
        }
        const float chosen = g_phaseSeconds[i].load(std::memory_order_relaxed);
        const int seconds = chosen > 0.0f ? std::max(1, static_cast<int>(std::lround(chosen))) : t.original.load();
        WriteSeconds(param, seconds);
        const uintptr_t order = a - 0xC;
        if (state == timerState) {
            t.status = 2;
            t.left = std::max(0.0f, seconds - At<float>(order + 0x14C));
        } else if (NameIs(state, t.name)) {
            t.status = 1;
            t.left = static_cast<float>(seconds);
        } else {
            t.status = 3;
            t.left = 0.0f;
        }
        const DWORD now = GetTickCount();
        t.tick = now ? now : 1;
        return;
    }
}

bool EnsureAgentUpdateHook() {
    static bool hooked = false;
    if (hooked) return true;
    // push esi; mov esi, ecx; mov eax, [esi+4]; test eax, eax; mov dword [esi+0xC], 0
    if (!HookAt(0x7D36A0, { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x04, 0x85, 0xC0, 0xC7, 0x46, 0x0C }, reinterpret_cast<void*>(&HookAgentUpdate),
                reinterpret_cast<void**>(&g_agentUpdate)))
        return false;
    hooked = true;
    LOG("survival timer: automaton update hook installed");
    return true;
}

// The game's lengths back (option off, unload): on the parameters seen within the last second (still loaded).
void RestoreSurvivalTimers() {
    const DWORD now = GetTickCount();
    for (SurvivalTimer& t : g_timers) {
        const uintptr_t param = t.param.exchange(0);
        if (param && now - t.tick.load() < 1000 && t.original.load() > 0) WriteSeconds(param, t.original.load());
        t.tick = 0;
    }
}

void DrawSurvival() {
    if (!g_survivalOn.load(std::memory_order_relaxed) || !actors::InGame()) return;
    const DWORD now = GetTickCount();
    float left = 0.0f;
    bool started = false, seen = false;
    for (const SurvivalTimer& t : g_timers) {
        const DWORD tick = t.tick.load(std::memory_order_relaxed);
        if (!tick || now - tick > 1000) continue;
        seen = true;
        const int status = t.status.load(std::memory_order_relaxed);
        if (status >= 2) started = true;
        if (status <= 2) left += t.left.load(std::memory_order_relaxed);
    }
    if (!seen || !started || left <= 0.0f) return;
    const int total = static_cast<int>(std::ceil(left));
    char text[48];
    std::snprintf(text, sizeof(text), i18n::Tr("Survie : %d:%02d"), total / 60, total % 60);
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float size = screen.y * 0.045f;
    const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 at((screen.x - extent.x) * 0.5f, screen.y * 0.08f);
    const ImU32 colour = left <= 10.0f ? IM_COL32(120, 255, 120, 255) : IM_COL32(255, 235, 200, 255);  // green at the end
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(ImGui::GetFont(), size, at, colour, text);
}

// The run from the collapsing floor (user, 2026-10-30: "the player I play falls and player 2 saves him: make player 2
// fall"). The scene is one object, uOma004_Collapse (vtable RVA 0xC068B8), both players in its events (vtables RVA
// 0xC06438 cActPlRun, 0xC06464 cActPlActBtn, 0xC06490 cActPlSuccess, 0xC064BC cActPlFailed; player +0x20, object
// +0x24). Each press on its mash prompt (handler RVA 0x707C40) gives the presser 2 points (+0x17FC + 4 x slot, copied to
// +0x1A84 + 4 x slot by VA 0xB07B70); who falls comes from comparing them (VA 0xB09DEC, 0xB0A15A, 0xB0A33E, the
// players' own VA 0xB0B1D7 / 0xB0B617): the one with fewer points falls (user: player 2 fell twice once player 1
// out-pressed her). A person's points must also keep above a target growing with the time or he stumbles (VA 0xB09A66,
// the AI partner exempt), so player 1's points are raised above hers rather than hers lowered.
std::atomic<bool> g_collapseP2Falls{ false };

void TickCollapseP2Falls() {
    const uintptr_t p1 = actors::Player(0), p2 = actors::Player(1);
    uint32_t action = 0;
    uintptr_t event = 0, vtable = 0, scene = 0, sceneVtable = 0;
    if (!p1 || !p2 || !mem::Read(p1 + 0x10E0, action) || action != 0x80000000 || !mem::Read(p1 + 0x10F8, event) || !event ||
        !mem::Read(event, vtable))
        return;
    const uintptr_t rva = vtable - reinterpret_cast<uintptr_t>(mem::Base());
    if ((rva != 0xC06438 && rva != 0xC06464 && rva != 0xC06490 && rva != 0xC064BC) || !mem::Read(event + 0x24, scene) ||
        !scene || !mem::Read(scene, sceneVtable) || sceneVtable != reinterpret_cast<uintptr_t>(mem::Rva(0xC068B8)))
        return;
    uint8_t s1 = 0, s2 = 0;
    if (!mem::Read(p1 + 0x2D9C, s1) || !mem::Read(p2 + 0x2D9C, s2) || s1 > 3 || s2 > 3 || s1 == s2) return;
    for (uintptr_t base : { scene + 0x17FC, scene + 0x1A84 }) {  // the points and their copy
        uint32_t mine = 0, hers = 0;
        if (!mem::Read(base + 4 * s1, mine) || !mem::Read(base + 4 * s2, hers)) continue;
        if ((mine & 0xFFFF) < (hers & 0xFFFF) + 8 && (hers & 0xFFFF) + 8 < 0xFFFF) {
            mem::Write<uint32_t>(base + 4 * s1, (hers & 0xFFFF) + 8);  // ahead of her by 4 presses
            static uintptr_t logged = 0;
            if (logged != scene) {
                logged = scene;
                LOG("collapse run %08X: player 1's points (%u) kept above player 2's (%u): player 2 falls",
                    static_cast<unsigned>(scene), mine & 0xFFFF, hers & 0xFFFF);
            }
        }
    }
}
}  // namespace

void RegisterSoloScenario() {
    OnFrame(TickAnswerSummary);
    OnHud(DrawSurvival);
    OnRestore(RestoreSurvivalTimers);
    Add({ .id = "scenario.survival_timer", .name = "Survie du chapitre 1-1 : compte à rebours",
          .help = "Au combat de la fin du chapitre 1-1, affiche en haut de l'écran le temps qu'il reste à tenir (sans les "
                  "appels radio entre les phases). Les trois curseurs « Survie 1-1 » règlent la durée de chaque partie.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureAgentUpdateHook()) return false;
              g_survivalOn = on;
              if (!on) RestoreSurvivalTimers();
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    struct Phase {
        const char* id;
        const char* name;
        const char* help;
        float value, min, max;
    };
    static const Phase kPhases[3] = {
        { "scenario.survival_phase1", "Survie 1-1 : jusqu'au bourreau", "Durée de la première partie, jusqu'à ce que le "
          "bourreau défonce la grille (100 s dans le jeu). Avec « Survie du chapitre 1-1 : compte à rebours ».", 100, 10, 300 },
        { "scenario.survival_phase2", "Survie 1-1 : jusqu'à l'appel de l'hélico", "Durée de la deuxième partie, jusqu'à "
          "l'appel du pilote de l'hélicoptère (200 s dans le jeu).", 200, 10, 600 },
        { "scenario.survival_phase3", "Survie 1-1 : jusqu'à la fin", "Durée de la dernière partie, après l'appel du pilote "
          "(50 s dans le jeu).", 50, 5, 300 },
    };
    for (int i = 0; i < 3; ++i) {
        Feature& f = Add({ .id = kPhases[i].id, .name = kPhases[i].name, .help = kPhases[i].help, .category = "Joueur",
                           .kind = Kind::Toggle,
                           .set = [i](bool on) {
                               if (!on) g_phaseSeconds[i] = 0.0f;  // the game's
                               return true;
                           },
                           .available = [] { return game::IsSupported(); },
                           .value = kPhases[i].value, .valueMin = kPhases[i].min, .valueMax = kPhases[i].max,
                           .valueFormat = "%.0f s", .defaultOn = true });
        Feature* const fp = &f;
        f.tick = [fp, i] { g_phaseSeconds[i].store(fp->value, std::memory_order_relaxed); };
    }
    OnFrame(HouseSpeedTick);
    Add({ .id = "partner.weapons_step_solo", .name = "Chapitre 1-1 : les armes du joueur 1 suffisent",
          .help = "Au début du chapitre 1-1, après le briefing, le jeu attend que les deux joueurs aient une arme équipée "
                  "pour lancer la suite. Avec cette option, le joueur 1 armé suffit : le joueur 2 compte comme armé sans "
                  "rien lui donner.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureWeaponsPatches()) return false;
              if (!on && g_weaponsCall[0]) mem::RestoreAll({ g_weaponsCall[0], g_weaponsCall[1] });
              g_weaponsSolo = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "partner.house_enter_first", .name = "Maison du chapitre 1-1 : le partenaire entre le premier",
          .help = "Quand il faut se réfugier dans la petite maison au chapitre 1-1, le partenaire IA entre le premier et "
                  "attend dedans au lieu de t'attendre devant la porte. Le jeu fait fermer la porte par le dernier entré : "
                  "c'est donc toi qui la fermes, que tu joues Chris ou Sheva. Pour qu'il arrive avant toi, il va 3 fois "
                  "plus vite de l'arrivée des Majini jusqu'à la scène de la porte, puis reprend sa vitesse (celle du "
                  "trainer si « Vitesse » est cochée).",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureAreaHitPlHook()) return false;
              g_houseFirst = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "partner.area_solo", .name = "Zones à deux : le joueur 1 suffit",
          .help = "Quand le jeu attend que les deux joueurs soient dans une zone pour lancer la suite (par exemple pour "
                  "parler à l'homme au début du chapitre 1-1), le joueur 2 compte comme présent dès que le joueur 1 y "
                  "est.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureAreaPatch() || !EnsureAreaTestHooks())) return false;
              g_areaSolo = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "partner.collapse_p2_falls", .name = "Course sur le sol qui s'effondre : le joueur 2 tombe",
          .help = "Dans la course où le sol s'effondre (avec les QTE), le jeu fait tomber celui des deux qui a le moins "
                  "martelé la touche, et l'autre le rattrape. Avec cette case, c'est toujours le joueur 2 qui tombe et toi "
                  "qui le sauves : tes appuis comptent toujours un peu plus que les siens.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_collapseP2Falls = on;
              return true;
          },
          .tick = [] { TickCollapseP2Falls(); },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "diag.scenario_flag_log", .name = "Journal des drapeaux du scénario (recherche)",
          .help = "Recherche : note dans trainer.log chaque vérification du script qui écrit un drapeau (appelant, drapeau, "
                  "valeur), une fois chacune.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureSetFlagHook() || !EnsureAreaPatch())) return false;
              g_flagLog = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "diag.item_caller_log", .name = "Journal des lectures d'objet équipé du joueur 2 (recherche)",
          .help = "Recherche : note dans trainer.log chaque endroit du jeu qui demande l'objet équipé du joueur 2.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureGetItemHook()) return false;
              g_log = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
}
}  // namespace features
