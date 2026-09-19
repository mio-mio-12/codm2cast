#include "batch_export.hpp"
#include <regex>
namespace codm {
J export_model_batch(const ExportRequest &options,const std::vector<J> &entries,
                     bool skipExisting,bool skipUntextured,JobContext *job) {
    require(!options.modelsDestination.empty(),"Choose a models destination");
    fs::create_directories(options.modelsDestination);
    auto reportPath=options.modelsDestination/("batch_"+hex64(uint64_t(Clock::now().time_since_epoch().count()))+".json");
    J report={{"status","running"},{"total",entries.size()},{"exported",0},{"skipped",0},{"failed",0},
        {"items",J::array()},{"reportPath",pathstr(reportPath)}};
    Source source(options.catalog,options.data);
    std::map<std::string,int> names;
    auto stemFor=[](const J &entry) {
        auto name=std::regex_replace(entry.at("name").get<std::string>(),std::regex("[^a-zA-Z0-9_-]+"),"_");
        return entry.at("category")=="Player" ? "c_codm_player_"+name+"_fb" : "codm_viewhands_"+name;
    };
    for(const auto &entry:entries) ++names[lower(stemFor(entry))];
    size_t at=0;
    for(const auto &entry:entries) {
        if(job && job->cancel) break;
        source.clear();
        std::string stem=stemFor(entry);
        if(names[lower(stem)]>1) stem+="_"+hex64(hash64(entry.at("id").get<std::string>()));
        J item={{"source",entry.at("id")},{"name",entry.at("name")},{"stem",stem}};
        try {
            require(entry.at("category")=="Player" || entry.at("category")=="Viewhands","Bulk export supports players and viewhands");
            if(job) {job->set_stage("Model "+std::to_string(at+1)+"/"+std::to_string(entries.size())+" | "+entry.at("name").get<std::string>());job->update(0,"Preparing model");}
            if(skipExisting && fs::exists(options.modelsDestination/(stem+".cast"))) {
                item["status"]="skipped";item["reason"]="Existing CAST preserved";
            } else {
                auto model=prepare_geometry(source,entry,J::array(),job);
                auto materials=prepare_materials(source,model,options.database,J::object(),0,job);
                item["materialWarnings"]=model.report.value("materialWarnings",J::array());
                item["materialErrors"]=materials.errors;
                if(options.omitUnresolved) {
                    omit_unresolved_surfaces(model,materials);
                    item["omittedSurfaces"]=model.report.value("omittedSurfaces",J::array());
                }
                if(!materials.errors.empty() && skipUntextured) {
                    item["status"]="skipped"; item["reason"]="Unresolved material surfaces";item["materialErrors"]=materials.errors;
                } else {
                    if(!materials.errors.empty()) {
                        item["materialErrors"]=materials.errors;
                        for(auto &surface:model.surfaces) if(!materials.materials.contains(surface.materialId)) {
                            Material neutral;neutral.id="missing_"+hex64(hash64(surface.meshId+":"+std::to_string(surface.submesh)));
                            neutral.name=neutral.id;neutral.maps["albedo"]={1,1,Bytes{180,180,180,255}};
                            neutral.provenance={{"fallback","Explicitly allowed untextured surface"},{"sourceMesh",surface.meshId}};
                            surface.materialId=neutral.id;materials.materials[neutral.id]=std::move(neutral);
                        }
                        materials.errors=J::array();
                    }
                    item["result"]=export_model(model,materials,options.modelsDestination,stem,job);
                    item["status"]="exported";
                }
            }
        } catch(const std::exception &error) {item["status"]=job && job->cancel ? "cancelled" : "failed"; item["error"]=error.what();}
        auto state=item.at("status").get<std::string>();
        if(state!="cancelled") report[state]=report.at(state).get<int>()+1;
        report["items"].push_back(std::move(item));++at;write_json(reportPath,report);
    }
    report["status"]=job && job->cancel ? "cancelled" : "complete";
    report["processed"]=at;write_json(reportPath,report);return report;
}
}
