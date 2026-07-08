"""Extract a .pac (or a whole folder of them) to disk."""
import os, sys
from pac import unpack


def extract_one(pac_path, out_dir):
    ents = unpack(pac_path)
    os.makedirs(out_dir, exist_ok=True)
    for e in ents:
        # names may contain '@' and subpaths; keep flat but safe
        safe = e.name.replace("/", "_").replace("\\", "_")
        with open(os.path.join(out_dir, safe), "wb") as f:
            f.write(e.data)
    return len(ents)


if __name__ == "__main__":
    src = sys.argv[1]
    out = sys.argv[2]
    n = extract_one(src, out)
    print(f"extracted {n} files -> {out}")
