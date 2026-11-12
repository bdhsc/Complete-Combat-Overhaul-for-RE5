// Quick knife (user, 2026-10-15): on keyboard / mouse the game has a quick knife key that does not go through the knife
// aim; bound to the attack key it also stabs instead of shooting while aiming. Research first: which game button the
// quick knife is and where the player reads it.
// The game reads its buttons through sBioPad [RVA 0xE49C40] queries thiscall(button, pad) ret 8 (ecx unused, the
// singleton is reloaded): RVA 0x39A3E0 (Triggered, see dying.cpp), 0x39A430, 0x39A480, 0x39A4D0 (each maps the button
// through RVA 0x7B6E10 and asks the pad object). diag.pad_log writes every query that answers true with its caller.
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/hotkeys.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "hooks/hooks.hpp"

#include <windows.h>
#include <intrin.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace features {
bool StaminaAllows();  // stamina.cpp: the red arc not filling

bool InventoryClickHeld();  // pickup.cpp: "Pas d'attaque depuis l'inventaire"
bool Re6MeleeTakesFire();   // wesker_moves.cpp: the RE6 melee takes the attack key (an enemy or a crate near)
bool Re6GroundHoldsPlayer();  // wesker_moves.cpp: lying or ducked on RE6's ground (no direction for the game)
bool WeskerKeepsHealth(uintptr_t enemy);  // wesker_boss.cpp: "Wesker n'abandonne jamais"
bool ForceMajiniState(uintptr_t enemy, int state);  // parry.cpp: a melee prompt state
void ArmPromptTick();                                // parry.cpp: game thread, the arm states' prompts
void EnemySpawnTick();                               // enemy_spawn.cpp: game thread, a requested spawn
void CoopSoloTick();                                 // coop_solo.cpp: game thread, a partner action validated
void BeltLampTick();                                 // belt_lamp.cpp: game thread, the lamps made / put out
void CrankTick();                                    // crank.cpp: game thread, a held gate let down
extern std::atomic<bool> g_envHeightReady;          // wesker_moves.cpp: an execution from a height ready
bool CoopSoloActionPrompt(uintptr_t owner);          // coop_solo.cpp: a co-op object done alone, its prompt on the action
void LeverTick();                                    // levers.cpp: game thread, a lever kept down
void BoatTick();                                     // boat.cpp: game thread, the airboat's driver aims
void RaftTick();                                     // raft.cpp: game thread, the raft played alone
void HeadBlowTick();                                 // melee_tuning.cpp: game thread, a critical's head blow
void EnvExecTick();                                  // wesker_moves.cpp: game thread, the environmental executions' checks
void SheathGunReloadTick();                          // inventory.cpp: game thread, the knife slot's gun reloaded
int Player1Pad();                                    // movement.cpp: the pad index player 1's buttons are read with
bool MajiniDieNow(uintptr_t enemy);                 // parry.cpp: health 0 without a hit, dies at once
bool MajiniBlowOff(uintptr_t enemy, uintptr_t player);  // parry.cpp: a shotgun blast's blow-off without a hit
bool Re6OnGround();  // wesker_moves.cpp: RE6's ground, no trainer move
bool EnsurePlayerRequestHook();                      // action_log.cpp: QuickShotRefusesPlayerAction is asked there
uint32_t Re6EvadeMotion(uint16_t slot);              // wesker_moves.cpp: a motion of re6_evade.lmt in player 1's lists
bool PlayShotLayer(uintptr_t p, uint32_t motion);    // movement.cpp: RE6's whole-body shot on the leg layer
float ShotLayerFrame(uintptr_t p);                   // movement.cpp: its frame (-1: not playing)
float ShotLayerLength(uintptr_t p);
void StopShotLayer(uintptr_t p);
bool CameraRay(float* eye, float& yaw, float& pitch);  // movement.cpp: the normal / aim camera
bool EnsureEnemyOnHitHook();                        // knife.cpp: QuickShotSawHit is called from it
uint32_t MajiniStateAction(int state);              // parry.cpp: the action of that state
bool EnsureEnemyDamageHook();                       // knife.cpp: RecordBulletDamage is called from it
bool EnemyOnGround(uintptr_t enemy);                // knife.cpp: falls, hit on the ground (not getting up)
uint16_t PlayerHeldItem(uintptr_t player);          // knife.cpp
bool PadHeldMask(uint32_t& mask);                   // movement.cpp: a pad's held mask (false on the keyboard)
bool UsePadMaskForQuickShot(bool on);               // movement.cpp
bool Player1PadInUse();                             // movement.cpp
bool NearbyItemHasNoRoom();                         // pickup.cpp: the item by player 1 would not fit
bool UseAimMarkerCalls(bool on);                    // patch_features.cpp: laser / crosshair call sites
void HideAimMarkers(bool hide);
bool LoadWeaponArchive(const char* name);           // inventory.cpp: Image\Archive\<name>, kept loaded
bool Player1InventoryOpen();                         // pickup.cpp
bool EnsureAttachedMatrixHook();                    // player.cpp: the attached objects' world matrices
void SetWeaponLift(uintptr_t unit, const float offset[3]);  // player.cpp: a held weapon moved in the hand
bool ParrySparesBlood(uintptr_t player);            // parry.cpp: a blow now would be parried
bool PerfectDodgeSparesBlood(uintptr_t player);     // perfect_dodge.cpp: a blow now would be dodged
bool WeskerMoveInvulnerable(uintptr_t player);      // wesker_moves.cpp: player 1 in one of Wesker's attacks
bool Player1Dying();                                // dying.cpp
bool EnsurePlayerDamageHook();                      // player.cpp: the actors' SubHealth (knife.cpp reads the enemies' amounts)

namespace {

constexpr int kQueries = 4;
constexpr uintptr_t kQueryRvas[kQueries] = { 0x39A3E0, 0x39A430, 0x39A480, 0x39A4D0 };
using QueryFn = bool(__fastcall*)(void* self, void* edx, int button, int pad);
QueryFn g_query[kQueries] = {};
std::atomic<bool> g_padLog{ false };

// One line per (query, button, caller) and per second at most.
struct Seen {
    int query;
    int button;
    uintptr_t caller;
    DWORD last;
};
Seen g_seen[256];
int g_seenCount = 0;

void LogQuery(int query, int button, int pad, uintptr_t caller) {
    const DWORD now = GetTickCount();
    for (int i = 0; i < g_seenCount; ++i) {
        Seen& seen = g_seen[i];
        if (seen.query == query && seen.button == button && seen.caller == caller) {
            if (now - seen.last < 1000) return;
            seen.last = now;
            LOG("pad query %d button 0x%X pad %d true, caller RVA %X", query, button, pad,
                static_cast<unsigned>(caller - reinterpret_cast<uintptr_t>(mem::Base())));
            return;
        }
    }
    if (g_seenCount < 256) g_seen[g_seenCount++] = { query, button, caller, now };
    LOG("pad query %d button 0x%X pad %d true, caller RVA %X (new)", query, button, pad,
        static_cast<unsigned>(caller - reinterpret_cast<uintptr_t>(mem::Base())));
}

// Pad log (2026-10-15, keyboard / mouse): quick knife = game button 0x27 (read by the player at RVA 0x78B408 through
// query 3), aim = 0x0C held (RVA 0x78B2EC), fire = 0x0E. "Couteau rapide seulement sans viser": while aim is held the
// quick knife button reads as released, so a quick knife bound to the attack key only stabs out of aim.
constexpr int kQuickKnifeButton = 0x27;
constexpr int kAimButton = 0x0C;
std::atomic<bool> g_noKnifeInAim{ false };

// "Viser avec le couteau (touche)" (user, 2026-10-15: the game ignores the extra mouse buttons, wanted on Mouse 5 as in
// the modern REs): the knife aim key is game button 0x0D; on keyboard / mouse the player reads its buttons through a
// pad mask built every frame by RVA 0x86D510 (query 3 "held" with pad -1 for 0x0C aim -> 0x800, 0x0D knife -> 0x400,
// 0x0E fire -> 0x8000, 0x04 -> 0x4000, 0x15 -> 0x200, 0x0B -> 0x100, 0x16 -> 0x1000, 0x10 -> 0x2000; tables RVA
// 0xD47418 / 0xD472D8, one caller RVA 0x86E272): while the option's own key is held that query answers "held" for
// 0x0D, the game makes the edges itself.
constexpr int kKnifeAimButton = 0x0D;
std::atomic<bool> g_knifeAimKey{ false };
Feature* g_knifeAimFeature = nullptr;

bool KnifeAimKeyHeld() {
    return g_knifeAimKey.load(std::memory_order_relaxed) && g_knifeAimFeature &&
           (hotkeys::Held(g_knifeAimFeature->hotkey) || hotkeys::Held(g_knifeAimFeature->hotkey2));
}

// A pad's held mask bits (input + 0x51C, recorded 2026-10-23): aim 0x800, fire 0x8000 | 0x100, knife aim 0x400. A pad's
// buttons never go through the button queries, so the quick shots read and force them there (movement.cpp's stub).
constexpr uint32_t kPadAimBit = 0x800, kPadFireBits = 0x8100, kPadKnifeBit = 0x400;
bool PadMaskHas(uint32_t bits) {
    uint32_t mask = 0;
    return PadHeldMask(mask) && (mask & bits) != 0;
}

// "Tirer (touche)" (user, 2026-10-23: the pad's control types put fire where they want): a key or pad button of the
// user's choice held = the game's fire held (query 3, and query 0 for its first 50 ms; on a pad the fire bits).
std::atomic<bool> g_fireKey{ false };
Feature* g_fireKeyFeature = nullptr;
std::atomic<ULONGLONG> g_fireKeyEdgeUntil{ 0 };

bool FireKeyHeld() {
    return g_fireKey.load(std::memory_order_relaxed) && g_fireKeyFeature &&
           (hotkeys::Held(g_fireKeyFeature->hotkey) || hotkeys::Held(g_fireKeyFeature->hotkey2));
}

// "Couteau rapide (touche)" (user, 2026-11-06, in place of the melee prompt shortcut, "useless": the game's quick knife
// cannot be had on a pad, its attack trigger being the partner location's; "ticking it, the attack key out of aim stabs
// with the knife as with the mouse"). Out of aim, a press of the pad's attack trigger RT (or of the key / button chosen in
// the shortcut column instead) is played as the knife aim held + fire held, as the quick knife played as the knife aim
// (TickQuickAsAim): on a pad through the held mask (QuickShotPadMask), on the keyboard through the button queries. RT
// is kept from the game out of aim meanwhile (no partner location; dinput_hook.cpp source 2), aiming it fires.
std::atomic<bool> g_knifeKey{ false };
Feature* g_knifeKeyFeature = nullptr;
std::atomic<bool> g_knifeKeyEmulating{ false };

bool KnifeKeyBound() {
    return g_knifeKeyFeature && (g_knifeKeyFeature->hotkey.IsBound() || g_knifeKeyFeature->hotkey2.IsBound());
}
// The option on the pad's RT (no key chosen) while player 1 plays with a pad.
bool KnifeKeyOnRt() { return g_knifeKey.load(std::memory_order_relaxed) && !KnifeKeyBound() && Player1PadInUse(); }
bool KnifeKeyHeld() {
    if (!g_knifeKey.load(std::memory_order_relaxed) || !g_knifeKeyFeature || hotkeys::MenuOpen() || Re6OnGround()) return false;
    if (KnifeKeyOnRt() && Re6MeleeTakesFire()) return false;  // RT = the attack key: the RE6 melee first near an enemy
    if (KnifeKeyBound()) return hotkeys::Held(g_knifeKeyFeature->hotkey) || hotkeys::Held(g_knifeKeyFeature->hotkey2);
    return Player1PadInUse() && hotkeys::Held(hotkeys::kPadRT);
}

void TickFireKey() {
    static bool was = false;
    const bool held = FireKeyHeld();
    if (held && !was) g_fireKeyEdgeUntil = GetTickCount64() + 50;
    was = held;
}

// "Couteau rapide comme la visée au couteau" (user, 2026-10-20): the game's quick knife (+0x715 in the player's input
// block [P+0x2D64]+0x220) looks for an enemy and aims at it on its first press, and its draw keeps the camera still
// (recorded: the aim values input+0x444 / +0x454 / +0x470 stay 0, the camera yaw does not move) while the knife aim
// key's draw follows the mouse. Patching the quick knife flags (P+0x2D84 0x400 / 0x200) in the camera code was not
// enough. Instead the quick knife button never reaches the game: a press out of aim is played as the knife aim key
// held + fire held (knife aim + fire = the same stab request input+0x2B, held = chained stabs) until the key is
// released, and at least until the stab (action 0x27) has started so a tap still stabs.
constexpr int kFireButton = 0x0E;
// Run toggle (movement.cpp): while set, the run button (0x04, pad mask 0x4000) reads as held in query 3.
constexpr int kRunButton = 0x04;
std::atomic<bool> g_forceRun{ false };
// "Déplacement selon la direction (RE6)" (movement.cpp): any direction button held reads as forward (0x00), the others
// (0x01 back, 0x02 left, 0x03 right) as released, so the game walks / runs straight ahead where the trainer turns him.
std::atomic<bool> g_directionForward{ false };
// "Recharger (raccourci)" (user, 2026-10-20: on a pad the game reloads with aim + run, which "Pas de rechargement avec
// la course" removes): the reload key (game button 0x12, read by the player through query 3 at VA 0xB8B394 /
// 0xB8B37D whatever the control) reads as held for 100 ms after the shortcut.
constexpr int kReloadButton = 0x12;
std::atomic<ULONGLONG> g_reloadUntil{ 0 };
std::atomic<bool> g_reloadNotOnPrompt{ false };

// An action prompt shown for player 1 (pick up, open, melee, struggle...): sActionButton manager 0 (singleton [RVA
// 0xDB1794] + 0x40), slots +0x230 / +0x278: +4 shown, +0xC its cActionButton (licker.cpp, MODLOG session 11). User,
// 2026-10-23: the reload shortcut on the pad's action button reloaded while picking up an item.
bool ActionPromptShown() {
    uintptr_t manager = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), manager) || !manager) return false;
    for (uintptr_t slot : { manager + 0x40 + actors::Player1PromptOffset() + 0x230, manager + 0x40 + actors::Player1PromptOffset() + 0x278 }) {
        uint8_t shown = 0;
        uintptr_t button = 0;
        if (mem::Read(slot + 4, shown) && shown && mem::Read(slot + 0xC, button) && button) return true;
    }
    return false;
}

// Traversal shortcut (user, 2026-10-23: one shortcut for the jumps and other crossings only, never a pickup or a
// melee move). Recorded prompts (cActionButton type +0x18 = 1, kind +0x1C): 0x21 ladders (object classes, vtable RVA
// 0xBF3F78 story, 0xBE3020 Mercenaries) and windows (0xC17770), 0x24..0x28 the player's own (jump down, vault, jump
// forward, climb up / down); not crossings: melee stomp 0x1B, pickup 0x1D (handler +0x3C = item_get VA 0xB6D310), 0x31
// (seen during a weapon change), and the Mercenaries time bonus, kind 0x21 too but owned by its own object class
// (vtable RVA 0xBF0158, handler RVA 0x6D8BD0; the shortcut's log named it at each bonus broken, user 2026-10-23: it was
// first taken for the jump down). The shortcut presses the shown traversal prompt itself: cActionButton::
// OnPressed (RVA 0x7C3360, thiscall(button, player index) ret 4; needs +0xA active) sets its pressed flags and runs what
// the prompt does (handler +0x3C(owner, index, +0x40, +0x44, +0x48), or RequestAction +0x58 on +0x54), whatever button.
// 0x24..0x28: the player's own crossings (handlers VA 0xB6C5A0, 0xB6C700, 0xB6C7C0, 0xB6C890, 0xB6C950 in a row; 0x26 /
// 0x27 climb up / climb down a ledge, user 2026-10-23: not taken at first; the game accepts those two held, VA 0x76376B).
bool TraversalKind(uint32_t kind) { return kind == 0x21 || (kind >= 0x24 && kind <= 0x28); }
constexpr uintptr_t kTimeBonusVtable = 0xBF0158;  // RVA
constexpr uint32_t kTraversalIconA = 3;            // see TickTraversalIcon
// Kind 0x21 also holds the objects grabbed by holding the aim (user, 2026-10-30: 4-x's laser mirrors, uOmLaserMirror,
// like the turrets): their prompt's icon is the aim's (6), and the grab's own release prompt (type 2, icon 0x205 / 0x206)
// lets go as soon as the aim is not held; moved to A they were grabbed without the aim and let go at once. Only the
// game's action icon (1) is a traversal - every crossing recorded had it - or the icon put there by this option (3).
bool TraversalIcon(uint32_t icon) { return icon == 1 || icon == kTraversalIconA; }

// A prompt still in sActionButton's list ([RVA 0xDB1794] +0x3C array, +0x30 count). A shown slot may still name a prompt
// its object has just destroyed (user, 2026-10-27: the game crashed twice in its heap, VA 0x6DAE19, as a two-person
// jump replaced its prompts while "Franchissements sur A" wrote the icon of the shown one from the render thread): the
// icons are written on the game thread (TickPromptIcons) and only into live prompts.
bool LiveButton(uintptr_t button) {
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

// The traversal prompts shown for player 1 (both slots), active or not.
int ShownTraversalPrompts(uintptr_t (&out)[2]) {
    int n = 0;
    uintptr_t manager = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), manager) || !manager) return 0;
    for (uintptr_t slot : { manager + 0x40 + actors::Player1PromptOffset() + 0x230, manager + 0x40 + actors::Player1PromptOffset() + 0x278 }) {
        uint8_t shown = 0;
        uintptr_t button = 0, owner = 0, vtable = 0;
        uint32_t type = 0, kind = 0;
        uint32_t icon = 0;
        if (!mem::Read(slot + 4, shown) || !shown || !mem::Read(slot + 0xC, button) || !button || !mem::Read(button + 0x18, type) ||
            type != 1 || !mem::Read(button + 0x1C, kind) || !TraversalKind(kind) || !mem::Read(button + 0x20, icon) ||
            !TraversalIcon(icon))
            continue;
        if (mem::Read(button + 4, owner) && owner && mem::Read(owner, vtable) &&
            vtable == reinterpret_cast<uintptr_t>(mem::Rva(kTimeBonusVtable)))
            continue;  // the Mercenaries time bonus
        if (CoopSoloActionPrompt(owner)) continue;  // a co-op object done alone, on the action icon by its own option
        if (!LiveButton(button)) continue;
        out[n++] = button;
    }
    return n;
}

// A pickup prompt shown for player 1 (kind 0x1D, handler item_get VA 0xB6D310). The reload shortcut stops on these
// only (user, 2026-10-24: with melee on the fire key, a melee prompt has no reason to stop the reload).
bool PickupPromptShown() {
    uintptr_t manager = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), manager) || !manager) return false;
    for (uintptr_t slot : { manager + 0x40 + actors::Player1PromptOffset() + 0x230, manager + 0x40 + actors::Player1PromptOffset() + 0x278 }) {
        uint8_t shown = 0;
        uintptr_t button = 0, handler = 0;
        uint32_t kind = 0;
        if (!mem::Read(slot + 4, shown) || !shown || !mem::Read(slot + 0xC, button) || !button) continue;
        if ((mem::Read(button + 0x1C, kind) && kind == 0x1D) ||
            (mem::Read(button + 0x3C, handler) && handler == reinterpret_cast<uintptr_t>(mem::Rva(0x76D310))))
            return true;
    }
    return false;
}

uintptr_t TraversalPrompt() {
    uintptr_t buttons[2]{};
    const int n = ShownTraversalPrompts(buttons);
    for (int i = 0; i < n; ++i) {
        uint8_t active = 0;
        if (mem::Read(buttons[i] + 0xA, active) && active) return buttons[i];
    }
    return 0;
}

// The button the traversal prompts ask for (user, 2026-10-23: A, the trainer's shortcut, instead of X). The slot's
// required button mask is the table [VA 0xF58E50 + icon * 4] of the prompt's icon +0x20 (set at VA 0x7674A3 when the
// prompt appears, again at VA 0x766F78 whenever the icon changed, traced): writing the icon changes the glyph shown
// and the button the game itself accepts. Icons 1..4 = masks 0x8000 (the game's action: X on the user's pad), 0x1000,
// 0x4000, 0x2000 (the partner button); 9..12 and 22..25 repeat them. Icon 3 (mask 0x4000) shows A on the user's pad
// (tried through icons 2..4). Option off: a shown traversal prompt still on icon 3 gets its own icon back (a shown
// prompt is alive), the one remembered or else 1, the game's action: every traversal prompt recorded had icon 1, and a
// reloaded trainer no longer knows which prompts its previous instance changed (user, 2026-10-23: unchecked = X).
std::atomic<uint32_t> g_traversalIcon{ 0 };  // 0 = the game's
std::atomic<bool> g_allOnA{ false };       // "Toutes les actions sur A": every prompt is handled there
std::atomic<uint32_t> g_iconTest{ 0 };     // research: an icon put on every shown action prompt
std::atomic<bool> g_iconTestOn{ false };
std::vector<std::pair<uintptr_t, uint32_t>> g_iconOriginals;

void TickTraversalIcon() {
    if (!actors::InGame() || g_iconTestOn.load(std::memory_order_relaxed)) return;
    // A pad's only (user, 2026-10-30: "why does it touch the keyboard and mouse"): on the keyboard the prompts keep the
    // game's icons, given back as when the option is off. With "Toutes les actions sur A" the action is already A: their
    // own icons too.
    const uint32_t icon = Player1PadInUse() && !g_allOnA.load(std::memory_order_relaxed)
                              ? g_traversalIcon.load(std::memory_order_relaxed) : 0u;
    uintptr_t buttons[2]{};
    const int n = ShownTraversalPrompts(buttons);
    for (int i = 0; i < n; ++i) {
        const uintptr_t button = buttons[i];
        uint32_t current = 0;
        if (!mem::Read(button + 0x20, current)) continue;
        auto known = std::find_if(g_iconOriginals.begin(), g_iconOriginals.end(),
                                  [button](const auto& e) { return e.first == button; });
        if (icon) {
            if (current == icon) continue;
            if (known == g_iconOriginals.end()) g_iconOriginals.push_back({ button, current });
            mem::Write<uint32_t>(button + 0x20, icon);
        } else {
            const uint32_t original = known != g_iconOriginals.end() ? known->second : 1u;
            if (current == kTraversalIconA && original != kTraversalIconA) mem::Write<uint32_t>(button + 0x20, original);
            if (known != g_iconOriginals.end()) g_iconOriginals.erase(known);
        }
    }
}

// The door's kick with "Franchissements sur A" (user, 2026-11-06: "the door opens on A, the kick stays on X"): a door's
// prompt is a traversal one (kind 0x21), its kick is not: while the door opens the game makes a hidden prompt (kind 2,
// icon 1, handler VA 0xAC0330, registered at VA 0xAC289A, alive 1.5 s, research/door_kick/rec2.txt; an execute trace of the
// handler had esi = that prompt, edi = player 1) on the action button. It gets the traversal's icon, while player 1 is in
// the door's event (action 0x80000000). Live prompts only, on the game thread (see LiveButton). User-verified 2026-11-08.
void TickDoorKickIcon() {
    const uint32_t icon = Player1PadInUse() && !g_allOnA.load(std::memory_order_relaxed)
                              ? g_traversalIcon.load(std::memory_order_relaxed) : 0u;
    if (!icon || !actors::InGame() || g_iconTestOn.load(std::memory_order_relaxed)) return;
    const uintptr_t p = actors::Player(0);
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action) || action != 0x80000000) return;
    uintptr_t system = 0, array = 0;
    uint32_t count = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), system) || !system || !mem::Read(system + 0x30, count) ||
        !mem::Read(system + 0x3C, array) || !array || count > 4000)
        return;
    const uintptr_t kick = reinterpret_cast<uintptr_t>(mem::Rva(0x6C0330));
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t button = 0, handler = 0;
        uint32_t current = 0;
        if (!mem::Read(array + 4 * i, button) || !button || !mem::Read(button + 0x3C, handler) || handler != kick ||
            !mem::Read(button + 0x20, current) || current == icon)
            continue;
        mem::Write<uint32_t>(button + 0x20, icon);
        LOG("traversal on A: the door's kick prompt %08X on A", static_cast<unsigned>(button));
    }
}

// "Toutes les actions sur A (manette)" (user, 2026-10-23: A the action button, X the run button with its quick turn
// and reload while aiming, as in RE4): the XInput hook swaps A and X for the game, so the functions really trade
// places. The prompts keep their icons and masks (the action's 0x8000, which the game now gets from the pad's A); only
// their drawing is swapped, 1 <-> 3 (and their variants 9 <-> 11, 22 <-> 24), so they show A for the action and X for
// the run (HookGlyph, SetAllOnA). Until 2026-11-06 the icons were written on the shown prompts with those entries of
// the icon -> mask table (VA 0xF58E50) swapped: hidden prompts (a door's kick) then asked for the pad's X.

uint32_t SwappedIcon(uint32_t icon) {
    switch (icon) {
    case 1: return 3;
    case 3: return 1;
    case 9: return 11;
    case 11: return 9;
    case 22: return 24;
    case 24: return 22;
    default: return 0;
    }
}

// Research (user, 2026-10-23: which icon is the fire button, for the melee prompts): the chosen icon on every shown
// action prompt (type 1), their own icon given back when the test is set back to none.
std::vector<std::pair<uintptr_t, uint32_t>> g_iconTestOriginals;

void TickIconTest() {
    const bool test = g_iconTestOn.load(std::memory_order_relaxed);
    const uint32_t icon = g_iconTest.load(std::memory_order_relaxed);
    if (!actors::InGame() || (!test && g_iconTestOriginals.empty())) return;
    uintptr_t manager = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), manager) || !manager) return;
    for (uintptr_t slot : { manager + 0x40 + actors::Player1PromptOffset() + 0x230, manager + 0x40 + actors::Player1PromptOffset() + 0x278 }) {
        uint8_t shown = 0;
        uintptr_t button = 0;
        uint32_t type = 0, current = 0;
        if (!mem::Read(slot + 4, shown) || !shown || !mem::Read(slot + 0xC, button) || !button || !mem::Read(button + 0x18, type) ||
            type != 1 || !mem::Read(button + 0x20, current) || !LiveButton(button))
            continue;
        auto known = std::find_if(g_iconTestOriginals.begin(), g_iconTestOriginals.end(),
                                  [button](const auto& e) { return e.first == button; });
        if (test) {
            if (current == icon) continue;
            if (known == g_iconTestOriginals.end()) g_iconTestOriginals.push_back({ button, current });
            mem::Write<uint32_t>(button + 0x20, icon);
        } else if (known != g_iconTestOriginals.end()) {
            if (current != known->second) mem::Write<uint32_t>(button + 0x20, known->second);
            g_iconTestOriginals.erase(known);
        }
    }
}

// Since 2026-11-06 the prompts keep their icons and the table its masks, only the drawing is swapped (HookGlyph: the
// icon -> sprite query VA 0x7633C0, which with a pad returns the icon, used as an index of sActionButton +0xB80 by its
// three callers VA 0x764E57 / 0x764EAE / 0x764EEB; drawing only): with A and X swapped for the game every action prompt,
// the hidden ones too, is on the pad's A (a door's kick user-verified 2026-11-08; the former way, table and icons
// swapped, is in git before 86d3e06).
bool EnsureGlyphHook();  // below

bool SetAllOnA(bool on) {
    if (on && !EnsureGlyphHook()) return false;
    g_allOnA = on;
    hooks::SwapPadAX(on);
    return true;
}

bool PressTraversalPrompt() {
    const uintptr_t button = TraversalPrompt();
    if (!button) return false;
    using OnPressedFn = void(__fastcall*)(void* button, void* edx, int player);
    uint32_t kind = 0;
    uintptr_t owner = 0, vtable = 0, handler = 0;
    mem::Read(button + 0x1C, kind);
    mem::Read(button + 0x3C, handler);
    if (mem::Read(button + 4, owner) && owner) mem::Read(owner, vtable);
    const unsigned base = static_cast<unsigned>(reinterpret_cast<uintptr_t>(mem::Base()));
    __try {
        reinterpret_cast<OnPressedFn>(mem::Rva(0x7C3360))(reinterpret_cast<void*>(button), nullptr,
                                                          actors::SlotOf(actors::Player(0)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("traversal shortcut: prompt %08X (kind %X) faulted", static_cast<unsigned>(button), kind);
        return false;
    }
    // Owner class and handler logged so that a wrong prompt can be excluded (as the time bonus).
    LOG("traversal shortcut: prompt %08X pressed (kind %X, owner vtable RVA %06X, handler RVA %06X)",
        static_cast<unsigned>(button), kind, vtable ? static_cast<unsigned>(vtable) - base : 0u,
        handler ? static_cast<unsigned>(handler) - base : 0u);
    return true;
}

bool RawHeld(int button, int pad);  // below: a game button as really held

// Melee shortcut and the fire button on the melee prompts (user, 2026-10-23: the attack key for the melee attacks, not
// the action key, and never while aiming). The melee prompts on Majini share the callback +0x5C VA 0x862530 (stomp
// kind 0x1B, kind 0x19...); pickups, crossings and the time bonus have none or their own. The Executioner's kept the
// action button (user, 2026-10-24): kind 0x19 too, but callback RVA 0x46BC10 and handler RVA 0x75E020 (diag.prompt_log;
// every prompt's owner +4 is the player). A melee prompt is an action prompt (type 1) of kind 0x19 / 0x1B, or with one
// of those callbacks, or with a handler in the game's attack handler table (RVA 0x75D9F0.., one per attack action,
// MODLOG session 13; the crossings' handlers RVA 0x76C5A0..0x76C950, the pickup's 0x76D310 are elsewhere).
constexpr uintptr_t kMeleePromptCallback = 0x462530;  // RVA
constexpr uintptr_t kExecutionerMeleeCallback = 0x46BC10;  // RVA
constexpr uintptr_t kAttackHandlersBegin = 0x75D9F0, kAttackHandlersEnd = 0x75E200;  // RVA

// A crossing prompt (climb up, jump down: kinds 0x24-0x28) while an execution from a height is ready (user, 2026-11-11: "replace it by
// the attack prompt when the attack prompt option is on", wesker_moves.cpp TickEnvHeightReady): a melee prompt.
bool EnvCrossingPrompt(uint32_t kind) { return kind >= 0x24 && kind <= 0x28 && g_envHeightReady.load(std::memory_order_relaxed); }

bool IsMeleePrompt(uintptr_t button) {
    uint32_t type = 0, kind = 0;
    uintptr_t callback = 0, handler = 0;
    if (!mem::Read(button + 0x18, type) || type != 1) return false;
    if (mem::Read(button + 0x1C, kind) && (kind == 0x19 || kind == 0x1B || EnvCrossingPrompt(kind))) return true;
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    if (mem::Read(button + 0x5C, callback) && (callback == base + kMeleePromptCallback || callback == base + kExecutionerMeleeCallback))
        return true;
    return mem::Read(button + 0x3C, handler) && handler >= base + kAttackHandlersBegin && handler < base + kAttackHandlersEnd;
}
constexpr uint32_t kFireIcon = 6;                      // mask 0x800: the fire button on the user's pad (icon test)
constexpr uint32_t kKeyboardMeleeIcon = 8;             // mask 0x200: no keyboard key gives it; drawn as the fire key
// A melee prompt's own icon when it is not remembered: the game's action (every melee prompt recorded had icon 1 or 9,
// both mask 0x8000). A trainer reloaded while a prompt showed ours never saw its own (user, 2026-10-25: Jill's throw
// stayed on icon 8 for the whole fight, its plain glyph Shift and no key taking it, even with the option off).
constexpr uint32_t kGameActionIcon = 1;
std::atomic<bool> g_meleeKeyboardIcon{ false };        // a melee prompt shown with that icon on the keyboard
// While aiming too (user, 2026-11-10: "while aiming the combat prompt goes back to the action key even with the option on; I want it
// to stay on the attack key while aiming as in RE6, the combat prompts before the shot"; and "I cannot do the Ground or the throat
// cut in the knife aim": its fire stabbed): a melee prompt shown in the aim keeps the fire icon, the fire key presses it
// (TickMeleeIcon, keyboard and pad), and fire is hidden from the game's other reads (no shot, no stab) while one is shown and until the
// key is let go after a press.
std::atomic<bool> g_firePromptInAim{ false };
std::atomic<bool> g_fireHeldAfterPrompt{ false };
std::atomic<bool> g_meleeOnFire{ false };
std::vector<std::pair<uintptr_t, uint32_t>> g_meleeOriginals;
// A boss's own melee prompt (owner +4 the enemy: Jill's throw, kind 0x19, callback RVA 0x53CFC0, registered by her at
// RVA 0x547F09 with type / icon 0x101 = action prompt, icon 1; user 2026-10-25: on the fire key, the shortcut's OnPressed
// did nothing, unlike the Executioner's, which is player 1's): pressed by the game itself, its icon given back and the
// prompt buttons (0xF, 0x10, 0xC: all the keyboard detector knows) held for the prompt reads (callers RVA 0x360000..
// 0x370000) until the game presses it (+0xC, the detector's own press: RVA 0x3675B7 the player's RVA 0x75CB20, then
// OnPressed), 150 ms at most.
std::atomic<uintptr_t> g_bossPromptPressed{ 0 };
std::atomic<ULONGLONG> g_bossPressUntil{ 0 };

bool BossPrompt(uintptr_t button) {
    uintptr_t owner = 0;
    return mem::Read(button + 4, owner) && owner && owner != actors::Player(0) && owner != actors::Player(1);
}

int ShownMeleePrompts(uintptr_t (&out)[2]) {
    int n = 0;
    uintptr_t manager = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), manager) || !manager) return 0;
    for (uintptr_t slot : { manager + 0x40 + actors::Player1PromptOffset() + 0x230, manager + 0x40 + actors::Player1PromptOffset() + 0x278 }) {
        uint8_t shown = 0;
        uintptr_t button = 0;
        if (mem::Read(slot + 4, shown) && shown && mem::Read(slot + 0xC, button) && button && IsMeleePrompt(button) &&
            LiveButton(button))
            out[n++] = button;
    }
    return n;
}

// Research (diag.prompt_log): each prompt newly shown for player 1, with its type, kind, icon, owner class (vtable RVA),
// handler +0x3C and callback +0x5C.
std::atomic<bool> g_promptLog{ false };

void TickPromptLog() {
    static uintptr_t last[2] = {};
    if (!g_promptLog.load(std::memory_order_relaxed) || !actors::InGame()) return;
    uintptr_t manager = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), manager) || !manager) return;
    const uintptr_t slots[2] = { manager + 0x40 + actors::Player1PromptOffset() + 0x230, manager + 0x40 + actors::Player1PromptOffset() + 0x278 };
    const unsigned base = static_cast<unsigned>(reinterpret_cast<uintptr_t>(mem::Base()));
    for (int i = 0; i < 2; ++i) {
        uint8_t shown = 0;
        uintptr_t button = 0;
        if (!mem::Read(slots[i] + 4, shown) || !shown || !mem::Read(slots[i] + 0xC, button)) button = 0;
        if (button == last[i]) continue;
        last[i] = button;
        if (!button) continue;
        uint32_t type = 0, kind = 0, icon = 0;
        uintptr_t owner = 0, vtable = 0, handler = 0, callback = 0;
        mem::Read(button + 0x18, type);
        mem::Read(button + 0x1C, kind);
        mem::Read(button + 0x20, icon);
        mem::Read(button + 4, owner);
        if (owner) mem::Read(owner, vtable);
        mem::Read(button + 0x3C, handler);
        mem::Read(button + 0x5C, callback);
        const auto enemies = actors::Enemies();
        const bool enemy = owner && std::find(enemies.begin(), enemies.end(), owner) != enemies.end();
        const auto rva = [base](uintptr_t va) { return va > base ? static_cast<unsigned>(va) - base : 0u; };
        LOG("prompt log: slot %d prompt %08X type %u kind %X icon %u owner %08X (vtable RVA %06X, %s) handler RVA %06X "
            "callback RVA %06X%s",
            i, static_cast<unsigned>(button), type, kind, icon, static_cast<unsigned>(owner), rva(vtable),
            owner == actors::Player(0) ? "player 1" : enemy ? "enemy" : "other", rva(handler), rva(callback),
            IsMeleePrompt(button) ? " = melee" : "");
    }
}

bool MeleePromptShown() {
    uintptr_t buttons[2]{};
    return ShownMeleePrompts(buttons) > 0;
}

// Player 1 aiming (a gun or the knife): an aim action, or an aim button held.
bool Player1Aiming() {
    const uintptr_t p = actors::Player(0);
    uint8_t pad = 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x2D9C, pad) || !mem::Read(p + 0x10E0, action)) return false;
    if (action == 0x1B || action == 0x1C || action == 0x1E || (action >= 0x22 && action <= 0x27 && action != 0x24)) return true;
    return RawHeld(kAimButton, pad) || RawHeld(kAimButton, -1) || RawHeld(kKnifeAimButton, pad) ||
           RawHeld(kKnifeAimButton, -1) || KnifeAimKeyHeld() || PadMaskHas(kPadAimBit | kPadKnifeBit);
}

bool PressMeleePrompt() {
    uintptr_t buttons[2]{};
    const int n = ShownMeleePrompts(buttons);
    for (int i = 0; i < n; ++i) {
        uint8_t active = 0;
        uint32_t kind = 0;
        if (!mem::Read(buttons[i] + 0xA, active) || !active) continue;
        mem::Read(buttons[i] + 0x1C, kind);
        if (BossPrompt(buttons[i])) {
            const auto known = std::find_if(g_meleeOriginals.begin(), g_meleeOriginals.end(),
                                            [&](const auto& e) { return e.first == buttons[i]; });
            mem::Write<uint32_t>(buttons[i] + 0x20, known != g_meleeOriginals.end() ? known->second : kGameActionIcon);
            g_bossPromptPressed = buttons[i];
            g_bossPressUntil = GetTickCount64() + 150;
            LOG("melee shortcut: boss prompt %08X (kind %X) pressed through its own button", static_cast<unsigned>(buttons[i]),
                kind);
            return true;
        }
        using OnPressedFn = void(__fastcall*)(void* button, void* edx, int player);
        __try {
            reinterpret_cast<OnPressedFn>(mem::Rva(0x7C3360))(reinterpret_cast<void*>(buttons[i]), nullptr,
                                                              actors::SlotOf(actors::Player(0)));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        LOG("melee shortcut: prompt %08X pressed (kind %X)", static_cast<unsigned>(buttons[i]), kind);
        return true;
    }
    if (n) LOG("melee shortcut: %d melee prompt(s) shown, none active", n);
    return false;
}

// The keyboard glyph of a prompt icon (user, 2026-10-23: a left click on the keyboard melee prompts, none of the 30
// icons drew it). The HUD asks sActionButton VA 0x7633C0 (thiscall(icon, index, pad) ret 0xC, called at VA 0x764E57
// every frame): with a pad it returns the icon, on the keyboard VA 0x79B8F0 turns the icon into a game button (0xF,
// 0x10, 0xC, 4...; never fire 0xE), that button into its bound key (VA 0x7B6E10 on [RVA 0xDB2158]) and the key into
// a glyph (VA 0x79A660 on sBioPad [RVA 0xE49C40]); act_btn_360 holds the mouse glyphs (left, right, middle, wheel).
// Icon 8 on a melee prompt (keyboard, "Corps à corps sur la touche de tir") is drawn as the fire key that way.
using GlyphFn = int(__fastcall*)(void* self, void* edx, int icon, int index, int pad);
GlyphFn g_glyphOriginal = nullptr;

int FireKeyGlyph() {
    using KeyOfFn = int(__fastcall*)(void* config, void* edx, int button);
    using GlyphOfFn = int(__fastcall*)(void* pad, void* edx, int key);
    uintptr_t config = 0, bioPad = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), config) || !config ||
        !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C40)), bioPad) || !bioPad)
        return 0;
    __try {
        const int key = reinterpret_cast<KeyOfFn>(mem::Rva(0x3B6E10))(reinterpret_cast<void*>(config), nullptr, kFireButton);
        return key ? reinterpret_cast<GlyphOfFn>(mem::Rva(0x39A660))(reinterpret_cast<void*>(bioPad), nullptr, key) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int __fastcall HookGlyph(void* self, void* edx, int icon, int index, int pad) {
    // "Toutes les actions sur A" on a pad: the icons drawn swapped, the prompts' own icons and masks untouched.
    if (g_allOnA.load(std::memory_order_relaxed) && icon > 0 && Player1PadInUse())
        if (const uint32_t swapped = SwappedIcon(static_cast<uint32_t>(icon))) icon = static_cast<int>(swapped);
    if (icon == static_cast<int>(kKeyboardMeleeIcon) && index == 0 && g_meleeKeyboardIcon.load(std::memory_order_relaxed) &&
        !Player1PadInUse())
        if (const int glyph = FireKeyGlyph()) return glyph;
    return g_glyphOriginal(self, edx, icon, index, pad);
}

bool EnsureGlyphHook() {
    static bool hooked = false;
    if (hooked) return true;
    // mov eax, [esp+0Ch]; mov ecx, [sBioPad]
    const uint8_t expected[] = { 0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x0D };
    uint8_t* target = mem::Rva(0x3633C0);
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("prompt glyph hook: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookGlyph), reinterpret_cast<void**>(&g_glyphOriginal)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("prompt glyph hook: MinHook failed");
        return false;
    }
    hooked = true;
    LOG("prompt glyph hook installed");
    return true;
}

// The fire icon (and so the fire button for the game) on the shown melee prompts while player 1 is not aiming; their
// own icon while he aims (the fire button shoots) or with the option off. The icon remembered is refreshed whenever a
// prompt is seen without ours ("Toutes les actions sur A" may have swapped it). Pad only (user, 2026-10-23: with the
// mouse icon 6 drew the right click, the aim): the keyboard prompt detector (VA 0x7637A4..) only knows game buttons
// 0xF, 0x10 and 0xC, not fire, so no prompt can ask for the left click; there the prompts keep their icon and a left
// click out of aim presses the shown melee prompt itself.
void TickMeleeIcon() {
    const bool on = g_meleeOnFire.load(std::memory_order_relaxed);
    if (!actors::InGame() || g_iconTestOn.load(std::memory_order_relaxed)) {
        g_firePromptInAim = false;
        g_fireHeldAfterPrompt = false;
        return;
    }
    const bool pad = Player1PadInUse(), aiming = Player1Aiming();
    static bool clickWas = false;
    // The keyboard out of aim, and both in the aim (the pad's RT out of aim is the game's, through the prompt's fire icon).
    uint8_t padIndex = 0;
    if (const uintptr_t p = actors::Player(0)) mem::Read(p + 0x2D9C, padIndex);
    const bool fireRaw = RawHeld(kFireButton, -1) || RawHeld(kFireButton, padIndex) || FireKeyHeld();
    const bool click = (!pad || aiming) && fireRaw;
    if (on && click && !clickWas && PressMeleePrompt() && aiming) g_fireHeldAfterPrompt = true;
    clickWas = click;
    if (!fireRaw) g_fireHeldAfterPrompt = false;
    const bool fireIcon = on;
    const uint32_t ourIcon = pad ? kFireIcon : kKeyboardMeleeIcon;  // keyboard: drawn as the fire key (HookGlyph)
    uintptr_t buttons[2]{};
    const int n = ShownMeleePrompts(buttons);
    for (int i = 0; i < n; ++i) {
        const uintptr_t button = buttons[i];
        uint32_t current = 0;
        if (!mem::Read(button + 0x20, current)) continue;
        auto known = std::find_if(g_meleeOriginals.begin(), g_meleeOriginals.end(),
                                  [button](const auto& e) { return e.first == button; });
        const bool ours = current == kFireIcon || current == kKeyboardMeleeIcon;
        if (!ours) {
            if (known != g_meleeOriginals.end()) known->second = current;
            else if (on) known = g_meleeOriginals.insert(g_meleeOriginals.end(), { button, current });
        }
        if (button == g_bossPromptPressed.load(std::memory_order_relaxed)) {
            uint8_t pressed = 0;
            if (mem::Read(button + 0xC, pressed) && pressed) {
                LOG("melee shortcut: boss prompt %08X pressed by the game", static_cast<unsigned>(button));
                g_bossPromptPressed = 0;
            } else if (GetTickCount64() < g_bossPressUntil.load(std::memory_order_relaxed)) {
                continue;  // its own icon while its own button is held
            } else {
                LOG("melee shortcut: boss prompt %08X not pressed by the game", static_cast<unsigned>(button));
                g_bossPromptPressed = 0;
            }
        }
        if (fireIcon) {
            if (current != ourIcon) mem::Write<uint32_t>(button + 0x20, ourIcon);
        } else {
            if (ours) mem::Write<uint32_t>(button + 0x20, known != g_meleeOriginals.end() ? known->second : kGameActionIcon);
            if (!on && known != g_meleeOriginals.end()) g_meleeOriginals.erase(known);
        }
    }
    // A crossing prompt no longer an execution's (the enemy gone from the edge) with the fire icon: its own given back.
    uintptr_t manager = 0;
    if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), manager) && manager) {
        for (uintptr_t slot : { manager + 0x40 + actors::Player1PromptOffset() + 0x230, manager + 0x40 + actors::Player1PromptOffset() + 0x278 }) {
            uint8_t shown = 0;
            uintptr_t button = 0;
            uint32_t type = 0, kind = 0, current = 0;
            if (!mem::Read(slot + 4, shown) || !shown || !mem::Read(slot + 0xC, button) || !button || !mem::Read(button + 0x18, type) ||
                type != 1 || !mem::Read(button + 0x1C, kind) || kind < 0x24 || kind > 0x28 || EnvCrossingPrompt(kind) ||
                !mem::Read(button + 0x20, current) || (current != kFireIcon && current != kKeyboardMeleeIcon) || !LiveButton(button))
                continue;
            auto known = std::find_if(g_meleeOriginals.begin(), g_meleeOriginals.end(), [button](const auto& e) { return e.first == button; });
            mem::Write<uint32_t>(button + 0x20, known != g_meleeOriginals.end() ? known->second : kGameActionIcon);
            if (known != g_meleeOriginals.end()) g_meleeOriginals.erase(known);
        }
    }
    g_meleeKeyboardIcon = fireIcon && !pad && n > 0;
    g_firePromptInAim = on && aiming && n > 0;
}
std::atomic<bool> g_quickAsAim{ false };
std::atomic<bool> g_emulating{ false };

bool RawHeld(int button, int pad) {
    // Without the hooks, the game's own query 3 (as GameButtonHeld).
    const QueryFn query = g_query[3] ? g_query[3] : reinterpret_cast<QueryFn>(mem::Rva(0x39A4D0));
    __try {
        return query(nullptr, nullptr, button, pad);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// "Tir sans viser" (user, 2026-10-21: like the quick knife, the attack key out of aim raises the weapon and fires, the
// camera / mouse aiming where it shoots). A press of fire (0x0E) out of aim is played as aim (0x0C) held for as long as
// the key is held; fire reads as released until the weapon is up (aim loop 0x1C, or firing 0x1E), so the game gets its
// press edge then (it shot nothing when fire was already held at the aim start). A tap still fires once: fire stays
// held until the shot (0x1E) started. Released after the shot, the weapon goes down. The quick knife on the same key
// (the user's binding) is ignored meanwhile. Not with the knife aim held (knife aim + fire = a stab).
std::atomic<bool> g_quickShot{ false };
std::atomic<bool> g_shotAim{ false };   // aim answered held
std::atomic<int> g_shotFire{ 0 };       // 0 untouched, 1 released, 2 held
bool g_shotSeen = false;

// "Tir rapide façon RE6" (user, 2026-10-21): the same shot, aimed by itself at the nearest enemy within 8 m (those in
// front first: one at the side or behind wins when 1.5 times nearer), player 1 and his aim camera turning to it every
// frame (yaw: unit quaternion +0x40, P+0x1024, aim yaw P+0x2A10, aim camera yaw P+0x2A24; once the camera faces it, the
// camera eye's direction to it, the shoulder offset), the mouse and the pad's right stick hidden from the game
// meanwhile; it fires as soon as the weapon is up (0x1C), one shot per press. Exclusive with "Tir sans viser".
// Tries to make the bullet land (user: too slow, still missed, "on triche"): waiting for the camera ray on the target
// (and 150 ms more), correcting the aim pitch P+0x2908 (> 0 looks down) towards its chest - dropped. Forcing a state
// after the game's OnHit lost against the bullet's own reaction a frame later (DmgHead 0x6D motion 0x50C, 10 ms later
// DmgArm 0x6E 0x51A) - re-forcing it worked but needed the hit.
// The player cannot tell whether the bullet landed (user, "on triche"): no laser and no crosshair during the shot
// (patch_features.cpp aim marker calls). A bullet that lands does its own damage and reaction (the shotgun's blow-off:
// a first version made the target ignore the bullets and wrote a recorded damage, the enemies no longer flew, user);
// a miss is taken as a hit 150 ms after the shot: the health player 1's last bullet with that weapon took off an enemy
// (not a head hit nor a kill; recorded in ApplyDamage from every bullet, quick shots too, kept in the data
// folder's quick_shot_damage.ini across loads), attacker index +0x136C set to player 1's pad as OnHit does (the kill's credit);
// a lethal miss writes 0 and asks a short flinch, after which its AI picks the death (parry.cpp MajiniDieNow; health 0
// written alone: a Majini walked on for up to 5.7 s, recorded). Then, with a handgun or a submachine gun only, a living
// Majini not on the ground at the hit (lying / getting up 0x6A / 0x6B, falls, hit on the ground: knife.cpp
// EnemyOnGround; kneeling 0x71 does get it) takes the state chosen in "État de
// l'ennemi touché" (parry.cpp ForceMajiniState), and for 0.3 s after a hit it is forced again (4 times at most) when the
// bullet's own reaction, a frame after its OnHit, replaced it (user-verified earlier: "de manière fluide"). Recorded:
// every forced state became the stagger with its prompt, from attacks too.
// After the shot the aim ends by itself (user: as in RE6, no shooting on; holding the key kept Chris aiming and the laser
// came back, showing he was not on the target): fire reads as released until the key is let go, and the laser /
// crosshair stay hidden until Chris is out of the aim actions 0x1B..0x1F (2.5 s at most). A new press is a new shot.
// Times are compared signed: several frames share a GetTickCount value, and "now - (now | 1)" read as 49 days.
std::atomic<bool> g_quickShotRe6{ false };
std::atomic<int> g_shotState{ 0 };
std::atomic<uintptr_t> g_shotTarget{ 0 };
std::atomic<DWORD> g_shotHitTick{ 0 };     // when a bullet hit the target (0 = not yet)
std::atomic<bool> g_shotDownAtHit{ false }; // the target was on the ground when the bullet hit
DWORD g_shotFiredTick = 0;                 // when 0x1E was first seen (0 = not yet)
bool g_shotResolved = false;
bool g_shotForcedState = false;            // the state was put: the bullet's reaction gets it back
int g_shotReforces = 0;
constexpr DWORD kShotMissMs = 150;
constexpr DWORD kShotWatchMs = 300;
constexpr int kShotMaxReforces = 4;
std::atomic<bool> g_shotAfter{ false };    // the shot is over: fire released, markers hidden, until the aim is over
std::atomic<bool> g_shotRe6{ false };      // the shot in progress is an RE6 one (not while dying: the plain quick shot)
DWORD g_shotAfterTick = 0;
std::atomic<int> g_bulletDamage[0x40]{};   // per weapon item 0x100..0x13F
std::atomic<int> g_bulletHits[0x40]{};     // how many hits (shotgun pellets) that shot made, the blood sprays of a miss
std::atomic<uint32_t> g_bloodFlags[0x40]{}; // the hit flags player 1's real hits with that weapon spawned blood with

// RE6's quick shots by weapon (user, 2026-11-09: "the quick shot's animations to vary the front and the one made when he turns round";
// recorded on Chris and Leon: BhA 56 a few frames, then the weapon's shot; re6_evade.lmt 62-69, tools/import_re6_motions.py): the front
// and half-turn slots, the frame each one's shot is marked at (its event bit 30) and the one its cancel window opens at (bit 31). The
// game's own fire is held back until that frame; a target behind (over 110 degrees) takes the half-turn, Chris turned to it over its
// first 8 frames. Handguns BhB D8 / DE, magnums MrB D2 / D6, rifles and submachine guns ArB D2 / D4 (a burst in RE6: the automatic
// fire held), shotguns and the rifles with a scope SpB D2 / D4 (user: "the same animation for the sniper without the pump").
struct Re6Shot {
    uint16_t slot;
    float fire, cancel;
};
struct Re6ShotPair {
    Re6Shot front, back;
};
constexpr Re6ShotPair kHandgunShots = { { 62, 8.0f, 30.0f }, { 63, 14.0f, 45.0f } };
constexpr Re6ShotPair kMagnumShots = { { 64, 10.0f, 65.0f }, { 65, 26.0f, 85.0f } };
constexpr Re6ShotPair kRifleShots = { { 66, 4.0f, 50.0f }, { 67, 24.0f, 60.0f } };
constexpr Re6ShotPair kShotgunShots = { { 68, 10.0f, 90.0f }, { 69, 28.0f, 100.0f } };
// Without its pump (user, 2026-11-10): the scoped rifles, the grenade launcher, the Jail Breaker (no pump in RE5); 68 frames / 53 taken out
// (the gun brought up, racked, lowered), its cancel that much earlier.
constexpr Re6ShotPair kNoPumpShots = { { 70, 10.0f, 22.0f }, { 71, 28.0f, 47.0f } };

const Re6ShotPair* Re6ShotsOf(uint16_t item) {
    switch (item) {
    case 0x102: case 0x110: case 0x111: case 0x112: case 0x11E: case 0x11F: case 0x129: return &kHandgunShots;
    case 0x10B: case 0x11A: case 0x11B: return &kMagnumShots;
    case 0x103: case 0x113: case 0x109: case 0x11D: return &kRifleShots;  // VZ61, MP5, SIG 556, AK-74
    case 0x104: case 0x116: return &kShotgunShots;  // Ithaca M37, M3
    case 0x117: case 0x105: case 0x11C: case 0x120: case 0x10C: return &kNoPumpShots;  // Jail Breaker, S75, PSG-1, Dragunov, grenade launcher
    // The Hydra its own one-handed shot (user, 2026-11-10: "the Hydra, though a shotgun, must use its native one-handed animation").
    default: return nullptr;  // the others keep the Hydra's one-handed shot
    }
}

constexpr bool kRe6ShotMotions = true;  // see the begin's note
std::atomic<int> g_shotSlot{ 0 };  // the RE6 shot playing (0: none)
// The game's aim let go once its fire was cut (user, 2026-11-10: "the animation loops a second time, the handgun too": the aim, held
// until the shot was resolved, showed again when the RE6 motion ended - logged in the aim's end 0x1D then); the shot's resolution goes on.
std::atomic<bool> g_shotAimDropped{ false };

// A player's action request (its vtable +0xDC, as swaps.cpp / wesker_moves.cpp ask theirs).
bool RequestPlayerAction(uintptr_t p, int action) {
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    if (!p || !mem::Read(p, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return false;
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(p), nullptr, action, 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}
std::atomic<float> g_shotFireFrame{ 0.0f }, g_shotCancelFrame{ 0.0f };
bool g_shotTurn = false;  // the half-turn: Chris turned from g_shotTurnFrom to the target
float g_shotTurnFrom = 0.0f;

// Handguns (PST) and submachine guns: the items whose quick shot puts the state.
bool StateWeapon(uint16_t item) {
    switch (item) {
    case 0x102: case 0x110: case 0x111: case 0x11E: case 0x11F: case 0x129:  // M92F, P8, P226, M93R, Px4, Samurai Edge
    case 0x112:                                                              // Barry's Samurai Edge
    case 0x103: case 0x113:                                                  // VZ61, MP5
        return true;
    default: return false;
    }
}

bool Shotgun(uint16_t item) { return item == 0x104 || item == 0x116 || item == 0x117 || item == 0x119; }
// Shotguns and magnums (S&W M29, L. Hawk, S&W M500) blow enemies off (user).
bool BlowOffWeapon(uint16_t item) { return Shotgun(item) || item == 0x10B || item == 0x11A || item == 0x11B; }

// A base damage per weapon type until a real hit taught the weapon's own (user, 2026-10-21: magnums and rifles kill a
// Majini at once, so they learnt nothing and their missed quick shots did nothing, "looks like a bug"). Body hits,
// level 1 (recorded M92F 127, Ithaca 760 for its pellets, Dragunov 552); explosives, the bow and the flamethrower none.
int BaseDamage(uint16_t item) {
    switch (item) {
    case 0x102: case 0x110: case 0x111: case 0x11E: case 0x11F: case 0x129: case 0x112: return 150;  // handguns
    case 0x103: case 0x113: return 80;                                                      // VZ61, MP5
    case 0x109: case 0x11D: case 0x115: return 110;                                         // SIG 556, AK-74, Gatling
    case 0x104: case 0x116: case 0x117: case 0x119: return 700;                            // shotguns (all pellets)
    case 0x10B: case 0x11A: case 0x11B: return 1500;                                        // magnums
    case 0x105: case 0x11C: case 0x120: return 550;                                         // S75, PSG-1, Dragunov
    default: return 0;
    }
}

int BulletDamage(uint16_t item) {
    const int learnt = item >= 0x100 && item < 0x140 ? g_bulletDamage[item - 0x100].load() : 0;
    return learnt > 0 ? learnt : BaseDamage(item);
}

std::atomic<bool> g_damageDirty{ false };
DWORD g_damageSaved = 0;

std::wstring DamageFile() { return logx::DataDir() + L"\\quick_shot_damage.ini"; }

void LoadBulletDamage() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    std::ifstream f(DamageFile());
    std::string line;
    unsigned item = 0, flags = 0;
    int damage = 0, hits = 0;
    while (std::getline(f, line)) {
        const int read = sscanf_s(line.c_str(), "%x=%d,%d,%x", &item, &damage, &hits, &flags);
        if (read < 2 || item < 0x100 || item >= 0x140 || damage <= 0) continue;
        g_bulletDamage[item - 0x100] = damage;
        if (read >= 3 && hits > 0) g_bulletHits[item - 0x100] = hits;
        if (read >= 4 && (flags >> 16) == 0x0701) g_bloodFlags[item - 0x100] = flags;
    }
}

void SaveBulletDamage() {
    std::ofstream f(DamageFile(), std::ios::trunc);
    char line[48];
    for (unsigned i = 0; i < 0x40; ++i)
        if (const int damage = g_bulletDamage[i].load(); damage > 0) {
            std::snprintf(line, sizeof(line), "%04X=%d,%d,%X\n", 0x100 + i, damage, g_bulletHits[i].load(),
                          g_bloodFlags[i].load());
            f << line;
        }
}

// The quick shot is fired like the Hydra, one-handed (user), and with the option on the Hydra's own shots get a normal
// shotgun's (user: "on voit bien la différence"). The player's weapon motions are motion bank 5 (P+0xA9C, an
// rMotionList): Player::SetWeaponMotion (RVA 0x75A7A0, thiscall(record key, slot), ret 8) installs the held unit's
// list +0xA8C (pl00wpXX) or +0xA90 (pl01wpXX, P+0x1AF8 set) with uModel::SetMotionList (RVA 0x1CCDD0, thiscall(list,
// slot)). Aim and fire play fixed slots of that bank whatever the weapon (motion log: aim begin 0x515-0x517, aim
// 0x519-0x51B, fire 0x51C-0x51E, aim end 0x518, up / down blends 0x53C-0x547), and every weapon list has the same 116
// slots, 49 used (read from the archives: pl00wp02, 04, 16, 19). So from the quick shot's press until Chris is out of
// the aim the bank is the Hydra's (pawn\pl\pl00\motion\pl00wp19), and while the Hydra is in hand otherwise the
// Ithaca M37's (pl00wp04) but for its reloads; then SetWeaponMotion gives the held weapon its own back. The lists come from their weapon's
// archive (uWp19 / uWp04, loaded by the trainer if the level did not) through sResource::Load (RVA 0x48B70, type
// rMotionList DTI RVA 0xE4B574); the references are kept.
constexpr uint8_t kHydra = 0x19, kNormalShotgun = 0x04;
void* g_weaponLists[2][2] = {};  // [Hydra, Ithaca][male, female]
bool g_bankSet = false;          // bank 5 holds a list of ours

void* WeaponList(uint8_t weapon, bool female) {
    void*& cached = g_weaponLists[weapon == kHydra ? 0 : 1][female ? 1 : 0];
    if (cached) return cached;
    char name[16], path[64];
    std::snprintf(name, sizeof(name), "uWp%02x", weapon);
    std::snprintf(path, sizeof(path), "pawn\\pl\\%s\\motion\\%swp%02x", female ? "pl01" : "pl00", female ? "pl01" : "pl00", weapon);
    void* mgr = nullptr;
    if (!LoadWeaponArchive(name) || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE34580)), mgr) || !mgr) return nullptr;
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
        LOG("quick shot RE6: motion list %s not available (%p, state %08X)", path, res, state);
        return nullptr;
    }
    LOG("quick shot RE6: motion list %s at %p", path, res);
    return cached = res;
}

void SetWeaponBank(uintptr_t p, void* list) {
    uintptr_t current = 0;
    if (!list || (mem::Read(p + 0xA9C, current) && current == reinterpret_cast<uintptr_t>(list))) return;
    using SetMotionListFn = void(__fastcall*)(void* model, void* edx, void* list, int slot);
    __try {
        reinterpret_cast<SetMotionListFn>(mem::Rva(0x1CCDD0))(reinterpret_cast<void*>(p), nullptr, list, 5);
        g_bankSet = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("quick shot RE6: motion bank change failed");
    }
}

void RestoreWeaponBank(uintptr_t p) {
    if (!g_bankSet || !p) return;
    g_bankSet = false;
    uintptr_t held = 0;
    if (!mem::Read(p + 0x2634, held) || !held) return;
    using SetWeaponMotionFn = void(__fastcall*)(void* player, void* edx, int key, int slot);
    __try {
        reinterpret_cast<SetWeaponMotionFn>(mem::Rva(0x75A7A0))(reinterpret_cast<void*>(p), nullptr, 0xE, 5);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("quick shot RE6: weapon motion restore failed");
    }
}

// Each frame (RE6 mode): the Hydra's list during the quick shot, the Ithaca's for the Hydra in hand (its aim not to be
// taken for a quick shot) but while it reloads (user, 2026-11-01: "its aim the M37's, not its reload"), else the
// weapon's. The quick shot always takes the men's Hydra list (user, 2026-10-26: Jill took a rifle pose - the women's
// Hydra motions, pl01wp19, hold it in both hands); a woman's own Ithaca list for the Hydra's normal shots.
constexpr uint32_t kReloadAction = 0x1F;
// camera.cpp's iron sights (user, 2026-11-04: "the Jail Breaker aims from the hip, give it the Ithaca's aim in the
// iron sight mode only"): the Jail Breaker (0x0117) in hand with the sights wanted gets the Ithaca M37's list too, but
// for its reloads, as the Hydra.
constexpr uint16_t kJailBreaker = 0x0117;
constexpr uint16_t kGrenadeLauncher = 0x010C;  // the same (user, 2026-11-04)
std::atomic<bool> g_ironSightsWanted{ false };
bool g_ironBank = false;  // bank 5 holds the Ithaca's list for the Jail Breaker

bool IronBankFor(uintptr_t p, uint16_t item, uint32_t action) {
    return g_ironSightsWanted.load(std::memory_order_relaxed) && (item == kJailBreaker || item == kGrenadeLauncher) &&
           action != kReloadAction && p;
}

// The Hydra held with the Ithaca's motions sits too low in the hands (user, 2026-11-04: "lower even waiting, not only
// aiming: raise it but in the quick shot"): moved up along its own up (+Z) in the hand meanwhile.
constexpr float kHydraLift = 4.0f;  // cm

void LiftHydra(uintptr_t p, bool on) {
    static bool lifted = false;
    uintptr_t unit = 0;
    if (on && p && mem::Read(p + 0x2634, unit) && unit && EnsureAttachedMatrixHook()) {
        const float offset[3] = { 0.0f, 0.0f, kHydraLift };
        SetWeaponLift(unit, offset);
        lifted = true;
    } else if (lifted) {
        SetWeaponLift(0, nullptr);
        lifted = false;
    }
}

void TickWeaponBank(uintptr_t p, bool shooting) {
    uint8_t female = 0;
    uint32_t action = 0;
    mem::Read(p + 0x1AF8, female);
    mem::Read(p + 0x10E0, action);
    const uint16_t item = PlayerHeldItem(p);
    const bool hydraAsIthaca = !shooting && item == 0x100 + kHydra && action != kReloadAction;
    if (shooting) SetWeaponBank(p, WeaponList(kHydra, false));
    else if (hydraAsIthaca) SetWeaponBank(p, WeaponList(kNormalShotgun, female != 0));
    else if (IronBankFor(p, item, action)) SetWeaponBank(p, WeaponList(kNormalShotgun, female != 0));
    else RestoreWeaponBank(p);
    LiftHydra(p, hydraAsIthaca);
}
}  // namespace

namespace {
bool Re6ShotOn();  // below
}  // namespace

// shell_reload.cpp: player p's weapon bank (5, +0xA9C) holds the men's Ithaca M37 list (the Hydra's shots in RE6 mode,
// the Jail Breaker's and the grenade launcher's with the iron sights).
bool WeaponBankIsIthaca(uintptr_t p) {
    uintptr_t current = 0;
    if (!p || !mem::Read(p + 0xA9C, current) || !current) return false;
    return current == reinterpret_cast<uintptr_t>(g_weaponLists[1][0]);
}

// Each frame from camera.cpp: whether the iron sights may be used now (the option and a first person for the aim).
void IronSightWeaponBank(uintptr_t p, bool wanted) {
    g_ironSightsWanted = wanted;
    if (Re6ShotOn()) return;  // TickWeaponBank does it
    uint8_t female = 0;
    uint32_t action = 0;
    if (p) {
        mem::Read(p + 0x1AF8, female);
        mem::Read(p + 0x10E0, action);
    }
    if (IronBankFor(p, p ? PlayerHeldItem(p) : 0, action)) {
        SetWeaponBank(p, WeaponList(kNormalShotgun, female != 0));
        g_ironBank = true;
    } else if (g_ironBank) {
        g_ironBank = false;
        RestoreWeaponBank(p);
    }
}

// action_log.cpp's request hook: a reload asked with the Hydra in hand gets the Hydra's own list back first, so its
// own reload motions start (they are set when the reload begins).
void HydraReloadBank(uintptr_t p, int action) {
    if (static_cast<uint32_t>(action) != kReloadAction || !p || p != actors::Player(0)) return;
    const uint16_t item = PlayerHeldItem(p);
    if (item == 0x100 + kHydra ||
        ((item == kJailBreaker || item == kGrenadeLauncher) && g_ironSightsWanted.load(std::memory_order_relaxed)))
        RestoreWeaponBank(p);
}

namespace {

DWORD Since(DWORD tick, DWORD now) { return static_cast<int32_t>(now - tick) > 0 ? now - tick : 0; }

float WrapAngle(float a) {
    constexpr float kPi = 3.14159265f;
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

float FacingYaw(uintptr_t p) {
    float q[4]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x40), q, sizeof(q))) return 0.0f;
    return WrapAngle(2.0f * std::atan2(q[1], q[3]));
}

void FaceYaw(uintptr_t p, float yaw) {
    yaw = WrapAngle(yaw);
    mem::Write(p + 0x2A10, yaw);
    mem::Write(p + 0x1024, yaw);
    mem::Write(p + 0x2A24, yaw);
    const float quat[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x40), quat, sizeof(quat));
}

// The nearest living enemy within 8 m of player 1, those in front (75 degrees) first; its direction.
uintptr_t ShotTarget(uintptr_t p, float& yaw) {
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return 0;
    const float facing = FacingYaw(p);
    uintptr_t best = 0;
    float bestScore = 1e9f;
    for (uintptr_t enemy : actors::Enemies()) {
        int16_t health = 0;
        float e[3]{};
        if (!mem::Read(enemy + 0x1364, health) || health <= 0 || !mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), e, sizeof(e)))
            continue;
        const float dx = e[0] - pos[0], dz = e[2] - pos[2], dist = std::sqrt(dx * dx + dz * dz);
        if (dist > 800.0f || std::fabs(e[1] - pos[1]) > 200.0f) continue;
        const float angle = std::atan2(dx, dz);
        const bool front = std::fabs(WrapAngle(angle - facing)) < 1.31f;
        const float score = front ? dist : dist * 1.5f;
        if (score < bestScore) {
            bestScore = score;
            best = enemy;
            yaw = angle;
        }
    }
    return best;
}

bool TargetAlive(uintptr_t enemy) {
    int16_t health = 0;
    for (uintptr_t e : actors::Enemies())
        if (e == enemy) return mem::Read(enemy + 0x1364, health) && health > 0;
    return false;
}

bool FireHeld(int pad) {
    return RawHeld(kFireButton, pad) || RawHeld(kFireButton, -1) || FireKeyHeld() || PadMaskHas(0x8000);
}

// Where a quick shot may start (user, 2026-10-21: fire during a grab started one and the camera went wild, Chris being
// turned to the target): the free moves cPlAction_pl_act_wait 0x00, relax, walk, walk_l / _r, run, back, turn_l / _r,
// turn_180 0x09 (0x0A..0x1A are the wall cover ones), or the aim end 0x1D for the next shot. Not grabs (0x80000000 /
// 0x82 / 0x84), hits, melee, knife, reload 0x1F, weapon change 0x20. A shot under way stops when Chris leaves the aim
// actions 0x1B..0x1F (a grab or a hit meanwhile).
bool ShotCanStart(uint32_t action) { return action <= 0x09 || action == 0x1D; }
bool ShotCanGoOn(uint32_t action) { return action <= 0x09 || (action >= 0x1B && action <= 0x1F); }

// RE6 quick shot by aim + fire together (user, 2026-10-23/24): a mode of its own, exclusive with "Tir rapide façon
// RE6" and "Tir sans viser" (checking one unchecks the others): fire alone does not shoot quickly; aim and fire pressed
// within the window (either first) make the RE6 quick shot on the nearest enemy, later it is a plain aimed shot. Such a
// shot is not stopped by the aim the player holds, and starts during the aim's start too.
std::atomic<bool> g_shotWindowOn{ false };
std::atomic<float> g_shotWindowMs{ 150.0f };
std::atomic<bool> g_shotByWindow{ false };

bool Re6ShotOn() { return g_quickShotRe6.load(std::memory_order_relaxed) || g_shotWindowOn.load(std::memory_order_relaxed); }
bool ShotModeOn() { return g_quickShot.load(std::memory_order_relaxed) || Re6ShotOn(); }
// The pad's held mask hook (movement.cpp) for the quick shots, the fire key and the quick knife key.
bool PadMaskWanted() { return ShotModeOn() || g_fireKey.load() || g_knifeKey.load(); }
// The modes where fire alone out of aim shoots ("Tir sans viser", the RE6 quick shot); not "viser et tirer ensemble",
// where fire alone is no shot (user, 2026-10-27: the quick knife on the attack key never came out with it).
bool FireAloneShoots() {
    return g_quickShot.load(std::memory_order_relaxed) || g_quickShotRe6.load(std::memory_order_relaxed);
}

void StopQuickShot(bool after = false) {
    g_shotAim = false;
    g_shotAimDropped = false;
    g_shotFire = 0;
    g_shotTarget = 0;
    hooks::FreezeLook(false);
    g_shotAfter = after;
    if (!after) HideAimMarkers(false);
}

// The flesh impact of a missed shot (user: the player must believe the bullet landed). A Majini's hit feedback (uEm10
// vfunc 127 = VA 0x861160, shared by uEm10 / 11 / 12 / 14 / 1d / 23; logged on three pistol hits) plays sound 0x104
// of the unit's own bank (+0x1454: se_em10, se_em11...) at the hit point, the hit info's +0x10 (a 16-byte vector),
// through Unit::PlaySe (RVA 0x7570E0, thiscall(id, const float* position), ret 8). Played at the target's chest (30 cm
// above its position when it is on the ground), for the units with that feedback only. With its parasite out (the
// plaga unit at +0x1F9C, uEm0d) the parasite plays it, from its own bank se_em0d at its own place (a parasite hit,
// logged: sound 0x104 of the uEm0d unit).
// And its blood (user: "du sang sort", yellowish on a parasite). A bullet hit spawns the target unit's common effects:
// the hit system (VA 0xC46A43, function VA 0xC463A0) calls sBioEffect ([RVA 0xE49C5C]) VA 0x7757F0 thiscall(unit,
// flags, const float* point, const float* direction, x) ret 0x14, flags 0x7010200 for the pistol (type 7 = a shot, 0x01
// flesh; 0x7011200 for material 0xC2). It takes the unit's rCommonEffect (+0x152C) parameter 0 (CommonEffectParam:
// a Majini's em1000 / em1008 / em1016 / em1020 / em1021, a parasite's effect\cef\em\em0d) and spawns each of its 8
// lists with VA 0x773360 (the direction normalized there). Found with a hardware breakpoint on the em1000 slot (read
// twice per pistol hit by CommonEffectParam::GetList VA 0x802880) and a call log of that getter. The point is the
// target's chest (or the parasite), the direction towards player 1. Units are made there: it runs on the game thread
// (the next button query of the player's update, HookQuery), the overlay frame may be the render thread.
// A shotgun blast sprays once per pellet (user: one spray looked weak): as many sprays as pellets hit the last time
// (6 when unknown), spread over 20 cm around the point. Each weapon's own blood (user): the flags the game passed for
// player 1's last real flesh hit with it (VA 0x7757F0 hooked: 0x0701xxxx while he fires), kept with the damage;
// before one, the pistol's 0x7010200 (0x7010800, a shotgun's reaction bits, spawned nothing: user).
struct FakeImpact {
    uintptr_t unit = 0;
    int sprays = 1;
    uint32_t flags = 0x7010200;
    alignas(16) float point[4]{};
    alignas(16) float direction[4]{};
};
FakeImpact g_impact;
std::atomic<bool> g_impactPending{ false };

void PlayImpactSound(uintptr_t target, bool down, uint16_t item) {
    uintptr_t vtable = 0, feedback = 0, plaga = 0;
    if (!mem::Read(target, vtable) || !mem::Read(vtable + 0x1FC, feedback) ||
        feedback != reinterpret_cast<uintptr_t>(mem::Rva(0x461160)) || g_impactPending.load())
        return;
    uintptr_t unit = target;
    float height = down ? 30.0f : 115.0f;
    if (mem::Read(target + 0x1F9C, plaga) && plaga) {
        unit = plaga;
        height = 0.0f;
    }
    FakeImpact& impact = g_impact;
    float player[3]{};
    const uintptr_t p = actors::Player(0);
    if (!mem::SafeRead(reinterpret_cast<void*>(unit + 0x30), impact.point, 3 * sizeof(float)) || !p ||
        !mem::SafeRead(reinterpret_cast<void*>(p + 0x30), player, sizeof(player)))
        return;
    impact.point[1] += height;
    const float dx = player[0] - impact.point[0], dy = player[1] + 120.0f - impact.point[1], dz = player[2] - impact.point[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1.0f) return;
    impact.direction[0] = dx / len;
    impact.direction[1] = dy / len;
    impact.direction[2] = dz / len;
    impact.direction[3] = 0.0f;
    impact.unit = unit;
    const int pellets = item >= 0x100 && item < 0x140 ? g_bulletHits[item - 0x100].load() : 0;
    impact.sprays = !Shotgun(item) ? 1 : pellets > 1 ? (pellets > 12 ? 12 : pellets) : 6;
    const uint32_t known = item >= 0x100 && item < 0x140 ? g_bloodFlags[item - 0x100].load() : 0;
    impact.flags = known ? known : 0x7010200;
    g_impactPending = true;  // HookQuery plays it on the game thread
}

using CommonEffectsFn = void(__fastcall*)(void* effects, void* edx, void* unit, uint32_t flags, const float* point,
                                           const float* direction, uint32_t x);
CommonEffectsFn g_commonEffects = nullptr;  // trampoline of VA 0x7757F0
bool g_fakeSpawning = false;                // game thread: our own calls are not recorded

void __fastcall HookCommonEffects(void* effects, void* edx, void* unit, uint32_t flags, const float* point,
                                  const float* direction, uint32_t x) {
    if (!g_fakeSpawning && reinterpret_cast<uintptr_t>(unit) == actors::Player(0) &&
        (ParrySparesBlood(reinterpret_cast<uintptr_t>(unit)) || WeskerMoveInvulnerable(reinterpret_cast<uintptr_t>(unit)) ||
         PerfectDodgeSparesBlood(reinterpret_cast<uintptr_t>(unit)))) {
        LOG("parry: player 1's blood (flags %08X) not spawned", flags);
        return;
    }
    if (!g_fakeSpawning && (flags >> 16) == 0x0701) {
        const uintptr_t p = actors::Player(0);
        uint32_t action = 0;
        if (p && mem::Read(p + 0x10E0, action) && action == 0x1E) {
            const uint16_t item = PlayerHeldItem(p);
            if (item >= 0x100 && item < 0x140 && g_bloodFlags[item - 0x100].exchange(flags) != flags) g_damageDirty = true;
        }
    }
    g_commonEffects(effects, edx, unit, flags, point, direction, x);
}

}  // namespace

bool MeleePromptButton(uintptr_t button) { return IsMeleePrompt(button); }  // stamina's strict option (action_log.cpp)

// wesker_moves.cpp's RE6 melee: player 1's attack key held out of any aim (keyboard or pad), no inventory click (the press itself: a
// prompt shown is tested at the press, Re6MeleePromptShown - user, 2026-11-09: with the melee on the fire key the game's prompt took
// the press, its prompt went, and the key still held looked like a new press).
bool Re6MeleePromptShown() { return MeleePromptShown(); }

// wesker_moves.cpp's RE6 melee: another option plays the attack key's press out of aim ("Tir sans viser", the RE6 quick shot, the
// quick knife key on RT or held with the attack key). Without one the RE6 blows play at every press, no enemy near too (user,
// 2026-11-09: "with nothing checked but the RE6 melee he stays doing nothing").
bool AttackKeyTakenElsewhere() {
    if (FireAloneShoots()) return true;
    if (!g_knifeKey.load(std::memory_order_relaxed) || !g_knifeKeyFeature) return false;
    if (KnifeKeyOnRt()) return true;
    return KnifeKeyBound() && (hotkeys::Held(g_knifeKeyFeature->hotkey) || hotkeys::Held(g_knifeKeyFeature->hotkey2));
}

bool Re6MeleeFireHeld() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint8_t pad = 0;
    if (!p || !mem::Read(p + 0x2D9C, pad)) return false;
    const bool aim = RawHeld(kAimButton, pad) || RawHeld(kAimButton, -1) || PadMaskHas(kPadAimBit);
    const bool knifeAim =
        RawHeld(kKnifeAimButton, pad) || RawHeld(kKnifeAimButton, -1) || KnifeAimKeyHeld() || PadMaskHas(kPadKnifeBit);
    return FireHeld(pad) && !aim && !knifeAim && !InventoryClickHeld();
}

bool EnsureCommonEffectsHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x3757F0);
    const uint8_t expected[] = { 0x51, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x85, 0xF6 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("quick shot RE6: common effects unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookCommonEffects), reinterpret_cast<void**>(&g_commonEffects));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("quick shot RE6: common effects hook installed at %p", target);
    return true;
}

namespace {

// Game thread.
void RunFakeImpact() {
    if (!g_impactPending.load(std::memory_order_relaxed) || !g_impactPending.exchange(false)) return;
    const FakeImpact& impact = g_impact;
    using PlaySeFn = void(__fastcall*)(void* unit, void* edx, int id, const float* position);
    uintptr_t effects = 0, common = 0;
    const CommonEffectsFn spawn = g_commonEffects ? g_commonEffects : reinterpret_cast<CommonEffectsFn>(mem::Rva(0x3757F0));
    __try {
        reinterpret_cast<PlaySeFn>(mem::Rva(0x7570E0))(reinterpret_cast<void*>(impact.unit), nullptr, 0x104, impact.point);
        if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C5C)), effects) && effects &&
            mem::Read(impact.unit + 0x152C, common) && common) {
            static uint32_t seed = GetTickCount() | 1;
            for (int i = 0; i < impact.sprays; ++i) {
                alignas(16) float point[4] = { impact.point[0], impact.point[1], impact.point[2], 0.0f };
                if (impact.sprays > 1)
                    for (int axis = 0; axis < 3; ++axis) {
                        seed ^= seed << 13;
                        seed ^= seed >> 17;
                        seed ^= seed << 5;
                        point[axis] += static_cast<float>(static_cast<int>(seed % 41) - 20);  // +-20 cm
                    }
                g_fakeSpawning = true;
                spawn(reinterpret_cast<void*>(effects), nullptr, reinterpret_cast<void*>(impact.unit), impact.flags, point,
                      impact.direction, 0);
                g_fakeSpawning = false;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("quick shot RE6: fake impact failed");
    }
}

// The shot's outcome on its target: a bullet that landed did its own damage; a miss gets the recorded one. Then the
// state, for handguns and submachine guns.
void ResolveShot(uintptr_t p, uintptr_t target, bool hit) {
    const uint16_t item = PlayerHeldItem(p);
    int16_t health = 0;
    if (!TargetAlive(target) || !mem::Read(target + 0x1364, health)) {
        LOG("quick shot RE6: enemy %08X %s, item %04X: dead", static_cast<unsigned>(target), hit ? "hit" : "missed", item);
        return;
    }
    int damage = 0;
    if (!hit) PlayImpactSound(target, EnemyOnGround(target), item);
    if (!hit && !WeskerKeepsHealth(target) && (damage = BulletDamage(item)) > 0) {
        uint8_t pad = 0;
        if (mem::Read(p + 0x2D9C, pad)) mem::Write<int32_t>(target + 0x136C, pad);
        health = static_cast<int16_t>(health > damage ? health - damage : 0);
        mem::Write(target + 0x1364, health);
    }
    // A missed shotgun blast or magnum shot still blows the enemy off, standing or lying (dead at 0 health when it lands,
    // as the game's close shotgun kills); other weapons: a lethal miss dies at once.
    if (!hit && BlowOffWeapon(item)) MajiniBlowOff(target, p);
    else if (!hit && damage > 0 && !health) MajiniDieNow(target);
    const int state = g_shotState.load(std::memory_order_relaxed);
    const bool down = hit ? g_shotDownAtHit.load() : EnemyOnGround(target);
    uint32_t enemyAction = 0;
    mem::Read(target + 0x10E0, enemyAction);
    g_shotForcedState = health > 0 && StateWeapon(item) && !down && ForceMajiniState(target, state);
    LOG("quick shot RE6: enemy %08X (action %02X) %s, item %04X damage %s -> health %d, state %d %s",
        static_cast<unsigned>(target), enemyAction, hit ? "hit" : "missed", item,
        hit ? "the bullet's" : damage > 0 ? std::to_string(damage).c_str() : "none for this weapon", health, state,
        g_shotForcedState ? "forced" : !StateWeapon(item) ? "not for this weapon" : down ? "not (down)"
                                                          : "not (dead or not a Majini)");
}

void TickQuickShot() {
    static bool fireWas = false;
    static DWORD start = 0;
    const bool re6 = Re6ShotOn();
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint8_t pad = 0;
    uint32_t action = 0;
    if (!p || Re6OnGround() || !mem::Read(p + 0x2D9C, pad) || !mem::Read(p + 0x10E0, action)) {
        StopQuickShot();
        fireWas = false;
        return;
    }
    // The RE6 shot's motion: to its end, or a direction once its cancel window opened, or out of the free / aim actions.
    if (g_shotSlot.load()) {
        const float frame = ShotLayerFrame(p), length = ShotLayerLength(p);
        // Safety: a frame that no longer moves for half a second (a frozen layer) or 4 s in all end it too.
        static float lastFrame = -1.0f;
        static DWORD moved = 0, began = 0;
        const DWORD tick = GetTickCount();
        if (frame < lastFrame - 0.5f || lastFrame < 0.0f) began = tick;  // a new shot
        if (frame > lastFrame + 0.05f || lastFrame < 0.0f) moved = tick;
        lastFrame = frame;
        const bool stuck = static_cast<int32_t>(tick - moved) > 500 || static_cast<int32_t>(tick - began) > 4000;
        const bool moving = frame >= g_shotCancelFrame.load() && (RawHeld(0x00, pad) || RawHeld(0x01, pad) || RawHeld(0x02, pad) ||
                                                                  RawHeld(0x03, pad) || RawHeld(0x00, -1) || RawHeld(0x01, -1) ||
                                                                  RawHeld(0x02, -1) || RawHeld(0x03, -1));
        if (frame < 0.0f || frame >= length - 1.5f || !ShotCanGoOn(action) || moving || stuck) {
            LOG("quick shot RE6: its RE6 motion %d over at frame %.0f of %.0f%s (action %X)", g_shotSlot.load(), frame, length,
                moving ? ", a direction held" : stuck ? ", stuck (safety)" : "", action);
            g_shotSlot = 0;
            lastFrame = -1.0f;
            StopShotLayer(p);
        }
    }
    const bool fire = FireHeld(pad);
    const bool aim = RawHeld(kAimButton, pad) || RawHeld(kAimButton, -1) || PadMaskHas(kPadAimBit);
    const bool knifeAim =
        RawHeld(kKnifeAimButton, pad) || RawHeld(kKnifeAimButton, -1) || KnifeAimKeyHeld() || PadMaskHas(kPadKnifeBit);
    const DWORD now = GetTickCount();
    // The presses of aim and fire, for the window.
    static bool aimWas = false;
    static DWORD aimPressed = 0, firePressed = 0;
    const bool aimEdge = aim && !aimWas, fireEdge = fire && !fireWas;
    aimWas = aim;
    if (aimEdge) aimPressed = now | 1;
    if (fireEdge) firePressed = now | 1;
    if (re6) TickWeaponBank(p, g_shotAfter.load() || (g_shotAim.load() && g_shotTarget.load()));
    if (g_damageDirty.load() && Since(g_damageSaved, now) > 3000) {
        g_damageDirty = false;
        g_damageSaved = now;
        SaveBulletDamage();
    }
    if (g_shotAfter.load()) {
        const bool aiming = action >= 0x1B && action <= 0x1F;
        if (aim || knifeAim || (fire && !fireWas) || (!fire && !aiming) || Since(g_shotAfterTick, now) > 2500) {
            g_shotAfter = false;
            HideAimMarkers(false);
        } else {
            fireWas = fire;
            return;
        }
    }
    if (!g_shotAim.load(std::memory_order_relaxed)) {
        // Not when the fire button takes a melee prompt ("Mêlée sur la touche de tir", user 2026-10-23).
        const bool meleeOnFire = g_meleeOnFire.load(std::memory_order_relaxed) && MeleePromptShown();
        const bool windowMode = g_shotWindowOn.load(std::memory_order_relaxed);
        bool begin = false;
        float windowYaw = 0;
        uintptr_t windowTarget = 0;
        if (windowMode) {
            const DWORD window = static_cast<DWORD>(g_shotWindowMs.load(std::memory_order_relaxed));
            const bool together = (fireEdge && aim && aimPressed && Since(aimPressed, now) <= window) ||
                                  (aimEdge && firePressed && Since(firePressed, now) <= window);
            const bool canStart = action <= 0x09 || (action >= 0x1B && action <= 0x1D);
            if (together && !knifeAim && !InventoryClickHeld() && canStart && !meleeOnFire && !Player1Dying())
                windowTarget = ShotTarget(p, windowYaw);  // no enemy in reach: a plain aimed shot
            begin = windowTarget != 0;
        } else {
            // The RE6 melee first when an enemy is near (user, 2026-11-09), the shot otherwise.
            begin = fire && !fireWas && !aim && !knifeAim && !InventoryClickHeld() && ShotCanStart(action) && !meleeOnFire &&
                    !Re6MeleeTakesFire();
        }
        if (begin && !StaminaAllows()) begin = false;  // "Endurance": none while the red arc fills
        if (begin) {
            g_shotByWindow = windowMode;
            g_shotAim = true;
            g_shotAimDropped = false;
            g_shotFire = 1;
            g_shotSeen = false;
            start = now;
            g_shotFiredTick = 0;
            g_shotHitTick = 0;
            g_shotDownAtHit = false;
            g_shotResolved = false;
            g_shotForcedState = false;
            g_shotReforces = 0;
            // While dying (user, 2026-10-21) the press is the plain "Tir sans viser": aimed by the player, no target, free
            // to move, the aim kept while fire is held.
            const bool re6Shot = re6 && !Player1Dying();
            g_shotRe6 = re6Shot;
            float yaw = windowYaw;
            const uintptr_t target = windowTarget ? windowTarget : re6Shot ? ShotTarget(p, yaw) : 0;
            g_shotTarget = target;
            if (target) {
                TickWeaponBank(p, true);  // before the aim starts: its motions come from the Hydra's list
                const float facing = FacingYaw(p);
                const bool turn = std::fabs(WrapAngle(yaw - facing)) > 1.92f;  // over 110 degrees: the half-turn
                g_shotSlot = 0;
                g_shotTurn = false;
                // RE6's shot on the leg layer fought the game's actions there (2026-11-10: frozen, looped, player 1 stuck; user: "keep RE6's
                // animation, just fix it, it is much better for the shotguns"): the game's fire is cut once its shot went (its pump waited for
                // its own motion there, asked every frame), its turns refused meanwhile (QuickShotRefusesPlayerAction).
                if (const Re6ShotPair* shots = kRe6ShotMotions && re6Shot ? Re6ShotsOf(PlayerHeldItem(p)) : nullptr) {
                    const Re6Shot& shot = turn ? shots->back : shots->front;
                    if (EnsurePlayerRequestHook() && PlayShotLayer(p, Re6EvadeMotion(shot.slot))) {
                        g_shotFireFrame = shot.fire;
                        g_shotCancelFrame = shot.cancel;
                        g_shotTurn = turn;
                        g_shotTurnFrom = facing;
                        g_shotSlot = shot.slot;
                        // The game's aim straight up (user, 2026-11-10: "the assault rifle and the shotgun still loop a second time": its
                        // aim's raise 0x1B took 0.42-0.49 s before 0x1C could fire, the RE6 shot showing its own at 0.07-0.17 s; logged): its
                        // aim 0x1C asked at once, so the real shot comes at the RE6 motion's marked frame.
                        LOG("quick shot RE6: the game's aim asked at once (%s)", RequestPlayerAction(p, 0x1C) ? "asked" : "refused");
                        LOG("quick shot RE6: its RE6 motion %u (%s, item %04X), fire at its frame %.0f", shot.slot, turn ? "half-turn" : "front",
                            PlayerHeldItem(p), shot.fire);
                        // Research (2026-11-10, "he pumps twice"): with DataDir\qs_burst, 100 whole pictures every 2 frames (fburst_NN.bmp).
                        const std::wstring burst = logx::DataDir() + L"\\qs_burst";
                        if (GetFileAttributesW(burst.c_str()) != INVALID_FILE_ATTRIBUTES && hooks::RequestFastBurstWhole(100, 4, 2)) {
                            DeleteFileW(burst.c_str());
                            LOG("burst research: an RE6 quick shot, 100 pictures from now");
                        }
                    }
                }
                FaceYaw(p, g_shotTurn ? facing : yaw);
                hooks::FreezeLook(true);
                HideAimMarkers(true);
            }
        }
    } else {
        if (!ShotCanGoOn(action)) {
            StopQuickShot();
            fireWas = fire;
            return;
        }
        if (const uintptr_t target = g_shotTarget.load(); re6 && target) {
            float e[3]{}, pos[3]{};
            if (TargetAlive(target) && mem::SafeRead(reinterpret_cast<void*>(target + 0x30), e, sizeof(e)) &&
                mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) {
                float yaw = std::atan2(e[0] - pos[0], e[2] - pos[2]);
                float eye[3]{}, camera = 0, pitch = 0;
                if (CameraRay(eye, camera, pitch) && std::fabs(WrapAngle(camera - yaw)) < 0.5f)
                    yaw = std::atan2(e[0] - eye[0], e[2] - eye[2]);
                if (g_shotTurn && g_shotSlot.load()) {  // the half-turn: over its first 8 frames, eased
                    const float t = std::clamp(ShotLayerFrame(p) / 8.0f, 0.0f, 1.0f), eased = t * t * (3.0f - 2.0f * t);
                    yaw = g_shotTurnFrom + WrapAngle(yaw - g_shotTurnFrom) * (ShotLayerFrame(p) < 0.0f ? 1.0f : eased);
                }
                FaceYaw(p, yaw);
            }
            // The RE6 shot's fire at its marked frame (the game's aim must be up too).
            const float shotFrame = g_shotSlot.load() ? ShotLayerFrame(p) : -1.0f;
            const bool fireNow = shotFrame < 0.0f || shotFrame >= g_shotFireFrame.load();
            if ((action == 0x1C || action == 0x1E) && fireNow && g_shotFire.load(std::memory_order_relaxed) == 1) g_shotFire = 2;
            if (action == 0x1E && !g_shotFiredTick) g_shotFiredTick = now | 1;
            // An RE6 shot's motion: the game's fire cut 80 ms after it went (its bullet, sound and flash out; its pump waited for its own
            // motion on the leg layer, asked every frame, and stood the RE6 shot still), the aim held on until the shot is resolved.
            static DWORD cutFor = 0;
            if (g_shotSlot.load() && action == 0x1E && g_shotFiredTick && cutFor != g_shotFiredTick && Since(g_shotFiredTick, now) >= 80) {
                cutFor = g_shotFiredTick;
                g_shotAimDropped = true;  // its aim let go too: the game back to its wait under the RE6 motion
                LOG("quick shot RE6: the game's fire and aim cut after its shot (%s)", RequestPlayerAction(p, 0x00) ? "idle asked" : "refused");
            }
            if (action == 0x1E) g_shotSeen = true;
            const DWORD hitAt = g_shotHitTick.load();
            const bool hit = hitAt != 0;
            const bool missed = !hit && g_shotFiredTick && Since(g_shotFiredTick, now) >= kShotMissMs;
            if (!g_shotResolved && (hit || missed)) {
                g_shotResolved = true;
                ResolveShot(p, target, hit);
            }
            uint32_t enemyAction = 0;
            const int state = g_shotState.load(std::memory_order_relaxed);
            if (hit && g_shotForcedState && g_shotReforces < kShotMaxReforces && TargetAlive(target) &&
                mem::Read(target + 0x10E0, enemyAction) && enemyAction != MajiniStateAction(state) &&
                !((state == 1 || state >= 5) && enemyAction == 0x7D) &&  // the stumbles are either stumble (parry.cpp)
                ForceMajiniState(target, state)) {
                ++g_shotReforces;
                LOG("quick shot RE6: enemy %08X took action %02X after the hit -> state %d again", static_cast<unsigned>(target),
                    enemyAction, state);
            }
            const bool aimStops = aim && !g_shotByWindow.load(std::memory_order_relaxed);  // the window's aim is held
            if ((hit && Since(hitAt, now) >= kShotWatchMs) || missed || aimStops || knifeAim || Since(start, now) > 3000) {
                g_shotAfterTick = now;
                StopQuickShot(hit || missed);  // one shot per press, the aim ends
                fireWas = fire;
                return;
            }
            fireWas = fire;
            return;  // the plain quick shot's rules below do not apply
        }
        if (action == 0x1C || action == 0x1E) {
            if (g_shotFire.load(std::memory_order_relaxed) == 1) g_shotFire = 2;
        }
        if (action == 0x1E) g_shotSeen = true;
        const bool raising = g_shotFire.load(std::memory_order_relaxed) == 1;
        if (aim || knifeAim || (raising && now - start > 1500) || (!fire && (g_shotSeen || now - start > 2000)))
            StopQuickShot();
    }
    fireWas = fire;
}

}  // namespace

// No walking nor running during the RE6-style quick shot, from the press until Chris is out of the aim (user, as in
// RE6): the direction buttons 0x00..0x03 and run 0x04 read as released (the run toggle's too: it stays on, Chris runs
// again after the shot, user), "Marcher pendant les actions" (movement.cpp) holds still.
bool QuickShotHoldsPlayer() {
    // RE6's shot motion: until its cancel window (user: as in RE6).
    if (g_shotSlot.load(std::memory_order_relaxed)) {
        const float frame = ShotLayerFrame(actors::InGame() ? actors::Player(0) : 0);
        if (frame >= 0.0f && frame < g_shotCancelFrame.load(std::memory_order_relaxed)) return true;
    }
    return Re6ShotOn() && g_shotRe6.load(std::memory_order_relaxed) &&
           (g_shotAim.load(std::memory_order_relaxed) || g_shotAfter.load(std::memory_order_relaxed));
}

// action_log.cpp's player request hook: no turns (turn_l / turn_r 0x07 / 0x08, turn_180 0x09) while RE6's shot motion plays (2026-11-10: they
// waited for their own motion on the leg layer and flipped with the wait every frame).
// Walking and running too until its cancel frame (user, 2026-11-10: "it still loops if I try to move"; logged: run 5 and wait 0 flipping).
bool QuickShotRefusesPlayerAction(uintptr_t player, int action) {
    if (!g_shotSlot.load(std::memory_order_relaxed) || !player || player != actors::Player(0)) return false;
    if (action >= 0x07 && action <= 0x09) return true;
    if (action < 0x02 || action > 0x06) return false;
    const float frame = ShotLayerFrame(player);
    return frame >= 0.0f && frame < g_shotCancelFrame.load(std::memory_order_relaxed);
}

// camera.cpp: the scoped weapons' scope cut during an RE6 quick shot (user, 2026-11-10: "in the quick shot's window the sniper's scope must
// not show, as for the iron sight, or I cannot see the animation").
bool QuickShotCutsScope() {
    return g_shotSlot.load(std::memory_order_relaxed) ||
           (Re6ShotOn() && g_shotRe6.load(std::memory_order_relaxed) &&
            (g_shotAim.load(std::memory_order_relaxed) || g_shotAfter.load(std::memory_order_relaxed)));
}

// Player 1's bullet hit an enemy (knife.cpp OnHit hook, before the game's).
bool QuickShotTarget(uintptr_t enemy) {
    return enemy && Re6ShotOn() && enemy == g_shotTarget.load();
}

void QuickShotSawHit(uintptr_t enemy) {
    DWORD none = 0;
    if (g_shotHitTick.compare_exchange_strong(none, GetTickCount() | 1)) g_shotDownAtHit = EnemyOnGround(enemy);
}

// One shot's damage on one enemy: every pellet of a shotgun is its own hit (the game's shown firepower is one pellet's,
// user: a missed shotgun quick shot took 4 shots to kill instead of 2), so the hits on the same enemy with the same
// weapon within 60 ms are added up (a machine gun's next bullet comes later). Head hits count for shotguns only (a
// handgun's head hit can be a critical). The damage is what the enemy's SubHealth received (knife.cpp), a kill
// included. Game thread.
void RecordBulletDamage(uintptr_t enemy, uint16_t item, int damage, bool head) {
    static uintptr_t lastEnemy = 0;
    static uint16_t lastItem = 0;
    static DWORD lastTick = 0;
    static int sum = 0;
    if (item < 0x100 || item >= 0x140 || damage <= 0 || (head && !Shotgun(item))) return;
    const DWORD now = GetTickCount();
    static int hits = 0;
    if (enemy == lastEnemy && item == lastItem && Since(lastTick, now) < 60) {
        sum += damage;
        ++hits;
    } else {
        sum = damage;
        hits = 1;
        lastEnemy = enemy;
        lastItem = item;
        lastTick = now;
    }
    g_bulletHits[item - 0x100] = hits;
    if (g_bulletDamage[item - 0x100].exchange(sum) != sum) g_damageDirty = true;
}

namespace {
void TickQuickAsAim() {
    static bool keyWas = false, stabStarted = false;
    static DWORD start = 0;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint8_t pad = 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x2D9C, pad) || !mem::Read(p + 0x10E0, action)) {
        g_emulating = false;
        keyWas = false;
        return;
    }
    const bool key = RawHeld(kQuickKnifeButton, pad);
    const DWORD now = GetTickCount();
    if (!g_emulating) {
        if (key && !keyWas && !RawHeld(kAimButton, pad) && !InventoryClickHeld() &&
            !(FireAloneShoots() && FireHeld(pad)) && !g_shotAim.load(std::memory_order_relaxed)) {
            g_emulating = true;
            stabStarted = false;
            start = now;
        }
    } else {
        if (action == 0x27) stabStarted = true;
        const bool drawing = action == 0x20 || action == 0x22;
        if (!key && (stabStarted || (!drawing && now - start > 300) || now - start > 1500)) g_emulating = false;
    }
    keyWas = key;
}

// "Couteau rapide (touche)": the press starts the knife aim + fire (not aiming, on foot, the inventory closed, no melee
// prompt the fire key takes with "Corps à corps sur la touche de tir"); held, the stabs chain; released, it ends once the
// stab has started (a tap = one stab), as TickQuickAsAim.
void TickKnifeKey() {
    static bool keyWas = false, stabStarted = false;
    static DWORD start = 0;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action)) {
        g_knifeKeyEmulating = false;
        keyWas = false;
        return;
    }
    const bool key = KnifeKeyHeld();
    const DWORD now = GetTickCount();
    // On RT with "Tir sans viser" / the RE6 quick shot (fire alone shoots), the shot wins, as the quick knife on the
    // attack key with the keyboard.
    const bool shotWins = KnifeKeyOnRt() && FireAloneShoots();
    if (!g_knifeKeyEmulating) {
        if (key && !keyWas && !shotWins && actors::OnFoot(p) && !Player1InventoryOpen() && !Player1Aiming() && !InventoryClickHeld() &&
            !g_shotAim.load(std::memory_order_relaxed) && !(g_meleeOnFire.load(std::memory_order_relaxed) && MeleePromptShown())) {
            g_knifeKeyEmulating = true;
            stabStarted = false;
            start = now;
            LOG("quick knife key: stab (action %X)", action);
        }
    } else {
        if (action == 0x27) stabStarted = true;
        const bool drawing = action == 0x20 || action == 0x22;
        if (!key && (stabStarted || (!drawing && now - start > 300) || now - start > 1500)) g_knifeKeyEmulating = false;
    }
    keyWas = key;
}

// The prompts' icons, on the game thread (from the pad queries, at most every 4 ms): see LiveButton.
void TickPromptIcons() {
    static LARGE_INTEGER frequency{}, last{};
    LARGE_INTEGER now{};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    if (now.QuadPart - last.QuadPart < frequency.QuadPart / 250) return;
    last = now;
    TickTraversalIcon();
    TickDoorKickIcon();
    TickIconTest();
    TickMeleeIcon();
}

template <int Q> bool __fastcall HookQuery(void* self, void* edx, int button, int pad) {
    RunFakeImpact();
    TickPromptIcons();
    ArmPromptTick();  // parry.cpp: the arm states' prompts (game thread)
    EnemySpawnTick();  // enemy_spawn.cpp: a requested spawn (game thread)
    CoopSoloTick();    // coop_solo.cpp: player 2's part of a partner action (game thread)
    BeltLampTick();    // belt_lamp.cpp: the belt lamps (game thread)
    CrankTick();       // crank.cpp: a cranked gate held open (game thread)
    LeverTick();       // levers.cpp: a lever kept down against the clock (game thread)
    BoatTick();        // boat.cpp: the airboat's driver aims (game thread)
    RaftTick();        // raft.cpp: the raft played alone (game thread)
    HeadBlowTick();    // melee_tuning.cpp: a critical's head blow without the game's hit (game thread)
    SheathGunReloadTick();  // inventory.cpp: the reload key in the knife aim reloads the knife slot's gun (game thread)
    EnvExecTick();          // wesker_moves.cpp: the environmental executions' checks (the game's collision probe: game thread)
    if (button >= 0x00 && button <= kRunButton && QuickShotHoldsPlayer()) return false;  // no move in the RE6 shot
    if (button >= 0x00 && button <= 0x03 && Re6GroundHoldsPlayer()) return false;  // RE6's ground: the crawl only (wesker_moves.cpp)
    // The prompts' own reads get the real buttons (user, 2026-10-24: Wesker's A + D dodge QTE, a type 3 prompt, mostly
    // missed): sActionButton's code, RVA 0x360000.. (QTE reads at RVA 0x3639C2 held / 0x3639F1 pressed, the struggles
    // RVA 0x366320 / 0x3664A0; the player's movement reads at RVA 0x78A7C5..0x78C688, diag.pad_log). Two opposite
    // directions held together are no move: the player's code gets them too (no walk forward during the QTE).
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool promptRead = caller >= reinterpret_cast<uintptr_t>(mem::Rva(0x360000)) &&
                            caller < reinterpret_cast<uintptr_t>(mem::Rva(0x370000));
    const bool opposite = button >= 0x00 && button <= 0x03 && g_directionForward.load(std::memory_order_relaxed) &&
                          (promptRead || (g_query[3](self, edx, 0x02, pad) && g_query[3](self, edx, 0x03, pad)) ||
                           (g_query[3](self, edx, 0x00, pad) && g_query[3](self, edx, 0x01, pad)));
    if (button >= 0x00 && button <= 0x03 && g_directionForward.load(std::memory_order_relaxed) && !opposite) {
        if (g_padLog.load(std::memory_order_relaxed) && g_query[Q](self, edx, button, pad))  // research: the real reads
            LogQuery(Q, button | 0x100, pad, reinterpret_cast<uintptr_t>(_ReturnAddress()));
        if (button != 0x00) return false;
        for (int b = 0x00; b <= 0x03; ++b)
            if (g_query[Q](self, edx, b, pad)) return true;
        return false;
    }
    if (Q == 3 && button == kRunButton && g_forceRun.load(std::memory_order_relaxed)) return true;
    if (promptRead && (button == 0x0F || button == 0x10 || button == 0x0C) && g_bossPromptPressed.load(std::memory_order_relaxed) &&
        GetTickCount64() < g_bossPressUntil.load(std::memory_order_relaxed))
        return true;  // a boss's melee prompt pressed by the fire key (PressMeleePrompt)
    // A melee prompt shown in the aim: the fire key is its (TickMeleeIcon), no shot nor stab (the prompts' own reads see it).
    if (button == kFireButton && !promptRead &&
        (g_firePromptInAim.load(std::memory_order_relaxed) || g_fireHeldAfterPrompt.load(std::memory_order_relaxed)))
        return false;
    if (Q == 3 && button == kReloadButton && GetTickCount64() < g_reloadUntil.load(std::memory_order_relaxed)) return true;
    // The quick knife on the attack key: the shot wins - a shot under way, or fire held in a mode where fire alone shoots
    // (not "viser et tirer ensemble": there fire alone is no shot; user, 2026-10-27: the knife never came out).
    if (button == kQuickKnifeButton && ShotModeOn() &&
        (g_shotAim.load(std::memory_order_relaxed) || (FireAloneShoots() && FireHeld(pad))))
        return false;
    if (Q == 3 && button == kAimButton && g_shotAim.load(std::memory_order_relaxed) && !g_shotAimDropped.load(std::memory_order_relaxed))
        return true;
    if (button == kFireButton && g_shotAfter.load(std::memory_order_relaxed)) return false;  // the RE6 shot is over
    if (button == kFireButton && g_shotAim.load(std::memory_order_relaxed)) {
        const int fire = g_shotFire.load(std::memory_order_relaxed);
        if (fire == 1) return false;                       // the weapon is not up yet
        if (fire == 2 && Q == 3 && !g_shotSeen) return true;  // a tap: held until its shot
        // An RE6 shot's motion: one shot (user, 2026-11-10: "at least three pump actions"), the fire released once it went.
        if (g_shotSeen && g_shotSlot.load(std::memory_order_relaxed)) return false;
    }
    if (button == kQuickKnifeButton && g_quickAsAim.load(std::memory_order_relaxed)) return false;
    if (button == kQuickKnifeButton && g_noKnifeInAim.load(std::memory_order_relaxed) &&
        g_query[3](self, edx, kAimButton, pad))
        return false;
    const bool emulating = g_emulating.load(std::memory_order_relaxed) || g_knifeKeyEmulating.load(std::memory_order_relaxed);
    if (Q == 3 && button == kKnifeAimButton && (KnifeAimKeyHeld() || emulating)) return true;
    if (Q == 3 && button == kFireButton && emulating) return true;
    if (button == kFireButton && FireKeyHeld() &&
        (Q == 3 || (Q == 0 && GetTickCount64() < g_fireKeyEdgeUntil.load(std::memory_order_relaxed))))
        return true;  // "Tirer (touche)"
    const bool result = g_query[Q](self, edx, button, pad);
    if (result && g_padLog.load(std::memory_order_relaxed))
        LogQuery(Q, button, pad, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return result;
}

void* const kHooks[kQueries] = { reinterpret_cast<void*>(&HookQuery<0>), reinterpret_cast<void*>(&HookQuery<1>),
                                 reinterpret_cast<void*>(&HookQuery<2>), reinterpret_cast<void*>(&HookQuery<3>) };

bool EnsureQueryHooks() {
    static bool hooked = false;
    if (hooked) return true;
    // mov ecx, [esp+8]; test ecx, ecx; mov eax, [sBioPad]
    const uint8_t expected[] = { 0x8B, 0x4C, 0x24, 0x08, 0x85, 0xC9, 0xA1 };
    for (int q = 0; q < kQueries; ++q) {
        uint8_t* target = mem::Rva(kQueryRvas[q]);
        if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
            LOG("pad query %d: unexpected bytes %s", q, mem::Hex(target, sizeof(expected)).c_str());
            return false;
        }
    }
    for (int q = 0; q < kQueries; ++q) {
        uint8_t* target = mem::Rva(kQueryRvas[q]);
        mem::EnsureExecutable(target, 8);
        MH_STATUS st = MH_CreateHook(target, kHooks[q], reinterpret_cast<void**>(&g_query[q]));
        if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
        st = MH_EnableHook(target);
        if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    }
    hooked = true;
    LOG("pad query hooks installed");
    return true;
}
}  // namespace

// For the parry (parry.cpp): a game button held (query 3, through the hooks so the knife aim key counts), the knife aim
// key of "Viser avec le couteau (touche)".
bool GameButtonHeld(int button, int pad) {
    QueryFn query = g_query[3] ? reinterpret_cast<QueryFn>(kHooks[3]) : reinterpret_cast<QueryFn>(mem::Rva(0x39A4D0));
    __try {
        return query(nullptr, nullptr, button, pad);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool EnsurePadQueryHooks() { return EnsureQueryHooks(); }


// inventory.cpp's knife slot gun: the game's reload button as player 1 really holds it, or "Recharger (raccourci)".
bool ReloadButtonPressed() {
    return GetTickCount64() < g_reloadUntil.load(std::memory_order_relaxed) || RawHeld(kReloadButton, Player1Pad());
}

// movement.cpp's pad mask stub (game thread): the fire key's fire, then the quick shots' aim and fire as their button
// queries answer on the keyboard (fire held back until the weapon is up, held until a tap's shot, cut after the RE6 shot).
void QuickShotPadMask(uint32_t& held) {
    if (FireKeyHeld()) held |= kPadFireBits;
    if (g_knifeKeyEmulating.load(std::memory_order_relaxed)) held |= kPadKnifeBit | kPadFireBits;  // "Couteau rapide (touche)"
    if (!ShotModeOn()) return;
    if (g_shotAfter.load(std::memory_order_relaxed)) {
        held &= ~kPadFireBits;
        return;
    }
    if (!g_shotAim.load(std::memory_order_relaxed)) return;
    if (g_shotAimDropped.load(std::memory_order_relaxed)) {  // an RE6 shot's motion after its fire: no aim, no fire
        held &= ~(kPadAimBit | kPadFireBits);
        return;
    }
    held |= kPadAimBit;
    const int fire = g_shotFire.load(std::memory_order_relaxed);
    if (fire == 1) held &= ~kPadFireBits;
    else if (fire == 2 && !g_shotSeen) held |= kPadFireBits;
    else if (g_shotSeen && g_shotSlot.load(std::memory_order_relaxed)) held &= ~kPadFireBits;  // an RE6 shot's motion: one shot
}

// For the run toggle (movement.cpp): a button as the player really holds it (no trainer override), and the forced run.
bool RawButtonHeld(int button, int pad) { return RawHeld(button, pad); }

// parry.cpp: a RE6-style parry on the attack key - the quick shot that press started is dropped (it is not started
// again while the key stays held: a new press is needed).
void CancelQuickShot() {
    if (g_shotAim.load()) StopQuickShot();
}

// parry.cpp, "Le couteau rapide ne déclenche pas la parade": the knife aim and fire the quick knife emulates are no
// press, the knife aim really held is; the RE6 parry won on that press drops the emulated knife.
bool QuickKnifeEmulating() {
    return g_emulating.load(std::memory_order_relaxed) || g_knifeKeyEmulating.load(std::memory_order_relaxed);
}
bool RealKnifeAimHeld() { return RawHeld(kKnifeAimButton, -1) || KnifeAimKeyHeld(); }
void CancelQuickKnife() {
    g_emulating = false;
    g_knifeKeyEmulating = false;
}
void ForceRunHeld(bool on) { g_forceRun.store(on, std::memory_order_relaxed); }
void ForceDirectionForward(bool on) { g_directionForward.store(on, std::memory_order_relaxed); }

void RegisterQuickKnife() {
    Add({ .id = "player.quick_knife_no_aim", .name = "Couteau rapide seulement sans viser",
          .help = "Le raccourci « couteau rapide » du clavier est ignoré tant que la touche de visée est tenue : on peut "
                  "le mettre sur la même touche que l'attaque, qui tire en visant et donne un coup de couteau sans viser.",
          .category = "Couteau : rapide", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureQueryHooks()) return false;
              g_noKnifeInAim = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    // The game's own quick knife (research, MODLOG session 14): on its first press it looks for an enemy in front (RVA
    // 0x784CEF..0x7850D7, +0x716 = found) and aims at it; found, P+0x2D84 0x400 lasts the first stab, the draw sets
    // 0x200 | 0x80. Patching those flags out of the camera code (RVA 0x786431 / 0x786439 / 0x786B62 / 0x786B6A /
    // 0x786D31) and skipping the search (RVA 0x784CF6) removed the auto-aim but the draw still held the camera: the knife
    // aim path is used instead (see TickQuickAsAim). The id is the one of the first attempt (setting kept).
    Add({ .id = "player.quick_knife_no_auto_aim", .name = "Couteau rapide comme la visée au couteau",
          .help = "Le couteau rapide passe par la visée au couteau : il frappe là où Chris regarde (le jeu ne choisit plus "
                  "l'ennemi le plus proche), la caméra reste libre pendant qu'il dégaine, et tenir la touche enchaîne les "
                  "coups. Hors visée seulement.",
          .category = "Couteau : rapide", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureQueryHooks()) return false;
              g_quickAsAim = on;
              if (!on) g_emulating = false;
              return true;
          },
          .tick = TickQuickAsAim,
          .available = [] { return game::IsSupported(); } });
    Feature& fireKey = Add({ .id = "player.fire_key", .name = "Tirer (touche)",
                             .help = "Une touche ou un bouton de manette de ton choix qui tire comme la touche de tir du "
                                     "jeu tant qu'il est tenu (hors visée : le tir sans viser ou le tir rapide RE6 s'ils "
                                     "sont cochés). Choisis-le dans la colonne Raccourci ; la case active ou coupe cette "
                                     "touche.",
                             .category = "Joueur", .kind = Kind::Toggle,
                             .set = [](bool on) {
                                 if (on && !EnsureQueryHooks()) return false;
                                 g_fireKey = on;
                                 UsePadMaskForQuickShot(PadMaskWanted());
                                 return true;
                             },
                             .tick = TickFireKey,
                             .available = [] { return game::IsSupported(); },
                             .holdKey = true });
    g_fireKeyFeature = &fireKey;
    Add({ .id = "player.quick_shot", .name = "Tir sans viser",
          .help = "La touche d'attaque sans viser fait lever l'arme et tirer tout de suite, comme le couteau rapide : la "
                  "souris vise pendant ce temps, tenir la touche garde l'arme levée (rafales des armes automatiques), "
                  "relâcher la baisse. Le couteau rapide placé sur la même touche est ignoré.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureQueryHooks()) return false;
              for (const char* id : { "player.quick_shot_re6", "player.quick_shot_window" })
                  if (Feature* other = Find(id); on && other && other->enabled) SetEnabled(*other, false);
              g_quickShot = on;
              if (!on) StopQuickShot();
              UsePadMaskForQuickShot(PadMaskWanted());
              return true;
          },
          .tick = TickQuickShot,
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "player.quick_shot_re6", .name = "Tir rapide façon RE6",
          .help = "La touche d'attaque sans viser vise toute seule l'ennemi le plus proche à 8 m (devant d'abord) et tire "
                  "une fois, sans laser ni réticule. Que la balle touche ou non, l'ennemi prend les dégâts de l'arme (une "
                  "balle ratée compte comme ta dernière balle qui a touché avec cette arme, retenue d'une partie à l'autre) "
                  "et, avec une arme de poing ou une mitraillette, un Majini debout est mis dans l'état choisi juste en "
                  "dessous. En étant mourant, la touche fait un « Tir sans viser » normal (tu vises toi-même). Cocher "
                  "cette case décoche « Tir sans viser » et « Tir rapide RE6 : viser et tirer ensemble ».\n"
                  "Dégâts d'un tir raté : une valeur de base par type d'arme, puis les vrais dégâts de l'arme dès sa "
                  "première balle qui touche un ennemi (tir mortel compris, hors tir dans la tête), gardés d'une partie à "
                  "l'autre. Lance-grenades, lance-roquettes, arc et lance-flammes : aucun dégât sur un tir raté.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureQueryHooks() || !EnsureEnemyOnHitHook() || !EnsureEnemyDamageHook() || !EnsurePlayerDamageHook() ||
                         !UseAimMarkerCalls(true)))
                  return false;
              if (on) EnsureCommonEffectsHook();  // the blood flags of each weapon (optional)
              if (on) LoadBulletDamage();
              for (const char* id : { "player.quick_shot", "player.quick_shot_window" })
                  if (Feature* other = Find(id); on && other && other->enabled) SetEnabled(*other, false);
              if (!on) {
                  UseAimMarkerCalls(false);
                  RestoreWeaponBank(actors::InGame() ? actors::Player(0) : 0);
              }
              g_quickShotRe6 = on;
              if (!on) StopQuickShot();
              UsePadMaskForQuickShot(PadMaskWanted());
              return true;
          },
          .tick = TickQuickShot,
          .available = [] { return game::IsSupported(); } });
    Feature& shotWindow = Add({ .id = "player.quick_shot_window", .name = "Tir rapide RE6 : viser et tirer ensemble",
                                .help = "Le tir rapide façon RE6, mais la touche de tir seule n'en fait plus : viser et "
                                        "tirer presque en même temps (dans un sens ou dans l'autre, l'écart sous la "
                                        "fenêtre du curseur, en millisecondes) fait le tir rapide sur l'ennemi le plus "
                                        "proche ; au-delà de la fenêtre, c'est un tir normal. Sans ennemi à portée (8 m), "
                                        "le tir reste normal. Mêmes dégâts et état de l'ennemi que le tir rapide RE6. "
                                        "Cocher cette case décoche « Tir rapide façon RE6 » et « Tir sans viser ».",
                                .category = "Joueur", .kind = Kind::Toggle,
                                .set = [](bool on) {
                                    if (on && (!EnsureQueryHooks() || !EnsureEnemyOnHitHook() || !EnsureEnemyDamageHook() ||
                                               !EnsurePlayerDamageHook()))
                                        return false;
                                    for (const char* id : { "player.quick_shot", "player.quick_shot_re6" })
                                        if (Feature* other = Find(id); on && other && other->enabled) SetEnabled(*other, false);
                                    if (on && !UseAimMarkerCalls(true)) return false;
                                    if (on) EnsureCommonEffectsHook();
                                    if (on) LoadBulletDamage();
                                    if (!on) {
                                        UseAimMarkerCalls(false);
                                        RestoreWeaponBank(actors::InGame() ? actors::Player(0) : 0);
                                    }
                                    g_shotWindowOn = on;
                                    if (!on) StopQuickShot();
                                    UsePadMaskForQuickShot(PadMaskWanted());
                                    return true;
                                },
                                .available = [] { return game::IsSupported(); },
                                .value = 150, .valueMin = 30, .valueMax = 500, .valueFormat = "%.0f ms" });
    {
        Feature* const window = &shotWindow;
        shotWindow.tick = [window] {
            g_shotWindowMs.store(window->value, std::memory_order_relaxed);
            TickQuickShot();
        };
    }
    Add({ .id = "player.quick_shot_state", .name = "État de l'ennemi touché (tir rapide RE6)",
          .help = "Le prompt de corps à corps ouvert sur le Majini touché par le tir rapide façon RE6.",
          .category = "Joueur", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_shotState = index;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 8, .valueFormat = "%.0f",
          .choices = { "Head flash", "Recul + head flash (comme la parade)", "Arm", "Leg", "Finisher",
                       "Recul + Arm (Arm Front de face, Arm Back de dos)",
                       "Recul + Arm Front (natif : head flash de dos)",
                       "Recul + Leg (Leg Front de face, Leg Back de dos)",
                       "Recul + Leg Front (de face comme de dos)" } });
    g_knifeAimFeature = &Add({ .id = "player.knife_aim_key", .name = "Viser avec le couteau (touche)",
                               .help = "Choisis une touche dans la colonne des raccourcis (Souris 4 ou 5 par exemple, que "
                                       "le jeu ne reconnaît pas) : tant qu'elle est tenue, Chris vise avec le couteau, "
                                       "comme avec la touche du jeu. La case active ou coupe cette touche.",
                               .category = "Couteau : rapide", .kind = Kind::Toggle,
                               .set = [](bool on) {
                                   if (on && !EnsureQueryHooks()) return false;
                                   g_knifeAimKey = on;
                                   return true;
                               },
                               .available = [] { return game::IsSupported(); },
                               .hotkey = { VK_XBUTTON1 },  // Souris 4 (user's choice)
                               .defaultOn = true, .holdKey = true });
    Add({ .id = "ammo.reload_key", .name = "Recharger (raccourci)",
          .help = "Recharge l'arme avec le raccourci choisi (une touche du clavier, de la souris ou un bouton de manette). "
                  "Utile à la manette avec « Pas de rechargement avec la course », le jeu y rechargeant avec viser + courir.",
          .category = "Munitions", .kind = Kind::Action,
          .run = [] {
              if (!EnsureQueryHooks()) return false;
              // A pickup the inventory has no room for does not count (user, 2026-10-23).
              if (g_reloadNotOnPrompt.load(std::memory_order_relaxed) && PickupPromptShown() && !NearbyItemHasNoRoom()) {
                  LOG("reload shortcut: not while a pickup prompt is shown");
                  return true;
              }
              g_reloadUntil = GetTickCount64() + 100;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "player.traversal_key", .name = "Franchissement (raccourci)",
          .help = "Déclenche le prompt de franchissement affiché (échelle, saut vers le bas ou vers l'avant, enjamber, "
                  "fenêtre...) et rien d'autre : ce raccourci ne ramasse jamais d'objet et ne lance aucun coup de corps "
                  "à corps (bonus de temps des Mercenaries compris). Sans prompt de franchissement, il ne fait rien.",
          .category = "Commandes", .kind = Kind::Action,
          .run = [] {
              PressTraversalPrompt();
              return true;
          },
          .available = [] { return game::IsSupported() && actors::InGame(); } });
    Add({ .id = "player.traversal_on_a", .name = "Franchissements sur A (manette)",
          .help = "Les prompts de franchissement (échelles, sauts, enjamber, monter, descendre, fenêtres) demandent A au "
                  "lieu du bouton d'action, et le jeu accepte A pour franchir. Les ramassages, le corps à corps et les "
                  "bonus de temps gardent le bouton d'action.",
          .category = "Commandes", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureQueryHooks()) return false;  // the icons are written on the game thread
              g_traversalIcon = on ? kTraversalIconA : 0u;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "player.all_actions_on_a", .name = "Toutes les actions sur A (manette)",
          .help = "Échange vraiment A et X pour le jeu, comme dans RE4 : A devient le bouton d'action (ramasser, "
                  "franchir, corps à corps...) et X le bouton de course (demi-tour, rechargement en visant). Les prompts "
                  "affichent le bon bouton. Tes raccourcis du trainer restent sur les vrais boutons de la manette.",
          .category = "Commandes", .kind = Kind::Toggle,
          .set = [](bool on) { return (!on || EnsureQueryHooks()) && SetAllOnA(on); },
          .available = [] { return game::IsSupported(); } });
    // The pad's minimap and partner location buttons kept from the game (user, 2026-11-06: "I want those buttons for
    // more interesting things, the smart heal, the quick knife - ignored on the pad, the attack button being the
    // location's"; the pad only, never the keyboard / mouse). On the user's control type the minimap is RB and the
    // partner location RT. The pad's buttons do not go through the game's button queries (diag.pad_log saw none): they
    // are taken off its XInput state (dinput_hook.cpp, source 2), the trainer's shortcuts still reading the real pad. RB:
    // in play on foot, the inventory closed. RT: the same, and only while player 1 does not aim (actions 0x1B weapon
    // ready .. 0x27 knife attack): aiming, RT fires; on a turret or in a vehicle (not on foot) it fires too.
    static std::atomic<bool> padNoMinimap{ false }, padNoLocate{ false };
    Add({ .id = "player.pad_no_minimap", .name = "Manette : RB n'ouvre plus la mini-carte",
          .help = "Le jeu ne voit plus RB de la manette en jeu (à pied, inventaire fermé) : la mini-carte ne s'ouvre plus et "
                  "RB reste libre pour tes raccourcis du trainer (soin intelligent...). Le clavier et la souris ne changent "
                  "pas.",
          .category = "Commandes", .kind = Kind::Toggle,
          .set = [](bool on) {
              padNoMinimap = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "player.pad_no_partner_locate", .name = "Manette : RT ne localise plus le partenaire",
          .help = "Le jeu ne voit plus RT de la manette quand le joueur 1 ne vise pas (à pied, inventaire fermé) : la "
                  "localisation du partenaire ne s'affiche plus et RT reste libre pour tes raccourcis du trainer (couteau "
                  "rapide...). En visant, RT tire toujours, comme aux tourelles et dans les véhicules. Le clavier et la "
                  "souris ne changent pas.",
          .category = "Commandes", .kind = Kind::Toggle,
          .set = [](bool on) {
              padNoLocate = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    // Their functions on other keys (user, 2026-11-07: "the point of blocking them is to put them on other buttons"): while
    // the chosen key is held the game sees the pad's RB / RT (dinput_hook.cpp, after the hidden buttons); the partner
    // location out of the aim only (aiming, RT fires).
    static Feature* minimapKey = &Add({ .id = "player.pad_minimap_key", .name = "Mini-carte (touche)",
                                        .help = "Une touche ou un bouton de manette de ton choix (colonne Raccourci) qui fait "
                                                "pour le jeu ce que fait RB de la manette : la mini-carte. Avec « Manette : RB "
                                                "n'ouvre plus la mini-carte », RB est libre pour tes raccourcis.",
                                        .category = "Commandes", .kind = Kind::Toggle,
                                        .set = [](bool) { return true; },
                                        .available = [] { return game::IsSupported(); }, .holdKey = true });
    static Feature* locateKey = &Add({ .id = "player.pad_partner_locate_key", .name = "Localiser le partenaire (touche)",
                                       .help = "Une touche ou un bouton de manette de ton choix (colonne Raccourci) qui fait "
                                               "pour le jeu ce que fait RT hors visée : la localisation du partenaire, après "
                                               "l'avoir tenue le temps du curseur (comme dans RE6 : un simple appui sur la "
                                               "touche, par exemple l'ordre au partenaire, ne la déclenche pas). Avec "
                                               "« Manette : RT ne localise plus le partenaire », RT est libre hors visée.",
                                       .category = "Commandes", .kind = Kind::Toggle,
                                       .set = [](bool) { return true; },
                                       .available = [] { return game::IsSupported(); },
                                       .value = 1.0f, .valueMin = 0.0f, .valueMax = 3.0f, .valueFormat = "%.1f s",
                                       .holdKey = true });
    OnFrame([] {
        const auto held = [](const Feature* f) {
            return f->enabled && (hotkeys::Held(f->hotkey) || hotkeys::Held(f->hotkey2));
        };
        uint32_t injected = 0;
        const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
        if (p1 && actors::OnFoot(p1) && !Player1InventoryOpen()) {
            uint32_t action = 0;
            mem::Read(p1 + 0x10E0, action);
            if (held(minimapKey)) injected |= 0x0200;  // RB
            // After the key is held the slider's time (user, 2026-11-07: on the partner's order button, as RE6, "the camera
            // moves as soon as I touch it; in RE6 the location starts after a second's hold").
            static ULONGLONG since = 0;
            if (!held(locateKey)) since = 0;
            else if (!since) since = GetTickCount64();
            const bool long_enough = since && GetTickCount64() - since >= static_cast<ULONGLONG>(locateKey->value * 1000.0f);
            if (long_enough && !(action >= 0x1B && action <= 0x27)) injected |= 0x20000;  // RT
        }
        hooks::InjectPadButtons(injected);
    });
    OnFrame([] {
        uint32_t mask = 0;
        const bool minimap = padNoMinimap.load(std::memory_order_relaxed), locate = padNoLocate.load(std::memory_order_relaxed);
        const bool knifeRt = KnifeKeyOnRt();  // "Couteau rapide (touche)" on RT
        const uintptr_t p = (minimap || locate || knifeRt) && actors::InGame() ? actors::Player(0) : 0;
        if (p && actors::OnFoot(p) && !Player1InventoryOpen()) {
            uint32_t action = 0;
            mem::Read(p + 0x10E0, action);
            const bool aiming = action >= 0x1B && action <= 0x27;
            // A melee prompt the fire key takes ("Corps à corps sur la touche de tir": on a pad the game accepts it with RT).
            const bool firePrompt = g_meleeOnFire.load(std::memory_order_relaxed) && MeleePromptShown();
            if (minimap) mask |= 0x0200;                                   // XINPUT_GAMEPAD_RIGHT_SHOULDER
            if ((locate || knifeRt) && !aiming && !firePrompt) mask |= 0x20000;  // the right trigger
        }
        hooks::HidePadButtons(2, mask);
    });
    // Every icon of the icon -> mask table (VA 0xF58E50, 30 entries: 0..0x1D; user, 2026-10-23: is one of them the left
    // click with the keyboard? icon 6 draws the right click there and the fire trigger on a pad).
    Add({ .id = "diag.prompt_icon_test", .name = "Essai : icône des prompts (recherche)",
          .help = "Met l'icône choisie sur tout prompt d'action affiché (et le bouton qu'il demande), pour trouver "
                  "l'icône d'un bouton. « Aucune » rend leur icône aux prompts.",
          .category = "Diagnostic", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              if (index && !EnsureQueryHooks()) return false;
              g_iconTest = index ? static_cast<uint32_t>(index - 1) : 0u;
              g_iconTestOn = index != 0;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 30, .valueFormat = "%.0f",
          .choices = { "Aucune", "Icône 0 (masque 0x0)", "Icône 1 (masque 0x8000)", "Icône 2 (masque 0x1000)",
                       "Icône 3 (masque 0x4000)", "Icône 4 (masque 0x2000)", "Icône 5 (masque 0x400)",
                       "Icône 6 (masque 0x800)", "Icône 7 (masque 0x100)", "Icône 8 (masque 0x200)",
                       "Icône 9 (masque 0x8000)", "Icône 10 (masque 0x1000)", "Icône 11 (masque 0x4000)",
                       "Icône 12 (masque 0x2000)", "Icône 13 (masque 0x9000)", "Icône 14 (masque 0x6000)",
                       "Icône 15 (masque 0xC000)", "Icône 16 (masque 0x3000)", "Icône 17 (masque 0xC00)",
                       "Icône 18 (masque 0x300)", "Icône 19 (masque 0xC00)", "Icône 20 (masque 0x0)",
                       "Icône 21 (masque 0x0)", "Icône 22 (masque 0x8000)", "Icône 23 (masque 0x1000)",
                       "Icône 24 (masque 0x4000)", "Icône 25 (masque 0x2000)", "Icône 26 (masque 0x0)",
                       "Icône 27 (masque 0x0)", "Icône 28 (masque 0x0)", "Icône 29 (masque 0x18001)" } });
    Add({ .id = "diag.prompt_log", .name = "Journal des prompts (recherche)",
          .help = "Écrit dans trainer.log chaque prompt d'action qui apparaît : son type, son icône, à qui il appartient "
                  "(ennemi, joueur, objet) et s'il compte comme un prompt de corps à corps.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_promptLog = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnFrame(TickPromptLog);
    g_knifeKeyFeature = &Add({ .id = "player.quick_knife_key", .name = "Couteau rapide (touche)",
                               .help = "Le couteau rapide à la manette : sans viser, la gâchette d'attaque (RT) donne un "
                                       "coup de couteau, comme le couteau rapide à la souris. Tenir enchaîne les coups ; en "
                                       "visant, RT tire normalement. Hors visée, le jeu ne voit plus RT (pas de "
                                       "localisation du partenaire), sauf quand un prompt de corps à corps est affiché "
                                       "avec « Corps à corps sur la touche de tir ». Une touche ou un bouton choisi dans la "
                                       "colonne Raccourci remplace RT (clavier compris).",
                               .category = "Mêlée", .kind = Kind::Toggle,
                               .set = [](bool on) {
                                   if (on && !EnsureQueryHooks()) return false;
                                   g_knifeKey = on;
                                   if (!on) g_knifeKeyEmulating = false;
                                   UsePadMaskForQuickShot(PadMaskWanted());
                                   return true;
                               },
                               .tick = TickKnifeKey,
                               .available = [] { return game::IsSupported(); },
                               .holdKey = true });
    Add({ .id = "player.melee_on_fire", .name = "Corps à corps sur la touche de tir",
          .help = "La touche de tir lance les prompts de corps à corps, comme dans RE6. À la manette ils affichent la touche "
                  "de tir et le jeu les accepte avec elle (plus avec le bouton d'action) ; au clavier ils affichent la "
                  "touche de tir (clic gauche) et elle les lance. Le tir sans viser et le tir rapide RE6 ne partent pas "
                  "tant qu'un prompt de corps à corps est affiché. En visant aussi (arme ou couteau), le prompt passe avant "
                  "le tir : la touche de tir le lance, sans tirer ni donner de coup de couteau.",
          .category = "Mêlée", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureGlyphHook() || !EnsureQueryHooks())) return false;  // the query hooks: boss prompts
              g_meleeOnFire = on;
              if (!on) g_meleeKeyboardIcon = false;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "ammo.reload_key_not_on_prompt", .name = "Recharger (raccourci) : pas pendant un prompt de ramassage",
          .help = "Le raccourci de rechargement ne fait rien tant qu'un prompt de ramassage est affiché. Utile quand il "
                  "partage le bouton d'action de la manette : ramasser ne recharge plus. Les autres prompts (corps à "
                  "corps, franchissements...) ne bloquent pas le rechargement, ni un objet pour lequel l'inventaire n'a "
                  "plus de place.",
          .category = "Munitions", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_reloadNotOnPrompt = on;
              return true;
          },
          .available = [] { return game::IsSupported(); },
          .defaultOn = true });
    Add({ .id = "diag.pad_log", .name = "Journal des boutons (recherche)",
          .help = "Écrit dans trainer.log chaque bouton du jeu lu comme appuyé, avec le code qui le lit.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureQueryHooks()) return false;
              g_padLog = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
}
}  // namespace features
