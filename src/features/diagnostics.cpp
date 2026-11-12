// Diagnostic tools (research). Writes reports to %LOCALAPPDATA%\RE5Trainer.
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace features {
namespace {

// Copy of the mapped image (reading it once avoids faulting on guard/no-access pages).
bool Snapshot(std::vector<uint8_t>& img, std::vector<bool>& execPage) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mem::Base());
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(mem::Base() + dos->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage;
    img.assign(size, 0);
    execPage.assign(size / 0x1000 + 1, false);
    for (size_t off = 0; off < size; off += 0x1000) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(mem::Base() + off, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            continue;
        mem::SafeRead(mem::Base() + off, img.data() + off, std::min<size_t>(0x1000, size - off));
        execPage[off / 0x1000] = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    }
    return true;
}

// Same algorithm as tools/mt_dti.py, on the live image.
bool ExportDti() {
    std::vector<uint8_t> img;
    std::vector<bool> execPage;
    Snapshot(img, execPage);
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    const size_t size = img.size();
    auto u32 = [&](size_t off) { uint32_t v; memcpy(&v, img.data() + off, 4); return v; };
    auto isCode = [&](uint32_t va) { size_t off = va - base; return va >= base && off < size && execPage[off / 0x1000]; };
    auto cstr = [&](uint32_t va, std::string& out) {
        size_t off = va - base;
        if (va < base || off >= size) return false;
        size_t end = off;
        while (end < size && end - off < 200 && img[end]) ++end;
        if (end == off || end >= size || img[end]) return false;
        for (size_t i = off; i < end; ++i) {
            char c = static_cast<char>(img[i]);
            if (!(isalnum(static_cast<unsigned char>(c)) || strchr("_:<>[] ,.@", c))) return false;
        }
        if (isdigit(img[off])) return false;
        out.assign(reinterpret_cast<const char*>(img.data() + off), end - off);
        return true;
    };

    struct Cand { size_t start; uint32_t parent, name, dti, call; std::string n; };
    std::vector<Cand> cands;
    std::map<uint32_t, int> targets;
    for (size_t i = 0; i + 20 <= size; ++i) {
        if (img[i] != 0x68 || img[i + 5] != 0x68 || img[i + 10] != 0xB9 || img[i + 15] != 0xE8) continue;
        if (!execPage[i / 0x1000]) continue;
        Cand c{ i, u32(i + 1), u32(i + 6), u32(i + 11), 0, {} };
        int32_t rel;
        memcpy(&rel, img.data() + i + 16, 4);
        c.call = static_cast<uint32_t>(base + i + 20 + rel);
        if (!cstr(c.name, c.n)) continue;
        targets[c.call]++;
        cands.push_back(std::move(c));
    }
    if (targets.empty()) {
        LOG("dti: no registrations found");
        return false;
    }
    uint32_t ctor = std::max_element(targets.begin(), targets.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;

    struct Cls { std::string name; uint32_t parent; uint32_t size; uint32_t init; };
    std::unordered_map<uint32_t, Cls> classes;
    for (auto& c : cands) {
        if (c.call != ctor) continue;
        uint32_t csize = 0xFFFFFFFF;
        if (c.start >= 2 && img[c.start - 2] == 0x6A) csize = img[c.start - 1];
        else if (c.start >= 5 && img[c.start - 5] == 0x68) csize = u32(c.start - 4);
        classes[c.dti] = { c.n, c.parent, csize, static_cast<uint32_t>(base + c.start) };
    }

    std::unordered_map<uint32_t, uint32_t> getdti;  // function -> dti
    for (size_t i = 0; i + 6 <= size; ++i)
        if (img[i] == 0xB8 && img[i + 5] == 0xC3 && execPage[i / 0x1000] && classes.count(u32(i + 1)))
            getdti[static_cast<uint32_t>(base + i)] = u32(i + 1);

    std::set<size_t> vtStarts;
    for (size_t i = 0; i + 6 <= size; ++i) {
        if (img[i] != 0xC7 || !execPage[i / 0x1000]) continue;
        uint8_t modrm = img[i + 1];
        if (modrm > 0x07 || modrm == 0x04 || modrm == 0x05) continue;
        uint32_t v = u32(i + 2);
        size_t off = v - base;
        if (v < base || off + 8 >= size || off % 4) continue;
        if (isCode(u32(off)) && isCode(u32(off + 4))) vtStarts.insert(off);
    }

    const std::wstring dir = logx::DataDir();
    FILE* fc = nullptr;
    FILE* fv = nullptr;
    FILE* ff = nullptr;
    _wfopen_s(&fc, (dir + L"\\re5_dti_classes.tsv").c_str(), L"w");
    _wfopen_s(&fv, (dir + L"\\re5_vtables.tsv").c_str(), L"w");
    _wfopen_s(&ff, (dir + L"\\re5_vfuncs.tsv").c_str(), L"w");
    if (!fc || !fv || !ff) return false;
    fprintf(fc, "dti_rva\tname\tparent\tsize\tinit_rva\n");
    for (auto& [dti, c] : classes) {
        auto p = classes.find(c.parent);
        fprintf(fc, "%08X\t%s\t%s\t0x%X\t%08X\n", static_cast<unsigned>(dti - base), c.name.c_str(),
                p != classes.end() ? p->second.name.c_str() : "?", c.size, static_cast<unsigned>(c.init - base));
    }
    std::map<uint32_t, std::vector<std::string>> funcs;
    int vtCount = 0;
    fprintf(fv, "vtable_rva\tclass\tslots\tgetdti_slot\n");
    for (size_t start : vtStarts) {
        std::vector<uint32_t> slots;
        for (size_t p = start; p + 4 < size && isCode(u32(p)) && (p == start || !vtStarts.count(p)) && slots.size() < 1000; p += 4)
            slots.push_back(u32(p));
        int dtiSlot = -1;
        for (size_t k = 0; k < slots.size() && dtiSlot < 0; ++k)
            if (getdti.count(slots[k])) dtiSlot = static_cast<int>(k);
        if (dtiSlot < 0) continue;
        const std::string& cls = classes[getdti[slots[dtiSlot]]].name;
        fprintf(fv, "%08X\t%s\t%zu\t%d\n", static_cast<unsigned>(start), cls.c_str(), slots.size(), dtiSlot);
        for (size_t k = 0; k < slots.size(); ++k)
            if (funcs[slots[k]].size() < 6) funcs[slots[k]].push_back(cls + "::vf" + std::to_string(k));
        ++vtCount;
    }
    fprintf(ff, "function_rva\tnames\n");
    for (auto& [f, names] : funcs) {
        fprintf(ff, "%08X\t", static_cast<unsigned>(f - base));
        for (size_t k = 0; k < names.size(); ++k) fprintf(ff, "%s%s", k ? ", " : "", names[k].c_str());
        fputc('\n', ff);
    }
    fclose(fc);
    fclose(fv);
    fclose(ff);
    LOG("dti export: ctor RVA %08X, %zu classes, %zu GetDTI, %d vtables, %zu functions", static_cast<unsigned>(ctor - base),
        classes.size(), getdti.size(), vtCount, funcs.size());
    Toast("Classes exportées : " + std::to_string(classes.size()));
    return true;
}
}

namespace {
// Effect log (research, 2026-10-21: which effect is the heal's green wave): the resource getter VA 0x448B70
// (thiscall(sResource [RVA 0xE34580]; class, path, flags), ret 0xC) is how every effect list (class VA 0x124ABE0,
// "effect\efl\...") is fetched when an effect is spawned (VA 0x7723E0). Each new (path, caller) pair is logged.
using GetResourceFn = void*(__fastcall*)(void* self, void* edx, void* cls, const char* path, int flags);
GetResourceFn g_getResource = nullptr;
std::atomic<bool> g_effectLog{ false };
std::mutex g_effectLock;
std::map<std::string, DWORD> g_effectSeen;  // path -> last logged (once in 2 s)
std::string StackCallers(const uint32_t* stack, int wanted, uint32_t* first = nullptr);

void* __fastcall HookGetResource(void* self, void* edx, void* cls, const char* path, int flags) {
    if (g_effectLog.load(std::memory_order_relaxed) && cls == mem::Rva(0xE4ABE0) && path) {
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(mem::Base());
        char copy[128]{};
        if (mem::SafeRead(path, copy, sizeof(copy) - 1)) {
            const DWORD now = GetTickCount();
            bool log = false;
            {
                std::lock_guard lock(g_effectLock);
                DWORD& last = g_effectSeen[copy];
                if (g_effectSeen.size() < 4000 && (!last || now - last >= 2000)) {
                    last = now ? now : 1;
                    log = true;
                }
            }
            if (log)
                LOG("effect: %s (caller RVA %06X | callers%s)", copy, static_cast<unsigned>(caller),
                    StackCallers(static_cast<const uint32_t*>(_AddressOfReturnAddress()), 8).c_str());
        }
    }
    return g_getResource(self, edx, cls, path, flags);
}

bool EnsureEffectLogHook() {
    static bool hooked = false;
    if (hooked) return true;
    uint8_t* target = mem::Rva(0x48B70);
    const uint8_t expected[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x0C, 0x8B, 0x45, 0x08 };
    if (!mem::IsGameCode(target, sizeof(expected)) || std::memcmp(target, expected, sizeof(expected)) != 0) {
        LOG("effect log: unexpected bytes %s", mem::Hex(target, sizeof(expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(expected));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookGetResource), reinterpret_cast<void**>(&g_getResource));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    hooked = true;
    LOG("effect log hook installed at %p", target);
    return true;
}

// Call logs (research, 2026-10-21: the flesh impact sound and the blood of a bullet hit, to play them on a missed quick
// shot): the sound requests all go through VA 0x497780 (the inner call of ~20 wrappers VA 0x495050.., e.g. the shared
// sBioSound bank [RVA 0xDB1D0C]+0x608 by VA 0x79F790(id, unit)), the effects through the effect manager's spawn VA
// 0x7723E0 ([RVA 0xE49C5C]). A naked stub logs the raw stack words (decoded: image RVA, object with its vtable and a
// resource path at +4, or a number) and the first return addresses found above them, then jumps to the game.
constexpr int kCallLogs = 8;
struct CallLogSite {
    const char* name;
    uintptr_t rva;
    uint8_t expected[6];
};
constexpr CallLogSite kCallLogSites[kCallLogs] = {
    { "sound", 0x97780, { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55 } },
    { "effect", 0x3723E0, { 0x51, 0x53, 0x8B, 0x5C, 0x24, 0x10 } },
    // VA 0x772200(&id, &position, &direction, 4, ..): the effects spawned at a point (the Majini hit feedback VA
    // 0x861160 calls it with the hit's direction).
    { "effect at", 0x372200, { 0x51, 0x53, 0x55, 0x8B, 0x6C, 0x24 } },
    // uBioEffect's DTI NewInstance (DTI RVA 0xDE9D70, vtable slot 1: alloc 0x200, ctor VA 0x828AF0): every effect unit
    // made, also from effect lists already loaded (no fetch by path: core\cr0000 seen near bullet hits).
    { "bioeffect new", 0x429ED0, { 0x8B, 0x0D, 0x94, 0xF2, 0x1A, 0x01 } },
    // CommonEffectParam::GetList(index) VA 0x802880 (thiscall, ret 4: [this + 4 + 4 index], 8 lists): read twice per
    // pistol hit on the em1000 slot of a Majini's common effects (hardware breakpoint, 2026-10-21).
    { "common effect list", 0x402880, { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xF8 } },
    // The subtitles of the partner's lines (user, 2026-10-26: "hide what she says too", her voice muted with "Joueur 2
    // invisible"): the constructors of uCaption (vtable RVA 0xBBC0C4, RVA 0x680200: +0x18 = -1, +0x20 = 600) and of
    // uMessage (vtable RVA 0xBBC090, RVA 0x680640), with who makes them.
    { "caption new", 0x680200, { 0x56, 0x8B, 0xF1, 0xE8, 0xE8, 0x3A } },
    { "message new", 0x680640, { 0x56, 0x8B, 0xF1, 0xC7, 0x06, 0x90 } },
    // The items made (user, 2026-10-27: knives dropped by the enemies): uItem's constructor VA 0xA79060, called by every
    // uItXXXX NewInstance (uIt0201 VA 0xA7A720: alloc 0x1BD0) and uWeapon's - who makes the enemies' drops.
    { "item new", 0x679060, { 0x53, 0x56, 0x57, 0x8B, 0xF1, 0xE8 } },
};
void* g_callLogOriginal[kCallLogs] = {};  // the trampolines the stubs jump to
std::atomic<bool> g_callLogOn[kCallLogs]{};
std::atomic<int> g_callLogLines[kCallLogs]{};
// The same (sound bank and id, or effect id; first caller) is logged once in 2 s: a stage start asked ~4000 sounds in 2 s, ambience
// loops repeat.
std::mutex g_callLogLock;
std::map<std::tuple<int, uint32_t, uint32_t, uint32_t>, DWORD> g_callLogLast;

bool InImageCode(uint32_t v) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    return v >= base + 0x1000 && v < base + 0xB00000;
}

std::string Describe(uint32_t v) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    char out[96];
    if (v >= base && v < base + 0x1000000) {
        std::snprintf(out, sizeof(out), "RVA %06X", static_cast<unsigned>(v - base));
        return out;
    }
    uint32_t vtable = 0;
    if (v > 0x10000 && mem::Read(v, vtable) && vtable >= base && vtable < base + 0x1000000) {
        char path[40]{};
        mem::SafeRead(reinterpret_cast<void*>(v + 4), path, sizeof(path) - 1);
        bool text = path[0] != 0;
        for (const char* c = path; *c && text; ++c) text = *c >= 0x20 && *c < 0x7F;
        std::snprintf(out, sizeof(out), "%08X(vt %06X%s%s)", v, static_cast<unsigned>(vtable - base), text ? " " : "",
                      text ? path : "");
        return out;
    }
    std::snprintf(out, sizeof(out), "%X", v);
    return out;
}

// Return addresses up a stack: words in the image's code just after a call (E8).
std::string StackCallers(const uint32_t* stack, int wanted, uint32_t* first) {
    std::string callers;
    int found = 0;
    for (int i = 0; i < 96 && found < wanted; ++i) {
        uint32_t v = 0;
        if (!mem::Read(reinterpret_cast<uintptr_t>(stack + i), v) || !InImageCode(v)) continue;
        uint8_t call = 0;
        if (!mem::Read(v - 5, call) || call != 0xE8) continue;
        char text[16];
        std::snprintf(text, sizeof(text), " %06X", static_cast<unsigned>(v - reinterpret_cast<uintptr_t>(mem::Base())));
        callers += text;
        if (!found && first) *first = v;
        ++found;
    }
    return callers;
}

extern "C" void __cdecl CallLogged(int which, const uint32_t* stack, uint32_t self) {
    const CallLogSite& site = kCallLogSites[which];
    if (!g_callLogOn[which].load(std::memory_order_relaxed) || g_callLogLines[which].load() >= 20000) return;
    std::string args;
    for (int i = 1; i <= 8; ++i) {
        uint32_t v = 0;
        if (!mem::Read(reinterpret_cast<uintptr_t>(stack + i), v)) break;
        args += " " + Describe(v);
    }
    uint32_t firstCaller = 0;  // return addresses up the stack (the wrapper's callers for the sounds)
    const std::string callers = StackCallers(stack, 6, &firstCaller);
    uint32_t a1 = 0, a2 = 0, a3 = 0;  // sounds: manager, bank, id
    mem::Read(reinterpret_cast<uintptr_t>(stack + 1), a1);
    mem::Read(reinterpret_cast<uintptr_t>(stack + 2), a2);
    mem::Read(reinterpret_cast<uintptr_t>(stack + 3), a3);
    if (which == 1 || which == 2) {  // the effect id is a struct on the caller's stack: its words
        uint32_t id[4]{};
        if (mem::SafeRead(reinterpret_cast<void*>(static_cast<uintptr_t>(a1)), id, sizeof(id))) {
            char text[64];
            std::snprintf(text, sizeof(text), " id {%X %X %X %X}", id[0], id[1], id[2], id[3]);
            args += text;
            a1 = id[0] ^ (id[2] << 8) ^ (id[3] << 16);
        }
    }
    if (which == 2) {  // effect at: its position, direction and parameters (user, 2026-10-28: the sparks for the parry)
        uint32_t a4 = 0, a5 = 0;
        mem::Read(reinterpret_cast<uintptr_t>(stack + 2), a2);
        mem::Read(reinterpret_cast<uintptr_t>(stack + 3), a3);
        mem::Read(reinterpret_cast<uintptr_t>(stack + 4), a4);
        mem::Read(reinterpret_cast<uintptr_t>(stack + 5), a5);
        float pos[3]{}, dir[3]{};
        uint32_t params[9]{};
        mem::SafeRead(reinterpret_cast<void*>(static_cast<uintptr_t>(a2)), pos, sizeof(pos));
        mem::SafeRead(reinterpret_cast<void*>(static_cast<uintptr_t>(a3)), dir, sizeof(dir));
        mem::SafeRead(reinterpret_cast<void*>(static_cast<uintptr_t>(a5)), params, sizeof(params));
        char text[256];
        std::snprintf(text, sizeof(text), " | at (%.1f %.1f %.1f) dir (%.2f %.2f %.2f) %u params %X %s %X %X %g %g %g %X %g",
                      pos[0], pos[1], pos[2], dir[0], dir[1], dir[2], a4, params[0], Describe(params[1]).c_str(), params[2],
                      params[3], *reinterpret_cast<float*>(&params[4]), *reinterpret_cast<float*>(&params[5]),
                      *reinterpret_cast<float*>(&params[6]), params[7], *reinterpret_cast<float*>(&params[8]));
        args += text;
        mem::Read(reinterpret_cast<uintptr_t>(stack + 1), a1);  // the dedup key below: the id again
        uint32_t id[4]{};
        if (mem::SafeRead(reinterpret_cast<void*>(static_cast<uintptr_t>(a1)), id, sizeof(id)))
            a1 = id[0] ^ (id[2] << 8) ^ (id[3] << 16);
    }
    {
        const DWORD now = GetTickCount();
        std::lock_guard lock(g_callLogLock);
        const uint32_t chain = which >= 3 ? static_cast<uint32_t>(std::hash<std::string>{}(callers)) : 0;
        DWORD& last = g_callLogLast[{ which, which == 0 ? a2 : which >= 3 ? chain : a1, which == 0 ? a3 : 0, firstCaller }];
        if (last && now - last < 2000) return;
        last = now ? now : 1;
    }
    g_callLogLines[which].fetch_add(1);
    LOG("%s: this %s args%s | callers%s", site.name, Describe(self).c_str(), args.c_str(), callers.c_str());
}

#define CALL_LOG_STUB(N)                                   \
    __declspec(naked) void CallLogStub##N() {              \
        __asm pushad                                       \
        __asm mov eax, esp                                 \
        __asm add eax, 32                                  \
        __asm push ecx                                     \
        __asm push eax                                     \
        __asm push N                                       \
        __asm call CallLogged                              \
        __asm add esp, 12                                  \
        __asm popad                                        \
        __asm jmp dword ptr [g_callLogOriginal + N * 4]   \
    }
CALL_LOG_STUB(0)
CALL_LOG_STUB(1)
CALL_LOG_STUB(2)
CALL_LOG_STUB(3)
CALL_LOG_STUB(4)
CALL_LOG_STUB(5)
CALL_LOG_STUB(6)
CALL_LOG_STUB(7)
void* const kCallLogStubs[kCallLogs] = { reinterpret_cast<void*>(&CallLogStub0), reinterpret_cast<void*>(&CallLogStub1),
                                         reinterpret_cast<void*>(&CallLogStub2), reinterpret_cast<void*>(&CallLogStub3),
                                         reinterpret_cast<void*>(&CallLogStub4), reinterpret_cast<void*>(&CallLogStub5),
                                         reinterpret_cast<void*>(&CallLogStub6), reinterpret_cast<void*>(&CallLogStub7) };

bool EnsureCallLog(int which) {
    const CallLogSite& site = kCallLogSites[which];
    if (g_callLogOriginal[which]) return true;
    uint8_t* target = mem::Rva(site.rva);
    if (!mem::IsGameCode(target, sizeof(site.expected)) || std::memcmp(target, site.expected, sizeof(site.expected)) != 0) {
        LOG("%s log: unexpected bytes %s", site.name, mem::Hex(target, sizeof(site.expected)).c_str());
        return false;
    }
    mem::EnsureExecutable(target, sizeof(site.expected));
    void* original = nullptr;
    MH_STATUS st = MH_CreateHook(target, kCallLogStubs[which], &original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    if (original) g_callLogOriginal[which] = original;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    LOG("%s log hook installed at %p", site.name, target);
    return true;
}
}  // namespace

namespace {
// "Surveillance des armes rangées" (user, 2026-10-26: "after some actions Chris has the rifle on his back and the knife
// sheath lower and further forward, inside his body; watch for it"). A holstered weapon keeps the offsets copied when
// it was attached: the equip pair chosen by the weapon-anim flag P+0x1AF8 (female) and the handedness P+0x1AF9 (left)
// at that moment (ITEM_EQUIP_OFS_INFO_STRUCT RVA 0xDB28B0), the back position by +0x1AF8 too (male joint 0x75 /
// female 2; MODLOG). Every frame: player 1's two flags, and each inventory unit (records P+0x21A4 + 0x30 k: id +4,
// unit +0x2C; unit +0x18 parent, +0x1C joint, +0x30 local position); every change is logged with his action and
// motion and the trainer options that write those flags, to find what moves them.
struct WatchedUnit {
    uintptr_t unit = 0;
    uint32_t joint = 0;
    float position[3] = {};
};
uintptr_t g_watchPlayer = 0;
uint8_t g_watchFemale = 0xFF, g_watchLeft = 0xFF;
WatchedUnit g_watchUnits[24];

const char* OnOff(const char* id) {
    const Feature* f = Find(id);
    return f && f->enabled ? "on" : "off";
}

void TickHolsterWatch() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    if (!p) {
        g_watchPlayer = 0;
        return;
    }
    uint32_t action = 0, motion = 0;
    uint8_t female = 0, left = 0;
    uintptr_t held = 0;
    mem::Read(p + 0x10E0, action);
    mem::Read(p + 0x384, motion);
    mem::Read(p + 0x2634, held);
    if (!mem::Read(p + 0x1AF8, female) || !mem::Read(p + 0x1AF9, left)) return;
    const bool fresh = p != g_watchPlayer;
    if (fresh) {
        g_watchPlayer = p;
        for (WatchedUnit& w : g_watchUnits) w = {};
        LOG("holster watch: player 1 %08X, weapon anims %s, %s-handed", static_cast<unsigned>(p), female ? "female" : "male",
            left ? "left" : "right");
    } else if (female != g_watchFemale || left != g_watchLeft) {
        LOG("holster watch: player 1's weapon anims %s -> %s, hand %s -> %s (action %X, motion %X; options: inverted "
            "anims %s, lefty %s, bow / Gatling hand %s)",
            g_watchFemale ? "female" : "male", female ? "female" : "male", g_watchLeft ? "left" : "right",
            left ? "left" : "right", action, motion & 0xFFFF, OnOff("player.female_anim_p1"), OnOff("player.lefty_p1"),
            OnOff("char.weapon_hand"));
    }
    g_watchFemale = female;
    g_watchLeft = left;
    for (int k = 0; k < 24; ++k) {
        const uintptr_t record = p + 0x21A4 + 0x30 * k;
        uint32_t id = 0, joint = 0;
        uintptr_t unit = 0, parent = 0;
        float position[3] = {};
        WatchedUnit& w = g_watchUnits[k];
        if (!mem::Read(record + 4, id) || !mem::Read(record + 0x2C, unit) || !unit || !mem::Read(unit + 0x18, parent) ||
            parent != p || !mem::Read(unit + 0x1C, joint) ||
            !mem::SafeRead(reinterpret_cast<const void*>(unit + 0x30), position, sizeof(position))) {
            w = {};
            continue;
        }
        const bool moved = w.unit == unit && (w.joint != joint || std::fabs(w.position[0] - position[0]) > 0.01f ||
                                              std::fabs(w.position[1] - position[1]) > 0.01f ||
                                              std::fabs(w.position[2] - position[2]) > 0.01f);
        if (moved && unit != held && joint != 8 && joint != 13)  // holstered (the hands are joints 8 / 13)
            LOG("holster watch: item %04X on joint %u (was %u) local (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f); action %X, "
                "motion %X, weapon anims %s, %s-handed",
                id & 0xFFFF, joint, w.joint, w.position[0], w.position[1], w.position[2], position[0], position[1],
                position[2], action, motion & 0xFFFF, female ? "female" : "male", left ? "left" : "right");
        w.unit = unit;
        w.joint = joint;
        std::memcpy(w.position, position, sizeof(position));
    }
}
}  // namespace

void RegisterDiagnostics() {
    // Research (2026-11-05, Wesker's dash for any character): player 1 asked for an action by its number (the player's
    // RequestAction, vtable +0xDC: (action, 0, 0, -1, 0)), with the action / motion logs on to see what plays.
    Feature& ask = Add({ .id = "diag.request_action", .name = "Essai : demander une action au joueur 1 (recherche)",
                         .help = "Demande au joueur 1 l'action du curseur (son numéro, en décimal) et l'écrit dans trainer.log.",
                         .category = "Diagnostic", .kind = Kind::Action,
                         .available = [] { return actors::InGame() && actors::Player(0) != 0; },
                         .value = 71, .valueMin = 0, .valueMax = 512, .valueFormat = "%.0f", .valueStep = 1 });
    Feature* const askp = &ask;
    ask.run = [askp] {
        const uintptr_t p = actors::Player(0);
        uintptr_t vtable = 0, fn = 0;
        if (!p || !mem::Read(p, vtable) || !mem::Read(vtable + 0xDC, fn) || !fn) return false;
        const int action = static_cast<int>(askp->value + 0.5f);
        uint32_t before = 0;
        mem::Read(p + 0x10E0, before);
        using RequestFn = void(__fastcall*)(void* self, void* edx, int action, int a, int b, int c, int d);
        __try {
            reinterpret_cast<RequestFn>(fn)(reinterpret_cast<void*>(p), nullptr, action, 0, 0, -1, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG("request action test: %X faulted", action);
            return false;
        }
        LOG("request action test: player 1 asked for action %X (was %X)", action, before);
        return true;
    };
    Add({ .id = "diag.holster_watch", .name = "Surveillance des armes rangées",
          .help = "Écrit dans trainer.log chaque changement de place d'une arme rangée de Chris (fusil dans le dos, étui du "
                  "couteau...) et de ses réglages d'animations d'armes (homme / femme) et de main (droitier / gaucher), "
                  "avec l'action en cours, pour trouver ce qui les déplace.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_watchPlayer = 0;
              return true;
          },
          .tick = TickHolsterWatch,
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "diag.effect_log", .name = "Journal des effets (recherche)",
          .help = "Écrit dans trainer.log le nom de chaque liste d'effets demandée par le jeu (effect\\efl\\...) et "
                  "l'endroit du code qui la demande.",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              if (on && !EnsureEffectLogHook()) return false;
              g_effectLog = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    for (int i = 0; i < kCallLogs; ++i) {
        const bool sound = i == 0;
        Add({ .id = sound ? "diag.sound_log"
                    : i == 1 ? "diag.effect_spawn_log"
                    : i == 2 ? "diag.effect_at_log"
                    : i == 3 ? "diag.effect_new_log"
                    : i == 4 ? "diag.common_effect_log"
                    : i == 5 ? "diag.caption_log"
                    : i == 6 ? "diag.message_log" : "diag.item_new_log",
              .name = sound ? "Journal des sons (recherche)"
                      : i == 1 ? "Journal des effets lancés (recherche)"
                      : i == 2 ? "Journal des effets placés (recherche)"
                      : i == 3 ? "Journal des effets créés (recherche)"
                      : i == 4 ? "Journal des effets communs (recherche)"
                      : i == 5 ? "Journal des sous-titres (recherche)"
                      : i == 6 ? "Journal des messages (recherche)" : "Journal des objets créés (recherche)",
              .help = sound ? "Écrit dans trainer.log chaque son demandé par le jeu (banque, numéro, unité) et le code qui "
                              "le demande."
                      : i == 7 ? "Écrit dans trainer.log chaque objet (munitions, soins, armes...) créé par le jeu et le "
                                 "code qui le crée."
                      : i >= 5 ? "Écrit dans trainer.log chaque sous-titre (ou message) créé par le jeu et le code qui le "
                                 "crée."
                            : "Écrit dans trainer.log chaque effet lancé par le jeu (identifiant, unité, paramètres) et le "
                              "code qui le lance.",
              .category = "Diagnostic", .kind = Kind::Toggle,
              .set = [i](bool on) {
                  if (on && !EnsureCallLog(i)) return false;
                  g_callLogOn[i] = on;
                  if (on) g_callLogLines[i] = 0;
                  return true;
              },
              .available = [] { return game::IsSupported(); } });
    }
    Add({
        .id = "diag.export_dti",
        .name = "Exporter les classes du moteur (recherche)",
        .help = "Écrit re5_dti_classes.tsv / re5_vtables.tsv / re5_vfuncs.tsv dans %LOCALAPPDATA%\\RE5TrainerPlus.\nNoms de classes MT Framework lus dans la mémoire du jeu (aucun fichier du jeu n'est modifié). Le jeu se fige quelques secondes.",
        .category = "Diagnostic",
        .kind = Kind::Action,
        .run = ExportDti,
    });
}
}
