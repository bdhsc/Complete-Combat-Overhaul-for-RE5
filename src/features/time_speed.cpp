// Game time speed (user, 2026-10-21). The clock singleton sFps [RVA 0xDB209C] (property names right after its vtable:
// mStopTimer, mTimeSpeed, mDeltaSec, mDeltaSecOrg, Fps): its update VA 0x7B3E80 sets +0x20 (the frame in 30 FPS frames,
// from sMain +0x68), +0x24 = +0x28 = that in seconds (/ sMain +0x38 = 30) and counts +0x30 down; +0x2C (1.0) is the
// time speed: every world unit's frame time is unit +0xFD4 x sFps +0x2C x unit +0x10 (uWork virtual VA 0x84F6A0, the
// only reader of +0x2C: ~17000 reads a second; write / read traces 2026-10-21). The menus are other units (uId*).
// Two options share it, the speed written = the game's x the two factors:
// - "Ralenti pendant l'inventaire" (inventory.slow_motion): while player 1's inventory is open, the slider (0 = time
//   stopped, written as 0.001: no division by zero, 1 = real time); player 1 keeps his time (at 0 the inventory cursor
//   no longer moved: its controller cOpeWepSlot [P+0x2654] belongs to him and counts his time), and the pause menu over
//   the open inventory gets the game's speed back (its navigation stopped too).
// - "Vitesse du temps" (game.time_dilation): its shortcut switches between the normal speed and the slider's
//   (0..2, 0 written as 0.001: time stopped, user 2026-10-25 to look at the rocket caught in Chris's hands), for
//   everything, player 1 included.
// - "Esquive parfaite" (perfect_dodge.cpp, user 2026-10-29): x0.25 for 4 s after a perfect dodge (StartDodgeSlowMotion),
//   player 1 included unless "Chris garde sa vitesse" (then his own time, as with the inventory).
// - "Vitesse du corps à corps" (combat.melee_speed, user 2026-10-24, x0.1..x3): while player 1 is in a melee attack
//   (wesker_moves.cpp Player1InMeleeAttack), below 1 the whole game slows down (a slow motion of the melee), above 1
//   player 1's own frame time is multiplied through the unit time hook, with the enemy's in a synchronised move only
//   (user: the neck break speeds the enemy up, a plain punch Chris alone; any character). A synchronised pair starts
//   together (the swaps.cpp SetMotion hook notes each unit's layer 0 starts): an enemy within 1.5 m (held against him) whose motion
//   started within 50 ms of player 1's, looked for during the move's first 150 ms; a hit's reaction starts at the
//   impact. Their lengths differ (the back escape: player 110 frames, Majini 180). Nothing else changes speed.
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace features {
bool Player1InventoryOpen();   // pickup.cpp
bool PauseMenuIsShown();       // pickup.cpp: the pause menu unit's slot 9 ran within 50 ms
bool EnsurePauseMenuWatch();   // pickup.cpp
bool Player1InMeleeAttack();   // wesker_moves.cpp
uintptr_t Player1SyncedMeleeTarget();  // wesker_moves.cpp: the enemy of a synchronised move, else 0
bool EnsureRocketTimeHooks();          // rocket_catch.cpp: the rockets in flight slowed with the world
bool StaminaAllowsSlowMotion();        // stamina.cpp: "Endurance : le ralenti consomme", 25 % at least
bool EnsureFireIntervalHook();         // fire_rate.cpp: the shots' timer counts real time, stretched by the time speed

namespace {
std::atomic<bool> g_inventoryOn{ false }, g_dilationOn{ false };
std::atomic<float> g_inventorySpeed{ 0.25f }, g_dilation{ 0.5f };
bool g_written = false;   // the trainer holds sFps +0x2C (render thread)
float g_gameSpeed = 1.0f; // its value before
std::atomic<uintptr_t> g_realTimeUnit{ 0 };  // player 1 while the inventory slows the world
std::atomic<float> g_realTimeFactor{ 1.0f }; // his speed meanwhile: the game's x the dilation
std::atomic<bool> g_meleeOn{ false };
std::atomic<float> g_meleeSpeed{ 1.5f };
constexpr int kFastUnits = 4;
std::atomic<uintptr_t> g_fastUnits[kFastUnits]{};  // player 1 and his synchronised enemies while a melee attack is sped up
std::atomic<float> g_fastFactor{ 1.0f };
std::atomic<float> g_dodgeSlow{ 1.0f };          // the perfect dodge's slow motion
std::atomic<ULONGLONG> g_dodgeSlowUntil{ 0 };    // until this real-time tick
std::atomic<bool> g_dodgeSlowPlayer{ false };    // player 1 at the game's speed meanwhile

// The last layer 0 motion start of the units seen by the SetMotion hook (game thread).
struct MotionStartEntry {
    uintptr_t unit = 0;
    ULONGLONG tick = 0;
};
std::mutex g_startLock;
MotionStartEntry g_starts[64];

ULONGLONG MotionStart(uintptr_t unit) {
    std::lock_guard lock(g_startLock);
    for (const MotionStartEntry& e : g_starts)
        if (e.unit == unit) return e.tick;
    return 0;
}

using UnitDeltaFn = float(__fastcall*)(void* unit, void* edx);
UnitDeltaFn g_unitDelta = nullptr;

std::atomic<uintptr_t> g_boostUnit{ 0 };  // one unit living faster (solo_scenario.cpp: the partner hurrying into the house)
std::atomic<float> g_boostFactor{ 1.0f };

float __fastcall HookUnitDelta(void* unit, void* edx) {
    const uintptr_t u = reinterpret_cast<uintptr_t>(unit);
    if (u && u == g_boostUnit.load(std::memory_order_relaxed)) return g_unitDelta(unit, edx) * g_boostFactor.load(std::memory_order_relaxed);
    if (u && u == g_realTimeUnit.load(std::memory_order_relaxed)) {
        float scale = 1.0f, step = 0.0f;
        if (mem::Read(u + 0xFD4, scale) && mem::Read(u + 0x10, step))
            return scale * step * g_realTimeFactor.load(std::memory_order_relaxed);
    }
    if (u)
        for (const auto& fast : g_fastUnits)
            if (u == fast.load(std::memory_order_relaxed)) return g_unitDelta(unit, edx) * g_fastFactor.load(std::memory_order_relaxed);
    return g_unitDelta(unit, edx);
}

bool EnsureUnitDeltaHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x44F6A0);
    // fld [ecx+0xFD4]; mov eax, [sFps]; fmul [eax+0x2C]; fmul [ecx+0x10]; ret
    const uint8_t expected[] = { 0xD9, 0x81, 0xD4, 0x0F, 0x00, 0x00, 0xA1, 0x9C, 0x20, 0x1B, 0x01, 0xD8, 0x48, 0x2C };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("unit frame time: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookUnitDelta), reinterpret_cast<void**>(&g_unitDelta));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("unit frame time hook installed at %p", target);
    return true;
}

uintptr_t TimeSpeedField() {
    uintptr_t fps = 0;
    return mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB209C)), fps) && fps ? fps + 0x2C : 0;
}

void ReleaseFast() {
    for (auto& fast : g_fastUnits) fast = 0;
}

uint32_t g_syncedAction = 0;            // player 1's action the enemies below go with (render thread)
uintptr_t g_synced[kFastUnits - 1] = {};

// Above x1 during a melee attack: player 1, and the live enemies of a synchronised move.
void TickFastMelee(bool melee, float speed) {
    const uintptr_t p = melee && speed > 1.0f ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action)) {
        ReleaseFast();
        g_syncedAction = 0;
        for (uintptr_t& e : g_synced) e = 0;
        return;
    }
    // Kept over the whole action (its motion may start again), searched again at each start of his motion.
    if (action != g_syncedAction) {
        g_syncedAction = action;
        for (uintptr_t& e : g_synced) e = 0;
    }
    const auto enemies = actors::Enemies();
    const ULONGLONG start = MotionStart(p), now = GetTickCount64();
    float at[3]{};
    if (start && now - start < 150 && mem::SafeRead(reinterpret_cast<void*>(p + 0x30), at, sizeof(at))) {
        for (uintptr_t e : enemies) {
            const ULONGLONG theirs = MotionStart(e);
            float pos[3]{};
            if (!theirs || (theirs > start ? theirs - start : start - theirs) > 50 ||
                !mem::SafeRead(reinterpret_cast<void*>(e + 0x30), pos, sizeof(pos)) ||
                std::hypot(pos[0] - at[0], pos[2] - at[2]) > 150.0f)
                continue;
            if (std::find(std::begin(g_synced), std::end(g_synced), e) != std::end(g_synced)) continue;
            for (uintptr_t& slot : g_synced)
                if (!slot) {
                    slot = e;
                    break;
                }
        }
    }
    uintptr_t units[kFastUnits] = { p };
    int n = 1;
    for (uintptr_t e : g_synced)
        if (e && std::find(enemies.begin(), enemies.end(), e) != enemies.end()) units[n++] = e;
    if (const uintptr_t thrown = Player1SyncedMeleeTarget(); thrown && n < kFastUnits &&
        std::find(units, units + n, thrown) == units + n && std::find(enemies.begin(), enemies.end(), thrown) != enemies.end())
        units[n++] = thrown;
    bool changed = false;
    for (int i = 0; i < kFastUnits; ++i) changed |= g_fastUnits[i].load() != units[i];
    if (changed) LOG("melee speed: x%.2f on player 1 and %d synchronised enem%s", speed, n - 1, n == 2 ? "y" : "ies");
    g_fastFactor = speed;
    for (int i = 0; i < kFastUnits; ++i) g_fastUnits[i] = units[i];
}

void Release() {
    g_realTimeUnit = 0;
    if (!g_written) return;
    if (const uintptr_t field = TimeSpeedField()) mem::Write(field, g_gameSpeed);
    g_written = false;
    LOG("time speed: %.2f back", g_gameSpeed);
}

// Every overlay frame (OnFrame).
void TickTime() {
    const bool inGame = actors::InGame();
    const uintptr_t field = inGame ? TimeSpeedField() : 0;
    const bool inventory = inGame && g_inventoryOn.load(std::memory_order_relaxed) && Player1InventoryOpen() &&
                           !PauseMenuIsShown();
    float dilation = g_dilationOn.load(std::memory_order_relaxed) ? g_dilation.load(std::memory_order_relaxed) : 1.0f;
    dilation = dilation < 0.001f ? 0.001f : dilation > 2.0f ? 2.0f : dilation;
    float slow = inventory ? g_inventorySpeed.load(std::memory_order_relaxed) : 1.0f;
    slow = slow < 0.001f ? 0.001f : slow > 1.0f ? 1.0f : slow;
    const bool melee = inGame && g_meleeOn.load(std::memory_order_relaxed) && Player1InMeleeAttack();
    float meleeSpeed = g_meleeSpeed.load(std::memory_order_relaxed);
    meleeSpeed = meleeSpeed < 0.1f ? 0.1f : meleeSpeed > 3.0f ? 3.0f : meleeSpeed;
    TickFastMelee(melee, meleeSpeed);
    if (melee && meleeSpeed < 1.0f) slow *= meleeSpeed;
    const bool dodgeSlow = inGame && GetTickCount64() < g_dodgeSlowUntil.load(std::memory_order_relaxed);
    if (dodgeSlow) slow *= std::clamp(g_dodgeSlow.load(std::memory_order_relaxed), 0.05f, 1.0f);
    if (!field || (dilation == 1.0f && slow == 1.0f)) {
        Release();
        return;
    }
    if (!g_written) {
        float speed = 1.0f;
        if (!mem::Read(field, speed) || speed <= 0.0f || speed > 4.0f) return;  // not a plain speed
        static bool fireHook = false;
        if (!fireHook) fireHook = EnsureFireIntervalHook();
        g_gameSpeed = speed;
        g_written = true;
        LOG("time speed: %.2f -> x%.3f (dilation %.2f, inventory / melee %.3f)", speed, dilation * slow, dilation, slow);
    }
    mem::Write(field, g_gameSpeed * dilation * slow);
    g_realTimeFactor = g_gameSpeed * dilation;
    g_realTimeUnit = inventory || (dodgeSlow && g_dodgeSlowPlayer.load(std::memory_order_relaxed)) ? actors::Player(0) : 0;
}
}  // namespace

// stamina.cpp: "Vitesse du temps" on and slowing the game.
bool TimeDilationSlowing() {
    return g_dilationOn.load(std::memory_order_relaxed) && g_dilation.load(std::memory_order_relaxed) < 0.999f;
}

// stamina.cpp: the slow motion stopped at 25 % of the stamina (its box unticked, as by the user).
void StopTimeDilation() {
    if (Feature* f = Find("game.time_dilation"); f && f->enabled) {
        Toast("Vitesse du temps : endurance à 25 %", false);
        SetEnabled(*f, false);
    }
}

// perfect_dodge.cpp: the world at `speed` for `seconds` of real time, player 1 too unless `playerKeepsSpeed`.
void StartDodgeSlowMotion(float speed, float seconds, bool playerKeepsSpeed) {
    g_dodgeSlow = speed;
    g_dodgeSlowPlayer = playerKeepsSpeed;
    g_dodgeSlowUntil = GetTickCount64() + static_cast<ULONGLONG>(std::max(0.0f, seconds) * 1000.0f);
    LOG("time speed: perfect dodge slow motion x%.2f for %.1f s%s", speed, seconds, playerKeepsSpeed ? " (not player 1)" : "");
}

// swaps.cpp's SetMotion hook: a unit's layer 0 motion starts now.
void NoteMotionStart(uintptr_t unit) {
    if (!unit) return;
    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(g_startLock);
    MotionStartEntry* oldest = &g_starts[0];
    for (MotionStartEntry& e : g_starts) {
        if (e.unit == unit) {
            e.tick = now;
            return;
        }
        if (e.tick < oldest->tick) oldest = &e;
    }
    oldest->unit = unit;
    oldest->tick = now;
}

// The world's time speed (sFps +0x2C: the game's x the options): rocket_catch.cpp slows the rockets, which fly a fixed
// step per frame.
float WorldTimeSpeed() {
    float speed = 1.0f;
    const uintptr_t field = TimeSpeedField();
    return field && mem::Read(field, speed) && speed >= 0.0f && speed <= 4.0f ? speed : 1.0f;
}

// The time speed player 1 lives at: sFps +0x2C (the game's x the options), or his own while the inventory slows the
// world. For the trainer's moves that count real time (movement.cpp: walking during actions, the pad's turn in the
// run; user, 2026-10-21: at x0.25 everything slowed but the walk while aiming).
float Player1TimeSpeed() {
    if (const uintptr_t unit = g_realTimeUnit.load(std::memory_order_relaxed); unit && unit == actors::Player(0))
        return g_realTimeFactor.load(std::memory_order_relaxed);
    float speed = 1.0f;
    const uintptr_t field = TimeSpeedField();
    return field && mem::Read(field, speed) && speed >= 0.0f && speed <= 4.0f ? speed : 1.0f;
}

// One unit's frame time multiplied (its moves, motions and thinking), 0 = none.
bool SetUnitTimeBoost(uintptr_t unit, float factor) {
    if (unit && !EnsureUnitDeltaHook()) return false;
    g_boostFactor = factor;
    g_boostUnit = unit;
    return true;
}

void RegisterTimeSpeed() {
    OnRestore([] { g_boostUnit = 0; });
    Feature& slow = Add({ .id = "inventory.slow_motion", .name = "Ralenti pendant l'inventaire",
                          .help = "Tant que l'inventaire est ouvert, le temps du jeu va à la vitesse du curseur : 0 = temps "
                                  "arrêté, 1 = temps réel, entre les deux = ralenti. Chris et l'inventaire gardent leur "
                                  "vitesse (un soin pris dans l'inventaire se joue normalement) ; le temps reprend dès "
                                  "que l'inventaire se ferme, et pendant le menu pause.",
                          .category = "Inventaire", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = 0.25f, .valueMin = 0.0f, .valueMax = 1.0f, .valueFormat = "%.2f" });
    Feature* const ps = &slow;
    slow.set = [ps](bool on) {
        if (on && (!EnsureUnitDeltaHook() || !EnsurePauseMenuWatch())) return false;
        if (on && !EnsureRocketTimeHooks()) LOG("inventory slow motion: the rockets in flight not slowed");
        g_inventorySpeed = ps->value;
        g_inventoryOn = on;
        return true;
    };
    slow.tick = [ps] { g_inventorySpeed.store(ps->value, std::memory_order_relaxed); };

    Feature& dilation = Add({ .id = "game.time_dilation", .name = "Vitesse du temps",
                              .help = "Cochée (ou avec son raccourci), tout le jeu va à la vitesse du curseur, Chris "
                                      "compris : 0 = temps arrêté, en dessous de 1 = ralenti, au-dessus = accéléré (2 = "
                                      "deux fois plus vite) ; décochée = vitesse normale. Le raccourci passe de l'une à "
                                      "l'autre.",
                              .category = "Jeu", .kind = Kind::Toggle,
                              .available = [] { return game::IsSupported(); },
                              .value = 0.5f, .valueMin = 0.0f, .valueMax = 2.0f, .valueFormat = "x%.2f" });
    Feature* const pd = &dilation;
    dilation.set = [pd](bool on) {
        // "Endurance : le ralenti consomme" (user, 2026-10-26): no slow motion under 25 % of the stamina; not wanted
        // any more, so the retry of persisted toggles does not turn it on later.
        if (on && pd->value < 0.999f && !StaminaAllowsSlowMotion()) {
            pd->wanted = false;
            Toast("Vitesse du temps : endurance sous 25 %", false);
            LOG("time speed: slow motion refused, stamina under 25 %%");
            return false;
        }
        if (on && !EnsureRocketTimeHooks()) LOG("time speed: the rockets in flight not slowed");
        g_dilation = pd->value;
        g_dilationOn = on;
        return true;
    };
    dilation.tick = [pd] { g_dilation.store(pd->value, std::memory_order_relaxed); };

    Feature& melee = Add({ .id = "combat.melee_speed", .name = "Vitesse du corps à corps",
                           .help = "Pendant les coups de corps à corps du joueur 1, quel que soit le personnage (ceux du jeu, "
                                   "la mêlée libre, les ripostes des parades, les désaisies ; pas les esquives), le curseur règle leur "
                                   "vitesse : au-dessus de 1, le personnage va plus vite, et l'ennemi avec lui seulement dans "
                                   "les mouvements synchronisés (neck breaker, prises, projections) ; en dessous de 1, c'est "
                                   "tout le jeu qui ralentit le temps du coup, un ralenti du corps à corps. Le reste du jeu "
                                   "garde sa vitesse.",
                           .category = "Mêlée", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = 1.5f, .valueMin = 0.1f, .valueMax = 3.0f, .valueFormat = "x%.2f" });
    Feature* const pm = &melee;
    melee.set = [pm](bool on) {
        if (on && !EnsureUnitDeltaHook()) return false;
        if (on && !EnsureRocketTimeHooks()) LOG("melee speed: the rockets in flight not slowed");
        g_meleeSpeed = pm->value;
        g_meleeOn = on;
        return true;
    };
    melee.tick = [pm] { g_meleeSpeed.store(pm->value, std::memory_order_relaxed); };
    OnFrame(TickTime);
    OnRestore([] {
        ReleaseFast();
        Release();
    });
}
}  // namespace features
