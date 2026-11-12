// "Faire apparaître des ennemis" (user, 2026-10-23: the Reaper took long to come back after each death; a table in the
// Ennemis tab to spawn the enemy types). The game spawns through sEnemy::SetEnemy (RVA 0x3AF6E0, thiscall(a, b, flag)
// ret 0xC on sEnemy [RVA 0xDB1FCC]) with a layout entry's two ids: the stage scripts' "set enemy" order (RVA 0x82E070:
// param +8 / +0xC, flag 1 or 0) calls it. It adds 1 to the Mercenaries spawned count (cGameSys +0x584) first; in
// Mercenaries it needs cGameSys +0x574 == +0x578; the creation (sEnemy vfunc +0x34 -> RVA 0x3AF5F0(a, b, 1)) is
// refused when sEnemy vfunc +0x40 says so (sBioMain +0x10434 bit 31), when vfunc +0x44(a, b) does (sBioMain RVA
// 0x37E4A0(a, b): the entry used), or when a unit with these ids lives (RVA 0x3F2FD0: sEnemy +0x24 count, +0x30 units);
// then the entry's class (by name) is built (the uEnemy base constructor RVA 0x5DB170) and keeps the ids at +0xD50 /
// +0xD54 (-1 for the parts a unit creates itself). The level's layout: sEnemy +0x60 groups, +0x6C their rLayout objects
// (+0x64 entries, +0x70 entry array; entry +0xC a string object, the class name at +8). Logged with
// diag.enemy_create_log, 2026-10-23: Majini (3, n), fat man (3, 0), Reaper (9, 0), chicken (0, B); the first try took
// a live unit's ids and was always refused.
// The table lists the classes of the level's layout; a spawn picks a free entry of that class (no unit with its ids, the
// game's vfunc +0x44 test passed) and calls SetEnemy on the game thread (quick_knife.cpp's pad query hook); the new unit
// can be put in front of player 1. Only the types the level's layout has can come: the others need their files loaded.
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace features {
bool EnsurePadQueryHooks();  // quick_knife.cpp: the game-thread tick below

namespace {
constexpr uintptr_t kEnemySystem = 0xDB1FCC;  // sEnemy (RVA of its pointer)

struct LevelType {
    int entries = 0;
    int alive = 0;
};
std::mutex g_typesMutex;
std::map<std::string, LevelType> g_types;  // render thread writes, menu reads

// A request for the game thread (its class name), and its unit for the render thread.
std::mutex g_requestMutex;
std::string g_requestClass;
std::atomic<bool> g_requested{ false };
std::atomic<uintptr_t> g_spawned{ 0 };
std::atomic<DWORD> g_spawnedTick{ 0 };
bool g_inFront = true;                    // menu
constexpr float kFrontDistance = 300.0f;  // cm in front of player 1
// The new unit held there while it settles: it enters the enemy list only later (user, 2026-10-23: two Executioners
// spawned out of sight, left at the layout entry's position (-26, 50, -22) - Mercenaries places its own enemies).
constexpr DWORD kPlaceMs = 1500;

uintptr_t EnemySystem() {
    uintptr_t system = 0;
    return mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(kEnemySystem)), system) ? system : 0;
}

// The class name of a layout entry, empty if none.
std::string EntryClass(uintptr_t system, uint32_t group, uint32_t index) {
    uint32_t groups = 0, entries = 0;
    uintptr_t array = 0, layout = 0, entryArray = 0, entry = 0, text = 0;
    if (!mem::Read(system + 0x60, groups) || group >= groups || !mem::Read(system + 0x6C, array) || !array ||
        !mem::Read(array + 4 * group, layout) || !layout || !mem::Read(layout + 0x64, entries) || index >= entries ||
        !mem::Read(layout + 0x70, entryArray) || !entryArray || !mem::Read(entryArray + 4 * index, entry) || !entry ||
        !mem::Read(entry + 0xC, text) || !text)
        return {};
    char name[32]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(text + 8), name, sizeof(name) - 1)) return {};
    return name;
}

uint32_t EntryCount(uintptr_t system, uint32_t group) {
    uintptr_t array = 0, layout = 0;
    uint32_t entries = 0;
    return mem::Read(system + 0x6C, array) && array && mem::Read(array + 4 * group, layout) && layout &&
                   mem::Read(layout + 0x64, entries) && entries < 1000
               ? entries
               : 0;
}

uint32_t GroupCount(uintptr_t system) {
    uint32_t groups = 0;
    return mem::Read(system + 0x60, groups) && groups < 256 ? groups : 0;
}

bool UnitWithIds(uint32_t a, uint32_t b) {
    for (uintptr_t e : actors::Enemies()) {
        uint32_t ea = 0, eb = 0;
        if (mem::Read(e + 0xD50, ea) && mem::Read(e + 0xD54, eb) && ea == a && eb == b) return true;
    }
    return false;
}

void LearnTypes() {
    std::map<std::string, LevelType> types;
    const uintptr_t system = actors::InGame() ? EnemySystem() : 0;
    if (system) {
        const uint32_t groups = GroupCount(system);
        for (uint32_t g = 0; g < groups; ++g) {
            const uint32_t entries = EntryCount(system, g);
            for (uint32_t i = 0; i < entries; ++i) {
                const std::string cls = EntryClass(system, g, i);
                if (!cls.empty()) ++types[cls].entries;
            }
        }
        for (uintptr_t e : actors::Enemies()) {
            uint32_t a = 0, b = 0;
            int16_t health = 0;
            if (!mem::Read(e + 0xD50, a) || !mem::Read(e + 0xD54, b) || a == 0xFFFFFFFFu) continue;
            const std::string cls = EntryClass(system, a, b);
            if (!cls.empty() && mem::Read(e + 0x1364, health) && health > 0) ++types[cls].alive;
        }
    }
    std::lock_guard<std::mutex> lock(g_typesMutex);
    g_types.swap(types);
}

// A point `distance` in front of player 1 and `side` to his right, and the facing towards him.
bool FrontOfPlayer(float distance, float side, float (&at)[3], float (&quat)[4]) {
    const uintptr_t p1 = actors::Player(0);
    float pos[3]{}, q[4]{};
    if (!p1 || !mem::SafeRead(reinterpret_cast<void*>(p1 + 0x30), pos, sizeof(pos)) ||
        !mem::SafeRead(reinterpret_cast<void*>(p1 + 0x40), q, sizeof(q)))
        return false;
    const float yaw = 2.0f * std::atan2(q[1], q[3]);
    at[0] = pos[0] + distance * std::sin(yaw) - side * std::cos(yaw);
    at[1] = pos[1];
    at[2] = pos[2] + distance * std::cos(yaw) + side * std::sin(yaw);
    const float back = yaw + 3.14159265f;  // facing player 1
    quat[0] = 0.0f;
    quat[1] = std::sin(back * 0.5f);
    quat[2] = 0.0f;
    quat[3] = std::cos(back * 0.5f);
    return true;
}

// "Ramener" (user: spawned Executioners lost out of sight): the live units of a class in front of player 1, side by
// side (render thread).
std::mutex g_bringMutex;
std::string g_bringClass;

void BringRequested() {
    std::string cls;
    {
        std::lock_guard<std::mutex> lock(g_bringMutex);
        cls.swap(g_bringClass);
    }
    const uintptr_t system = cls.empty() || !actors::InGame() ? 0 : EnemySystem();
    if (!system) return;
    int k = 0;
    for (uintptr_t e : actors::Enemies()) {
        uint32_t a = 0, b = 0;
        int16_t health = 0;
        if (!mem::Read(e + 0xD50, a) || !mem::Read(e + 0xD54, b) || a == 0xFFFFFFFFu || EntryClass(system, a, b) != cls ||
            !mem::Read(e + 0x1364, health) || health <= 0)
            continue;
        float at[3]{}, quat[4]{};
        const float side = 150.0f * static_cast<float>((k + 1) / 2) * (k % 2 ? 1.0f : -1.0f);  // 0, +1.5 m, -1.5 m...
        if (!FrontOfPlayer(kFrontDistance, side, at, quat)) return;
        mem::SafeWrite(reinterpret_cast<void*>(e + 0x30), at, sizeof(at));
        mem::SafeWrite(reinterpret_cast<void*>(e + 0x40), quat, sizeof(quat));
        ++k;
    }
    LOG("enemy spawn: %d %s brought in front of player 1", k, cls.c_str());
}

// The new unit in front of player 1, facing him, while it settles.
void PlaceSpawned() {
    const uintptr_t unit = g_spawned.load(std::memory_order_relaxed);
    if (!unit) return;
    if (GetTickCount() - g_spawnedTick.load() > kPlaceMs || !g_inFront) {
        g_spawned = 0;
        return;
    }
    uintptr_t vtable = 0;
    float at[3]{}, quat[4]{};
    if (!mem::Read(unit, vtable) || !vtable || !FrontOfPlayer(kFrontDistance, 0.0f, at, quat)) return;
    mem::SafeWrite(reinterpret_cast<void*>(unit + 0x30), at, sizeof(at));
    mem::SafeWrite(reinterpret_cast<void*>(unit + 0x40), quat, sizeof(quat));
}

const char* DisplayName(const std::string& cls) {
    static const std::map<std::string, const char*> kNames = {
        { "uEm10", "Majini" }, { "uEm11", "Majini (variante)" }, { "uEm12", "Majini (village)" },
        { "uEm13", "Gros Majini" }, { "uEm20", "Bourreau" }, { "uEm22", "Majini tronçonneuse" },
        { "uEm23", "Majini Gatling" }, { "uEm24", "Majini géant" }, { "uEm30", "Adjule (chien)" },
        { "uEm32", "Licker" }, { "uEm34", "Reaper" }, { "uEm62", "Poulet" }, { "uEm80", "Wesker" }, { "uEm83", "Jill" },
    };
    const auto it = kNames.find(cls);
    return it == kNames.end() ? nullptr : it->second;
}

void DrawSpawnTable() {
    ImGui::TextUnformatted(i18n::Tr("Faire apparaître un ennemi du niveau (les types prévus par le niveau en cours) :"));
    ImGui::Checkbox(i18n::Tr("Devant Chris (3 m)"), &g_inFront);
    std::map<std::string, LevelType> types;
    {
        std::lock_guard<std::mutex> lock(g_typesMutex);
        types = g_types;
    }
    if (types.empty()) {
        ImGui::TextDisabled(i18n::Tr("Aucun ennemi prévu (pas de niveau chargé)."));
        return;
    }
    const bool canSpawn = actors::InGame() && !g_requested.load();
    if (ImGui::BeginTable("enemy_spawn", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(i18n::Tr("Ennemi"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(i18n::Tr("Classe"));
        ImGui::TableSetupColumn(i18n::Tr("Entrées"));
        ImGui::TableSetupColumn(i18n::Tr("En vie"));
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const auto& [cls, type] : types) {
            ImGui::PushID(cls.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const char* name = DisplayName(cls);
            ImGui::TextUnformatted(name ? name : cls.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(cls.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", type.entries);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", type.alive);
            ImGui::TableSetColumnIndex(4);
            ImGui::BeginDisabled(!canSpawn);
            if (ImGui::SmallButton(i18n::Tr("Faire apparaître"))) {
                if (EnsurePadQueryHooks()) {
                    {
                        std::lock_guard<std::mutex> lock(g_requestMutex);
                        g_requestClass = cls;
                    }
                    g_requested = true;
                } else {
                    Toast("Apparition impossible : crochet du jeu absent", false);
                }
            }
            ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(5);
            ImGui::BeginDisabled(!actors::InGame() || type.alive == 0);
            if (ImGui::SmallButton(i18n::Tr("Ramener"))) {
                std::lock_guard<std::mutex> lock(g_bringMutex);
                g_bringClass = cls;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(i18n::Tr("Met les ennemis vivants de ce type devant Chris."));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled(i18n::Tr("Une entrée libre du niveau est utilisée (une par ennemi vivant). En Mercenaries chaque "
                        "apparition compte dans les ennemis de la manche."));
}

void DrawEnemyPanel() {
    DrawOptions("Ennemis");
    ImGui::Separator();
    DrawSpawnTable();
}

// Game thread: the game's own refusal of an entry (sEnemy vfunc +0x44, thiscall(a, b) ret 8).
bool EntryRefused(uintptr_t system, uint32_t a, uint32_t b) {
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(system, vtable) || !mem::Read(vtable + 0x44, fn) || !fn) return true;
    __try {
        return reinterpret_cast<bool(__fastcall*)(void*, void*, uint32_t, uint32_t)>(fn)(reinterpret_cast<void*>(system),
                                                                                          nullptr, a, b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

uintptr_t SetEnemy(uintptr_t system, uint32_t a, uint32_t b) {
    using SetEnemyFn = uintptr_t(__fastcall*)(void* self, void* edx, uint32_t a, uint32_t b, uint32_t flag);
    __try {
        return reinterpret_cast<SetEnemyFn>(mem::Rva(0x3AF6E0))(reinterpret_cast<void*>(system), nullptr, a, b, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
}  // namespace

// Game thread (quick_knife.cpp's pad query hook, every frame): the requested spawn on a free entry of its class.
void EnemySpawnTick() {
    if (!g_requested.load(std::memory_order_relaxed)) return;
    g_requested = false;
    std::string cls;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        cls = g_requestClass;
    }
    const uintptr_t system = actors::InGame() ? EnemySystem() : 0;
    if (!system) return;
    const uint32_t groups = GroupCount(system);
    for (uint32_t a = 0; a < groups; ++a) {
        const uint32_t entries = EntryCount(system, a);
        for (uint32_t b = 0; b < entries; ++b) {
            if (EntryClass(system, a, b) != cls || UnitWithIds(a, b) || EntryRefused(system, a, b)) continue;
            const uintptr_t unit = SetEnemy(system, a, b);
            LOG("enemy spawn: %s on entry (%X, %X) -> %08X", cls.c_str(), a, b, static_cast<unsigned>(unit));
            if (!unit) {
                Toast(i18n::Tr("Apparition refusée par le jeu : ") + cls, false);
                return;
            }
            const DWORD now = GetTickCount();
            g_spawnedTick = now ? now : 1;
            g_spawned = unit;
            return;
        }
    }
    LOG("enemy spawn: no free entry for %s", cls.c_str());
    Toast(i18n::Tr("Aucune entrée libre pour ") + cls + i18n::Tr(" (toutes en vie)"), false);
}

void RegisterEnemySpawn() {
    OnFrame([] {
        static DWORD last = 0;
        if (GetTickCount() - last >= 500) {  // the layout scan twice a second
            last = GetTickCount();
            LearnTypes();
        }
        BringRequested();
        PlaceSpawned();
    });
    AddPanel("Ennemis", DrawEnemyPanel, { "Ennemis" });
}
}  // namespace features
