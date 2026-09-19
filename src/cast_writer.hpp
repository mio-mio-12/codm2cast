#pragma once
#include "animation.hpp"
#include "materials.hpp"
namespace codm {
struct CastProperty {
    std::string name, type;
    uint32_t count = 0;
    Bytes bytes;
};
struct CastNode {
    uint32_t identifier;
    uint64_t hash;
    std::vector<CastProperty> properties;
    std::vector<std::unique_ptr<CastNode>> children;
    CastNode(uint32_t id, uint64_t h) : identifier(id), hash(h) {}
    CastNode &add(uint32_t id);
    void string(const std::string &name, const std::string &value);
    template <class T>
    void array(const std::string &name, const std::string &type, std::span<const T> values,
               int components = 1) {
        require(values.size() % components == 0, "CAST vector width mismatch");
        CastProperty p{name, type, uint32_t(values.size() / components), Bytes(values.size_bytes())};
        std::memcpy(p.bytes.data(), values.data(), p.bytes.size());
        properties.push_back(std::move(p));
    }
    template <class T> void scalar(const std::string &name, const std::string &type, T value) {
        array<T>(name, type, std::span(&value, 1));
    }
    size_t size() const;
    void write(Bytes &bytes) const;
};
Bytes encode_model_cast(const Model &model, const MaterialSet &materials, const std::string &stem,
                        const std::string &textureFolder);
void validate_cast(std::span<const uint8_t> bytes);
J export_model(Model model, const MaterialSet &materials, const fs::path &directory,
               const std::string &stem, JobContext *job = nullptr);
Bytes encode_animation_cast(const Model &model, const Animation &animation, const std::string &stem);
bool equivalent_animation_cast(const Bytes &a,const Bytes &b);
J export_animation(const Model &model, const Animation &animation, const fs::path &directory,
                   const std::string &stem, JobContext *job = nullptr, bool reuseIdentical = false);
} // namespace codm
