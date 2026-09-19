#include "preview_pose.hpp"
namespace codm {
bool PreviewPoseCache::update(std::shared_ptr<const Model> nextModel,
                             std::shared_ptr<const Animation> nextAnimation,double nextFrame) {
    if(model==nextModel && animation==nextAnimation && (!nextAnimation || frame==nextFrame)) return false;
    const bool changed=model!=nextModel;
    model=std::move(nextModel);animation=std::move(nextAnimation);frame=nextFrame;
    if(!model) {deformed.clear();centers.clear();return true;}
    if(changed) deformed.clear();
    deformed.resize(model->surfaces.size());centers.resize(model->surfaces.size());
    auto palette=animation ? sample_palette(*model,*animation,frame) : std::vector<M4>();
    for(size_t i=0;i<model->surfaces.size();++i) {
        const auto &surface=model->surfaces[i];auto &vertices=deformed[i];
        if(!palette.empty() && !surface.weights.empty()) {
            vertices.resize(surface.positions.size());
            for(size_t v=0;v<vertices.size();++v) {
                glm::dvec4 position(0);
                for(int k=0;k<4;++k) {
                    double weight=surface.weights[v][k];if(weight<=0) continue;
                    // Preserve the original skinning math and source rig.
                    position+=weight*palette.at(surface.joints[v][k])*glm::dvec4(surface.positions[v],1);
                }
                vertices[v]=V3(position);
            }
        } else vertices.clear();
        V3 center(0);const auto &points=positions(i);for(const auto &point:points) center+=point;
        centers[i]=center/float(std::max(size_t(1),points.size()));
    }
    return true;
}
const std::vector<V3> &PreviewPoseCache::positions(size_t surface) const {
    return deformed.at(surface).empty() ? model->surfaces.at(surface).positions : deformed.at(surface);
}
}
