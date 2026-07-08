# Hedgehog Engine 2 — Skeleton & Animation Format (Sonic Frontiers)

Research spec for a C++/Python parser of `.skl.pxd` skeletons and `.anm.pxd`
skeletal animations, targeting FBX export with an armature + animation
actions/takes.

**Scope of this document:** Skeleton (`.skl.pxd`) and skeletal animation
(`.anm.pxd`). Camera/UV/material animations (`.cam-anim`, `.uv-anim`,
`.mat-anim`) are noted briefly at the end but not fully specified.

---

## 0. Confidence legend

Every claim below is tagged:

- **[CONFIRMED]** — Directly read from parser/exporter source code that
  round-trips real game files. High confidence.
- **[INFERRED]** — Strongly implied by source + how the code uses the value,
  but not a labelled struct field. Medium confidence.
- **[UNKNOWN]** — Not determined from available sources; needs a real file to
  verify.

### Primary sources (all cloned & read locally)

| Tag | Repo | File | What it proves |
|-----|------|------|----------------|
| **[S1]** | `AdelQue/FrontiersAnimDecompress` (a.k.a. PXDAnimationTools, v2.1.5) | `Blender/FrontiersAnimationTools/skeleton/skeleton_import.py` | `.skl.pxd` read layout |
| **[S2]** | same | `Blender/FrontiersAnimationTools/skeleton/skeleton_export.py` | `.skl.pxd` write layout (authoritative struct) |
| **[S3]** | same | `Blender/FrontiersAnimationTools/animation/anim_import.py` | `.anm.pxd` header + track read |
| **[S4]** | same | `Blender/FrontiersAnimationTools/animation/anim_export.py` | `.anm.pxd` write layout (authoritative) |
| **[S5]** | same | `Blender/FrontiersAnimationTools/FrontiersAnimDecompress/process_buffer.py` | ACL compressed + decompressed buffer structs (documented in code) |
| **[S6]** | `WistfulHopes/FrontiersAnimDecompress` & `AdelQue/...` | `FrontiersAnimDecompress/FrontiersAnimDecompress.cpp` | ACL decode via nfrechette/acl; decompressed byte layout |
| **[S7]** | `Turk645/Hedgehog-Engine-2-Mesh-Blender-Importer` | `io_import_hedgehog_engine.py` | `.model` bone table + skin bone-index → skeleton binding |
| **[S8]** | `Turk645/...` | `io_import_hedgehog_engine_anim.py` | Legacy (uncompressed) `.anm.pxd` reader |
| **[S9]** | `blueskythlikesclouds/ModelConverter` | `Source/ModelConverter/Node.h` | model-embedded node/bone struct (`parentIndex`, `name`, `Float4x4`) |

Community docs consulted: HEModdingWiki "Sonic Frontiers Animation Tools",
HedgeDocs (SPA — content not machine-fetchable), the two FrontiersAnimDecompress
READMEs. History: PXD scripts first by **Turk645** (M&S Tokyo 2020, uncompressed);
from *Origins* on, tracks became **ACL-compressed** (identified by **ik-01**);
**WistfulHopes** wrote FrontiersAnimDecompress (ACL wrapper DLL); **AdelQue**
packaged it into the current Blender add-on.

> **Big-picture reliability:** Skeletons parse **reliably and completely** from
> source. Animations parse reliably **but require an ACL decoder** (nfrechette's
> `acl` library) to turn compressed track blobs into per-frame TRS — the PXD
> container around the ACL blob is fully understood; the ACL blob itself is a
> third-party format you link, not reimplement.

---

## 1. Container: BINA (v2) little-endian

Both `.skl.pxd` and `.anm.pxd` are **BINA** files (Sega's "Binary Data" v2
container) wrapping a payload. All multi-byte integers/floats are
**little-endian** (the `210L` magic — `L` = little). This is the same container
family HedgeLib/HedgeArcPack handle. **[CONFIRMED S2/S4]**

### 1.1 BINA header (0x00–0x10)  **[CONFIRMED S2, S4]**

| Off | Size | Field | Value |
|-----|------|-------|-------|
| 0x00 | 8 | `magic` | ASCII `"BINA210L"` |
| 0x08 | 4 | `fileSize` | total file size in bytes |
| 0x0C | 4 | `blockCount` | `1` |

### 1.2 DATA block header (0x10–0x40)  **[CONFIRMED S2, S4]**

| Off | Size | Field | Notes |
|-----|------|-------|-------|
| 0x10 | 4 | `magic` | ASCII `"DATA"` |
| 0x14 | 4 | `dataSize` | `fileSize - 0x10` |
| 0x18 | 4 | `stringTableOffset` | offset (rel. to 0x40) to string table; = end of the last data array. For anim, encodes payload size. |
| 0x1C | 4 | `stringTableSize` | bytes |
| 0x20 | 4 | `offsetTableSize` | size of the pointer-relocation table |
| 0x24 | 4 | `additionalDataSize` | `0x18` |
| 0x28 | 0x18 | padding | zeros |

> **KEY OFFSET RULE:** The payload starts at **0x40**. Every internal pointer
> stored in the payload is **relative to 0x40** — parsers add `0x40` to each
> raw pointer they read. This `+0x40` appears on *every* offset read in
> [S1]/[S3]. **[CONFIRMED S1, S3]**

### 1.3 BINA offset (relocation) table  **[INFERRED S2]**

After the payload + string table comes the BINA offset table: a run of bytes
encoding, in a 2-bit-prefix varint scheme, the delta between successive absolute
pointer fields so the engine can relocate them at load. Encoding (from the
skeleton exporter `offset_table()` [S2]):

- Top 2 bits of each group select stride width: `01` = 6-bit value (×4 bytes,
  small gap), `10` = 14-bit, `11` = 30-bit. Remaining bits = `offset >> 2`.
- In the skeleton file the sequence is: one entry to the parent-array pointer
  region, then repeated single-byte `0x44` entries (one per bone-name pointer),
  terminated by `0x00`, padded to 4 bytes.

**You do NOT need to parse the offset table to read data** — it is only for the
engine's in-place relocation. A read-only parser ignores it and uses the
explicit offsets in the payload. **[CONFIRMED — both importers ignore it]**

---

## 2. Skeleton — `.skl.pxd`

Payload magic `KSXP` (= "PXSK" little-endian-reversed). Layout below is the
payload at file offset **0x40**; all internal pointers are stored relative to
0x40, so add 0x40 after reading. **[CONFIRMED S1, S2]**

### 2.1 PXSK payload header (starts at 0x40)  **[CONFIRMED S1, S2]**

The payload is three identical 0x20-byte "array descriptors" (offset, capacity,
count, pad), one each for parent indices, names, and transforms. Byte-verified
against the exporter's writes [S2 lines 100-133] and the importer's reads [S1]:

| File off | Size | Field | Value / meaning |
|----------|------|-------|-----------------|
| 0x40 | 4 | `magic` | ASCII `"KSXP"` |
| 0x44 | 4 | `version` | `512` (0x200) |
| 0x48 | 8 | `parentArrayOffset` | ptr (rel 0x40) to parent-index array. Exporter writes `104` = 0x68 (→ abs 0xA8). |
| 0x50 | 8 | `parentCapacity` | = bone count |
| 0x58 | 8 | `parentCount` | = bone count |
| 0x60 | 8 | pad | `0` |
| 0x68 | 8 | `nameTableOffset` | ptr (rel 0x40) to name-pointer array |
| 0x70 | 8 | `nameCapacity` | = bone count |
| 0x78 | 8 | `nameCount` | = bone count |
| 0x80 | 8 | pad | `0` |
| 0x88 | 8 | `matrixTableOffset` | ptr (rel 0x40) to per-bone transform array |
| 0x90 | 8 | `matrixCapacity` | = bone count |
| 0x98 | 8 | `matrixCount` | = bone count |
| 0xA0 | 8 | pad | `0` |

> Reader [S1] reads exactly these positions: `parentArrayOffset` u32 @ **0x48**,
> `bone_count` u32 @ **0x50**, `nameTableOffset` u32 @ **0x68**,
> `matrixTableOffset` u32 @ **0x88** — all `+ 0x40`. Fields are stored as u64 but
> the counts/offsets fit in the low u32, so reading u32 there is safe. This is
> now **[CONFIRMED]** (importer reads and exporter writes agree byte-for-byte).

### 2.2 Parent-index array  **[CONFIRMED S1, S2]**

At `parentArrayOffset`. `parentCount` entries, each **int16 (signed, LE)**:

- `bone[i].parentIndex = int16`
- `-1` (0xFFFF) means **root** (no parent). Exporter writes `65535` for roots.
- Array is padded to an 8-byte boundary afterward.

### 2.3 Name-pointer array  **[CONFIRMED S1, S2]**

At `nameTableOffset`. One entry per bone, **stride 0x10 bytes**. The first 8
bytes (u64) of each entry are a pointer (rel 0x40) to a zero-terminated ASCII
bone name in the string region; the other 8 bytes are 0.

```
name_ptr[i] = read_u64(nameTableOffset + i*0x10) + 0x40   # points to C-string
```

### 2.4 Per-bone transform array (bind pose, LOCAL)  **[CONFIRMED S1, S2]**

At `matrixTableOffset`, **stride 0x30 (48) bytes** per bone. Stored as
**TRS**, NOT a 4×4 matrix, and **local to the parent** (parent-relative):

| Rel off | Size | Field | Type |
|---------|------|-------|------|
| 0x00 | 12 | `translation` (x,y,z) | 3×float32 |
| 0x0C | 4 | pad / `0.0` | float32 (exporter writes 0) |
| 0x10 | 16 | `rotation` quaternion (x,y,z,w) | 4×float32, **XYZW order in file** |
| 0x20 | 12 | `scale` (x,y,z) | 3×float32 (exporter writes 1,1,1) |
| 0x2C | 4 | pad / `0.0` | float32 |

**Quaternion order:** stored **X,Y,Z,W**. Importer reorders to W,X,Y,Z for
math libs: `q = (w, x, y, z)` from file floats `(f0=x, f1=y, f2=z, f3=w)`
→ `(f3, f0, f1, f2)`. **[CONFIRMED S1 lines reading `temp_rot[3],[0],[1],[2]`]**

**Coordinate system:** HE2 is **X-forward / Z-up-ish** in the game's native
axes; the Blender add-on optionally remaps to "YX" for ergonomics. **For a
faithful parser, DO NOT apply the YX swap** — read raw `(x,y,z)` translation and
`(x,y,z,w)` quaternion exactly as stored. Apply your own up-axis conversion at
FBX-export time (HE2 is Y-up in-engine per the add-on docs: "Y-up, lying on its
back… then rotated +90° about X" describes the *Blender* re-orientation, i.e.
raw data is authored Y-up). Treat the exact global up-axis as **[INFERRED]** and
confirm visually. The raw TRS-per-bone parsing itself is **[CONFIRMED]**.

### 2.5 Building local bind matrices

```
local_bind[i] = compose_TRS(translation_i, quat_xyzw_i, scale_i)   # 4x4
# parentIndex_i == -1  -> root; else child of bone[parentIndex_i]
# Transforms are already parent-local, so:
global_bind[i] = (parent==-1) ? local_bind[i]
                              : global_bind[parentIndex_i] @ local_bind[i]
```

This yields exactly what you need: `(bone_name, parent_index, local_bind_matrix)[]`.
**[CONFIRMED — this is precisely what S1 does before `armature_apply`]**

---

## 3. Skin binding: how `.model` bone indices map to the skeleton

From the mesh importer [S7]. The `.skl.pxd` and the `.model` are **separate
files sharing a base name** (`chr_sonic.model` + `chr_sonic.skl.pxd`).

- **`.model` files are BIG-ENDIAN** for their SampleChunk container (note the
  `byteorder='big'` throughout [S7]) — a different container from the
  little-endian BINA `.pxd`. Don't confuse the two.  **[CONFIRMED S7]**
- The model has its own **global bone-name table** (`BoneRef` in [S7]) — an
  ordered list of bone names, one string each.
- Each **mesh** has a local **`BoneRefTable`**: an array of indices (u8 if
  `boneCount ≤ 255`, else u16) mapping *mesh-local skin indices* → *model global
  bone index*.
- Each **vertex** stores up to 4 (or 8) bone indices + 4 (or 8) weights (bytes,
  /255). Vertex bone index → `BoneRefTable[idx]` → `BoneRef[...]` = **bone
  name**.
- **Binding to the skeleton is BY NAME.** The importer creates a vertex group
  named `BoneRef[BoneRefTable[idx]]` and relies on the armature (parsed from
  `.skl.pxd`) having a bone of that exact name.  **[CONFIRMED S7 lines 260-269]**

> **Implication for you:** you do not need a numeric index correspondence
> between model and skeleton. Match **by bone name**. The `.model`'s embedded
> `Node` array ([S9]: `parentIndex:u32, name:string, matrix:Float4x4`) can also
> supply a skeleton if the separate `.skl.pxd` is absent, but for Frontiers
> characters the `.skl.pxd` is the authority for the armature and animation
> track order.

---

## 4. Animation — `.anm.pxd`

Payload magic `NAXP` (= "PXAN" reversed). Two encodings exist:

- **Uncompressed** (M&S Tokyo 2020 era, and any file with `compressed` flag ≠ 8)
  — explicit sparse keyframe tables. Fully parseable with no external lib.
- **ACL-compressed** (Frontiers, Origins, Shadow Gen — the normal case) — one
  `compressed_tracks` blob per section, decoded with nfrechette's `acl`.

### 4.1 PXAN payload header (starts at 0x40)  **[CONFIRMED S3, S4]**

| File off | Size | Field | Meaning |
|----------|------|-------|---------|
| 0x40 | 4 | `magic` | ASCII `"NAXP"` |
| 0x44 | 4 | `version` | `512` (0x200) |
| 0x48 | 1 | `flag_additive` | `1` = additive animation, else absolute |
| 0x49 | 1 | `flag_compressed` | `8` = ACL-compressed; `0` = uncompressed |
| 0x4A | 2 | pad | 0 |
| 0x4C | 4 | pad | 0 |
| 0x50 | 4 | (const) | exporter writes `0x18` |
| 0x54 | 4 | pad | 0 |
| 0x58 | 4 | `duration` | float32, seconds |
| 0x5C | 4 | `frameCount` | u32 (number of samples per track) |
| 0x60 | 8 | `trackCount` | u64 — number of bone tracks (should equal skeleton bone count) |
| 0x68 | 8 | `mainOffset` | ptr (rel 0x40) to main track section. Exporter writes `0x40` → payload at 0x80. |
| 0x70 | 8 | `rootOffset` | ptr (rel 0x40) to **root-motion** section, or 0 if none |
| 0x78 | 8 | pad | 0 |

Derived: `frameRate = (frameCount - 1) / duration` (if `duration ≠ 0`, else
default 30). **[CONFIRMED S3 lines 178-198]**

Notes **[CONFIRMED S3]**:
- `mainOffset` is 0 → no main section (rare/invalid).
- `rootOffset` beyond EOF or 0 → treat as "no root motion" (old buggy exports
  left dangling offsets).
- The **root-motion** section is a single-track animation applied to the
  *armature object itself* (whole-character world transform), not a bone.

### 4.2 COMPRESSED main/root sections (Frontiers default)  **[CONFIRMED S3, S5, S6]**

At `mainOffset` (and `rootOffset`) sits a raw **ACL `compressed_tracks` blob**.
Its own self-describing header (documented in [S5]):

```
# ACL compressed_tracks header (little-endian)
0x00  u32   buffer_size        # total size of THIS blob (read this to know how many bytes to slice)
0x04  i32   hash
0x08  u32   tag                # 0xAC11AC11  (acl::compressed_tracks)
0x0C  u16   version            # 7  == acl v02_00_00
0x0E  u8    padding            # 0
0x0F  u8    track_type         # 12 == qvvf (quat + vec3 translation + vec3 scale, float)
0x10  u32   track_count        # == bone count (main) / 1 (root)
0x14  u32   frame_count
0x18  f32   frame_rate
0x1C ...    ACL bit-packed track data (buffer_size - 0x1C bytes)
```

**How the add-on reads a section [S3 lines 400-421]:**
1. Seek `mainOffset`; read u32 → `buffer_size`.
2. Seek back to `mainOffset`; read `buffer_size` bytes = the compressed blob.
3. Pass blob to `decompress()` (the DLL / `acl` decoder).

**What `decompress()` returns [S5, S6]** — a flat little-endian buffer:

```
# Decompressed buffer
0x00  f32  duration
0x04  f32  frame_rate
0x08  u32  frame_count
0x0C  u32  track_count       # bone count (main) or 1 (root)

# then frame_count * track_count records, each 0x30 (48) bytes, a QVVf transform:
for frame in range(frame_count):
    for track in range(track_count):
        0x00  4×f32  rotation quaternion   X, Y, Z, W     # (0x10 bytes)
        0x10  3×f32  translation           X, Y, Z        # (0x0C bytes)
        0x1C  1×f32  loc_w                  = bone length after scale (unused for playback)
        0x20  3×f32  scale                 X, Y, Z        # (0x0C bytes)
        0x2C  1×f32  scale_w               = 1.0 (unused)
```

Record indexing (row-major, frame-outer) **[CONFIRMED S3 line 432 / S6 line 153]**:
```
record_offset = 0x10 + 0x30 * (track_count * frame + track)
```

**Semantics of each track record [CONFIRMED S3 lines 439-471]:**
- These are **LOCAL, parent-relative** transforms (same space as the skeleton
  bind pose in §2.4). The add-on multiplies parent chains
  (`get_matrix_map_global`) to get world space.
- **Rotation quaternion is X,Y,Z,W in the buffer.** The add-on reads
  `r0,r1,r2,r3 = XYZW` and builds `Quaternion(w=r3, x=r0, y=r1, z=r2)` in the
  non-YX (raw) path. **Use `(r3,r0,r1,r2)` → (w,x,y,z).**
- **Translation is X,Y,Z.**
- **Scale:** if `(s0,s1,s2) == (0,0,0)`, treat as `(1,1,1)` (sentinel for
  "no scale"). Otherwise use as-is.
- `loc_w` (bone length) and `scale_w` (1.0) are engine bookkeeping — **ignore
  for animation playback**.
- **Every bone has a value every frame** after ACL decode (ACL resamples to a
  dense per-frame stream; there is no sparsity in the decompressed buffer). So
  the compressed path gives you full dense TRS tracks. **[CONFIRMED S3]**

**Root-motion section** decodes identically, `track_count == 1`, one QVVf per
frame; apply to the whole model/armature object. The add-on applies an extra
axis fix-up for root (`(p0, -p2, p1)` etc.) that is a Blender-space convention —
in a raw parser keep root translation `(x,y,z)` as stored and reconcile axes at
export. **[CONFIRMED S3 lines 476-498; axis remap is Blender-specific]**

### 4.3 UNCOMPRESSED sections (legacy / `flag_compressed != 8`)  **[CONFIRMED S3, S8]**

Sparse keyframes. At the section offset is a **per-track descriptor table**,
**stride 0x48 (72) bytes** per track:

| Rel off | Size | Field |
|---------|------|-------|
| 0x00 | 8 | `loc_count` |
| 0x08 | 8 | `loc_frame_offset` (ptr rel 0x40 → array of u16 frame indices) |
| 0x10 | 8 | `loc_data_offset`  (ptr rel 0x40 → array of records) |
| 0x18 | 8 | `rot_count` |
| 0x20 | 8 | `rot_frame_offset` |
| 0x28 | 8 | `rot_data_offset` |
| 0x30 | 8 | `scale_count` |
| 0x38 | 8 | `scale_frame_offset` |
| 0x40 | 8 | `scale_data_offset` |

For each channel, `count` keyframes:
- **frame index array:** u16 per entry, at `*_frame_offset + 2*i`.
- **value array:** stride **0x10 (16) bytes** per entry, at `*_data_offset + 0x10*i`.
  - Translation: 3×f32 (x,y,z) then 4 pad bytes.
  - Rotation: 4×f32 (x,y,z,w).  → reorder to (w,x,y,z) = `(f3,f0,f1,f2)`.
  - Scale: 3×f32 (x,y,z) then 4 pad bytes.

Keyframes are **sparse** (a channel only stores frames where it changes);
missing channels/frames hold the previous value. Values are LOCAL
parent-relative, same as compressed. **[CONFIRMED S8, S3 `get_uncompressed_frame_table`]**

> Note the table stride/order here (per-track 0x48 descriptor, then
> loc/rot/scale) is the **same idea** the Turk645 legacy reader [S8] used at
> header offset 0x58; treat uncompressed as a fallback — Frontiers character
> anims are compressed.

### 4.4 Additive vs absolute  **[INFERRED S3]**

`flag_additive == 1` means the track values are **deltas to be composed on top
of** a base pose (e.g. blended over a base animation), not absolute local
transforms. The add-on records the flag (`pxd_additive`) but imports values the
same way; how the engine composites additives is **[UNKNOWN]** from these
sources. For a viewer, treat non-additive files as absolute local TRS (the
common case for full-body clips). Flag additive files and, initially, either
skip them or display raw (may look wrong until compositing is known).

---

## 5. Relationship: bind pose vs animation

- Animation track values and skeleton bind values live in the **same space**:
  **local, parent-relative TRS**, quaternion **XYZW**. **[CONFIRMED — identical
  read code in §2.4 and §4.2]**
- For a non-additive clip, a bone's animated local transform **replaces** its
  bind local transform at each frame (it is absolute-local, not a delta).
  World transform = accumulate up the parent chain, exactly as with the bind
  pose. **[CONFIRMED S3 `get_matrix_map_global`]**
- Track **order** in the animation = **skeleton bone order** (`trackCount`
  should equal skeleton bone count; the add-on warns if they differ and maps
  track `i` → `pose.bones[i]`). **[CONFIRMED S3 lines 323-326, 439-441]**
  So: **animation track index i corresponds to skeleton bone index i.** No name
  table inside the anim file — you MUST pair an anim with its skeleton.
- Additive clips = deltas (see §4.4, compositing **[UNKNOWN]**).

---

## 6. Parser pseudocode

### 6.1 Skeleton → (name, parent, local_bind_matrix)[]

```python
def parse_skeleton(buf):                    # buf = whole .skl.pxd bytes
    assert buf[0x00:0x08] == b"BINA210L"
    assert buf[0x10:0x14] == b"DATA"
    P = 0x40                                 # payload base; all ptrs are +P
    assert buf[P:P+4] == b"KSXP"
    assert u32(buf, P+4) == 512

    parent_off = u32(buf, P+0x08) + P        # ptr field @0x48
    bone_count = u32(buf, P+0x10)            # count      @0x50
    name_off   = u32(buf, P+0x28) + P        # name-ptr array   @0x68
    matrix_off = u32(buf, P+0x48) + P        # transform array  @0x88

    bones = []
    for i in range(bone_count):
        parent = i16(buf, parent_off + 2*i)          # -1 == root
        npref  = u64(buf, name_off  + 0x10*i) + P
        name   = cstr(buf, npref)
        b      = matrix_off + 0x30*i
        tx,ty,tz = f32x3(buf, b + 0x00)
        qx,qy,qz,qw = f32x4(buf, b + 0x10)           # XYZW in file
        sx,sy,sz = f32x3(buf, b + 0x20)
        local = compose_TRS((tx,ty,tz), quat_wxyz(qw,qx,qy,qz), (sx,sy,sz))
        bones.append(Bone(name=name, parent=parent, local_bind=local))
    return bones
```

### 6.2 Animation → per-bone dense TRS tracks (compressed path)

```python
def parse_animation(buf, acl_decode):        # acl_decode = link to nfrechette/acl
    assert buf[0x10:0x14] == b"DATA"
    P = 0x40
    assert buf[P:P+4] == b"NAXP"
    additive   = buf[P+0x08] == 1
    compressed = buf[P+0x09] == 8
    duration    = f32(buf, P+0x18)
    frame_count = u32(buf, P+0x1C)
    track_count = u64(buf, P+0x20)
    main_off    = u64(buf, P+0x28); main_off = (main_off + P) if main_off else None
    root_off    = u64(buf, P+0x30); root_off = (root_off + P) if root_off else None
    if root_off and root_off > len(buf) - P: root_off = None

    if compressed:
        # ---- main section ----
        blob_size = u32(buf, main_off)
        blob      = buf[main_off : main_off + blob_size]     # ACL compressed_tracks
        dec       = acl_decode(blob)                          # -> flat buffer §4.2
        # dec: 0x00 f32 dur, 0x04 f32 fps, 0x08 u32 frames, 0x0C u32 tracks
        tracks = [[None]*frame_count for _ in range(track_count)]
        for frame in range(frame_count):
            for t in range(track_count):
                o = 0x10 + 0x30 * (track_count*frame + t)
                qx,qy,qz,qw = f32x4(dec, o + 0x00)            # XYZW
                px,py,pz    = f32x3(dec, o + 0x10)
                # dec[o+0x1C] = bone length (ignore)
                sx,sy,sz    = f32x3(dec, o + 0x20)
                if (sx,sy,sz) == (0,0,0): sx=sy=sz=1.0
                tracks[t][frame] = TRS((px,py,pz),
                                       quat_wxyz(qw,qx,qy,qz),
                                       (sx,sy,sz))
        root = decode_single_track(buf, root_off, frame_count, acl_decode) if root_off else None
        return Anim(frame_count, duration, additive, tracks, root)
    else:
        return parse_uncompressed(buf, main_off, root_off, frame_count, track_count)  # §4.3
```

- **`compose_TRS`**: `T @ R @ S` (translation, then rotation, then scale) — the
  add-on builds `Matrix.LocRotScale(loc, rot, scale)`, i.e. standard TRS.
- **`quat_wxyz`**: your math lib's quaternion ctor from (w,x,y,z).
- **Track i ↔ skeleton bone i** (§5). Missing tracks (if `track_count <
  bone_count`) → identity/keep bind.

---

## 7. Feasibility & open items

### Can skeletons be parsed reliably? **YES — high confidence.**
The full read + write path is in source [S1][S2] and round-trips real game
files. Output = exactly `(name, parent, local_bind_TRS)[]`. Header field
positions (§2.1) are byte-verified: importer reads and exporter writes agree,
so there are no open items on the skeleton binary layout itself. The only thing
left to confirm empirically is the global up-axis convention (see below), which
affects display orientation, not parsing.

### Can animations be parsed reliably? **YES for the container; the ACL blob needs a library.**
- The PXD wrapper (header, section offsets, root/main split, flags,
  frame/track counts) is **fully understood** [S3][S4][S5].
- The compressed track data is **ACL** (`nfrechette/acl`, tag `0xAC11AC11`,
  version 7 / v02.00.00, track type `qvvf`). You do **not** reverse it — you
  **link `acl`** (C++) and call its decoder, exactly as FrontiersAnimDecompress
  does [S6]. After decoding you get a trivial dense
  `frame × track × QVVf(0x30)` buffer (§4.2). This is the recommended path for
  a C++ viewer.
- For a **pure-Python** parser with no C++ dependency, you'd need a Python ACL
  decoder (none known to be mature) OR ship/call the FrontiersAnimDecompress
  DLL via ctypes (Windows-only), as the add-on does [S5]. Reimplementing ACL
  bit-unpacking in Python is possible but large and **not recommended**.
- **Uncompressed** anims (§4.3) parse with zero external deps, but Frontiers
  character clips are compressed, so this only covers legacy/edge files.

### Known unknowns
- **[UNKNOWN]** Exact additive-compositing math (how `flag_additive` blends).
- **[UNKNOWN]** Global up-axis / handedness convention of raw HE2 data (Y-up
  strongly inferred). Confirm visually; the YX/axis swaps in the add-on are
  Blender ergonomics, not the file format — keep raw values and convert once at
  FBX export.
- **[UNKNOWN]** `.uv-anim` / `.mat-anim` / `.cam-anim` — these are *different*
  formats (material/UV/camera keyframes), not skeletal, and are **not** covered
  by the PXD skeletal path above. Out of scope for armature+skeletal-anim FBX;
  research separately if needed (HedgeDocs "material animation").
### Recommended implementation path (C++ viewer → FBX)
1. Parse `.skl.pxd` → bones (name/parent/local TRS). Build armature. **[solid]**
2. Parse `.model` (big-endian SampleChunk, [S7]) → meshes + per-vertex bone
   **names** (via BoneRef/BoneRefTable). Bind to armature **by name**. **[solid]**
3. Vendor `nfrechette/acl` + `rtm`. Parse `.anm.pxd` container → for each
   section slice the ACL blob and decode to the dense QVVf buffer. Map track i →
   bone i → keyframes. **[solid, needs the lib]**
4. Export FBX with armature + one action/take per `.anm.pxd`, applying a single
   global axis conversion. Keep quaternions **XYZW→(w,x,y,z)** consistent.

---

## Appendix A — constants quick reference

| Thing | Value |
|-------|-------|
| BINA magic | `BINA210L` |
| Block magic | `DATA` |
| Payload base offset | `0x40` (all internal ptrs are relative to this) |
| Skeleton payload magic | `KSXP` (PXSK) |
| Animation payload magic | `NAXP` (PXAN) |
| PXD version | `512` (0x200) |
| Skeleton bone transform stride | `0x30` (TRS + pads) |
| Skeleton name-ptr entry stride | `0x10` |
| Parent index type | int16, `-1`/`0xFFFF` = root |
| Anim `flag_compressed` | `8` = ACL, `0` = uncompressed |
| Anim `flag_additive` | `1` = additive |
| ACL tag | `0xAC11AC11` |
| ACL version | `7` (v02.00.00) |
| ACL track type | `12` (qvvf) |
| Decompressed record stride | `0x30` (quat XYZW + vec3 T + f + vec3 S + f) |
| Decompressed record index | `0x10 + 0x30*(track_count*frame + track)` |
| Quaternion storage order | X, Y, Z, W (reorder to W,X,Y,Z for math) |
| Uncompressed per-track descriptor stride | `0x48` |
| Uncompressed keyframe value stride | `0x10` |

## Appendix B — local repo paths (cloned for this research)

```
scratchpad/research_repos/
  AdelQue-FrontiersAnimDecompress/     <- primary (PXDAnimationTools v2.1.5)
    Blender/FrontiersAnimationTools/skeleton/skeleton_import.py     [S1]
    Blender/FrontiersAnimationTools/skeleton/skeleton_export.py     [S2]
    Blender/FrontiersAnimationTools/animation/anim_import.py        [S3]
    Blender/FrontiersAnimationTools/animation/anim_export.py        [S4]
    Blender/FrontiersAnimationTools/FrontiersAnimDecompress/process_buffer.py  [S5]
    FrontiersAnimDecompress/FrontiersAnimDecompress.cpp             [S6]
  WistfulHopes-FrontiersAnimDecompress/   (original DLL source, matches [S5][S6])
  PXDAnimationTools/                       (same add-on, WistfulHopes fork)
  Turk645-HE2-Importer/
    io_import_hedgehog_engine.py           [S7]  (.model + skin binding + skeleton)
    io_import_hedgehog_engine_anim.py      [S8]  (legacy uncompressed anim)
  ModelConverter/
    Source/ModelConverter/Node.h           [S9]  (model-embedded node/bone struct)
```
