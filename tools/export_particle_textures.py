"""Extract the particle/effect textures used by a model's .cemt effects.
Scans the model's cemt files for ef_*/em_* texture refs, locates them across the
common effect pacs, resolves NTSI streaming, and writes PNG + DDS to <out>/particle_textures/.

Usage: export_particle_textures.py <pac_with_cemts> <name_filter> <out_dir>
"""
import io, os, re, sys, glob
import pac as pacmod
import ntsp
from PIL import Image

ROOT = r"E:\Games\steamapps\common\SonicFrontiers\image\x64\raw"
STREAM = os.path.join(ROOT, "texture_streaming")


def main(pac_path, name_filter, out_dir):
    out = os.path.join(out_dir, "particle_textures")
    os.makedirs(out, exist_ok=True)
    ents = pacmod.unpack(pac_path)
    cemts = [e for e in ents if e.ext == "cemt" and name_filter in e.name.lower()]
    refs = set()
    for e in cemts:
        for m in re.finditer(rb"(ef|em)_[a-zA-Z0-9_]{3,50}", e.data):
            refs.add(m.group().decode())
    targets = {r + ".dds" for r in refs}
    print(f"{len(cemts)} .cemt files -> {len(targets)} candidate texture names")

    # gather source pacs: EffectCommon + the source pac + island/common
    src_pacs = [os.path.join(ROOT, "EffectCommon.pac"), pac_path,
                os.path.join(ROOT, "CommonObject.pac"), os.path.join(ROOT, "stage", "IslandObject.pac")]
    stub_by_name = {}
    for p in src_pacs:
        if not os.path.exists(p):
            continue
        for e in pacmod.unpack(p):
            if e.ext == "dds" and e.name in targets and e.name not in stub_by_name:
                stub_by_name[e.name] = e.data

    ok = 0
    for name in sorted(stub_by_name):
        stub = stub_by_name[name]
        full = ntsp.resolve_dds(stub, name[:-4], STREAM) if ntsp.is_ntsi(stub) else stub
        if full is None:
            print(f"  [unresolved] {name}")
            continue
        open(os.path.join(out, name), "wb").write(full)
        try:
            im = Image.open(io.BytesIO(full)); im.load()
            im.convert("RGBA").save(os.path.join(out, name[:-4] + ".png"))
            print(f"  [ok] {name:36} {im.size[0]}x{im.size[1]}")
            ok += 1
        except Exception as ex:
            print(f"  [dds-only] {name:36} {ex}")
    print(f"\nwrote {ok} particle textures (PNG+DDS) to {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
