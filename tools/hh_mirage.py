"""Hedgehog Engine 2 'Mirage' sample-chunk container + Needle (NEDARCV1) wrapper.

Frontiers .model/.material data is BIG-ENDIAN sample-chunk (verified against real
chr_amy.model: magic 0x0133054A stored as bytes 01 33 05 4a). The .model file is
additionally wrapped in a little-endian Needle archive (NEDARCV1 -> NEDMDLV5 block).
"""
import struct

SC_MAGIC = 0x0133054A


def u8(d, o):  return d[o]
def u16be(d, o): return struct.unpack_from(">H", d, o)[0]
def u32be(d, o): return struct.unpack_from(">I", d, o)[0]
def s32be(d, o): return struct.unpack_from(">i", d, o)[0]
def f32be(d, o): return struct.unpack_from(">f", d, o)[0]


def u32le(d, o): return struct.unpack_from("<I", d, o)[0]


def offR(d, pos):
    """Self-relative little-endian off32 (HedgeLib off32: ptr = &field + value)."""
    v = u32le(d, pos)
    return 0 if v == 0 else pos + v


def cstr(d, o):
    if o <= 0 or o >= len(d):
        return ""
    e = d.find(b"\x00", o)
    return d[o:e if e >= 0 else len(d)].decode("utf-8", "replace")


class SCNode:
    __slots__ = ("flags", "value", "name", "off", "size", "is_leaf", "is_last", "is_root", "children")

    def __init__(self, d, off):
        self.off = off
        self.flags = u32be(d, off)
        self.value = u32be(d, off + 4)
        self.name = d[off + 8:off + 16].rstrip(b" \x00").decode("ascii", "replace")
        self.size = self.flags & 0x1FFFFFFF
        self.is_leaf = bool(self.flags & 0x20000000)
        self.is_last = bool(self.flags & 0x40000000)
        self.is_root = bool(self.flags & 0x80000000)
        self.children = []

    @property
    def data_off(self):
        return self.off + 0x10   # bytes immediately after the 16-byte node


def _parse_children(d, start, limit, depth=0):
    """Parse a sibling chain of sample-chunk nodes starting at `start`."""
    nodes = []
    p = start
    guard = 0
    while p + 0x10 <= limit and guard < 4096:
        guard += 1
        n = SCNode(d, p)
        nodes.append(n)
        if not n.is_leaf and n.size > 0x10:
            # children occupy [p+0x10, p+size)
            n.children = _parse_children(d, p + 0x10, min(p + n.size, limit), depth + 1)
        if n.is_last or n.is_root:
            break
        if n.size <= 0:
            break
        p += n.size
    return nodes


def parse_sample_chunk(d, header_off):
    """Parse a sample-chunk raw_header at header_off. Returns (version, root_nodes).
    raw_header: fileSize|flags(u32), magic(u32), offTable off32, offCount(u32)."""
    fs = u32be(d, header_off)
    assert u32be(d, header_off + 4) == SC_MAGIC, "not a sample-chunk header"
    size = fs & 0x1FFFFFFF
    limit = header_off + size if size else len(d)
    nodes_start = header_off + 0x10   # child nodes begin right after header (this+1)
    roots = _parse_children(d, nodes_start, min(limit, len(d)))
    return roots, limit


def find_node(nodes, name):
    """Depth-first search for a node by name."""
    for n in nodes:
        if n.name == name:
            return n
        found = find_node(n.children, name)
        if found:
            return found
    return None


def iter_nodes(nodes):
    for n in nodes:
        yield n
        yield from iter_nodes(n.children)


# ---------- Needle archive (NEDARCV1) ----------

def find_model_blocks(d):
    """Locate sample-chunk model headers inside a .model file (handles the
    NEDARCV1/NEDMDLV5 Needle wrapper by scanning for the sample-chunk magic).
    Returns list of header offsets (each a raw_header start, one per LOD)."""
    blocks = []
    mg = struct.pack(">I", SC_MAGIC)
    start = 0
    while True:
        i = d.find(mg, start)
        if i < 0:
            break
        h = i - 4
        if h >= 0 and (u32be(d, h) & 0x80000000):   # is_root flag set
            blocks.append(h)
        start = i + 1
    return blocks


def dump_tree(d, header_off):
    roots, limit = parse_sample_chunk(d, header_off)
    lines = [f"sample-chunk @0x{header_off:x} limit=0x{limit:x}"]

    def rec(nodes, ind):
        for n in nodes:
            lines.append(f"{'  '*ind}{n.name!r:12} val={n.value} size=0x{n.size:x} "
                         f"leaf={int(n.is_leaf)} last={int(n.is_last)} @0x{n.off:x}")
            rec(n.children, ind + 1)
    rec(roots, 1)
    return "\n".join(lines)


if __name__ == "__main__":
    import sys
    d = open(sys.argv[1], "rb").read()
    blocks = find_model_blocks(d)
    print(f"{len(blocks)} model block(s): " + ", ".join(f"0x{b:x}" for b in blocks))
    if blocks:
        print(dump_tree(d, blocks[0]))
