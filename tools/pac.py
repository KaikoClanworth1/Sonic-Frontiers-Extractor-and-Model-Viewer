"""Sonic Frontiers PACx403L archive unpacker (Hedgehog Engine 2 / "RANGERS").

Implements the onion described in docs/research/pac_format.md:
  PACx403L (v4 outer) -> LZ4-chunked root blob (PACx402L v3) -> type/file radix
  tree -> data entries; embedded LZ4 split blobs (also PACx402L) hold model/
  material/dds bytes for the root's proxy entries.

Pure-stdlib except lz4 (pip install lz4). Ports cleanly to C++.
"""
import struct
import lz4.block


class PacEntry:
    __slots__ = ("name", "ext", "data", "from_split")

    def __init__(self, name, ext, data, from_split):
        self.name = name          # full name incl. extension, e.g. "chr_amy.model"
        self.ext = ext            # "model", "material", "dds", "skl.pxd", ...
        self.data = data          # bytes
        self.from_split = from_split

    def __repr__(self):
        return f"<PacEntry {self.name} {len(self.data)}B split={self.from_split}>"


def _u16(b, o): return struct.unpack_from("<H", b, o)[0]
def _u32(b, o): return struct.unpack_from("<I", b, o)[0]
def _u64(b, o): return struct.unpack_from("<Q", b, o)[0]
def _s32(b, o): return struct.unpack_from("<i", b, o)[0]


def _cstr(b, o):
    if o == 0:
        return ""
    e = b.find(b"\x00", o)
    if e < 0:
        e = len(b)
    return b[o:e].decode("utf-8", "replace")


def _lz4_chunked(comp, chunks, dst_size):
    """Decompress concatenated LZ4 *block* chunks. chunks = [(comp,uncomp), ...]."""
    if len(comp) == dst_size:      # stored uncompressed
        return bytes(comp)
    out = bytearray()
    s = 0
    for (c, u) in chunks:
        out += lz4.block.decompress(bytes(comp[s:s + c]), uncompressed_size=u)
        s += c
    if len(out) != dst_size:
        raise ValueError(f"lz4 size mismatch {len(out)} != {dst_size}")
    return bytes(out)


def _walk_files(nodes_base, blob, node_i, path_prefix, out, skip_proxies):
    """Recursively walk a file_tree, emitting (name, ext, data_off, size, flags)."""
    n = nodes_base + node_i * 0x28
    name_off = _u64(blob, n + 0x00)
    data_off = _u64(blob, n + 0x08)
    child_idx_off = _u64(blob, n + 0x10)
    child_count = _u16(blob, n + 0x24)
    has_data = blob[n + 0x26]
    buf_start = blob[n + 0x27]

    if name_off:
        path_prefix = path_prefix[:buf_start] + _cstr(blob, name_off)

    if has_data and data_off:
        de = data_off
        data_size = _u32(blob, de + 0x04)
        payload_off = _u64(blob, de + 0x10)
        ext_off = _u64(blob, de + 0x20)
        flags = _u16(blob, de + 0x28)
        ext = _cstr(blob, ext_off)
        if not (skip_proxies and (flags & 1)):   # skip root proxies (not_here)
            name = path_prefix[:buf_start] if not name_off else path_prefix
            # path_prefix already holds the full accumulated name at this leaf
            full = path_prefix
            fname = full + ("." + ext if ext else "")
            out.append((fname, ext, payload_off, data_size, flags))

    for k in range(child_count):
        ci = _s32(blob, child_idx_off + k * 4)
        _walk_files(nodes_base, blob, ci, path_prefix, out, skip_proxies)


def _walk_type_tree(blob, types_off, skip_proxies):
    """Walk type_tree -> file_trees. Returns [(fname, ext, data_off, size, flags)]."""
    node_count = _u32(blob, types_off + 0x00)
    data_node_count = _u32(blob, types_off + 0x04)
    nodes_ptr = _u64(blob, types_off + 0x08)
    data_idx_ptr = _u64(blob, types_off + 0x10)

    files = []
    for di in range(data_node_count):
        tni = _s32(blob, data_idx_ptr + di * 4)
        tnode = nodes_ptr + tni * 0x28
        file_tree_off = _u64(blob, tnode + 0x08)      # off64 -> file_tree (0x18)
        if not file_tree_off:
            continue
        ft_nodes = _u64(blob, file_tree_off + 0x08)
        _walk_files(ft_nodes, blob, 0, "", files, skip_proxies)
    return files


def _parse_v3_blob(blob, outer_file, is_root, entries, from_split):
    """Parse a decompressed PACx402L (v3) blob; recurse into embedded splits."""
    assert blob[4:7] in (b"402", b"403"), f"inner magic {blob[:8]!r}"
    trees_size = _u32(blob, 0x10)
    dep_table_size = _u32(blob, 0x14)
    data_entries_size = _u32(blob, 0x18)
    str_table_size = _u32(blob, 0x1C)
    file_data_size = _u32(blob, 0x20)
    off_table_size = _u32(blob, 0x24)
    dep_count = _u32(blob, 0x2C)

    types_off = 0x30
    dep_off = types_off + trees_size

    files = _walk_type_tree(blob, types_off, skip_proxies=is_root)
    for (fname, ext, data_off, size, flags) in files:
        data = blob[data_off:data_off + size] if data_off else b""
        entries.append(PacEntry(fname, ext, data, from_split))

    # embedded splits (LZ4)
    if is_root and dep_count:
        dep_arr_count = _u64(blob, dep_off + 0x00)
        dep_arr_ptr = _u64(blob, dep_off + 0x08)
        for i in range(dep_arr_count):
            di = dep_arr_ptr + i * 0x20
            comp_size = _u32(blob, di + 0x08)
            uncomp_size = _u32(blob, di + 0x0C)
            data_pos = _u32(blob, di + 0x10)
            chunk_count = _u32(blob, di + 0x14)
            chunks_ptr = _u64(blob, di + 0x18)
            chunks = [(_u32(blob, chunks_ptr + j * 8), _u32(blob, chunks_ptr + j * 8 + 4))
                      for j in range(chunk_count)]
            comp = outer_file[data_pos:data_pos + comp_size]
            split = _lz4_chunked(comp, chunks, uncomp_size)
            _parse_v3_blob(split, outer_file, False, entries, from_split=True)


def unpack(path):
    """Unpack a .pac file. Returns list[PacEntry]."""
    with open(path, "rb") as f:
        file = f.read()
    return unpack_bytes(file)


def unpack_bytes(file):
    magic = file[:8]
    if magic[:4] != b"PACx":
        raise ValueError(f"not a PACx file: {magic!r}")
    ver = magic[4:7]
    entries = []

    if ver == b"403":
        root_off = _u32(file, 0x10)
        root_comp = _u32(file, 0x14)
        root_uncomp = _u32(file, 0x18)
        flags_v4 = _u16(file, 0x1C)
        flags_v3 = _u16(file, 0x1E)
        has_meta = flags_v4 & 0x80
        is_lz4 = flags_v3 & 0x200

        root_chunks = []
        if has_meta:
            parents_size = _u32(file, 0x20)
            chunk_off = 0x30 + parents_size
            n = _u32(file, chunk_off)
            root_chunks = [(_u32(file, chunk_off + 4 + 8 * i),
                            _u32(file, chunk_off + 4 + 8 * i + 4)) for i in range(n)]
        comp = file[root_off:root_off + root_comp]
        if is_lz4:
            root = _lz4_chunked(comp, root_chunks, root_uncomp)
        else:
            import zlib
            root = zlib.decompress(comp) if root_comp != root_uncomp else comp
        _parse_v3_blob(root, file, True, entries, from_split=False)

    elif ver in (b"402", b"301", b"300"):
        # on-disk v3 pac stored uncompressed (rare for Frontiers on-disk files)
        _parse_v3_blob(file, file, True, entries, from_split=False)
    else:
        raise ValueError(f"unsupported PACx version {ver!r}")

    return entries


if __name__ == "__main__":
    import sys
    from collections import Counter
    ents = unpack(sys.argv[1])
    print(f"{len(ents)} files")
    hist = Counter(e.ext for e in ents)
    print("ext histogram:", dict(hist.most_common()))
    for e in ents[:25]:
        print(f"  {e.name:50} {len(e.data):>9}B {'SPLIT' if e.from_split else 'root'}")
