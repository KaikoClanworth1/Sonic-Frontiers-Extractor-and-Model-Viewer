"""Needle Texture Streaming (NTSI stub in .pac -> PSTN .ntsp package) resolver.

A Frontiers .dds inside a .pac may be an NTSI stub (real pixels streamed). This
reconstructs the full .dds: read the stub's DDS header + package name, look the
texture up in <package_name>.ntsp by name hash, concatenate its mip blobs.
Format per HedgeLib++ hl_hh_needle_texture_streaming.h.
"""
import os, struct


def compute_name_hash(name: str) -> int:
    h = 0
    for c in name.encode("utf-8"):
        h = (h * 31 + c) & 0xFFFFFFFF
    return h & 0x7FFFFFFF


def is_ntsi(data: bytes) -> bool:
    return len(data) >= 4 and data[:4] == b"NTSI"


def parse_ntsi(data: bytes):
    """Return dict with package_name and dds_header (bytes)."""
    sig, ver, unk0, pns, m4s, m4i = struct.unpack_from("<6I", data, 0)
    assert sig == 0x4953544E  # "NTSI"
    name = data[0x18:0x18 + pns].split(b"\x00")[0].decode("utf-8", "replace")
    hdr_off = 0x18 + pns + m4s
    # header size: 0x94 if DX10 fourcc else 0x80
    fourcc = data[hdr_off + 0x54:hdr_off + 0x58]
    hdr_size = 0x94 if fourcc == b"DX10" else 0x80
    return {
        "package_name": name,
        "dds_header": data[hdr_off:hdr_off + hdr_size],
        "mip4x4": data[0x18 + pns:0x18 + pns + m4s],
        "mip4x4_index": m4i,
    }


class Package:
    """Read-only PSTN package. Indexes entries by name hash; loads blob data on demand."""
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            sig, ver, self.entry_count, self.blob_count = struct.unpack("<4I", f.read(16))
            self.header_size = struct.unpack("<Q", f.read(8))[0]
            f.seek(0)
            head = f.read(self.header_size)
        assert sig == 0x4E545350  # "PSTN"
        self.head = head
        self.blob_base = 0x18 + self.entry_count * 0x18
        # index entries by hash
        self.by_hash = {}
        for i in range(self.entry_count):
            o = 0x18 + i * 0x18
            nh, bi, bc = struct.unpack_from("<3I", head, o)
            w, h = struct.unpack_from("<2H", head, o + 12)
            self.by_hash[nh] = (bi, bc, w, h)

    def blob(self, index):
        o = self.blob_base + index * 0x10
        data_off, data_size = struct.unpack_from("<QQ", self.head, o)
        with open(self.path, "rb") as f:
            f.seek(data_off)
            return f.read(data_size)

    def get_mips(self, name):
        """Return concatenated mip bytes for a texture name, or None."""
        e = self.by_hash.get(compute_name_hash(name))
        if not e:
            return None, None
        bi, bc, w, h = e
        return b"".join(self.blob(bi + k) for k in range(bc)), (w, h)


_pkg_cache = {}


def resolve_dds(stub_bytes: bytes, texture_name: str, streaming_dir: str):
    """Reconstruct a full .dds from an NTSI stub. texture_name = dds base (no ext).
    Returns full .dds bytes, or the stub unchanged if not NTSI / package missing."""
    if not is_ntsi(stub_bytes):
        return stub_bytes
    info = parse_ntsi(stub_bytes)
    pkg_path = os.path.join(streaming_dir, info["package_name"] + ".ntsp")
    if not os.path.exists(pkg_path):
        return None
    pkg = _pkg_cache.get(pkg_path)
    if pkg is None:
        pkg = _pkg_cache[pkg_path] = Package(pkg_path)
    mips, dims = pkg.get_mips(texture_name)
    if mips is None:
        return None
    return info["dds_header"] + mips


if __name__ == "__main__":
    import sys
    from binlib import hexdump
    import pac
    ents = pac.unpack(sys.argv[1])                       # a .pac
    name = sys.argv[2]                                   # e.g. isl_obj_portalbit_body_abd.dds
    streaming = r"E:\Games\steamapps\common\SonicFrontiers\image\x64\raw\texture_streaming"
    stub = [e for e in ents if e.name == name][0].data
    base = name[:-4] if name.endswith(".dds") else name
    dds = resolve_dds(stub, base, streaming)
    print(f"stub {len(stub)}B -> full dds {len(dds) if dds else None}B")
    if dds:
        w = struct.unpack_from("<I", dds, 0x10)[0]; h = struct.unpack_from("<I", dds, 0x0C)[0]
        print(f"dds {w}x{h} fourcc={dds[0x54:0x58]}  mip0 expected DXT1={w*h//2}")
        print(hexdump(dds, 0, 32))
