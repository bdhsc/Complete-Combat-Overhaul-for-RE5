// "Endurance" (user, 2026-10-26, after a mod's HUD: a second ring outside the health ring): each free melee move or dodge
// of the trainer costs 500 stamina points (1000 at first; a slider raises the points and the arc with them). At 0 a
// red arc fills over it and, until it is full, none of the trainer's melee moves, dodges, quick shots or parries can
// be used; then the red arc goes and the stamina refills slowly. Two sliders: the red arc's time, the refill speed.
// Gates: swaps.cpp (PlayFreeMove, PlayDodge, PlayAimDodge, PlayFixedDodge), wesker_moves.cpp (StartCombatMove, not its
// chained catch nor a parry's forced riposte), parry.cpp (the RE4 / RE6 presses; the RE6, guard and throw parries cost
// 500 too, not the knife parry), quick_knife.cpp (the quick shot).
// The ring is drawn around player 1's health ring (its place measured on the user's screen); its colour is in Affichage.
// Free but holding the refill (stamina and red arc) while they play, as the sprint (user, 2026-10-26): the game's melee
// moves and dodges, Wesker's guard (the RE7 parry), the ground rolls, the grab struggles. The RE7 parry stays possible
// with the red arc; "Endurance stricte" refuses it and the game's melee prompts then, and makes them cost 500 (the
// game's dodges never: user). Dying empties it, and with 1 health (dying, or after it) the red arc waits for a heal; any
// heal fills it at once. While the red arc fills Chris takes his injured pose (the game's own: its story injury test).
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>

#include <MinHook.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace features {
bool PauseMenuIsShown();      // pickup.cpp
bool Player1Sprinting();      // movement.cpp: the trainer's sprint running
uintptr_t Player1Cockpit();   // player.cpp: player 1's HUD
bool Player1HudShown();       // player.cpp: the health ring on screen
bool EnsureCockpitWatch();    // player.cpp
bool EnsurePauseMenuWatch();  // pickup.cpp
bool EnsurePromptPressHook(); // action_log.cpp: the prompt presses
bool EnsureSetMotionHook();   // swaps.cpp: the stamina-out pose kept through the game's leg-layer requests (movement.cpp)
bool Player1InMeleeAttack();  // wesker_moves.cpp: the game's melee moves, the trainer's strikes, the escapes
bool Player1InTrainerDodge(); // wesker_moves.cpp: a trainer dodge or getup, RE6's slide / dives under the aim
bool WeskerGuardPlaying();    // wesker_moves.cpp: the RE7 parry's guard
bool Player1Dying();          // dying.cpp
bool Player1RingOnLeft();     // player.cpp: his character left-handed by the game
bool TimeDilationSlowing();   // time_speed.cpp: "Vitesse du temps" slowing the game
void StopTimeDilation();      // time_speed.cpp

namespace {
// The costs (user, 2026-10-26: one slider each): the trainer's actions (500), the game's prompts under "Endurance
// stricte" (200), RE6's techniques once imported (500, unused yet), the RE6 parry's counter (500), Wesker's guard (the
// RE7 parry, under "Endurance stricte" only: 200).
std::atomic<float> g_costTrainer{ 500.0f }, g_costNative{ 200.0f }, g_costRe6{ 500.0f }, g_costRe6Counter{ 500.0f },
    g_costRe7Guard{ 200.0f };
constexpr float kBasePoints = 1000.0f;   // the arc of the mod's picture
constexpr float kBaseSpan = 3.14159265f; // 1000 points = half a turn

std::atomic<bool> g_on{ false };
std::atomic<float> g_max{ 1000.0f }, g_regen{ 150.0f }, g_exhaustSeconds{ 3.0f };
// Around the health ring (measured on the user's screenshots at 2048x1280, 2026-10-26; the sliders that placed it are
// gone: user): its centre and radius as parts of the screen.
constexpr float kCentreX = 0.8135f, kCentreY = 0.8275f, kRadius = 0.098f;
// The left ring of a left-handed player 1 (Sheva, user's screenshots 2026-10-26): circles fitted on the pixels of the
// arc (known place and size: the scale) and of her ring, its dark pulse included (the bright part alone put it 10 px
// too high: user "too high"): (0.1885, 0.8240) and (0.1882, 0.8272) on two pictures - the right ring's height (the
// same fit on Chris's picture: (0.8140, 0.8268) for the arc's (0.8135, 0.8275)), near the right one's mirror (0.1865).
constexpr float kLeftCentreX = 0.1883f, kLeftCentreY = kCentreY;
std::atomic<float> g_thickness{ 1.0f };
// "Endurance : coût du sprint" (user, 2026-10-26): checked, the trainer's sprint drains the slider's points per second;
// unchecked it costs nothing; the stamina never refills while sprinting. Either way it stops while the red arc fills (movement.cpp).
std::atomic<bool> g_sprintCostOn{ false };
std::atomic<float> g_sprintCost{ 150.0f };
// "Endurance : le ralenti consomme" (user, 2026-10-26): "Vitesse du temps" slowing the game drains the slider's points
// per second (real time) and never refills meanwhile; at 25 % it stops by itself and under 25 % it is refused.
std::atomic<bool> g_slowCostOn{ false };
std::atomic<float> g_slowCost{ 50.0f };
constexpr float kSlowFloor = 0.25f;
// Its own colour (user, 2026-10-26: "to tell it apart from the health ring, or make it the same"), 0xRRGGBB.
std::atomic<uint32_t> g_colour{ 0x15920D };
float g_stamina = 1000.0f;   // render thread
bool g_exhausted = false;
float g_red = 0.0f;          // the red arc, 0..1
LARGE_INTEGER g_last{};
// "Endurance stricte" (user, 2026-10-26): the game's melee prompts and the RE7 parry refused while the red arc fills,
// each costing 500.
std::atomic<bool> g_strict{ false };
// "Endurance : pose d'épuisement de RE6": while the red arc fills, RE6's stamina-out upper body (movement.cpp) instead of the injured pose.
std::atomic<bool> g_re6Pose{ false };
std::atomic<DWORD> g_lastSpend{ 0 };
std::atomic<bool> g_injuredPose{ false };  // read by the game's thread (HookStoryInjured)
// Dying and healing (user, 2026-10-26: "while health is 1 the red arc cannot fill; out of dying there is no stamina
// until a heal; any heal fills it at once"). A heal = health above what it was a second ago (a parry gives back the
// blow's health at once: not a heal), not the revive (the rise that ends dying).
bool g_needHeal = false;
// player.cpp (user, 2026-10-26): "Soigner le joueur" is a heal even while dying, and "Vie max" scaling his health is none (its slider filled the stamina: "healed (12 -> 16)").
std::atomic<bool> g_trainerHeal{ false }, g_healthRescaled{ false };
uintptr_t g_healthOf = 0;
int16_t g_healthRef = 0;
DWORD g_lowSince = 0;
// Dying last frame: the health rise that ends it is the revive (user, 2026-10-26: a 2 s window after it also ate a
// heal taken 0.7 s after the RE6 getup, 3 -> 1000, and left the stamina stuck), any later rise a heal.
bool g_wasDying = false;

void Exhaust(const char* why) {
    if (!g_exhausted) LOG("stamina: %s, the red arc fills", why);
    g_stamina = 0.0f;
    g_exhausted = true;
    g_red = 0.0f;
}

// Player 1's health this frame: dying empties the stamina, a heal fills it. False while the red arc must wait.
bool WatchHealth(uintptr_t p, float max) {
    int16_t health = 0;
    if (!mem::Read(p + 0x1364, health)) return true;
    const DWORD now = GetTickCount();
    if (p != g_healthOf) {
        g_healthOf = p;
        g_healthRef = health;
        g_lowSince = 0;
        g_needHeal = false;
    }
    if (g_healthRescaled.exchange(false)) {
        g_healthRef = health;
        g_lowSince = 0;
    }
    if (g_trainerHeal.exchange(false) && health > 1) {
        LOG("stamina: the trainer's heal, full");
        g_stamina = max;
        g_exhausted = false;
        g_red = 0.0f;
        g_needHeal = false;
        g_wasDying = false;
        g_healthRef = health;
        g_lowSince = 0;
        return true;
    }
    if (Player1Dying()) {
        Exhaust("dying");
        g_needHeal = true;
        g_wasDying = true;
        g_healthRef = health;
        return false;
    }
    if (g_wasDying) {  // the revive: its health is no heal
        g_wasDying = false;
        g_healthRef = health;
        g_lowSince = 0;
        LOG("stamina: out of dying (health %d), a heal fills it", health);
    } else if (health > g_healthRef) {
        LOG("stamina: healed (%d -> %d), full", g_healthRef, health);
        g_stamina = max;
        g_exhausted = false;
        g_red = 0.0f;
        g_needHeal = false;
        g_healthRef = health;
        g_lowSince = 0;
    } else if (health < g_healthRef) {
        if (!g_lowSince) {
            g_lowSince = now | 1;
        } else if (now - g_lowSince >= 1000) {
            g_healthRef = health;
            g_lowSince = 0;
        }
    } else {
        g_lowSince = 0;
    }
    return !g_needHeal && health > 1;
}

// What holds the refill without costing: the sprint (its cost apart), the game's melee moves (not the knife's slashes
// 0x3A / 0x3E) and the trainer's strikes, the dodges (Quick Duck 0x50, rolls 0x51-0x55: the forward ground roll too),
// Wesker's guard, the DLC back roll (getup 0x97 playing motion 0x720), the struggles (held 0x80000000 / 0x82 / 0x84).
bool RefillHeld(uintptr_t p) {
    uint32_t action = 0, motion = 0;
    if (!mem::Read(p + 0x10E0, action)) return false;
    if (Player1Sprinting() || WeskerGuardPlaying() || Player1InTrainerDodge()) return true;
    if (action != 0x3A && action != 0x3E && Player1InMeleeAttack()) return true;
    if ((action >= 0x50 && action <= 0x55) || action == 0x80000000 || action == 0x82 || action == 0x84) return true;
    return action == 0x97 && mem::Read(p + 0x384, motion) && (motion & 0xFFFF) == 0x720;
}

// The game's story injury test (RVA 0x76AF10, thiscall(player) -> bool; its callers then put the injured locomotion
// type +0x1E30 = 6 and pose state +0x1E2D = 1, as low health does): player 1 injured while the red arc fills.
using StoryInjuredFn = bool(__fastcall*)(void* player, void* edx);
StoryInjuredFn g_storyInjured = nullptr;

bool __fastcall HookStoryInjured(void* player, void* edx) {
    if (g_injuredPose.load(std::memory_order_relaxed) && reinterpret_cast<uintptr_t>(player) == actors::Player(0)) return true;
    return g_storyInjured(player, edx);
}

bool EnsureInjuredHook() {
    static bool hooked = false;
    if (hooked) return true;
    const uint8_t expected[] = { 0x80, 0xB9, 0xDC, 0x1B, 0x00, 0x00, 0x00, 0x74, 0x03 };  // cmp byte [ecx+1BDCh], 0; je
    uint8_t* target = mem::Rva(0x76AF10);
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("stamina: injured test, unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookStoryInjured), reinterpret_cast<void**>(&g_storyInjured));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("stamina: injured test hook installed at %p", target);
    return true;
}

void Tick() {
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_last.QuadPart ? static_cast<float>(now.QuadPart - g_last.QuadPart) / freq.QuadPart : 0.0f;
    g_last = now;
    if (dt > 0.1f) dt = 0.1f;
    const float max = g_max.load(std::memory_order_relaxed);
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!g_on.load(std::memory_order_relaxed) || !p) {
        g_stamina = max;
        g_exhausted = false;
        g_red = 0.0f;
        g_needHeal = false;
        g_healthOf = 0;
        g_injuredPose = false;
        return;
    }
    if (PauseMenuIsShown()) return;
    const bool mayFill = WatchHealth(p, max);
    g_injuredPose = g_exhausted && !g_re6Pose.load(std::memory_order_relaxed);
    const bool held = RefillHeld(p);
    if (g_exhausted) {
        // Emptied by a move during the slow motion: it stops too.
        if (g_slowCostOn.load(std::memory_order_relaxed) && TimeDilationSlowing()) StopTimeDilation();
        if (!mayFill || held) return;
        const float seconds = std::max(0.1f, g_exhaustSeconds.load(std::memory_order_relaxed));
        g_red += dt / seconds;
        if (g_red >= 1.0f) {
            g_red = 0.0f;
            g_exhausted = false;
            g_stamina = 0.0f;
            LOG("stamina: red arc full, refilling");
        }
        return;
    }
    if (g_slowCostOn.load(std::memory_order_relaxed) && TimeDilationSlowing()) {
        g_stamina -= g_slowCost.load(std::memory_order_relaxed) * dt;
        if (g_stamina <= kSlowFloor * max) {
            g_stamina = std::max(0.0f, std::min(g_stamina, kSlowFloor * max));
            LOG("stamina: 25 %% reached, slow motion stopped");
            StopTimeDilation();
        }
        return;
    }
    // Sprinting it never refills, its cost or not (user, 2026-10-26).
    if (Player1Sprinting()) {
        if (!g_sprintCostOn.load(std::memory_order_relaxed)) return;
        g_stamina -= g_sprintCost.load(std::memory_order_relaxed) * dt;
        if (g_stamina <= 0.0f) Exhaust("emptied by the sprint");
        return;
    }
    if (held) return;
    g_stamina = std::min(max, g_stamina + g_regen.load(std::memory_order_relaxed) * dt);
}

// An arc from angle `from` to `to` (screen radians: 0 right, pi / 2 down), `thickness` pixels.
void Arc(ImDrawList* dl, ImVec2 c, float r, float from, float to, float thickness, ImU32 color) {
    if (std::fabs(to - from) <= 0.001f) return;
    dl->PathArcTo(c, r, from, to, std::max(8, static_cast<int>(std::fabs(to - from) * 24.0f)));
    dl->PathStroke(color, ImDrawFlags_None, thickness);
}

void Draw() {
    // Shown with the health ring only (user, 2026-10-29: it stayed in the pause menu and the in-game cutscenes).
    if (!g_on.load(std::memory_order_relaxed) || !actors::InGame() || !Player1HudShown()) return;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    // A left-handed character has his health ring on the left of the screen (user, 2026-10-26, Sheva: the ring at the
    // bottom left): the arc goes round that ring, the same way as Chris's (user: mirrored, it emptied the wrong way).
    // The character's own hand, not the one he holds his weapons with now ("Main inversée", the bow).
    const bool leftRing = Player1RingOnLeft();
    const ImVec2 c(screen.x * (leftRing ? kLeftCentreX : kCentreX), screen.y * (leftRing ? kLeftCentreY : kCentreY));
    const float r = screen.y * kRadius;
    // The health gauge's band is about 16 px wide at 1280 lines (user's screenshots, 2026-10-26).
    const float thickness = std::max(3.0f, screen.y * 0.0125f * g_thickness.load(std::memory_order_relaxed));
    const float max = g_max.load(std::memory_order_relaxed);
    // 1000 points = the ring's lower half (as in the mod's picture), from its right end (angle 0, by the health ring's
    // side) round the bottom; more points lengthen it from there to the left and up, its origin kept (user,
    // 2026-10-26: "it grew both ways"). Filled from that right end, so it empties from left to right (user: "the wrong
    // way" before).
    const float span = std::min(6.1f, kBaseSpan * max / kBasePoints);
    const float right = 0.0f, left = span;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    Arc(dl, c, r, left, right, thickness + 2.0f, IM_COL32(0, 0, 0, 170));
    const float fill = g_exhausted ? g_red : std::clamp(g_stamina / max, 0.0f, 1.0f);
    const uint32_t rgb = g_exhausted ? 0xD21E1Eu : g_colour.load(std::memory_order_relaxed);
    // Drawn as the health gauge looks (user, 2026-10-26: "same values, yet they do not look alike"; measured on two
    // screenshots, bright and dark phase): a band dark at its edges and bright in its middle, no light core nor outer
    // glow, its middle RGB(21, 146, 13) at the bright phase and (10, 102, 6) at the dark one - a 70 % to 100 % breathing
    // (the ring sprite's period, about 1.5 s). The game passes the gauge's chosen colour through its texture, so the
    // stamina's default is the colour seen, 0x15920D.
    // In step with the game's (user: "opposite pulses"): its HUD's counter +0x1BC (about 30 a second) - the ring's glow
    // is darkest when it is 11 modulo 45 and brightest 22.5 later (sampled 2026-10-26); our own clock without a HUD.
    float phase = 0.0f;
    float counter = 0.0f;
    if (const uintptr_t cockpit = Player1Cockpit(); cockpit && mem::Read(cockpit + 0x1BC, counter) && counter == counter) {
        phase = 0.5f - 0.5f * std::cos(6.2831853f * (std::fmod(counter, 45.0f) - 11.0f) / 45.0f);
    } else {
        static const ULONGLONG start = GetTickCount64();
        phase = 0.5f + 0.5f * std::sin(static_cast<float>(GetTickCount64() - start) * 0.001f * 6.2831853f / 1.5f);
    }
    const float bright = 0.70f + 0.30f * phase;
    const float to = span * fill;
    static constexpr struct { float width, shade; } kBand[] = { { 1.0f, 0.40f }, { 0.72f, 0.75f }, { 0.42f, 1.0f } };
    for (const auto& b : kBand) {
        const float k = bright * b.shade;
        Arc(dl, c, r, right, to, thickness * b.width,
            IM_COL32(static_cast<int>(((rgb >> 16) & 0xFF) * k), static_cast<int>(((rgb >> 8) & 0xFF) * k),
                     static_cast<int>((rgb & 0xFF) * k), 255));
    }
}
}  // namespace

// The gates: false while the red arc fills (the trainer's melee, dodges, quick shots, parries).
bool StaminaAllows() { return !g_on.load(std::memory_order_relaxed) || !g_exhausted; }

// player.cpp: "Soigner le joueur 1" (a heal for the stamina, whatever the dying state).
void StaminaTrainerHeal() { g_trainerHeal = true; }
// player.cpp: "Vie max du joueur 1" scaled his health (no heal).
void StaminaHealthRescaled() { g_healthRescaled = true; }

// time_speed.cpp: "Vitesse du temps" may slow the game: 25 % of the stamina at least when the slow motion costs.
bool StaminaAllowsSlowMotion() {
    if (!g_on.load(std::memory_order_relaxed) || !g_slowCostOn.load(std::memory_order_relaxed) || !actors::InGame())
        return true;
    return !g_exhausted && g_stamina > kSlowFloor * g_max.load(std::memory_order_relaxed);
}

// "Endurance stricte": the game's melee prompts and the RE7 parry refused while the red arc fills, and costing.
bool StaminaStrict() { return g_on.load(std::memory_order_relaxed) && g_strict.load(std::memory_order_relaxed); }

namespace {
// A move's cost; at 0 the red arc starts. Once per move: a swapped move pressed on a game prompt would pay at the press
// and at its start.
void Spend(float cost) {
    if (!g_on.load(std::memory_order_relaxed) || g_exhausted) return;
    const DWORD now = GetTickCount();
    if (now - g_lastSpend.load() < 300) return;
    g_lastSpend = now;
    g_stamina -= cost;
    if (g_stamina <= 0.0f) Exhaust("empty");
}
}  // namespace

// The trainer's free melee moves, dodges, RE6 / throw parries.
void StaminaSpend() { Spend(g_costTrainer); }
// "Endurance stricte": a game melee prompt pressed, the RE7 parry.
void StaminaSpendNative() { Spend(g_costNative); }
// RE6's techniques (to come).
void StaminaSpendRe6() { Spend(g_costRe6); }
// The RE6 parry's counter.
void StaminaSpendRe6Counter() { Spend(g_costRe6Counter); }
// Wesker's guard, the RE7 parry ("Endurance stricte").
void StaminaSpendRe7Guard() { Spend(g_costRe7Guard); }
// A trainer dodge started (user, 2026-11-09: "with Endurance the refill held during the animation, with strict stamina its cost; a slider on
// each dodge's option, at its lowest below 0 the refill held only"): free with "Endurance" (RefillHeld while it plays), its option's
// points with "Endurance stricte" (below 0: free there too).
// movement.cpp's upper overlay: RE6's stamina-out pose while the red arc fills.
bool Player1StaminaOutPose() {
    return g_on.load(std::memory_order_relaxed) && g_re6Pose.load(std::memory_order_relaxed) && g_exhausted;
}

void StaminaSpendDodge(float cost) {
    if (cost >= 0.0f && g_strict.load(std::memory_order_relaxed)) Spend(cost);
}

void RegisterStamina() {
    OnFrame(Tick);
    OnHud(Draw);
    Feature& on = Add({ .id = "player.stamina", .name = "Endurance",
                        .help = "Chaque coup de mêlée libre ou parade (RE6, projection ; pas la parade au "
                                "couteau ni la garde de Wesker) du trainer coûte de l'endurance (arc autour de "
                                "l'anneau de vie ; coûts réglables ci-dessous). À 0, un arc rouge se remplit : jusqu'à ce qu'il soit plein, ni "
                                "mêlée, ni esquive, ni tir rapide, ni parade au couteau du trainer, ni roulade au sol, et "
                                "Chris prend sa pose blessée. Les esquives du trainer (la glissade, les plongées et les "
                                "mouvements au sol de RE6 compris), les mêlées et esquives du jeu, la garde de Wesker, les "
                                "roulades au sol et les luttes ne coûtent rien mais arrêtent le remplissage pendant leur "
                                "animation (avec « Endurance stricte », chaque esquive du trainer coûte son curseur, dans "
                                "Mêlée > Dodge). Mourant : endurance vide, et l'arc rouge attend un soin (tant que la vie est à "
                                "1). Un soin remplit l'endurance. Le curseur règle le nombre de points (et la longueur de "
                                "l'arc).",
                        .category = "Joueur", .kind = Kind::Toggle,
                        .set = [](bool on) {
                            if (on && !EnsurePauseMenuWatch()) return false;
                            if (on && !EnsureCockpitWatch()) LOG("stamina: no HUD hook, its own pulse");
                            if (on && !EnsureInjuredHook()) LOG("stamina: no injured pose");
                            g_on = on;
                            return true;
                        },
                        .available = [] { return game::IsSupported(); },
                        .value = 1000, .valueMin = 500, .valueMax = 4000, .valueFormat = "%.0f pts" });
    Feature* const onFeature = &on;
    on.tick = [onFeature] { g_max.store(onFeature->value, std::memory_order_relaxed); };
    struct Slider {
        const char* id;
        const char* name;
        const char* help;
        float value, min, max;
        const char* format;
        std::atomic<float>* target;
    };
    static const Slider kSliders[] = {
        { "player.stamina_regen", "Endurance : vitesse de remplissage", "Points d'endurance regagnés par seconde.", 150, 10,
          1000, "%.0f pts/s", &g_regen },
        { "player.stamina_delay", "Endurance : durée de la barre rouge",
          "Temps que met l'arc rouge à se remplir quand l'endurance est vide (rien n'est possible pendant ce temps).", 3,
          0.5f, 15, "%.1f s", &g_exhaustSeconds },
        { "player.stamina_thickness", "Endurance : épaisseur", "Épaisseur de l'arc (1 = celle de départ).", 1.0f, 0.3f,
          3.0f, "x%.2f", &g_thickness },
        { "player.stamina_cost", "Endurance : coût des actions du trainer",
          "Points retirés par coup de mêlée libre, tir rapide ou parade-projection du trainer (les esquives ont chacune "
          "leur curseur, dans Mêlée > Dodge, pour l'endurance stricte).", 500, 0,
          2000, "%.0f pts", &g_costTrainer },
        { "player.stamina_cost_native", "Endurance : coût des prompts du jeu (stricte)",
          "Avec « Endurance stricte » : points retirés par prompt de corps à corps du jeu.", 200, 0, 2000, "%.0f pts",
          &g_costNative },
        { "player.stamina_cost_re6_counter", "Endurance : coût du contre RE6",
          "Points retirés par parade RE6 (le contre de Chris).", 500, 0, 2000, "%.0f pts", &g_costRe6Counter },
        { "player.stamina_cost_re7_guard", "Endurance : coût de la garde RE7 (stricte)",
          "Avec « Endurance stricte » : points retirés par garde de Wesker (parade RE7). Sans elle, la garde est "
          "gratuite.", 200, 0, 2000, "%.0f pts", &g_costRe7Guard },
        { "player.stamina_cost_re6", "Endurance : coût des techniques RE6",
          "Pour les techniques et animations de RE6 à importer (sans effet pour l'instant).", 500, 0, 2000, "%.0f pts",
          &g_costRe6 },
    };
    for (const Slider& s : kSliders) {
        Feature& f = Add({ .id = s.id, .name = s.name, .help = s.help, .category = "Joueur", .kind = Kind::Toggle,
                           .set = [](bool) { return true; },
                           .available = [] { return game::IsSupported(); },
                           .value = s.value, .valueMin = s.min, .valueMax = s.max, .valueFormat = s.format,
                           .defaultOn = true });
        Feature* const fp = &f;
        std::atomic<float>* const target = s.target;
        f.tick = [fp, target] { target->store(fp->value, std::memory_order_relaxed); };
    }
    Add({ .id = "player.stamina_strict", .name = "Endurance stricte",
          .help = "En plus de l'endurance : les prompts de corps à corps du jeu et la garde de Wesker (parade RE7) "
                  "coûtent aussi de l'endurance (leurs curseurs de coût), et sont refusés (et cachés) tant que l'arc rouge se remplit (pas "
                  "l'agenouillement ni l'injection de Wesker). Chaque esquive du trainer (glissade, plongées et mouvements au "
                  "sol de RE6 compris) coûte les points de son curseur (Mêlée > Dodge ; tout à gauche, « blocage seul » : elle "
                  "arrête seulement le remplissage). Les esquives du jeu restent libres.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsurePromptPressHook()) return false;
              g_strict = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "player.stamina_pose_re6", .name = "Endurance : pose d'épuisement de RE6",
          .help = "Tant que l'arc rouge se remplit, au lieu de la pose blessée : le haut du corps prend la pose d'épuisement "
                  "de RE6 (à bout de souffle), à l'arrêt et en se déplaçant (au rythme de ses jambes). Les jambes et la "
                  "vitesse restent celles du jeu. Le fichier vient de tools/import_re6_motions.py.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureSetMotionHook()) LOG("stamina: no SetMotion hook, the game's own leg layer will flash");
              g_re6Pose = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Feature& sprint = Add({ .id = "player.stamina_sprint", .name = "Endurance : coût du sprint",
                            .help = "Cochée : le sprint du trainer retire ces points d'endurance par seconde. Décochée : "
                                    "il ne coûte rien. Dans les deux cas, l'endurance ne remonte pas pendant le sprint, et "
                                    "pas de sprint tant que l'arc rouge se remplit.",
                            .category = "Joueur", .kind = Kind::Toggle,
                            .set = [](bool on) {
                                g_sprintCostOn = on;
                                return true;
                            },
                            .available = [] { return game::IsSupported(); },
                            .value = 150, .valueMin = 10, .valueMax = 1000, .valueFormat = "%.0f pts/s" });
    Feature* const sp = &sprint;
    sprint.tick = [sp] { g_sprintCost.store(sp->value, std::memory_order_relaxed); };
    Feature& slowMotion = Add({ .id = "player.stamina_slow_motion", .name = "Endurance : le ralenti consomme",
                                .help = "Cochée : le ralenti de « Vitesse du temps » (curseur sous 1) retire ces points "
                                        "d'endurance par seconde, sans remplissage pendant ce temps ; il s'arrête tout seul "
                                        "à 25 % de l'endurance et ne peut pas être lancé en dessous.",
                                .category = "Joueur", .kind = Kind::Toggle,
                                .set = [](bool on) {
                                    g_slowCostOn = on;
                                    return true;
                                },
                                .available = [] { return game::IsSupported(); },
                                .value = 50, .valueMin = 5, .valueMax = 500, .valueFormat = "%.0f pts/s" });
    Feature* const sm = &slowMotion;
    slowMotion.tick = [sm] { g_slowCost.store(sm->value, std::memory_order_relaxed); };
    Feature& colour = Add({ .id = "player.stamina_color", .name = "Endurance : couleur",
                            .help = "Couleur de l'arc d'endurance (l'arc rouge de l'épuisement garde la sienne). Décochée : "
                                    "le vert de la jauge de vie.",
                            .category = "Affichage", .kind = Kind::Toggle,
                            .available = [] { return game::IsSupported(); },
                            .value = static_cast<float>(0x15920D), .color = true });
    Feature* const cp = &colour;
    colour.set = [cp](bool on) {
        g_colour = on ? static_cast<uint32_t>(cp->value) : 0x15920Du;
        return true;
    };
    colour.tick = [cp] { g_colour.store(static_cast<uint32_t>(cp->value), std::memory_order_relaxed); };
}
}  // namespace features
