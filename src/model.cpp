#include "model.hpp"
#include "materials.hpp"
#include "weapon_supplements.hpp"
#include <regex>
namespace codm {
M4 transform_matrix(const J &t) {
    auto p = vec3(t.at("m_LocalPosition")), s = vec3(t.at("m_LocalScale"));
    auto &r = t.at("m_LocalRotation");
    Q q(r.at("w").get<double>(), r.at("x").get<double>(), r.at("y").get<double>(),
        r.at("z").get<double>());
    require(std::isfinite(glm::length(q)) && std::abs(glm::length(q) - 1) < .01,
            "Invalid transform quaternion");
    for (int i = 0; i < 3; i++)
        require(std::isfinite(p[i]) && std::isfinite(s[i]), "Nonfinite transform");
    return glm::translate(M4(1), p) * glm::mat4_cast(glm::normalize(q)) * glm::scale(M4(1), s);
}
void decompose(const M4 &m, DV3 &p, Q &q, DV3 &s) {
    p = DV3(m[3]);
    glm::dmat3 r(m);
    for (int i = 0; i < 3; i++) {
        s[i] = glm::length(r[i]);
        require(std::isfinite(s[i]) && s[i] > 1e-10, "Singular transform");
        r[i] /= s[i];
    }
    if (glm::determinant(r) < 0) {
        s[0] *= -1;
        r[0] *= -1;
    }
    auto test = glm::transpose(r) * r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            require(std::abs(test[i][j] - (i == j ? 1. : 0.)) < 1e-4, "Sheared bone transform");
    q = glm::normalize(glm::quat_cast(r));
    if (q.w < 0)
        q = -q;
}
M4 native_basis() {
    M4 c(1);
    c[0] = glm::dvec4(-1, 0, 0, 0);
    c[1] = glm::dvec4(0, 0, 1, 0);
    c[2] = glm::dvec4(0, -1, 0, 0);
    return c;
}
double matrix_error(const M4 &a, const M4 &b) {
    double e = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            e = std::max(e, std::abs(a[i][j] - b[i][j]));
    return e;
}
J matrix_json(const M4 &m) {
    J j = J::array();
    for (int i = 0; i < 4; i++)
        j.push_back({m[0][i], m[1][i], m[2][i], m[3][i]});
    return j;
}
M4 json_matrix(const J &j) {
    M4 m;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++)
            m[k][i] = j.at(i).at(k);
    return m;
}
static std::span<const uint8_t> binary(const J &j) {
    require(j.is_binary(), "Expected binary field");
    return j.get_binary();
}
static std::vector<uint32_t> unpack_ints(const J &p) {
    auto n = p.at("m_NumItems").get<uint32_t>(), bits = p.at("m_BitSize").get<uint32_t>();
    auto data = binary(p.at("m_Data"));
    require(n <= 30000000 && bits <= 32 && uint64_t(n) * bits <= uint64_t(data.size()) * 8,
            "Packed vector bounds invalid");
    std::vector<uint32_t> out(n);
    uint64_t bit = 0;
    for (auto &v : out) {
        v = 0;
        unsigned got = 0;
        while (got < bits) {
            unsigned at = unsigned(bit % 8), take = std::min(bits - got, 8 - at);
            v |= uint32_t((data[bit / 8] >> at) & ((1u << take) - 1)) << got;
            bit += take;
            got += take;
        }
    }
    return out;
}
static std::vector<double> unpack_floats(const J &p) {
    auto ints = unpack_ints(p);
    unsigned bits = p.at("m_BitSize");
    double range = p.at("m_Range"), start = p.at("m_Start");
    require(std::isfinite(range) && std::isfinite(start), "Nonfinite packed range");
    double denom = bits ? double((uint64_t(1) << bits) - 1) : 1;
    std::vector<double> out;
    out.reserve(ints.size());
    for (auto v : ints)
        out.push_back(v * range / denom + start);
    return out;
}
static double half(uint16_t v) {
    int sign = (v & 0x8000) ? -1 : 1, ex = (v >> 10) & 31;
    auto frac = v & 1023;
    require(ex != 31, "Nonfinite vertex half");
    return sign * std::ldexp(ex ? 1. + frac / 1024. : frac / 1024., ex ? ex - 15 : -14);
}
RawMesh decode_mesh(Source &source, Object &o) {
    require(o.cid == 43, "Expected Mesh: " + o.id());
    const auto &t = source.tree(o);
    RawMesh m;
    m.id = o.id();
    m.name = t.at("m_Name");
    for (auto &b : t.at("m_BindPose")) {
        M4 bm;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                bm[j][i] = b.at("e" + std::to_string(i) + std::to_string(j));
        m.bind.push_back(bm);
    }
    const auto &vd = t.at("m_VertexData");
    size_t n = vd.at("m_VertexCount");
    require(n <= 5000000, "Too many vertices");
    const auto &channels = vd.at("m_Channels");
    auto bytes = binary(vd.at("m_DataSize"));
    if (n && !bytes.empty()) {
        std::array<size_t, 16> stride{}, offset{};
        size_t streams = 0;
        auto width = [](int fmt) -> size_t {
            if (fmt == 0)
                return 4;
            if (fmt == 1)
                return 2;
            if (fmt == 2 || fmt == 3 || fmt == 5)
                return 1;
            if (fmt == 4)
                return 4;
            throw std::runtime_error("Unverified vertex format " + std::to_string(fmt));
        };
        for (auto &ch : channels) {
            size_t s = ch.at("stream");
            int dim = ch.at("dimension").get<int>() & 15;
            require(s < 16 && dim <= 4, "Invalid vertex channel");
            streams = std::max(streams, s + 1);
            if (dim)
                stride[s] += dim * width(ch.at("format"));
        }
        size_t at = 0, extent = 0;
        for (size_t s = 0; s < streams; s++) {
            offset[s] = at;
            extent = std::max(extent, at + stride[s] * n);
            at = (at + stride[s] * n + 15) & ~size_t(15);
        }
        require(extent <= bytes.size() && bytes.size() <= extent + 15,
                "Vertex stream extent mismatch");
        for (size_t ci = 0; ci < channels.size(); ci++) {
            const auto &ch = channels[ci];
            int dim = ch.at("dimension").get<int>() & 15;
            if (!dim)
                continue;
            int fmt = ch.at("format");
            size_t s = ch.at("stream"), co = ch.at("offset"), w = width(fmt);
            require(co + dim * w <= stride[s], "Vertex channel exceeds stride");
            if (ci == 0)
                m.positions.resize(n);
            else if (ci == 1)
                m.normals.resize(n);
            else if (ci == 3)
                m.uv0.resize(n);
            else if (ci == 4)
                m.uv1.resize(n);
            else
                continue;
            require(dim >= (ci <= 1 ? 3 : 2), "Vertex channel lacks components");
            for (size_t vi = 0; vi < n; vi++) {
                Reader r(bytes, o.file->big);
                r.seek(offset[s] + vi * stride[s] + co);
                double vals[4]{};
                for (int k = 0; k < dim; k++) {
                    if (fmt == 0)
                        vals[k] = r.get<float>();
                    else if (fmt == 1)
                        vals[k] = half(r.get<uint16_t>());
                    else if (fmt == 2 || fmt == 3)
                        vals[k] = r.get<uint8_t>();
                    else if (fmt == 4)
                        vals[k] = r.get<uint32_t>();
                    else
                        vals[k] = r.get<int8_t>();
                    require(std::isfinite(vals[k]), "Nonfinite mesh channel");
                }
                if (ci == 0)
                    m.positions[vi] = V3(vals[0], vals[1], vals[2]);
                if (ci == 1)
                    m.normals[vi] = V3(vals[0], vals[1], vals[2]);
                if (ci == 3)
                    m.uv0[vi] = V2(vals[0], vals[1]);
                if (ci == 4)
                    m.uv1[vi] = V2(vals[0], vals[1]);
            }
        }
    }
    std::vector<uint32_t> indices;
    auto ib = binary(t.at("m_IndexBuffer"));
    require(ib.size() % 2 == 0, "Odd index data length");
    Reader ir(ib, o.file->big);
    while (ir.pos < ib.size())
        indices.push_back(ir.get<uint16_t>());
    for (auto &skin : t.at("m_Skin")) {
        V4 w;
        glm::uvec4 j;
        for (int k = 0; k < 4; k++) {
            w[k] = skin.at("weight[" + std::to_string(k) + "]");
            j[k] = skin.at("boneIndex[" + std::to_string(k) + "]");
        }
        m.weights.push_back(w);
        m.joints.push_back(j);
    }
    const auto &c = t.at("m_CompressedMesh");
    const auto &packedUV = c.at("m_UV");
    if (packedUV.at("m_NumItems").get<unsigned>() &&
        packedUV.at("m_BitSize").get<unsigned>() > 0) {
        auto bits = packedUV.at("m_BitSize").get<unsigned>();
        m.uvPrecision = std::max(1e-7, packedUV.at("m_Range").get<double>() /
                                          (std::pow(2., bits) - 1.) * .51);
    }
    if (c.at("m_Vertices").at("m_NumItems").get<unsigned>()) {
        auto p = unpack_floats(c.at("m_Vertices"));
        require(p.size() % 3 == 0, "Packed position count mismatch");
        n = p.size() / 3;
        m.positions.resize(n);
        for (size_t i = 0; i < n; i++)
            m.positions[i] = V3(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]);
    }
    require(!m.positions.empty(), "Mesh contains no decoded vertices: " + m.name);
    n = m.positions.size();
    if (c.at("m_UV").at("m_NumItems").get<unsigned>()) {
        auto uv = unpack_floats(c.at("m_UV"));
        uint32_t info = c.value("m_UVInfo", 0u);
        size_t at = 0;
        auto layer = [&](std::vector<V2> &dst, int dim) {
            require(dim >= 2 && at + n * dim <= uv.size(), "Packed UV bounds");
            dst.resize(n);
            for (size_t i = 0; i < n; i++)
                dst[i] = V2(uv[at + i * dim], uv[at + i * dim + 1]);
            at += n * dim;
        };
        if (info) {
            for (int k = 0; k < 8; k++) {
                int bits = (info >> (4 * k)) & 15;
                if (bits & 4) {
                    int dim = 1 + (bits & 3);
                    if (k == 0)
                        layer(m.uv0, dim);
                    else if (k == 1)
                        layer(m.uv1, dim);
                    else {
                        require(at + n * dim <= uv.size(), "Packed secondary UV bounds");
                        at += n * dim;
                    }
                }
            }
        } else {
            layer(m.uv0, 2);
            if (uv.size() >= 4 * n)
                layer(m.uv1, 2);
        }
    }
    if (c.at("m_Normals").at("m_NumItems").get<unsigned>()) {
        auto p = unpack_floats(c.at("m_Normals"));
        auto signs = unpack_ints(c.at("m_NormalSigns"));
        require(p.size() == n * 2 && signs.size() == n, "Packed normal count mismatch");
        m.normals.resize(n);
        for (size_t i = 0; i < n; i++) {
            double x = p[2 * i], y = p[2 * i + 1], z2 = 1 - x * x - y * y;
            DV3 v(x, y, std::sqrt(std::max(0., z2)));
            if (z2 < 0)
                v = glm::normalize(v);
            if (signs[i] == 0)
                v.z = -v.z;
            m.normals[i] = v;
        }
    }
    if (c.at("m_Weights").at("m_NumItems").get<unsigned>()) {
        auto ws = unpack_ints(c.at("m_Weights")), js = unpack_ints(c.at("m_BoneIndices"));
        m.weights.assign(n, V4(0));
        m.joints.assign(n, glm::uvec4(0));
        size_t vi = 0, ji = 0;
        int slot = 0, sum = 0;
        for (auto w : ws) {
            require(vi < n && ji < js.size() && w <= 31 && sum + int(w) <= 31,
                    "Packed skin weights outside bounds");
            m.weights[vi][slot] = float(w) / 31;
            m.joints[vi][slot] = js[ji++];
            sum += int(w);
            slot++;
            if (sum == 31) {
                vi++;
                slot = 0;
                sum = 0;
            } else if (slot == 3) {
                require(ji < js.size(), "Fourth bone index missing");
                m.weights[vi][3] = float(31 - sum) / 31;
                m.joints[vi][3] = js[ji++];
                vi++;
                slot = 0;
                sum = 0;
            }
        }
        require(vi == n && slot == 0 && ji == js.size(), "Incomplete packed skin stream");
    }
    if (c.at("m_Triangles").at("m_NumItems").get<unsigned>())
        indices = unpack_ints(c.at("m_Triangles"));
    for (auto &sub : t.at("m_SubMeshes")) {
        size_t first = sub.at("firstByte").get<size_t>() / 2, count = sub.at("indexCount");
        require(sub.at("firstByte").get<size_t>() % 2 == 0 && first <= indices.size() &&
                    count <= indices.size() - first,
                "Submesh index bounds");
        int topology = sub.at("topology");
        std::vector<uint32_t> f;
        if (topology == 0) {
            require(count % 3 == 0, "Nontriangular index count");
            f.assign(indices.begin() + first, indices.begin() + first + count);
        } else if (topology == 1) {
            for (size_t i = 2; i < count; i++) {
                uint32_t a = indices[first + i - 2], b = indices[first + i - 1],
                         d = indices[first + i];
                if (a == b || a == d || b == d)
                    continue;
                if (i & 1)
                    std::swap(a, b);
                f.insert(f.end(), {a, b, d});
            }
        } else if (topology == 2) {
            require(count % 4 == 0, "Invalid quad count");
            for (size_t i = 0; i < count; i += 4) {
                auto v = &indices[first + i];
                f.insert(f.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
            }
        } else
            throw std::runtime_error("Unsupported primitive topology " + std::to_string(topology));
        for (auto i : f)
            require(i < n, "Triangle vertex outside mesh");
        m.faces.push_back(std::move(f));
    }
    require(m.weights.empty() || (m.weights.size() == n && m.joints.size() == n),
            "Inconsistent skin arrays");
    for (auto w : m.weights) {
        float sum = 0;
        for (int k = 0; k < 4; k++) {
            require(std::isfinite(w[k]) && w[k] >= 0, "Invalid skin weight");
            sum += w[k];
        }
        require(std::abs(sum - 1) < .002, "Skin weights do not sum to one");
    }
    return m;
}
Object *owner_transform(Source &s, Object &o) {
    Reader r(o.raw(), o.file->big);
    J p = {{"m_FileID", r.get<int32_t>()}, {"m_PathID", r.get<int64_t>()}};
    auto *go = s.ref(o, p);
    require(go && go->cid == 1, "Renderer owner is not GameObject");
    for (auto &comp : s.tree(*go).at("m_Component")) {
        auto *c = s.ref(*go, comp.contains("component") ? comp.at("component") : comp);
        if (c && c->cid == 4)
            return c;
    }
    throw std::runtime_error("Owner has no Transform");
}
Renderer renderer_info(Source &s, Object &o) {
    Renderer out;
    out.object = &o;
    out.transform = owner_transform(s, o);
    if (o.cid == 23) {
        auto *go = s.ref(*out.transform, s.tree(*out.transform).at("m_GameObject"));
        for (auto &c : s.tree(*go).at("m_Component")) {
            auto *comp = s.ref(*go, c.contains("component") ? c.at("component") : c);
            if (comp && comp->cid == 33) {
                out.mesh = s.ref(*comp, s.tree(*comp).at("m_Mesh"));
                break;
            }
        }
        require(out.mesh && out.mesh->cid == 43, "Static renderer has no valid MeshFilter");
        return out;
    }
    require(o.cid == 137, "Expected skin or static renderer");
    const auto &schema = s.types.at(137);
    auto begin = std::find_if(schema.children.begin(), schema.children.end(),
                              [](auto &n) { return n.name == "m_Mesh"; });
    require(begin != schema.children.end(), "Missing skin tail schema");
    int matches = 0;
    for (size_t off = 16; off < std::min(size_t(1024), o.size > 16 ? o.size - 16 : 0); off += 4) {
        try {
            Reader r(o.raw(), o.file->big);
            r.seek(off);
            int32_t fid = r.get<int32_t>();
            int64_t pid = r.get<int64_t>();
            if (!pid || fid < 0 || fid > int(o.file->externals.size()))
                continue;
            r.seek(off);
            J t;
            for (auto it = begin; it != schema.children.end(); it++)
                t[it->name] = parse_tree(r, *it);
            if (r.pos != o.size)
                continue;
            auto *m = s.ref(o, t.at("m_Mesh"));
            if (!m || m->cid != 43)
                continue;
            std::vector<Object *> bones;
            for (auto &p : t.at("m_Bones")) {
                auto *b = s.ref(o, p);
                require(b && b->cid == 4, "Invalid skin bone");
                bones.push_back(b);
            }
            if (bones.empty() || s.tree(*m).at("m_BindPose").size() != bones.size())
                continue;
            auto *root = s.ref(o, t.at("m_RootBone"));
            require(root && root->cid == 4, "Invalid root bone");
            out.mesh = m;
            out.bones = std::move(bones);
            out.tail = std::move(t);
            out.tailOffset = off;
            matches++;
        } catch (const std::exception &) {
        }
    }
    require(matches == 1,
            "Expected one validated skin tail in " + o.id() + ", found " + std::to_string(matches));
    return out;
}
M4 Hierarchy::world(Object *o, std::set<std::string> stack) {
    if (!o)
        return M4(1);
    auto id = o->id();
    require(!stack.contains(id), "Cyclic transform hierarchy at " + id);
    if (worlds.contains(id))
        return worlds.at(id);
    stack.insert(id);
    auto &t = source.tree(*o);
    auto *p = source.ref(*o, t.at("m_Father"));
    objects[id] = o;
    parents[id] = p ? p->id() : "";
    auto w = world(p, std::move(stack)) * transform_matrix(t);
    worlds[id] = w;
    return w;
}
Object &Hierarchy::root(Object &o) {
    world(&o);
    auto id = o.id();
    while (!parents.at(id).empty())
        id = parents.at(id);
    return *objects.at(id);
}
std::string Hierarchy::name(Object &o) {
    auto *g = source.ref(o, source.tree(o).at("m_GameObject"));
    require(g && g->cid == 1, "Transform owner invalid");
    return source.tree(*g).at("m_Name");
}
J Hierarchy::sockets(Object &root, const std::set<std::string> &requested) {
    J result = J::object();
    std::set<std::string> seen;
    std::function<void(Object &)> visit = [&](Object &o) {
        require(seen.insert(o.id()).second, "Repeated transform in socket hierarchy");
        auto n = name(o);
        if (requested.contains(n)) {
            require(!result.contains(n), "Ambiguous socket: " + n);
            result[n] = {{"id", o.id()}, {"matrix", matrix_json(world(&o))}};
        }
        for (auto &p : source.tree(o).at("m_Children")) {
            auto *c = source.ref(o, p);
            require(c && c->cid == 4, "Invalid transform child");
            visit(*c);
        }
    };
    visit(root);
    return result;
}
Model prepare_geometry(Source &source, const J &entry, const J &parts, JobContext *job) {
    Model model;
    model.name = entry.at("name");
    Hierarchy h(source);
    std::vector<Renderer> renderers;
    std::set<std::string> rootIds;
    auto refs = entry.value("renderers", J::array({J{{"id", entry.at("id")}}}));
    J omitted = J::array();
    double bindError = 0;
    for (auto &ref : refs) {
        if (job)
            job->check();
        auto r = renderer_info(source, source.object(ref.at("id")));
        // Named collision helpers with no assigned finish are omitted from the
        // visible model. The entry can explicitly request their geometry.
        if (r.object->cid == 23 && !entry.value("includeCollisionHelpers", false)) {
            auto n = lower(h.name(*r.transform));
            if (n.find("collider") != n.npos) {
                Reader reader(r.object->raw(), r.object->file->big);
                reader.seek(40);
                int count = reader.count(12, 128);
                bool placeholderOnly = true;
                for (int i = 0; i < count; i++) {
                    J ptr = {{"m_FileID", reader.get<int32_t>()},
                             {"m_PathID", reader.get<int64_t>()}};
                    auto *material = source.ref(*r.object, ptr);
                    if (material) {
                        auto name = lower(source.name(*material));
                        placeholderOnly &=
                            name.starts_with("empty") && name.find("_donotmodify") != name.npos;
                    }
                }
                if (placeholderOnly) {
                    omitted.push_back({{"renderer", r.object->id()},
                                       {"name", h.name(*r.transform)},
                                       {"reason", "Named collision helper with empty or "
                                                  "placeholder-only materials; "
                                                  "excluded from visible model"}});
                    continue;
                }
            }
        }
        rootIds.insert(h.root(*r.transform).id());
        h.world(r.transform);
        for (auto *b : r.bones)
            h.world(b);
        renderers.push_back(r);
    }
    require(rootIds.size() == 1, "Selected renderers do not share one hierarchy root");
    std::vector<std::string> ordered;
    std::unordered_map<std::string, int> boneIndex;
    std::function<void(std::string)> add = [&](std::string id) {
        if (boneIndex.contains(id))
            return;
        auto parent = h.parents.at(id);
        if (!parent.empty())
            add(parent);
        boneIndex[id] = int(ordered.size());
        ordered.push_back(id);
    };
    // Retain authored socket drivers generically, including removed magazines.
    auto &root = *h.objects.at(*rootIds.begin());
    // An Animator's Avatar owns animation-only transforms as well as skin bones.
    // Resolve full paths within this exact prefab; never borrow a same-named rig.
    std::map<std::string, uint32_t> avatarPaths;
    J avatarEvidence = J::array(), resolvedPaths = J::object();
    auto *rootGo = source.ref(root, source.tree(root).at("m_GameObject"));
    for (auto &component : source.tree(*rootGo).at("m_Component")) {
        auto *a = source.ref(*rootGo, component.contains("component") ? component.at("component")
                                                                      : component);
        if (!a || a->cid != 95)
            continue;
        if (!a->file->version.starts_with("5.6.4p4") || a->size != 104)
            continue;
        Reader reader(a->raw(), a->file->big);
        auto pointer = [&]() {
            return J{{"m_FileID", reader.get<int32_t>()}, {"m_PathID", reader.get<int64_t>()}};
        };
        require(source.ref(*a, pointer()) == rootGo, "Animator owner mismatch");
        require(reader.get<uint8_t>() <= 1, "Invalid Animator enabled flag");
        reader.align();
        auto *avatar = source.ref(*a, pointer());
        if (!avatar)
            continue;
        require(avatar->cid == 90, "Animator Avatar has wrong type");
        for (auto &row : source.tree(*avatar).at("m_TOS")) {
            auto hash = row.at("first").get<uint32_t>();
            auto path = row.at("second").get<std::string>();
            require(crc32(path) == hash, "Avatar path hash mismatch");
            avatarPaths[path] = hash;
            resolvedPaths[std::to_string(hash)] = path;
        }
        avatarEvidence.push_back({{"animator", a->id()},
                                  {"avatar", avatar->id()},
                                  {"avatarSHA256", sha256(avatar->raw())},
                                  {"root", root.id()}});
    }
    for (auto &r : renderers) {
        for (auto *b : r.bones)
            add(b->id());
        if (r.bones.empty())
            add(r.transform->id());
    }
    std::set<std::string> visitedTransforms;
    std::set<std::string> retainedAvatarPaths;
    std::function<void(Object &, std::string)> retain = [&](Object &t, std::string path) {
        require(visitedTransforms.insert(t.id()).second && visitedTransforms.size() < 100000,
                "Cyclic or excessive attachment hierarchy");
        auto name = lower(h.name(t));
        if (avatarPaths.contains(path) || name.find("point") != name.npos) {
            h.world(&t);
            add(t.id());
            if (avatarPaths.contains(path))
                require(retainedAvatarPaths.insert(path).second,
                        "Duplicate Avatar transform path: " + path);
        }
        for (auto &p : source.tree(t).at("m_Children")) {
            auto child = source.ref(t, p);
            require(child && child->cid == 4, "Missing or invalid child Transform");
            retain(*child, path.empty() ? h.name(*child) : path + "/" + h.name(*child));
        }
    };
    retain(root, "");
    J absentAvatarPaths = J::array();
    for (auto &[path, hash] : avatarPaths)
        if (!retainedAvatarPaths.contains(path))
            absentAvatarPaths.push_back({{"path", path}, {"hash", hash}});
    J supplementalEvidence;
    for(auto &r:weapon_supplements(source,h,root,entry,parts,supplementalEvidence,job)) {
        for(auto *bone:r.bones)add(bone->id());
        renderers.push_back(std::move(r));
    }
    std::vector<std::pair<Renderer,std::string>> attachmentRenderers;
    for (auto &p : parts) {
        auto &tr = source.object(p.at("transform"));
        auto &so = source.object(p.at("socket"));
        auto sw = h.world(&so);
        auto pw = h.world(&tr);
        require(!source.ref(tr, source.tree(tr).at("m_Father")),
                "Attachment transform must be an authored root");
        require(p.value("manualPlacement", false) || p.value("socketPlacement", false) || matrix_error(sw, pw) < .0001,
                "Attachment does not match socket: " + p.at("name").get<std::string>());
        h.worlds[tr.id()] = sw;
        h.parents[tr.id()] = so.id();
        add(tr.id());
        std::set<std::string> partVisited;
        std::function<void(Object&)> collect=[&](Object &node){
            require(partVisited.insert(node.id()).second && partVisited.size()<100000,"Invalid attachment renderer hierarchy");
            h.world(&node);auto *go=source.ref(node,source.tree(node).at("m_GameObject"));
            for(const auto &ptr:source.tree(*go).at("m_Component")) {
                auto *component=source.ref(*go,ptr.contains("component")?ptr.at("component"):ptr);
                if(!component || (component->cid!=23 && component->cid!=137))continue;
                auto renderer=renderer_info(source,*component);auto name=lower(source.name(*renderer.mesh));
                if(std::regex_search(name,std::regex("(?:lod|_l)[1-9][0-9]*")))continue;
                attachmentRenderers.push_back({std::move(renderer),p.value("materialContextMesh",std::string())});
            }
            for(const auto &ptr:source.tree(node).at("m_Children")) {
                auto *child=source.ref(node,ptr);require(child && child->cid==4,"Missing attachment child");
                h.worlds.erase(child->id());collect(*child);
            }
        };collect(tr);
    }
    deduplicate_weapon_supplements(source,entry,attachmentRenderers,supplementalEvidence);
    for(auto &[renderer,context]:attachmentRenderers) {
        if(renderer.bones.empty())add(renderer.transform->id());
        else for(auto *bone:renderer.bones){h.world(bone);add(bone->id());}
    }
    auto conv = native_basis();
    std::map<std::string, int> names;
    for (auto &id : ordered)
        names[h.name(*h.objects.at(id))]++;
    J renamedBones = J::array();
    for (auto &id : ordered) {
        Bone b;
        b.id = id;
        b.name = h.name(*h.objects.at(id));
        if (names.at(b.name) > 1) {
            auto old = b.name;
            b.name += "__" + hex64(hash64(id));
            renamedBones.push_back({{"source", id}, {"sourceName", old}, {"exportName", b.name}});
        }
        auto parent = h.parents.at(id);
        b.parent = parent.empty() ? -1 : boneIndex.at(parent);
        auto local =
            parent.empty() ? h.worlds.at(id) : glm::inverse(h.worlds.at(parent)) * h.worlds.at(id);
        decompose(conv * local * glm::transpose(conv), b.position, b.rotation, b.scale);
        b.world = conv * h.worlds.at(id) * glm::transpose(conv);
        b.inverseBind = glm::inverse(b.world);
        if (id == root.id() && avatarPaths.contains(""))
            b.paths.push_back(0);
        std::vector<std::string> path;
        auto cur = id;
        while (!cur.empty()) {
            path.push_back(h.name(*h.objects.at(cur)));
            cur = h.parents.at(cur);
        }
        std::reverse(path.begin(), path.end());
        for (size_t i = 0; i < path.size(); i++) {
            std::string s;
            for (size_t k = i; k < path.size(); k++) {
                if (k != i)
                    s += '/';
                s += path[k];
            }
            b.paths.push_back(crc32(s));
            auto key = std::to_string(crc32(s));
            if (!resolvedPaths.contains(key))
                resolvedPaths[key] = s;
        }
        model.bones.push_back(b);
    }
    J bindAliases = J::array();
    auto append = [&](Renderer &r, Object &mesh, const std::string &rigid,
                      const std::string &context) {
        if (job)
            job->check();
        auto raw = decode_mesh(source, mesh);
        auto w = h.worlds.at(rigid.empty() ? r.transform->id() : rigid);
        auto vm = conv * w;
        auto normalMatrix = glm::transpose(glm::inverse(glm::dmat3(vm)));
        bool flip = glm::determinant(glm::dmat3(vm)) < 0;
        std::map<uint32_t, uint32_t> skinJoints;
        if (rigid.empty() && !r.bones.empty()) {
            require(raw.weights.size() == raw.positions.size(),
                    "Skinned mesh lacks weights: " + raw.name);
            std::set<uint32_t> weighted;
            for (size_t v = 0; v < raw.weights.size(); v++)
                for (int k = 0; k < 4; k++)
                    if (raw.weights[v][k] > 0)
                        weighted.insert(raw.joints[v][k]);
            for (auto i : weighted) {
                require(i < r.bones.size(), "Weighted bone outside renderer");
                double error = matrix_error(h.world(r.bones[i]) * raw.bind.at(i), w);
                bindError = std::max(bindError, error);
                int sourceIndex = boneIndex.at(r.bones[i]->id());
                skinJoints[i] = uint32_t(sourceIndex);
                if (error > .002) {
                    // CAST stores bind transforms on bones rather than per mesh.
                    // Preserve a differing authored mesh inverse bind using a
                    // child bind alias. Its animated local transform is identity,
                    // so animatedWorld * inverseBind equals the source palette.
                    M4 bindWorld = conv * w * glm::inverse(raw.bind.at(i)) * glm::transpose(conv);
                    int aliasIndex = -1;
                    for (size_t n = 0; n < model.bones.size(); ++n)
                        if (model.bones[n].sourceBindAlias &&
                            model.bones[n].parent == sourceIndex &&
                            matrix_error(model.bones[n].world, bindWorld) < 1e-6) {
                            aliasIndex = int(n);
                            break;
                        }
                    if (aliasIndex < 0) {
                        Bone alias;
                        alias.id = mesh.id() + ":bind:" + r.bones[i]->id();
                        alias.name =
                            model.bones[sourceIndex].name + "__bind_" + hex64(hash64(alias.id));
                        alias.parent = sourceIndex;
                        alias.sourceBindAlias = true;
                        alias.world = bindWorld;
                        alias.inverseBind = glm::inverse(bindWorld);
                        decompose(model.bones[sourceIndex].inverseBind * bindWorld, alias.position,
                                  alias.rotation, alias.scale);
                        aliasIndex = int(model.bones.size());
                        model.bones.push_back(alias);
                    }
                    skinJoints[i] = uint32_t(aliasIndex);
                    bindAliases.push_back(
                        {{"sourceMesh", mesh.id()},
                         {"sourceBone", r.bones[i]->id()},
                         {"sourceName", model.bones[sourceIndex].name},
                         {"alias", model.bones[aliasIndex].name},
                         {"index", aliasIndex},
                         {"hierarchyBindError", error},
                         {"sourceInverseBind", matrix_json(raw.bind.at(i))},
                         {"animatedLocal",
                          "identity; inherits the original source bone world transform"}});
                }
            }
        }
        if (!rigid.empty())
            require(raw.bind.empty(), "Rigid attachment unexpectedly has skin bind poses");
        for (size_t si = 0; si < raw.faces.size(); si++) {
            if (raw.faces[si].empty())
                continue;
            Surface s;
            s.meshId = mesh.id();
            s.rendererId = r.object ? r.object->id() : "";
            s.materialContextMesh = context;
            s.name = raw.name + "_submesh" + std::to_string(si);
            s.submesh = int(si);
            std::vector<uint32_t> used = raw.faces[si];
            std::sort(used.begin(), used.end());
            used.erase(std::unique(used.begin(), used.end()), used.end());
            std::vector<uint32_t> remap(raw.positions.size());
            for (size_t vi = 0; vi < used.size(); vi++) {
                auto src = used[vi];
                remap[src] = uint32_t(vi);
                s.positions.push_back(V3(vm * glm::dvec4(raw.positions[src], 1)));
                s.sourcePositions.push_back(raw.positions[src]);
                if (!raw.normals.empty())
                    s.sourceNormals.push_back(raw.normals[src]);
                if (!raw.normals.empty()) {
                    DV3 nn = normalMatrix * DV3(raw.normals[src]);
                    require(std::isfinite(glm::length(nn)) && glm::length(nn) > 1e-8,
                            "Invalid vertex normal");
                    s.normals.push_back(V3(glm::normalize(nn)));
                }
                if (!raw.uv0.empty())
                    s.uv0.push_back(V2(raw.uv0[src].x, 1 - raw.uv0[src].y));
                if (!raw.uv1.empty())
                    s.uv1.push_back(V2(raw.uv1[src].x, 1 - raw.uv1[src].y));
                if (!rigid.empty() || r.bones.empty()) {
                    s.weights.push_back(V4(1, 0, 0, 0));
                    s.joints.push_back(glm::uvec4(
                        boneIndex.at(rigid.empty() ? r.transform->id() : rigid), 0, 0, 0));
                } else {
                    auto wgt = raw.weights[src];
                    auto js = raw.joints[src];
                    for (int k = 0; k < 4; k++) {
                        if (wgt[k] > 0) {
                            require(js[k] < r.bones.size(),
                                    "Skin joint outside renderer bone array");
                            js[k] = skinJoints.at(js[k]);
                        } else
                            js[k] = 0;
                    }
                    s.weights.push_back(wgt);
                    s.joints.push_back(js);
                }
            }
            for (size_t i = 0; i < raw.faces[si].size(); i += 3) {
                uint32_t a = remap[raw.faces[si][i]], b = remap[raw.faces[si][i + 1]],
                         c = remap[raw.faces[si][i + 2]];
                if (flip)
                    std::swap(b, c);
                s.indices.insert(s.indices.end(), {a, b, c});
            }
            model.surfaces.push_back(std::move(s));
        }
    };
    for (auto &r : renderers)
        append(r, *r.mesh, "", "");
    for(auto &[r,context]:attachmentRenderers)append(r,*r.mesh,r.bones.empty()?r.transform->id():std::string(),context);
    size_t vertices = 0, triangles = 0;
    for (auto &s : model.surfaces) {
        vertices += s.positions.size();
        triangles += s.indices.size() / 3;
    }
    require(triangles > 0, "No triangles in selected model");
    model.report = {{"name", model.name},
                    {"vertices", vertices},
                    {"triangles", triangles},
                    {"bones", model.bones.size()},
                    {"surfaces", model.surfaces.size()},
                    {"maximumBindError", bindError},
                    {"units", "metres"},
                    {"upAxis", "Z"},
                    {"parts", parts},
                    {"source", entry},
                    {"t6Compatible", false}};
    model.report["rigidGeometry"] =
        std::all_of(renderers.begin(), renderers.end(), [](auto &r) { return r.bones.empty(); });
    model.report["renamedDuplicateBones"] = renamedBones;
    model.report["omittedCollisionHelpers"] = omitted;
    model.report["supplementalGeometry"]=supplementalEvidence;
    for(const auto &item:supplementalEvidence)if(item.at("status")=="unmatched")model.report["assemblyWarnings"].push_back(item);
    if (!bindAliases.empty())
        model.report["sourceBindAliases"] = bindAliases;
    model.report["animationHierarchy"] = {{"avatars", avatarEvidence},
                                          {"retainedAvatarPaths", retainedAvatarPaths.size()},
                                          {"absentAvatarPaths", absentAvatarPaths},
                                          {"resolvedPaths", resolvedPaths},
                                          {"root", root.id()},
                                          {"rootWorld", matrix_json(h.world(&root))}};
    return model;
}
J raw_mesh_json(const RawMesh &m) {
    J j = {{"id", m.id},
           {"name", m.name},
           {"positions", J::array()},
           {"normals", J::array()},
           {"uv0", J::array()},
           {"weights", J::array()},
           {"joints", J::array()},
           {"faces", m.faces}};
    for (auto v : m.positions)
        j["positions"].push_back({v.x, v.y, v.z});
    for (auto v : m.normals)
        j["normals"].push_back({v.x, v.y, v.z});
    for (auto v : m.uv0)
        j["uv0"].push_back({v.x, v.y});
    for (auto v : m.weights)
        j["weights"].push_back({v.x, v.y, v.z, v.w});
    for (auto v : m.joints)
        j["joints"].push_back({v.x, v.y, v.z, v.w});
    return j;
}
} // namespace codm
