// Research: log the actions requested on players (to find melee/help swap sites, e.g. Sheva's knife
// slashes when a licker or parasite grabs Chris). Writes %LOCALAPPDATA%\RE5TrainerPlus\actions.log:
// one line per distinct (player, action id, requesting call site), with the bytes before the call so
// the `push <id>` to patch can be located.
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "features/melee_tuning.hpp"

#include <windows.h>
#include <intrin.h>
#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace features {
const char* UnitClassName(uintptr_t object);  // parry.cpp: its DTI class name
void OnPlayerAction(uintptr_t player, int action);  // swaps.cpp
int SoloComboAction(uintptr_t player, int action);   // combo.cpp
bool PickupRefuses(uintptr_t player, int action, uintptr_t caller);  // pickup.cpp: done without its animation
bool RocketSlashRefuses(uintptr_t player, int action);  // rocket_catch.cpp: no shot over the slash that sends a rocket back
bool PlayerHitLogOn();  // player.cpp: diag.player_hit_log
void OnEnemyGrabRequest(uintptr_t enemy, int action);  // grabs.cpp
bool WeskerPuppetRequest(uintptr_t enemy, int* action);  // wesker_moves.cpp: the Executioner in Jill's throw
bool ParryRefusesEnemyAction(uintptr_t enemy, int action);  // parry.cpp: an Executioner's lift on a parried catch
bool ParryRefusesPlayerAction(uintptr_t player, int action);  // parry.cpp: a startle after a parried spider's fall
bool QuickShotRefusesPlayerAction(uintptr_t player, int action);  // quick_knife.cpp: no turns during RE6's shot motion
bool KnifeSlotEmptyRefuses(uintptr_t player, int action);    // inventory.cpp: knife slot "Rien"
bool SheathGunEmptyRefuses(uintptr_t player, int action);    // inventory.cpp: the knife slot's gun empty
void HydraReloadBank(uintptr_t player, int action);          // quick_knife.cpp: the Hydra's own reload motions
bool KnifeUnusable(int playerIndex);                         // inventory.cpp: knife slot "Rien" or its knife broken
bool HoldsInventoryKnife(uintptr_t player);                  // inventory.cpp: a knife from the inventory in hand
bool JillKeptDown(uintptr_t enemy, int action, uintptr_t partner);  // wesker_boss.cpp: "Jill reste au sol..."
bool LinWeskerStays(uintptr_t enemy, int action);  // wesker_boss.cpp: "Wesker s'entête (Lost in Nightmares)"
void DoorActionRequested(uintptr_t unit, int action);  // camera.cpp: Lost in Nightmares' doors filmed by the game
void TrapLeverRequested(uintptr_t unit, int action);  // coop_solo.cpp: the trap lever pulled before the trap
bool PlayerInvincible(int index);                   // player.cpp
void ExecutionerReleaseReaction(uintptr_t enemy, int* action, uintptr_t caller);  // licker.cpp: the release's reaction
void FatalExecution(uintptr_t enemy, int* action, int* other);  // licker.cpp: a lift on a dying player 1 -> execution
bool Re6LocomotionOn();  // movement.cpp: "Déplacement selon la direction (RE6)" on (no quick turn in RE6)
void Re6QuickTurnAsked();  // movement.cpp: the quick turn's input turns the camera there
void BigMajiniKeepsFooting(uintptr_t enemy, int* action);   // parry.cpp: their blow-off replaced
void ChainsawLegsAfterHit(uintptr_t enemy, int* action);    // parry.cpp: a parried chainsaw's leg stagger after its hit
bool MeleeSwapThrow(uintptr_t player, int action, uintptr_t caller);  // swaps.cpp: a melee slot swapped to Jill's throw
bool MeleeWeaponSteal(uintptr_t player, int action, uintptr_t caller);  // swaps.cpp: RE6's weapon steal on an armed Majini
bool MeleeEnvExecution(uintptr_t player, int action, uintptr_t caller);  // swaps.cpp: RE6's execution against an obstacle
bool EnvHeightRequest(uintptr_t player, int action);                     // swaps.cpp: RE6's execution from a height
bool Re6GroundRequest(uintptr_t player, int action);                 // wesker_moves.cpp: RE6's Ground with a two-handed weapon / knife
void MeleeSwapBorrow(uintptr_t player, int action, uintptr_t caller);  // swaps.cpp: a slot swapped to Josh's moves
bool StaminaStrict();                  // stamina.cpp: "Endurance stricte"
bool StaminaAllows();                  // stamina.cpp: the red arc not filling
void StaminaSpendNative();             // stamina.cpp: a game prompt's cost (strict)
void CoopSoloPressed(uintptr_t button, int slot);  // coop_solo.cpp: player 1's press on a co-op object
bool CoopSoloRedirect(uintptr_t button, int slot); // coop_solo.cpp: a jump's launcher press given to player 2
bool RaftRedirect(uintptr_t button, int slot);     // raft.cpp: player 1's order on the raft's crank, his own turn
bool BeltLampOn();                                 // belt_lamp.cpp: a belt lamp is lit (no lamp pickup for player 2)
bool CrankHoldsOpen(uintptr_t unit, int action, int a, int b, int c, int d);  // crank.cpp: a let-go crank's rewind
bool FloorWaits(uintptr_t unit, int action, int a, int b, int c, int d, uintptr_t caller);  // levers.cpp: a platform
bool MeleePromptButton(uintptr_t button);  // quick_knife.cpp: a melee prompt
bool FinalFightPartnerFree();               // final_fight.cpp: the partner's limits stand aside in the final fight

bool PlayerHeldByEnemy(uintptr_t p);  // camera.cpp: an enemy near him in a holding action (Catch / Hang / Hold / Mount)

namespace {
// uPlayer::RequestAction (player vtable +0xDC), RVA 0x75C900: void __thiscall(int action, int, int, int, int).
// Callers push the action id last, e.g. the Quick Turn callback: push 0; push -1; push 0; push 0; push 9.
using RequestActionFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
RequestActionFn g_requestAction = nullptr;
std::atomic<bool> g_logging{ false };
bool g_hooked = false;
std::mutex g_lock;
std::set<std::tuple<int, int, uintptr_t>> g_seen;

void Record(int player, int action, uintptr_t caller, bool refused = false) {
    std::lock_guard lock(g_lock);
    if (!g_seen.insert({ player, action | (refused ? 0x1000 : 0), caller }).second || g_seen.size() > 4000) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (logx::DataDir() + L"\\actions.log").c_str(), L"a") != 0 || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    fprintf(f, "%02d:%02d:%02d P%d action 0x%02X caller RVA %06X%s  bytes before call: %s\n", t.wHour, t.wMinute,
            t.wSecond, player + 1, action, static_cast<unsigned>(caller - base), refused ? " REFUSED" : "",
            mem::Hex(reinterpret_cast<void*>(caller - 24), 24).c_str());
    fclose(f);
}

// Enemies request their actions through the same RequestAction (RVA 0x75C900 -> 0x759030 -> SetAction 0x759340,
// +0x10E0 action, +0x10E4 previous, +0x10EC changed): logged by class vtable (licker pounce research, 2026-10-15).
void RecordEnemy(uintptr_t enemy, int action, uintptr_t caller) {
    uintptr_t vtable = 0;
    if (!mem::Read(enemy, vtable)) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    std::lock_guard lock(g_lock);
    if (!g_seen.insert({ 100 + static_cast<int>((vtable - base) & 0xFFFFFF), action, caller }).second || g_seen.size() > 4000)
        return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (logx::DataDir() + L"\\actions.log").c_str(), L"a") != 0 || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d E vt %06X (%08X) action 0x%02X caller RVA %06X  bytes before call: %s\n", t.wHour, t.wMinute,
            t.wSecond, static_cast<unsigned>(vtable - base), static_cast<unsigned>(enemy), action,
            static_cast<unsigned>(caller - base), mem::Hex(reinterpret_cast<void*>(caller - 24), 24).c_str());
    fclose(f);
}

// ---- Player 2 actions refused by category (user, 2026-10-14). Each player keeps its action objects in an array
// indexed by action id: [P+0x10DC] (177 entries, P+0x10E0 = current id). The class of an action is read through its
// GetDTI method (vtable slot 4, DTI +4 = name, "cPlAction_pl_act_..."). Categories:
//   attack  "attack_*" (punches, kicks, knife slashes, finishers) and "knife_ready_begin*" (taking the knife out),
//           except the rescues below;
//   pickup  "item_get_*" (pick up, pick up and use);
//   revive  "coop_treat" and "item_use_partner_*" (reviving / healing the partner);
//   rescue  "attack_*" against a licker or a plaga holding the partner, "attack_*_em10" and the rescue hits asked
//           by the grab code: a normal Majini grab is ended by "attack_sakotu" (0x37) through the handler RVA
//           0x75DD50 (called by the enemy, RVA 0x481505), upper/straight_em10 through RVA 0x75DB10 / 0x75DC30;
//           the RequestAction return addresses RVA 0x75DDD0 / 0x75DB93 / 0x75DCB3 tell them from plain attacks
//           (user, 2026-10-14: "n'attaque pas" blocked the Majini rescue).
std::atomic<uint32_t> g_p2Blocked{ 0 };  // bit per category
uint32_t P2Blocked() { return FinalFightPartnerFree() ? 0 : g_p2Blocked.load(std::memory_order_relaxed); }
std::atomic<DWORD> g_p1GrabTick{ 0 };     // when a grab (0x84 / 0x82) was requested for player 1, 0 = other action since
uintptr_t g_blockedFor = 0;  // action array the mask was built from
uint8_t g_blocked[256] = {};  // category bits per action id

const char* ActionClassName(uintptr_t object) {
    using GetDtiFn = void*(__fastcall*)(void* self, void* edx);
    __try {
        const uintptr_t vtable = *reinterpret_cast<uintptr_t*>(object);
        void* dti = reinterpret_cast<GetDtiFn>(*reinterpret_cast<uintptr_t*>(vtable + 4 * 4))(reinterpret_cast<void*>(object), nullptr);
        return dti ? *reinterpret_cast<const char**>(reinterpret_cast<uintptr_t>(dti) + 4) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Also the parasite's rescue handler RVA 0x75DF90 (its request 0x37 returns at RVA 0x75E010).
bool IsRescueCall(uintptr_t caller) {
    const uintptr_t rva = caller - reinterpret_cast<uintptr_t>(mem::Base());
    return rva == 0x75DDD0 || rva == 0x75DB93 || rva == 0x75DCB3 || rva == 0x75E010;
}

// Exception to "no pickup" (user, 2026-10-15, the mines with the lamp): player 2 may still pick up the lamp (item
// 0x0130, held like a weapon, unit class uWp30, vtable RVA 0xC32780). The pickup handler (RVA 0x76D310) stores the
// target id at P+0x2E30 before asking item_get (RVA 0x76D383); sItem ([RVA 0xDB2200], units +0x30, count +0x24) holds
// the unit whose +0xFD0 is that id (the game's lookup is RVA 0x3BE7A0). Once she holds the lamp there is nothing else
// she may pick up: the block applies again.
// The item unit player `player` is about to pick up (0 if none found).
uintptr_t PickupTarget(uintptr_t player) {
    uint32_t id = 0, count = 0;
    uintptr_t items = 0, array = 0;
    if (!mem::Read(player + 0x2E30, id) || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2200)), items) || !items ||
        !mem::Read(items + 0x24, count) || !mem::Read(items + 0x30, array) || !array || count > 4096)
        return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t unit = 0;
        uint32_t unitId = 0;
        if (mem::Read(array + 4 * i, unit) && unit && mem::Read(unit + 0xFD0, unitId) && unitId == id) return unit;
    }
    return 0;
}

// The pickups player 2 may still make: the lamp (uWp30, vtable RVA 0xC32780) and the half of the gold emblem in Lost
// in Nightmares (uIt0526, vtable RVA 0xBB8A78; user, 2026-10-26: Jill, thrown up by Chris, opened its chest but could not
// take it, and nobody else can). Not the lamp while a belt lamp is lit (user, 2026-10-27: Sheva kept taking it).
bool PickupTargetIsLamp(uintptr_t player) {
    uintptr_t vtable = 0;
    const uintptr_t unit = PickupTarget(player);
    return unit && mem::Read(unit, vtable) &&
           ((vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xC32780)) && !BeltLampOn()) ||
            vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xBB8A78)));
}

// What a refused pickup of player 2 aimed at, once per target (user, 2026-10-26: found the gold emblem's half that way;
// another item only the partner can reach would show here).
void LogRefusedPickup(uintptr_t p2, int action) {
    static uint32_t lastId = 0xFFFFFFFF;
    uint32_t id = 0;
    mem::Read(p2 + 0x2E30, id);
    if (id == lastId) return;
    lastId = id;
    const uintptr_t unit = PickupTarget(p2);
    uintptr_t vtable = 0;
    if (unit) mem::Read(unit, vtable);
    const char* name = unit ? ActionClassName(unit) : nullptr;
    LOG("player 2: pickup %X refused, target id %X unit %08X (vtable RVA %06X, %s)", action, id, static_cast<unsigned>(unit),
        vtable ? static_cast<unsigned>(vtable - reinterpret_cast<uintptr_t>(mem::Base())) : 0u,
        name && !IsBadStringPtrA(name, 64) ? name : "?");
}

bool BlockedForPlayer2(void* self, int action, uintptr_t caller) {
    const uintptr_t p2 = actors::Player(1);
    if (reinterpret_cast<uintptr_t>(self) != p2 || action < 0 || action >= 256) return false;
    uintptr_t actions = 0;
    if (!mem::Read(p2 + 0x10DC, actions) || !actions) return false;
    if (actions != g_blockedFor) {
        g_blockedFor = actions;
        int count = 0;
        for (int i = 0; i < 256; ++i) {
            g_blocked[i] = 0;
            uintptr_t object = 0;
            if (i >= 177 || !mem::Read(actions + 4 * i, object) || !object) continue;
            const char* name = ActionClassName(object);
            if (!name || IsBadStringPtrA(name, 96)) continue;
            const bool attack = std::strstr(name, "pl_act_attack_") || std::strstr(name, "pl_act_knife_ready_begin");
            // knife_flying: the knife on a Kipepeo (uEm0c) holding the partner (user, 2026-11-06: "Sheva did not free me
            // from the flying parasite with 'n'attaque pas'").
            const bool grabbed = std::strstr(name, "licker") || std::strstr(name, "plaga") || std::strstr(name, "_em10") ||
                                 std::strstr(name, "knife_flying");
            // Her shots too (user, 2026-11-06: "she fired and lost ammunition": the fire functions refused by player.cpp's
            // hooks did not stop the shot nor its round): weapon_fire (0x1E) refused, never a rescue (bit 0x10; user:
            // "you must not allow the shots to free me").
            const bool shot = std::strstr(name, "pl_act_weapon_fire") != nullptr;
            if ((attack && !grabbed) || shot) g_blocked[i] |= 1;
            if (shot) g_blocked[i] |= 0x10;
            if (std::strstr(name, "pl_act_item_get_")) g_blocked[i] |= 2;
            if (std::strstr(name, "pl_act_coop_treat") || std::strstr(name, "pl_act_item_use_partner_")) g_blocked[i] |= 4;
            if (attack && grabbed) g_blocked[i] |= 8;
            count += g_blocked[i] != 0;
        }
        LOG("player 2 actions: %d classified", count);
    }
    uint8_t categories = g_blocked[action];
    if (categories & 1) {
        // An attack while player 1 is held by an enemy (the grab requests action 0x84 setEventAct_wait for player 1,
        // 0x82 for a plaga) is a rescue, whatever the enemy (Majini, dog, parasite...). Sheva answers in the same frame
        // as the grab, before P+0x10E0 changes: a grab requested for player 1 less than a second ago counts too
        // (2026-10-14: her parasite rescue, knife out 0x22 + attack_knife_flying 0x3E, was refused as an attack).
        uint32_t p1Action = 0;
        const uintptr_t p1 = actors::Player(0);
        const DWORD grab = g_p1GrabTick.load(std::memory_order_relaxed);
        // Or player 1 held in an event by an enemy near him in a holding action (a licker's mount, a Kipepeo's catch:
        // P+0x10E0 = 0x80000000, not 0x84 / 0x82; camera.cpp PlayerHeldByEnemy): her knife drawn for it too.
        if (!(categories & 0x10) &&
            (IsRescueCall(caller) || (grab && static_cast<int32_t>(GetTickCount() - grab) < 1000) ||
             (p1 && mem::Read(p1 + 0x10E0, p1Action) && (p1Action == 0x84 || p1Action == 0x82)) || PlayerHeldByEnemy(p1)))
            categories = 8;
    } else if (IsRescueCall(caller)) {
        categories = 8;
    }
    if ((categories & 2) && (P2Blocked() & 2) && PickupTargetIsLamp(p2)) categories &= ~2;
    const bool refused = (categories & P2Blocked()) != 0;
    if (refused && (categories & 2)) LogRefusedPickup(p2, action);
    return refused;
}

// Reviving / healing the partner starts in two handlers (cdecl, second argument = index of the helping player):
// coop_treat RVA 0x76D730 and item_use_partner RVA 0x76E400. They first put the partner in an event wait (target
// vtable +0xE0, P1 action 0x84 setEventAct_wait), then ask the helper's action: refusing only the action left Chris
// frozen, without health bar (user, 2026-10-14). The whole handler is skipped for player 2 instead.
using HelpHandlerFn = void(__cdecl*)(uintptr_t a, uintptr_t helper, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f);
HelpHandlerFn g_treatStart = nullptr;
HelpHandlerFn g_itemUsePartner = nullptr;

bool ReviveRefused(uintptr_t helper) {
    return helper == 1 && (P2Blocked() & 4) != 0;
}

void __cdecl HookTreatStart(uintptr_t a, uintptr_t helper, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) {
    if (ReviveRefused(helper)) return;
    g_treatStart(a, helper, c, d, e, f);
}

void __cdecl HookItemUsePartner(uintptr_t a, uintptr_t helper, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) {
    if (ReviveRefused(helper)) return;
    g_itemUsePartner(a, helper, c, d, e, f);
}

bool HookHandler(uintptr_t rva, void* detour, void** original, const char* what) {
    uint8_t* target = mem::Rva(rva);
    const uint8_t expected[] = { 0x8B, 0x44, 0x24, 0x08, 0x8B, 0x0D, 0xDC, 0x27, 0x1B, 0x01 };
    if (!mem::IsGameCode(target, sizeof(expected)) || memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("%s: unexpected bytes %s", what, mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    LOG("%s hook installed at %p", what, target);
    return true;
}

// A refused pickup left player 2 stuck in front of the item until called (and deaf to duo actions, user
// 2026-10-14): the pickup handlers (RVA 0x76D310...) end in RVA 0x76AF90 (thiscall, player), which marks a pickup
// in progress (P+0x2E34 = 1) that only the item_get action clears. Clearing it by hand freed her at once (verified
// live); the hook clears it after a refused pickup.
using PickupStartedFn = void(__fastcall*)(void* player, void* edx);
PickupStartedFn g_pickupStarted = nullptr;

void __fastcall HookPickupStarted(void* player, void* edx) {
    g_pickupStarted(player, edx);
    const uintptr_t p = reinterpret_cast<uintptr_t>(player);
    if (!(P2Blocked() & 2) || p != actors::Player(1)) return;
    uint32_t action = 0;
    if (!mem::Read(p + 0x10E0, action)) return;
    if ((action >= 0x57 && action <= 0x5A) || action == 0x65 || action == 0x66) return;  // a pickup really runs
    mem::Write(p + 0x2E34, static_cast<uint8_t>(0));
}

bool EnsurePickupHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x76AF90);
    const uint8_t expected[] = { 0x51, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x9C, 0x12, 0x00, 0x00 };
    if (!mem::IsGameCode(target, sizeof(expected)) || memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("pickup started: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookPickupStarted), reinterpret_cast<void**>(&g_pickupStarted));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("pickup started hook installed at %p", target);
    return true;
}

bool EnsureReviveHooks() {
    static bool hooked = false;
    if (!hooked)
        hooked = HookHandler(0x76D730, reinterpret_cast<void*>(&HookTreatStart), reinterpret_cast<void**>(&g_treatStart),
                             "partner treat") &&
                 HookHandler(0x76E400, reinterpret_cast<void*>(&HookItemUsePartner),
                             reinterpret_cast<void**>(&g_itemUsePartner), "partner item use");
    return hooked;
}

// Return addresses up the stack (direct calls E8, and indirect FF /2: call reg, [reg+d8], [reg+d32], [abs]); the
// indirect ones marked '*'. Stale words of older frames show up too.
std::string StackChain(const uint32_t* stack, int wanted) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    std::string out;
    int found = 0;
    for (int i = 0; i < 128 && found < wanted; ++i) {
        uint32_t v = 0;
        if (!mem::Read(reinterpret_cast<uintptr_t>(stack + i), v) || v < base + 0x1000 || v >= base + 0x900000) continue;
        uint8_t b[6]{};
        if (!mem::SafeRead(reinterpret_cast<void*>(v - 6), b, sizeof(b))) continue;
        const bool direct = b[1] == 0xE8;
        const bool indirect = (b[4] == 0xFF && (b[5] & 0xF8) == 0xD0) || (b[3] == 0xFF && (b[4] & 0xF8) == 0x50) ||
                              (b[0] == 0xFF && ((b[1] & 0xF8) == 0x90 || b[1] == 0x15));
        if (!direct && !indirect) continue;
        char text[16];
        std::snprintf(text, sizeof(text), " %06X%s", static_cast<unsigned>(v - base), direct ? "" : "*");
        out += text;
        ++found;
    }
    return out;
}

// No knife, no rescue with it (user, 2026-10-27: a knife broken by "Durabilité du couteau", knife slot "Rien"): the
// partner rescues that take the knife are refused to that player - a parasite (attack_knife_flying 0x3E, after the
// knife out 0x22 already refused), a licker's tongue (attack_knife_licker_tongue 0x3C), a plaga (attack_knife_plaga).
// By class name (the actions' DTI classes).
const char* RequestedActionName(uintptr_t unit, int action);  // below
bool KnifeRescueRefused(uintptr_t player, int action) {
    const int index = actors::PlayerIndex(player);
    if (index < 0 || index > 1 || !KnifeUnusable(index)) return false;
    const char* name = RequestedActionName(player, action);
    for (const char* rescue : { "attack_knife_flying", "attack_knife_licker_tongue", "attack_knife_plaga" })
        if (std::strstr(name, rescue)) {
            static DWORD logged = 0;
            if (GetTickCount() - logged > 2000) LOG("no knife: player %d's %s refused", index + 1, name);
            logged = GetTickCount();
            return true;
        }
    return false;
}

void __fastcall HookRequestAction(void* self, void* edx, int action, int a, int b, int c, int d) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (reinterpret_cast<uintptr_t>(self) == actors::Player(0)) {
        g_p1GrabTick = action == 0x84 || action == 0x82 ? (GetTickCount() | 1) : 0;
        if (PlayerHitLogOn())
            LOG("hit log: P1 request %X (%d) from RVA %06X | stack%s", action, a,
                static_cast<unsigned>(caller - reinterpret_cast<uintptr_t>(mem::Base())),
                StackChain(static_cast<const uint32_t*>(_AddressOfReturnAddress()), 10).c_str());
    }
    const bool refused = P2Blocked() && BlockedForPlayer2(self, action, caller);
    if (g_logging.load(std::memory_order_relaxed)) {
        const int player = actors::PlayerIndex(reinterpret_cast<uintptr_t>(self));
        if (player >= 0) Record(player, action, caller, refused);
    }
    if (refused || PickupRefuses(reinterpret_cast<uintptr_t>(self), action, caller) ||
        RocketSlashRefuses(reinterpret_cast<uintptr_t>(self), action) ||
        ParryRefusesPlayerAction(reinterpret_cast<uintptr_t>(self), action) ||
        QuickShotRefusesPlayerAction(reinterpret_cast<uintptr_t>(self), action) ||
        KnifeSlotEmptyRefuses(reinterpret_cast<uintptr_t>(self), action) ||
        SheathGunEmptyRefuses(reinterpret_cast<uintptr_t>(self), action) ||
        KnifeRescueRefused(reinterpret_cast<uintptr_t>(self), action))
        return;
    if (action == 0x09 && reinterpret_cast<uintptr_t>(self) == actors::Player(0) && Re6LocomotionOn()) {
        LOG("RE6 locomotion: quick turn refused");
        Re6QuickTurnAsked();
        return;
    }
    if (EnvHeightRequest(reinterpret_cast<uintptr_t>(self), action)) return;  // swaps.cpp: from a height (climb / jump prompts)
    if (MeleeEnvExecution(reinterpret_cast<uintptr_t>(self), action, caller)) return;  // swaps.cpp: against a wall / table / barrier
    if (MeleeWeaponSteal(reinterpret_cast<uintptr_t>(self), action, caller)) return;  // swaps.cpp: RE6's weapon steal
    if (MeleeSwapThrow(reinterpret_cast<uintptr_t>(self), action, caller)) return;  // swaps.cpp: Jill's throw
    MeleeSwapBorrow(reinterpret_cast<uintptr_t>(self), action, caller);              // swaps.cpp: Josh's moves
    action = SoloComboAction(reinterpret_cast<uintptr_t>(self), action);
    if (reinterpret_cast<uintptr_t>(self) == actors::Player(0) || reinterpret_cast<uintptr_t>(self) == actors::Player(1))
        OnPlayerAction(reinterpret_cast<uintptr_t>(self), action);  // swaps.cpp: target of a swapped melee move
    // melee_tuning.cpp, "Utiliser les back-up": a game move refused on its target, its back-up played instead.
    if (MeleeBackupRequest(reinterpret_cast<uintptr_t>(self), action)) return;
    if (Re6GroundRequest(reinterpret_cast<uintptr_t>(self), action)) return;
    HydraReloadBank(reinterpret_cast<uintptr_t>(self), action);
    g_requestAction(self, edx, action, a, b, c, d);
}

// ---- Motion log: which animation each character plays for each action -----------------------------
// Fed by the actor SetMotion hook of swaps.cpp (RVA 0x44CFF0, all animations): per character and action, the
// motion ids (bank << 8 | index) to build the moveset remap tables.
std::atomic<bool> g_motionLogging{ false };
std::set<std::tuple<int, int, int, uintptr_t>> g_motionSeen;
}

bool MotionLogEnabled() { return g_motionLogging.load(std::memory_order_relaxed); }

// Research (diag.boss_motion_log, user 2026-10-24: Jill's flips and somersaults at the start of her fight and her
// somersault attack, to give them to Chris later): every layer 0 motion Jill (uEm83, vtable RVA 0xB8EA40; uEm84 in the
// fight's second part, RVA 0xB90728: her throw, kick, wall run and cartwheel, user 2026-10-25) or Wesker (uEm80, RVA
// 0xB8BE60) starts, with its action (and its class name), its bank's motion list (unit +0xA88 + 4 * bank, path at +4,
// data at +0x60: entry offsets at +8, frames at entry +8) and its length, into trainer.log.
std::atomic<bool> g_bossMotionLogging{ false };
namespace {
const char* RequestedActionName(uintptr_t unit, int action);  // below
}  // namespace

void LogBossMotion(void* self, uint32_t layer, uint32_t motion) {
    if (!g_bossMotionLogging.load(std::memory_order_relaxed) || layer != 0) return;
    const uintptr_t unit = reinterpret_cast<uintptr_t>(self);
    uintptr_t vtable = 0;
    if (!mem::Read(unit, vtable)) return;
    const char* who = vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8EA40))   ? "Jill"
                      : vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB90728)) ? "Jill (2)"
                      : vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8BE60)) ? "Wesker"
                      : vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8C300)) ? "Wesker (final)"  // uEm81
                      : vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8C0B0)) ? "Wesker (LiN)"    // uEmA2
                                                                                  : nullptr;
    if (!who) return;
    static uintptr_t lastUnit = 0;
    static uint32_t lastMotion = 0;
    if (unit == lastUnit && motion == lastMotion) return;  // the same motion asked again
    lastUnit = unit;
    lastMotion = motion;
    uint32_t action = 0, frames = 0, entry = 0;
    mem::Read(unit + 0x10E0, action);
    const uint32_t bank = (motion >> 8) & 0xFF, index = motion & 0xFF;
    uintptr_t list = 0, data = 0;
    char path[64]{};
    if (bank < 16 && mem::Read(unit + 0xA88 + 4 * bank, list) && list) {
        mem::SafeRead(reinterpret_cast<void*>(list + 4), path, sizeof(path) - 1);
        if (mem::Read(list + 0x60, data) && data && mem::Read(data + 8 + 4 * index, entry) && entry)
            mem::Read(static_cast<uintptr_t>(entry) + 8, frames);  // the relocated entry pointer
    }
    LOG("boss motion: %s %08X action %X (%s) motion %X (%s slot %X, %u frames)", who, static_cast<unsigned>(unit), action,
        RequestedActionName(unit, static_cast<int>(action)), motion, path[0] ? path : "?", index, frames);
}

void LogMotion(void* self, uint32_t layer, uint32_t motion, uintptr_t caller) {
    const int player = actors::PlayerIndex(reinterpret_cast<uintptr_t>(self));
    if (player < 0 || player > 1) return;
    uintptr_t profile = 0;
    uint32_t character = 0xFF, action = 0;
    mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), profile);
    if (profile) mem::Read(profile + 0x23318 + 0x50 * player, character);
    mem::Read(reinterpret_cast<uintptr_t>(self) + 0x10E0, action);
    std::lock_guard lock(g_lock);
    if (g_motionSeen.size() >= 20000 ||
        !g_motionSeen.insert({ static_cast<int>(character), static_cast<int>(action), static_cast<int>(motion), caller }).second)
        return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (logx::DataDir() + L"\\motions.log").c_str(), L"a") != 0 || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d P%d char %u action 0x%02X layer %u motion 0x%03X caller RVA %06X\n", t.wHour, t.wMinute,
            t.wSecond, player + 1, character, action, layer, motion,
            static_cast<unsigned>(caller - reinterpret_cast<uintptr_t>(mem::Base())));
    fclose(f);
}

bool EnsureSetMotionHook();  // swaps.cpp

namespace {
// Enemies call the request core RVA 0x759030 directly (it is in their vtables; the players' RequestAction calls it
// too, from RVA 0x75C993): hooked for the log only, enemies only.
using RequestCoreFn = bool(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
RequestCoreFn g_requestCore = nullptr;

// diag.player_hit_log (player.cpp): the requests of the enemies within 8 m of player 1, in order.
void LogNearEnemyRequest(uintptr_t enemy, int action, uintptr_t caller, const uint32_t* stackTop) {
    const uintptr_t p1 = actors::Player(0);
    float a[3]{}, b[3]{};
    if (!p1 || !mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), a, sizeof(a)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), b, sizeof(b)))
        return;
    const float dx = a[0] - b[0], dz = a[2] - b[2];
    if (dx * dx + dz * dz > 800.0f * 800.0f) return;  // 8 m (units are centimetres)
    const char* name = ActionClassName(enemy);
    // The grab actions (Majini Catch 0x97 .. BackHoldDmg 0x9C) with their stack: who decides the hold.
    const std::string stack = action >= 0x97 && action <= 0x9C ? " | stack" + StackChain(stackTop, 12) : "";
    LOG("hit log: %s %08X request %X from RVA %06X%s", name && !IsBadStringPtrA(name, 64) ? name : "?",
        static_cast<unsigned>(enemy), action, static_cast<unsigned>(caller - reinterpret_cast<uintptr_t>(mem::Base())),
        stack.c_str());
}

// "Le joueur 2 n'attaque pas" (user, 2026-10-25: Sheva still threw Jill and did the combo on Wesker): those are the
// bosses' prompts, the boss asking its own action with the player who pressed as its partner (request argument b, as
// the trainer's throw asks Jill's Caught): Jill's Caught 0x27 / 0x2A (uEm83, vtable RVA 0xB8EA40) and Wesker's combo
// chain AtkSyutoRevGo 0x4C-0x4F / RevEnd 0x50 (uEm80, vtable RVA 0xB8BE60) refused with player 2 as the partner. Their
// requests naming a player are logged with diag.boss_motion_log (who the partner is).
// The class name of a unit's action `action` (its table +0x10D0 count / +0x10DC objects), "" if none.
const char* RequestedActionName(uintptr_t unit, int action) {
    uint32_t count = 0;
    uintptr_t table = 0, object = 0;
    if (action < 0 || !mem::Read(unit + 0x10D0, count) || static_cast<uint32_t>(action) >= count || count > 0x200 ||
        !mem::Read(unit + 0x10DC, table) || !table || !mem::Read(table + 4 * action, object) || !object)
        return "";
    const char* name = UnitClassName(object);
    return name ? name : "";
}

bool BossMoveOfBlockedPlayer2(uintptr_t enemy, int action, int a, int b, uintptr_t caller) {
    uintptr_t vtable = 0;
    if (!mem::Read(enemy, vtable)) return false;
    const bool jill1 = vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8EA40));
    // Jill in the fight's second part (uEm84; user, 2026-10-25: Sheva threw and grabbed her there): her moves with a
    // player by their names.
    const char* cls = jill1 ? nullptr : UnitClassName(enemy);
    const bool jill2 = cls && std::strncmp(cls, "uEm84", 5) == 0;
    const bool jill = jill1 || jill2;
    const bool wesker = vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8BE60)) ||
                        vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8C300)) ||  // uEm80, the final uEm81,
                        vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8C0B0));    // Lost in Nightmares' uEmA2
    if (!jill && !wesker) return false;
    const uintptr_t other = static_cast<uintptr_t>(static_cast<uint32_t>(b));
    const int who = other ? actors::PlayerIndex(other) : -1;
    if (who >= 0 && g_bossMotionLogging.load(std::memory_order_relaxed))
        LOG("boss request: %s %08X action %X (a %d) partner player %d, from RVA %06X", jill ? "Jill" : "Wesker",
            static_cast<unsigned>(enemy), action, a, who + 1, static_cast<unsigned>(caller - reinterpret_cast<uintptr_t>(mem::Base())));
    if (who != 1 || !(P2Blocked() & 1)) return false;
    // Jill's Pinioned 0x28 / PinionedAtk 0x29 too (user, 2026-10-25: Sheva still grabbed her; logged: 0x28 with player 2 as
    // the partner, from RVA 0x52FFB3).
    bool move = jill1 ? action >= 0x27 && action <= 0x2A : wesker && action >= 0x4C && action <= 0x50;
    if (jill2) {
        const char* name = RequestedActionName(enemy, action);
        for (const char* withPlayer : { "Caught", "Pinion", "Strip" }) move = move || std::strstr(name, withPlayer);
    }
    if (move) LOG("player 2: %s's %X with her refused (no attack)", jill ? "Jill" : "Wesker", action);
    return move;
}

// A Wesker's synchronised kill of an invincible player (AtkSyutoDie 0x4A after his chop, the DLC's Syuto_OK 0x83) is
// refused: the player cannot die, the event never ends (user, 2026-10-26: Jill invincible, soft lock).
bool WeskerKillOnInvinciblePlayer(uintptr_t enemy, int action) {
    if (action != 0x4A && action != 0x83) return false;
    uintptr_t vtable = 0, target = 0;
    if (!mem::Read(enemy, vtable)) return false;
    const uintptr_t rva = vtable - reinterpret_cast<uintptr_t>(mem::Base());
    if ((rva != 0xB8BE60 && rva != 0xB8C300 && rva != 0xB8C0B0) || (action == 0x83 && rva != 0xB8C0B0)) return false;
    if (!mem::Read(enemy + 0x1BA4, target) || !target) return false;
    const int player = actors::PlayerIndex(target);
    if (player < 0 || !PlayerInvincible(player)) return false;
    LOG("Wesker %08X's kill %X on invincible player %d refused", static_cast<unsigned>(enemy), action, player + 1);
    return true;
}

bool __fastcall HookRequestCore(void* self, void* edx, int action, int a, int b, int c, int d) {
    if (g_logging.load(std::memory_order_relaxed) && actors::PlayerIndex(reinterpret_cast<uintptr_t>(self)) < 0)
        RecordEnemy(reinterpret_cast<uintptr_t>(self), action, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if (PlayerHitLogOn() && actors::PlayerIndex(reinterpret_cast<uintptr_t>(self)) < 0)
        LogNearEnemyRequest(reinterpret_cast<uintptr_t>(self), action, reinterpret_cast<uintptr_t>(_ReturnAddress()),
                            static_cast<const uint32_t*>(_AddressOfReturnAddress()));
    if (BossMoveOfBlockedPlayer2(reinterpret_cast<uintptr_t>(self), action, a, b, reinterpret_cast<uintptr_t>(_ReturnAddress())))
        return false;
    if (JillKeptDown(reinterpret_cast<uintptr_t>(self), action, static_cast<uintptr_t>(static_cast<uint32_t>(b))))
        return false;  // wesker_boss.cpp
    if (LinWeskerStays(reinterpret_cast<uintptr_t>(self), action)) return false;  // wesker_boss.cpp
    DoorActionRequested(reinterpret_cast<uintptr_t>(self), action);
    TrapLeverRequested(reinterpret_cast<uintptr_t>(self), action);
    if (CrankHoldsOpen(reinterpret_cast<uintptr_t>(self), action, a, b, c, d)) return false;  // crank.cpp
    if (FloorWaits(reinterpret_cast<uintptr_t>(self), action, a, b, c, d, reinterpret_cast<uintptr_t>(_ReturnAddress())))
        return false;  // levers.cpp: a platform's departure delayed
    if (WeskerKillOnInvinciblePlayer(reinterpret_cast<uintptr_t>(self), action)) return false;
    // wesker_moves.cpp: an Executioner in Jill's throw plays its phases (refused or replaced).
    if (!WeskerPuppetRequest(reinterpret_cast<uintptr_t>(self), &action)) return false;
    if (ParryRefusesEnemyAction(reinterpret_cast<uintptr_t>(self), action)) return false;
    BigMajiniKeepsFooting(reinterpret_cast<uintptr_t>(self), &action);  // parry.cpp: no big Majini blown off
    FatalExecution(reinterpret_cast<uintptr_t>(self), &action, &b);
    ExecutionerReleaseReaction(reinterpret_cast<uintptr_t>(self), &action, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    ChainsawLegsAfterHit(reinterpret_cast<uintptr_t>(self), &action);   // parry.cpp: "Vacille (jambes)"
    MeleeEscapeRequest(reinterpret_cast<uintptr_t>(self), &action);     // melee_tuning.cpp: the back escape's Majini
    const bool result = g_requestCore(self, edx, action, a, b, c, d);
    OnEnemyGrabRequest(reinterpret_cast<uintptr_t>(self), action);  // grabs.cpp: "Saisies améliorées"
    return result;
}

// Wesker's own prompts (user, 2026-10-25: Sheva still did the combo on him; his chain's requests name no player): their
// handlers (cdecl: the prompt's owner, the index of the player who pressed) do nothing for player 2 with "Le joueur 2
// n'attaque pas": the combo on him stunned (RVA 0x50C6C0, Shrink's prompt: the player into the event 0x11F108C, then his
// chain) and the counter (RVA 0x50C4C0, AtkSH_Nee's).
using PromptHandlerFn = void(__cdecl*)(uintptr_t owner, int player, int c, int d);
PromptHandlerFn g_weskerCombo = nullptr, g_weskerCounter = nullptr;

bool Player2PromptRefused(int player, const char* what) {
    if (!actors::SlotIs(player, 1) || !(P2Blocked() & 1)) return false;
    static DWORD logged = 0;
    if (GetTickCount() - logged > 2000) LOG("player 2: Wesker's %s prompt refused (no attack)", what);
    logged = GetTickCount();
    return true;
}

void __cdecl HookWeskerCombo(uintptr_t owner, int player, int c, int d) {
    if (!Player2PromptRefused(player, "combo")) g_weskerCombo(owner, player, c, d);
}

void __cdecl HookWeskerCounter(uintptr_t owner, int player, int c, int d) {
    if (!Player2PromptRefused(player, "counter")) g_weskerCounter(owner, player, c, d);
}

// "Injection de Wesker sans le partenaire" (user, 2026-10-26: with "Le joueur 2 n'attaque pas" the final fight could not
// end: the injection takes two players). The final Wesker (uEm81, vtable RVA 0xB8C300) on his knees (Hiza 0x67: its
// prompt kind 0x19, handler RVA 0x50C520, cdecl(prompt, player slot) -> RVA 0x508470(player, 1) -> his vfunc +0x234
// RVA 0x5085E0: Pinion 0x6A with that player). Pinion's init (RVA 0x51B7A0) keeps the holder at +0x1F64, its update
// (RVA 0x511E70) opens the injection prompt for the other one (handler RVA 0x50C590 -> mode 2: Injection 0x6C with that
// player, refused unless he is in Pinion; Injection's init RVA 0x51BB00 needs the holder) in the Pinion object +0xAC,
// for about 1 s (logged: Pinion, then PinionEnd 0x6B 0.85 s later without an injection). With the option, player 1's
// press makes the partner hold him (the handler run for the partner's slot), Sheva's own press is let through, and once
// the partner holds him the injection prompt is pressed for player 1 (TickSoloInjection): Chris injects.
std::atomic<bool> g_soloInjection{ false };
PromptHandlerFn g_weskerKneel = nullptr;

bool IsFinalWesker(uintptr_t unit) {
    uintptr_t vtable = 0;
    return unit && mem::Read(unit, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xB8C300));
}

// The other player's slot, -1 if none.
int PartnerSlot(int slot) {
    for (int s = 0; s < 2; ++s)
        if (s != slot && actors::Slot(s)) return s;
    return -1;
}

// HookOnPressed: player 2's press on the kneeling final Wesker's prompt let through.
bool KneelPromptForPartner(uintptr_t owner) {
    uint32_t action = 0;
    return g_soloInjection.load(std::memory_order_relaxed) && IsFinalWesker(owner) && mem::Read(owner + 0x10E0, action) &&
           action == 0x67;
}

}  // namespace

bool Player2Invisible();  // player.cpp: "Joueur 2 invisible"

// The actor SetMotion hook (swaps.cpp): the final Wesker held by an invisible partner keeps kneeling (user, 2026-10-26:
// "the illusion that Sheva does not hold him and Chris manages alone"). Pinion 0x6A plays em80 0x6E (taken, 150 frames)
// then 0x6F (held, 51, the injection in it; logged), PinionEnd 0x6B 0x70 (130): they become Hiza's kneeling loop 0x73
// and its getup 0x75. Not when player 1 holds him.
uint32_t InvisibleHolderMotion(uintptr_t unit, uint32_t layer, uint32_t motion) {
    if (layer != 0 || !Player2Invisible() || !IsFinalWesker(unit)) return motion;
    uint32_t action = 0;
    uintptr_t holder = 0;
    if (!mem::Read(unit + 0x10E0, action) || action < 0x6A || action > 0x6C || !mem::Read(unit + 0x1F64, holder) ||
        (holder && holder == actors::Player(0)))
        return motion;
    const uint32_t index = motion & 0xFFFF;
    const uint32_t kneel = index == 0x6E || index == 0x6F ? 0x73 : index == 0x70 ? 0x75 : 0;
    if (!kneel) return motion;
    LOG("solo injection: Wesker's motion %X -> %X (his holder invisible)", index, kneel);
    return (motion & ~0xFFFFu) | kneel;
}

namespace {
void __cdecl HookWeskerKneel(uintptr_t prompt, int player, int c, int d) {
    const int partner = g_soloInjection.load(std::memory_order_relaxed) && actors::SlotIs(player, 0) ? PartnerSlot(player) : -1;
    if (partner >= 0) {
        LOG("solo injection: player 1 pressed the kneeling Wesker's prompt, the partner (slot %d) holds him", partner);
        g_weskerKneel(prompt, partner, c, d);
        return;
    }
    g_weskerKneel(prompt, player, c, d);
}

// Jill in the fight's second part (uEm84): her press function RVA 0x5395F0 (thiscall(Jill, player index, kind) ret 8:
// from her prompt handler RVA 0x53C210 and her other partner calls; through her vfunc +0x230 / +0x234 the kind is her
// move with that player, 0 Caught, 1 Pinioned, 2 Stripped...) first claims her current attack's slot for that player
// ([Jill+0xD80] at RVA 0x5DB070 -> 0x3F2E80: taken by the first player only, until that attack ends), then asks the move.
// Player 2's move refused afterwards by the request hook left her claim: player 1's throw prompt on Jill's kick was
// pressed and did nothing (user, 2026-10-25: the native throw once, then no more; Sheva's AI tries on each kick). Player
// 2's presses are refused there with "Le joueur 2 n'attaque pas".
using JillPressFn = void(__fastcall*)(void* jill, void* edx, int player, int kind);
JillPressFn g_jillPress = nullptr;

// Her attack's claim byte (the players' bits; 0 = free), 0 when she has no slot.
uintptr_t JillClaimByte(uintptr_t jill) {
    uintptr_t manager = 0, table = 0;
    uint32_t slot = 0;
    if (!mem::Read(jill + 0xD80, slot) || !slot || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1FCC)), manager) ||
        !manager || !mem::Read(manager + 0x80, table) || !table)
        return 0;
    return table + slot * 12 + 4;
}

void __fastcall HookJillPress(void* jill, void* edx, int player, int kind) {
    const bool p2Blocked = (P2Blocked() & 1) != 0;
    if (actors::SlotIs(player, 1) && p2Blocked) {
        static DWORD logged = 0;
        if (GetTickCount() - logged > 2000) LOG("player 2: Jill's move %d with her refused at her press (no attack)", kind);
        logged = GetTickCount();
        return;
    }
    const bool player1 = actors::SlotIs(player, 0);
    const uintptr_t j = reinterpret_cast<uintptr_t>(jill), claim = player1 ? JillClaimByte(j) : 0;
    uint8_t before = 0;
    if (claim && mem::Read(claim, before) && before == 2 && p2Blocked) {
        mem::Write<uint8_t>(claim, 0);  // player 2's (her AI's other claims), useless while she may not attack
        LOG("player 2: her claim on Jill's attack released for player 1");
    }
    uint32_t action = 0;
    g_jillPress(jill, edx, player, kind);
    uint8_t after = 0;
    if (player1 && mem::Read(j + 0x10E0, action))
        LOG("Jill press: player 1, move %d, her attack's slot %s (claim %u -> %u), her action now %X", kind,
            claim ? "found" : "none", before, claim && mem::Read(claim, after) ? after : 0u, action);
}

// cActionButton::OnPressed (RVA 0x7C3360, thiscall(button, player index) ret 4; its one caller the prompt detector, RVA
// 0x3675C5, for the AI partner too) sets the button's pressed flags +0xB / +0xC before its handler: a pressed button is
// skipped by the detector until re-armed (RVA 0x366F12). Player 2 refused at Jill's press only, her press still took the
// prompt: player 1's first presses on Jill's throw after her stun did nothing (user, 2026-10-25: it took several). With
// "Le joueur 2 n'attaque pas", player 2 never presses a prompt owned by an enemy (Jill's, Wesker's).
using OnPressedFn = void(__fastcall*)(void* button, void* edx, int player);
OnPressedFn g_onPressed = nullptr;

// The Weskers' defensive prompts (user, 2026-10-26: Jill, Wesker's target, could not dodge his chop - her press refused
// here - and his kill AtkSyutoDie 0x4A soft-locked the game, she being invincible). Their handlers (button +0x3C), pushed
// in his actions' code: 0x50C430 (a dodge reading the directions), 0x50C730, 0x50C660 (out of his grab), 0x50C790 (the
// DLC's lunge FirstAtkBgn / FirstAtk / Syuto: the back roll), 0x50C830 (Syuto_OK / Humithuke); the attacks stay
// refused: combos 0x50C600 / 0x50C6C0, the counter 0x50C4C0, the kneel 0x50C520 (unless solo injection), the injection.
bool DefensiveWeskerPrompt(uintptr_t button) {
    uintptr_t handler = 0;
    if (!mem::Read(button + 0x3C, handler) || !handler) return false;
    const uintptr_t rva = handler - reinterpret_cast<uintptr_t>(mem::Base());
    return rva == 0x50C430 || rva == 0x50C730 || rva == 0x50C660 || rva == 0x50C790 || rva == 0x50C830;
}

// "Endurance stricte" (stamina.cpp, user 2026-10-26): player 1's presses on the game's melee prompts refused while the
// red arc fills, else costing 500 each; not Wesker's defensive prompts (the game's dodges stay free: user), nor his
// kneel 0x50C520 or injection 0x50C590 (the fight's end).
bool StrictMeleePrompt(uintptr_t button, int player) {
    uintptr_t handler = 0;
    if (!StaminaStrict() || !actors::SlotIs(player, 0) || !MeleePromptButton(button) || DefensiveWeskerPrompt(button))
        return false;
    const uintptr_t rva = mem::Read(button + 0x3C, handler) ? handler - reinterpret_cast<uintptr_t>(mem::Base()) : 0;
    return rva != 0x50C520 && rva != 0x50C590;
}

void __fastcall HookOnPressed(void* button, void* edx, int player) {
    const bool strict = StrictMeleePrompt(reinterpret_cast<uintptr_t>(button), player);
    if (strict && !StaminaAllows()) {
        static DWORD logged = 0;
        if (GetTickCount() - logged > 2000) LOG("stamina: melee prompt refused (red arc filling)");
        logged = GetTickCount();
        return;
    }
    uint8_t active = 0;
    if (strict) mem::Read(reinterpret_cast<uintptr_t>(button) + 0xA, active);
    uintptr_t owner = 0;
    if (actors::SlotIs(player, 1) && (P2Blocked() & 1) &&
        mem::Read(reinterpret_cast<uintptr_t>(button) + 4, owner) && owner) {
        const auto enemies = actors::Enemies();
        // While an enemy holds player 1 its prompt is the partner's rescue (user, 2026-11-06: the Kipepeo's "Aide",
        // kind 0x11, refused here - Sheva could not free him): only "Le joueur 2 ne te libère pas des prises" refuses it.
        const uintptr_t p1 = actors::Player(0);
        uint32_t p1Action = 0;
        const bool rescue = p1 && ((mem::Read(p1 + 0x10E0, p1Action) && (p1Action == 0x84 || p1Action == 0x82)) ||
                                   PlayerHeldByEnemy(p1));
        if (std::find(enemies.begin(), enemies.end(), owner) != enemies.end() && !KneelPromptForPartner(owner) &&
            !DefensiveWeskerPrompt(reinterpret_cast<uintptr_t>(button)) && (!rescue || (P2Blocked() & 8))) {
            static DWORD logged = 0;
            uint32_t kind = 0;
            mem::Read(reinterpret_cast<uintptr_t>(button) + 0x1C, kind);
            if (GetTickCount() - logged > 2000)
                LOG("player 2: an enemy's prompt (kind %X, owner %08X) refused (%s)", kind, static_cast<unsigned>(owner),
                    rescue ? "no rescue" : "no attack");
            logged = GetTickCount();
            return;
        }
    }
    if (CoopSoloRedirect(reinterpret_cast<uintptr_t>(button), player)) return;
    if (RaftRedirect(reinterpret_cast<uintptr_t>(button), player)) return;
    g_onPressed(button, edx, player);
    if (strict && active) StaminaSpendNative();
    CoopSoloPressed(reinterpret_cast<uintptr_t>(button), player);
}

bool PressFor(uintptr_t button, int slot) {
    const OnPressedFn press = g_onPressed ? g_onPressed : reinterpret_cast<OnPressedFn>(mem::Rva(0x7C3360));
    __try {
        press(reinterpret_cast<void*>(button), nullptr, slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// The final Wesker held by the partner: his injection prompt pressed for player 1, once per prompt.
void TickSoloInjection() {
    static uintptr_t pressed = 0;
    if (!g_soloInjection.load(std::memory_order_relaxed) || !actors::InGame()) {
        pressed = 0;
        return;
    }
    const uintptr_t p1 = actors::Player(0), p2 = actors::Player(1);
    const int slot = actors::SlotOf(p1);
    if (!p2 || slot < 0) return;
    for (uintptr_t e : actors::Enemies()) {
        uint32_t action = 0, count = 0;
        uintptr_t holder = 0, table = 0, pinion = 0, button = 0;
        if (!IsFinalWesker(e) || !mem::Read(e + 0x10E0, action) || action != 0x6A || !mem::Read(e + 0x1F64, holder) ||
            holder != p2 || !mem::Read(e + 0x10D0, count) || count <= 0x6A || !mem::Read(e + 0x10DC, table) || !table ||
            !mem::Read(table + 4 * 0x6A, pinion) || !pinion || !mem::Read(pinion + 0xAC, button) || !button || button == pressed)
            continue;
        pressed = button;
        if (!PressFor(button, slot)) {
            LOG("solo injection: the injection prompt %08X could not be pressed", static_cast<unsigned>(button));
            return;
        }
        mem::Read(e + 0x10E0, action);
        LOG("solo injection: Wesker %08X held by the partner, injection prompt pressed for player 1 (his action now %X)",
            static_cast<unsigned>(e), action);
    }
}

// sActionButton's offer of a prompt to a player's manager (RVA 0x364690, thiscall(manager, button) ret 4; its one caller
// RVA 0x3677B1, every frame; manager +8 = the player's slot, its list +0x2C0 / count +0x2C8 the prompts shown): the
// melee prompts "Endurance stricte" refuses are not offered to player 1 (user, 2026-10-26: "refused but still shown").
using PromptOfferFn = void(__fastcall*)(void* manager, void* edx, void* button);
PromptOfferFn g_promptOffer = nullptr;

void __fastcall HookPromptOffer(void* manager, void* edx, void* button) {
    int slot = -1;
    if (button && mem::Read(reinterpret_cast<uintptr_t>(manager) + 8, slot) &&
        StrictMeleePrompt(reinterpret_cast<uintptr_t>(button), slot) && !StaminaAllows())
        return;
    g_promptOffer(manager, edx, button);
}

bool EnsurePromptOfferHook() {
    static bool hooked = false;
    if (hooked) return true;
    // push ebx; mov ebx, ecx; cmp byte ptr [ebx+4], 0; je ...
    const uint8_t expected[] = { 0x53, 0x8B, 0xD9, 0x80, 0x7B, 0x04, 0x00, 0x0F, 0x84 };
    uint8_t* target = mem::Rva(0x364690);
    if (!mem::IsGameCode(target, sizeof(expected)) || memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("prompt offer hook: unexpected bytes %s at %p", mem::Hex(target, sizeof(expected)).c_str(), target);
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookPromptOffer), reinterpret_cast<void**>(&g_promptOffer));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("prompt offer hook installed at %p", target);
    return true;
}

bool EnsureOnPressedHook() {
    static bool hooked = false;
    if (hooked) return true;
    // push esi; mov esi, ecx; cmp byte ptr [esi+0Ah], 0; je ...
    const uint8_t expected[] = { 0x56, 0x8B, 0xF1, 0x80, 0x7E, 0x0A, 0x00, 0x0F, 0x84 };
    uint8_t* target = mem::Rva(0x7C3360);
    if (!mem::IsGameCode(target, sizeof(expected)) || memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("prompt press hook: unexpected bytes %s at %p", mem::Hex(target, sizeof(expected)).c_str(), target);
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookOnPressed), reinterpret_cast<void**>(&g_onPressed));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("prompt press hook installed at %p", target);
    return true;
}

bool EnsureJillPressHook() {
    static bool hooked = false;
    if (hooked) return true;
    // mov eax, [0x012340A4]; mov edx, [eax+0x1042C]
    const uint8_t expected[] = { 0xA1, 0xA4, 0x40, 0x23, 0x01, 0x8B, 0x90, 0x2C, 0x04, 0x01, 0x00 };
    uint8_t* target = mem::Rva(0x5395F0);
    if (!mem::IsGameCode(target, sizeof(expected)) || memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("Jill press hook: unexpected bytes %s at %p", mem::Hex(target, sizeof(expected)).c_str(), target);
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookJillPress), reinterpret_cast<void**>(&g_jillPress));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("Jill press hook installed at %p", target);
    return true;
}

bool EnsureWeskerPromptHooks() {
    static bool hooked = false;
    if (hooked) return true;
    // push esi; mov esi, [esp+8]; mov eax, [esi]; mov edx, [eax+0x10]
    const uint8_t expected[] = { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x06, 0x8B, 0x50, 0x10 };
    const struct { uintptr_t rva; void* detour; PromptHandlerFn* original; } sites[] = {
        { 0x50C6C0, reinterpret_cast<void*>(&HookWeskerCombo), &g_weskerCombo },
        { 0x50C4C0, reinterpret_cast<void*>(&HookWeskerCounter), &g_weskerCounter },
        { 0x50C520, reinterpret_cast<void*>(&HookWeskerKneel), &g_weskerKneel },
    };
    for (const auto& site : sites) {
        uint8_t* target = mem::Rva(site.rva);
        if (!mem::IsGameCode(target, sizeof(expected)) || memcmp(target, expected, sizeof(expected)) != 0) {
            LOG("Wesker prompt handler: unexpected bytes %s at %p", mem::Hex(target, sizeof(expected)).c_str(), target);
            return false;
        }
        mem::EnsureExecutable(target, sizeof(expected));
        MH_STATUS st = MH_CreateHook(target, site.detour, reinterpret_cast<void**>(site.original));
        if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
        st = MH_EnableHook(target);
        if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    }
    hooked = true;
    LOG("Wesker prompt handler hooks installed");
    return true;
}

bool EnsureEnemyLogHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x759030);
    const uint8_t expected[] = { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x81, 0xFF, 0x00, 0x00, 0x00, 0x80 };
    if (!mem::IsGameCode(target, sizeof(expected)) || memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("enemy action log: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookRequestCore), reinterpret_cast<void**>(&g_requestCore));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("enemy action log hook installed at %p", target);
    return true;
}

bool EnsureHook() {
    if (g_hooked) return true;
    uint8_t* target = mem::Rva(0x75C900);
    const uint8_t expected[] = { 0x56, 0x8B, 0xF1, 0x80, 0xBE, 0x6C, 0x2D, 0x00, 0x00, 0x00 };
    if (!mem::IsGameCode(target, sizeof(expected)) || memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("action log: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS s = MH_CreateHook(target, &HookRequestAction, reinterpret_cast<void**>(&g_requestAction));
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) return false;
    s = MH_EnableHook(target);
    if (s != MH_OK && s != MH_ERROR_ENABLED) return false;
    g_hooked = true;
    LOG("action log hook installed at %p", target);
    return true;
}
}

bool EnsurePlayerRequestHook() { return EnsureHook(); }
bool EnsurePromptPressHook() { return EnsureOnPressedHook() && EnsurePromptOfferHook(); }
bool PressPromptFor(uintptr_t button, int slot) { return PressFor(button, slot); }
bool EnsureEnemyActionHook() { return EnsureEnemyLogHook(); }

// Player 2 options (player.cpp): category 0 attack, 1 pickup, 2 revive, 3 rescue from a grab.
bool BlockPlayer2Actions(int category, bool on) {
    if (on && !EnsureHook()) return false;
    if (on && category == 0 &&
        (!EnsureEnemyLogHook() || !EnsureWeskerPromptHooks() || !EnsureJillPressHook() || !EnsureOnPressedHook()))
        return false;  // the bosses' prompts
    if (on && category == 1 && !EnsurePickupHook()) return false;
    if (on && category == 2 && !EnsureReviveHooks()) return false;
    g_blockedFor = 0;
    if (on) g_p2Blocked |= 1u << category;
    else g_p2Blocked &= ~(1u << category);
    return true;
}

// "Journal des ennemis créés (recherche)" (user, 2026-10-23: a table to spawn any enemy): the enemy base constructor
// (RVA 0x5DB170, uEnemy: called first by each enemy class's constructor, 73 of them; the Reaper's at VA 0x8A49AE)
// logged with its call stack, the unit's class once its own constructor ran (next frame): the spawn path to call.
namespace {
using EnemyCtorFn = void*(__fastcall*)(void* self, void* edx);
EnemyCtorFn g_enemyCtor = nullptr;
std::atomic<bool> g_enemyCreateLog{ false };
std::mutex g_createdMutex;
std::vector<uintptr_t> g_created;  // units built, their class logged on the render thread

void* __fastcall HookEnemyCtor(void* self, void* edx) {
    void* result = g_enemyCtor(self, edx);
    if (g_enemyCreateLog.load(std::memory_order_relaxed)) {
        LOG("enemy created %08X | stack%s", static_cast<unsigned>(reinterpret_cast<uintptr_t>(self)),
            StackChain(static_cast<const uint32_t*>(_AddressOfReturnAddress()), 16).c_str());
        std::lock_guard<std::mutex> lock(g_createdMutex);
        g_created.push_back(reinterpret_cast<uintptr_t>(self));
    }
    return result;
}

void TickEnemyCreateLog() {
    std::vector<uintptr_t> created;
    {
        std::lock_guard<std::mutex> lock(g_createdMutex);
        created.swap(g_created);
    }
    for (uintptr_t unit : created) {
        const char* name = UnitClassName(unit);
        LOG("enemy created %08X is %s", static_cast<unsigned>(unit), name && !IsBadStringPtrA(name, 64) ? name : "?");
    }
}

// sEnemy::SetEnemy (RVA 0x3AF6E0, thiscall(a, b, flag) ret 0xC; [RVA 0xDB1FCC]): called by the stage scripts' "set
// enemy" order (RVA 0x82E070: a = param +8, b = param +0xC); adds 1 to the Mercenaries spawned count (+0x584), finds the
// layout entry (RVA 0x3F3D70(a, b)) and creates its unit. Logged with its arguments (the units built inside follow).
using SetEnemyFn = uintptr_t(__fastcall*)(void* self, void* edx, uint32_t a, uint32_t b, uint32_t flag);
SetEnemyFn g_setEnemy = nullptr;

uintptr_t __fastcall HookSetEnemy(void* self, void* edx, uint32_t a, uint32_t b, uint32_t flag) {
    const bool log = g_enemyCreateLog.load(std::memory_order_relaxed);
    if (log)
        LOG("SetEnemy(%X, %X, %u) from RVA %06X", a, b, flag,
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(mem::Base())));
    const uintptr_t result = g_setEnemy(self, edx, a, b, flag);
    if (log) LOG("SetEnemy(%X, %X) -> %08X", a, b, static_cast<unsigned>(result));
    return result;
}

bool EnsureEnemyCtorHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* set = mem::Rva(0x3AF6E0);
    const uint8_t setExpected[] = { 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x80, 0x7E, 0x1C, 0x00 };
    if (mem::IsGameCode(set, sizeof(setExpected)) && memcmp(set, setExpected, sizeof(setExpected)) == 0) {
        mem::EnsureExecutable(set, sizeof(setExpected));
        const MH_STATUS st = MH_CreateHook(set, reinterpret_cast<void*>(&HookSetEnemy), reinterpret_cast<void**>(&g_setEnemy));
        const MH_STATUS on = st == MH_OK || st == MH_ERROR_ALREADY_CREATED ? MH_EnableHook(set) : st;
        if (on == MH_OK || on == MH_ERROR_ENABLED) LOG("enemy create log: SetEnemy hook installed at %p", set);
    } else {
        LOG("enemy create log: SetEnemy unexpected bytes %s", mem::Hex(set, sizeof(setExpected)).c_str());
    }
    uint8_t* target = mem::Rva(0x5DB170);
    const uint8_t expected[] = { 0x53, 0x56, 0x8B, 0xF1, 0xE8 };
    if (!mem::IsGameCode(target, sizeof(expected)) || memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("enemy create log: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookEnemyCtor), reinterpret_cast<void**>(&g_enemyCtor));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("enemy create log: hook installed at %p", target);
    return true;
}

// "Journal des objets créés (recherche)" (user, 2026-10-31: a two-person vertical jump uOmf112 wanted in front of a
// raised container, none in that level): the stage objects' base constructor (RVA 0x683F80, uObjModel; the uOmf112 DTI's
// NewInstance VA 0xAA9E70 calls it) logged with its call stack (the first 24 of a load, then one in 50), the class once
// its own constructor ran: how the level creates, places and registers its objects.
using ObjectCtorFn = void*(__fastcall*)(void* self, void* edx);
ObjectCtorFn g_objectCtor = nullptr;
std::atomic<bool> g_objectCreateLog{ false };
std::vector<uintptr_t> g_objectsCreated;  // g_createdMutex
int g_objectsLogged = 0;

void* __fastcall HookObjectCtor(void* self, void* edx) {
    void* result = g_objectCtor(self, edx);
    if (g_objectCreateLog.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_createdMutex);
        const int n = g_objectsLogged++;
        if (n < 24 || n % 50 == 0)
            LOG("object created %08X (#%d) | stack%s", static_cast<unsigned>(reinterpret_cast<uintptr_t>(self)), n,
                StackChain(static_cast<const uint32_t*>(_AddressOfReturnAddress()), 20).c_str());
        if (g_objectsCreated.size() < 256) g_objectsCreated.push_back(reinterpret_cast<uintptr_t>(self));
    }
    return result;
}

void TickObjectCreateLog() {
    std::vector<uintptr_t> created;
    {
        std::lock_guard<std::mutex> lock(g_createdMutex);
        created.swap(g_objectsCreated);
    }
    for (uintptr_t unit : created) {
        const char* name = UnitClassName(unit);
        float pos[3]{};
        mem::SafeRead(reinterpret_cast<void*>(unit + 0x30), pos, sizeof(pos));
        LOG("object created %08X is %s at (%.0f %.0f %.0f)", static_cast<unsigned>(unit),
            name && !IsBadStringPtrA(name, 64) ? name : "?", pos[0], pos[1], pos[2]);
    }
}

bool EnsureObjectCtorHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x683F80);
    const uint8_t expected[] = { 0x56, 0x8B, 0xF1, 0xE8 };
    if (!mem::IsGameCode(target, sizeof(expected)) || memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("object create log: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookObjectCtor), reinterpret_cast<void**>(&g_objectCtor));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("object create log: hook installed at %p", target);
    return true;
}
}  // namespace

void RegisterActionLog() {
    Feature& solo = Add({ .id = "enemy.wesker_solo_injection", .name = "Injection de Wesker sans le partenaire",
                          .help = "Combat final : quand Wesker est à genou, appuyer sur son prompt fait que le partenaire "
                                  "le maîtrise et Chris lui fait l'injection tout de suite. Si le partenaire le maîtrise de "
                                  "lui-même, Chris l'injecte aussi. Marche avec « Le joueur 2 n'attaque pas ». Avec « Joueur 2 "
                                  "invisible », Wesker reste à genou pendant qu'il est tenu.",
                          .category = "Ennemis", .kind = Kind::Toggle,
                          .set = [](bool on) {
                              if (on && (!EnsureWeskerPromptHooks() || !EnsureOnPressedHook() || !EnsureSetMotionHook())) return false;
                              g_soloInjection = on;
                              return true;
                          },
                          .available = [] { return game::IsSupported(); }, .defaultOn = true });
    solo.tick = [] { TickSoloInjection(); };
    Feature& created = Add({ .id = "diag.enemy_create_log", .name = "Journal des ennemis créés (recherche)",
                             .help = "Écrit dans trainer.log chaque ennemi créé par le jeu (sa classe et le code qui le "
                                     "crée). Sert à trouver comment faire apparaître un ennemi.",
                             .category = "Diagnostic", .kind = Kind::Toggle,
                             .set = [](bool on) {
                                 if (on && !EnsureEnemyCtorHook()) return false;
                                 g_enemyCreateLog = on;
                                 return true;
                             },
                             .available = [] { return game::IsSupported(); } });
    created.tick = [] { TickEnemyCreateLog(); };
    Feature& objects = Add({ .id = "diag.object_create_log", .name = "Journal des objets créés (recherche)",
                             .help = "Écrit dans trainer.log chaque objet du décor créé par le jeu (sa classe, sa place et "
                                     "le code qui le crée). Sert à créer un objet qui n'existe pas dans le niveau.",
                             .category = "Diagnostic", .kind = Kind::Toggle,
                             .set = [](bool on) {
                                 if (on && !EnsureObjectCtorHook()) return false;
                                 g_objectsLogged = 0;
                                 g_objectCreateLog = on;
                                 return true;
                             },
                             .available = [] { return game::IsSupported(); } });
    objects.tick = [] { TickObjectCreateLog(); };
    Add({ .id = "diag.action_log", .name = "Journal des actions (recherche)",
          .help = "Enregistre chaque action déclenchée sur Chris/Sheva (numéro + endroit du code) dans actions.log.\n"
                  "Sert à trouver les mouvements à rendre échangeables (ex. coups de couteau de Sheva contre les lickers/parasites).",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureHook() || !EnsureEnemyLogHook())) return false;
              g_logging = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "diag.boss_motion_log", .name = "Journal des animations de Jill et Wesker (recherche)",
          .help = "Écrit dans trainer.log chaque animation que Jill ou Wesker lance : leur action, le numéro de "
                  "l'animation, la liste d'où elle vient et sa durée. Sert à reprendre leurs mouvements pour Chris.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureSetMotionHook()) return false;
              g_bossMotionLogging = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "diag.motion_log", .name = "Journal des animations (recherche)",
          .help = "Enregistre dans motions.log chaque animation lancée par les joueurs : personnage, action en cours et "
                  "numéro d'animation. Sert à faire correspondre les coups d'un personnage à un autre (échange "
                  "d'animations).",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureSetMotionHook()) return false;
              g_motionLogging = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
}
}
