// Enemies killed by a move that is not theirs (user, 2026-10-25: a neck break done on anyone through the free melee or a
// melee swap, the same for every one-hit kill such as Sheva's scissors or throat cut, can lock the game: Wesker dead on
// the ground with his AI gone, the chainsaw Majini locked in its break state without dropping its key, the Mercenaries
// Executioner never counted dead so the next one never comes). Research first (diag.death_log): every enemy whose health
// falls to 0 is followed 8 s - its class, actions (with their class names), layer 0 motions, health - with what the
// players were doing, into trainer.log.
// Found (2026-10-25, Chris's Reunion neck break on Wesker): the move puts its target in the victim's side of the
// player's synchronised event, action 0x80000000 with the player's motion list lent as its bank F (Wesker: em80 bank F
// = pawn\pl\pl00\motion\pl00action_reunion, motion 0xF1D, 281 frames), and nothing more: a Majini dies at its end, an
// enemy that does not plan it stays on its last frame for good (Wesker: health untouched, AI gone).
// Forced deaths handled (always: no option since 2026-10-26, user - unchecked by mistake, a neck break or a throw the
// game does not plan soft-locked the game; it was "Morts forcées gérées", enemy.forced_death_fix): an enemy still in
// that event 0.5 s after its motion
// ended is handed to its own way out - Wesker gets up (his getup after the native counter, wesker_moves.cpp) with his
// give-up gauge at 0 so he leaves the fight (user: "only Wesker must survive and flee"; Jill dead is a game over, not a
// lock), any other dies its own death: health 0 and
// the first death action of its table by class name ("DieLie" lying first, then "Die", not "DieBomb" / "DieIce" /
// "DieFire"), so it drops its item and counts as killed (Mercenaries: mercenaries.cpp reports the kill).
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace features {
const char* UnitClassName(uintptr_t object);  // parry.cpp: its DTI class name
bool IsMajiniEnemy(uintptr_t enemy);          // parry.cpp
bool WeskerForcedGetup(uintptr_t wesker);     // wesker_moves.cpp
bool WeskerKeepsHealth(uintptr_t enemy);      // wesker_boss.cpp: "Wesker n'abandonne jamais"
uintptr_t WeskerThrownUnit();                 // wesker_moves.cpp: the enemy in Jill's throw
uintptr_t WeskerThrowOther();                 // wesker_moves.cpp: the enemy of Jill's throw it has no throw for

namespace {
constexpr uintptr_t kWeskerVtable = 0xB8BE60;  // uEm80 (RVA)
// Lost in Nightmares' Wesker (user, 2026-10-26: the neck break left him lying for good, a soft lock): uEmA2, the
// uEm80 actions in the same slots (cEm80Act*, DmgS 0x58-0x63) and his own from 0x7B (cEmA2Act*), 8500 health, his
// motions from pawn\em\ema2\motion\ema2 (em80's slots for the shared moves, its getups too); +0x1FA8 is
// 5000 at his start (uEm80's give-up gauge there, not checked for him). Out of the event: 1 health and the forced
// getup from his belly (wesker_moves.cpp), his AI then goes on (it asks his defeat LoseBgn 0x94 by itself).
constexpr uintptr_t kLinWeskerVtable = 0xB8C0B0;  // uEmA2 (RVA)
// The final fight's Wesker (user, 2026-10-26: the neck break left him lying for good, the fight never ended): uEm81,
// uEm80's actions and motions (em80 bank 0), 30000 health; that fight ends by its own events, not his health nor a
// give-up gauge. Out of the event: his health at 1, the forced getup from his belly, then on his knees for the
// injection (user: Hiza 0x67, wesker_moves.cpp).
constexpr uintptr_t kFinalWeskerVtable = 0xB8C300;  // uEm81 (RVA)
// The Uroboros (user, 2026-10-30: "neck broken it stayed as if alive, the game did not go on"): its deaths are
// cEm40ActDead 0x0E-0x11 / 0x33 (init RVA 0x4B3020, update RVA 0x4AC570 by its slot: 0x0E dies where it is, 0x0F-0x11
// are placed at fixed points with their events, 0x33 placed with event 0x2152). The boss uEm41 (32767 health, never
// hurt) ends by its update (RVA 0x4B8210): its three stage objects [+0x2120 / +0x2124 / +0x2128] +0x18DC & 2 set, it
// asks Dead 0x33 (0, 0, -1, 0), sets +0x2181 and reports its kill (RVA 0x322070 on [aGame]'s RVA 0x368C10() object,
// (unit, -1, -1)): the same out of the event. The first one (uEm40) dies its own death 0x0E (not seen yet).
constexpr uintptr_t kUroborosVtable = 0xB7BD08, kUroborosBossVtable = 0xB7D160;  // uEm40, uEm41 (RVA)
constexpr uint32_t kEventAction = 0x80000000;
// A class name from its GetDTI (vtable slot 4, `mov eax, dti; ret`), read without calling it.
const char* DtiName(uintptr_t object) {
    uintptr_t vtable = 0, fn = 0, dti = 0, name = 0;
    uint8_t code[6]{};
    if (!object || !mem::Read(object, vtable) || !mem::Read(vtable + 16, fn) ||
        !mem::SafeRead(reinterpret_cast<void*>(fn), code, sizeof(code)) || code[0] != 0xB8 || code[5] != 0xC3)
        return "?";
    std::memcpy(&dti, code + 1, 4);
    if (!mem::Read(dti + 4, name) || !name || IsBadStringPtrA(reinterpret_cast<const char*>(name), 64)) return "?";
    return reinterpret_cast<const char*>(name);
}

// The action object of `action` in a unit's table (+0x10D0 count, +0x10DC objects).
const char* ActionName(uintptr_t unit, uint32_t action) {
    uint32_t count = 0;
    uintptr_t table = 0, object = 0;
    if (action >= 0x1000 || !mem::Read(unit + 0x10D0, count) || action >= count || !mem::Read(unit + 0x10DC, table) ||
        !table || !mem::Read(table + 4 * action, object))
        return "?";
    return DtiName(object);
}

struct Watched {
    DWORD since = 0;
    uint32_t action = 0xFFFFFFFF, motion = 0xFFFFFFFF;
    int16_t health = 0;
};

std::atomic<bool> g_deathLog{ false };
std::unordered_map<uintptr_t, int16_t> g_lastHealth;  // render thread
std::unordered_map<uintptr_t, Watched> g_watched;

void LogPlayers(const char* when) {
    for (int i = 0; i < 2; ++i) {
        const uintptr_t p = actors::Player(i);
        uint32_t action = 0, motion = 0;
        if (!p || !mem::Read(p + 0x10E0, action)) continue;
        mem::Read(p + 0x384, motion);
        LOG("death log: %s player %d action %X (%s) motion %X", when, i + 1, action, ActionName(p, action), motion & 0xFFFF);
    }
}

void TickDeathLog() {
    if (!g_deathLog.load(std::memory_order_relaxed) || !actors::InGame()) {
        g_lastHealth.clear();
        g_watched.clear();
        return;
    }
    const std::vector<uintptr_t> enemies = actors::Enemies();
    std::unordered_map<uintptr_t, int16_t> health;
    for (uintptr_t e : enemies) {
        int16_t hp = 0;
        if (!mem::Read(e + actors::off::Health, hp)) continue;
        health[e] = hp;
        const auto last = g_lastHealth.find(e);
        if (last != g_lastHealth.end() && last->second > 0 && hp <= 0 && !g_watched.count(e)) {
            g_watched[e] = Watched{ GetTickCount() | 1 };
            LOG("death log: %s %08X health %d -> %d", UnitClassName(e), static_cast<unsigned>(e), last->second, hp);
            LogPlayers("at its death,");
        }
    }
    for (auto it = g_watched.begin(); it != g_watched.end();) {
        const uintptr_t e = it->first;
        Watched& w = it->second;
        if (!health.count(e)) {
            LOG("death log: %08X left the enemy list after %u ms", static_cast<unsigned>(e), GetTickCount() - w.since);
            it = g_watched.erase(it);
            continue;
        }
        if (static_cast<int32_t>(GetTickCount() - w.since) > 8000) {
            LOG("death log: %08X still there after 8 s (action %X, health %d)", static_cast<unsigned>(e), w.action, w.health);
            it = g_watched.erase(it);
            continue;
        }
        uint32_t action = 0, motion = 0;
        mem::Read(e + 0x10E0, action);
        mem::Read(e + 0x384, motion);
        motion &= 0xFFFF;
        w.health = health[e];
        if (action != w.action || motion != w.motion) {
            w.action = action;
            w.motion = motion;
            LOG("death log: %08X +%u ms action %X (%s) motion %X health %d", static_cast<unsigned>(e),
                GetTickCount() - w.since, action, ActionName(e, action), motion, w.health);
        }
        ++it;
    }
    g_lastHealth = std::move(health);
}

// ---- The fix.
std::atomic<bool> g_fixOn{ true };
// Its death starting on the ground (user, 2026-10-25: the Executioner died natively, exploded and dropped its item, but
// stood up a second first - its Die plays a standing death): the first layer 0 motion its death asks becomes the victim
// motion it lies in, from its last frame, so the death goes on at once (SetMotion hook, swaps.cpp: blend a, start b).
std::atomic<uintptr_t> g_deathUnit{ 0 };
std::atomic<uint32_t> g_deathMotion{ 0 };
std::atomic<float> g_deathStart{ 0.0f };
std::atomic<DWORD> g_deathTick{ 0 };
std::unordered_map<uintptr_t, DWORD> g_endedSince;  // render thread: enemies whose victim motion ended, since when
std::unordered_map<uintptr_t, bool> g_handled;      // handled once per stay in the event
// Jill (uEm83, uEm84 in the fight's second part) plays her own death motion: its end brings the game over (user,
// 2026-10-25: shot dead she ends the game; neck broken, her DieNormal 0x62 on the victim motion's last frame locked
// it). Left so, her death is asked again without that (once) after 1 s.
std::unordered_map<uintptr_t, DWORD> g_jillStuckSince;
std::unordered_map<uintptr_t, bool> g_jillRedone;

bool IsJill(uintptr_t unit) {
    const char* name = UnitClassName(unit);
    return name && (std::strncmp(name, "uEm83", 5) == 0 || std::strncmp(name, "uEm84", 5) == 0);
}

bool IsClass(uintptr_t unit, uintptr_t vtableRva) {
    uintptr_t vtable = 0;
    return mem::Read(unit, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(vtableRva));
}

// Its bank F holds a player's motion list (pawn\pl\...): the victim's side of a player's move.
bool PlayerListLent(uintptr_t unit) {
    uintptr_t list = 0;
    char path[64]{};
    return mem::Read(unit + 0xA88 + 4 * 0xF, list) && list &&
           mem::SafeRead(reinterpret_cast<void*>(list + 4), path, sizeof(path) - 1) && std::strstr(path, "\\pl\\");
}

// Its own death action, by class name: the plain "...ActDie" first, then "DieLie", then another "Die" (not bomb / ice /
// fire), then the first "ActDead" (the Uroboros), -1 if none. The chainsaw Majini's DieLie 0x20 is no death (user,
// 2026-10-25: it got up enraged with 1500 health, and a second neck break left it where it had first died, no item); its
// Die 0x1F is (Jill's throw uses it).
int DeathAction(uintptr_t unit) {
    uint32_t count = 0;
    if (!mem::Read(unit + 0x10D0, count) || count > 0x200) return -1;
    int lying = -1, other = -1, dead = -1;
    for (uint32_t i = 0; i < count; ++i) {
        const char* name = ActionName(unit, i);
        if (dead < 0 && std::strstr(name, "ActDead")) dead = static_cast<int>(i);
        const char* die = std::strstr(name, "ActDie");
        if (!die) continue;
        if (std::strcmp(die, "ActDie") == 0) return static_cast<int>(i);
        if (lying < 0 && std::strstr(die, "DieLie")) lying = static_cast<int>(i);
        if (other < 0 && !std::strstr(die, "Bomb") && !std::strstr(die, "Ice") && !std::strstr(die, "Fire")) other = static_cast<int>(i);
    }
    return lying >= 0 ? lying : other >= 0 ? other : dead;
}

// `first`: the request's first argument (1 for ours; the Uroboros asks its end with 0).
bool RequestUnit(uintptr_t unit, int action, int first = 1) {
    using RequestFn = bool(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(unit, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return false;
    __try {
        return reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(unit), nullptr, action, first, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game's kill report without a hit (the Uroboros boss's own, RVA 0x322070: every player, marks +0x191C once).
void ReportKill(uintptr_t unit) {
    using ManagerFn = void*(__fastcall*)(void* game, void* edx);
    using ReportFn = void(__fastcall*)(void* manager, void* edx, uintptr_t unit, int players, int b);
    void* game = nullptr;
    uint8_t reported = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE34578)), game) || !game || !mem::Read(unit + 0x191C, reported) ||
        reported)
        return;
    __try {
        void* manager = reinterpret_cast<ManagerFn>(mem::Rva(0x368C10))(game, nullptr);
        if (manager) reinterpret_cast<ReportFn>(mem::Rva(0x322070))(manager, nullptr, unit, -1, -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("forced death: kill report of %08X failed", static_cast<unsigned>(unit));
    }
}

void HandleStuckVictim(uintptr_t e) {
    if (IsClass(e, kWeskerVtable)) {
        // His give-up gauge at 0, and unless "Wesker n'abandonne jamais" his health at 1: with "Wesker s'entête" (the
        // gauge kept) he leaves at 1 health (user, 2026-10-25); the gauge at 0 alone with his health full kept him.
        mem::Write<int32_t>(e + 0x1FA8, 0);
        const bool keeps = WeskerKeepsHealth(e);
        if (!keeps) mem::Write<int16_t>(e + actors::off::Health, 1);
        LOG("forced death: Wesker %08X left lying, getup %s, give-up gauge 0%s", static_cast<unsigned>(e),
            WeskerForcedGetup(e) ? "asked" : "refused", keeps ? " (he never gives up: health kept)" : ", health 1");
        return;
    }
    if (IsClass(e, kFinalWeskerVtable)) {
        // His health at 1 (user, 2026-10-26: kneeling with his health high he threw Sheva off before the injection;
        // Hiza's and Pinion's inits read his health %: the kneeling loops and the hold's length).
        int16_t health = 0;
        mem::Read(e + actors::off::Health, health);
        mem::Write<int16_t>(e + actors::off::Health, 1);
        const bool getup = WeskerForcedGetup(e);
        LOG("forced death: final Wesker %08X left lying, getup %s (health %d -> 1)%s", static_cast<unsigned>(e),
            getup ? "asked" : "refused", health, getup ? "" : (RequestUnit(e, 0x59) ? ", his DmgS asked" : ", his DmgS refused"));
        return;
    }
    if (IsClass(e, kLinWeskerVtable)) {
        // His DmgS alone stood him up at once (user, 2026-10-26: "he teleports standing"): the getup from his belly
        // first (his ema2 list keeps em80's slots).
        mem::Write<int16_t>(e + actors::off::Health, 1);
        const bool getup = WeskerForcedGetup(e);
        LOG("forced death: Lost in Nightmares' Wesker %08X left lying, health 1, getup %s%s", static_cast<unsigned>(e),
            getup ? "asked" : "refused", getup ? "" : (RequestUnit(e, 0x59) ? ", his DmgS asked" : ", his DmgS refused"));
        return;
    }
    if (IsClass(e, kUroborosBossVtable)) {  // its own end (its health untouched, as the game's)
        const bool asked = RequestUnit(e, 0x33, 0);
        mem::Write<uint8_t>(e + 0x2181, 1);
        ReportKill(e);
        LOG("forced death: Uroboros boss %08X left lying, its end Dead 0x33 %s, kill reported", static_cast<unsigned>(e),
            asked ? "asked" : "refused");
        return;
    }
    const int die = DeathAction(e);
    if (die < 0) {
        LOG("forced death: %s %08X left lying, no death action of its own", UnitClassName(e), static_cast<unsigned>(e));
        return;
    }
    if (!IsJill(e)) {
        uint32_t motion = 0;
        float length = 0.0f;
        mem::Read(e + 0x384, motion);
        mem::Read(e + 0x39C, length);
        g_deathMotion = motion & 0xFFFF;
        g_deathStart = length > 1.0f ? length - 1.0f : 0.0f;
        g_deathTick = GetTickCount() | 1;
        g_deathUnit = e;
    }
    mem::Write<int16_t>(e + actors::off::Health, 0);
    LOG("forced death: %s %08X left lying, its death %X (%s) %s", UnitClassName(e), static_cast<unsigned>(e), die,
        ActionName(e, die), RequestUnit(e, die) ? "asked" : "refused");
    if (IsClass(e, kUroborosVtable)) ReportKill(e);
}

void TickForcedDeaths() {
    if (!g_fixOn.load(std::memory_order_relaxed) || !actors::InGame()) {
        g_endedSince.clear();
        g_handled.clear();
        g_jillStuckSince.clear();
        g_jillRedone.clear();
        return;
    }
    const DWORD now = GetTickCount();
    std::unordered_map<uintptr_t, DWORD> ended;
    std::unordered_map<uintptr_t, bool> handled;
    std::unordered_map<uintptr_t, DWORD> jillStuck;
    for (uintptr_t e : actors::Enemies()) {
        uint32_t action = 0, motion = 0;
        float frame = 0, length = 0;
        int16_t health = 0;
        if (!mem::Read(e + 0x10E0, action)) continue;
        // Jill dead on a victim motion (bank F) at its end: her death again, her own motion.
        if (action != kEventAction && IsJill(e) && mem::Read(e + actors::off::Health, health) && health <= 0 &&
            std::strstr(ActionName(e, action), "Die") && mem::Read(e + 0x384, motion) && ((motion >> 8) & 0xFF) == 0xF &&
            !g_jillRedone.count(e)) {
            const auto it = g_jillStuckSince.find(e);
            const DWORD since = it != g_jillStuckSince.end() ? it->second : now;
            jillStuck[e] = since;
            if (now - since >= 1000) {
                g_jillRedone[e] = true;
                LOG("forced death: Jill %08X stuck in her death %X on motion %X, asked again: %s", static_cast<unsigned>(e),
                    action, motion & 0xFFFF, RequestUnit(e, static_cast<int>(action)) ? "asked" : "refused");
            }
            continue;
        }
        if (action != kEventAction) continue;  // out of the event: handled again later
        mem::Read(e + 0x384, motion);
        mem::Read(e + 0x398, frame);
        mem::Read(e + 0x39C, length);
        if (((motion >> 8) & 0xFF) != 0xF || length <= 0 || frame < length - 1.0f || IsMajiniEnemy(e) || !PlayerListLent(e))
            continue;
        if (g_handled.count(e)) {
            handled[e] = true;
            continue;
        }
        const auto it = g_endedSince.find(e);
        const DWORD since = it != g_endedSince.end() ? it->second : now;
        if (now - since < 500) {
            ended[e] = since;
            continue;
        }
        handled[e] = true;
        HandleStuckVictim(e);
    }
    g_endedSince.swap(ended);
    g_handled.swap(handled);
    g_jillStuckSince.swap(jillStuck);
}

// ---- Flying enemies (user, 2026-10-25: the neck break and Jill's throw "do not select the flying enemies"; hit by the
// moves they only flinched, not held by Chris, not killed): the Kipepeo (uEm0c, cEm0cActHover: 3.1 m above Chris when
// the free melee looked for its target, which took none more than 1.5 m above or below him), and any enemy with a
// "Hover" or "Fly" action. They are chosen up to 4 m above him (swaps.cpp, wesker_moves.cpp). Such a target of player
// 1's move that is not in its victim's event (none is planned for it) is held in front of him (his reach point P+0x2AD0,
// 1.1 m up) while the move plays and, after a synchronised kill (his motion in his event bank F, or Jill's throw), dies
// its own death (DeathAction: its DieBlow), the kill credited to player 1; the same for any target still not the game's
// victim 250 ms into his synchronised motion (the dogs, user: "surely the same"). Not Jill's throw on an enemy it has no
// throw for (held and killed at first): "a throw that cannot hold its enemy must not force its death, only its damage"
// (user, 2026-10-25: the dogs one-shot, the Nightmare DLC's enemies killed at once).
std::unordered_map<uintptr_t, bool> g_flyerClass;  // by vtable
uintptr_t g_heldFlyer = 0;
bool g_flyerKill = false;
DWORD g_syncSince = 0;  // player 1's synchronised motion (bank F) started then

bool IsFlyer(uintptr_t e) {
    uintptr_t vtable = 0;
    if (!e || !mem::Read(e, vtable)) return false;
    if (const auto it = g_flyerClass.find(vtable); it != g_flyerClass.end()) return it->second;
    uint32_t count = 0;
    bool flyer = false;
    if (mem::Read(e + 0x10D0, count) && count < 0x200)
        for (uint32_t i = 0; i < count && !flyer; ++i) {
            const char* name = ActionName(e, i);
            flyer = std::strstr(name, "Hover") || std::strstr(name, "ActFly");
        }
    g_flyerClass[vtable] = flyer;
    return flyer;
}

bool PlayerMove(uint32_t action) {
    return (action >= 0x30 && action <= 0x4D) || action == 0x6F || action == 0x81 || action == 0xAC || action == 0xAD;
}

void TickFlyerTarget() {
    const uintptr_t p = g_fixOn.load(std::memory_order_relaxed) && actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action) || !PlayerMove(action)) {
        const uintptr_t e = g_heldFlyer;
        uint32_t its = 0;
        // Alive or left at 0 health by the move's own hit without a reaction: not already in a death of its own.
        if (p && e && g_flyerKill && mem::Read(e + 0x10E0, its) && !std::strstr(ActionName(e, its), "Die")) {
            const auto enemies = actors::Enemies();
            if (std::find(enemies.begin(), enemies.end(), e) != enemies.end()) {
                uint8_t pad = 0;
                if (mem::Read(p + 0x2D9C, pad)) mem::Write<int32_t>(e + 0x136C, pad);
                mem::Write<int16_t>(e + actors::off::Health, 0);
                const int die = DeathAction(e);
                LOG("forced death: %s %08X killed by player 1's move, its death %X (%s) %s", UnitClassName(e),
                    static_cast<unsigned>(e), die, die >= 0 ? ActionName(e, die) : "none",
                    die >= 0 && RequestUnit(e, die) ? "asked" : "not asked");
            }
        }
        g_heldFlyer = 0;
        g_flyerKill = false;
        g_syncSince = 0;
        return;
    }
    uint32_t motion = 0;
    const bool synced = mem::Read(p + 0x384, motion) && ((motion >> 8) & 0xF) == 0xF;
    if (!synced) g_syncSince = 0;
    else if (!g_syncSince) g_syncSince = GetTickCount() | 1;
    // The game's victim is in its event within a few frames of his synchronised motion's start.
    const bool notVictim = synced && static_cast<int32_t>(GetTickCount() - g_syncSince) > 250;
    if (WeskerThrowOther()) return;  // Jill's throw on an enemy it has no throw for: its damage only
    const uintptr_t thrown = WeskerThrownUnit();
    uintptr_t target = thrown;
    if (!target) mem::Read(p + 0x2DA4, target);
    uint32_t its = 0;
    if (!g_heldFlyer) {
        // Only for a synchronised move (his motion in bank F: the neck break...; user, 2026-11-06: "the parasites freeze
        // when Chris hits them" - his plain blows and Wesker's dash 0x48 held them in front of him the whole move).
        int16_t health = 0;
        if (!synced || !target || target == thrown || (!IsFlyer(target) && !notVictim) ||
            !mem::Read(target + actors::off::Health, health) || health <= 0 || !mem::Read(target + 0x10E0, its) ||
            its == kEventAction)
            return;
        g_heldFlyer = target;
        LOG("forced death: %s %08X (%s) held in front of player 1 during his move %X", UnitClassName(target),
            static_cast<unsigned>(target),
            IsFlyer(target) ? "flying" : "not the game's victim", action);
    }
    const uintptr_t e = g_heldFlyer;
    if (mem::Read(e + 0x10E0, its) && its == kEventAction) {  // the game's own victim after all
        g_heldFlyer = 0;
        return;
    }
    if (synced) g_flyerKill = true;
    float reach[3]{}, feet[3]{};
    if (mem::SafeRead(reinterpret_cast<void*>(p + 0x2AD0), reach, sizeof(reach)) &&
        mem::SafeRead(reinterpret_cast<void*>(p + 0x30), feet, sizeof(feet))) {
        reach[1] = feet[1] + (IsFlyer(e) ? 110.0f : 0.0f);
        mem::SafeWrite(reinterpret_cast<void*>(e + 0x30), reach, sizeof(reach));
    }
}
}  // namespace

// swaps.cpp / wesker_moves.cpp: an enemy that flies (taken as a melee target up to 4 m above player 1).
bool FlyingEnemy(uintptr_t enemy) { return IsFlyer(enemy); }

// The actor SetMotion hook (swaps.cpp): the forced death's first layer 0 motion lies as the victim did.
void ForcedDeathMotion(uintptr_t unit, uint32_t layer, uint32_t& motion, uint32_t& blend, uint32_t& start) {
    if (layer != 0 || !unit || unit != g_deathUnit.load(std::memory_order_relaxed)) return;
    g_deathUnit = 0;
    if (static_cast<int32_t>(GetTickCount() - g_deathTick.load()) > 500) return;
    const float zero = 0.0f, from = g_deathStart.load();
    LOG("forced death: %08X death motion %X -> %X from frame %.0f (on the ground)", static_cast<unsigned>(unit), motion,
        g_deathMotion.load(), from);
    motion = g_deathMotion.load();
    std::memcpy(&blend, &zero, sizeof(blend));
    std::memcpy(&start, &from, sizeof(start));
}

// wesker_moves.cpp: an enemy killed by the trainer that has no reaction of its own there plays its death action.
bool RequestOwnDeath(uintptr_t unit) {
    const int die = DeathAction(unit);
    const bool asked = die >= 0 && RequestUnit(unit, die);
    LOG("forced death: %s %08X killed, its death %X (%s) %s", UnitClassName(unit), static_cast<unsigned>(unit), die,
        die >= 0 ? ActionName(unit, die) : "none", asked ? "asked" : "not asked");
    return asked;
}

void RegisterForcedDeath() {
    OnFrame(TickForcedDeaths);
    OnFrame(TickFlyerTarget);
    Add({ .id = "diag.death_log", .name = "Journal des morts d'ennemis (recherche)",
          .help = "Écrit dans trainer.log chaque ennemi dont les PV tombent à 0, puis ses actions et animations pendant "
                  "8 s, avec ce que font les joueurs. Sert à gérer les mises à mort forcées (neck break...) sur les "
                  "ennemis qui ne les prévoient pas.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_deathLog = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnFrame(TickDeathLog);
}
}  // namespace features
