// Knife parry (user, 2026-10-15: "close to Resident Evil 4 remake"): pressing the knife aim (game button 0x0D, also the
// "Viser avec le couteau (touche)" key), the quick knife (0x27) or the knife attack (fire 0x0E while the knife aim is
// held) just before an enemy blow lands cancels it: no damage, no hit reaction, Chris answers with a knife slash and a
// Majini attacker recoils with the head flash prompt (Recoil; the head stagger below is kept for the parasites).
// The blow: the player's pending hit [P+0x129C] (read by the hit reaction RVA 0x77B730, valid when +0x48), attacker
// resolved by RVA 0x849530; its damage reaches the player's SubHealth RVA 0x77C950. Both hooks (player.cpp) ask
// ParryHit: inside the window after a press the first enemy blow (not a bullet, flag 0x00080000) is parried, the
// window is used up; the same blow's later calls (SubHealth, reaction) are refused for 0.4 s.
// Riposte: player RequestAction (vtable +0xDC, RVA 0x75C900) 0x3E attack_knife_flying (the vertical slash, for the
// tests). Majini (action 0x6D = cEm10ActDmgHead): the light hit bits 0x100 / 0x200 of +0x1BB4 cleared and +0x1B5C |=
// 0x400, which the action setup copies to +0x1B60 (VA 0xC1AB30): DmgHead then always picks a stagger motion 0x50B-0x50D
// (action +0x24 = 1, melee prompt) - see knife.cpp; then the enemy's RequestAction(0x6D, 1, 0, -1, 0), once the
// slash is over (TickParryInput). That enemy's blows are refused while the slash lasts.
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "common/wav.hpp"
#include "features/melee_tuning.hpp"

#include <imgui.h>

#include <windows.h>
#include <mmsystem.h>
#include <intrin.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace features {
bool GameButtonHeld(int button, int pad);  // quick_knife.cpp
bool IsKnifeItem(uint16_t id);              // inventory.cpp
float Player1KnifeDamageFactor();          // knife.cpp: "Puissance du couteau" x the knife's own power
bool WeskerKeepsHealth(uintptr_t enemy);    // wesker_boss.cpp: "Wesker n'abandonne jamais"
bool StaminaAllows();                       // stamina.cpp: no parry while the red arc fills
void StaminaSpend();                        // stamina.cpp: a riposte's cost
void StaminaSpendRe6Counter();              // stamina.cpp: the RE6 parry's counter
void StaminaSpendRe7Guard();                 // stamina.cpp: the RE7 parry's (strict)
bool StaminaStrict();                       // stamina.cpp: "Endurance stricte"
int16_t LinWeskerDamage(uintptr_t enemy, int16_t amount);  // wesker_boss.cpp: "Dégâts reçus par Wesker (Lost in Nightmares)"
bool PadHeldMask(uint32_t& mask);          // movement.cpp
bool UsePadMaskForParry(bool on);          // movement.cpp
bool Re6OnGround();                        // wesker_moves.cpp: RE6's ground, no trainer move
bool Player1Dying();                       // dying.cpp
bool EnsurePadQueryHooks();                // quick_knife.cpp
bool EnsurePlayerDamageHook();             // player.cpp
bool EnsurePlayerHitReactionHook();        // player.cpp
bool EnsureCommonEffectsHook();            // quick_knife.cpp: the hit blood spawner VA 0x7757F0
bool EnsureEnemyOnHitHook();               // knife.cpp
void ClaimMeleeTarget(uintptr_t player, uintptr_t enemy);  // swaps.cpp: given back after the riposte
bool PlayNamedMove(int move);                              // wesker_moves.cpp: 0 = Wesker's guard
bool PlayerInWeskerMove(uintptr_t player);                 // wesker_moves.cpp
void FacePlayerTowards(uintptr_t player, const float* point);  // wesker_moves.cpp
bool WeskerCanThrow(uintptr_t enemy);                      // wesker_moves.cpp: Jill's throw can take it
void RequestThrowRiposte(uintptr_t enemy);                 // wesker_moves.cpp: thrown at the next frame
std::string BackupFor(const std::string& id, uintptr_t target);  // melee_tuning.cpp: its back-up on that enemy, "" none
void NoteTrainerMeleeKill(uintptr_t enemy);                       // mercenaries.cpp: its time bonus once counted
bool Player1InNeckBreak();                                        // wesker_moves.cpp: his neck broken from behind
bool PrepareWeskerMoves();                                 // wesker_moves.cpp
bool PlayGuardRiposte();                                   // wesker_moves.cpp: the guard, player 1 invulnerable in it
bool PlayCounterRiposte(uintptr_t target, int variant);    // wesker_moves.cpp: Chris's (0) or Sheva's (1) counter on Wesker
const char* CounterTuningId();                             // wesker_moves.cpp: the counter playing's free melee line
int Player1CharacterIndex();                               // swaps.cpp: 0 Chris, 1 Sheva, 2 Jill, 3 Wesker, 4 Josh...
const char* CharacterName(int character);                  // swaps.cpp
void DrawCharacterChoices(const char* table, const std::vector<std::pair<std::string, std::string>>& columns);  // swaps.cpp
bool PlayJoshCounter(uintptr_t target);                    // wesker_moves.cpp: "Josh Contre" on the parry's attacker
bool PlayRe6Counter(int which, uintptr_t target);          // wesker_moves.cpp: an RE6 counter on the parry's attacker
bool PlayKnifeCounter(uintptr_t player, uintptr_t target); // wesker_moves.cpp: RE6's knife counter with a knife in hand
bool PlaySpecialCounter(uintptr_t player, uintptr_t target);  // wesker_moves.cpp: RE6's counters on armed Majini
bool PlayRe6Sweep(uintptr_t target);                       // wesker_moves.cpp: RE6's sweep against a grab, on its grabber
bool Re6WeaponTwoHanded(uintptr_t p);                      // re6_motions.cpp: a two-handed weapon in hand
bool PrepareJoshCounter();                                 // swaps.cpp: Josh's list loaded
bool TrainerMoveReact(uintptr_t p, uintptr_t enemy, int reaction);  // wesker_moves.cpp: the table's reaction in place
void ForceNextEnemyMotion(uintptr_t enemy, uint32_t motion);  // wesker_moves.cpp: its next layer 0 motion replaced
bool RawButtonHeld(int button, int pad);                   // quick_knife.cpp: as the player really holds it
int Player1Pad();  // movement.cpp: the pad index player 1's buttons are read with
void CancelQuickShot();                                    // quick_knife.cpp: the press's quick shot dropped
bool QuickKnifeEmulating();                                // quick_knife.cpp: the quick knife played as the knife aim
bool RealKnifeAimHeld();                                   // quick_knife.cpp
void CancelQuickKnife();                                   // quick_knife.cpp
void KnifeWearParry();                                     // knife_icon.cpp: "Durabilité du couteau"
bool MajiniDieNow(uintptr_t enemy);                        // below
bool MajiniBlowOffFrom(uintptr_t enemy, const float* from, uint32_t part);  // below
bool EnsureEnemyActionHook();                              // action_log.cpp (the Executioner's lift refused)
bool EnsurePlayerRequestHook();                            // action_log.cpp (a release startle refused)
bool KnifeUnusable(int playerIndex);                       // inventory.cpp: knife slot "Rien" or its knife broken
bool BigEnemyClass(uintptr_t enemy);                       // knife.cpp: Executioners, chainsaw, Gatling, fat man, giant
bool IsChainsaw(uintptr_t enemy);                          // below
// "Réaction du Majini tronçonneuse à la parade" (user, 2026-10-23: a choice as the Executioner's): its action table (read
// live) cEm22ActDmgStaggers 0x19 (motion 0x64, a head stagger when asked alone - user; its AI's stagger after a body
// hit, pushed back by the shot itself) / 0x1A (after a leg hit), cEm22ActDmgActBtn 0x1B (a melee prompt, front; 0x1C
// from behind), cEm22ActDmg 0x17 (its hit reaction, motion 0x50: the recoil without prompt, user's default), or none
// (its attack only cut short). It is parried by the knife parry only (user: not the RE6 punch).
constexpr uint32_t kChainsawReactions[4] = { 0x19, 0x1A, 0x1B, 0x17 };
std::atomic<int> g_chainsawReaction{ 3 };
// "Vacille (jambes)": the leg stagger alone fell at once (user): its hit reaction 0x17 first, then 0x1A in place of what
// its AI asks once that ends (enemy request hook, ChainsawLegsAfterHit).
constexpr uint32_t kChainsawHit = 0x17, kChainsawLegs = 0x1A;
std::atomic<uintptr_t> g_chainsawLegs{ 0 };
std::atomic<DWORD> g_chainsawLegsTick{ 0 };
// The Reaper (uEm34, vtable RVA 0xB7AA88; user 2026-10-23: its execution could not be parried). Action table read live:
// Wait 0x00 / 0x0F, Ramp 0x01, Hang 0x02, Fall 0x03 / 0x0E, Move 0x04, Motion 0x05 / 0x07, Crawl 0x06, Jump 0x08-0x0D /
// 0x12, Turn 0x10 / 0x11, Smoke 0x13, Attack 0x14-0x16, AtkCatch 0x17, Damage 0x18-0x1A, CoreWait 0x1B, CoreMove 0x1C,
// FinalEnd 0x1D, Abort 0x1E-0x20, Blown 0x21, Die 0x22, Disrupted 0x23, Erase 0x24 (actions.log: hit 0x18 -> 0x19 ->
// 0x1A -> 0x1B -> 0x1C -> 0x1D -> Die; Blown seen once before a Die). Its execution: Attack 0x16 touches player 1, its
// AI asks AtkCatch 0x17 and player 1 gets a grab hit (+0x3C 0x40400010, power 0) whose reaction holds him (0x84), as
// the Executioner's catch. Like the chainsaw it is parried by the knife parry only (user: not the RE6 punch); its
// reaction a choice among its actions, to try them all (user: Damage 0x18 kneels it with its weak point open, too
// strong): Damage 0x18 / 0x19 / 0x1A, Blown 0x21, Disrupted 0x23, Abort 0x1E-0x20, Motion 0x05 / 0x07, Smoke 0x13, Fall
// 0x03, or none. Its death chain (CoreWait 0x1B, CoreMove 0x1C, FinalEnd 0x1D, Die 0x22, Erase 0x24) is left out.
bool IsReaper(uintptr_t enemy) {
    uintptr_t vtable = 0;
    return enemy && mem::Read(enemy, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB7AA88));  // uEm34
}
constexpr uint32_t kReaperCatch = 0x17;
// User's tries: Disrupted blows it up (deadly), Motion 0x05 / 0x07 and Smoke 0x13 taken out, Abort 0x20 the default.
constexpr uint32_t kReaperReactions[] = { 0x18, 0x19, 0x1A, 0x21, 0x23, 0x1E, 0x1F, 0x20, 0x03 };
constexpr int kReaperReactionCount = static_cast<int>(std::size(kReaperReactions));
std::atomic<int> g_reaperReaction{ 7 };
// The enemies whose execution only the knife parry stops (not the RE6 parry: user).
bool KnifeParryOnly(uintptr_t enemy) { return IsChainsaw(enemy) || IsReaper(enemy); }
uintptr_t JillThrownUnit();                                // wesker_moves.cpp: the Majini in Jill's throw
int JillDeviceMeleeDamage(uintptr_t enemy);                      // jill_device.cpp: 0 = her health
void MarkJillFallen(uintptr_t jill);                             // wesker_moves.cpp: her getup (or Sleep) after it
void DamageJillDevice(uintptr_t jill, int damage, const char* by);  // jill_device.cpp
bool JillKeepsHealth(uintptr_t enemy);                           // jill_device.cpp: "Figer la vie de Jill"
bool WeskerMoveInvulnerable(uintptr_t player);             // wesker_moves.cpp: player 1 in one of the moves
uintptr_t EnemyAttackInSphere(uintptr_t p, bool& grab);    // perfect_dodge.cpp: an armed attack in the sphere
int AutoParryKind();                                       // perfect_dodge.cpp: the automatic parry's riposte
bool AutoParryPress(int kind);                             // below: the automatic parry's window
bool AutoRe6Press();                                       // below: the automatic RE6 counter's window
bool StartDodgeMove(const char* id);                       // wesker_moves.cpp: a free melee move by id
bool IncomingBolt(uintptr_t& bolt, float& distance);       // rocket_catch.cpp: a bolt closing in on player 1
bool EnsureBoltHooks();                                    // rocket_catch.cpp: the bolts followed in flight
void HideFromProjectiles(DWORD ms);                        // perfect_dodge.cpp: player 1 hidden from the blows

namespace {
constexpr int kKnifeAim = 0x0D, kQuickKnife = 0x27, kFire = 0x0E, kAimButton = 0x0C;
constexpr uint32_t kSlashAction = 0x3E;      // attack_knife_flying
constexpr uint32_t kDmgHeadAction = 0x6D;
constexpr uintptr_t kDmgHeadVtable = 0xC3B1C8;  // cEm10ActDmgHead (RVA)
constexpr uint32_t kRecoilAction = 0x7E;        // cEm10ActDmgYoro_B
constexpr float kThrowerReach = 400.0f;         // cm: a Majini farther than this did not hit with its own blow
constexpr uintptr_t kRecoilVtable = 0xC3B4B4;   // (RVA)
constexpr uint32_t kHeadFlashAction = 0x32, kReunionHeadFlashAction = 0xAD;

std::atomic<bool> g_parryOn{ false };
std::atomic<float> g_window{ 0.3f };  // seconds
// "Riposte de la parade" (user, 2026-10-23): 0 the knife slash (RE4 Remake), 1 Wesker's guard (the same parry, his
// guard em80 0x74 in place of the slash: "RE7"), 2 Jill's throw on the attacker (not spared: the throw hurts it; the
// others neither, no riposte window; Chris is protected by the move itself). The throw falls back on the slash for an
// enemy it cannot take (and for projectiles).
// 3 (user, 2026-11-01: "Projection de Josh: Josh's Finisher Front parries the enemy's attack") the free melee's "Josh
// Contre" on the attacker, the slash if it is refused.
enum RiposteKind { kRiposteKnife = 0, kRiposteGuard = 1, kRiposteThrow = 2, kRiposteJosh = 3 };
// Per character (user, 2026-11-01: "a dropdown per character - Leon knife parry, Chris RE7... - and None, so that
// character cannot parry at all"): the knife parry's riposte (RiposteKind) and the RE6 parry's counter (0 Chris's, 1
// Sheva's), -1 "Aucune"; set by the choices from their registration on. Player 1's character: swaps.cpp.
constexpr int kCharacterCount = 8;
std::atomic<int> g_parryKindOf[kCharacterCount], g_re6KindOf[kCharacterCount];
// The RE6 parry's ripostes (user, 2026-11-01: "the two projections in the RE6 counter's dropdown too"): Chris's counter,
// Sheva's, Chris's projection (Jill's throw on the attacker), Josh's counter. "Aléatoire" per character: the chosen one
// half of the time, one of the three others otherwise (never "Aucune").
// Then RE6's own counters (user, 2026-11-09): Piers's, Chris's (RE6), Jake's, Leon's, Ada's and the bayonet's (a two-handed weapon
// only: in the random only then, Piers's otherwise) - wesker_moves.cpp PlayRe6Counter.
enum Re6Riposte { kRe6ChrisCounter = 0, kRe6ShevaCounter = 1, kRe6ChrisThrow = 2, kRe6JoshCounter = 3, kRe6PiersCounter = 4,
                  kRe6BayonetCounter = 9, kRe6Count = 10 };
std::atomic<bool> g_re6RandomOf[kCharacterCount];
// "Contre à la baïonnette de X (arme à deux mains)" (user, 2026-11-10: "take the bayonet counter out of the RE6 counter's list, a box
// beside it: the list's counter for the one-handed weapons, the two-handed one when I hold the right weapon, without forcing Piers's
// counter on one hand"): with a two-handed weapon in hand the bayonet's, whatever the list or the random gives.
std::atomic<bool> g_re6BayonetOf[kCharacterCount];
int RiposteKindNow() { return g_parryKindOf[Player1CharacterIndex()].load(std::memory_order_relaxed); }
int Re6KindNow() { return g_re6KindOf[Player1CharacterIndex()].load(std::memory_order_relaxed); }
int RiposteKind() { return std::max(RiposteKindNow(), 0); }  // where one is needed (none: the parry is off anyway)

int PickRe6Riposte() {
    const int c = Player1CharacterIndex();
    const uintptr_t p = actors::Player(0);
    if (g_re6BayonetOf[c].load(std::memory_order_relaxed) && p && Re6WeaponTwoHanded(p)) return kRe6BayonetCounter;
    const int chosen = std::clamp(Re6KindNow(), 0, kRe6BayonetCounter - 1);  // the list's (the bayonet's is the box's now)
    if (!g_re6RandomOf[c].load(std::memory_order_relaxed) || RollPercent(50.0f)) return chosen;
    int others[kRe6Count]{}, n = 0;
    for (int k = 0; k < kRe6BayonetCounter; ++k)
        if (k != chosen) others[n++] = k;
    return n ? others[static_cast<int>(GetTickCount() / 3 % static_cast<DWORD>(n))] : chosen;  // the same chance each
}
// "Réaction du bourreau à la parade" (user, 2026-10-23: kneeling opens him to a melee attack, "cool and not cool"):
// cEm20ActDmgKneelDown 0x1C (init RVA 0x474640: motion 0x31 and a melee prompt for the players, RVA 0x364480 type
// 0x19), cEm20ActBack 0x02 (a step back, no effect nor prompt), cEm20ActDmg 0x1D (his hit reaction: its init sprays
// blood, VA 0x7723E0), or none (his catch only cut short). No damage taken in any case.
constexpr uint32_t kExecutionerReactions[3] = { 0x1C, 0x02, 0x1D };
std::atomic<int> g_executionerReaction{ 0 };
// "Réaction de Wesker à la parade RE6" (user, 2026-10-25: "he does not do his native reaction"; recorded natively in the
// fight, diag.prompt_log / boss_motion_log): cEm80ActDmgS 0x62 (motion 0x6B, 87 frames: Chris's own melee prompts open,
// Leg Front / Arm Front, his default), cEm80ActShrink 0x68 (motion 0x77, 374 frames, stunned: his combo prompt, type 1
// kind 0x19 icon 9 handler RVA 0x50C6C0, then the chain AtkSyutoRevGo 0x4C-0x4F / RevEnd 0x50), cEm80ActAtkSH_NeeNg 0x34
// (the native counter: his fall 0x23, 144 frames, then his getup 0x2D; its init RVA 0x518960 takes [+0x1BA4] into the
// event 0x11F0E28 when there is one, as the counter prompt's handler RVA 0x50C4C0 does), or none. 0 / 1 at Chris's punch,
// 2 at the parry: with player 1 as his target the game plays its own counter on him (no RE6 riposte), else Chris's
// counter as for the others.
void Request(uintptr_t unit, uint32_t action, int flag);  // below
constexpr uint32_t kWeskerReactions[3] = { 0x62, 0x68, 0x34 };
std::atomic<int> g_weskerReaction{ 0 };
// The same choice for the knife parry and Wesker's guard (user, 2026-10-25: "they only stop the contact, no reaction of
// his"): asked at the parry, as the Executioner's; the native counter takes Chris from the slash / guard.
std::atomic<int> g_weskerKnifeReaction{ 0 };
// "Réaction de Jill à la parade" (user, 2026-10-26: "make her react, with a list as for Wesker"; the second part of her
// fight, uEm84, vtable RVA 0xB90728, actions read live): cEm84ActDmgCore 0x59 (stunned as by a bullet on her device,
// motion em84dmg 0x2C 291 frames: her throw prompt opens) / 0x5B (shorter, 155 frames), DmgNormalM 0x45 (a hit),
// DmgBlowL 0x49 (blown back), DmgKnockDown 0x4B (down), or none, at Chris's punch of the RE6 parry (user, 2026-10-26:
// its own list; the knife parry and the guard throw her, her Caught keeping her down under the device threshold, where
// a reaction of the trainer's could not: her AI, which asked her getup, asked it again at once). Her grab (AtckCatch 0x25's leap, then Catch 0x39 holding player 1; user: no parry
// took it) is parried when it is asked, the parry window open within 2.5 m, and refused for 2 s after the parry.
constexpr uint32_t kJillReactions[] = { 0x59, 0x5B, 0x45, 0x49, 0x4B };
constexpr int kJillReactionCount = static_cast<int>(std::size(kJillReactions));
constexpr uint32_t kJillCatch = 0x39;
std::atomic<int> g_jillRe6Reaction{ 0 };

bool IsJill2(uintptr_t enemy) {
    uintptr_t vtable = 0;
    return enemy && mem::Read(enemy, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB90728));  // uEm84
}

// Her reaction of the RE6 parry's list (false for none).
bool JillParried(uintptr_t jill) {
    const int reaction = g_jillRe6Reaction.load(std::memory_order_relaxed);
    if (reaction < 0 || reaction >= kJillReactionCount) return false;
    Request(jill, kJillReactions[reaction], 1);
    // Down (KnockDown 0x4B: her AI asked no getup, she stood up at once): the trainer's falls' getup.
    if (kJillReactions[reaction] == 0x4B) MarkJillFallen(jill);
    return true;
}
// His sweep (cEm80ActAtkLowKick 0x56, its hit follow-up AtkLowKickHit 0x57; user, 2026-10-25: "blocking high an attack
// that comes low is odd"): either parry makes Chris roll back away from him (pl Roll Back 0x52) in place of the riposte,
// and Wesker gets no reaction. His grab (AtkKataStart 0x51, then KataWait 0x52 / 0x53 holding player 1 in 0x0F0C and
// KataOk 0x54 in 0x0F0E; user: "only the native counter parries it", it alone replaces his action at once) and the
// sweep's follow-up are refused with the parry window open (within 2.5 m) and for 2 s after the parry
// (ParryRefusesEnemyAction).
constexpr uint32_t kPlayerBackRoll = 0x52;
// "État du Majini paré (parade RE6)" (user, 2026-10-23: the RE6 quick shot's choice of states, first for every parry, not
// the throw): ForceMajiniState's states, 1 = Recoil with the head flash prompt (the parry's own until then).
std::atomic<int> g_parryState{ 1 };
// "État du Majini paré (parade RE4)" (user, 2026-10-23: the RE6 parry keeps its recoil, the knife / guard parries their
// own choice, e.g. the arm hit + head flash). Then the guard ("parade RE7") its own too (user, 2026-10-23): 9 = the arm
// hit + head flash, the user's pick with it.
std::atomic<int> g_parryStateRe4{ 1 };
std::atomic<int> g_parryStateRe7{ 9 };
// "Parade façon RE6 (touche attaque)" (user, 2026-10-23): its own option, pressed with the attack key alone (fire 0x0E
// without the knife aim key, so the RE4 parry on the knife aim key works beside it); the riposte is Chris's counter on
// Wesker (em80_pl 0x01, PlayCounterRiposte: invulnerable, sped up like the guard), the attacker takes head-flash-like
// damage and the parried state (g_parryState); it is not spared (no riposte window).
std::atomic<bool> g_re6On{ false };
// "Parade RE6 : balayage contre les saisies" (user, 2026-11-09: RE6's counter against grabs, its sweep n° 57; the enemy falls as a
// running one shot in the leg): a parried grab gets the sweep (wesker_moves.cpp PlayRe6Sweep) instead of the character's riposte.
std::atomic<bool> g_re6Sweep{ false };
std::atomic<float> g_re6Window{ 0.3f };
constexpr int kRe6Damage = 500;  // health; a Mercenaries Majini has 800 (1000+ for the armoured ones)
}  // namespace
int Re6CounterDamage() { return kRe6Damage; }  // wesker_moves.cpp: the free melee table's damage column
int Re6ParryStateNow() { return g_parryState.load(std::memory_order_relaxed); }  // wesker_moves.cpp: the RE6 counters' reaction
uintptr_t Player1OwnSoundBank();  // swaps.cpp: player 1's own se bank while another character's is lent, else 0
bool FlyingParasiteReact(uintptr_t enemy, const char* by);  // knife.cpp: a Kipepeo lets go (Release) or is blown (DmgBlow)
namespace {
DWORD g_re6PressTick = 0;
bool g_re6PrevFire = false;
DWORD g_re6ReleasedSince = 0;  // the attack key released since then (0 = held)
constexpr DWORD kRe6ReleaseMs = 150;
DWORD g_pressTick = 0;                // render thread writes, game thread reads (same thread in this game)
bool g_prevAim = false, g_prevQuick = false, g_prevFire = false;
// "Parade automatique" / "Contre RE6 automatique" (perfect_dodge.cpp, user 2026-10-29): an enemy attack in the dodge
// sphere opens the knife parry's window (AutoParryPress, with its own riposte: g_autoKind) or the RE6 parry's
// (AutoRe6Press), whether or not the parries' own options are on; the parries then take the blow as after a press.
std::atomic<bool> g_autoKnifeOn{ false }, g_autoRe6On{ false };
std::atomic<int> g_autoKind{ 0 };  // kRiposteKnife / Guard / Throw
bool g_pressAuto = false;          // the last knife parry press was the automatic one
// Off for a character whose parry is "Aucune", the automatic ones too.
bool KnifeParryOn() {
    return (g_parryOn.load(std::memory_order_relaxed) || g_autoKnifeOn.load(std::memory_order_relaxed)) && RiposteKindNow() >= 0;
}
bool Re6ParryOn() {
    return (g_re6On.load(std::memory_order_relaxed) || g_autoRe6On.load(std::memory_order_relaxed)) && Re6KindNow() >= 0;
}
std::atomic<bool> g_parryNoQuickKnife{ false };
DWORD g_parriedTick = 0;
uintptr_t g_parriedAttacker = 0;
int16_t g_lastHealth = 0;  // player 1's health at the last frame (given back if a parried blow already took some)
// The riposte's enemy (ignores the slash, its blows refused while the slash lasts). The head stagger first waited
// for the end of the slash (its hit cut the head flash short); since the slash no longer touches it, the head stagger
// starts at the parry itself (user, 2026-10-15) and also cuts the attack.
uintptr_t g_pendingStagger = 0;
DWORD g_pendingTick = 0;
// Any riposte slash (melee or projectile parry). "La riposte blesse les autres ennemis" (user, 2026-10-20: parrying was
// far too strong, the slash killed everyone around): off by default = the slash hurts nobody; on = only the parried
// enemy is spared (the former behaviour).
DWORD g_riposteTick = 0;
std::atomic<bool> g_riposteHurtsOthers{ false };
// The riposte lasts 1.2 s after the parry, whatever the action (its blows landed 0.45-0.6 s after it in every trace;
// the slash can start late after a hit reaction or an early press).
constexpr DWORD kRiposteMs = 1200;

// Time stamps are never 0 (0 = none). They were "GetTickCount() | 1": an even time gave now + 1, the check a few
// milliseconds later read a delay of -1 = 4294967295 ms and ended the riposte at once (trace, 2026-10-20: every other
// riposte hurt, the parried enemy included).
DWORD Stamp() {
    const DWORD now = GetTickCount();
    return now ? now : 1;
}

DWORD Since(DWORD stamp) {
    const DWORD delay = GetTickCount() - stamp;
    return static_cast<int32_t>(delay) < 0 ? 0 : delay;
}

// The last blow player 1 took (player.cpp, after his hit reaction let it through): no parry nor perfect dodge of that
// enemy for kTakenMs (user, 2026-10-31: "hit, and still a short time to parry: the ring and the sound play though it
// missed" - an attack shape stays armed about 0.2 s after its contact, the sphere saw it then).
constexpr DWORD kTakenMs = 600;
std::atomic<uintptr_t> g_takenAttacker{ 0 };
std::atomic<DWORD> g_takenTick{ 0 };
bool BlowAlreadyTaken(uintptr_t attacker);  // below (after ClassName)

// "Son des parades" (user, 2026-10-21: RE6's counter sound at each parry; 2026-10-28: a choice, RE4 Remake's knife parry
// or RE6's - the knife's for the knife parry with its slash riposte only: the RE6 parry, Jill's throw and Wesker's guard
// keep RE6's): the user's own files in DataDir\sounds, never in git - parry_knife.wav (RE4 Remake's
// Play_WP_KNIFE_CM_SE_FX_REPEL_SUCCESS of bank ch_wp_knife_cm, picked by ear: tools/re_pak.py + tools/wwise_extract.py),
// parry_counter.wav (RE6's snd_game_cmn.spc wave 29, tools/spac_extract.py) and, for a parry that leaves the attacker
// standing (its reaction "Aucune", Wesker's sweep, a stopped projectile), parry_block.wav (RE4 Remake's REPEL_FAIL).
// The volume slider applies to the samples (common/wav.cpp: their own waveOut devices; PlaySound's own thread crashed
// the game in winmm right after a trainer reload, 2026-10-21).
enum ParrySound { kParrySoundNone, kParrySoundKnife, kParrySoundRe6 };
// None at first: the saved choice is applied at the start unless it is 0 ("Aucun": index 0 is never re-applied).
std::atomic<int> g_parrySoundKind{ kParrySoundNone };
std::atomic<float> g_parryVolume{ 1.0f };
std::atomic<bool> g_parrySoundReady{ false };  // off while the samples are made again (render thread)
// A grab parried with the knife has its own (user, 2026-10-28): parry_grab.wav, RE4 Remake's REPEL_BLOOD_SUCCESS (P3)
// until the user picks one by ear; optional (a missing file plays the knife's).
// Wesker's guard (the bare-hand RE7 parry) its own too (user, 2026-10-28: W16 of the page, RE4 Remake's Wesker bank
// mc_chi5): parry_guard.wav, optional (RE6's without it).
wav::Sound g_parryKnifeSound, g_parryBlockSound, g_parryRe6Sound, g_parryGrabSound, g_parryGuardSound;
bool g_parryGrabLoaded = false, g_parryGuardLoaded = false;

// The chosen sounds at the volume; false with `error` when a file is missing.
bool LoadParrySounds(std::string& error) {
    g_parrySoundReady = false;
    const int kind = g_parrySoundKind.load(std::memory_order_relaxed);
    if (kind == kParrySoundNone) {
        g_parryKnifeSound.Close();
        g_parryBlockSound.Close();
        g_parryRe6Sound.Close();
        return true;
    }
    const float gain = g_parryVolume.load(std::memory_order_relaxed);
    if (!g_parryRe6Sound.Load(L"parry_counter.wav", gain, error) ||
        (kind == kParrySoundKnife && (!g_parryKnifeSound.Load(L"parry_knife.wav", gain, error) ||
                                      !g_parryBlockSound.Load(L"parry_block.wav", gain, error))))
        return false;
    std::string optional;
    g_parryGrabLoaded = kind == kParrySoundKnife && g_parryGrabSound.Load(L"parry_grab.wav", gain, optional);
    g_parryGuardLoaded = kind == kParrySoundKnife && g_parryGuardSound.Load(L"parry_guard.wav", gain, optional);
    g_parrySoundReady = true;
    return true;
}

void CloseParrySounds() {
    g_parrySoundReady = false;
    g_parryKnifeSound.Close();
    g_parryBlockSound.Close();
    g_parryRe6Sound.Close();
    g_parryGrabSound.Close();
    g_parryGuardSound.Close();
    g_parryGrabLoaded = g_parryGuardLoaded = false;
}

// Wesker's guard also plays the player's own grunts (user, 2026-10-28: "le grognement du joueur lors d'un saut, Chris
// avec la voix de Chris, Sheva avec la voix de Sheva", then "193 et immédiatement après 196", found with "Essai : son
// du joueur"): sounds of the player's own bank (+0x1454: Chris's sound\se\pl\se_pl00, Sheva's se_pl01...). The game
// plays them too quietly (Unit::PlaySe RVA 0x7570E0: no volume of its own, copies at once did not add up; user: "même à
// 800 % très faible"), so the trainer plays them itself, louder: tools/extract_player_sounds.py writes each bank's takes
// of a request to DataDir\sounds\voices\<bank>_<id>_<n>.wav and their mix to <bank>_<id>.wav. 193 is a random group of
// two takes that never picks its last one again: "Essai" asked 4 copies at once, so the user heard both takes together
// (one take at a time was "pas les sons que j'ai entendu", and changed at each parry): the mix by default, or one take
// (g_guardGruntTake); without a file the game's own request.
std::atomic<int> g_guardGrunt{ -1 };  // the sound id, -1 none
std::atomic<float> g_guardGruntGain{ 4.0f };
std::atomic<int> g_guardGruntTake{ 0 };        // 0 the takes mixed, n the n-th take
std::atomic<int> g_guardGrunt2{ -1 };          // the second one (196), after its delay
std::atomic<int> g_guardGrunt2Delay{ 250 };    // ms
std::atomic<DWORD> g_guardGrunt2Due{ 0 };
constexpr int kSoundTryCopies = 4;

void PlayPlayerSound(int id, int copies) {
    const uintptr_t p = actors::Player(0);
    float at[3]{};
    if (id < 0 || !p || !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), at, sizeof(at))) return;
    using PlaySeFn = void(__fastcall*)(void* unit, void* edx, int id, const float* position);
    __try {
        for (int k = 0; k < std::clamp(copies, 1, 8); ++k)
            reinterpret_cast<PlaySeFn>(mem::Rva(0x7570E0))(reinterpret_cast<void*>(p), nullptr, id, at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Player 1's own bank name ("se_pl00"), empty if none.
std::string PlayerBankName() {
    const uintptr_t p = actors::Player(0);
    uintptr_t bank = Player1OwnSoundBank();  // his own while another character's is lent (swaps.cpp)
    char path[64]{};
    if (!p || (!bank && !mem::Read(p + 0x1454, bank)) || !bank ||
        !mem::SafeRead(reinterpret_cast<void*>(bank + 4), path, sizeof(path) - 1))
        return {};
    const char* slash = std::strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

// The voice files loaded (game thread; the unload closes them, g_voiceLock).
std::mutex g_voiceLock;
std::map<std::string, std::unique_ptr<wav::Sound>> g_voices;  // by file name, nullptr = missing

wav::Sound* VoiceFile(const std::string& name, float gain) {
    std::lock_guard<std::mutex> lock(g_voiceLock);
    auto it = g_voices.find(name);
    if (it == g_voices.end()) it = g_voices.emplace(name, std::make_unique<wav::Sound>()).first;
    if (!it->second) return nullptr;
    std::string error;
    const std::wstring file = L"voices\\" + std::wstring(name.begin(), name.end());
    if (!it->second->Load(file.c_str(), gain, error)) {
        it->second.reset();  // missing: the game's request from now on
        return nullptr;
    }
    return it->second.get();
}

// The player's sound `id`: the chosen take's file (a request of one take has no second: its mix), else the game's
// own request.
void PlayPlayerVoice(int id) {
    if (id < 0) return;
    const std::string bank = PlayerBankName();
    const float gain = g_guardGruntGain.load(std::memory_order_relaxed);
    const int take = g_guardGruntTake.load(std::memory_order_relaxed);
    const std::string name = bank + "_" + std::to_string(id);
    wav::Sound* sound = nullptr;
    if (!bank.empty() && take > 0) sound = VoiceFile(name + "_" + std::to_string(take - 1) + ".wav", gain);
    if (!bank.empty() && !sound) sound = VoiceFile(name + ".wav", gain);
    if (sound)
        sound->Play();
    else
        PlayPlayerSound(id, 1);
}

void CloseVoices() {
    std::lock_guard<std::mutex> lock(g_voiceLock);
    g_voices.clear();
}

void PlayGuardGrunt() {
    PlayPlayerVoice(g_guardGrunt.load(std::memory_order_relaxed));
    if (g_guardGrunt2.load(std::memory_order_relaxed) >= 0) {
        const DWORD due = GetTickCount() + static_cast<DWORD>(std::max(0, g_guardGrunt2Delay.load(std::memory_order_relaxed)));
        g_guardGrunt2Due = due ? due : 1;
    }
}

void GuardGruntTick() {  // game thread
    const DWORD due = g_guardGrunt2Due.load(std::memory_order_relaxed);
    if (!due || static_cast<int32_t>(GetTickCount() - due) < 0) return;
    g_guardGrunt2Due = 0;
    PlayPlayerVoice(g_guardGrunt2.load(std::memory_order_relaxed));
}

// "Essai : son du joueur" (research, user 2026-10-28: the grunt not heard under W16): the slider's sound of player 1's
// own bank alone, on the next game-thread tick (EffectTryTick).
std::atomic<int> g_soundTryPending{ -1 };

// `standing`: the attacker keeps its feet (no reaction of the trainer's); `riposte`: the knife parry's riposte kind
// (kRiposteKnife the slash, kRiposteGuard Wesker's guard...), -1 for the RE6 parry; `grab`: the parried blow was a grab
// (hit flag 0x40000000).
void PlayParrySound(bool standing, int riposte, bool grab = false) {
    if (!g_parrySoundReady.load(std::memory_order_relaxed)) return;
    const bool remake = g_parrySoundKind.load(std::memory_order_relaxed) == kParrySoundKnife;
    if (remake && riposte == kRiposteKnife)
        (grab && g_parryGrabLoaded ? g_parryGrabSound : standing ? g_parryBlockSound : g_parryKnifeSound).Play();
    else if (remake && riposte == kRiposteGuard && g_parryGuardLoaded)
        g_parryGuardSound.Play();
    else
        g_parryRe6Sound.Play();
    if (riposte == kRiposteGuard) PlayGuardGrunt();
}

// "Effet visuel de contre-attaque" (user, 2026-10-21: RE6's counter also shows a circle growing from the bottom of the
// screen, where the prompts are): drawn by the overlay, not the game's - two rings from the bottom centre, growing and
// fading out in ~0.4 s, started at each parry.
std::atomic<bool> g_parryRingOn{ false };
std::atomic<DWORD> g_parryRingTick{ 0 };

void DrawParryRing() {
    const DWORD start = g_parryRingTick.load(std::memory_order_relaxed);
    if (!g_parryRingOn.load(std::memory_order_relaxed) || !start) return;
    constexpr float kDurationMs = 420.0f, kSecondDelayMs = 70.0f;
    const float elapsed = static_cast<float>(Since(start));
    if (elapsed > kDurationMs + kSecondDelayMs) {
        g_parryRingTick = 0;
        return;
    }
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const ImVec2 centre(screen.x * 0.5f, screen.y * 0.84f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (int ring = 0; ring < 2; ++ring) {
        const float t = (elapsed - ring * kSecondDelayMs) / kDurationMs;
        if (t <= 0.0f || t >= 1.0f) continue;
        const float ease = 1.0f - (1.0f - t) * (1.0f - t);  // fast start, slow end
        const float radius = screen.y * (0.04f + 0.42f * ease);
        const float alpha = (1.0f - t) * (ring == 0 ? 0.9f : 0.55f);
        const float thickness = screen.y * (ring == 0 ? 0.012f : 0.006f) * (1.0f - 0.6f * t);
        dl->AddCircle(centre, radius, IM_COL32(200, 230, 255, static_cast<int>(alpha * 255)), 96, thickness);
        dl->AddCircle(centre, radius * 0.97f, IM_COL32(255, 255, 255, static_cast<int>(alpha * 120)), 96, thickness * 0.5f);
    }
}

void StartParryRing() {
    if (g_parryRingOn.load(std::memory_order_relaxed)) g_parryRingTick = Stamp();
}

// "Étincelles sur la parade au couteau" (user, 2026-10-28: RE4 Remake's knife parry has sparks "comme si les deux métaux
// s'entrechoquaient", not RE6's ring): the game's own metal hit sparks, core\cr001e - the bigger of the two its hit system
// spawns (VA 0x778340: cr001e above an impact strength threshold, cr001d below; user: "prends le plus grand") - spawned
// as that code does (VA 0x7786B5): sBioEffect [RVA 0xE49C5C] VA 0x772200 thiscall(&id {0, 0, 0, 0x1E}, &position,
// &axis (Z: VA 0x11AF830), 4, &params {-1, 0, 0, 0, s, s, s, 0, s}), ret 0x14, s = [VA 0xF554EC] = 1. Found with
// diag.effect_log / diag.effect_at_log. Game thread (the parry's hit reaction); between player 1 and the attacker at
// chest height. The knife parry with its slash riposte only: the other parries keep the ring.
std::atomic<bool> g_sparksOn{ false };
// The two core effects played together (user, 2026-10-28: "les étincelles sont les effets 3 et 4", found with "Essai :
// effet du jeu"; -1 = the second one off).
std::atomic<int> g_sparkEffect[2]{ { 3 }, { 4 } };

// A core effect (effect\efl\core\crNNNN) on a unit, as the game spawns the heal's and the knife's own (MODLOG: the
// heal's cr0008): sBioEffect [RVA 0xE49C5C] VA 0x7723E0 thiscall(&id {0, 0, 0, n}, unit, joint (-1 its origin, 0 the
// root), offset (the zero vector VA 0x1249CF0), 4, VA 0x10E3BB0). The point spawner VA 0x772200 with the hit system's
// arguments showed nothing (2026-10-28). Returns the effect's id (0 none). Game thread.
// Some effects loop as long as they live (user, 2026-10-28: after a few tries no effect showed any more, 12 of ours still
// on Chris): the trainer's are stopped by their id, sBioEffect VA 0x771170 thiscall(id) - SetBeam's way (it looks the id
// up in the active list +0x260, next +0x1E4, id +0x1E8: an ended one is not found).
// Any effect list by its id {type, 0, a, b} (the path builder VA 0x771400: type 0 "effect\efl\core\cr%02x%02x" of a / b,
// type 2 "effect\efl\pl\pl%02x\pl%02x%02x" of a, a, b - a character's list: {2, 0, 0x1d, 0x18} = pl1d\pl1d18, Rebecca's;
// 3 em, 5 wp, 7 np, 8 ev the same way).
uint32_t SpawnGameEffect(uint32_t type, uint32_t a, uint32_t b, uintptr_t unit, int joint) {
    uintptr_t effects = 0;
    if (!unit || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C5C)), effects) || !effects) return 0;
    alignas(16) uint32_t id[4] = { type, 0, a, b };
    using SpawnOnFn = uint32_t(__fastcall*)(void* effects, void* edx, const void* id, void* unit, int joint,
                                            const void* offset, int kind, const void* params);
    __try {
        return reinterpret_cast<SpawnOnFn>(mem::Rva(0x3723E0))(reinterpret_cast<void*>(effects), nullptr, id,
                                                               reinterpret_cast<void*>(unit), joint, mem::Rva(0xE49CF0), 4,
                                                               mem::Rva(0xCE3BB0));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint32_t SpawnCoreEffect(int index, uintptr_t unit, int joint) {
    return SpawnGameEffect(0, 0, static_cast<uint32_t>(index), unit, joint);
}

void StopEffect(uint32_t handle) {
    uintptr_t effects = 0;
    if (!handle || handle == 0xFFFFFFFFu || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C5C)), effects) || !effects)
        return;
    using StopFn = void(__fastcall*)(void* effects, void* edx, uint32_t handle);
    __try {
        reinterpret_cast<StopFn>(mem::Rva(0x371170))(reinterpret_cast<void*>(effects), nullptr, handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The trainer's effects alive (game thread): the try's and the parry sparks', each stopped after its time.
struct OwnEffect {
    uint32_t handle = 0;
    DWORD since = 0;
};
OwnEffect g_tryEffect, g_sparkEffectAlive[2];
constexpr DWORD kTryLife = 3000, kSparkLife = 2000;

void StopOld(OwnEffect& effect, DWORD life) {
    if (effect.handle && GetTickCount() - effect.since > life) {
        StopEffect(effect.handle);
        effect = {};
    }
}

// On the knife in player 1's hand (the slot's or an inventory one), else on player 1.
void SpawnParrySparks(uintptr_t player, uintptr_t attacker) {
    (void)attacker;
    uintptr_t held = 0, knife = 0;
    if (mem::Read(player + 0x2634, held) && held)
        for (int k = 0; k < 24 && !knife; ++k) {
            uint32_t item = 0;
            uintptr_t unit = 0;
            const uintptr_t record = player + 0x21A4 + 0x30 * k;
            if (mem::Read(record + 4, item) && mem::Read(record + 0x2C, unit) && unit == held && IsKnifeItem(item & 0xFFFF))
                knife = unit;
        }
    for (int k = 0; k < 2; ++k) {
        StopEffect(g_sparkEffectAlive[k].handle);
        g_sparkEffectAlive[k] = {};
        const int index = g_sparkEffect[k].load(std::memory_order_relaxed);
        if (index < 0) continue;
        g_sparkEffectAlive[k].handle = knife ? SpawnCoreEffect(index, knife, -1) : SpawnCoreEffect(index, player, 0);
        g_sparkEffectAlive[k].since = GetTickCount();
    }
}

// "Essai : effet du jeu" (research, user 2026-10-28: which of the game's core effects are the sparks): the effect of the
// slider's number on player 1 (his root joint), spawned by the next game-thread tick.
std::atomic<int> g_effectTryPending{ -1 };
std::atomic<int> g_effectTryJoint{ 0 };  // "Essai : effet de personnage" : the joint it is attached to

void EffectTryTick() {
    if (const int sound = g_soundTryPending.exchange(-1); sound >= 0) PlayPlayerSound(sound, kSoundTryCopies);
    GuardGruntTick();
    StopOld(g_tryEffect, kTryLife);
    for (OwnEffect& spark : g_sparkEffectAlive) StopOld(spark, kSparkLife);
    const int index = g_effectTryPending.exchange(-1);
    const uintptr_t p = actors::Player(0);
    if (index < 0 || !p) return;
    StopEffect(g_tryEffect.handle);
    if (index & 0x20000) {  // "Essai : effet de personnage": (character << 8) | list
        const uint32_t character = (index >> 8) & 0xFF, list = index & 0xFF;
        const int joint = g_effectTryJoint.load();
        g_tryEffect.handle = SpawnGameEffect(2, character, list, p, joint);
        LOG("effect try: pl%02x\\pl%02x%02x on player 1's joint %d (id %X)", character, character, list, joint,
            g_tryEffect.handle);
    } else {
        g_tryEffect.handle = SpawnCoreEffect(index, p, 0);
        LOG("effect try: core\\cr%04x on player 1 (id %X)", index, g_tryEffect.handle);
    }
    g_tryEffect.since = GetTickCount();
}

// "Parade de saisie au couteau : du sang" (user, 2026-10-28: a grab parried with the knife makes the enemy bleed, no
// sparks): the enemy's own blood as a bullet's (quick_knife.cpp's faked impacts): sBioEffect VA 0x7757F0 thiscall(unit,
// flags 0x7010200 = a pistol on flesh, point, direction, 0), ret 0x14, the unit's rCommonEffect (+0x152C) choosing it (a
// parasite's yellowish). At its chest, the direction from player 1. Game thread.
std::atomic<bool> g_grabBloodOn{ false };

bool SpawnParryBlood(uintptr_t player, uintptr_t attacker) {
    uintptr_t effects = 0, common = 0;
    float from[3]{}, to[3]{};
    if (!attacker || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C5C)), effects) || !effects ||
        !mem::Read(attacker + 0x152C, common) || !common ||
        !mem::SafeRead(reinterpret_cast<void*>(player + 0x30), from, sizeof(from)) ||
        !mem::SafeRead(reinterpret_cast<void*>(attacker + 0x30), to, sizeof(to)))
        return false;
    float dx = to[0] - from[0], dz = to[2] - from[2];
    const float length = std::sqrt(dx * dx + dz * dz);
    if (length > 1.0f) {
        dx /= length;
        dz /= length;
    } else {
        dx = 0.0f;
        dz = 1.0f;
    }
    alignas(16) float point[4] = { to[0] - dx * 15.0f, to[1] + 130.0f, to[2] - dz * 15.0f, 0.0f };
    alignas(16) float direction[4] = { dx, 0.0f, dz, 0.0f };
    using CommonEffectsFn = void(__fastcall*)(void* effects, void* edx, void* unit, uint32_t flags, const float* point,
                                              const float* direction, uint32_t x);
    __try {
        reinterpret_cast<CommonEffectsFn>(mem::Rva(0x3757F0))(reinterpret_cast<void*>(effects), nullptr,
                                                              reinterpret_cast<void*>(attacker), 0x7010200, point, direction, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool IsMajini(uintptr_t enemy);    // below

// A grab parried with the knife hurts its enemy as a knife hit (user, 2026-10-28: "les dégâts du couteau équipé à
// l'ennemi contré, que pour les saisies"): a stab of the game (50-60: 55) times player 1's knife power, at the parry;
// a kill as the RE6 counter's punch (CounterPunchLands). Game thread.
constexpr int kKnifeStab = 55;

void GrabParryDamage(uintptr_t player, uintptr_t enemy) {
    int16_t before = 0;
    if (!enemy || !mem::Read(enemy + 0x1364, before) || before <= 0) return;
    const int device = JillDeviceMeleeDamage(enemy);
    if (device) DamageJillDevice(enemy, device, "a grab parried with the knife");
    const int raw = static_cast<int>(kKnifeStab * Player1KnifeDamageFactor() + 0.5f);
    const int16_t stab = LinWeskerDamage(enemy, static_cast<int16_t>(std::clamp(raw, 1, 30000)));
    int16_t left = WeskerKeepsHealth(enemy) || device || JillKeepsHealth(enemy)
                       ? before
                       : static_cast<int16_t>(before > stab ? before - stab : 0);
    if (left < 1 && IsJill2(enemy)) left = 1;
    mem::Write(enemy + 0x1364, left);
    uint8_t pad = 0;
    if (mem::Read(player + 0x2D9C, pad)) mem::Write<int32_t>(enemy + 0x136C, pad);
    if (left <= 0 && IsMajini(enemy)) MajiniDieNow(enemy);
    LOG("parry: the grab of %08X parried with the knife, health %d -> %d", static_cast<unsigned>(enemy), before, left);
}

// The parry's visual: the knife parry's sparks (the slash riposte, with the option) - the enemy's blood for a grab -,
// else RE6's ring, but not for Wesker's guard (user, 2026-10-28: `ring` false).
void ParryVisual(uintptr_t player, uintptr_t attacker, bool knife, bool grab = false, bool ring = true) {
    if (knife && grab && g_grabBloodOn.load(std::memory_order_relaxed) && SpawnParryBlood(player, attacker)) return;
    if (knife && g_sparksOn.load(std::memory_order_relaxed)) SpawnParrySparks(player, attacker);
    else if (ring) StartParryRing();
}

// The RE6 parry's press: the attack key alone (not with the knife aim key held). Read raw: "Tir sans viser" and the RE6
// quick shot hide fire out of aim until the weapon is up (user, 2026-10-23: the parry only came while aiming).
void TickRe6Input() {
    if (Re6OnGround()) return;
    // The knife aim no longer stops it (user, 2026-11-10: "for the knife counter to work the knife aim must not stop the RE6 counters, only
    // the firearms' aim is refused"): its counter is then the knife counter (wesker_moves.cpp PlayRe6Counter).
    bool aim = false;
    bool fire = RawButtonHeld(kFire, -1) || RawButtonHeld(kFire, Player1Pad());
    // Not while aiming a weapon either (user, 2026-10-31: "refuse the RE6 parries when I aim, I sometimes parry by
    // accident while shooting"): the aim button (0x0C, pad bit 0x800) or an aim action (0x1B / 0x1C / 0x1E).
    uint32_t action = 0;
    const uintptr_t aimer = actors::Player(0);
    aim = aim || RawButtonHeld(kAimButton, -1) || RawButtonHeld(kAimButton, Player1Pad()) ||
          (aimer && mem::Read(aimer + 0x10E0, action) && (action == 0x1B || action == 0x1C || action == 0x1E));
    if (uint32_t mask = 0; PadHeldMask(mask)) {
        aim = aim || (mask & 0x800) != 0;  // the aim's bit (0x400, the knife aim's, no longer)
        fire = fire || (mask & 0x8000) != 0;
    }
    // One window per press (user, 2026-10-23: holding the key for a burst kept parrying): a press only counts after the
    // key was released for kRe6ReleaseMs, so a reading that drops between the shots of a burst opens no new window.
    if (fire && !g_re6PrevFire && !aim && g_re6ReleasedSince && Since(g_re6ReleasedSince) >= kRe6ReleaseMs &&
        StaminaAllows()) {
        g_re6PressTick = Stamp();
        LOG("parry RE6: press (keyboard / mouse %d, pad %d)", RawButtonHeld(kFire, -1), RawButtonHeld(kFire, Player1Pad()));
    }
    if (fire) g_re6ReleasedSince = 0;
    else if (!g_re6ReleasedSince) g_re6ReleasedSince = Stamp();
    g_re6PrevFire = fire;
    const uintptr_t p1 = actors::Player(0);
    if (p1 && !g_parryOn.load(std::memory_order_relaxed)) mem::Read(p1 + 0x1364, g_lastHealth);
}

// The knife riposte (attack_knife_flying 0x3E) outlasted the arm states' prompts (user, 2026-10-23: the prompt was gone
// when the slash ended): played faster through player 1's unit time scale +0xFD4 (as the guard, wesker_moves.cpp)
// while the riposte's slash plays, then given back. x2 was too fast, x1.3 right (user).
constexpr float kSlashSpeed = 1.3f;
uintptr_t g_slashSped = 0;  // render thread
float g_slashSavedScale = 1.0f;

void SpeedSlash(uintptr_t p, bool on) {
    if (!on) {
        if (g_slashSped && g_slashSped == p) mem::Write(g_slashSped + 0xFD4, g_slashSavedScale);
        g_slashSped = 0;
        return;
    }
    if (g_slashSped == p) return;
    float scale = 1.0f;
    if (!mem::Read(p + 0xFD4, scale) || scale < 0.5f || scale > 2.0f) return;  // not the plain 1.0 expected
    g_slashSped = p;
    g_slashSavedScale = scale;
    mem::Write(p + 0xFD4, scale * kSlashSpeed);
}

void TickParryRiposte();  // below

void TickParryInput() {
    if (Re6OnGround()) return;
    bool aim = GameButtonHeld(kKnifeAim, -1);
    bool quick = GameButtonHeld(kQuickKnife, Player1Pad());
    bool fire = GameButtonHeld(kFire, -1);
    if (g_parryNoQuickKnife.load(std::memory_order_relaxed) && QuickKnifeEmulating()) {
        aim = RealKnifeAimHeld();  // not the knife aim and fire the quick knife on the attack key emulates
        fire = RawButtonHeld(kFire, -1);
    }
    // With a pad the buttons only show in the player's held mask (user, 2026-10-20: no parry on the pad).
    if (uint32_t mask = 0; PadHeldMask(mask)) {
        aim = aim || (mask & 0x400) != 0;
        fire = fire || (mask & 0x8000) != 0;
    }
    // "Endurance": no knife parry while the red arc fills; Wesker's guard (the RE7 parry) is free and stays, but with
    // "Endurance stricte" (user, 2026-10-26).
    const bool staminaOk = StaminaAllows() || (RiposteKind() == kRiposteGuard && !StaminaStrict());
    // No knife-slash riposte with the knife slot "Rien" (user, 2026-10-27: no knife, no knife parry - Wesker's guard and
    // Jill's throw ripostes still parry).
    const bool noKnife = KnifeUnusable(0) && RiposteKind() == kRiposteKnife;
    // "Le couteau rapide ne déclenche pas la parade" (user, 2026-10-27: the quick knife on the attack key took every
    // press of the RE6 parry, the latest press deciding): the quick knife's presses open no knife parry window.
    const bool quickPress = quick && !g_prevQuick && !g_parryNoQuickKnife.load(std::memory_order_relaxed);
    if (((aim && !g_prevAim) || quickPress || (aim && fire && !g_prevFire)) && staminaOk && !noKnife) {
        g_pressTick = Stamp();
        g_pressAuto = false;
    }
    g_prevAim = aim;
    g_prevQuick = quick;
    g_prevFire = fire;
    TickParryRiposte();
}

// The riposte's time: its slash sped up, its end (also without the knife parry's option, for the automatic parry).
void TickParryRiposte() {
    const uintptr_t p1 = actors::Player(0);
    if (p1) mem::Read(p1 + 0x1364, g_lastHealth);
    uint32_t action = 0;
    SpeedSlash(p1, p1 && g_riposteTick && Since(g_riposteTick) < kRiposteMs && mem::Read(p1 + 0x10E0, action) &&
                       action == kSlashAction);
    if (!g_riposteTick || Since(g_riposteTick) < kRiposteMs) return;
    g_pendingStagger = 0;  // the riposte is over
    g_riposteTick = 0;
}

uintptr_t Attacker(uintptr_t hit) {
    using AttackerFn = uintptr_t(__fastcall*)(void* hit, void* edx);
    __try {
        return reinterpret_cast<AttackerFn>(mem::Rva(0x849530))(reinterpret_cast<void*>(hit), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// A held weapon's blow is its holder's (user, 2026-10-31: "an RE6 parry, and I lost health": the sphere names the
// Majini holding the weapon, a hit may name the weapon unit, whose holder is its +0x18 - the same blow not recognised,
// the press used up by the sphere's parry, the hit's damage went through).
const char* ClassName(uintptr_t object);  // below
uintptr_t BlowOwner(uintptr_t unit) {
    const char* cls = unit ? ClassName(unit) : nullptr;
    uintptr_t holder = 0;
    if (cls && std::strncmp(cls, "uWp", 3) == 0 && mem::Read(unit + 0x18, holder) && holder) return holder;
    return unit;
}

bool BlowAlreadyTaken(uintptr_t attacker) {
    const DWORD tick = g_takenTick.load(std::memory_order_relaxed);
    return attacker && tick && BlowOwner(attacker) == g_takenAttacker.load(std::memory_order_relaxed) && Since(tick) < kTakenMs;
}

void Request(uintptr_t unit, uint32_t action, int flag) {
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    // A player's riposte on foot only (user, 2026-10-28: a trainer move in the boat took Chris off it).
    if (actors::PlayerIndex(unit) >= 0 && !actors::OnFoot(unit)) return;
    if (!mem::Read(unit, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return;
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(unit), nullptr, static_cast<int>(action), flag, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The riposte aims at the attacker: melee moves align the player on its melee target P+0x2DA4, the point P+0x2E10
// (xyz) and the angle P+0x2E20 (the target's facing, towards the player; see swaps.cpp). The game never clears them:
// without this the slash took Chris to an old target (user, 2026-10-15: teleported).
void AimAt(uintptr_t player, uintptr_t enemy) {
    float enemyPos[3]{}, playerPos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), enemyPos, sizeof(enemyPos)) ||
        !mem::SafeRead(reinterpret_cast<void*>(player + 0x30), playerPos, sizeof(playerPos)))
        return;
    mem::Write<uint32_t>(player + 0x2DA4, static_cast<uint32_t>(enemy));
    ClaimMeleeTarget(player, enemy);  // a living target left behind made the knife draw turn and lock Chris
    mem::SafeWrite(reinterpret_cast<void*>(player + 0x2E10), enemyPos, sizeof(enemyPos));
    mem::Write(player + 0x2E20, std::atan2(playerPos[0] - enemyPos[0], playerPos[2] - enemyPos[2]));
}

// DTI class name of a unit (vtable slot 4 = GetDTI, DTI +4 = name), for the log.
const char* ClassName(uintptr_t object) {
    using GetDtiFn = void*(__fastcall*)(void* self, void* edx);
    __try {
        const uintptr_t vtable = *reinterpret_cast<uintptr_t*>(object);
        void* dti = reinterpret_cast<GetDtiFn>(*reinterpret_cast<uintptr_t*>(vtable + 4 * 4))(reinterpret_cast<void*>(object), nullptr);
        const char* name = dti ? *reinterpret_cast<const char**>(reinterpret_cast<uintptr_t>(dti) + 4) : nullptr;
        return name && !IsBadStringPtrA(name, 64) ? name : "?";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
}

// Projectiles and parasites: the slash is aimed at a virtual enemy at arm's reach in front of Chris (the point the
// swapped melee moves use, swaps.cpp: P+0x2AD0, angle = yaw P+0x2A10 + pi, no target), so he slashes on the spot.
void AimInFront(uintptr_t player) {
    constexpr float kPi = 3.14159265f;
    float reach[3]{}, yaw = 0;
    if (!mem::SafeRead(reinterpret_cast<void*>(player + 0x2AD0), reach, sizeof(reach)) || !mem::Read(player + 0x2A10, yaw))
        return;
    mem::Write<uint32_t>(player + 0x2DA4, 0);
    mem::SafeWrite(reinterpret_cast<void*>(player + 0x2E10), reach, sizeof(reach));
    float facing = yaw + kPi;
    if (facing > kPi) facing -= 2 * kPi;
    mem::Write(player + 0x2E20, facing);
}

bool HasAction(uintptr_t enemy, uint32_t index, uintptr_t vtableRva) {
    uintptr_t actions = 0, action = 0, vtable = 0;
    return mem::Read(enemy + 0x10DC, actions) && actions && mem::Read(actions + 4 * index, action) && action &&
           mem::Read(action, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(vtableRva));
}

bool IsMajini(uintptr_t enemy) { return HasAction(enemy, kDmgHeadAction, kDmgHeadVtable); }  // 0x6D = cEm10ActDmgHead

// A player's body (hip, joint 0 of [P+0x318], world position +0x80), where an enemy he hits sees him: in the RE6 parry's
// counter his origin runs ~1.2 m ahead of his body (the motion's offset), past the enemy he punches (user, 2026-10-23:
// it turned round after the counter, as hit in the back). His origin when the joints are not there.
bool PlayerBody(uintptr_t p, float (&at)[3]) {
    uintptr_t joints = 0;
    if (mem::Read(p + 0x318, joints) && joints && mem::SafeRead(reinterpret_cast<void*>(joints + 0x80), at, sizeof(at)))
        return true;
    return mem::SafeRead(reinterpret_cast<void*>(p + 0x30), at, sizeof(at));
}

bool IsExecutioner(uintptr_t enemy) {
    uintptr_t vtable = 0;
    // uEm20, and Lost in Nightmares' uEm27 (the same actions, read live 2026-10-26).
    return enemy && mem::Read(enemy, vtable) &&
           (vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB748A8)) || vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB74E38)));
}

// The chapter 5-3 Wesker (uEm80), the final fight's (uEm81, user 2026-10-26: "the parries must work on him as on the
// other", the same reaction lists) and Lost in Nightmares' (uEmA2): uEm80's actions in the same slots.
bool IsWesker(uintptr_t enemy) {
    uintptr_t vtable = 0;
    return enemy && mem::Read(enemy, vtable) &&
           (vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8BE60)) || vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8C300)) ||
            vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8C0B0)));
}

bool IsLinWesker(uintptr_t enemy) {
    uintptr_t vtable = 0;
    return enemy && mem::Read(enemy, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8C0B0));  // uEmA2
}

// Wesker's sweep parried: Chris rolls back away from him, no riposte nor reaction.
bool WeskerSweepRoll(uintptr_t player, uintptr_t attacker, bool melee, const char* parry) {
    uint32_t his = 0;
    if (!melee || !IsWesker(attacker) || !mem::Read(attacker + 0x10E0, his) || (his != 0x56 && his != 0x57)) return false;
    float at[3]{};
    if (mem::SafeRead(reinterpret_cast<void*>(attacker + 0x30), at, sizeof(at))) FacePlayerTowards(player, at);
    Request(player, kPlayerBackRoll, 0);
    LOG("parry %s: Wesker %08X's sweep (action %X), back roll asked", parry, static_cast<unsigned>(attacker), his);
    return true;
}

// A Majini is put in the head stagger (the strong motions: melee prompt).
bool HeadStagger(uintptr_t enemy) {
    if (!IsMajini(enemy)) return false;
    uint32_t light = 0, flags = 0;
    if (mem::Read(enemy + 0x1BB4, light)) mem::Write(enemy + 0x1BB4, light & ~0x300u);
    if (mem::Read(enemy + 0x1B5C, flags)) mem::Write(enemy + 0x1B5C, flags | 0x400u);
    Request(enemy, kDmgHeadAction, 1);
    return true;
}

// The parried Majini recoils like after Arm Front or a partner's rescue (user, 2026-10-20: holding its head after a
// parry looked odd), with the game's own head flash prompt. cEm10ActDmgYoro_B 0x7E: setup VA 0xC23900 plays motion
// 0x56B (+0x1B5C |= 0x8001); its update VA 0xBFEB20 calls vfunc 10 (VA 0xC0EBA0) when the motion's window flag
// +0x448 & 0x400 rises and vfunc 11 (VA 0xBFEC20, prompts removed) when it falls. vfunc 10 requests the head flash
// prompt (sActionButton RVA 0x364480, type 0x49, handler VA 0xB5DBA0 = player action 0x32; 0xD1 / VA 0xB5DC30 in
// Reunion) for every player whose bit 1 << (1 + index) of +0x1BB4 is clear: the player who hit is left out (after
// Chris's Arm Front +0x1BB4 = 0x02, only Sheva gets it; after Sheva's rescue, Chris). Player 1's bit is cleared here.
// Action +0x4C = 1 (previous action 0x7D, or +0x1B60 & 0x100000, copied from +0x1B5C by VA 0xC1AB30) gives other
// prompts (0x4C / 0x4B): that bit is cleared too.
// A Majini's short reaction at the RE6 parry: its arm hit DmgArm 0x6E played with motion 0x514 (163 frames; recorded on
// the user's arm shots 2026-10-23: a strong one plays 0x517 with the Arm Front prompt, the next one 0x514, the reaction
// wanted), the light hit bit 0x200 set so that it opens no prompt. The light DmgBody 0x6C first used sometimes threw it
// down, sometimes backed it far away; DmgArmWeapon 0x6F (motion 0x518) is a push. Not for one on the ground (it stood
// up, staggering).
constexpr uint32_t kCounterArmMotion = 0x514;

// A light arm hit (+0x1BB4 0x200) plays 0x53E or, three times in four (random +0x1034 & 3), a falling one 0x541 / 0x543 /
// 0x544 for which its setup (VA 0xC21680) also sets +0x1B5C |= 4 after the motion: the Majini falls. Played as the
// standing 0x514 it kept that bit and its AI stood it up from the ground at the end (RiseFaceUp 0x6A from RVA 0x6753E7,
// its getup's first frame lying: user, 2026-10-27, "il se téléporte au sol"). The bit is cleared while it plays 0x514 in
// that arm hit (game thread, StandingArmTick).
struct StandingArm {
    std::atomic<uintptr_t> enemy{ 0 };
    std::atomic<DWORD> tick{ 0 };
};
StandingArm g_standingArm[4];
std::atomic<int> g_standingArmNext{ 0 };

void KeepArmHitStanding(uintptr_t enemy) {
    StandingArm* slot = nullptr;
    for (StandingArm& s : g_standingArm)
        if (s.enemy.load() == enemy) slot = &s;
    if (!slot) slot = &g_standingArm[g_standingArmNext.fetch_add(1) & 3];
    slot->tick = Stamp();
    slot->enemy = enemy;
}

void StandingArmTick() {
    for (StandingArm& s : g_standingArm) {
        const uintptr_t enemy = s.enemy.load(std::memory_order_relaxed);
        if (!enemy) continue;
        const DWORD since = Since(s.tick.load(std::memory_order_relaxed));
        uint32_t action = 0, motion = 0, flags = 0;
        if (since > 6000 || !mem::Read(enemy + 0x10E0, action) || !mem::Read(enemy + 0x384, motion)) {
            s.enemy = 0;
            continue;
        }
        if (action != 0x6E) {
            if (since > 500) s.enemy = 0;  // the arm hit is over (or never came)
            continue;
        }
        if ((motion & 0xFFFF) != kCounterArmMotion || !mem::Read(enemy + 0x1B5C, flags) || !(flags & 4)) continue;
        mem::Write(enemy + 0x1B5C, flags & ~4u);
        LOG("parry: Majini %08X kept standing in its arm hit (fall bit cleared)", static_cast<unsigned>(enemy));
    }
}

bool LightFlinch(uintptr_t enemy) {
    if (!HasAction(enemy, kDmgHeadAction, kDmgHeadVtable)) return false;
    int16_t health = 0;
    uint32_t action = 0;
    if (!mem::Read(enemy + 0x1364, health) || health <= 0 || !mem::Read(enemy + 0x10E0, action)) return false;
    if (action == 0x6A || action == 0x6B || action == 0x82 || action == 0x83 || action >= 0x8B) return false;
    uint32_t light = 0;
    if (mem::Read(enemy + 0x1BB4, light)) mem::Write(enemy + 0x1BB4, light | 0x200u);
    ForceNextEnemyMotion(enemy, kCounterArmMotion);
    KeepArmHitStanding(enemy);
    Request(enemy, 0x6E, 1);
    return true;
}

bool Recoil(uintptr_t enemy) {
    if (!IsMajini(enemy) || !HasAction(enemy, kRecoilAction, kRecoilVtable)) return false;
    uint32_t players = 0, flags = 0;
    if (mem::Read(enemy + 0x1BB4, players)) mem::Write(enemy + 0x1BB4, players & ~0x302u);
    if (mem::Read(enemy + 0x1B5C, flags)) mem::Write(enemy + 0x1B5C, flags & ~0x100000u);
    Request(enemy, kRecoilAction, 1);
    return true;
}
}  // namespace

// wesker_moves.cpp (Rebecca's duo move's flames): a character's effect list (pl<character>\pl<character><list>) on a
// unit's joint, and its stop. Game thread.
uint32_t SpawnCharacterEffect(int character, int list, uintptr_t unit, int joint) {
    return SpawnGameEffect(2, static_cast<uint32_t>(character), static_cast<uint32_t>(list), unit, joint);
}
void StopGameEffect(uint32_t handle) { StopEffect(handle); }

// wesker_moves.cpp: a request of a player's se bank played by the trainer (user, 2026-10-31: "no effort sound on the
// trainer's moves" - the game's PlaySe plays the player's voices too faintly, as the guard's grunts): one take of it from
// DataDir\sounds\voices\<bank>_<id>_<n>.wav (tools/extract_player_sounds.py), at random among them; `bank` empty = the
// played character's (se_pl00...). False without the files.
bool PlayMoveSound(const std::string& wanted, int id, float gain) {
    static float loadedGain = 0.0f;  // render thread
    if (gain != loadedGain) {
        if (loadedGain != 0.0f) CloseVoices();  // loaded again with the new volume
        loadedGain = gain;
    }
    const std::string bank = wanted.empty() ? PlayerBankName() : wanted;
    std::vector<wav::Sound*> takes;
    for (int n = 0; n < 8 && !bank.empty(); ++n) {
        wav::Sound* sound = VoiceFile(bank + "_" + std::to_string(id) + "_" + std::to_string(n) + ".wav", gain);
        if (!sound) break;
        takes.push_back(sound);
    }
    if (takes.empty()) return false;
    takes[GetTickCount() % takes.size()]->Play();
    return true;
}

// The played character's effort on a trainer's move: his Head Flash's or Arm Front's voice (requests 10 and 9 of every
// player bank, voice package 1: recorded 2026-10-31 with Chris, Jill and Wesker - "the 66 on Wesker are not great, take
// the Head Flash's or the Arm Front's", user), one or the other in turn; without the files, the game's own request.
void PlayEffortGrunt(float gain) {
    static int last = 9;  // render thread
    last = last == 9 ? 10 : 9;
    if (!PlayMoveSound(std::string(), last, gain)) PlayPlayerSound(last, 1);
}

// player.cpp: player 1's hit reaction let a blow through (its attacker from his pending hit P+0x129C).
void NotePlayer1BlowTaken(uintptr_t player) {
    uintptr_t hit = 0;
    if (!player || !mem::Read(player + 0x129C, hit) || !hit) return;
    const uintptr_t attacker = BlowOwner(Attacker(hit));
    if (!attacker) return;
    g_takenAttacker = attacker;
    g_takenTick = Stamp();
}

// perfect_dodge.cpp: that enemy's blow just taken (no perfect dodge of it).
bool Player1BlowTaken(uintptr_t attacker) { return BlowAlreadyTaken(attacker); }

uintptr_t HitAttacker(uintptr_t hit) { return Attacker(hit); }
bool IsMajiniEnemy(uintptr_t enemy) { return IsMajini(enemy); }
const char* UnitClassName(uintptr_t object) { return ClassName(object); }

// The riposte does not touch its enemy (user, 2026-10-15: the slash sometimes killed it; only the parry's effect is
// wanted), nor the others unless "La riposte blesse les autres ennemis": during the riposte the enemies' OnHit
// (knife.cpp hook) ignores player 1's hits.
// A head flash from the recoil's prompt is not part of the riposte: it hits.
bool ParryIgnoresHit(uintptr_t enemy, void* hit) {
    if (!g_riposteTick || Since(g_riposteTick) >= kRiposteMs) return false;
    const bool target = g_pendingStagger && enemy == g_pendingStagger;
    if (!target && g_riposteHurtsOthers.load(std::memory_order_relaxed)) return false;
    const uintptr_t p1 = actors::Player(0);
    uint32_t action = 0;
    if (!p1 || (mem::Read(p1 + 0x10E0, action) && (action == kHeadFlashAction || action == kReunionHeadFlashAction)))
        return false;
    return Attacker(reinterpret_cast<uintptr_t>(hit)) == p1;
}

// "Parade en mourant" (user, 2026-10-20: the knife is out while dying, RE6 style, so parries worked there too): off by
// default, no parry while dying (health 1, see Player1Dying).
std::atomic<bool> g_parryDying{ false };

// "Parade pendant une saisie" (user, 2026-10-20: held from behind by a Majini, a parry fired on another enemy's blow and
// its riposte broke the grab): off by default, no parry while player 1 is held (event wait 0x80000000, or the grab
// requests 0x84 / 0x82, as dying.cpp and action_log.cpp).
std::atomic<bool> g_parryHeld{ false };

// "Parade contre le feu, les explosions et l'électricité" (user, 2026-10-23: the RE6 parry stopped blasts and electric
// arcs, "a bit of a cheat"; unticked, the default, none of the parries stops them): blasts and fire carry +0x3C bit
// 0x00200000 (logged: a barrel uOm0886 and a grenade uWp54 0x00202690, a thrown dynamite uWp6c 0x00200010; grenades
// 0x00204490), an electric arc comes from a stage object (uOm0933: +0x3C 0x00000010, +0x40 0x00008000, +0x9C
// 0x001E001A). The enemies' blows carry 0x00400000 (Majini 0x00400810, Reaper 0x00400010) or a grab 0x40000000; their
// thrown axes and bolts are weapon units (uWp*), still parried.
std::atomic<bool> g_parryHazards{ false };
constexpr uint32_t kBlastHit = 0x00200000;

static bool Held(uintptr_t player) {
    uint32_t action = 0;
    return mem::Read(player + 0x10E0, action) && (action == 0x80000000 || action == 0x84 || action == 0x82);
}

bool ForceMajiniState(uintptr_t enemy, int state);  // below: a melee prompt state (the RE6 quick shot's choice)

void CutMoveForParry();  // wesker_moves.cpp: the trainer's move playing cut, the riposte allowed to start

bool ParryAttack(uintptr_t player, uintptr_t attacker, uint32_t flags, bool fromReaction);

// What an enemy shot or threw (a crossbow bolt, an axe: weapon units uWp*). A bolt's hit carries the bullet flag
// 0x00080000 (logged 2026-10-30: "bullet of uWp81", the parries refused it); a gun's bullet comes from its shooter.
bool ProjectileUnit(uintptr_t unit) {
    const char* cls = unit ? ClassName(unit) : nullptr;
    return cls && std::strncmp(cls, "uWp", 3) == 0;
}

// The hit pending on player 1 comes from such a projectile.
bool PendingProjectileHit(uintptr_t player) {
    uintptr_t hit = 0;
    uint8_t valid = 0;
    uint32_t flags = 0;
    return player && mem::Read(player + 0x129C, hit) && hit && mem::Read(hit + 0x48, valid) && valid &&
           mem::Read(hit + 0x3C, flags) && ProjectileUnit(Attacker(hit));
}

// The automatic parries against projectiles (user, 2026-10-30: hit by a crossbow bolt - the sphere sees the enemies and
// the weapons they hold, not a bolt in flight): its hit opens the automatic parry's (or RE6 counter's) window itself.
void AutoParryProjectile(uintptr_t player, uintptr_t attacker, uint32_t flags) {
    if (!attacker || !ProjectileUnit(attacker)) return;
    const bool knifeOpen = g_pressTick && Since(g_pressTick) <= static_cast<DWORD>(g_window.load(std::memory_order_relaxed) * 1000.0f);
    const bool re6Open = g_re6PressTick && Since(g_re6PressTick) <= static_cast<DWORD>(g_re6Window.load(std::memory_order_relaxed) * 1000.0f);
    if (knifeOpen || re6Open) return;
    if (AutoParryPress(AutoParryKind()) || AutoRe6Press())
        LOG("parry: automatic parry window opened by %s %08X's hit", ClassName(attacker), static_cast<unsigned>(attacker));
}

// The hit pending on player 1 (player.cpp: his hit reaction, his SubHealth).
bool ParryHit(uintptr_t player, bool fromReaction) {
    uintptr_t hit = 0;
    uint8_t valid = 0;
    uint32_t flags = 0;
    const bool pending = player && mem::Read(player + 0x129C, hit) && hit && mem::Read(hit + 0x48, valid) && valid;
    if (pending) mem::Read(hit + 0x3C, flags);
    const uintptr_t attacker = pending ? Attacker(hit) : 0;
    if (player == actors::Player(0)) AutoParryProjectile(player, attacker, flags);
    return ParryAttack(player, attacker, flags, fromReaction);
}

// grabs.cpp, "Saisies améliorées": the Majini takes its hold in its decision, without a grab hit on player 1, so no hit
// reaction came to parry it (user, 2026-10-23: no parry worked on the grabs with that option). Parried there instead, as
// its grab hit (+0x3C 0x40000000) would be; the hold is then not given.
bool ParryGrab(uintptr_t enemy) { return ParryAttack(actors::Player(0), enemy, 0x40000000, false); }

bool ParryAttack(uintptr_t player, uintptr_t attacker, uint32_t flags, bool fromReaction) {
    const bool knifeOn = KnifeParryOn(), re6On = Re6ParryOn();
    if ((!knifeOn && !re6On) || !player || player != actors::Player(0) || Player1InNeckBreak()) return false;
    if (!g_parryDying.load(std::memory_order_relaxed) && Player1Dying()) return false;
    if (!g_parryHeld.load(std::memory_order_relaxed) && Held(player)) return false;
    const DWORD now = GetTickCount();
    // The same blow again (its SubHealth after the reaction, or the reverse), or that enemy during the riposte.
    uint32_t action = 0;
    const bool riposte = g_pendingStagger && mem::Read(player + 0x10E0, action) &&
                         (action == kSlashAction || PlayerInWeskerMove(player));
    if (g_parriedTick && (Since(g_parriedTick) < 400 || riposte) &&
        (!attacker || BlowOwner(attacker) == BlowOwner(g_parriedAttacker)))
        return true;
    if (g_parriedTick && Since(g_parriedTick) < 400 && attacker)
        LOG("parry: a hit of %s %08X %u ms after parrying %08X, not the same blow (%s)", ClassName(attacker),
            static_cast<unsigned>(attacker), static_cast<unsigned>(Since(g_parriedTick)),
            static_cast<unsigned>(g_parriedAttacker), fromReaction ? "reaction" : "damage");
    // Its blow already taken (user, 2026-10-31: "hit, then the parry's ring and sound play though it missed"): its attack
    // shape stays armed a while after the contact, the sphere saw it after his hit reaction.
    if (BlowAlreadyTaken(attacker)) {
        static uintptr_t logged = 0;
        if (logged != attacker) LOG("parry: %s %08X's blow already taken, no late parry", ClassName(attacker), static_cast<unsigned>(attacker));
        logged = attacker;
        return false;
    }
    const bool knifePress = knifeOn && g_pressTick &&
                            Since(g_pressTick) <= static_cast<DWORD>(g_window.load(std::memory_order_relaxed) * 1000.0f);
    const bool re6Press = re6On && g_re6PressTick && !KnifeParryOnly(attacker) &&
                          Since(g_re6PressTick) <= static_cast<DWORD>(g_re6Window.load(std::memory_order_relaxed) * 1000.0f);
    if (!knifePress && !re6Press) return false;
    if (!attacker) return false;
    // Blows of a listed enemy (melee) get the full parry; anything else that hits player 1 (thrown axes, crossbow
    // bolts, parasites - user, 2026-10-15) is stopped too, without stagger for the thrower. Not the players' own
    // hits, not bullets (flag 0x00080000).
    const auto enemies = actors::Enemies();
    // A Majini farther than any of its blows reaches threw what hit (user, 2026-10-31: a bottle parried with Jill's
    // throw brought its thrower from 5 m): parried as a projectile.
    float chris[3]{}, thrower[3]{};
    const bool farMajini = IsMajini(attacker) && mem::SafeRead(reinterpret_cast<void*>(attacker + 0x30), thrower, sizeof(thrower)) &&
                           mem::SafeRead(reinterpret_cast<void*>(player + 0x30), chris, sizeof(chris)) &&
                           std::hypot(thrower[0] - chris[0], thrower[2] - chris[2]) > kThrowerReach;
    const bool melee = !farMajini && std::find(enemies.begin(), enemies.end(), attacker) != enemies.end();
    const bool projectile = farMajini || ProjectileUnit(attacker);
    const char* attackerClass = ClassName(attacker);
    const bool stageObject = !melee && attackerClass && std::strncmp(attackerClass, "uOm", 3) == 0;
    const bool hazard = ((flags & kBlastHit) || stageObject) && !g_parryHazards.load(std::memory_order_relaxed);
    const bool bullet = (flags & 0x00080000) && !ProjectileUnit(attacker);  // a bolt is parried
    if (attacker == actors::Player(0) || attacker == actors::Player(1) || bullet || hazard) {
        LOG("parry refused: %s %08X (%s), flags %08X",
            bullet ? "bullet of" : hazard ? "blast, fire or electricity of" : "hit of",
            static_cast<unsigned>(attacker), attackerClass, flags);
        return false;
    }
    const bool re6 = re6Press && (!knifePress || g_re6PressTick >= g_pressTick);  // the latest press decides
    if (re6) g_re6PressTick = 0;
    else g_pressTick = 0;  // one blow per press
    g_parriedTick = Stamp();
    g_parriedAttacker = attacker;
    int16_t health = 0;
    // From 0 too: the chainsaw's one-hit kill (its decapitation refused, ParryRefusesEnemyAction).
    if (fromReaction && mem::Read(player + 0x1364, health) && health < g_lastHealth && (health > 0 || IsChainsaw(attacker)))
        mem::Write(player + 0x1364, g_lastHealth);  // the blow was already taken off before its reaction
    // The automatic parry's own riposte (its press), else the option's.
    const int kind = !re6 && g_pressAuto ? g_autoKind.load(std::memory_order_relaxed) : RiposteKind();
    if (!re6) g_pressAuto = false;
    // Not Jill's throw (always thrown in her second part) even with the slash riposte.
    const bool grab = (flags & 0x40000000) != 0;  // a grab parried (ParryGrab, or its hit)
    ParryVisual(player, attacker, !re6 && kind == kRiposteKnife && !(melee && IsJill2(attacker) && WeskerCanThrow(attacker)),
                grab, re6 || kind != kRiposteGuard);
    // "Endurance" (user, 2026-10-26): the RE6 parry and Jill's throw cost it as a free melee move; Wesker's guard (RE7)
    // only with "Endurance stricte"; the knife parry never.
    if (re6) StaminaSpendRe6Counter();
    else if (kind == kRiposteThrow) StaminaSpend();
    else if (kind == kRiposteGuard && StaminaStrict()) StaminaSpendRe7Guard();
    // "Durabilité du couteau" (user, 2026-10-27): the knife parry wears the knife, not Wesker's guard, Jill's throw nor
    // the RE6 counter.
    if (!re6 && kind == kRiposteKnife) KnifeWearParry();
    // A parry cuts the move playing (user, 2026-11-09: "despite the RE6 counter's sound and ring, Chris plays his free melee instead
    // of the counter", then "if the parry fires, the move playing is cut"): the RE6 melee started from the same press, or any trainer
    // move, stopped so that the riposte starts.
    CutMoveForParry();
    if (re6) {
        // Chris's counter on Wesker towards the attacker; the attacker hurt and put in the parried state.
        const int riposte = PickRe6Riposte();
        const int counter = riposte == kRe6ShevaCounter ? 1 : 0;  // the counter's variant (a projection refused: Chris's)
        float at[3]{};
        if (melee && mem::SafeRead(reinterpret_cast<void*>(attacker + 0x30), at, sizeof(at))) FacePlayerTowards(player, at);
        CancelQuickShot();   // the same press raised the weapon ("Tir sans viser")
        CancelQuickKnife();  // or drew the knife (the quick knife on the attack key)
        if (melee && grab && g_re6Sweep.load(std::memory_order_relaxed) && PlayRe6Sweep(attacker)) {
            PlayParrySound(false, -1);
            LOG("parry RE6: the grab of %s %08X swept", ClassName(attacker), static_cast<unsigned>(attacker));
            return true;
        }
        if (WeskerSweepRoll(player, attacker, melee, "RE6")) {
            PlayParrySound(true, -1);
            return true;
        }
        // Standing: no enemy (a projectile), or a reaction "Aucune" of the RE6 lists (Executioner, Wesker, Jill), or no
        // arm hit possible (below).
        const bool standing = !melee ||
                        (IsExecutioner(attacker) && g_executionerReaction.load(std::memory_order_relaxed) == 3) ||
                        (IsWesker(attacker) && g_weskerReaction.load(std::memory_order_relaxed) == 3) ||
                        (IsJill2(attacker) && g_jillRe6Reaction.load(std::memory_order_relaxed) >= kJillReactionCount);
        if (melee && IsWesker(attacker) && g_weskerReaction.load(std::memory_order_relaxed) == 2) {
            PlayParrySound(false, -1);
            uintptr_t target = 0;
            mem::Read(attacker + 0x1BA4, target);
            if (target != player) PlayCounterRiposte(attacker, counter);
            Request(attacker, kWeskerReactions[2], 1);
            LOG("parry RE6: Wesker %08X countered natively (his target %08X%s)", static_cast<unsigned>(attacker),
                static_cast<unsigned>(target), target == player ? ", the game's counter" : ", Chris's RE6 counter");
            return true;
        }
        // A projectile (a bolt, an axe): Wesker's left dodge, as with the guard or the throw (user, 2026-10-30: parried
        // otherwise than with the knife, a bolt is dodged).
        if (!melee && projectile && StartDodgeMove("melee.free_wesker_dodge1")) {
            PlayParrySound(true, -1);
            LOG("parry RE6: %s %08X (flags %08X) stopped, Wesker's left dodge", ClassName(attacker),
                static_cast<unsigned>(attacker), flags);
            return true;
        }
        // The knife in hand: RE6's knife counter, whatever riposte is chosen (user, 2026-11-10: "the RE6 counter with the knife in hand did
        // not give a knife counter, Chris made his own" - his riposte was "Contre de Chris", the punch).
        // An armed Majini's blow (user, 2026-11-10: "the special counters"): RE6's machete disarm or gun counter, before the knife counter.
        if (melee && PlaySpecialCounter(player, attacker)) {
            PlayParrySound(false, -1);
            LOG("parry RE6 (%s): %s %08X, a special counter", fromReaction ? "reaction" : "damage", ClassName(attacker),
                static_cast<unsigned>(attacker));
            return true;
        }
        if (melee && PlayKnifeCounter(player, attacker)) {
            PlayParrySound(false, -1);
            LOG("parry RE6 (%s): %s %08X, the knife counter (a knife in hand)", fromReaction ? "reaction" : "damage", ClassName(attacker),
                static_cast<unsigned>(attacker));
            return true;
        }
        // The projections: Chris's (Jill's throw, its back-up on an enemy it cannot take) or Josh's counter on the attacker;
        // the counter when neither can.
        if (melee && riposte == kRe6ChrisThrow &&
            (WeskerCanThrow(attacker) || !BackupFor("melee.free_jill_throw", attacker).empty())) {
            PlayParrySound(false, -1);
            RequestThrowRiposte(attacker);
            LOG("parry RE6 (%s): %s %08X, Chris's projection", fromReaction ? "reaction" : "damage", ClassName(attacker),
                static_cast<unsigned>(attacker));
            return true;
        }
        if (melee && riposte >= kRe6PiersCounter && PlayRe6Counter(riposte - kRe6PiersCounter, attacker)) {
            PlayParrySound(false, -1);
            LOG("parry RE6 (%s): %s %08X, RE6's counter %d", fromReaction ? "reaction" : "damage", ClassName(attacker),
                static_cast<unsigned>(attacker), riposte - kRe6PiersCounter);
            return true;
        }
        if (melee && riposte == kRe6JoshCounter && PlayJoshCounter(attacker)) {
            PlayParrySound(false, -1);
            LOG("parry RE6 (%s): %s %08X, Josh's counter", fromReaction ? "reaction" : "damage", ClassName(attacker),
                static_cast<unsigned>(attacker));
            return true;
        }
        PlayCounterRiposte(melee ? attacker : 0, counter);  // the character's counter (Chris's, Sheva's)
        if (!melee) {
            PlayParrySound(true, -1);
            LOG("parry RE6: %s %08X (flags %08X) stopped, counter on the spot", ClassName(attacker),
                static_cast<unsigned>(attacker), flags);
            return true;
        }
        g_pendingStagger = attacker;  // its blows refused during the counter
        // The Executioner family (uEm20, Lost in Nightmares' uEm27): its reaction of the list at the parry (user,
        // 2026-10-26: the axe man's grab took Chris during the counter, its punch came too late), its damage at the punch.
        if (IsExecutioner(attacker)) {
            const int reaction = g_executionerReaction.load(std::memory_order_relaxed);
            if (reaction >= 0 && reaction < 3) Request(attacker, kExecutionerReactions[reaction], 1);
            PlayParrySound(standing, -1);
            LOG("parry RE6 (%s): %s %08X, its reaction %d at the parry", fromReaction ? "reaction" : "damage",
                ClassName(attacker), static_cast<unsigned>(attacker), reaction);
            return true;
        }
        // At the parry only an arm hit that stops its attack in place (user, 2026-10-23: the recoil at the parry took it
        // away from Chris); its damage and the parried state come with Chris's punch (CounterPunchLands, from
        // wesker_moves.cpp at the counter's punch frame).
        const bool flinched = LightFlinch(attacker);
        PlayParrySound(standing || !flinched, -1);
        LOG("parry RE6 (%s): %s %08X, %s, its hit at the punch", fromReaction ? "reaction" : "damage", ClassName(attacker),
            static_cast<unsigned>(attacker), flinched ? "arm hit" : "no reaction");
        return true;
    }
    if (!melee) {
        // A projectile parried with Wesker's guard or Jill's throw as the riposte: Wesker's dodge to the left instead
        // (user, 2026-10-30: "une des anims de Wesker, esquive gauche"); with the knife slash, the slash.
        if (kind != kRiposteKnife && projectile && StartDodgeMove("melee.free_wesker_dodge1")) {
            PlayParrySound(true, kind);
            g_riposteTick = Stamp();
            LOG("parry (%s): %s %08X (flags %08X) stopped, Wesker's left dodge", fromReaction ? "reaction" : "damage",
                ClassName(attacker), static_cast<unsigned>(attacker), flags);
            return true;
        }
        PlayParrySound(true, kind);
        if (kind == kRiposteGuard) {
            PlayGuardRiposte();
        } else {
            AimInFront(player);
            Request(player, kSlashAction, 0);
        }
        g_riposteTick = Stamp();
        LOG("parry (%s): %s %08X (flags %08X) stopped, slash on the spot", fromReaction ? "reaction" : "damage",
            ClassName(attacker), static_cast<unsigned>(attacker), flags);
        return true;
    }
    if (WeskerSweepRoll(player, attacker, melee, "knife")) {
        PlayParrySound(true, kind);
        return true;
    }
    // Jill's second part: the knife parry and Wesker's guard throw her too (user, 2026-10-26): her native Caught, after
    // which she stays down under the device threshold (jill_device.cpp); a reaction of the trainer's could not hold her.
    if ((kind == kRiposteThrow || IsJill2(attacker)) && WeskerCanThrow(attacker)) {
        PlayParrySound(false, -1);  // Jill's throw: RE6's
        RequestThrowRiposte(attacker);
        LOG("parry (%s, health %d / before %d): %s %08X, Jill's throw", fromReaction ? "reaction" : "damage", health,
            g_lastHealth, ClassName(attacker), static_cast<unsigned>(attacker));
        return true;
    }
    // An enemy the throw cannot take (user, 2026-10-31: a flailing parasite parried with the throw got the knife slash,
    // its back-up being Help): the throw's back-up ("Utiliser les back-up"), played by wesker_moves.cpp as for a throw.
    if (kind == kRiposteThrow && !BackupFor("melee.free_jill_throw", attacker).empty()) {
        PlayParrySound(false, -1);
        RequestThrowRiposte(attacker);
        LOG("parry (%s, health %d / before %d): %s %08X, the throw's back-up", fromReaction ? "reaction" : "damage", health,
            g_lastHealth, ClassName(attacker), static_cast<unsigned>(attacker));
        return true;
    }
    // Josh's counter (user, 2026-11-01: "Projection de Josh uses Josh's Finisher Front to parry the enemy's attack"): the
    // free melee's "Josh Contre" (his Finisher Front borrowed, its own settings in Josh's table) on the attacker; the
    // knife slash below if it is refused (stamina, an enemy out of reach...).
    if (kind == kRiposteJosh) {
        if (PlayJoshCounter(attacker)) {
            PlayParrySound(false, kind, grab);
            g_riposteTick = Stamp();
            LOG("parry (%s, health %d / before %d): %s %08X, Josh's counter", fromReaction ? "reaction" : "damage", health,
                g_lastHealth, ClassName(attacker), static_cast<unsigned>(attacker));
            return true;
        }
        LOG("parry: Josh's counter refused on %s %08X, the knife slash", ClassName(attacker), static_cast<unsigned>(attacker));
    }
    // The chosen state, the head stagger for a Majini that has none of them (a parasite out).
    const int state = (kind == kRiposteGuard ? g_parryStateRe7 : g_parryStateRe4).load(std::memory_order_relaxed);
    bool staggered = ForceMajiniState(attacker, state) || HeadStagger(attacker);
    if (!staggered && IsExecutioner(attacker)) {
        const int reaction = g_executionerReaction.load(std::memory_order_relaxed);
        if (reaction >= 0 && reaction < 3) {
            Request(attacker, kExecutionerReactions[reaction], 1);
            staggered = true;
        }
    }
    if (!staggered && IsChainsaw(attacker)) {
        const int reaction = g_chainsawReaction.load(std::memory_order_relaxed);
        if (reaction >= 0 && reaction < 4) {
            const bool legs = kChainsawReactions[reaction] == kChainsawLegs;
            Request(attacker, legs ? kChainsawHit : kChainsawReactions[reaction], 1);
            if (legs) {
                g_chainsawLegsTick = GetTickCount();
                g_chainsawLegs = attacker;
            }
            staggered = true;
        }
    }
    if (!staggered && IsReaper(attacker)) {
        const int reaction = g_reaperReaction.load(std::memory_order_relaxed);
        if (reaction >= 0 && reaction < kReaperReactionCount) {
            Request(attacker, kReaperReactions[reaction], 1);
            staggered = true;
        }
    }
    if (!staggered && IsWesker(attacker)) {
        const int reaction = g_weskerKnifeReaction.load(std::memory_order_relaxed);
        if (reaction >= 0 && reaction < 3) {
            uintptr_t target = 0;
            mem::Read(attacker + 0x1BA4, target);
            Request(attacker, kWeskerReactions[reaction], 1);
            staggered = true;
            if (reaction == 2 && target == player) {  // the game's own counter takes Chris: no riposte over it
                PlayParrySound(false, kind, grab);
                LOG("parry (%s): Wesker %08X countered natively", fromReaction ? "reaction" : "damage",
                    static_cast<unsigned>(attacker));
                return true;
            }
        }
    }
    PlayParrySound(!staggered, kind, grab);
    if (grab && kind == kRiposteKnife) GrabParryDamage(player, attacker);
    if (kind == kRiposteGuard) {
        float at[3]{};
        if (mem::SafeRead(reinterpret_cast<void*>(attacker + 0x30), at, sizeof(at))) FacePlayerTowards(player, at);
        PlayGuardRiposte();
    } else {
        AimAt(player, attacker);
        Request(player, kSlashAction, 0);
    }
    g_pendingStagger = attacker;
    g_pendingTick = now;
    g_riposteTick = Stamp();
    LOG("parry (%s, health %d / before %d): %s %08X, %s, riposte", fromReaction ? "reaction" : "damage", health,
        g_lastHealth, ClassName(attacker), static_cast<unsigned>(attacker), staggered ? "recoil" : "no stagger (not a Majini)");
    return true;
}

// A parried blow drew player 1's blood all the same (user, 2026-10-21): the hit system spawns the target's common
// effects (sBioEffect VA 0x7757F0, flags 0x0701xxxx for a Majini's blow on Chris: 0x5100 / 0x5500 / 0x5800 / 0x7200)
// about 9 ms before the parry is decided in the player's reaction or SubHealth (logged). So the blood on player 1 is
// refused (quick_knife.cpp hook) when a blow now would be parried (the window open after a press, his state allowing a
// parry) or a parry just happened (0.4 s). The weapon's impact sound (e.g. se_wp55 2 on the Majini's weapon) and his
// pain sounds (se_pl00 0x2E / 0x05) did not play on the parried blows: the parry already stops them.
bool ParrySparesBlood(uintptr_t player) {
    const bool knifeOn = KnifeParryOn(), re6On = Re6ParryOn();
    if ((!knifeOn && !re6On) || !player || player != actors::Player(0)) return false;
    if (g_parriedTick && Since(g_parriedTick) < 400) return true;
    if (!g_parryDying.load(std::memory_order_relaxed) && Player1Dying()) return false;
    if (!g_parryHeld.load(std::memory_order_relaxed) && Held(player)) return false;
    // A projectile's hit with an automatic parry on: parried in the reaction that follows (AutoParryProjectile).
    if ((g_autoKnifeOn.load(std::memory_order_relaxed) || g_autoRe6On.load(std::memory_order_relaxed)) &&
        PendingProjectileHit(player))
        return true;
    return (knifeOn && g_pressTick &&
            Since(g_pressTick) <= static_cast<DWORD>(g_window.load(std::memory_order_relaxed) * 1000.0f)) ||
           (re6On && g_re6PressTick &&
            Since(g_re6PressTick) <= static_cast<DWORD>(g_re6Window.load(std::memory_order_relaxed) * 1000.0f));
}

// rocket_catch.cpp: the RPG Majini's rocket within reach. Caught on the knife parry's press only, whatever its riposte
// (slash, guard or throw: the catch is played for all), not on the RE6 parry's (user, 2026-10-25: the attack key also
// fires, the rocket was caught "without a parry"). A catch is a parry: its sound and ring, one per press.
bool ParryCatchesRocket(uintptr_t player, uintptr_t rocket) {
    if (!g_parryOn.load(std::memory_order_relaxed) || RiposteKindNow() < 0 || !player || player != actors::Player(0))
        return false;
    if (!g_parryDying.load(std::memory_order_relaxed) && Player1Dying()) return false;
    if (!g_parryHeld.load(std::memory_order_relaxed) && Held(player)) return false;
    if (!g_pressTick || Since(g_pressTick) > static_cast<DWORD>(g_window.load(std::memory_order_relaxed) * 1000.0f))
        return false;
    g_pressTick = 0;
    g_parriedTick = Stamp();
    g_parriedAttacker = rocket;
    PlayParrySound(false, RiposteKind());
    ParryVisual(player, rocket, RiposteKind() == kRiposteKnife, false,
                RiposteKind() != kRiposteGuard);
    if (RiposteKind() == kRiposteKnife) KnifeWearParry();
    return true;
}

// rocket_catch.cpp: with the knife slash as the riposte the rocket is sent back (user, 2026-10-25: "a basic slash like
// the one of the knife aim, and the rocket back to its sender"); with the guard or Jill's throw it is caught.
bool ParryRiposteIsSlash() { return RiposteKind() == kRiposteKnife; }

// That slash: the knife aim's own attack (pl_act_knife_attack 0x27).
void PlayKnifeAimSlash(uintptr_t player) {
    constexpr uint32_t kKnifeAttack = 0x27;
    Request(player, kKnifeAttack, 0);
}

// The Executioner's catch (AtkCatch 0x17, its hit +0x3C 0x40400010) lifts player 1 by itself (AtkHanging 0x18, his
// event) in the frame its hit lands, before his hit reaction decides the parry (logged 2026-10-23: the parry came,
// player 1 hung all the same; user: no parry works on that grab). With the parry window open (or this Executioner just
// parried) the lift is refused (action_log.cpp, enemy request hook), as is its execution kill (AtkExecKill 0x31 after
// AtkExecCatch 0x30); the parry then makes it kneel (or the throw).
// The chainsaw Majini's one-hit kill the same way (logged 2026-10-23, user: no parry against it): its swing 0x14 touches
// player 1 (his health to 0 with the 10000 hit), its AI asks the decapitation 0x15 in the same frame, then his hit
// reaction parries - too late. With the window open the decapitation is refused, and the parry gives his health back
// even from 0 (the reaction comes before his death is decided).
bool IsChainsaw(uintptr_t enemy) {
    uintptr_t vtable = 0;
    return enemy && mem::Read(enemy, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB75B20));  // uEm22
}

// The spider parasite's catch (uEm0b, vtable RVA 0xB6E318; user 2026-10-27: grabbed although parried): its AtkCatchHit
// (cEm0bActAtkCatchHit, by name in its action table +0x10D0 / +0x10DC) is asked in the frame its leap touches player 1
// and starts his event (cEm0bActAtkCatchHit_PL) before his hit reaction parries (logged: the parry and its riposte came,
// he was held all the same). Refused like the Executioner's lift.
// The class name of a unit's action `action` (its table +0x10D0 count / +0x10DC objects), nullptr if none.
const char* ActionClass(uintptr_t unit, int action) {
    uintptr_t table = 0, object = 0;
    uint32_t count = 0;
    if (action < 0 || !mem::Read(unit + 0x10D0, count) || static_cast<uint32_t>(action) >= count || count > 0x200 ||
        !mem::Read(unit + 0x10DC, table) || !table || !mem::Read(table + 4 * action, object) || !object)
        return nullptr;
    return ClassName(object);
}

bool IsSpiderCatch(uintptr_t enemy, int action) {
    uintptr_t vtable = 0;
    if (!enemy || !mem::Read(enemy, vtable) || vtable != reinterpret_cast<uintptr_t>(mem::Rva(0xB6E318))) return false;
    const char* name = ActionClass(enemy, action);
    return name && std::strcmp(name, "cEm0bActAtkCatchHit") == 0;
}

// The parried spider falls the way it falls off a head at the end of its hold (user: "that fall, it is perfect"): its
// AtkCatchEnd, asked on the next frame (not from inside its refused request). That fall also gives player 1 his
// release startle plaga_stick_out 0x83 (motion 0x063; user: it replaced his riposte): refused for 1.5 s
// (ParryRefusesPlayerAction, action_log.cpp's player request hook).
std::atomic<uintptr_t> g_spiderFall{ 0 };
std::atomic<DWORD> g_spiderFallTick{ 0 };

void TickSpiderFall() {
    const uintptr_t spider = g_spiderFall.exchange(0);
    if (!spider) return;
    bool alive = false;
    for (uintptr_t e : actors::Enemies()) alive = alive || e == spider;
    uint32_t count = 0;
    if (!alive || !mem::Read(spider + 0x10D0, count) || count > 0x200) return;
    for (uint32_t k = 0; k < count; ++k)
        if (const char* name = ActionClass(spider, static_cast<int>(k)); name && std::strcmp(name, "cEm0bActAtkCatchEnd") == 0) {
            EnsurePlayerRequestHook();
            g_spiderFallTick = Stamp();
            Request(spider, k, 1);
            LOG("parry: spider parasite %08X falls (AtkCatchEnd %X)", static_cast<unsigned>(spider), k);
            return;
        }
}

// action_log.cpp's player request hook: a player's release startle 0x83 within 1.5 s of a parried spider's fall (players
// 1 and 2 alike: user, 2026-10-27).
bool ParryRefusesPlayerAction(uintptr_t player, int action) {
    const DWORD tick = g_spiderFallTick.load(std::memory_order_relaxed);
    if (action != 0x83 || !tick || Since(tick) >= 1500 || actors::PlayerIndex(player) < 0) return false;
    LOG("parry: player %d's release startle 0x83 refused (parried spider falling)", actors::PlayerIndex(player) + 1);
    return true;
}

bool ParryRefusesEnemyAction(uintptr_t enemy, int action) {
    if (!KnifeParryOn() && !Re6ParryOn()) return false;
    if (IsSpiderCatch(enemy, action)) {
        const uintptr_t p1 = actors::Player(0);
        // Its leap keeps touching him for a while: refused during the riposte too (1.5 s after the parry).
        const bool parried = g_parriedTick && Since(g_parriedTick) < 1500 && enemy == g_parriedAttacker;
        if (!parried && !ParrySparesBlood(p1)) return false;
        LOG("parry: spider parasite %08X's catch %X refused (%s)", static_cast<unsigned>(enemy), action,
            parried ? "parried" : "window open");
        g_spiderFall = enemy;
        return true;
    }
    const bool lift = (action == 0x18 || action == 0x31) && IsExecutioner(enemy);
    const bool decapitation = action == 0x15 && IsChainsaw(enemy);
    const bool reaperCatch = action == static_cast<int>(kReaperCatch) && IsReaper(enemy);
    if ((action == 0x57 || (action >= 0x52 && action <= 0x54)) && IsWesker(enemy)) {
        // Asked in the frame his blow lands, before player 1's hit reaction decides the parry (logged 2026-10-25: the
        // sweep's 0x57 took him into its event with the back roll asked): refused with the window open too, near him.
        const uintptr_t p1 = actors::Player(0);
        float a[3]{}, b[3]{};
        const bool close = p1 && mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), a, sizeof(a)) &&
                          mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), b, sizeof(b)) &&
                          (a[0] - b[0]) * (a[0] - b[0]) + (a[2] - b[2]) * (a[2] - b[2]) < 250.0f * 250.0f;
        const bool parried = g_parriedTick && Since(g_parriedTick) < 2000 && enemy == g_parriedAttacker;
        if (!parried && !(close && ParrySparesBlood(p1))) return false;
        LOG("parry: Wesker %08X's %s %X refused (%s)", static_cast<unsigned>(enemy), action == 0x57 ? "sweep hit" : "grab",
            action, parried ? "parried" : "window open");
        return true;
    }
    // Lost in Nightmares' Wesker's own holds (uEmA2, user 2026-10-26: "his attacks and his grab parried"; their player
    // events by class name: cEmA2ActSyoteOk_PL / SyoteEsc_PL his grab, SyutoDie_PL his killing chop): FirstAtk 0x80 / 0x87
    // (the grab after FirstAtkBgn's lunge) and Syuto_OK 0x83 (the chop landed). Parried when asked, as Jill's catch: the
    // window open within 2.5 m, refused for 2 s after the parry.
    if ((action == 0x80 || action == 0x87 || action == 0x83) && IsLinWesker(enemy)) {
        const uintptr_t p1 = actors::Player(0);
        float a[3]{}, b[3]{};
        const bool close = p1 && mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), a, sizeof(a)) &&
                           mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), b, sizeof(b)) &&
                           (a[0] - b[0]) * (a[0] - b[0]) + (a[2] - b[2]) * (a[2] - b[2]) < 250.0f * 250.0f;
        if (g_parriedTick && Since(g_parriedTick) < 2000 && enemy == g_parriedAttacker) {
            LOG("parry: Lost in Nightmares' Wesker %08X's hold %X refused (parried)", static_cast<unsigned>(enemy), action);
            return true;
        }
        if (!close || !ParrySparesBlood(p1) || !ParryAttack(p1, enemy, 0x40000000, false)) return false;
        LOG("parry: Lost in Nightmares' Wesker %08X's hold %X parried", static_cast<unsigned>(enemy), action);
        return true;
    }
    if (action == static_cast<int>(kJillCatch) && IsJill2(enemy)) {
        const uintptr_t p1 = actors::Player(0);
        float a[3]{}, b[3]{};
        const bool close = p1 && mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), a, sizeof(a)) &&
                           mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), b, sizeof(b)) &&
                           (a[0] - b[0]) * (a[0] - b[0]) + (a[2] - b[2]) * (a[2] - b[2]) < 250.0f * 250.0f;
        if (g_parriedTick && Since(g_parriedTick) < 2000 && enemy == g_parriedAttacker) {
            LOG("parry: Jill %08X's catch refused (parried)", static_cast<unsigned>(enemy));
            return true;
        }
        // The parry itself here, as a grab hit would give it (her catch holds him before any hit).
        if (!close || !ParrySparesBlood(p1) || !ParryAttack(p1, enemy, 0x40000000, false)) return false;
        LOG("parry: Jill %08X's catch parried", static_cast<unsigned>(enemy));
        return true;
    }
    if (!lift && !decapitation && !reaperCatch) return false;
    const uintptr_t p1 = actors::Player(0);
    const bool parried = g_parriedTick && Since(g_parriedTick) < 600 && enemy == g_parriedAttacker;
    // The chainsaw and the Reaper are parried by the knife parry only (ParryAttack): its window alone spares their
    // execution. Nor is it taken during the riposte's Wesker / Jill move (player 1 invulnerable there).
    const bool knifeWindow = KnifeParryOn() && g_pressTick &&
                             Since(g_pressTick) <= static_cast<DWORD>(g_window.load(std::memory_order_relaxed) * 1000.0f);
    const bool knifeOnly = decapitation || reaperCatch;
    const bool spared = knifeOnly ? (knifeWindow && ParrySparesBlood(p1)) || WeskerMoveInvulnerable(p1)
                                  : ParrySparesBlood(p1);
    if (!parried && !spared) return false;
    LOG("parry: %s %08X's %s %X refused", lift ? "Executioner" : decapitation ? "chainsaw" : "Reaper",
        static_cast<unsigned>(enemy), lift ? "lift" : decapitation ? "decapitation" : "catch", action);
    return true;
}

bool MajiniHeadFlash(uintptr_t enemy) { return HeadStagger(enemy); }

// Enemy request hook (action_log.cpp): the parried chainsaw Majini's leg stagger once its hit reaction ended (its update
// RVA 0x478460 marks the end with the motion's, then its AI asks), not after a death or 4 s.
void ChainsawLegsAfterHit(uintptr_t enemy, int* action) {
    const uintptr_t chained = g_chainsawLegs.load(std::memory_order_relaxed);
    if (!chained || enemy != chained || *action == static_cast<int>(kChainsawHit)) return;
    if (GetTickCount() - g_chainsawLegsTick.load() > 4000 || (*action >= 0x1F && *action <= 0x22)) {
        g_chainsawLegs = 0;
        return;
    }
    uint32_t current = 0;
    uint8_t flags = 0;
    float frame = 0, length = 0;
    mem::Read(enemy + 0x10E0, current);
    mem::Read(enemy + 0x388, flags);
    mem::Read(enemy + 0x398, frame);
    mem::Read(enemy + 0x39C, length);
    if (current != kChainsawHit || !((flags & 4) || (length > 0 && frame >= length - 1.0f))) return;
    g_chainsawLegs = 0;
    LOG("parry: chainsaw %08X's leg stagger after its hit reaction (asked %X)", static_cast<unsigned>(enemy), *action);
    *action = static_cast<int>(kChainsawLegs);
}

// Enemy request hook (action_log.cpp): the big Majini (cEm10 classes: Gatling uEm23, giant uEm24, fat man uEm13) are never
// blown off (user, 2026-10-23: the head flash after a parry sent them flying like an ordinary Majini; only Jill's throw
// may throw them). cEm10ActDmgFlyoff 0x73 becomes their own heavy hit reaction, the one a melee move gets in the game
// (actions.log): DmgGG 0x78 (giant after its prompt's move 0x3F, Gatling after Leg Front), DmgFat 0x77 (fat man).
// The free melee table's "Projection" (melee_tuning.cpp, user 2026-10-31: "it may be interesting that projection forces
// those enemies to fly"): the blow-off it asks goes through, once.
std::atomic<uintptr_t> g_tableBlowOff{ 0 };
void AllowBigBlowOff(uintptr_t enemy) { g_tableBlowOff = enemy; }

void BigMajiniKeepsFooting(uintptr_t enemy, int* action) {
    if (*action != 0x73 || !enemy || enemy == JillThrownUnit()) return;
    if (g_tableBlowOff.load() == enemy) {
        g_tableBlowOff = 0;
        return;
    }
    uintptr_t vtable = 0, actions = 0, act = 0;
    if (!mem::Read(enemy, vtable)) return;
    int heavy = 0;
    switch (vtable - reinterpret_cast<uintptr_t>(mem::Base())) {
    case 0xB72680: case 0xB73068: heavy = 0x78; break;  // Gatling uEm23, giant uEm24
    case 0xB732E0: heavy = 0x77; break;                 // fat man uEm13
    default: return;
    }
    if (!mem::Read(enemy + 0x10DC, actions) || !actions || !mem::Read(actions + 4 * heavy, act) || !act) return;
    LOG("big Majini %08X: blow-off 0x73 replaced by its own reaction %X", static_cast<unsigned>(enemy), heavy);
    *action = heavy;
}


// A Majini's hit taken from a point: the yaw its AI turns it to, +0x1B74 (recorded 2026-10-23: the direction of player 1
// for the enemies near him), read by the stumbles' setups (the back one turns it towards it, the front one away).
void HitEnemyFrom(uintptr_t enemy, const float* point) {
    float e[3]{};
    if (!enemy || !mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), e, sizeof(e))) return;
    const float dx = point[0] - e[0], dz = point[2] - e[2];
    if (dx * dx + dz * dz >= 1.0f) mem::Write(enemy + 0x1B74, std::atan2(dx, dz));
}

// The RE6 parry's punch lands (wesker_moves.cpp, at the counter's punch frame): head-flash-like damage (the kill's credit
// to player 1, as OnHit does), then the parried state, or at 0 blown off like by a head flash (user, 2026-10-23: the
// flinch then a death on the spot looked odd; a big Majini only dies, it is never blown off), both from Chris's body
// (user: the hit from his centre, not his origin); the Executioner's chosen reaction.
void CounterPunchLands(uintptr_t enemy) {
    const uintptr_t p1 = actors::Player(0);
    int16_t before = 0;
    if (!p1 || !enemy || !mem::Read(enemy + 0x1364, before) || before <= 0) return;
    // Jill: her device instead of her health ("Corps à corps libre : le device de Jill", jill_device.cpp), her health
    // frozen, never her last point (as the trainer's moves).
    const int device = JillDeviceMeleeDamage(enemy);
    if (device) DamageJillDevice(enemy, device, "the RE6 parry's punch");
    // The free melee line of the counter playing ("Chris Contre RE6" / "Sheva Contre RE6", user 2026-11-01): its damage,
    // critical, one-hit kill and reaction, as the trainer's moves get theirs (wesker_moves.cpp Strike).
    MoveTuning tuning;
    const bool tuned = GetMoveTuning(CounterTuningId(), tuning) && !tuning.HitsDefault();
    int amount = kRe6Damage;
    if (tuned) {
        amount = static_cast<int>(static_cast<float>(amount) * tuning.damage + 0.5f);
        if (tuning.critical >= 0.0f && RollPercent(tuning.critical)) amount = QueueHeadBlow(enemy) ? before : amount * 5;
        if (tuning.kill) amount = before;
        amount = std::clamp(amount, 0, 32767);
    }
    const int16_t punch = LinWeskerDamage(enemy, static_cast<int16_t>(amount));
    int16_t left = WeskerKeepsHealth(enemy) || device || JillKeepsHealth(enemy)
                       ? before
                       : static_cast<int16_t>(before > punch ? before - punch : 0);
    if (left < 1 && IsJill2(enemy)) left = 1;
    mem::Write(enemy + 0x1364, left);
    if (!left) NoteTrainerMeleeKill(enemy);  // its Mercenaries time bonus, as a native bare-hand kill's
    uint8_t pad = 0;
    if (mem::Read(p1 + 0x2D9C, pad)) mem::Write<int32_t>(enemy + 0x136C, pad);
    bool reacted = false;
    float body[3]{};
    const bool bodyAt = PlayerBody(p1, body);
    if (left > 0 && FlyingParasiteReact(enemy, "the RE6 counter")) {
        reacted = true;  // a flying parasite (Kipepeo, knife.cpp): it lets go of a player it holds, else it is blown away
    } else if (tuned && tuning.reaction != kReactionGame && !IsExecutioner(enemy)) {
        reacted = TrainerMoveReact(p1, enemy, tuning.reaction);
    } else if (IsMajini(enemy)) {
        if (bodyAt) HitEnemyFrom(enemy, body);
        if (left > 0)
            reacted = ForceMajiniState(enemy, g_parryState.load(std::memory_order_relaxed)) || HeadStagger(enemy);
        else if (bodyAt && !BigEnemyClass(enemy))
            reacted = MajiniBlowOffFrom(enemy, body, 2);  // a head hit
        if (!reacted && left <= 0) reacted = MajiniDieNow(enemy);
    } else if (IsExecutioner(enemy)) {
        reacted = true;  // its reaction came at the parry
    } else if (IsJill2(enemy)) {
        reacted = JillParried(enemy);
    } else if (IsWesker(enemy)) {
        const int reaction = g_weskerReaction.load(std::memory_order_relaxed);
        if (reaction >= 0 && reaction < 2) {  // 2, the native counter, came at the parry
            Request(enemy, kWeskerReactions[reaction], 1);
            reacted = true;
        }
    }
    LOG("parry RE6: the punch lands on %08X, health %d -> %d, %s", static_cast<unsigned>(enemy), before, left,
        reacted ? "state" : "no reaction");
}

// A Majini forced into the state that opens one melee prompt (the RE6-style quick shot, 2026-10-21). Damage actions
// with their own prompt (vfunc 10): DmgHead 0x6D (head flash, action +0x24 1), DmgBody 0x6C (Leg Front 0x31 when its
// setup VA 0xC209C0 picks the strong motion 0x520 and +0x28 = 1: no light hit +0x1BB4 & 0x200, +0x1B60 & 0x400 - the
// same rule as DmgHead), DmgArm 0x6E (Arm Front 0x30 / Arm Back 0x33 / Finisher Back by +0x24 1..4), the stumbles
// (0x7E head flash + Arm Back, 0x7D Arm Front + head flash; with +0x4C the finishers 0x35 / 0x34). Strong = the light
// hit bits cleared and +0x1B5C |= 0x400 (copied to +0x1B60 by the setup), as the parry's head stagger.
// 0 head flash (DmgHead), 1 recoil with head flash (0x7E), 2 arm (DmgArm: Arm Front / Back by the player's side), 3 leg
// (DmgLeg, below), 4 finisher (0x7E with the combo flag +0x1B5C 0x100000 -> +0x4C 1), 5 front stumble (0x7D, below).
// Player 1 shot it: his prompt exclusion bit (1 << 1 of +0x1BB4) is cleared in every state, else only Sheva got the
// prompt (the first quick shot tries, 2026-10-21: "forced", no prompt for Chris).
// Leg (user, 2026-10-21: DmgBody only offered Leg Front, from behind too): cEm10ActDmgLeg 0x70, whose prompt creator
// (vfunc 10 VA 0xC0C4A0) offers Leg Front (handler VA 0xB5DA80) or Leg Back (VA 0xB5DDE0) by its +0x24, set by its setup
// (vfunc 6 VA 0xC21D40) from the last hit's part kind +0x1DB0 (legs 7..0xA) and the strong bit +0x1B60 & 0x400: the
// part kind of a leg hit (7) is written first. 5 = the arm stumble, as after a first Arm Front in a co-op combo: the
// front stumble DmgYoro_F 0x7D (vtable RVA 0xC3B484) offers Arm Front in front and a head flash behind, following the
// player's side live (user); the back one 0x7E a head flash in front and Arm Back behind. Picking the stumble by player
// 1's side when forced did not work (user). The front stumble's setup (VA 0xC23760) always turns the enemy round
// (+0x1B74 += pi) and plays a step forward (0x56C), the back stumble turns it towards the attacker and recoils (0x56B,
// the parry's): the front stumble shot in the face turned its back and walked, the back one shot in the back turned
// round (user). So the stumble follows the enemy's side to player 1 when forced (a hit in the back = the front
// stumble, else the back one) and their prompts are changed for the enemies put there (3 s), sActionButton::Request
// (VA 0x764480, thiscall 19 args, ret 0x4C: 7 type, 14 handler, 15 / 19 enemy): the back stumble's head flash (back at
// VA 0xC0F128, type 0x49 handler VA 0xB5DBA0, its front prompt) becomes Arm Front (type 0x0C, handler VA 0xB5D9F0, the
// front stumble's), its Arm Back (back at VA 0xC0F2F9, type 0x4A handler VA 0xB5DCC0) the head flash in state 6; the
// front stumble's head flash (back at VA 0xC0EB1C, its back prompt) becomes Arm Back in states 5 and 1, its Arm Front
// (back at VA 0xC0E8B0, type 0x0C) the head flash in state 1. 5 = Arm Front / Arm Back, 6 = Arm Front / head flash (the
// front stumble's own prompts, user: kept as a choice), 1 = head flash / Arm Back (the back stumble's, the parry's
// recoil: shot in the back it is the front stumble too, user), 7 = Leg Front / Leg Back, 8 = Leg Front on both sides
// (user). Leg prompts from DmgLeg's creator (VA 0xC0C4A0): Leg Front type 0x0B handler VA 0xB5DA80, Leg Back type 0x4F
// handler VA 0xB5DDE0. +0x1B5C 0x100000 cleared (else the finishers).
constexpr uint32_t kFrontStumbleAction = 0x7D;
constexpr uintptr_t kFrontStumbleVtable = 0xC3B484;

std::atomic<uintptr_t> g_armStumbleEnemy{ 0 };
std::atomic<DWORD> g_armStumbleTick{ 0 };
std::atomic<int> g_armStumbleState{ 5 };  // the state whose prompts the stumble shows

struct StumblePrompt {
    uint32_t type;
    uintptr_t handlerRva;
};
constexpr StumblePrompt kHeadFlashPrompt{ 0x49, 0x75DBA0 }, kArmFrontPrompt{ 0x0C, 0x75D9F0 },
    kArmBackPrompt{ 0x4A, 0x75DCC0 }, kLegFrontPrompt{ 0x0B, 0x75DA80 }, kLegBackPrompt{ 0x4F, 0x75DDE0 },
    kFinisherFrontPrompt{ 0x4C, 0x76CAC0 }, kFinisherBackPrompt{ 0x4B, 0x76CA00 };

// The prompts a stumble state shows in front of the enemy and behind it.
bool StumblePrompts(int state, StumblePrompt& front, StumblePrompt& back) {
    switch (state) {
    case 1: front = kHeadFlashPrompt, back = kArmBackPrompt; return true;
    case 5: front = kArmFrontPrompt, back = kArmBackPrompt; return true;
    case 6: front = kArmFrontPrompt, back = kHeadFlashPrompt; return true;
    case 7: front = kLegFrontPrompt, back = kLegBackPrompt; return true;
    case 8: front = kLegFrontPrompt, back = kLegFrontPrompt; return true;
    // The arm hit (DmgArm, motion 0x514) with a prompt (ArmPromptStates below).
    case 9: front = kHeadFlashPrompt, back = kArmBackPrompt; return true;
    case 10: front = kArmFrontPrompt, back = kArmBackPrompt; return true;
    case 11: front = kLegFrontPrompt, back = kLegBackPrompt; return true;
    case 12: front = kLegFrontPrompt, back = kLegFrontPrompt; return true;
    case 13: front = kFinisherFrontPrompt, back = kFinisherBackPrompt; return true;
    // Kneeling as after a leg shot (DmgLeg 0x70) with a prompt (ForceMajiniState 14..18).
    case 14: front = kHeadFlashPrompt, back = kArmBackPrompt; return true;
    case 15: front = kArmFrontPrompt, back = kArmBackPrompt; return true;
    case 16: front = kLegFrontPrompt, back = kLegBackPrompt; return true;
    case 17: front = kLegFrontPrompt, back = kLegFrontPrompt; return true;
    case 18: front = kFinisherFrontPrompt, back = kFinisherBackPrompt; return true;
    default: return false;
    }
}

using PromptRequestFn = void*(__fastcall*)(void* self, void* edx, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                                           uint32_t a5, uint32_t a6, uint32_t type, uint32_t a8, uint32_t a9, uint32_t a10,
                                           uint32_t a11, uint32_t a12, uint32_t a13, uint32_t handler, uint32_t a15,
                                           uint32_t a16, uint32_t a17, uint32_t a18, uint32_t enemy);
PromptRequestFn g_promptRequest = nullptr;

void* __fastcall HookPromptRequest(void* self, void* edx, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                                   uint32_t a6, uint32_t type, uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11,
                                   uint32_t a12, uint32_t a13, uint32_t handler, uint32_t a15, uint32_t a16, uint32_t a17,
                                   uint32_t a18, uint32_t enemy) {
    const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (enemy && enemy == g_armStumbleEnemy.load() && GetTickCount() - g_armStumbleTick.load() < 3000) {
        const auto at = [from](uintptr_t rva) { return from == reinterpret_cast<uintptr_t>(mem::Rva(rva)); };
        StumblePrompt front{}, back{};
        if (StumblePrompts(g_armStumbleState.load(), front, back)) {
            // Each stumble's prompt in front of the enemy and its prompt behind it (their call sites and own types).
            // DmgArm's creator (VA 0xBFD610): its Arm Front (back at VA 0xBFD761) and Arm Back (back at VA 0xBFD8CE).
            // DmgLeg's creator (VA 0xC0C4A0): its Leg Front (back at VA 0xC0C636; 0xD0 its Reunion variant) and Leg Back (VA 0xC0C79E).
            const bool isFront = (at(0x80F128) && type == 0x49) || (at(0x80E8B0) && type == 0x0C) ||
                                 (at(0x7FD761) && type == 0x0C) || (at(0x80C636) && (type == 0x0B || type == 0xD0));
            const bool isBack = (at(0x80F2F9) && type == 0x4A) || (at(0x80EB1C) && type == 0x49) ||
                                (at(0x7FD8CE) && type == 0x4A) || (at(0x80C79E) && type == 0x4F);
            if (isFront || isBack) {
                const StumblePrompt& wanted = isFront ? front : back;
                type = wanted.type;
                handler = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem::Rva(wanted.handlerRva)));
            }
        }
    }
    return g_promptRequest(self, edx, a1, a2, a3, a4, a5, a6, type, a8, a9, a10, a11, a12, a13, handler, a15, a16, a17, a18,
                           enemy);
}

bool EnsurePromptHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x364480);
    const uint8_t expected[] = { 0x56, 0xE8, 0x7A, 0xFF, 0xFF, 0xFF, 0x8B, 0xF0 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("arm stumble: prompt request unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookPromptRequest), reinterpret_cast<void**>(&g_promptRequest));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("arm stumble: prompt request hook installed at %p", target);
    return true;
}

constexpr uintptr_t kDmgArmVtable = 0xC3B1F8;  // cEm10ActDmgArm (RVA)
std::atomic<uintptr_t> g_armPromptEnemy{ 0 };   // its prompts to open (game thread, ArmPromptTick)
std::atomic<DWORD> g_armPromptTick{ 0 };
std::atomic<bool> g_armPromptOpened{ false };

uint32_t MajiniStateAction(int state) {
    constexpr uint32_t kActionOf[] = { 0x6D, kRecoilAction, 0x6E, 0x70, kRecoilAction, kRecoilAction, kRecoilAction,
                                       kRecoilAction, kRecoilAction, 0x6E, 0x6E, 0x6E, 0x6E, 0x6E, 0x70, 0x70, 0x70, 0x70, 0x70, 0x70 };
    return state >= 0 && state < static_cast<int>(std::size(kActionOf)) ? kActionOf[state] : 0x6D;
}

bool ForceMajiniState(uintptr_t enemy, int state) {
    if (!IsMajini(enemy)) return false;
    int16_t health = 0;
    if (!mem::Read(enemy + 0x1364, health) || health <= 0) return false;
    switch (state) {
    case 9: case 10: case 11: case 12: case 13: {
        // The arm hit with a prompt (user, 2026-10-23: the RE6 parry's arm hit suits the parry better than the recoil,
        // the melee opening kept): DmgArm 0x6E with motion 0x514 (light, no prompt of its own), then its prompt creator
        // (vfunc 10 VA 0xBFD610: Arm Front type 0x0C in front, Arm Back 0x4A behind, for +0x24 1..4) called on the game
        // thread once its action started, its prompts changed by the request hook to the state's (StumblePrompts), and
        // closed with its vfunc 11 (VA 0xBFD910, idempotent) when it leaves the action (ArmPromptTick).
        if (!HasAction(enemy, 0x6E, kDmgArmVtable) || !EnsurePromptHook()) return false;
        g_armStumbleState = state;
        g_armStumbleTick = GetTickCount();
        g_armStumbleEnemy = enemy;
        uint32_t players = 0, flags = 0;
        if (mem::Read(enemy + 0x1BB4, players)) mem::Write(enemy + 0x1BB4, (players & ~0x102u) | 0x200u);
        if (mem::Read(enemy + 0x1B5C, flags)) mem::Write(enemy + 0x1B5C, flags & ~0x100000u);
        ForceNextEnemyMotion(enemy, kCounterArmMotion);
        KeepArmHitStanding(enemy);
        Request(enemy, 0x6E, 1);
        const DWORD now = GetTickCount();
        g_armPromptTick = now ? now : 1;
        g_armPromptOpened = false;
        g_armPromptEnemy = enemy;
        return true;
    }
    case 19: {
        // Tripped as a running Majini shot in the leg (user, 2026-11-09, for RE6's sweep against grabs; recorded the same day: from its
        // running attack AtkDashOneHand 0x31, a leg shot gave DmgLeg 0x70 playing motion 0x542, its fall forward, then RiseFaceDown).
        uint32_t light = 0;
        mem::Write<uint32_t>(enemy + 0x1DB0, 7);
        if (mem::Read(enemy + 0x1BB4, light)) mem::Write(enemy + 0x1BB4, light & ~0x302u);
        ForceNextEnemyMotion(enemy, 0x542);
        Request(enemy, 0x70, 1);
        return true;
    }
    case 14: case 15: case 16: case 17: case 18: {
        // Kneeling as after a leg shot with a prompt (user, 2026-11-09, the RE6 slide: "open to the finisher, on their knees as after a
        // shot in the leg, as RE6's enemies react"): DmgLeg 0x70 as state 3 (a leg hit's part kind 7), its own prompt (Leg Front or Leg
        // Back, opened by its motion) changed by the request hook to the state's.
        if (!EnsurePromptHook()) return false;
        g_armStumbleState = state;
        g_armStumbleTick = GetTickCount();
        g_armStumbleEnemy = enemy;
        uint32_t light = 0, flags = 0;
        mem::Write<uint32_t>(enemy + 0x1DB0, 7);
        if (mem::Read(enemy + 0x1BB4, light)) mem::Write(enemy + 0x1BB4, light & ~0x302u);
        if (mem::Read(enemy + 0x1B5C, flags)) mem::Write(enemy + 0x1B5C, flags | 0x400u);
        Request(enemy, 0x70, 1);
        return true;
    }
    case 4: {
        if (!HasAction(enemy, kRecoilAction, kRecoilVtable)) return false;
        uint32_t players = 0, flags = 0;
        if (mem::Read(enemy + 0x1BB4, players)) mem::Write(enemy + 0x1BB4, players & ~0x302u);
        if (mem::Read(enemy + 0x1B5C, flags)) mem::Write(enemy + 0x1B5C, flags | 0x100000u);
        Request(enemy, kRecoilAction, 1);
        return true;
    }
    case 1:
    case 5:
    case 6:
    case 7:
    case 8: {
        if (!HasAction(enemy, kRecoilAction, kRecoilVtable) || !HasAction(enemy, kFrontStumbleAction, kFrontStumbleVtable) ||
            !EnsurePromptHook())
            return state == 1 && Recoil(enemy);
        bool behind = false;  // player 1 behind the enemy: shot in the back
        float e[3]{}, q[4]{}, pp[3]{};
        const uintptr_t p1 = actors::Player(0);
        if (p1 && mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), e, sizeof(e)) &&
            mem::SafeRead(reinterpret_cast<void*>(enemy + 0x40), q, sizeof(q)) && PlayerBody(p1, pp)) {
            constexpr float kPi = 3.14159265f;
            float side = 2.0f * std::atan2(q[1], q[3]) - std::atan2(pp[0] - e[0], pp[2] - e[2]);
            while (side > kPi) side -= 2 * kPi;
            while (side < -kPi) side += 2 * kPi;
            behind = std::fabs(side) > kPi / 2;
        }
        g_armStumbleState = state;
        g_armStumbleTick = GetTickCount();
        g_armStumbleEnemy = enemy;
        uint32_t players = 0, flags = 0;
        if (mem::Read(enemy + 0x1BB4, players)) mem::Write(enemy + 0x1BB4, players & ~0x302u);
        if (mem::Read(enemy + 0x1B5C, flags)) mem::Write(enemy + 0x1B5C, flags & ~0x100000u);
        Request(enemy, behind ? kFrontStumbleAction : kRecoilAction, 1);
        return true;
    }
    default: {
        const uint32_t action = MajiniStateAction(state);
        uint32_t light = 0, flags = 0;
        if (state == 3) mem::Write<uint32_t>(enemy + 0x1DB0, 7);  // a leg hit's part kind
        if (mem::Read(enemy + 0x1BB4, light)) mem::Write(enemy + 0x1BB4, light & ~0x302u);
        if (mem::Read(enemy + 0x1B5C, flags)) mem::Write(enemy + 0x1B5C, flags | 0x400u);
        Request(enemy, action, 1);
        return true;
    }
    }
}

// Game thread (quick_knife.cpp's pad query hook, every frame): the arm states' prompts opened once the arm hit started,
// closed once it is over (a melee move taken from them included).
void ArmPromptTick() {
    StandingArmTick();
    EffectTryTick();
    const uintptr_t enemy = g_armPromptEnemy.load(std::memory_order_relaxed);
    if (!enemy) return;
    uint32_t action = 0;
    uintptr_t actions = 0, act = 0, vtable = 0, fn = 0;
    const bool live = mem::Read(enemy + 0x10E0, action) && mem::Read(enemy + 0x10DC, actions) && actions &&
                      mem::Read(actions + 4 * 0x6E, act) && act && mem::Read(act, vtable);
    const bool opened = g_armPromptOpened.load();
    const DWORD since = GetTickCount() - g_armPromptTick.load();
    if (!opened) {
        if (live && action == 0x6E && static_cast<int32_t>(since) >= 60) {
            uint32_t variant = 0;
            if (mem::Read(act + 0x24, variant) && (variant < 1 || variant > 4)) mem::Write<uint32_t>(act + 0x24, 1);
            if (mem::Read(vtable + 4 * 10, fn) && fn) {
                __try {
                    reinterpret_cast<void(__fastcall*)(void*, void*)>(fn)(reinterpret_cast<void*>(act), nullptr);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
                LOG("parry state: arm hit prompts opened on %08X (state %d)", static_cast<unsigned>(enemy),
                    g_armStumbleState.load());
            }
            g_armPromptOpened = true;
        } else if (static_cast<int32_t>(since) > 1500) {
            g_armPromptEnemy = 0;  // its arm hit never came
        }
        return;
    }
    if (live && action == 0x6E) return;
    if (live && mem::Read(vtable + 4 * 11, fn) && fn) {
        __try {
            reinterpret_cast<void(__fastcall*)(void*, void*)>(fn)(reinterpret_cast<void*>(act), nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_armPromptEnemy = 0;
    g_armPromptOpened = false;
}

// A Majini whose health was set to 0 without a hit (the RE6-style quick shot's lethal miss) only died once its AI
// picked its next action (the deaths are requested there, caller RVA 0x6753E7 like its other actions): up to 5.7 s
// walking at 0 health (recorded). A short flinch (DmgBody 0x6C with the light hit bit 0x200 of +0x1BB4: its setup VA
// 0xC209C0 then plays the flinch) ends at once and the AI then picks the death. Not for a Majini on the ground: the
// flinch stood it up at once, staggering back (user): lying face up (RiseFaceUp 0x6A, DmgAtFaceUp 0x82) it gets
// cEm10ActDeadFaceUp 0x8D, face down (0x6B, 0x83) DeadFaceDown 0x8E (cEm10Act* DTI classes in creation order, action =
// index - 1: DeadNormal 0x8B / 0x8C seen in game); falling or blown off (0x73, 0x75, 0x76, 0x80) its AI picks the
// death when it lands.
bool MajiniDieNow(uintptr_t enemy) {
    if (!IsMajini(enemy)) return false;
    uint32_t action = 0;
    mem::Read(enemy + 0x10E0, action);
    switch (action) {
    case 0x6A: case 0x82: Request(enemy, 0x8D, 1); return true;
    case 0x6B: case 0x83: Request(enemy, 0x8E, 1); return true;
    case 0x73: case 0x75: case 0x76: case 0x80: return true;
    default: break;
    }
    uint32_t light = 0;
    if (mem::Read(enemy + 0x1BB4, light)) mem::Write(enemy + 0x1BB4, light | 0x200u);
    Request(enemy, 0x6C, 1);
    return true;
}

// The free melee table's flinch (melee_tuning.cpp, user 2026-10-30: "just a flinch, as a knife hit on the head without
// prompt"): DmgHead 0x6D with the knife's light hit bit 0x100 of +0x1BB4 (0x200 cleared), hit from the front (+0x1DAC 1),
// its random +0x1034 set to a flinch: the setup (VA 0xC21030) takes +0x1034 % 12 there, 4..7 = 0x502 and 8..11 = 0x501
// with +0x24 = 0 (no prompt), 1..3 and 0 its staggers with the prompt.
void MarkMajiniFlinch(uintptr_t enemy);  // knife.cpp: its draw set again at the action's setup

bool MajiniFlinch(uintptr_t enemy) {
    if (!IsMajini(enemy)) return false;
    int16_t health = 0;
    uint32_t light = 0, random = 0;
    if (!mem::Read(enemy + 0x1364, health) || health <= 0) return false;
    if (mem::Read(enemy + 0x1BB4, light)) mem::Write(enemy + 0x1BB4, (light & ~0x202u) | 0x100u);
    mem::Write<uint32_t>(enemy + 0x1DAC, 1);
    if (mem::Read(enemy + 0x1034, random)) mem::Write(enemy + 0x1034, random - random % 12 + ((random & 1) ? 4u : 8u));
    MarkMajiniFlinch(enemy);
    Request(enemy, 0x6D, 1);
    return true;
}

// A shotgun blast's blow-off without a hit (the RE6-style quick shot's missed shotgun shot, user 2026-10-21: no flying
// enemies). cEm10ActDmgFlyoff 0x73 (setup VA 0xC22AA0) reads what the last hit left: +0x1DAC (1 = from the front,
// motions 0x528 / 0x52A / 0x52C; otherwise the setup turns the enemy round, +0x1B74, and plays 0x530 / 0x532), +0x1DB0
// the part kind (2 head, 1 / 6 body, 7..10 legs: motions 0x52E..0x532), +0x1DC0..+0x1DC8 the shot's unit direction,
// shooter to enemy (recorded on four close shotgun kills: front, kinds 1 / 2 / 6, y about -0.06). Written as a front or
// back body hit from player 1, then the action is requested; a Majini at 0 health dies when it lands (as recorded).
bool MajiniBlowOff(uintptr_t enemy, uintptr_t player) {
    float p[3]{};
    return player && mem::SafeRead(reinterpret_cast<void*>(player + 0x30), p, sizeof(p)) && MajiniBlowOffFrom(enemy, p, 1);
}

// The same from a point (the RE6 parry's lethal punch: Chris's body) with the hit's part kind.
bool MajiniBlowOffFrom(uintptr_t enemy, const float* p, uint32_t part) {
    if (!IsMajini(enemy)) return false;
    constexpr float kPi = 3.14159265f;
    float e[3]{}, q[4]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), e, sizeof(e)) ||
        !mem::SafeRead(reinterpret_cast<void*>(enemy + 0x40), q, sizeof(q)))
        return false;
    const float dx = e[0] - p[0], dz = e[2] - p[2], len = std::sqrt(dx * dx + dz * dz);
    if (len < 1.0f) return false;
    const float dir[3] = { dx / len * 0.998f, -0.06f, dz / len * 0.998f };
    float facing = 2.0f * std::atan2(q[1], q[3]) - std::atan2(-dx, -dz);  // its yaw against the way to the player
    while (facing > kPi) facing -= 2 * kPi;
    while (facing < -kPi) facing += 2 * kPi;
    mem::Write<uint32_t>(enemy + 0x1DAC, std::fabs(facing) < kPi / 2 ? 1u : 0u);
    mem::Write<uint32_t>(enemy + 0x1DB0, part);
    mem::SafeWrite(reinterpret_cast<void*>(enemy + 0x1DC0), dir, sizeof(dir));
    Request(enemy, 0x73, 1);
    return true;
}

// perfect_dodge.cpp's automatic parries. Their hooks, as the parries' options install them.
bool SetAutoParry(bool knife, bool re6) {
    if ((knife || re6) && (!EnsurePadQueryHooks() || !EnsurePlayerDamageHook() || !EnsurePlayerHitReactionHook() ||
                           !EnsureEnemyOnHitHook() || !EnsureEnemyActionHook()))
        return false;
    if ((knife || re6) && !EnsureBoltHooks()) LOG("parry: the bolts are not followed in flight");
    if (knife || re6) {
        EnsureCommonEffectsHook();
        if (actors::InGame()) PrepareWeskerMoves();
    }
    g_autoKnifeOn = knife;
    g_autoRe6On = re6;
    return true;
}

// The knife parry's window opened by the automatic parry with `kind` as its riposte (0 knife slash, 1 Wesker's guard,
// 2 Jill's throw): not with a broken knife or no knife for the slash (user: "si le couteau est cassé la parade
// automatique ne se déclenchera pas"), nor while the stamina refuses it, as a press.
bool AutoParryPress(int kind) {
    if (!g_autoKnifeOn.load(std::memory_order_relaxed) || RiposteKindNow() < 0) return false;  // "Aucune": none at all
    if (kind == kRiposteKnife && KnifeUnusable(0)) return false;
    if (!(StaminaAllows() || (kind == kRiposteGuard && !StaminaStrict()))) return false;
    g_autoKind = std::clamp(kind, 0, 3);
    g_pressAuto = true;
    g_pressTick = Stamp();
    return true;
}

bool AutoRe6Press() {
    if (!g_autoRe6On.load(std::memory_order_relaxed) || Re6KindNow() < 0 || !StaminaAllows()) return false;
    g_re6PressTick = Stamp();
    return true;
}

// A blow parried within `ms` (the automatic parries wait for its riposte).
bool ParriedWithin(DWORD ms) { return g_parriedTick && Since(g_parriedTick) < ms; }

namespace {
// A crossbow bolt parried by a dodge (the guard, Jill's throw, the RE6 counter: Wesker's left dodge) still broke on Chris,
// its contact made (user, 2026-10-30: "pas crédible, qu'il le traverse sans casser"): such a parry is played before the
// contact - the bolt followed in flight (rocket_catch.cpp), within 3.5 m of his chest and closing in, with a parry's
// window open or the automatic parry on - and player 1 is hidden from the blows for 0.9 s (perfect_dodge.cpp, as by
// "Invincible"), so it flies through him. With the knife slash it is parried at its hit as before.
constexpr float kBoltReach = 350.0f;
uintptr_t g_boltDodged = 0;  // render thread
DWORD g_boltDodgedTick = 0;

void TickBoltDodge() {
    uintptr_t bolt = 0;
    float distance = 0;
    if (!IncomingBolt(bolt, distance) || distance > kBoltReach) return;
    if (bolt == g_boltDodged && Since(g_boltDodgedTick) < 2000) return;
    const uintptr_t p1 = actors::Player(0);
    if (!p1 || (!g_parryDying.load(std::memory_order_relaxed) && Player1Dying()) ||
        (!g_parryHeld.load(std::memory_order_relaxed) && Held(p1)))
        return;
    const bool knifeWindow = KnifeParryOn() && g_pressTick &&
                             Since(g_pressTick) <= static_cast<DWORD>(g_window.load(std::memory_order_relaxed) * 1000.0f);
    const bool re6Window = Re6ParryOn() && g_re6PressTick &&
                           Since(g_re6PressTick) <= static_cast<DWORD>(g_re6Window.load(std::memory_order_relaxed) * 1000.0f);
    const bool autoKnife = g_autoKnifeOn.load(std::memory_order_relaxed), autoRe6 = g_autoRe6On.load(std::memory_order_relaxed);
    bool dodge = false;
    if (knifeWindow) dodge = (g_pressAuto ? g_autoKind.load(std::memory_order_relaxed) : RiposteKind()) != kRiposteKnife;
    else if (re6Window) dodge = true;
    else if (autoKnife) dodge = AutoParryKind() != kRiposteKnife && StaminaAllows();
    else if (autoRe6) dodge = StaminaAllows();
    if (!dodge || !StartDodgeMove("melee.free_wesker_dodge1")) return;
    HideFromProjectiles(900);
    g_boltDodged = bolt;
    g_boltDodgedTick = Stamp();
    g_parriedTick = Stamp();
    g_parriedAttacker = bolt;
    g_pressTick = 0;
    g_re6PressTick = 0;
    PlayParrySound(true, -1);
    LOG("parry: bolt %08X dodged %.0f cm away (Wesker's left dodge, hidden from it)", static_cast<unsigned>(bolt), distance);
}
}  // namespace

namespace {
// An enemy's attack in the sphere around player 1 (perfect_dodge.cpp, "Esquive parfaite : taille de la sphère") while a
// parry's window is open: parried there, before it touches him (user, 2026-10-30: invincible - the option or the
// automatic dodges hide his hurt shapes - no blow reached him any more, "les parades ne marchent plus"); a catch as a
// grab. Its hit, if it still comes, is the same blow (refused by ParryAttack). Render thread (the sphere's cache).
void TickParrySphere() {
    if (!KnifeParryOn() && !Re6ParryOn()) return;
    const bool knife = g_pressTick && Since(g_pressTick) <= static_cast<DWORD>(g_window.load(std::memory_order_relaxed) * 1000.0f);
    const bool re6 =
        g_re6PressTick && Since(g_re6PressTick) <= static_cast<DWORD>(g_re6Window.load(std::memory_order_relaxed) * 1000.0f);
    const uintptr_t p1 = actors::Player(0);
    if ((!knife && !re6) || !p1) return;
    bool grab = false;
    const uintptr_t attacker = EnemyAttackInSphere(p1, grab);
    const DWORD parried = g_parriedTick;
    if (attacker && ParryAttack(p1, attacker, grab ? 0x40000000u : 0x00400000u, false) && g_parriedTick != parried)
        LOG("parry: %s %08X's %s seen in the sphere", ClassName(attacker), static_cast<unsigned>(attacker), grab ? "grab" : "blow");
}
}  // namespace

// The Parade tab (user, 2026-11-01): the per-character table above its options.
void DrawParryPanel() {
    ImGui::SeparatorText(i18n::Tr("Par personnage"));
    ImGui::TextDisabled(i18n::Tr("La riposte de la parade et le contre de la parade RE6 selon le personnage joué (« Aucune » : "
                        "il ne pare pas ainsi)."));
    DrawCharacterChoices("parry_kinds", { { "Parade", "combat.parry_kind_" }, { "Parade RE6", "combat.parry_re6_kind_" },
                                          { "Baïonnette (2 mains)", "combat.parry_re6_bayonet_" },
                                          { "Aléatoire", "combat.parry_re6_random_" } });
    ImGui::SeparatorText(i18n::Tr("Parades"));
    DrawOptions("Parade");
}

void RegisterParry() {
    // The riposte's time also runs for the automatic parries alone.
    OnFrame([] {
        if (!actors::InGame()) return;
        const bool knife = g_autoKnifeOn.load(std::memory_order_relaxed) && !g_parryOn.load(std::memory_order_relaxed);
        const bool re6 = g_autoRe6On.load(std::memory_order_relaxed) && !g_re6On.load(std::memory_order_relaxed);
        if (knife) TickParryRiposte();
        else if (re6 && actors::Player(0)) mem::Read(actors::Player(0) + 0x1364, g_lastHealth);
        if ((knife || re6) && !g_parryOn.load() && !g_re6On.load()) TickSpiderFall();
        TickParrySphere();
        if (KnifeParryOn() || Re6ParryOn()) TickBoltDodge();
    });
    Feature& parry = Add({ .id = "combat.knife_parry", .name = "Parade au couteau (façon RE4 Remake)",
                           .help = "Appuie sur viser avec le couteau, le couteau rapide ou l'attaque au couteau juste "
                                   "avant qu'un ennemi te frappe : le coup est paré (aucun dégât), Chris riposte d'un coup "
                                   "de couteau et un Majini recule (prompt head flash, comme après l'aide du partenaire). Le curseur règle la "
                                   "fenêtre après l'appui. Les projectiles (haches, carreaux) et les parasites sont "
                                   "aussi parés, sans faire vaciller le lanceur. Les balles ennemies ne se parent pas. "
                                   "Une attaque qui entre dans la sphère autour de Chris (sa taille : Dodge) est parée "
                                   "avant de le toucher, même s'il est invincible. La riposte de chaque personnage (ou aucune parade) se "
                                   "règle dans le tableau par personnage ci-dessus.",
                           .category = "Parade", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = 0.3f, .valueMin = 0.1f, .valueMax = 0.8f, .valueFormat = "%.2f s" });
    Feature* const pp = &parry;
    parry.set = [pp](bool on) {
        if (on && (!EnsurePadQueryHooks() || !EnsurePlayerDamageHook() || !EnsurePlayerHitReactionHook() ||
                   !EnsureEnemyOnHitHook() || !EnsureEnemyActionHook()))
            return false;
        if (on) EnsureCommonEffectsHook();  // no blood on a parried blow (optional)
        UsePadMaskForParry(on || g_re6On.load());
        g_window = pp->value;
        g_parryOn = on;
        return true;
    };
    OnRestore([] {  // unloaded during a slash: its time scale back
        if (g_slashSped) mem::Write(g_slashSped + 0xFD4, g_slashSavedScale);
        g_slashSped = 0;
    });
    parry.tick = [pp] {
        g_window.store(pp->value, std::memory_order_relaxed);
        if (actors::InGame()) TickParryInput();
        if (actors::InGame()) TickSpiderFall();
    };
    Add({ .id = "combat.parry_no_quick_knife", .name = "Le couteau rapide ne déclenche pas la parade",
          .help = "Coché : la parade au couteau ne se déclenche plus avec le couteau rapide (sa touche, ou la touche "
                  "d'attaque avec « Couteau rapide sans viser ») : seulement en visant au couteau ou avec l'attaque au "
                  "couteau en visant. La touche d'attaque reste alors à la parade façon RE6.",
          .category = "Parade", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_parryNoQuickKnife = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    // Per character (user, 2026-11-01): the parry's riposte and the RE6 parry's counter, "Aucune" first (that character
    // cannot parry so at all, the automatic parries neither). Defaults: the riposte the single option had (settings.ini,
    // combat.parry_riposte), Chris's counter.
    const std::wstring ini = logx::DataDir() + L"\\settings.ini";
    const int oldRiposte = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"values", L"combat.parry_riposte", 0, ini.c_str())), 0, 2);
    for (int c = 0; c < kCharacterCount; ++c) {
        g_parryKindOf[c] = -1;  // as the choices' index 0 until they are applied
        g_re6KindOf[c] = -1;
        static const char* const kKeys[kCharacterCount] = { "chris", "sheva", "jill", "wesker", "josh", "excella", "barry", "rebecca" };
        const std::string who = CharacterName(c);
        Add({ .id = std::string("combat.parry_kind_") + kKeys[c], .name = "Parade de " + who,
              .help = "Ce que fait " + who + " après une parade réussie (touches du couteau, « Parade au couteau » cochée). "
                      "Coup de couteau : la parade façon RE4 Remake. Garde de Wesker : la même parade, avec la garde de "
                      "Wesker au lieu du coup de couteau (RE7). Projection de Chris : il projette l'ennemi qui l'a attaqué "
                      "comme Chris projette Jill (Majini, bourreau, Jill), qui prend les dégâts de la projection ; sur un "
                      "autre ennemi, coup de couteau. Projection de Josh : le Finisher Front de Josh sur l'attaquant (la "
                      "ligne « Josh Contre » de la mêlée libre, ses propres réglages). Aucune : " + who +
                      " ne pare pas (la parade automatique non plus).",
              .category = "Parade par personnage", .kind = Kind::Choice,
              .available = [] { return game::IsSupported(); },
              .choose = [c](int index) {
                  const int kind = index - 1;
                  if ((kind == kRiposteGuard || kind == kRiposteThrow) && actors::InGame() && !PrepareWeskerMoves()) return false;
                  if (kind == kRiposteJosh && actors::InGame()) PrepareJoshCounter();
                  g_parryKindOf[c] = kind;
                  return true;
              },
              .value = static_cast<float>(oldRiposte + 1), .valueMin = 0, .valueMax = 4, .valueFormat = "%.0f",
              .choices = { "Aucune", "Coup de couteau (RE4 Remake)", "Garde de Wesker (RE7)", "Projection de Chris",
                           "Projection de Josh" } });
        Add({ .id = std::string("combat.parry_re6_kind_") + kKeys[c], .name = "Parade RE6 de " + who,
              .help = "Le contre que fait " + who + " avec la parade façon RE6 (touche d'attaque, « Parade façon RE6 » "
                      "cochée) : le contre de Chris ou celui de Sheva sur Wesker dans son combat (leurs lignes « Contre RE6 » "
                      "de la mêlée libre règlent ses dégâts, son critique...), ou une projection sur l'attaquant : celle de "
                      "Chris (comme il projette Jill) ou celle de Josh (« Josh Contre ») ; un ennemi qu'elle ne peut pas "
                      "prendre reçoit le contre de Chris. Aucune : " + who +
                      " n'a pas de parade RE6 (le contre automatique non plus).",
              .category = "Parade par personnage", .kind = Kind::Choice,
              .available = [] { return game::IsSupported(); },
              .choose = [c](int index) {
                  if (index > 0 && actors::InGame() && !PrepareWeskerMoves()) return false;
                  if (index - 1 == kRe6JoshCounter && actors::InGame()) PrepareJoshCounter();
                  g_re6KindOf[c] = index - 1;
                  return true;
              },
              .value = 1, .valueMin = 0, .valueMax = 9, .valueFormat = "%.0f",
              .choices = { "Aucune", "Contre de Chris", "Contre de Sheva", "Projection de Chris", "Projection de Josh",
                           "Contre de Piers (RE6)", "Contre de Chris (RE6)", "Contre de Jake (RE6)", "Contre de Leon (RE6)",
                           "Contre d'Ada (RE6)" } });
        g_re6BayonetOf[c] = false;
        Add({ .id = std::string("combat.parry_re6_bayonet_") + kKeys[c], .name = "Contre à la baïonnette de " + who + " (arme à deux mains)",
              .help = "Coché : avec une arme à deux mains en main, la parade RE6 de " + who + " fait le contre à la baïonnette de RE6, quel "
                      "que soit le contre de la liste (ou du hasard) ; avec une autre arme, celui de la liste. Sans effet quand sa "
                      "parade RE6 est « Aucune ».",
              .category = "Parade par personnage", .kind = Kind::Toggle,
              .set = [c](bool on) {
                  if (on && actors::InGame() && !PrepareWeskerMoves()) return false;
                  g_re6BayonetOf[c] = on;
                  return true;
              },
              .available = [] { return game::IsSupported(); } });
        g_re6RandomOf[c] = false;
        Add({ .id = std::string("combat.parry_re6_random_") + kKeys[c], .name = "Parade RE6 aléatoire de " + who,
              .help = "Coché : la riposte choisie pour la parade RE6 de " + who + " sort une fois sur deux ; les autres "
                      "fois, une des autres au hasard (contres de Chris et de Sheva, projections de Chris et de Josh, contres "
                      "de RE6), jamais « Aucune » ; celui à la baïonnette a sa propre case. Sans effet quand sa parade RE6 est "
                      "« Aucune ».",
              .category = "Parade par personnage", .kind = Kind::Toggle,
              .set = [c](bool on) {
                  if (on && actors::InGame()) PrepareJoshCounter();
                  g_re6RandomOf[c] = on;
                  return true;
              },
              .available = [] { return game::IsSupported(); } });
    }
    Feature& re6 = Add({ .id = "combat.parry_re6", .name = "Parade façon RE6 (touche attaque)",
                         .help = "Appuie sur la touche d'attaque (sans viser au couteau) juste avant qu'un ennemi te "
                                 "frappe : le coup est paré et Chris fait son contre sur Wesker (ou celui de Sheva, ou aucune "
                                 "parade : tableau par personnage ci-dessus). L'ennemi prend des "
                                 "dégâts comparables à un head flash (500) et l'état choisi dans « État du Majini paré (parade RE6) ». "
                                 "Elle marche en même temps que la parade au couteau (touche viser au couteau). Le "
                                 "curseur règle la fenêtre après l'appui ; la vitesse du contre est celle de la garde de "
                                 "Wesker. Une attaque qui entre dans la sphère autour de Chris (sa taille : Dodge) est "
                                 "parée avant de le toucher, même s'il est invincible.",
                         .category = "Parade", .kind = Kind::Toggle,
                         .available = [] { return game::IsSupported(); },
                         .value = 0.3f, .valueMin = 0.1f, .valueMax = 0.8f, .valueFormat = "%.2f s" });
    {
        Feature* const pr = &re6;
        re6.set = [pr](bool on) {
            if (on && (!EnsurePadQueryHooks() || !EnsurePlayerDamageHook() || !EnsurePlayerHitReactionHook() ||
                       !EnsureEnemyOnHitHook() || !EnsureEnemyActionHook()))
                return false;
            if (on) {
                EnsureCommonEffectsHook();
                if (actors::InGame()) PrepareWeskerMoves();
            }
            UsePadMaskForParry(on || g_parryOn.load());
            g_re6Window = pr->value;
            g_re6On = on;
            return true;
        };
        re6.tick = [pr] {
            g_re6Window.store(pr->value, std::memory_order_relaxed);
            if (actors::InGame()) TickRe6Input();
            if (actors::InGame()) TickSpiderFall();
        };
    }
    Add({ .id = "combat.parry_state_re4", .name = "État du Majini paré (parade RE4, couteau)",
          .help = "Le prompt de corps à corps ouvert sur le Majini dont tu as paré le coup avec la riposte au couteau "
                  "(pas avec la garde de Wesker ni la projection), comme pour le tir rapide façon RE6.",
          .category = "Parade", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_parryStateRe4 = index;
              return true;
          },
          .value = 1, .valueMin = 0, .valueMax = 13, .valueFormat = "%.0f",
          .choices = { "Head flash", "Recul + head flash (par défaut)", "Arm", "Leg", "Finisher",
                       "Recul + Arm (Arm Front de face, Arm Back de dos)",
                       "Recul + Arm Front (natif : head flash de dos)",
                       "Recul + Leg (Leg Front de face, Leg Back de dos)",
                       "Recul + Leg Front (de face comme de dos)",
                       "Bras + head flash (Arm Back de dos)", "Bras + Arm (Arm Front de face, Arm Back de dos)",
                       "Bras + Leg (Leg Front de face, Leg Back de dos)", "Bras + Leg Front (de face comme de dos)",
                       "Bras + Finisher" } });
    Add({ .id = "combat.parry_state_re7", .name = "État du Majini paré (parade RE7, garde de Wesker)",
          .help = "Le prompt de corps à corps ouvert sur le Majini dont tu as paré le coup avec la riposte « Garde de "
                  "Wesker », comme pour le tir rapide façon RE6.",
          .category = "Parade", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_parryStateRe7 = index;
              return true;
          },
          .value = 9, .valueMin = 0, .valueMax = 13, .valueFormat = "%.0f",
          .choices = { "Head flash", "Recul + head flash", "Arm", "Leg", "Finisher",
                       "Recul + Arm (Arm Front de face, Arm Back de dos)",
                       "Recul + Arm Front (natif : head flash de dos)",
                       "Recul + Leg (Leg Front de face, Leg Back de dos)",
                       "Recul + Leg Front (de face comme de dos)",
                       "Bras + head flash (Arm Back de dos, par défaut)",
                       "Bras + Arm (Arm Front de face, Arm Back de dos)",
                       "Bras + Leg (Leg Front de face, Leg Back de dos)", "Bras + Leg Front (de face comme de dos)",
                       "Bras + Finisher" } });
    Add({ .id = "combat.parry_re6_sweep", .name = "Parade RE6 : balayage contre les saisies",
          .help = "Une saisie parée avec la parade façon RE6 : Chris la contre avec le balayage de RE6 au lieu de sa riposte, "
                  "et le Majini tombe comme quand on lui tire dans la jambe pendant qu'il court.",
          .category = "Parade", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_re6Sweep = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "combat.parry_state", .name = "État du Majini paré (parade RE6)",
          .help = "Le prompt de corps à corps ouvert sur le Majini touché par le coup de poing du contre de la parade "
                  "façon RE6, comme pour le tir rapide façon RE6.",
          .category = "Parade", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_parryState = index;
              return true;
          },
          .value = 1, .valueMin = 0, .valueMax = 13, .valueFormat = "%.0f",
          .choices = { "Head flash", "Recul + head flash (par défaut)", "Arm", "Leg", "Finisher",
                       "Recul + Arm (Arm Front de face, Arm Back de dos)",
                       "Recul + Arm Front (natif : head flash de dos)",
                       "Recul + Leg (Leg Front de face, Leg Back de dos)",
                       "Recul + Leg Front (de face comme de dos)",
                       "Bras + head flash (Arm Back de dos)", "Bras + Arm (Arm Front de face, Arm Back de dos)",
                       "Bras + Leg (Leg Front de face, Leg Back de dos)", "Bras + Leg Front (de face comme de dos)",
                       "Bras + Finisher" } });
    Add({ .id = "combat.parry_executioner", .name = "Réaction du bourreau à la parade",
          .help = "Ce que fait le bourreau quand tu pares son coup ou sa saisie avec le couteau ou la garde de Wesker "
                  "(il ne perd jamais de vie). À genoux : il ouvre un corps à corps. Recul : il recule d'un pas. Coup "
                  "reçu : sa réaction aux coups, avec du sang. Aucune : son attaque est seulement coupée.",
          .category = "Parade", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_executionerReaction = index;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 3, .valueFormat = "%.0f",
          .choices = { "À genoux (ouvre un corps à corps)", "Recul d'un pas", "Coup reçu (avec du sang)", "Aucune" } });
    Add({ .id = "combat.parry_wesker", .name = "Réaction de Wesker à la parade RE6",
          .help = "Ce que fait Wesker quand tu pares son attaque avec la parade façon RE6. Coup reçu : sa réaction aux "
                  "coups, qui ouvre un corps à corps. Sonné : il est complètement sonné et ouvre son combo. Contre "
                  "natif : la réaction du contre du jeu, il tombe puis se relève. Aucune : son attaque est seulement "
                  "coupée.",
          .category = "Parade", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_weskerReaction = index;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 3, .valueFormat = "%.0f",
          .choices = { "Coup reçu (ouvre un corps à corps, par défaut)", "Sonné (ouvre le combo)",
                       "Contre natif (il tombe)", "Aucune" } });
    Add({ .id = "combat.parry_wesker_knife", .name = "Réaction de Wesker à la parade (couteau / garde)",
          .help = "Ce que fait Wesker quand tu pares son attaque avec la parade au couteau (RE4) ou la garde de Wesker "
                  "(RE7), au moment de la parade. Coup reçu : sa réaction aux coups, qui ouvre un corps à corps. Sonné : "
                  "il est complètement sonné et ouvre son combo. Contre natif : le contre du jeu à la place de la riposte, "
                  "il tombe puis se relève. Aucune : son attaque est seulement coupée. Son balayage paré donne toujours "
                  "une roulade arrière, sans réaction.",
          .category = "Parade", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_weskerKnifeReaction = index;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 3, .valueFormat = "%.0f",
          .choices = { "Coup reçu (ouvre un corps à corps, par défaut)", "Sonné (ouvre le combo)",
                       "Contre natif (il tombe)", "Aucune" } });
    Add({ .id = "combat.parry_jill_re6", .name = "Réaction de Jill à la parade RE6",
          .help = "Ce que fait Jill (deuxième partie du combat) quand tu pares son attaque ou sa saisie avec la parade façon "
                  "RE6, au coup de poing de Chris. Sonnée : comme une balle sur son device, ce qui ouvre sa projection. "
                  "La parade au couteau et la garde de Wesker, elles, la projettent toujours (elle reste au sol si son "
                  "device est sous le seuil).",
          .category = "Parade", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_jillRe6Reaction = index;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 5, .valueFormat = "%.0f",
          .choices = { "Sonnée (ouvre la projection, par défaut)", "Sonnée courte", "Coup reçu", "Repoussée", "Au sol",
                       "Aucune" } });
    Add({ .id = "combat.parry_chainsaw", .name = "Réaction du Majini tronçonneuse à la parade",
          .help = "Ce que fait le Majini à la tronçonneuse quand tu pares son coup, son coup mortel compris. Seule la "
                  "parade au couteau le pare (pas la parade façon RE6). Head stagger : il vacille, la tête touchée. "
                  "Vacille : son coup reçu, puis sa réaction aux tirs dans les jambes. Corps à corps : il ouvre un "
                  "prompt. Coup reçu : sa "
                  "réaction aux coups, un recul sans prompt. Aucune : son attaque est seulement coupée.",
          .category = "Parade", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_chainsawReaction = index;
              return true;
          },
          .value = 3, .valueMin = 0, .valueMax = 4, .valueFormat = "%.0f",
          .choices = { "Head stagger", "Vacille (jambes)", "Corps à corps (ouvre un prompt)",
                       "Coup reçu (par défaut)", "Aucune" } });
    Add({ .id = "combat.parry_reaper", .name = "Réaction du Reaper à la parade",
          .help = "Ce que fait le Reaper quand tu pares son coup ou son exécution. Seule la parade au couteau le pare (pas "
                  "la parade façon RE6). Chaque choix est une de ses propres actions (son nom dans le jeu entre "
                  "parenthèses). Aucune : son attaque est seulement coupée.",
          .category = "Parade", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_reaperReaction = index;
              return true;
          },
          .value = 7, .valueMin = 0, .valueMax = 9, .valueFormat = "%.0f",
          .choices = { "Dégâts 1 (Damage 0x18 : à genoux, point faible ouvert)", "Dégâts 2 (Damage 0x19)",
                       "Dégâts 3 (Damage 0x1A)", "Projeté (Blown 0x21)", "Explose, mortel (Disrupted 0x23)",
                       "Interrompu 1 (Abort 0x1E)", "Interrompu 2 (Abort 0x1F)", "Interrompu 3 (Abort 0x20, par défaut)",
                       "Chute (Fall 0x03)", "Aucune" } });
    Add({ .id = "combat.parry_sound_choice", .name = "Son des parades",
          .help = "Le son joué à chaque parade. Couteau : la parade au couteau (riposte au couteau) joue celle de "
                  "Resident Evil 4 Remake, et sa parade bloquée quand l'adversaire n'est pas déstabilisé (sa réaction "
                  "« Aucune », un projectile arrêté) ; la parade RE6, la projection et la garde de Wesker gardent le son "
                  "des contre-attaques de Resident Evil 6. RE6 : ce son-là pour toutes les parades. Les sons viennent de "
                  "tes jeux (fichiers sounds du dossier du trainer).",
          .category = "Parade", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_parrySoundKind = std::clamp(index, 0, 2);
              std::string error;
              if (LoadParrySounds(error)) return true;
              LOG("parry sound: %s", error.c_str());
              Toast(i18n::Tr("Son des parades : ") + error, false);
              return false;
          },
          .value = kParrySoundKnife, .valueMin = 0, .valueMax = 2, .valueFormat = "%.0f",
          .choices = { "Aucun", "Couteau (RE4 Remake, par défaut)", "Contre-attaque (RE6)" } });
    Feature& volume = Add({ .id = "combat.parry_sound_volume", .name = "Son des parades : volume",
                            .help = "Le volume des sons de parade (100 % = celui du fichier ; au-delà, un limiteur évite la saturation).",
                            .category = "Parade", .kind = Kind::Toggle,
                            .set = [](bool) { return true; },
                            .available = [] { return game::IsSupported(); },
                            .value = 100.0f, .valueMin = 0.0f, .valueMax = 800.0f, .valueFormat = "%.0f %%",
                            .defaultOn = true });
    Feature* const vp = &volume;
    volume.tick = [vp] {  // the volume changed: the samples again
        if (vp->value / 100.0f == g_parryVolume.load(std::memory_order_relaxed)) return;
        g_parryVolume = vp->value / 100.0f;
        std::string error;
        if (!LoadParrySounds(error)) LOG("parry sound: %s", error.c_str());
    };
    OnRestore(CloseParrySounds);
    Add({ .id = "combat.parry_ring", .name = "Effet visuel de contre-attaque sur les parades",
          .help = "À chaque parade, un anneau part du bas de l'écran (là où s'affichent les prompts), comme l'effet des "
                  "contre-attaques de Resident Evil 6. Dessiné par le trainer.",
          .category = "Parade", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_parryRingOn = on;
              g_parryRingTick = 0;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Feature& grunt = Add({ .id = "combat.parry_guard_grunt", .name = "Garde de Wesker : grognement du joueur",
                           .help = "La garde de Wesker (parade RE7) joue aussi un grognement du joueur, avec sa propre "
                                   "voix (Chris celle de Chris, Sheva celle de Sheva). Le curseur est le numéro du son dans "
                                   "la banque du personnage (193 ; « Essai : son du joueur » pour en écouter d'autres).",
                           .category = "Parade", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = 193, .valueMin = 0, .valueMax = 0x1FF, .valueFormat = "%.0f", .defaultOn = true });
    Feature* const gp = &grunt;
    grunt.set = [gp](bool on) {
        g_guardGrunt = on ? static_cast<int>(gp->value + 0.5f) : -1;
        return true;
    };
    grunt.tick = [gp] { g_guardGrunt.store(static_cast<int>(gp->value + 0.5f), std::memory_order_relaxed); };
    Feature& grunt2 = Add({ .id = "combat.parry_guard_grunt2", .name = "Garde de Wesker : second grognement",
                            .help = "Un second son du joueur juste après le premier (196), après le délai réglé ci-dessous.",
                            .category = "Parade", .kind = Kind::Toggle,
                            .available = [] { return game::IsSupported(); },
                            .value = 196, .valueMin = 0, .valueMax = 0x1FF, .valueFormat = "%.0f", .defaultOn = true });
    Feature* const g2p = &grunt2;
    grunt2.set = [g2p](bool on) {
        g_guardGrunt2 = on ? static_cast<int>(g2p->value + 0.5f) : -1;
        return true;
    };
    grunt2.tick = [g2p] { g_guardGrunt2.store(static_cast<int>(g2p->value + 0.5f), std::memory_order_relaxed); };
    Feature& grunt2Delay = Add({ .id = "combat.parry_guard_grunt2_delay", .name = "Garde de Wesker : délai du second grognement",
                                 .help = "Le temps entre les deux grognements de la garde de Wesker.",
                                 .category = "Parade", .kind = Kind::Toggle,
                                 .set = [](bool) { return true; },
                                 .available = [] { return game::IsSupported(); },
                                 .value = 0.25f, .valueMin = 0.0f, .valueMax = 1.5f, .valueFormat = "%.2f s",
                                 .defaultOn = true });
    Feature* const gdp = &grunt2Delay;
    grunt2Delay.tick = [gdp] {
        g_guardGrunt2Delay.store(static_cast<int>(gdp->value * 1000.0f + 0.5f), std::memory_order_relaxed);
    };
    Feature& gruntVolume = Add({ .id = "combat.parry_guard_grunt_volume", .name = "Garde de Wesker : volume du grognement",
                                 .help = "Le volume des grognements de la garde de Wesker (les sons du personnage extraits par "
                                         "tools/extract_player_sounds.py, joués par le trainer ; au-delà de 100 %, un limiteur "
                                         "évite la saturation).",
                                 .category = "Parade", .kind = Kind::Toggle,
                                 .set = [](bool) { return true; },
                                 .available = [] { return game::IsSupported(); },
                                 .value = 400.0f, .valueMin = 100.0f, .valueMax = 800.0f, .valueFormat = "%.0f %%",
                                 .defaultOn = true });
    Feature* const gvp = &gruntVolume;
    gruntVolume.tick = [gvp] { g_guardGruntGain.store(gvp->value / 100.0f, std::memory_order_relaxed); };
    OnRestore(CloseVoices);
    Add({ .id = "combat.parry_guard_grunt_take", .name = "Garde de Wesker : prise du grognement",
          .help = "Le jeu a deux prises du grognement 193 et les alterne. « Essai : son du joueur » les jouait ensemble "
                  "(plusieurs fois le son à la fois) : c'est le réglage par défaut. Une prise seule donne toujours le "
                  "même son, plus léger. Le 196 n'a qu'une prise.",
          .category = "Parade", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_guardGruntTake = std::clamp(index, 0, 2);
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 2, .valueFormat = "%.0f",
          .choices = { "Les deux ensemble (comme l'essai)", "Première prise", "Seconde prise" } });
    Add({ .id = "combat.parry_grab_blood", .name = "Parade de saisie au couteau : du sang",
          .help = "Une saisie parée au couteau (riposte au couteau) fait saigner l'ennemi (son propre sang : rouge, jaunâtre "
                  "pour un parasite) au lieu des étincelles, avec son propre son de parade (fichier sounds\\parry_grab.wav, "
                  "« Couteau » choisi dans « Son des parades »).",
          .category = "Parade", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_grabBloodOn = on;
              return true;
          },
          .available = [] { return game::IsSupported(); },
          .defaultOn = true });
    Add({ .id = "combat.parry_sparks", .name = "Étincelles sur la parade au couteau (RE4 Remake)",
          .help = "La parade au couteau (riposte au couteau) fait jaillir les étincelles du jeu entre le couteau et l'arme "
                  "de l'ennemi, comme les deux métaux qui s'entrechoquent dans Resident Evil 4 Remake, à la place de "
                  "l'anneau ; les autres parades gardent l'anneau (si son option est cochée).",
          .category = "Parade", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_sparksOn = on;
              return true;
          },
          .available = [] { return game::IsSupported(); },
          .defaultOn = true });
    Feature& effectTry = Add({ .id = "diag.effect_try", .name = "Essai : effet du jeu (recherche)",
                               .help = "Fait apparaître devant Chris l'effet du jeu de ce numéro (effect\\efl\\core\\cr...), "
                                       "pour trouver les étincelles des parades. Régler le numéro, puis le raccourci ou le "
                                       "bouton.",
                               .category = "Diagnostic", .kind = Kind::Action,
                               .available = [] { return actors::InGame(); },
                               .value = 0x38, .valueMin = 0, .valueMax = 0x7F, .valueFormat = "%.0f" });
    Feature* const tp = &effectTry;
    // Research (user, 2026-11-06: Rebecca's fire spray on Chris): a character's effect list, character x 256 + list
    // (7448 = 0x1D18 = pl1d\pl1d18), on player 1's root.
    Feature& plTry = Add({ .id = "diag.effect_try_pl", .name = "Essai : effet de personnage (recherche)",
                           .help = "Fait apparaître sur Chris la liste d'effets de personnage de ce numéro (personnage x 256 + "
                                   "liste : 7448 = pl1d18, de Rebecca), pour trouver la flamme de son spray.",
                           .category = "Diagnostic", .kind = Kind::Action,
                           .available = [] { return actors::InGame(); },
                           .value = 7448, .valueMin = 0, .valueMax = 65535, .valueFormat = "%.0f", .valueStep = 1 });
    Feature* const plp = &plTry;
    Feature& jointTry = Add({ .id = "diag.effect_try_joint", .name = "Essai : effet de personnage, articulation (recherche)",
                              .help = "L'articulation de Chris où « Essai : effet de personnage » attache l'effet (0 = la "
                                      "racine).",
                              .category = "Diagnostic", .kind = Kind::Action,
                              .available = [] { return actors::InGame(); },
                              .value = 0, .valueMin = -1, .valueMax = 255, .valueFormat = "%.0f", .valueStep = 1 });
    Feature* const jp = &jointTry;
    jointTry.run = [jp] {
        g_effectTryJoint = static_cast<int>(jp->value + (jp->value < 0 ? -0.5f : 0.5f));
        return true;
    };
    plTry.run = [plp] {
        g_effectTryPending = 0x20000 | (static_cast<int>(plp->value + 0.5f) & 0xFFFF);
        return true;
    };
    effectTry.run = [tp] {
        g_effectTryPending = static_cast<int>(tp->value + 0.5f);
        Toast(i18n::Tr("Effet core n° ") + std::to_string(static_cast<int>(tp->value + 0.5f)), true);
        return true;
    };
    // Two effects, the game's 3 and 4 by default (user); the second one unchecked plays alone the first.
    static constexpr struct { const char* id; const char* name; float value; } kSparkEffects[2] = {
        { "combat.parry_sparks_effect", "Étincelles : numéro du premier effet du jeu", 3.0f },
        { "combat.parry_sparks_effect2", "Étincelles : numéro du second effet du jeu", 4.0f },
    };
    Feature& soundTry = Add({ .id = "diag.sound_try", .name = "Essai : son du joueur (recherche)",
                              .help = "Joue le son de ce numéro de la banque de sons du personnage joué (sa propre voix et "
                                      "ses bruits), seul et à 400 % (quatre fois à la fois), pour trouver le grognement de "
                                      "saut de la garde de Wesker.",
                              .category = "Diagnostic", .kind = Kind::Action,
                              .available = [] { return actors::InGame(); },
                              .value = 0x78, .valueMin = 0, .valueMax = 0x1FF, .valueFormat = "%.0f" });
    Feature* const sp = &soundTry;
    soundTry.run = [sp] {
        g_soundTryPending = static_cast<int>(sp->value + 0.5f);
        Toast(i18n::Tr("Son n° ") + std::to_string(static_cast<int>(sp->value + 0.5f)), true);
        return true;
    };
    for (int k = 0; k < 2; ++k) {
        Feature& sparkEffect = Add({ .id = kSparkEffects[k].id, .name = kSparkEffects[k].name,
                                     .help = "Les numéros des effets du jeu joués ensemble sur le couteau par « Étincelles sur "
                                             "la parade au couteau » (3 et 4 : les étincelles du jeu ; trouvés avec « Essai : "
                                             "effet du jeu »). Décoché : cet effet n'est pas joué.",
                                     .category = "Parade", .kind = Kind::Toggle,
                                     .available = [] { return game::IsSupported(); },
                                     .value = kSparkEffects[k].value, .valueMin = 0, .valueMax = 0x7F, .valueFormat = "%.0f",
                                     .defaultOn = true });
        Feature* const ep = &sparkEffect;
        sparkEffect.set = [ep, k](bool on) {
            g_sparkEffect[k] = on ? static_cast<int>(ep->value + 0.5f) : -1;
            return true;
        };
        sparkEffect.tick = [ep, k] { g_sparkEffect[k].store(static_cast<int>(ep->value + 0.5f), std::memory_order_relaxed); };
    }
    OnHud(DrawParryRing);
    Add({ .id = "combat.parry_riposte_hurts", .name = "La riposte blesse les autres ennemis",
          .help = "Coché : le coup de couteau qui suit une parade blesse les ennemis autour (seul l'ennemi paré est "
                  "épargné). Décoché : la riposte ne blesse personne.",
          .category = "Parade", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_riposteHurtsOthers = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "combat.parry_hazards", .name = "Parade contre le feu, les explosions et l'électricité",
          .help = "Coché : toute parade arrête aussi les explosions (barils, grenades, dynamite), le feu et les arcs "
                  "électriques du décor. Décoché : ils ne se parent pas. Les haches et carreaux lancés par les ennemis "
                  "se parent toujours.",
          .category = "Parade", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_parryHazards = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "combat.parry_dying", .name = "Parade en mourant",
          .help = "Coché : on peut aussi parer en mode mourant (le couteau y est sorti). Décoché : pas de parade en "
                  "mourant.",
          .category = "Parade", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_parryDying = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "combat.parry_grabbed", .name = "Parade pendant une saisie",
          .help = "Coché : on peut parer le coup d'un autre ennemi pendant qu'un ennemi te tient (la riposte casse alors "
                  "la saisie). Décoché : pas de parade tant que tu es saisi.",
          .category = "Parade", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_parryHeld = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    AddPanel("Parade", DrawParryPanel, { "Parade", "Parade par personnage" }, false, true);
}
}  // namespace features
