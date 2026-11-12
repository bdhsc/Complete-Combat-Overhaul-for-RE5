#include "common/wav.hpp"

#include "common/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace wav {

void Sound::Close() {
    if (!device_) return;
    waveOutReset(device_);
    if (header_.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(device_, &header_, sizeof(header_));
    waveOutClose(device_);
    device_ = nullptr;
    header_ = {};
}

bool Sound::Load(const wchar_t* file, float gain, std::string& error) {
    if (device_ && gain == gain_ && file_ == file) return true;
    std::ifstream f(logx::DataDir() + L"\\sounds\\" + file, std::ios::binary);
    if (!f) {
        std::string name;  // an ASCII name
        for (const wchar_t* c = file; *c; ++c) name += static_cast<char>(*c);
        error = "fichier sounds\\" + name + " absent";
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
        error = "pas un fichier WAV";
        return false;
    }
    WAVEFORMATEX format{};
    std::vector<uint8_t> pcm;
    for (size_t pos = 12; pos + 8 <= data.size();) {
        uint32_t size = 0;
        std::memcpy(&size, data.data() + pos + 4, 4);
        if (std::memcmp(data.data() + pos, "fmt ", 4) == 0 && size >= 16) {
            std::memcpy(&format, data.data() + pos + 8, 16);
            // ffmpeg writes WAVE_FORMAT_EXTENSIBLE (0xFFFE, 40-byte fmt) with the PCM sub-format: plain PCM for waveOut.
            if (format.wFormatTag == 0xFFFE && size >= 40 && data[pos + 8 + 24] == 1 && data[pos + 8 + 25] == 0)
                format.wFormatTag = WAVE_FORMAT_PCM;
        } else if (std::memcmp(data.data() + pos, "data", 4) == 0) {
            const size_t end = std::min(data.size(), pos + 8 + static_cast<size_t>(size));
            pcm.assign(data.begin() + static_cast<ptrdiff_t>(pos + 8), data.begin() + static_cast<ptrdiff_t>(end));
        }
        pos += 8 + size + (size & 1);
    }
    if (format.wFormatTag != WAVE_FORMAT_PCM || format.wBitsPerSample != 16 || pcm.empty()) {
        error = "il faut du PCM 16 bits";
        return false;
    }
    // The volume, then a peak limiter (instant attack, 60 ms release): above 100 % the sound gets louder without
    // clipping (user, 2026-10-28: "200 % c'est pas assez" - the files already peak at -1 to -4 dBFS).
    const size_t channels = std::max<size_t>(1, format.nChannels);
    const float release = std::exp(-1.0f / (0.06f * static_cast<float>(std::max<DWORD>(1, format.nSamplesPerSec))));
    float envelope = 0.0f;
    for (size_t frame = 0; (frame + 1) * channels * 2 <= pcm.size(); ++frame) {
        int16_t samples[8] = {};
        const size_t count = std::min<size_t>(channels, 8), at = frame * channels * 2;
        std::memcpy(samples, pcm.data() + at, count * 2);
        float peak = 0.0f;
        for (size_t c = 0; c < count; ++c) peak = std::max(peak, std::fabs(samples[c] * gain) / 32767.0f);
        envelope = std::max(peak, envelope * release);
        const float reduce = envelope > 1.0f ? 1.0f / envelope : 1.0f;
        for (size_t c = 0; c < count; ++c)
            samples[c] = static_cast<int16_t>(std::clamp(samples[c] * gain * reduce, -32768.0f, 32767.0f));
        std::memcpy(pcm.data() + at, samples, count * 2);
    }
    Close();  // nothing reads the old samples any more
    format.cbSize = 0;
    if (waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        device_ = nullptr;
        error = "pas de sortie audio";
        return false;
    }
    pcm_ = std::move(pcm);
    header_ = {};
    header_.lpData = reinterpret_cast<LPSTR>(pcm_.data());
    header_.dwBufferLength = static_cast<DWORD>(pcm_.size());
    waveOutPrepareHeader(device_, &header_, sizeof(header_));
    gain_ = gain;
    file_ = file;
    return true;
}

void Sound::Play() {
    if (!device_) return;
    waveOutReset(device_);  // played again during the previous sound: restarted
    waveOutWrite(device_, &header_, sizeof(header_));
}

}  // namespace wav
