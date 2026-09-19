// Opt-in integration check: real source work through the rendered widgets, in
// a hidden window with a private database, settings, cache and destinations.
struct StudioSourceInputCheck : StudioInputCheck {
    J plan;
    fs::path evidence;
    explicit StudioSourceInputCheck(Studio &studio, J configuration, fs::path output)
        : StudioInputCheck(studio, false), plan(std::move(configuration)),
          evidence(output.parent_path()) {
        scope =
            "Actual Dear ImGui widgets and production background jobs in a hidden native window, "
            "using real CODM models/clips, a private library copy, isolated settings and fresh "
            "destinations. No desktop input, native folder picker or live Cadence interaction.";
        app.completeOnly = false;
        app.quality = 1;
        app.catalog = pathof(plan.at("catalog"));
        app.database = pathof(plan.at("database"));
        app.data = pathof(plan.at("data"));
        if (plan.contains("builder")) {
            scope = "Actual Dear ImGui builder widgets and production jobs in a hidden native "
                    "window, using real CODM parts with isolated settings, cache and preview. No "
                    "desktop input or Cadence execution.";
            auto setup = plan.at("builder");
            app.modelTab = 1;
            app.models[1] = {setup.at("model"), setup.at("donor")};
            add([] {});
            click("Model row/" + setup.at("model").at("id").get<std::string>());
            click("Build weapon");
            wait_until("Weapon builder exposes discovered slots",
                       [this] { return !app.profile.is_null() && !active("Discover parts"); });
            check("Pistol builder shows variant and automatic base choices", [this] {
                return items.contains("Part slot/rai") && !app.profile.at("baseWeapon").is_null() &&
                       app.profile.at("slots").size() >= 8 && !app.preview;
            });
            click("Part slot/rai");
            click("Part option/rai/" + setup.at("baseSlideMesh").get<std::string>());
            wait_until("Selected base slide reaches textured preview", [this] {
                return app.preview && app.materials && !active("Prepare preview");
            });
            check("Base slide choice drives the actual preview assembly", [this, setup] {
                bool included = false;
                for (auto &part : app.preview->report.at("parts"))
                    included |= part.at("mesh") == setup.at("baseSlideMesh") &&
                                part.value("baseFallback", false);
                return included && app.choices.at("rai") == setup.at("baseSlideMesh");
            });
            click("Part slot/mag");
            click("Part None/mag");
            wait_until("Magazine removal finishes", [this] { return !active("Prepare preview"); });
            click("Build weapon");
            wait_until("Refresh preserves custom assembly",
                       [this] { return !active("Discover parts"); });
            check("Explicit None survives automatic base fallback and refresh", [this, setup] {
                return app.choices.at("mag").is_null() &&
                       app.choices.at("rai") == setup.at("baseSlideMesh");
            });
            click("Load part set");
            click("Donor row/" + setup.at("donor").at("id").get<std::string>());
            wait_until("Additional donor set loads",
                       [this] { return !active("Load donor parts"); });
            check("Donor set adds options without replacing choices", [this, setup] {
                bool donor = false;
                for (auto &slot : app.profile.at("slots"))
                    for (auto &option : slot.at("options"))
                        donor |= option.value("manualPlacement", false) &&
                                 !option.value("baseFallback", false);
                return donor && app.choices.at("mag").is_null() &&
                       app.choices.at("rai") == setup.at("baseSlideMesh");
            });
            add([this] {
                measurements["builderProfile"] = app.profile;
                measurements["builderChoices"] = app.choices;
            });
            return;
        }
        for (int i = 0; i < 4; i++) {
            app.models[i] = {plan.at("cases").at(i).at("model")};
            app.clips[i] = {plan.at("cases").at(i).at("clip")};
        }
        app.models[1].push_back(plan.at("niche"));
        add([] {});
        for (int i = 0; i < 4; i++) {
            choose(i);
            check(categories[i] + " real rows remain text-only", [this, i] {
                return app.selected.at("id") == plan.at("cases").at(i).at("model").at("id") &&
                       app.selectedClip.at("id") == plan.at("cases").at(i).at("clip").at("id") &&
                       !app.preview && !app.materials && !app.animation && app.tasks.empty();
            });
        }
        choose(0);
        click("Index model tab");
        wait_until("Real incremental index job starts", [this] { return active("Index library"); });
        click("Prepare selected model");
        geometry_first(0);
        wait_until("Committed source rows become visible during indexing",
                   [this] { return app.models[0].size() > 1 && active("Index library"); });
        add([this] { measurements["incrementalPlayerRows"] = app.models[0].size(); });
        click("Cancel task/Index library");
        wait_until("Index cancellation finishes and restores the previous tab", [this] {
            return !active("Index library") && app.models[0].size() == 1 &&
                   app.clips[0].size() == 1 &&
                   app.models[0][0].at("id") == plan.at("cases")[0].at("model").at("id");
        });
        ready_and_animate(0);
        for (int i = 1; i < 4; i++) {
            choose(i);
            click("Prepare selected model");
            geometry_first(i);
            ready_and_animate(i);
        }
        // Change selection while a real prop reload is running. Its late result
        // must never replace the newly selected weapon's geometry or textures.
        click("Prepare selected model");
        wait_until("A real preview reload starts before changing selection",
                   [this] { return active("Prepare preview"); });
        click("Models/Weapon");
        click("Model row/" + plan.at("niche").at("id").get<std::string>());
        click("Prepare selected model");
        wait_until("Niche preview survives cancelled stale source work", [this] {
            return app.tasks.empty() && app.preview && app.materials &&
                   app.materials->errors.empty() &&
                   app.preview->report.at("source").at("id") == plan.at("niche").at("id");
        });
        check("Niche weapon discovers its full five-part assembly", [this] {
            return app.preview->report.at("parts").size() == 5 && app.profile.is_object();
        });
        click("Part slot/mag");
        click("Part None/mag");
        wait_until("Explicit None persists after real geometry and texture preparation", [this] {
            return app.tasks.empty() && app.choices.at("mag").is_null() && app.preview &&
                   app.materials && app.materials->errors.empty() &&
                   app.preview->report.at("parts").size() == 4;
        });
        add([this] {
            snapshot = evidence / "niche-custom.png";
            measurements["nicheCustom"] = app.preview->report;
        });
        click("Open export");
        check("Niche model-only export starts with no unselected animation", [this] {
            return app.exportModel && !app.exportAnimations &&
                   !items.contains("Animations destination");
        });
        edit("Export name", "oni_no_mag_ui");
        edit("Models destination", pathstr(evidence / "custom/models"));
        click("Start export");
        wait_until("Customized niche model exports from the actual dialog", [this] {
            return !active("Export assets") && app.status.starts_with("Export exported");
        });
        add([this] {
            auto report = J::parse(app.details);
            require(report.at("parts").size() == 4 && report.at("model").at("status") == "exported",
                    "Customized UI export restored the removed magazine");
            for (auto &part : report.at("parts"))
                require(part.at("slot") != "mag", "Removed magazine appears in UI export report");
            measurements["customExport"] = report;
        });
        // Exercise the actual Export modal with a valid player model and clip.
        // The destinations have different immediate parents.
        choose(0);
        click("Open export");
        edit("Export name", "ghost_ui");
        edit("Models destination", plan.at("modelsDestination"));
        edit("Animations destination", plan.at("animationsDestination"));
        click("Start export");
        check("Real combined export closes its modal on acceptance",
              [this] { return !items.contains("Start export") && active("Export assets"); });
        wait_until("Real model and animation export finishes", [this] {
            return !active("Export assets") && app.status.starts_with("Export exported");
        });
        add([this] {
            auto report = J::parse(app.details);
            require(report.at("status") == "exported" && report.at("exportedAnimations") == 1,
                    "UI export did not publish its selected clip");
            require(fs::exists(pathof(app.modelsDestination) / (app.exportStem + ".cast")),
                    "UI model destination not honored");
            require(report.at("baselineApplicable") == false &&
                        report.at("baselineComplete").is_null(),
                    "Player UI export acquired a weapon baseline");
            measurements["export"] = report;
            measurements["modelsDestination"] = app.modelsDestination;
            measurements["animationsDestination"] = app.animationsDestination;
        });
        click("Open export");
        click("Start export");
        check("Actual exported model is protected from overwrite in the modal", [this] {
            return app.exportError == "Model already exists. Change name or destination." &&
                   items.contains("Export error") && app.tasks.empty();
        });
        click("Cancel export");
    }
    bool active(const std::string &name) const {
        return std::any_of(app.tasks.begin(), app.tasks.end(),
                           [&](auto &task) { return task->name == name && !task->done; });
    }
    void choose(int i) {
        click("Models/" + categories[i]);
        click("Model row/" + plan.at("cases").at(i).at("model").at("id").get<std::string>());
        click("Animations/" + categories[i]);
        click("Clip row/" + plan.at("cases").at(i).at("clip").at("id").get<std::string>());
    }
    void geometry_first(int i) {
        wait_until(categories[i] + " geometry appears before materials", [this, i] {
            return app.preview && !app.materials &&
                   app.preview->report.at("source").at("id") ==
                       plan.at("cases").at(i).at("model").at("id");
        });
    }
    void ready_and_animate(int i) {
        wait_until(categories[i] + " fullbright textures finish loading", [this] {
            return !active("Prepare preview") && app.preview && app.materials &&
                   app.materials->errors.empty() && !app.textures.empty();
        });
        add([this, i] { measurements[categories[i]] = app.preview->report; });
        click("Preview selected animation");
        wait_until(categories[i] + " native source playback advances",
                   [this] { return app.animation && app.playing && app.frame > 2; });
        add([this, i] { snapshot = evidence / (lower(categories[i]) + "-before-pause.png"); });
        click("Toggle playback");
        check(categories[i] + " playback pauses from the actual widget",
              [this] { return app.animation && !app.playing && app.frame > 0; });
        add([this, i] {
            snapshot = evidence / (lower(categories[i]) + "-playing.png");
            measurements[categories[i]]["animation"] = app.animation->report;
            measurements[categories[i]]["pausedFrame"] = app.frame;
            measurements[categories[i]]["framing"] = check_framing();
        });
    }
    J check_framing() const {
        require(app.animationBounds.has_value(), "Animation did not publish framing bounds");
        V3 eye = app.center + app.distance * V3(std::cos(app.pitch) * std::cos(app.yaw),
                                                std::cos(app.pitch) * std::sin(app.yaw),
                                                std::sin(app.pitch));
        auto projection =
            glm::perspective(glm::radians(38.f), app.previewSize.x / app.previewSize.y,
                             std::max(.0001f, app.distance * .001f), app.distance * 100 + 100);
        auto view = glm::lookAt(eye, app.center, V3(0, 0, 1));
        M4 camera = M4(projection * view);
        double maximum = 0;
        size_t vertices = 0;
        for (int sample = 0; sample < app.animation->frames * 2 - 1; sample++) {
            auto palette = sample_palette(*app.preview, *app.animation, sample * .5);
            for (const auto &surface : app.preview->surfaces)
                for (size_t i = 0; i < surface.positions.size(); i++) {
                    glm::dvec4 original(surface.positions[i], 1), point(0);
                    if (surface.weights.empty())
                        point = original;
                    else
                        for (int k = 0; k < 4; k++)
                            if (surface.weights[i][k] > 0)
                                point += double(surface.weights[i][k]) *
                                         (palette.at(surface.joints[i][k]) * original);
                    auto clip = camera * point;
                    require(clip.w > 0, "Animated vertex is behind the preview camera");
                    maximum =
                        std::max({maximum, std::abs(clip.x / clip.w), std::abs(clip.y / clip.w)});
                    require(std::abs(clip.z / clip.w) <= 1.0001,
                            "Animated vertex misses preview depth range");
                    vertices++;
                }
        }
        require(maximum < 1, "Animation still moves outside the fitted viewport");
        return {{"posedVerticesChecked", vertices},
                {"maximumNormalizedScreenExtent", maximum},
                {"sampledFramesAndMidpoints", app.animation->frames * 2 - 1},
                {"scope",
                 "Actual skinned vertices projected through the native preview camera; all "
                 "source frames and interval midpoints"}};
    }
};
