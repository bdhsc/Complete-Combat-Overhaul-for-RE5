// Cranked gates held open (user, 2026-10-27, end of the caves: "turn the crank, the gate opens, let go and it closes: no
// way through alone; hold it open some seconds when I let go, a slider for how long"). The crank is a uOmCrank (uOm0872
// there; also uOm08a6, uOm08df, uOm08eb, uOm021b, uOm021e, uOm021f, uOm0220, uOm0221) with three actions: 0 cActWait,
// 1 cActWind (a player turns it: the gate rises), 2 cActRewind (let go: it turns back and the gate comes down). Its
// requests go through the units' request core (RVA 0x759030, action_log.cpp's HookRequestCore): Rewind is refused for
// the slider's seconds after the first ask, then asked again by the trainer (vtable +0xDC with the arguments the crank
// gave) if the crank did not ask it again itself.
// Cranked bridges (user, 2026-10-28: "a crank raises a bridge, it must be held for Sheva to cross; I need much more
// time to cross the bridge"): the same crank family, uOm08a6 (its actions the same three), with its own option and
// delay; the gates' option leaves the bridges alone. The time left before it comes down is shown at the top of the
// screen as a countdown (user).
// The time runs from the player's letting go, never from the end of the winding (user, 2026-10-28: the countdowns
// started at the end of the travel while he still held the handle): while a player is in the crank's player action
// uOmCrank::cActPlWind ([P+0x10F8], vtable RVA 0xBCCBA8) the rewind is refused without the time running; holding it
// again stops the time.
// Cranks turned with both hands (user, 2026-10-28: "force the weapons in hand to disappear during every crank"): the
// weapon in hand of a player in the crank's player actions (cActPlWind, its letting go cActPlLeave vtable RVA 0xBCCB00)
// is hidden (byte +5 bit 0x08 of [P+0x2634] = drawn, as the rocket catch does), given back after.
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <imgui.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace features {
bool EnsureEnemyActionHook();  // action_log.cpp: the units' request core hook
bool EnsurePadQueryHooks();    // quick_knife.cpp: CrankTick runs on the game thread
bool IsSoloRaftCrank(uintptr_t unit);  // raft.cpp: the raft's crank (the same class as the bridge's) is its own

namespace {
constexpr int kRewind = 2;
// Lost in Nightmares' platforms raised by a crank (user, 2026-11-03: room 0x2C1, uOm021e / 021f / 0220 / 0221): their
// own option and delay, the gates' option leaving them alone.
enum CrankType { kNone = -1, kGate = 0, kBridge = 1, kPlatform = 2 };
std::atomic<bool> g_on[3]{};
std::atomic<float> g_seconds[3]{ 8.0f, 30.0f, 60.0f };

// A class chain holding uOmCrank (GetDTI = vtable slot 4, `mov eax, dti; ret`; DTI +4 name, +0x10 parent), by vtable:
// a bridge for the classes that raise one (their own class, the chain's first name), a gate otherwise.
CrankType CrankKind(uintptr_t unit) {
    static uintptr_t vtables[8] = {};
    static CrankType results[8] = {};
    static int next = 0;
    uintptr_t vtable = 0;
    if (!unit || !mem::Read(unit, vtable) || !vtable) return kNone;
    for (int i = 0; i < 8; ++i)
        if (vtables[i] == vtable) return results[i];
    CrankType kind = kNone;
    bool bridge = false, platform = false;
    uintptr_t fn = 0, dti = 0;
    uint8_t code[6]{};
    if (mem::Read(vtable + 0x10, fn) && mem::SafeRead(reinterpret_cast<void*>(fn), code, sizeof(code)) && code[0] == 0xB8 &&
        code[5] == 0xC3) {
        std::memcpy(&dti, code + 1, 4);
        for (int depth = 0; dti && depth < 8 && kind == kNone; ++depth) {
            uintptr_t name = 0;
            char text[32]{};
            if (!mem::Read(dti + 4, name) || !name || !mem::SafeRead(reinterpret_cast<void*>(name), text, sizeof(text) - 1))
                break;
            if (depth == 0) {
                bridge = std::strcmp(text, "uOm08a6") == 0;
                platform = std::strcmp(text, "uOm021e") == 0 || std::strcmp(text, "uOm021f") == 0 ||
                           std::strcmp(text, "uOm0220") == 0 || std::strcmp(text, "uOm0221") == 0;
            }
            if (std::strcmp(text, "uOmCrank") == 0) kind = bridge ? kBridge : platform ? kPlatform : kGate;
            if (!mem::Read(dti + 0x10, dti)) break;
        }
    }
    vtables[next] = vtable;
    results[next] = kind;
    next = (next + 1) % 8;
    return kind;
}

bool IsCrank(uintptr_t unit) { return CrankKind(unit) != kNone; }

// Some cranks' delay multiplied (user, 2026-11-03: "some platforms are farther than others"): room, place (cm), factor.
struct CrankFactor {
    uint32_t room;
    float x, y, z, factor;
};
constexpr CrankFactor kCrankFactors[] = { { 0x2C1, -203.0f, 401.0f, 377.0f, 2.0f },    // the platform uOm0220
                                          { 0x2C1, 1195.0f, 100.0f, 1104.0f, 2.75f } };  // the platform uOm0221

bool CrankAt(uintptr_t unit, uint32_t room, float x, float y, float z) {
    uintptr_t profile = 0;
    uint32_t index = 0, now = 0;
    float pos[3]{};
    if (!unit || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), profile) || !profile ||
        !mem::Read(profile + 0x20, index) || index >= 4 || !mem::Read(profile + index * 0x5940 + 0x273D8, now) ||
        now != room || !mem::SafeRead(reinterpret_cast<void*>(unit + 0x30), pos, sizeof(pos)))
        return false;
    const float dx = pos[0] - x, dy = pos[1] - y, dz = pos[2] - z;
    return dx * dx + dy * dy + dz * dz < 100.0f * 100.0f;
}

float Factor(uintptr_t crank) {
    for (const CrankFactor& f : kCrankFactors)
        if (CrankAt(crank, f.room, f.x, f.y, f.z)) return f.factor;
    return 1.0f;
}

float Seconds(uintptr_t crank) {
    const CrankType kind = CrankKind(crank);
    return kind == kNone ? 0.0f : g_seconds[kind].load(std::memory_order_relaxed) * Factor(crank);
}

// The crank held (game thread).
struct Held {
    uintptr_t crank = 0;
    DWORD since = 0;
    int args[4] = {};
};
Held g_held;  // since 0: a player still holds the handle
bool g_replaying = false;
std::atomic<DWORD> g_bridgeSince{ 0 };  // a bridge or a platform held up since then (0 = none), for the countdown
std::atomic<int> g_countdownKind{ kBridge };
std::atomic<float> g_countdownFactor{ 1.0f };
int g_lastWinder = -1;  // the player last seen winding a crank (game thread)

void Release() {
    g_held = {};
    g_bridgeSince = 0;
}

void StartTime(DWORD since) {
    g_held.since = since;
    const CrankType kind = CrankKind(g_held.crank);
    if (kind == kBridge || kind == kPlatform) {
        g_countdownKind = kind;
        g_countdownFactor = Factor(g_held.crank);
        g_bridgeSince = since;
    }
}

void DrawCountdown() {
    const DWORD since = g_bridgeSince.load(std::memory_order_relaxed);
    if (!since) return;
    const int kind = g_countdownKind.load(std::memory_order_relaxed);
    const float left = g_seconds[kind].load(std::memory_order_relaxed) * g_countdownFactor.load(std::memory_order_relaxed) -
                       static_cast<float>(GetTickCount() - since) / 1000.0f;
    if (left <= 0.0f) return;
    char text[48];
    std::snprintf(text, sizeof(text), kind == kPlatform ? i18n::Tr("Plateforme : %d s") : i18n::Tr("Pont levé : %d s"),
                  static_cast<int>(std::ceil(left)));
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float size = screen.y * 0.045f;
    const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 at((screen.x - extent.x) * 0.5f, screen.y * 0.08f);
    const ImU32 colour = left <= 5.0f ? IM_COL32(255, 90, 70, 255) : IM_COL32(255, 235, 200, 255);  // red at the end
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(ImGui::GetFont(), size, at, colour, text);
}
}  // namespace

// The player's hands are on a crank's handle (winding, or letting it go).
bool OnCrank(uintptr_t p) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    uintptr_t action = 0, vtable = 0;
    return p && mem::Read(p + 0x10F8, action) && action && mem::Read(action, vtable) &&
           (vtable == base + 0xBCCBA8 || vtable == base + 0xBCCB00);
}

std::atomic<bool> g_hideWeapon{ false };
bool g_weaponHidden[2] = {}, g_weaponWasDrawn[2] = {};  // render thread

// player.cpp's "Arme toujours visible" leaves it hidden.
bool CrankHidesWeapon(int index) { return g_hideWeapon.load(std::memory_order_relaxed) && OnCrank(actors::Player(index)); }

void TickCrankWeapon(bool on) {
    for (int i = 0; i < 2; ++i) {
        const uintptr_t p = actors::InGame() ? actors::Player(i) : 0;
        uintptr_t weapon = 0;
        uint8_t flags = 0;
        if (!p || !mem::Read(p + 0x2634, weapon) || !weapon || !mem::Read(weapon + 5, flags)) {
            g_weaponHidden[i] = false;
            continue;
        }
        if (on && OnCrank(p)) {
            if (!g_weaponHidden[i]) g_weaponWasDrawn[i] = (flags & 0x08) != 0;
            g_weaponHidden[i] = true;
            if (flags & 0x08) mem::Write<uint8_t>(weapon + 5, flags & ~0x08);
        } else if (g_weaponHidden[i]) {
            g_weaponHidden[i] = false;
            if (g_weaponWasDrawn[i] && !(flags & 0x08)) mem::Write<uint8_t>(weapon + 5, flags | 0x08);
        }
    }
}

// A player holds a crank's handle (raft.cpp too).
bool CrankHeldByPlayer() {
    const uintptr_t wind = reinterpret_cast<uintptr_t>(mem::Base()) + 0xBCCBA8;
    for (int i = 0; i < 2; ++i) {
        const uintptr_t p = actors::Player(i);
        uintptr_t action = 0, vtable = 0;
        if (p && mem::Read(p + 0x10F8, action) && action && mem::Read(action, vtable) && vtable == wind) return true;
    }
    return false;
}

// A crank whose gate never comes down once let go (user, 2026-11-03: "one crank I want purely neutralised: let go, the
// gate stays where it is, unless I keep turning it because it was not at the top"): Lost in Nightmares' second crank,
// the uOm0872 of room 0x2BD. With the gates' option its Rewind is always refused (no countdown); turning it again goes on
// raising the gate.
struct StuckCrank {
    uint32_t room;
    float x, y, z;
};
constexpr StuckCrank kStuckCranks[] = { { 0x2BD, 1523.0f, 390.0f, -1298.0f } };

bool StuckCrankAt(uintptr_t unit) {
    for (const StuckCrank& c : kStuckCranks)
        if (CrankAt(unit, c.room, c.x, c.y, c.z)) return true;
    return false;
}

// action_log.cpp's HookRequestCore: true refuses the request.
bool CrankHoldsOpen(uintptr_t unit, int action, int a, int b, int c, int d) {
    if (g_replaying || action != kRewind || IsSoloRaftCrank(unit)) return false;
    const CrankType kind = CrankKind(unit);
    if (kind == kNone || !g_on[kind].load(std::memory_order_relaxed)) return false;
    if (kind == kGate && StuckCrankAt(unit)) {
        static uintptr_t told = 0;
        if (told != unit) LOG("crank: %08X let go, its gate stays where it is (no rewind)", static_cast<unsigned>(unit));
        told = unit;
        return true;
    }
    // A platform held only when player 1 wound it in solo (user, 2026-11-03: "if player 2 holds the crank, the game's
    // behaviour without a timer; the timer only when player 1 turned it and the game is solo"): let go by Jill (Chris's
    // order "let go" drops it), or with a human player 2 (+0x2DA8 not 1, the AI's), not held.
    if (kind == kPlatform && g_held.crank != unit) {
        const uintptr_t p2 = actors::Player(1);
        uint32_t controller = 0;
        if (g_lastWinder != 0 || !p2 || !mem::Read(p2 + 0x2DA8, controller) || controller != 1) return false;
    }
    const DWORD now = GetTickCount();
    if (g_held.crank != unit) {
        g_held = { unit, 0, { a, b, c, d } };
        const bool held = CrankHeldByPlayer();
        if (!held) StartTime(now ? now : 1);
        LOG("crank: %08X rewind asked%s, its %s held %.1f s from the letting go", static_cast<unsigned>(unit),
            held ? " (the handle still held)" : "", kind == kBridge ? "bridge" : "gate", Seconds(unit));
    }
    if (!g_held.since || static_cast<float>(now - g_held.since) < Seconds(unit) * 1000.0f) return true;
    Release();
    return false;
}

// quick_knife.cpp's pad query hook (game thread): the time up, Rewind asked again unless the crank did it or is turned.
void CrankTick() {
    for (int i = 0; i < 2; ++i) {
        const uintptr_t p = actors::InGame() ? actors::Player(i) : 0;
        uintptr_t action = 0, vtable = 0;
        if (p && mem::Read(p + 0x10F8, action) && action && mem::Read(action, vtable) &&
            vtable == reinterpret_cast<uintptr_t>(mem::Base()) + 0xBCCBA8)
            g_lastWinder = i;
    }
    if (!g_held.crank) return;
    const uintptr_t crank = g_held.crank;
    uint32_t action = 0;
    uintptr_t vtable = 0, fn = 0;
    if (!actors::InGame() || !mem::Read(crank + 0x10E0, action) || !IsCrank(crank)) {
        Release();
        return;
    }
    if (action == kRewind || action == 0) {  // rewinding already, or back at rest
        Release();
        return;
    }
    const DWORD now = GetTickCount();
    if (CrankHeldByPlayer()) {  // still or again in his hands: no time running
        if (g_held.since) LOG("crank: %08X held again", static_cast<unsigned>(crank));
        StartTime(0);
        return;
    }
    if (!g_held.since) {
        StartTime(now ? now : 1);
        LOG("crank: %08X let go", static_cast<unsigned>(crank));
        return;
    }
    if (static_cast<float>(now - g_held.since) < Seconds(crank) * 1000.0f) return;
    const Held held = g_held;
    Release();
    if (!mem::Read(crank, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return;
    using RequestFn = bool(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    g_replaying = true;
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(crank), nullptr, kRewind, held.args[0], held.args[1],
                                        held.args[2], held.args[3]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_replaying = false;
    LOG("crank: %08X rewinds (its action was %X)", static_cast<unsigned>(crank), action);
}

// The platforms' prompts (recorded 2026-11-03 in room 0x2C1): at rest the turning prompt (handler VA 0xAB19E0, its
// condition VA 0xAB18F0) and the order to the partner (handler VA 0xAB1E10, on the action icon 1 = E). The condition,
// cdecl(crank, slot) -> bool, in room 0x2C1 shows the turning prompt to an AI player, or to a human whose partner is
// human too: in solo only Jill turns, Chris orders her (user: "an E prompt, but it sends Jill"). With the option: shown
// to player 1 too while nobody turns it (+0x1995 clear), and the order on the partner icon 4 (F; the icon is also the
// button the game takes). While Jill turns, her "let go" order (F) is the game's.
using TurnConditionFn = bool(__cdecl*)(uintptr_t crank, int slot);
TurnConditionFn g_turnCondition = nullptr;

void OrderOnPartnerButton(uintptr_t crank) {
    uintptr_t system = 0, array = 0;
    uint32_t count = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1794)), system) || !system || !mem::Read(system + 0x30, count) ||
        !mem::Read(system + 0x3C, array) || !array)
        return;
    const uintptr_t order = reinterpret_cast<uintptr_t>(mem::Rva(0x6B1E10));
    for (uint32_t i = 0; i < count && i < 1024; ++i) {
        uintptr_t prompt = 0, owner = 0, handler = 0;
        uint32_t icon = 0;
        if (!mem::Read(array + 4 * i, prompt) || !prompt || !mem::Read(prompt + 4, owner) || owner != crank ||
            !mem::Read(prompt + 0x3C, handler) || handler != order || !mem::Read(prompt + 0x20, icon))
            continue;
        if (icon != 4) mem::Write<uint32_t>(prompt + 0x20, 4);
    }
}

bool __cdecl HookTurnCondition(uintptr_t crank, int slot) {
    const bool shown = g_turnCondition(crank, slot);
    uint8_t turned = 1;
    if (shown || !g_on[kPlatform].load(std::memory_order_relaxed) || CrankKind(crank) != kPlatform ||
        !actors::SlotIs(slot, 0) || !mem::Read(crank + 0x1995, turned) || turned)
        return shown;
    OrderOnPartnerButton(crank);
    return true;
}

bool EnsureTurnConditionHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x6B18F0);
    // push esi; mov esi, [esp+8]; mov eax, [esi]; mov edx, [eax+0x10]
    const uint8_t expected[] = { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x06, 0x8B, 0x50, 0x10 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("crank: turning condition, unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookTurnCondition), reinterpret_cast<void**>(&g_turnCondition));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("crank: turning condition hooked at %p", target);
    return true;
}

void RegisterCrank() {
    Feature& gate = Add({ .id = "player.crank_hold", .name = "Grille à manivelle : reste ouverte",
                          .help = "Quand on lâche une manivelle qui lève une grille, la grille reste ouverte le nombre de "
                                  "secondes du curseur avant de redescendre : le temps de passer seul. La deuxième "
                                  "manivelle de Lost in Nightmares garde sa grille là où on la lâche (on peut la "
                                  "reprendre pour la monter plus haut).",
                          .category = "Joueur", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = 8, .valueMin = 1, .valueMax = 30, .valueFormat = "%.0f s" });
    Feature& bridge = Add({ .id = "player.crank_bridge_hold", .name = "Pont à manivelle : reste levé",
                            .help = "Quand on lâche une manivelle qui lève un pont, le pont reste levé le nombre de "
                                    "secondes du curseur avant de redescendre : le temps de le traverser seul. Le temps restant "
                                    "s'affiche en haut de l'écran (en rouge les 5 dernières secondes).",
                            .category = "Joueur", .kind = Kind::Toggle,
                            .available = [] { return game::IsSupported(); },
                            .value = 30, .valueMin = 5, .valueMax = 180, .valueFormat = "%.0f s" });
    Feature& platform = Add({ .id = "player.crank_platform", .name = "Plateformes à manivelle (Lost in Nightmares)",
                              .help = "Les manivelles qui lèvent les plateformes de Lost in Nightmares : E fait tourner "
                                      "Chris lui-même, F envoie Jill la tourner. Quand Chris lâche la manivelle, la "
                                      "plateforme reste levée le nombre de secondes du curseur (le temps restant s'affiche "
                                      "en haut de l'écran) ; quand Jill la lâche sur ton ordre, elle tombe comme dans le "
                                      "jeu.",
                              .category = "Joueur", .kind = Kind::Toggle,
                              .available = [] { return game::IsSupported(); },
                              .value = 60, .valueMin = 5, .valueMax = 180, .valueFormat = "%.0f s" });
    Feature* const features[3] = { &gate, &bridge, &platform };
    for (int kind = 0; kind < 3; ++kind) {
        Feature* const self = features[kind];
        self->set = [self, kind](bool on) {
            if (on && (!EnsureEnemyActionHook() || !EnsurePadQueryHooks())) return false;
            if (on && kind == kPlatform && !EnsureTurnConditionHook()) return false;
            g_seconds[kind] = self->value;
            g_on[kind] = on;
            if (!on && CrankKind(g_held.crank) == kind) Release();
            return true;
        };
        self->tick = [self, kind] { g_seconds[kind].store(self->value, std::memory_order_relaxed); };
    }
    OnHud(DrawCountdown);
    Add({ .id = "player.crank_hide_weapon", .name = "Manivelles : arme cachée",
          .help = "Pendant qu'un joueur tourne une manivelle (à deux mains), l'arme qu'il tient disparaît de sa main ; "
                  "elle revient quand il la lâche.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_hideWeapon = on;
              if (!on) TickCrankWeapon(false);  // given back
              return true;
          },
          .tick = [] { TickCrankWeapon(true); },
          .available = [] { return game::IsSupported(); }, .defaultOn = true });
}
}  // namespace features
