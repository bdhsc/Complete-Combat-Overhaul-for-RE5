#pragma once

struct IDirect3DDevice9;

namespace overlay {
void OnPresent(IDirect3DDevice9* device);       // once per frame: input, features, menu
void OnReset(IDirect3DDevice9* device, bool before);
bool IsOpen();
bool BlocksGameInput();
void RequestShutdown();  // any thread; the render thread performs it on the next Present
bool ShutdownDone();
}
