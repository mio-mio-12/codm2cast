#include "animation.hpp"

namespace codm {
namespace {
using Components = std::array<double, 4>;
struct Key {
    double time = 0;
    Components value{}, incoming{}, outgoing{};
};
struct Track {
    Binding binding;
    std::vector<Key> keys;
};
std::span<const uint8_t> packed_data(const J &packed) {
    const auto &data = packed.at("m_Data");
    require(data.is_binary(), "Expected binary packed legacy curve data");
    return data.get_binary();
}
std::vector<uint32_t> packed_ints(const J &packed, size_t limit) {
    uint32_t count = packed.at("m_NumItems"), bits = packed.at("m_BitSize");
    auto data = packed_data(packed);
    require(count <= limit && bits <= 32 && data.size() == (uint64_t(count) * bits + 7) / 8,
            "Packed legacy vector count or byte length is invalid");
    std::vector<uint32_t> values(count);
    uint64_t bit = 0;
    for (auto &value : values) {
        for (uint32_t shift = 0; shift < bits;) {
            unsigned offset = unsigned(bit % 8), take = std::min(bits - shift, 8 - offset);
            value |= uint32_t((data[bit / 8] >> offset) & ((1u << take) - 1)) << shift;
            bit += take;
            shift += take;
        }
    }
    return values;
}
std::vector<double> packed_slopes(const J &packed) {
    auto integers = packed_ints(packed, 400000);
    uint32_t bits = packed.at("m_BitSize");
    double start = packed.at("m_Start"), range = packed.at("m_Range");
    require(std::isfinite(start) && std::isfinite(range) && range >= 0,
            "Invalid packed legacy tangent range");
    double denominator = bits ? double((uint64_t(1) << bits) - 1) : 1;
    std::vector<double> values;
    values.reserve(integers.size());
    for (auto integer : integers) {
        double value = start + integer * range / denominator;
        require(std::isfinite(value), "Nonfinite packed legacy tangent");
        values.push_back(value);
    }
    return values;
}
std::vector<Components> packed_rotations(const J &packed) {
    uint32_t count = packed.at("m_NumItems");
    auto data = packed_data(packed);
    require(count > 0 && count <= 100000 && data.size() == uint64_t(count) * 4,
            "Invalid packed legacy quaternion count or byte length");
    Reader reader(data);
    std::vector<Components> rotations;
    rotations.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t word = reader.get<uint32_t>(), flags = word & 7, omitted = flags & 3;
        word >>= 3;
        Components value{};
        double squared = 0;
        for (uint32_t k = 0; k < 4; k++) {
            if (k == omitted)
                continue;
            unsigned bits = k == (omitted + 1) % 4 ? 9 : 10;
            uint32_t mask = (1u << bits) - 1;
            value[k] = 2.0 * (word & mask) / mask - 1;
            squared += value[k] * value[k];
            word >>= bits;
        }
        require(squared <= 1, "Invalid packed legacy quaternion components");
        value[omitted] = std::sqrt(1 - squared) * (flags & 4 ? -1 : 1);
        rotations.push_back(value);
    }
    return rotations;
}
Components components(const J &value, int width, bool tangent) {
    Components out{};
    for (int k = 0; k < width; k++) {
        out[k] = value.at(std::string(1, "xyzw"[k])).get<double>();
        require(tangent ? !std::isnan(out[k]) : std::isfinite(out[k]),
                "Invalid legacy curve value or tangent");
    }
    return out;
}
double length(const Components &value) {
    double sum = 0;
    for (auto v : value)
        sum += v * v;
    return std::sqrt(sum);
}
void validate_track(const Track &track) {
    require(!track.keys.empty() && track.keys.size() <= 100000, "Empty or oversized legacy curve");
    double previous = -INFINITY;
    for (auto &key : track.keys) {
        require(std::isfinite(key.time) && key.time >= -1e-5 && key.time > previous,
                "Non-increasing or invalid legacy key time: " + track.binding.pathName);
        previous = key.time;
        if (track.binding.attribute == 2)
            require(std::abs(length(key.value) - 1) < .01, "Non-unit legacy rotation key");
    }
}
Components sample(const Track &track, double time) {
    const auto &keys = track.keys;
    if (time <= keys.front().time)
        return keys.front().value;
    if (time >= keys.back().time)
        return keys.back().value;
    auto upper = std::upper_bound(keys.begin(), keys.end(), time,
                                  [](double t, const Key &k) { return t < k.time; });
    const auto &a = *(upper - 1), &b = *upper;
    double duration = b.time - a.time, u = (time - a.time) / duration;
    double u2 = u * u, u3 = u2 * u;
    Components result{};
    for (int k = 0; k < track.binding.width; k++) {
        // Infinite slopes encode a held (stepped) segment. The upper key is
        // selected exactly at its time, before this interpolation branch.
        if (std::isinf(a.outgoing[k]) || std::isinf(b.incoming[k]))
            result[k] = a.value[k];
        else
            result[k] = (2 * u3 - 3 * u2 + 1) * a.value[k] +
                        (u3 - 2 * u2 + u) * duration * a.outgoing[k] +
                        (-2 * u3 + 3 * u2) * b.value[k] + (u3 - u2) * duration * b.incoming[k];
        require(std::isfinite(result[k]), "Nonfinite interpolated legacy curve");
    }
    return result;
}
} // namespace

Clip sample_legacy_curves(const J &header, JobContext *job) {
    require(header.at("m_Legacy").get<bool>() && header.at("m_MuscleClipSize") == 0,
            "Legacy curve sampler requires a legacy clip without muscle data");
    require(header.at("m_FloatCurves").empty() && header.at("m_PPtrCurves").empty(),
            "Legacy scalar or object-reference curves require a target adapter");
    Clip out;
    out.name = header.at("m_Name");
    out.fps = header.at("m_SampleRate");
    require(std::isfinite(out.fps) && out.fps >= 1 && out.fps <= 240, "Invalid legacy sample rate");
    int wrap = header.at("m_WrapMode");
    require(wrap == 0 || wrap == 1 || wrap == 2 || wrap == 8, "Unsupported legacy clip wrap mode");
    out.looping = wrap == 2;
    std::vector<Track> tracks;
    std::set<std::pair<uint32_t, int>> occupied;
    double stop = 0;
    size_t keyCount = 0;
    for (auto [field, attr] : {std::pair{"m_PositionCurves", 1},
                               {"m_RotationCurves", 2},
                               {"m_ScaleCurves", 3},
                               {"m_EulerCurves", 4}}) {
        for (auto &row : header.at(field)) {
            if (job)
                job->check();
            Track track;
            auto &b = track.binding;
            b.pathName = row.at("path");
            require(b.pathName.find('\0') == std::string::npos && b.pathName.size() < 65536,
                    "Invalid legacy curve path");
            b.path = crc32(b.pathName);
            b.type = 4;
            b.attribute = attr;
            b.width = attr == 2 ? 4 : 3;
            b.offset = out.columns;
            require(occupied.insert({b.path, attr == 4 ? 2 : attr}).second,
                    "Duplicate or colliding legacy transform curve: " + b.pathName);
            const auto &curve = row.at("curve");
            // Serialized curve infinity value 2 holds endpoint values. It is
            // distinct from the clip's WrapMode enum and does not loop a track.
            require(curve.at("m_PreInfinity") == 2 && curve.at("m_PostInfinity") == 2,
                    "Unsupported legacy curve infinity mode");
            if (attr == 4) {
                b.eulerOrder = curve.at("m_RotationOrder");
                require(b.eulerOrder >= 0 && b.eulerOrder < 6,
                        "Unknown legacy Euler rotation order");
            }
            for (auto &key : curve.at("m_Curve")) {
                require(++keyCount <= 2000000, "Legacy animation exceeds key memory limit");
                track.keys.push_back({key.at("time").get<double>(),
                                      components(key.at("value"), b.width, false),
                                      components(key.at("inSlope"), b.width, true),
                                      components(key.at("outSlope"), b.width, true)});
            }
            validate_track(track);
            stop = std::max(stop, track.keys.back().time);
            out.columns += b.width;
            require(out.columns <= 100000, "Excess legacy curves");
            out.bindings.push_back(b);
            tracks.push_back(std::move(track));
        }
    }
    size_t packedTrackCount = 0, packedKeyCount = 0;
    for (const auto &row : header.at("m_CompressedRotationCurves")) {
        if (job)
            job->check();
        Track track;
        auto &binding = track.binding;
        binding.pathName = row.at("m_Path");
        require(binding.pathName.find('\0') == std::string::npos && binding.pathName.size() < 65536,
                "Invalid packed legacy curve path");
        binding.path = crc32(binding.pathName);
        binding.type = 4;
        binding.attribute = 2;
        binding.width = 4;
        binding.offset = out.columns;
        require(occupied.insert({binding.path, 2}).second,
                "Duplicate or colliding legacy rotation curve: " + binding.pathName);
        require(row.at("m_PreInfinity") == 2 && row.at("m_PostInfinity") == 2,
                "Unsupported packed legacy curve infinity mode");
        auto deltas = packed_ints(row.at("m_Times"), 100000);
        auto rotations = packed_rotations(row.at("m_Values"));
        auto slopes = packed_slopes(row.at("m_Slopes"));
        require(deltas.size() == rotations.size() && slopes.size() == rotations.size() * 4,
                "Packed legacy time, rotation and tangent counts disagree");
        uint64_t tick = 0;
        for (size_t i = 0; i < rotations.size(); i++) {
            require(++keyCount <= 2000000, "Legacy animation exceeds key memory limit");
            tick += deltas[i];
            Components tangent{};
            for (int k = 0; k < 4; k++)
                tangent[k] = slopes[i * 4 + k];
            // Verified against Unity's native playback of 21 original packed
            // payloads: cumulative centiseconds and one derivative per key,
            // shared by its incoming and outgoing Hermite segments.
            track.keys.push_back({tick * .01, rotations[i], tangent, tangent});
        }
        validate_track(track);
        stop = std::max(stop, track.keys.back().time);
        out.columns += 4;
        require(out.columns <= 100000, "Excess legacy curves");
        packedKeyCount += track.keys.size();
        packedTrackCount++;
        out.bindings.push_back(binding);
        tracks.push_back(std::move(track));
    }
    require(!tracks.empty() && stop * out.fps <= 99999, "Empty or oversized legacy animation");
    // Export at the authored frame rate, retaining its terminal frame despite
    // floating-point drift in imported key times.
    out.frames = int(std::lround(stop * out.fps)) + 1;
    require(size_t(out.frames) * out.columns <= 20000000, "Legacy sample memory limit exceeded");
    out.samples.resize(size_t(out.frames) * out.columns);
    double normError = 0;
    for (int f = 0; f < out.frames; f++) {
        if (job && f % 32 == 0)
            job->check();
        double time = f == out.frames - 1 ? stop : f / out.fps;
        for (const auto &track : tracks) {
            auto value = sample(track, time);
            if (track.binding.attribute == 2) {
                double norm = length(value);
                require(std::isfinite(norm) && norm > 1e-8,
                        "Singular interpolated legacy rotation");
                normError = std::max(normError, std::abs(norm - 1));
                for (auto &v : value)
                    v /= norm;
            }
            for (int k = 0; k < track.binding.width; k++)
                out.samples[size_t(f) * out.columns + track.binding.offset + k] = value[k];
        }
    }
    out.events = J::array();
    out.report = {
        {"adapter",
         packedTrackCount ? "CODM legacy transform curves v2" : "CODM legacy transform curves v1"},
        {"sourceName", out.name},
        {"sourceAnimation", true},
        {"fps", out.fps},
        {"frames", out.frames},
        {"looping", out.looping},
        {"sourceWrapMode", wrap},
        {"sourceStopTime", stop},
        {"sourceKeys", keyCount},
        {"maximumQuaternionNormalization", normError},
        {"interpolation", "Cubic Hermite with authored tangents; normalized quaternion components"},
        {"bindings", J::array()},
        {"nonTransformBindings", J::array()},
        {"referencePose", J::array()},
        {"events", out.events}};
    for (const auto &b : out.bindings)
        out.report["bindings"].push_back({{"path", b.path},
                                          {"pathName", b.pathName},
                                          {"attribute", b.attribute},
                                          {"type", b.type},
                                          {"width", b.width},
                                          {"offset", b.offset}});
    if (packedTrackCount) {
        out.report["packedRotationTracks"] = packedTrackCount;
        out.report["packedRotationKeys"] = packedKeyCount;
        out.report["packedRotationEncoding"] =
            "32-bit omitted-component quaternion; cumulative centisecond times; "
            "four packed tangent components per key shared by incoming/outgoing segments";
        out.report["packedRotationReference"] = "Unity 2022.3.62f3 AnimationClip.SampleAnimation";
    }
    return out;
}
} // namespace codm
