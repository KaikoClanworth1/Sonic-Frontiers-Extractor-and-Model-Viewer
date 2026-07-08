#include "renderer.h"
#include <glad/gl.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <cmath>
#include <cstdio>

namespace sf {

// GL compressed formats (define explicitly; not all in core headers)
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#define GL_COMPRESSED_RED_RGTC1_ 0x8DBB
#define GL_COMPRESSED_RG_RGTC2_ 0x8DBD
#define GL_COMPRESSED_RGBA_BPTC_UNORM_ 0x8E8C
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ 0x8E8D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_ 0x8C4F
#define GL_COMPRESSED_SRGB_S3TC_DXT1_ 0x8C4C

static const char* VS = R"(#version 330 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNrm; layout(location=2) in vec2 aUv;
uniform mat4 uMVP; uniform mat4 uModel;
out vec3 vN; out vec2 vUv;
void main(){ gl_Position=uMVP*vec4(aPos,1.0); vN=mat3(uModel)*aNrm; vUv=aUv; })";

static const char* FS = R"(#version 330 core
in vec3 vN; in vec2 vUv; out vec4 frag;
uniform sampler2D uTex; uniform int uHasTex; uniform int uUseTex;
void main(){
  vec3 n=normalize(vN);
  float d=max(dot(n,normalize(vec3(0.4,0.7,0.6))),0.0)*0.8+0.35;
  vec3 base=vec3(0.72,0.72,0.75);
  if(uHasTex==1 && uUseTex==1){ vec4 t=texture(uTex,vUv); if(t.a<0.35) discard; base=t.rgb; }
  frag=vec4(base*d,1.0);
})";

static uint32_t compile(uint32_t type, const char* src) {
    uint32_t s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log); fprintf(stderr, "shader: %s\n", log); }
    return s;
}

bool Renderer::init() {
    uint32_t vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    prog_ = glCreateProgram();
    glAttachShader(prog_, vs); glAttachShader(prog_, fs); glLinkProgram(prog_);
    glDeleteShader(vs); glDeleteShader(fs);
    glEnable(GL_DEPTH_TEST);
    return prog_ != 0;
}

uint32_t Renderer::upload_dds(const Bytes& dds) {
    if (dds.size() < 128 || memcmp(dds.data(), "DDS ", 4) != 0) return 0;
    const uint8_t* d = dds.data();
    uint32_t height = u32le(d, 0x0C), width = u32le(d, 0x10);
    uint32_t mipCount = u32le(d, 0x1C); if (mipCount == 0) mipCount = 1;
    char fourcc[5] = {(char)d[0x54],(char)d[0x55],(char)d[0x56],(char)d[0x57],0};
    size_t data_off = 0x80;
    GLenum fmt = 0; int block = 16;
    if (memcmp(fourcc, "DXT1", 4) == 0) { fmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; block = 8; }
    else if (memcmp(fourcc, "DXT3", 4) == 0) { fmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; block = 16; }
    else if (memcmp(fourcc, "DXT5", 4) == 0) { fmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; block = 16; }
    else if (memcmp(fourcc, "ATI1", 4) == 0 || memcmp(fourcc, "BC4U", 4) == 0) { fmt = GL_COMPRESSED_RED_RGTC1_; block = 8; }
    else if (memcmp(fourcc, "ATI2", 4) == 0 || memcmp(fourcc, "BC5U", 4) == 0) { fmt = GL_COMPRESSED_RG_RGTC2_; block = 16; }
    else if (memcmp(fourcc, "DX10", 4) == 0) {
        uint32_t dxgi = u32le(d, 0x80); data_off = 0x94;
        switch (dxgi) {
        case 71: case 72: fmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; block = 8; break;
        case 74: case 75: fmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; block = 16; break;
        case 77: case 78: fmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; block = 16; break;
        case 80: case 81: fmt = GL_COMPRESSED_RED_RGTC1_; block = 8; break;
        case 83: case 84: fmt = GL_COMPRESSED_RG_RGTC2_; block = 16; break;
        case 98: case 99: fmt = GL_COMPRESSED_RGBA_BPTC_UNORM_; block = 16; break;
        default: return 0;
        }
    } else return 0;

    uint32_t tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    size_t off = data_off;
    uint32_t w = width, h = height;
    for (uint32_t lvl = 0; lvl < mipCount; lvl++) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        size_t size = (size_t)bw * bh * block;
        if (off + size > dds.size()) break;
        glCompressedTexImage2D(GL_TEXTURE_2D, lvl, fmt, w, h, 0, (GLsizei)size, d + off);
        off += size;
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipCount > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return tex;
}

void Renderer::clear_model() {
    for (auto& m : meshes_) {
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.ebo) glDeleteBuffers(1, &m.ebo);
        if (m.tex) glDeleteTextures(1, &m.tex);
    }
    meshes_.clear();
}

void Renderer::set_model(const Model& model, const TexLookup& tex,
                         const std::vector<std::pair<std::string, std::string>>& mesh_diffuse) {
    clear_model();
    float mn[3] = {1e9f,1e9f,1e9f}, mx[3] = {-1e9f,-1e9f,-1e9f};
    for (size_t mi = 0; mi < model.meshes.size(); mi++) {
        const Mesh& msh = model.meshes[mi];
        if (msh.positions.empty() || msh.faces.empty()) continue;
        // Skip inverted-hull outline meshes in the viewport (they z-fight); still exported to FBX.
        if (msh.material.find("outline") != std::string::npos) continue;
        std::vector<float> vb; vb.reserve(msh.positions.size() * 8);
        for (size_t i = 0; i < msh.positions.size(); i++) {
            const Vec3& p = msh.positions[i];
            for (int k = 0; k < 3; k++) { float c = (&p.x)[k]; if (c < mn[k]) mn[k] = c; if (c > mx[k]) mx[k] = c; }
            Vec3 nn = i < msh.normals.size() ? msh.normals[i] : Vec3{0,0,1};
            Vec2 uv = (!msh.uvs.empty() && i < msh.uvs[0].size()) ? msh.uvs[0][i] : Vec2{0,0};
            vb.push_back(p.x); vb.push_back(p.y); vb.push_back(p.z);
            vb.push_back(nn.x); vb.push_back(nn.y); vb.push_back(nn.z);
            vb.push_back(uv.u); vb.push_back(1.f - uv.v);
        }
        std::vector<uint32_t> ib; ib.reserve(msh.faces.size() * 3);
        for (auto& f : msh.faces) { ib.push_back(f[0]); ib.push_back(f[1]); ib.push_back(f[2]); }

        GpuMesh gm; gm.index_count = (int)ib.size();
        glGenVertexArrays(1, &gm.vao); glBindVertexArray(gm.vao);
        glGenBuffers(1, &gm.vbo); glBindBuffer(GL_ARRAY_BUFFER, gm.vbo);
        glBufferData(GL_ARRAY_BUFFER, vb.size() * 4, vb.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &gm.ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, ib.size() * 4, ib.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void*)0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void*)12); glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void*)24); glEnableVertexAttribArray(2);
        glBindVertexArray(0);

        // diffuse texture
        std::string dds = mi < mesh_diffuse.size() ? mesh_diffuse[mi].second : "";
        if (!dds.empty() && tex) {
            Bytes b = tex(dds);
            if (!b.empty()) { gm.tex = upload_dds(b); gm.has_tex = gm.tex != 0; }
        }
        meshes_.push_back(gm);
    }
    for (int k = 0; k < 3; k++) center_[k] = (mn[k] + mx[k]) * 0.5f;
    radius_ = 0.001f;
    for (int k = 0; k < 3; k++) radius_ = std::max(radius_, (mx[k] - mn[k]) * 0.5f);
    reset_view();
}

void Renderer::reset_view() { yaw_ = 0.6f; pitch_ = 0.15f; dist_ = radius_ * 3.2f; pan_x_ = pan_y_ = 0; }
void Renderer::orbit(float dy, float dp) { yaw_ += dy; pitch_ += dp; if (pitch_ > 1.5f) pitch_ = 1.5f; if (pitch_ < -1.5f) pitch_ = -1.5f; }
void Renderer::zoom(float dz) { dist_ *= (1.0f - dz * 0.1f); if (dist_ < radius_ * 0.2f) dist_ = radius_ * 0.2f; }
void Renderer::pan(float dx, float dy) { pan_x_ += dx * radius_; pan_y_ += dy * radius_; }

static void mat_mul(const float* a, const float* b, float* o) {
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) { float s = 0; for (int k = 0; k < 4; k++) s += a[k*4+r]*b[c*4+k]; o[c*4+r] = s; }
}

void Renderer::draw(int w, int h) {
    if (w <= 0 || h <= 0) return;
    glViewport(0, 0, w, h);
    glClearColor(0.13f, 0.14f, 0.17f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (meshes_.empty()) return;

    float aspect = (float)w / h;
    float fov = 45.f * 3.14159f / 180.f, nf = radius_ * 0.05f, ff = radius_ * 50.f;
    float f = 1.f / tanf(fov / 2);
    float proj[16] = { f/aspect,0,0,0, 0,f,0,0, 0,0,(ff+nf)/(nf-ff),-1, 0,0,(2*ff*nf)/(nf-ff),0 };

    float cx = center_[0] + pan_x_, cy = center_[1] + pan_y_, cz = center_[2];
    float ex = cx + dist_ * cosf(pitch_) * sinf(yaw_);
    float ey = cy + dist_ * sinf(pitch_);
    float ez = cz + dist_ * cosf(pitch_) * cosf(yaw_);
    float fwd[3] = {cx-ex, cy-ey, cz-ez};
    float fl = sqrtf(fwd[0]*fwd[0]+fwd[1]*fwd[1]+fwd[2]*fwd[2]); for (int k=0;k<3;k++) fwd[k]/=fl;
    float up[3] = {0,1,0};
    float s[3] = {fwd[1]*up[2]-fwd[2]*up[1], fwd[2]*up[0]-fwd[0]*up[2], fwd[0]*up[1]-fwd[1]*up[0]};
    float sl = sqrtf(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]); for (int k=0;k<3;k++) s[k]/=sl;
    float u[3] = {s[1]*fwd[2]-s[2]*fwd[1], s[2]*fwd[0]-s[0]*fwd[2], s[0]*fwd[1]-s[1]*fwd[0]};
    float view[16] = { s[0],u[0],-fwd[0],0, s[1],u[1],-fwd[1],0, s[2],u[2],-fwd[2],0,
                       -(s[0]*ex+s[1]*ey+s[2]*ez), -(u[0]*ex+u[1]*ey+u[2]*ez), (fwd[0]*ex+fwd[1]*ey+fwd[2]*ez), 1 };
    float mvp[16]; mat_mul(proj, view, mvp);
    float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    glUseProgram(prog_);
    glUniformMatrix4fv(glGetUniformLocation(prog_, "uMVP"), 1, GL_FALSE, mvp);
    glUniformMatrix4fv(glGetUniformLocation(prog_, "uModel"), 1, GL_FALSE, ident);
    glUniform1i(glGetUniformLocation(prog_, "uUseTex"), show_texture_ ? 1 : 0);
    glPolygonMode(GL_FRONT_AND_BACK, wire_ ? GL_LINE : GL_FILL);
    for (auto& m : meshes_) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m.tex);
        glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);
        glUniform1i(glGetUniformLocation(prog_, "uHasTex"), m.has_tex ? 1 : 0);
        glBindVertexArray(m.vao);
        glDrawElements(GL_TRIANGLES, m.index_count, GL_UNSIGNED_INT, 0);
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glBindVertexArray(0);
}

void Renderer::render(int w, int h) { draw(w, h); }

unsigned int Renderer::render_to_texture(int w, int h) {
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (vp_fbo_ == 0) glGenFramebuffers(1, &vp_fbo_);
    if (w != vp_tw_ || h != vp_th_) {
        if (vp_tex_) glDeleteTextures(1, &vp_tex_);
        if (vp_depth_) glDeleteRenderbuffers(1, &vp_depth_);
        glGenTextures(1, &vp_tex_); glBindTexture(GL_TEXTURE_2D, vp_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenRenderbuffers(1, &vp_depth_); glBindRenderbuffer(GL_RENDERBUFFER, vp_depth_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        vp_tw_ = w; vp_th_ = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, vp_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vp_tex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, vp_depth_);
    draw(w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return vp_tex_;
}

bool Renderer::capture_png(const std::string& path, int w, int h) {
    uint32_t fbo, color, depth;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &color); glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glGenRenderbuffers(1, &depth); glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    draw(w, h);
    std::vector<uint8_t> px(w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    // flip vertically
    std::vector<uint8_t> flip(w * h * 4);
    for (int y = 0; y < h; y++) memcpy(&flip[y*w*4], &px[(h-1-y)*w*4], w*4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &color); glDeleteRenderbuffers(1, &depth);
    return stbi_write_png(path.c_str(), w, h, 4, flip.data(), w * 4) != 0;
}

} // namespace sf
