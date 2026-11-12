// Belt lamp (user, 2026-10-27: "the lamp's light from the belt of the player who calls it, with a brightness slider").
// The searchlight (item 0x0130, uWp30, archive Image\Archive\uWp30.arc) lights through the scheduler resource
// pawn\wp\wp30\soft\wp30_lights (rScheduler, DTI RVA 0xE4A364). Its init (VA 0xBA6E30) makes a uScheduler (allocator
// [RVA 0xDAF294] vfunc +0x14(size 0x3C, align 0x10), ctor VA 0x45CBF0), gives it the resource (VA 0x45CEB0: the
// scheduler makes its units), flags it 0x2000 (+4), starts it (vfunc +0x18), sets its frame +0x1C to 10 and takes its
// six units (VA 0xCE1390(scheduler, i), cdecl): 0 / 1 uSpotMapLight (the beam: parent unit +0xA4, joint +0xAC,
// +0x120 = 1; colour +0x30, local offset +0x40, local direction +0x50), 2 uBioShadow (+0x1B0 = the first spot),
// 3 / 5 uPointLight (parent +0x68, joint +0x6C), 4 uBackLight, each flagged 0x2000; then the scheduler goes into sMain's
// move line 10 (VA 0x428B80, thiscall([RVA 0xE340B0], 10, unit)). The lamp's destructor (VA 0xBA6CF0) clears the flag
// and calls vfunc +0x30 on each, then on the scheduler.
// Here the same with the player as the parent and his joint 0 (the hips' root, 108 cm up, its Z axis his facing): the
// beam a little ahead of the belt, tilted down. The scheduler puts the resource's local offsets and directions back
// after it starts (the lamp's (0, 0, -15) / (0, 0, -1): from the hips the beam went backwards, user 2026-10-27), so
// ours are written again every frame. The colours are multiplied by the slider, from the resource's own read once the
// scheduler has set them (0.3 s after the start).
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace features {
bool EnsurePadQueryHooks();                  // quick_knife.cpp: BeltLampTick runs on the game thread
bool LoadWeaponArchive(const char* name);    // inventory.cpp: Image\Archive\<name>, kept loaded
bool Player1InventoryOpen();                 // pickup.cpp: its shortcut ignored there
bool OrganiserOpen();                        // knife_icon.cpp: the Organiser screen (the same)

namespace {
constexpr uintptr_t kSchedulerVtable = 0xCAE544;  // uScheduler (RVA)
constexpr int kUnits = 6;
constexpr char kResource[] = "pawn\\wp\\wp30\\soft\\wp30_lights";

struct Lamp {
    std::atomic<bool> wanted{ false };
    std::atomic<float> brightness{ 1.0f };
    uintptr_t player = 0;        // game thread from here
    uintptr_t scheduler = 0;
    uintptr_t units[kUnits] = {};
    float baseColor[kUnits][3] = {};
    float applied = -1.0f;
    DWORD failedAt = 0, createdAt = 0;
    bool baseTaken = false;
};
Lamp g_lamps[2];
void* g_resource = nullptr;

void* Resource() {
    if (g_resource) return g_resource;
    if (!LoadWeaponArchive("uWp30")) return nullptr;
    void* mgr = nullptr;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE34580)), mgr) || !mgr) return nullptr;
    using ResLoadFn = void*(__fastcall*)(void* mgr, void* edx, void* type, const char* path, unsigned flags);
    void* res = nullptr;
    __try {
        res = reinterpret_cast<ResLoadFn>(mem::Rva(0x48B70))(mgr, nullptr, mem::Rva(0xE4A364), kResource, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        res = nullptr;
    }
    uint32_t state = 0;
    if (!res || !mem::Read(reinterpret_cast<uintptr_t>(res) + 0x4C, state) || !(state & 1)) {
        LOG("belt lamp: %s not available (%p, state %08X)", kResource, res, state);
        return nullptr;
    }
    g_resource = res;
    LOG("belt lamp: %s at %p", kResource, res);
    return res;
}

bool SchedulerAlive(const Lamp& lamp) {
    uintptr_t vtable = 0;
    return lamp.scheduler && mem::Read(lamp.scheduler, vtable) &&
           vtable == reinterpret_cast<uintptr_t>(mem::Rva(kSchedulerVtable));
}

void Forget(Lamp& lamp) {
    lamp.player = lamp.scheduler = 0;
    std::memset(lamp.units, 0, sizeof(lamp.units));
    lamp.applied = -1.0f;
    lamp.baseTaken = false;
}

// The beam ahead of the belt, tilted down (joint 0's frame: Y up, Z his facing); the glows close to it.
// Tilted down by about 5 degrees (user, 2026-10-27: 15 degrees lit the floor too much).
constexpr float kBeamOffset[3] = { 0.0f, -8.0f, 18.0f }, kBeamDirection[3] = { 0.0f, -0.09f, 0.996f };
constexpr float kGlowOffset[2][3] = { { 0.0f, 0.0f, 40.0f }, { 0.0f, 0.0f, 25.0f } };

void Place(const Lamp& lamp) {
    const auto put = [](uintptr_t at, const float (&v)[3]) {
        float now[3]{};
        if (mem::SafeRead(reinterpret_cast<void*>(at), now, sizeof(now)) && std::memcmp(now, v, sizeof(now)) != 0)
            mem::SafeWrite(reinterpret_cast<void*>(at), v, sizeof(v));
    };
    for (int i : { 0, 1 })
        if (lamp.units[i]) {
            put(lamp.units[i] + 0x40, kBeamOffset);
            put(lamp.units[i] + 0x50, kBeamDirection);
        }
    if (lamp.units[3]) put(lamp.units[3] + 0x40, kGlowOffset[0]);
    if (lamp.units[5]) put(lamp.units[5] + 0x40, kGlowOffset[1]);
}

void Kill(uintptr_t unit) {
    uint32_t flags = 0;
    uintptr_t vtable = 0, fn = 0;
    if (!unit || !mem::Read(unit + 4, flags) || !mem::Read(unit, vtable) || !mem::Read(vtable + 0x30, fn) || !fn) return;
    mem::Write(unit + 4, flags & ~0x2000u);
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*)>(fn)(reinterpret_cast<void*>(unit), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Destroy(Lamp& lamp, int index) {
    if (SchedulerAlive(lamp)) {
        for (uintptr_t unit : lamp.units) Kill(unit);
        Kill(lamp.scheduler);
        LOG("belt lamp: player %d's light put out", index + 1);
    }
    Forget(lamp);
}

bool Create(Lamp& lamp, uintptr_t player, int index) {
    void* res = Resource();
    if (!res) return false;
    uintptr_t allocator = 0, vtable = 0, alloc = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDAF294)), allocator) || !allocator ||
        !mem::Read(allocator, vtable) || !mem::Read(vtable + 0x14, alloc) || !alloc)
        return false;
    uintptr_t scheduler = 0;
    __try {
        scheduler = reinterpret_cast<uintptr_t>(
            reinterpret_cast<void*(__fastcall*)(void*, void*, uint32_t, uint32_t)>(alloc)(reinterpret_cast<void*>(allocator),
                                                                                         nullptr, 0x3C, 0x10));
        if (!scheduler) return false;
        reinterpret_cast<void*(__fastcall*)(void*, void*)>(mem::Rva(0x5CBF0))(reinterpret_cast<void*>(scheduler), nullptr);
        reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(mem::Rva(0x5CEB0))(reinterpret_cast<void*>(scheduler),
                                                                                    nullptr, res);
        uint32_t flags = 0;
        mem::Read(scheduler + 4, flags);
        mem::Write(scheduler + 4, flags | 0x2000u);
        uintptr_t svtable = 0, start = 0;
        if (mem::Read(scheduler, svtable) && mem::Read(svtable + 0x18, start) && start)
            reinterpret_cast<void(__fastcall*)(void*, void*)>(start)(reinterpret_cast<void*>(scheduler), nullptr);
        uintptr_t held = 0;
        if (mem::Read(scheduler + 0x38, held) && held) mem::Write(scheduler + 0x1C, 10.0f);
        using UnitFn = uintptr_t(__cdecl*)(uintptr_t scheduler, int i);
        for (int i = 0; i < kUnits; ++i) lamp.units[i] = reinterpret_cast<UnitFn>(mem::Rva(0x8E1390))(scheduler, i);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("belt lamp: the scheduler could not be made");
        return false;
    }
    for (int i = 0; i < kUnits; ++i) {
        const uintptr_t unit = lamp.units[i];
        uint32_t flags = 0;
        if (!unit || !mem::Read(unit + 4, flags)) continue;
        mem::Write(unit + 4, flags | 0x2000u);
        if (i == 0 || i == 1) {  // the spots
            mem::Write<uint8_t>(unit + 0x120, 1);
            mem::Write(unit + 0xA4, player);
            mem::Write<uint32_t>(unit + 0xAC, 0);
        } else if (i == 2) {
            if (lamp.units[0]) mem::Write(unit + 0x1B0, lamp.units[0]);
        } else if (i == 3 || i == 5) {  // the glows
            mem::Write(unit + 0x68, player);
            mem::Write<uint32_t>(unit + 0x6C, 0);
        }
    }
    Place(lamp);
    uintptr_t main = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE340B0)), main) || !main) return false;
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, int, uintptr_t)>(mem::Rva(0x28B80))(reinterpret_cast<void*>(main),
                                                                                           nullptr, 10, scheduler);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("belt lamp: the scheduler could not be added");
        return false;
    }
    lamp.player = player;
    lamp.scheduler = scheduler;
    lamp.applied = -1.0f;
    lamp.baseTaken = false;
    lamp.createdAt = GetTickCount();
    LOG("belt lamp: player %d lit (scheduler %08X, units %08X %08X %08X %08X %08X %08X)", index + 1,
        static_cast<unsigned>(scheduler), static_cast<unsigned>(lamp.units[0]), static_cast<unsigned>(lamp.units[1]),
        static_cast<unsigned>(lamp.units[2]), static_cast<unsigned>(lamp.units[3]), static_cast<unsigned>(lamp.units[4]),
        static_cast<unsigned>(lamp.units[5]));
    return true;
}

void ApplyBrightness(Lamp& lamp) {
    if (!lamp.baseTaken) {
        if (GetTickCount() - lamp.createdAt < 300) return;
        for (int i = 0; i < kUnits; ++i)
            if (lamp.units[i] && i != 2 && i != 4)
                mem::SafeRead(reinterpret_cast<void*>(lamp.units[i] + 0x30), lamp.baseColor[i], sizeof(lamp.baseColor[i]));
        lamp.baseTaken = true;
        lamp.applied = -1.0f;
    }
    const float scale = lamp.brightness.load(std::memory_order_relaxed);
    if (scale == lamp.applied) return;
    for (int i = 0; i < kUnits; ++i) {
        if (!lamp.units[i] || i == 2 || i == 4) continue;
        const float color[3] = { lamp.baseColor[i][0] * scale, lamp.baseColor[i][1] * scale, lamp.baseColor[i][2] * scale };
        mem::SafeWrite(reinterpret_cast<void*>(lamp.units[i] + 0x30), color, sizeof(color));
    }
    lamp.applied = scale;
}
}  // namespace

void PutOutBeltLamps();  // below

// action_log.cpp: "Le joueur 2 ne ramasse pas" lets player 2 take the cave lamp, but not while a belt lamp is lit.
bool BeltLampOn() {
    return g_lamps[0].wanted.load(std::memory_order_relaxed) || g_lamps[1].wanted.load(std::memory_order_relaxed);
}

// quick_knife.cpp's pad query hook (game thread, every frame).
void BeltLampTick() {
    const bool inGame = actors::InGame();
    for (int i = 0; i < 2; ++i) {
        Lamp& lamp = g_lamps[i];
        // The scheduler lives on in sMain's move line 10 through level loads and player changes (2026-11-03: four lamps
        // made before loads still alive, their spots' parent cleared - the user saw the lighting go wrong after a New
        // Game+, in the campaign and the DLC): put out whenever it is still there, only forgotten once gone.
        if (!inGame) {
            if (lamp.scheduler) {
                if (SchedulerAlive(lamp)) Destroy(lamp, i);
                else Forget(lamp);
            }
            continue;
        }
        const uintptr_t player = actors::Player(i);
        const bool wanted = lamp.wanted.load(std::memory_order_relaxed) && player;
        if (lamp.scheduler && (!wanted || player != lamp.player || !SchedulerAlive(lamp))) {
            if (SchedulerAlive(lamp)) Destroy(lamp, i);
            else Forget(lamp);
        }
        if (!wanted) continue;
        if (!lamp.scheduler) {
            const DWORD now = GetTickCount();
            if (lamp.failedAt && now - lamp.failedAt < 3000) continue;
            if (!Create(lamp, player, i)) {
                lamp.failedAt = now ? now : 1;
                Forget(lamp);
                continue;
            }
            lamp.failedAt = 0;
        }
        Place(lamp);
        ApplyBrightness(lamp);
    }
}

void RegisterBeltLamp() {
    for (int i = 0; i < 2; ++i) {
        static const char* ids[2] = { "player.belt_lamp_p1", "player.belt_lamp_p2" };
        static const char* names[2] = { "Lampe à la ceinture — J1", "Lampe à la ceinture — J2" };
        Feature& lamp = Add({ .id = ids[i], .name = names[i],
                              .help = "La lumière de la lampe (le projecteur des grottes) part de la ceinture du joueur, "
                                      "vers l'avant, sans rien tenir : ses armes restent en main. Le curseur règle la "
                                      "luminosité (x1 = la lampe du jeu).",
                              .category = "Joueur", .kind = Kind::Toggle,
                              .available = [] { return game::IsSupported(); },
                              .value = 1, .valueMin = 0.25f, .valueMax = 5, .valueFormat = "x%.2f" });
        Feature* const self = &lamp;
        lamp.set = [i, self](bool on) {
            if (on && !EnsurePadQueryHooks()) return false;
            g_lamps[i].brightness = self->value;
            g_lamps[i].wanted = on;
            return true;
        };
        lamp.tick = [i, self] { g_lamps[i].brightness.store(self->value, std::memory_order_relaxed); };
        // Its key shared with "Mettre le couteau sélectionné dans l'étui" (user, 2026-10-27): ignored in the inventory.
        lamp.hotkeyIgnored = [] { return Player1InventoryOpen() || OrganiserOpen(); };
    }
    OnRestore(PutOutBeltLamps);
}

// Unload: put out (from the unload thread: the units are only flagged and asked to die by their own vfunc).
void PutOutBeltLamps() {
    for (int i = 0; i < 2; ++i) {
        g_lamps[i].wanted = false;
        if (actors::InGame()) Destroy(g_lamps[i], i);
        else Forget(g_lamps[i]);
    }
}
}  // namespace features
