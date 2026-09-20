#include "export.hpp"
#include "exported_animations.hpp"
#include "batch_export.hpp"
#include "model_pairs.hpp"
#include "viewport_navigation.hpp"
#include "discovery.hpp"
#include "export_naming.hpp"
#include "gallery.hpp"
#include "jobs.hpp"
#include "preview_pose.hpp"
#include <GLFW/glfw3.h>
#include <Windows.h>
#include <deque>
#include <glm/gtc/type_ptr.hpp>
#include <imgui/backends/imgui_impl_glfw.h>
#include <imgui/backends/imgui_impl_opengl2.h>
#include <imgui/imgui.h>
#include <imgui/misc/cpp/imgui_stdlib.h>
#include <regex>
#include <shellapi.h>
#include <shobjidl.h>
#include <thread>
namespace codm {
static const std::array<std::string, 4> categories = {"Player", "Weapon", "Viewhands", "Etc"};
static std::string folder_dialog(HWND owner, const std::string &initial) {
    IFileOpenDialog *dialog = nullptr;
    std::string result;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
        return result;
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (!initial.empty()) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(pathof(initial).c_str(), nullptr,
                                                  IID_PPV_ARGS(&item)))) {
            dialog->SetFolder(item);
            item->Release();
        }
    }
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = pathstr(fs::path(path));
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}
static bool search_match(const std::string &query,const J &row) {return SearchQuery(query).match(row);}

static std::string read_game_directory(const fs::path &ini) {
    wchar_t value[32768]{};
    GetPrivateProfileStringW(L"CODM", L"Directory", L"", value, DWORD(std::size(value)), fs::absolute(ini).c_str());
    return pathstr(fs::path(value));
}
static void save_game_directory(const fs::path &ini, const std::string &directory) {
    if (!fs::exists(ini)) {
        std::ofstream file(ini, std::ios::binary);
        file.write("\xff\xfe", 2); file.close();
        require(bool(file), "Could not create settings.ini.");
    }
    require(WritePrivateProfileStringW(L"CODM", L"Directory", pathof(directory).c_str(), fs::absolute(ini).c_str()) != 0,
            "Could not save settings.ini. Choose a writable application folder.");
}
static std::string directory_key(const std::string &directory) {
    return lower(pathstr(fs::weakly_canonical(pathof(directory))));
}
struct Studio {
    // Optional observation for the hidden input regression. It never activates
    // widgets or alters their behavior; input still travels through ImGuiIO.
    std::function<void(const std::string &)> observeItem;
    void observe(const std::string &label) {
        if (observeItem)
            observeItem(label);
    }
    GLFWwindow *window = nullptr;
    double renderMilliseconds=0;
    PreviewPoseCache poseCache;
    fs::path root, data, catalog, database, settings, sourceCache;
    bool directoryPopup = false, directoryRequired = true;
    std::string directoryDraft, directoryError;
    std::array<std::vector<J>, 4> models, clips;
    std::array<bool, 4> loaded{};
    int modelTab = 1, clipTab = 1;
    std::string modelSearch, clipSearch, donorSearch,
        gameRoot, status = "Choose a model or refresh the library.", details;
    bool textOnly = false, completeOnly = true, applicableOnly = true, playing = false,
         exportPopup = false, donorPopup = false, dirtySettings = false;
    bool showLods = false;
    bool galleryMode = false, galleryReady = false, galleryBusy = false;
    J galleryIcons = J::object();
    std::string galleryError;
    std::set<std::string> galleryMissing;
    struct IconTexture { GLuint id; int lastFrame; };
    std::map<std::string,IconTexture> galleryTextures;
    J galleryTags = J::object();
    J savedExportNames = J::object();
    std::map<std::string,J> weaponConfigurations;
    void remember_configuration() {
        if (!selected.is_null() && is_weapon_entry(selected) && !profile.is_null())
            weaponConfigurations[selected.at("id")] = {{"profile",profile},{"choices",choices},{"overrides",overrides}};
    }
    J configured_batch_plan() {
        remember_configuration();
        auto plan = batchPlan;
        for (auto &entry : plan["entries"]) {
            auto at = weaponConfigurations.find(entry.at("id"));
            if (at != weaponConfigurations.end()) entry["configuration"] = at->second;
        }
        return plan;
    }
    J weaponReference=J::object();
    std::string weaponCategoryFilter,weaponFamilyFilter;
    bool storeOpen=false,batchPopup=false,batchSkipExisting=true,batchSkipUntextured=true;
    int batchTab=0;
    std::array<std::set<std::string>,4> checkedModels;
    std::array<std::string,4> rangeAnchor;
    bool batchSelected=false,batchPair=false,batchPlanning=false;
    bool batchAnimations=false,batchFolderAnimations=false;
    uint64_t batchRevision=0;
    std::vector<J> batchEntries;
    J batchPlan=nullptr,knownIssues=J::object();
    std::string batchError;

    std::map<std::string,std::map<std::string,std::pair<std::string,int>>> familyMenu;
    size_t familyMenuSize=SIZE_MAX;
    int gallerySort = 0;
    uint64_t galleryTagRevision = 0;
    std::string galleryTagFilter, galleryIconSearch, galleryChoiceFilter;
    std::vector<std::string> galleryChoices;
    bool galleryIconsOnly = false;
    bool galleryTagsDirty = false;
    int perspectiveFilter = 0, quality = 1;
    const int dimensions[4] = {256, 512, 1024, 0};
    J selected = nullptr, selectedClip = nullptr, profile = nullptr, choices = J::object(),
      overrides = J::object();
    std::shared_ptr<Model> preview;
    std::shared_ptr<MaterialSet> materials;
    std::shared_ptr<Animation> animation;
    std::optional<std::array<DV3, 2>> animationBounds;
    uint64_t revision = 0;
    double frame = 0;
    float yaw = .65f, pitch = .18f, distance = 1;
    V3 center{0},navigationPivot{0};
    bool autoDepth=true,zoomToMouse=true,emulateMiddle=false,showGrid=true,orthographic=false;
    float orbitSpeed=.008f,zoomSpeed=.15f;
    bool depthSample=false,depthOrbit=false;
    ImVec2 depthCursor{};
    float depthZoom=1;

    ImVec2 previewMin{}, previewSize{};
    std::map<std::string, GLuint> textures;
    bool cameraNeedsFit = true;
    bool uploadPending = false;
    JobQueue jobs;
    std::vector<std::unique_ptr<Task>> &tasks = jobs.tasks;
    std::array<uint64_t, 4> rowRevisions{};
    std::mutex mailboxMutex;
    std::deque<std::function<void()>> mailbox;
    std::string modelsDestination, animationsDestination, exportWeapon,
        exportVariant, exportCategory = "special", exportStem, exportError,
        sniperArchetype = "semi_sniper";
    bool exportModel = true, exportAnimations = true, exportAllClips = true, exportWorld = false;
    bool omitUnresolved = true;
    bool includeViewhands = false;
    J companion = nullptr;
    std::string companionStem, companionSearch;
    std::string modelFilterKey, clipFilterKey, partStateKey;
    J cachedPartStates;
    std::vector<size_t> visibleModels, visibleClips;
    size_t uncheckedClips = 0;
    std::map<std::string, std::string> overrideInputs;
    J inheritedClips = J::array();
    std::string inheritedStatus;
    explicit Studio(fs::path directory)
        : root(std::move(directory)), data(root / "bin/data"), catalog(root / "catalog.json"),
          database(root / "library.sqlite"), settings(root / "studio.json"), sourceCache(root / "cache") {
        if (!fs::exists(data))
            data = root / "data";
        if(fs::exists(data/"weapon-reference.json")) weaponReference=read_json(data/"weapon-reference.json");
        if(fs::exists(root/"gallery-tags.json"))
            try { galleryTags=read_json(root/"gallery-tags.json"); } catch(const std::exception &) { }
        if(fs::exists(root/"export-names.json"))
            try { savedExportNames=read_json(root/"export-names.json"); } catch(const std::exception &) { }
        modelsDestination = pathstr(root / "exports/models");
        animationsDestination = pathstr(root / "exports/animations");
        if(fs::exists(root/"asset-issues.json")) try {
            auto recorded=read_json(root/"asset-issues.json");
            if(fs::exists(catalog) && recorded.value("catalogStamp",std::string())==std::to_string(fs::last_write_time(catalog).time_since_epoch().count())) knownIssues=recorded.value("issues",J::object());
        } catch(const std::exception &) {}
        if (fs::exists(settings))
            try {
                auto j = read_json(settings);
                modelsDestination = j.value("modelsDestination", modelsDestination);
                animationsDestination = j.value("animationsDestination", animationsDestination);
                directoryDraft = j.value("gameRoot", std::string());
                textOnly = j.value("textOnly", false);
                omitUnresolved = j.value("omitUnresolved", true);
                galleryMode = j.value("galleryMode", false);
                dirtySettings = j.contains("t6") || j.contains("t6OptInVersion");
                quality = std::clamp(j.value("quality", 1), 0, 3);
                auto prefs=j.value("navigation",J::object());
                autoDepth=prefs.value("autoDepth",true);zoomToMouse=prefs.value("zoomToMouse",true);
                emulateMiddle=prefs.value("emulateMiddle",false);showGrid=prefs.value("grid",true);
                orbitSpeed=std::clamp(prefs.value("orbitSpeed",.008f),.001f,.03f);
                zoomSpeed=std::clamp(prefs.value("zoomSpeed",.15f),.02f,.5f);
            } catch (const std::exception &e) {
                details = e.what();
            }
        gameRoot = read_game_directory(root / "settings.ini");
        directoryRequired = gameRoot.empty() || !fs::is_directory(pathof(gameRoot));
        if (!gameRoot.empty()) directoryDraft = gameRoot;
        directoryPopup = directoryRequired;
        if (!directoryRequired) configure_source_paths();
    }
    void configure_source_paths() {
        // Keep installation-specific indexes and caches separate. A matching legacy index remains usable.
        auto key = directory_key(gameRoot);
        uint64_t hash = 14695981039346656037ull;
        for (unsigned char c : key) { hash ^= c; hash *= 1099511628211ull; }
        auto base = root / "libraries" / hex64(hash);
        try {
            auto legacy = root / "catalog.json";
            if (fs::exists(legacy) && directory_key(read_json(legacy).at("root").get<std::string>()) == key) base = root;
        } catch (const std::exception &) {}
        fs::create_directories(base);
        catalog = base / "catalog.json"; database = base / "library.sqlite"; sourceCache = base / "cache";
    }
    bool set_game_directory(bool startLoading = true) {
        directoryError.clear();
        try {
            require(tasks.empty(), "Wait for current jobs to finish or cancel them before changing folders.");
            require(!directoryDraft.empty() && fs::is_directory(pathof(directoryDraft)), "Choose an existing CODM installation folder.");
            auto chosen = pathstr(fs::weakly_canonical(pathof(directoryDraft)));
            save_game_directory(root / "settings.ini", chosen);
            gameRoot = chosen; configure_source_paths(); directoryRequired = false;
            ++revision; ++batchRevision;
            for (auto &token : rowRevisions) ++token;
            for (auto &rows : models) rows.clear();
            for (auto &rows : clips) rows.clear();
            for (auto &marked : checkedModels) marked.clear();
            rangeAnchor.fill({}); loaded.fill(false);
            selected = selectedClip = profile = companion = nullptr;
            preview.reset(); materials.reset(); animation.reset(); animationBounds.reset();
            poseCache = PreviewPoseCache{}; playing = false; uploadPending = true;
            choices = overrides = J::object(); inheritedClips = J::array(); weaponConfigurations.clear();
            batchPlan = nullptr; batchEntries.clear(); knownIssues = J::object();
            modelFilterKey.clear(); clipFilterKey.clear(); partStateKey.clear(); familyMenuSize=SIZE_MAX;
            galleryIcons = J::object(); galleryReady = galleryBusy = false;
            galleryError.clear(); galleryMissing.clear();
            for (auto &[key, texture] : galleryTextures) glDeleteTextures(1, &texture.id);
            galleryTextures.clear(); status = "CODM directory saved.";
            if (startLoading) {
                if (fs::exists(database)) load_rows(modelTab); else index(modelTab, true);
            }
            return true;
        } catch (const std::exception &e) { directoryError = e.what(); return false; }
    }
    std::string issue_text(const J &report) const {
        std::string result;
        if(!report.is_object())return result;
        if(report.contains("error") && report["error"].is_string())result+=report["error"].get<std::string>()+"\n";
        for(auto field:{"materialErrors","materialWarnings","assemblyWarnings"})
            for(const auto &problem:report.value(field,J::array())) {
                auto reason=problem.value("error",problem.value("reason",std::string()));
                if(reason.starts_with("Fast preview:"))continue;
                result+=problem.value("surface",std::string())+": "+reason+"\n";
            }
        for(auto field:{"model","details","result"}) if(report.contains(field))result+=issue_text(report[field]);
        return result.substr(0,6000);
    }
    void record_issue(const J &entry,const J &report,bool persist=true) {
        if(entry.is_null() || !entry.contains("id"))return;
        auto key=entry.at("id").get<std::string>();auto message=issue_text(report);
        if(message.empty())knownIssues.erase(key);else knownIssues[key]=message;
        if(persist && fs::exists(catalog))write_json(root/"asset-issues.json",{{"catalogStamp",std::to_string(fs::last_write_time(catalog).time_since_epoch().count())},{"issues",knownIssues}});
    }
    std::string row_issue(const J &row) const {
        auto key=row.at("id").get<std::string>();
        if(knownIssues.contains(key))return knownIssues.at(key);
        if(row.contains("error"))return row.at("error").is_string()?row.at("error").get<std::string>():row.at("error").dump();
        if(!row.value("complete",false))return "Partial model or individual part; a complete set has not been recognized.";
        return {};
    }
    void warning_badge(const J &row) {
        auto problem=row_issue(row);if(problem.empty())return;
        auto pos=ImGui::GetCursorScreenPos();auto *draw=ImGui::GetWindowDrawList();
        draw->AddTriangleFilled(ImVec2(pos.x+7,pos.y),ImVec2(pos.x,pos.y+13),ImVec2(pos.x+14,pos.y+13),IM_COL32(245,180,40,255));
        draw->AddText(ImVec2(pos.x+4,pos.y),IM_COL32(30,25,20,255),"!");ImGui::Dummy(ImVec2(15,ImGui::GetTextLineHeight()));
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",problem.c_str());ImGui::SameLine();
    }
    void choose_row(const J &row,const std::vector<size_t> &visible) {
        auto id=row.at("id").get<std::string>();auto &marked=checkedModels[modelTab];auto &io=ImGui::GetIO();
        if(io.KeyShift && !rangeAnchor[modelTab].empty()) {
            int a=-1,b=-1;
            for(size_t i=0;i<visible.size();++i){auto key=models[modelTab][visible[i]].at("id");if(key==rangeAnchor[modelTab])a=int(i);if(key==id)b=int(i);}
            if(a>=0 && b>=0)for(int i=std::min(a,b);i<=std::max(a,b);++i)marked.insert(models[modelTab][visible[i]].at("id"));
        } else if(io.KeyCtrl) {if(!marked.erase(id))marked.insert(id);rangeAnchor[modelTab]=id;}
        else rangeAnchor[modelTab]=id;
        select(row);
    }
    void resolve_batch() {
        for(auto &task:jobs.tasks)if(task->name=="Resolve export pairs")task->context.cancel=true;
        auto token=++batchRevision;batchError.clear();batchPlan={{"entries",batchEntries},{"pairs",J::array()},{"issues",J::array()}};
        if(!batchPair){batchPlanning=false;return;}
        batchPlanning=true;auto entries=batchEntries;auto tab=batchTab;
        run("Resolve export pairs",[this,token,entries=std::move(entries),tab](JobContext &job){
            try {
                auto category=tab==0?"Viewhands":tab==2?"Player":"Weapon";
                auto rows=library_rows(database,"model",category);job.check();
                if(tab==1)enrich_discovery(rows,weaponReference);
                auto plan=plan_model_pairs(entries,rows,true);job.check();
                post([this,token,plan=std::move(plan)]{if(token==batchRevision){batchPlan=plan;batchPlanning=false;}});
            }catch(const std::exception &e){auto message=std::string(e.what());post([this,token,message]{if(token==batchRevision){batchPlanning=false;batchError=message;}});}
        });
    }
    void open_batch(int tab,bool onlySelected) {
        batchFolderAnimations=false;
        batchTab=tab;batchSelected=onlySelected;batchPair=onlySelected;batchEntries.clear();
        for(const auto &row:models[tab])if(onlySelected?checkedModels[tab].contains(row.at("id")):(showLods || row.value("lod",std::string("0"))=="0"))batchEntries.push_back(row);
        batchPopup=true;resolve_batch();
    }
    void post(std::function<void()> fn) {
        std::lock_guard lock(mailboxMutex);
        mailbox.push_back(std::move(fn));
    }
    ~Studio() { jobs.shutdown(); }
    void run(std::string name, std::function<void(JobContext &)> work) {
        auto token = revision;
        jobs.enqueue(name, [this, token, name, work = std::move(work)](JobContext &context) {
            try {
                context.check();
                work(context);
            } catch (const std::exception &e) {
                auto error = std::string(e.what());
                bool cancelled = context.cancel;
                post([this, token, error, name, cancelled] {
                    if(name == "Index weapon icons" || name == "Load visible weapon icons") {
                        galleryBusy=false;
                        galleryError=error;
                    }
                    if (token != revision &&
                        (name == "Prepare preview" || name == "Preview animation" ||
                         name == "Load donor parts" || name == "Discover shared animations"))
                        return;
                    if(!cancelled && name=="Prepare preview")record_issue(selected,{{"error",error}});
                    status = (cancelled ? "Cancelled: " : "Failed: ") + name;
                    details = error;
                });
            }
        });
    }
    void drain() {
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard lock(mailboxMutex);
            batch.swap(mailbox);
        }
        for (auto &fn : batch)
            fn();
        jobs.poll();
    }
    void save() {
        if (dirtySettings) {
            write_json(settings, {{"modelsDestination", modelsDestination},
                                  {"animationsDestination", animationsDestination},
                                  {"textOnly", textOnly},
                                  {"omitUnresolved", omitUnresolved},
                                  {"galleryMode", galleryMode},
                                  {"quality", quality},
                                  {"navigation",{{"autoDepth",autoDepth},{"zoomToMouse",zoomToMouse},{"emulateMiddle",emulateMiddle},{"grid",showGrid},{"orbitSpeed",orbitSpeed},{"zoomSpeed",zoomSpeed}}}});
            dirtySettings = false;
        }
    }
    void cancel_preview() {
        for (auto &t : tasks)
            if (t->name == "Prepare preview" || t->name == "Preview animation" ||
                t->name == "Discover parts" || t->name == "Load donor parts" ||
                t->name == "Discover shared animations")
                t->context.cancel = true;
    }
    void load_rows(int tab) {
        if (directoryRequired) return;
        for(const auto &task: tasks)
            if(!task->done && task->name == "Read " + categories[tab] + " library") return;
        for (auto &task : tasks)
            if (task->name == "Index library" && !task->done)
                return;
        auto token = ++rowRevisions[tab];
        run("Read " + categories[tab] + " library", [this, tab, token](JobContext &job) {
            job.update(0, "Reading cached library");
            auto m = library_rows(database, "model", categories[tab]);
            enrich_discovery(m,weaponReference);
            job.check();
            post([this, tab, token, m = std::move(m)]() mutable {
                if (rowRevisions[tab] != token)
                    return;
                modelFilterKey.clear();
                models[tab] = std::move(m); familyMenuSize=SIZE_MAX;
            });
            job.update(.6f,"Models ready; reading cached animations");
            auto a=library_rows(database,"animation",categories[tab]);
            enrich_discovery(a,J::object());job.check();
            post([this, tab, token, a=std::move(a)]() mutable {
                if(rowRevisions[tab]!=token) return;
                clipFilterKey.clear();
                clips[tab] = std::move(a);
                augment_inherited(tab);
                loaded[tab] = true;
            });
        });
    }
    void index(int tab, bool scan = false) {
        if (directoryRequired) { directoryPopup = true; return; }
        for (auto &t : tasks)
            if (t->name == "Index library") {
                status = "An index job is already running. Other loaded tabs remain usable.";
                return;
            }
        auto previousModels = models[tab], previousClips = clips[tab];
        auto folder = gameRoot;
        auto token = ++rowRevisions[tab];
        run("Index library",
            [this, tab, token, scan, folder, previousModels, previousClips](JobContext &job) {
                try {
                    if (scan || !fs::exists(catalog))
                        scan_catalog(pathof(folder), catalog, &job);
                    Source source(catalog, data);
                    auto last = Clock::now();
                    auto publish = [&] {
                        auto m = library_rows(database, "model", categories[tab]);
                        auto a = library_rows(database, "animation", categories[tab]);
                        enrich_discovery(m,weaponReference); enrich_discovery(a,J::object());
                        job.check();
                        post([this, tab, token, m = std::move(m), a = std::move(a)]() mutable {
                            if (rowRevisions[tab] != token)
                                return;
                            modelFilterKey.clear();
                            clipFilterKey.clear();
                            models[tab] = std::move(m); familyMenuSize=SIZE_MAX;
                            clips[tab] = std::move(a);
                            augment_inherited(tab);
                            loaded[tab] = true;
                        });
                    };
                    auto report = index_library(source, database, categories[tab], &job, [&] {
                        if (seconds(last) > 1) {
                            publish();
                            last = Clock::now();
                        }
                    });
                    publish();
                    post([this, report] {
                        status = "Library ready.";
                        details = report.dump(2);
                    });
                } catch (...) {
                    if (job.cancel)
                        post([this, tab, token, previousModels, previousClips] {
                            if (rowRevisions[tab] != token)
                                return;
                            ++rowRevisions[tab];
                            modelFilterKey.clear();
                            clipFilterKey.clear();
                            models[tab] = previousModels;
                            clips[tab] = previousClips;
                            status = "Index cancelled. Previous tab restored.";
                        });
                    throw;
                }
            });
        for (auto &task : tasks)
            if (task->name == "Index library")
                task->onCancel = [this, tab, token, previousModels, previousClips] {
                    if (rowRevisions[tab] != token)
                        return;
                    ++rowRevisions[tab];
                    modelFilterKey.clear();
                    clipFilterKey.clear();
                    models[tab] = previousModels;
                    clips[tab] = previousClips;
                    status = "Index cancelled. Previous tab restored.";
                };
    }
    void select(const J &row) {
        remember_configuration();
        cancel_preview();
        revision++;
        depthSample=false;navigationPivot=center;
        selected = row;
        selected.erase("declaredAnimationIDs");
        inheritedClips = J::array();
        inheritedStatus.clear();
        companion = nullptr;
        includeViewhands = false;
        yaw = row.value("category", std::string()) == "Player" ? -.85f : .65f;
        selectedClip = nullptr;
        profile = nullptr; partStateKey.clear();
        choices = J::object();
        overrides = J::object();
        if (auto at = weaponConfigurations.find(row.at("id")); at != weaponConfigurations.end()) {
            profile = at->second.at("profile"); choices = at->second.at("choices"); overrides = at->second.at("overrides");
        }
        overrideInputs.clear();
        preview.reset();
        materials.reset();
        animation.reset();
        animationBounds.reset();
        playing = false;
        uploadPending = true;
        status = row.value("status", std::string());
        details = row.dump(2);
        auto naming=detect_export_name(row, models[1]);
        exportWeapon=naming.weapon; exportVariant=naming.variant; exportCategory=naming.category;
        auto namingKey=weapon_icon_key(row.value("name",std::string()));
        if(savedExportNames.contains(namingKey)) {
            const auto &saved=savedExportNames.at(namingKey);
            exportWeapon=saved.value("weapon",exportWeapon);
            exportVariant=saved.value("variant",exportVariant);
            exportCategory=saved.value("category",exportCategory);
        }
        exportWorld = row.value("perspective", perspective(row.at("name"))) == "world";
        if (!textOnly)
            prepare(profile.is_null());
    }
    void augment_inherited(int tab) {
        std::set<std::string> ids;
        for (const auto &row : clips[tab])
            ids.insert(row.at("id"));
        for (const auto &row : inheritedClips)
            if (row.at("category") == categories[tab] && ids.insert(row.at("id")).second)
                clips[tab].push_back(row);
    }
    void discover_shared_animations() {
        if (!is_weapon_entry(selected))
            return;
        for (auto &task : tasks)
            if (task->name == "Discover shared animations" && !task->done)
                return;
        auto entry = selected;
        auto token = revision;
        entry.erase("declaredAnimationIDs");
        run("Discover shared animations", [this, entry, token](JobContext &job) {
            job.update(0, "Reading assigned animation controllers");
            Source source(catalog, data);
            J report;
            auto combined = weapon_animation_set(source, database, entry, &report, &job);
            J inherited = J::array();
            for (auto &row : combined)
                if (row.contains("inheritance"))
                    inherited.push_back(std::move(row));
            job.check();
            post([this, token, inherited = std::move(inherited),
                  report = std::move(report)]() mutable {
                publish_shared_animations(token, std::move(inherited), std::move(report));
            });
        });
    }
    void publish_shared_animations(uint64_t token, J inherited, J report) {
        if (revision != token || selected.is_null())
            return;
        inheritedClips = std::move(inherited);
        selected["declaredAnimationIDs"] = report.at("added");
        for (int tab = 0; tab < 4; tab++)
            augment_inherited(tab);
        clipFilterKey.clear();
        inheritedStatus = std::to_string(inheritedClips.size()) + " shared actions found";
        if (report["declarations"]["status"] == "partial")
            inheritedStatus += "; some source references are unresolved";
        status = inheritedStatus + ". Details contains source evidence.";
        details = report.dump(2);
    }
    void fit_camera() {
        if (!preview)
            return;
        std::vector<V3> points;
        if (animation && animationBounds) {
            for (int corner = 0; corner < 8; corner++) {
                V3 point;
                for (int k = 0; k < 3; k++)
                    point[k] = float((*animationBounds)[(corner >> k) & 1][k]);
                points.push_back(point);
            }
        } else {
            for (const auto &surface : preview->surfaces)
                points.insert(points.end(), surface.positions.begin(), surface.positions.end());
        }
        V3 lo(1e30f), hi(-1e30f);
        for (const auto &p : points) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        center = (lo + hi) * .5f;
        V3 direction(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw),
                     std::sin(pitch));
        V3 right = glm::normalize(glm::cross(direction, V3(0, 0, 1))),
           up = glm::cross(right, direction);
        float aspect = previewSize.y > 1 ? previewSize.x / previewSize.y : 1.5f,
              tanV = std::tan(glm::radians(19.f)), tanH = tanV * aspect;
        distance = .01f;
        for (const auto &p : points) {
            auto d = p - center;
            distance = std::max(distance, std::max(std::abs(glm::dot(d, right)) / tanH,
                                                   std::abs(glm::dot(d, up)) / tanV) +
                                              glm::dot(d, direction));
        }
        distance *= 1.18f;
        cameraNeedsFit = true;
    }
    void prepare(bool discover = false) {
        if (selected.is_null())
            return;
        remember_configuration();
        cancel_preview();
        uint64_t token = ++revision;
        auto entry = selected;
        auto currentProfile = profile;
        auto currentChoices = choices;
        auto currentOverrides = overrides;
        int dimension = dimensions[quality];
        animation.reset();
        animationBounds.reset();
        playing = false;
        run("Prepare preview", [this, token, entry, currentProfile, currentChoices,
                                currentOverrides, dimension, discover](JobContext &job) mutable {
            auto started = Clock::now();
            Source source(catalog, data);
            job.update(0, "Loading geometry");
            auto initialParts=(!discover && !currentProfile.is_null())
                ? resolve_parts(currentProfile,currentChoices,false,nullptr,false) : J::array();
            auto model = std::make_shared<Model>(prepare_geometry(source, entry, initialParts, &job));
            double firstGeometry = seconds(started);
            post([this, token, model] {
                if (token != revision)
                    return;
                preview = model;
                clipFilterKey.clear();
                materials.reset();
                uploadPending = true;
                fit_camera();
                status = "Geometry ready; preparing parts and textures.";
            });
            if ((discover || currentProfile.is_null()) && is_weapon_entry(entry)) {
                job.update(0, "Discovering compatible variant parts");
                auto previousProfile = currentProfile;
                currentProfile = discover_parts(source, entry, sourceCache / "parts", &job);
                if (!previousProfile.is_null())
                    for (auto &oldSlot : previousProfile.at("slots")) {
                        for (auto &option : oldSlot.at("options"))
                            if (option.value("manualPlacement", false)) {
                                bool slotFound = false;
                                for (auto &slot : currentProfile["slots"])
                                    if (slot.at("id") == oldSlot.at("id")) {
                                        slotFound = true;
                                        bool present = false;
                                        for (auto &existing : slot.at("options"))
                                            present |= existing.at("mesh") == option.at("mesh");
                                        if (!present)
                                            slot["options"].push_back(option);
                                    }
                                if (!slotFound) {
                                    auto slot = oldSlot;
                                    slot["options"] = J::array({option});
                                    currentProfile["slots"].push_back(slot);
                                }
                            }
                    }
                auto p = resolve_parts(currentProfile, currentChoices, true, nullptr, false);
                post([this, token, currentProfile, currentChoices] {
                    if (token != revision)
                        return;
                    profile = currentProfile; partStateKey.clear();
                    choices = currentChoices;
                    remember_configuration();
                });
            }
            J parts = currentProfile.is_null()
                          ? J::array()
                          : resolve_parts(currentProfile, currentChoices, false, nullptr, false);
            if (parts != initialParts)
                model = std::make_shared<Model>(prepare_geometry(source, entry, parts, &job));
            post([this, token, model] {
                if (token != revision)
                    return;
                preview = model;
                clipFilterKey.clear();
                materials.reset();
                uploadPending = true;
                fit_camera();
            });
            // Workers mutate only their own model after posting a geometry snapshot.
            model = std::make_shared<Model>(*model);
            model->report["firstGeometrySeconds"] = firstGeometry;
            model->report["assemblyReadySeconds"] = seconds(started);
            auto mats = std::make_shared<MaterialSet>(prepare_materials(
                source, *model, database, currentOverrides, dimension, &job, true));
            model->report["texturedPreviewSeconds"] = seconds(started);
            job.check();
            post([this, token, model, mats] {
                if (token != revision)
                    return;
                preview = model;
                clipFilterKey.clear();
                materials = mats;
                uploadPending = true;
                status = mats->errors.empty()
                             ? (model->report.contains("materialWarnings")
                                    ? "Textured preview ready with material fallbacks; see Details."
                                    : "Textured preview ready.")
                             : "Geometry ready; some surfaces need material selection.";
                record_issue(selected,model->report);
                details = model->report.dump(2);
            });
        });
    }
    void build_weapon() {
        if (selected.is_null())
            return;
        cancel_preview();
        auto token = ++revision;
        auto entry = selected;
        auto oldChoices = choices;
        auto oldProfile = profile;
        run("Discover parts",
            [this, token, entry, oldChoices, oldProfile](JobContext &job) mutable {
                Source source(catalog, data);
                auto p = discover_parts(source, entry, sourceCache / "parts", &job);
                if (!oldProfile.is_null())
                    for (auto &oldSlot : oldProfile.at("slots"))
                        for (auto &option : oldSlot.at("options"))
                            if (option.value("manualPlacement", false) &&
                                !option.value("baseFallback", false)) {
                                bool found = false;
                                for (auto &slot : p["slots"])
                                    if (slot.at("id") == oldSlot.at("id")) {
                                        bool duplicate = false;
                                        for (auto &o : slot["options"])
                                            duplicate |= o.at("mesh") == option.at("mesh");
                                        if (!duplicate)
                                            slot["options"].push_back(option);
                                        found = true;
                                    }
                                if (!found) {
                                    auto slot = oldSlot;
                                    slot["options"] = J::array({option});
                                    p["slots"].push_back(slot);
                                }
                            }
                resolve_parts(p, oldChoices, true, nullptr, false);
                job.check();
                post([this, token, p, oldChoices] {
                    if (token != revision)
                        return;
                    profile = p; partStateKey.clear();
                    choices = oldChoices;
                    status =
                        "Weapon builder ready. Choose a part in each slot, then preview or export.";
                    details = p.dump(2);
                });
            });
    }
    void discover_donor(const J &donor) {
        if (selected.is_null())
            return;
        cancel_preview();
        uint64_t token = ++revision;
        auto entry = selected;
        auto oldProfile = profile;
        auto oldChoices = choices;
        run("Load donor parts",
            [this, token, entry, donor, oldProfile, oldChoices](JobContext &job) mutable {
                Source source(catalog, data);
                if (oldProfile.is_null()) {
                    oldProfile = discover_parts(source, entry, sourceCache / "parts", &job);
                    resolve_parts(oldProfile, oldChoices, true, nullptr, false);
                }
                auto p = discover_parts(source, entry, sourceCache / "parts", &job, donor);
                if (!oldProfile.is_null()) {
                    for (auto &slot : p["slots"]) {
                        bool found = false;
                        for (auto &target : oldProfile["slots"])
                            if (target["id"] == slot["id"]) {
                                for (auto &option : slot["options"]) {
                                    bool exists = false;
                                    for (auto &previous : target["options"])
                                        exists |= previous.at("mesh") == option.at("mesh");
                                    if (!exists)
                                        target["options"].push_back(option);
                                }
                                found = true;
                                break;
                            }
                        if (!found)
                            oldProfile["slots"].push_back(slot);
                    }
                    p = oldProfile;
                }
                post([this, token, p, oldChoices] {
                    if (token != revision)
                        return;
                    profile = p; partStateKey.clear();
                    choices = oldChoices;
                    status = "Donor parts added. Select a part to place it on the target socket.";
                    details = p.dump(2);
                });
            });
    }
    void preview_animation() {
        if (!preview || selectedClip.is_null())
            return;
        uint64_t token = revision;
        auto model = preview;
        auto row = selectedClip;
        for (auto &t : tasks)
            if (t->name == "Preview animation")
                t->context.cancel = true;
        run("Preview animation", [this, token, model, row](JobContext &job) {
            Source source(catalog, data);
            job.update(0, "Decoding source motion");
            auto clip = decode_clip(source, source.object(row.at("id")), &job);
            auto anim = std::make_shared<Animation>(bind_animation(*model, clip));
            auto bounds = animation_preview_bounds(*model, *anim, &job);
            anim->report["previewBounds"] = {{bounds[0].x, bounds[0].y, bounds[0].z},
                                             {bounds[1].x, bounds[1].y, bounds[1].z}};
            anim->report["previewBoundsSamples"] = anim->frames * 2 - 1;
            job.check();
            post([this, token, row, anim, bounds] {
                if (token != revision || selectedClip.is_null() ||
                    selectedClip.at("id") != row.at("id"))
                    return;
                animation = anim;
                animationBounds = bounds;
                frame = 0;
                playing = true;
                fit_camera();
                status = "Playing source animation.";
                details = anim->report.dump(2);
            });
        });
    }
    void load_gallery_index() {
        if(galleryBusy) return;
        galleryBusy=true; galleryError.clear();
        run("Index weapon icons",[this](JobContext &job) {
            try {
                Source source(catalog,data);
                auto result=weapon_icon_index(source,sourceCache/"gallery",&job);
                post([this,result=std::move(result)] {
                    galleryIcons=result.at("icons"); galleryReady=true; galleryBusy=false;
                    galleryChoiceFilter.clear(); modelFilterKey.clear();
                    if(!result.at("errors").empty()) galleryError=std::to_string(result.at("errors").size())+" icon packages unavailable";
                });
            } catch(const std::exception &e) {
                auto message=std::string(e.what());
                post([this,message] { galleryBusy=false; galleryError=message; });
            }
        });
        tasks.back()->onCancel=[this,task=tasks.back().get()] { if(!task->started) {galleryBusy=false; galleryError="Icon indexing cancelled. Reload icons to resume.";} };
    }
    void load_gallery_images(std::vector<std::string> keys) {
        if(galleryBusy || keys.empty()) return;
        galleryBusy=true;
        J requested=J::object();
        for(auto &key:keys) requested[key]=galleryIcons.at(key);
        run("Load visible weapon icons",[this,requested](JobContext &job) {
            std::unique_ptr<Source> source;
            std::map<std::string,Image> atlases,images;
            std::set<std::string> failed;
            try {
                for(auto it=requested.begin();it!=requested.end();++it) {
                    job.check();
                    try { images[it.key()]=weapon_thumbnail(it.value(),sourceCache/"gallery/thumbs",catalog,data,source,atlases); }
                    catch(const std::exception &) { failed.insert(it.key()); }
                }
            } catch(const std::exception &) { }
            post([this,images=std::move(images),failed=std::move(failed)] {
                galleryBusy=false;
                galleryMissing.insert(failed.begin(),failed.end());
                for(const auto &[key,im]:images) {
                    if(galleryTextures.contains(key)) continue;
                    if(galleryTextures.size()>=128) {
                        auto oldest=std::min_element(galleryTextures.begin(),galleryTextures.end(),
                            [](const auto &a,const auto &b){return a.second.lastFrame<b.second.lastFrame;});
                        glDeleteTextures(1,&oldest->second.id); galleryTextures.erase(oldest);
                    }
                    GLuint id; glGenTextures(1,&id); glBindTexture(GL_TEXTURE_2D,id);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
                    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,im.width,im.height,0,GL_RGBA,GL_UNSIGNED_BYTE,im.pixels.data());
                    galleryTextures[key]={id,ImGui::GetFrameCount()};
                }
            });
        });
        tasks.back()->onCancel=[this,task=tasks.back().get()] { if(!task->started) galleryBusy=false; };
    }
    void upload_textures() {
        if (!uploadPending)
            return;
        for (auto &[key, tex] : textures)
            glDeleteTextures(1, &tex);
        textures.clear();
        if (materials)
            for (auto &[id, m] : materials->materials) {
                const auto &im = m.previewBaseColor;
                if (im.pixels.empty())
                    continue;
                GLuint tex;
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, im.width, im.height, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, im.pixels.data());
                textures[id] = tex;
            }
        uploadPending = false;
    }
    static void draw_preview(const ImDrawList *, const ImDrawCmd *command) {
        static_cast<Studio *>(command->UserCallbackData)->render_model(command->ClipRect);
    }
    void render_model(ImVec4 clip) {
        const auto renderStarted=Clock::now();
        if (previewSize.x < 2 || previewSize.y < 2)
            return;
        auto &io = ImGui::GetIO();
        int framebufferWidth, framebufferHeight;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        auto scale = io.DisplayFramebufferScale;
        int x = int(previewMin.x * scale.x),
            y = framebufferHeight - int((previewMin.y + previewSize.y) * scale.y),
            w = int(previewSize.x * scale.x), h = int(previewSize.y * scale.y);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
        glEnable(GL_SCISSOR_TEST);
        int sx = std::max(x, int(clip.x * scale.x)),
            sy = std::max(y, framebufferHeight - int(clip.w * scale.y));
        int ex = std::min(x + w, int(clip.z * scale.x)),
            ey = std::min(y + h, framebufferHeight - int(clip.y * scale.y));
        glScissor(sx, sy, std::max(0, ex - sx), std::max(0, ey - sy));
        glViewport(x, y, w, h);
        glClearColor(.075f, .075f, .075f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_ALPHA_TEST);
        glDisableClientState(GL_COLOR_ARRAY);
        auto projection =
            glm::perspective(glm::radians(38.f), float(w) / std::max(1, h),
                             std::max(.0001f, distance * .001f), distance * 100 + 100);
        if(orthographic) {
            float halfHeight=distance*std::tan(glm::radians(19.f)),halfWidth=halfHeight*float(w)/std::max(1,h);
            projection=glm::ortho(-halfWidth,halfWidth,-halfHeight,halfHeight,std::max(.0001f,distance*.001f),distance*100+100);
        }
        V3 eye = center + distance * V3(std::cos(pitch) * std::cos(yaw),
                                        std::cos(pitch) * std::sin(yaw), std::sin(pitch));
        auto view = glm::lookAt(eye, center, V3(0, 0, 1));
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadMatrixf(glm::value_ptr(projection));
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadMatrixf(glm::value_ptr(view));
        glDisable(GL_LIGHTING);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        if(showGrid) {
            glDisable(GL_TEXTURE_2D);glDepthMask(GL_FALSE);
            float step=std::pow(10.f,std::floor(std::log10(std::max(distance,.001f))))*.1f;
            float cx=std::floor(center.x/step)*step,cy=std::floor(center.y/step)*step,extent=step*20;
            glBegin(GL_LINES);
            for(int i=-20;i<=20;++i){glColor3f(.16f,.16f,.16f);float at=cx+i*step;glVertex3f(at,cy-extent,0);glVertex3f(at,cy+extent,0);at=cy+i*step;glVertex3f(cx-extent,at,0);glVertex3f(cx+extent,at,0);}
            glColor3f(.5f,.15f,.15f);glVertex3f(cx-extent,0,0);glVertex3f(cx+extent,0,0);
            glColor3f(.15f,.45f,.2f);glVertex3f(0,cy-extent,0);glVertex3f(0,cy+extent,0);
            glEnd();glDepthMask(GL_TRUE);
        }
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, .001f);
        if (preview) {
            poseCache.update(preview,animation,frame);
            glEnableClientState(GL_VERTEX_ARRAY);
            // The viewport is fullbright. Normals and inverse-transpose skinning
            // matrices have no effect while lighting is disabled.
            glDisableClientState(GL_NORMAL_ARRAY);
            std::vector<const Surface *> ordered;
            for (auto &s : preview->surfaces)
                ordered.push_back(&s);
            auto blended = [&](const Surface *s) {
                return materials && materials->materials.contains(s->materialId) &&
                       materials->materials.at(s->materialId)
                               .provenance.value("alphaMode", std::string()) == "blend";
            };
            auto depth = [&](const Surface *surface) {
                auto index=size_t(surface-preview->surfaces.data());
                return glm::length(poseCache.centers[index]-eye);
            };
            std::stable_sort(ordered.begin(), ordered.end(), [&](auto *a, auto *b) {
                if (blended(a) != blended(b))
                    return !blended(a);
                return blended(a) ? depth(a) > depth(b) : false;
            });
            for (auto *surfacePointer : ordered) {
                const auto &surface = *surfacePointer;
                if(surface.materialFailed)continue;
                if (blended(surfacePointer)) {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glDepthMask(GL_FALSE);
                } else {
                    glDisable(GL_BLEND);
                    glDepthMask(GL_TRUE);
                }
                auto index=size_t(surfacePointer-preview->surfaces.data());
                auto *positions=poseCache.positions(index).data();
                if (!surface.uv0.empty() && textures.contains(surface.materialId)) {
                    glBindTexture(GL_TEXTURE_2D, textures.at(surface.materialId));
                    glEnable(GL_TEXTURE_2D);
                    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                    glTexCoordPointer(2, GL_FLOAT, sizeof(V2), surface.uv0.data());
                    glColor4f(1, 1, 1, 1);
                } else {
                    glDisable(GL_TEXTURE_2D);
                    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                    glColor4f(.57f, .64f, .72f, 1);
                }
                glVertexPointer(3, GL_FLOAT, sizeof(V3), positions);
                glDrawElements(GL_TRIANGLES, GLsizei(surface.indices.size()), GL_UNSIGNED_INT,
                               surface.indices.data());
            }
            // Failed surfaces remain inspectable without filling or hiding the model.
            glDisable(GL_TEXTURE_2D);glDisable(GL_BLEND);glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glDepthMask(GL_FALSE);glPolygonMode(GL_FRONT_AND_BACK,GL_LINE);glColor4f(1,.08f,.08f,1);
            for(size_t i=0;i<preview->surfaces.size();++i) {
                const auto &s=preview->surfaces[i];if(!s.materialFailed)continue;
                glVertexPointer(3,GL_FLOAT,sizeof(V3),poseCache.positions(i).data());
                glDrawElements(GL_TRIANGLES,GLsizei(s.indices.size()),GL_UNSIGNED_INT,s.indices.data());
            }
            glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);glDepthMask(GL_TRUE);
        }
        if(depthSample) {
            float px=std::clamp((depthCursor.x-previewMin.x)*scale.x,0.f,float(w-1));
            float py=std::clamp((previewSize.y-(depthCursor.y-previewMin.y))*scale.y,0.f,float(h-1));
            float depth=1;
            if(autoDepth && preview)glReadPixels(x+int(px),y+int(py),1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&depth);
            // Read one pixel only when navigation begins/zooms, never each frame.
            // Empty background falls back to the current orbit-depth plane.
            if(depth>=1 || !std::isfinite(depth))depth=glm::project(center,view,projection,glm::vec4(0,0,w,h)).z;
            auto hit=glm::unProject(V3(px,py,depth),view,projection,glm::vec4(0,0,w,h));
            if(std::isfinite(hit.x) && std::isfinite(hit.y) && std::isfinite(hit.z)) {
                if(depthOrbit)navigationPivot=hit;
                if(depthZoom!=1)zoom_view(center,distance,zoomToMouse?hit:center,depthZoom);
            }
            depthSample=false;depthOrbit=false;depthZoom=1;
        }
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glPopClientAttrib();
        glPopAttrib();
        renderMilliseconds=seconds(renderStarted)*1000;
    }
    void draw();
    void draw_gallery(const std::vector<size_t> &visible);
    void draw_weapon_filters();
    void draw_batch();
    void begin_export() {
        require(!selected.is_null(), "Select a model first");
        exportPopup = true;
        exportError.clear();
        exportModel = true;
        exportAnimations = !selectedClip.is_null();
        exportAllClips = is_weapon_entry(selected);
    }
    void draw_export();
    void draw_donor();
    void start_export();
};
static void heading(const char *text) {
    ImGui::TextUnformatted(text);
    ImGui::Separator();
}
static bool category_tabs(int &current, const std::function<void(const std::string &)> &observe) {
    bool changed = false;
    for (int i = 0; i < 4; i++) {
        if (i)
            ImGui::SameLine(0, 3);
        ImGui::PushID(i);
        bool selected = current == i;
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.15f, .36f, .57f, 1));
        if (ImGui::SmallButton(categories[i].c_str())) {
            current = i;
            changed = true;
        }
        observe(categories[i]);
        if (selected)
            ImGui::PopStyleColor();
        ImGui::PopID();
    }
    return changed;
}
void Studio::draw() {
    auto &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("codm2cast_v14", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Checkbox("Text only", &textOnly)) {
        dirtySettings = true;
        if (textOnly) {
            cancel_preview();
            revision++;
        } else if (!selected.is_null())
            prepare(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh library"))
        index(modelTab);
    ImGui::SameLine();
    if (ImGui::Button("Rescan game files"))
        index(modelTab, true);
    ImGui::SameLine();
    float directoryButtonWidth = ImGui::CalcTextSize("CODM directory...").x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - directoryButtonWidth));
    if (ImGui::Button("CODM directory...")) {
        directoryDraft = gameRoot; directoryError.clear(); directoryPopup = true;
    }
    observe("CODM directory");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", gameRoot.empty() ? "Choose your CODM installation folder" : gameRoot.c_str());
    if (directoryPopup) { ImGui::OpenPopup("Choose CODM directory"); directoryPopup = false; }
    ImGui::SetNextWindowSize(ImVec2(600, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Choose CODM directory", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Choose the CODM PC installation folder containing your game files. The folder is saved in settings.ini.");
        ImGui::SetNextItemWidth(470);
        ImGui::InputText("##codmDirectory", &directoryDraft); observe("CODM directory input");
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) { auto folder = folder_dialog(nullptr, directoryDraft); if (!folder.empty()) directoryDraft = folder; }
        if (!directoryError.empty()) ImGui::TextWrapped("%s", directoryError.c_str());
        if (!tasks.empty()) ImGui::TextWrapped("Wait for current jobs to finish or cancel them before changing folders.");
        ImGui::BeginDisabled(!tasks.empty());
        if (ImGui::Button("Save and load")) { if (set_game_directory()) ImGui::CloseCurrentPopup(); }
        observe("Save CODM directory"); ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(directoryRequired ? "Exit" : "Cancel")) {
            if (directoryRequired) glfwSetWindowShouldClose(window, GLFW_TRUE);
            ImGui::CloseCurrentPopup();
        }
        observe("Cancel CODM directory"); ImGui::EndPopup();
    }
    ImGui::Separator();
    float available = ImGui::GetContentRegionAvail().x;
    float side = std::clamp(available * .23f, 245.f, 365.f);
    float height = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("models", ImVec2(side, height), ImGuiChildFlags_Borders);
    heading("Models");
    if (category_tabs(modelTab, [&](const auto &label) { observe("Models/" + label); }) &&
        !loaded[modelTab])
        load_rows(modelTab);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##modelSearch", "Search name or ID · *ak*", &modelSearch);
    ImGui::BeginDisabled(modelTab == 3);
    ImGui::Checkbox("Complete sets only", &completeOnly);
    ImGui::EndDisabled();
    ImGui::Checkbox("Show lower-detail models (LODs)", &showLods);
    if (modelTab == 1) {
        draw_weapon_filters();
        if(ImGui::Button("Weapon store")) {storeOpen=true;galleryMode=true;}
        if(ImGui::Checkbox("Icon gallery", &galleryMode)) dirtySettings=true;
        if(galleryMode) {
            if(!galleryReady && !galleryBusy && galleryError.empty()) load_gallery_index();
            if(!galleryReady) ImGui::TextUnformatted(galleryBusy ? "Indexing icons in background..." : "Icons unavailable");
            if(!galleryError.empty()) ImGui::TextWrapped("%s",galleryError.c_str());
            ImGui::BeginDisabled(galleryBusy);
            if(ImGui::SmallButton("Reload icons")) {
                for(auto &[key,texture]:galleryTextures) glDeleteTextures(1,&texture.id);
                galleryTextures.clear(); galleryMissing.clear(); galleryReady=false;
                load_gallery_index();
            }
            ImGui::EndDisabled();
            ImGui::Checkbox("Only weapons with matched icons", &galleryIconsOnly);
            const char *sorts[]={"Name","Season","Collaboration","Collection"};
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##gallerySort",&gallerySort,sorts,4);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##galleryTags","Filter season / collab / collection",&galleryTagFilter);
        }
        const char *items[] = {"All perspectives", "First person", "World / third person",
                               "UI / other"};
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##perspective", &perspectiveFilter, items, 4);
    }
    if(modelTab==0 || modelTab==2) {
        ImGui::BeginDisabled(models[modelTab].empty());
        if(ImGui::Button(modelTab==0 ? "Export all players..." : "Export all viewhands...")) open_batch(modelTab,false);
        observe("Export all models");
        ImGui::EndDisabled();
    }
    if(modelTab==1) {
        if(ImGui::Button("Animations for exported models...")){batchFolderAnimations=true;batchPopup=true;}
        observe("Folder animations");
    }
    bool selectVisibleRequested=false;
    if(modelTab!=3) {
        ImGui::BeginDisabled(checkedModels[modelTab].empty());
        if(ImGui::Button("Export selected..."))open_batch(modelTab,true);
        observe("Export selected models");ImGui::EndDisabled();
        ImGui::SameLine();ImGui::Text("%zu selected",checkedModels[modelTab].size());
        if(ImGui::SmallButton("Select visible"))selectVisibleRequested=true;
        ImGui::SameLine();if(ImGui::SmallButton("Clear selection"))checkedModels[modelTab].clear();
        ImGui::TextDisabled("Checkboxes / Ctrl-click / Shift-click");
    }
    if (ImGui::SmallButton("Load / update this tab"))
        index(modelTab);
    observe("Index model tab");
    ImGui::SameLine();
    ImGui::Text("%zu loaded", models[modelTab].size());
    ImGui::BeginChild("modelRows", ImVec2(0, 0));
    auto nextModelFilter =
        J::array({modelTab, modelSearch, completeOnly, showLods, perspectiveFilter, models[modelTab].size(),galleryMode,gallerySort,galleryTagFilter,galleryTagRevision,galleryIconsOnly,galleryReady,weaponCategoryFilter,weaponFamilyFilter})
            .dump();
    if (nextModelFilter != modelFilterKey) {
        modelFilterKey = nextModelFilter;
        visibleModels.clear();
        auto &visible = visibleModels;
        SearchQuery query(modelSearch);
        for (size_t i = 0; i < models[modelTab].size(); i++) {
            auto &row = models[modelTab][i];
            if (!query.match(row) ||
                (!showLods && row.value("lod", std::string("0")) != "0") ||
                (completeOnly && modelTab != 3 && !row.value("complete", false)))
                continue;
            if(modelTab==1 && ((!weaponCategoryFilter.empty() && row.value("weaponCategory",std::string("special"))!=weaponCategoryFilter) ||
                (!weaponFamilyFilter.empty() && row.value("weaponFamily",std::string())!=weaponFamilyFilter))) continue;
            auto p = row.value("perspective", std::string());
            if (modelTab == 1 && perspectiveFilter &&
                ((perspectiveFilter == 1 && p != "view") ||
                 (perspectiveFilter == 2 && p != "world") ||
                 (perspectiveFilter == 3 && (p == "view" || p == "world"))))
                continue;
            visible.push_back(i);
        }
        if(galleryMode && modelTab==1) {
            std::map<size_t,std::string> sortValues;
            const char *fields[]={"","season","collaboration","collection"};
            visible.erase(std::remove_if(visible.begin(),visible.end(),[&](size_t index) {
                auto key=weapon_icon_key(models[modelTab][index].at("name"));
                auto tags=galleryTags.value(key,J::object());
                if(!tags.contains("season")) tags["season"]=models[modelTab][index].value("referenceSeason",std::string());
                if(galleryIconsOnly && galleryReady && !galleryIcons.contains(tags.value("icon",key))) return true;
                sortValues[index]=lower(tags.value(fields[gallerySort],std::string()));
                auto text=lower(tags.value("season",std::string())+" "+tags.value("collaboration",std::string())+" "+tags.value("collection",std::string()));
                return !galleryTagFilter.empty() && text.find(lower(galleryTagFilter))==std::string::npos;
            }),visible.end());
            std::stable_sort(visible.begin(),visible.end(),[&](size_t a,size_t b) {
                if(gallerySort && sortValues[a]!=sortValues[b]) {
                    if(sortValues[a].empty()!=sortValues[b].empty()) return !sortValues[a].empty();
                    return natural_less(sortValues[a],sortValues[b]);
                }
                return natural_less(models[modelTab][a].at("name"),models[modelTab][b].at("name"));
            });
        }
    }
    auto &visible = visibleModels;
    if(selectVisibleRequested)for(auto i:visible)checkedModels[modelTab].insert(models[modelTab][i].at("id"));
    ImGuiListClipper clipper;
    if(galleryMode && modelTab==1) {
        draw_gallery(visible);
    } else {
    clipper.Begin(int(visible.size()));
    while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            auto &item = models[modelTab][visible[row]];
            ImGui::PushID(item.at("id").get_ref<const std::string &>().c_str());
            auto id=item.at("id").get<std::string>();bool marked=checkedModels[modelTab].contains(id);
            if(modelTab!=3) {if(ImGui::Checkbox("##exportSelection",&marked)){if(marked)checkedModels[modelTab].insert(id);else checkedModels[modelTab].erase(id);rangeAnchor[modelTab]=id;}observe("Select model/"+id);ImGui::SameLine();}
            warning_badge(item);
            bool active = !selected.is_null() && selected.at("id") == item.at("id");
            if (ImGui::Selectable(item.at("name").get_ref<const std::string &>().c_str(), active))
                choose_row(item,visible);
            observe("Model row/" + item.at("id").get<std::string>());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(item.at("id").get_ref<const std::string &>().c_str());
                ImGui::TextWrapped(
                    "%s",
                    item.value("completeEvidence", item.value("status", std::string())).c_str());
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("centerColumn", ImVec2(available - 2 * side - 16, height));
    ImGui::BeginChild("center", ImVec2(0,std::max(150.f,height-240.f)), ImGuiChildFlags_Borders);
    heading("Model, variant and parts");
    if (selected.is_null())
        ImGui::TextWrapped(
            "Select a model. Text only keeps browsing free of geometry and texture work.");
    else {
        ImGui::TextWrapped("%s", selected.at("name").get_ref<const std::string &>().c_str());
        if(modelTab==1 && ImGui::CollapsingHeader("Gallery organization / icon")) {
            ImGui::TextWrapped("Saved tags are yours; season/collab/collection tables have not been decoded. Blank means unknown.");
            auto key=weapon_icon_key(selected.at("name"));
            if(!galleryTags.contains(key)) galleryTags[key]=J::object();
            for(auto field:{"season","collaboration","collection"}) {
                auto value=galleryTags.value(key,J::object()).value(field,std::string());
                if(ImGui::InputText(field,&value)) { galleryTags[key][field]=value; ++galleryTagRevision; galleryTagsDirty=true; }
            }
            if(ImGui::Button(galleryTagsDirty ? "Save gallery tags *" : "Save gallery tags")) {
                write_json(root/"gallery-tags.json",galleryTags); galleryTagsDirty=false;
            }
            ImGui::SameLine();
            if(ImGui::Button("Choose game icon")) { if(!galleryReady && !galleryBusy) load_gallery_index(); ImGui::OpenPopup("Choose game icon"); }
            if(ImGui::BeginPopup("Choose game icon")) {
                ImGui::InputTextWithHint("Search icons","Search the game's icon names",&galleryIconSearch);
                auto filter=galleryIconSearch+"/"+std::to_string(galleryIcons.size());
                if(filter!=galleryChoiceFilter) {
                    galleryChoiceFilter=filter; galleryChoices.clear();
                    auto needle=lower(galleryIconSearch);
                    for(auto it=galleryIcons.begin();it!=galleryIcons.end();++it)
                        if(needle.empty() || lower(it.value().at("name")).find(needle)!=std::string::npos) galleryChoices.push_back(it.key());
                }
                const auto &matches=galleryChoices;
                ImGui::BeginChild("iconChoices",ImVec2(540,260));
                ImGuiListClipper choicesClip; choicesClip.Begin(int(matches.size()));
                std::string hovered;
                while(choicesClip.Step()) for(int i=choicesClip.DisplayStart;i<choicesClip.DisplayEnd;++i) {
                    const auto &candidate=matches[i];
                    if(ImGui::Selectable(galleryIcons.at(candidate).at("name").get_ref<const std::string &>().c_str())) {
                        galleryTags[key]["icon"]=candidate; galleryTagsDirty=true; ++galleryTagRevision;
                    }
                    if(ImGui::IsItemHovered()) hovered=candidate;
                }
                ImGui::EndChild();
                auto chosen=galleryTags.value(key,J::object()).value("icon",key);
                auto sample=hovered.empty()?chosen:hovered;
                if(galleryTextures.contains(sample)) ImGui::Image((ImTextureID)(intptr_t)galleryTextures.at(sample).id,ImVec2(384,192));
                else if(galleryIcons.contains(sample) && !galleryMissing.contains(sample)) load_gallery_images({sample});
                if(ImGui::Button("Use automatic match")) { galleryTags[key].erase("icon"); galleryTagsDirty=true; ++galleryTagRevision; }
                ImGui::EndPopup();
            }
        }
        if (modelTab == 1 &&
            ImGui::BeginCombo("Weapon variant",
                              selected.at("name").get_ref<const std::string &>().c_str())) {
            for (auto index : visible) {
                auto &row = models[modelTab][index];
                if (ImGui::Selectable(row.at("name").get_ref<const std::string &>().c_str(),
                                      row.at("id") == selected.at("id")))
                    select(row);
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Preview selected model"))
            prepare(profile.is_null());
        observe("Prepare selected model");
        ImGui::SameLine();
        if (ImGui::Button("Export..."))
            begin_export();
        observe("Open export");
        if (is_weapon_entry(selected)) {
            if (ImGui::Button(profile.is_null() ? "Build weapon" : "Refresh part sets"))
                build_weapon();
            observe("Build weapon");
            ImGui::SameLine();
            if (ImGui::Button("Parts from another weapon"))
                donorPopup = true;
            observe("Load part set");
        }
        const char *qualityNames[] = {"Fast · 256", "Balanced · 512", "Detailed · 1024",
                                      "Full source"};
        ImGui::SetNextItemWidth(170);
        if (ImGui::Combo("Preview texture quality", &quality, qualityNames, 4)) {
            dirtySettings = true;
            if (!textOnly || preview)
                prepare(false);
        }
        if (!profile.is_null() && ImGui::CollapsingHeader("Weapon builder / attachments")) {
            ImGui::TextWrapped(
                "Choose parts below. Changes update the preview; export uses this assembly.");
            if (profile.contains("baseWeapon") && !profile["baseWeapon"].is_null())
                ImGui::TextWrapped(
                    "Missing choices filled from: %s",
                    profile["baseWeapon"]["name"].get_ref<const std::string &>().c_str());
            if (profile.at("slots").empty())
                ImGui::TextWrapped("No swappable parts found for this model. Load another weapon's "
                                   "part set to add donor choices.");
            bool edited = false;
            auto nextPartStateKey=choices.dump();
            if(partStateKey!=nextPartStateKey) {
                auto checkedChoices=choices;
                resolve_parts(profile,checkedChoices,false,&cachedPartStates,false);
                partStateKey=std::move(nextPartStateKey);
            }
            auto &partStates=cachedPartStates;
            ImGui::BeginChild(
                "parts", ImVec2(0, std::min(180.f, 40.f * float(profile.at("slots").size()) + 25)),
                ImGuiChildFlags_Borders);
            for (auto &slot : profile.at("slots")) {
                auto id = slot.at("id").get<std::string>();
                std::string label = "None";
                if (choices.contains(id) && !choices[id].is_null())
                    for (auto &o : slot.at("options"))
                        if (o.at("mesh") == choices[id])
                            label = o.at("name");
                auto &partState = partStates.at(id);
                if (!partState.at("reason").get<std::string>().empty())
                    label += " (inactive)";
                ImGui::PushID(id.c_str());
                ImGui::SetNextItemWidth(std::max(100.f, ImGui::GetContentRegionAvail().x - 120));
                bool partOpen = ImGui::BeginCombo(
                    slot.at("label").get_ref<const std::string &>().c_str(), label.c_str());
                observe("Part slot/" + id);
                if (partOpen) {
                    if (ImGui::Selectable("None", label == "None")) {
                        choices[id] = nullptr;
                        edited = true;
                    }
                    observe("Part None/" + id);
                    for (auto &option : partState.at("options")) {
                        auto name = option.at("name").get<std::string>();
                        if (option.value("baseFallback", false))
                            name += " (base weapon)";
                        else if (option.value("manualPlacement", false))
                            name += " (donor)";
                        if (ImGui::Selectable(name.c_str(),
                                              choices.value(id, J()) == option.at("mesh"))) {
                            choices[id] = option.at("mesh");
                            edited = true;
                        }
                        observe("Part option/" + id + "/" + option.at("mesh").get<std::string>());
                    }
                    ImGui::EndCombo();
                }
                if (choices.contains(id) && !choices[id].is_null())
                    for (auto &option : slot.at("options"))
                        if (option.at("mesh") == choices[id] && option.contains("sourceWeapon"))
                            ImGui::TextDisabled(
                                "From: %s",
                                option.at("sourceWeapon").get_ref<const std::string &>().c_str());
                if (!partState.at("reason").get<std::string>().empty())
                    ImGui::TextWrapped(
                        "%s", partState.at("reason").get_ref<const std::string &>().c_str());
                ImGui::PopID();
            }
            ImGui::TextDisabled("First compatible options are not a verified game loadout.");
            ImGui::EndChild();
            if (edited)
                prepare(false);
        }
        if (preview && ImGui::CollapsingHeader("Surface materials / overrides")) {
            ImGui::BeginChild("surfaceOverrides", ImVec2(0, 170), ImGuiChildFlags_Borders);
            bool apply = false;
            for (auto &s : preview->surfaces) {
                auto key = s.meshId + ":" + std::to_string(s.submesh);
                ImGui::PushID(key.c_str());
                ImGui::TextWrapped("%s · slot %d", s.name.c_str(), s.submesh);
                auto &value = overrideInputs[key];
                ImGui::SetNextItemWidth(-65);
                ImGui::InputTextWithHint("##material", "Material CAB:path-id", &value);
                ImGui::SameLine();
                if (ImGui::SmallButton("Apply")) {
                    if (value.empty()) {
                        if (overrides.contains(s.meshId))
                            overrides[s.meshId].erase(std::to_string(s.submesh));
                    } else
                        overrides[s.meshId][std::to_string(s.submesh)] = value;
                    apply = true;
                }
                if (materials)
                    for (auto &r : materials->surfaces)
                        if (r.value("mesh", std::string()) == s.meshId &&
                            r.value("slot", -1) == s.submesh) {
                            ImGui::TextWrapped(
                                "%s", r.value("method", std::string("Unresolved")).c_str());
                            if (ImGui::BeginCombo("Candidates", value.empty() ? "Choose a material"
                                                                              : value.c_str())) {
                                for (auto &c : r.value("candidates", J::array()))
                                    if (ImGui::Selectable(
                                            c.at("name").get_ref<const std::string &>().c_str())) {
                                        value = c.at("id");
                                        overrides[s.meshId][std::to_string(s.submesh)] = value;
                                        apply = true;
                                    }
                                ImGui::EndCombo();
                            }
                        }
                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (apply)
                prepare(false);
        }
    }
    if (preview) {
        ImGui::Text("%zu surfaces · %zu bones", preview->surfaces.size(), preview->bones.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Frame model"))
            fit_camera();
    }
    if (animation) {
        if (ImGui::SmallButton(playing ? "Pause" : "Play"))
            playing = !playing;
        observe("Toggle playback");
        ImGui::SameLine();
        float f = float(frame);
        ImGui::SetNextItemWidth(-160);
        if (ImGui::SliderFloat("##frame", &f, 0, float(animation->frames - 1), "Frame %.1f")) {
            frame = f;
            playing = false;
        }
        ImGui::SameLine();
        ImGui::Text("%.0f fps", animation->fps);
    }
    if(ImGui::SmallButton("Front")){yaw=-glm::half_pi<float>();pitch=0;}
    ImGui::SameLine();if(ImGui::SmallButton("Right")){yaw=0;pitch=0;}
    ImGui::SameLine();if(ImGui::SmallButton("Top")){yaw=-glm::half_pi<float>();pitch=glm::half_pi<float>()-.001f;}
    ImGui::SameLine();if(ImGui::SmallButton(orthographic?"Orthographic":"Perspective"))orthographic=!orthographic;
    ImGui::SameLine();if(ImGui::SmallButton("Viewport preferences"))ImGui::OpenPopup("Navigation preferences");
    observe("Viewport preferences");
    if(ImGui::BeginPopup("Navigation preferences")) {
        ImGui::TextUnformatted("Blender-style navigation");ImGui::Separator();
        dirtySettings|=ImGui::Checkbox("Auto Depth",&autoDepth);observe("Navigation auto depth");
        dirtySettings|=ImGui::Checkbox("Zoom to mouse position",&zoomToMouse);
        dirtySettings|=ImGui::Checkbox("Emulate middle mouse (Alt + left)",&emulateMiddle);
        dirtySettings|=ImGui::Checkbox("Floor grid and axes",&showGrid);
        dirtySettings|=ImGui::SliderFloat("Orbit sensitivity",&orbitSpeed,.001f,.03f,"%.3f");
        dirtySettings|=ImGui::SliderFloat("Zoom sensitivity",&zoomSpeed,.02f,.5f,"%.2f");
        ImGui::TextDisabled("Solid depth stays enabled. Preferences are saved.");
        if(ImGui::Button("Close"))ImGui::CloseCurrentPopup();observe("Close navigation preferences");
        ImGui::EndPopup();
    }
    ImGui::TextDisabled("MMB: orbit | Shift-MMB: pan | Ctrl-MMB / wheel: zoom");
    previewMin=ImGui::GetCursorScreenPos();previewSize=ImGui::GetContentRegionAvail();previewSize.y=std::max(100.f,previewSize.y);
    if(cameraNeedsFit && preview){fit_camera();cameraNeedsFit=false;}
    ImGui::InvisibleButton("viewport",previewSize,ImGuiButtonFlags_MouseButtonMiddle|ImGuiButtonFlags_MouseButtonLeft);
    observe("Viewport navigation area");
    bool hovered=ImGui::IsItemHovered(),active=ImGui::IsItemActive();
    int mouse=emulateMiddle && io.KeyAlt?ImGuiMouseButton_Left:ImGuiMouseButton_Middle;
    if(hovered && ImGui::IsMouseClicked(mouse)) {
        navigationPivot=center;
        if(autoDepth){depthSample=true;depthOrbit=true;depthCursor=io.MousePos;depthZoom=1;}
    }
    if((hovered || active) && ImGui::IsMouseDragging(mouse) && !ImGui::IsMouseClicked(mouse)) {
        if(io.KeyShift) {
            auto basis=view_basis(yaw,pitch);float amount=2*distance*std::tan(glm::radians(19.f))/std::max(1.f,previewSize.y);
            auto delta=(-basis[0]*io.MouseDelta.x+basis[1]*io.MouseDelta.y)*amount;center+=delta;navigationPivot+=delta;
        } else if(io.KeyCtrl)zoom_view(center,distance,zoomToMouse?navigationPivot:center,std::exp(io.MouseDelta.y*zoomSpeed*.05f));
        else {
            float oldYaw=yaw,oldPitch=pitch;yaw-=io.MouseDelta.x*orbitSpeed;pitch=std::clamp(pitch+io.MouseDelta.y*orbitSpeed,-1.5698f,1.5698f);
            center=orbit_center(center,navigationPivot,oldYaw,oldPitch,yaw,pitch);
        }
    }
    if(hovered && io.MouseWheel!=0) {
        auto factor=std::exp(-io.MouseWheel*zoomSpeed);
        if(zoomToMouse){depthSample=true;depthOrbit=false;depthCursor=io.MousePos;depthZoom=factor;}
        else zoom_view(center,distance,center,factor);
    }
    if(hovered && !io.WantTextInput) {
        if(ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal) || ImGui::IsKeyPressed(ImGuiKey_Home))fit_camera();
        if(ImGui::IsKeyPressed(ImGuiKey_Keypad1)){yaw=io.KeyCtrl?glm::half_pi<float>():-glm::half_pi<float>();pitch=0;}
        if(ImGui::IsKeyPressed(ImGuiKey_Keypad3)){yaw=io.KeyCtrl?glm::pi<float>():0;pitch=0;}
        if(ImGui::IsKeyPressed(ImGuiKey_Keypad7)){yaw=-glm::half_pi<float>();pitch=io.KeyCtrl?-1.5698f:1.5698f;}
        if(ImGui::IsKeyPressed(ImGuiKey_Keypad5))orthographic=!orthographic;
    }
    ImGui::GetWindowDrawList()->AddCallback(draw_preview, this);
    ImGui::EndChild();
    ImGui::BeginChild("activity", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextWrapped("%s", status.c_str());
    std::vector<Task*> activityTasks;
    for(auto &task:tasks) activityTasks.push_back(task.get());
    std::stable_sort(activityTasks.begin(),activityTasks.end(),[](const auto *a,const auto *b) {
        return (a->name=="Export assets") > (b->name=="Export assets");
    });
    for (auto *task : activityTasks) {
        ImGui::PushID(task);
        std::string label;
        {
            std::lock_guard lock(task->context.mutex);
            label = task->context.label;
        }
        ImGui::Text("%s | %.0fs%s", task->name.c_str(), seconds(task->created), task->started ? "" : " | queued");
        std::string stage;
        { std::lock_guard lock(task->context.mutex); stage=task->context.stage; }
        if(!stage.empty()) ImGui::TextWrapped("%s",stage.c_str());
        ImGui::TextWrapped("%s",label.c_str());
        float progress=task->context.progress.load();
        std::string progressLabel=progress>0 ? std::to_string(int(std::clamp(progress,0.f,1.f)*100))+"% of current step" : (task->started ? "Working..." : "Queued");
        ImGui::SetNextItemWidth(std::max(120.f, ImGui::GetContentRegionAvail().x - 75));
        ImGui::ProgressBar(progress>0 ? progress : (task->started ? -float(ImGui::GetTime()) : 0.f),
                           ImVec2(std::max(120.f, ImGui::GetContentRegionAvail().x - 75), 0), progressLabel.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) {
            task->context.cancel = true;
            if (task->onCancel)
                task->onCancel();
            if (task->name == "Prepare preview" || task->name == "Preview animation" ||
                task->name == "Load donor parts" || task->name == "Discover parts")
                ++revision;
            if (!task->started)
                status = "Cancelled: " + task->name + " (queued)";
        }
        observe("Cancel task/" + task->name);
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Details", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Copy details"))
            ImGui::SetClipboardText(details.c_str());
        ImGui::SameLine();
        ImGui::TextWrapped(
            "Offline files · original source rigs · metres by default");
        ImGui::BeginChild("details", ImVec2(0, 90), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(details.c_str());
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("animations", ImVec2(side, height), ImGuiChildFlags_Borders);
    heading("Animations");
    if (category_tabs(clipTab, [&](const auto &label) { observe("Animations/" + label); }) &&
        !loaded[clipTab])
        load_rows(clipTab);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##clipSearch", "Search animations", &clipSearch);
    ImGui::Checkbox("Selected model only", &applicableOnly);
    if (applicableOnly && uncheckedClips)
        ImGui::TextWrapped("%zu %s binding checks; preview/export verifies them.", uncheckedClips,
                           uncheckedClips == 1 ? "clip awaits" : "clips await");
    if (ImGui::SmallButton("Load / update this tab##anim"))
        index(clipTab);
    if (is_weapon_entry(selected)) {
        if (ImGui::SmallButton("Discover shared animations"))
            discover_shared_animations();
        if (!inheritedStatus.empty())
            ImGui::TextWrapped("%s", inheritedStatus.c_str());
    }
    ImGui::BeginDisabled(!preview || selectedClip.is_null());
    if (ImGui::Button("Preview animation on model"))
        preview_animation();
    observe("Preview selected animation");
    ImGui::EndDisabled();
    if (!selectedClip.is_null()) {
        ImGui::TextWrapped("%s", selectedClip.at("name").get_ref<const std::string &>().c_str());
        ImGui::TextWrapped("Action: %s", animation_action(selectedClip).c_str());
    }
    ImGui::BeginChild("animationRows", ImVec2(0, 0));
    auto nextClipFilter =
        J::array({clipTab, clipSearch, applicableOnly, selected.is_null() ? J() : selected.at("id"),
                  clips[clipTab].size(), preview ? preview->bones.size() : 0})
            .dump();
    if (nextClipFilter != clipFilterKey) {
        clipFilterKey = nextClipFilter;
        visibleClips.clear();
        uncheckedClips = 0;
        auto &av = visibleClips;
        auto target = selected;
        if (preview && !target.is_null()) {
            std::set<uint32_t> paths;
            for (auto &bone : preview->bones)
                paths.insert(bone.paths.begin(), bone.paths.end());
            target["paths"] = paths;
        }
        AnimationFilter filter(target);
        SearchQuery query(clipSearch);
        for (size_t i = 0; i < clips[clipTab].size(); i++) {
            auto &row = clips[clipTab][i];
            if (!query.match(row))
                continue;
            if (applicableOnly) {
                auto match = filter.match(row);
                if (match == AnimationMatch::Unmatched)
                    continue;
                uncheckedClips += match == AnimationMatch::Unchecked;
            }
            av.push_back(i);
        }
    }
    auto &av = visibleClips;
    clipper.Begin(int(av.size()));
    while (clipper.Step())
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; index++) {
            auto &row = clips[clipTab][av[index]];
            ImGui::PushID(row.at("id").get_ref<const std::string &>().c_str());
            if (ImGui::Selectable(row.at("name").get_ref<const std::string &>().c_str(),
                                  !selectedClip.is_null() &&
                                      selectedClip.at("id") == row.at("id"))) {
                selectedClip = row;
                animation.reset();
                animationBounds.reset();
                playing = false;
                status = "Clip selected; source bindings will be checked on preview/export.";
            }
            observe("Clip row/" + row.at("id").get<std::string>());
            ImGui::PopID();
        }
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::End();
    draw_export();
    draw_donor();
    draw_batch();
    if(storeOpen) {
        ImGui::SetNextWindowSize(ImVec2(1100,760),ImGuiCond_FirstUseEver);
        if(ImGui::Begin("Weapon store",&storeOpen)) {
            draw_weapon_filters();
            ImGui::InputTextWithHint("##storeSearch","Search weapon, skin or ID",&modelSearch);
            ImGui::Text("%zu matching indexed models",visibleModels.size());
            ImGui::BeginChild("storeCards",ImVec2(0,0));
            if(modelTab==1) draw_gallery(visibleModels);
            ImGui::EndChild();
        }
        ImGui::End();
    }
}

void Studio::draw_weapon_filters() {
    static const std::map<std::string,std::string> labels={{"ar","Assault rifles"},{"smg","Submachine guns"},{"lmg","Light machine guns"},{"shotty","Shotguns"},{"sniper","Sniper rifles"},{"marksman","Marksman rifles"},{"pistol","Pistols"},{"launch","Launchers"},{"melee","Melee"},{"special","Other / unclassified"}};
    auto categoryLabel=[&](const std::string &key) {auto it=labels.find(key);return it==labels.end()?key:it->second;};
    if(familyMenuSize!=models[1].size()) {
        familyMenuSize=models[1].size();familyMenu.clear();
        for(const auto &row:models[1]) if(row.value("complete",false) && row.value("lod",std::string("0"))=="0") {
            auto category=row.value("weaponCategory",std::string("special"));
            auto id=row.value("weaponFamily",std::string());if(id.empty()) continue;
            auto &item=familyMenu[category][id];item.first=row.value("weaponLabel",id);++item.second;
        }
    }
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##weaponCategory",weaponCategoryFilter.empty()?"All weapon categories":categoryLabel(weaponCategoryFilter).c_str())) {
        if(ImGui::Selectable("All weapon categories",weaponCategoryFilter.empty())) {weaponCategoryFilter.clear();weaponFamilyFilter.clear();}
        for(const auto &[category,families]:familyMenu)
            if(ImGui::Selectable(categoryLabel(category).c_str(),weaponCategoryFilter==category)) {weaponCategoryFilter=category;weaponFamilyFilter.clear();}
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##weaponFamily",weaponFamilyFilter.empty()?"All weapons / variants":weaponFamilyFilter.c_str())) {
        if(ImGui::Selectable("All weapons / variants",weaponFamilyFilter.empty())) weaponFamilyFilter.clear();
        for(const auto &[category,families]:familyMenu) if(weaponCategoryFilter.empty() || category==weaponCategoryFilter)
            for(const auto &[id,item]:families) if(ImGui::Selectable((item.first+" ("+std::to_string(item.second)+")##"+id).c_str(),weaponFamilyFilter==id)) weaponFamilyFilter=id;
        ImGui::EndCombo();
    }
    if(ImGui::SmallButton("Discovery audit")) {
        auto report=discovery_audit(models[1],weaponReference);
        write_json(root/"weapon-discovery-audit.json",report);details=report.dump(2);
        status="Discovery audit saved: "+pathstr(root/"weapon-discovery-audit.json");
    }
}
void Studio::draw_batch() {
    if(batchPopup){ImGui::OpenPopup("Export models");batchPopup=false;}
    ImGui::SetNextWindowSize(ImVec2(760,0),ImGuiCond_Appearing);
    if(ImGui::BeginPopupModal("Export models",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        if(batchFolderAnimations) {
            ImGui::TextWrapped("Export all matching weapon animation sets for existing viewmodel CAST files. Saved source IDs are preferred; otherwise an exact unique export name is required. Animation bones are checked against each existing model. Models are preserved.");
            if(ImGui::InputText("Models folder to check",&modelsDestination))dirtySettings=true;
            ImGui::SameLine();if(ImGui::Button("Browse models")){auto p=folder_dialog(nullptr,modelsDestination);if(!p.empty()){modelsDestination=p;dirtySettings=true;}}
            if(ImGui::InputText("Animations destination",&animationsDestination))dirtySettings=true;
            ImGui::SameLine();if(ImGui::Button("Browse animations")){auto p=folder_dialog(nullptr,animationsDestination);if(!p.empty()){animationsDestination=p;dirtySettings=true;}}
            ImGui::TextWrapped("Subfolders are included. Identical existing animations are reused; conflicting files and unmatched models are reported.");
            bool active=false;for(const auto &task:tasks)active|=task->name=="Export assets" && !task->done;
            ImGui::BeginDisabled(active || modelsDestination.empty() || animationsDestination.empty());
            if(ImGui::Button("Check folder and export animations")) {
                ExportRequest options;options.catalog=catalog;options.database=database;options.data=data;options.modelsDestination=pathof(modelsDestination);options.animationsDestination=pathof(animationsDestination);
                run("Export assets",[this,options](JobContext &job){auto report=export_folder_animations(options,&job);post([this,report]{details=report.dump(2);status="Folder animations: "+std::to_string(report.at("verifiedModels").get<int>())+" / "+std::to_string(report.at("items").size())+" models have verified animations; "+std::to_string(report.at("partial").get<int>())+" partial sets; "+std::to_string(report.at("failed").get<int>())+" failed. Report: "+report.at("reportPath").get<std::string>();});});
                ImGui::CloseCurrentPopup();
            }
            observe("Export folder animations");ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();observe("Cancel folder animations");
            ImGui::EndPopup();return;
        }
        ImGui::Text("%zu %s models",batchEntries.size(),batchSelected?"selected":"loaded");
        if(batchTab==1) ImGui::TextWrapped("Uses each weapon's configured attachments. Weapons you have not configured use discovered defaults.");
        if(ImGui::Checkbox(batchTab==1?"Include matching viewmodels and worldmodels":batchTab==0?"Include corresponding viewhands":"Include corresponding player models",&batchPair))resolve_batch();
        observe("Batch pair models");
        ImGui::TextWrapped(batchTab==1?"Exports assembled models and textures using each weapon's default compatible part set. Optional animations include variant actions, base-weapon fallbacks and paired camera clips for viewmodels.":"Exports models and textures. Counterparts require an exact variant match; missing or ambiguous matches are reported.");
        if(batchTab==1){ImGui::Checkbox("Include corresponding weapon animations",&batchAnimations);observe("Batch animations");}
        if(batchPlanning)ImGui::TextUnformatted("Finding matching models...");
        if(!batchError.empty())ImGui::TextWrapped("%s",batchError.c_str());
        if(!batchPlanning && !batchPlan.is_null()) {
            ImGui::Text("%zu total models; %zu recognized pairs; %zu pairing warnings",batchPlan["entries"].size(),batchPlan["pairs"].size(),batchPlan["issues"].size());
            if(ImGui::CollapsingHeader("Review models and pairing warnings")) {
                ImGui::BeginChild("batchReview",ImVec2(710,180),ImGuiChildFlags_Borders);
                for(const auto &row:batchPlan["entries"])ImGui::TextWrapped("%s",row.at("name").get_ref<const std::string &>().c_str());
                for(const auto &issue:batchPlan["issues"])ImGui::TextWrapped("! %s: %s",issue.at("name").get_ref<const std::string &>().c_str(),issue.at("reason").get_ref<const std::string &>().c_str());
                ImGui::EndChild();
            }
        }
        if(ImGui::InputText("Models destination",&modelsDestination))dirtySettings=true;ImGui::SameLine();
        if(ImGui::Button("Browse")){auto folder=folder_dialog(nullptr,modelsDestination);if(!folder.empty()){modelsDestination=folder;dirtySettings=true;}}
        if(batchTab==1 && batchAnimations){
            if(ImGui::InputText("Animations destination",&animationsDestination))dirtySettings=true;
            ImGui::SameLine();if(ImGui::Button("Browse animations")){auto p=folder_dialog(nullptr,animationsDestination);if(!p.empty()){animationsDestination=p;dirtySettings=true;}}
            ImGui::TextWrapped("Existing models still receive missing animations when Skip existing exports is enabled. Worldmodels use their paired viewmodel's animation set.");
        }
        ImGui::Checkbox("Skip existing exports",&batchSkipExisting);observe("Batch skip existing");
        dirtySettings|=ImGui::Checkbox("Omit unresolved surfaces on export",&omitUnresolved);
        ImGui::TextWrapped("Omitted surfaces are recorded in the report. Preview keeps them as red wireframes.");
        if(batchTab!=1) {ImGui::Checkbox("Skip models with unresolved / untextured materials",&batchSkipUntextured);observe("Batch skip untextured");}
        ImGui::TextWrapped(omitUnresolved?"Unresolved surfaces are omitted before the material policy below is applied; models with no usable surfaces fail.":batchTab==1?"Weapons with unresolved materials fail individually and are listed in the report.":batchSkipUntextured?"Untextured models are skipped. Supported source-texture approximations are retained.":"Unresolved surfaces receive a neutral material and are listed in the report.");
        bool active=false;for(const auto &task:tasks)active|=task->name=="Export assets" && !task->done;
        ImGui::BeginDisabled(active || batchPlanning || !batchError.empty() || batchEntries.empty() || modelsDestination.empty() || (batchTab==1 && batchAnimations && animationsDestination.empty()));
        if(ImGui::Button("Export models")) {
            ExportRequest options;options.catalog=catalog;options.database=database;options.data=data;options.modelsDestination=pathof(modelsDestination);options.t6=false;
            options.omitUnresolved=omitUnresolved;
            options.allWeaponClips=batchTab==1 && batchAnimations;options.animationsDestination=pathof(animationsDestination);
            auto plan=configured_batch_plan();
            run("Export assets",[this,options,plan,skip=batchSkipExisting,strict=batchSkipUntextured](JobContext &job){
                auto report=export_selected_models(options,plan,skip,strict,&job);
                post([this,report]{
                    for(const auto &item:report.at("items"))if(item.at("status")!="skipped" || item.contains("materialErrors"))record_issue({{"id",item.at("source")}},item,false);
                    if(fs::exists(catalog))write_json(root/"asset-issues.json",{{"catalogStamp",std::to_string(fs::last_write_time(catalog).time_since_epoch().count())},{"issues",knownIssues}});
                    details=report.dump(2);status="Batch "+report.at("status").get<std::string>()+": "+std::to_string(report.at("exported").get<int>())+" exported, "+std::to_string(report.value("partial",0))+" partial, "+std::to_string(report.at("skipped").get<int>())+" skipped, "+std::to_string(report.at("failed").get<int>())+" failed. Report: "+report.at("reportPath").get<std::string>();
                });
            });
            dirtySettings=true;ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Cancel")){++batchRevision;batchPlanning=false;for(auto &task:jobs.tasks)if(task->name=="Resolve export pairs")task->context.cancel=true;ImGui::CloseCurrentPopup();}
        observe("Cancel batch");ImGui::EndPopup();
    }
}
void Studio::draw_gallery(const std::vector<size_t> &visible) {
        const int columns=std::max(1,int(ImGui::GetContentRegionAvail().x/145.f));
        const float cellWidth=(ImGui::GetContentRegionAvail().x-(columns-1)*ImGui::GetStyle().ItemSpacing.x)/columns;
        constexpr float cellHeight=116.f;
        std::vector<std::string> wanted;
        ImGuiListClipper rows;
        rows.Begin(int((visible.size()+columns-1)/columns),cellHeight+ImGui::GetStyle().ItemSpacing.y);
        while(rows.Step()) for(int row=rows.DisplayStart;row<rows.DisplayEnd;++row) {
            for(int col=0;col<columns;++col) {
                size_t index=size_t(row)*columns+col;
                if(index>=visible.size()) break;
                if(col) ImGui::SameLine();
                auto &item=models[1][visible[index]];
                const auto name=item.at("name").get<std::string>();
                auto key=weapon_icon_key(name);
                if(galleryTags.contains(key)) key=galleryTags.at(key).value("icon",key);
                ImGui::PushID(item.at("id").get_ref<const std::string &>().c_str());
                auto pos=ImGui::GetCursorScreenPos();
                bool active=!selected.is_null() && selected.at("id")==item.at("id");
                if(ImGui::Selectable("##weaponIcon",active || checkedModels[1].contains(item.at("id")),0,ImVec2(cellWidth,cellHeight))) {
                    auto mouse=ImGui::GetMousePos();auto id=item.at("id").get<std::string>();
                    if(mouse.x<pos.x+24 && mouse.y<pos.y+24){if(!checkedModels[1].erase(id))checkedModels[1].insert(id);}
                    else {choose_row(item,visible);if(!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift)storeOpen=false;}
                }
                observe("Model row/"+item.at("id").get<std::string>());
                auto *draw=ImGui::GetWindowDrawList();
                if(galleryTextures.contains(key)) {
                    auto &texture=galleryTextures.at(key); texture.lastFrame=ImGui::GetFrameCount();
                    float w=std::min(cellWidth-8,156.f),h=w/2,left=(cellWidth-w)/2;
                    draw->AddImage((ImTextureID)(intptr_t)texture.id,ImVec2(pos.x+left,pos.y+4),ImVec2(pos.x+left+w,pos.y+4+h));
                } else {
                    bool available=galleryReady && galleryIcons.contains(key) && !galleryMissing.contains(key);
                    draw->AddText(ImVec2(pos.x+6,pos.y+24),ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        available ? "Loading icon..." : galleryReady ? "No matching icon" : "Indexing icons...");
                    if(available && wanted.size()<8 && std::find(wanted.begin(),wanted.end(),key)==wanted.end()) wanted.push_back(key);
                }
                draw->AddRectFilled(ImVec2(pos.x+3,pos.y+3),ImVec2(pos.x+21,pos.y+21),ImGui::GetColorU32(ImGuiCol_FrameBg),3);
                if(checkedModels[1].contains(item.at("id")))draw->AddText(ImVec2(pos.x+7,pos.y+3),ImGui::GetColorU32(ImGuiCol_CheckMark),"x");
                auto problem=row_issue(item);
                if(!problem.empty())draw->AddText(ImVec2(pos.x+cellWidth-20,pos.y+4),IM_COL32(245,180,40,255),"!");
                auto label=name;
                if(auto id=weapon_identity(name)) label=id->first+" ("+id->second+")";
                ImVec4 clip(pos.x+4,pos.y+cellHeight-30,pos.x+cellWidth-4,pos.y+cellHeight);
                draw->AddText(ImGui::GetFont(),ImGui::GetFontSize(),ImVec2(clip.x,clip.y),ImGui::GetColorU32(ImGuiCol_Text),label.c_str(),nullptr,cellWidth-8,&clip);
                if(ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::TextUnformatted(name.c_str()); ImGui::TextUnformatted(item.at("id").get_ref<const std::string &>().c_str()); if(!problem.empty())ImGui::TextWrapped("%s",problem.c_str()); ImGui::EndTooltip(); }
                ImGui::PopID();
            }
        }
        if(galleryReady) load_gallery_images(std::move(wanted));
}
void Studio::draw_donor() {
    if (donorPopup) {
        ImGui::OpenPopup("Parts from another weapon");
        donorPopup = false;
    }
    ImGui::SetNextWindowSize(ImVec2(650, 500), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Parts from another weapon", nullptr,
                               ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("Choose a donor. Its material provenance is retained; parts use the "
                           "selected target's sockets.");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##donor", "Search donor weapons", &donorSearch);
        ImGui::BeginChild("donorRows", ImVec2(0, -40));
        for (auto &row : models[1])
            if (row.value("complete", false) && search_match(donorSearch, row) &&
                (showLods || row.value("lod", std::string("0")) == "0")) {
                ImGui::PushID(row.at("id").get_ref<const std::string &>().c_str());
                if (ImGui::Selectable(row.at("name").get_ref<const std::string &>().c_str())) {
                    discover_donor(row);
                    ImGui::CloseCurrentPopup();
                }
                observe("Donor row/" + row.at("id").get<std::string>());
                ImGui::PopID();
            }
        ImGui::EndChild();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
void Studio::start_export() {
    ExportRequest request;
    request.entry = selected;
    request.catalog = catalog;
    request.database = database;
    request.data = data;
    request.modelsDestination = pathof(modelsDestination);
    request.animationsDestination = pathof(animationsDestination);
    request.stem = exportStem;
    request.t6 = false;
    request.omitUnresolved = omitUnresolved;
    request.model = exportModel;
    request.includeWorldmodel = exportWorld && exportModel && is_weapon_entry(selected) &&
        selected.value("perspective", perspective(selected.at("name"))) == "view";
    request.overrides = overrides;
    request.category = exportCategory;
    request.sniperArchetype = sniperArchetype;
    request.title = exportWeapon + " " + exportVariant;
    if (includeViewhands && exportModel) {
        require(!companion.is_null(), "Choose a companion viewarms model");
        request.viewhands = companion;
        request.viewhandsStem = companionStem;
        require(!fs::exists(request.modelsDestination / (companionStem + ".cast")),
                "Companion already exists. Change its name or destination.");
    }
    request.discoverParts = profile.is_null() && is_weapon_entry(selected);
    auto currentChoices = choices;
    if (!profile.is_null())
        request.parts = resolve_parts(profile, currentChoices, false);
    if (exportAnimations) {
        if (exportAllClips && is_weapon_entry(selected)) {
            auto identity = weapon_identity(selected.at("name"));
            require(identity.has_value(),
                    "For this category, select a clip and turn off All applicable weapon clips.");
            request.allWeaponClips = true;
        } else {
            require(!selectedClip.is_null(), "Select an animation first");
            request.clips.push_back(selectedClip);
        }
        require(request.allWeaponClips || !request.clips.empty(),
                "No matching clips are loaded. Load the animation tab or select a clip.");
    }
    require(!selected.is_null() &&
                (request.model || request.allWeaponClips || !request.clips.empty()),
            "Select something to export");
    if (request.model) {
        require(!modelsDestination.empty(), "Enter Models destination");
        require(!fs::exists(request.modelsDestination / (request.stem + ".cast")),
                "Model already exists. Change name or destination.");
    }
    if (exportAnimations)
        require(!animationsDestination.empty(), "Enter Animations destination");
    require(!exportStem.empty() && exportStem.find_first_of("\\/:*?\"<>|") == exportStem.npos,
            "Invalid filename");
    savedExportNames[weapon_icon_key(selected.at("name"))]={{"weapon",exportWeapon},{"variant",exportVariant},{"category",exportCategory}};
    write_json(root/"export-names.json",savedExportNames);
    save();
    run("Export assets", [this, request](JobContext &job) {
        auto report = export_assets(request, &job);
        post([this, report] {
            if(report.contains("source"))record_issue(report.at("source"),report);
            status = "Export " + report.at("status").get<std::string>() + " · " +
                     std::to_string(report.at("exportedAnimations").get<int>()) + " animations · " +
                     report.at("reportPath").get<std::string>();
            details = report.dump(2);
        });
    });
    status = tasks.back()->started ? "Export started." : "Export queued.";
}
void Studio::draw_export() {
    if (exportPopup) {
        ImGui::OpenPopup("Export assets");
        exportPopup = false;
    }
    ImGui::SetNextWindowSize(ImVec2(820, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Export assets", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Checkbox("Model", &exportModel);
        observe("Export model toggle");
        dirtySettings|=ImGui::Checkbox("Omit unresolved surfaces on export",&omitUnresolved);
        ImGui::SameLine();
        ImGui::Checkbox("Animations", &exportAnimations);
        observe("Export animations toggle");
        ImGui::TextWrapped("Source rigs: original bones, proportions and placement.");
        const char *tags[] = {"ar",     "smg",     "lmg",     "shotty", "sniper",
                              "pistol", "launch",  "grenade", "melee",  "equipment",
                              "device", "special", "marksman"};
        if (ImGui::BeginCombo("Category", exportCategory.c_str())) {
            for (auto tag : tags)
                if (ImGui::Selectable(tag, exportCategory == tag))
                    exportCategory = tag;
            ImGui::EndCombo();
        }
        ImGui::InputText("Weapon / model name", &exportWeapon);
        observe("Export name");
        if (exportCategory == "sniper" || exportCategory == "marksman") {
            if (ImGui::BeginCombo("Profile action type", sniperArchetype.c_str())) {
                for (auto type : {"semi_sniper", "bolt_sniper", "bolt_individual_sniper"})
                    if (ImGui::Selectable(type, sniperArchetype == type))
                        sniperArchetype = type;
                ImGui::EndCombo();
            }
        }
        ImGui::InputText("Variant", &exportVariant);
        const bool sourceWorld = selected.value("perspective", perspective(selected.at("name"))) == "world";
        if (is_weapon_entry(selected) && !sourceWorld)
            ImGui::Checkbox("Include matching worldmodel (_3P)", &exportWorld);
        else if (is_weapon_entry(selected))
            ImGui::TextUnformatted("Worldmodel (_3P source)");
        auto clean = [](std::string s) {
            return std::regex_replace(s, std::regex("[^a-zA-Z0-9_-]+"), "_");
        };
        std::string kind = selected.value("category", std::string("Weapon"));
        auto defaultStem =
            std::string(kind == "Weapon" ? "" : "codm_") +
            (kind == "Weapon" ? (sourceWorld ? "worldmodel_" : "viewmodel_") + exportCategory + "_"
                              : lower(kind) + "_") +
            clean(exportWeapon) + (exportVariant.empty() ? "" : "_" + clean(exportVariant));
        exportStem = defaultStem;
        if (kind == "Player")
            exportStem = "c_codm_player_" + clean(exportWeapon) + (exportVariant.empty() ? "" : "_" + clean(exportVariant)) + "_fb";
        ImGui::TextWrapped("Output name: %s.cast", exportStem.c_str());
        if (kind == "Weapon" && !sourceWorld && exportWorld && exportModel)
            ImGui::TextUnformatted("Also exports the matching _3P and its parts as worldmodel; animations remain with the viewmodel.");
        auto folder = [&](const char *label, std::string &value) {
            ImGui::PushID(label);
            ImGui::TextUnformatted(label);
            const auto &style = ImGui::GetStyle();
            float browseWidth = ImGui::CalcTextSize("Browse").x + 2 * style.FramePadding.x;
            ImGui::SetNextItemWidth(std::max(100.f, ImGui::GetContentRegionAvail().x - browseWidth -
                                                        style.ItemSpacing.x));
            if (ImGui::InputText("##destination", &value))
                dirtySettings = true;
            observe(label);
            ImGui::SameLine();
            if (ImGui::Button("Browse")) {
                auto chosen = folder_dialog(nullptr, value);
                if (!chosen.empty()) {
                    value = chosen;
                    dirtySettings = true;
                }
            }
            observe(std::string(label) + "/Browse");
            ImGui::PopID();
        };
        if (exportModel) {
            folder("Models destination", modelsDestination);
            ImGui::TextWrapped("%s",
                               pathstr(pathof(modelsDestination) / (exportStem + ".cast")).c_str());
            if (kind == "Weapon" && !sourceWorld) {
                if (ImGui::Checkbox("Include viewarms", &includeViewhands) && includeViewhands &&
                    !loaded[2])
                    load_rows(2);
                if (includeViewhands) {
                    ImGui::InputTextWithHint("##handsSearch", "Find viewarms", &companionSearch);
                    if (ImGui::BeginCombo(
                            "Viewarms model",
                            companion.is_null()
                                ? "Choose a model"
                                : companion.at("name").get_ref<const std::string &>().c_str())) {
                        for (auto &row : models[2])
                            if (!row.contains("error") && search_match(companionSearch, row) &&
                                (showLods || row.value("lod", std::string("0")) == "0")) {
                                ImGui::PushID(row.at("id").get_ref<const std::string &>().c_str());
                                if (ImGui::Selectable(
                                        row.at("name").get_ref<const std::string &>().c_str())) {
                                    companion = row;
                                    companionStem =
                                        "codm_viewhands_" + clean(row.at("name"));
                                }
                                ImGui::PopID();
                            }
                        ImGui::EndCombo();
                    }
                    ImGui::InputText("Viewarms output name", &companionStem);
                    ImGui::TextWrapped(
                        "%s",
                        pathstr(pathof(modelsDestination) / (companionStem + ".cast")).c_str());
                    ImGui::TextDisabled(
                        "Manual pairing. Export viewarms animation separately from its own tab.");
                }
            }
        }
        if (exportAnimations) {
            folder("Animations destination", animationsDestination);
            if (is_weapon_entry(selected))
                ImGui::Checkbox("All applicable weapon clips", &exportAllClips);
            else
                exportAllClips = false;
            if (!exportAllClips)
                ImGui::TextWrapped(
                    "Selected: %s",
                    selectedClip.is_null()
                        ? "None"
                        : selectedClip.at("name").get_ref<const std::string &>().c_str());
            ImGui::TextWrapped(
                "%s",
                pathstr(pathof(animationsDestination) / (exportStem + "_<action>.cast")).c_str());
        }
        if (!exportError.empty()) {
            ImGui::TextColored(ImVec4(1, .45f, .4f, 1), "%s", exportError.c_str());
            observe("Export error");
        }
        if (ImGui::Button("Export", ImVec2(120, 0))) {
            try {
                start_export();
                ImGui::CloseCurrentPopup();
            } catch (const std::exception &e) {
                exportError = e.what();
            }
        }
        observe("Start export");
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        observe("Cancel export");
        ImGui::EndPopup();
    }
}
J studio_checks(const fs::path &directory) {
    fs::create_directories(directory);
    auto scratch =
        directory / ("state-" + hex64(uint64_t(Clock::now().time_since_epoch().count())));
    fs::create_directory(scratch);
    J report = {{"textOnly", false},
                {"destinationsPersistExactly", false},
                {"staleSelectionIgnored", false},
                {"workerLimit", 3}};
    require(!ExportRequest{}.t6, "Export requests must default to source rigs");
    {
        Studio fresh(scratch);
        require(fresh.directoryRequired && fresh.directoryPopup && fresh.gameRoot.empty(), "Fresh startup did not request a CODM directory");
        fresh.directoryDraft = pathstr(scratch / "missing");
        require(!fresh.set_game_directory(false) && !fs::exists(scratch / "settings.ini"), "Invalid directory was saved");
        auto firstFolder = scratch / fs::path(L"CODM folder \u65e5\u672c");
        auto secondFolder = scratch / "another installation";
        fs::create_directory(firstFolder); fs::create_directory(secondFolder);
        fresh.directoryDraft = pathstr(firstFolder);
        require(fresh.set_game_directory(false), fresh.directoryError);
        auto firstDatabase = fresh.database;
        Studio reopened(scratch);
        require(!reopened.directoryRequired && !reopened.directoryPopup && fs::equivalent(pathof(reopened.gameRoot), firstFolder), "Saved Unicode directory did not suppress startup prompt");
        reopened.models[1].push_back({{"id","previous-installation"}});
        reopened.directoryDraft = pathstr(secondFolder);
        require(reopened.set_game_directory(false), reopened.directoryError);
        require(reopened.database != firstDatabase && reopened.models[1].empty(), "Changing installation reused old library rows or database");
        Studio changed(scratch);
        require(fs::equivalent(pathof(changed.gameRoot), secondFolder), "Changed directory did not persist");
        save_game_directory(scratch / "settings.ini", pathstr(scratch / "unavailable"));
        Studio unavailable(scratch);
        require(unavailable.directoryRequired && unavailable.directoryPopup, "Unavailable saved directory was not recoverable");
        report["directoryPromptAndPersistence"] = true;
        report["installationCacheIsolation"] = true;
    }
    write_json(scratch / "studio.json", {{"t6",true},{"t6OptInVersion",1}});
    {Studio migrated(scratch);migrated.save();}
    require(!read_json(scratch / "studio.json").contains("t6"),"Removed rig preference persisted");
    report["sourceRigsOnly"] = true;
    {
        Studio app(scratch); app.textOnly=true;
        J a={{"id","weapon:a"},{"name","MainWeapon_001_Example_1P"},{"category","Weapon"}};
        J b={{"id","weapon:b"},{"name","MainWeapon_002_Other_1P"},{"category","Weapon"}};
        app.select(a); app.profile={{"entryId","weapon:a"},{"slots",J::array()}};
        app.choices={{"sto","stock:2"},{"mag",nullptr}}; app.overrides={{"surface","material:1"}};
        app.select(b); app.profile={{"entryId","weapon:b"},{"slots",J::array()}}; app.choices={{"sto","stock:3"}};
        app.select(a);
        require(app.choices.at("sto")=="stock:2" && app.choices.at("mag").is_null() && app.overrides.at("surface")=="material:1", "Selection lost custom parts, explicit None, or material overrides");
        app.batchPlan={{"entries",J::array({a,b})}};
        auto plan=app.configured_batch_plan(); app.choices["sto"]="stock:4";
        require(plan["entries"][0]["configuration"]["choices"]["sto"]=="stock:2" && plan["entries"][1]["configuration"]["choices"]["sto"]=="stock:3", "Queued batch did not retain independent configuration snapshots");
        report["configuredWeaponSelectionAndBatch"] = true;
    }
    {
        Studio app(scratch);
        app.textOnly = true;
        J first = {
            {"id", "index-only:1"}, {"name", "MainWeapon_080_AK117_1P"}, {"category", "Weapon"}};
        app.select(first);
        require(!app.preview && !app.materials && app.tasks.empty(),
                "Text-only selection prepared source assets");
        report["textOnly"] = true;
        auto old = app.revision;
        app.post([&app, old] {
            if (app.revision == old)
                app.status = "stale-result";
        });
        first["id"] = "index-only:2";
        app.select(first);
        app.drain();
        require(app.status != "stale-result", "Stale completion replaced the selection");
        report["staleSelectionIgnored"] = true;
        J shared = J::array({{{"id", "shared:1"},
                              {"name", "MainWeapon_011_AK47_1P_M_Idle"},
                              {"category", "Weapon"}}});
        J evidence = {{"added", J::array({"shared:1"})},
                      {"declarations", {{"status", "declared"}}}};
        app.publish_shared_animations(old, shared, evidence);
        require(app.inheritedClips.empty() && !app.selected.contains("declaredAnimationIDs"),
                "Stale controller discovery changed a different model");
        app.publish_shared_animations(app.revision, shared, evidence);
        app.publish_shared_animations(app.revision, shared, evidence);
        require(app.inheritedClips.size() == 1 && app.clips[1].size() == 1 &&
                    app.selected["declaredAnimationIDs"] == evidence["added"] && app.tasks.empty(),
                "Shared discovery duplicated rows or started geometry work");
        report["sharedAnimationPublication"] = true;
        app.select({{"id", "index-only:3"},
                    {"name", "C_F_Charly_Sinister_1P"},
                    {"category", "Viewhands"}});
        app.selectedClip = {{"id", "index-only:4"}, {"name", "ADV1P_BOCW_M_Sprint"}};
        require(app.inheritedClips.empty() && !app.selected.contains("declaredAnimationIDs"),
                "Shared animation ownership survived a model change");
        app.begin_export();
        require(app.exportAnimations && !app.exportAllClips && app.tasks.empty(),
                "Viewarms export defaults to a weapon set or starts preparation");
        report["viewhandsExportUsesSelectedClip"] = true;
        app.modelsDestination = "Z:\\unfinished path ";
        app.animationsDestination = "Y:\\separate parent\\animations\\";
        app.dirtySettings = true;
        app.save();
        Studio reloaded(scratch);
        require(reloaded.modelsDestination == app.modelsDestination &&
                    reloaded.animationsDestination == app.animationsDestination,
                "Destination input was changed during persistence");
        report["destinationsPersistExactly"] = true;
        require(app.autoDepth && app.zoomToMouse,"Auto Depth navigation defaults changed");
        app.autoDepth=false;app.zoomToMouse=false;app.emulateMiddle=true;app.orbitSpeed=.012f;app.dirtySettings=true;app.save();
        Studio navigationReloaded(scratch);
        require(!navigationReloaded.autoDepth && !navigationReloaded.zoomToMouse && navigationReloaded.emulateMiddle && std::abs(navigationReloaded.orbitSpeed-.012f)<1e-6f,"Navigation preferences did not persist");
        report["navigationPreferencesPersist"]=true;
    }
    remove_owned_tree(scratch, directory);
    report["status"] = "passed";
    return report;
}
struct StudioInputCheck {
    struct Item {
        ImVec2 min, max, windowMin, windowSize;
        bool visible = false;
        bool hovered = false, active = false;
    };
    Studio &app;
    std::map<std::string, Item> items;
    std::deque<std::function<bool()>> steps;
    J passed = J::array();
    J measurements = J::object();
    fs::path snapshot;
    std::string scope = "Actual Dear ImGui widgets driven through ImGuiIO mouse/key events in a "
                        "hidden native window, with synthetic index rows and isolated settings. "
                        "No desktop input or game files used.";
    int stepNumber = 0;
    std::string modelPath = "X:\\model folder\\unfinished ";
    std::string animationPath = "Y:\\different parent\\animations\\";
    explicit StudioInputCheck(Studio &studio, bool synthetic = true) : app(studio) {
        app.directoryRequired = false; app.directoryPopup = false;
        app.textOnly = true;
        app.applicableOnly = false;
        app.loaded.fill(true);
        app.observeItem = [this](const std::string &name) {
            items[name] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetWindowPos(),
                           ImGui::GetWindowSize(),  ImGui::IsItemVisible(),  ImGui::IsItemHovered(),
                           ImGui::IsItemActive()};
        };
        if (!synthetic)
            return;
        for (int i = 0; i < 4; i++) {
            std::string name = i == 1 ? "MainWeapon_001_Example_1P" : "Example_" + categories[i];
            app.models[i] = {{{"id", "ui-model:" + std::to_string(i)},
                              {"name", name},
                              {"category", categories[i]},
                              {"complete", true}}};
            app.clips[i] = {{{"id", "ui-clip:" + std::to_string(i)},
                             {"name", name + "_M_Idle"},
                             {"category", categories[i]}}};
        }
        add([] {});
        add([this] { app.directoryRequired = true; app.directoryPopup = true; });
        check("First launch requests CODM directory", [this] { return items.contains("Save CODM directory") && items.at("Save CODM directory").visible; });
        add([this] { app.directoryRequired = false; });
        click("Cancel CODM directory");
        click("CODM directory");
        check("Top-right directory control opens folder settings", [this] { return items.contains("CODM directory input") && items.at("CODM directory input").visible; });
        click("Cancel CODM directory");
        for (int i = 0; i < 4; i++) {
            click("Models/" + categories[i]);
            click("Model row/ui-model:" + std::to_string(i));
            click("Animations/" + categories[i]);
            click("Clip row/ui-clip:" + std::to_string(i));
            check(categories[i] + " model and animation tabs", [this, i] {
                return app.modelTab == i && app.clipTab == i &&
                       app.selected.at("category") == categories[i] &&
                       app.selectedClip.at("category") == categories[i];
            });
        }
        check("Text-only UI selection starts no source work", [this] {
            return !app.preview && !app.materials && !app.animation && app.tasks.empty();
        });
        click("Models/Player");click("Select model/ui-model:0");
        click("Models/Viewhands");click("Select model/ui-model:2");
        click("Models/Weapon");click("Select model/ui-model:1");
        check("Export selections persist independently without loading geometry",[this] {
            return app.checkedModels[0].contains("ui-model:0") && app.checkedModels[1].contains("ui-model:1") && app.checkedModels[2].contains("ui-model:2") && !app.preview && app.tasks.empty();
        });
        click("Viewport preferences");
        check("Auto Depth is enabled by default",[this]{return app.autoDepth && app.zoomToMouse;});
        click("Navigation auto depth");
        check("Navigation preference toggles",[this]{return !app.autoDepth;});
        click("Navigation auto depth");click("Close navigation preferences");
        add([this]{app.yaw=0;app.pitch=0;app.center=V3(0);app.distance=5;auto item=items.at("Viewport navigation area");ImGui::GetIO().AddMousePosEvent((item.min.x+item.max.x)*.5f,(item.min.y+item.max.y)*.5f);});
        add([]{ImGui::GetIO().AddMouseButtonEvent(2,true);});
        add([]{auto &io=ImGui::GetIO();io.AddMousePosEvent(io.MousePos.x+30,io.MousePos.y+20);});
        add([]{ImGui::GetIO().AddMouseButtonEvent(2,false);});
        check("Middle mouse gesture orbits the viewport",[this]{return app.yaw<-.1f && app.pitch>.1f;});
        add([]{ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift,true);ImGui::GetIO().AddMouseButtonEvent(2,true);});
        add([]{auto &io=ImGui::GetIO();io.AddMousePosEvent(io.MousePos.x+30,io.MousePos.y);});
        add([]{ImGui::GetIO().AddMouseButtonEvent(2,false);ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift,false);});
        check("Shift middle mouse pans the viewport",[this]{return glm::length(app.center)>.01f;});
        add([]{ImGui::GetIO().AddMouseWheelEvent(0,1);});
        check("Wheel zoom uses the depth navigation path",[this]{return app.distance<5 && !app.depthSample;});
        for(int tab:{0,2}) {
            click("Models/"+categories[tab]);click("Export all models");
            check("Bulk export modal for "+categories[tab],[this,tab] {return app.batchTab==tab && items.contains("Batch skip existing") && items.contains("Batch skip untextured") && app.tasks.empty();});
            click("Batch skip untextured");
            check("Bulk material policy toggles",[this] {return !app.batchSkipUntextured;});
            click("Batch skip untextured");click("Cancel batch");
        }
        click("Models/Weapon");
        click("Folder animations");
        check("Folder animation export has an independent modal",[this]{return app.batchFolderAnimations && items.contains("Export folder animations");});
        click("Cancel folder animations");
        add([this]{app.batchFolderAnimations=false;app.batchTab=1;app.batchEntries={app.models[1][0]};app.batchPlan={{"entries",app.batchEntries},{"pairs",J::array()},{"issues",J::array()}};app.batchPlanning=false;app.batchPopup=true;});
        click("Batch animations");
        check("Batch weapon animation option toggles without loading assets",[this]{return app.batchAnimations && app.tasks.empty();});
        click("Cancel batch");
        click("Model row/ui-model:1");
        click("Animations/Weapon");
        click("Clip row/ui-clip:1");
        click("Open export");
        check("Both Browse buttons fit inside the modal", [this] {
            for (auto name : {"Models destination/Browse", "Animations destination/Browse"}) {
                const auto &item = items.at(name);
                if (!item.visible || item.max.x > item.windowMin.x + item.windowSize.x -
                                                      ImGui::GetStyle().WindowPadding.x + .1f)
                    return false;
            }
            return true;
        });
        edit("Models destination", modelPath);
        edit("Animations destination", animationPath);
        check("Destination text preserves separate parents and unfinished input", [this] {
            return app.modelsDestination == modelPath && app.animationsDestination == animationPath;
        });
        click("Cancel export");
        check("Cancel closes the modal without source work",
              [this] { return !items.contains("Start export") && app.tasks.empty(); });
        click("Open export");
        check("Reopened destination fields preserve edits", [this] {
            return items.contains("Models destination") &&
                   items.contains("Animations destination") && app.modelsDestination == modelPath &&
                   app.animationsDestination == animationPath;
        });
        check("UI edits persist across an application reload", [this] {
            Studio reloaded(app.root);
            return reloaded.modelsDestination == modelPath &&
                   reloaded.animationsDestination == animationPath;
        });
        edit("Models destination", "");
        click("Start export");
        check("Invalid destination error stays visible inside the modal", [this] {
            return app.exportError == "Enter Models destination" &&
                   items.contains("Start export") && items.contains("Export error") &&
                   items.at("Export error").visible && app.tasks.empty();
        });
        click("Export model toggle");
        check("Animation-only mode shows its own destination", [this] {
            return !items.contains("Models destination") &&
                   items.contains("Animations destination");
        });
        click("Export model toggle");
        click("Export animations toggle");
        check("Model-only mode shows its own destination", [this] {
            return items.contains("Models destination") &&
                   !items.contains("Animations destination");
        });
        click("Export animations toggle");
        auto models = pathstr(app.root / "first parent/models");
        auto animations = pathstr(app.root / "second parent/clips");
        edit("Models destination", models);
        edit("Animations destination", animations);
        add([this] {
            fs::create_directories(pathof(app.modelsDestination));
            std::ofstream existing(pathof(app.modelsDestination) / (app.exportStem + ".cast"),
                                   std::ios::binary);
            existing << "protected UI fixture";
        });
        click("Start export");
        check("Existing model is protected before any job is queued", [this] {
            auto bytes = read_bytes(pathof(app.modelsDestination) / (app.exportStem + ".cast"));
            return app.exportError == "Model already exists. Change name or destination." &&
                   std::string(bytes.begin(), bytes.end()) == "protected UI fixture" &&
                   items.contains("Start export") && app.tasks.empty();
        });
        add([this] {
            // This exact file was created by the preceding test in app.root's
            // unique private fixture directory. No user export is touched.
            fs::remove(pathof(app.modelsDestination) / (app.exportStem + ".cast"));
            for (int i = 0; i < 3; i++)
                app.jobs.enqueue("UI fixture gate " + std::to_string(i), [](JobContext &job) {
                    while (!job.cancel)
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                });
        });
        click("Start export");
        check("Queued export closes its dialog and reports queued status", [this] {
            auto found = std::find_if(app.tasks.begin(), app.tasks.end(),
                                      [](auto &t) { return t->name == "Export assets"; });
            return !items.contains("Start export") && found != app.tasks.end() &&
                   !(*found)->started && app.status == "Export queued.";
        });
        click("Cancel task/Export assets");
        check("Queued cancellation reports completion and creates no exports", [this] {
            return std::none_of(app.tasks.begin(), app.tasks.end(),
                                [](auto &t) { return t->name == "Export assets"; }) &&
                   app.status == "Cancelled: Export assets (queued)" &&
                   fs::is_empty(pathof(app.modelsDestination)) &&
                   !fs::exists(pathof(app.animationsDestination));
        });
        add([this] { app.jobs.shutdown(); });
    }
    virtual ~StudioInputCheck() { app.observeItem = {}; }
    void add(std::function<void()> action) {
        steps.push_back([action = std::move(action)] {
            action();
            return true;
        });
        steps.push_back([] { return true; });
        steps.push_back([] { return true; });
    }
    void wait_until(const std::string &label, std::function<bool()> predicate,
                    double timeout = 120) {
        steps.push_back([this, label, predicate = std::move(predicate), timeout,
                         start = Clock::time_point{}]() mutable {
            if (start == Clock::time_point{})
                start = Clock::now();
            if (predicate()) {
                passed.push_back(label);
                return true;
            }
            require(seconds(start) < timeout, "Timed out: " + label + "; status: " + app.status +
                                                  "; " + app.details.substr(0, 500));
            return false;
        });
        // The predicate observes freshly delivered application state. Let that
        // state pass through layout before using the resulting widget rectangles.
        steps.push_back([] { return true; });
        steps.push_back([] { return true; });
    }
    void click(const std::string &name) {
        for (int phase = 0; phase < 3; phase++)
            add([this, name, phase] {
                const auto &item = items.at(name);
                require(item.visible, "UI target is clipped: " + name);
                auto &io = ImGui::GetIO();
                if (name == "Toggle playback") {
                    auto &trace = measurements["playbackInput"];
                    if (!trace.is_array())
                        trace = J::array();
                    trace.push_back({{"phase", phase},
                                     {"min", {item.min.x, item.min.y}},
                                     {"max", {item.max.x, item.max.y}},
                                     {"hovered", item.hovered},
                                     {"active", item.active},
                                     {"playing", app.playing},
                                     {"mouse", {io.MousePos.x, io.MousePos.y}},
                                     {"down", io.MouseDown[0]}});
                    write_json(app.root / "playback-input.json", trace);
                }
                io.AddMousePosEvent((item.min.x + item.max.x) * .5f,
                                    (item.min.y + item.max.y) * .5f);
                if (phase)
                    io.AddMouseButtonEvent(0, phase == 1);
            });
    }
    void edit(const std::string &name, const std::string &value) {
        click(name);
        add([] {
            auto &io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(ImGuiKey_A, true);
        });
        add([] {
            auto &io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiKey_A, false);
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
        });
        if (value.empty()) {
            add([] { ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true); });
            add([] { ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false); });
        } else
            add([value] { ImGui::GetIO().AddInputCharactersUTF8(value.c_str()); });
    }
    void check(const std::string &label, std::function<bool()> predicate) {
        add([this, label, predicate] {
            require(predicate(),
                    label + "; state=" +
                        J({{"playing", app.playing},
                           {"frame", app.frame},
                           {"animation", bool(app.animation)},
                           {"status", app.status},
                           {"selected", app.selected.is_null() ? J() : app.selected.at("id")},
                           {"clip", app.selectedClip.is_null() ? J() : app.selectedClip.at("id")}})
                            .dump());
            passed.push_back(label);
        });
    }
    void advance() {
        ImGui::GetIO().AddFocusEvent(true);
        ImGui::GetIO().DeltaTime = 1.f / 60;
        if (!steps.empty()) {
            auto action = std::move(steps.front());
            steps.pop_front();
            try {
                if (!action())
                    steps.push_front(std::move(action));
            } catch (const std::exception &e) {
                throw std::runtime_error("UI input step " + std::to_string(stepNumber) + ": " +
                                         e.what());
            }
            stepNumber++;
        }
        items.clear();
    }
};
#include "studio_source_check.inl"
} // namespace codm
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace codm;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int exitCode = 0;
    int argumentCount = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    bool selfCheck = argumentCount >= 3 && std::wstring(arguments[1]) == L"--self-check";
    if (selfCheck) {
        auto output = fs::path(arguments[2]);
        LocalFree(arguments);
        try {
            write_json(output, studio_checks(output.parent_path()));
            CoUninitialize();
            return 0;
        } catch (const std::exception &e) {
            write_json(output, {{"status", "failed"}, {"error", e.what()}});
            CoUninitialize();
            return 1;
        }
    }
    bool viewportBench=argumentCount>=4 && std::wstring(arguments[1])==L"--viewport-bench";
    bool smoke = argumentCount >= 4 && (std::wstring(arguments[1]) == L"--smoke" || viewportBench);
    bool gallerySmoke = argumentCount >= 3 && std::wstring(arguments[1]) == L"--gallery-smoke";
    fs::path galleryImage = gallerySmoke ? fs::path(arguments[2]) : fs::path();
    bool sourceCheck = argumentCount >= 4 && std::wstring(arguments[1]) == L"--source-ui-check";
    fs::path sourcePlan = sourceCheck ? fs::path(arguments[3]) : fs::path();
    bool uiCheck =
        sourceCheck || (argumentCount >= 3 && std::wstring(arguments[1]) == L"--ui-check");
    fs::path uiOutput = uiCheck ? fs::absolute(fs::path(arguments[2])) : fs::path();
    fs::path uiScratch;
    fs::path smokeEntry = smoke ? fs::path(arguments[2]) : fs::path(),
             smokeImage = smoke ? fs::path(arguments[3]) : fs::path();
    fs::path smokeParts = smoke && argumentCount > 4 ? fs::path(arguments[4]) : fs::path();
    std::string smokeClip = smoke && argumentCount > 5 ? pathstr(fs::path(arguments[5])) : "";
    LocalFree(arguments);
    try {
        wchar_t exePath[32768];
        GetModuleFileNameW(nullptr, exePath, 32768);
        auto exeDir = fs::path(exePath).parent_path();
        auto root =
            fs::exists(exeDir.parent_path() / "catalog.json") ? exeDir.parent_path() : exeDir;
        if (uiCheck) {
            fs::create_directories(uiOutput.parent_path());
            uiScratch = uiOutput.parent_path() /
                        ("input-" + hex64(uint64_t(Clock::now().time_since_epoch().count())));
            require(fs::create_directory(uiScratch), "Cannot create isolated UI test settings");
            root = uiScratch;
        }
        Studio app(root);
        require(glfwInit() == GLFW_TRUE, "Could not initialize native windowing");
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
        glfwWindowHint(GLFW_DEPTH_BITS, 24);
        if (smoke || uiCheck || gallerySmoke) {
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        }
        app.window = glfwCreateWindow(1600, 1000, "codm2cast_v14", nullptr, nullptr);
        require(app.window != nullptr, "Could not create OpenGL preview window");
        glfwMakeContextCurrent(app.window);
        glfwSwapInterval(viewportBench ? 0 : 1);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(app.window, true);
        ImGui_ImplOpenGL2_Init();
        std::unique_ptr<StudioInputCheck> inputCheck;
        if (smoke) {
            auto entry = read_json(smokeEntry);
            app.textOnly = true;
            app.select(entry);
            for (int i = 0; i < 4; i++)
                if (categories[i] == entry.value("category", std::string()))
                    app.modelTab = app.clipTab = i;
            app.models[app.modelTab] = {entry};
            app.loaded.fill(true);
            auto previewStart = Clock::now();
            Source source(app.catalog, app.data);
            J parts = J::array();
            if (!smokeParts.empty()) {
                auto p = read_json(smokeParts);
                parts = p.at("selected");
                if (p.contains("slots")) {
                    app.profile = p;
                    app.choices = p.value("choices", J::object());
                }
            }
            app.preview = std::make_shared<Model>(prepare_geometry(source, entry, parts));
            double geometrySeconds = seconds(previewStart);
            app.materials = std::make_shared<MaterialSet>(prepare_materials(
                source, *app.preview, app.database, J::object(), 512, nullptr, true));
            app.preview->report["geometryReadySeconds"] = geometrySeconds;
            app.preview->report["texturesReadySeconds"] = seconds(previewStart);
            app.uploadPending = true;
            app.fit_camera();
            app.status = app.materials->errors.empty() ? "Textured preview ready."
                                                       : "Material errors; see Details";
            if (app.materials->errors.empty() && app.preview->report.contains("materialWarnings"))
                app.status = "Textured preview ready with material fallbacks; see Details.";
            app.details = app.preview->report.dump(2);
            if (!smokeClip.empty()) {
                auto &object = source.object(smokeClip);
                auto clip = decode_clip(source, object);
                app.animation = std::make_shared<Animation>(bind_animation(*app.preview, clip));
                app.animationBounds = animation_preview_bounds(*app.preview, *app.animation);
                app.frame = std::min(30, clip.frames - 1);
                app.fit_camera();
                std::set<uint32_t> paths;
                for (auto &binding : clip.bindings)
                    if (binding.type == 4)
                        paths.insert(binding.path);
                app.selectedClip = {{"id", clip.id},
                                    {"name", clip.name},
                                    {"bundle", object.file->bundle},
                                    {"paths", paths},
                                    {"category", categories[app.clipTab]}};
                app.clips[app.clipTab] = {app.selectedClip};
            }
        } else if (gallerySmoke) {
            app.galleryMode=true; app.storeOpen=true; app.textOnly=true; app.loaded.fill(true);
            app.models[1]=library_rows(app.database,"model","Weapon");
            enrich_discovery(app.models[1],app.weaponReference);
            app.modelTab=1; app.perspectiveFilter=1;
            app.modelSearch="*chopper*";
            app.dirtySettings=false;
        } else if (sourceCheck)
            inputCheck =
                std::make_unique<StudioSourceInputCheck>(app, read_json(sourcePlan), uiOutput);
        else if (uiCheck)
            inputCheck = std::make_unique<StudioInputCheck>(app);
        else if(!gallerySmoke && !app.directoryRequired) {
            if (fs::exists(app.database)) app.load_rows(app.modelTab);
            else app.index(app.modelTab, !fs::exists(app.catalog));
        }
        int renderedFrames = 0;
        std::array<std::vector<double>,3> renderSamples,frameSamples;
        std::shared_ptr<Animation> benchmarkAnimation;
        if(viewportBench) {
            benchmarkAnimation=std::make_shared<Animation>();benchmarkAnimation->frames=2;
            benchmarkAnimation->name="Synthetic two-pose performance fixture";
            std::vector<Pose> pose;for(const auto &b:app.preview->bones) pose.push_back({b.position,b.scale,b.rotation});
            benchmarkAnimation->poses={pose,pose};
            if(!pose.empty()) benchmarkAnimation->poses[1][0].position.x+=.05;
        }
        auto galleryTestStart=Clock::now();
        auto last = Clock::now();
        while (!glfwWindowShouldClose(app.window)) {
            auto frameStarted=Clock::now();
            if(viewportBench) {
                app.animation=renderedFrames<120 ? nullptr : benchmarkAnimation;
                app.frame=renderedFrames<240 ? .25 : double(renderedFrames%120)/120.;
            }
            if(gallerySmoke) require(seconds(galleryTestStart)<90,"Gallery test timed out");
            glfwPollEvents();
            app.drain();
            double dt = seconds(last);
            last = Clock::now();
            if (app.animation && app.playing) {
                app.frame += dt * app.animation->fps;
                if (app.frame >= app.animation->frames - 1) {
                    if (app.animation->looping)
                        app.frame = std::fmod(app.frame, std::max(1, app.animation->frames - 1));
                    else {
                        app.frame = app.animation->frames - 1;
                        app.playing = false;
                    }
                }
            }
            app.upload_textures();
            ImGui_ImplOpenGL2_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            if (inputCheck)
                inputCheck->advance();
            ImGui::NewFrame();
            app.draw();
            ImGui::Render();
            int w, h;
            glfwGetFramebufferSize(app.window, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(.03f, .04f, .06f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
            if(viewportBench) {
                glFinish();
                if(renderedFrames%120>=20) {renderSamples[renderedFrames/120].push_back(app.renderMilliseconds);frameSamples[renderedFrames/120].push_back(seconds(frameStarted)*1000);}
                if(renderedFrames==359) {
                    J metrics;const char *names[]={"static","paused","animated"};
                    for(int phase=0;phase<3;++phase) {
                        auto stats=[](std::vector<double> samples) {std::sort(samples.begin(),samples.end());double sum=0;for(auto v:samples) sum+=v;return J{{"mean",sum/samples.size()},{"median",samples[samples.size()/2]},{"p95",samples[samples.size()*95/100]}};};
                        metrics[names[phase]]={{"renderMilliseconds",stats(renderSamples[phase])},{"frameMilliseconds",stats(frameSamples[phase])}};
                    }
                    metrics["scope"]="Hidden native OpenGL viewport; 100 measured frames per phase after 20 warmup frames. Source geometry with synthetic two-pose motion; frame time includes GPU completion, excludes vsync.";
                    write_json(smokeImage.parent_path()/(smokeImage.stem().string()+"-performance.json"),metrics);
                }
            }
            if (inputCheck && !inputCheck->snapshot.empty()) {
                Image pixels{w, h, Bytes(size_t(w) * h * 4)};
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.pixels.data());
                for (int y = 0; y < h / 2; y++)
                    for (int x = 0; x < w * 4; x++)
                        std::swap(pixels.pixels[size_t(y) * w * 4 + x],
                                  pixels.pixels[size_t(h - y - 1) * w * 4 + x]);
                write_png(inputCheck->snapshot, pixels);
                inputCheck->snapshot.clear();
            }
            if ((smoke && ++renderedFrames == (viewportBench ? 360 : 3)) || (inputCheck && inputCheck->steps.empty()) ||
                (gallerySmoke && app.galleryReady && !app.galleryBusy && !app.galleryTextures.empty())) {
                Image pixels{w, h, Bytes(size_t(w) * h * 4)};
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.pixels.data());
                for (int y = 0; y < h / 2; y++)
                    for (int x = 0; x < w * 4; x++)
                        std::swap(pixels.pixels[size_t(y) * w * 4 + x],
                                  pixels.pixels[size_t(h - y - 1) * w * 4 + x]);
                if (inputCheck) {
                    auto screenshot = uiOutput;
                    screenshot.replace_extension(".png");
                    write_png(screenshot, pixels);
                    write_json(uiOutput, {{"status", "passed"},
                                          {"checks", inputCheck->passed},
                                          {"inputFrames", inputCheck->stepNumber},
                                          {"screenshot", pathstr(screenshot)},
                                          {"measurements", inputCheck->measurements},
                                          {"scope", inputCheck->scope}});
                } else if(gallerySmoke) {
                    write_png(galleryImage,pixels);
                    require(!app.preview && !app.materials,"Gallery browsing prepared a model");
                    require(!app.visibleModels.empty(),"Gallery contains no selectable weapons");
                    auto picked=app.models[1][app.visibleModels.front()];
                    app.select(picked);
                    require(app.selected.at("id")==picked.at("id") && !app.preview,"Gallery selection did not preserve text-only behavior");
                    write_json(galleryImage.parent_path()/"ui-check.json",{{"status","passed"},{"icons",app.galleryTextures.size()},{"browsedWithoutGeometry",true},{"selectionUsesNormalPath",true}});
                } else {
                    write_png(smokeImage, pixels);
                    write_json(smokeImage.parent_path() / (smokeImage.stem().string() + ".json"),
                               app.preview->report);
                }
                glfwSetWindowShouldClose(app.window, GLFW_TRUE);
            }
            glfwSwapBuffers(app.window);
            if(!gallerySmoke && !smoke) app.save();
        }
        app.jobs.shutdown();
        app.drain();
        if(!gallerySmoke && !smoke) app.save();
        inputCheck.reset();
        for (auto &[key, tex] : app.textures)
            glDeleteTextures(1, &tex);
        ImGui_ImplOpenGL2_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(app.window);
        glfwTerminate();
        if (uiCheck && !sourceCheck)
            remove_owned_tree(uiScratch, uiOutput.parent_path());
    } catch (const std::exception &e) {
        if(gallerySmoke)
            write_json(galleryImage.parent_path()/"ui-check.json",{{"status","failed"},{"error",e.what()}});
        else if (uiCheck)
            write_json(uiOutput, {{"status", "failed"}, {"error", e.what()}});
        else if (smoke)
            write_json(smokeImage.parent_path() / (smokeImage.stem().string() + "-error.json"),
                       {{"error", e.what()}});
        else
            MessageBoxA(nullptr, e.what(), "codm2cast_v14", MB_ICONERROR);
        exitCode = 1;
    }
    CoUninitialize();
    return exitCode;
}
