#include "discovery.hpp"
#include "gallery.hpp"
#include <sstream>
namespace codm {
std::string compact_search(std::string_view text) {
    std::string out;out.reserve(text.size());
    for(unsigned char c:text) if(std::isalnum(c) || c>=128) out+=char(std::tolower(c));
    return out;
}
SearchQuery::SearchQuery(const std::string &text):query(lower(text)),glob(text.find_first_of("*?")!=text.npos) {
    std::istringstream stream(query);std::string token;
    while(stream>>token) { token=compact_search(token);if(!token.empty()) tokens.push_back(std::move(token)); }
}
bool SearchQuery::match(const J &row) const {
    if(query.empty()) return true;
    std::string fallback;
    const auto &text=row.contains("searchText") ? row.at("searchText").get_ref<const std::string &>() : (fallback=lower(row.value("name",std::string())+" "+row.value("id",std::string())));
    if(glob) return wildcard(query,text) || wildcard(query,row.value("name",std::string())) || wildcard(query,row.value("displayName",std::string()));
    std::string compactFallback;
    const auto &compact=row.contains("searchCompact") ? row.at("searchCompact").get_ref<const std::string &>() : (compactFallback=compact_search(text));
    return std::all_of(tokens.begin(),tokens.end(),[&](const auto &token){return compact.find(token)!=compact.npos;});
}
void enrich_discovery(std::vector<J> &rows,const J &reference) {
    // Compute search strings and name mappings once, away from the UI thread.
    std::map<std::string,J> matchedFamilies;
    static const J empty=J::array();
    const auto &weapons=reference.contains("weapons") ? reference.at("weapons") : empty;
    const auto &skins=reference.contains("skins") ? reference.at("skins") : empty;
    for(auto &row:rows) {
        std::string extra;
        if(row.value("type",std::string("model"))=="model" && row.value("category",std::string())=="Weapon") {
            auto identity=weapon_identity(row.at("name"));
            auto key=identity ? compact_search(identity->first) : weapon_icon_key(row.at("name"));
            J family;
            if(matchedFamilies.contains(key)) family=matchedFamilies.at(key);
            else {
                size_t length=0;
                for(const auto &item:weapons)
                    for(const auto &a:item.at("aliases")) {
                        auto alias=a.get<std::string>();
                        if(key.starts_with(alias) && alias.size()>length &&
                            (key.size()==alias.size() || !(std::isdigit(static_cast<unsigned char>(alias.back())) && std::isdigit(static_cast<unsigned char>(key[alias.size()]))))) {
                            family=item;family["matchedAlias"]=alias;length=alias.size();
                        }
                    }
                matchedFamilies[key]=family;
            }
            if(!family.is_null()) {
                row["weaponFamily"]=family.at("id");row["weaponLabel"]=family.at("name");row["weaponCategory"]=family.at("category");
                row["weaponAlias"]=family.at("matchedAlias");
                row["referenceUrl"]=family.at("source");extra=family.at("name").get<std::string>()+" "+family.at("category").get<std::string>();
                for(const auto &alias:family.at("aliases")) extra+=" "+alias.get<std::string>();
                for(const auto &skin:skins)
                    if(skin.at("weapon")==family.at("id") && compact_search(family.at("id").get<std::string>()+key.substr(family.at("matchedAlias").get<std::string>().size()))==skin.at("key").get<std::string>()) {
                        row["skinLabel"]=skin.at("name");row["referenceSeason"]=skin.at("season");extra+=" "+skin.at("name").get<std::string>()+" "+skin.at("season").get<std::string>();
                    }
            } else {
                auto name=detect_export_name(row,{});
                row["weaponFamily"]=name.weapon;row["weaponLabel"]=name.weapon;row["weaponCategory"]=name.category;
                extra=name.weapon+" "+name.variant+" "+name.category;
            }
        }
        auto text=lower(row.value("name",std::string())+" "+row.value("id",std::string())+" "+extra);
        row["searchCompact"]=compact_search(text);row["searchText"]=std::move(text);
    }
}
J discovery_audit(const std::vector<J> &rows,const J &reference) {
    J found=J::object(),missing=J::array(),unknown=J::array(),recovered=J::array();
    for(const auto &row:rows) if(row.value("complete",false) && row.value("lod",std::string("0"))=="0") {
        auto family=row.value("weaponFamily",std::string());
        if(!family.empty()) found[family]=found.value(family,0)+1;
        if(!row.contains("referenceUrl")) unknown.push_back({{"name",row.at("name")},{"id",row.at("id")}});
        if(row.value("completeEvidence",std::string()).starts_with("Authored weapon root")) recovered.push_back({{"name",row.at("name")},{"id",row.at("id")}});
    }
    for(const auto &item:reference.value("weapons",J::array())) if(!found.contains(item.at("id").get<std::string>())) missing.push_back(item);
    return {{"indexedModels",rows.size()},{"families",found},{"referenceNotMatched",missing},{"unclassifiedOrLocalOnly",unknown},{"recoveredLegacyReceivers",recovered},
        {"note","Reference not matched does not prove missing game files: internal aliases, region/build differences and unindexed packages may account for it."}};
}
}
