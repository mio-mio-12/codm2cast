#include "model_pairs.hpp"
#include "export_naming.hpp"
#include <regex>
namespace codm {
std::string character_pair_key(std::string name) {
    name=lower(name);
    // Only remove terminal presentation/platform tokens. Preserve all identity,
    // variant, gender and color tokens; never pair merely by a character prefix.
    name=std::regex_replace(name,std::regex("[^a-z0-9_].*$"),"");
    static const std::regex suffix("_(?:1p|3p|ui|br|mp|cn|gl|lod[0-9]+)$");
    for (;;) {auto next=std::regex_replace(name,suffix,"");if(next==name)break;name=next;}
    return name;
}
static std::string pair_key(const J &entry) {
    auto category=entry.value("category",std::string());
    if(category=="Weapon") {
        auto identity=weapon_identity(entry.at("name"));
        return identity ? "Weapon:"+identity->first+":"+identity->second : "";
    }
    return category+":"+character_pair_key(entry.at("name"));
}
J plan_model_pairs(const std::vector<J> &selected,const std::vector<J> &candidates,bool pair) {
    J result={{"entries",J::array()},{"pairs",J::array()},{"issues",J::array()}};
    std::map<std::string,std::vector<const J*>> index;
    for(const auto &row:candidates)
        if(!row.contains("error") && row.value("lod",std::string("0"))=="0" && row.value("complete",false))
            index[pair_key(row)].push_back(&row);
    std::set<std::string> added;
    std::set<std::pair<std::string,std::string>> linked;
    auto add=[&](const J &entry){if(added.insert(entry.at("id")).second) result["entries"].push_back(entry);};
    for(const auto &entry:selected) {
        add(entry);if(!pair)continue;
        auto category=entry.at("category").get<std::string>();std::string target;
        if(category=="Weapon") {
            auto identity=weapon_identity(entry.at("name"));
            if(identity && (identity->second=="1p" || identity->second=="3p"))
                target="Weapon:"+identity->first+":"+(identity->second=="1p"?"3p":"1p");
        } else if(category=="Player" || category=="Viewhands")
            target=(category=="Player"?"Viewhands:":"Player:")+character_pair_key(entry.at("name"));
        const J *best=nullptr;int bestRank=-1;bool ambiguous=false;
        for(const auto *candidate:index[target]) {
            int rank=0;
            if(candidate->value("bundle",std::string())==entry.value("bundle",std::string()))rank+=2;
            if(candidate->at("category")=="Player" && lower(candidate->at("name")).find("_ui")!=std::string::npos)rank+=4;
            if(rank>bestRank){best=candidate;bestRank=rank;ambiguous=false;}
            else if(rank==bestRank && candidate->at("id")!=best->at("id")) ambiguous=true;
        }
        if(!best || ambiguous) {
            result["issues"].push_back({{"source",entry.at("id")},{"name",entry.at("name")},
                {"reason",ambiguous?"Multiple counterparts match this variant; counterpart omitted":"No complete LOD0 counterpart indexed; selected model will still export"}});
        } else {
            add(*best);
            auto a=entry.at("id").get<std::string>(),b=best->at("id").get<std::string>();
            if(linked.insert(std::minmax(a,b)).second)
                result["pairs"].push_back({{"source",entry.at("id")},{"name",entry.at("name")},{"companion",best->at("id")},{"companionName",best->at("name")}});
        }
    }
    return result;
}
J export_selected_models(const ExportRequest &options,const J &plan,bool skipExisting,bool skipUntextured,JobContext *job) {
    std::vector<J> entries=plan.at("entries").get<std::vector<J>>();
    require(!entries.empty(),"No models selected");
    if(entries.front().at("category")!="Weapon") {
        auto report=export_model_batch(options,entries,skipExisting,skipUntextured,job);
        report["pairing"]=plan.at("pairs");report["pairingIssues"]=plan.at("issues");
        write_json(pathof(report.at("reportPath")),report);return report;
    }
    fs::create_directories(options.modelsDestination);
    auto file=options.modelsDestination/("batch_weapons_"+hex64(uint64_t(Clock::now().time_since_epoch().count()))+".json");
    J report={{"items",J::array()},{"exported",0},{"partial",0},{"skipped",0},{"failed",0},{"status","running"},
        {"total",entries.size()},{"pairing",plan.at("pairs")},{"pairingIssues",plan.at("issues")},{"reportPath",pathstr(file)}};
    std::map<std::string,int> names;std::vector<std::string> stems;
    for(const auto &entry:entries) {
        auto name=detect_export_name(entry,entries);auto id=weapon_identity(entry.at("name"));
        std::string stem=(id && id->second=="3p"?"worldmodel_":"viewmodel_")+name.category+"_"+name.weapon;
        if(!name.variant.empty())stem+="_"+name.variant;
        stem=std::regex_replace(stem,std::regex("[^a-zA-Z0-9_-]+"),"_");stems.push_back(stem);++names[lower(stem)];
    }
    for(size_t i=0;i<entries.size();++i) {
        if(job && job->cancel)break;
        const auto &entry=entries[i];auto stem=stems[i];if(names[lower(stem)]>1)stem+="_"+hex64(hash64(entry.at("id").get<std::string>()));
        J item={{"source",entry.at("id")},{"name",entry.at("name")},{"stem",stem}};
        try {
            bool existing=fs::exists(options.modelsDestination/(stem+".cast"));
            auto identity=weapon_identity(entry.at("name"));
            bool animations=options.allWeaponClips && identity && identity->second=="1p";
            if(skipExisting && existing && !animations)item["status"]="skipped";
            else {
                if(job)job->update(float(i)/entries.size(),"Exporting weapon "+std::to_string(i+1)+" / "+std::to_string(entries.size()));
                auto request=options;request.entry=entry;request.stem=stem;request.t6=false;
                request.model=!(skipExisting && existing);request.discoverParts=true;request.includeWorldmodel=false;request.allWeaponClips=animations;request.clips=J::array();
                request.category=detect_export_name(entry,entries).category;
                if(!request.model){
                    request.existingModel=options.modelsDestination/(stem+".cast");
                    auto sidecar=options.modelsDestination/(stem+".json");
                    if(fs::exists(sidecar)){auto saved=read_json(sidecar);require(saved.at("source").at("id")==entry.at("id"),"Existing filename belongs to a different source weapon");request.parts=saved.value("parts",J::array());request.discoverParts=false;}
                }
                auto result=export_assets(request,job);item["result"]=result;
                auto state=result.value("status",std::string());
                item["status"]=state=="exported"?"exported":state=="partial"?"partial":"failed";
            }
        }catch(const std::exception &e){item["status"]=job && job->cancel?"cancelled":"failed";item["error"]=e.what();}
        auto status=item.at("status").get<std::string>();if(status!="cancelled")report[status]=report.at(status).get<int>()+1;
        report["items"].push_back(item);write_json(file,report);
    }
    report["status"]=job && job->cancel?"cancelled":report["failed"].get<int>() || report["partial"].get<int>()?"partial":"complete";write_json(file,report);return report;
}
}
