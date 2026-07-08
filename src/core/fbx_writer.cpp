#include "fbx_writer.h"
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

namespace sf {

// ------------- FBX node tree -------------
struct Prop {
    char type;                 // I L D C S / d i (arrays)
    int64_t i = 0; double d = 0; std::string s;
    std::vector<double> da; std::vector<int32_t> ia;
};
struct FNode {
    std::string name;
    std::vector<Prop> props;
    std::vector<FNode> children;
    FNode(const std::string& n) : name(n) {}
    FNode& child(const std::string& n) { children.emplace_back(n); return children.back(); }
};
static Prop PI(int32_t v) { Prop p; p.type = 'I'; p.i = v; return p; }
static Prop PL(int64_t v) { Prop p; p.type = 'L'; p.i = v; return p; }
static Prop PD(double v) { Prop p; p.type = 'D'; p.d = v; return p; }
static Prop PC(bool v) { Prop p; p.type = 'C'; p.i = v ? 1 : 0; return p; }
static Prop PS(const std::string& v) { Prop p; p.type = 'S'; p.s = v; return p; }
static Prop Pd(std::vector<double> v) { Prop p; p.type = 'd'; p.da = std::move(v); return p; }
static Prop Pi(std::vector<int32_t> v) { Prop p; p.type = 'i'; p.ia = std::move(v); return p; }

template <class... A> static FNode& addc(FNode& n, const std::string& name, A... props) {
    FNode& c = n.child(name);
    (c.props.push_back(props), ...);
    return c;
}

static void put_u32(Bytes& o, uint32_t v) { o.insert(o.end(), (uint8_t*)&v, (uint8_t*)&v + 4); }
static void put_bytes(Bytes& o, const void* p, size_t n) { o.insert(o.end(), (const uint8_t*)p, (const uint8_t*)p + n); }

static void enc_prop(Bytes& o, const Prop& p) {
    o.push_back(p.type);
    switch (p.type) {
    case 'I': { int32_t v = (int32_t)p.i; put_bytes(o, &v, 4); break; }
    case 'L': { int64_t v = p.i; put_bytes(o, &v, 8); break; }
    case 'D': { double v = p.d; put_bytes(o, &v, 8); break; }
    case 'C': { int8_t v = (int8_t)p.i; put_bytes(o, &v, 1); break; }
    case 'S': { put_u32(o, (uint32_t)p.s.size()); put_bytes(o, p.s.data(), p.s.size()); break; }
    case 'd': { put_u32(o, (uint32_t)p.da.size()); put_u32(o, 0); put_u32(o, (uint32_t)(p.da.size() * 8)); put_bytes(o, p.da.data(), p.da.size() * 8); break; }
    case 'i': { put_u32(o, (uint32_t)p.ia.size()); put_u32(o, 0); put_u32(o, (uint32_t)(p.ia.size() * 4)); put_bytes(o, p.ia.data(), p.ia.size() * 4); break; }
    }
}

static void write_node(Bytes& o, const FNode& node) {
    size_t start = o.size();
    put_u32(o, 0); // endOffset placeholder
    put_u32(o, (uint32_t)node.props.size());
    // props length placeholder
    size_t plen_pos = o.size();
    put_u32(o, 0);
    o.push_back((uint8_t)node.name.size());
    put_bytes(o, node.name.data(), node.name.size());
    size_t props_start = o.size();
    for (auto& p : node.props) enc_prop(o, p);
    uint32_t props_len = (uint32_t)(o.size() - props_start);
    memcpy(o.data() + plen_pos, &props_len, 4);
    for (auto& c : node.children) write_node(o, c);
    if (!node.children.empty()) o.insert(o.end(), 13, 0);
    uint32_t end = (uint32_t)o.size();
    memcpy(o.data() + start, &end, 4);
}

static Bytes serialize(const std::vector<FNode>& roots) {
    Bytes o;
    const char magic[] = "Kaydara FBX Binary  ";
    put_bytes(o, magic, 20);
    o.push_back(0x00); o.push_back(0x1a); o.push_back(0x00);
    put_u32(o, 7400);
    for (auto& r : roots) write_node(o, r);
    o.insert(o.end(), 13, 0);
    o.insert(o.end(), 120, 0);
    put_u32(o, 7400);
    o.insert(o.end(), 120, 0);
    return o;
}

// ------------- math -------------
static void quat_to_euler(const float q[4], double out[3]) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double sinr = 2 * (w * x + y * z), cosr = 1 - 2 * (x * x + y * y);
    double roll = atan2(sinr, cosr);
    double sinp = 2 * (w * y - z * x);
    double pitch = fabs(sinp) >= 1 ? copysign(3.14159265358979 / 2, sinp) : asin(sinp);
    double siny = 2 * (w * z + x * y), cosy = 1 - 2 * (y * y + z * z);
    double yaw = atan2(siny, cosy);
    const double R = 180.0 / 3.14159265358979;
    out[0] = roll * R; out[1] = pitch * R; out[2] = yaw * R;
}
typedef double M4[4][4];
static void compose(const float t[3], const float q[4], const float s[3], M4 r) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double xx = x*x, yy = y*y, zz = z*z, xy = x*y, xz = x*z, yz = y*z, wx = w*x, wy = w*y, wz = w*z;
    r[0][0]=(1-2*(yy+zz))*s[0]; r[0][1]=2*(xy-wz)*s[1]; r[0][2]=2*(xz+wy)*s[2]; r[0][3]=t[0];
    r[1][0]=2*(xy+wz)*s[0]; r[1][1]=(1-2*(xx+zz))*s[1]; r[1][2]=2*(yz-wx)*s[2]; r[1][3]=t[1];
    r[2][0]=2*(xz-wy)*s[0]; r[2][1]=2*(yz+wx)*s[1]; r[2][2]=(1-2*(xx+yy))*s[2]; r[2][3]=t[2];
    r[3][0]=0; r[3][1]=0; r[3][2]=0; r[3][3]=1;
}
static void matmul(const M4 a, const M4 b, M4 out) {
    for (int i=0;i<4;i++) for (int j=0;j<4;j++){ double s=0; for(int k=0;k<4;k++) s+=a[i][k]*b[k][j]; out[i][j]=s; }
}
static std::vector<double> flat_colmajor(const M4 M) {
    std::vector<double> v(16);
    for (int c=0;c<4;c++) for (int r=0;r<4;r++) v[c*4+r]=M[r][c];
    return v;
}
static void invert_affine(const M4 M, M4 out) {
    double a[3][3]; for(int i=0;i<3;i++)for(int j=0;j<3;j++)a[i][j]=M[i][j];
    double det=a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])-a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])+a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
    if (fabs(det)<1e-12){ for(int i=0;i<4;i++)for(int j=0;j<4;j++)out[i][j]=(i==j)?1:0; return; }
    double id=1.0/det; double inv[3][3];
    inv[0][0]=(a[1][1]*a[2][2]-a[1][2]*a[2][1])*id; inv[0][1]=(a[0][2]*a[2][1]-a[0][1]*a[2][2])*id; inv[0][2]=(a[0][1]*a[1][2]-a[0][2]*a[1][1])*id;
    inv[1][0]=(a[1][2]*a[2][0]-a[1][0]*a[2][2])*id; inv[1][1]=(a[0][0]*a[2][2]-a[0][2]*a[2][0])*id; inv[1][2]=(a[0][2]*a[1][0]-a[0][0]*a[1][2])*id;
    inv[2][0]=(a[1][0]*a[2][1]-a[1][1]*a[2][0])*id; inv[2][1]=(a[0][1]*a[2][0]-a[0][0]*a[2][1])*id; inv[2][2]=(a[0][0]*a[1][1]-a[0][1]*a[1][0])*id;
    double t[3]={M[0][3],M[1][3],M[2][3]};
    for(int i=0;i<3;i++){ for(int j=0;j<3;j++) out[i][j]=inv[i][j]; out[i][3]=-(inv[i][0]*t[0]+inv[i][1]*t[1]+inv[i][2]*t[2]); }
    out[3][0]=0;out[3][1]=0;out[3][2]=0;out[3][3]=1;
}

static int64_t g_id = 1000000;
static int64_t new_id() { return ++g_id; }
static std::string obj_name(const std::string& n, const std::string& cls) { return n + std::string("\x00\x01", 2) + cls; }

bool build_fbx(const Model& model, const std::vector<SkelBone>& bones,
               const std::vector<Material>& materials, Bytes& out) {
    g_id = 1000000;
    std::vector<FNode> root;

    FNode hdr("FBXHeaderExtension");
    addc(hdr, "FBXHeaderVersion", PI(1003));
    addc(hdr, "FBXVersion", PI(7400));
    addc(hdr, "Creator", PS("SonicFrontiersExtractor"));
    root.push_back(hdr);

    FNode gs("GlobalSettings");
    addc(gs, "Version", PI(1000));
    { FNode& p = gs.child("Properties70");
      auto P=[&](const char* nm,const char* t,const char* t2,int v){ addc(p,"P",PS(nm),PS(t),PS(t2),PS(""),PI(v)); };
      P("UpAxis","int","Integer",1); P("UpAxisSign","int","Integer",1);
      P("FrontAxis","int","Integer",2); P("FrontAxisSign","int","Integer",1);
      P("CoordAxis","int","Integer",0); P("CoordAxisSign","int","Integer",1);
      addc(p,"P",PS("UnitScaleFactor"),PS("double"),PS("Number"),PS(""),PD(1.0)); }
    root.push_back(gs);

    FNode docs("Documents");
    addc(docs, "Count", PI(1));
    addc(docs, "Document", PL(new_id()), PS("Scene"), PS("Scene"));
    root.push_back(docs);
    root.push_back(FNode("References"));

    FNode objects("Objects");
    FNode connections("Connections");
    auto oo = [&](int64_t c, int64_t p) { addc(connections, "C", PS("OO"), PL(c), PL(p)); };
    auto op = [&](int64_t c, int64_t p, const std::string& pr) { addc(connections, "C", PS("OP"), PL(c), PL(p), PS(pr)); };
    std::map<std::string, int> defc;

    // bones
    std::vector<int64_t> bone_ids(bones.size());
    std::vector<std::vector<double>> bone_world(bones.size());
    std::unordered_map<std::string, int> name_to_bone;
    for (size_t i = 0; i < bones.size(); i++) name_to_bone[bones[i].name] = (int)i;
    for (size_t i = 0; i < bones.size(); i++) {
        int64_t bid = new_id(); bone_ids[i] = bid;
        FNode& mdl = objects.child("Model");
        mdl.props = {PL(bid), PS(obj_name(bones[i].name, "Model")), PS("LimbNode")};
        addc(mdl, "Version", PI(232));
        FNode& pp = mdl.child("Properties70");
        double eul[3]; quat_to_euler(bones[i].q, eul);
        addc(pp, "P", PS("Lcl Translation"), PS("Lcl Translation"), PS(""), PS("A"), PD(bones[i].t[0]), PD(bones[i].t[1]), PD(bones[i].t[2]));
        addc(pp, "P", PS("Lcl Rotation"), PS("Lcl Rotation"), PS(""), PS("A"), PD(eul[0]), PD(eul[1]), PD(eul[2]));
        addc(pp, "P", PS("Lcl Scaling"), PS("Lcl Scaling"), PS(""), PS("A"), PD(bones[i].s[0]), PD(bones[i].s[1]), PD(bones[i].s[2]));
        defc["Model"]++;
    }
    // world bind matrices
    std::vector<std::vector<std::vector<double>>> locals(bones.size());
    for (size_t i = 0; i < bones.size(); i++) {
        M4 m; compose(bones[i].t, bones[i].q, bones[i].s, m);
        locals[i].assign(4, std::vector<double>(4));
        for (int a=0;a<4;a++)for(int b=0;b<4;b++) locals[i][a][b]=m[a][b];
    }
    for (size_t i = 0; i < bones.size(); i++) {
        M4 M; for(int a=0;a<4;a++)for(int b=0;b<4;b++) M[a][b]=locals[i][a][b];
        int p = bones[i].parent;
        while (p >= 0) {
            M4 pm, res; for(int a=0;a<4;a++)for(int b=0;b<4;b++) pm[a][b]=locals[p][a][b];
            matmul(pm, M, res); memcpy(M, res, sizeof(M4));
            p = bones[p].parent;
        }
        bone_world[i] = flat_colmajor(M);
    }
    for (size_t i = 0; i < bones.size(); i++)
        if (bones[i].parent >= 0) oo(bone_ids[i], bone_ids[bones[i].parent]);
        else oo(bone_ids[i], 0);

    // material name -> texture list
    std::unordered_map<std::string, const Material*> mat_by_name;
    for (auto& mt : materials) { /* materials indexed by external map from caller */ }

    // meshes
    for (size_t mi = 0; mi < model.meshes.size(); mi++) {
        const Mesh& mesh = model.meshes[mi];
        int64_t geo_id = new_id(), mdl_id = new_id();
        FNode& geo = objects.child("Geometry");
        geo.props = {PL(geo_id), PS(obj_name("mesh" + std::to_string(mi), "Geometry")), PS("Mesh")};
        std::vector<double> verts; verts.reserve(mesh.positions.size() * 3);
        for (auto& p : mesh.positions) { verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z); }
        addc(geo, "Vertices", Pd(std::move(verts)));
        std::vector<int32_t> pvi; pvi.reserve(mesh.faces.size() * 3);
        for (auto& f : mesh.faces) { pvi.push_back(f[0]); pvi.push_back(f[1]); pvi.push_back(~(int32_t)f[2]); }
        addc(geo, "PolygonVertexIndex", Pi(std::move(pvi)));

        if (!mesh.normals.empty()) {
            FNode& ne = geo.child("LayerElementNormal"); ne.props={PI(0)};
            addc(ne,"Version",PI(101)); addc(ne,"Name",PS(""));
            addc(ne,"MappingInformationType",PS("ByVertice")); addc(ne,"ReferenceInformationType",PS("Direct"));
            std::vector<double> nv; nv.reserve(mesh.normals.size()*3);
            for (auto& p : mesh.normals){ nv.push_back(p.x); nv.push_back(p.y); nv.push_back(p.z); }
            addc(ne,"Normals",Pd(std::move(nv)));
        }
        if (!mesh.uvs.empty() && !mesh.uvs[0].empty()) {
            FNode& ue = geo.child("LayerElementUV"); ue.props={PI(0)};
            addc(ue,"Version",PI(101)); addc(ue,"Name",PS("UVMap"));
            addc(ue,"MappingInformationType",PS("ByVertice")); addc(ue,"ReferenceInformationType",PS("Direct"));
            std::vector<double> uv; uv.reserve(mesh.uvs[0].size()*2);
            for (auto& p : mesh.uvs[0]){ uv.push_back(p.u); uv.push_back(p.v); }
            addc(ue,"UV",Pd(std::move(uv)));
        }
        if (!mesh.colors.empty()) {
            FNode& ce = geo.child("LayerElementColor"); ce.props={PI(0)};
            addc(ce,"Version",PI(101)); addc(ce,"Name",PS("Col"));
            addc(ce,"MappingInformationType",PS("ByVertice")); addc(ce,"ReferenceInformationType",PS("Direct"));
            std::vector<double> cv; cv.reserve(mesh.colors.size()*4);
            for (auto& c : mesh.colors){ cv.push_back(c.x); cv.push_back(c.y); cv.push_back(c.z); cv.push_back(c.w); }
            addc(ce,"Colors",Pd(std::move(cv)));
        }
        { FNode& me = geo.child("LayerElementMaterial"); me.props={PI(0)};
          addc(me,"Version",PI(101)); addc(me,"Name",PS(""));
          addc(me,"MappingInformationType",PS("AllSame")); addc(me,"ReferenceInformationType",PS("IndexToDirect"));
          addc(me,"Materials",Pi({0})); }
        { FNode& layer = geo.child("Layer"); layer.props={PI(0)}; addc(layer,"Version",PI(100));
          for (const char* t : {"LayerElementNormal","LayerElementUV","LayerElementColor","LayerElementMaterial"}) {
            FNode& le = layer.child("LayerElement"); addc(le,"Type",PS(t)); addc(le,"TypedIndex",PI(0)); } }
        defc["Geometry"]++;

        FNode& mdl = objects.child("Model");
        mdl.props = {PL(mdl_id), PS(obj_name(mesh.material.empty()?("mesh"+std::to_string(mi)):mesh.material, "Model")), PS("Mesh")};
        addc(mdl,"Version",PI(232));
        { FNode& pp=mdl.child("Properties70"); addc(pp,"P",PS("Lcl Scaling"),PS("Lcl Scaling"),PS(""),PS("A"),PD(1),PD(1),PD(1)); }
        defc["Model"]++;
        oo(mdl_id, 0); oo(geo_id, mdl_id);

        int64_t mat_id = new_id();
        FNode& mat = objects.child("Material");
        mat.props={PL(mat_id), PS(obj_name(mesh.material.empty()?("mat"+std::to_string(mi)):mesh.material,"Material")), PS("")};
        addc(mat,"Version",PI(102)); addc(mat,"ShadingModel",PS("phong"));
        { FNode& mp=mat.child("Properties70"); addc(mp,"P",PS("DiffuseColor"),PS("Color"),PS(""),PS("A"),PD(0.8),PD(0.8),PD(0.8)); }
        defc["Material"]++; oo(mat_id, mdl_id);

        // skin
        if (!mesh.weights.empty() && !bones.empty()) {
            int64_t skin_id = new_id();
            FNode& sk = objects.child("Deformer");
            sk.props={PL(skin_id), PS(obj_name("","Deformer")), PS("Skin")};
            addc(sk,"Version",PI(101)); addc(sk,"Link_DeformAcuracy",PD(50.0));
            defc["Deformer"]++; oo(skin_id, geo_id);

            std::map<int, std::pair<std::vector<int32_t>, std::vector<double>>> bv;
            for (size_t vi = 0; vi < mesh.weights.size(); vi++) {
                for (int k = 0; k < 4; k++) {
                    float w = mesh.weights[vi][k]; if (w <= 0) continue;
                    uint32_t gni = mesh.bone_indices[vi][k];
                    if (gni >= model.node_names.size()) continue;
                    auto it = name_to_bone.find(model.node_names[gni]);
                    if (it == name_to_bone.end()) continue;
                    bv[it->second].first.push_back((int32_t)vi);
                    bv[it->second].second.push_back(w);
                }
            }
            for (auto& kv : bv) {
                int bi = kv.first;
                int64_t clu = new_id();
                FNode& cl = objects.child("Deformer");
                cl.props={PL(clu), PS(obj_name("","SubDeformer")), PS("Cluster")};
                addc(cl,"Version",PI(100)); addc(cl,"UserData",PS(""),PS(""));
                addc(cl,"Indexes",Pi(kv.second.first));
                addc(cl,"Weights",Pd(kv.second.second));
                M4 W; for(int a=0;a<4;a++)for(int b=0;b<4;b++) W[a][b]=(a==b)?1:0;
                // reconstruct from flat colmajor
                for (int c=0;c<4;c++) for (int r=0;r<4;r++) W[r][c]=bone_world[bi][c*4+r];
                M4 iW; invert_affine(W, iW);
                addc(cl,"Transform",Pd(flat_colmajor(iW)));
                addc(cl,"TransformLink",Pd(bone_world[bi]));
                defc["Deformer"]++;
                oo(clu, skin_id); oo(bone_ids[bi], clu);
            }
        }
    }

    FNode defs("Definitions");
    addc(defs,"Version",PI(100));
    int total=1; for (auto& kv:defc) total+=kv.second;
    addc(defs,"Count",PI(total));
    for (auto& kv:defc){ if(!kv.second) continue; FNode& ot=defs.child("ObjectType"); ot.props={PS(kv.first)}; addc(ot,"Count",PI(kv.second)); }
    root.push_back(defs);
    root.push_back(objects);
    root.push_back(connections);

    out = serialize(root);
    return true;
}

bool export_pac_model_to_fbx(const std::vector<PacEntry>& entries, const std::string& basename,
                             const std::string& out_path, std::string* err) {
    const PacEntry* mfile = nullptr;
    for (auto& e : entries) if (e.ext == "model" && (basename.empty() || e.name.find(basename) != std::string::npos)) { mfile = &e; break; }
    if (!mfile) { if (err) *err = "no matching .model"; return false; }
    Model model;
    if (!parse_model(mfile->data, model, err)) return false;

    std::vector<SkelBone> bones;
    std::string skl = mfile->name.substr(0, mfile->name.size() - 6) + ".skl.pxd";
    for (auto& e : entries) if (e.name == skl) { parse_skeleton(e.data, bones); break; }

    std::vector<Material> mats;   // (texture wiring kept minimal for now)
    Bytes fbx;
    if (!build_fbx(model, bones, mats, fbx)) { if (err) *err = "build_fbx failed"; return false; }
    if (!write_file(out_path, fbx.data(), fbx.size())) { if (err) *err = "write failed"; return false; }
    return true;
}

} // namespace sf
