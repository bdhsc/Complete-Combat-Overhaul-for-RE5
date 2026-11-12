#include "common/tracer.hpp"
#include "common/log.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

namespace tracer {
namespace {
std::atomic<bool> g_busy{ false };
// Cleared before the disarm: a thread inside the handler while ArmAll(false) ran got its exception context (debug
// registers still armed) back when the handler returned, so the breakpoint survived the trace; with the handler
// removed at the unload the game crashed on it (2026-11-03 22:51, EXCEPTION_SINGLE_STEP at RVA 0x1F1801 after an
// execute trace). Now a hit after the trace disarms that thread, and the disarm runs twice.
std::atomic<bool> g_armed{ false };
std::mutex g_lock;
// (eip, return address of the frame [ebp+4], the one above it) -> count. The callers are read only when ebp lies in the
// thread's stack (a function without a frame pointer gives its caller's frame: still a useful hint).
// Execution breakpoints (mode "x") record the registers instead: eax ecx edx ebx esi edi.
struct Key {
    uint32_t eip, caller, caller2;
    uint32_t regs[7];  // eax ecx edx ebx esi edi, [eax + g_deref]
    bool operator<(const Key& o) const {
        if (eip != o.eip) return eip < o.eip;
        if (caller != o.caller) return caller < o.caller;
        if (caller2 != o.caller2) return caller2 < o.caller2;
        return std::memcmp(regs, o.regs, sizeof(regs)) < 0;
    }
};
bool g_execute = false;
int g_deref = 0;  // mode "x": the len argument, an offset read from eax at the breakpoint

uint32_t ReadGuarded(uint32_t address) {
    __try {
        return *reinterpret_cast<const uint32_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xDEADBEEF;
    }
}
std::map<Key, uint32_t> g_hits;
PVOID g_veh = nullptr;

bool OnStack(uint32_t p) {
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    return p >= reinterpret_cast<uint32_t>(tib->StackLimit) && p + 8 <= reinterpret_cast<uint32_t>(tib->StackBase);
}
uintptr_t g_address = 0;
// Set by Shutdown: a trace still running when the trainer unloads stops at once. Removing the handler while the
// debug registers were armed crashed the game on the next access (EXCEPTION_SINGLE_STEP at RVA 0x441F93, a reload
// during a 120 s trace, 2026-10-14): the old Shutdown gave up waiting after 30 s.
HANDLE g_cancel = nullptr;

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* ctx = info->ContextRecord;
    if (!(ctx->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    if (!g_armed) {
        ctx->Dr0 = 0;
        ctx->Dr7 &= ~0x000F0003u;
        ctx->Dr6 = 0;
        ctx->EFlags |= 0x10000;  // an execute breakpoint: run the instruction
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    Key key{ ctx->Eip, 0, 0, {} };
    if (g_execute) {
        const DWORD r[7] = { ctx->Eax, ctx->Ecx, ctx->Edx, ctx->Ebx, ctx->Esi, ctx->Edi, ReadGuarded(ctx->Eax + g_deref) };
        std::memcpy(key.regs, r, sizeof(r));
        ctx->EFlags |= 0x10000;  // resume flag: run the instruction instead of breaking on it again
    } else if (OnStack(ctx->Ebp)) {
        const uint32_t* frame = reinterpret_cast<const uint32_t*>(ctx->Ebp);
        key.caller = frame[1];
        if (OnStack(frame[0])) key.caller2 = reinterpret_cast<const uint32_t*>(frame[0])[1];
    }
    {
        std::lock_guard lock(g_lock);
        if (g_hits.size() < 4096) ++g_hits[key];
    }
    ctx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void ArmAll(bool arm, DWORD dr7) {
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{ sizeof(te) };
    int count = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &ctx)) {
                ctx.Dr0 = arm ? static_cast<DWORD>(g_address) : 0;
                ctx.Dr7 = arm ? dr7 : (ctx.Dr7 & ~0x000F0003u);
                ctx.Dr6 = 0;
                if (SetThreadContext(t, &ctx)) ++count;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    LOG("tracer: %s on %d threads", arm ? "armed" : "disarmed", count);
}

struct Job {
    int seconds;
    DWORD dr7;
    std::string label;
};

DWORD WINAPI Worker(LPVOID param) {
    Job* job = static_cast<Job*>(param);
    g_armed = true;
    ArmAll(true, job->dr7);
    WaitForSingleObject(g_cancel, job->seconds * 1000);
    g_armed = false;
    ArmAll(false, 0);
    Sleep(100);
    ArmAll(false, 0);  // the threads that were inside the handler during the first pass
    Sleep(50);
    std::vector<std::pair<Key, uint32_t>> rows;
    {
        std::lock_guard lock(g_lock);
        rows.assign(g_hits.begin(), g_hits.end());
        g_hits.clear();
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second > b.second; });
    FILE* f = nullptr;
    if (_wfopen_s(&f, (logx::DataDir() + L"\\trace_" + std::wstring(job->label.begin(), job->label.end()) + L".txt").c_str(), L"w") == 0 && f) {
        fprintf(f, "# address %08X, %d s; eip after access (rva) count [ebp+4] [[ebp]+4]\n", static_cast<unsigned>(g_address),
                job->seconds);
        for (auto& [k, n] : rows) {
            if (g_execute)
                fprintf(f, "%08X\t%06X\t%u\teax %08X ecx %08X edx %08X ebx %08X esi %08X edi %08X [eax+%X] %08X (%g)\n", k.eip,
                        k.eip - 0x400000, n, k.regs[0], k.regs[1], k.regs[2], k.regs[3], k.regs[4], k.regs[5], g_deref,
                        k.regs[6], *reinterpret_cast<const float*>(&k.regs[6]));
            else
                fprintf(f, "%08X\t%06X\t%u\t%08X\t%08X\n", k.eip, k.eip - 0x400000, n, k.caller, k.caller2);
        }
        fclose(f);
    }
    LOG("tracer: %zu distinct instructions for %s", rows.size(), job->label.c_str());
    delete job;
    g_busy = false;
    return 0;
}
}

bool Busy() { return g_busy; }

void Shutdown() {
    if (g_cancel) SetEvent(g_cancel);
    for (int i = 0; i < 200 && g_busy; ++i) Sleep(50);
    g_armed = false;
    if (g_busy || g_veh) ArmAll(false, 0);  // never leave a breakpoint without its handler
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = nullptr;
}

bool Start(uintptr_t address, int len, const std::string& mode, int seconds, const std::string& label) {
    if (g_busy.exchange(true)) return false;
    if (!g_cancel) g_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(g_cancel);
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Handler);
    g_address = address;
    g_execute = mode == "x";
    g_deref = len;
    const DWORD rw = g_execute ? 0 : (mode == "w" ? 1 : 3);
    const DWORD l = g_execute ? 0 : (len == 1 ? 0 : (len == 2 ? 1 : 3));
    const DWORD dr7 = 1u | (rw << 16) | (l << 18);
    if (HANDLE t = CreateThread(nullptr, 0, Worker, new Job{ seconds, dr7, label }, 0, nullptr)) {
        CloseHandle(t);
        return true;
    }
    g_busy = false;
    return false;
}
}
