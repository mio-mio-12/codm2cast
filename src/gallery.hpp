#pragma once
#include "materials.hpp"
namespace codm {
std::string weapon_icon_key(std::string name);
J weapon_icon_index(Source &source, const fs::path &cache, JobContext *job = nullptr);
Image weapon_thumbnail(const J &icon, const fs::path &cache, const fs::path &catalog,
                       const fs::path &data, std::unique_ptr<Source> &source,
                       std::map<std::string, Image> &atlases);
}
