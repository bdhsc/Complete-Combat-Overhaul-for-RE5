// Obstacles climbed alone (user, 2026-11-01, first the room of the two containers raised by shooting the other one's
// switch, 0x1F4: "Chris has two native climbs, a small and a big one", then "I'd rather you played the Majini's
// animation, more natural, which jumps first to grab the ledge", "the prompt only while the container is up, and the
// two-person action with Sheva removed", "a prompt box big enough to notice it when passing by", "you took the climb the
// Majini use for the fence: use their jump when there is no intermediate step", "Chris also over the obstacle where I
// am, with the same animation": a container standing on the ground, uOm0926_Container, 2.50 m high, no lift; then
// "it hangs halfway and slides up: it should jump, grab the top at once, then pull itself up"; then "obstacles, not
// containers: some will not be containers", "the second one as a separate option, optional obstacles: when we see an
// obstacle I will tell you which of the two it goes in", "keep the fence animation: for some obstacles it will fit
// better"). The obstacles are kObstacles' classes, each in a group (one option each) and with its animation.
// The game's climb up a ledge: prompt kind 0x26 (icon 7) of the players' prompt table (built on the stack at VA
// 0xB7D7BC), condition VA 0xB5CF00 (cdecl(?, player index) -> bool) on the player's ledge probe block +0x2AB0 (filled
// every frame by the collision query VA 0x76FF80 from VA 0xB78E28, straight ahead of him): +0x2B50 bit 0x200 (the
// wall's collision says "climb up"), +0x2AB0 / +0x2AB4 / +0x2AB8 set, the top +0x2AE4 at least 40 cm ([VA
// 0x11B27B4]+0x33C) from his feet, then VA 0xB5CB60 (the AI partner's reach only). Its handler VA 0xB6C7C0 (cdecl(owner,
// player index, ...)): +0x2E10 the wall point +0x2AD0 at his height, +0x2E20 the yaw of the wall's plane +0x2B30 /
// +0x2B38, +0x2E24 the height (top - feet), +0x2E28 = +0x2B88, action 0x2B pl_act_bump_up. Its init (VA 0xC8D680)
// aligns him 40 cm from the wall in 0.1 s and picks pl00action 0x4A (1.00 m up, 80 cm forward, 75 frames) up to 150 cm
// ([VA 0x11B27B4]+0x9C), 0x4C (2.00 m up, 80 cm forward, 114 frames) above; its update (VA 0xC7DC20) puts him on the
// ground below (within 50 cm) at the start and ends with the motion. The height never scales them.
// A container's side gives only bit 0x100 (the raised one's top found 2.50 m above the walkway, 500 above its own base;
// seen from the walkway: its end, plane z = 3420.1 for the container at z 3718.8, normal (0, 0, -1) towards him,
// +0x2AD0 at his feet + 40, +0x2AB0.. 4 / 4 / 1 / 1): bit 0x200 is given while an obstacle of an option that is on has
// its top 2.0-2.8 m above him (a raisable one only while up, +0x1970 1), so the game's own prompt shows. Passing by (not
// facing it), the block is filled from the obstacle's box, within 1.5 m of a face. Its big climb plays a Majini's climb
// instead, put in bank 8 (em84act's, given back at the end): em10_g03 0x0A (Image\Archive\uEm10.arc, 250 frames, their
// fence climb; hands traced on Chris's skeleton: a crouched jump to the fence, hands at 2 m (frames 0-18), the fence
// climbed (18-57), hung from its top (57-87, hands at 305 for its 317 cm), pulled up (87-190), standing at 317 cm and
// 115 forward). Anim::Jump plays its jump, then its hanging from frame 60 (8 frames of blend, the game's SetMotion,
// player vtable +0x1E4): the fence's climb skipped, his jump made longer (up and back from the wall) so that his hands
// reach the top at the cut; the root's travel skipped by the cut is not applied by the game (a motion started at a frame
// starts from its root there). Anim::Fence plays all of it, 317 cm brought down to the height during its jump. 0x11
// (1.50 m) starts with the hands already on its ledge: no jump, it slid up when stretched (user).
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace features {
void* LoadMotionList(const char* archive, const char* path);  // wesker_moves.cpp: kept referenced
bool SetMotionBank(uintptr_t unit, void* list, int bank);     // wesker_moves.cpp
bool EnsureSetMotionHook();                                   // swaps.cpp: ObstacleClimbMotion asked there
bool EnsurePadQueryHooks();                                   // quick_knife.cpp: ObstacleClimbTick on the game thread
const char* UnitClassName(uintptr_t object);                  // parry.cpp: its DTI class name

namespace {
constexpr uintptr_t kClimbUpCondition = 0x75CF00, kClimbUpHandler = 0x76C7C0;  // RVAs
constexpr uintptr_t kObjModel = 0xDB2720;    // sObjModel: the level's objects (+0x24 count, +0x30 array)
constexpr uint32_t kBumpUpAction = 0x2B;
constexpr uint32_t kBigClimbMotion = 0x14C;  // pl00action 0x4C, bank 1
constexpr int kBank = 8;
constexpr uint32_t kMajiniClimbMotion = (kBank << 8) | 0x0A;
constexpr float kMajiniTop = 317.0f;                          // its ledge: its root at the end
constexpr float kJumpEnd = 18.0f, kHangFrame = 60.0f, kCutBlend = 8.0f;
constexpr float kFenceJump = 25.0f;                           // the whole climb: its jump's frames
constexpr float kRootJump[2] = { 145.0f, 46.9f };             // its root at kJumpEnd: height, forward (tools/lmt.py)
constexpr float kRootHang[2] = { 217.7f, 62.9f };             // at kHangFrame
constexpr float kMinHeight = 200.0f, kMaxHeight = 280.0f;
constexpr float kZoneDepth = 150.0f, kEdgeMargin = 25.0f;    // the prompt within 1.5 m of a face, not at its corners
constexpr float kSearch = 800.0f;                            // the obstacle searched within 8 m
constexpr uint32_t kCrossingBits = 0x200 | 0x400 | 0x800 | 0x2000 | 0x40000;  // the probe's crossing prompts

// The two options; the user puts each new obstacle in one of them.
enum Group { kNeeded, kOptional, kGroups };
// Its animation: the Majini's jump cut to its hanging from the top, or their whole fence climb.
enum class Anim { Jump, Fence };
struct Obstacle {
    const char* cls;   // its object's class name starts with it
    const char* what;  // for the log
    Group group;
    float top;         // its top above its base (+0x30)
    float half[2];     // its box around its base: local x, z (measured with the probe, MODLOG)
    bool onlyUp;       // raisable: only while up (+0x1970 1)
    Anim anim;
};
constexpr Obstacle kObstacles[] = {
    // Room 0x1F4: the containers raised by the other one's switch, climbed from the walkway (500 = lift 251 + 249).
    { "uOm0924_Container", "raised container", kNeeded, 500.0f, { 124.0f, 299.0f }, true, Anim::Jump },
    // The same room's containers standing on the ground (two joints, +0x1970 0).
    { "uOm0926_Container", "container on the ground", kOptional, 250.0f, { 124.0f, 299.0f }, false, Anim::Jump },
};

std::atomic<bool> g_on[kGroups]{};
Feature* g_features[kGroups] = {};  // their sliders: the start moved back (cm)
void* g_list = nullptr;             // em10_g03 (game thread)
DWORD g_listTried = 0;

using ConditionFn = bool(__cdecl*)(uintptr_t a, int index);
using HandlerFn = void(__cdecl*)(uintptr_t owner, int index, uintptr_t a, uintptr_t b, uintptr_t c);
ConditionFn g_condition = nullptr;
HandlerFn g_handler = nullptr;
bool g_hooked = false;

struct Climb {
    uintptr_t player = 0;
    DWORD armed = 0;          // the game's prompt made climbable (tick)
    const Obstacle* obstacle = nullptr;
    bool filled = false;      // from the obstacle's box (not facing it)
    bool playing = false;
    bool cut = false;         // Anim::Jump: its hanging started
    void* bank = nullptr;     // his bank 8 before
    float jump = kJumpEnd;    // the frames the corrections are spread over
    float out[2] = {};        // the wall's normal towards where he stood (x, z)
    float extra[2] = {};      // his jump made longer by: up, towards the wall (cm)
    float applied[2] = {};    // the corrections made so far
};
Climb g_climbs[2];  // by player index

bool AnyOn() {
    for (const auto& on : g_on)
        if (on.load(std::memory_order_relaxed)) return true;
    return false;
}

uint32_t Action(uintptr_t p) {
    uint32_t a = 0;
    mem::Read(p + 0x10E0, a);
    return a;
}

uintptr_t Layer0(uintptr_t unit) { return unit + 0x380; }

const Obstacle* ObstacleOf(uintptr_t object) {
    const char* name = UnitClassName(object);
    if (!name) return nullptr;
    for (const Obstacle& o : kObstacles)
        if (g_on[o.group].load(std::memory_order_relaxed) && std::strncmp(name, o.cls, std::strlen(o.cls)) == 0) return &o;
    return nullptr;
}

// The obstacle (of an option that is on) nearest to a point within 8 m, if it can be climbed now: its base, its yaw,
// its top.
uintptr_t ClimbableObstacle(const float* at, const Obstacle*& kind, float (&base)[3], float& yaw, float& top) {
    uintptr_t om = 0, array = 0, best = 0;
    uint32_t count = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(kObjModel)), om) || !om || !mem::Read(om + 0x24, count) ||
        !mem::Read(om + 0x30, array) || !array || count > 4096)
        return 0;
    float bestDist = kSearch * kSearch;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t o = 0;
        float p[3]{};
        if (!mem::Read(array + 4 * i, o) || !o || !mem::SafeRead(reinterpret_cast<void*>(o + 0x30), p, sizeof(p))) continue;
        const float dx = p[0] - at[0], dz = p[2] - at[2], d = dx * dx + dz * dz;
        if (d >= bestDist) continue;
        if (const Obstacle* k = ObstacleOf(o)) {
            bestDist = d;
            best = o;
            kind = k;
        }
    }
    float q[4]{};
    if (!best || !mem::SafeRead(reinterpret_cast<void*>(best + 0x30), base, sizeof(base)) ||
        !mem::SafeRead(reinterpret_cast<void*>(best + 0x40), q, sizeof(q)))
        return 0;
    if (kind->onlyUp) {
        int32_t state = 0;
        if (!mem::Read(best + 0x1970, state) || state != 1) return 0;
    }
    top = base[1] + kind->top;
    yaw = 2.0f * std::atan2(q[1], q[3]);
    return best;
}

// The ledge his probe found ahead of him is a climbable obstacle's top, 2.0-2.8 m above his feet.
const Obstacle* FacesObstacle(uintptr_t p) {
    float pos[3]{}, wall[3]{}, top = 0.0f, base[3]{}, yaw = 0.0f, expected = 0.0f;
    const Obstacle* kind = nullptr;
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p + 0x2AD0), wall, sizeof(wall)) || !mem::Read(p + 0x2AE4, top))
        return nullptr;
    const float height = top - pos[1];
    if (height < kMinHeight || height > kMaxHeight || !ClimbableObstacle(wall, kind, base, yaw, expected) ||
        std::fabs(top - expected) >= 30.0f)
        return nullptr;
    return kind;
}

// Passing by a climbable obstacle (within 1.5 m of a face whose top is 2.0-2.8 m above him): his probe block filled as
// if he faced that face where he is.
const Obstacle* FillFromObstacle(uintptr_t p) {
    float pos[3]{}, base[3]{}, yaw = 0.0f, top = 0.0f;
    const Obstacle* kind = nullptr;
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), pos, sizeof(pos)) || !ClimbableObstacle(pos, kind, base, yaw, top))
        return nullptr;
    const float height = top - pos[1];
    if (height < kMinHeight || height > kMaxHeight) return nullptr;
    const float hx = kind->half[0], hz = kind->half[1];
    const float right[2] = { std::cos(yaw), -std::sin(yaw) }, ahead[2] = { std::sin(yaw), std::cos(yaw) };
    const float dx = pos[0] - base[0], dz = pos[2] - base[2];
    const float lx = dx * right[0] + dz * right[1], lz = dx * ahead[0] + dz * ahead[1];
    // The nearest face he is in front of: its outward normal (local), his distance to it, the wall point (local).
    float best = kZoneDepth, n[2]{}, wl[2]{};
    bool found = false;
    if (std::fabs(lx) <= hx - kEdgeMargin && std::fabs(lz) > hz && std::fabs(lz) - hz < best) {
        best = std::fabs(lz) - hz;
        n[0] = 0.0f, n[1] = lz > 0.0f ? 1.0f : -1.0f;
        wl[0] = lx, wl[1] = n[1] * hz;
        found = true;
    }
    if (std::fabs(lz) <= hz - kEdgeMargin && std::fabs(lx) > hx && std::fabs(lx) - hx < best) {
        best = std::fabs(lx) - hx;
        n[0] = lx > 0.0f ? 1.0f : -1.0f, n[1] = 0.0f;
        wl[0] = n[0] * hx, wl[1] = lz;
        found = true;
    }
    if (!found) return nullptr;
    const float nw[2] = { n[0] * right[0] + n[1] * ahead[0], n[0] * right[1] + n[1] * ahead[1] };
    const float wall[3] = { base[0] + wl[0] * right[0] + wl[1] * ahead[0], pos[1] + 40.0f,
                            base[2] + wl[0] * right[1] + wl[1] * ahead[1] };
    const float wallPoint[4] = { wall[0], wall[1], wall[2], 0.0f }, topPoint[4] = { wall[0], top, wall[2], 0.0f };
    const float inside[4] = { wall[0] - nw[0] * 40.0f, 0.0f, wall[2] - nw[1] * 40.0f, 0.0f };
    const float plane[4] = { nw[0], 0.0f, nw[1], -(nw[0] * wall[0] + nw[1] * wall[2]) };
    const uint32_t ids[4] = { 4, 4, 1, 1 };
    uint32_t flags = 0;
    mem::Read(p + 0x2B50, flags);
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x2AB0), ids, sizeof(ids));
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x2AD0), wallPoint, sizeof(wallPoint));
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x2AE0), topPoint, sizeof(topPoint));
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x2AF0), topPoint, sizeof(topPoint));
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x2B20), inside, sizeof(inside));
    mem::SafeWrite(reinterpret_cast<void*>(p + 0x2B30), plane, sizeof(plane));
    mem::Write(p + 0x2B50, (flags & ~kCrossingBits) | 0x200);
    mem::Write<uint32_t>(p + 0x2B88, 0);
    return kind;
}

// His probe block made a climbable ledge (until his probe's next frame): the bit given when he faces an obstacle, else
// filled from an obstacle he passes by, if his probe found no other crossing.
const Obstacle* MakeClimbable(uintptr_t p, bool& filled) {
    uint32_t flags = 0;
    if (!mem::Read(p + 0x2B50, flags)) return nullptr;
    if (const Obstacle* kind = FacesObstacle(p)) {
        filled = false;
        return mem::Write(p + 0x2B50, flags | 0x200) ? kind : nullptr;
    }
    if (flags & kCrossingBits) return nullptr;
    filled = true;
    return FillFromObstacle(p);
}

bool __cdecl HookCondition(uintptr_t a, int index) {
    if (AnyOn() && index >= 0 && index < 2) {
        const uintptr_t p = actors::Player(index);
        bool filled = false;
        if (const Obstacle* kind = p ? MakeClimbable(p, filled) : nullptr) {
            Climb& c = g_climbs[index];
            if (c.player != p || !c.armed || c.filled != filled || c.obstacle != kind)
                LOG("obstacle climb: player %d can climb a %s (%s)", index, kind->what, filled ? "passing by" : "facing it");
            c.player = p;
            c.filled = filled;
            c.obstacle = kind;
            const DWORD now = GetTickCount();
            c.armed = now ? now : 1;
        }
    }
    return g_condition(a, index);
}

// The press: his block filled again (his probe may have run since the prompt's condition).
void __cdecl HookHandler(uintptr_t owner, int index, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (AnyOn() && index >= 0 && index < 2) {
        const Climb& climb = g_climbs[index];
        const uintptr_t p = actors::Player(index);
        bool filled = false;
        if (p && climb.player == p && climb.armed && GetTickCount() - climb.armed < 1000) MakeClimbable(p, filled);
    }
    g_handler(owner, index, a, b, c);
}

bool Hook(uintptr_t rva, const uint8_t* expected, size_t size, void* detour, void** original, const char* what) {
    uint8_t* target = mem::Rva(rva);
    if (!mem::IsGameCode(target, size) || std::memcmp(target, expected, size) != 0) {
        LOG("obstacle climb: unexpected bytes for the %s: %s", what, mem::Hex(target, size).c_str());
        return false;
    }
    mem::EnsureExecutable(target, size);
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    LOG("obstacle climb: the climb up %s hooked at %p", what, target);
    return true;
}

bool EnsureHooks() {
    if (g_hooked) return true;
    static const uint8_t condition[] = { 0x8B, 0x0D, 0xDC, 0x27, 0x1B, 0x01, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10 };
    static const uint8_t handler[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45, 0x0C, 0x8B, 0x0D, 0xDC, 0x27, 0x1B, 0x01 };
    if (!Hook(kClimbUpCondition, condition, sizeof(condition), reinterpret_cast<void*>(&HookCondition),
              reinterpret_cast<void**>(&g_condition), "condition") ||
        !Hook(kClimbUpHandler, handler, sizeof(handler), reinterpret_cast<void*>(&HookHandler),
              reinterpret_cast<void**>(&g_handler), "handler"))
        return false;
    g_hooked = true;
    return true;
}

float Smooth(float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// Layer 0 of a player restarted at a frame (his vtable's SetMotion, as shell_reload.cpp).
void StartAt(uintptr_t unit, uint32_t motion, float frame) {
    using SetMotionFn = uint32_t(__fastcall*)(void* self, void* edx, uint32_t layer, uint32_t motion, float blend, float start,
                                              float speed, uint32_t mode);
    uintptr_t vtable = 0, fn = 0;
    float speed = 1.0f;
    uint16_t mode = 0;
    if (!mem::Read(unit, vtable) || !vtable || !mem::Read(vtable + 0x1E4, fn) || !fn) return;
    mem::Read(Layer0(unit) + 0x24, speed);
    mem::Read(Layer0(unit) + 0xA, mode);
    __try {
        reinterpret_cast<SetMotionFn>(fn)(reinterpret_cast<void*>(unit), nullptr, 0, motion, kCutBlend, frame, speed, mode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void End(Climb& c, const char* why) {
    if (c.bank) SetMotionBank(c.player, c.bank, kBank);
    LOG("obstacle climb: player %08X done (%s), bank %X given back", static_cast<unsigned>(c.player), why, kBank);
    c.playing = false;
    c.armed = 0;
    c.bank = nullptr;
}
}  // namespace

// swaps.cpp's SetMotion hook: the big climb of an obstacle made climbable plays the Majini's climb.
uint32_t ObstacleClimbMotion(uintptr_t unit, uint32_t layer, uint32_t motion) {
    if (layer != 0 || motion != kBigClimbMotion || !g_list || !AnyOn()) return motion;
    for (Climb& c : g_climbs) {
        if (c.player != unit || !c.armed || !c.obstacle || GetTickCount() - c.armed > 3000 || Action(unit) != kBumpUpAction)
            continue;
        float height = 0.0f, pos[3]{}, wall[3]{}, yaw = 0.0f;
        if (!mem::Read(unit + 0x2E24, height) || height < kMinHeight || height > kMaxHeight ||
            !mem::SafeRead(reinterpret_cast<void*>(unit + 0x30), pos, sizeof(pos)) ||
            !mem::SafeRead(reinterpret_cast<void*>(unit + 0x2E10), wall, sizeof(wall)) || !mem::Read(unit + 0x2E20, yaw))
            return motion;
        void* before = nullptr;
        mem::Read(unit + 0xA88 + 4 * kBank, before);
        if (!SetMotionBank(unit, g_list, kBank)) return motion;
        c.bank = before;
        const Feature* f = g_features[c.obstacle->group];
        const float back = f ? f->value : 45.0f;
        if (c.obstacle->anim == Anim::Jump) {
            // At the cut his root goes from the jump's to the hanging's, which hangs (317 - height) lower here and `back`
            // further from the wall: his jump makes up the difference.
            c.extra[0] = kRootHang[0] - kRootJump[0] - (kMajiniTop - height);
            c.extra[1] = kRootHang[1] - kRootJump[1] - back;
            c.jump = kJumpEnd;
        } else {  // the whole fence climb, its 317 cm brought down to the height during its jump
            c.extra[0] = height - kMajiniTop;
            c.extra[1] = -back;
            c.jump = kFenceJump;
        }
        c.cut = false;
        // Back from the wall: along the normal whose yaw the climb took, towards where he stands.
        c.out[0] = std::sin(yaw);
        c.out[1] = std::cos(yaw);
        if ((pos[0] - wall[0]) * c.out[0] + (pos[2] - wall[2]) * c.out[1] < 0.0f) {
            c.out[0] = -c.out[0];
            c.out[1] = -c.out[1];
        }
        c.applied[0] = c.applied[1] = 0.0f;
        c.playing = true;
        LOG("obstacle climb: %08X climbs a %s, %.0f cm, with the Majini's %s (motion %X), away from the wall (%.2f %.2f), "
            "jump longer by %.0f up / %.0f ahead", static_cast<unsigned>(unit), c.obstacle->what, height,
            c.obstacle->anim == Anim::Jump ? "jump" : "fence climb", kMajiniClimbMotion, c.out[0], c.out[1], c.extra[0],
            c.extra[1]);
        return kMajiniClimbMotion;
    }
    return motion;
}

// coop_solo.cpp's pad query tick (game thread): the motion list loaded, the climbs' corrections made.
void ObstacleClimbTick() {
    if (!AnyOn()) return;
    if (!actors::InGame()) {
        for (Climb& c : g_climbs) c = Climb{};
        return;
    }
    const DWORD now = GetTickCount();
    if (!g_list && now - g_listTried > 5000) {
        g_listTried = now;
        g_list = LoadMotionList("uEm10", "pawn\\em\\em10\\motion\\em10_g03");
    }
    for (Climb& c : g_climbs) {
        if (!c.playing) continue;
        uint32_t motion = 0;
        float frame = 0.0f, pos[3]{};
        mem::Read(Layer0(c.player) + 4, motion);
        if (Action(c.player) != kBumpUpAction) {
            End(c, "his action changed");
            continue;
        }
        if ((motion & 0xFFFF) != kMajiniClimbMotion || !mem::Read(Layer0(c.player) + 0x18, frame) ||
            !mem::SafeRead(reinterpret_cast<void*>(c.player + 0x30), pos, sizeof(pos)))
            continue;
        if (c.obstacle && c.obstacle->anim == Anim::Jump && !c.cut && frame >= kJumpEnd) {  // the fence's climb skipped
            c.cut = true;
            StartAt(c.player, kMajiniClimbMotion, kHangFrame);
            LOG("obstacle climb: %08X hangs from the top (frame %.0f -> %.0f)", static_cast<unsigned>(c.player), frame,
                kHangFrame);
        }
        const float w = c.cut ? 1.0f : Smooth(frame / c.jump);
        const float up = c.extra[0] * w, ahead = c.extra[1] * w;  // ahead: towards the wall (against `out`)
        pos[1] += up - c.applied[0];
        pos[0] -= c.out[0] * (ahead - c.applied[1]);
        pos[2] -= c.out[1] * (ahead - c.applied[1]);
        mem::SafeWrite(reinterpret_cast<void*>(c.player + 0x30), pos, sizeof(pos));
        c.applied[0] = up;
        c.applied[1] = ahead;
    }
}

void RegisterObstacleClimb() {
    static constexpr struct {
        const char* id;
        const char* name;
        const char* help;
    } kOptions[kGroups] = {
        { "player.obstacle_climb", "Obstacles : grimper seul",
          "Les obstacles qu'on ne peut pas franchir seul pour avancer (2 à 2,8 m) : le prompt de grimpe du jeu apparaît "
          "devant eux et à 1,5 m de leurs faces, et Chris y monte seul avec le saut des Majini qui attrapent le rebord. "
          "Pour l'instant : les conteneurs qu'on fait monter (la salle des deux conteneurs), seulement quand ils sont en "
          "haut, depuis la passerelle. Le curseur recule son départ (cm) s'il traverse l'obstacle en grimpant (plus) ou "
          "s'il le manque (moins)." },
        { "player.obstacle_climb_optional", "Obstacles facultatifs : grimper",
          "Les obstacles qu'on n'a pas besoin de franchir pour avancer (2 à 2,8 m) : même prompt de grimpe du jeu et même "
          "saut des Majini qu'« Obstacles : grimper seul ». Pour l'instant : les conteneurs posés au sol (la salle des "
          "deux conteneurs). Le curseur recule son départ (cm) s'il traverse l'obstacle en grimpant (plus) ou s'il le "
          "manque (moins)." },
    };
    for (int group = 0; group < kGroups; ++group) {
        g_features[group] = &Add({ .id = kOptions[group].id, .name = kOptions[group].name, .help = kOptions[group].help,
                                   .category = "Joueur", .kind = Kind::Toggle,
                                   .set = [group](bool on) {
                                       if (on && (!EnsurePadQueryHooks() || !EnsureSetMotionHook() || !EnsureHooks()))
                                           return false;
                                       g_on[group] = on;
                                       return true;
                                   },
                                   .available = [] { return game::IsSupported(); },
                                   .value = 45, .valueMin = 0, .valueMax = 100, .valueFormat = "%.0f cm" });
    }
}
}  // namespace features
