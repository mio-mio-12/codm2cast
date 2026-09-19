#pragma once
#include "library.hpp"
namespace codm {
struct Image {
    int width = 0, height = 0;
    Bytes pixels;
};
std::string sha256(std::span<const uint8_t> bytes);
Image decode_texture(Source &source, Object &texture, int maxDimension = 0);
void write_png(const fs::path &path, const Image &image);
Image resize_image(const Image &image, int width, int height);
std::string material_content_key(Source &source, Object &material);
std::set<std::string> component_material_candidates(const std::string &meshName, const J &candidates);
J texture_content_fingerprint(const J &texture, std::span<const uint8_t> payload);
std::optional<double> material_uv_correspondence(const RawMesh &target, size_t targetSlot,
                                                const RawMesh &donor, size_t donorSlot);
struct Material {
    std::string id, name, shader;
    std::map<std::string, Image> maps;
    Image previewBaseColor;
    J provenance;
};
struct MaterialSet {
    std::map<std::string, Material> materials;
    J surfaces = J::array();
    J errors = J::array();
};
// Export-only: retain diagnostics while removing surfaces without usable materials.
void omit_unresolved_surfaces(Model &model, MaterialSet &materials);
class MaterialResolver {
    Source &source;
    fs::path dbPath;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> containerCache;
    std::map<std::string, std::vector<J>> related;
    std::map<std::string, std::vector<Object *>> peers;
    std::map<std::string, J> recoveryResults;

  public:
    MaterialResolver(Source &s, fs::path db) : source(s), dbPath(std::move(db)) {}
    std::vector<std::pair<std::string, std::string>> containers(SerializedFile &file);
    J candidates(Object &mesh, Object *context = nullptr);
    J resolve(Surface &surface, const J &overrides = J::object());
    J recover(Object &mesh, Object *context = nullptr, JobContext *job = nullptr);
};
MaterialSet prepare_materials(Source &source, Model &model, const fs::path &database,
                              const J &overrides = J::object(), int maxDimension = 0,
                              JobContext *job = nullptr, bool preview = false);
Material convert_material(Source &source, Object &material, const Surface &context, int maxDimension,
                          std::map<std::string, Image> &textureCache, bool preview = false);
} // namespace codm
