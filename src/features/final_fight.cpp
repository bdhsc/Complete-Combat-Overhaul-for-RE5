// The final fight in the volcano with Jill as the partner (user, 2026-11-01: "a new option for the final fight in the
// volcano: it gives back every function of "Joueur 2 invisible" (not only the visibility) and changes the partner into
// Jill with Jill's animations, whatever the trainer's partner or animations", "only after the cutscene with Wesker: in
// the volcano before the boss comes the partner may stay invisible", "or Jill from the start of the volcano, visible
// only after the cutscene", "it does not make her invisible: only if "Joueur 2 invisible" is on she stays invisible
// until the fight starts", "if the partner has room: a VZ61 fully loaded and upgraded and a box of machine-gun ammo",
// "ignore "Ne pas attaquer le joueur 2" too", "and every option limiting the partner's actions, picking up items,
// attacking enemies", "one option: checked, all of its effects").
// Chapter 6-3's volcano is room 0x1FC (room word [profile [RVA 0xDB2158] + its index (+0x20) * 0x5940 + 0x273D8]),
// the walk to Wesker and his whole fight: his cutscene changes no room. Its Wesker is uEm82 (vtable RVA 0xB8D878, with
// uEm8c and ten uEm8e, read live in the fight); uEm81 in room 0x1FA is the bomber's Wesker (the injection, MODLOG
// session 18), before. The fight is told by uEm82 among the enemies, out of an event (action below 0x80000000; user,
// 2026-11-01: "Jill was invisible during the fight" while the room was waited for). The partner's character is the
// profile's player block, read when the players are made: swaps.cpp writes Jill (character 2, costume 1: the Battle Suit) during
// every load in the volcano and keeps her own motion lists, so she comes at its first load there (a checkpoint restart
// in 0x1FC), and puts the partner of before back at the next load elsewhere. In the fight, players there: "Joueur 2
// invisible", "Le joueur 2 n'attaque pas / ne tire pas à la tourelle" (player.cpp), "ne ramasse pas
// les objets / ne te réanime pas / ne te libère pas des prises" (action_log.cpp) and "Ne pas attaquer le joueur 2"
// (enemies.cpp) stand aside; the partner gets a VZ61 (0x0103) with every upgrade level of its table and a full
// magazine (only if she has room and none), and her machine-gun ammo (0x0202) at the largest stack of the inventory tab's
// table (a box given if she has none and room).
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace features {
bool EnsureActorDamageHook();                           // player.cpp: the actors' damage (FinalQteDamage)
bool EnsurePadQueryHooks();                             // quick_knife.cpp: FinalQteTick on the game thread (coop_solo.cpp)
bool GiveUpgradedWeapon(int playerIndex, uint16_t id);  // inventory.cpp: every upgrade level, full magazine, if room
int FillStack(int playerIndex, uint16_t id);            // inventory.cpp: a stack at its largest size, held or given
bool PlayerHasItem(int playerIndex, uint16_t id);       // inventory.cpp

namespace {
constexpr uint32_t kVolcanoRoom = 0x1FC;
constexpr uintptr_t kVolcanoWeskerVtable = 0xB8D878;  // uEm82 (RVA)
constexpr uint16_t kVz61 = 0x0103, kMachineGunAmmo = 0x0202;
constexpr DWORD kGiveDelay = 2000;  // after the partner appears in the fight (her inventory set)

std::atomic<bool> g_on{ false };
std::atomic<uint32_t> g_room{ 0 };
std::atomic<bool> g_fight{ false };  // the final Wesker fights, players made
uintptr_t g_givenTo = 0;             // the partner given the VZ61 (a restart makes a new one)
DWORD g_partnerSeen = 0;

bool Room(uint32_t& room) {
    uintptr_t profile = 0;
    uint32_t index = 0;
    return mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), profile) && profile &&
           mem::Read(profile + 0x20, index) && index < 4 && mem::Read(profile + index * 0x5940 + 0x273D8, room);
}

// Her VZ61 when she has none (a checkpoint saved after it keeps it), and her machine-gun ammo at the largest stack (the
// inventory tab's stack table: user, 2026-11-02, "the box spawned with 30 rounds"), filled up at each fight start.
void GiveWeapons(uintptr_t partner) {
    g_givenTo = partner;
    const bool had = PlayerHasItem(1, kVz61);
    const bool vz61 = !had && GiveUpgradedWeapon(1, kVz61);
    if (!had && !vz61) {
        LOG("final fight: no VZ61 given (no room in the partner's inventory, or not loadable)");
        return;
    }
    const int ammo = FillStack(1, kMachineGunAmmo);
    LOG("final fight: the partner %s, machine-gun ammo %d", vz61 ? "got an upgraded VZ61" : "already has a VZ61", ammo);
    char text[96];
    if (vz61) snprintf(text, sizeof(text), ammo ? i18n::Tr("Jill reçoit un VZ61 amélioré et %d balles") : i18n::Tr("Jill reçoit un VZ61 amélioré (pas de place pour les munitions)"), ammo);
    else snprintf(text, sizeof(text), i18n::Tr("Munitions de Jill : %d balles"), ammo);
    if (vz61 || ammo) Toast(text);
}

// The volcano's Wesker present, and whether out of his events (his cutscene's).
bool WeskerPresent(bool& fighting) {
    fighting = false;
    bool present = false;
    for (uintptr_t e : actors::Enemies()) {
        uintptr_t vtable = 0;
        uint32_t action = 0;
        if (!mem::Read(e, vtable) || vtable != reinterpret_cast<uintptr_t>(mem::Rva(kVolcanoWeskerVtable))) continue;
        present = true;
        fighting = fighting || (mem::Read(e + 0x10E0, action) && action < 0x80000000u);
    }
    return present;
}

// The fight begins once he has been out of his events for a second (a restart showed him briefly), and lasts while he
// is there, his events during it included (his grabs: the partner never hidden or held back again in the middle).
constexpr DWORD kFightConfirm = 1000;
DWORD g_fightingSince = 0;

void Tick() {
    uint32_t room = 0;
    if (!Room(room)) room = 0;
    const uint32_t before = g_room.exchange(room);
    const bool players = actors::InGame() && actors::Player(1);
    bool fighting = false;
    const bool present = players && room == kVolcanoRoom && WeskerPresent(fighting);
    bool fight = false;
    if (!present) {
        g_fightingSince = 0;
    } else if (g_fight.load(std::memory_order_relaxed)) {
        fight = true;
    } else if (fighting) {
        const DWORD now = GetTickCount();
        if (!g_fightingSince) g_fightingSince = now ? now : 1;
        fight = now - g_fightingSince >= kFightConfirm;
    } else {
        g_fightingSince = 0;
    }
    if (before != room) LOG("final fight: room %X -> %X (players %d)", before, room, players ? 1 : 0);
    if (fight != g_fight.exchange(fight))
        LOG("final fight: %s", fight ? "the fight began: the partner is given back every action" : "out of the fight");
    if (!fight) {
        g_partnerSeen = 0;
        return;
    }
    const uintptr_t partner = actors::Player(1);
    if (partner == g_givenTo) return;
    const DWORD now = GetTickCount();
    if (!g_partnerSeen) g_partnerSeen = now ? now : 1;
    if (now - g_partnerSeen >= kGiveDelay) GiveWeapons(partner);
}
}  // namespace

// ---- "Combat final : prise finale automatique" (user, 2026-11-02: "the game ends either by hurting Wesker until his end
// scene, or by coming close when his weak point is exposed and playing the duo finish with Sheva: launch that one by
// itself a bit before he dies, so he can no longer be beaten by shooting; a slider for the health that launches it; the
// mashing made easy only when it launched by itself because I hurt him enough - come close earlier and it keeps the
// game's difficulty; a final timed QTE, if any, left intact").
// uEm82's finish is DmgLastPinion 0x3B (then 0x3C End, 0x3D Die), asked through his vfunc +0x234 (VA 0x9214C0,
// thiscall(player index, mode, 0) ret 0xC: mode 3 = request 0x3B with that player as the request's partner; mode 0 his
// Pinion 0x38, 1 / 2 its ends 0x39 / 0x3A; the prompts go through VA 0x9212F0(index, mode) -> vfunc +0x230 VA 0x921330,
// which asks mode 3 from his Pinion). DmgLastPinion's init (VA 0x92D0E0) takes the request's partner as +0xC4 and the
// other player (VA 0xB5BED0(+0xC4, 1)) as +0xC0, both put in their events. Its update (VA 0x926400): the player +0xC0
// presses its prompt [+0xC8] (pressed +0xB): +0xD0 counts them, a timer +0xE8 (table [VA 0x111BED0 + difficulty * 4]:
// 1.0, 1.0, 1.0, 0.9, 0.9, 0.8 s) checks them: fewer than 5 = 100 damage (VA 0xB56FB0: player vfunc +0x114), at 1 health
// mode 4 (the failure); the player +0xC4 presses its prompt [+0xCC]: +0xD4 counts them up to +0xD8, then mode 5 (the
// success). The trainer's own launch makes the woman the request's partner as the game does (RequesterIndex), the man
// the regular presses; it divides the target by 4 and keeps the regular count at 5 (no damage), only for that launch.
// Below the slider's health Wesker loses no more (the shots cannot end the fight), and the launch is asked when he is
// out of his events, jumps and other holds.
namespace {
constexpr uint32_t kLastPinion = 0x3B, kLastPinionDie = 0x3D;
constexpr int kTargetDivider = 4;
std::atomic<bool> g_qteOn{ false };
Feature* g_qteFeature = nullptr;  // its slider: the health that launches it
std::atomic<int> g_qteHealth{ 2000 };
bool g_qteOurs = false;           // game thread: the sequence under way is the trainer's
int32_t g_qteTargetWritten = -1;  // the target last written (game thread)
DWORD g_qteAsked = 0;

uintptr_t VolcanoWesker() {
    for (uintptr_t e : actors::Enemies()) {
        uintptr_t vtable = 0;
        if (mem::Read(e, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(kVolcanoWeskerVtable))) return e;
    }
    return 0;
}

// Not while he is in an event, a jump, a stage change, a hold or his finish.
bool WeskerBusy(uint32_t action) {
    return action >= 0x80000000u || (action >= 0x08 && action <= 0x0F) || (action >= 0x29 && action <= 0x2E) ||
           action >= 0x37;
}

// The request's partner as the game makes it: the woman (user, 2026-11-02: "Chris did Sheva's part and Jill Chris's" when
// player 1 was asked): his prompts flag the player by its weapon family (+0x1AF8: mode 0xA, 0x4000 the woman / 0x2000
// the man), and the finish then comes from her side - she fills the counter, the man does the regular presses (the part
// that killed the user in Professional). Player 1 when neither or both are women.
int RequesterIndex() {
    int woman = -1, women = 0;
    for (int i = 0; i < 2; ++i) {
        uint8_t female = 0;
        const uintptr_t p = actors::Player(i);
        if (p && mem::Read(p + 0x1AF8, female) && female == 1) {
            woman = i;
            ++women;
        }
    }
    return women == 1 ? woman : 0;
}

bool AskLastPinion(uintptr_t wesker, int index) {
    using PinionFn = void(__fastcall*)(void* self, void* edx, int player, int mode, int flag);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(wesker, vtable) || !mem::Read(vtable + 0x234, fn) || !fn) return false;
    __try {
        reinterpret_cast<PinionFn>(fn)(reinterpret_cast<void*>(wesker), nullptr, index, 3, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}
}  // namespace

// player.cpp's enemy damage: the volcano's Wesker kept at the slider's health until the trainer's finish.
int16_t FinalQteDamage(uintptr_t enemy, int16_t amount) {
    if (!g_qteOn.load(std::memory_order_relaxed) || amount <= 0) return amount;
    uintptr_t vtable = 0;
    int16_t health = 0;
    if (!mem::Read(enemy, vtable) || vtable != reinterpret_cast<uintptr_t>(mem::Rva(kVolcanoWeskerVtable)) ||
        !mem::Read(enemy + actors::off::Health, health))
        return amount;
    uint32_t action = 0;
    if (mem::Read(enemy + 0x10E0, action) && action >= kLastPinion && action <= kLastPinionDie) return amount;  // his finish
    const int floor = std::max(1, g_qteHealth.load(std::memory_order_relaxed));
    if (health - amount >= floor) return amount;
    return static_cast<int16_t>(std::max(0, health - floor));
}

// coop_solo.cpp's pad query tick (game thread): the finish launched at the slider's health, made easy when it is ours.
void FinalQteTick() {
    if (!g_qteOn.load(std::memory_order_relaxed) || !actors::InGame()) {
        g_qteOurs = false;
        return;
    }
    if (g_qteFeature) g_qteHealth = static_cast<int>(g_qteFeature->value);
    const uintptr_t w = VolcanoWesker();
    uint32_t action = 0;
    int16_t health = 0;
    if (!w || !mem::Read(w + 0x10E0, action) || !mem::Read(w + actors::off::Health, health)) {
        g_qteOurs = false;
        return;
    }
    const DWORD now = GetTickCount();
    if (action >= kLastPinion && action <= kLastPinionDie) {
        if (g_qteAsked && now - g_qteAsked < 3000 && !g_qteOurs) {
            g_qteOurs = true;
            g_qteTargetWritten = -1;
            LOG("final finish: the trainer's DmgLastPinion began (Wesker %d health)", health);
        }
        uintptr_t table = 0, object = 0;
        if (!g_qteOurs || action != kLastPinion || !mem::Read(w + 0x10DC, table) || !table ||
            !mem::Read(table + 4 * kLastPinion, object) || !object)
            return;
        int32_t presses = 0, target = 0;
        if (mem::Read(object + 0xD0, presses) && presses < 5) mem::Write<int32_t>(object + 0xD0, 5);  // no damage
        // The target once the game set it (user-verified 2026-11-02, logged: read on the sequence's first frame it was
        // still another value, 0x415E5BA8, and the game's own came after): any new one of the game's divided again.
        if (mem::Read(object + 0xD8, target) && target > 0 && target < 10000 && target != g_qteTargetWritten) {
            const int32_t easy = std::max(1, target / kTargetDivider);
            mem::Write<int32_t>(object + 0xD8, easy);
            g_qteTargetWritten = easy;
            LOG("final finish: presses to fill %d -> %d, the regular presses kept at 5", target, easy);
        }
        return;
    }
    if (g_qteOurs) LOG("final finish: the trainer's sequence ended (Wesker action %X, %d health)", action, health);
    g_qteOurs = false;
    const uintptr_t p1 = actors::Player(0);
    uint32_t p1Action = 0;
    if (health > g_qteHealth.load(std::memory_order_relaxed) || WeskerBusy(action) || !p1 || !actors::OnFoot(p1) ||
        !mem::Read(p1 + 0x10E0, p1Action) || p1Action >= 0x80000000u || now - g_qteAsked < 1500)
        return;
    g_qteAsked = now ? now : 1;
    const int requester = RequesterIndex();
    const bool asked = AskLastPinion(w, requester);
    LOG("final finish: Wesker at %d health (action %X): his DmgLastPinion %s with player %d", health, action,
        asked ? "asked" : "faulted", requester + 1);
}

// camera.cpp: the volcano Wesker's finish under way (DmgLastPinion, its end, his death), however launched: "Garder la
// caméra normale au corps à corps" gives its camera back to the animation then (user, 2026-11-02: "the free camera is
// really not clean on the finish").
bool VolcanoFinishPlaying() {
    if (!actors::InGame()) return false;
    const uintptr_t w = VolcanoWesker();
    uint32_t action = 0;
    return w && mem::Read(w + 0x10E0, action) && action >= kLastPinion && action <= kLastPinionDie;
}

// wesker_boss.cpp's "PV de Wesker": the health that launches the finish, -1 when the option is off.
int FinalQteHealth() { return g_qteOn.load(std::memory_order_relaxed) ? g_qteHealth.load(std::memory_order_relaxed) : -1; }

// swaps.cpp: during the loads in the volcano player 2 is Jill with her own motion lists.
bool FinalFightJill() {
    return g_on.load(std::memory_order_relaxed) && g_room.load(std::memory_order_relaxed) == kVolcanoRoom;
}

// player.cpp, action_log.cpp, enemies.cpp: in the fight the options hiding or limiting the partner stand aside.
bool FinalFightPartnerFree() { return g_on.load(std::memory_order_relaxed) && g_fight.load(std::memory_order_relaxed); }

void RegisterFinalFight() {
    Add({ .id = "partner.final_fight_jill", .name = "Combat final : Jill en partenaire",
          .help = "Dans le volcan (chapitre 6-3), le partenaire devient Jill avec ses propres animations, quels que soient "
                  "le partenaire et les animations choisis dans le trainer : au prochain chargement dans le volcan "
                  "(recommencer au point de passage). Après la cinématique de Wesker, dès qu'il combat, pendant "
                  "le combat : « Joueur 2 invisible » (elle reste invisible avant), « Ne pas attaquer le joueur 2 » et "
                  "les options qui limitent le partenaire (n'attaque pas, ne tire pas à la tourelle, ne ramasse pas, ne "
                  "te réanime pas, ne te libère pas des prises) ne s'appliquent plus, et elle reçoit, si elle a de la "
                  "place, un VZ61 chargé avec toutes ses améliorations et une boîte de munitions de mitraillette.",
          .category = "Personnage", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_on = on;
              if (!on) g_fight = false;
              return true;
          },
          .tick = Tick,
          .available = [] { return game::IsSupported(); } });
    g_qteFeature = &Add({ .id = "enemy.wesker_volcano_finish", .name = "Combat final : prise finale automatique",
                          .help = "Dans le volcan, quand Wesker descend aux PV du curseur, la prise finale à deux (celle qu'on "
                                  "lance en s'approchant de son point faible) se lance toute seule, même sans t'approcher, "
                                  "et il ne perd plus de PV avant : impossible de l'achever en tirant. Les rôles sont ceux du "
                                  "jeu (Jill remplit le compteur, Chris fait les appuis réguliers). Lancée ainsi, elle est "
                                  "facile : 4 fois moins d'appuis pour le compteur, et les appuis réguliers ne coûtent plus "
                                  "de PV. Lancée plus tôt "
                                  "en t'approchant, elle garde la difficulté du jeu. Ses PV restent affichés avec « PV de "
                                  "Wesker ».",
                          .category = "Ennemis", .kind = Kind::Toggle,
                          .set = [](bool on) {
                              if (on && (!EnsureActorDamageHook() || !EnsurePadQueryHooks())) return false;
                              g_qteOn = on;
                              return true;
                          },
                          .available = [] { return game::IsSupported(); },
                          .value = 2000, .valueMin = 1, .valueMax = 22000, .valueFormat = "%.0f PV" });
}
}  // namespace features
