// Minimal binary FBX 7.4 writer + Frontiers model->FBX convenience.
#pragma once
#include "reader.h"
#include "model.h"
#include "material.h"
#include "skeleton.h"
#include "pac.h"
#include <string>
#include <vector>

namespace sf {

struct FbxMeshInput {
    const Mesh* mesh;
    std::vector<std::pair<std::string, std::string>> textures;  // (semantic, dds)
    // skin resolved to (boneName,weight) per vertex is computed inside from model node names
};

// Build a binary FBX from a parsed model (+ optional skeleton & materials) into out bytes.
bool build_fbx(const Model& model, const std::vector<SkelBone>& bones,
               const std::vector<Material>& materials, Bytes& out);

// Convenience: parse model/skeleton/materials from pac entries and write an FBX file.
bool export_pac_model_to_fbx(const std::vector<PacEntry>& entries,
                             const std::string& basename, const std::string& out_path,
                             std::string* err = nullptr);

} // namespace sf
