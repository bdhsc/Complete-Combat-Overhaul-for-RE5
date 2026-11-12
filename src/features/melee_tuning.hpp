#pragma once
// The free melee's table (melee_tuning.cpp): per attack its reaction, damage, critical, one-hit kill and back-up move.
#include <cstdint>
#include <string>

namespace features {

enum MeleeReaction { kReactionGame = 0, kReactionBlowOff = 1, kReactionRecoil = 2, kReactionFlinch = 3 };

struct MoveTuning {
    int reaction = kReactionGame;
    float damage = 1.0f;     // x the move's own damage
    float critical = -1.0f;  // below 0: the game's; else the percent of hits given the game's head-blow flag
    bool kill = false;       // one-hit kill
    std::string backupId;    // the back-up move's feature id ("": "Par défaut", the move itself)
    bool followTurns = true;  // the first person's camera follows the body's turns during it (camera.cpp)
    bool HitsDefault() const { return reaction == kReactionGame && damage == 1.0f && critical < 0.0f && !kill; }
};

// The tuning of a free melee move by its tuning id (TuningIdOf); false = none (the game's).
bool GetMoveTuning(const std::string& id, MoveTuning& out);
// The first person's camera follows the body's turns during the move (the table's "Suivi 1re pers.", default true).
bool MoveFollowsTurns(const std::string& id);
bool RollPercent(float percent);

// knife.cpp's enemy hit hooks (game thread), for player 1's hits with the game's own melee moves.
struct MeleeHitState {
    void* hit = nullptr;
    std::string id;  // the move's tuning id
    MoveTuning tuning;
    uint32_t flags = 0;    // the hit's +0x3C before
    float critical = 0;    // its +0x38 before
    bool written = false;
    bool skipped = false;  // the Majini's own reaction to it skipped (MeleeSkipsReaction)
};
bool MeleeHitBegin(uintptr_t enemy, void* hit, MeleeHitState& state);  // before the game's OnHit
void MeleeHitAmount(uintptr_t enemy, void* hit, uint32_t& amount);      // in ApplyDamage
void MeleeHitEnd(uintptr_t enemy, void* hit, MeleeHitState& state);     // after it
// In the Majini reaction function (knife.cpp): the game's reaction left out for the table's recoil / flinch (not on a
// hit that kills: its death is the game's).
bool MeleeSkipsReaction(uintptr_t enemy, void* hit);
// player.cpp's enemy SubHealth hook (game thread): the back escape's damage (no hit of player 1 there).
void MeleeEnemySubHealth(uintptr_t enemy, int16_t& amount);
// action_log.cpp's enemy request hook (game thread): the back escape's Majini (its fall 0x9C, then its getup 0x6B).
void MeleeEscapeRequest(uintptr_t enemy, int* action);
// A critical without the game's hit (the trainer's moves, the back escape): the game's head blow on a Majini that has
// one, played on the game thread (false: none for it, its damage x5 instead). HeadBlowTick: quick_knife.cpp's pad
// query hook (game thread).
// `onGround`: blown once its head is near the floor (a pair's victim thrown: user, 2026-11-10, Chris's RE6 Head Flash "the head
// explodes in the air: it must explode when the head hits the ground"), 4 s at most.
bool QueueHeadBlow(uintptr_t enemy, bool onGround = false);
void HeadBlowTick();

// "Utiliser les back-up": the back-up of the move `id` (a feature id) for `target`, "" when the move is played itself.
std::string BackupFor(const std::string& id, uintptr_t target);
bool RunBackup(const std::string& backupId);   // render thread: plays it now (true = played)
void QueueBackup(const std::string& backupId); // any thread: played at the next render frame
// action_log.cpp's player request hook: player 1's request of a game move refused for its back-up (queued).
bool MeleeBackupRequest(uintptr_t p, int action);

}  // namespace features
