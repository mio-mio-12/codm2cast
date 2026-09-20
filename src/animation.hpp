#pragma once
#include "model.hpp"
namespace codm {
struct Binding {
    uint32_t path = 0, attribute = 0;
    int type = 0, custom = 0, width = 1;
    int eulerOrder = 4;
    size_t offset = 0;
    std::string pathName;
};
struct Clip {
    std::string id, name;
    double fps = 30;
    int frames = 0, columns = 0;
    bool looping = false;
    std::vector<Binding> bindings;
    std::vector<double> samples;
    J events, report;
    double at(int frame, size_t column) const {
        return samples.at(size_t(frame) * columns + column);
    }
};
struct Pose {
    DV3 position{0}, scale{1};
    Q rotation{1, 0, 0, 0};
};
struct Animation {
    std::string name;
    double fps = 30;
    int frames = 0;
    bool looping = false;
    std::vector<std::vector<Pose>> poses;
    std::set<std::pair<int, int>> animated;
    std::set<int> relativeTranslation;
    J events, report;
};
Clip decode_clip(Source &source, Object &object, JobContext *job = nullptr,
                 bool metadataOnly = false);
J clip_binding_metadata(Source &source, Object &object, JobContext *job = nullptr);
Clip sample_legacy_curves(const J &header, JobContext *job = nullptr);
Animation bind_animation(const Model &model, const Clip &clip, bool cameraTarget = false);
Model camera_target(Source &source, const Clip &clip);
M4 pose_matrix(const Pose &pose);
std::vector<M4> pose_worlds(const Model &model, const std::vector<Pose> &pose);
std::vector<M4> sample_palette(const Model &model, const Animation &anim, double frame);
std::array<DV3, 2> animation_preview_bounds(const Model &model, const Animation &anim,
                                            JobContext *job = nullptr);
std::string action_name(const std::string &sourceName);
std::string animation_action(const J &clip);
std::string camera_base_name(const std::string &sourceName);
J applicable_animations(const Model &model, const std::vector<J> &rows);
} // namespace codm
