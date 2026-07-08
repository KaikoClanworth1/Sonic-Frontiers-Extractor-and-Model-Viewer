// Sonic Frontiers .model / .terrain-model parser.
#pragma once
#include "reader.h"
#include <array>
#include <string>
#include <vector>

namespace sf {

struct Vec3 { float x = 0, y = 0, z = 0; };
struct Vec2 { float u = 0, v = 0; };
struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };

struct Mesh {
    std::string material;
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<std::vector<Vec2>> uvs;     // per channel
    std::vector<Vec4> colors;
    std::vector<std::array<uint32_t, 4>> bone_indices;  // global node indices
    std::vector<std::array<float, 4>> weights;
    std::vector<std::array<uint32_t, 3>> faces;
};

struct Model {
    int version = 0;
    bool is_terrain = false;
    std::vector<std::string> node_names;    // bone names
    std::vector<int> node_parents;
    std::vector<Mesh> meshes;
};

bool parse_model(const Bytes& data, Model& out, std::string* err = nullptr, int is_terrain = -1);

} // namespace sf
