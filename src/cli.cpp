#include "animation.hpp"
#include "cast_writer.hpp"
#include "export.hpp"
#include "exported_animations.hpp"
#include "gallery.hpp"
#include "discovery.hpp"
#include "batch_export.hpp"
#include "model_pairs.hpp"
#include "library.hpp"
#include "model.hpp"
#include "rig.hpp"
#include "source.hpp"
#include <iostream>
#include <regex>
using namespace codm;
int main(int argc, char **argv) {
    try {
        auto data = fs::absolute(pathof(argv[0])).parent_path() / "data";
        require(argc >= 2, "Commands: scan <root> <catalog>; inspect <catalog> <asset-id> <json>; "
                           "bundle <path>; self-test");
        std::string cmd = argv[1];
        auto start = Clock::now();
        if(cmd == "folder-animation-plan" || cmd == "folder-animations") {
            require(argc==6,"folder-animation-plan / folder-animations <catalog> <models-folder> <animations-folder> <report-json>");
            ExportRequest options;options.catalog=pathof(argv[2]);options.database=options.catalog.parent_path()/"library.sqlite";options.data=data;options.modelsDestination=pathof(argv[3]);options.animationsDestination=pathof(argv[4]);
            auto report=cmd=="folder-animation-plan"?plan_exported_animations(options):export_folder_animations(options);
            write_json(pathof(argv[5]),report);std::cout<<report.dump(2)<<"\n";
            if(report.value("failed",0) || report.value("unmatched",0))return 1;
            if(report.value("partial",0))return 2;
        } else if(cmd == "pair-models") {
            require(argc==5,"pair-models <catalog> <entries-json> <plan-json>");
            auto entries=read_json(pathof(argv[3])).get<std::vector<J>>();
            std::vector<J> candidates;
            auto reference=read_json(data/"weapon-reference.json");
            for(auto category:{"Player","Viewhands","Weapon"}) {
                auto rows=library_rows(pathof(argv[2]).parent_path()/"library.sqlite","model",category);
                if(std::string(category)=="Weapon")enrich_discovery(rows,reference);
                candidates.insert(candidates.end(),rows.begin(),rows.end());
            }
            enrich_discovery(entries,reference);
            auto plan=plan_model_pairs(entries,candidates,true);write_json(pathof(argv[4]),plan);
            std::cout<<plan.at("entries").size()<<" models, "<<plan.at("pairs").size()<<" pairs, "<<plan.at("issues").size()<<" issues\n";
        } else if(cmd == "export-selected") {
            require(argc==6 || argc==7,"export-selected <catalog> <plan-json> <models-destination> <skip-untextured:0|1> [animations-destination]");
            ExportRequest options;options.catalog=pathof(argv[2]);options.database=options.catalog.parent_path()/"library.sqlite";options.data=data;options.modelsDestination=pathof(argv[4]);
            if(argc==7){options.allWeaponClips=true;options.animationsDestination=pathof(argv[6]);}
            auto report=export_selected_models(options,read_json(pathof(argv[3])),true,std::string(argv[5])=="1");
            std::cout<<report.dump(2)<<"\n";if(report.at("failed").get<int>())return 1;if(report.value("partial",0))return 2;
        } else if(cmd == "batch-models") {
            require(argc==6,"batch-models <catalog> <entries-json> <models-destination> <skip-untextured:0|1>");
            ExportRequest options;options.catalog=pathof(argv[2]);options.database=options.catalog.parent_path()/"library.sqlite";options.data=data;options.modelsDestination=pathof(argv[4]);options.t6=false;
            auto report=export_model_batch(options,read_json(pathof(argv[3])).get<std::vector<J>>(),true,std::string(argv[5])=="1");
            std::cout<<report.dump(2)<<"\n";
        } else if(cmd == "discovery-check") {
            require(argc==4,"discovery-check <database> <report-json>");
            auto rows=library_rows(pathof(argv[2]),"model","Weapon");auto reference=read_json(data/"weapon-reference.json");
            enrich_discovery(rows,reference);auto report=discovery_audit(rows,reference);
            auto begin=Clock::now();size_t hits=0;
            for(int i=0;i<100;++i) {SearchQuery query("ak-47");for(const auto &row:rows) hits+=query.match(row);}
            report["searchMillisecondsPerPass"]=seconds(begin)*10;report["searchHits"]=hits/100;
            write_json(pathof(argv[3]),report);std::cout<<rows.size()<<" rows, "<<report.at("searchMillisecondsPerPass")<<" ms/search\n";
        } else if (cmd == "gallery-check") {
            require(argc==4,"gallery-check <catalog> <output>");
            Source source(pathof(argv[2]),data);
            auto result=weapon_icon_index(source,pathof(argv[2]).parent_path()/"cache/gallery");
            result["coverage"]=J::object();
            for(const auto &entry:library_rows(pathof(argv[2]).parent_path()/"library.sqlite","model","Weapon"))
                if(entry.value("complete",false)) result["coverage"][entry.at("name").get<std::string>()]=result["icons"].contains(weapon_icon_key(entry.at("name")));
            std::unique_ptr<Source> thumbSource; std::map<std::string,Image> atlases;
            for(const auto &key:{"ak47","locusscubasniper","chopperhelldog","725revenge"})
                if(result["icons"].contains(key)) {
                    auto image=weapon_thumbnail(result["icons"].at(key),pathof(argv[2]).parent_path()/"cache/gallery/thumbs",pathof(argv[2]),data,thumbSource,atlases);
                    write_png(pathof(argv[3]).parent_path()/(std::string(key)+".png"),image);
                }
            write_json(pathof(argv[3]),result);
            std::cout<<result["icons"].size()<<" icons; "<<result["errors"].size()<<" errors; "<<result["seconds"]<<" seconds\n";
        } else if (cmd == "self-test") {
            require(wildcard("*ak*", "MainWeapon_AK117") && !wildcard("ak?", "ak117"),
                    "Wildcard failed");
            require(natural_less("Mag2", "Mag10"), "Natural sort failed");
            require(crc32("123456789") == 0xcbf43926, "CRC32 failed");
            std::cout << "Core checks passed\n";
        } else if (cmd == "scan") {
            require(argc == 4, "scan <root> <catalog>");
            auto c = scan_catalog(pathof(argv[2]), pathof(argv[3]));
            std::cout << c["bundles"].size() << " bundles; " << c["errors"].size() << " skipped\n";
        } else if (cmd == "bundle") {
            require(argc == 3, "bundle <path>");
            Bundle b(pathof(argv[2]));
            for (auto &n : b.nodes)
                std::cout << n.name << " " << n.size << " " << n.flags << "\n";
        } else if (cmd == "texture-search" || cmd == "material-search" || cmd == "surface-search") {
            require(argc == 5, "texture-search / material-search / surface-search <catalog> <name-wildcard> <report-json>");
            Source source(pathof(argv[2]), data);
            J matches = J::array(), errors = J::array();
            size_t count = 0;
            for (const auto &bundle : source.catalog.at("bundles")) {
                source.clear();
                std::cout << count << " " << hint(bundle.at("path")) << "\n" << std::flush;
                try {
                    for (const auto &node : bundle.at("nodes")) {
                        auto key = lower(basename(node.at("name")));
                        if ((node.at("flags").get<int>() & 4) && source.nodeBundles.at(key) == bundle.at("path").get<std::string>())
                            source.file(node.at("name"));
                    }
                    for (const auto &[cab, file] : source.files)
                        for (auto &[pid, object] : file->objects)
                            if ((object.cid == (cmd == "material-search" ? 21 : 28) ||
                                 (cmd == "surface-search" && object.cid == 21)) &&
                                wildcard(lower(argv[3]), lower(source.name(object))))
                                matches.push_back({{"id", object.id()}, {"name", source.name(object)}, {"bundle",bundle.at("path")}});
                } catch (const std::exception &e) {
                    errors.push_back({{"bundle",bundle.at("path")},{"error",e.what()}});
                }
                if (++count % 500 == 0)
                    write_json(pathof(argv[4]), {{"matches",matches},{"errors",errors},{"bundles",count},{"complete",false}});
            }
            write_json(pathof(argv[4]), {{"matches",matches},{"errors",errors},{"bundles",count},{"complete",true}});
        } else if (cmd == "animation-set") {
            require(argc == 5, "animation-set <catalog> <entry-json> <report-json>");
            Source source(pathof(argv[2]), data);
            J report;
            report["clips"] = J::array();
            auto clips = weapon_animation_set(source, pathof(argv[2]).parent_path() / "library.sqlite",
                                               read_json(pathof(argv[3])), &report);
            report["clips"] = plan_animation_exports(clips);
            write_json(pathof(argv[4]), report);
            std::cout << clips.size() << " animation slots and source alternatives\n";
        } else if (cmd == "animation-sources") {
            require(argc == 5, "animation-sources <catalog> <entry-json> <report-json>");
            Source source(pathof(argv[2]), data);
            auto report = discover_animation_sources(source, read_json(pathof(argv[3])));
            write_json(pathof(argv[4]), report);
            std::cout << report.at("controllers").size() << " controllers, "
                      << report.at("clips").size() << " declared clips, "
                      << report.at("errors").size() << " unresolved owners\n";
        } else if (cmd == "inspect") {
            require(argc == 5, "inspect <catalog> <asset-id> <json>");
            Source source(pathof(argv[2]), data);
            auto &o = source.object(argv[3]);
            write_json(pathof(argv[4]), source.tree(o));
            std::cout << o.id() << " class " << o.cid << " bytes " << o.size << "\n";
        } else if (cmd == "objects") {
            require(argc == 4, "objects <catalog> <bundle>");
            Source source(pathof(argv[2]), data);
            source.load(argv[3]);
            for (auto &[key, f] : source.files)
                for (auto &[pid, o] : f->objects)
                    std::cout << o.id() << " class " << o.cid << " bytes " << o.size << "\n";
        } else if (cmd == "mesh") {
            require(argc == 5, "mesh <catalog> <asset-id> <json>");
            Source source(pathof(argv[2]), data);
            auto m = decode_mesh(source, source.object(argv[3]));
            write_json(pathof(argv[4]), raw_mesh_json(m));
            std::cout << m.name << " vertices " << m.positions.size() << "\n";
        } else if (cmd == "geometry") {
            require(argc == 5, "geometry <catalog> <entry-json> <report-json>");
            Source source(pathof(argv[2]), data);
            auto m = prepare_geometry(source, read_json(pathof(argv[3])));
            write_json(pathof(argv[4]), m.report);
            std::cout << m.report.dump(2) << "\n";
        } else if (cmd == "material-check") {
            require(argc == 5 || argc == 6,
                    "material-check <catalog> <entry-json> <report-json> [parts-json]");
            J result;
            try {
                Source source(pathof(argv[2]), data);
                auto entry = read_json(pathof(argv[3]));
                auto model = prepare_geometry(source, entry,
                                              argc == 6 ? read_json(pathof(argv[5])).at("selected")
                                                        : J::array());
                auto materials = prepare_materials(source, model,
                                                   pathof(argv[2]).parent_path() / "library.sqlite",
                                                   J::object(), 256, nullptr, true);
                result = model.report;
                result["preparedMaterials"] = materials.materials.size();
                result["preparedMaps"] = J::array();
                for (auto &[id, material] : materials.materials)
                    result["preparedMaps"].push_back({{"material", id},
                                                      {"maps", material.maps.size()},
                                                      {"provenance", material.provenance}});
                result["status"] = materials.errors.empty() ? "ready" : "material errors";
            } catch (const std::exception &e) {
                result = {{"status", "failed"}, {"error", e.what()}};
            }
            write_json(pathof(argv[4]), result);
            std::cout << result.at("status") << "\n";
        } else if (cmd == "parts") {
            require(argc == 5 || argc == 6,
                    "parts <catalog> <entry-json> <profile-json> [donor-entry-json]");
            Source source(pathof(argv[2]), data);
            auto profile = discover_parts(source, read_json(pathof(argv[3])),
                                          pathof(argv[4]).parent_path() / "part-cache", nullptr,
                                          argc == 6 ? read_json(pathof(argv[5])) : J());
            J choices = J::object();
            profile["selected"] = resolve_parts(profile, choices, true);
            profile["choices"] = choices;
            write_json(pathof(argv[4]), profile);
            std::cout << "Part slots " << profile["slots"].size() << ", selected "
                      << profile["selected"].size() << "\n";
        } else if (cmd == "index") {
            require(argc == 5, "index <catalog> <database> <category>");
            Source source(pathof(argv[2]), data);
            auto report = index_library(source, pathof(argv[3]), argv[4]);
            write_json(pathof(argv[3]).parent_path() / (std::string("index-") + argv[4] + ".json"),
                       report);
            std::cout << report.dump(2) << "\n";
        } else if (cmd == "export-model") {
            require(argc >= 6,
                    "export-model <catalog> <entry-json> <destination> <stem> [parts-json]");
            Source source(pathof(argv[2]), data);
            auto startGeometry = Clock::now();
            J parts = argc > 6 ? read_json(pathof(argv[6])).at("selected") : J::array();
            auto model = prepare_geometry(source, read_json(pathof(argv[3])), parts);
            model.report["geometrySeconds"] = seconds(startGeometry);
            std::optional<RigConversion> rig;
            if (argc > 7 && std::string(argv[7]) == "t6")
                rig = convert_rig(model, data);
            auto startMaterials = Clock::now();
            auto mats =
                prepare_materials(source, model, pathof(argv[2]).parent_path() / "library.sqlite");
            model.report["materialSeconds"] = seconds(startMaterials);
            if (rig) {
                *rig = convert_rig(model, data);
                model = std::move(rig->model);
            }
            if (!mats.errors.empty()) {
                write_json(pathof(argv[4]) / (std::string(argv[5]) + "-failure.json"),
                           model.report);
                throw std::runtime_error(mats.errors.dump(2));
            }
            auto result = export_model(model, mats, pathof(argv[4]), argv[5]);
            std::cout << result.dump(2) << "\n";
        } else if (cmd == "export-request") {
            require(argc == 3, "export-request <request-json>");
            auto json = read_json(pathof(argv[2]));
            ExportRequest r;
            r.catalog = pathof(json.at("catalog"));
            r.database =
                pathof(json.value("database", pathstr(r.catalog.parent_path() / "library.sqlite")));
            r.data = data;
            r.entry = json.at("entry");
            r.stem = json.at("stem");
            r.modelsDestination = pathof(json.value("modelsDestination", std::string()));
            r.animationsDestination = pathof(json.value("animationsDestination", std::string()));
            r.model = json.value("model", true);
            r.t6 = json.value("t6", false);
            r.omitUnresolved = json.value("omitUnresolved", true);
            r.parts = json.value("parts", J::array());
            r.overrides = json.value("overrides", J::object());
            r.clips = json.value("clips", J::array());
            r.category = json.value("category", std::string("ar"));
            r.title = json.value("title", r.stem);
            r.sniperArchetype = json.value("sniperArchetype", std::string("semi_sniper"));
            r.discoverParts = json.value("discoverParts", false);
            r.allWeaponClips = json.value("allWeaponClips", false);
            r.includeWorldmodel = json.value("includeWorldmodel", false);
            r.viewhands = json.value("viewhands", J());
            r.viewhandsStem = json.value("viewhandsStem", std::string());
            auto report = export_assets(r);
            std::cout << report.dump(2) << "\n";
            if (report.at("status") != "exported")
                return 2;
        } else if (cmd == "export-set") {
            require(argc >= 8,
                    "export-set <catalog> <entry-json> <models-dir> <animations-dir> <stem> "
                    "<parts-json> [native|t6]");
            ExportRequest r;
            r.catalog = pathof(argv[2]);
            r.database = r.catalog.parent_path() / "library.sqlite";
            r.data = data;
            r.entry = read_json(pathof(argv[3]));
            r.modelsDestination = pathof(argv[4]);
            r.animationsDestination = pathof(argv[5]);
            r.stem = argv[6];
            r.parts = read_json(pathof(argv[7])).at("selected");
            require(argc <= 8 || std::string(argv[8]) == "native" || std::string(argv[8]) == "t6",
                    "Export rig must be native or t6");
            r.t6 = argc > 8 && std::string(argv[8]) == "t6";
            std::smatch categoryMatch;
            if (std::regex_search(r.stem, categoryMatch,
                                  std::regex("^codm_(viewmodel|worldmodel)_([^_]+)_")))
                r.category = categoryMatch[2];
            r.allWeaponClips = true;
            auto result = export_assets(r);
            std::cout << result.at("status") << "; " << result.at("exportedAnimations")
                      << " animations; report " << result.at("reportPath") << "\n";
            if (result.at("status") != "exported")
                return 2;
        } else if (cmd == "motion-reference") {
            require(argc == 8, "motion-reference <catalog> <entry-json> <clip-id> <parts-json> "
                               "<output-json> <export-name>");
            Source source(pathof(argv[2]), data);
            auto native = prepare_geometry(source, read_json(pathof(argv[3])),
                                           read_json(pathof(argv[5])).at("selected"));
            auto clip = decode_clip(source, source.object(argv[4]));
            auto anim = bind_animation(native, clip);
            auto rig = convert_rig(native, data);
            anim = convert_animation_rig(native, rig, anim);
            J samples = J::array();
            for (int f = 0; f < anim.frames; f++) {
                auto worlds = pose_worlds(rig.model, anim.poses[f]);
                J bones = J::object();
                for (size_t b = 0; b < worlds.size(); b++) {
                    J flat = J::array();
                    for (int col = 0; col < 4; col++)
                        for (int row = 0; row < 4; row++)
                            flat.push_back(worlds[b][col][row]);
                    bones[rig.model.bones[b].name] = flat;
                }
                samples.push_back({{"frame", f}, {"bones", bones}});
            }
            write_json(pathof(argv[6]), {{"name", argv[7]}, {"samples", samples}});
        } else if (cmd == "export-animation") {
            require(argc >= 7,
                    "export-animation <catalog> <entry-json> <clip-id> <destination> <stem> "
                    "[parts-json] [t6]");
            Source source(pathof(argv[2]), data);
            auto model =
                prepare_geometry(source, read_json(pathof(argv[3])),
                                 argc > 7 ? read_json(pathof(argv[7])).at("selected") : J::array());
            auto clip = decode_clip(source, source.object(argv[4]));
            auto clipName = lower(clip.name);
            bool camera = clipName.ends_with("_camera") || clipName.ends_with("_camra");
            if (camera)
                model = camera_target(source, clip);
            auto anim = bind_animation(model, clip, camera);
            if (argc > 8 && std::string(argv[8]) == "t6") {
                auto rig = convert_rig(model, data);
                anim = convert_animation_rig(model, rig, anim);
                model = std::move(rig.model);
            }
            auto result = export_animation(model, anim, pathof(argv[5]), argv[6]);
            std::cout << result.at("path") << " frames " << anim.frames << "\n";
        } else if (cmd == "clip-headers") {
            require(argc == 6, "clip-headers <catalog> <database> <category> <report-json>");
            Source source(pathof(argv[2]), data);
            auto rows = library_rows(pathof(argv[3]), "animation", argv[4]);
            std::sort(rows.begin(), rows.end(),
                      [](const J &a, const J &b) { return a.at("bundle") < b.at("bundle"); });
            J found = J::array(), errors = J::array();
            std::string previous;
            size_t checked = 0;
            for (auto &row : rows) {
                try {
                    auto bundle = row.at("bundle").get<std::string>();
                    if (bundle != previous) {
                        source.clear();
                        previous = bundle;
                    }
                    auto &object = source.object(row.at("id"));
                    Reader reader(object.raw(), object.file->big);
                    J header = J::object();
                    for (auto &field : source.types.at(74).children) {
                        if (field.name == "m_MuscleClip")
                            break;
                        header[field.name] = parse_tree(reader, field);
                    }
                    bool explicitCurves = false;
                    for (auto key :
                         {"m_RotationCurves", "m_CompressedRotationCurves", "m_EulerCurves",
                          "m_PositionCurves", "m_ScaleCurves", "m_FloatCurves", "m_PPtrCurves"})
                        explicitCurves |= !header.at(key).empty();
                    if (explicitCurves) {
                        found.push_back(
                            {{"entry", row}, {"header", header}, {"muscleOffset", reader.pos}});
                        std::cout << row.at("name") << " explicit curves\n";
                    }
                    checked++;
                } catch (const std::exception &e) {
                    errors.push_back({{"entry", row}, {"error", e.what()}});
                }
            }
            write_json(pathof(argv[5]),
                       {{"checked", checked}, {"explicit", found}, {"errors", errors}});
            std::cout << checked << " checked; " << found.size() << " explicit; " << errors.size()
                      << " errors\n";
        } else if (cmd == "index-bindings") {
            require(argc == 6, "index-bindings <catalog> <database> <category> <report-json>");
            Source source(pathof(argv[2]), data);
            auto report = index_animation_bindings(source, pathof(argv[3]), argv[4]);
            write_json(pathof(argv[5]), report);
            std::cout << report.dump() << "\n";
        } else if (cmd == "applicable-clips") {
            require(argc == 6, "applicable-clips <database> <entry-json> <category> <report-json>");
            AnimationFilter filter(read_json(pathof(argv[3])));
            J matched = J::array(), unchecked = J::array();
            for (auto &row : library_rows(pathof(argv[2]), "animation", argv[4])) {
                auto match = filter.match(row);
                if (match == AnimationMatch::Applicable)
                    matched.push_back(row);
                else if (match == AnimationMatch::Unchecked)
                    unchecked.push_back(row);
            }
            write_json(pathof(argv[5]), {{"applicable", matched}, {"unchecked", unchecked}});
            std::cout << matched.size() << " applicable; " << unchecked.size() << " unchecked\n";
        } else if (cmd == "clip-validate") {
            require(argc == 5 || argc == 6,
                    "clip-validate <catalog> <headers-json> <report-json> [samples-dir]");
            Source source(pathof(argv[2]), data);
            auto fixtures = read_json(pathof(argv[3])).at("explicit");
            J result = J::array();
            std::string previous;
            for (auto &fixture : fixtures) {
                const auto &entry = fixture.at("entry");
                try {
                    auto bundle = entry.at("bundle").get<std::string>();
                    if (previous != bundle) {
                        source.clear();
                        previous = bundle;
                    }
                    auto clip = decode_clip(source, source.object(entry.at("id")));
                    clip.report["status"] = "decoded";
                    result.push_back(clip.report);
                    if (argc == 6) {
                        J dump = clip.report;
                        dump["columns"] = clip.columns;
                        dump["samples"] = clip.samples;
                        auto sampleName = clip.id;
                        std::replace(sampleName.begin(), sampleName.end(), ':', '_');
                        write_json(pathof(argv[5]) / (sampleName + ".json"), dump);
                    }
                } catch (const std::exception &e) {
                    result.push_back({{"source", entry.at("id")},
                                      {"sourceName", entry.at("name")},
                                      {"status", "failed"},
                                      {"error", e.what()}});
                }
            }
            write_json(pathof(argv[4]), result);
            size_t passed = 0;
            for (auto &row : result)
                passed += row.at("status") == "decoded";
            std::cout << passed << "/" << result.size() << " decoded\n";
        } else if (cmd == "clip") {
            require(argc == 5, "clip <catalog> <clip-id> <report-json>");
            Source source(pathof(argv[2]), data);
            auto clip = decode_clip(source, source.object(argv[3]));
            write_json(pathof(argv[4]), clip.report);
            std::cout << clip.name << " frames " << clip.frames << " columns " << clip.columns
                      << "\n";
        } else if (cmd == "clip-bundle") {
            require(argc == 5, "clip-bundle <catalog> <clip-id-in-bundle> <report-json>");
            Source source(pathof(argv[2]), data);
            auto &file = *source.object(argv[3]).file;
            J results = J::array();
            for (auto &[pid, obj] : file.objects)
                if (obj.cid == 74) {
                    try {
                        auto c = decode_clip(source, obj);
                        c.report["status"] = "decoded";
                        results.push_back(c.report);
                    } catch (const std::exception &e) {
                        results.push_back(
                            {{"source", obj.id()}, {"status", "failed"}, {"error", e.what()}});
                    }
                }
            write_json(pathof(argv[4]), results);
            size_t failed = 0;
            for (auto &r : results)
                failed += r.at("status") == "failed";
            std::cout << results.size() << " clips, " << failed << " failures\n";
        } else if (cmd == "texture-aliases") {
            require(argc == 6, "texture-aliases <catalog> <database> <report-json> <bundle-limit>");
            Source source(pathof(argv[2]), data);
            Database db(pathof(argv[3]));
            int limit = std::stoi(argv[5]);
            require(limit > 0 && limit <= 10000, "Invalid bundle limit");
            auto rows = db.query("SELECT DISTINCT bundle FROM assets WHERE type='model' "
                                 "AND category='Weapon' ORDER BY bundle LIMIT ?",
                                 {std::to_string(limit)});
            std::map<std::string, J> groups;
            std::set<std::string> seen;
            J errors = J::array();
            for (auto &row : rows) {
                source.clear();
                try {
                    source.load(row[0]);
                    std::vector<Object *> textures;
                    for (auto &[name, file] : source.files)
                        for (auto &[id, object] : file->objects)
                            if (object.cid == 28 && seen.insert(object.id()).second)
                                textures.push_back(&object);
                    for (auto *object : textures) {
                        const auto &tree = source.tree(*object);
                        const auto &stream = tree.at("m_StreamData");
                        Bytes payload;
                        if (!stream.at("path").get<std::string>().empty())
                            payload = source.resource(stream.at("path"), stream.at("offset"),
                                                      stream.at("size"));
                        else
                            payload = tree.at("image data").get_binary();
                        auto fingerprint = texture_content_fingerprint(tree, payload);
                        auto key = fingerprint.dump();
                        if (!groups.contains(key))
                            groups[key] = J::array();
                        groups[key].push_back({{"id", object->id()},
                                               {"name", tree.at("m_Name")},
                                               {"bundle", row[0]},
                                               {"fingerprint", fingerprint}});
                    }
                } catch (const std::exception &e) {
                    errors.push_back({{"bundle", row[0]}, {"error", e.what()}});
                }
            }
            J aliases = J::array();
            for (auto &[key, group] : groups) {
                std::set<std::string> names;
                for (auto &texture : group)
                    names.insert(texture.at("name"));
                if (names.size() > 1)
                    aliases.push_back(group);
            }
            write_json(pathof(argv[4]), {{"bundles", rows.size()},
                                         {"textures", seen.size()},
                                         {"renamedDuplicateGroups", aliases},
                                         {"errors", errors}});
            std::cout << seen.size() << " textures; " << aliases.size()
                      << " renamed duplicate groups; " << errors.size() << " bundle errors\n";
        } else if (cmd == "texture") {
            require(argc == 5 || argc == 6, "texture <catalog> <texture-id> <png> [report-json]");
            Source source(pathof(argv[2]), data);
            auto image = decode_texture(source, source.object(argv[3]));
            write_png(pathof(argv[4]), image);
            if (argc == 6)
                write_json(pathof(argv[5]), {{"source", argv[3]},
                                             {"width", image.width},
                                             {"height", image.height},
                                             {"rgbaSHA256", sha256(image.pixels)},
                                             {"dependencies", source.dependency_paths()}});
            std::cout << image.width << " x " << image.height << "\n";
        } else
            throw std::runtime_error("Unknown command: " + cmd);
        std::cout << "Elapsed " << seconds(start) << " s\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
