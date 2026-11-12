// Colours of the laser sight and of player 1's health gauge (user, 2026-10-24: the laser, the gauge fine (green by
// default), low (orange) and dying (red)). Research in MODLOG (2026-10-24).
// - Laser: the player's uLaserSight (uBioEffect, DTI RVA 0xDF7364, vtable RVA 0xBBBD00) at P+0x26DC holds RGBA floats,
//   red with an intensity: +0x250 the beam (1.3; copied into its three effects' particles every frame, VA 0xA7D652),
//   +0x270 / +0x280 the dot's two states (2.0 / 1.3; one is copied into +0x260 every frame, VA 0xA7E8D1 / 0xA7E989),
//   +0x2A0 its glow (1.0, alpha 0.2). The game sets them when the laser is created: here they get the chosen colour
//   times each one's intensity, their alpha kept.
// - Gauge: a static table filled once by the HUD (guard bit 1 of [VA 0x11F512C], VA 0x9F1381) at VA 0x11F5104, read
//   every frame: pairs (colour, its darker pulse) ARGB by state - 0 fine (5AA512 / 46820D), 1 low (C38A12 / 9B6C0D,
//   the game's test VA 0x7EA780), 2 dying (C33612 / 88260C, health 1 or 2), 3 full health (388C28 / 1F6414), 4 teal
//   (12AF94, not touched). Checked against the gauge at 1000, 500, 400, 200, 2 and 1 HP (the colours seen were mixes of
//   each pair). Rewritten with the chosen colours, each slot keeping the game's brightness against its state's colour
//   (full health follows the fine colour).
// - The ring's glow around the gauge (user: the same colour as the gauge): its sprite is the cockpit's (uIdCockpit00)
//   [+0x12D0 + 4 * [VA 0x1122608 + 4 * player]]; the same function writes its colour bytes (+0x34 B, +0x35 G, +0x36 R,
//   +0x37 A) by state (VA 0x9F149E): full health G = a pulse, dying R = a pulse, none in the other states (the ring kept
//   the full health green, traced). A call at VA 0x9F159B (after the state's branch, every state; cockpit in esi, its
//   player index in edi, the state at [esp+0x10]) recolours player 1's ring with the state's colour at the pulse's
//   brightness, then does the two instructions it replaced.
// - The laser's dot on a target: +0x2C0 RGBA (G +0x2C4, B +0x2C8, A +0x2CC; set by the laser's reset VA 0xA7D3F4, drawn
//   as a packed colour VA 0xA7F196), with the beam's colour (user).
#include "common/actors.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"
#include "common/settings.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace features {
namespace {
constexpr uintptr_t kLaserVtable = 0xBBBD00;  // uLaserSight (RVA)
struct LaserColour {
    uint32_t offset;
    float intensity;
};
// The dot's colours are turned into bytes by x255 with no clamp (VA 0xA7EC1B..: the game's 2.0 red wraps to 254, its
// 1.3 to 75; user, 2026-10-24: a violet dot came out red, its blue 1.29 wrapping to 73): they get the colour at those
// effective brightnesses, never above 1. The beam's colour goes to its particles as floats (1.3 kept).
constexpr LaserColour kLaserColours[] = { { 0x250, 1.3f }, { 0x270, 254.0f / 255.0f }, { 0x280, 75.0f / 255.0f },
                                          { 0x2A0, 1.0f }, { 0x2C0, 1.0f } };

constexpr uintptr_t kGaugeTable = 0xDF5104;  // RVA: 5 pairs of ARGB colours
enum Group { kFine, kLow, kDying, kGroups };
struct GaugeSlot {
    int index;           // dword in the table
    Group group;         // whose colour
    uint32_t game;       // the game's value
    uint32_t reference;  // the game's colour of that group (its brightness = the chosen colour's)
};
constexpr GaugeSlot kGaugeSlots[] = {
    { 0, kFine, 0xFF5AA512, 0xFF5AA512 },  { 1, kFine, 0xFF46820D, 0xFF5AA512 },
    { 2, kLow, 0xFFC38A12, 0xFFC38A12 },   { 3, kLow, 0xFF9B6C0D, 0xFFC38A12 },
    { 4, kDying, 0xFFC33612, 0xFFC33612 }, { 5, kDying, 0xFF88260C, 0xFFC33612 },
    { 6, kFine, 0xFF388C28, 0xFF5AA512 },  { 7, kFine, 0xFF1F6414, 0xFF5AA512 },
};

std::atomic<bool> g_gaugeOn[kGroups]{};
std::atomic<uint32_t> g_gaugeColour[kGroups]{};

int Brightest(uint32_t argb) { return std::max({ (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF }); }

// The chosen colour (0xRRGGBB) at the brightness the game gives this slot against its group's colour.
uint32_t Scaled(uint32_t rgb, const GaugeSlot& slot) {
    const float k = static_cast<float>(Brightest(slot.game)) / static_cast<float>(Brightest(slot.reference));
    uint32_t out = 0xFF000000;
    for (int shift : { 16, 8, 0 }) {
        const int v = std::min(255, static_cast<int>(static_cast<float>((rgb >> shift) & 0xFF) * k + 0.5f));
        out |= static_cast<uint32_t>(v) << shift;
    }
    return out;
}

// The ring (game thread, from RingStub): player 1's own gauge ring only (index 0 of his cockpit).
constexpr uintptr_t kRingIndex = 0xD22608;  // RVA: each player's ring sprite index in the cockpit
void __cdecl RecolourRing(uintptr_t cockpit, uint32_t index, uint32_t state) {
    uintptr_t player = 0, sprite = 0;
    uint32_t slot = 0;
    if (index != 0 || state > 3 || !mem::Read(cockpit + 0x1A0, player) || player != actors::Player(0) ||
        !mem::Read(reinterpret_cast<uintptr_t>(mem::Rva(kRingIndex)) + 4 * index, slot) || slot >= 0x291 ||
        !mem::Read(cockpit + 0x12D0 + 4 * slot, sprite) || !sprite)
        return;
    uint8_t bgr[3]{};
    if (!mem::SafeRead(reinterpret_cast<void*>(sprite + 0x34), bgr, sizeof(bgr))) return;
    const Group group = state == 2 ? kDying : state == 1 ? kLow : kFine;
    const int pulse = state == 3 ? bgr[1] : state == 2 ? bgr[2] : 0xEF;  // the game's: G full, R dying, none else
    const uint32_t rgb = g_gaugeOn[group].load(std::memory_order_relaxed)
                             ? g_gaugeColour[group].load(std::memory_order_relaxed)
                             : group == kDying ? 0xFF0000u : 0x00FF00u;  // the game's ring colours
    const uint8_t out[3] = { static_cast<uint8_t>((rgb & 0xFF) * pulse / 255),
                             static_cast<uint8_t>(((rgb >> 8) & 0xFF) * pulse / 255),
                             static_cast<uint8_t>(((rgb >> 16) & 0xFF) * pulse / 255) };
    mem::SafeWrite(reinterpret_cast<void*>(sprite + 0x34), out, sizeof(out));
}

__declspec(naked) void RingStub() {
    __asm {
        pushfd
        pushad
        sub esp, 0x80
        movups xmmword ptr [esp], xmm0
        movups xmmword ptr [esp + 0x10], xmm1
        movups xmmword ptr [esp + 0x20], xmm2
        movups xmmword ptr [esp + 0x30], xmm3
        movups xmmword ptr [esp + 0x40], xmm4
        movups xmmword ptr [esp + 0x50], xmm5
        movups xmmword ptr [esp + 0x60], xmm6
        movups xmmword ptr [esp + 0x70], xmm7
        mov eax, dword ptr [esp + 0x80 + 0x20 + 4 + 4 + 0x10]  // the caller's [esp+0x10]: the state
        push eax
        push edi
        push esi
        call RecolourRing
        add esp, 12
        movups xmm0, xmmword ptr [esp]
        movups xmm1, xmmword ptr [esp + 0x10]
        movups xmm2, xmmword ptr [esp + 0x20]
        movups xmm3, xmmword ptr [esp + 0x30]
        movups xmm4, xmmword ptr [esp + 0x40]
        movups xmm5, xmmword ptr [esp + 0x50]
        movups xmm6, xmmword ptr [esp + 0x60]
        movups xmm7, xmmword ptr [esp + 0x70]
        add esp, 0x80
        popad
        popfd
        mov eax, dword ptr [esp + 4 + 0x10]          // replaced: mov eax, [esp+0x10]
        mov edx, dword ptr [eax * 8 + 0x11F5104]     // replaced: the state's colour (absolute: no relocation)
        ret
    }
}

mem::Patch* g_ringCall = nullptr;

void SetRingCall(bool on) {
    if (!g_ringCall) {
        if (!on) return;
        uint8_t* site = mem::Rva(0x5F159B);
        const std::vector<uint8_t> original = { 0x8B, 0x44, 0x24, 0x10, 0x8B, 0x14, 0xC5, 0x04, 0x51, 0x1F, 0x01 };
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&RingStub) - reinterpret_cast<uintptr_t>(site + 5));
        std::vector<uint8_t> patched = { 0xE8 };
        for (int i = 0; i < 4; ++i) patched.push_back(static_cast<uint8_t>(rel >> (8 * i)));
        patched.resize(original.size(), 0x90);
        g_ringCall = new mem::Patch("gauge ring colour", site, original, patched);
    }
    if (on != g_ringCall->IsApplied()) on ? g_ringCall->Apply() : g_ringCall->Restore();
}

// Every frame (OnFrame): each slot gets its group's colour, or the game's back. The table is static data: written
// before the game fills it, it is filled over, and rewritten here the next frame.
void TickGauge() {
    bool any = false;
    for (const auto& on : g_gaugeOn) any |= on.load(std::memory_order_relaxed);
    SetRingCall(any);
    const uintptr_t table = reinterpret_cast<uintptr_t>(mem::Rva(kGaugeTable));
    for (const GaugeSlot& slot : kGaugeSlots) {
        const uint32_t want = g_gaugeOn[slot.group].load(std::memory_order_relaxed)
                                  ? Scaled(g_gaugeColour[slot.group].load(std::memory_order_relaxed), slot)
                                  : slot.game;
        uint32_t current = 0;
        if (mem::Read(table + 4 * slot.index, current) && current != want) mem::Write(table + 4 * slot.index, want);
    }
}

std::atomic<bool> g_laserOn{ false };
std::atomic<uint32_t> g_laserColour{ 0xFF0000 };  // 0xRRGGBB
uintptr_t g_laserTinted = 0;  // render thread: the laser our colour is on

uintptr_t Player1Laser() {
    const uintptr_t p = actors::InGame() ? actors::Player(0) : 0;
    uintptr_t laser = 0, vtable = 0;
    if (!p || !mem::Read(p + 0x26DC, laser) || !laser || !mem::Read(laser, vtable) ||
        vtable != reinterpret_cast<uintptr_t>(mem::Rva(kLaserVtable)))
        return 0;
    return laser;
}

void PaintLaser(uintptr_t laser, uint32_t rgb) {
    const float r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                b = static_cast<float>(rgb & 0xFF) / 255.0f;
    for (const LaserColour& c : kLaserColours) {
        const float want[3] = { r * c.intensity, g * c.intensity, b * c.intensity };
        float current[3]{};
        if (mem::SafeRead(reinterpret_cast<void*>(laser + c.offset), current, sizeof(current)) &&
            (current[0] != want[0] || current[1] != want[1] || current[2] != want[2]))
            mem::SafeWrite(reinterpret_cast<void*>(laser + c.offset), want, sizeof(want));
    }
}

// The dot on enemies and breakables (user, 2026-10-24: it stayed red): two elements of the laser's effect list [+0x190]
// (rEffectList effect\efl\core\cr0001, data [+0x64], size [+0x68]) with their own colour, the particle's not used
// (made white, the dot came out white after a restart): a glow ARGB 50FF1E00 (start / end at data +0x6B8 / +0x6BC; size
// 1.8) and the dot FFFF2600 (+0x858 / +0x85C; size 1.5) - cr0001.efl +0x6D8 / +0x878, its 0x20-byte header not
// loaded. The effect copies them when the laser is created, into the laser's allocation: start / end / current at
// laser +0xC58 / +0xC5C / +0xC98 (size +0xC60) and +0xD48 / +0xD4C / +0xD88 (size +0xD50), the same for both players.
// Both get the chosen colour (their alpha kept): the resource for the lasers created later, the copies at once.
struct DotElement {
    uint32_t resource[2];  // data offsets
    uint32_t copy[3];      // laser offsets
    uint32_t sizeAt;       // laser offset of the element's size, checked
    float size;
    uint32_t game;
};
constexpr DotElement kDotElements[] = {
    { { 0x6B8, 0x6BC }, { 0xC58, 0xC5C, 0xC98 }, 0xC60, 1.8f, 0x50FF1E00 },
    { { 0x858, 0x85C }, { 0xD48, 0xD4C, 0xD88 }, 0xD50, 1.5f, 0xFFFF2600 },
};

// Ours or the game's (same alpha): never another value.
void PaintColourSlot(uintptr_t at, uint32_t game, uint32_t want) {
    uint32_t current = 0;
    if (mem::Read(at, current) && current != want && (current & 0xFF000000) == (game & 0xFF000000))
        mem::Write(at, want);
}

void PaintDotElements(uintptr_t laser, bool on, uint32_t rgb) {
    uintptr_t list = 0, data = 0;
    uint32_t size = 0;
    char path[24]{};
    if (!mem::Read(laser + 0x190, list) || !list || !mem::SafeRead(reinterpret_cast<void*>(list + 4), path, sizeof(path) - 1) ||
        std::strcmp(path, "effect\\efl\\core\\cr0001") != 0 || !mem::Read(list + 0x64, data) || !data ||
        !mem::Read(list + 0x68, size) || size < 0x860)
        return;
    for (const DotElement& e : kDotElements) {
        const uint32_t want = on ? (e.game & 0xFF000000) | (rgb & 0xFFFFFF) : e.game;
        for (uint32_t off : e.resource) PaintColourSlot(data + off, e.game, want);
        float elementSize = 0.0f;
        if (!mem::Read(laser + e.sizeAt, elementSize) || elementSize != e.size) continue;
        for (uint32_t off : e.copy) PaintColourSlot(laser + off, e.game, want);
    }
}

void TickLaser() {
    const uintptr_t laser = Player1Laser();
    if (!laser) {
        g_laserTinted = 0;
        return;
    }
    PaintDotElements(laser, g_laserOn.load(std::memory_order_relaxed), g_laserColour.load(std::memory_order_relaxed));
    if (g_laserOn.load(std::memory_order_relaxed)) {
        PaintLaser(laser, g_laserColour.load(std::memory_order_relaxed));
        g_laserTinted = laser;
    } else if (g_laserTinted == laser) {
        PaintLaser(laser, 0xFF0000);  // the game's red
        g_laserTinted = 0;
    }
}

// The ammo count when the magazine is full (user, 2026-10-24: green in the gauge, its own colour): the HUD's number
// colour is (alpha << 24) | 0x00FF00 when full (or eax, 0xFF00 at VA 0x9F2032), else | 0xC8C8C8 (VA 0x9F205E), stored
// in the number's sprite (+0x34, VA 0x9F208A; traced on a shot and a reload). The immediate becomes the chosen colour.
std::atomic<bool> g_ammoOn{ false };
std::atomic<uint32_t> g_ammoColour{ 0x00FF00 };
mem::Patch* g_ammoPatch = nullptr;
uint32_t g_ammoPatched = 0;

void TickAmmoColour() {
    const bool on = g_ammoOn.load(std::memory_order_relaxed);
    const uint32_t rgb = g_ammoColour.load(std::memory_order_relaxed) & 0xFFFFFF;
    if (g_ammoPatch && (!on || g_ammoPatched != rgb)) {
        g_ammoPatch->Restore();
        delete g_ammoPatch;
        g_ammoPatch = nullptr;
    }
    if (!on || g_ammoPatch) return;
    g_ammoPatch = new mem::Patch("ammo full colour", 0x5F2032, { 0x0D, 0x00, 0xFF, 0x00, 0x00 },
                                 { 0x0D, static_cast<uint8_t>(rgb), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb >> 16),
                                   0x00 });
    g_ammoPatched = rgb;
    if (!g_ammoPatch->Apply()) {
        delete g_ammoPatch;
        g_ammoPatch = nullptr;
        g_ammoOn = false;
    }
}

uint32_t Rgb(float value) {
    const float v = value < 0.0f ? 0.0f : value > 16777215.0f ? 16777215.0f : value;
    return static_cast<uint32_t>(v + 0.5f);
}
}  // namespace

void RegisterHudColors() {
    Feature& laser = Add({ .id = "display.laser_color", .name = "Couleur du laser",
                           .help = "Le laser de visée de Chris (faisceau, point et halo) prend la couleur choisie, avec "
                                   "l'éclat du jeu. Décochée : le rouge du jeu.",
                           .category = "Affichage", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = static_cast<float>(0xFF0000), .color = true });
    Feature* const pl = &laser;
    laser.set = [pl](bool on) {
        g_laserColour = Rgb(pl->value);
        g_laserOn = on;
        return true;
    };
    laser.tick = [pl] { g_laserColour.store(Rgb(pl->value), std::memory_order_relaxed); };
    OnFrame(TickLaser);

    struct GaugeOption {
        const char* id;
        const char* name;
        const char* help;
        uint32_t game;
    };
    static constexpr GaugeOption kGaugeOptions[] = {
        { "display.gauge_color_fine", "Couleur de la jauge de vie : vie OK",
          "La couleur de la jauge de vie de Chris quand sa vie est bonne (vert dans le jeu), pleine vie comprise.",
          0x5AA512 },
        { "display.gauge_color_low", "Couleur de la jauge de vie : vie faible",
          "La couleur de la jauge de vie de Chris quand sa vie est basse (orange dans le jeu).", 0xC38A12 },
        { "display.gauge_color_dying", "Couleur de la jauge de vie : agonie",
          "La couleur de la jauge de vie de Chris quand il est mourant (rouge dans le jeu).", 0xC33612 },
    };
    for (int k = 0; k < kGroups; ++k) {
        Feature& f = Add({ .id = kGaugeOptions[k].id, .name = kGaugeOptions[k].name, .help = kGaugeOptions[k].help,
                           .category = "Affichage", .kind = Kind::Toggle,
                           .available = [] { return game::IsSupported(); },
                           .value = static_cast<float>(kGaugeOptions[k].game), .color = true });
        Feature* const pf = &f;
        f.set = [pf, k](bool on) {
            g_gaugeColour[k] = Rgb(pf->value);
            g_gaugeOn[k] = on;
            return true;
        };
        f.tick = [pf, k] { g_gaugeColour[k].store(Rgb(pf->value), std::memory_order_relaxed); };
    }
    OnFrame(TickGauge);
    Feature& ammo = Add({ .id = "display.ammo_full_color", .name = "Couleur des munitions au maximum",
                          .help = "La couleur du nombre de munitions de la jauge quand l'arme est pleine (vert dans le "
                                  "jeu). Décochée : le vert du jeu.",
                          .category = "Affichage", .kind = Kind::Toggle,
                          .available = [] { return game::IsSupported(); },
                          .value = static_cast<float>(0x00FF00), .color = true });
    Feature* const pa = &ammo;
    ammo.set = [pa](bool on) {
        g_ammoColour = Rgb(pa->value);
        g_ammoOn = on;
        return true;
    };
    ammo.tick = [pa] { g_ammoColour.store(Rgb(pa->value), std::memory_order_relaxed); };
    OnFrame(TickAmmoColour);
    // "Remettre les couleurs par défaut" (user, 2026-10-26: "to see the gauge's colour and give the stamina the same"):
    // every colour picker back to the game's colour (the stamina's: the gauge's green as seen), applied at once where checked.
    Add({ .id = "display.colors_default", .name = "Remettre les couleurs par défaut",
          .help = "Remet chaque sélecteur de couleur (laser, jauge de vie dans ses trois états, munitions, endurance) sur "
                  "la couleur du jeu, pour la voir ou la reprendre ailleurs. Les cases restent comme elles sont.",
          .category = "Affichage", .kind = Kind::Action,
          .run = [] {
              static constexpr struct { const char* id; uint32_t rgb; } kDefaults[] = {
                  { "display.laser_color", 0xFF0000 },     { "display.gauge_color_fine", 0x5AA512 },
                  { "display.gauge_color_low", 0xC38A12 }, { "display.gauge_color_dying", 0xC33612 },
                  { "display.ammo_full_color", 0x00FF00 }, { "player.stamina_color", 0x15920D },
              };
              for (const auto& d : kDefaults) {
                  Feature* f = Find(d.id);
                  if (!f) continue;
                  f->value = static_cast<float>(d.rgb);
                  if (f->set && f->enabled) f->set(true);
                  settings::SaveFeature(*f);
              }
              return true;
          },
          .available = [] { return game::IsSupported(); } });
    OnRestore([] {
        g_ammoOn = false;
        TickAmmoColour();
        for (auto& on : g_gaugeOn) on = false;
        TickGauge();
        SetRingCall(false);
        g_laserOn = false;
        TickLaser();
    });
}
// The laser's colour (0xRRGGBB): the chosen one with the laser colour option, else the game's red (camera.cpp's dot for
// the scoped weapons in the iron sight mode).
uint32_t LaserColourRgb() { return g_laserOn.load(std::memory_order_relaxed) ? g_laserColour.load(std::memory_order_relaxed) : 0xFF0000; }
}  // namespace features
