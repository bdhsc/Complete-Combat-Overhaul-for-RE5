#include "common/arc.hpp"

#include <windows.h>

#include <cstring>
#include <fstream>

namespace arc {
namespace {
// Deflate (RFC 1951) decoding, after Mark Adler's puff: canonical Huffman codes decoded bit by bit.
struct Bits {
    const uint8_t* in;
    size_t size;
    size_t pos = 0;
    uint32_t buffer = 0;
    int count = 0;
    bool error = false;

    int Get(int need) {
        uint32_t value = buffer;
        while (count < need) {
            if (pos >= size) {
                error = true;
                return 0;
            }
            value |= static_cast<uint32_t>(in[pos++]) << count;
            count += 8;
        }
        buffer = value >> need;
        count -= need;
        return static_cast<int>(value & ((1u << need) - 1));
    }
};

struct Huffman {
    short count[16] = {};
    short symbol[288] = {};
};

// The code of `lengths` (n symbols); false when over-subscribed.
bool Build(Huffman& h, const short* lengths, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    for (int s = 0; s < n; ++s) ++h.count[lengths[s]];
    if (h.count[0] == n) return true;  // no codes: allowed (a distance code of one stored block)
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;
    }
    short offsets[16] = {};
    for (int len = 1; len < 15; ++len) offsets[len + 1] = static_cast<short>(offsets[len] + h.count[len]);
    for (int s = 0; s < n; ++s)
        if (lengths[s]) h.symbol[offsets[lengths[s]]++] = static_cast<short>(s);
    return true;
}

int Decode(Bits& bits, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= bits.Get(1);
        if (bits.error) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

constexpr short kLengthBase[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
constexpr short kLengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
constexpr short kDistanceBase[30] = { 1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
constexpr short kDistanceExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

bool Codes(Bits& bits, std::vector<uint8_t>& out, const Huffman& lengths, const Huffman& distances) {
    for (;;) {
        int symbol = Decode(bits, lengths);
        if (symbol < 0) return false;
        if (symbol < 256) {
            out.push_back(static_cast<uint8_t>(symbol));
            continue;
        }
        if (symbol == 256) return true;
        symbol -= 257;
        if (symbol >= 29) return false;
        const int length = kLengthBase[symbol] + bits.Get(kLengthExtra[symbol]);
        const int d = Decode(bits, distances);
        if (d < 0 || d >= 30) return false;
        const size_t distance = static_cast<size_t>(kDistanceBase[d] + bits.Get(kDistanceExtra[d]));
        if (bits.error || distance > out.size()) return false;
        for (int k = 0; k < length; ++k) out.push_back(out[out.size() - distance]);
    }
}

bool Stored(Bits& bits, std::vector<uint8_t>& out) {
    bits.buffer = 0;
    bits.count = 0;
    if (bits.pos + 4 > bits.size) return false;
    const size_t length = bits.in[bits.pos] | (bits.in[bits.pos + 1] << 8);
    const size_t check = bits.in[bits.pos + 2] | (bits.in[bits.pos + 3] << 8);
    bits.pos += 4;
    if (length != (~check & 0xFFFF) || bits.pos + length > bits.size) return false;
    out.insert(out.end(), bits.in + bits.pos, bits.in + bits.pos + length);
    bits.pos += length;
    return true;
}

bool Fixed(Bits& bits, std::vector<uint8_t>& out) {
    static Huffman lengths, distances;
    static bool built = false;
    if (!built) {
        short l[288];
        int s = 0;
        for (; s < 144; ++s) l[s] = 8;
        for (; s < 256; ++s) l[s] = 9;
        for (; s < 280; ++s) l[s] = 7;
        for (; s < 288; ++s) l[s] = 8;
        Build(lengths, l, 288);
        short d[30];
        for (short& v : d) v = 5;
        Build(distances, d, 30);
        built = true;
    }
    return Codes(bits, out, lengths, distances);
}

bool Dynamic(Bits& bits, std::vector<uint8_t>& out) {
    static constexpr short kOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    const int nlen = bits.Get(5) + 257, ndist = bits.Get(5) + 1, ncode = bits.Get(4) + 4;
    if (bits.error || nlen > 286 || ndist > 30) return false;
    short lengths[320] = {};
    for (int k = 0; k < ncode; ++k) lengths[kOrder[k]] = static_cast<short>(bits.Get(3));
    Huffman code;
    if (bits.error || !Build(code, lengths, 19)) return false;
    int index = 0;
    while (index < nlen + ndist) {
        int symbol = Decode(bits, code);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[index++] = static_cast<short>(symbol);
            continue;
        }
        short value = 0;
        int repeat = 0;
        if (symbol == 16) {
            if (index == 0) return false;
            value = lengths[index - 1];
            repeat = 3 + bits.Get(2);
        } else if (symbol == 17) {
            repeat = 3 + bits.Get(3);
        } else {
            repeat = 11 + bits.Get(7);
        }
        if (bits.error || index + repeat > nlen + ndist) return false;
        while (repeat--) lengths[index++] = value;
    }
    if (lengths[256] == 0) return false;
    Huffman literal, distance;
    if (!Build(literal, lengths, nlen) || !Build(distance, lengths + nlen, ndist)) return false;
    return Codes(bits, out, literal, distance);
}
}  // namespace

std::wstring GameDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? dir : dir.substr(0, slash);
}

bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (size < 2 || (data[0] & 0x0F) != 8 || ((data[0] << 8) | data[1]) % 31 != 0) return false;  // zlib, deflate
    Bits bits{ data + 2, size - 2 };
    for (;;) {
        const int last = bits.Get(1);
        const int type = bits.Get(2);
        if (bits.error) return false;
        const bool ok = type == 0 ? Stored(bits, out) : type == 1 ? Fixed(bits, out) : type == 2 && Dynamic(bits, out);
        if (!ok) return false;
        if (last) return true;
    }
}

bool ReadEntry(const std::wstring& archive, const char* entry, std::vector<uint8_t>& out) {
    std::ifstream file(archive, std::ios::binary);
    if (!file) return false;
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp(data.data(), "ARC\0", 4) != 0) return false;
    uint16_t version = 0, count = 0;
    std::memcpy(&version, &data[4], 2);
    std::memcpy(&count, &data[6], 2);
    uint32_t pad = 1;
    std::memcpy(&pad, &data[8], 4);
    const size_t base = 8 + (version >= 7 && pad == 0 ? 4 : 0);
    for (size_t i = 0; i < count; ++i) {
        const size_t e = base + i * 0x50;
        if (e + 0x50 > data.size()) return false;
        char name[65] = {};
        std::memcpy(name, &data[e], 64);
        if (_stricmp(name, entry) != 0) continue;
        uint32_t compressed = 0, size = 0, offset = 0;
        std::memcpy(&compressed, &data[e + 68], 4);
        std::memcpy(&size, &data[e + 72], 4);
        std::memcpy(&offset, &data[e + 76], 4);
        size &= 0x00FFFFFF;
        if (offset > data.size() || compressed > data.size() - offset) return false;
        out.clear();
        if (compressed == size) {
            out.assign(data.begin() + offset, data.begin() + offset + compressed);
            return true;
        }
        out.reserve(size);
        return Inflate(&data[offset], compressed, out) && out.size() == size;
    }
    return false;
}
}  // namespace arc
