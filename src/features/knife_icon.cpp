// "Icône de l'étui à côté du nom" (user, 2026-10-27): the item in each player's knife slot (Chris's knife by Chris,
// Sheva's by Sheva; a handgun put there shows the handgun) drawn next to the player's name on the HUD; player 2's
// left out while "Joueur 2 invisible" hides its HUD.
// - The icons are the HUD's own (cockpit GUI cp_life_360 / cp_life_360_dc, rGUI XFS: MODLOG session 23): sprite "wpXX"
//   for the weapon 0x01XX, a rectangle of id\jpn\cockpit\cp05_360_NOMIP_BM (cp26 for Barry's Samurai Edge, Reunion's
//   GUI only), shown 80x80 in the ring. The texture is asked of sResource::Load (RVA 0x48B70, rTexture DTI RVA
//   0xE4A7E4; already loaded by the HUD, so its reference count only), its IDirect3DTexture9 at [+0xCC]+0x10.
// - The layout is the GUI's, on a 1280x720 screen: player 1's name centred at (1141, 641), 80x31, player 2's at (1042,
//   474), anchored at the bottom right and scaled by the screen (min of width / 1280 and height / 720: the health ring
//   measured at 2048x1280 for the stamina ring fits it); the whole HUD mirrored for a player 1 left-handed by the game.
// "Durabilité du couteau" (user, 2026-10-27): each player's knife starts at the slider's units, shown above its icon;
// each knife hit that lands on an enemy (enemy OnHit, knife flag, its attacker; knife.cpp) and each knife parry of
// player 1 whose riposte is the knife slash (not Wesker's guard, Jill's throw nor the RE6 counter; parry.cpp) takes off
// its slider's units. At 0 the knife slot becomes "Rien" (inventory.cpp: no knife drawn, empty sheath, no knife rescue:
// action_log.cpp); "Réparer le couteau" gives both knives their units back and the knife that was there. The units and
// the broken knives' choices are kept in DataDir\knife_durability.ini.
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "common/wav.hpp"

#include <windows.h>
#include <d3d9.h>
#include <imgui.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace features {
uint16_t KnifeSlotItem(int playerIndex);   // inventory.cpp
bool KnifeSlotGunAmmo(int playerIndex, int& count, int& capacity);  // inventory.cpp: a firearm in the knife slot
bool SheathGunRounds(int playerIndex, uint16_t id, int& count, int& capacity);  // inventory.cpp: in or out of the slot
bool SheathTakes(uint16_t id);                                  // inventory.cpp: an item the sheath's second place takes
bool SheathRefused();                                           // inventory.cpp: the last move refused with its message
bool DiscardSheathPlace(int playerIndex, int& place);           // inventory.cpp: the highlighted place emptied
bool EnsureEnemyDamageHook();              // knife.cpp: the knives' power is applied in it
bool KnifeSlotHoldsKnife(int playerIndex); // inventory.cpp: a knife there, not "Rien"
int KnifeSlotChoice(int playerIndex);      // inventory.cpp
int KnifeSlotNoneChoice();                 // inventory.cpp: the index of "Rien"
bool Player1RingOnLeft();                  // player.cpp: the HUD mirrored
bool PlayerCharacterLeftHanded(int index); // player.cpp: the inventory screen shows his grid on the right
bool ChamberPlusOneShown(int index);       // fire_rate.cpp: player 1's (0) / the partner's (1) count shows the magazine
bool Player2Invisible();                   // player.cpp: "Joueur 2 invisible" (its HUD hidden)
bool Player1HudShown();                    // player.cpp: the health ring on screen
bool EnsureCockpitWatch();                 // player.cpp
bool EnsureEnemyOnHitHook();               // knife.cpp: the knife hits (KnifeWearHit)
uintptr_t ItemRecordAddress(uintptr_t player, int k);  // inventory.cpp: record k in use, 0 otherwise
bool IsKnifeItem(uint16_t id);                         // inventory.cpp
uint32_t PlayerHeldGrid(uintptr_t player);             // inventory.cpp: the grid of the item in hand, 0xD none
uintptr_t InventoryKnifeInUse(uintptr_t player);       // inventory.cpp: the inventory knife in hand or equipped
uint16_t SheathPlaceItem(int playerIndex, int place);   // inventory.cpp: a place's item (0 the main one), 0 empty
int SheathPlaceCount();                                 // inventory.cpp: the sheath's places
int SheathActivePlace(int playerIndex);                 // inventory.cpp: the highlighted place, the one in the slot
bool SetSheathPlace(int playerIndex, int place, uint16_t id, bool activate);  // inventory.cpp: a place 1..
bool SetSheathActivePlace(int playerIndex, int place);  // inventory.cpp
void SetSheathActiveAtLoad(int playerIndex, int place); // inventory.cpp
uint16_t SheathMainItem(int playerIndex);               // inventory.cpp: the sheath's main item, 0 for "Rien"
uint16_t SheathOwnMain(int playerIndex);                // inventory.cpp: the main knife the choice puts in the slot
bool MoveKnifeToSheath(int playerIndex, uintptr_t record, uint16_t& swapped, int& place);  // inventory.cpp
bool SheathSecondActive(int playerIndex);               // inventory.cpp: a place 1..'s weapon is the one in the slot
bool Player1InventoryOpen();                           // pickup.cpp
float KnifeResistance(uint16_t item);                  // fire_rate.cpp: a knife's units ("Résistance" in the weapon table)
std::string ItemDisplayName(uint16_t id);              // inventory.cpp
void RefuseInMenu(const char* text);                   // inventory.cpp: a refusal's message and the menu's refusal sound
bool OrganiserIntoSheath(int playerIndex, uint16_t id, int32_t rounds, uint16_t& old, int32_t& oldRounds, int& place);
void KeepSlotCopiesRounds(int playerIndex);            // inventory.cpp: the copies' slot gun keeps its rounds
bool SheathPlaceOut(int playerIndex, uint16_t& item, int32_t& rounds, int& place);  // inventory.cpp: for an empty cell
bool EmptySheathPlace(int playerIndex, int place, uint16_t item);                  // inventory.cpp
bool SheathWeaponIntoCell(int playerIndex, uint32_t cell, uint16_t& item, int& place);  // inventory.cpp: in a level
void NoteOrganiserCellOut(int playerIndex, uint32_t cell, uint16_t id, int32_t rounds);  // inventory.cpp: a cell filled from the sheath
void ForgetOrganiserCellOut(int playerIndex, uint32_t cell);                            // inventory.cpp
void CheckOrganiserCellsOut(const uint16_t* ids);                                       // inventory.cpp: the screen's 18 cells
bool LoadWeaponArchive(const char* name);              // inventory.cpp: Image\Archive\<name>, kept loaded

namespace {
struct Icon {
    uint8_t low;      // the weapon 0x01XX
    uint8_t texture;  // kTextures
    int16_t x0, y0, x1, y1;
};
constexpr const char* kTextures[] = { "id\\jpn\\cockpit\\cp05_360_NOMIP_BM", "id\\jpn\\cockpit\\cp26_360_NOMIP_BM",
                                      "id\\jpn\\cockpit\\cp03_360_NOMIP_BM" };
constexpr int kDigitsTexture = 2;
// The HUD's digits number_00..number_09 (cp03), 24 x 32.
constexpr int16_t kDigits[10][2] = { { 96, 209 }, { 0, 177 }, { 24, 177 }, { 48, 177 }, { 72, 177 },
                                     { 96, 177 }, { 0, 209 }, { 24, 209 }, { 48, 209 }, { 72, 209 } };
constexpr Icon kIcons[] = {
    { 0x01, 0, 408, 102, 510, 204 }, { 0x02, 0, 408, 0, 510, 102 }, { 0x03, 0, 816, 204, 918, 306 }, { 0x04, 0, 510, 0, 612, 102 },
    { 0x05, 0, 714, 0, 816, 102 }, { 0x06, 0, 102, 204, 204, 306 }, { 0x07, 0, 306, 204, 408, 306 }, { 0x08, 0, 204, 204, 306, 306 },
    { 0x09, 0, 612, 0, 714, 102 }, { 0x0A, 0, 306, 408, 408, 510 }, { 0x0B, 0, 612, 102, 714, 204 }, { 0x0C, 0, 714, 102, 816, 204 },
    { 0x0D, 0, 408, 204, 510, 306 }, { 0x0E, 0, 306, 306, 408, 408 }, { 0x0F, 0, 510, 102, 612, 204 }, { 0x10, 0, 816, 0, 918, 102 },
    { 0x11, 0, 816, 102, 918, 204 }, { 0x12, 1, 0, 0, 102, 102 }, { 0x13, 0, 918, 204, 1020, 306 }, { 0x15, 0, 612, 204, 714, 306 },
    { 0x16, 0, 816, 306, 918, 408 }, { 0x17, 0, 918, 102, 1020, 204 }, { 0x18, 0, 918, 612, 1020, 714 }, { 0x19, 0, 918, 306, 1020, 408 },
    { 0x1A, 0, 816, 510, 918, 612 }, { 0x1B, 0, 918, 408, 1020, 510 }, { 0x1C, 0, 816, 408, 918, 510 }, { 0x1D, 0, 816, 612, 918, 714 },
    { 0x1E, 0, 102, 510, 204, 612 }, { 0x1F, 0, 918, 0, 1020, 102 }, { 0x20, 0, 918, 510, 1020, 612 }, { 0x21, 0, 510, 714, 612, 816 },
    { 0x22, 0, 612, 714, 714, 816 }, { 0x23, 0, 204, 714, 306, 816 }, { 0x24, 0, 306, 714, 408, 816 }, { 0x25, 0, 204, 102, 306, 204 },
    { 0x26, 0, 306, 102, 408, 204 }, { 0x27, 0, 0, 204, 102, 306 }, { 0x2C, 0, 714, 918, 816, 1020 }, { 0x2D, 0, 306, 918, 408, 1020 },
    { 0x2E, 0, 408, 918, 510, 1020 }, { 0x2F, 0, 612, 918, 714, 1020 }, { 0x30, 0, 0, 306, 102, 408 }, { 0x34, 0, 408, 714, 510, 816 },
    { 0x35, 0, 714, 714, 816, 816 }, { 0x36, 0, 510, 816, 612, 918 }, { 0x39, 0, 510, 204, 612, 306 }, { 0x3A, 0, 204, 306, 306, 408 },
    { 0x3B, 0, 714, 306, 816, 408 }, { 0x3C, 0, 612, 816, 714, 918 }, { 0x3D, 0, 714, 816, 816, 918 }, { 0x3E, 0, 816, 816, 918, 918 },
};

// Where each icon goes on the 1280x720 screen: its centre and size, beside the name (player 1's to its right, at the
// screen's edge; player 2's to its left, its ring being on the right).
struct Place {
    float x, y, size;
};
constexpr Place kPlaces[2] = { { 1207.0f, 641.0f, 40.0f }, { 980.0f, 474.0f, 36.0f } };

using ResLoadFn = void*(__fastcall*)(void* mgr, void* edx, void* type, const char* path, unsigned flags);
using ResReleaseFn = void(__fastcall*)(void* res, void* edx);

std::atomic<bool> g_on{ false };
struct Texture {
    void* resource = nullptr;
    DWORD triedAt = 0;
    bool logged = false;
};
Texture g_textures[std::size(kTextures)];  // render thread

const Icon* IconOf(uint16_t item) {
    if ((item >> 8) != 0x01) return nullptr;
    for (const Icon& icon : kIcons)
        if (icon.low == (item & 0xFF)) return &icon;
    return nullptr;
}

void* LoadTexture(const char* path) {
    void* mgr = nullptr;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xE34580)), mgr) || !mgr) return nullptr;
    void* res = nullptr;
    __try {
        res = reinterpret_cast<ResLoadFn>(mem::Rva(0x48B70))(mgr, nullptr, mem::Rva(0xE4A7E4), path, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        res = nullptr;
    }
    uint32_t state = 0;
    if (res && (!mem::Read(reinterpret_cast<uintptr_t>(res) + 0x4C, state) || !(state & 1) || (state & 0x10))) {
        __try {
            reinterpret_cast<ResReleaseFn>(mem::Rva(0x4F3E0))(res, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        LOG("knife icon: %s not loaded (state %08X)", path, state);
        return nullptr;
    }
    return res;
}

// A Direct3D texture: its vtable in a module other than the game's, and its type a texture.
IDirect3DTexture9* AsTexture(uintptr_t candidate) {
    uintptr_t vtable = 0;
    HMODULE module = nullptr;
    if (!candidate || !mem::Read(candidate, vtable) || !vtable ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(vtable), &module) ||
        module == GetModuleHandleA(nullptr))
        return nullptr;
    auto* texture = reinterpret_cast<IDirect3DTexture9*>(candidate);
    __try {
        return texture->GetType() == D3DRTYPE_TEXTURE ? texture : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

IDirect3DTexture9* TextureOf(int index) {
    Texture& t = g_textures[index];
    if (!t.resource) {
        const DWORD now = GetTickCount();
        if (t.triedAt && now - t.triedAt < 3000) return nullptr;
        t.triedAt = now ? now : 1;
        t.resource = LoadTexture(kTextures[index]);
        if (!t.resource) return nullptr;
    }
    // rTexture +0xCC = the engine's texture (made by its loader VA 0x4A2490, stored at VA 0x4A2AA5; vtable VA 0x10B3FE4,
    // "DXT5" at +0x24, size +0x48 / +0x4C), whose +0x10 is the IDirect3DTexture9 (read live 2026-10-27).
    uintptr_t engine = 0, pointer = 0;
    IDirect3DTexture9* texture = nullptr;
    if (mem::Read(reinterpret_cast<uintptr_t>(t.resource) + 0xCC, engine) && engine && mem::Read(engine + 0x10, pointer))
        texture = AsTexture(pointer);
    if (!t.logged) LOG("knife icon: %s at %p, its Direct3D texture %p", kTextures[index], t.resource, texture);
    t.logged = true;
    return texture;
}

// A place of the 1280x720 layout on the screen, anchored at the bottom right (bottom left mirrored).
ImVec2 OnScreen(float x, float y, float scale, bool mirrored, const ImVec2& screen) {
    return ImVec2(mirrored ? (1280.0f - x) * scale : screen.x - (1280.0f - x) * scale, screen.y - (720.0f - y) * scale);
}

// An item's HUD icon ("wpXX") centred at `c`, `sizePx` wide.
void DrawItemIcon(ImDrawList* dl, uint16_t item, const ImVec2& c, float sizePx, float alpha) {
    const Icon* icon = IconOf(item);
    if (!icon) return;
    IDirect3DTexture9* texture = TextureOf(icon->texture);
    D3DSURFACE_DESC desc{};
    if (!texture || FAILED(texture->GetLevelDesc(0, &desc)) || !desc.Width || !desc.Height) return;
    const float half = sizePx * 0.5f;
    const ImVec2 uv0(icon->x0 / static_cast<float>(desc.Width), icon->y0 / static_cast<float>(desc.Height));
    const ImVec2 uv1(icon->x1 / static_cast<float>(desc.Width), icon->y1 / static_cast<float>(desc.Height));
    dl->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture))), ImVec2(c.x - half, c.y - half),
                 ImVec2(c.x + half, c.y + half), uv0, uv1,
                 IM_COL32(255, 255, 255, static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)));
}

// ---- Durability ----
std::atomic<bool> g_durabilityOn{ false };
std::atomic<float> g_durabilityMax{ 100.0f }, g_wearHit{ 1.0f }, g_wearParry{ 5.0f };
// RE6's knife moves (user, 2026-11-10: "the counters, throat cuts and knife Grounds use no durability: a slider for each"): the knife
// counter, the throat cut, the knife Ground; worn once per move at its first blow (wesker_moves.cpp).
std::atomic<float> g_wearRe6Move[3] = { 5.0f, 5.0f, 3.0f };
// Each knife its own units (user, 2026-10-28: "en fonction du type de couteau", Sheva's 50, Chris's 120; since
// 2026-11-05 the "Résistance" column of the weapon table, fire_rate.cpp, with power and the rest).
constexpr uint16_t kKnifeTypes[4] = { 0x0101, 0x010E, 0x0123, 0x0124 };

int KnifeType(uint16_t item) {
    for (int k = 0; k < 4; ++k)
        if (item == kKnifeTypes[k]) return k;
    return -1;
}

// The units of a full knife of that item.
float MaxUnits(uint16_t item) {
    return KnifeType(item) >= 0 ? KnifeResistance(item) : g_durabilityMax.load(std::memory_order_relaxed);
}
// The sheath of each player: its main knife (the knife slot's choice, broken it stays there unusable: user, 2026-10-27)
// and the knives of its other places (inventory.cpp g_sheathPlace: moved in from the inventory; 2026-11-05 up to five
// places, the highlighted one in the slot).
constexpr int kSheathPlaces = 5;
struct Knife {
    std::atomic<float> units{ -1.0f };  // the main knife; -1 until worn (the maximum)
    std::atomic<int> brokenFrom{ -1 };  // a knife slot left "Rien" by an older break, its choice before, -1 none
    std::atomic<bool> breakPending{ false };
    std::atomic<float> extraUnits[kSheathPlaces]{ { -1.0f }, { -1.0f }, { -1.0f }, { -1.0f }, { -1.0f } };  // places 1..
    std::atomic<bool> extraBreakPending[kSheathPlaces]{};
    uint16_t loadedPlace[kSheathPlaces] = {};  // the file's places 1.., given back once
    int loadedActive = -1;               // the file's highlighted place
};
Knife g_knives[2];
std::atomic<bool> g_durabilityDirty{ false };
const char* const kSlotIds[2] = { "inventory.knife_slot_p1", "inventory.knife_slot_p2" };

// The inventory's knives (user, 2026-10-27: a knife added to the inventory, not the slot, has its own durability; hits
// with it wear it, not the slot's, and the reverse): each in-use record of grid 0-8 holding a knife, followed by its
// record (moving it in the grid keeps it); a new one is full. A level left (the player made again): their units go by
// grid to the next level's knives, and to the file.
struct InventoryKnife {
    uintptr_t record;
    uint16_t id;
    uint32_t grid;
    float units;  // -1 = full
};
std::mutex g_invLock;
std::vector<InventoryKnife> g_invKnives[2];
uintptr_t g_invPlayer[2] = {};
float g_savedInv[2][9];  // units by grid waiting for the knives of a new level, -2 none
std::vector<std::pair<uint16_t, float>> g_parked;  // knives put in the item box on the Organiser screen (below)

// Mercenaries (user, 2026-10-27: "the story's second knife was still there; every Mercenaries round starts with the
// main knife only"): the story's knives (units, second knives, the inventory's) are kept aside while Mercenaries plays,
// each round starts fresh (main knife full, no second one) and nothing of it comes back to the story or the file.
struct StoryKnives {
    float units[2] = { -1.0f, -1.0f };
    float placeUnits[2][kSheathPlaces] = { { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f }, { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f } };
    uint16_t place[2][kSheathPlaces] = {};
    int active[2] = {};
    float savedInv[2][9] = {};
};
StoryKnives g_story;     // render thread
bool g_inMerc = false;   // render thread
uintptr_t g_mercPlayer = 0;
// The round under way, written to the file too (user, 2026-10-28: a hot reload in a round forgot it, the second knife
// left in the slot taken for the main one): given back to a new instance in the same round (the same player 1).
StoryKnives g_mercSaved;
uint32_t g_mercSavedPlayer = 0;

std::wstring DurabilityFile() { return logx::DataDir() + L"\\knife_durability.ini"; }

void LoadDurability() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    for (auto& player : g_savedInv)
        for (float& units : player) units = -2.0f;
    for (auto& player : g_mercSaved.savedInv)
        for (float& units : player) units = -2.0f;
    std::ifstream f(DurabilityFile());
    std::string line;
    float units[2] = { -1.0f, -1.0f };
    int from[2] = { -1, -1 };
    while (std::getline(f, line)) {
        sscanf_s(line.c_str(), "durability_p1=%f", &units[0]);
        sscanf_s(line.c_str(), "durability_p2=%f", &units[1]);
        sscanf_s(line.c_str(), "broken_from_p1=%d", &from[0]);
        sscanf_s(line.c_str(), "broken_from_p2=%d", &from[1]);
        unsigned mercPlayer = 0;
        if (sscanf_s(line.c_str(), "merc_player=%x", &mercPlayer) == 1) g_mercSavedPlayer = mercPlayer;
        int mp = 0, mfirst = 0, mgrid = 0, mk = 0;
        unsigned msecond = 0;
        float munits = -1.0f, msecondUnits = -1.0f, minv = 0.0f;
        if (sscanf_s(line.c_str(), "merc_p%d=%f,%x,%f,%d", &mp, &munits, &msecond, &msecondUnits, &mfirst) == 5 && mp >= 1 &&
            mp <= 2) {  // the file of 2026-10-28: one second knife
            g_mercSaved.units[mp - 1] = munits;
            g_mercSaved.place[mp - 1][1] = static_cast<uint16_t>(msecond);
            g_mercSaved.placeUnits[mp - 1][1] = msecondUnits;
            g_mercSaved.active[mp - 1] = msecond && !mfirst ? 1 : 0;
        } else if (sscanf_s(line.c_str(), "merc_p%d=%f,%d", &mp, &munits, &mfirst) == 3 && mp >= 1 && mp <= 2) {
            g_mercSaved.units[mp - 1] = munits;
            g_mercSaved.active[mp - 1] = mfirst;
        }
        if (sscanf_s(line.c_str(), "merc_sheath%d_p%d=%x,%f", &mk, &mp, &msecond, &msecondUnits) == 4 && mp >= 1 && mp <= 2 &&
            mk >= 2 && mk <= kSheathPlaces) {
            g_mercSaved.place[mp - 1][mk - 1] = static_cast<uint16_t>(msecond);
            g_mercSaved.placeUnits[mp - 1][mk - 1] = msecondUnits;
        }
        if (sscanf_s(line.c_str(), "merc_inventory_p%d_grid%d=%f", &mp, &mgrid, &minv) == 3 && mp >= 1 && mp <= 2 && mgrid >= 0 &&
            mgrid < 9)
            g_mercSaved.savedInv[mp - 1][mgrid] = minv;
        int player = 0, grid = 0, k = 0, active = 0;
        unsigned second = 0;
        float secondUnits = -1.0f;
        int mainFirst = 0;
        // The file of 2026-10-27: one second knife and whether the main one was used.
        if (sscanf_s(line.c_str(), "main_first_p%d=%d", &player, &mainFirst) == 2 && player >= 1 && player <= 2)
            g_knives[player - 1].loadedActive = mainFirst ? 0 : 1;
        if (sscanf_s(line.c_str(), "second_p%d=%x,%f", &player, &second, &secondUnits) == 3 && player >= 1 && player <= 2) {
            g_knives[player - 1].loadedPlace[1] = static_cast<uint16_t>(second);
            g_knives[player - 1].extraUnits[1] = secondUnits;
            if (g_knives[player - 1].loadedActive < 0) g_knives[player - 1].loadedActive = 1;
        }
        // Since 2026-11-05: "sheathN_pP=ITEM,UNITS" for the places 2.. (shown numbers), "sheath_active_pP=N" (from 1).
        if (sscanf_s(line.c_str(), "sheath%d_p%d=%x,%f", &k, &player, &second, &secondUnits) == 4 && player >= 1 &&
            player <= 2 && k >= 2 && k <= kSheathPlaces) {
            g_knives[player - 1].loadedPlace[k - 1] = static_cast<uint16_t>(second);
            g_knives[player - 1].extraUnits[k - 1] = secondUnits;
        }
        if (sscanf_s(line.c_str(), "sheath_active_p%d=%d", &player, &active) == 2 && player >= 1 && player <= 2 && active >= 1 &&
            active <= kSheathPlaces)
            g_knives[player - 1].loadedActive = active - 1;
        float inv = 0.0f;
        if (sscanf_s(line.c_str(), "inventory_p%d_grid%d=%f", &player, &grid, &inv) == 3 && player >= 1 && player <= 2 &&
            grid >= 0 && grid < 9)
            g_savedInv[player - 1][grid] = inv;
        unsigned boxed = 0;
        float boxedUnits = -1.0f;
        if (sscanf_s(line.c_str(), "item_box_knife=%x,%f", &boxed, &boxedUnits) == 2)
            g_parked.emplace_back(static_cast<uint16_t>(boxed), boxedUnits);
    }
    for (int i = 0; i < 2; ++i) {
        g_knives[i].units = units[i];
        g_knives[i].brokenFrom = from[i];
    }
}

// The places as the file keeps them: those of the story while Mercenaries plays, else the live ones (the file's own
// until they are given back).
void StoryPlaces(int i, uint16_t (&place)[kSheathPlaces], float (&placeUnits)[kSheathPlaces], int& active) {
    const Knife& knife = g_knives[i];
    const bool loaded = std::any_of(std::begin(knife.loadedPlace), std::end(knife.loadedPlace), [](uint16_t id) { return id != 0; });
    for (int k = 1; k < kSheathPlaces; ++k) {
        place[k] = g_inMerc ? g_story.place[i][k] : loaded ? knife.loadedPlace[k] : SheathPlaceItem(i, k);
        placeUnits[k] = g_inMerc ? g_story.placeUnits[i][k] : knife.extraUnits[k].load();
    }
    active = g_inMerc ? g_story.active[i] : loaded && knife.loadedActive >= 0 ? knife.loadedActive : SheathActivePlace(i);
}

void SaveDurability() {
    const bool merc = g_inMerc;
    float units[2];
    for (int i = 0; i < 2; ++i) units[i] = merc ? g_story.units[i] : g_knives[i].units.load();
    std::ofstream f(DurabilityFile(), std::ios::trunc);
    char text[160];
    std::snprintf(text, sizeof(text), "durability_p1=%.1f\ndurability_p2=%.1f\nbroken_from_p1=%d\nbroken_from_p2=%d\n",
                  units[0], units[1], g_knives[0].brokenFrom.load(), g_knives[1].brokenFrom.load());
    f << text;
    for (int i = 0; i < 2; ++i) {
        uint16_t place[kSheathPlaces] = {};
        float placeUnits[kSheathPlaces] = {};
        int active = 0;
        StoryPlaces(i, place, placeUnits, active);
        for (int k = 1; k < kSheathPlaces; ++k)
            if (place[k]) {
                std::snprintf(text, sizeof(text), "sheath%d_p%d=%04X,%.1f\n", k + 1, i + 1, place[k], placeUnits[k]);
                f << text;
            }
        std::snprintf(text, sizeof(text), "sheath_active_p%d=%d\n", i + 1, active + 1);
        f << text;
    }
    if (merc) {  // the round under way, for a reload in it
        std::snprintf(text, sizeof(text), "merc_player=%X\n", static_cast<unsigned>(g_mercPlayer));
        f << text;
        for (int i = 0; i < 2; ++i) {
            std::snprintf(text, sizeof(text), "merc_p%d=%.1f,%d\n", i + 1, g_knives[i].units.load(), SheathActivePlace(i));
            f << text;
            for (int k = 1; k < kSheathPlaces; ++k)
                if (const uint16_t id = SheathPlaceItem(i, k)) {
                    std::snprintf(text, sizeof(text), "merc_sheath%d_p%d=%04X,%.1f\n", k + 1, i + 1, id,
                                  g_knives[i].extraUnits[k].load());
                    f << text;
                }
        }
    }
    std::lock_guard<std::mutex> lock(g_invLock);
    if (merc)
        for (int i = 0; i < 2; ++i) {
            float byGrid[9];
            std::copy(std::begin(g_savedInv[i]), std::end(g_savedInv[i]), byGrid);
            for (const InventoryKnife& knife : g_invKnives[i]) byGrid[knife.grid] = knife.units;
            for (int grid = 0; grid < 9; ++grid)
                if (byGrid[grid] >= -1.0f) {
                    std::snprintf(text, sizeof(text), "merc_inventory_p%d_grid%d=%.1f\n", i + 1, grid, byGrid[grid]);
                    f << text;
                }
        }
    for (int i = 0; i < 2; ++i) {
        float byGrid[9];
        if (merc) {
            std::copy(std::begin(g_story.savedInv[i]), std::end(g_story.savedInv[i]), byGrid);
        } else {
            std::copy(std::begin(g_savedInv[i]), std::end(g_savedInv[i]), byGrid);
            for (const InventoryKnife& knife : g_invKnives[i]) byGrid[knife.grid] = knife.units;
        }
        for (int grid = 0; grid < 9; ++grid)
            if (byGrid[grid] >= -1.0f) {
                std::snprintf(text, sizeof(text), "inventory_p%d_grid%d=%.1f\n", i + 1, grid, byGrid[grid]);
                f << text;
            }
    }
    for (const auto& knife : g_parked) {
        std::snprintf(text, sizeof(text), "item_box_knife=%04X,%.1f\n", knife.first, knife.second);
        f << text;
    }
}

float Units(float units, uint16_t item) {
    const float max = MaxUnits(item);
    return units < 0.0f ? max : std::min(units, max);
}

// The sheath's knife in use: the highlighted place's.
uint16_t SheathKnifeInUse(int player) { return SheathPlaceItem(player, SheathActivePlace(player)); }

// The units of a place's knife (-1 full).
std::atomic<float>& PlaceUnits(int player, int place) {
    Knife& knife = g_knives[player];
    return place > 0 && place < kSheathPlaces ? knife.extraUnits[place] : knife.units;
}

float Durability(int player) {
    return Units(PlaceUnits(player, SheathActivePlace(player)).load(std::memory_order_relaxed), SheathKnifeInUse(player));
}

// The inventory knife a player holds (its record), 0 when the knife in hand is the slot's or none.
uintptr_t HeldInventoryKnife(uintptr_t p) { return InventoryKnifeInUse(p); }

InventoryKnife* FindInventoryKnife(int player, uintptr_t record) {  // g_invLock held
    for (InventoryKnife& knife : g_invKnives[player])
        if (knife.record == record) return &knife;
    return nullptr;
}

// Render thread, every frame: the inventory's knives followed.
void ScanInventoryKnives() {
    for (int i = 0; i < 2; ++i) {
        const uintptr_t p = actors::InGame() ? actors::Player(i) : 0;
        std::lock_guard<std::mutex> lock(g_invLock);
        std::vector<InventoryKnife>& list = g_invKnives[i];
        if (p != g_invPlayer[i]) {  // a level left or a new player: the units wait by grid
            for (const InventoryKnife& knife : list) g_savedInv[i][knife.grid] = knife.units;
            list.clear();
            g_invPlayer[i] = p;
        }
        if (!p) continue;
        std::vector<InventoryKnife> next;
        for (int k = 0; k < 24; ++k) {
            const uintptr_t record = ItemRecordAddress(p, k);
            uint32_t grid = 0xD, id = 0;
            if (!record || !mem::Read(record + 0x18, grid) || grid >= 9 || !mem::Read(record + 4, id) ||
                !IsKnifeItem(static_cast<uint16_t>(id)))
                continue;
            float units = -1.0f;
            const InventoryKnife* old = FindInventoryKnife(i, record);
            if (old && old->id == static_cast<uint16_t>(id)) {
                units = old->units;
            } else {
                if (g_savedInv[i][grid] >= -1.0f) units = g_savedInv[i][grid];  // after a load
                g_savedInv[i][grid] = -2.0f;
                g_durabilityDirty = true;
                LOG("knife durability: player %d's inventory knife %04X in grid %u (%.0f units)", i + 1, id & 0xFFFF, grid,
                    Units(units, static_cast<uint16_t>(id)));
            }
            next.push_back({ record, static_cast<uint16_t>(id), grid, units });
        }
        if (next.size() != list.size()) g_durabilityDirty = true;
        list = std::move(next);
    }
}

// The Organiser screen between chapters (uIdItemBox00, vtable RVA 0xBA60A8; user, 2026-11-02: "the inventory knife has
// the M92F's icon and a count of 1"): the two players' pages are its records of 0x48 from +0x1678 (Chris's 9 cells,
// then Sheva's), each u16 id, u16 count, in the inventory's grid order. Its cells' drawing (VA 0xA06040..) is hooked
// below for the icon and the count; here, on the render thread, the knives moved on it keep their units: a knife gone
// from a cell gives them to a knife of its kind come into another, else they wait in the item box (g_parked, kept in
// the file) for one taken out of it. The units by cell go to g_savedInv, which the next level's knives take by grid.
constexpr uintptr_t kOrganiserVtable = 0xBA60A8;  // RVA
constexpr uintptr_t kOrganiserPages = 0x1678;
constexpr int kOrganiserCells = 18;
std::atomic<uintptr_t> g_organiserScreen{ 0 };  // the screen whose cells were last drawn, and when
std::atomic<DWORD> g_organiserSeen{ 0 };
std::atomic<float> g_organiserUnits[kOrganiserCells];  // the units shown, -1 full
struct Organiser {
    uintptr_t screen = 0;
    uint16_t ids[kOrganiserCells] = {};
    float units[kOrganiserCells] = {};
};
Organiser g_organiser;  // render thread

// The Organiser screen while it is shown, else 0. Its cells are not drawn every frame (a 300 ms window made the "+1"
// blink, and a knife's units were missing after a quiet second): known from its first cell drawn until its destructor
// (HookOrganiserDtor) - or, without that hook, 2 s after its last cell drawn -, opaque (+0x1B98 its opacity).
std::atomic<bool> g_organiserDtorHooked{ false };
uintptr_t OrganiserScreenNow() {
    const uintptr_t screen = g_organiserScreen.load();
    uintptr_t vtable = 0;
    uint8_t opacity = 0;
    if (!screen || actors::InGame() || (!g_organiserDtorHooked.load() && GetTickCount() - g_organiserSeen.load() > 2000) ||
        !mem::Read(screen, vtable) || vtable != reinterpret_cast<uintptr_t>(mem::Rva(kOrganiserVtable)) ||
        !mem::Read(screen + 0x1B98, opacity) || opacity < 128)
        return 0;
    return screen;
}

void TickOrganiser() {
    const uintptr_t screen = OrganiserScreenNow();
    if (!screen || g_inMerc) {
        g_organiser.screen = 0;
        return;
    }
    uint16_t now[kOrganiserCells] = {};
    for (int k = 0; k < kOrganiserCells; ++k) mem::Read(screen + kOrganiserPages + 0x48 * k, now[k]);
    CheckOrganiserCellsOut(now);
    std::lock_guard<std::mutex> lock(g_invLock);
    Organiser& o = g_organiser;
    if (o.screen != screen) {  // the screen opened: its knives' units by grid
        o.screen = screen;
        for (int k = 0; k < kOrganiserCells; ++k) {
            const float saved = g_savedInv[k / 9][k % 9];
            o.ids[k] = now[k];
            o.units[k] = IsKnifeItem(now[k]) ? (saved >= -1.0f ? saved : -1.0f) : -2.0f;
            if (IsKnifeItem(now[k]))
                LOG("knife durability: Organiser, player %d's cell %d holds knife %04X (%.0f units)", k / 9 + 1, k % 9, now[k],
                    Units(o.units[k], now[k]));
        }
    }
    std::vector<std::pair<uint16_t, float>> left;
    for (int k = 0; k < kOrganiserCells; ++k)
        if (now[k] != o.ids[k] && IsKnifeItem(o.ids[k])) left.emplace_back(o.ids[k], o.units[k]);
    const auto take = [](std::vector<std::pair<uint16_t, float>>& from, uint16_t id, float& units) {
        const auto it = std::find_if(from.begin(), from.end(), [id](const auto& knife) { return knife.first == id; });
        if (it == from.end()) return false;
        units = it->second;
        from.erase(it);
        return true;
    };
    for (int k = 0; k < kOrganiserCells; ++k) {
        if (now[k] == o.ids[k]) continue;
        float units = -2.0f;
        if (IsKnifeItem(now[k])) {
            units = -1.0f;  // a new knife is full
            if (!take(left, now[k], units)) take(g_parked, now[k], units);
            LOG("knife durability: Organiser, knife %04X into player %d's cell %d (%.0f units)", now[k], k / 9 + 1, k % 9,
                Units(units, now[k]));
        }
        o.ids[k] = now[k];
        o.units[k] = units;
        g_durabilityDirty = true;
    }
    for (const auto& knife : left) {
        g_parked.push_back(knife);
        LOG("knife durability: Organiser, knife %04X put in the item box (%.0f units)", knife.first,
            Units(knife.second, knife.first));
    }
    for (int k = 0; k < kOrganiserCells; ++k) {
        g_savedInv[k / 9][k % 9] = o.units[k];
        g_organiserUnits[k].store(o.units[k], std::memory_order_relaxed);
    }
}

// Player 1's cursor on the Organiser (recorded 2026-11-05 while the user moved it cell by cell): his side at +0x1B0
// (player 2's 0xA64 further), +0x00 its state (1 on a grid, 2 a sub-menu, 3 an item taken - +0x47 set, +0x4B its
// cell -, 5 the item box's list, +0x5C its line), +0x50 the page (0 Chris's grid, 1 Sheva's), +0x54 the cell (0-8 by rows).
constexpr uintptr_t kOrganiserSide = 0x1B0;
bool OrganiserCursor(uintptr_t screen, int& page, int& cell) {
    uint8_t state = 0;
    int32_t p = -1, c = -1;
    if (!mem::Read(screen + kOrganiserSide, state) || state != 1 || !mem::Read(screen + kOrganiserSide + 0x50, p) ||
        !mem::Read(screen + kOrganiserSide + 0x54, c) || p < 0 || p > 1 || c < 0 || c > 8)
        return false;
    page = p;
    cell = c;
    return true;
}

// A weapon's chambered round on the Organiser (OrganiserCount): its count moved left by kPlusOneShift for the "+1".
constexpr int kPlusOneShift = 18;  // layout units
int g_organiserShift = 0;          // read by OrganiserCountStub after each count
int OrganiserMax(uintptr_t screen, uintptr_t record);  // below

// The page of player 1's cursor (0 when it is in the item box's list).
int OrganiserCursorPage(uintptr_t screen) {
    int32_t page = 0;
    return mem::Read(screen + kOrganiserSide + 0x50, page) && page == 1 ? 1 : 0;
}

// "Son quand le couteau casse" (user, 2026-10-28: "plus de feed back" when the knife breaks in hand): the file
// sounds/knife_break.wav of DataDir (RE4 Remake's Play_WP5000_SE_BREAK of bank ch_wp5000, picked by ear by the user,
// tools/re_pak.py + tools/wwise_extract.py; RE6's snd_pl_cmn wave 8 before; never in git), at every break of a knife
// (the sheath's or the inventory's, either player), on the game thread that wore it.
wav::Sound g_breakSound;
std::atomic<bool> g_breakSoundOn{ false };

void PlayBreakSound() {
    if (g_breakSoundOn.load(std::memory_order_relaxed)) g_breakSound.Play();
}

void Wear(int player, float units, const char* what) {
    if (!g_durabilityOn.load(std::memory_order_relaxed) || player < 0 || player > 1 || units <= 0.0f) return;
    // The knife in hand from the inventory wears itself, not the slot's.
    const uintptr_t p = actors::Player(player);
    if (const uintptr_t record = p ? HeldInventoryKnife(p) : 0; record) {
        std::lock_guard<std::mutex> lock(g_invLock);
        InventoryKnife* knife = FindInventoryKnife(player, record);
        if (!knife) return;  // not seen yet (its first frame)
        const float before = Units(knife->units, knife->id), after = std::max(0.0f, before - units);
        knife->units = after;
        g_durabilityDirty = true;
        if (after <= 0.0f && before > 0.0f) {
            LOG("knife durability: player %d's inventory knife (grid %u) broken by a %s", player + 1, knife->grid, what);
            PlayBreakSound();
        }
        return;
    }
    if (!KnifeSlotHoldsKnife(player)) return;
    Knife& knife = g_knives[player];
    const int place = SheathActivePlace(player);
    const float before = Durability(player), after = std::max(0.0f, before - units);
    if (after <= 0.0f && before > 0.0f) {
        (place > 0 ? knife.extraBreakPending[place] : knife.breakPending) = true;  // told on the render thread (TickDurability)
        LOG("knife durability: player %d's knife of place %d broken by a %s", player + 1, place + 1, what);
        PlayBreakSound();
    }
    PlaceUnits(player, place) = after;
    g_durabilityDirty = true;
}

// A broken inventory knife in hand is thrown away (user, 2026-10-27: "the discard action played on it"): item_dump_first
// 0x5D (vtable VA 0x1045040, update VA 0xC74050) drops the item in hand (cItemManager VA 0xC4B700(0xE)), motion 0x131.
// Asked every 0.5 s while it is in hand (a move under way may refuse it); taken again later, it is thrown again.
void RequestPlayerAction(uintptr_t p, uintptr_t fn, int action) {
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(p), nullptr, action, 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ThrowBrokenKnives() {
    static DWORD asked[2] = {};
    for (int i = 0; i < 2; ++i) {
        const uintptr_t p = actors::InGame() ? actors::Player(i) : 0;
        const uintptr_t record = p ? HeldInventoryKnife(p) : 0;
        if (!record) continue;
        {
            std::lock_guard<std::mutex> lock(g_invLock);
            const InventoryKnife* knife = FindInventoryKnife(i, record);
            if (!knife || knife->units < 0.0f || knife->units > 0.0f) continue;
        }
        uint32_t action = 0;
        uintptr_t vtable = 0, fn = 0;
        const DWORD now = GetTickCount();
        if (now - asked[i] < 500 || !mem::Read(p + 0x10E0, action) || action == 0x5D || action == 0x5E ||
            !mem::Read(p, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn)
            continue;
        asked[i] = now;
        RequestPlayerAction(p, fn, 0x5D);
        LOG("knife durability: player %d throws the broken inventory knife away (action %X)", i + 1, action);
    }
}

// Render thread: a broken main knife stays in the sheath, unusable (inventory.cpp KnifeUnusable); a broken second knife
// is gone, the main one back in the slot. The file kept up to date.
// A knife chosen in the slot's list is a new one (user, 2026-10-27: "spawning a knife from the list always brings it at
// full durability"): the main knife full again when the slot's choice changes.
int g_seenChoice[2] = { -1, -1 };  // render thread

bool Broken(const std::atomic<float>& units) {
    const float value = units.load(std::memory_order_relaxed);
    return value >= 0.0f && value <= 0.0f;
}

bool MercenariesMode() {
    uint32_t mode = 0;
    const uintptr_t at = mem::Chain(reinterpret_cast<uintptr_t>(mem::Rva(0xE340A4)), { 0, 0x1042C, 0x58 });
    return at && mem::Read(at, mode) && (mode == 1 || mode == 2 || mode == 5);  // as dying.cpp / mercenaries.cpp
}

// Every place of a player's sheath set (the slot gets the highlighted one's weapon).
void SetPlaces(int i, const uint16_t (&place)[kSheathPlaces], int active) {
    for (int k = 1; k < kSheathPlaces; ++k) SetSheathPlace(i, k, place[k], false);
    if (!SetSheathActivePlace(i, active)) SetSheathActivePlace(i, 0);
}

// A Mercenaries round's fresh sheaths (the slots get the main knives back).
void FreshSheaths(uintptr_t player) {
    for (int i = 0; i < 2; ++i) {
        Knife& knife = g_knives[i];
        knife.units = -1.0f;
        knife.breakPending = false;
        for (int k = 0; k < kSheathPlaces; ++k) {
            knife.extraUnits[k] = -1.0f;
            knife.extraBreakPending[k] = false;
        }
        SetPlaces(i, {}, 0);  // the main knife into the slot, whatever this instance knew
    }
    std::lock_guard<std::mutex> lock(g_invLock);
    for (int i = 0; i < 2; ++i) {
        g_invKnives[i].clear();
        g_invPlayer[i] = actors::Player(i);
        std::fill(std::begin(g_savedInv[i]), std::end(g_savedInv[i]), -2.0f);
    }
    g_mercPlayer = player;
    g_durabilityDirty = true;  // the file's round is this one at once (a reload right after must not resume the last)
}

// A hot reload in a round: the file's round if it is this one, else what the slot holds is kept (a knife other than the
// main one taken for the second knife), the units full.
void ResumeOrKeepSheaths(uintptr_t player) {
    if (g_mercSavedPlayer && g_mercSavedPlayer == static_cast<uint32_t>(player)) {
        for (int i = 0; i < 2; ++i) {
            Knife& knife = g_knives[i];
            knife.units = g_mercSaved.units[i];
            for (int k = 1; k < kSheathPlaces; ++k) knife.extraUnits[k] = g_mercSaved.placeUnits[i][k];
            SetPlaces(i, g_mercSaved.place[i], g_mercSaved.active[i]);
        }
        {
            std::lock_guard<std::mutex> lock(g_invLock);
            for (int i = 0; i < 2; ++i) {
                g_invKnives[i].clear();
                g_invPlayer[i] = actors::Player(i);
                std::copy(std::begin(g_mercSaved.savedInv[i]), std::end(g_mercSaved.savedInv[i]), g_savedInv[i]);
            }
        }
        g_mercPlayer = player;
        LOG("knife durability: the Mercenaries round under way given back (a reload in it)");
        return;
    }
    for (int i = 0; i < 2; ++i) {
        Knife& knife = g_knives[i];
        knife.units = -1.0f;
        for (int k = 0; k < kSheathPlaces; ++k) knife.extraUnits[k] = -1.0f;
        const uint16_t inSlot = KnifeSlotItem(i), main = SheathOwnMain(i);
        if (inSlot && IsKnifeItem(inSlot) && main && inSlot != main && !SheathPlaceItem(i, 1)) {
            SetSheathPlace(i, 1, inSlot, true);
            LOG("knife durability: player %d's knife %04X in the slot kept in place 2", i + 1, inSlot);
        }
    }
    std::lock_guard<std::mutex> lock(g_invLock);
    for (int i = 0; i < 2; ++i) {
        g_invKnives[i].clear();
        g_invPlayer[i] = actors::Player(i);
        std::fill(std::begin(g_savedInv[i]), std::end(g_savedInv[i]), -2.0f);
    }
    g_mercPlayer = player;
}

// This instance's first frame: player 1 already there = a hot reload in a game (user, 2026-10-28: the knives are only
// replaced when a Mercenaries round starts, never by a reload in one).
uintptr_t g_playerAtLoad = 0;
bool g_firstFrameSeen = false;
// Player 1 gone since he was last seen: a stage loaded, so a new round (user, 2026-10-28: a round restarted kept the
// broken main knife and the second one - the new player 1 had the old one's address).
bool g_playerGone = false;

void TrackMercenaries() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!g_firstFrameSeen) {
        g_firstFrameSeen = true;
        g_playerAtLoad = p;
    }
    if (!p) {
        g_playerGone = true;
        return;
    }
    const bool merc = MercenariesMode();
    if (merc && !g_inMerc) {  // the story's knives aside
        {
            std::lock_guard<std::mutex> lock(g_invLock);
            for (int i = 0; i < 2; ++i) {
                std::copy(std::begin(g_savedInv[i]), std::end(g_savedInv[i]), g_story.savedInv[i]);
                for (const InventoryKnife& knife : g_invKnives[i]) g_story.savedInv[i][knife.grid] = knife.units;
            }
        }
        for (int i = 0; i < 2; ++i) {
            Knife& knife = g_knives[i];
            g_story.units[i] = knife.units.load();
            StoryPlaces(i, g_story.place[i], g_story.placeUnits[i], g_story.active[i]);
            std::fill(std::begin(knife.loadedPlace), std::end(knife.loadedPlace), uint16_t{ 0 });
            knife.loadedActive = -1;
        }
        g_inMerc = true;
        if (p == g_playerAtLoad && !g_playerGone) ResumeOrKeepSheaths(p);  // a reload in a round: nothing replaced
        else FreshSheaths(p);                                               // a round starting
        g_mercSavedPlayer = 0;  // given once
        LOG("knife durability: Mercenaries - the story's knives kept aside");
    } else if (merc && (p != g_mercPlayer || g_playerGone)) {
        FreshSheaths(p);
        LOG("knife durability: a new Mercenaries round, the main knives only");
    } else if (!merc && g_inMerc) {  // back to the story: its knives again
        g_inMerc = false;
        for (int i = 0; i < 2; ++i) {
            Knife& knife = g_knives[i];
            knife.units = g_story.units[i];
            knife.breakPending = false;
            for (int k = 1; k < kSheathPlaces; ++k) {
                knife.extraUnits[k] = g_story.placeUnits[i][k];
                knife.extraBreakPending[k] = false;
            }
            SetPlaces(i, g_story.place[i], g_story.active[i]);
        }
        std::lock_guard<std::mutex> lock(g_invLock);
        for (int i = 0; i < 2; ++i) {
            g_invKnives[i].clear();
            g_invPlayer[i] = actors::Player(i);
            std::copy(std::begin(g_story.savedInv[i]), std::end(g_story.savedInv[i]), g_savedInv[i]);
        }
        LOG("knife durability: back to the story, its knives again");
    }
    g_playerGone = false;
}

void TickDurability() {
    LoadDurability();
    ScanInventoryKnives();
    TickOrganiser();
    ThrowBrokenKnives();
    for (int i = 0; i < 2; ++i) {
        Knife& knife = g_knives[i];
        const bool loaded = std::any_of(std::begin(knife.loadedPlace), std::end(knife.loadedPlace), [](uint16_t id) { return id != 0; });
        if ((loaded || knife.loadedActive > 0) && actors::InGame() && actors::Player(i)) {
            // The file's places back in the sheath, the one used as it was.
            uint16_t place[kSheathPlaces] = {};
            for (int k = 1; k < kSheathPlaces; ++k) place[k] = SheathPlaceItem(i, k) ? SheathPlaceItem(i, k) : knife.loadedPlace[k];
            const int active = std::max(0, knife.loadedActive);
            std::fill(std::begin(knife.loadedPlace), std::end(knife.loadedPlace), uint16_t{ 0 });
            knife.loadedActive = -1;
            SetPlaces(i, place, active);
        }
        // The main knife breaking with other knives in the sheath: the first unbroken one used at once (user, 2026-10-27:
        // the broken main knife left in use blocked the knife and its parry). At the break only: chosen again, it stays.
        if (knife.breakPending.exchange(false)) {
            Toast(i == 0 ? "Couteau principal cassé (joueur 1)" : "Couteau principal cassé (joueur 2)", false);
            for (int k = 1; k < SheathPlaceCount() && SheathActivePlace(i) == 0; ++k)
                if (const uint16_t id = SheathPlaceItem(i, k); id && IsKnifeItem(id) && !Broken(knife.extraUnits[k])) {
                    SetSheathActivePlace(i, k);
                    g_durabilityDirty = true;
                    LOG("knife durability: player %d's main knife broken, the knife of place %d used", i + 1, k + 1);
                }
        }
        // A knife of another place breaking is gone (only the main knife stays broken in the sheath: user, 2026-10-27),
        // and the main one goes back into the slot at once - never left on nothing (once the knife is put away if in hand).
        for (int k = 1; k < kSheathPlaces; ++k)
            if (knife.extraBreakPending[k].exchange(false) && SheathPlaceItem(i, k)) {
                SetSheathPlace(i, k, 0, false);
                SetSheathActivePlace(i, 0);
                knife.extraUnits[k] = -1.0f;
                Toast(i == 0 ? "Second couteau cassé (joueur 1)" : "Second couteau cassé (joueur 2)", false);
                LOG("knife durability: player %d's knife of place %d broken and gone, the main one back", i + 1, k + 1);
                g_durabilityDirty = true;
            }
        const int choice = KnifeSlotChoice(i);
        const bool changed = g_seenChoice[i] >= 0 && choice != g_seenChoice[i];
        g_seenChoice[i] = choice;
        if (changed && KnifeSlotHoldsKnife(i)) {
            knife.units = -1.0f;  // the maximum
            knife.brokenFrom = -1;
            g_durabilityDirty = true;
            LOG("knife durability: player %d's main knife chosen in the slot (choice %d), full", i + 1, choice);
        }
    }
    static DWORD saved = 0;
    if (g_durabilityDirty.load() && GetTickCount() - saved > 2000) {
        saved = GetTickCount();
        g_durabilityDirty = false;
        SaveDurability();
    }
}

// "Réparer le couteau principal": the sheaths' main knives (a slot left "Rien" by an older break gets its choice back);
// "Réparer tous les couteaux": the second knives and the inventory's too.
bool RepairKnives(bool all) {
    LoadDurability();
    for (int i = 0; i < 2; ++i) {
        g_knives[i].units = -1.0f;
        if (all)
            for (auto& units : g_knives[i].extraUnits) units = -1.0f;
        const int from = g_knives[i].brokenFrom.exchange(-1);
        if (from >= 0 && KnifeSlotChoice(i) == KnifeSlotNoneChoice())
            if (Feature* slot = Find(kSlotIds[i])) Choose(*slot, from);
    }
    if (all) {
        std::lock_guard<std::mutex> lock(g_invLock);
        for (auto& list : g_invKnives)
            for (InventoryKnife& knife : list) knife.units = -1.0f;
        for (auto& player : g_savedInv)  // the knives waiting for a level (on the Organiser screen too)
            for (float& units : player)
                if (units >= -1.0f) units = -1.0f;
        for (float& units : g_organiser.units)
            if (units >= -1.0f) units = -1.0f;
        for (auto& knife : g_parked) knife.second = -1.0f;
    }
    LOG("knife durability: %s repaired", all ? "every knife" : "the main knives");
    SaveDurability();
    return true;
}

// "Durabilité : affichage" (user, 2026-10-27): the units as a number or a bar above the icon, or the icon itself fading
// (100 % to 15 % of opacity; gone once broken, the slot then holding "Rien").
enum Display { kNumber, kBar, kFading };
std::atomic<int> g_display{ kNumber };

float Share(int player) { return Durability(player) / std::max(1.0f, MaxUnits(SheathKnifeInUse(player))); }

ImU32 ShareColour(float share) {
    return share <= 0.1f ? IM_COL32(235, 60, 45, 255) : share <= 0.25f ? IM_COL32(245, 160, 40, 255)
                                                                       : IM_COL32(235, 235, 235, 255);
}

// An inventory knife in hand: its units inside the health ring as a weapon's ammo (user, 2026-10-27), with the HUD's
// own digits at its places (1p_000 / 1p_00 / 1p_0: (-36, 24), (-12, 24), (12, 24) from the ring's centre (1041, 583),
// 24 x 32; player 2's 2p_ ones (-27, 14), (-9, 14), (9, 14) from (1134, 464), 18 x 25), no leading zeros; green when
// full as a full magazine, else the ammo's grey (orange at a quarter, red at a tenth).
void DrawRingUnits(ImDrawList* dl, int player, float scale, bool mirrored, const ImVec2& screen) {
    const uintptr_t p = actors::Player(player);
    const uintptr_t record = p ? HeldInventoryKnife(p) : 0;
    if (!record) return;
    float units = -1.0f;
    uint16_t item = 0;
    {
        std::lock_guard<std::mutex> lock(g_invLock);
        const InventoryKnife* knife = FindInventoryKnife(player, record);
        if (!knife) return;
        units = knife->units;
        item = knife->id;
    }
    IDirect3DTexture9* texture = TextureOf(kDigitsTexture);
    D3DSURFACE_DESC desc{};
    if (!texture || FAILED(texture->GetLevelDesc(0, &desc)) || !desc.Width || !desc.Height) return;
    const float max = MaxUnits(item), value = Units(units, item), share = value / std::max(1.0f, max);
    const ImU32 colour = units < 0.0f || value >= max ? IM_COL32(0, 255, 0, 255)
                         : share <= 0.1f             ? IM_COL32(235, 60, 45, 255)
                         : share <= 0.25f            ? IM_COL32(245, 160, 40, 255)
                                                     : IM_COL32(200, 200, 200, 255);
    struct Ring {
        float x, y, left, top, step, width, height;
    };
    static constexpr Ring kRings[2] = { { 1041.0f, 583.0f, -36.0f, 24.0f, 24.0f, 24.0f, 32.0f },
                                        { 1134.0f, 464.0f, -27.0f, 14.0f, 18.0f, 18.0f, 25.0f } };
    const Ring& ring = kRings[player];
    const ImVec2 centre = OnScreen(ring.x, ring.y, scale, mirrored, screen);
    const int number = std::clamp(static_cast<int>(std::ceil(value)), 0, 999);
    const int digits[3] = { number / 100, number / 10 % 10, number % 10 };
    const int first = number >= 100 ? 0 : number >= 10 ? 1 : 2;
    for (int d = first; d < 3; ++d) {
        const ImVec2 p0(centre.x + (ring.left + ring.step * d) * scale, centre.y + ring.top * scale);
        const ImVec2 p1(p0.x + ring.width * scale, p0.y + ring.height * scale);
        const float u = kDigits[digits[d]][0], v = kDigits[digits[d]][1];
        dl->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture))), p0, p1,
                     ImVec2(u / desc.Width, v / desc.Height), ImVec2((u + 24.0f) / desc.Width, (v + 32.0f) / desc.Height),
                     colour);
    }
}

// The units above an icon, the game's white with a dark outline (orange at a quarter, red at a tenth), centred at `c`.
void DrawUnitsNumber(ImDrawList* dl, float units, float share, const ImVec2& c, float iconPx, float scale) {
    char text[16];
    std::snprintf(text, sizeof(text), "%d", static_cast<int>(std::ceil(units)));
    ImFont* font = ImGui::GetFont();
    const float size = iconPx * 0.52f;
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 at(c.x - extent.x * 0.5f, c.y - extent.y * 0.5f);
    const float o = std::max(1.0f, scale);
    for (const ImVec2 d : { ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o) })
        dl->AddText(font, size, ImVec2(at.x + d.x, at.y + d.y), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(font, size, at, ShareColour(share), text);
}

// The rounds of a firearm in the knife slot above its icon (user, 2026-11-05: "as the knife's durability", with the
// "+1" of a round in the chamber: a count above the capacity, as the HUD shows it): the HUD's ammo colours, green
// full, red empty, otherwise white.
void DrawSlotGunAmmo(ImDrawList* dl, int count, int capacity, const ImVec2& c, float iconPx, float scale) {
    const bool chambered = capacity > 0 && count > capacity;
    const int shown = chambered ? count - 1 : count;
    const ImU32 colour = shown <= 0                           ? IM_COL32(235, 60, 45, 255)
                         : capacity > 0 && shown >= capacity ? IM_COL32(40, 230, 40, 255)
                                                             : IM_COL32(235, 235, 235, 255);
    char text[16];
    std::snprintf(text, sizeof(text), "%d", shown);
    ImFont* font = ImGui::GetFont();
    const float size = iconPx * 0.52f, o = std::max(1.0f, scale);
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 at(c.x - extent.x * 0.5f, c.y - iconPx * 0.75f - extent.y * 0.5f);
    const auto outlined = [&](const ImVec2& p, float fontSize, const char* s, ImU32 col) {
        for (const ImVec2 d : { ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o) })
            dl->AddText(font, fontSize, ImVec2(p.x + d.x, p.y + d.y), IM_COL32(0, 0, 0, 200), s);
        dl->AddText(font, fontSize, p, col, s);
    };
    outlined(at, size, text, colour);
    if (chambered) outlined(ImVec2(at.x + extent.x + 2.0f * scale, at.y + size * 0.3f), size * 0.7f, "+1", IM_COL32(40, 230, 40, 255));
}

// A bar centred at `c`, `widthPx` wide, dark behind, its fill in the number's colours.
void DrawUnitsBar(ImDrawList* dl, float share, const ImVec2& c, float widthPx, float scale) {
    const float half = widthPx * 0.5f, height = std::max(3.0f, 4.0f * scale), border = std::max(1.0f, scale);
    const ImVec2 p0(c.x - half, c.y - height * 0.5f), p1(c.x + half, c.y + height * 0.5f);
    dl->AddRectFilled(ImVec2(p0.x - border, p0.y - border), ImVec2(p1.x + border, p1.y + border), IM_COL32(0, 0, 0, 190));
    share = std::clamp(share, 0.0f, 1.0f);
    if (share > 0.0f) dl->AddRectFilled(p0, ImVec2(p0.x + (p1.x - p0.x) * share, p1.y), ShareColour(share));
}

// An item's icon centred at `c` with its durability in the chosen display (`worn`: a knife whose units count). A broken
// knife's fading icon is gone (`brokenAlpha` 0) or kept faint.
void DrawWornIcon(ImDrawList* dl, uint16_t item, bool worn, float units, const ImVec2& c, float iconPx, float scale,
                  bool showIcon, float brokenAlpha) {
    const int display = g_display.load(std::memory_order_relaxed);
    const float share = units / std::max(1.0f, MaxUnits(item));
    if (worn && display == kFading) {
        const float alpha = share > 0.0f ? 0.15f + 0.85f * std::clamp(share, 0.0f, 1.0f) : brokenAlpha;
        if (alpha > 0.0f) DrawItemIcon(dl, item, c, iconPx, alpha);
    } else if (showIcon) {
        DrawItemIcon(dl, item, c, iconPx, 1.0f);
    }
    if (worn && display == kNumber) DrawUnitsNumber(dl, units, share, ImVec2(c.x, c.y - iconPx * 0.75f), iconPx, scale);
    if (worn && display == kBar) DrawUnitsBar(dl, share, ImVec2(c.x, c.y - iconPx * 0.5f - 5.0f * scale), iconPx, scale);
}

// A number in the HUD's digits (cp03), right-aligned on `right`, its bottom on `bottom`, each digit `w` x `h` pixels.
void DrawDigits(ImDrawList* dl, int number, float right, float bottom, float w, float h, ImU32 colour) {
    IDirect3DTexture9* texture = TextureOf(kDigitsTexture);
    D3DSURFACE_DESC desc{};
    if (!texture || FAILED(texture->GetLevelDesc(0, &desc)) || !desc.Width || !desc.Height) return;
    number = std::clamp(number, 0, 999);
    int count = number >= 100 ? 3 : number >= 10 ? 2 : 1;
    for (int k = 0; k < count; ++k, number /= 10) {
        const int digit = number % 10;
        const ImVec2 p1(right - w * k, bottom), p0(p1.x - w, p1.y - h);
        const float u = kDigits[digit][0], v = kDigits[digit][1];
        dl->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture))), p0, p1,
                     ImVec2(u / desc.Width, v / desc.Height), ImVec2((u + 24.0f) / desc.Width, (v + 32.0f) / desc.Height),
                     colour);
    }
}

// The colour of a knife's number in the game's digits: green full (as a full magazine), the ammo's grey, orange at a
// quarter, red at a tenth.
ImU32 DigitsColour(float units, uint16_t item) {
    const float max = MaxUnits(item), value = Units(units, item), share = value / std::max(1.0f, max);
    return units < 0.0f || value >= max ? IM_COL32(0, 255, 0, 255)
           : share <= 0.1f             ? IM_COL32(235, 60, 45, 255)
           : share <= 0.25f            ? IM_COL32(245, 160, 40, 255)
                                       : IM_COL32(200, 200, 200, 255);
}

// A place of the inventory screen's 1280x720 layout: centred on the screen (not bottom-anchored as the HUD: the counts
// drawn that way sat 40 below their cells, user 2026-10-27).
ImVec2 OnInventoryScreen(float x, float y, float scale, const ImVec2& screen) {
    return ImVec2((screen.x - 1280.0f * scale) * 0.5f + x * scale, (screen.y - 720.0f * scale) * 0.5f + y * scale);
}

// The sheaths on the inventory screen (user, 2026-10-27: the knife in the sheath - or the weapon the trainer put there -
// with its durability, centred above each player's grid, the second knife above the right column; the one in use framed
// when there are two). Layout of cp_bukislot_360 (1280x720, centred on the
// screen): the grids null_slot01 (416, 352) and null_slot02 (864, 352), cells 96 apart: a character right-handed by the
// game on the left (Chris), a left-handed one on the right (Sheva), whoever plays it (user, 2026-11-04: playing Sheva,
// her knife's units sat in Chris's grid); the names "Chris" / "Sheva" on the row y ~190, so the icons above it. The
// partner's grid shows whatever "Joueur 2 invisible".
// The inventory's knives get their units in their cells as the game's counts (bottom right of the cell, smaller digits;
// measured on the screenshot: 13 x 17.5, right edge 3 left of the centre, bottom 41 below it).
int ChamberedCount(uintptr_t player, uintptr_t record);  // below: a weapon's count holding its chambered round, else -1

// A green "+1", its bottom left at `at`, `height` high, outlined like the counts.
void DrawPlusOne(ImDrawList* dl, const ImVec2& at, float height, float scale) {
    ImFont* font = ImGui::GetFont();
    const float size = height * 0.85f, o = std::max(1.0f, scale);
    const ImVec2 p(at.x, at.y - size);
    for (const ImVec2 d : { ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o) })
        dl->AddText(font, size, ImVec2(p.x + d.x, p.y + d.y), IM_COL32(0, 0, 0, 200), "+1");
    dl->AddText(font, size, p, IM_COL32(40, 230, 40, 255), "+1");
}

void DrawInventorySheaths(ImDrawList* dl, float scale, const ImVec2& screen, bool durability) {
    if (!Player1InventoryOpen()) return;
    static constexpr float kGridX[2] = { 416.0f, 864.0f };
    constexpr float kGridY = 352.0f, kCell = 96.0f, kSize = 44.0f;
    // The places in a row above the grid, centred on it, all the same distance apart (user, 2026-11-05: place 1 left of
    // the grid and place 5 right of it were too far from the others, "the same space between each"): 92 apart (the
    // columns are 96; five places at 96 nearly touched the other grid's), at the height the user chose (148).
    constexpr float kPlaceStep = 92.0f, kPlaceY = 148.0f;
    const float icon = kSize * scale;
    // A line between the two inventories (user, 2026-11-05: the two rows of places so close that it was hard to tell
    // Chris's from Sheva's), in the colour of the places not highlighted: from the top of the places' frames to the
    // bottom of the grids, halfway between them.
    if (actors::Player(1) && SheathPlaceCount() > 1) {
        const ImVec2 top = OnInventoryScreen((kGridX[0] + kGridX[1]) * 0.5f, kPlaceY - kSize * 0.62f, scale, screen);
        const ImVec2 bottom = OnInventoryScreen((kGridX[0] + kGridX[1]) * 0.5f, kGridY + 1.5f * kCell, scale, screen);
        dl->AddLine(top, bottom, IM_COL32(150, 150, 150, 90), std::max(1.0f, 1.5f * scale));
    }
    for (int i = 0; i < 2; ++i) {
        if (i == 1 && !actors::Player(1)) continue;
        const float gridX = kGridX[PlayerCharacterLeftHanded(i) ? 1 : 0];
        const int count = SheathPlaceCount(), active = SheathActivePlace(i);
        for (int k = 0; k < count; ++k) {
            const ImVec2 at = OnInventoryScreen(gridX + (static_cast<float>(k) - (count - 1) * 0.5f) * kPlaceStep, kPlaceY, scale,
                                                screen);
            const uint16_t item = SheathPlaceItem(i, k);
            if (item) {
                DrawWornIcon(dl, item, durability && IsKnifeItem(item), Units(PlaceUnits(i, k).load(), item), at, icon, scale, true,
                             0.15f);
                int rounds = 0, magazine = 0;
                if (SheathGunRounds(i, item, rounds, magazine)) DrawSlotGunAmmo(dl, rounds, magazine, at, icon, scale);
            }
            // Every place framed once there are several, the highlighted one bright.
            if (count > 1) {
                const float half = icon * 0.62f;
                dl->AddRect(ImVec2(at.x - half, at.y - half), ImVec2(at.x + half, at.y + half),
                            k == active ? IM_COL32(230, 230, 230, 220) : IM_COL32(150, 150, 150, 90), 4.0f * scale,
                            ImDrawFlags_None, std::max(1.0f, (k == active ? 1.5f : 1.0f) * scale));
            }
        }
        // The "+1" of a weapon holding its chambered round, right of its count (which shows the magazine).
        if (const uintptr_t p = actors::Player(i))
            for (int k = 0; k < 24; ++k) {
                const uintptr_t r = ItemRecordAddress(p, k);
                uint32_t grid = 0xD;
                if (!r || !mem::Read(r + 0x18, grid) || grid > 8 || ChamberedCount(p, r) < 0) continue;
                const float cellX = gridX + (static_cast<float>(grid % 3) - 1.0f) * kCell;
                const float cellY = kGridY + (static_cast<float>(grid / 3) - 1.0f) * kCell;
                DrawPlusOne(dl, OnInventoryScreen(cellX - 1.0f, cellY + 41.0f, scale, screen), 17.5f * scale, scale);
            }
        if (!durability) continue;
        std::lock_guard<std::mutex> lock(g_invLock);
        for (const InventoryKnife& knife : g_invKnives[i]) {
            const float cellX = gridX + (static_cast<float>(knife.grid % 3) - 1.0f) * kCell;
            const float cellY = kGridY + (static_cast<float>(knife.grid / 3) - 1.0f) * kCell;
            const ImVec2 corner = OnInventoryScreen(cellX - 3.0f, cellY + 41.0f, scale, screen);
            DrawDigits(dl, static_cast<int>(std::ceil(Units(knife.units, knife.id))), corner.x, corner.y, 13.0f * scale,
                       17.5f * scale, DigitsColour(knife.units, knife.id));
        }
    }
}

// "Balle dans la chambre (+1)" (user, 2026-11-04: "it stays at 300 and writes +1"): a "+1" right of player 1's ammo
// count while it shows the magazine without its extra chambered round (fire_rate.cpp). The count's three digit
// elements (cockpit + 0x12D0 + 4 * idx, VA 0x9F1E90; 24 x 32 each) sit at -36 / -12 / +12, 24 below their anchor, the
// leading zeros hidden: the ones always in the same place. Measured on the user's screenshot at 2560x1600 (scale 2):
// the ones digit centred at (1063.5, 622) of the bottom-right-anchored 1280x720 layout, so the anchor at (1051.5, 598).
// The "+" drawn, the "1" in the HUD's own digits, 60 % of their size, in the full count's green.
// The partner's gauge too (user, 2026-11-06: "Sheva's VZ61 at 301 instead of 300 +1"): her count's digits 0.75 of the
// size (35 px apart instead of 46 at 2560x1600), the ones digit centred at (2302.5, 1138) = (1151.25, 489) of the layout.
struct CountSpot {
    float anchorX, onesCentreY, size;  // the digits' anchor (the ones digit's centre - 12 x size), layout units
};
constexpr CountSpot kPlayerCount{ 1051.5f, 622.0f, 1.0f }, kPartnerCount{ 1142.25f, 489.0f, 0.75f };

void DrawChamberPlusOne(ImDrawList* dl, float scale, bool mirrored, const ImVec2& screen, const CountSpot& spot) {
    constexpr float kSmall = 0.6f;
    const float digitW = 24.0f * spot.size, digitH = 32.0f * spot.size;
    const float onesRight = spot.anchorX + 12.0f * spot.size + digitW * 0.5f;  // layout units
    const float left = mirrored ? (1280.0f - spot.anchorX + 12.0f * spot.size + digitW * 0.5f) * scale
                                : screen.x - (1280.0f - onesRight) * scale;
    const float w = digitW * kSmall * scale, h = digitH * kSmall * scale;
    const float top = screen.y - (720.0f - (spot.onesCentreY - digitH * 0.5f)) * scale;  // aligned on the digits' top
    const ImU32 green = IM_COL32(0, 255, 0, 255), shadow = IM_COL32(0, 0, 0, 200);
    const float thick = std::max(2.0f, 2.2f * scale), arm = w * 0.38f;
    const ImVec2 c(left + 2.0f * scale + w * 0.5f, top + h * 0.5f);
    for (int pass = 0; pass < 2; ++pass) {
        const float o = pass == 0 ? std::max(1.0f, scale) : 0.0f;
        const ImU32 colour = pass == 0 ? shadow : green;
        dl->AddRectFilled(ImVec2(c.x - arm + o, c.y - thick * 0.5f + o), ImVec2(c.x + arm + o, c.y + thick * 0.5f + o), colour);
        dl->AddRectFilled(ImVec2(c.x - thick * 0.5f + o, c.y - arm + o), ImVec2(c.x + thick * 0.5f + o, c.y + arm + o), colour);
    }
    DrawDigits(dl, 1, left + 2.0f * scale + w + w * 0.8f, top + h, w, h, green);
}

// The sheaths on the Organiser screen (user, 2026-11-05: "the sheaths cannot be seen nor used from the Organiser"): each
// page's sheath in a column beside its grid, on the outer side (user, 2026-11-05: Chris's left of his grid, Sheva's
// right of hers), the highlighted place framed. The grids' origins: the screen's +0x1B88 (x) / +0x1B90 (y) by page, in
// the 1280x720 layout (the name bar from it to 24 below, cells 72 apart from x 0 / y 26, the first one's centre 36 / 62
// from it, its count right-aligned at 70, its digits from 78 to 94 below: the picture of 2026-11-05). The "+1" of a chambered round right of its count (which moved left for it), from
// the cells' records every frame (their drawing is not).
void DrawOrganiser(ImDrawList* dl, float scale, const ImVec2& display, bool durability) {
    const uintptr_t screen = OrganiserScreenNow();
    if (!screen) return;
    // Frames 49.6 high (40 x 0.62 x 2): 52 apart, 2 between them (user, 2026-11-05: 46 overlapped; the middle one stays).
    constexpr float kCell = 72.0f, kIcon = 40.0f, kStep = 52.0f;
    const int count = SheathPlaceCount();
    const float icon = kIcon * scale;
    for (int page = 0; page < 2; ++page) {
        int32_t x0 = 0, y0 = 0;
        if (!mem::Read(screen + 0x1B88 + 4 * page, x0) || !mem::Read(screen + 0x1B90 + 4 * page, y0)) continue;
        const int active = SheathActivePlace(page);
        for (int k = 0; k < count; ++k) {
            const float x = static_cast<float>(x0) + (page == 0 ? -36.0f : 3.0f * kCell + 36.0f);
            const float y = static_cast<float>(y0) + 134.0f + (static_cast<float>(k) - (count - 1) * 0.5f) * kStep;
            const ImVec2 at = OnInventoryScreen(x, y, scale, display);
            const uint16_t item = SheathPlaceItem(page, k);
            if (item) {
                DrawWornIcon(dl, item, durability && IsKnifeItem(item), Units(PlaceUnits(page, k).load(), item), at, icon, scale,
                             true, 0.15f);
                int rounds = 0, magazine = 0;
                if (SheathGunRounds(page, item, rounds, magazine)) DrawSlotGunAmmo(dl, rounds, magazine, at, icon, scale);
            }
            if (count > 1) {
                const float half = icon * 0.62f;
                dl->AddRect(ImVec2(at.x - half, at.y - half), ImVec2(at.x + half, at.y + half),
                            k == active ? IM_COL32(230, 230, 230, 220) : IM_COL32(150, 150, 150, 90), 4.0f * scale,
                            ImDrawFlags_None, std::max(1.0f, (k == active ? 1.5f : 1.0f) * scale));
            }
        }
        for (int cell = 0; cell < 9; ++cell) {
            const uintptr_t record = screen + kOrganiserPages + 0x48 * (page * 9 + cell);
            uint16_t id = 0;
            int16_t rounds = 0;
            if (!mem::Read(record, id) || (id >> 8) != 1 || IsKnifeItem(id) || !mem::Read(record + 2, rounds)) continue;
            const int max = OrganiserMax(screen, record);
            if (max <= 0 || rounds <= max) continue;
            const float right = static_cast<float>(x0) + 70.0f + kCell * static_cast<float>(cell % 3);
            const float bottom = static_cast<float>(y0) + 94.0f + kCell * static_cast<float>(cell / 3);
            DrawPlusOne(dl, OnInventoryScreen(right - kPlusOneShift + 2.0f, bottom, scale, display), 16.0f * scale, scale);
        }
    }
}

void Draw() {
    const bool icons = g_on.load(std::memory_order_relaxed), durability = g_durabilityOn.load(std::memory_order_relaxed);
    if (!actors::InGame()) {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        DrawOrganiser(ImGui::GetBackgroundDrawList(), std::min(display.x / 1280.0f, display.y / 720.0f), display, durability);
    }
    if (actors::InGame() && Player1HudShown()) {
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        const float scale = std::min(screen.x / 1280.0f, screen.y / 720.0f);
        if (ChamberPlusOneShown(0))
            DrawChamberPlusOne(ImGui::GetBackgroundDrawList(), scale, Player1RingOnLeft(), screen, kPlayerCount);
        if (ChamberPlusOneShown(1) && actors::Player(1) && !Player2Invisible())
            DrawChamberPlusOne(ImGui::GetBackgroundDrawList(), scale, Player1RingOnLeft(), screen, kPartnerCount);
    }
    if ((!icons && !durability) || !actors::InGame()) return;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float scale = std::min(screen.x / 1280.0f, screen.y / 720.0f);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    DrawInventorySheaths(dl, scale, screen, durability);
    if (!Player1HudShown()) return;  // with the health ring: not in the pause menu nor the in-game cutscenes
    const bool mirrored = Player1RingOnLeft();
    const bool second = actors::Player(1) && !Player2Invisible();
    for (int i = 0; i < (second ? 2 : 1); ++i) {
        // Next to the name: the sheath's knife in use (the second one while there is one). The fading icon is the
        // durability's display: drawn even without "Icône de l'étui à côté du nom", gone once broken.
        const Place& place = kPlaces[i];
        const ImVec2 c = OnScreen(place.x, place.y, scale, mirrored, screen);
        DrawWornIcon(dl, KnifeSlotItem(i), durability && KnifeSlotHoldsKnife(i), Durability(i), c, place.size * scale, scale,
                     icons, 0.0f);
        int count = 0, capacity = 0;
        if (icons && KnifeSlotGunAmmo(i, count, capacity)) DrawSlotGunAmmo(dl, count, capacity, c, place.size * scale, scale);
        if (durability) DrawRingUnits(dl, i, scale, mirrored, screen);
    }
}

// The Organiser's cells (see TickOrganiser). Its item icons are named "item_%03x" by VA 0xA02A10 (thiscall(screen, name,
// id) ret 8, the screen unused) from a table of the items the game stores (VA 0x1128C70, 12-byte entries: u16 id, u16
// icon), where no knife is: a knife got the default icon 0x94, which its GUI (id\jpn\itembox\itembox_360) lacks, so the
// M92F's stayed. The GUI's icons follow the weapons' order (0x0122 item_140, 0x0125 item_143...) and keep the knives'
// places, used by no item: item_123 (Chris's knife 0x0101), item_130 (Sheva's 0x010E), item_141 / item_142 (their
// variants 0x0123 / 0x0124, each its own icon in the game's inventory: user, 2026-11-02). Both calls (VA 0xA061A6 the
// cells, VA 0xA0777F an item's icon elsewhere on the screen) go through OrganiserIconName.
void __fastcall OrganiserIconName(uintptr_t screen, void*, char* name, uint32_t id) {
    g_organiserScreen.store(screen, std::memory_order_relaxed);
    g_organiserSeen.store(GetTickCount(), std::memory_order_relaxed);
    const char* knife = nullptr;
    switch (static_cast<uint16_t>(id)) {
    case 0x0101: knife = "item_123"; break;
    case 0x010E: knife = "item_130"; break;
    case 0x0123: knife = "item_141"; break;
    case 0x0124: knife = "item_142"; break;
    }
    if (knife) {
        std::memcpy(name, knife, 9);
        return;
    }
    using IconNameFn = void(__fastcall*)(uintptr_t, void*, char*, uint32_t);
    reinterpret_cast<IconNameFn>(mem::Rva(0x602A10))(screen, nullptr, name, id);
}

// A cell's count is printed by the CRT's sprintf (VA 0xE63B3E: "%d" of the record's +2) at VA 0xA062FD, the record in
// ecx, the screen in esi, the text's colour at the caller's [esp + 0x10] (alpha << 24 | rgb). A knife's count is its
// units instead (nothing without "Durabilité du couteau"), coloured as the game colours a full stack ([VA 0x11AF658]),
// or an empty one ([VA 0x11AF654]) from a tenth.
// A weapon holding its chambered round (a count above the cell's maximum, VA 0xA028A0 thiscall(screen, record) ret 4:
// the magazine from the record's capacity level +0x32; user, 2026-11-05: "my M92F shows 11 instead of 10 +1 on the
// Organiser") shows its magazine, moved left by kPlusOneShift (OrganiserCountStub moves the caller's x in ebp) for the
// overlay's green "+1" (DrawOrganiser).
int OrganiserMax(uintptr_t screen, uintptr_t record) {
    using MaxFn = int(__fastcall*)(uintptr_t screen, void* edx, uintptr_t record);
    __try {
        return reinterpret_cast<MaxFn>(mem::Rva(0x6028A0))(screen, nullptr, record);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" int __cdecl OrganiserCount(char* text, const char* format, int count, uintptr_t record, uintptr_t screen,
                                      uint32_t* colour) {
    using SprintfFn = int(__cdecl*)(char*, const char*, ...);
    const auto print = reinterpret_cast<SprintfFn>(mem::Rva(0xA63B3E));
    g_organiserScreen.store(screen, std::memory_order_relaxed);
    g_organiserSeen.store(GetTickCount(), std::memory_order_relaxed);
    g_organiserShift = 0;
    uint16_t id = 0;
    const intptr_t offset = static_cast<intptr_t>(record - (screen + kOrganiserPages));
    const bool cellRecord = offset >= 0 && offset % 0x48 == 0 && offset / 0x48 < kOrganiserCells;
    if (!mem::Read(record, id)) return print(text, format, count);
    if (!IsKnifeItem(id)) {
        if ((id >> 8) == 1 && cellRecord) {
            const int max = OrganiserMax(screen, record);
            if (max > 0 && count > max) {
                g_organiserShift = kPlusOneShift;
                return print(text, format, count - 1);
            }
        }
        return print(text, format, count);
    }
    text[0] = '\0';
    if (!g_durabilityOn.load(std::memory_order_relaxed) || !cellRecord) return 0;
    if (g_organiser.screen != screen) {  // its knives read now: this text stays until the cell is drawn again
        LoadDurability();
        TickOrganiser();
        if (g_organiser.screen != screen) return 0;
    }
    const float units = Units(g_organiserUnits[offset / 0x48].load(std::memory_order_relaxed), id), max = MaxUnits(id);
    uint32_t rgb = 0xFFFFFF;
    if (units >= max) mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDAF658)), rgb);
    else if (units <= max * 0.1f) mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDAF654)), rgb);
    *colour = (*colour & 0xFF000000u) | (rgb & 0xFFFFFFu);
    return print(text, "%d", static_cast<int>(std::ceil(units)));
}

__declspec(naked) void OrganiserCountStub() {
    __asm {
        lea eax, [esp + 0x20]        // the caller's text colour, its [esp + 0x10]
        push eax
        push esi                     // the screen
        push ecx                     // the cell's record
        push dword ptr [esp + 0x18]  // the count
        push dword ptr [esp + 0x18]  // the format
        push dword ptr [esp + 0x18]  // the text
        call OrganiserCount
        add esp, 0x18
        sub ebp, g_organiserShift    // the caller's x of the text (recomputed for each cell)
        ret
    }
}

// A call (E8 rel32) at `rva` to the game's function at RVA `target` made to go to `detour`.
mem::Patch* CallPatch(const char* name, uintptr_t rva, uintptr_t target, void* detour) {
    uint8_t* site = mem::Rva(rva);
    const auto call = [site](uintptr_t to) {
        const int32_t rel = static_cast<int32_t>(to - reinterpret_cast<uintptr_t>(site + 5));
        std::vector<uint8_t> bytes = { 0xE8, 0, 0, 0, 0 };
        std::memcpy(bytes.data() + 1, &rel, 4);
        return bytes;
    };
    mem::Patch* patch = new mem::Patch(name, site, call(reinterpret_cast<uintptr_t>(mem::Rva(target))),
                                       call(reinterpret_cast<uintptr_t>(detour)));
    OnRestore([patch] { patch->Restore(); });  // it calls into this DLL
    return patch;
}

// The inventory screen's counts (user, 2026-11-05: "my M92F shows 11 instead of 10 +1, right in the game, not in the
// inventory"): its cells' update VA 0xA732C0 reads a record's count at VA 0xA73DF4 (mov edi, [ebp+8]: the record in ebp,
// its player at [esp+0x28], the record's max +0xC read beside it - green when equal, grey otherwise, red at 0, 999 at
// most; the infinite ones get their own sign through VA 0xC4C200) and draws it in the HUD's digits. A weapon holding its
// chambered round (a count above its capacity, fire_rate.cpp's "Balle dans la chambre") shows its magazine: the two
// reads at VA 0xA73DF0 (mov ecx, [esp+0x28]; mov edi, [ebp+8]) go through InventoryCountStub, and the overlay draws the
// "+1" beside (DrawInventorySheaths).
int RecordCapacity(uintptr_t player, uintptr_t record) {
    using CapacityFn = int(__fastcall*)(void* manager, void* edx, uintptr_t record);
    __try {
        return reinterpret_cast<CapacityFn>(mem::Rva(0x84C050))(reinterpret_cast<void*>(player + 0x21A0), nullptr, record);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// A weapon's count holding its chambered round, else -1.
int ChamberedCount(uintptr_t player, uintptr_t record) {
    int32_t count = 0;
    uint32_t id = 0;
    if (!player || !record || !mem::Read(record + 8, count) || !mem::Read(record + 4, id) || ((id >> 8) & 0xFF) != 1) return -1;
    const int capacity = RecordCapacity(player, record);
    return capacity > 0 && count > capacity ? count : -1;
}

extern "C" int __cdecl InventoryShownCount(uintptr_t record, uintptr_t player) {
    int32_t count = 0;
    mem::Read(record + 8, count);
    return ChamberedCount(player, record) > 0 ? count - 1 : count;
}

__declspec(naked) void InventoryCountStub() {
    __asm {
        mov ecx, [esp + 0x2C]  // the caller's [esp + 0x28]: the player
        push eax
        push ecx
        push edx
        push ecx               // the player
        push ebp               // the record
        call InventoryShownCount
        add esp, 8
        mov edi, eax
        pop edx
        pop ecx
        pop eax
        ret
    }
}

mem::Patch* InventoryCountPatch() {
    uint8_t* site = mem::Rva(0x673DF0);
    std::vector<uint8_t> patched = { 0xE8, 0, 0, 0, 0, 0x90, 0x90 };
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&InventoryCountStub) - reinterpret_cast<uintptr_t>(site + 5));
    std::memcpy(patched.data() + 1, &rel, 4);
    mem::Patch* patch = new mem::Patch("inventory: a chambered weapon's count", site, { 0x8B, 0x4C, 0x24, 0x28, 0x8B, 0x7D, 0x08 },
                                       patched);
    OnRestore([patch] { patch->Restore(); });  // it calls into this DLL
    return patch;
}

// Always on: the knives' icon is a fix, their count changes only with the durability.
void EnsureOrganiserPatches() {
    static bool tried = false;
    if (tried || !game::IsSupported()) return;
    tried = true;
    mem::Patch* const patches[] = {
        CallPatch("Organiser: knife icons (cells)", 0x6061A6, 0x602A10, reinterpret_cast<void*>(&OrganiserIconName)),
        CallPatch("Organiser: knife icons", 0x60777F, 0x602A10, reinterpret_cast<void*>(&OrganiserIconName)),
        CallPatch("Organiser: knife units as the count", 0x6062FD, 0xA63B3E, reinterpret_cast<void*>(&OrganiserCountStub)),
        InventoryCountPatch(),
    };
    for (mem::Patch* patch : patches)
        if (!patch->Apply()) LOG("knife durability: patch \"%s\" refused", patch->Name());
}
}  // namespace


// inventory.cpp's KnifeUnusable: the sheath's main knife broken stays there, unusable (a broken second one is gone).
bool SheathKnifeBroken(int player) {
    if (!g_durabilityOn.load(std::memory_order_relaxed) || player < 0 || player > 1) return false;
    return Broken(PlaceUnits(player, SheathActivePlace(player))) && KnifeSlotHoldsKnife(player);
}

// The inventory's "item put down" sound (user, 2026-10-27: feedback for the sheath's knives): sound\se\core\se_core00
// 0x3A (diag.sound_log while the user put items down: 0x39 taken, 0x3A put down), played as the inventory's controller
// plays it (VA 0xC6BCCF: sBioSound [RVA 0xDB1D0C], VA 0x79F790 thiscall(id, source) ret 8, the source being player 1's
// cOpeWepSlot). From the render thread, which is the game's (one thread).
// A sound of the inventory menu (bank sound\se\core\se_core00; inventory.cpp's refusals too).
// belt_lamp.cpp: its shortcut (the wheel click, like "Mettre l'arme sélectionnée dans l'étui") ignored there.
bool OrganiserOpen() { return OrganiserScreenNow() != 0; }

// inventory.cpp's checkpoint snapshot of the sheath (user, 2026-11-05: "the sheath must follow the checkpoints"): the
// units of each player's places and of his inventory knives by grid (-2: no knife there), out and back in. Not in the
// Mercenaries (their knives are kept apart, StoryKnives).
bool KnifeStoryMode() { return !g_inMerc; }

// The places of knife_durability.ini (the state at the last quit) given back to the sheath (TickDurability), or none
// waiting: a snapshot taken before would hold an empty sheath.
bool SheathFileApplied() {
    LoadDurability();
    for (const Knife& knife : g_knives)
        if (knife.loadedActive > 0 ||
            std::any_of(std::begin(knife.loadedPlace), std::end(knife.loadedPlace), [](uint16_t id) { return id != 0; }))
            return false;
    return true;
}

void KnifeUnitsSnapshot(float (&places)[2][5], float (&inv)[2][9]) {
    LoadDurability();
    std::lock_guard<std::mutex> lock(g_invLock);
    for (int i = 0; i < 2; ++i) {
        for (int place = 0; place < kSheathPlaces; ++place) places[i][place] = PlaceUnits(i, place).load();
        for (int grid = 0; grid < 9; ++grid) inv[i][grid] = g_savedInv[i][grid] >= -1.0f ? g_savedInv[i][grid] : -2.0f;
        for (const InventoryKnife& knife : g_invKnives[i])
            if (knife.grid < 9) inv[i][knife.grid] = knife.units;
    }
}

void KnifeUnitsRestore(int i, const float (&places)[2][5], const float (&inv)[2][9]) {
    if (i < 0 || i > 1) return;
    LoadDurability();
    std::lock_guard<std::mutex> lock(g_invLock);
    // The file's places (the state at the quit) are not given back over the checkpoint's.
    std::fill(std::begin(g_knives[i].loadedPlace), std::end(g_knives[i].loadedPlace), uint16_t{ 0 });
    g_knives[i].loadedActive = -1;
    for (int place = 0; place < kSheathPlaces; ++place) PlaceUnits(i, place) = places[i][place];
    bool known[9] = {};
    for (InventoryKnife& knife : g_invKnives[i])  // the rebuilt inventory's knives already seen
        if (knife.grid < 9) {
            known[knife.grid] = true;
            if (inv[i][knife.grid] >= -1.0f) knife.units = inv[i][knife.grid];
        }
    for (int grid = 0; grid < 9; ++grid)  // the others take them when they are seen
        if (!known[grid]) g_savedInv[i][grid] = inv[i][grid];
    // The Organiser shown already (user, 2026-11-10: a checkpoint restarted after the knife was used, "the durability shown on the knife
    // is the one after use" - the screen opened 3 ms before this, its knives took the worn units; the level then had the checkpoint's):
    // its knives' units are the checkpoint's too.
    if (g_organiser.screen)
        for (int grid = 0; grid < 9; ++grid) {
            const int k = i * 9 + grid;
            if (!IsKnifeItem(g_organiser.ids[k]) || inv[i][grid] < -1.0f) continue;
            g_organiser.units[k] = inv[i][grid];
            g_organiserUnits[k].store(inv[i][grid], std::memory_order_relaxed);
            g_savedInv[i][grid] = inv[i][grid];
            LOG("knife durability: Organiser, player %d's cell %d: the checkpoint's units (%.0f)", i + 1, grid,
                Units(inv[i][grid], g_organiser.ids[k]));
        }
    g_seenChoice[i] = KnifeSlotChoice(i);  // the main place's choice put back is no new knife
    g_knives[i].brokenFrom = -1;
    g_durabilityDirty = true;
}

// On the Organiser screen (no player) the screen is the source, as its own sounds are played (VA 0x9E4A20).
void PlayMenuSound(int id) {
    const uintptr_t p1 = actors::Player(0);
    uintptr_t ope = 0, sound = 0;
    if (!p1 || !mem::Read(p1 + 0x2654, ope) || !ope) ope = OrganiserScreenNow();
    if (!ope || !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB1D0C)), sound) || !sound) return;
    using PlayFn = void(__fastcall*)(void* sound, void* edx, int id, void* source);
    __try {
        reinterpret_cast<PlayFn>(mem::Rva(0x39F790))(reinterpret_cast<void*>(sound), nullptr, id, reinterpret_cast<void*>(ope));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void PlayPutDownSound() { PlayMenuSound(0x3A); }

// "Mettre le couteau sélectionné dans l'étui" (user, 2026-10-27): the knife under the inventory's cursor goes into its
// owner's sheath as the second knife, its units with it; its cell is freed. Any weapon the knife slot takes since
// 2026-11-05 (user: "any weapon in that second place"; a firearm keeps its rounds, inventory.cpp). The cursor: player 1's cOpeWepSlot
// [P+0x2654], +0x08 the cell (0-8 by rows, = the records' grid +0x18), +0x10 the grid (0 his own, 1 the partner's),
// +0x68 & 1 open (recorded 2026-10-27 while the user moved it cell by cell).
// The same on the Organiser screen between chapters (user, 2026-11-05): player 1's cursor cell (OrganiserCursor) of
// Chris's or Sheva's grid into the highlighted place of that character's sheath (page = player); the place's weapon
// back into the cell, its record made by the game's own fill from an inventory entry (VA 0xA35AD0 thiscall(screen,
// record, entry) ret 8, what the screen does for each cell at its opening, VA 0xA016F7: id, count, the equip state
// +0x18, the levels +0x1C..+0x24 - none here, as an inventory weapon added in a level). The cell's knife units follow.
bool FillOrganiserCell(uintptr_t screen, uintptr_t record, const void* entry) {
    using FillFn = void(__fastcall*)(uintptr_t screen, void* edx, uintptr_t record, const void* entry);
    __try {
        reinterpret_cast<FillFn>(mem::Rva(0x635AD0))(screen, nullptr, record, entry);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MoveOrganiserWeaponToSheath(uintptr_t screen) {
    int page = 0, cell = 0;
    if (!OrganiserCursor(screen, page, cell)) {
        Toast("Place le curseur sur un couteau ou une arme de l'étui dans la grille de Chris ou de Sheva", false);
        return false;
    }
    const int k = page * 9 + cell;
    const uintptr_t record = screen + kOrganiserPages + 0x48 * k;
    uint16_t id = 0;
    int16_t count = 0;
    uint8_t equipped = 0;
    if (!mem::Read(record, id) || !mem::Read(record + 2, count) || !mem::Read(record + 4, equipped)) return false;
    if (!id) {  // an empty cell: the highlighted place's weapon into it (user, 2026-11-05)
        LoadDurability();
        const float units = PlaceUnits(page, SheathActivePlace(page)).load();
        uint16_t item = 0;
        int32_t rounds = -1;
        int place = 0;
        if (!SheathPlaceOut(page, item, rounds, place)) return SheathRefused();
        if (!EmptySheathPlace(page, place, item)) return false;
        uint8_t entry[0x2C] = {};
        const uint32_t amount = static_cast<uint32_t>(rounds >= 0 ? rounds : 1);
        const uint32_t fields[3] = { item, amount, amount };
        std::memcpy(entry, fields, sizeof(fields));
        const uint32_t grid = static_cast<uint32_t>(cell);
        std::memcpy(entry + 0x14, &grid, 4);
        entry[0x25] = 1;
        uint8_t blank[0x48] = {};
        mem::SafeWrite(reinterpret_cast<void*>(record), blank, sizeof(blank));
        if (!FillOrganiserCell(screen, record, entry)) LOG("sheath: the Organiser's cell fill faulted");
        {
            std::lock_guard<std::mutex> lock(g_invLock);
            const float cellUnits = IsKnifeItem(item) ? units : -2.0f;
            if (g_organiser.screen == screen) {
                g_organiser.ids[k] = item;
                g_organiser.units[k] = cellUnits;
            }
            g_organiserUnits[k].store(cellUnits, std::memory_order_relaxed);
            g_savedInv[page][cell] = cellUnits;
        }
        PlaceUnits(page, place) = -1.0f;
        NoteOrganiserCellOut(page, static_cast<uint32_t>(cell), item, rounds);
        g_durabilityDirty = true;
        PlayPutDownSound();
        LOG("knife durability: Organiser, player %d's place %d out into cell %d (%04X, %d rounds, %.0f units)", page + 1,
            place + 1, cell, item, rounds, Units(units, item));
        return true;
    }
    if (!SheathTakes(id)) {
        Toast("L'étui ne prend que les armes", false);
        return false;
    }
    if (equipped) {  // its "E": the weapon in hand at the next level
        RefuseInMenu("Impossible de ranger un objet équipé dans l'étui");
        return SheathRefused();
    }
    LoadDurability();
    float units = -1.0f;
    {
        std::lock_guard<std::mutex> lock(g_invLock);
        if (g_organiser.screen == screen && IsKnifeItem(id)) units = g_organiser.units[k];
    }
    const float oldUnits = PlaceUnits(page, SheathActivePlace(page)).load();
    uint16_t old = 0;
    int32_t oldRounds = -1;
    int place = 0;
    if (!OrganiserIntoSheath(page, id, count, old, oldRounds, place)) return SheathRefused();
    ForgetOrganiserCellOut(page, static_cast<uint32_t>(cell));
    uint8_t blank[0x48] = {};
    mem::SafeWrite(reinterpret_cast<void*>(record), blank, sizeof(blank));
    if (old) {
        uint8_t entry[0x2C] = {};
        const uint32_t amount = static_cast<uint32_t>(oldRounds >= 0 ? oldRounds : 1);
        const uint32_t fields[3] = { old, amount, amount };
        std::memcpy(entry, fields, sizeof(fields));
        const uint32_t grid = static_cast<uint32_t>(cell);
        std::memcpy(entry + 0x14, &grid, 4);
        entry[0x25] = 1;
        if (!FillOrganiserCell(screen, record, entry)) LOG("sheath: the Organiser's cell fill faulted");
        LOG("sheath: Organiser cell %d.%d now %04X: %s", page, cell, old, mem::Hex(reinterpret_cast<const void*>(record), 0x48).c_str());
    }
    {
        std::lock_guard<std::mutex> lock(g_invLock);
        const float cellUnits = old && IsKnifeItem(old) ? oldUnits : -2.0f;
        if (g_organiser.screen == screen) {
            g_organiser.ids[k] = old;
            g_organiser.units[k] = cellUnits;
        }
        g_organiserUnits[k].store(cellUnits, std::memory_order_relaxed);
        g_savedInv[page][cell] = cellUnits;
    }
    PlaceUnits(page, place) = units;
    if (place == 0) {
        g_seenChoice[page] = KnifeSlotChoice(page);
        g_knives[page].brokenFrom = -1;
    }
    g_durabilityDirty = true;
    PlayPutDownSound();
    LOG("knife durability: Organiser, player %d's weapon of cell %d in place %d (%.0f units)", page + 1, cell, place + 1,
        Units(units, id));
    return true;
}

bool MoveSelectedKnifeToSheath() {
    if (!actors::InGame())
        if (const uintptr_t screen = OrganiserScreenNow()) return MoveOrganiserWeaponToSheath(screen);
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t ope = 0;
    uint32_t flags = 0, cell = 0xD, side = 0;
    if (!p1 || !mem::Read(p1 + 0x2654, ope) || !ope || !mem::Read(ope + 0x68, flags) || !(flags & 1) ||
        !mem::Read(ope + 8, cell) || !mem::Read(ope + 0x10, side)) {
        Toast("Ouvre l'inventaire et place le curseur sur un couteau ou une arme de l'étui", false);
        return false;
    }
    const int player = side == 1 ? 1 : 0;
    const uintptr_t p = actors::Player(player);
    uintptr_t record = 0;
    bool other = false;  // a weapon the sheath does not take
    for (int k = 0; k < 24 && p && !record; ++k) {
        const uintptr_t r = ItemRecordAddress(p, k);
        uint32_t grid = 0xD, id = 0;
        if (!r || !mem::Read(r + 0x18, grid) || grid != cell || !mem::Read(r + 4, id)) continue;
        if (SheathTakes(static_cast<uint16_t>(id))) record = r;
        else other = true;
    }
    if (!record && !other && cell < 9) {  // an empty cell: the highlighted place's weapon into it (user, 2026-11-05)
        LoadDurability();
        const float units = PlaceUnits(player, SheathActivePlace(player)).load();
        uint16_t item = 0;
        int place = 0;
        if (!SheathWeaponIntoCell(player, cell, item, place)) return SheathRefused();
        {
            std::lock_guard<std::mutex> lock(g_invLock);
            if (IsKnifeItem(item)) g_savedInv[player][cell] = units;  // the new record's knife takes them
        }
        PlaceUnits(player, place) = -1.0f;
        g_durabilityDirty = true;
        PlayPutDownSound();
        LOG("knife durability: player %d's place %d out into cell %u (%04X, %.0f units)", player + 1, place + 1, cell, item,
            Units(units, item));
        return true;
    }
    if (!record) {
        Toast("L'étui ne prend que les armes", false);
        return false;
    }
    LoadDurability();
    float units = -1.0f;
    {
        std::lock_guard<std::mutex> lock(g_invLock);
        if (const InventoryKnife* knife = FindInventoryKnife(player, record)) units = knife->units;
    }
    const int target = SheathActivePlace(player);
    const float oldUnits = PlaceUnits(player, target).load();
    uint16_t swapped = 0;
    int place = 0;
    if (!MoveKnifeToSheath(player, record, swapped, place)) return SheathRefused();  // a refusal has its own message
    {
        // Its record is no longer this knife; the knife that went back to the cell keeps its units.
        std::lock_guard<std::mutex> lock(g_invLock);
        auto& list = g_invKnives[player];
        list.erase(std::remove_if(list.begin(), list.end(), [record](const InventoryKnife& k) { return k.record == record; }),
                   list.end());
        if (swapped && cell < 9 && IsKnifeItem(swapped)) g_savedInv[player][cell] = oldUnits;
    }
    PlaceUnits(player, place) = units;
    if (place == 0) {  // the main place's new choice is no new knife: its units kept
        g_seenChoice[player] = KnifeSlotChoice(player);
        g_knives[player].brokenFrom = -1;
    }
    g_durabilityDirty = true;
    PlayPutDownSound();
    LOG("knife durability: player %d's weapon of cell %u in place %d (%.0f units)", player + 1, cell, place + 1,
        Units(units, SheathPlaceItem(player, place)));
    return true;
}

// "Jeter l'arme de l'étui surligné" (user, 2026-11-05): the highlighted place of the sheath whose grid the inventory's
// cursor is on (player 1's when it is closed) emptied; the place's units full for its next weapon.
bool DiscardHighlightedSheath() {
    const uintptr_t p1 = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t ope = 0;
    uint32_t side = 0;
    if (p1 && mem::Read(p1 + 0x2654, ope) && ope) mem::Read(ope + 0x10, side);
    int player = side == 1 && actors::Player(1) ? 1 : 0;
    if (!actors::InGame())
        if (const uintptr_t screen = OrganiserScreenNow()) player = OrganiserCursorPage(screen);  // the cursor's grid
    int place = 0;
    if (!DiscardSheathPlace(player, place)) return SheathRefused();
    PlaceUnits(player, place) = -1.0f;
    g_durabilityDirty = true;
    PlayPutDownSound();
    return true;
}

// "Changer de couteau dans l'étui" (user, 2026-10-27; 2026-11-05 the places in turn, the empty ones too, so a weapon can
// be moved into any of them): the next place highlighted, its weapon in the slot.
bool SwitchSheathKnife(int player) {
    const int count = SheathPlaceCount();
    if (count < 2) {
        Toast("L'étui n'a qu'un emplacement", false);
        return false;
    }
    if (!actors::InGame()) KeepSlotCopiesRounds(player);  // the Organiser: the gun leaving the slot keeps its rounds
    // The inventory closed (user, 2026-11-05): only the places holding a weapon; open (or the Organiser), every place.
    const bool everyPlace = Player1InventoryOpen() || OrganiserScreenNow() != 0;
    int place = (SheathActivePlace(player) + 1) % count;
    if (!everyPlace) {
        int tries = 0;
        while (!SheathPlaceItem(player, place) && ++tries < count) place = (place + 1) % count;
        if (!SheathPlaceItem(player, place) || place == SheathActivePlace(player)) {
            Toast("L'étui n'a pas d'autre arme", false);
            return false;
        }
    }
    if (!SetSheathActivePlace(player, place)) return false;
    g_durabilityDirty = true;
    PlayPutDownSound();
    // In a level the inventory is not in sight (user, 2026-11-05: "the key in the game too, not only in the inventory"):
    // the place and its weapon in the message.
    const uint16_t item = SheathPlaceItem(player, place);
    char text[160];
    std::snprintf(text, sizeof(text), "%s %d : %s", i18n::Tr("Étui : emplacement"), place + 1,
                  item ? i18n::Tr(ItemDisplayName(item).c_str()) : i18n::Tr("vide"));
    Toast(text);
    return true;
}

// "Couteaux lâchés par les ennemis" (user, 2026-10-27: knives were only found by giving them with the trainer). The
// drops: the item manager's VA 0x7C0CA0 (thiscall: position, direction, ...) asks VA 0x7BF450 (thiscall(&id, &amount, a,
// b), ret 0x10) whether something drops and what, then fills a set-item record (+0x76 id, +0x78 amount) for VA
// 0x7BE550, which makes the unit of class id (0x70000 | id) << 8 (VA 0x6D0690 by the DTIs' +0x20: uIt0201 0x07020100,
// uWp01 0x07010100 - a weapon is a uItem, it drops as a stage's weapon lies). Found with diag.item_new_log (uItem's
// constructor VA 0xA79060) while the user killed Majini. After the game's choice, a roll by player 1's usable knives
// (the main one not broken, the second one, the inventory's) may make it one of the four knives, amount 1. Never
// without "Durabilité du couteau" (an endless knife needs no other).
using DropFn = bool(__fastcall*)(void* items, void* edx, uint32_t* id, uint32_t* amount, int a, int b);
DropFn g_drop = nullptr;
std::atomic<bool> g_dropOn{ false };
std::atomic<float> g_dropChance[4]{ { 30.0f }, { 15.0f }, { 5.0f }, { 0.0f } };  // 0, 1, 2, 3+ knives
constexpr const uint16_t (&kDropKnives)[4] = kKnifeTypes;
constexpr const char* kDropArchives[4] = { "uWp01", "uWp0e", "uWp23", "uWp24" };
std::atomic<bool> g_dropLoaded[4]{};

// The beam of the dropped knives (user, 2026-10-28: "difficile à repérer, pas le grand faisceau de couleur"). An item's
// beam is its sub-object (+0x1A70) SetBeam VA 0xA78DD0 (thiscall(on, unit, blink), ret 0xC; the effect handle at its
// +0xB4, released at every call, off at the pickup), found with diag.effect_log (core\cr0067 / cr0068 spawned from RVA
// 0x678F34). It picks a core effect from the item id (+0x86 = item +0x1AF6): category id >> 8 (weapons and ammo 0xB,
// recovery 0xC, 4 -> 0xA, 5 -> 0xF; +0x5C while it blinks before vanishing) and, for weapons, a byte table from 0x106:
// only the thrown ones keep a beam (0x106-0x108 / 0x10A 0xB, 0x136 / 0x13C-0x13E 0xC), every other weapon has none -
// knives included, no enemy dropping weapons. For a knife a drop made, the call sees the hand grenade's id 0x0106, the
// knife's own back right after (same thread). Not for a knife thrown away (a broken one: picked up again it would be
// full): the drop manager's units VA 0x7C0CA0 (thiscall, 5 arguments, ret 0x14, returns the unit it made) are kept
// while HookDrop made the drop a knife.
using DropSpawnFn = uintptr_t(__fastcall*)(void* items, void* edx, void* a, void* b, int c, int d, int e);
using BeamFn = void(__fastcall*)(void* self, void* edx, int on, void* unit, int blink);
DropSpawnFn g_dropSpawn = nullptr;
BeamFn g_beam = nullptr;
thread_local bool t_knifeDropped = false;  // HookDrop's knife, inside VA 0x7C0CA0
std::mutex g_dropUnitsLock;
std::vector<uintptr_t> g_dropUnits;  // the last knives dropped (game thread)

uintptr_t __fastcall HookDropSpawn(void* items, void* edx, void* a, void* b, int c, int d, int e) {
    t_knifeDropped = false;
    const uintptr_t unit = g_dropSpawn(items, edx, a, b, c, d, e);
    if (t_knifeDropped && unit) {
        std::lock_guard<std::mutex> lock(g_dropUnitsLock);
        g_dropUnits.push_back(unit);
        if (g_dropUnits.size() > 16) g_dropUnits.erase(g_dropUnits.begin());
        LOG("knife drop: unit %08X, its beam given", static_cast<unsigned>(unit));
    }
    t_knifeDropped = false;
    return unit;
}

void __fastcall HookBeam(void* self, void* edx, int on, void* unit, int blink) {
    const uintptr_t s = reinterpret_cast<uintptr_t>(self);
    uint16_t id = 0;
    bool dropped = false;
    if (on && g_dropOn.load(std::memory_order_relaxed) && mem::Read(s + 0x86, id) && KnifeType(id) >= 0) {
        std::lock_guard<std::mutex> lock(g_dropUnitsLock);
        dropped = std::find(g_dropUnits.begin(), g_dropUnits.end(), s - 0x1A70) != g_dropUnits.end();
    }
    if (!dropped) {
        g_beam(self, edx, on, unit, blink);
        return;
    }
    mem::Write<uint16_t>(s + 0x86, 0x0106);
    g_beam(self, edx, on, unit, blink);
    mem::Write<uint16_t>(s + 0x86, id);
    LOG("knife drop: beam of %04X on unit %08X (blink %d)", id, static_cast<unsigned>(s - 0x1A70), blink);
}

bool HookAt(uint32_t rva, const std::vector<uint8_t>& expected, void* detour, void** original, const char* what) {
    uint8_t* target = mem::Rva(rva);
    if (!mem::IsGameCode(target, expected.size()) || std::memcmp(target, expected.data(), expected.size()) != 0) {
        LOG("%s: unexpected bytes %s", what, mem::Hex(target, expected.size()).c_str());
        return false;
    }
    mem::EnsureExecutable(target, expected.size());
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    LOG("%s hook installed at %p", what, target);
    return true;
}

// The Organiser screen's destructor (its vtable's first slot, VA 0xA0F860: its destructor VA 0xA01560, then the free):
// the screen forgotten before its memory goes.
using OrganiserDtorFn = void*(__fastcall*)(void* self, void* edx, int flags);
OrganiserDtorFn g_organiserDtor = nullptr;

void* __fastcall HookOrganiserDtor(void* self, void* edx, int flags) {
    if (reinterpret_cast<uintptr_t>(self) == g_organiserScreen.load()) {
        g_organiserScreen.store(0);
        LOG("Organiser screen %p closed", self);
    }
    return g_organiserDtor(self, edx, flags);
}

void EnsureOrganiserDtorHook() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    g_organiserDtorHooked = HookAt(0x60F860, { 0x56, 0x8B, 0xF1, 0xE8 }, reinterpret_cast<void*>(&HookOrganiserDtor),
                                   reinterpret_cast<void**>(&g_organiserDtor), "Organiser screen destructor");
}

bool EnsureBeamHooks() {
    static bool hooked = false;
    if (hooked) return true;
    hooked = HookAt(0x3C0CA0, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC }, reinterpret_cast<void*>(&HookDropSpawn),
                    reinterpret_cast<void**>(&g_dropSpawn), "knife drop: drop manager") &&
             HookAt(0x678DD0, { 0x83, 0xEC, 0x10, 0x53, 0x56, 0x8B, 0xF1 }, reinterpret_cast<void*>(&HookBeam),
                    reinterpret_cast<void**>(&g_beam), "knife drop: item beam");
    return hooked;
}

int UsableKnives() {
    int count = 0;
    for (int k = 0; k < SheathPlaceCount(); ++k)
        if (const uint16_t id = SheathPlaceItem(0, k); id && IsKnifeItem(id) && !Broken(PlaceUnits(0, k))) ++count;
    std::lock_guard<std::mutex> lock(g_invLock);
    for (const InventoryKnife& knife : g_invKnives[0])
        if (knife.units < 0.0f || knife.units > 0.0f) ++count;
    return count;
}

bool __fastcall HookDrop(void* items, void* edx, uint32_t* id, uint32_t* amount, int a, int b) {
    const bool drop = g_drop(items, edx, id, amount, a, b);
    if (!g_dropOn.load(std::memory_order_relaxed) || !g_durabilityOn.load(std::memory_order_relaxed) || !id || !amount)
        return drop;
    const int knives = UsableKnives();
    const float chance = g_dropChance[std::min(knives, 3)].load(std::memory_order_relaxed);
    static std::minstd_rand random(GetTickCount());
    if (chance <= 0.0f || std::uniform_real_distribution<float>(0.0f, 100.0f)(random) >= chance) return drop;
    // Any of the four, owned or not (OwnedCheck below lets a knife already owned be picked up).
    int pick = static_cast<int>(random() % 4);
    for (int k = 0; k < 4 && !g_dropLoaded[pick].load(); ++k) pick = (pick + 1) % 4;
    if (!g_dropLoaded[pick].load()) return drop;
    LOG("knife drop: %04X instead of %s%04X (%d usable knife(s), %.0f %%)", kDropKnives[pick], drop ? "" : "nothing / ",
        drop ? *id & 0xFFFF : 0, knives, chance);
    *id = kDropKnives[pick];
    *amount = 1;
    t_knifeDropped = true;  // its unit gets the beam (HookDropSpawn)
    return true;
}

bool EnsureDropHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x3BF450);
    const uint8_t expected[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45, 0x08 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("knife drop: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookDrop), reinterpret_cast<void**>(&g_drop));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("knife drop hook installed at %p", target);
    return true;
}

// A knife already owned picked up all the same (user, 2026-10-28: "comme le jeu accepte de prendre une grenade même si
// j'en ai déjà"; the game said "inventory full"). The room test P.VA 0xB65BF0(type, a, b), for a weapon whose table
// entry (VA 0x11B5170 + index * 0x140) +0x130 is 0, asks the inventory (P+0x21A0) VA 0xC4B5D0(type, 0) for a record of
// that type and refuses when there is one. The adder (VA 0x7E9DF0 -> VA 0xC4B480) never looks for one: it always takes
// a free record, so the second knife of a kind gets its own cell. The call at RVA 0x765C7C goes to OwnedCheck: no record
// found for the four knives while "Couteaux lâchés par les ennemis" is on.
using OwnedFn = uintptr_t(__fastcall*)(void* inventory, void* edx, uint32_t type, int index);

uintptr_t __fastcall OwnedCheck(void* inventory, void* edx, uint32_t type, int index) {
    if (g_dropOn.load(std::memory_order_relaxed))
        for (uint16_t knife : kDropKnives)
            if ((type & 0xFFFF) == knife) return 0;
    return reinterpret_cast<OwnedFn>(mem::Rva(0x84B5D0))(inventory, edx, type, index);
}

mem::Patch* g_owned = nullptr;

void SetOwnedKnives(bool on) {
    if (!g_owned) {
        if (!on) return;
        uint8_t* site = mem::Rva(0x765C7C);
        const std::vector<uint8_t> original = { 0xE8, 0x4F, 0x59, 0x0E, 0x00 };  // call VA 0xC4B5D0
        const int32_t rel =
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(&OwnedCheck) - reinterpret_cast<uintptr_t>(site + 5));
        std::vector<uint8_t> patched = { 0xE8 };
        for (int b = 0; b < 4; ++b) patched.push_back(static_cast<uint8_t>(rel >> (8 * b)));
        g_owned = new mem::Patch("knife drop: knives already owned picked up", site, original, patched);
    }
    if (on != g_owned->IsApplied()) on ? g_owned->Apply() : g_owned->Restore();
}

// The knives' archives held, so a dropped knife has its model (a level only loads the weapons it holds).
void LoadDropKnives() {
    for (int k = 0; k < 4; ++k)
        if (!g_dropLoaded[k].load() && LoadWeaponArchive(kDropArchives[k])) g_dropLoaded[k] = true;
}

void KnifeWearHit(int player) { Wear(player, g_wearHit.load(std::memory_order_relaxed), "hit"); }
void KnifeWearParry() { Wear(0, g_wearParry.load(std::memory_order_relaxed), "parry"); }
void KnifeWearRe6Move(int player, int which) {
    static const char* const kWhat[3] = { "knife counter", "throat cut", "knife Ground" };
    if (which >= 0 && which < 3) Wear(player, g_wearRe6Move[which].load(std::memory_order_relaxed), kWhat[which]);
}

// The Combat tab in two sub-tabs (user, 2026-10-28: "une page spéciale destinée au couteau", everything about the knife
// in one place, its staggers included): the knife's options in four sections. The knife parry stays in the Parade tab.
void DrawCombatPanel() {
    if (!ImGui::BeginTabBar("combat")) return;
    if (ImGui::BeginTabItem(i18n::Tr("Général"))) {
        ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_None);
        DrawOptions("Combat");
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(i18n::Tr("Couteau"))) {
        ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_None);
        ImGui::TextDisabled(i18n::Tr("La parade au couteau et ses réglages sont dans l'onglet Parade."));
        ImGui::TextDisabled(i18n::Tr("L'étui (l'emplacement du couteau) est dans l'onglet Éditeur d'inventaire, page Général."));
        static constexpr const char* kSections[][2] = {
            { "Attaques et vacillement", "Couteau" },
            { "Couteau rapide et visée", "Couteau : rapide" },
            { "Durabilité", "Couteau : durabilité" },
        };
        for (const auto& section : kSections) {
            ImGui::SeparatorText(section[0]);
            ImGui::PushID(section[1]);
            DrawOptions(section[1]);
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

// ("Par type de couteau", the four knives' power and durability, became the knives' rows of the weapon table on
// 2026-11-05; its empty section removed, user 2026-11-06.)

void RegisterKnifeIcon() {
    OnFrame([] {
        EnsureOrganiserPatches();
        EnsureOrganiserDtorHook();
    });
    Add({ .id = "hud.knife_slot_icon", .name = "Icône de l'étui à côté du nom",
          .help = "Affiche l'icône de l'objet rangé dans l'étui du couteau à côté du nom de chaque joueur sur le HUD (le "
                  "couteau de Chris à côté de Chris, celui de Sheva à côté de Sheva ; un pistolet mis dans l'étui montre "
                  "le pistolet). Celle du joueur 2 n'apparaît pas quand « Joueur 2 invisible » cache son HUD.",
          .category = "Couteau : étui", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureCockpitWatch()) return false;
              g_on = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Feature& durability = Add({ .id = "combat.knife_durability", .name = "Durabilité du couteau",
                                .help = "Chaque couteau a des unités de résistance (colonne « Résistance » du tableau, onglet "
                                        "Réglages des armes), affichées au-dessus de l'icône de son étui. Chaque coup de "
                                        "couteau qui touche un ennemi et chaque parade au "
                                        "couteau (riposte au couteau, pas la garde de Wesker, les projections ni le "
                                        "contre RE6) en retirent (curseurs d'usure). À 0, le couteau est retiré : l'étui "
                                        "passe à « Rien » et ce joueur ne peut plus libérer son partenaire au couteau "
                                        "(parasite, parasite volant, langue de licker). « Réparer le couteau » le rend.",
                                .category = "Couteau : durabilité", .kind = Kind::Toggle,
                                .available = [] { return game::IsSupported(); } });
    // Its units slider gone (user, 2026-11-05: "every knife's durability is set in the table").
    durability.set = [](bool on) {
        if (on && (!EnsureCockpitWatch() || !EnsureEnemyOnHitHook())) return false;
        LoadDurability();
        g_durabilityOn = on;
        return true;
    };
    durability.tick = [] { TickDurability(); };
    Add({ .id = "combat.knife_durability_display", .name = "Durabilité : affichage",
          .help = "Chiffre : les unités au-dessus de l'icône de l'étui. Barre : une jauge au-dessus de l'icône. Opacité de "
                  "l'icône : l'icône du couteau pâlit avec l'usure (jusqu'à 15 % d'opacité) et disparaît quand il casse "
                  "(affichée même sans « Icône de l'étui à côté du nom »).",
          .category = "Couteau : durabilité", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              g_display = std::clamp(index, 0, 2);
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 2, .valueFormat = "%.0f",
          .choices = { "Chiffre", "Barre", "Opacité de l'icône" } });
    struct Slider {
        const char* id;
        const char* name;
        const char* help;
        float value, max;
        std::atomic<float>* target;
    };
    static const Slider kWear[] = {
        { "combat.knife_wear_hit", "Durabilité : usure par coup",
          "Unités retirées au couteau d'un joueur par coup de couteau qui touche un ennemi (pas les coups dans le vide).",
          1.0f, 50.0f, &g_wearHit },
        { "combat.knife_wear_parry", "Durabilité : usure par parade",
          "Unités retirées au couteau du joueur 1 par parade au couteau réussie (riposte au couteau seulement).", 5.0f,
          100.0f, &g_wearParry },
        { "combat.knife_wear_re6_counter", "Durabilité : usure par contre au couteau (RE6)",
          "Unités retirées au couteau en main du joueur 1 par contre au couteau de RE6 (une fois par contre, à son coup).", 5.0f,
          100.0f, &g_wearRe6Move[0] },
        { "combat.knife_wear_re6_throat_cut", "Durabilité : usure par égorgement (RE6)",
          "Unités retirées au couteau en main du joueur 1 par égorgement de RE6 (une fois par égorgement, à son coup).", 5.0f,
          100.0f, &g_wearRe6Move[1] },
        { "combat.knife_wear_re6_ground", "Durabilité : usure par Ground au couteau (RE6)",
          "Unités retirées au couteau en main du joueur 1 par Ground au couteau de RE6 (une fois par coup au sol, quand il "
          "touche).", 3.0f, 100.0f, &g_wearRe6Move[2] },
    };
    for (const Slider& s : kWear) {
        Feature& f = Add({ .id = s.id, .name = s.name, .help = s.help, .category = "Couteau : durabilité",
                           .kind = Kind::Toggle, .set = [](bool) { return true; },
                           .available = [] { return game::IsSupported(); },
                           .value = s.value, .valueMin = 0.0f, .valueMax = s.max, .valueFormat = "%.0f unités",
                           .defaultOn = true });
        Feature* const fp = &f;
        std::atomic<float>* const target = s.target;
        f.tick = [fp, target] { target->store(fp->value, std::memory_order_relaxed); };
    }
    Feature& sound = Add({ .id = "combat.knife_break_sound", .name = "Son quand le couteau casse",
                           .help = "Avec « Durabilité du couteau » : joue un son quand un couteau casse en main (étui ou "
                                   "inventaire). Le son vient de ton RE6 (fichier sounds\\knife_break.wav du dossier du"
                                   "trainer). Le curseur règle son volume (au-delà de 100 %, un limiteur évite la saturation).",
                           .category = "Couteau : durabilité", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = 100.0f, .valueMin = 0.0f, .valueMax = 800.0f, .valueFormat = "%.0f %%" });
    Feature* const sp = &sound;
    sound.set = [sp](bool on) {
        if (!on) {
            g_breakSoundOn = false;
            g_breakSound.Close();
            return true;
        }
        std::string error;
        if (!g_breakSound.Load(L"knife_break.wav", sp->value / 100.0f, error)) {
            sp->lastError = error;
            LOG("knife break sound: %s", error.c_str());
            return false;
        }
        g_breakSoundOn = true;
        return true;
    };
    sound.tick = [sp] {  // the volume changed: the samples again (the sound off meanwhile, nothing plays old ones)
        if (sp->value / 100.0f == g_breakSound.Gain()) return;
        std::string error;
        g_breakSoundOn = false;
        if (g_breakSound.Load(L"knife_break.wav", sp->value / 100.0f, error)) g_breakSoundOn = true;
        else LOG("knife break sound: %s", error.c_str());
    };
    OnRestore([] {
        g_breakSoundOn = false;
        g_breakSound.Close();
    });
    Add({ .id = "combat.knife_to_sheath", .name = "Mettre l'arme sélectionnée dans l'étui",
          .help = "Inventaire ou Organiser ouvert, curseur sur une arme (couteau, matraque, arme à feu, lanceur, grenade, "
                  "mine, œuf ; la tienne ou celle du partenaire) : elle prend la place de l'arme de l'emplacement surligné de "
                  "l'étui (« Changer de couteau dans l'étui » change d'emplacement), qui revient dans la case avec ses "
                  "balles ou sa durabilité. L'emplacement 1 est le choix du menu « Emplacement couteau ». Un couteau "
                  "cassé disparaît et le principal revient (le principal cassé, lui, reste dans l'étui). L'arme équipée "
                  "ne peut pas être rangée. Sur une case vide : l'arme de l'emplacement surligné y revient et "
                  "l'emplacement se vide.",
          .category = "Couteau : étui", .kind = Kind::Action,
          .run = [] { return MoveSelectedKnifeToSheath(); },
          .available = [] { return game::IsSupported(); } });
    // Its key may be shared with the belt lamp (user, 2026-10-27): nothing out of the inventory.
    Find("combat.knife_to_sheath")->hotkeyIgnored = [] { return !Player1InventoryOpen() && !OrganiserScreenNow(); };
    Add({ .id = "combat.sheath_discard", .name = "Jeter l'arme de l'étui surligné",
          .help = "Inventaire ou Organiser ouvert : l'arme de l'emplacement surligné de l'étui (le tien, ou celui du partenaire si le "
                  "curseur est sur sa grille) est jetée et l'emplacement se vide (pour l'emplacement 1, le menu passe à "
                  "« Rien »). Un emplacement vide surligné est un étui vide : aucun couteau ne sort.",
          .category = "Couteau : étui", .kind = Kind::Action,
          .run = [] { return DiscardHighlightedSheath(); },
          .available = [] { return game::IsSupported(); } });
    Find("combat.sheath_discard")->hotkeyIgnored = [] { return !Player1InventoryOpen() && !OrganiserScreenNow(); };
    for (int i = 0; i < 2; ++i) {
        static const char* ids[2] = { "combat.knife_switch_p1", "combat.knife_switch_p2" };
        static const char* names[2] = { "Changer de couteau dans l'étui — J1", "Changer de couteau dans l'étui — J2" };
        Feature& sw = Add({ .id = ids[i], .name = names[i],
                            .help = "Passe à l'emplacement suivant de l'étui (1, 2, 3... jusqu'au dernier, puis de "
                                    "nouveau 1 ; leur nombre : « Étui : nombre d'emplacements »), les vides "
                                    "compris : l'arme de l'emplacement surligné est celle de l'étui en jeu, et c'est avec "
                                    "lui que « Mettre l'arme sélectionnée dans l'étui » échange. Les autres gardent leur "
                                    "durabilité ou leurs balles. Le raccourci marche en jeu (inventaire fermé : seulement les "
                                    "emplacements qui ont une arme ; un message donne l'emplacement et son arme), inventaire "
                                    "ou Organiser ouvert (tous les emplacements).",
                            .category = "Couteau : étui", .kind = Kind::Action,
                            .run = [i] { return SwitchSheathKnife(i); },
                            .available = [] { return actors::InGame() || OrganiserScreenNow() != 0; } });
        // In the inventory, on the Organiser screen and in the game too (user, 2026-11-05: "not only in the inventory,
        // it makes me open it to go from one sheath weapon to the next"; before, its key could be shared with an in-game
        // shortcut).
        (void)sw;
    }
    Feature& drop = Add({ .id = "combat.knife_drop", .name = "Couteaux lâchés par les ennemis",
                          .help = "Avec « Durabilité du couteau » : un ennemi qui meurt peut lâcher un des quatre couteaux "
                                  "(à la place de l'objet qu'il aurait lâché), d'autant plus souvent que tu as peu de "
                                  "couteaux utilisables (principal en état, second couteau, couteaux de l'inventaire) - "
                                  "voir les curseurs de chance. Sans la durabilité, jamais. Un couteau que tu as déjà "
                                  "se ramasse quand même (il prend sa propre case).",
                          .category = "Couteau : durabilité", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); } });
    drop.set = [](bool on) {
        if (on && (!EnsureDropHook() || !EnsureBeamHooks())) return false;
        if (on && actors::InGame()) LoadDropKnives();
        SetOwnedKnives(on);
        g_dropOn = on;
        return true;
    };
    OnRestore([] { SetOwnedKnives(false); });
    drop.tick = [] {
        static DWORD tried = 0;  // a level loaded since: the archives again (held by the trainer, at most every 5 s)
        if (actors::InGame() && GetTickCount() - tried > 5000) {
            tried = GetTickCount();
            LoadDropKnives();
        }
    };
    static const Slider kChances[] = {
        { "combat.knife_drop_0", "Couteaux lâchés : chance sans couteau",
          "Chance qu'un ennemi lâche un couteau quand tu n'as aucun couteau utilisable.", 30.0f, 100.0f, &g_dropChance[0] },
        { "combat.knife_drop_1", "Couteaux lâchés : chance avec 1 couteau",
          "Chance avec un seul couteau utilisable (par exemple le principal).", 15.0f, 100.0f, &g_dropChance[1] },
        { "combat.knife_drop_2", "Couteaux lâchés : chance avec 2 couteaux",
          "Chance avec deux couteaux utilisables (par exemple le principal et le second couteau).", 5.0f, 100.0f,
          &g_dropChance[2] },
        { "combat.knife_drop_3", "Couteaux lâchés : chance avec 3 couteaux ou plus",
          "Chance avec trois couteaux utilisables ou plus (étui et inventaire).", 0.0f, 100.0f, &g_dropChance[3] },
    };
    for (const Slider& s : kChances) {
        Feature& f = Add({ .id = s.id, .name = s.name, .help = s.help, .category = "Couteau : durabilité",
                           .kind = Kind::Toggle, .set = [](bool) { return true; },
                           .available = [] { return game::IsSupported(); },
                           .value = s.value, .valueMin = 0.0f, .valueMax = s.max, .valueFormat = "%.0f %%",
                           .defaultOn = true });
        Feature* const fp = &f;
        std::atomic<float>* const target = s.target;
        f.tick = [fp, target] { target->store(fp->value, std::memory_order_relaxed); };
    }
    Add({ .id = "combat.knife_repair", .name = "Réparer le couteau principal",
          .help = "Rend au couteau principal de l'étui de chaque joueur (le couteau de base, pas le second couteau ni ceux "
                  "de l'inventaire) toutes ses unités de résistance.",
          .category = "Couteau : durabilité", .kind = Kind::Action,
          .run = [] { return RepairKnives(false); },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "combat.knife_repair_all", .name = "Réparer tous les couteaux",
          .help = "Rend toutes leurs unités à tous les couteaux des deux joueurs : principal et second couteau de l'étui, "
                  "couteaux de l'inventaire.",
          .category = "Couteau : durabilité", .kind = Kind::Action,
          .run = [] { return RepairKnives(true); },
          .available = [] { return game::IsSupported(); } });
    OnHud(Draw);
    // The story's sheaths aside in Mercenaries whatever the durability option (a second knife exists without it).
    OnFrame([] {
        LoadDurability();
        TrackMercenaries();
    });
    AddPanel("Combat", DrawCombatPanel,
             { "Combat", "Couteau", "Couteau : rapide", "Couteau : durabilité" }, true,
             true);
}
}  // namespace features
