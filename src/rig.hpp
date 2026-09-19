#pragma once
#include "animation.hpp"
namespace codm {
struct RigConversion {
    Model model;
    std::vector<int> sourceToTarget;
    std::vector<glm::dmat3> fit;
    glm::dmat3 alignment{1};
    DV3 origin{0};
    std::string kind;
    int weaponRoot = 2, weaponMount = 1;
    std::shared_ptr<RigConversion> candidateSource;
    double palmScale = 1;
    bool standaloneViewarms = false;
};
void configure_candidate13(RigConversion &conversion, const Model &native,
                           const Animation &referencePose, const fs::path &dataRoot);
Animation convert_candidate13(const Model &native, const RigConversion &conversion,
                              const Animation &animation, JobContext *job);
std::string translated_bone(const std::string &name);
RigConversion convert_rig(const Model &native, const fs::path &dataRoot,
                          bool fitFirstPerson = true);
Animation convert_animation_rig(const Model &native, const RigConversion &conversion,
                                const Animation &animation, JobContext *job = nullptr);
} // namespace codm
