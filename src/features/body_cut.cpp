// Player 1's body cut triangle by triangle in the first person (see body_cut.hpp).
#include "features/body_cut.hpp"
#include "common/features.hpp"
#include "common/game.hpp"
#include "common/log.hpp"
#include "common/mem.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace bodycut {
namespace {
// Player 1's model (2026-11-03, read live on Chris pl0400; the draws logged through the device hooks): uModel +0xA80 =
// its rModel; rModel +0x70 the primitives (0x34 bytes each, count +0x74: +0 the group of the parts mask uModel +0x150,
// +2 the material, +4 drawn, +5 the levels of detail, +0xC the vertex count, +0x10 the first vertex, +0x14 the vertex
// stream offset, +0x1C the first index, +0x20 the index count (a strip: its draw has count - 2 triangles), +0x2D the
// bone palette), +0x80 the bone palettes (0x24 bytes: count, then up to 32 joint indices; count +0x84), +0xA8 the index
// buffer element (cTrans::IndexBuffer, its IDirect3DIndexBuffer9 at +0x10; managed pool, 16-bit). The skinned vertices
// (stride 32): SHORT4N position, UBYTE4 blend indices at +8 (slots of the primitive's palette), UBYTE4N weights at +12.
// The uModel's joints (+0x318, 0x90 each, count +0x314: +5 parent, +7 id) are the palettes' joint indices. The main
// pass draws one primitive a call, the shadow passes join neighbours of the same stream offset and palette.
//
// Closing the cut (user, 2026-11-04: "the hands look hollow at the wrists, not what a VR version wants"): the kept
// triangles' edges that a removed triangle shared - by the vertices' positions, so the edge of a glove against the
// removed skin of the forearm counts too - are chained into loops, each filled by a fan of the loop's own vertices:
// the lid is skinned like the stump's rim and follows it.
struct Prim {
    uint32_t voff, istart, icount;
    uint8_t palette, lod;
};

struct Plan {
    IDirect3DIndexBuffer9* ib = nullptr;
    std::vector<Prim> prims;
    std::vector<std::array<uint8_t, 32>> hidden;  // per palette slot: 1 = skinned to a hidden joint
    std::vector<uint8_t> paletteCount;
};

std::atomic<bool> g_hooked{ false };
std::atomic<bool> g_log{ false };

// Game thread.
uintptr_t g_player = 0, g_model = 0;
int g_mode = -1;

std::mutex g_planLock;
Plan g_plan;                                              // under g_planLock
std::atomic<uint32_t> g_generation{ 0 };                  // bumped by each new plan
std::atomic<IDirect3DIndexBuffer9*> g_target{ nullptr };  // the index buffer cut now (null: none)

// Render thread: the device's current state, kept by the hooks (a pure device answers no Get* call).
IDirect3DIndexBuffer9* g_indices = nullptr;
IDirect3DVertexBuffer9* g_stream[4]{};
UINT g_streamOffset[4]{}, g_streamStride[4]{};
IDirect3DVertexDeclaration9* g_decl = nullptr;

// A stream offset's vertex layout, from the draws seen.
// Up to two sets of four bones (usage indices 0 / 1: some blocks skin 8 bones a vertex, user, 2026-11-04: the left
// shoulder stayed open, its vertices read with half their weights).
struct Format {
    UINT stride = 0;
    int pos = -1;
    BYTE posType = 0;
    int indices[2] = { -1, -1 }, weights[2] = { -1, -1 };
    BYTE indexType[2]{}, weightType[2]{};
};

enum class CutKind { Game, Nothing, Ours };
struct Cut {
    CutKind kind = CutKind::Game;
    IDirect3DIndexBuffer9* ib = nullptr;
    UINT triangles = 0;
    UINT minIndex = 0, numVertices = 0;  // the list's own vertex range (a lid may use another primitive's corners)
};

struct PrimCut {
    bool analysed = false, changed = false;
    std::vector<uint16_t> list;  // the triangles to draw (kept and lids), a list
};

uint32_t g_renderGeneration = 0;
Plan g_renderPlan;
IDirect3DVertexBuffer9* g_vertexBuffer = nullptr;  // the model's (every primitive's stream 0)
std::map<UINT, Format> g_formats;
bool g_built = false;
int g_drawsSinceNewFormat = 0;
std::vector<PrimCut> g_primCuts;
std::map<std::tuple<UINT, UINT, UINT, INT, int>, Cut> g_cuts;  // (stream offset, start, count, base, type)

void ReleaseCuts() {
    for (auto& [key, cut] : g_cuts)
        if (cut.ib) cut.ib->Release();
    g_cuts.clear();
}

bool UnderId(const std::vector<int>& parent, const std::vector<uint8_t>& id, int j, uint8_t a, uint8_t b) {
    for (int k = j, depth = 0; k >= 0 && depth < 64; k = parent[k], ++depth)
        if (id[k] == a || id[k] == b) return true;
    return false;
}

// The joints hidden in a mode, as camera.cpp's shrinking ones: 0 the neck (id 3) and what hangs from it, else all but
// what hangs from the upper arms (6 / 11), the forearms (7 / 12) or the hands (8 / 13). The arms mode keeps all that
// hangs from the collarbones (5 / 10) but them (user, 2026-11-04: "keep the shoulders' balls, it cuts the front of the
// shoulders too early": the deltoids are skinned to the collarbones' other children, ids 62 / 64 on Chris).
bool BuildPlan(uintptr_t p, uintptr_t model, int mode, Plan& plan) {
    uintptr_t joints = 0, prims = 0, palettes = 0, element = 0;
    uint32_t jointCount = 0, primCount = 0, paletteCount = 0;
    if (!mem::Read(p + 0x318, joints) || !joints || !mem::Read(p + 0x314, jointCount) || !jointCount || jointCount > 256 ||
        !mem::Read(model + 0x70, prims) || !prims || !mem::Read(model + 0x74, primCount) || !primCount || primCount > 4096 ||
        !mem::Read(model + 0x80, palettes) || !palettes || !mem::Read(model + 0x84, paletteCount) || !paletteCount ||
        paletteCount > 256 || !mem::Read(model + 0xA8, element) || !element || !mem::Read(element + 0x10, plan.ib) || !plan.ib)
        return false;
    std::vector<int> parent(jointCount, -1);
    std::vector<uint8_t> id(jointCount, 0xFF);
    for (uint32_t j = 0; j < jointCount; ++j) {
        uint8_t up = 0xFF;
        mem::Read(joints + 0x90 * j + 5, up);
        mem::Read(joints + 0x90 * j + 7, id[j]);
        parent[j] = up < jointCount ? up : -1;
    }
    static constexpr uint8_t kKept[4][2] = { { 0, 0 }, { 6, 11 }, { 7, 12 }, { 8, 13 } };
    std::vector<uint8_t> hiddenJoint(jointCount, 0);
    for (uint32_t j = 0; j < jointCount; ++j) {
        if (mode <= 0) {
            hiddenJoint[j] = UnderId(parent, id, j, 3, 3);
        } else if (mode == 1) {
            const bool collarbone = id[j] == 5 || id[j] == 10;
            hiddenJoint[j] = collarbone || !UnderId(parent, id, j, 5, 10);
        } else {
            hiddenJoint[j] = !UnderId(parent, id, j, kKept[mode][0], kKept[mode][1]);
        }
    }
    plan.hidden.assign(paletteCount, {});
    plan.paletteCount.assign(paletteCount, 0);
    for (uint32_t i = 0; i < paletteCount; ++i) {
        uint8_t entry[0x24]{};
        if (!mem::SafeRead(reinterpret_cast<void*>(palettes + 0x24 * i), entry, sizeof(entry))) return false;
        const uint32_t n = std::min<uint32_t>(*reinterpret_cast<uint32_t*>(entry), 32);
        plan.paletteCount[i] = static_cast<uint8_t>(n);
        for (uint32_t s = 0; s < n; ++s) {
            const uint8_t j = entry[4 + s];
            plan.hidden[i][s] = j < jointCount ? hiddenJoint[j] : 0;
        }
    }
    plan.prims.clear();
    for (uint32_t i = 0; i < primCount; ++i) {
        uint8_t e[0x34]{};
        if (!mem::SafeRead(reinterpret_cast<void*>(prims + 0x34 * i), e, sizeof(e))) return false;
        Prim prim{ *reinterpret_cast<uint32_t*>(e + 0x14), *reinterpret_cast<uint32_t*>(e + 0x1C),
                   *reinterpret_cast<uint32_t*>(e + 0x20), e[0x2D], e[5] };
        if (prim.palette < paletteCount && prim.icount >= 3) plan.prims.push_back(prim);
    }
    return true;
}

// The current declaration's layout (stream 0): position, blend indices (UBYTE4 / D3DCOLOR), weights (UBYTE4N /
// D3DCOLOR / FLOATn; none = the first index alone).
Format ReadFormat() {
    Format f;
    f.stride = g_streamStride[0];
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1]{};
    UINT n = MAXD3DDECLLENGTH + 1;
    if (!g_decl || FAILED(g_decl->GetDeclaration(elements, &n))) return f;
    for (UINT i = 0; i < n && elements[i].Stream != 0xFF; ++i) {
        const D3DVERTEXELEMENT9& e = elements[i];
        if (g_log.load(std::memory_order_relaxed))
            LOG("body cut: offset %u element offset %u type %u usage %u index %u", g_streamOffset[0], e.Offset, e.Type, e.Usage, e.UsageIndex);
        if (e.Stream != 0 || e.UsageIndex > 1) continue;
        const int set = e.UsageIndex;
        if (e.Usage == D3DDECLUSAGE_POSITION && set == 0) {
            f.pos = e.Offset;
            f.posType = e.Type;
        } else if (e.Usage == D3DDECLUSAGE_BLENDINDICES && (e.Type == D3DDECLTYPE_UBYTE4 || e.Type == D3DDECLTYPE_D3DCOLOR)) {
            f.indices[set] = e.Offset;
            f.indexType[set] = e.Type;
        } else if (e.Usage == D3DDECLUSAGE_BLENDWEIGHT &&
                   (e.Type == D3DDECLTYPE_UBYTE4N || e.Type == D3DDECLTYPE_D3DCOLOR || e.Type <= D3DDECLTYPE_FLOAT4)) {
            f.weights[set] = e.Offset;
            f.weightType[set] = e.Type;
        }
    }
    return f;
}

// One set of four bones' slots and weights (no weights = the first slot alone).
void ReadBones(const uint8_t* px, int indices, BYTE indexType, int weights, BYTE weightType, uint8_t slot[4], float weight[4]) {
    std::memcpy(slot, px + indices, 4);
    if (indexType == D3DDECLTYPE_D3DCOLOR) std::swap(slot[0], slot[2]);  // BGRA in memory
    weight[0] = 1.0f;
    weight[1] = weight[2] = weight[3] = 0.0f;
    if (weights < 0) return;
    if (weightType == D3DDECLTYPE_UBYTE4N || weightType == D3DDECLTYPE_D3DCOLOR) {
        uint8_t w[4];
        std::memcpy(w, px + weights, 4);
        if (weightType == D3DDECLTYPE_D3DCOLOR) std::swap(w[0], w[2]);
        for (int i = 0; i < 4; ++i) weight[i] = w[i] / 255.0f;
    } else {
        const int n = weightType - D3DDECLTYPE_FLOAT1 + 1;
        float w[4]{};
        std::memcpy(w, px + weights, sizeof(float) * n);
        float rest = 1.0f;
        for (int i = 0; i < n; ++i) rest -= weight[i] = w[i];
        if (n < 4) weight[n] = std::max(0.0f, rest);  // the last weight is implied
    }
}

// The share of a vertex's weight on hidden joints (0..1); kept below one half.
float HiddenShare(const uint8_t* px, const Format& f, const std::array<uint8_t, 32>& hidden, UINT slots) {
    float hiddenWeight = 0.0f, total = 0.0f;
    for (int set = 0; set < 2; ++set) {
        if (f.indices[set] < 0) continue;
        uint8_t slot[4];
        float weight[4];
        ReadBones(px, f.indices[set], f.indexType[set], f.weights[set], f.weightType[set], slot, weight);
        if (set == 1 && f.weights[1] < 0) break;  // a second set of slots without weights: unknown, left out
        for (int i = 0; i < 4; ++i) {
            if (weight[i] <= 0.0f) continue;
            total += weight[i];
            if (slot[i] < slots && hidden[slot[i]]) hiddenWeight += weight[i];
        }
    }
    return total > 0.0f ? hiddenWeight / total : 0.0f;
}

// A vertex's position as a key: the same place in two primitives (a texture seam, a material border) is one point.
uint64_t PositionKey(const uint8_t* px, const Format& f, uint32_t fallback) {
    if (f.pos < 0) return 0x8000000000000000ull | fallback;
    if (f.posType == D3DDECLTYPE_SHORT4N || f.posType == D3DDECLTYPE_SHORT4) {
        uint64_t k = 0;
        std::memcpy(&k, px + f.pos, 6);
        return k;
    }
    if (f.posType == D3DDECLTYPE_FLOAT3 || f.posType == D3DDECLTYPE_FLOAT4) {
        uint64_t h = 1469598103934665603ull;  // FNV-1a over the 12 bytes
        for (int i = 0; i < 12; ++i) h = (h ^ px[f.pos + i]) * 1099511628211ull;
        return h | 0x4000000000000000ull;
    }
    return 0x8000000000000000ull | fallback;
}

struct EdgeKeyHash {
    size_t operator()(const std::pair<uint64_t, uint64_t>& e) const { return std::hash<uint64_t>()(e.first * 0x9E3779B97F4A7C15ull ^ e.second); }
};

// A kept vertex this much skinned to hidden joints is near the cut: a boundary edge between two such is closed.
constexpr float kRimShare = 0.05f;

// Every primitive with a known layout analysed at once (the edges are counted across primitives of the same levels of
// detail): its kept triangles and the lids of its cut.
void Analyse() {
    g_primCuts.assign(g_renderPlan.prims.size(), {});
    if (!g_vertexBuffer || !g_renderPlan.ib) return;
    D3DINDEXBUFFER_DESC id{};
    D3DVERTEXBUFFER_DESC vd{};
    if (FAILED(g_renderPlan.ib->GetDesc(&id)) || id.Format != D3DFMT_INDEX16 || FAILED(g_vertexBuffer->GetDesc(&vd))) return;
    std::vector<uint16_t> indices(id.Size / 2);
    std::vector<uint8_t> vertices(vd.Size);
    void* data = nullptr;
    if (FAILED(g_renderPlan.ib->Lock(0, 0, &data, D3DLOCK_READONLY)) || !data) return;
    std::memcpy(indices.data(), data, indices.size() * 2);
    g_renderPlan.ib->Unlock();
    if (FAILED(g_vertexBuffer->Lock(0, 0, &data, D3DLOCK_READONLY)) || !data) return;
    std::memcpy(vertices.data(), data, vertices.size());
    g_vertexBuffer->Unlock();
    if (g_log.load(std::memory_order_relaxed)) {  // research (2026-11-04): the model for an analysis outside the game
        FILE* f = nullptr;
        if (_wfopen_s(&f, (logx::DataDir() + L"\\body_ib.bin").c_str(), L"wb") == 0 && f) {
            fwrite(indices.data(), 2, indices.size(), f);
            fclose(f);
        }
        if (_wfopen_s(&f, (logx::DataDir() + L"\\body_vb.bin").c_str(), L"wb") == 0 && f) {
            fwrite(vertices.data(), 1, vertices.size(), f);
            fclose(f);
        }
        if (_wfopen_s(&f, (logx::DataDir() + L"\\body_info.txt").c_str(), L"w") == 0 && f) {
            for (const auto& [voff, fmt] : g_formats)
                fprintf(f, "format %u stride %u pos %d %u indices %d %u %d %u weights %d %u %d %u\n", voff, fmt.stride, fmt.pos,
                        fmt.posType, fmt.indices[0], fmt.indexType[0], fmt.indices[1], fmt.indexType[1], fmt.weights[0],
                        fmt.weightType[0], fmt.weights[1], fmt.weightType[1]);
            for (const Prim& prim : g_renderPlan.prims)
                fprintf(f, "prim %u %u %u %u %u\n", prim.voff, prim.istart, prim.icount, prim.palette, prim.lod);
            for (size_t i = 0; i < g_renderPlan.hidden.size(); ++i) {
                fprintf(f, "palette %zu %u", i, g_renderPlan.paletteCount[i]);
                for (UINT k = 0; k < g_renderPlan.paletteCount[i]; ++k) fprintf(f, " %u", g_renderPlan.hidden[i][k]);
                fprintf(f, "\n");
            }
            fclose(f);
        }
    }

    struct Triangle {
        uint16_t v[3];
        uint64_t key[3];
        float share[3];  // hidden share of each corner
        bool kept;
    };
    std::vector<std::vector<Triangle>> triangles(g_renderPlan.prims.size());
    for (size_t i = 0; i < g_renderPlan.prims.size(); ++i) {
        const Prim& prim = g_renderPlan.prims[i];
        const auto fit = g_formats.find(prim.voff);
        if (fit == g_formats.end() || fit->second.indices[0] < 0 || !fit->second.stride) continue;
        const Format& f = fit->second;
        if (prim.istart + prim.icount > indices.size()) continue;
        const auto& hidden = g_renderPlan.hidden[prim.palette];
        const UINT slots = g_renderPlan.paletteCount[prim.palette];
        bool fits = true;
        std::unordered_map<uint16_t, std::pair<float, uint64_t>> seen;  // vertex -> hidden share, position key
        const auto vertex = [&](uint16_t v) -> std::pair<float, uint64_t> {
            if (auto it = seen.find(v); it != seen.end()) return it->second;
            const size_t at = prim.voff + static_cast<size_t>(v) * f.stride;
            std::pair<float, uint64_t> r{ 0.0f, 0 };
            if (at + f.stride > vertices.size()) fits = false;
            else r = { HiddenShare(vertices.data() + at, f, hidden, slots), PositionKey(vertices.data() + at, f, prim.voff + v) };
            return seen[v] = r;
        };
        for (uint32_t t = 0; t + 2 < prim.icount; ++t) {
            uint16_t a = indices[prim.istart + t], b = indices[prim.istart + t + 1], c = indices[prim.istart + t + 2];
            if (a == b || b == c || a == c) continue;
            if (t & 1) std::swap(a, b);  // a strip's odd triangles in their drawn winding
            const auto va = vertex(a), vb = vertex(b), vc = vertex(c);
            triangles[i].push_back({ { a, b, c }, { va.second, vb.second, vc.second }, { va.first, vb.first, vc.first },
                                     va.first <= 0.5f && vb.first <= 0.5f && vc.first <= 0.5f });
        }
        if (!fits) triangles[i].clear();
        else g_primCuts[i].analysed = true;
    }

    // The edges by position, per level of detail: kept / removed triangles using each.
    std::map<uint8_t, std::unordered_map<std::pair<uint64_t, uint64_t>, std::pair<int, int>, EdgeKeyHash>> edges;
    const auto edgeKey = [](uint64_t a, uint64_t b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); };
    for (size_t i = 0; i < triangles.size(); ++i)
        for (const Triangle& t : triangles[i])
            for (int k = 0; k < 3; ++k) {
                auto& count = edges[g_renderPlan.prims[i].lod][edgeKey(t.key[k], t.key[(k + 1) % 3])];
                (t.kept ? count.first : count.second)++;
            }

    // The kept triangles.
    for (size_t i = 0; i < triangles.size(); ++i) {
        PrimCut& cut = g_primCuts[i];
        if (!cut.analysed) continue;
        for (const Triangle& t : triangles[i]) {
            if (t.kept) cut.list.insert(cut.list.end(), { t.v[0], t.v[1], t.v[2] });
            else cut.changed = true;
        }
    }

    // The rim: the kept triangles' edges kept once and removed at least once, walked the removed side's way; or kept
    // once with both ends partly skinned to hidden joints (user, 2026-11-04: the shoulders stayed open in the arms mode -
    // a sleeve is a tube of its own, open at the top under the vest, never joined to the chest: no removed triangle shares
    // its rim; a short sleeve's hem, on the kept upper arm alone, stays open). Chained over every primitive of a level of
    // detail at once (each shoulder's rim runs over two primitives, two materials: chained one primitive at a time, the
    // left one was closed by pieces only). A lid goes to the list of its first edge's primitive; its corners must share
    // that primitive's stream offset (one vertex range), else each stream offset's part is closed on its own.
    struct Rim {
        uint32_t prim;
        uint16_t from, to;
        uint64_t fromKey, toKey;
        uint8_t lod;
        bool used;
    };
    std::vector<Rim> rim;
    for (size_t i = 0; i < triangles.size(); ++i) {
        if (!g_primCuts[i].analysed) continue;
        const uint8_t lod = g_renderPlan.prims[i].lod;
        auto& lodEdges = edges[lod];
        for (const Triangle& t : triangles[i]) {
            if (!t.kept) continue;
            for (int k = 0; k < 3; ++k) {
                const int n = (k + 1) % 3;
                const auto it = lodEdges.find(edgeKey(t.key[k], t.key[n]));
                if (it != lodEdges.end() && it->second.first == 1 &&
                    (it->second.second > 0 || (t.share[k] >= kRimShare && t.share[n] >= kRimShare)))
                    rim.push_back({ static_cast<uint32_t>(i), t.v[n], t.v[k], t.key[n], t.key[k], lod, false });
            }
        }
    }
    std::multimap<std::pair<uint8_t, uint64_t>, size_t> byStart;
    for (size_t r = 0; r < rim.size(); ++r) byStart.emplace(std::make_pair(rim[r].lod, rim[r].fromKey), r);
    size_t lids = 0, loops = 0;
    const auto addLid = [&](uint32_t prim, const std::vector<uint16_t>& loop) {
        if (loop.size() < 3) return;
        ++loops;
        PrimCut& cut = g_primCuts[prim];
        // Both faces: the lid closes the stump whichever side it is seen from.
        for (size_t k = 1; k + 1 < loop.size(); ++k) {
            cut.list.insert(cut.list.end(), { loop[0], loop[k], loop[k + 1], loop[0], loop[k + 1], loop[k] });
            lids += 2;
        }
        cut.changed = true;
    };
    for (size_t r = 0; r < rim.size(); ++r) {
        if (rim[r].used) continue;
        std::vector<size_t> chain;
        const uint64_t first = rim[r].fromKey;
        size_t at = r;
        for (int guard = 0; guard < 4096; ++guard) {
            rim[at].used = true;
            chain.push_back(at);
            if (rim[at].toKey == first) break;
            size_t next = SIZE_MAX;
            for (auto [it, end] = byStart.equal_range(std::make_pair(rim[at].lod, rim[at].toKey)); it != end; ++it)
                if (!rim[it->second].used) {
                    next = it->second;
                    break;
                }
            if (next == SIZE_MAX) break;  // an open rim: closed straight across
            at = next;
        }
        // By stream offset: the whole loop in one when its corners share one.
        std::map<uint32_t, std::pair<uint32_t, std::vector<uint16_t>>> parts;  // stream offset -> (primitive, corners)
        for (size_t e : chain) {
            auto& part = parts[g_renderPlan.prims[rim[e].prim].voff];
            if (part.second.empty()) part.first = rim[e].prim;
            part.second.push_back(rim[e].from);
        }
        for (const auto& [voff, part] : parts) addLid(part.first, part.second);
    }
    if (g_log.load(std::memory_order_relaxed)) {
        size_t analysed = 0, changed = 0;
        for (const PrimCut& c : g_primCuts) analysed += c.analysed, changed += c.changed;
        LOG("body cut: %zu of %zu primitives analysed, %zu cut, %zu loops closed by %zu triangles", analysed, g_primCuts.size(),
            changed, loops, lids);
    }
}

// A draw's index list: the primitives it covers (same stream offset), each wholly.
Cut BuildCut(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT start, UINT count) {
    Cut cut;
    if (type != D3DPT_TRIANGLESTRIP && type != D3DPT_TRIANGLELIST) return cut;
    const UINT streamOffset = g_streamOffset[0];
    const UINT indexCount = type == D3DPT_TRIANGLESTRIP ? count + 2 : count * 3;
    std::vector<uint16_t> list;
    bool changed = false, startFound = false, endFound = false;
    for (size_t i = 0; i < g_renderPlan.prims.size(); ++i) {
        const Prim& prim = g_renderPlan.prims[i];
        if (prim.voff != streamOffset || prim.istart + prim.icount <= start || prim.istart >= start + indexCount) continue;
        if (prim.istart < start || prim.istart + prim.icount > start + indexCount || !g_primCuts[i].analysed) return cut;
        startFound |= prim.istart == start;
        endFound |= prim.istart + prim.icount == start + indexCount;
        changed |= g_primCuts[i].changed;
        list.insert(list.end(), g_primCuts[i].list.begin(), g_primCuts[i].list.end());
    }
    if (!startFound || !endFound || !changed) return cut;
    if (list.empty()) {
        cut.kind = CutKind::Nothing;
        return cut;
    }
    IDirect3DIndexBuffer9* ib = nullptr;
    void* data = nullptr;
    if (FAILED(device->CreateIndexBuffer(static_cast<UINT>(list.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr)) || !ib)
        return cut;
    if (FAILED(ib->Lock(0, 0, &data, 0)) || !data) {
        ib->Release();
        return cut;
    }
    std::memcpy(data, list.data(), list.size() * 2);
    ib->Unlock();
    cut.kind = CutKind::Ours;
    cut.ib = ib;
    cut.triangles = static_cast<UINT>(list.size() / 3);
    const auto [lo, hi] = std::minmax_element(list.begin(), list.end());
    cut.minIndex = *lo;
    cut.numVertices = *hi - *lo + 1u;
    return cut;
}
}  // namespace

void HooksReady() { g_hooked = true; }
bool Ready() { return g_hooked.load(std::memory_order_relaxed); }

void OnSetIndices(IDirect3DIndexBuffer9* ib) { g_indices = ib; }

void OnSetStream(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride) {
    if (stream >= 4) return;
    g_stream[stream] = vb;
    g_streamOffset[stream] = offset;
    g_streamStride[stream] = stride;
}

void OnSetDecl(IDirect3DVertexDeclaration9* decl) { g_decl = decl; }

// The layouts are learnt from the draws first (the model's primitives drawn unchanged for a few frames), then every
// primitive is analysed at once; a layout seen later (another level of detail) makes a new analysis.
bool OnDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT numVertices, UINT start,
            UINT count, DrawFn draw) {
    IDirect3DIndexBuffer9* const target = g_target.load(std::memory_order_relaxed);
    if (!target || g_indices != target) return false;
    if (const uint32_t generation = g_generation.load(std::memory_order_acquire); generation != g_renderGeneration) {
        ReleaseCuts();
        {
            std::lock_guard lock(g_planLock);
            g_renderPlan = g_plan;
        }
        g_renderGeneration = generation;
        g_formats.clear();
        g_primCuts.clear();
        g_built = false;
        g_drawsSinceNewFormat = 0;
        g_vertexBuffer = nullptr;
    }
    if (g_renderPlan.ib != target || !g_stream[0] || base != 0) return false;
    if (!g_formats.count(g_streamOffset[0])) {
        g_formats[g_streamOffset[0]] = ReadFormat();
        g_vertexBuffer = g_stream[0];
        g_drawsSinceNewFormat = 0;
        if (g_built) {
            g_built = false;
            ReleaseCuts();
        }
    }
    if (!g_built) {
        if (++g_drawsSinceNewFormat < 300) return false;
        Analyse();
        g_built = true;
    }
    const auto key = std::make_tuple(g_streamOffset[0], start, count, base, static_cast<int>(type));
    auto it = g_cuts.find(key);
    if (it == g_cuts.end()) it = g_cuts.emplace(key, BuildCut(device, type, start, count)).first;
    const Cut& cut = it->second;
    if (cut.kind == CutKind::Game) return false;
    if (cut.kind == CutKind::Nothing) return true;
    device->SetIndices(cut.ib);
    draw(device, D3DPT_TRIANGLELIST, base, cut.minIndex, cut.numVertices, 0, cut.triangles);
    device->SetIndices(target);
    return true;
}

void Set(uintptr_t player, int mode) {
    uintptr_t model = 0;
    if (mode >= 0 && player) mem::Read(player + 0xA80, model);
    if (mode < 0 || !model) {
        g_target.store(nullptr, std::memory_order_relaxed);
        return;
    }
    if (player != g_player || model != g_model || mode != g_mode) {
        Plan plan;
        g_player = player;
        g_model = model;
        g_mode = mode;
        if (!BuildPlan(player, model, mode, plan)) {
            LOG("body cut: player %08X's model %08X not read", static_cast<unsigned>(player), static_cast<unsigned>(model));
            plan = Plan{};
        } else {
            LOG("body cut: mode %d for player %08X (model %08X, %zu primitives, %zu palettes)", mode,
                static_cast<unsigned>(player), static_cast<unsigned>(model), plan.prims.size(), plan.hidden.size());
        }
        IDirect3DIndexBuffer9* const ib = plan.ib;
        {
            std::lock_guard lock(g_planLock);
            g_plan = std::move(plan);
        }
        g_generation.fetch_add(1, std::memory_order_release);
        g_target.store(ib, std::memory_order_relaxed);
        return;
    }
    g_target.store(g_plan.ib, std::memory_order_relaxed);
}

void RegisterFeatures() {
    using namespace features;
    Add({ .id = "diag.body_draw_log", .name = "Journal des dessins du joueur 1 (recherche)",
          .help = "Écrit dans trainer.log la découpe du modèle du joueur 1 en première personne (formats des sommets, "
                  "primitives coupées, bords refermés).",
          .category = "Diagnostic", .kind = Kind::Toggle,
          .set = [](bool on) {
              g_log = on;
              return true;
          },
          .available = [] { return game::IsSupported(); } });
}
}  // namespace bodycut
