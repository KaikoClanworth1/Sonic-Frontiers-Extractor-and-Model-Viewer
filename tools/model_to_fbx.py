"""End-to-end: a .pac (or a folder of extracted files) -> binary FBX with meshes,
materials (+texture refs), armature and skin weights.
Usage: model_to_fbx.py <pac_or_dir> <model_basename> <out.fbx>
"""
import os, sys
import pac as pacmod
import model as modelmod
import material as matmod
import skeleton as skelmod
import fbx as fbxmod


def load_entries(src):
    """Return {name: bytes} from a pac file or an already-extracted directory."""
    files = {}
    if os.path.isdir(src):
        for f in os.listdir(src):
            files[f] = open(os.path.join(src, f), "rb").read()
    else:
        for e in pacmod.unpack(src):
            files[e.name] = e.data
    return files


def convert(src, base, out_path):
    files = load_entries(src)
    model_file = base + ".model"
    if model_file not in files:
        cand = [k for k in files if k.endswith(".model") and base in k]
        if not cand:
            raise SystemExit(f"no .model matching {base}; have: "
                             f"{[k for k in files if k.endswith('.model')][:10]}")
        model_file = cand[0]
    mdl = modelmod.parse_model(files[model_file])

    # skeleton
    bones = []
    skl_file = model_file.replace(".model", ".skl.pxd")
    if skl_file in files:
        sk = skelmod.parse_skeleton(files[skl_file])
        bones = [(b.name, (b.parent if b.parent >= 0 else -1), b.translation,
                  b.rotation, b.scale) for b in sk]
    skel_names = {b[0] for b in bones}

    # materials (map material name -> texture list)
    mat_tex = {}
    for k, v in files.items():
        if k.endswith(".material"):
            try:
                mt = matmod.parse_material(v)
                mat_tex[k[:-len(".material")]] = [(t.semantic, t.dds) for t in mt.textures]
            except Exception:
                pass

    meshes = []
    unmapped = set()
    for msh in mdl.meshes:
        # resolve skin to bone names
        skin = None
        if msh.weights and bones:
            skin = []
            for gi, wv in zip(msh.bone_indices, msh.weights):
                infl = []
                for idx, w in zip(gi, wv):
                    if w <= 0:
                        continue
                    nm = mdl.node_names[idx] if idx < len(mdl.node_names) else None
                    if nm and nm in skel_names:
                        infl.append((nm, w))
                    elif nm:
                        unmapped.add(nm)
                skin.append(infl)
        meshes.append({
            "positions": msh.positions,
            "normals": msh.normals,
            "uvs": msh.uvs,
            "colors": msh.colors,
            "faces": msh.faces,
            "material": msh.material,
            "material_textures": mat_tex.get(msh.material, []),
            "skin": skin,
        })

    fbxmod.build_fbx(meshes, bones=bones, out_path=out_path)
    tv = sum(len(m["positions"]) for m in meshes)
    tf = sum(len(m["faces"]) for m in meshes)
    print(f"FBX written: {out_path}")
    print(f"  meshes={len(meshes)} verts={tv} faces={tf} bones={len(bones)} "
          f"materials_with_tex={sum(1 for m in meshes if m['material_textures'])}")
    if unmapped:
        print(f"  NOTE: {len(unmapped)} model bone names not in skeleton (skin infl dropped): "
              f"{sorted(unmapped)[:6]}")
    return out_path


if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2], sys.argv[3])
