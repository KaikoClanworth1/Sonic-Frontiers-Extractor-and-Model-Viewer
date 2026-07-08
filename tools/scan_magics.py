"""Scan a binary for known Hedgehog/Needle magic signatures to see if data is
stored inline uncompressed."""
import sys

MAGICS = {
    b"PACx": "PACx archive header",
    b"BINA": "BINA v2 container",
    b"DDS ": "DDS texture",
    b"NEDMDLV5": "Needle Model V5",
    b"NEDARCV1": "Needle Archive V1",
    b"NEDLDMV5": "Needle LOD model?",
    b"HHNEEDLE": "Needle wrapper",
    b"NXIF": "Mirage NXIF",
    b"SkelPxd": "Skeleton Pxd",
}

def main(path):
    data = open(path, "rb").read()
    print(f"file: {path}  size: {len(data)}")
    for magic, name in MAGICS.items():
        offs = []
        start = 0
        while True:
            i = data.find(magic, start)
            if i < 0:
                break
            offs.append(i)
            start = i + 1
            if len(offs) > 50:
                break
        if offs:
            shown = ", ".join(f"0x{o:x}" for o in offs[:12])
            more = f" (+{len(offs)-12} more)" if len(offs) > 12 else ""
            print(f"  {magic!r:14} {name:24} x{len(offs):<4} @ {shown}{more}")

if __name__ == "__main__":
    main(sys.argv[1])
