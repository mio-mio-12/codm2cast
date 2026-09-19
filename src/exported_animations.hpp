#pragma once
#include "export.hpp"
namespace codm {
void use_exported_skeleton(Model &model,const fs::path &path);
J plan_exported_animations(const ExportRequest &options,JobContext *job=nullptr);
J export_folder_animations(const ExportRequest &options,JobContext *job=nullptr);
}
