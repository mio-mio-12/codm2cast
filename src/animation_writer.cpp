#include "cast_writer.hpp"
namespace codm {
bool equivalent_animation_cast(const Bytes &a,const Bytes &b) {
    if(a==b)return true;
    if(a.size()!=b.size())return false;
    auto normalized=[](Bytes bytes){
        validate_cast(bytes);Reader r(bytes);r.skip(8);auto roots=r.get<uint32_t>();r.skip(4);
        std::function<void()> node=[&]{
            auto id=r.get<uint32_t>();
            // These animation nodes contain no hash-reference properties. Do
            // not normalize arbitrary model/material files with references.
            static const std::set<uint32_t> allowed={0x746f6f72,0x6174656d,0x6c646f6d,0x6c656b73,0x656e6f62,0x6d696e61,0x76727563,0x6669746e};
            require(allowed.contains(id),"Unexpected node in animation reuse check");
            r.skip(4);auto at=r.pos;r.skip(8);std::fill(bytes.begin()+at,bytes.begin()+at+8,0);
            auto np=r.get<uint32_t>(),nc=r.get<uint32_t>();
            for(uint32_t i=0;i<np;++i){auto raw=r.take(2);std::string type;for(auto c:raw)if(c)type+=char(c);auto length=r.get<uint16_t>();auto count=r.get<uint32_t>();r.skip(length);
                if(type=="s")r.cstr();else{static const std::map<std::string,size_t> widths={{"b",1},{"h",2},{"i",4},{"l",8},{"f",4},{"d",8},{"2v",8},{"3v",12},{"4v",16}};r.skip(widths.at(type)*count);}
            }
            for(uint32_t i=0;i<nc;++i)node();
        };for(uint32_t i=0;i<roots;++i)node();return bytes;
    };
    return normalized(a)==normalized(b);
}
Bytes encode_animation_cast(const Model &model, const Animation &anim, const std::string &stem) {
    require(anim.frames > 0 && !anim.animated.empty(), "Animation has no bound tracks");
    CastNode root(0x746f6f72, 0x636f646d616e696dull);
    auto &meta = root.add(0x6174656d);
    meta.string("up", "z");
    meta.string("s", "CODM CAST Studio native animation");
    if (model.report.value("cameraTarget", false)) {
        auto &target = root.add(0x6c646f6d);
        target.string("n", stem + "_camera_target");
        auto &skeleton = target.add(0x6c656b73);
        for (auto &b : model.bones) {
            auto &bone = skeleton.add(0x656e6f62);
            bone.string("n", b.name);
            bone.scalar<int32_t>("p", "i", b.parent);
            std::vector<float> p = {float(b.position.x), float(b.position.y), float(b.position.z)},
                               q = {float(b.rotation.x), float(b.rotation.y), float(b.rotation.z),
                                    float(b.rotation.w)},
                               s = {float(b.scale.x), float(b.scale.y), float(b.scale.z)};
            bone.array<float>("lp", "3v", p, 3);
            bone.array<float>("lr", "4v", q, 4);
            bone.array<float>("s", "3v", s, 3);
        }
    }
    auto &out = root.add(0x6d696e61);
    auto category = model.report.value("source", J::object()).value("category", std::string());
    // Cadence recognizes these domains from CAST animation names. Keep the
    // user-chosen filename while identifying character motion unambiguously.
    auto consumerName = (category == "Player" ? "pb_" : category == "Viewhands" ? "vm_" : "") + stem;
    if (category == "Etc" && !model.report.value("cameraTarget", false)) {
        consumerName = stem;
        std::replace(consumerName.begin(), consumerName.end(), '_', ' ');
        consumerName += " (prop motion)";
    }
    out.string("n", consumerName);
    out.scalar<float>("fr", "f", float(anim.fps));
    out.scalar<uint8_t>("lo", "b", anim.looping ? 1 : 0);
    std::vector<uint32_t> frames(anim.frames);
    for (int f = 0; f < anim.frames; f++)
        frames[f] = f;
    for (auto [index, attr] : anim.animated) {
        require(index >= 0 && size_t(index) < model.bones.size(), "Curve targets absent bone");
        auto &bone = model.bones[index];
        for (int axis = 0; axis < (attr == 2 ? 1 : 3); axis++) {
            std::string prop = attr == 2 ? "rq" : std::string(attr == 1 ? "t" : "s") + "xyz"[axis];
            auto &c = out.add(0x76727563);
            c.string("nn", bone.name);
            c.string("kp", prop);
            bool relative = attr == 1 && anim.relativeTranslation.contains(index);
            c.string("m", relative ? "relative" : "absolute");
            c.array<uint32_t>("kb", "i", frames);
            std::vector<float> values;
            values.reserve(size_t(anim.frames) * (attr == 2 ? 4 : 1));
            for (int f = 0; f < anim.frames; f++) {
                auto &pose = anim.poses.at(f).at(index);
                if (attr == 2) {
                    for (double v :
                         {pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w})
                        values.push_back(float(v));
                } else
                    values.push_back(
                        float(attr == 1 ? pose.position[axis] - (relative ? bone.position[axis] : 0)
                                        : pose.scale[axis]));
            }
            c.array<float>("kv", attr == 2 ? "4v" : "f", values, attr == 2 ? 4 : 1);
        }
    }
    std::map<std::string, std::vector<uint32_t>> notifications;
    for (auto &e : anim.events) {
        std::string name = e.value("functionName", std::string());
        if (name.empty())
            continue;
        double time = e.value("time", 0.0);
        require(std::isfinite(time) && time >= 0, "Invalid event time");
        int frame = int(std::lround(time * anim.fps));
        require(frame <= anim.frames, "Event outside clip time range");
        notifications[name].push_back(uint32_t(std::min(frame, anim.frames - 1)));
    }
    for (auto &[name, keys] : notifications) {
        auto &n = out.add(0x6669746e);
        n.string("n", name);
        n.array<uint32_t>("kb", "i", keys);
    }
    Bytes bytes(16);
    uint32_t header[] = {0x74736163, 1, 1, 0};
    std::memcpy(bytes.data(), header, 16);
    root.write(bytes);
    validate_cast(bytes);
    return bytes;
}
J export_animation(const Model &model, const Animation &animation, const fs::path &directory,
                   const std::string &stem, JobContext *job, bool reuseIdentical) {
    require(!stem.empty() && stem.find_first_of("\\/:*?\"<>|") == stem.npos && stem != "." &&
                stem != ".." && stem.back() != '.' && stem.back() != ' ',
            "Invalid animation filename");
    fs::create_directories(directory);
    auto dest = directory / (stem + ".cast"), report = directory / (stem + ".json");
    auto bytes = encode_animation_cast(model, animation, stem);
    if (reuseIdentical && fs::exists(dest) && fs::exists(report)) {
        require(equivalent_animation_cast(read_bytes(dest),bytes), "Existing animation differs from this rig or source: " + pathstr(dest));
        auto previous = read_json(report);
        require(previous.value("status", std::string()) == "exported",
                "Existing animation has no successful export report: " + pathstr(dest));
        previous["status"] = "existing";
        previous["reuseEvidence"] = "Animation payload is identical after normalizing file-local node IDs; file retained without rewriting";
        return previous;
    }
    require(!fs::exists(dest) && !fs::exists(report), "Animation output exists: " + pathstr(dest));
    auto stage = directory / (".codm-clip-" + hex64(uint64_t(Clock::now().time_since_epoch().count())));
    require(fs::create_directory(stage), "Cannot stage animation");
    bool reportPublished = false;
    try {
        if (job)
            job->check();
        std::ofstream f(stage / (stem + ".cast"), std::ios::binary);
        f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        f.close();
        require(bool(f), "Animation write failed");
        J result = animation.report;
        result["status"] = "exported";
        result["path"] = pathstr(dest);
        result["name"] = stem;
        result["tracks"] = animation.animated.size();
        result["targetModel"] = model.name;
        result["t6Compatible"] = model.report.value("t6Compatible", false);
        if (model.report.value("cameraTarget", false)) {
            result["targetKind"] = "camera";
            result["cameraOwnership"] = model.report.at("cameraOwnership");
        }
        write_json(stage / (stem + ".json"), result);
        if (job)
            job->check();
        // The binary is the final commit marker, so interrupted work cannot look like a valid export.
        fs::rename(stage / (stem + ".json"), report);
        reportPublished = true;
        fs::rename(stage / (stem + ".cast"), dest);
        fs::remove(stage);
        return result;
    } catch (...) {
        std::error_code ec;
        remove_owned_tree(stage, directory);
        if (reportPublished && !fs::exists(dest))
            fs::remove(report, ec);
        throw;
    }
}
} // namespace codm
