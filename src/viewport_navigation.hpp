#pragma once
#include "common.hpp"
namespace codm {
inline V3 view_direction(float yaw,float pitch) {return {std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),std::sin(pitch)};}
inline glm::mat3 view_basis(float yaw,float pitch) {
    auto direction=view_direction(yaw,pitch);auto right=glm::normalize(glm::cross(V3(0,0,1),direction));
    return glm::mat3(right,glm::cross(direction,right),direction);
}
inline V3 orbit_center(V3 center,V3 pivot,float oldYaw,float oldPitch,float yaw,float pitch) {
    return pivot+view_basis(yaw,pitch)*glm::transpose(view_basis(oldYaw,oldPitch))*(center-pivot);
}
inline void zoom_view(V3 &center,float &distance,V3 anchor,float factor) {
    auto next=std::clamp(distance*factor,.001f,10000.f);factor=next/distance;
    center=anchor+(center-anchor)*factor;distance=next;
}
}
