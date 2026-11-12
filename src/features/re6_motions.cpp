// RE6's motions on RE5's player (user, 2026-10-23: RE6's sprints, then its dodges and moves). tools/import_re6_motions.py
// converts RE6 player motions (LMT v67, PlayerBase.arc) into an RE5 list (LMT v51) at DataDir\motions\re6_moves.lmt:
// same bone ids, the leg IK flags of RE5, RE6's layered legs + upper body merged (MODLOG session 16). In memory a list
// is the file with its offsets turned into addresses (beta_anims.cpp); the relocated list is never freed, and empty
// slots of player 1's action list (rMotionList, vtable RVA 0xCB6114, path at +4, data at +0x60, [P+0xA88 + 4 * bank];
// his damage list when the action list is full: FindList) point at its motions, so a motion id (bank << 8) | slot plays
// them. Patched again when a level loads a new list.
// Sprint: while "Sprint (tenir la course)" sprints, the run's layer 0 motion (slot 0x04 of its weapon list: 0x504...)
// becomes RE6's sprint (list slot 0: arms swinging, 1: two hands; by the held weapon or the user's choice); the run is
// asked again when the sprint starts or stops, so the motion changes at once.
// The same for RE4's (user, 2026-10-28: Leon's moves in the free melee and the dodges, to test them on Chris one by one):
// Leon's moves of RE4 (2005) by tools/re4_build_moves.py (re4_leon_moves.lmt), his knife parries and his knife grab
// moves of RE4 Remake by tools/re4r_to_lmt.py (re4r_leon_parry.lmt, re4r_leon_grab_moves.lmt); wesker_moves.cpp plays
// them as its borrowed moves (ForeignMotion).
#include "common/i18n.hpp"
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace features {
bool EnsureSetMotionHook();  // swaps.cpp
bool Player1Sprinting();     // movement.cpp
uint16_t PlayerHeldItem(uintptr_t player);  // knife.cpp
bool IsKnifeItem(uint16_t id);              // inventory.cpp

namespace {
constexpr uintptr_t kMotionListVtable = 0xCB6114;  // rMotionList (RVA)
constexpr uint32_t kEntry = 0xC0, kTrack = 0x20;

bool Within(size_t off, size_t len, size_t size) { return off <= size && len <= size - off; }

// Bank `b`'s motion list (v51): its data, and whether its path holds "action" (pl00action, pl00action_reunion in
// Mercenaries: read live, 2026-10-23).
bool BankList(uintptr_t p, int b, uintptr_t& data, bool& action) {
    uintptr_t object = 0, vtable = 0;
    char path[64]{};
    data = 0;
    if (!mem::Read(p + 0xA88 + 4 * b, object) || !object || !mem::Read(object, vtable) ||
        vtable != reinterpret_cast<uintptr_t>(mem::Rva(kMotionListVtable)) ||
        !mem::SafeRead(reinterpret_cast<void*>(object + 4), path, sizeof(path) - 1) || !mem::Read(object + 0x60, data) ||
        !data)
        return false;
    uint8_t header[8]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(data), header, sizeof(header)) || std::memcmp(header, "LMT\0", 4) != 0 ||
        header[4] != 51)
        return false;
    action = std::strstr(path, "action") != nullptr;
    return true;
}

// One list of DataDir\motions put into player 1's lists.
struct ForeignList {
    const char* file;  // in DataDir\motions
    const char* tool;  // what writes it (the error message)
    uint8_t* list = nullptr;  // relocated, never freed (a motion in progress may still use it)
    uint32_t count = 0;       // its motions (slots 0..count-1)
    uint32_t fileSize = 0;    // its size
    std::string loadError;
    // The player list our motions are in, and where.
    uintptr_t target = 0;  // its data
    int bank = -1;
    std::vector<uint8_t> slots;  // its slot of each of our motions
    DWORD lastScan = 0;
    uintptr_t noRoom = 0;  // the player whose lists had no room (logged once)

    bool Load() {
        if (list) return true;
        const std::string name(file);
        const std::wstring path = logx::DataDir() + L"\\motions\\" + std::wstring(name.begin(), name.end());
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            loadError = "fichier motions\\" + name + " absent : lancer " + tool;
            return false;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const size_t size = bytes.size();
        if (size < 8 || std::memcmp(bytes.data(), "LMT\0", 4) != 0 || bytes[4] != 51 || bytes[5] != 0) {
            loadError = name + " : pas une liste d'animations v51";
            return false;
        }
        const uint32_t n = bytes[6] | bytes[7] << 8;
        auto u32 = [&](size_t off) {
            uint32_t v = 0;
            std::memcpy(&v, bytes.data() + off, 4);
            return v;
        };
        if (!n || size < 8 + 4u * n) {
            loadError = name + " : liste vide";
            return false;
        }
        auto* data = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!data) {
            loadError = "mémoire insuffisante";
            return false;
        }
        std::memcpy(data, bytes.data(), size);
        const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data));
        auto fix = [&](size_t field, size_t len) {
            uint32_t off = 0;
            std::memcpy(&off, data + field, 4);
            if (!off) return true;
            if (!Within(off, len, size)) return false;
            const uint32_t address = base + off;
            std::memcpy(data + field, &address, 4);
            return true;
        };
        bool ok = true;
        for (uint32_t i = 0; i < n && ok; ++i) {
            const uint32_t entry = u32(8 + 4 * i);
            if (!entry || !Within(entry, kEntry, size)) {
                ok = false;
                break;
            }
            const uint32_t tracks = u32(entry), ntracks = u32(entry + 4);
            ok = fix(8 + 4 * i, kEntry) && fix(entry, static_cast<size_t>(ntracks) * kTrack) &&
                 fix(entry + 0x74, static_cast<size_t>(u32(entry + 0x70)) * 8) &&
                 fix(entry + 0xBC, static_cast<size_t>(u32(entry + 0xB8)) * 8);
            for (uint32_t t = 0; t < ntracks && ok; ++t) {
                const size_t track = tracks + static_cast<size_t>(t) * kTrack;
                ok = Within(track, kTrack, size) && fix(track + 0xC, u32(track + 8));
            }
        }
        if (!ok) {
            VirtualFree(data, 0, MEM_RELEASE);
            loadError = name + " : décalage hors du fichier";
            return false;
        }
        list = data;
        count = n;
        fileSize = static_cast<uint32_t>(size);
        LOG("foreign motions: %s loaded at %p (%u motions, %u bytes)", file, data, n, static_cast<unsigned>(size));
        return true;
    }

    uint32_t Entry(uint32_t i) const {
        uint32_t entry = 0;
        std::memcpy(&entry, list + 8 + 4 * i, 4);
        return entry;
    }

    bool OursIn(uintptr_t data) const {
        if (slots.size() != count) return false;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t entry = 0;
            if (!mem::Read(data + 8 + 4 * slots[i], entry) || entry != Entry(i)) return false;
        }
        return true;
    }

    // The slots of `data` for our motions: those already holding them (a list patched before: going back and forth
    // between two lists takes no more slots), the others its last empty ones (another list's motions are not empty).
    bool ChooseSlots(uintptr_t data, std::vector<uint8_t>& chosen) const {
        uint16_t n = 0;
        if (!mem::Read(data + 6, n) || !n || n > 0x100) return false;
        std::vector<uint32_t> entries(n);
        if (!mem::SafeRead(reinterpret_cast<void*>(data + 8), entries.data(), 4u * n)) return false;
        chosen.assign(count, 0);
        std::vector<bool> found(count, false);
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t s = 0; s < n && !found[i]; ++s)
                if (entries[s] == Entry(i)) {
                    chosen[i] = static_cast<uint8_t>(s);
                    found[i] = true;
                }
        int s = static_cast<int>(n) - 1;
        for (uint32_t i = 0; i < count; ++i) {
            if (found[i]) continue;
            while (s >= 0 && entries[s]) --s;
            if (s < 0) return false;
            chosen[i] = static_cast<uint8_t>(s--);
        }
        return true;
    }

    // The list our motions go in: player 1's action list, else - no room there: Barry's pl06action has 5 empty slots,
    // Rebecca's pl07action none, Josh's pl04action 21, Jill's pl02action 114 (read live, 2026-10-27: Barry ran RE5's run
    // sped up) - his damage list (bank 0), then his two weapon lists.
    bool Find(uintptr_t p, int& b, uintptr_t& data, std::vector<uint8_t>& chosen) const {
        uintptr_t lists[16] = {};
        int action = -1;
        for (int k = 0; k < 16; ++k) {
            bool isAction = false;
            if (BankList(p, k, lists[k], isAction) && isAction && action < 0) action = k;
        }
        for (const int k : { action, 0, 2, 3 }) {
            if (k < 0 || !lists[k]) continue;
            if (lists[k] == target && k == bank && OursIn(lists[k])) {
                chosen = slots;
            } else if (!ChooseSlots(lists[k], chosen)) {
                continue;
            }
            b = k;
            data = lists[k];
            return true;
        }
        return false;
    }

    // Our motions into `chosen` slots of the list.
    void Patch(uintptr_t data, int b, const std::vector<uint8_t>& chosen) {
        for (uint32_t i = 0; i < count; ++i) mem::Write<uint32_t>(data + 8 + 4 * chosen[i], Entry(i));
        slots = chosen;
        target = data;
        bank = b;
        LOG("foreign motions: %s, %u motions in player 1's %s list (bank %X, slots %X..%X)", file, count,
            b == 0 ? "damage" : "action", b, *std::min_element(chosen.begin(), chosen.end()),
            *std::max_element(chosen.begin(), chosen.end()));
    }

    void Unpatch() {
        if (!target) return;
        uint8_t header[4]{};
        if (mem::SafeRead(reinterpret_cast<void*>(target), header, sizeof(header)) &&
            std::memcmp(header, "LMT\0", 4) == 0 && OursIn(target))
            for (uint8_t s : slots) mem::Write<uint32_t>(target + 8 + 4 * s, 0);
        target = 0;
        bank = -1;
        slots.clear();
    }

    // Kept in player 1's lists (every 0.5 s, or now).
    void Tick(bool now) {
        if (!list) return;
        const DWORD tick = GetTickCount();
        if (!now && tick - lastScan < 500) return;
        lastScan = tick;
        const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
        int b = -1;
        uintptr_t data = 0;
        std::vector<uint8_t> chosen;
        if (!p) return;
        if (!Find(p, b, data, chosen)) {
            if (noRoom != p) LOG("foreign motions: %s, no room in player 1's lists", file);  // once per player
            noRoom = p;
            return;
        }
        if (data != target || b != bank || !OursIn(data)) Patch(data, b, chosen);
    }
};

ForeignList g_re6{ "re6_moves.lmt", "tools/import_re6_motions.py" };
// RE4's lists (wesker_moves.cpp's pseudo banks 0x20 + n - 1): kept in player 1's lists once one of their moves played.
ForeignList g_foreign[4] = { { "re4_leon_moves.lmt", "tools/re4_build_moves.py" },
                             { "re4r_leon_parry.lmt", "tools/re4r_to_lmt.py" },
                             { "re4r_leon_grab_moves.lmt", "tools/re4r_to_lmt.py" },
                               { "re6_evade.lmt", "tools/import_re6_motions.py" } };
std::atomic<bool> g_foreignWanted[4]{};
// RE4's synchronised pairs (user, 2026-10-31: HUNK's neck break and Leon's suplex played "in the void, no synchronised
// animation"): research\re4_anims\poc\re4_pairs_shared_origin.lmt (tools/re4_build_moves.py pairs) copied to
// DataDir\motions - even slots the Ganado's side for a Majini (0 suplexed, 2 neck broken, 4 / 6 front hold / kick-off,
// 8 front death, 10 / 12 back hold / throw), odd ones Leon's, both from the player's origin and yaw, started together.
// An enemy's side goes into an empty slot of the enemy's own lists (shared by every enemy of its kind; slots the game
// never plays; not its bank 6, lent to the player in its grabs), found there again by its address.
ForeignList g_pairs{ "re4_pairs_shared_origin.lmt", "tools/re4_build_moves.py pairs (research\\re4_anims\\poc)" };
struct EnemySlot {
    uintptr_t data;  // the enemy list's data
    uint16_t slot;
    uint32_t entry;  // our motion there
};
std::vector<EnemySlot> g_enemySlots;  // render thread: patched, emptied again at the unload

std::atomic<int> g_sprintChoice{ 0 };  // 0 = the game's run, 1 = by the weapon, 2 = arms swinging, 3 = two hands
// The sprint's motion refused because our motions were not in player 1's list (user, 2026-10-30: right after a free
// melee move - the neck break - the run started sprinting while the move's borrowed list had just been given back, our
// slots of that list played nothing, Chris stood frozen until the run key was let go): the run is asked again once
// they are back (Tick).
std::atomic<bool> g_sprintStale{ false };

// Our motions are in player 1's list as noted.
bool Re6InPlace(uintptr_t p) {
    uintptr_t data = 0;
    bool action = false;
    return g_re6.bank >= 0 && BankList(p, g_re6.bank, data, action) && data == g_re6.target && g_re6.OursIn(data);
}

// RE6's two-handed sprint for long guns (shotguns, rifles, machine guns, launchers, bow, flamethrower; item ids from
// item_names.inc); handguns, magnums, knives and grenades swing the arms.
bool TwoHanded(uint16_t item) {
    switch (item) {
    case 0x103: case 0x104: case 0x105: case 0x109: case 0x10C: case 0x10D: case 0x10F: case 0x113: case 0x115:
    case 0x116: case 0x117: case 0x119: case 0x11C: case 0x11D: case 0x120: case 0x121: case 0x125: case 0x126:
    case 0x127: case 0x135: case 0x139: case 0x13A: case 0x13B:
        return true;
    default:
        return false;
    }
}

void AskRun(uintptr_t p);  // below

void Tick() {
    if (g_sprintChoice.load(std::memory_order_relaxed)) {
        const bool stale = g_sprintStale.exchange(false);
        g_re6.Tick(stale);
        const uintptr_t p = stale && actors::InGame() ? actors::Player(0) : 0;
        uint32_t action = 0;
        if (p && Player1Sprinting() && mem::Read(p + 0x10E0, action) && action == 0x05 && Re6InPlace(p)) {
            LOG("RE6 sprint: our motions back in player 1's list, the run asked again");
            AskRun(p);
        }
    }
    for (int k = 0; k < 4; ++k)
        if (g_foreignWanted[k].load(std::memory_order_relaxed)) g_foreign[k].Tick(false);
}

void Unpatch() {
    g_re6.Unpatch();
    for (ForeignList& list : g_foreign) list.Unpatch();
    for (const EnemySlot& e : g_enemySlots) {  // still an LMT list holding our motion there (not a list freed with a level)
        uint8_t header[8]{};
        uint32_t entry = 0;
        if (mem::SafeRead(reinterpret_cast<void*>(e.data), header, sizeof(header)) && std::memcmp(header, "LMT\0", 4) == 0 &&
            mem::Read(e.data + 8 + 4u * e.slot, entry) && entry == e.entry)
            mem::Write<uint32_t>(e.data + 8 + 4u * e.slot, 0);
    }
    g_enemySlots.clear();
}

// The player's RequestAction (vtable +0xDC): the run asked again, its motion set again.
void AskRun(uintptr_t p) {
    using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
    uintptr_t vtable = 0, fn = 0;
    if (!mem::Read(p, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return;
    __try {
        reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(p), nullptr, 0x05, 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
}  // namespace

// swaps.cpp's SetMotion hook: player 1's run motion while sprinting becomes the chosen RE6 motion. The run is three
// layers set together (motions.log): 0 the run (slot 0x04), 1 and 2 its turning runs (slots 0x05 / 0x06, either way
// round), weighed by how fast Chris turns; all three become the sprint (user, 2026-10-24: turning with the keyboard,
// the mouse or the left stick blended RE5's run into the sprint; the right stick turns him without those weights).
// The run is RE6's straight sprint (the mean of its two leaning loops, list slots 0 / 1), the turning runs its leaning
// loops themselves (user, 2026-10-25: "use the leaning runs when I turn"; slots 2 / 3 arms swinging, 4 / 5 two hands,
// each leaning as RE5's slot 5 / 6: tools/import_re6_motions.py). An older file without them keeps the mean.
uint32_t Re6SprintRemap(uintptr_t unit, uint32_t layer, uint32_t motion) {
    const int choice = g_sprintChoice.load(std::memory_order_relaxed);
    const uint32_t slot = motion & 0xFF;
    if (!choice || layer > 2 || !g_re6.target || g_re6.bank < 0 || g_re6.slots.size() < 2 || slot < 0x04 || slot > 0x06 ||
        unit != actors::Player(0) || !Player1Sprinting())
        return motion;
    uint32_t action = 0;
    if (!mem::Read(unit + 0x10E0, action) || action != 0x05) return motion;
    if (!Re6InPlace(unit)) {  // the game's run meanwhile
        g_sprintStale = true;
        return motion;
    }
    const bool twoHands = choice == 1 ? TwoHanded(PlayerHeldItem(unit)) : choice == 3;
    const size_t ours = slot == 0x04 || g_re6.slots.size() < 6 ? (twoHands ? 1 : 0) : 2 + 2 * twoHands + (slot - 0x05);
    return (static_cast<uint32_t>(g_re6.bank) << 8) | g_re6.slots[ours];
}

// movement.cpp, the sprint starting or stopping during the run: the run's motion set again (ours or the game's).
void Re6SprintChanged(uintptr_t p) {
    if (g_sprintChoice.load(std::memory_order_relaxed) && g_re6.target && p) AskRun(p);
}

// wesker_moves.cpp: RE6's aiming poses on the ground, the long guns' or the hands' (the sprint's rule).
bool Re6WeaponTwoHanded(uintptr_t p) { return TwoHanded(PlayerHeldItem(p)); }
// The slot of re6_evade.lmt with the ground pose of what he holds when it is not a gun: 28 knife, 29 thrown (grenades, mine, eggs), 30 anything that is not a weapon.
int Re6HeldPoseSlot(uintptr_t p) {
    const uint16_t item = PlayerHeldItem(p);
    if (!item) return 0;
    if (IsKnifeItem(item)) return 28;
    switch (item) {
    case 0x0106: case 0x0107: case 0x0108: case 0x010A: case 0x0136: case 0x013C: case 0x013D: case 0x013E:
        return 29;
    default:
        return (item >> 8) != 1 ? 30 : 0;
    }
}

// wesker_moves.cpp's RE6 melee test bank (user, 2026-11-09: "the RE6 melee options: look for them in the game"):
// DataDir\motions\re6_melee.lmt (tools/import_re6_melee.py, RE6's AcA pairs), loaded whole for a motion bank of its own.
ForeignList g_re6Melee{ "re6_melee.lmt", "tools/import_re6_melee.py" };

// A bank takes a motion-list resource, not its data (uModel::SetMotionList; the first try gave it the data: the motion was asked and
// replaced at once, user: "Chris trembles and grunts, no move"). An rMotionList read live (2026-11-09): vtable RVA kMotionListVtable, its
// path +4 (64 bytes), references +0x44 (2 / 3 on Chris's lists), flags +0x48 0x92 / +0x4C 1 (loaded), the data's size +0x50, the LMT
// data +0x60 (offsets made addresses). Ours: a copy of player 1's action list resource, our path, size and data, its references
// never reaching 0 (the game would free it), never freed itself (a bank may still hold it after an unload).
uint8_t* g_re6MeleeResource = nullptr;

// wesker_moves.cpp's RE6 pairs: a motion entry of re6_melee.lmt (the enemy's side, put in its own lists; its root travel).
uint32_t Re6MeleeEntry(uint16_t index) { return g_re6Melee.Load() && index < g_re6Melee.count ? g_re6Melee.Entry(index) : 0; }

void* Re6MeleeList(uint32_t& count) {
    if (!g_re6Melee.Load()) {
        Toast(g_re6Melee.loadError, false);
        return nullptr;
    }
    count = g_re6Melee.count;
    if (g_re6MeleeResource) return g_re6MeleeResource;
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t live = 0, vtable = 0;
    if (!p || !mem::Read(p + 0xA88 + 4, live) || !live || !mem::Read(live, vtable) ||
        vtable != reinterpret_cast<uintptr_t>(mem::Rva(kMotionListVtable)))
        return nullptr;
    auto* res = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!res) return nullptr;
    if (!mem::SafeRead(reinterpret_cast<void*>(live), res, 0x68)) {
        VirtualFree(res, 0, MEM_RELEASE);
        return nullptr;
    }
    std::memset(res + 4, 0, 0x40);
    std::memcpy(res + 4, "trainer\\re6_melee", sizeof("trainer\\re6_melee"));
    const uint32_t refs = 0x100000, bytes = g_re6Melee.fileSize;
    const uint32_t data = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_re6Melee.list));
    std::memcpy(res + 0x44, &refs, 4);
    std::memcpy(res + 0x50, &bytes, 4);
    std::memcpy(res + 0x60, &data, 4);
    g_re6MeleeResource = res;
    LOG("foreign motions: re6_melee.lmt's resource made at %p (%u motions)", res, g_re6Melee.count);
    return res;
}

// wesker_moves.cpp: motion `index` of RE4 list `which` (0 Leon's RE4 moves, 1 his RE4 Remake knife parries, 2 his knife
// grab moves) on player 1, as (bank << 8) | slot; 0 when the file or the room is missing (a toast says why).
uint32_t ForeignMotion(int which, uint16_t index) {
    if (which < 0 || which >= 4) return 0;
    ForeignList& list = g_foreign[which];
    if (!list.Load()) {
        Toast(list.loadError, false);
        return 0;
    }
    if (index >= list.count) return 0;
    g_foreignWanted[which] = true;
    list.Tick(true);
    if (!list.target || list.bank < 0 || list.slots.size() != list.count) {
        Toast(std::string(list.file) + i18n::Tr(" : pas de place dans les listes d'animations de Chris"), false);
        return 0;
    }
    return (static_cast<uint32_t>(list.bank) << 8) | list.slots[index];
}

bool MotionRootAt(uint32_t motionEntry, float frame, float (&out)[3]);      // below
uint32_t EnemyMotionSlot(uintptr_t enemy, uint32_t entry, const char* what);  // below

// wesker_moves.cpp: the root travel of motion `index` of RE4 list `which` (0-2 ForeignMotion's, 3 the pairs) at `frame`,
// cm in the motion's own frame (x side, z forward): bone 255's position track (usage 4), type 9 (keys of a float3 and
// the frames to the next one, tools/lmt.py), linear between keys; false without one.
bool ForeignRootAt(int which, uint16_t index, float frame, float (&out)[3]) {
    ForeignList* const list = which == 3 ? &g_pairs : which == 4 ? &g_foreign[3] : which >= 0 && which < 3 ? &g_foreign[which] : nullptr;
    if (!list || !list->Load() || index >= list->count) return false;
    return MotionRootAt(list->Entry(index), frame, out);
}

// The same for any motion entry of an LMT in memory (the game's lists too: their offsets are addresses there).
bool MotionRootAt(uint32_t motionEntry, float frame, float (&out)[3]) {
    const uint8_t* entry = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(motionEntry));
    if (!entry) return false;
    uint32_t tracks = 0, count = 0;
    std::memcpy(&tracks, entry, 4);
    std::memcpy(&count, entry + 4, 4);
    for (uint32_t t = 0; t < count; ++t) {
        const uint8_t* rec = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(tracks)) + 0x20 * t;
        if (rec[3] != 255 || rec[1] != 4 || rec[0] != 9) continue;
        uint32_t size = 0, buffer = 0;
        std::memcpy(&size, rec + 8, 4);
        std::memcpy(&buffer, rec + 0xC, 4);
        const uint8_t* keys = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(buffer));
        const uint32_t n = size / 16;
        if (!n) return false;
        float start = 0.0f;
        for (uint32_t k = 0; k < n; ++k) {
            float v[3]{};
            uint32_t frames = 0;
            std::memcpy(v, keys + 16 * k, 12);
            std::memcpy(&frames, keys + 16 * k + 12, 4);
            if (k + 1 == n || frame < start + static_cast<float>(frames)) {
                float next[3]{};
                if (k + 1 < n && frames) {
                    std::memcpy(next, keys + 16 * (k + 1), 12);
                    const float s = std::clamp((frame - start) / static_cast<float>(frames), 0.0f, 1.0f);
                    for (int c = 0; c < 3; ++c) out[c] = v[c] + (next[c] - v[c]) * s;
                } else {
                    for (int c = 0; c < 3; ++c) out[c] = v[c];
                }
                return true;
            }
            start += static_cast<float>(frames);
        }
    }
    return false;
}

// wesker_moves.cpp's pair lock: the root's turn of a motion entry at `frame`, radians about the up axis as a unit's yaw (user, 2026-11-10:
// "at the end of Ada's Head Flash the enemy's place resets and it is turned the wrong way, head away from me"; research/re6_live/
// victim_hips.py: RE6's victim sides turn their root by up to 180 degrees - Leon's Head Flash -130, Chris's 180, Ada's -150 - and the
// lock held them at the pair's yaw, so the lying getup turned them round at once). Bone 255's rotation track (usage 3): type 6 keys
// (u64: x / y / z / w 14-bit signed / 4096 at bits 42 / 28 / 14 / 0, the frames to the next key in bits 56-63), between keys
// component-wise and normalized; type 4 one rotation (x, y, z floats). False without one.
bool MotionRootYawAt(uint32_t motionEntry, float frame, float& yaw) {
    const uint8_t* entry = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(motionEntry));
    if (!entry) return false;
    uint32_t tracks = 0, count = 0;
    std::memcpy(&tracks, entry, 4);
    std::memcpy(&count, entry + 4, 4);
    const auto decode = [](uint64_t v, float (&q)[4]) {
        for (int i = 0; i < 4; ++i) {
            int c = static_cast<int>((v >> (42 - 14 * i)) & 0x3FFF);
            if (c & 0x2000) c -= 0x4000;
            q[i] = static_cast<float>(c) / 4096.0f;
        }
    };
    for (uint32_t t = 0; t < count; ++t) {
        const uint8_t* rec = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(tracks)) + 0x20 * t;
        if (rec[3] != 255 || rec[1] != 3) continue;
        uint32_t size = 0, buffer = 0;
        std::memcpy(&size, rec + 8, 4);
        std::memcpy(&buffer, rec + 0xC, 4);
        const uint8_t* keys = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(buffer));
        float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        if (rec[0] == 4) {
            std::memcpy(q, keys, 12);
            q[3] = std::sqrt(std::max(0.0f, 1.0f - q[0] * q[0] - q[1] * q[1] - q[2] * q[2]));
        } else if (rec[0] == 6) {
            const uint32_t n = size / 8;
            if (!n) return false;
            float start = 0.0f;
            for (uint32_t k = 0; k < n; ++k) {
                uint64_t v = 0;
                std::memcpy(&v, keys + 8 * k, 8);
                const float frames = static_cast<float>(v >> 56);
                if (k + 1 == n || frame < start + frames) {
                    decode(v, q);
                    if (k + 1 < n && frames > 0.0f) {
                        uint64_t w = 0;
                        std::memcpy(&w, keys + 8 * (k + 1), 8);
                        float next[4];
                        decode(w, next);
                        const float dot = q[0] * next[0] + q[1] * next[1] + q[2] * next[2] + q[3] * next[3];
                        const float s = std::clamp((frame - start) / frames, 0.0f, 1.0f), sign = dot < 0.0f ? -1.0f : 1.0f;
                        for (int c = 0; c < 4; ++c) q[c] += (sign * next[c] - q[c]) * s;
                    }
                    break;
                }
                start += frames;
            }
        } else {
            continue;
        }
        yaw = std::atan2(2.0f * (q[0] * q[2] + q[3] * q[1]), 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
        return true;
    }
    return false;
}

bool ForeignRootYawAt(int which, uint16_t index, float frame, float& yaw) {
    ForeignList* const list = which == 3 ? &g_pairs : which == 4 ? &g_foreign[3] : which >= 0 && which < 3 ? &g_foreign[which] : nullptr;
    if (!list || !list->Load() || index >= list->count) return false;
    return MotionRootYawAt(list->Entry(index), frame, yaw);
}

// wesker_moves.cpp: the enemy's side `index` of an RE4 pair on `enemy`, as (bank << 8) | slot of its own lists; 0 when
// the file or the room is missing (a toast says why).
uint32_t PairVictimMotion(uintptr_t enemy, uint16_t index) {
    if (!g_pairs.Load()) {
        Toast(g_pairs.loadError, false);
        return 0;
    }
    if (!enemy || index >= g_pairs.count) return 0;
    return EnemyMotionSlot(enemy, g_pairs.Entry(index), "re4_pairs_shared_origin.lmt");
}

// wesker_moves.cpp: a motion entry (of an LMT in memory: ours, or a game list's) in an empty slot of `enemy`'s own lists
// (shared by every enemy of its kind; not its bank 6, lent to the player in its grabs), found there again by its address;
// (bank << 8) | slot, 0 without room (a toast names `what`).
uint32_t EnemyMotionSlot(uintptr_t enemy, uint32_t entry, const char* what) {
    if (!enemy || !entry) return 0;
    int best = -1, bestEmpty = 0;
    uintptr_t bestData = 0;
    for (int b = 0; b < 16; ++b) {
        uintptr_t data = 0;
        bool action = false;
        uint16_t n = 0;
        if (b == 6 || !BankList(enemy, b, data, action) || !mem::Read(data + 6, n) || !n || n > 0x400) continue;
        std::vector<uint32_t> entries(n);
        if (!mem::SafeRead(reinterpret_cast<void*>(data + 8), entries.data(), 4u * n)) continue;
        int empty = 0;
        for (uint16_t s = 0; s < n && s < 0x100; ++s) {
            if (entries[s] == entry) return (static_cast<uint32_t>(b) << 8) | s;
            if (!entries[s]) ++empty;
        }
        if (empty > bestEmpty) {
            bestEmpty = empty;
            best = b;
            bestData = data;
        }
    }
    uint16_t n = 0;
    if (best < 0 || !mem::Read(bestData + 6, n)) {
        Toast(std::string(what) + i18n::Tr(" : pas de place dans les listes d'animations de l'ennemi"), false);
        return 0;
    }
    for (int s = std::min<int>(n, 0x100) - 1; s >= 0; --s) {
        uint32_t v = 0;
        if (!mem::Read(bestData + 8 + 4u * s, v) || v) continue;
        mem::Write<uint32_t>(bestData + 8 + 4u * s, entry);
        if (g_enemySlots.size() > 64) g_enemySlots.erase(g_enemySlots.begin());
        g_enemySlots.push_back({ bestData, static_cast<uint16_t>(s), entry });
        LOG("foreign motions: %s motion %08X in %08X's bank %X slot %X", what, entry, static_cast<unsigned>(enemy), best, s);
        return (static_cast<uint32_t>(best) << 8) | static_cast<uint32_t>(s);
    }
    return 0;
}

void RegisterRe6Motions() {
    Add({ .id = "player.sprint_re6_motion", .name = "Animation du sprint (RE6)",
          .help = "L'animation que joue Chris quand il sprinte (« Sprint (tenir la course) ») : celle de la course du jeu, "
                  "ou le sprint de RE6 (bras qui balancent pour les pistolets, magnums, couteaux et grenades ; arme à "
                  "deux mains pour les fusils, fusils à pompe, mitraillettes et lance-grenades), choisi selon l'arme "
                  "tenue ou imposé. Le fichier vient de tools/import_re6_motions.py.",
          .category = "Joueur", .kind = Kind::Choice,
          .available = [] { return game::IsSupported(); },
          .choose = [](int index) {
              if (index && (!g_re6.Load() || !EnsureSetMotionHook())) {
                  if (!g_re6.loadError.empty()) Toast(g_re6.loadError, false);
                  return false;
              }
              g_sprintChoice = index;
              g_re6.lastScan = 0;
              return true;
          },
          .value = 0, .valueMin = 0, .valueMax = 3, .valueFormat = "%.0f",
          .choices = { "Course du jeu (accélérée)", "Sprint de RE6 selon l'arme", "Sprint de RE6, bras qui balancent",
                       "Sprint de RE6, arme à deux mains" } });
    OnFrame(Tick);
    OnRestore(Unpatch);
}
}  // namespace features
