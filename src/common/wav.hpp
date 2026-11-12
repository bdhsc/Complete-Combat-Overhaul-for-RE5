#pragma once
// A short 16-bit PCM WAV of DataDir\sounds, played on its own waveOut device the way parry.cpp plays RE6's counter sound
// (PlaySound's own thread crashed the game in winmm after a trainer reload, 2026-10-21). Load opens the device and
// prepares the samples once, the volume applied to them; Play restarts the sound.
#include <windows.h>
#include <mmsystem.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wav {

class Sound {
public:
    bool Load(const wchar_t* file, float gain, std::string& error);  // `file`: a name in DataDir\sounds
    void Play();
    void Close();
    float Gain() const { return gain_; }

private:
    std::vector<uint8_t> pcm_;
    std::wstring file_;
    HWAVEOUT device_ = nullptr;
    WAVEHDR header_{};
    float gain_ = -1.0f;
};

}  // namespace wav
