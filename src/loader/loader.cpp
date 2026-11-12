// RE5TrainerLoader: waits for re5dx9.exe and loads RE5Trainer.dll (next to this exe) into it.
// If a previous trainer instance is running in the game, it is unloaded first (hot reload).
// The DLL is injected from a copy in %LOCALAPPDATA%\RE5TrainerPlus\bin so the build output never stays locked.
// Nothing is written to the game folder.  Flags: --quiet (no message boxes).
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {
const wchar_t* kTitle = L"RE5 Trainer";
constexpr const wchar_t* kUnloadEvent = L"Local\\RE5TrainerPlus_Unload";
constexpr const wchar_t* kUnloadedEvent = L"Local\\RE5TrainerPlus_Unloaded";
bool g_quiet = false;

void LogLine(const std::wstring& text);

int Message(const std::wstring& text, UINT flags) {
    LogLine(text);
    if (g_quiet) return IDOK;
    return MessageBoxW(nullptr, text.c_str(), kTitle, flags);
}

std::wstring DataBin();

void LogLine(const std::wstring& text) {
    std::wstring path = DataBin() + L"\\..\\loader.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fwprintf(f, L"[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, text.c_str());
        fclose(f);
    }
}

DWORD FindGame() {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{ sizeof(e) };
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (_wcsicmp(e.szExeFile, L"re5dx9.exe") == 0) pid = e.th32ProcessID;
    CloseHandle(snap);
    return pid;
}

bool HasModule(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W m{ sizeof(m) };
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &m); ok && !found; ok = Module32NextW(snap, &m))
        found = _wcsicmp(m.szModule, name) == 0;
    CloseHandle(snap);
    return found;
}

// A hot-reloadable copy (RE5Trainer_<tick>.dll) loaded in the game.
bool HasTrainerCopy(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W m{ sizeof(m) };
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &m); ok && !found; ok = Module32NextW(snap, &m))
        found = _wcsnicmp(m.szModule, L"RE5Trainer_", 11) == 0;
    CloseHandle(snap);
    return found;
}

bool UnloadEventExists() {
    HANDLE unload = OpenEventW(SYNCHRONIZE, FALSE, kUnloadEvent);
    if (unload) CloseHandle(unload);
    return unload != nullptr;
}

struct WindowSearch { DWORD pid; bool found; };
BOOL CALLBACK FindVisibleWindow(HWND hwnd, LPARAM param) {
    auto* s = reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == s->pid && IsWindowVisible(hwnd)) s->found = true;
    return !s->found;
}

std::wstring DataBin() {
    wchar_t* base = nullptr;
    std::wstring dir = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\RE5TrainerPlus";
        CoTaskMemFree(base);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\bin";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// Deletes old copies that are no longer loaded (locked ones fail silently).
void CleanOldCopies(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\RE5Trainer_*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Returns false if an old instance exists but did not unload in time.
bool UnloadPrevious() {
    HANDLE unload = OpenEventW(EVENT_MODIFY_STATE, FALSE, kUnloadEvent);
    if (!unload) return true;  // no instance running
    HANDLE unloaded = CreateEventW(nullptr, TRUE, FALSE, kUnloadedEvent);
    ResetEvent(unloaded);
    SetEvent(unload);
    DWORD r = WaitForSingleObject(unloaded, 8000);
    // The game paused in the background: the old instance unloads once it runs again (2026-11-10: twice the loader gave up after 8 s
    // and the old instance unloaded 2-3 s later, the game left without a trainer until the loader was run again). Waited for while the
    // game lives, 10 minutes at most.
    for (int i = 0; i < 600 && r != WAIT_OBJECT_0 && FindGame(); ++i) r = WaitForSingleObject(unloaded, 1000);
    ResetEvent(unload);
    CloseHandle(unload);
    CloseHandle(unloaded);
    Sleep(300);
    return r == WAIT_OBJECT_0;
}

bool Inject(DWORD pid, const std::wstring& dll, std::wstring& error) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                 PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        error = L"Impossible d'ouvrir le processus du jeu (code " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = remote && WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr);
    if (ok) {
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
        if (thread) {
            WaitForSingleObject(thread, 15000);
            DWORD code = 0;
            GetExitCodeThread(thread, &code);
            ok = code != 0;
            if (!ok) error = L"Le jeu a refusé de charger la DLL du trainer.";
            CloseHandle(thread);
        } else {
            ok = false;
            error = L"CreateRemoteThread a échoué (code " + std::to_wstring(GetLastError()) + L").";
        }
    } else {
        error = L"Écriture dans le processus du jeu impossible.";
    }
    if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}
}

// The data shipped with a release (motions, sounds, hit shapes: the "data" folder next to this exe) copied into the trainer's data folder,
// a file there replaced only by a newer one (user, 2026-11-12: the Nexus release carries the RE6 / RE4 animations).
void InstallBundledData(const std::wstring& exeDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path from = fs::path(exeDir) / L"data";
    if (!fs::is_directory(from, ec)) return;
    fs::copy(from, fs::path(DataBin()).parent_path(), fs::copy_options::recursive | fs::copy_options::update_existing, ec);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    g_quiet = cmd && wcsstr(cmd, L"--quiet");
    if (cmd && wcsstr(cmd, L"--unload")) {  // remove the trainer from the running game, inject nothing
        if (!UnloadPrevious()) {
            Message(L"Le trainer ne s'est pas déchargé (jeu en pause ou réduit ?).", MB_ICONWARNING);
            return 1;
        }
        return 0;
    }
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring source(self);
    source = source.substr(0, source.find_last_of(L'\\') + 1) + L"RE5Trainer.dll";
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Message(L"Fichier introuvable :\n" + source, MB_ICONERROR);
        return 1;
    }
    {
        InstallBundledData(source.substr(0, source.find_last_of(L'\\')));
    }

    DWORD pid = FindGame();
    if (!pid) {
        if (Message(L"Resident Evil 5 n'est pas lancé.\n\nLancez le jeu : le trainer se chargera tout seul dès que la fenêtre du jeu apparaît.\n\n(OK = attendre, Annuler = quitter)",
                    MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return 0;
        for (int i = 0; i < 1200 && !(pid = FindGame()); ++i) Sleep(500);
        if (!pid) return 0;
    }
    if (HasModule(pid, L"RE5Trainer.dll")) {
        Message(L"Une ancienne version du trainer (sans rechargement à chaud) est chargée.\nRelancez le jeu une fois pour passer à la nouvelle version.", MB_ICONWARNING);
        return 1;
    }
    // An instance still starting has no unload event yet (it comes at the end of its init): two loaders 9 s apart left
    // two instances patching the game at once, which crashed it (2026-10-26). Its init is awaited, then it is unloaded.
    for (int i = 0; i < 120 && HasTrainerCopy(pid) && !UnloadEventExists(); ++i) Sleep(500);
    if (!UnloadPrevious()) {
        Message(L"L'ancienne instance du trainer ne s'est pas déchargée (jeu en pause ou réduit ?).\nRéessayez avec la fenêtre du jeu visible.", MB_ICONWARNING);
        return 1;
    }
    for (int i = 0; i < 10 && HasTrainerCopy(pid); ++i) Sleep(300);  // its DLL freed after its unloaded event
    if (HasTrainerCopy(pid)) {
        Message(L"Une instance du trainer est encore chargée dans le jeu : rien n'a été injecté.", MB_ICONWARNING);
        return 1;
    }
    // Let the game create its window and D3D device first.
    for (int i = 0; i < 600; ++i) {
        WindowSearch s{ pid, false };
        EnumWindows(FindVisibleWindow, reinterpret_cast<LPARAM>(&s));
        if (s.found && HasModule(pid, L"d3d9.dll")) break;
        Sleep(500);
    }

    const std::wstring bin = DataBin();
    CleanOldCopies(bin);
    const std::wstring copy = bin + L"\\RE5Trainer_" + std::to_wstring(GetTickCount64()) + L".dll";
    if (!CopyFileW(source.c_str(), copy.c_str(), FALSE)) {
        Message(L"Copie de la DLL impossible vers :\n" + copy, MB_ICONERROR);
        return 1;
    }
    std::wstring error;
    if (!Inject(pid, copy, error)) {
        Message(error, MB_ICONERROR);
        return 1;
    }
    LogLine(L"injected " + copy);
    MessageBeep(MB_OK);
    return 0;
}
