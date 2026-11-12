#include "common/crashlog.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>

namespace crashlog {
namespace {
PVOID g_handler = nullptr;
std::atomic<int> g_logged{ 0 };

bool Fatal(DWORD code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
           code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000409 /* stack buffer overrun */ ||
           code == 0xC000000D /* CRT invalid parameter */ || code == EXCEPTION_INT_DIVIDE_BY_ZERO;
}

// "re5dx9+RVA", "<module>+off" or "" for a value that is not inside a loaded module.
void Describe(uintptr_t v, char* out, size_t size) {
    out[0] = 0;
    HMODULE mod = nullptr;
    if (!v || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCSTR>(v), &mod) || !mod)
        return;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    sprintf_s(out, size, "%s+%X", name ? name + 1 : path, static_cast<unsigned>(v - reinterpret_cast<uintptr_t>(mod)));
}

uint32_t ReadU32(uintptr_t a, bool& ok) {
    uint32_t v = 0;
    ok = mem::IsReadable(reinterpret_cast<void*>(a), 4) && mem::SafeRead(reinterpret_cast<void*>(a), &v, 4);
    return v;
}

void Value(FILE* f, const char* label, uintptr_t v) {
    char where[160];
    Describe(v, where, sizeof(where));
    fprintf(f, "  %-4s %08X %s", label, static_cast<unsigned>(v), where);
    bool ok = false;
    const uint32_t vtable = ReadU32(v, ok);
    if (ok) {
        char vt[160];
        Describe(vtable, vt, sizeof(vt));
        if (vt[0]) fprintf(f, "  -> object, vtable %08X %s", vtable, vt);
    }
    fprintf(f, "\n");
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (!Fatal(rec->ExceptionCode) || g_logged.fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = info->ContextRecord;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (logx::DataDir() + L"\\crash.log").c_str(), L"a") != 0 || !f) return EXCEPTION_CONTINUE_SEARCH;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char where[160];
    Describe(c->Eip, where, sizeof(where));
    fprintf(f, "==== %04d-%02d-%02d %02d:%02d:%02d exception %08lX at %08lX %s thread %lu\n", t.wYear, t.wMonth, t.wDay,
            t.wHour, t.wMinute, t.wSecond, rec->ExceptionCode, c->Eip, where, GetCurrentThreadId());
    if (rec->NumberParameters >= 2)
        fprintf(f, "  access %s %08lX\n", rec->ExceptionInformation[0] == 8 ? "execute" : rec->ExceptionInformation[0] ? "write" : "read",
                static_cast<unsigned long>(rec->ExceptionInformation[1]));
    Value(f, "eax", c->Eax);
    Value(f, "ebx", c->Ebx);
    Value(f, "ecx", c->Ecx);
    Value(f, "edx", c->Edx);
    Value(f, "esi", c->Esi);
    Value(f, "edi", c->Edi);
    Value(f, "ebp", c->Ebp);
    Value(f, "esp", c->Esp);

    fprintf(f, "  frames (ebp chain):\n");
    uintptr_t ebp = c->Ebp;
    for (int i = 0; i < 24 && ebp; ++i) {
        bool ok1 = false, ok2 = false;
        const uint32_t next = ReadU32(ebp, ok1), ret = ReadU32(ebp + 4, ok2);
        if (!ok1 || !ok2) break;
        Describe(ret, where, sizeof(where));
        fprintf(f, "    %08X %s  args %08X %08X %08X\n", ret, where, ReadU32(ebp + 8, ok1), ReadU32(ebp + 12, ok1),
                ReadU32(ebp + 16, ok1));
        if (next <= ebp) break;
        ebp = next;
    }

    fprintf(f, "  stack:\n");
    for (int i = 0; i < 160; ++i) {
        bool ok = false;
        const uint32_t v = ReadU32(c->Esp + 4 * i, ok);
        if (!ok) break;
        Describe(v, where, sizeof(where));
        if (where[0]) fprintf(f, "    esp+%03X %08X %s\n", 4 * i, v, where);
    }
    fprintf(f, "  strings on the stack:\n");
    {
        char text[0x800];
        const size_t n = mem::IsReadable(reinterpret_cast<void*>(c->Esp), sizeof(text)) &&
                                 mem::SafeRead(reinterpret_cast<void*>(c->Esp), text, sizeof(text))
                             ? sizeof(text)
                             : 0;
        int shown = 0;
        for (size_t i = 0; i < n && shown < 8;) {
            size_t j = i;
            while (j < n && text[j] >= 0x20 && text[j] < 0x7F) ++j;
            if (j - i >= 12) {
                fprintf(f, "    esp+%03X %.*s\n", static_cast<unsigned>(i), static_cast<int>(j - i), text + i);
                ++shown;
            }
            i = j + 1;
        }
    }
    fclose(f);
    return EXCEPTION_CONTINUE_SEARCH;  // the game / Windows handle it as before
}
}

void Install() {
    if (!g_handler) g_handler = AddVectoredExceptionHandler(0, Handler);
}

void Uninstall() {
    if (g_handler) RemoveVectoredExceptionHandler(g_handler);
    g_handler = nullptr;
}
}
