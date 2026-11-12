// Beta back-grab throw (user, 2026-10-20): the Fluffy mod "RE5 BETA ANIMS RESTORED" (uEm10.arc) restores unused
// grapple breakers; only its back one is wanted (Chris throws the Majini instead of the elbow; its front one is the
// prototype head flash, worse than melee.grab_head_flash). The mod replaces one list, pawn\em\em10\motion\em10_g06
// (LMT v51, 175 slots): back escape = enemy slots 0x04 / 0x40 (motions 0x604 / 0x640, male / female player held) and
// player slots 0x68 / 0xA4 (the enemy's list is the player's bank 0xF during a grab: motion 0xF68).
// tools/extract_beta_grab.py writes those four motions to DataDir\motions\em10_g06_beta.lmt (packed v51 list). In
// memory a list is the file with its offsets turned into addresses (2665 dwords checked live): the table, motion +0x00
// (tracks), +0x74 / +0xBC (events, counts +0x70 / +0xB8, 8 bytes each), track +0x0C (buffer). The Majini's loaded
// list is rMotionList (vtable RVA 0xCB6114, path at +4, data at +0x60) at enemy +0xA88 + 4 * bank 6, shared by every
// uEm10. The option relocates the beta list the same way and points those four table slots at it (motions started
// afterwards play it); off restores them. The beta buffer is never freed (a motion in progress may still use it).
// The throw ends earlier for the Majini (141 frames, it leaves BackHoldDmg 0x9C for 0x6B ~1.95 s in) than for the player
// (158): the player's update in the back escape (VA 0xC27F62..) releases him normally (RequestAction(0, 1, 0, -1, 0),
// return RVA 0x827FF0) when his motion ends (+0x388 & 4), waits (return 1) while the Majini is still in 0x9C, and
// otherwise takes the "freed by someone else" release 0x83 (return RVA 0x828060, startle motion 0x063 - user: "un
// sursaut" at the end of the throw). The wait branch at VA 0xC28045 (74 77, je) is made unconditional (EB 77) while the
// option is on: the player finishes his throw and gets the normal release. With the game's elbow the player's motion
// always ends first, so it changes nothing there.
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace features {
bool StaminaAllows();  // stamina.cpp: the red arc not filling

bool EnsurePlayerContactHook();  // player.cpp: cObjAdj contacts
bool EnsureSetMotionHook();      // swaps.cpp: lying on its back after the throw (wesker_moves.cpp FaceUpTwin)

namespace {
constexpr uint8_t kSlots[] = { 0x04, 0x40, 0x68, 0xA4 };
constexpr uintptr_t kMotionListVtable = 0xCB6114;  // rMotionList (RVA)
constexpr char kListPath[] = "pawn\\em\\em10\\motion\\em10_g06";
constexpr int kBank = 6;
constexpr uint16_t kCount = 175;
constexpr uint32_t kEntry = 0xC0, kTrack = 0x20;

uint8_t* g_beta = nullptr;  // relocated beta list, never freed
uintptr_t g_list = 0;       // the game's list we patched (its data)
uint32_t g_original[std::size(kSlots)] = {};
DWORD g_lastScan = 0;
mem::Patch g_waitThrow("beta back throw: player waits for his motion", 0x828045, { 0x74, 0x77 }, { 0xEB, 0x77 });
std::atomic<bool> g_betaOn{ false };

bool Within(size_t off, size_t len, size_t size) { return off <= size && len <= size - off; }

// Loads and relocates the packed beta list; false (with the reason) if the file is missing or not as expected.
bool LoadBeta(std::string& error) {
    if (g_beta) return true;
    const std::wstring path = logx::DataDir() + L"\\motions\\em10_g06_beta.lmt";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "fichier motions\\em10_g06_beta.lmt absent : lancer tools/extract_beta_grab.py sur le uEm10.arc du mod";
        return false;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t size = file.size();
    auto u32 = [&](size_t off) {
        uint32_t v = 0;
        std::memcpy(&v, file.data() + off, 4);
        return v;
    };
    if (size < 8 + 4u * kCount || std::memcmp(file.data(), "LMT\0", 4) != 0 || file[4] != 51 || file[5] != 0 ||
        (file[6] | file[7] << 8) != kCount) {
        error = "em10_g06_beta.lmt : pas une liste d'animations v51 de 175 entrées";
        return false;
    }
    for (uint8_t slot : kSlots) {
        const uint32_t entry = u32(8 + 4 * slot);
        if (!entry || !Within(entry, kEntry, size)) {
            error = "em10_g06_beta.lmt : animation manquante";
            return false;
        }
    }
    auto* data = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!data) {
        error = "mémoire insuffisante";
        return false;
    }
    std::memcpy(data, file.data(), size);
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
    for (uint32_t i = 0; i < kCount && ok; ++i) {
        const uint32_t entry = u32(8 + 4 * i);
        if (!entry) continue;
        if (!Within(entry, kEntry, size)) {
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
        error = "em10_g06_beta.lmt : décalage hors du fichier";
        return false;
    }
    g_beta = data;
    LOG("beta back throw: %s loaded at %p (%u bytes)", "em10_g06_beta.lmt", data, static_cast<unsigned>(size));
    return true;
}

uint32_t BetaEntry(uint8_t slot) {
    uint32_t entry = 0;
    std::memcpy(&entry, g_beta + 8 + 4 * slot, 4);
    return entry;
}

// The game's em10_g06 list data, from any Majini holding it as bank 6.
uintptr_t FindList() {
    for (uintptr_t enemy : actors::Enemies()) {
        uintptr_t object = 0, vtable = 0, data = 0;
        char path[sizeof(kListPath)]{};
        if (!mem::Read(enemy + 0xA88 + 4 * kBank, object) || !object || !mem::Read(object, vtable) ||
            vtable != reinterpret_cast<uintptr_t>(mem::Rva(kMotionListVtable)) ||
            !mem::SafeRead(reinterpret_cast<void*>(object + 4), path, sizeof(path) - 1) ||
            _stricmp(path, kListPath) != 0 || !mem::Read(object + 0x60, data) || !data)
            continue;
        uint8_t header[8]{};
        if (!mem::SafeRead(reinterpret_cast<void*>(data), header, sizeof(header)) || std::memcmp(header, "LMT\0", 4) != 0 ||
            header[4] != 51 || (header[6] | header[7] << 8) != kCount)
            continue;
        return data;
    }
    return 0;
}

bool OursIn(uintptr_t list) {
    for (uint8_t slot : kSlots) {
        uint32_t entry = 0;
        if (!mem::Read(list + 8 + 4 * slot, entry) || entry != BetaEntry(slot)) return false;
    }
    return true;
}

void Patch(uintptr_t list) {
    for (size_t i = 0; i < std::size(kSlots); ++i) {
        mem::Read(list + 8 + 4 * kSlots[i], g_original[i]);
        mem::Write<uint32_t>(list + 8 + 4 * kSlots[i], BetaEntry(kSlots[i]));
    }
    g_list = list;
    LOG("beta back throw: em10_g06 at %08X patched", static_cast<unsigned>(list));
}

// Only into the list we patched, still loaded (a Majini holds it) or still holding our four pointers.
void Unpatch() {
    if (!g_list) return;
    const uintptr_t list = g_list;
    g_list = 0;
    uint8_t header[4]{};
    if (FindList() != list &&
        (!mem::SafeRead(reinterpret_cast<void*>(list), header, sizeof(header)) || std::memcmp(header, "LMT\0", 4) != 0 ||
         !OursIn(list)))
        return;  // unloaded with its level
    for (size_t i = 0; i < std::size(kSlots); ++i) {
        uint32_t entry = 0;
        if (mem::Read(list + 8 + 4 * kSlots[i], entry) && entry == BetaEntry(kSlots[i]))
            mem::Write<uint32_t>(list + 8 + 4 * kSlots[i], g_original[i]);
    }
    LOG("beta back throw: em10_g06 at %08X restored", static_cast<unsigned>(list));
}

// Each level loads its own copy: patched again when the Majini's list changes (checked twice a second). With the stamina
// empty (user, 2026-10-26: "only the plain escapes") the list and the code are the game's until it refills.
void Tick() {
    if (!StaminaAllows()) {
        if (g_list) Unpatch();
        if (g_waitThrow.IsApplied()) g_waitThrow.Restore();
        return;
    }
    if (!g_waitThrow.IsApplied()) g_waitThrow.Apply();
    const DWORD now = GetTickCount();
    if (now - g_lastScan < 500) return;
    g_lastScan = now;
    const uintptr_t list = FindList();
    if (!list) return;
    if (list != g_list) {
        g_list = 0;  // the old one went with its level
        Patch(list);
    } else if (!OursIn(list)) {
        Patch(list);
    }
}
}  // namespace

// player.cpp's cObjAdj contact hook: while the option is on, a Majini in the back escape (BackHoldDmg 0x9C) does not
// push player 1 (user 2026-10-22: the throw "jams" like Jill's throw did before its contacts were dropped).
bool BetaBackThrowOn() { return g_betaOn.load(std::memory_order_relaxed); }

void RegisterBetaAnims() {
    Feature& f = Add({ .id = "melee.beta_back_throw", .name = "Projection de dos (animation beta)",
                       .help = "Quand tu te libères seul d'un Majini qui t'a saisi de dos, Chris le projette au lieu du "
                               "coup de coude : animation de la version beta (mod « Beta Animations Restored »), pour "
                               "Chris comme pour Sheva. Il faut d'abord extraire ces animations du mod avec "
                               "tools/extract_beta_grab.py.",
                       .category = "Mêlée", .kind = Kind::Toggle,
                       .available = [] { return game::IsSupported(); } });
    Feature* const fp = &f;
    f.set = [fp](bool on) {
        if (!on) {
            g_betaOn = false;
            Unpatch();
            g_waitThrow.Restore();
            return true;
        }
        std::string error;
        if (!LoadBeta(error)) {
            fp->lastError = error;
            LOG("beta back throw: %s", error.c_str());
            return false;
        }
        if (!g_waitThrow.Apply()) {
            fp->lastError = "code de la désaisie de dos inattendu";
            return false;
        }
        if (!EnsurePlayerContactHook()) LOG("beta back throw: no contact hook, the Majini will push player 1");
        if (!EnsureSetMotionHook()) LOG("beta back throw: no SetMotion hook, the Majini will spin when it gets up");
        g_betaOn = true;
        g_lastScan = 0;
        if (actors::InGame()) Tick();
        return true;
    };
    f.tick = [] {
        if (actors::InGame()) Tick();
    };
}
}  // namespace features
