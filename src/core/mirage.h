// Hedgehog Engine 2 "Mirage" sample-chunk container (big-endian node headers).
#pragma once
#include "reader.h"
#include <string>
#include <vector>
#include <memory>

namespace sf {

static const uint32_t SC_MAGIC = 0x0133054A;

struct SCNode {
    uint32_t flags = 0, value = 0;
    std::string name;
    size_t off = 0, size = 0;
    bool is_leaf = false, is_last = false, is_root = false;
    std::vector<SCNode> children;
    size_t data_off() const { return off + 0x10; }
};

// Parse a sibling chain of sample-chunk nodes in [start, limit).
std::vector<SCNode> sc_parse_children(const uint8_t* d, size_t n, size_t start, size_t limit, int depth = 0);

// Parse a sample-chunk raw_header at header_off. Returns root nodes; sets limit.
std::vector<SCNode> sc_parse(const uint8_t* d, size_t n, size_t header_off, size_t& limit);

// Depth-first search by name.
const SCNode* sc_find(const std::vector<SCNode>& nodes, const std::string& name);

// Locate sample-chunk model headers (handles NEDARCV1 wrapper) — returns raw_header offsets.
std::vector<size_t> sc_find_model_blocks(const uint8_t* d, size_t n);

} // namespace sf
