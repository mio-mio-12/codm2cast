#include "export.hpp"
#include "exported_animations.hpp"
namespace codm {
J corresponding_worldmodel(const J &entry, const std::vector<J> &rows) {
    auto identity = weapon_identity(entry.at("name"));
    require(identity && identity->second == "1p", "Worldmodel pairing requires a first-person weapon");
    J best = nullptr;
    std::set<std::string> meshes;
    int bestRank = -1;
    bool ambiguous = false;
    for (const auto &row : rows) {
        auto candidate = weapon_identity(row.at("name"));
        if (!candidate || candidate->first != identity->first || candidate->second != "3p" ||
            !row.value("complete", false) || row.value("lod", std::string("0")) != "0" ||
            row.contains("error"))
            continue;
        int rank = row.value("bundle", std::string()) == entry.value("bundle", std::string()) ? 1 : 0;
        std::set<std::string> signature;
        for (const auto &renderer : row.at("renderers"))
            signature.insert(renderer.at("mesh"));
        if (rank > bestRank) {
            best = row;
            bestRank = rank;
            meshes = signature;
            ambiguous = false;
        } else if (rank == bestRank) {
            ambiguous |= signature != meshes;
            if (row.at("id").get<std::string>() < best.at("id").get<std::string>())
                best = row;
        }
    }
    require(!best.is_null(), "No matching LOD0 _3P weapon is indexed. Refresh the Weapon library.");
    require(!ambiguous, "Multiple different matching _3P models; select the worldmodel explicitly");
    return best;
}

static std::string quote_profile(std::string s) {
    for (auto &c : s)
        if (c == '"' || c == '\r' || c == '\n')
            c = ' ';
    return '"' + s + '"';
}
static J export_assets_impl(const ExportRequest &r, JobContext *job) {
    require(!r.stem.empty() && r.stem.find_first_of("\\/:*?\"<>|") == r.stem.npos,
            "Invalid export name");
    require(r.model || r.allWeaponClips || !r.clips.empty(), "No models or animations selected");
    if (r.model)
        require(!fs::exists(r.modelsDestination / (r.stem + ".cast")) &&
                    !fs::exists(r.modelsDestination / (r.stem + ".json")) &&
                    !fs::exists(r.modelsDestination / (r.stem + "_materials")),
                "Model output already exists. Choose a different name or destination.");
    if(job) job->set_stage("1/5 | Discovering source assets and animation set");
    Source source(r.catalog, r.data);
    J worldEntry = nullptr;
    std::string worldStem;
    if (r.includeWorldmodel && r.model) {
        worldEntry = corresponding_worldmodel(r.entry, library_rows(r.database, "model", "Weapon"));
        worldStem = r.stem;
        auto marker = worldStem.find("viewmodel_");
        require(marker != std::string::npos, "Paired export requires a viewmodel filename");
        worldStem.replace(marker, 10, "worldmodel_");
        for (const auto &suffix : {".cast", ".json", "_materials"})
            require(!fs::exists(r.modelsDestination / (worldStem + suffix)),
                    "Worldmodel output already exists. Choose a different name or destination.");
    }
    J clips = r.clips, parts = r.parts, partChoices = J::object(), animationSources = nullptr;
    if (r.allWeaponClips) {
        require(is_weapon_entry(r.entry), "Weapon animation sets require a Weapon category model");
        clips = weapon_animation_set(source, r.database, r.entry, &animationSources, job);
        require(!clips.empty(),
                "No matching clips are indexed. Load the Weapon and Etc animation tabs.");
    }
    if (r.discoverParts) {
        auto profile =
            discover_parts(source, r.entry, r.catalog.parent_path() / "cache/parts", job);
        parts = resolve_parts(profile, partChoices, true);
    }
    clips = plan_animation_exports(clips);
    if (job)
        job->update(0, "Checking geometry and rig");
    if(job) job->set_stage("2/5 | Preparing geometry and rig");
    auto native = prepare_geometry(source, r.entry, parts, job);
    if(!r.existingModel.empty()) {
        require(!r.model && !r.t6,"Existing model animations require the native exported rig");
        use_exported_skeleton(native,r.existingModel);
    }
    std::optional<RigConversion> conversion;
    std::optional<Animation> fitPose;
    if (r.t6)
        conversion = convert_rig(native, r.data, false);
    bool viewWeapon =
        is_weapon_entry(r.entry) && (r.entry.value("perspective", std::string()) == "view" ||
                                     lower(r.entry.value("name", std::string())).ends_with("_1p"));
    bool viewarms = r.entry.value("category", std::string()) == "Viewhands";
    if (conversion && ((viewWeapon && conversion->kind == "weapon-hands") ||
                       (viewarms && conversion->kind == "viewhands"))) {
        J references = clips;
        AnimationFilter filter(r.entry);
        for (auto category : {"Weapon", "Etc"})
            for (auto &row : library_rows(r.database, "animation", category))
                if (!viewarms && filter.match(row) == AnimationMatch::Applicable)
                    references.push_back(row);
        std::string poseID;
        for (auto action : {"pose", "idle"}) {
            std::set<std::string> candidates;
            for (auto &row : references)
                if (animation_action(row) == action)
                    candidates.insert(row.at("id").get<std::string>());
            require(candidates.size() <= 1, "Ambiguous reference pose for T6 hand sizing");
            if (!candidates.empty()) {
                poseID = *candidates.begin();
                break;
            }
        }
        if (!poseID.empty()) {
            auto clip = decode_clip(source, source.object(poseID), job);
            fitPose = bind_animation(native, clip);
        } else {
            Animation bind;
            bind.name = "source bind reference";
            bind.frames = 1;
            std::vector<Pose> p;
            for (size_t i = 0; i < native.bones.size(); ++i) {
                auto &b = native.bones[i];
                p.push_back({b.position, b.scale, b.rotation});
                bind.animated.insert({int(i), 1});
                bind.animated.insert({int(i), 2});
            }
            bind.poses.push_back(p);
            bind.report = {
                {"reference", "Native bind pose; no unambiguous authored pose/idle was available"}};
            fitPose = std::move(bind);
        }
    }
    if (fitPose)
        configure_candidate13(*conversion, native, *fitPose, r.data);
    auto &target = conversion ? conversion->model : native;
    J report = {{"source", r.entry}, {"stem", r.stem},      {"t6Requested", r.t6},
                {"model", nullptr},  {"clips", J::array()}, {"cancelled", false},
                {"started", true}};
    auto start = Clock::now();
    report["parts"] = parts;
    report["detectedAnimations"] = clips.size();
    report["animationPlan"] = clips;
    if (!animationSources.is_null())
        report["animationSources"] = animationSources;
    report["automaticPartChoices"] = partChoices;
    report["incompleteReasons"] = J::array();
    if (conversion && viewWeapon && conversion->kind != "weapon-hands")
        report["incompleteReasons"].push_back(
            "First-person arm conversion is unavailable for this source hierarchy");
    if(job) job->set_stage("3/5 | Resolving materials and writing model");
    if (r.model) {
        try {
            auto mats = prepare_materials(source, native, r.database, r.overrides, 0, job);
            if (conversion) {
                *conversion = convert_rig(native, r.data, false);
                if (fitPose)
                    configure_candidate13(*conversion, native, *fitPose, r.data);
            }
            if(r.omitUnresolved)omit_unresolved_surfaces(target,mats);
            report["model"] = export_model(target, mats, r.modelsDestination, r.stem, job);
        } catch (const std::exception &e) {
            report["model"] = {{"status", "failed"}, {"error", e.what()}};
            report["model"]["details"] = native.report;
            if (job && job->cancel)
                report["cancelled"] = true;
        }
    }
    source.clear();
    std::map<std::string, std::string> published;
    std::map<std::string, std::string> publishedSources;
    size_t done = 0, skippedEmpty = 0;
    for (auto &row : clips) {
        if (job && job->cancel) {
            report["cancelled"] = true;
            break;
        }
        auto id = row.at("id").get<std::string>();
        auto action = row.at("action").get<std::string>();
        if(job) job->set_stage("4/5 | Animation " + std::to_string(done+1) + "/" + std::to_string(clips.size()) + " | " + action);
        try {
            if (job)
                job->update(float(done) / std::max(size_t(1), clips.size()),
                            "Exporting animation " + action);
            auto clip = decode_clip(source, source.object(id), job);
            if (clip.report.value("emptyPlaceholder", false)) {
                report["clips"].push_back({{"source", id},
                                           {"name", row.at("name")},
                                           {"action", action},
                                           {"status", "skipped"},
                                           {"reason", clip.report.at("reason")},
                                           {"sourceSHA256", clip.report.at("sourceSHA256")}});
                skippedEmpty++;
                done++;
                continue;
            }
            bool camera = row.at("cameraTarget");
            if (camera) {
                auto cameraModel = camera_target(source, clip);
                auto anim = bind_animation(cameraModel, clip, true);
                if (r.t6) {
                    auto rig = convert_rig(cameraModel, r.data);
                    anim = convert_animation_rig(cameraModel, rig, anim, job);
                    cameraModel = std::move(rig.model);
                }
                auto result = export_animation(cameraModel, anim, r.animationsDestination,
                                               r.stem + "_" + action, job, r.allWeaponClips);
                result["action"] = action;
                for (auto field :
                     {"pairedSource", "pairedAction", "pairingEvidence", "pairingStatus"})
                    if (row.contains(field))
                        result[field] = row.at(field);
                report["clips"].push_back(result);
                published[action] = r.stem + "_" + action + ".cast";
                publishedSources[id] = published[action];
                done++;
                continue;
            }
            auto anim = bind_animation(native, clip);
            if (viewWeapon)
                for (auto &omission : anim.report.at("omittedBindings"))
                    if (omission.value("type", 0) == 4) {
                        report["incompleteReasons"].push_back("Unresolved transform bindings in " +
                                                              action);
                        break;
                    }
            if (conversion)
                anim = convert_animation_rig(native, *conversion, anim, job);
            auto result =
                export_animation(target, anim, r.animationsDestination, r.stem + "_" + action, job, r.allWeaponClips);
            result["action"] = action;
            report["clips"].push_back(result);
            published[action] = r.stem + "_" + action + ".cast";
            publishedSources[id] = published[action];
            if (action == "pose" &&
                r.entry.value("id", std::string()) == "CAB-fc7f158fd7e2ea2b24ac46eb0c739d67:386") {
                bool hasIdle = false;
                for (auto &planned : clips)
                    hasIdle |= planned.at("action") == "idle";
                if (!hasIdle) {
                    auto alias = anim;
                    alias.report["aliasOf"] = r.stem + "_pose.cast";
                    alias.report["aliasEvidence"] =
                        "Reviewed hip pose: idle alias requested in T6 implementation handoff";
                    auto idle = export_animation(target, alias, r.animationsDestination,
                                                 r.stem + "_idle", job);
                    idle["action"] = "idle";
                    idle["aliasOf"] = r.stem + "_pose.cast";
                    report["clips"].push_back(idle);
                    published["idle"] = r.stem + "_idle.cast";
                }
            }
        } catch (const std::exception &e) {
            report["clips"].push_back({{"source", id},
                                       {"name", row.at("name")},
                                       {"action", action},
                                       {"status", "failed"},
                                       {"error", e.what()}});
        }
        done++;
    }
    for (auto &clip : report["clips"])
        if (clip.contains("pairedSource")) {
            auto id = clip.at("pairedSource").get<std::string>();
            clip["pairingStatus"] =
                publishedSources.contains(id) ? "exported" : "base action not exported";
            if (publishedSources.contains(id))
                clip["pairedAnimation"] = publishedSources.at(id);
        }
    J missing = J::array();
    const bool baselineApplicable = is_weapon_entry(r.entry);
    if (baselineApplicable)
        for (auto action : {"idle", "fire", "reload", "pullout", "putaway", "ads_up", "ads_down"})
            if (!published.contains(action))
                missing.push_back(action);
    report["baselineApplicable"] = baselineApplicable;
    report["missingBaselineActions"] = missing;
    if (r.allWeaponClips && !missing.empty())
        report["incompleteReasons"].push_back({{"reason","Weapon animation set is missing baseline actions"},{"actions",missing},
            {"sourceEvidence",animationSources.value("unavailableActions",J::array())}});
    report["exportedAnimations"] = published.size();
    report["reusedAnimations"] = std::count_if(report["clips"].begin(), report["clips"].end(),
        [](const J &clip) { return clip.value("status", std::string()) == "existing"; });
    report["skippedEmptyAnimations"] = skippedEmpty;
    report["elapsedSeconds"] = seconds(start);
    bool modelOK =
        report["model"].is_object() && report["model"].value("status", std::string()) == "exported";
    bool failures = (r.model && !modelOK) ||
                    publishedSources.size() + skippedEmpty < clips.size() ||
                    !report["incompleteReasons"].empty();
    for (auto &result : report["clips"])
        failures |= result.value("status", std::string()) == "failed";
    if (r.model && !r.viewhands.is_null() && !(job && job->cancel)) {
        try {
            require(r.viewhands.value("category", std::string()) == "Viewhands",
                    "Companion must be a viewhands model");
            require(!r.viewhandsStem.empty() && r.viewhandsStem != r.stem,
                    "Choose a different companion filename");
            ExportRequest hands = r;
            hands.entry = r.viewhands;
            hands.stem = r.viewhandsStem;
            hands.parts = J::array();
            hands.overrides = J::object();
            hands.clips = J::array();
            hands.discoverParts = false;
            hands.allWeaponClips = false;
            hands.viewhands = nullptr;
            hands.includeWorldmodel = false;
            auto companion = export_assets(hands, job);
            report["viewhands"] = {
                {"selection", "User-selected pairing; no automatic source relationship claimed"},
                {"result", companion}};
            failures |= companion.value("status", std::string()) != "exported";
        } catch (const std::exception &e) {
            report["viewhands"] = {{"status", "failed"}, {"error", e.what()}};
            failures = true;
        }
    }
    if (!worldEntry.is_null() && !(job && job->cancel)) {
      try {
        ExportRequest world = r;
        world.entry = worldEntry;
        world.stem = worldStem;
        world.includeWorldmodel = false;
        world.viewhands = nullptr;
        world.clips = J::array();
        world.allWeaponClips = false;
        world.overrides = J::object();
        world.parts = J::array();
        world.discoverParts = false;
        auto worldProfile = discover_parts(source, worldEntry, r.catalog.parent_path() / "cache/parts", job);
        J worldChoices = J::object();
        for (const auto &slot : worldProfile.at("slots"))
            for (const auto &selectedPart : parts)
                if (selectedPart.value("slot", std::string()) == slot.at("id").get<std::string>()) {
                    auto selectedIdentity = weapon_identity(selectedPart.at("name"));
                    for (const auto &option : slot.at("options")) {
                        auto optionIdentity = weapon_identity(option.at("name"));
                        if (selectedIdentity && optionIdentity && selectedIdentity->first == optionIdentity->first) {
                            worldChoices[slot.at("id").get<std::string>()] = option.at("mesh");
                            break;
                        }
                    }
                }
        world.parts = resolve_parts(worldProfile, worldChoices, true);
        auto result = export_assets(world, job);
        report["worldmodel"] = result;
        failures |= result.value("status", std::string()) != "exported";
      } catch (const std::exception &error) {
        report["worldmodel"] = {{"status", "failed"}, {"error", error.what()}};
        failures = true;
      }
    }
    if (job && job->cancel)
        report["cancelled"] = true;
    report["status"] = report["cancelled"].get<bool>() ? "cancelled"
                       : failures                      ? "partial"
                                                       : "exported";
    report["baselineComplete"] = baselineApplicable ? J(missing.empty() && modelOK) : J(nullptr);
    auto destination = r.model ? r.modelsDestination : r.animationsDestination;
    fs::create_directories(destination);
    auto reportPath =
        destination / (r.stem + "_export_report_" +
                       hex64(uint64_t(Clock::now().time_since_epoch().count())) + ".json");
    report["reportPath"] = pathstr(reportPath);
    if (modelOK && !published.empty() && is_weapon_entry(r.entry)) {
        auto profilePath = r.modelsDestination / (r.stem + ".weapon");
        if (!fs::exists(profilePath)) {
            static const std::map<std::string, std::string> categories = {
                {"ar", "rifle"},
                {"smg", "smg"},
                {"lmg", "lmg"},
                {"pistol", "pistol"},
                {"shotty", "shotgun"},
                {"launch", "launcher"},
                {"grenade", "equipment"},
                {"melee", "equipment"},
                {"device", "equipment"},
                {"special", "equipment"},
                {"equipment", "equipment"}};
            bool sniper = r.category == "sniper" || r.category == "marksman";
            require(!sniper || r.sniperArchetype == "semi_sniper" ||
                        r.sniperArchetype == "bolt_sniper" ||
                        r.sniperArchetype == "bolt_individual_sniper",
                    "Invalid sniper profile action type");
            auto archetype = sniper                            ? r.sniperArchetype
                             : categories.contains(r.category) ? categories.at(r.category)
                                                               : "equipment";
            std::ofstream profile(profilePath);
            profile << "IWWEAPON 1\nname " << quote_profile(r.title.empty() ? r.stem : r.title)
                    << "\ninternal " << quote_profile(r.stem) << "\nsource \"CODM\"\narchetype "
                    << archetype << "\nanimation_prefix " << quote_profile(r.stem + "_")
                    << (r.stem.starts_with("worldmodel_") ||
                                r.stem.find("_worldmodel_") != r.stem.npos
                            ? "\nworld_model "
                            : "\nview_model ")
                    << quote_profile(r.stem) << "\n";
            if (report.contains("viewhands") && report["viewhands"].contains("result") &&
                report["viewhands"]["result"].value("status", std::string()) == "exported")
                profile << "hand_model " << quote_profile(r.viewhandsStem) << "\n";
            if (report.contains("worldmodel") && report["worldmodel"].value("status", std::string()) == "exported")
                profile << "world_model " << quote_profile(worldStem) << "\n";
            for (auto &[action, file] : published)
                if (!action.ends_with("_camera"))
                    profile << "animation " << quote_profile(action) << " " << quote_profile(file)
                            << "\n";
            if (r.category == "melee")
                profile << "melee_weapon 1\n";
            profile.close();
            if (profile)
                report["weaponProfile"] = pathstr(profilePath);
            report["consumerArchetype"] = archetype;
            report["profileTimingNote"] =
                "Timing values were not authored; the consumer may supply its own defaults.";
        } else
            report["profileNote"] = "Existing weapon profile preserved";
    }
    if(job && !job->cancel) job->set_stage("5/5 | Writing export report");
    write_json(reportPath, report);
    return report;
}
J export_assets(const ExportRequest &request, JobContext *job) {
    require(!request.stem.empty() &&
                request.stem.find_first_of("\\/:*?\"<>|") == request.stem.npos &&
                request.stem != "." && request.stem != "..",
            "Invalid export name");
    auto destination = request.model ? request.modelsDestination : request.animationsDestination;
    require(!destination.empty(), "Choose an output destination");
    try {
        return export_assets_impl(request, job);
    } catch (const std::exception &error) {
        J report = {{"status", job && job->cancel ? "cancelled" : "failed"},
                    {"source", request.entry},
                    {"stem", request.stem},
                    {"error", error.what()},
                    {"exportedAnimations", 0},
                    {"clips", J::array()},
                    {"model", nullptr},
                    {"baselineApplicable", is_weapon_entry(request.entry)},
                    {"baselineComplete", is_weapon_entry(request.entry) ? J(false) : J(nullptr)}};
        auto path =
            destination / (request.stem + "_export_failure_" +
                           hex64(uint64_t(Clock::now().time_since_epoch().count())) + ".json");
        report["reportPath"] = pathstr(path);
        write_json(path, report);
        return report;
    }
}
} // namespace codm
