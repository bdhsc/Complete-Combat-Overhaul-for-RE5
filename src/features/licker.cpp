// Licker pounce (user, 2026-10-15): when a licker (uEm32, vtable RVA 0xB79060) jumps on a player it goes through
// its actions 0x26 AtkMount -> 0x28 MountHold (the player is driven by it: P+0x10E0 = 0x80000000, motion bank F,
// F/2D) -> 0x29 MountFinish (~5 s later, the player dies) unless the partner hits it: then 0x2A MountMiss (the player
// throws it off, motion F/2F) or 0x31 DmgBlown. Enemies request their actions through RequestAction (vtable +0xDC,
// core RVA 0x759030, thiscall(action, flag, 0, -1, 0) like the AI dispatcher RVA 0x675370). Asking the licker for
// MountMiss one second into the hold freed Chris with his own animation (user-verified).
// The base game has no struggle prompt for it: "Se libérer seul d'un licker" adds one (mash the keys the game uses to
// struggle out of a Majini grab: Q / D on AZERTY = A / D on QWERTY, read by scan code); full gauge = MountMiss,
// otherwise the game goes on (death). Independent from Sheva's rescue (her own box).
//
// Native struggles (Majini grab, licker tongue...): the on-screen prompts are units of sActionButton ([RVA 0xDB1794],
// the struggle one at +0x270); the "mash" type is updated by RVA 0x366320, the "wiggle left/right" type by RVA
// 0x3664A0 (thiscall(unit, ?, 1, player) ret 0xC, returns 1 when won). Each left/right alternation (game buttons 2 / 3,
// sBioPad::Triggered RVA 0x39A3E0 = thiscall(button, pad)) adds 1.5 to the gauge unit +0x2C, capped by +0x30 = 40:
// about 27 alternations (2026-10-15, measured). "Échappements automatiques" fills the gauge before the check;
// "Débattement facile" multiplies what each alternation adds.
#include "features/dying.hpp"

#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "hooks/hooks.hpp"

#include <windows.h>
#include <MinHook.h>
#include <imgui.h>

#include <atomic>
#include <cstring>
#include <string>

namespace features {
bool EnsurePlayerDamageHook();  // player.cpp: SubHealth hooks (actors and players)
bool EnsureEnemyActionHook();   // action_log.cpp: the enemies' request core hook (RVA 0x759030)
const char* UnitClassName(uintptr_t object);  // parry.cpp: its DTI class name
bool StartReversedNeckBreak(uintptr_t majini);  // wesker_moves.cpp: a failed back grab's end
float WorldTimeSpeed();         // time_speed.cpp: sFps +0x2C, the world's time speed (the inventory's slow motion too)
bool PauseMenuIsShown();        // pickup.cpp: the pause menu on screen
bool EnsurePauseMenuWatch();    // pickup.cpp: its watch

namespace {

constexpr uintptr_t kLickerVtableRva = 0xB79060;
constexpr uint32_t kMountHold = 0x28;
constexpr uint32_t kMountMiss = 0x2A;
constexpr UINT kScanLeft = 0x1E;   // A on QWERTY, Q on AZERTY
constexpr UINT kScanRight = 0x20;  // D
constexpr DWORD kMinHoldMs = 600;  // the licker must be settled in its hold before it accepts MountMiss
// Lost in Nightmares' axe man (uEm27, the Executioner's cEm20 actions; user, 2026-10-26: "like the licker, alone you
// cannot get free of its grab"): its catch AtkCatch 0x17 takes player 1 into AtkExecCatch 0x30 (logged, then AtkExecKill
// 0x31 kills him) or lifts him into AtkHanging 0x18 (then AtkHangingTate 0x1A / Skewer 0x1B). A hit reaction of his
// frees him (logged: the RE6 parry's Dmg 0x1D during ExecCatch); the escape asks the reaction of "Réaction du bourreau
// à la libération" (user, 2026-10-26: kneeling looked odd): Dmg 0x1D (by default), DmgKneelDown 0x1C (a melee prompt for
// the players) or Back 0x02 (a step back). The same list replaces the Dmg 0x1D-0x1F the game asks for him or the
// Executioner (uEm20) during their catch (AtkExecCatch 0x30, AtkHanging 0x18): a release by the partner's hits.
// Its own option and number of presses (user, 2026-10-26: its execution is faster than the licker's finish, he killed
// before the 25 presses), a shorter settle time.
constexpr uintptr_t kHangerVtableRva = 0xB74E38;
constexpr uint32_t kHanging = 0x18, kHangingEscape = 0x19, kExecCatch = 0x30, kKneelDown = 0x1C;
// The spider parasite (uEm0b, vtable RVA 0xB6E318; user, 2026-10-27: "like the licker, a prompt to get free alone, its
// own option and number of presses"): its leap AtkCatch, then AtkCatchHit (player 1 in its event cEm0bActAtkCatchHit_PL)
// and AtkCatchLoop hold him; the escape asks AtkCatchEnd (its own end of the hold: it falls off), else DmgBlown. Its
// actions are found by class name in its table (+0x10D0 count, +0x10DC objects).
constexpr uintptr_t kSpiderVtableRva = 0xB6E318;
enum HolderKind { kLicker = 0, kAxeMan = 1, kSpider = 2, kHolderKinds };
constexpr DWORD kMinHoldMsOf[kHolderKinds] = { kMinHoldMs, 200, 300 };
constexpr const char* kHolderNames[kHolderKinds] = { "licker", "axe man", "spider parasite" };
constexpr uint32_t kReleaseReactions[] = { 0x1D, 0x1C, 0x02 };
std::atomic<int> g_releaseReaction{ 0 };

uint32_t ReleaseReaction() {
    const int r = g_releaseReaction.load(std::memory_order_relaxed);
    return kReleaseReactions[r >= 0 && r < static_cast<int>(std::size(kReleaseReactions)) ? r : 0];
}

// The class name of a unit's action `action` ("" if none).
const char* ActionNameOf(uintptr_t unit, uint32_t action) {
    uint32_t count = 0;
    uintptr_t table = 0, object = 0;
    if (!mem::Read(unit + 0x10D0, count) || action >= count || count > 0x200 || !mem::Read(unit + 0x10DC, table) || !table ||
        !mem::Read(table + 4 * action, object) || !object)
        return "";
    const char* name = UnitClassName(object);
    return name ? name : "";
}

// The index of a unit's action by class name, -1 if none.
int ActionIndexOf(uintptr_t unit, const char* name) {
    uint32_t count = 0;
    if (!mem::Read(unit + 0x10D0, count) || count > 0x200) return -1;
    for (uint32_t k = 0; k < count; ++k)
        if (std::strcmp(ActionNameOf(unit, k), name) == 0) return static_cast<int>(k);
    return -1;
}

// The spider parasite on player 1's head: within 80 cm of him across, 60-220 cm above his feet.
bool OnPlayer1Head(uintptr_t enemy, uintptr_t p1) {
    float e[3]{}, p[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), e, sizeof(e)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), p, sizeof(p)))
        return false;
    const float dx = e[0] - p[0], dz = e[2] - p[2], up = e[1] - p[1];
    return dx * dx + dz * dz < 80.0f * 80.0f && up > 60.0f && up < 220.0f;
}

// The enemy of that kind holding player 1 with no escape of the game's (a licker in MountHold, the axe man in
// AtkExecCatch / AtkHanging, the spider parasite in AtkCatchHit / AtkCatchLoop), and the action that frees him.
// The spider's hold (recorded 2026-10-27, scratchpad spiderrec.py): AtkCatch 0xB (leap) -> AtkCatchHit 0xC (player 1 in
// its event, his motion 0xFC8, 1.3 s) -> AtkCatchLoop 0xE (player 1 free again: plaga_stick_in 0x82, then he walks with
// it on his head) -> by itself ~5 s later AtkCatchEnd 0x10 (it falls off: motions 0x31, 0x1B, then Getup 0xA) while
// player 1 plays plaga_stick_out 0x83 (motion 0x063). So its hold is not an event of player 1's all along.
uintptr_t HolderOfPlayer1(int kind, uint32_t* escape = nullptr) {
    const uintptr_t p1 = actors::Player(0);
    uint32_t p1Action = 0;
    if (!p1 || !mem::Read(p1 + 0x10E0, p1Action) || (p1Action != 0x80000000 && kind != kSpider)) return 0;
    const uintptr_t vtable = reinterpret_cast<uintptr_t>(
        mem::Rva(kind == kLicker ? kLickerVtableRva : kind == kAxeMan ? kHangerVtableRva : kSpiderVtableRva));
    for (uintptr_t enemy : actors::Enemies()) {
        uintptr_t vt = 0;
        uint32_t action = 0;
        if (!mem::Read(enemy, vt) || vt != vtable || !mem::Read(enemy + 0x10E0, action)) continue;
        if (kind == kLicker && action == kMountHold) {
            if (escape) *escape = kMountMiss;
            return enemy;
        }
        if (kind == kAxeMan && (action == kHanging || action == kExecCatch)) {
            if (escape) *escape = ReleaseReaction();
            return enemy;
        }
        if (kind == kSpider) {
            const char* name = ActionNameOf(enemy, action);
            if (std::strcmp(name, "cEm0bActAtkCatchHit") != 0 && std::strcmp(name, "cEm0bActAtkCatchLoop") != 0) continue;
            if (!OnPlayer1Head(enemy, p1)) continue;  // on player 2
            int end = ActionIndexOf(enemy, "cEm0bActAtkCatchEnd");
            if (end < 0) end = ActionIndexOf(enemy, "cEm0bActDmgBlown");
            if (end < 0) continue;  // no way to free him: no prompt
            if (escape) *escape = static_cast<uint32_t>(end);
            return enemy;
        }
    }
    return 0;
}

bool RequestEnemyAction(uintptr_t enemy, uint32_t action) {
    using RequestFn = bool(__fastcall*)(void* self, void* edx, int action, int flag, int a, int b, int c);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(enemy, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return false;
    __try {
        return reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(enemy), nullptr, static_cast<int>(action), 1, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string KeyName(UINT scan) {
    wchar_t wide[32]{};
    if (!GetKeyNameTextW(static_cast<LONG>(scan << 16), wide, 32)) return "?";
    char utf8[64]{};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return utf8;
}

bool GameInFront() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// The trainer's grab gauges count the game's time (user, 2026-10-31: paused after a back grab, its bar emptied and
// Chris died in the pause; "the same with the inventory's time"): none in the pause menu or with the game in the
// background, the world's time speed otherwise (stopped or slowed by the inventory's slow motion, "Vitesse du temps").
float GameTimeFactor() {
    if (!GameInFront() || PauseMenuIsShown()) return 0.0f;
    return WorldTimeSpeed();
}

bool PadTriggered(int button, int pad) {
    using TriggeredFn = bool(__fastcall*)(void* bioPad, void* edx, int button, int pad);
    uintptr_t bioPad = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C40)), bioPad) || !bioPad) return false;
    __try {
        return reinterpret_cast<TriggeredFn>(mem::Rva(0x39A3E0))(reinterpret_cast<void*>(bioPad), nullptr, button, pad);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::atomic<bool> g_autoEscape{ false };
std::atomic<bool> g_easyStruggle{ false };
std::atomic<float> g_easyFactor{ 3.0f };
// "Prise de dos : se libérer en appuyant" (user, 2026-10-30: from behind "Chris se libère automatiquement, taper la
// touche ne fait qu'accélérer"): the struggle updates (mash RVA 0x366320, wiggle 0x3664A0) take a byte flag, their 2nd
// stack argument, that adds one step to the gauge every [sActionButton +0xDFC] seconds (their unit's timer +0x20): the
// game sets it for a Majini's back hold (cEm10ActBackHold 0x9B), not for its front grab. Cleared for player 1's back
// hold: only his presses fill the gauge, within the slider's seconds (a bar shows them), else the Majini breaks his neck
// (wesker_moves.cpp: Chris's own neck break reversed) and he dies.
// "Saisies mortelles en étant mourant" (user, 2026-10-30: "toutes les saisies alors qu'on est mourant tuent immédiatement
// le personnage avec le finisher", no struggle, no rescue by the partner). Player 1 dying (the grabs come with "Saisie
// en étant mourant"):
// - a Majini's front grab (cEm10ActNeckHang 0x99, its update RVA 0x8119A0: a state byte at the action +0x14; state 1
//   plays the grab-in motion 0x60D (female +0x3C) and bites (0x600) once it ends, its +0x388 bit 4 or frame 120): its
//   grab-in cut once settled (frame 30: its frame set to 120, and player 1's own grab-in in his event, bank F, to the
//   same frame - user: his arms still struggled, out of step), so the bite comes at once with its own blend; then
//   state 2, the choking bite 0x600 during which he can still struggle free (user: "impossible de se débattre"), ends
//   at its frame 90 into the final bite 0x60E (the prompt removed, he is thrown down): cut there at once too (the first
//   version sped the grabber and Chris up x8 until the bite, user: "ça accélère toute l'anim, très moche"); the bite
//   kills (dying.cpp Re6DyingHitKills: his dying time emptied). Other grabs with a struggle prompt (a dog...) bite at
//   their own time and kill the same way;
// - a Majini's back hold: its neck break at once (wesker_moves.cpp, as a missed "Prise de dos" escape);
// - a licker's pounce: its MountFinish 0x29 once settled in its hold;
// - the Executioner (uEm20) / Lost in Nightmares' axe man (uEm27): the lift AtkHanging 0x18 its AI asks after its catch
//   becomes its execution AtkExecCatch 0x30 (he is thrown down, then AtkExecKill 0x31), its other choice there.
std::atomic<bool> g_fatalGrabs{ false };
constexpr float kGrabInSettled = 30.0f, kGrabInEnd = 120.0f;  // NeckHang's grab-in frames
constexpr float kChokeEnd = 90.0f;                              // its choking bite's last frame (state 2)
constexpr uint32_t kMountFinish = 0x29;
std::atomic<bool> g_backPresses{ false };
std::atomic<float> g_backSeconds{ 4.0f };
std::atomic<uintptr_t> g_backHolder{ 0 };  // the Majini holding player 1 from behind (render thread writes)
std::atomic<float> g_backLeft{ 0.0f };      // 0..1 of the time left (the bar)
DWORD g_backLast = 0;                        // render thread: the last tick
float g_backElapsed = 0.0f;                  // render thread: the game's time held, ms (GameTimeFactor)
bool g_backFailed = false;

struct Struggle {
    uintptr_t licker = 0;
    DWORD since = 0;
    DWORD last = 0;
    float gauge = 0;
    bool freed = false;
    bool blocked = false;  // dying without "Se débattre en étant mourant": no struggle (as for the game's grabs)
    int stickSide = 0;     // the side the pad's left stick was last pushed to (-1 left, 1 right)
};
Struggle g_struggle[kHolderKinds];
std::atomic<bool> g_escapeOn[kHolderKinds]{};
Feature* g_escape[kHolderKinds] = {};

void TickEscape(int kind) {
    Struggle& g_struggle = features::g_struggle[kind];
    if (!game::IsSupported() || !actors::InGame()) {
        g_struggle = {};
        return;
    }
    uint32_t escape = kMountMiss;
    const uintptr_t licker = HolderOfPlayer1(kind, &escape);
    const DWORD now = GetTickCount();
    if (!licker) {
        g_struggle.licker = 0;
        return;
    }
    if (licker != g_struggle.licker) {
        g_struggle = {};
        g_struggle.licker = licker;
        g_struggle.since = g_struggle.last = now;
        uint32_t holding = 0;
        mem::Read(licker + 0x10E0, holding);
        LOG("%s %08X holds player 1 (its action %X %s, escape %X %s): struggle", kHolderNames[kind],
            static_cast<unsigned>(licker), holding, ActionNameOf(licker, holding), escape, ActionNameOf(licker, escape));
    }
    g_struggle.blocked = Player1Dying() && !DyingStruggleAllowed() && !GrabRescueActive();
    if (g_struggle.freed || g_struggle.blocked) {
        g_struggle.last = now;
        return;
    }
    const float dt = static_cast<float>(now - g_struggle.last) / 1000.0f * GameTimeFactor();
    g_struggle.last = now;
    g_struggle.gauge -= 0.12f * dt;  // slows down if the player stops mashing
    if (g_struggle.gauge < 0) g_struggle.gauge = 0;
    float needed = g_escape[kind] && g_escape[kind]->value >= 1 ? g_escape[kind]->value : 25.0f;
    if (g_easyStruggle.load(std::memory_order_relaxed)) needed /= g_easyFactor.load(std::memory_order_relaxed);
    // The game's own left / right buttons (keyboard and pad alike), as its wiggle prompts read them.
    uint8_t pad = 0;
    mem::Read(actors::Player(0) + 0x2D9C, pad);
    for (int button = 2; button <= 3; ++button)
        if (PadTriggered(button, pad) && GameInFront()) g_struggle.gauge += 1.0f / needed;
    // A pad's left stick: its directions never reach the button queries (user, 2026-10-26: the gauge did not move on a
    // pad). Each push to the other side counts as a press.
    float x = 0.0f, y = 0.0f;
    hooks::PadLeftStick(x, y);
    const int side = x < -0.5f ? -1 : x > 0.5f ? 1 : 0;
    if (side && side != g_struggle.stickSide && GameInFront()) g_struggle.gauge += 1.0f / needed;
    if (side) g_struggle.stickSide = side;
    if (g_autoEscape.load(std::memory_order_relaxed) || GrabRescueActive()) g_struggle.gauge = 1.0f;
    if (g_struggle.gauge >= 1.0f) {
        g_struggle.gauge = 1.0f;
        if (now - g_struggle.since < kMinHoldMsOf[kind]) return;
        g_struggle.freed = RequestEnemyAction(licker, escape);
        LOG("%08X: struggle won, its escape %X %s", static_cast<unsigned>(licker), escape,
            g_struggle.freed ? "accepted" : "refused");
    }
}

void DrawStruggle() {
    int kind = 0;
    while (kind < kHolderKinds && (!g_escapeOn[kind].load(std::memory_order_relaxed) || !g_struggle[kind].licker ||
                                   g_struggle[kind].freed || g_struggle[kind].blocked))
        ++kind;
    if (kind == kHolderKinds) return;
    const Struggle& g_struggle = features::g_struggle[kind];
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const std::string text = i18n::Tr("Débats-toi !   ") + KeyName(kScanLeft) + "  /  " + KeyName(kScanRight) + i18n::Tr("   (gauche / droite)");
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const float width = size.x > 260 ? size.x + 40 : 300;
    const ImVec2 box(screen.x * 0.5f - width * 0.5f, screen.y * 0.72f);
    dl->AddRectFilled(box, ImVec2(box.x + width, box.y + size.y + 46), IM_COL32(15, 10, 10, 210), 6);
    dl->AddText(ImVec2(box.x + (width - size.x) * 0.5f, box.y + 10), IM_COL32(255, 230, 200, 255), text.c_str());
    const ImVec2 bar(box.x + 20, box.y + size.y + 20);
    const float barWidth = width - 40;
    dl->AddRectFilled(bar, ImVec2(bar.x + barWidth, bar.y + 14), IM_COL32(60, 40, 35, 255), 3);
    dl->AddRectFilled(bar, ImVec2(bar.x + barWidth * g_struggle.gauge, bar.y + 14), IM_COL32(220, 70, 50, 255), 3);
}
using StruggleFn = int(__fastcall*)(void* unit, void* edx, void* a, int b, void* player);
StruggleFn g_mash = nullptr;
StruggleFn g_wiggle = nullptr;

int Struggle(StruggleFn original, void* unit, void* edx, void* a, int b, void* player) {
    const bool mine = player && reinterpret_cast<uintptr_t>(player) == actors::Player(0);
    if (mine && g_backPresses.load(std::memory_order_relaxed) && g_backHolder.load(std::memory_order_relaxed))
        b &= ~0xFF;  // the back hold: no filling with time
    const uintptr_t u = reinterpret_cast<uintptr_t>(unit);
    float max = 0, before = 0;
    if (mine && mem::Read(u + 0x30, max) && max > 0 && (g_autoEscape.load(std::memory_order_relaxed) || GrabRescueActive()))
        mem::Write(u + 0x2C, max);
    mem::Read(u + 0x2C, before);
    const int won = original(unit, edx, a, b, player);
    if (mine && !won && max > 0 && g_easyStruggle.load(std::memory_order_relaxed)) {
        float after = 0;
        if (mem::Read(u + 0x2C, after) && after > before) {
            after = before + (after - before) * g_easyFactor.load(std::memory_order_relaxed);
            mem::Write(u + 0x2C, after > max ? max : after);
        }
    }
    return won;
}

int __fastcall HookMash(void* unit, void* edx, void* a, int b, void* player) { return Struggle(g_mash, unit, edx, a, b, player); }

// A Majini holding player 1 from behind (its action's class cEm10ActBackHold, not ...BackHoldDmg), within 2.5 m.
uintptr_t BackHolder(uintptr_t p1) {
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), pos, sizeof(pos))) return 0;
    for (uintptr_t e : actors::Enemies()) {
        uint32_t action = 0, count = 0;
        uintptr_t table = 0, object = 0;
        float at[3]{};
        if (!mem::Read(e + 0x10E0, action) || action != 0x9B || !mem::Read(e + 0x10D0, count) || action >= count ||
            count > 0x200 || !mem::Read(e + 0x10DC, table) || !table || !mem::Read(table + 4 * action, object) || !object)
            continue;
        const char* cls = UnitClassName(object);
        if (!cls || std::strcmp(cls, "cEm10ActBackHold") != 0 ||
            !mem::SafeRead(reinterpret_cast<void*>(e + 0x30), at, sizeof(at)) ||
            (at[0] - pos[0]) * (at[0] - pos[0]) + (at[2] - pos[2]) * (at[2] - pos[2]) > 250.0f * 250.0f)
            continue;
        return e;
    }
    return 0;
}

void TickBackHold() {
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    const uintptr_t holder = p1 && mem::Read(p1 + 0x10E0, action) && action == 0x80000000 ? BackHolder(p1) : 0;
    const DWORD now = GetTickCount();
    if (!holder) {
        g_backHolder = 0;
        g_backFailed = false;
        return;
    }
    if (holder != g_backHolder.load()) {
        g_backHolder = holder;
        g_backLast = now;
        g_backElapsed = 0.0f;
        g_backFailed = false;
        LOG("back grab: Majini %08X holds player 1 from behind, %.1f s to struggle free", static_cast<unsigned>(holder),
            g_backSeconds.load());
    }
    if (g_backFailed) return;
    const float window = std::max(0.5f, g_backSeconds.load(std::memory_order_relaxed)) * 1000.0f;
    g_backElapsed += static_cast<float>(now - g_backLast) * GameTimeFactor();
    g_backLast = now;
    const float elapsed = g_backElapsed;
    g_backLeft = std::max(0.0f, 1.0f - elapsed / window);
    if (elapsed < window) return;
    g_backFailed = true;
    LOG("back grab: not freed in time");
    if (!StartReversedNeckBreak(holder)) LOG("back grab: the neck break could not start");
}

// The time left to struggle free, under the game's prompt.
void DrawBackHold() {
    if (!g_backPresses.load(std::memory_order_relaxed) || !g_backHolder.load(std::memory_order_relaxed) || g_backFailed)
        return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float width = 260.0f;
    const ImVec2 bar(screen.x * 0.5f - width * 0.5f, screen.y * 0.80f);
    dl->AddRectFilled(ImVec2(bar.x - 4, bar.y - 4), ImVec2(bar.x + width + 4, bar.y + 12), IM_COL32(15, 10, 10, 200), 4);
    dl->AddRectFilled(bar, ImVec2(bar.x + width * g_backLeft.load(std::memory_order_relaxed), bar.y + 8),
                      IM_COL32(220, 70, 50, 255), 3);
}
int __fastcall HookWiggle(void* unit, void* edx, void* a, int b, void* player) { return Struggle(g_wiggle, unit, edx, a, b, player); }

bool HookStruggle(uintptr_t rva, const uint8_t* expected, size_t n, void* detour, void** original, const char* what) {
    uint8_t* target = mem::Rva(rva);
    if (!mem::IsGameCode(target, n) || std::memcmp(target, expected, n) != 0) {
        LOG("%s: unexpected bytes %s", what, mem::Hex(target, n).c_str());
        return false;
    }
    mem::EnsureExecutable(target, n);
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    LOG("%s hook installed at %p", what, target);
    return true;
}

// Licker pounce drain (user, 2026-10-15): native grabs drain health (a Majini bite: 3 HP per frame through the player
// SubHealth, ~180 HP/s at 60 FPS, measured). The licker's MountHold update (action vtable slot 7, RVA 0x492B00,
// thiscall, held player at +0x20) only runs while the game runs: the drain is applied there, per second of game time
// ([RVA 0xDB209C] +0x24 = seconds of the frame), through the player SubHealth (RVA 0x77C950, so infinite health and
// invincibility still protect), never below 1 HP (the licker's finish kills).
using MountHoldFn = void(__fastcall*)(void* action, void* edx);
MountHoldFn g_mountHold = nullptr;
std::atomic<bool> g_drainOn{ false };
std::atomic<float> g_drainRate{ 200.0f };
float g_drainDebt = 0;

void __fastcall HookMountHold(void* action, void* edx) {
    g_mountHold(action, edx);
    if (!g_drainOn.load(std::memory_order_relaxed)) return;
    uintptr_t held = 0, clock = 0;
    float dt = 0;
    const uintptr_t p1 = actors::Player(0);
    if (!p1 || !mem::Read(reinterpret_cast<uintptr_t>(action) + 0x20, held) || held != p1) return;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB209C)), clock) || !clock || !mem::Read(clock + 0x24, dt)) return;
    if (dt <= 0 || dt > 0.25f) return;
    g_drainDebt += g_drainRate.load(std::memory_order_relaxed) * dt;
    if (g_drainDebt < 1.0f) return;
    int16_t health = 0;
    if (!mem::Read(p1 + 0x1364, health)) return;
    int amount = static_cast<int>(g_drainDebt);
    g_drainDebt -= static_cast<float>(amount);
    if (amount > health - 1) amount = health - 1;
    if (amount <= 0) return;
    using SubHealthFn = void(__fastcall*)(void* self, void* edx, int16_t amount);
    reinterpret_cast<SubHealthFn>(mem::Rva(0x77C950))(reinterpret_cast<void*>(p1), nullptr, static_cast<int16_t>(amount));
}

bool EnsureDrainHook() {
    static bool hooked = false;
    if (!hooked) {
        const uint8_t prologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x44, 0x53, 0x56 };
        hooked = HookStruggle(0x492B00, prologue, sizeof(prologue), reinterpret_cast<void*>(&HookMountHold),
                              reinterpret_cast<void**>(&g_mountHold), "licker mount hold");
    }
    return hooked;
}

bool EnsureStruggleHooks() {
    static bool hooked = false;
    if (!hooked) {
        const uint8_t mash[] = { 0x8B, 0x44, 0x24, 0x04, 0x53, 0x8B, 0x5C, 0x24, 0x10 };
        const uint8_t wiggle[] = { 0x51, 0x8B, 0x44, 0x24, 0x08, 0x53, 0x55, 0x56 };
        hooked = HookStruggle(0x366320, mash, sizeof(mash), reinterpret_cast<void*>(&HookMash),
                              reinterpret_cast<void**>(&g_mash), "struggle mash") &&
                 HookStruggle(0x3664A0, wiggle, sizeof(wiggle), reinterpret_cast<void*>(&HookWiggle),
                              reinterpret_cast<void**>(&g_wiggle), "struggle wiggle");
    }
    return hooked;
}
// Grace at the start of a grab (user, 2026-10-15): a dog holds the player 1.35 s before its first bite (5 grabs
// recorded), a Majini longer, the licker tongue drains at once; fast struggling during that window escapes without
// damage. While player 1 is held (P+0x10E0 0x80000000 / 0x84) with a struggle prompt shown (sActionButton manager 0:
// singleton + 0x40, slots +0x230 / +0x278: +4 shown, +0xC cActionButton, type +0x18 4 = mash, 6 = left/right), the
// game's own window is measured by its first damage (time since the hold began) and the player SubHealth refuses
// damage until that window times the slider (user: a multiplier, each enemy has its own window). The licker pounce
// has no game prompt: not concerned (its finish must still kill).
// Below x1 (harder, user): the grabbing enemy (owner +4 of the struggle cActionButton) runs faster until its first
// damage: unit time scale +0xFD4 (float, 1.0 set once by the uWork constructor RVA 0x7C2510 and only read afterwards,
// as a multiplier of the frame time, ~800 reads) = 1 / slider, its original value written back at the first damage, at
// the end of the hold or when the option is switched off. The enemy alone gave 1.02 s instead of 1.34 s at x0.5 (dog,
// 2026-10-15): the held player's animation counts too, so player 1 is sped up the same way.
std::atomic<bool> g_graceOn{ false };
std::atomic<float> g_graceFactor{ 2.0f };
std::atomic<DWORD> g_heldSince{ 0 };
std::atomic<DWORD> g_gameWindow{ 0 };  // ms, measured at the first damage of the grab (0 = not yet)
std::atomic<bool> g_strugglePrompt{ false };
uintptr_t g_spedEnemy = 0;  // render thread
float g_spedOriginal = 1.0f;
uintptr_t g_spedPlayer = 0;
float g_spedPlayerOriginal = 1.0f;

// Owner (+4) of the struggle prompt shown for player 1, 0 if none.
uintptr_t StrugglePromptOwner() {
    uintptr_t manager = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), manager) || !manager) return 0;
    for (uintptr_t slot : { manager + 0x40 + actors::Player1PromptOffset() + 0x230, manager + 0x40 + actors::Player1PromptOffset() + 0x278 }) {
        uint8_t shown = 0;
        uintptr_t button = 0, owner = 0;
        uint32_t type = 0;
        if (mem::Read(slot + 4, shown) && shown && mem::Read(slot + 0xC, button) && button && mem::Read(button + 0x18, type) &&
            (type == 4 || type == 6) && mem::Read(button + 4, owner))
            return owner ? owner : 1;
    }
    return 0;
}

void RestoreEnemySpeed() {
    if (g_spedPlayer) {
        if (g_spedPlayer == actors::Player(0)) mem::Write(g_spedPlayer + 0xFD4, g_spedPlayerOriginal);
        g_spedPlayer = 0;
    }
    if (!g_spedEnemy) return;
    for (uintptr_t enemy : actors::Enemies())
        if (enemy == g_spedEnemy) {
            mem::Write(enemy + 0xFD4, g_spedOriginal);
            break;
        }
    LOG("grab: enemy %08X back to speed %.2f", static_cast<unsigned>(g_spedEnemy), g_spedOriginal);
    g_spedEnemy = 0;
}

void SpeedUpGrabber(uintptr_t owner) {
    const float factor = g_graceFactor.load(std::memory_order_relaxed);
    if (g_spedEnemy || factor >= 1.0f || factor <= 0.05f || owner <= 1) return;
    for (uintptr_t enemy : actors::Enemies()) {
        if (enemy != owner && enemy != owner + 0x20) continue;
        float speed = 0;
        if (!mem::Read(enemy + 0xFD4, speed) || speed < 0.5f || speed > 2.0f) return;  // not the plain 1.0 expected
        g_spedEnemy = enemy;
        g_spedOriginal = speed;
        mem::Write(enemy + 0xFD4, speed / factor);
        const uintptr_t p1 = actors::Player(0);
        float own = 0;
        if (p1 && mem::Read(p1 + 0xFD4, own) && own >= 0.5f && own <= 2.0f) {
            g_spedPlayer = p1;
            g_spedPlayerOriginal = own;
            mem::Write(p1 + 0xFD4, own / factor);
        }
        LOG("grab: enemy %08X sped up x%.2f until its first damage", static_cast<unsigned>(enemy), 1.0f / factor);
        return;
    }
}

void TickGrabGrace() {
    const uintptr_t p1 = actors::Player(0);
    uint32_t action = 0;
    const bool held = actors::InGame() && p1 && mem::Read(p1 + 0x10E0, action) && (action == 0x80000000 || action == 0x84);
    if (!held) {
        g_heldSince = 0;
        g_gameWindow = 0;
        g_strugglePrompt = false;
        RestoreEnemySpeed();
        return;
    }
    if (!g_heldSince.load()) g_heldSince = GetTickCount() | 1;
    const uintptr_t owner = StrugglePromptOwner();
    g_strugglePrompt = owner != 0;
    if (g_gameWindow.load()) RestoreEnemySpeed();  // its first damage came
    else if (owner) SpeedUpGrabber(owner);
}
}  // namespace

bool EnsureGrabStruggleHooks() { return EnsureStruggleHooks(); }

// action_log.cpp's enemy request hook: the Executioner or the axe man holding a player (AtkExecCatch 0x30, AtkHanging
// 0x18) asked by the game for a hit reaction Dmg 0x1D-0x1F (the release) gets the list's reaction instead. Not the
// trainer's own requests (a parry's reaction of its own list). The main game's Executioner (uEm20) lifts player 1 in
// AtkHanging 0x18 and a won struggle asks AtkHangingEscape 0x19 (its prompt's press, caller RVA 0x7C33EC; actions.log,
// user 2026-10-26: "even kneeling gives his hit reaction"): replaced too. If player 1 is still held a second later the
// game's escape is asked again and let through.
std::atomic<uintptr_t> g_releasedEnemy{ 0 };
std::atomic<DWORD> g_releasedTick{ 0 };
std::atomic<bool> g_letEscape{ false };

// action_log.cpp's enemy request hook, "Saisies mortelles en étant mourant": the Executioner's (or the axe man's) lift
// AtkHanging 0x18 asked while player 1 is dying (within 3 m of him) becomes its execution AtkExecCatch 0x30, with player
// 1 as the action's other unit (+0xC, the request's 3rd argument): the AI asks its actions with none, and the first
// version (the action alone) never took him - its ExecKill 0x31 came, game over without its animation (user,
// 2026-10-30). The main game's Executioner never chose ExecCatch itself in the logs (always the lift), the axe man did.
// Madness while player 1 is dying (user, 2026-10-30: the Executioner never tries to catch a dying player - logged: only
// Walk, Wait and AtkTate, 13 strikes in a row - "mets-le en état de madness, et le Majini tronçonneuse aussi", as the
// Ultimate Trainer's "Madness Butcher / Chainsaw"): within 4 m of him, the Executioner's attacks become its chained
// horizontal swings (cEm20ActAtkComboExtStart, which goes on into its Loop / End), the chainsaw Majini's (uEm22, vtable
// RVA 0xB75B20) its flailing (cEm22ActAtkRanbu, its rage after its second getup); the hit kills a dying player.
// The Ultimate Trainer's own "Madness": captured 2026-10-30 (UTSpy, its hooks now enabled at once), "Mad Butcher" writes
// 0x12 = cEm20ActAtkComboExtStart into the Executioner's current action (+0x10E0) once, 4.5 s after it is ticked (no
// chainsaw Majini on that map: "Mad Chainsaw" wrote nothing). "Bourreau en folie" / "Majini tronçonneuse en folie" do it
// the clean way, always: every attack asked becomes the madness one.
std::atomic<bool> g_madButcher{ false }, g_madChainsaw{ false };
// The madness from further (user, 2026-10-30: the chainsaw Majini "doit se coller d'abord", then "pour mad boucher
// pareil"): with "Majini tronçonneuse en folie" / "Bourreau en folie" a walk or run within the option's slider reach of
// player 1 becomes the madness too (the Executioner's -> its chained swings); "Saisies mortelles en étant mourant" uses
// the same reaches near a dying player 1.
std::atomic<float> g_madChainsawReach{ 600.0f }, g_madButcherReach{ 600.0f };  // cm

bool MadnessRequest(uintptr_t enemy, int* action, bool executioner, bool approach = false) {
    const char* asked = ActionNameOf(enemy, static_cast<uint32_t>(*action));
    const char* mad = nullptr;
    if (!executioner && approach && (std::strcmp(asked, "cEm22ActWalk") == 0 || std::strcmp(asked, "cEm22ActRun") == 0)) {
        mad = "cEm22ActAtkRanbu";
    } else if (executioner && approach &&
               (std::strcmp(asked, "cEm20ActWalk") == 0 || std::strcmp(asked, "cEm20ActRun") == 0)) {
        mad = "cEm20ActAtkComboExtStart";
    } else if (executioner) {
        if (std::strncmp(asked, "cEm20ActAtk", 11) != 0 || std::strstr(asked, "ComboExt") || std::strstr(asked, "Catch") ||
            std::strstr(asked, "Hanging") || std::strstr(asked, "Exec"))
            return false;
        mad = "cEm20ActAtkComboExtStart";
    } else {
        if (std::strncmp(asked, "cEm22ActAtk", 11) != 0 || std::strstr(asked, "Ranbu") || std::strstr(asked, "_PL") ||
            std::strstr(asked, "Cutting") || std::strstr(asked, "CutsDown"))
            return false;
        mad = "cEm22ActAtkRanbu";
    }
    const int index = ActionIndexOf(enemy, mad);
    if (index < 0) return false;
    LOG("madness: %08X's %X (%s) -> %X (%s)", static_cast<unsigned>(enemy), *action, asked, index, mad);
    *action = index;
    return true;
}

void FatalExecution(uintptr_t enemy, int* action, int* other) {
    const bool butcherMad = g_madButcher.load(std::memory_order_relaxed), chainsawMad = g_madChainsaw.load(std::memory_order_relaxed);
    const bool fatal = g_fatalGrabs.load(std::memory_order_relaxed);
    if (!butcherMad && !chainsawMad && !fatal) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    const uintptr_t p1 = actors::Player(0);
    uintptr_t vtable = 0;
    float e[3]{}, p[3]{};
    if (!p1 || !mem::Read(enemy, vtable)) return;
    const bool executioner = vtable == base + 0xB748A8 || vtable == base + kHangerVtableRva;
    const bool chainsaw = vtable == base + 0xB75B20;
    if ((executioner && butcherMad) || (chainsaw && chainsawMad)) {  // the madness options: always
        bool inReach = false;
        if (mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), e, sizeof(e)) &&
            mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), p, sizeof(p))) {
            const float reach = (executioner ? g_madButcherReach : g_madChainsawReach).load(std::memory_order_relaxed);
            inReach = (e[0] - p[0]) * (e[0] - p[0]) + (e[2] - p[2]) * (e[2] - p[2]) < reach * reach;
        }
        MadnessRequest(enemy, action, executioner, inReach);
        return;
    }
    if (!fatal || !Player1Dying()) return;
    if ((!executioner && !chainsaw) || !mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), e, sizeof(e)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), p, sizeof(p)))
        return;
    const float distance2 = (e[0] - p[0]) * (e[0] - p[0]) + (e[2] - p[2]) * (e[2] - p[2]);
    if (executioner && *action == static_cast<int>(kHanging) && distance2 < 300.0f * 300.0f) {
        LOG("fatal grab: %08X's lift %X while player 1 is dying -> its execution %X on him (other was %08X)",
            static_cast<unsigned>(enemy), *action, kExecCatch, static_cast<unsigned>(*other));
        *action = static_cast<int>(kExecCatch);
        *other = static_cast<int>(p1);
        return;
    }
    const float reach = (executioner ? g_madButcherReach : g_madChainsawReach).load(std::memory_order_relaxed);
    MadnessRequest(enemy, action, executioner, distance2 < reach * reach);
}

void ExecutionerReleaseReaction(uintptr_t enemy, int* action, uintptr_t caller) {
    const int r = g_releaseReaction.load(std::memory_order_relaxed);
    const bool escape = *action == static_cast<int>(kHangingEscape);
    if (r <= 0 || ((*action < 0x1D || *action > 0x1F) && !escape)) return;
    if (escape && g_letEscape.exchange(false)) return;  // the safety's own request
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    if (caller < base || caller >= base + 0x1000000) return;
    uintptr_t vtable = 0;
    uint32_t current = 0;
    if (!mem::Read(enemy, vtable) || (vtable != base + 0xB748A8 && vtable != base + kHangerVtableRva) ||
        !mem::Read(enemy + 0x10E0, current) || (current != kHanging && current != kExecCatch) ||
        (escape && current != kHanging))
        return;
    LOG("release: %08X's %s %X in its catch %X -> %X", static_cast<unsigned>(enemy), escape ? "escape" : "Dmg", *action,
        current, ReleaseReaction());
    *action = static_cast<int>(ReleaseReaction());
    if (current == kHanging) {  // the safety: from the hanging only (its escape is the game's way out there)
        g_releasedEnemy = enemy;
        g_releasedTick = GetTickCount() | 1;
    }
}

namespace {
// The safety: player 1 still held a second after a replaced release gets the game's escape.
void TickReleaseSafety() {
    const DWORD tick = g_releasedTick.load();
    if (!tick || GetTickCount() - tick < 1000) return;
    const uintptr_t enemy = g_releasedEnemy.exchange(0);
    g_releasedTick = 0;
    const uintptr_t p1 = actors::Player(0);
    uint32_t action = 0;
    if (!enemy || !p1 || !mem::Read(p1 + 0x10E0, action) || (action != 0x80000000 && action != 0x84)) return;
    g_letEscape = true;
    LOG("release: player 1 still held a second after, the game's escape asked (%s)",
        RequestEnemyAction(enemy, kHangingEscape) ? "accepted" : "refused");
    g_letEscape = false;
}
}  // namespace

bool FreePlayer1FromLicker() {
    for (int kind = 0; kind < kHolderKinds; ++kind) {
        uint32_t escape = kMountMiss;
        if (const uintptr_t holder = HolderOfPlayer1(kind, &escape)) return RequestEnemyAction(holder, escape);
    }
    return false;
}

bool GrabGraceRefusesDamage() {
    const bool fatal = g_fatalGrabs.load(std::memory_order_relaxed) && Player1Dying();
    if ((!g_graceOn.load(std::memory_order_relaxed) && !fatal) || !g_strugglePrompt.load(std::memory_order_relaxed))
        return false;
    const DWORD since = g_heldSince.load(std::memory_order_relaxed);
    if (!since) return false;
    const DWORD elapsed = static_cast<int32_t>(GetTickCount() - since) > 0 ? GetTickCount() - since : 0;
    DWORD window = g_gameWindow.load(std::memory_order_relaxed);
    if (!window) {  // first damage of this grab: the game's window ends now
        window = elapsed ? elapsed : 1;
        g_gameWindow = window;
        const float factor = g_graceFactor.load(std::memory_order_relaxed);
        if (factor > 1.0f) LOG("grab: game window %.2f s, without damage until %.2f s", window / 1000.0f, window * factor / 1000.0f);
        else LOG("grab: first damage after %.2f s", window / 1000.0f);
    }
    const float factor = g_graceFactor.load(std::memory_order_relaxed);
    return !fatal && g_graceOn.load(std::memory_order_relaxed) && factor > 1.0f &&
           static_cast<float>(elapsed) < static_cast<float>(window) * factor;
}

bool FatalGrabHolds() {
    if (!g_fatalGrabs.load(std::memory_order_relaxed)) return false;
    const uintptr_t p1 = actors::Player(0);
    uint32_t action = 0;
    return p1 && mem::Read(p1 + 0x10E0, action) && (action == 0x80000000 || action == 0x84);
}

namespace {
uintptr_t g_fatalBack = 0;    // render thread: the back holder already sent to its neck break
uintptr_t g_fatalLicker = 0;  // the licker already asked its finish
uintptr_t g_fatalFront = 0;   // the Majini whose front grab is cut
int g_fatalFrontCut = 0;      // its last state cut (1 grab-in, 2 choking bite)

// A held unit's layer 0 motion set to frame `to` (its end at most); the other side of the hold too when `also`.
void CutMotion(uintptr_t unit, float to) {
    float frame = 0, length = 0;
    if (!mem::Read(unit + 0x398, frame) || !mem::Read(unit + 0x39C, length) || length <= 1.0f || frame >= to) return;
    mem::Write(unit + 0x398, std::min(to, length - 0.5f));
}

// A Majini holding player 1 by the front (cEm10ActNeckHang) within 2.5 m, its action object in `object`.
uintptr_t FrontHolder(uintptr_t p1, uintptr_t& object) {
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), pos, sizeof(pos))) return 0;
    for (uintptr_t e : actors::Enemies()) {
        uint32_t action = 0, count = 0;
        uintptr_t table = 0;
        float at[3]{};
        object = 0;
        if (!mem::Read(e + 0x10E0, action) || action != 0x99 || !mem::Read(e + 0x10D0, count) || action >= count ||
            count > 0x200 || !mem::Read(e + 0x10DC, table) || !table || !mem::Read(table + 4 * action, object) || !object)
            continue;
        const char* cls = UnitClassName(object);
        if (!cls || std::strcmp(cls, "cEm10ActNeckHang") != 0 ||
            !mem::SafeRead(reinterpret_cast<void*>(e + 0x30), at, sizeof(at)) ||
            (at[0] - pos[0]) * (at[0] - pos[0]) + (at[2] - pos[2]) * (at[2] - pos[2]) > 250.0f * 250.0f)
            continue;
        return e;
    }
    return 0;
}

void TickFatalGrabs() {
    if (!g_graceOn.load(std::memory_order_relaxed)) TickGrabGrace();  // the speed-up to the first bite
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p1 || !Player1Dying() || !mem::Read(p1 + 0x10E0, action) || action != 0x80000000) {
        g_fatalBack = g_fatalLicker = g_fatalFront = 0;
        g_fatalFrontCut = 0;
        return;
    }
    uintptr_t object = 0;
    if (const uintptr_t front = FrontHolder(p1, object)) {
        if (front != g_fatalFront) {
            g_fatalFront = front;
            g_fatalFrontCut = 0;
        }
        uint8_t state = 0;
        float frame = 0;
        uint32_t motion = 0;
        mem::Read(p1 + 0x384, motion);
        const bool his = ((motion >> 8) & 0xFF) == 0xF;  // his side in his event (bank F, the Majini's list lent)
        if (mem::Read(object + 0x14, state) && mem::Read(front + 0x398, frame) && state > g_fatalFrontCut &&
            ((state == 1 && frame >= kGrabInSettled) || state == 2)) {
            const float to = (state == 1 ? kGrabInEnd : kChokeEnd) + 0.5f;
            g_fatalFrontCut = state;
            CutMotion(front, to);
            if (his) CutMotion(p1, to);
            LOG("fatal grab: front grab while dying, %08X's %s cut at frame %.0f (player 1's %X%s)", static_cast<unsigned>(front),
                state == 1 ? "grab-in" : "choking bite", frame, motion & 0xFFFF, his ? "" : ", not his side of it");
        }
    }
    if (const uintptr_t holder = BackHolder(p1); holder && holder != g_fatalBack) {
        g_fatalBack = holder;
        LOG("fatal grab: back hold while dying, neck break %s", StartReversedNeckBreak(holder) ? "started" : "refused");
    }
    const DWORD since = g_heldSince.load();
    if (const uintptr_t licker = HolderOfPlayer1(kLicker); licker && licker != g_fatalLicker && since &&
                                                            GetTickCount() - since >= kMinHoldMs) {
        g_fatalLicker = licker;
        LOG("fatal grab: licker pounce while dying, its finish %s",
            RequestEnemyAction(licker, kMountFinish) ? "asked" : "refused");
    }
}
}  // namespace

void RegisterLicker() {
    Feature& escape = Add({ .id = "combat.licker_escape", .name = "Se libérer seul d'un licker",
                            .help = "Quand un licker te saute dessus (normalement seul le partenaire peut t'en sortir), "
                                    "un prompt apparaît : martèle les touches indiquées (Q / D en AZERTY, A / D en QWERTY) "
                                    "pour remplir la jauge et repousser le licker. Raté : il t'achève comme d'habitude. "
                                    "Le curseur règle le nombre d'appuis. Indépendant de l'aide de Sheva.",
                            .category = "Combat", .kind = Kind::Toggle,
                            .available = [] { return game::IsSupported(); },
                            .value = 25, .valueMin = 3, .valueMax = 40, .valueFormat = "%.0f appuis" });
    g_escape[kLicker] = &escape;
    escape.set = [](bool on) {
        g_escapeOn[kLicker] = on;
        if (!on) g_struggle[kLicker] = {};
        return true;
    };
    escape.tick = [] { TickEscape(kLicker); };
    Feature& axeMan = Add({ .id = "combat.axeman_escape", .name = "Se libérer seul du monstre à la hache (Lost in Nightmares)",
                            .help = "Quand le monstre à la hache de Lost in Nightmares t'attrape (normalement seul le "
                                    "partenaire peut t'en sortir), un prompt apparaît : martèle les touches indiquées (Q / D "
                                    "en AZERTY, A / D en QWERTY) pour remplir la jauge ; il s'agenouille et te lâche. Raté : "
                                    "il t'exécute comme d'habitude, et plus vite qu'un licker. Le curseur règle le nombre "
                                    "d'appuis.",
                            .category = "Combat", .kind = Kind::Toggle,
                            .available = [] { return game::IsSupported(); },
                            .value = 10, .valueMin = 3, .valueMax = 40, .valueFormat = "%.0f appuis" });
    g_escape[kAxeMan] = &axeMan;
    axeMan.set = [](bool on) {
        g_escapeOn[kAxeMan] = on;
        if (!on) g_struggle[kAxeMan] = {};
        return true;
    };
    axeMan.tick = [] { TickEscape(kAxeMan); };
    Feature& spider = Add({ .id = "combat.spider_escape", .name = "Se libérer seul d'un parasite araignée",
                            .help = "Quand un parasite araignée te saute dessus et te tient (normalement seul le partenaire "
                                    "peut t'en sortir), un prompt apparaît : martèle les touches indiquées (Q / D en AZERTY, "
                                    "A / D en QWERTY, ou le stick gauche) pour remplir la jauge et l'arracher. Raté : la "
                                    "prise continue comme d'habitude. Le curseur règle le nombre d'appuis.",
                            .category = "Combat", .kind = Kind::Toggle,
                            .available = [] { return game::IsSupported(); },
                            .value = 10, .valueMin = 3, .valueMax = 40, .valueFormat = "%.0f appuis" });
    g_escape[kSpider] = &spider;
    spider.set = [](bool on) {
        g_escapeOn[kSpider] = on;
        if (!on) g_struggle[kSpider] = {};
        return true;
    };
    spider.tick = [] { TickEscape(kSpider); };
    Add({ .id = "combat.executioner_release", .name = "Réaction du bourreau à la libération",
          .help = "Ce que fait le bourreau (et le monstre à la hache de Lost in Nightmares) quand tu te libères de sa prise, "
                  "seul (prompt du trainer) ou grâce à un coup qu'il reçoit (ton partenaire). Coup reçu : sa réaction du "
                  "jeu. À genoux : il s'agenouille, ce qui ouvre un corps à corps. Recul : un pas en arrière.",
          .category = "Combat", .kind = Kind::Choice,
          .tick = TickReleaseSafety,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              if (index > 0 && !EnsureEnemyActionHook()) return false;
              g_releaseReaction = index;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 2, .valueFormat = "%.0f",
          .choices = { "Coup reçu (par défaut)", "À genoux (ouvre un corps à corps)", "Recul d'un pas" } });
    OnHud(DrawStruggle);

    Add({ .id = "combat.auto_escape", .name = "Échappements automatiques",
          .help = "Toutes les prises dont on se libère en se débattant (Majini, langue de licker, chien...) sont gagnées "
                  "tout de suite, sans marteler. Le saut d'un licker aussi si « Se libérer seul d'un licker » est coché.",
          .category = "Combat", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureStruggleHooks()) return false;
              g_autoEscape = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Feature& drain = Add({ .id = "combat.licker_drain", .name = "Saisie du licker : drain de vie",
                           .help = "Quand un licker te saute dessus, tu perds de la vie tant qu'il te tient, comme pendant "
                                   "les autres prises (une morsure de Majini draine environ 180 PV/s). Le curseur règle "
                                   "les PV perdus par seconde ; jamais en dessous de 1 PV (c'est son coup final qui tue). "
                                   "La vie infinie et l'invincibilité protègent toujours.",
                           .category = "Combat", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = 200, .valueMin = 20, .valueMax = 400, .valueFormat = "%.0f PV/s" });
    Feature* const pd = &drain;
    drain.set = [pd](bool on) {
        if (on && !EnsureDrainHook()) return false;
        g_drainRate = pd->value;
        g_drainDebt = 0;
        g_drainOn = on;
        return true;
    };
    drain.tick = [pd] { g_drainRate.store(pd->value, std::memory_order_relaxed); };

    Feature& easy = Add({ .id = "combat.easy_struggle", .name = "Débattement facile",
                          .help = "Chaque mouvement gauche / droite compte plusieurs fois quand tu te débats (prises du "
                                  "jeu et prompt du licker) : le curseur règle le multiplicateur. Le jeu demande environ "
                                  "27 allers-retours normalement.",
                          .category = "Combat", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = 3, .valueMin = 1.5f, .valueMax = 10, .valueFormat = "x%.1f" });
    Feature* const pe = &easy;
    easy.set = [pe](bool on) {
        if (on && !EnsureStruggleHooks()) return false;
        g_easyFactor = pe->value;
        g_easyStruggle = on;
        return true;
    };
    easy.tick = [pe] { g_easyFactor.store(pe->value, std::memory_order_relaxed); };

    {
        Feature& butcher = Add({ .id = "enemies.mad_butcher", .name = "Bourreau en folie (Mad Butcher)",
                                 .help = "Comme l'option « Mad Butcher » du Ultimate Trainer : chaque attaque du bourreau devient "
                                         "son enchaînement de frappes horizontales, et dès qu'il est à la distance du curseur "
                                         "de toi il s'y met au lieu de marcher ou courir.",
                                 .category = "Ennemis", .kind = Kind::Toggle,
                                 .available = [] { return game::IsSupported(); },
                                 .value = 6, .valueMin = 2, .valueMax = 20, .valueFormat = "%.0f m" });
        Feature* const pb = &butcher;
        butcher.set = [pb](bool on) {
            if (on && !EnsureEnemyActionHook()) return false;
            g_madButcherReach = pb->value * 100.0f;
            g_madButcher = on;
            return true;
        };
        butcher.tick = [pb] { g_madButcherReach.store(pb->value * 100.0f, std::memory_order_relaxed); };
    }
    {
        Feature& chainsaw = Add({ .id = "enemies.mad_chainsaw", .name = "Majini tronçonneuse en folie (Mad Chainsaw)",
                                  .help = "Comme l'option « Mad Chainsaw » du Ultimate Trainer : chaque attaque du Majini à la "
                                          "tronçonneuse devient sa tronçonneuse dans tous les sens (son état quand il se relève "
                                          "une deuxième fois), et dès qu'il est à la distance du curseur de toi il s'y met au "
                                          "lieu de marcher ou courir.",
                                  .category = "Ennemis", .kind = Kind::Toggle,
                                  .available = [] { return game::IsSupported(); },
                                  .value = 6, .valueMin = 2, .valueMax = 20, .valueFormat = "%.0f m" });
        Feature* const pc = &chainsaw;
        chainsaw.set = [pc](bool on) {
            if (on && !EnsureEnemyActionHook()) return false;
            g_madChainsawReach = pc->value * 100.0f;
            g_madChainsaw = on;
            return true;
        };
        chainsaw.tick = [pc] { g_madChainsawReach.store(pc->value * 100.0f, std::memory_order_relaxed); };
    }
    Add({ .id = "combat.dying_grab_kill", .name = "Saisies mortelles en étant mourant",
          .help = "Une saisie quand tu es mourant te tue tout de suite avec le coup final de l'ennemi, sans lutte ni "
                  "sauvetage du partenaire : de face, la morsure du Majini arrive aussitôt ; de dos, il te brise la nuque ; "
                  "un licker t'achève ; les autres (chiens...) te tuent à leur première morsure. Le bourreau (qui ne saisit "
                  "pas un mourant) et le Majini à la tronçonneuse passent en folie près de toi : frappes horizontales "
                  "enchaînées, tronçonneuse dans tous les sens, dès les distances réglées dans « Bourreau en folie » et "
                  "« Majini tronçonneuse en folie ». Les ennemis ne saisissent en étant mourant qu'avec « Saisie en étant mourant » (onglet "
                  "Ennemis).",
          .category = "Combat", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsurePlayerDamageHook() || !EnsureEnemyActionHook())) return false;
              g_fatalGrabs = on;
              return true;
          },
          .tick = TickFatalGrabs,
          .available = [] { return game::IsSupported(); } });
    Feature& back = Add({ .id = "combat.back_grab_presses", .name = "Prise de dos : se libérer en appuyant",
                          .help = "Saisi de dos par un Majini, Chris ne se libère plus tout seul avec le temps : seuls tes "
                                  "appuis remplissent la jauge, comme pour une prise de face, dans le temps du curseur (une "
                                  "barre rouge le montre). Raté : le Majini lui brise la nuque (le neck break de Chris à "
                                  "l'envers) et il meurt.",
                          .category = "Combat", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = 4, .valueMin = 1, .valueMax = 15, .valueFormat = "%.1f s" });
    Feature* const pb = &back;
    back.set = [pb](bool on) {
        if (on && !EnsureStruggleHooks()) return false;
        if (on && !EnsurePauseMenuWatch()) LOG("back grab: no pause menu watch, its bar runs in the pause");
        g_backSeconds = pb->value;
        g_backPresses = on;
        if (!on) g_backHolder = 0;
        return true;
    };
    back.tick = [pb] {
        g_backSeconds.store(pb->value, std::memory_order_relaxed);
        TickBackHold();
    };
    OnHud(DrawBackHold);

    Feature& grace = Add({ .id = "combat.grab_grace", .name = "Délai avant les dégâts d'une prise",
                           .help = "Au début d'une prise où l'on peut se débattre, le jeu laisse un moment sans dégâts "
                                   "(environ 1,35 s pour un chien, 2 s pour un Majini, rien pour la langue d'un licker) : "
                                   "le curseur le multiplie (x2 = 2,7 s pour un chien). En se débattant vite on s'échappe "
                                   "sans perdre de vie. Sous x1, plus dur : l'ennemi qui te saisit est accéléré jusqu'à sa "
                                   "première morsure (x0,5 = deux fois plus vite). Pas le saut d'un licker (pas de prompt "
                                   "du jeu).",
                           .category = "Combat", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = 2, .valueMin = 0.5f, .valueMax = 6, .valueFormat = "x%.1f" });
    Feature* const pg = &grace;
    grace.set = [pg](bool on) {
        if (on && !EnsurePlayerDamageHook()) return false;
        g_graceFactor = pg->value;
        g_heldSince = 0;
        g_gameWindow = 0;
        g_graceOn = on;
        if (!on) RestoreEnemySpeed();
        return true;
    };
    grace.tick = [pg] {
        g_graceFactor.store(pg->value, std::memory_order_relaxed);
        TickGrabGrace();
    };
}
}  // namespace features
