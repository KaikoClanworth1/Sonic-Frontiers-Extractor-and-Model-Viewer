#include "ntsp.h"
#include <fstream>
#include <map>
#include <memory>
#include <unordered_map>

namespace sf {

static uint32_t name_hash(const std::string& s) {
    uint32_t h = 0;
    for (unsigned char c : s) h = h * 31 + c;
    return h & 0x7FFFFFFF;
}

// Cached read-only PSTN package: entry hash -> (blobIndex, blobCount), + blob table.
struct Package {
    std::string path;
    std::ifstream f;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> by_hash;  // hash -> (blobIndex,count)
    Bytes head;
    size_t blob_base = 0;
    bool ok = false;

    explicit Package(const std::string& p) : path(p), f(p, std::ios::binary) {
        if (!f) return;
        uint8_t hdr[24];
        f.read((char*)hdr, 24);
        if (memcmp(hdr, "PSTN", 4) != 0) return;
        uint32_t entry_count = u32le(hdr, 8), blob_count = u32le(hdr, 12);
        uint64_t header_size = u64le(hdr, 16);
        head.resize(header_size);
        f.seekg(0);
        f.read((char*)head.data(), header_size);
        blob_base = 0x18 + (size_t)entry_count * 0x18;
        (void)blob_count;
        for (uint32_t i = 0; i < entry_count; i++) {
            size_t o = 0x18 + (size_t)i * 0x18;
            by_hash[u32le(head.data(), o)] = {u32le(head.data(), o + 4), u32le(head.data(), o + 8)};
        }
        ok = true;
    }

    bool mips(const std::string& name, Bytes& out) {
        auto it = by_hash.find(name_hash(name));
        if (it == by_hash.end()) return false;
        uint32_t bi = it->second.first, bc = it->second.second;
        for (uint32_t k = 0; k < bc; k++) {
            size_t o = blob_base + (size_t)(bi + k) * 0x10;
            if (o + 16 > head.size()) return false;
            uint64_t off = u64le(head.data(), o), sz = u64le(head.data(), o + 8);
            size_t cur = out.size();
            out.resize(cur + sz);
            f.seekg(off);
            f.read((char*)out.data() + cur, sz);
        }
        return true;
    }
};

static std::map<std::string, std::unique_ptr<Package>>& cache() {
    static std::map<std::string, std::unique_ptr<Package>> c;
    return c;
}

bool resolve_streamed_dds(const Bytes& stub, const std::string& tex_base,
                          const std::string& streaming_dir, Bytes& out) {
    if (!is_ntsi(stub) || stub.size() < 0x18) return false;
    const uint8_t* d = stub.data();
    uint32_t pns = u32le(d, 0x0C), m4s = u32le(d, 0x10);
    std::string pkg_name((const char*)d + 0x18);   // nul-terminated within packageNameSize
    if (pkg_name.size() > pns) pkg_name.resize(pns);
    size_t hdr_off = 0x18 + pns + m4s;
    if (hdr_off + 0x80 > stub.size()) return false;
    bool dx10 = memcmp(d + hdr_off + 0x54, "DX10", 4) == 0;
    size_t hdr_size = dx10 ? 0x94 : 0x80;

    std::string pkg_path = streaming_dir + "\\" + pkg_name + ".ntsp";
    auto& c = cache();
    auto it = c.find(pkg_path);
    if (it == c.end())
        it = c.emplace(pkg_path, std::make_unique<Package>(pkg_path)).first;
    if (!it->second->ok) return false;

    out.assign(d + hdr_off, d + hdr_off + hdr_size);   // DDS header from the stub
    return it->second->mips(tex_base, out);            // append streamed mip data
}

} // namespace sf
