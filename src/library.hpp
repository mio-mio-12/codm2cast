#pragma once
#include "model.hpp"
#include <sqlite/sqlite3.h>
namespace codm {
class Database {
    sqlite3 *db = nullptr;

  public:
    explicit Database(const fs::path &p);
    ~Database();
    Database(const Database &) = delete;
    void exec(const std::string &sql);
    std::vector<std::vector<std::string>> query(const std::string &sql,
                                                const std::vector<std::string> &args = {});
    void run(const std::string &sql, const std::vector<std::string> &args = {});
};
std::string asset_category(const std::string &name, const std::string &bundle);
std::string animation_category(const std::string &name, const std::string &bundle);
std::string perspective(const std::string &name);
J index_bundle(Source &source, const J &bundle);
J index_library(Source &source, const fs::path &dbPath, const std::string &category,
                JobContext *job = nullptr, const std::function<void()> &committed = {});
J index_animation_bindings(Source &source, const fs::path &dbPath, const std::string &category,
                           JobContext *job = nullptr, const std::function<void()> &committed = {});
enum class AnimationMatch { Applicable, Unmatched, Unchecked };
class AnimationFilter {
    bool selected = false;
    std::optional<std::pair<std::string, std::string>> identity;
    std::set<uint32_t> paths;
    std::set<std::string> declaredClips;
    std::string bundle;
    std::string category;

  public:
    explicit AnimationFilter(const J &model);
    AnimationMatch match(const J &clip) const;
};
AnimationMatch animation_match(const J &model, const J &clip);
bool is_weapon_entry(const J &entry);
J base_weapon(const fs::path &database, const std::pair<std::string, std::string> &identity);
J fill_base_animation_slots(const J &selected, const J &baseClips, const J &baseEntry);
J weapon_animation_set(Source &source, const fs::path &database, const J &entry,
                       J *report = nullptr, JobContext *job = nullptr);
J discover_animation_sources(Source &source, const J &entry, JobContext *job = nullptr);
J inherit_animation_sources(const J &entry, const J &selected, const J &declarations,
                            J *report = nullptr);
std::vector<J> library_rows(const fs::path &db, const std::string &type, const std::string &category);
std::vector<J> unique_model_rows(std::vector<J> rows);
std::vector<Object *> renderer_materials(Source &source, Object &renderer);
bool placeholder(Source &source, Object *material);
} // namespace codm
