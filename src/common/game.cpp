#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>
#include <vector>

namespace game {
namespace {
std::string g_version = "?";

struct FindWindowCtx {
    DWORD pid;
    HWND best;
    long bestArea;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<FindWindowCtx*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT r{};
    GetClientRect(hwnd, &r);
    long area = (r.right - r.left) * (r.bottom - r.top);
    if (area > ctx->bestArea) {
        ctx->best = hwnd;
        ctx->bestArea = area;
    }
    return TRUE;
}
}

bool Init() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (size) {
        std::vector<uint8_t> data(size);
        VS_FIXEDFILEINFO* info = nullptr;
        UINT len = 0;
        if (GetFileVersionInfoW(path, 0, size, data.data()) &&
            VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) && info) {
            char buf[64];
            sprintf_s(buf, "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                      HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
            g_version = buf;
        }
    }
    LOG("game version %s (%s), base %p", g_version.c_str(), IsSupported() ? "supported" : "UNSUPPORTED", mem::Base());
    return IsSupported();
}

bool IsSupported() { return g_version == kSupportedVersion; }
const std::string& Version() { return g_version; }

void* MainWindow() {
    FindWindowCtx ctx{ GetCurrentProcessId(), nullptr, 0 };
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

uintptr_t CurrentPlayer() {
    uintptr_t manager = 0, player = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(rva::PlayerManager)), manager) || !manager) return 0;
    if (!mem::Read(manager + off::ManagerCurrentPlayer, player)) return 0;
    return player;
}

bool PlayerOnFoot() {
    uintptr_t player = CurrentPlayer();
    uint32_t context = 0;
    if (!player || !mem::Read(player + off::PlayerVehicleContext, context)) return false;
    return context == 0x101;
}
}
