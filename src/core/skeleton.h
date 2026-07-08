// Sonic Frontiers .skl.pxd skeleton parser (PXSK / BINA210L, little-endian).
#pragma once
#include "reader.h"
#include <string>
#include <vector>

namespace sf {

struct SkelBone {
    std::string name;
    int parent = -1;
    float t[3] = {0, 0, 0};
    float q[4] = {0, 0, 0, 1};   // x,y,z,w
    float s[3] = {1, 1, 1};
};

bool parse_skeleton(const Bytes& data, std::vector<SkelBone>& out, std::string* err = nullptr);

} // namespace sf
