#include "common/features.hpp"
#include "common/i18n.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/settings.hpp"
#include "hooks/hooks.hpp"
#include "ui/overlay.hpp"
#include "common/crashlog.hpp"
#include "common/tracer.hpp"
#include "features/body_cut.hpp"

#include <windows.h>
#include <MinHook.h>

namespace features {
bool AccessoryRestorePending();  // player.cpp: a borrowed move's leftovers not yet put back
void RegisterAmmo();
void RegisterDiagnostics();
void RegisterPatchFeatures();
void RegisterPlayerFeatures();
void RegisterEnemyFeatures();
void RegisterMercenariesFeatures();
void RegisterActionLog();
void RegisterCoopSolo();
void RegisterFinalFight();
void RegisterBeltLamp();
void RegisterCrank();
void RegisterBoat();
void RegisterRaft();
void RegisterSoloScenario();
void RegisterKnifeIcon();
void RegisterPerfectDodge();
void RegisterShellReload();
void RegisterEnvExec();
void RegisterPerCharacterMelee();
void RegisterSwapFeatures();
void RegisterCameraFeatures();
void RegisterInventory();
void RegisterLicker();
void RegisterDying();
void RegisterKnife();
void RegisterQuickKnife();
void RegisterParry();
void RegisterCombo();
void RegisterBetaAnims();
void RegisterPickup();
void RegisterMenuKeys();
void RegisterMovement();
void RegisterGrabs();
void RegisterFireRate();
void RegisterEnemySpawn();
void RegisterRe6Motions();
void RegisterTimeSpeed();
void RegisterWeskerMoves();
void RegisterHudColors();
void RegisterWeskerBoss();
void RegisterStamina();
void RegisterJillDevice();
void RegisterForcedDeath();
void RegisterRocketCatch();
void RegisterMeleeTuning();

void RegisterAll() {
    RegisterPlayerFeatures();
    RegisterAmmo();
    RegisterPatchFeatures();
    RegisterEnemyFeatures();
    RegisterMercenariesFeatures();
    RegisterSwapFeatures();
    RegisterCameraFeatures();
    RegisterInventory();
    RegisterLicker();
    RegisterDying();
    RegisterKnife();
    RegisterQuickKnife();
    RegisterParry();
    RegisterCombo();
    RegisterBetaAnims();
    RegisterPickup();
    RegisterMenuKeys();
    RegisterMovement();
    RegisterGrabs();
    RegisterFireRate();
    RegisterEnemySpawn();
    RegisterRe6Motions();
    RegisterTimeSpeed();
    RegisterWeskerMoves();
    RegisterHudColors();
    RegisterWeskerBoss();
    RegisterStamina();
    RegisterJillDevice();
    RegisterForcedDeath();
    RegisterRocketCatch();
    RegisterMeleeTuning();
    RegisterDiagnostics();
    bodycut::RegisterFeatures();
    RegisterActionLog();
    RegisterCoopSolo();
    RegisterFinalFight();
    i18n::Register();
    RegisterBeltLamp();
    RegisterCrank();
    RegisterBoat();
    RegisterRaft();
    RegisterSoloScenario();
    RegisterKnifeIcon();
    RegisterPerfectDodge();
    RegisterShellReload();
    RegisterEnvExec();
    RegisterPerCharacterMelee();  // last: it copies the melee swap page's options per character
}
}

namespace {
// Hot reload: the loader signals kUnloadEvent; this instance undoes its changes, unhooks, signals
// kUnloadedEvent and frees itself so a freshly built copy can be injected without restarting the game.
constexpr const wchar_t* kUnloadEvent = L"Local\\RE5TrainerPlus_Unload";
constexpr const wchar_t* kUnloadedEvent = L"Local\\RE5TrainerPlus_Unloaded";

HMODULE g_module = nullptr;

// Bare-bones breadcrumb (no CRT, no statics) for diagnosing early start-up problems:
// boot.txt next to the loaded DLL.
void Breadcrumb(const char* text) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (!n || n >= MAX_PATH - 16) return;
    wchar_t* slash = path + n;
    while (slash > path && slash[-1] != L'\\') --slash;
    lstrcpyW(slash, L"boot.txt");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, text, lstrlenA(text), &w, nullptr);
    WriteFile(h, "\r\n", 2, &w, nullptr);
    CloseHandle(h);
}

DWORD WINAPI UnloadWatcher(LPVOID event) {
    WaitForSingleObject(event, INFINITE);
    LOG("unload requested");
    // Not during a borrowed move (player.cpp: its leftovers on the accessories are put back at its end; user, 2026-11-06:
    // a reload during Wesker's guard left the sheath through the harness until a room change): 10 s at most.
    if (features::AccessoryRestorePending()) {
        LOG("unload: waiting for player 1's borrowed move to end (accessories)");
        for (int i = 0; i < 500 && features::AccessoryRestorePending(); ++i) Sleep(20);
        Sleep(100);
    }
    features::RequestStop();
    overlay::RequestShutdown();
    // The render thread undoes everything at its next Present; a frame may still be applying options (seconds at the
    // start). Never freed before that (2026-10-27: freed after 2 s while the start-up's batch still ran, the game
    // crashed twice): up to 60 s, else this instance stays loaded (the loader then refuses to inject a second one).
    for (int i = 0; i < 3000 && !overlay::ShutdownDone(); ++i) Sleep(20);
    if (!overlay::ShutdownDone()) {
        LOG("unload: the overlay did not shut down in 60 s (no frame drawn?): this instance stays loaded");
        return 0;
    }
    tracer::Shutdown();
    crashlog::Uninstall();
    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(300);  // let threads already inside a detour return
    MH_Uninitialize();
    LOG("unloaded");
    HANDLE done = OpenEventW(EVENT_MODIFY_STATE, FALSE, kUnloadedEvent);
    if (done) {
        SetEvent(done);
        CloseHandle(done);
    }
    CloseHandle(event);
    FreeLibraryAndExitThread(g_module, 0);
}

DWORD WINAPI InitThread(LPVOID) {
    Breadcrumb("init thread start");
    logx::Init();
    Breadcrumb("log ready");
    LOG("RE5TrainerPlus loading");
    crashlog::Install();
    for (int i = 0; i < 600 && !GetModuleHandleW(L"d3d9.dll"); ++i) Sleep(100);

    game::Init();
    if (MH_Initialize() != MH_OK) {
        LOG("MH_Initialize failed");
        return 0;
    }
    features::RegisterAll();
    settings::Load();
    if (!hooks::InstallD3D9()) LOG("overlay unavailable: d3d9 hook failed");
    hooks::InstallDInput();

    HANDLE unload = CreateEventW(nullptr, TRUE, FALSE, kUnloadEvent);
    if (unload) ResetEvent(unload);  // the object may survive the previous instance, still signalled
    if (HANDLE unloaded = OpenEventW(EVENT_MODIFY_STATE, FALSE, kUnloadedEvent)) {
        ResetEvent(unloaded);
        CloseHandle(unloaded);
    }
    if (unload) {
        if (HANDLE t = CreateThread(nullptr, 0, UnloadWatcher, unload, 0, nullptr)) CloseHandle(t);
    }
    LOG("RE5TrainerPlus ready");
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        Breadcrumb("DllMain attach");
        DisableThreadLibraryCalls(module);
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
