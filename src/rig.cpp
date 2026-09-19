#include "rig.hpp"
#include <glm/gtx/quaternion.hpp>
namespace codm {
std::string translated_bone(const std::string &name) {
    static const auto names = [] {
        std::map<std::string, std::string> m = {
            {"b_Root", "tag_origin"},     {"Bip01", "j_mainroot"},      {"b_Hips", "pelvis"},
            {"b_Spine2", "j_spinelower"}, {"b_Spine1", "j_spineupper"}, {"b_Spine", "j_spine4"},
            {"b_Neck", "j_neck"},         {"b_Head", "j_head"}};
        for (auto [side, suffix] :
             {std::pair<std::string, std::string>{"Left", "le"}, {"Right", "ri"}}) {
            for (auto [a, b] : std::map<std::string, std::string>{{"Clav", "clavicle"},
                                                                  {"Arm", "shoulder"},
                                                                  {"ForeArm", "elbow"},
                                                                  {"Hand", "wrist"},
                                                                  {"LegUpper", "hip"},
                                                                  {"Leg", "knee"},
                                                                  {"Ankle", "ankle"},
                                                                  {"Toe", "ball"},
                                                                  {"ForeArmRoll", "wristtwist"}})
                m["b_" + side + a] = "j_" + b + "_" + suffix;
            m["Bip01 " + side.substr(0, 1) + " ForeTwist"] = "j_wristtwist_" + suffix;
            m["Bip01 " + side.substr(0, 1) + "UpArmTwist"] = "j_shouldertwist_" + suffix;
            int f = 0;
            for (auto [a, b] :
                 std::vector<std::pair<std::string, std::string>>{{"Thumb", "thumb"},
                                                                  {"Index", "index"},
                                                                  {"Middle", "mid"},
                                                                  {"Ring", "ring"},
                                                                  {"Pinky", "pinky"}}) {
                for (int j = 1; j <= 3; j++)
                    m["b_" + side + a + std::to_string(j)] =
                        "j_" + b + "_" + suffix + "_" + std::to_string(j);
                m["b_" + side + "Finger" + std::to_string(f)] = "j_" + b + "_" + suffix + "_1";
                m["b_" + side + "Finger" + std::to_string(f) + "1"] =
                    "j_" + b + "_" + suffix + "_2";
                f++;
            }
        }
        return m;
    }();
    auto it = names.find(name);
    return it == names.end() ? name : it->second;
}
static glm::dmat3 rotate_z(double angle) {
    return glm::dmat3(glm::rotate(M4(1), angle, DV3(0, 0, 1)));
}
static void update_worlds(Model &model) {
    for (size_t i = 0; i < model.bones.size(); i++) {
        auto &b = model.bones[i];
        require(b.parent >= -1 && b.parent < int(i), "Invalid converted bone hierarchy");
        auto length = glm::length(b.rotation);
        require(std::isfinite(length) && std::abs(length - 1) < .01,
                "Invalid converted quaternion: " + b.name);
        b.rotation = glm::normalize(b.rotation);
        b.world = pose_matrix({b.position, b.scale, b.rotation});
        if (b.parent >= 0)
            b.world = model.bones[b.parent].world * b.world;
        b.inverseBind = glm::inverse(b.world);
    }
}
static J bones_json(const Model &model) {
    J out = J::array();
    for (auto &b : model.bones)
        out.push_back({{"name", b.name},
                       {"source", b.id},
                       {"parent", b.parent},
                       {"position", {b.position.x, b.position.y, b.position.z}},
                       {"rotation", {b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w}},
                       {"scale", {b.scale.x, b.scale.y, b.scale.z}}});
    return out;
}
static void validate_source(const Model &model) {
    std::set<std::string> names;
    for (size_t i = 0; i < model.bones.size(); i++) {
        auto &b = model.bones[i];
        require(names.insert(b.name).second, "Duplicate source bone: " + b.name);
        require(b.parent < int(i) && b.parent >= -1, "Source rig not parent ordered: " + b.name);
        double lo = std::min({b.scale.x, b.scale.y, b.scale.z}),
               hi = std::max({b.scale.x, b.scale.y, b.scale.z});
        require(std::isfinite(lo) && std::isfinite(hi) && lo > 0 && hi - lo < .002,
                "T6 conversion needs positive uniform rest scale: " + b.name);
    }
}
RigConversion convert_rig(const Model &native, const fs::path &dataRoot, bool fitFirstPerson) {
    validate_source(native);
    RigConversion out;
    out.model = native;
    out.sourceToTarget.assign(native.bones.size(), -1);
    out.fit.assign(native.bones.size(), glm::dmat3(1));
    out.alignment = rotate_z(glm::radians(90.0));
    std::map<std::string, int> sourceIndex;
    for (size_t i = 0; i < native.bones.size(); i++)
        sourceIndex[native.bones[i].name] = int(i);
    auto &target = out.model;
    J mapping = J::object(), evidence = J::object(), extras = J::array();
    if (native.report.value("rigidGeometry", false) ||
        native.report.value("source", J::object()).value("category", std::string()) == "Etc") {
        out.kind = "rigid";
        M4 axis(out.alignment);
        for (size_t i = 0; i < target.bones.size(); i++) {
            out.sourceToTarget[i] = int(i);
            auto &b = target.bones[i];
            M4 world = axis * native.bones[i].world * glm::transpose(axis);
            world[3] = glm::dvec4(DV3(world[3]) * 100.0, 1);
            M4 local = b.parent >= 0 ? glm::inverse(target.bones[b.parent].world) * world : world;
            decompose(local, b.position, b.rotation, b.scale);
            b.world = world;
            b.inverseBind = glm::inverse(world);
        }
        for (auto &s : target.surfaces) {
            for (auto &p : s.positions)
                p = V3(out.alignment * DV3(p) * 100.0);
            for (auto &n : s.normals)
                n = V3(out.alignment * DV3(n));
        }
        target.report["t6Compatibility"] = {
            {"kind", "source hierarchy"},
            {"profile", "T6 axes and centimetres; authored hierarchy and weights"},
            {"semanticCoverage", "Prop/worldmodel source hierarchy. No canonical humanoid or "
                                 "first-person mechanism rig is implied."}};
    } else if (native.report.value("source", J::object()).value("category", std::string()) ==
                   "Weapon" &&
               sourceIndex.contains("Bone_RightHand") && weapon_identity(native.name)) {
        out.kind = "weapon";
        int root = sourceIndex.at("Bone_RightHand");
        out.origin = DV3(native.bones[root].world[3]);
        std::set<int> descendants{root};
        for (size_t i = root + 1; i < native.bones.size(); i++)
            if (descendants.contains(native.bones[i].parent))
                descendants.insert(int(i));
        std::map<int, std::string> semantic{{root, "j_gun"}};
        evidence["Bone_RightHand"] = "authored motion root";
        bool mp5 = sourceIndex.contains("MainWeapon_179_MP5_1P") &&
                   sourceIndex.contains("Mag_01") && sourceIndex.contains("Bone001") &&
                   sourceIndex.contains("Bone002");
        if (mp5)
            for (auto [name, role] : std::map<std::string, std::string>{
                     {"Mag_01", "tag_clip"}, {"Bone001", "j_bolt"}, {"Bone002", "j_reload"}}) {
                semantic[sourceIndex.at(name)] = role;
                evidence[name] = "MP5 fixture: mechanism geometry and reload channels";
            }
        else {
            for (auto name : {"Mag_point", "Mag_Point"})
                if (sourceIndex.contains(name)) {
                    int i = sourceIndex.at(name), p = native.bones[i].parent;
                    require(descendants.contains(i), "Magazine socket outside motion root");
                    int children = 0;
                    for (auto &b : native.bones)
                        children += b.parent == p;
                    int driver = p != root && children == 1 ? p : i;
                    for (auto &[index, role] : semantic)
                        require(role != "tag_clip", "Ambiguous magazine drivers");
                    semantic[driver] = "tag_clip";
                    evidence[native.bones[driver].name] = "unique authored magazine socket driver";
                }
            for (auto [name, role] :
                 std::map<std::string, std::string>{{"Bone_QiangShuan", "j_bolt"},
                                                    {"Bone_DanJia", "tag_clip"},
                                                    {"Mag_01", "tag_clip"},
                                                    {"tag_clip", "tag_clip"},
                                                    {"j_bolt", "j_bolt"},
                                                    {"j_slide", "j_slide"},
                                                    {"j_reload", "j_reload"},
                                                    {"tag_flash", "tag_flash"}})
                if (sourceIndex.contains(name) && descendants.contains(sourceIndex[name])) {
                    bool used = false;
                    for (auto &[i, r] : semantic)
                        used |= r == role;
                    if (!used) {
                        semantic[sourceIndex[name]] = role;
                        evidence[name] = "explicit mechanism name";
                    }
                }
        }
        target.bones.clear();
        for (auto [name, parent] : std::vector<std::pair<std::string, int>>{
                 {"tag_view", -1}, {"tag_weapon", 0}, {"j_gun", 1}}) {
            Bone b;
            b.name = name;
            b.parent = parent;
            target.bones.push_back(b);
        }
        out.sourceToTarget[root] = 2;
        for (int i : descendants) {
            if (i == root)
                continue;
            auto &source = native.bones[i];
            Bone b = source;
            b.name = semantic.contains(i) ? semantic.at(i) : source.name;
            b.parent = semantic.contains(i) ? 2 : out.sourceToTarget.at(source.parent);
            require(b.parent >= 0, "Unmapped weapon ancestor: " + source.name);
            for (auto &existing : target.bones)
                require(existing.name != b.name, "Converted weapon name collision: " + b.name);
            DV3 worldPos = out.alignment * (DV3(source.world[3]) - out.origin) * 100.0;
            b.position = worldPos - DV3(target.bones[b.parent].world[3]);
            b.rotation = Q(1, 0, 0, 0);
            b.scale = DV3(1);
            b.world = glm::translate(M4(1), worldPos);
            b.inverseBind = glm::inverse(b.world);
            b.paths.clear();
            out.sourceToTarget[i] = int(target.bones.size());
            target.bones.push_back(b);
            mapping[source.name] = b.name;
            if (!semantic.contains(i) && lower(source.name).starts_with("bone"))
                extras.push_back(source.name);
        }
        for (auto &s : target.surfaces) {
            for (auto &p : s.positions)
                p = V3(out.alignment * (DV3(p) - out.origin) * 100.0);
            for (auto &n : s.normals)
                n = V3(out.alignment * DV3(n));
            for (size_t v = 0; v < s.weights.size(); v++)
                for (int k = 0; k < 4; k++) {
                    int source = s.joints[v][k];
                    int mapped = out.sourceToTarget.at(source);
                    require(s.weights[v][k] <= 1e-8 || mapped >= 0,
                            "Weighted bone outside weapon hierarchy: " + native.bones[source].name);
                    s.joints[v][k] = mapped >= 0 ? mapped : 2;
                }
        }
        mapping["Bone_RightHand"] = "j_gun";
        target.report["t6Compatibility"] = {
            {"kind", "weapon"},
            {"profile", mp5 ? "t6-mp5-weapon" : "t6-structural-weapon"},
            {"mapping", mapping},
            {"evidence", evidence},
            {"unclassifiedBones", extras},
            {"semanticCoverage", "root and evidenced mechanisms; additional bones retained"},
            {"meshFit", "rigid axis and unit conversion; physical geometry and "
                        "pivots retained"}};
    } else {
        std::vector<std::string> names;
        sourceIndex.clear();
        for (size_t i = 0; i < native.bones.size(); i++) {
            auto n = translated_bone(native.bones[i].name);
            require(!sourceIndex.contains(n), "Anatomical name collision: " + n);
            sourceIndex[n] = int(i);
            names.push_back(n);
        }
        std::string kind;
        const bool hasHands =
            sourceIndex.contains("j_shoulder_le") && sourceIndex.contains("j_shoulder_ri") &&
            sourceIndex.contains("j_index_le_1") && sourceIndex.contains("j_index_ri_1");
        const bool viewhands =
            native.report.value("source", J::object()).value("category", std::string()) ==
            "Viewhands";
        if (viewhands) {
            // A viewarm Avatar can retain an entire unweighted character rig.
            // The selected renderer category owns the export role, not those legs.
            require(hasHands, "Viewhands source lacks an evidenced arm/finger hierarchy");
            kind = "viewhands";
        } else if (sourceIndex.contains("j_hip_le") && sourceIndex.contains("j_hip_ri") &&
                   sourceIndex.contains("j_head"))
            kind = "player";
        else if (hasHands)
            kind = "viewhands";
        if (kind.empty()) {
            bool weighted = false;
            for (auto &s : native.surfaces)
                for (auto &w : s.weights)
                    weighted |= (w.x + w.y + w.z + w.w) > 0;
            require(!weighted, "No evidenced T6 rig for this asset. Export source "
                               "rig or select a supported "
                               "character/weapon.");
            out.kind = "static";
            target.bones.clear();
            for (auto &s : target.surfaces) {
                for (auto &p : s.positions)
                    p = V3(out.alignment * DV3(p) * 100.0);
                for (auto &n : s.normals)
                    n = V3(out.alignment * DV3(n));
            }
            target.report["t6Compatibility"] = {{"kind", "static"},
                                                {"profile", "T6 axes and centimetres"}};
        } else {
            out.kind = kind;
            auto profile =
                read_json(dataRoot / "profiles" /
                          (kind == "viewhands" ? "viewhands_seal6.json" : kind + ".json"));
            std::map<std::string, int> targetIndex;
            target.bones.clear();
            for (auto &row : profile.at("bones")) {
                Bone b;
                b.name = row.at("name");
                b.parent = row.at("parent");
                auto p = row.at("position"), q = row.at("rotation"), s = row.at("scale");
                b.position = {p[0], p[1], p[2]};
                b.rotation = Q(q[3].get<double>(), q[0].get<double>(), q[1].get<double>(),
                               q[2].get<double>());
                b.scale = {s[0], s[1], s[2]};
                targetIndex[b.name] = int(target.bones.size());
                target.bones.push_back(b);
            }
            update_worlds(target);
            auto pair = kind == "player"
                            ? std::pair<std::string, std::string>{"j_hip_le", "j_hip_ri"}
                            : std::pair<std::string, std::string>{"j_shoulder_le", "j_shoulder_ri"};
            auto axis = [&](auto &bones, auto &index) {
                DV3 a = DV3(bones[index.at(pair.first)].world[3]) -
                        DV3(bones[index.at(pair.second)].world[3]);
                a.z = 0;
                require(glm::length(a) > 1e-6, "Cannot establish anatomical facing");
                return glm::normalize(a);
            };
            auto a = axis(native.bones, sourceIndex), b = axis(target.bones, targetIndex);
            out.alignment = rotate_z(std::atan2(glm::cross(a, b).z, glm::dot(a, b)));
            std::map<std::string, std::string> next = {{"pelvis", "j_spinelower"},
                                                       {"j_spinelower", "j_spineupper"},
                                                       {"j_spineupper", "j_spine4"},
                                                       {"j_spine4", "j_neck"},
                                                       {"j_neck", "j_head"}};
            for (std::string side : {"le", "ri"}) {
                for (auto [a, b] : std::map<std::string, std::string>{{"clavicle", "shoulder"},
                                                                      {"shoulder", "elbow"},
                                                                      {"elbow", "wrist"},
                                                                      {"wrist", "mid"},
                                                                      {"hip", "knee"},
                                                                      {"knee", "ankle"},
                                                                      {"ankle", "ball"}})
                    next["j_" + a + "_" + side] =
                        b == "mid" ? "j_mid_" + side + "_1" : "j_" + b + "_" + side;
                for (std::string finger : {"thumb", "index", "mid", "ring", "pinky"})
                    for (int j = 1; j <= 2; j++)
                        next["j_" + finger + "_" + side + "_" + std::to_string(j)] =
                            "j_" + finger + "_" + side + "_" + std::to_string(j + 1);
            }
            for (size_t i = 0; i < native.bones.size(); i++) {
                auto name = names[i];
                int parent = native.bones[i].parent;
                auto base = parent >= 0 ? out.fit[parent] : out.alignment;
                out.fit[i] = base;
                bool palmDone = false;
                if (name == "j_wrist_le" || name == "j_wrist_ri") {
                    auto side = name.substr(name.size() - 2);
                    std::vector<std::string> landmarks = {name, "j_mid_" + side + "_1",
                                                          "j_index_" + side + "_1",
                                                          "j_pinky_" + side + "_1"};
                    bool valid = true;
                    for (auto &n : landmarks)
                        valid &= sourceIndex.contains(n) && targetIndex.contains(n);
                    if (valid) {
                        auto palm = [&](auto &bones, auto &index) {
                            DV3 p = DV3(bones[index.at(landmarks[0])].world[3]),
                                x = DV3(bones[index.at(landmarks[1])].world[3]) - p,
                                y = DV3(bones[index.at(landmarks[2])].world[3]) -
                                    DV3(bones[index.at(landmarks[3])].world[3]);
                            require(glm::length(x) > 1e-7, "Collapsed hand landmarks");
                            x = glm::normalize(x);
                            y -= x * glm::dot(x, y);
                            require(glm::length(y) > 1e-7, "Collinear hand landmarks");
                            y = glm::normalize(y);
                            return glm::dmat3(x, y, glm::cross(x, y));
                        };
                        out.fit[i] = palm(target.bones, targetIndex) *
                                     glm::transpose(palm(native.bones, sourceIndex));
                        palmDone = true;
                    }
                }
                auto end = next.contains(name) ? next.at(name) : "";
                if (!palmDone && targetIndex.contains(name) && sourceIndex.contains(end) &&
                    targetIndex.contains(end)) {
                    DV3 from = base * (DV3(native.bones[sourceIndex.at(end)].world[3]) -
                                       DV3(native.bones[i].world[3])),
                        to = DV3(target.bones[targetIndex.at(end)].world[3]) -
                             DV3(target.bones[targetIndex.at(name)].world[3]);
                    if (std::min(glm::length(from), glm::length(to)) > 1e-5) {
                        from = glm::normalize(from);
                        to = glm::normalize(to);
                        require(glm::dot(from, to) > -.99999, "Opposite anatomical axes: " + name);
                        out.fit[i] = glm::mat3_cast(glm::rotation(from, to)) * base;
                    }
                }
                std::string owner = name;
                for (auto [a, b] : std::vector<std::pair<std::string, std::string>>{
                         {"wristtwist", "elbow"}, {"shouldertwist", "shoulder"}}) {
                    auto p = owner.find(a);
                    if (p != owner.npos)
                        owner.replace(p, a.size(), b);
                }
                if (owner != name && sourceIndex.contains(owner) && sourceIndex.at(owner) < int(i))
                    out.fit[i] = out.fit[sourceIndex.at(owner)];
                if (targetIndex.contains(name))
                    out.sourceToTarget[i] = targetIndex.at(name);
                else {
                    // Retain every extra source bone under its fitted source parent,
                    // including weighted cloth/mechanisms.
                    Bone extra = native.bones[i];
                    extra.name = name;
                    extra.paths.clear();
                    extra.parent = parent >= 0 ? out.sourceToTarget[parent] : -1;
                    require(parent < 0 || extra.parent >= 0,
                            "Missing anatomical parent for extra bone");
                    DV3 pos = extra.parent >= 0
                                  ? DV3(target.bones[extra.parent].world[3]) +
                                        out.fit[i] *
                                            (DV3(native.bones[i].world[3]) -
                                             DV3(native.bones[parent].world[3])) *
                                            100.0
                                  : out.fit[i] * DV3(native.bones[i].world[3]) * 100.0;
                    M4 world(out.fit[i] * glm::dmat3(native.bones[i].world));
                    world[3] = glm::dvec4(pos, 1);
                    M4 local = extra.parent >= 0
                                   ? glm::inverse(target.bones[extra.parent].world) * world
                                   : world;
                    decompose(local, extra.position, extra.rotation, extra.scale);
                    extra.world = world;
                    extra.inverseBind = glm::inverse(world);
                    out.sourceToTarget[i] = int(target.bones.size());
                    targetIndex[name] = int(target.bones.size());
                    target.bones.push_back(extra);
                    extras.push_back(name);
                }
                mapping[native.bones[i].name] = target.bones[out.sourceToTarget[i]].name;
            }
            for (auto &s : target.surfaces)
                for (size_t v = 0; v < s.positions.size(); v++) {
                    require(v < s.weights.size(), "Unskinned surface in anatomical conversion");
                    DV3 p(0), n(0);
                    double sum = 0;
                    for (int k = 0; k < 4; k++) {
                        int i = s.joints[v][k];
                        double w = s.weights[v][k];
                        if (w <= 0) {
                            s.joints[v][k] = 0;
                            continue;
                        }
                        int t = out.sourceToTarget.at(i);
                        p +=
                            w * (out.fit[i] *
                                     (DV3(s.positions[v]) - DV3(native.bones[i].world[3])) * 100.0 +
                                 DV3(target.bones[t].world[3]));
                        n += w * out.fit[i] * DV3(s.normals[v]);
                        s.joints[v][k] = t;
                        sum += w;
                    }
                    require(std::abs(sum - 1) < .002, "Anatomical mesh weights do not sum to one");
                    s.positions[v] = V3(p);
                    s.normals[v] = V3(glm::normalize(n));
                }
            target.report["t6Compatibility"] = {
                {"kind", kind},
                {"profile", profile.at("name")},
                {"referenceSource", profile.at("source")},
                {"referenceSHA256", profile.at("sha256")},
                {"mapping", mapping},
                {"extraBonesPreserved", extras},
                {"meshFit", "weighted anatomical fit to reference rest skeleton; "
                            "character proportions change"}};
        }
    }
    if (out.kind == "weapon" && sourceIndex.contains("b_LeftHand") &&
        sourceIndex.contains("b_RightHand") &&
        native.report.value("animationHierarchy", J::object())
                .value("retainedAvatarPaths", size_t(0)) > 0) {
        // Fit orientation frames to the actual target reference, but preserve the
        // source's world-space joint trajectories and the common weapon origin.
        Model arms = native;
        arms.surfaces.clear();
        arms.report["source"]["category"] = "Viewhands";
        arms.report["weaponHandFit"] = true;
        auto fitted = convert_rig(arms, dataRoot, false);
        require(fitted.kind == "viewhands", "Weapon Avatar is not a complete arm rig");
        auto profile = read_json(dataRoot / "profiles" / "viewhands_seal6.json");
        auto weaponBones = target.bones;
        auto weaponMapping = out.sourceToTarget;
        target.bones.assign(fitted.model.bones.begin(),
                            fitted.model.bones.begin() + profile.at("bones").size());
        out.sourceToTarget.assign(native.bones.size(), -1);
        std::map<int, int> hands;
        for (size_t i = 0; i < native.bones.size(); i++) {
            int t = fitted.sourceToTarget[i];
            if (t >= 0 && size_t(t) < target.bones.size()) {
                hands[t] = int(i);
                out.sourceToTarget[i] = t;
            }
        }
        int mount = -1;
        for (size_t t = 0; t < target.bones.size(); t++) {
            auto &bone = target.bones[t];
            M4 local = pose_matrix({bone.position, bone.scale, bone.rotation});
            M4 world = bone.parent >= 0 ? target.bones[bone.parent].world * local : local;
            if (hands.contains(int(t))) {
                int i = hands.at(int(t));
                world = M4(out.alignment * glm::transpose(fitted.fit[i]) *
                           glm::dmat3(fitted.model.bones[t].world));
                world[3] = glm::dvec4(
                    out.alignment * (DV3(native.bones[i].world[3]) - out.origin) * 100.0, 1);
                mapping[native.bones[i].name] = bone.name;
            }
            if (bone.name == "tag_weapon") {
                mount = int(t);
                world = M4(1);
            }
            local =
                bone.parent >= 0 ? glm::inverse(target.bones[bone.parent].world) * world : world;
            decompose(local, bone.position, bone.rotation, bone.scale);
            bone.world = world;
            bone.inverseBind = glm::inverse(world);
        }
        require(mount >= 0, "Viewhands profile lacks weapon mount");
        std::vector<int> remap(weaponBones.size(), -1);
        remap[0] = 0;
        remap[1] = mount;
        for (size_t i = 2; i < weaponBones.size(); i++) {
            Bone bone = weaponBones[i];
            bone.parent = remap.at(bone.parent);
            auto local = glm::inverse(target.bones[bone.parent].world) * bone.world;
            decompose(local, bone.position, bone.rotation, bone.scale);
            remap[i] = int(target.bones.size());
            target.bones.push_back(bone);
        }
        for (size_t i = 0; i < weaponMapping.size(); i++)
            if (weaponMapping[i] >= 0)
                out.sourceToTarget[i] = remap.at(weaponMapping[i]);
        for (auto &surface : target.surfaces)
            for (auto &joints : surface.joints)
                for (int k = 0; k < 4; k++)
                    joints[k] = remap.at(joints[k]);
        out.weaponRoot = remap[2];
        out.weaponMount = mount;
        out.kind = "weapon-hands";
        target.report["t6Compatibility"]["profile"] = "t6-seal6-weapon-hands";
        target.report["t6Compatibility"]["mapping"] = mapping;
        target.report["t6Compatibility"]["referenceSource"] = profile.at("source");
        target.report["t6Compatibility"]["referenceSHA256"] = profile.at("sha256");
        target.report["t6Compatibility"]["semanticCoverage"] =
            "Avatar-owned arms and fingers, weapon root and evidenced mechanisms";
        target.report["t6Compatibility"]["handMotion"] =
            "Source joint world positions; anatomical orientation fit to SEAL6 "
            "rest frames. Unmapped reference helpers inherit their parent.";
        target.report["t6Compatibility"]["mappedHandBones"] = hands.size();
    }
    update_worlds(target);
    bool firstPersonWeapon = out.kind == "weapon" || out.kind == "weapon-hands";
    target.report["t6Compatible"] = !firstPersonWeapon;
    if (firstPersonWeapon) {
        target.report["t6Compatibility"]["completeArmHierarchy"] = out.kind == "weapon-hands";
        target.report["t6Compatibility"]["validationStatus"] =
            "Native conversion; consumer visual acceptance pending";
        target.report["t6Compatibility"]["unitScale"] = 100.0;
        target.report["t6Compatibility"]["commonSourceOrigin"] = {out.origin.x, out.origin.y,
                                                                  out.origin.z};
        target.report["t6Compatibility"]["scaleEvidence"] =
            "Complete prefab world transforms; metres to centimetres once for "
            "geometry, joints and motion. No model-size multiplier.";
    }
    target.report["units"] = "centimetres";
    target.report["bones"] = target.bones.size();
    target.report["t6Compatibility"]["nativeBones"] = bones_json(native);
    target.report["t6Compatibility"]["targetBones"] = bones_json(target);
    auto source = native.report.value("source", J::object());
    bool viewWeapon = source.value("category", std::string()) == "Weapon" &&
                      (source.value("perspective", std::string()) == "view" ||
                       lower(source.value("name", std::string())).ends_with("_1p"));
    if (fitFirstPerson &&
        ((viewWeapon && out.kind == "weapon-hands") ||
         (source.value("category", std::string()) == "Viewhands" && out.kind == "viewhands"))) {
        Animation pose;
        pose.name = "source bind reference";
        pose.frames = 1;
        std::vector<Pose> local;
        for (size_t i = 0; i < native.bones.size(); ++i) {
            auto &b = native.bones[i];
            local.push_back({b.position, b.scale, b.rotation});
            pose.animated.insert({int(i), 1});
            pose.animated.insert({int(i), 2});
        }
        pose.poses.push_back(local);
        pose.report = {{"reference", "Native bind pose for direct rig conversion"}};
        configure_candidate13(out, native, pose, dataRoot);
    }
    return out;
}
Animation convert_animation_rig(const Model &native, const RigConversion &conversion,
                                const Animation &anim, JobContext *job) {
    if (conversion.candidateSource)
        return convert_candidate13(native, conversion, anim, job);
    require(conversion.kind != "static", "Static geometry has no animation rig");
    auto &target = conversion.model;
    Animation out = anim;
    out.poses.clear();
    out.animated.clear();
    out.relativeTranslation.clear();
    std::vector<int> targetToSource(target.bones.size(), -1);
    for (size_t i = 0; i < conversion.sourceToTarget.size(); i++)
        if (conversion.sourceToTarget[i] >= 0)
            targetToSource[conversion.sourceToTarget[i]] = int(i);
    bool weapon = conversion.kind == "weapon" || conversion.kind == "weapon-hands";
    if (weapon && conversion.weaponMount >= 0)
        targetToSource[conversion.weaponMount] = targetToSource[conversion.weaponRoot];
    if (weapon) {
        // Only channels affected by authored bindings in the source-to-target
        // parent interval belong to this clip. Shared ancestor motion cancels.
        for (size_t t = 0; t < target.bones.size(); t++) {
            int i = targetToSource[t];
            if (i < 0)
                continue;
            int p = target.bones[t].parent;
            while (p >= 0 && targetToSource[p] < 0)
                p = target.bones[p].parent;
            int sp = p >= 0 ? targetToSource[p] : -1;
            std::set<int> left, right;
            for (int n = i; n >= 0; n = native.bones[n].parent)
                left.insert(n);
            for (int n = sp; n >= 0; n = native.bones[n].parent)
                right.insert(n);
            for (auto [bone, attr] : anim.animated) {
                if (left.contains(bone) == right.contains(bone))
                    continue;
                if (attr == 2)
                    out.animated.insert({int(t), 2});
                if (attr == 3)
                    out.animated.insert({int(t), 3});
                if (attr == 1 || attr == 3 || (attr == 2 && (bone != i || right.contains(bone))))
                    out.animated.insert({int(t), 1});
            }
        }
    } else if (conversion.kind == "rigid") {
        for (auto [i, attr] : anim.animated)
            out.animated.insert({conversion.sourceToTarget.at(i), attr});
    }
    for (int f = 0; f < anim.frames; f++) {
        if (job && f % 16 == 0)
            job->check();
        auto current = pose_worlds(native, anim.poses[f]);
        std::vector<M4> desired(target.bones.size(), M4(1));
        std::vector<Pose> poses(target.bones.size());
        if (conversion.kind == "rigid") {
            M4 axis(conversion.alignment);
            for (size_t i = 0; i < current.size(); i++) {
                desired[i] = axis * current[i] * glm::transpose(axis);
                desired[i][3] = glm::dvec4(DV3(desired[i][3]) * 100.0, 1);
            }
        } else if (weapon) {
            for (size_t t = 0; t < target.bones.size(); t++) {
                auto &bone = target.bones[t];
                auto rest = pose_matrix({bone.position, bone.scale, bone.rotation});
                desired[t] = bone.parent >= 0 ? desired[bone.parent] * rest : rest;
                int i = targetToSource[t];
                if (i < 0)
                    continue;
                require(glm::length(anim.poses[f][i].scale - native.bones[i].scale) < .004,
                        "Animated weapon scale changes: " + native.bones[i].name);
                M4 world(conversion.alignment * glm::dmat3(current[i]) *
                         glm::inverse(glm::dmat3(native.bones[i].world)) *
                         glm::transpose(conversion.alignment) * glm::dmat3(bone.world));
                world[3] = glm::dvec4(
                    conversion.alignment * (DV3(current[i][3]) - conversion.origin) * 100.0, 1);
                desired[t] = world;
            }
        } else {
            for (size_t t = 0; t < target.bones.size(); t++) {
                auto &bone = target.bones[t];
                M4 restLocal = pose_matrix({bone.position, bone.scale, bone.rotation});
                M4 world = bone.parent >= 0 ? desired[bone.parent] * restLocal : restLocal;
                int i = targetToSource[t];
                if (i >= 0) {
                    auto fit = conversion.fit[i];
                    auto rotation = fit * glm::dmat3(current[i]) *
                                    glm::inverse(glm::dmat3(native.bones[i].world)) *
                                    glm::transpose(fit) * glm::dmat3(bone.world);
                    for (int c = 0; c < 3; c++)
                        world[c] = glm::dvec4(rotation[c], 0);
                    int sp = native.bones[i].parent;
                    DV3 delta = anim.poses[f][i].position - native.bones[i].position;
                    if (sp < 0 || bone.name == "tag_origin" || bone.name == "j_mainroot")
                        world[3] = glm::dvec4(
                            DV3(bone.world[3]) +
                                fit * (DV3(current[i][3]) - DV3(native.bones[i].world[3])) * 100.0,
                            1);
                    else if (glm::length(delta) > 1e-8) {
                        DV3 translated =
                            conversion.fit[sp] * glm::dmat3(native.bones[sp].world) * delta * 100.0;
                        if (bone.parent >= 0)
                            translated = glm::dmat3(desired[bone.parent]) *
                                         glm::inverse(glm::dmat3(target.bones[bone.parent].world)) *
                                         translated;
                        world[3] += glm::dvec4(translated, 0);
                    }
                }
                desired[t] = world;
            }
        }
        for (size_t i = 0; i < target.bones.size(); i++) {
            int p = target.bones[i].parent;
            M4 local = p >= 0 ? glm::inverse(desired[p]) * desired[i] : desired[i];
            try {
                decompose(local, poses[i].position, poses[i].rotation, poses[i].scale);
            } catch (const std::exception &e) {
                throw std::runtime_error("Animation " + anim.name + " frame " + std::to_string(f) +
                                         " bone " + target.bones[i].name + ": " + e.what() +
                                         " matrix " + matrix_json(local).dump());
            }
            if (f && glm::dot(out.poses[f - 1][i].rotation, poses[i].rotation) < 0)
                poses[i].rotation = -poses[i].rotation;
            if (!weapon && conversion.kind != "rigid")
                for (int attr : {1, 2, 3})
                    out.animated.insert({int(i), attr});
            if (weapon && int(i) >= conversion.weaponRoot)
                out.relativeTranslation.insert(int(i));
        }
        out.poses.push_back(std::move(poses));
    }
    out.report["t6Compatibility"] = target.report.at("t6Compatibility");
    out.report["t6Compatibility"].erase("nativeBones");
    out.report["t6Compatibility"].erase("targetBones");
    out.report["units"] = "centimetres";
    out.report["motionConversion"] =
        "sampled world motion converted into target hierarchy and rest frames";
    if (weapon) {
        J owned = J::array(), missing = J::array();
        for (auto [i, attr] : out.animated)
            owned.push_back({{"bone", target.bones[i].name}, {"attribute", attr}});
        for (auto side : {"le", "ri"})
            for (auto part :
                 {"shoulder", "elbow", "wrist", "thumb", "index", "mid", "ring", "pinky"}) {
                std::string name = "j_" + std::string(part) + "_" + side;
                bool finger = std::string(part) != "shoulder" && std::string(part) != "elbow" &&
                              std::string(part) != "wrist";
                if (finger)
                    name += "_1";
                bool found = false;
                for (auto [i, attr] : out.animated)
                    found |= target.bones[i].name == name && attr == 2;
                if (!found)
                    missing.push_back(name);
            }
        out.report["ownedTargetChannels"] = owned;
        out.report["armCoverage"] = {{"missingRotationTracks", missing},
                                     {"completeForThisClip", missing.empty()},
                                     {"scope", "Authored clip ownership only; sparse actions may "
                                               "intentionally omit arms"}};
        out.report["channelOwnership"] =
            "Only authored source channels and dependencies introduced by "
            "reparenting; no blanket rest keys";
    }
    return out;
}
} // namespace codm
