// Levers pulled by two, done alone against the clock (user, 2026-10-29, 3-3 after the boat: "when I switch a lever a
// countdown starts and I run to switch the second one before the end: done; else at the end the first lever goes back
// and it must be done again"). The levers are uOmSwitch objects (uOm08f3 there) paired by +0x199C, each with an index
// +0x19A0 (0 / 1) into the game's held bytes (VA 0x1202040 + index, set while a player holds it). Its actions (table RVA
// 0xC11C40): 0 cActWaitToOn (prompts), 1 cActWaitToOff, 2 cActDisable, 3 cActSwitchOn, 4 cActSwitchWait (on), 5
// cActSwitchOff (back up, then 0), 6 cActMessage. Pressing one gives the lever its action 3 (vtable RVA 0xC0AEB4: its
// player [+0xC], its phase byte +0x14) and the player the event cEventSwitchOn (action 0x80000000, [P+0x10F8] vtable RVA
// 0xC11D60, its lever [+0xC]; per frame vtable slot 10, VA 0xB3BC40, phase byte +0x14): phase 1 sets the held byte,
// phase 2 waits for VA 0x1202042, then plays the end of the pull and asks his action 0 (phase 3). The lever's phase 2
// waits for VA 0x1202042 too (back to rest if its player left the event, VA 0xB3A9C2), then plays its end and goes on
// (action 4). VA 0x1202042 is the pair's: the levers' update (VA 0xB36EF0) sets their OmFlag +0x19A8 once both held bytes
// are set, the automatons Lever_B / Lever_C send them message 10 (VA 0x1202042 = 1). In action 4 the lever counts its
// timer +0x1990 down in game time (from +0x1994 at the start; VA 0xB3AD40) and goes back up (action 5) at 0, unless
// +0x1998 is set or the lever is of kind +0xD78 = 9 (these) without its byte +0x199A set that frame.
// With the option, player 1 waiting alone at a paired lever gets the pair's end for his lever only (VA 0x1202042 set by
// the trainer: Chris and the lever play the whole pull, user 2026-10-30 "no feedback, only the timer"), then the lever
// stays on with its timer set to the slider (its duration +0x1994 while it goes on: the action's start copies it into the
// timer after the index changed, user 2026-10-30 "the countdown vanished") and its held byte kept set: player 1 pulls the other one the normal way and
// the pair is done. Time up: +0x199A set, the lever goes back up the game's way - unless player 1 is pulling the other one.
// The pair: a human player's lever event sends its lever message 8 when he grabs it (VA 0xB3BE61); with both held bytes
// set the lever sends itself message 10 (offline, VA 0xB37329) - with the first lever on and its byte kept, the second
// pull alone does it. Each lever's automaton (Lever_B / Lever_C, state "chk") sets its scenario flag (RSF 0 / 1) as soon
// as the property OmFlag is true, its lever on (vtable +0x194, VA 0xB22010: +0x1970 == 1, through VA 0xB36F60), and
// Lever_BC opens the gate on both flags: a lever on alone set its flag for good (user, 2026-10-30: "its button stays red
// and the second lever alone opens the door after the time"). Its button lights with that flag (user: "make the button
// red at the activation anyway" - hiding the lever's on state from its automaton left it dark): the automaton runs as
// usual, the trainer notes the flags set in the second after it leaves its state (its new state's Flag action runs on
// a later frame than the change: the flags read at the change were the old ones, user 2026-10-30; the scenario flags, 256 bits at [VA 0x11B2158] +
// [+0x20] x 0x5940 + 0x23218, flag n = bit 0x80000000 >> (n & 31) of word n >> 5) and, the lever back at rest after the
// time, clears them (VA 0x77FD50) and puts the automaton back in its state (the lever's order [+0x1890], its core +0x10,
// current state +8; mCntState +0x14C).
// The button's light is not the flag's (user, 2026-10-30: "the button stays red" with the flag cleared): the room's own
// script (VA 0x743000, the three levers' ids at VA 0xF54C78, objects in its +0x2C.., light effects' ids in +0x38..)
// lights each lever's effect from the descriptor VA 0x11B0908 + 0x20 x i (the effects manager [VA 0x1249C5C], create VA
// 0x772200 thiscall(manager, descriptor, a, b, c, d) -> id, remove VA 0x7712C0 thiscall(manager, id)); once the lever's
// OmFlag is true it removes it, creates the red one (descriptor + 0x10, VA 0x74311F; its id thrown away) and forgets the
// lever (its id slot 0). That call goes through LampRedStub (the red id noted for the lever on alone): back at rest the
// red one is removed and the first one created again in its slot, the script watching the lever again.
// The partner goes for the lever not held by the other player (VA 0xC9D190, thiscall(ai, lever, lever, x) ret 0xC: held
// byte set and holder +0x19A4 not her slot -> VA 0xC94750, go there; user: "Sheva keeps rushing to the second one"):
// with the option the function returns at once, the levers are player 1's.
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace features {
bool EnsurePadQueryHooks();    // quick_knife.cpp: LeverTick runs on the game thread
bool Player1HudShown();        // player.cpp: the health ring on screen (the chains' time runs only then)
bool EnsureEnemyActionHook();  // action_log.cpp: the units' request core hook (FloorWaits)
const char* UnitClassName(uintptr_t object);  // parry.cpp: its DTI class name

namespace {
constexpr uint32_t kEventAction = 0x80000000;
constexpr uint32_t kRest = 0, kSwitchOn = 3, kSwitchWait = 4;

std::atomic<bool> g_on{ false };
std::atomic<float> g_seconds{ 10.0f };
std::atomic<uintptr_t> g_hudLever{ 0 };  // the lever counting (render thread reads its timer)
void FloorTick();                        // below: a platform's delayed departure

mem::Patch g_partnerLeaves("levers: the partner leaves the paired levers to player 1", 0x89D190, { 0x53, 0x56, 0x8B },
                           { 0xC2, 0x0C, 0x00 });

enum class Stage { None, Pulling, Counting, Done, GoingBack };
struct Kept {
    uintptr_t lever = 0, other = 0, vtable = 0;
    Stage stage = Stage::None;
    DWORD since = 0;
    bool grace = false;
    float duration = 0.0f;  // the lever's own +0x1994, given back
    uintptr_t order = 0, core = 0, before = 0, after = 0;  // its automaton: the state before / after its flag
    uint32_t flags[8] = {}, set[8] = {};                    // the scenario flags before, the ones it set
    DWORD left = 0;                                         // when it left its state
    uintptr_t lampSlot = 0, lampDescriptor = 0, lampArgs[4] = {};
    int32_t lampRed = 0;  // the room script's red light for it (0: none)
};
Kept g_state;  // game thread

uintptr_t Va(uintptr_t rva) { return reinterpret_cast<uintptr_t>(mem::Rva(rva)); }

bool VtableIs(uintptr_t object, uintptr_t rva) {
    uintptr_t vtable = 0;
    return object && mem::Read(object, vtable) && vtable == Va(rva);
}

int HeldIndex(uintptr_t lever) {
    int32_t index = -1;
    return mem::Read(lever + 0x19A0, index) && (index == 0 || index == 1) ? index : -1;
}

void SetHeld(uintptr_t lever, uint8_t held) {
    const int index = HeldIndex(lever);
    if (index >= 0) mem::Write<uint8_t>(Va(0xE02040) + index, held);
}

uint32_t ActionOf(uintptr_t unit) {
    uint32_t action = 0xFFFFFFFF;
    mem::Read(unit + 0x10E0, action);
    return action;
}

// The lever player 1 holds through his lever event (0 if none).
uintptr_t HeldLever(uintptr_t p) {
    uintptr_t event = 0, lever = 0;
    if (!p || ActionOf(p) != kEventAction || !mem::Read(p + 0x10F8, event) || !VtableIs(event, 0xC11D60) ||
        !mem::Read(event + 0xC, lever))
        return 0;
    return lever;
}

using CreateEffectFn = int32_t(__fastcall*)(uintptr_t manager, void* edx, uintptr_t descriptor, uintptr_t a, uintptr_t b,
                                            uintptr_t c, uintptr_t d);
using RemoveEffectFn = void(__fastcall*)(uintptr_t manager, void* edx, int32_t id);
uintptr_t g_lampSlot = 0;  // the script's light id slot at its red light's creation (LampRedStub)

int32_t __fastcall LampRed(uintptr_t manager, void* edx, uintptr_t descriptor, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    const int32_t id = reinterpret_cast<CreateEffectFn>(mem::Rva(0x372200))(manager, edx, descriptor, a, b, c, d);
    uintptr_t lever = 0;
    if (g_lampSlot && g_state.lever && mem::Read(g_lampSlot - 0xC, lever) && lever == g_state.lever &&
        g_state.stage != Stage::Done) {
        g_state.lampSlot = g_lampSlot;
        g_state.lampDescriptor = descriptor;
        g_state.lampArgs[0] = a;
        g_state.lampArgs[1] = b;
        g_state.lampArgs[2] = c;
        g_state.lampArgs[3] = d;
        g_state.lampRed = id;
        LOG("levers: %08X's button lit red (light %d)", static_cast<unsigned>(lever), id);
    }
    return id;
}

// The room script's red light creation (VA 0x74311F: call VA 0x772200), its light id slot in esi.
__declspec(naked) void LampRedStub() {
    __asm {
        mov dword ptr [g_lampSlot], esi
        jmp LampRed
    }
}

mem::Patch* g_lampPatch = nullptr;

bool EnsureLampPatch() {
    if (!g_lampPatch) {
        uint8_t* site = mem::Rva(0x34311F);
        std::vector<uint8_t> original = { 0xE8, 0, 0, 0, 0 }, patched = { 0xE8, 0, 0, 0, 0 };
        const int32_t game = static_cast<int32_t>(reinterpret_cast<uintptr_t>(mem::Rva(0x372200)) - reinterpret_cast<uintptr_t>(site + 5));
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&LampRedStub) - reinterpret_cast<uintptr_t>(site + 5));
        std::memcpy(original.data() + 1, &game, 4);
        std::memcpy(patched.data() + 1, &rel, 4);
        g_lampPatch = new mem::Patch("levers: the 3-3 room's red button lights noted", site, original, patched);
        OnRestore([] { g_lampPatch->Restore(); });  // it calls into this DLL
    }
    return g_lampPatch->IsApplied() || g_lampPatch->Apply();
}

// Back at rest: its red light removed, its first light created again and watched by the room script.
void RestoreLamp() {
    uintptr_t manager = 0, slot = 0;
    if (!g_state.lampRed || !mem::Read(Va(0xE49C5C), manager) || !manager || !mem::Read(g_state.lampSlot, slot) || slot) return;
    __try {
        reinterpret_cast<RemoveEffectFn>(mem::Rva(0x3712C0))(manager, nullptr, g_state.lampRed);
        const int32_t id = reinterpret_cast<CreateEffectFn>(mem::Rva(0x372200))(
            manager, nullptr, g_state.lampDescriptor - 0x10, g_state.lampArgs[0], g_state.lampArgs[1], g_state.lampArgs[2],
            g_state.lampArgs[3]);
        mem::Write<int32_t>(g_state.lampSlot, id);
        LOG("levers: %08X's button back to its first light (light %d)", static_cast<unsigned>(g_state.lever), id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ScenarioFlags(uint32_t (&words)[8]) {
    uintptr_t base = 0;
    int32_t index = 0;
    return mem::Read(Va(0xDB2158), base) && base && mem::Read(base + 0x20, index) &&
           mem::SafeRead(reinterpret_cast<void*>(base + index * 0x5940 + 0x23218), words, sizeof(words));
}

// The lever's automaton leaving its state: the flags it set noted (game thread, every query).
void TrackAutomaton(DWORD tick) {
    if (!g_state.core || (g_state.after && tick - g_state.left > 1000)) return;
    uintptr_t state = 0;
    uint32_t now[8] = {};
    if (!mem::Read(g_state.core + 8, state) || !ScenarioFlags(now)) return;
    if (!g_state.after) {
        if (state == g_state.before) {
            std::memcpy(g_state.flags, now, sizeof(now));
            return;
        }
        g_state.after = state;
        g_state.left = tick;
        LOG("levers: %08X's automaton left its state", static_cast<unsigned>(g_state.lever));
    }
    bool more = false;
    for (int i = 0; i < 8; ++i) {
        const uint32_t added = now[i] & ~g_state.flags[i] & ~g_state.set[i];
        more |= added != 0;
        g_state.set[i] |= added;
    }
    if (more)
        LOG("levers: %08X's automaton set the scenario flags %08X %08X", static_cast<unsigned>(g_state.lever), g_state.set[0],
            g_state.set[1]);
}

// The lever back at rest after the time: its flags cleared, its automaton back in its state.
void RevertAutomaton() {
    uintptr_t state = 0, flags = 0;
    if (!g_state.after || !mem::Read(g_state.core + 8, state) || state != g_state.after) return;
    mem::Write(g_state.core + 8, g_state.before);
    mem::Write<float>(g_state.order + 0x14C, 0.0f);
    if (!mem::Read(Va(0xE340A4), flags) || !flags) return;
    LOG("levers: %08X's automaton back in its state", static_cast<unsigned>(g_state.lever));
    using ClearFn = void(__fastcall*)(uintptr_t flags, void* edx, int index);
    for (int i = 0; i < 256; ++i) {
        if (!(g_state.set[i >> 5] & (0x80000000u >> (i & 31)))) continue;
        __try {
            reinterpret_cast<ClearFn>(mem::Rva(0x37FD50))(flags, nullptr, i);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        LOG("levers: %08X's scenario flag %d cleared", static_cast<unsigned>(g_state.lever), i);
    }
}

void Forget() {
    if (g_state.lever) {
        uintptr_t vtable = 0;
        if (mem::Read(g_state.lever, vtable) && vtable == g_state.vtable) mem::Write<float>(g_state.lever + 0x1994, g_state.duration);
    }
    g_state = {};
    g_hudLever = 0;
}

// The lever's own way back up (its timer at 0 with +0x199A set: action 5, then rest).
void GoBack(const char* why) {
    const uintptr_t lever = g_state.lever;
    SetHeld(lever, 0);
    mem::Write<float>(lever + 0x1990, 0.0f);
    mem::Write<uint8_t>(lever + 0x199A, 1);
    g_state.stage = Stage::GoingBack;
    g_state.since = GetTickCount();
    g_hudLever = 0;
    LOG("levers: %08X goes back up (%s)", static_cast<unsigned>(lever), why);
}

void KeptTick(uintptr_t p1, DWORD now) {
    const uintptr_t lever = g_state.lever;
    uintptr_t vtable = 0;
    if (!mem::Read(lever, vtable) || vtable != g_state.vtable) {
        Forget();
        return;
    }
    const uint32_t action = ActionOf(lever);
    if (g_state.stage != Stage::Done) TrackAutomaton(now);
    switch (g_state.stage) {
    case Stage::Pulling: {  // the pull's end played by Chris and the lever
        uintptr_t current = 0;
        if (action == kSwitchWait && mem::Read(lever + 0x10F8, current) && VtableIs(current, 0xC11E5C)) {  // started
            uint8_t permanent = 0;
            uint32_t kind = 0;
            float timer = 0.0f;
            mem::Read(lever + 0x1998, permanent);
            mem::Read(lever + 0xD78, kind);
            mem::Read(lever + 0x1990, timer);
            if (timer <= 0.0f) mem::Write<float>(lever + 0x1990, g_seconds.load(std::memory_order_relaxed));
            SetHeld(lever, 1);
            g_state.stage = Stage::Counting;
            g_hudLever = lever;
            LOG("levers: %08X on alone, %.0f s to pull the other one %08X (its timer %.2f, kind %u, permanent %u)",
                static_cast<unsigned>(lever), g_seconds.load(std::memory_order_relaxed), static_cast<unsigned>(g_state.other),
                timer, kind, permanent);
        } else if ((action != kSwitchOn && action != kSwitchWait) || now - g_state.since > 15000) {
            LOG("levers: %08X pull not ended (its action %X)", static_cast<unsigned>(lever), action);
            Forget();
        }
        return;
    }
    case Stage::Counting: {
        uint8_t pair = 0;
        mem::Read(Va(0xE02042), pair);
        const uint32_t other = ActionOf(g_state.other);
        if (other == kSwitchWait || (pair && other == kSwitchOn)) {  // the pair done by the game: both flags may go
            g_hudLever = 0;
            g_state.stage = Stage::Done;
            g_state.since = now;
            LOG("levers: %08X and %08X done together", static_cast<unsigned>(lever), static_cast<unsigned>(g_state.other));
            return;
        }
        if (action != kSwitchWait) {
            LOG("levers: %08X left its on state (its action %X)", static_cast<unsigned>(lever), action);
            SetHeld(lever, 0);
            Forget();
            return;
        }
        SetHeld(lever, 1);  // the pair needs it (his event's end cleared it)
        mem::Write<uint8_t>(lever + 0x199A, 0);
        float left = 0.0f;
        mem::Read(lever + 0x1990, left);
        if (left > 0.0f) return;
        if (HeldLever(p1) == g_state.other) {  // pulling the other one at the end: his pull decides
            if (!g_state.grace) LOG("levers: time up while player 1 pulls the other lever: waiting for it");
            g_state.grace = true;
            return;
        }
        GoBack("time up");
        return;
    }
    case Stage::Done:  // its held byte kept till the other one is on
        if (ActionOf(g_state.other) == kSwitchWait || now - g_state.since > 5000) {
            SetHeld(lever, 0);
            Forget();
        }
        return;
    case Stage::GoingBack:
        if (action == kRest || now - g_state.since > 15000) {
            LOG("levers: %08X back at rest", static_cast<unsigned>(lever));
            RevertAutomaton();
            RestoreLamp();
            Forget();
        } else if (action == kSwitchWait) {
            mem::Write<uint8_t>(lever + 0x199A, 1);  // cleared by its update each frame till it goes
        }
        return;
    default:
        Forget();
        return;
    }
}
// The chains pulled by two (user, 2026-10-30, after the collapsing floor: "the same as the levers: one chain down, a
// countdown, then the second one; and Sheva must not pull the other one"). One object, uOmCoopSwitch_S301 (uOma006 there,
// vtables RVA 0xBCA9B0 / 0xBCB3C0), two posts (0 / 1), each with a handle (uOmCoopSwitch_S301_Lever, [+0x1A64 + 4 x
// post]), an area and two prompts. Per player slot: its state +0x1A51 + slot (1 grabbing, 2 holding, 3 pulled) and its
// post +0x1A55 + slot (2: none). A grab is the player's event cActPlReady (vtable RVA 0xBCA954: player +0x20, object
// +0x24, post +0x10, phase +0x14): phase 10 (0x0A) sets his state 2, pulls the handle down (its motion 1 / 5) and sends
// message 0xF0, 11 (0x0B) holds, 12 lets go. The message filter (vtable +0x184, VA 0xAAAF10) asks the object's action 1
// (cActAnother) on 0xF1 only with slots 0 and 1 both at 2; action 1 gives every player (slots 0-3: its GetPlayer call VA
// 0xAABF72, the slot in edi) the pull cActPlPull (vtable RVA 0xBCA980) at his post, then waits for both states at 3 (the
// pulls' end) before its action 2 (cActOpened). The grab's exit (VA 0xAAC400) clears the slot's state and post and, the object at rest, gives the
// post back (VA 0xAAB230 thiscall(object, post): handle up, prompts made again).
// The pull (its start VA 0xAACD60 sets the slot's post from its own and drops the post's prompts; phase 1 waits for the
// handle's motion end and sets the slot's state 3; its exit VA 0xAACDC0 plays the handle back up, motion 0 / 4 by post,
// VA 0x59E880 thiscall(model, 0, motion, [VA 0xFF02FC], 0, 0, 4) ret 0x18 at VA 0xAACE30) waits for nobody.
// With the option, player 1 holding a chain alone (the other slot free) is given the pull at once (the event's request
// vtable +0xE0, its class VA 0x11F9620, as action 1 does; user 2026-10-30: "he let go without pulling") and the chain
// kept: a phantom partner in player 2's slot holds that post (state 2, its post), the post's give-back skipped
// (ChainGiveBack) and its handle left down at the pull's end (ChainHandleBack). His pull holds him about 15 s (the
// handle's motion, then his own long motion made for the opening: user, "stuck about 12 seconds"): he is let go (his
// action 0, as the pull's end asks it) once the handle is down (its phase 2) and the time runs from then (while the HUD
// shows). Player 1 grabs the other chain the normal way, the pair is made, the pull goes to the real holders only
// (ChainPullStub) and the phantom's state follows his to 3; the kept handle goes up with the opening. Time up: the
// phantom leaves, the handle goes up and the post is given back.
// The same family holds levers pulled by two (uOm0939, vtable RVA 0xBCAB78; user, 2026-10-30: "an examine prompt, it
// tells Sheva to pull it, I must pull the other one"): its byte +0x1A50 set, with an AI partner the grab prompt (its
// availability VA 0xAAAB50: +0x1A50 0 -> anyone; else not before the AI got her post +0x1A7E) is refused until player 1
// sends her to a post with the talk prompt (VA 0xAAADF0, shown when the grab is refused). With the option the byte is
// not tested (VA 0xAAAB87, jne -> nops): player 1 grabs either post. The family is told by its message filter (vtable
// +0x184 = VA 0xAAAF10).
// Those levers make a fight of the partner left behind (user, 2026-10-30: "a fight scene is gone"; chapter's s301_em04:
// in "chk_Area" its enemies come once its FreeFlag holds bit 0 - player 1 near the second post - AND bit 2, which the
// first lever's scenario area clears when the last player leaves it; recorded 4 -> 5 with Sheva there, 4 -> 1 alone).
// So for them the partner is sent to stand at the kept post as the talk prompt does (her post +0x1A7E; the order VA
// 0xB77B30 thiscall(player 1, 5, object) gives the AI partner [P+0x2A74] the order through VA 0xB6A360 thiscall(partner,
// 5, object) then plays player 1's order gesture, action 0x81 - it cut his pull, user: only the AI part is called): the
// post held, she cannot grab it and waits there; her post freed at the end.
struct Chain {
    uintptr_t object = 0, vtable = 0, handle = 0;
    int slot = -1, phantom = -1, post = -1;  // player 1's slot, player 2's (the phantom), the post kept
    float elapsed = 0.0f;
    DWORD last = 0;
    bool pulling = false, grace = false, alone = true;  // alone: player 1's own pull of the kept chain not over
    bool pullSeen = false, letGo = false;
    DWORD start = 0;
    bool partnerSent = false;  // a lever of the partner-first kind: player 2 sent to stand at the kept post
};
Chain g_chain;  // game thread
std::atomic<float> g_chainLeft{ -1.0f };  // the countdown on screen (-1: none)

bool IsChainObject(uintptr_t object) {
    uintptr_t vtable = 0, filter = 0;
    return object && mem::Read(object, vtable) && vtable && mem::Read(vtable + 0x184, filter) && filter == Va(0x6AAF10);
}

mem::Patch g_chainAnyPost("chains: any post grabbed without sending the partner first", 0x6AAB87, { 0x75, 0x04 }, { 0x90, 0x90 });
uint8_t ChainByte(uintptr_t object, int offset, int index) {
    uint8_t value = 0xFF;
    mem::Read(object + offset + index, value);
    return value;
}

using GiveBackFn = void(__fastcall*)(uintptr_t object, void* edx, int post);

// The grab's exit giving a post back (VA 0xAAC4B6: call VA 0xAAB230), skipped for the post kept.
void __fastcall ChainGiveBack(uintptr_t object, void* edx, int post) {
    if (g_chain.object && object == g_chain.object && post == g_chain.post) {
        LOG("chains: %08X post %d kept down (its holder left)", static_cast<unsigned>(object), post);
        return;
    }
    reinterpret_cast<GiveBackFn>(mem::Rva(0x6AB230))(object, edx, post);
}

using HandleMotionFn = void(__fastcall*)(uintptr_t model, void* edx, int bank, int motion, float blend, float start,
                                         float speed, int mode);

// The kept handle back up (or the pull's exit, VA 0xAACE30, its handle down kept).
void HandleUp(uintptr_t handle, int post) {
    float blend = 0.0f;
    mem::Read(Va(0xBF02FC), blend);
    __try {
        reinterpret_cast<HandleMotionFn>(mem::Rva(0x19E880))(handle, nullptr, 0, post ? 4 : 0, blend, 0.0f, 0.0f, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall ChainHandleBack(uintptr_t model, void* edx, int bank, int motion, float blend, float start, float speed,
                                int mode) {
    if (g_chain.object && model == g_chain.handle && !g_chain.pulling) {
        LOG("chains: %08X's handle kept down after player 1's pull", static_cast<unsigned>(g_chain.object));
        return;
    }
    reinterpret_cast<HandleMotionFn>(mem::Rva(0x19E880))(model, edx, bank, motion, blend, start, speed, mode);
}

extern "C" uintptr_t __cdecl ChainPullPlayer(uintptr_t manager, int slot) {
    if (g_chain.object && slot == g_chain.phantom) return 0;  // the phantom pulls nothing (player 2 stays)
    using GetPlayerFn = uintptr_t(__fastcall*)(uintptr_t manager, void* edx, int slot);
    return reinterpret_cast<GetPlayerFn>(mem::Rva(0x3E6220))(manager, nullptr, slot);
}

// cActAnother's loop (VA 0xAABF72: mov ecx, [manager]; push edi; call GetPlayer).
__declspec(naked) void ChainPullStub() {
    __asm {
        push dword ptr [esp + 4]  // the slot
        push ecx                  // the player manager
        call ChainPullPlayer
        add esp, 8
        ret 4
    }
}

mem::Patch* g_chainGiveBack = nullptr;
mem::Patch* g_chainPull = nullptr;
mem::Patch* g_chainHandle = nullptr;

bool EnsureChainPatches() {
    if (!g_chainGiveBack) {
        uint8_t* site = mem::Rva(0x6AC4B6);
        std::vector<uint8_t> original = { 0xE8, 0, 0, 0, 0 }, patched = { 0xE8, 0, 0, 0, 0 };
        const int32_t game = static_cast<int32_t>(reinterpret_cast<uintptr_t>(mem::Rva(0x6AB230)) - reinterpret_cast<uintptr_t>(site + 5));
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&ChainGiveBack) - reinterpret_cast<uintptr_t>(site + 5));
        std::memcpy(original.data() + 1, &game, 4);
        std::memcpy(patched.data() + 1, &rel, 4);
        g_chainGiveBack = new mem::Patch("chains: the kept post not given back", site, original, patched);
        OnRestore([] { g_chainGiveBack->Restore(); });  // it calls into this DLL
    }
    if (!g_chainPull) {
        uint8_t* site = mem::Rva(0x6ABF72);
        std::vector<uint8_t> original = { 0x8B, 0x0D, 0xDC, 0x27, 0x1B, 0x01, 0x57, 0xE8, 0, 0, 0, 0 };
        const int32_t game = static_cast<int32_t>(reinterpret_cast<uintptr_t>(mem::Rva(0x3E6220)) - reinterpret_cast<uintptr_t>(site + 12));
        std::memcpy(original.data() + 8, &game, 4);
        std::vector<uint8_t> patched = original;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&ChainPullStub) - reinterpret_cast<uintptr_t>(site + 12));
        std::memcpy(patched.data() + 8, &rel, 4);
        g_chainPull = new mem::Patch("chains: the pull for the real holders only", site, original, patched);
        OnRestore([] { g_chainPull->Restore(); });
    }
    if (!g_chainHandle) {
        uint8_t* site = mem::Rva(0x6ACE30);
        std::vector<uint8_t> original = { 0xE8, 0, 0, 0, 0 }, patched = { 0xE8, 0, 0, 0, 0 };
        const int32_t game = static_cast<int32_t>(reinterpret_cast<uintptr_t>(mem::Rva(0x19E880)) - reinterpret_cast<uintptr_t>(site + 5));
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&ChainHandleBack) - reinterpret_cast<uintptr_t>(site + 5));
        std::memcpy(original.data() + 1, &game, 4);
        std::memcpy(patched.data() + 1, &rel, 4);
        g_chainHandle = new mem::Patch("chains: the kept handle left down", site, original, patched);
        OnRestore([] { g_chainHandle->Restore(); });
    }
    return (g_chainGiveBack->IsApplied() || g_chainGiveBack->Apply()) && (g_chainPull->IsApplied() || g_chainPull->Apply()) &&
           (g_chainHandle->IsApplied() || g_chainHandle->Apply());
}

void ForgetChain() {
    if (g_chain.partnerSent) mem::Write<uint8_t>(g_chain.object + 0x1A7E, 2);  // player 2's post freed
    g_chain = {};
    g_chainLeft = -1.0f;
}

// Time up: the phantom leaves, the handle goes up, the post is given back (its prompts again).
void ChainGoBack(const char* why) {
    const Chain chain = g_chain;
    ForgetChain();
    mem::Write<uint8_t>(chain.object + 0x1A51 + chain.phantom, 0);
    mem::Write<uint8_t>(chain.object + 0x1A55 + chain.phantom, 2);
    if (chain.handle) HandleUp(chain.handle, chain.post);
    __try {
        reinterpret_cast<GiveBackFn>(mem::Rva(0x6AB230))(chain.object, nullptr, chain.post);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    LOG("chains: %08X post %d given back (%s)", static_cast<unsigned>(chain.object), chain.post, why);
}

void ChainTick(uintptr_t p1, DWORD now, bool on) {
    if (g_chain.object) {
        const uintptr_t object = g_chain.object;
        uintptr_t vtable = 0;
        if (!mem::Read(object, vtable) || vtable != g_chain.vtable) {
            ForgetChain();
            return;
        }
        const uint32_t action = ActionOf(object);
        if (action == 1) {  // the pair made: the pull, the phantom done with player 1
            if (!g_chain.pulling) {
                LOG("chains: %08X pulled with the kept post %d", static_cast<unsigned>(object), g_chain.post);
                // The kept handle pulled again with the other one, as the partner's pull would (its motion 3 / 7, VA
                // 0x735F90 thiscall(model, motion, blend, start, speed, mode) ret 0x14, the pull's own call; user,
                // 2026-10-30: "the sound plays on the first chain, not again at the puzzle's end").
                using SetMotionFn = void(__fastcall*)(uintptr_t model, void* edx, int motion, float blend, float start,
                                                      float speed, int mode);
                if (g_chain.handle) {
                    __try {
                        reinterpret_cast<SetMotionFn>(mem::Rva(0x335F90))(g_chain.handle, nullptr, g_chain.post ? 7 : 3, 0.0f, 0.0f,
                                                                          1.0f, 4);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
                }
            }
            g_chain.pulling = true;
            g_chainLeft = -1.0f;
            if (ChainByte(object, 0x1A51, g_chain.slot) == 3 && ChainByte(object, 0x1A51, g_chain.phantom) != 3)
                mem::Write<uint8_t>(object + 0x1A51 + g_chain.phantom, 3);
            return;
        }
        if (action != 0 || g_chain.pulling) {
            LOG("chains: %08X done (its action %u)", static_cast<unsigned>(object), action);
            if (g_chain.handle) HandleUp(g_chain.handle, g_chain.post);  // with the other one, as the game does
            ForgetChain();
            return;
        }
        if (!on) {
            ChainGoBack("option off");
            return;
        }
        mem::Write<uint8_t>(object + 0x1A51 + g_chain.phantom, 2);  // still held
        mem::Write<uint8_t>(object + 0x1A55 + g_chain.phantom, static_cast<uint8_t>(g_chain.post));
        uintptr_t current = 0, pulled = 0;
        if (g_chain.alone) {  // his own pull first: no time running
            uint8_t phase = 0;
            g_chain.last = now;
            if (ActionOf(p1) == kEventAction && mem::Read(p1 + 0x10F8, current) && VtableIs(current, 0xBCA980) &&
                mem::Read(current + 0x24, pulled) && pulled == object) {
                g_chain.pullSeen = true;
                if (!g_chain.letGo && mem::Read(current + 0x14, phase) && phase >= 2) {  // the handle down: he lets go
                    uintptr_t vtable = 0, fn = 0;
                    if (mem::Read(p1, vtable) && mem::Read(vtable + 0xDC, fn) && fn) {
                        using RequestFn = bool(__fastcall*)(uintptr_t self, void* edx, int action, int a, int b, int c, int d);
                        __try {
                            reinterpret_cast<RequestFn>(fn)(p1, nullptr, 0, 0, 0, -1, 0);
                        } __except (EXCEPTION_EXECUTE_HANDLER) {
                        }
                    }
                    g_chain.letGo = true;
                    LOG("chains: %08X handle down, player 1 lets go", static_cast<unsigned>(object));
                }
                return;
            }
            if (!g_chain.pullSeen && now - g_chain.start < 3000) return;  // his pull not started yet
            g_chain.alone = false;
            LOG("chains: %08X player 1's pull over, the time runs", static_cast<unsigned>(object));
        }
        if (Player1HudShown()) g_chain.elapsed += static_cast<float>(now - g_chain.last) / 1000.0f;
        g_chain.last = now;
        const float left = g_seconds.load(std::memory_order_relaxed) - g_chain.elapsed;
        g_chainLeft = left > 0.0f ? left : 0.0f;
        if (left > 0.0f) return;
        uintptr_t event = 0, grabbed = 0;
        if (mem::Read(p1 + 0x10F8, event) && VtableIs(event, 0xBCA954) && mem::Read(event + 0x24, grabbed) && grabbed == object) {
            if (!g_chain.grace) LOG("chains: time up while player 1 grabs the other chain: waiting for it");
            g_chain.grace = true;  // grabbing the other one at the end: his grab decides
            return;
        }
        ChainGoBack("time up");
        return;
    }
    if (!on) return;
    // Player 1 holding a chain alone (phase 0x0B), player 2's slot free, the object at rest.
    const uintptr_t p2 = actors::Player(1);
    uintptr_t event = 0, object = 0;
    uint8_t phase = 0, slot = 0xFF, other = 0xFF;
    if (!p1 || !p2 || ActionOf(p1) != kEventAction || !mem::Read(p1 + 0x10F8, event) || !VtableIs(event, 0xBCA954) ||
        !mem::Read(event + 0x14, phase) || phase != 0x0B || !mem::Read(event + 0x24, object) || !IsChainObject(object) ||
        ActionOf(object) != 0 || !mem::Read(p1 + 0x2D9C, slot) || !mem::Read(p2 + 0x2D9C, other) || slot > 1 || other > 1 ||
        slot == other || ChainByte(object, 0x1A51, slot) != 2 || ChainByte(object, 0x1A51, other) != 0)
        return;
    const uint8_t post = ChainByte(object, 0x1A55, slot);
    if (post > 1 || !EnsureChainPatches()) return;
    g_chain = { object, 0, 0, slot, other, post, 0.0f, now, false, false, true, false, false, now, false };
    mem::Read(object, g_chain.vtable);
    mem::Read(object + 0x1A64 + 4 * post, g_chain.handle);
    mem::Write<uint8_t>(object + 0x1A51 + other, 2);  // the phantom holds his post
    mem::Write<uint8_t>(object + 0x1A55 + other, post);
    uintptr_t vtable = 0, fn = 0;
    if (mem::Read(p1, vtable) && mem::Read(vtable + 0xE0, fn) && fn) {  // player 1 pulls it (the pair's pull)
        using EventFn = bool(__fastcall*)(uintptr_t self, void* edx, uintptr_t event, uintptr_t object, int post, int flag);
        __try {
            reinterpret_cast<EventFn>(fn)(p1, nullptr, Va(0xDF9620), object, post, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    int32_t partnerControl = 0;
    if (ChainByte(object, 0x1A50, 0) && mem::Read(p2 + 0x2DA8, partnerControl) && partnerControl == 1) {
        mem::Write<uint8_t>(object + 0x1A7E, post);  // a partner-first lever: player 2 stands at the kept post
        using OrderFn = void(__fastcall*)(uintptr_t partner, void* edx, int order, uintptr_t object);
        __try {
            reinterpret_cast<OrderFn>(mem::Rva(0x76A360))(p2, nullptr, 5, object);
            g_chain.partnerSent = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        LOG("chains: %08X a partner-first lever: player 2 sent to stand at post %d", static_cast<unsigned>(object), post);
    }
    g_chainLeft = g_seconds.load(std::memory_order_relaxed);
    LOG("chains: %08X post %d held alone by player 1 (slot %u): kept down %.0f s, a phantom in slot %u",
        static_cast<unsigned>(object), post, slot, g_seconds.load(std::memory_order_relaxed), other);
}
}  // namespace

// quick_knife.cpp's pad query hook (game thread, every query).
void LeverTick() {
    if (!actors::InGame()) {
        if (g_state.lever) Forget();
        if (g_chain.object) ForgetChain();
        FloorTick();  // forgets a platform held
        return;
    }
    FloorTick();
    const bool on = g_on.load(std::memory_order_relaxed);
    ChainTick(actors::Player(0), GetTickCount(), on);
    if (g_state.lever && !on && g_state.stage == Stage::Counting) GoBack("option off");
    const uintptr_t p1 = actors::Player(0);
    const DWORD now = GetTickCount();
    if (g_state.lever) {
        KeptTick(p1, now);
        return;
    }
    if (!on) return;
    // Player 1 alone at a paired lever pulled down, waiting for the pair (both phases 2), the other lever at rest.
    const uintptr_t lever = HeldLever(p1);
    uintptr_t other = 0, action = 0, event = 0;
    uint8_t phase = 0, eventPhase = 0;
    if (!lever || !mem::Read(lever + 0x199C, other) || !other || HeldIndex(lever) < 0 || ActionOf(lever) != kSwitchOn ||
        !mem::Read(lever + 0x10F8, action) || !VtableIs(action, 0xC0AEB4) || !mem::Read(action + 0x14, phase) || phase != 2 ||
        !mem::Read(p1 + 0x10F8, event) || !mem::Read(event + 0x14, eventPhase) || eventPhase != 2 || ActionOf(other) != kRest)
        return;
    g_state = { lever, other, 0, Stage::Pulling, now, false, 0.0f };
    mem::Read(lever, g_state.vtable);
    uintptr_t owner = 0;
    if (mem::Read(lever + 0x1890, g_state.order) && g_state.order && mem::Read(g_state.order + 8, owner) && owner == lever &&
        mem::Read(g_state.order + 0x10, g_state.core) && g_state.core && mem::Read(g_state.core + 8, g_state.before))
        ScenarioFlags(g_state.flags);
    else
        g_state.order = g_state.core = g_state.before = 0;
    mem::Read(lever + 0x1994, g_state.duration);
    mem::Write<float>(lever + 0x1994, g_seconds.load(std::memory_order_relaxed));  // its on state's timer
    mem::Write<uint8_t>(Va(0xE02042), 1);  // the pair's end, for this lever and its player only (the other one at rest)
    LOG("levers: player 1 alone at %08X: its pull ended as a pair's (the other lever %08X)", static_cast<unsigned>(lever),
        static_cast<unsigned>(other));
}

// ---- The platform sent by a lever (user, 2026-10-30: "Sheva goes on the platform, I pull the lever, then she pulls the
// other side's to bring me over; alone I need a timer as usual: the platform waits 5 s before it leaves, the time to get
// on it"). The platform is a uOm090d_Floor (actions 0 Wait, 1 Move, 2 Stop, 3 Auto, 4 Dock), its levers two unpaired
// uOm08bc (uOmSwitch). Its requests go through the units' request core (its vtable +0xDC is RVA 0x759030 itself,
// action_log.cpp's HookRequestCore): a Move asked while it is not moving is refused for the slider's seconds from the
// first ask, then asked again with its arguments (FloorTick), as crank.cpp does with the cranks' rewind. Every departure
// waits (either lever).
// The lift right after it (user, 2026-10-31: "a lift platform on the same principle, I need a timer too to get on it
// before it leaves"): its lever moves it itself, a uOm0944 (uOmSwitch: 0 WaitToUp, 1 WaitToDown, 2 Disable, 3 SwitchOn,
// 4 LiftUp, 5 LiftDown); SwitchOn's update (RVA 0x71A6F0) asks LiftUp / LiftDown through vtable +0xDC (the request
// core) once the pull is done: held the same way. The departures are found by their action names (cActMove of a
// "_Floor" class, cActLiftUp / cActLiftDown).
// Its states switch four parts of the stage (ids at +0x19E4 / +0x19E8 / +0x19EC / +0x19F0, through VA 0x728F90(0, id, on):
// [aGame]+0x3830's group table, then [VA 0x1249C64]'s VA 0x4B5C80): WaitToUp (down) 0 1 0 0, WaitToDown (up) 0 0 1 0,
// SwitchOn and the moves 1 . . 1. The second is the jump over the barrier at the bottom (user, 2026-10-31: "I cannot get
// on: its jump prompt is gone with the timer", SwitchOn switched it off), the third its twin at the top: while a lift is
// held its waiting state's parts are given back, SwitchOn's set again before it leaves.
namespace {
std::atomic<bool> g_floorOn{ false };
std::atomic<float> g_floorSeconds{ 5.0f };
std::atomic<float> g_floorLeft{ -1.0f };  // the countdown on screen (-1: none)
struct HeldFloor {
    uintptr_t floor = 0;
    DWORD since = 0;
    int action = 0;
    int args[4] = {};
    bool lift = false;  // a lever lift (its stage parts switched)
};
HeldFloor g_floor;  // game thread (the request core and LeverTick)
bool g_floorReplaying = false;

// The departure actions of a unit's class (bit n = action n), by vtable.
uint32_t Departures(uintptr_t unit) {
    static uintptr_t vtables[8] = {};
    static uint32_t results[8] = {};
    static int next = 0;
    uintptr_t vtable = 0, table = 0;
    if (!unit || !mem::Read(unit, vtable) || !vtable) return 0;
    for (int i = 0; i < 8; ++i)
        if (vtables[i] == vtable) return results[i];
    uint32_t mask = 0, count = 0;
    const char* name = UnitClassName(unit);
    const bool floor = name && std::strncmp(name, "uOm", 3) == 0 && std::strstr(name, "_Floor");
    if (name && std::strncmp(name, "uOm", 3) == 0 && mem::Read(unit + 0x10D0, count) && count <= 32 &&
        mem::Read(unit + 0x10DC, table) && table)
        for (uint32_t i = 0; i < count; ++i) {
            uintptr_t object = 0;
            const char* action = mem::Read(table + 4 * i, object) && object ? UnitClassName(object) : nullptr;
            const char* tail = action ? std::strstr(action, "::cAct") : nullptr;
            if (tail && ((floor && std::strcmp(tail, "::cActMove") == 0) || std::strcmp(tail, "::cActLiftUp") == 0 ||
                         std::strcmp(tail, "::cActLiftDown") == 0))
                mask |= 1u << i;
        }
    vtables[next] = vtable;
    results[next] = mask;
    next = (next + 1) % 8;
    return mask;
}

// The class name of a unit's action `index` after its "::cAct" ("" if none).
const char* ActionTail(uintptr_t unit, int index) {
    uint32_t count = 0;
    uintptr_t table = 0, object = 0;
    if (index < 0 || !mem::Read(unit + 0x10D0, count) || static_cast<uint32_t>(index) >= count || count > 64 ||
        !mem::Read(unit + 0x10DC, table) || !table || !mem::Read(table + 4 * index, object) || !object)
        return "";
    const char* name = UnitClassName(object);
    const char* tail = name ? std::strstr(name, "::cAct") : nullptr;
    return tail ? tail + 6 : "";
}

void LiftParts(uintptr_t lift, const int (&on)[4]) {
    using SwitchFn = void(__stdcall*)(int group, int id, int on);
    for (int i = 0; i < 4; ++i) {
        int32_t id = 0;
        if (!mem::Read(lift + 0x19E4 + 4 * i, id)) continue;
        __try {
            reinterpret_cast<SwitchFn>(mem::Rva(0x328F90))(0, id, on[i]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

bool Departing(uintptr_t unit) {
    const uint32_t action = ActionOf(unit);
    return action < 32 && (Departures(unit) & (1u << action));
}

void ForgetFloor() {
    g_floor = {};
    g_floorLeft = -1.0f;
}

// The game thread: the time up (or the option off), Move asked again unless it left by itself.
void FloorTick() {
    const uintptr_t floor = g_floor.floor;
    if (!floor) return;
    if (!actors::InGame() || !Departures(floor)) {
        ForgetFloor();
        return;
    }
    if (Departing(floor)) {
        LOG("platform: %08X left by itself", static_cast<unsigned>(floor));
        ForgetFloor();
        return;
    }
    const float total = g_floorSeconds.load(std::memory_order_relaxed);
    const float elapsed = static_cast<float>(GetTickCount() - g_floor.since) / 1000.0f;
    if (g_floorOn.load(std::memory_order_relaxed) && elapsed < total) {
        g_floorLeft = total - elapsed;
        return;
    }
    const HeldFloor held = g_floor;
    ForgetFloor();
    if (held.lift) LiftParts(floor, { 1, 0, 0, 1 });  // SwitchOn's
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(floor, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return;
    using RequestFn = bool(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    bool asked = false;
    g_floorReplaying = true;
    __try {
        asked = reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(floor), nullptr, held.action, held.args[0],
                                                held.args[1], held.args[2], held.args[3]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_floorReplaying = false;
    LOG("platform: %08X leaves after %.1f s (action %X %s)", static_cast<unsigned>(floor), elapsed, held.action,
        asked ? "asked" : "refused");
}
}  // namespace

// action_log.cpp's HookRequestCore: true refuses the request.
bool FloorWaits(uintptr_t unit, int action, int a, int b, int c, int d, uintptr_t caller) {
    if (g_floorReplaying || action < 0 || action >= 32 || !g_floorOn.load(std::memory_order_relaxed) ||
        !(Departures(unit) & (1u << action)) || Departing(unit))
        return false;
    if (g_floor.floor != unit) {
        const DWORD now = GetTickCount();
        g_floor = { unit, now ? now : 1, action, { a, b, c, d } };
        g_floorLeft = g_floorSeconds.load(std::memory_order_relaxed);
        const char* tail = ActionTail(unit, action);
        if (std::strcmp(tail, "LiftUp") == 0 || std::strcmp(tail, "LiftDown") == 0) {
            g_floor.lift = true;
            if (tail[4] == 'U') LiftParts(unit, { 0, 1, 0, 0 });  // WaitToUp's: down, the jump over its barrier
            else LiftParts(unit, { 0, 0, 1, 0 });                 // WaitToDown's: up
        }
        LOG("platform: %s %08X asked to leave (action %X, its action %X, caller %08X), it waits %.0f s", UnitClassName(unit),
            static_cast<unsigned>(unit), action, ActionOf(unit), static_cast<unsigned>(caller),
            g_floorSeconds.load(std::memory_order_relaxed));
    }
    return true;
}

// coop_solo.cpp's action icon: a prompt of a paired lever done alone (user, 2026-10-30: "a double lever shows a partner
// prompt with the option on"; uOmSwitch is no co-op class). Paired: each one's +0x199C names the other.
bool LeverPromptAlone(uintptr_t owner) {
    uintptr_t other = 0, back = 0;
    return owner && g_on.load(std::memory_order_relaxed) && mem::Read(owner + 0x199C, other) && other && other != owner &&
           mem::Read(other + 0x199C, back) && back == owner && HeldIndex(owner) >= 0;
}

namespace {
void DrawCountdown() {
    const uintptr_t lever = g_hudLever.load(std::memory_order_relaxed);
    float left = g_floorLeft.load(std::memory_order_relaxed);
    const char* format = i18n::Tr("Plateforme : %d s");
    if (left < 0.0f) {
        left = g_chainLeft.load(std::memory_order_relaxed);
        format = i18n::Tr("Chaîne : %d s");
    }
    if (left < 0.0f) {
        if (!lever || !mem::Read(lever + 0x1990, left)) return;
        format = i18n::Tr("Levier : %d s");
    }
    if (left < 0.0f) left = 0.0f;
    char text[48];
    std::snprintf(text, sizeof(text), format, static_cast<int>(std::ceil(left)));
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float size = screen.y * 0.045f;
    const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 at((screen.x - extent.x) * 0.5f, screen.y * 0.08f);
    const ImU32 colour = left <= 3.0f ? IM_COL32(255, 90, 70, 255) : IM_COL32(255, 235, 200, 255);  // red at the end
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(ImGui::GetFont(), size, at, colour, text);
}
}  // namespace

void RegisterLevers() {
    Feature& self = Add({ .id = "partner.levers_alone", .name = "Leviers et chaînes à deux : seul avec un compte à rebours",
                          .help = "Les paires de leviers à abaisser ou de chaînes à tirer ensemble : quand Chris abaisse un "
                                  "levier ou tient une chaîne, il la lâche et elle reste baissée le nombre de secondes du "
                                  "curseur (le temps restant s'affiche en haut de l'écran). Abaisse l'autre avant la fin : "
                                  "c'est validé. Sinon la première remonte et il faut recommencer. Sheva les laisse à Chris.",
                          .category = "Joueur", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = 10, .valueMin = 3, .valueMax = 60, .valueFormat = "%.0f s" });
    Feature* const feature = &self;
    self.set = [feature](bool on) {
        if (on && !EnsurePadQueryHooks()) return false;
        if (on && !EnsureLampPatch()) LOG("levers: the red light patch not applied (another room?)");
        if (on && !EnsureChainPatches()) LOG("chains: patches not applied");
        if (on && !g_chainAnyPost.IsApplied()) g_chainAnyPost.Apply();
        if (!on && g_chainAnyPost.IsApplied()) g_chainAnyPost.Restore();
        if (on && !g_partnerLeaves.IsApplied()) g_partnerLeaves.Apply();
        if (!on && g_partnerLeaves.IsApplied()) g_partnerLeaves.Restore();
        g_seconds = feature->value;
        g_on = on;  // a lever still counting goes back up (LeverTick)
        return true;
    };
    self.tick = [feature] { g_seconds.store(feature->value, std::memory_order_relaxed); };
    Feature& floor = Add({ .id = "partner.platform_wait", .name = "Plateformes et ascenseurs à levier : départ retardé",
                           .help = "Une plateforme ou un ascenseur mis en marche par un levier (Sheva monte dessus, Chris "
                                   "abaisse le levier) attend le nombre de secondes du curseur avant de partir : le temps "
                                   "de monter dessus après avoir abaissé le levier. Le temps restant s'affiche en haut de "
                                   "l'écran.",
                           .category = "Joueur", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = 5, .valueMin = 2, .valueMax = 30, .valueFormat = "%.0f s" });
    Feature* const floorFeature = &floor;
    floor.set = [floorFeature](bool on) {
        if (on && (!EnsurePadQueryHooks() || !EnsureEnemyActionHook())) return false;
        g_floorSeconds = floorFeature->value;
        g_floorOn = on;  // a platform held leaves at once (FloorTick)
        return true;
    };
    floor.tick = [floorFeature] { g_floorSeconds.store(floorFeature->value, std::memory_order_relaxed); };
    OnHud(DrawCountdown);
    OnRestore([] {
        g_partnerLeaves.Restore();
        g_chainAnyPost.Restore();
    });
}
}  // namespace features
