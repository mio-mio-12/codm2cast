#pragma once
#include "model.hpp"
namespace codm {
struct ExportName { std::string weapon, variant, category; };
ExportName detect_export_name(const J &entry, const std::vector<J> &library);
}
