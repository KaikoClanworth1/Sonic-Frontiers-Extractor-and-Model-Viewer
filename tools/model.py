"""Sonic Frontiers .model / .terrain-model parser (Hedgehog Engine 2, big-endian
sample-chunk data inside a Needle NEDARCV1 wrapper).

Produces a list of Mesh objects with positions/normals/uvs/colors/skin and the
model's node (bone) name list. Verified against real extracted Frontiers models.
"""
import struct
import hh_mirage as m


# ---- offset helpers ----
# Frontiers ships .model files in TWO offset conventions (counts are always BIG-endian):
#   * "needle_le" (NEDARCV1-wrapped models, e.g. amy/sonic): off32 is LITTLE-endian and
#     SELF-RELATIVE  (pointer = &field + value).  [HedgeLib off32 semantics]
#   * "mirage_be" (bare sample-chunk at file 0, e.g. eggman): off32 is BIG-endian and
#     relative to the sample-chunk nodes base (header+0x10).  [legacy Mirage]
# _MODE / _NBASE are set per-file by parse_model().
_MODE = "needle_le"
_NBASE = 0


def _u32le(d, pos):
    return struct.unpack_from("<I", d, pos)[0]


def off32(d, pos, base=0):
    if _MODE == "mirage_be":
        v = m.u32be(d, pos)
        return 0 if v == 0 else _NBASE + v
    v = _u32le(d, pos)             # needle_le: self-relative little-endian
    return 0 if v == 0 else pos + v


def arr32(d, pos, base=0):        # arr32 = { u32 count (BE); off32 data }
    return m.u32be(d, pos), off32(d, pos + 4)


# ---- vertex format decoding (D3DDECLTYPE codes) ----
def _half(u16):
    return struct.unpack("<e", struct.pack("<H", u16))[0]


def _sign10(v):
    return v - 0x400 if v >= 0x200 else v


def decode_vec(d, o, fmt):
    """Decode a vertex element to a float tuple (for pos/normal/uv/color/weight)."""
    if fmt == 0x2A23B9:   # float3
        return struct.unpack_from(">fff", d, o)
    if fmt == 0x1A23A6:   # float4
        return struct.unpack_from(">ffff", d, o)
    if fmt == 0x2C23A5:   # float2
        return struct.unpack_from(">ff", d, o)
    if fmt == 0x2C83A4:   # float1
        return (struct.unpack_from(">f", d, o)[0],)
    if fmt == 0x2C235F:   # float16_2 (UV)
        a, b = struct.unpack_from(">HH", d, o)
        return (_half(a), _half(b))
    if fmt == 0x1A2360:   # float16_4
        a, b, c, e = struct.unpack_from(">HHHH", d, o)
        return (_half(a), _half(b), _half(c), _half(e))
    if fmt == 0x1A2086:   # ubyte4_norm (weights/colors)
        b0, b1, b2, b3 = d[o], d[o + 1], d[o + 2], d[o + 3]
        return (b0 / 255, b1 / 255, b2 / 255, b3 / 255)
    if fmt == 0x182886:   # d3d_color (A,R,G,B in the u32; big-endian bytes = A R G B)
        a, r, g, b = d[o], d[o + 1], d[o + 2], d[o + 3]
        return (r / 255, g / 255, b / 255, a / 255)
    if fmt in (0x2A2187, 0x2A2087):  # dec3_norm / udec3_norm  (packed 10/10/10 normal)
        v = m.u32be(d, o)
        x = _sign10(v & 0x3FF) / 511.0
        y = _sign10((v >> 10) & 0x3FF) / 511.0
        z = _sign10((v >> 20) & 0x3FF) / 511.0
        return (x, y, z)
    if fmt in (0x1A2187, 0x1A2087):  # dec4_norm (10/10/10/2)
        v = m.u32be(d, o)
        x = _sign10(v & 0x3FF) / 511.0
        y = _sign10((v >> 10) & 0x3FF) / 511.0
        z = _sign10((v >> 20) & 0x3FF) / 511.0
        return (x, y, z)
    # fallback: treat as 4 raw bytes normalized
    b0, b1, b2, b3 = d[o], d[o + 1], d[o + 2], d[o + 3]
    return (b0 / 255, b1 / 255, b2 / 255, b3 / 255)


def decode_ints(d, o, fmt):
    """Decode a blend-index element to a tuple of ints."""
    if fmt == 0x1A225A:   # ushort4
        return struct.unpack_from(">HHHH", d, o)
    if fmt in (0x1A2286, 0x1A2086, 0x182886):  # ubyte4 (raw)
        return (d[o], d[o + 1], d[o + 2], d[o + 3])
    if fmt == 0x2C2259:   # ushort2
        return struct.unpack_from(">HH", d, o)
    return (d[o], d[o + 1], d[o + 2], d[o + 3])


FMT_SIZE = {
    0x2C83A4: 4, 0x2C23A5: 8, 0x2A23B9: 12, 0x1A23A6: 16,
    0x2C235F: 4, 0x1A2360: 8, 0x182886: 4, 0x1A2286: 4, 0x1A2386: 4,
    0x1A2086: 4, 0x1A2186: 4, 0x2C2359: 4, 0x1A235A: 8, 0x2C2259: 4,
    0x1A225A: 8, 0x2A2187: 4, 0x2A2087: 4, 0x1A2187: 4, 0x1A2087: 4,
}

# vertex semantic types (D3DDECLUSAGE)
POSITION, BLEND_WEIGHT, BLEND_INDICES, NORMAL, TEXCOORD, TANGENT, BINORMAL, COLOR = 0, 1, 2, 3, 5, 6, 7, 10


class Mesh:
    def __init__(self):
        self.material = ""
        self.positions = []
        self.normals = []
        self.uvs = []            # list of channels: uvs[ch] = [(u,v),...]
        self.colors = []
        self.bone_indices = []   # per-vertex tuple of GLOBAL node indices
        self.weights = []        # per-vertex tuple of floats
        self.faces = []          # list of (a,b,c)


class Model:
    def __init__(self):
        self.version = 0
        self.node_names = []     # model.nodes[i].name  (bone names)
        self.node_parents = []
        self.meshes = []


def _read_vertex_elements(d, ptr):
    elems = []
    p = ptr
    while True:
        fmt = m.u32be(d, p + 4)
        if fmt == 0xFFFFFFFF:
            break
        elems.append({
            "stream": m.u16be(d, p), "offset": m.u16be(d, p + 2),
            "format": fmt, "method": d[p + 8], "type": d[p + 9], "index": d[p + 10],
        })
        p += 12
        if len(elems) > 64:
            break
    return elems


def _parse_mesh(d, mesh_off, base, bone_index_width):
    msh = Mesh()
    mat_name_off = off32(d, mesh_off + 0x00, base)
    msh.material = m.cstr(d, mat_name_off)
    face_count, faces_ptr = arr32(d, mesh_off + 0x04, base)
    vtx_count = m.u32be(d, mesh_off + 0x0C)
    vtx_size = m.u32be(d, mesh_off + 0x10)
    vtx_ptr = off32(d, mesh_off + 0x14, base)
    velem_ptr = off32(d, mesh_off + 0x18, base)
    bni_count, bni_ptr = arr32(d, mesh_off + 0x1C, base)

    # bone node index palette (8-bit for r1/v5, 16-bit for r2/v6)
    palette = []
    for i in range(bni_count):
        if bone_index_width == 2:
            palette.append(m.u16be(d, bni_ptr + i * 2))
        else:
            palette.append(d[bni_ptr + i])

    elems = _read_vertex_elements(d, velem_ptr)
    by_type = {}
    for e in elems:
        by_type.setdefault(e["type"], []).append(e)

    max_uv = 0
    for e in elems:
        if e["type"] == TEXCOORD:
            max_uv = max(max_uv, e["index"] + 1)
    msh.uvs = [[] for _ in range(max_uv)]

    for i in range(vtx_count):
        vbase = vtx_ptr + i * vtx_size
        for e in by_type.get(POSITION, [])[:1]:
            v = decode_vec(d, vbase + e["offset"], e["format"])
            msh.positions.append((v[0], v[1], v[2]))
        for e in by_type.get(NORMAL, [])[:1]:
            v = decode_vec(d, vbase + e["offset"], e["format"])
            msh.normals.append((v[0], v[1], v[2]))
        for e in by_type.get(TEXCOORD, []):
            v = decode_vec(d, vbase + e["offset"], e["format"])
            if e["index"] < len(msh.uvs):
                msh.uvs[e["index"]].append((v[0], 1.0 - v[1]))   # flip V
        for e in by_type.get(COLOR, [])[:1]:
            v = decode_vec(d, vbase + e["offset"], e["format"])
            msh.colors.append(v)
        # skin
        idx_e = by_type.get(BLEND_INDICES, [])
        w_e = by_type.get(BLEND_WEIGHT, [])
        if idx_e and w_e:
            local = decode_ints(d, vbase + idx_e[0]["offset"], idx_e[0]["format"])
            wv = decode_vec(d, vbase + w_e[0]["offset"], w_e[0]["format"])
            gi = tuple(palette[li] if li < len(palette) else 0 for li in local)
            msh.bone_indices.append(gi)
            msh.weights.append(tuple(wv))

    # faces (topology decided by caller via face list interpretation)
    faces = [m.u16be(d, faces_ptr + i * 2) for i in range(face_count)]
    msh._raw_faces = faces
    msh._vtx_count = vtx_count
    return msh


def strip_to_faces(strip_indices):
    out = []
    substrips = []
    cur = []
    for v in strip_indices:
        if v == 0xFFFF:
            if cur:
                substrips.append(cur); cur = []
        else:
            cur.append(v)
    if cur:
        substrips.append(cur)
    for s in substrips:
        flip = False
        for i in range(len(s) - 2):
            a, b, c = s[i], s[i + 1], s[i + 2]
            if a != b and b != c and a != c:
                out.append((a, c, b) if flip else (a, b, c))
            flip = not flip
    return out


def list_to_faces(indices):
    out = []
    for i in range(0, len(indices) - 2, 3):
        out.append((indices[i + 2], indices[i + 1], indices[i]))
    return out


def _detect_mode(data, base, header_off, limit):
    """Pick the offset convention by testing which yields an in-range meshGroups ptr."""
    global _MODE, _NBASE
    nbase = header_off + 0x10
    # needle_le candidate
    le = base + 0x04 + _u32le(data, base + 0x04)
    # mirage_be candidate
    be = nbase + m.u32be(data, base + 0x04)
    def ok(p):
        return 0x10 <= p < limit
    if ok(le) and not ok(be):
        return "needle_le", 0
    if ok(be) and not ok(le):
        return "mirage_be", nbase
    # both plausible: wrapper presence decides (NEDARC -> needle native LE)
    if header_off > 0 or data[:8] == b"NEDARCV1":
        return "needle_le", 0
    return "mirage_be", nbase


def parse_model(data, is_terrain=None):
    global _MODE, _NBASE
    blocks = m.find_model_blocks(data)
    if not blocks:
        raise ValueError("no model blocks found")
    header_off = blocks[0]                       # LOD0
    roots, limit = m.parse_sample_chunk(data, header_off)
    ctx = m.find_node(roots, "Contexts")
    if not ctx:
        raise ValueError("no Contexts node")
    version = ctx.value
    base = ctx.data_off                          # data base for all offsets

    _MODE, _NBASE = _detect_mode(data, base, header_off, limit)

    topo_node = m.find_node(roots, "Topology")
    topology = topo_node.value if topo_node else 4   # default strips

    model = Model()
    model.version = version

    # Terrain models (raw_terrain_model_v5r1/r2) have no node list: { meshGroups; name; [flags] }.
    # Skeletal models (v5/v6) have { meshGroups; unknown1; nodeCount; nodes; nodeMatrices; bounds }.
    # Auto-detect terrain if not told: skeletal nodeCount@0x10 must be sane and its nodes ptr in-range.
    if is_terrain is None:
        nc = m.u32be(data, base + 0x10)
        np = off32(data, base + 0x14, base)
        is_terrain = not (0 < nc < 4096 and np and 0x10 <= np < limit)

    group_count, groups_ptr = arr32(data, base + 0x00, base)
    bone_index_width = 2 if version >= 6 else 1

    if not is_terrain:
        node_count = m.u32be(data, base + 0x10)
        nodes_ptr = off32(data, base + 0x14, base)   # -> array of off32 -> raw_node
        for i in range(node_count):
            node_off = off32(data, nodes_ptr + i * 4, base)
            if not node_off:
                model.node_names.append(""); model.node_parents.append(-1); continue
            parent = m.s32be(data, node_off + 0x00)
            name_off = off32(data, node_off + 0x04, base)
            model.node_names.append(m.cstr(data, name_off))
            model.node_parents.append(parent)

    # mesh groups -> slots (opaq/trans/punch) -> meshes
    for g in range(group_count):
        grp_off = off32(data, groups_ptr + g * 4, base)
        if not grp_off:
            continue
        for slot_rel in (0x00, 0x08, 0x10):      # opaq, trans, punch
            mc, meshes_ptr = arr32(data, grp_off + slot_rel, base)
            for k in range(mc):
                mesh_off = off32(data, meshes_ptr + k * 4, base)
                if not mesh_off:
                    continue
                msh = _parse_mesh(data, mesh_off, base, bone_index_width)
                if topology == 3:
                    msh.faces = list_to_faces(msh._raw_faces)
                else:
                    msh.faces = strip_to_faces(msh._raw_faces)
                model.meshes.append(msh)
    return model


def sanity(model):
    import math
    issues = []
    for mi, msh in enumerate(model.meshes):
        for (x, y, z) in msh.positions[:50]:
            if not all(map(math.isfinite, (x, y, z))) or max(abs(x), abs(y), abs(z)) > 1e4:
                issues.append(f"mesh{mi} bad pos {(x,y,z)}"); break
        vc = len(msh.positions)
        for f in msh.faces[:200]:
            if any(idx >= vc for idx in f):
                issues.append(f"mesh{mi} face idx OOB {f} >= {vc}"); break
    return issues


if __name__ == "__main__":
    import sys
    data = open(sys.argv[1], "rb").read()
    mdl = parse_model(data)
    print(f"model version={mdl.version}  nodes(bones)={len(mdl.node_names)}  meshes={len(mdl.meshes)}")
    tv = tf = 0
    for i, msh in enumerate(mdl.meshes):
        tv += len(msh.positions); tf += len(msh.faces)
        print(f"  mesh{i:2} mat={msh.material:22} verts={len(msh.positions):6} faces={len(msh.faces):6} "
              f"uv_ch={len(msh.uvs)} col={'Y' if msh.colors else 'n'} skin={'Y' if msh.weights else 'n'}")
    print(f"  TOTAL verts={tv} faces={tf}")
    iss = sanity(mdl)
    print("SANITY:", "OK" if not iss else iss[:8])
    # show a few positions
    if mdl.meshes and mdl.meshes[0].positions:
        print("  sample pos:", [tuple(round(c,3) for c in p) for p in mdl.meshes[0].positions[:3]])
        if mdl.meshes[0].bone_indices:
            print("  sample skin idx:", mdl.meshes[0].bone_indices[0], "w:", [round(w,2) for w in mdl.meshes[0].weights[0]])
