#pragma once
#include "model.hpp"
namespace codm {
std::string supplemental_component(const std::string &meshName,const std::string &family,const std::string &perspective);
std::string supplemental_layer(const std::string &meshName,const std::string &family,const std::string &perspective);
std::vector<Renderer> weapon_supplements(Source &source,Hierarchy &hierarchy,Object &root,
    const J &entry,const J &parts,J &evidence,JobContext *job);
void deduplicate_weapon_supplements(Source &source,const J &entry,
    std::vector<std::pair<Renderer,std::string>> &attachments,J &evidence);
}
