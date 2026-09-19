#include "library.hpp"
#include <regex>
namespace codm {
static bool receiver_group(const J &row) {
    auto name=row.value("name",std::string());
    auto rootName=lower(name);
    // Bare attachment renderers can have a matching mesh name too. Only promote
    // legacy entries with an authored weapon-prefab root, never a loose part.
    if(!rootName.starts_with("mainweapon_") && !rootName.starts_with("assiweapon_") &&
       !rootName.starts_with("secondaryweapon_") && !rootName.starts_with("pov_") && !rootName.starts_with("wea")) return false;
    auto identity=weapon_identity(name);
    if(!identity || lower(name).find("_m_")!=std::string::npos || row.contains("error")) return false;
    for(const auto &renderer:row.value("renderers",J::array())) {
        auto meshName=lower(renderer.value("name",std::string()));
        if(part_identity(meshName)) continue;
        auto meshIdentity=weapon_identity(meshName);
        if(meshName.starts_with("wea") || (meshIdentity && meshIdentity->first==identity->first)) return true;
    }
    return false;
}
Database::Database(const fs::path &p) {
    fs::create_directories(p.parent_path());
    int rc =
        sqlite3_open_v2(pathstr(p).c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "SQLite open failure";
        if (db)
            sqlite3_close(db);
        db = nullptr;
        throw std::runtime_error(err);
    }
    sqlite3_busy_timeout(db, 30000);
    exec(
        "PRAGMA journal_mode=WAL; PRAGMA temp_store=MEMORY; PRAGMA synchronous=NORMAL; CREATE TABLE IF "
        "NOT "
        "EXISTS bundles(path TEXT PRIMARY KEY,fingerprint TEXT,error TEXT); CREATE TABLE IF NOT EXISTS "
        "assets(id TEXT,bundle TEXT,type TEXT,category TEXT,name TEXT,data TEXT,PRIMARY "
        "KEY(id,bundle)); "
        "CREATE INDEX IF NOT EXISTS browse_idx ON assets(type,category,name); CREATE TABLE IF NOT "
        "EXISTS "
        "donors(mesh TEXT,renderer TEXT,bundle TEXT,counts TEXT,materials TEXT,PRIMARY "
        "KEY(mesh,renderer)); "
        "CREATE INDEX IF NOT EXISTS donor_counts ON donors(counts); "
        "CREATE TABLE IF NOT EXISTS animation_bindings(id TEXT,bundle TEXT,version INTEGER,"
        "fingerprint TEXT,data TEXT,PRIMARY KEY(id,bundle));");
}
Database::~Database() {
    if (db)
        sqlite3_close(db);
}
void Database::exec(const std::string &sql) {
    char *err = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string s = err ? err : "SQLite execution failed";
        sqlite3_free(err);
        throw std::runtime_error(s);
    }
}
std::vector<std::vector<std::string>> Database::query(const std::string &sql,
                                                      const std::vector<std::string> &args) {
    sqlite3_stmt *raw = nullptr;
    require(sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) == SQLITE_OK, sqlite3_errmsg(db));
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
    for (size_t i = 0; i < args.size(); i++)
        require(sqlite3_bind_text(raw, int(i) + 1, args[i].c_str(), int(args[i].size()),
                                  SQLITE_TRANSIENT) == SQLITE_OK,
                "SQLite binding failed");
    std::vector<std::vector<std::string>> out;
    int rc;
    while ((rc = sqlite3_step(raw)) == SQLITE_ROW) {
        std::vector<std::string> row;
        for (int i = 0; i < sqlite3_column_count(raw); i++) {
            auto s = sqlite3_column_text(raw, i);
            row.emplace_back(s ? reinterpret_cast<const char *>(s) : "");
        }
        out.push_back(std::move(row));
    }
    require(rc == SQLITE_DONE, sqlite3_errmsg(db));
    return out;
}
void Database::run(const std::string &s, const std::vector<std::string> &a) { query(s, a); }
std::string asset_category(const std::string &name, const std::string &bundle) {
    auto n = lower(name), b = lower(bundle);
    if (n.find("viewhand") != n.npos || n.find("view_hand") != n.npos || n.find("sleeve") != n.npos ||
        n.find("1p_hand") != n.npos ||
        (b.find("/avatar/") != b.npos && (n.find("_1p") != n.npos || n.find("_hand") != n.npos)))
        return "Viewhands";
    if (b.find("/avatar/") != b.npos ||
        std::regex_search(n, std::regex("(^|_)(player|character|merc|pb)(_|[0-9])")))
        return "Player";
    if (b.find("/weapons/") != b.npos || n.find("viewmodel") != n.npos)
        return "Weapon";
    if (std::regex_search(n, std::regex("(^|_)\\w*1p[_\\W]")))
        return "Viewhands";
    return "Etc";
}
std::string perspective(const std::string &name) {
    auto n = lower(name);
    if (std::regex_search(n, std::regex("(^|_)(3p|world)(_|\\b)")))
        return "world";
    if (std::regex_search(n, std::regex("(^|_)(1p|pov|viewmodel)(_|\\b)")))
        return "view";
    return "other";
}
std::string animation_category(const std::string &name, const std::string &bundle) {
    auto n = lower(name), b = lower(bundle);
    if (n.find("camera") != n.npos || n.find("camra") != n.npos)
        return "Etc";
    if (std::regex_search(n, std::regex("^(mainweapon|secondaryweapon|assiweapon)_[0-9]+_")))
        return "Weapon";
    if (b.find("/weapons/animations/") != b.npos || n.starts_with("adv") || n.starts_with("advance")) {
        if (std::regex_search(n, std::regex("(^|_)(adv1p|advance1p|1p)(_|$)")))
            return "Viewhands";
        if (std::regex_search(n, std::regex("(^|_)(adv3p|advance3p|2p|3p)(_|$)")))
            return "Player";
    }
    return asset_category(name, bundle);
}
std::vector<Object *> renderer_materials(Source &s, Object &o) {
    Reader r(o.raw(), o.file->big);
    r.seek(40);
    int count = r.count(12, 128);
    std::vector<Object *> mats;
    for (int i = 0; i < count; i++) {
        J p = {{"m_FileID", r.get<int32_t>()}, {"m_PathID", r.get<int64_t>()}};
        auto *m = s.ref(o, p);
        require(!m || m->cid == 21, "Renderer material reference has wrong class");
        mats.push_back(m);
    }
    return mats;
}
bool placeholder(Source &s, Object *o) {
    if (!o)
        return true;
    auto name = lower(s.tree(*o).value("m_Name", std::string()));
    return name.starts_with("empty") && name.find("_donotmodify") != name.npos;
}
J index_bundle(Source &source, const J &bundle) {
    auto path = bundle.at("path").get<std::string>();
    source.load(path);
    std::vector<Object *> objects;
    for (auto &n : bundle.at("nodes")) {
        auto key = lower(basename(n.at("name")));
        if (source.files.contains(key) && source.nodeBundles.at(key) == path)
            for (auto &[pid, o] : source.files.at(key)->objects)
                objects.push_back(&o);
    }
    J result = J::array();
    std::map<std::string, J> groups;
    Hierarchy h(source);
    auto label = hint(path);
    for (auto *obj : objects) {
        auto &o = *obj;
        if (o.cid == 74) {
            std::string name;
            try {
                Reader r(o.raw(), o.file->big);
                name = r.str();
            } catch (...) {
                name = o.id();
            }
            auto category = animation_category(name, label);
            result.push_back({{"id", o.id()},
                              {"bundle", path},
                              {"type", "animation"},
                              {"category", category},
                              {"name", name},
                              {"status", "Decode and target binding checked on preview/export"}});
        } else if (o.cid == 137 || o.cid == 23) {
            try {
                auto r = renderer_info(source, o);
                auto meshName = source.name(*r.mesh);
                auto &root = h.root(*r.transform);
                auto name = h.name(root);
                auto cat = asset_category(meshName + "_" + name, label);
                std::smatch lod;
                auto lowerName = lower(meshName);
                bool hasLod = std::regex_search(lowerName, lod, std::regex("(?:lod|_l)([0-9]+)"));
                std::string level = hasLod ? lod[1].str() : "0";
                auto key = root.id() + ":" + cat + ":" + level;
                {
                    auto &a = groups[key];
                    if (a.is_null())
                        a = {{"id", o.id()},
                             {"bundle", path},
                             {"type", "model"},
                             {"category", cat},
                             {"name", name + (hasLod && level != "0" ? " · LOD " + level : "")},
                             {"renderers", J::array()},
                             {"lod", level},
                             {"perspective", perspective(name + "_" + meshName)},
                             {"complete", false},
                             {"paths", J::array()},
                             {"status",
                              "Source geometry; assembly and materials checked on preparation"}};
                    a["renderers"].push_back(
                        {{"id", o.id()}, {"name", meshName}, {"mesh", r.mesh->id()}});
                    a["static"] = a.value("static", true) && r.bones.empty();
                    std::set<uint32_t> paths = a.at("paths").get<std::set<uint32_t>>();
                    for (auto *b : r.bones) {
                        auto id = b->id();
                        h.world(b);
                        std::vector<std::string> names;
                        while (!id.empty()) {
                            names.push_back(h.name(*h.objects.at(id)));
                            id = h.parents.at(id);
                        }
                        std::reverse(names.begin(), names.end());
                        for (size_t i = 0; i < names.size(); i++) {
                            std::string p;
                            for (size_t k = i; k < names.size(); k++) {
                                if (k != i)
                                    p += '/';
                                p += names[k];
                            }
                            paths.insert(crc32(p));
                        }
                    }
                    a["paths"] = paths;
                    if (cat == "Weapon" && receiver_group(a)) {
                        a["complete"] = true;
                        a["completeEvidence"] =
                            "Recognized weapon receiver grouped by authored root and "
                            "LOD; available parts must be assembled.";
                    }
                    if (cat == "Viewhands" && !r.bones.empty()) {
                        a["complete"] = true;
                        a["completeEvidence"] = "Authored skinned viewhands renderer group.";
                    }
                }
            } catch (const std::exception &e) {
                result.push_back({{"id", o.id()},
                                  {"bundle", path},
                                  {"type", "model"},
                                  {"category", asset_category("", label)},
                                  {"name", o.id()},
                                  {"error", e.what()},
                                  {"complete", false},
                                  {"status", e.what()}});
            }
        }
    }
    for (auto &[key, a] : groups) {
        if (a.at("category") == "Player") {
            bool body = false, head = false;
            for (auto &r : a.at("renderers")) {
                auto n = lower(r.at("name"));
                body |= n.find("body") != n.npos;
                head |= n.find("head") != n.npos;
            }
            a["complete"] = body && head;
            if (body && head)
                a["completeEvidence"] = "Body and head grouped under the same authored root and LOD.";
        }
        result.push_back(a);
    }
    return result;
}
J index_library(Source &source, const fs::path &dbPath, const std::string &cat, JobContext *job,
                const std::function<void()> &committed) {
    Database db(dbPath);
    db.exec("CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT)");
    if (db.query("SELECT value FROM metadata WHERE key='animation-categories' AND value='2'").empty()) {
        auto rows = db.query("SELECT id,bundle,data FROM assets WHERE type='animation'");
        db.exec("BEGIN IMMEDIATE");
        try {
            for (auto &row : rows) {
                auto asset = J::parse(row[2]);
                auto category = animation_category(asset.at("name"), hint(row[1]));
                asset["category"] = category;
                db.run("UPDATE assets SET category=?,data=? WHERE id=? AND bundle=?",
                       {category, asset.dump(), row[0], row[1]});
            }
            db.run("INSERT OR REPLACE INTO metadata VALUES('animation-categories','2')");
            db.exec("COMMIT");
        } catch (...) {
            db.exec("ROLLBACK");
            throw;
        }
    }
    J errors = J::array();
    std::vector<J> bundles;
    for (auto &b : source.catalog.at("bundles")) {
        auto h = lower(hint(b.at("path")));
        bool relevant =
            (cat == "Etc" && (h.find("cod_models/") != h.npos || h.find("cod_prefabs/") != h.npos)) ||
            (cat == "Player" ? (h.find("/avatar/") != h.npos || h.find("/animations/") != h.npos)
             : cat == "Viewhands"
                 ? (h.find("/avatar/") != h.npos || h.find("/weapons/") != h.npos ||
                    h.find("/animations/") != h.npos)
                 : (h.find("/weapons/") != h.npos || h.find("/animations/") != h.npos));
        if (relevant)
            bundles.push_back(b);
    }
    // Both selected CABs and loose player dependencies affect renderer discovery.
    // The adapter revision also invalidates failures recorded by older readers.
    auto revision =
        hex64(hash64(J::array({source.catalog.at("bundles"), source.installed_data_state()}).dump()));
    size_t rebuilt = 0, cached = 0, i = 0;
    auto start = Clock::now();
    for (auto &b : bundles) {
        if (job)
            job->update(float(i++) / std::max(size_t(1), bundles.size()),
                        "Indexing " + cat + ": " + std::to_string(i) + " / " +
                            std::to_string(bundles.size()));
        auto path = b.at("path").get<std::string>();
        auto fingerprint =
            "native4:" + revision + ":" + b.at("size").dump() + ":" + b.at("mtime").dump();
        auto old = db.query("SELECT fingerprint,error FROM bundles WHERE path=?", {path});
        // Retry failures from the old eager type-string reader, without
        // forcing every successfully indexed bundle through another rebuild.
        bool oldTypeFailure = !old.empty() && old[0][1].find("Unknown common type string") != std::string::npos;
        if (!old.empty() && old[0][0] == fingerprint && !oldTypeFailure) {
            cached++;
            continue;
        }
        source.clear();
        J assets = J::array();
        std::string error;
        try {
            assets = index_bundle(source, b);
        } catch (const std::exception &e) {
            error = e.what();
            errors.push_back({{"bundle", path}, {"error", error}});
        }
        if (job)
            job->check();
        db.exec("BEGIN IMMEDIATE");
        try {
            db.run("DELETE FROM assets WHERE bundle=?", {path});
            db.run("DELETE FROM donors WHERE bundle=?", {path});
            for (auto &a : assets) {
                db.run("INSERT OR REPLACE INTO assets VALUES(?,?,?,?,?,?)",
                       {a.at("id"), path, a.at("type"), a.at("category"), a.at("name"), a.dump()});
                if (a.at("type") == "model" && !a.contains("error")) {
                    for (auto &ref : a.value("renderers", J::array({J{{"id", a.at("id")}}}))) {
                        try {
                            auto r = renderer_info(source, source.object(ref.at("id")));
                            auto mats = renderer_materials(source, *r.object);
                            J ids = J::array();
                            bool any = false;
                            for (auto *m : mats) {
                                auto valid = !placeholder(source, m);
                                ids.push_back(valid ? J(m->id()) : J());
                                any |= valid;
                            }
                            if (!any)
                                continue;
                            std::vector<int> counts;
                            for (auto &sub : source.tree(*r.mesh).at("m_SubMeshes"))
                                counts.push_back(sub.at("indexCount").get<int>() / 3);
                            std::sort(counts.begin(), counts.end());
                            db.run("INSERT OR REPLACE INTO donors VALUES(?,?,?,?,?)",
                                   {r.mesh->id(), r.object->id(), path, J(counts).dump(), ids.dump()});
                        } catch (const std::exception &) {
                        }
                    }
                }
            }
            db.run("INSERT OR REPLACE INTO bundles VALUES(?,?,?)", {path, fingerprint, error});
            db.exec("COMMIT");
        } catch (...) {
            db.exec("ROLLBACK");
            throw;
        }
        rebuilt++;
        if (committed && (rebuilt % 10 == 0))
            committed();
    }
    source.clear();
    auto animationBindings = index_animation_bindings(source, dbPath, cat, job, committed);
    if (committed)
        committed();
    return {{"category", cat},
            {"bundles", bundles.size()},
            {"rebuilt", rebuilt},
            {"cached", cached},
            {"errors", errors},
            {"seconds", seconds(start)},
            {"animationBindings", animationBindings}};
}
std::vector<J> unique_model_rows(std::vector<J> rows) {
    std::vector<J> out;
    std::map<std::string, size_t> seen;
    out.reserve(rows.size());
    for (auto &row : rows) {
        if(!row.value("complete",false) && row.value("category",std::string())=="Weapon" && receiver_group(row)) {
            row["complete"]=true;
            row["completeEvidence"]="Authored weapon root with matching receiver mesh; attachments resolved on preparation";
        }
        std::set<std::string> meshes;
        for (const auto &renderer : row.value("renderers", J::array()))
            if (renderer.contains("mesh")) meshes.insert(renderer.at("mesh"));
        if (meshes.empty() || row.contains("error")) { out.push_back(std::move(row)); continue; }
        auto key = J::array({row.value("name", std::string()), row.value("category", std::string()),
            row.value("perspective", std::string()), row.value("lod", std::string("0")),
            row.value("static", false), meshes}).dump();
        if (seen.contains(key)) {
            auto &representative = out[seen.at(key)];
            if (!representative.contains("duplicateInstances"))
                representative["duplicateInstances"] = J::array();
            representative["duplicateInstances"].push_back(row.at("id"));
        } else { seen[key] = out.size(); out.push_back(std::move(row)); }
    }
    return out;
}
std::vector<J> library_rows(const fs::path &path, const std::string &type, const std::string &cat) {
    Database db(path);
    std::vector<J> out;
    if (type == "animation") {
        for (auto &row :
             db.query("SELECT a.data,c.data FROM assets a LEFT JOIN bundles b ON b.path=a.bundle "
                      "LEFT JOIN animation_bindings c ON c.id=a.id AND c.bundle=a.bundle "
                      "AND c.version=2 AND c.fingerprint=b.fingerprint "
                      "WHERE a.type='animation' AND a.category=? ORDER BY a.name,a.id",
                      {cat})) {
            auto entry = J::parse(row[0]);
            if (!row[1].empty()) {
                auto metadata = J::parse(row[1]);
                entry["bindingMetadata"] = metadata;
                if (metadata.contains("paths"))
                    entry["paths"] = metadata.at("paths");
            }
            out.push_back(std::move(entry));
        }
        return out;
    }
    for (auto &row :
         db.query("SELECT data FROM assets WHERE type=? AND category=? ORDER BY name,id", {type, cat}))
        out.push_back(J::parse(row[0]));
    return type == "model" ? unique_model_rows(std::move(out)) : std::move(out);
}
} // namespace codm
