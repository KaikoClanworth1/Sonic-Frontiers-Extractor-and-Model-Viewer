// OpenGL model renderer for the viewport (+ offscreen PNG capture).
#pragma once
#include "model.h"
#include "reader.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sf {

// texture lookup: given a dds file name, return its bytes (empty if missing).
using TexLookup = std::function<Bytes(const std::string&)>;

class Renderer {
public:
    bool init();                 // compile shaders; call after GL context ready
    void set_model(const Model& model, const TexLookup& tex,
                   const std::vector<std::pair<std::string, std::string>>& mesh_diffuse);
    void clear_model();
    void render(int fb_w, int fb_h);
    // Render the model into an internal FBO of size w x h; returns the color texture id.
    unsigned int render_to_texture(int w, int h);
    bool capture_png(const std::string& path, int w, int h);

    // camera controls
    void orbit(float dyaw, float dpitch);
    void zoom(float dz);
    void pan(float dx, float dy);
    void reset_view();
    bool has_model() const { return !meshes_.empty(); }
    int mesh_count() const { return (int)meshes_.size(); }
    void set_show_texture(bool v) { show_texture_ = v; }
    void set_show_wire(bool v) { wire_ = v; }

private:
    struct GpuMesh { uint32_t vao=0, vbo=0, ebo=0, tex=0; int index_count=0; bool has_tex=false; };
    std::vector<GpuMesh> meshes_;
    uint32_t prog_ = 0;
    uint32_t vp_fbo_ = 0, vp_tex_ = 0, vp_depth_ = 0;
    int vp_tw_ = 0, vp_th_ = 0;
    float center_[3] = {0,0,0};
    float radius_ = 1.0f;
    float yaw_ = 0.6f, pitch_ = 0.2f, dist_ = 3.0f;
    float pan_x_ = 0, pan_y_ = 0;
    bool show_texture_ = true, wire_ = false;
    uint32_t upload_dds(const Bytes& dds);
    void draw(int w, int h);
};

} // namespace sf
