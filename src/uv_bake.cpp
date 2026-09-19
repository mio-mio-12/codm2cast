#include "uv_bake.hpp"
namespace codm {
static float linear(float c) {
    return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
}
V4 sample_linear_overlay(const Image &im, V2 uv) {
    // Decoded channels are bytes. Each of the 256 sRGB conversions is identical
    // for every texel, so avoid repeating pow() at all four bilinear taps.
    static const auto linearBytes = [] {
        std::array<float, 256> values;
        for (size_t i = 0; i < values.size(); i++)
            values[i] = linear(float(i) / 255.f);
        return values;
    }();
    require(std::isfinite(uv.x) && std::isfinite(uv.y), "Nonfinite overlay UV");
    uv -= glm::floor(uv);
    float x = uv.x * im.width - .5f, y = uv.y * im.height - .5f;
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = x - ix, fy = y - iy;
    V4 out(0);
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
            int xx = (ix + dx + im.width) % im.width, yy = (iy + dy + im.height) % im.height;
            auto p = &im.pixels[(size_t(yy) * im.width + xx) * 4];
            V4 value(linearBytes[p[0]], linearBytes[p[1]], linearBytes[p[2]], p[3] / 255.f);
            out += value * ((dx ? fx : 1 - fx) * (dy ? fy : 1 - fy));
        }
    return out;
}
std::vector<V4> bake_overlay(const Image &image, const Surface &context, int w, int h,
                             const std::vector<float> &importance,
                             const std::vector<float> &vertexAlpha, V2 sampleScale, V2 sampleOffset,
                             bool clampUV) {
    require(context.uv0.size() == context.positions.size() &&
                context.uv1.size() == context.positions.size(),
            "Overlay requires complete UV0 and UV1");
    size_t pixels = size_t(w) * h;
    require(importance.size() == pixels, "Overlay importance dimensions mismatch");
    require(vertexAlpha.empty() || vertexAlpha.size() == context.positions.size(),
            "Overlay alpha dimensions mismatch");
    struct Layer {
        std::vector<V4> values;
        std::vector<uint32_t> indices;
    };
    std::vector<Layer> layers;
    layers.push_back({std::vector<V4>(pixels, V4(0, 0, 0, -1)), {}});
    for (size_t tri = 0; tri < context.indices.size(); tri += 3) {
        auto ia = context.indices[tri], ib = context.indices[tri + 1], ic = context.indices[tri + 2];
        V2 a = context.uv0.at(ia) * V2(w, h), b = context.uv0.at(ib) * V2(w, h),
           c = context.uv0.at(ic) * V2(w, h);
        double det = double(b.x - a.x) * (c.y - a.y) - double(b.y - a.y) * (c.x - a.x);
        std::vector<std::pair<size_t, V4>> values;
        if (std::abs(det) > 1e-10) {
            int x0 = std::clamp(int(std::floor(std::min({a.x, b.x, c.x}) - 1)), 0, w),
                x1 = std::clamp(int(std::ceil(std::max({a.x, b.x, c.x}) + 1)), 0, w),
                y0 = std::clamp(int(std::floor(std::min({a.y, b.y, c.y}) - 1)), 0, h),
                y1 = std::clamp(int(std::ceil(std::max({a.y, b.y, c.y}) + 1)), 0, h);
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    double px = x + .5 - a.x, py = y + .5 - a.y,
                           u = (px * (c.y - a.y) - py * (c.x - a.x)) / det,
                           v = ((b.x - a.x) * py - (b.y - a.y) * px) / det;
                    if (u >= -1e-5 && v >= -1e-5 && u + v <= 1.00001) {
                        auto uv = float(1 - u - v) * context.uv1.at(ia) +
                                  float(u) * context.uv1.at(ib) + float(v) * context.uv1.at(ic);
                        if (clampUV)
                            uv = glm::clamp(uv, V2(0), V2(1));
                        auto value = sample_linear_overlay(image, uv * sampleScale + sampleOffset);
                        if (!vertexAlpha.empty())
                            value.a *= float(1 - u - v) * vertexAlpha.at(ia) +
                                       float(u) * vertexAlpha.at(ib) + float(v) * vertexAlpha.at(ic);
                        values.push_back({size_t(y) * w + x, value});
                    }
                }
        }
        size_t index = 0;
        for (; index < layers.size(); index++) {
            bool conflict = false;
            for (auto &[pixel, value] : values) {
                auto old = layers[index].values[pixel];
                if (old.a < 0)
                    continue;
                auto delta = glm::abs(V3(old) * old.a - V3(value) * value.a);
                if (std::max({delta.x, delta.y, delta.z, std::abs(old.a - value.a)}) *
                        importance[pixel] >
                    1e-4) {
                    conflict = true;
                    break;
                }
            }
            if (!conflict)
                break;
        }
        if (index == layers.size()) {
            require(layers.size() < 16, "Overlay needs more than 16 material layers");
            require(pixels * (layers.size() + 1) * sizeof(V4) <= 1024ull * 1024 * 1024,
                    "Overlay layer memory limit exceeded");
            layers.push_back({std::vector<V4>(pixels, V4(0, 0, 0, -1)), {}});
        }
        for (auto &[pixel, value] : values)
            layers[index].values[pixel] = value;
        auto &indices = layers[index].indices;
        indices.insert(indices.end(), {ia, ib, ic});
    }
    if (layers.size() > 1) {
        std::vector<std::vector<uint32_t>> groups;
        for (auto &layer : layers)
            groups.push_back(std::move(layer.indices));
        throw UVLayers(std::move(groups));
    }
    auto result = std::move(layers[0].values);
    for (int pass = 0; pass < 4; pass++)
        for (auto direction : std::array<std::pair<int, int>, 4>{{{0, -1}, {0, 1}, {-1, 0}, {1, 0}}}) {
            std::vector<std::pair<size_t, V4>> fill;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    size_t i = size_t(y) * w + x;
                    if (result[i].a >= 0)
                        continue;
                    int xx = (x + direction.first + w) % w, yy = (y + direction.second + h) % h;
                    auto value = result[size_t(yy) * w + xx];
                    if (value.a >= 0)
                        fill.push_back({i, value});
                }
            for (auto &[pixel, value] : fill)
                result[pixel] = value;
        }
    for (auto &p : result)
        if (p.a < 0)
            p = V4(0);
    return result;
}
Surface subset_surface(const Surface &source, const std::vector<uint32_t> &indices) {
    Surface out;
    out.name = source.name;
    out.meshId = source.meshId;
    out.rendererId = source.rendererId;
    out.materialContextMesh = source.materialContextMesh;
    out.materialId = source.materialId;
    out.submesh = source.submesh;
    std::map<uint32_t, uint32_t> map;
    for (auto index : indices) {
        if (!map.contains(index)) {
            map[index] = uint32_t(out.positions.size());
            out.positions.push_back(source.positions.at(index));
            if (!source.sourcePositions.empty())
                out.sourcePositions.push_back(source.sourcePositions.at(index));
            if (!source.sourceNormals.empty())
                out.sourceNormals.push_back(source.sourceNormals.at(index));
            if (!source.normals.empty())
                out.normals.push_back(source.normals.at(index));
            if (!source.uv0.empty())
                out.uv0.push_back(source.uv0.at(index));
            if (!source.uv1.empty())
                out.uv1.push_back(source.uv1.at(index));
            if (!source.weights.empty())
                out.weights.push_back(source.weights.at(index));
            if (!source.joints.empty())
                out.joints.push_back(source.joints.at(index));
        }
        out.indices.push_back(map.at(index));
    }
    return out;
}
} // namespace codm
