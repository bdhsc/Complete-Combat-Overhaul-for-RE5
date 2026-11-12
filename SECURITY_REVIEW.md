# Security review notes

This document explains intentional behavior to help a reviewer investigate detections. It does not assert that an antivirus detection has already been confirmed as a false positive.

## Entry points

- `src/loader/loader.cpp`: process enumeration, selecting `re5dx9.exe`, `OpenProcess`, allocating/writing the DLL path, and `CreateRemoteThread` with `LoadLibraryW`; copying optional data; cleanup of old trainer DLL copies; local hot-reload events.
- `src/dllmain.cpp`: initialization, feature registration, hooks, supported-game checks and unload coordination.
- `src/common/mem.cpp`: guarded memory access and code patches.
- `src/common/game.cpp`: supported executable checks.
- `src/hooks/`: Direct3D 9 and DirectInput integration.
- `src/features/`: individual gameplay patches and runtime actions.
- `src/common/features.cpp`: shortcut handling and the file-based local test bridge.
- `src/common/settings.cpp`, `log.cpp`, `crashlog.cpp`, `tracer.cpp`: settings and diagnostic output.
- `third_party/minhook/` and `third_party/imgui/`: vendored dependencies, with upstream notices.

## Local writes and expected functionality

The trainer intentionally accesses and modifies the running game's process memory. It stores its own data under `%LOCALAPPDATA%\RE5TrainerPlus`, including settings, logs, diagnostic exports, bridge files and runtime data. Inventory, money and character options can affect the game's saved state through the game itself.

The file-based test bridge and diagnostic functionality are included in the original first-party source snapshot. They have not been removed to make the review build appear different from the release implementation.

## Material available for review

- Buildable source and build steps.
- License notices for included dependencies.
- SHA-256 hashes of the two release executables from `RE5TrainerPlus_v1.0.zip` as it existed when this source snapshot was prepared.
- Local build verification in `BUILD_VERIFICATION.md`.

No scan verdict is inferred from these documents. Any review should refer to the exact uploaded binary hashes and the actual scanner report.
