#include "cast_writer.hpp"
namespace codm {
static std::atomic<uint64_t> nextHash{0x534e495752545250ull};
CastNode &CastNode::add(uint32_t id) {
    children.push_back(std::make_unique<CastNode>(id, nextHash++));
    return *children.back();
}
void CastNode::string(const std::string &name, const std::string &value) {
    require(value.find('\0') == value.npos, "NUL in CAST string");
    CastProperty p{name, "s", 1, Bytes(value.begin(), value.end())};
    p.bytes.push_back(0);
    properties.push_back(std::move(p));
}
size_t CastNode::size() const {
    size_t n = 24;
    for (auto &p : properties)
        n += 8 + p.name.size() + p.bytes.size();
    for (auto &c : children)
        n += c->size();
    require(n <= UINT32_MAX, "CAST node exceeds 4 GiB");
    return n;
}
template <class T> static void append(Bytes &b, T v) {
    static_assert(std::endian::native == std::endian::little);
    auto *p = reinterpret_cast<const uint8_t *>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}
void CastNode::write(Bytes &b) const {
    append(b, identifier);
    append(b, uint32_t(size()));
    append(b, hash);
    append(b, uint32_t(properties.size()));
    append(b, uint32_t(children.size()));
    for (auto &p : properties) {
        require(!p.name.empty() && p.name.size() <= 65535 && p.type.size() <= 2,
                "Invalid CAST property name/type");
        b.push_back(p.type[0]);
        b.push_back(p.type.size() == 2 ? p.type[1] : 0);
        append(b, uint16_t(p.name.size()));
        append(b, p.count);
        b.insert(b.end(), p.name.begin(), p.name.end());
        b.insert(b.end(), p.bytes.begin(), p.bytes.end());
    }
    for (auto &c : children)
        c->write(b);
}
Bytes encode_model_cast(const Model &model, const MaterialSet &mats, const std::string &stem,
                        const std::string &folder) {
    require(!model.surfaces.empty(), "No model geometry to export");
    require(mats.errors.empty(), "Cannot export unresolved material surfaces");
    CastNode root(0x746f6f72, nextHash++);
    auto &meta = root.add(0x6174656d);
    meta.string("a", "CODM CAST Studio");
    meta.string("s", "CODM native C++ offline exporter");
    meta.string("up", "z");
    auto &m = root.add(0x6c646f6d);
    m.string("n", stem);
    auto &skeleton = m.add(0x6c656b73);
    for (size_t i = 0; i < model.bones.size(); i++) {
        auto &b = model.bones[i];
        require(b.parent < int(i) && b.parent >= -1, "Invalid parent order: " + b.name);
        auto &node = skeleton.add(0x656e6f62);
        node.string("n", b.name);
        node.scalar<uint32_t>("p", "i", uint32_t(b.parent));
        node.scalar<uint8_t>("ssc", "b", 0);
        std::array<float, 3> p{float(b.position.x), float(b.position.y), float(b.position.z)},
            s{float(b.scale.x), float(b.scale.y), float(b.scale.z)};
        std::array<float, 4> q{float(b.rotation.x), float(b.rotation.y), float(b.rotation.z),
                               float(b.rotation.w)};
        node.array<float>("lp", "3v", p, 3);
        node.array<float>("lr", "4v", q, 4);
        node.array<float>("s", "3v", s, 3);
    }
    std::map<std::string, uint64_t> materialHashes;
    for (auto &[key, mat] : mats.materials) {
        auto &node = m.add(0x6c74616d);
        materialHashes[key] = node.hash;
        node.string("n", mat.name);
        node.string("t", "pbr");
        for (auto &[slot, im] : mat.maps) {
            auto &f = node.add(0x656c6966);
            f.string("p", folder + "/" + mat.name + "_" + slot + ".png");
            node.scalar<uint64_t>(slot, "l", f.hash);
        }
    }
    for (auto &s : model.surfaces) {
        require(materialHashes.contains(s.materialId), "Surface has no converted material: " + s.name);
        auto &node = m.add(0x6873656d);
        node.string("n", s.name);
        node.array<float>(
            "vp", "3v",
            std::span(reinterpret_cast<const float *>(s.positions.data()), s.positions.size() * 3), 3);
        if (!s.normals.empty())
            node.array<float>(
                "vn", "3v",
                std::span(reinterpret_cast<const float *>(s.normals.data()), s.normals.size() * 3), 3);
        node.array<uint32_t>("f", "i", s.indices);
        node.scalar<uint64_t>("m", "l", materialHashes.at(s.materialId));
        node.scalar<uint8_t>("ul", "b", s.uv0.empty() ? 0 : 1);
        if (!s.uv0.empty())
            node.array<float>(
                "u0", "2v", std::span(reinterpret_cast<const float *>(s.uv0.data()), s.uv0.size() * 2),
                2);
        node.scalar<uint8_t>("mi", "b", s.weights.empty() ? 0 : 4);
        node.string("sm", "linear");
        if (!s.weights.empty()) {
            node.array<float>(
                "wv", "f",
                std::span(reinterpret_cast<const float *>(s.weights.data()), s.weights.size() * 4));
            node.array<uint32_t>(
                "wb", "i",
                std::span(reinterpret_cast<const uint32_t *>(s.joints.data()), s.joints.size() * 4));
        }
    }
    Bytes result;
    result.reserve(root.size() + 16);
    append(result, uint32_t(0x74736163));
    append(result, uint32_t(1));
    append(result, uint32_t(1));
    append(result, uint32_t(0));
    root.write(result);
    validate_cast(result);
    return result;
}
void validate_cast(std::span<const uint8_t> bytes) {
    Reader r(bytes);
    require(r.get<uint32_t>() == 0x74736163 && r.get<uint32_t>() == 1, "Invalid CAST header");
    auto roots = r.get<uint32_t>();
    r.get<uint32_t>();
    require(roots > 0 && roots < 1024, "Invalid CAST root count");
    std::set<uint64_t> hashes;
    std::function<void(size_t)> node = [&](size_t limit) {
        size_t start = r.pos;
        r.get<uint32_t>();
        auto size = r.get<uint32_t>();
        require(size >= 24 && start <= limit && size <= limit - start, "CAST node size outside parent");
        auto hash = r.get<uint64_t>();
        require(hashes.insert(hash).second, "Duplicate CAST node hash");
        auto np = r.get<uint32_t>(), nc = r.get<uint32_t>();
        require(np < 100000 && nc < 1000000, "Excess CAST node counts");
        std::set<std::string> names;
        for (uint32_t i = 0; i < np; i++) {
            auto type = r.take(2);
            auto length = r.get<uint16_t>();
            auto count = r.get<uint32_t>();
            auto name = r.take(length);
            require(names.insert(std::string(name.begin(), name.end())).second,
                    "Duplicate CAST property");
            std::string t;
            for (auto c : type)
                if (c)
                    t += char(c);
            if (t == "s") {
                require(count == 1, "CAST string count must be one");
                r.cstr();
            } else {
                static const std::map<std::string, size_t> widths = {{"b", 1},  {"h", 2},   {"i", 4},
                                                                     {"l", 8},  {"f", 4},   {"d", 8},
                                                                     {"2v", 8}, {"3v", 12}, {"4v", 16}};
                require(widths.contains(t), "Unknown CAST property type");
                require(count < 100000000, "Excess CAST array count");
                r.skip(size_t(count) * widths.at(t));
            }
            require(r.pos <= start + size, "CAST property outside node");
        }
        for (uint32_t i = 0; i < nc; i++)
            node(start + size);
        require(r.pos == start + size, "CAST node length mismatch");
    };
    for (uint32_t i = 0; i < roots; i++)
        node(bytes.size());
    require(r.pos == bytes.size(), "CAST trailing data");
}
J export_model(Model model, const MaterialSet &mats, const fs::path &dir, const std::string &stem,
               JobContext *job) {
    require(!stem.empty() && stem.find_first_of("\\/:*?\"<>|") == stem.npos && stem != "." &&
                stem != ".." && stem.back() != '.' && stem.back() != ' ',
            "Invalid output filename");
    require(mats.errors.empty(), "Unresolved material surfaces; inspect material errors before export");
    fs::create_directories(dir);
    require(fs::is_directory(dir), "Models destination is not a folder");
    auto cast = dir / (stem + ".cast"), textures = dir / (stem + "_materials"),
         report = dir / (stem + ".json");
    require(!fs::exists(cast) && !fs::exists(textures) && !fs::exists(report),
            "Output already exists. Choose a different name or destination.");
    auto start = Clock::now();
    auto folder = stem + "_materials";
    auto bytes = encode_model_cast(model, mats, stem, folder);
    auto stage = dir / (".codm-stage-" + hex64(uint64_t(Clock::now().time_since_epoch().count())));
    require(fs::create_directory(stage), "Cannot create export staging folder");
    size_t written = 0;
    size_t totalMaps=0;for(const auto &[key,mat]:mats.materials) totalMaps+=mat.maps.size();
    bool texturesPublished = false, castPublished = false, reportPublished = false;
    try {
        for (auto &[key, mat] : mats.materials) {
            for (auto &[slot, im] : mat.maps) {
                if (job)
                    job->update(float(written)/std::max(size_t(1),totalMaps),"Writing texture "+std::to_string(written+1)+"/"+std::to_string(totalMaps)+": "+mat.name+" "+slot);
                write_png(stage / folder / (mat.name + "_" + slot + ".png"), im);
                written++;
            }
            write_json(stage / folder / (mat.name + ".json"), mat.provenance);
        }
        if (job)
            job->check();
        std::ofstream out(stage / (stem + ".cast"), std::ios::binary);
        require(bool(out), "Cannot create CAST output");
        out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        out.close();
        require(bool(out), "CAST write failed");
        validate_cast(read_bytes(stage / (stem + ".cast")));
        auto result = model.report;
        result["surfaces"] = model.surfaces.size();
        result["bones"] = model.bones.size();
        size_t vertices = 0, triangles = 0;
        for (auto &surface : model.surfaces) {
            vertices += surface.positions.size();
            triangles += surface.indices.size() / 3;
        }
        result["vertices"] = vertices;
        result["triangles"] = triangles;
        result["status"] = "exported";
        result["model"] = pathstr(fs::absolute(cast));
        result["textures"] = pathstr(fs::absolute(textures));
        result["pngFiles"] = written;
        result["exportWriteSeconds"] = seconds(start);
        result["emission"] = false;
        write_json(stage / (stem + ".json"), result);
        if (job)
            job->check();
        fs::rename(stage / folder, textures);
        texturesPublished = true;
        fs::rename(stage / (stem + ".json"), report);
        reportPublished = true;
        fs::rename(stage / (stem + ".cast"), cast);
        castPublished = true;
        fs::remove(stage);
        return result;
    } catch (...) {
        std::error_code ec;
        remove_owned_tree(stage, dir);
        if (!castPublished && texturesPublished)
            remove_owned_tree(textures, dir);
        if (!castPublished && reportPublished)
            fs::remove(report, ec);
        throw;
    }
}
} // namespace codm
