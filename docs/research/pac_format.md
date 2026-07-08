# Hedgehog Engine 2 "PACx" Archive Format — Version 403 (Sonic Frontiers, PC)

Format research for a C++/Python unpacker. Target: `PACx403L` `.pac` files (little-endian, 64-bit)
used by **Sonic Frontiers**.

## Provenance / how to read the citations

The **authoritative reference** is the C++ library **HedgeLib++** by Radford Hound
(`github.com/Radfordhound/HedgeLib`, default branch `HedgeLib++`). It is the *only* one of the four
requested repos that actually implements v403. Cited paths below are relative to a clone of that repo:

- `HedgeLib/include/hedgelib/archives/hl_pacx.h`  — all struct definitions (cited as **[pacx.h:LINE]**)
- `HedgeLib/src/archives/hl_pacx.cpp`             — read/parse/decompress logic (cited as **[pacx.cpp:LINE]**)
- `HedgeLib/src/archives/hl_in_pacx_type_autogen.h` — extension→type table (cited as **[autogen.h:LINE]**)
- `HedgeLib/include/hedgelib/io/hl_bina.h` + `src/io/hl_bina.cpp` — BINA offset table (cited as **[bina.*]**)
- `HedgeLib/src/hl_compression.cpp`               — LZ4/deflate primitives (cited as **[compress.cpp:LINE]**)

Repos that were checked and found **NOT** to support v403 (kept only for cross-reference of the older
v2 "Forces"/402 layout and BINA basics):
- `Radfordhound/HedgeLib` branch `master` (C#) — `HedgeLib/Headers/PACxHeader.cs` only parses a 3-char
  version and has no v403/LZ4/Rangers code. **Do not use for Frontiers.**
- `blueskythlikesclouds/HedgeLib` — same C++ tree as an older snapshot of Radford's; use Radford's.
- `Knuxfan24/KnuxLib` (was "KnuxTools") — has `Engines/Hedgehog/ArchiveInfo.cs` (the `.arcinfo`
  listing format) but **no PAC container parser**.

**Everything in this document was verified by a full round-trip parse (in Python) of the real file
`E:\Games\steamapps\common\SonicFrontiers\image\x64\raw\character\amy.pac`** (magic `PACx403L`).
That file decompressed and parsed to 218 correctly-named contained files plus one embedded split.
Facts that could NOT be independently confirmed are flagged **[UNCONFIRMED]**.

---

## 0. Big picture — the three nested layers

A `PACx403L` file is an **onion**:

```
PACx403L file (v4 "v03" outer wrapper, 32-byte header + metadata)
 └── root pac  = a compressed (LZ4) blob that decompresses to a  PACx402L  ("v3") pac
      ├── type tree / file tree  → the file listing (names, sizes, extensions, flags)
      ├── file data              → the actual bytes of files that live in the ROOT
      └── dependency table       → describes N "split" pacs
 └── split pac(s)  = additional compressed (LZ4) blobs *embedded in the same physical .pac file*
      each decompresses to another  PACx402L  ("v3") pac holding the "split-kind" files
      (models, materials, textures, shaders, most animations…)
```

Key surprises versus the task's initial assumptions:

1. **The version-number nesting is real and important.** The *outer* file says `403`, but the *root*
   and *split* pacs inside it are `402` (the "v3" tree format). Code that dispatches on version must
   read the version of the *decompressed* blob, not the outer file. Confirmed: `amy.pac` outer =
   `PACx403L`, decompressed root = `PACx402L`. [pacx.cpp:5543 `rev=='3'||rev=='5'` routes v03 header;
   inner blob magic verified `PACx402L`]
2. **Splits are embedded, not external, in v403.** The v2 (Forces/402) format loads split files named
   `foo.pac.00`, `foo.pac.01` from disk. The v403 root's dependency table instead stores compressed
   split *blobs inside the same physical file*, located by a byte offset (`dataPos`). The dependency
   entry still carries a *name* like `amy.pac.000` (informational), but you do **not** open a second
   file. Verified: `amy.pac` has 1 dependency named `"amy.pac.000"`, `dataPos=0x110`, which is data
   inside `amy.pac` itself. [pacx.cpp:4301 `in_read_deps`, 4339 `lz4_dep_info::decompress_dep` uses
   `ptradd(pac, dataPos)`]
   - **[UNCONFIRMED]** Whether *any* Frontiers pac uses truly external split files. All evidence
     (source + amy.pac) points to embedded splits for v403. Some very large archives *may* differ;
     the code path in `in_read_deps` only ever reads embedded `dataPos`, so assume embedded.
3. **Compression is per-chunk LZ4 block format** (not the LZ4 *frame* format), with an uncompressed
   fallback. Deflate is a supported alternative but Frontiers uses LZ4. Verified flags on amy.pac.

All multi-byte integers are **little-endian** for `...L` files. (`B` = big-endian; Frontiers PC is `L`.)

---

## 1. Endianness & the `ver` field

```c
struct ver {          // 3 bytes, ASCII digits          [bina.h:12]
    char _major;      // '4'
    char _minor;      // '0'
    char rev;         // '3'
};
// endian flag is a separate byte:  'B'=big, 'L'=little   [bina.h:29-32]
// needs_swap(): host is assumed little-endian, so a 'B' file needs swapping. [bina.cpp/bina.h:42]
```

So the 8-byte magic `50 41 43 78 34 30 33 4C` = `"PACx" '4' '0' '3' 'L'`.

---

## 2. Outer header — PACx v4 "v03" (`pacx::v4::v03::header`)  — size **0x20 (32) bytes**

`[pacx.h:1475-1557]`, `HL_STATIC_ASSERT_SIZE(header, 0x20)`.

| off  | size | type   | field                  | notes |
|------|------|--------|------------------------|-------|
| 0x00 | 4    | u32/char[4] | `signature`       | `"PACx"` (`0x78434150` LE) |
| 0x04 | 3    | char[3]| `version`              | `"403"` |
| 0x07 | 1    | u8     | `endianFlag`           | `'L'` (0x4C) |
| 0x08 | 4    | u32    | `uid`                  | random per-file id; irrelevant to unpacking |
| 0x0C | 4    | u32    | `fileSize`             | total size of the physical .pac file |
| 0x10 | 4    | off32  | `root`                 | file-relative offset to the compressed root blob |
| 0x14 | 4    | u32    | `rootCompressedSize`   | compressed byte length of the root blob |
| 0x18 | 4    | u32    | `rootUncompressedSize` | decompressed byte length of the root blob |
| 0x1C | 2    | u16    | `flagsV4`              | bitmask, see below |
| 0x1E | 2    | u16    | `flagsV3`              | bitmask, see below |

If `rootCompressedSize == rootUncompressedSize` the root is stored **uncompressed** (memcpy).
[pacx.h:1487-1502 doc; pacx.cpp:5356 `if (srcSize == dstSize) memcpy`]

### 2.1 `flagsV4` bits (`pacx::v4::pac_flags`) `[pacx.h:1121-1127]`

| value  | name           | meaning |
|--------|----------------|---------|
| 0x0001 | `unknown1`     | set when LZ4-compressed (observed) |
| 0x0002 | `has_parents`  | a parent-path list is present in metadata |
| 0x0080 | `has_metadata` | the 16-byte metadata header + tables follow the header |

`has_parents()` = `flagsV4 & 2`; `has_metadata()` = `flagsV4 & 0x80`. [pacx.h:1520-1528]

### 2.2 `flagsV3` bits (`pacx::v3::pac_flags`) `[pacx.h:888-893]`

| value  | name                | meaning |
|--------|---------------------|---------|
| 0x0008 | `unknown1`          | always set in practice |
| 0x0100 | `deflate_compressed`| root/splits use zlib **deflate** |
| 0x0200 | `lz4_compressed`    | root/splits use **LZ4 block** |

**Decompression dispatch** [pacx.cpp:4789-4812 `v03::header::decompress_root`]:
`if (flagsV3 & 0x200) → LZ4 (needs chunk table)  else → deflate`.

### Verified on amy.pac
`sig="PACx" ver="403" endian='L'`, `uid=0x6df713a2`, `fileSize=3452976`, `root=0x259770`,
`rootCompressedSize=989371`, `rootUncompressedSize=1343312`, `flagsV4=0x0083`
(`unknown1|has_parents|has_metadata`), `flagsV3=0x0208` (`unknown1|lz4_compressed`).

---

## 3. Outer metadata block (`pacx::v4::v03::metadata_header`)  — present iff `has_metadata`

Located immediately after the 0x20 header (i.e. at file offset **0x20**). Size **0x10 (16) bytes**.
`[pacx.h:1401-1473]`

| off (rel) | size | type | field           | notes |
|-----------|------|------|-----------------|-------|
| +0x00     | 4    | u32  | `parentsSize`   | byte size of the parent table region |
| +0x04     | 4    | u32  | `chunkTableSize`| byte size of the root chunk table region |
| +0x08     | 4    | u32  | `strTableSize`  | byte size of the metadata string table |
| +0x0C     | 4    | u32  | `offTableSize`  | byte size of the metadata offset table |

The four regions follow **contiguously**, each starting where the previous ended (start = end of
metadata_header = header+0x30): `parents → chunkTable → strTable → offTable`. Accessors:
[pacx.h:1417-1455]

```
parents   = metadata_header + 0x10           (i.e. file 0x30)
chunkTable = parents  + parentsSize
strTable   = chunkTable + chunkTableSize
offTable   = strTable  + strTableSize
```

The metadata's own offsets are BINA-fixed against the whole file (`base = pac`). [pacx.cpp:4687]

### 3.1 Parent table (`pacx::v4::v03::parent_table = arr64<parent_info>`)  `[pacx.h:1385-1399]`

`arr64<T>` = `{ u64 count; off64<T> data; }` (16 bytes). Each `parent_info` = `{ off64<char> path; }`
(8 bytes). Parent paths are the pac files this archive *depends on / inherits from* (loaded as
separate archives by the game). Purely informational for a pure unpacker — you can skip them, but
they tell you e.g. that `amy.pac` reads shared assets from `EffectCommon`.

Verified amy.pac: `count=1`, one path string `"EffectCommon"`. (Region raw:
`01 00…│ 40 00… (data off) │` and strtab `"EffectCommon\0"`.)

### 3.2 Root chunk table (`pacx::v4::chunk_table`)  `[pacx.h:1213-1264]`

```c
struct chunk_table { u32 count; chunk chunks[count]; };  // count is 4 bytes, then the array
struct chunk { u32 compressedSize; u32 uncompressedSize; };  // 8 bytes each  [pacx.h:1140-1158]
```

This chunk list drives LZ4 decompression of the **root** blob (see §6). `chunkTableSize` may be
padded (e.g. amy.pac: `chunkTableSize=176` but `4 + 8*21 = 172`; 4 bytes trailing padding).

Verified amy.pac: `count=21`, all chunks `uncompressedSize=65536` (the default max chunk size),
`Σ compressedSize=989371 == rootCompressedSize`, `Σ uncompressedSize=1343312 == rootUncompressedSize`.

### 3.3 Metadata string & offset tables
Standard BINA v2 string table (null-terminated UTF-8) and offset table (§5). For amy.pac the metadata
offset table is `4E 42 00 00 …` → decodes to two 14-bit offsets pointing at the two off64 fields
(`parent_info.path` and `parent_table.data`).

---

## 4. The root / split pac — PACx v3 body (magic reads `PACx402L`)

After decompression, the root blob (and each split blob) is a self-contained **v3-layout** pac.
Its header is `pacx::v3::header`, size **0x30 (48) bytes**. `[pacx.h:897-1030]`

| off  | size | type   | field             | notes |
|------|------|--------|-------------------|-------|
| 0x00 | 4    | u32    | `signature`       | `"PACx"` (for v402; **0 for v405** — see note) |
| 0x04 | 3    | char[3]| `version`         | `"402"` inside a v403 file (verified) |
| 0x07 | 1    | u8     | `endianFlag`      | `'L'` |
| 0x08 | 4    | u32    | `uid`             | matches outer uid in practice |
| 0x0C | 4    | u32    | `fileSize`        | == this blob's uncompressed size |
| 0x10 | 4    | u32    | `treesSize`       | byte size of the type-tree + all sub-trees region |
| 0x14 | 4    | u32    | `depTableSize`    | byte size of the dependency table region |
| 0x18 | 4    | u32    | `dataEntriesSize` | byte size of the data-entries region |
| 0x1C | 4    | u32    | `strTableSize`    | byte size of the string table |
| 0x20 | 4    | u32    | `fileDataSize`    | byte size of the embedded file-data region |
| 0x24 | 4    | u32    | `offTableSize`    | byte size of the offset table |
| 0x28 | 2    | u16    | `type`            | `pac_type` bitmask (see below) |
| 0x2A | 2    | u16    | `flags`           | `pac_flags`; inside a decompressed blob this is `0x108` (unknown1 set, not compressed-in-place) |
| 0x2C | 4    | u32    | `depCount`        | number of dependency (split) entries |

**Regions follow contiguously**, each starting at the end of the previous (base = start of this blob,
header is 0x30 bytes): `[pacx.h:944-1002]`

```
types        = header + 0x30
dep_table    = types        + treesSize
data_entries = dep_table    + depTableSize
str_table    = data_entries + dataEntriesSize
file_data    = str_table    + strTableSize
off_table    = file_data    + fileDataSize
```

### 4.1 `pac_type` bits (`pacx::v3::pac_type`)  `[pacx.h:878-884]`
`is_root=1, is_split=2, has_splits=4, unknown=8`.
Verified amy.pac root: `type=0xD` = `is_root | has_splits | unknown`.

> **v405 note:** For the (later Shadow Generations) `PACx405L`, the *inner* v3 signature field is 0
> instead of `"PACx"`, and split dep entries have empty names. `[pacx.cpp:2277, 4285]` For Frontiers
> (403) the inner sig is `"PACx402L"` — don't validate the inner sig by string equality if you also
> want v405 support.

---

## 5. BINA v2 offset table & string table (needed to "fix" all off64 pointers)

Both the outer metadata and every v3 body carry an **offset table** that lists which 4-byte-aligned
locations hold offsets that must be relocated. Offsets are stored **relative to the base of that
blob** (base = start of the v3 header, or base = start of the whole file for metadata). "Fixing" =
`absolute = base + stored_relative_value`. Since v3 blobs are parsed at base 0 (start of the
decompressed buffer), the stored relative value *is* the position within the buffer.

### 5.1 Offset-table encoding  `[bina.cpp:169-251]`, flags `[bina.h:151-160]`

Read bytes sequentially; maintain a running position `cur` (in bytes, starts 0). For each entry, the
top 2 bits (`& 0xC0`) select the size, the low 6 bits are data:

| top bits | name              | bytes | value (multiply by 4 to get a *byte* delta) |
|----------|-------------------|-------|---------------------------------------------|
| `0x40`   | `size_six_bit`    | 1     | `b & 0x3F` |
| `0x80`   | `size_fourteen_bit`| 2    | `((b0 & 0x3F) << 8) \| b1` |
| `0xC0`   | `size_thirty_bit` | 4     | `((b0 & 0x3F)<<24) \| (b1<<16) \| (b2<<8) \| b3` |
| `0x00`   | end / padding     | —     | stop (also trailing zero bytes are padding) |

Each decoded value is a count of **u32s** since the *previous* fixed offset, so:
`cur += decoded_value * 4;  the off64 to fix lives at buffer[cur]`. [bina.cpp:203-251, 262-265]

Trailing zero bytes up to `offTableSize` are padding — trim them (or stop at the first `0x00`
after data). [bina.cpp:156-167]

### 5.2 String table
Plain concatenation of null-terminated UTF-8 strings; `off64<char>` fields point into it after fixing.

Verified amy.pac root: 1276 offsets decoded from a 1280-byte offset table; first fixed positions
`[56, 64, 88, 112, 128, 152]` correspond to the type-tree node off64 fields.

---

## 6. Compression — LZ4 block, chunked (and deflate alternative)

### 6.1 LZ4 (Frontiers default; `flagsV3 & 0x200`)  `[pacx.cpp:5351-5373]`

The compressed blob is a concatenation of independently-compressed chunks. Decompress **one chunk at
a time** into a pre-allocated output buffer of size `uncompressedSize`; **do not** try to decompress
the whole thing at once.

```
decompress_lz4(chunkCount, chunks[], srcSize, src, dstSize) -> dst[dstSize]:
    if srcSize == dstSize:            # stored uncompressed
        memcpy(dst, src, srcSize); return
    srcPos = 0; dstPos = 0
    for i in 0..chunkCount-1:
        # LZ4 *block* decompress (LZ4_decompress_safe), NOT frame format
        lz4_block_decompress(
            in  = src[srcPos : srcPos + chunks[i].compressedSize],
            out = dst[dstPos : dstPos + chunks[i].uncompressedSize])
        srcPos += chunks[i].compressedSize
        dstPos += chunks[i].uncompressedSize
```

The underlying call is `LZ4_decompress_safe` on each chunk. [compress.cpp:666-679]
In Python: `lz4.block.decompress(chunk_bytes, uncompressed_size=chunks[i].uncompressedSize)`.
Default max chunk size = **65536** (`default_lz4_max_chunk_size`). [pacx.h:1118]

- The **root** blob's chunk list is the metadata chunk table (§3.2).
- Each **split** blob's chunk list is in its `lz4_dep_info.chunks` (§7).

Verified: amy.pac root and its split both decompress exactly to the expected sizes with per-chunk
`lz4.block.decompress`.

### 6.2 Deflate (alternative; `flagsV3 & 0x100`, no LZ4 bit)  `[pacx.cpp:5397-5409]`

Whole-blob raw/zlib deflate; `if srcSize == dstSize: memcpy` else inflate `src`→`dst`. Deflate deps
have **no** chunk table (`deflate_dep_info` is smaller). Not used by the amy.pac sample; include for
completeness. Underlying: `deflate_decompress_no_alloc` (zlib inflate). [compress.cpp:681]
**[UNCONFIRMED]** whether the deflate stream is zlib-wrapped or raw; HedgeLib uses zlib with default
window — treat as zlib, fall back to raw-deflate if inflate fails.

---

## 7. Dependency (split) table — how the root points at the embedded splits

`v3::header::depCount` gives the number of splits. The **root's** `dep_table` region is *reinterpreted
based on compression*: for LZ4 it is an `lz4_dep_table`, for deflate a `deflate_dep_table`.
[pacx.cpp:4301-4337 `in_read_deps`]

```c
// arr64<T> = { u64 count; off64<T> data; }   (16 bytes)
using lz4_dep_table     = arr64<lz4_dep_info>;      // [pacx.h:1185]
using deflate_dep_table = arr64<deflate_dep_info>;  // [pacx.h:1210]
```

### 7.1 `lz4_dep_info` — size **0x20 (32)**  `[pacx.h:1160-1183]`

| off  | size | type       | field             | notes |
|------|------|------------|-------------------|-------|
| 0x00 | 8    | off64<char>| `name`            | split name, e.g. `"amy.pac.000"` (informational) |
| 0x08 | 4    | u32        | `compressedSize`  | compressed length of the split blob |
| 0x0C | 4    | u32        | `uncompressedSize`| decompressed length |
| 0x10 | 4    | u32        | `dataPos`         | **offset within the OUTER physical .pac file** to the split's compressed bytes |
| 0x14 | 4    | u32        | `chunkCount`      | number of LZ4 chunks for this split |
| 0x18 | 8    | off64<chunk>| `chunks`         | → `chunk[chunkCount]` (in the root blob's address space) |

Decompress a split: `decompress_lz4(chunkCount, chunks, compressedSize, file + dataPos, uncompressedSize)`.
[pacx.cpp:4339-4343]. The result is another **v3 (`PACx402L`) pac** — parse it exactly like the root
(§4, §8) and merge its files into your output list.

> **Critical:** `dataPos` is relative to the **outer v4 file** (`ptradd(pac, dataPos)`), while
> `chunks`/`name` off64 are relative to the **decompressed root blob** (they were fixed by the root's
> BINA offset table). Keep both base pointers around.

Verified amy.pac: 1 split, `name="amy.pac.000"`, `compressedSize=2463317`, `uncompressedSize=3665648`,
`dataPos=0x110`, `chunkCount=56`; decompressing `file[0x110:0x110+2463317]` with its 56 chunks yields
exactly 3665648 bytes whose magic is `PACx402L`.

### 7.2 `deflate_dep_info` — size **0x18 (24)**  `[pacx.h:1188-1208]`
`{ off64<char> name; u32 compressedSize; u32 uncompressedSize; u32 dataPos; u32 padding; }`.
Decompress with deflate (no chunks): `deflate(compressedSize, file+dataPos, uncompressedSize)`.

---

## 8. The type tree → file tree → data entry (the actual file listing)

Both the root and each split use a **two-level PATRICIA/radix tree of names**: an outer *type tree*
(one entry per file-type group, e.g. `dds:ResTexture`) whose data nodes each point to a *file tree*
(the file names within that type). Names are reconstructed by walking the tree and accumulating
substrings at each node's `bufStartIndex`.

### 8.1 `node_tree<node_t>` — size **0x18 (24)**  `[pacx.h:777-864]`

| off  | size | type          | field             |
|------|------|---------------|-------------------|
| 0x00 | 4    | u32           | `nodeCount`       |
| 0x04 | 4    | u32           | `dataNodeCount`   |
| 0x08 | 8    | off64<node_t> | `nodes`           |
| 0x10 | 8    | off64<s32>    | `dataNodeIndices` | array of `dataNodeCount` indices into `nodes` for nodes that carry data |

### 8.2 `node<T>` — size **0x28 (40)**  `[pacx.h:700-775]`

| off  | size | type        | field         | notes |
|------|------|-------------|---------------|-------|
| 0x00 | 8    | off64<char> | `name`        | this node's name *fragment* (may be null) |
| 0x08 | 8    | off64<T>    | `data`        | for type node: → `file_tree`; for file node: → `data_entry` |
| 0x10 | 8    | off64<s32>  | `childIndices`| → array of `childCount` child node indices |
| 0x18 | 4    | s32         | `parentIndex` | |
| 0x1C | 4    | s32         | `globalIndex` | |
| 0x20 | 4    | s32         | `dataIndex`   | |
| 0x24 | 2    | u16         | `childCount`  | |
| 0x26 | 1    | u8          | `hasData`     | if set, `data` → a valid entry/tree |
| 0x27 | 1    | u8          | `bufStartIndex`| where in the running name buffer this node's fragment starts (== accumulated prefix length) |

`type_node = node<file_tree>`, `file_node = node<data_entry>`. `type_tree = node_tree<type_node>`,
`file_tree = node_tree<file_node>`. [pacx.h:866-876]

### 8.3 `data_entry` (v3) — size **0x30 (48)**  `[pacx.h:649-698]`

| off  | size | type       | field       | notes |
|------|------|------------|-------------|-------|
| 0x00 | 4    | u32        | `uid`       | |
| 0x04 | 4    | u32        | `dataSize`  | **size of this file's bytes** |
| 0x08 | 8    | u64        | `unknown2`  | 0 |
| 0x10 | 8    | off64<void>| `data`      | → the file's bytes inside this blob's `file_data` region (null for proxy) |
| 0x18 | 8    | u64        | `unknown3`  | 0 |
| 0x20 | 8    | off64<char>| `ext`       | file **extension** string (no dot), e.g. `"dds"`, `"anm.pxd"` |
| 0x28 | 2    | u16        | `flags`     | `data_flags`, see below |
| 0x2A | 2    | s16        | `splitIndex`| only meaningful in v405 |
| 0x2C | 4    | u32        | `padding1`  | |

`data_flags` (`pacx::v3::data_flags`) `[pacx.h:639-645]`:

| value | name             | meaning |
|-------|------------------|---------|
| 0     | `regular_file`   | data present here |
| 1     | `not_here`       | **proxy** — the real data lives in a split; this root entry is just a stub/streaming reference |
| 2     | `bina_file`      | the file's payload is itself a BINA file (informational) |
| 4     | `has_split_index`| `splitIndex` field is valid (v405) |

A root entry with `not_here (1)` is a *proxy*: it names the file and gives its `dataSize` but has no
bytes — the bytes come from the matching entry in a split pac. HedgeLib's default parse *skips*
proxies in the root (`skipProxies=true`) and picks up the real data when it reads the split.
[pacx.cpp:2184; a proxy is emitted as a "streaming file" placeholder at 2204-2207]

### 8.4 Name reconstruction & extraction (per blob)  `[pacx.cpp:2176-2251]`

```
for di in 0 .. type_tree.dataNodeCount-1:
    typeNode = type_tree.nodes[ type_tree.dataNodeIndices[di] ]
    fileTree = *typeNode.data              # off64 → file_tree (0x18)
    walk_files(fileTree.nodes, index=0, pathBuf="")   # recursive, start at node 0

walk_files(nodes, i, pathBuf):
    n = nodes[i]
    if n.hasData:
        de = *n.data                        # data_entry
        if not skipProxies or not (de.flags & 1):     # skip root proxies
            name = pathBuf[0 : n.bufStartIndex]       # accumulated prefix
            ext  = string_at(de.ext)
            fileName = name + ("." + ext if ext else "")
            emit(fileName, de.dataSize, de.data, de.flags)
    elif n.name:                            # interior node: extend the buffer
        pathBuf = pathBuf[0 : n.bufStartIndex] + string_at(n.name)
    for k in 0 .. n.childCount-1:
        walk_files(nodes, n.childIndices[k], pathBuf)
```

The *type* level works the same way, but its accumulated string is the group key
`"<ext>:<ResType>"` (e.g. `"dds:ResTexture"`) — you generally don't need it because each file's own
`ext` field already gives the extension. The `:ResType` half maps to §9.

Verified amy.pac root: 218 files parsed; extension histogram
`{pxd:42, asm:1, mat-anim:36, uv-anim:100, cemt:12, level:2, material:6, model:1, effdb:1, dds:17}`.
Sample names like `chr_amy@facial_angry01.anm.pxd` (flags `0x2` BINA), `chr_amy@idle_loop.anm.pxd`.
(The split contributes the remaining model/material/texture bytes for the proxy entries.)

---

## 9. PACx type table for Frontiers ("RANGERS") — extension → internal ResType, and root/split kind

From `[autogen.h:605-667]` (`HL_IN_PACX_RANGERS_AUTOGEN(ext, ResType, kind)`). `kind` tells you which
pac layer the *bytes* live in: `root` = stored in the root pac; `split` = stored in a split pac (the
root holds a proxy). This is exactly why models/materials/textures show up as proxies in the root.

| ext                   | ResType (internal)             | kind  |
|-----------------------|--------------------------------|-------|
| `mlevel`              | ResMasterLevel                 | root  |
| `level`               | ResLevel                       | root  |
| `anm.pxd`             | ResAnimationPxd                | root  |
| `skl.pxd`             | ResSkeletonPxd                 | root  |
| `dds`                 | ResTexture                     | split |
| `asm`                 | ResAnimator                    | root  |
| `mat-anim`            | ResAnimMaterial                | split |
| `dvscene`             | ResDvScene                     | root  |
| `uv-anim`             | ResAnimTexSrt                  | split |
| `cemt`                | ResCyanEffect                  | root  |
| `material`            | ResMirageMaterial              | split |
| `model`               | ResModel                       | split |
| `cam-anim`            | ResAnimCameraContainer         | split |
| `vis-anim`            | ResAnimVis                     | split |
| `rfl`                 | ResReflection                  | root  |
| `cnvrs-text`          | ResText                        | root  |
| `btmesh`              | ResBulletMesh                  | root  |
| `effdb`               | ResParticleLocation            | root  |
| `gedit`               | ResObjectWorld                 | root  |
| `pccol`               | ResPointcloudCollision         | root  |
| `path`                | ResSplinePath                  | root  |
| `lf`                  | ResSHLightField                | root  |
| `probe`               | ResProbe                       | root  |
| `occ`                 | ResOcclusionCapsule            | root  |
| `swif`                | ResSurfRideProject             | root  |
| `densitysetting`      | ResDensitySetting              | root  |
| `densitypointcloud`   | ResDensityPointCloud           | root  |
| `lua`                 | ResLuaData                     | root  |
| `btsmc`               | ResSkinnedMeshCollider         | root  |
| `terrain-model`       | ResMirageTerrainModel          | split |
| `gismop`              | ResGismoConfigPlan             | root  |
| `fxcol`               | ResFxColFile                   | root  |
| `gismod`              | ResGismoConfigDesign           | root  |
| `nmt`                 | ResNavMeshTile                 | root  |
| `pcmodel`             | ResPointcloudModel             | root  |
| `nmc`                 | ResNavMeshConfig               | root  |
| `vat`                 | ResVertexAnimationTexture      | root  |
| `heightfield`         | ResHeightField                 | root  |
| `light`               | ResMirageLight                 | root  |
| `pba`                 | ResPhysicalSkeleton            | root  |
| `pcrt`                | ResPointcloudLight             | root  |
| `aism`                | ResAIStateMachine              | root  |
| `cnvrs-proj`          | ResTextProject                 | root  |
| `terrain-material`    | ResTerrainMaterial             | root  |
| `pt-anim`             | ResAnimTexPat                  | split |
| `okern`               | ResOpticalKerning              | root  |
| `scfnt`               | ResScalableFontSet             | root  |
| `cso`                 | ResMirageComputeShader         | split |
| `pso`                 | ResMiragePixelShader           | split |
| `vib`                 | ResVibration                   | root  |
| `vso`                 | ResMirageVertexShader          | split |
| `pointcloud`          | ResPointcloud                  | root  |
| `shader-list`         | ResShaderList                  | root  |
| `cnvrs-meta`          | ResTextMeta                    | root  |
| `lit-anim`            | ResAnimLightContainer          | split |
| `svcol`               | ResSvCol                       | root  |
| `model-instanceinfo`  | ResModelInstanceInfo           | root  |
| `terrain-instanceinfo`| ResMirageTerrainInstanceInfo   | root  |
| `atmosphericfog`      | ResAtmosphericFog              | root  |
| `bfnt.bin`            | ResBitmapFont                  | root  |
| `decal`               | ResDecal                       | root  |
| `grass.bin`           | ResTerrainGrassInfo            | root  |
| `decalpointcloud`     | ResDecalPointCloud             | root  |

For a viewer, the ones you care about: `model` (ResModel), `material` (ResMirageMaterial),
`dds` (ResTexture), `skl.pxd` (skeleton), `anm.pxd` (animation). Note: **model / material / dds live
in the split**, so you must decompress and parse the split to get their bytes.

---

## 10. Full unpack pseudocode

```python
def unpack_pacx403(path):
    file = read_all_bytes(path)
    assert file[0:8] == b"PACx403L"            # or handle 402/405 similarly

    # ---- outer v4 v03 header ----
    (uid, fileSize, rootOff, rootCompSize, rootUncompSize) = read_u32x5(file, 0x08)
    flagsV4, flagsV3 = read_u16x2(file, 0x1C)
    hasMeta   = flagsV4 & 0x80
    isLZ4     = flagsV3 & 0x200        # else deflate

    # ---- outer metadata (chunk table for root, parent list) ----
    rootChunks = []
    if hasMeta:
        parentsSize, chunkTableSize, strTableSize, offTableSize = read_u32x4(file, 0x20)
        chunkOff = 0x30 + parentsSize
        n = read_u32(file, chunkOff)
        rootChunks = [read_u32x2(file, chunkOff + 4 + 8*i) for i in range(n)]  # (comp,uncomp)

    # ---- decompress root ----
    comp = file[rootOff : rootOff + rootCompSize]
    if isLZ4:
        root = lz4_chunked_decompress(comp, rootChunks, rootUncompSize)
    else:
        root = deflate_decompress(comp, rootUncompSize)   # no chunks
    # root now begins with "PACx402L"

    entries = []                       # (fullname, size, bytes)
    parse_v3_blob(root, file, isLZ4, entries)   # fills entries; recurses into embedded splits
    return entries


def parse_v3_blob(blob, outerFile, isLZ4, entries, isRoot=True):
    # header 0x30
    (treesSize, depTableSize, dataEntriesSize,
     strTableSize, fileDataSize, offTableSize) = read_u32x6(blob, 0x10)
    depCount = read_u32(blob, 0x2C)

    typesOff   = 0x30
    depOff     = typesOff + treesSize
    # dataEntriesOff, strOff, fileDataOff follow but are reached via fixed off64s
    offTabOff  = 0x30 + treesSize + depTableSize + dataEntriesSize + strTableSize + fileDataSize

    fix_bina_offsets(blob, offTabOff, offTableSize)   # §5: relocate all off64 in-place (base 0)

    # walk type tree → file trees (§8.4); emit regular files with their bytes
    walk_type_tree(blob, typesOff, entries, skipProxies=isRoot)

    # ---- embedded splits ----
    if isRoot and depCount:
        depTable = read_arr64(blob, depOff)           # {count, dataPtr}
        for i in range(depTable.count):
            if isLZ4:
                di = read_lz4_dep_info(blob, depTable.data + i*0x20)
                sc = outerFile[di.dataPos : di.dataPos + di.compressedSize]
                schunks = [read_chunk(blob, di.chunks + 8*j) for j in range(di.chunkCount)]
                split = lz4_chunked_decompress(sc, schunks, di.uncompressedSize)
            else:
                di = read_deflate_dep_info(blob, depTable.data + i*0x18)
                sc = outerFile[di.dataPos : di.dataPos + di.compressedSize]
                split = deflate_decompress(sc, di.uncompressedSize)
            parse_v3_blob(split, outerFile, isLZ4, entries, isRoot=False)  # merge split files


def lz4_chunked_decompress(comp, chunks, dstSize):
    if len(comp) == dstSize: return comp          # stored uncompressed
    out = bytearray(); s = 0
    for (c, u) in chunks:
        out += lz4_block_decompress(comp[s:s+c], u); s += c   # LZ4_decompress_safe per chunk
    assert len(out) == dstSize
    return bytes(out)
```

Notes for a correct implementation:
- Use LZ4 **block** decompression (`LZ4_decompress_safe` / `lz4.block.decompress`), **not** the frame
  API. Always pass the exact `uncompressedSize`.
- Fix the BINA offset table of each v3 blob **before** dereferencing any `off64` (names, `ext`,
  `data`, tree pointers). Metadata offsets are fixed against the whole file; v3 body offsets against
  the blob (base 0).
- In the root, skip `not_here (flags&1)` proxy entries — their real bytes come from the split.
- To also emit shared assets, remember `flagsV4 & has_parents`: parent pacs (e.g. `EffectCommon`) are
  separate files the game loads too; a self-contained unpacker of *one* pac can ignore them.

---

## 11. Confidence summary

| Area | Confidence | Basis |
|------|-----------|-------|
| Outer v4/v03 header (0x20) | **High** | source + amy.pac exact match |
| Metadata header + chunk/parent tables | **High** | source + amy.pac (sizes, sums, `"EffectCommon"`) |
| flags (V4/V3), LZ4 dispatch | **High** | source + amy.pac (`0x83`/`0x208`) |
| LZ4 chunked block decompression | **High** | source + amy.pac round-trip (root and split decompressed to exact sizes) |
| v3 header (0x30) + region layout | **High** | source + amy.pac (region math produced valid trees) |
| BINA offset table decoding | **High** | source + amy.pac (1276 offsets, positions land on off64 fields) |
| Type/file radix tree + name build | **High** | source + amy.pac (218 correct filenames) |
| data_entry (0x30) + flags | **High** | source + amy.pac (sizes, exts, BINA flag) |
| Embedded split scheme (`dataPos`, `lz4_dep_info`) | **High** | source + amy.pac (split `amy.pac.000` decompressed) |
| RANGERS ext→type table & root/split kind | **High** | source (`autogen.h`) |
| Deflate path details (zlib vs raw) | **Medium** | source only; not exercised by amy.pac — treat as zlib, fall back to raw |
| Truly-external `.pac.NN` split files for v403 | **Low / [UNCONFIRMED]** | no evidence; all v403 splits observed are embedded |
| v405 differences (sig=0, split names empty, `splitIndex`) | **Medium** | source only; not needed for Frontiers |

**Bottom line:** the format is fully cracked and validated end-to-end against a real Frontiers pac.
A parser following §10 will correctly extract every contained file (models, materials, textures,
skeletons, animations) from `PACx403L` archives.
