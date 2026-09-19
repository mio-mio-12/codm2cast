#pragma once
#include "materials.hpp"
namespace codm {
struct UVLayers : std::runtime_error {
    std::vector<std::vector<uint32_t>> groups;
    explicit UVLayers(std::vector<std::vector<uint32_t>> g)
        : std::runtime_error("Secondary UV overlay needs separate material layers"),
          groups(std::move(g)) {}
};
V4 sample_linear_overlay(const Image &image, V2 uv);
std::vector<V4> bake_overlay(const Image &image, const Surface &context, int width, int height,
                             const std::vector<float> &importance,
                             const std::vector<float> &vertexAlpha = {}, V2 sampleScale = V2(1),
                             V2 sampleOffset = V2(0), bool clampUV = false);
Surface subset_surface(const Surface &surface, const std::vector<uint32_t> &indices);
} // namespace codm
