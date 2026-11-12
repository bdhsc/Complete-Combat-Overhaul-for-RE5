// Caméra. Camera owner = [[[re5dx9+0xE3457C]+0x34]+0x1B0]; it points to 6 parameter blocks:
// +0x2A4/+0x2A8/+0x2AC normal camera variants, +0x2B0/+0x2B4/+0x2B8 aim variants (UT's "Normal" and
// "Aim" panes edit +0x2A8 and +0x2B4). Block floats: [0] horizontal distance (shoulder side),
// [1] vertical distance, [2] distance, [4] horizontal adjustment, [5] vertical adjustment, [6] angle, [9] FOV.
#include "common/i18n.hpp"
#include "features/body_cut.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "common/settings.hpp"
#include "hooks/hooks.hpp"
#include "ui/sliders.hpp"

#include <windows.h>
#include <imgui.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace features {
uintptr_t CameraBlock(int slot);         // used by player.cpp (lefty camera)
bool CameraBlocksReady();
bool VolcanoFinishPlaying();  // final_fight.cpp: the volcano Wesker's finish under way
bool QuickShotCutsScope();     // quick_knife.cpp: an RE6 quick shot under way (the scope cut)
void IronSightWeaponBank(uintptr_t p, bool wanted);  // quick_knife.cpp: the Jail Breaker aims with the Ithaca's motions
uint32_t LaserColourRgb();                            // hud_colors.cpp: the laser's colour, 0xRRGGBB
bool EnsureEnemyActionHook();  // action_log.cpp: every unit's request core (the doors' actions)
std::string Player1NativeMoveId(uintptr_t p, uint32_t action);  // swaps.cpp: the free melee move player 1's action plays
bool MoveFollowsTurns(const std::string& id);                    // melee_tuning.cpp: its "Suivi 1re pers." column
const char* ActiveCombatMoveId();                                // wesker_moves.cpp: the trainer move playing

namespace {
bool Supported() { return game::IsSupported(); }

uintptr_t CameraOwner() {
    uintptr_t a = 0, b = 0, c = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE3457C)), a) || !a) return 0;
    if (!mem::Read(a + 0x34, b) || !b) return 0;
    if (!mem::Read(b + 0x1B0, c)) return 0;
    return c;
}

// The game rewrites the camera values from its own tables unless this byte is set (UT "Freeze").
mem::Patch g_freezeValues("camera.freeze_values", 0x4468C2, { 0x00 }, { 0x0A });
int g_freezeUsers = 0;

void AcquireFreeze() {
    if (g_freezeUsers++ == 0) g_freezeValues.Apply();
}
void ReleaseFreeze() {
    if (g_freezeUsers > 0 && --g_freezeUsers == 0) g_freezeValues.Restore();
}

struct CamField {
    const char* id;
    const char* name;
    int block;   // 1 = normal (+0x2A8), 4 = aim (+0x2B4)
    int index;   // float index in the block
    float def, lo, hi;
};

const CamField kFields[] = {
    { "camera.normal.hdist", "Caméra normale : décalage horizontal (épaule)", 1, 0, -35, -200, 200 },
    { "camera.normal.vdist", "Caméra normale : hauteur", 1, 1, 171, -100, 500 },
    { "camera.normal.dist", "Caméra normale : recul", 1, 2, -135, -600, 50 },
    { "camera.normal.hadj", "Caméra normale : visée horizontale", 1, 4, -60, -200, 200 },
    { "camera.normal.vadj", "Caméra normale : visée verticale", 1, 5, 135, -200, 500 },
    { "camera.normal.angle", "Caméra normale : angle", 1, 6, 175, 0, 360 },
    { "camera.normal.fov", "Caméra normale : champ de vision", 1, 9, 42.5f, 10, 120 },
    { "camera.aim.hdist", "Caméra de visée : décalage horizontal (épaule)", 4, 0, -43, -200, 200 },
    { "camera.aim.vdist", "Caméra de visée : hauteur", 4, 1, 168, -100, 500 },
    { "camera.aim.dist", "Caméra de visée : recul", 4, 2, -78, -600, 50 },
    { "camera.aim.hadj", "Caméra de visée : visée horizontale", 4, 4, -50, -200, 200 },
    { "camera.aim.vadj", "Caméra de visée : visée verticale", 4, 5, 139, -200, 500 },
    { "camera.aim.angle", "Caméra de visée : angle", 4, 6, 220, 0, 360 },
    { "camera.aim.fov", "Caméra de visée : champ de vision", 4, 9, 33, 10, 120 },
};

// Switching a slider off writes the game's value (CamField::def, read in game without the trainer). The value read
// when switching on was not reliable: after a trainer reload the block already held the user's value, so "Défaut"
// left the camera far away (user, 2026-10-14).

// Players present for a few seconds: right after a load the chain above still leads to another object.
bool LevelSettled() {
    static ULONGLONG since = 0;
    if (!actors::InGame()) {
        since = 0;
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (!since) since = now;
    return now - since >= 3000;
}

// A real parameter block: 10 ordinary floats, FOV [9] in 5..170 degrees, [6] (the look's depth, "angle") within 1500. A pointer
// read as a float is a tiny non-zero number, rejected. [6] may be negative (2026-11-08: the slide's camera pulled back moves it
// below 0; with 0..400 here the block was refused and the camera could not be given back, user: "when he gets up I see nothing").
bool LooksLikeCameraBlock(uintptr_t block) {
    float v[10]{};
    if (!block || !mem::SafeRead(reinterpret_cast<void*>(block), v, sizeof(v))) return false;
    for (float x : v)
        if (!std::isfinite(x) || std::fabs(x) > 100000.0f || (x != 0.0f && std::fabs(x) < 1e-4f)) return false;
    return v[9] >= 5.0f && v[9] <= 170.0f && std::fabs(v[6]) <= 1500.0f;
}
}

// ---- Presets (user, 2026-10-14): five sets of every camera option (on/off + slider value), one active at a time.
// The active preset is pushed into the options; while it is active the options are copied back into it every
// frame, so the +/- hotkeys and the plain option rows edit it too. None active = every camera option off (game
// camera). Saved in camera_presets.ini (data folder).
struct PresetEntry {
    bool on = false;
    float value = 0;
};
struct CameraPreset {
    std::string name;
    std::map<std::string, PresetEntry> entries;
};
constexpr int kPresets = 5;
CameraPreset g_presets[kPresets];
int g_activePreset = -1;
bool g_presetsLoaded = false;
bool g_presetsDirty = false;
ULONGLONG g_presetsDirtySince = 0;
Feature* g_presetActions[kPresets] = {};

// The choices too (user, 2026-11-03: "Ouverture des portes" was only in the current settings, not per preset): their
// entry's value is the choice index, on = not the default.
std::vector<Feature*> CameraOptions() {
    std::vector<Feature*> options;
    for (Feature* f : List())
        if (f->category == "Caméra" && (f->kind == Kind::Toggle || f->kind == Kind::Choice) && f->id != "camera.fov_lod")
            options.push_back(f);
    return options;
}

// The game's own value of a camera slider, read in game without the trainer (user's request, 2026-10-14: normal
// -35 / 171 / -135 / -60 / 135 / 175 / 42.5, aim -43 / 168 / -78 / -50 / 139 / 220 / 33); other options: their value.
float GameValue(const Feature& f) {
    if (f.kind == Kind::Choice) return 0.0f;  // its default
    for (const CamField& field : kFields)
        if (f.id == field.id) return field.def;
    return f.value;
}

// Preset 0 is "Défaut": the game camera, every option off at the game's values, not editable.
PresetEntry& Entry(int preset, Feature& f) {
    PresetEntry& entry = g_presets[preset].entries.try_emplace(f.id, PresetEntry{ false, GameValue(f) }).first->second;
    if (preset == 0) entry = PresetEntry{ false, GameValue(f) };
    return entry;
}

std::wstring PresetsPath() { return logx::DataDir() + L"\\camera_presets.ini"; }

void SavePresets() {
    FILE* file = nullptr;
    if (_wfopen_s(&file, PresetsPath().c_str(), L"wb") || !file) return;
    std::fprintf(file, "version=2\nactive=%d\n", g_activePreset);
    for (int i = 0; i < kPresets; ++i) {
        std::fprintf(file, "name%d=%s\n", i, g_presets[i].name.c_str());
        for (const auto& [id, entry] : g_presets[i].entries)
            std::fprintf(file, "p%d.%s=%d,%g\n", i, id.c_str(), entry.on ? 1 : 0, entry.value);
    }
    std::fclose(file);
    g_presetsDirty = false;
}

void MarkPresetsDirty() {
    if (!g_presetsDirty) g_presetsDirtySince = GetTickCount64();
    g_presetsDirty = true;
}

void RenamePresetAction(int i) {
    if (g_presetActions[i]) g_presetActions[i]->name = "Activer le preset « " + g_presets[i].name + " »";
}

// Pushes one option of a preset into the live option. Unavailable options (menus, loading) only remember the wish:
// they are applied when the camera exists again.
void ApplyEntry(Feature& f, const PresetEntry& entry) {
    if (f.kind == Kind::Choice) {
        const int index = static_cast<int>(entry.value + 0.5f);
        if (static_cast<int>(f.value + 0.5f) != index || f.applied != index) Choose(f, index);
        return;
    }
    if (HasSlider(f) && f.value != entry.value) {
        f.value = entry.value;
        settings::SaveFeature(f);
    }
    if (f.enabled == entry.on) {
        f.wanted = entry.on;
        return;
    }
    if (f.available && !f.available()) {
        f.wanted = entry.on;
        settings::SaveFeature(f);
        return;
    }
    SetEnabled(f, entry.on);
}

bool g_gameValuesPending = false;
bool FirstPersonKeepsNormalCamera();  // below: first person on foot, not given back to the third person
bool FirstPersonHidesSheath(uintptr_t player);  // below
void ReleaseStaleFirstPerson();                  // below: the head given back when the cave stops running
bool RefreshModelState(uintptr_t player);       // inventory.cpp: the sheath's model state picked again
// The sheathed knife hidden in the first person (inventory.cpp's model state hook asks FirstPersonHidesSheath): the
// state picked again when that changes, or for a new player 1 (unknown at start: picked once).
uintptr_t g_sheathPlayer = 0;
bool g_sheathHidden = true;

// Mounted guns (truck turret, gun emplacements: items 0x012C-0x012F / 0x0131, uWpGunEmpRide / uWpGunEmpJeep) aim
// with the animation-driven camera behind the gun: the shots missed the crosshair when "keep the normal camera in
// melee" forced the shoulder camera there (user, 2026-10-14). The user's camera values themselves are kept.
bool g_mountedGun = false;
bool g_scopedRifle = false;  // player 1 aims with a scoped rifle: the patch is lifted too
// "Keep the normal camera in melee" forces the normal camera over the animation-driven one, which mounted guns use
// too (camera behind the gun, aligned with its crosshair): while a mounted gun is held the patch is lifted.
mem::Patch g_keepInMelee("camera.keep_in_melee", 0x441F71, { 0x76, 0x14 }, { 0xEB, 0x14 });
bool g_keepInMeleeOn = false;

uint32_t HeldItemId(uintptr_t p) {
    uintptr_t held = 0;
    if (!p || !mem::Read(p + 0x2634, held) || !held) return 0;
    for (int k = 0; k < 24; ++k) {
        const uintptr_t item = p + 0x21A4 + 0x30 * k;
        uintptr_t unit = 0;
        uint32_t id = 0;
        if (mem::Read(item + 0x2C, unit) && unit == held && mem::Read(item + 4, id)) return id & 0xFFFF;
    }
    return 0;
}

bool HoldsMountedGun() {
    const uint32_t id = HeldItemId(actors::InGame() ? actors::Player(0) : 0);
    return id == 0x012C || id == 0x012D || id == 0x012E || id == 0x012F || id == 0x0131;
}

// The rifle scopes too (user, 2026-10-21: with the option on, the scope never showed): while player 1 aims (actions
// 0x1B..0x1F: aim start / loop / end, fire, reload) with the S75, the H&K PSG-1, the Dragunov SVD or the rocket
// launcher 0x010D (its scope too, user 2026-10-27), the patch is lifted. Melee with a rifle in hand keeps the normal
// camera.
bool AimsScopedRifle() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action) || action < 0x1B || action > 0x1F) return false;
    const uint32_t id = HeldItemId(p);
    return id == 0x0105 || id == 0x011C || id == 0x0120 || id == 0x010D;
}

// The normal camera kept in melee loses Chris on some moves (user, 2026-10-26: "it goes into the scenery and comes
// back very late", Jill's second throw): sampled (tools/camsample.py), the game's moves and Wesker's kick ask for the
// animation camera (the callback's request [[ctrl+0x2DE0+0x250]+0x10], priority +4 = 4) and the normal camera stays on
// him (its target +0x190 about 2 m from his hips), but on Jill's throw motion 0x93C its target jumped to (0, 148, 148)
// and stayed there with the request about 30 s after the move: that camera animation only ends while it plays. Then
// (the user's idea) the game's melee camera is given back until its request ends.
constexpr float kLostDistance = 400.0f;  // cm between the normal camera's target and player 1's hips
bool g_meleeCameraLent = false;

uint32_t AnimationCameraPriority(uintptr_t ctrl) {
    uintptr_t holder = 0, request = 0;
    uint32_t priority = 0;
    if (!mem::Read(ctrl + 0x2DE0 + 0x250, holder) || !holder || !mem::Read(holder + 0x10, request) || !request ||
        !mem::Read(request + 4, priority))
        return 0;
    return priority;
}

bool NormalCameraLost(uintptr_t ctrl, uintptr_t p) {
    uintptr_t normal = 0, joints = 0;
    float target[3]{}, hips[3]{};
    if (!mem::Read(ctrl + 0xDF0, normal) || !normal || !mem::SafeRead(reinterpret_cast<void*>(normal + 0x190), target, sizeof(target)) ||
        !mem::Read(p + 0x318, joints) || !joints || !mem::SafeRead(reinterpret_cast<void*>(joints + 0x80), hips, sizeof(hips)))
        return false;
    const float dx = target[0] - hips[0], dy = target[1] - hips[1], dz = target[2] - hips[2];
    return dx * dx + dy * dy + dz * dz > kLostDistance * kLostDistance;
}

// Research (2026-11-02, Lost in Nightmares' doors filmed by the animation camera): every new animation camera request
// logged with player 1's action / motion, the room and the request's first words. Remove once the doors are known.
void LogAnimationCameraRequests() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t holder = 0, ctrl = 0, slot = 0, request = 0;
    if (!p || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE3457C)), holder) || !holder || !mem::Read(holder + 0x34, ctrl) ||
        !ctrl || !mem::Read(ctrl + 0x2DE0 + 0x250, slot) || !slot)
        return;
    mem::Read(slot + 0x10, request);
    static uintptr_t last = 0;
    if (request == last) return;
    last = request;
    uint32_t action = 0, motion = 0, room = 0, words[6]{};
    mem::Read(p + 0x10E0, action);
    mem::Read(p + 0x384, motion);
    uintptr_t profile = 0;
    uint32_t index = 0;
    if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), profile) && profile && mem::Read(profile + 0x20, index) &&
        index < 4)
        mem::Read(profile + index * 0x5940 + 0x273D8, room);
    if (request) mem::SafeRead(reinterpret_cast<void*>(request), words, sizeof(words));
    LOG("camera research: animation camera request %08X (%08X %08X %08X %08X %08X %08X), action %X, motion %X, room %X",
        static_cast<unsigned>(request), words[0], words[1], words[2], words[3], words[4], words[5], action, motion & 0xFFFF,
        room);
}

// Lost in Nightmares' doors (user, 2026-11-02: "the doors open in the first person through the melee camera; keeping
// the normal camera in melee breaks it"): a door uOm020e (vtable RVA 0xBE5988) or uOm020f (0xBE5B30) asks its action 3
// as it opens (action 1 once shut; request core RVA 0x759030, action_log.cpp), player 1 then in action 0x80000000
// playing motion 0xF03, and the animation camera is asked (priority 4) until the door is passed (camera research log,
// room 0x2BC). The kept normal camera is lifted from the door's action until that request ends (3 s without one).
// Every door of that kind, not those two only (user, 2026-11-03: "all the doors on the right of the mansion", the
// emblem's door uOm0207 among them): the doors whose action table getter (vtable +0xE8) is the classic doors' (VA
// 0xAC69A0 -> table VA 0xFE1B28, actions 3-5 OpenClassic; read 2026-11-03: uOm0207 / 0209 / 020e / 020f / 0210), the
// others (VA 0xAC6410 -> VA 0xFE1A98) opening by hand in the third person as in the campaign.
// "Ouverture des portes" (user, 2026-11-03): "Par défaut" leaves each door its own; "Normales" makes the classic getter
// answer the campaign's table (its mov eax, imm32: their actions 3-5 OpenHand), "Classiques" the campaign's getter
// answer the classic one (actions 3-5 OpenClassic, the first person). A door makes its 18 actions from its table when it
// is created (+0x10D0 count, +0x10DC the action objects): the choice applies to the doors loaded after it (user,
// 2026-11-03: "it does not work" on the doors already there).
// "Normales" goes through a hook of the classic getter instead of a patch, so that the doors whose doorway the stage
// walls off keep theirs (user, 2026-11-03: the two ground-floor doors of the mansion's hall, room 0x2BC, blocked Chris
// although open, their own collision +0x1AF0 off - the stage's: the classic event carries the player through).
std::atomic<int> g_doorStyle{ 0 };  // 0 the doors' own, 1 all classic, 2 all as in the campaign
using TableGetterFn = uintptr_t(__fastcall*)(void* door, void* edx);
TableGetterFn g_classicTable = nullptr;

bool DoorRoom(uint32_t& room) {
    uintptr_t profile = 0;
    uint32_t index = 0;
    return mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), profile) && profile &&
           mem::Read(profile + 0x20, index) && index < 4 && mem::Read(profile + index * 0x5940 + 0x273D8, room);
}

// The doors kept classic: room, place (cm).
struct WalledDoor {
    uint32_t room;
    float x, y, z;
};
// The hall's ground floor: its double door on the east side (two leaves, linked +0x1A00) and its single door on the
// west side (uOm0209; user, 2026-11-03: "one works, the second still blocks me").
constexpr WalledDoor kWalledDoors[] = { { 0x2BC, 1228.0f, 0.0f, 471.0f }, { 0x2BC, 1228.0f, 0.0f, 731.0f },
                                        { 0x2BC, -1273.0f, 0.0f, 488.0f } };

uintptr_t __fastcall HookClassicTable(void* door, void* edx) {
    const uintptr_t table = g_classicTable(door, edx);
    if (g_doorStyle.load(std::memory_order_relaxed) != 2) return table;
    float pos[3]{};
    uint32_t room = 0;
    const bool placed = mem::SafeRead(reinterpret_cast<uint8_t*>(door) + 0x30, pos, sizeof(pos));
    DoorRoom(room);
    bool walled = false;
    for (const WalledDoor& w : kWalledDoors) {
        const float dx = pos[0] - w.x, dy = pos[1] - w.y, dz = pos[2] - w.z;
        if (placed && room == w.room && dx * dx + dy * dy + dz * dz < 60.0f * 60.0f) walled = true;
    }
    LOG("camera: door %08X (room %X, at %.0f %.0f %.0f) %s", static_cast<unsigned>(reinterpret_cast<uintptr_t>(door)), room,
        pos[0], pos[1], pos[2], walled ? "kept classic (walled doorway)" : "opens as in the campaign");
    return walled ? table : reinterpret_cast<uintptr_t>(mem::Rva(0xBE1A98));
}

bool EnsureClassicTableHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x6C69A0);
    // mov eax, 0xFE1B28; ret
    const uint8_t expected[] = { 0xB8, 0x28, 0x1B, 0xFE, 0x00, 0xC3 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("camera: classic door table getter, unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookClassicTable), reinterpret_cast<void**>(&g_classicTable));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("camera: classic door table getter hooked at %p", target);
    return true;
}

// "Classiques" (user, 2026-11-03, after the classic table on the campaign doors failed - they lack its motions: "keep
// it but change its meaning: during a door's opening the camera goes to the first person, the trainer's, and if the
// first person option is off it goes back to the third at the animation's end"): see TickDoorFirstPerson.
std::atomic<bool> g_doorFirstPersonOn{ false };
std::atomic<DWORD> g_doorOpenedAt{ 0 };
bool g_doorCameraSeen = false;  // render thread
bool g_doorCamera = false;

bool DoorCameraPlaying() {
    const DWORD opened = g_doorOpenedAt.load(std::memory_order_relaxed);
    if (!opened) return false;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t holder = 0, ctrl = 0;
    const bool requested = p && mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE3457C)), holder) && holder &&
                           mem::Read(holder + 0x34, ctrl) && ctrl && AnimationCameraPriority(ctrl) != 0;
    if (!g_doorCamera) {
        g_doorCamera = true;
        LOG("camera: Lost in Nightmares door opening - the game's camera until it is passed");
    }
    if (requested) {
        g_doorCameraSeen = true;
        return true;
    }
    // The door's camera over, or it never came (signed: `opened` may be a tick ahead, its low bit set).
    if (g_doorCameraSeen || static_cast<int32_t>(GetTickCount() - opened) > 3000) {
        g_doorOpenedAt = 0;
        g_doorCameraSeen = false;
        g_doorCamera = false;
        LOG("camera: Lost in Nightmares door passed, normal camera kept again");
        return false;
    }
    return true;
}

// Research (2026-11-02, Lost in Nightmares' fire where Chris burns a paper): player 1's current action object (P+0x10F8,
// its vtable RVA = its class in re5_vtables.tsv) logged at each change. Remove once the fire is known.
void LogPlayerActionObject() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t object = 0, vtable = 0;
    if (!p || !mem::Read(p + 0x10F8, object)) return;
    if (object) mem::Read(object, vtable);
    static uintptr_t last = 0;
    if (vtable == last) return;
    last = vtable;
    uint32_t action = 0, motion = 0;
    mem::Read(p + 0x10E0, action);
    mem::Read(p + 0x384, motion);
    LOG("action research: player 1's action object vtable RVA %06X, action %X, motion %X",
        static_cast<unsigned>(vtable ? vtable - reinterpret_cast<uintptr_t>(mem::Base()) : 0), action, motion & 0xFFFF);
}

void TickLentMeleeCamera(bool keep) {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t holder = 0, ctrl = 0;
    if (!p || !keep || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE3457C)), holder) || !holder ||
        !mem::Read(holder + 0x34, ctrl) || !ctrl) {
        g_meleeCameraLent = false;
        return;
    }
    const uint32_t priority = AnimationCameraPriority(ctrl);
    if (g_meleeCameraLent && !priority) {
        g_meleeCameraLent = false;
        LOG("camera: the animation camera's request ended, normal camera kept again");
    } else if (!g_meleeCameraLent && priority && NormalCameraLost(ctrl, p)) {
        g_meleeCameraLent = true;
        uint32_t action = 0, motion = 0;
        mem::Read(p + 0x10E0, action);
        mem::Read(p + 0x384, motion);
        LOG("camera: normal camera lost (action %X, motion %X, request priority %u): the game's melee camera until it ends",
            action, motion & 0xFFFF, priority);
    }
}

void WriteGameValues(bool evenEnabled = false) {
    for (const CamField& field : kFields) {
        const Feature* f = Find(field.id);
        if (!f || (f->enabled && !evenEnabled)) continue;
        if (const uintptr_t block = CameraBlock(field.block)) mem::Write(block + 4 * field.index, field.def);
    }
}

void ActivatePreset(int index) {
    g_activePreset = index;
    for (Feature* f : CameraOptions())
        ApplyEntry(*f, index >= 0 ? Entry(index, *f) : PresetEntry{ false, f->kind == Kind::Choice ? 0.0f : f->value });
    g_gameValuesPending = true;
    SavePresets();
    LOG("camera preset %d active", index + 1);
}

void LoadPresets() {
    for (int i = 0; i < kPresets; ++i) g_presets[i].name = i == 0 ? "Défaut" : "Preset " + std::to_string(i + 1);
    FILE* file = nullptr;
    int version = 1;
    if (!_wfopen_s(&file, PresetsPath().c_str(), L"rb") && file) {
        char line[512];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text(line);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            const size_t eq = text.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = text.substr(0, eq), value = text.substr(eq + 1);
            int i = 0;
            if (key == "active") g_activePreset = std::atoi(value.c_str());
            else if (key == "version") version = std::atoi(value.c_str());
            else if (sscanf_s(key.c_str(), "name%d", &i) == 1 && i > 0 && i < kPresets) g_presets[i].name = value;
            else if (key.size() > 3 && key[0] == 'p' && sscanf_s(key.c_str(), "p%d.", &i) == 1 && i >= 0 && i < kPresets) {
                int on = 0;
                float v = 0;
                if (sscanf_s(value.c_str(), "%d,%f", &on, &v) == 2)
                    g_presets[i].entries[key.substr(key.find('.') + 1)] = PresetEntry{ on != 0, v };
            }
        }
        std::fclose(file);
        if (g_activePreset < -1 || g_activePreset >= kPresets) g_activePreset = -1;
        if (version < 2) {
            for (int i = 1; i < kPresets; ++i)
                for (Feature* f : CameraOptions())
                    if (PresetEntry& entry = Entry(i, *f); !entry.on) entry.value = GameValue(*f);
            SavePresets();
        }
    } else {
        // First run: "Défaut" = the game camera; the options the user already had become preset 2.
        bool any = false;
        for (Feature* f : CameraOptions()) {
            Entry(1, *f) = PresetEntry{ f->wanted, f->wanted ? f->value : GameValue(*f) };
            any = any || f->wanted;
        }
        if (any) g_presets[1].name = "Mon réglage";
        g_activePreset = any ? 1 : 0;
        SavePresets();
    }
    for (int i = 0; i < kPresets; ++i) RenamePresetAction(i);
}

// Keeps the active preset equal to the live options (hotkeys, option rows) and saves changes after a second.
void UpdatePresets() {
    if (!g_presetsLoaded) {
        g_presetsLoaded = true;
        LoadPresets();
        if (g_activePreset >= 0) ActivatePreset(g_activePreset);
    }
    if (g_activePreset >= 0)
        for (Feature* f : CameraOptions()) {
            PresetEntry& entry = Entry(g_activePreset, *f);
            const bool choice = f->kind == Kind::Choice;
            const float value = HasSlider(*f) || choice ? f->value : entry.value;
            const bool on = choice ? f->value != 0.0f : f->wanted;
            if (entry.on != on || entry.value != value) {
                entry.on = on;
                entry.value = value;
                MarkPresetsDirty();
            }
        }
    if (g_gameValuesPending && CameraBlock(1)) {
        WriteGameValues();
        g_gameValuesPending = false;
    }
    const bool mounted = HoldsMountedGun(), scoped = AimsScopedRifle();
    if (mounted != g_mountedGun || scoped != g_scopedRifle) {
        if (mounted != g_mountedGun) LOG("camera: mounted gun %s", mounted ? "taken" : "left");
        g_mountedGun = mounted;
        g_scopedRifle = scoped;
    }
    // The first person keeps the normal camera through the animations' camera too (user, 2026-11-01: the moves and
    // crossings gave the third person back), unless its options give the third person back for them.
    const bool keep = g_keepInMeleeOn || FirstPersonKeepsNormalCamera();
    LogAnimationCameraRequests();
    LogPlayerActionObject();
    TickLentMeleeCamera(keep);
    const bool lifted = g_mountedGun || g_scopedRifle || g_meleeCameraLent || VolcanoFinishPlaying() || DoorCameraPlaying();
    if (keep && g_keepInMelee.IsApplied() == lifted) {
        if (lifted) g_keepInMelee.Restore();
        else g_keepInMelee.Apply();
    } else if (!keep && g_keepInMelee.IsApplied()) {
        g_keepInMelee.Restore();
    }
    ReleaseStaleFirstPerson();
    const uintptr_t p1 = actors::InGame() && LevelSettled() ? actors::Player(0) : 0;
    const bool hideSheath = p1 && FirstPersonHidesSheath(p1);
    if (p1 && (p1 != g_sheathPlayer || hideSheath != g_sheathHidden)) {
        RefreshModelState(p1);
        LOG("camera: player 1's sheathed knife %s", hideSheath ? "hidden (first person)" : "shown");
        g_sheathPlayer = p1;
        g_sheathHidden = hideSheath;
    }
    if (g_presetsDirty && GetTickCount64() - g_presetsDirtySince > 1000) SavePresets();
}

void DrawCameraPanel() {
    ImGui::TextDisabled(i18n::Tr("Cinq presets : cocher « Activer ce preset » applique ses réglages et décoche les autres. "
                        "Aucun preset actif = caméra du jeu."));
    // The preset sub-tabs stay in view, each one scrolls below them with the shortcuts (user, 2026-10-23).
    const auto footer = [] {
        ImGui::Separator();
        if (ImGui::CollapsingHeader(i18n::Tr("Raccourcis : activer un preset")))
            DrawOptions("Presets caméra");
        if (ImGui::CollapsingHeader(i18n::Tr("Réglages en cours (preset actif) et leurs raccourcis")))
            DrawOptions("Caméra");
        ImGui::EndChild();
    };
    if (ImGui::BeginTabBar("presets")) {
        for (int i = 0; i < kPresets; ++i) {
            const std::string label = g_presets[i].name + (i == g_activePreset ? "  (actif)" : "") + "###preset" +
                                      std::to_string(i);
            if (!ImGui::BeginTabItem(label.c_str())) continue;
            ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_None);
            ImGui::PushID(i);
            bool active = g_activePreset == i;
            if (ImGui::Checkbox(i18n::Tr("Activer ce preset"), &active)) ActivatePreset(active ? i : -1);
            if (i == 0) {
                ImGui::TextDisabled(i18n::Tr("Caméra d'origine du jeu : toutes les options de caméra désactivées."));
                ImGui::PopID();
                footer();
                ImGui::EndTabItem();
                continue;
            }
            ImGui::SameLine();
            char name[48];
            std::snprintf(name, sizeof(name), "%s", g_presets[i].name.c_str());
            ImGui::SetNextItemWidth(200 * ImGui::GetFontSize() / 13.0f);
            if (ImGui::InputText("Nom", name, sizeof(name))) {
                g_presets[i].name = name;
                RenamePresetAction(i);
                MarkPresetsDirty();
            }
            if (ImGui::BeginTable("options", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn(i18n::Tr("Option"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(i18n::Tr("Valeur"), ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
                for (Feature* f : CameraOptions()) {
                    PresetEntry& entry = Entry(i, *f);
                    ImGui::PushID(f->id.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const bool choice = f->kind == Kind::Choice;
                    if (choice) {
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(i18n::Tr(f->name.c_str()));
                    } else if (ImGui::Checkbox(f->name.c_str(), &entry.on)) {
                        MarkPresetsDirty();
                        if (active) ApplyEntry(*f, entry);
                    }
                    if (!f->help.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr(f->help.c_str()));
                    ImGui::TableSetColumnIndex(1);
                    if (choice && !f->choices.empty()) {
                        int index = std::clamp(static_cast<int>(entry.value + 0.5f), 0, static_cast<int>(f->choices.size()) - 1);
                        ImGui::SetNextItemWidth(220 * ImGui::GetFontSize() / 13.0f);
                        if (ImGui::BeginCombo("##choice", i18n::Tr(f->choices[index].c_str()))) {
                            for (int c = 0; c < static_cast<int>(f->choices.size()); ++c) {
                                if (ImGui::Selectable(i18n::Tr(f->choices[c].c_str()), c == index) && c != index) {
                                    entry.value = static_cast<float>(c);
                                    entry.on = c != 0;
                                    MarkPresetsDirty();
                                    if (active) ApplyEntry(*f, entry);
                                }
                            }
                            ImGui::EndCombo();
                        }
                    } else if (HasSlider(*f)) {
                        ImGui::SetNextItemWidth(220 * ImGui::GetFontSize() / 13.0f);
                        if (widgets::SliderFloat("##value", &entry.value, f->valueMin, f->valueMax, f->valueFormat)) {
                            MarkPresetsDirty();
                            if (active) ApplyEntry(*f, entry);
                        }
                    }
                    // Back to the game's value (the slider) or off (options without a value).
                    ImGui::TableSetColumnIndex(2);
                    const float game = GameValue(*f);
                    const bool atGame = HasSlider(*f) || choice ? entry.value == game : !entry.on;
                    if (atGame) ImGui::BeginDisabled();
                    if (ImGui::SmallButton(i18n::Tr("Valeur du jeu"))) {
                        if (HasSlider(*f) || choice) entry.value = game;
                        if (!HasSlider(*f)) entry.on = false;
                        MarkPresetsDirty();
                        if (active) ApplyEntry(*f, entry);
                    }
                    if (atGame) ImGui::EndDisabled();
                    if (HasSlider(*f) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        char text[48];
                        std::snprintf(text, sizeof(text), f->valueFormat, game);
                        ImGui::SetTooltip(i18n::Tr("Valeur du jeu : %s"), text);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
            footer();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

bool CameraBlocksReady() { return CameraOwner() != 0; }

// Only in a running level, a few seconds after the load, and only if the block looks like camera settings.
// During a load (menu "Continue", checkpoint restart) the camera is destroyed and its memory reused while the
// chain above still points there; right after the load it leads to another object. Writing the sliders'
// values there corrupted the game (d3d9 reading 0xC3000000 = -128.0, a write through 0xC3828000 = -261.0:
// the user's distance sliders, 2026-10-13).
uintptr_t CameraBlock(int slot) {  // slot 0..5 = +0x2A4 .. +0x2B8
    if (!LevelSettled()) return 0;
    uintptr_t owner = CameraOwner(), block = 0;
    if (!owner || !mem::Read(owner + 0x2A4 + 4 * slot, block) || !LooksLikeCameraBlock(block)) return 0;
    return block;
}

// Copies the camera builds when it is set up (RVA 0x43D530, at level load): block values plus per-camera offsets,
// the horizontal offsets negated when the player is left-handed at that moment (+0x1AF9). Normal camera:
// +0x2C0/+0x2F0/+0x320 (RVA 0x4451A0), aiming: +0x350/+0x380/+0x3B0 (RVA 0x445200); same layout as the blocks.
// A parameter block (or setup copy) is still valid: level settled and the memory still looks like camera values.
bool IsCameraParams(uintptr_t block) { return LevelSettled() && LooksLikeCameraBlock(block); }

uintptr_t CameraOverride(int k) {  // k 0..5
    static const uintptr_t offsets[6] = { 0x2C0, 0x2F0, 0x320, 0x350, 0x380, 0x3B0 };
    if (k < 0 || k >= 6 || !LevelSettled()) return 0;
    uintptr_t owner = CameraOwner();
    if (!owner || !LooksLikeCameraBlock(owner + offsets[k])) return 0;
    return owner + offsets[k];
}

// movement.cpp: the RE6 locomotion keeps the camera free through melee moves only over the normal camera.
bool NormalCameraInMelee() { return g_keepInMeleeOn; }

bool FirstPersonOn();  // below: movement.cpp sets the free camera and the RE6 locomotion aside meanwhile
const char* UnitClassName(uintptr_t object);  // parry.cpp: its DTI class name

namespace {
// ---- Field of view and first-person view (user, 2026-11-01: "a FOV slider, a FOV for the first person, and a first
// person view with the character's head invisible"). The camera controller uCameraCtrl ([[RVA 0xE3457C]+0x34], vtable
// RVA 0xB6CA30, its update vtable +0x1C = VA 0x841C50) updates its active camera (+0x1B0: the normal / aim camera
// uCameraQFPS at +0xE00, the animations' uCameraCallback at +0x2DE0, the events' others) through its vtable +0x40 (VA
// 0x842133), then copies its eye +0x170, up +0x180, target +0x190 and FOV +0x1A0 (degrees: 42.6 with the normal
// camera's block value 42.5) into its own (VA 0x84213B-0x8421BC), which the view is made from. Ours go into that copy,
// through a cave at VA 0x8421C2 (the instruction after the FOV's: mov cl, [eax+0x162]), so no camera feeds them back.
// FOV: a factor on the normal camera's 42.5 for the gameplay cameras (normal, aim, melee: the aim's zoom kept), the
// events' left alone. First person (normal / aim camera only, player 1 on foot): the eye at his head (joint id 4, 9 cm
// up and 9 forward along his facing), the camera's own direction kept (the mouse / stick look as usual), its FOV; the
// head scaled to almost nothing meanwhile in the joint (+0x30) and its 8 motion layer slots (record +0xD8, 0x60 per
// joint, scale +0x50: the layers apply them every frame, as player.cpp's accessories), back to 1 after.
constexpr float kGameFov = 42.5f;  // the normal camera's own (its block [9])
std::atomic<bool> g_fovOn{ false }, g_firstPerson{ false };
std::atomic<bool> g_firstPersonOption{ false };  // "Vue à la première personne" itself (g_firstPerson also during a door)
std::atomic<float> g_fov{ kGameFov }, g_firstPersonFov{ 75.0f };
std::atomic<float> g_eyeOffset[3] = { 9.0f, 0.0f, 9.0f };  // cm from the head: up, to his right, ahead
// Iron sights (user, 2026-11-04: "the view puts the weapon's sights in line, as a first-person aim, only while aiming";
// and "the third person goes to the first person while aiming", an iron sight too when the first is on): SightLine.
std::atomic<bool> g_ironFirst{ false }, g_ironThird{ false };
std::atomic<float> g_ironDistance{ 20.0f }, g_ironHeight{ 0.0f };  // cm behind the weapon's back end / above its top
std::atomic<float> g_ironDistanceStock{ 0.0f };  // the same for every weapon but the handguns and magnums
std::atomic<float> g_ironRollDegrees{ 8.0f };  // the camera's roll while aiming with the sights (+ = to the right)
std::atomic<bool> g_ironThirdNow{ false };  // the first person made for an aim in the third person
std::atomic<float> g_ironWeight{ 0.0f };    // 0 the head's eye .. 1 the sights' (game thread)
// The near clip in first person (user, 2026-11-01: "whatever the camera touches is invisible, his shoulder, his knife"):
// the controller's own +0x1C, 16 cm (far +0x18 4e6, aspect +0x20, FOV +0x24: its projection VA 0x835C50 -> VA
// 0x6D1650, read several times a frame; the cameras' own +0x1C copies are never read). Ours while first person
// applies, the game's put back after (when still ours). At 0.5 cm the lights flickered everywhere "like a
// corruption" (user, 2026-11-01: the depth's precision against the far 4e6 cm); 2 cm was clean: kMinNear.
constexpr float kMinNear = 2.0f;
std::atomic<float> g_firstPersonNear{ 3.0f };
uintptr_t g_nearCtrl = 0;  // game thread: the controller whose near is ours
float g_savedNear = 0.0f, g_ourNear = 0.0f;

void SetFirstPersonNear(uintptr_t ctrl) {
    float now = 0.0f;
    if (!mem::Read(ctrl + 0x1C, now)) return;
    if (g_nearCtrl != ctrl || now != g_ourNear) {  // a new controller, or the game set its own since
        g_nearCtrl = ctrl;
        g_savedNear = now;
    }
    g_ourNear = std::clamp(g_firstPersonNear.load(std::memory_order_relaxed), kMinNear, 16.0f);
    mem::Write(ctrl + 0x1C, g_ourNear);
}

void RestoreNear(uintptr_t ctrl) {
    float now = 0.0f;
    if (g_nearCtrl && g_nearCtrl == ctrl && mem::Read(ctrl + 0x1C, now) && now == g_ourNear)
        mem::Write(ctrl + 0x1C, g_savedNear);
    g_nearCtrl = 0;
}

uintptr_t CameraCtrl() {
    uintptr_t a = 0, ctrl = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE3457C)), a) || !a || !mem::Read(a + 0x34, ctrl)) return 0;
    return ctrl;
}
uintptr_t g_headPlayer = 0;  // game thread: the player whose head is shrunk
// The body very see-through in first person (user, 2026-11-01): every character's camera fade (a virtual method, VA
// 0xB57710, no argument) measures for each of the 2 viewports its camera's eye against a point of the body and puts
// the ratio through VA 0xB576C0(viewport, value): P+0x1724[viewport] = value, then the camera manager's ramp (VA
// 0xB55410: near +0x3090 0.3, far +0x3094 0.7, opacities +0x3098 1 / +0x309C 0) gives P+0x1734[viewport], the
// opacity. The camera's own code sets it too (VA 0xB576C0's callers at VA 0x839E21, 0x83D2C3...: still 0.4 / 0.25
// read in first person after the first try): so the setter itself is hooked - player 1's value 1 (opaque) in first
// person whoever sets it - and the fade too, for its direct ramp calls (VA 0xB57A1F...). (Giving the controller's own
// eye +0x30 the third-person one instead moved the picture back: the render reads it.)
std::atomic<bool> g_firstPersonApplied{ false };  // game thread: this frame's view made first person
ULONGLONG g_firstPersonTick = 0;                  // game thread: when it last was
float g_firstPersonHeadAt[3]{};                   // game thread: the head joint's place then

// Research (2026-11-04, the hands jump sideways in the picture, not in the updates): the last 16 first person updates'
// eye and right hand (joint id 8), numbered, to find which update the picture's camera and body come from.
struct UpdateNote {
    uint32_t number = 0;
    float eye[3]{}, hand[3]{};
};
UpdateNote g_updateNotes[16]{};
uint32_t g_updateNumber = 0;
// During a burst: every update's number, eye and right hand (offline analysis, research/fp_turns/burst*.txt).
std::atomic<bool> g_burstUpdates{ false };
char g_burstUpdateText[256][128];
std::atomic<int> g_burstUpdateCount{ 0 };

bool RightHand(uintptr_t p, float out[3]) {
    uintptr_t joints = 0;
    uint32_t count = 0;
    if (!p || !mem::Read(p + 0x318, joints) || !joints || !mem::Read(p + 0x314, count)) return false;
    for (uint32_t j = 0; j < std::min<uint32_t>(count, 256); ++j) {
        uint8_t id = 0;
        if (mem::Read(joints + 0x90 * j + 7, id) && id == 8)
            return mem::SafeRead(reinterpret_cast<void*>(joints + 0x90 * j + 0x80), out, sizeof(float) * 3);
    }
    return false;
}

std::atomic<bool> g_burstOn{ false };  // a burst armed or running (ResearchBurst): the updates noted

void NoteUpdate(uintptr_t p, const float eye[3]) {
    if (!g_burstOn.load(std::memory_order_relaxed)) return;
    UpdateNote& note = g_updateNotes[++g_updateNumber % 16];
    note.number = g_updateNumber;
    std::memcpy(note.eye, eye, sizeof(note.eye));
    if (!RightHand(p, note.hand)) std::memset(note.hand, 0, sizeof(note.hand));
    if (g_burstUpdates.load(std::memory_order_relaxed)) {
        const int n = g_burstUpdateCount.load(std::memory_order_relaxed);
        if (n < 256) {
            sprintf_s(g_burstUpdateText[n], "U %u eye %.3f %.3f %.3f hand %.3f %.3f %.3f", note.number, note.eye[0], note.eye[1],
                      note.eye[2], note.hand[0], note.hand[1], note.hand[2]);
            g_burstUpdateCount.store(n + 1, std::memory_order_relaxed);
        }
    }
}

// The eye's own steadying (user, 2026-11-04: once the eye's place is no longer eased by the game - no lag any more -
// "it still trembles when I walk aiming": the head's bob, which the easing hid): the eye's place against player 1's
// (P+0x30) is eased, the less the more it moves in a frame (a change of kFollowCm or more a frame passes whole: the rolls,
// the blows), and his own move is never eased (the jumps down, the runs: no headless body).
// The ease: a quarter of a second while the eye stays within kDeadCm of where it is eased to (the walk's bob: the
// weapon swayed +-1.5 cm against the eye, about 3 degrees at its 30 cm - user, "the hands tremble walking"), whole
// beyond kDeadCm + kFollowCm (the rolls, the blows, the getups).
constexpr float kDeadCm = 3.0f, kFollowCm = 10.0f, kSteadySeconds = 0.25f;
float g_steadyLocal[3]{};
uintptr_t g_steadyPlayer = 0;
ULONGLONG g_steadyTick = 0;

// Research (2026-11-04, "walking while aiming it trembles"): with an empty DataDir\\fp_trace, 600 first person updates
// (game thread, into a static buffer: no allocation there - a first try with a growing string ended in a heap crash)
// written to DataDir\\fp_trace.txt by the overlay's frame: time (ms), action, player 1's place and yaw, the head, the eye,
// the look.
// The neck is hidden with the head (user, 2026-11-01: "sprinting, more leaned, I still see the neck"): its joint (the
// head's parent, id 3, its only child the head); the head's place is made from the neck's world matrix (rows
// normalized) and the head's bind offset (+0x10), as if the neck were whole.
int g_headJoint = -1, g_neckJoint = -1, g_chestJoint = -1;
// BodyTurn's last decision, for the trace (user, 2026-11-10: "I want it to follow the turn, but it does not go round properly"): the head's
// continuous tilt, the turn's speed, the path (0 none, 1 whole, 2 cut at the look-up limit, 3 looking up already), the scale kept, the
// up's levelling.
float g_traceTilt = 0.0f, g_traceSpeed = 0.0f, g_traceScale = 0.0f, g_traceLevel = 0.0f;
int g_tracePath = 0;
float g_traceLook[3]{}, g_traceAxes[3][3]{};  // the game's look and the head's axes (x, y, z rows) given to BodyTurn
constexpr int kTraceLines = 600, kTraceLine = 720;
char g_traceText[kTraceLines * kTraceLine];
std::atomic<int> g_traceLines{ -1 };  // -1: off; 0..kTraceLines-1: filling; kTraceLines: full, to be written
// Armed, the updates go round a ring (user, 2026-11-10: the flip came later than the first tilt that started it); a flip's end writes it,
// oldest first (g_traceRing: the next slot, g_traceRingWrapped: it went round once).
int g_traceRing = 0;
bool g_traceRingWrapped = false;
std::atomic<bool> g_traceFlipEnded{ false };
std::atomic<ULONGLONG> g_traceCheck{ 0 };

void ResearchFirstPersonTrace(uintptr_t p, const float at[3], const float head[3], const float dir[3], float yaw) {
    const int n = g_traceLines.load(std::memory_order_relaxed);
    if (n < 0 || n >= kTraceLines) return;
    LARGE_INTEGER qpc{}, freq{};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&freq);
    float root[3]{};
    uint32_t action = 0;
    mem::SafeRead(reinterpret_cast<void*>(p + 0x30), root, sizeof(root));
    mem::Read(p + 0x10E0, action);
    // The hands (joint ids 8 right, 13 left: world place +0x80) and the layers 0 / 7 frames (the "hands tremble" research).
    float hands[2][3]{};
    uintptr_t joints = 0;
    uint32_t count = 0;
    if (mem::Read(p + 0x318, joints) && joints && mem::Read(p + 0x314, count))
        for (uint32_t j = 0; j < std::min<uint32_t>(count, 256); ++j) {
            uint8_t id = 0;
            if (!mem::Read(joints + 0x90 * j + 7, id) || (id != 8 && id != 13)) continue;
            mem::SafeRead(reinterpret_cast<void*>(joints + 0x90 * j + 0x80), hands[id == 8 ? 0 : 1], sizeof(hands[0]));
        }
    float frame0 = 0.0f, frame7 = 0.0f;
    mem::Read(p + 0x380 + 0x18, frame0);
    mem::Read(p + 0x380 + 0xE0 * 7 + 0x18, frame7);
    // The body's turns ("Chris shifts sideways and is pulled back", user, 2026-11-04): the unit quaternion +0x40, the
    // world yaw +0x1024, the mouse's aim yaw +0x2A10, the aim camera's +0x2A24, the hips' (joint 0) and the chest's
    // facing (their world Z rows +0x70), the view's direction; degrees.
    constexpr float kDeg = 57.2957795f;
    float quat[4]{}, world = 0.0f, mouse = 0.0f, aim = 0.0f, hipsZ[3]{}, chestZ[3]{};
    mem::SafeRead(reinterpret_cast<void*>(p + 0x40), quat, sizeof(quat));
    mem::Read(p + 0x1024, world);
    mem::Read(p + 0x2A10, mouse);
    mem::Read(p + 0x2A24, aim);
    if (joints) {
        mem::SafeRead(reinterpret_cast<void*>(joints + 0x70), hipsZ, sizeof(hipsZ));
        if (g_chestJoint >= 0) mem::SafeRead(reinterpret_cast<void*>(joints + 0x90 * g_chestJoint + 0x70), chestZ, sizeof(chestZ));
    }
    const int slot = g_traceRing;
    sprintf_s(g_traceText + slot * kTraceLine, kTraceLine,
              "%.3f %X root %.2f %.2f %.2f yaw %.3f head %.2f %.2f %.2f eye %.2f %.2f %.2f rh %.2f %.2f %.2f lh %.2f %.2f %.2f "
              "f0 %.2f f7 %.2f quat %.3f world %.3f mouse %.3f aim %.3f hips %.3f chest %.3f view %.3f pitch %.1f tilt %.0f speed %.0f "
              "path %d scale %.2f level %.2f look %.2f %.2f %.2f hx %.2f %.2f %.2f hy %.2f %.2f %.2f hz %.2f %.2f %.2f dir %.2f %.2f %.2f",
              static_cast<double>(qpc.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart), action, root[0], root[1], root[2],
              yaw * kDeg, head[0], head[1], head[2], at[0], at[1], at[2], hands[0][0], hands[0][1], hands[0][2],
              hands[1][0], hands[1][1], hands[1][2], frame0, frame7, 2.0f * std::atan2(quat[1], quat[3]) * kDeg, world * kDeg,
              mouse * kDeg, aim * kDeg, std::atan2(hipsZ[0], hipsZ[2]) * kDeg, std::atan2(chestZ[0], chestZ[2]) * kDeg,
              std::atan2(dir[0], dir[2]) * kDeg, std::asin(std::clamp(dir[1], -1.0f, 1.0f)) * kDeg, g_traceTilt, g_traceSpeed,
              g_tracePath, g_traceScale, g_traceLevel, g_traceLook[0], g_traceLook[1], g_traceLook[2], g_traceAxes[0][0], g_traceAxes[0][1],
              g_traceAxes[0][2], g_traceAxes[1][0], g_traceAxes[1][1], g_traceAxes[1][2], g_traceAxes[2][0], g_traceAxes[2][1], g_traceAxes[2][2],
              dir[0], dir[1], dir[2]);
    g_traceRing = (g_traceRing + 1) % kTraceLines;
    if (g_traceRing == 0) g_traceRingWrapped = true;
    if (g_traceFlipEnded.exchange(false)) g_traceLines.store(kTraceLines, std::memory_order_release);  // a flip's end: written
}

// The overlay's frame: starts a trace when DataDir\\fp_trace appears, writes it when full.
void TickFirstPersonTrace() {
    const int n = g_traceLines.load(std::memory_order_acquire);
    if (n >= kTraceLines) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, (logx::DataDir() + L"\\fp_trace.txt").c_str(), L"w") == 0 && f) {
            const int first = g_traceRingWrapped ? g_traceRing : 0, count = g_traceRingWrapped ? kTraceLines : g_traceRing;
            for (int k = 0; k < count; ++k) {
                const int i = (first + k) % kTraceLines;
                fputs(g_traceText + i * kTraceLine, f);
                fputc('\n', f);
            }
            fclose(f);
        }
        LOG("first person trace written (%d updates)", kTraceLines);
        g_traceLines = -1;
        return;
    }
    if (n >= 0) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_traceCheck.load() < 1000) return;
    g_traceCheck = now;
    const std::wstring flag = logx::DataDir() + L"\\fp_trace";
    if (GetFileAttributesW(flag.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(flag.c_str());
    g_traceRing = 0;
    g_traceRingWrapped = false;
    g_traceFlipEnded = false;
    g_traceLines = 0;
    LOG("first person trace armed: written at the end of the next flip");
}

void SteadyEye(uintptr_t p, float at[3]) {
    float root[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), root, sizeof(root))) return;
    const float local[3] = { at[0] - root[0], at[1] - root[1], at[2] - root[2] };
    const ULONGLONG now = GetTickCount64();
    const float d[3] = { local[0] - g_steadyLocal[0], local[1] - g_steadyLocal[1], local[2] - g_steadyLocal[2] };
    const float gap = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (g_steadyPlayer != p || now - g_steadyTick > 250 || gap > 60.0f) {
        std::memcpy(g_steadyLocal, local, sizeof(local));
    } else {
        static LARGE_INTEGER last{}, freq{};
        LARGE_INTEGER qpc{};
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&qpc);
        const float dt = last.QuadPart ? std::min(0.1f, static_cast<float>(qpc.QuadPart - last.QuadPart) /
                                                            static_cast<float>(freq.QuadPart))
                                       : 0.0f;
        last = qpc;
        const float ease = 1.0f - std::exp(-dt / kSteadySeconds);
        const float w = ease + (1.0f - ease) * std::clamp((gap - kDeadCm) / kFollowCm, 0.0f, 1.0f);
        for (int i = 0; i < 3; ++i) g_steadyLocal[i] += d[i] * w;
    }
    g_steadyPlayer = p;
    g_steadyTick = now;
    for (int i = 0; i < 3; ++i) at[i] = root[i] + g_steadyLocal[i];
}

// Game thread: the controller whose view this update is the first person's (BlenderHook), 0 = none. Per controller
// (user, 2026-11-04: "walking while aiming the hands and the weapon tremble, a copy of them rises more than the real
// ones when I shoot", the first person only): a single flag was cleared by the other controller's view copy (09CC11A0),
// which comes between the picture's controller's view copy and its blender in most updates (216 of 240, counted); the
// blender's eased eye then reached the picture now and then (full-speed pictures, research/fp_turns/burst_*: one in
// about ten, the eye half a step behind the walk, the hands 30 cm away jumping 22 pixels for one picture). Fixed,
// user-verified.
uintptr_t g_firstPersonFinalCtrl = 0;

// What stays visible in the first person (user, 2026-11-01: "an option saying what stays visible in the first person,
// as in VR: full body, arms, forearms, hands only"). The hidden joints get kHidden in their world matrices (their
// mesh shrinks to their origin), the others their size and place: a joint's local scale is its world weight over its
// parent's, and below a hidden parent its local translation (its bind offset +0x10) is divided by the parent's weight
// - in the joint record (+0x20 rotation, +0x30 scale, +0x40 translation, +0x50.. world matrix) and in the 8 motion
// layers' slots (layer +0xD8, 0x60 per joint: +0x30 rotation, +0x40 translation, +0x50 scale), which the layers
// apply every frame. Every joint keeps its true place: the hurt shapes (their boxes keep their size, only their place
// follows the joint: the head's 24 cm box read with the head shrunk) and the limbs the trainer's blows use are
// untouched. 0 full body: the neck (id 3) and what hangs from it hidden; kept from the upper arms (ids 6 / 11), the
// forearms (7 / 12) or the hands (8 / 13) down otherwise.
constexpr float kHidden = 0.01f;
std::atomic<int> g_bodyMode{ 0 };
struct BodyJoint {
    int index;
    float scale;  // local
    bool moved;   // below a hidden parent: its translation divided by kHidden
    float offset[3];
};
std::vector<BodyJoint> g_body;  // game thread: for g_headPlayer
int g_bodyBuiltMode = -1;

void WriteJointLocal(uintptr_t p, uintptr_t joints, int index, const float* scale, const float* offset) {
    if (scale) mem::SafeWrite(reinterpret_cast<void*>(joints + 0x90 * index + 0x30), scale, 12);
    if (offset) mem::SafeWrite(reinterpret_cast<void*>(joints + 0x90 * index + 0x40), offset, 12);
    for (uint32_t layer = 0; layer < 8; ++layer) {
        uintptr_t slots = 0;
        if (!mem::Read(p + 0x380 + 0xE0 * layer + 0xD8, slots) || !slots) continue;
        if (scale) mem::SafeWrite(reinterpret_cast<void*>(slots + 0x60 * index + 0x50), scale, 12);
        if (offset) mem::SafeWrite(reinterpret_cast<void*>(slots + 0x60 * index + 0x40), offset, 12);
    }
}

void BuildBody(uintptr_t p, int mode) {
    g_body.clear();
    g_bodyBuiltMode = mode;
    uintptr_t joints = 0;
    uint32_t count = 0;
    if (!mem::Read(p + 0x318, joints) || !joints || !mem::Read(p + 0x314, count) || !count || count > 256) return;
    std::vector<int> parent(count, -1);
    std::vector<uint8_t> id(count, 0xFF);
    for (uint32_t j = 0; j < count; ++j) {
        uint8_t up = 0xFF;
        mem::Read(joints + 0x90 * j + 5, up);
        mem::Read(joints + 0x90 * j + 7, id[j]);
        parent[j] = up < count ? up : -1;
    }
    static constexpr uint8_t kKept[4][2] = { { 0, 0 }, { 6, 11 }, { 7, 12 }, { 8, 13 } };
    const auto under = [&](uint32_t j, uint8_t a, uint8_t b) {
        for (int k = static_cast<int>(j), depth = 0; k >= 0 && depth < 64; k = parent[k], ++depth)
            if (id[k] == a || id[k] == b) return true;
        return false;
    };
    std::vector<float> weight(count, 1.0f);
    for (uint32_t j = 0; j < count; ++j) {
        const bool hidden = mode <= 0 ? under(j, 3, 3) : !under(j, kKept[mode][0], kKept[mode][1]);
        weight[j] = hidden ? kHidden : 1.0f;
    }
    for (uint32_t j = 0; j < count; ++j) {
        const float above = parent[j] >= 0 ? weight[parent[j]] : 1.0f;
        BodyJoint entry{ static_cast<int>(j), weight[j] / above, above != 1.0f, {} };
        if (entry.scale == 1.0f && !entry.moved) continue;
        mem::SafeRead(reinterpret_cast<void*>(joints + 0x90 * j + 0x10), entry.offset, sizeof(entry.offset));
        g_body.push_back(entry);
    }
    LOG("camera: first person body mode %d: %zu of player 1's %u joints changed", mode, g_body.size(), count);
}

void ApplyBody(uintptr_t p) {
    uintptr_t joints = 0;
    if (!p || !mem::Read(p + 0x318, joints) || !joints) return;
    for (const BodyJoint& e : g_body) {
        const float s[3] = { e.scale, e.scale, e.scale };
        const float t[3] = { e.offset[0] / kHidden, e.offset[1] / kHidden, e.offset[2] / kHidden };
        WriteJointLocal(p, joints, e.index, s, e.moved ? t : nullptr);
    }
}

void RestoreBody(uintptr_t p) {
    uintptr_t joints = 0;
    if (p && mem::Read(p + 0x318, joints) && joints) {
        static constexpr float kOne[3] = { 1.0f, 1.0f, 1.0f };
        for (const BodyJoint& e : g_body) WriteJointLocal(p, joints, e.index, kOne, e.moved ? e.offset : nullptr);
    }
    g_body.clear();
    g_bodyBuiltMode = -1;
}

// Player 1's actions (the cPlAction_pl_act_* classes by index, read live 2026-11-01): the crossings 0x28 jump_broad_3m,
// 0x29 jump_broad_4m, 0x2A jump_down, 0x2B / 0x2C bump_up / bump_down, 0x2D fence, 0x2E ladder_v_climb, the co-op jumps
// 0x71..0x76, the cliffs 0x7B grasp_cliff_first .. 0x7F coop_climb_cliff_rescue; the melee moves as movement.cpp's
// (0x30..0x4D, 0x6F, the Reunion 0xAC / 0xAD, the trainer's carrier 0x81).
bool TraversalAction(uint32_t a) { return (a >= 0x28 && a <= 0x2E) || (a >= 0x71 && a <= 0x76) || (a >= 0x7B && a <= 0x7F); }
bool MeleeMoveAction(uint32_t a) { return (a >= 0x30 && a <= 0x4D) || a == 0x6F || a == 0xAC || a == 0xAD || a == 0x81; }

// "Première personne : troisième personne pendant les traversées / les coups de mêlée / les saisies ennemies" (user,
// 2026-11-01).
std::atomic<bool> g_thirdInTraversal{ false }, g_thirdInMelee{ false }, g_thirdInGrab{ false };

// Held by an enemy's grab (recorded 2026-11-01: P+0x18DC stays 0 for a player held): his action is the grab's event
// (0x84 setEventAct_wait, 0x81, 0x80000000 a motion played by the event) while an enemy within kGrabReach is in a
// holding action by its class: the Majini's Catch / DashCatch / NeckHang(Dmg) / BackHold(Dmg) (cEm10Act...), the
// licker's mount, the axe man's hanging and execution catch, the spider's AtkCatchHit / Loop.
constexpr float kGrabReach = 250.0f;  // cm

bool HoldingAction(uintptr_t enemy, uint32_t action) {
    uint32_t count = 0;
    uintptr_t table = 0, object = 0;
    if (!mem::Read(enemy + 0x10D0, count) || action >= count || count > 0x200 || !mem::Read(enemy + 0x10DC, table) ||
        !table || !mem::Read(table + 4 * action, object) || !object)
        return false;
    const char* name = UnitClassName(object);
    const char* tail = name ? std::strstr(name, "Act") : nullptr;
    if (!tail) return false;
    for (const char* hold : { "Catch", "Hang", "Hold", "Mount" })
        if (std::strstr(tail, hold)) return true;
    return false;
}

bool GrabbedByEnemy(uintptr_t p, uint32_t action) {
    if (action != 0x84 && action != 0x81 && action != 0x80000000) return false;
    float at[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(p + 0x30), at, sizeof(at))) return false;
    for (uintptr_t enemy : actors::Enemies()) {
        float e[3]{};
        uint32_t its = 0;
        if (!mem::SafeRead(reinterpret_cast<void*>(enemy + 0x30), e, sizeof(e)) || !mem::Read(enemy + 0x10E0, its)) continue;
        const float dx = e[0] - at[0], dy = e[1] - at[1], dz = e[2] - at[2];
        if (dx * dx + dy * dy + dz * dz <= kGrabReach * kGrabReach && HoldingAction(enemy, its)) return true;
    }
    return false;
}
}  // namespace

// action_log.cpp ("Le joueur 2 n'attaque pas"): player `p` held by an enemy (a licker's mount, a Kipepeo's catch...):
// the partner's attacks then are rescues.
bool PlayerHeldByEnemy(uintptr_t p) {
    uint32_t action = 0;
    return p && mem::Read(p + 0x10E0, action) && GrabbedByEnemy(p, action);
}

namespace {

// On foot for the first person: the crossings too (user, 2026-11-01: "the first person does not work in the
// traversals"): recorded, the jump down 0x2A and the bump down 0x2C set his vehicle context +0x364 to 1 (0x101 on
// foot) while the normal camera stays active, and it stays 1 a moment after the landing (wait 0x00): kept
// kCrossingGraceMs after a crossing. The jumps through windows (user: "they stay in the third person") are a motion
// played by an event (action 0x80000000) that begins on foot (context 0x101) and turns the context to 1: an event
// begun on foot that leaves it counts as a crossing to its end (one begun otherwise - a death, a cutscene - does not;
// the grabs stay on foot).
constexpr ULONGLONG kCrossingGraceMs = 400;
constexpr uint32_t kEventAction = 0x80000000;
ULONGLONG g_lastCrossing = 0;  // game thread
uint32_t g_lastAction = 0;
bool g_lastOnFoot = false, g_eventOnFoot = false, g_eventCrossing = false;

bool FirstPersonOnFoot(uintptr_t p) {
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action)) return false;
    const ULONGLONG now = GetTickCount64();
    const bool onFoot = actors::OnFoot(p);
    if (action != kEventAction) {
        g_eventOnFoot = g_eventCrossing = false;
    } else if (g_lastAction != kEventAction) {
        g_eventOnFoot = onFoot || g_lastOnFoot;
    }
    if (action == kEventAction && g_eventOnFoot && !onFoot) g_eventCrossing = true;
    g_lastAction = action;
    g_lastOnFoot = onFoot;
    const bool crossing = TraversalAction(action) || g_eventCrossing;
    if (crossing) g_lastCrossing = now;
    return onFoot || crossing || (g_lastCrossing && now - g_lastCrossing < kCrossingGraceMs);
}

bool FirstPersonPaused(uintptr_t p) {
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action)) return false;
    return (g_thirdInTraversal.load(std::memory_order_relaxed) && (TraversalAction(action) || g_eventCrossing)) ||
           (g_thirdInMelee.load(std::memory_order_relaxed) && MeleeMoveAction(action)) ||
           (g_thirdInGrab.load(std::memory_order_relaxed) && GrabbedByEnemy(p, action));
}

// "Première personne : suivre les rotations du corps" (user, 2026-11-01: "in a roll the camera turns as if it did a
// real roll"; then "only during the dodges and the trainer's moves: in plain play it makes you sick when Chris looks
// at the enemies or runs"): during the dodges (avoid actions 0x4E..0x56) and the trainer's moves (its carrier 0x81),
// the head's turn since the action began (against his facing) is put on the look and the up, times the strength,
// eased in over kTurnInSeconds and out over kTurnOutSeconds; elsewhere the neutral just follows the head.
// The head's turn, no longer the chest's (user, 2026-11-04: "rolling forward the camera seems to roll sideways", "in
// Jill's slide it looks to the side for no reason", then "why not the head?"; recorded, research/fp_turns/dodges.txt:
// the forward roll goes over a shoulder - the chest's pitch and roll alike, 68 / 72 degrees at frame 12 - and the slide
// turns the chest 120-180 degrees about his up and 60 back; the head keeps its look far straighter). Its whole turn: a
// try keeping only its twist across his travel lost the turns about his up (user: "why cut left and right? HUNK's neck
// break turns him round and I see him headless in front of me").
std::atomic<bool> g_bodyRotation{ false };
std::atomic<float> g_bodyRotationStrength{ 1.0f };
constexpr float kTurnInSeconds = 0.08f, kTurnOutSeconds = 0.3f;

// The game's melee moves too (user, 2026-11-04: "the option is ignored anyway outside the actions like the dodges or
// the melee attacks"), and the crossings - their actions and the events begun on foot that leave it (user: "on the
// crossings the camera clearly does not follow the turn"). The melee moves each by the free melee tables' column "Suivi
// 1re pers." (user, 2026-11-05: "follow on the neck break, not on the straight punch"; all ticked by default):
// TickMeleeFollow (render thread) looks the move up - the game's by player 1's action (Player1NativeMoveId), the
// trainer's carrier 0x81 by its playing move (ActiveCombatMoveId), none found: followed.
std::atomic<bool> g_meleeFollows{ true };

bool FollowsBodyTurns(uintptr_t p) {
    uint32_t action = 0;
    return p && mem::Read(p + 0x10E0, action) &&
           ((action >= 0x4E && action <= 0x56) || (MeleeMoveAction(action) && g_meleeFollows.load(std::memory_order_relaxed)) ||
            TraversalAction(action) || g_eventCrossing);
}

void TickMeleeFollow() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action) || !MeleeMoveAction(action)) {
        g_meleeFollows = true;
        return;
    }
    std::string id;
    if (action == 0x81) {
        if (const char* playing = ActiveCombatMoveId()) id = playing;
    } else {
        id = Player1NativeMoveId(p, action);
    }
    g_meleeFollows = id.empty() || MoveFollowsTurns(id);
}

struct Quat {
    float x, y, z, w;
};
constexpr Quat kNoTurn{ 0.0f, 0.0f, 0.0f, 1.0f };

Quat Mul(const Quat& a, const Quat& b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
Quat Conj(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }
float Dot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
Quat Normalized(const Quat& q) {
    const float n = std::sqrt(Dot(q, q));
    return n > 1e-9f ? Quat{ q.x / n, q.y / n, q.z / n, q.w / n } : kNoTurn;
}

// The rotation taking the x, y, z axes to axes[0..2] (world), Shepperd's way.
Quat FromAxes(const float axes[3][3]) {
    const auto m = [&](int r, int c) { return axes[c][r]; };
    const float trace = m(0, 0) + m(1, 1) + m(2, 2);
    Quat q;
    if (trace > 0.0f) {
        const float k = 2.0f * std::sqrt(trace + 1.0f);
        q = { (m(2, 1) - m(1, 2)) / k, (m(0, 2) - m(2, 0)) / k, (m(1, 0) - m(0, 1)) / k, 0.25f * k };
    } else if (m(0, 0) > m(1, 1) && m(0, 0) > m(2, 2)) {
        const float k = 2.0f * std::sqrt(1.0f + m(0, 0) - m(1, 1) - m(2, 2));
        q = { 0.25f * k, (m(0, 1) + m(1, 0)) / k, (m(0, 2) + m(2, 0)) / k, (m(2, 1) - m(1, 2)) / k };
    } else if (m(1, 1) > m(2, 2)) {
        const float k = 2.0f * std::sqrt(1.0f + m(1, 1) - m(0, 0) - m(2, 2));
        q = { (m(0, 1) + m(1, 0)) / k, 0.25f * k, (m(1, 2) + m(2, 1)) / k, (m(0, 2) - m(2, 0)) / k };
    } else {
        const float k = 2.0f * std::sqrt(1.0f + m(2, 2) - m(0, 0) - m(1, 1));
        q = { (m(0, 2) + m(2, 0)) / k, (m(1, 2) + m(2, 1)) / k, 0.25f * k, (m(1, 0) - m(0, 1)) / k };
    }
    return Normalized(q);
}

void Rotate(const Quat& q, float v[3]) {
    const Quat r = Mul(Mul(q, Quat{ v[0], v[1], v[2], 0.0f }), Conj(q));
    v[0] = r.x;
    v[1] = r.y;
    v[2] = r.z;
}

// The same axis, `s` times the angle (the shorter way round).
Quat Scaled(const Quat& q, float s) {
    const Quat a = q.w < 0.0f ? Quat{ -q.x, -q.y, -q.z, -q.w } : q;
    const float half = std::acos(std::min(1.0f, a.w)), sinHalf = std::sin(half);
    if (sinHalf < 1e-6f) return kNoTurn;
    const float k = std::sin(half * s) / sinHalf;
    return { a.x * k, a.y * k, a.z * k, std::cos(half * s) };
}

Quat g_neutral = kNoTurn;  // game thread: the head against his facing before the action
bool g_neutralSet = false;
ULONGLONG g_neutralTick = 0;
float g_turnWeight = 0.0f;  // 0..1, eased
// The look up held below kMaxUpDegrees in slow turns (user, 2026-11-04, Jill's slide: "at the top it seems to mirror, I
// see my legs upside down", "every slide the camera turns another way"; recorded, research/fp_turns/slide.txt: lying on
// her back 0.6 s her head looks 65-77 degrees up and over towards her back, 140 degrees left one time, right the next -
// near the vertical a hair decides the side). While the head turns slower than kFlipDegreesPerSecond (a roll or a flip
// goes round in about half a second) the turn is cut where the look reaches the limit, and the more the full turn
// would pass it, the more the camera's up is levelled (no upside-down picture).
constexpr float kMaxUpDegrees = 55.0f, kFlipDegreesPerSecond = 500.0f, kLevelOverDegrees = 20.0f;
Quat g_lastGap = kNoTurn;
float g_turnSpeed = 0.0f;  // degrees a second, smoothed
float g_levelUp = 0.0f;    // 0..1: this frame's levelling of the camera's up (BodyTurn -> its caller)

// The head's turn followed the whole way round in a flip (user, 2026-11-09: "the camera's inversion during the back flip in the first person";
// 2026-11-10, after a first try kept the tilt short of a flip: "now the camera does not turn at all"): Scaled takes the shorter way, so past
// 180 degrees the view went back the way it came. The turn split into its twist about the up axis and its swing (the tilt); the swing's
// sign kept from one frame to the next so its angle runs on continuously (0..360 over a whole flip), and scaled as that angle.
Quat g_swingPrev = kNoTurn;
// A flip (user, 2026-11-10, trace of a back flip at the normal speed: the head turned at 100-400 degrees a second, under
// kFlipDegreesPerSecond, so the look-up limit held the view from 71 to 110 degrees of tilt and let go at 141 - an 85-degree jump): the
// head's tilt past kFlipTilt latches the flip until the follow ends, no limit then; the scale kept by the limit rises back to the whole
// turn at kRatioRate a second instead of at once.
bool g_flipLatched = false;
bool g_firstPersonTurned = false;  // this update the first person turned the view with the body (BlenderHook gives its up to the picture)
float g_scaleRatio = 1.0f;
constexpr float kFlipTilt = 80.0f, kRatioRate = 5.0f;
struct SplitTurn {
    Quat twist = kNoTurn;
    float axis[3]{};
    float degrees = 0.0f;  // the swing's, continuous
};

SplitTurn SplitTurnOf(const Quat& gap) {
    SplitTurn t;
    t.twist = Normalized({ 0.0f, gap.y, 0.0f, gap.w });
    Quat swing = Mul(gap, Conj(t.twist));
    if (Dot(swing, g_swingPrev) < 0.0f) swing = { -swing.x, -swing.y, -swing.z, -swing.w };
    g_swingPrev = swing;
    const float s = std::sqrt(swing.x * swing.x + swing.y * swing.y + swing.z * swing.z);
    if (s < 1e-6f) return t;
    t.axis[0] = swing.x / s;
    t.axis[1] = swing.y / s;
    t.axis[2] = swing.z / s;
    t.degrees = 2.0f * std::atan2(s, swing.w) * 57.2957795f;  // 0..360
    if (t.degrees > 180.0f) {
        static ULONGLONG told = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - told > 250) {
            told = now;
            LOG("first person turn: the head's tilt %.0f degrees, followed on round", t.degrees);
        }
    }
    return t;
}

// continuous: the swing's angle as it ran (in a flip); else the shorter way (easing out after one: 355 degrees is -5, not a flip played
// backwards - user, 2026-11-10, trace research/fp_turns: the view went back round as the follow eased out).
Quat ComposeTurn(const SplitTurn& t, float scale, bool continuous) {
    const float degrees = !continuous && t.degrees > 180.0f ? t.degrees - 360.0f : t.degrees;
    const float half = 0.5f * scale * degrees / 57.2957795f, sinHalf = std::sin(half);
    return Mul(Quat{ t.axis[0] * sinHalf, t.axis[1] * sinHalf, t.axis[2] * sinHalf, std::cos(half) }, Scaled(t.twist, scale));
}

float ElevationDegrees(const Quat& turn, const float look[3]) {
    float v[3] = { look[0], look[1], look[2] };
    Rotate(turn, v);
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return n > 1e-6f ? std::asin(std::clamp(v[1] / n, -1.0f, 1.0f)) * 57.2957795f : 0.0f;
}

// This frame's turn to put on the view (world), none when the option is off or outside those actions. `look`: the
// game's look direction (for the limit above).
Quat BodyTurn(uintptr_t p, const float axes[3][3], float yaw, const float look[3]) {
    g_levelUp = 0.0f;
    std::memcpy(g_traceLook, look, sizeof(g_traceLook));
    std::memcpy(g_traceAxes, axes, sizeof(g_traceAxes));
    if (!g_bodyRotation.load(std::memory_order_relaxed)) {
        g_neutralSet = false;
        g_turnWeight = 0.0f;
        g_swingPrev = kNoTurn;
        return kNoTurn;
    }
    const float det = axes[0][0] * (axes[1][1] * axes[2][2] - axes[1][2] * axes[2][1]) -
                      axes[0][1] * (axes[1][0] * axes[2][2] - axes[1][2] * axes[2][0]) +
                      axes[0][2] * (axes[1][0] * axes[2][1] - axes[1][1] * axes[2][0]);
    if (det < 0.5f) return kNoTurn;  // a mirrored head: no proper rotation
    const Quat facing{ 0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw) };
    Quat local = Mul(Conj(facing), FromAxes(axes));
    const ULONGLONG now = GetTickCount64();
    if (!g_neutralSet) {
        g_neutral = local;
        g_neutralSet = true;
        g_neutralTick = now;
    }
    if (Dot(local, g_neutral) < 0.0f) local = { -local.x, -local.y, -local.z, -local.w };
    const float dt = std::min(0.1f, static_cast<float>(now - g_neutralTick) / 1000.0f);
    g_neutralTick = now;
    const bool follow = FollowsBodyTurns(p);
    g_turnWeight = follow ? std::min(1.0f, g_turnWeight + dt / kTurnInSeconds)
                         : std::max(0.0f, g_turnWeight - dt / kTurnOutSeconds);
    if (!follow && g_turnWeight <= 0.0f) {  // plain play: the neutral is the head as it is
        g_neutral = local;
        g_swingPrev = kNoTurn;
        g_flipLatched = false;
        g_scaleRatio = 1.0f;
        return kNoTurn;
    }
    const float strength = std::clamp(g_bodyRotationStrength.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const Quat gap = Mul(local, Conj(g_neutral));
    if (dt > 0.0f) {
        const Quat delta = Mul(gap, Conj(g_lastGap));
        const float degrees = 2.0f * std::acos(std::min(1.0f, std::fabs(delta.w))) * 57.2957795f;
        g_turnSpeed = 0.7f * g_turnSpeed + 0.3f * degrees / dt;
    }
    g_lastGap = gap;
    const float k = strength * g_turnWeight;
    const SplitTurn split = SplitTurnOf(gap);
    g_traceTilt = split.degrees;
    g_traceSpeed = g_turnSpeed;
    g_traceScale = k;
    g_tracePath = 1;
    g_traceLevel = 0.0f;
    if (follow && split.degrees > kFlipTilt && split.degrees < 360.0f - kFlipTilt && !g_flipLatched) {
        g_flipLatched = true;
        LOG("first person turn: a flip (the head's tilt %.0f degrees), followed round", split.degrees);
    }
    if (!follow && g_flipLatched) {
        g_flipLatched = false;
        if (g_traceLines.load(std::memory_order_relaxed) >= 0) g_traceFlipEnded = true;
    }
    const auto turnFor = [&](float scale) { return Mul(Mul(facing, ComposeTurn(split, scale, follow)), Conj(facing)); };
    // The kept scale's ratio rises back smoothly (a limit letting go), drops at once (a limit taking hold).
    const auto eased = [&](float target) {
        g_scaleRatio = target < g_scaleRatio ? target : std::min(target, g_scaleRatio + kRatioRate * dt);
        return k * g_scaleRatio;
    };
    const Quat full = turnFor(k);
    const float fullUp = ElevationDegrees(full, look);
    if (g_flipLatched || fullUp <= kMaxUpDegrees || g_turnSpeed >= kFlipDegreesPerSecond) {
        const float scale = eased(1.0f);
        g_traceScale = scale;
        return scale == k ? full : turnFor(scale);
    }
    float lo = 0.0f, hi = k;
    if (ElevationDegrees(turnFor(0.0f), look) >= kMaxUpDegrees) {  // looking up already
        g_tracePath = 3;
        g_traceScale = 0.0f;
        g_scaleRatio = 0.0f;
        return turnFor(0.0f);
    }
    for (int i = 0; i < 12; ++i) {
        const float mid = 0.5f * (lo + hi);
        (ElevationDegrees(turnFor(mid), look) > kMaxUpDegrees ? hi : lo) = mid;
    }
    g_levelUp = std::clamp((fullUp - kMaxUpDegrees) / kLevelOverDegrees, 0.0f, 1.0f);
    g_tracePath = 2;
    g_traceScale = lo;
    g_traceLevel = g_levelUp;
    g_scaleRatio = k > 0.0f ? lo / k : 0.0f;
    static ULONGLONG told = 0;
    if (now - told > 500) {
        told = now;
        LOG("first person turn: look held at %.0f degrees up (the head's %.0f, %.0f deg/s), level %.2f", kMaxUpDegrees,
            fullUp, g_turnSpeed, g_levelUp);
    }
    return turnFor(lo);
}

int HeadJoint(uintptr_t p) {
    uintptr_t joints = 0;
    uint32_t count = 0;
    if (!mem::Read(p + 0x318, joints) || !joints || !mem::Read(p + 0x314, count)) return -1;
    for (uint32_t j = 0; j < std::min<uint32_t>(count, 256); ++j) {
        uint8_t id = 0;
        if (mem::Read(joints + 0x90 * j + 7, id) && id == 4) return static_cast<int>(j);
    }
    return -1;
}

int ParentJoint(uintptr_t p, int joint) {
    uintptr_t joints = 0;
    uint8_t parent = 0xFF;
    if (joint < 0 || !mem::Read(p + 0x318, joints) || !joints || !mem::Read(joints + 0x90 * joint + 5, parent)) return -1;
    return parent == 0xFF ? -1 : parent;
}

// The player whose head is hidden, still that object (user, 2026-11-01: after a Majini's neck break in the first person
// the head stayed invisible): not only while he is player 1 - his slot may change while he dies - but while his
// vtable and his joint array are the ones seen when the head was hidden.
uintptr_t g_headVtable = 0, g_headJoints = 0;

bool HeadPlayerLive() {
    if (!g_headPlayer) return false;
    if (g_headPlayer == actors::Player(0)) return true;
    uintptr_t vtable = 0, joints = 0;
    return mem::Read(g_headPlayer, vtable) && vtable == g_headVtable && mem::Read(g_headPlayer + 0x318, joints) &&
           joints && joints == g_headJoints;
}

void ShowHead() {
    bodycut::Set(0, -1);
    if (g_headPlayer) {
        const bool live = HeadPlayerLive();
        LOG("camera: head shown again%s (player %08X, player 1 %08X)", live ? "" : " - REFUSED, object gone",
            static_cast<unsigned>(g_headPlayer), static_cast<unsigned>(actors::Player(0)));
        if (live) RestoreBody(g_headPlayer);
    }
    g_body.clear();
    g_bodyBuiltMode = -1;
    g_headPlayer = 0;
    g_headJoint = g_neckJoint = g_chestJoint = -1;
}

// The held weapon (player +0x2634: its unit, parent +0x18 = the player, world matrix +0xA0, rows X / Y / Z / place) and its
// sight line, read on the handgun wp0200 (2026-11-04): its muzzle is joint id 101 (local offsets +0x10 summed up its
// parents: (-31, -2.5, 8.8) cm), the barrel along the axis the muzzle lies on (-X there), the model's bounding box
// rModel +0xD0 min / +0xE0 max (x -30.7 .. -3.9, z -6.9 .. 10.6: the slide's top 10.6). The front sight: the muzzle's
// end along the barrel, the muzzle's line across, the weapon's top (+ g_ironHeight) along the axis nearest the world's
// up; barrel: its direction (world); top: its up axis (world); length: the weapon's along the barrel. A weapon without
// joint 101 (grenades, knives) has no sights.
// The back end the eye is measured from: the box's, but never farther back than 4 cm behind the grip (the weapon's
// origin, where the hand holds it) - a stock reaches far behind the sights (user, 2026-11-04: the M93R's, wp1e00, its
// box to +31 cm, then the machine guns and rifles' took the eye far behind; the handguns' backs are at -3.9).
constexpr float kRearLimit = 4.0f;

// The item in player 1's hands (its cItem in use with state 1), 0 if none.
uint16_t HeldItem(uintptr_t p) {
    for (int k = 0; k < 24; ++k) {
        const uintptr_t item = p + 0x21A4 + 0x30 * k;  // cItem k of the item manager at P+0x21A0
        uint8_t inUse = 0;
        uint32_t state = 0, id = 0;
        if (mem::Read(item + 0x29, inUse) && inUse && mem::Read(item + 0x1C, state) && state == 1 && mem::Read(item + 4, id))
            return static_cast<uint16_t>(id);
    }
    return 0;
}

// A handgun or a magnum (inventory.cpp's groups 1 and 4): the ones the aim leans.
bool HandgunItem(uint16_t id) {
    switch (id) {
    case 0x0102: case 0x0110: case 0x0111: case 0x0112: case 0x011E: case 0x011F: case 0x0129:  // handguns
    case 0x010B: case 0x011A: case 0x011B:                                                      // magnums
        return true;
    default:
        return false;
    }
}

// The eye's distance per weapon, added to its kind's (user, 2026-11-04: "the VZ too near now, halfway would be good;
// the AK slightly too near, the camera clips the weapon").
float SightDistanceExtra(uint16_t id) {
    switch (id) {
    case 0x0103: return 10.0f;  // VZ61
    case 0x011D: return 7.5f;   // AK-74 (wp1d)
    case 0x0104: return 6.0f;   // Ithaca M37
    case 0x0116: return 3.0f;   // M3
    case 0x0117: return 5.0f;   // Jail Breaker (the Ithaca's aim, quick_knife.cpp)
    case 0x0105: return 5.0f;                // S75 (sniper rifles: their scope cut)
    case 0x011C: return 15.0f;               // H&K PSG1 (wp1c)
    case 0x0120: return 10.0f;               // Dragunov (wp20)
    case 0x0135: return 20.5f;               // the second rocket launcher (wp35)
    default: return 0.0f;
    }
}

bool SightLine(uintptr_t p, float front[3], float barrel[3], float top[3], float& length, bool* stocked = nullptr,
               uintptr_t* unit = nullptr, float* localUp = nullptr) {
    uintptr_t weapon = 0, parent = 0, model = 0, joints = 0;
    uint32_t count = 0;
    float w[16]{}, lo[3]{}, hi[3]{};
    if (!p || !mem::Read(p + 0x2634, weapon) || !weapon || !mem::Read(weapon + 0x18, parent) || parent != p ||
        !mem::SafeRead(reinterpret_cast<void*>(weapon + 0xA0), w, sizeof(w)) || !mem::Read(weapon + 0xA80, model) || !model ||
        !mem::SafeRead(reinterpret_cast<void*>(model + 0xD0), lo, sizeof(lo)) ||
        !mem::SafeRead(reinterpret_cast<void*>(model + 0xE0), hi, sizeof(hi)) || !mem::Read(weapon + 0x318, joints) || !joints ||
        !mem::Read(weapon + 0x314, count) || !count || count > 64)
        return false;
    int muzzle = -1;
    for (uint32_t j = 0; j < count && muzzle < 0; ++j) {
        uint8_t id = 0;
        if (mem::Read(joints + 0x90 * j + 7, id) && id == 101) muzzle = static_cast<int>(j);
    }
    if (muzzle < 0) return false;
    float m[3]{};
    for (int j = muzzle, depth = 0; j >= 0 && j < static_cast<int>(count) && depth < 16; ++depth) {
        float local[3]{};
        uint8_t up = 0xFF;
        if (!mem::SafeRead(reinterpret_cast<void*>(joints + 0x90 * j + 0x10), local, sizeof(local)) || !mem::Read(joints + 0x90 * j + 5, up))
            return false;
        for (int i = 0; i < 3; ++i) m[i] += local[i];
        j = up < count ? up : -1;
    }
    int along = 0;
    for (int i = 1; i < 3; ++i)
        if (std::fabs(m[i]) > std::fabs(m[along])) along = i;
    if (std::fabs(m[along]) < 1.0f) return false;
    const float sign = m[along] < 0.0f ? -1.0f : 1.0f;
    int upAxis = -1;
    for (int i = 0; i < 3; ++i)
        if (i != along && (upAxis < 0 || std::fabs(w[4 * i + 1]) > std::fabs(w[4 * upAxis + 1]))) upAxis = i;
    const int side = 3 - along - upAxis;
    const float upSign = w[4 * upAxis + 1] >= 0.0f ? 1.0f : -1.0f;
    float local[3];
    local[along] = sign < 0.0f ? lo[along] : hi[along];
    local[side] = m[side];
    local[upAxis] = upSign > 0.0f ? hi[upAxis] + g_ironHeight.load(std::memory_order_relaxed)
                                  : lo[upAxis] - g_ironHeight.load(std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) front[i] = local[0] * w[i] + local[1] * w[4 + i] + local[2] * w[8 + i] + w[12 + i];
    const float scale = std::sqrt(w[4 * along] * w[4 * along] + w[4 * along + 1] * w[4 * along + 1] + w[4 * along + 2] * w[4 * along + 2]);
    if (scale < 1e-6f) return false;
    for (int i = 0; i < 3; ++i) {
        barrel[i] = sign * w[4 * along + i] / scale;
        top[i] = upSign * w[4 * upAxis + i] / scale;
    }
    length = (sign < 0.0f ? std::min(hi[along], -kRearLimit) - lo[along] : hi[along] - std::max(lo[along], kRearLimit)) * scale;
    if (stocked) *stocked = (sign < 0.0f ? hi[along] : -lo[along]) > kRearLimit + 1.0f;
    if (unit) *unit = weapon;
    if (localUp)
        for (int i = 0; i < 3; ++i) localUp[i] = i == upAxis ? upSign : 0.0f;
    return true;
}

// Player 1 aims (aim start 0x1B, aim 0x1C, fire 0x1E; with reloadToo its reload 0x1F).
bool Aims(uintptr_t p, bool reloadToo) {
    uint32_t action = 0;
    if (!p || !mem::Read(p + 0x10E0, action)) return false;
    return action == 0x1B || action == 0x1C || action == 0x1E || (reloadToo && action == 0x1F);
}

// The first person's eye moved behind the sights while aiming, over 0.15 s each way (user, 2026-11-04). The direction is
// never touched: the game aims with the camera it is given back, and every view direction made from the weapon (along
// the barrel, towards the barrel's meeting with the aim ray, the barrel's angles smoothed) swayed, drifted or jerked
// and once turned Chris round when the aim ended. The eye only goes on the sight line (through the front sight, along
// the barrel) behind the weapon: eye, rear and front sights in one line, so they cover each other on the picture where
// the weapon points - the laser's dot - a few degrees from the middle at most (the game converges the weapon on its
// camera's ray). The aim's animation leans the weapon aside on the picture (user, 2026-11-04: "his arm leans the weapon
// over"): the camera rolls about its direction by g_ironRollDegrees (faded in with the sights) - the up vector only
// (ctrl +0x180); true when it was changed.

bool ApplyIronSight(uintptr_t p, float at[3], const float dir[3], float upward[3]) {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    const float dt = last ? static_cast<float>(std::min<ULONGLONG>(now - last, 100)) / 1000.0f : 0.0f;
    last = now;
    const bool wanted = g_ironFirst.load(std::memory_order_relaxed) &&
                        (g_firstPersonOption.load(std::memory_order_relaxed) || g_ironThirdNow.load(std::memory_order_relaxed));
    float front[3], barrel[3], top[3], weaponLength = 0.0f;  // top: the weapon's up axis, unused (see the roll)
    bool stocked = false;
    const bool sights = SightLine(p, front, barrel, top, weaponLength, &stocked);
    const bool aiming = wanted && sights && Aims(p, false);
    const float before = g_ironWeight.load(std::memory_order_relaxed);
    const float weight = sights ? std::clamp(before + (aiming ? dt : -dt) / 0.15f, 0.0f, 1.0f) : 0.0f;
    g_ironWeight.store(weight, std::memory_order_relaxed);
    if (weight <= 0.0f) return false;
    const float k = weight * weight * (3.0f - 2.0f * weight);
    // Every weapon but the handguns and magnums (the M93R's stock included): the eye nearer (user, 2026-11-04: "with
    // the machine guns and rifles we see well behind the shoulders", then the VZ61 too).
    const uint16_t item = HeldItem(p);
    const bool handgun = HandgunItem(item);
    // No sights for the Gatling (0x0115) and the bow (0x010F) (user, 2026-11-04): the plain first person.
    if (item == 0x0115 || item == 0x010F) return false;
    const float back = weaponLength + (handgun ? g_ironDistance : g_ironDistanceStock).load(std::memory_order_relaxed) +
                       SightDistanceExtra(item);
    for (int i = 0; i < 3; ++i) at[i] += (front[i] - barrel[i] * back - at[i]) * k;
    // The roll: the camera's up turned about the view direction by the user's angle (the weapon's own up axis leans only
    // a degree or two at rest and jumps to 26 in the aim's animation: the lean seen is the arm's and the perspective's).
    const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float dl = std::sqrt(dot(dir, dir));
    // Handguns and magnums without a stock only (user, 2026-11-04: "the rifles do not lean", nor the M93R with its stock).
    const float roll = handgun && !stocked ? g_ironRollDegrees.load(std::memory_order_relaxed) * 3.14159265f / 180.0f * k : 0.0f;
    if (dl < 1e-6f || std::fabs(roll) < 1e-5f) return false;
    float d[3], u0[3];
    for (int i = 0; i < 3; ++i) d[i] = dir[i] / dl;
    const float ud = dot(upward, d);
    for (int i = 0; i < 3; ++i) u0[i] = upward[i] - d[i] * ud;
    const float ul = std::sqrt(dot(u0, u0));
    if (ul < 1e-4f) return false;
    for (int i = 0; i < 3; ++i) u0[i] /= ul;
    const float side[3] = { d[1] * u0[2] - d[2] * u0[1], d[2] * u0[0] - d[0] * u0[2], d[0] * u0[1] - d[1] * u0[0] };
    const float c = std::cos(roll), sn = std::sin(roll);
    for (int i = 0; i < 3; ++i) upward[i] = (u0[i] * c + side[i] * sn) * ul;
    return true;
}

// The first person's look up and down (user, 2026-11-04: "the first person can hardly look up or down, aiming it can").
// The normal camera (uCameraQFPS, ctrl +0xE00 = [ctrl +0xDF0]) has no pitch angle: its height +0x1D0, -1 (down) .. 1
// (up), clamped at VA 0x845A2C / 0x845C66, blends three camera settings (VA 0x846BE9..: +0x450 down, +0x420 middle,
// +0x3F0 up; the aim's when its mode byte +0x1B1 is 1, the height then -P+0x2908): about -30 .. +28 degrees (snapshots,
// research/snaps/pitch_up / pitch_down). The mouse adds its move to the height (VA 0x845C3A, when +0x634 is set), a
// pad's right stick sets it (VA 0x845C56). Its update runs before the view copy. In the first person out of the aim
// the trainer keeps its own pitch: each update the height's move since the last one (it is put back to 0, the middle,
// so it never meets its clamp) turns the look by kPitchPerHeight degrees (the game's own ratio); with a pad the height
// (the stick) turns it at kPadPitchSpeed degrees a second at full tilt; the look between +-kMaxPitch. In the aim the
// game's own look; its pitch is carried over when the aim ends, and the height given back when the first person ends.
constexpr float kPitchPerHeight = 29.0f, kPadPitchSpeed = 120.0f, kMaxPitch = 80.0f;
float g_fpPitch = 0.0f;       // game thread: the first person's pitch, degrees (up > 0)
bool g_fpPitchSet = false;    // g_fpPitch holds the look
bool g_fpHeightFresh = true;  // the height holds the game's own value, not a move
ULONGLONG g_fpPitchTick = 0;

void FirstPersonPitch(uintptr_t ctrl, float dir[3], float upward[3]) {
    const uintptr_t cam = ctrl + 0xE00;
    uint8_t aim = 0, mouse = 0;
    float height = 0.0f;
    if (!mem::Read(cam + 0x1B1, aim) || !mem::Read(cam + 0x634, mouse) || !mem::Read(cam + 0x1D0, height)) return;
    const float flat = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
    const float length = std::sqrt(flat * flat + dir[1] * dir[1]);
    if (flat < 1e-4f || length < 1e-4f) return;
    constexpr float kDeg = 57.2957795f;
    const float gamePitch = std::atan2(dir[1], flat) * kDeg;
    const ULONGLONG now = GetTickCount64();
    if (!g_fpPitchSet || now - g_fpPitchTick > 250) {
        g_fpPitch = gamePitch;
        g_fpHeightFresh = true;
        g_fpPitchSet = true;
    }
    g_fpPitchTick = now;
    if (aim == 1) {  // the aim's own look (it goes far enough), carried over when it ends
        g_fpPitch = gamePitch;
        g_fpHeightFresh = true;
        return;
    }
    if (!g_fpHeightFresh) {
        if (mouse) {
            g_fpPitch += height * kPitchPerHeight;
        } else {
            uintptr_t clock = 0;
            float dt = 0.0f;
            if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB209C)), clock) && clock) mem::Read(clock + 0x24, dt);
            g_fpPitch += height * kPadPitchSpeed * std::clamp(dt, 0.0f, 0.1f);
        }
    }
    g_fpHeightFresh = false;
    g_fpPitch = std::clamp(g_fpPitch, -kMaxPitch, kMaxPitch);
    mem::Write(cam + 0x1D0, 0.0f);
    const float p = g_fpPitch / kDeg, c = std::cos(p), s = std::sin(p);
    const float hx = dir[0] / flat, hz = dir[2] / flat;
    dir[0] = hx * c * length;
    dir[1] = s * length;
    dir[2] = hz * c * length;
    upward[0] = -s * hx;
    upward[1] = c;
    upward[2] = -s * hz;
    mem::SafeWrite(reinterpret_cast<void*>(ctrl + 0x180), upward, sizeof(float) * 3);
}

// The first person ended: the normal camera's height from its pitch (the third person looks where it looked).
void EndFirstPersonPitch(uintptr_t ctrl) {
    if (!g_fpPitchSet) return;
    g_fpPitchSet = false;
    uint8_t aim = 0;
    if (mem::Read(ctrl + 0xE00 + 0x1B1, aim) && aim != 1)
        mem::Write(ctrl + 0xE00 + 0x1D0, std::clamp(g_fpPitch / kPitchPerHeight, -1.0f, 1.0f));
}

void AdjustCameraCopy(uintptr_t ctrl) {
    uintptr_t active = 0;
    if (!mem::Read(ctrl + 0x1B0, active) || !active) return;
    const bool normal = active == ctrl + 0xE00, gameplay = normal || active == ctrl + 0x2DE0;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    float fov = 0.0f;
    mem::Read(ctrl + 0x1A0, fov);
    // The view copy runs for more than one camera controller a frame (recorded 2026-11-04, research/fp_turns/fp_trace_walk.txt:
    // three looks a frame, yaw -89 / 10 / -171 - only the first player 1's): the first person is the picture's controller's
    // only ([sCamera +0x34], CameraCtrl), the others' views left alone (their blends mixed in its state: the view shook).
    if (ctrl != CameraCtrl()) {
        static ULONGLONG told = 0;
        if (g_firstPerson.load(std::memory_order_relaxed) && GetTickCount64() - told > 5000) {
            told = GetTickCount64();
            LOG("camera: view copy of another controller %08X (the picture's is %08X): no first person there",
                static_cast<unsigned>(ctrl), static_cast<unsigned>(CameraCtrl()));
        }
        return;
    }
    const bool first = g_firstPerson.load(std::memory_order_relaxed) && normal && p && FirstPersonOnFoot(p) && !FirstPersonPaused(p);
    // Research (user, 2026-11-01: the head invisible after a neck break): each change of the first person logged.
    static int lastFirst = -1;
    if (g_firstPerson.load(std::memory_order_relaxed) && static_cast<int>(first) != lastFirst) {
        lastFirst = first;
        uint32_t action = 0;
        if (p) mem::Read(p + 0x10E0, action);
        LOG("camera: first person %s (active ctrl+%X, player 1 %08X action %X, on foot %d, paused %d, head player %08X)",
            first ? "made" : "not made", static_cast<unsigned>(active - ctrl), static_cast<unsigned>(p), action,
            p ? FirstPersonOnFoot(p) : 0, p ? FirstPersonPaused(p) : 0, static_cast<unsigned>(g_headPlayer));
    }
    if (first) {
        if (g_headPlayer != p) {
            ShowHead();
            g_headPlayer = p;
            mem::Read(p, g_headVtable);
            mem::Read(p + 0x318, g_headJoints);
            g_headJoint = HeadJoint(p);
            g_neckJoint = ParentJoint(p, g_headJoint);
            g_chestJoint = ParentJoint(p, g_neckJoint);
        }
        uintptr_t joints = 0;
        float m[16]{}, neck[16]{}, chest[16]{}, headLocal[3]{}, eye[3]{}, target[3]{}, upward[3]{}, q[4]{};
        if (g_headJoint >= 0 && g_neckJoint >= 0 && g_chestJoint >= 0 && mem::Read(p + 0x318, joints) && joints &&
            mem::SafeRead(reinterpret_cast<void*>(joints + 0x90 * g_headJoint + 0x50), m, sizeof(m)) &&
            mem::SafeRead(reinterpret_cast<void*>(joints + 0x90 * g_headJoint + 0x10), headLocal, sizeof(headLocal)) &&
            mem::SafeRead(reinterpret_cast<void*>(joints + 0x90 * g_neckJoint + 0x50), neck, sizeof(neck)) &&
            mem::SafeRead(reinterpret_cast<void*>(joints + 0x90 * g_chestJoint + 0x50), chest, sizeof(chest)) &&
            mem::SafeRead(reinterpret_cast<void*>(ctrl + 0x170), eye, sizeof(eye)) &&
            mem::SafeRead(reinterpret_cast<void*>(ctrl + 0x180), upward, sizeof(upward)) &&
            mem::SafeRead(reinterpret_cast<void*>(ctrl + 0x190), target, sizeof(target)) &&
            mem::SafeRead(reinterpret_cast<void*>(p + 0x40), q, sizeof(q))) {
            // The body cut by its triangles when the device hooks are in (body_cut.cpp: no joint shrunk, nothing
            // stretched between a hidden part and a kept one - user, 2026-11-03), else its joints shrunk.
            if (bodycut::Ready()) {
                if (g_bodyBuiltMode != -1) RestoreBody(p);
                bodycut::Set(p, g_bodyMode.load(std::memory_order_relaxed));
            } else {
                bodycut::Set(p, -1);  // no cut before the device hooks are in
                if (const int mode = g_bodyMode.load(std::memory_order_relaxed); mode != g_bodyBuiltMode) {
                    RestoreBody(p);
                    BuildBody(p, mode);
                }
                ApplyBody(p);
            }
            g_firstPersonApplied = true;
            g_firstPersonTick = GetTickCount64();
            std::memcpy(g_firstPersonHeadAt, m + 12, sizeof(g_firstPersonHeadAt));
            SetFirstPersonNear(ctrl);
            // The eye from the head's place along the chest's frame (joint id 2, the neck's parent; user, 2026-11-01:
            // "running, his body goes ahead and the neck shows", "up a slope the camera goes right into his back":
            // along the world's up and his facing the offsets stayed put while the body leans; along the head's own
            // frame, "things pass in front of the camera, only on slopes": recorded, going down his head turned and
            // bent up to 75-84 degrees and took the eye up to 25 cm aside, the chest's front joints ids 102 / 103
            // 5-10 cm behind the lens). World rows +0x50 left, +0x60 up, +0x70 forward (normalized: the shrunk joints'
            // are scaled), +0x80 the place. (A probe found the joints and his position the same at this copy as at
            // the frame's end: no lag to make up.)
            const float yaw = 2.0f * std::atan2(q[1], q[3]);
            const float facing[3][3] = { { std::cos(yaw), 0.0f, -std::sin(yaw) }, { 0.0f, 1.0f, 0.0f },
                                         { std::sin(yaw), 0.0f, std::cos(yaw) } };
            float neckAxes[3][3], chestAxes[3][3], headAxes[3][3];
            const auto normalize = [&](const float* matrix, float out[3][3]) {
                for (int r = 0; r < 3; ++r) {
                    const float* row = matrix + 4 * r;
                    const float len = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
                    for (int i = 0; i < 3; ++i) out[r][i] = len > 1e-12f ? row[i] / len : facing[r][i];
                }
            };
            normalize(neck, neckAxes);
            normalize(chest, chestAxes);
            normalize(m, headAxes);
            // The head shrunk (the joints' way of hiding it, without the body cut): the chest's turn instead.
            const bool headScaled = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) > 0.5f;
            float headAt[3];
            for (int i = 0; i < 3; ++i)
                headAt[i] = neck[12 + i] + headLocal[0] * neckAxes[0][i] + headLocal[1] * neckAxes[1][i] + headLocal[2] * neckAxes[2][i];
            const float up = g_eyeOffset[0].load(std::memory_order_relaxed), right = g_eyeOffset[1].load(std::memory_order_relaxed),
                        ahead = g_eyeOffset[2].load(std::memory_order_relaxed);
            float at[3];
            for (int i = 0; i < 3; ++i)
                at[i] = headAt[i] + up * chestAxes[1][i] + ahead * chestAxes[2][i] - right * chestAxes[0][i];
            float dir[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
            FirstPersonPitch(ctrl, dir, upward);
            const Quat turn = BodyTurn(p, headScaled ? headAxes : chestAxes, yaw, dir);
            g_firstPersonTurned = turn.w < 0.99999f;
            if (turn.w < 0.99999f) {
                Rotate(turn, dir);
                Rotate(turn, upward);
                if (g_levelUp > 0.0f) {  // the up levelled towards the world's (no upside-down picture)
                    const float dn = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                    if (dn > 1e-6f) {
                        const float d[3] = { dir[0] / dn, dir[1] / dn, dir[2] / dn };
                        float level[3] = { -d[1] * d[0], 1.0f - d[1] * d[1], -d[1] * d[2] };  // world up minus its part along d
                        const float ln = std::sqrt(level[0] * level[0] + level[1] * level[1] + level[2] * level[2]);
                        if (ln > 1e-6f) {
                            float mixed[3];
                            for (int i = 0; i < 3; ++i) mixed[i] = upward[i] + (level[i] / ln - upward[i]) * g_levelUp;
                            const float mn = std::sqrt(mixed[0] * mixed[0] + mixed[1] * mixed[1] + mixed[2] * mixed[2]);
                            if (mn > 1e-6f)
                                for (int i = 0; i < 3; ++i) upward[i] = mixed[i] / mn;
                        }
                    }
                }
                mem::SafeWrite(reinterpret_cast<void*>(ctrl + 0x180), upward, sizeof(upward));
            }
            if (ApplyIronSight(p, at, dir, upward)) mem::SafeWrite(reinterpret_cast<void*>(ctrl + 0x180), upward, sizeof(upward));
            SteadyEye(p, at);  // after the sights: the weapon's own bob in the hands while walking (user, 2026-11-04)
            ResearchFirstPersonTrace(p, at, headAt, dir, yaw);
            const float look[3] = { at[0] + dir[0], at[1] + dir[1], at[2] + dir[2] };
            mem::SafeWrite(reinterpret_cast<void*>(ctrl + 0x170), at, sizeof(at));
            g_firstPersonFinalCtrl = ctrl;
            NoteUpdate(p, at);
            mem::SafeWrite(reinterpret_cast<void*>(ctrl + 0x190), look, sizeof(look));
            if (fov > 1.0f) mem::Write(ctrl + 0x1A0, g_firstPersonFov.load(std::memory_order_relaxed) * fov / kGameFov);
        }
        return;
    }
    // Lost in Nightmares' rooms make the view twice a frame, first with the animation camera (uCameraAnimation, ctrl
    // +0x1480), then with the normal one, which is shown (logged 2026-11-03): the head and body were given back and
    // hidden again every frame - Chris "flickered as if he wanted to be see-through" (user). A pass on another camera
    // within 100 ms of a first-person one leaves it all as it is; a real change (an event's camera) gives it back after.
    if (g_firstPersonApplied.load(std::memory_order_relaxed) && GetTickCount64() - g_firstPersonTick < 100) return;
    if (normal) EndFirstPersonPitch(ctrl);
    g_firstPersonApplied = false;
    g_neutralSet = false;
    g_turnWeight = 0.0f;
    if (g_nearCtrl) RestoreNear(ctrl);
    if (g_headPlayer) ShowHead();
    if (g_fovOn.load(std::memory_order_relaxed) && gameplay && fov > 1.0f)
        mem::Write(ctrl + 0x1A0, fov * g_fov.load(std::memory_order_relaxed) / kGameFov);
}

// The first person without the camera's lag (user, 2026-11-04: "jumping down you really see Chris fall without the
// camera, then it catches up"; measured: the picture's camera 15-35 cm from the eye written while running, up to 100
// cm): after the view copy the controller (VA 0x841CE0) runs its blender at ctrl +0x380 (vtable +0x40, VA 0x83DEF0):
// k = pow([blender +0x1B4], dt) (+0x1B0), then eye = k * eye + (1 - k) * the view (+0x30 <- +0x170), the target and the
// FOV alike, the up through an orientation blend - its result, through the object at ctrl +0x610, is the picture's
// (copied at VA 0x84287D..: eye +0x30, up +0x40, target +0x50, FOV +0x24). Two tries wrote the first person's view over
// it (after the copy at VA 0x8428CB: a ghost of Chris in front of him; after the blender's update at VA 0x8427E8: the
// view shook, "Chris trembles", user; then its base made 0 - k = 0, VA 0x8427D0 after the controller writes it every
// update at VA 0x8427C8 -: no tremble at rest, but walking while aiming the view shook (the game's aim view sways and
// rolls with the walk, which the easing hid) and the jumps still lagged). So the easing stays, and at VA 0x8427E8, right
// after the blender's update, only its eye (ctrl +0x3B0) becomes the first person's (ctrl +0x170, written by the view
// copy's cave this update), its target (+0x3D0) moved by as much: no lag of the place, the eased look and up kept.
void __cdecl BlenderHook(uintptr_t ctrl) {
    if (!ctrl || ctrl != g_firstPersonFinalCtrl) return;
    float ours[3], eased[3], goal[3];
    if (!mem::SafeRead(reinterpret_cast<void*>(ctrl + 0x170), ours, sizeof(ours)) ||
        !mem::SafeRead(reinterpret_cast<void*>(ctrl + 0x3B0), eased, sizeof(eased)) ||
        !mem::SafeRead(reinterpret_cast<void*>(ctrl + 0x3D0), goal, sizeof(goal)))
        return;
    for (int i = 0; i < 3; ++i) goal[i] += ours[i] - eased[i];
    mem::SafeWrite(reinterpret_cast<void*>(ctrl + 0x3B0), ours, sizeof(ours));
    mem::SafeWrite(reinterpret_cast<void*>(ctrl + 0x3D0), goal, sizeof(goal));
    // The body's turns (user, 2026-11-10: "from where it blocked before, the camera turns 180 degrees on the other axis and comes back when
    // his feet land"; pictures: the view's direction followed the head, the picture stayed upright; then "remove it completely in the first
    // person: even a jump down from a platform makes it when Chris looks too far down"): the blender's orientation blend keeps the up
    // upright (it writes it at VA 0x84024E), so it turned the picture over as the look passed the vertical. Whenever the first person
    // turned the view with the body this update, the up is the first person's (ctrl +0x180, turned with the head); else the eased one
    // (the aim's walk sway stays hidden).
    float up[3];
    if (g_firstPersonTurned && mem::SafeRead(reinterpret_cast<void*>(ctrl + 0x180), up, sizeof(up)))
        mem::SafeWrite(reinterpret_cast<void*>(ctrl + 0x3C0), up, sizeof(up));
}

void __cdecl CameraCopyHook(uintptr_t ctrl) {
    // Only the picture's controller's view copy starts its update afresh (see g_firstPersonFinalCtrl).
    if (ctrl == CameraCtrl()) g_firstPersonFinalCtrl = 0;  // set again by AdjustCameraCopy for a first person view
    __try {
        AdjustCameraCopy(ctrl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

using CameraFadeFn = void(__fastcall*)(void* self, void* edx);
using SetCameraFadeFn = void(__fastcall*)(void* self, void* edx, int viewport, float value);
CameraFadeFn g_cameraFade = nullptr;
SetCameraFadeFn g_setCameraFade = nullptr;

// Every unit that fades, in first person: player 1, the items he carries (user, 2026-11-01: "the weapon is still
// see-through": the item objects have the same fade, W+0x1724 0.4 / W+0x1734 0.25 read), and the enemies, the
// partner and their weapons (user: "in the synchronized actions the enemy, very close, is often invisible").
bool FirstPersonOpaque() {
    return g_firstPersonApplied.load(std::memory_order_relaxed) && g_firstPerson.load(std::memory_order_relaxed);
}

void __fastcall HookSetCameraFade(void* self, void* edx, int viewport, float value) {
    g_setCameraFade(self, edx, viewport, FirstPersonOpaque() ? 1.0f : value);
}

void __fastcall HookCameraFade(void* self, void* edx) {
    g_cameraFade(self, edx);
    if (!FirstPersonOpaque() || !g_setCameraFade) return;
    __try {
        for (int viewport = 0; viewport < 2; ++viewport) g_setCameraFade(self, nullptr, viewport, 1.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool HookCode(uintptr_t rva, const uint8_t* expected, size_t n, void* detour, void** original, const char* what) {
    uint8_t* const target = mem::Rva(rva);
    if (!mem::IsGameCode(target, n) || std::memcmp(target, expected, n) != 0) {
        LOG("%s hook refused: unexpected bytes %s", what, mem::Hex(target, n).c_str());
        return false;
    }
    const MH_STATUS created = MH_CreateHook(target, detour, original);
    if ((created != MH_OK && created != MH_ERROR_ALREADY_CREATED) || MH_EnableHook(target) != MH_OK) {
        LOG("%s hook: %s", what, MH_StatusToString(created));
        return false;
    }
    LOG("%s hook installed at %p", what, target);
    return true;
}

bool EnsureCameraFadeHook() {
    if (g_cameraFade && g_setCameraFade) return true;
    static constexpr uint8_t kFade[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x44, 0x53, 0x56, 0x8B, 0xF1 };
    static constexpr uint8_t kSet[] = { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x85, 0xF6, 0x7C, 0x3D };
    void* original = nullptr;
    if (!g_setCameraFade) {
        if (!HookCode(0x7576C0, kSet, sizeof(kSet), reinterpret_cast<void*>(&HookSetCameraFade), &original, "camera fade setter"))
            return false;
        g_setCameraFade = reinterpret_cast<SetCameraFadeFn>(original);
    }
    if (!g_cameraFade) {
        if (!HookCode(0x757710, kFade, sizeof(kFade), reinterpret_cast<void*>(&HookCameraFade), &original, "camera fade"))
            return false;
        g_cameraFade = reinterpret_cast<CameraFadeFn>(original);
    }
    return true;
}

// The cave: the registers, flags and xmm0-7 kept round a call of `callee(esi)` (esi is the controller),
// then the instruction replaced (position independent) and back.
mem::Patch* g_cameraSite = nullptr;
mem::Patch* g_blenderSite = nullptr;
int g_cameraUsers = 0;

std::vector<uint8_t> EmitCallCave(uint8_t* cave, uint8_t* site, const std::vector<uint8_t>& original, void (__cdecl* callee)(uintptr_t)) {
    std::vector<uint8_t> c = { 0x60, 0x9C, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00 };  // pushad; pushfd; sub esp, 0x80 (imm32)
    for (uint8_t r = 0; r < 8; ++r) c.insert(c.end(), { 0xF3, 0x0F, 0x7F, static_cast<uint8_t>(0x44 | (r << 3)), 0x24, static_cast<uint8_t>(r * 0x10) });
    c.push_back(0x56);  // push esi (the controller)
    c.push_back(0xE8);  // call callee
    const int32_t call = static_cast<int32_t>(reinterpret_cast<uintptr_t>(callee) - (reinterpret_cast<uintptr_t>(cave) + c.size() + 4));
    c.insert(c.end(), reinterpret_cast<const uint8_t*>(&call), reinterpret_cast<const uint8_t*>(&call) + 4);
    c.insert(c.end(), { 0x83, 0xC4, 0x04 });  // add esp, 4
    for (uint8_t r = 0; r < 8; ++r) c.insert(c.end(), { 0xF3, 0x0F, 0x6F, static_cast<uint8_t>(0x44 | (r << 3)), 0x24, static_cast<uint8_t>(r * 0x10) });
    c.insert(c.end(), { 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00, 0x9D, 0x61 });  // add esp, 0x80 (imm32); popfd; popad
    c.insert(c.end(), original.begin(), original.end());
    c.push_back(0xE9);  // jmp back
    const int32_t back = static_cast<int32_t>(reinterpret_cast<uintptr_t>(site) + original.size() - (reinterpret_cast<uintptr_t>(cave) + c.size() + 4));
    c.insert(c.end(), reinterpret_cast<const uint8_t*>(&back), reinterpret_cast<const uint8_t*>(&back) + 4);
    std::memcpy(cave, c.data(), c.size());
    const int32_t jump = static_cast<int32_t>(reinterpret_cast<uintptr_t>(cave) - (reinterpret_cast<uintptr_t>(site) + 5));
    std::vector<uint8_t> patched = { 0xE9 };
    patched.insert(patched.end(), reinterpret_cast<const uint8_t*>(&jump), reinterpret_cast<const uint8_t*>(&jump) + 4);
    patched.resize(original.size(), 0x90);
    return patched;
}

// The stage's parts missing with a wide field of view (user, 2026-11-03: in the first person and in the third person,
// whole areas in the void): every render context's depth scale [ctx +0x1F0] = 1 / ([sRender +0x28] * projection
// [1][1]) = 2 tan(fov / 2) (function VA 0x5EF5A0, written at VA 0x5F1818), and the models multiply their view depth by
// it to pick their level of detail and their draw distance (VA 0x598316, 0x58D7DD...): a 100 degree field of view made
// everything look 2.6 times farther, the parts without a far level vanished. The cave at VA 0x5F1801 (mulss xmm1,
// [edx+0x14]) uses the cotangent of kGameFov at least for a perspective projection ([edx +0x2C] = -1; the passes with
// the identity matrix keep theirs): a wide field of view keeps the game's own levels of detail, a zoom keeps its own.
mem::Patch* g_lodSite = nullptr;

bool EnsureLodCave() {
    if (g_lodSite) return true;
    // Never freed: the game may still be inside it when the trainer unloads (it needs nothing from the DLL).
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x40, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) return false;
    uint8_t* const site = mem::Rva(0x1F1801);
    const std::vector<uint8_t> original = { 0xF3, 0x0F, 0x59, 0x4A, 0x14 };  // mulss xmm1, [edx+0x14]
    const float cap = 1.0f / std::tan(kGameFov * 0.5f * 3.14159265f / 180.0f);
    std::memcpy(cave + 0x38, &cap, sizeof(cap));
    const uint32_t capAt = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(cave + 0x38));
    std::vector<uint8_t> c = {
        0x9C,                          // pushfd
        0x83, 0x7A, 0x2C, 0x00,        // cmp dword ptr [edx+0x2C], 0   (perspective: -1)
        0x74, 0x25,                    // je plain
        0x83, 0xEC, 0x10,              // sub esp, 0x10
        0x0F, 0x11, 0x14, 0x24,        // movups [esp], xmm2
        0xF3, 0x0F, 0x10, 0x52, 0x14,  // movss xmm2, [edx+0x14]
        0xF3, 0x0F, 0x5F, 0x15,        // maxss xmm2, [cap]
    };
    c.insert(c.end(), reinterpret_cast<const uint8_t*>(&capAt), reinterpret_cast<const uint8_t*>(&capAt) + 4);
    c.insert(c.end(), {
        0xF3, 0x0F, 0x59, 0xCA,        // mulss xmm1, xmm2
        0x0F, 0x10, 0x14, 0x24,        // movups xmm2, [esp]
        0x83, 0xC4, 0x10,              // add esp, 0x10
        0x9D,                          // popfd
    });
    auto jumpBack = [&] {
        c.push_back(0xE9);
        const int32_t back = static_cast<int32_t>(reinterpret_cast<uintptr_t>(site) + original.size() -
                                                  (reinterpret_cast<uintptr_t>(cave) + c.size() + 4));
        c.insert(c.end(), reinterpret_cast<const uint8_t*>(&back), reinterpret_cast<const uint8_t*>(&back) + 4);
    };
    jumpBack();
    if (c.size() != 0x2C) return false;  // the je above lands here
    c.insert(c.end(), original.begin(), original.end());  // plain: the game's own multiply
    c.push_back(0x9D);                                     // popfd
    jumpBack();
    std::memcpy(cave, c.data(), c.size());
    FlushInstructionCache(GetCurrentProcess(), cave, 0x40);
    const int32_t jump = static_cast<int32_t>(reinterpret_cast<uintptr_t>(cave) - (reinterpret_cast<uintptr_t>(site) + 5));
    std::vector<uint8_t> patched = { 0xE9 };
    patched.insert(patched.end(), reinterpret_cast<const uint8_t*>(&jump), reinterpret_cast<const uint8_t*>(&jump) + 4);
    static mem::Patch lodSite("camera.fov_lod", mem::Rva(0x1F1801), original, patched);
    g_lodSite = &lodSite;
    LOG("camera: level of detail cave at %p (cotangent at least %.4f)", cave, cap);
    return true;
}

bool EnsureCameraCave() {
    if (g_cameraSite) return true;
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) return false;
    const std::vector<uint8_t> copy = { 0x8A, 0x88, 0x62, 0x01, 0x00, 0x00 };  // mov cl, [eax+0x162]
    static mem::Patch copySite("camera.view_copy", mem::Rva(0x4421C2), copy,
                               EmitCallCave(cave, mem::Rva(0x4421C2), copy, &CameraCopyHook));
    FlushInstructionCache(GetCurrentProcess(), cave, 0x100);
    g_cameraSite = &copySite;
    LOG("camera: view copy cave at %p", cave);
    // The blender's result (see BlenderHook): fld dword ptr [esi+0x3B0] - esi the controller.
    auto* blendCave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (blendCave) {
        const std::vector<uint8_t> clear = { 0xD9, 0x86, 0xB0, 0x03, 0x00, 0x00 };
        static mem::Patch blendSite("camera.blender_eye", mem::Rva(0x4427E8), clear,
                                    EmitCallCave(blendCave, mem::Rva(0x4427E8), clear, &BlenderHook));
        FlushInstructionCache(GetCurrentProcess(), blendCave, 0x100);
        g_blenderSite = &blendSite;
        LOG("camera: blender eye cave at %p", blendCave);
    }
    return true;
}

}  // namespace

bool FirstPersonOn() { return g_firstPerson.load(std::memory_order_relaxed); }

// The cave is not reached by every camera (user, 2026-11-01: a Majini's neck break execution went to the third person
// with Chris's head still invisible): once the first person has not been made for kStaleMs while his head moves (not
// in the pause, whose frozen picture stays the first person's), the head and the neck are given back, and the near.
void ReleaseStaleFirstPerson() {
    constexpr ULONGLONG kStaleMs = 150;
    if (!g_headPlayer || GetTickCount64() - g_firstPersonTick < kStaleMs) return;
    uintptr_t joints = 0;
    float head[3]{};
    if (HeadPlayerLive() && g_headJoint >= 0 && mem::Read(g_headPlayer + 0x318, joints) && joints &&
        mem::SafeRead(reinterpret_cast<void*>(joints + 0x90 * g_headJoint + 0x80), head, sizeof(head))) {
        const float dx = head[0] - g_firstPersonHeadAt[0], dy = head[1] - g_firstPersonHeadAt[1],
                    dz = head[2] - g_firstPersonHeadAt[2];
        if (dx * dx + dy * dy + dz * dz < 0.25f) return;  // still where the first person left it
    }
    if (g_firstPersonApplied.load(std::memory_order_relaxed))
        LOG("camera: first person not made for %llu ms: player 1's head shown again", GetTickCount64() - g_firstPersonTick);
    g_firstPersonApplied = false;
    g_neutralSet = false;
    g_turnWeight = 0.0f;
    ShowHead();
    if (g_nearCtrl) RestoreNear(CameraCtrl());
}

// Research (2026-11-04, "the hands tremble walking, only in the first person"): with an empty DataDir\\fp_burst, the
// next 40 pictures are saved at half size (shot_burst_NN.bmp) once player 1 has walked while aiming (action 0x1C, root
// moving) for 30 pictures, to see the hands from one picture to the next.
// The picture's view-projection (sCamera +0x3B0, clip = M x (p, 1) by rows) and its eye (x = y = w = 0).
bool PictureCamera(float m[16], float eye[3]) {
    uintptr_t camera = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE3457C)), camera) || !camera ||
        !mem::SafeRead(reinterpret_cast<void*>(camera + 0x3B0), m, sizeof(float) * 16))
        return false;
    const float a[3][3] = { { m[0], m[1], m[2] }, { m[4], m[5], m[6] }, { m[12], m[13], m[14] } };
    const float b[3] = { -m[3], -m[7], -m[15] };
    const auto det3 = [](const float t[3][3]) {
        return t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
               t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0]);
    };
    const float det = det3(a);
    if (std::fabs(det) < 1e-12f) return false;
    for (int k = 0; k < 3; ++k) {
        float t[3][3];
        for (int r = 0; r < 3; ++r)
            for (int q = 0; q < 3; ++q) t[r][q] = q == k ? b[r] : a[r][q];
        eye[k] = det3(t) / det;
    }
    return true;
}

// A point's place in the picture, pixels of the half-size burst pictures (1280 x 800 for 2560 x 1600).
void ScreenOf(const float m[16], const float p[3], float& x, float& y) {
    const float cx = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    const float cy = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    const float cw = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
    x = cw != 0.0f ? (cx / cw * 0.5f + 0.5f) * 1280.0f : -1.0f;
    y = cw != 0.0f ? (0.5f - cy / cw * 0.5f) * 800.0f : -1.0f;
}

char g_burstText[40][256];
char g_burstMatrices[40][900];

void NoteBurstMatrices(int index) {
    uintptr_t camera = 0, render = 0;
    uint32_t buffer = 99;
    float m[32]{};
    g_burstMatrices[index][0] = 0;
    if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE345D4)), render) && render) mem::Read(render + 0x158BC, buffer);
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE3457C)), camera) || !camera ||
        !mem::SafeRead(reinterpret_cast<void*>(camera + 0x3B0), m, sizeof(m)))
        return;
    int at = sprintf_s(g_burstMatrices[index], "P %02d latest %u buffer %u M", index, g_updateNumber, buffer);
    for (int i = 0; i < 32 && at > 0 && at < 880; ++i)
        at += sprintf_s(g_burstMatrices[index] + at, sizeof(g_burstMatrices[index]) - at, " %.6g", m[i]);
}

void NoteBurstPicture(int index) {
    float m[16]{}, c[3]{}, live[3]{};
    if (index < 0 || index >= 40) return;
    g_burstText[index][0] = 0;
    NoteBurstMatrices(index);
    if (!PictureCamera(m, c)) return;
    // The update whose eye the picture's camera is.
    int best = -1;
    float bestGap = 1e9f;
    for (int i = 0; i < 16; ++i) {
        const UpdateNote& note = g_updateNotes[i];
        if (!note.number) continue;
        const float d = std::sqrt((note.eye[0] - c[0]) * (note.eye[0] - c[0]) + (note.eye[1] - c[1]) * (note.eye[1] - c[1]) +
                                  (note.eye[2] - c[2]) * (note.eye[2] - c[2]));
        if (d < bestGap) bestGap = d, best = i;
    }
    float lx = -1, ly = -1, mx = -1, my = -1;
    if (RightHand(g_headPlayer, live)) ScreenOf(m, live, lx, ly);
    if (best >= 0) ScreenOf(m, g_updateNotes[best].hand, mx, my);
    sprintf_s(g_burstText[index], "%02d latest %u camera of %u (%.2f cm) hand live %.1f %.1f, of the camera's update %.1f %.1f",
              index, g_updateNumber, best >= 0 ? g_updateNotes[best].number : 0u, bestGap, lx, ly, mx, my);
}

void ResearchBurst() {
    static int left = 0, index = 0, walking = 0;
    static bool armed = false;
    static float last[3]{};
    static ULONGLONG checked = 0;
    if (left > 0) {
        if (index < 40) NoteBurstPicture(index);
        ++index;
        if (--left == 0) {
            LOG("burst research: 40 pictures saved");
            g_burstUpdates = false;
            g_burstOn = false;
            FILE* f = nullptr;
            if (_wfopen_s(&f, (logx::DataDir() + L"\\burst.txt").c_str(), L"w") == 0 && f) {
                for (int i = 0; i < 40; ++i) {
                    fputs(g_burstText[i], f);
                    fputc('\n', f);
                }
                for (int i = 0; i < 40; ++i) {
                    fputs(g_burstMatrices[i], f);
                    fputc('\n', f);
                }
                const int n = g_burstUpdateCount.load();
                for (int i = 0; i < n; ++i) {
                    fputs(g_burstUpdateText[i], f);
                    fputc('\n', f);
                }
                fclose(f);
            }
        }
        return;
    }
    // A flip seen in the first person (user, 2026-11-10: "the camera is inverted and in the second half of the flip I see Chris's body as
    // if he faced me"): with an empty DataDir\\fp_flipburst, the next 100 whole pictures, a quarter each way (fburst_NN.bmp), from the
    // head's tilt past 25 degrees.
    static bool flipArmed = false;
    if (flipArmed) {
        if (g_flipLatched && hooks::RequestFastBurstWhole(100, 4, 2)) {
            flipArmed = false;
            LOG("burst research: a flip, 100 pictures from now");
        }
        return;
    }
    if (armed) {
        const uintptr_t p = g_headPlayer;
        uint32_t action = 0;
        float root[3]{};
        if (p && mem::Read(p + 0x10E0, action) && mem::SafeRead(reinterpret_cast<void*>(p + 0x30), root, sizeof(root))) {
            const float dx = root[0] - last[0], dz = root[2] - last[2];
            std::memcpy(last, root, sizeof(root));
            walking = action == 0x1C && dx * dx + dz * dz > 0.25f * 0.25f ? walking + 1 : 0;
            if (walking >= 30 && hooks::RequestFastBurst(900, 700, 900, 900, 48)) {
                armed = false;
                left = 40;
                index = 0;
                g_burstUpdateCount = 0;
                g_burstUpdates = true;
                LOG("burst research: walking while aiming, 40 pictures from now");
            }
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - checked < 500) return;
    checked = now;
    const std::wstring flipFlag = logx::DataDir() + L"\\fp_flipburst";
    if (GetFileAttributesW(flipFlag.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(flipFlag.c_str());
        flipArmed = true;
        LOG("burst research: armed for a flip");
        return;
    }
    const std::wstring flag = logx::DataDir() + L"\\fp_burst";
    if (GetFileAttributesW(flag.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(flag.c_str());
    armed = true;
    g_burstOn = true;
    walking = 0;
    LOG("burst research: armed");
}

bool FirstPersonHidesSheath(uintptr_t player) {
    return player && player == actors::Player(0) && g_firstPerson.load(std::memory_order_relaxed) &&
           g_firstPersonApplied.load(std::memory_order_relaxed);
}

// action_log.cpp's request core: a Lost in Nightmares door opening (see DoorCameraPlaying).
// A door whose asked action (3-5) is in its list (+0x10DC, made at its creation) a cOmDoorActOpenClassic (vtable RVA
// 0xBE1E84): whatever "Ouverture des portes" made of it.
void DoorActionRequested(uintptr_t unit, int action) {
    uintptr_t list = 0, object = 0, vtable = 0;
    uint32_t count = 0;
    if (action < 3 || action > 5 || !unit || !mem::Read(unit + 0x10D0, count) || static_cast<uint32_t>(action) >= count ||
        !mem::Read(unit + 0x10DC, list) || !list || !mem::Read(list + 4 * action, object) || !object ||
        !mem::Read(object, vtable) || vtable != reinterpret_cast<uintptr_t>(mem::Rva(0xBE1E84)))
        return;
    g_doorOpenedAt = GetTickCount() | 1;
}

bool FirstPersonKeepsNormalCamera() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    return g_firstPerson.load(std::memory_order_relaxed) && p && FirstPersonOnFoot(p) && !FirstPersonPaused(p);
}

namespace {
bool UseCameraCave(bool on) {
    if (on) {
        if (!EnsureCameraCave() || (g_cameraUsers == 0 && !g_cameraSite->Apply())) return false;
        if (g_cameraUsers == 0 && g_blenderSite) g_blenderSite->Apply();
        ++g_cameraUsers;
        return true;
    }
    if (g_cameraUsers > 0 && --g_cameraUsers == 0 && g_cameraSite) {
        g_cameraSite->Restore();
        if (g_blenderSite && g_blenderSite->IsApplied()) g_blenderSite->Restore();
    }
    return true;
}

// "Ouverture des portes" "Classiques": player 1 in a campaign door's event (cOmDoorEventPL_ActOpenHand / ActOpenKick,
// vtable RVA 0xBE21CC / 0xBE21F8) has the trainer's first person for as long as it lasts (the camera cave and the fade
// hook taken by the choice); with "Vue à la première personne" off it is dropped at the event's end.
// The third person given back 0.6 s after the event (user, 2026-11-03: given back as the event ended, Chris's head came
// back wrong - the joints put back while his motion layers changed over to his own motions; switching the first person
// on and off by hand, in a settled pose, mended it).
bool g_doorFirstPersonNow = false;  // render thread
DWORD g_doorEventEnd = 0;           // render thread: when player 1 left the door's event

// The scopes (user, 2026-11-04: "cut the sniper rifles' switch to the scope camera in the iron sight mode, I want to
// keep aiming normally"; the rocket launchers' is the same): the scope is the callback camera (ctrl +0x2DE0, its entry
// [+0x250] +0x10, priority +4: 7 while scoped, the aim's 2) taking over in the controller's choice (VA 0x841F47: its
// "active" VA 0x842AF0, then its priority against the current camera's). While the sights are wanted and player 1 aims
// with a scoped weapon, that camera answers "not active": the aim camera stays, the iron sights apply.
std::atomic<bool> g_noScope{ false };
using CallbackActiveFn = bool(__fastcall*)(void* camera, void* edx);
CallbackActiveFn g_callbackActive = nullptr;

bool __fastcall HookCallbackActive(void* camera, void* edx) {
    if (g_noScope.load(std::memory_order_relaxed) && reinterpret_cast<uintptr_t>(camera) == CameraCtrl() + 0x2DE0) return false;
    return g_callbackActive(camera, edx);
}

bool EnsureNoScopeHook() {
    static bool hooked = false, failed = false;
    if (hooked || failed) return hooked;
    uint8_t* target = mem::Rva(0x442AF0);
    const uint8_t expected[] = { 0x8B, 0x81, 0x50, 0x02, 0x00, 0x00, 0x83, 0x78, 0x10, 0x00 };  // mov eax, [ecx+0x250]; cmp [eax+0x10], 0
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("camera: callback camera check, unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        failed = true;
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookCallbackActive), reinterpret_cast<void**>(&g_callbackActive));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        failed = true;
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        failed = true;
        return false;
    }
    hooked = true;
    LOG("camera: callback camera check hooked at %p", target);
    return true;
}

// The scope also hides Chris and his weapon (user, 2026-11-04: "hidden as if the game did not want them in the scope's
// way: keep them visible in this mode"): every frame the game sets each unit's draw byte +7 (bit 0 drawn: 0x0F / 0x0E)
// through VA 0xB540C0 (thiscall(unit, byte), ret 4); while the scope is cut, player 1 and what is attached to him
// (+0x18, his weapons) keep bit 0.
using SetDrawByteFn = void(__fastcall*)(void* unit, void* edx, uint8_t value);
SetDrawByteFn g_setDrawByte = nullptr;

void __fastcall HookSetDrawByte(void* unit, void* edx, uint8_t value) {
    if (g_noScope.load(std::memory_order_relaxed) && !(value & 1) && unit) {
        const uintptr_t u = reinterpret_cast<uintptr_t>(unit), p = actors::Player(0);
        uintptr_t parent = 0;
        if (p && (u == p || (mem::Read(u + 0x18, parent) && parent == p))) value |= 1;
    }
    g_setDrawByte(unit, edx, value);
}

bool EnsureScopeDrawHook() {
    static bool hooked = false, failed = false;
    if (hooked || failed) return hooked;
    uint8_t* target = mem::Rva(0x7540C0);
    const uint8_t expected[] = { 0x8A, 0x44, 0x24, 0x04, 0x88, 0x41, 0x07, 0xC2, 0x04, 0x00 };  // mov al, [esp+4]; mov [ecx+7], al; ret 4
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("camera: draw byte setter, unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        failed = true;
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookSetDrawByte), reinterpret_cast<void**>(&g_setDrawByte));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        failed = true;
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        failed = true;
        return false;
    }
    hooked = true;
    LOG("camera: draw byte setter hooked at %p", target);
    return true;
}

// The scoped weapons have no laser nor crosshair of their own outside the scope (user, 2026-11-04: "give them back a
// crosshair or a laser in the iron sight mode"): a small dot where the weapon points while their scope is cut and the
// eye is at the sights - a point 30 m along the barrel put on the picture with the frame's own view-projection
// (sCamera [RVA 0xE3457C] +0x3B0: viewport 0's, a row per clip coordinate, w the last). Without its scope such a weapon
// moves in a free zone of ~7 degrees before the camera turns (user: a dot at the middle did not follow it).
void DrawScopeCrosshair() {
    if (!g_noScope.load(std::memory_order_relaxed) || g_ironWeight.load(std::memory_order_relaxed) < 0.5f) return;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    float front[3], barrel[3], top[3], length = 0.0f, m[16]{};
    uintptr_t camera = 0;
    if (!SightLine(p, front, barrel, top, length) || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE3457C)), camera) ||
        !camera || !mem::SafeRead(reinterpret_cast<void*>(camera + 0x3B0), m, sizeof(m)))
        return;
    float point[4] = { front[0] + barrel[0] * 3000.0f, front[1] + barrel[1] * 3000.0f, front[2] + barrel[2] * 3000.0f, 1.0f };
    float clip[4];
    for (int r = 0; r < 4; ++r)
        clip[r] = m[4 * r] * point[0] + m[4 * r + 1] * point[1] + m[4 * r + 2] * point[2] + m[4 * r + 3] * point[3];
    if (clip[3] <= 1e-3f) return;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const ImVec2 c((clip[0] / clip[3] * 0.5f + 0.5f) * screen.x, (0.5f - clip[1] / clip[3] * 0.5f) * screen.y);
    if (c.x < 0.0f || c.y < 0.0f || c.x > screen.x || c.y > screen.y) return;
    const float unit = screen.y / 1080.0f;  // a dot only (user, 2026-11-04)
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddCircleFilled(c, 3.2f * unit, IM_COL32(0, 0, 0, 170));
    const uint32_t rgb = LaserColourRgb();  // the laser colour option's (user, 2026-11-04)
    dl->AddCircleFilled(c, 2.0f * unit, IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 235));
}

bool ScopedItem(uint16_t id) {
    switch (id) {
    case 0x0105: case 0x011C: case 0x0120:  // sniper rifles
    case 0x010D: case 0x0135:               // rocket launchers
        return true;
    default:
        return false;
    }
}

// "Première personne pendant la visée": in the third person, the first person while player 1 aims (its reloads in the
// aim kept), the third person back as soon as he stops; an iron sight too with "Visée iron sight".
void TickIronSightThird() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    {
        const bool sightsWanted = g_ironFirst.load(std::memory_order_relaxed) &&
                                  (g_firstPersonOption.load(std::memory_order_relaxed) || g_ironThird.load(std::memory_order_relaxed));
        // Also through an RE6 quick shot (quick_knife.cpp QuickShotCutsScope: its animation seen, not the scope).
        const bool noScope = p && ((sightsWanted && Aims(p, true)) || QuickShotCutsScope()) && ScopedItem(HeldItem(p));
        g_noScope = noScope && EnsureNoScopeHook() && EnsureScopeDrawHook();
    }
    // The Jail Breaker aims with the Ithaca's motions whenever its sights may be used (quick_knife.cpp).
    IronSightWeaponBank(p, g_ironFirst.load(std::memory_order_relaxed) &&
                               (g_firstPersonOption.load(std::memory_order_relaxed) || g_ironThird.load(std::memory_order_relaxed)));
    const bool option = g_firstPersonOption.load(std::memory_order_relaxed);
    const bool now = g_ironThirdNow.load(std::memory_order_relaxed);
    if (g_ironThird.load(std::memory_order_relaxed) && !option && Aims(p, now)) {
        if (!now) {
            g_ironThirdNow = true;
            g_firstPerson = true;
        }
        return;
    }
    if (!now) return;
    g_ironThirdNow = false;
    g_ironWeight = 0.0f;
    if (option || g_doorFirstPersonNow) return;
    g_firstPerson = false;
    ShowHead();
}

void TickDoorFirstPerson() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t action = 0, vtable = 0;
    const bool door = g_doorFirstPersonOn.load(std::memory_order_relaxed) && p && mem::Read(p + 0x10F8, action) && action &&
                      mem::Read(action, vtable) &&
                      (vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xBE21CC)) ||
                       vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xBE21F8)));
    const bool option = g_firstPersonOption.load(std::memory_order_relaxed);
    if (door) {
        g_doorEventEnd = 0;
        if (g_doorFirstPersonNow) return;
        g_doorFirstPersonNow = true;
        if (option) return;  // the first person is on anyway
        g_firstPerson = true;
        LOG("camera: a door opening, the first person for it");
        return;
    }
    if (!g_doorFirstPersonNow) return;
    const DWORD now = GetTickCount();
    if (p && !g_doorEventEnd) {
        g_doorEventEnd = now ? now : 1;
        return;
    }
    if (p && now - g_doorEventEnd < 600) return;
    g_doorFirstPersonNow = false;
    g_doorEventEnd = 0;
    if (option) return;
    g_firstPerson = false;
    ShowHead();
    LOG("camera: the door passed, the third person again");
}
}  // namespace

bool Player1PadInUse();  // movement.cpp
void SetPadMouseWay(bool on);                  // movement.cpp: the pad's aim through the game's mouse aim
void SetMouseLaserWay(bool on);                // movement.cpp: the mouse through the game's pad aim
bool LaserOnKeyboard();                       // patch_features.cpp: "Laser au clavier/souris"
bool CrosshairOnPad();                        // patch_features.cpp: "Réticule à la manette"

namespace {
// "Manette : visée comme à la souris" (user, 2026-11-06: "the crosshair on the pad is really annoying"). Recorded
// (tools/aimrec.py, research/pad_aim/rec1.txt): both controls aim through the same free zone - the weapon up to ~7
// degrees off the body (input +0x450, its copy P+0x1954, back to 0 out of the aim) before the body turns - but the aim
// camera follows the aim with the mouse and the body with a pad. The player's aim update (VA 0xB751BC, sBioPad "pad in
// use" VA 0x799AF0) builds, for the mouse only, the aim basis from the aim angles P+0x2A20 / 2A24 / 2A28 (body + zone,
// VA 0xCDE1D0), and gives it with P+0x1180 to the aim camera through VA 0x845260 (thiscall(cam = ctrl +0xE00, matrix,
// vector), copied to cam +0x1E0..0x21C / +0x220; one caller, VA 0xB753FB). The pad's mouse rendering is now the game's
// own mouse aim fed by the right stick (movement.cpp SetPadMouseWay). Tried before (2026-11-06, MODLOG): the basis turned
// by the zone, then the zone set to 0 with the basis turned by this update's turn, then a trainer-driven aim yaw - none
// gave the mouse's feel (Chris trailing the camera, his turning steps, the same top for the camera and the weapon).
// The rendering (user, 2026-11-06): with the crosshair always the mouse's, whatever the control; with the laser the
// mouse's for the mouse and the pad's for the pad by default, each swapped by its option.
std::atomic<bool> g_padAimLikeMouse{ false };  // "Manette : rendu souris avec le laser"
std::atomic<bool> g_mouseAimLikePad{ false };  // "Souris : rendu manette avec le laser"
}  // namespace

bool MouseLaserLikePad() { return g_mouseAimLikePad.load(std::memory_order_relaxed) && LaserOnKeyboard(); }
void SetPadAimLikeMouse(bool on) { g_padAimLikeMouse = on; }  // patch_features.cpp's options, with the other aim markers
void SetMouseAimLikePad(bool on) { g_mouseAimLikePad = on; }

bool g_re6CameraActive = false;  // the blocks' places are the slide's: the camera sliders that are on wait (see their tick)
Feature* g_re6CameraWidth = nullptr, *g_re6CameraHeight = nullptr, *g_re6CameraDepth = nullptr;  // "Caméra au sol (RE6) : ..."

uint16_t PlayerHeldItem(uintptr_t player);  // knife.cpp

// wesker_moves.cpp: the camera follows him down, `part` from 0 to 1. A block holds the eye (x, y, z at 0, 1, 2) and the point looked at
// (at 4, 5, 6): both move by the sliders, the view's direction kept (user, 2026-11-08: pulling only the eye back "raised" the camera
// instead of moving it back). The normal and the aim cameras, over what they held; 0 gives them back.
void SetRe6CameraDrop(float part) {
    static bool active = false;
    static float base[2][6];
    static uintptr_t blocks[2] = { 0, 0 };  // the blocks of when it started: the active camera changes (the knife drawn on the ground: user, 2026-11-08, its
                                            // blocks got this offset and kept it, "the camera fell and comes back up with the pistol")
    static constexpr int kBlocks[2] = { 1, 4 }, kIndices[6] = { 0, 4, 1, 5, 2, 6 };
    static constexpr float kMinLookDepth = 30.0f;
    // A weapon change while he is down (user, 2026-11-09: "the camera problem when switching to the grenades or the knife on the ground"):
    // the change rebuilds the active camera's blocks and the owner's copies for the new weapon from what it finds (our offset in them), and
    // the knife's / grenades' aim set is not the firearm's we moved. Given back as at the drop's end when it starts (action 0x20, or the held
    // item changed), taken again from the new weapon's once it is over.
    static uint16_t heldAtStart = 0;
    static bool suspended = false;
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    uint32_t p1Action = 0;
    if (p1) mem::Read(p1 + 0x10E0, p1Action);
    const uint16_t heldNow = p1 ? PlayerHeldItem(p1) : 0;
    if (part > 0.0f && active && (p1Action == 0x20 || heldNow != heldAtStart)) {
        LOG("camera: RE6 drop given back for a weapon change (action %X, item %04X -> %04X)", p1Action, heldAtStart, heldNow);
        SetRe6CameraDrop(0.0f);
        suspended = true;
        return;
    }
    if (suspended) {
        if (part > 0.0f && p1Action == 0x20) return;
        suspended = false;
        if (part > 0.0f) LOG("camera: RE6 drop taken again on item %04X", heldNow);
    }
    if (part <= 0.0f && !active) return;
    if (active && !actors::InGame()) {  // a load: those blocks are gone
        active = false;
        g_re6CameraActive = false;
        return;
    }
    // The camera's own copies of its blocks (owner +0x2C0 .. +0x3B0, 0x30 each): a weapon change rebuilds them from the blocks, offset included, and they
    // stayed low once he was up (user, 2026-11-08: "it went down again after the switch to the knife"; it came back at the next weapon change).
    static constexpr uintptr_t kCopies[6] = { 0x2C0, 0x2F0, 0x320, 0x350, 0x380, 0x3B0 };
    static uintptr_t copiesOwner = 0;
    static float copies[6][12];
    if (!active) {
        heldAtStart = heldNow;
        copiesOwner = CameraOwner();
        for (int k = 0; k < 6 && copiesOwner; ++k)
            if (!mem::SafeRead(reinterpret_cast<void*>(copiesOwner + kCopies[k]), copies[k], sizeof(copies[k]))) copiesOwner = 0;
    }
    const float amount = std::max(part, 0.0f);
    const auto slider = [](const Feature* f) { return f && f->enabled ? f->value : 0.0f; };
    const float moved[3] = { slider(g_re6CameraWidth), slider(g_re6CameraHeight), -slider(g_re6CameraDepth) };
    for (int b = 0; b < 2; ++b) {
        if (!active) blocks[b] = CameraBlock(kBlocks[b]);
        if (!blocks[b]) continue;
        for (int i = 0; i < 6; ++i) {
            const uintptr_t field = blocks[b] + 4 * kIndices[i];
            if (!active) mem::Read(field, base[b][i]);
            float value = base[b][i] + amount * moved[i / 2];
            // The point looked at stays in front of him (user, 2026-11-08: past some depth "the camera looks behind", [6] below 0):
            // beyond that only the eye goes on moving back.
            if (kIndices[i] == 6) value = std::max(value, std::min(base[b][i], kMinLookDepth));
            mem::Write(field, value);
        }
    }
    if (part <= 0.0f && copiesOwner && copiesOwner == CameraOwner())
        for (int k = 0; k < 6; ++k) mem::SafeWrite(reinterpret_cast<void*>(copiesOwner + kCopies[k]), copies[k], sizeof(copies[k]));
    active = part > 0.0f;
    g_re6CameraActive = active;
}

void RegisterCameraFeatures() {
    OnRestore([] { SetRe6CameraDrop(0.0f); });
    g_re6CameraHeight = &Add({ .id = "camera.re6_ground_height", .name = "Caméra en glissade (RE6) : hauteur",
                             .help = "Monte (+) ou descend (-) la caméra à la troisième personne pendant les esquives et la glissade de RE6 et au sol, sans changer son angle.",
                             .category = "Caméra", .kind = Kind::Toggle, .set = [](bool) { return true; }, .available = Supported,
                             .value = -70, .valueMin = -300, .valueMax = 300, .valueFormat = "%.0f cm", .defaultOn = true });
    g_re6CameraDepth = &Add({ .id = "camera.re6_ground_depth", .name = "Caméra en glissade (RE6) : profondeur",
                             .help = "Recule (+) ou avance (-) la caméra à la troisième personne pendant les esquives et la glissade de RE6 et au sol, sans changer son angle.",
                             .category = "Caméra", .kind = Kind::Toggle, .set = [](bool) { return true; }, .available = Supported,
                             .value = 150, .valueMin = -300, .valueMax = 2400, .valueFormat = "%.0f cm", .defaultOn = true });
    g_re6CameraWidth = &Add({ .id = "camera.re6_ground_width", .name = "Caméra en glissade (RE6) : largeur",
                             .help = "Décale la caméra sur le côté à la troisième personne pendant les esquives et la glissade de RE6 et au sol, sans changer son angle.",
                             .category = "Caméra", .kind = Kind::Toggle, .set = [](bool) { return true; }, .available = Supported,
                             .value = 0, .valueMin = -300, .valueMax = 300, .valueFormat = "%.0f cm", .defaultOn = true });
    // Which aim each control runs, by the rendering rule above (movement.cpp): a pad through the game's mouse aim; the mouse
    // with its laser and "Souris : rendu manette avec le laser" through the game's pad aim (2026-11-08: the aim camera's basis
    // turned to the body instead locked the camera's yaw, in the aim and out of it, while the mouse still turned Chris).
    OnFrame([] {
        if (!game::IsSupported()) return;
        // Once a level is there, the game's values in the camera blocks for every slider that is off (2026-11-08: the slide's camera
        // left in them by an instance that could not give them back; "even after a reload the camera is at the legs").
        static bool checked = false;
        if (!checked && actors::InGame() && CameraBlock(1) && CameraBlock(4)) {
            checked = true;
            g_gameValuesPending = true;
            LOG("camera: the game's values written in the camera blocks");
        }
        const bool pad = actors::InGame() && Player1PadInUse();
        SetPadMouseWay(pad && (CrosshairOnPad() || g_padAimLikeMouse.load(std::memory_order_relaxed)));
        SetMouseLaserWay(actors::InGame() && !pad && MouseLaserLikePad());
    });
    Add({ .id = "camera.freeze_cam1", .name = "Figer la caméra (photo)",
          .help = "Immobilise complètement la caméra, pour les captures d'écran.", .category = "Caméra",
          .kind = Kind::Toggle,
          .set = [](bool on) {
              static mem::Patch a("camera.freeze1", 0x441CF3, { 0x00 }, { 0x7F });
              static mem::Patch b("camera.freeze1", 0x7865DE, { 0x74, 0x02 }, { 0x90, 0x90 });
              return on ? mem::ApplyAll({ &a, &b }) : mem::RestoreAll({ &b, &a });
          },
          .available = Supported });
    // Every frame uCameraCtrl (RVA 0x441F47..0x441F8D) picks its active camera (+0x1B0): the animation-driven
    // uCameraCallback (+0x2DE0) when an animation asks for it with a higher priority (melee attacks: action 0x32),
    // else the normal camera [+0xDF0] (uCameraQFPS). The normal camera's values (the sliders) did not apply during
    // melee (user, 2026-10-14). Jumping always to the normal-camera branch keeps it (UT "Freeze melee camera").
    Add({ .id = "camera.keep_in_melee", .name = "Garder la caméra normale au corps à corps",
          .help = "Pendant les attaques au corps à corps, la caméra reste la caméra normale (avec tes réglages) au lieu "
                  "de passer à la caméra de l'animation (option « Freeze melee camera » de l'Ultimate Trainer). Sans effet "
                  "pendant la visée avec un fusil à lunette, sur une mitrailleuse fixe et à l'ouverture des portes de Lost in "
                  "Nightmares (leur caméra reste celle du jeu).",
          .category = "Caméra", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on) EnsureEnemyActionHook();  // the doors of Lost in Nightmares (DoorActionRequested)
              g_keepInMeleeOn = on;
              if (!on) return g_keepInMelee.IsApplied() ? g_keepInMelee.Restore() : true;
              return g_mountedGun || g_scopedRifle || g_keepInMelee.Apply();
          },
          .available = Supported });
    Add({ .id = "camera.door_style", .name = "Ouverture des portes",
          .help = "Par défaut : chaque porte s'ouvre à sa façon (celles de Lost in Nightmares à la première personne, les "
                  "autres comme dans la campagne). Classiques : pendant l'ouverture d'une porte de la campagne, la caméra "
                  "passe à la première personne du trainer ; elle revient à la troisième personne à la fin si « Vue à la "
                  "première personne » n'est pas cochée. Normales : toutes les portes s'ouvrent comme dans la campagne, le "
                  "personnage poussant la porte, vu à la troisième personne (sauf les deux du rez-de-chaussée du hall du "
                  "manoir) ; elles prennent cette façon à leur chargement : relancer le point de contrôle ou changer de zone.",
          .category = "Caméra", .kind = Kind::Choice, .available = Supported,
          .choose = [](int index) {
              index = std::clamp(index, 0, 2);
              if (index == 2 && !EnsureClassicTableHook()) return false;
              const bool firstPerson = index == 1;
              if (firstPerson != g_doorFirstPersonOn.load()) {
                  if (firstPerson && (!EnsureCameraFadeHook() || !UseCameraCave(true))) return false;
                  if (!firstPerson) UseCameraCave(false);
                  g_doorFirstPersonOn = firstPerson;
              }
              g_doorStyle = index;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 2, .valueFormat = "%.0f",
          .choices = { "Par défaut", "Classiques", "Normales" } });
    Add({ .id = "camera.freeze_cam2", .name = "Figer la caméra (réglable)",
          .help = "Fige la caméra tout en permettant de l'ajuster.", .category = "Caméra", .kind = Kind::Toggle,
          .set = [](bool on) {
              static mem::Patch a("camera.freeze2", 0x442910, { 0x00 }, { 0x01 });
              static mem::Patch b("camera.freeze2", 0x7741EA, { 0x01 }, { 0x00 });
              static mem::Patch c("camera.freeze2", 0x774328, { 0x00 }, { 0x01 });
              return on ? mem::ApplyAll({ &a, &b, &c }) : mem::RestoreAll({ &c, &b, &a });
          },
          .available = Supported });

    for (const CamField& f : kFields) {
        const CamField* field = &f;
        Feature& feature = Add({
            .id = f.id, .name = f.name,
            .help = "Cocher pour imposer la valeur du curseur (le jeu ne la remplace plus). Décocher remet la valeur d'origine.",
            .category = "Caméra", .kind = Kind::Toggle,
            .available = [] { return Supported() && CameraBlock(1) != 0; },
            .value = f.def, .valueMin = f.lo, .valueMax = f.hi, .valueFormat = "%.0f" });
        Feature* self = &feature;
        feature.set = [field](bool on) {
            uintptr_t block = CameraBlock(field->block);
            if (!block) return false;
            if (on) {
                AcquireFreeze();
            } else {
                mem::Write(block + 4 * field->index, field->def);
                ReleaseFreeze();
            }
            return true;
        };
        feature.tick = [field, self] {
            // During the slide its camera moves these fields, over the sliders' values (measured 2026-11-08: "Caméra de visée :
            // recul" on, written every frame, kept the depth at -78 whatever the slide's slider said).
            if (g_re6CameraActive && field->index != 9) return;
            if (uintptr_t block = CameraBlock(field->block)) mem::Write(block + 4 * field->index, self->value);
        };
    }

    Add({ .id = "camera.fov_lod", .name = "Décor complet avec un grand champ de vision",
          .help = "Le jeu choisit le niveau de détail et la distance d'affichage des morceaux du décor d'après le champ de "
                  "vision : plus il est large, plus tout semble loin, et des murs ou des zones entières disparaissaient "
                  "(première personne, grand champ de vision). Coché : le décor garde la distance d'affichage du champ de "
                  "vision du jeu (42,5°) quand le champ est plus large ; les zooms de la visée gardent la leur.",
          .category = "Caméra", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (!EnsureLodCave()) return false;
              return on ? g_lodSite->Apply() : g_lodSite->Restore();
          },
          .available = Supported,
          .defaultOn = true });

    // Field of view and first person (user, 2026-11-01; the view copy's cave above).
    Feature& fov = Add({ .id = "camera.fov", .name = "Champ de vision",
                         .help = "Le champ de vision des caméras de jeu (normale, visée, corps à corps), en degrés pour la "
                                 "caméra normale (42.5 = celui du jeu) : les autres suivent dans la même proportion (le "
                                 "zoom de la visée reste). Les cinématiques gardent le leur.",
                         .category = "Caméra", .kind = Kind::Toggle,
                         .available = Supported,
                         .value = kGameFov, .valueMin = 20.0f, .valueMax = 120.0f, .valueFormat = "%.1f°" });
    {
        Feature* const self = &fov;
        fov.set = [self](bool on) {
            if (!UseCameraCave(on)) return false;
            g_fov = self->value;
            g_fovOn = on;
            return true;
        };
        fov.tick = [self] { g_fov.store(self->value, std::memory_order_relaxed); };
    }
    Feature& fpFov = Add({ .id = "camera.first_person_fov", .name = "Champ de vision à la première personne",
                           .help = "Le champ de vision de la vue à la première personne, en degrés (la visée zoome dans la "
                                   "même proportion que la caméra du jeu). Décoché : 75°.",
                           .category = "Caméra", .kind = Kind::Toggle,
                           .available = Supported,
                           .value = 75.0f, .valueMin = 20.0f, .valueMax = 120.0f, .valueFormat = "%.1f°" });
    {
        Feature* const self = &fpFov;
        fpFov.set = [self](bool on) {
            g_firstPersonFov = on ? self->value : 75.0f;
            return true;
        };
        fpFov.tick = [self] { g_firstPersonFov.store(self->value, std::memory_order_relaxed); };
    }
    // The eye's place from the head joint (user, 2026-11-01: "Chris's body often meets the camera while he moves, the
    // pieces are seen from inside: three sliders, height, width, depth"), in the head's frame; unchecked: the defaults.
    static constexpr struct {
        const char* id;
        const char* name;
        const char* help;
        int axis;  // 0 height, 1 width, 2 depth
        float def, lo, hi;
    } kEyeOffsets[] = {
        { "camera.first_person_height", "Première personne : hauteur de la caméra",
          "La hauteur de la caméra à la première personne au-dessus de la base de la tête de Chris, en cm, le long de sa tête (elle suit son inclinaison dans les animations). Décoché : 9 cm.", 0, 9.0f,
          -30.0f, 40.0f },
        { "camera.first_person_width", "Première personne : largeur de la caméra",
          "Le décalage de la caméra à la première personne vers la droite de la tête de Chris (négatif : vers sa gauche), en cm. "
          "Décoché : 0.", 1, 0.0f, -40.0f, 40.0f },
        { "camera.first_person_depth", "Première personne : profondeur de la caméra",
          "L'avance de la caméra à la première personne devant la base de la tête de Chris, dans la direction de son visage (négatif : en arrière), en cm ; plus loin "
          "devant, son corps rentre moins dans la caméra quand il bouge. Décoché : 9 cm.", 2, 9.0f, -30.0f, 60.0f },
    };
    for (const auto& offset : kEyeOffsets) {
        Feature& f = Add({ .id = offset.id, .name = offset.name, .help = offset.help, .category = "Caméra",
                           .kind = Kind::Toggle, .available = Supported, .value = offset.def, .valueMin = offset.lo,
                           .valueMax = offset.hi, .valueFormat = "%.0f cm" });
        Feature* const self = &f;
        const int axis = offset.axis;
        const float def = offset.def;
        f.set = [self, axis, def](bool on) {
            g_eyeOffset[axis] = on ? self->value : def;
            return true;
        };
        f.tick = [self, axis] { g_eyeOffset[axis].store(self->value, std::memory_order_relaxed); };
    }
    Feature& fpNear = Add({ .id = "camera.first_person_near", .name = "Première personne : distance de coupe proche",
                            .help = "Ce qui est plus près de la caméra que cette distance n'est pas dessiné (le jeu : 16 cm, "
                                    "d'où l'épaule ou le couteau coupés à la première personne). Plus petit = moins de coupe ; "
                                    "2 cm au moins : plus bas, les lumières et les surfaces lointaines scintillaient. "
                                    "Décoché : 3 cm.",
                            .category = "Caméra", .kind = Kind::Toggle, .available = Supported, .value = 3.0f,
                            .valueMin = kMinNear, .valueMax = 16.0f, .valueFormat = "%.1f cm" });
    {
        Feature* const self = &fpNear;
        fpNear.set = [self](bool on) { g_firstPersonNear = on ? self->value : 3.0f; return true; };
        fpNear.tick = [self] { g_firstPersonNear.store(self->value, std::memory_order_relaxed); };
    }
    Add({ .id = "camera.first_person_body", .name = "Première personne : corps visible",
          .help = "Ce qui reste visible de Chris à la première personne, comme en réalité virtuelle : le corps entier "
                  "(sans la tête), les bras, les avant-bras ou les mains seulement. Les parties cachées ne changent rien "
                  "au jeu (les ennemis le touchent comme avant).",
          .category = "Caméra", .kind = Kind::Choice, .available = Supported,
          .choose = [](int index) {
              g_bodyMode = std::clamp(index, 0, 3);
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 3, .valueFormat = "%.0f",
          .choices = { "Corps entier", "Bras", "Avant-bras", "Mains seulement" } });
    Feature& bodyTurn = Add({ .id = "camera.first_person_body_rotation", .name = "Première personne : suivre les rotations du corps",
                              .help = "À la première personne, pendant les esquives (celles du jeu et du trainer), les franchissements et les coups au corps à corps cochés dans la colonne « Suivi 1re pers. » des tableaux de la Mêlée libre (tous par défaut), la caméra tourne avec la tête de Chris, comme si elle faisait le mouvement avec lui (une roulade la fait rouler) ; le reste du temps elle ne suit pas son corps. Le curseur règle la force.",
                              .category = "Caméra", .kind = Kind::Toggle, .available = Supported, .value = 100.0f,
                              .valueMin = 0.0f, .valueMax = 100.0f, .valueFormat = "%.0f %%" });
    {
        Feature* const self = &bodyTurn;
        bodyTurn.set = [self](bool on) {
            g_bodyRotationStrength = self->value / 100.0f;
            g_bodyRotation = on;
            return true;
        };
        bodyTurn.tick = [self] {
            g_bodyRotationStrength.store(self->value / 100.0f, std::memory_order_relaxed);
            TickMeleeFollow();
        };
    }
    Add({ .id = "camera.first_person_third_traversal", .name = "Première personne : troisième personne pendant les traversées",
          .help = "À la première personne, la caméra repasse à la troisième personne pendant les sauts, les montées et "
                  "descentes, les clôtures, les échelles et les falaises, puis revient à la première personne.",
          .category = "Caméra", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_thirdInTraversal = on;
              return true;
          },
          .available = Supported });
    Add({ .id = "camera.first_person_third_grab", .name = "Première personne : troisième personne pendant les saisies ennemies",
          .help = "À la première personne, la caméra repasse à la troisième personne quand un ennemi saisit Chris, puis "
                  "revient à la première personne quand il est libéré.",
          .category = "Caméra", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_thirdInGrab = on;
              return true;
          },
          .available = Supported });
    Add({ .id = "camera.first_person_third_melee", .name = "Première personne : troisième personne pendant les coups de mêlée",
          .help = "À la première personne, la caméra repasse à la troisième personne pendant les coups au corps à corps "
                  "(ceux du jeu et ceux du trainer), puis revient à la première personne.",
          .category = "Caméra", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_thirdInMelee = on;
              return true;
          },
          .available = Supported });
    Add({ .id = "camera.first_person", .name = "Vue à la première personne",
          .help = "La caméra normale et celle de la visée passent dans les yeux du joueur 1 (sa tête est rendue invisible "
                  "pour éviter les rendus bizarres) ; la souris ou le stick regardent comme d'habitude. Les caméras du "
                  "corps à corps et des cinématiques restent les leurs (sa tête revient avec elles ; « Garder la caméra "
                  "normale au corps à corps » garde la première personne pendant les coups). Pas dans les véhicules.",
          .category = "Caméra", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && (!EnsureCameraFadeHook() || !UseCameraCave(true))) return false;
              g_firstPersonOption = on;
              g_firstPerson = on;
              if (!on) {
                  ShowHead();
                  UseCameraCave(false);
                  if (g_cameraUsers == 0) RestoreNear(CameraCtrl());  // else the cave puts it back
              }
              return true;
          },
          .available = Supported });
    Add({ .id = "camera.ironsight", .name = "Visée iron sight",
          .help = "Pendant la visée à la première personne (« Vue à la première personne » ou « Première personne pendant "
                  "la visée »), avec une arme à feu, la caméra se place derrière les organes de visée de l'arme et regarde "
                  "le long du canon (le cran de mire et le guidon alignés au centre), puis revient aux yeux du personnage "
                  "quand il arrête de viser. Les grenades et le couteau gardent la vue normale.",
          .category = "Caméra", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_ironFirst = on;
              return true;
          },
          .available = Supported });
    Add({ .id = "camera.first_person_aim", .name = "Première personne pendant la visée",
          .help = "À la troisième personne, viser passe à la première personne ; la troisième personne revient dès que le "
                  "personnage arrête de viser (le rechargement pendant la visée la garde). Avec « Visée iron sight », la "
                  "caméra se met derrière les organes de visée de l'arme. Sans effet quand « Vue à la première personne » "
                  "est cochée.",
          .category = "Caméra", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on == g_ironThird.load()) return true;
              if (on && (!EnsureCameraFadeHook() || !UseCameraCave(true))) return false;
              g_ironThird = on;
              if (!on) {
                  if (g_ironThirdNow.exchange(false) && !g_firstPersonOption.load() && !g_doorFirstPersonNow) {
                      g_firstPerson = false;
                      ShowHead();
                  }
                  UseCameraCave(false);
              }
              return true;
          },
          .available = Supported });
    static constexpr struct {
        const char* id;
        const char* name;
        const char* help;
        int target;  // 0 distance, 1 height, 2 distance with a stock
        float def, lo, hi;
    } kIronSliders[] = {
        { "camera.ironsight_distance", "Visée iron sight : distance de l'œil",
          "La distance entre l'œil et l'arrière de l'arme pendant la visée iron sight, en cm (plus petit : l'arme paraît "
          "plus grande). Décoché : 20 cm.", 0, 20.0f, 0.0f, 80.0f },
        { "camera.ironsight_distance_stock", "Visée iron sight : distance de l'œil (armes longues)",
          "La même distance pour les armes autres que les pistolets et les magnums (mitraillettes, fusils d'assaut, fusils "
          "à pompe, fusils), mesurée depuis la poignée quand une crosse dépasse derrière ; le VZ61, l'AK-74, l'Ithaca, le "
          "M3 et le Jail Breaker ont en plus un réglage à eux. Décoché : 0 cm.", 2, 0.0f, -30.0f, 80.0f },
        { "camera.ironsight_height", "Visée iron sight : hauteur de l'œil",
          "La hauteur de l'œil au-dessus du dessus de l'arme pendant la visée iron sight, en cm (négatif : plus bas), pour "
          "aligner le cran de mire et le guidon. Décoché : 0.", 1, 0.0f, -15.0f, 15.0f },
    };
    Feature& roll = Add({ .id = "camera.ironsight_roll", .name = "Visée iron sight : rotation de la caméra",
                          .help = "L'animation de visée penche les pistolets et les magnums sur le côté : pendant la visée "
                                  "iron sight avec l'une de ces armes, la caméra tourne de cet angle autour de la direction de "
                                  "visée pour que l'arme paraisse droite (les fusils ne penchent pas). Décoché : 8°.",
                          .category = "Caméra", .kind = Kind::Toggle, .available = Supported, .value = 8.0f,
                          .valueMin = -30.0f, .valueMax = 30.0f, .valueFormat = "%.1f°" });
    {
        Feature* const self = &roll;
        roll.set = [self](bool on) {
            g_ironRollDegrees = on ? self->value : 8.0f;
            return true;
        };
        roll.tick = [self] { g_ironRollDegrees.store(self->value, std::memory_order_relaxed); };
    }
    for (const auto& slider : kIronSliders) {
        Feature& f = Add({ .id = slider.id, .name = slider.name, .help = slider.help, .category = "Caméra",
                           .kind = Kind::Toggle, .available = Supported, .value = slider.def, .valueMin = slider.lo,
                           .valueMax = slider.hi, .valueFormat = "%.1f cm" });
        Feature* const self = &f;
        std::atomic<float>* const target = slider.target == 0 ? &g_ironDistance : slider.target == 1 ? &g_ironHeight : &g_ironDistanceStock;
        const float def = slider.def;
        f.set = [self, target, def](bool on) {
            *target = on ? self->value : def;
            return true;
        };
        f.tick = [self, target] { target->store(self->value, std::memory_order_relaxed); };
    }
    OnRestore([] {  // the cave calls into the DLL: never left behind it
        g_noScope = false;
        g_ironThirdNow = false;
        g_ironWeight = 0.0f;
        g_firstPerson = false;
        g_fovOn = false;
        ShowHead();
        if (g_cameraSite && g_cameraSite->IsApplied()) g_cameraSite->Restore();
        if (g_blenderSite && g_blenderSite->IsApplied()) g_blenderSite->Restore();
        RestoreNear(CameraCtrl());
        g_firstPersonApplied = false;
        g_cameraUsers = 0;
    });

    for (int i = 0; i < kPresets; ++i) {
        Feature& action = Add({ .id = "camera.preset_" + std::to_string(i + 1), .name = "Activer le preset " + std::to_string(i + 1),
                                .help = "Active ce preset de caméra (et désactive les autres).",
                                .category = "Presets caméra", .kind = Kind::Action,
                                .run = [i] {
                                    ActivatePreset(i);
                                    return true;
                                } });
        g_presetActions[i] = &action;
    }
    OnFrame(UpdatePresets);
    OnFrame(TickDoorFirstPerson);
    OnFrame(TickIronSightThird);
    OnHud(DrawScopeCrosshair);
    OnHud(ResearchBurst);
    OnFrame(TickFirstPersonTrace);
    AddPanel("Caméra", DrawCameraPanel, { "Caméra", "Presets caméra" }, true);
}
}
