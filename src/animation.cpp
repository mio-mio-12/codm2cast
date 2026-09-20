#include "animation.hpp"
#include "materials.hpp"
#include <limits>
#include <regex>
namespace codm {
static constexpr double quantMax = 65533.0;
static const TypeNode &child(const TypeNode &node, const std::string &name) {
    for (auto &c : node.children)
        if (c.name == name)
            return c;
    throw std::runtime_error("Missing animation schema field: " + name);
}
static double finite_float(Reader &r) {
    double v = r.get<float>();
    require(std::isfinite(v), "Nonfinite animation value at byte " + std::to_string(r.pos - 4));
    return v;
}
struct StreamKey {
    double time;
    std::array<double, 4> coefficients;
};
static std::vector<double> streamed_samples(const J &clip, int frames, double fps) {
    size_t count = clip.at("curveCount");
    require(count < 100000, "Excess streamed curves");
    auto words = clip.at("data").get<std::vector<uint32_t>>();
    Reader r(std::span(reinterpret_cast<const uint8_t *>(words.data()), words.size() * 4));
    std::vector<std::vector<StreamKey>> tracks(count);
    bool terminal = !count && words.empty();
    double last = -INFINITY;
    while (r.pos < r.data.size()) {
        double time = r.get<float>();
        auto keys = r.count(20, count);
        if (time == INFINITY && keys == 0) {
            terminal = true;
            break;
        }
        require(std::isfinite(time) && time >= last, "Invalid streamed time");
        last = time;
        std::set<int> seen;
        for (int k = 0; k < keys; k++) {
            int index = r.get<uint32_t>();
            require(index >= 0 && size_t(index) < count && seen.insert(index).second,
                    "Invalid streamed key index");
            StreamKey key{time, {}};
            for (auto &v : key.coefficients)
                v = finite_float(r);
            tracks[index].push_back(key);
        }
    }
    require(terminal && r.pos == r.data.size(), "Missing streamed terminal marker");
    std::vector<double> out(size_t(frames) * count);
    for (size_t i = 0; i < count; i++) {
        auto &track = tracks[i];
        require(!track.empty() && track[0].time <= 0, "Missing initial streamed key");
        size_t k = 0;
        for (int f = 0; f < frames; f++) {
            double t = f / fps;
            while (k + 1 < track.size() && track[k + 1].time <= t)
                k++;
            auto &key = track[k];
            double dt = key.time < -1e10 ? 0 : t - key.time;
            auto &a = key.coefficients;
            out[size_t(f) * count + i] = ((a[0] * dt + a[1]) * dt + a[2]) * dt + a[3];
        }
    }
    return out;
}
Clip decode_clip(Source &source, Object &object, JobContext *job, bool metadataOnly) {
    require(object.cid == 74, "Selected asset is not an AnimationClip");
    Reader r(object.raw(), object.file->big);
    auto &schema = source.types.at(74);
    J header = J::object();
    for (auto &c : schema.children) {
        if (c.name == "m_MuscleClip")
            break;
        header[c.name] = parse_tree(r, c);
    }
    if (header.at("m_Legacy").get<bool>() && header.at("m_MuscleClipSize") == 0) {
        // This exact empty CODM muscle template was observed in independently
        // parsed legacy clips. Never skip an unknown tail based on its size.
        auto emptyMuscle = r.take(1000);
        require(sha256(emptyMuscle) ==
                    "b81cf8d6741fec1708465c10e14ce8541b961fda03b12399f0dcd6438b3bbff8",
                "Unrecognized legacy CODM muscle template");
        auto bindings = parse_tree(r, child(schema, "m_ClipBindingConstant"));
        auto events = parse_tree(r, child(schema, "m_Events"));
        require(bindings.at("genericBindings").empty() && bindings.at("pptrCurveMapping").empty(),
                "Legacy clip also contains muscle bindings");
        require(r.pos == r.data.size(), "Unconsumed legacy animation bytes");
        auto out = sample_legacy_curves(header, job);
        out.id = object.id();
        out.events = events;
        out.report["source"] = out.id;
        out.report["sourceSHA256"] = sha256(object.raw());
        out.report["events"] = events;
        return out;
    }
    for (auto key : {"m_RotationCurves", "m_CompressedRotationCurves", "m_EulerCurves",
                     "m_PositionCurves", "m_ScaleCurves", "m_FloatCurves", "m_PPtrCurves"})
        require(header.at(key).empty(),
                std::string("Non-muscle animation curves require another adapter: ") + key);
    J prefix = J::object();
    for (auto &c : child(schema, "m_MuscleClip").children) {
        if (c.name == "m_DeltaPose" || c.name == "m_LeftFootStartX" ||
            c.name == "m_RightFootStartX")
            continue;
        prefix[c.name] = parse_tree(r, c);
        if (c.name == "m_Clip")
            break;
    }
    auto &core = prefix.at("m_Clip").at("data");
    auto constants = core.at("m_ConstantClip").at("data").get<std::vector<double>>();
    static constexpr uint32_t observed[13] = {0, 0, 4, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0};
    for (auto v : observed)
        require(r.get<uint32_t>() == v, "Unsupported CODM quantized clip prefix");
    size_t words = r.count(4);
    auto sparseBytes = r.take(words * 4);
    size_t sparseCount = r.get<uint32_t>();
    require(sparseCount < 100000, "Excess sparse curves");
    struct SparseRaw {
        uint16_t time, index, tangent, value;
    };
    std::vector<SparseRaw> sparseKeys;
    Reader sr(sparseBytes);
    bool terminal = words == 0;
    int lastCode = -1;
    while (sr.pos < sr.data.size()) {
        auto time = sr.get<uint16_t>(), count = sr.get<uint16_t>();
        if (time == 65535 && count == 0) {
            terminal = true;
            break;
        }
        require(time < 65535 && time >= lastCode && count <= sparseCount, "Invalid sparse frame");
        lastCode = time;
        std::set<int> seen;
        for (int i = 0; i < count; i++) {
            auto index = sr.get<uint16_t>(), tangent = sr.get<uint16_t>(),
                 value = sr.get<uint16_t>();
            require(index < sparseCount && seen.insert(index).second && tangent <= quantMax &&
                        value <= quantMax,
                    "Invalid sparse key or reserved value code");
            sparseKeys.push_back({time, index, tangent, value});
        }
    }
    auto padding = sr.take(sr.data.size() - sr.pos);
    require(terminal && padding.size() <= 3 &&
                std::all_of(padding.begin(), padding.end(), [](auto v) { return v == 0; }),
            "Invalid sparse padding");
    auto bounds = [&]() {
        std::array<double, 3> out;
        out[0] = finite_float(r);
        require(r.get<uint32_t>() == 2, "Unsupported sparse bounds");
        out[1] = finite_float(r);
        out[2] = finite_float(r);
        return out;
    };
    auto low = bounds(), span = bounds();
    for (auto v : span)
        require(v >= 0, "Negative sparse span");
    int frameCount = r.get<uint32_t>();
    size_t quantCount = r.get<uint32_t>();
    double fps = finite_float(r), begin = finite_float(r);
    size_t streamCount = core.at("m_StreamedClip").at("curveCount"),
           standardCount = core.at("m_DenseClip").at("m_CurveCount");
    size_t total = streamCount + standardCount + constants.size() + sparseCount + quantCount;
    const bool emptyPlaceholder = total == 0 && frameCount == 0 && fps == 0 && begin == 0;
    require(emptyPlaceholder ||
                (frameCount >= 1 && frameCount <= 100000 && fps >= 1 && fps <= 240 && begin == 0),
            "Invalid animation timing");
    require((total > 0 || emptyPlaceholder) && total <= 100000 &&
                (metadataOnly || size_t(frameCount) * total <= 20000000),
            "Animation exceeds sample memory limit");
    std::array<std::vector<double>, 3> quant;
    std::array<size_t, 3> quantCounts{};
    J emptyQuantizedGroups = J::array();
    for (int group = 0; group < 3; group++) {
        size_t count = r.get<uint32_t>();
        double minimum = r.get<float>(), range = r.get<float>();
        auto kind = r.get<uint32_t>();
        size_t nwords = r.count(4);
        auto bytes = r.take(nwords * 4);
        if (emptyPlaceholder) {
            require(count == 0 && minimum == 0 && range == 0 && kind == 0 && nwords == 0,
                    "Nonempty quantized data in empty animation placeholder");
            continue;
        }
        bool emptySentinel = count == 0 && nwords == 0 &&
                             minimum == double(std::numeric_limits<float>::max()) &&
                             range == -INFINITY;
        require(
            kind == 3 && count % ((group == 1) ? 4 : 3) == 0 && count <= quantCount &&
                (emptySentinel || (std::isfinite(minimum) && std::isfinite(range) && range >= 0)),
            "Unsupported dense quantization");
        if (emptySentinel) {
            emptyQuantizedGroups.push_back(group);
            minimum = range = 0;
        }
        size_t needed = size_t(frameCount) * count * 2;
        // The source stores SIMD-aligned arrays: an odd final vector may add six
        // zero bytes.
        require(bytes.size() >= needed && bytes.size() <= needed + 7,
                "Dense sample byte count mismatch");
        require(std::all_of(bytes.begin() + needed, bytes.end(), [](auto v) { return v == 0; }),
                "Nonzero dense padding");
        quantCounts[group] = count;
        if (metadataOnly)
            continue;
        Reader qr(bytes.first(needed));
        auto &samples = quant[group];
        samples.resize(size_t(frameCount) * count);
        for (auto &v : samples) {
            auto q = qr.get<uint16_t>();
            require(q <= quantMax, "Reserved dense value code");
            v = minimum + q / quantMax * range;
        }
    }
    require(quantCounts[0] + quantCounts[1] + quantCounts[2] == quantCount &&
                r.get<uint32_t>() == 0,
            "Dense totals disagree");
    double start = finite_float(r), stop = finite_float(r);
    for (int i = 0; i < 4; i++)
        finite_float(r);
    require(start == 0 && stop >= 0 &&
                (emptyPlaceholder ? stop <= 86400 : stop < double(frameCount) / fps),
            "Invalid clip range");
    r.skip(size_t(r.count(4)) * 4);
    size_t deltaCount = r.count(4);
    r.skip(deltaCount * 4);
    finite_float(r);
    finite_float(r);
    size_t referenceCount = r.count(4);
    require(referenceCount == 0 || referenceCount == total,
            "Reference pose scalar count differs from clip bindings");
    std::vector<double> referencePose(referenceCount);
    for (auto &v : referencePose)
        v = finite_float(r);
    auto flags = r.take(11);
    require(std::all_of(flags.begin(), flags.end(), [](auto v) { return v <= 1; }),
            "Invalid animation flags");
    bool looping = flags[1] != 0;
    r.align();
    auto bindingsTree = parse_tree(r, child(schema, "m_ClipBindingConstant"));
    auto events = parse_tree(r, child(schema, "m_Events"));
    require(r.pos == r.data.size(), "Unconsumed animation bytes");
    if (emptyPlaceholder) {
        require(core.at("m_StreamedClip").at("data").empty() &&
                    core.at("m_DenseClip").at("m_SampleArray").empty() && sparseKeys.empty() &&
                    deltaCount == 0 && referenceCount == 0 &&
                    bindingsTree.at("genericBindings").empty() &&
                    bindingsTree.at("pptrCurveMapping").empty() && events.empty(),
                "Empty animation placeholder contains payload or events");
        Clip empty;
        empty.id = object.id();
        empty.name = header.at("m_Name");
        empty.fps = header.at("m_SampleRate");
        require(std::isfinite(empty.fps) && empty.fps >= 1 && empty.fps <= 240,
                "Invalid empty animation sample rate");
        empty.events = events;
        empty.report = {{"emptyPlaceholder", true},
                        {"source", empty.id},
                        {"sourceSHA256", sha256(object.raw())},
                        {"sourceStopTime", stop},
                        {"reason", "Source placeholder contains no curves or events"}};
        return empty;
    }
    Clip out;
    out.id = object.id();
    out.name = header.at("m_Name");
    out.fps = fps;
    out.frames = std::min(frameCount, int(std::lround(stop * fps)) + 1);
    out.columns = int(total);
    out.looping = looping;
    out.events = events;
    size_t offset = 0;
    J nonTransform = J::array();
    int eulerCount = 0;
    for (auto &b : bindingsTree.at("genericBindings")) {
        Binding binding;
        binding.path = b.at("path");
        binding.attribute = b.at("attribute");
        binding.type = b.at("typeID");
        binding.custom = b.at("customType");
        binding.offset = offset;
        require(b.at("isPPtrCurve").get<int>() == 0,
                "Object-reference animation needs a separate target adapter");
        if (binding.type == 4) {
            require(binding.attribute >= 1 && binding.attribute <= 4,
                    "Unknown transform animation attribute");
            require(binding.custom == 0 ||
                        (binding.attribute == 4 &&
                         ((binding.custom >= 0 && binding.custom <= 5) || binding.custom == 10)),
                    "Unknown transform curve encoding");
            binding.width = binding.attribute == 2 ? 4 : 3;
            if (binding.attribute == 4) {
                // Ordinary Unity bindings store RotationOrder directly. CODM's
                // encoding 10 uses XYZ: source Arctic multi-axis clavicle/finger
                // tracks reproduce the independently serialized avatar quaternions.
                // See validation/native-euler-v2.json; do not infer order from framing.
                binding.eulerOrder = binding.custom == 10 ? 0 : binding.custom;
                eulerCount++;
            }
        } else
            nonTransform.push_back(b);
        offset += binding.width;
        out.bindings.push_back(binding);
    }
    require(offset == total && deltaCount == total,
            "Animation scalar totals disagree with bindings");
    if (metadataOnly)
        return out;
    auto streamed = streamed_samples(core.at("m_StreamedClip"), frameCount, fps);
    auto &sd = core.at("m_DenseClip");
    int standardFrames = sd.at("m_FrameCount");
    double standardFps = sd.at("m_SampleRate"), standardBegin = sd.at("m_BeginTime");
    auto standard = sd.at("m_SampleArray").get<std::vector<double>>();
    if (standardCount)
        require(standardFrames >= 1 && standardFps > 0 &&
                    standard.size() == size_t(standardFrames) * standardCount,
                "Invalid standard dense samples");
    struct SparseKey {
        double t, slope, value;
    };
    std::vector<std::vector<SparseKey>> tracks(sparseCount);
    std::set<size_t> snapshots;
    std::vector<SparseKey> initial(sparseCount);
    for (auto &key : sparseKeys) {
        if (key.time == 0) {
            require(snapshots.insert(key.index).second, "Duplicate sparse snapshot");
            initial[key.index]={0,low[1]+key.tangent/quantMax*span[1],low[2]+key.value/quantMax*span[2]};
            continue;
        }
        auto &track = tracks[key.index];
        double t = low[0] + (key.time - 1) / quantMax * span[0];
        require(track.empty() || t > track.back().t, "Duplicate sparse time");
        track.push_back({t, low[1] + key.tangent / quantMax * span[1],
                         low[2] + key.value / quantMax * span[2]});
    }
    require(snapshots.size() == sparseCount, "Missing sparse snapshot");
    // Time-code zero stores the initial snapshot. Finite keys can begin later
    // (including a few microseconds after zero); do not require a second key
    // at zero when the source already provides the initial value and slope.
    for(size_t i=0;i<tracks.size();++i) {
        auto &track=tracks[i];
        require(track.empty() || track.front().t>=0,"Sparse key precedes clip start");
        if(track.empty() || track.front().t>0)track.insert(track.begin(),initial[i]);
    }
    std::vector<size_t> active(sparseCount, 0);
    out.samples.resize(size_t(out.frames) * total);
    for (int f = 0; f < out.frames; f++) {
        if (job && f % 32 == 0)
            job->check();
        size_t c = 0;
        double t = f / fps;
        auto put = [&](double v) {
            require(std::isfinite(v), "Nonfinite decoded motion");
            out.samples[size_t(f) * total + c++] = v;
        };
        for (size_t i = 0; i < streamCount; i++)
            put(streamed[size_t(f) * streamCount + i]);
        double sf = std::clamp((t - standardBegin) * standardFps, 0.0,
                               double(std::max(0, standardFrames - 1)));
        int a = int(sf), b = std::min(a + 1, standardFrames - 1);
        double blend = sf - a;
        for (size_t i = 0; i < standardCount; i++)
            put(std::lerp(standard[size_t(a) * standardCount + i],
                          standard[size_t(b) * standardCount + i], blend));
        for (auto v : constants)
            put(v);
        for (size_t i = 0; i < sparseCount; i++) {
            auto &track = tracks[i];
            auto &k = active[i];
            while (k + 1 < track.size() && track[k + 1].t <= t)
                k++;
            auto &key = track[k];
            put(key.value + std::max(0.0, std::min(t, track.back().t) - key.t) * key.slope);
        }
        for (int g = 0; g < 3; g++)
            for (size_t i = 0; i < quantCounts[g]; i++)
                put(quant[g][size_t(f) * quantCounts[g] + i]);
        require(c == total, "Incomplete animation frame");
    }
    double maxQuaternionError = 0;
    for (auto &b : out.bindings)
        if (b.type == 4 && b.attribute == 2)
            for (int f = 0; f < out.frames; f++) {
                auto *q = out.samples.data() + size_t(f) * total + b.offset;
                double norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                maxQuaternionError = std::max(maxQuaternionError, std::abs(norm - 1));
                require(std::abs(norm - 1) < .003, "Decoded rotation is not a unit quaternion");
                for (int i = 0; i < 4; i++)
                    q[i] /= norm;
            }
    out.report = {{"source", out.id},
                  {"sourceSHA256", sha256(object.raw())},
                  {"sourceName", out.name},
                  {"fps", fps},
                  {"frames", out.frames},
                  {"looping", looping},
                  {"standardDenseCurves", standardCount},
                  {"streamedCurves", streamCount},
                  {"sparseCurves", sparseCount},
                  {"quantizedDenseCurves", quantCount},
                  {"emptyQuantizedSentinelGroups", emptyQuantizedGroups},
                  {"eulerBindings", eulerCount},
                  {"nonTransformBindings", nonTransform},
                  {"events", events},
                  {"maximumQuaternionError", maxQuaternionError},
                  {"sourceAnimation", true}};
    out.report["referencePose"] = referencePose;
    out.report["constantCurves"] = constants.size();
    out.report["nativeDecoderVersion"] = 2;
    out.report["eulerPolicy"] = "Serialized Unity RotationOrder; CODM customType 10 is XYZ, "
                                "verified against authored avatar quaternion landmarks";
    out.report["scaleRanges"] = J::array();
    for (auto &b : out.bindings)
        if (b.type == 4 && b.attribute == 3) {
            J ranges = J::array();
            for (int k = 0; k < 3; k++) {
                double lo = INFINITY, hi = -INFINITY;
                for (int f = 0; f < out.frames; f++) {
                    auto v = out.at(f, b.offset + k);
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                ranges.push_back({lo, hi});
            }
            out.report["scaleRanges"].push_back({{"path", b.path}, {"range", ranges}});
        }
    out.report["bindings"] = J::array();
    for (auto &b : out.bindings)
        out.report["bindings"].push_back({{"path", b.path},
                                          {"attribute", b.attribute},
                                          {"type", b.type},
                                          {"customType", b.custom},
                                          {"eulerOrder", b.attribute == 4 ? J(b.eulerOrder) : J()},
                                          {"offset", b.offset},
                                          {"width", b.width}});
    return out;
}
M4 pose_matrix(const Pose &p) {
    return glm::translate(M4(1), p.position) * glm::mat4_cast(p.rotation) *
           glm::scale(M4(1), p.scale);
}
std::vector<M4> pose_worlds(const Model &model, const std::vector<Pose> &pose) {
    require(pose.size() == model.bones.size(), "Pose and skeleton sizes differ");
    std::vector<M4> worlds(pose.size());
    for (size_t i = 0; i < pose.size(); i++) {
        worlds[i] = pose_matrix(pose[i]);
        int parent = model.bones[i].parent;
        require(parent < int(i) && parent >= -1, "Invalid animation hierarchy");
        if (parent >= 0)
            worlds[i] = worlds[parent] * worlds[i];
    }
    return worlds;
}
Animation bind_animation(const Model &model, const Clip &clip, bool cameraTarget) {
    bool camera = !camera_base_name(clip.name).empty();
    require(!camera || cameraTarget,
            "Camera clip needs its camera target, not the selected gun skeleton");
    require(size_t(clip.frames) * model.bones.size() <= 2000000,
            "Animation pose memory limit exceeded");
    Animation out;
    out.name = clip.name;
    out.fps = clip.fps;
    out.frames = clip.frames;
    out.looping = clip.looping;
    out.events = clip.events;
    out.report = clip.report;
    out.report["nativeDecoderVersion"] = 2;
    std::vector<Pose> rest;
    std::map<uint32_t, std::set<int>> lookup;
    for (size_t i = 0; i < model.bones.size(); i++) {
        auto &b = model.bones[i];
        rest.push_back({b.position, b.scale, b.rotation});
        for (auto path : b.paths)
            lookup[path].insert(int(i));
    }
    out.poses.assign(clip.frames, rest);
    J matched = J::array(), omitted = J::array();
    std::set<std::pair<int, int>> occupied;
    auto basis = glm::dmat3(native_basis());
    auto paths =
        model.report.value("animationHierarchy", J::object()).value("resolvedPaths", J::object());
    for (auto &binding : clip.bindings) {
        J description = {{"path", binding.path},
                         {"attribute", binding.attribute},
                         {"type", binding.type},
                         {"customType", binding.custom}};
        if (binding.attribute == 4)
            description["eulerOrder"] = binding.eulerOrder;
        auto key = std::to_string(binding.path);
        if (paths.contains(key))
            description["sourcePath"] = paths.at(key);
        else if (!binding.pathName.empty())
            description["sourcePath"] = binding.pathName;
        if (binding.type != 4) {
            description["reason"] = "non-transform target";
            omitted.push_back(description);
            continue;
        }
        auto found = lookup.find(binding.path);
        if (found == lookup.end()) {
            description["reason"] = "absent from selected target";
            omitted.push_back(description);
            continue;
        }
        require(found->second.size() == 1,
                "Ambiguous animation path hash: " + std::to_string(binding.path));
        int bone = *found->second.begin();
        int attr = binding.attribute == 4 ? 2 : binding.attribute;
        require(occupied.insert({bone, attr}).second,
                "Conflicting animation bindings for " + model.bones[bone].name);
        out.animated.insert({bone, attr});
        for (int f = 0; f < clip.frames; f++) {
            auto &p = out.poses[f][bone];
            auto at = [&](int i) { return clip.at(f, binding.offset + i); };
            if (attr == 1)
                p.position = basis * DV3(at(0), at(1), at(2));
            else if (attr == 3) {
                p.scale = {at(0), at(2), at(1)};
                require(std::isfinite(p.scale.x) && std::isfinite(p.scale.y) && std::isfinite(p.scale.z),"Animation has nonfinite scale");
            } else {
                Q q;
                if (binding.attribute == 4) {
                    DV3 e = glm::radians(DV3(at(0), at(1), at(2)));
                    static constexpr int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 2, 0},
                                                         {1, 0, 2}, {2, 0, 1}, {2, 1, 0}};
                    require(binding.eulerOrder >= 0 && binding.eulerOrder < 6,
                            "Unknown Euler rotation order");
                    q = Q(1, 0, 0, 0);
                    for (int axis : orders[binding.eulerOrder]) {
                        DV3 direction(0);
                        direction[axis] = 1;
                        q = glm::angleAxis(e[axis], direction) * q;
                    }
                } else
                    q = Q(at(3), at(0), at(1), at(2));
                DV3 v = -basis * DV3(q.x, q.y, q.z);
                p.rotation = glm::normalize(Q(q.w, v.x, v.y, v.z));
                if (f && glm::dot(out.poses[f - 1][bone].rotation, p.rotation) < 0)
                    p.rotation = -p.rotation;
            }
        }
        description["bone"] = model.bones[bone].name;
        description["sourceHash"] = binding.path;
        matched.push_back(description);
    }
    require(!matched.empty(), "No clip tracks bind to the selected model");
    for (size_t i = 0; i < model.bones.size(); ++i)
        if (model.bones[i].sourceBindAlias) {
            for (auto &frame : out.poses)
                frame[i] = {DV3(0), DV3(1), Q(1, 0, 0, 0)};
            for (int attr : {1, 2, 3})
                out.animated.insert({int(i), attr});
        }
    if (model.report.contains("sourceBindAliases"))
        out.report["sourceBindAliases"] = model.report.at("sourceBindAliases");
    out.report["matchedBindings"] = matched;
    out.report["omittedBindings"] = omitted;
    return out;
}
Model camera_target(Source &source, const Clip &clip) {
    auto lowerName = lower(clip.name);
    require(!camera_base_name(clip.name).empty(),
            "Not a labeled source camera clip");
    require(!clip.bindings.empty(), "Camera has no bindings");
    bool rotation = false, translation = false;
    for (auto &b : clip.bindings) {
        require(b.type == 4 && b.path == 0,
                "Camera target must have only authored root-transform bindings");
        rotation |= b.attribute == 2 || b.attribute == 4;
        translation |= b.attribute == 1;
    }
    require(rotation && translation, "Camera clip must declare both root translation and rotation");
    auto &file = *source.object(clip.id).file;
    Object *root = nullptr;
    for (auto &[pid, o] : file.objects)
        if (o.cid == 4) {
            auto &tree = source.tree(o);
            auto go = source.ref(o, tree.at("m_GameObject"));
            if (go && source.name(*go) == clip.name && !source.ref(o, tree.at("m_Father"))) {
                require(!root, "Camera root name is ambiguous");
                root = &o;
            }
        }
    Model model;
    model.name = clip.name + "_target";
    Bone bone;
    bone.id = root?root->id():clip.id+":camera";
    bone.name = "tag_camera";
    bone.paths = {0};
    auto basis = native_basis();
    // A root-only clip with complete translation and rotation is self-contained.
    // Some shared clips have no separately serialized camera prefab.
    bone.world = root?basis * transform_matrix(source.tree(*root)) * glm::transpose(basis):M4(1);
    bone.inverseBind = glm::inverse(bone.world);
    decompose(bone.world, bone.position, bone.rotation, bone.scale);
    model.bones.push_back(bone);
    model.report = {{"cameraTarget", true},
                    {"rigidGeometry", true},
                    {"source", {{"category", "Etc"}}},
                    {"cameraOwnership",
                     {{"transform", root?J(root->id()):J(nullptr)},
                      {"bindingPath", 0},
                      {"evidence",root?"Matching authored camera root and exclusively root-transform curves":"Explicit camera clip with complete root translation and rotation; standalone camera target"}}},
                    {"units", "metres"},
                    {"limitations",
                     J::array({"Camera transform motion only. No FOV or runtime camera-controller "
                               "behavior is inferred."})}};
    return model;
}
std::vector<M4> sample_palette(const Model &model, const Animation &anim, double frame) {
    require(anim.frames > 0, "Empty animation");
    frame = std::clamp(frame, 0.0, double(anim.frames - 1));
    int a = int(frame), b = std::min(a + 1, anim.frames - 1);
    double t = frame - a;
    auto pose = anim.poses[a];
    for (size_t i = 0; i < pose.size(); i++) {
        pose[i].position = glm::mix(pose[i].position, anim.poses[b][i].position, t);
        pose[i].scale = glm::mix(pose[i].scale, anim.poses[b][i].scale, t);
        pose[i].rotation = glm::slerp(pose[i].rotation, anim.poses[b][i].rotation, t);
    }
    auto worlds = pose_worlds(model, pose);
    for (size_t i = 0; i < worlds.size(); i++)
        worlds[i] *= model.bones[i].inverseBind;
    return worlds;
}
std::array<DV3, 2> animation_preview_bounds(const Model &model, const Animation &anim,
                                            JobContext *job) {
    require(anim.frames > 0 && size_t(anim.frames) == anim.poses.size(),
            "Invalid preview animation");
    using Box = std::array<DV3, 2>;
    const Box empty{DV3(INFINITY), DV3(-INFINITY)};
    std::vector<Box> boneBounds(model.bones.size(), empty);
    std::vector<bool> used(model.bones.size());
    Box result = empty;
    auto include = [](Box &box, DV3 point) {
        for (int k = 0; k < 3; k++)
            require(std::isfinite(point[k]), "Nonfinite animated preview extent");
        box[0] = glm::min(box[0], point);
        box[1] = glm::max(box[1], point);
    };
    for (const auto &surface : model.surfaces) {
        if (job)
            job->check();
        for (size_t i = 0; i < surface.positions.size(); i++) {
            const DV3 point(surface.positions[i]);
            if (surface.weights.empty()) {
                include(result, point);
                continue;
            }
            for (int k = 0; k < 4; k++) {
                if (surface.weights.at(i)[k] <= 0)
                    continue;
                auto bone = surface.joints.at(i)[k];
                require(bone < boneBounds.size(), "Preview weight targets an absent bone");
                include(boneBounds[bone], point);
                used[bone] = true;
            }
        }
    }
    // Positive skin weights form a convex combination of their transformed
    // influence points. Their per-bone boxes bound every skinned vertex at each
    // sample without reskinning the full mesh. Include intermediate poses too.
    for (int sample = 0; sample < anim.frames * 2 - 1; sample++) {
        if (job && sample % 16 == 0)
            job->check();
        auto palette = sample_palette(model, anim, sample * .5);
        for (size_t bone = 0; bone < boneBounds.size(); bone++) {
            if (!used[bone])
                continue;
            for (int corner = 0; corner < 8; corner++) {
                DV3 point;
                for (int k = 0; k < 3; k++)
                    point[k] = boneBounds[bone][(corner >> k) & 1][k];
                include(result, DV3(palette.at(bone) * glm::dvec4(point, 1)));
            }
        }
    }
    require(std::isfinite(result[0].x) && std::isfinite(result[1].x),
            "Empty animated preview bounds");
    return result;
}
std::string camera_base_name(const std::string &sourceName) {
    static const std::regex suffix("_(?:camera|camra)(?:_?(start|loop|end))?$",std::regex::icase);
    std::smatch match;if(!std::regex_search(sourceName,match,suffix))return {};
    return sourceName.substr(0,size_t(match.position()))+(match[1].matched?match[1].str():std::string());
}
std::string animation_action(const J &clip) {
    return clip.value("controllerAction", action_name(clip.at("name")));
}
std::string action_name(const std::string &sourceName) {
    if(auto base=camera_base_name(sourceName);!base.empty())return action_name(base)+"_camera";
    std::string s = sourceName;
    auto at = lower(s).find("_m_");
    if (at != s.npos)
        s = s.substr(at + 3);
    else {
        std::smatch perspective;
        static const std::regex marker("(^|_)(?:adv|advance)?(1p|3p|pov)_", std::regex::icase);
        if (std::regex_search(s, perspective, marker))
            s = s.substr(size_t(perspective.position() + perspective.length()));
    }
    bool camera = false;
    for (auto suffix : {std::string("_camera"), std::string("_camra")})
        if (lower(s).ends_with(suffix)) {
            camera = true;
            s.resize(s.size() - suffix.size());
        }
    auto k = lower(s);
    static const std::map<std::string, std::string> actions = {{"idle", "idle"},
                                                               {"idlepose", "idle"},
                                                               {"weaponchangeclip", "reload"},
                                                               {"weaponchangeclip_e", "reload_empty"},
                                                               {"weaponquickchangeclip", "reload_quick"},
                                                               {"weaponquickchangeclip_e", "reload_quick_empty"},
                                                               {"unaimingon", "ads_down"},
                                                               {"unaimingidle", "idle"},
                                                               {"unaimingfire", "fire"},
                                                               {"fire", "fire"},
                                                               {"aimingfire", "ads_fire"},
                                                               {"changeclip", "reload"},
                                                               {"changeclip_e", "reload_empty"},
                                                               {"aimingon", "ads_up"},
                                                               {"aimingoff", "ads_down"},
                                                               {"unaiming", "ads_down"},
                                                               {"equip", "pullout"},
                                                               {"first_equip", "first_raise"},
                                                               {"putdown", "putaway"},
                                                               {"inspection", "inspect"},
                                                               {"run", "sprint_loop"},
                                                               {"melee", "melee"},
                                                               {"aimingidle", "ads_idle"}};
    if (actions.contains(k))
        k = actions.at(k);
    else {
        k = std::regex_replace(s, std::regex("([a-z0-9])([A-Z])"), "$1_$2");
        k = lower(std::regex_replace(k, std::regex("[^a-zA-Z0-9_]+"), "_"));
    }
    return k + (camera ? "_camera" : "");
}
J applicable_animations(const Model &model, const std::vector<J> &rows) {
    std::set<uint32_t> paths;
    for (auto &b : model.bones)
        for (auto p : b.paths)
            paths.insert(p);
    J result = J::array();
    auto identity = weapon_identity(model.name);
    for (auto row : rows) {
        bool matches = false;
        for (auto p : row.value("paths", J::array()))
            if (paths.contains(p.get<uint32_t>())) {
                matches = true;
                break;
            }
        auto name = row.value("name", std::string());
        if (identity) {
            auto clipIdentity = weapon_identity(name);
            matches = clipIdentity && *clipIdentity == *identity;
        }
        if (matches) {
            row["action"] = action_name(name);
            result.push_back(row);
        }
    }
    return result;
}
} // namespace codm
