#include "foliage.h"

#include "foliage_data.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <emmintrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// FoliageSway: grass, flowers, bushes, ivy and leaves sway in the wind as in Remastered.
//
// Remastered (QLOC) gave its foliage new materials (M_2Foliage[DSB]..., M_3Ivy[DSB]...: g_IsFoliage, g_WindSpeed 0.3,
// g_WindStrength 0.1, g_WindVerticality 0.4, g_WindPhaseScale 0.5) that draw with wind vertex shaders
// (FRPG_Wind*/FRPG_Inst_Wind*), and re-exported the map models with a second UV set holding each vertex's sway: u its
// amplitude (0 at a blade's root, 1024 at its tip) and v a phase (random per blade). The wind shader moves the vertex in
// the model's space before its world transform (frpg_wind_pin_d_non.vpo, cb0[5] = gVC_WindParam_0, cb0[6] =
// gVC_WindParam_1):
//   p = 0.1 * dot(W0.xy, pos.xz) + W1.x * W0.z + W1.w * phase
//   a = W1.y * amplitude
//   h = cos(p) cos(3p) cos(5p) cos(7p),  v = cos(0.5p) cos(1.5p) cos(2.5p) cos(3.5p)
//   pos -= (h a W0.x, v a W1.z, h a W0.y)
// with W0 = (wind direction x, z, time, the previous frame's time) and W1 = the material's (speed, strength,
// verticality, phase scale). Read from the running game (Remastered 1.03.1, 2026-11-19): every wind draw had
// W0.xy = (1, -0), W0.z = seconds (1.000 per second of real time, taken once a frame; W0.w = the frame before's, for the
// motion vectors) and W1 = (0.3, 0.1, 0.4, 0.5) unchanged, so the graphics settings behind it ("Wind Angle", "Wind Time
// Scale", "Wind Intensity", beside the LOD bias and MSAA in its debug menu; the same in every map) are at 0, 1 and 1.
// Every material its map models use has those four values (the one with phase scale 0.4, M_2Foliage[DSBT][L]_Edge, is
// used by none).
//
// Prepare to Die Edition's models have no second UV set, so the sway is worked out here as a model loads
// (FlverModel's constructor 0xF33D40, ecx = the model's name, [esp+8] = the FLVER in memory, before the game builds its
// vertex buffers), for each mesh Remastered made sway (its model and diffuse texture, foliage_data.h), by the rule that
// comes closest to Remastered's own amplitudes for that mesh: the texture's linear rule (a + b u + c v of its texture
// coordinates), per plant (the vertices its triangles join) from 0 at its root to 1 at its tip along the texture axis that
// follows its height (the root its lowest point, or its highest for hanging ivy), or Remastered's exporter's copy of the
// texture coordinates (amplitude u, phase v). Plants get a random phase each, as in Remastered. Checked against
// Remastered's own amplitudes on its models: Firelink's textures within 0.02 on average, 84% of all foliage vertices
// within 0.15. They go into the three bytes of the vertex's bone indices that every map model leaves 0 (the shaders read
// only the first): amplitude (0..2), phase, and 165 to mark the vertex.
//
// Every static model vertex shader (one that reads bone indices and no bone weights) is drawn with a copy that first
// moves the position as above when its bone indices' last byte is 165 (a branch; everything else draws as before),
// with the wind in c250/c251 as Remastered's W0/W1 (no shader of the game's uses c242-c245 or c249-c255) and its
// constants in c252-c255.
namespace {

constexpr uint32_t kFlverCtor = 0x00F33D40;
constexpr uint8_t kCtorHead[7] = {0x6A, 0xFF, 0x68, 0xA4, 0xAC, 0x05, 0x01};  // push -1; push 0x105ACA4
constexpr uint8_t kMark = 165;

bool g_on = false;
float g_strength = 0.1f;  // Remastered's g_WindStrength
float g_speed = 0.3f;     // g_WindSpeed
constexpr float kVerticality = 0.4f;
constexpr float kPhaseScale = 0.5f;
constexpr float kDirX = 1.0f, kDirZ = 0.0f;  // Remastered's wind direction (its "Wind Angle" 0)

uint32_t g_ctor_back = kFlverCtor + 7;
std::unordered_map<std::string, int> g_pairs;  // "model|texture" -> the weight rule (foliage_data.h)
std::unordered_map<std::string, const foliage_data::Linear*> g_linear;
std::unordered_set<std::string> g_textures;
std::atomic<int> g_models{0}, g_meshes{0}, g_vertices{0}, g_names_logged{0};
std::atomic<long long> g_work_ticks{0};  // QueryPerformanceCounter ticks spent in on_flver (the loading threads)

// --- the FLVER (FLVER2, 0x2000C) -------------------------------------------------------------------------------------
template <typename T>
bool rd(const uint8_t* d, size_t size, size_t off, T* out) {
    if (off > size || size - off < sizeof(T)) return false;
    std::memcpy(out, d + off, sizeof(T));
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A file name without its folders ('\' or '/'), lower case; wide or narrow.
std::string base_name(const uint8_t* d, size_t size, uint32_t off, bool wide) {
    std::string s;
    if (!off || off >= size) return s;
    if (wide) {
        for (size_t i = off; i + 1 < size && s.size() < 260; i += 2) {
            const uint16_t c = static_cast<uint16_t>(d[i] | (d[i + 1] << 8));
            if (!c) break;
            s.push_back(c < 128 ? static_cast<char>(c) : '?');
        }
    } else {
        for (size_t i = off; i < size && s.size() < 260; ++i) {
            if (!d[i]) break;
            s.push_back(static_cast<char>(d[i]));
        }
    }
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    return lower(s);
}

struct Member {
    int32_t offset, type, semantic, index;
};

struct Vtx {
    float x, y, z, u, v;
};

uint32_t find_root(std::vector<uint32_t>& parent, uint32_t a) {
    while (parent[a] != a) {
        parent[a] = parent[parent[a]];
        a = parent[a];
    }
    return a;
}

uint8_t amp_byte(float a) {  // 0..2 in 255 steps
    a = a < 0.0f ? 0.0f : (a > 2.0f ? 2.0f : a);
    return static_cast<uint8_t>(a * 127.5f + 0.5f);
}

// Amplitude and phase for one mesh's vertices (kind and linear rule from foliage_data.h).
void mesh_weights(const std::vector<Vtx>& vs, const std::vector<uint32_t>& tris, int kind,
                  const foliage_data::Linear* lin, std::vector<uint8_t>& amp, std::vector<uint8_t>& phase) {
    const size_t n = vs.size();
    std::vector<uint32_t> parent(n);
    for (uint32_t i = 0; i < n; ++i) parent[i] = i;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const uint32_t a = find_root(parent, tris[t]), b = find_root(parent, tris[t + 1]);
        parent[b] = a;
        const uint32_t c = find_root(parent, tris[t + 2]);
        parent[c] = find_root(parent, a);
    }
    std::unordered_map<uint32_t, std::vector<uint32_t>> plants;
    for (uint32_t i = 0; i < n; ++i) plants[find_root(parent, i)].push_back(i);
    amp.assign(n, 0);
    phase.assign(n, 0);
    if (kind == 3) {  // Remastered's exporter copied the texture coordinates into the sway: amplitude u, phase v
        for (size_t i = 0; i < n; ++i) {
            amp[i] = amp_byte(vs[i].u);
            const float f = vs[i].v - std::floor(vs[i].v);
            phase[i] = static_cast<uint8_t>(std::min(255.0f, f * 256.0f));
        }
        return;
    }
    for (const auto& kv : plants) {
        const auto& m = kv.second;
        const bool top = kind == 2;
        // The plant's root: its lowest point (its highest for hanging ivy).
        uint32_t root = m[0];
        for (uint32_t i : m) {
            if (top ? vs[i].y > vs[root].y : vs[i].y < vs[root].y) root = i;
        }
        const double rx = vs[root].x, rz = vs[root].z;
        double h = std::sin(rx * 12.9898 + rz * 78.233) * 43758.5453;
        h -= std::floor(h);
        const uint8_t ph = static_cast<uint8_t>(std::min(255.0, std::floor(h * 256.0)));
        if (kind == 9 || kind == 10) {  // by height above the plant's lowest point, or by distance from it
            const Vtx& r = vs[root];
            float hi = 0.0f;
            for (uint32_t i : m) {
                const float dx = vs[i].x - r.x, dy = vs[i].y - r.y, dz = vs[i].z - r.z;
                const float a = kind == 9 ? dy : std::sqrt(dx * dx + dy * dy + dz * dz);
                hi = std::max(hi, a);
            }
            for (uint32_t i : m) {
                const float dx = vs[i].x - r.x, dy = vs[i].y - r.y, dz = vs[i].z - r.z;
                const float a = kind == 9 ? dy : std::sqrt(dx * dx + dy * dy + dz * dz);
                amp[i] = amp_byte(hi < 1e-4f ? 0.0f : a / hi);
                phase[i] = ph;
            }
            continue;
        }
        if (kind >= 4 && kind <= 8) {  // along one texture axis within the plant (8: single-triangle leaves only)
            if (kind == 8 && m.size() != 3) continue;
            const bool along_v = kind == 4 || kind == 5 || kind == 8;
            const float sign = (kind == 5 || kind == 7) ? -1.0f : 1.0f;
            float lo = 1e30f, hi = -1e30f;
            for (uint32_t i : m) {
                const float a = sign * (along_v ? vs[i].v : vs[i].u);
                lo = std::min(lo, a);
                hi = std::max(hi, a);
            }
            for (uint32_t i : m) {
                const float a = sign * (along_v ? vs[i].v : vs[i].u);
                amp[i] = amp_byte(hi - lo < 1e-5f ? 0.0f : (a - lo) / (hi - lo));
                phase[i] = ph;
            }
            continue;
        }
        if (kind == 0 && lin) {
            for (uint32_t i : m) {
                float a = lin->a + lin->b * vs[i].u + lin->c * vs[i].v;
                a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
                amp[i] = amp_byte(a);
                phase[i] = ph;
            }
            continue;
        }
        // Along the texture axis that follows the plant's height (its sign from the correlation).
        double best_c = 0.0;
        int best_axis = -1;
        for (int axis = 0; axis < 2; ++axis) {
            double su = 0, sy = 0, suu = 0, syy = 0, suy = 0;
            for (uint32_t i : m) {
                const double u = axis ? vs[i].v : vs[i].u;
                const double y = top ? -vs[i].y : vs[i].y;
                su += u;
                sy += y;
                suu += u * u;
                syy += y * y;
                suy += u * y;
            }
            const double k = static_cast<double>(m.size());
            const double vu = suu - su * su / k, vy = syy - sy * sy / k, cov = suy - su * sy / k;
            if (vu < 1e-10 || vy < 1e-10) continue;
            const double c = cov / std::sqrt(vu * vy);
            if (best_axis < 0 || std::fabs(c) > std::fabs(best_c)) {
                best_axis = axis;
                best_c = c;
            }
        }
        if (best_axis < 0 || std::fabs(best_c) < 0.3) {
            best_axis = 1;  // the texture's top at v = 0
            best_c = -1.0;
        }
        float lo = 1e30f, hi = -1e30f;
        for (uint32_t i : m) {
            const float u = best_axis ? vs[i].v : vs[i].u;
            lo = std::min(lo, u);
            hi = std::max(hi, u);
        }
        for (uint32_t i : m) {
            const float u = best_axis ? vs[i].v : vs[i].u;
            float a = hi - lo < 1e-5f ? 0.0f : (best_c > 0 ? (u - lo) / (hi - lo) : (hi - u) / (hi - lo));
            a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
            amp[i] = amp_byte(a);
            phase[i] = ph;
        }
    }
}

void process_flver(uint8_t* d, const std::string& model) {
    if (std::memcmp(d, "FLVER\0L\0", 8) != 0) return;
    uint32_t version, data_off, data_len;
    int32_t dummies, mats, bones, meshes, vbufs, face_sets, layouts, textures;
    std::memcpy(&version, d + 0x08, 4);
    std::memcpy(&data_off, d + 0x0C, 4);
    std::memcpy(&data_len, d + 0x10, 4);
    if (version != 0x2000C && version != 0x2000D) return;
    const size_t size = static_cast<size_t>(data_off) + data_len;
    if (data_off < 0x80 || size > 0x20000000) return;
    std::memcpy(&dummies, d + 0x14, 4);
    std::memcpy(&mats, d + 0x18, 4);
    std::memcpy(&bones, d + 0x1C, 4);
    std::memcpy(&meshes, d + 0x20, 4);
    std::memcpy(&vbufs, d + 0x24, 4);
    std::memcpy(&face_sets, d + 0x50, 4);
    std::memcpy(&layouts, d + 0x54, 4);
    std::memcpy(&textures, d + 0x58, 4);
    const bool wide = d[0x49] != 0;
    if (dummies < 0 || mats < 0 || bones < 0 || meshes < 0 || vbufs < 0 || face_sets < 0 || layouts < 0 ||
        textures < 0 || mats > 4096 || meshes > 4096 || vbufs > 8192 || face_sets > 65536 || layouts > 4096 ||
        textures > 65536 || dummies > 65536 || bones > 65536)
        return;
    const size_t mat_off = 0x80 + static_cast<size_t>(dummies) * 0x40;
    const size_t mesh_off = mat_off + static_cast<size_t>(mats) * 0x20 + static_cast<size_t>(bones) * 0x80;
    const size_t fs_off = mesh_off + static_cast<size_t>(meshes) * 0x30;
    const size_t vb_off = fs_off + static_cast<size_t>(face_sets) * 0x20;
    const size_t lay_off = vb_off + static_cast<size_t>(vbufs) * 0x20;
    const size_t tex_off = lay_off + static_cast<size_t>(layouts) * 0x10;
    if (tex_off + static_cast<size_t>(textures) * 0x20 > size) return;

    for (int mi = 0; mi < meshes; ++mi) {
        const size_t mo = mesh_off + static_cast<size_t>(mi) * 0x30;
        int32_t mat, fs_count, fs_idx_off, vb_count, vb_idx_off;
        rd(d, size, mo + 4, &mat);
        rd(d, size, mo + 0x20, &fs_count);
        rd(d, size, mo + 0x24, &fs_idx_off);
        rd(d, size, mo + 0x28, &vb_count);
        rd(d, size, mo + 0x2C, &vb_idx_off);
        if (mat < 0 || mat >= mats || fs_count < 1 || vb_count < 1) continue;
        // The material's diffuse texture.
        int32_t tc, ti;
        rd(d, size, mat_off + static_cast<size_t>(mat) * 0x20 + 8, &tc);
        rd(d, size, mat_off + static_cast<size_t>(mat) * 0x20 + 12, &ti);
        std::string diffuse;
        for (int k = 0; k < tc && ti + k < textures; ++k) {
            uint32_t path, type;
            rd(d, size, tex_off + static_cast<size_t>(ti + k) * 0x20, &path);
            rd(d, size, tex_off + static_cast<size_t>(ti + k) * 0x20 + 4, &type);
            if (base_name(d, size, type, wide) == "g_diffuse") {
                diffuse = base_name(d, size, path, wide);
                break;
            }
        }
        if (diffuse.empty() || !g_textures.count(diffuse)) continue;
        const auto pair = g_pairs.find(model + "|" + diffuse);
        if (pair == g_pairs.end()) {
            static std::atomic<int> told{0};
            if (told < 40) {
                ++told;
                LOG_INFO("FoliageSway: %s mesh %d (%s) is foliage in no Remastered model of this name", model.c_str(), mi,
                         diffuse.c_str());
            }
            continue;
        }
        const int kind = pair->second;
        const auto lin_it = g_linear.find(diffuse);
        const foliage_data::Linear* lin = lin_it == g_linear.end() ? nullptr : lin_it->second;
        // Its first vertex buffer and its layout.
        int32_t vbi;
        if (!rd(d, size, static_cast<size_t>(vb_idx_off), &vbi) || vbi < 0 || vbi >= vbufs) continue;
        int32_t layout, vsize, vcount, voff;
        const size_t vbo = vb_off + static_cast<size_t>(vbi) * 0x20;
        rd(d, size, vbo + 4, &layout);
        rd(d, size, vbo + 8, &vsize);
        rd(d, size, vbo + 12, &vcount);
        rd(d, size, vbo + 28, &voff);
        if (layout < 0 || layout >= layouts || vsize <= 0 || vcount <= 0 || vcount > 1000000) continue;
        const size_t vbase = static_cast<size_t>(data_off) + static_cast<size_t>(voff);
        if (vbase + static_cast<size_t>(vsize) * vcount > size) continue;
        int32_t mcount, moff;
        rd(d, size, lay_off + static_cast<size_t>(layout) * 0x10, &mcount);
        rd(d, size, lay_off + static_cast<size_t>(layout) * 0x10 + 12, &moff);
        if (mcount <= 0 || mcount > 32) continue;
        int pos = -1, bi = -1, uv = -1, uv_type = 0;
        bool weighted = false;
        for (int k = 0; k < mcount; ++k) {
            Member mem{};
            const size_t at = static_cast<size_t>(moff) + static_cast<size_t>(k) * 0x14;
            rd(d, size, at + 4, &mem.offset);
            rd(d, size, at + 8, &mem.type);
            rd(d, size, at + 12, &mem.semantic);
            rd(d, size, at + 16, &mem.index);
            if (mem.semantic == 0 && mem.type == 0x2) pos = mem.offset;
            if (mem.semantic == 1) weighted = true;
            if (mem.semantic == 2 && mem.type == 0x11) bi = mem.offset;
            if (mem.semantic == 5 && mem.index == 0 && (mem.type == 0x15 || mem.type == 0x16)) {
                uv = mem.offset;
                uv_type = mem.type;
            }
        }
        if (pos < 0 || bi < 0 || uv < 0 || weighted) continue;
        std::vector<Vtx> vs(static_cast<size_t>(vcount));
        for (int i = 0; i < vcount; ++i) {
            const uint8_t* p = d + vbase + static_cast<size_t>(i) * vsize;
            std::memcpy(&vs[i].x, p + pos, 12);
            int16_t uvv[2];
            std::memcpy(uvv, p + uv, 4);
            vs[i].u = uvv[0] / 1024.0f;
            vs[i].v = uvv[1] / 1024.0f;
        }
        // Its first face set (the full detail): a triangle list or strip.
        int32_t fsi;
        if (!rd(d, size, static_cast<size_t>(fs_idx_off), &fsi) || fsi < 0 || fsi >= face_sets) continue;
        const size_t fso = fs_off + static_cast<size_t>(fsi) * 0x20;
        uint8_t strip = 0;
        int32_t icount, ioff, isize;
        rd(d, size, fso + 4, &strip);
        rd(d, size, fso + 8, &icount);
        rd(d, size, fso + 12, &ioff);
        rd(d, size, fso + 24, &isize);
        if (isize == 0) isize = d[0x48];
        if (icount <= 0 || (isize != 16 && isize != 32)) continue;
        const size_t ibase = static_cast<size_t>(data_off) + static_cast<size_t>(ioff);
        if (ibase + static_cast<size_t>(icount) * (isize / 8) > size) continue;
        std::vector<uint32_t> idx(static_cast<size_t>(icount));
        for (int i = 0; i < icount; ++i) {
            if (isize == 16) {
                uint16_t v16;
                std::memcpy(&v16, d + ibase + static_cast<size_t>(i) * 2, 2);
                idx[i] = v16 == 0xFFFF ? 0xFFFFFFFFu : v16;
            } else {
                std::memcpy(&idx[i], d + ibase + static_cast<size_t>(i) * 4, 4);
            }
        }
        std::vector<uint32_t> tris;
        if (strip) {
            for (size_t i = 0; i + 2 < idx.size(); ++i) {
                const uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
                if (a == 0xFFFFFFFFu || b == 0xFFFFFFFFu || c == 0xFFFFFFFFu || a == b || b == c || a == c) continue;
                if (a >= vs.size() || b >= vs.size() || c >= vs.size()) continue;
                tris.insert(tris.end(), {a, b, c});
            }
        } else {
            for (size_t i = 0; i + 2 < idx.size(); i += 3) {
                if (idx[i] >= vs.size() || idx[i + 1] >= vs.size() || idx[i + 2] >= vs.size()) continue;
                tris.insert(tris.end(), {idx[i], idx[i + 1], idx[i + 2]});
            }
        }
        std::vector<uint8_t> amp, phase;
        mesh_weights(vs, tris, kind, lin, amp, phase);
        for (int i = 0; i < vcount; ++i) {
            uint8_t* b = d + vbase + static_cast<size_t>(i) * vsize + bi;
            b[1] = amp[i];
            b[2] = phase[i];
            b[3] = kMark;
        }
        ++g_meshes;
        g_vertices += vcount;
    }
}

void on_flver_work(uint8_t* data, const void* name) {
    {
        std::string model;
        if (name) {
            const auto* n = static_cast<const uint8_t*>(name);
            const bool wide = n[1] == 0 && n[0] != 0;
            for (int i = 0; i < 32; ++i) {
                const unsigned c = wide ? reinterpret_cast<const uint16_t*>(n)[i] : n[i];
                if (!c) break;
                model.push_back(static_cast<char>(c < 128 ? c : '?'));
            }
            model = lower(model);
            const size_t dot = model.find('.');
            if (dot != std::string::npos) model.resize(dot);
            const size_t slash = model.find_last_of("\\/");
            if (slash != std::string::npos) model = model.substr(slash + 1);
        }
        const int before = g_meshes;
        process_flver(data, model);
        const int models = ++g_models;
        if (g_meshes != before && g_names_logged < 6) {
            ++g_names_logged;
            LOG_INFO("FoliageSway: %s: %d meshes sway (%d so far in %d models)", model.c_str(), g_meshes - before,
                     g_meshes.load(), models);
        }
    }
}

void __cdecl on_flver(uint8_t* data, const void* name) {
    if (!g_on || !data) return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    __try {
        on_flver_work(data, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    QueryPerformanceCounter(&t1);
    g_work_ticks += t1.QuadPart - t0.QuadPart;
}

__declspec(naked) void flver_ctor_entry() {
    __asm {
        pushad
        mov eax, dword ptr [esp + 32 + 8]
        push ecx
        push eax
        call on_flver
        add esp, 8
        popad
        push -1
        push 0x0105ACA4
        jmp dword ptr [g_ctor_back]
    }
}

// --- the vertex shaders (vs_3_0) --------------------------------------------------------------------------------------
constexpr DWORD kEnd = 0x0000FFFF;
constexpr DWORD kTemp = 0, kInput = 1, kConst = 2;
constexpr DWORD kX = 0x00, kY = 0x55, kZ = 0xAA, kW = 0xFF, kXYZW = 0xE4;
constexpr DWORD kWind0 = 250, kWind1 = 251, kK0 = 252, kK1 = 253, kK2 = 254, kK3 = 255;

DWORD reg_type(DWORD p) { return ((p >> 28) & 7) | ((p >> 8) & 0x18); }
DWORD dst(DWORD type, DWORD num, DWORD mask) {
    return 0x80000000 | ((type & 7) << 28) | ((type & 0x18) << 8) | (mask << 16) | num;
}
DWORD src(DWORD type, DWORD num, DWORD swizzle, bool neg = false) {
    return 0x80000000 | ((type & 7) << 28) | ((type & 0x18) << 8) | (swizzle << 16) | (neg ? (1u << 24) : 0) | num;
}
DWORD fbits(float f) {
    DWORD d;
    std::memcpy(&d, &f, 4);
    return d;
}
DWORD op(DWORD code, DWORD len) { return code | (len << 24); }

// A copy of a static model's vertex shader with the sway in front. False when it is not one (no bone indices, bone
// weights, or our registers in use).
std::atomic<int> g_why_logged{0};

bool patch_vs(const DWORD* code, size_t count, std::vector<DWORD>& out) {
    if (count < 2 || code[0] != 0xFFFE0300) return false;
    const char* why = nullptr;
    int pos = -1, bi = -1, max_temp = -1;
    bool weights = false;
    size_t after_dcl = 0, end = 0;
    for (size_t i = 1; i < count;) {
        const DWORD tok = code[i];
        if (tok == kEnd) {
            end = i;
            break;
        }
        const DWORD o = tok & 0xFFFF;
        if (o == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        if (o == 0x19 || o == 0x1A || o == 0x1C || o == 0x1E) why = "subroutines";  // call, callnz, ret, label
        if (o == 0x1F && i + 2 < count) {
            const DWORD usage = code[i + 1], reg = code[i + 2];
            if (reg_type(reg) == kInput) {
                const DWORD u = usage & 0x1F, ui = (usage >> 16) & 0xF;
                if (u == 0 && ui == 0) pos = static_cast<int>(reg & 0x7FF);
                if (u == 2 && ui == 0) bi = static_cast<int>(reg & 0x7FF);
                if (u == 1) weights = true;
            }
            after_dcl = i + 1 + len;
        }
        for (DWORD k = 1; k <= len && i + k < count; ++k) {
            const DWORD p = code[i + k];
            if ((o == 0x51 || o == 0x30 || o == 0x2F) && k > 1) continue;  // def literals
            if (o == 0x1F && k == 1) continue;                               // dcl usage
            const DWORD type = reg_type(p), num = p & 0x7FF;
            if (type == kTemp) max_temp = std::max(max_temp, static_cast<int>(num));
            if (type == kConst && num >= kWind0) why = "constants c250-c255";
        }
        i += 1 + len;
    }
    if (!end || pos < 0 || bi < 0 || weights || !after_dcl) return false;
    if (!why && max_temp + 5 > 31) why = "temporaries";
    if (why) {
        if (g_why_logged < 12) {
            ++g_why_logged;
            LOG_INFO("FoliageSway: a static model vertex shader (%zu tokens) has no swaying copy: %s", count, why);
        }
        return false;
    }
    const DWORD A = max_temp + 1, B = max_temp + 2, C = max_temp + 3, D = max_temp + 4, E = max_temp + 5;
    const DWORD P = static_cast<DWORD>(pos), I = static_cast<DWORD>(bi);
    const float two_pi = 6.28318530718f;
    out.clear();
    out.push_back(code[0]);
    const DWORD defs[] = {
        op(0x51, 5), dst(kConst, kK0, 0xF), fbits(0.1f), fbits(2.0f / 255.0f), fbits(static_cast<float>(kMark)),
        fbits(1.0f / 255.0f),
        op(0x51, 5), dst(kConst, kK1, 0xF), fbits(3.0f), fbits(5.0f), fbits(7.0f), fbits(0.5f),
        op(0x51, 5), dst(kConst, kK2, 0xF), fbits(1.0f), fbits(1.5f), fbits(2.5f), fbits(3.5f),
        op(0x51, 5), dst(kConst, kK3, 0xF), fbits(1.0f / two_pi), fbits(0.5f), fbits(two_pi), fbits(-3.14159265359f),
    };
    out.insert(out.end(), std::begin(defs), std::end(defs));
    out.insert(out.end(), code + 1, code + after_dcl);
    std::vector<DWORD> s = {
        op(0x01, 2), dst(kTemp, A, 0xF), src(kInput, P, kXYZW),                        // mov A, v_pos
        op(0x01, 2), dst(kTemp, A, 8), src(kConst, kK2, kX),                           // A.w = 1
        op(0x29 | (2 << 16), 2), src(kInput, I, kW), src(kConst, kK0, kZ),            // if (bi.w == 165)
        op(0x05, 3), dst(kTemp, B, 1), src(kConst, kWind0, kX), src(kInput, P, kX),     // B.x = W0.x pos.x
        op(0x04, 4), dst(kTemp, B, 1), src(kConst, kWind0, kY), src(kInput, P, kZ), src(kTemp, B, kX),  // + W0.y pos.z
        op(0x05, 3), dst(kTemp, B, 1), src(kTemp, B, kX), src(kConst, kK0, kX),         // * 0.1
        op(0x04, 4), dst(kTemp, B, 1), src(kConst, kWind1, kX), src(kConst, kWind0, kZ), src(kTemp, B, kX),  // + speed t
        op(0x05, 3), dst(kTemp, B, 2), src(kInput, I, kY), src(kConst, kK0, kY),        // B.y = amplitude 0..2
        op(0x05, 3), dst(kTemp, B, 4), src(kInput, I, kZ), src(kConst, kK0, kW),        // B.z = phase 0..1
        op(0x04, 4), dst(kTemp, B, 1), src(kTemp, B, kZ), src(kConst, kWind1, kW), src(kTemp, B, kX),  // p += phase scale
        op(0x05, 3), dst(kTemp, B, 2), src(kTemp, B, kY), src(kConst, kWind1, kY),     // a = amplitude strength
        op(0x05, 3), dst(kTemp, C, 0xF), src(kTemp, B, kX), src(kConst, kK1, kXYZW),   // C = p (3, 5, 7, 0.5)
        op(0x05, 3), dst(kTemp, E, 0xF), src(kTemp, B, kX), src(kConst, kK2, kXYZW),   // E = p (1, 1.5, 2.5, 3.5)
        // both into [-pi, pi) for sincos
        op(0x04, 4), dst(kTemp, C, 0xF), src(kTemp, C, kXYZW), src(kConst, kK3, kX), src(kConst, kK3, kY),
        op(0x13, 2), dst(kTemp, C, 0xF), src(kTemp, C, kXYZW),
        op(0x04, 4), dst(kTemp, C, 0xF), src(kTemp, C, kXYZW), src(kConst, kK3, kZ), src(kConst, kK3, kW),
        op(0x04, 4), dst(kTemp, E, 0xF), src(kTemp, E, kXYZW), src(kConst, kK3, kX), src(kConst, kK3, kY),
        op(0x13, 2), dst(kTemp, E, 0xF), src(kTemp, E, kXYZW),
        op(0x04, 4), dst(kTemp, E, 0xF), src(kTemp, E, kXYZW), src(kConst, kK3, kZ), src(kConst, kK3, kW),
        // h = cos p cos 3p cos 5p cos 7p in B.z
        op(0x25, 2), dst(kTemp, D, 1), src(kTemp, E, kX),
        op(0x01, 2), dst(kTemp, B, 4), src(kTemp, D, kX),
        op(0x25, 2), dst(kTemp, D, 1), src(kTemp, C, kX),
        op(0x05, 3), dst(kTemp, B, 4), src(kTemp, B, kZ), src(kTemp, D, kX),
        op(0x25, 2), dst(kTemp, D, 1), src(kTemp, C, kY),
        op(0x05, 3), dst(kTemp, B, 4), src(kTemp, B, kZ), src(kTemp, D, kX),
        op(0x25, 2), dst(kTemp, D, 1), src(kTemp, C, kZ),
        op(0x05, 3), dst(kTemp, B, 4), src(kTemp, B, kZ), src(kTemp, D, kX),
        // v = cos 0.5p cos 1.5p cos 2.5p cos 3.5p in B.w
        op(0x25, 2), dst(kTemp, D, 1), src(kTemp, C, kW),
        op(0x01, 2), dst(kTemp, B, 8), src(kTemp, D, kX),
        op(0x25, 2), dst(kTemp, D, 1), src(kTemp, E, kY),
        op(0x05, 3), dst(kTemp, B, 8), src(kTemp, B, kW), src(kTemp, D, kX),
        op(0x25, 2), dst(kTemp, D, 1), src(kTemp, E, kZ),
        op(0x05, 3), dst(kTemp, B, 8), src(kTemp, B, kW), src(kTemp, D, kX),
        op(0x25, 2), dst(kTemp, D, 1), src(kTemp, E, kW),
        op(0x05, 3), dst(kTemp, B, 8), src(kTemp, B, kW), src(kTemp, D, kX),
        // offset (h a W0.x, v a W1.z, h a W0.y), taken off the position
        op(0x05, 3), dst(kTemp, B, 4), src(kTemp, B, kZ), src(kTemp, B, kY),            // h a
        op(0x05, 3), dst(kTemp, B, 8), src(kTemp, B, kW), src(kTemp, B, kY),            // v a
        op(0x05, 3), dst(kTemp, C, 1), src(kTemp, B, kZ), src(kConst, kWind0, kX),
        op(0x05, 3), dst(kTemp, C, 4), src(kTemp, B, kZ), src(kConst, kWind0, kY),
        op(0x05, 3), dst(kTemp, C, 2), src(kTemp, B, kW), src(kConst, kWind1, kZ),
        op(0x02, 3), dst(kTemp, A, 7), src(kTemp, A, kXYZW), src(kTemp, C, kXYZW, true),
        op(0x2B, 0),                                                                     // endif
    };
    out.insert(out.end(), s.begin(), s.end());
    const size_t body = out.size();
    out.insert(out.end(), code + after_dcl, code + end);
    for (size_t i = body; i < out.size();) {
        const DWORD tok = out[i];
        const DWORD o = tok & 0xFFFF;
        if (o == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        for (DWORD k = 1; k <= len && i + k < out.size(); ++k) {
            DWORD& p = out[i + k];
            if ((o == 0x51 || o == 0x30 || o == 0x2F) && k > 1) continue;
            if (o == 0x1F && k == 1) continue;
            if (reg_type(p) == kInput && (p & 0x7FF) == P) {
                p = (p & ~(0x70000000u | 0x00001800u | 0x7FFu)) | A;  // v_pos -> rA (type temp is 0)
            }
        }
        i += 1 + len;
    }
    out.push_back(kEnd);
    return true;
}

// --- the device ------------------------------------------------------------------------------------------------------
using CreateVsFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using SetVsFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
CreateVsFn o_create_vs = nullptr;
SetVsFn o_set_vs = nullptr;
PresentFn o_present = nullptr;
// The wind's time for the frame being drawn, taken at each Present: every pass of a frame (the depth pre-pass, the
// colour pass that draws only where its depth equals the pre-pass's, the shadows) has to move a blade the same.
volatile double g_frame_t = 0.0;

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<IDirect3DVertexShader9*, IDirect3DVertexShader9*> g_swaps;
volatile LONG g_gen = 1;
__declspec(align(64)) uint64_t g_cache[4096][2];  // key | gen << 32, swap
std::atomic<int> g_patched{0}, g_refused{0};
LARGE_INTEGER g_freq{}, g_t0{};

void* patch_slot_here(void* object, int slot, void* replacement) {
    void** vtable = *reinterpret_cast<void***>(object);
    void* original = vtable[slot];
    if (original == replacement) return nullptr;
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    vtable[slot] = replacement;
    DWORD ignored = 0;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &ignored);
    return original;
}

size_t shader_size(const DWORD* code) {
    size_t i = 1;
    while (i < 65536) {
        const DWORD tok = code[i];
        if (tok == kEnd) return i + 1;
        const DWORD o = tok & 0xFFFF;
        if (o == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
        } else {
            i += 1 + ((tok >> 24) & 0xF);
        }
    }
    return 0;
}

IDirect3DVertexShader9* make_swap(IDirect3DDevice9* dev, const DWORD* code) {
    IDirect3DVertexShader9* swap = nullptr;
    const size_t n = shader_size(code);
    std::vector<DWORD> patched;
    if (n && patch_vs(code, n, patched)) {
        if (SUCCEEDED(o_create_vs(dev, patched.data(), &swap)) && swap) {
            ++g_patched;
        } else {
            swap = nullptr;
            ++g_refused;
        }
    }
    return swap;
}

IDirect3DVertexShader9* make_swap_guarded(IDirect3DDevice9* dev, const DWORD* code) {
    __try {
        return make_swap(dev, code);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

HRESULT WINAPI hk_create_vs(IDirect3DDevice9* dev, const DWORD* code, IDirect3DVertexShader9** out) {
    const HRESULT hr = o_create_vs(dev, code, out);
    if (FAILED(hr) || !out || !*out || !code) return hr;
    IDirect3DVertexShader9* swap = g_on ? make_swap_guarded(dev, code) : nullptr;
    AcquireSRWLockExclusive(&g_lock);
    if (swap) {
        g_swaps[*out] = swap;
    } else {
        g_swaps.erase(*out);
    }
    InterlockedIncrement(&g_gen);
    ReleaseSRWLockExclusive(&g_lock);
    return hr;
}

HRESULT WINAPI hk_present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_frame_t = static_cast<double>(now.QuadPart - g_t0.QuadPart) / static_cast<double>(g_freq.QuadPart);
    return o_present(dev, a, b, w, r);
}

void send_wind(IDirect3DDevice9* dev) {
    double t = g_frame_t;
    const float speed = g_speed;
    if (speed > 1e-6f) {
        const double period = 4.0 * 3.14159265358979 / speed;  // every cosine back where it started
        t = std::fmod(t, period);
    }
    // Remastered's W0 = (direction x, z, time, previous time: not drawn here) and W1 = (speed, strength, verticality,
    // phase scale).
    const float c[8] = {kDirX, kDirZ, static_cast<float>(t), 0.0f, speed, g_strength, kVerticality, kPhaseScale};
    dev->SetVertexShaderConstantF(kWind0, c, 2);
}

HRESULT WINAPI hk_set_vs(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs) {
    if (vs && g_on) {
        const uint32_t key = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(vs));
        uint64_t* slot = g_cache[(key * 2654435761u) >> 20];
        const uint32_t gen = static_cast<uint32_t>(g_gen);
        IDirect3DVertexShader9* swap = nullptr;
        if (slot[0] == (key | (static_cast<uint64_t>(gen) << 32))) {
            swap = reinterpret_cast<IDirect3DVertexShader9*>(static_cast<uintptr_t>(slot[1]));
        } else {
            AcquireSRWLockShared(&g_lock);
            const auto it = g_swaps.find(vs);
            if (it != g_swaps.end()) swap = it->second;
            ReleaseSRWLockShared(&g_lock);
            slot[1] = reinterpret_cast<uintptr_t>(swap);
            slot[0] = key | (static_cast<uint64_t>(gen) << 32);
        }
        if (swap) {
            send_wind(dev);
            return o_set_vs(dev, swap);
        }
    }
    return o_set_vs(dev, vs);
}

}  // namespace

void foliage_configure(bool on, int strength_percent, int speed_percent) {
    g_on = on;
    foliage_set_strength(strength_percent);
    foliage_set_speed(speed_percent);
}

void foliage_set_strength(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 1000) percent = 1000;
    g_strength = 0.1f * static_cast<float>(percent) / 100.0f;
}

void foliage_set_speed(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 1000) percent = 1000;
    g_speed = 0.3f * static_cast<float>(percent) / 100.0f;
}

bool foliage_install() {
    if (!g_on) return false;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    for (const auto& p : foliage_data::kPairs) {
        g_pairs[std::string(p.model) + "|" + p.texture] = p.kind;
        g_textures.insert(p.texture);
    }
    for (const auto& l : foliage_data::kLinear) g_linear[l.texture] = &l;
    auto* site = reinterpret_cast<uint8_t*>(kFlverCtor);
    if (std::memcmp(site, kCtorHead, sizeof(kCtorHead)) != 0) {
        LOG_ERROR("FoliageSway: the model loader at %08X does not match this build; foliage does not sway",
                  static_cast<unsigned>(kFlverCtor));
        g_on = false;
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(site, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
    site[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&flver_ctor_entry) - (kFlverCtor + 5));
    std::memcpy(site + 1, &rel, 4);
    site[5] = 0x90;
    site[6] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(site, 7, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 7);
    LOG_INFO("FoliageSway: grass, flowers, bushes, ivy and leaves sway in the wind as in Remastered (%zu models' "
             "foliage, strength %.0f%%, speed %.0f%%)",
             g_pairs.size(), g_strength / 0.1f * 100.0f, g_speed / 0.3f * 100.0f);
    return true;
}

void foliage_hook_device(IDirect3DDevice9* dev) {
    if (!g_on || !dev) return;
    void* p = patch_slot_here(dev, 91, reinterpret_cast<void*>(&hk_create_vs));
    if (p && !o_create_vs) o_create_vs = reinterpret_cast<CreateVsFn>(p);
    p = patch_slot_here(dev, 92, reinterpret_cast<void*>(&hk_set_vs));
    if (p && !o_set_vs) o_set_vs = reinterpret_cast<SetVsFn>(p);
    p = patch_slot_here(dev, 17, reinterpret_cast<void*>(&hk_present));
    if (p && !o_present) o_present = reinterpret_cast<PresentFn>(p);
}

void foliage_log_stats() {
    const double ms = g_freq.QuadPart ? static_cast<double>(g_work_ticks.load()) * 1000.0 / g_freq.QuadPart : 0.0;
    LOG_INFO("FoliageSway: %d models loaded, %d meshes sway (%d vertices, %.1f ms on the loading threads); %d vertex "
             "shaders have a swaying copy, %d refused by the device",
             g_models.load(), g_meshes.load(), g_vertices.load(), ms, g_patched.load(), g_refused.load());
}
