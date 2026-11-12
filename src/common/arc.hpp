#pragma once
// The game's own data read by the trainer (user, 2026-11-06: "the mod will be shared: how will people recreate the left
// flame?"): what the Python tools of tools/ made from the player's files is made by the trainer itself, from the
// player's own game. MT Framework archives (nativePC_MT\Image\Archive\*.arc): "ARC\0", u16 version, u16 count, then
// entries of 0x50 bytes after 8 bytes (12 from version 7 when the dword at 8 is 0): name[64], type, compressed size,
// size (low 24 bits), offset; an entry whose compressed size differs from its size is a zlib stream.
#include <cstdint>
#include <string>
#include <vector>

namespace arc {
// The game's folder (re5dx9.exe's).
std::wstring GameDir();
// One entry of an archive, decompressed (entry names as tools/arc_list.py prints them, case ignored).
bool ReadEntry(const std::wstring& archive, const char* entry, std::vector<uint8_t>& out);
// A zlib stream (2-byte header, deflate data; the checksum is not checked) inflated into `out`.
bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out);
}  // namespace arc
