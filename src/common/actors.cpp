#include "common/actors.hpp"
#include "common/game.hpp"
#include "common/mem.hpp"

#include <algorithm>

namespace actors {
namespace {
constexpr uintptr_t kEnemyArray = 0x650;  // manager field: pointer to an array of enemy pointers
constexpr int kMaxEnemies = 150;          // the Ultimate Trainer's "extended" limit

bool LooksLikeActor(uintptr_t p) {
    if (p < 0x10000) return false;
    uintptr_t vtable = 0;
    if (!mem::Read(p, vtable)) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem::Base());
    return vtable >= base && vtable < base + 0x01000000;  // vtables live in the game image
}
}

uintptr_t Manager() {
    uintptr_t manager = 0;
    mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(game::rva::PlayerManager)), manager);
    return manager;
}

uintptr_t Slot(int slot) {
    uintptr_t manager = Manager(), player = 0;
    if (!manager || slot < 0 || slot > 3 || !mem::Read(manager + 0x24 + 4 * slot, player)) return 0;
    return LooksLikeActor(player) ? player : 0;
}

int SlotOf(uintptr_t actor) {
    if (!actor) return -1;
    for (int i = 0; i < 4; ++i)
        if (Slot(i) == actor) return i;
    return -1;
}

namespace {
// The game's slot of player 1: 1 when slot 0 is the AI partner and slot 1 the local player (user, 2026-10-25: Jill,
// the second character of Desperate Escape, was hidden by "Joueur 2 invisible" while she was player 1). A player's
// +0x2DA8 is who controls it: 0 the local player, 1 the AI partner (the prompt detector's AI path, RVA 0x366F9D; the
// model swap fix at RVA 0x469287), 2 online.
int Player1Slot() {
    const uintptr_t first = Slot(0), second = Slot(1);
    uint32_t a = 0, b = 0;
    return first && second && mem::Read(first + 0x2DA8, a) && mem::Read(second + 0x2DA8, b) && a == 1 && b == 0 ? 1 : 0;
}
}

uintptr_t Player1PromptOffset() { return 0x2D0 * static_cast<uintptr_t>(Player1Slot()); }

uintptr_t Player(int index) {
    if (index == 0 || index == 1) index ^= Player1Slot();
    return Slot(index);
}

int PlayerIndex(uintptr_t actor) {
    const int slot = SlotOf(actor);
    return slot == 0 || slot == 1 ? slot ^ Player1Slot() : slot;
}

bool SlotIs(int slot, int index) {
    const uintptr_t p = Player(index);
    return p && Slot(slot) == p;
}

std::vector<uintptr_t> Enemies() {
    std::vector<uintptr_t> out;
    uintptr_t manager = Manager(), array = 0;
    if (!manager || !mem::Read(manager + kEnemyArray, array) || !array) return out;
    for (int i = 0; i < kMaxEnemies; ++i) {
        uintptr_t e = 0;
        if (!mem::Read(array + 4 * i, e) || !e) break;  // list ends at the first empty slot
        if (!LooksLikeActor(e) || PlayerIndex(e) >= 0) continue;
        if (std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
    }
    return out;
}

bool InGame() { return game::IsSupported() && Player(0) != 0; }

bool OnFoot(uintptr_t player) {
    uint32_t context = 0;
    return player && mem::Read(player + game::off::PlayerVehicleContext, context) && context == 0x101;
}
}
