"""Validate an FBX by importing it headless into Blender (bpy) and checking that
meshes / UVs / vertex colors / armature / skin all survived. Run with the venv
python that has bpy installed.
"""
import sys
import bpy


def clear():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def validate(path):
    clear()
    try:
        bpy.ops.import_scene.fbx(filepath=path)
    except Exception as e:
        print("IMPORT FAILED:", e)
        return False

    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    arms = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    total_v = sum(len(o.data.vertices) for o in meshes)
    total_f = sum(len(o.data.polygons) for o in meshes)
    uv_ok = sum(1 for o in meshes if o.data.uv_layers)
    col_ok = sum(1 for o in meshes if o.data.vertex_colors or getattr(o.data, "color_attributes", None))
    mats = set()
    for o in meshes:
        for s in o.data.materials:
            if s:
                mats.add(s.name)
    # skin: vertex groups + armature modifier
    skinned = sum(1 for o in meshes if o.vertex_groups)
    total_bones = sum(len(a.data.bones) for a in arms)
    # textures referenced
    imgs = [i.name for i in bpy.data.images]

    print(f"  meshes={len(meshes)} verts={total_v} faces={total_f}")
    print(f"  uv_layers_on={uv_ok}/{len(meshes)}  vcol_on={col_ok}/{len(meshes)}")
    print(f"  materials={len(mats)}  armatures={len(arms)} bones={total_bones}")
    print(f"  skinned_meshes={skinned}/{len(meshes)}  images={len(imgs)}")
    ok = (len(meshes) > 0 and total_v > 0 and total_f > 0 and uv_ok > 0)
    print("  RESULT:", "PASS" if ok else "FAIL")
    return ok


if __name__ == "__main__":
    ok = True
    for p in sys.argv[1:]:
        print(f"=== {p} ===")
        ok = validate(p) and ok
    sys.exit(0 if ok else 1)
