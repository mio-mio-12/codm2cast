#include "exported_animations.hpp"
#include "discovery.hpp"
#include <regex>
namespace codm {
void use_exported_skeleton(Model &model,const fs::path &path) {
    auto bytes=read_bytes(path);validate_cast(bytes);Reader r(bytes);r.skip(8);auto roots=r.get<uint32_t>();r.skip(4);
    std::vector<Bone> bones;int models=0,skeletons=0;
    std::function<void(bool)> node=[&](bool inModel){
        auto id=r.get<uint32_t>();r.skip(12);auto np=r.get<uint32_t>(),nc=r.get<uint32_t>();
        if(id==0x6c646f6d){++models;inModel=true;}
        if(inModel && id==0x6c656b73)++skeletons;
        Bone bone;bool isBone=inModel && id==0x656e6f62;
        for(uint32_t i=0;i<np;++i){
            auto t=r.take(2);std::string type;for(auto c:t)if(c)type+=char(c);
            auto len=r.get<uint16_t>();auto count=r.get<uint32_t>();auto n=r.take(len);std::string name(n.begin(),n.end());
            if(type=="s"){auto value=r.cstr();if(isBone && name=="n")bone.name=value;}
            else {
                static const std::map<std::string,size_t> widths={{"b",1},{"h",2},{"i",4},{"l",8},{"f",4},{"d",8},{"2v",8},{"3v",12},{"4v",16}};
                if(isBone && name=="p" && type=="i" && count==1)bone.parent=r.get<int32_t>();
                else if(isBone && (name=="lp" || name=="s") && type=="3v" && count==1){DV3 v;for(int k=0;k<3;++k)v[k]=r.get<float>();if(name=="lp")bone.position=v;else bone.scale=v;}
                else if(isBone && name=="lr" && type=="4v" && count==1){double x=r.get<float>(),y=r.get<float>(),z=r.get<float>(),w=r.get<float>();bone.rotation=Q(w,x,y,z);}
                else r.skip(widths.at(type)*count);
            }
        }
        if(isBone)bones.push_back(std::move(bone));
        for(uint32_t i=0;i<nc;++i)node(inModel);
    };for(uint32_t i=0;i<roots;++i)node(false);
    require(models==1 && skeletons==1 && !bones.empty(),"Expected one exported model skeleton");
    std::map<std::string,const Bone*> source;for(const auto &bone:model.bones)require(source.emplace(bone.name,&bone).second,"Ambiguous source bone name");
    std::set<std::string> names;
    for(size_t i=0;i<bones.size();++i){auto &bone=bones[i];require(names.insert(bone.name).second,"Duplicate exported bone name");
        require(bone.parent>=-1 && bone.parent<int(i),"Invalid exported skeleton hierarchy");
        require(source.contains(bone.name),"Exported bone is absent from current source rig: "+bone.name);
        bone.paths=source.at(bone.name)->paths;bone.id=source.at(bone.name)->id;
        bone.sourceBindAlias=source.at(bone.name)->sourceBindAlias;
        auto local=glm::translate(M4(1),bone.position)*glm::mat4_cast(bone.rotation)*glm::scale(M4(1),bone.scale);
        bone.world=bone.parent<0?local:bones[bone.parent].world*local;bone.inverseBind=glm::inverse(bone.world);
    }
    model.bones=std::move(bones);model.surfaces.clear();model.report["existingModel"]=pathstr(path);model.report["existingModelSHA256"]=sha256(bytes);
}
J plan_exported_animations(const ExportRequest &options,JobContext *job) {
    require(fs::is_directory(options.modelsDestination),"Choose an existing exported Models folder");
    auto rows=library_rows(options.database,"model","Weapon");
    enrich_discovery(rows,read_json(options.data/"weapon-reference.json"));
    std::map<std::string,J> ids;std::map<std::string,std::vector<J>> byName;
    auto canonical=[](std::string s){s=lower(s);if(s.starts_with("codm_"))s.erase(0,5);if(s.ends_with("_default"))s.resize(s.size()-8);return s;};
    for(const auto &row:rows){
        if(!row.value("complete",false) || row.value("lod",std::string("0"))!="0")continue;
        ids[row.at("id")]=row;auto id=weapon_identity(row.at("name"));if(!id || id->second!="1p")continue;
        auto name=detect_export_name(row,rows);auto stem="viewmodel_"+name.category+"_"+name.weapon+(name.variant.empty()?"":"_"+name.variant);
        byName[canonical(stem)].push_back(row);
    }
    J plan={{"items",J::array()},{"matched",0},{"unmatched",0},{"modelsFolder",pathstr(options.modelsDestination)}};
    for(const auto &file:fs::recursive_directory_iterator(options.modelsDestination,fs::directory_options::skip_permission_denied)) {
        if(job)job->check();if(!file.is_regular_file() || lower(pathstr(file.path().extension()))!=".cast")continue;
        auto stem=pathstr(file.path().stem());auto name=canonical(stem);if(!name.starts_with("viewmodel_"))continue;
        J item={{"stem",stem},{"modelPath",pathstr(file.path())},{"status","unmatched"}};
        try {
            auto sidecar=file.path();sidecar.replace_extension(".json");J entry=nullptr,parts=nullptr;
            if(fs::exists(sidecar)) {
                auto report=read_json(sidecar);
                if(report.contains("source") && report["source"].is_object() && report["source"].contains("id")) {
                    auto id=report["source"]["id"].get<std::string>();require(ids.contains(id),"Saved source model is not in the current library");
                    entry=ids.at(id);parts=report.value("parts",J::array());item["matchEvidence"]="Saved export source ID";
                }
            }
            if(entry.is_null()) {
                auto candidates=byName[name];require(candidates.size()==1,candidates.empty()?"No matching weapon export name in library":"Ambiguous weapon export name");
                entry=candidates.front();item["matchEvidence"]="Unique generated export name";
            }
            auto identity=weapon_identity(entry.at("name"));require(identity && identity->second=="1p","Saved export is not a weapon viewmodel");
            item["entry"]=entry;item["parts"]=parts;item["status"]="matched";
        }catch(const std::exception &e){item["error"]=e.what();}
        auto key=item["status"]=="matched"?"matched":"unmatched";plan[key]=plan[key].get<int>()+1;plan["items"].push_back(std::move(item));
    }
    std::sort(plan["items"].begin(),plan["items"].end(),[](const J&a,const J&b){return a.at("modelPath").get<std::string>()<b.at("modelPath").get<std::string>();});
    return plan;
}
J export_folder_animations(const ExportRequest &options,JobContext *job) {
    require(!options.animationsDestination.empty(),"Choose an Animations destination");
    auto report=plan_exported_animations(options,job);require(!report["items"].empty(),"No weapon viewmodel CAST files found in Models folder");
    auto file=options.animationsDestination/("folder_animations_"+hex64(uint64_t(Clock::now().time_since_epoch().count()))+".json");
    report["reportPath"]=pathstr(file);report["exported"]=0;report["partial"]=0;report["verifiedModels"]=0;report["failed"]=0;report["processed"]=0;report["status"]="running";
    std::map<std::string,int> names;for(const auto &item:report["items"])++names[lower(item.at("stem"))];
    for(auto &item:report["items"]) {
        if(job && job->cancel)break;
        try {
            require(item.at("status")=="matched",item.value("error",std::string("Unmatched model")));
            require(names[lower(item.at("stem"))]==1,"Duplicate model filename in subfolders; choose a narrower Models folder");
            auto request=options;request.entry=item.at("entry");request.stem=item.at("stem");request.model=false;request.t6=false;request.allWeaponClips=true;request.includeWorldmodel=false;
            request.existingModel=pathof(item.at("modelPath"));request.discoverParts=item.at("parts").is_null();request.parts=request.discoverParts?J::array():item.at("parts");
            auto result=export_assets(request,job);item["result"]=result;
            size_t playable=0;
            for(const auto &clip:result.at("clips"))if((clip.value("status",std::string())=="exported" || clip.value("status",std::string())=="existing") && !clip.value("action",std::string()).ends_with("_camera"))++playable;
            item["verifiedWeaponAnimations"]=playable;
            item["status"]=playable?(result.value("status",std::string())=="exported"?"exported":"partial"):"failed";
            if(playable)report["verifiedModels"]=report["verifiedModels"].get<int>()+1;
        }catch(const std::exception &e){item["status"]="failed";item["error"]=e.what();}
        auto state=item.at("status").get<std::string>();report[state]=report[state].get<int>()+1;report["processed"]=report["processed"].get<int>()+1;write_json(file,report);
    }
    report["status"]=job && job->cancel?"cancelled":report["failed"].get<int>() || report["partial"].get<int>()?"partial":"complete";
    report["coveragePercent"]=100.0*report["verifiedModels"].get<int>()/report["items"].size();
    report["coverageMeaning"]="Exported viewmodels with at least one non-camera animation bound to the existing CAST skeleton. Partial sets retain individual clip failures and omitted bindings.";
    write_json(file,report);return report;
}
}
