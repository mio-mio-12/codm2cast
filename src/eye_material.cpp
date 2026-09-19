#include "eye_material.hpp"
#include "uv_bake.hpp"
namespace codm {
static float linear(float v) { return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f); }
static uint8_t byte(float v) { return uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255)); }
static uint8_t color_byte(float v) { return byte(v <= .0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - .055f); }
Material bake_eye_surface(const Image &iris, const Image &sclera,
                          const EyeSurfaceParameters &p, bool preview) {
    require(p.scale > 0 && p.radius > 0 && p.edgeWidth > 0 && p.scleraScale > 0,
            "Invalid eye UV scale or iris radius");
    int w = std::max(iris.width, sclera.width), h = std::max(iris.height, sclera.height);
    Material out;
    out.maps["albedo"] = {w, h, Bytes(size_t(w) * h * 4)};
    if (!preview) for (auto name : {"specular", "gloss"}) out.maps[name] = {w, h, Bytes(size_t(w) * h * 4)};
    // Installed Eye fragment program: reproduce static UV/color composition,
    // excluding the view-dependent refraction displacement and lighting.
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        V2 uv((x + .5f) / w, (y + .5f) / h), centered = (uv - .5f) / p.scale;
        float radius = glm::length(centered);
        float coverage = std::clamp(1 - (radius - (p.radius - p.edgeWidth)) / p.edgeWidth, 0.f, 1.f);
        coverage = coverage * coverage * (3 - 2 * coverage);
        V2 irisUV(0);
        if (radius > 1e-8f) {
            float pupil = std::clamp((1 - radius / p.radius) * p.pupil, 0.f, 1.f);
            irisUV = centered / radius * (.5f * (1 - pupil));
        }
        float edge = std::max(0.f, 1 - std::pow(glm::length(irisUV * p.darkScale), p.hardness));
        V3 irisColor = V3(sample_linear_overlay(iris, irisUV + .5f)) * p.irisTint * edge;
        V3 scleraColor = V3(sample_linear_overlay(sclera, (uv - .5f) / p.scleraScale + .5f)) * p.scleraTint;
        V3 color = glm::mix(scleraColor, irisColor, coverage);
        size_t i = (size_t(y) * w + x) * 4;
        auto &albedo = out.maps.at("albedo").pixels;
        for (int k = 0; k < 3; ++k) albedo[i + k] = color_byte(color[k]);
        albedo[i + 3] = 255;
        if (!preview) {
            uint8_t gloss = byte(1 - glm::mix(p.scleraRoughness, p.corneaRoughness, coverage));
            auto &spec = out.maps.at("specular").pixels; auto &g = out.maps.at("gloss").pixels;
            for (int k = 0; k < 3; ++k) { spec[i + k] = color_byte(.04f); g[i + k] = gloss; }
            spec[i + 3] = gloss; g[i + 3] = 255;
        }
    }
    if (preview) out.previewBaseColor = out.maps.at("albedo");
    return out;
}
Material convert_eye_material(Source &source, Object &material, int maxDimension,
                              std::map<std::string, Image> &cache, bool preview) {
    const auto &tree = source.tree(material); auto *shader = source.ref(material, tree.at("m_Shader"));
    require(shader && sha256(shader->raw()) == "b8e8a757701fb4a71b3e3569e9df4cbbc66048083c49a142090ccf3598cf18f5",
            "Shader revision differs from verified adapter: CODMStandard Eye");
    const auto &parsed = source.tree(*shader).at("m_ParsedForm");
    const auto &saved = tree.at("m_SavedProperties");
    std::map<std::string, V4> values;
    for (const auto &p : parsed.at("m_PropInfo").at("m_Props")) {
        V4 v; for (int k = 0; k < 4; ++k) v[k] = p.value("m_DefValue[" + std::to_string(k) + "]", 0.f);
        values[p.at("m_Name")] = v;
    }
    for (const auto &p : saved.at("m_Floats")) values[p.at("first")] = V4(p.at("second").get<float>());
    for (const auto &p : saved.at("m_Colors")) { const auto &v = p.at("second"); values[p.at("first")] = V4(v.at("r"), v.at("g"), v.at("b"), v.at("a")); }
    auto scalar = [&](const char *name) { return values.at(name).x; };
    auto tint = [&](const char *name) { auto v = V3(values.at(name)); for (int k = 0; k < 3; ++k) v[k] = linear(v[k]); return v; };
    J records = J::object();
    auto texture = [&](const char *name) {
        for (const auto &p : saved.at("m_TexEnvs")) if (p.at("first") == name) {
            const auto &env = p.at("second"); auto *asset = source.ref(material, env.at("m_Texture"));
            require(asset && asset->cid == 28, std::string("Missing authored eye texture: ") + name);
            auto key = asset->id() + ":" + std::to_string(maxDimension);
            if (!cache.contains(key)) cache[key] = decode_texture(source, *asset, maxDimension);
            records[name] = {{"source", asset->id()}, {"name", source.name(*asset)}};
            return cache.at(key);
        }
        throw std::runtime_error(std::string("Missing authored eye texture: ") + name);
    };
    EyeSurfaceParameters p;
    p.scale = scalar("_ScaleByCenter"); p.radius = scalar("_IrisUVRadius"); p.edgeWidth = scalar("_LimbusUVwidth");
    p.pupil = scalar("_PupilScale"); p.scleraScale = scalar("_ScleraScale"); p.darkScale = scalar("_LimbusDarkScale");
    p.hardness = scalar("_LimbusHardness"); p.scleraRoughness = scalar("_ScleraRoughness"); p.corneaRoughness = scalar("_CorneaRoughness");
    p.irisTint = tint("_IrisiColor"); p.scleraTint = tint("_ScleraColor");
    auto iris = texture("_IrisMap"), sclera = texture("_ScleraMap");
    auto out = bake_eye_surface(iris, sclera, p, preview);
    out.id = material.id(); out.name = "eye_" + hex64(hash64(out.id)); out.shader = "CODMStandard Eye";
    out.provenance = {{"shader", out.shader}, {"shaderSHA256", sha256(shader->raw())}, {"textures", records},
        {"workflow", "Static iris/sclera composite with authored pupil scale, tint and limbus; dielectric specular/gloss"},
        {"alphaMode", "opaque"}, {"emission", false},
        {"limitations", "View-dependent refraction, procedural cornea normals, caustics, eye shadow and environment reflections require a live eye shader and are not baked."}};
    return out;
}
}
