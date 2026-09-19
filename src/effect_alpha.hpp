#pragma once
#include "materials.hpp"
namespace codm {
// 0: not additive; 1: One/One; 5: SrcAlpha/One (Unity blend values).
int additive_source_factor(const J &parsed,const J &saved);
V4 straight_alpha_from_additive(V3 contribution);
void bleed_transparent_rgb(Image &image);
V4 sample_effect_data(const Image &image,V2 uv);
}
