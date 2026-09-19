#pragma once
#include "source.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
namespace codm {
using V2 = glm::vec2;
using V3 = glm::vec3;
using V4 = glm::vec4;
using M4 = glm::dmat4;
using DV3 = glm::dvec3;
using Q = glm::dquat;
inline DV3 vec3(const J &j) {
    return {j.at("x").get<double>(), j.at("y").get<double>(), j.at("z").get<double>()};
}
M4 transform_matrix(const J &t);
void decompose(const M4 &m, DV3 &p, Q &q, DV3 &s);
M4 native_basis();
double matrix_error(const M4 &a, const M4 &b);
J matrix_json(const M4 &m);
M4 json_matrix(const J &j);
struct RawMesh {
    std::string id, name;
    double uvPrecision = 1e-7;
    std::vector<V3> positions, normals;
    std::vector<V2> uv0, uv1;
    std::vector<V4> weights;
    std::vector<glm::uvec4> joints;
    std::vector<std::vector<uint32_t>> faces;
    std::vector<M4> bind;
};
RawMesh decode_mesh(Source &source, Object &o);
struct Renderer {
    Object *object = nullptr, *mesh = nullptr, *transform = nullptr;
    std::vector<Object *> bones;
    J tail;
    size_t tailOffset = 0;
};
Object *owner_transform(Source &source, Object &o);
Renderer renderer_info(Source &source, Object &o);
struct Hierarchy {
    Source &source;
    std::map<std::string, M4> worlds;
    std::map<std::string, Object *> objects;
    std::map<std::string, std::string> parents;
    explicit Hierarchy(Source &s) : source(s) {}
    M4 world(Object *o, std::set<std::string> stack = {});
    Object &root(Object &o);
    std::string name(Object &o);
    J sockets(Object &root, const std::set<std::string> &requested);
};
struct Bone {
    std::string id, name;
    int parent = -1;
    DV3 position{0}, scale{1};
    Q rotation{1, 0, 0, 0};
    M4 world{1}, inverseBind{1};
    std::vector<uint32_t> paths;
    bool sourceBindAlias = false;
};
struct Surface {
    std::string name, meshId, rendererId, materialId, materialContextMesh;
    int submesh = 0;
    bool materialFailed = false;
    std::vector<V3> positions, normals;
    std::vector<V3> sourcePositions, sourceNormals;
    std::vector<V2> uv0, uv1;
    std::vector<V4> weights;
    std::vector<glm::uvec4> joints;
    std::vector<uint32_t> indices;
};
struct Model {
    std::string name;
    std::vector<Bone> bones;
    std::vector<Surface> surfaces;
    J report;
};
Model prepare_geometry(Source &source, const J &entry, const J &parts = J::array(),
                       JobContext *job = nullptr);
J raw_mesh_json(const RawMesh &mesh);
std::optional<std::pair<std::string, std::string>> weapon_identity(const std::string &name);
std::optional<std::array<std::string, 3>> part_identity(const std::string &name);
J discover_parts(Source &source, const J &entry, const fs::path &cache, JobContext *job = nullptr,
                 const J &donor = nullptr);
J resolve_parts(const J &profile, J &choices, bool fillUnset, J *states = nullptr,
                bool strict = true);
} // namespace codm
