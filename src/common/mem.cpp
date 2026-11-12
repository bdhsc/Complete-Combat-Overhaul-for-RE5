#include "common/mem.hpp"
#include "common/log.hpp"

#include <windows.h>
#include <cstring>
#include <map>

namespace mem {

uint8_t* Base() {
    static uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    return base;
}

namespace {
bool QueryCommitted(const void* address, MEMORY_BASIC_INFORMATION& info) {
    return address && VirtualQuery(address, &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT &&
           !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}

bool RangeFits(const void* address, size_t size, const MEMORY_BASIC_INFORMATION& info) {
    auto start = reinterpret_cast<uintptr_t>(address);
    auto end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return size <= end - start;
}
}

// "Game code" = inside an executable section of re5dx9.exe according to its PE headers. The current
// page protection is not used: other tools (the Ultimate Trainer) leave code pages as PAGE_READWRITE.
bool IsGameCode(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!QueryCommitted(address, info) || info.Type != MEM_IMAGE || info.AllocationBase != Base()) return false;
    if (!RangeFits(address, size, info)) return false;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base());
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(Base() + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);
    const uintptr_t rva = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(Base());
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (rva >= section[i].VirtualAddress && rva + size <= section[i].VirtualAddress + section[i].Misc.VirtualSize) return true;
    }
    return false;
}

bool IsReadable(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!QueryCommitted(address, info)) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (info.Protect & readable) && RangeFits(address, size, info);
}

bool SafeRead(const void* address, void* out, size_t size) {
    if (!address) return false;
    __try {
        std::memcpy(out, address, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWrite(void* address, const void* in, size_t size) {
    if (!address) return false;
    __try {
        std::memcpy(address, in, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t Chain(uintptr_t base, std::initializer_list<uintptr_t> offsets) {
    uintptr_t current = base;
    size_t index = 0;
    for (uintptr_t offset : offsets) {
        if (++index == offsets.size()) return current ? current + offset : 0;
        uintptr_t next = 0;
        if (!current || !Read(current + offset, next) || !next) return 0;
        current = next;
    }
    // No offsets: dereference base once.
    uintptr_t value = 0;
    return Read(base, value) ? value : 0;
}

bool WriteCode(void* address, const void* bytes, size_t size) {
    DWORD old = 0;
    if (!address || !VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    const bool ok = SafeWrite(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD ignored = 0;
    VirtualProtect(address, size, old, &ignored);
    return ok;
}

void EnsureExecutable(void* address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) return;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (info.Protect & exec) return;
    DWORD old = 0;
    if (VirtualProtect(address, size, PAGE_EXECUTE_READ, &old))
        LOG("restored PAGE_EXECUTE_READ at %p (was 0x%lX)", address, old);
}

namespace {
bool ParsePattern(const char* text, std::vector<int>& out) {
    out.clear();
    for (const char* p = text; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') {
            out.push_back(-1);
            p += (p[1] == '?') ? 2 : 1;
            continue;
        }
        char byte[3] = { p[0], p[1], 0 };
        if (!p[1]) return false;
        out.push_back(static_cast<int>(strtoul(byte, nullptr, 16)));
        p += 2;
    }
    return !out.empty();
}
}

uint8_t* FindUnique(const char* pattern) {
    std::vector<int> pat;
    if (!ParsePattern(pattern, pat)) return nullptr;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(Base());
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(Base() + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);
    uint8_t* match = nullptr;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* start = Base() + section[i].VirtualAddress;
        size_t size = section[i].Misc.VirtualSize;
        if (size < pat.size() || !IsReadable(start, size)) continue;
        for (size_t off = 0; off + pat.size() <= size; ++off) {
            size_t k = 0;
            while (k < pat.size() && (pat[k] < 0 || start[off + k] == pat[k])) ++k;
            if (k != pat.size()) continue;
            if (match) return nullptr;  // ambiguous
            match = start + off;
        }
    }
    return match;
}

Patch::Patch(const char* name, uintptr_t rva, std::vector<uint8_t> original, std::vector<uint8_t> patched)
    : name_(name), rva_(rva), original_(std::move(original)), patched_(std::move(patched)) {}

Patch::Patch(const char* name, uint8_t* address, std::vector<uint8_t> original, std::vector<uint8_t> patched)
    : name_(name), address_(address), original_(std::move(original)), patched_(std::move(patched)) {}

uint8_t* Patch::Address() const { return address_ ? address_ : Rva(rva_); }

namespace {
// Patches may target code or read-only data of re5dx9.exe (e.g. resource path strings).
bool InImage(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    return IsReadable(address, size) && VirtualQuery(address, &info, sizeof(info)) && info.AllocationBase == Base();
}
}

bool Patch::IsApplied() const {
    auto* site = Address();
    return InImage(site, patched_.size()) && std::memcmp(site, patched_.data(), patched_.size()) == 0;
}

bool Patch::IsRecognised() const {
    auto* site = Address();
    if (!InImage(site, original_.size())) return false;
    return std::memcmp(site, original_.data(), original_.size()) == 0 || IsApplied();
}

namespace {
std::map<uint8_t*, int> g_siteRefs;
}

bool Patch::Apply() {
    if (held_ && IsApplied()) return true;
    auto* site = Address();
    int& refs = g_siteRefs[site];
    // Another feature already patched these exact bytes, or a trainer instance before this one left them patched (a
    // hot reload; 2026-11-01: the partner's character patches refused every frame): taken over, restored with the last.
    if (IsApplied()) {
        if (refs == 0) LOG("patch %s found applied at %p: taken over", name_, site);
        ++refs;
        held_ = true;
        return true;
    }
    if (!InImage(site, original_.size()) || std::memcmp(site, original_.data(), original_.size()) != 0) {
        LOG("patch %s refused: unexpected bytes at %p: %s", name_, site,
            IsReadable(site, original_.size()) ? Hex(site, original_.size()).c_str() : "unreadable");
        return false;
    }
    if (!WriteCode(site, patched_.data(), patched_.size())) return false;
    refs = 1;
    held_ = true;
    LOG("patch %s applied at %p", name_, site);
    return true;
}

bool Patch::Restore() {
    auto* site = Address();
    if (held_) {
        held_ = false;
        int& refs = g_siteRefs[site];
        if (refs > 1) {  // still needed by another feature
            --refs;
            return true;
        }
        refs = 0;
    }
    if (InImage(site, original_.size()) && std::memcmp(site, original_.data(), original_.size()) == 0) return true;
    if (!IsApplied()) {
        LOG("patch %s restore refused: site changed", name_);
        return false;
    }
    if (!WriteCode(site, original_.data(), original_.size())) return false;
    LOG("patch %s restored", name_);
    return true;
}

bool ApplyAll(std::initializer_list<Patch*> patches) {
    for (Patch* p : patches)
        if (!p->IsRecognised()) {
            LOG("patch set refused: %s not recognised", p->Name());
            return false;
        }
    std::vector<Patch*> done;
    for (Patch* p : patches) {
        if (!p->Apply()) {
            for (auto it = done.rbegin(); it != done.rend(); ++it) (*it)->Restore();
            return false;
        }
        done.push_back(p);
    }
    return true;
}

bool RestoreAll(std::initializer_list<Patch*> patches) {
    bool ok = true;
    for (Patch* p : patches) ok = p->Restore() && ok;
    return ok;
}

std::string Hex(const void* data, size_t size) {
    std::string out;
    char buf[4];
    auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        uint8_t b = 0;
        SafeRead(bytes + i, &b, 1);
        sprintf_s(buf, "%02X ", b);
        out += buf;
    }
    if (!out.empty()) out.pop_back();
    return out;
}
}
