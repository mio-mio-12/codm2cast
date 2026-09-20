#include "export.hpp"

namespace codm {
J plan_animation_exports(const J &clips) {
    require(clips.is_array(), "Expected a list of animation selections");
    std::map<std::string, J> unique;
    bool firstEquip = false;
    for (const auto &clip : clips) {
        auto id = clip.at("id").get<std::string>();
        require(!id.empty(), "Animation selection has no source identity");
        auto [it, inserted] = unique.emplace(id, clip);
        require(inserted || it->second.at("name") == clip.at("name"),
                "Conflicting names for animation source " + id);
        firstEquip |= animation_action(clip) == "first_raise";
    }
    J result = J::array();
    std::set<std::string> occupied;
    for (auto &[id, clip] : unique) {
        auto sourceAction = animation_action(clip);
        bool camera = sourceAction.ends_with("_camera");
        std::string action = sourceAction;
        auto base = camera ? action.substr(0, action.size() - 7) : action;
        if (!firstEquip && (base == "pick_up" || base == "pickup")) {
            base = "first_raise";
            action = base + (camera ? "_camera" : "");
            clip["actionFallback"] = "PickUp supplies first_raise because no First_Equip is selected";
        }
        require(!action.empty() && std::all_of(action.begin(), action.end(),
                                               [](unsigned char c) {
                                                   return (c >= 'a' && c <= 'z') ||
                                                          (c >= '0' && c <= '9') || c == '_';
                                               }),
                "Invalid animation action: " + action);
        if (!occupied.insert(action).second) {
            // Preserve the camera suffix so downstream consumers cannot mistake
            // a disambiguated camera target for a gun animation.
            action = base + "_" + hex64(hash64(id)) + (camera ? "_camera" : "");
            require(occupied.insert(action).second, "Animation filename hash collision");
        }
        clip["sourceAction"] = sourceAction;
        clip["action"] = action;
        clip["cameraTarget"] = camera;
        result.push_back(clip);
    }
    for (auto &camera : result) {
        if (!camera.at("cameraTarget").get<bool>())
            continue;
        auto name = lower(camera_base_name(camera.at("name")));
        std::vector<const J *> candidates, sameBundle;
        for (const auto &other : result) {
            if (other.at("cameraTarget").get<bool>() || lower(other.at("name")) != name)
                continue;
            candidates.push_back(&other);
            if (other.value("bundle", std::string()) == camera.value("bundle", std::string()) &&
                !other.value("bundle", std::string()).empty())
                sameBundle.push_back(&other);
        }
        const auto &matches = sameBundle.empty() ? candidates : sameBundle;
        if (matches.size() == 1) {
            camera["pairedSource"] = matches.front()->at("id");
            camera["pairedAction"] = matches.front()->at("action");
            camera["pairingEvidence"] = "Exact source action name after camera suffix removal";
        } else {
            camera["pairingStatus"] = matches.empty() ? "base action not selected" : "ambiguous";
        }
    }
    return result;
}
} // namespace codm
