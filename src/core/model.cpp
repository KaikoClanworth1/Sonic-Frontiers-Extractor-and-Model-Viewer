#include "model.h"
#include "mirage.h"
#include <unordered_map>

namespace sf {

// offset conventions (see model.py / frontiers-model-endianness):
//  needle_le : off32 little-endian, self-relative (ptr = field + value)
//  mirage_be : off32 big-endian, base-relative to sample-chunk nodes base (header+0x10)
struct OffCtx {
    const uint8_t* d;
    size_t n;
    bool mirage_be;
    size_t nbase;
    size_t off(size_t pos) const {
        if (mirage_be) { uint32_t v = u32be(d, pos); return v ? nbase + v : 0; }
        uint32_t v = u32le(d, pos); return v ? pos + v : 0;
    }
    uint32_t count(size_t pos) const { return u32be(d, pos); }  // counts always BE
};

static int _sign10(uint32_t v) { return (int)(v >= 0x200 ? (int)v - 0x400 : (int)v); }

static Vec4 decode_vec(const uint8_t* d, size_t o, uint32_t fmt) {
    Vec4 r;
    switch (fmt) {
    case 0x2A23B9: r.x = f32be(d, o); r.y = f32be(d, o + 4); r.z = f32be(d, o + 8); return r;
    case 0x1A23A6: r.x = f32be(d, o); r.y = f32be(d, o + 4); r.z = f32be(d, o + 8); r.w = f32be(d, o + 12); return r;
    case 0x2C23A5: r.x = f32be(d, o); r.y = f32be(d, o + 4); return r;
    case 0x2C83A4: r.x = f32be(d, o); return r;
    case 0x2C235F: r.x = f16(u16be(d, o)); r.y = f16(u16be(d, o + 2)); return r;
    case 0x1A2360: r.x = f16(u16be(d, o)); r.y = f16(u16be(d, o + 2)); r.z = f16(u16be(d, o + 4)); r.w = f16(u16be(d, o + 6)); return r;
    case 0x1A2086: r.x = d[o] / 255.f; r.y = d[o + 1] / 255.f; r.z = d[o + 2] / 255.f; r.w = d[o + 3] / 255.f; return r;
    case 0x182886: r.x = d[o + 1] / 255.f; r.y = d[o + 2] / 255.f; r.z = d[o + 3] / 255.f; r.w = d[o] / 255.f; return r;
    case 0x2A2187: case 0x2A2087: {
        uint32_t v = u32be(d, o);
        r.x = _sign10(v & 0x3FF) / 511.f; r.y = _sign10((v >> 10) & 0x3FF) / 511.f; r.z = _sign10((v >> 20) & 0x3FF) / 511.f; return r; }
    case 0x1A2187: case 0x1A2087: {
        uint32_t v = u32be(d, o);
        r.x = _sign10(v & 0x3FF) / 511.f; r.y = _sign10((v >> 10) & 0x3FF) / 511.f; r.z = _sign10((v >> 20) & 0x3FF) / 511.f; return r; }
    default:
        r.x = d[o] / 255.f; r.y = d[o + 1] / 255.f; r.z = d[o + 2] / 255.f; r.w = d[o + 3] / 255.f; return r;
    }
}

static std::array<uint32_t, 4> decode_ints(const uint8_t* d, size_t o, uint32_t fmt) {
    std::array<uint32_t, 4> r{0, 0, 0, 0};
    if (fmt == 0x1A225A) { r[0] = u16be(d, o); r[1] = u16be(d, o + 2); r[2] = u16be(d, o + 4); r[3] = u16be(d, o + 6); }
    else if (fmt == 0x2C2259) { r[0] = u16be(d, o); r[1] = u16be(d, o + 2); }
    else { r[0] = d[o]; r[1] = d[o + 1]; r[2] = d[o + 2]; r[3] = d[o + 3]; }
    return r;
}

enum { POSITION = 0, BLEND_WEIGHT = 1, BLEND_INDICES = 2, NORMAL = 3, TEXCOORD = 5, COLOR = 10 };

struct VElem { uint16_t stream, offset; uint32_t format; uint8_t method, type, index; };

static std::vector<VElem> read_velems(const uint8_t* d, size_t ptr) {
    std::vector<VElem> v;
    size_t p = ptr;
    while (v.size() < 64) {
        uint32_t fmt = u32be(d, p + 4);
        if (fmt == 0xFFFFFFFF) break;
        VElem e; e.stream = u16be(d, p); e.offset = u16be(d, p + 2);
        e.format = fmt; e.method = d[p + 8]; e.type = d[p + 9]; e.index = d[p + 10];
        v.push_back(e); p += 12;
    }
    return v;
}

static std::vector<std::array<uint32_t, 3>> strip_to_faces(const std::vector<uint16_t>& s) {
    std::vector<std::array<uint32_t, 3>> out;
    std::vector<std::vector<uint16_t>> subs;
    std::vector<uint16_t> cur;
    for (uint16_t v : s) { if (v == 0xFFFF) { if (!cur.empty()) { subs.push_back(cur); cur.clear(); } } else cur.push_back(v); }
    if (!cur.empty()) subs.push_back(cur);
    for (auto& sub : subs) {
        bool flip = false;
        for (size_t i = 0; i + 2 < sub.size(); i++) {
            uint16_t a = sub[i], b = sub[i + 1], c = sub[i + 2];
            if (a != b && b != c && a != c) out.push_back(flip ? std::array<uint32_t,3>{a, c, b} : std::array<uint32_t,3>{a, b, c});
            flip = !flip;
        }
    }
    return out;
}

static std::vector<std::array<uint32_t, 3>> list_to_faces(const std::vector<uint16_t>& idx) {
    std::vector<std::array<uint32_t, 3>> out;
    for (size_t i = 0; i + 2 < idx.size(); i += 3) out.push_back({(uint32_t)idx[i + 2], (uint32_t)idx[i + 1], (uint32_t)idx[i]});
    return out;
}

// Fraction of edges shared by exactly 2 triangles (a coherent surface -> ~1).
static double manifold_score(const std::vector<std::array<uint32_t, 3>>& faces) {
    if (faces.empty()) return 0.0;
    std::unordered_map<uint64_t, int> ec;
    ec.reserve(faces.size() * 3);
    for (auto& f : faces)
        for (int e = 0; e < 3; e++) {
            uint32_t i = f[e], j = f[(e + 1) % 3];
            uint64_t k = i < j ? ((uint64_t)i << 32 | j) : ((uint64_t)j << 32 | i);
            ec[k]++;
        }
    size_t shared = 0;
    for (auto& kv : ec) if (kv.second == 2) shared++;
    return (double)shared / ec.size();
}

// The Topology node (always 3) is unreliable; pick list vs strip by manifold coherence.
static std::vector<std::array<uint32_t, 3>> choose_faces(const std::vector<uint16_t>& faces) {
    auto ls = list_to_faces(faces);
    auto st = strip_to_faces(faces);
    return manifold_score(ls) >= manifold_score(st) ? ls : st;
}

static bool parse_mesh(const OffCtx& c, size_t mesh_off, int bone_width, uint32_t topology, Mesh& msh) {
    const uint8_t* d = c.d; size_t n = c.n;
    auto inb = [&](size_t p, size_t need) { return p != 0 && p + need <= n; };
    msh.material = cstr(d, c.n, c.off(mesh_off + 0x00));
    uint32_t face_count = c.count(mesh_off + 0x04);
    size_t faces_ptr = c.off(mesh_off + 0x08);
    uint32_t vtx_count = u32be(d, mesh_off + 0x0C);
    uint32_t vtx_size = u32be(d, mesh_off + 0x10);
    size_t vtx_ptr = c.off(mesh_off + 0x14);
    size_t velem_ptr = c.off(mesh_off + 0x18);
    uint32_t bni_count = c.count(mesh_off + 0x1C);
    size_t bni_ptr = c.off(mesh_off + 0x20);

    if (vtx_count > 5000000 || face_count > 20000000 || vtx_size == 0 || vtx_size > 4096) return false;
    if (!inb(vtx_ptr, (size_t)vtx_count * vtx_size)) return false;
    if (face_count && !inb(faces_ptr, (size_t)face_count * 2)) return false;
    if (!inb(velem_ptr, 12)) return false;
    if (bni_count > 100000) bni_count = 0;

    std::vector<uint32_t> palette(bni_count);
    for (uint32_t i = 0; i < bni_count; i++) {
        if (!inb(bni_ptr + i * (bone_width == 2 ? 2 : 1), bone_width == 2 ? 2 : 1)) break;
        palette[i] = bone_width == 2 ? u16be(d, bni_ptr + i * 2) : d[bni_ptr + i];
    }

    auto elems = read_velems(d, velem_ptr);
    const VElem *pos = nullptr, *nrm = nullptr, *col = nullptr, *bidx = nullptr, *bw = nullptr;
    std::vector<const VElem*> tex;
    int max_uv = 0;
    for (auto& e : elems) {
        if (e.type == POSITION && !pos) pos = &e;
        else if (e.type == NORMAL && !nrm) nrm = &e;
        else if (e.type == COLOR && !col) col = &e;
        else if (e.type == BLEND_INDICES && !bidx) bidx = &e;
        else if (e.type == BLEND_WEIGHT && !bw) bw = &e;
        else if (e.type == TEXCOORD) { tex.push_back(&e); if (e.index + 1 > max_uv) max_uv = e.index + 1; }
    }
    msh.uvs.resize(max_uv);

    for (uint32_t i = 0; i < vtx_count; i++) {
        size_t vb = vtx_ptr + (size_t)i * vtx_size;
        if (pos) { Vec4 v = decode_vec(d, vb + pos->offset, pos->format); msh.positions.push_back({v.x, v.y, v.z}); }
        if (nrm) { Vec4 v = decode_vec(d, vb + nrm->offset, nrm->format); msh.normals.push_back({v.x, v.y, v.z}); }
        for (auto* e : tex) { Vec4 v = decode_vec(d, vb + e->offset, e->format); if (e->index < (int)msh.uvs.size()) msh.uvs[e->index].push_back({v.x, 1.f - v.y}); }
        if (col) { Vec4 v = decode_vec(d, vb + col->offset, col->format); msh.colors.push_back(v); }
        if (bidx && bw) {
            auto li = decode_ints(d, vb + bidx->offset, bidx->format);
            Vec4 wv = decode_vec(d, vb + bw->offset, bw->format);
            std::array<uint32_t, 4> gi;
            for (int k = 0; k < 4; k++) gi[k] = li[k] < palette.size() ? palette[li[k]] : 0;
            msh.bone_indices.push_back(gi);
            msh.weights.push_back({wv.x, wv.y, wv.z, wv.w});
        }
    }

    std::vector<uint16_t> faces(face_count);
    for (uint32_t i = 0; i < face_count; i++) faces[i] = u16be(d, faces_ptr + i * 2);
    msh.faces = choose_faces(faces);
    (void)topology;
    return true;
}

bool parse_model(const Bytes& data, Model& model, std::string* err, int is_terrain_in) {
    const uint8_t* d = data.data();
    size_t n = data.size();
    auto blocks = sc_find_model_blocks(d, n);
    if (blocks.empty()) { if (err) *err = "no model blocks"; return false; }
    size_t header_off = blocks[0];
    size_t limit;
    auto roots = sc_parse(d, n, header_off, limit);
    const SCNode* ctx = sc_find(roots, "Contexts");
    if (!ctx) { if (err) *err = "no Contexts node"; return false; }
    model.version = ctx->value;
    size_t base = ctx->data_off();

    // detect offset mode
    size_t nbase = header_off + 0x10;
    OffCtx cle{d, n, false, 0}, cbe{d, n, true, nbase};
    size_t le = base + 0x04 + u32le(d, base + 0x04);
    size_t be = nbase + u32be(d, base + 0x04);
    auto ok = [&](size_t p) { return p >= 0x10 && p < limit; };
    OffCtx c = cle;
    if (ok(le) && !ok(be)) c = cle;
    else if (ok(be) && !ok(le)) c = cbe;
    else if (header_off > 0 || (n >= 8 && memcmp(d, "NEDARCV1", 8) == 0)) c = cle;
    else c = cbe;

    const SCNode* topo = sc_find(roots, "Topology");
    uint32_t topology = topo ? topo->value : 4;

    // terrain detection
    bool is_terrain;
    if (is_terrain_in >= 0) is_terrain = is_terrain_in != 0;
    else {
        uint32_t nc = u32be(d, base + 0x10);
        size_t np = c.off(base + 0x14);
        is_terrain = !(nc > 0 && nc < 4096 && np && np >= 0x10 && np < limit);
    }
    model.is_terrain = is_terrain;

    // bounds guard: reject absurd offsets/counts so a mis-parse fails gracefully vs crashing
    auto inb = [&](size_t p, size_t need) { return p != 0 && p + need <= n; };

    uint32_t group_count = c.count(base + 0x00);
    size_t groups_ptr = c.off(base + 0x04);
    int bone_width = model.version >= 6 ? 2 : 1;
    if (group_count > 100000) { if (err) *err = "bad group count"; return false; }

    if (!is_terrain) {
        uint32_t node_count = u32be(d, base + 0x10);
        size_t nodes_ptr = c.off(base + 0x14);
        if (node_count > 100000) node_count = 0;
        for (uint32_t i = 0; i < node_count; i++) {
            if (!inb(nodes_ptr + i * 4, 4)) break;
            size_t node_off = c.off(nodes_ptr + i * 4);
            if (!inb(node_off, 8)) { model.node_names.push_back(""); model.node_parents.push_back(-1); continue; }
            model.node_parents.push_back(s32be(d, node_off + 0x00));
            model.node_names.push_back(cstr(d, n, c.off(node_off + 0x04)));
        }
    }

    for (uint32_t g = 0; g < group_count; g++) {
        if (!inb(groups_ptr + g * 4, 4)) break;
        size_t grp = c.off(groups_ptr + g * 4);
        if (!inb(grp, 0x18)) continue;
        for (size_t slot : {(size_t)0x00, (size_t)0x08, (size_t)0x10}) {
            uint32_t mc = c.count(grp + slot);
            size_t meshes_ptr = c.off(grp + slot + 4);
            if (mc > 100000) continue;
            for (uint32_t k = 0; k < mc; k++) {
                if (!inb(meshes_ptr + k * 4, 4)) break;
                size_t mo = c.off(meshes_ptr + k * 4);
                if (!inb(mo, 0x2C)) continue;
                Mesh msh;
                if (parse_mesh(c, mo, bone_width, topology, msh)) model.meshes.push_back(std::move(msh));
            }
        }
    }
    return true;
}

} // namespace sf
