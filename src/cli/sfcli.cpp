// CLI test harness for the Sonic Frontiers core parsers.
#include "pac.h"
#include "model.h"
#include "material.h"
#include "skeleton.h"
#include "fbx_writer.h"
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>

using namespace sf;

static const PacEntry* find_ext(const std::vector<PacEntry>& e, const std::string& ext, const std::string& contains = "") {
    for (auto& x : e) if (x.ext == ext && (contains.empty() || x.name.find(contains) != std::string::npos)) return &x;
    return nullptr;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: sfcli <listpac|model|mat|skel|fbx> <file.pac> [basename] [out.fbx]\n");
        return 1;
    }
    std::string cmd = argv[1];

    if (cmd == "batch") {   // sweep a folder: parse every model/mat/skel in every pac
        namespace fs = std::filesystem;
        int pac_ok = 0, pac_fail = 0, m_ok = 0, m_fail = 0, mat_ok = 0, mat_fail = 0, s_ok = 0, s_fail = 0;
        for (auto& e : fs::recursive_directory_iterator(argv[2], fs::directory_options::skip_permission_denied)) {
            if (!e.is_regular_file() || e.path().extension() != ".pac") continue;
            std::vector<PacEntry> es; std::string er;
            if (!unpack_pac(e.path().string(), es, &er)) { pac_fail++; printf("PACFAIL %s\n", e.path().string().c_str()); continue; }
            pac_ok++;
            for (auto& x : es) {
                std::string er2;
                if (x.ext == "model" || x.ext == "terrain-model") { Model md; if (parse_model(x.data, md, &er2)) m_ok++; else { m_fail++; printf("MODELFAIL %s %s\n", x.name.c_str(), er2.c_str()); } }
                else if (x.ext == "material") { Material mm; if (parse_material(x.data, mm, &er2)) mat_ok++; else mat_fail++; }
                else if (x.ext == "skl.pxd") { std::vector<SkelBone> b; if (parse_skeleton(x.data, b, &er2)) s_ok++; else s_fail++; }
            }
            if ((pac_ok + pac_fail) % 100 == 0) printf("...%d pacs, models %d ok/%d fail\n", pac_ok + pac_fail, m_ok, m_fail);
        }
        printf("\nBATCH: pacs %d ok/%d fail | models %d ok/%d fail | mats %d ok/%d fail | skels %d ok/%d fail\n",
               pac_ok, pac_fail, m_ok, m_fail, mat_ok, mat_fail, s_ok, s_fail);
        return 0;
    }

    std::vector<PacEntry> ents;
    std::string err;
    if (!unpack_pac(argv[2], ents, &err)) { printf("PAC FAIL: %s\n", err.c_str()); return 1; }

    if (cmd == "listpac") {
        std::map<std::string, int> hist;
        size_t total = 0;
        for (auto& e : ents) { hist[e.ext]++; total += e.data.size(); }
        printf("%zu files, %zu bytes\n", ents.size(), total);
        for (auto& kv : hist) printf("  %-12s %d\n", kv.first.c_str(), kv.second);
        return 0;
    }
    if (cmd == "model") {
        const PacEntry* m = find_ext(ents, "model", argc > 3 ? argv[3] : "");
        if (!m) { printf("no model\n"); return 1; }
        Model mdl;
        if (!parse_model(m->data, mdl, &err)) { printf("MODEL FAIL: %s\n", err.c_str()); return 1; }
        size_t tv = 0, tf = 0;
        for (auto& msh : mdl.meshes) { tv += msh.positions.size(); tf += msh.faces.size(); }
        printf("%s  version=%d nodes=%zu meshes=%zu verts=%zu faces=%zu\n",
               m->name.c_str(), mdl.version, mdl.node_names.size(), mdl.meshes.size(), tv, tf);
        for (size_t i = 0; i < mdl.meshes.size() && i < 20; i++)
            printf("  mesh%zu mat=%-22s verts=%zu faces=%zu skin=%c\n", i,
                   mdl.meshes[i].material.c_str(), mdl.meshes[i].positions.size(),
                   mdl.meshes[i].faces.size(), mdl.meshes[i].weights.empty() ? 'n' : 'Y');
        return 0;
    }
    if (cmd == "mat") {
        for (auto& e : ents) {
            if (e.ext != "material") continue;
            Material mat;
            if (!parse_material(e.data, mat, &err)) { printf("%s FAIL %s\n", e.name.c_str(), err.c_str()); continue; }
            printf("%s shader=%s tex=%zu\n", e.name.c_str(), mat.shader.c_str(), mat.textures.size());
            for (auto& t : mat.textures) printf("    %-10s %-10s %s\n", t.slot.c_str(), t.semantic.c_str(), t.dds.c_str());
        }
        return 0;
    }
    if (cmd == "skel") {
        const PacEntry* s = find_ext(ents, "skl.pxd");
        if (!s) { printf("no skeleton\n"); return 1; }
        std::vector<SkelBone> bones;
        if (!parse_skeleton(s->data, bones, &err)) { printf("SKEL FAIL: %s\n", err.c_str()); return 1; }
        printf("%s  %zu bones\n", s->name.c_str(), bones.size());
        for (size_t i = 0; i < bones.size() && i < 8; i++)
            printf("  %-18s parent=%d t=(%.3f %.3f %.3f)\n", bones[i].name.c_str(), bones[i].parent,
                   bones[i].t[0], bones[i].t[1], bones[i].t[2]);
        return 0;
    }
    if (cmd == "fbx") {
        std::string basename = argc > 3 ? argv[3] : "";
        std::string outp = argc > 4 ? argv[4] : "out.fbx";
        if (!export_pac_model_to_fbx(ents, basename, outp, &err)) { printf("FBX FAIL: %s\n", err.c_str()); return 1; }
        printf("wrote %s\n", outp.c_str());
        return 0;
    }
    printf("unknown cmd\n");
    return 1;
}
