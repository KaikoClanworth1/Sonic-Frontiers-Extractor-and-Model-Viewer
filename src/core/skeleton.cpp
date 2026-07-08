#include "skeleton.h"

namespace sf {

bool parse_skeleton(const Bytes& data, std::vector<SkelBone>& out, std::string* err) {
    const uint8_t* d = data.data();
    size_t n = data.size();
    const size_t B = 0x40;
    if (n < B + 0x50 || memcmp(d + B, "KSXP", 4) != 0) { if (err) *err = "bad skeleton magic"; return false; }
    size_t parent_arr = u32le(d, B + 0x08) + B;
    uint32_t count = u32le(d, B + 0x10);
    size_t name_tab = u32le(d, B + 0x28) + B;
    size_t mtx_tab = u32le(d, B + 0x48) + B;

    for (uint32_t i = 0; i < count; i++) {
        SkelBone b;
        b.parent = s16le(d, parent_arr + i * 2);
        size_t name_ptr = (size_t)u64le(d, name_tab + i * 0x10) + B;
        b.name = cstr(d, n, name_ptr);
        size_t mo = mtx_tab + i * 0x30;
        for (int k = 0; k < 3; k++) b.t[k] = f32le(d, mo + k * 4);
        for (int k = 0; k < 4; k++) b.q[k] = f32le(d, mo + 0x10 + k * 4);
        for (int k = 0; k < 3; k++) b.s[k] = f32le(d, mo + 0x20 + k * 4);
        out.push_back(std::move(b));
    }
    return true;
}

} // namespace sf
