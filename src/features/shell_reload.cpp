// Shell-by-shell reloads (user, 2026-11-01: "the pump shotgun holds 20 shells in Mercenaries, but whatever is missing
// Chris always puts two in and pumps: one shell used, one inserted; twenty missing, twenty insertions before the pump";
// "each shell inserted adds 1: interrupted after 5, the gun has 5 more, not the whole magazine as in the original").
// The reload (player action 0x1F, update VA 0xC8AC30) starts its motions on layers 0-5 (the Ithaca: 0x520, 0x521,
// 0x51F, 0x549, 0x54A, 0x548, layer 0 at 0.8 of the others' pace) and the gun its own (its layer 0, motion 4); at the
// layer 0 frame of the weapon parameter +0x20 (VA 0x11B5170 + (item & 0xFF) * 0x140) its step VA 0xC728C0 (vtable
// +0x28, state +0x14 == 1) fills the magazine at once: capacity (getter RVA 0x84C050) - count, clamped to the reserve
// and taken from it (not with "Recharger sans réserve", ammo.cpp). Recorded on the Ithaca (layer 1 frames, the left
// hand in the chest's frame): the hand leaves the fore-end 0-10, takes a shell 10-28 (se_pl00 0x46 at 25), pushes it
// in 28-76 (0x93 at 68), takes and pushes the second 76-115 (the gun's se_wp04 0xB at 89), goes back to the fore-end
// 115-145, pumps 145-160, ends at 179; at 115 the hand is where it was at 76 (2 cm). So the game's fill is refused,
// each push gives one shell (the step called with the capacity read as count + 1: the game's own clamp, reserve and
// infinite rules), and the 76-115 cycle is played again while shells are missing, or skipped when the first one filled
// the magazine (each reload layer started again at its matching frame through the player's SetMotion, vtable +0x1E4,
// blend 4 frames; the gun's own motion given its frame (+0x18): its SetMotion, VA 0xB96130, faulted on every jump at
// re5dx9+0x22C44, caught - a game crash came later). A new press of the aim button stops the reload (user: "now that
// it is longer, cancel it, not to be locked"): the reload's cancel flag 0x20000000 (VA 0xB5C350, as its own state 2)
// then the aim begin 0x1B asked; the shells in stay.
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "hooks/hooks.hpp"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace features {
bool EnsureMagazineCapHook();                       // fire_rate.cpp
bool GameButtonHeld(int button, int pad);           // quick_knife.cpp
int Player1Pad();                                   // movement.cpp
void SetMagazineCap(uintptr_t record, int cap);     // fire_rate.cpp: the record's capacity read as at most `cap`
int MagazineOfRecord(uintptr_t record, int getter); // fire_rate.cpp: the record's max when bigger than the getter (Mercenaries)
int ShellsPerPush(uint16_t item);                   // fire_rate.cpp: the weapon table's "Cartouches par geste"
bool WeaponBankIsIthaca(uintptr_t p);               // quick_knife.cpp: the Ithaca M37's list in the weapon bank
bool ChamberFillBegin(uintptr_t record);            // fire_rate.cpp: its round in the chamber read as one more
void ChamberFillEnd();

namespace {
constexpr uint32_t kReloadAction = 0x1F;
constexpr int kReloadLayers = 6;   // the reload's motions: layers 0..5
constexpr float kBlend = 4.0f;     // frames of the restart's blend
constexpr int kAimButton = 0x0C;
constexpr uint32_t kAimBegin = 0x1B, kCancelFlag = 0x20000000;

struct ShellWeapon {
    uint16_t item;
    int layer;                 // the layer the frames below are told in
    float firstShell;          // the first push ends: +1
    float cycleFrom, cycleTo;  // one more shell's cycle (the hand at the same place at both ends)
    float cycleShell;          // its push ends: +1
    // A second push after the cycle that gives the last round (the S75): the cycle is looped while more than one push
    // is missing, and skipped (cycleFrom -> cycleTo) when one push is enough: the second push gives it.
    float lastShell = 0.0f;
    // Four rounds natively (the revolvers, user 2026-11-04: "it puts four in natively: cut for one, two and three"):
    // the frames each round goes in (the first = firstShell, the third = cycleShell inside the cycle), and after the
    // first one, the jumps that skip the next ones when only R more pushes are needed (R = 0, 1, 2; from -> to).
    float shellAt[4] = {};
    float skipFrom[3] = {}, skipTo[3] = {};
};
constexpr ShellWeapon kShellWeapons[] = {
    { 0x0104, 1, 70.0f, 76.0f, 115.0f, 110.0f },  // Ithaca M37
    // M3: the left hand (layer 1, at the pace of layer 0) pushes its first shell 48-60, takes the second 63-74, pushes
    // it 86-92, leaves 95-108 for the fore-end: the hands at 62 and 90.6 are 3 cm apart (the Ithaca's 76 / 115 put them
    // some 40 cm apart there: "a small jump after the first two", user).
    { 0x0116, 1, 58.0f, 62.0f, 90.6f, 88.0f },
    // S75 (recorded 2026-11-04, reload_rec3.py, all its layers at one pace; pictures every 3 frames of a native
    // reload): the right hand fetches the rounds at the belt 29-62, holds them up 62-82, pushes the first 86-111 (the
    // game's fill at 90.8), presses the second 112-139 (in sight under the hand at 114), then 142-171 and the bolt
    // 174-205 (user: "the second gesture is a second round, as the Ithaca's: one missing, cut the second; two, native;
    // more, loop the first part one time less"). At 111.3 both hands are back where they were at 82.3 (right 1.7 cm,
    // left 2.3 cm): the first push is the cycle. One missing: the cycle skipped, the second press gives it (cutting the
    // second press itself left 7 cm jumps; skips at 113 and 139 left the second round in sight, user).
    { 0x0105, 1, 89.0f, 82.3f, 111.3f, 89.0f, 125.0f },
    // Hydra (recorded 2026-11-04 with pictures every 3 frames; always its own reload list, quick_knife.cpp
    // HydraReloadBank; every layer and the gun at one pace): opened 0-49, the spent shells thrown 66-70 while the left
    // hand takes the first round at the hip 53-75, puts it in 78-104 (at the breech from 94, the game's fill at 98), takes
    // the second 103-123, puts it in 126-147 (another barrel: the left hand 8 cm from where the first went), closes
    // 150-186. User: "the same as the S75: two rounds natively". 95.0 and 143.1 are the same moment of the two puts
    // (right hand 4.0 cm, left 7.8 cm apart, both at the same speeds): the second put is the cycle, as the Ithaca's.
    { 0x0119, 1, 94.0f, 95.0f, 143.1f, 142.0f },
    // Jail Breaker (recorded 2026-11-04, capacity 5 here, its own reload list, every layer and the gun at one pace; the
    // game's fill at 62): the left hand takes a shell at the hip 8-35, puts it in the drum 51-58, takes the second 65-80,
    // puts it in 87-96, then the fore-end and the aim 103-160 (pictures every 3 frames). 62.5 and 101.0 are the hand
    // leaving the drum after each (right 2.8 cm, left 7.5 cm, equal speeds): the second put is the cycle.
    { 0x0117, 1, 57.0f, 62.5f, 101.0f, 95.0f },
    // M29 (recorded 2026-11-04, capacity 6, the game's fill at 91, every layer and the gun at one pace; pictures every 3
    // frames): opened 0-10, the left hand takes the rounds at the belt 10-48 while the right flicks the cylinder out,
    // puts a round in at 87-91, 111-115, 135-142 and 158-164 (each the left hand down onto the cylinder), closes and
    // aims 165-201. User: "four rounds natively; cut for one, two and three". 129.5 and 155.0 are the same moment of the
    // third and fourth gestures (right hand 0.3 cm, left 4.0 cm, equal speeds): the third gesture is the cycle. After
    // the first round: 101.0 -> 165.0 skips the three others (right 3.5 cm, left 5.4 cm), 112.0 -> 160.0 the second and
    // third (2.4 / 7.6 cm), 97.0 -> 124.5 the second (2.8 / 5.5 cm).
    { 0x010B, 1, 90.0f, 129.5f, 155.0f, 140.0f, 0.0f, { 90.0f, 114.0f, 140.0f, 162.0f }, { 101.0f, 112.0f, 97.0f },
      { 165.0f, 160.0f, 124.5f } },
    // S&W M500 (recorded 2026-11-04, capacity 5, research/shell_reload/m500_reload.txt): the M29's motion (the hands
    // within 2 cm of it from frame 10 to 170; its end 10 frames shorter), so the M29's frames.
    { 0x011B, 1, 90.0f, 129.5f, 155.0f, 140.0f, 0.0f, { 90.0f, 114.0f, 140.0f, 162.0f }, { 101.0f, 112.0f, 97.0f },
      { 165.0f, 160.0f, 124.5f } },
};

const ShellWeapon* ShellWeaponOf(uint16_t item) {
    for (const ShellWeapon& w : kShellWeapons)
        if (w.item == item) return &w;
    return nullptr;
}

struct ShellRun {
    uintptr_t player = 0;
    const ShellWeapon* weapon = nullptr;
    uintptr_t record = 0, gun = 0;
    uint32_t motions[kReloadLayers]{};
    uint32_t gunMotion = 0;
    float last = 0.0f;
    int pushed = 0;
    bool dry = false;  // a push gave nothing: no shell left
    bool aimHeld = false;  // the aim button, last tick (player 1)
    bool cancelled = false;
};
ShellRun g_runs[2];
std::atomic<bool> g_shellOn{ false };
// Shells each push puts in (user, 2026-11-01: "how many each insertion puts in at once, 1 to 10, 1 by default; with
// one missing and 10 set, only one", then "in the weapon table, per weapon"): the weapon table's column; the
// capacity is read as count + n, the game's own fill clamps to the real capacity and the reserve.
bool g_ownFill = false;  // game thread: our own call of the step

uintptr_t Layer(uintptr_t unit, int n) { return unit + 0x380 + 0xE0 * n; }
float LayerFrame(uintptr_t unit, int n) {
    float f = -1.0f;
    mem::Read(Layer(unit, n) + 0x18, f);
    return f;
}
uint32_t LayerMotion(uintptr_t unit, int n) {
    uint32_t m = 0;
    mem::Read(Layer(unit, n) + 4, m);
    return m & 0xFFFF;
}

uintptr_t ReloadActionOf(uintptr_t p) {
    uintptr_t table = 0, action = 0;
    if (!mem::Read(p + 0x10DC, table) || !table || !mem::Read(table + 4 * kReloadAction, action)) return 0;
    return action;
}

uintptr_t HeldRecord(uintptr_t p, uint16_t* item, uintptr_t* unit) {
    uintptr_t held = 0;
    if (!mem::Read(p + 0x2634, held) || !held) return 0;
    for (int k = 0; k < 24; ++k) {
        const uintptr_t record = p + 0x21A4 + 0x30 * k;
        uintptr_t u = 0;
        uint32_t id = 0;
        if (mem::Read(record + 0x2C, u) && u == held && mem::Read(record + 4, id)) {
            *item = static_cast<uint16_t>(id & 0xFFFF);
            *unit = held;
            return record;
        }
    }
    return 0;
}

int Count(uintptr_t record) {
    int32_t n = 0;
    mem::Read(record + 8, n);
    return n;
}

int GetterCapacity(uintptr_t p, uintptr_t record) {
    using CapacityFn = int(__fastcall*)(void* manager, void* edx, uintptr_t record);
    __try {
        return reinterpret_cast<CapacityFn>(mem::Rva(0x84C050))(reinterpret_cast<void*>(p + 0x21A0), nullptr, record);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The getter, or the record's max when bigger (Mercenaries sets its weapons' magazine there: No Mercy's Ithaca stopped at 6 of 20).
int Capacity(uintptr_t p, uintptr_t record) { return MagazineOfRecord(record, GetterCapacity(p, record)); }

bool Missing(const ShellRun& r) { return !r.dry && Count(r.record) < Capacity(r.player, r.record); }

using StepFn = void(__fastcall*)(void* action, void* edx);
StepFn g_step = nullptr;

// The game's fill of a reload we lead is refused (also the commit of an interrupted one).
void __fastcall HookStep(void* action, void* edx) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(action);
    uint8_t state = 0;
    uintptr_t p = 0;
    if (!g_ownFill && g_shellOn.load(std::memory_order_relaxed) && mem::Read(a + 0x14, state) && state == 1 &&
        mem::Read(a + 0x18, p) && p)
        for (const ShellRun& r : g_runs)
            if (r.player == p && r.weapon && a == ReloadActionOf(p)) return;
    // "Balle dans la chambre (+1)": the game's fill of a weapon holding a round fills one more.
    bool chamber = false;
    if (!g_ownFill && mem::Read(a + 0x14, state) && state == 1 && mem::Read(a + 0x18, p) && p && a == ReloadActionOf(p)) {
        uint16_t item = 0;
        uintptr_t unit = 0;
        if (const uintptr_t record = HeldRecord(p, &item, &unit)) chamber = ChamberFillBegin(record);
    }
    g_step(action, edx);
    if (chamber) ChamberFillEnd();
}

bool EnsureStepHook() {
    if (g_step) return true;
    uint8_t* const target = mem::Rva(0x8728C0);
    static constexpr uint8_t kExpected[] = { 0x57, 0x8B, 0xF9, 0x8B, 0x4F, 0x18, 0x85, 0xC9 };
    if (!mem::IsGameCode(target, sizeof(kExpected)) || std::memcmp(target, kExpected, sizeof(kExpected)) != 0) {
        LOG("shell reload: unexpected bytes at the reload step %s", mem::Hex(target, sizeof(kExpected)).c_str());
        return false;
    }
    void* original = nullptr;
    const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<void*>(&HookStep), &original);
    if ((created != MH_OK && created != MH_ERROR_ALREADY_CREATED) || MH_EnableHook(target) != MH_OK) {
        LOG("shell reload: reload step hook %s", MH_StatusToString(created));
        return false;
    }
    g_step = reinterpret_cast<StepFn>(original);
    LOG("shell reload: reload step hook installed at %p", target);
    return true;
}

// One push: the game's step in its fill state, the capacity read as the count + the shells per push.
bool PushShell(ShellRun& r) {
    const uintptr_t action = ReloadActionOf(r.player);
    uint8_t state = 0;
    if (!g_step || !action || !mem::Read(action + 0x14, state)) return false;
    const int before = Count(r.record);
    SetMagazineCap(r.record, before + std::clamp(ShellsPerPush(r.weapon->item), 1, 20));
    g_ownFill = true;
    mem::Write<uint8_t>(action + 0x14, 1);
    __try {
        g_step(reinterpret_cast<void*>(action), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    mem::Write<uint8_t>(action + 0x14, state);
    g_ownFill = false;
    SetMagazineCap(0, 0);
    const int after = Count(r.record);
    ++r.pushed;
    LOG("shell reload: push %d (%d -> %d of %d)", r.pushed, before, after, Capacity(r.player, r.record));
    return after > before;
}

void StartAt(uintptr_t unit, int layer, uint32_t motion, float frame, float blend = kBlend) {
    using SetMotionFn = uint32_t(__fastcall*)(void* self, void* edx, uint32_t layer, uint32_t motion, float blend, float start,
                                              float speed, uint32_t mode);
    uintptr_t vtable = 0, fn = 0;
    float speed = 1.0f;
    uint16_t mode = 0;
    if (!mem::Read(unit, vtable) || !vtable || !mem::Read(vtable + 0x1E4, fn) || !fn) return;
    mem::Read(Layer(unit, layer) + 0x24, speed);
    mem::Read(Layer(unit, layer) + 0xA, mode);
    __try {
        reinterpret_cast<SetMotionFn>(fn)(reinterpret_cast<void*>(unit), nullptr, layer, motion, blend, frame, speed, mode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Every reload layer and the gun to the frame matching `target` on the told layer (they run at fixed paces).
void JumpTo(const ShellRun& r, float target) {
    const float from = LayerFrame(r.player, r.weapon->layer);
    if (from <= 0.0f) return;
    const float scale = target / from;
    for (int n = 0; n < kReloadLayers; ++n)
        if (r.motions[n] && LayerMotion(r.player, n) == r.motions[n])
            StartAt(r.player, n, r.motions[n], LayerFrame(r.player, n) * scale);
    if (r.gun && r.gunMotion && LayerMotion(r.gun, 0) == r.gunMotion) mem::Write(Layer(r.gun, 0) + 0x18, LayerFrame(r.gun, 0) * scale);
}

void RequestAction(uintptr_t p, uint32_t action) {
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(p, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return;
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(p), nullptr, static_cast<int>(action), 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void AllowCancel(uintptr_t p) {
    using FlagFn = void(__fastcall*)(void* self, void* edx, uint32_t flag, float delay);
    __try {
        reinterpret_cast<FlagFn>(mem::Rva(0x75C350))(reinterpret_cast<void*>(p), nullptr, kCancelFlag, 0.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TickShellPlayer(int i) {
    ShellRun& r = g_runs[i];
    const uintptr_t p = actors::InGame() ? actors::Player(i) : 0;
    uint32_t action = 0;
    if (!g_shellOn.load(std::memory_order_relaxed) || !p || !mem::Read(p + 0x10E0, action) || action != kReloadAction) {
        if (r.weapon) LOG("shell reload: player %d's reload over (%d shell(s), %d now)", i + 1, r.pushed, Count(r.record));
        r = ShellRun{};
        return;
    }
    if (r.player != p) {
        r = ShellRun{};
        r.player = p;
        uint16_t item = 0;
        r.record = HeldRecord(p, &item, &r.gun);
        r.weapon = r.record ? ShellWeaponOf(item) : nullptr;
        if (!r.weapon) return;
        for (int n = 0; n < kReloadLayers; ++n)
            if (LayerFrame(p, n) < 15.0f) r.motions[n] = LayerMotion(p, n);  // started with the reload
        if (r.gun && LayerFrame(r.gun, 0) < 15.0f) r.gunMotion = LayerMotion(r.gun, 0);
        r.last = LayerFrame(p, r.weapon->layer);
        r.aimHeld = i == 0 && GameButtonHeld(kAimButton, Player1Pad());
        LOG("shell reload: player %d reloads %04X (%d of %d), motion %X", i + 1, item, Count(r.record),
            Capacity(p, r.record), r.motions[r.weapon->layer]);
    }
    if (r.weapon && i == 0 && !r.cancelled) {  // a new press of the aim stops it
        const bool aim = GameButtonHeld(kAimButton, Player1Pad());
        if (aim && !r.aimHeld) {
            r.cancelled = true;
            AllowCancel(p);
            RequestAction(p, kAimBegin);
            uint32_t now = 0;
            mem::Read(p + 0x10E0, now);
            LOG("shell reload: aim pressed, reload stopped after %d push(es) (%d now, action %X)", r.pushed, Count(r.record), now);
        }
        r.aimHeld = aim;
    }
    if (r.cancelled) return;
    if (!r.weapon || !r.motions[r.weapon->layer] || LayerMotion(p, r.weapon->layer) != r.motions[r.weapon->layer]) return;
    const ShellWeapon& w = *r.weapon;
    const float last = r.last;
    float now = LayerFrame(p, w.layer);
    const auto crossed = [&](float mark) { return last < mark && now >= mark; };
    if (w.shellAt[3] > 0.0f) {  // four rounds natively (the revolvers)
        const int per = std::max(1, ShellsPerPush(w.item));
        const auto missing = [&] { return r.dry ? 0 : std::max(0, Capacity(r.player, r.record) - Count(r.record)); };
        const auto pushesLeft = [&] { return (missing() + per - 1) / per; };
        for (float at : w.shellAt)
            if (crossed(at) && missing() > 0 && !PushShell(r)) r.dry = true;
        bool jumped = false;
        if (r.pushed >= 1)
            for (int left = 0; left < 3 && !jumped; ++left)
                if (crossed(w.skipFrom[left]) && pushesLeft() == left) {
                    const float from = now;
                    now = w.skipTo[left] + (now - w.skipFrom[left]);
                    JumpTo(r, now);
                    jumped = true;
                    LOG("shell reload: %d more push(es), %d gesture(s) skipped (frame %.1f -> %.1f)", left, 3 - left, from, now);
                }
        if (!jumped && crossed(w.cycleTo) && pushesLeft() > 1) {
            now = w.cycleFrom + (now - w.cycleTo);
            JumpTo(r, now);
            LOG("shell reload: another push (%d missing), back to frame %.1f", missing(), now);
        }
        r.last = now;
        return;
    }
    if (w.lastShell > 0.0f) {  // the first push is the cycle, a second push gives the last round (the S75)
        const int per = std::max(1, ShellsPerPush(w.item));
        const auto missing = [&] { return r.dry ? 0 : std::max(0, Capacity(r.player, r.record) - Count(r.record)); };
        if (r.pushed == 0 && crossed(w.cycleFrom) && missing() <= per) {  // one push is enough: the second one
            const float from = now;
            now = w.cycleTo + (now - w.cycleFrom);
            JumpTo(r, now);
            LOG("shell reload: one push missing, the first skipped (frame %.1f -> %.1f)", from, now);
        } else {
            if (crossed(w.cycleShell) && !PushShell(r)) r.dry = true;
            const int left = missing();
            if (crossed(w.cycleTo) && left > per) {
                now = w.cycleFrom + (now - w.cycleTo);
                JumpTo(r, now);
                LOG("shell reload: another push (%d missing), back to frame %.1f", left, now);
            } else if (crossed(w.lastShell) && !PushShell(r)) {
                r.dry = true;
            }
        }
        r.last = now;
        return;
    }
    if (r.pushed == 0 && crossed(w.firstShell) && !PushShell(r)) r.dry = true;
    if (crossed(w.cycleFrom) && !Missing(r)) {  // full after the first: the second shell's cycle skipped
        now = w.cycleTo + (now - w.cycleFrom);
        JumpTo(r, now);
    } else {
        if (crossed(w.cycleShell) && !PushShell(r)) r.dry = true;
        if (crossed(w.cycleTo) && Missing(r)) {  // another shell's cycle
            now = w.cycleFrom + (now - w.cycleTo);
            JumpTo(r, now);
        }
    }
    r.last = now;
}

// Research (2026-11-04, the weapons' reloads and shots): with DataDir\reload_shots present, player 1's reloads (0x1F)
// and shots (0x1E) pictured every 3 frames of layer 1 at half size (shot_rl<n>_<frame x 10>.bmp; it costs frames).
struct ShotRun {
    uintptr_t player = 0;
    uint32_t action = 0;
    bool on = false;
    int index = 0;
    float next = 0.0f;
};
ShotRun g_shotRun;
int g_shotRuns = 0;

// Also the dodges (0x4E..0x56), every melee move (0x30..0x4D, 0x6F, 0xAC, 0xAD, the trainer's 0x81), the crossings
// (0x28..0x2E, 0x71..0x76, 0x7B..0x7F) and the events (0x80000000): the first person's turns and its ghost
// (2026-11-04), their frames on layer 0.
bool PicturedMove(uint32_t a) {
    return (a >= 0x4E && a <= 0x56) || (a >= 0x30 && a <= 0x4D) || a == 0x6F || a == 0xAC || a == 0xAD || a == 0x81 ||
           (a >= 0x28 && a <= 0x2E) || (a >= 0x71 && a <= 0x76) || (a >= 0x7B && a <= 0x7F) || a == 0x80000000u;
}

void TickResearchShots() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action) || (action != kReloadAction && action != 0x1E && !PicturedMove(action))) {
        g_shotRun = ShotRun{};
        return;
    }
    if (g_shotRun.player != p || g_shotRun.action != action) {
        g_shotRun = ShotRun{};
        g_shotRun.player = p;
        g_shotRun.action = action;
        g_shotRun.on = GetFileAttributesW((logx::DataDir() + L"\\reload_shots").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (g_shotRun.on) g_shotRun.index = ++g_shotRuns;
    }
    if (!g_shotRun.on) return;
    const float frame = LayerFrame(p, PicturedMove(action) ? 0 : 1);
    if (frame >= g_shotRun.next || frame < g_shotRun.next - 6.0f) {
        char label[32];
        sprintf_s(label, "rl%d_%04d", g_shotRun.index, static_cast<int>(frame * 10.0f));
        hooks::RequestShot(label, 2);
        g_shotRun.next = frame + 3.0f;
    }
}

// The Hydra fired with the Ithaca M37's motions (quick_knife.cpp, RE6 mode) pumps a fore-end it has not (user,
// 2026-11-04: "keep the barrel going up and down, cut the hand that pumps between them, for the Hydra only"; then "the
// grenade launcher and the Jail Breaker too", which take the Ithaca's motions with the iron sights). Recorded
// (research/shell_reload/hydra_fire.txt, action 0x1E, layers 0-5 and the gun's own motion 2 at one pace, layer 1 =
// 0x51E): the recoil 0-10, the barrel down to a lowered hold 10-38, the pump 39-57 (the only frames where the hands
// are not 44.5 cm apart), back up to the aim 57-88. 37.5 -> 57.1 cuts the pump (right hand 2.6 cm, left 9 cm apart,
// blended over 8 frames). The men's Ithaca list only (recorded on Chris; a woman's is not).
bool PumpLessItem(uint16_t item) {
    return item == 0x0119 || item == 0x0117 || item == 0x010C || (item >= 0x0125 && item <= 0x0127) ||
           (item >= 0x0139 && item <= 0x013B);
}
constexpr uint32_t kFireAction = 0x1E;
constexpr float kPumpFrom = 37.5f, kPumpTo = 57.1f, kPumpBlend = 8.0f;
struct FireCut {
    uintptr_t player = 0;
    float last = 0.0f;
    bool done = false;
};
FireCut g_fireCut;

void TickHydraFire() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    uint16_t item = 0;
    uintptr_t gun = 0;
    if (!p || !mem::Read(p + 0x10E0, action) || action != kFireAction || !HeldRecord(p, &item, &gun) ||
        !PumpLessItem(item) || !WeaponBankIsIthaca(p)) {
        g_fireCut = FireCut{};
        return;
    }
    const float now = LayerFrame(p, 1);
    if (g_fireCut.player != p || now < g_fireCut.last - 1.0f) {  // a new shot
        g_fireCut = FireCut{};
        g_fireCut.player = p;
    }
    if (!g_fireCut.done && g_fireCut.last < kPumpFrom && now >= kPumpFrom && now < kPumpTo) {
        const float target = kPumpTo + (now - kPumpFrom);
        for (int n = 0; n < kReloadLayers; ++n) {  // the fire's layers run in step with layer 1
            const float frame = LayerFrame(p, n);
            if (std::fabs(frame - now) < 1.0f) StartAt(p, n, LayerMotion(p, n), target + (frame - now), kPumpBlend);
        }
        if (gun && std::fabs(LayerFrame(gun, 0) - now) < 1.0f) mem::Write(Layer(gun, 0) + 0x18, target + (LayerFrame(gun, 0) - now));
        g_fireCut.done = true;
        LOG("shot with the Ithaca's motions: its pump cut for %04X (frame %.1f -> %.1f)", item, now, target);
        g_fireCut.last = target;
        return;
    }
    g_fireCut.last = now;
}

void TickShellReload() {
    for (int i = 0; i < 2; ++i) TickShellPlayer(i);
    TickHydraFire();
    TickResearchShots();
}
}  // namespace

bool ShellReloadWeapon(uint16_t item) { return ShellWeaponOf(item) != nullptr; }
bool EnsureReloadStepHook() { return EnsureStepHook(); }

void RegisterShellReload() {
    Add({ .id = "ammo.shell_reload", .name = "Rechargement cartouche par cartouche",
          .help = "Les fusils à pompe et les revolvers se rechargent une cartouche à la fois, comme dans RE4 et RE6 : "
                  "Chris met autant de cartouches qu'il en manque (une seule s'il n'en manque qu'une, vingt s'il en "
                  "manque vingt) avant de pomper, et chaque cartouche mise compte tout de suite : interrompu, le "
                  "rechargement garde celles déjà mises. Les cartouches mises à chaque geste se règlent par arme dans le "
                  "tableau ci-dessous (« Cartouches par geste », jamais plus qu'il n'en manque). Viser arrête le "
                  "rechargement. Pour l'instant : l'Ithaca M37, le M3, l'Hydra, le Jail Breaker, le fusil S75 et les revolvers "
                  "S&W M29 et M500.",
          .category = "Munitions", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureMagazineCapHook() || !EnsureStepHook())) return false;
              g_shellOn = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnFrame(TickShellReload);
    OnRestore([] {
        g_shellOn = false;
        SetMagazineCap(0, 0);
        for (ShellRun& r : g_runs) r = ShellRun{};
        g_fireCut = FireCut{};
    });
}
}  // namespace features
