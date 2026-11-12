// Inventaire : objets ajoutés par le jeu lui-même (comme un ramassage), noms des objets du jeu.
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <imgui.h>
#include <MinHook.h>
#include "ui/sliders.hpp"

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <utility>
#include <string>
#include <vector>

namespace features {
uint32_t ActorCharacterInfo(void* actor);  // swaps.cpp: (character << 8) | costume
bool EnsurePlayerRequestHook();            // action_log.cpp: the players' RequestAction (knife slot "Rien")
bool HoldsInventoryKnife(uintptr_t player); // below: a knife from the inventory in hand
uint32_t PlayerHeldGrid(uintptr_t player);  // below: the grid of the item in hand
bool KnifeSlotEmpty(int playerIndex);       // below: "Rien" (no second knife in the sheath)
bool FirstPersonHidesSheath(uintptr_t player);  // camera.cpp: player 1 seen in the first person
bool KnifeUnusable(int playerIndex);        // below: no knife to draw ("Rien" or a broken sheath knife)
bool SheathKnifeBroken(int playerIndex);    // knife_icon.cpp: "Durabilité du couteau"
bool Player1InventoryOpen();                // pickup.cpp
bool EnsurePadQueryHooks();                 // quick_knife.cpp: InventoryDiscardTick on the game thread (coop_solo.cpp)
uint16_t SheathPlaceItem(int playerIndex, int place);  // below: a place's item (0 the main one), 0 empty
bool SetSheathPlace(int playerIndex, int place, uint16_t id, bool activate);  // below: a place 1..'s weapon
void TickSheathCheckpoint();                // below: the sheath follows the checkpoints
bool OrganiserOpen();                       // knife_icon.cpp: the Organiser screen shown
bool KnifeStoryMode();                      // knife_icon.cpp: not the Mercenaries
bool SheathFileApplied();                   // knife_icon.cpp: the places at the last quit given back (or none)
void KnifeUnitsSnapshot(float (&places)[2][5], float (&inv)[2][9]);              // knife_icon.cpp
void KnifeUnitsRestore(int i, const float (&places)[2][5], const float (&inv)[2][9]);  // knife_icon.cpp
int KnifeSlotNoneChoice();                  // below: the index of "Rien"

namespace {

struct ItemName {
    uint16_t id;
    const char* category;
    const char* name;
};
// Item id -> French name, from the game's mes_item_f (tools/gen_item_table.py); only ids the game has a class for.
const ItemName kItems[] = {
#include "features/item_names.inc"
};
// Items the message file does not name (gen_item_table.py skips them): Barry's Samurai Edge, a Mercenaries Reunion
// weapon (uWp12, "(dummy_wp12)"; user 2026-10-27: to give it in the story too). Wesker's (0x0129) is named.
const ItemName kExtraItems[] = { { 0x0112, "Armes", "Samurai Edge (Barry)" } };

// Weapons listed by kind (user, 2026-10-27: Wesker's Samurai Edge was far from the other handguns): melee, handguns,
// machine guns and rifles, shotguns, magnums, sniper rifles, launchers and the rest, thrown ones.
int WeaponGroup(uint16_t id) {
    switch (id) {
    case 0x0100: case 0x0101: case 0x010E: case 0x0122: case 0x0123: case 0x0124: case 0x0137: return 0;
    case 0x0102: case 0x0110: case 0x0111: case 0x0112: case 0x011E: case 0x011F: case 0x0129: return 1;
    case 0x0103: case 0x0109: case 0x0113: case 0x011D: return 2;
    case 0x0104: case 0x0116: case 0x0117: case 0x0119: return 3;
    case 0x010B: case 0x011A: case 0x011B: return 4;
    case 0x0105: case 0x011C: case 0x0120: return 5;
    case 0x0106: case 0x0107: case 0x0108: case 0x010A: case 0x0136: case 0x013C: case 0x013D: case 0x013E: return 7;
    default: return 6;
    }
}

// The give list, the knife slot weapons and the stack table: both lists, by category (id >> 8), weapons by kind, then id.
const std::vector<ItemName>& AllItems() {
    static const std::vector<ItemName> all = [] {
        std::vector<ItemName> items(std::begin(kItems), std::end(kItems));
        items.insert(items.end(), std::begin(kExtraItems), std::end(kExtraItems));
        const auto key = [](const ItemName& item) {
            return std::make_pair(((item.id >> 8) << 4) | ((item.id >> 8) == 1 ? WeaponGroup(item.id) : 0), item.id);
        };
        std::stable_sort(items.begin(), items.end(), [&](const ItemName& a, const ItemName& b) { return key(a) < key(b); });
        return items;
    }();
    return all;
}

// Each player owns a cItemManager at P+0x21A0 (vtable RVA 0xC4281C) followed by 24 cItem of 0x30 bytes (vtable RVA
// 0xC42778): +0x04 id, +0x08 count, +0x0C max, +0x18 grid position (0-8, 9 knife, 0xB hand-to-hand, 0xD none),
// +0x25 rapid fire, +0x27 infinite ammo, +0x29 in use, +0x2C item unit (uWpXX / uItCCXX).
constexpr uintptr_t kItemManager = 0x21A0;
constexpr int kItemRecords = 24;

// cItemManager::AddItem (RVA 0x84E4F0, thiscall, 18 dword arguments, ret 0x48) is what a pickup calls (traced,
// 2026-10-13): it takes a free record (RVA 0x84B500), fills it with cItem::Set (RVA 0x84E290: -1 = the item's
// default count / max) and, given the owner actor and a grid position, creates and attaches the item unit
// (RVA 0x84C9B0 / 0x84CB70). Writing an id into a record does not create the unit: the weapon is not in hand.
using AddItemFn = void*(__fastcall*)(void* manager, void* edx, uint32_t id, int32_t count, int32_t max, int32_t a3,
                                     int32_t a4, uint32_t position, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t level,
                                     uint32_t a10, uint32_t rapid, uint32_t a12, uint32_t infinite, uint32_t a14,
                                     uint32_t owner, uint32_t a16, uint32_t unit);

void* CallAddItem(uintptr_t player, uint16_t id, int count, uint32_t position) {
    auto add = reinterpret_cast<AddItemFn>(mem::Rva(0x84E4F0));
    __try {
        return add(reinterpret_cast<void*>(player + kItemManager), nullptr, id, count, -1, -1, -1, position, 0, 0, 0, 0,
                   0, 0, 0, 0, 0, static_cast<uint32_t>(player), 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool HasItemManager(uintptr_t player) {
    uintptr_t vtable = 0;
    return player && mem::Read(player + kItemManager, vtable) && vtable == reinterpret_cast<uintptr_t>(mem::Rva(0xC4281C));
}

int FreeGridPosition(uintptr_t player) {
    bool used[9] = {};
    for (int k = 0; k < kItemRecords; ++k) {
        const uintptr_t item = player + kItemManager + 4 + 0x30 * k;
        uint8_t inUse = 0;
        uint32_t position = 0xD;
        if (mem::Read(item + 0x29, inUse) && inUse && mem::Read(item + 0x18, position) && position < 9) used[position] = true;
    }
    for (int i = 0; i < 9; ++i)
        if (!used[i]) return i;
    return -1;
}

// Weapons the inventory cannot keep (taken from the world and dropped when switching): left out of the give list.
bool Storable(uint16_t id) {
    switch (id) {
    case 0x0100:  // hand-to-hand
    case 0x012F:  // turret
    case 0x0130:  // searchlight
    case 0x0134:  // laser targeting device (LTD): removed from the inventory at load
    case 0x0137:  // hand-to-hand
        return false;
    default:
        return true;
    }
}

// Item defaults (what cItem::Set takes for -1): table [RVA 0xCEACF0 + category * 4] + (id & 0xFF) * 0x48,
// +8 count, +0xC max.
bool ItemDefaults(uint16_t id, int& count, int& max) {
    uintptr_t table = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xCEACF0)) + 4 * (id >> 8), table) || !table) return false;
    const uintptr_t info = table + (id & 0xFF) * 0x48;
    return mem::Read(info + 8, count) && mem::Read(info + 0xC, max);
}

// A weapon can be handed over live only if the level already loaded its resources: the unit the game creates for
// an unloaded weapon has no model (+0xA80 = 0) and Chris freezes in a grenade pose (user, 2026-10-13). Loaded =
// some player already holds a unit of that weapon with a model.
bool WeaponLoaded(uint16_t id) {
    for (int i = 0; i < 2; ++i) {
        const uintptr_t player = actors::Player(i);
        if (!HasItemManager(player)) continue;
        for (int k = 0; k < kItemRecords; ++k) {
            const uintptr_t item = player + kItemManager + 4 + 0x30 * k;
            uint32_t itemId = 0;
            uintptr_t unit = 0, model = 0;
            uint8_t inUse = 0;
            if (mem::Read(item + 0x29, inUse) && inUse && mem::Read(item + 4, itemId) && (itemId & 0xFFFF) == id &&
                mem::Read(item + 0x2C, unit) && unit && mem::Read(unit + 0xA80, model) && model)
                return true;
        }
    }
    return false;
}

// The game rebuilds the inventory from saved copies when a checkpoint is restarted (and creates every unit with
// its resources then). Seven copies, 24 entries of 0x2C bytes per player (= cItem +0x04..+0x2F, so +0x14
// position, +0x25 in use), entry k = grid position k: profile [RVA 0xDB2158] +0x23450 / +0x249E0 / +0x27620,
// sLoadMan [RVA 0xDB26C4] +0x3DA4, sBioStorage [RVA 0xDB1DD8] +0x3E90, and the restart buffer [sLoadMan+0x20768]
// +0x14030 / +0x155C0 (reallocated by every restart: the source the restart reads). Writing only the first five
// was not enough (user test, 2026-10-13).
std::vector<uintptr_t> InventoryCopies(int player) {
    auto global = [](uintptr_t rva) {
        uintptr_t v = 0;
        mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(rva)), v);
        return v;
    };
    const uintptr_t profile = global(0xDB2158), loadMan = global(0xDB26C4), storage = global(0xDB1DD8);
    uintptr_t restart = 0;
    if (loadMan) mem::Read(loadMan + 0x20768, restart);
    const std::pair<uintptr_t, uintptr_t> places[] = { { profile, 0x23450 }, { profile, 0x249E0 }, { profile, 0x27620 },
                                                        { loadMan, 0x3DA4 },  { storage, 0x3E90 },  { restart, 0x14030 },
                                                        { restart, 0x155C0 } };
    std::vector<uintptr_t> copies;
    for (const auto& [object, offset] : places) {
        if (!object) continue;
        const uintptr_t base = object + offset + 0x2C * kItemRecords * player;
        bool valid = true;  // every entry holds its own grid position
        for (int k = 0; k < 12 && valid; ++k) {
            uint32_t position = 0xFF;
            valid = mem::Read(base + 0x2C * k + 0x14, position) && position == static_cast<uint32_t>(k);
        }
        if (valid) copies.push_back(base);
    }
    return copies;
}

// Each weapon has its own archive nativePC_MT\Image\Archive\uWpXX.arc: model, weapon motions and the players'
// motions for it (pawn\pl\pl00\motion\pl00wpXX, pl01...). Loading it through sResource::Load (RVA 0x48B70,
// thiscall, ret 0xC; type rArchive RVA 0xE4B604, flags 1 = load now; state +0x4C bit 0 loaded, bit 4 failed)
// gives the level what it needs to create the weapon live. The reference is kept until the trainer unloads.
using ResLoadFn = void*(__fastcall*)(void* mgr, void* edx, void* type, const char* path, unsigned flags);
using ResReleaseFn = void(__fastcall*)(void* res, void* edx);
std::vector<std::pair<std::string, void*>> g_archives;  // archive name -> resource held by the trainer

bool LoadArchive(const char* name) {
    for (const auto& archive : g_archives)
        if (archive.first == name) return true;
    void* mgr = nullptr;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE34580)), mgr) || !mgr) return false;
    char path[64];
    std::snprintf(path, sizeof(path), "Image\\Archive\\%s", name);
    auto load = reinterpret_cast<ResLoadFn>(mem::Rva(0x48B70));
    void* res = nullptr;
    __try {
        res = load(mgr, nullptr, mem::Rva(0xE4B604), path, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        res = nullptr;
    }
    uint32_t state = 0;
    if (!res || !mem::Read(reinterpret_cast<uintptr_t>(res) + 0x4C, state) || !(state & 1) || (state & 0x10)) {
        if (res) reinterpret_cast<ResReleaseFn>(mem::Rva(0x4F3E0))(res, nullptr);
        LOG("archive %s could not be loaded (state %08X)", path, state);
        return false;
    }
    g_archives.emplace_back(name, res);
    LOG("archive %s loaded (%p)", path, res);
    return true;
}

// What the level loader adds for an item besides its own archive (RVA 0x3D5A27..0x3D6198, decoded from the running
// game): the weapon's ammo item (weapon parameters RVA 0xDB5170 + low*0x140, +0x1C) and the shells it fires (switch
// on the item id). Without its shell archive a weapon given live fired nothing: the bow's arrows (uWp45) left no
// arrow and hurt nobody until a reload (user, 2026-10-14). Grenade launchers -> rounds -> shells (two levels).
// Type 6 entries (uOm cartridges, shared by every gun and loaded with the level) are left out.
struct ItemDependencies {
    uint16_t id;
    uint16_t deps[2];
};
constexpr ItemDependencies kItemDependencies[] = {
    { 0x0102, { 0x0201 } }, { 0x0103, { 0x0202 } }, { 0x0104, { 0x0203 } }, { 0x0105, { 0x0204 } },
    { 0x0109, { 0x0202 } }, { 0x010B, { 0x0209 } }, { 0x010D, { 0x0144 } }, { 0x010F, { 0x0145 } },
    { 0x0110, { 0x0201 } }, { 0x0111, { 0x0201 } }, { 0x0112, { 0x0201 } }, { 0x0113, { 0x0202 } },
    { 0x0115, { 0x0202, 0x0146 } }, { 0x0116, { 0x0203 } }, { 0x0117, { 0x0203 } }, { 0x0118, { 0x0201 } },
    { 0x0119, { 0x0203 } }, { 0x011A, { 0x0209 } }, { 0x011B, { 0x0209 } }, { 0x011C, { 0x0204 } },
    { 0x011D, { 0x0202 } }, { 0x011E, { 0x0201 } }, { 0x011F, { 0x0201 } }, { 0x0120, { 0x0204 } },
    { 0x0125, { 0x0206 } }, { 0x0126, { 0x0207 } }, { 0x0127, { 0x0208 } }, { 0x0128, { 0x0201 } },
    { 0x0129, { 0x0201 } }, { 0x012A, { 0x0202 } }, { 0x012C, { 0x014A } }, { 0x012F, { 0x0149, 0x0185 } },
    { 0x0131, { 0x014B } }, { 0x0134, { 0x013F } }, { 0x0135, { 0x014C } }, { 0x0138, { 0x0144 } },
    { 0x0139, { 0x020E } }, { 0x013A, { 0x020F } }, { 0x013B, { 0x0210 } }, { 0x0152, { 0x0181 } },
    { 0x0156, { 0x0203, 0x0187 } }, { 0x015D, { 0x0202, 0x0184 } }, { 0x0160, { 0x015F } }, { 0x0161, { 0x0165 } },
    { 0x0162, { 0x0180 } }, { 0x0163, { 0x016F } }, { 0x0167, { 0x0201 } }, { 0x0168, { 0x0183 } },
    { 0x016A, { 0x0202, 0x0186 } }, { 0x016B, { 0x0204 } },
    { 0x0206, { 0x0140 } }, { 0x0207, { 0x0141 } }, { 0x0208, { 0x0142 } }, { 0x020E, { 0x014D } },
    { 0x020F, { 0x014E } }, { 0x0210, { 0x014F } },
};

void ArchiveName(uint16_t id, char (&name)[16]) {
    if ((id >> 8) == 1) std::snprintf(name, sizeof(name), "uWp%02x", id & 0xFF);
    else std::snprintf(name, sizeof(name), "uIt%04x", id);
}

void LoadDependencies(uint16_t id, int depth) {
    for (const ItemDependencies& entry : kItemDependencies) {
        if (entry.id != id) continue;
        for (uint16_t dep : entry.deps) {
            if (!dep) continue;
            char name[16];
            ArchiveName(dep, name);
            LoadArchive(name);
            if (depth < 1) LoadDependencies(dep, depth + 1);
        }
    }
}

// Everything an item needs to be created and used live: its own archive (weapons not already in an inventory) and
// its dependencies. False only when the weapon's own archive could not be loaded.
bool LoadItemResources(uint16_t id) {
    if ((id >> 8) == 1 && !WeaponLoaded(id)) {
        char name[16];
        ArchiveName(id, name);
        if (!LoadArchive(name)) return false;
    }
    LoadDependencies(id, 0);
    return true;
}

bool GiveAtRestart(int playerIndex, uint16_t id, int count) {
    const std::vector<uintptr_t> copies = InventoryCopies(playerIndex);
    if (copies.size() != 7) {
        Toast("Copies de l'inventaire introuvables : objet non donné", false);
        return false;
    }
    int position = -1;
    for (int k = 0; k < 9 && position < 0; ++k) {
        uint32_t itemId = 1;
        if (mem::Read(copies[0] + 0x2C * k, itemId) && (itemId & 0xFFFF) == 0) position = k;
    }
    if (position < 0) {
        Toast("Inventaire du point de passage plein : libère une case", false);
        return false;
    }
    int defCount = 1, defMax = 1;
    ItemDefaults(id, defCount, defMax);
    uint8_t entry[0x2C] = {};
    const uint32_t fields[3] = { id, static_cast<uint32_t>(count > 0 ? count : defCount), static_cast<uint32_t>(defMax) };
    std::memcpy(entry, fields, sizeof(fields));
    const uint32_t pos = static_cast<uint32_t>(position);
    std::memcpy(entry + 0x14, &pos, 4);
    entry[0x25] = 1;
    for (uintptr_t base : copies) mem::SafeWrite(reinterpret_cast<void*>(base + 0x2C * position), entry, sizeof(entry));
    LOG("give item %04X x%d to P%d at restart, grid %d", id, count, playerIndex + 1, position);
    Toast("Objet ajouté au point de passage : Pause > Redémarrer pour le recevoir");
    return true;
}

bool GiveItem(int playerIndex, uint16_t id, int count) {
    const uintptr_t player = actors::Player(playerIndex);
    if (!id || !HasItemManager(player)) return false;
    if (!LoadItemResources(id)) return GiveAtRestart(playerIndex, id, count);
    const int position = FreeGridPosition(player);
    if (position < 0) {
        Toast("Inventaire plein : libère une case", false);
        return false;
    }
    void* item = CallAddItem(player, id, count > 0 ? count : -1, static_cast<uint32_t>(position));
    LOG("give item %04X x%d to P%d at grid %d -> %p", id, count, playerIndex + 1, position, item);
    return item != nullptr;
}

std::vector<uint16_t> g_giveIds;  // choice index - 1 -> item id
std::vector<std::string> g_giveChoices;  // "—" then "Category : name"

// Removing an item the way the game empties an inventory (RVA 0x84D390): the unit is checked by the unit manager
// [RVA 0xDB2200] (RVA 0x3BEB50, thiscall(manager, unit) -> bool) and destroyed by its vtable slot +0x30, then the
// record is zeroed (+0x04..+0x2F) with grid position 0xD. Never the weapon in hand ([P+0x2634]).
using UnitValidFn = bool(__fastcall*)(void* manager, void* edx, void* unit);
using UnitKillFn = void(__fastcall*)(void* manager, void* edx, void* unit);

bool KillUnit(void* manager, void* unit) {
    __try {
        if (reinterpret_cast<UnitValidFn>(mem::Rva(0x3BEB50))(manager, nullptr, unit)) {
            uintptr_t vtable = 0, kill = 0;
            if (!mem::Read(reinterpret_cast<uintptr_t>(manager), vtable) || !mem::Read(vtable + 0x30, kill) || !kill) return false;
            reinterpret_cast<UnitKillFn>(kill)(manager, nullptr, unit);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RemoveItem(uintptr_t player, int record) {
    const uintptr_t item = player + kItemManager + 4 + 0x30 * record;
    uintptr_t unit = 0, held = 0, manager = 0;
    mem::Read(item + 0x2C, unit);
    mem::Read(player + 0x2634, held);
    if (unit && unit == held) {
        Toast("Change d'arme avant de retirer celle que tu tiens", false);
        return false;
    }
    mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2200)), manager);
    if (unit && (!manager || !KillUnit(reinterpret_cast<void*>(manager), reinterpret_cast<void*>(unit)))) return false;
    uint8_t zero[0x2C] = {};
    mem::SafeWrite(reinterpret_cast<void*>(item + 4), zero, sizeof(zero));
    mem::Write<uint32_t>(item + 0x18, 0xD);
    // P+0x2638 can still point at the destroyed unit (a weapon of the knife slot from the last load: the knife put
    // back afterwards stayed on the back when drawn, user 2026-10-13). Never leave it dangling.
    uintptr_t second = 0;
    if (unit && mem::Read(player + 0x2638, second) && second == unit) mem::Write<uintptr_t>(player + 0x2638, 0);
    LOG("removed item record %d of %08X", record, static_cast<unsigned>(player));
    return true;
}

// Removes the first record holding this item (grid cases only, never the weapon in hand).
bool RemoveItemById(int playerIndex, uint16_t id) {
    const uintptr_t player = actors::Player(playerIndex);
    if (!id || !HasItemManager(player)) return false;
    for (int k = 0; k < kItemRecords; ++k) {
        const uintptr_t item = player + kItemManager + 4 + 0x30 * k;
        uint8_t inUse = 0;
        uint32_t itemId = 0, position = 0xD;
        if (mem::Read(item + 0x29, inUse) && inUse && mem::Read(item + 4, itemId) && (itemId & 0xFFFF) == id &&
            mem::Read(item + 0x18, position) && position < 9)
            return RemoveItem(player, k);
    }
    Toast("Cet objet n'est pas dans l'inventaire", false);
    return false;
}

// ---- Knife slot (grid position 9, its own quick key) holding another weapon (Ultimate Trainer: e.g. the stun rod
// without using a case, or a pistol with infinite ammo). The knife record is removed and the weapon added by the
// game at position 9. A restart rebuilds the inventory with the knife (the new player object often has the same
// address as the old one): whenever the slot has held something else than the choice for 3 s, the choice is applied
// again. "Par défaut" puts back the weapon that was there first.
std::vector<uint16_t> g_knifeIds;  // choice index -> weapon id (0: "Par défaut", "Rien")
int g_noneChoice = 0;              // the index of "Rien"

uint16_t ChoiceWeapon(int choice) {
    return choice > 0 && choice < static_cast<int>(g_knifeIds.size()) ? g_knifeIds[choice] : 0;
}

int ChoiceIndexOf(uint16_t id) {
    for (int k = 1; id && k < static_cast<int>(g_knifeIds.size()); ++k)
        if (g_knifeIds[k] == id) return k;
    return -1;
}

// Weapons offered for the knife slot (user's choice, 2026-10-13): knives, stun rod, handguns, magnums and the Hydra.
// Two-handed weapons were left out (their endless ammunition there was not credible) and single-use ones (grenades,
// mines, eggs, rocket launchers) left the player stuck once used up; the sheath's guns now run dry (no shot when
// empty) and a used-up single-use weapon empties its place (SingleUseUsedUp): both allowed again (user, 2026-11-05).
// The second knife models 0x0123 / 0x0124 (Sheva's stabs instead of slashing) have their sheath sunk into Chris's
// back: moved by sliders. FirstKnifeSlotWeapon is the list of 2026-10-13, whose menu indices are saved.
bool FirstKnifeSlotWeapon(uint16_t id) {
    switch (id) {
    case 0x0101: case 0x010E: case 0x0123: case 0x0124:                          // knives
    case 0x0122:                                                                  // stun rod
    case 0x0102: case 0x0110: case 0x0111: case 0x011E: case 0x011F: case 0x0129:  // handguns
    case 0x0112:                                                                  // Barry's Samurai Edge
    case 0x010B: case 0x011A: case 0x011B:                                        // magnums
    case 0x0119:                                                                  // Hydra
        return true;
    default:
        return false;
    }
}

// Thrown and single-shot weapons: grenades, the mine, the eggs, the rocket launchers.
bool SingleUse(uint16_t id) {
    switch (id) {
    case 0x0106: case 0x0107: case 0x0108: case 0x010A: case 0x0136: case 0x013C: case 0x013D: case 0x013E:
    case 0x010D: case 0x0135:
        return true;
    default:
        return false;
    }
}

// A firearm's magazine at its first capacity level (VA 0x7BBBA0 cdecl(id, level): the table at VA 0x11C06A0, 0x7A
// levels by weapon), what the Organiser's cells call their maximum. The item table's count (ItemDefaults) is what a
// weapon found on the ground holds: 5 for the Ithaca whose magazine is 6, 4 for the M3 whose magazine is 5 (user,
// 2026-11-05: "6 in the inventory, 5 +1 in the sheath; a pump shotgun has no +1").
int BaseMagazine(uint16_t id) {
    using MagazineFn = int(__cdecl*)(int id, int level);
    __try {
        return reinterpret_cast<MagazineFn>(mem::Rva(0x3BBBA0))(id, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// A weapon of the sheath full: a firearm's magazine, a thrown one's (or a knife's) default count.
int FullCount(uint16_t id) {
    int full = 1, max = 1;
    ItemDefaults(id, full, max);
    if ((id >> 8) != 1 || SingleUse(id)) return full;
    const int magazine = BaseMagazine(id);
    return magazine > 0 ? magazine : full;
}

// The capacity a record's weapon shows (the game's getter, the trainer's magazine factors included).
int RecordMagazine(uintptr_t player, uintptr_t record) {
    using GetFn = int(__fastcall*)(void* manager, void* edx, uintptr_t record);
    __try {
        return reinterpret_cast<GetFn>(mem::Rva(0x84C050))(reinterpret_cast<void*>(player + kItemManager), nullptr, record);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool KnifeSlotWeapon(uint16_t id) {
    if (FirstKnifeSlotWeapon(id) || SingleUse(id)) return true;
    switch (id) {
    case 0x0103: case 0x0109: case 0x0113: case 0x011D:                          // machine guns, rifles
    case 0x0104: case 0x0116: case 0x0117:                                        // shotguns
    case 0x0105: case 0x011C: case 0x0120:                                        // sniper rifles
    case 0x010C: case 0x0125: case 0x0126: case 0x0127: case 0x0139: case 0x013A: case 0x013B:  // grenade launchers
    case 0x010F: case 0x0115: case 0x0121:                                        // bow, Gatling, flamethrower
        return true;
    default:
        return false;
    }
}

bool g_swapChoosing = false;  // the main place's choice set by a swap with the inventory (its rounds kept)

struct KnifeSlot {
    int choice = 0;
    bool chosen = false;  // the choice already set once (the first set is the settings' restore)
    ULONGLONG mismatchSince = 0;
    bool infinite = false;
    // The sheath's knife changed while the slot's was in hand (a second knife broken by a hit, user 2026-10-28: the
    // broken second stayed in the slot as the main one, "Par défaut" taking any knife): exactly that item until it is in.
    bool sheathPending = false;
} g_knife[2];

int KnifeRecord(uintptr_t player) {
    for (int k = 0; k < kItemRecords; ++k) {
        const uintptr_t item = player + kItemManager + 4 + 0x30 * k;
        uint8_t inUse = 0;
        uint32_t position = 0xD;
        if (mem::Read(item + 0x29, inUse) && inUse && mem::Read(item + 0x18, position) && position == 9) return k;
    }
    return -1;
}

// A firearm out of the knife slot keeps its rounds for its return (user, 2026-11-05: the sheath switched to the second
// knife and back gave the pistol a full magazine; the slot's item is removed and added anew): by player and item, the
// sheath's main gun and a second one (also moved in from the inventory with its rounds) each their own. Kept in
// DataDir\sheath_guns.ini ("p1=0102,7"): a trainer reload or a game restart gave back full magazines.
std::map<uint16_t, int32_t> g_slotGunRounds[2];

bool IsSheathFirearm(uint16_t id) {
    return id != 0x0101 && id != 0x010E && id != 0x0123 && id != 0x0124 && id != 0x0122;
}

std::wstring SlotGunFile() { return logx::DataDir() + L"\\sheath_guns.ini"; }

void LoadSlotGunRounds() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    std::ifstream f(SlotGunFile());
    std::string line;
    while (std::getline(f, line)) {
        int player = 0, count = 0;
        unsigned id = 0;
        if (sscanf_s(line.c_str(), "p%d=%x,%d", &player, &id, &count) == 3 && player >= 1 && player <= 2)
            g_slotGunRounds[player - 1][static_cast<uint16_t>(id)] = count;
    }
}

void SaveSlotGunRounds() {
    std::ofstream f(SlotGunFile(), std::ios::trunc);
    for (int i = 0; i < 2; ++i)
        for (const auto& [id, count] : g_slotGunRounds[i]) {
            char text[48];
            std::snprintf(text, sizeof(text), "p%d=%04X,%d\n", i + 1, id, count);
            f << text;
        }
}

// The rounds a firearm left the slot (or the inventory) with, for its record now at `record`; forgotten then.
void GiveBackRounds(int playerIndex, uint16_t id, uintptr_t record, const char* where) {
    LoadSlotGunRounds();
    auto& kept = g_slotGunRounds[playerIndex];
    const auto it = kept.find(id);
    if (!record || it == kept.end()) return;
    mem::Write<int32_t>(record + 8, it->second);
    LOG("%s of P%d: %04X back with its %d rounds", where, playerIndex + 1, id, it->second);
    kept.erase(it);
    SaveSlotGunRounds();
}

void ForgetRounds(int playerIndex, uint16_t id) {
    LoadSlotGunRounds();
    if (g_slotGunRounds[playerIndex].erase(id)) {
        SaveSlotGunRounds();
        LOG("knife slot of P%d: the rounds kept for %04X forgotten", playerIndex + 1, id);
    }
}

void KeepRounds(int playerIndex, uint16_t id, int32_t rounds) {
    LoadSlotGunRounds();
    g_slotGunRounds[playerIndex][id] = std::max<int32_t>(rounds, 0);
    SaveSlotGunRounds();
}

bool PutInKnifeSlot(int playerIndex, uint16_t id, bool quiet = false) {
    const uintptr_t player = actors::Player(playerIndex);
    if (!id || !HasItemManager(player) || playerIndex < 0 || playerIndex > 1) return false;
    const int record = KnifeRecord(player);
    if (record >= 0) {
        const uintptr_t item = player + kItemManager + 4 + 0x30 * record;
        uint32_t current = 0;
        mem::Read(item + 4, current);
        if ((current & 0xFFFF) == id) return true;
        uintptr_t unit = 0, held = 0;
        mem::Read(item + 0x2C, unit);
        mem::Read(player + 0x2634, held);
        if (unit && unit == held) {
            if (!quiet) Toast("Range l'arme de l'emplacement couteau avant de la changer", false);
            return false;
        }
        const uint16_t leaving = static_cast<uint16_t>(current & 0xFFFF);
        int32_t rounds = 0;
        if (IsSheathFirearm(leaving) && mem::Read(item + 8, rounds)) KeepRounds(playerIndex, leaving, rounds);
        if (!RemoveItem(player, record)) return false;
    }
    if (!LoadItemResources(id)) return false;
    void* item = CallAddItem(player, id, -1, 9);
    LOG("knife slot of P%d -> %04X (%p)", playerIndex + 1, id, item);
    if (item) {
        const int k = KnifeRecord(player);
        const uintptr_t added = k >= 0 ? player + kItemManager + 4 + 0x30 * k : 0;
        LoadSlotGunRounds();
        if (added && IsSheathFirearm(id) && !SingleUse(id) && !g_slotGunRounds[playerIndex].count(id)) {
            const int magazine = RecordMagazine(player, added);
            if (magazine > 0) mem::Write<int32_t>(added + 8, magazine);  // a new weapon: its magazine full
        }
        GiveBackRounds(playerIndex, id, added, "knife slot");
    }
    return item != nullptr;
}

// "Par défaut" = the character's own knife (Sheva 0x010E, everyone else Chris's 0x0101). Remembering "the weapon that
// was there" failed: after a trainer reload or an autosave the slot already held a replacement (the bow stayed).
uint16_t OwnKnife(uintptr_t player) {
    const uint32_t info = player ? ActorCharacterInfo(reinterpret_cast<void*>(player)) : 0xFFFFFFFF;
    return info != 0xFFFFFFFF && ((info >> 8) & 0xFFFF) == 1 ? 0x010E : 0x0101;
}

// The choice also goes into the checkpoint copies (entry 9 = knife slot), so a restart or an autosave already
// holds it: otherwise an autosave taken with the bow in the slot brings the bow back after every reload.
void KnifeIntoCopies(int playerIndex, uint16_t id) {
    // Only on the Organiser screen (its records become the next level's inventory there): during play the copies are the
    // last checkpoint's, which the sheath follows (TickSheathCheckpoint; user, 2026-11-05: the sheath's current weapon
    // written into them came back after a restart next to the one the checkpoint had).
    if (!OrganiserOpen()) return;
    int count = 1, max = 1;
    ItemDefaults(id, count, max);
    count = FullCount(id);
    // A firearm's kept rounds rather than a full magazine (user, 2026-11-05: a gun moved into the sheath on the Organiser
    // screen kept its rounds; the copies are what the next level's slot is made from).
    if (IsSheathFirearm(id) && KnifeSlotWeapon(id)) {
        LoadSlotGunRounds();
        const auto it = g_slotGunRounds[playerIndex].find(id);
        if (it != g_slotGunRounds[playerIndex].end()) count = it->second;
    }
    uint8_t entry[0x2C] = {};
    const uint32_t fields[3] = { id, static_cast<uint32_t>(count), static_cast<uint32_t>(max) };
    std::memcpy(entry, fields, sizeof(fields));
    const uint32_t position = 9;
    std::memcpy(entry + 0x14, &position, 4);
    entry[0x25] = 1;
    for (uintptr_t base : InventoryCopies(playerIndex)) mem::SafeWrite(reinterpret_cast<void*>(base + 0x2C * 9), entry, sizeof(entry));
}

// The knife slot's entry (9) of the first copy: its item and count (between levels, the Organiser screen: the sheath's
// weapon in use is there).
bool CopiesSlot(int playerIndex, uint16_t& id, int32_t& count) {
    const std::vector<uintptr_t> copies = InventoryCopies(playerIndex);
    uint32_t raw = 0;
    if (copies.empty() || !mem::Read(copies[0] + 0x2C * 9, raw) || !mem::Read(copies[0] + 0x2C * 9 + 4, count)) return false;
    id = static_cast<uint16_t>(raw);
    return id != 0;
}

// "Rien" (user, 2026-10-27: nothing in the slot, no knife drawn, no knife parry, the sheath looking empty): the last
// choice of the list, after the weapons (the saved indices of the others kept). The slot keeps the character's own knife
// as "Par défaut" (emptying it was fragile: the knife counts as the item in hand when no weapon is equipped, and a
// restart from emptied copies gave Chris Sheva's knife); its drawing actions are refused (action_log.cpp), the knife
// parry too (parry.cpp), and its unit is drawn at scale 0 (HideSheathedKnife).
bool NoKnifeChoice(int choice) { return choice > 0 && choice == g_noneChoice; }

// The sheath's places (user, 2026-10-27: a second knife moved in from the inventory; 2026-11-05: "a third sheath, the
// wheel click swaps with the highlighted one", generalised to five, their number chosen by a slider): place 0 is the
// main one (the menu's choice), places 1.. hold weapons moved in from the inventory (knife_icon.cpp keeps their
// durability, a firearm's rounds are kept here). The highlighted place is the one in the knife slot; an empty one
// highlighted is an empty sheath, as "Rien" (nothing drawn, the sheathed knife hidden, the character's own knife kept in
// the slot).
constexpr int kMaxSheathPlaces = 5;
std::atomic<int> g_sheathPlaceCount{ 3 };
std::atomic<uint16_t> g_sheathPlace[2][kMaxSheathPlaces]{};  // places 1..: their item, 0 empty ([0] unused)
std::atomic<int> g_sheathActive[2]{};

int PlaceCount() { return std::clamp(g_sheathPlaceCount.load(), 1, kMaxSheathPlaces); }

// The item of the highlighted place when it is one of the places 1.. (0: the main one or an empty place).
uint16_t ActiveSecond(int playerIndex) {
    const int active = g_sheathActive[playerIndex].load();
    return active > 0 && active < kMaxSheathPlaces ? g_sheathPlace[playerIndex][active].load() : 0;
}

bool ActivePlaceEmpty(int playerIndex) {
    const int active = g_sheathActive[playerIndex].load();
    return active > 0 && active < kMaxSheathPlaces && !g_sheathPlace[playerIndex][active].load();
}

// The item the slot gets: the highlighted place's weapon, the character's own knife for an empty place, else the
// choice's weapon, else the character's own knife.
uint16_t KnifeChoiceItem(int playerIndex, uintptr_t player) {
    const KnifeSlot& slot = g_knife[playerIndex];
    const uint16_t second = ActiveSecond(playerIndex);
    return second                          ? second
           : ActivePlaceEmpty(playerIndex) ? OwnKnife(player)
           : ChoiceWeapon(slot.choice)     ? ChoiceWeapon(slot.choice)
                                           : OwnKnife(player);
}

bool ApplyKnifeChoice(int playerIndex, bool quiet = false) {
    const uint16_t id = KnifeChoiceItem(playerIndex, actors::Player(playerIndex));
    const bool ok = PutInKnifeSlot(playerIndex, id, quiet);
    if (ok) KnifeIntoCopies(playerIndex, id);
    return ok;
}

bool IsKnife(uint32_t id) { return id == 0x0101 || id == 0x010E || id == 0x0123 || id == 0x0124; }

// The level loader only loads the archives of the items held: with another weapon in the knife slot, Chris's own
// knife (uWp01) was not loaded, and chapter 3-3 still asked for its model at start: "Failed open file
// pawn\wp\wp01\model\wp0100.mod", the game closed (user, 2026-10-15, variant 0x0124 in the slot, saved). While a
// knife choice is set, both characters' own knives stay loaded (held by the trainer, from the title screen on).
void KeepOwnKnivesLoaded() {
    LoadArchive("uWp01");
    LoadArchive("uWp0e");
}

// The player is prepared at creation for what its knife slot holds (a weapon there is stored like a long gun: a
// knife put back live afterwards hung upside down on the back, user 2026-10-13). A restart first copies the save
// (autosave) into the profile, then creates the player: the knife-slot entry of the copies is corrected every frame,
// loading screens included, so the player is created with the right item.
void FixKnifeCopies() {
    if (!OrganiserOpen()) return;  // the copies are the checkpoint's in play (TickSheathCheckpoint)
    uintptr_t profile = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), profile) || !profile) return;
    for (int i = 0; i < 2; ++i) {
        const KnifeSlot& slot = g_knife[i];
        const std::vector<uintptr_t> copies = InventoryCopies(i);
        if (copies.empty()) continue;
        uint32_t current = 0;
        mem::Read(copies[0] + 0x2C * 9, current);
        current &= 0xFFFF;
        uint16_t want = ActiveSecond(i);
        if (want) {
        } else if (!ActivePlaceEmpty(i) && ChoiceWeapon(slot.choice)) {
            want = ChoiceWeapon(slot.choice);
        } else if (current && !IsKnife(current)) {
            uint32_t character = 0;  // profile: P1 character +0x23318, P2 +0x50 further
            mem::Read(profile + 0x23318 + 0x50 * i, character);
            want = character == 1 ? 0x010E : 0x0101;
        }
        if (!want) continue;
        for (uintptr_t base : copies) {
            uint32_t id = 0;
            if (mem::Read(base + 0x2C * 9, id) && (id & 0xFFFF) != want) {
                KnifeIntoCopies(i, want);
                LOG("knife slot of P%d set to %04X in the inventory copies", i + 1, want);
                break;
            }
        }
    }
}

// Equip offsets: sParameter ITEM_EQUIP_OFS_INFO_STRUCT array (RVA 0xDB28B0, 0xB0 each, index = item info +0x3C):
// mPosMR/mAngMR, mPosML/mAngML, mPosFR/mAngFR, mPosFL/mAngFL (male/female weapon anims P+0x1AF8, right/left
// handedness P+0x1AF9), mScaM, mScaF. The attach function (RVA 0x84CB70) copies the chosen pair into the unit's
// local position/rotation (+0x30 / +0x40, local matrix translation +0x90) and scale (+0x50) when the weapon goes to
// the hand; the holstered knife keeps them. Sheva's knife (0x010E) has (-13.63, -0.4, 1.31) for her belt: on Chris
// the sheath sank into his back (user, 2026-10-14), at zero it sits where Chris's own knife does (user-verified).
// The table itself is left alone: Sheva's own knife is created before her flags are set (male right pair), so a
// table change moved her knife too.
// The second knife models (0x0123 Chris's motions, 0x0124 Sheva's: a stab instead of a slash) have zero offsets and
// still sank into Chris's back (user, 2026-10-14): their sheathed position gets the user's slider offset (player 1),
// the knife in hand keeps the game's.
uintptr_t ItemInfo(uint16_t id);
float g_variantSheath[3] = {};

bool EquipPosition(uintptr_t player, uint16_t id, float (&position)[3]) {
    const uintptr_t info = ItemInfo(id);
    uint32_t index = 0;
    uint8_t female = 0, left = 0;
    if (!info || !mem::Read(info + 0x3C, index) || index >= 0x100 || !mem::Read(player + 0x1AF8, female) ||
        !mem::Read(player + 0x1AF9, left) || left > 1)
        return false;
    const uintptr_t pair = reinterpret_cast<uintptr_t>(mem::Rva(0xDB28B0)) + index * 0xB0 + 0x10 + (left ? 0x20 : 0) +
                           (female ? 0x40 : 0);
    return mem::SafeRead(reinterpret_cast<const void*>(pair), position, sizeof(position));
}

void PlaceKnife(int playerIndex, uintptr_t player, uintptr_t item, uint16_t id) {
    uintptr_t unit = 0, held = 0;
    if (!mem::Read(item + 0x2C, unit) || !unit) return;
    float position[3] = {};
    if (id == 0x010E) {
        if (OwnKnife(player) == 0x010E) return;  // Sheva's own knife: the game's placement
    } else {
        if (!EquipPosition(player, id, position)) return;
        mem::Read(player + 0x2634, held);
        if (playerIndex == 0 && unit != held)
            for (int axis = 0; axis < 3; ++axis) position[axis] += g_variantSheath[axis];
    }
    float current[3] = {};
    if (!mem::SafeRead(reinterpret_cast<const void*>(unit + 0x30), current, sizeof(current)) ||
        std::memcmp(current, position, sizeof(position)) == 0)
        return;
    mem::SafeWrite(reinterpret_cast<void*>(unit + 0x30), position, sizeof(position));
    mem::SafeWrite(reinterpret_cast<void*>(unit + 0x90), position, sizeof(position));  // local matrix translation
}

// "Rien", the sheath shown empty: the sheathed knife is not the knife unit (moved 1 km away or scaled to 0 it still
// showed, user 2026-10-27) but a variation of the character model, like the holstered pistol: RVA 0x765F20 (thiscall on
// the player, at weapon changes) picks the model state from P+0x1E1C (pistol holstered) and P+0x1E18 (knife sheathed):
// states by (0x1E1C, 0x1E18): (set, set) 0, (set, 0) 2, (0, set) 1, (0, 0) 3, passed to RVA 0x44DBF0 (P+0x1930 = the
// state, then the variation object [P+0x1938]'s vtable +0x34). Hooked: for a player whose slot is "Rien", or player
// 1 in the first person (user, 2026-11-01: "the sheathed knife often passes in front of the camera; not the knife in
// hand"), P+0x1E18 reads 0 during the call (put back after). Applies at the next weapon change, or RefreshModelState.
bool HookGame(uintptr_t rva, const uint8_t* expected, size_t size, void* detour, void** original, const char* what);
using ModelStateFn = void(__fastcall*)(void* player, void* edx);
ModelStateFn g_modelState = nullptr;

void __fastcall HookModelState(void* self, void* edx) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(self);
    const int index = actors::PlayerIndex(p);
    uint32_t sheathed = 0;
    const bool hide = (KnifeSlotEmpty(index) || FirstPersonHidesSheath(p)) && mem::Read(p + 0x1E18, sheathed);
    if (hide) mem::Write<uint32_t>(p + 0x1E18, 0);
    g_modelState(self, edx);
    if (hide) mem::Write<uint32_t>(p + 0x1E18, sheathed);
}

bool EnsureModelStateHook() {
    static bool hooked = false;
    if (hooked) return true;
    const uint8_t expected[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x06, 0x8B, 0x50, 0x10, 0x57 };
    hooked = HookGame(0x765F20, expected, sizeof(expected), reinterpret_cast<void*>(&HookModelState),
                      reinterpret_cast<void**>(&g_modelState), "model state (sheath)");
    return hooked;
}

}  // namespace

// The model state picked again now (game thread), through the hook: what the game does at a weapon change.
bool RefreshModelState(uintptr_t player) {
    if (!player || !EnsureModelStateHook()) return false;
    __try {
        reinterpret_cast<ModelStateFn>(mem::Rva(0x765F20))(reinterpret_cast<void*>(player), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// A weapon's highest upgrade level in its row of the upgrade table (RVA 0xDC06A0 + (low * 0x7A + offset) * 4, 14 levels
// each): the capacity (offset 0, integers) up to its last increase before a 0 or a smaller value; the float sections
// up to the last value that differs from their 14th (the padding: VZ61 firepower 50 / 60 / 80 / 100 then 1.0).
int UpgradeMaxLevel(uint16_t id, int offset, bool capacity) {
    if ((id >> 8) != 1 || id >= 0x150) return 0;
    const uintptr_t section = reinterpret_cast<uintptr_t>(mem::Rva(0xDC06A0)) + ((id & 0xFF) * 0x7A + offset) * 4;
    int max = 0;
    if (capacity) {
        int32_t previous = 0;
        for (int level = 0; level < 14; ++level) {
            int32_t value = 0;
            if (!mem::Read(section + 4 * level, value) || value <= 0 || (level > 0 && value <= previous)) break;
            previous = value;
            max = level;
        }
        return max;
    }
    float padding = 0.0f;
    if (!mem::Read(section + 4 * 13, padding)) return 0;
    for (int level = 0; level < 13; ++level) {
        float value = padding;
        if (mem::Read(section + 4 * level, value) && value != padding) max = level;
    }
    return max;
}

// final_fight.cpp: a weapon given at its highest upgrade levels and with a full magazine (cItem::Set: count -2 = its
// capacity at its level), in a free grid place. The levels cItem::Set keeps, read by their getters (MODLOG 2026-11-01):
// +0x20 firepower (row - 45, RVA 0x634A30), +0x22 reload speed (row - 15, RVA 0x634A70), +0x23 capacity (row + 0),
// +0x25 (row + 15, RVA 0x795470), +0x26 (row + 30, RVA 0x7954B0: piercing, above 1).
bool GiveUpgradedWeapon(int playerIndex, uint16_t id) {
    const uintptr_t player = actors::Player(playerIndex);
    if (!HasItemManager(player) || !LoadItemResources(id)) return false;
    const int position = FreeGridPosition(player);
    if (position < 0) return false;
    const uint32_t fire = UpgradeMaxLevel(id, -45, false), reload = UpgradeMaxLevel(id, -15, false),
                   capacity = UpgradeMaxLevel(id, 0, true), special = UpgradeMaxLevel(id, 15, false),
                   piercing = UpgradeMaxLevel(id, 30, false);
    auto add = reinterpret_cast<AddItemFn>(mem::Rva(0x84E4F0));
    void* item = nullptr;
    __try {
        item = add(reinterpret_cast<void*>(player + kItemManager), nullptr, id, -2, -1, -1, -1, static_cast<uint32_t>(position),
                   fire, 0, reload, capacity, 0, special, piercing, 0, 0, static_cast<uint32_t>(player), 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        item = nullptr;
    }
    LOG("give upgraded weapon %04X to P%d at grid %d (firepower %u, reload %u, capacity %u, +0x25 %u, +0x26 %u) -> %p", id,
        playerIndex + 1, position, fire, reload, capacity, special, piercing, item);
    return item != nullptr;
}

// final_fight.cpp: a stack at its largest size (user, 2026-11-02: "the box at what the trainer asks as the largest stack:
// 150 by default, 999 if set so in the inventory tab"): the item info's +0x0C, which the stack table writes. A stack the
// player already holds is filled up (its record's count +0x08, its max +0x0C raised if lower), else one is given if he
// has room (no toast). Returns the count, 0 when nothing was done.
int FillStack(int playerIndex, uint16_t id) {
    const uintptr_t player = actors::Player(playerIndex);
    int count = 0, max = 0;
    if (!HasItemManager(player) || !ItemDefaults(id, count, max) || max <= 0) return 0;
    for (int k = 0; k < kItemRecords; ++k) {
        const uintptr_t r = player + kItemManager + 4 + 0x30 * k;
        uint32_t itemId = 0, recordMax = 0;
        uint8_t inUse = 0;
        if (!mem::Read(r + 0x29, inUse) || !inUse || !mem::Read(r + 4, itemId) || (itemId & 0xFFFF) != id) continue;
        if (mem::Read(r + 0x0C, recordMax) && recordMax < static_cast<uint32_t>(max)) mem::Write<uint32_t>(r + 0x0C, max);
        mem::Write<uint32_t>(r + 0x08, static_cast<uint32_t>(max));
        LOG("fill stack %04X of P%d to %d", id, playerIndex + 1, max);
        return max;
    }
    if (FreeGridPosition(player) < 0) return 0;
    return GiveItem(playerIndex, id, max) ? max : 0;
}

bool PlayerHasItem(int playerIndex, uint16_t id) {
    const uintptr_t player = actors::Player(playerIndex);
    if (!HasItemManager(player)) return false;
    for (int k = 0; k < kItemRecords; ++k) {
        const uintptr_t item = player + kItemManager + 4 + 0x30 * k;
        uint32_t itemId = 0;
        uint8_t inUse = 0;
        if (mem::Read(item + 0x29, inUse) && inUse && mem::Read(item + 4, itemId) && (itemId & 0xFFFF) == id) return true;
    }
    return false;
}

// "Jeter l'objet sélectionné" (user, 2026-11-02: "a shortcut that only works while the inventory is open and throws the
// highlighted item away"): what the menu's "Discard" does. The menu's commands are flags of a block of player 1's
// uPlIntel ([P+0x2D64] + 0x220; read by VA 0xB81F90: +0x244 equip VA 0xB782E0, +0x281 discard, ...) with their item's
// grid cell (+0x434 give, +0x43C discard); the discard command VA 0xB78840 (cdecl(player, its slot +0x2D9C, 0, 0, 0.0f))
// asks item_dump_first 0x5D at once for the item in hand (grid cell from VA 0xC4BB00(0xE)) or none (0xD), else queues
// 0x5D (P+0x1B3C[P+0x1B7C]) and equips that cell's item first (VA 0xB782E0). The highlighted cell is the inventory
// cursor's (cOpeWepSlot [P+0x2654]: +0x08 cell 0-8, +0x10 grid 0 his own / 1 the partner's, +0x68 & 1 open).
std::atomic<bool> g_discardAsked{ false };

bool CallDiscard(uintptr_t player, uint8_t slot) {
    using DiscardFn = void(__cdecl*)(uintptr_t player, int slot, int a2, int a3, float a4);
    __try {
        reinterpret_cast<DiscardFn>(mem::Rva(0x778840))(player, slot, 0, 0, 0.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void InventoryDiscardTick() {  // coop_solo.cpp's pad query tick (game thread)
    if (!g_discardAsked.exchange(false)) return;
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t cursor = 0, intel = 0;
    uint32_t cell = 0xD, grid = 2, open = 0, id = 0;
    uint8_t slot = 0;
    if (!p1 || !mem::Read(p1 + 0x2654, cursor) || !cursor || !mem::Read(cursor + 0x08, cell) || !mem::Read(cursor + 0x10, grid) ||
        !mem::Read(cursor + 0x68, open) || !(open & 1))
        return;
    // The partner's grid (+0x10 1; user, 2026-11-02: "an item of the partner's selected, the partner throws it away"):
    // the same command on the partner, his own block and slot.
    const uintptr_t p = grid == 1 ? actors::Player(1) : grid == 0 ? p1 : 0;
    if (!p || cell > 8 || !mem::Read(p + 0x2D64, intel) || !intel || !mem::Read(p + 0x2D9C, slot)) return;
    for (int k = 0; k < kItemRecords && !id; ++k) {
        const uintptr_t r = p + kItemManager + 4 + 0x30 * k;
        uint32_t position = 0xD, itemId = 0;
        uint8_t inUse = 0;
        if (mem::Read(r + 0x18, position) && position == cell && mem::Read(r + 0x29, inUse) && inUse && mem::Read(r + 4, itemId))
            id = itemId & 0xFFFF;
    }
    if (!id) return;  // an empty cell
    mem::Write(intel + 0x220 + 0x43C, cell);
    if (!CallDiscard(p, slot)) {
        LOG("discard: the game's discard command faulted");
        return;
    }
    LOG("discard: player %d throws away %04X (grid cell %u)", grid == 1 ? 2 : 1, id, cell);
}

namespace {

// "Rien" while the knife is out (user, 2026-10-27: a knife broken in the knife aim kept slashing; its drawing actions
// are refused, not the aim already started): knife_ready_end 0x24 asked while the player is in knife_ready_begin 0x22,
// its loop 0x23, the begin from a weapon 0x25 or knife_attack 0x27.
void PutKnifeAway(int playerIndex, uintptr_t player) {
    static DWORD asked[2] = {};
    uint32_t action = 0;
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(player + 0x10E0, action) || (action != 0x22 && action != 0x23 && action != 0x25 && action != 0x27)) return;
    const DWORD now = GetTickCount();
    if (now - asked[playerIndex] < 300 || !mem::Read(player, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return;
    asked[playerIndex] = now;
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(player), nullptr, 0x24, 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    LOG("knife slot: player %d's knife put away (action %X, item in hand grid %u)", playerIndex + 1, action,
        PlayerHeldGrid(player));
}

// The long gun hung on the back (user, 2026-11-05: "the weapon I put in the sheath goes on his back instead of his hand,
// the pistol, the knife too: nothing in his hand but he still attacks"): the item manager caches the record of its back
// weapon (+0x484; VA 0xC4B700(0x10), read every frame into P+0x2638 at VA 0xB705B7 and held on joint 117 by the back
// holder P+0x2040) and gives it back as long as the equipped weapon is one-handed, without checking it still holds a
// long gun. A long gun that left the knife slot left its record there: the knife or pistol added into that record was
// taken as the back weapon. The cache is cleared once its record holds no two-handed weapon (weapon parameters VA
// 0x11B5170 + low * 0x140, +0xC8) and the back holder let go (VA 0xB8E6C0 thiscall(holder, 0, 0, 0), what the game does
// when its back weapon is none): the game looks for its back weapon again.
void FixBackWeapon(int playerIndex, uintptr_t player) {
    uintptr_t cached = 0;
    if (!mem::Read(player + kItemManager + 0x484, cached) || !cached) return;
    uint8_t inUse = 0, twoHanded = 0;
    uint32_t id = 0;
    if (mem::Read(cached + 0x29, inUse) && inUse && mem::Read(cached + 4, id) && ((id >> 8) & 0xFF) == 1 &&
        mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB5170)) + (id & 0xFF) * 0x140 + 0xC8, twoHanded) && twoHanded)
        return;
    mem::Write<uintptr_t>(player + kItemManager + 0x484, 0);
    using ReleaseFn = void(__fastcall*)(void* holder, void* edx, int a, int b, int c);
    __try {
        reinterpret_cast<ReleaseFn>(mem::Rva(0x78E6C0))(reinterpret_cast<void*>(player + 0x2040), nullptr, 0, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    LOG("back weapon of P%d: the cached record %08X holds %04X (no long gun): cleared", playerIndex + 1,
        static_cast<unsigned>(cached), id & 0xFFFF);
}

// A thrown or single-shot weapon of the sheath used up (user, 2026-11-05: "empty the sheath, and no bug once it is
// empty"): its place emptied as "Jeter l'arme de l'étui surligné" does (an empty place is an empty sheath; "Rien" for
// the main one) - its record at 0, or gone after its last one was seen in hand in that place.
struct SingleUseWatch {
    uintptr_t player = 0;
    uint16_t id = 0;
    int place = -1;
    int32_t count = 0;
    bool inHand = false;
} g_singleUse[2];

bool SingleUseUsedUp(int i, uintptr_t player, uintptr_t item, uint16_t current) {
    SingleUseWatch& w = g_singleUse[i];
    const int place = g_sheathActive[i].load();
    const uint16_t want = SheathPlaceItem(i, place);
    if (!SingleUse(want) || g_knife[i].sheathPending) {
        w = {};
        return false;
    }
    int32_t count = 0;
    const bool there = item && current == want && mem::Read(item + 8, count);
    if (there && count > 0) {
        uintptr_t unit = 0, held = 0;
        w = { player, want, place, count, mem::Read(item + 0x2C, unit) && unit && mem::Read(player + 0x2634, held) && held == unit };
        return false;
    }
    const bool seen = w.player == player && w.id == want && w.place == place;
    if (!(there && count <= 0 && seen) && !(!there && seen && w.count == 1 && w.inHand)) {
        if (!seen) w = {};
        return false;
    }
    w = {};
    bool emptied = false;
    if (place > 0) {
        emptied = EnsurePlayerRequestHook() && EnsureModelStateHook() && SetSheathPlace(i, place, 0, false);
    } else if (Feature* slot = Find(i == 0 ? "inventory.knife_slot_p1" : "inventory.knife_slot_p2")) {
        emptied = Choose(*slot, KnifeSlotNoneChoice());
    }
    ForgetRounds(i, want);
    uint32_t action = 0;
    mem::Read(player + 0x10E0, action);
    LOG("sheath of P%d: %04X used up in place %d (%s, action %X), the place %s", i + 1, want, place + 1,
        there ? "at 0" : "gone", action, emptied ? "emptied" : "NOT emptied");
    if (emptied) {
        PutKnifeAway(i, player);  // the aim ends with it (the knife aim's actions; then every frame while the sheath is empty)
        Toast(i == 0 ? "Étui : arme épuisée, emplacement vidé (joueur 1)" : "Étui : arme épuisée, emplacement vidé (joueur 2)", false);
    }
    return emptied;
}

void UpdateKnifeSlots() {
    TickSheathCheckpoint();
    FixKnifeCopies();
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 2; ++i) {
        KnifeSlot& slot = g_knife[i];
        const uintptr_t player = actors::Player(i);
        if (!player || !HasItemManager(player)) {
            slot.mismatchSince = 0;
            continue;
        }
        const int record = KnifeRecord(player);
        const uintptr_t item = record >= 0 ? player + kItemManager + 4 + 0x30 * record : 0;
        uint32_t current = 0;
        if (item) mem::Read(item + 4, current);
        current &= 0xFFFF;
        if (item && (current == 0x010E || current == 0x0123 || current == 0x0124))
            PlaceKnife(i, player, item, static_cast<uint16_t>(current));
        FixBackWeapon(i, player);
        if (SingleUseUsedUp(i, player, item, static_cast<uint16_t>(current))) continue;
        if (KnifeUnusable(i) || slot.sheathPending) PutKnifeAway(i, player);  // its change waits for it
        // "Par défaut": any knife is fine (each character has its own); another weapon left in the slot by a
        // previous choice, or kept by an autosave, is replaced by the character's knife. The sheath's second knife wins.
        const bool isKnife = current == 0x0101 || current == 0x010E || current == 0x0123 || current == 0x0124;
        const uint16_t second = ActiveSecond(i);
        const bool wanted = slot.sheathPending ? current == KnifeChoiceItem(i, player)
                            : second ? current == second
                            : !ActivePlaceEmpty(i) && ChoiceWeapon(slot.choice) ? current == ChoiceWeapon(slot.choice)
                                                                                : (isKnife || !item);
        if (wanted) {
            slot.mismatchSince = 0;
            slot.sheathPending = false;
            continue;
        }
        if (!slot.mismatchSince) {
            slot.mismatchSince = now;
            continue;
        }
        // Retried in 3 s (e.g. the knife was in hand); a sheath change waits silently for the knife to be put away, tried
        // every 0.1 s (user, 2026-10-28: the broken second knife stayed visible 3 s).
        if (now - slot.mismatchSince < (slot.sheathPending ? 0u : 3000u)) continue;  // a sheath change: every frame
        const bool applied = ApplyKnifeChoice(i, slot.sheathPending);
        slot.mismatchSince = applied ? 0 : now;
        if (applied) slot.sheathPending = false;
    }
}

// ---- A firearm in the knife slot (user, 2026-11-05: "the slide locks once the magazine is spent but I can still
// fire; the infinite box does nothing"). The knife aim with a gun in the slot draws it as the weapon in hand
// (cItemManager key 0xE) and its shot is the game's fire action 0x1E (update VA 0xC89F30, MODLOG): it takes the round
// (VA 0xC4C1E0 at VA 0xC8A1A1: record +8 -= 1) unless VA 0xC4C200 (thiscall(manager, record) ret 4) says the weapon
// uses none (capacity level byte +0x23 above 13 = the game's infinite ammo, its global infinite mode...), then puts
// the gun in its empty state (slide back) when VA 0xC4C2F0 finds no round left. Nothing checks the magazine before a
// shot of the knife aim (the weapon aim does it for the main weapon). The old box wrote 1 into +0x23 (a capacity
// level, not infinite).
// - Infinite: VA 0xC4C200 answers true for that player's knife-slot gun: no round taken, the slide never locks.
// - Otherwise: the shot 0x1E is refused while that gun is in hand and empty, and the reload key in the knife aim
//   (0x22..0x27, or its shot 0x1E) asks the reload 0x1F: the game's reload of the weapon in hand, with its reserve
//   rules ("Recharger sans réserve", or the inventory's rounds) and the round in the chamber.
using AmmoInfiniteFn = bool(__fastcall*)(void* manager, void* edx, uintptr_t record);
using CapacityFn = int(__fastcall*)(void* manager, void* edx, uintptr_t record);
AmmoInfiniteFn g_ammoInfinite = nullptr;

// The knife slot's record when it holds a firearm (not a knife nor the stun rod), its player in `playerOut`.
uintptr_t SlotGunRecord(int playerIndex, uintptr_t* playerOut = nullptr) {
    const uintptr_t player = playerIndex >= 0 && playerIndex < 2 ? actors::Player(playerIndex) : 0;
    if (playerOut) *playerOut = player;
    if (!player || !HasItemManager(player)) return 0;
    const int k = KnifeRecord(player);
    const uintptr_t record = k >= 0 ? player + kItemManager + 4 + 0x30 * k : 0;
    uint32_t id = 0;
    if (!record || !mem::Read(record + 4, id)) return 0;
    id &= 0xFFFF;
    return KnifeSlotWeapon(static_cast<uint16_t>(id)) && !IsKnife(id) && id != 0x0122 ? record : 0;
}

// That gun is the weapon in hand.
bool SlotGunInHand(uintptr_t player, uintptr_t record) {
    uintptr_t unit = 0, held = 0;
    return record && mem::Read(record + 0x2C, unit) && unit && mem::Read(player + 0x2634, held) && held == unit;
}

bool __fastcall HookAmmoInfinite(void* manager, void* edx, uintptr_t record) {
    for (int i = 0; i < 2; ++i)
        if (g_knife[i].infinite && record && record == SlotGunRecord(i)) return true;
    return g_ammoInfinite(manager, edx, record);
}

bool AmmoInfinite(uintptr_t player, uintptr_t record) {
    const AmmoInfiniteFn infinite = g_ammoInfinite ? g_ammoInfinite : reinterpret_cast<AmmoInfiniteFn>(mem::Rva(0x84C200));
    __try {
        return infinite(reinterpret_cast<void*>(player + kItemManager), nullptr, record);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int SlotCapacity(uintptr_t player, uintptr_t record) {
    __try {
        return reinterpret_cast<CapacityFn>(mem::Rva(0x84C050))(reinterpret_cast<void*>(player + kItemManager), nullptr, record);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool EnsureAmmoInfiniteHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x84C200);
    // push esi; push edi; mov edi, [esp+0xC]; mov eax, [edi+4]
    const uint8_t expected[] = { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0x47, 0x04 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("knife slot gun: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookAmmoInfinite), reinterpret_cast<void**>(&g_ammoInfinite));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("knife slot gun: infinite ammo hook installed at %p", target);
    return true;
}

// Several items share a name in the game's message file: four knives (Chris's, Sheva's, and a second model for each:
// uWp23 holds Chris's motions, uWp24 Sheva's) and two pairs of herbs. Lists showed four "Couteau" and only the first
// could be picked (user, 2026-10-14): every label is made unique.
std::string DisplayName(uint16_t id, const char* name) {
    switch (id) {
    case 0x0101: return "Couteau (Chris)";
    case 0x010E: return "Couteau (Sheva)";
    case 0x0123: return "Couteau (Chris, variante)";
    case 0x0124: return "Couteau (Sheva, variante)";
    // Two Samurai Edges (user, 2026-10-27): Barry's (Mercenaries Reunion: uWp12, "(dummy_wp12)" in the message file,
    // the inventory shows "Samurai Edge") and Wesker's.
    case 0x0112: return "Samurai Edge (Barry)";
    case 0x0129: return "Samurai Edge (Wesker)";
    default: break;
    }
    if (!name) return {};
    int same = 0;
    for (const ItemName& other : kItems) same += std::strcmp(other.name, name) == 0;
    if (same < 2) return name;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s (%04X)", name, id);
    return buf;
}

// ---- Stack sizes (user, 2026-10-14). The item info table ([RVA 0xCEACF0 + cat*4] + low*0x48) holds each item's
// largest stack at +0x0C (handgun ammo 50, machine-gun ammo 150, herbs / spray / grenades 1) and every inventory
// record keeps its own copy (cItem +0x0C). Both are changed: the table for the records the game creates later, the
// records for the stacks already held (kept up to date every frame, restarts included). Offered: inventory items
// (+0x1C) that stack (+0x1F). Saved in stacks.ini (data folder); the table is restored when the trainer unloads.
// Weapons ignore the info table: cItem::Set (RVA 0x84E290) takes their capacity from the weapon upgrade table
// (RVA 0xDC06A0 + (low*0x7A + capacity level)*4, 14 levels, item ids below 0x150): magazine sizes for guns, and 5 for
// grenades, mines and eggs (one level). Healing items really are 1 in the base game.
std::vector<uint16_t> g_stackIds;
std::vector<std::string> g_stackLabels;
std::map<uint16_t, uint32_t> g_stackSet;       // configured largest stack per item
std::map<uint16_t, uint32_t> g_stackOriginal;  // table value before the trainer changed it
std::atomic<uint32_t> g_healStack[256] = {};   // healing items (0x03XX): configured stack, read by the merge hooks

void RefreshHealStacks() {
    for (std::atomic<uint32_t>& max : g_healStack) max.store(0, std::memory_order_relaxed);
    for (const auto& [id, max] : g_stackSet)
        if ((id >> 8) == 3) g_healStack[id & 0xFF].store(max, std::memory_order_relaxed);
}

uintptr_t ItemInfo(uint16_t id) {
    uintptr_t table = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xCEACF0)) + 4 * (id >> 8), table) || !table) return 0;
    return table + (id & 0xFF) * 0x48;
}

uintptr_t StackField(uint16_t id) {
    if ((id >> 8) == 1)
        return (id & 0xFF) < 0x50 ? reinterpret_cast<uintptr_t>(mem::Rva(0xDC06A0)) + (id & 0xFF) * 0x7A * 4 : 0;
    const uintptr_t info = ItemInfo(id);
    return info ? info + 0xC : 0;
}

void WriteStackField(uintptr_t field, uint32_t max) {
    if (!mem::Write(field, max)) mem::WriteCode(reinterpret_cast<void*>(field), &max, sizeof(max));
}

uint32_t OriginalStack(uint16_t id) {
    const auto known = g_stackOriginal.find(id);
    if (known != g_stackOriginal.end()) return known->second;
    uint32_t max = 0;
    if (const uintptr_t field = StackField(id)) mem::Read(field, max);
    return max;
}

void WriteStackTable(uint16_t id, uint32_t max) {
    const uintptr_t field = StackField(id);
    uint32_t current = 0;
    if (!field || !mem::Read(field, current) || current == max) return;
    if (!g_stackOriginal.count(id)) g_stackOriginal[id] = current;
    WriteStackField(field, max);
}

void WriteStackRecords(uint16_t id, uint32_t max) {
    if (!actors::InGame()) return;
    for (int i = 0; i < 2; ++i) {
        const uintptr_t player = actors::Player(i);
        if (!HasItemManager(player)) continue;
        for (int k = 0; k < kItemRecords; ++k) {
            const uintptr_t item = player + kItemManager + 4 + 0x30 * k;
            uint8_t inUse = 0;
            uint32_t itemId = 0, current = 0;
            if (mem::Read(item + 0x29, inUse) && inUse && mem::Read(item + 4, itemId) && (itemId & 0xFFFF) == id &&
                mem::Read(item + 0xC, current) && current != max)
                mem::Write(item + 0xC, max);
        }
    }
}

std::wstring StacksPath() { return logx::DataDir() + L"\\stacks.ini"; }

void SaveStacks() {
    FILE* file = nullptr;
    if (_wfopen_s(&file, StacksPath().c_str(), L"w") || !file) return;
    for (const auto& [id, max] : g_stackSet) std::fprintf(file, "%04X=%u\n", id, max);
    std::fclose(file);
}

// Single green and red herbs are combined in the inventory (green + green, green + red): a stack of them could no
// longer be combined (user, 2026-10-14). Mixed herbs and the spray can stack.
bool CombinableHerb(uint16_t id) { return id == 0x0301 || id == 0x0302 || id == 0x0305 || id == 0x0306; }

void LoadStacks() {
    FILE* file = nullptr;
    if (_wfopen_s(&file, StacksPath().c_str(), L"r") || !file) return;
    unsigned id = 0, max = 0;
    while (fscanf_s(file, "%x=%u\n", &id, &max) == 2)
        if (max >= 1 && max <= 999 && std::find(g_stackIds.begin(), g_stackIds.end(), id) != g_stackIds.end())
            g_stackSet[static_cast<uint16_t>(id)] = max;
    std::fclose(file);
    RefreshHealStacks();
}

// The inventory grid (uIdWpSlot00, RVA 0x673DE2) draws an item's count only when item info +0x44 is set (1 ammo,
// grenades, mines, eggs; 2 guns; 0 healing items): stacked sprays and herbs showed no number (user, 2026-10-14). Set
// to 1 while a stack above 1 is configured for an item that has 0.
std::map<uint16_t, uint32_t> g_countOriginal;

void ShowCount(uint16_t id, bool show) {
    const uintptr_t info = ItemInfo(id);
    uint32_t current = 0;
    if (!info || !mem::Read(info + 0x44, current)) return;
    if (show) {
        if (current) return;
        g_countOriginal.emplace(id, current);
        WriteStackField(info + 0x44, 1);
        return;
    }
    const auto known = g_countOriginal.find(id);
    if (known == g_countOriginal.end()) return;
    WriteStackField(info + 0x44, known->second);
    g_countOriginal.erase(known);
}

bool EnsureMergeHooks();

void SetStack(uint16_t id, uint32_t max) {
    if (max == OriginalStack(id)) g_stackSet.erase(id);
    else g_stackSet[id] = max;
    RefreshHealStacks();
    if ((id >> 8) == 3 && g_stackSet.count(id)) EnsureMergeHooks();
    WriteStackTable(id, max);
    WriteStackRecords(id, max);
    ShowCount(id, g_stackSet.count(id) && max > 1);
}

void UpdateStacks() {
    if (g_stackSet.empty() || !game::IsSupported()) return;
    static bool hooksTried = false;
    if (!hooksTried) {
        hooksTried = true;
        for (const auto& entry : g_stackSet)
            if ((entry.first >> 8) == 3) {
                EnsureMergeHooks();
                break;
            }
    }
    for (const auto& [id, max] : g_stackSet) {
        WriteStackTable(id, max);
        WriteStackRecords(id, max);
        ShowCount(id, max > 1);
    }
}

void RestoreStacks() {
    for (std::atomic<uint32_t>& max : g_healStack) max.store(0, std::memory_order_relaxed);
    for (const auto& [id, original] : g_countOriginal)
        if (const uintptr_t info = ItemInfo(id)) WriteStackField(info + 0x44, original);
    g_countOriginal.clear();
    for (const auto& [id, max] : g_stackOriginal) {
        if (const uintptr_t field = StackField(id)) WriteStackField(field, max);
        WriteStackRecords(id, max);
    }
    g_stackOriginal.clear();
}

// ---- Merging stacks of healing items (user, 2026-10-14). Dropping an item on another in the inventory asks
// cItemManager::CanCombine (RVA 0x84D6E0, thiscall(from, to), ret 8) and, if allowed, runs cItemManager::Combine
// (RVA 0x84E640, thiscall(player, from, to), ret 0xC); otherwise the two cases are swapped (RVA 0x84BCA0). Ammo,
// grenades, mines and eggs of the same id merge their counts; healing items only follow the herb recipes (lookup
// RVA 0x84C350, table RVA 0xD46A04; RVA 0x84DED0 replaces both records with the result), so two V+R herbs could not
// be stacked. For a healing item whose stack the trainer raised, the same id merges like ammo: the dropped stack
// fills the target and an emptied record is cleared by cItem::Clear (RVA 0x84C4E0, thiscall(1)), as the game does.
using CanCombineFn = bool(__fastcall*)(void* manager, void* edx, int from, int to);
using CombineFn = bool(__fastcall*)(void* manager, void* edx, void* player, int from, int to);
using ItemClearFn = void(__fastcall*)(void* item, void* edx, int release);
CanCombineFn g_canCombine = nullptr;
CombineFn g_combine = nullptr;
bool g_mergeHooked = false;

uintptr_t RecordAt(uintptr_t manager, int position) {
    for (int k = 0; k < kItemRecords; ++k) {
        const uintptr_t item = manager + 4 + 0x30 * k;
        uint8_t inUse = 0;
        int32_t at = -1;
        if (mem::Read(item + 0x29, inUse) && inUse && mem::Read(item + 0x18, at) && at == position) return item;
    }
    return 0;
}

// Both records, when they hold the same healing item with a stack raised by the trainer.
bool HealStackPair(uintptr_t manager, int from, int to, uintptr_t& source, uintptr_t& target) {
    if (from == to || from < 0 || from > 8 || to < 0 || to > 8) return false;
    source = RecordAt(manager, from);
    target = RecordAt(manager, to);
    uint32_t a = 0, b = 0;
    if (!source || !target || !mem::Read(source + 4, a) || !mem::Read(target + 4, b)) return false;
    a &= 0xFFFF;
    b &= 0xFFFF;
    return a == b && (a >> 8) == 3 && g_healStack[a & 0xFF].load(std::memory_order_relaxed) > 1;
}

bool __fastcall HookCanCombine(void* manager, void* edx, int from, int to) {
    uintptr_t source = 0, target = 0;
    if (HealStackPair(reinterpret_cast<uintptr_t>(manager), from, to, source, target)) {
        int32_t count = 0, max = 0;
        mem::Read(target + 8, count);
        mem::Read(target + 0xC, max);
        return max > count;
    }
    return g_canCombine(manager, edx, from, to);
}

bool __fastcall HookCombine(void* manager, void* edx, void* player, int from, int to) {
    uintptr_t source = 0, target = 0;
    if (!HealStackPair(reinterpret_cast<uintptr_t>(manager), from, to, source, target))
        return g_combine(manager, edx, player, from, to);
    int32_t moved = 0, count = 0, max = 0;
    mem::Read(source + 8, moved);
    mem::Read(target + 8, count);
    mem::Read(target + 0xC, max);
    if (max <= count || moved <= 0) return false;
    if (moved > max - count) moved = max - count;
    int32_t left = 0;
    mem::Read(source + 8, left);
    left -= moved;
    mem::Write<int32_t>(target + 8, count + moved);
    mem::Write<int32_t>(source + 8, left);
    if (left <= 0) {
        __try {
            reinterpret_cast<ItemClearFn>(mem::Rva(0x84C4E0))(reinterpret_cast<void*>(source), nullptr, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    LOG("stack merge: %d moved from case %d to case %d (%d left)", moved, from + 1, to + 1, left > 0 ? left : 0);
    return true;
}

bool HookGame(uintptr_t rva, const uint8_t* expected, size_t size, void* detour, void** original, const char* what) {
    uint8_t* target = mem::Rva(rva);
    if (!mem::IsGameCode(target, size) || std::memcmp(target, expected, size) != 0) {
        LOG("%s hook: unexpected bytes %s", what, mem::Hex(target, size).c_str());
        return false;
    }
    mem::EnsureExecutable(target, size);
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    LOG("%s hook installed at %p", what, target);
    return true;
}

bool EnsureMergeHooks() {
    if (g_mergeHooked) return true;
    const uint8_t canCombine[] = { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10 };
    const uint8_t combine[] = { 0x83, 0xEC, 0x08, 0x8B, 0x44, 0x24, 0x10, 0x53, 0x56 };
    g_mergeHooked = HookGame(0x84D6E0, canCombine, sizeof(canCombine), reinterpret_cast<void*>(&HookCanCombine),
                             reinterpret_cast<void**>(&g_canCombine), "can-combine") &&
                    HookGame(0x84E640, combine, sizeof(combine), reinterpret_cast<void*>(&HookCombine),
                             reinterpret_cast<void**>(&g_combine), "combine");
    return g_mergeHooked;
}

std::string ItemLabel(uint16_t id) {
    for (const ItemName& item : kItems)
        if (item.id == id) return DisplayName(item.id, item.name);
    if (std::string named = DisplayName(id, nullptr); !named.empty()) return named;  // a DLC weapon without its name
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Objet %04X", id);
    return buf;
}

int g_editPlayer = 0;
int g_addChoice = 0;
int g_addCount = 0;

void DrawStackTable();

// Inventory editor tab: the live records of one player, sorted by grid position. Counts, max and the two weapon
// flags (record bytes +0x21 rapid fire / +0x23 infinite ammo, the Ultimate Trainer's) apply at once. "Général" holds
// the options of the Inventaire category (no tab of their own any more, user 2026-10-14) and the stack sizes.
void DrawInventoryPage();

// The player choice stays in view, its page scrolls below it (user, 2026-10-23).
void DrawInventoryEditor() {
    ImGui::RadioButton(i18n::Tr("Joueur 1"), &g_editPlayer, 0);
    ImGui::SameLine();
    ImGui::RadioButton(i18n::Tr("Joueur 2"), &g_editPlayer, 1);
    ImGui::SameLine();
    ImGui::RadioButton(i18n::Tr("Général"), &g_editPlayer, 2);
    ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_None);
    DrawInventoryPage();
    ImGui::EndChild();
}

void DrawInventoryPage() {
    if (g_editPlayer == 2) {
        DrawOptions("Inventaire");
        ImGui::SeparatorText(i18n::Tr("Étui (emplacement couteau)"));
        ImGui::PushID("sheath");
        DrawOptions("Couteau : étui");
        ImGui::PopID();
        ImGui::Separator();
        DrawStackTable();
        return;
    }
    // The player's sheath (user, 2026-11-05: both in Général, each player's on his page too).
    ImGui::PushID("sheath");
    DrawOption(g_editPlayer == 0 ? "inventory.knife_slot_p1" : "inventory.knife_slot_p2");
    ImGui::PopID();
    const uintptr_t player = actors::Player(g_editPlayer);
    if (!game::IsSupported() || !HasItemManager(player)) {
        ImGui::TextDisabled(i18n::Tr("Aucun joueur chargé."));
        return;
    }
    struct Row {
        int record;
        uint32_t position;
    };
    std::vector<Row> rows;
    for (int k = 0; k < kItemRecords; ++k) {
        const uintptr_t item = player + kItemManager + 4 + 0x30 * k;
        uint8_t inUse = 0;
        uint32_t position = 0xD;
        if (mem::Read(item + 0x29, inUse) && inUse && mem::Read(item + 0x18, position)) rows.push_back({ k, position });
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.position < b.position; });
    const float scale = ImGui::GetFontSize() / 13.0f;
    if (ImGui::BeginTable("inventory", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(i18n::Tr("Case"));
        ImGui::TableSetupColumn(i18n::Tr("Objet"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(i18n::Tr("Quantité"));
        ImGui::TableSetupColumn(i18n::Tr("Max"));
        ImGui::TableSetupColumn(i18n::Tr("Rapide / Infini"));
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const Row& row : rows) {
            const uintptr_t item = player + kItemManager + 4 + 0x30 * row.record;
            uint32_t id = 0, count = 0, max = 0;
            mem::Read(item + 4, id);
            mem::Read(item + 8, count);
            mem::Read(item + 0xC, max);
            ImGui::PushID(row.record);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (row.position < 9) ImGui::Text("%u", row.position + 1);
            else if (row.position == 9) ImGui::TextUnformatted(i18n::Tr("Couteau"));
            else ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(ItemLabel(static_cast<uint16_t>(id)).c_str());
            ImGui::TableSetColumnIndex(2);
            int value = static_cast<int>(count);
            ImGui::SetNextItemWidth(90 * scale);
            if (ImGui::InputInt("##count", &value, 1, 10) && value >= 0) mem::Write<uint32_t>(item + 8, static_cast<uint32_t>(value));
            ImGui::TableSetColumnIndex(3);
            value = static_cast<int>(max);
            ImGui::SetNextItemWidth(90 * scale);
            if (ImGui::InputInt("##max", &value, 1, 10) && value >= 0) mem::Write<uint32_t>(item + 0xC, static_cast<uint32_t>(value));
            ImGui::TableSetColumnIndex(4);
            if ((id >> 8) == 1) {
                uint8_t rapid = 0, infinite = 0;
                mem::Read(item + 0x21, rapid);
                mem::Read(item + 0x23, infinite);
                bool r = rapid != 0, i = infinite != 0;
                if (ImGui::Checkbox("##rapid", &r)) mem::Write<uint8_t>(item + 0x21, r ? 1 : 0);
                ImGui::SameLine();
                if (ImGui::Checkbox("##infinite", &i)) mem::Write<uint8_t>(item + 0x23, i ? 1 : 0);
            }
            ImGui::TableSetColumnIndex(5);
            if (row.position < 9 && ImGui::SmallButton(i18n::Tr("Retirer"))) RemoveItem(player, row.record);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextUnformatted(i18n::Tr("Ajouter un objet :"));
    if (g_addChoice < 0 || g_addChoice >= static_cast<int>(g_giveChoices.size())) g_addChoice = 0;
    ImGui::SetNextItemWidth(300 * scale);
    if (ImGui::BeginCombo("##add", g_giveChoices.empty() ? "" : g_giveChoices[g_addChoice].c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int k = 0; k < static_cast<int>(g_giveChoices.size()); ++k) {
            ImGui::PushID(k);
            if (ImGui::Selectable(g_giveChoices[k].c_str(), k == g_addChoice)) g_addChoice = k;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * scale);
    ImGui::InputInt(i18n::Tr("Quantité##add"), &g_addCount, 1, 10);
    if (g_addCount < 0) g_addCount = 0;
    ImGui::SameLine();
    if (ImGui::Button(i18n::Tr("Ajouter")) && g_addChoice > 0)
        GiveItem(g_editPlayer, g_giveIds[g_addChoice - 1], g_addCount);
    ImGui::TextDisabled(i18n::Tr("Quantité 0 = quantité normale de l'objet. Une arme absente du niveau est chargée à la volée."));
    ImGui::TextDisabled(i18n::Tr("Rapide / Infini : tir rapide et munitions infinies de l'arme (cases à cocher)."));
    ImGui::TextDisabled(i18n::Tr("Options d'inventaire et taille des piles : vignette « Général »."));
}

void DrawStackTable() {
    const float scale = ImGui::GetFontSize() / 13.0f;
    ImGui::TextUnformatted(i18n::Tr("Taille des piles (les deux joueurs, gardée après redémarrage) :"));
    if (!g_stackSet.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(i18n::Tr("Tout par défaut"))) {
            std::vector<uint16_t> changed;
            for (const auto& entry : g_stackSet) changed.push_back(entry.first);
            for (uint16_t id : changed) SetStack(id, OriginalStack(id));
            SaveStacks();
        }
    }
    if (ImGui::BeginTable("stacks", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_SizingFixedFit, ImVec2(0, 300 * scale))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(i18n::Tr("Objet"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(i18n::Tr("Normal"));
        ImGui::TableSetupColumn(i18n::Tr("Pile max"));
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (int k = 0; k < static_cast<int>(g_stackIds.size()); ++k) {
            const uint16_t id = g_stackIds[k];
            const uint32_t original = OriginalStack(id);
            const auto configured = g_stackSet.find(id);
            const bool changed = configured != g_stackSet.end();
            int value = static_cast<int>(changed ? configured->second : original);
            ImGui::PushID(k);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(g_stackLabels[k].c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", original);
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(180 * scale);
            if (widgets::SliderInt("##max", &value, 1, 999)) SetStack(id, static_cast<uint32_t>(value));
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveStacks();
            ImGui::TableSetColumnIndex(3);
            if (changed && ImGui::SmallButton(i18n::Tr("Par défaut"))) {
                SetStack(id, original);
                SaveStacks();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled(i18n::Tr("Normal = valeur du jeu. Glisser un curseur pour régler, clic simple pour taper la valeur."));
}
int g_giveChoice = 0;  // index in g_giveIds + 1 (0 = nothing chosen)

// Reunion weapons' icons in the story (user, 2026-10-27: Barry's Samurai Edge given in the story had no icon). The
// weapons' inventory icons are elements "wpXX" of the cockpit GUI: the story loads id\cockpit\cp_life_360 (no wp12),
// the DLC and Mercenaries Reunion (mode [[RVA 0xE340A4]+0x1042C]+0x58 = 3 / 4 / 5, RVA 0x5F3C0D) its superset
// cp_life_360_dc (wp12, textures cp26 / cp28), both in the archive Image\Archive\Game2Resource_<language> (the language
// added by RVA 0x7BA250, thiscall on [RVA 0xDB2158] (path, 0, 0), as RVA 0x3D67B4 does). The option loads that archive
// and makes both GUI loads (RVA 0x5F3C2D and the unconditional one RVA 0x5FC277) take the _dc path: at the next load.
mem::Patch g_dcGuiByMode("inventory.reunion_icons (mode)", 0x5F3C2D, { 0x68, 0xC4, 0x04, 0xFA, 0x00 },
                         { 0x68, 0xA8, 0x04, 0xFA, 0x00 });
mem::Patch g_dcGuiAlways("inventory.reunion_icons", 0x5FC277, { 0x68, 0xC4, 0x04, 0xFA, 0x00 }, { 0x68, 0xA8, 0x04, 0xFA, 0x00 });

// The game's localized path of `path` (RVA 0x3BA250, thiscall on [RVA 0xDB2158] with the path alone: the two zeros
// pushed before its call at RVA 0x3D67B4 are the next call's - taken for its own, they were left on the stack and the
// trainer crashed with a null register, user 2026-10-27). The stack pointer is saved around the call whatever the
// callee removes.
const char* Localize(void* sys, const char* path) {
    void* fn = mem::Rva(0x3BA250);
    const char* result = nullptr;
    uint32_t saved = 0;
    __asm {
        mov saved, esp
        push path
        mov ecx, sys
        call fn
        mov esp, saved
        mov result, eax
    }
    return result;
}

// Trading with the partner (user, 2026-10-27: "trade anything from anywhere"). The item sub-menu (cOpeWepSlot RVA
// 0x86A4C0: commands +0x94[i], 5 give / 8 exchange, enabled +0xB8[i]) enables "Donner" when (a) RVA 0x7605E0: the
// partner exists and is in range 0x11 of RVA 0x75C0F0 = within 300 cm across / 180 cm up (sParameter +0x368 / +0x36C,
// RVA 0x75C060) and in sight (collision rays), and (b) the partner can receive the item (RVA 0x765E30: room, then RVA
// 0x3BD850 "tradable": anything but weapons, a few weapons (grenades, mines, eggs) excepted). The give itself is an action
// button of the item in hand: availability RVA 0x777FA0 (the same range check at RVA 0x7780B8), handler RVA 0x778AC0
// (actions 0x63 / 0x64 item_hand_partner_first / second); the partner's AI, asked for an item, takes it in hand and walks
// to player 1 until that button is available.
// - "Échanger les armes": RVA 0x3BD850 answers "tradable" for everything.
// - "Donner et demander à distance": the sub-menu's range test (call at RVA 0x86A52E) and the button's (call at RVA
//   0x7780B8, thiscall with 2 arguments: they are removed) answer yes.
mem::Patch g_tradeWeapons("inventory.trade_weapons", 0x3BD850, { 0x57, 0x8B, 0x7C }, { 0xB0, 0x01, 0xC3 });
mem::Patch g_giveMenuRange("inventory.trade_anywhere (menu)", 0x86A52E, { 0xE8, 0xAD, 0x60, 0xEF, 0xFF },
                           { 0xB0, 0x01, 0x90, 0x90, 0x90 });
mem::Patch g_giveButtonRange("inventory.trade_anywhere (button)", 0x7780B8, { 0xE8, 0x33, 0x40, 0xFE, 0xFF },
                             { 0x83, 0xC4, 0x08, 0xB0, 0x01 });

// "Échanges sans animation" (user, 2026-10-27): the give button's handler (RVA 0x778AC0, cdecl, 2nd argument the
// player's slot; the grid asked = [[P+0x2D64]+0x220]+0x438, 0xD = the item in hand) is replaced by what 0x64's first
// update does (VA 0xC74785..): the transfer P.VA 0xB5A8E0(partner, grid) (-> sPlayer VA 0x7E9BD0), then for a local
// receiver (+0x2DA8 == 1) its HUD notice ([VA 0xB5C420(P)+0x954] VA 0xCA6B20(id), P.VA 0xB739F0(2)) and for a local
// giver his (VA 0xCA6BB0(id), VA 0xB739F0(-2)); the menu's give request cleared (cOpeWepSlot VA 0xC69BA0). No weapon
// change to the item first, no 0x63 / 0x64. Mercenaries ([[RVA 0xE340A4]+0x1042C]+0x56C) has its own transfer: the game's.
using GiveHandlerFn = void(__cdecl*)(int a0, int slot, int a2, int a3, int a4);
GiveHandlerFn g_giveHandler = nullptr;
std::atomic<bool> g_tradeNoAnim{ false };

uintptr_t PlayerOfSlot(int slot) {
    uintptr_t mgr = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB27DC)), mgr) || !mgr) return 0;
    __try {
        return reinterpret_cast<uintptr_t(__fastcall*)(void*, void*, int)>(mem::Rva(0x3E6220))(reinterpret_cast<void*>(mgr),
                                                                                                 nullptr, slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The grid of the item in a player's hand (cItemManager VA 0xC4BB00(0xE)), 0xD if none.
uint32_t HeldGrid(uintptr_t player) {
    __try {
        return reinterpret_cast<uint32_t(__fastcall*)(void*, void*, int)>(mem::Rva(0x84BB00))(
            reinterpret_cast<void*>(player + kItemManager), nullptr, 0xE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xD;
    }
}

// The HUD notice of a local player who received (+2) or gave (-2) the item.
void TradeNotice(uintptr_t player, uint16_t id, bool received) {
    uint32_t local = 0;
    if (!mem::Read(player + 0x2DA8, local) || local != 1) return;
    __try {
        const uintptr_t hud = reinterpret_cast<uintptr_t(__fastcall*)(void*, void*)>(mem::Rva(0x75C420))(
            reinterpret_cast<void*>(player), nullptr);
        uintptr_t notice = 0;
        if (hud && mem::Read(hud + 0x954, notice) && notice)
            reinterpret_cast<void(__fastcall*)(void*, void*, int)>(mem::Rva(received ? 0x8A6B20 : 0x8A6BB0))(
                reinterpret_cast<void*>(notice), nullptr, id);
        reinterpret_cast<void(__fastcall*)(void*, void*, int)>(mem::Rva(0x7739F0))(reinterpret_cast<void*>(player), nullptr,
                                                                                    received ? 2 : -2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool TradeNow(uintptr_t giver, uintptr_t partner, uint32_t grid) {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, uintptr_t, uint32_t)>(mem::Rva(0x75A8E0))(
            reinterpret_cast<void*>(giver), nullptr, partner, grid);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ClearGiveRequest(uintptr_t player) {
    uintptr_t ope = 0;
    if (!mem::Read(player + 0x2654, ope) || !ope) return;
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, int)>(mem::Rva(0x869BA0))(reinterpret_cast<void*>(ope), nullptr, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __cdecl HookGiveHandler(int a0, int slot, int a2, int a3, int a4) {
    uint8_t merc = 0;
    const uintptr_t mercFlag = mem::Chain(reinterpret_cast<uintptr_t>(mem::Rva(0xE340A4)), { 0, 0x1042C, 0x56C });
    if (!g_tradeNoAnim.load(std::memory_order_relaxed) || (mercFlag && mem::Read(mercFlag, merc) && merc))
        return g_giveHandler(a0, slot, a2, a3, a4);
    const uintptr_t giver = PlayerOfSlot(slot);
    uint8_t partnerSlot = 0xFF;
    uintptr_t input = 0;
    uint32_t grid = 0xD;
    if (!giver || !mem::Read(giver + 0x2D9D, partnerSlot) || !mem::Read(giver + 0x2D64, input) || !input ||
        !mem::Read(input + 0x220 + 0x438, grid))
        return g_giveHandler(a0, slot, a2, a3, a4);
    if (grid == 0xD) grid = HeldGrid(giver);
    const uintptr_t partner = PlayerOfSlot(partnerSlot);
    uintptr_t record = 0;
    uint32_t id = 0;
    if (!partner || grid >= 0xD) return g_giveHandler(a0, slot, a2, a3, a4);
    for (int k = 0; k < kItemRecords && !record; ++k) {
        const uintptr_t r = giver + kItemManager + 4 + 0x30 * k;
        uint32_t position = 0xD;
        uint8_t inUse = 0;
        if (mem::Read(r + 0x18, position) && position == grid && mem::Read(r + 0x29, inUse) && inUse) record = r;
    }
    if (!record || !mem::Read(record + 4, id) || !id || !TradeNow(giver, partner, grid))
        return g_giveHandler(a0, slot, a2, a3, a4);
    TradeNotice(partner, static_cast<uint16_t>(id), true);
    TradeNotice(giver, static_cast<uint16_t>(id), false);
    ClearGiveRequest(giver);
    LOG("trade without animation: player slot %d gave %04X (grid %u) to slot %u", slot, id, grid, partnerSlot);
}

bool EnsureGiveHandlerHook() {
    static bool hooked = false;
    if (hooked) return true;
    const uint8_t expected[] = { 0x8B, 0x0D, 0xDC, 0x27, 0x1B, 0x01, 0x53, 0x8B, 0x5C, 0x24, 0x0C };
    hooked = HookGame(0x778AC0, expected, sizeof(expected), reinterpret_cast<void*>(&HookGiveHandler),
                      reinterpret_cast<void**>(&g_giveHandler), "give handler");
    return hooked;
}

// "Image\Archive\Game2Resource_<language>" as the game builds it (nullptr on a fault).
const char* Game2ResourcePath() {
    void* sys = nullptr;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), sys) || !sys) return nullptr;
    __try {
        return Localize(sys, reinterpret_cast<const char*>(mem::Rva(0xB64BE4)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool LoadReunionResources() {
    const char* path = Game2ResourcePath();
    constexpr char kPrefix[] = "Image\\Archive\\";
    if (!path || !mem::IsReadable(path, 32) || std::strncmp(path, kPrefix, sizeof(kPrefix) - 1) != 0) {
        LOG("reunion icons: no localized archive name");
        return false;
    }
    const std::string name = path + sizeof(kPrefix) - 1;
    return LoadArchive(name.c_str());
}
}

bool LoadWeaponArchive(const char* name) { return LoadArchive(name); }

// action_log.cpp's player request hook: a player whose knife slot is "Rien" does not draw a knife (knife_ready_begin
// 0x22, knife_ready_begin_from_weapon_ready 0x25, knife_attack 0x27).
// action_log.cpp's player request hook: the shot refused while the knife slot's gun is in hand and empty (see
// SlotGunRecord). In the knife aim the shot is its knife_attack 0x27 (requested at RVA 0x76C59F; user, 2026-11-05: the
// count went to -9 with 0x1E alone refused), the weapon aim's 0x1E otherwise.
bool SheathGunEmptyRefuses(uintptr_t player, int action) {
    if (action != 0x1E && action != 0x27) return false;
    const int i = actors::PlayerIndex(player);
    uintptr_t p = 0;
    const uintptr_t record = SlotGunRecord(i, &p);
    int32_t count = 0;
    uint32_t id = 0;
    // Not a thrown or single-shot one (user, 2026-11-05: refused, it would stay stuck in its motion): used up, its place
    // is emptied and the aim put away (SingleUseUsedUp).
    if (!record || p != player || !mem::Read(record + 4, id) || SingleUse(static_cast<uint16_t>(id)) ||
        !SlotGunInHand(player, record) || !mem::Read(record + 8, count) || count > 0 || g_knife[i].infinite ||
        AmmoInfinite(player, record))
        return false;
    static DWORD logged[2] = {};
    if (GetTickCount() - logged[i] > 2000) {
        logged[i] = GetTickCount();
        LOG("knife slot gun: player %d's shot refused, the magazine is empty", i + 1);
    }
    return true;
}

// Game thread (quick_knife.cpp's pad queries): the reload key in the knife aim reloads the knife slot's gun.
bool ReloadButtonPressed();  // quick_knife.cpp: the game's reload button or "Recharger (raccourci)"
void SheathGunReloadTick() {
    static ULONGLONG last = 0;
    static bool was = false;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 10) return;  // many queries a frame
    last = now;
    uintptr_t player = 0;
    const uintptr_t record = SlotGunRecord(0, &player);
    int32_t rounds = 0;
    if (record && mem::Read(record + 8, rounds) && rounds < 0) mem::Write<int32_t>(record + 8, 0);  // shots before the fix
    const bool pressed = record && ReloadButtonPressed();
    const bool edge = pressed && !was;
    was = pressed;
    uint32_t action = 0;
    int32_t count = 0;
    uint32_t slotId = 0;
    if (!edge || !mem::Read(record + 4, slotId) || SingleUse(static_cast<uint16_t>(slotId)) || !SlotGunInHand(player, record) ||
        !mem::Read(player + 0x10E0, action) ||
        (action != 0x1E && (action < 0x22 || action > 0x27)) || !mem::Read(record + 8, count) || g_knife[0].infinite ||
        AmmoInfinite(player, record))
        return;
    const int capacity = SlotCapacity(player, record);
    if (capacity > 0 && count >= capacity) return;
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(player, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return;
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(player), nullptr, 0x1F, 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    LOG("knife slot gun: reload asked in the knife aim (action %X, %d / %d rounds)", action, count, capacity);
}

// knife_icon.cpp's inventory screen: the rounds of a firearm of the sheath (user, 2026-11-05: "the count in the
// inventory too"): the slot's record while it holds it, else the rounds it left with (a full magazine if none).
bool SheathGunRounds(int playerIndex, uint16_t id, int& count, int& capacity) {
    if (playerIndex < 0 || playerIndex > 1 || IsKnife(id) || id == 0x0122 || !KnifeSlotWeapon(id)) return false;
    uintptr_t player = 0;
    const uintptr_t record = SlotGunRecord(playerIndex, &player);
    uint32_t current = 0;
    int32_t rounds = 0;
    // A thrown or single-shot weapon: its count, no capacity (no "+1", no green).
    if (record && mem::Read(record + 4, current) && (current & 0xFFFF) == id && mem::Read(record + 8, rounds)) {
        count = rounds;
        capacity = SingleUse(id) ? 0 : SlotCapacity(player, record);
        return true;
    }
    const int full = FullCount(id);
    capacity = SingleUse(id) ? 0 : full;
    uint16_t slotId = 0;
    if (!actors::InGame() && CopiesSlot(playerIndex, slotId, rounds) && slotId == id) {  // the Organiser screen
        count = rounds;
        return true;
    }
    LoadSlotGunRounds();
    const auto it = g_slotGunRounds[playerIndex].find(id);
    count = it != g_slotGunRounds[playerIndex].end() ? it->second : full;
    return true;
}

// knife_icon.cpp: the rounds of the knife slot's gun (false when the slot holds no firearm).
bool KnifeSlotGunAmmo(int playerIndex, int& count, int& capacity) {
    uintptr_t player = 0;
    const uintptr_t record = SlotGunRecord(playerIndex, &player);
    int32_t rounds = 0;
    if (!record || !mem::Read(record + 8, rounds)) return false;
    count = rounds;
    uint32_t id = 0;
    capacity = mem::Read(record + 4, id) && SingleUse(static_cast<uint16_t>(id)) ? 0 : SlotCapacity(player, record);
    return true;
}

bool KnifeSlotEmpty(int playerIndex) {
    if (playerIndex < 0 || playerIndex > 1) return false;
    return ActivePlaceEmpty(playerIndex) ||
           (g_sheathActive[playerIndex].load() == 0 && NoKnifeChoice(g_knife[playerIndex].choice));
}

// knife_icon.cpp ("Durabilité du couteau", the inventory's knives): record k of a player if in use, a knife's id, the
// grid of the item in hand (0xD none).
uintptr_t ItemRecordAddress(uintptr_t player, int k) {
    if (k < 0 || k >= kItemRecords || !HasItemManager(player)) return 0;
    const uintptr_t record = player + kItemManager + 4 + 0x30 * k;
    uint8_t inUse = 0;
    return mem::Read(record + 0x29, inUse) && inUse ? record : 0;
}
bool IsKnifeItem(uint16_t id) { return IsKnife(id); }
uint32_t PlayerHeldGrid(uintptr_t player) { return HasItemManager(player) ? HeldGrid(player) : 0xD; }

// The knife from the inventory (grid 0-8) a player uses, its record (0 none): the item in hand if it is one, else the
// equipped weapon (record +0x1C == 1) if it is one - the knife's drawing changes the item in hand (user, 2026-10-27: with
// the slot "Rien", the inventory knife's draw 0x22 was let through, then put away as the slot's).
uintptr_t InventoryKnifeInUse(uintptr_t player) {
    const uint32_t held = PlayerHeldGrid(player);
    uintptr_t equipped = 0;
    for (int k = 0; k < kItemRecords; ++k) {
        const uintptr_t record = ItemRecordAddress(player, k);
        uint32_t grid = 0xD, id = 0, state = 0;
        if (!record || !mem::Read(record + 0x18, grid) || grid >= 9 || !mem::Read(record + 4, id) || !IsKnife(id & 0xFFFF))
            continue;
        if (grid == held) return record;
        if (mem::Read(record + 0x1C, state) && state == 1) equipped = record;
    }
    return equipped;
}

// A knife from the inventory in use: usable whatever the knife slot holds ("Rien" or a broken knife).
bool HoldsInventoryKnife(uintptr_t player) { return InventoryKnifeInUse(player) != 0; }

// knife_icon.cpp ("Durabilité du couteau"): a knife in the slot (not "Rien"), the slot's choice, the index of "Rien".
uint16_t KnifeSlotItem(int playerIndex);  // below
bool KnifeSlotHoldsKnife(int playerIndex) { return !KnifeSlotEmpty(playerIndex) && IsKnife(KnifeSlotItem(playerIndex)); }
int KnifeSlotChoice(int playerIndex) { return playerIndex >= 0 && playerIndex < 2 ? g_knife[playerIndex].choice : 0; }
int KnifeSlotNoneChoice() { return g_noneChoice; }

// knife_icon.cpp: the item in a player's knife slot (grid 9), 0 if none or "Rien" (its knife kept there is no knife:
// no icon, user 2026-10-27).
uint16_t KnifeSlotItem(int playerIndex) {
    const uintptr_t player = actors::Player(playerIndex);
    if (KnifeSlotEmpty(playerIndex) || !HasItemManager(player)) return 0;
    // A sheath change waiting for the knife in hand to be put away: the knife it brings (the icons show no broken second).
    if (g_knife[playerIndex].sheathPending) return KnifeChoiceItem(playerIndex, player);
    const int record = KnifeRecord(player);
    uint32_t id = 0;
    if (record < 0 || !mem::Read(player + kItemManager + 4 + 0x30 * record + 4, id)) return 0;
    return static_cast<uint16_t>(id);
}

// No knife to draw (its draws refused, put away, no knife parry nor rescue with it): the slot "Rien" or its knife broken
// ("Durabilité du couteau": it stays in the sheath, unusable; user 2026-10-27) - unless a knife of the inventory is in use.
bool KnifeUnusable(int playerIndex) {
    if (playerIndex < 0 || playerIndex > 1) return false;
    const uintptr_t player = actors::Player(playerIndex);
    return (KnifeSlotEmpty(playerIndex) || SheathKnifeBroken(playerIndex)) && !(player && HoldsInventoryKnife(player));
}

// knife_icon.cpp: the sheath's places (see g_sheathPlace): their number, the highlighted one, a place 1..'s weapon in
// the slot now.
int SheathPlaceCount() { return PlaceCount(); }
int SheathActivePlace(int playerIndex) { return playerIndex >= 0 && playerIndex < 2 ? g_sheathActive[playerIndex].load() : 0; }
bool SheathSecondActive(int playerIndex) { return playerIndex >= 0 && playerIndex < 2 && ActiveSecond(playerIndex) != 0; }

void ApplySheath(int playerIndex) {
    KnifeSlot& slot = g_knife[playerIndex];
    slot.mismatchSince = 0;
    slot.sheathPending = false;
    if (!actors::InGame() || !actors::Player(playerIndex)) return;  // applied when a level is loaded
    if (ApplyKnifeChoice(playerIndex, true)) return;
    slot.sheathPending = true;
    LOG("sheath of P%d: applied once the knife is put away", playerIndex + 1);
}

// A place 1..'s weapon (0 empties it), highlighted with `activate`.
bool SetSheathPlace(int playerIndex, int place, uint16_t id, bool activate) {
    if (playerIndex < 0 || playerIndex > 1 || place < 1 || place >= kMaxSheathPlaces) return false;
    g_sheathPlace[playerIndex][place] = id;
    if (activate) g_sheathActive[playerIndex] = place;
    ApplySheath(playerIndex);
    return true;
}

// The highlighted place (an empty one is an empty sheath: the "Rien" hooks installed for it).
bool SetSheathActivePlace(int playerIndex, int place) {
    if (playerIndex < 0 || playerIndex > 1 || place < 0 || place >= PlaceCount()) return false;
    if (g_sheathActive[playerIndex].load() == place) return true;
    if (place > 0 && !g_sheathPlace[playerIndex][place].load() && (!EnsurePlayerRequestHook() || !EnsureModelStateHook()))
        return false;
    g_sheathActive[playerIndex] = place;
    ApplySheath(playerIndex);
    LOG("sheath of P%d: place %d highlighted (%04X)", playerIndex + 1, place + 1,
        place > 0 ? g_sheathPlace[playerIndex][place].load() : 0);
    return true;
}

// The file's state at load (no apply: the places' sets do).
void SetSheathActiveAtLoad(int playerIndex, int place) {
    if (playerIndex >= 0 && playerIndex < 2 && place >= 0 && place < kMaxSheathPlaces) g_sheathActive[playerIndex] = place;
}

// "Étui : nombre d'emplacements" (user, 2026-11-05: "a slider choosing between 1 and 5, it must work for all five"). The
// weapons of the places taken away stay there, hidden (knife_durability.ini keeps all five, the rounds are kept per
// weapon), and come back with them; a highlighted place taken away gives the main one back.
void SetPlaceCount(int count) {
    count = std::clamp(count, 1, kMaxSheathPlaces);
    if (g_sheathPlaceCount.exchange(count) != count) LOG("sheath: %d place(s)", count);
    for (int i = 0; i < 2; ++i)
        if (g_sheathActive[i].load() >= count) SetSheathActivePlace(i, 0);
}

// The character's own knife, also without a player (the Organiser screen between chapters: the profile's character,
// as FixKnifeCopies reads it).
uint16_t OwnKnifeOf(int playerIndex) {
    if (const uintptr_t player = actors::Player(playerIndex)) return OwnKnife(player);
    uintptr_t profile = 0;
    uint32_t character = 0;
    if (mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2158)), profile) && profile)
        mem::Read(profile + 0x23318 + 0x50 * playerIndex, character);
    return character == 1 ? 0x010E : 0x0101;
}

// knife_icon.cpp (the sheaths on the inventory screen): the sheath's main item - the choice's weapon, else the knife
// the slot holds ("Par défaut"), the character's own while a second knife is there; 0 for "Rien".
uint16_t SheathMainItem(int playerIndex) {
    if (playerIndex < 0 || playerIndex > 1 || NoKnifeChoice(g_knife[playerIndex].choice)) return 0;
    const int choice = g_knife[playerIndex].choice;
    if (const uint16_t weapon = ChoiceWeapon(choice)) return weapon;
    if (g_sheathActive[playerIndex].load() == 0 && actors::Player(playerIndex)) return KnifeSlotItem(playerIndex);
    return OwnKnifeOf(playerIndex);
}

// knife_icon.cpp: the item of a place (0 the main one; 0 for an empty place or "Rien").
uint16_t SheathPlaceItem(int playerIndex, int place) {
    if (playerIndex < 0 || playerIndex > 1 || place < 0 || place >= kMaxSheathPlaces) return 0;
    return place == 0 ? SheathMainItem(playerIndex) : g_sheathPlace[playerIndex][place].load();
}

// knife_icon.cpp: the main knife the slot's choice puts there (its weapon, else the character's own knife), 0 for
// "Rien" - not the knife the slot holds now (a second one may be there).
uint16_t SheathOwnMain(int playerIndex) {
    if (playerIndex < 0 || playerIndex > 1 || NoKnifeChoice(g_knife[playerIndex].choice)) return 0;
    const int choice = g_knife[playerIndex].choice;
    if (const uint16_t weapon = ChoiceWeapon(choice)) return weapon;
    return OwnKnifeOf(playerIndex);
}

// A move refused in the inventory (user, 2026-11-05: "the refusal sound of a full inventory and a message"): the
// message and the menu's refusal sound (se_core00, id below), the shortcut's own "failed" message left out.
void PlayMenuSound(int id);  // knife_icon.cpp
constexpr int kMenuRefusedSound = 0x41;  // the Organiser's refusal (sound log, 2026-11-05; 0x3C is the cursor)
std::atomic<bool> g_sheathRefused{ false };

void RefuseInMenu(const char* text) {
    Toast(text, false);
    PlayMenuSound(kMenuRefusedSound);
    g_sheathRefused = true;
}

bool SheathRefused() { return g_sheathRefused.exchange(false); }

// The weapon of `record` (grid 0-8) out of the inventory and into the sheath's highlighted place (user, 2026-10-27 the
// knives as a second one; 2026-11-05 every weapon the knife slot takes - knives, stun rod, handguns, magnums, Hydra,
// two-handed and single-use ones refused there too - and "the wheel click swaps with the highlighted place"): the
// weapon that place held goes back into the freed cell (`swapped` its id, 0 none; `place` the place). The main place
// takes it as the menu's choice (its rounds kept: g_swapChoosing). A firearm keeps its rounds both ways (the slot's
// record while it is the one there, else the rounds kept for it).
// The weapon in hand into the sheath (user, 2026-11-10: the Jail Breaker refused as the equipped weapon, "yes, accept it"): it cannot
// leave the grid while the game holds its unit (P+0x2634), so another weapon of the grid is taken in hand first (cItemManager::Equip,
// RVA 0x84D480, and its motions, Player::SetWeaponMotion RVA 0x75A7A0, as the parry's knife goes back), and the move is made once the
// player's update holds that one (TickPendingSheath); the weapon that came back from the sheath is then taken in hand.
struct PendingSheath {
    int player = -1;
    uint16_t id = 0;
    uint32_t cell = 0xD;
    ULONGLONG since = 0;
};
PendingSheath g_pendingSheath;

bool EquipGrid(uintptr_t player, uint32_t grid) {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, uint32_t)>(mem::Rva(0x84D480))(reinterpret_cast<void*>(player + kItemManager),
                                                                                        nullptr, grid);
        reinterpret_cast<void(__fastcall*)(void*, void*, int, int)>(mem::Rva(0x75A7A0))(reinterpret_cast<void*>(player), nullptr, 0xE, 5);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool MoveKnifeToSheath(int playerIndex, uintptr_t record, uint16_t& swapped, int& place) {
    swapped = 0;
    place = playerIndex >= 0 && playerIndex < 2 ? g_sheathActive[playerIndex].load() : 0;
    const uintptr_t player = actors::Player(playerIndex);
    if (playerIndex < 0 || playerIndex > 1 || !HasItemManager(player)) return false;
    const uintptr_t first = player + kItemManager + 4;
    if (record < first || (record - first) % 0x30 != 0 || (record - first) / 0x30 >= kItemRecords) return false;
    uint32_t id = 0;
    uintptr_t unit = 0, held = 0;
    if (!mem::Read(record + 4, id) || !KnifeSlotWeapon(static_cast<uint16_t>(id & 0xFFFF))) return false;
    id &= 0xFFFF;
    mem::Read(record + 0x2C, unit);
    mem::Read(player + 0x2634, held);
    if (unit && unit == held) {
        // The weapon in hand: another weapon of the grid in hand first, the move once the game holds it (PendingSheath).
        uint32_t cell = 0xD, other = 0xFF;
        mem::Read(record + 0x18, cell);
        for (int k = 0; k < kItemRecords && other == 0xFF; ++k) {
            const uintptr_t r = first + 0x30 * k;
            uint8_t inUse = 0;
            uint32_t grid = 0xD, item = 0;
            if (r != record && mem::Read(r + 0x29, inUse) && inUse && mem::Read(r + 0x18, grid) && grid <= 8 && mem::Read(r + 4, item) &&
                (item & 0xFFFF) >= 0x0100 && (item & 0xFFFF) < 0x0140)
                other = grid;
        }
        if (other == 0xFF) {
            RefuseInMenu("Aucune autre arme à prendre en main pour ranger celle-ci dans l'étui");
            return false;
        }
        if (!EquipGrid(player, other)) return false;
        g_pendingSheath = { playerIndex, static_cast<uint16_t>(id), cell, GetTickCount64() };
        LOG("sheath of P%d: %04X in hand: the weapon of cell %u taken first, the move once it is in hand", playerIndex + 1, id, other);
        return true;
    }
    uint32_t cell = 0xD;
    int32_t rounds = 0;
    mem::Read(record + 0x18, cell);
    mem::Read(record + 8, rounds);
    int choiceIndex = 0;  // the main place: the weapon's place in the menu's list
    if (place == 0) {
        choiceIndex = ChoiceIndexOf(static_cast<uint16_t>(id));
        if (choiceIndex < 0) {
            RefuseInMenu("Cette arme ne peut pas aller dans l'étui principal");
            return false;
        }
    }
    const uint16_t old = place > 0 ? g_sheathPlace[playerIndex][place].load() : SheathOwnMain(playerIndex);
    // The rounds of the one leaving: the slot's record while it is the one there.
    int32_t oldRounds = -1;
    if (old && IsSheathFirearm(old) && KnifeSlotItem(playerIndex) == old) {
        const int k = KnifeRecord(player);
        if (k >= 0) mem::Read(player + kItemManager + 4 + 0x30 * k + 8, oldRounds);
    }
    if (!RemoveItem(player, static_cast<int>((record - first) / 0x30))) return false;
    if (old && cell < 9) {
        if (CallAddItem(player, old, -1, static_cast<int>(cell))) {
            swapped = old;
            if (IsSheathFirearm(old)) {
                uintptr_t back = 0;
                for (int k = 0; k < kItemRecords && !back; ++k) {
                    const uintptr_t r = player + kItemManager + 4 + 0x30 * k;
                    uint8_t inUse = 0;
                    uint32_t grid = 0xD, other = 0;
                    if (mem::Read(r + 0x29, inUse) && inUse && mem::Read(r + 0x18, grid) && grid == cell && mem::Read(r + 4, other) &&
                        (other & 0xFFFF) == old)
                        back = r;
                }
                if (oldRounds >= 0 && back) mem::Write<int32_t>(back + 8, oldRounds);
                else GiveBackRounds(playerIndex, old, back, "inventory");
            }
        } else {
            LOG("sheath of P%d: %04X of place %d could not go back to cell %u", playerIndex + 1, old, place + 1, cell);
        }
    }
    if (IsSheathFirearm(static_cast<uint16_t>(id))) KeepRounds(playerIndex, static_cast<uint16_t>(id), rounds);
    LOG("sheath of P%d: inventory weapon %04X moved into place %d (cell %u, %d rounds; %04X back there)", playerIndex + 1,
        id, place + 1, cell, rounds, swapped);
    if (place > 0) return SetSheathPlace(playerIndex, place, static_cast<uint16_t>(id), true);
    Feature* slot = Find(playerIndex == 0 ? "inventory.knife_slot_p1" : "inventory.knife_slot_p2");
    if (!slot) return false;
    g_swapChoosing = true;
    const bool chosen = Choose(*slot, choiceIndex);
    g_swapChoosing = false;
    return chosen;
}

// The move of a weapon that was in hand, once the game holds the other one (2 s at most).
void TickPendingSheath() {
    PendingSheath& pending = g_pendingSheath;
    if (pending.player < 0) return;
    const uintptr_t player = actors::InGame() ? actors::Player(pending.player) : 0;
    if (!player || GetTickCount64() - pending.since > 2000) {
        LOG("sheath of P%d: %04X not moved (no other weapon in hand in time)", pending.player + 1, pending.id);
        pending = {};
        return;
    }
    uintptr_t record = 0, unit = 0, held = 0;
    for (int k = 0; k < kItemRecords && !record; ++k) {
        const uintptr_t r = player + kItemManager + 4 + 0x30 * k;
        uint8_t inUse = 0;
        uint32_t grid = 0xD, item = 0;
        if (mem::Read(r + 0x29, inUse) && inUse && mem::Read(r + 0x18, grid) && grid == pending.cell && mem::Read(r + 4, item) &&
            (item & 0xFFFF) == pending.id)
            record = r;
    }
    if (!record) {
        pending = {};
        return;
    }
    mem::Read(record + 0x2C, unit);
    mem::Read(player + 0x2634, held);
    if (unit && unit == held) return;  // not yet
    const int who = pending.player;
    const uint32_t cell = pending.cell;
    pending = {};
    uint16_t swapped = 0;
    int place = 0;
    const bool moved = MoveKnifeToSheath(who, record, swapped, place);
    // The weapon that came back from the sheath into that cell in hand (a swap of hands).
    const bool back = moved && swapped && cell <= 8 && swapped >= 0x0100 && swapped < 0x0140 && EquipGrid(player, cell);
    LOG("sheath of P%d: the weapon that was in hand %s%s", who + 1, moved ? "moved into the sheath" : "not moved",
        back ? ", the one back from it in hand" : "");
}

// knife_icon.cpp: an item the sheath's places take.
bool SheathTakes(uint16_t id) { return KnifeSlotWeapon(id); }

// The highlighted place's weapon for an empty cell (user, 2026-11-05: "the wheel click on an empty cell: the sheath's
// weapon back into the inventory, the sheath emptied without throwing it away"): `item`, its `rounds` (-1 for no
// firearm) - the slot's record while it holds it (in a level), the copies' knife slot (the Organiser screen), else the
// rounds kept for it, else a full magazine - and the `place`. An empty place is refused with its message.
bool SheathPlaceOut(int playerIndex, uint16_t& item, int32_t& rounds, int& place) {
    item = 0;
    rounds = -1;
    place = 0;
    if (playerIndex < 0 || playerIndex > 1) return false;
    place = g_sheathActive[playerIndex].load();
    item = SheathPlaceItem(playerIndex, place);
    if (!item) {
        RefuseInMenu("L'emplacement surligné de l'étui est vide");
        return false;
    }
    if (IsSheathFirearm(item)) {
        uintptr_t player = 0;
        const uintptr_t record = SlotGunRecord(playerIndex, &player);
        uint32_t current = 0;
        uint16_t slotId = 0;
        int32_t count = 0;
        LoadSlotGunRounds();
        const auto kept = g_slotGunRounds[playerIndex].find(item);
        if (record && mem::Read(record + 4, current) && (current & 0xFFFF) == item && mem::Read(record + 8, count)) rounds = count;
        else if (!actors::InGame() && CopiesSlot(playerIndex, slotId, count) && slotId == item) rounds = count;
        else if (kept != g_slotGunRounds[playerIndex].end()) rounds = kept->second;
        else rounds = FullCount(item);
    }
    return true;
}

// That place emptied (place 1: the menu's "Rien"; an empty place is an empty sheath), its weapon's kept rounds forgotten
// (they went with it).
bool EmptySheathPlace(int playerIndex, int place, uint16_t item) {
    bool emptied = false;
    if (place > 0) {
        emptied = EnsurePlayerRequestHook() && EnsureModelStateHook() && SetSheathPlace(playerIndex, place, 0, false);
    } else if (Feature* slot = Find(playerIndex == 0 ? "inventory.knife_slot_p1" : "inventory.knife_slot_p2")) {
        emptied = Choose(*slot, KnifeSlotNoneChoice());
    }
    if (emptied) ForgetRounds(playerIndex, item);
    LOG("sheath of P%d: %04X of place %d taken out, the place %s", playerIndex + 1, item, place + 1,
        emptied ? "emptied" : "NOT emptied");
    return emptied;
}

// The Organiser's cells filled from the sheath (user, 2026-11-10: "the knife came out of the sheath but did not come into the inventory: I
// lost a knife moving it in the Organiser"): the game carried the cell to the next level without its id - the level start's checkpoint
// copy held in that cell an item of id 0, count 1, maximum 1, in use (the knife 0123 had been put there with a count and a maximum of 1).
// Each cell filled is noted while the screen shows it (CheckOrganiserCellsOut drops it once the cell holds something else); the next
// level's new player gets the weapon back in that cell (an id 0 record there taken away first), its rounds with it, and the copies (the
// level start's checkpoint) its record (TickSheathCheckpoint).
struct OrganiserCellOut {
    int player = 0;
    uint32_t cell = 0;
    uint16_t id = 0;
    int32_t rounds = -1;
};
std::mutex g_cellsOutLock;
std::vector<OrganiserCellOut> g_cellsOut;
std::atomic<ULONGLONG> g_cellsOutSeen{ 0 };           // the Organiser last seen holding them
std::atomic<bool> g_organiserSheathChanged{ false };  // a snapshot to take at once (TickSheathCheckpoint)

void NoteOrganiserCellOut(int playerIndex, uint32_t cell, uint16_t id, int32_t rounds) {
    {
        std::lock_guard<std::mutex> lock(g_cellsOutLock);
        std::erase_if(g_cellsOut, [&](const OrganiserCellOut& c) { return c.player == playerIndex && c.cell == cell; });
        g_cellsOut.push_back({ playerIndex, cell, id, rounds });
    }
    g_cellsOutSeen = GetTickCount64();
    g_organiserSheathChanged = true;
    LOG("sheath of P%d: %04X noted in the Organiser's cell %u (given back at the next level if the game loses it)", playerIndex + 1, id,
        cell);
}

void ForgetOrganiserCellOut(int playerIndex, uint32_t cell) {
    {
        std::lock_guard<std::mutex> lock(g_cellsOutLock);
        std::erase_if(g_cellsOut, [&](const OrganiserCellOut& c) { return c.player == playerIndex && c.cell == cell; });
    }
    g_organiserSheathChanged = true;
}

// knife_icon.cpp's TickOrganiser, the screen shown: its 18 cells' ids (Chris's 9, then Sheva's).
void CheckOrganiserCellsOut(const uint16_t* ids) {
    std::lock_guard<std::mutex> lock(g_cellsOutLock);
    if (g_cellsOut.empty()) return;
    g_cellsOutSeen = GetTickCount64();
    std::erase_if(g_cellsOut, [ids](const OrganiserCellOut& c) {
        const bool moved = c.player < 0 || c.player > 1 || c.cell > 8 || ids[c.player * 9 + c.cell] != c.id;
        if (moved) LOG("sheath of P%d: the Organiser's cell %u no longer holds %04X: not given back", c.player + 1, c.cell, c.id);
        return moved;
    });
}

// A new player of a level (its records there): the cells noted for it. An id 0 record in the cell is the lost weapon (taken away, the
// weapon put back); an empty cell only within 2 minutes of the Organiser (not after a save of another time loaded); another item: left.
bool GiveBackOrganiserCells(int playerIndex, uintptr_t player) {
    std::vector<OrganiserCellOut> mine;
    {
        std::lock_guard<std::mutex> lock(g_cellsOutLock);
        for (const OrganiserCellOut& c : g_cellsOut)
            if (c.player == playerIndex) mine.push_back(c);
        std::erase_if(g_cellsOut, [playerIndex](const OrganiserCellOut& c) { return c.player == playerIndex; });
    }
    if (mine.empty() || !HasItemManager(player)) return false;
    const bool recent = GetTickCount64() - g_cellsOutSeen.load() < 120000;
    bool any = false;
    for (const OrganiserCellOut& c : mine) {
        int at = -1;
        uint32_t id = 0;
        for (int k = 0; k < kItemRecords && at < 0; ++k) {
            const uintptr_t r = player + kItemManager + 4 + 0x30 * k;
            uint8_t inUse = 0;
            uint32_t grid = 0xD;
            if (mem::Read(r + 0x29, inUse) && inUse && mem::Read(r + 0x18, grid) && grid == c.cell && mem::Read(r + 4, id)) at = k;
        }
        id &= 0xFFFF;
        if (at >= 0 && id == c.id) {
            LOG("sheath of P%d: %04X came into cell %u with the game", playerIndex + 1, c.id, c.cell);
            continue;
        }
        if ((at >= 0 && id != 0) || (at < 0 && !recent)) {
            LOG("sheath of P%d: %04X not given back in cell %u (%s)", playerIndex + 1, c.id, c.cell,
                at >= 0 ? "another item there" : "empty, the Organiser too long ago");
            continue;
        }
        if (at >= 0 && !RemoveItem(player, at)) continue;  // the record left without its id
        if (!LoadItemResources(c.id) || !CallAddItem(player, c.id, -1, c.cell)) {
            LOG("sheath of P%d: %04X could not be given back in cell %u", playerIndex + 1, c.id, c.cell);
            continue;
        }
        for (int k = 0; k < kItemRecords; ++k) {
            const uintptr_t r = player + kItemManager + 4 + 0x30 * k;
            uint8_t inUse = 0;
            uint32_t grid = 0xD, item = 0;
            if (!mem::Read(r + 0x29, inUse) || !inUse || !mem::Read(r + 0x18, grid) || grid != c.cell || !mem::Read(r + 4, item) ||
                (item & 0xFFFF) != c.id)
                continue;
            if (c.rounds >= 0) mem::Write<int32_t>(r + 8, c.rounds);
            uint8_t entry[0x2C] = {};  // the copies' entry = the record's +0x04..+0x2F
            if (mem::SafeRead(reinterpret_cast<void*>(r + 4), entry, sizeof(entry)))
                for (uintptr_t base : InventoryCopies(playerIndex))
                    mem::SafeWrite(reinterpret_cast<void*>(base + 0x2C * c.cell), entry, sizeof(entry));
            break;
        }
        any = true;
        LOG("sheath of P%d: %04X given back in cell %u (%s, %d rounds): the game lost it on its way from the Organiser", playerIndex + 1,
            c.id, c.cell, at >= 0 ? "its id 0 record replaced" : "the cell was empty", c.rounds);
    }
    return any;
}

// ---- The sheath follows the checkpoints (user, 2026-11-05: "at a checkpoint restart the inventory goes back to the
// checkpoint but the sheath stays as it was when I quit: a shotgun picked up before the checkpoint and put in the
// sheath after it came back in the inventory and stayed in the sheath - two shotguns; save the sheath only at the
// saves and checkpoints") ----
// The game keeps the last checkpoint's inventory in its copies (InventoryCopies: all seven kept 11 / 10 rounds after two
// shots of each 92F) and rebuilds the inventory from them at a restart, a Continue or a death's retry. The sheath's
// state (the main place's choice, the places 2.., the highlighted one, the rounds kept, the knives' units) is
// snapshotted with the copies' hash when the copies change during play (a checkpoint) or on the Organiser screen;
// when a new player object comes with an inventory equal to the copies (the same items by grid), the snapshot is put
// back if its hash is theirs (a restart / Continue: the copies of that checkpoint), else taken (a level start whose
// checkpoint was written during the load). Kept in DataDir\sheath_checkpoint.ini (a quit and a Continue).
struct SheathSnapshot {
    bool valid = false;
    uint32_t hash = 0;
    int choice[2] = {};
    uint16_t place[2][kMaxSheathPlaces] = {};
    int active[2] = {};
    std::map<uint16_t, int32_t> rounds[2];
    float units[2][kMaxSheathPlaces] = {};
    float inv[2][9] = {};
};
SheathSnapshot g_checkpoint;

std::wstring CheckpointFile() { return logx::DataDir() + L"\\sheath_checkpoint.ini"; }

void SaveCheckpointSnapshot() {
    std::ofstream f(CheckpointFile(), std::ios::trunc);
    const SheathSnapshot& s = g_checkpoint;
    char line[256];
    std::snprintf(line, sizeof(line), "hash=%08X\n", s.hash);
    f << line;
    for (int i = 0; i < 2; ++i) {
        std::snprintf(line, sizeof(line), "p%d=%d,%d,%04X,%04X,%04X,%04X\n", i + 1, s.choice[i], s.active[i], s.place[i][1],
                      s.place[i][2], s.place[i][3], s.place[i][4]);
        f << line;
        std::snprintf(line, sizeof(line), "units_p%d=%.1f,%.1f,%.1f,%.1f,%.1f\n", i + 1, s.units[i][0], s.units[i][1],
                      s.units[i][2], s.units[i][3], s.units[i][4]);
        f << line;
        std::snprintf(line, sizeof(line), "inv_p%d=%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", i + 1, s.inv[i][0], s.inv[i][1],
                      s.inv[i][2], s.inv[i][3], s.inv[i][4], s.inv[i][5], s.inv[i][6], s.inv[i][7], s.inv[i][8]);
        f << line;
        for (const auto& [id, count] : s.rounds[i]) {
            std::snprintf(line, sizeof(line), "rounds_p%d=%04X,%d\n", i + 1, id, count);
            f << line;
        }
    }
}

void LoadCheckpointSnapshot() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    std::ifstream f(CheckpointFile());
    std::string text;
    SheathSnapshot s;
    bool hashed = false;
    while (std::getline(f, text)) {
        const char* line = text.c_str();
        unsigned hash = 0, a = 0, b = 0, c = 0, d = 0, id = 0;
        int player = 0, choice = 0, active = 0, count = 0;
        float u[9] = {};
        if (sscanf_s(line, "hash=%x", &hash) == 1) {
            s.hash = hash;
            hashed = true;
        } else if (sscanf_s(line, "p%d=%d,%d,%x,%x,%x,%x", &player, &choice, &active, &a, &b, &c, &d) == 7 && player >= 1 &&
                   player <= 2) {
            s.choice[player - 1] = choice;
            s.active[player - 1] = std::clamp(active, 0, kMaxSheathPlaces - 1);
            s.place[player - 1][1] = static_cast<uint16_t>(a);
            s.place[player - 1][2] = static_cast<uint16_t>(b);
            s.place[player - 1][3] = static_cast<uint16_t>(c);
            s.place[player - 1][4] = static_cast<uint16_t>(d);
        } else if (sscanf_s(line, "units_p%d=%f,%f,%f,%f,%f", &player, &u[0], &u[1], &u[2], &u[3], &u[4]) == 6 && player >= 1 &&
                   player <= 2) {
            for (int k = 0; k < kMaxSheathPlaces; ++k) s.units[player - 1][k] = u[k];
        } else if (sscanf_s(line, "inv_p%d=%f,%f,%f,%f,%f,%f,%f,%f,%f", &player, &u[0], &u[1], &u[2], &u[3], &u[4], &u[5], &u[6],
                            &u[7], &u[8]) == 10 &&
                   player >= 1 && player <= 2) {
            for (int k = 0; k < 9; ++k) s.inv[player - 1][k] = u[k];
        } else if (sscanf_s(line, "rounds_p%d=%x,%d", &player, &id, &count) == 3 && player >= 1 && player <= 2) {
            s.rounds[player - 1][static_cast<uint16_t>(id)] = count;
        }
    }
    s.valid = hashed;
    if (hashed) g_checkpoint = s;
}

// The content of both players' copies: the item and count of entries 0-9 (grids 0-8 and the knife slot), hashed
// (FNV-1a). Not their bytes: the game rewrites other fields of the copies (the equip states, VA 0x72A9F0) - at the
// quit of 20:13:51 (2026-11-05) the raw bytes changed with the same items, taken for a checkpoint.
uint32_t CopiesHash() {
    uint32_t hash = 2166136261u;
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        const std::vector<uintptr_t> copies = InventoryCopies(i);
        for (int k = 0; k < 10; ++k) {
            uint32_t id = 0;
            int32_t count = 0;
            uint8_t inUse = 0;
            if (!copies.empty() && mem::Read(copies[0] + 0x2C * k + 0x25, inUse) && inUse) {
                mem::Read(copies[0] + 0x2C * k, id);
                mem::Read(copies[0] + 0x2C * k + 4, count);
                any = true;
            }
            const uint32_t words[2] = { id & 0xFFFF, static_cast<uint32_t>(count) };
            for (uint32_t word : words)
                for (int b = 0; b < 4; ++b) hash = (hash ^ ((word >> (8 * b)) & 0xFF)) * 16777619u;
        }
    }
    return any ? (hash ? hash : 1) : 0;
}

// The player's items by grid (0-8) are the copies' (a rebuilt inventory).
bool InventoryMatchesCopies(int playerIndex, uintptr_t player) {
    const std::vector<uintptr_t> copies = InventoryCopies(playerIndex);
    if (copies.empty() || !HasItemManager(player)) return false;
    uint16_t copied[9] = {}, live[9] = {};
    for (int k = 0; k < 9; ++k) {
        uint32_t id = 0;
        uint8_t inUse = 0;
        if (mem::Read(copies[0] + 0x2C * k, id) && mem::Read(copies[0] + 0x2C * k + 0x25, inUse) && inUse)
            copied[k] = static_cast<uint16_t>(id);
    }
    for (int k = 0; k < kItemRecords; ++k) {
        const uintptr_t r = player + kItemManager + 4 + 0x30 * k;
        uint8_t inUse = 0;
        uint32_t grid = 0xD, id = 0;
        if (mem::Read(r + 0x29, inUse) && inUse && mem::Read(r + 0x18, grid) && grid < 9 && mem::Read(r + 4, id))
            live[grid] = static_cast<uint16_t>(id);
    }
    return std::memcmp(copied, live, sizeof(copied)) == 0;
}

bool HasRecords(uintptr_t player) {
    if (!HasItemManager(player)) return false;
    for (int k = 0; k < kItemRecords; ++k) {
        uint8_t inUse = 0;
        if (mem::Read(player + kItemManager + 4 + 0x30 * k + 0x29, inUse) && inUse) return true;
    }
    return false;
}

void TakeCheckpointSnapshot(uint32_t hash, const char* why) {
    SheathSnapshot s;
    s.valid = true;
    s.hash = hash;
    LoadSlotGunRounds();
    for (int i = 0; i < 2; ++i) {
        s.choice[i] = g_knife[i].choice;
        s.active[i] = g_sheathActive[i].load();
        for (int k = 1; k < kMaxSheathPlaces; ++k) s.place[i][k] = g_sheathPlace[i][k].load();
        s.rounds[i] = g_slotGunRounds[i];
        uintptr_t player = 0;
        uint32_t id = 0;
        int32_t count = 0;
        if (const uintptr_t record = SlotGunRecord(i, &player);
            record && mem::Read(record + 4, id) && mem::Read(record + 8, count))  // the slot's gun: its record
            s.rounds[i][static_cast<uint16_t>(id)] = count;
    }
    KnifeUnitsSnapshot(s.units, s.inv);
    g_checkpoint = s;
    SaveCheckpointSnapshot();
    LOG("sheath: snapshot at %s (copies %08X): P1 choice %d places %04X %04X %04X %04X active %d", why, hash, s.choice[0],
        s.place[0][1], s.place[0][2], s.place[0][3], s.place[0][4], s.active[0] + 1);
}

void RestoreCheckpointSnapshot(int i) {
    const SheathSnapshot& s = g_checkpoint;
    if (i < 0 || i > 1) return;
    LoadSlotGunRounds();
    g_slotGunRounds[i] = s.rounds[i];
    SaveSlotGunRounds();
    for (int k = 1; k < kMaxSheathPlaces; ++k) g_sheathPlace[i][k] = s.place[i][k];
    g_sheathActive[i] = s.active[i];
    if (g_knife[i].choice != s.choice[i])
        if (Feature* slot = Find(i == 0 ? "inventory.knife_slot_p1" : "inventory.knife_slot_p2")) {
            g_swapChoosing = true;  // its kept rounds stay
            Choose(*slot, s.choice[i]);
            g_swapChoosing = false;
        }
    if ((s.active[i] > 0 && !s.place[i][s.active[i]]) || NoKnifeChoice(s.choice[i])) {
        EnsurePlayerRequestHook();  // an empty sheath
        EnsureModelStateHook();
    }
    g_knife[i].mismatchSince = 0;
    g_knife[i].sheathPending = false;
    KnifeUnitsRestore(i, s.units, s.inv);
    LOG("sheath of P%d: back to the checkpoint's (choice %d, places %04X %04X %04X %04X, place %d highlighted)", i + 1,
        s.choice[i], s.place[i][1], s.place[i][2], s.place[i][3], s.place[i][4], s.active[i] + 1);
}

// The game saves its players for the checkpoint (a checkpoint, a save, a chapter's start): VA 0xC93630 (thiscall on its
// store, no argument) calls VA 0xC932D0(slot) for slots 0-3, which copies each player's health, place and inventory
// (cItemManager VA 0xC4B700 by grid) into the copies - found with a write trace on the profile's copy (2026-11-05:
// 0xC932D0 wrote it 6 times in 5 minutes of loads, a save and a checkpoint). Before it, a player just made is checked
// against the copies about to be overwritten (CheckNewPlayers); after it, the sheath is snapshotted. The copies' content
// alone missed a checkpoint whose inventory had not changed (the sheath had: two weapons thrown away came back).
using SavePlayersFn = void(__fastcall*)(void* store, void* edx);
SavePlayersFn g_savePlayers = nullptr;
uintptr_t g_sheathSeen[2] = {};
uint32_t g_sheathLastHash = 0;
bool g_sheathStarted = false;

// The players made since the last look, once their inventory is there: the snapshot back if their inventory is the
// copies' and the snapshot is of these copies (a restart, a Continue, a death's retry).
void CheckNewPlayers() {
    const bool inGame = actors::InGame();
    for (int i = 0; i < 2; ++i) {
        const uintptr_t p = inGame ? actors::Player(i) : 0;
        if (!p) {
            g_sheathSeen[i] = 0;
            continue;
        }
        if (p == g_sheathSeen[i] || !HasRecords(p)) continue;
        g_sheathSeen[i] = p;
        const uint32_t hash = CopiesHash();
        if (!InventoryMatchesCopies(i, p)) {
            LOG("sheath of P%d: new player, inventory not the copies' (the sheath kept)", i + 1);
        } else if (g_checkpoint.valid && g_checkpoint.hash == hash) {
            RestoreCheckpointSnapshot(i);
        } else {
            LOG("sheath of P%d: new player, no snapshot of these copies (%08X; the snapshot's %08X): the sheath kept", i + 1,
                hash, g_checkpoint.hash);
        }
        g_sheathLastHash = hash;
    }
}

void __fastcall HookSavePlayers(void* store, void* edx) {
    const bool story = g_sheathStarted && KnifeStoryMode() && SheathFileApplied();
    if (story) CheckNewPlayers();
    g_savePlayers(store, edx);
    if (!story) return;
    const uint32_t hash = CopiesHash();
    g_sheathLastHash = hash;
    if (actors::InGame() && actors::Player(0)) TakeCheckpointSnapshot(hash, "a save");
}

bool EnsureSavePlayersHook() {
    static int state = 0;  // 0 not tried, 1 hooked, 2 refused
    if (state == 0) {
        const uint8_t expected[] = { 0x56, 0x57, 0x8B, 0xF9, 0x33, 0xF6, 0x56, 0x8B, 0xCF, 0xE8 };
        state = HookGame(0x893630, expected, sizeof(expected), reinterpret_cast<void*>(&HookSavePlayers),
                         reinterpret_cast<void**>(&g_savePlayers), "checkpoint save (sheath)")
                    ? 1
                    : 2;
    }
    return state == 1;
}

// Game thread, every frame (UpdateKnifeSlots). A snapshot is only taken once the places of the last quit are given
// back (SheathFileApplied: before, the sheath reads empty - the first try took one at the trainer's first frame).
void TickSheathCheckpoint() {
    static bool seedPending = false;
    static ULONGLONG startedAt = 0, lastHashed = 0;
    if (!KnifeStoryMode()) return;
    LoadCheckpointSnapshot();
    EnsureSavePlayersHook();
    const bool inGame = actors::InGame();
    const ULONGLONG now = GetTickCount64();
    if (!g_sheathStarted) {  // the trainer (re)loaded: the players and copies as they are, nothing put back
        g_sheathStarted = true;
        startedAt = now;
        g_sheathLastHash = CopiesHash();
        for (int i = 0; i < 2; ++i) g_sheathSeen[i] = inGame ? actors::Player(i) : 0;
        seedPending = !g_checkpoint.valid && g_sheathSeen[0] != 0;
        return;
    }
    // The first time (no snapshot yet), in a level whose inventory is still the checkpoint's: taken once the sheath is
    // what it was.
    if (seedPending && now - startedAt > 3000 && SheathFileApplied()) {
        seedPending = false;
        const uintptr_t p = inGame ? actors::Player(0) : 0;
        if (p && HasRecords(p) && InventoryMatchesCopies(0, p) && g_sheathLastHash)
            TakeCheckpointSnapshot(g_sheathLastHash, "the first run");
    }
    if (SheathFileApplied()) CheckNewPlayers();
    // The Organiser's cells the game lost on the way (GiveBackOrganiserCells), once a new player has its records; the checkpoint copies
    // changed with them, the snapshot follows.
    static uintptr_t cellsSeen[2] = {};
    for (int i = 0; i < 2; ++i) {
        const uintptr_t p = inGame ? actors::Player(i) : 0;
        if (!p) {
            cellsSeen[i] = 0;
            continue;
        }
        if (p == cellsSeen[i] || !HasRecords(p)) continue;
        cellsSeen[i] = p;
        if (GiveBackOrganiserCells(i, p) && actors::Player(0)) {
            const uint32_t hash = CopiesHash();
            g_sheathLastHash = hash;
            TakeCheckpointSnapshot(hash, "the Organiser's cells given back");
        }
    }
    // The Organiser screen opened on the copies of the snapshot (a save loaded from the title): its sheath back; then
    // its changes (its records and the knife slot written into the copies) snapshotted.
    static bool organiser = false;
    const bool organiserNow = !inGame && OrganiserOpen();
    if (organiserNow && !organiser) {
        const uint32_t hash = CopiesHash();
        if (g_checkpoint.valid && g_checkpoint.hash == hash) {
            RestoreCheckpointSnapshot(0);
            RestoreCheckpointSnapshot(1);
        } else {
            LOG("sheath: Organiser on copies %08X, the snapshot's %08X: the sheath kept", hash, g_checkpoint.hash);
        }
        g_sheathLastHash = hash;
    }
    organiser = organiserNow;
    // A sheath change of the trainer's on the screen (user, 2026-11-10: "the knife moved out of the sheath was not saved and came back into
    // the sheath when I resumed": a weapon out into a cell changes the copies only when the game carries the screen over, so no snapshot
    // was taken and the last one, the weapon in the sheath, was put back at the level): snapshotted at once.
    if (organiserNow && g_organiserSheathChanged.exchange(false) && SheathFileApplied()) {
        const uint32_t hash = CopiesHash();
        if (hash) {
            g_sheathLastHash = hash;
            TakeCheckpointSnapshot(hash, "the Organiser (a sheath change)");
        }
    }
    if (!organiserNow || now - lastHashed < 200) return;
    lastHashed = now;
    const uint32_t hash = CopiesHash();
    if (!hash || hash == g_sheathLastHash || !SheathFileApplied()) return;
    g_sheathLastHash = hash;
    TakeCheckpointSnapshot(hash, "the Organiser");
}

// In a level: the highlighted place's weapon into the player's empty cell `cell` (0-8), with its rounds.
bool SheathWeaponIntoCell(int playerIndex, uint32_t cell, uint16_t& item, int& place) {
    const uintptr_t player = actors::Player(playerIndex);
    int32_t rounds = -1;
    if (!HasItemManager(player) || cell > 8 || !SheathPlaceOut(playerIndex, item, rounds, place)) return false;
    if (!LoadItemResources(item) || !CallAddItem(player, item, -1, static_cast<int>(cell))) {
        LOG("sheath of P%d: %04X could not go into cell %u", playerIndex + 1, item, cell);
        return false;
    }
    if (rounds >= 0)
        for (int k = 0; k < kItemRecords; ++k) {
            const uintptr_t r = player + kItemManager + 4 + 0x30 * k;
            uint8_t inUse = 0;
            uint32_t grid = 0xD, id = 0;
            if (mem::Read(r + 0x29, inUse) && inUse && mem::Read(r + 0x18, grid) && grid == cell && mem::Read(r + 4, id) &&
                (id & 0xFFFF) == item) {
                mem::Write<int32_t>(r + 8, rounds);
                break;
            }
        }
    return EmptySheathPlace(playerIndex, place, item);
}

// The Organiser screen between chapters (knife_icon.cpp, user 2026-11-05: "the trainer's sheath shortcuts there too"):
// the firearm the sheath holds in use (the copies' knife slot) keeps its rounds before another place is highlighted
// (in a level, PutInKnifeSlot does it).
void KeepSlotCopiesRounds(int playerIndex) {
    uint16_t id = 0;
    int32_t count = 0;
    if (playerIndex < 0 || playerIndex > 1 || actors::InGame() || !CopiesSlot(playerIndex, id, count) || !IsSheathFirearm(id) ||
        !KnifeSlotWeapon(id))
        return;
    KeepRounds(playerIndex, id, count);
    LOG("sheath of P%d: %04X keeps its %d rounds (Organiser)", playerIndex + 1, id, count);
}

// The Organiser's weapon `id` (its `rounds` for a firearm) into the highlighted place of the sheath; `old` the weapon
// that place held, for the cell (0 none), with its rounds (`oldRounds`, -1 for no firearm): the copies' slot count
// while it is the one there, else its kept rounds, else a full magazine. The places as MoveKnifeToSheath sets them.
bool OrganiserIntoSheath(int playerIndex, uint16_t id, int32_t rounds, uint16_t& old, int32_t& oldRounds, int& place) {
    old = 0;
    oldRounds = -1;
    place = 0;
    if (playerIndex < 0 || playerIndex > 1 || !KnifeSlotWeapon(id)) return false;
    place = g_sheathActive[playerIndex].load();
    int choiceIndex = 0;
    if (place == 0) {
        choiceIndex = ChoiceIndexOf(id);
        if (choiceIndex < 0) {
            RefuseInMenu("Cette arme ne peut pas aller dans l'étui principal");
            return false;
        }
    }
    old = place > 0 ? g_sheathPlace[playerIndex][place].load() : SheathOwnMain(playerIndex);
    if (old && IsSheathFirearm(old)) {
        uint16_t slotId = 0;
        int32_t slotCount = 0;
        LoadSlotGunRounds();
        const auto kept = g_slotGunRounds[playerIndex].find(old);
        if (CopiesSlot(playerIndex, slotId, slotCount) && slotId == old) oldRounds = slotCount;
        else if (kept != g_slotGunRounds[playerIndex].end()) oldRounds = kept->second;
        else oldRounds = FullCount(old);
        ForgetRounds(playerIndex, old);
    }
    if (IsSheathFirearm(id)) KeepRounds(playerIndex, id, rounds);
    LOG("sheath of P%d: Organiser weapon %04X (%d rounds) into place %d, %04X (%d rounds) out", playerIndex + 1, id, rounds,
        place + 1, old, oldRounds);
    if (place > 0) return SetSheathPlace(playerIndex, place, id, true);
    Feature* slot = Find(playerIndex == 0 ? "inventory.knife_slot_p1" : "inventory.knife_slot_p2");
    if (!slot) return false;
    g_swapChoosing = true;
    const bool chosen = Choose(*slot, choiceIndex);
    g_swapChoosing = false;
    return chosen;
}

// "Jeter l'arme de l'étui surligné" (user, 2026-11-05: "the discard only throws what is in the inventory, nothing of the
// sheath"): the highlighted place emptied (the main place's choice set to "Rien"), the weapon gone with its kept rounds;
// knife_icon.cpp gives the place full units for the next one. Its place returned in `place`.
bool DiscardSheathPlace(int playerIndex, int& place) {
    place = playerIndex >= 0 && playerIndex < 2 ? g_sheathActive[playerIndex].load() : 0;
    const uint16_t item = SheathPlaceItem(playerIndex, place);
    if (!item) {
        RefuseInMenu("L'emplacement surligné de l'étui est vide");
        return false;
    }
    if (place > 0) {
        if (!EnsurePlayerRequestHook() || !EnsureModelStateHook()) return false;  // an empty sheath now
        SetSheathPlace(playerIndex, place, 0, false);
    } else {
        Feature* slot = Find(playerIndex == 0 ? "inventory.knife_slot_p1" : "inventory.knife_slot_p2");
        if (!slot || !Choose(*slot, KnifeSlotNoneChoice())) return false;
    }
    ForgetRounds(playerIndex, item);
    LOG("sheath of P%d: %04X of place %d thrown away", playerIndex + 1, item, place + 1);
    return true;
}

bool KnifeSlotEmptyRefuses(uintptr_t player, int action) {
    if (action != 0x22 && action != 0x25 && action != 0x27) return false;
    const int i = actors::PlayerIndex(player);
    // The sheath's own draw (the knife aim puts the slot's grid 9 into P+0x2DD8 before its request, RVA 0x77857A) from an
    // empty or broken sheath: refused even with a knife of the inventory equipped (user, 2026-11-05: its exception let the
    // slot's hidden knife be drawn, put away at once, drawn again - a loop).
    uint32_t asked = 0xD;
    if ((action == 0x22 || action == 0x25) && i >= 0 && (KnifeSlotEmpty(i) || SheathKnifeBroken(i)) &&
        mem::Read(player + 0x2DD8, asked) && asked == 9)
        return true;
    // A sheath change waiting for the weapon in hand to be put away (user, 2026-11-05: from the knife to a firearm in the
    // knife aim, the aim key held drew the old knife again at once, put away again, for 3 s before the change got in):
    // the old weapon is not drawn again; the change comes in the next frame, then the aim draws the new one.
    if ((action == 0x22 || action == 0x25) && i >= 0 && i < 2 && g_knife[i].sheathPending && mem::Read(player + 0x2DD8, asked) &&
        asked == 9)
        return true;
    return KnifeUnusable(i);
}
std::string ItemDisplayName(uint16_t id) { return ItemLabel(id); }

void RegisterInventory() {
    std::vector<std::string> choices{ "—" };
    for (const ItemName& item : AllItems())
        if (Storable(item.id)) {
            g_giveIds.push_back(item.id);
            choices.push_back(std::string(item.category) + " : " + DisplayName(item.id, item.name));
        }
    g_giveChoices = choices;
    // The sheath's options too (user, 2026-11-05: "the sheath is an inventory slot - a handgun goes in it": they were
    // in Combat > Couteau).
    AddPanel("Éditeur d'inventaire", DrawInventoryEditor, { "Inventaire", "Couteau : étui" }, true);
    Add({ .id = "inventory.give_choice", .name = "Objet à donner",
          .help = "Choisis un objet, puis « Donner l'objet choisi » (joueur 1 ou 2). Le jeu l'ajoute comme s'il était "
                  "ramassé, dans la première case libre. Une arme que le niveau n'a pas encore chargée arrive au "
                  "point de passage : Pause > Redémarrer pour la recevoir.",
          .category = "Inventaire", .kind = Kind::Choice,
          .choose = [](int index) {
              g_giveChoice = index;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = static_cast<float>(choices.size() - 1), .valueFormat = "%.0f",
          .choices = choices });
    for (int player = 0; player < 2; ++player) {
        static const char* ids[2] = { "inventory.give_p1", "inventory.give_p2" };
        static const char* names[2] = { "Donner l'objet choisi — J1", "Donner l'objet choisi — J2" };
        Feature& give = Add({ .id = ids[player], .name = names[player],
                              .help = "Ajoute l'objet choisi au-dessus. Curseur = quantité (0 = quantité normale de l'objet).",
                              .category = "Inventaire", .kind = Kind::Action,
                              .available = actors::InGame,
                              .value = 0, .valueMin = 0, .valueMax = 999, .valueFormat = "%.0f", .valueStep = 1 });
        Feature* self = &give;
        give.run = [player, self] {
            if (g_giveChoice <= 0 || g_giveChoice > static_cast<int>(g_giveIds.size())) {
                Toast("Choisis d'abord un objet à donner", false);
                return false;
            }
            return GiveItem(player, g_giveIds[g_giveChoice - 1], static_cast<int>(self->value));
        };
    }
    for (int player = 0; player < 2; ++player) {
        static const char* ids[2] = { "inventory.remove_p1", "inventory.remove_p2" };
        static const char* names[2] = { "Retirer l'objet choisi — J1", "Retirer l'objet choisi — J2" };
        Add({ .id = ids[player], .name = names[player],
              .help = "Retire de l'inventaire l'objet choisi dans « Objet à donner » (une case). Jamais l'arme tenue en "
                      "main : change d'arme avant.",
              .category = "Inventaire", .kind = Kind::Action,
              .run = [player] {
                  if (g_giveChoice <= 0 || g_giveChoice > static_cast<int>(g_giveIds.size())) {
                      Toast("Choisis d'abord un objet", false);
                      return false;
                  }
                  return RemoveItemById(player, g_giveIds[g_giveChoice - 1]);
              },
              .available = actors::InGame });
    }
    // The list of 2026-10-13, then "Rien", then the weapons allowed since (2026-11-05): the indices saved in settings.ini
    // keep their weapon (NoKnifeChoice).
    std::vector<std::string> knifeChoices{ "Par défaut" };
    g_knifeIds.assign(1, 0);
    for (int pass = 0; pass < 2; ++pass) {
        for (const ItemName& item : AllItems())
            if (KnifeSlotWeapon(item.id) && FirstKnifeSlotWeapon(item.id) == (pass == 0)) {
                g_knifeIds.push_back(item.id);
                knifeChoices.push_back(DisplayName(item.id, item.name));
            }
        if (pass == 0) {
            g_noneChoice = static_cast<int>(g_knifeIds.size());
            g_knifeIds.push_back(0);
            knifeChoices.push_back("Rien");
        }
    }
    for (int player = 0; player < 2; ++player) {
        static const char* ids[2] = { "inventory.knife_slot_p1", "inventory.knife_slot_p2" };
        static const char* names[2] = { "Emplacement couteau — J1", "Emplacement couteau — J2" };
        Add({ .id = ids[player], .name = names[player],
              .help = "Met une autre arme dans l'emplacement spécial du couteau (sa touche rapide), sans prendre de case : "
                      "couteaux, matraque électrique, armes à feu, lanceurs, grenades, mine et œufs (une arme à usage "
                      "unique épuisée vide son emplacement). Réappliqué après chaque chargement. « Par défaut » remet "
                      "le couteau du personnage. « Rien » vide l'emplacement : le personnage ne sort plus de couteau.\n"
                      "Attention : le choix est enregistré dans la sauvegarde. Certains chapitres (3-3) réclament le "
                      "couteau d'origine au chargement et le jeu se ferme s'il manque : le trainer le garde chargé, "
                      "mais il doit être injecté AVANT de lancer le chapitre (à l'écran titre). Sans le trainer, "
                      "remettre « Par défaut » et sauvegarder avant.",
              .category = "Couteau : étui", .kind = Kind::Choice,
              .available = [] { return game::IsSupported(); },
              .choose = [player](int index) {
                  // A weapon chosen here is a new one, full (user, 2026-11-05): the rounds kept for that gun out of the
                  // slot (sheath switches) are forgotten - not at the settings' restore (a reload must keep them).
                  if (!g_swapChoosing && g_knife[player].chosen && g_knife[player].choice != index && ChoiceWeapon(index))
                      ForgetRounds(player, ChoiceWeapon(index));
                  g_knife[player].chosen = true;
                  g_knife[player].choice = index;
                  g_knife[player].mismatchSince = 0;
                  if (index != 0) KeepOwnKnivesLoaded();
                  if (NoKnifeChoice(index) && (!EnsurePlayerRequestHook() || !EnsureModelStateHook())) return false;
                  if (!actors::InGame()) return true;  // applied when a level is loaded
                  // Not possible right now (its weapon in hand): kept, UpdateKnifeSlots applies it once it is (user,
                  // 2026-10-27: "refused" shown, the list left on the old choice, and the slot changed 3 s later).
                  if (!ApplyKnifeChoice(player))
                      LOG("knife slot of P%d: choice %d applied once possible (retried every 3 s)", player + 1, index);
                  return true;
              },
              .value = 0, .valueMin = 0, .valueMax = static_cast<float>(knifeChoices.size() - 1), .valueFormat = "%.0f",
              .choices = knifeChoices });
    }
    Feature& places = Add({ .id = "inventory.sheath_places", .name = "Étui : nombre d'emplacements",
          .help = "Nombre d'emplacements de l'étui, de 1 à 5 (décochée : 1, l'étui du jeu). « Changer de couteau dans "
                  "l'étui » passe de l'un à l'autre, « Mettre l'arme sélectionnée dans l'étui » échange avec celui qui est "
                  "surligné. Les armes des emplacements retirés y restent, cachées, et reviennent quand on les remet ; si "
                  "l'emplacement surligné est retiré, l'emplacement 1 reprend.",
          .category = "Couteau : étui", .kind = Kind::Toggle,
          .available = [] { return game::IsSupported(); },
          .value = 3, .valueMin = 1, .valueMax = static_cast<float>(kMaxSheathPlaces), .valueFormat = "%.0f",
          .valueStep = 1, .defaultOn = true });
    Feature* placesFeature = &places;
    places.set = [placesFeature](bool on) {
        SetPlaceCount(on ? static_cast<int>(std::lround(placesFeature->value)) : 1);
        return true;
    };
    places.tick = [placesFeature] {
        // Whole numbers only: the slider snaps to them.
        const int count = std::clamp(static_cast<int>(std::lround(placesFeature->value)), 1, kMaxSheathPlaces);
        placesFeature->value = static_cast<float>(count);
        SetPlaceCount(count);
    };
    Add({ .id = "inventory.knife_slot_infinite_p1", .name = "Emplacement couteau : munitions infinies — J1",
          .help = "Munitions infinies pour l'arme de l'emplacement couteau seulement : elle ne consomme pas de balles et sa "
                  "culasse ne se bloque jamais. Décochée, elle se vide : plus de tir à chargeur vide, et la touche de "
                  "rechargement en visant au couteau la recharge (avec les balles de l'inventaire, sauf avec « Recharger "
                  "sans réserve »).",
          .category = "Couteau : étui",
          .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureAmmoInfiniteHook()) return false;
              g_knife[0].infinite = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnFrame(UpdateKnifeSlots);
    OnFrame(TickPendingSheath);
    Add({ .id = "inventory.trade_weapons", .name = "Échanger les armes avec le partenaire",
          .help = "Les armes peuvent se donner, se demander et s'échanger avec le partenaire comme les autres objets "
                  "(normalement « Donner » est grisé pour une arme). Joueurs 1 et 2.",
          .category = "Inventaire", .kind = Kind::Toggle,
          .set = [](bool on) { return on ? mem::ApplyAll({ &g_tradeWeapons }) : mem::RestoreAll({ &g_tradeWeapons }); },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "inventory.trade_anywhere", .name = "Donner et demander à distance",
          .help = "« Donner » n'est plus grisé quand le partenaire est loin (normalement il faut être à moins de 3 m et le "
                  "voir), et un objet demandé au partenaire est donné tout de suite, sans qu'il vienne jusqu'à toi. "
                  "Joueurs 1 et 2.",
          .category = "Inventaire", .kind = Kind::Toggle,
          .set = [](bool on) {
              return on ? mem::ApplyAll({ &g_giveMenuRange, &g_giveButtonRange })
                        : mem::RestoreAll({ &g_giveButtonRange, &g_giveMenuRange });
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "inventory.trade_no_anim", .name = "Échanges sans animation",
          .help = "Donner un objet au partenaire (ou lui en demander un) se fait tout de suite : plus d'animation de don, "
                  "l'objet passe d'un inventaire à l'autre avec le message du jeu. Joueurs 1 et 2. Pas en Mercenaries.",
          .category = "Inventaire", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureGiveHandlerHook()) return false;
              g_tradeNoAnim = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "inventory.discard_selected", .name = "Jeter l'objet sélectionné",
          .help = "Inventaire ouvert : le raccourci jette l'objet surligné, comme « Jeter » dans le menu de l'objet (le "
                  "personnage le prend en main puis le jette) : de ton inventaire par toi, de celui du partenaire par le "
                  "partenaire. Inventaire fermé, le raccourci ne fait rien.",
          .category = "Inventaire", .kind = Kind::Action,
          .run = [] {
              if (!Player1InventoryOpen() || !EnsurePadQueryHooks()) return false;
              g_discardAsked = true;
              return true;
          },
          .available = [] { return game::IsSupported() && actors::InGame(); },
          .hotkeyIgnored = [] { return !Player1InventoryOpen(); } });
    Add({ .id = "inventory.reunion_icons", .name = "Icônes des armes de Reunion en campagne",
          .help = "En campagne, le jeu charge l'interface du HUD sans les icônes des armes de Mercenaries Reunion (le Samurai "
                  "Edge de Barry n'a pas d'icône dans l'inventaire). Cochée, le trainer charge l'archive de Reunion et le "
                  "jeu prend l'interface de Reunion (la même, avec ces icônes en plus). Prend effet au prochain chargement "
                  "(Pause > Redémarrer au dernier point de passage).",
          .category = "Inventaire", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (!on) return mem::RestoreAll({ &g_dcGuiAlways, &g_dcGuiByMode });
              return LoadReunionResources() && mem::ApplyAll({ &g_dcGuiByMode, &g_dcGuiAlways });
          },
          .available = [] { return game::IsSupported(); } });

    for (const ItemName& item : AllItems()) {
        const uintptr_t info = ItemInfo(item.id), field = StackField(item.id);
        uint8_t inventory = 0, stacks = 0;
        uint32_t max = 0;
        if (info && field && Storable(item.id) && !CombinableHerb(item.id) && mem::Read(info + 0x1C, inventory) && inventory &&
            mem::Read(info + 0x1F, stacks) && stacks && mem::Read(field, max) && max) {
            g_stackIds.push_back(item.id);
            g_stackLabels.push_back(std::string(item.category) + " : " + DisplayName(item.id, item.name));
        }
    }
    LoadStacks();
    OnFrame(UpdateStacks);
    OnRestore(RestoreStacks);

    {
        static const char* ids[3] = { "inventory.knife_variant_x", "inventory.knife_variant_y", "inventory.knife_variant_z" };
        static const char* names[3] = { "Étui du couteau (variante) : axe X", "Étui du couteau (variante) : axe Y",
                                         "Étui du couteau (variante) : axe Z" };
        for (int axis = 0; axis < 3; ++axis) {
            Feature& f = Add({ .id = ids[axis], .name = names[axis],
                               .help = "Déplace l'étui rangé quand l'emplacement couteau du joueur 1 contient une variante "
                                       "de couteau (Chris ou Sheva). Le couteau en main garde sa place. Effet immédiat.",
                               .category = "Couteau : étui", .kind = Kind::Toggle,
                               .set = [axis](bool on) {
                                   if (!on) g_variantSheath[axis] = 0.0f;
                                   return true;
                               },
                               .value = 0, .valueMin = -30, .valueMax = 30, .valueFormat = "%.1f", .defaultOn = true });
            Feature* self = &f;
            f.tick = [axis, self] { g_variantSheath[axis] = self->value; };
        }
    }
    // Ultimate Trainer "Give RPG".
    Add({ .id = "inventory.give_rpg_p1", .name = "Donner un lance-roquettes — J1",
          .help = "Ajoute un lance-roquettes (1 roquette) au joueur 1 : tout de suite s'il est déjà chargé dans le "
                  "niveau, sinon au point de passage (Pause > Redémarrer).",
          .category = "Inventaire", .kind = Kind::Action, .run = [] { return GiveItem(0, 0x010D, -1); },
          .available = actors::InGame });
}
}
