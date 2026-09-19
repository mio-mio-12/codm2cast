#pragma once
#include "animation.hpp"
namespace codm {
// Geometry snapshots are immutable while displayed. Reuse deformed vertices
// until either the snapshot, animation, or sampled time changes.
class PreviewPoseCache {
    std::shared_ptr<const Model> model;
    std::shared_ptr<const Animation> animation;
    double frame=0;
  public:
    std::vector<std::vector<V3>> deformed;
    std::vector<V3> centers;
    bool update(std::shared_ptr<const Model> nextModel,
                std::shared_ptr<const Animation> nextAnimation,double nextFrame);
    const std::vector<V3> &positions(size_t surface) const;
};
}
