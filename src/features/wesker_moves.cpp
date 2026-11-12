// Wesker's and Jill's moves on the players (user, 2026-10-21: an RE7-style parry with Wesker's guard, an RE6-style parry
// with Chris's counter on him, a throw parry with Jill's throw, an automatic dodge like his). Research in MODLOG (Jill &
// Wesker fight): his motions are in pawn\em\em80\motion\em80 (guard 0x74, dodges 0x13 / 0x14 / 0x17 / 0x18 / 0x19 / 0x26
// / 0x28, sidestep 0x07) and the player's in his events pawn\em\em80\motion\em80_pl (the counter 0x01), both in
// Image\Archive\uEm80.arc; Jill's throw is the player's pawn\em\em83\motion\em83evt 0x01 (Image\Archive\uEm83.arc).
// Their skeletons share the body joints with the players'.
// A move: the lists are installed in player 1's free motion banks 0xC / 0xD / 0xE (uModel::SetMotionList RVA 0x1CCDD0,
// thiscall(list, slot)), a carrier action is requested (pl_act_emotion 0x81: plays one motion on layer 0, ends with it)
// and its layer 0 motion is replaced (the actor SetMotion hook of swaps.cpp asks WeskerMoveMotion) by the move's.
// First step: a test action (diag.wesker_move_test) playing the slider's move, to see them on Chris.
#include "common/arc.hpp"
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "features/melee_tuning.hpp"
#include "features/env_exec.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace features {
bool LoadWeaponArchive(const char* name);  // inventory.cpp: Image\Archive\<name>, kept loaded
bool EnsureSetMotionHook();                 // swaps.cpp
bool EnsurePlayerVoiceHook();               // player.cpp: Player::PlayVoice (the borrowed moves' speech skipped)
bool Player1RunHeld();                      // movement.cpp: the run button held
bool DyingActionsAllowed();                  // dying.cpp: "Actions en étant mourant"
bool StaminaAllows();                       // stamina.cpp: the red arc not filling
void StaminaSpendRe6Counter();              // stamina.cpp: the RE6 parry's counter
int Re6CounterDamage();                     // parry.cpp: the RE6 counter's blow, before the table
void StaminaSpend();                        // stamina.cpp: a free melee move started
void StaminaSpendDodge(float cost);         // stamina.cpp: a dodge started (its option's cost, "Endurance stricte")
int16_t LinWeskerDamage(uintptr_t enemy, int16_t amount);  // wesker_boss.cpp: his damage scaled
bool UsePadMaskForGroundRoll(bool on);      // movement.cpp: the pad's real run bit read
bool EnsurePlayerRequestHook();             // action_log.cpp
bool WeskerKeepsHealth(uintptr_t enemy);    // wesker_boss.cpp: "Wesker n'abandonne jamais"
int JillDeviceMeleeDamage(uintptr_t enemy);                      // jill_device.cpp: 0 = her health
bool JillKeepsHealth(uintptr_t enemy);                           // jill_device.cpp: "Figer la vie de Jill"
void DamageJillDevice(uintptr_t jill, int damage, const char* by);  // jill_device.cpp
bool BetaBackThrowOn();                     // beta_anims.cpp: "Projection de dos (animation beta)"
uint32_t ForeignMotion(int which, uint16_t index);  // re6_motions.cpp: RE4's lists put in player 1's own
void* Re6MeleeList(uint32_t& count);                // re6_motions.cpp: the RE6 melee test bank, loaded whole
uint32_t Re6MeleeEntry(uint16_t index);             // re6_motions.cpp: a motion entry of re6_melee.lmt (0: none)
bool Player1Sprinting();                            // movement.cpp: the trainer's sprint running
int Player1CharacterIndex();                        // swaps.cpp: 0 Chris, 1 Sheva, 2 Jill, 3 Wesker, 4 Josh, 5 Excella...
bool Re6MeleeFireHeld();                            // quick_knife.cpp: the attack key out of any aim
bool Re6MeleePromptShown();                         // quick_knife.cpp: a game melee prompt on screen
bool AttackKeyTakenElsewhere();                     // quick_knife.cpp: another option plays the attack key out of aim
bool Re6OnGround();                                 // below: RE6's ground, no trainer move
bool IsMajiniEnemy(uintptr_t enemy);                 // parry.cpp
bool IsKnifeItem(uint16_t id);                       // inventory.cpp
void SetWeaponJointUnit(uintptr_t unit, int index);  // player.cpp: a held weapon given RE6's weapon joint (0: none)
void SetStolenWeapon(uintptr_t unit, uintptr_t from, uintptr_t holder);  // player.cpp: a Majini's weapon in player 1's hand
void SetStolenWeaponHand(uint32_t joint);  // player.cpp: the hand holding it (8 / 13; 0 the Majini's own)
void DropStolenWeapon(uintptr_t unit, uintptr_t majini, float floor);  // player.cpp: its fall from player 1's hand
bool JointWorldById(uintptr_t unit, uint8_t id, float (&out)[3]);  // below: a joint's world place by its id
void KnifeWearRe6Move(int player, int which);        // knife_icon.cpp: "Durabilité du couteau", RE6's knife moves (0-2)
void SetReverseGrip(uintptr_t unit);                 // player.cpp: a held knife turned to the reverse grip (0: none)
bool EnsureAttachedMatrixHook();                     // player.cpp
uint16_t PlayerHeldItem(uintptr_t player);          // knife.cpp: the item in a player's hand
int Re6ParryStateNow();                             // parry.cpp: "État du Majini paré (parade RE6)"
bool PlayNativeMeleeMove(uint8_t action);           // swaps.cpp: the game's melee move on the nearest enemy (free melee)
void Player1BodyDirection(float& forward, float& left);  // movement.cpp
void Player1MoveDirection(float& forward, float& left);  // below: the side a trainer move plays (a left-handed player mirrored)
void Re6GroundLegs(uintptr_t p, uint32_t motion, bool wholeBody, float startFrame = 0.0f, bool ownRoot = false);  // movement.cpp
bool Re6WeaponTwoHanded(uintptr_t p);                    // re6_motions.cpp
int Re6HeldPoseSlot(uintptr_t p);                        // re6_motions.cpp
void SetRe6CameraDrop(float cm);                         // camera.cpp
void Re6GroundLegsOff(uintptr_t p);
bool Re6GroundLegsMirrored();                       // movement.cpp: the ground's legs play mirrored (a left-handed player)
void Re6AimLayers(uintptr_t p, const uint32_t (&motions)[3], float startFrame, bool sync, float blend = 4.0f);  // movement.cpp
bool StartRe6Layered(int which, float startFrame, bool fromMove);  // below
uint32_t PairVictimMotion(uintptr_t enemy, uint16_t index);  // re6_motions.cpp: an RE4 pair's enemy side in its lists
int Player1MeleeRow();                                       // swaps.cpp: the played character's melee row
void NoteTrainerMeleeKill(uintptr_t enemy);                  // mercenaries.cpp: its time bonus once counted
void PlayEffortGrunt(float gain);                            // parry.cpp: one of the played character's efforts
bool PlayMoveSound(const std::string& bank, int id, float gain);  // parry.cpp: a player bank's request, from its waves
bool ForeignRootAt(int which, uint16_t index, float frame, float (&out)[3]);  // re6_motions.cpp: a motion's root travel
bool MotionRootAt(uint32_t motionEntry, float frame, float (&out)[3]);        // re6_motions.cpp: any motion entry's
bool MotionRootYawAt(uint32_t motionEntry, float frame, float& yaw);         // re6_motions.cpp: a motion's root turn
bool ForeignRootYawAt(int which, uint16_t index, float frame, float& yaw);  // re6_motions.cpp
uint32_t EnemyMotionSlot(uintptr_t enemy, uint32_t entry, const char* what);  // re6_motions.cpp: a motion in its lists
void* JoshActionList();                                                      // swaps.cpp: his pl04action, kept loaded
uint32_t JoshMotionEntry(uint16_t slot);                                     // swaps.cpp: a motion entry of it
bool FlyingParasiteReact(uintptr_t enemy, const char* by);  // knife.cpp: a Kipepeo lets go (Release) or is blown (DmgBlow)
uint32_t SpawnCharacterEffect(int character, int list, uintptr_t unit, int joint);  // parry.cpp: pl<character><list>
void StopGameEffect(uint32_t handle);                                                // parry.cpp
bool LendMovesetForTrainerMove(int set, uint32_t carrier);  // swaps.cpp: a lender's effects, sounds, parts for the move

namespace {
constexpr int kWeskerBank = 0xC, kWeskerPlayerBank = 0xD, kJillBank = 0xE;
// Jill's moves in the second part of her fight (uEm84, Image\Archive\uEm84.arc; user, 2026-10-25: in the free melee and
// the melee swaps as Wesker's), in player 1's free banks 8 / 9 / A (his 0-6 his own, B-E the trainer's, F his events'):
// em84atk (her throw 0x3C, high kick 0x33, round-off 0x0B), em84nml (her cartwheels 0x1E / 0x1F), em84act (her wall
// runs and back flip candidates, for the test action).
constexpr int kJill2ActBank = 0x8, kJill2AtkBank = 0x9, kJill2NmlBank = 0xA;
// Jill's first part (uEm83): her somersault kick AtckKick 0x12 = em83act 0x00 (user, 2026-10-27: "the salto of phase 1
// that opens a dodge prompt, a kick too"). No bank left: em83act goes into bank 8 when one of its moves starts (the
// next em84act move puts its own back: InstallBanks); this pseudo bank says so.
constexpr int kJill1ActBank = 0x18;
// Josh's action list (pl04action, swaps.cpp JoshActionList): his HEAD CRUSH as a pair ("Josh Contre"), put in bank 8
// the same way when it starts; a pair's victim from 0x100 is slot (victim - 0x100) of that list, turned to player 1.
constexpr int kJoshActBank = 0x19;
constexpr int kJoshPairVictim = 0x100;
// RE6's synchronised sides (wesker_moves.cpp's RE6 pairs): kRe6PairVictim + the index of re6_melee.lmt; they start at their own
// origin (hips over it): the enemy is put kRe6PairDistance in front of player 1, facing him.
constexpr int kRe6PairVictim = 0x200;
constexpr float kRe6PairDistance = 100.0f;
// Some pairs placed otherwise (RE6 places each by its attack table): the victim's origin `distance` in front of player 1 and `left`
// to his left (cm). Chris's Leg Back (user, 2026-11-10: "Chris must wrap his arm round the enemy's neck, he is 50 cm off": the hips
// 50-58 cm apart through the hold at 1 m). Piers's Head Flash (user, 2026-11-10: "the character is too far left during the
// animation", "move him a little less than 50 cm to the right", then "too far: bring it back 15 cm", then "perfect sideways, but
// he is a little too far forward, barely 10 cm"): its victim 110 cm in front and 30 cm to his left, player 1 never moved. Leon's Leg Back
// (user, 2026-11-10: "shifted to the left by at least 30 cm"): its victim 30 cm to his left. Helena's Head Flash (user, 2026-11-10:
// "the same, Chris is too far left, at least 40 cm"): its victim 40 cm to his left. Piers's Leg Back (user, 2026-11-10: "the
// opposite, Chris is too far right, 10 cm, maybe a little more"): its victim 12 cm to his right. Jake's Head Flash (user,
// 2026-11-10: "Chris goes a little too far forward, 5 to 10 cm", then at 108 "I see no difference: move the Majini back another ten
// cm"): its victim at 118 cm. Jake's Ground (user, 2026-11-10: "Chris is too far forward too, move the victim 15 cm", then "still too close, another 15 cm", then "maybe I am wrong: back to the start and the other way, 90", then "closer still, 75 maybe", then "better, 5 more and it will be right, 70"): at 70 cm.
struct Re6PairPlace {
    const char* id;
    float distance, left;
};
// The pairs whose victim is thrown far for a fixed time (user, 2026-11-10, Jake's Ground and Sherry's Head Flash: against a wall "it stays
// frozen in the air until the animation ends"; "once it took off, cut the animation and put a backward blow-off in its place, as from a
// shotgun"): at the blow the pair ends and the Majini is blown off by the game's own reaction (walls, landing, dead when it lands at 0).
// Sherry's Head Flash at its blow (user-verified: "perfect"). Jake's Ground (user: "cut too early, and the enemy does not go the right
// way": Jake lifts its J'avo at the blow, frame 100, and lets it go flying at ~114): the blow only damages (a lethal one dies
// when it lands), the pair ends once its root takes off (TickPairLock), the Majini blown off along the throw's way, straight
// (Re6PairTakesOff, its blow-off 0x528: user, "not the spinning fall a shotgun hit gives, really the straight projection").
bool Re6PairBlowsOff(const char* id) { return std::strcmp(id, "melee.free_re6_sherry_head_flash") == 0; }
bool Re6PairTakesOff(const char* id) {
    return std::strcmp(id, "melee.free_re6_jake_ground") == 0 || std::strcmp(id, "melee.free_re6_jake_ground_belly") == 0;
}
constexpr Re6PairPlace kRe6PairPlaces[] = {
    { "melee.free_re6_chris_leg_back", 55.0f, 0.0f },
    { "melee.free_re6_piers_head_flash", 110.0f, 30.0f },
    { "melee.free_re6_leon_leg_back", kRe6PairDistance, 30.0f },
    { "melee.free_re6_helena_head_flash", kRe6PairDistance, 40.0f },
    { "melee.free_re6_piers_leg_back", kRe6PairDistance, -12.0f },
    { "melee.free_re6_jake_head_flash", 118.0f, 0.0f },
    { "melee.free_re6_jake_ground", 70.0f, 0.0f },
    // Ada's Head Flash (user, 2026-11-10: "Chris slightly too far right (5 cm)", then at 5 "no difference: still too far right of the
    // Majini's shoulders in the scissors", then at 15 "the gap looks even larger, you move it the wrong way: back to 0 and 5 cm the other
    // way"): its victim 5 cm to his left (Ada's scissors read the other way round from Piers's Leg Back), then "better, the right way, not
    // enough: 10": 10 cm to his left.
    { "melee.free_re6_ada_head_flash", kRe6PairDistance, 10.0f },
    // Its face-down one (user, 2026-11-10: "Chris is shifted about 20 cm to the left"): its victim 20 cm to his left.
    // Ada's Leg Back (user, 2026-11-10: "Chris a little too far forward: back 5 cm for now", then "better, another 5, and Chris is 5 cm to
    // the left"): its victim at 110 cm, 5 cm to his left (from behind, as Piers's Leg Back).
    { "melee.free_re6_ada_leg_back", 110.0f, 5.0f },
    { "melee.free_re6_common_knife_counter", kRe6PairDistance, -60.0f },  // user, 2026-11-10: "Chris about 60 cm to the right"
    { "melee.free_re6_jake_ground_belly", 70.0f, 20.0f },  // turned the other way (fromBehind), the face-up one's distance to start
    // The knife's throat cut (user, 2026-11-10: "Chris too far behind the Majini: at least 15 cm closer", then "another 10 cm").
    { "melee.free_re6_common_throat_cut", 75.0f, 0.0f },
    // The machete disarm (user, 2026-11-10: "Chris too far from the Majini: at least 20 cm closer", then "another 20 cm").
    { "melee.free_re6_common_machete_counter", 10.0f, 0.0f },  // then "still 20 cm closer" (40), again (20), "10 cm more" (10)
    // The gun counter, bottles included (user, 2026-11-10: "the bottle counter works but I am much too far: bring Chris 45 cm closer").
    { "melee.free_re6_common_gun_counter", kRe6PairDistance - 55.0f, 0.0f },  // then "still 10 cm closer" (45)
    // The armed-zombie counters and the weapon steal: RE6 plays both sides from the same origin (recorded 2026-11-10: the player and the
    // zombie at the same place from the pair's first frame).
    { "melee.free_re6_common_club_counter_1", 0.0f, 0.0f },
    { "melee.free_re6_common_club_counter_2", 0.0f, 0.0f },
    { "melee.free_re6_common_weapon_steal", 0.0f, 0.0f },
    // The environmental executions (recorded in RE6 2026-11-11, research/re6_live/env_origin.py): RE6 puts both units at the same place on
    // the pair's frames ~23-47, each moved by its root; the J'avo's side's origin from the player's, the roots before the import's reroot.
    { "melee.free_re6_common_env_wall", 54.9f, -40.9f },         // AcA 3E / CoC 3E (the user's "wall at the side")
    { "melee.free_re6_common_env_table_side", 88.7f, -45.3f },   // AcA 42 / CoC 42 ("barrier at the side")
    { "melee.free_re6_common_env_table_front", 64.0f, -50.4f },  // AcA 44 / CoC 44 (a table / barrier, from the front)
    // From a height (the J'avo's origin 100 cm up from below, on the floor from above: research/re6_live/env_origin.py).
    { "melee.free_re6_common_env_below_facing", 112.8f, -28.2f },  // AcA 36 / CoC 36 ("from below, the enemy facing me")
    { "melee.free_re6_common_env_below_behind", 92.2f, -40.9f },   // AcA 38 / CoC 38 ("from below, the enemy's back")
    { "melee.free_re6_common_env_above_facing", 119.3f, -43.0f },  // AcA 3A / CoC 3A ("from above")
    { "melee.free_re6_common_env_above_behind", 114.0f, -45.8f },  // AcA 3C / CoC 3C (from above, its back: not recorded)
};
// The characters' duo moves of Mercenaries Reunion (user, 2026-11-05: the actions 0x47 / 0x48 asked of Chris played his
// TAG UPPER and TAG NECK - "add them to the free melee, and every character's"): the melee table's moves 0x19 / 0x1A
// (TAG ...) give their slot in the character's action list. Each list loaded once and kept, put in bank 8 when its move
// starts (as Josh's): Chris's and Sheva's long lists (the loose copies of tools/extract_reunion_motions.py; their short
// ones lack these slots), the Reunion characters' own.
struct PlayerMoveList {
    const char* archive;  // nullptr: a loose file of nativePC_MT
    const char* path;
    int loanSet;  // its character in swaps.cpp kLoanSets: its effects, sounds and model parts lent for the move
};
constexpr PlayerMoveList kPlayerLists[] = {
    { nullptr, "pawn\\pl\\pl00\\motion\\pl00action_reunion", 7 },       // 0 Chris
    { nullptr, "pawn\\pl\\pl01\\motion\\pl01action_reunion", 0 },       // 1 Sheva
    { "uPl04JoshCos2", "pawn\\pl\\pl00\\motion\\pl04action", 3 },       // 2 Josh
    { "uPl05ExcellaNormal", "pawn\\pl\\pl01\\motion\\pl05action", 6 },  // 3 Excella
    { "uPl06BarryNormal", "pawn\\pl\\pl00\\motion\\pl06action", 4 },    // 4 Barry
    { "uPl07RebeccaNormal", "pawn\\pl\\pl01\\motion\\pl07action", 5 },  // 5 Rebecca
};
constexpr int kPlayerListBank = 0x28;  // + the index in kPlayerLists
constexpr int kChrisListBank = kPlayerListBank, kShevaListBank = kPlayerListBank + 1, kJoshListBank = kPlayerListBank + 2,
              kExcellaListBank = kPlayerListBank + 3, kBarryListBank = kPlayerListBank + 4,
              kRebeccaListBank = kPlayerListBank + 5;
void* g_playerLists[std::size(kPlayerLists)] = {};
bool PlayerListBank(int bank) { return bank >= kPlayerListBank && bank < kPlayerListBank + static_cast<int>(std::size(kPlayerLists)); }
// Leon's moves (user, 2026-10-28: in the free melee and the dodges, tested on Chris one by one): pseudo banks for the
// lists of DataDir\motions re6_motions.cpp puts in empty slots of player 1's own lists (ForeignMotion): RE4 (2005)
// re4_leon_moves.lmt (tools/re4_build_moves.py, research\re4_anims\REPORT.md), RE4 Remake re4r_leon_parry.lmt and
// re4r_leon_grab_moves.lmt (tools/re4r_to_lmt.py, research\re4r_anims).
constexpr int kRe4LeonBank = 0x20, kRe4rParryBank = 0x21, kRe4rGrabBank = 0x22, kRe6EvadeBank = 0x23;
// The RE6 melee test bank (user, 2026-11-09, research: re6_melee.lmt put whole in bank 8 as Josh's list): its motion is
// g_re6TestIndex ("Test : corps à corps de RE6 (AcA)").
constexpr int kRe6MeleeBank = 0x24;
std::atomic<int> g_re6TestIndex{ 0 };
void* g_jill1ActList = nullptr;  // pawn\em\em83\motion\em83act, reference kept
constexpr uint32_t kCarrier = 0x81;  // pl_act_emotion

struct ListSource {
    const char* archive;
    const char* path;
    int bank;
};
constexpr ListSource kLists[] = {
    { "uEm80", "pawn\\em\\em80\\motion\\em80", kWeskerBank },
    { "uEm80", "pawn\\em\\em80\\motion\\em80_pl", kWeskerPlayerBank },
    { "uEm83", "pawn\\em\\em83\\motion\\em83evt", kJillBank },
    // Optional: without them only Jill's second-part moves fail.
    { "uEm84", "pawn\\em\\em84\\motion\\em84act", kJill2ActBank },
    { "uEm84", "pawn\\em\\em84\\motion\\em84atk", kJill2AtkBank },
    { "uEm84", "pawn\\em\\em84\\motion\\em84nml", kJill2NmlBank },
};
constexpr size_t kRequiredLists = 3;

struct Move {
    const char* name;
    int bank;
    uint16_t index;
};
constexpr Move kMoves[] = {
    { "garde (em80 0x74)", kWeskerBank, 0x74 },
    { "contre (em80_pl 0x01)", kWeskerPlayerBank, 0x01 },
    { "esquive DmgNot 0x71 (em80 0x14)", kWeskerBank, 0x14 },
    { "esquive DmgNot 0x72 (em80 0x13)", kWeskerBank, 0x13 },
    { "esquive DmgNot 0x75 (em80 0x18)", kWeskerBank, 0x18 },
    { "esquive DmgNot 0x76 (em80 0x19)", kWeskerBank, 0x19 },
    { "esquive DmgNot 0x77 (em80 0x26)", kWeskerBank, 0x26 },
    { "esquive DmgNot 0x79 (em80 0x17)", kWeskerBank, 0x17 },
    { "projection de Jill (em83evt 0x01)", kJillBank, 0x01 },
    { "coup de pied AtkKick 0x24 (em80 0xC8)", kWeskerBank, 0xC8 },
    { "coup de pied AtkKick 0x25 (em80 0xC9)", kWeskerBank, 0xC9 },
    { "coup de pied AtkKick 0x26 (em80 0xCA)", kWeskerBank, 0xCA },
    { "balayage AtkLowKick 0x56 (em80 0xDE)", kWeskerBank, 0xDE },
    { "riposte de fin d'esquives EscapeTurn 0x6F / 0x70 (em80 0x2C)", kWeskerBank, 0x2C },
    // Jill, second part (MODLOG): her wall runs (one motion each: up 0x00 -> 0x01 -> 0x04, off with a somersault 0x03 /
    // 0x07) and the back flip candidates (seen once by the user, not logged).
    { "Jill 2 : course murale, début (em84act 0x00)", kJill2ActBank, 0x00 },
    { "Jill 2 : course murale, montée (em84act 0x01)", kJill2ActBank, 0x01 },
    { "Jill 2 : course murale, arrivée (em84act 0x04)", kJill2ActBank, 0x04 },
    { "Jill 2 : course murale, salto (em84act 0x03)", kJill2ActBank, 0x03 },
    { "Jill 2 : course murale, salto (em84act 0x07)", kJill2ActBank, 0x07 },
    { "Jill 2 : flip arrière ? (em84nml 0x20)", kJill2NmlBank, 0x20 },
    { "Jill 2 : flip arrière ? (em84nml 0x21)", kJill2NmlBank, 0x21 },
    { "Jill 2 : flip arrière ? (em84act 0x1E)", kJill2ActBank, 0x1E },
    { "Jill 2 : flip arrière ? (em84act 0x1F)", kJill2ActBank, 0x1F },
    // Her round-off somersault (user: em84atk 0x0B, first taken for it, is her drawing both pistols): the candidates.
    { "Jill 2 : rondade ? (em84atk 0x03)", kJill2AtkBank, 0x03 },
    { "Jill 2 : rondade ? (em84atk 0x06)", kJill2AtkBank, 0x06 },
    { "Jill 2 : rondade ? (em84atk 0x0C)", kJill2AtkBank, 0x0C },
    { "Jill 2 : rondade ? (em84atk 0x0E)", kJill2AtkBank, 0x0E },
    { "Jill 2 : rondade ? (em84atk 0x0F)", kJill2AtkBank, 0x0F },
    // Her forward sweep (AtckAshibarai / AtckAshibaraiHit, seen once by the user, never logged): the candidates, 0x34 / 0x35
    // beside her kick first.
    { "Jill 2 : balayage ? (em84atk 0x34)", kJill2AtkBank, 0x34 },
    { "Jill 2 : balayage ? (em84atk 0x35)", kJill2AtkBank, 0x35 },
    { "Jill 2 : balayage ? (em84atk 0x0A)", kJill2AtkBank, 0x0A },
    { "Jill 2 : balayage ? (em84atk 0x05)", kJill2AtkBank, 0x05 },
    { "Jill 2 : balayage ? (em84atk 0x3D)", kJill2AtkBank, 0x3D },
};
// (em80 0x28 and the sidestep 0x07 were dropped: a step and a half turn, neither dodge nor attack - user.)

void* g_lists[std::size(kLists)] = {};  // references kept
std::atomic<uint32_t> g_pendingMotion{ 0 };  // (bank << 8) | index for player 1's next layer 0 motion
std::atomic<DWORD> g_pendingSince{ 0 };
std::atomic<float> g_pendingBegin{ 0.0f };   // its start frame (CombatMove::begin)
float g_beginNow = -1.0f;                    // game thread: the start frame of the motion just put (MoveBeginStart)
// The carrier action 0x81 is the game's speech gesture: a borrowed boss move made Chris call the partner, who answered
// (user, 2026-10-27). The players' voice lines are skipped from a move's start for 2.5 s (player.cpp's voice hook).
std::atomic<DWORD> g_bossMoveStart{ 0 };
std::atomic<uintptr_t> g_thrownUnit{ 0 };    // a Majini in Jill's throw: its BackHoldDmg motion becomes Jill's
std::atomic<DWORD> g_thrownSince{ 0 };
// The riposte's motion: the guard em80 0x74 (bank C), or Chris's counter on Wesker em80_pl 0x01 (bank D) for the
// RE6-style parry (user, 2026-10-23); both invulnerable, the guard alone sped up.
std::atomic<uint32_t> g_riposteMotion{ 0 };
// The counter's target: its cObjAdj contacts with player 1 dropped while the counter plays (user, 2026-10-23: Chris and
// the Majini were both pushed at the parry, as with Jill's throw before).
std::atomic<uintptr_t> g_counterTarget{ 0 };
// The counter's punch still to land on g_counterTarget (parry.cpp CounterPunchLands at its kCounters frame).
std::atomic<bool> g_counterPunchDue{ false };
// An enemy's next layer 0 motion replaced (parry.cpp: the RE6 parry's arm hit 0x514), within 500 ms of the request.
std::atomic<uintptr_t> g_forcedMotionUnit{ 0 };
std::atomic<uint32_t> g_forcedMotion{ 0 };
std::atomic<DWORD> g_forcedMotionTick{ 0 };
// The straight blow-off (Re6PairTakesOff, Re6PairBlowsOff; user, 2026-11-10: "not the spinning fall", "always the right one, not random"):
// that Majini's DmgFlyoff flights (the game picks one of 0x528 / 0x52A / 0x52C for a front hit) become 0x52A and their landings 0x52B,
// for 3 s. Measured frame by frame on bone 1 (the game plays the flight 0.3-0.4 s, until it lands): 0x528 turns 317 degrees round the
// vertical in its first 25 frames (the spin), 0x52A goes straight back to horizontal (106 degrees, no turn), 0x52C straight back into
// a back flip (141), 0x534 only tilts back 75 (user: "less pretty").
std::atomic<uintptr_t> g_straightBlowUnit{ 0 };
std::atomic<DWORD> g_straightBlowTick{ 0 };
float g_thrownSpeed = 0.0f;    // render thread: its own motion speed (1.2 in Mercenaries), back after the throw
bool g_thrownLethal = false;   // render thread: the throw's damage kills it once it lies
// A survivor of Jill's second throw, up again with Chris's side of it: its getup request 0x6B becomes its Wait.
std::atomic<uintptr_t> g_jill2Survivor{ 0 };
std::atomic<DWORD> g_jill2SurvivorSince{ 0 };
// Jill's Caught motion 0x23 is in her bank 0 list em83dmg (146 frames; her bank 5 is em83evt, whose 0x23 is a held
// motion of 104 frames - user: the Majini started a hold and froze). Given to the Majini in its free bank E.
constexpr uint32_t kJillThrownMotion = 0xE23;
void* g_jillDamageList = nullptr;  // pawn\em\em83\motion\em83dmg, reference kept
// Jill's second throw (her Catch em84atk 0x3C on player 1): the Majini plays Chris's side of it, em84evt 0x00 (247
// frames: his motion 0xF00 when she throws him, motions.log 2026-10-25), in its free bank C. The thrown motion of the
// throw in progress: one or the other.
constexpr int kJill2VictimBank = 0xC;
constexpr uint32_t kJill2VictimMotion = static_cast<uint32_t>(kJill2VictimBank) << 8;
std::atomic<uint32_t> g_thrownMotion{ kJillThrownMotion };
std::atomic<uint32_t> g_pairMotion{ 0 };  // the last RE4 pair side played by a Majini (its motion id in its lists)
// RE4 pairs' sides (scratchpad lying_poses.py on re4_pairs_shared_origin.lmt and em10_g05, 2026-10-31): the suplexed one
// ends on its back, the neck-broken one on its belly, each with its head the other way round than the game's lying
// motions want (those start with the hips on the unit's origin, the head opposite its facing on the back - em10_g05
// 0x52 / 0x54 / 0x56 / 0x3C - and along it on the belly - 0x53 / 0x55 / 0x57 / 0x3D, 6 degrees off) and its body away
// from its origin: the unit is turned and moved under the body when its next motion is one of those (PairLyingRealign).
bool PairEndsFaceDown(int victim) { return victim == 2; }
std::atomic<uintptr_t> g_pairVictim{ 0 };  // the unit playing a pair's side, until it lies in one of the game's motions
std::atomic<DWORD> g_pairVictimTick{ 0 };
std::atomic<bool> g_pairPuppet{ false };   // the puppet in a pair: from its pair's side straight to its getup
// Killed during the pair (user, 2026-10-31: "Chris's neck break kills at the twist, then the dead enemy falls"): its death
// action's first motion is the pair's side going on from where it was (PairCorpseMotion).
std::atomic<uintptr_t> g_pairCorpse{ 0 };
std::atomic<DWORD> g_pairCorpseTick{ 0 };
std::atomic<float> g_pairCorpseFrame{ 0.0f };
// Its own death motion asked once the pair's fall is over (TickThrown): realigned under the body (PairLyingRealign).
std::atomic<uintptr_t> g_corpseRealign{ 0 };
std::atomic<bool> g_corpseFaceDown{ false };
std::atomic<int> g_pairCorpseVictim{ -1 };  // the killed pair's victim side (KillPairVictim)
// A native death started part-way (the machete disarm's victim: its standing death from its knees): the unit, its first motion's frame.
std::atomic<uintptr_t> g_corpseNative{ 0 };
std::atomic<float> g_corpseNativeStart{ 0.0f };
std::atomic<uint32_t> g_corpseNativeMotion{ 0 };  // its first motion instead of the death's own (0: the death's)
// Test sliders of the pairs' placement (0 the neck break, 1 the suplex: Chris's side / forward offset in cm, his turn
// in degrees), removed once set (user).
std::atomic<float> g_pairTest[2][3]{};
void PairTestOffset(int test, float& side, float& forward, float& turn) {
    side = g_pairTest[test][0].load();
    forward = g_pairTest[test][1].load();
    turn = g_pairTest[test][2].load();
}
void* g_jill2EventList = nullptr;  // pawn\em\em84\motion\em84evt, reference kept
// Jill's second throw on Jill herself (user, 2026-10-25: Chris played her throw and she the game's own Caught, out of
// step, not Chris's side of it): she plays Chris's side (em84evt 0x00, the list given in her bank C) from his origin,
// turned to him, through her knock down with its motion replaced, collision off and held there while his motion plays;
// the throw's damage without her reaction (Strike), never her last health point.
std::atomic<uintptr_t> g_jillVictim{ 0 };
// The leap's target (the nearest enemy within 3 m at its start: caught from 20 % of the leap, as the native one at frame
// 36, user: "hard to touch Jill on the move"). Neither is held after the start: in the native catch both move with their
// motions (sampled: together 70 cm forward from frame 35, then Chris 2.8 m behind her from frame 50: thrown over her);
// held on the spot the victim never went over (user).
uintptr_t g_leapTarget = 0;
// Jill knocked down by a move of the trainer's (user, 2026-10-25: after the new moves she "teleports standing"): her AI's
// next request out of that fall becomes her getup (DmgGetup), as after a fall of the game's own.
std::atomic<uintptr_t> g_jillKnocked{ 0 };
std::atomic<DWORD> g_jillKnockedTick{ 0 };
float g_jillVictimAt[3] = {}, g_jillVictimQuat[4] = {};
DWORD g_jillVictimSince = 0;
bool g_jillVictimSeen = false;
// Jill's throw on an Executioner (uEm20: no back hold nor lying state of its own, user 2026-10-22): its Dmg action
// (cEm20ActDmg, init RVA 0x474880 plays its layer 0 motion, update RVA 0x46A2E0 ends it with that motion: +0x388 & 4)
// carries Jill's motions in turn, thrown em83dmg 0x23 -> lying 0x25 (180 frames, her own after Caught) -> a Majini's
// face-up getup (em10_g05 0x54 of RiseFaceUp, in its bank D; user: Jill's own getup 0x06 looked like playable Jill and
// took the camera onto him), each phase a new Dmg request put in place of
// what its AI asks once the phase's motion ended (WeskerPuppetRequest, enemy request hook); a lethal throw dies lying
// (Die 0x23 with the lying motion). Phase 1..3, 4 = its death asked (lying motion pending), 0 = none.
std::atomic<uintptr_t> g_puppet{ 0 };
std::atomic<int> g_puppetPhase{ 0 };
std::atomic<uint32_t> g_puppetMotion{ 0 };  // the puppet's next layer 0 motion (SetMotion hook)
std::atomic<DWORD> g_puppetSince{ 0 };      // its phase's start
std::atomic<bool> g_puppetOwn{ false };     // our own request is going through the request hook
constexpr int kMajiniRiseBank = 0xD;
uint32_t kPuppetMotions[3] = { 0xE23, 0xE25, 0xD54 };  // the getup falls back on Jill's 0xE06 without the Majini's list
void* g_majiniRiseList = nullptr;  // pawn\em\em10\motion\em10_g05, reference kept
// "Relevé acrobatique" (user): player 1 gets up from a knock down with Jill's getups (em83dmg 0x06 from the back,
// 0x07 from the belly; her DmgGetup init RVA 0x5363D0 picks one of them) in his free bank B: his getup actions
// pl_act_damage_get_up_front 0x97 (lying on his back: his own motion pl00damage 0x06, logged) / _back 0x98 (their
// motions come from the action tables, not their inits; user: the first try had them swapped).
constexpr int kPlayerJillDamageBank = 0xB;
std::atomic<bool> g_acrobaticGetup{ false };
// "Roulade arrière au sol (RE6)" (user, 2026-10-26: "as in RE6, Chris knocked down with the run button held plays it to
// cut the wait on the ground before getting up"): Chris's back roll of Lost in Nightmares, his dodge of the DLC Wesker's
// stomp (Humituke_Kaihi_PL, init RVA 0x520020: bank F + its character's base [+0x28] + 0x20; pawn\em\ema2\motion\ema2_pl
// 0x20, 81 frames, from lying on his back to a guard, about 1.9 m; uEma2.arc). The first one taken, 0x1E (127 frames),
// is the struggle's escape Humituke_Esc_PL (init RVA 0x51FFA0: + 0x1E; user: "not that one").
// in player 1's free bank 7. Lying on his back (lie_wait front 0x95) with the run button held, his getup front 0x97 is
// asked at once and plays it (SetMotion hook), in place of pl00damage 0x06 (or Jill's with "Relevé acrobatique").
constexpr int kPlayerRollBank = 0x7;
constexpr uint32_t kGroundRollMotion = (static_cast<uint32_t>(kPlayerRollBank) << 8) | 0x20;
std::atomic<bool> g_groundRoll{ false };
std::atomic<DWORD> g_groundRollAsked{ 0 };  // our getup asked then
// Thrown down he never lies waiting (0x95): blow_s 0x86 (pl00damage 0x04 / 0x05 / 0x16 / 0x17), blow_l 0x89 (0x0C /
// 0x0D), knock_down 0x8B / 0x8C (0x13) play the fall and the wait on the ground in one motion, then the game asks 0x97
// (user, 2026-10-26: "it starts after the wait, useless"). Landed = his hips (joint 0, world matrix [P+0x318] + 0x50,
// translation +0x80) under 35 cm above his feet (P+0x34), facing up = joint 1's z axis (+0x70) pointing up (y > 0.6).
// Every way he is thrown down (user: "he has several, all of them"): blow_s 0x86-0x88, blow_l 0x89 / 0x8A, knock_down
// 0x8B / 0x8C, damage_wesker_blow 0xAF / _wall_hit 0xB0; the landing read from his skeleton, not per motion.
// Face down (user, 2026-10-26: "a forward roll then"; no game has one from the belly: RE5's lists and the DLC's only
// get up plainly - pl00damage 0x07 - and RE6's damage list pl0000DmA has no upper body): his dodge roll forward
// (roll_front 0x51, pl00action 0x22 = motion 0x122, 82 frames: bent forward to 16, hands down at 20, on his back
// curled 24-32, standing by 56) from its frame 20, so he pushes off the ground into it.
constexpr uint32_t kForwardRollAction = 0x51, kForwardRollMotion = 0x122;
constexpr float kForwardRollStart = 20.0f;
std::atomic<DWORD> g_bellyRollAsked{ 0 };

bool FallAction(uint32_t action) { return (action >= 0x86 && action <= 0x8C) || action == 0xAF || action == 0xB0; }

bool LyingFaceDown(uintptr_t p);  // below

// Its hips (joint 0) and the next joint's front (a Majini's pelvis, id 15; a player's spine, id 1). The Executioner's skeleton starts with
// a root on the floor (index 0, id 0) before its hips (index 1, id 0 again) and pelvis (index 2, id 15) - read live, 2026-11-10: its hips
// were read at 0 cm, never lying, and it got up with the getup from the back after falling on its belly (user): one joint further there.
bool LyingOnBack(uintptr_t p, float* hipHeight, float* facing) {
    uintptr_t joints = 0;
    float hip[3]{}, z[3]{}, feet = 0;
    uint8_t second = 0xFF;
    if (!mem::Read(p + 0x318, joints) || !joints) return false;
    const uintptr_t first = joints + (mem::Read(joints + 0x90 + 7, second) && second == 0 ? 0x90 : 0);
    if (!mem::SafeRead(reinterpret_cast<void*>(first + 0x80), hip, sizeof(hip)) ||
        !mem::SafeRead(reinterpret_cast<void*>(first + 0x90 + 0x70), z, sizeof(z)) || !mem::Read(p + 0x34, feet))
        return false;
    *hipHeight = hip[1] - feet;
    *facing = z[1];
    return *hipHeight < 35.0f && *facing > 0.6f;
}

bool DownFaceDown(uintptr_t e);  // below
bool LyingFaceDown(uintptr_t p) {
    float hip = 0, facing = 0;
    LyingOnBack(p, &hip, &facing);
    return hip > 0.0f && hip < 35.0f && facing < -0.6f;
}
// Not by its state (user, 2026-11-10: "it turned the enemy over though it was already on its back": a Majini on its back is in the
// face-down states too - RiseFaceDown 0x6B, the AI's pick after a fall, MODLOG session 15): its pelvis alone.
bool DownFaceDown(uintptr_t e) {
    uint32_t action = 0;
    mem::Read(e + 0x10E0, action);
    float hip = 0, facing = 0;
    LyingOnBack(e, &hip, &facing);
    LOG("wesker moves: %08X down (action %X): hips %.0f cm, pelvis front %.2f", static_cast<unsigned>(e), action, hip, facing);
    return hip > 0.0f && hip < 45.0f && facing < 0.0f;
}
void* g_ema2PlayerList = nullptr;
// The classes thrown that way: their Dmg slots (two of them, not the same twice in a row), their death played lying and
// the deaths their AI may ask. The chainsaw Majini (uEm22, user 2026-10-23) has the Executioner's layout (its action
// table read live): cEm22ActDmg 0x17 / 0x18 / 0x1D / 0x1E (init RVA 0x47BBD0 plays motion 0x50 / 0x51 through VA
// 0x9D9EC0, 0x1D / 0x1E also turn it; update RVA 0x478460 ends it with the motion, +0x1BA0 = 1, as cEm20ActDmg), Die
// 0x1F, DieLie 0x20, DieBomb 0x21, DieIce 0x22, Getup 0x23; its banks 1-F are free.
struct PuppetClass {
    uintptr_t vtable;  // RVA
    int dmg[3];
    int die, deathFirst, deathLast;
    const char* name;
    uint32_t ownGetup;    // its own getup motion (bank 0), else a Majini's (a human)
    bool driven = false;  // its phases follow the motions' frames (TickDrivenPuppet), its AI refused meanwhile
    // Out of a synchronised kill (WeskerForcedGetup: lying on his belly): his getup from the belly (bank 0), then this
    // action of his in place of his AI's (-1: his AI).
    uint32_t fallenGetup = 0;
    int afterFallen = -1;
};
// The Reaper (uEm34, user 2026-10-23: "try it anyway", then "very good"): its Damage is a multi-step state machine, but
// cEm34ActMotion 0x05 / 0x07 plays one motion (init RVA 0x4A5930: motion 0x0C / 0x0E through vfunc +0x168) and ends with
// it (update RVA 0x4A08B0, +0x1BA0 = 1), as cEm20ActDmg. Blown 0x21 / Die 0x22. Its getup: without one it stood up at
// once and killed him (user); cEm34ActBlown (init RVA 0x4A61E0 plays 0x5C, update RVA 0x4A0FF0 then 0x5B once it lands)
// gets up with motion 0x5B.
// Wesker (uEm80, user 2026-10-25: "Jill's throw does not work on him"): cEm80ActDmgS 0x58-0x63 (init RVA 0x51A430 plays
// one layer 0 motion chosen by its slot, update RVA 0x511660 ends with it: +0x388 & 4 / 5 or its last frame, then
// +0x1BA0 = 1; in the fight's room 0x2C2 the end waits for RVA 0x508370 too: layer 0's motion ended). His action table
// read live (MODLOG). No death in this fight: the throw never takes his last health point (Strike); his motion banks 4-F
// are free. Driven (user: "he stays on the ground for three hours"): Jill's lying motion never ends there and his AI
// asks nothing meanwhile, and his thrown motion was cut at 1.2 s by an early end flag; so thrown em83dmg 0x23 to its last
// frame (he lands about 1 s before it), then at once his own getup after the native counter (user: em80 0x2D, 122
// frames; his NeeNg 0x34 plays 0x23, 144 frames, then 0x2D), then his AI again.
constexpr PuppetClass kPuppetClasses[] = {
    { 0xB748A8, { 0x1D, 0x1E, 0x1D }, 0x23, 0x23, 0x25, "Executioner", 0 },  // uEm20: cEm20ActDmg, cEm20ActDie
    // Lost in Nightmares' axe man (uEm27, user 2026-10-26: "Jill's throw does not throw the DLC's monster"): the
    // Executioner's actions under their cEm20 names in the same slots (table read live: AtkCatch 0x17, KneelDown 0x1C,
    // Dmg 0x1D-0x1F, Die 0x23, DieBomb 0x24, DieIce 0x25, AtkExecCatch 0x30 / Kill 0x31).
    { 0xB74E38, { 0x1D, 0x1E, 0x1D }, 0x23, 0x23, 0x25, "Lost in Nightmares' Executioner", 0 },
    { 0xB75B20, { 0x17, 0x18, 0x17 }, 0x1F, 0x1F, 0x22, "chainsaw", 0 },     // uEm22
    { 0xB7AA88, { 0x05, 0x07, 0x05 }, 0x22, 0x21, 0x22, "Reaper", 0x5B },    // uEm34: cEm34ActMotion
    // A neck break leaves him on his belly (user, 2026-10-26: "not the getup after the counter, his back; the one after a
    // full combo"): the combo's end AtkSyutoRevEnd 0x50 (update RVA 0x510B70) plays his fall em80 0xE7 / 0xD6, lying
    // 0x03, then 0x0E, 128 frames from the belly (hips facing down, 11 cm) to standing (tools/lmt.py, 2026-10-26).
    { 0xB8BE60, { 0x58, 0x59, 0x58 }, 0x58, -1, -1, "Wesker", 0x2D, true, 0x0E },  // uEm80: cEm80ActDmgS, no death
    // The final fight's Wesker (uEm81, user 2026-10-26: "Jill's two throws do not work on him"): uEm80's actions in the
    // same slots, then uEmA2's from 0x7B (table read live, 151 actions), his motions from pawn\em\em80\motion\em80 (bank
    // 0; 1 em80_pr, 2 em80_face, 3 em80_pl), 30000 health; so uEm80's DmgS and getups. Out of a neck break (user: "on his
    // knees for the injection") his Hiza 0x67 after the getup: its init (RVA 0x51B460) opens the injection prompt in the
    // fight's room 0x1FA (RVA 0x50DAF0, handler RVA 0x50C520), kneels (em80 0x72), stays on his knees (0x73, 110 frames)
    // as many loops as its +0xD0 (a table by his health %: 1 above 75 %), then gets up (0x75).
    { 0xB8C300, { 0x58, 0x59, 0x58 }, 0x58, -1, -1, "final Wesker", 0x2D, true, 0x0E, 0x67 },  // uEm81
    // Lost in Nightmares' Wesker (uEmA2, user 2026-10-26: "everything for the DLC's one"): uEm80's actions in the same
    // slots, his motions from pawn\em\ema2\motion\ema2, which keeps em80's slots for them (tools/lmt.py, 2026-10-26: 0x03,
    // 0x0E, 0x23, 0x2D, 0x64-0x77, 0xD6, 0xD7, 0xE7 the same frames, hips and heights).
    { 0xB8C0B0, { 0x58, 0x59, 0x58 }, 0x58, -1, -1, "Lost in Nightmares' Wesker", 0x2D, true, 0x0E },  // uEmA2
    // The Uroboros (user, 2026-10-30: "Jill's two throws on it as on the other bosses"): uEm40 (the first one) and uEm41
    // (its arms and cores separate units), uEm40's actions (uEm41's table read live, 0x00-0x2B uEm40's): cEm40ActThreat
    // 0x03 / 0x23 / 0x24 (init RVA 0x4AF1F0 plays its layer 0 motion 0x003 through vfunc +0x260, update RVA 0x4AB410
    // ends with it: +0x388 & 4, then +0x1BA0 = 1; 0x23 asks Walk at once on anything but a uEm41, 0x24 asks Run 0x25 at
    // its end), cEm40ActDead 0x0E-0x11 / 0x33 (forced_death.cpp). Its banks D and E free (uEm41: 0-7 em40 / em41 lists).
    { 0xB7BD08, { 0x03, 0x24, 0x03 }, 0x0E, 0x0E, 0x11, "Uroboros", 0 },       // uEm40
    { 0xB7D160, { 0x03, 0x23, 0x03 }, 0x33, 0x33, 0x33, "Uroboros boss", 0 },  // uEm41: its end Dead 0x33
};
constexpr int kKneelLoops = 3;  // the final Wesker on his knees after a neck break: 3 loops of 0x73 (about 11 s)
std::atomic<const PuppetClass*> g_puppetClass{ &kPuppetClasses[0] };
std::atomic<int> g_puppetThen{ -1 };       // the driven puppet's action after its getup (-1: its AI)
std::atomic<uintptr_t> g_kneeling{ 0 };    // the final Wesker asked to kneel after a neck break (its loops not set yet)
std::atomic<DWORD> g_kneelingTick{ 0 };

void WatchKneel(uintptr_t e, int action) {
    if (action != 0x67) return;
    const DWORD now = GetTickCount();
    g_kneelingTick = now ? now : 1;
    g_kneeling = e;
}

const PuppetClass* PuppetClassOf(uintptr_t unit) {
    uintptr_t vtable = 0;
    if (!unit || !mem::Read(unit, vtable)) return nullptr;
    for (const PuppetClass& c : kPuppetClasses)
        if (vtable == reinterpret_cast<uintptr_t>(mem::Rva(c.vtable))) return &c;
    return nullptr;
}

void* LoadList(const char* archive, const char* path) {
    void* mgr = nullptr;
    if ((archive && !LoadWeaponArchive(archive)) || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE34580)), mgr) || !mgr)
        return nullptr;
    using ResLoadFn = void*(__fastcall*)(void* mgr, void* edx, void* type, const char* path, unsigned flags);
    void* res = nullptr;
    __try {
        res = reinterpret_cast<ResLoadFn>(mem::Rva(0x48B70))(mgr, nullptr, mem::Rva(0xE4B574), path, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        res = nullptr;
    }
    uintptr_t vtable = 0;
    uint32_t state = 0;
    if (!res || !mem::Read(reinterpret_cast<uintptr_t>(res), vtable) || vtable != reinterpret_cast<uintptr_t>(mem::Rva(0xCB6114)) ||
        !mem::Read(reinterpret_cast<uintptr_t>(res) + 0x4C, state) || !(state & 1)) {
        LOG("wesker moves: motion list %s not available (%p, state %08X)", path, res, state);
        return nullptr;
    }
    LOG("wesker moves: motion list %s at %p", path, res);
    return res;
}

bool EnsureLists() {
    bool all = true;
    for (size_t k = 0; k < std::size(kLists); ++k) {
        if (!g_lists[k]) g_lists[k] = LoadList(kLists[k].archive, kLists[k].path);
        if (k < kRequiredLists) all = all && g_lists[k];
    }
    return all;
}

// A character's action list of kPlayerLists (the duo moves), loaded at its first use and kept.
void* PlayerListAt(int which) {
    if (which < 0 || which >= static_cast<int>(std::size(kPlayerLists))) return nullptr;
    if (!g_playerLists[which]) g_playerLists[which] = LoadList(kPlayerLists[which].archive, kPlayerLists[which].path);
    return g_playerLists[which];
}

bool BankLoaded(int bank) {
    for (size_t k = 0; k < std::size(kLists); ++k)
        if (kLists[k].bank == bank) return g_lists[k] != nullptr;
    return true;
}

// A motion list in a unit's bank (uModel::SetMotionList; players and enemies alike).
bool SetBank(uintptr_t unit, void* list, int bank) {
    using SetMotionListFn = void(__fastcall*)(void* model, void* edx, void* list, int slot);
    uintptr_t current = 0;
    if (mem::Read(unit + 0xA88 + 4 * bank, current) && current == reinterpret_cast<uintptr_t>(list)) return true;
    __try {
        reinterpret_cast<SetMotionListFn>(mem::Rva(0x1CCDD0))(reinterpret_cast<void*>(unit), nullptr, list, bank);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("wesker moves: bank %X of %08X not set", bank, static_cast<unsigned>(unit));
        return false;
    }
    return true;
}

bool InstallBanks(uintptr_t p) {
    for (size_t k = 0; k < std::size(kLists); ++k)
        if (g_lists[k] && !SetBank(p, g_lists[k], kLists[k].bank)) return false;
    return true;
}

bool RequestPlayer(uintptr_t p, uint32_t action) {
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(p, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return false;
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(p), nullptr, static_cast<int>(action), 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}
}  // namespace

// swaps.cpp (borrowed movesets): another character's motion list, kept referenced, and a unit's bank.
void* LoadMotionList(const char* archive, const char* path) { return LoadList(archive, path); }
bool SetMotionBank(uintptr_t unit, void* list, int bank) { return SetBank(unit, list, bank); }

// Plays a move on player 1 (the request and the motion are taken at his next update).
bool PlayWeskerMove(int bank, uint16_t index, float begin = 0.0f);

// The named moves, for the parries and the dodge (parry.cpp...): 0 Wesker's guard, 1 Chris's counter on him, 2 Jill's
// throw, 3.. the dodges (DmgNot motions 0x14, 0x13, 0x18, 0x19, 0x26, 0x17).
bool PlayNamedMove(int move) {
    static constexpr struct { int bank; uint16_t index; } kNamed[] = {
        { kWeskerBank, 0x74 }, { kWeskerPlayerBank, 0x01 }, { kJillBank, 0x01 },
        { kWeskerBank, 0x14 }, { kWeskerBank, 0x13 }, { kWeskerBank, 0x18 }, { kWeskerBank, 0x19 }, { kWeskerBank, 0x26 },
        { kWeskerBank, 0x17 },
        { kWeskerBank, 0xD7 },  // 9: his rocket catch (cEm80ActRokeUke 0x66, init RVA 0x51B000 plays em80 0xD7)
        { kWeskerPlayerBank, 0x65 },  // 10: Sheva's counter on him (the RE6 parry's)
    };
    if (move < 0 || move >= static_cast<int>(std::size(kNamed))) return false;
    return PlayWeskerMove(kNamed[move].bank, kNamed[move].index);
}

// Turns player 1 towards a point (his facing quaternion +0x40, P+0x1024 and the aim yaw P+0x2A10, as movement.cpp).
void FacePlayerTowards(uintptr_t p, const float* point) {
    float pos[3]{};
    if (!p || !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return;
    const float yaw = std::atan2(point[0] - pos[0], point[2] - pos[2]);
    mem::Write(p + 0x2A10, yaw);
    mem::Write(p + 0x1024, yaw);
    const float quat[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x40), quat, sizeof(quat));
}

// Whether player 1 is in a move's carrier action (the riposte of the parries, the dodge).
bool PlayerInWeskerMove(uintptr_t p) {
    uint32_t action = 0;
    return p && mem::Read(p + 0x10E0, action) && action == kCarrier;
}

// A move's motion id on player 1: its bank and slot, the pseudo banks resolved (em83act plays from bank 8, RE4's lists
// from the slots they were given); 0 when it cannot play.
uint32_t MoveMotion(int bank, uint16_t index) {
    if (bank == kRe6MeleeBank)  // index 0xFFFF: the test's choice
        return (static_cast<uint32_t>(kJill2ActBank) << 8) |
               (index == 0xFFFF ? static_cast<uint32_t>(g_re6TestIndex.load()) : static_cast<uint32_t>(index));
    if (bank >= kRe4LeonBank && bank <= kRe6EvadeBank) return ForeignMotion(bank - kRe4LeonBank, index);
    return (static_cast<uint32_t>(bank == kJill1ActBank || bank == kJoshActBank || PlayerListBank(bank) ? kJill2ActBank : bank)
            << 8) |
           index;
}

bool PlayWeskerMove(int bank, uint16_t index, float begin) {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p || !EnsureSetMotionHook() || !EnsureLists() || !InstallBanks(p)) return false;
    uint32_t motion = 0;
    if (bank >= kRe4LeonBank && bank <= kRe6EvadeBank) {
        motion = MoveMotion(bank, index);
        if (!motion) return false;
    } else if (bank == kJill1ActBank) {
        if (!g_jill1ActList) g_jill1ActList = LoadList("uEm83", "pawn\\em\\em83\\motion\\em83act");
        if (!g_jill1ActList || !SetBank(p, g_jill1ActList, kJill2ActBank)) return false;
        bank = kJill2ActBank;
    } else if (bank == kJoshActBank) {
        void* const josh = JoshActionList();
        if (!josh || !SetBank(p, josh, kJill2ActBank)) return false;
        bank = kJill2ActBank;
    } else if (PlayerListBank(bank)) {
        void* const list = PlayerListAt(bank - kPlayerListBank);
        if (!list || !SetBank(p, list, kJill2ActBank)) return false;
        if (!LendMovesetForTrainerMove(kPlayerLists[bank - kPlayerListBank].loanSet, kCarrier))
            LOG("wesker moves: %s's effects and sounds not lent", kPlayerLists[bank - kPlayerListBank].path);
        bank = kJill2ActBank;
    } else if (bank == kRe6MeleeBank) {
        uint32_t count = 0;
        void* const list = Re6MeleeList(count);
        const uint32_t which = index == 0xFFFF ? static_cast<uint32_t>(g_re6TestIndex.load()) : index;
        if (!list || which >= count || !SetBank(p, list, kJill2ActBank)) return false;
        motion = MoveMotion(kRe6MeleeBank, index);
    } else if (!BankLoaded(bank)) {
        LOG("wesker moves: the motion list of bank %X is not loaded", bank);
        return false;
    }
    g_pendingBegin = begin;
    g_pendingMotion = motion ? motion : (static_cast<uint32_t>(bank) << 8) | index;
    const DWORD now = GetTickCount();
    g_pendingSince = now ? now : 1;
    g_bossMoveStart = now ? now : 1;
    EnsurePlayerVoiceHook();
    if (!RequestPlayer(p, kCarrier)) {
        g_pendingMotion = 0;
        return false;
    }
    LOG("wesker moves: player 1 plays %X (carrier action %X)", g_pendingMotion.load(), kCarrier);
    return true;
}

// A Majini thrown by Jill's throw or the beta back throw lands on its back, head towards player 1 (opposite its facing),
// but its AI then picks the face-down states (RiseFaceDown 0x6B after BackHoldDmg, DmgAtFaceDown 0x83, DeadFaceDown
// 0x8E) whose motions want the head along its facing: the blend spun the body 180 degrees (user 2026-10-22; recorded:
// head direction flipped within 12 frames of the getup). While it lies there its face-down motions play their face-up
// twins (cEm10 bank 5, recorded pairs: getups 0x555 / 0x557 -> 0x554 / 0x556 of RiseFaceUp 0x6A, death 0x553 -> 0x552,
// hit on the ground 0x53D -> 0x53C), the state and its timing kept (RiseFaceUp itself stood it up at once: its init
// skips the wait for a Majini above half health, RVA 0x820244..).
bool FaceDownMotion(uint32_t m) { return m == 0x53D || m == 0x553 || m == 0x555 || m == 0x557; }
bool FaceUpMotion(uint32_t m) { return m == 0x53C || m == 0x552 || m == 0x554 || m == 0x556; }
uintptr_t g_faceUp[8] = {};  // game thread: Majini lying on their back after a throw
int g_faceUpNext = 0;

uint32_t FaceUpTwin(uintptr_t unit, uint32_t motion) {
    int slot = -1;
    for (int i = 0; i < 8; ++i)
        if (g_faceUp[i] == unit) slot = i;
    if (slot < 0) {
        if (!FaceDownMotion(motion)) return motion;
        uint32_t current = 0;
        mem::Read(unit + 0x384, current);
        current &= 0xFFFF;
        const uint32_t pair = g_pairMotion.load(std::memory_order_relaxed) & 0xFFFF;
        const bool thrown = current == kJillThrownMotion || current == kJill2VictimMotion ||
                            (BetaBackThrowOn() && (current == 0x604 || current == 0x640)) ||
                            (pair && current == pair && !LyingFaceDown(unit));  // an RE4 pair's side: as it lies
        if (!thrown) return motion;
        slot = g_faceUpNext;
        g_faceUpNext = (g_faceUpNext + 1) % 8;
        g_faceUp[slot] = unit;
    }
    if (FaceDownMotion(motion)) {
        LOG("wesker moves: Majini %08X on its back: motion %X -> %X", static_cast<unsigned>(unit), motion, motion - 1);
        return motion - 1;
    }
    if (!FaceUpMotion(motion)) {  // up again (or blown off): its own motions
        uint32_t action = 0;
        mem::Read(unit + 0x10E0, action);
        LOG("wesker moves: Majini %08X no longer on its back: motion %X (action %X)", static_cast<unsigned>(unit), motion,
            action);
        g_faceUp[slot] = 0;
    }
    return motion;
}

// Actor SetMotion hook (swaps.cpp): player 1's first layer 0 motion after a move's request becomes the move's.
uint32_t WeskerMoveMotion(uintptr_t unit, uint32_t layer, uint32_t motion) {
    if (layer == 0 && unit && unit == g_straightBlowUnit.load(std::memory_order_relaxed)) {
        if (static_cast<int32_t>(GetTickCount() - g_straightBlowTick.load()) > 3000) {
            g_straightBlowUnit = 0;
        } else {
            const uint32_t m = motion & 0xFFFF;
            const uint32_t straight = m == 0x528 || m == 0x52C || m == 0x52E || m == 0x534   ? 0x52Au
                                      : m == 0x529 || m == 0x52D || m == 0x52F || m == 0x535 ? 0x52Bu
                                                                                           : 0u;
            if (straight) {
                LOG("wesker moves: enemy %08X blow-off motion %X -> %X (straight)", static_cast<unsigned>(unit), motion, straight);
                return (motion & ~0xFFFFu) | straight;
            }
        }
    }
    if (layer == 0 && unit && unit == g_forcedMotionUnit.load(std::memory_order_relaxed)) {
        g_forcedMotionUnit = 0;
        if (static_cast<int32_t>(GetTickCount() - g_forcedMotionTick.load()) < 500) {
            LOG("wesker moves: enemy %08X motion %X -> %X (forced)", static_cast<unsigned>(unit), motion, g_forcedMotion.load());
            return g_forcedMotion.load();
        }
    }
    if (layer == 0 && unit && unit == g_puppet.load(std::memory_order_relaxed)) {
        const uint32_t puppet = g_puppetMotion.exchange(0);
        if (puppet) {
            LOG("wesker moves: %s %08X motion %X -> %X (phase %d)", g_puppetClass.load()->name, static_cast<unsigned>(unit),
                motion, puppet, g_puppetPhase.load());
            if (g_puppetPhase.load() == 4) {  // its death plays lying: its own again
                g_puppet = 0;
                g_puppetPhase = 0;
            }
            return puppet;
        }
    }
    if (layer == 0 && unit) motion = FaceUpTwin(unit, motion);
    if (layer == 0 && unit && g_groundRoll.load(std::memory_order_relaxed) && unit == actors::Player(0)) {
        uint32_t action = 0;
        uintptr_t list = 0;
        const bool asked = static_cast<int32_t>(GetTickCount() - g_groundRollAsked.load()) < 500;
        if (mem::Read(unit + 0x10E0, action) && action == 0x97 && (asked || Player1RunHeld()) && StaminaAllows() && g_ema2PlayerList &&
            mem::Read(unit + 0xA88 + 4 * kPlayerRollBank, list) && list == reinterpret_cast<uintptr_t>(g_ema2PlayerList)) {
            g_groundRollAsked = 0;
            LOG("wesker moves: player 1 getup 97: motion %X -> %X (the DLC back roll)", motion, kGroundRollMotion);
            return kGroundRollMotion;
        }
    }
    if (layer == 0 && unit && g_acrobaticGetup.load(std::memory_order_relaxed) && unit == actors::Player(0)) {
        uint32_t action = 0, list = 0;
        if (mem::Read(unit + 0x10E0, action) && (action == 0x97 || action == 0x98) &&
            mem::Read(unit + 0xA88 + 4 * kPlayerJillDamageBank, list) && list == reinterpret_cast<uintptr_t>(g_jillDamageList) &&
            list) {
            const uint32_t jill = (static_cast<uint32_t>(kPlayerJillDamageBank) << 8) | (action == 0x97 ? 0x06 : 0x07);
            LOG("wesker moves: player 1 getup %X: motion %X -> %X (Jill)", action, motion, jill);
            return jill;
        }
    }
    if (layer == 0 && unit && unit == g_thrownUnit.load(std::memory_order_relaxed) && (motion == 0x604 || motion == 0x640)) {
        if (static_cast<int32_t>(GetTickCount() - g_thrownSince.load(std::memory_order_relaxed)) < 1000) {
            const uint32_t thrown = g_thrownMotion.load(std::memory_order_relaxed);
            LOG("wesker moves: Majini %08X motion %X -> %X (Jill thrown)", static_cast<unsigned>(unit), motion, thrown);
            return thrown;
        }
    }
    const uint32_t pending = g_pendingMotion.load(std::memory_order_relaxed);
    if (!pending || layer != 0 || unit != actors::Player(0)) return motion;
    const DWORD since = g_pendingSince.load(std::memory_order_relaxed);
    g_pendingMotion = 0;
    // Signed: "| 1" stamps once gave -1 = 49 days within the same tick (every other move played the carrier's own
    // emotion: speech and a turn, user 2026-10-22; parry.cpp Stamp).
    if (static_cast<int32_t>(GetTickCount() - since) > 500) return motion;
    uint32_t action = 0;
    mem::Read(unit + 0x10E0, action);
    LOG("wesker moves: layer 0 motion %X -> %X (action %X)", motion, pending, action);
    g_beginNow = g_pendingBegin.load(std::memory_order_relaxed);
    return pending;
}

// ---- Attacks (user, 2026-10-22: on Chris the motions hit nobody and leave him open, while Wesker's own actions hurt
// and make him invincible: the motion carries neither). The trainer adds both: while a move's motion plays on player
// 1's layer 0 (P+0x384, frame +0x398, length +0x39C) every enemy within reach in front is hit once during the move's
// window (damage on its health +0x1364, the kill credited to player 1's pad +0x136C as OnHit does, then the move's
// reaction: a Majini blown off as by a shotgun; Jill DmgBlowL 0x19, or DmgKnockDown 0x1B for the sweep; Wesker DmgS
// 0x58; others none), and player 1's hits are refused for the whole move (reaction, damage and blood:
// WeskerMoveInvulnerable, asked by player.cpp and quick_knife.cpp).
// Jill's throw (user: free melee and the melee swap; no fallback for now, to see what it does on each enemy first):
// the nearest enemy within 3 m is put 70 cm in front of player 1, its back to him, while he plays the throw; a Majini
// goes in BackHoldDmg 0x9C (motion 0x604: the beta "thrown" motion with melee.beta_back_throw on, else the game's elbow
// reaction; the action ends lying, 0x6B); its damage comes near the end, with the knock down of Jill / Wesker's
// reaction. Without an enemy near, the throw plays on the spot.
// On Jill it is the game's own throw: her Caught 0x27 (cEm83ActCaught, init RVA 0x536EF0) requested with player 1 as
// the other unit (the enemy RequestAction RVA 0x759030 stores its 3rd argument in the action +0xC; Caught checks it is
// a uPlayer) puts player 1 on her (her position + an offset along her facing, his yaw hers + a constant), plays her
// motion 0x23 and starts his event cEm83PlEvent_Catch (bank F = em83evt, motion 0xF01), synchronised; she ends lying.
// She is first turned towards player 1 so that he does not jump around her.
// On a Majini (user: "the native animations", not the beta one): Jill's own thrown motion em83dmg 0x23, made to play
// with player 1's 0xE01 the way Caught does it: the Majini gets em83dmg in its free bank E, is put on player 1's origin
// turned to his yaw + pi (Caught: both at her position + 100 cm along her facing, him at her yaw + pi; her motion keeps
// her body 102 cm behind that origin), its collision off (RVA 0x44D2A0(0), as Caught's init; its exit: 0x44D310 /
// 0x44D2A0(1) / 0x44D2D0, done here when the Majini leaves BackHoldDmg) and its cObjAdj contacts dropped (player.cpp:
// it pushed him away), its motion speed player 1's, and requested BackHoldDmg 0x9C, whose motion 0x604 / 0x640 the
// SetMotion hook turns into 0xE23. A lethal throw leaves it 1 health until it lies, then DeadFaceUp (user-verified).
// (Chris's counter on Wesker is the RE6 parry's riposte, not a free melee move - user.)
}  // namespace features

namespace features {
bool MajiniBlowOff(uintptr_t enemy, uintptr_t player);  // parry.cpp: blown off (dead at 0 health when it lands)
bool MajiniBlowOffFrom(uintptr_t enemy, const float* from, uint32_t part);  // parry.cpp: the same from a point
bool MajiniDieNow(uintptr_t enemy);                      // parry.cpp
bool ForceMajiniState(uintptr_t enemy, int state);       // parry.cpp: 7 = Recul + Leg
bool MajiniFlinch(uintptr_t enemy);                      // parry.cpp: a knife-like flinch on the head, no prompt
bool EnemyOnGround(uintptr_t enemy);                     // knife.cpp: falls, hit on the ground (not getting up)
void AllowBigBlowOff(uintptr_t enemy);                   // parry.cpp: the next blow-off of a big Majini goes through
bool IsMajiniEnemy(uintptr_t enemy);                     // parry.cpp
bool FlyingEnemy(uintptr_t enemy);                       // forced_death.cpp
const char* UnitClassName(uintptr_t object);             // parry.cpp: DTI class name
bool EnsureCommonEffectsHook();                          // quick_knife.cpp: the blood hook
bool EnemyIsDown(uintptr_t enemy);                       // knife.cpp: a Majini down, kneeling or falling
bool EnsurePlayerContactHook();                          // player.cpp: cObjAdj contacts (the thrown Majini dropped)
void TickJillVictim();                                   // below: Jill in Chris's side of her second throw
bool EnsureEnemyActionHook();                            // action_log.cpp: enemy requests (the thrown Executioner)
bool EnsureGrabRefusalHook();                            // grabs.cpp: no grab on player 1 during a move
void CounterPunchLands(uintptr_t enemy);                 // parry.cpp: the RE6 parry's damage and state
bool StartCombatMove(int k, uintptr_t forced = 0, bool chained = false);  // below: `forced` = its target (the throw parry)
float DodgeCostOf(const char* id);  // below: a dodge's stamina slider ("Endurance stricte"), -1 = the refill held only
bool ReactToMeleeHit(uintptr_t p, uintptr_t enemy, int reaction, bool ownDeath);  // below: the free melee table's

bool GuardRiposteActive(uintptr_t player);              // below: the parry's guard

bool MajiniHoldsStealable(uintptr_t target);  // below: a Majini holding a weapon to strike with
bool BigEnemyClass(uintptr_t enemy);          // knife.cpp: Executioners, chainsaw, Gatling, fat man, giant
// Below: the environmental execution an obstacle near `enemy` allows (its plan g_envPlan), else -1.
int EnvExecutionFor(uintptr_t p, uintptr_t enemy, int only = -1, float tolerance = -1.0f, bool quiet = false);
int EnvHeightFor(uintptr_t p, float tolerance, int only, uintptr_t onlyEnemy, bool quiet);  // below: the same from a height
int EnvCachedWall(uintptr_t enemy);  // below: the game thread's plan against an obstacle for that staggered enemy, -1 none
bool EnsurePadQueryHooks();  // quick_knife.cpp: its pad query hook, the game thread's tick

namespace {
// The knife drawn by the parry's press stayed equipped through the guard and had to be put away after it (user,
// 2026-10-23: "he must find his weapon right away"). The knife aim equips the knife slot (cItem +0x18 grid 9) with
// cItemManager::Equip (RVA 0x84D480, thiscall(grid) ret 4: the new item's +0x1C = 1, the previous weapon's = 2); while
// the guard plays, the previous weapon is equipped again the same way and given its motions, without the weapon change
// action (the player update draws the item whose +0x1C is 1 when it changes, MODLOG session 8).
void GunBackFromKnife(uintptr_t p) {
    int knife = -1;
    uint32_t previous = 0xFF;
    for (int k = 0; k < 24; ++k) {
        const uintptr_t record = p + 0x21A4 + 0x30 * k;
        uint32_t id = 0, grid = 0, state = 0;
        if (!mem::Read(record + 4, id) || !(id & 0xFFFF) || !mem::Read(record + 0x18, grid) || !mem::Read(record + 0x1C, state))
            continue;
        if (state == 1 && grid == 9) knife = k;
        if (state == 2 && grid <= 8) previous = grid;
    }
    if (knife < 0 || previous == 0xFF) return;
    // Its weapon motions too (bank 5 kept the knife's: user, no weapon while walking and the knife guard when aiming):
    // Player::SetWeaponMotion (RVA 0x75A7A0, thiscall(record key, slot) ret 8) installs the list of the item in hand
    // (key 0xE) in slot 5, as the weapon change does (quick_knife.cpp).
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, uint32_t)>(mem::Rva(0x84D480))(reinterpret_cast<void*>(p + 0x21A0),
                                                                                        nullptr, previous);
        reinterpret_cast<void(__fastcall*)(void*, void*, int, int)>(mem::Rva(0x75A7A0))(reinterpret_cast<void*>(p), nullptr,
                                                                                        0xE, 5);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    LOG("wesker moves: guard riposte: knife put away, weapon of grid %u equipped again with its motions", previous);
}

// RE6's knife pairs (the knife counter, the throat cut): the knife slot equipped for the move as the parry draws it (cItemManager::Equip,
// grid 9: the player update draws the item whose +0x1C becomes 1), the previous weapon given back at its end (GunBackFromKnife). None
// without a knife in the slot.
// The knife held reversed (user, 2026-11-10: "Chris holds it the right way up but he must have it reversed for this move and for the
// knife Leg Back"): the knife Ground and the throat cut.
bool ReverseGripMove(const char* id) {
    return std::strcmp(id, "melee.free_re6_ground_attack_knife") == 0 || std::strcmp(id, "melee.free_re6_common_throat_cut") == 0;
}

// RE6's weapon joint (bone 9, the right hand's weapon holder) by frame of the RE6 moves that move it (tools/export_re6_weapon_joint.py,
// DataDir\motions\re6_weapon_joint.txt; user, 2026-11-10: the two-handed second blow, "Chris slides the rifle to strike with the stock:
// the second hand is in the void", and "the two-handed Ground has the same problem"): n° 3 (AcA 06: 63 degrees, 12 cm), n° 13 (AcA 2E:
// 45 degrees, 38 cm); n° 14 / 29 too (the knife's reverse grip, AcA 30 / 5C) but those keep the user's own grip (ReverseGripMove).
// Read once, a key (quaternion, position) per frame.
struct WeaponJointFrames {
    int index = -1;
    std::vector<std::array<float, 7>> frames;
};
std::vector<WeaponJointFrames> g_weaponJoints;
std::atomic<bool> g_weaponJointsLoaded{ false };

void LoadWeaponJoints() {
    if (g_weaponJointsLoaded.load()) return;
    std::ifstream f(logx::DataDir() + L"\\motions\\re6_weapon_joint.txt");
    std::string line;
    while (std::getline(f, line)) {
        int index = 0, frames = 0;
        std::array<float, 7> v{};
        if (sscanf_s(line.c_str(), "move %d %d", &index, &frames) == 2) {
            g_weaponJoints.push_back({ index, {} });
        } else if (!g_weaponJoints.empty() &&
                   sscanf_s(line.c_str(), "%f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7) {
            g_weaponJoints.back().frames.push_back(v);
        }
    }
    LOG("wesker moves: RE6's weapon joint for %u move(s)%s", static_cast<unsigned>(g_weaponJoints.size()),
        g_weaponJoints.empty() ? " (no DataDir\\motions\\re6_weapon_joint.txt: tools/export_re6_weapon_joint.py)" : "");
    g_weaponJointsLoaded = true;
}

const WeaponJointFrames* WeaponJointFor(int index) {
    if (!g_weaponJointsLoaded.load()) return nullptr;
    for (const WeaponJointFrames& w : g_weaponJoints)
        if (w.index == index && !w.frames.empty()) return &w;
    return nullptr;
}

bool Re6KnifePair(const char* id) {
    return std::strcmp(id, "melee.free_re6_common_knife_counter") == 0 || std::strcmp(id, "melee.free_re6_common_throat_cut") == 0;
}

// Moves played with player 1's weapon hidden (user, 2026-11-10, the machete disarm: "not Chris's knife: hide all of Chris's weapons, he kills
// the enemy with its own weapon"): the weapon in hand's drawn bit (item +5, 0x08) cleared every frame of the move, given back at its end
// if it was drawn (as the cranks and the rocket catch do); player.cpp's "Arme toujours visible" stands aside (TrainerMoveHidesWeapon).
// The armed-zombie counters and the weapon steal too (user, 2026-11-10: "hide the weapon in Chris's hand and attach the enemy's to it").
bool HidesWeaponMove(const char* id) {
    return std::strcmp(id, "melee.free_re6_common_machete_counter") == 0 || std::strcmp(id, "melee.free_re6_common_club_counter_1") == 0 ||
           std::strcmp(id, "melee.free_re6_common_club_counter_2") == 0 || std::strcmp(id, "melee.free_re6_common_weapon_steal") == 0;
}
std::atomic<bool> g_moveHidesWeapon{ false };
uintptr_t g_moveHiddenWeapon = 0;
bool g_moveWeaponWasDrawn = false;

void HideMoveWeapon(uintptr_t p) {
    uintptr_t weapon = 0;
    uint8_t flags = 0;
    if (!p || !mem::Read(p + 0x2634, weapon) || !weapon || !mem::Read(weapon + 5, flags)) return;
    if (!g_moveHidesWeapon.load() || weapon != g_moveHiddenWeapon) {
        g_moveWeaponWasDrawn = (flags & 0x08) != 0;
        g_moveHiddenWeapon = weapon;
        g_moveHidesWeapon = true;
    }
    if (flags & 0x08) mem::Write<uint8_t>(weapon + 5, static_cast<uint8_t>(flags & ~0x08));
}

void ShowMoveWeapon(uintptr_t p) {
    if (!g_moveHidesWeapon.exchange(false)) return;
    uintptr_t weapon = 0;
    uint8_t flags = 0;
    if (g_moveWeaponWasDrawn && p && mem::Read(p + 0x2634, weapon) && weapon == g_moveHiddenWeapon && mem::Read(weapon + 5, flags) &&
        !(flags & 0x08))
        mem::Write<uint8_t>(weapon + 5, static_cast<uint8_t>(flags | 0x08));
    g_moveHiddenWeapon = 0;
}

// RE6's weapon steals (user, 2026-11-10: "Chris steals its weapon and hits it with it: hide Chris's weapon in hand and attach the enemy's
// to it"): the pair's Majini's weapon (+0x1B6C) drawn in player 1's hand from the frame RE6 hands it over (recorded in RE6: its parent
// the player's from frame 135 of the second club counter, 85 of the weapon steal) to the move's end, when it is the Majini's again
// (RE6 drops it there). player.cpp computes its world matrix from his joint of the hand that held it (8 right, 13 left: the pair is
// mirrored for a left-handed Majini), its parent left to the Majini.
uintptr_t g_stolenFrom = 0;  // the Majini whose weapon player 1 holds (render thread)
void TickStolenWeapon(uintptr_t p);  // below (g_active)
float StealFrame(const char* id) {
    // The first counter too (user, 2026-11-11: "he takes the axe when he strikes, a little after the elbow"; RE6 kept it the zombie's
    // there): his elbow at ~110, his right arm's fast swing 126-128 (the blow, re6_melee 172's arms): taken at 118, then "it must attach
    // 1 s earlier" (user, 2026-11-11): 58.
    if (std::strcmp(id, "melee.free_re6_common_club_counter_1") == 0) return 58.0f;
    // The player's right hand grips the zombie's weapon before RE6 hands it over (its IK flags, research/re6_special_counters: bone 250's
    // second flag 16 and bone 253's weight - 19-69 in the first counter, 90-139 in the second, 17-85 in the steal, the hand-over where the
    // grip ends): taken when the hand grips it (user, 2026-11-11: "on the steal too it attaches too late"). The second: 90, then "too
    // late on the second (the one without the elbow)": its left hand takes it first (bone 250's first flag 13 from 28; measured on re6_melee
    // 174 / 175, the left hand 5-11 cm from the zombie's weapon hand on 39-48, then the zombie pushed away with the weapon left to him),
    // the right one at 90 (StealRightFrame): taken at 42.
    if (std::strcmp(id, "melee.free_re6_common_club_counter_2") == 0) return 42.0f;
    // RE6 hands it over at ~85, just after the blow at 80 - which kills the Majini here, and a dead Majini lets its weapon go (user,
    // 2026-11-10: "on the steal the weapon stays in the enemy's hand"; logged: never taken): taken at 78, before the blow; then at the
    // hand's grip, 18.
    if (std::strcmp(id, "melee.free_re6_common_weapon_steal") == 0) return 18.0f;
    return -1.0f;
}

// The first armed-zombie counter's elbow in the zombie's head (re6_melee 172 / 173: his right elbow fastest at 51-52, 10-14 cm from its head
// on 54-56): its blow's sound alone at 54 (no move: never).
float SoundOnlyBlowFrame(const char* id) {
    return std::strcmp(id, "melee.free_re6_common_club_counter_1") == 0 ? 54.0f : 1.0e9f;
}

// The frame from which the stolen weapon is in his right hand (before it, from StealFrame, in his left): the second counter's 90.
// The first counter's too (user, 2026-11-11: "he takes it straight in the right hand, but really he takes it in the left and passes it
// to the right"; re6_melee 172 / 173: his left hand on the zombie's weapon hand 30-69, away with it from 72, his hands together 93-96,
// then the right arm's swing 126-128): 94.
// The second counter straight in the right hand at 42 (user, 2026-11-11: "not in the second: it is the first that starts with the left
// then the right").
float StealRightFrame(const char* id) {
    return std::strcmp(id, "melee.free_re6_common_club_counter_1") == 0 ? 94.0f : 0.0f;
}
uint32_t g_stolenBaseJoint = 8;  // the hand the Majini held it with (13: a left-handed Majini, the pair mirrored)
uint32_t g_stolenHand = 0;       // the hand set now

// The Majini's own weapon release (VA 0x85DFB0, thiscall, no argument, ret: its weapon's per-class release - the weapon's vtable +0x1D8
// with the Majini - then its +0x1B6C / +0x1F50 cleared and +0x18DC |= 2; about 20 callers, the arm shot among them).
bool CallMajiniWeaponRelease(uintptr_t majini) {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*)>(mem::Rva(0x45DFB0))(reinterpret_cast<void*>(majini), nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// At the move's end RE6 drops it (user, 2026-11-11: "at the end of the animation the weapon goes back into the enemy's hand; it should
// stay detached so Chris lets it fall to the ground, as when I shoot an enemy's hand and it drops its weapon"; "it works natively for the
// one-handed weapons - clubs, bottles, wrenches, axes - not for the two-handed ones like the long sticks: drop those by hand"). The
// Majini's release (VA 0x85DFB0) clears its pointers, then the weapon's own drop - its action 4 cItAction_it_act_drop (every weapon's
// table: 0 wait, 1 throw, 2 get, 3 put, 4 drop; asked through its vtable +0xDC, VA 0xB59030; a shot's drop detaches it through VA
// 0x489860 SetParent(0, -1) - traced on a bottle) - while it is still drawn in player 1's hand: kept there until the game detached it
// (TickStolenDrop), so it falls from his hand; not detached within 0.5 s, the trainer makes its fall (player.cpp DropStolenWeapon).
bool RequestEnemy(uintptr_t enemy, uint32_t action, uintptr_t other);  // below
struct StolenDrop {
    uintptr_t weapon = 0, majini = 0;
    ULONGLONG since = 0;
    float feet = 0.0f;
};
StolenDrop g_stolenDrop;  // render thread

void EndStolenWeapon() {
    if (!g_stolenFrom) return;
    bool alive = false;
    for (uintptr_t u : actors::Enemies()) alive = alive || u == g_stolenFrom;
    uintptr_t weapon = 0;
    const bool held = alive && mem::Read(g_stolenFrom + 0x1B6C, weapon) && weapon;
    const bool released = held && CallMajiniWeaponRelease(g_stolenFrom);
    const bool asked = released && RequestEnemy(weapon, 4, 0);
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    float feet = 0.0f;
    if (released && p) mem::Read(p + 0x34, feet);
    LOG("wesker moves: the weapon of %08X %s", static_cast<unsigned>(g_stolenFrom),
        !held ? "no longer held" : !released ? "back to it (its release failed)"
                                 : asked ? "released, its drop asked" : "released, its drop refused");
    if (released) {
        g_stolenDrop = { weapon, g_stolenFrom, GetTickCount64(), feet };  // still drawn in his hand until detached
    } else {
        SetStolenWeapon(0, 0, 0);
    }
    g_stolenHand = 0;
    g_stolenFrom = 0;
}

void TickStolenDrop() {
    StolenDrop& d = g_stolenDrop;
    if (!d.weapon) return;
    bool alive = false;
    for (uintptr_t u : actors::Enemies()) alive = alive || u == d.majini;
    uintptr_t parent = 0;
    if (!alive || !mem::Read(d.weapon + 0x18, parent) || parent != d.majini) {
        if (!g_stolenFrom) SetStolenWeapon(0, 0, 0);
        LOG("wesker moves: the stolen weapon %08X %s", static_cast<unsigned>(d.weapon), alive ? "dropped by the game" : "gone with its Majini");
        d = {};
        return;
    }
    if (GetTickCount64() - d.since < 500) return;
    DropStolenWeapon(d.weapon, d.majini, d.feet);
    if (!g_stolenFrom) SetStolenWeapon(0, 0, 0);
    LOG("wesker moves: the stolen weapon %08X not dropped by the game: its fall made by the trainer", static_cast<unsigned>(d.weapon));
    d = {};
}

bool g_knifeDrawnForMove = false;  // KnifeIntoHand drew it: the weapon in hand before given back at the move's end
uint32_t g_knifePreviousGrid = 0xFF;

bool EquipPlayerGrid(uintptr_t p, uint32_t grid) {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, uint32_t)>(mem::Rva(0x84D480))(reinterpret_cast<void*>(p + 0x21A0), nullptr, grid);
        reinterpret_cast<void(__fastcall*)(void*, void*, int, int)>(mem::Rva(0x75A7A0))(reinterpret_cast<void*>(p), nullptr, 0xE, 5);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// A knife in hand for the move (user, 2026-11-10: "you drew what was in the sheath instead of the knife I had in hand: he cut the
// throat with a pistol; I can have knives in the inventory"): a knife in hand already is kept (the sheath's or the inventory's); else
// the sheath's (grid 9) if it holds a knife, else one of the grid's (0-8) - never another weapon. The weapon in hand before noted.
bool KnifeIntoHand(uintptr_t p) {
    g_knifeDrawnForMove = false;
    if (IsKnifeItem(PlayerHeldItem(p))) return true;
    int sheath = -1, inventory = -1;
    g_knifePreviousGrid = 0xFF;
    for (int k = 0; k < 24; ++k) {
        const uintptr_t record = p + 0x21A4 + 0x30 * k;
        uint32_t id = 0, grid = 0, state = 0;
        uint8_t inUse = 0;
        if (!mem::Read(record + 0x29, inUse) || !inUse || !mem::Read(record + 4, id) || !(id & 0xFFFF) || !mem::Read(record + 0x18, grid) ||
            !mem::Read(record + 0x1C, state))
            continue;
        if (state == 1) g_knifePreviousGrid = grid;
        if (!IsKnifeItem(static_cast<uint16_t>(id & 0xFFFF))) continue;
        if (grid == 9) sheath = 9;
        else if (grid <= 8 && inventory < 0) inventory = static_cast<int>(grid);
    }
    const int grid = sheath >= 0 ? sheath : inventory;
    if (grid < 0 || !EquipPlayerGrid(p, static_cast<uint32_t>(grid))) return false;
    g_knifeDrawnForMove = true;
    LOG("wesker moves: the knife of cell %d in hand for RE6's knife move (the weapon of cell %u before)", grid, g_knifePreviousGrid);
    return true;
}

// The guard (em80 0x74, 123 frames, 2 s) froze Chris far too long (user, 2026-10-23). Its layer speed P+0x3A4 is set
// back to 1 by the game every frame (recorded: 2.0 written at its start, 1.0 the next frame), so player 1's unit time
// scale +0xFD4 (1.0 from the uWork constructor, only read afterwards; enemies.cpp, licker.cpp) is doubled while the
// guard plays, then given back.
std::atomic<float> g_guardSpeed{ 2.0f };  // "Vitesse de la garde de Wesker (parade)" (x2 without the option)
uintptr_t g_guardSped = 0;  // render thread
float g_guardSavedScale = 1.0f;

void SpeedGuard(uintptr_t p, bool on) {
    if (!on) {
        if (g_guardSped && g_guardSped == p) mem::Write(g_guardSped + 0xFD4, g_guardSavedScale);
        g_guardSped = 0;
        return;
    }
    const float factor = g_guardSpeed.load(std::memory_order_relaxed);
    if (g_guardSped == p) {
        mem::Write(p + 0xFD4, g_guardSavedScale * factor);  // the slider may move meanwhile
        return;
    }
    float scale = 1.0f;
    if (!mem::Read(p + 0xFD4, scale) || scale < 0.5f || scale > 2.0f) return;  // not the plain 1.0 expected
    g_guardSped = p;
    g_guardSavedScale = scale;
    mem::Write(p + 0xFD4, scale * factor);
    LOG("wesker moves: guard played x%.1f (time scale %.2f)", factor, scale * factor);
}

// Chris's counter on Wesker steps him back about 1.2 m (made for Wesker's synchronised reaction; recorded 2026-10-23:
// with the Majini's own recoil they ended 4.7 m apart). The user kept the parried state at once and wanted Chris to stay
// put: his ground position is held where the counter started while it plays (height left to the game).
uintptr_t g_counterHeld = 0;  // render thread
// The RE6 parry's counters (user, 2026-11-01: "Chris's counter, and Sheva's: she counters Wesker in the fight too"). The
// players' events of the Wesker fights, em80_pl, hold Chris's motions at 0x00-0x19 and Sheva's at 0x64-0x7D in the same
// order (her own motions, most of the same lengths): his counter 0x01 (176 frames), hers 0x65 (121 frames). Their blow:
// his punch at frame 66 (measured from his hands' reach, 2026-10-23: the right hand thrown forward 80 cm in frames 60-68,
// at head height). The blow lands at the striking limb's contact (TickCounterBlow, user 2026-11-01: "the enemy reacts
// before the contact"; any limb from the swing hit at once: his left forearm at frame 56, her right forearm at 50, the
// target held close). Measured offline on Chris's skeleton (scratchpad counter_limbs.py, em80_pl): his right wrist
// thrown forward from 60 to 68 at 140 cm (fastest 64-66); hers sweeps the right leg low at 52-54, then her left leg
// kicks high, the ankle 125-155 cm up and 93 cm ahead at frames 72-76 (her blow; the sound event 70 at 73). `punch`:
// the slide's end and the blow on an enemy without hurt shapes.
struct CounterMove {
    uint16_t index;   // em80_pl
    float punch;      // the blow's frame
    float from, to;   // the blow's window: the striking limb's contact with the target
    uint32_t parts;   // that limb (kLimbParts below: 0 / 1 right forearm / fist, 6 / 7 left shin / foot)
};
constexpr CounterMove kCounters[] = { { 0x01, 66.0f, 60.0f, 76.0f, 0x03 }, { 0x65, 74.0f, 68.0f, 86.0f, 0xC0 } };
std::atomic<int> g_counterVariant{ 0 };  // the counter playing (kCounters)
// Their lines in the free melee tables (Chris's and Sheva's tabs: "Contre RE6").
const char* const kCounterIds[2] = { "melee.free_chris_counter", "melee.free_sheva_counter" };
uint32_t CounterMotion(int variant) {
    return (static_cast<uint32_t>(kWeskerPlayerBank) << 8) | kCounters[variant < 0 || variant > 1 ? 0 : variant].index;
}
float CounterPunchFrame() { return kCounters[g_counterVariant.load(std::memory_order_relaxed) == 1 ? 1 : 0].punch; }

// The motion carries his body away from his origin (hip joint 1.2 m off it 5 frames in), back over the motion, and
// from frame ~155 the origin's own root motion catches up with the body (recorded: hip 67 cm off at 155, 8 at 170).
// Holding the origin left that step back in (user, 2026-10-23: "Chris still steps back, his punch never reaches the
// Majini") and made him slide back to it at the end: his hip (joint 0) is held instead, over the whole counter, from the
// spot he stood on, slid to the punch's reach of its target until the punch (the target followed), then left there.
// 70 cm: the fist went through it (user, 2026-10-23: 5 cm more, "almost right").
constexpr float kCounterReach = 75.0f;        // cm from his hip to the target's origin at the punch
constexpr float kCounterSlideFrames = 45.0f;  // the slide over, before the punch is thrown (frames 60-68)
constexpr float kCounterMaxSlide = 250.0f;
float g_counterStart[2] = {}, g_counterHold[2] = {}, g_counterDir[2] = {};
bool g_counterSlide = false;

bool ReadHip(uintptr_t p, float (&hip)[3]) {
    uintptr_t joints = 0;
    return mem::Read(p + 0x318, joints) && joints && mem::SafeRead(reinterpret_cast<void*>(joints + 0x80), hip, sizeof(hip));
}

void HoldCounterSpot(uintptr_t p, bool on, float frame, uintptr_t target) {
    if (!on) {
        g_counterHeld = 0;
        return;
    }
    float pos[3]{}, hip[3]{}, at[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos)) || !ReadHip(p, hip)) return;
    const bool targetAt = target && mem::SafeRead(reinterpret_cast<void*>(target + 0x30), at, sizeof(at));
    if (g_counterHeld != p) {
        // His hip stood over his origin before the counter (the motion's offset may already be in this frame's pose).
        g_counterHeld = p;
        g_counterStart[0] = g_counterHold[0] = pos[0];
        g_counterStart[1] = g_counterHold[1] = pos[2];
        const float dx = at[0] - pos[0], dz = at[2] - pos[2], length = std::sqrt(dx * dx + dz * dz);
        g_counterSlide = targetAt && length > 1.0f && length < kCounterReach + kCounterMaxSlide;
        if (g_counterSlide) g_counterDir[0] = dx / length, g_counterDir[1] = dz / length;
        LOG("wesker moves: counter from %.0f cm of its target, slid to %.0f", targetAt ? length : -1.0f,
            g_counterSlide ? kCounterReach : -1.0f);
    }
    if (g_counterSlide && targetAt && frame <= CounterPunchFrame()) {
        const float goal[2] = { at[0] - g_counterDir[0] * kCounterReach, at[2] - g_counterDir[1] * kCounterReach };
        float s = frame / kCounterSlideFrames;
        s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
        s = s * s * (3.0f - 2.0f * s);
        g_counterHold[0] = g_counterStart[0] + (goal[0] - g_counterStart[0]) * s;
        g_counterHold[1] = g_counterStart[1] + (goal[1] - g_counterStart[1]) * s;
    }
    pos[0] += g_counterHold[0] - hip[0];
    pos[2] += g_counterHold[1] - hip[2];
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos));
}

// The punch's reach, logged when it lands (kCounterReach to tune): the right hand (joint 8) ahead of the hip.
void LogCounterReach(uintptr_t p, uintptr_t target) {
    uintptr_t joints = 0;
    float hip[3]{}, hand[3]{}, at[3]{}, origin[3]{};
    if (!ReadHip(p, hip) || !mem::Read(p + 0x318, joints) ||
        !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), origin, sizeof(origin)) ||
        !mem::SafeRead(reinterpret_cast<void*>(joints + 8 * 0x90 + 0x80), hand, sizeof(hand)) || !target ||
        !mem::SafeRead(reinterpret_cast<void*>(target + 0x30), at, sizeof(at)))
        return;
    const float ahead = (hand[0] - hip[0]) * g_counterDir[0] + (hand[2] - hip[2]) * g_counterDir[1];
    const float hipToTarget = (at[0] - hip[0]) * g_counterDir[0] + (at[2] - hip[2]) * g_counterDir[1];
    const float originAhead = (origin[0] - hip[0]) * g_counterDir[0] + (origin[2] - hip[2]) * g_counterDir[1];
    LOG("wesker moves: counter punch, hand %.0f cm ahead of the hip (height %.0f), target %.0f cm ahead (height %.0f), "
        "origin %.0f cm ahead", ahead, hand[1] - at[1], hipToTarget, hip[1] - at[1], originAhead);
}

// A free melee move asked from elsewhere (RequestCombatMove: the front escape follow-up, the melee swaps), started at
// the next render frame on its target (0 = as its key does).
std::atomic<int> g_moveRequest{ -1 };
// CutMoveForParry (parry.cpp, game thread): the tick of the cut, the cut move's start (TickParryCut drops only that one), asked.
std::atomic<DWORD> g_parryCut{ 0 }, g_parryCutStart{ 0 };
std::atomic<bool> g_parryCutAsked{ false };
std::atomic<uintptr_t> g_moveRequestTarget{ 0 };
std::atomic<DWORD> g_moveRequestTick{ 0 };
// The throw parry's riposte (parry.cpp, game thread): its enemy, thrown at the next render frame (TickCombat).
std::atomic<uintptr_t> g_throwRiposte{ 0 };
std::atomic<DWORD> g_throwRiposteTick{ 0 };
std::atomic<bool> g_throwNearest{ false };  // a melee swap's throw without a target: the nearest enemy
enum class Reaction { BlowOff, KnockDown, ReculLeg, Thrown, Slide, Recoil, Ground, Re6Light, Re6Strong, Re6Elbow, Re6Sprint, Re6Tired,
                      Re6Counter, Re6Trip };
struct CombatMove {
    const char* id;
    const char* name;
    int bank;
    uint16_t index;
    int damage;
    Reaction reaction;
    float from, to;  // the hit window, as parts of the motion (none: from > to)
    bool behind = false;  // a kick behind him (user: his back turned to the enemy, the hits behind)
    // Turned to the nearest enemy at the start (user, 2026-10-23: the back kick turns him round like a quick turn and the
    // final kick series must go straight ahead: played where he faces, the hits from his facing at the start).
    bool lock = true;
    float from2 = 1.0f, to2 = 0.0f;  // a second hit window, each enemy hit again in it (the kick series' spin)
    // The move ends there (idle asked), as a part of the motion (user, 2026-10-25: Jill's sweep ends in her own pose).
    float end = 1.0f;
    // It starts at this frame of its motion (user, 2026-10-28: Jill's dodge in place is her guard pose, the end of her
    // kick that the kick cuts): MoveBeginStart.
    float begin = 0.0f;
    // An RE4 pair (user, 2026-10-31: the suplex and the neck break "in the void"): the Majini's side, slot of
    // re4_pairs_shared_origin.lmt (PairVictimMotion), played from player 1's origin and yaw as Jill's throw plays its own.
    int victim = -1;
};
// Wesker's dodges (DmgNot motions, user 2026-10-23: in the free melee and the melee swaps): no hit, invulnerable. Named by
// their direction (user, 2026-10-28: 1 left, 2 right, 3 back, 4 forward, 6 low; the "Dodge Wesker" style).
constexpr CombatMove kCombat[] = {
    { "melee.free_wesker_kick1", "Coup de pied de Wesker 1", kWeskerBank, 0xC8, 300, Reaction::BlowOff, 0.25f, 0.60f },
    { "melee.free_wesker_kick2", "Coup de pied de Wesker 2", kWeskerBank, 0xC9, 300, Reaction::BlowOff, 0.25f, 0.60f },
    // Windows measured from the foot's height on Chris's skeleton (tools/motion_preview.py, 2026-10-23; user: the back
    // kick hit at its start, the series' spin never hit): back kick foot up 0.43-0.66 (peak 0.51-0.58); series first
    // kick 0.18-0.39, its spin 0.53-0.71.
    { "melee.free_wesker_kick3", "Coup de pied retourné de Wesker", kWeskerBank, 0xCA, 300, Reaction::BlowOff, 0.47f, 0.63f,
      true, false },
    { "melee.free_wesker_sweep", "Balayage de Wesker", kWeskerBank, 0xDE, 200, Reaction::KnockDown, 0.20f, 0.60f },
    { "melee.free_wesker_riposte", "Série de coups de pied de Wesker (fin d'esquives)", kWeskerBank, 0x2C, 400,
      Reaction::BlowOff, 0.20f, 0.38f, false, false, 0.54f, 0.70f },
    { "melee.free_jill_throw", "Projection de Chris", kJillBank, 0x01, 400, Reaction::Thrown, 0.75f, 0.95f },
    { "melee.free_wesker_dodge1", "Esquive gauche de Wesker (em80 0x14)", kWeskerBank, 0x14, 0, Reaction::BlowOff, 1.0f, 0.0f,
      false, false },
    { "melee.free_wesker_dodge2", "Esquive droite de Wesker (em80 0x13)", kWeskerBank, 0x13, 0, Reaction::BlowOff, 1.0f, 0.0f,
      false, false },
    { "melee.free_wesker_dodge3", "Esquive arrière de Wesker (em80 0x18)", kWeskerBank, 0x18, 0, Reaction::BlowOff, 1.0f, 0.0f,
      false, false },
    { "melee.free_wesker_dodge4", "Esquive avant de Wesker (em80 0x19)", kWeskerBank, 0x19, 0, Reaction::BlowOff, 1.0f, 0.0f,
      false, false },
    { "melee.free_wesker_dodge5", "Esquive de Wesker 5 (em80 0x26)", kWeskerBank, 0x26, 0, Reaction::BlowOff, 1.0f, 0.0f,
      false, false },
    { "melee.free_wesker_dodge6", "Esquive basse de Wesker (em80 0x17)", kWeskerBank, 0x17, 0, Reaction::BlowOff, 1.0f, 0.0f,
      false, false },
    // Jill's, second part of her fight (user, 2026-10-25; at the end, the order above being the melee swaps' and the
    // combos'): her throw (the Majini plays Chris's side of it), her high kick (Wesker's kicks' damage), her cartwheels and
    // round-off (no hit, invulnerable). Hit windows to adjust.
    // Her throw is two moves (user, 2026-10-25: "she leaps forward in a somersault and, touching someone at its end, the
    // catch starts"): AtckCatch's leap em84atk 0x3E (169 frames), then on an enemy reached the catch 0x3C (kThrow2Catch).
    // A miss is cut before her landing pose (user). Her native catch, sampled 2026-10-25: started at frame 36 of 169 with
    // Chris 131 cm away, he put at her very position turned the other way, neither moving after (the motions do it all).
    { "melee.free_jill2_throw", "Projection de Jill (2e phase)", kJill2AtkBank, 0x3E, 0, Reaction::BlowOff, 1.0f, 0.0f, false,
      true, 1.0f, 0.0f, 0.55f },
    // Cut before her guard at its end (user, 2026-10-26): em84atk 0x33, 122 frames: the kick 15-40, landed 55, upright
    // 78, then crouched in her guard to the end (tools/motion_preview.py).
    { "melee.free_jill2_kick", "Coup de pied de Jill", kJill2AtkBank, 0x33, 300, Reaction::BlowOff, 0.25f, 0.55f, false, true,
      1.0f, 0.0f, 0.65f },
    { "melee.free_jill2_cartwheel1", "Roue de Jill 1", kJill2NmlBank, 0x1E, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false },
    { "melee.free_jill2_cartwheel2", "Roue de Jill 2", kJill2NmlBank, 0x1F, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false },
    // Her round-off somersault: em84atk 0x06 (112 frames; the move test's slot 24, user; 0x0B was her drawing both pistols).
    // Cut before she aims at its end (user).
    { "melee.free_jill2_roundoff", "Rondade de Jill", kJill2AtkBank, 0x06, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f,
      0.0f, 0.70f },
    // Her back flip: em84nml 0x20 (56 frames; the move test's slot 19, user).
    { "melee.free_jill2_backflip", "Flip arrière de Jill", kJill2NmlBank, 0x20, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false,
      1.0f, 0.0f, 0.70f },  // cut before her pose at its end (user)
    // Her forward sweep: em84atk 0x34 (91 frames; the move test's slot 28, user), knocking down as Wesker's, cut before her
    // own pose at its end.
    { "melee.free_jill2_sweep", "Balayage de Jill", kJill2AtkBank, 0x34, 200, Reaction::KnockDown, 0.20f, 0.55f, false, true,
      1.0f, 0.0f, 0.65f },
    // Her slide (AtckSliding): em84atk 0x35 (156 frames; the move test's slot 29, user), knocking down. Window to adjust.
    // Cut before her pose at its end, after his getup (user: 70 % cut a hair too much, the getup must end). Played where
    // he faces, no enemy aimed at (user, 2026-10-30: "just in the character's direction").
    { "melee.free_jill2_slide", "Glissade de Jill", kJill2AtkBank, 0x35, 200, Reaction::KnockDown, 0.15f, 0.50f, false, false,
      1.0f, 0.0f, 0.78f },
    // Her throw's catch (not an option of its own: started by the leap on the enemy it reaches).
    // Cut before her pose at its end, as the missed leap (user); its hit before the cut.
    { "melee.free_jill2_catch", "Projection de Jill (2e phase), prise", kJill2AtkBank, 0x3C, 400, Reaction::Thrown, 0.45f, 0.70f,
      false, true, 1.0f, 0.0f, 0.72f },
    // Her somersault kick of the first part (AtckKick 0x12, em83act 0x00, 152 frames; user, 2026-10-27). On Chris's
    // skeleton (tools/motion_preview.py pose): run-up to 20, upside down 28-52, the feet coming down 75-90 cm ahead of the
    // hips 44-56 (the kick), landed 60 after about 2.5 m, sliding up, her guard from about 0.78: hit 0.26-0.38, cut there.
    { "melee.free_jill1_salto", "Salto de Jill (coup de pied)", kJill1ActBank, 0x00, 300, Reaction::BlowOff, 0.26f, 0.38f,
      false, true, 1.0f, 0.0f, 0.76f },
    // Leon's (user, 2026-10-28), at the end (the indices above are the melee swaps' and the combos'). RE4 (2005): the
    // strikes' windows around the frame where the limb is furthest out (research\re4_anims\REPORT.md section 6: 60 fps
    // frames of 51 / 101 / 97 / 131 / 103), the knife's a stagger with a prompt (the parried state "Recul + Leg"); the
    // paired moves (suplex, neck break, grab escapes) played alone to see them ("test").
    { "melee.free_leon_slash", "Coup de couteau de Leon (RE4)", kRe4LeonBank, 0x02, 60, Reaction::ReculLeg, 0.18f, 0.35f },
    { "melee.free_leon_slash_low", "Coup de couteau bas de Leon (RE4)", kRe4LeonBank, 0x04, 60, Reaction::ReculLeg, 0.28f,
      0.45f },
    { "melee.free_leon_slash_up", "Coup de couteau haut de Leon (RE4)", kRe4LeonBank, 0x06, 60, Reaction::ReculLeg, 0.22f,
      0.40f },
    { "melee.free_leon_roundhouse", "Coup de pied circulaire de Leon (RE4)", kRe4LeonBank, 0x08, 300, Reaction::BlowOff,
      0.30f, 0.45f },
    { "melee.free_leon_kick_kneeling", "Coup de pied de Leon sur un ennemi à genoux (RE4)", kRe4LeonBank, 0x09, 300,
      Reaction::BlowOff, 0.26f, 0.40f },
    // Wesker's palm strike of RE4's Mercenaries (plem10Showtay plays this entry: shotei, the palm heel; the agent's
    // REPORT section 6) in his tab, HUNK's neck break in his own (user, 2026-10-28: "one sub-tab per character").
    { "melee.free_wesker4_palm", "Coup de paume de Wesker (RE4)", kRe4LeonBank, 0x0A, 300, Reaction::BlowOff, 0.19f, 0.30f },
    { "melee.free_leon_front_kick", "Coup de pied haut de Leon (RE4)", kRe4LeonBank, 0x0D, 300, Reaction::BlowOff, 0.22f,
      0.35f },
    // With an enemy's side (RE4's pairs, user 2026-10-31): its damage when its head hits the ground / when its neck turns
    // (scratchpad pair_timing.py on the victims: the head on the ground at frame 45 of 177 - the first window, 0.40, made
    // the head blow late, user; the head turning 180 degrees from the chest over frames 74-116 of 155, fastest at 85).
    // Suplex 600 (user's default).
    { "melee.free_leon_suplex", "Suplex de Leon (RE4)", kRe4LeonBank, 0x0B, 600, Reaction::KnockDown, 0.25f, 0.40f, false,
      true, 1.0f, 0.0f, 1.0f, 0.0f, 0 },
    { "melee.free_hunk_neck_break", "Brise-nuque de HUNK (RE4)", kRe4LeonBank, 0x0C, 400, Reaction::KnockDown, 0.52f, 0.65f,
      false, true, 1.0f, 0.0f, 1.0f, 0.0f, 2 },
    { "melee.free_leon_escape_front", "Libération de face de Leon, seule (RE4, test)", kRe4LeonBank, 0x12, 100,
      Reaction::BlowOff, 0.28f, 0.40f },
    { "melee.free_leon_escape_back", "Libération de dos de Leon, seule (RE4, test)", kRe4LeonBank, 0x14, 100,
      Reaction::KnockDown, 0.30f, 0.50f },
    // His dodges: the side dive cut after the dive (frames 0-60 of 219, then a crouched pose), the back handspring (the
    // knife block, Krauser's hold in fact, and the quick turn dropped: user, 2026-10-28).
    { "melee.free_leon_dodge_dive", "Plongeon latéral de Leon (RE4)", kRe4LeonBank, 0x0E, 0, Reaction::BlowOff, 1.0f, 0.0f,
      false, false, 1.0f, 0.0f, 0.30f },
    // The back handspring cut after its first bounce (frame 70 of 147: landed at 44, a hop to 70, another to 96, then he
    // stands; user, 2026-10-28: "l'attente en fin est trop longue", then 2026-10-29: the aim window after a perfect
    // dodge closed before it ended, "arrête l'anim après le premier rebond").
    { "melee.free_leon_dodge_handspring", "Saut arrière de Leon (RE4)", kRe4LeonBank, 0x0F, 0, Reaction::BlowOff, 1.0f, 0.0f,
      false, false, 1.0f, 0.0f, 0.48f },
    // RE4 Remake: his knife parries (the parry slots 0-5) and the kicks that follow a parry (10-12, a 2.5-3.3 m run-up to
    // the stunned enemy: windows to adjust), his knife grab moves alone (their enemy side not played yet: "test").
    { "melee.free_leon_r_parry", "Parade au couteau de Leon (RE4 Remake, test)", kRe4rParryBank, 0, 0, Reaction::BlowOff,
      1.0f, 0.0f },
    { "melee.free_leon_r_parry_just", "Parade parfaite de Leon (RE4 Remake, test)", kRe4rParryBank, 1, 0, Reaction::BlowOff,
      1.0f, 0.0f },
    { "melee.free_leon_r_parry_draw", "Parade de Leon, couteau dégainé (RE4 Remake, test)", kRe4rParryBank, 2, 0,
      Reaction::BlowOff, 1.0f, 0.0f },
    { "melee.free_leon_r_parry_left", "Parade de Leon à gauche (RE4 Remake, test)", kRe4rParryBank, 3, 0, Reaction::BlowOff,
      1.0f, 0.0f },
    { "melee.free_leon_r_parry_right", "Parade de Leon à droite (RE4 Remake, test)", kRe4rParryBank, 4, 0, Reaction::BlowOff,
      1.0f, 0.0f },
    { "melee.free_leon_r_parry_lose", "Parade ratée de Leon (RE4 Remake, test)", kRe4rParryBank, 5, 0, Reaction::BlowOff,
      1.0f, 0.0f },
    { "melee.free_leon_r_kick_a", "Coup de pied de Leon après la parade A (RE4 Remake)", kRe4rParryBank, 10, 300,
      Reaction::BlowOff, 0.45f, 0.70f },
    { "melee.free_leon_r_kick_b", "Coup de pied de Leon après la parade B (RE4 Remake)", kRe4rParryBank, 11, 300,
      Reaction::BlowOff, 0.45f, 0.70f },
    { "melee.free_leon_r_kick_c", "Coup de pied de Leon après la parade C (RE4 Remake)", kRe4rParryBank, 12, 300,
      Reaction::BlowOff, 0.45f, 0.70f },
    { "melee.free_leon_r_escape_knife", "Libération au couteau de Leon, seule (RE4 Remake, test)", kRe4rGrabBank, 0, 0,
      Reaction::BlowOff, 1.0f, 0.0f },
    { "melee.free_leon_r_escape_kill", "Libération au couteau mortelle de Leon, seule (RE4 Remake, test)", kRe4rGrabBank, 1,
      0, Reaction::BlowOff, 1.0f, 0.0f },
    { "melee.free_leon_r_grab_parry", "Saisie parée par Leon, seule (RE4 Remake, test)", kRe4rGrabBank, 2, 0,
      Reaction::BlowOff, 1.0f, 0.0f },
    { "melee.free_leon_r_grab_parry_b", "Saisie parée par Leon B, seule (RE4 Remake, test)", kRe4rGrabBank, 3, 0,
      Reaction::BlowOff, 1.0f, 0.0f },
    { "melee.free_leon_r_knife_kill", "Mise à mort au couteau de Leon, seule (RE4 Remake, test)", kRe4rGrabBank, 4, 0,
      Reaction::BlowOff, 1.0f, 0.0f },
    { "melee.free_leon_r_stealth_kill", "Exécution furtive de Leon, seule (RE4 Remake, test)", kRe4rGrabBank, 5, 0,
      Reaction::BlowOff, 1.0f, 0.0f },
    { "melee.free_leon_r_kneel_finisher", "Achèvement au couteau d'un ennemi à genoux, seul (RE4 Remake, test)",
      kRe4rGrabBank, 6, 0, Reaction::BlowOff, 1.0f, 0.0f },
    { "melee.free_leon_r_escape", "Libération de Leon sans couteau, seule (RE4 Remake, test)", kRe4rGrabBank, 7, 0,
      Reaction::BlowOff, 1.0f, 0.0f },
    { "melee.free_leon_r_suplex", "Suplex de Leon, seul (RE4 Remake, test)", kRe4rGrabBank, 8, 0, Reaction::BlowOff, 1.0f,
      0.0f },
    // Jill's guard pose, the "Dodge Jill" style's dodge in place (user, 2026-10-28: "la pose que Jill faisait en fin de
    // chacune de ses anims"): her kick em84atk 0x33 from its cut (frame 79 of 122: upright, then crouched in her guard
    // to the end, 46 cm back).
    { "melee.free_jill2_guard_pose", "Pose de garde de Jill", kJill2AtkBank, 0x33, 0, Reaction::BlowOff, 1.0f, 0.0f, false,
      false, 1.0f, 0.0f, 1.0f, 79.0f },
    // The duo moves of Mercenaries Reunion (kPlayerLists; user, 2026-11-05): played alone, each in its character's sub-tab -
    // Chris's TAG NECK first in Wesker's ("a move of Wesker's", user), in Chris's since 2026-11-06 (it is his second duo
    // move: Wesker has none; the id melee.free_wesker_tag_neck had no shortcut bound). Windows from the hands and feet on Chris's skeleton
    // (scratchpad limbs.py, 2026-11-05): Chris's TAG UPPER 0x16 (159 frames) his right uppercut 49-57, then the arm held
    // up; his TAG NECK 0x17 (110) the left hand raised forward 21-30, the right palm 39-47 (the blow); Sheva's TAG ADD 0x16
    // (130) a chop 5-12, an uppercut 24-33, a low left 54-61, an uppercut 66-74 (two windows); Josh's TAG STRAIGHT 0x16
    // (120) a left jab 24-31 and the right straight 33-40, lunging 1 m; Excella's TAG HOOK 0x16 (80) both hands 26-32;
    // Barry's TAG HOOK 0x0A (116) both hands 43-50; Rebecca's TAG HOOK 0x16 (156) both hands raised 3-9 then pushed
    // forward slowly to 48 and held: her fire spray (its melee entry +0x5C = 3 as her FIRE SPRAY's; the flame comes from
    // the game's attack code, not from the motion: not shown by the trainer move yet). The melee swap's choices (swaps.cpp kActions) name them by
    // their index here, 54-60: keep them in this order, before Josh's counter (the last).
    { "melee.free_chris_tag_upper", "Coup duo de Chris (TAG UPPER, Reunion)", kChrisListBank, 0x16, 300, Reaction::BlowOff,
      0.30f, 0.37f },
    { "melee.free_chris_tag_neck", "Coup duo de Chris (TAG NECK, Reunion)", kChrisListBank, 0x17, 300, Reaction::BlowOff,
      0.33f, 0.44f },
    { "melee.free_sheva_tag_add", "Coup duo de Sheva (TAG ADD, Reunion)", kShevaListBank, 0x16, 300, Reaction::BlowOff, 0.03f,
      0.26f, false, true, 0.40f, 0.58f },
    { "melee.free_josh_tag_straight", "Coup duo de Josh (TAG STRAIGHT, Reunion)", kJoshListBank, 0x16, 300,
      Reaction::BlowOff, 0.19f, 0.35f },
    { "melee.free_excella_tag_hook", "Coup duo d'Excella (TAG HOOK, Reunion)", kExcellaListBank, 0x16, 300,
      Reaction::BlowOff, 0.31f, 0.42f },
    { "melee.free_barry_tag_hook", "Coup duo de Barry (TAG HOOK, Reunion)", kBarryListBank, 0x0A, 300, Reaction::BlowOff,
      0.36f, 0.45f },
    // Rebecca's: from her arms stretched out with the spray's sound (user, 2026-11-06: "hit while she prepares her spray,
    // it should be when she stretches her arms"), frames 39-70.
    { "melee.free_rebecca_tag_hook", "Coup duo de Rebecca (TAG HOOK, Reunion)", kRebeccaListBank, 0x16, 300,
      Reaction::BlowOff, 0.25f, 0.45f },
    // "Josh Contre" (user, 2026-11-01: "synchronised as the projection, but the death only from the damage, the critical
    // or the one-hit kill"): Josh's Finisher Front HEAD CRUSH (his row 4 move 5: his side slot 0x0F, the victim's 0x1D,
    // both 145 frames; no attack shape, its param 0xA2 power 3000 - the game's finisher kills and blows the head itself)
    // as a pair: the victim's side from player 1's origin turned to him (its body 1.3 m ahead, lying on its back at the
    // end), the blow when its body is slammed down (its hips from 120 cm at frame 53 to 40 at 60; Josh's effort 10 at 56).
    { "melee.free_josh_counter", "Projection de Josh (Finisher Front)", kJoshActBank, 0x0F, 3000, Reaction::KnockDown, 0.36f,
      0.43f, false, true, 1.0f, 0.0f, 1.0f, 0.0f, kJoshPairVictim + 0x1D },
    // RE6's dodges, slide and getups (user, 2026-11-08; re6_evade.lmt): cut just before their end, the getup then chosen by
    // Re6EvadeEnded. Index order matters there: dodges first, then the getups.
    { "melee.free_re6_dodge_duck", "Esquive sur place (RE6)", kRe6EvadeBank, 5, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f, 0.0f, 0.97f },
    { "melee.free_re6_dodge_forward", "Esquive avant (RE6)", kRe6EvadeBank, 0, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f, 0.0f, 0.97f },
    { "melee.free_re6_dodge_back", "Esquive arrière (RE6)", kRe6EvadeBank, 1, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f, 0.0f, 0.97f },
    { "melee.free_re6_dodge_left", "Esquive gauche (RE6)", kRe6EvadeBank, 2, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f, 0.0f, 0.97f },
    { "melee.free_re6_dodge_right", "Esquive droite (RE6)", kRe6EvadeBank, 3, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f, 0.0f, 0.97f },
    { "melee.free_re6_slide", "Glissade (RE6)", kRe6EvadeBank, 4, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f, 0.0f, 0.55f },  // frame 60: still sliding, the hips at the getup's height; after it he only sinks and stops
    { "melee.free_re6_getup", "Relevé sur place (RE6)", kRe6EvadeBank, 8, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false },
    { "melee.free_re6_getup_slide", "Relevé après glissade (RE6)", kRe6EvadeBank, 9, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false },
    { "melee.free_re6_getup_forward", "Relevé avant (RE6)", kRe6EvadeBank, 10, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false },
    { "melee.free_re6_getup_left", "Relevé gauche (RE6)", kRe6EvadeBank, 11, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false },
    { "melee.free_re6_getup_right", "Relevé droite (RE6)", kRe6EvadeBank, 12, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false },
    { "melee.free_re6_getup_back_left", "Relevé arrière gauche (RE6)", kRe6EvadeBank, 13, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false },
    { "melee.free_re6_getup_back_right", "Relevé arrière droite (RE6)", kRe6EvadeBank, 14, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false },
    { "melee.free_re6_rise", "Relevé de l'esquive sur place (RE6)", kRe6EvadeBank, 15, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false },
    // On the ground (user, 2026-11-09: "the lateral rolls and RE6's back dash from the ground on a shortcut"; re6_evade.lmt 43 / 44 BhA A8 / AA, 45 BhB 4E,
    // 47 BhB 22): the rolls and the back dash end lying (down again with the aim held, else the getup), the back roll ends standing. Played by "Au sol :
    // roulade ou dash arrière (RE6)"; 4E first taken for the back dash was "the back roll of a getup, keep it in place" (user).
    { "melee.free_re6_ground_roll_left", "Roulade au sol gauche (RE6)", kRe6EvadeBank, 43, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f, 0.0f, 0.97f },
    { "melee.free_re6_ground_roll_right", "Roulade au sol droite (RE6)", kRe6EvadeBank, 44, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f, 0.0f, 0.97f },
    { "melee.free_re6_ground_back_roll", "Roulade arrière de relevé (RE6)", kRe6EvadeBank, 45, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false },
    { "melee.free_re6_ground_back_dash", "Dash arrière au sol (RE6)", kRe6EvadeBank, 47, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f, 0.0f, 0.97f },
    // "Se baisser en visant" (user, 2026-11-09: "Chris goes down while aiming, the pose frozen, another key than RE6's dodge"): the dodge in place cut at its
    // lowest frame (16 of 34), the pose held (re6_evade.lmt 46) while the aim is, then the rest of that dodge.
    { "melee.free_re6_crouch", "Se baisser (pose figée)", kRe6EvadeBank, 5, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f, 0.0f, 0.47f },
    { "melee.free_re6_crouch_rise", "Se relever (pose figée)", kRe6EvadeBank, 5, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f, 0.0f, 1.0f, 0.47f },
    // Research (user, 2026-11-09): "Test : corps à corps de RE6 (AcA)" plays the test bank's motion g_re6TestIndex (no shortcut row).
    { "melee.re6_melee_test_move", "Test : corps à corps de RE6", kRe6MeleeBank, 0xFFFF, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false },
    // RE6's moves named by the user in the test bank (docs/RE6_MELEE.md; re6_melee.lmt's index, played from bank 8 as the test). The blow
    // windows from RE6's own events (the attack type's bit) and the limb's reach (scratchpad reach.py, 2026-11-09).
    // The counter against leaping enemies (n° 6, AcA 0C: the right fist down at frames 8-16, 78 cm ahead at 111 cm; RE6's window 8-12),
    // with a two-handed weapon n° 7 (AcA 0E, window 8-12): Jill's and Josh's throws become it on dogs, lickers and spider / flying
    // parasites (StartCombatMove; user, 2026-11-09: "the RE6 counter against leaping enemies replaces Jill's and Josh's throws, as a
    // counter or in the free melee, on dogs, spider parasites, flying parasites and lickers").
    { "melee.free_re6_counter_leap", "Contre RE6 (ennemis qui sautent)", kRe6MeleeBank, 6, 600, Reaction::BlowOff, 0.075f, 0.15f },
    { "melee.free_re6_counter_leap_long", "Contre RE6 avec une arme à deux mains (ennemis qui sautent)", kRe6MeleeBank, 7, 600,
      Reaction::BlowOff, 0.094f, 0.165f },
    // The Arm Fronts of RE6's characters (kicks; user, 2026-11-09: "add them to the free melee under each one and to the melee swap").
    { "melee.free_re6_leon_arm_front", "Arm Front de Leon (RE6)", kRe6MeleeBank, 8, 400, Reaction::Recoil, 0.23f, 0.36f },
    { "melee.free_re6_jake_arm_front", "Arm Front de Jake (RE6)", kRe6MeleeBank, 9, 400, Reaction::Recoil, 0.336f, 0.416f },
    { "melee.free_re6_ada_arm_front", "Arm Front d'Ada (RE6)", kRe6MeleeBank, 10, 400, Reaction::Recoil, 0.398f, 0.53f },
    { "melee.free_re6_helena_arm_front", "Arm Front d'Helena (RE6)", kRe6MeleeBank, 11, 400, Reaction::Recoil, 0.333f, 0.429f },
    // The Grounds (blows on a downed enemy; user, 2026-11-09): Leon's (n° 12, AcA 2C, RE6's blow event frames 34-38 of 106), everyone's
    // with a two-handed weapon (n° 13, AcA 2E, 44-50 of 159) and with the knife (n° 14, AcA 30, 58-64 of 172): "Ground de RE6 avec
    // une arme à deux mains ou le couteau" turns the game's Ground into the last two (Re6GroundRequest).
    { "melee.free_re6_leon_ground", "Ground de Leon (RE6)", kRe6MeleeBank, 12, 400, Reaction::Ground, 0.30f, 0.40f },
    { "melee.free_re6_ground_attack_long", "Ground avec une arme à deux mains (RE6)", kRe6MeleeBank, 13, 400, Reaction::Ground,
      0.264f, 0.327f },
    { "melee.free_re6_ground_attack_knife", "Ground au couteau (RE6)", kRe6MeleeBank, 14, 400, Reaction::Ground, 0.326f, 0.384f },
    // The RE6 melee (user, 2026-11-09; docs/RE6_MELEE.md): the characters' two-blow combos (a light first blow, a stronger second), the
    // No melee targeting for the front series, the sprint attacks and the elbows (user, 2026-11-09: lock false, played where he faces).
    // two-handed weapon's, the sprint attacks, the back elbows, the blow without stamina, the crate blows. Windows from RE6's blow events.
    { "melee.free_re6_chris_combo1", "Chris : 1er coup de sa série (RE6)", kRe6MeleeBank, 32, 150, Reaction::Re6Light, 0.24f, 0.38f, false, false },
    { "melee.free_re6_chris_combo2", "Chris : 2e coup de sa série (RE6)", kRe6MeleeBank, 33, 400, Reaction::Re6Strong, 0.33f, 0.46f, false, false },
    { "melee.free_re6_leon_combo1", "Leon : 1er coup de sa série (RE6)", kRe6MeleeBank, 0, 150, Reaction::Re6Light, 0.247f, 0.382f, false, false },
    { "melee.free_re6_leon_combo2", "Leon : 2e coup de sa série (RE6)", kRe6MeleeBank, 2, 400, Reaction::Re6Strong, 0.167f, 0.31f, false, false },
    { "melee.free_re6_helena_combo1", "Helena : 1er coup de sa série (RE6)", kRe6MeleeBank, 38, 150, Reaction::Re6Light, 0.396f, 0.505f, false, false },
    { "melee.free_re6_helena_combo2", "Helena : 2e coup de sa série (RE6)", kRe6MeleeBank, 39, 400, Reaction::Re6Strong, 0.26f, 0.40f, false, false },
    { "melee.free_re6_jake_combo1", "Jake : 1er coup de sa série (RE6)", kRe6MeleeBank, 43, 150, Reaction::Re6Light, 0.22f, 0.33f, false, false },
    { "melee.free_re6_jake_combo2", "Jake : 2e coup de sa série (RE6)", kRe6MeleeBank, 44, 400, Reaction::Re6Strong, 0.16f, 0.31f, false, false },
    // Sherry's (user, 2026-11-12, replaying the characters in RE6: "I had swapped her first and second blows"): n° 42 first, n° 41
    // second (the ids kept: the shortcuts and the table's lines follow the blow's place in the series).
    { "melee.free_re6_common_x41_1", "Sherry : 1er coup de sa série (RE6)", kRe6MeleeBank, 42, 150, Reaction::Re6Light, 0.374f,
      0.523f, false, false },
    { "melee.free_re6_common_x41_2", "Sherry : 2e coup de sa série (RE6)", kRe6MeleeBank, 41, 400, Reaction::Re6Strong, 0.228f,
      0.33f, false, false },
    { "melee.free_re6_common_long1", "1er coup avec une arme à deux mains (RE6)", kRe6MeleeBank, 1, 200, Reaction::Re6Light, 0.283f,
      0.404f, false, false },
    { "melee.free_re6_common_long2", "2e coup avec une arme à deux mains (RE6)", kRe6MeleeBank, 3, 400, Reaction::Re6Strong, 0.396f,
      0.515f, false, false },
    { "melee.free_re6_common_shoulder", "Coup d'épaule en sprint (RE6)", kRe6MeleeBank, 4, 300, Reaction::Re6Sprint, 0.296f, 0.469f, false, false },
    { "melee.free_re6_common_jump_kick", "Coup de pied sauté en sprint (RE6)", kRe6MeleeBank, 5, 300, Reaction::Re6Sprint, 0.23f, 0.374f, false, false },
    { "melee.free_re6_common_sprint_var", "Attaque en sprint, variante (RE6)", kRe6MeleeBank, 36, 300, Reaction::Re6Sprint, 0.27f, 0.457f, false, false },
    { "melee.free_re6_common_elbow", "Coup de coude arrière (RE6)", kRe6MeleeBank, 30, 400, Reaction::Re6Elbow, 0.309f, 0.433f, true, false },
    { "melee.free_re6_common_elbow_var", "Coup de coude arrière, variante (RE6)", kRe6MeleeBank, 31, 400, Reaction::Re6Elbow, 0.322f,
      0.46f, true, false },
    { "melee.free_re6_common_tired", "Coup sans endurance (RE6)", kRe6MeleeBank, 27, 50, Reaction::Re6Tired, 0.193f, 0.269f, false, false },
    { "melee.free_re6_common_crate_high", "Coup de crosse sur une caisse en hauteur (RE6)", kRe6MeleeBank, 37, 100, Reaction::Re6Light,
      0.333f, 0.494f, false, false },
    { "melee.free_re6_common_crate_low", "Coup sur une caisse au sol (RE6)", kRe6MeleeBank, 47, 100, Reaction::Re6Light, 0.138f, 0.241f,
      false, false },
    // RE6's characters' moves without a synchronised victim (user, 2026-11-09, docs/RE6_MELEE.md): Chris's Arm Front, the Finishers
    // (the game's finish blow's power), the generic Help (Jake's), Jake's strikes while aiming; windows from RE6's blow events.
    { "melee.free_re6_chris_arm_front", "Arm Front de Chris (RE6)", kRe6MeleeBank, 34, 400, Reaction::Recoil, 0.304f, 0.400f },
    { "melee.free_re6_chris_finisher", "Finisher Front de Chris (RE6)", kRe6MeleeBank, 78, 3000, Reaction::BlowOff, 0.262f, 0.415f },
    { "melee.free_re6_leon_finisher", "Finisher Front de Leon (RE6)", kRe6MeleeBank, 66, 3000, Reaction::BlowOff, 0.142f, 0.219f },
    { "melee.free_re6_helena_finisher", "Finisher Front d'Helena (RE6)", kRe6MeleeBank, 72, 3000, Reaction::BlowOff, 0.170f, 0.270f },
    { "melee.free_re6_piers_finisher", "Finisher Front de Piers (RE6)", kRe6MeleeBank, 84, 3000, Reaction::BlowOff, 0.264f, 0.337f },
    { "melee.free_re6_jake_finisher", "Finisher Front de Jake (RE6)", kRe6MeleeBank, 95, 3000, Reaction::BlowOff, 0.179f, 0.256f },
    { "melee.free_re6_sherry_finisher", "Finisher de Sherry (RE6)", kRe6MeleeBank, 102, 3000, Reaction::BlowOff, 0.243f, 0.467f },
    { "melee.free_re6_ada_finisher", "Finisher d'Ada (RE6)", kRe6MeleeBank, 109, 3000, Reaction::BlowOff, 0.250f, 0.370f },
    { "melee.free_re6_agent_finisher", "Finisher de l'agent (RE6)", kRe6MeleeBank, 115, 3000, Reaction::BlowOff, 0.247f, 0.298f },
    { "melee.free_re6_jake_help", "Help de Jake (RE6)", kRe6MeleeBank, 53, 300, Reaction::Recoil, 0.275f, 0.396f },
    { "melee.free_re6_jake_leg_front", "Leg Front de Jake (RE6)", kRe6MeleeBank, 119, 400, Reaction::BlowOff, 0.133f, 0.248f },
    { "melee.free_re6_jake_hook_right", "Crochet droit de Jake en visant (RE6)", kRe6MeleeBank, 124, 150, Reaction::Re6Light, 0.000f, 0.227f },
    { "melee.free_re6_jake_hook_left", "Crochet gauche de Jake en visant (RE6)", kRe6MeleeBank, 125, 150, Reaction::Re6Light, 0.218f, 0.436f },
    { "melee.free_re6_jake_kick", "Coup de pied de Jake (RE6)", kRe6MeleeBank, 126, 300, Reaction::Recoil, 0.364f, 0.523f },
    { "melee.free_re6_jake_strike", "Frappe de Jake (RE6)", kRe6MeleeBank, 127, 300, Reaction::Re6Strong, 0.355f, 0.620f },
    { "melee.free_re6_jake_sweep", "Balayage de Jake (RE6)", kRe6MeleeBank, 128, 300, Reaction::BlowOff, 0.252f, 0.360f },
    { "melee.free_re6_jake_last_strike", "Dernière frappe de Jake en visant (RE6)", kRe6MeleeBank, 129, 500, Reaction::BlowOff, 0.288f, 0.366f },
    { "melee.free_re6_jake_clear_kick", "Coup de pied dégagé de Jake (RE6)", kRe6MeleeBank, 133, 400, Reaction::BlowOff, 0.267f, 0.383f },
    // RE6's synchronised moves (user, 2026-11-09: Head Flash / Leg Back "with the enemy's victim reaction", Jake's Ground "one option,
    // the trainer chooses by the enemy's position"): the enemy plays its side (kRe6PairVictim + its re6_melee.lmt index) 1 m in front of
    // player 1, facing him, held by the pair lock; the blow at the victim side's damage mark (its event 0x19).
    { "melee.free_re6_leon_leg_back", "Leg Back de Leon (RE6)", kRe6MeleeBank, 63, 600, Reaction::KnockDown, 0.362f, 0.409f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 61 },
    { "melee.free_re6_leon_head_flash", "Head Flash de Leon (RE6)", kRe6MeleeBank, 64, 500, Reaction::KnockDown, 0.776f, 0.814f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 62 },
    { "melee.free_re6_helena_leg_back", "Leg Back d'Helena (RE6)", kRe6MeleeBank, 69, 600, Reaction::KnockDown, 0.439f, 0.474f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 67 },
    { "melee.free_re6_helena_head_flash", "Head Flash d'Helena (RE6)", kRe6MeleeBank, 70, 500, Reaction::KnockDown, 0.745f, 0.783f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 68 },
    { "melee.free_re6_chris_head_flash", "Head Flash de Chris (RE6)", kRe6MeleeBank, 76, 500, Reaction::KnockDown, 0.593f, 0.622f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 149 },  // 149: Chris 24 to frame 137 + the J'avo's fall onto its back em1000_g00 3E (user, 2026-11-10)
    { "melee.free_re6_piers_leg_back", "Leg Back de Piers (RE6)", kRe6MeleeBank, 81, 600, Reaction::KnockDown, 0.157f, 0.188f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 79 },
    { "melee.free_re6_piers_head_flash", "Head Flash de Piers (RE6)", kRe6MeleeBank, 82, 500, Reaction::KnockDown, 0.622f, 0.667f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 80 },
    { "melee.free_re6_jake_leg_back", "Leg Back de Jake (RE6)", kRe6MeleeBank, 86, 600, Reaction::KnockDown, 0.350f, 0.378f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 85 },
    { "melee.free_re6_jake_head_flash", "Head Flash de Jake (RE6)", kRe6MeleeBank, 91, 500, Reaction::KnockDown, 0.497f, 0.534f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 87 },
    { "melee.free_re6_jake_ground", "Ground de Jake (RE6)", kRe6MeleeBank, 92, 400, Reaction::KnockDown, 0.559f, 0.592f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 88 },
    { "melee.free_re6_jake_ground_belly", "Ground de Jake, ennemi sur le ventre (RE6)", kRe6MeleeBank, 93, 400, Reaction::KnockDown, 0.616f, 0.645f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 89 },
    { "melee.free_re6_sherry_leg_back", "Leg Back de Sherry (RE6)", kRe6MeleeBank, 99, 600, Reaction::KnockDown, 0.504f, 0.557f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 96 },
    { "melee.free_re6_sherry_head_flash", "Head Flash de Sherry (RE6)", kRe6MeleeBank, 100, 500, Reaction::KnockDown, 0.426f, 0.468f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 97 },
    { "melee.free_re6_ada_head_flash", "Head Flash d'Ada (RE6)", kRe6MeleeBank, 104, 500, Reaction::KnockDown, 0.367f, 0.396f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 103 },
    { "melee.free_re6_ada_leg_back", "Leg Back d'Ada (RE6)", kRe6MeleeBank, 106, 600, Reaction::KnockDown, 0.458f, 0.504f, false, true,
      1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 105 },
    // RE6's counters (user, 2026-11-09: Piers's, Chris's, Jake's, Leon's, Ada's "in the RE6 counter's dropdown and the random"; the
    // bayonet's with a two-handed weapon only): the RE6 parry's ripostes (parry.cpp), the parried state as their reaction.
    { "melee.free_re6_counter_piers", "Contre de Piers (RE6)", kRe6MeleeBank, 54, 500, Reaction::Re6Counter, 0.396f, 0.515f },
    { "melee.free_re6_counter_chris", "Contre de Chris (RE6)", kRe6MeleeBank, 55, 500, Reaction::Re6Counter, 0.376f, 0.495f },
    // Its blow at the left elbow's rise (user, 2026-11-10: "its damage and blow sound too early: with the elbow"; research/re6_long_strike/
    // elbow.py: the right arm's parry on frames 0-18, the left elbow up to the head's height on 29-32, fastest at 31): frames 30-33.
    { "melee.free_re6_counter_jake", "Contre de Jake (RE6)", kRe6MeleeBank, 58, 500, Reaction::Re6Counter, 0.297f, 0.327f },
    // Its kick (user, 2026-11-11: "the sound and the damage come a hair late"; re6_melee 59, 99 f: the right foot fastest 19-23, at the
    // head's height, leg stretched, on 24-25, held up to 45): from frame 28 (was 40).
    { "melee.free_re6_counter_leon", "Contre de Leon (RE6)", kRe6MeleeBank, 59, 500, Reaction::Re6Counter, 0.283f, 0.404f },
    { "melee.free_re6_counter_ada", "Contre d'Ada (RE6)", kRe6MeleeBank, 60, 500, Reaction::Re6Counter, 0.205f, 0.308f },
    { "melee.free_re6_counter_bayonet", "Contre à la baïonnette (RE6)", kRe6MeleeBank, 56, 500, Reaction::Re6Counter, 0.426f, 0.545f },
    // RE6's sweep against grabs (n° 57, AcA 9A: the right foot low 1.07 m ahead at frame 36 of 111): "Parade RE6 : balayage contre les
    // saisies", the Majini tripped (ForceMajiniState 19).
    { "melee.free_re6_counter_sweep", "Balayage contre les saisies (RE6)", kRe6MeleeBank, 57, 300, Reaction::Re6Trip, 0.27f, 0.378f },
    // RE6's half-turn in the slide (user, 2026-11-09; re6_evade.lmt 51, 52 with two hands): backwards held during a slide (TickRe6SlideTurn).
    { "melee.free_re6_slide_turn", "Demi-tour en glissade (RE6)", kRe6EvadeBank, 51, 0, Reaction::BlowOff, 1.0f, 0.0f, false, false, 1.0f, 0.0f, 0.9f },
    // Chris's RE6 Leg Back (user, 2026-11-10: "I told you to put anim 46 as Chris RE6's Leg Back"): AcA 80 (n° 46, 197 frames) with the
    // J'avo's side em1000CoC 80 (re6_melee.lmt 165, 255 frames: the knee given at ~24, down to the ground from 152). Two blows (user, 2026-11-10:
    // "the blow's sound at the kick in the leg, then a second blow + a grunt on the downward elbow"; measured on his skeleton: the right
    // foot fastest at frames 18-19, the right elbow coming down fastest at 98-101): the kick 19-23, its impact alone; the elbow 100-104, its
    // impact and his grunt (the hit windows' `silent`); 300 each (600 before, at frame 152 - the J'avo reaching the ground).
    { "melee.free_re6_chris_leg_back", "Leg Back de Chris (RE6)", kRe6MeleeBank, 46, 300, Reaction::KnockDown, 0.096f, 0.117f, false, true,
      0.508f, 0.528f, 1.0f, 0.0f, kRe6PairVictim + 165 },
    // RE6's knife counter and throat cut (user, 2026-11-09, recorded: "I just made a knife counter", "the throat cut"; the next RE6
    // features): AcA 5A (n° 28, 191 f) with the J'avo's side em1000CoC 5A (re6_melee 162, 367 f) and AcA 5C (n° 29, 171 f, the knife's Leg
    // Back: from behind) with CoC 5C (163, 314 f). Their blow is the event bit 29 of block 0x34, one frame with its power (as Chris's Leg
    // Back's elbow at 101, 800): the counter's at 51 (550), the throat cut's at 80 (3000). The knife in hand through them (KnifeIntoHand).
    { "melee.free_re6_common_knife_counter", "Contre au couteau (RE6)", kRe6MeleeBank, 28, 550, Reaction::KnockDown, 0.267f, 0.293f,
      false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 162 },
    { "melee.free_re6_common_throat_cut", "Égorgement au couteau (RE6)", kRe6MeleeBank, 29, 3000, Reaction::KnockDown, 0.468f, 0.491f,
      false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 163 },
    // RE6's special counters on armed J'avo (user, 2026-11-10: "the special counters"; tools/import_re6_melee.py): the machete disarm, Chris
    // em1000_gcob CE / CF (re6_melee 168, 181 f) with the J'avo's em1000_gcca D2 / D3 (169, 391 f) - he kills it with its own blade: the
    // knife in hand as that blade (Re6KnifePair), the kill at his right arm's fast swing, frames 126-131 (research/re6_long_strike/
    // elbow.py); the gun counter, Chris em1000_gcob D0 / D1 (170, 101 f) with the J'avo's gcob 24 / 25 then g00 C2 / C3 (171, 320 f): his
    // left fist's blow at frames 51-54 (27 cm / frame).
    // Its blow where the knife goes in (user, 2026-11-11: "the knife's sound comes after the knife is planted in the body", then "still
    // at least a second too late"): both fists go down from 140 to 87 cm on frames 30-44 (research/re6_long_strike/elbow.py 168; set at
    // 40: "too early, it is not when both hands go down that he runs it through"), then thrust forward from 39 to 61 cm ahead on 44-56,
    // furthest at 56-58 - the knife going in: frames 54-59 (the swing at 124-132 comes after, RE6's event bit 29 at 98-105), then "a
    // little too early, at the thrust's start: wait until the knife is seen going in": frames 60-65.
    { "melee.free_re6_common_machete_counter", "Désarmement à la machette (RE6)", kRe6MeleeBank, 168, 3000, Reaction::KnockDown,
      0.331f, 0.359f, false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 169 },
    { "melee.free_re6_common_gun_counter", "Contre de l'arme à feu (RE6)", kRe6MeleeBank, 170, 600, Reaction::KnockDown, 0.505f,
      0.535f, false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 171 },
    // RE6's armed-zombie counters (user, 2026-11-10, recorded in RE6 with Jake on Urban Chaos: "the counters against the clubs, the wrenches
    // and the axes", and "the weapon steal after a prompt"; tools/import_re6_melee.py, MODLOG "RE6's armed-zombie counters"): em5000_zob
    // 50 (re6_melee 172, 241 f) with the zombie's em5000_bhb 50 (173, 341 f), zob 52 (174, 236 f) with bhb 52 (175, 336 f) - the weapon
    // in the player's hand from frame 135 -, the steal zob 5A (176, 194 f) with bhb 5A (177, 179 f) - in his hand from frame 85. Their
    // blow is the player side's event bit 29 (one frame, its power): 128 (1000), 111 (100), 80 (1000); the zombie's damage window
    // (its bit 25) starts there too. Both sides start from the same origin (kRe6PairPlaces 0).
    { "melee.free_re6_common_club_counter_1", "Contre contre massue, clé ou hache 1 (RE6)", kRe6MeleeBank, 172, 1000,
      Reaction::KnockDown, 0.531f, 0.548f, false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 173 },
    { "melee.free_re6_common_club_counter_2", "Contre contre massue, clé ou hache 2 (RE6)", kRe6MeleeBank, 174, 100,
      Reaction::KnockDown, 0.470f, 0.487f, false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 175 },
    { "melee.free_re6_common_weapon_steal", "Vol d'arme (RE6)", kRe6MeleeBank, 176, 1000, Reaction::KnockDown, 0.412f, 0.433f, false,
      true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 177 },
    // RE6's environmental executions (user, 2026-11-11, recorded in RE6: "the wall at the side" AcA 3E, "the barrier at the side" AcA 42, a
    // table / barrier from the front AcA 44; tools/import_re6_melee.py: n° 19 / 21 / 22 with the J'avo's em1000CoC 3E / 42 / 44 at 154 /
    // 156 / 157). The blow where the J'avo's head stops against the obstacle (research/re6_live/env_wall.py): the wall at frame 38 (its
    // head pushed 120 cm on to it), the tables at 46-47 (the head down on the top) and 43-44.
    { "melee.free_re6_common_env_wall", "Exécution contre un mur (RE6)", kRe6MeleeBank, 19, 3000, Reaction::KnockDown, 0.294f,
      0.325f, false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 154 },
    { "melee.free_re6_common_env_table_side", "Exécution contre une barrière, de côté (RE6)", kRe6MeleeBank, 21, 3000,
      Reaction::KnockDown, 0.319f, 0.348f, false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 156 },
    { "melee.free_re6_common_env_table_front", "Exécution contre une table ou une barrière, de face (RE6)", kRe6MeleeBank, 22, 3000,
      Reaction::KnockDown, 0.286f, 0.313f, false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 157 },
    // From a height (user, 2026-11-11: "from below, the enemy facing me" AcA 36, "from below, the enemy's back" AcA 38, "from above" AcA 3A;
    // AcA 3C from above on its back not recorded): n° 15-18 with CoC 36-3C at 150-153. Their blow (research/re6_live/env_wall.py): from
    // below facing, its head on the floor at frame 134 of the J'avo's side; from below from behind, its face on the ledge's top at 94;
    // from above, its head on the floor at 66 (player 1 landing on it).
    { "melee.free_re6_common_env_below_facing", "Exécution par le dessous, ennemi de face (RE6)", kRe6MeleeBank, 15, 3000,
      Reaction::KnockDown, 0.936f, 0.965f, false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 150 },
    { "melee.free_re6_common_env_below_behind", "Exécution par le dessous, ennemi de dos (RE6)", kRe6MeleeBank, 16, 3000,
      Reaction::KnockDown, 0.609f, 0.636f, false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 151 },
    { "melee.free_re6_common_env_above_facing", "Exécution par le dessus, ennemi de face (RE6)", kRe6MeleeBank, 17, 3000,
      Reaction::KnockDown, 0.374f, 0.398f, false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 152 },
    { "melee.free_re6_common_env_above_behind", "Exécution par le dessus, ennemi de dos (RE6)", kRe6MeleeBank, 18, 3000,
      Reaction::KnockDown, 0.374f, 0.398f, false, true, 1.0f, 0.0f, 1.0f, 0.0f, kRe6PairVictim + 153 },
};
constexpr int kThrowMove = 5, kThrow2Move = 12, kThrow2Catch = 20;
constexpr bool SameId(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}
constexpr int CombatIndex(const char* id) {
    for (int k = 0; k < static_cast<int>(std::size(kCombat)); ++k)
        if (SameId(kCombat[k].id, id)) return k;
    return -1;
}
// By its id (2026-11-09: it was the table's last line, and since the RE6 dodges were added after it on 2026-11-08 "Projection de Josh"
// played RE6's rise from the dodge in place).
constexpr int kJoshCounterMove = CombatIndex("melee.free_josh_counter");
constexpr int kRe6CounterMove = CombatIndex("melee.free_re6_counter_leap");
constexpr int kRe6CounterLongMove = CombatIndex("melee.free_re6_counter_leap_long");
// swaps.cpp's melee swap entries name these by their index (kSwapEntries' third field).
static_assert(kRe6CounterMove >= 0 && kRe6CounterLongMove >= 0 && CombatIndex("melee.free_re6_leon_arm_front") == 85 &&
              CombatIndex("melee.free_re6_jake_arm_front") == 86 && CombatIndex("melee.free_re6_ada_arm_front") == 87 &&
              CombatIndex("melee.free_re6_helena_arm_front") == 88 && CombatIndex("melee.free_re6_leon_ground") == 89 &&
              CombatIndex("melee.free_re6_ground_attack_long") == 90 && CombatIndex("melee.free_re6_ground_attack_knife") == 91);
constexpr int kRe6GroundLongMove = 90, kRe6GroundKnifeMove = 91;
// The RE6 melee's moves: the combos by the per-character choice (0 none, 1 Chris, 2 Leon, 3 Helena, 4 Jake, 5 Sherry).
constexpr int kRe6Combo[6][2] = { { -1, -1 },
                                  { CombatIndex("melee.free_re6_chris_combo1"), CombatIndex("melee.free_re6_chris_combo2") },
                                  { CombatIndex("melee.free_re6_leon_combo1"), CombatIndex("melee.free_re6_leon_combo2") },
                                  { CombatIndex("melee.free_re6_helena_combo1"), CombatIndex("melee.free_re6_helena_combo2") },
                                  { CombatIndex("melee.free_re6_jake_combo1"), CombatIndex("melee.free_re6_jake_combo2") },
                                  { CombatIndex("melee.free_re6_common_x41_1"), CombatIndex("melee.free_re6_common_x41_2") } };
constexpr int kRe6Long1 = CombatIndex("melee.free_re6_common_long1"), kRe6Long2 = CombatIndex("melee.free_re6_common_long2");
constexpr int kRe6Sprints[3] = { CombatIndex("melee.free_re6_common_shoulder"), CombatIndex("melee.free_re6_common_jump_kick"),
                                 CombatIndex("melee.free_re6_common_sprint_var") };
constexpr int kRe6Elbows[2] = { CombatIndex("melee.free_re6_common_elbow"), CombatIndex("melee.free_re6_common_elbow_var") };
constexpr int kRe6Tired = CombatIndex("melee.free_re6_common_tired");
constexpr int kRe6CrateHigh = CombatIndex("melee.free_re6_common_crate_high"), kRe6CrateLow = CombatIndex("melee.free_re6_common_crate_low");
static_assert(kRe6Combo[5][1] >= 0 && kRe6Long2 >= 0 && kRe6Sprints[2] >= 0 && kRe6Elbows[1] >= 0 && kRe6Tired >= 0 && kRe6CrateLow >= 0);
// The RE6 melee's own blows leave player 1 vulnerable (user, 2026-11-09: "the RE6 free attack must not make me invincible, I could
// avoid every hit by spamming it"): the combos, the two-handed weapon's, the sprint attacks, the elbows, the tired and crate blows.
constexpr bool Re6MeleeVulnerable(int k) {
    for (int c = 1; c < 6; ++c)
        if (k == kRe6Combo[c][0] || k == kRe6Combo[c][1]) return true;
    return k == kRe6Long1 || k == kRe6Long2 || k == kRe6Sprints[0] || k == kRe6Sprints[1] || k == kRe6Sprints[2] ||
           k == kRe6Elbows[0] || k == kRe6Elbows[1] || k == kRe6Tired || k == kRe6CrateHigh || k == kRe6CrateLow;
}
// The RE6 pairs: swaps.cpp names them by index (130-144); Jake's Ground face down is his Ground's variant (no row of its own).
static_assert(CombatIndex("melee.free_re6_leon_leg_back") == 130 && CombatIndex("melee.free_re6_ada_leg_back") == 144);
constexpr int kRe6JakeGround = CombatIndex("melee.free_re6_jake_ground"), kRe6JakeGroundBelly = CombatIndex("melee.free_re6_jake_ground_belly");
// The RE6 parry's counters (PlayRe6Counter's order: Piers, Chris, Jake, Leon, Ada, the bayonet).
constexpr int kRe6Counters[6] = { CombatIndex("melee.free_re6_counter_piers"), CombatIndex("melee.free_re6_counter_chris"),
                                  CombatIndex("melee.free_re6_counter_jake"), CombatIndex("melee.free_re6_counter_leon"),
                                  CombatIndex("melee.free_re6_counter_ada"), CombatIndex("melee.free_re6_counter_bayonet") };
static_assert(kRe6Counters[0] >= 0 && kRe6Counters[5] >= 0);
constexpr int kRe6Sweep = CombatIndex("melee.free_re6_counter_sweep");
static_assert(kRe6Sweep >= 0);
constexpr bool Re6CounterMove(int k) {
    for (int c : kRe6Counters)
        if (k == c) return true;
    return false;
}
// swaps.cpp names these by index (kSwapEntries, 112-129).
static_assert(CombatIndex("melee.free_re6_chris_arm_front") == 112 && CombatIndex("melee.free_re6_jake_clear_kick") == 129);
constexpr int kRe6CrouchMove = CombatIndex("melee.free_re6_crouch");
constexpr int kRe6SlideTurnMove = CombatIndex("melee.free_re6_slide_turn");
constexpr uint16_t kRe6SlideTurn = 51;  // re6_evade.lmt: one hand, +1 two hands
static_assert(kRe6SlideTurnMove >= 0);
static_assert(CombatIndex("melee.free_re6_chris_leg_back") == 153);  // swaps.cpp's "Leg Back Chris RE6"
static_assert(CombatIndex("melee.free_re6_common_knife_counter") == 154 && CombatIndex("melee.free_re6_common_throat_cut") == 155);
static_assert(kJoshCounterMove >= 0 && kRe6CrouchMove >= 0);
bool ThrowMove(int k) { return k == kThrowMove || k == kThrow2Catch; }
constexpr float kReach = 220.0f, kHeight = 150.0f, kArc = 1.3f;  // cm, cm, radians either side
constexpr uintptr_t kJillVtable = 0xB8EA40, kWeskerVtable = 0xB8BE60;  // uEm83, uEm80 (RVA)
constexpr uintptr_t kFinalWeskerVtable = 0xB8C300;                      // uEm81, the final fight (RVA)
constexpr uintptr_t kLinWeskerVtable = 0xB8C0B0;                        // uEmA2, Lost in Nightmares (RVA)
constexpr uintptr_t kReaperVtable = 0xB7AA88;                          // uEm34 (RVA)
constexpr uintptr_t kExecutionerClass = 0xB748A8, kChainsawClass = 0xB75B20;  // uEm20, uEm22 (RVA)
constexpr uintptr_t kNightmareExecutionerClass = 0xB74E38;  // uEm27, Lost in Nightmares (the Executioner's actions)

struct ActiveMove {
    const CombatMove* move = nullptr;
    uint32_t motion = 0;
    DWORD start = 0;
    bool seen = false;  // its motion has been seen on layer 0
    uintptr_t hit[16] = {};
    int hits = 0;
    bool worn = false;  // its knife's durability worn (RE6's knife moves, once a move)
    uintptr_t only = 0;  // the throw: its Majini alone
    float yaw = 0.0f;    // his facing at the start: the hits' direction (the back kick turns him during the motion)
    int window = 0;      // the hit window in progress (1 / 2)
    bool cut = false;    // its end asked (CombatMove::end)
    bool cue = false;    // its sound-only blow played (SoundOnlyBlowFrame)
};
ActiveMove g_active;  // render thread
std::atomic<bool> g_invulnerable{ false };

void TickStolenWeapon(uintptr_t p) {
    const float at = g_active.move ? StealFrame(g_active.move->id) : -1.0f;
    // Research (user, 2026-11-11: "the weapon stays in the enemy's hand instead of going into Chris's", "I think it fights between the
    // two hands"; traced: the world matrix made by the hooked VA 0x48ABA0 every frame, then moved by VA 0xB540D0's offset): where the
    // weapon, its first joint, player 1's hand and the Majini's hand are, every 100 ms of the hold.
    if (g_stolenFrom && at >= 0.0f) {
        static ULONGLONG told = 0;
        uintptr_t weapon = 0, wj = 0;
        float wpos[3]{}, jpos[3]{}, mine[3]{}, its[3]{};
        if (GetTickCount64() - told > 100 && mem::Read(g_stolenFrom + 0x1B6C, weapon) && weapon &&
            mem::SafeRead(reinterpret_cast<void*>(weapon + 0xD0), wpos, sizeof(wpos))) {
            told = GetTickCount64();
            if (mem::Read(weapon + 0x318, wj) && wj) mem::SafeRead(reinterpret_cast<void*>(wj + 0x80), jpos, sizeof(jpos));
            JointWorldById(p, 8, mine);
            JointWorldById(g_stolenFrom, 8, its);
            float frame = 0.0f;
            mem::Read(p + 0x398, frame);
            LOG("stolen weapon research: frame %.0f, weapon (%.0f %.0f %.0f), its joint 0 (%.0f %.0f %.0f), player 1's hand (%.0f %.0f %.0f) %.0f cm, "
                "the Majini's (%.0f %.0f %.0f) %.0f cm", frame, wpos[0], wpos[1], wpos[2], jpos[0], jpos[1], jpos[2], mine[0], mine[1], mine[2],
                std::hypot(wpos[0] - mine[0], wpos[1] - mine[1], wpos[2] - mine[2]), its[0], its[1], its[2],
                std::hypot(wpos[0] - its[0], wpos[1] - its[1], wpos[2] - its[2]));
        }
    }
    if (at >= 0.0f && g_stolenFrom) {  // its hand: the left one (the other than the Majini's) until StealRightFrame
        float frame = 0.0f;
        uint32_t motion = 0;
        if (mem::Read(p + 0x384, motion) && (motion & 0xFFFF) == (g_active.motion & 0xFFFF) && mem::Read(p + 0x398, frame)) {
            const uint32_t hand = frame < StealRightFrame(g_active.move->id) ? (g_stolenBaseJoint == 8 ? 13u : 8u) : g_stolenBaseJoint;
            if (hand != g_stolenHand) {
                g_stolenHand = hand;
                SetStolenWeaponHand(hand);
                LOG("wesker moves: the stolen weapon in player 1's %s hand at frame %.0f", hand == 8 ? "right" : "left", frame);
            }
        }
    }
    if (at < 0.0f || g_stolenFrom) return;  // taken once, kept to the move's end
    uintptr_t victim = g_pairVictim.load(std::memory_order_relaxed);
    if (!victim) victim = g_pairCorpse.load(std::memory_order_relaxed);  // killed at the blow (the steal's at frame 80)
    uint32_t motion = 0;
    float frame = 0.0f;
    uintptr_t weapon = 0;
    if (!victim || !mem::Read(p + 0x384, motion) || (motion & 0xFFFF) != (g_active.motion & 0xFFFF) || !mem::Read(p + 0x398, frame) ||
        frame < at || !mem::Read(victim + 0x1B6C, weapon) || !weapon)
        return;
    if (g_stolenDrop.weapon) {
        DropStolenWeapon(g_stolenDrop.weapon, g_stolenDrop.majini, g_stolenDrop.feet);
        g_stolenDrop = {};
    }
    g_stolenFrom = victim;
    uint32_t base = 8;
    g_stolenBaseJoint = mem::Read(weapon + 0x1C, base) && base == 13 ? 13 : 8;
    g_stolenHand = frame < StealRightFrame(g_active.move->id) ? (g_stolenBaseJoint == 8 ? 13u : 8u) : g_stolenBaseJoint;
    SetStolenWeaponHand(g_stolenHand);
    SetStolenWeapon(weapon, victim, p);
    const char* cls = UnitClassName(weapon);
    LOG("wesker moves: %s: the weapon of %08X (%s) in player 1's %s hand at frame %.0f", g_active.move->name, static_cast<unsigned>(victim),
        cls ? cls : "?", g_stolenHand == 8 ? "right" : "left", frame);
}
uintptr_t g_caught = 0;  // Jill in the game's throw (render thread)
DWORD g_caughtStart = 0;
bool g_caughtPlayed = false;  // player 1 entered the throw's event action

float YawOf(uintptr_t unit) {
    float q[4]{};
    return mem::SafeRead(reinterpret_cast<void*>(unit + 0x40), q, sizeof(q)) ? 2.0f * std::atan2(q[1], q[3]) : 0.0f;
}

float Wrap(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

bool IsClass(uintptr_t enemy, uintptr_t vtableRva);

// Jill, in either part of her fight: uEm83, then uEm84 once Wesker has left (user, 2026-10-25: the throw never held her
// there, it was taken for another enemy). uEm84's actions are not uEm83's: found by their class names.
bool IsJillUnit(uintptr_t enemy) {
    if (IsClass(enemy, kJillVtable)) return true;
    const char* name = UnitClassName(enemy);
    return name && std::strncmp(name, "uEm84", 5) == 0;
}

// The name of an action object of a unit's table (+0x10D0 count, +0x10DC objects) after its "Act" ("" if none).
const char* ActionTail(uintptr_t unit, uint32_t action) {
    uint32_t count = 0;
    uintptr_t table = 0, object = 0;
    if (!mem::Read(unit + 0x10D0, count) || action >= count || count > 0x200 || !mem::Read(unit + 0x10DC, table) || !table ||
        !mem::Read(table + 4 * action, object) || !object)
        return "";
    const char* name = UnitClassName(object);
    const char* tail = name ? std::strstr(name, "Act") : nullptr;
    return tail ? tail + 3 : "";
}

// Jill's action by its name ("Caught" for cEm83ActCaught / cEm84ActCaught), -1 if none.
int JillAction(uintptr_t jill, const char* tail) {
    uint32_t count = 0;
    if (!mem::Read(jill + 0x10D0, count) || count > 0x200) return -1;
    for (uint32_t i = 0; i < count; ++i)
        if (std::strcmp(ActionTail(jill, i), tail) == 0) return static_cast<int>(i);
    return -1;
}

// Not to be thrown (user 2026-10-22: a throw caught a Majini on the ground): a Majini down, kneeling, falling or in our
// throw (knife.cpp EnemyIsDown, cEm10 actions), Jill knocked down (0x1B), getting up (0x21 / 0x22) or lying after a
// throw (Caught 0x27 / 0x2A); and Jill in an action the throw cannot hold her from (user, 2026-10-25: in a jump the throw
// did not hold her and killed her, held by Sheva it took both out of the room - soft locks): jumps, climbs, hanging,
// the handrail, held or holding, stripped, down, sleeping, appearing, dying.
bool CannotBeThrown(uintptr_t enemy) {
    if (enemy == g_thrownUnit.load(std::memory_order_relaxed)) return true;
    uint32_t action = 0;
    if (IsMajiniEnemy(enemy)) {
        // Its grabs stand up (Catch 0x97, DashCatch 0x98, the holds NeckHang 0x99 / BackHold 0x9B): EnemyIsDown counts
        // every action from 0x8B, so a parried grab got the knife riposte instead of the throw (user, 2026-10-30).
        if (mem::Read(enemy + 0x10E0, action) && (action == 0x97 || action == 0x98 || action == 0x99 || action == 0x9B))
            return false;
        return EnemyIsDown(enemy);
    }
    if (!IsJillUnit(enemy) || !mem::Read(enemy + 0x10E0, action)) return false;
    if (action == 0x80000000) return true;  // in a player's event (held by Sheva, a cutscene move)
    if (IsClass(enemy, kJillVtable) && (action == 0x1B || action == 0x21 || action == 0x22 || action == 0x27 || action == 0x2A))
        return true;
    const char* tail = ActionTail(enemy, action);
    for (const char* busy : { "Jump", "Climb", "Hang", "Handrail", "Caught", "Catch", "Pinion", "Strip", "KnockDown", "Getup",
                              "Sleep", "Appear", "Die", "Roll", "Sliding" })
        if (std::strstr(tail, busy)) return true;
    return false;
}

// A Majini's parasite once out ([host +0x1F9C], an enemy unit of its own on the head, user 2026-10-22: the throw took
// it instead of its Majini).
bool IsParasiteOut(uintptr_t enemy) {
    for (uintptr_t host : actors::Enemies()) {
        uintptr_t plaga = 0;
        if (host != enemy && mem::Read(host + 0x1F9C, plaga) && plaga == enemy && IsMajiniEnemy(host)) return true;
    }
    return false;
}

// The nearest living enemy within the distance (cm), its position in `at` (`throwable`: CannotBeThrown ones and the
// parasites skipped).
uintptr_t NearestEnemy(uintptr_t p, float within, float* at, bool throwable = false) {
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return 0;
    uintptr_t best = 0;
    float bestDist = within * within;
    for (uintptr_t e : actors::Enemies()) {
        int16_t health = 0;
        float ep[3]{};
        if (!mem::Read(e + 0x1364, health) || health <= 0 || !mem::SafeRead(reinterpret_cast<void*>(e + 0x30), ep, sizeof(ep)))
            continue;
        if (throwable && (CannotBeThrown(e) || IsParasiteOut(e))) continue;
        const float dx = ep[0] - pos[0], dz = ep[2] - pos[2], d = dx * dx + dz * dz;
        const float up = ep[1] - pos[1];  // a flying enemy up to 4 m above him (forced_death.cpp)
        if (d < bestDist && up > -kHeight && up < (FlyingEnemy(e) ? 400.0f : kHeight)) {
            bestDist = d;
            best = e;
            for (int k = 0; k < 3; ++k) at[k] = ep[k];
        }
    }
    return best;
}

// The enemy a move from player 1 would meet (the free melee table's back-ups): the nearest within 3 m, any kind.
uintptr_t NearestTarget() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    float at[3]{};
    return p ? NearestEnemy(p, 300.0f, at) : 0;
}

// `other`: the unit the action is about (stored in the action +0xC; 0 = the enemy itself). False when refused.
bool RequestEnemy(uintptr_t enemy, uint32_t action, uintptr_t other = 0) {
    using RequestFn = bool(__fastcall*)(void* self, void* edx, int action, int a, uintptr_t b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(enemy, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return false;
    __try {
        return reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(enemy), nullptr, static_cast<int>(action), 1, other,
                                               -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsClass(uintptr_t enemy, uintptr_t vtableRva) {
    uintptr_t vtable = 0;
    return mem::Read(enemy, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(vtableRva));
}

// true = it reacted (or was already dead).
bool ReactInPlace(uintptr_t p, uintptr_t enemy, int reaction, bool killHere);  // below

void DieLying(uintptr_t unit, const char* why);  // below
std::atomic<uintptr_t> g_pairDying{ 0 };          // killed in a pair: its corpse still playing the pair's side

// The played character's effort at a trainer move's blow and a dodge's start (user, 2026-10-31: "the character's grunts on
// the trainer's moves", then "no effort sound": the game's Unit::PlaySe of request 66 was too faint, as the guard's
// grunts): parry.cpp plays one take of his own request 66 from the extracted waves. "Cris d'effort sur les mouvements du
// trainer" (on by default): its volume, 0 = off.
std::atomic<float> g_gruntGain{ 0.3f };

// Unit::PlaySe (RVA 0x7570E0, thiscall(id, const float* position)): a request of the unit's own se bank (+0x1454).
void PlayUnitSound(uintptr_t unit, int id, const float* at) {
    using PlaySeFn = void(__fastcall*)(void* unit, void* edx, int id, const float* position);
    __try {
        reinterpret_cast<PlaySeFn>(mem::Rva(0x7570E0))(reinterpret_cast<void*>(unit), nullptr, id, at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The knife's hit on an enemy as the game plays it (user, 2026-11-10: "a knife's sound, not a blow's"; the sound log of the knife
// aim's slashes on a Majini: at their hit frame 14, se_wp01 1 by player 1 - the blade's swing, which the RE6 knife moves' motions
// already ask - and se_em10 251 by the Majini, its flesh hit): request 251 of the enemy's own bank, `height` cm above its origin.
void PlayKnifeHit(uintptr_t enemy, float height) {
    float at[3]{};
    if (!enemy || !mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), at, sizeof(at))) return;
    at[1] += height;
    PlayUnitSound(enemy, 251, at);
}

void PlayMoveGrunt(uintptr_t p) {
    const float gain = g_gruntGain.load(std::memory_order_relaxed);
    if (p && gain > 0.0f) PlayEffortGrunt(gain);
}

// The native sounds of a trainer move's blow (user, 2026-10-31: "a kick of Jill's boss must make the impact sounds of a
// native kick of Jill's, Wesker's of his"; the sound request log the same day): at the hit, as the game's moves play them
// from the character's own bank, package 0 - 56 then 60 / 61 for a kick (Arm Back), 56 then 42 / 43 for a punch (Arm
// Front) - Wesker's bank for his moves, Jill's for hers, the played character's otherwise. HUNK's neck break: Chris's
// neck break crack (64 / 65 of se_pl00, played on its victim at frame 88) and a Majini's own groan there (its bank's
// 379, through Unit::PlaySe). Leon's knife slashes: none (the knife's own hit).
void PlayMoveImpact(const CombatMove& move, uintptr_t enemy) {
    const float gain = g_gruntGain.load(std::memory_order_relaxed);
    if (gain <= 0.0f || move.reaction == Reaction::ReculLeg) return;
    if (move.victim == 2) {
        PlayMoveSound("se_pl00", 64, gain);
        PlayMoveSound("se_pl00", 65, gain);
        float at[3]{};
        if (IsMajiniEnemy(enemy) && mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), at, sizeof(at))) {
            at[1] += 140.0f;  // its head
            PlayUnitSound(enemy, 379, at);
        }
        return;
    }
    const int bank = move.bank;
    const char* style = bank == kWeskerBank || bank == kWeskerPlayerBank || std::strncmp(move.id, "melee.free_wesker4", 18) == 0
                            ? "se_pl03"
                        : bank == kJillBank || bank == kJill2ActBank || bank == kJill2AtkBank || bank == kJill2NmlBank ||
                                  bank == kJill1ActBank
                            ? "se_pl02"
                            : "";
    const bool punch = std::strstr(move.id, "palm") != nullptr || std::strstr(move.id, "_tag_") != nullptr ||
                       std::strstr(move.id, "counter_leap") != nullptr;  // RE6's counter: a punch (with the gun, its stock)
    PlayMoveSound(style, 56, gain);
    PlayMoveSound(style, punch ? 42 : 60, gain);
    PlayMoveSound(style, punch ? 43 : 61, gain);
}

// An RE4 pair's side killed at the pair's blow: dead at once in its own death action (a Majini's DeadFaceUp 0x8D /
// DeadFaceDown 0x8E as the pair ends, a puppet's Die), whose first motion is the pair's side from its frame now
// (PairCorpseMotion): the corpse ends the fall the pair plays, kept out of the collisions until then (TickThrown).
int PlayerActionNamed(uintptr_t p, const char* name);  // below
void KillPairVictim(uintptr_t enemy, const CombatMove& move) {
    float frame = 0.0f;
    mem::Read(enemy + 0x398, frame);
    const DWORD now = GetTickCount();
    g_pairCorpseFrame = frame;
    g_pairCorpseTick = now ? now : 1;
    g_pairCorpseVictim = move.victim;
    g_pairCorpse = enemy;
    g_pairDying = enemy;
    g_pairVictim = 0;  // the corpse keeps the pair's last pose: nothing to put under it
    bool asked = false;
    if (IsMajiniEnemy(enemy)) {
        asked = RequestEnemy(enemy, PairEndsFaceDown(move.victim) ? 0x8E : 0x8D);
    } else if (enemy == g_puppet.load() && g_puppetClass.load()) {
        const int die = g_puppetClass.load()->die;
        DieLying(enemy, "killed by an RE4 pair");  // its lying motion: the pair's side (kPuppetMotions[1])
        g_puppetOwn = true;
        asked = RequestEnemy(enemy, static_cast<uint32_t>(die));
        g_puppetOwn = false;
    }
    LOG("wesker moves: %s kills %08X at its frame %.0f (its death %s)", move.name, static_cast<unsigned>(enemy), frame,
        asked ? "asked" : "refused");
}

// The leaping enemies (user, 2026-11-09: RE6's counter for them): dogs (uEm30), lickers (uEm32), spider parasites (uEm0b), flying
// parasites (Kipepeo, uEm0c). Their reaction to the trainer's blows, chosen by their actions' names (their tables were not read yet:
// none of them in the stage of the day): a spider falls as at the end of its hold (AtkCatchEnd, the parry's), the others their
// strongest hit reaction found (DmgBlow, Blow, DmgFly, DmgBig, DmgL, DmgS, Dmg); killed, their own death (as forced_death.cpp
// DeathAction: Die, DieLie, another Die not bomb / ice / fire). The Kipepeo alive keeps knife.cpp's (FlyingParasiteReact).
bool LeapEnemy(uintptr_t enemy) {
    const char* cls = enemy ? UnitClassName(enemy) : nullptr;
    if (!cls) return false;
    for (const char* leaper : { "uEm30", "uEm31", "uEm32", "uEm0b", "uEm0c" })
        if (std::strcmp(cls, leaper) == 0) return true;
    return false;
}

int ActionStartingWith(uintptr_t unit, std::initializer_list<const char*> names) {
    uint32_t count = 0;
    if (!mem::Read(unit + 0x10D0, count) || count > 0x200) return -1;
    for (const char* name : names)
        for (uint32_t i = 0; i < count; ++i)
            if (std::strncmp(ActionTail(unit, i), name, std::strlen(name)) == 0) return static_cast<int>(i);
    return -1;
}

int OwnDeathAction(uintptr_t unit) {
    uint32_t count = 0;
    if (!mem::Read(unit + 0x10D0, count) || count > 0x200) return -1;
    int lying = -1, other = -1;
    for (uint32_t i = 0; i < count; ++i) {
        const char* tail = ActionTail(unit, i);
        if (std::strncmp(tail, "Die", 3) != 0) continue;
        if (std::strcmp(tail, "Die") == 0) return static_cast<int>(i);
        if (lying < 0 && std::strncmp(tail, "DieLie", 6) == 0) lying = static_cast<int>(i);
        if (other < 0 && !std::strstr(tail, "Bomb") && !std::strstr(tail, "Ice") && !std::strstr(tail, "Fire")) other = static_cast<int>(i);
    }
    return lying >= 0 ? lying : other;
}

// The RE6 melee's reactions on a Majini (user, 2026-11-09: "the first blow plays a hit received without a prompt; the second, stronger,
// opens a prompt I choose; either one on an enemy already staggered (by a slide...) opens a finisher"; the back elbow "a reaction I
// choose, kneel + Finisher by default"; the sprint attack pushes the enemy back with a chosen prompt; the blow without stamina "no
// prompt, a hit received to interrupt"). ForceMajiniState's states, chosen in Mêlée libre > RE6 (commun): the second blow (10 arm hit +
// Arm), a staggered Majini (18 kneel + Finisher), the elbow (18), the sprint attack (5 recoil + Arm). The first Majini of a blow only
// (the arm prompts follow one enemy), the others a flinch; one lying only takes the damage.
std::atomic<int> g_re6MeleeState[4] = { 10, 18, 18, 5 };

bool Re6Staggered(uintptr_t enemy);  // below: a stagger that opens melee (not the first blow's flinch)

bool Re6MeleeReaction(uintptr_t enemy, Reaction reaction) {
    if (reaction == Reaction::Re6Counter) return ForceMajiniState(enemy, Re6ParryStateNow()) || MajiniFlinch(enemy);
    if (EnemyOnGround(enemy)) return true;
    uint32_t action = 0;
    mem::Read(enemy + 0x10E0, action);
    // The first blow's own flinch is no stagger (user, 2026-11-09: "the second attack must always open the Arm Front": it found the
    // first one's flinch and gave the staggered state); the second blow always its chosen state.
    const bool staggered = reaction != Reaction::Re6Strong && Re6Staggered(enemy);
    if (reaction == Reaction::Re6Tired || g_active.hits > 1 || (reaction == Reaction::Re6Light && !staggered)) return MajiniFlinch(enemy);
    int state = staggered                         ? g_re6MeleeState[1].load()
                : reaction == Reaction::Re6Elbow  ? g_re6MeleeState[2].load()
                : reaction == Reaction::Re6Sprint ? g_re6MeleeState[3].load()
                                                  : g_re6MeleeState[0].load();
    state = std::clamp(state, 0, 19);
    constexpr int kStumbleOfArm[5] = { 1, 5, 7, 8, 4 };  // a chosen arm hit on one already in it: the stumble, same prompts
    if (action == 0x6E && state >= 9 && state <= 13) state = kStumbleOfArm[state - 9];
    const bool forced = ForceMajiniState(enemy, state);
    LOG("re6 melee: %08X %s (action %02X): state %d %s", static_cast<unsigned>(enemy), staggered ? "staggered" : "standing", action,
        state, forced ? "forced" : "refused");
    return forced || MajiniFlinch(enemy);
}

bool LeapReaction(uintptr_t enemy, bool alive) {
    const char* cls = UnitClassName(enemy);
    const int action = !alive                                    ? OwnDeathAction(enemy)
                       : cls && std::strcmp(cls, "uEm0b") == 0 ? ActionStartingWith(enemy, { "AtkCatchEnd" })
                       : ActionStartingWith(enemy, { "DmgBlow", "Blow", "DmgFly", "DmgBig", "DmgL", "DmgS", "Dmg" });
    const bool asked = action >= 0 && RequestEnemy(enemy, static_cast<uint32_t>(action));
    LOG("wesker moves: leaping enemy %s %08X %s: %s %X %s", cls ? cls : "?", static_cast<unsigned>(enemy), alive ? "hit" : "killed",
        action >= 0 ? ActionTail(enemy, static_cast<uint32_t>(action)) : "(no action found)", action, asked ? "asked" : "refused");
    return asked;
}

// The RE6 slide's blow (user, 2026-11-09: "the RE6 slide must deal small damage, like the Arm Front, and give the enemies two reactions: a
// normal enemy plays a hit on the body and the prompt chosen by a dropdown appears; an enemy already staggered (by a quick shot, a shot that
// staggers or a combo with Arm Front) gets a prompt towards another action, chosen too; Arm Front by default, Finisher for the second"; "no
// option, it is part of the RE6 slide"; then "let me choose the Majini's reaction too, as for the parry: open to the finisher, on its knees as
// after a leg shot, as RE6's enemies"). The trainer blow method (TickSlideBlow): Chris's Arm Front power (his hook, 300), a kick's impact, its
// own hit list per slide. The reaction (Strike, Reaction::Slide): the first Majini it hits in a slide gets the chosen ForceMajiniState state (the
// parry's list and the kneels 14..18; by default the arm hit + Arm, the kneel + Finisher); a chosen arm hit on one already in it becomes the
// stumble with the same prompts (its prompts are opened by the trainer once it starts); the others a flinch; one lying only takes the damage.
const CombatMove kSlideBlow = { "melee.free_re6_slide", "Glissade (RE6)", kRe6EvadeBank, 4, 300, Reaction::Slide, 1.0f, 0.0f, false, false };
std::atomic<int> g_slideState[2] = { 10, 18 };  // normal / staggered enemy: ForceMajiniState's state (10 arm hit + Arm, 18 kneel + Finisher)
struct SlideBlow {
    ULONGLONG seen = 0;  // a slide seen playing (whole body or under the aim) at that time
    bool swung = false;  // its grunt played
    bool prompted = false;  // a Majini got its prompt
    uintptr_t hit[16] = {};
    int hits = 0;
};
SlideBlow g_slideBlow;  // render thread

bool SlideReaction(uintptr_t enemy) {
    uint32_t action = 0;
    mem::Read(enemy + 0x10E0, action);
    if (EnemyOnGround(enemy)) {
        LOG("re6 slide: %08X lying (action %02X), the damage only", static_cast<unsigned>(enemy), action);
        return true;
    }
    if (g_slideBlow.prompted) return MajiniFlinch(enemy);
    const bool staggered = Re6Staggered(enemy);  // a flinch (DmgHead without prompt) is none
    int state = std::clamp(g_slideState[staggered ? 1 : 0].load(std::memory_order_relaxed), 0, 19);
    // The arm hit's prompts are opened by the trainer once it starts: on one already in it, the stumble with the same prompts.
    constexpr int kStumbleOfArm[5] = { 1, 5, 7, 8, 4 };
    if (action == 0x6E && state >= 9 && state <= 13) state = kStumbleOfArm[state - 9];
    g_slideBlow.prompted = ForceMajiniState(enemy, state);
    LOG("re6 slide: %08X %s (action %02X): state %d %s", static_cast<unsigned>(enemy), staggered ? "staggered" : "normal", action, state,
        g_slideBlow.prompted ? "forced" : "refused");
    return g_slideBlow.prompted || MajiniFlinch(enemy);
}

void EndPairForBlowOff(uintptr_t enemy);  // below
float g_blowDir[2] = {};  // render thread: EndPairForBlowOff's throw direction (world, unit)
bool g_blowDirOk = false;

bool Strike(uintptr_t p, uintptr_t enemy, const CombatMove& move) {
    int16_t health = 0;
    if (!mem::Read(enemy + 0x1364, health) || health <= 0) return true;
    const bool blowOff = move.victim >= 0 && Re6PairBlowsOff(move.id) && enemy == g_thrownUnit.load(std::memory_order_relaxed) &&
                         IsMajiniEnemy(enemy);
    if (blowOff) EndPairForBlowOff(enemy);  // Re6PairBlowsOff: the game's blow-off from here
    // The free melee table (melee_tuning.cpp): its damage, critical, one-hit kill and reaction (Jill's second throw: its
    // catch takes the throw's). A critical replays the game's head blow on a Majini that has one, which it kills; x5
    // damage on the others.
    MoveTuning tuning;
    const bool tuned = GetMoveTuning(&move == &kCombat[kThrow2Catch] ? kCombat[kThrow2Move].id : move.id, tuning) &&
                       !tuning.HitsDefault();
    int moveDamage = move.damage;
    if (tuned && moveDamage > 0) {
        moveDamage = static_cast<int>(static_cast<float>(moveDamage) * tuning.damage + 0.5f);
        // A pair's victim: its head blown when it hits the floor (QueueHeadBlow's onGround; user, 2026-11-10, Chris's RE6 Head Flash).
        // Only the throws (user, 2026-11-11: "the head must blow up on the floor for the throws, as Chris's Head Flash; for the blows,
        // at the impact" - the armed-zombie counters and the steal had it at the fall): the Head Flashes, Jake's Grounds, the suplex,
        // Josh's Finisher Front and Jill's throws on the floor; every other pair (Leg Backs, counters, the neck break...) at the blow.
        const bool pairThrow = move.reaction == Reaction::Thrown || std::strstr(move.id, "head_flash") != nullptr ||
                               std::strstr(move.id, "jake_ground") != nullptr || std::strstr(move.id, "suplex") != nullptr ||
                               std::strcmp(move.id, "melee.free_josh_counter") == 0;
        if (tuning.critical >= 0.0f && RollPercent(tuning.critical))
            moveDamage = QueueHeadBlow(enemy, move.victim >= 0 && pairThrow) ? health : moveDamage * 5;
        if (tuning.kill) moveDamage = health;
        moveDamage = std::clamp(moveDamage, 0, 32767);
    }
    // "Corps à corps libre : le device de Jill au lieu de sa vie" (jill_device.cpp, user 2026-10-26): her health kept.
    const int device = moveDamage > 0 ? JillDeviceMeleeDamage(enemy) : 0;
    if (device) DamageJillDevice(enemy, device, move.name);
    const bool kept = device || JillKeepsHealth(enemy);
    if (enemy == g_jillVictim.load(std::memory_order_relaxed)) {  // in Chris's side of her throw: no reaction of hers
        const int16_t left = kept ? health : static_cast<int16_t>(health > moveDamage ? health - moveDamage : 1);
        mem::Write(enemy + 0x1364, left);
        LOG("wesker moves: %s hits Jill %08X: health %d -> %d", move.name, static_cast<unsigned>(enemy), health, left);
        return true;
    }
    const int16_t damage = LinWeskerDamage(enemy, static_cast<int16_t>(moveDamage));
    int16_t left = WeskerKeepsHealth(enemy) || kept ? health : static_cast<int16_t>(health > damage ? health - damage : 0);
    // Never Jill's last health point (user, 2026-10-25: at 0 from these moves she fought on, no more hit by them - the
    // trainer skips enemies at 0 - nor in the state the game's own damage takes her to): the game's damage ends her.
    if (left < 1 && IsJillUnit(enemy)) left = 1;
    const bool thrown =
        (move.reaction == Reaction::Thrown || move.victim >= 0) && enemy == g_thrownUnit.load(std::memory_order_relaxed);
    // No death of his in these fights (the final one ends by its own events).
    const bool wesker =
        IsClass(enemy, kWeskerVtable) || IsClass(enemy, kFinalWeskerVtable) || IsClass(enemy, kLinWeskerVtable);
    if (thrown && !left && wesker) left = 1;
    if (tuned && !left && wesker) left = 1;  // the table's kill leaves him 1 health, as the neck break
    // An RE4 pair kills at its blow (user, 2026-10-31: "Chris's neck break kills the enemy when the neck turns, then the
    // dead enemy falls; HUNK's dies after its fall, much less natural, the parasites above all"): dead now, its death
    // action going on with the pair's side as its corpse (KillPairVictim).
    const bool pairKill = thrown && !left && move.victim >= 0 && !Re6PairTakesOff(move.id);  // a take-off's: when it lands
    if (!left) NoteTrainerMeleeKill(enemy);  // its Mercenaries time bonus, as a native bare-hand kill's
    // A lethal throw kills once the enemy lies (TickThrown): at 0 health in the air it stood up to die as if shot.
    if (thrown && !left && !pairKill) g_thrownLethal = true;
    mem::Write(enemy + 0x1364, thrown && !left && !pairKill ? static_cast<int16_t>(1) : left);
    if (pairKill) KillPairVictim(enemy, move);
    uint8_t pad = 0;
    if (mem::Read(p + 0x2D9C, pad)) mem::Write<int32_t>(enemy + 0x136C, pad);
    bool reacted = false;
    if (thrown) {
        reacted = true;  // already thrown: its action ends lying
    } else if (blowOff) {
        // Always the straight one (user, 2026-11-10: "it must always be the right one, not random"), along RE6's throw (user: "sometimes it
        // does not go the right way: I throw it towards a door and it flies to the right" - it was blown away from player 1, and Sherry
        // swings it round her: RE6 throws it straight ahead of her, its root's travel): faced against that way (a front hit, no turning
        // round), its flights made 0x52A / 0x52B (WeskerMoveMotion).
        float at[3]{}, me[3]{};
        reacted = false;
        if (mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), at, sizeof(at)) && mem::SafeRead(reinterpret_cast<void*>(p + 0x30), me, sizeof(me))) {
            float dx = g_blowDir[0], dz = g_blowDir[1];
            if (!g_blowDirOk) {  // no travel known: away from player 1
                const float len = std::max(1.0f, std::hypot(at[0] - me[0], at[2] - me[2]));
                dx = (at[0] - me[0]) / len, dz = (at[2] - me[2]) / len;
            }
            const float back = std::atan2(-dx, -dz);
            const float quat[4] = { 0.0f, std::sin(back * 0.5f), 0.0f, std::cos(back * 0.5f) };
            mem::SafeWrite(reinterpret_cast<void*>(enemy + 0x40), quat, sizeof(quat));
            const float from[3] = { at[0] - dx * 100.0f, at[1], at[2] - dz * 100.0f };
            const DWORD now = GetTickCount();
            g_straightBlowTick = now ? now : 1;
            g_straightBlowUnit = enemy;
            reacted = MajiniBlowOffFrom(enemy, from, 1);
            LOG("wesker moves: %08X blown off along the throw (%.2f %.2f)%s", static_cast<unsigned>(enemy), dx, dz,
                g_blowDirOk ? "" : " (away from player 1)");
        }
    } else if (left > 0 && FlyingParasiteReact(enemy, move.name)) {
        reacted = true;  // a flying parasite (Kipepeo): it lets go of a player it holds, else it is blown away
    } else if (left > 0 && (move.reaction == Reaction::Re6Light || move.reaction == Reaction::Re6Strong || move.reaction == Reaction::Re6Elbow ||
                            move.reaction == Reaction::Re6Sprint || move.reaction == Reaction::Re6Tired) &&
               !(IsMajiniEnemy(enemy) && !BigEnemyClass(enemy))) {
        // RE6's series blows (the two of each character's series, the two-handed weapon's, the sprint's, the elbows, the tired one) make only
        // the ordinary Majini react (user, 2026-11-11: "they should not make the Executioner, the giant Majini or the other special enemies
        // react: only the normal Majini"): the damage alone on the others; a killing one keeps its reaction (below), so that it dies.
        reacted = true;
        LOG("wesker moves: %s on the special enemy %s %08X: damage only", move.name, UnitClassName(enemy), static_cast<unsigned>(enemy));
    } else if (tuned && tuning.reaction != kReactionGame && move.reaction != Reaction::Thrown) {
        reacted = ReactInPlace(p, enemy, tuning.reaction, true);
    } else if (IsMajiniEnemy(enemy)) {
        if (move.reaction == Reaction::ReculLeg) reacted = left > 0 ? ForceMajiniState(enemy, 7) : MajiniDieNow(enemy);
        // A killing RE6 blow (user, 2026-11-09: "they play the normal reaction then die seconds later: as the counter, flying away when
        // they die, the right reaction when they survive"): a standing Majini blown off (dead when it lands), one lying dies there.
        else if (left <= 0 && (move.reaction == Reaction::Slide || move.reaction == Reaction::Recoil ||
                               move.reaction == Reaction::Re6Light || move.reaction == Reaction::Re6Strong ||
                               move.reaction == Reaction::Re6Elbow || move.reaction == Reaction::Re6Sprint ||
                               move.reaction == Reaction::Re6Tired || move.reaction == Reaction::Re6Trip ||
                               move.reaction == Reaction::Re6Counter))
            reacted = EnemyOnGround(enemy) ? MajiniDieNow(enemy) : MajiniBlowOff(enemy, p);
        else if (move.reaction == Reaction::Slide) reacted = left > 0 ? SlideReaction(enemy) : MajiniDieNow(enemy);
        else if (move.reaction == Reaction::Recoil) reacted = left > 0 ? ForceMajiniState(enemy, 1) : MajiniDieNow(enemy);  // an Arm Front's
        else if (move.reaction == Reaction::Re6Light || move.reaction == Reaction::Re6Strong || move.reaction == Reaction::Re6Elbow ||
                 move.reaction == Reaction::Re6Sprint || move.reaction == Reaction::Re6Tired)
            reacted = left > 0 ? Re6MeleeReaction(enemy, move.reaction) : MajiniDieNow(enemy);
        else if (move.reaction == Reaction::Re6Counter) reacted = left > 0 ? Re6MeleeReaction(enemy, move.reaction) : MajiniBlowOff(enemy, p);
        else if (move.reaction == Reaction::Re6Trip) reacted = left > 0 ? ForceMajiniState(enemy, 19) : MajiniDieNow(enemy);
        // A Ground: lying, its hit on the ground (DmgAtFaceUp 0x82 / DmgAtFaceDown 0x83), killed its death lying; standing, a flinch.
        else if (move.reaction == Reaction::Ground)
            reacted = left <= 0                ? MajiniDieNow(enemy)
                      : EnemyOnGround(enemy) ? RequestEnemy(enemy, LyingFaceDown(enemy) ? 0x83 : 0x82)
                                             : MajiniFlinch(enemy);
        else reacted = MajiniBlowOff(enemy, p);
    } else if (IsClass(enemy, kJillVtable)) {
        reacted = RequestEnemy(enemy, move.reaction == Reaction::KnockDown || move.reaction == Reaction::Thrown ? 0x1B : 0x19);
    } else if (IsJillUnit(enemy)) {  // uEm84: the same reactions by name
        const int reaction =
            JillAction(enemy, move.reaction == Reaction::KnockDown || move.reaction == Reaction::Thrown ? "DmgKnockDown" : "DmgBlowL");
        reacted = reaction >= 0 && RequestEnemy(enemy, static_cast<uint32_t>(reaction));
    } else if (wesker) {  // the final and the DLC's ignored the free melee (user, 2026-10-26): DmgS as uEm80
        reacted = RequestEnemy(enemy, 0x58);
    } else if (IsClass(enemy, kReaperVtable)) {
        // The Reaper (uEm34): Blown 0x21 for the blows that throw down, Damage 0x18 for the others.
        reacted = RequestEnemy(enemy, move.reaction == Reaction::ReculLeg ? 0x18 : 0x21);
    } else if (IsClass(enemy, kExecutionerClass) || IsClass(enemy, kNightmareExecutionerClass)) {
        // The Executioner ignored Wesker's kicks (user, 2026-10-23): its hit reaction cEm20ActDmg 0x1D, kneeling
        // (DmgKneelDown 0x1C) under the sweep.
        reacted = RequestEnemy(enemy, move.reaction == Reaction::KnockDown ? 0x1C : 0x1D);
    } else if (IsClass(enemy, kChainsawClass)) {
        // The chainsaw Majini (uEm22): its hit reaction cEm22ActDmg 0x17, its leg stagger 0x1A under the sweep.
        reacted = RequestEnemy(enemy, move.reaction == Reaction::KnockDown ? 0x1A : 0x17);
    } else if (LeapEnemy(enemy)) {
        reacted = LeapReaction(enemy, left > 0);
    }
    if (reacted && !thrown && IsJillUnit(enemy)) {  // her getup after it (JillGetupRequest)
        const DWORD tick = GetTickCount();
        g_jillKnockedTick = tick ? tick : 1;
        g_jillKnocked = enemy;
    }
    LOG("wesker moves: %s hits %08X: health %d -> %d%s", move.name, static_cast<unsigned>(enemy), health, left,
        reacted ? "" : " (no reaction for this enemy)");
    return reacted;
}

// A unit's collision (its object +0x1944, flag 0x400), as cEm83ActCaught's init (off) and exit (back).
void CollisionOff(uintptr_t unit) {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, int)>(mem::Rva(0x44D2A0))(reinterpret_cast<void*>(unit), nullptr, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void CollisionBack(uintptr_t unit) {
    __try {
        if (!reinterpret_cast<bool(__fastcall*)(void*, void*)>(mem::Rva(0x44D310))(reinterpret_cast<void*>(unit), nullptr)) {
            reinterpret_cast<void(__fastcall*)(void*, void*, int)>(mem::Rva(0x44D2A0))(reinterpret_cast<void*>(unit), nullptr, 1);
            reinterpret_cast<void(__fastcall*)(void*, void*)>(mem::Rva(0x44D2D0))(reinterpret_cast<void*>(unit), nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The thrown Majini's collision and speed back, its death if the throw killed it (`alive`: still in the enemy list).
void ReleaseThrown(bool alive) {
    const uintptr_t e = g_thrownUnit.load(std::memory_order_relaxed);
    if (e && alive) {
        CollisionBack(e);
        if (g_thrownSpeed > 0) mem::Write(e + actors::off::Speed, g_thrownSpeed);
        // Jill's thrown motion ends on the back (user: "always on their back"): dead where it lies DeadFaceUp 0x8D
        // (user-verified). Alive it keeps the game's RiseFaceDown 0x6B, which lets it lie a while (user: RiseFaceUp
        // 0x6A stood it up at once, no finisher on the ground).
        uint32_t action = 0;
        mem::Read(e + 0x10E0, action);
        if (g_thrownLethal) {
            // An RE4 pair's side may end face down (DeadFaceDown 0x8E then, its own motion; FaceUpTwin leaves it).
            const bool faceDown = LyingFaceDown(e);
            mem::Write<int16_t>(e + 0x1364, 0);
            RequestEnemy(e, faceDown ? 0x8E : 0x8D);
            LOG("wesker moves: thrown Majini %08X killed lying on its %s (action was %X)", static_cast<unsigned>(e),
                faceDown ? "belly" : "back", action);
        }
    }
    g_thrownSpeed = 0.0f;
    g_thrownLethal = false;
    g_thrownUnit = 0;
}

void EndPuppet(uintptr_t unit, const char* why) {
    g_puppet = 0;
    g_puppetPhase = 0;
    g_pairPuppet = false;
    g_thrownLethal = false;  // its death was requested here, not ReleaseThrown's Majini one
    LOG("wesker moves: %s %08X out of Jill's throw (%s)", g_puppetClass.load()->name, static_cast<unsigned>(unit), why);
}

// Its death (asked by its AI or ours) plays Jill's lying motion; the SetMotion hook ends the throw once it is set.
void DieLying(uintptr_t unit, const char* why) {
    g_thrownLethal = false;
    const DWORD now = GetTickCount();
    g_puppetSince = now ? now : 1;
    g_puppetMotion = kPuppetMotions[1];
    g_puppetPhase = 4;
    LOG("wesker moves: %s %08X dies lying (%s)", g_puppetClass.load()->name, static_cast<unsigned>(unit), why);
}

bool DrivenPhaseDone(uintptr_t e, float margin);  // below
bool DrivenGetupOver(uintptr_t e, float margin);  // below

// The Executioner's (or the chainsaw Majini's) requests while it plays Jill's throw (game thread; ours pass): refused while the phase's motion
// plays, once it ended the next phase's Dmg in place of what was asked (its AI asks as soon as Dmg ends). A death asked
// meanwhile (a shot) plays lying; after the getup, or 5 s into a phase whose motion never came, its own again.
bool PuppetRequest(uintptr_t enemy, int* action) {
    if (!enemy || enemy != g_puppet.load() || g_puppetOwn.load()) return true;
    const int phase = g_puppetPhase.load();
    if (phase == 4) return true;
    const PuppetClass& puppetClass = *g_puppetClass.load();
    if (puppetClass.driven) {
        // TickDrivenPuppet moves it on; once its getup is over its AI's next action goes through (it asks once), or
        // the action planned after a forced getup in its place.
        if (phase != 3 || !DrivenGetupOver(enemy, 3.0f)) return false;
        const int then = g_puppetThen.exchange(-1);
        EndPuppet(enemy, then >= 0 ? "up again, its planned action" : "up again, its AI's next action");
        if (then >= 0) {
            LOG("wesker moves: %08X asked %X, %X in its place", static_cast<unsigned>(enemy), *action, then);
            *action = then;
            WatchKneel(enemy, then);
        }
        return true;
    }
    if (*action >= puppetClass.deathFirst && *action <= puppetClass.deathLast) {
        DieLying(enemy, "died");
        return true;
    }
    uint32_t motion = 0;
    uint8_t flags = 0;
    float frame = 0, length = 0;
    mem::Read(enemy + 0x384, motion);
    mem::Read(enemy + 0x388, flags);
    mem::Read(enemy + 0x398, frame);
    mem::Read(enemy + 0x39C, length);
    const bool done = (flags & 4) || (length > 0 && frame >= length - 1.0f);
    if (phase >= 2 && phase <= 3 && (motion & 0xFFFF) == kPuppetMotions[phase - 2] && done) {
        // The previous phase's motion still there: our Dmg was refused, asked again in this request's place.
        g_puppetMotion = kPuppetMotions[phase - 1];
        *action = puppetClass.dmg[phase - 1];
        return true;
    }
    const bool ours = phase >= 1 && phase <= 3 && (motion & 0xFFFF) == kPuppetMotions[phase - 1];
    const bool ended = ours && done;
    // 5 s for a phase whose motion never came; 15 s while its motion plays (user, 2026-11-10: "the Executioner teleports standing after the
    // knife counter" - its side, CoC 5A, is 367 frames: cut at 5 s as "phase too long", its AI back before the getup). Never once the
    // phase ended (then "it still teleports": the side ended at 5.8 s - Mercenaries plays enemies a little faster - and its end was
    // taken as too long).
    const bool late = !ended && static_cast<int32_t>(GetTickCount() - g_puppetSince.load()) > (ours ? 15000 : 5000);
    if (!ended && !late) return false;
    if (phase >= 3 || late) {
        EndPuppet(enemy, late ? "phase too long" : "up again");
        return true;
    }
    if (phase == 1 && g_thrownLethal) {
        mem::Write<int16_t>(enemy + 0x1364, 0);
        *action = puppetClass.die;
        DieLying(enemy, "killed lying");
        return true;
    }
    const DWORD now = GetTickCount();
    g_puppetSince = now ? now : 1;
    const int next = phase == 1 && g_pairPuppet.load() ? 2 : phase;  // an RE4 pair's side lies already: its getup
    // A pair's side ending on its belly (user, 2026-11-10: "after the RE6 knife counter the Executioner falls on its belly and has no getup
    // from the belly: give it the Majini's, as the one from the back after Jill's throw"): the Majini's RiseFaceDown twin em10_g05 0x55
    // instead of RiseFaceUp's 0x54, by how the body lies now (PairLyingRealign puts it under the body).
    if (next == 2 && g_pairPuppet.load() && !puppetClass.ownGetup && ((kPuppetMotions[2] >> 8) & 0xFF) == static_cast<uint32_t>(kMajiniRiseBank)) {
        const bool belly = LyingFaceDown(enemy);
        kPuppetMotions[2] = (static_cast<uint32_t>(kMajiniRiseBank) << 8) | (belly ? 0x55u : 0x54u);
        LOG("wesker moves: %s %08X gets up from its %s (a Majini's em10_g05 %X)", puppetClass.name, static_cast<unsigned>(enemy),
            belly ? "belly" : "back", belly ? 0x55 : 0x54);
    }
    g_puppetMotion = kPuppetMotions[next];
    g_puppetPhase = next + 1;
    LOG("wesker moves: %s %08X phase %d (asked %X, Dmg %X)", puppetClass.name, static_cast<unsigned>(enemy), next + 1,
        *action, puppetClass.dmg[next]);
    *action = puppetClass.dmg[next];
    return true;
}

// A driven puppet (Wesker): its thrown motion to its last frame, then its getup at once, then its AI; 8 s at most. Its
// AI asked once when its Dmg ended (refused then) and never again (user, 2026-10-25: "frozen once standing, until I shoot
// him"): his Wait 0x00 (cEm80ActWait) at the end gives it back. (The thrown motion lasting 1.26 s, not 2.43 s: "Vitesse du
// corps à corps" x1.93 speeds up the enemy of a synchronised move too, user.)
// The phase's motion at its end (within `margin` frames, or its end flag for the getup's AI request).
bool DrivenPhaseDone(uintptr_t e, float margin) {
    const int phase = g_puppetPhase.load();
    uint32_t motion = 0;
    uint16_t flags = 0;
    float frame = 0, length = 0;
    if (phase < 1 || phase > 3 || !mem::Read(e + 0x384, motion) || (motion & 0xFFFF) != kPuppetMotions[phase - 1]) return false;
    mem::Read(e + 0x388, flags);
    mem::Read(e + 0x398, frame);
    mem::Read(e + 0x39C, length);
    return (length > 0 && frame >= length - margin) || (margin > 1.0f && (flags & 4));
}

// Its getup over: at its end, or another motion in its place after 1 s (logged 2026-10-25, a forced getup: its end was
// never seen, the 8 s limit ended it, then the Wait; he fled 17 s after his fall, user: "he left").
bool DrivenGetupOver(uintptr_t e, float margin) {
    if (g_puppetPhase.load() != 3) return false;
    if (DrivenPhaseDone(e, margin)) return true;
    uint32_t motion = 0;
    return mem::Read(e + 0x384, motion) && (motion & 0xFFFF) != kPuppetMotions[2] &&
           static_cast<int32_t>(GetTickCount() - g_puppetSince.load()) > 1000;
}

void ResumeDrivenPuppet(uintptr_t e, const char* why) {
    const int then = g_puppetThen.exchange(-1);
    EndPuppet(e, why);
    if (then >= 0) {
        const bool accepted = RequestEnemy(e, static_cast<uint32_t>(then));
        if (accepted) WatchKneel(e, then);
        LOG("wesker moves: %08X its planned action %X %s", static_cast<unsigned>(e), then, accepted ? "accepted" : "refused");
        if (accepted) return;
    }
    LOG("wesker moves: %08X back to its AI (Wait %s)", static_cast<unsigned>(e), RequestEnemy(e, 0x00) ? "accepted" : "refused");
}

// The final Wesker kneeling after a neck break (Hiza 0x67): once its init ran (his kneeling motion em80 0x72 set) its
// loops count (the action object +0xD0) raised to kKneelLoops; 3 s at most.
void TickKneel() {
    const uintptr_t e = g_kneeling.load();
    if (!e) return;
    if (static_cast<int32_t>(GetTickCount() - g_kneelingTick.load()) > 3000) {
        g_kneeling = 0;
        LOG("wesker moves: %08X never knelt", static_cast<unsigned>(e));
        return;
    }
    uint32_t action = 0, motion = 0, count = 0;
    uintptr_t table = 0, hiza = 0;
    if (!mem::Read(e + 0x10E0, action) || action != 0x67 || !mem::Read(e + 0x384, motion) || (motion & 0xFFFF) != 0x72 ||
        !mem::Read(e + 0x10D0, count) || count <= 0x67 || !mem::Read(e + 0x10DC, table) || !table ||
        !mem::Read(table + 4 * 0x67, hiza) || !hiza)
        return;
    g_kneeling = 0;
    int32_t loops = 0;
    mem::Read(hiza + 0xD0, loops);
    if (loops < kKneelLoops) mem::Write<int32_t>(hiza + 0xD0, kKneelLoops);
    LOG("wesker moves: %08X on his knees for the injection, %d loops (was %d)", static_cast<unsigned>(e),
        loops < kKneelLoops ? kKneelLoops : loops, loops);
}

void TickDrivenPuppet() {
    const uintptr_t e = g_puppet.load();
    const PuppetClass& puppetClass = *g_puppetClass.load();
    const int phase = g_puppetPhase.load();
    if (!e || !puppetClass.driven || phase < 1 || phase > 3) return;
    static DWORD upSince = 0;  // its getup over, its AI's request awaited
    if (static_cast<int32_t>(GetTickCount() - g_puppetSince.load()) > 8000) {
        ResumeDrivenPuppet(e, "phase too long");
        return;
    }
    if (phase == 3 ? !DrivenGetupOver(e, 1.0f) : !DrivenPhaseDone(e, 1.0f)) {
        upSince = 0;
        return;
    }
    if (phase == 3) {
        // Its AI asks at the end of the getup (PuppetRequest ends the throw then); its Wait if it did not within 300 ms
        // (user, 2026-10-25: the Wait alone kept him idle 3 s, unable to dodge).
        if (!upSince) upSince = GetTickCount() | 1;
        if (static_cast<int32_t>(GetTickCount() - upSince) >= 300) {
            upSince = 0;
            ResumeDrivenPuppet(e, "up again, no request of its AI");
        }
        return;
    }
    const DWORD now = GetTickCount();
    g_puppetSince = now ? now : 1;
    g_puppetMotion = kPuppetMotions[2];
    g_puppetPhase = 3;
    g_puppetOwn = true;
    const bool accepted = RequestEnemy(e, puppetClass.dmg[1]);
    g_puppetOwn = false;
    LOG("wesker moves: %s %08X getup %X (Dmg %X %s)", puppetClass.name, static_cast<unsigned>(e), kPuppetMotions[2],
        puppetClass.dmg[1], accepted ? "accepted" : "refused");
    if (!accepted) {
        g_puppetMotion = 0;
        ResumeDrivenPuppet(e, "refused its getup");
    }
}

// The Majini of Jill's throw: its motion at player 1's speed while thrown (it ran x1.2 and landed at his frame 117 of
// 146), then its collision and speed back once it left BackHoldDmg 0x9C (or is gone).
int PlayerActionNamed(uintptr_t p, const char* name);  // below

// An RE4 pair's side held on player 1 every frame (user, 2026-10-31: "the victim moves much more easily and gets out of
// step with the ground's shape, never with the native neck break"): each unit followed its own root travel (bone 255's
// track: HUNK walks 2.5 m round the Ganado, the Ganado 56 cm, then falls back 75 cm; no root rotation) on its own floor
// height. Now the victim's origin is player 1's less his travel so far plus its own (both from the pair's start, its
// yaw kept), at his height; once his move is over, from where his travel left him. Also its corpse's fall.
// A pair's sides from their source: an RE4 pair (DataDir\motions, re6_motions.cpp) or Josh's own list (kJoshPairVictim).
uint32_t PairSideMotion(uintptr_t enemy, int victim) {
    if (victim >= kRe6PairVictim)
        return EnemyMotionSlot(enemy, Re6MeleeEntry(static_cast<uint16_t>(victim - kRe6PairVictim)), "re6_melee.lmt");
    return victim >= kJoshPairVictim ? EnemyMotionSlot(enemy, JoshMotionEntry(static_cast<uint16_t>(victim - kJoshPairVictim)),
                                                      "pl04action (Josh)")
                                     : PairVictimMotion(enemy, static_cast<uint16_t>(victim));
}

// A left-handed player 1 (user, 2026-11-10: "your offsets are not valid left-handed: the knife counter, perfect, puts Chris a good 30 cm
// to the enemy's left"): the game plays his motions mirrored (a left-handed player's requests toggle the layer's mirror bit 0x100 but for
// five of bank 1's, swaps.cpp MirroredForLeftHand), so the whole pair is played as its mirror image: the victim's side mirrored too (its
// layer 0's bit, a Majini: TickPairLock), the pair's places and both sides' root travels with their x negated, their turns negated. Set
// at each pair's start from player 1's hand (P+0x1AF9 = 1: left).
std::atomic<bool> g_pairMirror{ false };
// The pair mirrored otherwise than player 1's hand mirrors his motions (RE6's special counters on a Majini holding its weapon in its left
// hand, user 2026-11-10: "it attacks with its left hand and Chris counters with his right"): his layer 0's mirror bit set to the pair's
// every frame of the move (the move tick), the victim's too (TickPairLock).
std::atomic<bool> g_flipPlayerMirror{ false };

bool PairRootAt(int victim, float frame, float (&out)[3]) {
    const bool ok = victim >= kRe6PairVictim ? MotionRootAt(Re6MeleeEntry(static_cast<uint16_t>(victim - kRe6PairVictim)), frame, out)
                    : victim >= kJoshPairVictim
                        ? MotionRootAt(JoshMotionEntry(static_cast<uint16_t>(victim - kJoshPairVictim)), frame, out)
                        : ForeignRootAt(3, static_cast<uint16_t>(victim), frame, out);
    if (ok && g_pairMirror.load(std::memory_order_relaxed)) out[0] = -out[0];
    return ok;
}

// The victim side's root turn at `frame` (radians; 0 without a rotation track).
float PairRootYawAt(int victim, float frame) {
    float yaw = 0.0f;
    if (victim >= kRe6PairVictim) MotionRootYawAt(Re6MeleeEntry(static_cast<uint16_t>(victim - kRe6PairVictim)), frame, yaw);
    else if (victim >= kJoshPairVictim) MotionRootYawAt(JoshMotionEntry(static_cast<uint16_t>(victim - kJoshPairVictim)), frame, yaw);
    else ForeignRootYawAt(3, static_cast<uint16_t>(victim), frame, yaw);
    return g_pairMirror.load(std::memory_order_relaxed) ? -yaw : yaw;
}

bool PlayerSideRootAt(int bank, uint16_t index, float frame, float (&out)[3]) {
    const bool ok = bank == kRe6MeleeBank   ? index != 0xFFFF && MotionRootAt(Re6MeleeEntry(index), frame, out)
                    : bank == kJoshActBank ? MotionRootAt(JoshMotionEntry(index), frame, out)
                                           : ForeignRootAt(bank == kRe6EvadeBank ? 4 : bank - kRe4LeonBank, index, frame, out);
    if (ok && g_pairMirror.load(std::memory_order_relaxed)) out[0] = -out[0];  // his motion mirrored by the game (a pair's only)
    return ok;
}

struct PairLock {
    uintptr_t unit = 0;
    int chrisBank = 0;  // player 1's move: its bank (PlayerSideRootAt)
    uint16_t chrisIndex = 0, victimIndex = 0;
    uint32_t chrisMotion = 0;
    float victimYaw = 0.0f;  // the pair's yaw
    float chrisYaw = 0.0f;   // player 1's (turned by his test slider)
    float offset[2]{};       // the victim's origin from player 1's at the start (his test sliders' move)
    float anchor[3]{};       // player 1's origin less his travel, the last time his move played
    bool chrisDone = false;
    bool logged = false;
    float start[3]{};  // player 1's origin at the start
    // The walls (user, 2026-11-10: "thrown towards an obstacle the body goes less far because of the collision, and the particles are
    // behind the wall as if it had gone through"): the game moves the victim by its root and stops it at a wall, the lock wrote it on
    // through. Between two writes the game should move it by its travel's step; what it moved less (a wall) is kept as `shift`. Only
    // once the game is seen moving it by its root (moved / planned over its first 20 cm), so a victim the game does not move is not
    // held in place.
    float shift[2]{};
    float lastPlace[3]{};
    float lastTravel[2]{};
    bool written = false;
    float moved = 0.0f, planned = 0.0f;
    int gameMoves = -1;  // -1 not known yet, 0 no, 1 yes
    bool takesOff = false;  // Re6PairTakesOff: blown off once its root leaves the floor
    bool mirrored = false;       // its victim's layer 0 mirrored by the trainer (g_pairMirror, a Majini)
    bool mirrorChecked = false;  // looked at once
    // An environmental execution (AlignPlayerToObstacle): both units slid into the pair's places over its first kEnvSlideFrames frames
    // from where they stood (user, 2026-11-11: "make the place against the wall more generous than RE6's"): player 1's offset still to
    // go (slideP, applied so far appliedP) and the victim's (slideV, from its own frame). No wall following then (the obstacle is the
    // pair's own).
    bool env = false;
    float slideP[2]{}, appliedP[2]{}, slideV[2]{};
    // From a height: the victim's height off the pair's (its side made for a 100 cm ledge: from below `victimDy` = the ledge's height - 100,
    // let go over its frames decayFrom-decayTo as it comes down; from above -the height, the pair's floor under player 1's); player 1
    // driven from above (his root's drop scaled to the ledge's height, dropScale; his root's height at the start rootY0).
    float victimDy = 0.0f, decayFrom = 0.0f, decayTo = 0.0f;
    bool drivePlayer = false;
    float dropScale = 1.0f, rootY0 = 0.0f;
};
constexpr float kEnvSlideFrames = 14.0f;

// A unit put at `place` with its terrain collision's last place moved as much (user, 2026-11-11: "from above Chris does not come down from
// the ledge, he stays caught on the invisible wall: free him as in a jump down"; "from below the enemy went up then came back down on to
// the ledge"): the collision moves a unit from its last place (+0x1170, +0x11D0: player.cpp "Rester sur place en réactivant les
// collisions"; the Majini have them at the same offsets, read 2026-11-11, research/env_collision_fields.py) to its place, stopped by
// the walls in between.
void PlaceUnit(uintptr_t unit, const float (&place)[3]) {
    float at[3]{};
    if (!unit || !mem::SafeRead(reinterpret_cast<void*>(unit + 0x30), at, sizeof(at))) return;
    for (uintptr_t field : { 0x1170u, 0x11D0u }) {
        float last[3]{};
        if (!mem::SafeRead(reinterpret_cast<void*>(unit + field), last, sizeof(last))) continue;
        for (int i = 0; i < 3; ++i) last[i] += place[i] - at[i];
        mem::SafeWrite(reinterpret_cast<void*>(unit + field), last, sizeof(last));
    }
    mem::SafeWrite(reinterpret_cast<void*>(unit + 0x30), place, sizeof(place));
}
PairLock g_pairLock;  // render thread

// The mirror bit the trainer set taken back from the pair's victim (alive, still on the pair's motion) when its side is cut: a motion
// the game sets next has its own mode.
void UnmirrorPairVictim() {
    PairLock& lock = g_pairLock;
    if (!lock.mirrored || !lock.unit) return;
    lock.mirrored = false;
    bool alive = false;
    for (uintptr_t u : actors::Enemies()) alive = alive || u == lock.unit;
    uint32_t motion = 0;
    uint16_t mode = 0;
    if (alive && mem::Read(lock.unit + 0x384, motion) && (motion & 0xFFFF) == (g_pairMotion.load() & 0xFFFF) &&
        mem::Read(lock.unit + 0x38A, mode) && (mode & 0x100))
        mem::Write<uint16_t>(lock.unit + 0x38A, static_cast<uint16_t>(mode & ~0x100));
}

void StartPairLock(uintptr_t victim, uintptr_t p, const CombatMove& move, const float* origin, float yaw, float chrisYaw) {
    g_pairLock = {};
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return;
    g_pairLock.unit = victim;
    g_pairLock.chrisBank = move.bank;
    g_pairLock.chrisIndex = move.index;
    g_pairLock.victimIndex = static_cast<uint16_t>(move.victim);
    g_pairLock.chrisMotion = MoveMotion(move.bank, move.index);
    g_pairLock.victimYaw = yaw;
    g_pairLock.chrisYaw = chrisYaw;
    g_pairLock.takesOff = move.victim >= kRe6PairVictim && Re6PairTakesOff(move.id);
    g_pairLock.offset[0] = origin[0] - pos[0];
    g_pairLock.offset[1] = origin[2] - pos[2];
    std::memcpy(g_pairLock.anchor, pos, sizeof(pos));
    std::memcpy(g_pairLock.start, pos, sizeof(pos));
}

void TickPairLock() {
    PairLock& lock = g_pairLock;
    if (!lock.unit) return;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    bool alive = false;
    for (uintptr_t u : actors::Enemies()) alive = alive || u == lock.unit;
    uint32_t motion = 0;
    float frame = 0.0f, length = 0.0f;
    if (!p || !alive || !mem::Read(lock.unit + 0x384, motion) || (motion & 0xFFFF) != (g_pairMotion.load() & 0xFFFF) ||
        !mem::Read(lock.unit + 0x398, frame) || !mem::Read(lock.unit + 0x39C, length) || frame >= length - 0.5f) {
        UnmirrorPairVictim();
        lock.unit = 0;  // its side over: its own motions from there
        return;
    }
    // The pair mirrored (a left-handed player 1): its victim's side too - layer 0's mode (+0x380 + 0xA), bit 0x100 as a left-handed
    // player's - on a Majini (its skeleton a man's). The game mirrors it itself for a left-handed attacker (user-verified 2026-11-10:
    // its mode 0146 at the pair's start, 0046 otherwise; "the left-handed mode works"): only set when it did not.
    const bool wantMirror = g_pairMirror.load(std::memory_order_relaxed);
    if ((wantMirror || g_flipPlayerMirror.load(std::memory_order_relaxed)) && !lock.mirrorChecked && IsMajiniEnemy(lock.unit)) {
        uint16_t mode = 0;
        if (mem::Read(lock.unit + 0x38A, mode)) {
            lock.mirrorChecked = true;
            if (wantMirror && !(mode & 0x100)) {
                mem::Write<uint16_t>(lock.unit + 0x38A, static_cast<uint16_t>(mode | 0x100));
                lock.mirrored = true;
            } else if (!wantMirror && (mode & 0x100)) {  // mirrored by the game for a left-handed player 1, not the pair
                mem::Write<uint16_t>(lock.unit + 0x38A, static_cast<uint16_t>(mode & ~0x100));
            }
            LOG("pair lock: %08X's side %s (its mode %04X%s)", static_cast<unsigned>(lock.unit), wantMirror ? "mirrored" : "not mirrored", mode,
                lock.mirrored ? ", set by the trainer" : "");
        }
    }
    auto world = [](const float (&v)[3], float yaw, float& x, float& z) {
        const float c = std::cos(yaw), s = std::sin(yaw);
        x = v[0] * c + v[2] * s;
        z = -v[0] * s + v[2] * c;
    };
    uint32_t chrisMotion = 0;
    float chrisFrame = 0.0f, pos[3]{};
    if (!lock.chrisDone && mem::Read(p + 0x384, chrisMotion) && (chrisMotion & 0xFFFF) == (lock.chrisMotion & 0xFFFF) &&
        mem::Read(p + 0x398, chrisFrame) && mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) {
        if (lock.env) {  // his slide to the pair's place (he started where he stood)
            const float k = 1.0f - std::clamp(chrisFrame / kEnvSlideFrames, 0.0f, 1.0f);
            const float want[2] = { lock.slideP[0] * k, lock.slideP[1] * k };
            if (want[0] != lock.appliedP[0] || want[1] != lock.appliedP[1]) {
                pos[0] += want[0] - lock.appliedP[0];
                pos[2] += want[1] - lock.appliedP[1];
                PlaceUnit(p, pos);
                lock.appliedP[0] = want[0];
                lock.appliedP[1] = want[1];
            }
        }
        float root[3]{}, x = 0.0f, z = 0.0f;
        if (PlayerSideRootAt(lock.chrisBank, lock.chrisIndex, chrisFrame, root)) world(root, lock.chrisYaw, x, z);
        if (lock.drivePlayer) {  // from above: his place written from the pair's (the ledge's edge and its invisible walls not in the way)
            pos[0] = lock.start[0] + x + lock.appliedP[0];
            pos[1] = lock.start[1] + (root[1] - lock.rootY0) * lock.dropScale;
            pos[2] = lock.start[2] + z + lock.appliedP[1];
            PlaceUnit(p, pos);
        }
        if (!lock.logged && chrisFrame >= 60.0f) {  // the units do follow their root travel (checked once a pair)
            lock.logged = true;
            LOG("wesker moves: pair at frame %.0f: player 1 moved %.0f cm from the start for a root travel of %.0f cm",
                chrisFrame, std::hypot(pos[0] - lock.start[0], pos[2] - lock.start[2]), std::hypot(x, z));
        }
        lock.anchor[0] = pos[0] - x - lock.appliedP[0];
        lock.anchor[1] = lock.drivePlayer ? lock.start[1] : pos[1];
        lock.anchor[2] = pos[2] - z - lock.appliedP[1];
    } else {
        lock.chrisDone = true;
    }
    float root[3]{}, x = 0.0f, z = 0.0f;
    if (!PairRootAt(lock.victimIndex, frame, root)) return;
    world(root, lock.victimYaw, x, z);
    if (lock.takesOff && root[1] > 60.0f && IsMajiniEnemy(lock.unit)) {
        // Let go by Jake and flying (user: "wait until Jake lets it go and it flies"; its root 60 cm up: frame 114, 148 for the belly one, its
        // hips 97 cm up), put down on player 1's floor (the blow-off starts with its hips about 100 cm up): the pair over, the Majini blown off
        // along the throw's way (its root's travel over the next frames), facing
        // back against it (a front hit: the straight blow-off backwards), dead when it lands if the blow was lethal (TickThrown kept it at 1).
        const uintptr_t e = lock.unit;
        float ahead[3]{}, ax = 0.0f, az = 0.0f, pos[3]{};
        PairRootAt(lock.victimIndex, frame + 8.0f, ahead);
        world(ahead, lock.victimYaw, ax, az);
        float dx = ax - x, dz = az - z;
        float len = std::hypot(dx, dz);
        const float facing = lock.victimYaw + PairRootYawAt(lock.victimIndex, frame);
        if (len < 1.0f) {  // no travel yet: backwards from its facing
            dx = -std::sin(facing), dz = -std::cos(facing), len = 1.0f;
        }
        dx /= len, dz /= len;
        UnmirrorPairVictim();
        lock.unit = 0;
        g_pairVictim = 0;
        const bool lethal = g_thrownLethal;
        g_thrownLethal = false;  // ReleaseThrown would kill it lying
        if (e == g_thrownUnit.load()) ReleaseThrown(true);
        if (lethal) mem::Write<int16_t>(e + 0x1364, 0);
        const float back = std::atan2(-dx, -dz);
        const float quat[4] = { 0.0f, std::sin(back * 0.5f), 0.0f, std::cos(back * 0.5f) };
        mem::SafeWrite(reinterpret_cast<void*>(e + 0x40), quat, sizeof(quat));
        bool asked = false;
        if (mem::SafeRead(reinterpret_cast<void*>(e + 0x30), pos, sizeof(pos))) {
            pos[1] = lock.anchor[1];
            mem::SafeWrite(reinterpret_cast<void*>(e + 0x30), pos, sizeof(pos));
            const float from[3] = { pos[0] - dx * 100.0f, pos[1], pos[2] - dz * 100.0f };
            const DWORD now = GetTickCount();
            g_straightBlowTick = now ? now : 1;
            g_straightBlowUnit = e;  // its flight 0x52A, its landing 0x52B (WeskerMoveMotion)
            asked = MajiniBlowOffFrom(e, from, 1);
        }
        LOG("wesker moves: %08X takes off at its frame %.0f: blown off along the throw (%.2f %.2f)%s, %s", static_cast<unsigned>(e), frame, dx,
            dz, lethal ? ", dead when it lands" : "", asked ? "asked" : "refused");
        return;
    }
    float actual[3]{};
    if (!lock.env && lock.written && mem::SafeRead(reinterpret_cast<void*>(lock.unit + 0x30), actual, sizeof(actual))) {
        const float stepX = x - lock.lastTravel[0], stepZ = z - lock.lastTravel[1];
        const float step = std::hypot(stepX, stepZ), went = std::hypot(actual[0] - lock.lastPlace[0], actual[2] - lock.lastPlace[2]);
        if (lock.gameMoves < 0 && step > 0.5f && step < 50.0f) {
            lock.planned += step;
            lock.moved += std::min(went, step * 2.0f);
            if (lock.planned >= 20.0f) {
                lock.gameMoves = lock.moved >= 0.5f * lock.planned ? 1 : 0;
                LOG("pair lock: the game moved its victim %.0f cm for %.0f cm of travel: %s", lock.moved, lock.planned,
                    lock.gameMoves ? "walls followed" : "placed by the lock alone");
            }
        }
        // What it went short of the step (the directions may differ a little in turns: the distances only), taken back along the step.
        if (lock.gameMoves == 1 && step > 0.5f && step < 50.0f) {
            const float off = step - went;
            if (off > 1.5f) {
                lock.shift[0] -= stepX / step * off;
                lock.shift[1] -= stepZ / step * off;
                static ULONGLONG told = 0;
                if (GetTickCount64() - told > 500) {
                    told = GetTickCount64();
                    LOG("pair lock: its victim held %.0f cm short (a wall), %.0f cm in all", off, std::hypot(lock.shift[0], lock.shift[1]));
                }
            }
        }
    }
    // Its height: the root's when it is thrown up (user, 2026-11-10, Jake's Ground: "it never falls back to the ground and dies in the air";
    // RE6's thrown sides go up to 70-114 cm), never under player 1's floor (RE6's wall / table sides keep their root at -100 cm). RE6's sides only.
    const float slid = lock.env ? 1.0f - std::clamp(frame / kEnvSlideFrames, 0.0f, 1.0f) : 0.0f;  // its own slide to its place
    const float dy = lock.decayTo > lock.decayFrom
                         ? lock.victimDy * (1.0f - std::clamp((frame - lock.decayFrom) / (lock.decayTo - lock.decayFrom), 0.0f, 1.0f))
                         : lock.victimDy;
    const float place[3] = { lock.anchor[0] + lock.offset[0] + x + lock.shift[0] + lock.slideV[0] * slid,
                             lock.anchor[1] + (lock.victimIndex >= kRe6PairVictim ? std::max(0.0f, root[1]) : 0.0f) + dy,
                             lock.anchor[2] + lock.offset[1] + z + lock.shift[1] + lock.slideV[1] * slid };
    std::memcpy(lock.lastPlace, place, sizeof(place));
    lock.lastTravel[0] = x;
    lock.lastTravel[1] = z;
    lock.written = true;
    // Its facing turned by its root's turn (RE6's victims turn up to 180 degrees: user, 2026-11-10, "turned the wrong way at the end").
    const float facing = lock.victimYaw + PairRootYawAt(lock.victimIndex, frame);
    const float quat[4] = { 0.0f, std::sin(facing * 0.5f), 0.0f, std::cos(facing * 0.5f) };
    if (lock.env) PlaceUnit(lock.unit, place);
    else mem::SafeWrite(reinterpret_cast<void*>(lock.unit + 0x30), place, sizeof(place));
    mem::SafeWrite(reinterpret_cast<void*>(lock.unit + 0x40), quat, sizeof(quat));
}

void TickThrown() {
    TickPairLock();
    const uintptr_t e = g_thrownUnit.load(std::memory_order_relaxed);
    if (!e) return;
    bool alive = false;
    for (uintptr_t u : actors::Enemies()) alive = alive || u == e;
    uint32_t action = 0;
    // An RE4 pair's side killed at the blow: its corpse plays the pair's fall to its end, out of the collisions (and
    // player 1's contacts) meanwhile.
    if (alive && e == g_pairDying.load(std::memory_order_relaxed)) {
        uint32_t m = 0;
        float f = 0.0f, len = 0.0f;
        const bool onPair = mem::Read(e + 0x384, m) && (m & 0xFFFF) == (g_pairMotion.load() & 0xFFFF);
        // The machete disarm's victim killed (user, 2026-11-10: "its side is the one where it survives: it pulls the weapon out of its back
        // and gets up"; "the Majini's standing death falls to its knees before its belly: cut its start"): at its side's frame 232, on its
        // knees before it rises, its standing death cEm10ActDeadNormal2 (its init RVA 0x824C50 plays 0x551: on its knees on frames 32-72,
        // lying by 100) from frame 76, a native death to its end (research/re6_special_counters/em10_curve.py).
        static uintptr_t kneelTried = 0;  // once per corpse: refused, it goes on as the others (not left frozen)
        if (onPair && e != kneelTried && g_pairCorpseVictim.load() == kRe6PairVictim + 169 && IsMajiniEnemy(e) && mem::Read(e + 0x398, f) &&
            f >= 232.0f) {
            kneelTried = e;
            // DeadNormal2 itself is refused there (logged at the kill and at its knees: "its death refused"), the lying deaths not: DeadFaceDown
            // 0x8E asked, its motion replaced by DeadNormal2's 0x551 from frame 76 (on its knees, then down on its belly).
            // Turned to where its body faces (user, 2026-11-10: "the side turns it sideways in the hold and the death makes it fall in front: not
            // the way the animation means it to fall"): the pair kept its unit's yaw while the side turned its body, and the death falls along the
            // unit's facing. The pelvis's front (the joint after the hips, as LyingOnBack), flattened, is its yaw; its origin is already under its
            // hips (the victim sides are re-rooted there). The lock let go first: it writes the pair's yaw every frame.
            {
                uintptr_t joints = 0;
                uint8_t second = 0xFF;
                float front[3]{};
                if (mem::Read(e + 0x318, joints) && joints) {
                    const uintptr_t first = joints + (mem::Read(joints + 0x90 + 7, second) && second == 0 ? 0x90 : 0);
                    if (mem::SafeRead(reinterpret_cast<void*>(first + 0x90 + 0x70), front, sizeof(front)) && std::hypot(front[0], front[2]) > 0.1f) {
                        if (g_pairLock.unit == e) {
                            g_pairLock.mirrored = false;  // the death's motion comes with its own mode
                            g_pairLock.unit = 0;
                        }
                        const float yaw = std::atan2(front[0], front[2]);
                        const float quat[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
                        mem::SafeWrite(reinterpret_cast<void*>(e + 0x40), quat, sizeof(quat));
                        // Its native fall from the knees carries it forward (user, 2026-11-10: "it falls a little further than where it
                        // kneels, as if it went 20 cm forward in its native fall"): its origin kKneelFallBack cm back along that facing.
                        constexpr float kKneelFallBack = 40.0f;  // then "another 20 cm back" (40)
                        float at[3]{};
                        if (mem::SafeRead(reinterpret_cast<void*>(e + 0x30), at, sizeof(at))) {
                            at[0] -= std::sin(yaw) * kKneelFallBack;
                            at[2] -= std::cos(yaw) * kKneelFallBack;
                            mem::SafeWrite(reinterpret_cast<void*>(e + 0x30), at, sizeof(at));
                        }
                        LOG("wesker moves: %08X turned to its body's facing (%.0f degrees) for its death, %.0f cm back", static_cast<unsigned>(e),
                            yaw * 57.2958f, kKneelFallBack);
                    }
                }
            }
            const int die = 0x8E;
            g_corpseNativeStart = 76.0f;
            g_corpseNativeMotion = 0x551;
            g_corpseNative = e;
            const bool asked = RequestEnemy(e, static_cast<uint32_t>(die));
            if (!asked) g_corpseNativeMotion = 0;
            LOG("wesker moves: %08X killed by the machete disarm: DeadFaceDown %X with the kneeling death 551 from its knees %s",
                static_cast<unsigned>(e), die, asked ? "asked" : "refused");
            if (asked) {
                g_pairDying = 0;
                return;
            }
            g_corpseNative = 0;
        }
        // Lying already, a second after the blow: no longer (user: "a very long time on the ground" - the throat cut's side is 314
        // frames, the corpse played it from frame 80 until the 10 s cap).
        // Flat and still for 0.4 s (user: "after Chris's RE6 Leg Back the enemy died on its back though its fall ends on its belly": it
        // lay low on its back a moment before rolling over), its side taken as it is then.
        float hip = 0.0f, facing = 0.0f;
        LyingOnBack(e, &hip, &facing);  // its hips' height above its origin; its pelvis's facing (> 0.6 on its back, < -0.6 belly)
        static uintptr_t stillUnit = 0;
        static float stillFacing = 0.0f;
        static DWORD stillSince = 0;
        const DWORD tick = GetTickCount();
        if (e != stillUnit || std::fabs(facing - stillFacing) > 0.08f || hip <= 0.0f || hip >= 35.0f || std::fabs(facing) < 0.6f) {
            stillUnit = e;  // its facing compared with the one it had when it was first seen still (a slow roll adds up)
            stillFacing = facing;
            stillSince = tick;
        }
        const bool lying = static_cast<int32_t>(tick - g_pairCorpseTick.load()) > 1000 && static_cast<int32_t>(tick - stillSince) > 400;
        if (onPair && !lying && mem::Read(e + 0x398, f) && mem::Read(e + 0x39C, len) && f < len - 1.0f &&
            static_cast<int32_t>(GetTickCount() - g_thrownSince.load(std::memory_order_relaxed)) < 10000)
            return;
        mem::Read(e + 0x398, f);
        mem::Read(e + 0x39C, len);
        g_pairDying = 0;
        // The fall over, its death's own motion (user, 2026-11-10, Mercenaries: "after the knife's throat cut the enemy stays stuck on
        // the ground and does not despawn", "the same with Ada's Leg Back; not in the campaign": logged, the corpse in DeadFaceUp 0x8D
        // on the pair's side C4, ended, for over a minute - the game's end of a corpse never came with that motion): the death asked
        // again, its own lying motion realigned under the body (PairLyingRealign), as Jill's throw's corpses.
        int16_t health = 1;
        if (onPair && IsMajiniEnemy(e) && mem::Read(e + 0x1364, health) && health <= 0) {
            const bool faceDown = LyingFaceDown(e);
            g_corpseFaceDown = faceDown;
            g_corpseRealign = e;
            const bool asked = RequestEnemy(e, faceDown ? 0x8E : 0x8D);
            if (!asked) g_corpseRealign = 0;
            LOG("wesker moves: %08X's corpse ends the pair's fall at its frame %.0f / %.0f: its own death on its %s %s",
                static_cast<unsigned>(e), f, len, faceDown ? "belly" : "back", asked ? "asked" : "refused");
        }
    }
    if (e == g_puppet.load() && g_puppetPhase.load() == 4 &&
        static_cast<int32_t>(GetTickCount() - g_puppetSince.load()) > 2000)
        EndPuppet(e, "its death never set a motion");
    // Jill's second throw killing its Majini (user, 2026-10-30: Chris's side has the victim get up, "il se relève puis
    // meurt, c'est bizarre"): the motion cut once it lies (its hips under 35 cm, past the first third of the motion), dead
    // where it lies; a survivor keeps the whole motion and gets up with it.
    uint32_t motion = 0;
    float frame = 0, length = 0, hip = 0, facing = 0;
    const bool jill2 = alive && mem::Read(e + 0x10E0, action) && action == 0x9C && mem::Read(e + 0x384, motion) &&
                       (motion & 0xFFFF) == kJill2VictimMotion && mem::Read(e + 0x398, frame) &&
                       mem::Read(e + 0x39C, length) && length > 0;
    if (jill2 && g_thrownLethal && frame > length * 0.3f) {
        LyingOnBack(e, &hip, &facing);
        if (hip > 0.0f && hip < 35.0f) {
            LOG("wesker moves: Jill's second throw killed %08X: its getup cut at frame %.0f / %.0f", static_cast<unsigned>(e),
                frame, length);
            ReleaseThrown(true);
            return;
        }
    }
    // A survivor gets up with Chris's side of the throw (user: "il peut se relever avec l'anim de victime"); its
    // BackHoldDmg then ended in RiseFaceDown 0x6B, back on the ground to get up again (user, 2026-10-31: "il se relève,
    // freeze, puis se téléporte au sol pour jouer son relevé"): its Wait at the motion's end, and that getup's request
    // made its Wait too (WeskerPuppetRequest).
    if (jill2 && !g_thrownLethal) {
        const DWORD now = GetTickCount();
        g_jill2SurvivorSince = now ? now : 1;
        g_jill2Survivor = e;
        if (frame >= length - 2.0f) {
            const int wait = PlayerActionNamed(e, "cEm10ActWait");
            LOG("wesker moves: Jill's second throw survived by %08X: up with its motion, its Wait %X", static_cast<unsigned>(e),
                wait);
            ReleaseThrown(true);
            if (wait >= 0) RequestEnemy(e, static_cast<uint32_t>(wait));
            return;
        }
    }
    // A survivor left in the pair's carrier action after its side ended (user, 2026-11-11: the second armed-zombie counter, "frozen
    // without dying", then "it teleported standing after more than 30 s"; logged: no way out of 0x9C, where another of these went on to
    // RiseFaceDown 0x6B by itself): half a second after its side's last frame, the game's RiseFaceDown asked.
    {
        static uintptr_t endedUnit = 0;
        static DWORD endedSince = 0;
        uint32_t m = 0;
        float f = 0.0f, len = 0.0f;
        int16_t health = 0;
        if (alive && mem::Read(e + 0x10E0, action) && action == 0x9C && IsMajiniEnemy(e) && mem::Read(e + 0x1364, health) && health > 0 &&
            mem::Read(e + 0x384, m) && (m & 0xFFFF) == (g_pairMotion.load() & 0xFFFF) && mem::Read(e + 0x398, f) &&
            mem::Read(e + 0x39C, len) && len > 0.0f && f >= len - 1.0f) {
            const DWORD tick = GetTickCount();
            if (endedUnit != e) {
                endedUnit = e;
                endedSince = tick;
            } else if (static_cast<int32_t>(tick - endedSince) > 500) {
                endedUnit = 0;
                const bool asked = RequestEnemy(e, 0x6B, 0);
                LOG("wesker moves: %08X still in the pair's carrier after its side: its RiseFaceDown %s", static_cast<unsigned>(e),
                    asked ? "asked" : "refused");
            }
        } else if (endedUnit == e) {
            endedUnit = 0;
        }
    }
    if (alive && (e == g_puppet.load() || (mem::Read(e + 0x10E0, action) && action == 0x9C))) {
        const uintptr_t p = actors::Player(0);
        float speed = 0;
        if (p && mem::Read(p + actors::off::Speed, speed) && speed > 0) mem::Write(e + actors::off::Speed, speed);
        return;
    }
    if (alive && static_cast<int32_t>(GetTickCount() - g_thrownSince.load(std::memory_order_relaxed)) < 300) return;
    if (alive) mem::Read(e + 0x10E0, action);
    ReleaseThrown(alive);
    LOG("wesker moves: %08X out of the throw (action %X)", static_cast<unsigned>(e), action);
}

// Jill's throw: player 1 invulnerable while she is caught (0x27 / 0x2A) and he plays his part (the event action
// 0x80000000 her Caught gives him), at least 1.5 s, at most 8 s. Not until she leaves Caught: Sheva holding her keeps
// her in it (user, 2026-10-24: after a throw Chris had no reaction to Wesker's attacks and could not be grabbed for a
// minute and a half).
void TickCaught() {
    if (!g_caught) return;
    uint32_t action = 0, playerAction = 0;
    int16_t health = 0;
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    const bool caught = p1 && mem::Read(g_caught + 0x10E0, action) && (action == 0x27 || action == 0x2A);
    const bool playing = p1 && mem::Read(p1 + 0x10E0, playerAction) && playerAction == 0x80000000;
    if (playing) g_caughtPlayed = true;
    const DWORD elapsed = GetTickCount() - g_caughtStart;
    if (elapsed < 1500 || (caught && (playing || !g_caughtPlayed) && elapsed < 8000)) return;
    mem::Read(g_caught + 0x1364, health);
    LOG("wesker moves: Jill's throw over after %u ms (Jill action %X, player 1 action %X, health %d)",
        static_cast<unsigned>(elapsed), action, playerAction, health);
    g_caught = 0;
    if (!g_active.move) g_invulnerable = false;
}

// "Prise de dos : se libérer en appuyant" (licker.cpp, user 2026-10-30): a back grab escape missed, the Majini breaks
// player 1's neck - Chris's own neck break reversed (user: "inverser le neck break"). His neck break (Leg Back 0x39;
// the first version took Finisher Back 0x34 / 0x10E, user: "le neck break c'est Leg Back") plays his 0x110 (176
// frames) while its victim plays 0xF1D (281 frames), his own action list lent as its bank F (the victim's side of his
// event, forced_death.cpp). Here the Majini gets that list in its bank F and plays 0xF10
// through its BackHoldDmg 0x9C (the thrown-unit path of the SetMotion hook, as Jill's throw: collision off, contacts
// dropped, its speed player 1's), and player 1 plays his 0x11D through the moves' carrier, both from the Majini's origin
// and facing (the attacker's, as in his event). Lying at its end, he dies: health 0 and his action table's die_lie,
// whose motion (on his back: user, 2026-10-30, "the enemies end on their belly") becomes the victim's last frame. He
// cannot be hit nor grabbed meanwhile (WeskerMoveInvulnerable: another Majini's easier grab had cut the first one at
// 0.5 s, the camera spun and he died on his back at once).
// The played character's own neck break (user, 2026-10-31: "it only works with Chris, Wesker and the others play no
// victim's motion"): his melee row's move 9 (melee table entry +0x18 his side, +0x1C the victim's) when it is one -
// Chris's NECK TWIST 0x10 / 0x1D, Sheva's and Jill's NECK 0x0E / 0x1D. The others have another move there (Wesker's
// BLOW KICK, Barry's CHOP, Josh's BACK DROP...): Chris's action list is lent in their bank 1 meanwhile (given back
// 3 s after the kill), with his slots.
constexpr int kNeckBank = 0xF;
constexpr uint16_t kNeckAttacker = 0x10, kNeckVictim = 0x1D;  // Chris's
uint16_t g_neckAttacker = kNeckAttacker, g_neckVictim = kNeckVictim;  // render thread: this neck break's
void* g_neckOwnList = nullptr;   // player 1's own bank 1 list while Chris's is lent
uintptr_t g_neckListPlayer = 0;
DWORD g_neckRestoreAt = 0;       // when it is given back (0: not yet killed)
std::atomic<uintptr_t> g_neckKiller{ 0 };  // render thread
DWORD g_neckSince = 0;
float g_neckOrigin[3]{}, g_neckQuat[4]{};
bool g_neckPlayed = false;  // player 1's victim motion seen
// BackHoldDmg ends as after an escape, in RiseFaceDown 0x6B (the getup of a Majini thrown on its belly): standing after
// the neck break it was put lying at once (user, 2026-10-30: "il s'est téléporté par terre", its getup on the last
// frame before the game over's fade). That request becomes its Wait (cEm10ActWait) for the killer, 10 s at most.
std::atomic<uintptr_t> g_neckMajini{ 0 };
std::atomic<DWORD> g_neckMajiniSince{ 0 };
float g_neckLength = 281.0f;  // its frames (the last one kept by his death)

// A unit's action whose class is `name` (its table +0x10D0 count / +0x10DC objects), -1 if none.
int PlayerActionNamed(uintptr_t p, const char* name) {
    uint32_t count = 0;
    uintptr_t table = 0;
    if (!mem::Read(p + 0x10D0, count) || count > 0x200 || !mem::Read(p + 0x10DC, table) || !table) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t object = 0;
        const char* cls = mem::Read(table + 4 * i, object) && object ? UnitClassName(object) : nullptr;
        if (cls && std::strcmp(cls, name) == 0) return static_cast<int>(i);
    }
    return -1;
}

void NeckBreakKill(uintptr_t p, const char* why) {
    g_neckKiller = 0;
    mem::Write<int16_t>(p + 0x1364, 0);
    const int die = PlayerActionNamed(p, "cPlAction_pl_act_die_lie");
    // Its motion: the victim's last frame, face down (the SetMotion hook's pending motion and start frame).
    g_pendingBegin = std::max(0.0f, g_neckLength - 1.0f);
    g_pendingMotion = (1u << 8) | g_neckVictim;
    const DWORD now = GetTickCount();
    g_pendingSince = now ? now : 1;
    if (g_neckOwnList) g_neckRestoreAt = (now + 3000) | 1;  // his own list back once the death's motion is set
    const bool asked = die >= 0 && RequestPlayer(p, static_cast<uint32_t>(die));
    if (!asked) g_pendingMotion = 0;
    LOG("back grab: player 1 killed by the Majini's neck break (%s; die_lie %X %s)", why, die, asked ? "asked" : "not asked");
}

void PlaceAt(uintptr_t unit, const float* pos, const float* quat, bool player) {
    mem::SafeWrite(reinterpret_cast<void*>(unit + 0x30), pos, 3 * sizeof(float));
    mem::SafeWrite(reinterpret_cast<void*>(unit + 0x40), quat, 4 * sizeof(float));
    if (!player) return;
    const float yaw = 2.0f * std::atan2(quat[1], quat[3]);
    mem::Write(unit + 0x1024, yaw);
    mem::Write(unit + 0x2A10, yaw);
}

void TickNeckBreak() {
    // Chris's list lent for a neck break: the player's own back (the same player, still holding Chris's there).
    if (g_neckOwnList && g_neckRestoreAt && static_cast<int32_t>(GetTickCount() - g_neckRestoreAt) >= 0) {
        const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
        if (p && p == g_neckListPlayer) SetBank(p, g_neckOwnList, 1);
        LOG("back grab: player 1's own action list back%s", p && p == g_neckListPlayer ? "" : " (another player now)");
        g_neckOwnList = nullptr;
        g_neckRestoreAt = 0;
    }
    const uintptr_t m = g_neckKiller.load(std::memory_order_relaxed);
    if (!m) return;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p) {
        g_neckKiller = 0;
        return;
    }
    const DWORD elapsed = GetTickCount() - g_neckSince;
    if (elapsed < 250) {  // both on the origin until their motions run (BackHoldDmg's init moves the Majini)
        PlaceAt(p, g_neckOrigin, g_neckQuat, true);
        PlaceAt(m, g_neckOrigin, g_neckQuat, false);
    }
    uint32_t motion = 0;
    float frame = 0, length = 0;
    mem::Read(p + 0x384, motion);
    mem::Read(p + 0x398, frame);
    mem::Read(p + 0x39C, length);
    if ((motion & 0xFFFF) == ((1u << 8) | g_neckVictim)) {
        g_neckPlayed = true;
        if (length > 0) g_neckLength = length;
        if (length > 0 && frame >= length - 2.0f) NeckBreakKill(p, "lying at the end of the victim's motion");
        return;
    }
    if (g_neckPlayed) NeckBreakKill(p, "the victim's motion left early");
    else if (elapsed > 1500) NeckBreakKill(p, "the victim's motion never came");
}

// ---- The trainer moves' hits on the limbs (user, 2026-10-31: "the zones are sometimes too big or too small, Jill's slide
// for one: make them like the native blows"). In a move's hit window an enemy is hit when one of player 1's limbs meets
// one of its hurt shapes - the game's own (type +0xC4 1, on +0xBD, in its list +0x1290: +4 shapes, +0x18 count), whose
// world box (+0x10 min, +0x20 max, their radius in) the game keeps on the body every frame. The limbs come from player
// 1's joints ([P+0x318], 0x90 each, count P+0x314: bone id byte 7, world position +0x80), numbered alike on every
// player skeleton (his hurt shapes' bones): right elbow 7, wrist 8; left C, D; right knee 0x11, ankle 0x12, toes 0x13;
// left 0x15, 0x16, 0x17. Not his own shapes: the invincibility puts their number to 0 and the game stops placing them.
// An enemy with no hurt shape on keeps the old zone (kReach / kArc / kHeight from his origin).
struct LimbPart {
    uint8_t from, to;  // bone ids (the same: a sphere)
    float radius;      // cm around the segment
    const char* name;
};
constexpr LimbPart kLimbParts[] = {
    { 0x07, 0x08, 15.0f, "right forearm" }, { 0x08, 0x08, 30.0f, "right fist" },
    { 0x0C, 0x0D, 15.0f, "left forearm" },  { 0x0D, 0x0D, 30.0f, "left fist" },
    { 0x11, 0x12, 15.0f, "right shin" },    { 0x12, 0x13, 25.0f, "right foot" },
    { 0x15, 0x16, 15.0f, "left shin" },     { 0x16, 0x17, 25.0f, "left foot" },
};

float BoxDistance(const float* point, const float* lo, const float* hi) {
    float d2 = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float out = point[c] < lo[c] ? lo[c] - point[c] : point[c] > hi[c] ? point[c] - hi[c] : 0.0f;
        d2 += out * out;
    }
    return std::sqrt(d2);
}

// The enemy's hurt boxes on (count of them in `boxes`); false if it has none (the old zone then).
int HurtBoxes(uintptr_t enemy, float (*boxes)[6], int most) {
    uintptr_t shapes = 0;
    uint32_t count = 0;
    if (!mem::Read(enemy + 0x1290 + 4, shapes) || !shapes || !mem::Read(enemy + 0x1290 + 0x18, count)) return 0;
    int found = 0;
    for (uint32_t i = 0; i < std::min<uint32_t>(count, 64) && found < most; ++i) {
        uintptr_t shape = 0;
        uint32_t type = 0;
        uint8_t on = 0;
        float* box = boxes[found];
        if (!mem::Read(shapes + 4 * i, shape) || !shape || !mem::Read(shape + 0xC4, type) || type != 1 ||
            !mem::Read(shape + 0xBD, on) || !on || !mem::SafeRead(reinterpret_cast<void*>(shape + 0x10), box, 12) ||
            !mem::SafeRead(reinterpret_cast<void*>(shape + 0x20), box + 3, 12))
            continue;
        bool valid = true;
        for (int c = 0; c < 3; ++c)
            valid = valid && std::isfinite(box[c]) && std::isfinite(box[c + 3]) && box[c] <= box[c + 3] &&
                    box[c + 3] - box[c] < 1000.0f;
        if (valid) ++found;
    }
    return found;
}

// The parts' end points in the world this frame.
struct LimbPoints {
    bool ok[std::size(kLimbParts)] = {};
    float a[std::size(kLimbParts)][3] = {}, b[std::size(kLimbParts)][3] = {};
};

LimbPoints ReadLimbs(uintptr_t p) {
    LimbPoints limbs;
    uintptr_t joints = 0;
    uint32_t count = 0;
    if (!mem::Read(p + 0x318, joints) || !joints || !mem::Read(p + 0x314, count)) return limbs;
    uintptr_t bone[0x20] = {};  // bone id -> its joint (the first one with it)
    for (uint32_t j = 0; j < std::min<uint32_t>(count, 0x100); ++j) {
        uint8_t id = 0;
        if (!mem::Read(joints + j * 0x90 + 7, id)) break;
        if (id < std::size(bone) && !bone[id]) bone[id] = joints + j * 0x90;
    }
    for (size_t k = 0; k < std::size(kLimbParts); ++k) {
        const uintptr_t from = bone[kLimbParts[k].from], to = bone[kLimbParts[k].to];
        limbs.ok[k] = from && to && mem::SafeRead(reinterpret_cast<void*>(from + 0x80), limbs.a[k], sizeof(limbs.a[k])) &&
                      mem::SafeRead(reinterpret_cast<void*>(to + 0x80), limbs.b[k], sizeof(limbs.b[k]));
    }
    return limbs;
}

// One of player 1's limbs in one of the enemy's hurt boxes: the part's name, nullptr if none; `zone` true when the enemy
// has no hurt box on (the old zone decides).
// `parts`: a mask of kLimbParts' indices (the striking limb only: the counters).
const char* LimbContact(const LimbPoints& limbs, uintptr_t enemy, bool& zone, uint32_t parts = 0xFF) {
    float boxes[32][6];
    const int count = HurtBoxes(enemy, boxes, 32);
    zone = count == 0;
    for (size_t k = 0; k < std::size(kLimbParts); ++k) {
        if (!limbs.ok[k] || !(parts & (1u << k))) continue;
        for (int s = 0; s <= 6; ++s) {  // points along the segment
            const float t = s / 6.0f;
            float point[3];
            for (int c = 0; c < 3; ++c) point[c] = limbs.a[k][c] + (limbs.b[k][c] - limbs.a[k][c]) * t;
            for (int i = 0; i < count; ++i)
                if (BoxDistance(point, boxes[i], boxes[i] + 3) <= kLimbParts[k].radius) return kLimbParts[k].name;
        }
    }
    return nullptr;
}

// The counter's blow, as the trainer's blows (user, 2026-11-01: "the enemy reacts before the contact; the zones as for
// the trainer's moves, their impact sound and the character's grunts"): from its window's start the played character's
// grunt, then the blow at the first contact of one of his limbs with its target's hurt shapes (LimbContact), with the
// native impact of a punch or a kick (by the limb: the played character's bank, as the trainer's own moves); an enemy
// without hurt shapes takes it at the motion's frame; past the window it missed.
bool g_counterSwung = false;  // render thread: its grunt played

void PlayCounterImpact(const char* limb) {
    const float gain = g_gruntGain.load(std::memory_order_relaxed);
    if (gain <= 0.0f) return;
    const bool punch = limb && (std::strstr(limb, "fist") || std::strstr(limb, "forearm"));
    PlayMoveSound("", 56, gain);
    PlayMoveSound("", punch ? 42 : 60, gain);
    PlayMoveSound("", punch ? 43 : 61, gain);
}

void TickCounterBlow(uintptr_t p, float frame) {
    const CounterMove& move = kCounters[g_counterVariant.load(std::memory_order_relaxed) == 1 ? 1 : 0];
    if (frame < move.from) return;
    if (!g_counterSwung) {
        g_counterSwung = true;
        PlayMoveGrunt(p);
    }
    const uintptr_t target = g_counterTarget.load();
    if (!g_counterPunchDue.load() || !target) return;
    if (frame > move.to) {
        g_counterPunchDue = false;
        LOG("wesker moves: the counter missed %08X (no contact by frame %.0f)", static_cast<unsigned>(target), move.to);
        return;
    }
    bool zone = false;
    // Played mirrored (layer 0's mode bit 0x100: a left-handed player, swaps.cpp ShapesWantMirror), the other side's limb.
    uint16_t mode = 0;
    uint32_t parts = move.parts;
    if (mem::Read(p + 0x380 + 0xA, mode) && (mode & 0x100))
        parts = ((parts & 0x33) << 2) | ((parts & 0xCC) >> 2);  // right forearm / fist / shin / foot <-> left
    const char* limb = LimbContact(ReadLimbs(p), target, zone, parts);
    if (!limb && !(zone && frame >= move.punch)) return;
    g_counterPunchDue = false;
    LOG("wesker moves: counter blow on %08X at frame %.0f (%s)", static_cast<unsigned>(target), frame,
        limb ? limb : "no hurt shape: its frame");
    LogCounterReach(p, target);
    PlayCounterImpact(limb);
    CounterPunchLands(target);
}

// Rebecca's duo move sprays fire from both hands (user, 2026-11-06: "she uses two sprays, one in each hand, so two
// flames"; the game makes them in its attack code from her melee entry +0x5C = 3, never run by a trainer move): her FIRE
// SPRAY's list pl1d14 (her borrowed Head Flash asks pl0014 at its start, 2026-11-06; pl1d09, taken first, is a body on
// fire: user) on her right hand, and its left-hand copy pl1d66 (tools/make_left_flame.py: the list hooks its jet to a
// joint itself, element record +0x3C - 9 the right hand -, the spawner's joint argument changes nothing; user: "she uses
// two sprays"), until frame 125 (her arms coming down). The list has its own delay
// of about a second before its jet (the game asks it at the FIRE SPRAY's start): made at frame 46, her spray's sound, the
// flame came a second late (user, 2026-11-06), so it is made at the move's start.
uint32_t g_moveFlames[2] = {};  // render thread
// The frame read when the move's motion is first seen can still be the previous motion's: the flames start at the next
// tick, once the move's own frames are read (still near its start).
const CombatMove* g_flameMove = nullptr;
float g_flamePart = 2.0f;

// The left-hand copy pl1d66 made by the trainer itself (user, 2026-11-06: "the mod will be shared: how will people
// recreate the left flame?" - every player owns the game): tools/make_left_flame.py's work on the player's own
// uPl07Eff.arc, written once to nativePC_MT\effect\efl\pl\pl1d\pl1d66.efl when it is missing. pl1d14 (an EFL: element
// count +0xC, its table at 0x20, 16 bytes each, the first dword's offset >> 8 from 0x20 leading to the element's first
// record): the elements on joint 9 / 8 (right hand / wrist, +0x3C) moved to 14 / 13 and turned 180 degrees about the
// joint's Y axis (place +0x30 (-x, y, -z), rotation +0x40 y180 x q). Game thread, at the duo move's first flame.
bool EnsureLeftFlameFile() {
    static int state = 0;  // 0 not tried, 1 there, 2 failed
    if (state) return state == 1;
    const std::wstring root = arc::GameDir() + L"\\nativePC_MT";
    const std::filesystem::path target = root + L"\\effect\\efl\\pl\\pl1d\\pl1d66.efl";
    std::error_code error;
    if (std::filesystem::exists(target, error)) {
        state = 1;
        return true;
    }
    std::vector<uint8_t> data;
    if (!arc::ReadEntry(root + L"\\Image\\Archive\\uPl07Eff.arc", "effect\\efl\\pl\\pl1d\\pl1d14", data) || data.size() < 0x20 ||
        std::memcmp(data.data(), "EFL\0", 4) != 0) {
        LOG("wesker moves: Rebecca's fire spray (uPl07Eff.arc, pl1d14) not read: no left-hand flame");
        state = 2;
        return false;
    }
    uint32_t count = 0;
    std::memcpy(&count, &data[0xC], 4);
    int changed = 0;
    for (uint32_t i = 0; i < count && 0x20 + 16 * (i + 1) <= data.size(); ++i) {
        uint32_t first = 0;
        std::memcpy(&first, &data[0x20 + 16 * i], 4);
        const size_t record = (first >> 8) + 0x20;
        if (record + 0x50 > data.size()) continue;
        uint32_t joint = 0;
        std::memcpy(&joint, &data[record + 0x3C], 4);
        if (joint != 9 && joint != 8) continue;
        joint = joint == 9 ? 14 : 13;
        std::memcpy(&data[record + 0x3C], &joint, 4);
        float place[3] = {}, q[4] = {};
        std::memcpy(place, &data[record + 0x30], sizeof(place));
        std::memcpy(q, &data[record + 0x40], sizeof(q));
        place[0] = -place[0];
        place[2] = -place[2];
        // (0, 1, 0, 0) x q, quaternions (x, y, z, w); 0 - v keeps a zero positive, as the Python tool writes it.
        const float turned[4] = { q[2], q[3], 0.0f - q[0], 0.0f - q[1] };
        std::memcpy(&data[record + 0x30], place, sizeof(place));
        std::memcpy(&data[record + 0x40], turned, sizeof(turned));
        ++changed;
    }
    std::filesystem::create_directories(target.parent_path(), error);
    std::ofstream file(target, std::ios::binary);
    if (!file || !file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        LOG("wesker moves: Rebecca's left-hand flame not written (pl1d66.efl)");
        state = 2;
        return false;
    }
    LOG("wesker moves: Rebecca's left-hand flame made (pl1d66.efl, %d of %u elements moved and turned)", changed, count);
    state = 1;
    return true;
}

void StopMoveFlames() {
    for (uint32_t& handle : g_moveFlames) {
        if (handle) StopGameEffect(handle);
        handle = 0;
    }
    g_flameMove = nullptr;
}

void TickMoveFlames(uintptr_t p, const CombatMove& move, float part) {
    if (std::strcmp(move.id, "melee.free_rebecca_tag_hook") != 0) return;
    constexpr float kTo = 125.0f / 156.0f;
    if (g_flameMove != &move) {
        g_flameMove = &move;
        g_flamePart = 2.0f;
    }
    // The second tick of the move, its frames its own - not a frame limit: the first loan after a trainer load installs
    // its hooks (about a second, 2026-11-06) and the move was past its start when its frames were first read.
    const bool started = g_flamePart <= 1.0f;
    g_flamePart = part;
    if (part >= kTo) {
        for (uint32_t& handle : g_moveFlames) {
            if (handle) StopGameEffect(handle);
            handle = 0;
        }
        return;
    }
    if (!started || g_moveFlames[0] || g_moveFlames[1]) return;
    g_moveFlames[0] = SpawnCharacterEffect(0x1D, 0x14, p, 0);
    if (EnsureLeftFlameFile()) g_moveFlames[1] = SpawnCharacterEffect(0x1D, 0x66, p, 0);
    LOG("wesker moves: Rebecca's flames on player 1's hands (%X, %X)", g_moveFlames[0], g_moveFlames[1]);
}

// RE6's dodges and slide (user, 2026-11-08): at their end he gets up, or stays down as long as the aim is held and shoots from
// the ground (the game's aim action, the ground pose on the hips and legs: Re6GroundLegs). Getting up later is the same choice.
int g_re6Ground = 0;        // 0 up, 1 lying, 2 ducked, 3 sliding with the aim held
// What plays under the aim (kind 3): 0-3 the dives forward / back / left / right, 4 the slide (user, 2026-11-08: "on the roll I cannot shoot until he is settled").
// aimFrame: the dives stay whole-body until their weapon is back in front (user, 2026-11-08: "not from the start, at one moment his head is upside down, but he must
// be able to shoot as soon as the weapon points forward"; measured from the hand's axis, and it stays there to the end).
struct Re6Layered { float frames; const char* move; float aimFrame; };
constexpr Re6Layered kRe6Layered[5] = { { 80.0f, "melee.free_re6_dodge_forward", 48.0f }, { 73.0f, "melee.free_re6_dodge_back", 10.0f },
                                        { 69.0f, "melee.free_re6_dodge_left", 56.0f }, { 69.0f, "melee.free_re6_dodge_right", 54.0f },
                                        { 109.0f, "melee.free_re6_slide", 0.0f } };
int g_re6LayeredWhich = 4;
// The frame each dive lands, its hips at the ground (forward, back, left, right; RE6's BhA BE / C0 / C2 / C4 read 2026-11-09): with the aim not
// held there he gets up at once (user: "in RE6 he gets up at once without the ground pose when I stop aiming, here he stops a second on the
// ground": the dives' last second is their slide and settling), with it held the dive goes on to its end and he stays down.
constexpr float kRe6DiveLanding[4] = { 42.0f, 42.0f, 42.0f, 48.0f };
std::atomic<DWORD> g_re6GroundMoveAsked{ 0 }, g_re6CrouchAsked{ 0 };  // the shortcuts' presses (GetTickCount | 1)
// Forward from the ground (user, 2026-11-09: "RE6's forward getup then a forward roll, a forward dash, which RE6 has not"): the getup forward (BhB 3E,
// 56 frames) then RE6's forward dive from its frame kDashDiveFrom, rising (hips at 75 cm; from 42, up at 94, "he runs a little before the dive", user).
constexpr float kDashDiveFrom = 30.0f;
// The ground shortcut's four directions faster (user, 2026-11-09: "the side rolls and the back dash seem slow next to my memory of RE6", x1.2,
// the forward getup and its dive too): player 1's unit time scale +0xFD4 times kRe6GroundSpeed while that move plays (the guard's way), given back after.
constexpr float kRe6GroundSpeed = 1.2f;
const CombatMove* g_re6Sped = nullptr;  // the move played faster
DWORD g_re6SpedSince = 0;
uintptr_t g_re6SpedPlayer = 0;
float g_re6SpedScale = 1.0f;  // his own before
bool g_re6ForwardDash = false;
DWORD g_re6ForwardDashSince = 0;  // the getup asked: the move may start a frame later
float g_re6SlideFrame = 0.0f;  // how far it got: the game resets the layer when the aim starts (user: "the slide repeats")
// RE6's slide pose until a shot (user, 2026-11-09, recorded in RE6: "the slide pose without aiming, one or two hands by the weapon, as long
// as I do not fire; the firing pose blended only when I fire"): the upper body plays re6_evade.lmt 48 / 49 (BhA / ArA C9) on the aim's three
// layers until the fire action 0x1E, then the slide's aim (31 / 34) to its end.
bool g_re6SlideShot = false;
float g_re6TurnStartYaw = 0.0f;  // the slide's half-turn: his facing at its start (logged at its end)
// The ground pose after a whole-body move (user, 2026-11-09: "during the half-turn the weapon goes up, so the switch to the ground pose
// jerks"): the aim's layers blended in over this many frames the first time (Re6EvadeEnded sets it).
float g_re6GroundEntryBlend = 0.0f;
constexpr float kTurnAimBlend = 18.0f;
// Then the aim's pitch (user, 2026-11-09: "Chris still aims too high during the half-turn, so if I aim low enough he snaps at the start of
// the ground pose"): the game weights the three aim layers by the pitch at once; the level pose on all three first (no pitch), then the
// up / down poses on layers 1 / 2 blended in over kPitchBlend frames.
ULONGLONG g_re6PitchFrom = 0;
constexpr ULONGLONG kLevelMs = 300;
constexpr float kPitchBlend = 14.0f;
constexpr uint16_t kRe6SlidePose = 48, kRe6SlideLong = 50;  // re6_evade.lmt: the pose (one hand; +1 two), the whole-body slide with two hands
// The knife's and the grenades' (user, 2026-11-09: "the half-turn with a grenade in hand uses the one-handed weapon pose"): RE6's KnA / BoA C9
// poses 55 / 56, their whole-body slides 57 / 58, their half-turns 53 / 54 (KnB / BoB 2D).
constexpr uint16_t kRe6SlidePoseKnife = 55, kRe6SlidePoseItem = 56, kRe6SlideKnife = 57, kRe6SlideItem = 58;
bool Re6SlidePoseSlot(uint16_t slot) {
    return slot == kRe6SlidePose || slot == kRe6SlidePose + 1 || slot == kRe6SlidePoseKnife || slot == kRe6SlidePoseItem;
}
// The slide's end (user, 2026-11-09: "the aim at the slide's end is not towards where I aim, so the switch to the ground pose jerks: let him
// orient his weapon where I aim as the aim's start does, one or two hands"): from its frame kSlideAimFrom the slide's aim, the game blending
// the pose into it over kSlideAimBlend frames, aimed when the ground pose comes (frame 0.97 x 109); that one blended in over kGroundAimBlend.
constexpr float kSlideAimFrom = 82.0f, kSlideAimBlend = 20.0f, kGroundAimBlend = 10.0f;
float g_moveBeginOverride = -1.0f;  // the next StartCombatMove's first frame (the slide's aim released, at the same point)
uint16_t g_re6GroundFrom = 0;  // the dodge he went down with
ULONGLONG g_re6GroundSince = 0;

uint32_t HeldMaskOf(uintptr_t p) {
    uintptr_t block = 0;
    uint32_t held = 0;
    return mem::Read(p + 0x2D64, block) && block && mem::Read(block + 0x220 + 0x51C, held) ? held : 0;
}

// The getup: by the direction held relative to his body (user: "the way he gets up depends on the direction he moves in during
// it"); the duck stands up its own way, the slide runs out of it only with the run held (else the forward getup).
bool StartRe6Getup(uintptr_t p, uint16_t from) {
    float forward = 0.0f, left = 0.0f;
    Player1MoveDirection(forward, left);
    const bool held = forward != 0.0f || left != 0.0f;
    const char* next = "melee.free_re6_getup";
    if (from == 5 || from == 46) next = "melee.free_re6_crouch_rise";  // the crouch's pose: the rest of the dodge in place
    else if (held && from == 4 && forward > 0.5f && (HeldMaskOf(p) & 0x4000)) next = "melee.free_re6_getup_slide";
    else if (held && forward >= std::fabs(left)) next = "melee.free_re6_getup_forward";
    else if (held && forward <= -0.3f) next = left >= 0.0f ? "melee.free_re6_getup_back_left" : "melee.free_re6_getup_back_right";
    else if (held) next = left > 0.0f ? "melee.free_re6_getup_left" : "melee.free_re6_getup_right";
    for (int k = 0; k < static_cast<int>(std::size(kCombat)); ++k)
        if (std::strcmp(kCombat[k].id, next) == 0) {
            const bool started = StartCombatMove(k, 0, true);
            LOG("re6 evade: %s: %s", next, started ? "started" : "failed");
            return started;
        }
    return false;
}

// RE6's hips and legs on the leg layer, its upper body on the game's three aiming layers (re6_evade.lmt 22.., by the weapon), so the aim moves it.
std::atomic<uint32_t> g_re6Layer7{ 0 }, g_re6Upper[3]{};  // what the SetMotion hook gives the game's own requests, game thread

// The reload and the weapon change keep the game's upper body (user, 2026-11-08: "no reload nor swap animation on the ground").
bool Re6UpperBodyIsGames(uintptr_t p) {
    uint32_t action = 0;
    if (!mem::Read(p + 0x10E0, action)) return false;
    if (action >= 0x1F && action <= 0x21) return true;
    // A pump or bolt worked between shots (user, 2026-11-09: "on the ground Chris does not pump the shotgun nor work the S75 between
    // shots"): the fire action 0x1E's own upper body for the Ithaca, the M3, the Jail Breaker, the Hydra and the S75.
    if (action == 0x1E) {
        const uint16_t item = PlayerHeldItem(p);
        if (item == 0x0104 || item == 0x0116 || item == 0x0117 || item == 0x0119 || item == 0x0105) return true;
    }
    // A knife, a grenade, an item: the game's draw, blow, throw or use (their loop only is RE6's pose): the blow waits for the draw's motion to end (user: "the knife does not strike").
    return Re6HeldPoseSlot(p) != 0 && action != 0x1C && action != 0x23 && action > 0x0A;
}

void Re6AimUpperBody(uintptr_t p) {
    const int held = Re6HeldPoseSlot(p);
    const bool slide = g_re6Ground == 3 && g_re6LayeredWhich == 4;
    const bool two = Re6WeaponTwoHanded(p);
    const bool slidePose = slide && !g_re6SlideShot && g_re6SlideFrame < kSlideAimFrom;
    const uint16_t slidePoseSlot = held == 28 ? kRe6SlidePoseKnife : held ? kRe6SlidePoseItem : static_cast<uint16_t>(kRe6SlidePose + (two ? 1 : 0));
    const uint16_t first = slidePose ? slidePoseSlot
                           : held    ? static_cast<uint16_t>(held)
                           : slide   ? (two ? 34 : 31)
                                     : (two ? 25 : 22);
    const bool level = GetTickCount64() < g_re6PitchFrom;  // after the half-turn: the pitch held level, then blended in
    static bool wasLevel = false;
    const bool pitchBack = wasLevel && !level;
    wasLevel = level;
    const bool same = held || slidePose || level;  // one pose on the three layers: no pitch
    const uint32_t layers[3] = { MoveMotion(kRe6EvadeBank, first), MoveMotion(kRe6EvadeBank, same ? first : first + 1),
                                 MoveMotion(kRe6EvadeBank, same ? first : first + 2) };
    for (int k = 0; k < 3; ++k) g_re6Upper[k] = layers[k];
    // The switch out of the slide's pose blended (the aim's start), quicker for a shot; the ground pose after the slide's aim too.
    static uint16_t last = 0;
    float blend = 4.0f;
    if (g_re6GroundEntryBlend > 0.0f) {
        blend = g_re6GroundEntryBlend;
        g_re6GroundEntryBlend = 0.0f;
        LOG("re6 evade: the ground's upper body blended in over %.0f frames", blend);
    }
    if (pitchBack) {
        blend = kPitchBlend;
        LOG("re6 evade: the aim's pitch blended in over %.0f frames", blend);
    }
    if (blend == 4.0f && last != first && Re6SlidePoseSlot(last)) blend = g_re6SlideShot ? 6.0f : kSlideAimBlend;
    else if (blend == 4.0f && last != first && (last == 31 || last == 34) && (first == 22 || first == 25)) blend = kGroundAimBlend;
    if (last != first && blend != 4.0f) LOG("re6 slide: upper body %u -> %u blended over %.0f frames (slide frame %.0f)", last, first, blend, g_re6SlideFrame);
    last = first;
    if (!Re6UpperBodyIsGames(p)) Re6AimLayers(p, layers, slide ? g_re6SlideFrame : 0.0f, slide, blend);
}

// The legs while he is down (user, 2026-11-08): the direction held crawls (re6_evade.lmt 37 forward, 38 back, 39 left, 40 right, moving him by their own root), the body turning with
// the aim plays the pivots (41 / 42, their own turn left out: the aim turns him); 0 = lying still.
int Re6CrawlSlot(uintptr_t p) {
    float forward = 0.0f, left = 0.0f;
    Player1BodyDirection(forward, left);
    if (Re6GroundLegsMirrored()) left = -left;  // mirrored, the left crawl's motion moves him right (as the aim walk's side steps)
    if (std::fabs(forward) > 0.3f || std::fabs(left) > 0.3f) {
        if (std::fabs(forward) >= std::fabs(left)) return forward > 0.0f ? 37 : 38;
        return left > 0.0f ? 39 : 40;
    }
    static float lastYaw = 0.0f;
    static ULONGLONG lastTick = 0, turning = 0;
    static int side = 0;
    const ULONGLONG now = GetTickCount64();
    const float yaw = YawOf(p);
    const float dt = lastTick ? static_cast<float>(now - lastTick) * 0.001f : 0.0f;
    if (dt > 0.0f && dt < 0.2f) {
        const float rate = Wrap(yaw - lastYaw) / dt;
        if (std::fabs(rate) > 0.35f) {
            turning = now;
            side = rate > 0.0f ? 41 : 42;
        }
    }
    if (now != lastTick) {
        lastYaw = yaw;
        lastTick = now;
    }
    if (side && Re6GroundLegsMirrored()) side = side == 41 ? 42 : 41;  // the pivots too
    return side && now - turning < 150 ? side : 0;
}

// On the ground; ducked, only his legs and hips (the game's aim above them).
void Re6GroundPose(uintptr_t p) {
    if (g_re6Ground == 2) {
        g_re6Layer7 = MoveMotion(kRe6EvadeBank, 46);
        Re6GroundLegs(p, g_re6Layer7, false);
        return;
    }
    const int crawl = Re6CrawlSlot(p);
    g_re6Layer7 = MoveMotion(kRe6EvadeBank, static_cast<uint16_t>(crawl ? crawl : Re6WeaponTwoHanded(p) ? 17 : 16));
    Re6GroundLegs(p, g_re6Layer7, false, 0.0f, crawl >= 37 && crawl <= 40);
    Re6AimUpperBody(p);
}

// The camera goes down and back with him (0..1, camera.cpp's sliders): smoothly, from the dives' start to their getups' start.
void TickRe6Camera(uintptr_t p) {
    static float now = 0.0f;
    static ULONGLONG last = 0;
    const ULONGLONG tick = GetTickCount64();
    const float dt = last ? std::min(static_cast<float>(tick - last) * 0.001f, 0.1f) : 0.0f;
    last = tick;
    float target = 0.0f;
    if (g_re6Ground == 2) target = 0.57f;
    else if (g_re6Ground) target = 1.0f;
    else if (p && PlayerInWeskerMove(p) && g_active.move && g_active.move->bank == kRe6EvadeBank &&
             (g_active.move->index <= 5 || (g_active.move->index >= 43 && g_active.move->index <= 47) || g_active.move->index == kRe6SlideTurn))
        target = g_active.move->index == 5 ? 0.57f : 1.0f;
    const float step = 3.0f * dt;
    now = target > now ? std::min(now + step, target) : std::max(now - step, target);
    SetRe6CameraDrop(now);
}

bool Re6EvadeEnded(uintptr_t p, const CombatMove& move) {
    // The dodge in place stands up again by itself, aim held or not (user, 2026-11-09: as in RE6), the back dash ends standing; the crouch's
    // descent and the ground rolls end down.
    const bool crouch = &move == &kCombat[kRe6CrouchMove];
    const bool roll = move.index == 43 || move.index == 44 || move.index == 47;  // the rolls and the back dash, on the ground
    const bool turn = move.index == kRe6SlideTurn;  // the slide's half-turn: as the slide's end
    if (move.bank != kRe6EvadeBank || (!crouch && !roll && !turn && move.index > 4)) return false;
    const uint16_t from = crouch ? 46 : turn ? 4 : move.index;
    if (turn) {
        // Facing back now: the aim's yaw with him (else the aim would pivot him back to the camera's way at once).
        const float yaw = YawOf(p);
        LOG("re6 slide: half-turn over, facing %.2f (%.2f at its start)", yaw, g_re6TurnStartYaw);
        mem::Write(p + 0x2A10, yaw);
        mem::Write(p + 0x1024, yaw);
    }
    if (!(HeldMaskOf(p) & 0xC00)) return StartRe6Getup(p, from);
    g_re6Ground = crouch ? 2 : 1;
    g_re6GroundFrom = from;
    if (turn) {
        g_re6GroundEntryBlend = kTurnAimBlend;
        g_re6PitchFrom = GetTickCount64() + kLevelMs;
    }
    g_re6GroundSince = GetTickCount64();
    Re6GroundPose(p);
    LOG("re6 evade: stays down aiming after %s (idle %s)", move.name, RequestPlayer(p, 0x00) ? "asked" : "failed");
    return true;
}

// A whole-body dive with the aim not held: up at its landing.
void TickRe6DiveLanding(uintptr_t p) {
    if (!p || g_re6Ground || !PlayerInWeskerMove(p) || !g_active.move || g_active.move->bank != kRe6EvadeBank || g_active.move->index > 3 ||
        (HeldMaskOf(p) & 0xC00))
        return;
    uint32_t motion = 0;
    float frame = 0.0f;
    if (!mem::Read(p + 0x384, motion) || (motion & 0xFFFF) != (MoveMotion(kRe6EvadeBank, g_active.move->index) & 0xFFFF) ||
        !mem::Read(p + 0x398, frame) || frame < kRe6DiveLanding[g_active.move->index])
        return;
    LOG("re6 evade: %s landed at frame %.0f, the aim not held: up at once", g_active.move->name, frame);
    StartRe6Getup(p, g_active.move->index);
}

bool Fresh(std::atomic<DWORD>& asked) {
    const DWORD tick = asked.exchange(0);
    return tick && static_cast<int32_t>(GetTickCount() - tick) < 300;
}

bool StartCombatMoveById(const char* id, bool faster = false) {
    const int k = CombatIndex(id);
    if (k < 0 || !StartCombatMove(k, 0, true)) return false;
    if (faster) {
        g_re6Sped = &kCombat[k];
        g_re6SpedSince = GetTickCount();
    }
    return true;
}

void TickRe6GroundSpeed(uintptr_t p) {
    const bool playing = p && g_re6Sped && PlayerInWeskerMove(p) && g_active.move == g_re6Sped;
    if (!playing) {
        if (g_re6SpedPlayer) mem::Write(g_re6SpedPlayer + 0xFD4, g_re6SpedScale);
        g_re6SpedPlayer = 0;
        if (g_re6Sped && GetTickCount() - g_re6SpedSince > 300) g_re6Sped = nullptr;  // past its start: over
        return;
    }
    if (g_re6SpedPlayer != p) {
        float scale = 1.0f;
        if (!mem::Read(p + 0xFD4, scale) || scale < 0.5f || scale > 2.0f) return;  // not the plain 1.0 expected
        g_re6SpedPlayer = p;
        g_re6SpedScale = scale;
    }
    mem::Write(p + 0xFD4, g_re6SpedScale * kRe6GroundSpeed);
}

// "Au sol : roulade ou dash arrière (RE6)": lying, left / right held rolls that way, back the back dash on the ground, forward the getup forward
// then the forward dive, none the back roll that stands him up.
// "Se baisser en visant": while he aims standing, down in the frozen low pose; pressed again (or the aim let go), up.
void TickRe6Shortcuts(uintptr_t p) {
    if (!p) return;
    if (Fresh(g_re6GroundMoveAsked)) {
        if (g_re6Ground != 1) {
            LOG("re6 ground move: not lying (ground %d)", g_re6Ground);
        } else if (!StaminaAllows()) {
            LOG("re6 ground move: refused, the stamina's red arc");
        } else {
            float forward = 0.0f, left = 0.0f;
            Player1MoveDirection(forward, left);
            const bool side = std::fabs(left) >= 0.3f && std::fabs(left) > std::fabs(forward);
            const bool along = std::fabs(forward) >= 0.3f && !side;
            const char* id = side                    ? (left > 0.0f ? "melee.free_re6_ground_roll_left" : "melee.free_re6_ground_roll_right")
                             : along && forward < 0.0f ? "melee.free_re6_ground_back_dash"
                             : along                  ? "melee.free_re6_getup_forward"
                                                      : "melee.free_re6_ground_back_roll";
            g_re6ForwardDash = along && forward > 0.0f;
            g_re6ForwardDashSince = GetTickCount();
            g_re6Ground = 0;
            Re6GroundLegsOff(p);
            const bool started = StartCombatMoveById(id, std::strcmp(id, "melee.free_re6_ground_back_roll") != 0);
            if (started) StaminaSpendDodge(DodgeCostOf(g_re6ForwardDash ? "melee.free_re6_dodge_forward" : id));  // the forward dash: its dive's
            LOG("re6 ground move: %s %s", id, started ? "started" : "failed");
        }
    }
    if (g_re6ForwardDash) {  // the getup forward nearly done: the dive
        uint32_t motion = 0;
        float frame = 0.0f;
        const int getup = CombatIndex("melee.free_re6_getup_forward");
        const bool settling = GetTickCount() - g_re6ForwardDashSince < 300;
        if (!PlayerInWeskerMove(p) || !g_active.move || g_active.move != &kCombat[getup]) {
            if (!settling) g_re6ForwardDash = false;
        } else if (mem::Read(p + 0x384, motion) && (motion & 0xFFFF) == (MoveMotion(kRe6EvadeBank, g_active.move->index) & 0xFFFF) &&
                   mem::Read(p + 0x398, frame) && frame >= kDashDiveFrom) {
            g_re6ForwardDash = false;
            LOG("re6 ground move: forward dash, the dive from the getup's frame %.0f %s", frame,
                StartCombatMoveById("melee.free_re6_dodge_forward", true) ? "started" : "failed");
        }
    }
    if (Fresh(g_re6CrouchAsked)) {
        uint32_t action = 0;
        mem::Read(p + 0x10E0, action);
        if (g_re6Ground == 2) {
            g_re6Ground = 0;
            Re6GroundLegsOff(p);
            LOG("re6 crouch: up %s", StartRe6Getup(p, 46) ? "started" : "failed");
        } else if (!g_re6Ground && action >= 0x1B && action <= 0x1E && !PlayerInWeskerMove(p)) {
            LOG("re6 crouch: down %s", StartCombatMove(kRe6CrouchMove) ? "started" : "failed");
        } else {
            LOG("re6 crouch: refused (ground %d, action %X)", g_re6Ground, action);
        }
    }
}

// RE6's half-turn in the slide (user, 2026-11-09: "the half-turn when I go backwards during the slide"; recorded in RE6: the slide cut at its
// frame ~39 for BhB 2C, its root turning 180 degrees in 30 frames as it slides on, lying facing back at its end): backwards held during either
// slide (under the aim or whole-body), from its frame kTurnFrom to kTurnTo, plays it, then down aiming or up as at a slide's end (Re6EvadeEnded).
constexpr float kTurnFrom = 10.0f, kTurnTo = 70.0f;
void TickRe6SlideTurn(uintptr_t p) {
    if (!p) return;
    float frame = -1.0f;
    const bool layered = g_re6Ground == 3 && g_re6LayeredWhich == 4;
    if (layered) {
        frame = g_re6SlideFrame;
    } else if (!g_re6Ground && PlayerInWeskerMove(p) && g_active.move && g_active.move->bank == kRe6EvadeBank && g_active.move->index == 4) {
        uint32_t motion = 0;
        if (mem::Read(p + 0x384, motion) && (motion & 0xFFFF) == g_active.motion) mem::Read(p + 0x398, frame);
    }
    if (frame < kTurnFrom || frame > kTurnTo) return;
    float forward = 0.0f, left = 0.0f;
    Player1BodyDirection(forward, left);
    if (forward > -0.5f) return;
    if (layered) {
        g_re6Ground = 0;
        Re6GroundLegsOff(p);
    }
    g_re6TurnStartYaw = YawOf(p);
    LOG("re6 slide: backwards at its frame %.0f, the half-turn %s", frame, StartCombatMove(kRe6SlideTurnMove, 0, true) ? "started" : "failed");
}

// An item the game does not aim, lying down (user, 2026-11-10: "Chris does not pivot to follow the camera on the ground in the third person with an
// ammunition box"): the game's idle camera turns about him; his body turned towards the camera's yaw (P+0x2A10) as the aim does, at most
// kGroundTurnRate radians a second (the ground's pivots play from his turning, Re6CrawlSlot).
constexpr float kGroundTurnRate = 4.0f;
void Re6TurnWithCamera(uintptr_t p) {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    const float dt = last && now - last < 200 ? static_cast<float>(now - last) * 0.001f : 0.0f;
    last = now;
    float camera = 0.0f;
    if (!mem::Read(p + 0x2A10, camera)) return;
    const float body = YawOf(p), diff = Wrap(camera - body), step = kGroundTurnRate * dt;
    if (std::fabs(diff) < 1e-3f) return;
    const float yaw = std::fabs(diff) <= step ? camera : Wrap(body + (diff > 0.0f ? step : -step));
    mem::Write(p + 0x1024, yaw);
    const float quat[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x40), quat, sizeof(quat));
}

void TickRe6Ground() {
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    TickRe6Shortcuts(p1);
    TickRe6GroundSpeed(p1);
    TickRe6DiveLanding(p1);
    TickRe6SlideTurn(p1);
    if (!g_re6Ground) return;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action)) {
        g_re6Ground = 0;
        return;
    }
    const bool reloading = action >= 0x1F && action <= 0x21;
    const bool aiming = (action >= 0x1B && action <= 0x1E) || (action >= 0x22 && action <= 0x27) || reloading;
    const bool waiting = action == 0 && GetTickCount64() - g_re6GroundSince < 500;
    const bool keyHeld = (HeldMaskOf(p) & 0xC00) != 0;
    if (g_re6Ground == 3) {
        // RE6's slide with the aim held (user, 2026-11-08: "if the aim is held he aims during the slide, not just at its end"):
        // its legs and hips on the leg layer under the game's aim, moving him by their own root; the aim released,
        // the whole-body slide from the same frame (a tap gives that one from the start).
        const uint16_t slideSlot = g_re6LayeredWhich == 4 ? (Re6WeaponTwoHanded(p) ? 19 : 18) : static_cast<uint16_t>(g_re6LayeredWhich);
        const uint32_t motion = MoveMotion(kRe6EvadeBank, slideSlot);
        const uintptr_t record = p + 0x380 + 7 * 0xE0;
        float frame = 0.0f;
        uint16_t current = 0;
        mem::Read(record + 0x18, frame);
        mem::Read(record + 4, current);
        const bool playing = current == motion;
        if (playing && frame > g_re6SlideFrame) g_re6SlideFrame = frame;
        if (!keyHeld) {
            g_re6Ground = 0;
            Re6GroundLegsOff(p);
            // Down already: up at once (user, 2026-11-08: "as soon as I let go of the aim, the getup; there is a dead time on the ground"). A tap at its very start stays the whole slide.
            if ((g_re6LayeredWhich == 4 && g_re6SlideFrame >= 30.0f) ||
                (g_re6LayeredWhich < 4 && g_re6SlideFrame >= kRe6DiveLanding[g_re6LayeredWhich])) {
                StartRe6Getup(p, static_cast<uint16_t>(g_re6LayeredWhich));
                return;
            }
            for (int k = 0; k < static_cast<int>(std::size(kCombat)); ++k)
                if (std::strcmp(kCombat[k].id, kRe6Layered[g_re6LayeredWhich].move) == 0) {
                    g_moveBeginOverride = g_re6SlideFrame;
                    LOG("re6 slide: aim released at frame %.0f, whole body %s", frame, StartCombatMove(k, 0, true) ? "started" : "failed");
                    g_moveBeginOverride = -1.0f;
                }
            return;
        }
        if (GetTickCount64() - g_re6GroundSince > 400 && !aiming) {
            LOG("re6 slide: stopped (action %X)", action);
            g_re6Ground = 0;
            Re6GroundLegsOff(p);
            return;
        }
        g_re6Layer7 = motion;
        if (g_re6LayeredWhich == 4 && action == 0x1E && !g_re6SlideShot) {
            g_re6SlideShot = true;
            LOG("re6 slide: a shot at frame %.0f, the slide's aim from here", g_re6SlideFrame);
        }
        Re6GroundLegs(p, motion, false, g_re6SlideFrame, true);  // the game moves him by its root: the same distance as the whole-body slide, and its collisions
        Re6AimUpperBody(p);
        if (playing && frame >= kRe6Layered[g_re6LayeredWhich].frames * 0.97f) {
            g_re6Ground = 1;
            g_re6GroundFrom = static_cast<uint16_t>(g_re6LayeredWhich);
            g_re6GroundSince = GetTickCount64();
        }
        return;
    }
    // The aim let go while he runs off (user: "no getup, he teleports up"): the game is already in its walk or run, actions up to 0xA.
    const bool unaimable = keyHeld && action <= 0xA && Re6HeldPoseSlot(p) != 0;  // an item the game does not aim: still down
    if (!aiming && !waiting && !unaimable && !(action <= 0xA && !keyHeld)) {  // a hit, a grab, another dodge...: the game's from here
        LOG("re6 evade: up without a getup (action %X)", action);
        g_re6Ground = 0;
        Re6GroundLegsOff(p);
        return;
    }
    if (!keyHeld && !reloading) {
        const uint16_t from = g_re6GroundFrom;
        g_re6Ground = 0;
        Re6GroundLegsOff(p);
        StartRe6Getup(p, from);
        return;
    }
    if (unaimable) Re6TurnWithCamera(p);
    Re6GroundPose(p);
}

// The slide playing (its whole-body move or, under the aim, its legs on layer 7: TickRe6Ground's state 3): from its window's start the grunt,
// then each enemy its legs reach (LimbContact on the shins and feet; the zone in front for one without hurt shapes) hit once per slide.
constexpr float kSlideHitFrom = 6.0f, kSlideHitTo = 60.0f;  // frames of RE6's slide (BhA C8, 109): from the drop to the getup's point

// The slide breaks the crates (user, 2026-11-09). A wooden crate (uOm0890, vtable RVA 0xBC1E68) is a unit as the enemies: health int16 +0x1364 (1),
// action +0x10E0 (0, broken 1), the attacker's pad +0x136C, position +0x30 (its base). Its OnHit (vtable +0x104, VA 0xA943B0, ret 0x14; the
// hit dispatcher calls it at VA 0x77A084) while whole (byte +0x19E0 clear): a player attacker's pad to +0x136C (not one whose +0x2DA8 is 2),
// its hit shape's collision call (VA 0xD3DAF0), its break action Request(1, 1, 0, -1, 0) (+0xDC = the request core VA 0xB59030, the
// enemies'), SetHealth(0) (+0x10C, VA 0x84F740) and VA 0x791FF0(crate, 7, 0) on [0x1249C1C] (a manager that only acts with its session flag
// set: the online sync). Traced and read live 2026-11-09 (one knife slash broke it; its break action's setup writes the health again).
// The trainer does the same without the collision call for every level object (sObjModel) whose OnHit is that one, whole and alive, a
// shin or foot of Chris within 55 cm of its axis and up to 1 m above its base.
constexpr uintptr_t kCrateOnHit = 0x6943B0;  // RVA
constexpr float kCrateRadius = 55.0f, kCrateHeight = 100.0f;

bool BreakCrate(uintptr_t p, uintptr_t crate) {
    uint8_t pad = 0;
    if (mem::Read(p + 0x2D9C, pad)) mem::Write<int32_t>(crate + 0x136C, pad);
    if (!RequestEnemy(crate, 1)) return false;
    uintptr_t vtable = 0, setHealth = 0, manager = 0;
    __try {
        if (mem::Read(crate, vtable) && mem::Read(vtable + 0x10C, setHealth) && setHealth)
            reinterpret_cast<void(__fastcall*)(void*, void*, int)>(setHealth)(reinterpret_cast<void*>(crate), nullptr, 0);
        if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C1C)), manager) && manager)
            reinterpret_cast<void(__fastcall*)(void*, void*, uintptr_t, int, int)>(mem::Rva(0x391FF0))(reinterpret_cast<void*>(manager),
                                                                                                   nullptr, crate, 7, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return true;
}

void SlideBreakCrates(uintptr_t p, const LimbPoints& limbs, const float* pos, uint32_t parts = 0xF0) {
    uintptr_t om = 0, array = 0;
    uint32_t count = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2720)), om) || !om || !mem::Read(om + 0x24, count) ||
        !mem::Read(om + 0x30, array) || !array || count > 4096)
        return;
    const uintptr_t onHit = reinterpret_cast<uintptr_t>(mem::Rva(kCrateOnHit));
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t o = 0, vtable = 0, fn = 0;
        float at[3]{};
        if (!mem::Read(array + 4 * i, o) || !o || !mem::SafeRead(reinterpret_cast<void*>(o + 0x30), at, sizeof(at))) continue;
        const float dx = at[0] - pos[0], dz = at[2] - pos[2];
        if (dx * dx + dz * dz > 300.0f * 300.0f || !mem::Read(o, vtable) || !mem::Read(vtable + 0x104, fn) || fn != onHit) continue;
        uint8_t broken = 1;
        int16_t health = 0;
        if (!mem::Read(o + 0x19E0, broken) || broken || !mem::Read(o + 0x1364, health) || health <= 0) continue;
        bool touched = false;
        for (size_t k = 0; k < std::size(kLimbParts) && !touched; ++k) {  // the parts asked (the slide: shins and feet)
            if (!limbs.ok[k] || !(parts & (1u << k))) continue;
            for (int s = 0; s <= 4 && !touched; ++s) {
                const float t = s / 4.0f;
                float point[3];
                for (int c = 0; c < 3; ++c) point[c] = limbs.a[k][c] + (limbs.b[k][c] - limbs.a[k][c]) * t;
                const float px = point[0] - at[0], pz = point[2] - at[2];
                touched = px * px + pz * pz <= kCrateRadius * kCrateRadius && point[1] >= at[1] - 20.0f &&
                          point[1] <= at[1] + kCrateHeight;
            }
        }
        if (!touched) continue;
        const bool broke = BreakCrate(p, o);
        LOG("re6 slide: crate %s %08X %s", UnitClassName(o), static_cast<unsigned>(o), broke ? "broken" : "refused");
    }
}

void TickSlideBlow(uintptr_t p) {
    float frame = -1.0f;
    if (p && g_re6Ground == 3 && g_re6LayeredWhich == 4) {
        frame = g_re6SlideFrame;
    } else if (p && g_active.move && g_active.move->bank == kRe6EvadeBank && g_active.move->index == 4 && PlayerInWeskerMove(p)) {
        uint32_t motion = 0;
        if (mem::Read(p + 0x384, motion) && (motion & 0xFFFF) == g_active.motion) mem::Read(p + 0x398, frame);
    }
    if (frame < 0.0f) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_slideBlow.seen > 250) g_slideBlow = {};  // a new slide (the aim's switch between its two forms keeps the same one)
    g_slideBlow.seen = now;
    if (frame < kSlideHitFrom || frame > kSlideHitTo) return;
    if (!g_slideBlow.swung) {
        g_slideBlow.swung = true;
        PlayMoveGrunt(p);
    }
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return;
    const float facing = YawOf(p);
    const LimbPoints limbs = ReadLimbs(p);
    for (uintptr_t e : actors::Enemies()) {
        bool done = false;
        for (int k = 0; k < g_slideBlow.hits; ++k) done = done || g_slideBlow.hit[k] == e;
        float ep[3]{};
        int16_t health = 0;
        if (done || g_slideBlow.hits >= 16 || !mem::Read(e + 0x1364, health) || health <= 0 ||
            !mem::SafeRead(reinterpret_cast<void*>(e + 0x30), ep, sizeof(ep)))
            continue;
        const float dx = ep[0] - pos[0], dz = ep[2] - pos[2];
        if (dx * dx + dz * dz > 600.0f * 600.0f) continue;
        bool zone = false;
        const char* limb = LimbContact(limbs, e, zone, 0xF0);  // shins and feet
        if (zone) {
            if (dx * dx + dz * dz > 120.0f * 120.0f || std::fabs(ep[1] - pos[1]) > kHeight ||
                std::fabs(Wrap(std::atan2(dx, dz) - facing)) > kArc)
                continue;
        } else if (!limb) {
            continue;
        }
        LOG("re6 slide: %s on %08X at frame %.0f", limb ? limb : "no hurt shape: the zone", static_cast<unsigned>(e), frame);
        g_slideBlow.hit[g_slideBlow.hits++] = e;
        if (g_slideBlow.hits == 1) PlayMoveImpact(kSlideBlow, e);
        Strike(p, e, kSlideBlow);
    }
    SlideBreakCrates(p, limbs, pos);
}

// ---- The RE6 melee (user, 2026-11-09: "the attack key: a first blow (a hit received, no prompt); pressed again, the second, stronger,
// that opens a chosen prompt ... each character his own two-blow combo, an option in each character's free melee tab; attack with
// the direction back: the back elbow; sprint + attack: the shoulder charge ...; before the quick shot or shooting without aiming when
// an enemy is near, after them otherwise"). Per character (Player1CharacterIndex, swaps.cpp's tab choices): the combo (0 none: the
// attack key does what it did), the sprint attack (shoulder / jump kick / variant / random), the elbow (n° 30 / 31 / random). An enemy
// within kRe6MeleeReach (kRe6SprintReach sprinting): sprinting the sprint attack, the direction back the elbow, without stamina the
// tired blow, a two-handed weapon its generic combo, else the character's combo; no enemy but a whole crate in front: the crate blow
// (high above his feet or on the ground). The press is taken a frame later (the RE6 parry on the same key goes first). The second blow:
// pressed during the first, it starts at the first's window end + 4 frames; pressed within 0.5 s after the first, at once.
constexpr float kRe6MeleeReach = 250.0f, kRe6SprintReach = 400.0f, kRe6CrateReach = 160.0f;
std::atomic<int> g_re6ComboOf[8]{}, g_re6SprintOf[8]{}, g_re6ElbowOf[8]{};
// "Aléatoire" check boxes (user, 2026-11-09: "I see no box to tick for the random"): every variant drawn at each blow.
std::atomic<bool> g_re6SprintRandom[8]{}, g_re6ElbowRandom[8]{};
// The finisher (user, 2026-11-09: "the finisher, a choice too, is played by attacking an enemy already staggered"): per character, 0 the
// combo's own (its RE6 character's), 1 the game's Finisher Front, 2.. the RE6 finishers of kRe6Finishers.
std::atomic<int> g_re6FinisherOf[8]{};
constexpr int kRe6Finishers[8] = { CombatIndex("melee.free_re6_chris_finisher"), CombatIndex("melee.free_re6_leon_finisher"),
                                   CombatIndex("melee.free_re6_helena_finisher"), CombatIndex("melee.free_re6_piers_finisher"),
                                   CombatIndex("melee.free_re6_jake_finisher"), CombatIndex("melee.free_re6_sherry_finisher"),
                                   CombatIndex("melee.free_re6_ada_finisher"), CombatIndex("melee.free_re6_agent_finisher") };
constexpr int kComboFinisher[6] = { 0, 0, 1, 2, 4, 5 };  // the combo's RE6 character: none, Chris, Leon, Helena, Jake, Sherry

// A Majini standing in a stagger that opens melee (its head stagger with a prompt, not the flinch: DmgHead's +0x24 0; the arm / leg
// hits, the stumbles).
bool Re6Staggered(uintptr_t enemy) {
    uint32_t action = 0;
    if (!IsMajiniEnemy(enemy) || EnemyOnGround(enemy) || !mem::Read(enemy + 0x10E0, action)) return false;
    if (action == 0x6E || action == 0x70 || action == 0x7D || action == 0x7E) return true;
    if (action != 0x6D) return false;
    uintptr_t actions = 0, act = 0;
    uint32_t variant = 0;
    return mem::Read(enemy + 0x10DC, actions) && actions && mem::Read(actions + 4 * 0x6D, act) && act && mem::Read(act + 0x24, variant) &&
           variant != 0;
}
struct Re6Chain {
    int first = -1, second = -1;
    bool asked = false, done = false;
    DWORD since = 0, ended = 0;
};
Re6Chain g_re6Chain;  // render thread

// A whole crate within reach in front of player 1 (SlideBreakCrates' kind), its base in `at` and its height above his feet.
uintptr_t CrateInFront(uintptr_t p, float* at, float& height) {
    uintptr_t om = 0, array = 0;
    uint32_t count = 0;
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos)) ||
        !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2720)), om) || !om || !mem::Read(om + 0x24, count) ||
        !mem::Read(om + 0x30, array) || !array || count > 4096)
        return 0;
    const uintptr_t onHit = reinterpret_cast<uintptr_t>(mem::Rva(kCrateOnHit));
    const float facing = YawOf(p);
    uintptr_t best = 0;
    float bestDist = kRe6CrateReach * kRe6CrateReach;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t o = 0, vtable = 0, fn = 0;
        float c[3]{};
        if (!mem::Read(array + 4 * i, o) || !o || !mem::SafeRead(reinterpret_cast<void*>(o + 0x30), c, sizeof(c))) continue;
        const float dx = c[0] - pos[0], dz = c[2] - pos[2], d = dx * dx + dz * dz;
        if (d >= bestDist || std::fabs(Wrap(std::atan2(dx, dz) - facing)) > 1.2f || !mem::Read(o, vtable) ||
            !mem::Read(vtable + 0x104, fn) || fn != onHit)
            continue;
        uint8_t broken = 1;
        int16_t health = 0;
        if (!mem::Read(o + 0x19E0, broken) || broken || !mem::Read(o + 0x1364, health) || health <= 0) continue;
        bestDist = d;
        best = o;
        for (int k = 0; k < 3; ++k) at[k] = c[k];
        height = c[1] - pos[1];
    }
    return best;
}

// The environmental executions (below): the one an obstacle near `enemy` allows within the option's tolerance (g_envPlan), else -1.
struct EnvPlan {
    uintptr_t enemy = 0;
    int move = -1;
    float yaw = 0.0f;      // the pair's (towards the obstacle)
    float origin[3]{};     // player 1's place for it
    float height = 0.0f;   // from a height: the ledge's (player 1's height to the enemy's)
    DWORD tick = 0;
};
EnvPlan g_envPlan;  // the plan of the move about to start (game thread: the prompts; render thread: StartCombatMove), g_envLock
// The plans checked on the game thread every 100 ms (EnvExecTick): against an obstacle for the staggered enemy nearest player 1, and from
// a height; the render thread's starts (the RE6 melee, the free melee shortcuts) take them from here.
EnvPlan g_envWallCache, g_envHeightCache;
std::mutex g_envLock;

int Re6ComboChoice() {
    const int character = Player1CharacterIndex();
    return character >= 0 && character < 8 ? std::clamp(g_re6ComboOf[character].load(), 0, 5) : 0;
}

// The game's prompts first (user, 2026-11-09: "below the prompts: with a prompt shown Chris did the RE6 melee"), and only from free
// movement (idle, walking, running, turning: actions up to 0x09), never over a game move in progress.
bool Re6MeleeFree(uintptr_t p) {
    uint32_t action = 0;
    return p && mem::Read(p + 0x10E0, action) && action <= 0x09 && !Re6MeleePromptShown();
}

bool Re6MeleeWouldTake(uintptr_t p) {
    if (!p || Re6OnGround() || !Re6ComboChoice() || PlayerInWeskerMove(p) || !Re6MeleeFree(p)) return false;
    float at[3]{}, height = 0.0f;
    // No other option on the attack key (AttackKeyTakenElsewhere): every press is the RE6 melee's, no enemy near too.
    return !AttackKeyTakenElsewhere() || NearestEnemy(p, Player1Sprinting() ? kRe6SprintReach : kRe6MeleeReach, at) ||
           CrateInFront(p, at, height);
}

bool Re6MeleeStart(uintptr_t p) {
    const int combo = Re6ComboChoice();
    const int character = Player1CharacterIndex();
    if (!combo || character < 0 || character >= 8) return false;
    float at[3]{};
    const bool sprint = Player1Sprinting();
    const uintptr_t enemy = NearestEnemy(p, sprint ? kRe6SprintReach : kRe6MeleeReach, at);
    const int pick = static_cast<int>(GetTickCount() / 7);  // the random choices
    int first = -1, second = -1;
    const char* why = "";
    float height = 0.0f;
    if (!enemy && CrateInFront(p, at, height)) {
        FacePlayerTowards(p, at);
        first = height > 50.0f ? kRe6CrateHigh : kRe6CrateLow;
        why = height > 50.0f ? "a crate above" : "a crate on the ground";
    } else if (!enemy && AttackKeyTakenElsewhere()) {
        return false;  // the quick shot, "Tir sans viser" or the quick knife takes the press then
    } else {
        // An enemy near, or none with no other option on the attack key: the blows where he faces.
        float forward = 0.0f, left = 0.0f;
        Player1BodyDirection(forward, left);
        if (sprint) {
            int choice = std::clamp(g_re6SprintOf[character].load(), 0, 2);
            if (g_re6SprintRandom[character].load()) choice = pick % 3;
            first = kRe6Sprints[choice];
            why = "sprinting";
        } else if (forward < -0.5f) {
            int choice = std::clamp(g_re6ElbowOf[character].load(), 0, 1);
            if (g_re6ElbowRandom[character].load()) choice = pick % 2;
            first = kRe6Elbows[choice];
            why = "backwards";
        } else if (enemy && Re6Staggered(enemy) && EnvCachedWall(enemy) >= 0) {
            // An obstacle near the staggered enemy ("Exécutions contre le décor (RE6)"): its execution against it (forced on it below). The
            // plan made on the game thread (the collision probe is the game's: never called from this render thread).
            const int env = EnvCachedWall(enemy);
            const bool started = StartCombatMove(env, enemy, false);
            LOG("re6 melee: %s on the staggered %08X %s", kCombat[env].name, static_cast<unsigned>(enemy), started ? "started" : "failed");
            g_re6Chain = {};
            return started;
        } else if (enemy && Re6Staggered(enemy)) {
            int choice = std::clamp(g_re6FinisherOf[character].load(), 0, 9);
            if (choice == 1) {
                const bool played = PlayNativeMeleeMove(0x35);
                LOG("re6 melee: the game's Finisher Front on the staggered %08X %s", static_cast<unsigned>(enemy), played ? "asked" : "refused");
                g_re6Chain = {};
                return played;
            }
            first = kRe6Finishers[choice == 0 ? kComboFinisher[combo] : choice - 2];
            why = "a staggered enemy";
        } else if (!StaminaAllows()) {
            LOG("re6 melee: no stamina, %s on %08X %s", kCombat[kRe6Tired].name, static_cast<unsigned>(enemy),
                StartCombatMove(kRe6Tired, 0, true) ? "started" : "failed");  // chained: past the red arc's refusal, no targeting
            g_re6Chain = {};
            return true;
        } else if (Re6WeaponTwoHanded(p)) {
            first = kRe6Long1;
            second = kRe6Long2;
            why = "a two-handed weapon";
        } else {
            first = kRe6Combo[combo][0];
            second = kRe6Combo[combo][1];
            why = "the combo";
        }
    }
    const bool started = first >= 0 && StartCombatMove(first);
    LOG("re6 melee: %s (%s) %s", first >= 0 ? kCombat[first].name : "?", why, started ? "started" : "failed");
    g_re6Chain = {};
    if (started) {
        g_re6Chain.first = first;
        g_re6Chain.second = second;
        g_re6Chain.since = GetTickCount();
    }
    return started;
}

void TickRe6Melee(uintptr_t p) {
    static bool was = false;
    static DWORD pending = 0;
    const bool held = p && Re6MeleeFireHeld();
    const bool edge = held && !was;
    was = held;
    Re6Chain& c = g_re6Chain;
    if (!p || !Re6ComboChoice()) {
        c = {};
        pending = 0;
        return;
    }
    const DWORD now = GetTickCount();
    if (c.first >= 0 && c.second >= 0 && !c.done) {
        const bool firstOn = PlayerInWeskerMove(p) && g_active.move == &kCombat[c.first];
        if (firstOn && c.asked) {
            uint32_t motion = 0;
            float frame = 0.0f, length = 0.0f;
            if (mem::Read(p + 0x384, motion) && (motion & 0xFFFF) == g_active.motion && mem::Read(p + 0x398, frame) &&
                mem::Read(p + 0x39C, length) && length > 0.0f && frame >= kCombat[c.first].to * length + 4.0f) {
                c.done = true;
                LOG("re6 melee: %s chained at frame %.0f %s", kCombat[c.second].name, frame,
                    StartCombatMove(c.second, 0, true) ? "started" : "failed");
            }
        } else if (!firstOn && now - c.since > 300) {
            if (c.asked) {
                c.done = true;
                LOG("re6 melee: %s after the first %s", kCombat[c.second].name, StartCombatMove(c.second) ? "started" : "failed");
            } else if (!c.ended) {
                c.ended = now | 1;
            }
        }
    }
    if (edge && !Re6OnGround() && !Re6MeleePromptShown()) {
        if (c.first >= 0 && c.second >= 0 && !c.done) {
            if (PlayerInWeskerMove(p) && g_active.move == &kCombat[c.first]) {
                c.asked = true;
                return;
            }
            if (c.ended && now - c.ended < 500 && Re6MeleeFree(p)) {
                c.done = true;
                LOG("re6 melee: %s pressed after the first %s", kCombat[c.second].name, StartCombatMove(c.second) ? "started" : "failed");
                return;
            }
        }
        if (Re6MeleeFree(p)) pending = now | 1;  // a parry on the same press cuts it (CutMoveForParry)
    }
    if (pending && now - pending >= 30) {
        pending = 0;
        const DWORD cut = g_parryCut.load();
        if (!PlayerInWeskerMove(p) && Re6MeleeFree(p) && !(cut && now - cut < 500)) Re6MeleeStart(p);
    }
}

// The parry's cut done on the render thread before anything of the frame: the cut move's state (its blow, the RE6 chain) dropped,
// unless a riposte already started another move.
// A pair cut before its end: its victim let go (user, 2026-11-10: a prompt's move cut by the RE6 parry, "the enemy targeted by the
// prompt's synchronised attack plays it alone though Chris makes no attack on it"; logged: the Majini played its side 4 s, then lay
// down): the lock dropped, its collision and speed back, its Wait asked (a big one: its own Dmg, a stagger). Killed already: its corpse
// ends the fall (g_pairDying).
// Strike's Re6PairBlowsOff: the pair's lock and throw ended at the blow (collision and speed back), the blow-off asked after.
void EndPairForBlowOff(uintptr_t enemy) {
    // RE6's throw: its victim side's root travel over the next 8 frames, turned by the pair's yaw.
    g_blowDirOk = false;
    float frame = 0.0f, a[3]{}, b[3]{};
    if (g_pairLock.unit == enemy && mem::Read(enemy + 0x398, frame) && PairRootAt(g_pairLock.victimIndex, frame, a) &&
        PairRootAt(g_pairLock.victimIndex, frame + 8.0f, b)) {
        const float c = std::cos(g_pairLock.victimYaw), sn = std::sin(g_pairLock.victimYaw);
        const float lx = b[0] - a[0], lz = b[2] - a[2];
        const float wx = lx * c + lz * sn, wz = -lx * sn + lz * c, len = std::hypot(wx, wz);
        if (len > 1.0f) {
            g_blowDir[0] = wx / len;
            g_blowDir[1] = wz / len;
            g_blowDirOk = true;
        }
    }
    UnmirrorPairVictim();
    g_pairLock.unit = 0;
    g_pairVictim = 0;
    if (enemy == g_thrownUnit.load()) ReleaseThrown(true);
    LOG("wesker moves: %08X out of its pair at the blow, blown off", static_cast<unsigned>(enemy));
}

void ReleaseCutPair() {
    const uintptr_t e = g_pairVictim.load();
    UnmirrorPairVictim();
    g_pairLock.unit = 0;
    if (!e || e == g_pairDying.load()) return;
    bool alive = false;
    for (uintptr_t u : actors::Enemies()) alive = alive || u == e;
    g_pairVictim = 0;
    const PuppetClass* const puppet = e == g_puppet.load() ? g_puppetClass.load() : nullptr;
    if (puppet) EndPuppet(e, "the parry cut its pair");
    if (e == g_thrownUnit.load()) ReleaseThrown(alive);
    if (!alive) return;
    int action = -1;
    if (puppet) action = puppet->dmg[0];
    else if (IsMajiniEnemy(e)) action = PlayerActionNamed(e, "cEm10ActWait");
    const bool asked = action >= 0 && RequestEnemy(e, static_cast<uint32_t>(action));
    LOG("wesker moves: %08X let go, its pair cut by the parry (%s %X %s)", static_cast<unsigned>(e), puppet ? "its Dmg" : "its Wait",
        action, asked ? "asked" : "refused");
}

void TickParryCut() {
    if (!g_parryCutAsked.exchange(false)) return;
    g_re6Chain = {};
    if (!g_active.move || g_active.start != g_parryCutStart.load()) return;
    if (g_active.move->victim >= 0) ReleaseCutPair();
    SetReverseGrip(0);
    SetWeaponJointUnit(0, -1);
    ShowMoveWeapon(actors::InGame() ? actors::Player(0) : 0);
    EndStolenWeapon();
    StopMoveFlames();
    g_active = {};
    g_invulnerable = false;
}

void TickCombat() {
    TickParryCut();
    TickStolenDrop();
    TickRe6Ground();
    TickSlideBlow(actors::InGame() ? actors::Player(0) : 0);
    TickRe6Melee(actors::InGame() ? actors::Player(0) : 0);
    TickRe6Camera(actors::InGame() ? actors::Player(0) : 0);
    TickCaught();
    TickNeckBreak();
    TickJillVictim();
    TickDrivenPuppet();
    TickKneel();
    TickThrown();
    if (const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0) {
        const bool riposte = PlayerInWeskerMove(p1) && GuardRiposteActive(p1);
        if (riposte) GunBackFromKnife(p1);
        // Only the guard is sped up: the counter plays at the character's speed (user, 2026-10-23).
        SpeedGuard(p1, riposte && g_riposteMotion.load() == ((static_cast<uint32_t>(kWeskerBank) << 8) | 0x74));
        if (!GuardRiposteActive(p1)) g_counterTarget = 0;  // not before the counter's action has started
        uint32_t motion = 0;
        float frame = 0;
        const uint32_t counter = CounterMotion(g_counterVariant.load());
        const bool countering = riposte && g_riposteMotion.load() == counter && mem::Read(p1 + 0x384, motion) &&
                                (motion & 0xFFFF) == counter;
        if (countering) mem::Read(p1 + 0x398, frame);
        // After the counter (user, 2026-11-10: "at the end of Chris's counter he slides to his right: keep him in place"): its last pose
        // has the hip off his origin and the game's idle blends the hip back over it - the hip stays held where the counter left it while
        // he stands (action 0) for kCounterSettleMs, his origin moved under it meanwhile.
        constexpr ULONGLONG kCounterSettleMs = 500;
        static ULONGLONG settleUntil = 0;
        static bool wasCountering = false;
        uint32_t action = 0;
        mem::Read(p1 + 0x10E0, action);
        const ULONGLONG now = GetTickCount64();
        if (countering) {
            settleUntil = now + kCounterSettleMs;
        } else if (wasCountering) {
            float hip[3]{}, at[3]{};
            if (ReadHip(p1, hip) && mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), at, sizeof(at)))
                LOG("wesker moves: counter over, its hip %.0f cm off its origin: held there while he stands", std::hypot(hip[0] - at[0], hip[2] - at[2]));
        }
        wasCountering = countering;
        const bool settling = !countering && now < settleUntil && action == 0x00;
        HoldCounterSpot(p1, countering || settling, countering ? frame : 1000.0f, countering ? g_counterTarget.load() : 0);
        if (countering) TickCounterBlow(p1, frame);
        if (!riposte) g_counterPunchDue = false;
    }
    if (g_throwNearest.exchange(false) && static_cast<int32_t>(GetTickCount() - g_throwRiposteTick.load()) < 300)
        LOG("wesker moves: melee swap throw %s", StartCombatMove(kThrowMove) ? "started" : "failed");
    if (const int requested = g_moveRequest.exchange(-1); requested >= 0) {
        const bool fresh = static_cast<int32_t>(GetTickCount() - g_moveRequestTick.load()) < 300;
        const uintptr_t target = g_moveRequestTarget.load();
        // "Utiliser les back-up" (melee_tuning.cpp): its back-up on an enemy that refuses it.
        const std::string backup = fresh ? BackupFor(kCombat[requested].id, target ? target : NearestTarget()) : std::string();
        const bool started = fresh && (backup.empty() ? StartCombatMove(requested, target) : RunBackup(backup));
        LOG("wesker moves: %s requested on %08X %s", kCombat[requested].name, static_cast<unsigned>(target),
            started ? "started" : "failed");
    }
    if (const uintptr_t riposte = g_throwRiposte.exchange(0)) {
        const bool fresh = static_cast<int32_t>(GetTickCount() - g_throwRiposteTick.load()) < 300;
        // "Utiliser les back-up" (melee_tuning.cpp; user, 2026-10-31: the parries' throw ignored it, manual or
        // automatic): the throw's back-up on an enemy that refuses it.
        const std::string backup = fresh ? BackupFor(kCombat[kThrowMove].id, riposte) : std::string();
        const bool thrown = fresh && (backup.empty() ? StartCombatMove(kThrowMove, riposte) : RunBackup(backup));
        LOG("wesker moves: throw parry on %08X %s", static_cast<unsigned>(riposte), thrown ? "started" : "failed");
    }
    if (!g_active.move) return;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    const DWORD now = GetTickCount();
    uint32_t motion = 0;
    float frame = 0, length = 0;
    if (!p || !mem::Read(p + 0x384, motion)) {
        if (g_stolenFrom) {  // no player 1 any more (a load): the weapon left to its Majini, nothing called on it
            SetStolenWeapon(0, 0, 0);
            g_stolenHand = 0;
            g_stolenFrom = 0;
        }
        StopMoveFlames();
        g_active = {};
        g_invulnerable = false;
        return;
    }
    motion &= 0xFFFF;
    if (g_active.move->bank == kRe6MeleeBank && !ReverseGripMove(g_active.move->id) && WeaponJointFor(g_active.move->index)) {
        uintptr_t held = 0;  // its weapon held as RE6's weapon joint holds it (player.cpp)
        mem::Read(p + 0x2634, held);
        SetWeaponJointUnit(held, g_active.move->index);
    }
    if (HidesWeaponMove(g_active.move->id)) HideMoveWeapon(p);
    TickStolenWeapon(p);
    if (g_flipPlayerMirror.load(std::memory_order_relaxed) && motion == (g_active.motion & 0xFFFF)) {
        uint16_t mode = 0;  // his layer 0's mirror bit as the pair's (g_flipPlayerMirror)
        const bool want = g_pairMirror.load(std::memory_order_relaxed);
        if (mem::Read(p + 0x38A, mode) && ((mode & 0x100) != 0) != want)
            mem::Write<uint16_t>(p + 0x38A, static_cast<uint16_t>(want ? (mode | 0x100) : (mode & ~0x100)));
    }
    if (ReverseGripMove(g_active.move->id)) {  // the knife in hand reversed while the move plays (the knife drawn may take a frame)
        uintptr_t held = 0;
        mem::Read(p + 0x2634, held);
        SetReverseGrip(IsKnifeItem(PlayerHeldItem(p)) ? held : 0);
    }
    if (motion == g_active.motion) {
        g_active.seen = true;
    } else if (g_active.seen || now - g_active.start > 1500) {
        SetReverseGrip(0);
        SetWeaponJointUnit(0, -1);
        ShowMoveWeapon(p);
        EndStolenWeapon();
        g_flipPlayerMirror = false;
        LOG("wesker moves: %s over (%d hit)", g_active.move->name, g_active.hits);
        // Its knife put away, the weapon back - only when the move drew it (a swap, the free melee); with the knife already out (the
        // counter / Leg Back replaced, user: "the animation must not draw the knife") it stays.
        if (Re6KnifePair(g_active.move->id) && g_knifeDrawnForMove && g_knifePreviousGrid != 0xFF) {
            const bool back = EquipPlayerGrid(p, g_knifePreviousGrid);
            LOG("wesker moves: the knife put away, the weapon of cell %u in hand again (%s)", g_knifePreviousGrid, back ? "ok" : "failed");
        }
        g_knifeDrawnForMove = false;
        StopMoveFlames();
        g_active = {};
        g_invulnerable = false;
        return;
    }
    if (!g_active.seen || !mem::Read(p + 0x398, frame) || !mem::Read(p + 0x39C, length) || length <= 0) return;
    const float part = frame / length;
    const CombatMove& active = *g_active.move;
    TickMoveFlames(p, active, part);
    // Jill's leap: the catch from 20 % of it on its target within 3 m (brought to him, as the native one brings Chris),
    // else on the first enemy within 1.3 m in front.
    if (&active == &kCombat[kThrow2Move] && part >= 0.20f) {
        float pos[3]{};
        if (mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) {
            auto reach = [&](uintptr_t e, float within, bool front) {
                float ep[3]{};
                int16_t health = 0;
                if (!e || !mem::Read(e + 0x1364, health) || health <= 0 || !mem::SafeRead(reinterpret_cast<void*>(e + 0x30), ep, sizeof(ep)))
                    return false;
                const auto enemies = actors::Enemies();
                if (std::find(enemies.begin(), enemies.end(), e) == enemies.end()) return false;
                const float dx = ep[0] - pos[0], dz = ep[2] - pos[2];
                return dx * dx + dz * dz <= within * within && std::fabs(ep[1] - pos[1]) <= kHeight &&
                       (!front || std::fabs(Wrap(std::atan2(dx, dz) - YawOf(p))) <= 1.2f);
            };
            uintptr_t caught = reach(g_leapTarget, 300.0f, false) ? g_leapTarget : 0;
            if (!caught)
                for (uintptr_t e : actors::Enemies())
                    if (reach(e, 130.0f, true)) {
                        caught = e;
                        break;
                    }
            if (caught) {
                g_leapTarget = 0;
                LOG("wesker moves: Jill's leap on %08X at frame %.0f: her catch %s", static_cast<unsigned>(caught), frame,
                    StartCombatMove(kThrow2Catch, caught, true) ? "started" : "failed");
                return;
            }
        }
    }

    // A dive with the aim held: under the aim from the moment its weapon is back in front.
    if (active.bank == kRe6EvadeBank && active.index <= 3 && !g_active.cut && frame >= kRe6Layered[active.index].aimFrame && (HeldMaskOf(p) & 0xC00)) {
        g_active.cut = true;
        LOG("re6 evade: %s goes on under the aim at frame %.0f: %s", active.name, frame, StartRe6Layered(active.index, frame, true) ? "ok" : "refused");
        return;
    }
    if (part >= active.end && !g_active.cut) {
        g_active.cut = true;
        if (Re6EvadeEnded(p, active)) return;
        LOG("wesker moves: %s cut at frame %.0f of %.0f, idle %s", active.name, frame, length,
            RequestPlayer(p, 0x00) ? "asked" : "failed");
    }
    // A blow heard without its damage (user, 2026-11-11: the first armed-zombie counter, "a blow's sound at the elbow"; its damage would kill
    // before he takes the weapon): its native impact on the pair's victim, once.
    if (!g_active.cue && g_active.only && frame >= SoundOnlyBlowFrame(active.id)) {
        g_active.cue = true;
        PlayMoveImpact(active, g_active.only);
    }
    const int window = part >= active.from && part <= active.to ? 1 : part >= active.from2 && part <= active.to2 ? 2 : 0;
    if (!window) return;
    if (window != g_active.window) {  // a new impact: every enemy may be hit again
        g_active.window = window;
        g_active.hits = 0;
        // Jill's throws: silent at the ground's impact (user, 2026-10-31: "no sound needed at the impact on the ground"). Chris's RE6 Leg
        // Back: no grunt at its kick, only at its elbow (user, 2026-11-10).
        const bool silent = window == 1 && std::strcmp(active.id, "melee.free_re6_chris_leg_back") == 0;
        if (active.damage > 0 && active.reaction != Reaction::Thrown && !silent) PlayMoveGrunt(p);
        // A pair's blows sound at their window even when the first one killed its victim (the elbow after a lethal kick). Not the RE6
        // throws (user, 2026-11-10: "remove the blow's sound on Leon's, Ada's, Helena's Head Flash, all those that are throws"): the
        // Head Flashes and Jake's Ground.
        const bool throwing = std::strstr(active.id, "free_re6_") != nullptr &&
                              (std::strstr(active.id, "head_flash") != nullptr || std::strstr(active.id, "jake_ground") != nullptr);
        // The knife pairs: the knife's own hit on the victim (PlayKnifeHit) instead of a blow's (user, 2026-11-10: "it played a blow's
        // sound instead of a knife slash").
        // The machete disarm too, its Majini's own knife or sickle through it (user, 2026-11-11: "a knife's sound when I run the Majini
        // through with its knife in the counters against the knives, not the axe's: the one we validated").
        if ((Re6KnifePair(active.id) || std::strcmp(active.id, "melee.free_re6_common_machete_counter") == 0) && g_active.only) {
            PlayKnifeHit(g_active.only, 120.0f);
        } else if (active.victim >= 0 && active.damage > 0 && active.reaction != Reaction::Thrown && g_active.only && !throwing) {
            PlayMoveImpact(active, g_active.only);
        }
    }
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return;
    const float facing = g_active.yaw + (g_active.move->behind ? 3.14159265f : 0.0f);
    const LimbPoints limbs = ReadLimbs(p);
    for (uintptr_t e : actors::Enemies()) {
        bool done = false;
        for (int k = 0; k < g_active.hits; ++k) done = done || g_active.hit[k] == e;
        float ep[3]{};
        int16_t health = 0;
        if (done || g_active.hits >= 16 || (g_active.only && e != g_active.only) || !mem::Read(e + 0x1364, health) ||
            health <= 0 ||
            !mem::SafeRead(reinterpret_cast<void*>(e + 0x30), ep, sizeof(ep)))
            continue;
        const float dx = ep[0] - pos[0], dz = ep[2] - pos[2];
        if (!g_active.only) {
            if (dx * dx + dz * dz > 600.0f * 600.0f) continue;  // far: none of its boxes looked at
            bool zone = false;
            const char* limb = LimbContact(limbs, e, zone);
            if (zone) {
                if (dx * dx + dz * dz > kReach * kReach || std::fabs(ep[1] - pos[1]) > kHeight) continue;
                if (std::fabs(Wrap(std::atan2(dx, dz) - facing)) > kArc) continue;
                LOG("wesker moves: %s on %08X (no hurt shape: the zone)", g_active.move->name, static_cast<unsigned>(e));
            } else if (!limb) {
                continue;
            } else {
                LOG("wesker moves: %s, %s on %08X at %.0f%%", g_active.move->name, limb, static_cast<unsigned>(e), part * 100.0f);
            }
        }
        g_active.hit[g_active.hits++] = e;
        // RE6's knife moves wear the knife in hand once, at their first blow that lands (user, 2026-11-10).
        if (!g_active.worn) {
            const int which = std::strcmp(active.id, "melee.free_re6_common_knife_counter") == 0 ? 0
                              : std::strcmp(active.id, "melee.free_re6_common_throat_cut") == 0  ? 1
                              : std::strcmp(active.id, "melee.free_re6_ground_attack_knife") == 0 ? 2
                                                                                                   : -1;
            if (which >= 0 && IsKnifeItem(PlayerHeldItem(p))) {
                g_active.worn = true;
                KnifeWearRe6Move(p == actors::Player(1) ? 1 : 0, which);
            }
        }
        if (g_active.hits == 1 && g_active.move->reaction != Reaction::Thrown && !(g_active.move->victim >= 0 && g_active.only)) {
            // once a blow (a pair's at its window); the knife Ground: the knife's hit (user, 2026-11-10: "the blow's sound you play
            // instead of a knife's")
            if (std::strcmp(active.id, "melee.free_re6_ground_attack_knife") == 0) PlayKnifeHit(e, 40.0f);
            else PlayMoveImpact(*g_active.move, e);
        }
        Strike(p, e, *g_active.move);
    }
    // The RE6 melee's crate blows break the crates any limb touches (or the weapon in the hands: the fists).
    if (&active == &kCombat[kRe6CrateHigh] || &active == &kCombat[kRe6CrateLow]) SlideBreakCrates(p, limbs, pos, 0xFF);
}

// Free player 1 (not dying, held or hurt: actions under 0x80).
// The free melee table's reactions (melee_tuning.cpp): blown off as by Chris's punch, the recoil of an Arm Front (its
// head flash prompt), a flinch without prompt; the other enemies their own hit reactions, as Strike gives them.
// `killHere`: a Majini the trainer's own strike left at 0 health dies at once (the game's hit has its own death).
bool ReactInPlace(uintptr_t p, uintptr_t enemy, int reaction, bool killHere) {
    int16_t health = 0;
    if (!p || !enemy || !mem::Read(enemy + 0x1364, health)) return false;
    if (IsMajiniEnemy(enemy)) {
        if (reaction == kReactionBlowOff) {  // at 0 health it dies when it lands; the big Majini too (the table's choice)
            AllowBigBlowOff(enemy);
            return MajiniBlowOff(enemy, p);
        }
        if (health <= 0) return killHere && MajiniDieNow(enemy);
        return reaction == kReactionRecoil ? ForceMajiniState(enemy, 1) : MajiniFlinch(enemy);
    }
    if (health <= 0) return false;
    bool reacted = false;
    if (IsClass(enemy, kJillVtable)) {
        reacted = RequestEnemy(enemy, 0x19);
    } else if (IsJillUnit(enemy)) {
        const int dmg = JillAction(enemy, "DmgBlowL");
        reacted = dmg >= 0 && RequestEnemy(enemy, static_cast<uint32_t>(dmg));
    } else if (IsClass(enemy, kWeskerVtable) || IsClass(enemy, kFinalWeskerVtable) || IsClass(enemy, kLinWeskerVtable)) {
        reacted = RequestEnemy(enemy, 0x58);
    } else if (IsClass(enemy, kReaperVtable)) {
        reacted = RequestEnemy(enemy, reaction == kReactionBlowOff ? 0x21 : 0x18);
    } else if (IsClass(enemy, kExecutionerClass) || IsClass(enemy, kNightmareExecutionerClass)) {
        reacted = RequestEnemy(enemy, 0x1D);
    } else if (IsClass(enemy, kChainsawClass)) {
        reacted = RequestEnemy(enemy, 0x17);
    }
    if (reacted && IsJillUnit(enemy)) {  // her getup after it (JillGetupRequest)
        const DWORD tick = GetTickCount();
        g_jillKnockedTick = tick ? tick : 1;
        g_jillKnocked = enemy;
    }
    return reacted;
}

uintptr_t FreePlayer1(bool carrierOk = false) {  // carrierOk: a parry just cut his move (its carrier action still on)
    if (g_re6Ground) return 0;  // on the ground (RE6): none of the trainer's moves until he is up (user, 2026-11-08)
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    int16_t health = 0;
    uint32_t action = 0;
    // On foot only (user, 2026-10-28: Jill's slide in the boat took Chris off it and locked him).
    if (!p || !actors::OnFoot(p) || !mem::Read(p + 0x1364, health) || health < 1 || (health == 1 && !DyingActionsAllowed()) ||
        !mem::Read(p + 0x10E0, action) ||
        (action >= 0x80 && !(carrierOk && action == kCarrier)))
        return 0;
    return p;
}
}  // namespace

void ForceNextEnemyMotion(uintptr_t enemy, uint32_t motion);  // below

// swaps.cpp's SetMotion hook: Jill in Chris's side of the throw: another layer 0 motion her action asks (her knock down's
// next ones: "nothing happens on Jill", user) becomes it again from where it is, until it ends.
// The actor SetMotion hook (swaps.cpp): the forward roll asked face down starts at its frame 20.
void GroundRollStart(uintptr_t unit, uint32_t layer, uint32_t motion, uint32_t& blend, uint32_t& start) {
    if (layer != 0 || !unit || unit != actors::Player(0) || (motion & 0xFFFF) != kForwardRollMotion ||
        static_cast<int32_t>(GetTickCount() - g_bellyRollAsked.load()) > 500)
        return;
    g_bellyRollAsked = 0;
    const float from = kForwardRollStart, over = 6.0f;
    std::memcpy(&start, &from, sizeof(start));
    std::memcpy(&blend, &over, sizeof(blend));
    LOG("wesker moves: player 1's forward roll from frame %.0f (face down)", from);
}

// The actor SetMotion hook (swaps.cpp), after WeskerMoveMotion: the move's motion just put starts at its frame (the
// blend kept).
void MoveBeginStart(uintptr_t unit, uint32_t layer, uint32_t& start) {
    const float begin = g_beginNow;
    g_beginNow = -1.0f;
    if (layer != 0 || !unit || unit != actors::Player(0) || begin <= 0.0f) return;
    std::memcpy(&start, &begin, sizeof(start));
    LOG("wesker moves: player 1's move from frame %.0f", begin);
}

void JillVictimMotion(uintptr_t unit, uint32_t layer, uint32_t& motion, uint32_t& blend, uint32_t& start) {
    if (layer != 0 || !unit || unit != g_jillVictim.load(std::memory_order_relaxed) || (motion & 0xFFFF) == kJill2VictimMotion)
        return;
    uint32_t current = 0;
    float frame = 0.0f, length = 0.0f;
    if (!mem::Read(unit + 0x384, current) || (current & 0xFFFF) != kJill2VictimMotion || !mem::Read(unit + 0x398, frame) ||
        !mem::Read(unit + 0x39C, length) || frame >= length - 2.0f)
        return;
    LOG("wesker moves: Jill %08X motion %X kept on Chris's side %X (frame %.0f of %.0f)", static_cast<unsigned>(unit), motion,
        kJill2VictimMotion, frame, length);
    motion = kJill2VictimMotion;
    const float zero = 0.0f;
    std::memcpy(&blend, &zero, sizeof(blend));
    std::memcpy(&start, &frame, sizeof(start));
}

// swaps.cpp's SetMotion hook: the death of an RE4 pair's side killed at the pair's blow (KillPairVictim) plays the
// pair's side on from where it was, without a blend.
void PairCorpseMotion(uintptr_t unit, uint32_t layer, uint32_t& motion, uint32_t& blend, uint32_t& start) {
    if (layer == 0 && unit && unit == g_corpseNative.load(std::memory_order_relaxed)) {  // a native death from part-way (TickThrown)
        g_corpseNative = 0;
        if (const uint32_t native = g_corpseNativeMotion.exchange(0)) motion = native;
        const float from = g_corpseNativeStart.load(), frames = 8.0f;
        std::memcpy(&start, &from, sizeof(start));
        std::memcpy(&blend, &frames, sizeof(blend));
        LOG("wesker moves: %08X death motion %X from its frame %.0f", static_cast<unsigned>(unit), motion, from);
        return;
    }
    if (layer != 0 || !unit || unit != g_pairCorpse.load(std::memory_order_relaxed)) return;
    g_pairCorpse = 0;
    if (static_cast<int32_t>(GetTickCount() - g_pairCorpseTick.load()) > 500) return;
    const float zero = 0.0f, from = g_pairCorpseFrame.load();
    LOG("wesker moves: %08X death motion %X -> %X from frame %.0f (the pair's fall)", static_cast<unsigned>(unit), motion,
        g_pairMotion.load(), from);
    motion = g_pairMotion.load();
    std::memcpy(&blend, &zero, sizeof(blend));
    std::memcpy(&start, &from, sizeof(start));
}

// swaps.cpp's SetMotion hook, after the others: an RE4 pair's side (alive) going from the pair to one of the game's
// lying motions (em10_g05 0x52-0x57, 0x3C / 0x3D: the Majini's bank 5, a puppet's bank D) is turned and moved first,
// so that motion starts where the body lies: its origin under the hips, its facing from the head (opposite on the
// back, 6 degrees off along it on the belly), without a blend (scratchpad lying_poses.py, 2026-10-31).
void PairLyingRealign(uintptr_t unit, uint32_t layer, uint32_t motion, uint32_t& blend) {
    if (layer != 0 || !unit) return;
    const bool corpse = unit == g_corpseRealign.load(std::memory_order_relaxed);  // its own death after the pair's fall (any motion)
    if (!corpse && unit != g_pairVictim.load(std::memory_order_relaxed)) return;
    if (!corpse && static_cast<int32_t>(GetTickCount() - g_pairVictimTick.load()) > 20000) {
        g_pairVictim = 0;
        return;
    }
    uint32_t current = 0;
    const uint32_t bank = (motion >> 8) & 0xFF, slot = motion & 0xFF;
    const bool lyingBank = bank == (IsMajiniEnemy(unit) ? 5u : static_cast<uint32_t>(kMajiniRiseBank));
    bool faceUp = slot == 0x52 || slot == 0x54 || slot == 0x56 || slot == 0x3C;
    bool faceDown = slot == 0x53 || slot == 0x55 || slot == 0x57 || slot == 0x3D;
    if (corpse) {
        g_corpseRealign = 0;
        faceDown = g_corpseFaceDown.load();
        faceUp = !faceDown;
    }
    if (!mem::Read(unit + 0x384, current) || (current & 0xFFFF) != (g_pairMotion.load() & 0xFFFF) ||
        (!corpse && (!lyingBank || (!faceUp && !faceDown))))
        return;
    if (!corpse) g_pairVictim = 0;
    uintptr_t joints = 0;
    uint32_t count = 0;
    float hips[3]{}, head[3]{}, pos[3]{};
    uint8_t second = 0xFF;  // the Executioner's root on the floor first, its hips next (LyingOnBack)
    if (!mem::Read(unit + 0x318, joints) || !joints || !mem::Read(unit + 0x314, count) ||
        !mem::SafeRead(reinterpret_cast<void*>(joints + (mem::Read(joints + 0x90 + 7, second) && second == 0 ? 0x90 : 0) + 0x80), hips,
                       sizeof(hips)) ||
        !mem::SafeRead(reinterpret_cast<void*>(unit + 0x30), pos, sizeof(pos)))
        return;
    bool found = false;
    for (uint32_t j = 0; j < std::min<uint32_t>(count, 0x100) && !found; ++j) {
        uint8_t id = 0;
        found = mem::Read(joints + j * 0x90 + 7, id) && id == 4 &&
                mem::SafeRead(reinterpret_cast<void*>(joints + j * 0x90 + 0x80), head, sizeof(head));
    }
    if (!found) return;
    const float headYaw = std::atan2(head[0] - hips[0], head[2] - hips[2]);
    const float yaw = headYaw - (faceUp ? 3.14159265f : -0.105f);
    const float place[3] = { hips[0], pos[1], hips[2] };
    const float quat[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
    mem::SafeWrite(reinterpret_cast<void*>(unit + 0x30), place, sizeof(place));
    mem::SafeWrite(reinterpret_cast<void*>(unit + 0x40), quat, sizeof(quat));
    const float zero = 0.0f;
    std::memcpy(&blend, &zero, sizeof(blend));
    LOG("wesker moves: %08X lies after the pair: motion %X on its %s, moved %.0f cm under its hips, facing %.2f",
        static_cast<unsigned>(unit), motion, faceUp ? "back" : "belly",
        std::hypot(place[0] - pos[0], place[2] - pos[2]), yaw);
}

// Jill as Chris's side of her second throw (g_jillVictim): at his origin turned to him, her knock down playing em84evt
// 0x00 from her bank C.
void StartJillVictim(uintptr_t jill, const float* pos, float yaw) {
    if (!g_jill2EventList) g_jill2EventList = LoadList("uEm84", "pawn\\em\\em84\\motion\\em84evt");
    const int knock = IsClass(jill, kJillVtable) ? 0x1B : JillAction(jill, "DmgKnockDown");
    if (!g_jill2EventList || knock < 0 || !SetBank(jill, g_jill2EventList, kJill2VictimBank)) {
        LOG("wesker moves: Jill %08X not put in Chris's side of the throw (list %p, knock down %X)", static_cast<unsigned>(jill),
            g_jill2EventList, knock);
        return;
    }
    const float back = yaw + 3.14159265f;
    const float quat[4] = { 0.0f, std::sin(back * 0.5f), 0.0f, std::cos(back * 0.5f) };
    std::memcpy(g_jillVictimAt, pos, sizeof(g_jillVictimAt));
    std::memcpy(g_jillVictimQuat, quat, sizeof(g_jillVictimQuat));
    CollisionOff(jill);
    EnsurePlayerContactHook();
    mem::SafeWrite(reinterpret_cast<void*>(jill + 0x30), g_jillVictimAt, sizeof(g_jillVictimAt));
    mem::SafeWrite(reinterpret_cast<void*>(jill + 0x40), g_jillVictimQuat, sizeof(g_jillVictimQuat));
    g_jillVictimSince = GetTickCount();
    g_jillVictimSeen = false;
    g_jillVictim = jill;
    ForceNextEnemyMotion(jill, kJill2VictimMotion);
    const bool asked = RequestEnemy(jill, static_cast<uint32_t>(knock));
    LOG("wesker moves: Jill %08X in Chris's side of her throw (knock down %X %s, motion %X)", static_cast<unsigned>(jill), knock,
        asked ? "asked" : "refused", kJill2VictimMotion);
}

// Every frame (TickCombat): held at the origin while Chris plays the throw, then her collision back.
// Jill at 0 health still fighting (left so by the moves before the rule above): 1 health back, once per unit, so that
// the game's damage takes her on.
void RepairJillHealth() {
    static uintptr_t repaired = 0;
    for (uintptr_t e : actors::Enemies()) {
        int16_t health = 1;
        uint32_t action = 0;
        if (e == repaired || !IsJillUnit(e) || !mem::Read(e + 0x1364, health) || health > 0 || !mem::Read(e + 0x10E0, action))
            continue;
        const char* tail = ActionTail(e, action);
        bool down = action == 0x80000000;
        for (const char* state : { "Die", "Sleep", "Strip", "Pinion", "Core", "Caught", "Down", "Getup", "Appear", "Dmg" })
            down = down || std::strstr(tail, state);
        if (down) continue;
        repaired = e;
        mem::Write<int16_t>(e + 0x1364, 1);
        LOG("wesker moves: Jill %08X at 0 health in %s: 1 health back", static_cast<unsigned>(e), tail);
    }
}

void TickJillVictim() {
    RepairJillHealth();
    const uintptr_t jill = g_jillVictim.load(std::memory_order_relaxed);
    if (!jill) return;
    // Hers until Chris's side ends (it is longer than his catch): collision off, its motion kept (JillVictimMotion).
    uint32_t motion = 0;
    float frame = 0.0f, length = 0.0f;
    const bool playing = mem::Read(jill + 0x384, motion) && (motion & 0xFFFF) == kJill2VictimMotion &&
                         mem::Read(jill + 0x398, frame) && mem::Read(jill + 0x39C, length) && frame < length - 1.0f;
    if (playing) g_jillVictimSeen = true;
    const int32_t since = static_cast<int32_t>(GetTickCount() - g_jillVictimSince);
    if ((!playing && (g_jillVictimSeen || since > 1000)) || since > 8000) {
        CollisionBack(jill);
        g_jillVictim = 0;
        const DWORD tick = GetTickCount();  // her getup or Sleep after it (JillGetupRequest)
        g_jillKnockedTick = tick ? tick : 1;
        g_jillKnocked = jill;
        LOG("wesker moves: Jill %08X out of Chris's side of her throw", static_cast<unsigned>(jill));
    }
}

// Jill's throw on an enemy it has no throw for (forced_death.cpp then neither holds nor kills it).
std::atomic<uintptr_t> g_throwOther{ 0 };
std::atomic<DWORD> g_throwOtherTick{ 0 };

// A dodge of the Dodge sub-tab (RegisterWeskerMoves: free while a move plays).
bool IsDodgeMove(int k) {
    const char* id = kCombat[k].id;
    return std::strncmp(id, "melee.free_leon_dodge", 21) == 0 ||
           (std::strncmp(id, "melee.free_leon", 15) != 0 && std::strncmp(id, "melee.free_hunk", 15) != 0 && kCombat[k].damage == 0 &&
            !ThrowMove(k) && k != kThrow2Move);
}

// The dodges' stamina (user, 2026-11-09: "the dodges, the slide and the ground moves in the stamina system: with Endurance the refill held
// during their animation, with strict stamina their cost; a slider on each dodge's option, at its lowest below 0 the refill held only"):
// the dodges with a cost slider of their own (not the getups, the duck's pieces nor the slide, whose slider is its option's).
bool CostedDodge(int k) {
    const char* id = kCombat[k].id;
    return IsDodgeMove(k) && !std::strstr(id, "getup") && !std::strstr(id, "_rise") && !std::strstr(id, "crouch") &&
           std::strcmp(id, "melee.free_re6_slide") != 0;
}

float DodgeCostOf(const char* id) {
    if (std::strcmp(id, "melee.free_re6_slide") == 0) id = "player.re6_slide";
    const Feature* f = Find(id);
    return f && f->valueMax > f->valueMin ? f->value : -1.0f;
}

void SpendForMove(int k) {
    if (IsDodgeMove(k)) StaminaSpendDodge(DodgeCostOf(kCombat[k].id));
    else StaminaSpend();
}

// Starts an attack: facing the nearest enemy within 3 m, its motion, its hits and player 1's invulnerability.
// A joint's world place by its id (joints [unit +0x318], 0x90 each: +7 its id, +0x80 its world position).
bool JointWorldById(uintptr_t unit, uint8_t id, float (&out)[3]) {
    uintptr_t joints = 0;
    uint32_t count = 0;
    if (!mem::Read(unit + 0x318, joints) || !joints || !mem::Read(unit + 0x314, count) || count > 255) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t jid = 0xFF;
        if (mem::Read(joints + i * 0x90 + 7, jid) && jid == id)
            return mem::SafeRead(reinterpret_cast<void*>(joints + i * 0x90 + 0x80), out, sizeof(out));
    }
    return false;
}

// Jake's Grounds on a lying Majini (user, 2026-11-10: "the enemy moves into place instead of Chris: Chris must place himself facing its
// legs"): it stays where it lies and player 1 is placed and turned for the pair. Its body's way (hips to head, joints 0 / 4) gives the
// pair's yaw: at the victim side's frame 0 that way is -175 degrees in its root's frame face up (re6_melee 88), -11 face down (89) - its
// feet towards player 1 both times (the face-up victim faces him, the face-down one has its back to him). Then the pair's own placement
// (kRe6PairPlaces, its frame-0 root) run backwards from where it lies gives player 1's place.
bool AlignPlayerToLying(uintptr_t p, uintptr_t e, const CombatMove& move) {
    const bool belly = std::strcmp(move.id, "melee.free_re6_jake_ground_belly") == 0;
    if (!belly && std::strcmp(move.id, "melee.free_re6_jake_ground") != 0) return false;
    float hips[3]{}, head[3]{}, at[3]{}, me[3]{}, root0[3]{};
    if (!JointWorldById(e, 0, hips) || !JointWorldById(e, 4, head) || !mem::SafeRead(reinterpret_cast<void*>(e + 0x30), at, sizeof(at)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), me, sizeof(me)) || std::hypot(head[0] - hips[0], head[2] - hips[2]) < 10.0f)
        return false;
    constexpr float kPi = 3.14159265f;
    const float body = std::atan2(head[0] - hips[0], head[2] - hips[2]);
    const float mirror = g_pairMirror.load(std::memory_order_relaxed) ? -1.0f : 1.0f;
    const float victimYaw = body - mirror * (belly ? -11.0f : -175.0f) * kPi / 180.0f;
    const float yaw = belly ? victimYaw : victimYaw - kPi;  // the face-up victim faces him, the face-down one turns its back to him
    float distance = kRe6PairDistance, left = 0.0f;
    for (const Re6PairPlace& place : kRe6PairPlaces)
        if (std::strcmp(move.id, place.id) == 0) distance = place.distance, left = place.left;
    left *= mirror;
    PairRootAt(move.victim, 0.0f, root0);
    const float vc = std::cos(victimYaw), vs = std::sin(victimYaw), c = std::cos(yaw), s = std::sin(yaw);
    const float origin[2] = { at[0] - (root0[0] * vc + root0[2] * vs), at[2] - (-root0[0] * vs + root0[2] * vc) };
    const float place[3] = { origin[0] - distance * s - left * c, me[1], origin[1] - distance * c + left * s };
    const float quat[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x30), place, sizeof(place));
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x40), quat, sizeof(quat));
    mem::Write(p + 0x2A10, yaw);
    mem::Write(p + 0x1024, yaw);
    LOG("wesker moves: %s: player 1 placed at its %s legs (%.0f cm away, yaw %.0f deg), the Majini left where it lies", move.name,
        belly ? "face-down" : "face-up", std::hypot(place[0] - me[0], place[2] - me[2]),
        yaw * 180.0f / kPi);
    return true;
}

// RE6's environmental executions (user, 2026-11-11: "replace the attacks on a staggered enemy by the executions against a wall, a table or
// a barrier", "more generous than RE6, where you must be stuck to the wall", "a tolerance slider"). Each pair is made with its obstacle
// straight ahead of player 1's place (research/re6_live/env_wall.py, the J'avo's head in his frame): the wall 186 cm ahead (its head
// pushed 120 cm on to it), the barrier from the side 127 cm (its back over it), the table from the front 144 cm (face down on it).
// Round the enemy the game's ledge probe (env_exec.cpp) finds the obstacles (16 ways, low sweeps at 70 and 95 cm, a high one at 150 cm:
// a wall meets both, a table or a barrier the low ones only); for each one and each pair of its kind the pair's place is the one whose
// obstacle is that one, along it where both units move least; the pair is played if neither moves more than the tolerance.
// From a height (user, 2026-11-11: "from above and from below, no stagger: close enough to the edge, the enemy and the player"): the
// ledge's face ahead of player 1's place (research/re6_live/env_ledge.py): from below 88 cm (the J'avo facing him: his body under the
// top up to 82, its toes at ~90) or 84 (its back: its heels), from above 40 (his toes on the top at ~32, the J'avo below from 95).
enum EnvKind { kEnvWall, kEnvLow, kEnvBelow, kEnvAbove };  // below: player 1 below the J'avo (it is on the ledge)
struct EnvPairGeometry {
    const char* id;
    float obstacle;  // its obstacle's distance ahead of player 1's place (cm): a face, or a ledge's edge
    EnvKind kind;
    bool behind;     // the J'avo's back to him
    float decayFrom = 0.0f, decayTo = 0.0f;  // from below: the J'avo's frames coming down from the top
};
constexpr EnvPairGeometry kEnvPairs[] = {
    { "melee.free_re6_common_env_wall", 186.0f, kEnvWall, false },
    { "melee.free_re6_common_env_table_side", 127.0f, kEnvLow, false },
    { "melee.free_re6_common_env_table_front", 144.0f, kEnvLow, true },
    { "melee.free_re6_common_env_below_facing", 88.0f, kEnvBelow, false, 60.0f, 100.0f },
    { "melee.free_re6_common_env_below_behind", 84.0f, kEnvBelow, true, 98.0f, 134.0f },
    { "melee.free_re6_common_env_above_facing", 40.0f, kEnvAbove, false },
    { "melee.free_re6_common_env_above_behind", 40.0f, kEnvAbove, true },
};
const EnvPairGeometry* EnvGeometryOf(int k) {
    for (const EnvPairGeometry& g : kEnvPairs)
        if (CombatIndex(g.id) == k) return &g;
    return nullptr;
}
bool EnvHeightTolerance(float& tolerance);  // swaps.cpp: "Exécutions d'une hauteur (RE6)" on, its tolerance (cm)
bool EnvWallTolerance(float& tolerance);  // swaps.cpp: "Exécutions contre le décor (RE6)" on, its tolerance (cm)

struct EnvObstacle {
    float n[2];   // its horizontal normal (towards the enemy)
    float dist;   // the enemy's distance to it
    float d;      // its plane: n . (x, z) + d = 0
    bool wall;
    float top;    // the top found just past its face, from the enemy's floor (-1: none)
};

// The obstacle's span in front of `from` (user, 2026-11-11: "the animation finds the wall but often misses it one way or the other, above
// all with thin walls like columns"): sweeps straight at it at `height` from points along it each side (every 10 cm up to 60), those that
// meet it at about its distance; false when the one straight ahead misses. `lo` / `hi`: the span along his left axis (cm from `from`).
// Then (user, 2026-11-11: "give the wall executions priority over the weapon steal: I place an enemy near a wall and get the steal" - the
// straight sweep at 130 cm missed a wall the low sweeps had met): the heights tried in turn, every 20 cm up to a metre each side, the run of
// sweeps meeting it nearest the enemy (the enemy slid along to it), 40 cm off its distance at most.
bool ObstacleSpan(const float* from, const float (&n)[2], float dist, const float* heights, int count, float& lo, float& hi) {
    const float yaw = std::atan2(-n[0], -n[1]);
    const float l[2] = { std::cos(yaw), -std::sin(yaw) };
    for (int h = 0; h < count; ++h) {
        auto meets = [&](float side) {
            const float at[3] = { from[0] + l[0] * side, from[1], from[2] + l[1] * side };
            EnvHit hit;
            if (!EnvProbe(at, yaw, heights[h], dist + 50.0f, 6.0f, hit)) return false;
            const float met = (at[0] - hit.point[0]) * n[0] + (at[2] - hit.point[2]) * n[1];  // how far along the normal
            return std::fabs(met - dist) < 40.0f;
        };
        bool met[11]{};
        bool any = false;
        for (int k = 0; k < 11; ++k) any = (met[k] = meets(-100.0f + 20.0f * static_cast<float>(k))) || any;
        if (!any) continue;
        int best = -1;
        for (int step = 0; step <= 5 && best < 0; ++step) {
            if (met[5 + step]) best = 5 + step;
            else if (met[5 - step]) best = 5 - step;
        }
        int a = best, b = best;
        while (a > 0 && met[a - 1]) --a;
        while (b < 10 && met[b + 1]) ++b;
        lo = -100.0f + 20.0f * static_cast<float>(a);
        hi = -100.0f + 20.0f * static_cast<float>(b);
        return true;
    }
    return false;
}

// `avoid`: player 1's feet (a sweep meeting him is no obstacle). A wall or a low obstacle: the game's own sweep (40 cm up, radius 35, as
// its crossings' probe; user, 2026-11-11: "the barrier where I am is taken for a wall: mind the windows, the barriers and other low things"
// - RE5's low barriers carry invisible walls up high, met by the 150 cm sweep; a sweep down finds nothing, the probe meets no floor) gives
// flags 0x18 and a top (+0x70) on a low obstacle, 4 on a wall (research logs 2026-11-11: a 66 cm top read so).
int FindEnvObstacles(const float* at, const float* avoid, float reach, EnvObstacle* out, int max) {
    constexpr float kPi = 3.14159265f;
    int count = 0;
    for (int i = 0; i < 16 && count < max; ++i) {
        const float yaw = static_cast<float>(i) * kPi / 8.0f;
        const float way[2] = { std::sin(yaw), std::cos(yaw) };
        // Its plane met by a sweep at `height`, facing the sweep: the enemy's horizontal distance to it.
        auto plane = [&](const EnvHit& hit, float& dist, float (&n)[2], float& d) {
            if (std::fabs(hit.normal[1]) > 0.35f || std::hypot(hit.point[0] - avoid[0], hit.point[2] - avoid[2]) < 45.0f) return false;
            const float len = std::hypot(hit.normal[0], hit.normal[2]);
            n[0] = hit.normal[0] / len;
            n[1] = hit.normal[2] / len;
            if (-(n[0] * way[0] + n[1] * way[1]) < 0.6f) return false;  // not across this way
            d = (hit.d + hit.normal[1] * hit.point[1]) / len;
            dist = n[0] * at[0] + n[1] * at[2] + d;
            return dist > -5.0f && dist < reach + 20.0f;
        };
        auto sweep = [&](float height, float& dist, float (&n)[2], float& d) {
            EnvHit hit;
            return EnvProbe(at, yaw, height, reach, 10.0f, hit) && plane(hit, dist, n, d);
        };
        float low = 0.0f, high = 0.0f, n[2]{}, nh[2]{}, d = 0.0f, dh = 0.0f;
        float low2 = 0.0f, n2[2]{}, d2 = 0.0f;
        bool hasLow = sweep(70.0f, low, n, d);
        if (sweep(95.0f, low2, n2, d2) && (!hasLow || low2 < low)) {
            hasLow = true;
            low = low2, n[0] = n2[0], n[1] = n2[1], d = d2;
        }
        // The game's sweep, from 60 cm back (its sphere clear of the obstacle at the start).
        const float back[3] = { at[0] - way[0] * 60.0f, at[1], at[2] - way[1] * 60.0f };
        EnvHit game;
        float gameDist = 0.0f, gn[2]{}, gd = 0.0f;
        const bool gameHit = EnvProbe(back, yaw, 40.0f, reach + 60.0f, 35.0f, game) && plane(game, gameDist, gn, gd);
        if (!hasLow && gameHit) {
            hasLow = true;
            low = gameDist, n[0] = gn[0], n[1] = gn[1], d = gd;
        }
        if (!hasLow) continue;
        const float gameTop = gameHit && (game.flags & 0x18) && std::fabs(gameDist - low) < 30.0f ? game.top[1] - at[1] : -1.0f;
        const bool lowTop = gameTop >= 50.0f && gameTop <= 140.0f;
        const bool hasHigh = sweep(150.0f, high, nh, dh);
        const bool wall = !lowTop && hasHigh && std::fabs(high - low) < 25.0f;
        if (!lowTop && hasHigh && !wall && high < low + 40.0f) continue;  // something else above it
        bool same = false;
        for (int j = 0; j < count && !same; ++j)
            same = out[j].n[0] * n[0] + out[j].n[1] * n[1] > 0.95f && std::fabs(out[j].dist - low) < 15.0f && out[j].wall == wall;
        if (same) continue;
        out[count++] = { { n[0], n[1] }, low, d, wall, lowTop ? gameTop : (gameHit ? -2.0f - static_cast<float>(game.flags) : -1.0f) };
    }
    return count;
}

int EnvExecutionFor(uintptr_t p, uintptr_t enemy, int only, float tolerance, bool quiet) {
    if (tolerance < 0.0f && !EnvWallTolerance(tolerance)) return -1;
    if (!p || !enemy || !IsMajiniEnemy(enemy)) return -1;
    float at[3]{}, me[3]{}, quat[4]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), at, sizeof(at)) || !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), me, sizeof(me)) ||
        !mem::SafeRead(reinterpret_cast<void*>(enemy + 0x40), quat, sizeof(quat)) || std::fabs(at[1] - me[1]) > 40.0f)
        return -1;
    constexpr float kPi = 3.14159265f;
    const float enemyYaw = 2.0f * std::atan2(quat[1], quat[3]);
    EnvObstacle found[16];
    const int count = FindEnvObstacles(at, me, 150.0f + tolerance, found, 16);
    int best = -1;
    float bestCost = 1e9f;
    EnvPlan plan;
    for (int i = 0; i < count; ++i) {
        const EnvObstacle& o = found[i];
        const float yaw = std::atan2(-o.n[0], -o.n[1]);  // towards it
        const float l[2] = { std::cos(yaw), -std::sin(yaw) };  // his left
        const float distP = o.n[0] * me[0] + o.n[1] * me[2] + o.d;
        const float el = (at[0] - me[0]) * l[0] + (at[2] - me[2]) * l[1];
        // An obstacle beyond the enemy as seen from player 1 first (user, 2026-11-11: "with the wall at the side Chris strikes sideways, across
        // the wall": a side wall chosen turned him to it): its way off the way from him to the enemy costs (80 cm for a half turn), more than
        // 110 degrees off (behind him) refused; then 70 refused the walls the user meant ("I place an enemy near a wall and get the steal").
        const float off = std::hypot(at[0] - me[0], at[2] - me[2]) > 40.0f
                              ? std::fabs(std::remainder(yaw - std::atan2(at[0] - me[0], at[2] - me[2]), 2.0f * kPi))
                              : 0.0f;
        if (off > 110.0f * kPi / 180.0f) {
            if (!quiet) LOG("env executions: %s %.0f cm away (way %.0f deg) behind player 1", o.wall ? "a wall" : "a low obstacle", o.dist,
                            yaw * 180.0f / kPi);
            continue;
        }
        // The enemy at the obstacle (user, 2026-11-11: "the enemy must always be at the wall"): its span in front of the enemy where its head
        // meets it (a wall at 130 cm, a low obstacle under its top); the enemy kept where it is along it, inside the span by 15 cm, its middle
        // when narrower than 30.
        const float wallHeights[] = { 130.0f, 110.0f, 150.0f, 90.0f };
        const float lowHeights[] = { o.top > 0.0f ? std::max(45.0f, o.top - 12.0f) : 80.0f, 70.0f, 95.0f, 55.0f };
        float lo = 0.0f, hi = 0.0f;
        if (!ObstacleSpan(at, o.n, o.dist, o.wall ? wallHeights : lowHeights, 4, lo, hi)) {
            if (!quiet) LOG("env executions: %s %.0f cm away (way %.0f deg) not straight in front of the enemy", o.wall ? "a wall" : "a low obstacle",
                            o.dist, yaw * 180.0f / kPi);
            continue;
        }
        const float target = hi - lo < 40.0f ? (lo + hi) * 0.5f : std::clamp(0.0f, lo + 20.0f, hi - 20.0f);
        for (const EnvPairGeometry& g : kEnvPairs) {
            const int k = CombatIndex(g.id);
            if (g.kind != (o.wall ? kEnvWall : kEnvLow) || k < 0 || (only >= 0 && k != only)) continue;
            float ahead = kRe6PairDistance, left = 0.0f;
            for (const Re6PairPlace& place : kRe6PairPlaces)
                if (std::strcmp(g.id, place.id) == 0) ahead = place.distance, left = place.left;
            // Along the obstacle's normal both places are fixed; along it the J'avo at `target`, player 1 wherever that puts him.
            const float vn = g.obstacle - ahead - o.dist, pn = g.obstacle - distP;
            const float s = target - left;
            const float moveV = std::hypot(vn, target), moveP = std::hypot(pn, el + s);
            // The J'avo's turn: its side faces him (its back to the obstacle) or turns its back to him (facing the obstacle).
            float turn = std::fabs(std::remainder(enemyYaw - (g.behind ? yaw : yaw + kPi), 2.0f * kPi));
            const float cost = std::max(moveV, moveP) + turn / kPi * 40.0f + off / kPi * 80.0f;
            if (!quiet)
            LOG("env executions: %s: %s %.0f cm away (way %.0f deg, its top %.0f cm, %.0f cm wide there): the Majini moved %.0f cm, player 1 %.0f "
                "cm, turned %.0f deg", g.id + 22, o.wall ? "a wall" : "a low obstacle", o.dist, yaw * 180.0f / kPi, o.top, hi - lo + 10.0f, moveV,
                moveP, turn * 180.0f / kPi);
            if (std::max(moveV, moveP) > tolerance || cost >= bestCost) continue;
            // His place: the enemy moved on to the obstacle's distance, then along it.
            const float q[2] = { at[0] + (g.obstacle - o.dist) * o.n[0], at[2] + (g.obstacle - o.dist) * o.n[1] };
            const float origin[3] = { q[0] + s * l[0], me[1], q[1] + s * l[1] };
            // Nothing between him and that place.
            const float way = std::hypot(origin[0] - me[0], origin[2] - me[2]);
            EnvHit hit;
            if (way > 15.0f && EnvProbe(me, std::atan2(origin[0] - me[0], origin[2] - me[2]), 60.0f, way, 20.0f, hit) &&
                std::hypot(hit.point[0] - me[0], hit.point[2] - me[2]) < way - 10.0f) {
                if (!quiet) LOG("env executions: %s: something in his way to its place", g.id + 22);
                continue;
            }
            best = k;
            bestCost = cost;
            plan.yaw = yaw;
            std::memcpy(plan.origin, origin, sizeof(origin));
        }
    }
    if (best < 0) return -1;
    plan.enemy = enemy;
    plan.move = best;
    const DWORD now = GetTickCount();
    plan.tick = now ? now : 1;
    g_envPlan = plan;
    if (!quiet) LOG("env executions: %s chosen on %08X (tolerance %.0f cm)", kCombat[best].name, static_cast<unsigned>(enemy), tolerance);
    return best;
}

// From a height: a Majini 70-140 cm above or under player 1 (on its feet), the ledge's face between them (swept from the lower one towards
// the other at 40 cm), each pair of that side placed as the walls' (its edge `obstacle` ahead of player 1's place), within the tolerance.
// `quiet`: no log (the prompt's check, every 100 ms).
int EnvHeightFor(uintptr_t p, float tolerance, int only, uintptr_t onlyEnemy, bool quiet) {
    float me[3]{};
    if (!p || !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), me, sizeof(me))) return -1;
    constexpr float kPi = 3.14159265f;
    int best = -1;
    float bestCost = 1e9f;
    EnvPlan plan;
    for (uintptr_t e : actors::Enemies()) {
        int16_t health = 0;
        float at[3]{}, quat[4]{};
        if ((onlyEnemy && e != onlyEnemy) || !IsMajiniEnemy(e) || !mem::Read(e + 0x1364, health) || health <= 0 || EnemyOnGround(e) ||
            !mem::SafeRead(reinterpret_cast<void*>(e + 0x30), at, sizeof(at)) || !mem::SafeRead(reinterpret_cast<void*>(e + 0x40), quat, sizeof(quat)))
            continue;
        const float dy = at[1] - me[1], h = std::fabs(dy), flat = std::hypot(at[0] - me[0], at[2] - me[2]);
        if (h < 70.0f || h > 140.0f || flat > 170.0f + tolerance) continue;
        const bool below = dy > 0.0f;  // player 1 below it
        const float* low = below ? me : at;
        const float* up = below ? at : me;
        EnvHit hit;
        if (!EnvProbe(low, std::atan2(up[0] - low[0], up[2] - low[2]), 40.0f, flat + 60.0f, 10.0f, hit) || std::fabs(hit.normal[1]) > 0.35f)
            continue;
        const float len = std::hypot(hit.normal[0], hit.normal[2]), sign = below ? 1.0f : -1.0f;  // towards player 1's side
        const float n[2] = { sign * hit.normal[0] / len, sign * hit.normal[2] / len };
        const float d = sign * (hit.d + hit.normal[1] * hit.point[1]) / len;
        const float distP = n[0] * me[0] + n[1] * me[2] + d, distE = n[0] * at[0] + n[1] * at[2] + d;
        if (distP < -15.0f || distE > 15.0f) continue;  // each on its own side of the face
        const float yaw = std::atan2(-n[0], -n[1]);
        const float l[2] = { std::cos(yaw), -std::sin(yaw) };
        const float el = (at[0] - me[0]) * l[0] + (at[2] - me[2]) * l[1];
        const float enemyYaw = 2.0f * std::atan2(quat[1], quat[3]);
        for (const EnvPairGeometry& g : kEnvPairs) {
            const int k = CombatIndex(g.id);
            if (g.kind != (below ? kEnvBelow : kEnvAbove) || k < 0 || (only >= 0 && k != only)) continue;
            float ahead = kRe6PairDistance, left = 0.0f;
            for (const Re6PairPlace& place : kRe6PairPlaces)
                if (std::strcmp(g.id, place.id) == 0) ahead = place.distance, left = place.left;
            // The J'avo kept where it is along the ledge (user: "the enemy must always be at the wall"), player 1 moved along it.
            const float vn = g.obstacle - ahead - distE, pn = g.obstacle - distP;
            const float s = -left;
            const float moveV = std::fabs(vn), moveP = std::hypot(pn, el + s);
            const float turn = std::fabs(std::remainder(enemyYaw - (g.behind ? yaw : yaw + kPi), 2.0f * kPi));
            const float cost = std::max(moveV, moveP) + turn / kPi * 40.0f;
            if (!quiet)
                LOG("env executions: %s: a ledge of %.0f cm, the Majini %.0f cm past its edge, player 1 %.0f cm before it: the Majini moved "
                    "%.0f cm, player 1 %.0f cm, turned %.0f deg", g.id + 22, h, -distE, distP, moveV, moveP, turn * 180.0f / kPi);
            if (std::max(moveV, moveP) > tolerance || cost >= bestCost) continue;
            const float q[2] = { at[0] + (g.obstacle - distE) * n[0], at[2] + (g.obstacle - distE) * n[1] };
            best = k;
            bestCost = cost;
            plan.enemy = e;
            plan.yaw = yaw;
            plan.height = h;
            plan.origin[0] = q[0] + s * l[0];
            plan.origin[1] = me[1];
            plan.origin[2] = q[1] + s * l[1];
        }
    }
    if (best < 0) return -1;
    plan.move = best;
    const DWORD now = GetTickCount();
    plan.tick = now ? now : 1;
    g_envPlan = plan;
    if (!quiet)
        LOG("env executions: %s chosen on %08X (a ledge of %.0f cm, tolerance %.0f cm)", kCombat[best].name, static_cast<unsigned>(plan.enemy),
            plan.height, tolerance);
    return best;
}

// The prompt (quick_knife.cpp: a crossing prompt taken as a melee prompt, on the attack key with "Corps à corps sur la touche de tir"):
// an execution from a height ready, checked every 100 ms.
std::atomic<bool> g_envHeightReady{ false };
// quick_knife.cpp's pad query hook (the game thread), every 100 ms: the plans the render thread's starts take (g_envWallCache: the
// staggered enemy nearest player 1 within 3 m; g_envHeightCache and the crossing prompt's g_envHeightReady).
std::atomic<bool> g_envScanAsked{ false };  // "Tester la détection du décor": a scan round player 1 at the next check
void EnvExecTick() {
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 100;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    float me[3]{};
    if (g_envScanAsked.exchange(false) && p && mem::SafeRead(reinterpret_cast<void*>(p + 0x30), me, sizeof(me))) {
        const float from[3] = { 0.0f, 200.0f, 0.0f }, to[3] = { 0.0f, -50.0f, 0.0f }, away[3] = { me[0] + 1e5f, me[1], me[2] };
        EnvHit hit;
        const bool met = EnvSweep(me, 0.0f, from, to, 3.0f, hit);
        LOG("env scan: down from 2 m over player 1 (%.1f %.1f %.1f): %s, point y %.1f, normal (%.2f %.2f %.2f), flags %X", me[0], me[1], me[2],
            met ? "met" : "nothing", hit.point[1], hit.normal[0], hit.normal[1], hit.normal[2], hit.flags);
        EnvObstacle found[16];
        const int count = FindEnvObstacles(me, away, 300.0f, found, 16);
        for (int i = 0; i < count; ++i)
            LOG("env scan: %s %.0f cm away, way %.0f deg, its top %.0f cm", found[i].wall ? "a wall" : "a low obstacle", found[i].dist,
                std::atan2(-found[i].n[0], -found[i].n[1]) * 57.2958f, found[i].top);
        LOG("env scan: %d obstacle(s) within 3 m", count);
    }
    float tolerance = 0.0f, at[3]{};
    std::lock_guard<std::mutex> guard(g_envLock);
    const EnvPlan kept = g_envPlan;  // a pressed prompt's plan is not overwritten by the checks
    g_envWallCache = {};
    if (p && EnvWallTolerance(tolerance) && !g_active.move) {
        const uintptr_t e = NearestEnemy(p, 300.0f, at);
        if (e && Re6Staggered(e) && EnvExecutionFor(p, e, -1, tolerance, true) >= 0) g_envWallCache = g_envPlan;
    }
    g_envHeightCache = {};
    const bool ready = p && EnvHeightTolerance(tolerance) && !g_active.move && EnvHeightFor(p, tolerance, -1, 0, true) >= 0;
    if (ready) g_envHeightCache = g_envPlan;
    g_envPlan = kept;
    if (ready != g_envHeightReady.exchange(ready)) LOG("env executions: from a height %s", ready ? "ready" : "no longer ready");
}

int EnvCachedWall(uintptr_t enemy) {
    std::lock_guard<std::mutex> guard(g_envLock);
    if (!enemy || g_envWallCache.enemy != enemy || g_envWallCache.move < 0 || GetTickCount() - g_envWallCache.tick > 400) return -1;
    g_envPlan = g_envWallCache;
    const DWORD now = GetTickCount();
    g_envPlan.tick = now ? now : 1;
    return g_envPlan.move;
}

void RequestCombatMove(int k, uintptr_t target);  // below

// swaps.cpp: a crossing prompt pressed (climb up 0x2B, jump down 0x2A / 0x2C) - the execution from a height it allows, at the next frame.
bool PlayEnvHeightExecution() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    float tolerance = 0.0f;
    if (!p || !EnvHeightTolerance(tolerance)) return false;
    std::lock_guard<std::mutex> guard(g_envLock);
    const int k = EnvHeightFor(p, tolerance, -1, 0, false);
    if (k < 0) return false;
    RequestCombatMove(k, g_envPlan.enemy);
    return true;
}

// swaps.cpp: a staggered enemy's prompt pressed - its execution against an obstacle near it, at the next frame; false: none in reach.
bool PlayEnvExecution(uintptr_t target) {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    std::lock_guard<std::mutex> guard(g_envLock);
    const int k = EnvExecutionFor(p, target);
    if (k < 0) return false;
    RequestCombatMove(k, target);
    return true;
}

// StartCombatMove: player 1 put at the pair's place, turned to its obstacle (`from`: where he stood). The plan of the prompt (fresh), else
// one made now for that move (its free melee shortcut: any distance); none: played where he stands.
bool AlignPlayerToObstacle(uintptr_t p, uintptr_t e, int k, float (&from)[3]) {
    const EnvPairGeometry* g = EnvGeometryOf(k);
    if (!g || !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), from, sizeof(from))) return false;
    const bool height = g->kind == kEnvBelow || g->kind == kEnvAbove;
    std::lock_guard<std::mutex> guard(g_envLock);
    const bool fresh = g_envPlan.enemy == e && g_envPlan.move == k && GetTickCount() - g_envPlan.tick < 1500;
    // A free melee shortcut: the game thread's last check, if it chose that move on that enemy (no probe from this render thread).
    const EnvPlan& cached = height ? g_envHeightCache : g_envWallCache;
    if (!fresh && cached.enemy == e && cached.move == k && GetTickCount() - cached.tick < 400) g_envPlan = cached;
    else if (!fresh) {
        LOG("wesker moves: %s: no obstacle found, played where he stands", kCombat[k].name);
        return false;
    }
    const float yaw = g_envPlan.yaw;
    const float place[3] = { g_envPlan.origin[0], from[1], g_envPlan.origin[2] };
    const float quat[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x30), place, sizeof(place));
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x40), quat, sizeof(quat));
    mem::Write(p + 0x2A10, yaw);
    mem::Write(p + 0x1024, yaw);
    g_envPlan.enemy = 0;
    return true;
}

bool StartCombatMove(int k, uintptr_t forced, bool chained) {
    // "Endurance" (stamina.cpp): a move of the player's own (not the throw's catch nor a parry's riposte) costs it.
    const bool own = !chained && !forced;
    if (own && !StaminaAllows()) return false;
    if (k < 0 || k >= static_cast<int>(std::size(kCombat))) return false;
    // `chained`: from a move of his in progress (his carrier action is not "free").
    // A riposte right after a parry cut his move (CutMoveForParry): from the cut move's carrier action too (its end not yet taken).
    const DWORD cut = g_parryCut.load();
    const bool afterCut = cut && static_cast<int32_t>(GetTickCount() - cut) < 500;
    const uintptr_t p = chained ? (actors::InGame() ? actors::Player(0) : 0) : FreePlayer1(afterCut);
    if (!p) return false;
    if (afterCut) g_parryCut = 0;
    float at[3]{};
    const CombatMove& move = kCombat[k];
    g_throwOther = 0;
    const uintptr_t target = forced ? forced : move.lock ? NearestEnemy(p, 300.0f, at, ThrowMove(k) || move.victim >= 0) : 0;
    // RE6's counter instead of Jill's or Josh's throw on a leaping enemy (LeapEnemy; the parries' ripostes and the melee swaps come
    // here too), the two-handed weapon's variant with one in hand.
    // Jake's Ground: its face-down variant on an enemy lying face down (user, 2026-11-10: "he still grabs it by the head" - logged: the
    // variant never started, LyingFaceDown's pelvis test failing): its pelvis's front pointing down at all, its hips under 45 cm
    // (DownFaceDown).
    if (k == kRe6JakeGround && target && DownFaceDown(target)) {
        if (own) SpendForMove(k);
        return StartCombatMove(kRe6JakeGroundBelly, target, true);
    }
    if ((k == kThrowMove || k == kThrow2Move || k == kJoshCounterMove) && target && LeapEnemy(target)) {
        const int counter = Re6WeaponTwoHanded(p) ? kRe6CounterLongMove : kRe6CounterMove;
        LOG("wesker moves: %s on the leaping %s %08X: %s instead", move.name, UnitClassName(target), static_cast<unsigned>(target),
            kCombat[counter].name);
        if (own) SpendForMove(k);
        return StartCombatMove(counter, target, true);
    }
    if (forced && !mem::SafeRead(reinterpret_cast<void*>(forced + 0x30), at, sizeof(at))) return false;
    if (k == kThrowMove && target && IsJillUnit(target)) {
        float pos[3]{};
        int16_t health = 0;
        if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return false;
        const float yaw = std::atan2(pos[0] - at[0], pos[2] - at[2]);
        const float quat[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
        mem::SafeWrite(reinterpret_cast<void*>(target + 0x40), quat, sizeof(quat));
        EnsureCommonEffectsHook();
        EnsureGrabRefusalHook();
        const int caught = IsClass(target, kJillVtable) ? 0x27 : JillAction(target, "Caught");
        if (caught < 0 || !RequestEnemy(target, static_cast<uint32_t>(caught), p)) {
            LOG("wesker moves: Jill (%s) refused Caught %X", UnitClassName(target), caught);
            return false;
        }
        mem::Read(target + 0x1364, health);
        LOG("wesker moves: Jill caught by player 1 (health %d)", health);
        g_caught = target;
        g_caughtStart = GetTickCount();
        g_caughtPlayed = false;
        g_invulnerable = true;
        if (own) StaminaSpend();
        return true;
    }
    if (target && move.behind) {
        // His back to the enemy: facing the point opposite to it.
        float pos[3]{};
        if (mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) {
            const float away[3] = { 2.0f * pos[0] - at[0], at[1], 2.0f * pos[2] - at[2] };
            FacePlayerTowards(p, away);
        }
    } else if (target) {
        FacePlayerTowards(p, at);
    }
    EnsureCommonEffectsHook();
    if (!EnsureGrabRefusalHook()) LOG("wesker moves: no grab hook, player 1 may be grabbed during the move");
    // The whole-body slide with a two-handed weapon: RE6's ArA C9 upper body (re6_evade.lmt 50; user, 2026-11-09).
    // The slide's half-turn too (51: one hand, 52 two hands, 53 the knife, 54 grenades and items); the knife's and items' slides 57 / 58.
    uint16_t played = move.index;
    if (move.bank == kRe6EvadeBank && (move.index == 4 || move.index == kRe6SlideTurn)) {
        const bool turn = move.index == kRe6SlideTurn;
        const int heldPose = Re6HeldPoseSlot(p);
        if (heldPose == 28) played = turn ? static_cast<uint16_t>(kRe6SlideTurn + 2) : kRe6SlideKnife;
        else if (heldPose) played = turn ? static_cast<uint16_t>(kRe6SlideTurn + 3) : kRe6SlideItem;
        else if (Re6WeaponTwoHanded(p)) played = turn ? static_cast<uint16_t>(kRe6SlideTurn + 1) : kRe6SlideLong;
    }
    if (!PlayWeskerMove(move.bank, played, g_moveBeginOverride >= 0.0f ? g_moveBeginOverride : move.begin)) return false;
    if (Re6KnifePair(move.id)) KnifeIntoHand(p);  // RE6's knife counter / throat cut: the knife in hand
    if (ReverseGripMove(move.id)) EnsureAttachedMatrixHook();
    if (move.bank == kRe6MeleeBank) {  // RE6's weapon joint for the moves that move it
        LoadWeaponJoints();
        if (WeaponJointFor(move.index)) EnsureAttachedMatrixHook();
    }
    if (move.damage == 0 && !ThrowMove(k) && k != kThrow2Move && !(chained && move.bank == kRe6EvadeBank))
        PlayMoveGrunt(p);  // a dodge: his effort at its start (not RE6's getups, which follow one)
    // An RE4 pair on a Majini (the suplex, the neck break): its side of the pair from player 1's origin and yaw (the
    // pair's convention: its motion carries its place in front of him), through BackHoldDmg 0x9C as Jill's throw (its
    // motion 0x604 / 0x640 replaced by the SetMotion hook, collision off, contacts dropped, his speed); damage in the
    // move's window, a lethal one once it lies (ReleaseThrown: dead face up or down as it lies).
    // The big ones (user, 2026-10-31: "not on the big enemies: the same principle as Jill's throw"): the Executioner, the
    // chainsaw Majini, the Reaper... through their Dmg action carrying the pair's side (the puppet path of Jill's throw),
    // straight to their getup after it.
    bool paired = false;
    const bool pairMajini = move.victim >= 0 && target && IsMajiniEnemy(target);
    const PuppetClass* const pairPuppet = move.victim >= 0 && target && !pairMajini ? PuppetClassOf(target) : nullptr;
    if (pairMajini || (pairPuppet && EnsureEnemyActionHook())) {
        uint8_t hand = 0;
        g_pairMirror = mem::Read(p + 0x1AF9, hand) && hand == 1;  // a left-handed player 1: the pair mirrored
        // RE6's special counters: mirrored when the Majini holds its weapon in its left hand (its item +0x1C = 13), whatever player 1's hand.
        g_flipPlayerMirror = false;
        if (std::strcmp(move.id, "melee.free_re6_common_machete_counter") == 0 || std::strcmp(move.id, "melee.free_re6_common_gun_counter") == 0 ||
            std::strcmp(move.id, "melee.free_re6_common_club_counter_1") == 0 || std::strcmp(move.id, "melee.free_re6_common_club_counter_2") == 0 ||
            std::strcmp(move.id, "melee.free_re6_common_weapon_steal") == 0) {
            uintptr_t weapon = 0;
            uint32_t joint = 0;
            const bool left = mem::Read(target + 0x1B6C, weapon) && weapon && mem::Read(weapon + 0x1C, joint) && joint == 13;
            g_flipPlayerMirror = left != g_pairMirror.load();
            g_pairMirror = left;
            LOG("wesker moves: %s on a Majini holding its weapon in its %s hand%s", move.name, left ? "left" : "right",
                g_flipPlayerMirror ? ": player 1's motion mirrored for it" : "");
        }
        if (pairMajini) AlignPlayerToLying(p, target, move);  // Jake's Grounds: player 1 to the Majini, not the other way
        // An environmental execution: player 1 put at the pair's place and turned to the obstacle for the setup below, then given back his
        // place (the lock slides both units there over the first frames).
        float envFrom[3]{}, envEnemy[3]{};
        const bool env = pairMajini && mem::SafeRead(reinterpret_cast<void*>(target + 0x30), envEnemy, sizeof(envEnemy)) &&
                         AlignPlayerToObstacle(p, target, k, envFrom);
        const uint32_t victim = PairSideMotion(target, move.victim);
        float pos[3]{}, quat[4]{};
        if (victim && mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos)) &&
            mem::SafeRead(reinterpret_cast<void*>(p + 0x40), quat, sizeof(quat))) {
            const uintptr_t previous = g_thrownUnit.load(std::memory_order_relaxed);
            if (previous && previous != target) {
                bool alive = false;
                for (uintptr_t u : actors::Enemies()) alive = alive || u == previous;
                ReleaseThrown(alive);
            }
            CollisionOff(target);
            if (previous != target && !mem::Read(target + actors::off::Speed, g_thrownSpeed)) g_thrownSpeed = 0.0f;
            if (!EnsurePlayerContactHook()) LOG("wesker moves: no contact hook, the enemy will push player 1");
            mem::SafeWrite(reinterpret_cast<void*>(target + 0x30), pos, sizeof(pos));
            // Josh's victim side is made facing him (the game's melee target faces the player: P+0x2E20 = yaw + pi).
            const bool facing = move.victim >= kJoshPairVictim;
            // An RE6 Leg Back takes its enemy from behind (user, 2026-11-10: Chris's RE6 Leg Back; RE5's Leg Backs are from behind too): its
            // back to him, not facing him.
            // Jake's Ground on a Majini lying on its belly too (user, 2026-11-10: "Chris places himself at its head when he must be at its legs
            // as in the face-up one, turned the other way"; its side PeA 4E drags it -z where the face-up one's 4C drags it +z).
            const bool fromBehind = move.victim >= kRe6PairVictim && (std::strstr(move.id, "leg_back") != nullptr ||
                                                                      std::strstr(move.id, "throat_cut") != nullptr ||
                                                                      std::strcmp(move.id, "melee.free_re6_jake_ground_belly") == 0 ||
                                                                      // The armed-zombie pairs are made in player 1's own frame (their victim
                                                                      // side starts 1.2 m ahead, turned 180 degrees by its root: user, 2026-11-10,
                                                                      // "the enemy teleports 2 m behind Chris"): his yaw, not turned to him.
                                                                      std::strncmp(move.id, "melee.free_re6_common_club_counter", 34) == 0 ||
                                                                      std::strcmp(move.id, "melee.free_re6_common_weapon_steal") == 0 ||
                                                                      // the table from the front: the J'avo's back to him (recorded)
                                                                      std::strcmp(move.id, "melee.free_re6_common_env_table_front") == 0 ||
                                                                      std::strcmp(move.id, "melee.free_re6_common_env_below_behind") == 0 ||
                                                                      std::strcmp(move.id, "melee.free_re6_common_env_above_behind") == 0);
            const float facingYaw = 2.0f * std::atan2(quat[1], quat[3]) + (facing && !fromBehind ? 3.14159265f : 0.0f);
            const float victimQuat[4] = { 0.0f, std::sin(facingYaw * 0.5f), 0.0f, std::cos(facingYaw * 0.5f) };
            // RE6's sides start at their own origin: the enemy kRe6PairDistance in front of him (its offset kept by the lock).
            float origin[3] = { pos[0], pos[1], pos[2] };
            if (move.victim >= kRe6PairVictim) {
                const float yaw = 2.0f * std::atan2(quat[1], quat[3]);
                // Each pair its own place (kRe6PairPlaces; a root's +x is its left: (cos, -sin) in the world).
                float distance = kRe6PairDistance, left = 0.0f;
                for (const Re6PairPlace& place : kRe6PairPlaces)
                    if (std::strcmp(move.id, place.id) == 0) distance = place.distance, left = place.left;
                if (g_pairMirror.load(std::memory_order_relaxed)) left = -left;  // the pair mirrored
                origin[0] += distance * std::sin(yaw) + left * std::cos(yaw);
                origin[2] += distance * std::cos(yaw) - left * std::sin(yaw);
                LOG("wesker moves: %s: its victim placed %.0f cm ahead, %.0f cm to the left%s", move.name, distance, left,
                    g_pairMirror.load() ? " (mirrored: player 1 left-handed)" : "");
                // Its root at frame 0 (the victims re-rooted under their hips: tools/import_re6_melee.py reroot) as the lock will put it, no
                // jump at its first frame.
                float root0[3]{};
                float placed[3] = { origin[0], origin[1], origin[2] };
                if (PairRootAt(move.victim, 0.0f, root0)) {
                    const float y = facing && !fromBehind ? yaw + 3.14159265f : yaw, c = std::cos(y), s = std::sin(y);
                    placed[0] += root0[0] * c + root0[2] * s;
                    placed[2] += -root0[0] * s + root0[2] * c;
                }
                if (env) {  // it stays where it stands, slid to its place by the lock
                    g_pairLock.slideV[0] = envEnemy[0] - placed[0];
                    g_pairLock.slideV[1] = envEnemy[2] - placed[2];
                    placed[0] = envEnemy[0];
                    placed[2] = envEnemy[2];
                    // From below its side is made 100 cm up from the pair's floor (its unit on player 1's floor, the ledge's height - 100
                    // over it); from above on its own floor.
                    if (const EnvPairGeometry* g = EnvGeometryOf(k); g && g->kind == kEnvBelow) placed[1] = pos[1] + g_envPlan.height - 100.0f;
                    else if (g && g->kind == kEnvAbove) placed[1] = envEnemy[1];
                    mem::SafeWrite(reinterpret_cast<void*>(target + 0x30), envEnemy, sizeof(envEnemy));  // its collision's from there
                    PlaceUnit(target, placed);
                }
                mem::SafeWrite(reinterpret_cast<void*>(target + 0x30), placed, sizeof(placed));
            }
            mem::SafeWrite(reinterpret_cast<void*>(target + 0x40), facing ? victimQuat : quat, sizeof(quat));
            g_thrownMotion = victim;
            g_pairMotion = victim;
            g_thrownLethal = false;
            const DWORD now = GetTickCount();
            g_thrownSince = now ? now : 1;
            g_thrownUnit.store(target, std::memory_order_release);
            g_pairVictimTick = now ? now : 1;
            g_pairVictim = target;
            if (pairMajini) {
                paired = RequestEnemy(target, 0x9C);
            } else {
                if (pairPuppet->ownGetup) {
                    kPuppetMotions[2] = pairPuppet->ownGetup;
                } else {
                    if (!g_majiniRiseList) g_majiniRiseList = LoadList("uEm10", "pawn\\em\\em10\\motion\\em10_g05");
                    const bool rise = g_majiniRiseList && SetBank(target, g_majiniRiseList, kMajiniRiseBank);
                    kPuppetMotions[2] = rise ? (static_cast<uint32_t>(kMajiniRiseBank) << 8) | 0x54 : victim;
                }
                kPuppetMotions[0] = kPuppetMotions[1] = victim;  // its corpse goes on with it (PairCorpseMotion)
                g_pairPuppet = true;
                g_puppetClass = pairPuppet;
                g_puppetThen = -1;
                g_puppetPhase = 1;
                g_puppetMotion = victim;
                g_puppetSince = now ? now : 1;
                g_puppet = target;
                g_puppetOwn = true;
                paired = RequestEnemy(target, pairPuppet->dmg[0]);
                g_puppetOwn = false;
                if (!paired) {
                    g_puppetMotion = 0;
                    EndPuppet(target, "refused its Dmg");
                }
            }
            // Test sliders (user, 2026-10-31: "to move Chris and set it perfectly", removed afterwards): Chris moved
            // from the pair's placement, in his own frame, and turned.
            const int test = move.victim == 2 ? 0 : 1;
            float side = 0.0f, forward = 0.0f, turn = 0.0f;
            if (!facing) PairTestOffset(test, side, forward, turn);  // the RE4 pairs' only
            if (side != 0.0f || forward != 0.0f || turn != 0.0f) {
                const float yaw = 2.0f * std::atan2(quat[1], quat[3]);
                const float moved[3] = { pos[0] + forward * std::sin(yaw) + side * std::cos(yaw), pos[1],
                                         pos[2] + forward * std::cos(yaw) - side * std::sin(yaw) };
                const float turned = yaw + turn * 3.14159265f / 180.0f;
                const float q[4] = { 0.0f, std::sin(turned * 0.5f), 0.0f, std::cos(turned * 0.5f) };
                mem::SafeWrite(reinterpret_cast<void*>(p + 0x30), moved, sizeof(moved));
                mem::SafeWrite(reinterpret_cast<void*>(p + 0x40), q, sizeof(q));
            }
            if (paired) {
                const float yaw = 2.0f * std::atan2(quat[1], quat[3]);
                const float slideV[2] = { env ? g_pairLock.slideV[0] : 0.0f, env ? g_pairLock.slideV[1] : 0.0f };
                StartPairLock(target, p, move, origin, facing ? facingYaw : yaw, yaw + turn * 3.14159265f / 180.0f);
                if (env) {
                    g_pairLock.env = true;
                    if (const EnvPairGeometry* g = EnvGeometryOf(k); g && g->kind == kEnvBelow) {
                        g_pairLock.victimDy = g_envPlan.height - 100.0f;
                        g_pairLock.decayFrom = g->decayFrom;
                        g_pairLock.decayTo = g->decayTo;
                    } else if (g && g->kind == kEnvAbove) {
                        float root0[3]{};
                        g_pairLock.victimDy = -g_envPlan.height;
                        g_pairLock.drivePlayer = true;
                        g_pairLock.dropScale = g_envPlan.height / 100.0f;
                        if (PlayerSideRootAt(move.bank, move.index, 0.0f, root0)) g_pairLock.rootY0 = root0[1];
                    }
                    g_pairLock.slideP[0] = g_pairLock.appliedP[0] = envFrom[0] - pos[0];
                    g_pairLock.slideP[1] = g_pairLock.appliedP[1] = envFrom[2] - pos[2];
                    g_pairLock.slideV[0] = slideV[0];
                    g_pairLock.slideV[1] = slideV[1];
                    LOG("wesker moves: %s: player 1 slid %.0f cm to his place, the Majini %.0f cm to its own", move.name,
                        std::hypot(g_pairLock.slideP[0], g_pairLock.slideP[1]), std::hypot(slideV[0], slideV[1]));
                }
            }
            if (env) {  // where he stood (the lock slides him)
                const float back[3] = { envFrom[0], pos[1], envFrom[2] };
                mem::SafeWrite(reinterpret_cast<void*>(p + 0x30), back, sizeof(back));
            }
            LOG("wesker moves: %s with %s %08X (its side %X from (%.1f %.1f %.1f), Chris moved %.0f / %.0f cm, turned %.0f "
                "deg%s)",
                move.name, pairMajini ? "Majini" : pairPuppet->name, static_cast<unsigned>(target), victim, pos[0], pos[1],
                pos[2], side, forward, turn, paired ? "" : ", its action refused");
            if (!paired) {
                g_pairVictim = 0;
                ReleaseThrown(true);
            }
        }
    }
    if (ThrowMove(k) && target) {
        float pos[3]{};
        if (mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) {
            const float yaw = std::atan2(at[0] - pos[0], at[2] - pos[2]);
            if (!g_jillDamageList) g_jillDamageList = LoadList("uEm83", "pawn\\em\\em83\\motion\\em83dmg");
            const bool majini = IsMajiniEnemy(target);
            const PuppetClass* const puppetClass = majini ? nullptr : PuppetClassOf(target);
            const bool executioner = puppetClass && EnsureEnemyActionHook();
            // Jill's second throw on a Majini: Chris's side of it (em84evt 0x00 in its bank C); else her thrown em83dmg.
            const bool second = k == kThrow2Catch && majini;
            if (second && !g_jill2EventList) g_jill2EventList = LoadList("uEm84", "pawn\\em\\em84\\motion\\em84evt");
            const bool banked = second ? g_jill2EventList && SetBank(target, g_jill2EventList, kJill2VictimBank)
                                       : g_jillDamageList && SetBank(target, g_jillDamageList, kJillBank);
            g_thrownMotion = second ? kJill2VictimMotion : kJillThrownMotion;
            if (k == kThrow2Catch && IsJillUnit(target)) {
                StartJillVictim(target, pos, yaw);
            } else if ((majini || executioner) && banked) {
                // Jill's thrown motion from his origin, facing him, as in Caught (her motion keeps her body 102 cm
                // behind its origin: em83dmg 0x23 joint 0 at (10, 99, -102)). BackHoldDmg's init (RVA 0x8269A0) puts it
                // there anyway, on its grabber [+0x1BA4] (his vfunc +0x38(-1)). Its body then pushed him 40-80 cm away
                // (cObjAdj contacts; logged 2026-10-22, user: "a distance between the two"): player.cpp's contact hook
                // drops the contacts with the thrown Majini (user: put 1 m away instead it no longer flew over him).
                const float back = yaw + 3.14159265f;
                const float place[3] = { pos[0], pos[1], pos[2] };
                const float quat[4] = { 0.0f, std::sin(back * 0.5f), 0.0f, std::cos(back * 0.5f) };
                const uintptr_t previous = g_thrownUnit.load(std::memory_order_relaxed);
                if (previous && previous != target) {
                    bool alive = false;
                    for (uintptr_t u : actors::Enemies()) alive = alive || u == previous;
                    ReleaseThrown(alive);
                }
                CollisionOff(target);
                if (previous != target && !mem::Read(target + actors::off::Speed, g_thrownSpeed)) g_thrownSpeed = 0.0f;
                LOG("wesker moves: throw from player (%.1f %.1f %.1f) yaw %.2f, %s (%.1f %.1f %.1f) put at (%.1f %.1f %.1f)",
                    pos[0], pos[1], pos[2], yaw, majini ? "Majini" : puppetClass->name, at[0], at[1], at[2], place[0],
                    place[1], place[2]);
                if (!EnsurePlayerContactHook()) LOG("wesker moves: no contact hook, the Majini will push player 1");
                mem::SafeWrite(reinterpret_cast<void*>(target + 0x30), place, sizeof(place));
                mem::SafeWrite(reinterpret_cast<void*>(target + 0x40), quat, sizeof(quat));
                const DWORD now = GetTickCount();
                g_thrownSince = now ? now : 1;
                g_thrownUnit.store(target, std::memory_order_release);
                if (majini) {
                    if (!RequestEnemy(target, 0x9C)) LOG("wesker moves: Majini refused BackHoldDmg");
                } else {
                    kPuppetMotions[0] = kJillThrownMotion;  // Jill's own (an RE4 pair put its side there)
                    kPuppetMotions[1] = 0xE25;
                    g_pairPuppet = false;
                    if (puppetClass->ownGetup) {
                        kPuppetMotions[2] = puppetClass->ownGetup;
                    } else {
                        if (!g_majiniRiseList) g_majiniRiseList = LoadList("uEm10", "pawn\\em\\em10\\motion\\em10_g05");
                        const bool rise = g_majiniRiseList && SetBank(target, g_majiniRiseList, kMajiniRiseBank);
                        kPuppetMotions[2] = rise ? (static_cast<uint32_t>(kMajiniRiseBank) << 8) | 0x54 : 0xE06;
                        if (!rise) LOG("wesker moves: no Majini getup for the %s, Jill's instead", puppetClass->name);
                    }
                    g_puppetClass = puppetClass;
                    g_puppetThen = -1;
                    g_puppetPhase = 1;
                    g_puppetMotion = kPuppetMotions[0];
                    g_puppetSince = now ? now : 1;
                    g_puppet = target;
                    g_puppetOwn = true;
                    const bool accepted = RequestEnemy(target, puppetClass->dmg[0]);
                    g_puppetOwn = false;
                    if (!accepted) {
                        g_puppetMotion = 0;
                        EndPuppet(target, "refused its Dmg");
                    }
                }
            } else {
                // Another enemy: 70 cm in front of player 1, its back to him, its damage only (user, 2026-10-25: held
                // and killed at the end it one-shot the dogs and the Nightmare DLC's enemies; forced_death.cpp leaves
                // it alone).
                g_throwOther = target;
                const DWORD tick = GetTickCount();
                g_throwOtherTick = tick ? tick : 1;
                const float place[3] = { pos[0] + 70.0f * std::sin(yaw), at[1], pos[2] + 70.0f * std::cos(yaw) };
                mem::SafeWrite(reinterpret_cast<void*>(target + 0x30), place, sizeof(place));
                const float quat[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
                mem::SafeWrite(reinterpret_cast<void*>(target + 0x40), quat, sizeof(quat));
            }
        }
    }
    if (k == kThrow2Move) g_leapTarget = target;
    EndStolenWeapon();  // a move started over a stealing one
    g_active = {};
    // A counter given its target (the RE6 parry's riposte): it hits its attacker at its window, contact or not, as the projections.
    g_active.only = ThrowMove(k) || paired || (forced && (Re6CounterMove(k) || k == kRe6Sweep)) ? target : 0;
    g_active.move = &move;
    g_active.motion = MoveMotion(move.bank, played);  // the pseudo banks resolved
    g_active.start = GetTickCount();
    g_active.yaw = YawOf(p);
    g_invulnerable = !Re6MeleeVulnerable(k);
    if (own) SpendForMove(k);
    return true;
}

// player.cpp's cObjAdj contact hook: player 1 is not pushed by the Majini of Jill's throw, nor by the RE6 parry's
// target during the counter.
// licker.cpp: the Majini `majini` holding player 1 from behind breaks his neck (TickNeckBreak above).
bool StartReversedNeckBreak(uintptr_t majini) {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t list = 0;
    float pos[3]{}, quat[4]{};
    if (!p || !majini || g_neckKiller.load() || !mem::Read(p + 0xA88 + 4 * 1, list) || !list ||
        !mem::SafeRead(reinterpret_cast<void*>(majini + 0x30), pos, sizeof(pos)) ||
        !mem::SafeRead(reinterpret_cast<void*>(majini + 0x40), quat, sizeof(quat)) || !EnsureSetMotionHook())
        return false;
    // Always Chris's neck break (user, 2026-10-31: with Sheva "it played the throat cut" - her Leg Back NECK is her knife
    // across the throat): his own list for Chris (melee rows 0 / 8), else Chris's in the player's bank 1.
    const int row = Player1MeleeRow();
    const bool own = row == 0 || row == 8;
    const char* name = "not Chris's";
    g_neckAttacker = kNeckAttacker;
    g_neckVictim = kNeckVictim;
    if (!own) {
        void* chris = LoadList("uPl00ChrisNormal", "pawn\\pl\\pl00\\motion\\pl00action");
        if (!chris) {
            LOG("back grab: player 1's melee row %d (%s), no Chris's list", row, name);
            return false;
        }
        if (!g_neckOwnList || g_neckListPlayer != p) {
            g_neckOwnList = reinterpret_cast<void*>(list);
            g_neckListPlayer = p;
        }
        g_neckRestoreAt = 0;
        if (reinterpret_cast<uintptr_t>(chris) != list && !SetBank(p, chris, 1)) return false;
        list = reinterpret_cast<uintptr_t>(chris);
        LOG("back grab: player 1's melee row %d (%s): Chris's neck break lent", row, name);
    }
    if (!SetBank(majini, reinterpret_cast<void*>(list), kNeckBank)) return false;
    const uintptr_t previous = g_thrownUnit.load(std::memory_order_relaxed);
    if (previous && previous != majini) {
        bool alive = false;
        for (uintptr_t u : actors::Enemies()) alive = alive || u == previous;
        ReleaseThrown(alive);
    }
    CollisionOff(majini);
    if (previous != majini && !mem::Read(majini + actors::off::Speed, g_thrownSpeed)) g_thrownSpeed = 0.0f;
    if (!EnsurePlayerContactHook()) LOG("back grab: no contact hook, the Majini will push player 1");
    const DWORD now = GetTickCount();
    std::memcpy(g_neckOrigin, pos, sizeof(pos));
    std::memcpy(g_neckQuat, quat, sizeof(quat));
    g_neckSince = now;
    g_neckPlayed = false;
    g_neckLength = 281.0f;
    g_thrownMotion = (static_cast<uint32_t>(kNeckBank) << 8) | g_neckAttacker;
    g_thrownLethal = false;
    g_thrownSince = now ? now : 1;
    g_thrownUnit.store(majini, std::memory_order_release);
    g_neckKiller = majini;
    g_neckMajini = majini;
    g_neckMajiniSince = now ? now : 1;
    if (!EnsureEnemyActionHook()) LOG("back grab: no enemy request hook, the Majini will end lying");
    const bool killer = RequestEnemy(majini, 0x9C);
    const bool victim = PlayWeskerMove(1, g_neckVictim);
    float before = 0;
    mem::Read(p + 0x1024, before);
    PlaceAt(p, pos, quat, true);
    LOG("back grab: Majini %08X breaks player 1's neck (its 0x%X %s, his 0x%X %s) from (%.0f %.0f %.0f), yaw %.2f (his "
        "%.2f)",
        static_cast<unsigned>(majini), g_thrownMotion.load(), killer ? "asked" : "refused", (1u << 8) | g_neckVictim,
        victim ? "asked" : "refused", pos[0], pos[1], pos[2], 2.0f * std::atan2(quat[1], quat[3]), before);
    if (!victim) NeckBreakKill(p, "his victim's motion refused");
    return true;
}

// parry.cpp / perfect_dodge.cpp: player 1 having his neck broken by a Majini from behind - no parry nor dodge then (user,
// 2026-10-31: an RE6 parry's counter replaced his victim's motion and he died at once).
bool Player1InNeckBreak() { return g_neckKiller.load(std::memory_order_relaxed) != 0; }

uintptr_t WeskerThrownUnit() {
    const uintptr_t thrown = g_thrownUnit.load(std::memory_order_relaxed);
    if (thrown) return thrown;
    if (const uintptr_t jill = g_jillVictim.load(std::memory_order_relaxed)) return jill;
    return g_counterTarget.load(std::memory_order_relaxed);
}

// parry.cpp: the one enemy Jill's throw throws (a big Majini blown off by nothing else).
uintptr_t JillThrownUnit() { return g_thrownUnit.load(std::memory_order_relaxed); }

// forced_death.cpp: Wesker left lying in a player's synchronised kill (the victim's event, no death of his): his getup
// from the belly through the driven puppet (DmgS 0x59 carrying em80 0x0E), then his AI (the final one: on his knees).
bool WeskerForcedGetup(uintptr_t wesker) {
    const PuppetClass* const puppetClass = PuppetClassOf(wesker);
    if (!puppetClass || !puppetClass->driven || g_puppet.load() || !EnsureEnemyActionHook()) return false;
    kPuppetMotions[2] = puppetClass->fallenGetup ? puppetClass->fallenGetup : puppetClass->ownGetup;
    g_puppetThen = puppetClass->afterFallen;
    g_puppetClass = puppetClass;
    const DWORD now = GetTickCount();
    g_puppetSince = now ? now : 1;
    g_puppetMotion = kPuppetMotions[2];
    g_puppetPhase = 3;
    g_puppet = wesker;
    g_puppetOwn = true;
    const bool accepted = RequestEnemy(wesker, puppetClass->dmg[1]);
    g_puppetOwn = false;
    if (!accepted) {
        g_puppetMotion = 0;
        g_puppet = 0;
        g_puppetPhase = 0;
        g_puppetThen = -1;
    }
    LOG("wesker moves: %s %08X forced getup %X (Dmg %X %s)", puppetClass->name, static_cast<unsigned>(wesker),
        kPuppetMotions[2], puppetClass->dmg[1], accepted ? "accepted" : "refused");
    return accepted;
}

// parry.cpp, "Riposte de la parade": an enemy Jill's throw can take (a standing Majini, its parasite in or out, an
// Executioner, a chainsaw Majini, Jill), and the throw on it at the next frame.
bool WeskerCanThrow(uintptr_t enemy) {
    int16_t health = 0;
    if (!enemy || !mem::Read(enemy + 0x1364, health) || health <= 0 || CannotBeThrown(enemy) || IsParasiteOut(enemy))
        return false;
    return IsMajiniEnemy(enemy) || PuppetClassOf(enemy) || IsJillUnit(enemy);
}

// "Ground de RE6 avec une arme à deux mains ou le couteau" (user, 2026-11-09: "an option that replaces everyone's Ground when they hold a
// two-handed weapon", "the same rule for the knife"): action_log.cpp's player request hook, player 1's Ground 0x38 on his melee target
// (+0x2DA4) becomes RE6's with a two-handed weapon (n° 13) or the knife (n° 14) in hand; refused there, played the next frame.
std::atomic<bool> g_re6GroundWeapons{ false };
uint16_t PlayerHeldItem(uintptr_t player);  // knife.cpp
bool IsKnifeItem(uint16_t id);              // inventory.cpp
void RequestCombatMove(int k, uintptr_t target);  // below

// The throat cut in place of a Leg Back with the knife in hand, whatever option (user, 2026-11-10: "the throat cut replaces the
// character's Leg Back if he has the knife in hand"; then "it did not work" - his Leg Back slot was swapped to Chris's RE6 one, and the
// rule sat under the RE6 Ground's option, off): the game's Leg Back here and a swapped Leg Back slot (swaps.cpp MeleeSwapThrow).
int KnifeLegBackMove(uintptr_t p) {
    return p && IsKnifeItem(PlayerHeldItem(p)) ? CombatIndex("melee.free_re6_common_throat_cut") : -1;
}

// RE6's Ground by the weapon in hand (the option on): two-handed n° 13, the knife n° 14, else -1. Also for a swapped Ground slot (user,
// 2026-11-10: "the knife Ground does not work either: I equipped a knife and it played Jake's Ground" - his Ground slot swapped).
// The knife's whatever option, as the knife counter and the throat cut (user, 2026-11-10: "the three must work like that: the knife
// counter, the knife Ground and the knife Leg Back"); the two-handed weapon's under the option.
int WeaponGroundMove(uintptr_t p) {
    if (!p) return -1;
    if (IsKnifeItem(PlayerHeldItem(p))) return kRe6GroundKnifeMove;
    return g_re6GroundWeapons.load(std::memory_order_relaxed) && Re6WeaponTwoHanded(p) ? kRe6GroundLongMove : -1;
}

bool Re6GroundRequest(uintptr_t p, int action) {
    if ((action != 0x38 && action != 0x39) || !p || p != actors::Player(0)) return false;
    uint32_t target = 0;
    if (!mem::Read(p + 0x2DA4, target) || !target) return false;
    if (action == 0x39) {
        if (KnifeLegBackMove(p) < 0) return false;
        const int cut = CombatIndex("melee.free_re6_common_throat_cut");
        LOG("wesker moves: Leg Back 0x39 -> %s on %08X", kCombat[cut].name, static_cast<unsigned>(target));
        RequestCombatMove(cut, target);
        return true;
    }
    const int k = WeaponGroundMove(p);  // the knife's always, the two-handed weapon's under the option
    if (k < 0) return false;
    LOG("wesker moves: Ground 0x38 -> %s on %08X", kCombat[k].name, static_cast<unsigned>(target));
    RequestCombatMove(k, target);
    return true;
}

// combo.cpp (front escape follow-up) and swaps.cpp (melee swaps): free melee move `k` (kCombat order: 0-2 Wesker's kicks,
// 3 his sweep, 4 his kick series, 5 Jill's throw, 6-11 his dodges) on a target, the next frame.
void RequestCombatMove(int k, uintptr_t target) {
    if (k < 0 || k >= static_cast<int>(std::size(kCombat))) return;
    const DWORD now = GetTickCount();
    g_moveRequestTick = now ? now : 1;
    g_moveRequestTarget = target;
    g_moveRequest = k;
}

// parry.cpp, "Projection de Josh": his counter on the parry's attacker at the next render frame (as the throw's
// riposte); false when his list cannot be loaded (the knife slash then).
bool PlayJoshCounter(uintptr_t target) {
    if (!target || !JoshActionList()) return false;
    RequestCombatMove(kJoshCounterMove, target);
    LOG("wesker moves: Josh's counter asked on %08X", static_cast<unsigned>(target));
    return true;
}

void RequestThrowRiposte(uintptr_t enemy) {
    const DWORD now = GetTickCount();
    g_throwRiposteTick = now ? now : 1;
    g_throwRiposte = enemy;
}

// swaps.cpp, a melee slot swapped to Jill's throw: its prompt's enemy (a Majini kneeling on a Leg prompt too), or the
// nearest one.
void RequestThrowFromMelee(uintptr_t target) {
    // "Utiliser les back-up" (melee_tuning.cpp): a melee swap's throw on an enemy that refuses it plays its back-up.
    if (const std::string backup = BackupFor(kCombat[kThrowMove].id, target ? target : NearestTarget()); !backup.empty()) {
        QueueBackup(backup);
        return;
    }
    uint32_t action = 0;
    const bool kneeling = target && IsMajiniEnemy(target) && mem::Read(target + 0x10E0, action) && action == 0x71;
    int16_t health = 0;
    if (target && (WeskerCanThrow(target) || (kneeling && mem::Read(target + 0x1364, health) && health > 0))) {
        RequestThrowRiposte(target);
        return;
    }
    const DWORD now = GetTickCount();
    g_throwRiposteTick = now ? now : 1;
    g_throwNearest = true;
}

// The parries' Wesker moves (guard) ready before a parry asks for one from the game thread.
bool PrepareWeskerMoves() { return EnsureSetMotionHook() && EnsureLists(); }

// melee_tuning.cpp: a game move's hit given the table's reaction, after the game's own (which has its death) or in
// its place (`ownDeath`: a Majini left at 0 health dies here).
bool ReactToMeleeHit(uintptr_t p, uintptr_t enemy, int reaction, bool ownDeath) {
    return ReactInPlace(p, enemy, reaction, ownDeath);
}

// melee_tuning.cpp: the table's one-hit kill leaves Wesker and Jill 1 health, as the neck break.
bool KillLeavesOne(uintptr_t enemy) {
    return IsJillUnit(enemy) || IsClass(enemy, kWeskerVtable) || IsClass(enemy, kFinalWeskerVtable) ||
           IsClass(enemy, kLinWeskerVtable);
}

// melee_tuning.cpp: -1 not a move of this file, 0 a move without a hit (a dodge, a pose, a test), 2 a throw (its reaction
// is the throw), 1 an attack.
// melee_tuning.cpp: a trainer move's own damage (before the table's multiplier), -1 if not one of them.
int CombatMoveDamage(const std::string& featureId) {
    if (featureId == kCounterIds[0] || featureId == kCounterIds[1]) return Re6CounterDamage();
    for (const CombatMove& m : kCombat)
        if (featureId == m.id) return m.damage;
    return -1;
}

// camera.cpp: the trainer move player 1 plays now (its feature id), nullptr if none (render thread's state, read once).
const char* ActiveCombatMoveId() {
    const CombatMove* const move = g_active.move;
    return move ? move->id : nullptr;
}

int CombatMoveKind(const std::string& featureId) {
    if (featureId == kCounterIds[0] || featureId == kCounterIds[1]) return 1;
    for (int k = 0; k < static_cast<int>(std::size(kCombat)); ++k) {
        if (featureId != kCombat[k].id) continue;
        // A pair too (the RE4 ones, Josh's counter): its synchronised side is its reaction.
        if (ThrowMove(k) || k == kThrow2Move || kCombat[k].reaction == Reaction::Thrown || kCombat[k].victim >= 0) return 2;
        const CombatMove& m = kCombat[k];
        return m.damage <= 0 || (m.from > m.to && m.from2 > m.to2) ? 0 : 1;
    }
    return -1;
}

// An attack's shortcut during a move of player 1 (user, 2026-10-30: the neck break's key pressed again during it played
// it again, in a loop until its animation ended): his melee moves (0x30..0x4D, 0x6F) and the game's dodges (0x50..0x55)
// play to their end; the trainer's carrier 0x81 and the Reunion moves 0xAC / 0xAD are refused by FreePlayer1 already
// (0x80 and above). The trainer's own chains (parries' ripostes, automatic dodges) do not go through the shortcuts.
bool Player1MovePlaying() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    return p && mem::Read(p + 0x10E0, action) &&
           ((action >= 0x30 && action <= 0x4D) || action == 0x6F || (action >= 0x50 && action <= 0x55));
}

// swaps.cpp: the dodge styles' moves, by their option id.
// swaps.cpp, the SetMotion hook (game thread): while he is down, what the game asks on these layers is RE6's (user, 2026-11-08: "images in conflict",
// the game's aim, fire and small layer 7 motions each shown for a frame until the tick put RE6's back); during RE6's dodges, nothing on layer 7.
uint32_t Re6GroundMotion(uintptr_t unit, uint32_t layer, uint32_t motion, uint32_t& start) {
    if (unit != actors::Player(0)) return motion;
    if (g_re6Ground) {
        if (layer == 7) {
            const uint32_t ours = g_re6Layer7.load(std::memory_order_relaxed);
            if (!ours) return motion;
            if (g_re6Ground == 3 && g_re6SlideFrame > 0.0f) {
                std::memcpy(&start, &g_re6SlideFrame, sizeof(start));
            } else if (g_re6Ground == 1) {  // a shot while he crawls: the crawl goes on where it was
                uint16_t current = 0;
                float frame = 0.0f;
                const uintptr_t record = unit + 0x380 + 7 * 0xE0;
                if (mem::Read(record + 4, current) && current == ours && mem::Read(record + 0x18, frame)) std::memcpy(&start, &frame, sizeof(start));
            }
            return ours;
        }
        if (layer <= 2 && g_re6Ground != 2 && !Re6UpperBodyIsGames(unit)) {
            const uint32_t ours = g_re6Upper[layer].load(std::memory_order_relaxed);
            if (ours && g_re6Ground == 3 && g_re6LayeredWhich == 4 && g_re6SlideFrame > 0.0f) std::memcpy(&start, &g_re6SlideFrame, sizeof(start));
            return ours ? ours : motion;
        }
        return motion;
    }
    if (layer == 7 && PlayerInWeskerMove(unit) && g_active.move && g_active.move->bank == kRe6EvadeBank && g_active.move->index <= 5) return 0xFFFF;
    return motion;
}

bool Re6OnGround() { return g_re6Ground != 0; }
// movement.cpp's upper overlay: a motion of re6_evade.lmt as (bank << 8) | slot in player 1's lists (0 when it cannot play).
uint32_t Re6EvadeMotion(uint16_t slot) { return MoveMotion(kRe6EvadeBank, slot); }
// A left-handed player's moves play mirrored (the game gives them mode 0x100: trainer.log 2026-11-10, "trainer move motion ... mode 144,
// hand 1"), so a side chosen from the direction is the other side's move (user: "left-handed, the getup directions are inverted: going
// left Chris gets up to the right"). His handedness byte P+0x1AF9 (1 left).
void Player1MoveDirection(float& forward, float& left) {
    Player1BodyDirection(forward, left);
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint8_t hand = 0;
    if (p && mem::Read(p + 0x1AF9, hand) && hand == 1) left = -left;
}
// quick_knife.cpp's button queries: lying or ducked, the game gets no direction (user, 2026-11-10: with an ammunition box on the ground "he
// does not really aim, so he keeps the RE6 moves: he pivots towards the direction instead of strafing"); the crawl reads them raw.
bool Re6GroundHoldsPlayer() { return g_re6Ground == 1 || g_re6Ground == 2; }

// parry.cpp: the RE6 parry's riposte `which` (0 Piers, 1 Chris, 2 Jake, 3 Leon, 4 Ada, 5 the bayonet's - Piers's without a two-handed
// weapon, user: "otherwise it needs a back-up like Piers's counter") on the attacker, the next frame.
// parry.cpp (game thread): a parry fired; the trainer's move playing is cut (user, 2026-11-09: "if the parry fires, the move playing
// is cut"): his action back to free at once (the riposte's own request comes after it), the move's state dropped on the render
// thread (TickParryCut), the riposte allowed from the carrier action meanwhile (StartCombatMove's afterCut).
void CutMoveForParry() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    const CombatMove* const move = g_active.move;
    if (!p || !move || !PlayerInWeskerMove(p)) return;
    LOG("wesker moves: %s cut by the parry", move->name);
    g_parryCutStart = g_active.start;
    const DWORD now = GetTickCount();
    g_parryCut = now ? now : 1;
    g_parryCutAsked = true;
    RequestPlayer(p, 0x00);
}

bool PlayRe6Sweep(uintptr_t target) {
    if (!target) return false;
    RequestCombatMove(kRe6Sweep, target);
    return true;
}

// player.cpp's attached-matrix hook: RE6's weapon joint of move `index` at `frame` of `motion` (that move's, played from bank 8), its
// rotation and position in the hand's frame, linear between the frames.
bool Re6WeaponJointAt(int index, uint32_t motion, float frame, float (&q)[4], float (&t)[3]) {
    if (index < 0 || (motion & 0xFF) != static_cast<uint32_t>(index) || ((motion >> 8) & 0xFF) != static_cast<uint32_t>(kJill2ActBank))
        return false;
    const WeaponJointFrames* w = WeaponJointFor(index);
    if (!w) return false;
    const int last = static_cast<int>(w->frames.size()) - 1;
    const float at = std::clamp(frame, 0.0f, static_cast<float>(last));
    const int a = static_cast<int>(at), b = std::min(a + 1, last);
    const float s = at - static_cast<float>(a);
    const auto& ka = w->frames[a];
    const auto& kb = w->frames[b];
    float n = 0.0f;
    for (int i = 0; i < 4; ++i) {
        q[i] = ka[i] + (kb[i] - ka[i]) * s;
        n += q[i] * q[i];
    }
    n = n > 0.0f ? 1.0f / std::sqrt(n) : 0.0f;
    for (float& c : q) c *= n;
    for (int i = 0; i < 3; ++i) t[i] = ka[4 + i] + (kb[4 + i] - ka[4 + i]) * s;
    return n > 0.0f;
}

// RE6's special counters (user, 2026-11-10): an RE6 parry on a Majini swinging a one-handed weapon (cEm10ActAtkOneHand /
// AtkDashOneHand: a machete, a hatchet, a sickle) plays the machete disarm, on one striking with its gun's butt (cEm10ActAtkGunBlow,
// the only melee blow of a Majini with a gun) the gun counter. "Contres spéciaux de RE6 (Majini armés)" (Parade, on by default).
std::atomic<bool> g_specialCounters{ true };

bool PlaySpecialCounter(uintptr_t player, uintptr_t target) {
    if (!player || !target || !g_specialCounters.load(std::memory_order_relaxed) || !IsMajiniEnemy(target)) return false;
    uint32_t action = 0, count = 0;
    uintptr_t table = 0, object = 0;
    if (!mem::Read(target + 0x10E0, action) || !mem::Read(target + 0x10D0, count) || action >= count || count > 0x200 ||
        !mem::Read(target + 0x10DC, table) || !table || !mem::Read(table + 4 * action, object) || !object)
        return false;
    const char* cls = UnitClassName(object);
    if (!cls) return false;
    // Its weapon (+0x1B6C, an item uWpXX on its joint 8 right hand or 13 left; read live 2026-11-10). The machete disarm only on the blades
    // the user named ("ignore the axes and the bottles, keep only the knives and the sickles"): uWp55 the knife (seen on the user's prompt
    // target), uWp51 the sickle (the user's parry of 22:06:42).
    uintptr_t weapon = 0;
    const char* held = mem::Read(target + 0x1B6C, weapon) && weapon ? UnitClassName(weapon) : nullptr;
    const bool blade = held && (std::strcmp(held, "uWp55") == 0 || std::strcmp(held, "uWp51") == 0);
    // The bottle (uWp5a: the user's stomp prompt target, in its left hand; its archive holds the broken pieces wp5a01 / 02): the gun
    // counter (user, 2026-11-10: "for the bottle enemies, play the gun counter; the mirror if it holds the bottle in its left hand").
    const bool bottle = held && std::strcmp(held, "uWp5a") == 0;
    const bool oneHand = std::strcmp(cls, "cEm10ActAtkOneHand") == 0 || std::strcmp(cls, "cEm10ActAtkDashOneHand") == 0;
    // The axe (uWp50, and uWp69 its model), the club (uWp72) and the wrench (uWp73), read live on the user's Majini (2026-11-10: "the enemy
    // in front of me has an axe", "club just in front", "wrench next to me"): RE6's two counters against the armed zombies, one or the
    // other at random, on any swing with them (a one- or two-handed blow).
    const bool club = held && (std::strcmp(held, "uWp50") == 0 || std::strcmp(held, "uWp69") == 0 || std::strcmp(held, "uWp72") == 0 ||
                               std::strcmp(held, "uWp73") == 0);
    const bool swing = std::strncmp(cls, "cEm10ActAtk", 11) == 0 && std::strstr(cls, "Hand") != nullptr;
    const char* id = oneHand && blade                                           ? "melee.free_re6_common_machete_counter"
                     : std::strcmp(cls, "cEm10ActAtkGunBlow") == 0 || (oneHand && bottle) ? "melee.free_re6_common_gun_counter"
                     : swing && club ? (GetTickCount() / 7 % 2 ? "melee.free_re6_common_club_counter_2" : "melee.free_re6_common_club_counter_1")
                                     : nullptr;
    LOG("wesker moves: %08X attacks with %s holding %s: %s", static_cast<unsigned>(target), cls, held ? held : "nothing",
        id ? id : "no special counter");
    if (!id) return false;
    RequestCombatMove(CombatIndex(id), target);
    return true;
}

bool TrainerMoveHidesWeapon() { return g_moveHidesWeapon.load(std::memory_order_relaxed); }

// swaps.cpp's "Vol d'arme (RE6) sur les ennemis armés": a Majini holding a weapon to strike with (the blades and blunt ones its archives
// mark wp_blds / wp_hand, the bottle included; not its guns, dynamite, fire or electric weapons, nor the big ones of the larger enemies).
bool MajiniHoldsStealable(uintptr_t target) {
    uintptr_t weapon = 0;
    const char* held = target && IsMajiniEnemy(target) && mem::Read(target + 0x1B6C, weapon) && weapon ? UnitClassName(weapon) : nullptr;
    if (!held) return false;
    for (const char* name : { "uWp50", "uWp51", "uWp52", "uWp53", "uWp55", "uWp57", "uWp59", "uWp5a", "uWp5f", "uWp62", "uWp69", "uWp6a",
                              "uWp6e", "uWp6f", "uWp71", "uWp72", "uWp73" })
        if (_stricmp(held, name) == 0) return true;
    return false;
}

bool PlayWeaponSteal(uintptr_t target) {
    if (!target) return false;
    RequestCombatMove(CombatIndex("melee.free_re6_common_weapon_steal"), target);
    return true;
}

bool PlayKnifeCounter(uintptr_t player, uintptr_t target) {
    if (!player || !target || !IsKnifeItem(PlayerHeldItem(player))) return false;
    RequestCombatMove(CombatIndex("melee.free_re6_common_knife_counter"), target);
    return true;
}

bool PlayRe6Counter(int which, uintptr_t target) {
    if (which < 0 || which > 5 || !target) return false;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    // The knife in hand (user, 2026-11-10: "the knife counter replaces the RE6 counter if Chris has the knife in hand"; the knife aim lets
    // the RE6 parry through for it, parry.cpp TickRe6Input).
    if (p && IsKnifeItem(PlayerHeldItem(p))) {
        RequestCombatMove(CombatIndex("melee.free_re6_common_knife_counter"), target);
        return true;
    }
    if (which == 5 && !(p && Re6WeaponTwoHanded(p))) which = 0;
    RequestCombatMove(kRe6Counters[which], target);
    return true;
}

// quick_knife.cpp: the RE6 melee takes the attack key's press (an enemy near or a crate in front, the option on for the played
// character): no quick shot, no shooting without aiming, no quick knife on it then.
bool Re6MeleeTakesFire() { return Re6MeleeWouldTake(actors::InGame() ? actors::Player(0) : 0); }

// swaps.cpp's per-character choices (kind 0 combo, 1 sprint attack, 2 elbow) and the reactions (kind 3: index 0-3).
void SetRe6MeleeChoice(int kind, int index, int value) {
    if (index < 0 || index >= 8) return;
    if (kind == 0) g_re6ComboOf[index] = value;
    else if (kind == 1) g_re6SprintOf[index] = value;
    else if (kind == 2) g_re6ElbowOf[index] = value;
    else if (kind == 3 && index < 4) g_re6MeleeState[index] = value;
    else if (kind == 4) g_re6SprintRandom[index] = value != 0;
    else if (kind == 5) g_re6ElbowRandom[index] = value != 0;
    else if (kind == 6) g_re6FinisherOf[index] = value;
}

// swaps.cpp: RE6's slide, the aim pressed during the sprint (see TickRe6Ground).
bool StartRe6Layered(int which, float startFrame, bool fromMove) {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p || g_re6Ground || (!fromMove && PlayerInWeskerMove(p))) return false;
    g_re6LayeredWhich = which;
    g_re6Ground = 3;
    g_re6SlideShot = false;  // RE6's slide pose until a shot
    g_re6GroundFrom = static_cast<uint16_t>(which);
    g_re6GroundSince = GetTickCount64();
    g_re6SlideFrame = startFrame;
    // The aim at once: from the sprint the game could take over a second (user: "one time in two the arms are aside until he is settled").
    uint32_t action = 0;
    if (mem::Read(p + 0x10E0, action) && !(action >= 0x1B && action <= 0x1E)) RequestPlayer(p, 0x1B);
    return true;
}

// An item the game does not aim (an ammunition box, herbs, a spray: user, 2026-11-09, "the slide bugs with an ammunition box in hand"): the
// aim never starts, so the slide under the aim stopped after 400 ms while the game ran on; the whole-body slide (its own pose) instead,
// down at its end with the aim held (Re6EvadeEnded).
bool StartRe6AimSlide() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (p && Re6HeldPoseSlot(p) == 30) {
        const bool started = StartCombatMove(CombatIndex("melee.free_re6_slide"), 0, true);
        LOG("re6 slide: an item in hand, the whole-body slide %s", started ? "started" : "failed");
        return started;
    }
    return StartRe6Layered(4, 0.0f, false);
}

bool StartDodgeMove(const char* id) {
    for (int k = 0; k < static_cast<int>(std::size(kCombat)); ++k)
        if (std::strcmp(kCombat[k].id, id) == 0) return StartCombatMove(k);
    LOG("wesker moves: no move %s", id);
    return false;
}

// swaps.cpp's dodge while aiming from the frozen duck (user, 2026-11-09: "from the frozen pose, the dives on the other key, since I am
// aiming"): the duck left for the dodge (nullptr: only left, for the game's own dodge); the dodge in place goes on from the duck's frame,
// its rest standing him up.
bool Re6Ducked() { return g_re6Ground == 2; }

// swaps.cpp: "Glissade (RE6) : réaction d'un Majini normal / déjà sonné".
void SetSlideState(bool staggered, int state) { g_slideState[staggered ? 1 : 0] = state; }

bool StartDodgeFromDuck(const char* id) {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p || g_re6Ground != 2) return false;
    g_re6Ground = 0;
    Re6GroundLegsOff(p);
    if (!id) return true;
    if (std::strcmp(id, "melee.free_re6_dodge_duck") == 0) g_moveBeginOverride = 16.0f;  // re6_evade.lmt 46 = BhB 38's frame 16
    const bool started = StartDodgeMove(id);
    g_moveBeginOverride = -1.0f;
    LOG("re6 crouch: %s from the duck %s", id, started ? "started" : "failed");
    return started;
}

// parry.cpp: Jill knocked down by a parry's reaction (user, 2026-10-26: "Au sol" teleported her standing).
void MarkJillFallen(uintptr_t jill) {
    const DWORD tick = GetTickCount();
    g_jillKnockedTick = tick ? tick : 1;
    g_jillKnocked = jill;
}

// action_log.cpp's enemy request hook: the Executioner in Jill's throw (false = refused, `action` may be replaced).
// Jill out of a fall of the trainer's (a move, a parry's reaction): her AI's request (not a damage, a death, her getup
// or a hold) becomes her getup. Her Sleep put there instead with her device under the threshold did not hold (2026-10-26,
// logged: Sleep, then 9 ms later her getup again): her AI's request RVA 0x5D0D90 (vfunc +0x48 of her AI at Jill+0x1B68,
// vtable RVA 0xB98680) sets its own timer for the action it asks (+0x30), so it still waited for its getup. Only her
// native throw keeps her down (the parries throw her, parry.cpp).
void JillGetupRequest(uintptr_t enemy, int* action) {
    if (!enemy || enemy != g_jillKnocked.load(std::memory_order_relaxed)) return;
    if (static_cast<int32_t>(GetTickCount() - g_jillKnockedTick.load()) > 8000) {
        g_jillKnocked = 0;
        return;
    }
    uint32_t current = 0;
    if (!mem::Read(enemy + 0x10E0, current)) return;
    const char* now = ActionTail(enemy, current);
    const char* asked = ActionTail(enemy, static_cast<uint32_t>(*action));
    if (std::strstr(now, "Getup") || std::strstr(now, "Sleep") || std::strstr(asked, "Getup") || std::strstr(asked, "Die") ||
        std::strstr(asked, "Caught") || std::strstr(asked, "Sleep")) {
        g_jillKnocked = 0;
        return;
    }
    const bool fallen = std::strstr(now, "KnockDown") || std::strstr(now, "DmgBlow");
    if (!fallen || std::strstr(asked, "Dmg")) return;
    const int getup = JillAction(enemy, "DmgGetup");
    if (getup < 0) return;
    LOG("wesker moves: Jill %08X after %s: her AI's %s (%X) -> her getup %X", static_cast<unsigned>(enemy), now, asked, *action,
        getup);
    *action = getup;
    g_jillKnocked = 0;
}

bool WeskerPuppetRequest(uintptr_t enemy, int* action) {
    // A straight blow-off's landing (user, 2026-11-10: "it lands on its back but gets up on its belly"): the game picks its lying and dead
    // states for the flight it chose, not the one played (0x52A / 0x52B): they follow the body as it lies (its hips under 60 cm, the
    // pelvis's front up or down).
    if (enemy && enemy == g_straightBlowUnit.load(std::memory_order_relaxed) &&
        static_cast<int32_t>(GetTickCount() - g_straightBlowTick.load()) < 8000 &&
        (*action == 0x6A || *action == 0x6B || *action == 0x8D || *action == 0x8E)) {
        float hip = 0.0f, facing = 0.0f;
        LyingOnBack(enemy, &hip, &facing);
        if (hip > 0.0f && hip < 60.0f && facing != 0.0f) {
            const bool down = facing < 0.0f;
            const int want = *action == 0x6A || *action == 0x6B ? (down ? 0x6B : 0x6A) : (down ? 0x8E : 0x8D);
            LOG("wesker moves: %08X lands from its straight blow-off on its %s (hips %.0f cm): %X%s", static_cast<unsigned>(enemy),
                down ? "belly" : "back", hip, want, want != *action ? " in place of the game's" : "");
            *action = want;
        }
    }
    // A pair's Majini plays its side to its end (user, 2026-11-10, Jake's Ground and Sherry's Head Flash: a survivor "teleports standing";
    // logged: its AI's AtkInst 0x16 a tenth of a second after the blow took it out of BackHoldDmg): its requests refused meanwhile but
    // its lying and dead ones (RiseFaceDown / Up 0x6B / 0x6A, Dead 0x6C, DeadFaceUp / Down 0x8D / 0x8E) and the pair's BackHoldDmg.
    if (enemy && enemy == g_pairLock.unit && IsMajiniEnemy(enemy) && *action != 0x9C && *action != 0x6A && *action != 0x6B &&
        *action != 0x6C && *action != 0x8D && *action != 0x8E) {
        static uintptr_t told = 0;
        if (told != enemy) {
            told = enemy;
            LOG("wesker moves: %08X asked %X during its pair's side: refused", static_cast<unsigned>(enemy), *action);
        }
        return false;
    }
    if (enemy && enemy == g_jill2Survivor.load(std::memory_order_relaxed)) {
        if (static_cast<int32_t>(GetTickCount() - g_jill2SurvivorSince.load()) > 5000) {
            g_jill2Survivor = 0;
        } else if (*action == 0x6B) {
            const int wait = PlayerActionNamed(enemy, "cEm10ActWait");
            g_jill2Survivor = 0;
            if (wait >= 0) {
                LOG("wesker moves: the RiseFaceDown after Jill's second throw (up already) -> its Wait %X", wait);
                *action = wait;
            }
        }
    }
    if (enemy && enemy == g_neckMajini.load(std::memory_order_relaxed)) {
        if (static_cast<int32_t>(GetTickCount() - g_neckMajiniSince.load()) > 10000) {
            g_neckMajini = 0;
        } else if (*action == 0x6B) {
            const int wait = PlayerActionNamed(enemy, "cEm10ActWait");
            g_neckMajini = 0;
            if (wait >= 0) {
                LOG("back grab: the Majini's RiseFaceDown after the neck break -> its Wait %X", wait);
                *action = wait;
            }
        }
    }
    JillGetupRequest(enemy, action);
    return PuppetRequest(enemy, action);
}

// The guard riposte of the parry (parry.cpp, "Garde de Wesker"): player 1 invulnerable the whole guard (user,
// 2026-10-23: another enemy hit him while he was still in it), from the parry until his guard motion em80 0x74 ends.
std::atomic<DWORD> g_guardTick{ 0 };

bool GuardRiposteActive(uintptr_t player) {
    const DWORD since = g_guardTick.load(std::memory_order_relaxed);
    if (!since) return false;
    if (static_cast<int32_t>(GetTickCount() - since) < 600) return true;  // its motion not started yet
    uint32_t motion = 0;
    if (PlayerInWeskerMove(player) && mem::Read(player + 0x384, motion) && (motion & 0xFFFF) == g_riposteMotion.load())
        return true;
    g_guardTick = 0;
    return false;
}

void ForceNextEnemyMotion(uintptr_t enemy, uint32_t motion) {
    EnsureSetMotionHook();
    const DWORD now = GetTickCount();
    g_forcedMotion = motion;
    g_forcedMotionTick = now ? now : 1;
    g_forcedMotionUnit = enemy;
}

// movement.cpp ("Marcher pendant les actions"): player 1 walks during the parry's guard.
bool WeskerGuardPlaying() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    return p && g_riposteMotion.load() == ((static_cast<uint32_t>(kWeskerBank) << 8) | 0x74) && PlayerInWeskerMove(p) &&
           GuardRiposteActive(p);
}

// movement.cpp: the leg walk's layer speed during the guard, against its time scale (user: the guard faster, not the
// walk).
float WeskerGuardLegSpeed() {
    const float factor = g_guardSpeed.load(std::memory_order_relaxed);
    return g_guardSped && factor > 0.01f ? 1.0f / factor : 1.0f;
}

bool PlayRiposte(int named, uint32_t motion) {
    EnsureGrabRefusalHook();
    const DWORD now = GetTickCount();
    g_riposteMotion = motion;
    g_guardTick = now ? now : 1;
    if (PlayNamedMove(named)) return true;
    g_guardTick = 0;
    return false;
}

bool PlayGuardRiposte() {
    g_counterTarget = 0;
    return PlayRiposte(0, (static_cast<uint32_t>(kWeskerBank) << 8) | 0x74);
}

// forced_death.cpp: the enemy of Jill's throw it has no throw for, 0 once 5 s old.
uintptr_t WeskerThrowOther() {
    const DWORD tick = g_throwOtherTick.load(std::memory_order_relaxed);
    return tick && static_cast<int32_t>(GetTickCount() - tick) < 5000 ? g_throwOther.load(std::memory_order_relaxed) : 0;
}

// rocket_catch.cpp: Wesker's rocket catch on player 1, invulnerable through it as through the guard.
bool PlayRocketCatch() {
    g_counterTarget = 0;
    return PlayRiposte(9, (static_cast<uint32_t>(kWeskerBank) << 8) | 0xD7);
}

// rocket_catch.cpp: the blast of a rocket sent back, as strong as player 1's own (user, 2026-10-25: the enemy rocket's
// blast hurts the Majini "far less than a rocket of Chris", its shooter not at all): every enemy whose body is within
// `radius` takes `damage` and is blown off, the kill credited to player 1 (Strike, as Wesker's blows).
bool RequestOwnDeath(uintptr_t unit);  // forced_death.cpp: its own death action

int RocketBlast(const float* at, float radius, int damage) {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p) return 0;
    const CombatMove blast{ "rocket_blast", "roquette renvoyée", 0, 0, damage, Reaction::BlowOff, 1.0f, 0.0f };
    int hit = 0;
    for (uintptr_t e : actors::Enemies()) {
        float pos[3]{};
        if (!mem::SafeRead(reinterpret_cast<void*>(e + 0x30), pos, sizeof(pos))) continue;
        const float height = std::clamp(at[1] - pos[1], 50.0f, 200.0f);  // the nearest point of its body
        const float dx = at[0] - pos[0], dy = at[1] - (pos[1] + height), dz = at[2] - pos[2];
        if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
        // An enemy it has no reaction for (the RPG Majini, logged 2026-10-25: "no reaction", left at 0 health) plays its
        // own death.
        int16_t health = 0;
        if (!Strike(p, e, blast) && mem::Read(e + 0x1364, health) && health <= 0) RequestOwnDeath(e);
        ++hit;
    }
    return hit;
}

// `variant`: 0 Chris's counter, 1 Sheva's (kCounters).
bool PlayCounterRiposte(uintptr_t target, int variant) {
    if (target) EnsurePlayerContactHook();
    g_counterTarget = target;
    g_counterPunchDue = target != 0;
    g_counterSwung = false;
    g_counterVariant = variant == 1 ? 1 : 0;
    return PlayRiposte(variant == 1 ? 10 : 1, CounterMotion(variant));
}

// "Chris Contre RE6" / "Sheva Contre RE6" (user, 2026-11-01: "the counters the RE6 parries use in the free melee, tunable
// like Josh's counter: power, critical..."): the counter played on the nearest enemy (3 m), turned to it, without a
// parry; its blow is the parry's (parry.cpp CounterPunchLands), tuned by the line of the counter playing.
const char* CounterTuningId() { return kCounterIds[g_counterVariant.load(std::memory_order_relaxed) == 1 ? 1 : 0]; }

bool PlayFreeCounter(int variant) {
    if (!StaminaAllows()) return false;
    const uintptr_t p = FreePlayer1();
    if (!p || !PrepareWeskerMoves()) return false;
    float at[3]{};
    const uintptr_t target = NearestEnemy(p, 300.0f, at);
    if (target) FacePlayerTowards(p, at);
    if (!PlayCounterRiposte(target, variant)) return false;
    StaminaSpendRe6Counter();
    LOG("wesker moves: %s's counter (free melee) on %08X", variant == 1 ? "Sheva" : "Chris", static_cast<unsigned>(target));
    return true;
}

// parry.cpp: the table's reaction for the counter's blow, as Strike gives it to the trainer's moves.
bool TrainerMoveReact(uintptr_t p, uintptr_t enemy, int reaction) { return ReactInPlace(p, enemy, reaction, true); }

// player.cpp (hit reaction, SubHealth), quick_knife.cpp (blood) and grabs.cpp: player 1 is in one of these attacks or
// in the parry's guard.
bool WeskerMoveInvulnerable(uintptr_t player) {
    if (!player || player != actors::Player(0)) return false;
    return g_invulnerable.load(std::memory_order_relaxed) || GuardRiposteActive(player) ||
           g_neckKiller.load(std::memory_order_relaxed);  // the back grab's neck break (no other grab, no hit)
}

// player.cpp's voice hook: no voice line from either player while a borrowed boss move starts (g_bossMoveStart).
bool BossMoveSilencesVoices() {
    const DWORD start = g_bossMoveStart.load(std::memory_order_relaxed);
    return start && static_cast<int32_t>(GetTickCount() - start) < 2500;
}

namespace {
// Chris shaking his right hand after a punch (user, 2026-10-24: after the RE6 parry's counter and his punch 0x37, "as if
// he had hurt himself"): the move ends before it, player 1 asked back to idle (RequestAction 0x00) at the frame the
// shake starts. Measured on the motions (bones 6 / 8, the right forearm and hand, fast and reversing while the body is
// still; scratchpad shake.py): pl00action 0x0C (action 0x37, 131 frames, the punch at frame 28) shakes from frame ~82,
// em80_pl 0x01 (the counter, 176 frames, the punch at 66) from ~108. Arm Front 0x10A and the finisher 0x10F do not.
struct HandShakeCut {
    uint32_t motion;
    float frame;
    bool chrisOnly;  // a motion of Chris's own list (another character's 0x10C is another move)
};
constexpr HandShakeCut kHandShakeCuts[] = {
    { 0x10C, 80.0f, true },
    { (static_cast<uint32_t>(kWeskerPlayerBank) << 8) | 0x01, 106.0f, false },
};
uint32_t g_shakeCut = 0;  // render thread: the motion already cut, asked once

bool MeleeOrCarrier(uint32_t action) {
    return (action >= 0x30 && action <= 0x4D) || action == 0x6F || action == 0xAC || action == 0xAD || action == kCarrier;
}

void TickHandShakeCut() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t motion = 0, action = 0;
    float frame = 0;
    if (!p || !mem::Read(p + 0x384, motion) || !mem::Read(p + 0x10E0, action) || !mem::Read(p + 0x398, frame) ||
        !MeleeOrCarrier(action)) {
        g_shakeCut = 0;
        return;
    }
    motion &= 0xFFFF;
    const HandShakeCut* cut = nullptr;
    for (const HandShakeCut& c : kHandShakeCuts)
        if (c.motion == motion) cut = &c;
    if (!cut || frame < cut->frame) {
        g_shakeCut = 0;
        return;
    }
    if (g_shakeCut == motion) return;
    if (cut->chrisOnly) {
        uintptr_t profile = 0;
        uint32_t character = 0xFF;
        if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), profile) || !profile ||
            !mem::Read(profile + 0x23318, character) || character != 0)
            return;
    }
    g_shakeCut = motion;
    const bool asked = RequestPlayer(p, 0x00);
    LOG("hand shake cut: motion %X (action %X) at frame %.0f, idle %s", motion, action, frame, asked ? "asked" : "failed");
}
}  // namespace

// time_speed.cpp ("Vitesse du corps à corps"): player 1 in a melee attack: the game's melee moves (0x30-0x4D, 0x6F, the
// Reunion 0xAC / 0xAD), the trainer's strikes and throw (the carrier 0x81 with a damaging move), the RE6 parry's
// counter and the grab escapes (user, 2026-10-24: "the escapes too"; in the event action 0x80000000, motion bank F =
// the grabber's list: Majini back / front 0xF68 / 0xF6F, female 0xFA4 / 0xFAB, MODLOG grabs; the licker's throw-off
// 0xF2F, licker.cpp) and the parries' ripostes (user, 2026-10-24: the knife slash 0x3E, Wesker's guard and the RE6
// counter, GuardRiposteActive; Jill's throw is a carrier move); not the dodges.
// stamina.cpp: a dodge of the trainer's playing (its getups and the duck's pieces too), RE6's slide or a dive under the aim (the refill held).
bool Player1InTrainerDodge() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p) return false;
    if (g_re6Ground == 3) return true;
    return mem::Read(p + 0x10E0, action) && action == kCarrier && g_active.move && g_active.move->damage == 0;
}

bool Player1InMeleeAttack() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action)) return false;
    if ((action >= 0x30 && action <= 0x4D) || action == 0x6F || action == 0xAC || action == 0xAD) return true;
    if (action == 0x80000000) {
        uint32_t motion = 0;
        if (!mem::Read(p + 0x384, motion)) return false;
        motion &= 0xFFFF;
        return motion == 0xF68 || motion == 0xF6F || motion == 0xFA4 || motion == 0xFAB || motion == 0xF2F;
    }
    if (action != kCarrier) return false;
    if ((g_active.move && g_active.move->damage > 0) || GuardRiposteActive(p)) return true;
    uint32_t motion = 0;
    return mem::Read(p + 0x384, motion) && ((motion & 0xFFFF) == CounterMotion(0) || (motion & 0xFFFF) == CounterMotion(1));
}

// Jill's throw (a carrier move): the Majini or Jill in it, sped up with player 1 ("Vitesse du corps à corps"); the
// game's synchronised moves are found by their start (time_speed.cpp). 0 otherwise.
uintptr_t Player1SyncedMeleeTarget() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action)) return 0;
    if (action == kCarrier && g_active.move && g_active.move->reaction == Reaction::Thrown)
        return g_thrownUnit.load() ? g_thrownUnit.load() : g_caught;
    return 0;
}

// A duo move's sub-tab: its character's (melee.free_<who>_tag_...).
const char* TagCategory(const char* id) {
    static constexpr struct { const char* key; const char* category; } kTabs[] = {
        { "melee.free_chris_", "Mêlée libre Chris" },     { "melee.free_wesker_", "Mêlée libre Wesker" },
        { "melee.free_sheva_", "Mêlée libre Sheva" },     { "melee.free_josh_", "Mêlée libre Josh" },
        { "melee.free_excella_", "Mêlée libre Excella" }, { "melee.free_barry_", "Mêlée libre Barry" },
        { "melee.free_rebecca_", "Mêlée libre Rebecca" },
    };
    for (const auto& tab : kTabs)
        if (std::strncmp(id, tab.key, std::strlen(tab.key)) == 0) return tab.category;
    return "Mêlée libre";
}

// "Test : corps à corps de RE6": its line of DataDir\motions\re6_melee.txt (index, source list and slot, frames, side, forward,
// attack@frame).
const std::vector<std::string>& Re6MeleeRows() {
    static std::vector<std::string> rows;
    if (rows.empty()) {
        std::ifstream f(logx::DataDir() + L"\\motions\\re6_melee.txt");
        std::string line;
        std::getline(f, line);  // its header
        while (std::getline(f, line)) rows.push_back(line);
    }
    return rows;
}

std::string Re6MeleeLabel(int index) {
    const std::vector<std::string>& rows = Re6MeleeRows();
    if (index < 0 || index >= static_cast<int>(rows.size())) return "?";
    std::string cells[6];
    size_t from = 0;
    for (int c = 0; c < 6; ++c) {
        const size_t tab = rows[index].find('\t', from);
        cells[c] = rows[index].substr(from, tab == std::string::npos ? std::string::npos : tab - from);
        if (tab == std::string::npos) break;
        from = tab + 1;
    }
    return cells[1] + ", " + cells[2] + " images, avance " + cells[4] + " cm, attaque " + cells[5];
}

void RegisterWeskerMoves() {
    Add({ .id = "test.env_scan", .name = "Tester la détection du décor (exécutions RE6)",
          .help = "Écrit dans trainer.log les murs et obstacles bas trouvés autour du joueur 1 (3 m), avec la hauteur de leur dessus.",
          .category = "Diagnostic", .kind = Kind::Action,
          .run = [] {
              if (!EnsurePadQueryHooks()) return false;  // the scan runs on the game thread
              g_envScanAsked = true;
              return true;
          },
          .available = [] { return actors::InGame(); } });
    constexpr int kRe6TestMove = CombatIndex("melee.re6_melee_test_move");
    static_assert(kRe6TestMove >= 0);
    for (int k = 0; k < static_cast<int>(std::size(kCombat)); ++k) {
        if (k == kThrow2Catch || k == kRe6TestMove || k == kRe6JakeGroundBelly) continue;  // started by the leap / the test / his Ground
        // Its sub-tab's "Dodge" (below): free while a move plays; the attacks wait for its end.
        const bool dodge = IsDodgeMove(k);
        const bool costed = CostedDodge(k);  // its stamina slider ("Endurance stricte")
        Add({ .id = kCombat[k].id, .name = kCombat[k].name,
              .help = kCombat[k].victim >= 0
                          ? "Chris joue ce mouvement de Resident Evil 4 sur l'ennemi le plus proche (3 m), tourné vers lui. "
                            "Un Majini joue l'animation du Ganado de Resident Evil 4, synchronisée avec Chris, et prend les "
                            "dégâts du coup ; un autre ennemi est seulement touché par les membres de Chris. Chris est "
                            "invincible pendant tout le mouvement. Les animations viennent de tes jeux (fichiers motions du "
                            "dossier du trainer)."
                          : std::strncmp(kCombat[k].id, "melee.free_leon", 15) == 0 ||
                              std::strncmp(kCombat[k].id, "melee.free_hunk", 15) == 0 ||
                              std::strncmp(kCombat[k].id, "melee.free_wesker4", 18) == 0
                          ? (kCombat[k].damage == 0
                                 ? "Chris joue cette animation de Resident Evil 4 sur place, dès qu'on appuie sur le "
                                   "raccourci, pour la voir sur lui (sans ennemi synchronisé) ; il est invincible pendant "
                                   "tout le mouvement."
                                 : "Chris joue ce mouvement de Resident Evil 4 dès qu'on appuie sur le raccourci, tourné "
                                   "vers l'ennemi le plus proche (3 m) : il blesse les ennemis devant lui pendant le coup et "
                                   "il est invincible pendant tout le mouvement. Les animations viennent de tes jeux "
                                   "(fichiers motions du dossier du trainer).")
                          : std::strstr(kCombat[k].id, "_tag_")
                          ? "Le coup duo de ce personnage en Mercenaries Reunion (fait avec un partenaire dans le jeu), joué "
                            "seul par le joueur 1 dès qu'on appuie sur le raccourci, tourné vers l'ennemi le plus proche "
                            "(3 m) : il blesse l'ennemi que ses mains touchent pendant le coup et il est invincible pendant "
                            "tout le mouvement. L'animation vient des fichiers du jeu (Reunion)."
                          : k == kJoshCounterMove
                          ? "Le Finisher Front de Josh (HEAD CRUSH) joué sur l'ennemi le plus proche (3 m), synchronisé avec "
                            "lui comme la projection de Chris, mais avec ses propres réglages : l'ennemi ne meurt que si les "
                            "dégâts, le critique ou « Tue d'un coup » le décident (sinon il se relève) ; le vrai Finisher "
                            "Front garde les siens. C'est aussi la riposte « Projection de Josh » des parades."
                          : k == kThrow2Move
                          ? "La projection de Jill de la deuxième phase de son combat : Chris bondit en avant en salto vers "
                            "l'ennemi le plus proche (3 m) et, s'il en touche un, le projette. Un Majini ou Jill joue "
                            "l'animation de Chris projeté, synchronisée avec lui, et prend des dégâts ; un autre ennemi est "
                            "seulement placé devant lui. Chris est invincible pendant tout le mouvement."
                          : k == kThrowMove
                          ? "Chris projette l'ennemi le plus proche (3 m) comme il projette Jill dans son combat. Sur Jill c'est la "
                            "vraie projection du jeu ; un Majini joue l'animation de Jill projetée, synchronisée avec "
                            "Chris, et prend des dégâts à la réception ; un autre ennemi est seulement placé devant lui. "
                            "Chris est invincible pendant tout le mouvement. Sans ennemi proche, Chris la joue sur place."
                          : std::strncmp(kCombat[k].id, "melee.free_re6_counter_leap", 27) == 0
                          ? "Le contre de Resident Evil 6 contre les ennemis qui sautent (avec une arme à deux mains : sa variante), "
                            "joué tout de suite vers l'ennemi le plus proche (3 m). C'est aussi ce que deviennent la projection de "
                            "Jill et celle de Josh (en mêlée libre, en échange ou en riposte de parade) sur les chiens, les lickers "
                            "et les parasites araignées ou volants. Chris est invincible pendant tout le mouvement."
                          : std::strncmp(kCombat[k].id, "melee.free_re6_", 15) == 0 && kCombat[k].damage > 0
                          ? "Chris joue ce mouvement de Resident Evil 6 dès qu'on appuie sur le raccourci, tourné vers l'ennemi le "
                            "plus proche (3 m) : il blesse les ennemis que ses membres touchent pendant le coup et il est "
                            "invincible pendant tout le mouvement. L'animation vient de tes fichiers de RE6 (motions\\re6_melee.lmt)."
                          : kCombat[k].damage == 0
                          ? "Chris joue ce mouvement sur place, dès qu'on appuie sur le raccourci ; il est invincible pendant "
                            "tout le mouvement."
                          : kCombat[k].behind
                          ? "Coup de pied retourné : Chris le fait sur place, comme un demi-tour, et blesse les ennemis qui "
                            "étaient derrière lui ; il est invincible pendant tout le mouvement."
                          : !kCombat[k].lock
                          ? "Chris joue ce mouvement de Wesker droit devant lui, sans se tourner vers un ennemi, et blesse "
                            "les ennemis devant lui pendant les coups ; il est invincible pendant tout le mouvement."
                          : "Chris joue ce mouvement de Wesker dès qu'on appuie sur le raccourci, tourné vers l'ennemi le "
                            "plus proche (3 m) : il blesse les ennemis devant lui pendant le coup (un Majini est projeté, "
                            "Jill et Wesker réagissent) et Chris est invincible pendant tout le mouvement.",
              // Wesker's dodges in the Dodge sub-tab (user, 2026-10-23); in the free melee (user, 2026-10-27) Jill's throw
              // with the game's moves, her second part's moves and Wesker's strikes in their own sub-tabs.
              // Each character's RE6 counter in his own sub-tab (user, 2026-11-11: "you forgot the characters' counters in each character's
              // free melee list: I cannot set the critical or the power of Leon's or Jake's counter"); the bayonet's stays common.
              .category = std::strstr(kCombat[k].id, "_tag_")                         ? TagCategory(kCombat[k].id)
                          : std::strcmp(kCombat[k].id, "melee.free_re6_counter_chris") == 0 ? "Mêlée libre Chris RE6"
                          : std::strcmp(kCombat[k].id, "melee.free_re6_counter_piers") == 0 ? "Mêlée libre Piers RE6"
                          : std::strcmp(kCombat[k].id, "melee.free_re6_counter_jake") == 0  ? "Mêlée libre Jake RE6"
                          : std::strcmp(kCombat[k].id, "melee.free_re6_counter_leon") == 0  ? "Mêlée libre Leon RE6"
                          : std::strcmp(kCombat[k].id, "melee.free_re6_counter_ada") == 0   ? "Mêlée libre Ada RE6"
                          : std::strncmp(kCombat[k].id, "melee.free_re6_chris_", 21) == 0 ? "Mêlée libre Chris RE6"
                          : std::strncmp(kCombat[k].id, "melee.free_re6_piers_", 21) == 0 ? "Mêlée libre Piers RE6"
                          : std::strncmp(kCombat[k].id, "melee.free_re6_sherry_", 22) == 0 ? "Mêlée libre Sherry RE6"
                          : std::strncmp(kCombat[k].id, "melee.free_re6_agent_", 21) == 0 ? "Mêlée libre HUNK"
                          : std::strncmp(kCombat[k].id, "melee.free_re6_common_", 22) == 0 ||
                                    std::strncmp(kCombat[k].id, "melee.free_re6_counter", 22) == 0 ||
                                    std::strncmp(kCombat[k].id, "melee.free_re6_ground_attack_", 29) == 0
                              ? "Mêlée libre RE6"
                          : std::strncmp(kCombat[k].id, "melee.free_re6_leon_", 20) == 0  ? "Mêlée libre Leon RE6"
                          : std::strncmp(kCombat[k].id, "melee.free_re6_helena_", 22) == 0 ? "Mêlée libre Helena RE6"
                          : std::strncmp(kCombat[k].id, "melee.free_re6_jake_", 20) == 0  ? "Mêlée libre Jake RE6"
                          : std::strncmp(kCombat[k].id, "melee.free_re6_ada_", 19) == 0   ? "Mêlée libre Ada RE6"
                          : std::strncmp(kCombat[k].id, "melee.free_leon_dodge", 21) == 0  ? "Dodge"
                          : std::strncmp(kCombat[k].id, "melee.free_leon_r_", 18) == 0     ? "Mêlée libre Leon Remake"
                          : std::strncmp(kCombat[k].id, "melee.free_leon", 15) == 0        ? "Mêlée libre Leon"
                          : std::strncmp(kCombat[k].id, "melee.free_hunk", 15) == 0        ? "Mêlée libre HUNK"
                          : std::strncmp(kCombat[k].id, "melee.free_josh", 15) == 0        ? "Mêlée libre Josh"
                          : kCombat[k].damage == 0 && !ThrowMove(k) && k != kThrow2Move   ? "Dodge"
                          : std::strncmp(kCombat[k].id, "melee.free_wesker", 17) == 0     ? "Mêlée libre Wesker"
                          : std::strncmp(kCombat[k].id, "melee.free_jill1", 16) == 0 ||
                                    std::strncmp(kCombat[k].id, "melee.free_jill2", 16) == 0  ? "Mêlée libre Jill"
                                                                                          : "Mêlée libre",
              .kind = Kind::Action,
              .run = [k, dodge] {
                  if (dodge) return StartCombatMove(k);
                  if (Player1MovePlaying()) return false;
                  if (std::strcmp(kCombat[k].id, "melee.free_re6_common_weapon_steal") == 0) {  // on an armed Majini only
                      // The enemy aimed at (the nearest within 3 m) decides (user, 2026-11-11: "if it has no weapon to steal, its back-up -
                      // the Executioner, a licker, a giant Majini, a bare-handed Majini...; a Majini with a weapon to steal: the steal").
                      const uintptr_t target = NearestTarget();
                      if (const std::string backup = BackupFor(kCombat[k].id, target); !backup.empty()) return RunBackup(backup);
                      if (target && MajiniHoldsStealable(target)) return StartCombatMove(k, target);
                      LOG("wesker moves: %s: the enemy aimed at (%08X) holds no weapon to steal, no back-up", kCombat[k].name, static_cast<unsigned>(target));
                      return false;
                  }
                  // "Utiliser les back-up" (melee_tuning.cpp): its back-up on an enemy that refuses it.
                  if (const std::string backup = BackupFor(kCombat[k].id, NearestTarget()); !backup.empty())
                      return RunBackup(backup);
                  return StartCombatMove(k);
              },
              .available = [] { return actors::InGame(); },
              .value = costed ? 200.0f : 0.0f, .valueMin = costed ? -100.0f : 0.0f, .valueMax = costed ? 2000.0f : 0.0f,
              .valueFormat = "%.0f pts (stricte)", .negativeText = "blocage seul" });
    }
    OnFrame(TickCombat);
    // The RE6 parry's counters in the free melee (user, 2026-11-01), their lines tuning the parry's too.
    for (int variant = 0; variant < 2; ++variant) {
        const char* who = variant == 1 ? "Sheva" : "Chris";
        // Named as the parry's riposte list names them (user, 2026-11-11: "I want to set them in the table too" - they were there as
        // "Chris Contre RE6" / "Sheva Contre RE6").
        Add({ .id = kCounterIds[variant], .name = std::string("Contre de ") + who + " (RE5)",
              .help = std::string("Le contre de ") + who + " sur Wesker (dans son combat), joué tout de suite sur l'ennemi "
                      "le plus proche (3 m), tourné vers lui, sans parade ; il est invincible pendant le contre. Ses réglages "
                      "de ce tableau (dégâts, critique, tue d'un coup, réaction) valent aussi pour la parade façon RE6 "
                      "d'un personnage dont le contre est celui de " + who + " (Parade, tableau par personnage).",
              .category = std::string("Mêlée libre ") + who, .kind = Kind::Action,
              .run = [variant] { return !Player1MovePlaying() && PlayFreeCounter(variant); },
              .available = [] { return actors::InGame(); } });
    }
    // Test sliders of the RE4 pairs' placement (user, 2026-10-31: "test sliders, removed afterwards, so that I move Chris
    // and set it perfectly"): Chris moved from the pair's placement when it starts (PairTestOffset). The neck break's
    // removed (user: "well placed" once held on him every frame); the suplex's still there.
    OnRestore([] {  // a ground move played faster when the trainer goes: his own time scale back
        if (g_re6SpedPlayer) mem::Write(g_re6SpedPlayer + 0xFD4, g_re6SpedScale);
        g_re6SpedPlayer = 0;
        g_re6Sped = nullptr;
    });
    // Research (user, 2026-11-09: "the RE6 melee options: look for them in the game"; RE6 picks each character's moves in its code):
    // RE6's 61 melee motions (AcA) played one by one on player 1 for the user to name them.
    Add({ .id = "melee.re6_melee_test", .name = "Test : corps à corps de RE6",
          .help = "Recherche : joue sur Chris le mouvement de corps à corps de RE6 choisi par le curseur (la liste commune AcA, "
                  "puis les mouvements de chaque personnage, Jake, le couteau...), sans coup ni ennemi. Le message donne sa "
                  "liste et son numéro dans RE6, et son type d'attaque. "
                  "Les touches + et - du curseur passent au suivant ou au précédent.",
          .category = "Test RE6", .kind = Kind::Action,
          .run = [kRe6TestMove] {
              const Feature* f = Find("melee.re6_melee_test");
              const int index = f ? static_cast<int>(f->value + 0.5f) : 0;
              g_re6TestIndex = index;
              if (!StartCombatMove(kRe6TestMove)) return false;
              Toast("RE6 n° " + std::to_string(index) + " : " + Re6MeleeLabel(index), true);
              return true;
          },
          .available = [] { return actors::InGame(); },
          .value = 0, .valueMin = 0, .valueMax = static_cast<float>(std::max<size_t>(Re6MeleeRows().size(), 2) - 1),
          .valueFormat = "n° %.0f", .valueStep = 1 });
    static const std::vector<std::string> kStates = { "Head flash", "Recul + head flash", "Arm", "Leg", "Finisher",
        "Recul + Arm (Arm Front de face, Arm Back de dos)", "Recul + Arm Front (natif : head flash de dos)",
        "Recul + Leg (Leg Front de face, Leg Back de dos)", "Recul + Leg Front (de face comme de dos)",
        "Bras + head flash (Arm Back de dos)", "Bras + Arm (Arm Front de face, Arm Back de dos)",
        "Bras + Leg (Leg Front de face, Leg Back de dos)", "Bras + Leg Front (de face comme de dos)",
        "Bras + Finisher", "Genou + head flash (Arm Back de dos)",
        "Genou + Arm (Arm Front de face, Arm Back de dos)", "Genou + Leg (Leg Front de face, Leg Back de dos)",
        "Genou + Leg Front (de face comme de dos)", "Genou + Finisher", "Chute (tir dans la jambe en courant)" };
    static constexpr struct { const char* id; const char* name; const char* help; int value; } kRe6States[4] = {
        { "melee.re6_state_second", "Corps à corps RE6 : réaction au 2e coup",
          "Ce que fait un Majini touché par le 2e coup du combo de la mêlée RE6 (le 1er le fait seulement sursauter).", 10 },
        { "melee.re6_state_staggered", "Corps à corps RE6 : réaction d'un Majini déjà sonné",
          "Un Majini déjà sonné (glissade, tir rapide, tir qui le fait vaciller, Arm Front...) touché par un coup de la mêlée RE6.", 18 },
        { "melee.re6_state_elbow", "Corps à corps RE6 : réaction au coup de coude arrière",
          "Ce que fait un Majini touché par le coup de coude arrière (attaque en reculant).", 18 },
        { "melee.re6_state_sprint", "Corps à corps RE6 : réaction à l'attaque en sprint",
          "Ce que fait un Majini touché par l'attaque en sprint (coup d'épaule, coup de pied sauté...).", 5 },
    };
    for (int i = 0; i < 4; ++i)
        Add({ .id = kRe6States[i].id, .name = kRe6States[i].name, .help = kRe6States[i].help, .category = "Réglages RE6",
              .kind = Kind::Choice, .available = [] { return game::IsSupported(); },
              .choose = [i](int index) {
                  g_re6MeleeState[i] = index;
                  return true;
              },
              .value = static_cast<float>(kRe6States[i].value), .valueMin = 0, .valueMax = 19, .valueFormat = "%.0f",
              .choices = kStates });
    Add({ .id = "combat.parry_re6_special", .name = "Contres spéciaux de RE6 (Majini armés)",
          .help = "Avec la parade façon RE6 : un Majini qui frappe avec un couteau ou une faucille est désarmé et tué avec sa propre arme, un Majini à la bouteille ou qui frappe avec la crosse de son fusil reçoit le contre de l'arme à feu, un Majini à la hache, à la massue ou à la clé reçoit l'un des deux contres de RE6 contre ces armes (au hasard ; dans le second, Chris lui prend son arme). Les armes de Chris sont cachées pendant ces coups. Aussi dans Mêlée libre RE6, avec un raccourci.",
          .category = "Parade", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_specialCounters = on;
              return true;
          },
          .available = [] { return game::IsSupported(); },
          .defaultOn = true });
    Add({ .id = "melee.re6_ground_weapons", .name = "Ground de RE6 avec une arme à deux mains ou le couteau",
          .help = "Le Ground (le coup sur un ennemi à terre) de tous les personnages devient celui de Resident Evil 6 avec une "
                  "arme à deux mains quand le joueur 1 en tient une, et celui au couteau quand il tient le couteau. Sinon, le "
                  "Ground du jeu.",
          .category = "Mêlée", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_re6GroundWeapons = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "melee.re6_ground_move", .name = "Au sol : roulade ou dash arrière (RE6)",
          .help = "Couché au sol après une esquive RE6 (visée tenue) : avec gauche ou droite, Chris roule de ce côté ; avec "
                  "arrière, il fait le dash arrière au sol de RE6 (il reste au sol si tu vises encore, sinon il se relève) ; "
                  "avec avant, il se relève vers l'avant et enchaîne une plongée avant (un dash avant) ; sans direction, la "
                  "roulade arrière de relevé le remet debout. Avec « Endurance stricte », chacun coûte le curseur de son "
                  "mouvement (celui de « Esquive avant (RE6) » pour le dash avant).",
          .category = "Dodge", .kind = Kind::Action,
          .run = [] {
              g_re6GroundMoveAsked = GetTickCount() | 1;
              return true;
          },
          .available = [] { return actors::InGame(); } });
    Add({ .id = "melee.re6_crouch", .name = "Se baisser en visant (pose figée)",
          .help = "En visant debout, Chris se baisse (le début de l'esquive basse de RE6) et garde la pose la plus basse tant que "
                  "tu vises ; appuie de nouveau, ou lâche la visée, pour qu'il se relève. L'esquive sur place de RE6 garde sa "
                  "propre touche et remonte toujours.",
          .category = "Dodge", .kind = Kind::Action,
          .run = [] {
              g_re6CrouchAsked = GetTickCount() | 1;
              return true;
          },
          .available = [] { return actors::InGame(); } });
    Add({ .id = "melee.trainer_grunts", .name = "Sons des mouvements du trainer (cris et impacts)",
          .help = "Le personnage joué pousse un de ses cris (ceux de son Head Flash et de son Arm Front, avec sa propre voix) "
                  "au coup des mouvements du trainer (Wesker, Jill, Leon, HUNK...) et au début de leurs esquives ; quand le "
                  "coup touche, les impacts natifs d'un coup de pied (ou de poing pour la paume) de Wesker pour ses coups, "
                  "de Jill pour les siens, du personnage joué sinon ; le brise-nuque de HUNK fait le craquement du neck break "
                  "de Chris. Le curseur règle le volume (au-delà de 100 %, un limiteur évite la saturation). Les sons "
                  "viennent de tes fichiers du jeu (tools/extract_player_sounds.py).",
          .category = "Mêlée", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (!on) g_gruntGain = 0.0f;
              return true;
          },
          .available = [] { return game::IsSupported(); },
          .value = 30.0f, .valueMin = 10.0f, .valueMax = 800.0f, .valueFormat = "%.0f %%",
          .defaultOn = true });  // 30 % (user: 300 %, 150 %, 100 %, 50 % too loud)
    if (Feature* grunts = Find("melee.trainer_grunts"))
        grunts->tick = [grunts] { g_gruntGain = grunts->value / 100.0f; };
    {
        static const char* const kPairs[2] = { "brise-nuque de HUNK", "suplex de Leon" };
        static const char* const kPairIds[2] = { "neck", "suplex" };
        static const char* const kAxes[3] = { "Chris sur le côté", "Chris en avant", "Chris tourné" };
        static const char* const kAxisIds[3] = { "side", "forward", "turn" };
        for (int pair = 1; pair < 2; ++pair)
            for (int axis = 0; axis < 3; ++axis) {
                Feature& f = Add({ .id = std::string("test.re4_pair_") + kPairIds[pair] + "_" + kAxisIds[axis],
                                   .name = std::string("Test ") + kPairs[pair] + " : " + kAxes[axis],
                                   .help = "Réglage de test, à retirer une fois la position trouvée : au début du mouvement, "
                                           "Chris est déplacé de cette valeur par rapport à l'ennemi synchronisé (côté et "
                                           "avant en cm dans son propre repère, rotation en degrés). Dis-moi les valeurs "
                                           "qui vont bien et je les mets dans le mouvement.",
                                   .category = "Mêlée", .kind = Kind::Toggle,
                                   .set = [pair, axis](bool on) {
                                       if (!on) g_pairTest[pair][axis] = 0.0f;
                                       return true;
                                   },
                                   .available = [] { return game::IsSupported(); },
                                   .value = 0.0f, .valueMin = axis == 2 ? -180.0f : -100.0f,
                                   .valueMax = axis == 2 ? 180.0f : 100.0f, .valueFormat = axis == 2 ? "%.0f deg" : "%.0f cm",
                                   .defaultOn = true });
                Feature* self = &f;
                f.tick = [pair, axis, self] { g_pairTest[pair][axis] = self->value; };
            }
    }
    Add({ .id = "melee.no_hand_shake", .name = "Pas de main secouée après un coup de poing",
          .help = "Chris ne secoue plus la main comme s'il s'était fait mal à la fin de certains coups de poing (le contre "
                  "de la parade RE6, son coup de poing de mêlée) : le mouvement s'arrête juste avant et Chris reprend "
                  "la main aussitôt.",
          .category = "Mêlée", .kind = Kind::Toggle,
          .tick = TickHandShakeCut,
          .available = [] { return game::IsSupported(); },
          .defaultOn = true });
    Feature& guardSpeed = Add({ .id = "combat.parry_guard_speed", .name = "Vitesse de la garde de Wesker (parade)",
          .help = "Accélère la garde de Wesker après une parade (riposte « Garde de Wesker ») du facteur du curseur. "
                  "Sans cette option elle est jouée deux fois plus vite. La marche pendant la garde garde son rythme.",
          .category = "Parade", .kind = Kind::Toggle,
          .available = [] { return game::IsSupported(); },
          .value = 3.0f, .valueMin = 1.0f, .valueMax = 5.0f, .valueFormat = "x%.1f" });
    {
        Feature* const gs = &guardSpeed;
        gs->set = [gs](bool on) {
            g_guardSpeed = on ? gs->value : 2.0f;
            return true;
        };
        gs->tick = [gs] { g_guardSpeed.store(gs->value, std::memory_order_relaxed); };
    }
    OnRestore([] {  // unloaded during a guard: its time scale back
        if (g_guardSped) mem::Write(g_guardSped + 0xFD4, g_guardSavedScale);
        g_guardSped = 0;
    });
    Add({ .id = "player.acrobatic_getup", .name = "Relevé acrobatique",
          .help = "Quand Chris se relève après avoir été mis à terre, il le fait avec le relevé de Jill (celui qu'elle "
                  "joue en combat), sur le dos comme sur le ventre.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureSetMotionHook()) return false;
              g_acrobaticGetup = on;
              return true;
          },
          .tick = [] {
              // Jill's damage list in player 1's free bank B (each level loads a new player).
              const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
              if (!p) return;
              if (!g_jillDamageList) g_jillDamageList = LoadList("uEm83", "pawn\\em\\em83\\motion\\em83dmg");
              uintptr_t list = 0;
              if (g_jillDamageList && mem::Read(p + 0xA88 + 4 * kPlayerJillDamageBank, list) &&
                  list != reinterpret_cast<uintptr_t>(g_jillDamageList))
                  SetBank(p, g_jillDamageList, kPlayerJillDamageBank);
          },
          .available = [] { return actors::InGame(); } });
    Add({ .id = "player.ground_back_roll", .name = "Roulade arrière au sol (RE6)",
          .help = "Quand Chris est mis à terre, tenir la touche de course le relève tout de suite, sans l'attente au sol "
                  "(comme dans RE6) : sur le dos par la roulade arrière de Lost in Nightmares, sur le ventre par une "
                  "roulade avant.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureSetMotionHook() || !EnsurePlayerRequestHook())) return false;
              if (!UsePadMaskForGroundRoll(on) && on) LOG("ground roll: no pad mask hook, the pad's run button not read");
              g_groundRoll = on;
              return true;
          },
          .tick = [] {
              const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
              if (!p) return;
              // The DLC Wesker's player list in player 1's free bank 7 (each level loads a new player).
              if (!g_ema2PlayerList) g_ema2PlayerList = LoadList("uEma2", "pawn\\em\\ema2\\motion\\ema2_pl");
              uintptr_t list = 0;
              if (!g_ema2PlayerList || !mem::Read(p + 0xA88 + 4 * kPlayerRollBank, list)) return;
              if (list != reinterpret_cast<uintptr_t>(g_ema2PlayerList)) SetBank(p, g_ema2PlayerList, kPlayerRollBank);
              uint32_t action = 0, motion = 0;
              if (!mem::Read(p + 0xA88 + 4 * kPlayerRollBank, list) || list != reinterpret_cast<uintptr_t>(g_ema2PlayerList) ||
                  !mem::Read(p + 0x10E0, action))
                  return;
              float hip = 0, facing = 0;
              const bool fall = FallAction(action);
              const bool lying = action == 0x95 || (fall && LyingOnBack(p, &hip, &facing));
              static uint32_t loggedFall = 0;  // his landing logged once per fall (the thresholds to tune)
              if (!fall) loggedFall = 0;
              if (fall && hip > 0.0f && hip < 35.0f && loggedFall != action) {  // landed, on his back or not
                  loggedFall = action;
                  mem::Read(p + 0x384, motion);
                  float frame = 0;
                  mem::Read(p + 0x398, frame);
                  LOG("wesker moves: player 1 on the ground in his fall %X (motion %X frame %.0f, hips %.0f cm, facing %.2f)%s",
                      action, motion & 0xFFFF, frame, hip, facing, Player1RunHeld() ? ", run held" : "");
              }
              // Not while the red arc of "Endurance" fills (user, 2026-10-26).
              if (!StaminaAllows()) return;
              if (fall && !lying && Player1RunHeld() && LyingFaceDown(p) &&
                  static_cast<int32_t>(GetTickCount() - g_bellyRollAsked.load()) >= 500) {
                  g_bellyRollAsked = GetTickCount() | 1;
                  LOG("wesker moves: player 1 face down (action %X), run held: forward roll asked (%s)", action,
                      RequestPlayer(p, kForwardRollAction) ? "accepted" : "refused");
              }
              if (lying && Player1RunHeld() && static_cast<int32_t>(GetTickCount() - g_groundRollAsked.load()) >= 500) {
                  g_groundRollAsked = GetTickCount() | 1;
                  LOG("wesker moves: player 1 on his back (action %X), run held: getup asked (%s)", action,
                      RequestPlayer(p, 0x97) ? "accepted" : "refused");
              }
          },
          .available = [] { return actors::InGame(); } });
    Feature& test = Add({ .id = "diag.wesker_move_test", .name = "Essai : mouvement de Wesker / Jill sur Chris (recherche)",
                          .help = "Fait jouer à Chris le mouvement choisi par le curseur (0 garde de Wesker, 1 contre, 2-7 "
                                  "esquives, 8 projection de Chris (sur Jill), 9-11 coups de pied de Wesker, 12 son balayage, 13 sa riposte après une série d'esquives, "
                                  "14-18 les courses murales de Jill (2e phase), 19-22 ses flips arrière possibles, 23-27 ses "
                                  "rondades possibles, 28-32 ses balayages possibles), "
                                  "pour voir s'il passe bien sur son squelette.",
                          .category = "Diagnostic", .kind = Kind::Action,
                          .available = [] { return game::IsSupported(); },
                          .value = 0, .valueMin = 0, .valueMax = static_cast<float>(std::size(kMoves) - 1),
                          .valueFormat = "%.0f" });
    Feature* const pt = &test;
    test.run = [pt] {
        const int k = static_cast<int>(pt->value + 0.5f);
        if (k < 0 || k >= static_cast<int>(std::size(kMoves))) return false;
        const bool ok = PlayWeskerMove(kMoves[k].bank, kMoves[k].index);
        Toast(std::string(i18n::Tr("Mouvement : ")) + kMoves[k].name + (ok ? "" : i18n::Tr(" (échec)")), ok);
        return ok;
    };
}
}  // namespace features
