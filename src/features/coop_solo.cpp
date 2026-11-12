// Partner actions validated from afar (user, 2026-10-27: "Sheva validates the partner action at a distance as soon as
// Chris is ready", like the give at a distance; she is never moved). The co-op objects (classes under uOmCoop* /
// uOmAreaCoop: area doors uOmf300-f304, doors, card doors, gates, boxes, the S301 levers, treasures, twists, broad
// jumps) count the players who pressed their prompt. The area door uOmf302 (vtable RVA 0xBC8A18): its prompt (kind 0x1F,
// handler RVA 0x6A3A90) puts the presser in cOmEventPL_ActPre, then its vfunc +0x188(index, 0, choice) sets his bit
// +0x18DC and his choice +0x17FC and gives him cOmCoopDoorAction_pl_act_push_ready (the waiting pose: player action
// 0x80000000, an event action); its check (vfunc +0x184, kind 0xF1) opens once 2 players share a choice. The partner's
// AI walks there and presses the same prompt (a door) or another object's (the other of two levers).
// With the option on, by family (user, 2026-10-27: "it depends: exceptions"):
// - area doors (uOmCoopDoorArea): first pressed for player 2 where she stands once player 1 had waited 0.4 s in an event
//   action (user-verified), but she only answers once free (user, 2026-10-29: after climbing a ladder, more than 5 s with
//   her time slowed: "the kind of wait that can get me killed"): now done by player 1 alone like the gates (their check
//   VA 0xAA2DD0 opens once a choice has VA 0xA83070() players). The remote press (Mode::RemotePress: her prompt, else
//   the nearest prompt with the same handler on another co-op object within 15 m) is kept for no class now;
// - the actions done together (a gate lifted by two, uOmCoopGate: her press placed her at her post next to Chris, the
//   game's own snap, user: "no teleport"): player 1 does them alone through the game's one-player count. The players
//   needed (VA 0xA83070, thiscall on the object, 18 callers in the co-op classes: 2, or 1 with the game flag
//   [[RVA 0xE340A4]+0x10434] & 0x80000) answers 1 for them; player 2 is left alone;
// - the boxes pushed by two (uOmCoopBox, uOm089b in the caves): their message function (VA 0xA9B1A0, kind 0xF1) takes
//   the pushers needed from the box's own +0x1A48 (2 there, 1 for a box one player pushes); with one registered of two it
//   asks its action 6 (it shakes) instead of the push: user, 2026-10-27, stuck. Player 1's press on a box's prompt sets
//   +0x1A48 to 1 first (put back when the option goes off), and the box is pushed alone as a one-player box;
// - the broad jumps (uOmCoopJumpBroad: boosts, jumps to a ladder) need both players (user): left to the game;
// - elevators taking both players to another area (uOm08b2 under uOmElevator, not a co-op class: "an area change not
//   covered", user): first pressed for player 2 like the area doors once the elevator showed its cancel prompt (kind
//   2, handler RVA 0x6C8360 on uOm08b2); now player 1 alone (user, 2026-10-29, same wait): their check (uOmElevator
//   vtable slot 97, VA 0xAC6D50) counts the registered players (VA 0xA82CA0) against a fixed 2 at VA 0xAC6DB9 and
//   0xAC6DF7 (cmp ebx, 2), made 1 while the option is on. The airboat (uOmBoatAir) stays pressed for her (RemoteWait).
// - the levers turned by two (uOmCoopTwist, uOm08e4; user, 2026-10-30: "the prompt is a simple one but Chris still waits
//   for Sheva"): a player at his post marks himself ready (+0x1970 + slot) and sends message 0xF0; the object's message
//   filter (vtable +0x184, VA 0xAAFAD0) passes 0xF0 (it becomes 0xF1, all ready) and asks the rotation (action 1) on
//   0xF1 only with 2 registered players (cmp ebx, 2 at VA 0xAAFB97 and 0xAAFBCD), made 1 while the option is on. The
//   rotation's start (VA 0xAB08A0) gives every player its turn event (cOmCoopTwistEventPL_ActRotate, DTI 0x11F99E0),
//   registered or not, like the kicked doors: its GetPlayer call (VA 0xAB08B3, the slot in ebx) goes through
//   TwistRotateStub. Its end (VA 0xAB0B60) releases the registered players only.
// - the doors opened by two buttons pressed together (uOmCoopCardDoor, uOm0918; user, 2026-10-30: "a double button
//   waits for Sheva"): the same message filter (vtable +0x184, VA 0xAA0F30: cmp ebp, 2 at VA 0xAA0F7E for 0xF1 and VA
//   0xAA0FD7 for 0xF0), made 1. Their opening's start (VA 0xAA2410) gives every player cOmCoopCardDoorEventPL_ActOpen
//   (DTI 0x11F8CEC), whose start (VA 0xAA24A0) places him beyond the door (his place from VA 0xAA1850 by his +0x2D9C,
//   written to +0x30; a fade for player 1): left to every player, registered or not (user, 2026-10-30: kept to the
//   registered ones, Sheva stayed out as the door closed - "Sheva has to be teleported for this one").
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/i18n.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <MinHook.h>
#include <imgui.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace features {
bool EnsurePadQueryHooks();                        // quick_knife.cpp: CoopSoloTick runs on the game thread
bool EnsurePromptPressHook();                      // action_log.cpp: CoopSoloPressed is called from it
bool EnsureEnemyActionHook();                      // action_log.cpp: the units' request core (TrapLeverRequested)
bool PressPromptFor(uintptr_t button, int slot);   // action_log.cpp: the game's OnPressed, no hook rules
bool LeverPromptAlone(uintptr_t owner);            // levers.cpp: a paired lever done alone
bool CoopSoloActionPrompt(uintptr_t owner);        // below: a prompt of an object done alone, on the action button
bool LivePrompt(uintptr_t button);                 // below: a prompt still in the game's list
void ObstacleClimbTick();                          // obstacle_climb.cpp: obstacles climbed alone (game thread)
void InventoryDiscardTick();                       // inventory.cpp: "Jeter l'objet sélectionné" (game thread)
void FinalQteTick();                               // final_fight.cpp: the volcano's finish (game thread)
void RegisterObstacleClimb();                      // obstacle_climb.cpp

namespace {
constexpr uint32_t kEventAction = 0x80000000;
std::atomic<bool> g_on{ false };

enum class Mode { None, RemotePress, RemoteWait, Alone, Box, Both };

// The family of an object from its class chain (GetDTI = vtable slot 4, `mov eax, dti; ret`; DTI +4 name, +0x10
// parent): not a co-op object, area door, a broad jump, or another co-op object.
Mode ClassMode(uintptr_t vtable) {
    uintptr_t fn = 0, dti = 0;
    uint8_t code[6]{};
    if (!mem::Read(vtable + 0x10, fn) || !mem::SafeRead(reinterpret_cast<void*>(fn), code, sizeof(code)) ||
        code[0] != 0xB8 || code[5] != 0xC3)
        return Mode::None;
    std::memcpy(&dti, code + 1, 4);
    bool coop = false;
    for (int depth = 0; dti && depth < 8; ++depth) {
        uintptr_t name = 0;
        char text[48]{};
        if (!mem::Read(dti + 4, name) || !name || !mem::SafeRead(reinterpret_cast<void*>(name), text, sizeof(text) - 1))
            break;
        if (std::strcmp(text, "uOmElevator") == 0) return Mode::Alone;  // not a co-op class (user, 2026-10-27)
        if (std::strcmp(text, "uOmBoatAir") == 0) return Mode::RemoteWait;   // the airboat (3-1; user, 2026-10-28)
        if (std::strstr(text, "JumpBroad")) return Mode::Both;
        if (std::strcmp(text, "uOmCoopBox") == 0) return Mode::Box;
        if (std::strstr(text, "Coop")) coop = true;
        if (!mem::Read(dti + 0x10, dti)) break;
    }
    return coop ? Mode::Alone : Mode::None;
}

// By vtable, remembered (the count is asked by the objects' own code, from the game thread only).
Mode ModeOf(uintptr_t object) {
    static uintptr_t vtables[16] = {};
    static Mode modes[16] = {};
    static int next = 0;
    uintptr_t vtable = 0;
    if (!object || !mem::Read(object, vtable) || !vtable) return Mode::None;
    for (int i = 0; i < 16; ++i)
        if (vtables[i] == vtable) return modes[i];
    const Mode mode = ClassMode(vtable);
    vtables[next] = vtable;
    modes[next] = mode;
    next = (next + 1) % 16;
    return mode;
}

bool CoopObject(uintptr_t object) { return ModeOf(object) != Mode::None; }

const char* ClassName(uintptr_t object) {
    static char text[48];
    uintptr_t vtable = 0, fn = 0, dti = 0, name = 0;
    uint8_t code[6]{};
    text[0] = 0;
    if (object && mem::Read(object, vtable) && mem::Read(vtable + 0x10, fn) &&
        mem::SafeRead(reinterpret_cast<void*>(fn), code, sizeof(code)) && code[0] == 0xB8) {
        std::memcpy(&dti, code + 1, 4);
        if (mem::Read(dti + 4, name) && name) mem::SafeRead(reinterpret_cast<void*>(name), text, sizeof(text) - 1);
    }
    return text[0] ? text : "?";
}

bool Position(uintptr_t unit, float (&pos)[3]) { return unit && mem::SafeRead(reinterpret_cast<void*>(unit + 0x30), pos, sizeof(pos)); }

uint32_t Action(uintptr_t unit) {
    uint32_t action = 0;
    return unit && mem::Read(unit + 0x10E0, action) ? action : 0xFFFFFFFF;
}

// Game thread.
enum class Stage { Idle, Pressed, Tried, Done };
Stage g_stage = Stage::Idle;
uintptr_t g_button = 0, g_owner = 0, g_handler = 0, g_tried = 0;
Mode g_mode = Mode::None;
DWORD g_pressTick = 0, g_waitSince = 0, g_triedTick = 0;
bool g_fallbackTried = false;

void Reset() {
    g_stage = Stage::Idle;
    g_button = g_owner = g_handler = g_tried = 0;
    g_mode = Mode::None;
    g_waitSince = 0;
    g_fallbackTried = false;
}

// A prompt pressed for a player (its active flag +0xA set for the call when the game has it off: he is away; the game
// sets it again itself, and nothing is written into the prompt after the press: its object may replace its prompts
// then, a jump does).
bool PressForPlayer(uintptr_t button, int index, const char* what) {
    const uintptr_t p = actors::Player(index);
    const int slot = actors::SlotOf(p);
    uint8_t active = 0;
    uintptr_t owner = 0;
    if (!p || slot < 0 || !mem::Read(button + 0xA, active) || !mem::Read(button + 4, owner)) return false;
    if (!active) mem::Write<uint8_t>(button + 0xA, 1);
    const bool ok = PressPromptFor(button, slot);
    LOG("coop alone: %s prompt %08X (%s %08X) pressed for player %d (slot %d)%s, his action now %X", what,
        static_cast<unsigned>(button), ClassName(owner), static_cast<unsigned>(owner), index + 1, slot,
        ok ? "" : " - FAILED", Action(p));
    return ok;
}

bool PressForPlayer2(uintptr_t button, const char* what) {
    const bool ok = PressForPlayer(button, 1, what);
    g_tried = button;
    g_triedTick = GetTickCount();
    return ok;
}

float Distance(uintptr_t a, uintptr_t b) {
    float pa[3]{}, pb[3]{};
    if (!Position(a, pa) || !Position(b, pb)) return 1e9f;
    return std::sqrt((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) + (pa[2] - pb[2]) * (pa[2] - pb[2]));
}

// `owner` shows its cancel prompt (kind 2, active): a player waits for the other one.
// The player is registered on the object (its message kind 0 sets the bit of his game slot in +0x18DC).
bool Registered(uintptr_t object, uintptr_t player) {
    const int slot = actors::SlotOf(player);
    uint8_t bits = 0;
    return slot >= 0 && slot < 4 && mem::Read(object + 0x18DC, bits) && (bits >> slot) & 1;
}

bool CancelShown(uintptr_t owner) {
    uintptr_t system = 0, array = 0;
    uint32_t count = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), system) || !system || !mem::Read(system + 0x30, count) ||
        !mem::Read(system + 0x3C, array) || !array || count > 2000)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t button = 0, o = 0;
        uint32_t kind = 0;
        uint8_t active = 0;
        if (mem::Read(array + 4 * i, button) && button && mem::Read(button + 4, o) && o == owner &&
            mem::Read(button + 0x1C, kind) && kind == 2 && mem::Read(button + 0xA, active) && active)
            return true;
    }
    return false;
}

// The prompt of `owner` with that handler (RVA), from sActionButton's list.
uintptr_t PromptOf(uintptr_t owner, uintptr_t handlerRva) {
    uintptr_t system = 0, array = 0;
    uint32_t count = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), system) || !system || !mem::Read(system + 0x30, count) ||
        !mem::Read(system + 0x3C, array) || !array || count > 2000)
        return 0;
    const uintptr_t wanted = reinterpret_cast<uintptr_t>(mem::Rva(handlerRva));
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t button = 0, o = 0, handler = 0;
        if (mem::Read(array + 4 * i, button) && button && mem::Read(button + 4, o) && o == owner &&
            mem::Read(button + 0x3C, handler) && handler == wanted)
            return button;
    }
    return 0;
}

// The nearest prompt with player 1's handler on another co-op object within 15 m of his (the other lever).
uintptr_t OtherObjectPrompt() {
    uintptr_t system = 0, array = 0;
    uint32_t count = 0;
    float from[3]{};
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), system) || !system || !mem::Read(system + 0x30, count) ||
        !mem::Read(system + 0x3C, array) || !array || count > 2000 || !Position(g_owner, from))
        return 0;
    uintptr_t best = 0;
    float bestDistance = 1500.0f;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t button = 0, owner = 0, handler = 0;
        float pos[3]{};
        if (!mem::Read(array + 4 * i, button) || !button || !mem::Read(button + 4, owner) || !owner || owner == g_owner ||
            !mem::Read(button + 0x3C, handler) || handler != g_handler || !Position(owner, pos) || !CoopObject(owner))
            continue;
        const float d = std::sqrt((pos[0] - from[0]) * (pos[0] - from[0]) + (pos[1] - from[1]) * (pos[1] - from[1]) +
                                  (pos[2] - from[2]) * (pos[2] - from[2]));
        if (d < bestDistance) {
            bestDistance = d;
            best = button;
        }
    }
    return best;
}

// The players a co-op object needs: 1 for the ones player 1 does alone.
using NeededFn = int(__fastcall*)(void* self, void* edx);
NeededFn g_needed = nullptr;

int __fastcall HookNeeded(void* self, void* edx) {
    const int game = g_needed(self, edx);
    const uintptr_t object = reinterpret_cast<uintptr_t>(self);
    const Mode mode = ModeOf(object);
    if (!g_on.load(std::memory_order_relaxed) || game <= 1 || (mode != Mode::Alone && mode != Mode::Box)) return game;
    static uintptr_t logged = 0;
    if (logged != object) LOG("coop alone: %s %08X needs player 1 only", ClassName(object), static_cast<unsigned>(object));
    logged = object;
    return 1;
}

// Research (user, 2026-10-28: a box made a one-pusher box starts its push and cancels it, keyboard too): every message a
// co-op box gets (VA 0xA83370, thiscall(object, player index, kind, arg) ret 0xC; kind 0 register, 1 cancel, 2 ...,
// 0xF0-0xF2 checks) logged with its caller, while the option is on.
using MessageFn = void(__fastcall*)(void* self, void* edx, int index, int kind, int arg);
MessageFn g_message = nullptr;

void __fastcall HookMessage(void* self, void* edx, int index, int kind, int arg) {
    const uintptr_t object = reinterpret_cast<uintptr_t>(self);
    if (g_on.load(std::memory_order_relaxed) && ModeOf(object) == Mode::Box) {
        int32_t needed = 0;
        uint8_t registered = 0;
        uint32_t action = 0;
        mem::Read(object + 0x1A48, needed);
        mem::Read(object + 0x18DC, registered);
        mem::Read(object + 0x10E0, action);
        LOG("coop box: %08X message kind %X player %d arg %d from RVA %06X (needed %d, registered %02X, its action %X)",
            static_cast<unsigned>(object), kind, index, arg,
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(mem::Base())),
            needed, registered, action);
    }
    g_message(self, edx, index, kind, arg);
}

bool EnsureMessageHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x683370);
    // mov eax, [0x012340A4]; mov eax, [eax+0x1042C]
    const uint8_t expected[] = { 0xA1, 0xA4, 0x40, 0x23, 0x01, 0x8B, 0x80, 0x2C, 0x04, 0x01, 0x00 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("coop box: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookMessage), reinterpret_cast<void**>(&g_message));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("coop box: message hook installed at %p", target);
    return true;
}

bool EnsureNeededHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x683070);
    // mov eax, [0x012340A4]; mov eax, [eax+0x10434]
    const uint8_t expected[] = { 0xA1, 0xA4, 0x40, 0x23, 0x01, 0x8B, 0x80, 0x34, 0x04, 0x01, 0x00 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("coop alone: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookNeeded), reinterpret_cast<void**>(&g_needed));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("coop alone: players-needed hook installed at %p", target);
    return true;
}

// ---- Lost in Nightmares' piano (uOm0201_Piano, vtable RVA 0xC0FAD8), user 2026-11-02: "only Jill can play it; with
// this option Chris should play it too". Its prompts (VA 0xB2DB30, MODLOG "PIANO"): index 0 PLAY for Jill's side
// (handler RVA 0x72D930), index 1 the partner command for Chris's side (handler RVA 0x72C9B0: the partner gets event 7,
// Chris the command gesture 0x81). Both cdecl(owner, slot). With the option Chris's press plays it himself through the
// PLAY handler: the piano puts its own player motions (pawn\om\om0201\motion\om0201_pl) in the player's bank 0xF, as
// it did for Jill (0xF02 then 0xF00, action 0x80000000), so Chris needs nothing of hers.
using PianoHandlerFn = void(__cdecl*)(uintptr_t owner, int slot);
PianoHandlerFn g_pianoCommand = nullptr, g_pianoPlay = nullptr;

// The piano's setup (VA 0xB2FD60) sets +0x199D when player 2 is not a local human (the AI Jill), and its PLAY handler
// then starts mode 4 (+0x1971) instead of 1: the AI's playing, whose end belongs to her AI (her event 7) - Chris played
// a few notes and stayed seated, the music cut (user, 2026-11-03, twice). With the option, the human's mode: +0x199D
// cleared before the handler.
void __cdecl HookPianoPlay(uintptr_t owner, int slot) {
    LOG("coop piano: PLAY pressed by slot %d (player %d)", slot, actors::Slot(slot) == actors::Player(0) ? 1 : 2);
    // Player 1 only: the AI partner sent by the command keeps her own mode.
    if (g_on.load(std::memory_order_relaxed) && owner && actors::Slot(slot) == actors::Player(0)) {
        uint8_t ai = 0;
        if (mem::Read(owner + 0x199D, ai) && ai) {
            mem::Write<uint8_t>(owner + 0x199D, 0);
            LOG("coop piano: played as a human (mode 1, not the AI's 4), slot %d", slot);
        }
    }
    g_pianoPlay(owner, slot);
}

// The command was first redirected to PLAY (Chris played on the partner's key); since the PLAY prompt is made for his
// side (g_pianoPlayPrompt) it is the game's again: "F" asks the partner, "E" plays (user, 2026-11-03).
// The "player entry" the maker reads ([[VA 0x11E8660]] VA 0x7EFA90(0) -> VA 0x7EFB10(n) +8) is the prompt's area +0x38
// (read 2026-11-03: PLAY and the command both at entry 0's): the piano's two sides, Chris's (0) and Jill's (1). With PLAY
// and the seated prompts 4 / 5 moved to Chris's side, the partner sent by the command went to hers and stood there
// without playing (user, 2026-11-03, three times): they are moved back to her side from the command until she has left
// the piano (TickPianoPartner), then to Chris's again. (+0x10, 4 on all of them, is not the side: written 1 then 4, it
// changed nothing for her.)
std::atomic<uintptr_t> g_pianoForPartner{ 0 };
std::atomic<DWORD> g_pianoPartnerSince{ 0 };
bool g_pianoPartnerSeated = false;  // render thread

uintptr_t PianoSideArea(uint32_t entry) {
    uintptr_t manager = 0, list = 0, entries = 0, at = 0, area = 0;
    uint32_t count = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDE8660)), manager) || !manager || !mem::Read(manager + 0xD4, list) ||
        !list || !mem::Read(list + 8, count) || entry >= count || !mem::Read(list + 0x14, entries) || !entries ||
        !mem::Read(entries + 4 * entry, at) || !at || !mem::Read(at + 8, area))
        return 0;
    return area;
}

void SetPianoPromptsSide(uintptr_t piano, uint32_t entry) {
    const uintptr_t area = PianoSideArea(entry);
    if (!area) return;
    for (const int index : { 0, 4, 5 }) {
        uintptr_t prompt = 0, owner = 0;
        if (mem::Read(piano + 0x1974 + 4 * index, prompt) && prompt && mem::Read(prompt + 4, owner) && owner == piano) {
            mem::Write<uint32_t>(prompt + 0x10, 4);
            mem::Write<uintptr_t>(prompt + 0x38, area);
        }
    }
}

// Moved to her side before the game's command (her event 7 is given in it).
void __cdecl HookPianoCommand(uintptr_t owner, int slot) {
    if (g_on.load(std::memory_order_relaxed) && owner) {
        SetPianoPromptsSide(owner, 1);
        const DWORD now = GetTickCount();
        g_pianoPartnerSince = now ? now : 1;
        g_pianoForPartner = owner;
        uint8_t ai = 0;
        mem::Read(owner + 0x199D, ai);
        LOG("coop piano: the partner sent to play (AI flag %u), its prompts on her side", ai);
    }
    g_pianoCommand(owner, slot);
}

bool EnsurePianoHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x72C9B0);
    // mov eax, [esp+8]; mov ecx, [0x011B27DC]
    const uint8_t expected[] = { 0x8B, 0x44, 0x24, 0x08, 0x8B, 0x0D, 0xDC, 0x27, 0x1B, 0x01 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("coop piano: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookPianoCommand), reinterpret_cast<void**>(&g_pianoCommand));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    // push ebx; push esi; mov esi, [esp+0xC]; mov eax, [esi]; mov edx, [eax+0x10]
    uint8_t* play = mem::Rva(0x72D930);
    const uint8_t playExpected[] = { 0x53, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x8B, 0x06, 0x8B, 0x50, 0x10 };
    if (!mem::IsGameCode(play, sizeof(playExpected)) || std::memcmp(play, playExpected, sizeof(playExpected)) != 0) {
        LOG("coop piano: unexpected PLAY bytes %s", mem::Hex(play, sizeof(playExpected)).c_str());
        return false;
    }
    mem::EnsureExecutable(play, sizeof(playExpected));
    st = MH_CreateHook(play, reinterpret_cast<void*>(&HookPianoPlay), reinterpret_cast<void**>(&g_pianoPlay));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(play);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("coop piano: command and PLAY hooks installed at %p / %p", target, play);
    return true;
}

// ---- Lost in Nightmares' lock (uOmf108_KeyPick, vtable RVA 0xBDF9F8, a uOmDive), user 2026-11-03: "the same problem
// as the piano: a first Examine prompt saying only Jill can pick it, then the partner action; Chris should pick it
// alone or ask Jill". Its prompts (maker VA 0xABD530, kept at +0x1978 + 4 * index; objects: +8 / +0x34 shown, +0x10
// icon, +0x24 message, +0x3C handler, +0x5C condition): 0 the partner command (handler VA 0xABCDD0: the partner's event
// 8, condition VA 0xABCDA0), 1 the Examine (VA 0xABCE00: the hint message "only Jill", then off), 2 / 3 (VA 0xABD270 /
// 0xABD200, messages 3 / 2 in progress). A human Jill's own prompt (made by VA 0xABD700, message 0x0A, handler VA
// 0xABD190 = cOmEventPL_ActPre on the presser and the lock's message 0: the presser picks it) is not made for Chris.
// Its update (VA 0xABCFD0, vtable) picks one of the two every frame (VA 0xABCEE0(index, shown)): scenario flag 0x13
// set and the lock not examined yet (flag 0xB clear) - the Examine for an AI partner, the command for a human one; once
// examined, the command (user, 2026-11-03: "they come one after the other, not side by side as at the piano": a first
// version shown the Examine from the command's condition, put back off by this update every frame, and the command only
// came after the Examine).
// A human Jill (player 1, recorded 2026-11-03) is shown prompt 3 (+0x1984, handler VA 0xABD200, message 0xC4: "Crocheter"
// on her screen) with +0x10 = 1: a prompt Chris never sees (the Examine given 1 vanished for him), the Examine and the
// command having 4.
// With the option and a man as player 1, after the game's update, the lock waiting (its action 0) and one of its two
// prompts shown: the command and Jill's prompt 3 shown, the latter with +0x10 = 4 (the Examine hidden); player 1's press
// on it goes to the pick handler (KeyPickPressed): "E" picks, "F" asks the partner, never the "only Jill" message. A woman
// as player 1 keeps the game's prompts (her own pick). Without prompt 3, the Examine takes its place with its message.
// Its text: the Examine with the uOmDive base's message 0x0A read "Sauter à l'extérieur" (user, 2026-11-03: "ça n'a pas
// de sens ici"), with 0xC4 "Ouvrir" for Chris; a prompt's text is taken when it first appears (numbers written while it
// was shown, even with the prompt hidden for a frame, changed nothing).
using KeyPickUpdateFn = void(__fastcall*)(void* lock, void* edx);
KeyPickUpdateFn g_keyPickUpdate = nullptr;
constexpr uint32_t kKeyPickText = 0xC4;

// The Examine seen first is the stage script's, not the lock's (user, 2026-11-03: "the Examine that comes first, where
// Chris says only Jill will open it"; logged 2026-11-03): a scenario area at the door (cScenarioArg_BootFSM, prompt kind
// 0x23) booting the automaton "Pick_chk", whose press gives Chris's line, sets scenario flag 0x13 and switches its area
// off; the lock's update shows its prompts only from then (flag 0x13 set, 0xB clear). Two "Pick_chk" areas in the room
// (the other one upstairs, 16 m away): with the option and a man as player 1 within 8 m of the lock's prompt area, the one
// whose prompt is on screen is switched off and the flag set, as chapter 1-1's door does (TutorialAreaTick).
bool BootsAutomaton(uintptr_t arg, const char* name);  // below

void KeyPickSkipExamine(uintptr_t lock, uintptr_t p1) {
    const auto test = reinterpret_cast<int(__stdcall*)(int)>(mem::Rva(0x37E460));
    uintptr_t area = 0, manager = 0, flags = 0;
    float at[3]{}, pos[3]{};
    if (!mem::Read(lock + 0x1974, area) || !area || !mem::SafeRead(reinterpret_cast<void*>(area + 0x20), at, sizeof(at)) ||
        !Position(p1, pos))
        return;
    const float dx = pos[0] - at[0], dy = pos[1] - at[1], dz = pos[2] - at[2];
    if (dx * dx + dy * dy + dz * dz > 800.0f * 800.0f) return;
    __try {
        if (test(0x13)) return;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), manager) || !manager) return;
    for (uintptr_t slot : { manager + 0x40 + actors::Player1PromptOffset() + 0x230, manager + 0x40 + actors::Player1PromptOffset() + 0x278 }) {
        uint8_t shown = 0;
        uintptr_t button = 0, arg = 0, scenario = 0, list = 0;
        uint32_t kind = 0, count = 0;
        if (!mem::Read(slot + 4, shown) || !shown || !mem::Read(slot + 0xC, button) || !button || !LivePrompt(button) ||
            !mem::Read(button + 0x1C, kind) || kind != 0x23 || !mem::Read(button + 4, arg) || !BootsAutomaton(arg, "Pick_chk"))
            continue;
        if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDE8660)), scenario) || !scenario ||
            !mem::Read(scenario + 0x74, count) || !mem::Read(scenario + 0x80, list) || !list)
            return;
        for (uint32_t i = 0; i < count && i < 128; ++i) {
            uintptr_t entry = 0, entryArg = 0;
            uint32_t state = 0;
            if (!mem::Read(list + 4 * i, entry) || !entry || !mem::Read(entry + 0xC, entryArg) || entryArg != arg ||
                !mem::Read(entry + 0x1C, state) || (state & 4))
                continue;
            using SwitchOffFn = void(__fastcall*)(uintptr_t area, void* edx, bool off);
            reinterpret_cast<SwitchOffFn>(mem::Rva(0x8BFD20))(entry, nullptr, true);
            using FlagSetFn = void(__fastcall*)(uintptr_t flags, void* edx, int index);
            if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE340A4)), flags) && flags)
                reinterpret_cast<FlagSetFn>(mem::Rva(0x37FCE0))(flags, nullptr, 0x13);
            LOG("coop lock: the script's Examine (area %u) switched off, the lock examined (scenario flag 0x13)", i);
            return;
        }
    }
}

void __fastcall HookKeyPickUpdate(void* self, void* edx) {
    const uintptr_t lock = reinterpret_cast<uintptr_t>(self);
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    uintptr_t command = 0, examine = 0, jill = 0;
    uint8_t commandShown = 0, examineShown = 0, female = 1;
    // Before the game's update, so that it shows the lock's prompts in the same frame.
    const bool mine = g_on.load(std::memory_order_relaxed) && p1 && mem::Read(p1 + 0x1AF8, female) && !female &&
                      mem::Read(lock + 0x10E0, action) && action == 0 && mem::Read(lock + 0x1978, command) && command &&
                      mem::Read(lock + 0x197C, examine) && examine;
    if (mine) KeyPickSkipExamine(lock, p1);
    g_keyPickUpdate(self, edx);
    if (!mine || !mem::Read(command + 0x34, commandShown) || !mem::Read(examine + 0x34, examineShown) ||
        !(commandShown || examineShown))
        return;
    uintptr_t pick = examine;
    if (mem::Read(lock + 0x1984, jill) && jill) {
        pick = jill;
        mem::Write<uint8_t>(examine + 8, 0);
        mem::Write<uint8_t>(examine + 0x34, 0);
    } else {
        // The Examine's 0 was a hint's: 4 as the command's; its button +0x20 is already the action one (1: E / X).
        mem::Write<uint32_t>(examine + 0x24, kKeyPickText);
    }
    mem::Write<uint32_t>(pick + 0x10, 4);
    for (const uintptr_t prompt : { command, pick }) {
        mem::Write<uint8_t>(prompt + 8, 1);
        mem::Write<uint8_t>(prompt + 0x34, 1);
    }
}

bool EnsureKeyPickHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x6BCFD0);
    // push ebx; push esi; mov esi, ecx; call VA 0xA82650 (the uOm update)
    const uint8_t expected[] = { 0x53, 0x56, 0x8B, 0xF1, 0xE8, 0x77, 0x56, 0xFC, 0xFF };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("coop lock: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookKeyPickUpdate), reinterpret_cast<void**>(&g_keyPickUpdate));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("coop lock: update hook installed at %p", target);
    return true;
}

// Player 1's press (a man) on Jill's prompt 3 or the Examine: the pick handler instead (he picks it).
bool KeyPickPressed(uintptr_t button, int slot) {
    uintptr_t owner = 0, handler = 0;
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    uint8_t female = 1;
    if (!g_on.load(std::memory_order_relaxed) || !button || !actors::SlotIs(slot, 0) || !p1 ||
        !mem::Read(p1 + 0x1AF8, female) || female || !mem::Read(button + 0x3C, handler) ||
        (handler != reinterpret_cast<uintptr_t>(mem::Rva(0x6BCE00)) && handler != reinterpret_cast<uintptr_t>(mem::Rva(0x6BD200))) ||
        !mem::Read(button + 4, owner) || !owner)
        return false;
    LOG("coop lock: player 1 picks the lock %08X himself (slot %d)", static_cast<unsigned>(owner), slot);
    __try {
        reinterpret_cast<void(__cdecl*)(uintptr_t, int)>(mem::Rva(0x6BD190))(owner, slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("coop lock: the pick handler faulted");
    }
    return true;
}

// ---- Two-person jumps (uOmCoopJumpBroad: uOmf111 / uOmf112), recorded 2026-10-27 on omf111 -----------------------
// The first player to press the jump's prompt (kind 0x21, icon 3, handler RVA 0x6A88B0: cOmEventPL_ActPre, then vfunc
// +0x184 kind 0, where VA 0xA82810 claims the object for him: first come, a byte of [RVA 0xDB2720]+0x80 by the
// object's handle +0xD80) is the launcher: the object's action 1 (cActWaitJumpReady) puts him in cActPlJumpReady. The
// other one presses its second prompt (handler RVA 0x6A89B0, kind 2): the object's action 3 (cActJump) plays the jump,
// both in sync (motion 0xF02). Their event motions come from bank F, set by the actions (VA 0xAA90F9): the object's
// bank 0 (omf111_pm) for a player without the female flag P+0x1AF8, its bank 1 (omf111_pf) with it - so Chris always
// launched and Sheva jumped (user: "by character").
// - "rôles inversés": player 1's press on the launcher prompt is given to player 2 (she claims the jump); for that
//   jump, uModel::SetMotionList (RVA 0x1CCDD0) into bank F gets the object's other list (pm <-> pf) and the jump's two
//   player slots are swapped (SwapJumpSlots); "le joueur 1 saute toujours" (user, 2026-11-04: "the same, but only if
//   player 1 is a man: playing Chris the animations are swapped, playing Sheva not, so that she jumps") does it only
//   for a player 1 without the female flag P+0x1AF8 (the jump's motions are chosen by it: a man launches);
// - "amener le partenaire": once a player waits as the launcher, the jumper prompt is pressed for the other one at
//   once (his cOmEventPL_ActPre places him at his post).
constexpr uintptr_t kLaunchHandler = 0x6A88B0, kJumpHandler = 0x6A89B0;  // RVA
std::atomic<bool> g_jumpSwap{ false }, g_jumpBring{ false }, g_jumpSwapMen{ false };

// The swap asked for player 1's press: "rôles inversés", or "le joueur 1 saute toujours" with a man as player 1.
bool JumpSwapWanted() {
    if (g_jumpSwap.load(std::memory_order_relaxed)) return true;
    if (!g_jumpSwapMen.load(std::memory_order_relaxed)) return false;
    const uintptr_t p1 = actors::Player(0);
    uint8_t female = 1;
    return p1 && mem::Read(p1 + 0x1AF8, female) && female == 0;
}
uintptr_t g_jump = 0;          // the jump player 1 pressed (game thread)
bool g_jumpSwapped = false;    // its roles swapped: bank F lists swapped
bool g_jumperPressed = false;  // "amener": done for this jump
DWORD g_jumpTick = 0;

using SetMotionListFn = void(__fastcall*)(void* model, void* edx, void* list, int slot);
SetMotionListFn g_setMotionList = nullptr;

void __fastcall HookSetMotionList(void* model, void* edx, void* list, int slot) {
    if (slot == 0xF && g_jumpSwapped && g_jump && list) {
        uintptr_t pm = 0, pf = 0;
        const uintptr_t l = reinterpret_cast<uintptr_t>(list);
        if (mem::Read(g_jump + 0xA88, pm) && mem::Read(g_jump + 0xA8C, pf) && pm && pf && (l == pm || l == pf)) {
            list = reinterpret_cast<void*>(l == pm ? pf : pm);
            LOG("coop jump: player %08X gets the %s list (roles swapped)", static_cast<unsigned>(reinterpret_cast<uintptr_t>(model)),
                l == pm ? "jumper's" : "launcher's");
        }
    }
    g_setMotionList(model, edx, list, slot);
}

bool EnsureSetMotionListHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x1CCDD0);
    // push ebx; mov ebx, [esp+0Ch]; push esi; mov esi, ecx; push edi
    const uint8_t expected[] = { 0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56, 0x8B, 0xF1, 0x57 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("coop jump: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st =
        MH_CreateHook(target, reinterpret_cast<void*>(&HookSetMotionList), reinterpret_cast<void**>(&g_setMotionList));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("coop jump: motion list hook installed at %p", target);
    return true;
}

bool LaunchPrompt(uintptr_t button, uintptr_t& owner) {
    uintptr_t handler = 0;
    return mem::Read(button + 4, owner) && owner && ModeOf(owner) == Mode::Both && mem::Read(button + 0x3C, handler) &&
           handler == reinterpret_cast<uintptr_t>(mem::Rva(kLaunchHandler));
}

// The jump itself: the object's cActJump (its action 3, init VA 0xAA9450) gives the launcher's cActPlJumpSecond to the
// player in game slot 0 and the jumper's cActPlJump (collision and fall handled for the flight, VA 0xAA9BA0) to slot 1,
// whatever the claim (the only fixed slots of the jump's code: push 0 at VA 0xAA9469, push 1 at VA 0xAA9476). With the
// roles swapped Chris played the jumper's motion in the launcher's action: placed and turned as the launcher, then held
// back by the obstacle (user, 2026-10-27). The two pushes are swapped while a swapped jump is pending.
mem::Patch g_jumpSlot0("coop jump: launcher slot", 0x6A9469, { 0x6A, 0x00 }, { 0x6A, 0x01 });
mem::Patch g_jumpSlot1("coop jump: jumper slot", 0x6A9476, { 0x6A, 0x01 }, { 0x6A, 0x00 });

void SwapJumpSlots(bool on) {
    if (on) {
        if (!g_jumpSlot0.IsApplied() && g_jumpSlot0.Apply() && !g_jumpSlot1.Apply()) g_jumpSlot0.Restore();
        return;
    }
    if (g_jumpSlot0.IsApplied()) g_jumpSlot0.Restore();
    if (g_jumpSlot1.IsApplied()) g_jumpSlot1.Restore();
}

void JumpTick(DWORD now) {
    if (!g_jump) return;
    const uintptr_t p1 = actors::Player(0), p2 = actors::Player(1);
    const uint32_t objectAction = Action(g_jump);
    if (!p1 || !p2 || objectAction == 0xFFFFFFFF || now - g_jumpTick > 30000 ||
        (objectAction == 0 && now - g_jumpTick > 3000 && Action(p1) != kEventAction && Action(p2) != kEventAction)) {
        g_jump = 0;  // the jump is over, cancelled or never started
        g_jumpSwapped = g_jumperPressed = false;
        SwapJumpSlots(false);
        return;
    }
    if (!g_jumpBring.load(std::memory_order_relaxed) || g_jumperPressed || objectAction != 1 || now - g_jumpTick < 300) return;
    // The launcher waits: the jumper prompt for the other player.
    const bool p1Waits = Action(p1) == kEventAction, p2Waits = Action(p2) == kEventAction;
    if (p1Waits == p2Waits) return;
    g_jumperPressed = true;
    const uintptr_t button = PromptOf(g_jump, kJumpHandler);
    if (!button) {
        LOG("coop jump: no jumper prompt on %s %08X", ClassName(g_jump), static_cast<unsigned>(g_jump));
        return;
    }
    PressForPlayer(button, p1Waits ? 1 : 0, "the jumper");
}

// The doors kicked open by two (uOmCoopDoor: uOm1010 / 102c / 102d; user, 2026-10-29: Sheva "teleported" to kick
// with Chris): the door's push action (cOmCoopDoorAction_om_act_push, start VA 0xAA55E0, doors of class group 0x1010)
// gives every player the kick (event action DTI 0x11F8FC4, cOmCoopDoorAction_pl_act_push_kick) at his post, registered
// or not; its update (VA 0xAA3FD0) only checks the registered ones (VA 0xA82CA0: the door's +0x18DC bit of the slot).
// A door done alone (Mode::Alone) gives it to the registered players only: player 1 kicks alone, player 2 stays.
// The same for the gates lifted by two (uOmCoopGate, uOm0893; user, 2026-10-29: "was Sheva teleported?" - she was):
// their opening action (cOmCoopGateAction_om_act_opening vtable slot 10, VA 0xAA66D0, the gate in edi) gives every
// player cOmCoopGateAction_pl_act_opening (DTI 0x11F9198), whose end (VA 0xAA68F0) only releases the players who got
// it: its GetPlayer call (VA 0xAA66E7) goes through GateOpenStub.
uintptr_t PartPlayer(uintptr_t manager, int slot, uintptr_t object, const char* what) {
    using GetPlayerFn = uintptr_t(__fastcall*)(uintptr_t manager, void* edx, int slot);
    using InvolvedFn = bool(__fastcall*)(uintptr_t object, void* edx, int slot);
    const uintptr_t player = reinterpret_cast<GetPlayerFn>(mem::Rva(0x3E6220))(manager, nullptr, slot);
    if (!player || !object || !g_on.load(std::memory_order_relaxed) || ModeOf(object) != Mode::Alone ||
        reinterpret_cast<InvolvedFn>(mem::Rva(0x682CA0))(object, nullptr, slot))
        return player;
    static uintptr_t logged = 0;
    if (logged != object) {
        logged = object;
        LOG("coop alone: %s %08X %s without the unregistered player in slot %d", ClassName(object), static_cast<unsigned>(object),
            what, slot);
    }
    return 0;
}

extern "C" uintptr_t __cdecl DoorKickPlayer(uintptr_t manager, int slot, uintptr_t action) {
    uintptr_t door = 0;
    mem::Read(action + 0x18, door);
    return PartPlayer(manager, slot, door, "kicked");
}

extern "C" uintptr_t __cdecl GateOpenPlayer(uintptr_t manager, int slot, uintptr_t gate) {
    return PartPlayer(manager, slot, gate, "lifted");
}

extern "C" uintptr_t __cdecl TwistRotatePlayer(uintptr_t manager, int slot, uintptr_t twist) {
    return PartPlayer(manager, slot, twist, "turned");
}

// The gate's loop (VA 0xAA66E0: mov ecx, [manager]; push esi; call GetPlayer), the gate in edi.
__declspec(naked) void GateOpenStub() {
    __asm {
        push edi                  // the gate
        push dword ptr [esp + 8]  // the slot pushed by the caller
        push ecx                  // the player manager
        call GateOpenPlayer
        add esp, 12
        ret 4
    }
}

// The rotation's loop (VA 0xAB08B3: mov ecx, [manager]; push ebx; call GetPlayer), its action in esi (the lever +0x18).
__declspec(naked) void TwistRotateStub() {
    __asm {
        push dword ptr [esi + 0x18]  // the lever
        push dword ptr [esp + 8]     // the slot pushed by the caller
        push ecx                     // the player manager
        call TwistRotatePlayer
        add esp, 12
        ret 4
    }
}

// The game's first partner action (that door, chapter 1-1; user, 2026-10-29: "a first prompt says we can kick it open
// together"): a scenario area at the door (cScenarioArg_BootFSM, prompt kind 0x23, its cFSMRelate list +0x40 / count
// +0x34, each relate's name an MtString at +4) boots the automaton "Tuto_B": Sheva's line (MessageSce), the partner
// action tutorial (EventTask 0xE), then its area off. Its prompt comes before the door's own. With the option the area
// is switched off before it shows: cScenario +0x1C bit 0x4 = off (its update VA 0xCC0810 skips it), set by VA 0xCBFD20
// (thiscall(area, bool) ret 4: on true it first takes the area down, VA 0xCBFB40), what the automaton's area off does.
// Writing +0x1C = 0 instead switched it back on again and again (user, 2026-10-29: "the prompt never goes away").
// The door itself stays locked until then: stage s116's script (VA 0x73A810) locks it (uOmCoopDoor VA 0xAA3160,
// thiscall(door, locked) ret 4: +0x1A4C and its prompts) and unlocks it once scenario flag 0x40 is set (cleared at once),
// which the examine sets (a before / after snapshot of the examine, 2026-10-29): the flag is set with the area off.
bool BootsAutomaton(uintptr_t arg, const char* name) {
    uintptr_t vtable = 0, relates = 0;
    uint32_t count = 0;
    if (!mem::Read(arg, vtable) || vtable != reinterpret_cast<uintptr_t>(mem::Rva(0xC498F8)) || !mem::Read(arg + 0x34, count) ||
        !mem::Read(arg + 0x40, relates) || !relates)
        return false;
    const size_t wanted = std::strlen(name);
    for (uint32_t i = 0; i < count && i < 8; ++i) {
        uintptr_t relate = 0, text = 0;
        uint32_t length = 0;
        char buffer[32]{};
        if (mem::Read(relates + 4 * i, relate) && relate && mem::Read(relate + 4, text) && text && mem::Read(text + 4, length) &&
            length == wanted && wanted < sizeof(buffer) && mem::SafeRead(reinterpret_cast<void*>(text + 8), buffer, wanted) &&
            std::memcmp(buffer, name, wanted) == 0)
            return true;
    }
    return false;
}

void TutorialAreaTick(DWORD now) {
    static DWORD last = 0;
    if (now - last < 250) return;
    last = now;
    uintptr_t scenario = 0, list = 0;
    uint32_t count = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDE8660)), scenario) || !scenario || !mem::Read(scenario + 0x74, count) ||
        !mem::Read(scenario + 0x80, list) || !list)
        return;
    for (uint32_t i = 0; i < count && i < 128; ++i) {
        uintptr_t area = 0, arg = 0;
        uint32_t state = 0;
        if (!mem::Read(list + 4 * i, area) || !area || !mem::Read(area + 0x1C, state) || (state & 4) || !mem::Read(area + 0xC, arg) ||
            !arg || !BootsAutomaton(arg, "Tuto_B"))
            continue;
        using SwitchOffFn = void(__fastcall*)(uintptr_t area, void* edx, bool off);
        reinterpret_cast<SwitchOffFn>(mem::Rva(0x8BFD20))(area, nullptr, true);
        uintptr_t flags = 0;
        using FlagSetFn = void(__fastcall*)(uintptr_t flags, void* edx, int index);
        if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE340A4)), flags) && flags)
            reinterpret_cast<FlagSetFn>(mem::Rva(0x37FCE0))(flags, nullptr, 0x40);
        LOG("coop alone: the partner action tutorial area %u switched off (state %X), the door unlocked (scenario flag 0x40)", i,
            state);
    }
}

// Its loop's GetPlayer call (VA 0xAA57C7: mov ecx, [manager]; push esi; call), the action in ebp.
__declspec(naked) void DoorKickStub() {
    __asm {
        push ebp                  // the door's push action
        push dword ptr [esp + 8]  // the slot pushed by the caller
        push ecx                  // the player manager
        call DoorKickPlayer
        add esp, 12
        ret 4
    }
}

mem::Patch* g_doorKick = nullptr;
mem::Patch* g_gateOpen = nullptr;
mem::Patch* g_twistRotate = nullptr;

// A GetPlayer call (mov ecx, [manager]; push esi (or `push`); call 0x7E6220) made to go through `stub`.
mem::Patch* PlayerLoopPatch(const char* name, uintptr_t rva, void* stub, uint8_t push = 0x56) {
    uint8_t* site = mem::Rva(rva);
    const std::vector<uint8_t> original = { 0x8B, 0x0D, 0xDC, 0x27, 0x1B, 0x01, push, 0xE8, 0, 0, 0, 0 };
    std::vector<uint8_t> current(original.size());
    if (!mem::SafeRead(site, current.data(), current.size())) return nullptr;
    std::vector<uint8_t> expected = original;
    const int32_t game = static_cast<int32_t>(reinterpret_cast<uintptr_t>(mem::Rva(0x3E6220)) - reinterpret_cast<uintptr_t>(site + 12));
    std::memcpy(expected.data() + 8, &game, 4);
    std::vector<uint8_t> patched = expected;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - reinterpret_cast<uintptr_t>(site + 12));
    std::memcpy(patched.data() + 8, &rel, 4);
    mem::Patch* patch = new mem::Patch(name, site, expected, patched);
    OnRestore([patch] { patch->Restore(); });  // it calls into this DLL
    return patch;
}

bool EnsureDoorKickPatch() {
    if (!g_doorKick) g_doorKick = PlayerLoopPatch("coop door: kick for the registered players only", 0x6A57C0, &DoorKickStub);
    if (!g_gateOpen) g_gateOpen = PlayerLoopPatch("coop gate: lift for the registered players only", 0x6A66E0, &GateOpenStub);
    if (!g_twistRotate)
        g_twistRotate = PlayerLoopPatch("coop twist: turned by the registered players only", 0x6B08B3, &TwistRotateStub, 0x53);
    return g_doorKick && g_gateOpen && g_twistRotate && (g_doorKick->IsApplied() || g_doorKick->Apply()) &&
           (g_gateOpen->IsApplied() || g_gateOpen->Apply()) && (g_twistRotate->IsApplied() || g_twistRotate->Apply());
}

}  // namespace

// A box made a one-pusher box needs the button held while it is pushed (user, 2026-10-28: the push started and was
// cancelled, keyboard too): the box's hold check (VA 0xA9A180, one-pusher boxes only: +0x1A48 <= 1) writes the player's
// "held" byte +0x1A5E + his pad, which his box action reads (VA 0xA9E1B8: held -> message 0xF1, the push; not held ->
// message 1, cancel). For a box whose class byte (GetDTI() +0x20) is 6 - the co-op boxes - it reads the partner button
// (mask 0x2000 / game button 0x10), else the action button (mask 0x8000 / game button 0xF): the co-op box's prompt made
// a simple one asks for the action button. The class test's jump (VA 0xA9A237, jne) always takes the action button:
// only the boxes made one-pusher boxes reach it with that class (the game's co-op boxes need two and skip the check).
mem::Patch g_boxHoldOnAction("coop box: held on the action button", 0x69A237, { 0x75, 0x26 }, { 0xEB, 0x26 });

// The elevators' players needed (see the top): cmp ebx, 2 -> cmp ebx, 1 at both counts.
mem::Patch g_elevatorOne("coop elevator: one player", 0x6C6DB9, { 0x83, 0xFB, 0x02, 0x0F, 0x82 }, { 0x83, 0xFB, 0x01, 0x0F, 0x82 });
mem::Patch g_elevatorOneGo("coop elevator: one player (go)", 0x6C6DF7, { 0x83, 0xFB, 0x02, 0x72, 0x43 }, { 0x83, 0xFB, 0x01, 0x72, 0x43 });

// The piano with coop_alone (user, 2026-11-02 / 03): its PLAY prompt (index 0) and the seated player's prompts (4 / 5:
// VA 0xB2DA50, the piano's message 1 = the melody, and the way out) are made for player entry 1 (Jill's side) by VA
// 0xB2DB30; the first try sat Chris down through the command's redirect (HookPianoCommand) without them: no melody, no
// door, no way out. Their `push 1` before the entry getter VA 0x7EFB10 -> push 0 (Chris's side): made from then on (an
// index-0 prompt already made for Jill stays hers until the piano makes it again; the command's redirect covers it).
mem::Patch g_pianoPlayPrompt("coop piano: PLAY prompt for player 1", 0x72DB5D, { 0x6A, 0x01 }, { 0x6A, 0x00 });
mem::Patch g_pianoSeatPrompt4("coop piano: seated prompt 4 for player 1", 0x72DC74, { 0x6A, 0x01 }, { 0x6A, 0x00 });
mem::Patch g_pianoSeatPrompt5("coop piano: seated prompt 5 for player 1", 0x72DCD2, { 0x6A, 0x01 }, { 0x6A, 0x00 });
// The same two made again inline by the piano's setup (VA 0xB2FD60, at the room's start: the first fix left them Jill's
// and Chris without a way out).
mem::Patch g_pianoSetupPrompt4("coop piano: setup prompt 4 for player 1", 0x72FF52, { 0x6A, 0x01 }, { 0x6A, 0x00 });
mem::Patch g_pianoSetupPrompt5("coop piano: setup prompt 5 for player 1", 0x72FFBE, { 0x6A, 0x01 }, { 0x6A, 0x00 });

// A player seated at the piano: his action object [P+0x10F8] is its uOm0201_Piano::cEventActPiano (vtable RVA
// 0xC0FD1C, recorded 2026-11-02).
bool AtPiano(uintptr_t p) {
    uintptr_t action = 0, vtable = 0;
    return p && mem::Read(p + 0x10F8, action) && action && mem::Read(action, vtable) &&
           vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xC0FD1C));
}

// Lost in Nightmares' fire where Chris burns a paper with his right hand (user, 2026-11-02 / 03): the fire is a uOmBroil
// (vtable RVA 0xBDFBA0, a uOmDive), the player then in its uOmDive::cActPlDive (vtable RVA 0xBDF7E0, recorded
// 2026-11-03), whose object is at +0xC (its getter VA 0xABE640). The same action serves the other uOmDive objects (the
// lock, the windows): the fire's only.
bool AtFire(uintptr_t p) {
    uintptr_t action = 0, vtable = 0, object = 0, objectVtable = 0;
    return p && mem::Read(p + 0x10F8, action) && action && mem::Read(action, vtable) &&
           vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xBDF7E0)) && mem::Read(action + 0xC, object) && object &&
           mem::Read(object, objectVtable) && objectVtable == reinterpret_cast<uintptr_t>(mem::Rva(0xBDFBA0));
}

// Both hands busy: at the piano or burning the paper.
bool HandsBusy(uintptr_t p) { return AtPiano(p) || AtFire(p); }

// The weapon in hand hidden while playing or burning (user: "Arme toujours visible" or not), as for the cranks
// (crank.cpp): the held item's drawn bit +5 & 0x08 off, given back after; player.cpp's always-show tick skips him meanwhile.
bool PianoHidesWeapon(int index) { return HandsBusy(actors::InGame() ? actors::Player(index) : 0); }

bool g_pianoHidden[2] = {}, g_pianoWasDrawn[2] = {};  // render thread

// The piano's failure (cActFailed, init VA 0xB308C0: the wrong note that makes the seated player stop and stand up) is
// asked of the player in game slot 1 (Jill) only: Chris seated stayed stuck after a missed or wrong press (user,
// 2026-11-03: "avec Jill une fausse note et elle arrête, avec Chris il reste bloqué"). While the player of game slot 0
// is at the piano its `push 1` before sPlayer (VA 0x7E6220) -> push 0; the game's again otherwise (Jill played).
mem::Patch g_pianoFailedSlot("coop piano: the failure for the seated slot 0", 0x7308CE, { 0x6A, 0x01 }, { 0x6A, 0x00 });

void TickPianoFailure() {
    const bool slot0 = actors::InGame() && AtPiano(actors::Slot(0));
    if (slot0 && !g_pianoFailedSlot.IsApplied()) {
        if (g_pianoFailedSlot.Apply()) LOG("coop piano: game slot 0 seated, its failure made his");
    } else if (!slot0 && g_pianoFailedSlot.IsApplied() && !AtPiano(actors::InGame() ? actors::Slot(1) : 0)) {
        g_pianoFailedSlot.Restore();
    }
}

// The partner sent by the command: the piano's prompts on her side while she goes there (60 s at most) and plays; back on
// Chris's side once she has stood up.
bool AtPiano(uintptr_t p);  // below

void TickPianoPartner() {
    const uintptr_t piano = g_pianoForPartner.load();
    if (!piano) return;
    uintptr_t vtable = 0;
    if (!actors::InGame() || !mem::Read(piano, vtable) || vtable != reinterpret_cast<uintptr_t>(mem::Rva(0xC0FAD8))) {
        g_pianoForPartner = 0;
        g_pianoPartnerSeated = false;
        return;
    }
    const bool seated = AtPiano(actors::Player(1));
    if (seated) g_pianoPartnerSeated = true;
    if (seated || (!g_pianoPartnerSeated && GetTickCount() - g_pianoPartnerSince.load() < 60000)) return;
    SetPianoPromptsSide(piano, 0);
    g_pianoForPartner = 0;
    g_pianoPartnerSeated = false;
    LOG("coop piano: the partner done, its prompts on player 1's side");
}

void TickPianoWeapon() {
    TickPianoFailure();
    TickPianoPartner();
    for (int i = 0; i < 2; ++i) {
        const uintptr_t p = actors::InGame() ? actors::Player(i) : 0;
        uintptr_t weapon = 0;
        uint8_t flags = 0;
        if (!p || !mem::Read(p + 0x2634, weapon) || !weapon || !mem::Read(weapon + 5, flags)) {
            g_pianoHidden[i] = false;
            continue;
        }
        if (HandsBusy(p)) {
            if (!g_pianoHidden[i]) g_pianoWasDrawn[i] = (flags & 0x08) != 0;
            g_pianoHidden[i] = true;
            if (flags & 0x08) mem::Write<uint8_t>(weapon + 5, flags & ~0x08);
        } else if (g_pianoHidden[i]) {
            g_pianoHidden[i] = false;
            if (g_pianoWasDrawn[i] && !(flags & 0x08)) mem::Write<uint8_t>(weapon + 5, flags | 0x08);
        }
    }
}

// Seated, Chris overlapped the piano (user: bulkier than Jill): a man (+0x1AF8 == 0) at the piano moved back along his
// facing by kPianoBack (user, 2026-11-03: "les dix centimètres sont parfaits", no option); again whenever the game puts
// him somewhere else (his place differs from what the trainer wrote), so a seat held every frame or once is moved the same.
constexpr float kPianoBack = 10.0f;  // cm
float g_pianoWritten[2][3] = {};          // render thread: where the trainer put him last
bool g_pianoMoved[2] = {};

void TickPianoSeat();  // below

// "Piano : la porte secrète reste ouverte" (user, 2026-11-03: the pianist must play on until the other one takes the
// emblem behind the secret door, and the partner may not pick it up - "no pickup for player 2", wanted - so alone the
// door shut as soon as Chris stopped; "the same as the gate: a countdown, the door open until its end, then shut; once
// the emblem is taken it stays open, as in the game"). The door uOm0202_Door (vtable RVA 0xC0FDD0, the piano's +0x19C8),
// recorded 2026-11-03 (a snapshot a second): its update VA 0xB30250 is driven by its request +0x1970 (1 while the piano
// plays: it slides down from y 400 to 141 at ~7.5 cm/s, sub-state +0x1971 1, then 2 at the bottom; 2 = back up, sub-state
// 4 at the top: shut in under a second after a wrong note; 3 = held open once scenario flag 0x96 is set, the emblem).
// Going back up, its mode 2 asks VA 0xB2CFB0 (thiscall(door) -> bool, its only caller: [+0x197C] +0x10 bit 1 set and
// bit 0 clear, the way being clear) whether it may move: answered false for the slider's seconds from its first ask (a
// countdown on screen), the request kept until then; the game's answer after. Only when the door was fully open (its y
// +0x34 at the bottom, 141; user, 2026-11-03: "a failure kept it open and it went on from where it was: no countdown
// unless it is fully open, else it shuts as in the game").
constexpr float kDoorOpenY = 141.0f;  // recorded 2026-11-03: shut at 400, fully open at 141
using DoorMayCloseFn = bool(__fastcall*)(void* door, void* edx);
DoorMayCloseFn g_doorMayClose = nullptr;
std::atomic<bool> g_doorHoldOn{ true };
std::atomic<float> g_doorHoldSeconds{ 10.0f };
std::atomic<uintptr_t> g_doorHeld{ 0 };
std::atomic<DWORD> g_doorHoldSince{ 0 };  // its first ask to shut (0 none)

bool __fastcall HookDoorMayClose(void* door, void* edx) {
    const bool may = g_doorMayClose(door, edx);
    const float seconds = g_doorHoldSeconds.load(std::memory_order_relaxed);
    if (!may || !g_doorHoldOn.load(std::memory_order_relaxed) || seconds <= 0.0f) return may;
    const DWORD now = GetTickCount();
    if (g_doorHeld.load() != reinterpret_cast<uintptr_t>(door) || !g_doorHoldSince.load()) {
        float y = 0.0f;
        if (!mem::Read(reinterpret_cast<uintptr_t>(door) + 0x34, y) || y > kDoorOpenY + 1.0f) return may;  // not fully open
        g_doorHeld = reinterpret_cast<uintptr_t>(door);
        g_doorHoldSince = now ? now : 1;
        LOG("coop piano: the secret door held open %.0f s", seconds);
    }
    return static_cast<int32_t>(now - g_doorHoldSince.load()) >= static_cast<int32_t>(seconds * 1000.0f);
}

bool EnsureDoorHoldHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x72CFB0);
    // push esi; push edi; mov edi, ecx; xor esi, esi; mov eax, 1
    const uint8_t expected[] = { 0x56, 0x57, 0x8B, 0xF9, 0x33, 0xF6, 0xB8, 0x01, 0x00, 0x00, 0x00 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("coop piano door: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookDoorMayClose), reinterpret_cast<void**>(&g_doorMayClose));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("coop piano door: hold hook installed at %p", target);
    return true;
}

// The held door's countdown let go once it is no longer asking to go up (shut, opened again by the piano, or held open
// by the emblem's flag), so a new stop gets a full one.
void TickDoorHold() {
    const uintptr_t door = g_doorHeld.load();
    uint8_t mode = 0;
    if (!door || !g_doorHoldSince.load()) return;
    if (!actors::InGame() || !mem::Read(door + 0x1970, mode) || mode != 2) {
        g_doorHoldSince = 0;
        g_doorHeld = 0;
    }
}

void DrawDoorCountdown() {
    const DWORD since = g_doorHoldSince.load(std::memory_order_relaxed);
    if (!since || !actors::InGame()) return;
    const float left = g_doorHoldSeconds.load(std::memory_order_relaxed) - static_cast<float>(GetTickCount() - since) / 1000.0f;
    if (left <= 0.0f) return;
    char text[48];
    std::snprintf(text, sizeof(text), i18n::Tr("Porte secrète : %d s"), static_cast<int>(std::ceil(left)));
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float size = screen.y * 0.045f;
    const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 at((screen.x - extent.x) * 0.5f, screen.y * 0.08f);
    const ImU32 colour = left <= 5.0f ? IM_COL32(255, 90, 70, 255) : IM_COL32(255, 235, 200, 255);  // red at the end
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(ImGui::GetFont(), size, at, colour, text);
}

// "Porte au cadenas : l'enfoncer en deux coups de pied" (user, 2026-11-03: Lost in Nightmares' door whose padlock Jill
// shoots from the other side - Chris is always on the wrong side of it; "each kick takes 50 % of the padlock's health,
// so it falls at the second, the door opening with Chris's kick"). The door (a uOmDoor, here uOm0210) is locked while
// its +0x19E0 bit 3 is set; its update (VA 0xABFD30) unlocks it once its padlock (+0x19F0, a uOm0014SF: health int16
// +0x1364, max +0x1366, 400) has no health left (bit 3 off, +0x18DC |= 0x40, +0x19F0 / +0x19EC = 0). A press sends the
// door its message VA 0xABEF60 (thiscall(door, slot, kind, param) ret 0xC); its actions are (DTI, variant) pairs of the
// classic doors' table at VA 0xFE1B30 (read 2026-11-03): 1-2 Wait, 3-5 OpenClassic, 6-8 OpenKick, 9 OpenEM, 10
// OpenAuto, 11 Close, 0xC-0xE Miss, 0xF Bang, 0x10 Break. Kind 0 = open (3 / 4), kind 1 = kicked open (6 / 7), kind 2 =
// the miss given to a locked door: 0xD locked (the door shakes; its init VA 0xAC03E0 gives the padlock action 1, its
// rattle, or 2, its fall, at no health), else 0xC. With the option, before the game's miss: the padlock loses half its
// health; at none left, its fall (action 2, as the miss gives it), the door unlocked as its update does and the message
// made kind 1: the door kicked open (a first try kept kind 2: the padlock fell, the door did not open - user, 2026-11-03).
using DoorMessageFn = void(__fastcall*)(void* door, void* edx, int slot, int kind, int param);
DoorMessageFn g_doorMessage = nullptr;
std::atomic<bool> g_padlockKick{ true };

void __fastcall HookDoorMessage(void* self, void* edx, int slot, int kind, int param) {
    const uintptr_t door = reinterpret_cast<uintptr_t>(self);
    uint32_t flags = 0;
    uintptr_t padlock = 0, vtable = 0;
    int16_t health = 0, most = 0;
    if (g_padlockKick.load(std::memory_order_relaxed) && kind == 2 && param != 0 && param != 3 && mem::Read(door + 0x19E0, flags) &&
        (flags & 8) && mem::Read(door + 0x19F0, padlock) && padlock && mem::Read(padlock, vtable) &&
        vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xBF4DD0)) && mem::Read(padlock + 0x1364, health) && health > 0 &&
        mem::Read(padlock + 0x1366, most) && most > 0) {
        health = static_cast<int16_t>(std::max(0, health - (most + 1) / 2));
        mem::Write<int16_t>(padlock + 0x1364, health);
        if (health > 0) {
            LOG("coop padlock: kicked, %d / %d health left", health, most);
        } else {
            __try {
                using RequestFn = void(__fastcall*)(uintptr_t unit, void* edx, int action, int sub, uintptr_t arg, int a, int b);
                uintptr_t table = 0, request = 0;
                if (mem::Read(padlock, table) && mem::Read(table + 0xDC, request) && request)
                    reinterpret_cast<RequestFn>(request)(padlock, nullptr, 2, 0, door, -1, 0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                LOG("coop padlock: its fall faulted");
            }
            uint8_t state = 0;
            mem::Read(door + 0x18DC, state);
            mem::Write<uint8_t>(door + 0x18DC, state | 0x40);
            mem::Write<uintptr_t>(door + 0x19F0, 0);
            mem::Write<uint32_t>(door + 0x19EC, 0);
            mem::Write<uint32_t>(door + 0x19E0, flags & ~8u);
            LOG("coop padlock: kicked off, the door %08X unlocked and kicked open (slot %d, param %d)",
                static_cast<unsigned>(door), slot, param);
            kind = 1;
        }
    }
    g_doorMessage(self, edx, slot, kind, param);
}

// The trap's lever behind that door (uOm08bc, vtable RVA 0xC12280; user, 2026-11-03: "kicking the door lets me pull the
// lever behind early, then when Jill falls into the trap I cannot save her, the lever is already pulled; it should reset
// by itself while Jill is not in the trap"). Its actions (logged 2026-11-03): 0 ready, 3 pulled (its motion), 4 done
// (VA 0xB3AA96 at the motion's end: +0x1970 = 1, +0x1984 = 1, then action 4); a load gives 0 or 4. With the option, its
// done (action 4) asked before the trap puts it back 0.6 s later: +0x1970 / +0x1984 = 0 and action 0, as a fresh load.
// The trap (recorded 2026-11-03 with the scenario flag log): scenario flags 201 and 208 set as Jill falls in, 202 at
// the lever's rescue; the early pulls set none. So "before the trap" = flag 201 clear: from then on the lever is the
// game's (a first version looked at Jill's action: passing the kicked door read as the trap, the trap itself did not).
std::atomic<uintptr_t> g_trapLever{ 0 };
std::atomic<DWORD> g_trapLeverAt{ 0 };

void TrapLeverRequested(uintptr_t unit, int action) {
    uintptr_t vtable = 0;
    if (action != 4 || !g_padlockKick.load(std::memory_order_relaxed) || !unit || !mem::Read(unit, vtable) ||
        vtable != reinterpret_cast<uintptr_t>(mem::Rva(0xC12280)))
        return;
    const DWORD now = GetTickCount();
    g_trapLeverAt = now ? now : 1;
    g_trapLever = unit;
}

bool JillTrapped() {
    __try {
        return reinterpret_cast<int(__stdcall*)(int)>(mem::Rva(0x37E460))(201) != 0;  // the flag test (wesker_boss.cpp)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;  // unknown: the lever left to the game
    }
}

void TickTrapLever() {
    const uintptr_t lever = g_trapLever.load();
    if (!lever || GetTickCount() - g_trapLeverAt.load() < 600) return;
    g_trapLever = 0;
    uintptr_t vtable = 0, table = 0, request = 0;
    uint32_t action = 0;
    if (!actors::InGame() || !mem::Read(lever, vtable) || vtable != reinterpret_cast<uintptr_t>(mem::Rva(0xC12280)) ||
        !mem::Read(lever + 0x10E0, action) || action != 4)
        return;
    if (JillTrapped()) {
        LOG("coop trap lever: pulled once the trap has happened (scenario flag 201): the game's");
        return;
    }
    mem::Write<uint8_t>(lever + 0x1970, 0);
    mem::Write<uint8_t>(lever + 0x1984, 0);
    __try {
        using RequestFn = void(__fastcall*)(uintptr_t unit, void* edx, int action, int sub, int arg, int a, int b);
        if (mem::Read(lever, table) && mem::Read(table + 0xDC, request) && request)
            reinterpret_cast<RequestFn>(request)(lever, nullptr, 0, 1, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("coop trap lever: its reset faulted");
        return;
    }
    LOG("coop trap lever: pulled before the trap, set ready again");
}

bool EnsureDoorMessageHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x6BEF60);
    // push ebx; mov ebx, [esp+8]; push esi; push edi; mov esi, ecx
    const uint8_t expected[] = { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x57, 0x8B, 0xF1 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("coop padlock: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookDoorMessage), reinterpret_cast<void**>(&g_doorMessage));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("coop padlock: door message hook installed at %p", target);
    return true;
}

void TickPianoSeat() {
    for (int i = 0; i < 2; ++i) {
        const uintptr_t p = actors::InGame() ? actors::Player(i) : 0;
        uint8_t female = 1;
        float pos[3]{}, q[4]{};
        if (!AtPiano(p) || !mem::Read(p + 0x1AF8, female) || female ||
            !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos)) ||
            !mem::SafeRead(reinterpret_cast<void*>(p + 0x40), q, sizeof(q))) {
            g_pianoMoved[i] = false;
            continue;
        }
        if (g_pianoMoved[i] && pos[0] == g_pianoWritten[i][0] && pos[1] == g_pianoWritten[i][1] && pos[2] == g_pianoWritten[i][2])
            continue;  // still where the trainer put him
        const float yaw = 2.0f * std::atan2(q[1], q[3]), back = kPianoBack;
        const float at[3] = { pos[0] - std::sin(yaw) * back, pos[1], pos[2] - std::cos(yaw) * back };
        mem::SafeWrite(reinterpret_cast<void*>(p + 0x30), at, sizeof(at));
        std::memcpy(g_pianoWritten[i], at, sizeof(at));
        if (!g_pianoMoved[i]) LOG("coop piano: player %d seated, moved back %.0f cm", i + 1, back);
        g_pianoMoved[i] = true;
    }
}
// The levers turned by two's players needed (see the top), the same way.
mem::Patch g_twistOne("coop twist: one player ready", 0x6AFB97, { 0x83, 0xFB, 0x02, 0x0F, 0x82 }, { 0x83, 0xFB, 0x01, 0x0F, 0x82 });
mem::Patch g_twistOneGo("coop twist: one player turns", 0x6AFBCD, { 0x83, 0xFB, 0x02, 0x72, 0x2D }, { 0x83, 0xFB, 0x01, 0x72, 0x2D });
// The doors opened by two buttons, the same way (the count in ebp).
mem::Patch g_cardOne("coop card door: one player opens", 0x6A0F7E, { 0x83, 0xFD, 0x02, 0x72, 0x2D }, { 0x83, 0xFD, 0x01, 0x72, 0x2D });
mem::Patch g_cardOneReady("coop card door: one player ready", 0x6A0FD7, { 0x83, 0xFD, 0x02, 0x72, 0x43 }, { 0x83, 0xFD, 0x01, 0x72, 0x43 });

// Boxes made one-player boxes (their pushers needed +0x1A48 and the value it had), put back when the option goes off.
struct SoloBox {
    uintptr_t box = 0, vtable = 0;
    int32_t needed = 0;
};
SoloBox g_boxes[8];

void MakeOnePlayerBox(uintptr_t box) {
    int32_t needed = 0;
    uintptr_t vtable = 0;
    if (!mem::Read(box + 0x1A48, needed) || needed < 2 || !mem::Read(box, vtable)) return;
    SoloBox* slot = &g_boxes[0];
    for (SoloBox& b : g_boxes) {
        if (b.box == box) return;  // already one (another value written since)
        if (!b.box) {
            slot = &b;
            break;
        }
    }
    *slot = { box, vtable, needed };
    mem::Write<int32_t>(box + 0x1A48, 1);
    LOG("coop alone: %s %08X pushed alone (pushers needed %d -> 1)", ClassName(box), static_cast<unsigned>(box), needed);
}

void RestoreBoxes() {
    for (SoloBox& b : g_boxes) {
        uintptr_t vtable = 0;
        int32_t needed = 0;
        if (b.box && mem::Read(b.box, vtable) && vtable == b.vtable && mem::Read(b.box + 0x1A48, needed) && needed == 1)
            mem::Write<int32_t>(b.box + 0x1A48, b.needed);
        b = {};
    }
}

// The push (box action 1, om_act_push VA 0xA9CE10) gives a co-op box (class byte 6) pushed by one player a speed of 0
// (+0x1A28, its motion speed +0x3A4: it waits for the second pusher, who makes it 1.5): the box never moved, and the
// pusher's push loop (VA 0xA9EAB0, its motion at the box's speed, 0.6 at 0) waits for the box's motion end (+0x1A5D
// cleared at +0x388 & 4) - Chris strained in place (user, 2026-10-28). A box made one-pusher box gets the speed the game
// gives a one-pusher box: 1, 0.8 for a pusher with the female weapon animations (P+0x1AF8).
void BoxSpeedTick() {
    for (const SoloBox& b : g_boxes) {
        uintptr_t vtable = 0;
        int32_t needed = 0, action = 0;
        float speed = 1.0f;
        uint8_t pushers = 0;
        if (!b.box || !mem::Read(b.box, vtable) || vtable != b.vtable || !mem::Read(b.box + 0x1A48, needed) || needed != 1 ||
            !mem::Read(b.box + 0x10E0, action) || action != 1 || !mem::Read(b.box + 0x1A28, speed) || speed != 0.0f ||
            !mem::Read(b.box + 0x18DC, pushers))
            continue;
        speed = 1.0f;
        for (int i = 0; i < 4; ++i) {
            uint8_t female = 0;
            const uintptr_t pusher = (pushers >> i) & 1 ? actors::Slot(i) : 0;
            if (!pusher) continue;
            if (mem::Read(pusher + 0x1AF8, female) && female == 1) speed = 0.8f;
            break;
        }
        mem::Write<float>(b.box + 0x1A28, speed);
        mem::Write<float>(b.box + 0x3A4, speed);
        LOG("coop alone: box %08X pushed alone at speed %.1f", static_cast<unsigned>(b.box), speed);
    }
}

// action_log.cpp's OnPressed hook, before the game's press: true when the press is taken over. "Rôles inversés":
// player 1's press on a two-person jump's launcher prompt is player 2's (she must be within 3 m without "amener").
// The AI partner leaves the boxes to player 1 (user, 2026-10-31: "I was pushing the box when a licker's tongue grabbed
// me and the box went on by itself": Sheva, invisible, pressed its prompt 4 s later and pushed it alone, a one-pusher box
// by then - her register from the prompt's handler RVA 0x69AFA4, then her pushes 0xF1 every 1.25 s at her 0.8): her press
// on a co-op box's prompt is dropped. A player 2 played by a person (+0x2DA8 0) keeps his.
bool CoopSoloRedirect(uintptr_t button, int slot) {
    if (KeyPickPressed(button, slot)) return true;
    uintptr_t owner = 0;
    if (g_on.load(std::memory_order_relaxed) && button && mem::Read(button + 4, owner) && owner && ModeOf(owner) == Mode::Box) {
        if (actors::SlotIs(slot, 0)) {
            MakeOnePlayerBox(owner);
        } else {
            const uintptr_t unit = actors::Slot(slot);
            uint32_t controller = 0;
            if (unit && mem::Read(unit + 0x2DA8, controller) && controller == 1) {
                static uintptr_t told = 0;
                if (told != owner) LOG("coop alone: the AI partner's press on box %08X dropped", static_cast<unsigned>(owner));
                told = owner;
                return true;
            }
        }
    }
    owner = 0;
    if (!button || !actors::SlotIs(slot, 0) || !LaunchPrompt(button, owner) || !JumpSwapWanted()) return false;
    const uintptr_t p2 = actors::Player(1);
    if (!p2) return false;
    if (!g_jumpBring.load(std::memory_order_relaxed) && Distance(p2, owner) > 300.0f) {
        Toast("Saut à deux (rôles inversés) : Sheva doit être près du saut", false);
        return true;
    }
    g_jump = owner;
    g_jumpTick = GetTickCount();
    g_jumpSwapped = true;
    g_jumperPressed = false;
    SwapJumpSlots(true);
    if (!PressForPlayer(button, 1, "the launcher")) {
        g_jumpSwapped = false;
        SwapJumpSlots(false);
    }
    return true;
}

// action_log.cpp's OnPressed hook, after the game's press: player 1's press on a co-op object's prompt.
void CoopSoloPressed(uintptr_t button, int slot) {
    if (!button || !actors::SlotIs(slot, 0)) return;
    uintptr_t jump = 0;
    if (g_jumpBring.load(std::memory_order_relaxed) && LaunchPrompt(button, jump)) {  // he is the launcher
        g_jump = jump;
        g_jumpTick = GetTickCount();
        g_jumpSwapped = g_jumperPressed = false;
        SwapJumpSlots(false);
        return;
    }
    if (!g_on.load(std::memory_order_relaxed)) return;
    uintptr_t owner = 0, handler = 0;
    if (!mem::Read(button + 4, owner) || !owner) return;
    const Mode mode = ModeOf(owner);
    if (mode != Mode::RemotePress && mode != Mode::RemoteWait) return;
    mem::Read(button + 0x3C, handler);
    Reset();
    g_stage = Stage::Pressed;
    g_button = button;
    g_owner = owner;
    g_mode = mode;
    g_handler = handler;
    g_pressTick = GetTickCount();
    LOG("coop alone: player 1 pressed prompt %08X of %s %08X (handler RVA %06X)", static_cast<unsigned>(button),
        ClassName(owner), static_cast<unsigned>(owner),
        handler ? static_cast<unsigned>(handler - reinterpret_cast<uintptr_t>(mem::Base())) : 0u);
}

// "Actions à deux faites seul : touche d'action" (user, 2026-10-30, once "Franchissements sur A" stopped moving them:
// "make the actions done alone solo prompts - X or A on the pad, E on the keyboard - and unchecked their own"): the
// shown prompts of the objects done alone (every family but the broad jumps) asking for the partner button (icon 4:
// F / B) get the game's action icon 1 (E / X; A through "Toutes les actions sur A", which swaps icon 1 on a pad). The
// icon also sets the button the game accepts (table VA 0xF58E50). Their own icon back at once when the option goes off
// or the trainer unloads (user, 2026-10-30: unchecked, E / X stayed - given back only when shown again, and forgotten by
// a reloaded trainer). Written into live prompts only (quick_knife.cpp's LiveButton: a shown slot may name a prompt
// just destroyed).
std::atomic<bool> g_actionIcon{ false };
std::vector<std::pair<uintptr_t, uint32_t>> g_actionIconOriginals;  // game thread
constexpr uint32_t kPartnerIcon = 4, kActionIcon = 1, kActionIconOnA = 3;

bool LivePrompt(uintptr_t button) {
    uintptr_t system = 0, array = 0;
    uint32_t count = 0;
    if (!button || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), system) || !system ||
        !mem::Read(system + 0x30, count) || !mem::Read(system + 0x3C, array) || !array || count > 4000)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t b = 0;
        if (mem::Read(array + 4 * i, b) && b == button) return true;
    }
    return false;
}

// Every prompt given the action icon back to its own.
void RestoreActionIcons() {
    for (const auto& [button, original] : g_actionIconOriginals) {
        uint32_t icon = 0;
        if (LivePrompt(button) && mem::Read(button + 0x20, icon) && (icon == kActionIcon || icon == kActionIconOnA))
            mem::Write<uint32_t>(button + 0x20, original);
    }
    g_actionIconOriginals.clear();
}

void TickActionIcons() {
    const bool on = g_actionIcon.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed);
    if (!on) {
        if (!g_actionIconOriginals.empty()) RestoreActionIcons();
        return;
    }
    if (!actors::InGame()) {
        g_actionIconOriginals.clear();
        return;
    }
    uintptr_t manager = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), manager) || !manager) return;
    for (uintptr_t slot : { manager + 0x40 + actors::Player1PromptOffset() + 0x230, manager + 0x40 + actors::Player1PromptOffset() + 0x278 }) {
        uint8_t shown = 0;
        uintptr_t button = 0, owner = 0;
        uint32_t icon = 0;
        if (!mem::Read(slot + 4, shown) || !shown || !mem::Read(slot + 0xC, button) || !button || !mem::Read(button + 0x20, icon) ||
            !LivePrompt(button))
            continue;
        mem::Read(button + 4, owner);
        auto known = std::find_if(g_actionIconOriginals.begin(), g_actionIconOriginals.end(),
                                  [button](const auto& e) { return e.first == button; });
        if (on && CoopSoloActionPrompt(owner)) {
            if (icon != kPartnerIcon) continue;
            if (known == g_actionIconOriginals.end()) {
                if (g_actionIconOriginals.size() > 64) g_actionIconOriginals.erase(g_actionIconOriginals.begin());
                g_actionIconOriginals.push_back({ button, icon });
            }
            mem::Write<uint32_t>(button + 0x20, kActionIcon);
        } else if (known != g_actionIconOriginals.end()) {
            if (icon == kActionIcon || icon == kActionIconOnA) mem::Write<uint32_t>(button + 0x20, known->second);
            g_actionIconOriginals.erase(known);
        }
    }
}

// quick_knife.cpp's traversal prompts leave these out (user, 2026-10-30: "A on the pad while all on A is unchecked":
// kind 0x21 with the action icon, "Franchissements sur A" moved them on).
bool CoopSoloActionPrompt(uintptr_t owner) {
    if (!owner || !g_actionIcon.load(std::memory_order_relaxed) || !g_on.load(std::memory_order_relaxed)) return false;
    const Mode mode = ModeOf(owner);
    // The two-person jumps too once player 1 does them alone (user, 2026-10-31: "the added jump's prompt shows F").
    const bool jumpsAlone = g_jumpSwap.load(std::memory_order_relaxed) || g_jumpBring.load(std::memory_order_relaxed) ||
                            g_jumpSwapMen.load(std::memory_order_relaxed);
    return (mode != Mode::None && (mode != Mode::Both || jumpsAlone)) || LeverPromptAlone(owner);
}

// quick_knife.cpp's pad query hook (game thread, every frame).
void CoopSoloTick() {
    TickActionIcons();
    ObstacleClimbTick();
    InventoryDiscardTick();
    FinalQteTick();
    const DWORD now = GetTickCount();
    if (!actors::InGame()) {
        g_jump = 0;
        g_jumpSwapped = g_jumperPressed = false;
        SwapJumpSlots(false);
    }
    JumpTick(now);
    if (g_on.load(std::memory_order_relaxed) && actors::InGame()) {
        BoxSpeedTick();
        TutorialAreaTick(now);
    }
    if (g_stage == Stage::Idle || !g_on.load(std::memory_order_relaxed)) return;
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0, p2 = p1 ? actors::Player(1) : 0;
    if (!p1 || !p2 || now - g_pressTick > 20000) {
        Reset();
        return;
    }
    const bool p1Waits = Action(p1) == kEventAction;
    static DWORD lastWhy = 0;  // why nothing was pressed yet, once a second (user, 2026-10-28: an elevator never was)
    if (g_stage == Stage::Pressed && now - lastWhy >= 1000) {
        lastWhy = now;
        LOG("coop alone: waiting on %s %08X (player 1 action %X, player 2 action %X%s, cancel shown %d)", ClassName(g_owner),
            static_cast<unsigned>(g_owner), Action(p1), Action(p2), Registered(g_owner, p2) ? " registered" : "",
            g_mode == Mode::RemoteWait ? CancelShown(g_owner) : -1);
    }
    switch (g_stage) {
    case Stage::Pressed:
        if (!p1Waits) {
            g_waitSince = 0;
            if (now - g_pressTick > 3000) Reset();  // he walked off, or it needed nobody else
            return;
        }
        if (!g_waitSince) g_waitSince = now;
        if (now - g_waitSince < 400) return;
        if (g_mode == Mode::RemoteWait && !CancelShown(g_owner)) return;  // an elevator: waiting, not riding
        if (Registered(g_owner, p2)) {  // she is already doing hers
            LOG("coop alone: player 2 already registered there, nothing pressed");
            g_stage = Stage::Done;
            return;
        }
        // In another event action (user, 2026-10-28: an elevator never went; she was going through a door uOm101b
        // nearby, and that was taken for hers): waited for, then pressed.
        if (Action(p2) == kEventAction) return;
        PressForPlayer2(g_button, "the same");
        g_stage = Stage::Tried;
        return;
    case Stage::Tried:
        if (Action(p2) == kEventAction || !p1Waits) {
            LOG("coop alone: done (player 1 action %X, player 2 action %X)", Action(p1), Action(p2));
            g_stage = Stage::Done;
            return;
        }
        if (now - g_triedTick < 500) return;
        if (!g_fallbackTried && g_mode == Mode::RemotePress) {
            g_fallbackTried = true;
            if (const uintptr_t other = OtherObjectPrompt()) {
                PressForPlayer2(other, "another object's");
                return;
            }
            LOG("coop alone: the press changed nothing and no other object's prompt was found");
        }
        g_stage = Stage::Done;
        return;
    case Stage::Done:
        if (!p1Waits) Reset();
        return;
    default:
        return;
    }
}

void RegisterLevers();  // levers.cpp: the levers pulled by two, alone against the clock

void RegisterCoopSolo() {
    OnFrame(TickPianoWeapon);
    OnFrame(TickTrapLever);
    OnFrame(TickPianoSeat);
    OnFrame(TickDoorHold);
    OnHud(DrawDoorCountdown);
    Feature& doorHold = Add({ .id = "partner.piano_door_hold", .name = "Piano : la porte secrète reste ouverte",
                              .help = "Lost in Nightmares : quand la musique du piano s'arrête (fausse note ou en se "
                                      "levant) alors que la porte secrète est complètement ouverte, elle reste ouverte le "
                                      "temps du curseur, avec un compte à rebours à l'écran, puis se referme. Pas encore "
                                      "complètement ouverte, elle se referme comme dans le jeu. Une fois l'emblème "
                                      "ramassé, elle reste ouverte comme dans le jeu.",
                              .category = "Joueur", .kind = Kind::Toggle,
                              .available = [] { return game::IsSupported(); },
                              .value = 10.0f, .valueMin = 5.0f, .valueMax = 60.0f, .valueFormat = "%.0f s",
                              .defaultOn = true });
    {
        Feature* const dh = &doorHold;
        doorHold.set = [dh](bool on) {
            if (on && !EnsureDoorHoldHook()) return false;
            g_doorHoldSeconds = dh->value;
            g_doorHoldOn = on;
            if (!on) {
                g_doorHoldSince = 0;
                g_doorHeld = 0;
            }
            return true;
        };
        doorHold.tick = [dh] { g_doorHoldSeconds.store(dh->value, std::memory_order_relaxed); };
    }
    Add({ .id = "partner.padlock_kick", .name = "Porte au cadenas : l'enfoncer en deux coups de pied",
          .help = "Lost in Nightmares : la porte dont Jill doit briser le cadenas d'un tir, de l'autre côté. Chaque coup de "
                  "pied dans la porte retire la moitié de la vie du cadenas : au premier la porte tremble comme dans le "
                  "jeu, au deuxième le cadenas tombe et la porte s'ouvre d'un coup de pied. Le levier du piège, derrière, "
                  "se réarme tant que Jill n'est pas dans le piège.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureDoorMessageHook() || !EnsureEnemyActionHook())) return false;
              g_padlockKick = on;
              return true;
          },
          .available = [] { return game::IsSupported(); }, .defaultOn = true });
    OnRestore(RestoreBoxes);  // unload: the boxes' own pushers needed back
    OnRestore(RestoreActionIcons);  // unload: the prompts' own icons back
    Add({ .id = "partner.coop_alone", .name = "Actions de partenaire sans attendre",
          .help = "Plus besoin d'attendre Sheva pour les actions à deux : les portes de fin de zone, les ascenseurs et les "
                  "actions faites ensemble (grilles, portes à défoncer, caisses, leviers à tourner...), Chris les fait seul et Sheva ne bouge "
                  "pas (sans le tutoriel de la première porte). Les sauts à deux (coup de pouce, échelle) restent à deux.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsurePromptPressHook() || !EnsurePadQueryHooks() || !EnsureNeededHook() || !EnsureMessageHook()))
                  return false;
              // The piano played by Chris (user, 2026-11-02 / 03; MODLOG "PIANO"): the command redirected to PLAY and
              // its PLAY / seated prompts made for his side (the first try had no seated prompt: stuck).
              if (on && !EnsurePianoHook()) LOG("coop piano: command hook not installed");
              if (on && !EnsureKeyPickHook()) LOG("coop lock: hook not installed");
              if (on && !g_pianoPlayPrompt.IsApplied())
                  mem::ApplyAll({ &g_pianoPlayPrompt, &g_pianoSeatPrompt4, &g_pianoSeatPrompt5, &g_pianoSetupPrompt4,
                                  &g_pianoSetupPrompt5 });
              if (!on && g_pianoPlayPrompt.IsApplied())
                  mem::RestoreAll({ &g_pianoPlayPrompt, &g_pianoSeatPrompt4, &g_pianoSeatPrompt5, &g_pianoSetupPrompt4,
                                    &g_pianoSetupPrompt5 });
              if (on && !g_boxHoldOnAction.IsApplied()) g_boxHoldOnAction.Apply();
              if (!on && g_boxHoldOnAction.IsApplied()) g_boxHoldOnAction.Restore();
              if (on && !g_elevatorOne.IsApplied()) mem::ApplyAll({ &g_elevatorOne, &g_elevatorOneGo });
              if (!on && g_elevatorOne.IsApplied()) mem::RestoreAll({ &g_elevatorOne, &g_elevatorOneGo });
              if (on && !g_twistOne.IsApplied()) mem::ApplyAll({ &g_twistOne, &g_twistOneGo });
              if (!on && g_twistOne.IsApplied()) mem::RestoreAll({ &g_twistOne, &g_twistOneGo });
              if (on && !g_cardOne.IsApplied()) mem::ApplyAll({ &g_cardOne, &g_cardOneReady });
              if (!on && g_cardOne.IsApplied()) mem::RestoreAll({ &g_cardOne, &g_cardOneReady });
              if (on && !EnsureDoorKickPatch()) LOG("coop door: kick patch not applied");
              if (!on && g_doorKick && g_doorKick->IsApplied()) g_doorKick->Restore();
              if (!on && g_gateOpen && g_gateOpen->IsApplied()) g_gateOpen->Restore();
              if (!on && g_twistRotate && g_twistRotate->IsApplied()) g_twistRotate->Restore();
              g_on = on;
              if (!on) {
                  Reset();
                  RestoreBoxes();
              }
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "partner.coop_alone_action_prompt", .name = "Actions à deux faites seul : touche d'action",
          .help = "Avec « Actions de partenaire sans attendre », les objets que Chris fait seul (portes de zone, ascenseurs, "
                  "grilles, caisses, chaînes...) demandent la touche d'action (E au clavier, X à la manette, ou A avec "
                  "« Toutes les actions sur A ») au lieu de la touche de partenaire (F / B). Décochée, ils gardent leur "
                  "touche de partenaire. Les sauts à deux aussi quand « rôles inversés » ou « amener le partenaire » est cochée.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsurePadQueryHooks()) return false;
              g_actionIcon = on;  // off: given back by the next tick (game thread)
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "partner.jump_swap", .name = "Sauts à deux : rôles inversés",
          .help = "Dans les sauts à deux (courte échelle), Sheva fait la courte échelle et Chris saute, chacun avec les "
                  "animations de l'autre. Quand Chris appuie, Sheva prend la place du lanceur : elle doit être près du "
                  "saut, sauf avec « Sauts à deux : amener le partenaire ». Chris appuie ensuite pour sauter.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsurePromptPressHook() || !EnsurePadQueryHooks() || !EnsureSetMotionListHook())) return false;
              g_jumpSwap = on;
              if (!on) SwapJumpSlots(false);
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "partner.jump_swap_men", .name = "Sauts à deux : le joueur 1 saute toujours",
          .help = "Comme « Sauts à deux : rôles inversés », mais seulement quand le joueur 1 est un homme (Chris...) : c'est "
                  "toujours le personnage que tu joues qui saute. Si tu joues Sheva, qui saute déjà dans le jeu, rien ne "
                  "change. Quand tu appuies, l'autre joueur prend la place du lanceur : il doit être près du saut, sauf avec « "
                  "Sauts à deux : amener le partenaire ». Tu appuies ensuite pour sauter.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsurePromptPressHook() || !EnsurePadQueryHooks() || !EnsureSetMotionListHook())) return false;
              g_jumpSwapMen = on;
              if (!on && !g_jumpSwap.load(std::memory_order_relaxed)) SwapJumpSlots(false);
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "partner.jump_bring", .name = "Sauts à deux : amener le partenaire",
          .help = "Dans les sauts à deux, dès que le lanceur est en place, l'autre joueur (Sheva, ou Chris si tu joues "
                  "Sheva) est placé tout de suite à son poste par le jeu : plus besoin d'attendre qu'il arrive.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsurePromptPressHook() || !EnsurePadQueryHooks())) return false;
              g_jumpBring = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    RegisterLevers();
    // The added two-person vertical jumps (coop_jump_spots.cpp, "Sauts verticaux à deux ajoutés") removed (user,
    // 2026-11-06: no use since "Obstacles : grimper seul"; how to create a level object: MODLOG, git history).
    RegisterObstacleClimb();
}
}  // namespace features
