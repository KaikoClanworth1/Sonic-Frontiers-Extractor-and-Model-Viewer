"""Extract a full effect set (particle textures + particle meshes) from one or more
pacs' .cemt effect files into <out>/particles/{textures,meshes}/.

Usage: export_effect_set.py <out_dir> <pac1> [pac2 ...]
"""
import io, os, re, sys
import pac as pacmod
import ntsp
import export_fbx_textured as exfbx
from PIL import Image

ROOT = r"E:\Games\steamapps\common\SonicFrontiers\image\x64\raw"
STREAM = os.path.join(ROOT, "texture_streaming")
SHARED = [os.path.join(ROOT, "EffectCommon.pac"), os.path.join(ROOT, "CommonObject.pac"),
          os.path.join(ROOT, "stage", "IslandObject.pac")]


def main(out_dir, pac_paths):
    tex_dir = os.path.join(out_dir, "particles", "textures")
    mesh_dir = os.path.join(out_dir, "particles", "meshes")
    os.makedirs(tex_dir, exist_ok=True)
    os.makedirs(mesh_dir, exist_ok=True)

    # 1) collect ef_/em_ refs from all .cemt across the input pacs
    refs = set()
    ncemt = 0
    for p in pac_paths:
        for e in pacmod.unpack(p):
            if e.ext == "cemt":
                ncemt += 1
                for m in re.finditer(rb"(ef|em)_[a-zA-Z0-9_]{3,50}", e.data):
                    refs.add(m.group().decode())
    print(f"{ncemt} .cemt files -> {len(refs)} distinct ef_/em_ refs")

    # 2) index dds + model entries across input + shared pacs
    dds_src, model_src = {}, {}
    for p in list(pac_paths) + SHARED:
        if not os.path.exists(p):
            continue
        for e in pacmod.unpack(p):
            if e.ext == "dds" and e.name[:-4] in refs and e.name not in dds_src:
                dds_src[e.name] = e.data
            elif e.ext == "model" and e.name[:-6] in refs and e.name not in model_src:
                model_src[e.name] = p

    # 3) textures -> resolve NTSI, write PNG + DDS
    ntex = 0
    for name in sorted(dds_src):
        full = ntsp.resolve_dds(dds_src[name], name[:-4], STREAM) if ntsp.is_ntsi(dds_src[name]) else dds_src[name]
        if full is None:
            continue
        open(os.path.join(tex_dir, name), "wb").write(full)
        try:
            im = Image.open(io.BytesIO(full)); im.load()
            im.convert("RGBA").save(os.path.join(tex_dir, name[:-4] + ".png"))
            ntex += 1
        except Exception:
            pass
    print(f"wrote {ntex} particle textures (PNG+DDS) -> {tex_dir}")

    # 4) meshes -> textured FBX
    nmesh = 0
    for name in sorted(model_src):
        try:
            exfbx.convert(model_src[name], name[:-6], os.path.join(mesh_dir, name[:-6] + ".fbx"))
            nmesh += 1
        except Exception as e:
            print("  mesh FAIL", name, e)
    print(f"wrote {nmesh} particle meshes (FBX) -> {mesh_dir}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
