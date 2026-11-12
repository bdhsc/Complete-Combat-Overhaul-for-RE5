#pragma once
#include <cstdint>
#include <vector>

// Live actors of re5dx9.exe 1.0.0.129. Players and enemies share the same object layout for the
// fields below (observed with UTSpy, see MODLOG.md).
namespace actors {

namespace off {
constexpr uintptr_t Position = 0x30;     // float x, y, z
constexpr uintptr_t Scale = 0x50;        // float x, y, z
constexpr uintptr_t Speed = 0x3A4;       // float, 1.0 = normal
constexpr uintptr_t Health = 0x1364;     // int16
constexpr uintptr_t HealthMax = 0x1366;  // int16
}

uintptr_t Manager();               // [re5dx9+0xDB27DC]
// Player 1 = the one at the keyboard / pad, player 2 = the partner (the game's slots follow the character choice:
// Sheva, or Jill in Desperate Escape, may be player 1 in slot 1). 0..3, 0 if absent.
uintptr_t Player(int index);
int PlayerIndex(uintptr_t actor);  // -1 if not a player
// The game's own player slots ([manager]+0x24 + 4 * slot), the index the game's functions take or pass.
uintptr_t Slot(int slot);
int SlotOf(uintptr_t actor);         // -1 if not a player
bool SlotIs(int slot, int index);    // the game's slot `slot` holds Player(index)
// sActionButton ([RVA 0xDB1794]) keeps one prompt manager per game slot at +0x40 + 0x2D0 * slot (shown slots +0x230 /
// +0x278): player 1's is the one of his game slot (user, 2026-11-04: playing Sheva, the co-op prompts kept F - every
// prompt feature read slot 0's, the AI Chris's). The offset to add to +0x40.
uintptr_t Player1PromptOffset();
std::vector<uintptr_t> Enemies();  // [[manager]+0x650][i], validated, players excluded, no duplicates
bool InGame();
// Not aboard a vehicle: P+0x364 is 0x101 on foot, 1 in the boat (3-1) and the truck (2-3); false when unreadable.
bool OnFoot(uintptr_t player);
}
