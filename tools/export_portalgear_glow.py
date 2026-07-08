"""Export the Portal Gear FBX with a CORRECT emissive setup for the Iridescence
shader: bake emission = line-mask (alp) tinted by the emission colour (ems cyan),
so it glows like in-game instead of a flat cyan smear. Drops literal transparency
(the gear body is opaque). Self-contained (textures embedded).
"""
import io, os, sys
import numpy as np
import pac as pacmod
import model as modelmod
import material as matmod
import ntsp
import fbx as fbxmod
from PIL import Image

RAW = r"E:\Games\steamapps\common\SonicFrontiers\image\x64\raw"
STREAM = os.path.join(RAW, "texture_streaming")
EXTRA = [os.path.join(RAW, "CommonObject.pac"), os.path.join(RAW, "stage", "IslandObject.pac")]


def load_img(ents, dds):
    stub = next((e.data for e in ents if e.name == dds), None)
    if stub is None:
        for pp in EXTRA:
            if os.path.exists(pp):
                stub = next((x.data for x in pacmod.unpack(pp) if x.name == dds), None)
                if stub: break
    if stub is None:
        return None
    full = ntsp.resolve_dds(stub, dds[:-4], STREAM) if ntsp.is_ntsi(stub) else stub
    if full is None:
        return None
    im = Image.open(io.BytesIO(full)); im.load()
    return im.convert("RGBA")


def png_bytes(im):
    b = io.BytesIO(); im.save(b, "PNG"); return b.getvalue()


def main(pac_path, out_fbx):
    ents = pacmod.unpack(pac_path)
    mfile = next(e for e in ents if e.name == "isl_obj_portalbit01.model")
    mdl = modelmod.parse_model(mfile.data)

    P = "isl_obj_portalbit_body_"
    abd = load_img(ents, P + "abd.dds")
    nrw = load_img(ents, P + "nrw.dds")
    prm = load_img(ents, P + "prm.dds")
    alp = load_img(ents, P + "alp.dds")     # glow line mask (white lines on black)
    ems = load_img(ents, P + "ems.dds")     # flat emission colour (cyan)

    # emission colour = mean of bright ems pixels
    a = np.asarray(ems.resize((64, 64)), float)
    bright = a[(a[..., :3].sum(2) > 120)]
    cyan = (bright[:, :3].mean(0) / 255.0) if len(bright) else np.array([0.0, 1.0, 0.95])
    # emissive map = line mask (alp luminance) * cyan
    mask = np.asarray(alp.convert("L"), float) / 255.0
    em = (mask[..., None] * cyan * 255.0).clip(0, 255).astype(np.uint8)
    emissive = Image.fromarray(em, "RGB").convert("RGBA")
    print(f"emission cyan tint = {tuple(round(c,3) for c in cyan)}, mask {alp.size}")

    names = {"abd": P + "abd.png", "nrw": P + "nrw.png", "prm": P + "prm.png",
             "glow": P + "emissive_lines.png"}
    embedded = {names["abd"]: png_bytes(abd), names["nrw"]: png_bytes(nrw),
                names["prm"]: png_bytes(prm), names["glow"]: png_bytes(emissive)}

    meshes = []
    for msh in mdl.meshes:
        meshes.append({
            "positions": msh.positions, "normals": msh.normals, "uvs": msh.uvs,
            "colors": msh.colors, "faces": msh.faces, "material": msh.material,
            # NOTE: emission uses the baked cyan line-mask; no literal transparency (opaque body)
            "material_textures": [("diffuse", names["abd"]), ("normal", names["nrw"]),
                                  ("specular", names["prm"]), ("emission", names["glow"])],
            "skin": None,
        })
    fbxmod.build_fbx(meshes, out_path=out_fbx, embedded_png=embedded)
    print(f"wrote {out_fbx}  (emission = alp line-mask x cyan)")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
