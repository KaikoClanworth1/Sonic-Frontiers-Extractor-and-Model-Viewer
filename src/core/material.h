// Sonic Frontiers .material parser (legacy Mirage, big-endian base-relative offsets).
#pragma once
#include "reader.h"
#include <string>
#include <vector>

namespace sf {

struct TexEntry {
    std::string slot, dds, semantic;
    uint8_t uv_index = 0, wrap_u = 0, wrap_v = 0;
};

struct Material {
    int version = 0;
    std::string shader, sub_shader;
    std::vector<TexEntry> textures;
    uint8_t alpha_threshold = 0;
    bool additive = false, no_backface_cull = false;
};

bool parse_material(const Bytes& data, Material& out, std::string* err = nullptr);

} // namespace sf
