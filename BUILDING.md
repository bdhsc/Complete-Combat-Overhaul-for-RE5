# Building RE5 Trainer+ v1.0

## Prerequisites

- Windows x64 capable of building and running Win32 applications.
- Visual Studio 2026 Community (or another edition) with **Desktop development with C++**, MSVC x86/x64 build tools and a Windows SDK.
- CMake 3.20 or newer. This snapshot is validated separately with the installed Visual Studio 2026 CMake toolchain; see `BUILD_VERIFICATION.md` for actual results.

Dear ImGui and MinHook sources are included. No package downloads, game files, external credentials or Python generators are required to compile the two targets. Translation tables and generated include files used by the build are already in `src`.

## Build commands

Open a PowerShell terminal where `cmake` is available and change to the repository root:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release --target RE5Trainer RE5TrainerLoader
```

The architecture **must be Win32 (x86)**. A 64-bit configuration is rejected because the supported game executable is 32-bit.

Visual Studio's bundled CMake can also be invoked by its full installed path if `cmake` is not on PATH. The exact path depends on the installed edition and location.

The original configuration requires C++20, uses the static MSVC runtime, links Windows/DirectX libraries and emits a DLL map file for crash diagnostics. Keep the Release configuration for review of release behavior.

## Outputs

- `build/Release/RE5Trainer.dll`
- `build/Release/RE5TrainerLoader.exe`
- `build/Release/RE5Trainer.map` (diagnostic link map)

To verify compilation, no game needs to be running. Do not run the loader just to verify a build: running it is a separate operation that attaches to the game.

## Optional runtime use

Keep the loader and DLL together. If testing imported animation/audio features, place the separately distributed `data` directory beside them. Start the supported game, stay at the title screen, then run the loader. Press Insert to open the menu.

Back up saves before testing options that change inventory, money or characters. Restore a modified knife slot to Default and save before playing without the trainer. Use only offline/solo.

## Reproduction limits

This repository is a source snapshot, not an archive of the original build machine. A successful rebuild establishes that the provided source is buildable. It does not alone prove that a binary is safe or that it is byte-identical to the Nexus upload. Compare the exact uploaded release against `RELEASE_SHA256.txt` and review the source and behavior independently.
