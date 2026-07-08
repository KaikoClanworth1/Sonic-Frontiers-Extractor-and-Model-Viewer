#include "material.h"
#include "mirage.h"

namespace sf {

bool parse_material(const Bytes& data, Material& mat, std::string* err) {
    const uint8_t* d = data.data();
    size_t n = data.size();
    auto blocks = sc_find_model_blocks(d, n);
    size_t header_off = blocks.empty() ? 0 : blocks[0];
    size_t limit;
    auto roots = sc_parse(d, n, header_off, limit);
    const SCNode* ctx = sc_find(roots, "Contexts");
    if (!ctx) ctx = sc_find(roots, "Material");
    if (!ctx) { if (err) *err = "no Contexts/Material node"; return false; }
    size_t base = ctx->data_off();
    size_t ob = header_off + 0x10;
    mat.version = ctx->value;

    auto off = [&](size_t field) -> size_t {
        uint32_t v = u32be(d, field);
        return v ? ob + v : 0;
    };

    mat.shader = cstr(d, n, off(base + 0x00));
    mat.sub_shader = cstr(d, n, off(base + 0x04));
    size_t tex_names_arr = off(base + 0x08);
    size_t tex_entries_arr = off(base + 0x0C);
    mat.alpha_threshold = d[base + 0x10];
    mat.no_backface_cull = d[base + 0x11] != 0;
    mat.additive = d[base + 0x12] != 0;
    uint8_t tex_count = d[base + 0x17];

    for (uint8_t i = 0; i < tex_count; i++) {
        TexEntry te;
        if (tex_names_arr) te.slot = cstr(d, n, off(tex_names_arr + i * 4));
        if (tex_entries_arr) {
            size_t ent = off(tex_entries_arr + i * 4);
            if (ent) {
                std::string name = cstr(d, n, off(ent + 0x00));
                te.dds = name.empty() ? "" : name + ".dds";
                te.uv_index = d[ent + 0x04];
                te.wrap_u = d[ent + 0x05];
                te.wrap_v = d[ent + 0x06];
                te.semantic = cstr(d, n, off(ent + 0x08));
            }
        }
        mat.textures.push_back(std::move(te));
    }
    return true;
}

} // namespace sf
