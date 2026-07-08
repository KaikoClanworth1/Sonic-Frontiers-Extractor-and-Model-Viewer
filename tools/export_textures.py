"""Export all texture maps of a model's material(s) to a folder as PNG + DDS.
Resolves NTSI-streamed textures from the .ntsp packages. Also scans a few common
pacs for any referenced texture not present in the source pac (e.g. shared maps).

Usage: export_textures.py <pac> <model_basename> <out_dir>
"""
import io, os, sys, glob
import pac as pacmod
import model as modelmod
import material as matmod
import ntsp
from PIL import Image

RAW = r"E:\Games\steamapps\common\SonicFrontiers\image\x64\raw"
STREAMING = os.path.join(RAW, "texture_streaming")
# extra pacs to search for shared textures not in the source pac
EXTRA_PACS = [os.path.join(RAW, "CommonObject.pac"),
              os.path.join(RAW, "stage", "IslandObject.pac"),
              os.path.join(RAW, "stage", "w1_common.pac")]

SEMANTIC = {"abd": "albedo/diffuse", "nrw": "normal", "nrm": "normal", "nra": "normal",
            "prm": "specular/PBR-packed", "alp": "transparency/alpha", "ems": "emission",
            "spt": "reflection/spec-tint", "ref": "reflection"}


def find_stub(name, primary):
    e = next((x for x in primary if x.name == name), None)
    if e:
        return e.data
    for pp in EXTRA_PACS:
        if not os.path.exists(pp):
            continue
        for x in pacmod.unpack(pp):
            if x.name == name:
                return x.data
    return None


def main(pac_path, basename, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    ents = pacmod.unpack(pac_path)
    # collect texture slots from all materials belonging to this model's meshes
    mfile = next((e for e in ents if e.ext in ("model", "terrain-model") and basename in e.name), None)
    mdl = modelmod.parse_model(mfile.data)
    mat_names = {m.material for m in mdl.meshes}
    slots = []  # (semantic, dds)
    for e in ents:
        if e.ext == "material" and e.name[:-len(".material")] in mat_names:
            for t in matmod.parse_material(e.data).textures:
                if t.dds and (t.semantic, t.dds) not in slots:
                    slots.append((t.semantic, t.dds))

    print(f"{len(slots)} texture slots for {basename}:")
    written = []
    for semantic, dds in slots:
        base = dds[:-4] if dds.endswith(".dds") else dds
        suffix = base.rsplit("_", 1)[-1]
        kind = SEMANTIC.get(suffix, semantic or "?")
        stub = find_stub(dds, ents)
        if stub is None:
            print(f"  [MISSING] {dds:42} ({kind}) - not found in source or common pacs")
            continue
        full = ntsp.resolve_dds(stub, base, STREAMING) if ntsp.is_ntsi(stub) else stub
        if full is None:
            print(f"  [UNRESOLVED] {dds:42} ({kind}) - streaming entry missing")
            continue
        # write reconstructed DDS
        dds_path = os.path.join(out_dir, dds)
        open(dds_path, "wb").write(full)
        # decode to PNG
        try:
            im = Image.open(io.BytesIO(full)); im.load()
            png_path = os.path.join(out_dir, base + ".png")
            im.convert("RGBA").save(png_path)
            print(f"  [OK] {dds:42} {im.size[0]}x{im.size[1]:<4} ({kind})")
            written.append(png_path)
        except Exception as ex:
            print(f"  [DDS-only] {dds:42} ({kind}) PNG decode failed: {ex}")
    print(f"\nwrote {len(written)} PNG + DDS files to: {out_dir}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
