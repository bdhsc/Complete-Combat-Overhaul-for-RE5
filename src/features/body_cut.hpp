#pragma once
#include <d3d9.h>

#include <cstdint>

// Player 1's body cut triangle by triangle in the first person (camera.cpp's "Première personne : corps visible"):
// the device calls that draw his model get a copy of their index list without the triangles skinned to hidden joints.
// No joint is shrunk: nothing stretches between a hidden part and a kept one, the cut follows the triangles' edges.
namespace bodycut {
using DrawFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

// Render thread, from the device hooks (hooks/d3d9_hook.cpp).
void HooksReady();
void OnSetIndices(IDirect3DIndexBuffer9* ib);
void OnSetStream(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride);
void OnSetDecl(IDirect3DVertexDeclaration9* decl);
// true: the draw was made here (or dropped); false: the game's own call goes on.
bool OnDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT numVertices, UINT start,
            UINT count, DrawFn draw);

// Game thread. Ready: the device hooks are in. Set: player 1's object and what stays visible (mode as camera.cpp's
// g_bodyMode: 0 all but the neck and the head, 1 the arms, 2 the forearms, 3 the hands; -1 nothing cut).
bool Ready();
void Set(uintptr_t player, int mode);

void RegisterFeatures();
}
