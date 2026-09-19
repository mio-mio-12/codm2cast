#pragma once
#include "materials.hpp"
namespace codm {
struct EyeSurfaceParameters {
    float scale = 1, radius = .18f, edgeWidth = .035f, pupil = .85f;
    float scleraScale = 1.2f, darkScale = 2.15f, hardness = 6;
    float scleraRoughness = .1f, corneaRoughness = .05f;
    V3 irisTint{1}, scleraTint{1}; // Linear RGB.
};
Material bake_eye_surface(const Image &iris, const Image &sclera,
                          const EyeSurfaceParameters &parameters, bool preview);
Material convert_eye_material(Source &source, Object &material, int maxDimension,
                              std::map<std::string, Image> &cache, bool preview);
}
