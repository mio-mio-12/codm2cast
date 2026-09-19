#include "animation.hpp"
#include "library.hpp"

namespace codm {
bool is_weapon_entry(const J &entry) {
    return entry.is_object() && entry.value("category", std::string()) == "Weapon" &&
           weapon_identity(entry.value("name", std::string())).has_value();
}
J clip_binding_metadata(Source &source, Object &object, JobContext *job) {
    require(object.cid == 74, "Expected animation clip");
    Reader reader(object.raw(), object.file->big);
    J header = J::object();
    for (auto &field : source.types.at(74).children) {
        if (field.name == "m_MuscleClip")
            break;
        header[field.name] = parse_tree(reader, field);
    }
    std::set<uint32_t> paths;
    size_t transformBindings = 0, otherBindings = 0;
    if (header.at("m_Legacy").get<bool>() && header.at("m_MuscleClipSize") == 0) {
        for (auto field : {"m_PositionCurves", "m_RotationCurves", "m_EulerCurves", "m_ScaleCurves"})
            for (auto &row : header.at(field)) {
                paths.insert(crc32(row.at("path")));
                transformBindings++;
            }
        for (auto &row : header.at("m_CompressedRotationCurves")) {
            paths.insert(crc32(row.at("m_Path")));
            transformBindings++;
        }
        otherBindings = header.at("m_FloatCurves").size() + header.at("m_PPtrCurves").size();
    } else {
        auto clip = decode_clip(source, object, job, true);
        for (auto &binding : clip.bindings) {
            if (binding.type == 4) {
                paths.insert(binding.path);
                transformBindings++;
            } else
                otherBindings++;
        }
    }
    return {
        {"status", "declared"},
        {"paths", paths},
        {"transformBindings", transformBindings},
        {"otherBindings", otherBindings},
        {"scope",
         "Declared paths only; motion decoding and unique target binding checked on preview/export"}};
}
J index_animation_bindings(Source &source, const fs::path &dbPath, const std::string &category,
                           JobContext *job, const std::function<void()> &committed) {
    Database db(dbPath);
    auto rows =
        db.query("SELECT a.id,a.bundle,b.fingerprint,c.version,c.fingerprint,c.data FROM assets a "
                 "JOIN bundles b ON b.path=a.bundle LEFT JOIN animation_bindings c ON "
                 "c.id=a.id AND c.bundle=a.bundle "
                 "WHERE a.type='animation' AND a.category=? ORDER BY a.bundle,a.id",
                 {category});
    size_t cached = 0, indexed = 0, failed = 0;
    auto start = Clock::now();
    for (size_t first = 0; first < rows.size();) {
        size_t last = first + 1;
        while (last < rows.size() && rows[last][1] == rows[first][1])
            last++;
        if (job)
            job->update(float(first) / std::max(size_t(1), rows.size()),
                        "Indexing animation targets: " + std::to_string(first) + " / " +
                            std::to_string(rows.size()));
        source.clear();
        std::vector<std::pair<size_t, J>> updates;
        for (size_t i = first; i < last; i++) {
            auto &row = rows[i];
            if (row[3] == "2" && row[4] == row[2]) {
                cached++;
                failed += J::parse(row[5]).value("status", std::string()) == "unchecked";
                continue;
            }
            if (job)
                job->check();
            J metadata;
            try {
                metadata = clip_binding_metadata(source, source.object(row[0]), job);
            } catch (const std::exception &e) {
                if (job)
                    job->check();
                metadata = {{"status", "unchecked"}, {"error", e.what()}};
                failed++;
            }
            updates.emplace_back(i, std::move(metadata));
        }
        if (!updates.empty()) {
            if (job)
                job->check();
            db.exec("BEGIN IMMEDIATE");
            try {
                for (auto &[i, metadata] : updates) {
                    auto &row = rows[i];
                    db.run("INSERT OR REPLACE INTO animation_bindings VALUES(?,?,2,?,?)",
                           {row[0], row[1], row[2], metadata.dump()});
                }
                if (job)
                    job->check();
                db.exec("COMMIT");
            } catch (...) {
                db.exec("ROLLBACK");
                throw;
            }
            indexed += updates.size();
            if (committed)
                committed();
        }
        first = last;
    }
    source.clear();
    return {{"clips", rows.size()},
            {"indexed", indexed},
            {"cached", cached},
            {"unchecked", failed},
            {"seconds", seconds(start)}};
}
AnimationFilter::AnimationFilter(const J &model) {
    if (model.is_null())
        return;
    selected = true;
    declaredClips = model.value("declaredAnimationIDs", J::array()).get<std::set<std::string>>();
    category = model.value("category", std::string());
    if (model.value("category", std::string()) == "Weapon")
        identity = weapon_identity(model.value("name", std::string()));
    paths = model.value("paths", J::array()).get<std::set<uint32_t>>();
    bundle = model.value("bundle", std::string());
}
AnimationMatch AnimationFilter::match(const J &clip) const {
    if (!selected)
        return AnimationMatch::Unchecked;
    if (declaredClips.contains(clip.value("id", std::string())))
        return AnimationMatch::Applicable;
    // Generic numbered prop bones are not enough to establish ownership across
    // unrelated bundles (for example a chest and a helicopter both use Bone001).
    if (category == "Etc" && (bundle.empty() || bundle != clip.value("bundle", std::string())))
        return AnimationMatch::Unmatched;
    auto motion = identity ? weapon_identity(clip.value("name", std::string())) : std::nullopt;
    if (identity && motion) {
        if (!motion->second.empty())
            return *identity == *motion ? AnimationMatch::Applicable : AnimationMatch::Unmatched;
        if (identity->first != motion->first)
            return AnimationMatch::Unmatched;
        // Some authored motion names omit perspective. Exact variant ownership
        // plus indexed target paths is required; never infer 1P from the name.
        if (!clip.contains("paths") || paths.empty())
            return AnimationMatch::Unchecked;
        for (const auto &path : clip.at("paths"))
            if (paths.contains(path.get<uint32_t>()))
                return AnimationMatch::Applicable;
        return AnimationMatch::Unmatched;
    }
    // Unnamed mechanism tracks are only offered to weapons from their authored
    // bundle. A common numbered bone alone does not establish weapon ownership.
    if (identity && (bundle.empty() || bundle != clip.value("bundle", std::string())))
        return AnimationMatch::Unmatched;
    if (!clip.contains("paths") || paths.empty())
        return AnimationMatch::Unchecked;
    for (auto &p : clip.at("paths"))
        if (paths.contains(p.get<uint32_t>()))
            return AnimationMatch::Applicable;
    return AnimationMatch::Unmatched;
}
AnimationMatch animation_match(const J &model, const J &clip) {
    return AnimationFilter(model).match(clip);
}
} // namespace codm
