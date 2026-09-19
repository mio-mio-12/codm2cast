#include "library.hpp"
#include "model.hpp"
#include <regex>
namespace codm {
std::optional<std::pair<std::string, std::string>> weapon_identity(const std::string &n) {
    auto name = lower(n);
    if (name.starts_with("pov_")) {
        name = name.substr(4);
        name = std::regex_replace(name, std::regex("_(1p|pov)(_lod0)?$"), "");
        return std::pair{name, std::string("1p")};
    }
    std::smatch m;
    static const std::regex re("(?:mainweapon_[0-9]+_|secondaryweapon_[0-9]+_|assiweapon_[0-9]+_|"
                               "wea[0-9]+_)?(.+?)_(1p|3p|pov|ui)(?:_|$)");
    if (std::regex_search(name, m, re))
        return std::pair{m[1].str(), m[2] == "pov" ? std::string("1p") : m[2].str()};
    static const std::regex motion("^(?:mainweapon|secondaryweapon|assiweapon)_[0-9]+_(.+?)_m_");
    if (std::regex_search(name, m, motion))
        return std::pair{m[1].str(), std::string()};
    return {};
}
std::optional<std::array<std::string, 3>> part_identity(const std::string &n) {
    auto name = lower(n);
    std::smatch m;
    static const std::regex pattern("(mag|sto|rai|bar|sig|gri|grip|iro|muz|sco|guide|tri|sra)[0-9]+"
                                    "(?:up)?_(front_)?(.+?)_(1p|3p|pov|ui)(?:_lod0)?");
    if (!std::regex_match(name, m, pattern))
        return {};
    std::string slot = m[1];
    if (slot == "grip")
        slot = "gri";
    if (m[2].matched)
        slot += "_front";
    return std::array<std::string, 3>{slot, m[3], m[4] == "pov" ? std::string("1p") : m[4].str()};
}
J base_weapon(const fs::path &path, const std::pair<std::string, std::string> &identity) {
    if (!fs::exists(path) || identity.first.size() < 3)
        return nullptr;
    Database database(path);
    J best = nullptr;
    size_t length = identity.first.size();
    auto rows = database.query("SELECT data FROM assets WHERE type='model' AND category='Weapon' "
                               "AND lower(name) LIKE ? ORDER BY name,id",
                               {"%" + identity.first.substr(0, 3) + "%"});
    for (auto &row : rows) {
        auto entry = J::parse(row.at(0));
        if (!entry.value("complete", false))
            continue;
        auto candidate = weapon_identity(entry.at("name"));
        if (candidate && candidate->second == identity.second && candidate->first.size() < length &&
            identity.first.starts_with(candidate->first)) {
            best = entry;
            length = candidate->first.size();
        }
    }
    return best;
}
J discover_parts(Source &source, const J &entry, const fs::path &cache, JobContext *job,
                 const J &donor) {
    struct Def {
        std::string id, label, socket;
    };
    std::vector<Def> pending = {
        {"mag", "Magazine", "Mag_point"},        {"sto", "Stock", "Sto_point"},
        {"rai", "Handguard", "Rai_point"},       {"bar", "Barrel", "Bar_point"},
        {"sig", "Rear sight", "Sight_point"},    {"sig_front", "Front sight", "Sight_point_F"},
        {"iro", "Iron sight", "Sight_point"},    {"sco", "Scope", "Sight_point"},
        {"gri", "Grip", "Grip_point"},           {"muz", "Muzzle", "Muzzle_point"},
        {"guide", "Optic mount", "Sight_point"}, {"tri", "Trigger", "Tri_point"},
        {"sra", "Rail accessory", "Sra_point"}};
    std::set<std::string> requestedSockets;
    for (const auto &slot : pending)
        requestedSockets.insert(slot.socket);
    Hierarchy h(source);
    J base = J::object();
    std::set<std::string> meshes, roots;
    auto identity = weapon_identity(entry.at("name"));
    std::string context;
    for (auto &ref : entry.value("renderers", J::array({J{{"id", entry.at("id")}}}))) {
        auto r = renderer_info(source, source.object(ref.at("id")));
        meshes.insert(r.mesh->id());
        context = r.mesh->id();
        if (!identity)
            identity = weapon_identity(source.name(*r.mesh));
        auto &root = h.root(*r.transform);
        if (roots.insert(root.id()).second) {
            auto sockets = h.sockets(root, requestedSockets);
            for (auto it = sockets.begin(); it != sockets.end(); it++) {
                require(!base.contains(it.key()) || base[it.key()] == it.value(),
                        "Selected renderers have ambiguous sockets");
                base[it.key()] = it.value();
            }
        }
    }
    bool manual = !donor.is_null();
    if (manual) {
        identity = weapon_identity(donor.at("name"));
        auto r = renderer_info(
            source,
            source.object(
                donor.value("renderers", J::array({J{{"id", donor.at("id")}}})).at(0).at("id")));
        context = r.mesh->id();
    }
    require(identity.has_value(), "No exact weapon variant and perspective identity");
    J baseEntry = manual ? J() : base_weapon(source.catalogDirectory / "library.sqlite", *identity);
    std::string baseIdentity, baseContext;
    if (!baseEntry.is_null()) {
        baseIdentity = weapon_identity(baseEntry.at("name"))->first;
        auto renderer =
            renderer_info(source, source.object(baseEntry.at("renderers").at(0).at("id")));
        baseContext = renderer.mesh->id();
    }
    J candidates = J::array(), errors = J::array();
    std::vector<J> bundles;
    for (auto &b : source.catalog.at("bundles")) {
        auto name = lower(hint(b.at("path")));
        if (name.find("/weapons/component") != name.npos && name.find("/pendant/") == name.npos)
            bundles.push_back(b);
    }
    fs::create_directories(cache);
    size_t done = 0;
    for (auto &b : bundles) {
        if (job)
            job->update(float(done++) / std::max(size_t(1), bundles.size()),
                        "Discovering parts: " + std::to_string(done) + " / " +
                            std::to_string(bundles.size()));
        auto path = b.at("path").get<std::string>();
        auto cp =
            cache /
            (hex64(hash64(path + b.at("size").dump() + b.at("mtime").dump() + "parts4")) + ".json");
        source.clear();
        try {
            J names = J::object();
            if (fs::exists(cp))
                names = read_json(cp);
            else {
                source.load(path);
                for (auto &node : b.at("nodes")) {
                    auto key = lower(basename(node.at("name")));
                    if (!source.files.contains(key))
                        continue;
                    for (auto &[pid, o] : source.files.at(key)->objects)
                        if (o.cid == 33) {
                            try {
                                auto *mesh = source.ref(o, source.tree(o).at("m_Mesh"));
                                if (!mesh || mesh->cid != 43)
                                    continue;
                                auto *tr = owner_transform(source, o);
                                if (source.ref(*tr, source.tree(*tr).at("m_Father")))
                                    continue;
                                Hierarchy namesHierarchy(source);
                                auto name = namesHierarchy.name(*tr);
                                if (!part_identity(name))
                                    name = source.name(*mesh);
                                if (part_identity(name))
                                    names[o.id()] = {{"name", name}, {"mesh", mesh->id()}};
                            } catch (const std::exception &e) {
                                if (!names.contains("$errors"))
                                    names["$errors"] = J::array();
                                names["$errors"].push_back(
                                    {{"bundle", path}, {"object", o.id()}, {"error", e.what()}});
                            }
                        }
                }
                write_json(cp, names);
            }
            std::map<std::string, std::string> wanted;
            for (auto &error : names.value("$errors", J::array()))
                errors.push_back(error);
            for (auto it = names.begin(); it != names.end(); it++) {
                if (it.key() == "$errors")
                    continue;
                auto p = part_identity(it.value().at("name"));
                if (p && ((*p)[1] == identity->first || (*p)[1] == baseIdentity) &&
                    (*p)[2] == identity->second && !meshes.contains(it.value().at("mesh")))
                    wanted[it.key()] = it.value().at("name");
            }
            if (wanted.empty())
                continue;
            source.load(path);
            std::set<std::string> cabs;
            for (auto &[id, name] : wanted)
                cabs.insert(id.substr(0, id.find_last_of(':')));
            for (auto &cab : cabs) {
                auto &file = source.file(cab);
                Hierarchy ph(source);
                for (auto &[pid, o] : file.objects)
                    if (o.cid == 33) {
                        auto *mesh = source.ref(o, source.tree(o).at("m_Mesh"));
                        if (!mesh || !wanted.contains(o.id()))
                            continue;
                        auto *tr = owner_transform(source, o);
                        if (source.ref(*tr, source.tree(*tr).at("m_Father")) ||
                            !source.tree(*mesh).at("m_BindPose").empty())
                            continue;
                        auto name = wanted.at(o.id());
                        auto p = *part_identity(name);
                        bool fallback = !baseIdentity.empty() && p[1] == baseIdentity;
                        candidates.push_back(
                            {{"name", name},
                             {"slot", p[0]},
                             {"bundle", path},
                             {"mesh", mesh->id()},
                             {"meshName", source.name(*mesh)},
                             {"transform", tr->id()},
                             {"matrix", matrix_json(ph.world(tr))},
                             {"sockets", ph.sockets(*tr, requestedSockets)},
                             {"materialContextMesh", fallback ? baseContext : context},
                             {"sourceWeapon", fallback ? baseEntry.at("name")
                                              : manual ? donor.at("name")
                                                       : entry.at("name")},
                             {"baseFallback", fallback},
                             {"socketPlacement", !manual && !fallback},
                             {"manualPlacement", manual || fallback}});
                    }
            }
        } catch (const std::exception &e) {
            errors.push_back({{"bundle", path}, {"error", e.what()}});
        }
    }
    // Fill missing numbered component choices from the base, without replacing
    // the variant's own version of that component.
    std::set<std::string> variantPieces;
    auto piece = [](const J &c) {
        auto name = lower(c.at("name").get<std::string>());
        auto prefix = name.substr(0, name.find('_'));
        return c.at("slot").get<std::string>() + ":" +
               prefix.substr(prefix.find_first_of("0123456789"));
    };
    for (auto &candidate : candidates)
        if (!candidate.value("baseFallback", false))
            variantPieces.insert(piece(candidate));
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](const J &c) {
                                        return c.value("baseFallback", false) &&
                                               variantPieces.contains(piece(c));
                                    }),
                     candidates.end());
    J slots = J::array();
    for (int pass = 0; pass < 10 && !pending.empty(); pass++) {
        bool progress = false;
        for (auto it = pending.begin(); it != pending.end();) {
            auto d = *it;
            std::vector<std::pair<std::string, J>> locations;
            if (base.contains(d.socket))
                locations.push_back({"", base[d.socket]});
            for (auto &slot : slots)
                for (auto &option : slot.at("options"))
                    if (option.at("sockets").contains(d.socket))
                        locations.push_back({slot.at("id"), option.at("sockets").at(d.socket)});
            std::set<std::string> parents;
            J valid = J::array();
            for (auto &c : candidates)
                if (c.at("slot") == d.id) {
                    bool accepted = false;
                    for (auto &[parent, s] : locations)
                        if (c.value("manualPlacement", false) || c.value("socketPlacement", false) ||
                            matrix_error(json_matrix(c.at("matrix")),
                                         json_matrix(s.at("matrix"))) <= .0001) {
                            parents.insert(parent);
                            accepted = true;
                        }
                    if (accepted)
                        valid.push_back(c);
                }
            if (parents.contains("") &&
                (manual || std::any_of(valid.begin(), valid.end(),
                                       [](const J &c) { return c.value("baseFallback", false) || c.value("socketPlacement", false); })))
                parents = {""};
            if (parents.size() != 1 || valid.empty()) {
                ++it;
                continue;
            }
            std::sort(valid.begin(), valid.end(), [](const J &a, const J &b) {
                if (a.value("baseFallback", false) != b.value("baseFallback", false))
                    return !a.value("baseFallback", false);
                if (a.at("name") != b.at("name"))
                    return natural_less(a.at("name"), b.at("name"));
                return a.at("transform").get<std::string>() < b.at("transform").get<std::string>();
            });
            J unique = J::array();
            std::set<std::string> seen;
            for (auto &c : valid)
                if (seen.insert(c.at("mesh")).second)
                    unique.push_back(c);
            auto parent = *parents.begin();
            slots.push_back(
                {{"id", d.id},
                 {"label", d.id == "rai" && base.contains("Tri_point") ? "Slide / upper" : d.label},
                 {"parent", parent},
                 {"socketName", d.socket},
                 {"baseSocket", base.value(d.socket, J())},
                 {"options", unique}});
            it = pending.erase(it);
            progress = true;
        }
        if (!progress)
            break;
    }
    source.clear();
    return {
        {"entryId", entry.at("id")},
        {"weapon", entry.at("name")},
        {"exactVariant", identity->first},
        {"perspective", identity->second},
        {"slots", slots},
        {"candidateCount", candidates.size()},
        {"errors", errors},
        {"manualDonor", manual},
        {"baseWeapon", baseEntry},
        {"selectionStatus",
         "First naturally sorted compatible options; not evidence of the game's default loadout."}};
}
J resolve_parts(const J &profile, J &choices, bool fillUnset, J *states, bool strict) {
    J selected = J::object(), parts = J::array();
    if (states)
        *states = J::object();
    std::set<std::string> occupied;
    for (auto &slot : profile.at("slots")) {
        auto id = slot.at("id").get<std::string>(), parent = slot.at("parent").get<std::string>(),
             socketName = slot.at("socketName").get<std::string>();
        J socket = slot.at("baseSocket");
        if (!parent.empty()) {
            socket = nullptr;
            if (selected.contains(parent) && selected[parent].at("sockets").contains(socketName))
                socket = selected[parent].at("sockets").at(socketName);
        }
        J options = J::array();
        if (!socket.is_null())
            for (auto &o : slot.at("options"))
                if (o.value("manualPlacement", false) || o.value("socketPlacement", false) ||
                    matrix_error(json_matrix(o.at("matrix")), json_matrix(socket.at("matrix"))) <=
                        .0001)
                    options.push_back(o);
        if (fillUnset && !choices.contains(id) && !options.empty() &&
            !occupied.contains(socket.at("id")))
            choices[id] = options.at(0).at("mesh");
        bool available = !socket.is_null() && !occupied.contains(socket.at("id"));
        if (states)
            (*states)[id] = {{"available", available},
                             {"options", available ? options : J::array()},
                             {"reason", socket.is_null() ? "Requires its parent attachment"
                                        : !available     ? "Socket used by another attachment"
                                                         : ""}};
        if (!choices.contains(id) || choices[id].is_null())
            continue;
        // Preserve dependent choices while their parent is absent. Never silently
        // omit an explicit choice when its active socket rejects it.
        if (socket.is_null())
            continue;
        bool matched = false;
        for (auto &o : options)
            if (o.at("mesh") == choices[id]) {
                if (!available)
                    break;
                occupied.insert(socket.at("id"));
                matched = true;
                selected[id] = o;
                J p = o;
                p.erase("matrix");
                p.erase("sockets");
                p["socket"] = socket.at("id");
                parts.push_back(p);
                break;
            }
        if (!matched) {
            auto reason = available ? "Selected part does not fit the current parent"
                                    : "Socket used by another attachment";
            if (states)
                (*states)[id]["reason"] = reason;
            if (strict)
                throw std::runtime_error(slot.at("label").get<std::string>() + ": " + reason +
                                         ". Choose a compatible option or None.");
        }
    }
    return parts;
}
} // namespace codm
