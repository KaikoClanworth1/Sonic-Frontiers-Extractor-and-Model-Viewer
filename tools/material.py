"""Sonic Frontiers .material parser (Hedgehog Engine 2 sample-chunk, version 3).

Unlike the Needle .model (LE self-relative offsets), Frontiers materials keep the
LEGACY Mirage convention: offsets are BIG-endian and relative to the sample-chunk
nodes base (header+0x10). Verified against chr_amy_skin.material (shader 'cyber' at
0x10+0x44; slot names at 0x10+0x294). Resolves shader + texture slots -> DDS names.
"""
import struct
import hh_mirage as m


class TexEntry:
    def __init__(self):
        self.slot = ""       # sampler/slot name (textureEntryNames[i])
        self.dds = ""        # DDS file name (texName + ".dds")
        self.semantic = ""   # "diffuse"/"normal"/...
        self.uv_index = 0
        self.wrap = (0, 0)


class Material:
    def __init__(self):
        self.version = 0
        self.shader = ""
        self.sub_shader = ""
        self.textures = []            # list[TexEntry]
        self.float4 = {}              # name -> list of (x,y,z,w)
        self.alpha_threshold = 0
        self.no_backface_cull = False
        self.additive = False


def parse_material(data):
    blocks = m.find_model_blocks(data)     # sample-chunk detection (works for material too)
    header_off = blocks[0] if blocks else 0
    roots, limit = m.parse_sample_chunk(data, header_off)
    ctx = m.find_node(roots, "Contexts") or m.find_node(roots, "Material")
    if not ctx:
        raise ValueError("no Contexts/Material node")
    base = ctx.data_off                    # raw_material struct location
    ob = header_off + 0x10                 # offset base (Mirage nodes base)
    ver = ctx.value

    def off(field):                        # BE off32, base-relative to `ob`
        v = struct.unpack_from(">I", data, field)[0]
        return 0 if v == 0 else ob + v

    mat = Material()
    mat.version = ver
    mat.shader = m.cstr(data, off(base + 0x00))
    mat.sub_shader = m.cstr(data, off(base + 0x04))

    tex_names_arr = off(base + 0x08)
    tex_entries_arr = off(base + 0x0C)
    mat.alpha_threshold = data[base + 0x10]
    mat.no_backface_cull = bool(data[base + 0x11])
    mat.additive = bool(data[base + 0x12])
    f4_count = data[base + 0x14]
    tex_count = data[base + 0x17]
    f4_arr = off(base + 0x18)

    for i in range(tex_count):
        te = TexEntry()
        if tex_names_arr:
            te.slot = m.cstr(data, off(tex_names_arr + i * 4))
        if tex_entries_arr:
            ent = off(tex_entries_arr + i * 4)
            if ent:
                name = m.cstr(data, off(ent + 0x00))
                te.dds = (name + ".dds") if name else ""
                te.uv_index = data[ent + 0x04]
                te.wrap = (data[ent + 0x05], data[ent + 0x06])
                te.semantic = m.cstr(data, off(ent + 0x08))
        mat.textures.append(te)

    # float4 params
    for i in range(f4_count):
        if not f4_arr:
            break
        p = off(f4_arr + i * 4)
        if not p:
            continue
        vcount = data[p + 0x02]
        pname = m.cstr(data, off(p + 0x04))
        vals_ptr = off(p + 0x08)
        vals = [struct.unpack_from(">ffff", data, vals_ptr + k * 16) for k in range(vcount)]
        mat.float4[pname] = vals

    return mat


if __name__ == "__main__":
    import sys
    data = open(sys.argv[1], "rb").read()
    mat = parse_material(data)
    print(f"material v{mat.version}  shader={mat.shader!r} sub={mat.sub_shader!r} "
          f"alpha={mat.alpha_threshold} additive={mat.additive}")
    for t in mat.textures:
        print(f"  slot={t.slot:12} semantic={t.semantic:12} dds={t.dds:28} uv{t.uv_index} wrap{t.wrap}")
    for name, vals in mat.float4.items():
        print(f"  param {name}: {vals[0] if vals else None}")
