# Hedgehog Engine 2 — Model / Material / Texture Format Spec (Sonic Frontiers, PC)

Target: a C++/Python parser that reads `.model`, `.terrain-model`, `.material`, `.dds`
(extracted from Frontiers `.pac` archives), decodes meshes with UVs / normals / colors /
skin weights, resolves materials → textures, and exports to FBX.

**Primary sources (cited inline as `[file:line]`):**

- `Turk645/Hedgehog-Engine-2-Mesh-Blender-Importer` — `io_import_hedgehog_engine.py`
  (the canonical community Frontiers `.model` importer; big-endian reads, offsets `+0x10`).
- `Radfordhound/HedgeLib` (HedgeLib++, C++17, the modern Frontiers-capable branch). This is
  the authoritative reference. Key files:
  - `HedgeLib/include/hedgelib/models/hl_hh_model.h` — all raw structs + enums.
  - `HedgeLib/src/models/hl_hh_model.cpp` — parse logic, vertex decode, strip→list.
  - `HedgeLib/include/hedgelib/materials/hl_hh_material.h` + `src/.../hl_hh_material.cpp`.
  - `HedgeLib/include/hedgelib/textures/hl_hh_texture.h`.
  - `HedgeLib/include/hedgelib/io/hl_hh_mirage.h` + `src/.../hl_hh_mirage.cpp` — Mirage /
    SampleChunk / standard header, offset-table fixing.
  - `HedgeLib/include/hedgelib/hh/hl_hh_needle_texture_streaming.h` — streamed DDS (NTSI/PSTN).
  - `HedgeLib/include/hedgelib/hl_internal.h` — `off32`/`arr32` offset semantics.

Verified against real file: `E:\...\SonicFrontiers\image\x64\raw\character\amy.pac`
begins with magic `PACx403L` (`50 41 43 78 34 30 33 4C`) → Frontiers PAC v403, little-endian.

> **Endianness note.** Frontiers PC files are **little-endian**. The Turk645 importer reads
> *big-endian* because it was written/tested against console (Wii U / 360-lineage) data or an
> LE→BE quirk; **for Frontiers PC use little-endian everywhere.** The raw D3D format enum
> *values* (e.g. `0x2A23B9`) are identical regardless — they are just numeric constants.
> HedgeLib stores everything BE-on-disk historically but its `fix()` byte-swaps to host on
> PC; treat Frontiers PC on-disk data as LE. When in doubt, sanity-check: positions should be
> reasonable floats (roughly -100..100), `vertexSize` small (e.g. 0x20–0x60), counts sane.

---

## 0. File wrapping overview (outermost → innermost)

```
amy.pac  (PACx403L archive, LE)                     ← unpack first (separate concern)
  └─ amy.model            (one entry)               ← Mirage-wrapped skeletal model
  └─ SomeMaterial.material                          ← Mirage-wrapped material v3
  └─ SomeTexture.dds                                ← standard DDS (BCn), or streamed (NTSI)
  └─ amy.skl.pxd / *.anm.pxd                        ← skeleton/anim (other agent)
```

Each individual `.model` / `.material` file is wrapped in one of **three** header types
[`hl_hh_mirage.h:18-34`]:

| header_type          | Used by                                             | Magic / detection |
|----------------------|-----------------------------------------------------|-------------------|
| `standard` (0)       | Old HH1 games; also simple models w/ no params      | 0x18-byte header  |
| `sample_chunk_v1` (1)| Lost World … Forces                                 | root node flag    |
| `sample_chunk_v2` (2)| M&S Tokyo 2020 **and Frontiers**                    | root node flag    |

**Detection** [`hl_hh_mirage.h:465-472`]: read the first `u32`.
- On-disk (unfixed, LE): if `firstU32 & 0x80 != 0` → **sample-chunk** header; else standard.
- After endian-fix: test `& 0x80000000` (the `is_root` node flag).

Frontiers `.model`/`.material` use the **sample-chunk** header. `.dds` are raw DDS (no Mirage).

---

## 1. Standard Mirage header (`header_type::standard`)

`struct raw_header` — 0x18 bytes [`hl_hh_mirage.h:92-133`]:

| off | type       | field     | meaning |
|-----|------------|-----------|---------|
| 0x00| u32        | fileSize  | total file size |
| 0x04| u32        | version   | **format version** (model=2/5/6, material=1/3, terrain=5) |
| 0x08| u32        | dataSize  | size of data block |
| 0x0C| off32      | data      | offset to data block (usually 0x18) |
| 0x10| off32      | offTable  | offset to offset-table |
| 0x14| off32      | fileName  | offset to file name (or 0) |

- Offsets are relative **to the start of the header (`this`)** for the 3 header offsets
  [`hl_hh_mirage.cpp:31-33` `data.fix(this)`].
- The **offset table** is at `offTable`: first a `u32 offCount`, then `offCount` × `u32`
  relative-offset-positions [`hl_hh_mirage.cpp:13-16`].
- Each entry in the offset table is a byte position **relative to `data`** where a 32-bit
  pointer lives; fixing = add `data` base to the stored value [`hl_hh_mirage.cpp:366-387`].
- `get_data()` returns `data` and sets `version` from the header [`hl_hh_mirage.h:141-150`].

> This is the layout Turk645 assumes: he seeks `0x8` (=`data` offset for his files sitting at
> a 0x10-based sub-header), reads a pointer, and adds `+0x10` to every offset — i.e. his data
> base is at file offset **0x10**. HedgeLib generalizes this via `fix()`.

---

## 2. Sample-Chunk header (`sample_chunk_v1/v2`) — the Frontiers wrapper

### 2.1 `struct sample_chunk::raw_header` — 16 bytes [`hl_hh_mirage.h:323-392`]

| off | type  | field    | meaning |
|-----|-------|----------|---------|
| 0x00| u32   | fileSize | **also carries flags**: `& 0x80000000` (is_root) set → has nodes |
| 0x04| u32   | magic    | `0x0133054A` (`raw_header_magic`), not actually checked by the game |
| 0x08| off32 | offTable | offset to offset table (LE relative to header start) |
| 0x0C| u32   | offCount | number of offset entries |

The header is itself treated as a node; the child nodes start immediately after
(`this + 1`, i.e. at 0x10) when `has_nodes()` [`hl_hh_mirage.h:340-353`].

### 2.2 `struct sample_chunk::raw_node` — 16 bytes [`hl_hh_mirage.h:180-321`]

| off | type    | field | meaning |
|-----|---------|-------|---------|
| 0x00| u32     | flags | high 3 bits = node flags; low 29 bits = **node size in bytes** |
| 0x04| u32     | value | node value — for the `Contexts` node this is the **data version**; for property nodes it's the param value |
| 0x08| char[8] | name  | 8-char, space-padded, node name (e.g. `"Model   "`, `"Contexts"`) |

**Node flags** (mask `0xE0000000`) [`hl_hh_mirage.h:164-176`]:
- `0x20000000` `is_leaf` — no children (data follows directly).
- `0x40000000` `is_last_child` — last sibling.
- `0x80000000` `is_root`.
- size = `flags & 0x1FFFFFFF`.

Traversal [`hl_hh_mirage.h:223-243`, `hl_hh_mirage.cpp:121-165`]:
- `children()` = `this + 1` (16 bytes later) if not a leaf.
- `next()` = `ptradd(this, size())` unless `is_last_or_root`.
- `data<T>()` = `this + 1` (bytes immediately after the node struct).
- `get_child(name, recursive)` / `get_node(name)`: name-match on 8 chars (`strncmp(...,8)`).
  Names shorter than 8 are space-padded via `make_node_name`.

### 2.3 Node tree for a Frontiers `.model`

```
(header)
 └─ "Model"                         (top model node)
     ├─ "NodesExt"  (optional)      per-bone extra params
     │    └─ "NodePrms" value=nodeIndex
     │         └─ "SCAParam"
     │              └─ <property nodes...>
     ├─ <model property nodes...>   e.g. "Topology" value=3or4  ← IMPORTANT
     └─ "Contexts"  value=VERSION   ← the actual model data starts at Contexts.data()
```

- `get_data()` for sample-chunk [`hl_hh_mirage.h:433-446`]: find node **`"Contexts"`**,
  set `version = Contexts.value`, and return `Contexts.data<T>()` (= the raw model/material
  struct). **Offsets inside the data block are relative to that data base** (the address
  returned by `get_data`, i.e. `Contexts + 16`), because `offsets_fix` uses that base
  [`hl_hh_model.cpp` via `mirage::fix`, base = `nodesPtr` for sample chunk, `data` for standard;
  offset entries are positions relative to that base — `hl_hh_mirage.cpp:255,366-387`].

- **Topology** is a per-model sample-chunk property named `"Topology"`; its `value` is the
  `raw_topology_type` (see §4). If absent → default **triangle strips**
  [`hl_hh_model.cpp:2258-2266`].

### 2.4 Sample-chunk `property` (parsed params) [`hl_hh_mirage.h:394-425`]
A property is just a `raw_node`: `{ name[8], value:u32 }`. Model params (like `Topology`)
and per-node SCA params are read as `property{name, value}`.

---

## 3. `off32` / `arr32` offset semantics [`hl_internal.h`]

- `off32<T>` = a single `u32 m_val`. `.get()` returns `base + m_val`; `m_val == 0` → null.
  `.fix(base)` = `set(base + m_val)` [`hl_internal.h:534-537`].
- `arr32<T>` = `{ u32 count; off32<T> dataPtr; }` (8 bytes) [`hl_internal.h:870-884`].
- All offsets in model/material data are **relative to the data base** returned by
  `get_data()`. When writing your own parser, compute `dataBase` once (Contexts data pointer
  for sample-chunk, or `header.data` for standard) and add every stored offset to it.

Pseudocode helpers:
```
read_off32(f, base): v=read_u32_le(f); return None if v==0 else base+v
read_arr32(f, base): n=read_u32_le(f); p=read_off32(f, base); return (n, p)
```

---

## 4. Topology & index buffers

`enum raw_topology_type : u32` [`hl_hh_model.h:23-27`] (= D3D_PRIMITIVE_TOPOLOGY − 1):
- `3` = **triangle_list**
- `4` = **triangle_strip**  ← Frontiers character models are almost always strips

Index buffer = `arr32<u16> faces` in each mesh. Values are u16.
- **Triangle strip decode** [`hl_hh_model.cpp:1137-1188`] with `0xFFFF` as primitive-restart:
  - Walk faces; maintain `f1,f2,f3` sliding window, `reverse` toggles winding each step.
  - On `0xFFFF`: restart — take next two indices as new `f1,f2`, reset `reverse=false`.
  - Emit a triangle only if `f1,f2,f3` are all distinct (degenerate strips are skipped).
  - Winding: even step → `(f1,f2,f3)`; odd step (`reverse`) → `(f1,f3,f2)`.
  - Turk645's equivalent [`io_import_hedgehog_engine.py:126-146,367-376`] splits on `65535`
    into sub-strips then `strip2face`. Note Turk reverses to `(f[2],f[1],f[0])` for lists.

> Whether a file uses strips or lists is determined by the `Topology` sample-chunk property
> (§2.3), NOT stored per-mesh. Default when absent = strips.

---

## 5. Vertex format — the decode heart

### 5.1 `struct raw_vertex_element` — 12 bytes (D3DVERTEXELEMENT9) [`hl_hh_model.h:140-163`]

| off | type | field  | meaning |
|-----|------|--------|---------|
| 0x00| u16  | stream | vertex stream index (usually 0) |
| 0x02| u16  | offset | **byte offset of this element within each vertex** |
| 0x04| u32  | format | `raw_vertex_format` (D3DDECLTYPE code, see §5.2) |
| 0x08| u8   | method | `raw_vertex_method` (D3DDECLMETHOD, usually 0=normal) |
| 0x09| u8   | type   | `raw_vertex_type` (D3DDECLUSAGE — POSITION/NORMAL/…, see §5.3) |
| 0x0A| u8   | index  | usage index (e.g. UV channel 0..3, color set) |
| 0x0B| u8   | padding| — |

The vertex-declaration is an **array of these terminated by a sentinel** whose
`format == 0xFFFFFFFF` (`last_entry`) [`hl_hh_model.cpp:1632-1640`; Turk breaks on `-1`].
To parse: read elements sequentially until `format == 0xFFFFFFFF`.

Per vertex `i`, element data is at `vertices_base + i*vertexSize + element.offset`.
`vertexSize` (a.k.a. stride) and `vertexCount` come from the mesh struct (§6).

### 5.2 `raw_vertex_format` — FULL enum (numeric value → meaning) [`hl_hh_model.h:33-83`]

These are Xbox-360 D3DDECLTYPE codes (thanks Skyth). **Byte size** and decode from
`convert_to_vec4` [`hl_hh_model.cpp:20-398`]:

| value (hex) | name          | bytes | decode |
|-------------|---------------|-------|--------|
| 0x2C83A4 | float1        | 4  | f32 x |
| 0x2C23A5 | float2        | 8  | f32 x,y |
| 0x2A23B9 | **float3**    | 12 | f32 x,y,z  (common POSITION/NORMAL) |
| 0x1A23A6 | float4        | 16 | f32 x,y,z,w |
| 0x2C83A1 | int1          | 4  | s32 |
| 0x2C23A2 | int2          | 8  | s32×2 |
| 0x1A23A3 | int4          | 16 | s32×4 |
| 0x2C82A1 | uint1         | 4  | u32 |
| 0x2C22A2 | uint2         | 8  | u32×2 |
| 0x1A22A3 | uint4         | 16 | u32×4 |
| 0x2C81A1 | int1_norm     | 4  | snorm(s32) |
| 0x2C21A2 | int2_norm     | 8  | snorm×2 |
| 0x1A21A3 | int4_norm     | 16 | snorm×4 |
| 0x2C80A1 | uint1_norm    | 4  | unorm(u32) |
| 0x2C20A2 | uint2_norm    | 8  | unorm×2 |
| 0x1A20A3 | uint4_norm    | 16 | unorm×4 |
| 0x182886 | d3d_color     | 4  | BGRA u8→unorm: `(v>>24,>>16,>>8,&0xFF)` i.e. (A,R,G,B) order in the u32; result vec = (a,r,g,b)/255 |
| 0x1A2286 | ubyte4        | 4  | u8×4 (raw, not normalized) |
| 0x1A2386 | byte4         | 4  | s8×4 (raw) |
| 0x1A2086 | ubyte4_norm   | 4  | u8×4 / 255 |
| 0x1A2186 | byte4_norm    | 4  | s8×4 snorm |
| 0x2C2359 | short2        | 4  | s16×2 (raw) |
| 0x1A235A | short4        | 8  | s16×4 (raw) |
| 0x2C2259 | ushort2       | 4  | u16×2 (raw) |
| 0x1A225A | **ushort4**   | 8  | u16×4 (raw) — used for **BLENDINDICES (16-bit)** |
| 0x2C2159 | short2_norm   | 4  | s16×2 snorm |
| 0x1A215A | short4_norm   | 8  | s16×4 snorm |
| 0x2C2059 | ushort2_norm  | 4  | u16×2 unorm |
| 0x1A205A | ushort4_norm  | 8  | u16×4 unorm |
| 0x2A2287 | udec3         | 4  | 10/10/10 packed uint (x=&0x3FF, y=>>10, z=>>20) |
| 0x2A2387 | dec3          | 4  | 10:10:10 signed (TODO in HL) |
| 0x2A2087 | udec3_norm    | 4  | 10/10/10 unorm |
| 0x2A2187 | **dec3_norm** | 4  | 10/10/10 **snorm** — common packed **NORMAL/TANGENT** |
| 0x1A2287 | udec4         | 4  | 10/10/10/2 uint |
| 0x1A2387 | dec4          | 4  | 10/10/10/2 signed |
| 0x1A2087 | udec4_norm    | 4  | 10/10/10/2 unorm |
| 0x1A2187 | dec4_norm     | 4  | 10/10/10/2 snorm |
| 0x2A2290 | uhend3        | 4  | 11/11/10 uint |
| 0x2A2390 | hend3         | 4  | 11/11/10 signed |
| 0x2A2090 | uhend3_norm   | 4  | 11/11/10 unorm |
| 0x2A2190 | hend3_norm    | 4  | 11/11/10 snorm |
| 0x2A2291 | udhen3        | 4  | 10/11/11 uint |
| 0x2A2391 | dhen3         | 4  | 10/11/11 signed |
| 0x2A2091 | udhen3_norm   | 4  | 10/11/11 unorm |
| 0x2A2191 | dhen3_norm    | 4  | 10/11/11 snorm |
| 0x2C235F | **float16_2** | 4  | half×2 — common **TEXCOORD (UV)** |
| 0x1A2360 | float16_4     | 8  | half×4 |
| 0xFFFFFFFF | last_entry   | —  | sentinel (end of vertex-element array) |

**snorm/unorm helpers:** `unorm_to_float(u8)=v/255`; `unorm_to_float<10>(v)=(v&0x3FF)/1023`;
`snorm_to_float<10>(v)=` sign-extend 10-bit then `/511`. Turk's `ten_bit_normal_read`
[`io_import_hedgehog_engine.py:353-382`] divides by 512 (an approximation; use `/511` for
strict D3D snorm, difference is negligible for rendering).

**10-bit packed normal (dec3_norm / 0x2A2187):** read u32, extract three signed 10-bit
fields (bits 0-9, 10-19, 20-29), sign-extend (`if x>=0x200: x-=0x400`), divide by 511
(Turk uses 512), then normalize.

### 5.3 `raw_vertex_type` (D3DDECLUSAGE — semantic) [`hl_hh_model.h:102-118`]

| value | semantic       | maps to |
|-------|----------------|---------|
| 0 | position      | vertex positions |
| 1 | blend_weight  | **skin weights** (per-vertex, usually 4 or 8) |
| 2 | blend_indices | **skin bone indices** (into mesh.boneNodeIndices) |
| 3 | normal        | normals |
| 4 | psize         | (point size, ignore) |
| 5 | texcoord      | UV; `.index` = UV channel 0..3 (V flip on import, see §5.5) |
| 6 | tangent       | tangents |
| 7 | binormal      | binormals/bitangents |
| 8 | tess_factor   | (ignore) |
| 9 | position_t    | (ignore) |
| 10| color         | vertex color; `.index` = color set |
| 11| fog / 12 depth / 13 sample | (ignore) |

`raw_vertex_method` [`hl_hh_model.h:88-97`]: 0 normal, 1 partial_u, 2 partial_v, 3 cross_uv,
4 uv, 5 lookup, 6 lookup_presampled. Frontiers meshes use `0` (normal); others rare.

### 5.4 Which element goes where — how HedgeLib routes them [`hl_hh_model.cpp:1210-1443`]

Loop every vertex element; switch on `type`:
- position → `mesh.vertices` (convert_to_vec4, take xyz)
- normal → `mesh.normals`; tangent → tangents; binormal → binormals
- texcoord → `mesh.uvs[index]` (take xy of vec4); only indices 0..3 supported
- color → `mesh.colors` (vec4 rgba)
- blend_weight → `mesh.boneWeights` (vec4)
- blend_indices → resolve bone refs (see §7)

### 5.5 Turk645's concrete field IDs (cross-check)
Turk645 keys vertex elements by a **packed `(type<<16)|index`-like** id he calls `VTypeIndex`
[`io_import_hedgehog_engine.py:149-166`]. His observed ids (big-endian read of the element's
type/index field composite): position `0x0`, weights `0x10000`/`0x10100`,
bone-indices `0x20000`/`0x20100`, normal `0x30000`, UV `0x50000`, color `0xA0000`. These line
up with `raw_vertex_type` × usage-index: `type=5 texcoord`→`0x5....`, `type=0xA color`. Prefer
parsing the real 12-byte `raw_vertex_element` struct (§5.1) over Turk's heuristic ids.

- UV: Turk reads `float16_2` and stores `(u, 1-v)` — **flip V** on import for standard UV
  convention [`io_import_hedgehog_engine.py:186-188`].

---

## 6. Mesh, mesh-slot, mesh-group structures

### 6.1 `raw_mesh` — the per-submesh struct — 0x2C bytes
Two revisions; **only difference is the bone-index element width**
[`hl_hh_model.h:165-217`]:

`raw_mesh_r1` (revision 1 — Forces-era, **8-bit** bone node indices):

| off | type                       | field |
|-----|----------------------------|-------|
| 0x00| off32<char>                | materialName |
| 0x04| arr32<u16>                 | faces (count @0x04, ptr @0x08) |
| 0x0C| u32                        | vertexCount |
| 0x10| u32                        | vertexSize (stride) |
| 0x14| off32<void>                | vertices (raw vertex buffer) |
| 0x18| off32<raw_vertex_element>  | vertexElements (declaration array) |
| 0x1C| arr32<u8>                  | boneNodeIndices (count @0x1C, ptr @0x20) |
| 0x24| arr32<off32<raw_texture_unit>> | textureUnits (count @0x24, ptr @0x28) |

`raw_mesh_r2` (revision 2 — **Frontiers**, **16-bit** bone node indices)
[`hl_hh_model.h:192-217`]: identical layout but `boneNodeIndices` is `arr32<u16>`.

> Turk645 mirrors this: `BoneRefSize = 2 if BoneCount>255 else 1`
> [`io_import_hedgehog_engine.py:85-88,118-121`]. For Frontiers characters expect **u16**.

Field read order in Turk's `parse_mesh` [`io_import_hedgehog_engine.py:100-110`]:
materialName, indiceCount, indiceOffset, vertCount, vertSize, vertChunkOffset,
vertDataTypeOffset, boneRefCount, boneRefOffset — same struct.

### 6.2 `raw_texture_unit` — 8 bytes [`hl_hh_model.h:120-135`]
`{ off32<char> name; u8 index; u8 pad×3 }`. This is a **per-mesh texture-unit / sampler
binding by name** (e.g. `"diffuse"`), distinct from the material's own texture list. Usually
you don't need it for basic export — the material (§8) provides the DDS names.

### 6.3 Mesh slots & group — how meshes are organized

`raw_mesh_slot<T>` = `arr32<off32<T>>` (a count + array of pointers-to-mesh)
[`hl_hh_model.h:219-226`].

`raw_mesh_group<T>` — 0x28 bytes [`hl_hh_model.h:411-443`], four slots by blend category:

| off  | field   | meaning |
|------|---------|---------|
| 0x00 | opaq    | opaque meshes (mesh_slot) |
| 0x08 | trans   | transparent/alpha-blend meshes |
| 0x10 | punch   | alpha-test ("punch-through") meshes |
| 0x18 | special | `raw_special_meshes` (typed extra slots, see below) — 16 bytes |

`raw_special_meshes<T>` — 16 bytes [`hl_hh_model.h:228-409`]:
`{ u32 count; off32<off32<char>> types; off32<off32<u32>> meshCounts; off32<off32<off32<T>>> meshes; }`
i.e. `count` parallel arrays: `types[i]` (a name like `"Watr"`), `meshCounts[i]`, and
`meshes[i]` (array of `meshCount[i]` mesh pointers). Rarely needed for characters.

The group **name** is the C-string immediately after the struct (`this + 1`)
[`hl_hh_model.h:428-436`] — only meaningful in terrain models.

### 6.4 Terrain model (`.terrain-model`, version 5)
`get_data().version == 5`. Two revisions distinguished by a heuristic (SEGA didn't bump the
version) — if any pointer points before the end of the v5r2 struct, it's r1
[`hl_hh_model.cpp:998-1021`].

`raw_terrain_model_v5r1` — 12 bytes [`hl_hh_model.h:460-475`]:
`{ arr32<off32<raw_mesh_group_r1>> meshGroups; off32<char> name; }`

`raw_terrain_model_v5r2` — 16 bytes [`hl_hh_model.h:490-513`]: adds
`raw_terrain_model_flags flags` (`0x1 = is_instanced`, meaning a `.terrain-instanceinfo`
places copies of it). Terrain meshes use `raw_mesh_group_r1` (**8-bit** bone indices — but
terrain is generally unskinned).

### 6.5 Skeletal model (`.model`) versions [`hl_hh_model.h:517-625`, dispatch `hl_hh_model.cpp:2695-2721`]

`get_data().version` selects the struct:

- **v2** `raw_skeletal_model_v2` (0x2C): `meshes` is a single `raw_mesh_slot_r1` (not grouped),
  then `unknown1/2`, `nodeCount`, `nodes`, `nodeMatrices`, `bounds`, `unknown3`.
- **v4** (0x30): mesh **groups** (r1) + 3 unknown arrays + nodes… (HL: TODO, uncommon).
- **v5** `raw_skeletal_model_v5` (0x20): mesh groups (r1, **8-bit** bones).
- **v6** `raw_skeletal_model_v6` (0x20): mesh groups (**r2, 16-bit** bones) — **Frontiers**.

`raw_skeletal_model_v5/v6` layout (0x20) [`hl_hh_model.h:575-625`]:

| off  | type                                   | field |
|------|----------------------------------------|-------|
| 0x00 | arr32<off32<raw_mesh_group_rX>>        | meshGroups |
| 0x08 | arr32<void>                            | unknown1 (Sajid: morphers) |
| 0x10 | u32                                    | nodeCount |
| 0x14 | off32<off32<raw_node>>                 | nodes (array of pointers to raw_node) |
| 0x18 | off32<matrix4x4>                       | nodeMatrices (one 4×4 float mtx per node) |
| 0x1C | off32<aabb>                            | bounds (bounding box) |

`raw_node` (model's internal node list — the bind skeleton reference) — 8 bytes
[`hl_hh_model.h:445-458`]: `{ s32 parentIndex; off32<char> name; }`. `nodeMatrices[i]` is that
node's matrix (bind / inverse-bind — 16 floats). **This node list is what bone indices
resolve into** (see §7). Note: the *animation* skeleton lives in `.skl.pxd` (other agent), but
the model's own `nodes[]` names must match the skeleton bone names.

> Turk645 reads bone names differently: he pulls them from a bone-name pointer table
> [`io_import_hedgehog_engine.py:342-351`] and the actual transforms from the `.skl.pxd`
> [`parse_skeleton`, lines 282-340]. For rendering you can use `nodeMatrices` directly, or the
> `.skl.pxd` for the animation-ready skeleton.

---

## 7. Skin weights & bone indices — the mapping

Two vertex elements carry skinning:
- `blend_indices` (type 2): per-vertex, typically 4 (or 8) indices. **These index into the
  mesh's own `boneNodeIndices` array**, NOT the global node list directly.
- `blend_weight` (type 1): matching weights (u8/255 or unorm), same count.

Resolution chain [`hl_hh_model.cpp:1330-1371`; Turk `io_import_hedgehog_engine.py:260-269`]:

```
localIdx        = vertex.blendIndices[k]        # 0..boneRefCount-1
globalNodeIdx   = mesh.boneNodeIndices[localIdx]
boneName        = model.nodes[globalNodeIdx].name
weight          = vertex.blendWeights[k] / 255  (if u8) or unorm
```

Details:
- `mesh.boneNodeIndices` (`arr32<u16>` in r2 / `arr32<u8>` in r1) is the mesh's **palette**:
  a small list of global node indices used by this submesh (D3D bone-palette skinning).
- Weight/index count: 4 by default; 8 if both `0x20000` and `0x20100` (index-set-0 and set-1)
  present [Turk `WBCount=8`, lines 167-174]. Skip indices whose weight is 0.
- Bone-index element format is usually `ubyte4` (r1) or `ushort4`/`0x1A225A` (r2 — Turk
  detects `WBSize=2` when format `==0x1A225A`, else 1, lines 167-172).
- Weights element is usually `ubyte4_norm` / `ubyte4` → divide by 255.

---

## 8. Material format (`.material`)

Mirage-wrapped (sample-chunk in Frontiers). `get_data().version` → 1 (Generations-era) or
**3 (Frontiers)** [`hl_hh_material.cpp:170-184`]. Node name for sample-chunk material params is
`"Material"` [`hl_hh_material.cpp:152-161`].

### 8.1 `raw_material_v3` — 0x24 bytes (**Frontiers**) [`hl_hh_material.h:76-109`]

| off  | type                                        | field |
|------|---------------------------------------------|-------|
| 0x00 | off32<char>                                 | shaderName (e.g. `"Common_d"`) |
| 0x04 | off32<char>                                 | subShaderName |
| 0x08 | off32<off32<char>>                          | textureEntryNames — array of `textureEntryCount` name pointers (the **sampler/slot names**) |
| 0x0C | off32<off32<raw_texture_entry_v1>>          | textureEntries — array of `textureEntryCount` entry pointers |
| 0x10 | u8                                          | alphaThreshold (unorm → /255) |
| 0x11 | u8                                          | noBackfaceCulling (bool) |
| 0x12 | u8                                          | useAdditiveBlending (bool) |
| 0x13 | u8                                          | unknownFlag1 |
| 0x14 | u8                                          | float4ParamCount |
| 0x15 | u8                                          | int4ParamCount |
| 0x16 | u8                                          | bool4ParamCount |
| 0x17 | u8                                          | textureEntryCount |
| 0x18 | off32<off32<raw_material_param<vec4>>>      | float4Params (array of float4ParamCount ptrs) |
| 0x1C | off32<off32<raw_material_param<ivec4>>>     | int4Params |
| 0x20 | off32<off32<raw_material_param<bvec4>>>     | bool4Params |

### 8.2 `raw_material_v1` — 0x24 bytes (Generations-era, some stages)
[`hl_hh_material.h:41-74`]: like v3 but instead of inline texture entries it has a
`off32<char> texsetName` at 0x08 (+ `u32 reserved1`) referencing an **external `.texset`**
file (which lists `.texture` files, each pointing to a `.dds`). Counts at 0x14-0x16, param
arrays at 0x18-0x20. If you meet v1 you must also load the sibling `.texset`/`.texture`.

### 8.3 `raw_texture_entry_v1` — 12 bytes [`hl_hh_texture.h:23-47`]

| off | type                  | field |
|-----|-----------------------|-------|
| 0x00| off32<char>           | texName — **the DDS file name (without `.dds`)** |
| 0x04| u8                    | texCoordIndex (which UV channel) |
| 0x05| raw_texture_wrap_mode | wrapModeU |
| 0x06| raw_texture_wrap_mode | wrapModeV |
| 0x07| u8                    | padding |
| 0x08| off32<char>           | type — texture semantic string (e.g. `"diffuse"`, `"normal"`, `"specular"`) |

`raw_texture_wrap_mode` [`hl_hh_texture.h:14-21`]: 0 repeat, 1 mirror, 2 clamp,
3 mirror_once, 4 border.

**Slot → DDS mapping** (Frontiers v3) [`hl_hh_material.cpp:123-150`]:
```
for i in range(textureEntryCount):
    slotName = textureEntryNames[i]        # e.g. "diffuse" sampler binding
    entry    = textureEntries[i]
    ddsFile  = entry.texName + ".dds"      # the actual texture file to load
    semantic = entry.type                  # diffuse/normal/specular/...
    # entry.texCoordIndex, wrapModeU/V
```
The material's texset name = the material's own name; entries are stored inline
[`hl_hh_material.cpp:129-138`]. HedgeLib forms the file path as `texName + ".dds"`
[`hl_hh_material.cpp:257-258`].

### 8.4 `raw_material_param<T>` — 12 bytes [`hl_hh_material.h:17-39`]

| off | type        | field |
|-----|-------------|-------|
| 0x00| u8          | flag1 (usually 2 or 0) |
| 0x01| u8          | flag2 (0) |
| 0x02| u8          | valueCount (# of T values in `values[]`) |
| 0x03| u8          | flag3 (0) |
| 0x04| off32<char> | name (param name, e.g. `"diffuse"`, `"PBRFactor"`) |
| 0x08| off32<T>    | values (array of `valueCount` × T) |

`T` = `vec4` (4 floats) for float4Params, `ivec4` (4 s32) for int4, `bvec4` for bool4. Common
float4 param names that map to standard PBR/lighting [`hl_hh_material.cpp:225-240`]: `diffuse`,
`specular`, `ambient`, `emissive` (take `values[0].xyz`).

---

## 9. Textures (`.dds`)

- Frontiers textures are **standard DirectDraw Surface `.dds`** files. No Mirage wrapper on the
  DDS itself. Confirmed by HedgeLib forming `texName + ".dds"` and by the DDS/DX10 handling in
  the texture-streaming header [`hl_hh_needle_texture_streaming.h:23-25,198-200`].
- Expect DXGI/BCn compressed formats: **BC1** (DXT1), **BC3** (DXT5), **BC4/BC5** (normal &
  mask maps), **BC7** (high-quality). Use the DDS header (`0x54` = format/fourCC offset; DX10
  extended header when fourCC == `"DX10"`, header size `0x80` normal / `0x94` DX10)
  [`hl_hh_needle_texture_streaming.h:23-26,196-200`]. Parse the standard DDS `DDS_HEADER`
  (+ optional `DDS_HEADER_DXT10`) and hand the mip data to a BCn decoder, or let the FBX/render
  layer consume the raw `.dds`.
- **Streamed textures (NTSI/PSTN).** Some Frontiers textures are stored via Needle texture
  streaming. A **`.dds` you find may be only a stub**; the real pixels come from a streaming
  **package** [`hl_hh_needle_texture_streaming.h`]:
  - Package magic `"PSTN"` (`signature_package`), info magic `"NTSI"` (`signature_info`).
  - The `raw_info` struct stores a rebuildable DDS header at the end (after package name +
    the tiny 4×4 mip), which you prepend to the streamed blob to reconstruct a valid `.dds`
    [`raw_info::dds_header()`, lines 186-200]. Entry names are hashed
    (`compute_name_hash`, `hash = hash*31 + c, &0x7FFFFFFF`, lines 64-73).
  - For a first-pass renderer, prefer the non-streamed `.dds` present in the PAC; handle NTSI
    only if a texture is missing/stub.

---

## 10. End-to-end parsing pseudocode

```python
# ---- MODEL ----
def parse_model(data: bytes):
    # 1. Detect header & get data base + version
    first = u32_le(data, 0)
    if first & 0x80:                                  # sample-chunk (Frontiers)
        # walk nodes from offset 0x10; find "Contexts"
        off_table_off = u32_le(data, 0x08); off_count = u32_le(data, 0x0C)
        node = find_node(data, start=0x10, name="Contexts")
        version   = node.value
        data_base = node.data_off                     # = node_pos + 16
    else:                                             # standard header
        version   = u32_le(data, 0x04)
        data_base = u32_le(data, 0x0C)                # 'data' offset (rel to file start)
        off_table_off = u32_le(data, 0x10)
    # (Optionally pre-fix all offsets using the offset table; or add data_base on read.)

    # 2. Read model struct by version (skeletal .model: v2/v5=r1, v6=r2; terrain=5)
    #    Frontiers skinned char = v6 (r2, u16 bone indices)
    mesh_groups = read_arr32(data, off=data_base+0x00, base=data_base)   # to raw_mesh_group_r2
    node_count  = u32_le(data, data_base+0x10)
    nodes_ptr   = read_off32(data, data_base+0x14, data_base)            # ptr→ptr→raw_node
    mtx_ptr     = read_off32(data, data_base+0x18, data_base)
    model_nodes = [read_node(nodes_ptr, i, data_base) for i in range(node_count)]

    # 3. Topology (strip vs list) from "Topology" property node; default = strip
    topo = find_property(data, "Topology") or 4       # 4=strip, 3=list

    out_meshes = []
    for grp in mesh_groups:
        for slot in (grp.opaq, grp.trans, grp.punch):  # + grp.special
            for mesh_ptr in slot:
                out_meshes.append(parse_mesh(data, mesh_ptr, data_base, topo, model_nodes))
    return out_meshes, model_nodes

def parse_mesh(data, mp, base, topo, model_nodes):
    material_name = read_cstr(read_off32(data, mp+0x00, base))
    faces_cnt, faces_ptr = read_arr32(data, mp+0x04, base)
    vtx_count  = u32_le(data, mp+0x0C)
    vtx_size   = u32_le(data, mp+0x10)
    vtx_ptr    = read_off32(data, mp+0x14, base)
    velem_ptr  = read_off32(data, mp+0x18, base)
    bidx_cnt, bidx_ptr = read_arr32(data, mp+0x1C, base)   # u16[] (r2) / u8[] (r1)

    # vertex declaration: read 12-byte raw_vertex_element until format==0xFFFFFFFF
    elems = []
    p = velem_ptr
    while True:
        e = read_vertex_element(data, p); p += 12
        if e.format == 0xFFFFFFFF: break
        elems.append(e)

    bone_palette = [u16_le or u8(data, bidx_ptr + i*sz) for i in range(bidx_cnt)]

    positions=normals=uvs=colors=weights=bones = per-vertex arrays
    for i in range(vtx_count):
        v0 = vtx_ptr + i*vtx_size
        for e in elems:
            raw = data[v0+e.offset : ...]
            val = decode_format(e.format, raw)        # §5.2 table → floats/ints
            route by e.type (§5.3):
              position→positions; normal→normals; texcoord→uvs[e.index]=(u,1-v);
              color→colors; blend_weight→weights (÷255); blend_indices→bones (raw local idx)

    # skinning resolution (§7)
    skin = [(model_nodes[bone_palette[li]].name, w) for (li,w) in zip(bones[i], weights[i]) if w>0]

    # index buffer (§4)
    faces_u16 = [u16_le(data, faces_ptr + 2*k) for k in range(faces_cnt)]
    tris = triangle_list(faces_u16) if topo==3 else strips_to_tris(faces_u16)  # 0xFFFF restart

    return Mesh(material_name, positions, normals, uvs, colors, skin, tris)

# ---- MATERIAL ----
def parse_material(data):
    base, version = mirage_get_data(data)             # sample-chunk "Contexts"
    assert version == 3                               # Frontiers
    shader     = read_cstr(read_off32(data, base+0x00, base))
    sub_shader = read_cstr(read_off32(data, base+0x04, base))
    tex_name_arr = read_off32(data, base+0x08, base)  # → off32<char>[]
    tex_ent_arr  = read_off32(data, base+0x0C, base)  # → off32<raw_texture_entry_v1>[]
    f4c=u8(base+0x14); i4c=u8(base+0x15); b4c=u8(base+0x16); tec=u8(base+0x17)
    f4_arr=read_off32(base+0x18); i4_arr=read_off32(base+0x1C); b4_arr=read_off32(base+0x20)

    textures = {}   # slot_name -> {dds, type, uv, wrapU, wrapV}
    for i in range(tec):
        slot = read_cstr(read_off32(data, tex_name_arr + 4*i, base))
        ep   = read_off32(data, tex_ent_arr + 4*i, base)
        dds  = read_cstr(read_off32(data, ep+0x00, base)) + ".dds"
        uv   = u8(ep+0x04); wu=u8(ep+0x05); wv=u8(ep+0x06)
        typ  = read_cstr(read_off32(data, ep+0x08, base))
        textures[slot] = dict(dds=dds, type=typ, uv=uv, wrapU=wu, wrapV=wv)

    params = {}
    for arr,cnt,T in [(f4_arr,f4c,'vec4'),(i4_arr,i4c,'ivec4'),(b4_arr,b4c,'bvec4')]:
        for i in range(cnt):
            pp = read_off32(data, arr + 4*i, base)
            vc = u8(pp+0x02); nm = read_cstr(read_off32(data, pp+0x04, base))
            vp = read_off32(data, pp+0x08, base)
            params[nm] = [read_T(data, vp + k*16, T) for k in range(vc)]
    return dict(shader=shader, sub_shader=sub_shader, textures=textures, params=params)
```

---

## 11. Practical decode notes / gotchas

- **Endianness: Frontiers PC = little-endian.** Turk645 reads big-endian — do not copy that
  blindly. Validate by checking positions look like sane floats.
- **Vertex-element array is null-terminated** by `format == 0xFFFFFFFF`, not by a count.
- **`vertexSize` is the stride**; every element sits at `base + i*stride + element.offset`.
  Elements can interleave in any order; trust `offset`, not declaration order.
- **UVs are usually `float16_2`** (half floats) → convert with a half→float routine; **flip V**
  (`1 - v`) for OpenGL/Blender/FBX convention.
- **Normals may be packed `dec3_norm` (0x2A2187)** (10-bit) OR full `float3` (0x2A23B9). Handle
  both. Sign-extend 10-bit and normalize.
- **Bone indices are 16-bit (`ushort4`, format 0x1A225A) in Frontiers r2 meshes**; 8-bit in
  older r1/terrain. The per-vertex index is a *local* index into `mesh.boneNodeIndices`.
- **Colors** are often `ubyte4`/`d3d_color`; watch channel order — `d3d_color` decodes as
  (A,R,G,B) from the u32; Turk reverses to store RGBA. Verify visually.
- **Topology** comes from the `"Topology"` sample-chunk property (3=list, 4=strip), default
  strip. Strips use `0xFFFF` primitive restart and alternating winding; drop degenerate tris.
- **Material version 3 = Frontiers**; version 1 = older (needs external `.texset`).
- **Textures = standard `.dds` (BCn)**; some are NTSI/PSTN-streamed (reconstruct DDS header
  from `raw_info` if a `.dds` stub is empty).
- The model's `nodes[]` names (with `nodeMatrices`) are the **bind skeleton**; for animation
  you additionally need `.skl.pxd` (separate agent). For a static-pose render, `nodeMatrices`
  suffice.

## 12. Open uncertainties (flagged)

- `raw_skeletal_model_v*.unknown1` is labelled "morphers" (Sajid) in HedgeLib — not decoded;
  irrelevant for basic mesh export.
- Packed formats `udec4/dec4/uhend3/hend3/dhen3` decode paths are marked `// TODO: Is this
  correct??` in HedgeLib — rarely used by Frontiers meshes; verify if you hit them.
- Whether `nodeMatrices` are bind or inverse-bind matrices isn't documented in HL comments;
  test by transforming a vertex. (Turk uses the `.skl.pxd` transforms instead.)
- v4 skeletal model is a HedgeLib TODO (uncommon; Frontiers uses v6 for skinned, v5/v6 groups).
- Sample-chunk `magic` field is `0x0133054A` but "not checked by the actual games" per HL —
  don't rely on it for detection; use the `is_root` (0x80) flag test.
```
```

## Appendix A — Repo map (cloned to scratchpad/research_repos/)

- `Hedgehog-Engine-2-Mesh-Blender-Importer/` — Turk645 Python importer (model+skeleton).
- `radford_HedgeLib/` — **Radfordhound/HedgeLib (HedgeLib++)**, authoritative C++ structs.
- `bluesky_HedgeLib/` — duplicate HedgeLib++ checkout.
- `KnuxLib/` — Knuxfan24/KnuxLib (C#); has Hedgehog **InstanceInfo/terrain** helpers but no
  full HE2 model mesh parser — not needed for this spec.
- `hyperbx_HedgeLib/` — hyperbx/HedgeLib (C#, older HedgeLib); `Models/GensModel.cs`,
  `Materials/GensMaterial.cs`, `Headers/MirageHeader.cs` — Generations-era reference only.
