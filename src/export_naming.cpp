#include "export_naming.hpp"
#include <regex>
#include <sstream>
namespace codm {
ExportName detect_export_name(const J &entry, const std::vector<J> &library) {
    auto name=entry.value("name",std::string());
    auto identity=weapon_identity(name);
    if(entry.value("category",std::string())!="Weapon" || !identity)
        return {name,"","special"};
    // Family recognition is independent of skin names. Longest match keeps
    // overlapping families (AK117/AK47, M4/M4LMG, etc.) distinct.
    static const std::map<std::string,std::string> families=[] {
        std::map<std::string,std::string> out;
        const std::pair<const char*,const char*> groups[]={
            {"ar","ak47 ak117 asm10 an94 m4 type25 bk57 lk24 icr icr1 manowar hbra3 hvk30 kn44 drh peacekeeper fr556 asval cr56amax m13 swordfish kilo141 oden krig6 em2 ffar1 ffar groza type19 bp50"},
            {"smg","qq9 mp5 rus79u aks74u rus pdw57 razorback msmc hg40 pharo gks cordite fennec agr556 qxr pp19bizon mx9 cbr4 p90 ppsh41 mac10 ksp45 switchblade9 x9 lapa ots9 striker45 cx9 tec9 iso uss9"},
            {"lmg","s36 ul736 rpd m4lmg chopper holger26 holger hades pkm dingo mg42 raal"},
            {"shotty","725 by15 hs0405 hs2126 striker krm262 echo r90 jak12 argus"},
            {"sniper","arctic50 dlq33 locus xpr50 m21ebr outlaw na45 rytec zrg20mm hdr tundra lw3tundra"},
            {"marksman","kilo spr208 sks mk2 type63"},
            {"pistol","mw11 j358 50gs renetti shorty crossbow lcar9 dobvra nailgun machinepistol"},
            {"launch","smrs fhj18 thumper d13sector"},
            {"melee","knife katana karambit axe baseballbat prizefighters nunchucks butterflyknife sai spear machete sickle shovel wrench"}
        };
        for(auto [category,names]:groups) { std::istringstream stream(names); std::string word; while(stream>>word) out[word]=category; }
        return out;
    }();
    auto raw=identity->first;
    auto matches=[&](const std::string &base) {
        return raw.starts_with(base) && (raw.size()==base.size() ||
            !(std::isdigit(static_cast<unsigned char>(base.back())) && std::isdigit(static_cast<unsigned char>(raw[base.size()]))));
    };
    std::string base,category="special";
    for(const auto &[candidate,cat]:families)
        if(matches(candidate) && candidate.size()>base.size()) { base=candidate; category=cat; }
    if(entry.contains("weaponAlias")) {base=entry.at("weaponAlias");category=entry.at("weaponCategory");}
    if(base.empty()) {
        // For unrecognized families, use a shorter actual complete model name
        // only when it is a source prefix; never invent a skin translation.
        base=raw;
        for(const auto &row:library) {
            if(!row.value("complete",false) || row.contains("error")) continue;
            auto other=weapon_identity(row.value("name",std::string()));
            if(other && other->first.size()>=3 && other->first.size()<base.size() && matches(other->first)) base=other->first;
        }
    }
    static const std::regex prefix("^(?:(?:MainWeapon|SecondaryWeapon|AssiWeapon)_[0-9]+_|wea[0-9]+_|pov_)",std::regex::icase);
    static const std::regex suffix("_(?:1p|3p|pov|ui)(?:_.*)?$",std::regex::icase);
    auto authored=std::regex_replace(std::regex_replace(name,prefix,""),suffix,"");
    auto variant=authored.size()>=base.size() ? authored.substr(base.size()) : raw.substr(base.size());
    while(!variant.empty() && (variant.front()=='_' || variant.front()=='-' || variant.front()==' ')) variant.erase(variant.begin());
    return {entry.value("weaponFamily",base),variant,category};
}
}
