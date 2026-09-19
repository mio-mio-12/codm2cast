#pragma once
#include "export_naming.hpp"
namespace codm {
std::string compact_search(std::string_view text);
struct SearchQuery {
    std::string query;
    std::vector<std::string> tokens;
    bool glob=false;
    explicit SearchQuery(const std::string &text);
    bool match(const J &row) const;
};
void enrich_discovery(std::vector<J> &rows,const J &reference);
J discovery_audit(const std::vector<J> &rows,const J &reference);
}
