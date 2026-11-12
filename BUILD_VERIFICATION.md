# Build verification — 2026-11-12

The source-only copy in this repository was configured and built independently from the development project's original build directory.

- Generator: Visual Studio 18 2026, **Win32**.
- CMake: **4.3.1-msvc1**.
- Compiler identification: **MSVC 19.51.36260.0**.
- MSVC toolset directory: **14.51.36231**.
- Selected Windows SDK: **10.0.26100.0**.
- Configuration: **Release**.
- `RE5TrainerLoader`: **build succeeded**.
- `RE5Trainer`: **build succeeded**.

Commands used (run from the repository root, with the installed CMake executable available):

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release --target RE5TrainerLoader -- /nr:false /m:1 /v:minimal
cmake --build build --config Release --target RE5Trainer -- /nr:false /m:1 /v:minimal
```

A CMake deprecation warning from MinHook's older minimum-version declaration did not prevent the build. Dependencies were built from the vendored source.

All 107 first-party source/build-input files in the prepared snapshot were compared by SHA-256 with the development directory and matched. No gameplay code was changed for this publication. This comparison covers the source inputs and top-level CMake file, not the newly written documentation.

No game or trainer was launched during this verification. The rebuilt executables were not substituted for the published v1.0 release. Compilation success is not an antivirus verdict, and no byte-for-byte binary reproducibility claim is made.
