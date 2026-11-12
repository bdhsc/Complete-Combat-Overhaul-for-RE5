#pragma once
#include <cstdint>
#include <string>

// Facts about re5dx9.exe 1.0.0.129 (Steam). RVAs are offsets from the module base.
// Every fact here is recorded in MODLOG.md with how it was verified.
namespace game {

constexpr const char* kSupportedVersion = "1.0.0.129";

namespace rva {
constexpr uintptr_t PlayerManager = 0xDB27DC;  // ptr; current player at +0x24 (Codex, user-validated)
}

namespace off {
constexpr uintptr_t ManagerCurrentPlayer = 0x24;
constexpr uintptr_t PlayerVehicleContext = 0x364;  // 0x101 on foot, 0x001 boat/truck (user-validated)
}

bool Init();                       // reads the exe version
bool IsSupported();                // version == kSupportedVersion
const std::string& Version();
void* MainWindow();                // HWND of the game window (largest visible top-level window of this process)

uintptr_t CurrentPlayer();         // 0 when not in game
bool PlayerOnFoot();               // false in vehicles or when unknown (fail closed)
}
