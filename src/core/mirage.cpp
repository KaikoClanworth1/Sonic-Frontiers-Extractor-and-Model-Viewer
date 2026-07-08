#include "mirage.h"

namespace sf {

static SCNode read_node(const uint8_t* d, size_t off) {
    SCNode n;
    n.off = off;
    n.flags = u32be(d, off);
    n.value = u32be(d, off + 4);
    char nm[9] = {0};
    memcpy(nm, d + off + 8, 8);
    // strip trailing spaces/nulls
    int e = 8;
    while (e > 0 && (nm[e - 1] == ' ' || nm[e - 1] == 0)) e--;
    n.name.assign(nm, e);
    n.size = n.flags & 0x1FFFFFFF;
    n.is_leaf = (n.flags & 0x20000000) != 0;
    n.is_last = (n.flags & 0x40000000) != 0;
    n.is_root = (n.flags & 0x80000000) != 0;
    return n;
}

std::vector<SCNode> sc_parse_children(const uint8_t* d, size_t n, size_t start, size_t limit, int depth) {
    std::vector<SCNode> nodes;
    size_t p = start;
    int guard = 0;
    while (p + 0x10 <= limit && guard < 4096) {
        guard++;
        SCNode node = read_node(d, p);
        if (!node.is_leaf && node.size > 0x10) {
            size_t clim = p + node.size < limit ? p + node.size : limit;
            node.children = sc_parse_children(d, n, p + 0x10, clim, depth + 1);
        }
        bool last = node.is_last || node.is_root;
        size_t sz = node.size;
        nodes.push_back(std::move(node));
        if (last || sz == 0) break;
        p += sz;
    }
    return nodes;
}

std::vector<SCNode> sc_parse(const uint8_t* d, size_t n, size_t header_off, size_t& limit) {
    uint32_t fs = u32be(d, header_off);
    size_t size = fs & 0x1FFFFFFF;
    limit = size ? header_off + size : n;
    if (limit > n) limit = n;
    return sc_parse_children(d, n, header_off + 0x10, limit);
}

const SCNode* sc_find(const std::vector<SCNode>& nodes, const std::string& name) {
    for (auto& node : nodes) {
        if (node.name == name) return &node;
        const SCNode* f = sc_find(node.children, name);
        if (f) return f;
    }
    return nullptr;
}

std::vector<size_t> sc_find_model_blocks(const uint8_t* d, size_t n) {
    std::vector<size_t> blocks;
    uint8_t mg[4] = {0x01, 0x33, 0x05, 0x4a};
    for (size_t i = 0; i + 4 <= n; i++) {
        if (memcmp(d + i, mg, 4) == 0) {
            if (i >= 4 && (u32be(d, i - 4) & 0x80000000)) blocks.push_back(i - 4);
        }
    }
    return blocks;
}

} // namespace sf
