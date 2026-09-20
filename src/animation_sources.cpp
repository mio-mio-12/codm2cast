#include "animation.hpp"
#include "library.hpp"
#include "materials.hpp"

namespace codm {
namespace {
J pointer(Reader &reader) {
    return {{"m_FileID", reader.get<int32_t>()}, {"m_PathID", reader.get<int64_t>()}};
}
J clip_reference(Source &source, Object &owner, const J &reference) {
    auto *clip = source.ref(owner, reference);
    if (!clip)
        return nullptr;
    require(clip->cid == 74, "Controller references a non-animation asset: " + clip->id());
    Reader reader(clip->raw(), clip->file->big);
    auto name = reader.str();
    return {{"id", clip->id()},
            {"name", name},
            {"bundle", clip->file->bundle},
            {"type", "animation"},
            {"category", animation_category(name, hint(clip->file->bundle))},
            {"action", action_name(name)}};
}
J override_declarations(Source &source, Object &controller, JobContext *job) {
    require(controller.cid == 221, "Assigned controller is not an override controller");
    require(controller.file->version.starts_with("5.6.4p4"), "Unverified controller version");
    Reader reader(controller.raw(), controller.file->big);
    J out = {
        {"id", controller.id()}, {"name", reader.str()}, {"pairs", J::array()}, {"errors", J::array()}};
    auto *base = source.ref(controller, pointer(reader));
    require(base && base->cid == 91, "Override controller has no supported base controller");
    out["baseController"] = base->id();
    const auto count = reader.count(24, 100000);
    std::map<std::string, J> originals;
    bool conflicting = false;
    for (int i = 0; i < count; i++) {
        if (job)
            job->check();
        const auto originalPointer = pointer(reader), replacementPointer = pointer(reader);
        try {
            auto original = clip_reference(source, controller, originalPointer);
            require(!original.is_null(), "Null original clip in override declaration");
            auto replacement = clip_reference(source, controller, replacementPointer);
            auto effective = replacement.is_null() ? original : replacement;
            auto id = original.at("id").get<std::string>();
            auto [at, inserted] = originals.emplace(id, effective.at("id"));
            conflicting |= !inserted && at->second != effective.at("id");
            require(!conflicting, "Conflicting overrides for source clip " + id);
            out["pairs"].push_back({{"original", original},
                                    {"effective", effective},
                                    {"overridden", !replacement.is_null()}});
        } catch (const std::exception &e) {
            out["errors"].push_back({{"pair", i}, {"error", e.what()}});
        }
    }
    require(!conflicting, "Controller contains conflicting override declarations");
    // This is a declaration reader, not a controller-state decoder. The CODM
    // extension is preserved as provenance and never interpreted as motion or
    // accepted through Source::tree's full-object size check.
    out["declarationBytes"] = reader.pos;
    out["uninterpretedStateBytes"] = reader.data.size() - reader.pos;
    out["uninterpretedStateSHA256"] = sha256(reader.data.subspan(reader.pos));
    out["scope"] = "Serialized clip declarations only; controller state reachability is not inferred";
    return out;
}
} // namespace

J discover_animation_sources(Source &source, const J &entry, JobContext *job) {
    J out = {{"source", entry.value("id", std::string())},
             {"controllers", J::array()},
             {"clips", J::array()},
             {"errors", J::array()}};
    require(is_weapon_entry(entry), "Shared weapon motion requires a Weapon category model");
    Hierarchy hierarchy(source);
    std::set<std::string> roots, visitedControllers;
    std::map<std::string, J> clips;
    auto renderers=entry.value("renderers",J::array({{{"id",entry.at("id")}}}));
    // Geometry-identical patched prefabs can own the Animator while the chosen
    // original prefab deliberately has no controller. Inspect indexed copies.
    for(const auto &id:entry.value("duplicateInstances",J::array()))renderers.push_back({{"id",id}});
    for (const auto &renderer : renderers) {
        try {
            auto &object = source.object(renderer.at("id"));
            auto &root = hierarchy.root(*owner_transform(source, object));
            require(weapon_identity(hierarchy.name(root))==weapon_identity(entry.at("name")),"Controller owner belongs to another weapon variant");
            if (!roots.insert(root.id()).second)
                continue;
            auto *go = source.ref(root, source.tree(root).at("m_GameObject"));
            require(go && go->cid == 1, "Weapon hierarchy root has no GameObject");
            for (const auto &component : source.tree(*go).at("m_Component")) {
                if (job)
                    job->check();
                auto *animator = source.ref(
                    *go, component.contains("component") ? component.at("component") : component);
                if (!animator || animator->cid != 95)
                    continue;
                require(animator->file->version.starts_with("5.6.4p4") && animator->size == 104,
                        "Unverified Animator declaration layout: " + animator->id());
                Reader reader(animator->raw(), animator->file->big);
                auto *owner = source.ref(*animator, pointer(reader));
                require(owner == go, "Animator ownership differs from selected hierarchy");
                require(reader.get<uint8_t>() <= 1, "Invalid Animator enabled flag");
                reader.align();
                pointer(reader); // Avatar identity is independent of controller clip declarations.
                auto *controller = source.ref(*animator, pointer(reader));
                if (!controller || !visitedControllers.insert(controller->id()).second)
                    continue;
                auto declaration = override_declarations(source, *controller, job);
                declaration["animator"] = animator->id();
                declaration["rootTransform"] = root.id();
                for (const auto &pair : declaration.at("pairs")) {
                    auto clip = pair.at("effective");
                    auto id = clip.at("id").get<std::string>();
                    if (!clips.contains(id)) {
                        clip["controllerReferences"] = J::array();
                        clips[id] = std::move(clip);
                    }
                    clips[id]["controllerReferences"].push_back(
                        {{"controller", controller->id()},
                         {"animator", animator->id()},
                         {"rootTransform", root.id()},
                         {"original", pair.at("original").at("id")},
                         {"originalName", pair.at("original").at("name")},
                         {"overridden", pair.at("overridden")}});
                }
                out["controllers"].push_back(std::move(declaration));
            }
        } catch (const std::exception &e) {
            if (job)
                job->check();
            out["errors"].push_back({{"renderer", renderer.at("id")}, {"error", e.what()}});
        }
    }
    for (auto &[id, clip] : clips)
        out["clips"].push_back(std::move(clip));
    bool unresolved = !out["errors"].empty();
    for (const auto &controller : out["controllers"])
        unresolved |= !controller.at("errors").empty();
    out["status"] = unresolved ? "partial" : "declared";
    return out;
}

J fill_base_animation_slots(const J &selected, const J &baseClips, const J &baseEntry) {
    J result = selected;
    std::set<std::string> occupied, ids;
    for (const auto &clip : selected) {
        occupied.insert(action_name(clip.at("name")));
        ids.insert(clip.at("id"));
    }
    // Keep all distinct base clips for a missing action. Filename planning
    // already preserves duplicate source actions and their camera partners.
    for (auto clip : baseClips) {
        auto action = action_name(clip.at("name"));
        if (occupied.contains(action) || !ids.insert(clip.at("id")).second)
            continue;
        clip["inheritance"] = "Missing variant action supplied by the base weapon";
        clip["baseWeapon"] = baseEntry;
        result.push_back(std::move(clip));
    }
    return result;
}

J weapon_animation_set(Source &source, const fs::path &database, const J &entry,
                       J *report, JobContext *job) {
    auto indexed = entry;
    indexed.erase("declaredAnimationIDs");
    AnimationFilter filter(indexed);
    J exact = J::array(), indexedClips = J::array();
    for (auto category : {"Weapon", "Etc"})
        for (auto &clip : library_rows(database, "animation", category)) {
            if (filter.match(clip) == AnimationMatch::Applicable)
                exact.push_back(clip);
            indexedClips.push_back(std::move(clip));
        }
    J evidence;
    auto declarations = discover_animation_sources(source, entry, job);
    auto combined = inherit_animation_sources(entry, exact, declarations, &evidence);
    auto identity = weapon_identity(entry.at("name"));
    require(identity.has_value(), "No weapon identity for base animation inheritance");
    auto base = base_weapon(database, *identity);
    evidence["baseWeapon"] = base;
    if (!base.is_null()) {
        AnimationFilter baseFilter(base);
        J baseClips = J::array();
        for (const auto &clip : indexedClips)
            if (baseFilter.match(clip) == AnimationMatch::Applicable)
                baseClips.push_back(clip);
        auto baseDeclarations = discover_animation_sources(source, base, job);
        baseClips = inherit_animation_sources(base, baseClips, baseDeclarations);
        combined = fill_base_animation_slots(combined, baseClips, base);
        evidence["baseDeclarations"] = std::move(baseDeclarations);
    }
    evidence["added"] = J::array();
    evidence["unavailableActions"] = J::array();
    std::set<std::string> supplied;
    for (const auto &clip : combined) supplied.insert(action_name(clip.at("name")));
    for (const auto &clip : evidence.at("declarations").at("clips")) {
        auto action=action_name(clip.at("name"));
        if (supplied.contains(action) || (action!="fire" && action!="ads_fire")) continue;
        if (!lower(clip.at("name")).starts_with("empty_")) continue;
        try {
            auto decoded=decode_clip(source,source.object(clip.at("id")),job);
            if (decoded.frames==0 && decoded.columns==0)
                evidence["unavailableActions"].push_back({{"action",action},{"source",clip.at("id")},{"name",clip.at("name")},
                    {"reason","Assigned controller slot contains an empty placeholder (zero frames and curves); no authored motion to export"}});
        } catch(const std::exception &e) { if(job)job->check(); evidence["unavailableActions"].push_back({{"action",action},{"source",clip.at("id")},{"reason",e.what()}}); }
    }
    for (const auto &clip : combined)
        if (clip.contains("inheritance"))
            evidence["added"].push_back(clip.at("id"));
    if (report)
        *report = std::move(evidence);
    return combined;
}

J inherit_animation_sources(const J &entry, const J &selected, const J &declarations, J *report) {
    require(is_weapon_entry(entry), "Animation inheritance requires a weapon model");
    J result = selected, added = J::array(), skipped = J::array();
    const auto target = *weapon_identity(entry.at("name"));
    std::set<std::string> ids, actions;
    for (const auto &clip : selected) {
        ids.insert(clip.at("id"));
        actions.insert(action_name(clip.at("name")));
    }
    std::map<std::string, std::vector<J>> byAction;
    for (auto clip : declarations.at("clips")) {
        auto identity = weapon_identity(clip.at("name"));
        auto id = clip.at("id").get<std::string>();
        if (ids.contains(id))
            continue;
        bool replaced = false;
        for (const auto &reference : clip.value("controllerReferences", J::array()))
            replaced |= reference.value("overridden", false) &&
                        reference.value("original", std::string()) != id;
        if (!replaced) {
            skipped.push_back(
                {{"source", id},
                 {"reason", "Unchanged generic controller slot; reachability not established"}});
            continue;
        }
        if (clip.at("category") != "Weapon" || !identity || identity->second != target.second) {
            skipped.push_back({{"source", id}, {"reason", "Different target category or perspective"}});
            continue;
        }
        auto action = action_name(clip.at("name"));
        if (actions.contains(action)) {
            skipped.push_back(
                {{"source", id}, {"reason", "Selected variant already supplies this action"}});
            continue;
        }
        clip["inheritance"] =
            "Assigned override controller declares this clip for the selected hierarchy";
        byAction[action].push_back(std::move(clip));
    }
    for (auto &[action, candidates] : byAction) {
        if (candidates.size() != 1) {
            skipped.push_back(
                {{"action", action}, {"reason", "Multiple declared clips for missing action"}});
            continue;
        }
        added.push_back(candidates.front().at("id"));
        result.push_back(std::move(candidates.front()));
    }
    if (report)
        *report = {{"added", added}, {"skipped", skipped}, {"declarations", declarations}};
    return result;
}
} // namespace codm
