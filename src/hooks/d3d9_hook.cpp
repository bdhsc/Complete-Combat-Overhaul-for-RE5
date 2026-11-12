#include "hooks/hooks.hpp"
#include "common/log.hpp"
#include "features/body_cut.hpp"
#include "ui/overlay.hpp"

#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace hooks {
namespace {
using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

PresentFn g_present = nullptr;
ResetFn g_reset = nullptr;

// The draw hooks for features/body_cut.cpp, made from the game's own device at its first Present (a HAL or pure
// device's methods may not be the throw-away NULLREF device's).
using SetIndicesFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DIndexBuffer9*);
using SetStreamFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
using SetDeclFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
using DrawIndexedFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
SetIndicesFn g_setIndices = nullptr;
SetStreamFn g_setStream = nullptr;
SetDeclFn g_setDecl = nullptr;
DrawIndexedFn g_drawIndexed = nullptr;
bool g_drawHooksTried = false;

HRESULT __stdcall HookSetIndices(IDirect3DDevice9* device, IDirect3DIndexBuffer9* ib) {
    const HRESULT hr = g_setIndices(device, ib);
    if (SUCCEEDED(hr)) bodycut::OnSetIndices(ib);
    return hr;
}

HRESULT __stdcall HookSetStream(IDirect3DDevice9* device, UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride) {
    const HRESULT hr = g_setStream(device, stream, vb, offset, stride);
    if (SUCCEEDED(hr)) bodycut::OnSetStream(stream, vb, offset, stride);
    return hr;
}

HRESULT __stdcall HookSetDecl(IDirect3DDevice9* device, IDirect3DVertexDeclaration9* decl) {
    const HRESULT hr = g_setDecl(device, decl);
    if (SUCCEEDED(hr)) bodycut::OnSetDecl(decl);
    return hr;
}

HRESULT __stdcall HookDrawIndexed(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT numVertices,
                                  UINT start, UINT count) {
    if (bodycut::OnDraw(device, type, base, minIndex, numVertices, start, count, g_drawIndexed)) return D3D_OK;
    return g_drawIndexed(device, type, base, minIndex, numVertices, start, count);
}

void EnsureDrawHooks(IDirect3DDevice9* device) {
    if (g_drawHooksTried) return;
    g_drawHooksTried = true;
    void** vtable = *reinterpret_cast<void***>(device);
    // IDirect3DDevice9: 82 DrawIndexedPrimitive, 87 SetVertexDeclaration, 100 SetStreamSource, 104 SetIndices
    struct Site {
        int slot;
        void* detour;
        void** original;
    } sites[] = { { 104, reinterpret_cast<void*>(&HookSetIndices), reinterpret_cast<void**>(&g_setIndices) },
                  { 100, reinterpret_cast<void*>(&HookSetStream), reinterpret_cast<void**>(&g_setStream) },
                  { 87, reinterpret_cast<void*>(&HookSetDecl), reinterpret_cast<void**>(&g_setDecl) },
                  { 82, reinterpret_cast<void*>(&HookDrawIndexed), reinterpret_cast<void**>(&g_drawIndexed) } };
    for (const Site& s : sites) {
        if (MH_CreateHook(vtable[s.slot], s.detour, s.original) != MH_OK || MH_EnableHook(vtable[s.slot]) != MH_OK) {
            LOG("d3d9 draw hooks: slot %d at %p failed", s.slot, vtable[s.slot]);
            return;
        }
    }
    bodycut::HooksReady();
    LOG("d3d9 draw hooks installed (DrawIndexedPrimitive %p)", vtable[82]);
}

// Bridge "shot <label>": the next presented picture (before the overlay) written to DataDir\shot_<label>.bmp, for an
// agent to see the game (research, 2026-11-03).
std::mutex g_shotLock;
std::string g_shotLabel;
int g_shotStep = 1;

void WriteBmp(const std::wstring& path, const uint8_t* bits, int pitch, int fullWidth, int fullHeight, int step) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    const int width = fullWidth / step, height = fullHeight / step;
    const int row = (width * 3 + 3) & ~3;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + row * height;
    ih.biSize = sizeof(ih);
    ih.biWidth = width;
    ih.biHeight = height;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> line(row, 0);
    for (int y = height - 1; y >= 0; --y) {
        const uint8_t* px = bits + y * step * pitch;
        for (int x = 0; x < width; ++x) {
            line[x * 3] = px[x * step * 4];
            line[x * 3 + 1] = px[x * step * 4 + 1];
            line[x * 3 + 2] = px[x * step * 4 + 2];
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
}

void TakeShot(IDirect3DDevice9* device, const std::string& label, int step) {
    IDirect3DSurface9* back = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    D3DSURFACE_DESC desc{};
    back->GetDesc(&desc);
    IDirect3DSurface9* source = back;
    IDirect3DSurface9* resolved = nullptr;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE &&
        SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) &&
        SUCCEEDED(device->StretchRect(back, nullptr, resolved, nullptr, D3DTEXF_NONE)))
        source = resolved;
    IDirect3DSurface9* copy = nullptr;
    if ((desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8) &&
        SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr)) &&
        SUCCEEDED(device->GetRenderTargetData(source, copy))) {
        D3DLOCKED_RECT rect{};
        if (SUCCEEDED(copy->LockRect(&rect, nullptr, D3DLOCK_READONLY))) {
            WriteBmp(logx::DataDir() + L"\\shot_" + std::wstring(label.begin(), label.end()) + L".bmp",
                     static_cast<const uint8_t*>(rect.pBits), rect.Pitch, static_cast<int>(desc.Width), static_cast<int>(desc.Height),
                     step);
            copy->UnlockRect();
            LOG("shot %s: %ux%u", label.c_str(), desc.Width, desc.Height);
        }
    } else {
        LOG("shot %s failed (format %d, multisample %d)", label.c_str(), static_cast<int>(desc.Format), static_cast<int>(desc.MultiSampleType));
    }
    if (copy) copy->Release();
    if (resolved) resolved->Release();
    back->Release();
}

// The fast burst (research, 2026-11-04: "the hands tremble in the first person", full-speed pictures): the region of
// each presented picture copied by the GPU into its own render target (StretchRect, no wait), all read back and written
// once the last one is kept.
std::mutex g_fastLock;
RECT g_fastRect{};
int g_fastCount = 0;   // pictures asked (0: none)
int g_fastIndex = -1;  // the next one kept
bool g_fastWhole = false;  // the whole back buffer (its rect taken at the first picture)
int g_fastShrink = 1;
int g_fastStride = 1, g_fastSkip = 0;  // one picture kept in g_fastStride
std::vector<IDirect3DSurface9*> g_fastSurfaces;

void ReleaseFastBurst() {
    for (IDirect3DSurface9* s : g_fastSurfaces)
        if (s) s->Release();
    g_fastSurfaces.clear();
    g_fastCount = 0;
    g_fastIndex = -1;
    g_fastWhole = false;
    g_fastShrink = 1;
    g_fastStride = 1;
    g_fastSkip = 0;
}

void WriteFastBurst(IDirect3DDevice9* device) {
    for (size_t i = 0; i < g_fastSurfaces.size(); ++i) {
        IDirect3DSurface9* target = g_fastSurfaces[i];
        if (!target) continue;
        D3DSURFACE_DESC desc{};
        target->GetDesc(&desc);
        IDirect3DSurface9* copy = nullptr;
        if (SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr)) &&
            SUCCEEDED(device->GetRenderTargetData(target, copy))) {
            D3DLOCKED_RECT rect{};
            if (SUCCEEDED(copy->LockRect(&rect, nullptr, D3DLOCK_READONLY))) {
                wchar_t name[64];
                swprintf_s(name, L"\\fburst_%02zu.bmp", i);
                WriteBmp(logx::DataDir() + name, static_cast<const uint8_t*>(rect.pBits), rect.Pitch, static_cast<int>(desc.Width),
                         static_cast<int>(desc.Height), 1);
                copy->UnlockRect();
            }
        }
        if (copy) copy->Release();
    }
    LOG("fast burst: %zu pictures written", g_fastSurfaces.size());
}

void KeepFastBurst(IDirect3DDevice9* device) {
    std::lock_guard lock(g_fastLock);
    if (g_fastCount <= 0 || g_fastIndex < 0) return;
    if (g_fastSkip++ % g_fastStride != 0) return;
    IDirect3DSurface9* back = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    D3DSURFACE_DESC desc{};
    back->GetDesc(&desc);
    if (g_fastSurfaces.empty()) {
        if (g_fastWhole) g_fastRect = { 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
        g_fastSurfaces.assign(g_fastCount, nullptr);
        for (int i = 0; i < g_fastCount; ++i)
            if (FAILED(device->CreateRenderTarget((g_fastRect.right - g_fastRect.left) / g_fastShrink,
                                                  (g_fastRect.bottom - g_fastRect.top) / g_fastShrink, desc.Format,
                                                  D3DMULTISAMPLE_NONE, 0, FALSE, &g_fastSurfaces[i], nullptr))) {
                LOG("fast burst: render target %d not made", i);
                back->Release();
                ReleaseFastBurst();
                return;
            }
    }
    if (desc.MultiSampleType == D3DMULTISAMPLE_NONE)
        device->StretchRect(back, &g_fastRect, g_fastSurfaces[g_fastIndex], nullptr, g_fastShrink > 1 ? D3DTEXF_LINEAR : D3DTEXF_NONE);
    back->Release();
    if (++g_fastIndex >= g_fastCount) {
        WriteFastBurst(device);
        ReleaseFastBurst();
    }
}

HRESULT __stdcall HookPresent(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
    KeepFastBurst(device);
    std::string shot;
    int step = 1;
    {
        std::lock_guard lock(g_shotLock);
        shot.swap(g_shotLabel);
        step = g_shotStep;
    }
    // "shot +<label>": after the trainer's overlay (its HUD drawings seen; 2026-11-05).
    const bool afterOverlay = !shot.empty() && shot[0] == '+';
    if (!shot.empty() && !afterOverlay) TakeShot(device, shot, step);
    EnsureDrawHooks(device);
    overlay::OnPresent(device);
    if (afterOverlay) TakeShot(device, shot.substr(1), step);
    return g_present(device, src, dst, wnd, dirty);
}

HRESULT __stdcall HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    {
        std::lock_guard lock(g_fastLock);
        ReleaseFastBurst();  // default pool targets go before a Reset
    }
    overlay::OnReset(device, true);
    HRESULT hr = g_reset(device, params);
    LOG("device %p Reset -> 0x%08lX", device, hr);
    if (SUCCEEDED(hr)) overlay::OnReset(device, false);
    return hr;
}

// Reads the shared IDirect3DDevice9 vtable from a throw-away NULLREF device.
bool GetDeviceVtable(void** out, size_t count) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RE5TrainerDummyD3D";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    bool ok = false;
    if (IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION)) {
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = wnd;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        IDirect3DDevice9* dev = nullptr;
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, wnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp, &dev);
        if (SUCCEEDED(hr) && dev) {
            memcpy(out, *reinterpret_cast<void***>(dev), count * sizeof(void*));
            dev->Release();
            ok = true;
        } else {
            LOG("dummy D3D9 device failed: 0x%08lX", hr);
        }
        d3d->Release();
    }
    DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}
}

bool RequestFastBurst(int x, int y, int w, int h, int count) {
    std::lock_guard lock(g_fastLock);
    if (g_fastCount > 0 || w <= 0 || h <= 0 || count <= 0 || count > 120) return false;
    g_fastRect = { x, y, x + w, y + h };
    g_fastCount = count;
    g_fastIndex = 0;
    return true;
}

bool RequestFastBurstWhole(int count, int shrink, int stride) {
    std::lock_guard lock(g_fastLock);
    if (g_fastCount > 0 || count <= 0 || count > 120 || shrink < 1) return false;
    g_fastWhole = true;
    g_fastShrink = shrink;
    g_fastStride = stride < 1 ? 1 : stride;
    g_fastSkip = 0;
    g_fastCount = count;
    g_fastIndex = 0;
    return true;
}

int FastBurstFrame() {
    std::lock_guard lock(g_fastLock);
    return g_fastCount > 0 ? g_fastIndex : -1;
}

void RequestShot(const std::string& label, int step) {
    std::lock_guard lock(g_shotLock);
    g_shotLabel = label;
    g_shotStep = step < 1 ? 1 : step;
}

bool InstallD3D9() {
    void* vtable[119]{};
    if (!GetDeviceVtable(vtable, 119)) return false;
    // IDirect3DDevice9: 16 = Reset, 17 = Present
    if (MH_CreateHook(vtable[16], &HookReset, reinterpret_cast<void**>(&g_reset)) != MH_OK ||
        MH_CreateHook(vtable[17], &HookPresent, reinterpret_cast<void**>(&g_present)) != MH_OK) {
        LOG("MH_CreateHook d3d9 failed");
        return false;
    }
    if (MH_EnableHook(vtable[16]) != MH_OK || MH_EnableHook(vtable[17]) != MH_OK) {
        LOG("MH_EnableHook d3d9 failed");
        return false;
    }
    LOG("d3d9 hooks installed (Reset %p, Present %p)", vtable[16], vtable[17]);
    return true;
}
}
