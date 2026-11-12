#pragma once
#include <cstdint>
#include <string>

namespace hooks {
bool InstallD3D9();    // IDirect3DDevice9::Present / Reset
void RequestShot(const std::string& label, int step = 1);  // the next presented picture to DataDir\shot_<label>.bmp
                                                         // (bridge "shot"; step 2 = one pixel in 2 each way)
// Research: the next `count` presented pictures' region (x, y, w, h of the back buffer) kept in video memory at full
// speed (a GPU copy each), written at the end to DataDir\fburst_NN.bmp. False while one runs.
bool RequestFastBurst(int x, int y, int w, int h, int count);
int FastBurstFrame();  // the picture being kept now (0..count-1), -1 when none runs
// The same for the whole picture, each one `shrink` times smaller each way (a flip seen in the first person, 2026-11-10).
bool RequestFastBurstWhole(int count, int shrink, int stride = 1);  // stride: one picture kept in `stride`
bool InstallDInput();  // IDirectInputDevice8::GetDeviceState / GetDeviceData (input blocking while the menu is open)
unsigned long long LastDInputMouseRead();  // GetTickCount64 of the game's last DirectInput mouse read (0 = never)
long TakeRawMouseX();  // horizontal raw mouse counts the game received since the last call (menu closed)
void FreezeLook(bool on);
void HidePadRightX(bool on);  // hide the pad's right stick horizontal axis from the game (menu closed)
void HidePadRightXForAim(bool on);  // the same for the pad's mouse-like aim (camera.cpp / movement.cpp), its own source
void HidePadLeftForAim(bool on);    // the left stick hidden from the game in that aim (PadLeftStick still reads it)
float PadRightX();            // its real value, -1 (left) .. 1 (right), dead zone removed
float PadRightY();            // its real value, -1 (down) .. 1 (up), dead zone removed
// The left stick seen by the game pushed straight forward with its real tilt (menu closed), and its real position
// (-1..1, right / up positive, no dead zone removed).
void PadLeftForward(bool on);
void PadLeftStick(float& x, float& y);
// Pad buttons hidden from the game (XINPUT_GAMEPAD bits, 0x10000 LT, 0x20000 RT); source 0 = the shortcuts held with
// their key to hold (source 1 free). The trainer's shortcuts still read them.
void HidePadButtons(int source, uint32_t mask);
uint32_t PadHiddenButtonsHeld();  // source 2's buttons the player really holds while the game does not see them
void InjectPadButtons(uint32_t mask);  // pad buttons the game sees held (XINPUT_GAMEPAD bits, 0x10000 LT, 0x20000 RT)
uint32_t PadInjectedButtons();
void SwapPadAX(bool on);  // the game sees the pad's A as X and X as A  // hide mouse movement and the pad's right stick from the game (menu closed)
}
