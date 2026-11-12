// Move while aiming (user, 2026-10-15, like Nutelcio's "Enhanced Movement"): research prototype.
// The player plays its motions on 8 layers, records at unit + 0x380 + k*0xE0 (motions.log, layer dumps 2026-10-15):
// +0x04 motion (bank<<8|index), +0x0A mode, +0x0C blend frames, +0x14 weight, +0x18 frame, +0x1C length, +0x24
// speed, +0xD8 the per-joint slots (0x60 each, by joint index: +0x00 / +0x10 / +0x20 track pointers, +0x0C / +0x1C /
// +0x2C values; a joint without a track has them all 0, VA 0x59D690). Aim loop 0x1C: layer 0 stance 0x51A (weight
// ~0.9) + layers 1/2 turn variants 0x51B/0x519 (the rest of the weight), layers 3-5 the weapon's pitch poses (frame
// = pitch), 6 the hand, 7 (weight 1) the head / face. Walk 0x02 = layer 0 0x501 (+ 1/2 0x502/0x503), back 0x507.
// Actor SetMotion RVA 0x44CFF0 thiscall(layer, motion, float blend frames, float start frame, float speed, int mode).
// Skeleton: joints at [unit+0x318] (0x90 each: +5 parent index, +7 joint id), count [unit+0x314]; under the root
// (index 0) two branches: index 1 (upper body, ids 1...) and the joint id 0x0F (lower pelvis: both legs).
// Prototype: while player 1 aims (0x1C) or shoots (0x1E) and holds a direction button (0x00 forward, 0x01 back, 0x02
// left, 0x03 right), the walk plays on layer 7 with every joint but the lower-pelvis branch emptied (the aim keeps the
// upper body), and the player is moved along the aim yaw P+0x2A10 at the slider's speed; the game's collision still applies (user, 2026-10-20: no walking through walls or off
// ledges). Layer 7
// gets its own motion back when the direction is released or another action starts.
// Extended (user, 2026-10-20: Chris stopped during the aim start, and standing still to reload, switch weapons, heal,
// knife aim... is a pain): the player's actions are the cPlAction_pl_act_* classes in creation order, one less (DTI
// export: 0x1B weapon_ready_begin, 0x1C _loop, 0x1D _end, 0x1E weapon_fire, 0x1F weapon_reload, 0x20 weapon_change,
// 0x21 weapon_change_ready, 0x22 knife_ready_begin, 0x23 _loop, 0x24 _end, 0x25 / 0x26 knife from / to the weapon aim,
// 0x27 knife_attack; healing with an equipped herb: 0x5F then 0x60, recorded, health rises in 0x5F; 0x61 / 0x62 too). Their main motion is on layer 0; layer 7 only
// holds small ones (0x573 aim, 0x56E weapon change, 0x56F item use, 0x17E): the leg walk goes on it the same way.
// All of them strafe along Chris's yaw P+0x2A10 (equal to the unit quaternion +0x40 during actions, recorded). Moving
// relative to the camera and turning Chris toward the movement made him spin (user, 2026-10-20: the camera follows
// his yaw, so the goal turned with him); the user wants them like the aim.
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/hotkeys.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "hooks/hooks.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <cmath>
#include <cstring>
#include <vector>

namespace features {
bool GameButtonHeld(int button, int pad);  // quick_knife.cpp
void AimWalkGameStep();                     // below: the aim walk's step, in player 1's update
int Player1Pad();  // movement.cpp: the pad index player 1's buttons are read with
bool RawButtonHeld(int button, int pad);   // quick_knife.cpp
void ForceRunHeld(bool on);                // quick_knife.cpp
void ForceDirectionForward(bool on);       // quick_knife.cpp: any direction button reads as forward
bool EnsurePlayerRequestHook();            // action_log.cpp: its quick turn refused in the RE6 locomotion
bool NormalCameraInMelee();                // camera.cpp: "Garder la caméra normale au corps à corps"
bool RocketCatchPlaying();                 // rocket_catch.cpp: player 1 in the rocket catch
bool RocketSlashPlaying();                 // rocket_catch.cpp: player 1 in the knife slash that sent a rocket back
bool EnsurePadQueryHooks();                // quick_knife.cpp
bool Player1Dying();                      // dying.cpp
bool StaminaAllows();                      // stamina.cpp: the red arc not filling
bool CameraRay(float* eye, float& yaw, float& pitch);
bool QuickShotHoldsPlayer();               // quick_knife.cpp: no move during the RE6-style quick shot
bool Re6OnGround();                        // wesker_moves.cpp: RE6's ground (lying, ducked, sliding under the aim)
uint32_t Re6EvadeMotion(uint16_t slot);    // wesker_moves.cpp: a motion of re6_evade.lmt in player 1's lists
bool PlayerInWeskerMove(uintptr_t p);      // wesker_moves.cpp: player 1 in a trainer move's carrier action
uint16_t PlayerHeldItem(uintptr_t player);  // knife.cpp
float Player1TimeSpeed();                  // time_speed.cpp: the game's time speed for player 1 (real-time moves follow it)
bool WeskerGuardPlaying();                 // wesker_moves.cpp: the parry's guard (carrier action 0x81)
void Re6SprintChanged(uintptr_t p);        // re6_motions.cpp: the sprint started / stopped during the run
void QuickShotPadMask(uint32_t& held);     // quick_knife.cpp: the quick shots' aim / fire and the fire key on a pad
float WeskerGuardLegSpeed();               // wesker_moves.cpp: the leg layer's speed during the guard (1 / its factor)
bool Player1InventoryOpen();               // pickup.cpp: the real-time inventory open
void PadMouseWayStep(uintptr_t object);    // below: the pad's aim through the game's mouse aim (mask builder)
bool PadMouseWayNow();                     // below: on this frame (the left stick is hidden from the game then)
bool PadMouseWayWanted();                  // below
bool EnsureSetMotionHook();                // swaps.cpp: LowReadyLayerMotion asked there
bool Player1StaminaOutPose();              // stamina.cpp: the red arc filling with RE6's stamina-out pose chosen

bool MouseLaserNow();  // below: the mouse through the pad's aim this frame (the reload turn research)
namespace {
// The actions Chris may walk through (strafing, facing where he faces).
bool WalkAction(uint32_t action) {
    switch (action) {
    case 0x1B: case 0x1C: case 0x1D: case 0x1E:            // aim begin / loop / end, fire (grenade throws too)
    case 0x1F: case 0x20: case 0x21:                        // reload, weapon change
    case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:  // knife aim (and the quick knife)
    case 0x27:                                              // knife attack
    case 0x5F: case 0x60: case 0x61: case 0x62:             // healing (0x5F / 0x60 recorded: health rose in 0x5F)
        return true;
    case 0x81:  // the moves' carrier: only the parry's Wesker guard (user, 2026-10-23: not the knife slash or the throw)
        return WeskerGuardPlaying();
    default:
        return false;
    }
}

// Mouse turning where the game ignores the mouse (user, 2026-10-20: reload and weapon change kept Chris facing the
// same way). The raw mouse counts (hooks::TakeRawMouseX) turn his yaw P+0x2A10 and the unit quaternion +0x40 by the
// game's own ratio, learned while the game turns him itself in the aim (least squares of yaw change on counts).
bool MouseIgnored(uint32_t action) { return action == 0x1F || action == 0x20 || action == 0x21 || (action >= 0x5F && action <= 0x62); }

double g_sxy = 0.0, g_sxx = 0.0;
// "Déplacement selon la direction (RE6)": the camera kept where it looked through a reload, weapon change, heal or dodge
// (render thread). Through a reload, weapon change or heal (MouseIgnored) he moves as in the RE6 mode (user, 2026-10-25:
// "I strafe as in the aim, not the free movement I asked for", the same for weapon changes and heals): walking turns him
// to his direction (TickAimWalk) and the mouse / right stick turn the camera alone.
bool g_locoKeeping = false;
std::atomic<float> g_locoTurnSpeed{ 720.0f };  // degrees per second ("Déplacement selon la direction (RE6)")
void SetBodyYaw(uintptr_t p, float yaw);  // below: the body only (quaternion, P+0x1024)
long g_cameraDx = 0;  // raw mouse counts TickMouseTurn took, for the RE6 locomotion's camera (render thread)
bool g_boatCamera = false;  // TickBoatCamera: the airboat's driver turns the camera (render thread; takes the mouse)
float g_prevYaw = 0.0f;
bool g_havePrev = false;

float Wrap(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// Chris's facing is the unit quaternion +0x40 (P+0x1024 holds the same angle). P+0x2A10 is the mouse's aim yaw: with a
// pad it stays where it was (recorded 2026-10-20: 0.00 in a pad aim that turned him 0.17 rad, -0.20 in a run at
// +0.04). Turning from it snapped Chris back to that old angle (user: "il se recentre dans une direction").
float QuatYaw(uintptr_t p) {
    float q[4]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x40), q, sizeof(q))) return 0.0f;
    return Wrap(2.0f * std::atan2(q[1], q[3]));
}

// On a moving platform (3-3's boat, the mine cart before the caves: his parent [P+0x18] carries him) the quaternion +0x40
// and P+0x2A10 are relative to it, P+0x1024 and the aim camera's yaw P+0x2A24 are the world's (recorded on the boat
// without the trainer's camera options, 2026-10-29: the aim start copies P+0x1024 into P+0x2A24). The trainer wrote the
// platform's angle into both: the aim turned him by the boat's heading ("a half turn at each aim", user). The parent's
// share: the facing (Z row) of his world matrix (+0xA0..+0xD0) minus his local matrix's (+0x60..+0x90); 0 on the ground.
float ParentYaw(uintptr_t p) {
    float world[3]{}, local[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0xC0), world, sizeof(world)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p + 0x80), local, sizeof(local)))
        return 0.0f;
    const float w = std::atan2(world[0], world[2]), l = std::atan2(local[0], local[2]);
    return w == w && l == l ? Wrap(w - l) : 0.0f;
}

// The aim camera's yaw for a platform-relative `yaw`.
void WriteAimYaw(uintptr_t p, float yaw) { mem::Write(p + 0x2A24, Wrap(yaw + ParentYaw(p))); }

void SetYaw(uintptr_t p, float yaw) {
    yaw = Wrap(yaw);
    mem::Write(p + 0x2A10, yaw);
    mem::Write(p + 0x1024, Wrap(yaw + ParentYaw(p)));
    const float quat[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x40), quat, sizeof(quat));
}

float TurnPerCount() {
    if (g_sxx < 2000.0) return -0.0025f;  // not learned yet: about 140 degrees per inch at 800 dpi, right = clockwise
    return static_cast<float>(g_sxy / g_sxx);
}

// Research (user, 2026-10-21: twice the mouse turned Chris extremely slowly for a few seconds during a reload): the
// ratio used on entering an ignored action, and per ignored action the counts received and the turn given.
uint32_t g_turnAction = 0;
long g_turnCounts = 0;
float g_turnGiven = 0.0f, g_turnStartYaw = 0.0f;
DWORD g_turnSince = 0;

void LogMouseTurn(uintptr_t p, uint32_t action, long dx, float delta) {
    if (MouseIgnored(action)) {
        if (action != g_turnAction) {
            g_turnAction = action;
            g_turnCounts = 0;
            g_turnGiven = 0.0f;
            g_turnStartYaw = QuatYaw(p);
            g_turnSince = GetTickCount();
            LOG("mouse turn: action %X, ratio %.5f per count (learned on %.0f, sxy %.4f)", action, TurnPerCount(), g_sxx, g_sxy);
        }
        g_turnCounts += dx;
        g_turnGiven += delta;
        return;
    }
    if (g_turnAction)
        LOG("mouse turn: action %X over after %u ms: %ld counts, turn given %.3f rad, body turned %.3f rad",
            g_turnAction, GetTickCount() - g_turnSince, g_turnCounts, g_turnGiven, Wrap(QuatYaw(p) - g_turnStartYaw));
    g_turnAction = 0;
}

// Research (user, 2026-11-10 23:24: "a strange camera snap at the reload", "it did it several times"; logged: the body turned by
// 130-150 degrees more than the mouse gave in some reloads): Chris's body yaw turned by someone else than this tick since its last
// frame (over 3 degrees), in the actions whose mouse turn is the trainer's and on the frames entering / leaving them, with the yaws
// the game keeps: P+0x2A10 the mouse's, P+0x2A24 the aim's, P+0x1024 the world's, input +0x4B4 the aim's free zone.
uintptr_t InputBlock(uintptr_t p);  // below
float g_reloadBodyAfter = 0.0f;
uint32_t g_reloadPrevAction = 0;
bool g_reloadHave = false;
void LogReloadTurn(uintptr_t p, uint32_t action) {
    const float body = QuatYaw(p);
    const bool ignored = MouseIgnored(action), edge = g_reloadHave && action != g_reloadPrevAction &&
                                                      (ignored || MouseIgnored(g_reloadPrevAction));
    const float moved = g_reloadHave ? Wrap(body - g_reloadBodyAfter) : 0.0f;
    if (edge || (ignored && std::fabs(moved) > 0.05f)) {
        float mouseYaw = 0.0f, aimYaw = 0.0f, world = 0.0f, zone = 0.0f;
        mem::Read(p + 0x2A10, mouseYaw);
        mem::Read(p + 0x2A24, aimYaw);
        mem::Read(p + 0x1024, world);
        if (const uintptr_t input = InputBlock(p)) mem::Read(input + 0x4B4, zone);
        constexpr float kDeg = 57.2957795f;
        LOG("reload turn research: action %X (was %X): body %.0f deg, turned %+.1f by another since the last frame; mouse yaw %.0f, "
            "aim yaw %.0f, world %.0f, zone %.1f; RE6 keeping %d, mouse aim through the pad's %d",
            action, g_reloadPrevAction, body * kDeg, moved * kDeg, mouseYaw * kDeg, aimYaw * kDeg, world * kDeg, zone,
            g_locoKeeping ? 1 : 0, MouseLaserNow() ? 1 : 0);
    }
    g_reloadPrevAction = action;
}

void TickMouseTurn(uintptr_t p, uint32_t action) {
    LogReloadTurn(p, action);
    struct AfterTurn {
        uintptr_t p;
        ~AfterTurn() {
            g_reloadBodyAfter = QuatYaw(p);
            g_reloadHave = true;
        }
    } after{ p };
    const long dx = hooks::TakeRawMouseX();
    g_cameraDx += dx;
    float yaw = 0.0f;
    if (!mem::Read(p + 0x2A10, yaw)) return;
    LogMouseTurn(p, action, dx, MouseIgnored(action) ? TurnPerCount() * static_cast<float>(dx) : 0.0f);
    // Learned only from frames where the aim yaw moved: inside the aim's free zone the crosshair moves and the body
    // does not, and those frames dragged the ratio to a third (user, 2026-10-21: the reload turned extremely slowly;
    // logged -0.0007 per count instead of about -0.0025). Not during the RE6 quick shot, which turns Chris itself.
    const double dyaw = g_havePrev ? Wrap(yaw - g_prevYaw) : 0.0;
    if ((action == 0x1C || action == 0x1E) && g_havePrev && dx != 0 && std::fabs(dyaw) > 1e-5 && !QuickShotHoldsPlayer()) {
        g_sxy = g_sxy * 0.995 + dyaw * dx;
        g_sxx = g_sxx * 0.995 + static_cast<double>(dx) * dx;
    } else if (MouseIgnored(action) && dx != 0) {
        const float delta = TurnPerCount() * static_cast<float>(dx);
        yaw = Wrap(QuatYaw(p) + delta);
        if (g_locoKeeping) {  // RE6 mode: the camera alone (his walking turns him)
            float camera = 0.0f;
            if (mem::Read(p + 0x2A10, camera)) mem::Write(p + 0x2A10, Wrap(camera + delta));
            yaw = QuatYaw(p);
        } else {
            SetYaw(p, yaw);
            // The aim camera (camera mode byte +0x1B1, aim and aim + reload) follows the aim yaw P+0x2A24 (body + free
            // zone offset, recorded 2026-10-20: 0.999 with the camera), not the body: turned by the same amount (user:
            // reloading while aiming turned Chris but not the camera).
            float aimYaw = 0.0f;
            if (mem::Read(p + 0x2A24, aimYaw)) mem::Write(p + 0x2A24, Wrap(aimYaw + delta));
        }
    }
    g_prevYaw = yaw;
    g_havePrev = true;
}

// Side steps (user, 2026-10-20: strafing played the forward walk): walk_l 0x03 = 0x50C, walk_r 0x04 = 0x50D (motions.log).
constexpr uint32_t kWalk = 0x501, kWalkBack = 0x507, kWalkLeft = 0x50C, kWalkRight = 0x50D;
constexpr uint32_t kLegLayer = 7;
constexpr uint8_t kLowerPelvisId = 0x0F;

std::atomic<float> g_speed{ 120.0f };  // units (cm) per second

// Running (user, 2026-10-20): the run button (game button 0x04) held while moving forward plays the run 0x504 (layer
// 0 of run 0x05) on the legs at the run speed (the game's run: ~426 units/s, walk ~172, recorded). One option for the
// actions out of aim (reload, weapon change, healing...: with an aim button held Chris still walks), another for the
// aim itself (aim start / loop, fire, knife). The game has no side / back run: those directions keep walking.
constexpr uint32_t kRun = 0x504;
std::atomic<bool> g_actionRun{ false }, g_aimRun{ false };
std::atomic<float> g_runSpeed{ 426.0f };

bool AimAction(uint32_t action) {
    switch (action) {
    case 0x1B: case 0x1C: case 0x1E: case 0x22: case 0x23: case 0x25: case 0x26: case 0x27:
        return true;
    default:
        return false;
    }
}

// Run toggle and sprint (user, 2026-10-20). Toggle: a short press of the run button starts running, another one stops
// it; meanwhile the run button reads as held (ForceRunHeld, pad query 3). Sprint (with the toggle): holding the run
// button longer than 250 ms sprints until it is released (the run goes on): the plain run 0x05 gets the unit time
// scale +0xFD4 (1.0, as the chainsaw speed) times the factor (faster legs, faster root motion), the runs during
// actions their speed times the factor. +0x3A4 (player speed) is left to "Vitesse - Joueur 1", which writes it.
// The animation is the plain run until a RE6 sprint motion is imported.
std::atomic<bool> g_toggleRun{ false }, g_sprintOn{ false };
std::atomic<float> g_sprintFactor{ 3.0f };
bool g_running = false, g_sprinting = false;
// The run press the RE6 locomotion took for its quick turn (back + run): it neither stops nor starts the run toggle
// (user, 2026-10-24: running toward the camera, the quick turn's tap walked him, then "back" walked him back).
bool g_runPressQuickTurn = false;
float g_savedTimeScale = 1.0f;
uintptr_t g_scaledPlayer = 0;
bool g_walkOn = false;  // "Marcher pendant les actions" enabled

// Gamepad (user, 2026-10-20: walking during actions and the run toggle only read the keyboard). With a pad the player
// reads the left stick from its input block (input = [P+0x2D64]+0x220: +0x410 x, right = +1, +0x414 y, forward = +1,
// recorded, also while aiming) and the run button only through the held mask input+0x51C (bit 0x4000; the pad never
// goes through the button queries), built every frame by VA 0xC6E150 (thiscall, ecx = input+0x4E8, mask [ecx+0x34])
// from the raw pad mask. Its call at VA 0xB8A6FE is redirected: the real run bit is read there and set while the run
// toggle runs. "Pad in use" = VA 0x799AF0 (sBioPad [RVA 0xE49C40], the player's pad P+0x2D9C).
bool PadInUse(uintptr_t p) {
    uint8_t pad = 0;
    uintptr_t bioPad = 0;
    if (!p || !mem::Read(p + 0x2D9C, pad) || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE49C40)), bioPad) || !bioPad)
        return false;
    using PadInUseFn = bool(__fastcall*)(void* self, void* edx, int pad);
    __try {
        return reinterpret_cast<PadInUseFn>(mem::Rva(0x399AF0))(reinterpret_cast<void*>(bioPad), nullptr, pad);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CameraYaw(float& yaw) {
    float eye[3]{}, pitch = 0;
    return CameraRay(eye, yaw, pitch);
}

uintptr_t InputBlock(uintptr_t p) {
    uintptr_t block = 0;
    return p && mem::Read(p + 0x2D64, block) && block ? block + 0x220 : 0;
}

std::atomic<bool> g_padRunReal{ false };
std::atomic<uint32_t> g_heldMask{ 0 };  // player 1's held mask as the game built it (before the run toggle's bit)

// Mouse-like pad aim, part of "Réticule à la manette" (user, 2026-10-20: no box of its own). Recorded: in a pad aim the
// right stick's horizontal part moves the weapon inside a free zone (offset input+0x450, degrees, copied to P+0x1954)
// and the camera only turns at its edge, so the centred crosshair was off the aim; up / down aims at once. Tried and
// dropped: turning Chris from the trainer (the camera lagged, the crosshair never turned green) and feeding the stick
// to the mouse code (input+0x6E8.., readers VA 0xB8AD71 / 0x845905 behind keyboard tests: left / right stopped
// moving). The zone's half width is the game parameter [RVA 0xDB27B4]+0x2AC (6.93), read at one place, VA 0xB87A27,
// which already uses 0 for one weapon kind (+0x24 == 3): it is 0 while a pad is in use.
std::atomic<bool> g_padMouseAim{ false };
float g_savedZone = 6.93f;
bool g_zoneZeroed = false;

void SetZoneZero(bool zero) {
    if (zero == g_zoneZeroed) return;
    uintptr_t params = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB27B4)), params) || !params) return;
    if (zero) {
        float zone = 0.0f;
        if (mem::Read(params + 0x2AC, zone) && zone > 0.5f) g_savedZone = zone;
        mem::Write(params + 0x2AC, 0.0f);
    } else {
        mem::Write(params + 0x2AC, g_savedZone);
    }
    g_zoneZeroed = zero;
}

bool Re6RunAnyDirection();  // below: the RE6 locomotion's own run

// With the run toggle on a pad, run only while the left stick points forward (user, 2026-10-20: the game's pad run
// steers with the left stick, tank-like, and ignores the right stick): sideways / backwards Chris walks and strafes
// as on the keyboard. The stick of the previous frame (input +0x410 x, +0x414 y, relative to the camera). Not when
// the RE6 locomotion runs (user, 2026-10-26: back with the toggle, "the camera bugs and Chris does not know whether to
// turn round or run backwards"): its run goes every way, and this test, which reads the stick it forces, flipped it
// every frame with the game's walk (trainer.log: "on (action 8)" / "speed 0 left to the game").
bool StickRunsForward(uintptr_t input) {
    if (!PadInUse(actors::Player(0))) return true;  // keyboard: the run key's own behaviour
    float stick[2]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(input + 0x410), stick, sizeof(stick))) return false;
    if (Re6RunAnyDirection()) return std::hypot(stick[0], stick[1]) > 0.3f;
    return stick[1] > 0.3f && stick[1] >= 0.7f * std::fabs(stick[0]);
}
void* g_maskBuild = nullptr;  // VA 0xC6E150
mem::Patch* g_maskCall = nullptr;

uint32_t g_shotAdded = 0;  // bits the quick shots / fire key added to the previous frame's mask

void __cdecl AfterMaskBuild(uintptr_t object) {
    const uintptr_t input = InputBlock(actors::Player(0));
    uint32_t held = 0;
    if (!input || object != input + 0x4E8 || !mem::Read(object + 0x34, held)) return;
    AimWalkGameStep();
    g_padRunReal = (held & 0x4000) != 0;
    PadMouseWayStep(object);
    // RT kept from the game out of aim (quick_knife.cpp: "RT ne localise plus le partenaire", "Couteau rapide (touche)")
    // is still the fire for the trainer's readers (user, 2026-11-06: "Tir sans viser" and the RE6 parry on the pad).
    // RT pressed by "Localiser le partenaire (touche)" is the game's only (user, 2026-11-07: "Tir sans viser" fired with
    // it): its fire bits off for the trainer's readers, unless RT is really held.
    const uint32_t seen = (hooks::PadInjectedButtons() & 0x20000) && !hotkeys::Held(hotkeys::kPadRT) ? held & ~0x8100u : held;
    g_heldMask = seen | ((hooks::PadHiddenButtonsHeld() & 0x20000) ? 0x8100u : 0u);
    // Zero free zone tried and dropped (user, 2026-10-20: the pad camera still lags behind Chris, up / down too, and the
    // crosshair never turns green; tried again 2026-11-06 with the camera's basis on the body: Chris never trailed the
    // camera as with the mouse): the zone stays the game's. SetZoneZero is kept for later research.
    SetZoneZero(false);
    uint32_t out = held;
    // Not in the inventory (its buttons there are the grid's).
    if (g_running && g_toggleRun.load(std::memory_order_relaxed) && StickRunsForward(input) && !Player1InventoryOpen())
        out |= 0x4000;
    // The quick shots and the fire key on a pad (user, 2026-10-23: they did nothing there; a pad's buttons only reach
    // the game through this mask: aim 0x800, fire 0x8000 | 0x100, knife aim 0x400, recorded). A bit they add is also a
    // new press for the game: its trigger mask (+0x3C) was built from the real buttons.
    const uint32_t before = out;
    QuickShotPadMask(out);
    const uint32_t added = out & ~before;
    if (const uint32_t pressed = added & ~g_shotAdded) {
        uint32_t trigger = 0;
        if (mem::Read(object + 0x3C, trigger)) mem::Write<uint32_t>(object + 0x3C, trigger | pressed);
    }
    g_shotAdded = added;
    if (out != held) mem::Write<uint32_t>(object + 0x34, out);
}

__declspec(naked) void MaskBuildStub() {
    __asm {
        push ecx                    // the mask object, kept for AfterMaskBuild
        call dword ptr [g_maskBuild]
        call AfterMaskBuild
        add esp, 4
        ret
    }
}

bool EnsureMaskHook() {
    if (g_maskCall) return g_maskCall->IsApplied() || g_maskCall->Apply();
    uint8_t* site = mem::Rva(0x78A6FE);
    int32_t rel = 0;
    if (site[0] != 0xE8 || !mem::Read(reinterpret_cast<uintptr_t>(site + 1), rel) || site + 5 + rel != mem::Rva(0x86E150)) {
        LOG("pad mask: unexpected call %s", mem::Hex(site, 5).c_str());
        return false;
    }
    g_maskBuild = mem::Rva(0x86E150);
    const int32_t stub = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&MaskBuildStub) - reinterpret_cast<uintptr_t>(site + 5));
    std::vector<uint8_t> original(site, site + 5), patched = { 0xE8 };
    for (int i = 0; i < 4; ++i) patched.push_back(static_cast<uint8_t>(stub >> (8 * i)));
    g_maskCall = new mem::Patch("pad mask", site, original, patched);
    return g_maskCall->Apply();
}

// Taken back once neither option needs it.
std::atomic<bool> g_parryMask{ false };      // the parry reads the pad's held mask
std::atomic<bool> g_quickShotMask{ false };  // the quick shots and the fire key write it
std::atomic<bool> g_groundRollMask{ false };  // the ground roll reads the pad's real run bit

void ReleaseMaskHook() {
    if (g_maskCall && !g_walkOn && !g_toggleRun.load(std::memory_order_relaxed) &&
        !g_padMouseAim.load(std::memory_order_relaxed) && !g_parryMask.load(std::memory_order_relaxed) &&
        !g_quickShotMask.load(std::memory_order_relaxed) && !g_groundRollMask.load(std::memory_order_relaxed) &&
        !PadMouseWayWanted())
        g_maskCall->Restore();
}

// The base is always 1.0 (the uWork constructor's value, never changed by the game): reading it back took another
// option's raised scale for the base (user, 2026-10-20: a pickup during a sprint left Chris at x1.8, and sprints at
// x1.8 x the factor).
void SetSprintScale(uintptr_t p, bool on) {
    if (on && g_scaledPlayer != p) {
        g_savedTimeScale = 1.0f;
        g_scaledPlayer = p;
    }
    if (on) {
        mem::Write(p + 0xFD4, g_savedTimeScale * g_sprintFactor.load(std::memory_order_relaxed));
    } else if (g_scaledPlayer) {
        if (g_scaledPlayer == actors::Player(0)) mem::Write(g_scaledPlayer + 0xFD4, g_savedTimeScale);
        g_scaledPlayer = 0;
    }
}

void StopToggleRun() {
    g_running = false;
    g_sprinting = false;
    ForceRunHeld(false);
    SetSprintScale(0, false);
}

// The right stick turns Chris during the run (0x05) on a pad, as the mouse does on the keyboard: the game ignores it
// there. Recorded (2026-10-20): the right stick (input +0x420 x, right = +1) turns the pad camera by about -2.75 rad/s
// at full tilt; the same rate on Chris's yaw P+0x2A10 and the unit quaternion +0x40, the camera follows him.
void TickPadRunTurn(uintptr_t p, uint32_t action) {
    static LARGE_INTEGER last{};
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    dt *= Player1TimeSpeed();
    const uintptr_t input = InputBlock(p);
    float rx = 0.0f;
    if (action != 0x05 || !input || !PadInUse(p) || !mem::Read(input + 0x420, rx) || std::fabs(rx) < 0.2f) return;
    SetYaw(p, QuatYaw(p) - 2.75f * rx * dt);
}

void TickToggleRun() {
    static bool was = false, toggledOn = false, sprinted = false;
    static DWORD start = 0;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action)) {
        StopToggleRun();
        was = false;
        return;
    }
    const bool key = PadInUse(p) ? g_padRunReal.load(std::memory_order_relaxed)
                                 : RawButtonHeld(0x04, -1) || RawButtonHeld(0x04, Player1Pad());
    const DWORD now = GetTickCount();
    if (key && !was) {
        start = now;
        sprinted = false;
        toggledOn = !g_running;
        g_running = true;
    }    // Only the plain run sprints: not during actions (the scale below is for the run 0x05 alone) nor while dying (user).
    const bool wasSprinting = g_sprinting;
    // No sprint while the stamina's red arc fills (user, 2026-10-26: "cut the sprint when the stamina is empty").
    g_sprinting = key && g_sprintOn.load(std::memory_order_relaxed) && now - start >= 250 && !Player1Dying() &&
                  StaminaAllows();
    if (g_sprinting) sprinted = true;
    if (g_sprinting != wasSprinting && action == 0x05) Re6SprintChanged(p);  // re6_motions.cpp: the sprint motion now
    // The quick turn's press while walking does not turn the run on either (user, 2026-10-24).
    if (key && g_runPressQuickTurn && toggledOn) {
        g_running = false;
        toggledOn = false;
    }
    if (!key && was && !sprinted && !toggledOn && !g_runPressQuickTurn) g_running = false;  // a short press while running stops it
    if (!key) g_runPressQuickTurn = false;
    // The run button is also the one letting go of the objects held through an event (user, 2026-10-30: the laser
    // mirror's grab, uOmLaserMirror, was cancelled as soon as its cancel prompt showed): the toggled run ends there.
    if (action == 0x80000000 && g_running) {
        g_running = false;
        g_sprinting = false;
    }
    was = key;
    ForceRunHeld(g_running);
    SetSprintScale(p, g_sprinting && action == 0x05);
    TickPadRunTurn(p, action);
}

bool RunAllowed(uint32_t action) {
    const bool aiming = AimAction(action) || GameButtonHeld(0x0C, Player1Pad()) || GameButtonHeld(0x0D, Player1Pad());
    if (aiming ? !g_aimRun.load(std::memory_order_relaxed) : !g_actionRun.load(std::memory_order_relaxed)) return false;
    if (PadInUse(actors::Player(0)))
        return g_padRunReal.load(std::memory_order_relaxed) || (g_running && g_toggleRun.load(std::memory_order_relaxed));
    return GameButtonHeld(0x04, Player1Pad()) || GameButtonHeld(0x04, -1);
}
bool g_moving = false;
uint32_t g_legMotion = 0;     // walk motion we put on layer 7
uint32_t g_legMirror = 0;     // the mode bit 0x100 the legs were set with (a left-handed player)
uint32_t g_savedMotion = 0;   // layer 7's own motion and mode
uint16_t g_savedMode = 0;
LARGE_INTEGER g_last{};
// The aim walk's move on the game thread (user, 2026-11-04: in the first person without the camera's easing, "it trembles
// walking while aiming"; recorded, research/fp_turns/jitter.txt: the eye against Chris jumped 1-1.5 cm on single
// frames): TickAimWalk runs with the overlay, out of step with the game's frames, so some frames got two steps and some
// none. The step is taken where the game takes its own walks' (user: "can't you let him walk normally?"): in player 1's
// update, when his pad mask is built (AfterMaskBuild, before his motions and his skeleton), by the game's frame time
// (sFps +0x24). Taken in the camera's view copy first (after his skeleton, by the clock's time): the body drawn and the
// eye were a step of irregular length apart, the hands jumped sideways in the first person (burst pictures, 2026-11-04).
// While it runs TickAimWalk only sets the velocity; without it, TickAimWalk moves him itself as before.
std::atomic<float> g_walkVx{ 0.0f }, g_walkVz{ 0.0f };  // cm a second
std::atomic<uintptr_t> g_walkPlayer{ 0 };
std::atomic<ULONGLONG> g_walkVelocityTick{ 0 }, g_walkGameStepTick{ 0 };

bool WalkStepsOnGameThread() {
    const ULONGLONG t = g_walkGameStepTick.load(std::memory_order_relaxed);
    return t && GetTickCount64() - t < 200;
}

void StopWalkVelocity() {
    g_walkVx = 0.0f;
    g_walkVz = 0.0f;
    g_walkVelocityTick = GetTickCount64();
}

uintptr_t LayerRecord(uintptr_t unit, uint32_t layer) { return unit + 0x380 + layer * 0xE0; }

// The native walk (user, 2026-11-04: "rather than moving him by hand, let him walk with the game's own movement"): the
// motion update (VA 0x59EAA0) moves a unit by its layers' root motion - each layer whose mode lacks bit 0x01 blends its
// root move into the sum by its weight (layer +0x14 x the position track's value +0xAC: track +0xA0; the turn: +0xB0
// / +0xBC), then P+0x30 += the sum (VA 0x5A041A) before the skeleton is made. The leg layer's position track is kept
// (it was emptied on 2026-10-15, when the forward walk played every direction and pushed him forward when strafing):
// the walks, side steps, back walk and run move him themselves, their turn track stays empty (the aim turns him).
// Off (2026-11-04): tried for the first person's tremble, which it did not change (the camera's cause: camera.cpp
// g_firstPersonFinalCtrl); the user asked for the trainer's own move back. Moved 162 cm a second sideways (0x50C).
// Kept for the diagonals (two leg layers' root moves blended by their weights) and the speed sliders (the layer's speed).
std::atomic<bool> g_nativeWalk{ false };

bool SetMotion(uintptr_t unit, uint32_t layer, uint32_t motion, float blend, uint32_t mode, float start = 0.0f) {
    using SetMotionFn = uint32_t(__fastcall*)(void* self, void* edx, uint32_t layer, uint32_t motion, float blend, float start,
                                              float speed, uint32_t mode);
    __try {
        reinterpret_cast<SetMotionFn>(mem::Rva(0x44CFF0))(reinterpret_cast<void*>(unit), nullptr, layer, motion, blend, start,
                                                         1.0f, mode);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Joint indices of the lower-pelvis branch (legs), per skeleton.
std::vector<uint8_t> g_isLeg;
uintptr_t g_skeletonOf = 0;

// RE6's ground poses (Re6GroundLegs): the hips (bone 0, their height) with the legs on the leg layer, the aim's upper body kept.
bool g_groundLegs = false;
bool g_groundFull = false;  // the whole body from the layer (RE6's aiming poses), not the hips and legs only
bool g_groundRoot = false;  // the layer moves him by its own root (RE6's slide and dives under the aim)
// "Pose basse au pistolet (RE6)": the leg layer holds RE6's upper body over the game's own legs (MaskLegLayer keeps all but the hips and legs).
bool g_upperOverlay = false;
// quick_knife.cpp's RE6 quick shots: a whole-body motion on the leg layer (PlayShotLayer), its root's translation moving him.
bool g_shotLayer = false;
float g_shotLastFrame = 0.0f;  // its frame last seen (the keeper sets it again from there, never from 0)
std::vector<uint8_t> g_isGround;

bool LegJoints(uintptr_t p) {
    uintptr_t joints = 0;
    uint32_t count = 0;
    if (!mem::Read(p + 0x318, joints) || !joints || !mem::Read(p + 0x314, count) || count == 0 || count > 255) return false;
    if (g_skeletonOf == joints && g_isLeg.size() == count) return true;
    std::vector<uint8_t> parent(count), id(count);
    for (uint32_t i = 0; i < count; ++i)
        if (!mem::Read(joints + i * 0x90 + 5, parent[i]) || !mem::Read(joints + i * 0x90 + 7, id[i])) return false;
    g_isLeg.assign(count, 0);
    g_isGround.assign(count, 0);
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = i, guard = 0; j < count && guard < 64; j = parent[j], ++guard)
            if (id[j] == kLowerPelvisId) {
                g_isLeg[i] = 1;
                break;
            }
    for (uint32_t i = 0; i < count; ++i) g_isGround[i] = g_isLeg[i] || id[i] == 0;
    g_skeletonOf = joints;
    return true;
}

// Empties layer 7's slots for every joint outside the legs (as the game does for a joint without a track) and its root
// motion, once its motion is set up (the "changed" bit 2 of +0x08 is cleared by the update that fills the slots).
void MaskLegLayer(uintptr_t p) {
    const uintptr_t record = LayerRecord(p, kLegLayer);
    uint16_t motion = 0, flags = 0;
    uintptr_t slots = 0;
    if (!mem::Read(record + 4, motion) || motion != g_legMotion || !mem::Read(record + 8, flags) || (flags & 2) ||
        !mem::Read(record + 0xD8, slots) || !slots || !LegJoints(p))
        return;
    for (size_t i = 0; i < g_isLeg.size(); ++i) {
        if (g_groundLegs ? g_groundFull || g_isGround[i] : g_upperOverlay ? !g_isGround[i] : g_isLeg[i]) continue;
        const uintptr_t slot = slots + i * 0x60;
        for (uintptr_t off : { 0x00u, 0x10u, 0x20u }) {
            mem::Write<uint32_t>(slot + off, 0);
            mem::Write(slot + off + 0xC, 0.0f);
        }
    }
    // Its root motion too (user, 2026-10-15: moving sideways went diagonally, the walk pushed forward): the layer's own
    // track types 3 / 4 (record +0xB0 / +0xA0, track pointer and value +0xC), filled by the same setup VA 0x59D8DF.
    for (uintptr_t off : { 0xA0u, 0xB0u }) {
        if (off == 0xA0u && !(g_upperOverlay && !g_groundLegs) &&
            (g_nativeWalk.load(std::memory_order_relaxed) || (g_groundLegs && g_groundRoot)))
            continue;  // his moves (see g_nativeWalk); never the low ready's
        mem::Write<uint32_t>(record + off, 0);
        mem::Write(record + off + 0xC, 0.0f);
    }
}

// The mask goes on in the very update that sets the layer up (user, 2026-10-15: a direction change flashed the walk on
// the whole body for a frame or two, the mask coming one frame late): the layer setup VA 0x59D760 (eax = model, ecx =
// motion, stack = layer, ret 4) has one caller, the motion update (call at VA 0x59D6E4); that call is redirected to
// LayerSetupStub, which calls it and then masks player 1's leg layer.
void* g_layerSetup = nullptr;  // VA 0x59D760

bool g_turnSteps = false;  // game thread: the first person's turn steps own the leg layer (see TurnSteps)

void __cdecl AfterLayerSetup(uintptr_t unit, uint32_t layer) {
    if ((g_moving || g_turnSteps) && layer == kLegLayer && unit == actors::Player(0)) MaskLegLayer(unit);
}

__declspec(naked) void LayerSetupStub() {
    __asm {
        push eax                    // the model
        push dword ptr [esp + 8]    // the layer (return address, then ours)
        call dword ptr [g_layerSetup]
        pop eax
        push dword ptr [esp + 4]    // the layer
        push eax
        call AfterLayerSetup
        add esp, 8
        ret 4
    }
}

mem::Patch* g_setupCall = nullptr;

bool EnsureLayerSetupHook() {
    if (g_setupCall) return g_setupCall->IsApplied() || g_setupCall->Apply();
    uint8_t* site = mem::Rva(0x19D6E4);
    int32_t rel = 0;
    if (site[0] != 0xE8 || !mem::Read(reinterpret_cast<uintptr_t>(site + 1), rel) ||
        site + 5 + rel != mem::Rva(0x19D760)) {
        LOG("aim walk: unexpected layer setup call %s", mem::Hex(site, 5).c_str());
        return false;
    }
    g_layerSetup = mem::Rva(0x19D760);
    const int32_t stub = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&LayerSetupStub) - reinterpret_cast<uintptr_t>(site + 5));
    std::vector<uint8_t> original(site, site + 5), patched = { 0xE8 };
    for (int i = 0; i < 4; ++i) patched.push_back(static_cast<uint8_t>(stub >> (8 * i)));
    g_setupCall = new mem::Patch("aim walk layer setup", site, original, patched);
    return g_setupCall->Apply();
}

void RestoreLegLayer(uintptr_t p) {
    if (!g_moving) return;
    g_moving = false;
    g_upperOverlay = false;
    uint16_t motion = 0;
    if (p && mem::Read(LayerRecord(p, kLegLayer) + 4, motion) && motion == g_legMotion)
        SetMotion(p, kLegLayer, g_savedMotion, 10.0f, g_savedMode);
    g_legMotion = 0;
}

// The pad's right stick where the game ignores it (user, 2026-10-24: during a reload the camera could not turn on a
// pad, the mouse did): the same actions as the mouse (MouseIgnored), Chris turned at the game's pad turn speed (2.75
// rad/s at full tilt, recorded), the aim yaw P+0x2A24 too (reload while aiming: the aim camera follows it).
void TickPadIgnoredTurn(uintptr_t p, uint32_t action, float dt) {
    if (!MouseIgnored(action) || !PadInUse(p)) return;
    const float rx = hooks::PadRightX();
    if (rx == 0.0f) return;
    const float delta = -2.75f * rx * dt;
    float camera = 0.0f;
    if (g_locoKeeping) {  // RE6 mode: the camera alone (his walking turns him)
        if (mem::Read(p + 0x2A10, camera)) mem::Write(p + 0x2A10, Wrap(camera + delta));
        return;
    }
    SetYaw(p, QuatYaw(p) + delta);
    float aimYaw = 0.0f;
    if (mem::Read(p + 0x2A24, aimYaw)) mem::Write(p + 0x2A24, Wrap(aimYaw + delta));
}

// "Pose basse au pistolet (RE6)" (user, 2026-11-09: "Leon's pose with the handgun to give to Chris: low ready in RE6, high ready for Chris in
// RE5"; "RE6 has no strafe: keep the original legs and pass only the upper body"): out of the aim with a handgun (idle, walking, running:
// actions up to 0x09, not sprinting, not on RE6's ground nor in a trainer move), RE6's HgA 01 (re6_evade.lmt 59, looped) on the leg
// layer for everything but the hips and legs, mirrored for a left-handed player; true while it holds the layer. Called by TickAimWalk
// ("Marcher pendant les actions" on), else every frame by TickLowReadyAlone.
// The same layer plays RE6's stamina-out pose while the red arc fills ("Endurance : pose d'épuisement de RE6", stamina.cpp; user, 2026-11-09:
// "not the injured pose, the lack of stamina pose"), whatever the weapon: re6_evade.lmt 60 (InA 01) standing, 61 (InA 37, RE6's jog's upper
// body) moving, in step with his legs: its frame from layer 0's share of its cycle + kTiredPhase (each foot most forward: RE6's BhA 36
// toe 23 at frame 10 / 50, toe 19 at 38; RE5's walk pl00wp06 01 at 53 / 60 and 23, its run 04 at 40 / 45 and 18: 0.31-0.38 later in RE6).
std::atomic<bool> g_lowReadyOn{ false };
constexpr uint16_t kLowReadySlot = 59, kTiredIdleSlot = 60, kTiredMoveSlot = 61;
constexpr float kTiredMoveFrames = 50.0f;
constexpr float kTiredPhase = 0.34f;

bool LowReadyItem(uint16_t id) {  // the handguns (inventory.cpp's group 1; not the magnums: RE6 holds them otherwise)
    switch (id) {
    case 0x0102: case 0x0110: case 0x0111: case 0x0112: case 0x011E: case 0x011F: case 0x0129:
        return true;
    default:
        return false;
    }
}

bool TickLowReady(uintptr_t p, uint32_t action) {
    const bool free = action <= 0x09 && !g_sprinting && !g_groundLegs && !PlayerInWeskerMove(p);
    const bool tired = free && Player1StaminaOutPose();
    const bool low = free && !tired && g_lowReadyOn.load(std::memory_order_relaxed) && LowReadyItem(PlayerHeldItem(p));
    if (!tired && !low) {
        if (g_upperOverlay) RestoreLegLayer(p);
        return false;
    }
    const bool moving = tired && action >= 0x02 && action <= 0x08;
    const uint32_t motion = Re6EvadeMotion(tired ? (moving ? kTiredMoveSlot : kTiredIdleSlot) : kLowReadySlot);
    if (!motion || !EnsureLayerSetupHook()) return false;
    const uintptr_t record = LayerRecord(p, kLegLayer);
    uint16_t current = 0, mode = 0;
    mem::Read(record + 4, current);
    if (!g_moving) {
        mem::Read(record + 0xA, mode);
        g_savedMotion = current;
        g_savedMode = mode;
        g_moving = true;
        g_legMotion = 0;
    } else if (g_legMotion && current != (g_legMotion & 0xFFFF)) {  // the game put its own there: that one given back at the end
        mem::Read(record + 0xA, mode);
        g_savedMotion = current;
        g_savedMode = mode;
        g_legMotion = 0;
    }
    g_upperOverlay = true;
    uint8_t hand = 0;
    mem::Read(p + 0x1AF9, hand);
    const uint32_t mirror = hand == 1 ? 0x100u : 0u;
    if (g_legMotion != motion || g_legMirror != mirror) {
        g_legMotion = motion;
        g_legMirror = mirror;
        SetMotion(p, kLegLayer, motion, 12.0f, 0x40 | mirror);
        LOG("upper overlay: %s on (item %04X, action %X, motion %X)",
            tired ? (moving ? "RE6's stamina-out pose, moving" : "RE6's stamina-out pose") : "RE6's handgun low ready", PlayerHeldItem(p),
            action, motion);
    }
    MaskLegLayer(p);
    float speed = 1.0f;
    if (moving) {  // in step with his legs
        const uintptr_t base = LayerRecord(p, 0);
        float frame = 0.0f, length = 0.0f, baseSpeed = 1.0f;
        if (mem::Read(base + 0x18, frame) && mem::Read(base + 0x1C, length) && mem::Read(base + 0x24, baseSpeed) && length > 1.0f) {
            float phase = frame / length + kTiredPhase;
            phase -= std::floor(phase);
            mem::Write(record + 0x18, phase * kTiredMoveFrames);
            speed = kTiredMoveFrames / length * baseSpeed;
        }
    }
    mem::Write(record + 0x24, speed);
    return true;
}

// Every frame when "Marcher pendant les actions" is off (TickAimWalk calls TickLowReady itself otherwise): the low ready and the stamina-out pose.
void TickLowReadyAlone() {
    if (g_walkOn || (!g_upperOverlay && !g_lowReadyOn.load(std::memory_order_relaxed) && !Player1StaminaOutPose())) return;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !actors::OnFoot(p) || !mem::Read(p + 0x10E0, action)) {
        if (g_upperOverlay) RestoreLegLayer(p);
        return;
    }
    TickLowReady(p, action);
}

void TickAimWalk() {
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_last.QuadPart ? static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
    g_last = now;
    if (dt > 0.1f) dt = 0.1f;
    dt *= Player1TimeSpeed();  // the game's time speed ("Vitesse du temps", user 2026-10-21)
    const uintptr_t p = actors::Player(0);
    uint32_t action = 0;
    // Not aboard the boat or the truck: his position there is the vehicle's local one.
    if (!actors::InGame() || !p || !actors::OnFoot(p) || !mem::Read(p + 0x10E0, action)) {
        RestoreLegLayer(p);
        g_havePrev = false;
        if (!g_boatCamera) hooks::TakeRawMouseX();  // TickBoatCamera's while driving the airboat
        return;
    }
    TickMouseTurn(p, action);
    TickPadIgnoredTurn(p, action, dt);
    if (g_groundLegs) return;  // on the ground (RE6): the leg layer is Re6GroundLegs's, no walking yet
    if (TickLowReady(p, action)) return;  // RE6's handgun low ready over his own legs (the layer is its)
    if (!WalkAction(action) || QuickShotHoldsPlayer()) {
        StopWalkVelocity();
        RestoreLegLayer(p);
        return;
    }
    float f = 0.0f, r = 0.0f;  // forward, left
    const uintptr_t input = InputBlock(p);
    float stick[2]{};
    const bool usePad = PadInUse(p);
    if (usePad && PadMouseWayNow()) {  // the game does not see the left stick in this aim: its real position
        float x = 0.0f, y = 0.0f;
        hooks::PadLeftStick(x, y);
        f = y;
        r = -x;
        if (f * f + r * r < 0.25f * 0.25f) f = r = 0.0f;
    } else if (usePad && input && mem::SafeRead(reinterpret_cast<void*>(input + 0x410), stick, sizeof(stick))) {
        f = stick[1];
        r = -stick[0];
        if (f * f + r * r < 0.25f * 0.25f) f = r = 0.0f;  // stick dead zone
    } else {
        const bool forward = GameButtonHeld(0x00, Player1Pad()), back = GameButtonHeld(0x01, Player1Pad());
        const bool left = GameButtonHeld(0x02, Player1Pad()), right = GameButtonHeld(0x03, Player1Pad());
        f = (forward ? 1.0f : 0.0f) - (back ? 1.0f : 0.0f);
        r = (left ? 1.0f : 0.0f) - (right ? 1.0f : 0.0f);
    }
    if (f == 0.0f && r == 0.0f) {
        StopWalkVelocity();
        RestoreLegLayer(p);
        return;
    }
    // The direction in the world: relative to Chris on the keyboard (the mouse turns him, the camera follows), relative
    // to the camera on a pad (the right stick turns the camera around him, user, 2026-10-20: forward went sideways).
    // The leg motion follows the direction relative to Chris. In the RE6 locomotion's kept camera (reload, weapon change,
    // heal) relative to the camera with the keyboard too (user, 2026-10-24: facing the camera, forward walked him back).
    float yaw = 0, pos[3]{};
    yaw = QuatYaw(p);
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return;
    float base = yaw;
    if ((usePad || g_locoKeeping) && CameraYaw(base)) base = Wrap(base - ParentYaw(p));  // the camera's, on his platform
    else base = yaw;
    const float len = std::sqrt(f * f + r * r);
    const float mx = (std::sin(base) * f + std::cos(base) * r) / len;  // side (left) = (cos, -sin): user, 2026-10-15
    const float mz = (std::cos(base) * f - std::sin(base) * r) / len;
    // RE6 mode through a reload, weapon change or heal: he turns to his direction at the mode's speed and walks ahead.
    if (g_locoKeeping && MouseIgnored(action)) {
        const float want = std::atan2(mx, mz), diff = Wrap(want - yaw);
        const float turn = g_locoTurnSpeed.load(std::memory_order_relaxed) * 0.0174533f * dt;
        yaw = std::fabs(diff) <= turn ? want : Wrap(yaw + (diff > 0.0f ? turn : -turn));
        SetBodyYaw(p, yaw);
    }
    const float fr = mx * std::sin(yaw) + mz * std::cos(yaw), rr = mx * std::cos(yaw) - mz * std::sin(yaw);
    const bool frontBack = std::fabs(fr) >= 0.5f * std::fabs(rr);  // the dominant axis picks the motion
    // Dying (user, 2026-10-20: aiming walked faster than the dying walk): no run, half the walking speed.
    const bool dying = Player1Dying();
    const bool running = fr > 0.0f && frontBack && !dying && RunAllowed(action);
    // A left-handed player's motions play mirrored (user, 2026-10-26: Sheva's legs walked right-handed under her
    // left-handed aim, her body twisted): the game's layer 0 mode has bit 0x100 then (Sheva 0x140 / 0x141 / 0x144,
    // Chris 0x40 / 0x41 / 0x44; trainer.log SetMotion lines); the legs take it too, a side step mirrored is the other
    // side's.
    uint16_t baseMode = 0;
    mem::Read(LayerRecord(p, 0) + 0xA, baseMode);
    const uint32_t mirror = baseMode & 0x100;
    const bool towardsLeft = mirror ? rr <= 0.0f : rr > 0.0f;
    // The legs walk in the list the aim plays from (user, 2026-10-26: Sheva's body twisted walking forward with her
    // bow): its aim and walk are in bank 2 (0x216 / 0x218 / 0x21A, walk 0x201-0x203: trainer.log), the other weapons'
    // in bank 5, same indices.
    uint16_t baseMotion = 0;
    mem::Read(LayerRecord(p, 0) + 4, baseMotion);
    const uint32_t bank = (baseMotion >> 8) == 2 ? 0x200u : 0x500u;
    const uint32_t motion = bank | ((running ? kRun
                                     : frontBack ? (fr < 0.0f ? kWalkBack : kWalk) : (towardsLeft ? kWalkLeft : kWalkRight)) & 0xFF);
    if (!g_moving) {
        uint16_t saved = 0, mode = 0;
        mem::Read(LayerRecord(p, kLegLayer) + 4, saved);
        mem::Read(LayerRecord(p, kLegLayer) + 0xA, mode);
        g_savedMotion = saved;
        g_savedMode = mode;
        g_moving = true;
    }
    // The fire action puts its own motion on layer 7 (user, 2026-10-15: the legs froze when shooting): it becomes the
    // motion given back at the end, and the walk goes back on.
    uint16_t current = 0, layerMode = 0;
    mem::Read(LayerRecord(p, kLegLayer) + 4, current);
    if (g_legMotion && current != g_legMotion) {
        mem::Read(LayerRecord(p, kLegLayer) + 0xA, layerMode);
        g_savedMotion = current;
        g_savedMode = layerMode;
        g_legMotion = 0;
    }
    if (g_legMotion != motion || g_legMirror != mirror) {
        g_legMotion = motion;
        g_legMirror = mirror;
        SetMotion(p, kLegLayer, motion, 10.0f, 0x40 | mirror);
    }
    MaskLegLayer(p);
    // During the parry's guard player 1's time runs faster (wesker_moves.cpp): the legs keep their pace.
    if (g_nativeWalk.load(std::memory_order_relaxed)) {  // the game moves him by the legs' motion (see g_nativeWalk)
        mem::Write(LayerRecord(p, kLegLayer) + 0x24, WeskerGuardLegSpeed() * (dying ? 0.5f : 1.0f));
        StopWalkVelocity();
        return;
    }
    mem::Write(LayerRecord(p, kLegLayer) + 0x24, WeskerGuardLegSpeed());
    float step = (running ? g_runSpeed : g_speed).load(std::memory_order_relaxed) * dt * std::min(len, 1.0f);
    if (dying) step *= 0.5f;
    // No sprint during actions (user, 2026-10-20): the run speed only.
    if (WalkStepsOnGameThread()) {  // the game's frames move him (AimWalkGameStep)
        const float speed = (running ? g_runSpeed : g_speed).load(std::memory_order_relaxed) * std::min(len, 1.0f) *
                            (dying ? 0.5f : 1.0f) * Player1TimeSpeed();
        g_walkVx = mx * speed;
        g_walkVz = mz * speed;
        g_walkPlayer = p;
        g_walkVelocityTick = GetTickCount64();
        return;
    }
    pos[0] += mx * step;
    pos[2] += mz * step;
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos));
}
}  // namespace

// wesker_moves.cpp: RE6's ground pose while he stays down aiming (user, 2026-11-08): the leg layer plays it on the hips and
// legs, the game's aim keeps the rest (the weapon, the fire, the crosshair).
void Re6GroundLegs(uintptr_t p, uint32_t motion, bool wholeBody, float startFrame, bool ownRoot) {
    if (!p || !motion || !EnsureLayerSetupHook()) return;
    uint16_t current = 0, mode = 0, baseMode = 0;
    const uintptr_t record = LayerRecord(p, kLegLayer);
    mem::Read(record + 4, current);
    if (!g_moving) {
        mem::Read(record + 0xA, mode);
        g_savedMotion = current;
        g_savedMode = mode;
        g_moving = true;
    }
    g_groundLegs = true;
    g_upperOverlay = false;  // the low ready's layer becomes the ground's
    g_shotLayer = false;     // and a quick shot's
    g_groundFull = wholeBody;
    g_groundRoot = ownRoot;
    StopWalkVelocity();
    // Also when the motion is already there but not as ours: the SetMotion hook gives the game's own request on this layer the last slide's motion before the first tick,
    // and with g_legMotion unset the layer was never masked: its spine over the aim (30 degrees) and its root moving him (user, 2026-11-08: "twisted one time in two,
    // and those go further"; after a roll the hook gave the ground's motion instead, so the slide was set up here).
    // A left-handed player (user, 2026-11-10: "you forgot to mirror the slide's legs in the left-handed animation"): layer 0's mirror
    // bit was read once, at the slide's start, before the aim (mirrored) began; now his handedness byte P+0x1AF9 (1 left: "Main
    // inversée" or Sheva's own) too, every update, the legs set again when it differs.
    mem::Read(LayerRecord(p, 0) + 0xA, baseMode);
    uint8_t hand = 0;
    mem::Read(p + 0x1AF9, hand);
    const uint32_t mirror = (baseMode & 0x100) || hand == 1 ? 0x100u : 0u;
    uint16_t nowMode = 0;
    mem::Read(record + 0xA, nowMode);  // the layer as it is (a game request may have set its own mode)
    if (current != motion || g_legMotion != motion || mirror != g_legMirror || (nowMode & 0x100u) != mirror) {
        g_legMotion = motion;
        g_legMirror = mirror;
        SetMotion(p, kLegLayer, motion, 4.0f, 0x40 | g_legMirror, startFrame);
    }
    MaskLegLayer(p);
    mem::Write(record + 0x24, 1.0f);
}
// RE6's upper body on the game's three aiming layers (the game blends them by the pitch): put back whenever the game changes them.
// sync: these motions run with the slide's frame; layers 1 and 2 keep the last slide's motion at a stale frame after a tap (only layer 0 is taken by the whole-body
// slide), and the same motion was left alone: the arms stayed aside on the next slide (user, 2026-11-08: "after a click's slide the next one is wrong").
// blend: the frames the game interpolates a changed motion over (the slide's pose turned to the aim: user, 2026-11-09).
void Re6AimLayers(uintptr_t p, const uint32_t (&motions)[3], float startFrame, bool sync, float blend) {
    for (uint32_t n = 0; n < 3; ++n) {
        const uintptr_t record = LayerRecord(p, n);
        uint16_t current = 0, mode = 0;
        mem::Read(record + 4, current);
        float frame = startFrame;
        if (sync) mem::Read(record + 0x18, frame);
        if (!motions[n] || (current == motions[n] && std::fabs(frame - startFrame) < 8.0f)) continue;
        mem::Read(record + 0xA, mode);
        SetMotion(p, n, motions[n], blend, mode, startFrame);
    }
}
// swaps.cpp's SetMotion hook (game thread): while the low ready holds the leg layer, the game's own request there is kept to be given
// back at the end and the low ready stays, at its frame (user, 2026-11-10: "the animation fights to go back to the high ready: I
// sometimes see the high one for a frame"; logged: the game puts its own motion there at every action change - wait, walk, run - and
// the tick put the low ready back one frame later, blended).
constexpr uint32_t kKeepLegLayer = 0xFFFFFFFFu;  // swaps.cpp's SetMotion hook: the game's request not made
uint32_t LowReadyLayerMotion(uintptr_t unit, uint32_t layer, uint32_t motion, uint32_t& start, uint32_t& speed, uint32_t& mode) {
    const bool ours = (g_upperOverlay && !g_groundLegs) || g_shotLayer;  // the low ready / stamina-out pose, a quick shot
    if (layer != kLegLayer || !ours || !g_legMotion || unit != actors::Player(0)) return motion;
    if ((motion & 0xFFFF) == (g_legMotion & 0xFFFF)) return motion;  // ours
    g_savedMotion = motion;
    g_savedMode = static_cast<uint16_t>(mode);
    uint16_t current = 0;
    float frame = 0.0f;
    const uintptr_t record = LayerRecord(unit, kLegLayer);
    // Not dropped (tried 2026-11-10: the game's actions wait for their own motion there - its turns 7 / 8 then flipped with the wait every
    // frame and player 1 was stuck, user: "even worse, stuck in the loop"); kKeepLegLayer stays unused. Ours set again a frame on (a
    // request every frame stood it still at the same frame: the shotgun's pump, frame 48 of 119).
    if (mem::Read(record + 4, current) && current == (g_legMotion & 0xFFFF) && mem::Read(record + 0x18, frame)) {
        frame += 1.0f;
        if (g_shotLayer) g_shotLastFrame = frame;
        std::memcpy(&start, &frame, sizeof(start));
    } else if (g_shotLayer) {  // the layer lost it (another way): back from where it was (user, 2026-11-10: "the shotgun looped up to 3 times")
        std::memcpy(&start, &g_shotLastFrame, sizeof(start));
    }
    mode = 0x40 | g_legMirror;
    // At its own speed (user, 2026-11-10: a shotgun quick shot stuck in its shot and pump: the game's own request there carried its speed,
    // a held pose's 0, and the RE6 shot froze in the middle, player 1 held until its end).
    const float one = 1.0f;
    std::memcpy(&speed, &one, sizeof(speed));
    return g_legMotion;
}
// wesker_moves.cpp's crawl: the ground's legs mirrored (a left-handed player): his left crawl is the right one's motion.
bool Re6GroundLegsMirrored() { return g_legMirror != 0; }
void Re6GroundLegsOff(uintptr_t p) {
    if (!g_groundLegs) return;
    g_groundLegs = false;
    RestoreLegLayer(p);
}

// quick_knife.cpp's RE6 quick shots (user, 2026-11-09): RE6's whole-body shot on the leg layer from its frame 0, every joint, its root's
// translation moving him (the shot steps back), its turn none (the shot turns him), mirrored for a left-handed player; kept through
// the game's aim and fire requests there (LowReadyLayerMotion).
bool PlayShotLayer(uintptr_t p, uint32_t motion) {
    if (!p || !motion || !EnsureLayerSetupHook()) return false;
    EnsureSetMotionHook();
    const uintptr_t record = LayerRecord(p, kLegLayer);
    uint16_t current = 0, mode = 0;
    mem::Read(record + 4, current);
    if (!g_moving) {
        mem::Read(record + 0xA, mode);
        g_savedMotion = current;
        g_savedMode = mode;
        g_moving = true;
    }
    g_upperOverlay = false;
    g_groundLegs = true;
    g_groundFull = true;
    g_groundRoot = true;
    g_shotLayer = true;
    g_shotLastFrame = 0.0f;
    StopWalkVelocity();
    uint8_t hand = 0;
    mem::Read(p + 0x1AF9, hand);
    g_legMirror = hand == 1 ? 0x100u : 0u;
    g_legMotion = motion;
    const bool set = SetMotion(p, kLegLayer, motion, 4.0f, 0x40 | g_legMirror, 0.0f);
    MaskLegLayer(p);
    mem::Write(record + 0x24, 1.0f);
    return set;
}

// Its frame and length (-1 when the layer no longer plays it).
float ShotLayerFrame(uintptr_t p) {
    uint16_t current = 0;
    float frame = -1.0f;
    const uintptr_t record = p ? LayerRecord(p, kLegLayer) : 0;
    if (!g_shotLayer || !p || !mem::Read(record + 4, current) || current != (g_legMotion & 0xFFFF) || !mem::Read(record + 0x18, frame))
        return -1.0f;
    g_shotLastFrame = frame;
    return frame;
}

float ShotLayerLength(uintptr_t p) {
    float length = 0.0f;
    return p && mem::Read(LayerRecord(p, kLegLayer) + 0x1C, length) ? length : 0.0f;
}

void StopShotLayer(uintptr_t p) {
    if (!g_shotLayer) return;
    g_shotLayer = false;
    Re6GroundLegsOff(p);
}

// Player 1's pad mask build (game thread, once a game frame, in his update): the aim walk's step (see g_walkVx).
void AimWalkGameStep() {
    uintptr_t clock = 0;
    float dt = 0.0f;  // the game's frame in seconds, before the time speed (in the velocity)
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB209C)), clock) || !clock || !mem::Read(clock + 0x24, dt) ||
        !(dt > 0.0f))
        return;
    dt = std::min(dt, 0.1f);
    g_walkGameStepTick = GetTickCount64();
    const ULONGLONG t = g_walkVelocityTick.load(std::memory_order_relaxed);
    const uintptr_t p = g_walkPlayer.load(std::memory_order_relaxed);
    if (!t || GetTickCount64() - t > 150 || !p || p != actors::Player(0) || dt <= 0.0f) return;
    const float vx = g_walkVx.load(std::memory_order_relaxed), vz = g_walkVz.load(std::memory_order_relaxed);
    if (vx == 0.0f && vz == 0.0f) return;
    float pos[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos))) return;
    pos[0] += vx * dt;
    pos[2] += vz * dt;
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos));
}

// re6_motions.cpp: player 1 sprinting ("Sprint (tenir la course)").
bool Player1Sprinting() { return g_sprinting; }
// swaps.cpp's slide: the run key really held now (the pad's real bit, else the keyboard / pad button), as the sprint reads it.
bool Player1SprintKeyHeld() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p) return false;
    return PadInUse(p) ? g_padRunReal.load(std::memory_order_relaxed) : RawButtonHeld(0x04, -1) || RawButtonHeld(0x04, Player1Pad());
}

// The normal camera's eye (+0x170) and its yaw / pitch (up > 0) towards its target (+0x190): [[RVA 0xE3457C]+0x34]+0xDF0
// (uCameraQFPS, also the aim camera: its mode bytes +0x1B1..).
bool CameraRay(float* eye, float& yaw, float& pitch) {
    uintptr_t holder = 0, ctrl = 0, cam = 0;
    float target[3]{};
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE3457C)), holder) || !holder ||
        !mem::Read(holder + 0x34, ctrl) || !ctrl || !mem::Read(ctrl + 0xDF0, cam) || !cam ||
        !mem::SafeRead(reinterpret_cast<void*>(cam + 0x170), eye, 3 * sizeof(float)) ||
        !mem::SafeRead(reinterpret_cast<void*>(cam + 0x190), target, sizeof(target)))
        return false;
    const float dx = target[0] - eye[0], dz = target[2] - eye[2];
    if (dx * dx + dz * dz < 1e-4f) return false;
    yaw = std::atan2(dx, dz);
    pitch = std::atan2(target[1] - eye[1], std::sqrt(dx * dx + dz * dz));
    return true;
}

// Player 1's pad index (P+0x2D9C), the one the game reads his buttons with: 1 for a local player in the game's slot 1
// (user, 2026-10-26: Jill in Desperate Escape could not walk while aiming nor use the RE6 locomotion, his
// buttons read on pad 0).
int Player1Pad() {
    uint8_t pad = 0;
    const uintptr_t p = actors::Player(0);
    return p && mem::Read(p + 0x2D9C, pad) && pad < 4 ? pad : 0;
}

// Player 1 plays with a pad (the game's own test, VA 0x799AF0 on his pad P+0x2D9C).
bool Player1PadInUse() { return PadInUse(actors::Player(0)); }

// The direction player 1 holds (forward, left: -1..1), from the left stick on a pad (its directions never go through
// the button queries), from the direction buttons otherwise. For the dodges (swaps.cpp; user, 2026-10-23: the aim dodge
// did nothing on a pad).
// Read as the player really holds them: "Déplacement selon la direction (RE6)" shows the game forward only.
void Player1Direction(float& forward, float& left) {
    forward = left = 0.0f;
    const uintptr_t p = actors::Player(0);
    if (!p) return;
    if (PadInUse(p)) {
        float x = 0.0f, y = 0.0f;
        hooks::PadLeftStick(x, y);
        if (x * x + y * y < 0.25f * 0.25f) return;
        forward = y;
        left = -x;
        return;
    }
    forward = (RawButtonHeld(0x00, Player1Pad()) ? 1.0f : 0.0f) - (RawButtonHeld(0x01, Player1Pad()) ? 1.0f : 0.0f);
    left = (RawButtonHeld(0x02, Player1Pad()) ? 1.0f : 0.0f) - (RawButtonHeld(0x03, Player1Pad()) ? 1.0f : 0.0f);
}

bool Re6LocomotionOn();  // below
bool FirstPersonOn();    // camera.cpp: "Vue à la première personne" (the free camera and the RE6 locomotion stand aside)
bool FirstPersonHidesSheath(uintptr_t player);  // camera.cpp: player 1 seen in the first person now

// The direction held relative to player 1's body (forward, left), for the dodges (swaps.cpp; user, 2026-10-27: the
// dodge read his movement, so in an animation that does not move him a quick duck came instead of a roll): the keys
// are relative to him (the mouse turns him), the pad's stick and the keys of "Déplacement selon la direction (RE6)"
// relative to the camera (as the walk during actions).
void Player1BodyDirection(float& forward, float& left) {
    float f = 0.0f, r = 0.0f;
    Player1Direction(f, r);
    forward = f;
    left = r;
    const uintptr_t p = actors::Player(0);
    if (!p || (f == 0.0f && r == 0.0f) || (!PadInUse(p) && !Re6LocomotionOn())) return;
    // The camera's yaw is the world's: his body's too (on a boat the quaternion is the boat's: user, 2026-10-29, the
    // dodges came 90 degrees off).
    const float yaw = Wrap(QuatYaw(p) + ParentYaw(p));
    float base = yaw;
    if (!CameraYaw(base)) return;
    const float mx = std::sin(base) * f + std::cos(base) * r, mz = std::cos(base) * f - std::sin(base) * r;
    forward = mx * std::sin(yaw) + mz * std::cos(yaw);
    left = mx * std::cos(yaw) - mz * std::sin(yaw);
}

// For the parry (parry.cpp): with a pad its buttons only reach the game through the held mask (knife aim 0x400, fire
// 0x8000). Returns false on the keyboard (the button queries work there).
bool PadHeldMask(uint32_t& mask) {
    if (!g_maskCall || !g_maskCall->IsApplied() || !PadInUse(actors::Player(0))) return false;
    mask = g_heldMask.load(std::memory_order_relaxed);
    return true;
}

// "Réticule à la manette" (patch_features.cpp) switches the mouse-like pad aim on and sets its speed.
bool SetPadMouseAim(bool on) {
    if (on && !EnsureMaskHook()) return false;
    g_padMouseAim = on;
    if (!on) {
        SetZoneZero(false);
        ReleaseMaskHook();
    }
    return true;
}

// The quick shots and the fire key (quick_knife.cpp) need the pad's held mask.
// "Roulade arrière au sol" (wesker_moves.cpp) reads the pad's real run bit (user: "pad compatible").
bool UsePadMaskForGroundRoll(bool on) {
    g_groundRollMask = on;
    if (on) return EnsureMaskHook();
    ReleaseMaskHook();
    return true;
}

bool UsePadMaskForQuickShot(bool on) {
    g_quickShotMask = on;
    if (on) return EnsureMaskHook();
    ReleaseMaskHook();
    return true;
}

// The pad's aim through the game's own mouse aim (user, 2026-11-06: the mouse's rendering with a pad - the camera leading,
// Chris following with his turning steps, the same top for the camera and the weapon). The on-foot aim reads the mouse's
// virtual stick (input +0x6E8 direction in degrees, +0x6EC amount; +0x6F4 / +0x6F8 dx / dy) behind VA 0xB5C4F0 ("keyboard
// / mouse": !pad in use for the local player) at VA 0xB8BA7F / 0xB8BCED / 0xB8BEBF / 0xB8C028 / 0xB8C6A6 / 0xB8C78A; the
// aim camera's basis from the aim angles behind the "pad in use" calls VA 0xB751BC, its height from P+0x2908 behind VA
// 0x8455A1. The mask builder VA 0xC6E150 fills that virtual stick from the mouse (sMouse [RVA 0xE49C78] +0x2C / +0x30, at
// VA 0xC6E482: on its object input +0x4E8, +0x200 = -atan2(dx, -dy) in degrees, +0x204 = |(dx, dy)|, +0x20C / +0x210 =
// dx / dy, all times the mouse setting) and zeroes it with a pad. While player 1 aims on foot with a pad and the rule
// wants it (camera.cpp), those calls answer "mouse", the right stick is hidden from the game and fills the virtual stick
// after the mask builder (AfterMaskBuild) as mouse counts.
std::atomic<bool> g_padMouseWay{ false };     // camera.cpp's rule: the mouse's rendering for the pad
std::atomic<bool> g_padMouseWayNow{ false };  // game thread: on this frame
// The reverse (user, 2026-11-08: "Souris : rendu manette avec le laser"): the mouse with its laser through the game's pad aim
// (camera on the body, height from P+0x2DC8, the laser in the free zone), its counts added to the zone and the pitch.
std::atomic<bool> g_mouseLaserWay{ false };
std::atomic<bool> g_mouseLaserNow{ false };
bool MouseLaserNow() { return g_mouseLaserNow.load(std::memory_order_relaxed); }
std::deque<mem::Patch> g_padMouseCalls;
LARGE_INTEGER g_padMouseLast{};

using KeyboardMouseFn = bool(__fastcall*)(void* player, void* edx);
using PadInUseQueryFn = bool(__fastcall*)(void* bioPad, void* edx, int index);

bool __fastcall AimSiteKeyboardMouse(void* player, void* edx) {
    if (reinterpret_cast<uintptr_t>(player) == actors::Player(0)) {
        if (g_mouseLaserNow.load(std::memory_order_relaxed)) return false;
        if (g_padMouseWayNow.load(std::memory_order_relaxed)) return true;
    }
    return reinterpret_cast<KeyboardMouseFn>(mem::Rva(0x75C4F0))(player, edx);
}
// The tests only the mouse's way through the pad's aim changes.
bool __fastcall AimSiteMouseLaserKeyboard(void* player, void* edx) {
    if (g_mouseLaserNow.load(std::memory_order_relaxed) && reinterpret_cast<uintptr_t>(player) == actors::Player(0)) return false;
    return reinterpret_cast<KeyboardMouseFn>(mem::Rva(0x75C4F0))(player, edx);
}
// The control type (VA 0xCEE1B0, thiscall(config, 0) ret 4: 1 while the game takes the mouse as used, [RVA 0xE3457C]
// +0x3085, else the options' +0x5C - 0 with a pad): 1 at the aim's sites, as with the mouse (recorded 2026-11-07: the zone,
// VA 0xB87A81, only follows the mouse's aim with 1 - with 0 Chris stood still, the zone 0, P+0x2A10 still).
using ControlTypeFn = int(__fastcall*)(void* config, void* edx, int which);
int __fastcall AimSiteControlType(void* config, void* edx, int which) {
    if (g_mouseLaserNow.load(std::memory_order_relaxed) && which == 0) return 0;
    if (g_padMouseWayNow.load(std::memory_order_relaxed) && which == 0) return 1;
    return reinterpret_cast<ControlTypeFn>(mem::Rva(0x8EE1B0))(config, edx, which);
}
bool __fastcall AimSitePadInUse(void* bioPad, void* edx, int index) {
    if (g_mouseLaserNow.load(std::memory_order_relaxed) && index == Player1Pad()) return true;
    if (g_padMouseWayNow.load(std::memory_order_relaxed)) return false;
    return reinterpret_cast<PadInUseQueryFn>(mem::Rva(0x399AF0))(bioPad, edx, index);
}

// The actions of the aim those call sites answer for.
bool PadWayAimAction(uint32_t action) {
    return (action >= 0x1B && action <= 0x1E) || (action >= 0x22 && action <= 0x27 && action != 0x24);
}

bool EnsurePadMouseCalls() {
    if (!g_padMouseCalls.empty()) return true;
    const auto call = [](uintptr_t siteRva, uintptr_t targetRva, void* replacement) {
        uint8_t* site = mem::Rva(siteRva);
        auto rel = [site](uintptr_t to) {
            const int32_t r = static_cast<int32_t>(to - reinterpret_cast<uintptr_t>(site + 5));
            return std::vector<uint8_t>{ 0xE8, static_cast<uint8_t>(r), static_cast<uint8_t>(r >> 8),
                                         static_cast<uint8_t>(r >> 16), static_cast<uint8_t>(r >> 24) };
        };
        g_padMouseCalls.emplace_back("pad aim through the mouse's", site, rel(reinterpret_cast<uintptr_t>(mem::Rva(targetRva))),
                                     rel(reinterpret_cast<uintptr_t>(replacement)));
    };
    // The aim's turn and zone (the function VA 0xB86CD0: the pad's speed VA 0xB877FC, the zone VA 0xB87A27; recorded
    // 2026-11-06 23:50: with only the virtual stick's readers redirected the aim turned but Chris stood still) and the
    // virtual stick's readers.
    // Not VA 0xB8C37C / 0xB85E72 / 0xB86D7B / 0xB873F8 / 0xB878A0 (2026-11-08): they run the mouse's own correction (VA
    // 0xB85E50: the aim point in the camera's frame as a stick) and Chris spun, 350 degrees an update, the zone at its limits
    // (research/pad_aim/rec16.txt). Chris's aim is put on the camera's instead: PadMouseWayPinAim.
    for (uintptr_t site : { 0x786ED4u, 0x7871AAu, 0x78731Du, 0x78737Eu, 0x787478u, 0x7875BBu,
                            0x787A64u, 0x787D6Cu, 0x787F12u, 0x788045u, 0x788192u, 0x788262u, 0x7882FEu, 0x78832Du, 0x78837Eu,
                            0x78BA7Fu, 0x78BCEDu, 0x78BEBFu, 0x78C028u, 0x78C6A6u, 0x78C78Au,
                            // the input's mouse turn (VA 0xB8AD2F, reads the virtual stick at VA 0xB8AD69 / 0xB8AD71):
                            // P+0x2A10, Chris turned to it with his turning steps (2026-11-07: he stood still without it)
                            0x78AD2Fu })
        call(site, 0x75C4F0, reinterpret_cast<void*>(&AimSiteKeyboardMouse));
    // The crosshair's colour (user, 2026-11-08: never green, never red on a target with the pad): VA 0xB76C22 skips it with
    // a pad (VA 0xB76D5C: 0xB69280's result to [P+0x2640]+0x374), VA 0xB671DE gives the target test the weapon's point
    // instead of the mouse's aim point P+0x2850.
    for (uintptr_t site : { 0x7751BCu, 0x4455A1u, 0x776C22u, 0x7671DEu })
        call(site, 0x399AF0, reinterpret_cast<void*>(&AimSitePadInUse));
    // The other "keyboard / mouse" tests of the aim, for the mouse through the pad's aim only.
    for (uintptr_t site : { 0x786D7Bu, 0x7873F8u, 0x7878A0u, 0x78C37Cu, 0x7848ECu, 0x784A07u, 0x784CD4u, 0x7858E3u, 0x78A648u,
                            0x78B053u, 0x78B135u, 0x78B526u, 0x78B75Eu, 0x78CA54u, 0x78CBC4u })
        call(site, 0x75C4F0, reinterpret_cast<void*>(&AimSiteMouseLaserKeyboard));
    // Not VA 0xB751ED / 0xB75379 (2026-11-08): they answered 1 where the game chooses the aim block's way P+0x291F (its test VA
    // 0xB67916, only VA 0xB7538C writes 1): with 1 the left stick turned the aim and the right one stopped, until a real
    // mouse aim gave it 0 back (user: "as soon as I touch the left stick the right one no longer answers").
    for (uintptr_t site : { 0x78AD50u, 0x78C6C3u, 0x78C857u })
        call(site, 0x8EE1B0, reinterpret_cast<void*>(&AimSiteControlType));
    // Given back at the unload (2026-11-06 23:56: without it they still called this DLL once freed and the game crashed
    // at the hot reload - the 2026-10-27 lesson, every call into the DLL needs its restore).
    OnRestore([] {
        g_padMouseWayNow = false;
        g_mouseLaserNow = false;
        hooks::HidePadRightXForAim(false);
        hooks::HidePadLeftForAim(false);
        for (mem::Patch& patch : g_padMouseCalls) patch.Restore();
    });
    bool ok = true;
    for (mem::Patch& patch : g_padMouseCalls) ok = patch.Apply() && ok;
    if (!ok) {
        for (mem::Patch& patch : g_padMouseCalls) patch.Restore();
        LOG("pad aim through the mouse's: a call site was not the expected one");
        return false;
    }
    return true;
}

constexpr float kMouseWayGain = 40.0f;  // the game's turn per virtual stick unit is 40 times its parameter (measured, see below)

// The limits the aim function uses, degrees (VA 0xB87213 / 0xB87221 by the weapon's table line, else VA 0xB8734C / 0xB8735A).
bool AimPitchLimits(uintptr_t p, float& up, float& down) {
    up = 69.6f;
    down = 71.6f;
    mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB5994)), up);
    mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB5998)), down);
    using RecordFn = uintptr_t(__fastcall*)(void*, void*, int);
    const uintptr_t record = reinterpret_cast<RecordFn>(mem::Rva(0x84B700))(reinterpret_cast<void*>(p + 0x21A0), nullptr, 0xE);
    uint32_t id = 0;
    if (record && mem::Read(record + 4, id) && (id & 0xFF00) == 0x100) {
        const uintptr_t line = reinterpret_cast<uintptr_t>(mem::Rva(0xDB5170)) + (id & 0xFF) * 0x140;
        mem::Read(line + 0xA4, up);
        mem::Read(line + 0xA8, down);
    }
    return up > 1.0f && down > 1.0f;
}

// The mouse through the game's pad aim: its counts (the virtual stick the mask builder just filled) added where the pad's
// right stick would be integrated (input +0x4B4 the free zone in degrees, past it the game turns Chris; +0x4B8 the pitch).
void MouseLaserStep(uintptr_t object, uintptr_t p) {
    const uintptr_t input = InputBlock(p);
    uintptr_t params = 0;
    float dx = 0.0f, dy = 0.0f;
    if (!input || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB27B4)), params) || !params || !mem::Read(object + 0x20C, dx) ||
        !mem::Read(object + 0x210, dy) || (dx == 0.0f && dy == 0.0f))
        return;
    float perCount = 0.001f, pitchMax = 0.997f;
    mem::Read(params + 0x450, perCount);
    mem::Read(params + 0x29C, pitchMax);
    const float degrees = perCount * kMouseWayGain * 57.2957795f;
    float zone = 0.0f, pitch = 0.0f, up = 0.0f, down = 0.0f;
    if (mem::Read(input + 0x4B4, zone)) mem::Write(input + 0x4B4, zone - dx * degrees);
    if (mem::Read(input + 0x4B8, pitch) && AimPitchLimits(p, up, down)) {
        pitch += dy * degrees / (pitch < 0.0f ? up : down);
        mem::Write(input + 0x4B8, std::max(-pitchMax, std::min(pitchMax, pitch)));
    }
}

// One source for Chris and the camera (user, 2026-11-08: two speeds, however close, drift apart, and up / down Chris went
// further than the camera): the right stick only moves the camera's aim (the virtual stick), Chris's aim is set from it
// before the game's aim function reads it (VA 0xB86CD0, same update). Across, input +0x4B4 = the aim yaw P+0x2A24 less his
// body's: within the free zone he stays, past it the game turns him by the excess (input +0x454, VA 0xB87C87), as the mouse
// (recorded: the zone is the aim less the body, +-6.93). Up / down, input +0x4B8 = the camera's pitch over the weapon's
// limit (recorded with the mouse, research/pad_aim/rec4-5: -pitch / 71.6 down, -pitch / its limit up; 0.909 at -64.2).
void PadMouseWayPinAim(uintptr_t p, float dt) {
    const uintptr_t input = InputBlock(p);
    uintptr_t params = 0;
    if (!input || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB27B4)), params) || !params) return;
    float zone = 6.93f, pitchMax = 0.997f;
    mem::Read(params + 0x2AC, zone);
    mem::Read(params + 0x29C, pitchMax);
    float aimYaw = 0.0f;
    if (mem::Read(p + 0x2A24, aimYaw)) {
        constexpr float kMaxTurnDegrees = 540.0f;  // a second: a camera far behind him at the aim's start is not a jump
        const float behind = Wrap(aimYaw - Wrap(QuatYaw(p) + ParentYaw(p))) * 57.2957795f;
        const float reach = zone + kMaxTurnDegrees * dt;
        mem::Write(input + 0x4B4, std::max(-reach, std::min(reach, behind)));
    }
    float eye[3]{}, cameraYaw = 0.0f, cameraPitch = 0.0f;
    if (!CameraRay(eye, cameraYaw, cameraPitch)) return;
    float up = 0.0f, down = 0.0f;
    if (!AimPitchLimits(p, up, down)) return;
    const float degrees = cameraPitch * 57.2957795f;
    const float pitch = -degrees / (degrees > 0.0f ? up : down);
    mem::Write(input + 0x4B8, std::max(-pitchMax, std::min(pitchMax, pitch)));
}

// Mask builder (game thread): on or off for this frame, and the virtual stick from the right stick.
void PadMouseWayStep(uintptr_t object) {
    const uintptr_t p = actors::Player(0);
    uint32_t action = 0;
    const bool aiming = p && actors::OnFoot(p) && mem::Read(p + 0x10E0, action) && PadWayAimAction(action);
    const bool pad = aiming && PadInUse(p);
    const bool on = g_padMouseWay.load(std::memory_order_relaxed) && pad;
    const bool mouseLaser = g_mouseLaserWay.load(std::memory_order_relaxed) && aiming && !pad;
    // The camera snap at the reload (user, 2026-11-10: "a strange camera snap at the reload", several times; research log "reload turn
    // research"): through the pad's aim the game left the mouse's yaw P+0x2A10 where it was before the aim (as with a pad), so the normal
    // camera came back 70-160 degrees off Chris when the aim was lowered, and jumped onto him when the reload's mouse turn set P+0x2A10
    // from his body. And in the aim's end 0x1D the counts still went into the free zone, which the game no longer turns him by there:
    // up to 164 degrees piled up (logged; its limit is 6.93). Now P+0x2A10 follows his body all through that aim, as the mouse's own
    // code keeps it, the aim's end takes no counts, and the zone is brought back within its limit when this way ends.
    if (mouseLaser != g_mouseLaserNow.load(std::memory_order_relaxed)) {
        g_mouseLaserNow = mouseLaser;
        LOG("mouse aim through the pad's: %s (action %X)", mouseLaser ? "on" : "off", action);
        uintptr_t params = 0;
        float limit = 6.93f, zone = 0.0f;
        const uintptr_t input = p ? InputBlock(p) : 0;
        if (!mouseLaser && input && mem::Read(input + 0x4B4, zone)) {
            if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB27B4)), params) && params) mem::Read(params + 0x2AC, limit);
            if (std::fabs(zone) > limit) mem::Write(input + 0x4B4, zone > 0.0f ? limit : -limit);
        }
    }
    if (mouseLaser) {
        if (action != 0x1D) MouseLaserStep(object, p);
        mem::Write(p + 0x2A10, QuatYaw(p));
    }
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_padMouseLast.QuadPart ? static_cast<float>(now.QuadPart - g_padMouseLast.QuadPart) / static_cast<float>(freq.QuadPart)
                                       : 0.0f;
    g_padMouseLast = now;
    if (on != g_padMouseWayNow.load(std::memory_order_relaxed)) {
        // The right stick only feeds the virtual stick: seen by the game it would move Chris's aim as well. The left stick
        // is the walk's (user, 2026-11-08: through the mouse's way the game turned the aim with it, as with the direction keys).
        hooks::HidePadRightXForAim(on);
        hooks::HidePadLeftForAim(on);
        g_padMouseWayNow = on;
        LOG("pad aim through the mouse's: %s (action %X)", on ? "on" : "off", action);
    }
    if (!on) return;
    if (dt > 0.05f) dt = 0.05f;
    // As mouse counts: the turn the game's pad aim reaches at full tilt (parameter +0x2D0, 57.9 degrees / s, scaled by
    // "Sensibilité de visée (manette)") through the mouse's multiplier (+0x450, 0.001 a count, scaled by "Sensibilité de
    // visée (souris)"), divided by kMouseWayGain: measured 2026-11-06 23:50, the aim turned ~40 times faster than that
    // (research/pad_aim/rec7.txt: 40-80 degrees every 12 ms).
    uintptr_t params = 0;
    float padSpeed = 57.9f, perCount = 0.001f;
    if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB27B4)), params) && params) {
        mem::Read(params + 0x2D0, padSpeed);
        mem::Read(params + 0x450, perCount);
    }
    if (!(perCount > 1e-6f)) perCount = 0.001f;
    const float counts = padSpeed * 0.0174532925f / perCount * dt / kMouseWayGain;
    const float dx = hooks::PadRightX() * counts, dy = -hooks::PadRightY() * counts;
    const float angle = dx == 0.0f && dy == 0.0f ? 0.0f : -std::atan2(dx, -dy) * 57.2957795f;
    mem::Write(object + 0x200, angle);
    mem::Write(object + 0x204, std::sqrt(dx * dx + dy * dy));
    mem::Write(object + 0x20C, dx);
    mem::Write(object + 0x210, dy);
    PadMouseWayPinAim(p, dt);
}

// Wanted by camera.cpp's rule (render thread): the call sites and the mask builder's hook installed then.
bool PadMouseWayWanted() { return g_padMouseWay.load(std::memory_order_relaxed) || g_mouseLaserWay.load(std::memory_order_relaxed); }
bool PadMouseWayNow() { return g_padMouseWayNow.load(std::memory_order_relaxed); }
void SetPadMouseWay(bool on) {
    if (on == g_padMouseWay.load(std::memory_order_relaxed)) return;
    if (on && (!EnsureMaskHook() || !EnsurePadMouseCalls())) return;
    g_padMouseWay = on;
    if (!on) ReleaseMaskHook();
}
void SetMouseLaserWay(bool on) {
    if (on == g_mouseLaserWay.load(std::memory_order_relaxed)) return;
    if (on && (!EnsureMaskHook() || !EnsurePadMouseCalls())) return;
    g_mouseLaserWay = on;
    if (!on) {
        g_mouseLaserNow = false;
        ReleaseMaskHook();
    }
}

// The parry (parry.cpp) needs the pad's held mask.
bool UsePadMaskForParry(bool on) {
    g_parryMask = on;
    if (on) return EnsureMaskHook();
    ReleaseMaskHook();
    return true;
}

namespace {
// Free camera standing still (user, 2026-10-23): the mouse / right stick turns the camera around Chris while he stands,
// and he turns to the camera when he moves. Recorded with the mouse: the camera follows the mouse's yaw P+0x2A10 (0.03
// rad behind) and the body follows it through the turns in place 7 / 8. The mouse code (VA 0xB8AD98..0xB8AFE7) adds the
// counts to P+0x2A10; while it is more than 5 degrees ([VA 0xF6EB78]) from the body it sets P+0x2A14 and the turn mask
// 4 / 8 (VA 0xB8AFDD / 0xB8AFE7) that turns him, and with P+0x2A14 clear it first brings P+0x2A10 back within 5 degrees
// of the body (VA 0xB8ADAF..0xB8ADF3). Standing still (wait 0, no direction held) both are jumped over: the mouse only
// turns the camera. On a pad the game already turns the camera alone with the right stick while he stands (the mouse
// code is not run; the pad part is to do). Leaving the stand, the body turns to the camera: at once for the aim, at
// kAlignSpeed for a move.
// Pad (user, 2026-10-23: the right stick like the mouse): there the right stick turned Chris in place (actions 7 / 8,
// recorded) and the camera followed his body. The camera follows P+0x2A10 only while P+0x2A15 is set (VA 0xB74530):
// the mouse code sets it every frame (VA 0xB8AFCD) and it is cleared before the next frame's input (VA 0xB74B56); a pad
// skips the mouse code (je at VA 0xB8AD36 after the keyboard / mouse test VA 0xB5C4F0), so P+0x2A10 stays stale and
// setting P+0x2A15 from here was cleared before use (user: the camera no longer moved). While he stands or turns to the
// camera, the pad runs the mouse code too (that je becomes nops, with the same patches as the mouse); the right stick's
// horizontal axis is hidden from the game (hooks::HidePadRightX) and turns P+0x2A10 at the game's pad turn speed.
// The camera's use of P+0x2A10 (VA 0xB74530, in the player update VA 0xB74040) is also skipped on a pad (jne at VA
// 0xB744D6 after the pad test VA 0x799AF0; traced: P+0x2A15 set by the mouse code but never read): nops too.
// Standing includes the turns in place 7 / 8 (the right stick turned him before it was hidden: he stayed turning).
// Moving, the game copies the body's yaw into P+0x2A10 every frame (traced: VA 0xB8AFBB, 528 writes, the path taken
// when P+0x2D84 bit 5 is clear) and the camera came back behind Chris (user: "Chris must turn to catch the camera"):
// while he turns to it that store is dropped (fstp st(0)), so the camera keeps its yaw until he faces it.
mem::Patch g_freeCamResync("free camera: no resync", 0x78ADAD, { 0x75 }, { 0xEB });
mem::Patch g_freeCamTurn("free camera: no turn", 0x78AFD4, { 0x76 }, { 0xEB });
mem::Patch g_freeCamKeep("free camera: keep the camera yaw", 0x78AFBB, { 0xD9, 0x9E, 0x10, 0x2A, 0x00, 0x00 },
                         { 0xDD, 0xD8, 0x90, 0x90, 0x90, 0x90 });
mem::Patch g_freeCamPadMouse("free camera: the pad runs the mouse code", 0x78AD36,
                             { 0x0F, 0x84, 0xB3, 0x02, 0x00, 0x00 }, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 });
mem::Patch g_freeCamPadCamera("free camera: the pad's camera follows P+0x2A10", 0x7744D6,
                              { 0x0F, 0x85, 0x5E, 0x06, 0x00, 0x00 }, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 });
// "Déplacement selon la direction (RE6)": the mouse code's moving path (je at VA 0xB8AD92 when P+0x2D84 bit 5 is clear:
// P+0x2A10 = the body, the mouse steers him) never taken, so moving too the mouse turns only the camera.
mem::Patch g_locoStandPath("RE6 locomotion: the mouse code's standing path", 0x78AD92, { 0x0F, 0x84, 0x07, 0x02, 0x00, 0x00 },
                           { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 });
// Reloads, weapon changes, heals and dodges do not run the mouse code, so P+0x2A15 stays clear and the player update
// (VA 0xB74530) gives the camera the body's yaw: it came back behind him (user, 2026-10-24). There the camera's yaw is
// P+0x2A10 whatever P+0x2A15 (je at VA 0xB74537 nopped), and the resync before it (P+0x2A10 = body when P+0x2A16
// differs from unit +0x18 != 0, je at VA 0xB7451D) is jumped over.
mem::Patch g_locoCamYaw("RE6 locomotion: the camera follows P+0x2A10", 0x774537, { 0x74, 0x11 }, { 0x90, 0x90 });
mem::Patch g_locoCamNoResync("RE6 locomotion: no camera resync", 0x77451D, { 0x74, 0x11 }, { 0xEB, 0x11 });
std::atomic<bool> g_locoOn{ false };
// Their options on, set aside in the first-person view (user, 2026-11-01: "ignore the free idle camera and the RE6
// locomotion in first person, it is weird otherwise"): released once, taken up again after.
bool LocoOn() { return g_locoOn.load(std::memory_order_relaxed) && !FirstPersonOn(); }
bool g_freeCamPatched = false, g_freeCamWasFree = false, g_aligning = false;
float g_freeYaw = 0.0f;  // the camera's yaw (P+0x2A10) while he stood
constexpr float kAlignSpeed = 12.0f;  // rad/s: half a turn in about a quarter of a second
constexpr float kPadTurnSpeed = 2.75f;  // rad/s at full tilt: the game's own pad turn (recorded)

void SetFreeCamPatches(bool on) {
    if (on == g_freeCamPatched) return;
    if (on ? mem::ApplyAll({ &g_freeCamResync, &g_freeCamTurn }) : mem::RestoreAll({ &g_freeCamResync, &g_freeCamTurn }))
        g_freeCamPatched = on;
}

void SetKeepCameraYaw(bool on) {
    if (on != g_freeCamKeep.IsApplied()) on ? g_freeCamKeep.Apply() : g_freeCamKeep.Restore();
}

void SetPadMouseCode(bool on) {
    if (on != g_freeCamPadMouse.IsApplied())
        on ? mem::ApplyAll({ &g_freeCamPadMouse, &g_freeCamPadCamera }) : mem::RestoreAll({ &g_freeCamPadMouse, &g_freeCamPadCamera });
}

std::atomic<bool> g_freeIdleOn{ false };  // "Caméra libre à l'arrêt"
bool FreeIdleOn() { return g_freeIdleOn.load(std::memory_order_relaxed) && !FirstPersonOn(); }  // as LocoOn

// Driving the airboat (action 0x6A, user, 2026-10-28: "with the free camera I want to move it"): the game keeps the
// camera behind the boat. The player update (VA 0xB74500) gives the camera his local yaw (from his rotation, local to
// the boat: P+0x18) unless P+0x2A15 is set, then P+0x2A10 (traced driving: read at VA 0xB74539 every frame); P+0x2A10
// is first reset to the body when P+0x2A16 differs from "has a parent" (je at VA 0xB7451D), and the game's mouse code
// copies the body's yaw into it every frame (VA 0xB8AFBB, its moving path: traced driving, 1223 writes in 10 s). With
// the free camera or the RE6 locomotion on, that copy is dropped (the free camera's g_freeCamKeep), the mouse counts
// (the game's ratio, TurnPerCount) or the right stick turn P+0x2A10, P+0x2A15 and P+0x2A16 set: the camera turns about
// Chris, in the boat's frame, and stays where it was put. Every frame (render thread); while it drives it takes the
// mouse counts (TickAimWalk and the RE6 locomotion leave them).
void TickBoatCamera() {
    static LARGE_INTEGER last{};
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    float yaw = 0.0f;
    if (!p || !mem::Read(p + 0x10E0, action) || action != 0x6A || actors::OnFoot(p) || !mem::Read(p + 0x2A10, yaw) ||
        !(FreeIdleOn() || LocoOn())) {
        if (g_boatCamera) {  // out of the driving: the free camera / RE6 locomotion manage it again
            SetKeepCameraYaw(false);
            g_boatCamera = false;
        }
        return;
    }
    SetKeepCameraYaw(true);
    g_boatCamera = true;
    const long dx = hooks::TakeRawMouseX();
    float delta = TurnPerCount() * static_cast<float>(dx);
    if (PadInUse(p)) delta -= kPadTurnSpeed * hooks::PadRightX() * dt;
    mem::Write(p + 0x2A10, Wrap(yaw + delta));
    mem::Write<uint8_t>(p + 0x2A15, 1);
    mem::Write<uint8_t>(p + 0x2A16, 1);
}

// Chris's body only (quaternion and P+0x1024): P+0x2A10, the mouse's yaw, is the one the camera follows.
void SetBodyYaw(uintptr_t p, float yaw) {
    yaw = Wrap(yaw);
    mem::Write(p + 0x1024, Wrap(yaw + ParentYaw(p)));  // the world's (see ParentYaw)
    const float quat[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x40), quat, sizeof(quat));
}

void TickFreeCamera() {
    if (LocoOn()) return;  // the RE6 locomotion runs the camera then
    static LARGE_INTEGER last{};
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    dt *= Player1TimeSpeed();
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action)) {
        SetFreeCamPatches(false);
        SetKeepCameraYaw(false);
        SetPadMouseCode(false);
        hooks::HidePadRightX(false);
        g_freeCamWasFree = g_aligning = false;
        return;
    }
    const bool pad = PadInUse(p);
    bool held = false;
    if (pad) {
        float stick[2]{};
        const uintptr_t input = InputBlock(p);
        held = input && mem::SafeRead(reinterpret_cast<void*>(input + 0x410), stick, sizeof(stick)) &&
               stick[0] * stick[0] + stick[1] * stick[1] >= 0.25f * 0.25f;
    } else {
        held = GameButtonHeld(0x00, Player1Pad()) || GameButtonHeld(0x01, Player1Pad()) || GameButtonHeld(0x02, Player1Pad()) || GameButtonHeld(0x03, Player1Pad());
    }
    // Not on RE6's ground (user, 2026-11-10: with an ammunition box lying down "cut the free camera at rest too, so that Chris pivots with the
    // camera as when aiming; I could turn the camera without him"; the game gets no direction there, so it always looked like a stand).
    const bool standing = (action == 0x00 || (pad && (action == 0x07 || action == 0x08))) && !Re6OnGround();
    if (standing && !held) {
        float yaw = 0.0f;
        if (pad) {
            // P+0x2A10 is stale on a pad: from the body when the stand starts, then as the mouse code left it.
            if (!g_freeCamWasFree) g_freeYaw = QuatYaw(p);
            else if (mem::Read(p + 0x2A10, yaw)) g_freeYaw = yaw;
            hooks::HidePadRightX(true);
            g_freeYaw = Wrap(g_freeYaw - kPadTurnSpeed * hooks::PadRightX() * dt);
            mem::Write(p + 0x2A10, g_freeYaw);
        } else if (mem::Read(p + 0x2A10, yaw)) {
            g_freeYaw = yaw;
        }
        SetKeepCameraYaw(false);
        SetFreeCamPatches(true);
        SetPadMouseCode(pad);
        g_freeCamWasFree = true;
        g_aligning = false;
        return;
    }
    SetFreeCamPatches(false);
    hooks::HidePadRightX(false);
    const float target = g_freeYaw;
    const bool moving = action == 0x00 || (action >= 0x02 && action <= 0x06);  // wait, walks, run, back
    // The aim starts where the camera looks (its camera follows the aim yaw P+0x2A24, MODLOG session 14).
    auto aimAtCamera = [&] {
        SetBodyYaw(p, target);
        mem::Write(p + 0x2A10, target);
        WriteAimYaw(p, target);
        SetKeepCameraYaw(false);
        SetPadMouseCode(false);
        g_aligning = false;
    };
    if (g_freeCamWasFree) {
        g_freeCamWasFree = false;
        if (AimAction(action)) {
            aimAtCamera();
            return;
        }
        if (moving && held) {
            g_aligning = true;
            SetKeepCameraYaw(true);
            mem::Write(p + 0x2A10, target);  // in case the game copied the body already
        }
    }
    if (!g_aligning) {
        SetPadMouseCode(false);
        return;
    }
    if (AimAction(action)) {
        aimAtCamera();
        return;
    }
    if (!moving) {  // another action (melee, quick turn...): the game's camera again
        SetKeepCameraYaw(false);
        SetPadMouseCode(false);
        g_aligning = false;
        return;
    }
    const float body = QuatYaw(p), diff = Wrap(target - body), step = kAlignSpeed * dt;
    if (std::fabs(diff) <= step) {
        SetBodyYaw(p, target);
        SetKeepCameraYaw(false);
        SetPadMouseCode(false);
        g_aligning = false;
        return;
    }
    SetBodyYaw(p, body + (diff > 0.0f ? step : -step));
}

// "Déplacement selon la direction (RE6)" (user, 2026-10-24): Chris goes where the direction points from the camera, as
// in RE6 (left = to the screen's left, back = towards the camera), turning to it; the camera stays where the mouse /
// right stick put it, moving or not. While he stands, walks or runs (actions 0x00, 0x02..0x08) and does not aim:
// - camera: the free camera's patches (no resync, no turn mask, on a pad the mouse code and P+0x2A10 for the camera,
//   the right stick's X hidden and turning P+0x2A10) and the mouse code's standing path always taken;
// - the game sees forward only (keyboard: any direction button reads as forward, quick_knife.cpp; pad: the left stick
//   pushed straight up with its tilt, dinput_hook.cpp), so it walks / runs ahead with its own speeds and motions;
// - his body (quaternion +0x40, P+0x1024) turns to the camera's yaw P+0x2A10 + the direction's angle at the slider's
//   speed. An aim starts where the camera looks (as the free camera); the other actions get the game's camera back.
bool g_locoActive = false;                     // render thread: its patches and overrides on
float g_keptCamera = 0.0f;                     // P+0x2A10 as left by the last kept frame (the game's mouse code check)
bool g_keptCameraKnown = false;
// The speeds left to the game (user, 2026-10-24: RE6 sprints like a tank as RE5 runs, RE4 / RE5 strafe walking): 0 walk,
// 1 run, 2 sprint. Chosen from what the player asks for (the trainer's sprint, the run button or the run toggle), not
// from the game's action: a walk left to the game backs off with "back" and back + run is its quick turn, never a run.
std::atomic<bool> g_locoNative[3]{};
bool g_locoAligning = false;  // leaving the RE6 mode: he turns to the camera, which keeps its yaw (as the free camera)
float g_locoAlignTo = 0.0f;
// The quick turn's input turns the camera to where he goes instead, behind him standing (user, 2026-10-24: what RE6
// does in place of the quick turn), at kRecenterSpeed: on the keyboard the game's quick turn key (its request 0x09,
// refused by action_log.cpp, is noted here) and on both back + a real press of run (the run toggle's forced run does
// not count: user, back while it ran turned the camera). His direction in the world is then held while the same
// direction stays held (else "back", now towards the new camera, would turn him round again); released or changed, the
// directions are the camera's again.
constexpr float kRecenterSpeed = 10.0f;  // rad/s: half a turn in about a third of a second
std::atomic<ULONGLONG> g_quickTurnAsked{ 0 };  // GetTickCount64 of the game's last quick turn request (game thread)
bool g_wasRun = false;
bool g_recentering = false, g_latched = false;
float g_recenterTo = 0.0f, g_latchYaw = 0.0f, g_latchForward = 0.0f, g_latchLeft = 0.0f;

bool LocomotionAction(uint32_t action) { return action == 0x00 || (action >= 0x02 && action <= 0x08); }

void SetLocoStandPath(bool on) {
    if (on != g_locoStandPath.IsApplied()) on ? g_locoStandPath.Apply() : g_locoStandPath.Restore();
}

void SetLocoCameraKept(bool on) {
    if (on != g_locoCamYaw.IsApplied())
        on ? mem::ApplyAll({ &g_locoCamYaw, &g_locoCamNoResync }) : mem::RestoreAll({ &g_locoCamYaw, &g_locoCamNoResync });
}

// The first person's turns as fast as the aim's (user, 2026-11-05: "turning, Chris is not fast enough to catch the
// normal camera: turning too fast I see him from the front; aiming it never happens"). The mouse code (VA 0xB8AD2F..
// 0xB8AFE7): standing it adds the mouse to P+0x2A10, which the camera follows, and the body catches it through the
// turns in place 7 / 8; moving it copies the body into P+0x2A10 and the mouse only steers him. In the first person out
// of the aim, with the keyboard and mouse, in the moves 0x00 / 0x02..0x08: the free camera's patches (no resync, no turn
// in place) and the RE6 locomotion's standing path (the mouse turns P+0x2A10 while moving too), and right after the
// mouse code - its call of VA 0xB6A820 at VA 0xB8AFF1, where its paths and the pad's skip meet, redirected to
// AfterMouseStub - the body takes P+0x2A10 in the same update: his skeleton, the camera and the picture agree.
std::atomic<bool> g_fpTurn{ false };  // set by the render thread while the patches are in
// "Rotation comme en visée" (user, 2026-11-05: "an option so he turns as when aiming in the third person too, the first
// person does it by itself now; ignored with the free camera standing of course"): the same out of the first person,
// standing aside for the free camera standing and the RE6 locomotion (they run the camera and the body themselves).
std::atomic<bool> g_turnLikeAim{ false };
void* g_afterMouseNext = nullptr;     // VA 0xB6A820
mem::Patch* g_afterMouseCall = nullptr;

// The feet's steps when the first person turns him standing (user, 2026-11-05: "aiming he has foot animations when he
// turns, here none: give him his turning steps, more natural when I look down"). The game's turns in place (actions 7 /
// 8) play 0x?09 (left, the yaw growing) / 0x?0A (right) on layer 0 in the held weapon's bank (0x509 / 0x50A with a
// handgun), 2.2 degrees a frame, 81 frames for 178 degrees, their frame following the turn, not the time (recorded,
// research/fp_turns/turn_in_place_layers.txt). Standing (action 0) the leg layer plays them, legs only (MaskLegLayer:
// the other joints' tracks and the root's emptied), the frame moved by the turn / 2.2 (looping before its end), from a
// 3 degree turn on. When the turn stops the layer's weight (+0x14, its blend over the layers below) fades to 0 in
// kTurnFadeSeconds, the motion held where it stopped (user, 2026-11-05: given back at once, the legs "teleported" from
// the middle of a step to the end), then the layer gets its motion back; a new turn brings the weight back up.
constexpr float kTurnDegPerFrame = 2.2f, kTurnLoopFrames = 78.0f, kTurnStartDeg = 3.0f;
constexpr float kTurnFadeSeconds = 0.3f, kTurnRiseSeconds = 0.1f;
uint32_t g_turnMotion = 0, g_turnSavedMotion = 0;
uint16_t g_turnSavedMode = 0;
float g_turnFrame = 0.0f, g_turnPending = 0.0f, g_turnWeight = 0.0f;

void EndTurnSteps(uintptr_t p) {
    g_turnPending = 0.0f;
    if (!g_turnSteps) return;
    g_turnSteps = false;
    uint16_t motion = 0;
    if (p && mem::Read(LayerRecord(p, kLegLayer) + 4, motion) && motion == g_turnMotion)
        SetMotion(p, kLegLayer, g_turnSavedMotion, g_turnWeight > 0.05f ? 8.0f : 0.0f, g_turnSavedMode);
    g_turnWeight = 0.0f;
    if (g_legMotion == g_turnMotion) g_legMotion = 0;
    g_turnMotion = 0;
}

void TurnSteps(uintptr_t p, float turnDeg) {
    uint32_t action = 0;
    uintptr_t clock = 0;
    float dt = 0.0f;
    if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB209C)), clock) && clock) mem::Read(clock + 0x24, dt);
    if (!mem::Read(p + 0x10E0, action) || action != 0x00 || g_moving) {  // standing only (the aim walk owns it moving)
        EndTurnSteps(p);
        return;
    }
    const bool turning = std::fabs(turnDeg) > 0.01f;
    if (!g_turnSteps) {
        if (!turning) {
            g_turnPending = 0.0f;
            return;
        }
        g_turnPending += turnDeg;
        if (std::fabs(g_turnPending) < kTurnStartDeg) return;
        turnDeg = g_turnPending;
        g_turnPending = 0.0f;
    } else if (!turning) {  // stopped: the steps fade out where they are
        g_turnWeight -= dt / kTurnFadeSeconds;
        if (g_turnWeight <= 0.0f) {
            EndTurnSteps(p);
            return;
        }
        mem::Write(LayerRecord(p, kLegLayer) + 0x14, g_turnWeight);
        return;
    }
    // The held weapon's bank and a left-handed player's mirror (layer 0's motion and mode bit 0x100: as the aim walk).
    uint16_t baseMotion = 0, baseMode = 0;
    mem::Read(LayerRecord(p, 0) + 4, baseMotion);
    mem::Read(LayerRecord(p, 0) + 0xA, baseMode);
    if (baseMotion == 0xFFFF) return;
    const uint32_t mirror = baseMode & 0x100;
    const bool left = (turnDeg > 0.0f) != (mirror != 0);
    const uint32_t motion = (baseMotion & 0xFF00u) | (left ? 0x09u : 0x0Au);
    if (!g_turnSteps) {
        uint16_t saved = 0, mode = 0;
        mem::Read(LayerRecord(p, kLegLayer) + 4, saved);
        mem::Read(LayerRecord(p, kLegLayer) + 0xA, mode);
        g_turnSavedMotion = saved;
        g_turnSavedMode = mode;
        g_turnSteps = true;
        g_turnWeight = 1.0f;  // SetMotion's own blend brings it in
    }
    if (motion != g_turnMotion) {
        g_turnMotion = motion;
        g_legMotion = motion;  // MaskLegLayer's
        g_turnFrame = 0.0f;
        SetMotion(p, kLegLayer, motion, 6.0f, 0x40 | mirror);
    }
    g_turnFrame += std::fabs(turnDeg) / kTurnDegPerFrame;
    while (g_turnFrame >= kTurnLoopFrames) g_turnFrame -= kTurnLoopFrames;
    g_turnWeight = std::min(1.0f, g_turnWeight + dt / kTurnRiseSeconds);
    mem::Write(LayerRecord(p, kLegLayer) + 0x14, g_turnWeight);
    mem::Write(LayerRecord(p, kLegLayer) + 0x18, g_turnFrame);
    mem::Write(LayerRecord(p, kLegLayer) + 0x24, 0.0f);  // the turn moves it, not the time
}

void __cdecl AfterMouseCode(uintptr_t p) {
    if (!p || p != actors::Player(0)) return;
    if (!g_fpTurn.load(std::memory_order_relaxed)) {
        if (g_turnSteps) EndTurnSteps(p);
        return;
    }
    float yaw = 0.0f;
    if (!mem::Read(p + 0x2A10, yaw) || yaw != yaw) return;
    const float before = QuatYaw(p);
    SetBodyYaw(p, yaw);
    TurnSteps(p, Wrap(yaw - before) * 57.2957795f);
}

__declspec(naked) void AfterMouseStub() {
    __asm {
        push ecx                    // the player (thiscall of VA 0xB6A820 after)
        push ecx
        call AfterMouseCode
        add esp, 4
        pop ecx
        jmp dword ptr [g_afterMouseNext]
    }
}

bool EnsureAfterMouseHook() {
    if (g_afterMouseCall) return g_afterMouseCall->IsApplied() || g_afterMouseCall->Apply();
    uint8_t* site = mem::Rva(0x78AFF1);
    int32_t rel = 0;
    if (site[0] != 0xE8 || !mem::Read(reinterpret_cast<uintptr_t>(site + 1), rel) || site + 5 + rel != mem::Rva(0x76A820)) {
        LOG("first person turn: unexpected call %s", mem::Hex(site, 5).c_str());
        return false;
    }
    g_afterMouseNext = mem::Rva(0x76A820);
    const int32_t stub = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&AfterMouseStub) - reinterpret_cast<uintptr_t>(site + 5));
    std::vector<uint8_t> original(site, site + 5), patched = { 0xE8 };
    for (int i = 0; i < 4; ++i) patched.push_back(static_cast<uint8_t>(stub >> (8 * i)));
    g_afterMouseCall = new mem::Patch("first person turn: after the mouse code", site, original, patched);
    return g_afterMouseCall->Apply();
}

void ReleaseFirstPersonTurn() {
    if (!g_fpTurn.load(std::memory_order_relaxed)) return;
    g_fpTurn = false;
    SetFreeCamPatches(false);
    SetLocoStandPath(false);
}

void TickFirstPersonTurn() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    const bool view = FirstPersonOn() ? FirstPersonHidesSheath(p)
                                      : g_turnLikeAim.load(std::memory_order_relaxed) && !FreeIdleOn() && !LocoOn();
    const bool want = p && view && actors::OnFoot(p) && !PadInUse(p) && mem::Read(p + 0x10E0, action) &&
                      LocomotionAction(action);
    if (!want) {
        ReleaseFirstPersonTurn();
        return;
    }
    if (g_fpTurn.load(std::memory_order_relaxed) || !EnsureAfterMouseHook()) return;
    mem::Write(p + 0x2A10, QuatYaw(p));  // from where he faces (the aim or another action may have turned him)
    SetFreeCamPatches(true);
    SetLocoStandPath(true);
    g_fpTurn = true;
}

// Melee moves (0x30..0x4D, 0x6F, the Reunion 0xAC / 0xAD, the trainer's carrier 0x81), hit reactions and events: the
// game gave the camera the body's yaw there, behind him and locked (user, 2026-10-24). The action ids are the
// classes of player 1's action objects [P+0x10DC + 4 * id] (read live): 0x85 damage_common (the flinch), 0x86..0x88
// blow_s, 0x89 / 0x8A blow_l, 0x8B / 0x8C knock downs, 0x8D..0x94 chair / fire / flash / sulfate / ice / stunrod,
// 0x95 / 0x96 lie waits, 0x97 / 0x98 getups, 0x99 guard, 0x9A..0x9C boatair / syokei_lie / egg (0x9D.. die), 0xAE
// sulfate_l, 0xAF / 0xB0 Wesker's blow / wall hit; events: 0x84 setEventAct_wait (a grab starts), 0x80000000 a motion
// played by an event (Wesker's grab, the counter QTE: recorded 0x84 -> 0x81 -> 0x80000000 F00 -> 0x86 -> 0x97).
bool MeleeMove(uint32_t action) {
    return (action >= 0x30 && action <= 0x4D) || action == 0x6F || action == 0xAC || action == 0xAD || action == 0x81;
}
bool HitReaction(uint32_t action) { return (action >= 0x85 && action <= 0x9C) || (action >= 0xAE && action <= 0xB0); }
bool EventAction(uint32_t action) { return action == 0x84 || action == 0x80000000; }

// The actions the camera stays free through (user, 2026-10-24): reload 0x1F, weapon change 0x20 / 0x21, heals 0x5F..0x62,
// the dodges 0x50..0x55 (free dodge keys included), the hit reactions, and the melee moves when the normal camera is
// kept in melee (camera.keep_in_melee; the game's melee camera otherwise).
bool KeepCameraAction(uint32_t action) {
    return (action >= 0x1F && action <= 0x21) || (action >= 0x5F && action <= 0x62) || (action >= 0x50 && action <= 0x55) ||
           HitReaction(action) || EventAction(action) || (MeleeMove(action) && NormalCameraInMelee());
}

void ReleaseLocomotion() {
    SetLocoCameraKept(false);
    g_locoKeeping = false;
    SetFreeCamPatches(false);
    SetLocoStandPath(false);
    SetKeepCameraYaw(false);
    SetPadMouseCode(false);
    hooks::HidePadRightX(false);
    hooks::PadLeftForward(false);
    ForceDirectionForward(false);
    g_locoActive = false;
    g_locoAligning = false;
    g_recentering = g_latched = false;
}

// The speed player 1 asks for: the trainer's sprint, else the run (pad: the real run button or the run toggle - not the
// game's held mask, whose toggle bit depends on the stick, StickRunsForward; keyboard: the run button through the
// query hook, the forced run included), else the walk.
int LocoSpeed(uintptr_t p, bool pad) {
    if (g_sprinting) return 2;
    if (pad) return g_padRunReal.load(std::memory_order_relaxed) || (g_running && g_toggleRun.load(std::memory_order_relaxed)) ? 1 : 0;
    return GameButtonHeld(0x04, Player1Pad()) || GameButtonHeld(0x04, -1) ? 1 : 0;
}

// The pad mask's run toggle bit (game thread): every way while the RE6 locomotion runs Chris itself (its run not left
// to the game) and he walks / runs / stands.
bool Re6RunAnyDirection() {
    const uintptr_t p = actors::Player(0);
    uint32_t action = 0;
    return LocoOn() && !g_locoNative[1].load(std::memory_order_relaxed) && p &&
           mem::Read(p + 0x10E0, action) && LocomotionAction(action);
}

}  // namespace

// wesker_moves.cpp ("Roulade arrière au sol"): player 1's run button really held (user, 2026-10-26: with the run toggle
// on he always got up rolling): the pad's real run bit, the keyboard's key read past the query hook, as the quick turn.
bool Player1RunHeld() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p) return false;
    return PadInUse(p) ? g_padRunReal.load(std::memory_order_relaxed)
                       : RawButtonHeld(0x04, Player1Pad()) || RawButtonHeld(0x04, -1);
}

namespace {
// A speed left to the game: its own movement, after he turned to the camera if the RE6 mode left them apart.
void TickLocoNative(uintptr_t p, bool pad, float dt) {
    if (g_locoActive) {
        float camera = 0.0f;
        mem::Read(p + 0x2A10, camera);
        LOG("RE6 locomotion: speed %d left to the game (%s)", LocoSpeed(p, pad), pad ? "pad" : "keyboard / mouse");
        ReleaseLocomotion();
        g_locoAlignTo = camera;
        g_locoAligning = std::fabs(Wrap(camera - QuatYaw(p))) > 0.01f;
    }
    if (!g_locoAligning) return;
    SetKeepCameraYaw(true);
    SetPadMouseCode(pad);
    mem::Write(p + 0x2A10, g_locoAlignTo);
    const float body = QuatYaw(p), diff = Wrap(g_locoAlignTo - body), step = kAlignSpeed * dt;
    if (std::fabs(diff) <= step) {
        SetBodyYaw(p, g_locoAlignTo);
        SetKeepCameraYaw(false);
        SetPadMouseCode(false);
        g_locoAligning = false;
        return;
    }
    SetBodyYaw(p, body + (diff > 0.0f ? step : -step));
}

void TickRe6Locomotion() {
    static LARGE_INTEGER last{};
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    dt *= Player1TimeSpeed();
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    // The mouse's raw counts since the last frame: TickMouseTurn takes them when the walk during actions is on.
    long mouseDx = 0;
    if (g_walkOn) {
        mouseDx = g_cameraDx;
        g_cameraDx = 0;
    } else if (!g_boatCamera) {  // TickBoatCamera's while driving the airboat
        mouseDx = hooks::TakeRawMouseX();
    }
    if (!p || !mem::Read(p + 0x10E0, action)) {
        ReleaseLocomotion();
        return;
    }
    // An RE6 quick shot holding him (user, 2026-11-10: the shotgun's shot played again while he walked through it: logged, the walk 2
    // started through the RE6 mode): the RE6 mode stands aside meanwhile.
    if (QuickShotHoldsPlayer()) {
        if (g_locoActive || g_locoAligning || g_locoKeeping) ReleaseLocomotion();
        return;
    }
    // The inventory open (user, 2026-11-06: running with the run toggle on a pad, the left stick turned Chris instead of
    // moving the grid's cursor - the stick shown straight forward to the game, his turn made here): the sticks are the
    // inventory's, the RE6 mode takes him again once it closes.
    if (Player1InventoryOpen()) {
        if (g_locoActive || g_locoAligning || g_locoKeeping) {
            LOG("RE6 locomotion: inventory open, sticks left to it");
            ReleaseLocomotion();
        }
        return;
    }
    const bool pad = PadInUse(p);
    // A reload, weapon change, heal or dodge from the RE6 mode: the camera stays where it looks, his directions are
    // the game's again (the walk during actions strafes) and his turning there keeps the camera's offset.
    // The rocket catch (rocket_catch.cpp) also when it starts from the aim, which is not the RE6 mode (the knife parry's
    // key aims; user, 2026-10-25: the camera turned in the catch "depending on the times"), and the knife slash that
    // sends a rocket back (the knife aim's attack 0x27; user: the camera locked during it).
    const bool rocketSlash = RocketSlashPlaying();
    if ((KeepCameraAction(action) || rocketSlash) && (g_locoActive || g_locoKeeping || RocketCatchPlaying() || rocketSlash)) {
        if (!g_locoKeeping) LOG("RE6 locomotion: camera kept through action %X", action);
        ForceDirectionForward(false);
        hooks::PadLeftForward(false);
        // Melee and hit reactions: the right stick turns the camera alone (the game's mouse code turns P+0x2A10 with
        // the mouse there); reloads, weapon changes and heals turn him with it (TickPadIgnoredTurn / TickMouseTurn).
        const bool cameraAlone = MeleeMove(action) || HitReaction(action) || EventAction(action) || rocketSlash;
        hooks::HidePadRightX(pad && cameraAlone);
        if (pad && cameraAlone) {
            float camera = 0.0f;
            if (!g_locoKeeping && !g_locoActive) camera = QuatYaw(p);
            else mem::Read(p + 0x2A10, camera);
            mem::Write(p + 0x2A10, Wrap(camera - kPadTurnSpeed * hooks::PadRightX() * dt));
        } else if (cameraAlone) {
            // Hit reactions and events skip the game's mouse code (user: the camera went up / down, not sideways):
            // the mouse turns P+0x2A10 here at the game's ratio, unless the game moved it since the last frame (its
            // mouse code ran: melee moves).
            float camera = 0.0f;
            if (mem::Read(p + 0x2A10, camera)) {
                if (g_keptCameraKnown && camera == g_keptCamera && mouseDx) {
                    camera = Wrap(camera + TurnPerCount() * static_cast<float>(mouseDx));
                    mem::Write(p + 0x2A10, camera);
                }
                g_keptCamera = camera;
                g_keptCameraKnown = true;
            }
        }
        SetKeepCameraYaw(false);
        SetFreeCamPatches(true);
        SetLocoStandPath(true);
        SetPadMouseCode(pad);
        SetLocoCameraKept(true);
        g_locoActive = false;
        g_locoKeeping = true;
        return;
    }
    g_keptCameraKnown = false;
    // Not on RE6's ground either (user, 2026-11-10: with an ammunition box he did not aim, so the RE6 locomotion pivoted him lying down).
    if (!LocomotionAction(action) || AimAction(action) || Re6OnGround()) {
        if ((g_locoActive || g_locoAligning || g_locoKeeping) && AimAction(action)) {  // the aim starts where the camera looks
            float camera = 0.0f;
            if (mem::Read(p + 0x2A10, camera)) {
                SetBodyYaw(p, camera);
                WriteAimYaw(p, camera);
            }
        }
        if (g_locoActive || g_locoAligning || g_locoKeeping) ReleaseLocomotion();
        return;
    }
    float forward = 0.0f, left = 0.0f;
    Player1Direction(forward, left);
    const bool held = forward != 0.0f || left != 0.0f;
    if (held && g_locoNative[LocoSpeed(p, pad)].load(std::memory_order_relaxed)) {
        TickLocoNative(p, pad, dt);
        return;
    }
    if (g_locoAligning) {  // back to the RE6 mode before he faced the camera
        SetKeepCameraYaw(false);
        g_locoAligning = false;
    }
    float camera = 0.0f;
    if (!mem::Read(p + 0x2A10, camera)) return;
    if (pad) {
        // P+0x2A10 is stale on a pad until the mouse code runs there: from the body when it starts.
        if ((!g_locoActive && !g_locoKeeping) || !g_freeCamPadMouse.IsApplied()) camera = QuatYaw(p);
        hooks::HidePadRightX(true);
        camera = Wrap(camera - kPadTurnSpeed * hooks::PadRightX() * dt);
        mem::Write(p + 0x2A10, camera);
    }
    SetKeepCameraYaw(false);
    SetLocoCameraKept(false);
    g_locoKeeping = false;
    SetFreeCamPatches(true);
    SetLocoStandPath(true);
    SetPadMouseCode(pad);
    ForceDirectionForward(true);
    hooks::PadLeftForward(true);
    if (!g_locoActive) LOG("RE6 locomotion: on (action %X, %s)", action, pad ? "pad" : "keyboard / mouse");
    g_locoActive = true;
    // The quick turn's input: the camera turns to his direction (held meanwhile), or behind him standing.
    // Back + a real press of run on both (user, 2026-10-24: the keyboard too, besides its key); the keyboard's run key
    // read past the query hook, so the run toggle's forced run does not count.
    const bool runPressed = pad ? g_padRunReal.load(std::memory_order_relaxed)
                                : RawButtonHeld(0x04, Player1Pad()) || RawButtonHeld(0x04, -1);
    const bool backRun = runPressed && !g_wasRun && held && forward < -0.7f && std::fabs(left) < 0.7f;
    g_wasRun = runPressed;
    if (backRun) g_runPressQuickTurn = true;  // this press does not stop the run toggle
    const ULONGLONG asked = g_quickTurnAsked.exchange(0);
    const bool keyQuickTurn = !pad && asked && GetTickCount64() - asked < 250;
    if (backRun || keyQuickTurn) {
        if (held) {
            g_recenterTo = g_latchYaw = Wrap(camera + std::atan2(left, forward));
            g_latchForward = forward;
            g_latchLeft = left;
            g_latched = true;
        } else {
            g_recenterTo = QuatYaw(p);
            g_latched = false;
        }
        g_recentering = true;
        LOG("RE6 locomotion: quick turn input (%s), the camera turns %s", backRun ? "back + run" : "quick turn key",
            held ? "to his direction" : "behind him");
    }
    if (g_latched && (!held || forward * g_latchForward + left * g_latchLeft <
                                   0.9f * std::hypot(forward, left) * std::hypot(g_latchForward, g_latchLeft))) {
        g_latched = false;
        LOG("RE6 locomotion: direction latch released (held %d, forward %.2f, left %.2f)", held, forward, left);
    }
    if (g_recentering) {
        const float diff = Wrap(g_recenterTo - camera), turn = kRecenterSpeed * dt;
        if (std::fabs(diff) <= turn) {
            camera = g_recenterTo;
            g_recentering = false;
        } else {
            camera = Wrap(camera + (diff > 0.0f ? turn : -turn));
        }
        mem::Write(p + 0x2A10, camera);
    }
    if (!held) return;  // standing: the camera alone turns
    const float target = g_latched ? g_latchYaw : Wrap(camera + std::atan2(left, forward));
    const float body = QuatYaw(p), diff = Wrap(target - body);
    const float step = g_locoTurnSpeed.load(std::memory_order_relaxed) * 0.0174533f * dt;
    SetBodyYaw(p, std::fabs(diff) <= step ? target : body + (diff > 0.0f ? step : -step));
}
}  // namespace

// action_log.cpp: no quick turn at all while the RE6 locomotion is on (user, 2026-10-24: RE6 has none, the speeds left
// to the game included); turn_180 0x09 is refused.
bool Re6LocomotionOn() { return LocoOn(); }
// action_log.cpp: the game asked for a quick turn (refused): its input turns the camera in the RE6 mode.
void Re6QuickTurnAsked() {
    const ULONGLONG now = GetTickCount64();
    g_quickTurnAsked.store(now ? now : 1, std::memory_order_relaxed);
}

namespace {
// Aim sensitivity (user, 2026-10-24: faster than the game allows, one slider for the mouse, one for the pad). Game
// parameters [RVA 0xDB27B4]: +0x450 (0.001) multiplies the mouse's movement (mouse turn VA 0xB8AD79, the aim at VA
// 0xB8BAB8 / 0xB8BEFE / 0xB8C073): scaled while aiming only (gun or knife); +0x2D0 / +0x2D4 (57.9 each) are the pad
// aim's horizontal / vertical speeds (VA 0xB87802, multiplied by the stick and the frame time at VA 0xB87904 /
// 0xB8790F; measured 74 degrees / s at full tilt): scaled while aiming too. The game's own values are kept as the base
// and given back; a value the game sets itself (options) becomes the new base.
struct ScaledParam {
    uint32_t offset;
    float base = 0.0f, written = 0.0f;
    bool known = false;
};
ScaledParam g_mouseSens{ 0x450 }, g_padAimYaw{ 0x2D0 }, g_padAimPitch{ 0x2D4 };
std::atomic<bool> g_mouseSensOn{ false }, g_padSensOn{ false };
std::atomic<float> g_mouseSensFactor{ 1.5f }, g_padSensFactor{ 1.5f };

uintptr_t GameParams() {
    uintptr_t params = 0;
    return mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB27B4)), params) ? params : 0;
}

void ScaleParam(ScaledParam& param, float factor) {
    const uintptr_t params = GameParams();
    float current = 0.0f;
    if (!params || !mem::Read(params + param.offset, current)) return;
    if (!param.known || (current != param.written && current != param.base)) {
        param.base = current;
        param.known = true;
    }
    const float value = param.base * factor;
    if (current != value) mem::Write(params + param.offset, value);
    param.written = value;
}

void RestoreParam(ScaledParam& param) {
    const uintptr_t params = GameParams();
    float current = 0.0f;
    if (param.known && params && mem::Read(params + param.offset, current) && current == param.written)
        mem::Write(params + param.offset, param.base);
    param.known = false;
}

void TickAimSensitivity() {
    if (!actors::InGame()) return;
    const uintptr_t p = actors::Player(0);
    uint32_t action = 0;
    const bool aiming = p && mem::Read(p + 0x10E0, action) && AimAction(action);
    if (g_mouseSensOn.load(std::memory_order_relaxed))
        ScaleParam(g_mouseSens, aiming ? g_mouseSensFactor.load(std::memory_order_relaxed) : 1.0f);
    if (g_padSensOn.load(std::memory_order_relaxed)) {
        // Pad only: the mouse aim reads them too (VA 0xCAEF34, times the frame time) and doubled they made it diverge
        // (user, 2026-10-24: Chris spun every way aiming with the mouse; recorded the turn growing x1.6 every 80 ms).
        const float factor = aiming && PadInUse(p) ? g_padSensFactor.load(std::memory_order_relaxed) : 1.0f;
        ScaleParam(g_padAimYaw, factor);
        ScaleParam(g_padAimPitch, factor);
    }
}
}  // namespace

void RegisterMovement() {
    {
        Feature& mouse = Add({ .id = "player.aim_sensitivity_mouse", .name = "Sensibilité de la visée (souris)",
                               .help = "Multiplie la vitesse de la souris pendant la visée (arme ou couteau). Hors visée, "
                                       "la souris garde la vitesse du jeu.",
                               .category = "Joueur", .kind = Kind::Toggle,
                               .set = [](bool on) {
                                   g_mouseSensOn = on;
                                   if (!on) RestoreParam(g_mouseSens);
                                   return true;
                               },
                               .available = [] { return game::IsSupported(); },
                               .value = 1.5f, .valueMin = 0.1f, .valueMax = 5.0f, .valueFormat = "x%.2f" });
        Feature* const pm = &mouse;
        mouse.tick = [pm] {
            g_mouseSensFactor.store(pm->value, std::memory_order_relaxed);
            TickAimSensitivity();
        };
        Feature& pad = Add({ .id = "player.aim_sensitivity_pad", .name = "Sensibilité de la visée (manette)",
                             .help = "Multiplie la vitesse de la visée au stick droit (horizontale et verticale), au-delà "
                                     "du maximum du jeu (environ 74 degrés par seconde).",
                             .category = "Joueur", .kind = Kind::Toggle,
                             .set = [](bool on) {
                                 g_padSensOn = on;
                                 if (!on) {
                                     RestoreParam(g_padAimYaw);
                                     RestoreParam(g_padAimPitch);
                                 }
                                 return true;
                             },
                             .available = [] { return game::IsSupported(); },
                             .value = 1.5f, .valueMin = 0.1f, .valueMax = 4.0f, .valueFormat = "x%.2f" });
        Feature* const pp = &pad;
        pad.tick = [pp] {
            g_padSensFactor.store(pp->value, std::memory_order_relaxed);
            if (!g_mouseSensOn.load(std::memory_order_relaxed)) TickAimSensitivity();
        };
    }
    Add({ .id = "player.turn_like_aim", .name = "Rotation comme en visée",
          .help = "Hors visée, la souris tourne Chris aussitôt, comme en visée : il ne pivote plus sur place en retard sur la caméra, et en marchant il tourne avec la souris au lieu d'être dirigé lentement. À l'arrêt, ses pas de pivot suivent la rotation. Clavier et souris (à la manette, la caméra suit déjà Chris). Toujours actif à la première personne. Sans effet avec « Caméra libre à l'arrêt » ou « Déplacement selon la direction (RE6) ».",
          .category = "Caméra", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_turnLikeAim = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "camera.free_idle", .name = "Caméra libre à l'arrêt",
          .help = "Quand Chris ne bouge pas, la souris (ou le stick droit) fait tourner la caméra autour de lui sans le "
                  "faire pivoter. Dès qu'il se déplace, il pivote pour rattraper la caméra. En visant, il vise où "
                  "regarde la caméra, puis la visée reste celle du jeu.",
          .category = "Caméra", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_freeIdleOn = on;
              if (!on) {
                  SetFreeCamPatches(false);
                  SetKeepCameraYaw(false);
                  SetPadMouseCode(false);
                  hooks::HidePadRightX(false);
                  g_freeCamWasFree = g_aligning = false;
              }
              return true;
          },
          .tick = [] {
              static bool aside = false;  // first person: released once
              if (FirstPersonOn()) {
                  if (!aside) {
                      SetFreeCamPatches(false);
                      SetKeepCameraYaw(false);
                      SetPadMouseCode(false);
                      hooks::HidePadRightX(false);
                      g_freeCamWasFree = g_aligning = false;
                      aside = true;
                  }
                  return;
              }
              aside = false;
              TickFreeCamera();
          },
          .available = [] { return game::IsSupported(); } });
    Feature& loco = Add({ .id = "player.re6_locomotion", .name = "Déplacement selon la direction (RE6)",
                          .help = "Comme dans RE6 : Chris va dans la direction choisie par rapport à la caméra (gauche = "
                                  "vers la gauche de l'écran, arrière = vers la caméra) en se tournant vers elle, et la "
                                  "caméra reste où la souris ou le stick droit la met, qu'il bouge ou non. Clavier et "
                                  "manette. En visant, il vise où regarde la caméra, puis la visée reste celle du jeu. Le "
                                  "curseur règle la vitesse à laquelle il se tourne (degrés par seconde). Pas de "
                                  "demi-tour rapide tant qu'elle est cochée (il n'existe pas dans RE6) : arrière + "
                                  "course, ou la touche de demi-tour du clavier, fait pivoter la caméra vers où va "
                                  "Chris, derrière lui à l'arrêt.",
                          .category = "Joueur", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = 720, .valueMin = 180, .valueMax = 1800, .valueFormat = "%.0f deg/s" });
    Feature* const pl = &loco;
    loco.set = [pl](bool on) {
        if (on && (!EnsurePadQueryHooks() || !EnsurePlayerRequestHook())) return false;
        g_locoTurnSpeed = pl->value;
        g_cameraDx = 0;  // counts left while it was off
        g_locoOn = on;
        if (!on) ReleaseLocomotion();
        g_freeCamWasFree = g_aligning = false;
        return true;
    };
    loco.tick = [pl] {
        g_locoTurnSpeed.store(pl->value, std::memory_order_relaxed);
        static bool aside = false;  // first person: released once
        if (FirstPersonOn()) {
            if (!aside) ReleaseLocomotion();
            aside = true;
            return;
        }
        aside = false;
        TickRe6Locomotion();
    };
    OnRestore(ReleaseLocomotion);
    // The mask builder's call into the DLL always goes back at the unload, whichever option still wants it (2026-11-08:
    // ReleaseMaskHook keeps it while the pad's mouse way is wanted; the game then called the freed stub and crashed).
    OnRestore([] {
        // The leg layer's overlays given back (a hot reload left a quick shot's motion there, frozen, 2026-11-10).
        if (g_shotLayer || g_upperOverlay) {
            g_shotLayer = false;
            g_upperOverlay = false;
            RestoreLegLayer(actors::InGame() ? actors::Player(0) : 0);
        }
        g_groundLegs = false;
        g_padMouseWay = false;
        g_padMouseWayNow = false;
        g_mouseLaserWay = false;
        g_mouseLaserNow = false;
        if (g_maskCall) g_maskCall->Restore();
    });
    OnFrame(TickFirstPersonTurn);  // the first person turns as fast as the aim (keyboard / mouse)
    OnRestore([] {
        ReleaseFirstPersonTurn();
        if (g_afterMouseCall) g_afterMouseCall->Restore();
    });
    OnFrame(TickBoatCamera);  // the airboat's driver turns the camera (free camera / RE6 locomotion)
    struct NativeSpeed {
        const char* id;
        const char* name;
        const char* help;
    };
    static constexpr NativeSpeed kNativeSpeeds[] = {
        { "player.re6_locomotion_game_walk", "Déplacement RE6 : marche du jeu (pas de côté)",
          "Grisée sans « Déplacement selon la direction (RE6) ». En marchant (sans la touche de course), Chris se déplace "
          "comme dans le jeu de base : pas de côté, recul, caméra derrière lui (comme RE4 et RE5)." },
        { "player.re6_locomotion_game_run", "Déplacement RE6 : course du jeu (tank)",
          "Grisée sans « Déplacement selon la direction (RE6) ». En courant (touche de course ou course verrouillée, "
          "sans le sprint), Chris court comme dans le jeu de base : en tank, la souris ou le stick le dirige, caméra "
          "derrière lui." },
        { "player.re6_locomotion_game_sprint", "Déplacement RE6 : sprint du jeu (tank)",
          "Grisée sans « Déplacement selon la direction (RE6) ». En sprintant (« Sprint (tenir la course) »), Chris "
          "sprinte en tank comme dans RE6 : la souris ou le stick le dirige, caméra derrière lui." },
    };
    for (int k = 0; k < 3; ++k)
        Add({ .id = kNativeSpeeds[k].id, .name = kNativeSpeeds[k].name, .help = kNativeSpeeds[k].help,
              .category = "Joueur", .kind = Kind::Toggle,
              .set = [k](bool on) {
                  g_locoNative[k] = on;
                  return true;
              },
              .available = [] { return game::IsSupported() && g_locoOn.load(std::memory_order_relaxed); } });
    Feature& walk = Add({ .id = "player.aim_walk", .name = "Marcher pendant les actions (prototype)",
                          .help = "Les directions font marcher Chris (en pas de côté, il reste tourné du même côté) "
                                  "pendant la visée (début et fin compris), le tir, les lancers, le rechargement, le "
                                  "changement d'arme, les soins, la visée et les coups de couteau. Jambes en marche, haut "
                                  "du corps dans l'action. Les collisions du jeu s'appliquent. Le curseur règle la "
                                  "vitesse.",
                          .category = "Joueur", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = 120, .valueMin = 30, .valueMax = 400, .valueFormat = "%.0f" });
    Feature* const pw = &walk;
    walk.set = [](bool on) {
        g_walkOn = on;
        if (on) return EnsureLayerSetupHook() && EnsureMaskHook();
        ReleaseMaskHook();
        RestoreLegLayer(actors::Player(0));
        if (g_setupCall) g_setupCall->Restore();
        return true;
    };
    walk.tick = [pw] {
        g_speed.store(pw->value, std::memory_order_relaxed);
        TickAimWalk();
    };
    Feature& run = Add({ .id = "player.action_run", .name = "Courir pendant les actions",
                         .help = "Avec « Marcher pendant les actions » : la touche de course fait courir Chris (en "
                                 "avançant) pendant le rechargement, le changement d'arme et les soins. En visant, il "
                                 "marche toujours. Le curseur règle la vitesse de course (426 = celle du jeu).",
                         .category = "Joueur", .kind = Kind::Toggle,
                         .available = [] { return game::IsSupported(); },
                         .value = 426, .valueMin = 200, .valueMax = 700, .valueFormat = "%.0f" });
    Feature* const pr = &run;
    run.set = [](bool on) {
        g_actionRun = on;
        return true;
    };
    run.tick = [pr] { g_runSpeed.store(pr->value, std::memory_order_relaxed); };
    Add({ .id = "player.aim_run", .name = "Courir aussi en visant",
          .help = "Avec « Marcher pendant les actions » : la touche de course fait aussi courir Chris (en avançant) "
                  "pendant la visée, son démarrage, le tir et le couteau. Vitesse : le curseur de « Courir pendant les "
                  "actions ». Active « Pas de rechargement avec la course », sinon viser + courir recharge.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_aimRun = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "player.toggle_run", .name = "Course en bascule",
          .help = "Un appui bref sur la touche de course lance la course, un autre l'arrête (plus besoin de la tenir). "
                  "Avec « Sprint », tenir la touche fait sprinter.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsurePadQueryHooks() || !EnsureMaskHook())) return false;
              g_toggleRun = on;
              if (!on) ReleaseMaskHook();
              if (!on) StopToggleRun();
              return true;
          },
          .tick = TickToggleRun,
          .available = [] { return game::IsSupported(); } });
    Feature& sprint = Add({ .id = "player.sprint", .name = "Sprint (tenir la course)",
                            .help = "Avec « Course en bascule » : tenir la touche de course plus d'un quart de seconde fait "
                                    "sprinter Chris (course et animation accélérées ; pas pendant les actions ni en mourant) ; la relâcher "
                                    "revient à la course. Le curseur règle l'accélération.",
                            .category = "Joueur", .kind = Kind::Toggle,
                            .available = [] { return game::IsSupported(); },
                            .value = 3.0f, .valueMin = 1.1f, .valueMax = 4.0f });  // x3 by default (user)
    Feature* const ps = &sprint;
    sprint.set = [](bool on) {
        g_sprintOn = on;
        if (!on) {
            g_sprinting = false;
            SetSprintScale(0, false);
        }
        return true;
    };
    sprint.tick = [ps] { g_sprintFactor.store(ps->value, std::memory_order_relaxed); };
    Add({ .id = "player.low_ready_re6", .name = "Pose basse au pistolet (RE6)",
          .help = "Hors visée, un pistolet en main : le haut du corps prend la pose basse de RE6 (arme tenue vers le bas, "
                  "comme Leon), les jambes restent celles du jeu (à l'arrêt, en marchant, en courant ; pas en sprint). "
                  "Le fichier vient de tools/import_re6_motions.py.",
          .category = "Joueur", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureLayerSetupHook()) return false;
              if (on && !EnsureSetMotionHook()) LOG("low ready: no SetMotion hook, the game's own leg layer will flash");
              g_lowReadyOn = on;
              if (!on && g_upperOverlay) RestoreLegLayer(actors::Player(0));
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnFrame(TickLowReadyAlone);
}
}  // namespace features
