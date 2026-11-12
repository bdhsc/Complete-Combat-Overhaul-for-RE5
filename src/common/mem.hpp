#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace mem {

uint8_t* Base();  // re5dx9.exe image base
inline uint8_t* Rva(uintptr_t rva) { return Base() + rva; }

bool IsGameCode(const void* address, size_t size = 1);
bool IsReadable(const void* address, size_t size);

// Fault-tolerant copies (SEH guarded). Return false instead of crashing.
bool SafeRead(const void* address, void* out, size_t size);
bool SafeWrite(void* address, const void* in, size_t size);

template <class T> bool Read(uintptr_t address, T& out) { return SafeRead(reinterpret_cast<const void*>(address), &out, sizeof(T)); }
template <class T> bool Write(uintptr_t address, const T& value) { return SafeWrite(reinterpret_cast<void*>(address), &value, sizeof(T)); }

// Follows base -> [base] + o1 -> [..] + o2 ... ; the last offset is added, not dereferenced.
// Returns 0 when any link is unreadable or null.
uintptr_t Chain(uintptr_t base, std::initializer_list<uintptr_t> offsets);

// Writes into executable memory (changes page protection, flushes icache).
bool WriteCode(void* address, const void* bytes, size_t size);

// Gives game code pages back an executable protection if another tool left them PAGE_READWRITE.
void EnsureExecutable(void* address, size_t size);

// "8B 0D ?? ?? ?? ?? 56" style pattern; returns the single match in executable game sections,
// or nullptr if there is none or more than one.
uint8_t* FindUnique(const char* pattern);

// A guarded byte patch: applies only when the site holds `original` (or already `patched`),
// restores only when it still holds `patched`. Never writes over bytes it doesn't recognise.
// Sites are reference-counted: when two features patch the same bytes (e.g. crosshair + laser),
// the original is written back only when the last one is switched off.
class Patch {
public:
    Patch(const char* name, uintptr_t rva, std::vector<uint8_t> original, std::vector<uint8_t> patched);
    Patch(const char* name, uint8_t* address, std::vector<uint8_t> original, std::vector<uint8_t> patched);
    bool Apply();
    bool Restore();
    bool IsApplied() const;     // site currently holds the patched bytes
    bool IsRecognised() const;  // site holds original or patched bytes
    const char* Name() const { return name_; }
    uint8_t* Address() const;

private:
    const char* name_;
    uintptr_t rva_ = 0;
    uint8_t* address_ = nullptr;
    std::vector<uint8_t> original_, patched_;
    bool held_ = false;  // this instance holds a reference on the site
};

// Applies all patches or none (rolls back on failure).
bool ApplyAll(std::initializer_list<Patch*> patches);
bool RestoreAll(std::initializer_list<Patch*> patches);

std::string Hex(const void* data, size_t size);
}
