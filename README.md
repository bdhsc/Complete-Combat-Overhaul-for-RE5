# Complete Combat Overhaul for RE5 — v1.0 source

Source code for **Complete Combat Overhaul for RE5**, the Resident Evil 5 mod/trainer published by **bdhsc**. The executable and internal project names remain RE5 Trainer+. This repository provides the C/C++ implementation and build instructions for code review, including Nexus Mods' manual safety review.

Supported game: **Resident Evil 5 / Gold Edition on Steam, `re5dx9.exe` 1.0.0.129, DirectX 9, 32-bit**. Intended for solo/offline use.

Version 1.0 is the first release. Some imported moves, environmental execution placement and prototype movement still need refinement. Future plans include more detailed parry customization by enemy attack, playable RE6 characters and RE4 Remake-style knife grab escapes.

## Build

See [BUILDING.md](BUILDING.md) for prerequisites and complete instructions. With Visual Studio 2026, C++ desktop tools, the Windows SDK and CMake installed:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release --target RE5Trainer RE5TrainerLoader
```

Outputs: `build/Release/RE5Trainer.dll` and `build/Release/RE5TrainerLoader.exe`.

## What the loader and DLL do

- The loader searches running processes for `re5dx9.exe` and loads the trainer DLL into that process using Windows process-memory and remote-thread APIs. See `src/loader/loader.cpp`.
- The DLL hooks supported game functions and Direct3D 9 / DirectInput to implement the menu, shortcuts and gameplay changes. MinHook handles function hooks; Dear ImGui draws the menu.
- The loader copies its DLL and optional runtime data to `%LOCALAPPDATA%\RE5TrainerPlus`. Settings, logs, diagnostic files and the local test bridge are stored there.
- The loader supports unloading a previous trainer instance through local Windows events before loading a new copy.
- Injection and memory patching are intentional trainer functionality and may contribute to heuristic detections. This explanation is not an antivirus verdict or a guarantee of safety; the release should be reviewed on its merits.

See [SECURITY_REVIEW.md](SECURITY_REVIEW.md) for review entry points and [RELEASE_SHA256.txt](RELEASE_SHA256.txt) for hashes of the v1.0 binaries in the local release archive used to prepare this source snapshot.

## Scope

First-party source and the top-level build configuration are copied without gameplay changes from the v1.0 working project. Relevant vendored library sources and their notices are included. Private Git history, development notes, research scripts, binary memory dumps, saved games, generated build files, compiled releases and game animation/audio assets are excluded.

The source compiles without the external game animation/audio data. Imported animation and audio features require the corresponding runtime data distributed separately with the mod; that data is not recreated by compiling this repository. Do not expect a byte-for-byte match between a fresh build and the uploaded binaries: compiler versions, build paths and build metadata can differ.

## Credits and licensing

- Inspired by the Ultimate Trainer by Raz0r & Dante.
- Developed with assistance from Claude (Anthropic).
- Dear ImGui: MIT. MinHook: BSD 2-Clause. See their `LICENSE.txt` files and `THIRD_PARTY_LICENSES.txt`.
- Resident Evil, its characters and original game assets belong to Capcom. No game animation/audio binaries are included here.

No new license is assigned to the author's original source in this publication. Public visibility permits inspection; it does not by itself grant general reuse rights. Third-party components retain their existing licenses.
