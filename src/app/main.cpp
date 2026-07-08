// Sonic Frontiers Extractor & Model Viewer — Dear ImGui + OpenGL desktop app.
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "tinyfiledialogs.h"

#include "pac.h"
#include "model.h"
#include "material.h"
#include "skeleton.h"
#include "config.h"
#include "fbx_writer.h"
#include "ntsp.h"
#include "renderer.h"
#include "stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>

namespace fs = std::filesystem;
using namespace sf;

struct IndexEntry { std::string name, ext, pac; };

struct App {
    Config cfg;
    Renderer renderer;
    std::string status = "Set the game folder to begin.";

    // browser: category -> list of pac paths
    std::map<std::string, std::vector<std::string>> categories;

    // global search index (built in background)
    std::vector<IndexEntry> index;
    std::mutex index_mtx;
    std::atomic<bool> indexing{false};
    std::atomic<int> index_done{0}, index_total{0};
    std::thread index_thread;

    char search[128] = "";
    bool models_only = false;                // filter browser + search to .model/.terrain-model
    int sort_mode = 0;                        // 0 = by name, 1 = by type/extension
    std::string open_pac;                    // currently expanded pac
    std::vector<PacEntry> open_entries;      // its contents
    std::string loaded_label;

    ~App() { if (index_thread.joinable()) index_thread.join(); }
};

static std::string base_of(const std::string& p) {
    auto s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

static void scan_folder(App& app) {
    app.categories.clear();
    std::string root = app.cfg.game_folder;
    if (root.empty() || !fs::exists(root)) return;
    // accept either the raw/ folder or the game root; locate raw/
    fs::path raw = root;
    if (fs::exists(fs::path(root) / "image" / "x64" / "raw")) raw = fs::path(root) / "image" / "x64" / "raw";
    for (auto& e : fs::recursive_directory_iterator(raw, fs::directory_options::skip_permission_denied)) {
        if (!e.is_regular_file()) continue;
        auto p = e.path();
        if (p.extension() != ".pac") continue;
        std::string rel = fs::relative(p.parent_path(), raw).string();
        std::string cat = rel.empty() ? "." : rel.substr(0, rel.find_first_of("/\\"));
        app.categories[cat].push_back(p.string());
    }
    for (auto& kv : app.categories) std::sort(kv.second.begin(), kv.second.end());
    app.status = "Scanned " + std::to_string(app.categories.size()) + " categories.";
}

static void build_index(App* app) {
    std::vector<std::string> pacs;
    for (auto& kv : app->categories) for (auto& p : kv.second) pacs.push_back(p);
    app->index_total = (int)pacs.size();
    app->index_done = 0;
    std::vector<IndexEntry> idx;
    for (auto& p : pacs) {
        std::vector<std::pair<std::string, std::string>> names;
        if (list_pac_names(p, names))
            for (auto& n : names) idx.push_back({n.first, n.second, p});
        app->index_done++;
    }
    { std::lock_guard<std::mutex> lk(app->index_mtx); app->index = std::move(idx); }
    app->indexing = false;
}

static void start_index(App& app) {
    if (app.indexing) return;
    if (app.index_thread.joinable()) app.index_thread.join();
    app.indexing = true;
    app.index_thread = std::thread(build_index, &app);
}

// Load a .model (by basename) from a pac into the renderer, wiring diffuse textures.
static bool load_model(App& app, const std::string& pac_path, const std::string& basename, std::string* err) {
    std::vector<PacEntry> ents;
    if (!unpack_pac(pac_path, ents, err)) return false;
    auto ismdl = [](const std::string& x) { return x == "model" || x == "terrain-model"; };
    const PacEntry* mfile = nullptr;
    // prefer an exact basename match, then any prefix match, then the first model
    for (auto& e : ents) if (ismdl(e.ext) && e.name.substr(0, e.name.find_last_of('.')) == basename) { mfile = &e; break; }
    if (!mfile) for (auto& e : ents) if (ismdl(e.ext) && e.name.find(basename) != std::string::npos) { mfile = &e; break; }
    if (!mfile) for (auto& e : ents) if (ismdl(e.ext)) { mfile = &e; break; }
    if (!mfile) { if (err) *err = "no model in pac"; return false; }

    Model model;
    if (!parse_model(mfile->data, model, err)) return false;

    // material name -> diffuse dds
    std::map<std::string, std::string> mat_diffuse;
    for (auto& e : ents) {
        if (e.ext != "material") continue;
        Material mt;
        if (!parse_material(e.data, mt)) continue;
        std::string base = e.name.substr(0, e.name.size() - 9);   // strip ".material"
        std::string diff;
        for (auto& t : mt.textures) { if (t.semantic == "diffuse") { diff = t.dds; break; } }
        if (diff.empty() && !mt.textures.empty()) diff = mt.textures[0].dds;
        mat_diffuse[base] = diff;
    }
    std::vector<std::pair<std::string, std::string>> mesh_diffuse;
    for (auto& msh : model.meshes) mesh_diffuse.push_back({msh.material, mat_diffuse.count(msh.material) ? mat_diffuse[msh.material] : ""});

    // streaming dir: <raw>\texture_streaming  (raw = game_folder or game_folder\image\x64\raw)
    std::string raw = app.cfg.game_folder;
    if (fs::exists(fs::path(raw) / "image" / "x64" / "raw"))
        raw = (fs::path(raw) / "image" / "x64" / "raw").string();
    std::string streaming = (fs::path(raw) / "texture_streaming").string();

    // texture lookup: pull dds from this pac, resolving NTSI streamed stubs from .ntsp packages
    auto tex = [ents_copy = ents, streaming](const std::string& dds) -> Bytes {
        const PacEntry* found = nullptr;
        for (auto& e : ents_copy) if (e.ext == "dds" && e.name == dds) { found = &e; break; }
        if (!found) for (auto& e : ents_copy) if (e.ext == "dds" && e.name.find(dds) != std::string::npos) { found = &e; break; }
        if (!found) return {};
        if (is_ntsi(found->data)) {
            std::string base = dds.size() > 4 && dds.substr(dds.size() - 4) == ".dds" ? dds.substr(0, dds.size() - 4) : dds;
            Bytes full;
            if (resolve_streamed_dds(found->data, base, streaming, full)) return full;
            return {};   // streamed but unresolved -> untextured rather than garbage
        }
        return found->data;
    };
    app.renderer.set_model(model, tex, mesh_diffuse);
    size_t tv = 0; for (auto& m : model.meshes) tv += m.positions.size();
    app.loaded_label = base_of(pac_path) + " : " + mfile->name;
    app.status = "Loaded " + mfile->name + "  (" + std::to_string(model.meshes.size()) +
                 " meshes, " + std::to_string(tv) + " verts, v" + std::to_string(model.version) +
                 (model.is_terrain ? ", terrain)" : ")");
    return true;
}

static void extract_entry(App& app, const PacEntry& e) {
    const char* dir = tinyfd_selectFolderDialog("Extract to folder", app.cfg.last_export_dir.c_str());
    if (!dir) return;
    app.cfg.last_export_dir = dir; app.cfg.save();
    std::string out = std::string(dir) + "\\" + e.name;
    if (write_file(out, e.data.data(), e.data.size())) app.status = "Extracted " + e.name;
    else app.status = "Extract FAILED";
}

static void export_fbx(App& app, const std::string& pac_path, const std::string& basename) {
    std::string def = app.cfg.last_export_dir + "\\" + basename + ".fbx";
    const char* out = tinyfd_saveFileDialog("Export FBX", def.c_str(), 0, nullptr, nullptr);
    if (!out) return;
    std::vector<PacEntry> ents; std::string err;
    if (!unpack_pac(pac_path, ents, &err)) { app.status = "FBX: " + err; return; }
    if (export_pac_model_to_fbx(ents, basename, out, &err)) app.status = std::string("Exported FBX: ") + out;
    else app.status = "FBX export FAILED: " + err;
    app.cfg.last_export_dir = fs::path(out).parent_path().string(); app.cfg.save();
}

// viewport: requested size (read by the GL pass) + the FBO texture (shown by ImGui)
static float g_vp_w = 700, g_vp_h = 500;
static ImTextureID g_vp_tex = 0;

// ---------------- UI ----------------
static void draw_ui(App& app, int win_w, int win_h) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::Button("Set Game Folder")) {
            const char* d = tinyfd_selectFolderDialog("Select SonicFrontiers folder (or its raw/)",
                                                      app.cfg.game_folder.c_str());
            if (d) { app.cfg.game_folder = d; app.cfg.save(); scan_folder(app); start_index(app); }
        }
        ImGui::TextDisabled("|");
        ImGui::SetNextItemWidth(160);
        if (ImGui::SliderFloat("UI Scale", &app.cfg.ui_scale, 0.75f, 3.0f, "%.2fx")) {}
        if (ImGui::IsItemDeactivatedAfterEdit()) app.cfg.save();
        ImGui::TextDisabled("|");
        if (app.indexing) ImGui::Text("Indexing %d/%d...", app.index_done.load(), app.index_total.load());
        else { std::lock_guard<std::mutex> lk(app.index_mtx); ImGui::Text("%zu files indexed", app.index.size()); }
        ImGui::EndMenuBar();
    }

    ImGui::Columns(2, "cols");
    static bool init_w = false;
    if (!init_w) { ImGui::SetColumnWidth(0, 380 * app.cfg.ui_scale); init_w = true; }

    // ---- left: search + browser ----
    ImGui::BeginChild("left");
    ImGui::TextUnformatted("Search all files:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "e.g. chr_sonic, portalgate, amy", app.search, sizeof(app.search));
    ImGui::Checkbox("Models only", &app.models_only);
    ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
    ImGui::TextUnformatted("Sort:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(110); ImGui::Combo("##sort", &app.sort_mode, "Name\0Type\0");
    std::string q = app.search;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    auto is_model_ext = [](const std::string& e) { return e == "model" || e == "terrain-model"; };

    if (!q.empty()) {
        std::lock_guard<std::mutex> lk(app.index_mtx);
        // collect matches, then filter + sort so models are easy to find
        std::vector<const IndexEntry*> matches;
        for (auto& ie : app.index) {
            if (app.models_only && !is_model_ext(ie.ext)) continue;
            std::string ln = ie.name; std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
            if (ln.find(q) != std::string::npos) matches.push_back(&ie);
        }
        std::sort(matches.begin(), matches.end(), [&](const IndexEntry* a, const IndexEntry* b) {
            if (app.sort_mode == 1 && a->ext != b->ext) return a->ext < b->ext;
            if (a->name != b->name) return a->name < b->name;
            return a->pac < b->pac;
        });
        ImGui::Text("%zu match%s", matches.size(), matches.size() == 1 ? "" : "es");
        ImGui::BeginChild("results");
        int shown = 0;
        for (const IndexEntry* iep : matches) {
            const IndexEntry& ie = *iep;
            if (shown >= 1000) { ImGui::TextDisabled("... %zu more (refine search)", matches.size() - shown); break; }
            shown++;
            std::string label = ie.name + "  [" + base_of(ie.pac) + "]##" + std::to_string((size_t)&ie);
            if (ImGui::Selectable(label.c_str())) {
                if (is_model_ext(ie.ext)) {
                    std::string b = ie.name.substr(0, ie.name.find_last_of('.'));
                    std::string err;
                    if (!load_model(app, ie.pac, b, &err)) app.status = "Load failed: " + err;
                } else {
                    app.open_pac = ie.pac; app.open_entries.clear();
                    unpack_pac(ie.pac, app.open_entries);
                    app.status = "Opened " + base_of(ie.pac);
                }
            }
        }
        if (shown == 0) ImGui::TextDisabled(app.indexing ? "indexing..." : "no matches");
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("tree");
        for (auto& kv : app.categories) {
            if (!ImGui::TreeNode(kv.first.c_str())) continue;
            for (auto& pac : kv.second) {
                std::string nm = base_of(pac);
                bool open = ImGui::TreeNode(nm.c_str());
                if (ImGui::IsItemClicked() && app.open_pac != pac) {
                    app.open_pac = pac; app.open_entries.clear();
                    unpack_pac(pac, app.open_entries);
                }
                if (open) {
                    if (app.open_pac == pac) {
                        // sorted + filtered view of this pac's entries
                        std::vector<const PacEntry*> view;
                        for (auto& e : app.open_entries)
                            if (!app.models_only || is_model_ext(e.ext)) view.push_back(&e);
                        std::sort(view.begin(), view.end(), [&](const PacEntry* a, const PacEntry* b) {
                            if (app.sort_mode == 1 && a->ext != b->ext) return a->ext < b->ext;
                            return a->name < b->name;
                        });
                        for (const PacEntry* ep : view) {
                            const PacEntry& e = *ep;
                            bool ismdl = is_model_ext(e.ext);
                            std::string lbl = (ismdl ? "[M] " : "") + e.name + "##" + pac;
                            if (ImGui::Selectable(lbl.c_str())) {
                                if (ismdl) {
                                    std::string b = e.name.substr(0, e.name.find_last_of('.'));
                                    std::string err;
                                    if (!load_model(app, pac, b, &err)) app.status = "Load failed: " + err;
                                    else app.status = "Loaded " + e.name;
                                } else app.status = "Selected " + e.name;
                            }
                            if (ImGui::BeginPopupContextItem()) {
                                if (ImGui::MenuItem("Extract...")) extract_entry(app, e);
                                if (ismdl && ImGui::MenuItem("Export FBX..."))
                                    export_fbx(app, pac, e.name.substr(0, e.name.find_last_of('.')));
                                ImGui::EndPopup();
                            }
                        }
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::NextColumn();
    // ---- right: viewport ----
    ImGui::BeginChild("viewport_panel");
    ImGui::Text("%s", app.loaded_label.empty() ? "(no model loaded)" : app.loaded_label.c_str());
    ImGui::SameLine();
    static bool tex = true, wire = false;
    if (ImGui::Checkbox("Texture", &tex)) app.renderer.set_show_texture(tex);
    ImGui::SameLine(); if (ImGui::Checkbox("Wire", &wire)) app.renderer.set_show_wire(wire);
    ImGui::SameLine(); if (ImGui::Button("Reset View")) app.renderer.reset_view();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    g_vp_w = avail.x > 2 ? avail.x : 2; g_vp_h = avail.y > 2 ? avail.y : 2;
    if (g_vp_tex) {
        ImGui::Image(g_vp_tex, avail, ImVec2(0, 1), ImVec2(1, 0));   // flip V
        if (ImGui::IsItemHovered()) {
            ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !io.KeyShift)
                app.renderer.orbit(io.MouseDelta.x * 0.01f, io.MouseDelta.y * 0.01f);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && io.KeyShift))
                app.renderer.pan(-io.MouseDelta.x * 0.003f, io.MouseDelta.y * 0.003f);
            if (io.MouseWheel != 0) app.renderer.zoom(io.MouseWheel);
        }
    } else ImGui::Dummy(avail);
    ImGui::EndChild();

    ImGui::Columns(1);
    ImGui::Separator();
    ImGui::TextUnformatted(app.status.c_str());
    ImGui::End();
}

int main(int argc, char** argv) {
    App app;
    app.cfg.load();

    if (!glfwInit()) { fprintf(stderr, "glfw init failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    const char* dump = getenv("SFV_DUMP_PNG");
    if (dump) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(1400, 860, "Sonic Frontiers Extractor & Model Viewer", nullptr, nullptr);
    if (!win) { fprintf(stderr, "window failed\n"); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    if (!gladLoadGL(glfwGetProcAddress)) { fprintf(stderr, "glad failed\n"); return 1; }

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    app.renderer.init();

    if (!app.cfg.game_folder.empty()) { scan_folder(app); start_index(app); }

    // ---- headless PNG dump: SFV_DUMP_PNG (model only) or SFV_DUMP_UI (full app frame) ----
    const char* dump_ui = getenv("SFV_DUMP_UI");
    if (dump || dump_ui) {
        const char* gd = getenv("SFV_GAMEDIR");
        if (gd) { app.cfg.game_folder = gd; scan_folder(app); }
        const char* mp = getenv("SFV_MODEL");
        std::string pac, base, err;
        if (mp) { std::string s = mp; auto bar = s.find('|'); pac = s.substr(0, bar); base = bar == std::string::npos ? "" : s.substr(bar + 1); }
        if (mp) { if (!load_model(app, pac, base, &err)) printf("load failed: %s\n", err.c_str()); }
        if (getenv("SFV_SEARCH")) {
            strncpy(app.search, getenv("SFV_SEARCH"), sizeof(app.search) - 1);
            build_index(&app);   // synchronous so the screenshot shows results
        }
        if (getenv("SFV_MODELS_ONLY")) app.models_only = true;

        if (dump_ui) {
            int W = 1400, H = 860;
            uint32_t fbo, col, dep;
            glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glGenTextures(1, &col); glBindTexture(GL_TEXTURE_2D, col);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col, 0);
            glGenRenderbuffers(1, &dep); glBindRenderbuffer(GL_RENDERBUFFER, dep);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, dep);

            ImGui::GetIO().DisplaySize = ImVec2((float)W, (float)H);
            // two passes: first establishes viewport size, second shows the rendered texture
            for (int pass = 0; pass < 2; pass++) {
                if (g_vp_w > 2 && g_vp_h > 2)
                    g_vp_tex = (ImTextureID)(intptr_t)app.renderer.render_to_texture((int)g_vp_w, (int)g_vp_h);
                ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
                draw_ui(app, W, H);
                ImGui::Render();
            }
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, W, H); glClearColor(0.09f, 0.09f, 0.11f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            std::vector<uint8_t> px(W * H * 4), flip(W * H * 4);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (int y = 0; y < H; y++) memcpy(&flip[y*W*4], &px[(H-1-y)*W*4], W*4);
            stbi_write_png(dump_ui, W, H, 4, flip.data(), W * 4);
            printf("dumped UI %s\n", dump_ui);
        } else {
            app.renderer.capture_png(dump, 900, 900);
            printf("dumped %s\n", dump);
        }
        glfwTerminate();
        return 0;
    }

    // ---- interactive loop ----
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        int w, h; glfwGetFramebufferSize(win, &w, &h);

        // render the model into the offscreen viewport texture (sized from last frame)
        if (g_vp_w > 2 && g_vp_h > 2)
            g_vp_tex = (ImTextureID)(intptr_t)app.renderer.render_to_texture((int)g_vp_w, (int)g_vp_h);

        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        ImGui::GetIO().FontGlobalScale = app.cfg.ui_scale;
        draw_ui(app, w, h);
        ImGui::Render();

        glViewport(0, 0, w, h);
        glClearColor(0.09f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(win); glfwTerminate();
    return 0;
}
