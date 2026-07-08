// Needle Texture Streaming: reconstruct full DDS from an NTSI stub + PSTN .ntsp package.
#pragma once
#include "reader.h"
#include <string>

namespace sf {

inline bool is_ntsi(const Bytes& d) { return d.size() >= 4 && memcmp(d.data(), "NTSI", 4) == 0; }

// Reconstruct a full .dds from an NTSI stub. tex_base = dds name without extension.
// streaming_dir = ...\image\x64\raw\texture_streaming. Returns false if unresolved.
bool resolve_streamed_dds(const Bytes& stub, const std::string& tex_base,
                          const std::string& streaming_dir, Bytes& out);

} // namespace sf
