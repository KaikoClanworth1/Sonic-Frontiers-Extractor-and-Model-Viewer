"""Fast global name search: list every contained file name across all pacs by
decompressing only each pac's ROOT blob (skips the multi-MB split data), then grep.
"""
import os, sys, struct, lz4.block

def u32(d, o): return struct.unpack_from("<I", d, o)[0]
def u64(d, o): return struct.unpack_from("<Q", d, o)[0]
def s32(d, o): return struct.unpack_from("<i", d, o)[0]

def cstr(d, o):
    if o == 0 or o >= len(d): return ""
    e = d.find(b"\x00", o); return d[o:e if e >= 0 else len(d)].decode("utf-8", "replace")

def lz4_chunks(comp, chunks, dst):
    if len(comp) == dst: return bytes(comp)
    out = bytearray(); s = 0
    for c, u in chunks:
        out += lz4.block.decompress(bytes(comp[s:s+c]), uncompressed_size=u); s += c
    return bytes(out)

def names_walk(b, nodes_base, idx, path, out):
    nd = nodes_base + idx * 0x28
    name_off = u64(b, nd + 0x00); data_off = u64(b, nd + 0x08)
    child_idx = u64(b, nd + 0x10); child_count = struct.unpack_from("<H", b, nd + 0x24)[0]
    has_data = b[nd + 0x26]; buf_start = b[nd + 0x27]
    if name_off: path = path[:buf_start] + cstr(b, name_off)
    if has_data and data_off:
        ext = cstr(b, u64(b, data_off + 0x20))
        out.append(path + ("." + ext if ext else ""))
    for k in range(child_count):
        names_walk(b, nodes_base, s32(b, child_idx + k * 4), path, out)

def list_names(path):
    d = open(path, "rb").read()
    if d[:4] != b"PACx" or d[4:7] != b"403": return []
    root_off, root_comp, root_unc = u32(d, 0x10), u32(d, 0x14), u32(d, 0x18)
    fv4, fv3 = struct.unpack_from("<HH", d, 0x1C)
    chunks = []
    if fv4 & 0x80:
        ps = u32(d, 0x20); co = 0x30 + ps; n = u32(d, co)
        chunks = [(u32(d, co + 4 + 8*i), u32(d, co + 4 + 8*i + 4)) for i in range(n)]
    root = lz4_chunks(d[root_off:root_off+root_comp], chunks, root_unc) if (fv3 & 0x200) else d[root_off:root_off+root_comp]
    b = root
    dnc = u32(b, 0x34); nodes_ptr = u64(b, 0x38); didx = u64(b, 0x40)
    out = []
    for di in range(dnc):
        tni = s32(b, didx + di * 4); tnode = nodes_ptr + tni * 0x28
        ft = u64(b, tnode + 0x08)
        if ft: names_walk(b, u64(b, ft + 0x08), 0, "", out)
    return out

if __name__ == "__main__":
    root = r"E:\Games\steamapps\common\SonicFrontiers\image\x64\raw"
    patterns = [p.lower() for p in sys.argv[1:]] or ["portal", "gate"]
    hits = []
    for dp, _, fs in os.walk(root):
        for f in fs:
            if not f.lower().endswith(".pac"): continue
            p = os.path.join(dp, f)
            try:
                for name in list_names(p):
                    ln = name.lower()
                    if any(pat in ln for pat in patterns):
                        hits.append((name, os.path.relpath(p, root)))
            except Exception:
                pass
    # dedupe by name, keep pac list
    from collections import defaultdict
    byname = defaultdict(set)
    for n, pac in hits: byname[n].add(pac)
    for n in sorted(byname):
        pacs = sorted(byname[n])
        print(f"{n:55} {pacs[0]}" + (f"  (+{len(pacs)-1} more pacs)" if len(pacs) > 1 else ""))
    print(f"\n{len(byname)} distinct names matching {patterns}")
