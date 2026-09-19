#pragma once
#include "cast_writer.hpp"
#include "rig.hpp"
namespace codm {
struct ExportRequest {
    J entry, parts = J::array(), overrides = J::object(), clips = J::array();
    fs::path catalog, database, data, modelsDestination, animationsDestination;
    fs::path existingModel;
    std::string stem, category = "ar", title, sniperArchetype = "semi_sniper";
    bool t6 = false, model = true;
    bool omitUnresolved = true;
    bool discoverParts = false, allWeaponClips = false;
    bool includeWorldmodel = false;
    J viewhands = nullptr;
    std::string viewhandsStem;
};
// Resolve set-level fallbacks and filename collisions before writing any clips.
J plan_animation_exports(const J &clips);
J corresponding_worldmodel(const J &entry, const std::vector<J> &rows);
J export_assets(const ExportRequest &request, JobContext *job = nullptr);
} // namespace codm
