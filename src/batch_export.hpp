#pragma once
#include "export.hpp"
namespace codm {
J export_model_batch(const ExportRequest &options,const std::vector<J> &entries,
                     bool skipExisting,bool skipUntextured,JobContext *job=nullptr);
}
