// The free melee's table (user, 2026-10-30: "for each attack a table like the weapons' one"): each sub-tab of Mêlée >
// Mêlée libre lists its attacks with their shortcuts and, per attack:
// - the reaction: the move's own, blown off (Chris's punch), the recoil of an Arm Front (with its head flash prompt) or a
//   flinch without prompt (a knife hit on the head);
// - the damage, a multiplier of the move's own (x1 = as it is);
// - the critical: the game's, or the percent of hits given the game's head-blow flag (hit +0x3C bit 0x10000: the
//   Majini damage computation VA 0x860450 then skips its head test and blows the head, as Finisher Front does; the
//   bosses' own computations only add damage - user);
// - one-hit kill (as the neck break; Wesker and Jill keep 1 health, as the neck break leaves them);
// - a back-up move (user: "a neck break on the Executioner: Chris plays Finisher Front instead"): with "Utiliser les
//   back-up" (Mêlée > Échanges) a move whose back-up is not "Par défaut" is refused on every enemy but the Majini and
//   the fat Majini whose parasite is not out (the Executioners, the Gatling, chainsaw and giant Majini, lickers, Reapers,
//   dogs, parasites, Uroboros, Wesker, Jill, the bosses...) and its back-up is played instead, from the free melee, a
//   melee swap or the game's prompt; unticked, every move goes on anyone as before;
// - "Rétablir": the move's settings back to the game's.
// The game's moves (Chris, Sheva... and the Général sub-tab's, the played character's) are tuned in the enemies' hit
// hooks (knife.cpp) whenever player 1 plays them, from a shortcut or the game's prompt (user); the trainer's own moves
// (Wesker, Jill, Leon, HUNK) in their strike (wesker_moves.cpp), where a critical, without the game's hit, replays the
// game's head blow on a Majini that has one (QueueHeadBlow) and kills it, x5 damage on the others.
// Saved in DataDir\melee_moves.txt (one line per tuned move).
#include "features/melee_tuning.hpp"

#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "common/settings.hpp"
#include "ui/sliders.hpp"

#include <windows.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace features {
bool EnsureEnemyDamageHook();  // knife.cpp
bool EnsureEnemyOnHitHook();   // knife.cpp
bool EnsureEnemyReactionHook();  // knife.cpp: the Majini reaction function
bool EnsureEnemyActionSetupHook();  // knife.cpp: the flinch's draw at its setup
bool EnsurePlayerDamageHook();      // player.cpp: the actors' SubHealth (the back escape's damage)
bool EnsurePadQueryHooks();         // quick_knife.cpp: its game thread tick (the head blows)
bool BetaBackThrowOn();             // beta_anims.cpp: the back escape is the beta throw
bool IsMajiniEnemy(uintptr_t enemy);                // parry.cpp: a Majini (cEm10 actions)
uintptr_t WeskerThrownUnit();                       // wesker_moves.cpp: the enemy the trainer holds in step (throws, pairs)
const char* UnitClassName(uintptr_t object);        // parry.cpp: its DTI class name
// swaps.cpp
std::string TuningIdOf(const std::string& featureId);           // a Général standard move: the played character's
std::string Player1NativeMoveId(uintptr_t p, uint32_t action);  // the free melee move player 1's action plays
std::string Player1MovesetName();                               // the played character's moves (Chris...)
// wesker_moves.cpp
bool ReactToMeleeHit(uintptr_t p, uintptr_t enemy, int reaction, bool ownDeath);
bool KillLeavesOne(uintptr_t enemy);              // Wesker, Jill: 1 health left, as the neck break
int CombatMoveKind(const std::string& featureId);  // -1 not its move, 0 no hit, 1 hits, 2 a throw
int CombatMoveDamage(const std::string& featureId);  // its own damage, -1 not its move
int NativeMovePower(const std::string& featureId);   // swaps.cpp: a game move's power, -1 none

namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<std::string, MoveTuning> g_tunings;  // by tuning id
std::atomic<bool> g_anyHitTuned{ false };              // a move's hits differ from the game's: the hooks are needed
std::atomic<bool> g_useBackup{ false };                // "Utiliser les back-up"
std::atomic<bool> g_inBackup{ false };                 // a back-up playing: its own requests go through

std::wstring TablePath() { return logx::DataDir() + L"\\melee_moves.txt"; }

// A move's settings before any change (user, 2026-10-31: HUNK's neck break kills at once, Leon's suplex is critical 85 %
// of the time; its 600 damage is its own); "Rétablir" gives them back.
MoveTuning DefaultTuningOf(const std::string& id) {
    MoveTuning t;
    if (id == "melee.free_hunk_neck_break") t.kill = true;
    else if (id == "melee.free_leon_suplex") t.critical = 85.0f;
    return t;
}

bool SameTuning(const MoveTuning& a, const MoveTuning& b) {
    return a.reaction == b.reaction && a.damage == b.damage && a.critical == b.critical && a.kill == b.kill &&
           a.backupId == b.backupId && a.followTurns == b.followTurns;
}

// Kept in the file when it differs from the move's defaults.
bool Stored(const std::string& id, const MoveTuning& t) { return !SameTuning(t, DefaultTuningOf(id)); }

void RefreshAny() {
    bool any = false;
    for (const auto& [id, t] : g_tunings) any = any || !t.HitsDefault();
    g_anyHitTuned = any;
}

void Save() {
    std::ofstream file(TablePath(), std::ios::trunc);
    if (!file) return;
    AcquireSRWLockShared(&g_lock);
    for (const auto& [id, t] : g_tunings)
        file << id << '\t' << t.reaction << '\t' << t.damage << '\t' << t.critical << '\t' << (t.kill ? 1 : 0) << '\t'
             << t.backupId << '\t' << (t.followTurns ? 1 : 0) << '\n';
    ReleaseSRWLockShared(&g_lock);
}

void Load() {
    std::ifstream file(TablePath());
    std::string line;
    AcquireSRWLockExclusive(&g_lock);
    g_tunings.clear();
    while (std::getline(file, line)) {
        std::istringstream in(line);
        std::string id, backupId;
        MoveTuning t;
        int kill = 0;
        if (!std::getline(in, id, '\t') || id.empty()) continue;
        in >> t.reaction >> t.damage >> t.critical >> kill;
        if (in.fail()) continue;
        in.get();
        std::getline(in, backupId);
        // The first person's follow after the back-up (a column added 2026-11-05: older lines have none, followed).
        if (const size_t tab = backupId.find('\t'); tab != std::string::npos) {
            t.followTurns = backupId.compare(tab + 1, std::string::npos, "0") != 0;
            backupId.resize(tab);
        }
        t.reaction = std::clamp(t.reaction, 0, 3);
        t.damage = std::clamp(t.damage, 0.0f, 10.0f);
        t.critical = std::clamp(t.critical, -1.0f, 100.0f);
        t.kill = kill != 0;
        t.backupId = backupId;
        if (Stored(id, t)) g_tunings[id] = t;
    }
    RefreshAny();
    ReleaseSRWLockExclusive(&g_lock);
}

void SetMoveTuning(const std::string& id, const MoveTuning& t) {
    AcquireSRWLockExclusive(&g_lock);
    if (Stored(id, t)) g_tunings[id] = t;
    else g_tunings.erase(id);
    RefreshAny();
    ReleaseSRWLockExclusive(&g_lock);
}

// The hit hooks, once a move's hits are tuned (the game must be supported).
void TickHooks() {
    static bool ensured = false;
    if (ensured || !g_anyHitTuned.load(std::memory_order_relaxed) || !game::IsSupported()) return;
    ensured = EnsureEnemyOnHitHook() && EnsureEnemyDamageHook() && EnsureEnemyReactionHook() && EnsureEnemyActionSetupHook() &&
              EnsurePlayerDamageHook() && EnsurePadQueryHooks();
    if (!ensured) LOG("melee table: the enemy hit hooks are missing, the game's moves keep their effects");
}

// The back-ups asked from the game thread (a prompt's or a melee swap's request), played at the next render frame.
std::mutex g_queueLock;
std::string g_queued;
DWORD g_queuedTick = 0;

void TickBackups() {
    std::string id;
    {
        std::lock_guard<std::mutex> lock(g_queueLock);
        if (g_queued.empty()) return;
        id.swap(g_queued);
        if (GetTickCount() - g_queuedTick > 500) return;  // too late (a load, the menu...)
    }
    RunBackup(id);
}

uintptr_t HitAttacker(void* hit) {
    using AttackerFn = uintptr_t(__fastcall*)(void* hit, void* edx);
    __try {
        return reinterpret_cast<AttackerFn>(mem::Rva(0x849530))(hit, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool MeleeAction(uint32_t action) { return (action >= 0x30 && action <= 0x4D) || action == 0x6F || action == 0xAC || action == 0xAD; }

MeleeHitState* g_current = nullptr;  // game thread: the hit OnHit is applying (its ApplyDamage runs inside)
// The damage player 1's game moves really did (user, 2026-10-31: the table shows what its multiplier gives; the game's
// hit is not its attack parameter's power: 480 for Chris's Head Flash of power 400): each move's last amount before the
// table's multiplier, by tuning id (game thread writes it, the menu reads it).
std::mutex g_seenLock;
std::unordered_map<std::string, int> g_seenDamage;
void* g_observedHit = nullptr;  // game thread: an untuned move's hit, its amount read in ApplyDamage
std::string g_observedId;

void SeeDamage(const std::string& id, uint32_t amount) {
    if (id.empty() || amount >= 100000) return;
    std::lock_guard<std::mutex> lock(g_seenLock);
    g_seenDamage[id] = static_cast<int>(amount);
}

int SeenDamage(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_seenLock);
    const auto it = g_seenDamage.find(id);
    return it == g_seenDamage.end() ? -1 : it->second;
}

// The reaction of a tuned hit, given a moment after it (user, 2026-10-30: "Projection" on Arm Front, the Majini only
// recoiled as natively - the game's own reaction to the move came after the one asked inside its hit): 50 ms later,
// on the render thread. A recoil or a flinch skips the Majini's own reaction (user: a flinch on the Head Flash still
// flew: blown off at once, DmgFlyoff 0x73 refused the flinch) unless the hit kills (user: "if it is fatal I want it to
// fly": the game's death); `ownDeath` when the game's reaction was skipped.
std::atomic<uintptr_t> g_reactEnemy{ 0 };
std::atomic<int> g_reactKind{ 0 };
std::atomic<DWORD> g_reactTick{ 0 };
std::atomic<bool> g_reactOwnDeath{ false };

void TickReaction() {
    const uintptr_t enemy = g_reactEnemy.load();
    if (!enemy || static_cast<int32_t>(GetTickCount() - g_reactTick.load()) < 50) return;
    g_reactEnemy = 0;
    const auto enemies = actors::Enemies();
    if (!actors::InGame() || std::find(enemies.begin(), enemies.end(), enemy) == enemies.end()) return;
    uint32_t action = 0;
    int16_t health = 0;
    mem::Read(enemy + 0x10E0, action);
    mem::Read(enemy + 0x1364, health);
    const int kind = g_reactKind.load();
    const bool reacted = ReactToMeleeHit(actors::Player(0), enemy, kind, g_reactOwnDeath.load());
    LOG("melee table: reaction %d on %08X (its action was %X, health %d) %s", kind, static_cast<unsigned>(enemy), action,
        health, reacted ? "asked" : "not given");
}

constexpr uint32_t kHeadBlowFlag = 0x00010000, kWholeHealthFlag = 0x10000000, kBulletFlag = 0x00080000;

// The back escape (user, 2026-10-31: "in the table too, for its power, critical..., never on a shortcut"): player 1 in
// the grab's event (action 0x80000000) playing his escape from a Majini's back hold, 0xF68 (a woman's 0xFA4). A row of
// the Général sub-tab without button nor back-up.
constexpr const char* kBackEscapeId = "melee.free_back_escape";
constexpr uint32_t kEventAction = 0x80000000;

bool BackEscapePlaying(uintptr_t p) {
    uint32_t motion = 0;
    return mem::Read(p + 0x384, motion) && ((motion & 0xFFFF) == 0xF68 || (motion & 0xFFFF) == 0xFA4);
}

// The escape hurts its Majini without a hit of player 1 (user, 2026-10-31: the table's settings did nothing on it):
// through the actors' SubHealth while it is in BackHoldDmg 0x9C, tuned there (MeleeEnemySubHealth). The game's elbow
// fells it; the table's recoil or flinch is given instead (not with the beta throw, a throw), and player 1 then finishes
// his elbow and gets his normal release: his escape update (VA 0xC27F62..) waits only while the Majini is in 0x9C, else
// startles (0x83, "freed by someone else") - its wait made unconditional as the beta throw does (VA 0xC28045, the same
// counted site).
mem::Patch g_escapeWait("melee table: player 1 finishes his back escape", 0x828045, { 0x74, 0x77 }, { 0xEB, 0x77 });

bool EscapeReactionWanted() {
    MoveTuning t;
    return GetMoveTuning(kBackEscapeId, t) && t.reaction != kReactionGame && !BetaBackThrowOn();
}

bool g_escapeWaitHeld = false;  // render thread: ours (the site is the beta throw's too: its bytes say nothing of ours)

void TickEscapeWait() {
    const bool wanted = game::IsSupported() && EscapeReactionWanted();
    if (wanted == g_escapeWaitHeld) return;
    g_escapeWaitHeld = wanted ? g_escapeWait.Apply() : !g_escapeWait.Restore();
}

// The escape's Majini (game thread): noted at its fall's request 0x9C, killed where it lies at its getup's 0x6B when the
// kill or a critical was drawn (no hit there: the escape has no damage of its own).
std::atomic<uintptr_t> g_escapeEnemy{ 0 };
std::atomic<bool> g_escapeKill{ false };
std::atomic<DWORD> g_escapeTick{ 0 };
// Its recoil / flinch, given when player 1's escape motion reaches the elbow's blow (user, 2026-10-31: 0.2 s into the
// escape it came before the elbow): the frame of the row's slider (Feature value), render thread.
std::atomic<uintptr_t> g_elbowEnemy{ 0 };
std::atomic<int> g_elbowKind{ 0 };
Feature* g_backEscape = nullptr;

void TickElbowReaction() {
    const uintptr_t enemy = g_elbowEnemy.load();
    if (!enemy) return;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    float frame = 0;
    const bool playing = p && BackEscapePlaying(p) && mem::Read(p + 0x398, frame);
    const float blow = g_backEscape ? g_backEscape->value : 35.0f;
    if (GetTickCount() - g_escapeTick.load() > 4000) {
        g_elbowEnemy = 0;
        return;
    }
    if (playing && frame < blow) return;  // not there yet (or his escape is over: at once)
    g_elbowEnemy = 0;
    const bool reacted = ReactToMeleeHit(p, enemy, g_elbowKind.load(), false);
    LOG("melee table: back escape's reaction %d on %08X at his frame %.0f %s", g_elbowKind.load(), static_cast<unsigned>(enemy),
        frame, reacted ? "asked" : "not given");
}

// Every enemy but the Majini and the fat Majini takes the back-ups (user, 2026-10-30: the Executioner, the Gatling,
// chainsaw and giant Majini, Reapers, lickers, Uroboros, Wesker, Jill; then "the flying and spider parasites, the
// Popokarimu", "the Majini and the fat Majini too" accept everything, "the parasites out of the neck take a back-up,
// even on an ordinary Majini"): the cEm10 family (IsMajiniEnemy) save the Gatling uEm23 and the giant uEm24, and none
// whose plaga is out (its sub-unit +0x1F9C). Jill (uEm83 / uEm84) takes none (user, 2026-10-31: "I cannot throw her
// with the throw that takes her to the ground, Chris plays the help instead").
bool BackupEnemy(uintptr_t enemy) {
    if (!enemy) return false;
    const char* name = UnitClassName(enemy);
    if (name && (std::strncmp(name, "uEm83", 5) == 0 || std::strncmp(name, "uEm84", 5) == 0)) return false;
    if (!IsMajiniEnemy(enemy)) return true;
    uintptr_t plaga = 0;
    if (mem::Read(enemy + 0x1F9C, plaga) && plaga) return true;
    return std::strcmp(name, "uEm23") == 0 || std::strcmp(name, "uEm24") == 0;
}

// ---- The table.
const char* const kReactions[] = { "Par défaut", "Projection (coup de poing de Chris)", "Recul (comme un Arm Front)",
                                   "Sursaut (sans prompt)" };

struct Candidate {
    std::string id, label;
};

std::string TabOf(const std::string& category) {
    static const std::string prefix = "Mêlée libre";
    if (category == prefix) return "Général";
    return category.size() > prefix.size() + 1 ? category.substr(prefix.size() + 1) : category;
}

// Every attack of the free melee (its sub-tabs), for the back-up lists.
std::vector<Candidate> Candidates() {
    std::vector<Candidate> list;
    for (Feature* f : List()) {
        if (f->kind != Kind::Action || f->category.rfind("Mêlée libre", 0) != 0 || CombatMoveKind(f->id) == 0 ||
            f->id == kBackEscapeId)
            continue;
        list.push_back({ f->id, f->name + "  (" + TabOf(f->category) + ")" });
    }
    return list;
}

const char* LabelOf(const std::vector<Candidate>& list, const std::string& id) {
    if (id.empty()) return "Par défaut";
    for (const Candidate& c : list)
        if (c.id == id) return c.label.c_str();
    return id.c_str();
}

void Header(const char* name, const char* tip) {
    ImGui::TableHeader(*name ? i18n::Tr(name) : name);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr(tip));
}
}  // namespace

bool RollPercent(float percent) {
    static uint32_t state = GetTickCount() | 1;
    uint32_t s = state;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    state = s;
    return static_cast<float>(s % 10000) / 100.0f < percent;
}

bool GetMoveTuning(const std::string& id, MoveTuning& out) {
    AcquireSRWLockShared(&g_lock);
    const auto it = g_tunings.find(id);
    const bool found = it != g_tunings.end();
    if (found) out = it->second;
    ReleaseSRWLockShared(&g_lock);
    if (!found) out = DefaultTuningOf(id);
    return found || !out.HitsDefault();
}

bool MoveFollowsTurns(const std::string& id) {
    AcquireSRWLockShared(&g_lock);
    const auto it = g_tunings.find(id);
    const bool follow = it != g_tunings.end() ? it->second.followTurns : DefaultTuningOf(id).followTurns;
    ReleaseSRWLockShared(&g_lock);
    return follow;
}

bool MeleeHitBegin(uintptr_t enemy, void* hit, MeleeHitState& state) {
    g_observedHit = nullptr;
    if (!g_anyHitTuned.load(std::memory_order_relaxed) || !hit || !enemy) return false;
    const uintptr_t h = reinterpret_cast<uintptr_t>(hit);
    const uintptr_t p = actors::Player(0);
    uint32_t flags = 0, action = 0;
    if (!p || !mem::Read(h + 0x3C, flags) || (flags & kBulletFlag) || !mem::Read(p + 0x10E0, action) ||
        (!MeleeAction(action) && action != kEventAction) || HitAttacker(hit) != p)
        return false;
    const bool escape = action == kEventAction && BackEscapePlaying(p);
    if (action == kEventAction && !escape) return false;
    const std::string id = escape ? std::string(kBackEscapeId) : Player1NativeMoveId(p, action);
    const bool tuned = !id.empty() && GetMoveTuning(id, state.tuning) && !state.tuning.HitsDefault();
    static std::string lastId;  // game thread
    static DWORD lastTick = 0;
    const DWORD now = GetTickCount();
    // Once a blow (its shapes meet the enemy for several frames), a move again included (2026-11-01: three Grounds in a
    // row logged nothing and looked missed).
    if (id != lastId || now - lastTick > 1000) {
        lastTick = now;
        int32_t power[2]{};  // the hit's power (+0x28 / +0x2C), to check the table's damage values against
        mem::Read(h + 0x28, power[0]);
        mem::Read(h + 0x2C, power[1]);
        LOG("melee table: player 1's hit with %s (action %X, power %d / %d)%s", id.empty() ? "no move of the table" : id.c_str(),
            action, power[0], power[1], tuned ? ", tuned" : "");
        lastId = id;
    }
    if (!tuned) {
        g_observedHit = id.empty() ? nullptr : hit;  // its damage seen in ApplyDamage
        g_observedId = id;
        return false;
    }
    state.id = id;
    state.hit = hit;
    state.flags = flags;
    mem::Read(h + 0x38, state.critical);
    uint32_t wanted = flags;
    bool noCritical = false;
    if (state.tuning.critical >= 0.0f) {
        if (RollPercent(state.tuning.critical)) {
            wanted |= kHeadBlowFlag;
        } else {
            wanted &= ~kHeadBlowFlag;
            noCritical = true;
        }
    }
    if (state.tuning.kill && !KillLeavesOne(enemy)) wanted |= kWholeHealthFlag;
    if (wanted != flags || noCritical) {
        mem::Write(h + 0x3C, wanted);
        if (noCritical) mem::Write(h + 0x38, 0.0f);
        state.written = true;
    }
    g_current = &state;
    return true;
}

void MeleeHitAmount(uintptr_t enemy, void* hit, uint32_t& amount) {
    if (hit && hit == g_observedHit) {
        g_observedHit = nullptr;
        SeeDamage(g_observedId, amount);
    }
    MeleeHitState* const state = g_current;
    if (!state || state->hit != hit) return;
    SeeDamage(state->id, amount);
    const MoveTuning& t = state->tuning;
    if (t.damage != 1.0f && amount < 100000) {
        amount = static_cast<uint32_t>(static_cast<float>(amount) * t.damage + 0.5f);
        const uintptr_t h = reinterpret_cast<uintptr_t>(hit);
        for (uintptr_t off : { 0x28u, 0x2Cu }) {  // the power (the big enemies' stagger gauges read it)
            int32_t value = 0;
            if (mem::Read(h + off, value) && value > 0 && value < 100000)
                mem::Write(h + off, static_cast<int32_t>(static_cast<float>(value) * t.damage + 0.5f));
        }
    }
    int16_t health = 0;
    if (t.kill && mem::Read(enemy + 0x1364, health) && health > 0) {
        const bool keepOne = KillLeavesOne(enemy);
        const uint32_t whole = static_cast<uint32_t>(health) - (keepOne ? 1u : 0u);
        if (amount < whole || keepOne) amount = whole;
    }
}

void MeleeHitEnd(uintptr_t enemy, void* hit, MeleeHitState& state) {
    g_current = nullptr;
    const uintptr_t h = reinterpret_cast<uintptr_t>(hit);
    if (state.written) {
        mem::Write(h + 0x3C, state.flags);
        mem::Write(h + 0x38, state.critical);
    }
    if (state.tuning.reaction != kReactionGame) {
        g_reactOwnDeath = state.skipped;
        g_reactKind = state.tuning.reaction;
        g_reactTick = GetTickCount();
        g_reactEnemy = enemy;
    }
}

void MeleeEnemySubHealth(uintptr_t enemy, int16_t& amount) {
    if (amount <= 0 || !enemy || enemy != g_escapeEnemy.load() || GetTickCount() - g_escapeTick.load() > 4000) return;
    MoveTuning t;
    if (!GetMoveTuning(kBackEscapeId, t) || t.HitsDefault()) return;
    int16_t health = 0;
    uint32_t its = 0;
    mem::Read(enemy + 0x1364, health);
    mem::Read(enemy + 0x10E0, its);
    const int value = std::clamp(static_cast<int>(static_cast<float>(amount) * t.damage + 0.5f), 0, 32767);
    LOG("melee table: back escape damage on %08X (its action %X, health %d): %d -> %d", static_cast<unsigned>(enemy), its,
        health, amount, value);
    amount = static_cast<int16_t>(value);
}

void MeleeEscapeRequest(uintptr_t enemy, int* action) {
    if (!enemy || !g_anyHitTuned.load(std::memory_order_relaxed)) return;
    if (*action == 0x9C) {  // its fall: the escape made (by the game, the presses or the beta throw)
        const uintptr_t p = actors::Player(0);
        uint32_t its = 0, theirs = 0;
        // Not the trainer's own BackHoldDmg (user, 2026-10-31: the back hold's neck break on Wesker - "his enemy plays a
        // startle instead": this row's flinch took the killer's request): the reversed neck break, a throw, a pair.
        if (!p || !mem::Read(p + 0x10E0, theirs) || theirs != kEventAction || !mem::Read(enemy + 0x10E0, its) || its != 0x9B ||
            !IsMajiniEnemy(enemy) || enemy == WeskerThrownUnit())
            return;
        MoveTuning t;
        if (!GetMoveTuning(kBackEscapeId, t) || t.HitsDefault()) return;
        const bool critical = t.critical >= 0.0f && RollPercent(t.critical);  // no hit: the head blow replayed, a kill
        const bool kill = t.kill || critical;
        if (critical) QueueHeadBlow(enemy);
        const DWORD now = GetTickCount();
        g_escapeKill = kill;
        g_escapeTick = now;
        g_escapeEnemy = enemy;
        // The recoil / flinch in place of its fall (not after the beta throw, a throw): at the elbow's blow.
        const bool react = !kill && t.reaction != kReactionGame && !BetaBackThrowOn();
        LOG("melee table: back escape on %08X%s%s", static_cast<unsigned>(enemy), kill ? (critical ? ", critical: killed" : ", killed") : "",
            react ? ", its reaction in place of its fall" : "");
        if (react) {
            g_elbowKind = t.reaction;
            g_elbowEnemy = enemy;
        }
        return;
    }
    if (*action == 0x6B && enemy == g_escapeEnemy.load() && g_escapeKill.load() && GetTickCount() - g_escapeTick.load() < 8000) {
        g_escapeEnemy = 0;
        g_escapeKill = false;
        mem::Write<int16_t>(enemy + 0x1364, 0);
        const uintptr_t p = actors::Player(0);
        uint8_t pad = 0;
        if (p && mem::Read(p + 0x2D9C, pad)) mem::Write<int32_t>(enemy + 0x136C, pad);  // his kill
        LOG("melee table: back escape's Majini %08X dies where it lies (getup %X -> DeadFaceDown 0x8E)", static_cast<unsigned>(enemy),
            *action);
        *action = 0x8E;
    }
}

// The head blow (user, 2026-10-31: "Wesker's, Jill's kicks, and Leon's RE4 kicks often blow heads off"): the Majini
// damage computation's own sequence once its critical is drawn (VA 0x860D2B..0x860D92), replayed for a critical made
// without the game's hit: its head joint from vfunc +0x23C (0x10), the blood effect on it by the effect manager
// [VA 0x1249C5C] RVA 0x3723E0 (thiscall: {3, 0, joint, 0xF}, the enemy, 0, VA 0x1249CF0, 4, VA 0x10E3BB0; ret 0x18),
// its handle in +0x1FC0, the head lost +0x1F2C |= 1, its sound 0x17 (RVA 0x757090, thiscall(id), its se bank +0x1454)
// and vfunc +0x260 (its neck's spurt, made once into +0x1FA8). Not where the game makes none: a plaga out (+0x1F9C),
// a head already lost, the classes it tests before (RVA 0x430B20 IsKindOf: VA 0x11EA9AC / 0x11EA988 / 0x11EA9F4).
// On the game thread (the effect and unit managers are the game's).
struct PendingBlow {
    uintptr_t enemy;
    DWORD tick;
    bool onGround = false;  // waits for its head near the floor (QueueHeadBlow)
};
std::mutex g_headBlowLock;
std::vector<PendingBlow> g_headBlows;       // a kick may make several criticals at once
std::atomic<bool> g_headBlowPending{ false };  // read by the pad query hook without the lock

bool HeadBlowAllowed(uintptr_t enemy) {
    if (!enemy || !IsMajiniEnemy(enemy)) return false;
    uintptr_t plaga = 0;
    uint8_t lost = 0;
    if ((mem::Read(enemy + 0x1F9C, plaga) && plaga) || (mem::Read(enemy + 0x1F2C, lost) && (lost & 1))) return false;
    using KindFn = bool(__fastcall*)(void* self, void* edx, uintptr_t dti);
    for (const uintptr_t dti : { 0xDEA9ACu, 0xDEA988u, 0xDEA9F4u }) {
        bool kind = true;
        __try {
            kind = reinterpret_cast<KindFn>(mem::Rva(0x430B20))(reinterpret_cast<void*>(enemy), nullptr,
                                                                 reinterpret_cast<uintptr_t>(mem::Rva(dti)));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (kind) return false;
    }
    return true;
}

bool PlayHeadBlow(uintptr_t enemy) {
    struct Desc {
        uint32_t kind, zero, joint, flags;
    };
    using JointFn = uint32_t(__fastcall*)(void* self, void* edx);
    using EffectFn = uint32_t(__fastcall*)(void* self, void* edx, Desc* desc, uintptr_t unit, int zero, uintptr_t a, int four,
                                           uintptr_t b);
    using SoundFn = void(__fastcall*)(void* self, void* edx, int id);
    using SpurtFn = void(__fastcall*)(void* self, void* edx);
    uintptr_t vtable = 0, joint = 0, spurt = 0, effects = 0;
    if (!mem::Read(enemy, vtable) || !mem::Read(vtable + 0x23C, joint) || !mem::Read(vtable + 0x260, spurt) || !joint || !spurt ||
        !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C5C)), effects) || !effects)
        return false;
    __try {
        Desc desc{ 3, 0, reinterpret_cast<JointFn>(joint)(reinterpret_cast<void*>(enemy), nullptr), 0xF };
        const uint32_t handle = reinterpret_cast<EffectFn>(mem::Rva(0x3723E0))(
            reinterpret_cast<void*>(effects), nullptr, &desc, enemy, 0, reinterpret_cast<uintptr_t>(mem::Rva(0xE49CF0)), 4,
            reinterpret_cast<uintptr_t>(mem::Rva(0xCE3BB0)));
        uint8_t lost = 0;
        mem::Read(enemy + 0x1F2C, lost);
        mem::Write<uint8_t>(enemy + 0x1F2C, lost | 1);
        mem::Write<uint32_t>(enemy + 0x1FC0, handle);
        reinterpret_cast<SoundFn>(mem::Rva(0x757090))(reinterpret_cast<void*>(enemy), nullptr, 0x17);
        reinterpret_cast<SpurtFn>(spurt)(reinterpret_cast<void*>(enemy), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Its head joint (id 4) near the floor: within 30 cm of player 1's feet or its own origin, whichever is lower (a pair's victim is placed
// above the floor while it flies).
bool HeadNearFloor(uintptr_t enemy) {
    uintptr_t joints = 0;
    uint32_t count = 0;
    float own = 0.0f, feet = 0.0f;
    if (!mem::Read(enemy + 0x318, joints) || !joints || !mem::Read(enemy + 0x314, count) || !mem::Read(enemy + 0x34, own)) return true;
    const uintptr_t p = actors::Player(0);
    const float floor = p && mem::Read(p + 0x34, feet) ? std::min(own, feet) : own;
    for (uint32_t i = 0; i < std::min<uint32_t>(count, 0x100); ++i) {
        uint8_t id = 0;
        float head[3]{};
        if (mem::Read(joints + i * 0x90 + 7, id) && id == 4 && mem::SafeRead(reinterpret_cast<void*>(joints + i * 0x90 + 0x80), head, sizeof(head)))
            return head[1] - floor < 30.0f;
    }
    return true;
}

bool QueueHeadBlow(uintptr_t enemy, bool onGround) {
    if (!HeadBlowAllowed(enemy)) return false;
    std::lock_guard<std::mutex> lock(g_headBlowLock);
    if (g_headBlows.size() < 16) g_headBlows.push_back({ enemy, GetTickCount(), onGround });
    g_headBlowPending = true;
    return true;
}

void HeadBlowTick() {
    if (!g_headBlowPending.load(std::memory_order_relaxed)) return;
    std::vector<PendingBlow> blows;
    {
        std::lock_guard<std::mutex> lock(g_headBlowLock);
        blows.swap(g_headBlows);
        g_headBlowPending = false;
    }
    const auto enemies = actors::Enemies();
    std::vector<PendingBlow> later;
    for (const PendingBlow& blow : blows) {
        const DWORD age = GetTickCount() - blow.tick;
        if (age > (blow.onGround ? 4000u : 1000u) || std::find(enemies.begin(), enemies.end(), blow.enemy) == enemies.end() ||
            !HeadBlowAllowed(blow.enemy))
            continue;
        if (blow.onGround && age < 3800 && !HeadNearFloor(blow.enemy)) {
            later.push_back(blow);  // its head still in the air
            continue;
        }
        LOG("melee table: critical without a game hit, %08X's head blown %s%s", static_cast<unsigned>(blow.enemy),
            PlayHeadBlow(blow.enemy) ? "off" : "(failed)", blow.onGround ? " (its head on the floor)" : "");
    }
    if (!later.empty()) {
        std::lock_guard<std::mutex> lock(g_headBlowLock);
        g_headBlows.insert(g_headBlows.end(), later.begin(), later.end());
        g_headBlowPending = true;
    }
}

bool MeleeSkipsReaction(uintptr_t enemy, void* hit) {
    MeleeHitState* const state = g_current;
    if (!state || state->hit != hit) return false;
    const int reaction = state->tuning.reaction;
    int16_t health = 0;  // its damage is already applied here
    if ((reaction != kReactionRecoil && reaction != kReactionFlinch) || !mem::Read(enemy + 0x1364, health) || health <= 0)
        return false;
    state->skipped = true;
    return true;
}

bool MajiniHoldsStealable(uintptr_t target);  // wesker_moves.cpp: a Majini holding a weapon to strike with

std::string BackupFor(const std::string& id, uintptr_t target) {
    // RE6's weapon steal: refused by any enemy without a weapon to take (user, 2026-11-11: "a back-up when the enemy aimed at cannot take
    // it").
    const bool refuses = BackupEnemy(target) ||
                         (target && id == "melee.free_re6_common_weapon_steal" && !MajiniHoldsStealable(target));
    if (!g_useBackup.load(std::memory_order_relaxed) || g_inBackup.load() || !refuses) return {};
    MoveTuning t;
    GetMoveTuning(TuningIdOf(id), t);
    if (t.backupId.empty() || t.backupId == id || !Find(t.backupId)) return {};
    LOG("melee table: %s refused on %s %08X, its back-up %s instead", id.c_str(), UnitClassName(target),
        static_cast<unsigned>(target), t.backupId.c_str());
    return t.backupId;
}

bool RunBackup(const std::string& backupId) {
    Feature* const backup = Find(backupId);
    if (!backup || !backup->run) return false;
    g_inBackup = true;
    const bool played = backup->run();
    g_inBackup = false;
    if (!played) LOG("melee table: the back-up %s was refused", backupId.c_str());
    return played;
}

void QueueBackup(const std::string& backupId) {
    std::lock_guard<std::mutex> lock(g_queueLock);
    g_queued = backupId;
    g_queuedTick = GetTickCount();
}

bool MeleeBackupRequest(uintptr_t p, int action) {
    if (!g_useBackup.load(std::memory_order_relaxed) || g_inBackup.load() || !p || p != actors::Player(0) ||
        action < 0x30 || (action > 0x39 && action != 0xAC && action != 0xAD))
        return false;
    uint32_t target = 0;
    if (!mem::Read(p + 0x2DA4, target) || !target) return false;
    const std::string id = Player1NativeMoveId(p, static_cast<uint32_t>(action));
    const std::string backup = id.empty() ? std::string() : BackupFor(id, target);
    if (backup.empty()) return false;
    QueueBackup(backup);
    return true;
}

// swaps.cpp's free melee sub-tabs: their attacks as a table.
// A cell's column: the first person's follow (written 10th) next to the move's name (user, 2026-11-05: at the far right
// it was out of view, the table scrolls sideways), the others after it in their written order.
constexpr int Col(int written) { return written == 0 ? 0 : written == 10 ? 1 : written == 11 ? 11 : written + 1; }

void DrawMeleeTable(const std::string& category) {
    const float scale = ImGui::GetFontSize() / 13.0f;
    std::vector<Feature*> rows;
    for (Feature* f : List())
        if (f->category == category && f->kind == Kind::Action) rows.push_back(f);
    if (rows.empty()) return;
    if (category == "Mêlée libre")
        ImGui::TextDisabled(i18n::Tr("Arm Front ... Leg Back : les coups du personnage joué, réglés comme dans son onglet (%s)."),
                            Player1MovesetName().c_str());
    const std::vector<Candidate> candidates = Candidates();
    const float rowHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f * scale;
    const float height = std::min(rowHeight * (static_cast<float>(rows.size()) + 1.0f) + ImGui::GetStyle().ScrollbarSize + 8.0f,
                                  560.0f * scale);
    constexpr int kColumns = 12;
    if (!ImGui::BeginTable("melee_moves", kColumns,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_SizingFixedFit,
                           ImVec2(0, height)))
        return;
    ImGui::TableSetupScrollFreeze(1, 1);
    // Columns in the order drawn; the cells below are placed by Col (their order as written).
    const char* const names[kColumns] = { "Coup", "Suivi 1re pers.", "Tenir", "Raccourci", "2e : tenir", "2e raccourci",
                                          "Réaction", "Dégâts", "Critique", "Tue d'un coup", "Back-up", "" };
    const char* const tips[kColumns] = {
        "Cliquer pour jouer le coup. Ses réglages valent aussi quand le jeu le lance par son prompt ou un échange.",
        "Avec « Première personne : suivre les rotations du corps » (Caméra) : à la première personne, la caméra tourne avec Chris pendant ce coup (le neck break qui le retourne...). Décoché : elle garde sa direction pendant le coup. Coché par défaut.",
        "Touche à maintenir avant le raccourci (facultative).",
        "Le raccourci du coup.",
        "Touche à maintenir avant le 2e raccourci (facultative).",
        "Un 2e raccourci (manette...).",
        "Ce que fait l'ennemi touché : la réaction du coup, projeté comme par le coup de poing de Chris, le recul d'un "
        "Arm Front (avec son prompt de head flash) ou un simple sursaut sans prompt, comme un coup de couteau à la tête.",
        "x1 = les dégâts du coup tels qu'ils sont ; x2 = deux fois plus. Entre parenthèses, les dégâts que ça donne : "
        "d'après le dernier coup porté (ils peuvent varier selon l'ennemi), sinon d'après la puissance du coup dans le jeu "
        "(un peu plus bas que les vrais dégâts).",
        "« du jeu » = ses critiques à lui (le Finisher Front fait toujours exploser la tête d'un Majini) ; sinon la chance "
        "en % que le coup soit critique : tête qui explose sur un Majini, dégâts en plus sur un boss.",
        "Le coup tue l'ennemi d'un coup, comme le neck break (Wesker et Jill gardent 1 point de vie, comme après un neck "
        "break).",
        "Avec « Utiliser les back-up » (Échanges) : ce coup est refusé à tout ennemi sauf les Majini et gros Majini dont "
        "le parasite n'est pas sorti (bourreau, Majini géant, Gatling, tronçonneuse, licker, Reaper, chien, parasites, "
        "Uroboros, Wesker, Jill, boss...) et le coup choisi ici est joué à la place. « Par défaut » : le coup est joué "
        "sur tout le monde.",
        "Remet les réglages du coup comme dans le jeu (pas ses raccourcis)." };
    for (int c = 0; c < kColumns; ++c) ImGui::TableSetupColumn(names[c]);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < kColumns; ++c) {
        ImGui::TableSetColumnIndex(c);
        Header(names[c], tips[c]);
    }
    for (Feature* f : rows) {
        ImGui::PushID(f->id.c_str());
        ImGui::TableNextRow();
        const bool available = !f->available || f->available();
        const std::string tid = TuningIdOf(f->id);
        const int kind = CombatMoveKind(f->id);  // -1: a game move
        MoveTuning t;
        GetMoveTuning(tid, t);
        const MoveTuning before = t;
        bool save = false;

        ImGui::TableSetColumnIndex(Col(0));
        if (!available) ImGui::BeginDisabled();
        if (ImGui::Button(f->name.c_str())) Trigger(*f);
        if (!available) ImGui::EndDisabled();
        if (!f->help.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", f->help.c_str());
        if (f->id == kBackEscapeId) {  // the frame of the elbow's blow (its recoil / flinch)
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130 * scale);
            widgets::SliderFloat("##elbow", &f->value, f->valueMin, f->valueMax, "coude : image %.0f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(i18n::Tr("L'image de l'animation de Chris où le coup de coude touche : le recul ou le sursaut choisi "
                                  "remplace la chute du Majini à ce moment-là (sans l'animation beta)."));
            if (ImGui::IsItemDeactivatedAfterEdit()) settings::SaveFeature(*f);
        }
        for (int c = 0; c < 4; ++c) {
            ImGui::TableSetColumnIndex(Col(1 + c));
            DrawKeyButton(*f, c >= 2, c % 2 == 0);
        }
        if (kind == 0) {  // no hit of its own
            for (int c = 5; c <= 9; ++c) {
                ImGui::TableSetColumnIndex(Col(c));
                ImGui::TextDisabled("—");
            }
        } else {
            ImGui::TableSetColumnIndex(Col(5));
            ImGui::SetNextItemWidth(140 * scale);  // narrower (user, 2026-11-05: the table fits): the list shows it whole
            if (kind == 2) {  // a throw: the throw is its reaction
                ImGui::BeginDisabled();
                int none = 0;
                ImGui::Combo("##reaction", &none, kReactions, 1);
                ImGui::EndDisabled();
            } else if (ImGui::Combo("##reaction", &t.reaction, kReactions, IM_ARRAYSIZE(kReactions))) {
                save = true;
            }
            ImGui::TableSetColumnIndex(Col(6));
            // Its damage with the multiplier (user, 2026-10-31: "write the damage the percentage gives").
            const int seen = kind < 0 ? SeenDamage(tid) : -1;
            const int base = seen >= 0 ? seen : kind < 0 ? NativeMovePower(tid) : CombatMoveDamage(f->id);
            char format[48] = "x%.2f";
            if (base >= 0)
                std::snprintf(format, sizeof(format), "x%%.2f (%d)", static_cast<int>(static_cast<float>(base) * t.damage + 0.5f));
            ImGui::SetNextItemWidth(150 * scale);
            widgets::SliderFloat("##damage", &t.damage, 0.0f, 10.0f, format);
            if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
            ImGui::TableSetColumnIndex(Col(7));
            ImGui::SetNextItemWidth(110 * scale);
            widgets::SliderFloat("##critical", &t.critical, -1.0f, 100.0f, t.critical < 0.0f ? "du jeu" : "%.0f %%");
            if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
            ImGui::TableSetColumnIndex(Col(8));
            if (ImGui::Checkbox("##kill", &t.kill)) save = true;
            ImGui::TableSetColumnIndex(Col(9));
            ImGui::SetNextItemWidth(160 * scale);
            if (f->id == kBackEscapeId) {
                ImGui::TextDisabled("—");
            } else if (ImGui::BeginCombo("##backup", LabelOf(candidates, t.backupId), ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable(i18n::Tr("Par défaut"), t.backupId.empty())) {
                    t.backupId.clear();
                    save = true;
                }
                for (const Candidate& c : candidates) {
                    if (c.id == f->id) continue;
                    const bool selected = c.id == t.backupId;
                    if (ImGui::Selectable(c.label.c_str(), selected)) {
                        t.backupId = c.id;
                        save = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        ImGui::TableSetColumnIndex(Col(10));  // every move, the dodges too (user, 2026-11-05)
        if (ImGui::Checkbox("##follow", &t.followTurns)) save = true;
        ImGui::TableSetColumnIndex(Col(11));
        if (ImGui::SmallButton(i18n::Tr("Rétablir"))) {
            t = DefaultTuningOf(tid);
            save = true;
        }
        const bool changed = t.reaction != before.reaction || t.damage != before.damage || t.critical != before.critical ||
                             t.kill != before.kill || t.backupId != before.backupId || t.followTurns != before.followTurns;
        if (changed) SetMoveTuning(tid, t);
        if (save) Save();
        ImGui::PopID();
    }
    ImGui::EndTable();
    ImGui::TextDisabled(i18n::Tr("Clic simple sur un curseur pour taper la valeur. Coups de Wesker, Jill, Leon, HUNK et désaisie : le "
                        "critique fait exploser la tête d'un Majini qui peut la perdre, sinon il fait 5 fois les dégâts."));
}

void RegisterMeleeTuning() {
    Load();
    g_backEscape = &Add({ .id = kBackEscapeId, .name = "Désaisie de dos",
          .help = "Le coup de Chris quand il se libère d'une prise de dos d'un Majini. Il ne se lance pas d'un raccourci (il "
                  "se joue seulement dans la prise), mais ses réglages du tableau valent pour lui. Son curseur : l'image du "
                  "coup de coude, où le recul ou le sursaut choisi remplace la chute du Majini.",
          .category = "Mêlée libre", .kind = Kind::Action,
          .run = [] { return false; },
          .available = [] { return false; },
          .value = 35, .valueMin = 0, .valueMax = 110, .valueFormat = "%.0f" });
    OnFrame(TickHooks);
    OnFrame(TickBackups);
    OnFrame(TickReaction);
    OnFrame(TickEscapeWait);
    OnFrame(TickElbowReaction);
    OnRestore([] { g_escapeWait.Restore(); });
    Add({ .id = "melee.use_backup", .name = "Utiliser les back-up",
          .help = "Les coups dont le « Back-up » n'est pas « Par défaut » (tableaux de la Mêlée libre) sont refusés à tout "
                  "ennemi sauf les Majini et gros Majini dont le parasite n'est pas sorti : bourreau, Majini géant, Gatling, "
                  "tronçonneuse, licker, Reaper, chien, parasites (et Majini au parasite sorti), Uroboros, Wesker, Jill, "
                  "les boss... Chris joue leur back-up à la place, que le coup vienne de la mêlée libre, d'un échange ou du "
                  "prompt du jeu. Décoché : chaque coup se joue sur tout le monde.",
          .category = "Mêlée", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_useBackup = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
}
}  // namespace features
