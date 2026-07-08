"""Shared little-endian binary reading utilities + BINA v2 helpers.

Used by all Sonic Frontiers (Hedgehog Engine 2 / Needle) format prototypes.
Kept dependency-light (struct only) so it ports cleanly to C++.
"""
import struct


class Reader:
    """Seekable little-endian binary reader over an in-memory buffer."""

    def __init__(self, data: bytes, pos: int = 0):
        self.d = data
        self.p = pos

    # -- cursor --
    def tell(self) -> int:
        return self.p

    def seek(self, pos: int):
        self.p = pos
        return self

    def skip(self, n: int):
        self.p += n
        return self

    def align(self, a: int):
        self.p = (self.p + a - 1) & ~(a - 1)
        return self

    def eof(self) -> bool:
        return self.p >= len(self.d)

    # -- scalar reads (advance cursor) --
    def u8(self):  v = self.d[self.p]; self.p += 1; return v
    def i8(self):  v = struct.unpack_from("<b", self.d, self.p)[0]; self.p += 1; return v
    def u16(self): v = struct.unpack_from("<H", self.d, self.p)[0]; self.p += 2; return v
    def i16(self): v = struct.unpack_from("<h", self.d, self.p)[0]; self.p += 2; return v
    def u32(self): v = struct.unpack_from("<I", self.d, self.p)[0]; self.p += 4; return v
    def i32(self): v = struct.unpack_from("<i", self.d, self.p)[0]; self.p += 4; return v
    def u64(self): v = struct.unpack_from("<Q", self.d, self.p)[0]; self.p += 8; return v
    def i64(self): v = struct.unpack_from("<q", self.d, self.p)[0]; self.p += 8; return v
    def f16(self): v = struct.unpack_from("<e", self.d, self.p)[0]; self.p += 2; return v
    def f32(self): v = struct.unpack_from("<f", self.d, self.p)[0]; self.p += 4; return v

    def bytes(self, n: int) -> bytes:
        v = self.d[self.p:self.p + n]; self.p += n; return v

    # -- big-endian variants (some HE2 files are big-endian 'B') --
    def u32be(self): v = struct.unpack_from(">I", self.d, self.p)[0]; self.p += 4; return v

    # -- non-advancing reads --
    def u32_at(self, pos):
        return struct.unpack_from("<I", self.d, pos)[0]

    def u64_at(self, pos):
        return struct.unpack_from("<Q", self.d, pos)[0]

    def cstr_at(self, pos) -> str:
        end = self.d.find(b"\x00", pos)
        if end < 0:
            end = len(self.d)
        return self.d[pos:end].decode("utf-8", "replace")

    def cstr(self) -> str:
        s = self.cstr_at(self.p)
        self.p += len(s.encode("utf-8", "replace")) + 1
        return s


def decode_bina_offset_table(table: bytes, base: int = 0):
    """Decode a BINA v2 offset table into a list of positions that hold pointers.

    BINA offset-table encoding (HedgeLib BINAv2): each entry's high 2 bits pick
    the delta width; the delta is in 4-byte words and is added to the running
    position. Value 0 terminates.
      0b01_xxxxxx                              -> 6-bit delta
      0b10_xxxxxx xxxxxxxx                     -> 14-bit delta
      0b11_xxxxxx xxxxxxxx xxxxxxxx xxxxxxxx   -> 30-bit delta
    Returns absolute positions (base + running offset) where a relocated
    pointer field lives.
    """
    positions = []
    pos = base
    i = 0
    n = len(table)
    while i < n:
        b = table[i]
        kind = b & 0xC0
        if kind == 0x40:
            delta = b & 0x3F
            i += 1
        elif kind == 0x80:
            delta = ((b & 0x3F) << 8) | table[i + 1]
            i += 2
        elif kind == 0xC0:
            delta = ((b & 0x3F) << 24) | (table[i + 1] << 16) | (table[i + 2] << 8) | table[i + 3]
            i += 4
        else:  # 0x00 -> terminator/padding
            break
        pos += delta * 4
        positions.append(pos)
    return positions


def hexdump(data: bytes, off: int = 0, length: int = 256, base: int = 0):
    """Return a classic hexdump string of data[off:off+length]."""
    out = []
    chunk = data[off:off + length]
    for i in range(0, len(chunk), 16):
        row = chunk[i:i + 16]
        hexs = " ".join(f"{b:02x}" for b in row)
        asci = "".join(chr(b) if 0x20 <= b < 0x7f else "." for b in row)
        out.append(f"{base + off + i:08x}  {hexs:<47}  {asci}")
    return "\n".join(out)
