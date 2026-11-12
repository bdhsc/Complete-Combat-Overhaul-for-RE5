// Options that are pure code patches. Sites, original and patched bytes were observed with UTSpy
// (research/ut_pass1.md); originals are re-checked at runtime before any write.
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <atomic>
#include <cstring>
#include <deque>
#include <vector>

namespace features {
namespace {

struct Site {
    uintptr_t rva;
    std::vector<uint8_t> original;
    std::vector<uint8_t> patched;
};

std::deque<mem::Patch> g_patches;  // stable addresses

void AddPatchToggle(const char* id, const char* name, const char* help, const char* category, std::vector<Site> sites) {
    std::vector<mem::Patch*> list;
    for (Site& s : sites) {
        g_patches.emplace_back(id, s.rva, std::move(s.original), std::move(s.patched));
        list.push_back(&g_patches.back());
    }
    Add({
        .id = id,
        .name = name,
        .help = help,
        .category = category,
        .kind = Kind::Toggle,
        .set = [list](bool on) {
            if (!on) {
                bool ok = true;
                for (auto it = list.rbegin(); it != list.rend(); ++it) ok = (*it)->Restore() && ok;
                return ok;
            }
            for (mem::Patch* p : list)
                if (!p->IsRecognised()) return false;
            std::vector<mem::Patch*> done;
            for (mem::Patch* p : list) {
                if (!p->Apply()) {
                    for (auto it = done.rbegin(); it != done.rend(); ++it) (*it)->Restore();
                    return false;
                }
                done.push_back(p);
            }
            return true;
        },
        .available = [] { return game::IsSupported(); },
    });
}
}

// The Ultimate Trainer "restores" the missing-file logger (RVA 0x2F9690) with bytes from another build:
// `cmp [0x1239D0C], 0` instead of `cmp [0x1249C8C], 0`. The logger then skips fopen and calls fprintf on a
// null FILE* as soon as any file is missing → crash (seen with an anim swap, 2026-10-12). Put the real bytes back.
void RepairUtDamage() {
    uint8_t* site = mem::Rva(0x2F9690);
    const uint8_t utBytes[] = { 0x83, 0x3D, 0x0C, 0x9D, 0x23, 0x01, 0x00 };
    const uint8_t real[] = { 0x83, 0x3D, 0x8C, 0x9C, 0x24, 0x01, 0x00 };
    if (mem::IsGameCode(site, sizeof(utBytes)) && std::memcmp(site, utBytes, sizeof(utBytes)) == 0 &&
        mem::WriteCode(site, real, sizeof(real)))
        LOG("repaired missing-file logger at %p (Ultimate Trainer had left wrong bytes)", site);
}

bool SetPadMouseAim(bool on);  // movement.cpp
void SetPadAimLikeMouse(bool on);  // camera.cpp
void SetMouseAimLikePad(bool on);  // camera.cpp

namespace {
// Laser or crosshair, per control type (user, 2026-10-20: "Laser au clavier/souris" forced the pad's laser on both, a
// pad crosshair forced the keyboard's on both and the two excluded each other). The game asks which control is used at
// three places: the crosshair function VA 0xB69970 returns at once on a pad (VA 0x799AF0 "pad in use", call at VA
// 0xB699A4; with a pad the control mode VA 0xCEE1B0 stays 0, recorded), and the laser branches call VA 0xB5C4F0
// (keyboard / mouse, false = laser) at VA 0xB76DBA and 0xB7714A. Those calls go to the functions below, which answer
// for the control really used: the keyboard gets the laser with "Laser au clavier/souris", the pad the crosshair with
// "Réticule à la manette"; both can be on.
std::atomic<bool> g_laserKeyboard{ false }, g_crosshairPad{ false };
// The RE6-style quick shot (quick_knife.cpp, user 2026-10-21): no laser and no crosshair during its shot, so the
// player cannot judge whether the bullet hit (its target ignores the bullet and takes the damage anyway).
std::atomic<bool> g_markersHidden{ false };
bool g_markersForShot = false;  // the calls stay redirected while the quick shot is on
using KeyboardFn = bool(__fastcall*)(void* player, void* edx);
using PadInUseFn = bool(__fastcall*)(void* pad, void* edx, int index);

bool __fastcall LaserSiteKeyboard(void* player, void* edx) {  // true = the keyboard path (no laser)
    if (g_markersHidden.load(std::memory_order_relaxed)) return true;
    const bool keyboard = reinterpret_cast<KeyboardFn>(mem::Rva(0x75C4F0))(player, edx);
    uint32_t remote = 0;
    if (!mem::Read(reinterpret_cast<uintptr_t>(player) + 0x2DA8, remote) || remote) return keyboard;
    return keyboard ? !g_laserKeyboard.load(std::memory_order_relaxed) : g_crosshairPad.load(std::memory_order_relaxed);
}

bool __fastcall CrosshairSitePad(void* pad, void* edx, int index) {  // true = no crosshair (the pad path)
    if (g_markersHidden.load(std::memory_order_relaxed)) return true;
    const bool padUsed = reinterpret_cast<PadInUseFn>(mem::Rva(0x399AF0))(pad, edx, index);
    return padUsed ? !g_crosshairPad.load(std::memory_order_relaxed) : g_laserKeyboard.load(std::memory_order_relaxed);
}

mem::Patch* CallPatch(const char* name, uintptr_t siteRva, uintptr_t targetRva, void* replacement) {
    uint8_t* site = mem::Rva(siteRva);
    auto rel = [site](uintptr_t to) {
        const int32_t r = static_cast<int32_t>(to - reinterpret_cast<uintptr_t>(site + 5));
        return std::vector<uint8_t>{ 0xE8, static_cast<uint8_t>(r), static_cast<uint8_t>(r >> 8), static_cast<uint8_t>(r >> 16),
                                     static_cast<uint8_t>(r >> 24) };
    };
    g_patches.emplace_back(name, site, rel(reinterpret_cast<uintptr_t>(mem::Rva(targetRva))),
                           rel(reinterpret_cast<uintptr_t>(replacement)));
    return &g_patches.back();
}

std::vector<mem::Patch*> g_markerCalls;

// The calls stay redirected while either option (or the quick shot) is on.
bool UpdateMarkerCalls() {
    const bool want = g_laserKeyboard || g_crosshairPad || g_markersForShot;
    bool ok = true;
    for (mem::Patch* c : g_markerCalls) ok = (want ? (c->IsApplied() || c->Apply()) : (!c->IsApplied() || c->Restore())) && ok;
    return ok;
}

void AddAimMarkers() {
    if (game::IsSupported()) {
        g_markerCalls = { CallPatch("aim markers", 0x7699A4, 0x399AF0, reinterpret_cast<void*>(&CrosshairSitePad)),
                          CallPatch("aim markers", 0x776DBA, 0x75C4F0, reinterpret_cast<void*>(&LaserSiteKeyboard)),
                          CallPatch("aim markers", 0x77714A, 0x75C4F0, reinterpret_cast<void*>(&LaserSiteKeyboard)) };
    }
    static auto update = [] { return UpdateMarkerCalls(); };
    Add({ .id = "display.laser_keyboard", .name = "Laser au clavier/souris",
          .help = "Au clavier/souris, affiche le laser de visée (normalement réservé à la manette) à la place du réticule.",
          .category = "Affichage", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_laserKeyboard = on;
              return update();
          },
          .available = [] { return game::IsSupported(); } });
    // The crosshair needs the mouse-like aim: with the pad's free zone it stayed centred while the weapon moved.
    Add({ .id = "display.crosshair_pad", .name = "Réticule à la manette",
          .help = "À la manette, affiche le réticule du clavier/souris à la place du laser (les armes qui ont leur propre "
                  "laser le gardent), avec la visée de la souris : la caméra tourne tout de suite et Chris la suit. Marche "
                  "avec « Laser au clavier/souris ».",
          .category = "Affichage", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_crosshairPad = on;
              const bool aim = SetPadMouseAim(on);
              return update() && (aim || !on);
          },
          .available = [] { return game::IsSupported(); } });
    // The rendering of each control's laser, swapped (the menu shows them with the markers' options).
    Add({ .id = "camera.mouse_aim_like_pad", .name = "Souris : rendu manette avec le laser",
          .help = "À la souris avec le laser (« Laser au clavier/souris »), la caméra de visée suit le corps de Chris "
                  "comme à la manette : le laser se déplace dans l'écran dans la zone libre avant que la caméra tourne. "
                  "Avec le réticule, la souris garde son rendu.",
          .category = "Affichage", .kind = Kind::Toggle,
          .set = [](bool on) {
              SetMouseAimLikePad(on);
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    Add({ .id = "camera.pad_aim_like_mouse", .name = "Manette : rendu souris avec le laser",
          .help = "À la manette avec le laser, la visée tourne comme à la souris : le stick droit tourne la visée et la "
                  "caméra tout de suite, Chris suit en restant dans la zone libre, l'arme vise le centre de l'écran. Avec "
                  "le réticule (« Réticule à la manette »), c'est toujours le cas. Vitesse : « Sensibilité de visée "
                  "(manette) ».",
          .category = "Affichage", .kind = Kind::Toggle,
          .set = [](bool on) {
              SetPadAimLikeMouse(on);
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    // The crosshair's colour byte [P+0x2640]+0x374 (0 plain, 1 green, 2 on a target), written at VA 0xB76D89 and VA 0xB76D54:
    // the tests that choose it skipped, it stays 0 (mouse and pad).
    AddPatchToggle("display.crosshair_no_color", "Réticule sans couleur",
                   "Le réticule garde sa couleur neutre : il ne passe plus au vert, ni au rouge sur une cible ou un objet "
                   "interactif, à la souris comme à la manette.",
                   "Affichage", { { 0x776D64, { 0x74, 0x1D }, { 0xEB, 0x1D } }, { 0x776C99, { 0x74, 0x23 }, { 0xEB, 0x23 } } });
}
}

bool UseAimMarkerCalls(bool on) {
    g_markersForShot = on;
    return UpdateMarkerCalls();
}

void HideAimMarkers(bool hide) { g_markersHidden = hide; }
// camera.cpp's aim rendering: which marker each control shows (the crosshair: always the mouse's rendering).
bool LaserOnKeyboard() { return g_laserKeyboard.load(std::memory_order_relaxed); }
bool CrosshairOnPad() { return g_crosshairPad.load(std::memory_order_relaxed); }

void RegisterPatchFeatures() {
    if (game::IsSupported()) RepairUtDamage();

    // ---- Affichage ----
    AddPatchToggle("display.crosshair_hide", "Masquer le viseur", "Retire le réticule (clavier/souris).", "Affichage",
                   { { 0x769A91, { 0x74 }, { 0xEB } } });
    AddAimMarkers();
    AddPatchToggle("display.hud_hide", "Masquer le HUD", "Cache l'interface de jeu.", "Affichage",
                   { { 0x5F8D19, { 0x01 }, { 0x00 } }, { 0x5F8EB0, { 0x74 }, { 0xEB } }, { 0x5F9125, { 0x01 }, { 0x00 } } });
    AddPatchToggle("display.map_hide", "Masquer la carte", "Cache la mini-carte (elle reste consultable).", "Affichage",
                   { { 0x61440D, { 0x01 }, { 0x00 } }, { 0x616010, { 0x00 }, { 0x01 } } });
    AddPatchToggle("display.inventory_hide", "Masquer l'inventaire", "Cache l'inventaire affiché (il reste utilisable).", "Affichage",
                   { { 0x86C75B, { 0x01 }, { 0x00 } } });
    AddPatchToggle("display.prompts_hide", "Masquer les boutons d'action", "Cache la plupart des indications de touches (hors cinématiques).",
                   "Affichage", { { 0x366A65, { 0x83, 0xEC, 0x18 }, { 0xC3, 0x90, 0x90 } } });
    // Colour filter. Each viewport has a uColorCorrectFilter ([[RVA 0xDB2384] + 0x74 / + 0x78], vtable RVA 0xCAFF08).
    // RVA 0x3C2F80 copies the area's colour correction into it EVERY frame: 16 correctors at +0x30 (0x30 each: +4
    // enabled, +8 type, +0x10 / +0x20 parameters) and colour matrices +0x330..+0x43F. The patch skips that copy, so
    // the last copied values stayed on screen until the next load (user, 2026-10-14). Switching on also writes the
    // values of a freshly built filter (read after a patched load: type 0, identity matrices), which removed the tint
    // at once (checked with screenshots); switching off lets the copy run again and the tint comes back next frame.
    {
        static mem::Patch skipCopy("display.color_filter_off", 0x3C2F93, { 0x0F, 0x84, 0x50, 0x04, 0x00, 0x00 },
                                   { 0xE9, 0x51, 0x04, 0x00, 0x00, 0x90 });
        Add({ .id = "display.color_filter_off", .name = "Supprimer le filtre de couleur",
              .help = "Retire le filtre vert/jaune de l'image, tout de suite. Décocher le remet.",
              .category = "Affichage", .kind = Kind::Toggle,
              .set = [](bool on) {
                  if (!on) return skipCopy.Restore();
                  if (!skipCopy.Apply()) return false;
                  uintptr_t set = 0;
                  if (!mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(0xDB2384)), set) || !set) return true;
                  for (int viewport = 0; viewport < 2; ++viewport) {
                      uintptr_t filter = 0, vtable = 0;
                      if (!mem::Read(set + 0x74 + 4 * viewport, filter) || !filter || !mem::Read(filter, vtable) ||
                          vtable != reinterpret_cast<uintptr_t>(mem::Rva(0xCAFF08)))
                          continue;
                      struct Corrector {
                          uint32_t enabled, type, unused;
                          float add[4], mul[4];
                      };
                      for (int k = 0; k < 16; ++k) {
                          const Corrector neutral{ k == 10 || k == 11 ? 0u : 1u, 0, 0, { 0.5f, 0.5f, 0.5f, 0 }, { 1, 1, 1, 0 } };
                          mem::SafeWrite(reinterpret_cast<void*>(filter + 0x30 + 0x30 * k + 4), &neutral, sizeof(neutral));
                      }
                      float matrices[0x110 / 4] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
                      for (int row = 0; row < 12; ++row)
                          for (int c = 0; c < 4; ++c) matrices[16 + row * 4 + c] = row % 2 == 0 && c == 0 ? 0.0f : 1.0f;
                      matrices[64] = matrices[65] = matrices[66] = 1;
                      matrices[67] = 0;
                      mem::SafeWrite(reinterpret_cast<void*>(filter + 0x330), matrices, sizeof(matrices));
                  }
                  return true;
              },
              .available = [] { return game::IsSupported(); } });
    }
    AddPatchToggle("display.hd_shadows", "Ombres HD", "Résolution des ombres x4.", "Affichage",
                   { { 0x23C899, { 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC }, { 0xB8, 0x00, 0x10, 0x00, 0x00, 0xEB, 0x05 } },
                     { 0x23C8A1, { 0x8B, 0x44, 0x24, 0x08 }, { 0xEB, 0xF6, 0x90, 0x90 } } });

    // ---- Jeu ----
    AddPatchToggle("game.skip_cutscenes", "Passer les cinématiques", "Saute la plupart des cinématiques.", "Jeu",
                   { { 0x3B1A54, { 0x75 }, { 0xEB } },
                     { 0x82FCC3, { 0x0F, 0x85, 0xF8, 0x01, 0x00, 0x00 }, { 0xE9, 0xF9, 0x01, 0x00, 0x00, 0x00 } } });
    AddPatchToggle("game.ignore_missing_files", "Ignorer les fichiers manquants",
                   "Nécessaire pour changer de personnage sans patch de fichiers.", "Jeu",
                   // UT restores 83 3D 0C 9D 23 01 00 here (another build): wrong for 1.0.0.129, whose original is below.
                   { { 0x2F9690, { 0x83, 0x3D, 0x8C, 0x9C, 0x24, 0x01, 0x00 }, { 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } } });

    // ---- Munitions ----
    AddPatchToggle("ammo.infinite", "Munitions infinies", "Le chargeur ne se vide jamais.", "Munitions",
                   { { 0x84C1E6, { 0x74 }, { 0xEB } } });
    AddPatchToggle("ammo.infinite_eggs", "Œufs infinis", "Les œufs ne sont pas consommés.", "Munitions",
                   { { 0x3E776B, { 0x89, 0x71, 0x08 }, { 0x90, 0x90, 0x90 } } });
    // Heat of mounted guns: the common weapon update (RVA 0x797280) adds heat to unit +0x1C68 (0..1) while +0x1C41
    // (firing) is set, overheats at 1 (+0x1C40) and cools down otherwise. Found on the truck turret (item 0x012D,
    // uWpGunEmpJeep) with a write trace (user, 2026-10-14). Always taking the cooling branch: no overheating.
    AddPatchToggle("ammo.turret_no_overheat", "Tourelles sans surchauffe",
                   "Les tourelles et mitrailleuses montées ne chauffent plus quand on tire.", "Munitions",
                   { { 0x7979FE, { 0x75, 0x77 }, { 0xEB, 0x77 } } });
    // Reload request = the player input byte +0x23 ([P+0x2D64] + 0x220, written from the pad masks, VA 0xB8B363..):
    // set by the reload key (game button 0x12), by the run button (0x04, trigger mask 0x4000) while aim is held (or in
    // the aim states of P+0x2D8C bits 8-11), and by fire with an empty magazine. Dropping the two run tests (jne to
    // the "+0x23 = 1" store) leaves the reload key and the empty-magazine reload (user, 2026-10-20).
    AddPatchToggle("ammo.no_run_reload", "Pas de rechargement avec la course",
                   "En visant, la touche de course ne recharge plus : seule la touche de rechargement (et un tir chargeur "
                   "vide) recharge.",
                   "Munitions", { { 0x78B36B, { 0x75, 0x74 }, { 0x90, 0x90 } }, { 0x78B392, { 0x75, 0x19 }, { 0x90, 0x90 } } });

    // ---- Combat ----
    // The Ultimate Trainer's "Not So Dying" (no death countdown 0x77CE8C, weapons at 1 HP 0x452C62 / 0x76A89B /
    // 0x76A8FB, can-act at 1 HP 0x76A82D) is gone (user, 2026-10-30): "Mourant façon RE6" keeps the weapons and
    // "Vitesse normale en étant mourant" the walk and run (dying.cpp).

    // ---- Personnage ----
    AddPatchToggle("char.tremors_fix", "Pas de tremblements des mains", "Supprime le tremblement en visée.", "Personnage",
                   { { 0x787227, { 0xF3, 0x0F, 0x10, 0x83, 0xAC, 0x00, 0x00, 0x00 }, { 0x0F, 0x57, 0xC0, 0xEB, 0x03, 0x90, 0x90, 0x90 } },
                     { 0x76E78D, { 0x8B, 0x90, 0x28, 0x01, 0x00, 0x00 }, { 0x31, 0xD2, 0xEB, 0x02, 0x90, 0x90 } },
                     { 0x76E80E, { 0x8B, 0xB0, 0x28, 0x01, 0x00, 0x00 }, { 0x31, 0xF6, 0xEB, 0x02, 0x90, 0x90 } } });
    // Disabled until 'Swap Character Anims' (Reunion set) exists: alone it breaks the melee prompt
    // (Chris does no uppercut at all; user test 2026-10-12). Sites kept for later:
    // AddPatchToggle("char.reunion_moves", "Mouvements Reunion partout",
    // "Débloque les mouvements de Mercenaires Reunion dans tous les modes.", "Mêlée",
    // { { 0x3E607C, { 0x74 }, { 0xEB } },
    // { 0x877E3D, { 0x74, 0x64 }, { 0xEB, 0x00 } },
    // { 0x877D40, { 0xBE, 0x01, 0x00, 0x00, 0x00 }, { 0xEB, 0x0A, 0x90, 0x90, 0x90 } },
    // { 0x877D56, { 0xBE, 0x02, 0x00, 0x00, 0x00 }, { 0xEB, 0x0A, 0x90, 0x90, 0x90 } } });
    // The holstered pistol is a variation of the character model, not the weapon unit: RVA 0x765F20 picks the model
    // state (0x84DBF0 -> P+0x1930) from P+0x1E1C (0 pistol in hand, 1 holstered) and P+0x1E18. Found by diffing
    // Chris with the pistol in hand / holstered (user, 2026-10-14). Reading P+0x1E1C as 0 keeps the holster empty.
    AddPatchToggle("char.hide_holstered_handgun", "Cacher le pistolet dans l'étui",
                   "L'étui reste vide quand le pistolet est rangé (option « Remove handgun » de l'Ultimate Trainer). "
                   "Prend effet au prochain changement d'arme.", "Personnage",
                   { { 0x765F8B, { 0x8B, 0x86, 0x1C, 0x1E, 0x00, 0x00 }, { 0x31, 0xC0, 0x90, 0x90, 0x90, 0x90 } } });
    AddPatchToggle("char.wesker_sunglasses", "Wesker garde ses lunettes", "Wesker ne perd jamais ses lunettes.", "Personnage",
                   { { 0x77BC91, { 0x75 }, { 0xEB } } });
    AddPatchToggle("char.weapon_on_back", "Cacher l'arme dans le dos",
                   "L'arme rangée dans le dos n'est plus affichée (option « Weapon on back » de l'Ultimate Trainer).",
                   "Personnage",
                   { { 0x84E0E9, { 0x01 }, { 0x03 } },
                     { 0x84E0BC, { 0x75 }, { 0xEB } },
                     { 0x84E174, { 0x75 }, { 0xEB } },
                     { 0x84E1A6, { 0x02 }, { 0x0A } } });
    AddPatchToggle("char.never_change_size", "Garder la taille modifiée (échelles, sauts)",
                   "À utiliser avec le curseur Taille : sans cette option, le jeu remet la taille normale quand le "
                   "personnage monte à une échelle ou saute d'un rebord.", "Personnage",
                   { { 0x77A6D8, { 0xF3, 0x0F, 0x11, 0x46, 0x50 }, { 0x90, 0x90, 0x90, 0x90, 0x90 } },
                     { 0x77A6E5, { 0xF3, 0x0F, 0x11, 0x46, 0x54 }, { 0x90, 0x90, 0x90, 0x90, 0x90 } },
                     { 0x77A6FA, { 0xF3, 0x0F, 0x11, 0x46, 0x58 }, { 0x90, 0x90, 0x90, 0x90, 0x90 } } });

    AddPatchToggle("combat.excella_first_aid_fix", "Correctif premiers soins d'Excella",
                   "Évite le plantage quand Excella utilise ses premiers soins. Attention : les ennemis deviennent plus agressifs.",
                   "Combat",
                   { { 0xC18770,
                       { 0x70, 0x61, 0x77, 0x6E, 0x5C, 0x65, 0x6D, 0x5C, 0x65, 0x6D, 0x34, 0x30, 0x5C, 0x70, 0x61, 0x74, 0x68, 0x5C,
                         0x65, 0x6D, 0x31, 0x30, 0x30, 0x30, 0x5F, 0x70, 0x61, 0x74, 0x68, 0x5C, 0x65, 0x6D, 0x31, 0x30, 0x30, 0x30 },
                       std::vector<uint8_t>(36, 0x00) } });

    // ---- Caméra ----
    AddPatchToggle("camera.normal_values_aiming", "Caméra normale en visée",
                   "La visée utilise les réglages de la caméra normale.", "Caméra", { { 0x446BC4, { 0x01 }, { 0x0A } } });

    // ---- Graphismes / physique ----
    AddPatchToggle("physics.doa", "Physique « DoA »", "Physique des vêtements améliorée (manteau de Wesker...).", "Graphismes",
                   { { 0x20FE92, { 0xB4 }, { 0xC4 } } });
}
}
