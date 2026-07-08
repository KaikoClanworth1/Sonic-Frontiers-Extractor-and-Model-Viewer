"""Export a Frontiers model to a self-contained binary FBX with EMBEDDED textures.
Resolves NTSI-streamed textures from the .ntsp packages and decodes them to PNG.

Usage: export_fbx_textured.py <pac> <model_basename> <out.fbx>
"""
import io, os, sys
import pac as pacmod
import model as modelmod
import material as matmod
import skeleton as skelmod
import ntsp
import fbx as fbxmod
from PIL import Image

STREAMING = r"E:\Games\steamapps\common\SonicFrontiers\image\x64\raw\texture_streaming"


def resolve_png(dds_name, pac_entries):
    """Return (png_bytes, note) for a referenced .dds, resolving NTSI streaming."""
    base = dds_name[:-4] if dds_name.endswith(".dds") else dds_name
    ent = next((e for e in pac_entries if e.name == dds_name), None)
    if ent is None:
        return None, "not in pac"
    data = ent.data
    if ntsp.is_ntsi(data):
        dds = ntsp.resolve_dds(data, base, STREAMING)
        if dds is None:
            return None, "streaming package/entry missing"
    else:
        dds = data
    try:
        im = Image.open(io.BytesIO(dds)); im.load()
        buf = io.BytesIO(); im.convert("RGBA").save(buf, "PNG")
        return buf.getvalue(), f"{im.size[0]}x{im.size[1]}"
    except Exception as e:
        return None, f"decode failed: {e}"


def convert(pac_path, basename, out_path):
    ents = pacmod.unpack(pac_path)
    mfile = next((e for e in ents if e.ext in ("model", "terrain-model")
                  and e.name[:e.name.find(".")] == basename), None)
    if not mfile:
        mfile = next((e for e in ents if e.ext in ("model", "terrain-model") and basename in e.name), None)
    if not mfile:
        raise SystemExit("model not found")
    mdl = modelmod.parse_model(mfile.data)

    # skeleton
    bones = []
    skl = mfile.name[:mfile.name.find(".")] + ".skl.pxd"
    se = next((e for e in ents if e.name == skl), None)
    if se:
        for b in skelmod.parse_skeleton(se.data):
            bones.append((b.name, b.parent if b.parent >= 0 else -1, b.translation, b.rotation, b.scale))
    skel_names = {b[0] for b in bones}

    # materials -> texture slots
    mat_tex = {}
    for e in ents:
        if e.ext != "material":
            continue
        try:
            mt = matmod.parse_material(e.data)
            mat_tex[e.name[:-len(".material")]] = [(t.semantic, t.dds) for t in mt.textures]
        except Exception:
            pass

    embedded = {}
    notes = []
    meshes = []
    for msh in mdl.meshes:
        tslots = mat_tex.get(msh.material, [])
        # dedupe by dds, resolve+embed
        for semantic, dds in tslots:
            if dds and dds not in embedded:
                png, note = resolve_png(dds, ents)
                if png:
                    embedded[dds] = png
                notes.append(f"  {semantic:12} {dds:40} {note}")
        skin = None
        if msh.weights and bones:
            skin = []
            for gi, wv in zip(msh.bone_indices, msh.weights):
                infl = [(mdl.node_names[i], w) for i, w in zip(gi, wv)
                        if w > 0 and i < len(mdl.node_names) and mdl.node_names[i] in skel_names]
                skin.append(infl)
        meshes.append({
            "positions": msh.positions, "normals": msh.normals, "uvs": msh.uvs,
            "colors": msh.colors, "faces": msh.faces, "material": msh.material,
            "material_textures": tslots, "skin": skin,
        })

    fbxmod.build_fbx(meshes, bones=bones, out_path=out_path, embedded_png=embedded)
    tv = sum(len(m["positions"]) for m in meshes)
    print(f"FBX: {out_path}")
    print(f"  meshes={len(meshes)} verts={tv} bones={len(bones)} embedded_textures={len(embedded)}")
    print("texture resolution:")
    for n in dict.fromkeys(notes):
        print(n)
    return out_path, embedded


if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2], sys.argv[3])
