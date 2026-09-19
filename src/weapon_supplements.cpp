#include "weapon_supplements.hpp"
#include "materials.hpp"
#include <regex>
namespace codm {
std::string supplemental_component(const std::string &meshName,const std::string &family,const std::string &perspective) {
    auto name=lower(meshName),suffix="_"+family+"_"+perspective;
    if(!name.ends_with(suffix))return {};
    name.resize(name.size()-suffix.size());
    std::smatch match;
    static const std::regex pattern("^.+_((?:wea|mag|sto|rai|bar|sig|gri|grip|iro|muz|sco|guide|tri|sra)(?:[0-9]+(?:up)?)?)$");
    return std::regex_match(name,match,pattern)?match[1].str():std::string();
}
std::string supplemental_layer(const std::string &meshName,const std::string &family,const std::string &perspective) {
    auto component=supplemental_component(meshName,family,perspective);
    if(component.empty())return {};
    auto name=lower(meshName);name.resize(name.size()-family.size()-perspective.size()-component.size()-3);
    static const std::regex number("[0-9]+$");
    return std::regex_replace(name,number,"");
}
std::vector<Renderer> weapon_supplements(Source &source,Hierarchy &h,Object &root,
    const J &entry,const J &parts,J &evidence,JobContext *job) {
    evidence=J::array();std::vector<Renderer> out;
    auto identity=weapon_identity(entry.at("name"));
    if(entry.value("category",std::string())!="Weapon" || !identity)return out;
    std::set<std::string> pieces;
    auto addPiece=[&](std::string name){name=lower(name);auto suffix="_"+identity->first+"_"+identity->second;
        if(name.ends_with(suffix))pieces.insert(name.substr(0,name.size()-suffix.size()));};
    for(const auto &row:entry.at("renderers"))addPiece(row.value("name",std::string()));
    for(const auto &part:parts)addPiece(part.at("name"));
    std::map<std::string,std::vector<Object*>> targets;std::set<std::string> visited;
    std::function<void(Object&)> visit=[&](Object &t){
        require(visited.insert(t.id()).second && visited.size()<100000,"Invalid supplemental target hierarchy");
        targets[h.name(t)].push_back(&t);
        for(const auto &ptr:source.tree(t).at("m_Children")){auto *child=source.ref(t,ptr);require(child && child->cid==4,"Missing supplemental target transform");visit(*child);}
    };visit(root);
    struct Candidate {Renderer renderer;size_t drivers=0;double bindError=0;J report;};
    std::map<std::string,Candidate> selected;
    for(auto &[pid,object]:root.file->objects) {
        if(object.cid!=137)continue;if(job)job->check();
        bool relevant=false;
        try {
            auto r=renderer_info(source,object);auto component=supplemental_component(source.name(*r.mesh),identity->first,identity->second);
            if(component.empty())continue;
            if(!pieces.contains(component)) {
                std::vector<std::string> matching;
                if(component.find_first_of("0123456789")==std::string::npos)
                    for(const auto &piece:pieces)if(piece.starts_with(component) && piece.size()>component.size() && std::isdigit(static_cast<unsigned char>(piece[component.size()])))matching.push_back(piece);
                if(matching.size()!=1)continue;
                component=matching[0];
            }
            auto &otherRoot=h.root(*r.transform);
            if(otherRoot.id()==root.id())continue;
            // Independently authored supplemental prefabs must name this exact
            // family/perspective, and agree with the selected rig in bind space.
            auto rootName=lower(h.name(otherRoot));auto suffix="_"+identity->first+"_"+identity->second;
            if(!rootName.ends_with(suffix))continue;
            relevant=true;
            auto raw=decode_mesh(source,*r.mesh);std::set<uint32_t> weighted;
            for(size_t v=0;v<raw.weights.size();++v)for(int k=0;k<4;++k)if(raw.weights[v][k]>0)weighted.insert(raw.joints[v][k]);
            if(weighted.empty())continue;
            J mappings=J::array();bool compatible=true;
            for(auto i:weighted) {
                require(i<r.bones.size(),"Supplement has invalid weighted joint");auto *original=r.bones[i];auto name=h.name(*original);
                const auto &matches=targets[name];
                if(matches.size()!=1 || matrix_error(h.world(original),h.world(matches[0]))>.0001){compatible=false;break;}
                mappings.push_back({{"source",original->id()},{"target",matches[0]->id()},{"name",name}});r.bones[i]=matches[0];
            }
            if(!compatible) {evidence.push_back({{"renderer",object.id()},{"mesh",r.mesh->id()},{"status","unmatched"},{"reason","Weighted bones do not uniquely match this weapon's bind transforms"}});continue;}
            // Unweighted joints never reach the output skin. Keep them mapped
            // where possible so no foreign prefab hierarchy is retained.
            for(size_t i=0;i<r.bones.size();++i)if(!weighted.contains(uint32_t(i)))r.bones[i]=r.bones[*weighted.begin()];
            auto world=h.world(r.transform);double bindError=0;
            for(auto i:weighted)bindError=std::max(bindError,matrix_error(h.world(r.bones[i])*raw.bind.at(i),world));
            // Numbered forms of one supplemental layer are alternatives, not
            // additive geometry. Prefer the form with usable animation drivers.
            auto layer=supplemental_layer(source.name(*r.mesh),identity->first,identity->second);auto key=layer+":"+component;
            Candidate candidate{r,weighted.size(),bindError,{{"renderer",object.id()},{"mesh",r.mesh->id()},{"name",source.name(*r.mesh)},{"component",component},{"layer",layer},{"status","included"},{"bindError",bindError},{"boneMappings",mappings}}};
            auto at=selected.find(key);
            if(at==selected.end() || (candidate.bindError<=.0001 && at->second.bindError>.0001) ||
               ((candidate.bindError<=.0001)==(at->second.bindError<=.0001) && (candidate.drivers>at->second.drivers ||
                (candidate.drivers==at->second.drivers && natural_less(candidate.report.at("name"),at->second.report.at("name"))))))selected.insert_or_assign(key,std::move(candidate));
        }catch(const std::exception &e){if(job && job->cancel)throw;if(relevant)evidence.push_back({{"renderer",object.id()},{"status","unmatched"},{"reason",e.what()}});}
    }
    for(auto &[key,candidate]:selected){
        if(candidate.bindError<=.0001)out.push_back(candidate.renderer);
        else {candidate.report["status"]="unmatched";candidate.report["reason"]="Supplemental mesh bind pose does not match the selected rig";}
        evidence.push_back(candidate.report);
    }
    return out;
}
void deduplicate_weapon_supplements(Source &source,const J &entry,
    std::vector<std::pair<Renderer,std::string>> &attachments,J &evidence) {
    auto identity=weapon_identity(entry.at("name"));
    if(entry.value("category",std::string())!="Weapon" || !identity)return;
    struct Child {size_t index,drivers;std::string name,component,layer;};
    std::map<std::string,Child> chosen;std::map<size_t,std::string> replaced;
    auto componentMatches=[](const std::string &a,const std::string &b){
        return a==b || (a.find_first_of("0123456789")==std::string::npos && b.starts_with(a) && b.size()>a.size() && std::isdigit(static_cast<unsigned char>(b[a.size()])));
    };
    const auto standalone=evidence;
    for(size_t i=0;i<attachments.size();++i) {
        auto &r=attachments[i].first;auto name=source.name(*r.mesh);
        auto component=supplemental_component(name,identity->first,identity->second);
        if(component.empty())continue;
        auto layer=supplemental_layer(name,identity->first,identity->second);
        // A verified animated form in the main weapon supersedes static or
        // animated copies nested inside an attachment prefab.
        for(const auto &item:standalone)if(item.value("status",std::string())=="included" && item.at("layer")==layer && componentMatches(component,item.at("component"))) {
            replaced[i]=item.at("renderer");break;
        }
        if(replaced.contains(i))continue;
        std::set<uint32_t> drivers;
        if(!r.bones.empty()) {
            auto raw=decode_mesh(source,*r.mesh);
            for(size_t v=0;v<raw.weights.size();++v)for(int k=0;k<4;++k)if(raw.weights[v][k]>0)drivers.insert(raw.joints[v][k]);
        }
        auto key=layer+":"+component;auto at=chosen.find(key);
        Child child{i,drivers.size(),name,component,layer};
        if(at==chosen.end())chosen.emplace(key,child);
        else if(child.drivers>at->second.drivers || (child.drivers==at->second.drivers && natural_less(child.name,at->second.name))) {
            replaced[at->second.index]=r.object->id();at->second=child;
        } else replaced[i]=attachments[at->second.index].first.object->id();
    }
    for(const auto &[i,replacement]:replaced) {
        const auto &r=attachments[i].first;
        evidence.push_back({{"renderer",r.object->id()},{"mesh",r.mesh->id()},{"name",source.name(*r.mesh)},{"status","duplicate"},{"replacement",replacement},{"reason","One form per supplemental layer and component"}});
    }
    std::vector<std::pair<Renderer,std::string>> kept;
    for(size_t i=0;i<attachments.size();++i)if(!replaced.contains(i))kept.push_back(std::move(attachments[i]));
    attachments=std::move(kept);
}
}
