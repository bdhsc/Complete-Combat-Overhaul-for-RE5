#pragma once

namespace features {
// Player 1 is in the dying state (1 HP and dying time left).
bool Player1Dying();
// "Se débattre en étant mourant": grab struggles stay possible while dying.
bool DyingStruggleAllowed();
// "Sauvé si une prise te met mourant": a grab took player 1 from health to dying, until he is free again.
bool GrabRescueActive();
// "Mourant façon RE6": player 1 is dying and the next hit kills (checked before the damage is applied); also a bite
// while he is held with "Saisies mortelles en étant mourant" (licker.cpp: FatalGrabHolds).
bool Re6DyingHitKills();
// licker.cpp: "Saisies mortelles en étant mourant" and player 1 dying in a hold.
bool FatalGrabHolds();
// licker.cpp: struggle hooks (won / easier), and the licker pounce released for player 1.
bool EnsureGrabStruggleHooks();
bool FreePlayer1FromLicker();
}  // namespace features
