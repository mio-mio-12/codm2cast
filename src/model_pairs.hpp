#pragma once
#include "batch_export.hpp"
namespace codm {
std::string character_pair_key(std::string name);
J plan_model_pairs(const std::vector<J> &selected, const std::vector<J> &candidates, bool pair);
J export_selected_models(const ExportRequest &options, const J &plan,
                         bool skipExisting, bool skipUntextured, JobContext *job=nullptr);
}
