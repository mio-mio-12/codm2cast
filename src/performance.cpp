#include "export.hpp"
#include "discovery.hpp"
#include <iostream>
using namespace codm;
int main(int argc,char **argv) {
  try {
    require(argc==5,"codm_perf <project> <library|model> <entry-json-or-category> <output-folder>");
    auto root=fs::absolute(pathof(argv[1])),out=fs::absolute(pathof(argv[4]));fs::create_directories(out);
    J report;auto total=Clock::now();
    auto measure=[&](const char *label,auto action) {auto start=Clock::now();action();report[label]=seconds(start);write_json(out/"timings.json",report);std::cout<<label<<": "<<report[label]<<"\n"<<std::flush;};
    if(std::string(argv[2])=="library") {
      std::vector<J> m,a;J reference=read_json(root/"data/weapon-reference.json");
      measure("modelRowsSeconds",[&]{m=library_rows(root/"library.sqlite","model",argv[3]);});
      measure("modelEnrichmentSeconds",[&]{enrich_discovery(m,reference);});
      measure("animationRowsSeconds",[&]{a=library_rows(root/"library.sqlite","animation",argv[3]);});
      measure("animationEnrichmentSeconds",[&]{enrich_discovery(a,J::object());});
      report["models"]=m.size();report["animations"]=a.size();
    } else {
      std::unique_ptr<Source> source;Model model;MaterialSet materials;
      auto entry=read_json(pathof(argv[3]));J parts=entry.value("parts",J::array());if(entry.contains("entry")) entry=entry.at("entry").get<J>();
      measure("sourceSeconds",[&]{source=std::make_unique<Source>(root/"catalog.json",root/"data");});
      measure("geometrySeconds",[&]{model=prepare_geometry(*source,entry,parts);});
      const std::string mode=argv[2];
      const bool preview=mode.starts_with("preview");
      int dimension=preview ? std::stoi(mode.substr(7)) : 0;
      if(preview && is_weapon_entry(entry) && parts.empty()) {
        J profile,choices=J::object();
        measure("partDiscoverySeconds",[&]{profile=discover_parts(*source,entry,root/"cache/parts");});
        parts=resolve_parts(profile,choices,true,nullptr,false);
        if(!parts.empty()) measure("assemblySeconds",[&]{model=prepare_geometry(*source,entry,parts);});
      }
      measure("materialsSeconds",[&]{materials=prepare_materials(*source,model,root/"library.sqlite",J::object(),dimension,nullptr,preview);});
      report["errors"]=materials.errors;report["materials"]=materials.materials.size();
      if(!preview && materials.errors.empty()) measure("writeSeconds",[&]{report["export"]=export_model(model,materials,out,"perf");});
      J details=J::object();
      for(const auto &surface:model.surfaces) if(!surface.materialId.empty() && !details.contains(surface.materialId)) {
        try {auto &m=source->object(surface.materialId);const auto &tree=source->tree(m);auto *shader=source->ref(m,tree.at("m_Shader"));
          details[m.id()]={{"material",tree},{"shader",shader?source->tree(*shader).at("m_ParsedForm").at("m_Name"):J()}, {"shaderId",shader?J(shader->id()):J()}};
        } catch(const std::exception &) {}
      }
      write_json(out/"material-details.json",details);
      for(const auto &[key,mat]:materials.materials) write_json(out/(mat.name+".json"),mat.provenance);
      write_json(out/"model-report.json",model.report);
    }
    report["totalSeconds"]=seconds(total);write_json(out/"timings.json",report);
    return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
