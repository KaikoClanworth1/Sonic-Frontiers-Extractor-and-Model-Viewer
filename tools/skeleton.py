"""Sonic Frontiers .skl.pxd skeleton parser (PXSK / BINA210L, little-endian).

Returns bones as (name, parent_index, translation, quat_xyzw, scale) with
parent-local (bind) transforms.
"""
import struct


def _u32(d, o): return struct.unpack_from("<I", d, o)[0]
def _u64(d, o): return struct.unpack_from("<Q", d, o)[0]
def _s16(d, o): return struct.unpack_from("<h", d, o)[0]
def _f32(d, o): return struct.unpack_from("<f", d, o)[0]


def _cstr(d, o):
    e = d.find(b"\x00", o)
    return d[o:e if e >= 0 else len(d)].decode("utf-8", "replace")


class Bone:
    __slots__ = ("name", "parent", "translation", "rotation", "scale")

    def __init__(self, name, parent, t, r, s):
        self.name = name
        self.parent = parent
        self.translation = t         # (x,y,z)
        self.rotation = r            # quaternion (x,y,z,w) as stored
        self.scale = s               # (x,y,z)


def parse_skeleton(data):
    B = 0x40
    assert data[B:B + 4] == b"KSXP", f"bad skeleton magic {data[B:B+4]!r}"
    parent_arr = _u32(data, B + 0x08) + B
    count = _u32(data, B + 0x10)          # parentCapacity/count (low u32)
    name_tab = _u32(data, B + 0x28) + B   # 0x40+0x28 = 0x68
    mtx_tab = _u32(data, B + 0x48) + B    # 0x40+0x48 = 0x88

    bones = []
    for i in range(count):
        parent = _s16(data, parent_arr + i * 2)
        name_ptr = _u64(data, name_tab + i * 0x10) + B
        name = _cstr(data, name_ptr)
        mo = mtx_tab + i * 0x30
        t = (_f32(data, mo + 0), _f32(data, mo + 4), _f32(data, mo + 8))
        r = (_f32(data, mo + 0x10), _f32(data, mo + 0x14),
             _f32(data, mo + 0x18), _f32(data, mo + 0x1C))     # x,y,z,w
        s = (_f32(data, mo + 0x20), _f32(data, mo + 0x24), _f32(data, mo + 0x28))
        bones.append(Bone(name, parent, t, r, s))
    return bones


if __name__ == "__main__":
    import sys, math
    data = open(sys.argv[1], "rb").read()
    bones = parse_skeleton(data)
    print(f"{len(bones)} bones")
    roots = [b for b in bones if b.parent < 0]
    print(f"roots: {[b.name for b in roots]}")
    bad = 0
    for b in bones:
        if not all(map(math.isfinite, b.translation + b.rotation + b.scale)):
            bad += 1
        if not (-1 <= b.parent < len(bones)):
            bad += 1
    print("sanity:", "OK" if bad == 0 else f"{bad} bad bones")
    for b in bones[:12]:
        print(f"  {b.name:20} parent={b.parent:4} t={tuple(round(x,3) for x in b.translation)} "
              f"q={tuple(round(x,3) for x in b.rotation)}")
