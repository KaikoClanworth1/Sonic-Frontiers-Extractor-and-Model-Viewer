// Sonic Frontiers PACx403L archive unpacker.
#pragma once
#include "reader.h"
#include <string>
#include <vector>

namespace sf {

struct PacEntry {
    std::string name;      // full name incl. extension, e.g. "chr_amy.model"
    std::string ext;       // "model", "material", "dds", "skl.pxd", ...
    Bytes data;
    bool from_split = false;
};

// Unpack a .pac file from disk. Returns false on failure.
bool unpack_pac(const std::string& path, std::vector<PacEntry>& out, std::string* err = nullptr);
// Unpack from an in-memory buffer.
bool unpack_pac_bytes(const Bytes& file, std::vector<PacEntry>& out, std::string* err = nullptr);

// Fast: list all contained file names (incl. split proxies) by decompressing only the
// root blob (skips the multi-MB split decompression). Used to build the global search index.
bool list_pac_names(const std::string& path, std::vector<std::pair<std::string, std::string>>& out);

} // namespace sf
