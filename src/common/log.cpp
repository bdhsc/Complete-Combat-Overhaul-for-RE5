#include "common/log.hpp"

#include <windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>

namespace logx {
namespace {
std::mutex g_lock;
FILE* g_file = nullptr;
}

const std::wstring& DataDir() {
    static std::wstring dir = [] {
        wchar_t* base = nullptr;
        std::wstring d = L".";
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
            d = std::wstring(base) + L"\\RE5TrainerPlus";
            CoTaskMemFree(base);
        }
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

void Init() {
    std::lock_guard lock(g_lock);
    if (g_file) return;
    // The previous load's log is kept (a reload during a research session wiped what had just been logged).
    const std::wstring path = DataDir() + L"\\trainer.log", previous = DataDir() + L"\\trainer.prev.log";
    if (!MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING)) CopyFileW(path.c_str(), previous.c_str(), FALSE);
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
}

void Write(const char* fmt, ...) {
    std::lock_guard lock(g_lock);
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);
    fputc('\n', g_file);
    fflush(g_file);
}
}
