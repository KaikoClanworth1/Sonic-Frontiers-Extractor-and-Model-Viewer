#include "pac.h"
#include <fstream>
extern "C" {
#include "lz4.h"
}

namespace sf {

bool read_file(const std::string& path, Bytes& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    f.seekg(0);
    out.resize((size_t)n);
    return (bool)f.read((char*)out.data(), n);
}

bool write_file(const std::string& path, const uint8_t* data, size_t n) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)data, n);
    return (bool)f;
}

// LZ4 chunked block decompress. chunks = pairs (comp,uncomp).
static bool lz4_chunked(const uint8_t* comp, size_t comp_len,
                        const std::vector<std::pair<uint32_t, uint32_t>>& chunks,
                        size_t dst_size, Bytes& out) {
    if (comp_len == dst_size) { out.assign(comp, comp + comp_len); return true; }
    out.resize(dst_size);
    size_t sp = 0, dp = 0;
    for (auto& c : chunks) {
        int r = LZ4_decompress_safe((const char*)comp + sp, (char*)out.data() + dp,
                                    (int)c.first, (int)(dst_size - dp));
        if (r < 0 || (uint32_t)r != c.second) return false;
        sp += c.first; dp += c.second;
    }
    return dp == dst_size;
}

// Self-relative not used for PAC; offsets in v3 blob are relative to blob base 0.
struct Blob { const uint8_t* d; size_t n; };

static void walk_files(const Blob& b, size_t nodes_base, uint32_t idx,
                       std::string path, bool skip_proxies, std::vector<PacEntry>& out) {
    size_t nd = nodes_base + (size_t)idx * 0x28;
    uint64_t name_off = u64le(b.d, nd + 0x00);
    uint64_t data_off = u64le(b.d, nd + 0x08);
    uint64_t child_idx_off = u64le(b.d, nd + 0x10);
    uint16_t child_count = u16le(b.d, nd + 0x24);
    uint8_t has_data = b.d[nd + 0x26];
    uint8_t buf_start = b.d[nd + 0x27];

    if (name_off) path = path.substr(0, buf_start) + cstr(b.d, b.n, (size_t)name_off);

    if (has_data && data_off) {
        size_t de = (size_t)data_off;
        uint32_t data_size = u32le(b.d, de + 0x04);
        uint64_t payload = u64le(b.d, de + 0x10);
        uint64_t ext_off = u64le(b.d, de + 0x20);
        uint16_t flags = u16le(b.d, de + 0x28);
        if (!(skip_proxies && (flags & 1))) {
            std::string ext = cstr(b.d, b.n, (size_t)ext_off);
            PacEntry e;
            e.ext = ext;
            e.name = path + (ext.empty() ? "" : ("." + ext));
            if (payload && payload + data_size <= b.n)
                e.data.assign(b.d + payload, b.d + payload + data_size);
            e.from_split = !skip_proxies;
            out.push_back(std::move(e));
        }
    }
    for (uint16_t k = 0; k < child_count; k++) {
        int32_t ci = s32le(b.d, (size_t)child_idx_off + k * 4);
        walk_files(b, nodes_base, (uint32_t)ci, path, skip_proxies, out);
    }
}

static void walk_type_tree(const Blob& b, size_t types_off, bool skip_proxies,
                           std::vector<PacEntry>& out) {
    uint32_t data_node_count = u32le(b.d, types_off + 0x04);
    uint64_t nodes_ptr = u64le(b.d, types_off + 0x08);
    uint64_t data_idx_ptr = u64le(b.d, types_off + 0x10);
    for (uint32_t di = 0; di < data_node_count; di++) {
        int32_t tni = s32le(b.d, (size_t)data_idx_ptr + di * 4);
        size_t tnode = (size_t)nodes_ptr + (size_t)tni * 0x28;
        uint64_t file_tree_off = u64le(b.d, tnode + 0x08);
        if (!file_tree_off) continue;
        uint64_t ft_nodes = u64le(b.d, (size_t)file_tree_off + 0x08);
        walk_files(b, (size_t)ft_nodes, 0, "", skip_proxies, out);
    }
}

static bool parse_v3_blob(const Bytes& blob, const Bytes& outer, bool is_root,
                          std::vector<PacEntry>& out, bool from_split) {
    Blob b{blob.data(), blob.size()};
    if (blob.size() < 0x30) return false;
    uint32_t trees_size = u32le(b.d, 0x10);
    uint32_t dep_count = u32le(b.d, 0x2C);
    size_t types_off = 0x30;
    size_t dep_off = types_off + trees_size;

    size_t before = out.size();
    walk_type_tree(b, types_off, is_root, out);
    for (size_t i = before; i < out.size(); i++) out[i].from_split = from_split;

    if (is_root && dep_count) {
        uint64_t dep_arr_count = u64le(b.d, dep_off + 0x00);
        uint64_t dep_arr_ptr = u64le(b.d, dep_off + 0x08);
        for (uint64_t i = 0; i < dep_arr_count; i++) {
            size_t di = (size_t)dep_arr_ptr + (size_t)i * 0x20;
            uint32_t comp_size = u32le(b.d, di + 0x08);
            uint32_t uncomp_size = u32le(b.d, di + 0x0C);
            uint32_t data_pos = u32le(b.d, di + 0x10);
            uint32_t chunk_count = u32le(b.d, di + 0x14);
            uint64_t chunks_ptr = u64le(b.d, di + 0x18);
            std::vector<std::pair<uint32_t, uint32_t>> chunks;
            for (uint32_t j = 0; j < chunk_count; j++)
                chunks.emplace_back(u32le(b.d, (size_t)chunks_ptr + j * 8),
                                    u32le(b.d, (size_t)chunks_ptr + j * 8 + 4));
            if ((size_t)data_pos + comp_size > outer.size()) continue;
            Bytes split;
            if (!lz4_chunked(outer.data() + data_pos, comp_size, chunks, uncomp_size, split)) continue;
            parse_v3_blob(split, outer, false, out, true);
        }
    }
    return true;
}

bool unpack_pac_bytes(const Bytes& file, std::vector<PacEntry>& out, std::string* err) {
    if (file.size() < 0x20 || memcmp(file.data(), "PACx", 4) != 0) {
        if (err) *err = "not a PACx file";
        return false;
    }
    const uint8_t* d = file.data();
    char ver[4] = {(char)d[4], (char)d[5], (char)d[6], 0};
    if (memcmp(ver, "403", 3) == 0) {
        uint32_t root_off = u32le(d, 0x10);
        uint32_t root_comp = u32le(d, 0x14);
        uint32_t root_uncomp = u32le(d, 0x18);
        uint16_t flags_v4 = u16le(d, 0x1C);
        uint16_t flags_v3 = u16le(d, 0x1E);
        bool has_meta = flags_v4 & 0x80;
        bool is_lz4 = flags_v3 & 0x200;
        std::vector<std::pair<uint32_t, uint32_t>> root_chunks;
        if (has_meta) {
            uint32_t parents_size = u32le(d, 0x20);
            size_t chunk_off = 0x30 + parents_size;
            uint32_t n = u32le(d, chunk_off);
            for (uint32_t i = 0; i < n; i++)
                root_chunks.emplace_back(u32le(d, chunk_off + 4 + 8 * i),
                                         u32le(d, chunk_off + 4 + 8 * i + 4));
        }
        Bytes root;
        if (is_lz4) {
            if (!lz4_chunked(d + root_off, root_comp, root_chunks, root_uncomp, root)) {
                if (err) *err = "root lz4 decompress failed";
                return false;
            }
        } else {
            root.assign(d + root_off, d + root_off + root_comp);
        }
        return parse_v3_blob(root, file, true, out, false);
    } else if (memcmp(ver, "402", 3) == 0 || memcmp(ver, "301", 3) == 0) {
        return parse_v3_blob(file, file, true, out, false);
    }
    if (err) *err = std::string("unsupported PACx version ") + ver;
    return false;
}

bool unpack_pac(const std::string& path, std::vector<PacEntry>& out, std::string* err) {
    Bytes file;
    if (!read_file(path, file)) { if (err) *err = "cannot read file"; return false; }
    return unpack_pac_bytes(file, out, err);
}

// Names-only walk (includes proxies, no split recursion).
static void names_walk(const Blob& b, size_t nodes_base, uint32_t idx, std::string path,
                       std::vector<std::pair<std::string, std::string>>& out) {
    size_t nd = nodes_base + (size_t)idx * 0x28;
    uint64_t name_off = u64le(b.d, nd + 0x00);
    uint64_t data_off = u64le(b.d, nd + 0x08);
    uint64_t child_idx_off = u64le(b.d, nd + 0x10);
    uint16_t child_count = u16le(b.d, nd + 0x24);
    uint8_t has_data = b.d[nd + 0x26];
    uint8_t buf_start = b.d[nd + 0x27];
    if (name_off) path = path.substr(0, buf_start) + cstr(b.d, b.n, (size_t)name_off);
    if (has_data && data_off) {
        std::string ext = cstr(b.d, b.n, (size_t)u64le(b.d, (size_t)data_off + 0x20));
        out.emplace_back(path + (ext.empty() ? "" : "." + ext), ext);
    }
    for (uint16_t k = 0; k < child_count; k++)
        names_walk(b, nodes_base, (uint32_t)s32le(b.d, (size_t)child_idx_off + k * 4), path, out);
}

bool list_pac_names(const std::string& path, std::vector<std::pair<std::string, std::string>>& out) {
    Bytes file;
    if (!read_file(path, file)) return false;
    if (file.size() < 0x20 || memcmp(file.data(), "PACx", 4) != 0) return false;
    const uint8_t* d = file.data();
    if (memcmp(d + 4, "403", 3) != 0) {   // treat non-403 as a v3 blob directly
        Blob b{file.data(), file.size()};
        uint32_t trees = u32le(b.d, 0x10); (void)trees;
        // fall through to generic below is complex; just full-unpack rare cases
        std::vector<PacEntry> ents;
        if (!unpack_pac_bytes(file, ents, nullptr)) return false;
        for (auto& e : ents) out.emplace_back(e.name, e.ext);
        return true;
    }
    uint32_t root_off = u32le(d, 0x10), root_comp = u32le(d, 0x14), root_uncomp = u32le(d, 0x18);
    uint16_t flags_v4 = u16le(d, 0x1C), flags_v3 = u16le(d, 0x1E);
    std::vector<std::pair<uint32_t, uint32_t>> chunks;
    if (flags_v4 & 0x80) {
        uint32_t parents_size = u32le(d, 0x20);
        size_t co = 0x30 + parents_size;
        uint32_t nn = u32le(d, co);
        for (uint32_t i = 0; i < nn; i++) chunks.emplace_back(u32le(d, co + 4 + 8*i), u32le(d, co + 4 + 8*i + 4));
    }
    Bytes root;
    if (flags_v3 & 0x200) { if (!lz4_chunked(d + root_off, root_comp, chunks, root_uncomp, root)) return false; }
    else root.assign(d + root_off, d + root_off + root_comp);
    Blob b{root.data(), root.size()};
    uint32_t trees_size = u32le(b.d, 0x10);
    size_t types_off = 0x30;
    uint32_t data_node_count = u32le(b.d, types_off + 0x04);
    uint64_t nodes_ptr = u64le(b.d, types_off + 0x08);
    uint64_t data_idx_ptr = u64le(b.d, types_off + 0x10);
    for (uint32_t di = 0; di < data_node_count; di++) {
        int32_t tni = s32le(b.d, (size_t)data_idx_ptr + di * 4);
        size_t tnode = (size_t)nodes_ptr + (size_t)tni * 0x28;
        uint64_t ft = u64le(b.d, tnode + 0x08);
        if (!ft) continue;
        names_walk(b, (size_t)u64le(b.d, (size_t)ft + 0x08), 0, "", out);
    }
    (void)trees_size;
    return true;
}

} // namespace sf
