#include "rig.hpp"
#include <glm/gtx/quaternion.hpp>

namespace codm {
namespace {
std::map<std::string, int> indices(const Model &m) {
    std::map<std::string, int> result;
    for (size_t i = 0; i < m.bones.size(); ++i)
        require(result.emplace(m.bones[i].name, int(i)).second, "Duplicate T6 bone");
    return result;
}
void rebuild(Model &m) {
    for (size_t i = 0; i < m.bones.size(); ++i) {
        auto &b = m.bones[i];
        require(b.parent < int(i) && b.parent >= -1, "T6 profile is not parent ordered");
        b.rotation = glm::normalize(b.rotation);
        b.world = pose_matrix({b.position, b.scale, b.rotation});
        if (b.parent >= 0)
            b.world = m.bones[b.parent].world * b.world;
        b.inverseBind = glm::inverse(b.world);
    }
}
Q rotation(const M4 &m) {
    DV3 p, s;
    Q q;
    decompose(m, p, q, s);
    return glm::normalize(q);
}
Q swing(DV3 a, DV3 b) {
    // Degenerate axes retain roll. Opposite axes choose a repeatable perpendicular.
    if (glm::length(a) < 1e-10 || glm::length(b) < 1e-10)
        return Q(1, 0, 0, 0);
    a = glm::normalize(a);
    b = glm::normalize(b);
    double d = glm::clamp(glm::dot(a, b), -1.0, 1.0);
    if (d > 1.0 - 1e-12)
        return Q(1, 0, 0, 0);
    if (d < -1.0 + 1e-10) {
        DV3 seed = std::abs(a.x) <= std::abs(a.y) && std::abs(a.x) <= std::abs(a.z) ? DV3(1, 0, 0)
                   : std::abs(a.y) <= std::abs(a.z)                                 ? DV3(0, 1, 0)
                                                                                    : DV3(0, 0, 1);
        return glm::angleAxis(glm::pi<double>(), glm::normalize(glm::cross(a, seed)));
    }
    auto c = glm::cross(a, b);
    return glm::normalize(Q(1 + d, c.x, c.y, c.z));
}
bool helper(const std::string &n) {
    return n.find("wristtwist") != n.npos || n.find("sleav") != n.npos ||
           n.find("palm") != n.npos || n.find("webbing") != n.npos;
}
// Evaluate the original channel ownership on the unchanged target rest skeleton.
// This is the mathematical input of the reviewed candidate, not a consumer call.
std::vector<M4> original_globals(const RigConversion &c, const Animation &a, int f) {
    auto &m = c.model;
    std::vector<Pose> p;
    for (auto &b : m.bones)
        p.push_back({b.position, b.scale, b.rotation});
    for (auto [i, attr] : a.animated) {
        if (attr == 1) {
            p[i].position = a.poses[f][i].position;
            if (a.relativeTranslation.contains(i))
                p[i].position += m.bones[i].position - c.candidateSource->model.bones[i].position;
        } else if (attr == 2)
            p[i].rotation = a.poses[f][i].rotation;
        else if (attr == 3)
            p[i].scale = a.poses[f][i].scale;
    }
    // Weapon bind scaling belongs to the output, not the original sampled input.
    for (size_t i = c.weaponRoot; i < m.bones.size(); ++i) {
        if (!a.animated.contains({int(i), 1}))
            p[i].position = c.candidateSource->model.bones[i].position;
        else if (a.relativeTranslation.contains(int(i)))
            p[i].position = a.poses[f][i].position;
    }
    return pose_worlds(m, p);
}
J bone_metadata(const Model &m) {
    J result = J::array();
    for (auto &b : m.bones)
        result.push_back({{"name", b.name},
                          {"source", b.id},
                          {"parent", b.parent},
                          {"position", {b.position.x, b.position.y, b.position.z}},
                          {"rotation", {b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w}},
                          {"scale", {b.scale.x, b.scale.y, b.scale.z}}});
    return result;
}
} // namespace
void configure_candidate13(RigConversion &c, const Model &native, const Animation &pose,
                           const fs::path &dataRoot) {
    auto profile = read_json(dataRoot / "profiles/viewhands_seal6.json");
    const size_t referenceCount = profile.at("bones").size();
    if (c.kind == "viewhands") {
        c.standaloneViewarms = true;
        c.kind = "weapon-hands";
        c.weaponRoot = int(c.model.bones.size());
        c.weaponMount = -1;
        c.alignment = glm::dmat3(glm::rotate(M4(1), glm::radians(90.0), DV3(0, 0, 1)));
        c.origin = DV3(0);
        std::map<int, int> mapped;
        for (size_t i = 0; i < c.sourceToTarget.size(); ++i) {
            int t = c.sourceToTarget[i];
            auto n = t >= 0 ? c.model.bones[t].name : std::string();
            bool arm = n.starts_with("j_shoulder_") || n.starts_with("j_elbow_") ||
                       n.starts_with("j_wrist_") || n.starts_with("j_wristtwist_");
            for (auto d : {"thumb", "index", "mid", "ring", "pinky"})
                arm |= n.starts_with("j_" + std::string(d) + "_");
            if (arm || t >= int(referenceCount))
                mapped[t] = int(i);
            else
                c.sourceToTarget[i] = -1;
        }
        auto reference = c.model.bones;
        for (size_t t = 0; t < c.model.bones.size(); ++t) {
            auto &b = c.model.bones[t];
            M4 world = pose_matrix({b.position, b.scale, b.rotation});
            if (b.parent >= 0)
                world = c.model.bones[b.parent].world * world;
            if (mapped.contains(int(t))) {
                int i = mapped.at(int(t));
                world = M4(c.alignment * glm::transpose(c.fit[i]) * glm::dmat3(reference[t].world));
                world[3] = glm::dvec4(c.alignment * DV3(native.bones[i].world[3]) * 100.0, 1);
            }
            auto local =
                b.parent >= 0 ? glm::inverse(c.model.bones[b.parent].world) * world : world;
            decompose(local, b.position, b.rotation, b.scale);
            b.world = world;
            b.inverseBind = glm::inverse(world);
        }
    }
    require(!c.candidateSource && c.kind == "weapon-hands",
            "Candidate 13 needs complete viewmodel arms");
    require(pose.frames > 0, "Candidate 13 needs the reviewed pose frame zero");
    c.candidateSource = std::make_shared<RigConversion>(c);
    c.candidateSource->candidateSource.reset();
    c.candidateSource->model.surfaces.clear();
    require(profile.at("sha256") ==
                "50e849766dd10b192d28201dd929c277f123240a57059286b8d7a89342708801",
            "Unexpected SEAL6 profile identity");
    auto &m = c.model;
    auto index = indices(m);
    for (auto &j : profile.at("bones")) {
        auto &b = m.bones.at(index.at(j.at("name").get<std::string>()));
        b.parent = j.at("parent");
        auto p = j.at("position"), q = j.at("rotation"), s = j.at("scale");
        b.position = DV3(p[0], p[1], p[2]);
        b.rotation = Q(q[3], q[0], q[1], q[2]);
        b.rotation = glm::normalize(b.rotation);
        b.scale = DV3(s[0], s[1], s[2]);
    }
    // Preserve old weapon global binds while replacing reference bones verbatim.
    for (size_t i = 0; i < m.bones.size(); ++i) {
        auto &b = m.bones[i];
        if (i >= size_t(c.weaponRoot)) {
            M4 local =
                glm::inverse(m.bones[b.parent].world) * c.candidateSource->model.bones[i].world;
            decompose(local, b.position, b.rotation, b.scale);
        }
        b.world = pose_matrix({b.position, b.scale, b.rotation});
        if (b.parent >= 0)
            b.world = m.bones[b.parent].world * b.world;
        b.inverseBind = glm::inverse(b.world);
    }
    auto oldPose = convert_animation_rig(native, *c.candidateSource, pose);
    auto g = original_globals(c, oldPose, 0);
    std::vector<double> ratios;
    J measurements = J::array();
    for (auto side : {"le", "ri"})
        for (auto digit : {"index", "mid", "ring", "pinky"}) {
            auto stem = "j_" + std::string(digit) + "_" + side + "_";
            int w = index.at("j_wrist_" + std::string(side));
            double sourceLength = glm::length(DV3(g[index.at(stem + "1")][3]) - DV3(g[w][3]));
            double targetLength =
                glm::length(DV3(m.bones[index.at(stem + "0")].world[3]) - DV3(m.bones[w].world[3]));
            require(sourceLength > 1e-5 && targetLength > 1e-5,
                    "Invalid semantic palm measurement");
            ratios.push_back(targetLength / sourceLength);
            measurements.push_back({{"side", side},
                                    {"digit", digit},
                                    {"sourceCm", sourceLength},
                                    {"targetCm", targetLength},
                                    {"ratio", ratios.back()}});
        }
    std::sort(ratios.begin(), ratios.end());
    c.palmScale = (ratios[3] + ratios[4]) * .5;
    require(std::isfinite(c.palmScale) && c.palmScale > .1 && c.palmScale < 10 &&
                ratios.back() / ratios.front() < 1.5,
            "Inconsistent semantic palm correspondence; T6 fit refused");
    if (!c.standaloneViewarms)
        for (auto &s : m.surfaces)
            for (auto &p : s.positions)
                p *= float(c.palmScale);
    for (size_t i = size_t(c.weaponRoot); i < m.bones.size(); ++i) {
        auto &b = m.bones[i];
        M4 world = c.candidateSource->model.bones[i].world;
        world[3] = glm::dvec4(DV3(world[3]) * c.palmScale, 1);
        auto local = glm::inverse(m.bones[b.parent].world) * world;
        decompose(local, b.position, b.rotation, b.scale);
        b.world = world;
    }
    rebuild(m);
    auto &report = m.report["t6Compatibility"];
    for (auto side : {"Left", "Right"})
        for (auto digit : {"Index", "Middle", "Ring", "Pinky", "Thumb"})
            for (int joint = 1; joint <= 3; ++joint) {
                auto sourceName = "b_" + std::string(side) + digit + std::to_string(joint);
                auto name = translated_bone(sourceName);
                auto oldName = name;
                name.back() = char('0' + joint - 1);
                int matches = 0;
                for (size_t i = 0; i < native.bones.size(); ++i)
                    if (translated_bone(native.bones[i].name) == oldName) {
                        matches++;
                        c.sourceToTarget[i] = index.at(name);
                        report["mapping"][native.bones[i].name] = name;
                    }
                require(matches == 1, "Missing or ambiguous semantic finger joint: " + sourceName);
            }
    for (size_t i = 0; i < native.bones.size(); ++i)
        if (native.bones[i].name.find("ForeArmRoll") != std::string::npos) {
            c.sourceToTarget[i] = -1;
            report["mapping"].erase(native.bones[i].name);
        }
    if (c.standaloneViewarms) {
        // Mesh correspondence is independent of the animation helper solution.
        // In particular, an elbow-origin roll influence cannot use a distal pivot.
        auto sourceIndex = indices(native);
        auto meshTargets = c.sourceToTarget;
        std::map<int, int> next, rollOwner;
        for (auto side : {"Left", "Right"}) {
            std::string prefix = "b_" + std::string(side);
            auto suffix = std::string(side) == "Left" ? "le" : "ri";
            int arm = sourceIndex.at(prefix + "Arm"), elbow = sourceIndex.at(prefix + "ForeArm"),
                wrist = sourceIndex.at(prefix + "Hand");
            next[arm] = elbow;
            next[elbow] = wrist;
            if (sourceIndex.contains(prefix + "ForeArmRoll")) {
                int roll = sourceIndex.at(prefix + "ForeArmRoll");
                require(glm::length(DV3(native.bones[roll].world[3]) -
                                    DV3(native.bones[elbow].world[3])) < 1e-5,
                        "Forearm roll pivot requires a separately evidenced mesh mapping");
                meshTargets[roll] = index.at("j_elbow_" + std::string(suffix));
                rollOwner[roll] = elbow;
            }
            for (auto digit : {"Index", "Middle", "Ring", "Pinky", "Thumb"})
                for (int j = 1; j < 3; ++j)
                    next[sourceIndex.at(prefix + digit + std::to_string(j))] =
                        sourceIndex.at(prefix + digit + std::to_string(j + 1));
        }
        auto shoulderAxis = [](const Model &model, int left, int right) {
            DV3 v = DV3(model.bones[left].world[3]) - DV3(model.bones[right].world[3]);
            v.z = 0;
            require(glm::length(v) > 1e-7, "Collapsed shoulder frame");
            return glm::normalize(v);
        };
        auto from = shoulderAxis(native, sourceIndex.at("b_LeftArm"), sourceIndex.at("b_RightArm"));
        auto to = shoulderAxis(m, index.at("j_shoulder_le"), index.at("j_shoulder_ri"));
        auto base = glm::dmat3(glm::rotate(
            M4(1), std::atan2(glm::cross(from, to).z, glm::dot(from, to)), DV3(0, 0, 1)));
        auto palm = [](const Model &model, int w, int mid, int ind, int pink) {
            DV3 x = DV3(model.bones[mid].world[3]) - DV3(model.bones[w].world[3]);
            DV3 y = DV3(model.bones[ind].world[3]) - DV3(model.bones[pink].world[3]);
            require(glm::length(x) > 1e-7, "Collapsed palm axis");
            x = glm::normalize(x);
            y -= x * glm::dot(x, y);
            require(glm::length(y) > 1e-7, "Collinear palm landmarks");
            y = glm::normalize(y);
            return glm::dmat3(x, y, glm::cross(x, y));
        };
        std::vector<glm::dmat3> meshFits(native.bones.size(), base);
        J meshMapping = J::object();
        for (size_t i = 0; i < native.bones.size(); ++i) {
            auto &bone = native.bones[i];
            auto fit = bone.parent >= 0 ? meshFits[bone.parent] : base;
            if (bone.name == "b_LeftHand" || bone.name == "b_RightHand") {
                auto side = bone.name == "b_LeftHand" ? "Left" : "Right";
                auto suffix = bone.name == "b_LeftHand" ? "le" : "ri";
                std::string prefix = "b_" + std::string(side);
                fit = palm(m, index.at("j_wrist_" + std::string(suffix)),
                           index.at("j_mid_" + std::string(suffix) + "_0"),
                           index.at("j_index_" + std::string(suffix) + "_0"),
                           index.at("j_pinky_" + std::string(suffix) + "_0")) *
                      glm::transpose(palm(native, int(i), sourceIndex.at(prefix + "Middle1"),
                                          sourceIndex.at(prefix + "Index1"),
                                          sourceIndex.at(prefix + "Pinky1")));
            } else if (next.contains(int(i))) {
                int child = next.at(int(i)), t = meshTargets[i], tc = meshTargets[child];
                require(t >= 0 && tc >= 0, "Unmapped mesh segment");
                auto sourceDelta = DV3(native.bones[child].world[3]) - DV3(bone.world[3]);
                auto targetDelta = DV3(m.bones[tc].world[3]) - DV3(m.bones[t].world[3]);
                fit = glm::mat3_cast(swing(fit * sourceDelta, targetDelta)) * fit;
            }
            if (rollOwner.contains(int(i)))
                fit = meshFits[rollOwner.at(int(i))];
            meshFits[i] = fit;
            if (meshTargets[i] >= 0)
                meshMapping[bone.name] = m.bones[meshTargets[i]].name;
        }
        // Fit native viewarm vertices to the corrected target rest correspondence;
        // shared animation placement must never be baked into these vertices.
        for (size_t si = 0; si < m.surfaces.size(); ++si) {
            auto &dst = m.surfaces[si];
            auto &src = native.surfaces[si];
            for (size_t v = 0; v < src.positions.size(); ++v) {
                DV3 p(0), n(0);
                for (int k = 0; k < 4; ++k) {
                    double w = src.weights[v][k];
                    if (w <= 0) {
                        dst.joints[v][k] = 0;
                        continue;
                    }
                    int i = src.joints[v][k], t = meshTargets[i];
                    if (t < 0)
                        t = c.candidateSource->sourceToTarget[i];
                    require(t >= 0, "Unmapped weighted viewarm bone: " + native.bones[i].name);
                    auto fit = meshFits[i];
                    p +=
                        w * (fit * (DV3(src.positions[v]) - DV3(native.bones[i].world[3])) * 100.0 +
                             DV3(m.bones[t].world[3]));
                    n += w * fit * DV3(src.normals[v]);
                    dst.joints[v][k] = t;
                }
                dst.positions[v] = V3(p);
                dst.normals[v] = V3(glm::normalize(n));
            }
        }
        report["meshMapping"] = meshMapping;
        report["viewarmMeshFitVersion"] = 2;
        report["forearmRollMeshPolicy"] =
            "Elbow-origin roll weights retain their amounts at the target elbow pivot; independent "
            "roll motion is not retained by this conservative mesh mapping";
        report["meshOrientationPolicy"] = "Palm and segment frames calculated after semantic 1/2/3 "
                                          "to 0/1/2 mapping; no per-segment size multiplier";
        c.kind = "viewhands";
    }
    report["profile"] = "t6-viewmodel-anatomical-v2";
    report["profileVersion"] = 2;
    report["nativeDecoderVersion"] = 2;
    report["referenceSource"] = profile.at("source");
    report["referenceSHA256"] = profile.at("sha256");
    report["palmScale"] = c.palmScale;
    report["palmMeasurements"] = measurements;
    report["referencePose"] = {
        {"name", pose.name},
        {"frame", 0},
        {"source", pose.report.value("source", std::string())},
        {"sourceSHA256", pose.report.value("sourceSHA256", std::string())},
        {"selection",
         pose.report.value("reference", std::string("Authored pose or idle frame zero"))}};
    report["unitScale"] = 100.0;
    report["placementCm"] = {-12.5, 0, 0};
    report["placementScope"] = "All first-person clips; camera unchanged; no worldmodel adaptation";
    report["scaleEvidence"] = "Median of eight semantic wrist-to-knuckle ratios at reviewed pose "
                              "frame zero; separate from metres-to-centimetres conversion";
    report["handMotion"] =
        "Candidate 13: corrected finger rest frames, free shoulders with native T6 arm lengths, "
        "shortest axis swing, reconstructed wrist twists and target-local helpers";
    report["bindPolicy"] = "SEAL6 reference rest unchanged; weapon-only global binds and vertices "
                           "scaled once; framing translation only in animation";
    report["limitations"] =
        "Based on reviewed QQ9 candidate 13 math; each avatar is sized independently. Distal "
        "fingertip roll/contact and broader visual acceptance require user review";
    report["targetBones"] = bone_metadata(m);
    m.report["t6Compatible"] = false;
    report["validationStatus"] = "Exporter numerical validation; user visual acceptance pending";
}

Animation convert_candidate13(const Model &native, const RigConversion &c, const Animation &input,
                              JobContext *job) {
    auto old = convert_animation_rig(native, *c.candidateSource, input, job);
    auto &m = c.model;
    auto ix = indices(m);
    Animation out = old;
    out.poses.clear();
    out.animated.clear();
    out.relativeTranslation.clear();
    for (int f = 0; f < old.frames; ++f) {
        if (job && f % 16 == 0)
            job->check();
        auto original = original_globals(c, old, f), g = original;
        for (size_t i = 0; i < g.size(); ++i)
            if (m.bones[i].name.find("camera") == std::string::npos)
                g[i][3] = glm::dvec4(DV3(g[i][3]) * c.palmScale + DV3(-12.5, 0, 0), 1);
        for (auto side : {"le", "ri"}) {
            int w = ix.at("j_wrist_" + std::string(side)),
                e = ix.at("j_elbow_" + std::string(side)),
                s = ix.at("j_shoulder_" + std::string(side));
            auto direction = [&](int a, int b) {
                DV3 v = DV3(original[a][3]) - DV3(original[b][3]);
                require(glm::length(v) > 1e-9, "Degenerate authored arm segment");
                return glm::normalize(v);
            };
            DV3 ep = DV3(g[w][3]) + direction(e, w) * glm::length(DV3(m.bones[e].world[3]) -
                                                                  DV3(m.bones[w].world[3]));
            DV3 sp = ep + direction(s, e) *
                              glm::length(DV3(m.bones[s].world[3]) - DV3(m.bones[e].world[3]));
            g[e][3] = glm::dvec4(ep, 1);
            g[s][3] = glm::dvec4(sp, 1);
        }
        const auto before = g;
        for (auto side : {"le", "ri"})
            for (auto digit : {"index", "mid", "ring", "pinky", "thumb"})
                for (int j = 0; j < 3; ++j) {
                    auto stem = "j_" + std::string(digit) + "_" + side + "_";
                    int dst = ix.at(stem + std::to_string(j)),
                        src = ix.at(stem + std::to_string(j + 1));
                    g[dst] = before[src] * m.bones[src].inverseBind * m.bones[dst].world;
                    g[dst][3] = before[src][3];
                }
        for (size_t i = 0; i < g.size(); ++i) {
            auto n = m.bones[i].name;
            std::string child;
            if (n.starts_with("j_shoulder_"))
                child = "j_elbow_" + n.substr(n.size() - 2);
            else if (n.starts_with("j_elbow_"))
                child = "j_wrist_" + n.substr(n.size() - 2);
            else
                for (auto digit : {"index", "mid", "ring", "pinky", "thumb"})
                    if (n.starts_with("j_" + std::string(digit) + "_") && n.back() >= '0' &&
                        n.back() < '2') {
                        child = n;
                        ++child.back();
                    }
            if (child.empty())
                continue;
            int ch = ix.at(child);
            DV3 p(g[i][3]);
            DV3 delta(m.bones[i].inverseBind * m.bones[ch].world[3]);
            Q q = swing(glm::dmat3(g[i]) * delta, DV3(g[ch][3]) - p);
            g[i] = M4(glm::mat3_cast(q)) * g[i];
            g[i][3] = glm::dvec4(p, 1);
        }
        std::vector<Pose> local(g.size());
        for (size_t i = 0; i < g.size(); ++i) {
            auto &b = m.bones[i];
            M4 parent = b.parent >= 0 ? g[b.parent] : M4(1);
            if (helper(b.name)) {
                g[i] = parent * pose_matrix({b.position, b.scale, b.rotation});
                if (b.name.find("wristtwist") != b.name.npos) {
                    auto side = b.name.substr(b.name.size() - 2);
                    int e = ix.at("j_elbow_" + side), w = ix.at("j_wrist_" + side);
                    double t = glm::length(DV3(b.world[3]) - DV3(m.bones[e].world[3])) /
                               glm::length(DV3(m.bones[w].world[3]) - DV3(m.bones[e].world[3]));
                    Q qe = rotation(g[e] * m.bones[e].inverseBind * b.world),
                      qw = rotation(g[w] * m.bones[w].inverseBind * b.world);
                    if (glm::dot(qe, qw) < 0)
                        qw = -qw;
                    g[i] = pose_matrix({glm::mix(DV3(g[e][3]), DV3(g[w][3]), t), DV3(1),
                                        glm::normalize(glm::slerp(qe, qw, t))});
                }
            }
            auto l = glm::inverse(parent) * g[i];
            try {
                decompose(l, local[i].position, local[i].rotation, local[i].scale);
            } catch (const std::exception &e) {
                throw std::runtime_error(input.name + " frame " + std::to_string(f) + " bone " +
                                         b.name + ": " + e.what() + " " + matrix_json(l).dump());
            }
            local[i].rotation = glm::normalize(local[i].rotation);
            if (f && glm::dot(out.poses.back()[i].rotation, local[i].rotation) < 0)
                local[i].rotation = -local[i].rotation;
        }
        out.poses.push_back(std::move(local));
    }
    // Retain authored ownership after correspondence changes. Additional channels
    // are emitted only where this clip's solved local differs from target rest.
    // Target-local helpers consequently own nothing; no blanket scale/rest tracks.
    for (auto [i, attr] : old.animated) {
        auto n = m.bones[i].name;
        if (helper(n))
            continue;
        for (auto digit : {"index", "mid", "ring", "pinky", "thumb"})
            if (n.starts_with("j_" + std::string(digit) + "_") && n.back() >= '1' &&
                n.back() <= '3') {
                --n.back();
                break;
            }
        out.animated.insert({ix.at(n), attr});
    }
    for (size_t i = 0; i < m.bones.size(); ++i)
        for (auto &p : out.poses) {
            auto &b = m.bones[i];
            if (glm::length(p[i].position - b.position) > 1e-7)
                out.animated.insert({int(i), 1});
            if (1 - std::abs(glm::dot(p[i].rotation, b.rotation)) > 1e-12)
                out.animated.insert({int(i), 2});
            require(glm::length(p[i].scale - b.scale) < 1e-5,
                    "Unexpected candidate 13 animated scale");
        }
    // A mechanism-only overlay must not acquire either arm merely because its
    // unowned source pose was initialized from rest for numerical evaluation.
    for (auto side : {"le", "ri"}) {
        auto belongs = [&](int i) {
            for (; i >= 0; i = m.bones[i].parent) {
                auto &n = m.bones[i].name;
                if (n.ends_with("_" + std::string(side)) ||
                    n.find("_" + std::string(side) + "_") != n.npos)
                    return true;
            }
            return false;
        };
        bool owned = false;
        for (auto [i, a] : old.animated)
            owned |= belongs(i);
        if (!owned)
            std::erase_if(out.animated, [&](auto channel) { return belongs(channel.first); });
    }
    for (auto [i, attr] : out.animated)
        if (i >= c.weaponRoot && attr == 1)
            out.relativeTranslation.insert(i);
    out.report["t6Compatibility"] = m.report.at("t6Compatibility");
    out.report["t6Compatibility"].erase("nativeBones");
    out.report["t6Compatibility"].erase("targetBones");
    out.report["channelOwnership"] =
        "Authored remapped channels plus non-rest local dependencies of this clip's "
        "anatomical/framing solution; unchanged helper rest and scale channels omitted";
    J owned = J::array();
    for (auto [i, a] : out.animated)
        owned.push_back({{"bone", m.bones[i].name}, {"attribute", a}});
    out.report["ownedTargetChannels"] = owned;
    out.report.erase("armCoverage");
    return out;
}
} // namespace codm
