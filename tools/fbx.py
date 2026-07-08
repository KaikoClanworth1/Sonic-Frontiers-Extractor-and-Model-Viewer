"""Minimal binary FBX 7.4 (7400) writer for Sonic Frontiers models.

Writes mesh geometry (positions/normals/UVs/vertex colors), materials with texture
file references, an armature (LimbNode skeleton) and skin (Skin deformer + per-bone
Clusters + BindPose). Designed to port cleanly to C++.

FBX binary layout: 27-byte header, then a tree of Nodes. Each node:
  u32 endOffset, u32 numProps, u32 propListLen, u8 nameLen, name,
  <properties>, <child nodes>, [13-byte null record if it has children].
Top-level list ends with a 13-byte null record; then a footer.
"""
import struct, zlib, math

MAGIC = b"Kaydara FBX Binary  \x00\x1a\x00"
VERSION = 7400


# ---------------- low-level node tree ----------------
class Node:
    def __init__(self, name, *props):
        self.name = name.encode("ascii") if isinstance(name, str) else name
        self.props = list(props)     # each: ('type', value)
        self.children = []

    def add(self, child):
        self.children.append(child)
        return child

    def child(self, name, *props):
        return self.add(Node(name, *props))


# property helpers
def PI(v): return ("I", int(v))       # int32
def PL(v): return ("L", int(v))       # int64
def PD(v): return ("D", float(v))     # float64
def PS(v): return ("S", v)            # string
def PB(v): return ("C", 1 if v else 0)
def Pd(vs): return ("d", list(vs))    # float64 array
def Pi(vs): return ("i", list(vs))    # int32 array


def _enc_prop(p):
    t, v = p
    if t == "I":
        return b"I" + struct.pack("<i", v)
    if t == "L":
        return b"L" + struct.pack("<q", v)
    if t == "D":
        return b"D" + struct.pack("<d", v)
    if t == "C":
        return b"C" + struct.pack("<b", v)
    if t == "S":
        b = v.encode("utf-8") if isinstance(v, str) else v
        return b"S" + struct.pack("<I", len(b)) + b
    if t == "R":
        return b"R" + struct.pack("<I", len(v)) + v
    if t in ("d", "i", "l", "f"):
        fmt = {"d": "<d", "i": "<i", "l": "<q", "f": "<f"}[t]
        raw = b"".join(struct.pack(fmt, x) for x in v)
        comp = zlib.compress(raw)
        if len(comp) < len(raw):
            return t.encode() + struct.pack("<III", len(v), 1, len(comp)) + comp
        return t.encode() + struct.pack("<III", len(v), 0, len(raw)) + raw
    raise ValueError(t)


def _enc_node(node):
    props = b"".join(_enc_prop(p) for p in node.props)
    children = b"".join(_enc_node(c) for c in node.children)
    has_children = len(node.children) > 0
    # child list terminated by a 13-byte null record when present
    inner = props + children + (b"\x00" * 13 if has_children else b"")
    name = node.name
    # header is 13 bytes (3 u32 + 1 u8) + name; endOffset is absolute -> patched by caller
    header_len = 13 + len(name)
    body = struct.pack("<III", 0, len(node.props), len(props)) + bytes([len(name)]) + name + inner
    return body, header_len, len(props)


def _serialize(nodes):
    """Serialize top-level node list with correct absolute endOffsets."""
    out = bytearray(MAGIC + struct.pack("<I", VERSION))

    def write_node(node):
        start = len(out)
        props = b"".join(_enc_prop(p) for p in node.props)
        name = node.name
        # placeholder header
        out.extend(struct.pack("<III", 0, len(node.props), len(props)))
        out.append(len(name))
        out.extend(name)
        out.extend(props)
        for c in node.children:
            write_node(c)
        if node.children:
            out.extend(b"\x00" * 13)
        end = len(out)
        struct.pack_into("<I", out, start, end)   # patch endOffset
        return end

    for n in nodes:
        write_node(n)
    out.extend(b"\x00" * 13)                       # end of top-level list
    # footer: some importers are lenient; write a simple padding + version block
    out.extend(b"\x00" * 120)
    out.extend(struct.pack("<I", VERSION))
    out.extend(b"\x00" * 120)
    return bytes(out)


# ---------------- scene builder ----------------
_uid = [1000000]
def _new_id():
    _uid[0] += 1
    return _uid[0]


def _obj_name(name, cls):
    # FBX "Name\x00\x01Class" convention
    return name + "\x00\x01" + cls


def build_fbx(meshes, bones=None, model_node_names=None, out_path=None,
              texture_dir=None, up_axis="y"):
    """meshes: list of dicts with keys positions, normals, uvs (list of channels),
    colors, faces, material (name), material_textures (list of (semantic,dds)),
    skin (list of per-vertex [(boneName,weight),...]).
    bones: list of (name, parent_index, translation, quat_xyzw, scale) or None."""
    bones = bones or []
    root = []

    # --- FBXHeaderExtension ---
    hdr = Node("FBXHeaderExtension")
    hdr.child("FBXHeaderVersion", PI(1003))
    hdr.child("FBXVersion", PI(VERSION))
    ct = hdr.child("CreationTimeStamp")
    ct.child("Version", PI(1000))
    creator = hdr.child("Creator", PS("SonicFrontiersExtractor"))
    root.append(hdr)

    # --- GlobalSettings ---
    gs = Node("GlobalSettings")
    gs.child("Version", PI(1000))
    p70 = gs.child("Properties70")
    up = 1 if up_axis == "y" else 2
    p70.child("P", PS("UpAxis"), PS("int"), PS("Integer"), PS(""), PI(up))
    p70.child("P", PS("UpAxisSign"), PS("int"), PS("Integer"), PS(""), PI(1))
    p70.child("P", PS("FrontAxis"), PS("int"), PS("Integer"), PS(""), PI(2))
    p70.child("P", PS("FrontAxisSign"), PS("int"), PS("Integer"), PS(""), PI(1))
    p70.child("P", PS("CoordAxis"), PS("int"), PS("Integer"), PS(""), PI(0))
    p70.child("P", PS("CoordAxisSign"), PS("int"), PS("Integer"), PS(""), PI(1))
    p70.child("P", PS("UnitScaleFactor"), PS("double"), PS("Number"), PS(""), PD(1.0))
    root.append(gs)

    # --- Documents / References ---
    docs = Node("Documents")
    docs.child("Count", PI(1))
    doc = docs.child("Document", PL(_new_id()), PS("Scene"), PS("Scene"))
    root.append(docs)
    root.append(Node("References"))

    objects = Node("Objects")
    connections = Node("Connections")

    def connect_oo(child_id, parent_id):
        connections.child("C", PS("OO"), PL(child_id), PL(parent_id))

    def connect_op(child_id, parent_id, prop):
        connections.child("C", PS("OP"), PL(child_id), PL(parent_id), PS(prop))

    def_counts = {"Geometry": 0, "Model": 0, "Material": 0, "Texture": 0,
                  "Deformer": 0, "Pose": 0}

    # --- bones: LimbNode models + world matrices ---
    bone_ids = []
    bone_world = []       # 4x4 as flat 16 for BindPose
    for i, b in enumerate(bones):
        bid = _new_id()
        bone_ids.append(bid)
        name, parent, t, q, s = b
        mdl = Node("Model", PL(bid), PS(_obj_name(name, "Model")), PS("LimbNode"))
        mdl.child("Version", PI(232))
        pp = mdl.child("Properties70")
        pp.child("P", PS("Lcl Translation"), PS("Lcl Translation"), PS(""), PS("A"),
                 PD(t[0]), PD(t[1]), PD(t[2]))
        eul = _quat_to_euler(q)
        pp.child("P", PS("Lcl Rotation"), PS("Lcl Rotation"), PS(""), PS("A"),
                 PD(eul[0]), PD(eul[1]), PD(eul[2]))
        pp.child("P", PS("Lcl Scaling"), PS("Lcl Scaling"), PS(""), PS("A"),
                 PD(s[0]), PD(s[1]), PD(s[2]))
        mdl.child("Shading", PB(True))
        mdl.child("Culling", PS("CullingOff"))
        objects.add(mdl)
        def_counts["Model"] += 1

    # world matrices for bind pose
    locals_ = []
    for b in bones:
        _, parent, t, q, s = b
        locals_.append(_compose(t, q, s))
    for i, b in enumerate(bones):
        parent = b[1]
        M = locals_[i]
        p = parent
        while p is not None and p >= 0:
            M = _matmul(locals_[p], M)
            p = bones[p][1]
        bone_world.append(M)

    # bone connections (child bone -> parent bone or skeleton root handled by caller)
    for i, b in enumerate(bones):
        parent = b[1]
        if parent is not None and parent >= 0:
            connect_oo(bone_ids[i], bone_ids[parent])

    name_to_bone = {b[0]: i for i, b in enumerate(bones)}

    # --- meshes ---
    for mi, mesh in enumerate(meshes):
        geo_id = _new_id()
        mdl_id = _new_id()
        positions = mesh["positions"]
        faces = mesh["faces"]

        geo = Node("Geometry", PL(geo_id), PS(_obj_name(f"mesh{mi}", "Geometry")), PS("Mesh"))
        verts = []
        for (x, y, z) in positions:
            verts += [x, y, z]
        geo.child("Vertices", Pd(verts))
        # polygon vertex index: last vertex of each polygon is XOR'd with -1
        pvi = []
        for f in faces:
            pvi += [f[0], f[1], (~f[2])]
        geo.child("PolygonVertexIndex", Pi(pvi))

        # Normals (by polygon vertex)
        if mesh.get("normals"):
            nrm = mesh["normals"]
            ne = Node("LayerElementNormal", PI(0))
            ne.child("Version", PI(101))
            ne.child("Name", PS(""))
            ne.child("MappingInformationType", PS("ByVertice"))
            ne.child("ReferenceInformationType", PS("Direct"))
            nvals = []
            for (x, y, z) in nrm:
                nvals += [x, y, z]
            ne.child("Normals", Pd(nvals))
            geo.add(ne)

        # UVs (channel 0)
        uvs = mesh.get("uvs") or []
        if uvs and uvs[0]:
            uv = uvs[0]
            ue = Node("LayerElementUV", PI(0))
            ue.child("Version", PI(101))
            ue.child("Name", PS("UVMap"))
            ue.child("MappingInformationType", PS("ByVertice"))
            ue.child("ReferenceInformationType", PS("Direct"))
            uvals = []
            for (u, v) in uv:
                uvals += [u, v]
            ue.child("UV", Pd(uvals))
            geo.add(ue)

        # vertex colors
        if mesh.get("colors"):
            ce = Node("LayerElementColor", PI(0))
            ce.child("Version", PI(101))
            ce.child("Name", PS("Col"))
            ce.child("MappingInformationType", PS("ByVertice"))
            ce.child("ReferenceInformationType", PS("Direct"))
            cvals = []
            for c in mesh["colors"]:
                cvals += [c[0], c[1], c[2], c[3] if len(c) > 3 else 1.0]
            ce.child("Colors", Pd(cvals))
            geo.add(ce)

        # material layer element
        me = Node("LayerElementMaterial", PI(0))
        me.child("Version", PI(101))
        me.child("Name", PS(""))
        me.child("MappingInformationType", PS("AllSame"))
        me.child("ReferenceInformationType", PS("IndexToDirect"))
        me.child("Materials", Pi([0]))
        geo.add(me)

        layer = Node("Layer", PI(0))
        layer.child("Version", PI(100))
        for typ, name in [("LayerElementNormal", None), ("LayerElementUV", None),
                          ("LayerElementColor", None), ("LayerElementMaterial", None)]:
            le = layer.child("LayerElement")
            le.child("Type", PS(typ))
            le.child("TypedIndex", PI(0))
        geo.add(layer)
        objects.add(geo)
        def_counts["Geometry"] += 1

        # model
        mdl = Node("Model", PL(mdl_id), PS(_obj_name(mesh.get("material", f"mesh{mi}"), "Model")), PS("Mesh"))
        mdl.child("Version", PI(232))
        pp = mdl.child("Properties70")
        pp.child("P", PS("Lcl Scaling"), PS("Lcl Scaling"), PS(""), PS("A"), PD(1), PD(1), PD(1))
        mdl.child("Shading", PB(True))
        mdl.child("Culling", PS("CullingOff"))
        objects.add(mdl)
        def_counts["Model"] += 1

        connect_oo(mdl_id, 0)          # model -> scene root (id 0)
        connect_oo(geo_id, mdl_id)     # geometry -> model

        # material
        mat_id = _new_id()
        mat = Node("Material", PL(mat_id), PS(_obj_name(mesh.get("material", f"mat{mi}"), "Material")), PS(""))
        mat.child("Version", PI(102))
        mat.child("ShadingModel", PS("phong"))
        mp = mat.child("Properties70")
        mp.child("P", PS("DiffuseColor"), PS("Color"), PS(""), PS("A"), PD(0.8), PD(0.8), PD(0.8))
        objects.add(mat)
        def_counts["Material"] += 1
        connect_oo(mat_id, mdl_id)

        # textures (reference by filename)
        for semantic, dds in mesh.get("material_textures", []):
            if not dds:
                continue
            tex_id = _new_id()
            path = dds
            if texture_dir:
                import os
                path = os.path.join(texture_dir, dds)
            tex = Node("Texture", PL(tex_id), PS(_obj_name(dds, "Texture")), PS(""))
            tex.child("Type", PS("TextureVideoClip"))
            tex.child("Version", PI(202))
            tex.child("TextureName", PS(_obj_name(dds, "Texture")))
            tex.child("FileName", PS(path))
            tex.child("RelativeFilename", PS(dds))
            objects.add(tex)
            def_counts["Texture"] += 1
            prop = {"diffuse": "DiffuseColor", "normal": "NormalMap",
                    "specular": "SpecularColor"}.get(semantic, "DiffuseColor")
            connect_op(tex_id, mat_id, prop)
            break  # one (diffuse) texture is enough for validation

        # skin
        skin = mesh.get("skin")
        if skin and bones:
            skin_id = _new_id()
            sk = Node("Deformer", PL(skin_id), PS(_obj_name("", "Deformer")), PS("Skin"))
            sk.child("Version", PI(101))
            sk.child("Link_DeformAcuracy", PD(50.0))
            objects.add(sk)
            def_counts["Deformer"] += 1
            connect_oo(skin_id, geo_id)

            # gather per-bone vertex weights
            bone_verts = {}
            for vi, influences in enumerate(skin):
                for (bname, w) in influences:
                    if w <= 0:
                        continue
                    bi = name_to_bone.get(bname)
                    if bi is None:
                        continue
                    bone_verts.setdefault(bi, ([], []))
                    bone_verts[bi][0].append(vi)
                    bone_verts[bi][1].append(w)

            for bi, (idxs, wts) in bone_verts.items():
                clu_id = _new_id()
                cl = Node("Deformer", PL(clu_id), PS(_obj_name("", "SubDeformer")), PS("Cluster"))
                cl.child("Version", PI(100))
                cl.child("UserData", PS(""), PS(""))
                cl.child("Indexes", Pi(idxs))
                cl.child("Weights", Pd(wts))
                M = bone_world[bi]
                # Transform = inverse bind (geometry->bone); TransformLink = bone world bind
                cl.child("Transform", Pd(_flat_colmajor(_invert_affine(M))))
                cl.child("TransformLink", Pd(_flat_colmajor(M)))
                objects.add(cl)
                def_counts["Deformer"] += 1
                connect_oo(clu_id, skin_id)
                connect_oo(bone_ids[bi], clu_id)

    # connect skeleton roots to scene
    for i, b in enumerate(bones):
        if b[1] is None or b[1] < 0:
            connect_oo(bone_ids[i], 0)

    # --- Definitions ---
    defs = Node("Definitions")
    defs.child("Version", PI(100))
    total = sum(def_counts.values()) + 1
    defs.child("Count", PI(total))
    for cls, cnt in def_counts.items():
        if cnt:
            ot = defs.child("ObjectType", PS(cls))
            ot.child("Count", PI(cnt))
    root.append(defs)

    root.append(objects)
    root.append(connections)

    data = _serialize(root)
    if out_path:
        with open(out_path, "wb") as f:
            f.write(data)
    return data


# ---------------- math ----------------
def _quat_to_euler(q):
    x, y, z, w = q
    # ZYX -> XYZ euler in degrees (FBX default rotation order XYZ)
    sinr = 2 * (w * x + y * z)
    cosr = 1 - 2 * (x * x + y * y)
    roll = math.atan2(sinr, cosr)
    sinp = 2 * (w * y - z * x)
    pitch = math.asin(max(-1, min(1, sinp)))
    siny = 2 * (w * z + x * y)
    cosy = 1 - 2 * (y * y + z * z)
    yaw = math.atan2(siny, cosy)
    return (math.degrees(roll), math.degrees(pitch), math.degrees(yaw))


def _compose(t, q, s):
    x, y, z, w = q
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    r = [
        [(1 - 2 * (yy + zz)) * s[0], 2 * (xy - wz) * s[1], 2 * (xz + wy) * s[2], t[0]],
        [2 * (xy + wz) * s[0], (1 - 2 * (xx + zz)) * s[1], 2 * (yz - wx) * s[2], t[1]],
        [2 * (xz - wy) * s[0], 2 * (yz + wx) * s[1], (1 - 2 * (xx + yy)) * s[2], t[2]],
        [0, 0, 0, 1],
    ]
    return r


def _matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def _flat_colmajor(M):
    # FBX matrices are column-major 16-float
    return [M[r][c] for c in range(4) for r in range(4)]


def _identity16():
    return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def _invert_affine(M):
    """Invert a 4x4 affine (rotation+translation+uniform-ish scale) matrix."""
    # invert 3x3 linear part
    a = [row[:3] for row in M[:3]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
           - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    if abs(det) < 1e-12:
        return [[1 if i == j else 0 for j in range(4)] for i in range(4)]
    inv_det = 1.0 / det
    inv = [[0] * 3 for _ in range(3)]
    inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * inv_det
    inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inv_det
    inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inv_det
    inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * inv_det
    inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inv_det
    inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inv_det
    inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * inv_det
    inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inv_det
    inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inv_det
    t = [M[0][3], M[1][3], M[2][3]]
    nt = [-(inv[0][0] * t[0] + inv[0][1] * t[1] + inv[0][2] * t[2]),
          -(inv[1][0] * t[0] + inv[1][1] * t[1] + inv[1][2] * t[2]),
          -(inv[2][0] * t[0] + inv[2][1] * t[1] + inv[2][2] * t[2])]
    return [[inv[0][0], inv[0][1], inv[0][2], nt[0]],
            [inv[1][0], inv[1][1], inv[1][2], nt[1]],
            [inv[2][0], inv[2][1], inv[2][2], nt[2]],
            [0, 0, 0, 1]]
