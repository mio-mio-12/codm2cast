#pragma once
#include <algorithm>
namespace codm {
inline float hair_coverage(float alpha, float rootGreen, float cutoff, float tips) {
    const float threshold = cutoff + (tips - cutoff) * rootGreen;
    // DEPTH writes opaque coverage above threshold. FORWARD also draws the
    // finer strands below it, with saturate(MainTex.a / (threshold + .001)).
    return alpha >= threshold ? 1.f : std::clamp(alpha / (threshold + .001f), 0.f, 1.f);
}
}
