#include "cast_writer.hpp"
#include "export.hpp"
#include "gallery.hpp"
#include "export_naming.hpp"
#include "discovery.hpp"
#include "preview_pose.hpp"
#include "jobs.hpp"
#include "rig.hpp"
#include "uv_bake.hpp"
#include "eye_material.hpp"
#include "hair_coverage.hpp"
#include "model_pairs.hpp"
#include "viewport_navigation.hpp"
#include "effect_alpha.hpp"
#include "weapon_supplements.hpp"
#include "exported_animations.hpp"
#include <condition_variable>
#include <iostream>
using namespace codm;
int main(int argc, char **argv) {
  try {
    require(supplemental_component("Reactive02_rai01_ExampleGold_1P","examplegold","1p")=="rai01","Supplemental variant geometry not recognized");
    require(action_name("MainWeapon_101_Test_1P_M_ChangeClip_CameraStart").ends_with("_camera") && camera_base_name("Test_ChangeClip_CameraStart")=="Test_ChangeClipStart","Camera phase classified as weapon motion");
    require(supplemental_component("Reactive02_Wea_ExampleGold_1P","examplegold","1p")=="wea" && supplemental_layer("Reactive02_rai01_ExampleGold_1P","examplegold","1p")==supplemental_layer("Reactive01_rai01_ExampleGold_1P","examplegold","1p"),"Supplemental alternatives not grouped");
    require(supplemental_component("Reactive02_rai01_ExampleGold_3P","examplegold","1p").empty() && supplemental_component("Reactive02_rai01_ExampleGold_1P","example","1p").empty(),"Supplement recognition crossed perspective or variant");
    {
      auto soft=straight_alpha_from_additive(V3(.01f,.2f,.1f));
      require(std::abs(soft.a-.2f)<1e-6f && glm::length(V3(soft)*soft.a-V3(.01f,.2f,.1f))<1e-6f,"Additive alpha conversion retains a black matte or changes contribution");
      require(straight_alpha_from_additive(V3(0)).a==0,"Black additive background stays opaque");
      Image edge{3,1,Bytes{0,0,0,0,0,255,255,128,0,0,0,0}};bleed_transparent_rgb(edge);
      require(edge.pixels[1]==255 && edge.pixels[9]==255 && edge.pixels[3]==0 && edge.pixels[11]==0,"Transparent border bleed changes coverage or leaves black RGB");
      J state={{"srcBlend",{{"val",5},{"name","<noninit>"}}},{"destBlend",{{"val",1},{"name","<noninit>"}}},{"blendOp",{{"val",0},{"name","<noninit>"}}},{"colMask",{{"val",15},{"name","<noninit>"}}}};
      J parsed={{"m_SubShaders",J::array({{{"m_Passes",J::array({{{"m_State",{{"rtBlend0",state}}}}})}}})}};
      J saved={{"m_Floats",J::array()}};
      require(additive_source_factor(parsed,saved)==5,"Authored additive blend not recognized");
      parsed["m_SubShaders"][0]["m_Passes"][0]["m_State"]["rtBlend0"]["destBlend"]["val"]=10;
      require(additive_source_factor(parsed,saved)==0,"Ordinary alpha blend treated as additive");
    }
    {
      Model m;Surface valid,failed,missing;valid.materialId="good";valid.indices={0,1,2};valid.positions={V3(0),V3(1),V3(2)};
      failed=valid;failed.materialFailed=true;missing.materialId="absent";m.surfaces={valid,failed,missing};
      MaterialSet mats;mats.materials["good"]=Material{};mats.materials["unused"]=Material{};mats.errors=J::array({{{"error","test"}}});m.report["materialErrors"]=mats.errors;
      omit_unresolved_surfaces(m,mats);
      require(m.surfaces.size()==1 && m.report["omittedSurfaces"].size()==2 && mats.materials.size()==1 && mats.errors.empty() && !m.report["materialErrors"].empty(),"Omission removed usable geometry or lost diagnostics");
      m.surfaces={failed};bool rejected=false;try{omit_unresolved_surfaces(m,mats);}catch(const std::exception &){rejected=true;}
      require(rejected,"Fully unresolved model exported empty");
    }
    {
      auto row=[](std::string id,std::string name,std::string category){return J{{"id",id},{"name",name},{"category",category},{"complete",true},{"lod","0"},{"bundle","test"}};};
      auto player=row("p","C_M_Atlas_Arctic_BR_UI","Player"),hands=row("h","C_M_Atlas_Arctic_1P","Viewhands");
      require(character_pair_key("C_F_Fiona_Cammy_Color2_SLQ_BR_UI_CN")=="c_f_fiona_cammy_color2_slq","Character variant identity lost");
      auto pair=plan_model_pairs({player,hands},{player,hands},true);
      require(pair["entries"].size()==2 && pair["pairs"].size()==1 && pair["issues"].empty(),"Bidirectional character pairing duplicates selected companions");
      auto wrong=hands;wrong["name"]="C_M_Atlas_Arctic_Color2_1P";
      require(plan_model_pairs({player},{wrong},true)["issues"].size()==1,"Unrelated character variant paired");
      auto duplicate=hands;duplicate["id"]="h2";
      require(plan_model_pairs({player},{hands,duplicate},true)["entries"].size()==1,"Ambiguous character counterpart silently selected");
      auto lod=hands;lod["lod"]="1";
      require(plan_model_pairs({player},{lod},true)["entries"].size()==1,"Lower LOD counterpart selected");
      auto view=row("v","MainWeapon_005_Arctic50_1P","Weapon"),world=row("w","MainWeapon_005_Arctic50_3P","Weapon");
      require(plan_model_pairs({view,world},{view,world},true)["pairs"].size()==1,"Weapon perspective pairing failed");
      require(plan_model_pairs({player},{hands},false)["entries"].size()==1,"Disabled pairing added companion");
    }
    {
      V3 center(1,2,3),pivot(-2,.5f,1);float yaw=.7f,pitch=.3f,nextYaw=1.3f,nextPitch=-.4f;
      auto moved=orbit_center(center,pivot,yaw,pitch,nextYaw,nextPitch);
      auto before=glm::transpose(view_basis(yaw,pitch))*(pivot-center);
      auto after=glm::transpose(view_basis(nextYaw,nextPitch))*(pivot-moved);
      require(glm::length(before-after)<1e-5f,"Auto Depth orbit moves pivot in camera coordinates");
      require(glm::length(orbit_center(moved,pivot,nextYaw,nextPitch,yaw,pitch)-center)<1e-5f,"Depth orbit roundtrip drift");
      float distance=5;auto offset=(center-pivot)/distance;
      zoom_view(center,distance,pivot,.6f);
      require(glm::length((center-pivot)/distance-offset)<1e-5f && std::abs(distance-3)<1e-5f,"Cursor zoom does not preserve anchor projection");
      zoom_view(center,distance,pivot,1e-8f);
      require(distance>=.001f && glm::length((center-pivot)/distance-offset)<.002f,"Clamped depth zoom jumps away from anchor");
    }
    require(hair_coverage(0,1,.5f,.6f)==0 && hair_coverage(.8f,1,.5f,.6f)==1,
            "Hair opaque/empty coverage is incorrect");
    require(std::abs(hair_coverage(.3f,1,.5f,.6f)-.3f/.601f)<1e-6f,
            "Hair forward-pass strands were discarded");
    {
      Image iris{9,9,Bytes(9*9*4)},sclera=iris;
      for(size_t i=0;i<iris.pixels.size();i+=4) {
        iris.pixels[i]=255;iris.pixels[i+3]=255;
        sclera.pixels[i+1]=255;sclera.pixels[i+3]=255;
      }
      EyeSurfaceParameters parameters;parameters.darkScale=0;
      auto full=bake_eye_surface(iris,sclera,parameters,false);
      auto preview=bake_eye_surface(iris,sclera,parameters,true);
      require(full.maps.at("albedo").pixels==preview.previewBaseColor.pixels,"Eye preview differs from exported color");
      const auto &pixels=full.maps.at("albedo").pixels;size_t center=(4*9+4)*4;
      require(pixels[center]==255 && pixels[center+1]==0 && pixels[0]==0 && pixels[1]==255,"Iris and sclera composition swapped or iris missing");
      require(full.maps.at("specular").pixels[center+3]>full.maps.at("specular").pixels[3],"Cornea/sclera roughness partition lost");
      require(full.maps.size()==3 && preview.maps.size()==1,"Preview generated export-only eye maps");
      parameters.scale=0;bool rejected=false;try {bake_eye_surface(iris,sclera,parameters,false);}catch(const std::exception &){rejected=true;}
      require(rejected,"Invalid eye scale accepted");
    }
    {
      auto model=std::make_shared<Model>();Bone a,b;b.parent=0;b.position=DV3(0,1,0);b.inverseBind=glm::translate(M4(1),DV3(0,-1,0));model->bones={a,b};
      Surface surface;surface.positions={V3(1,2,3),V3(-2,1,0)};surface.weights={V4(.4f,.6f,0,0),V4(0,1,0,0)};surface.joints={glm::uvec4(0,1,0,0),glm::uvec4(0,1,0,0)};model->surfaces.push_back(surface);
      auto animation=std::make_shared<Animation>();animation->frames=2;
      std::vector<Pose> rest={Pose{a.position,a.scale,a.rotation},Pose{b.position,b.scale,b.rotation}};animation->poses={rest,rest};animation->poses[1][1].rotation=glm::angleAxis(.7,DV3(0,0,1));animation->poses[1][0].scale=DV3(1.2,.8,1.1);
      PreviewPoseCache cache;require(cache.update(model,animation,.25),"Initial viewport pose not computed");
      auto palette=sample_palette(*model,*animation,.25);
      for(size_t v=0;v<surface.positions.size();++v) {glm::dvec4 expected(0);for(int k=0;k<4;++k) if(surface.weights[v][k]>0) expected+=double(surface.weights[v][k])*palette.at(surface.joints[v][k])*glm::dvec4(surface.positions[v],1);require(glm::length(cache.positions(0)[v]-V3(expected))<1e-7f,"Cached viewport changed skinning");}
      auto buffer=cache.positions(0).data();require(!cache.update(model,animation,.25) && buffer==cache.positions(0).data(),"Paused viewport recalculated its pose");
      require(cache.update(model,animation,.75),"Scrubbing did not update viewport pose");
      auto other=std::make_shared<Animation>(*animation);other->poses[1][1].position.z+=1;
      require(cache.update(model,other,.75),"Same-time animation change retained stale vertices");
      auto replacement=std::make_shared<Model>(*model);replacement->surfaces[0].positions[0].x+=1;
      require(cache.update(replacement,other,.75),"Replacement model retained stale pose");
      require(cache.update(replacement,nullptr,0) && cache.positions(0).data()==replacement->surfaces[0].positions.data(),"Static viewport copied or deformed source geometry");
      cache.update(nullptr,nullptr,0);require(cache.centers.empty() && cache.deformed.empty(),"Viewport cache retained cleared model buffers");
    }
    {
      J reference={{"weapons",J::array({J{{"id","qq9"},{"name","QQ9"},{"category","smg"},{"aliases",J::array({"qq9","mp5"})},{"source","fixture"}},J{{"id","ak47"},{"name","AK-47"},{"category","ar"},{"aliases",J::array({"ak47"})},{"source","fixture"}}})},{"skins",J::array()}};
      std::vector<J> rows={{{"id","a"},{"name","MainWeapon_1_MP5_BlackGold_1P"},{"category","Weapon"}},{{"id","b"},{"name","MainWeapon_2_AK47_1P"},{"category","Weapon"}},{{"id","c"},{"name","MainWeapon_3_AK470_1P"},{"category","Weapon"}}};
      enrich_discovery(rows,reference);
      require(SearchQuery("qq9 black gold").match(rows[0]) && SearchQuery("AK-47").match(rows[1]) && SearchQuery("*ak47*").match(rows[1]) && !SearchQuery("qq9 red").match(rows[0]),"Normalized / alias search failed");
      require(!rows[2].contains("referenceUrl"),"Weapon reference crossed numeric identity boundary");
      auto naming=detect_export_name(rows[0],{});require(naming.weapon=="qq9" && naming.variant=="BlackGold" && naming.category=="smg","Reference alias naming failed");
      J receiver={{"id","legacy"},{"name","AssiWeapon_1_MW11_3P"},{"category","Weapon"},{"complete",false},{"renderers",J::array({J{{"name","MW11_3P_lod0"},{"mesh","m"}}})}};
      auto promoted=unique_model_rows({receiver});require(promoted[0].at("complete")==true,"Legacy receiver stayed hidden");
      receiver["renderers"][0]["name"]="Mag01_MW11_3P";
      require(unique_model_rows({receiver})[0].at("complete")==false,"Loose attachment promoted to receiver");
    }
    for(const auto &[name,base,variant,category]:std::vector<std::array<std::string,4>>{
        {"MainWeapon_080_AK117_1P","ak117","","ar"},
        {"MainWeapon_080_AK117Kuromaku_1P","ak117","Kuromaku","ar"},
        {"MainWeapon_28605_ChopperHellDog_3P","chopper","HellDog","lmg"},
        {"MainWeapon_005_Arctic50Scuba_1P","arctic50","Scuba","sniper"},
        {"AssiWeapon_087_50GSGirlsFrontline_1P","50gs","GirlsFrontline","pistol"},
        {"MainWeapon_1_M4LMG_1P","m4lmg","","lmg"},
        {"pov_QQ9_BlackGold","qq9","BlackGold","smg"},
        {"MainWeapon_1_UnknownNewGun_1P","unknownnewgun","","special"}}) {
        auto actual=detect_export_name(J{{"name",name},{"category","Weapon"}},{});
        require(actual.weapon==base && actual.variant==variant && actual.category==category,"Export identity failed: "+name);
    }
    require(weapon_icon_key("MainWeapon_28605_ChopperHellDog_1P")==
                weapon_icon_key("MainWeapon_10308067_CHOPPER_HellDog"),"Weapon icon identity mismatch");
    require(weapon_icon_key("MainWeapon_1_AK47_1P")!=weapon_icon_key("MainWeapon_2_AK117_1P"),"Icon identity merged different guns");
    {
      J options=J::array({J{{"id","a"},{"name","Rai_Test42Skin_mtl"}},
                         J{{"id","b"},{"name","Wea_Test42Skin_mtl"}}});
      require(component_material_candidates("Rai01_Test42Skin_1P",options)==std::set<std::string>{"a"},
              "Unnumbered component material was missed");
      require(component_material_candidates("Rai01_Test43Skin_1P",options).empty(),
              "Variant digits were erased by component matching");
      options.push_back(J{{"id","c"},{"name","Rai02_Test42Skin_mtl"}});
      require(component_material_candidates("Rai01_Test42Skin_3P",options).size()==2,
              "Competing component materials were hidden");
      options.push_back(J{{"id","d"},{"name","Rai01_Test42Skin_mtl"}});
      require(component_material_candidates("Rai01_Test42Skin_3P",options)==std::set<std::string>{"d"},
              "Exact component match did not retain priority");
    }
    {
      J view={{"id","v:1"},{"name","MainWeapon_1_Test_1P"},{"category","Weapon"},{"bundle","a"}};
      J world={{"id","w:1"},{"name","MainWeapon_1_Test_3P"},{"category","Weapon"},
          {"bundle","a"},{"perspective","world"},{"lod","0"},{"complete",true},
          {"renderers",J::array({J{{"mesh","mesh:1"}}})}};
      J clone=world; clone["id"]="w:2";
      J lod=world; lod["id"]="w:3"; lod["lod"]="1";
      J variant=world; variant["name"]="MainWeapon_2_TestSkin_3P";
      auto rows=unique_model_rows({world,clone,lod,variant});
      require(rows.size()==3 && rows[0].at("duplicateInstances").size()==1,
              "Model deduplication lost a variant/LOD or retained a duplicate instance");
      require(corresponding_worldmodel(view,rows).at("id")=="w:1", "Incorrect paired worldmodel");
      bool rejected=false;
      try { corresponding_worldmodel(view,{variant,lod}); } catch (const std::exception &) { rejected=true; }
      require(rejected,"World pairing substituted a different variant or LOD");
    }
    {
      auto clip = [](const char *id, const char *name) { return J{{"id",id},{"name",name}}; };
      auto selected = J::array({clip("variant:1","GunSkin_M_Fire"),clip("variant:2","GunSkin_M_Idle")});
      auto base = J::array({clip("base:1","Gun_M_Fire"),clip("base:2","Gun_M_Reload"),
                           clip("base:3","Gun_M_Reload_camera"),clip("base:4","Gun_M_Reload"),
                           clip("base:2","Gun_M_Reload")});
      auto result = fill_base_animation_slots(selected,base,J{{"id","base:model"}});
      require(result.size()==5 && result[0]==selected[0] && result[1]==selected[1],
              "Base fallback replaced a variant action or duplicated a source");
      require(fill_base_animation_slots(result,base,J{{"id","base:model"}})==result,
              "Base fallback added an already filled slot");
      J target={{"name","MainWeapon_123_ExampleSkin_1P"},{"category","Weapon"},{"paths",J::array({42})}};
      J unmarked={{"name","MainWeapon_123_ExampleSkin_M_Fire"},{"paths",J::array({42})}};
      require(animation_match(target,unmarked)==AnimationMatch::Applicable,
              "Exact named motion without perspective lost its indexed target paths");
      unmarked["paths"]=J::array({43});
      require(animation_match(target,unmarked)==AnimationMatch::Unmatched,
              "Perspective-free motion was accepted without matching target paths");
    }
    {
      RawMesh target, donor;
      target.positions.resize(4);
      target.uv0 = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
      target.faces = {{0, 1, 2, 0, 2, 3}};
      target.uvPrecision = .0005;
      donor = target;
      donor.positions.assign(4,
                             V3(100)); // pose/scale do not change a UV binding
      for (auto &uv : donor.uv0)
        uv += V2(.0003f, -.0002f);
      donor.faces[0].insert(donor.faces[0].end(), {0, 1, 3});
      require(material_uv_correspondence(target, 0, donor, 0).has_value(),
              "Quantized UV slot was not recovered from a combined donor");
      donor.uv0[2].x += .02f;
      require(!material_uv_correspondence(target, 0, donor, 0),
              "Different UV topology accepted for material transfer");
      donor = target;
      donor.faces = {{0, 1, 2, 0, 1, 2}};
      require(!material_uv_correspondence(target, 0, donor, 0),
              "One matching triangle concealed a missing target triangle");
    }
    {
      // Engine oracle: Unity 2022.3.62f3 samples of authored (20,30,40)
      // Euler keys with each serialized RotationOrder. These values are
      // independent of the exporter's quaternion implementation.
      const double unity[6][4] = {
          {.0704393461, .296882927, .283114076, .9092553},
          {.244792372, .296882927, .283114076, .8785122},
          {.07043935, .18214798, .3675802, .9092553},
          {.0704393461, .296882927, .367580175, .878512144},
          {.244792357, .18214795, .283114076, .9092553},
          {.244792357, .18214795, .367580175, .8785122}};
      Model m;
      m.report = J::object();
      Bone bone;
      bone.name = "Bone";
      bone.paths = {1};
      m.bones.push_back(bone);
      Clip clip;
      clip.name = "Unity Euler oracle";
      clip.frames = 1;
      clip.columns = 3;
      clip.samples = {20, 30, 40};
      clip.report = J::object();
      Binding b;
      b.path = 1;
      b.type = 4;
      b.attribute = 4;
      b.width = 3;
      for (int order = 0; order < 6; ++order) {
        b.eulerOrder = order;
        clip.bindings = {b};
        auto result = bind_animation(m, clip);
        Q expected(unity[order][3], unity[order][0], unity[order][2],
                   -unity[order][1]);
        require(1 - std::abs(glm::dot(result.poses[0][0].rotation,
                                      glm::normalize(expected))) <
                    1e-12,
                "Euler binding disagrees with Unity rotation order or native "
                "basis");
      }
    }
    {
      Model model;
      model.report = J::object();
      Bone source;
      source.name = "b_Head";
      source.paths = {1};
      model.bones.push_back(source);
      Bone alias;
      alias.name = "b_Head__bind_fixture";
      alias.parent = 0;
      alias.position = DV3(.1, -.2, .3);
      alias.rotation = glm::angleAxis(.3, DV3(0, 1, 0));
      alias.world = glm::translate(M4(1), alias.position) *
                    glm::mat4_cast(alias.rotation);
      alias.inverseBind = glm::inverse(alias.world);
      alias.sourceBindAlias = true;
      model.bones.push_back(alias);
      Clip clip;
      clip.name = "per-mesh inverse bind regression";
      clip.frames = 2;
      clip.columns = 4;
      clip.samples = {0, 0, 0, 1, 0, 0, .70710677f, .70710677f};
      clip.report = J::object();
      Binding binding;
      binding.path = 1;
      binding.type = 4;
      binding.attribute = 2;
      binding.width = 4;
      clip.bindings = {binding};
      auto animation = bind_animation(model, clip);
      M4 meshWorld = glm::translate(M4(1), DV3(.4, .5, -.2));
      M4 sourceInverseBind = alias.inverseBind * meshWorld;
      for (auto &frame : animation.poses) {
        auto parent = pose_matrix(frame[0]);
        auto actual =
            parent * pose_matrix(frame[1]) * alias.inverseBind * meshWorld;
        require(matrix_error(actual, parent * sourceInverseBind) < 1e-12,
                "Bind alias changed the source mesh skinning palette");
      }
      for (int attr : {1, 2, 3})
        require(animation.animated.contains({1, attr}),
                "Bind alias requires identity tracks for every transform "
                "component");
    }
    {
      auto pistol = weapon_identity("AssiWeapon_087_50GSGirlsFrontline_1P");
      require(pistol && pistol->first == "50gsgirlsfrontline" &&
                  pistol->second == "1p",
              "Pistol identity must match its component family");
      for (auto [name, slot] : std::vector<std::pair<std::string, std::string>>{
               {"Grip02_50GS_1P", "gri"},
               {"Guide01_50GS_1P", "guide"},
               {"Tri01_50GSGirlsFrontline_1P", "tri"},
               {"Sra02_50GSGirlsFrontline_1P", "sra"},
               {"Sig01UP_Front_50GSLL_1P", "sig_front"}}) {
        auto part = part_identity(name);
        require(part && (*part)[0] == slot && (*part)[2] == "1p",
                "Pistol component type or perspective lost: " + name);
      }
    }
    {
      Model source;
      for (int i = 0; i < 4; i++) {
        Bone bone;
        bone.name = std::to_string(i);
        bone.parent = i - 1;
        bone.position = i ? DV3(1, 0, 0) : DV3(0);
        bone.world = glm::translate(M4(1), DV3(i, 0, 0));
        source.bones.push_back(bone);
      }
      RigConversion rig;
      rig.kind = "weapon";
      rig.sourceToTarget = {-1, 2, -1, 3};
      rig.model.report["t6Compatibility"] = J::object();
      for (auto [name, parent] :
           std::vector<std::pair<std::string, int>>{{"tag_view", -1},
                                                    {"tag_weapon", 0},
                                                    {"j_gun", 1},
                                                    {"mechanism", 2}}) {
        Bone bone;
        bone.name = name;
        bone.parent = parent;
        rig.model.bones.push_back(bone);
      }
      rig.model.bones[3].position = DV3(200, 0, 0);
      rig.model.bones[3].world = glm::translate(M4(1), DV3(200, 0, 0));
      rig.origin = DV3(1, 0, 0);
      Animation clip;
      clip.name = "sparse ownership regression";
      clip.frames = 1;
      clip.report = J::object();
      clip.poses.resize(1);
      for (auto &bone : source.bones)
        clip.poses[0].push_back({bone.position, bone.scale, bone.rotation});
      clip.poses[0][3].rotation = glm::angleAxis(.5, DV3(0, 0, 1));
      clip.animated = {{3, 2}};
      auto result = convert_animation_rig(source, rig, clip);
      require(result.animated == std::set<std::pair<int, int>>{{3, 2}},
              "Sparse mechanism rotation overwrote unrelated translation, "
              "scale or mount");
      clip.poses[0][3].rotation = Q(1, 0, 0, 0);
      clip.poses[0][2].rotation = glm::angleAxis(.5, DV3(0, 0, 1));
      clip.animated = {{2, 2}};
      result = convert_animation_rig(source, rig, clip);
      require(result.animated == std::set<std::pair<int, int>>{{3, 1}, {3, 2}},
              "Reparented mechanism lost authored ancestor "
              "rotation/translation dependency");
      require(glm::length(
                  result.poses[0][3].position -
                  DV3(100 + 100 * std::cos(.5), 100 * std::sin(.5), 0)) < 1e-8,
              "Reparented mechanism motion is not in the common weapon frame");
      clip.poses[0][2].rotation = Q(1, 0, 0, 0);
      clip.poses[0][0].rotation = glm::angleAxis(.5, DV3(0, 0, 1));
      clip.animated = {{0, 2}};
      result = convert_animation_rig(source, rig, clip);
      require(result.animated == std::set<std::pair<int, int>>{{1, 1}, {1, 2}},
              "Shared ancestor motion was keyed twice on mount and gun");
    }
    {
      J weapon = {{"category", "Weapon"},
                  {"name", "MainWeapon_002_ExampleSkin_1P"}};
      auto clip = [](const char *id, const char *name,
                     const char *category = "Weapon") {
        return J{{"id", id},
                 {"name", name},
                 {"category", category},
                 {"controllerReferences", J::array({{{"controller", "cab:99"},
                                                     {"original", "base:1"},
                                                     {"overridden", true}}})}};
      };
      J exact =
          J::array({clip("cab:1", "MainWeapon_002_ExampleSkin_1P_M_Fire")});
      J declarations = {
          {"clips",
           J::array({clip("cab:2", "MainWeapon_001_Example_1P_M_Fire"),
                     clip("cab:3", "MainWeapon_001_Example_1P_M_Idle"),
                     clip("cab:4", "MainWeapon_001_Example_3P_M_Run"),
                     clip("cab:5", "Unarmed_1P_M_Swim", "Viewhands"),
                     clip("cab:6", "MainWeapon_001_Example_1P_M_Equip"),
                     clip("cab:7", "MainWeapon_009_Other_1P_M_Equip")})}};
      J report;
      auto unchanged = clip("cab:8", "MainWeapon_009_Other_1P_M_Inspection");
      unchanged["controllerReferences"][0]["original"] = "cab:8";
      declarations["clips"].push_back(unchanged);
      auto combined =
          inherit_animation_sources(weapon, exact, declarations, &report);
      require(combined.size() == 2 && combined[0]["id"] == "cab:1" &&
                  combined[1]["id"] == "cab:3" &&
                  combined[1].contains("controllerReferences"),
              "Inheritance replaced a variant action, guessed a conflict, or "
              "lost provenance");
      weapon["declaredAnimationIDs"] = report.at("added");
      require(animation_match(weapon, combined[1]) ==
                  AnimationMatch::Applicable,
              "Declared shared clip is hidden by the variant filter");
      weapon.erase("declaredAnimationIDs");
      require(animation_match(weapon, combined[1]) == AnimationMatch::Unmatched,
              "A similar name fabricated an inheritance relationship");
    }
    {
      auto row = [](const char *id, const char *name, const char *bundle) {
        return J{{"id", id}, {"name", name}, {"bundle", bundle}};
      };
      J clips = J::array({row("cab:4", "Gun_M_Fire_camera", "b"),
                          row("cab:2", "Gun_M_Fire", "b"),
                          row("cab:3", "Gun_M_Fire_camera", "a"),
                          row("cab:1", "Gun_M_Fire", "a"),
                          row("cab:5", "Gun_M_PickUp", "a")});
      auto plan = plan_animation_exports(clips);
      std::reverse(clips.begin(), clips.end());
      require(plan == plan_animation_exports(clips),
              "Action filenames depend on index ordering");
      require(plan[4]["action"] == "first_raise",
              "PickUp fallback was not selected");
      require(plan[2]["pairedSource"] == "cab:1" &&
                  plan[3]["pairedSource"] == "cab:2" &&
                  plan[3]["pairedAction"] == plan[1]["action"],
              "Camera paired to the wrong duplicate source action");
      require(
          plan[3]["action"].get<std::string>().ends_with("_camera"),
          "Duplicate camera could become an ordinary weapon profile action");
      clips.push_back(row("cab:6", "Gun_M_First_Equip", "a"));
      clips.push_back(clips[0]);
      plan = plan_animation_exports(clips);
      require(plan.size() == 6 && plan[4]["action"] == "pick_up" &&
                  plan[5]["action"] == "first_raise",
              "First_Equip precedence or duplicate source identity lost");
      auto cameraOnly = plan_animation_exports(
          J::array({row("cab:7", "Gun_M_Idle_camra", "a")}));
      require(!cameraOnly[0].contains("pairedSource") &&
                  cameraOnly[0]["pairingStatus"] == "base action not selected",
              "Camera-only selection fabricated a base action");
    }
    {
      auto vector = [](double x) {
        return J{{"x", x}, {"y", 0.0}, {"z", 0.0}};
      };
      auto key = [&](double time, double x, double slope) {
        return J{{"time", time},
                 {"value", vector(x)},
                 {"inSlope", vector(slope)},
                 {"outSlope", vector(slope)}};
      };
      J curve = {{"m_Curve", J::array({key(0, 0, 0), key(1, 1, 2)})},
                 {"m_PreInfinity", 2},
                 {"m_PostInfinity", 2},
                 {"m_RotationOrder", 4}};
      J header = {{"m_Legacy", true},
                  {"m_MuscleClipSize", 0},
                  {"m_Name", "quadratic"},
                  {"m_SampleRate", 4.0},
                  {"m_WrapMode", 2},
                  {"m_PositionCurves",
                   J::array({{{"path", "root/child"}, {"curve", curve}}})}};
      for (auto field :
           {"m_RotationCurves", "m_CompressedRotationCurves", "m_EulerCurves",
            "m_ScaleCurves", "m_FloatCurves", "m_PPtrCurves"})
        header[field] = J::array();
      auto clip = sample_legacy_curves(header);
      require(clip.frames == 5 && clip.looping &&
                  clip.bindings[0].path == crc32("root/child"),
              "Legacy curve timing, wrapping, or path binding lost");
      for (int f = 0; f < clip.frames; f++)
        require(std::abs(clip.at(f, 0) - (f / 4.0) * (f / 4.0)) < 1e-12,
                "Legacy sampler does not reproduce an analytic quadratic");
      header["m_PositionCurves"][0]["curve"]["m_Curve"][0]["outSlope"]["x"] =
          INFINITY;
      clip = sample_legacy_curves(header);
      require(clip.at(3, 0) == 0 && clip.at(4, 0) == 1,
              "Stepped curve lost its terminal key");
      header["m_PositionCurves"].push_back(header["m_PositionCurves"][0]);
      bool duplicateRejected = false;
      try {
        sample_legacy_curves(header);
      } catch (...) {
        duplicateRejected = true;
      }
      require(duplicateRejected,
              "Duplicate legacy bindings silently overwritten");
      header["m_PositionCurves"] = J::array();
      Bytes packedQuaternions;
      for (uint32_t word : {3u | (255u << 3) | (511u << 12) | (511u << 22),
                            2u | (511u << 3) | (511u << 13) | (255u << 23)})
        for (unsigned shift = 0; shift < 32; shift += 8)
          packedQuaternions.push_back(uint8_t(word >> shift));
      J packed = {
          {"m_Path", "root/child"},
          {"m_PreInfinity", 2},
          {"m_PostInfinity", 2},
          {"m_Times",
           {{"m_NumItems", 2},
            {"m_BitSize", 7},
            {"m_Data", J::binary(Bytes{0, 50})}}},
          {"m_Values",
           {{"m_NumItems", 2}, {"m_Data", J::binary(packedQuaternions)}}},
          {"m_Slopes",
           {{"m_NumItems", 8},
            {"m_BitSize", 0},
            {"m_Start", 0.0},
            {"m_Range", 0.0},
            {"m_Data", J::binary(Bytes{})}}}};
      header["m_CompressedRotationCurves"] = J::array({packed});
      clip = sample_legacy_curves(header);
      require(clip.frames == 5 && clip.columns == 4 &&
                  clip.report.at("packedRotationKeys") == 2,
              "Packed rotation time deltas or binding width changed");
      require(
          std::abs(clip.at(2, 2) - std::sqrt(.5)) < .003 &&
              std::abs(clip.at(2, 3) - std::sqrt(.5)) < .003,
          "Packed half-turn does not pass through its normalized quarter-turn");
      auto rejectsPacked = [&](J changed) {
        auto invalid = header;
        invalid["m_CompressedRotationCurves"] = J::array({changed});
        try {
          sample_legacy_curves(invalid);
        } catch (const std::exception &) {
          return true;
        }
        return false;
      };
      auto changed = packed;
      changed["m_Times"]["m_Data"] = J::binary(Bytes{0});
      require(rejectsPacked(changed),
              "Truncated packed time vector was accepted");
      changed = packed;
      changed["m_Times"]["m_Data"] = J::binary(Bytes{0, 0});
      require(rejectsPacked(changed),
              "Duplicate packed key times were accepted");
      changed = packed;
      changed["m_Slopes"]["m_NumItems"] = 4;
      require(rejectsPacked(changed),
              "Missing packed key tangents were accepted");
      changed = packed;
      changed["m_Slopes"]["m_Range"] = INFINITY;
      require(rejectsPacked(changed),
              "Nonfinite packed tangent range was accepted");
      changed = packed;
      changed["m_Values"]["m_Data"] = J::binary(Bytes(8, 255));
      require(rejectsPacked(changed), "Invalid packed quaternion was accepted");
    }
    Image texture{2, 1, {0, 0, 0, 255, 255, 255, 255, 255}};
    {
      J a = {{"m_Name", "original"},
             {"m_Width", 2},
             {"m_Height", 1},
             {"m_TextureFormat", 4},
             {"m_ColorSpace", 1},
             {"m_MipCount", 1},
             {"m_TextureSettings", {{"m_WrapMode", 0}, {"m_FilterMode", 1}}}};
      J renamed = a;
      renamed["m_Name"] = "different_name";
      renamed["m_StreamData"] = {{"path", "another.resS"}, {"offset", 32768}};
      auto key = texture_content_fingerprint(a, texture.pixels);
      require(key == texture_content_fingerprint(renamed, texture.pixels),
              "Renamed and relocated duplicate texture not recognized");
      renamed["m_TextureSettings"]["m_WrapMode"] = 1;
      require(key != texture_content_fingerprint(renamed, texture.pixels),
              "Different sampler settings collapsed into one material");
      renamed = a;
      renamed["m_ColorSpace"] = 0;
      require(key != texture_content_fingerprint(renamed, texture.pixels),
              "Different texture color spaces collapsed into one material");
    }
    auto pixel = sample_linear_overlay(texture, V2(.5, .5));
    require(std::abs(pixel.x - .5f) < 1e-6,
            "Overlay filtering must occur in linear space");
    Surface surface;
    surface.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0},
                         {0, 0, 1}, {1, 0, 1}, {0, 1, 1}};
    surface.uv0 = {{0, 0}, {1, 0}, {0, 1}, {0, 0}, {1, 0}, {0, 1}};
    surface.uv1 = {V2(.25, .5), V2(.25, .5), V2(.25, .5),
                   V2(.75, .5), V2(.75, .5), V2(.75, .5)};
    surface.indices = {0, 1, 2, 3, 4, 5};
    bool split = false;
    try {
      bake_overlay(texture, surface, 8, 8, std::vector<float>(64, 1));
    } catch (const UVLayers &layers) {
      split = true;
      require(layers.groups.size() == 2,
              "Overlapping differently colored islands must split");
      for (auto &g : layers.groups) {
        auto part = subset_surface(surface, g);
        require(part.positions.size() == 3 && part.indices.size() == 3,
                "Layer geometry was not compacted");
        auto baked =
            bake_overlay(texture, part, 8, 8, std::vector<float>(64, 1));
        require(baked[0].a == 1, "Baked triangle coverage lost");
      }
    }
    require(split, "Conflicting mirror bake silently flattened");
    auto invisible =
        bake_overlay(texture, surface, 8, 8, std::vector<float>(64, 0));
    require(invisible.size() == 64, "Invisible overlap should not split");
    bool bounded = false;
    try {
      Bytes data{1, 2, 3};
      Reader reader(data);
      reader.get<uint32_t>();
    } catch (...) {
      bounded = true;
    }
    require(bounded, "Truncated binary accepted");
    require(animation_category("Knife_M_3P_Run2Prone",
                               "cod_models/weapons/animations/mp.pak") ==
                "Player",
            "Player action category");
    require(animation_category("MainWeapon_005_Arctic50_1P_M_Fire",
                               "cod_models/weapons/animations/mp.pak") ==
                "Weapon",
            "Weapon action category");
    {
      J hands = {{"name", "C_F_Charly_Sinister_1P"},
                 {"category", "Viewhands"},
                 {"bundle", "hands"},
                 {"paths", J::array({100, 200})}};
      J clip = {{"name", "ADV1P_BOCW_M_Sprint"},
                {"bundle", "motions"},
                {"paths", J::array({200, 300})}};
      require(!is_weapon_entry(hands) &&
                  !is_weapon_entry(
                      J{{"name", "Vehicles_44249_AttackHelicopter_UI_Ani"},
                        {"category", "Etc"}}),
              "Character or prop gets weapon assembly/export controls from its "
              "name");
      require(
          animation_match(hands, clip) == AnimationMatch::Applicable,
          "Viewhands misidentified as a weapon because its name contains 1P");
      clip["paths"] = J::array({400});
      require(animation_match(hands, clip) == AnimationMatch::Unmatched,
              "Unrelated character motion offered as applicable");
      clip.erase("paths");
      require(animation_match(hands, clip) == AnimationMatch::Unchecked,
              "Unsupported indexed motion silently hidden");
      J weapon = {{"name", "MainWeapon_005_Arctic50_1P"},
                  {"category", "Weapon"},
                  {"bundle", "gun"},
                  {"paths", J::array({100})}};
      clip = {{"name", "MainWeapon_005_Arctic50_1P_M_Fire"},
              {"bundle", "motions"}};
      require(animation_match(weapon, clip) == AnimationMatch::Applicable,
              "Known weapon action lost while decoder metadata is unavailable");
      clip = {{"name", "NumberedPartIdle"},
              {"bundle", "otherGun"},
              {"paths", J::array({100})}};
      require(animation_match(weapon, clip) == AnimationMatch::Unmatched,
              "Numbered bone used to infer unrelated weapon ownership");
      clip["bundle"] = "gun";
      require(animation_match(weapon, clip) == AnimationMatch::Applicable,
              "Authored weapon part motion absent from filter");
      J prop = {{"name", "Helicopter"},
                {"category", "Etc"},
                {"bundle", "helicopter"},
                {"paths", J::array({100})}};
      clip = {{"name", "ChestOpen"},
              {"bundle", "chest"},
              {"paths", J::array({100})}};
      require(animation_match(prop, clip) == AnimationMatch::Unmatched,
              "Common numbered prop bones used to infer unrelated animation "
              "ownership");
    }
    J socket = {{"id", "socket"}, {"matrix", matrix_json(M4(1))}};
    J option = {{"mesh", "part"},
                {"name", "part"},
                {"matrix", matrix_json(M4(1))},
                {"sockets", J::object()}};
    J slots = {{"slots", J::array({{{"id", "mag"},
                                    {"label", "Magazine"},
                                    {"parent", ""},
                                    {"socketName", "Mag_point"},
                                    {"baseSocket", socket},
                                    {"options", J::array({option})}}})}};
    J choices = {{"mag", nullptr}}, states;
    require(resolve_parts(slots, choices, true).empty() &&
                choices["mag"].is_null(),
            "Explicit None overwritten");
    choices = J::object();
    require(resolve_parts(slots, choices, true).size() == 1,
            "Auto selection missing");
    choices["mag"] = "incompatible";
    bool rejected = false;
    try {
      resolve_parts(slots, choices, false);
    } catch (...) {
      rejected = true;
    }
    require(rejected, "Invalid manual choice silently dropped");
    resolve_parts(slots, choices, false, &states, false);
    require(!states["mag"]["reason"].get<std::string>().empty(),
            "Invalid choice lacks UI explanation");
    {
      JobQueue queue(2);
      std::mutex mutex;
      std::condition_variable condition;
      bool release = false;
      std::atomic_int started = 0;
      auto work = [&](JobContext &) {
        started++;
        condition.notify_all();
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return release; });
      };
      for (int i = 0; i < 5; i++)
        queue.enqueue("test", work);
      {
        std::unique_lock lock(mutex);
        bool ready = condition.wait_for(lock, std::chrono::seconds(5),
                                        [&] { return started == 2; });
        release = true;
        condition.notify_all();
        require(ready, "Worker queue failed to start bounded jobs");
      }
      for (size_t i = 2; i < queue.tasks.size(); i++)
        queue.tasks[i]->context.cancel = true;
      queue.shutdown();
      require(started == 2, "Queued cancelled work ran");
    }
    require(argc == 2, "Test scratch folder required");
    auto directory = fs::absolute(pathof(argv[1]));
    fs::create_directories(directory);
    auto test =
        directory /
        ("case-" + hex64(uint64_t(Clock::now().time_since_epoch().count())));
    fs::create_directory(test);
    Model model;
    model.name = "fixture";
    model.report = J::object();
    Bone bone;
    bone.name = "root";
    model.bones.push_back(bone);
    surface = subset_surface(surface, {0, 1, 2});
    surface.name = "triangle";
    surface.materialId = "material";
    surface.normals.assign(3, V3(0, 0, 1));
    surface.weights.assign(3, V4(1, 0, 0, 0));
    surface.joints.assign(3, glm::uvec4(0));
    model.surfaces.push_back(surface);
    {
      Animation motion;
      motion.frames = 3;
      motion.poses.resize(3, std::vector<Pose>(1));
      motion.poses[0][0].position = DV3(-20, 4, -6);
      motion.poses[1][0].position = DV3(15, -8, 3);
      motion.poses[1][0].rotation = glm::angleAxis(1.7, DV3(0, 0, 1));
      motion.poses[2][0].position = DV3(4, 19, 11);
      auto bounds = animation_preview_bounds(model, motion);
      require(bounds[0].x < -19 && bounds[1].y > 18,
              "Animated preview bounds only describe the rest pose");
      for (double frame = 0; frame <= 2; frame += .5) {
        auto palette = sample_palette(model, motion, frame);
        for (const auto &point : surface.positions) {
          DV3 posed(palette[0] * glm::dvec4(point, 1));
          for (int axis = 0; axis < 3; axis++)
            require(posed[axis] >= bounds[0][axis] - 1e-8 &&
                        posed[axis] <= bounds[1][axis] + 1e-8,
                    "Animated preview bounds omit a posed vertex");
        }
      }
      JobContext cancelledBounds;
      cancelledBounds.cancel = true;
      bool stopped = false;
      try {
        animation_preview_bounds(model, motion, &cancelledBounds);
      } catch (const std::exception &) {
        stopped = true;
      }
      require(stopped, "Preview framing ignored cancellation");
    }
    MaterialSet mats;
    Material material;
    material.id = "material";
    material.name = "material";
    for (auto slot : {"albedo", "normal", "specular", "gloss", "ao"})
      material.maps[slot] = texture;
    mats.materials[material.id] = material;
    JobContext cancel;
    cancel.cancel = true;
    bool cancelled = false;
    try {
      export_model(model, mats, test, "cancelled", &cancel);
    } catch (...) {
      cancelled = true;
    }
    require(cancelled && !fs::exists(test / "cancelled.cast") &&
                fs::is_empty(test),
            "Cancellation left published or staging output");
    model.report = {{"surfaces", 999},
                    {"vertices", 999},
                    {"triangles", 999},
                    {"bones", 999}};
    auto exported = export_model(model, mats, test, "existing");
    require(exported.at("surfaces") == 1 && exported.at("vertices") == 3 &&
                exported.at("triangles") == 1 && exported.at("bones") == 1,
            "Export statistics describe stale geometry rather than the written "
            "model");
    auto before = read_bytes(test / "existing.cast");
    {
        auto fixture=test/"folder-animations";fs::create_directories(fixture/"models");
        write_json(fixture/"weapon-reference.json",J::object());
        ExportRequest request;request.data=fixture;request.database=fixture/"library.sqlite";request.modelsDestination=fixture/"models";
        J entry={{"id","fixture:weapon"},{"type","model"},{"category","Weapon"},{"name","MainWeapon_123_AK117_1P"},{"complete",true},{"lod","0"},{"bundle","fixture"}};
        Database database(request.database);database.run("INSERT INTO assets VALUES(?,?,?,?,?,?)",{"fixture:weapon","fixture","model","Weapon","MainWeapon_123_AK117_1P",entry.dump()});
        for(auto stem:{"viewmodel_ar_ak117","viewmodel_ar_CustomName","viewmodel_ar_unknown"})fs::copy_file(test/"existing.cast",fixture/"models"/(std::string(stem)+".cast"));
        write_json(fixture/"models/viewmodel_ar_CustomName.json",{{"source",entry},{"parts",J::array()}});
        auto plan=plan_exported_animations(request);
        require(plan["matched"]==2 && plan["unmatched"]==1,"Export-name or saved-source matching failed");
        entry["id"]="fixture:other";database.run("INSERT INTO assets VALUES(?,?,?,?,?,?)",{"fixture:other","fixture","model","Weapon","MainWeapon_123_AK117_1P",entry.dump()});
        plan=plan_exported_animations(request);
        require(plan["matched"]==1 && plan["unmatched"]==2,"Ambiguous name was guessed");
        JobContext stopped;stopped.cancel=true;bool cancelled=false;
        try{plan_exported_animations(request,&stopped);}catch(...){cancelled=true;}
        require(cancelled,"Folder scan ignored cancellation");
    }
    {
        auto source=model;source.bones[0].paths={123};source.bones[0].position=DV3(99);
        use_exported_skeleton(source,test/"existing.cast");
        require(source.surfaces.empty() && source.bones[0].paths==std::vector<uint32_t>{123} && glm::length(source.bones[0].position)<1e-8,"Existing CAST skeleton was not used for animation binding");
        source.bones[0].name="wrong";bool rejected=false;
        try{use_exported_skeleton(source,test/"existing.cast");}catch(...){rejected=true;}
        require(rejected,"Unrelated exported skeleton was accepted");
        auto scaled=model;scaled.bones[0].paths={123};Clip clip;clip.name="visibility";clip.frames=2;clip.columns=3;clip.bindings={{123,3,4,0,3,4,0}};clip.samples={0,0,0,-1,1,1};clip.report=J::object();
        auto motion=bind_animation(scaled,clip);
        require(motion.poses[0][0].scale==DV3(0) && motion.poses[1][0].scale.x==-1,"Authored visibility or reflected scale was rejected");
        auto a=encode_animation_cast(scaled,motion,"repeat"),b=encode_animation_cast(scaled,motion,"repeat");
        require(a!=b && equivalent_animation_cast(a,b),"File-local node IDs prevented identical animation reuse");
        motion.poses[1][0].scale.x=2;
        require(!equivalent_animation_cast(a,encode_animation_cast(scaled,motion,"repeat")),"Changed animation payload was treated as identical");
    }
    bool protectedOutput = false;
    try {
      export_model(model, mats, test, "existing");
    } catch (...) {
      protectedOutput = true;
    }
    require(protectedOutput && before == read_bytes(test / "existing.cast"),
            "Existing output overwritten");
    {
      auto fixture = test / "resources";
      fs::create_directories(fixture);
      write_json(fixture / "catalog.json", {{"bundles", J::array()}});
      write_json(fixture / "codm_types.json", J::object());
      Source source(fixture / "catalog.json", fixture);
      {
        J active = {{"path", "active.pak"},
                    {"nodes", J::array({{{"name", "CAB-active"}}})}};
        J older = {{"path", "older.pak"},
                   {"nodes", J::array({{{"name", "CAB-active"}},
                                       {{"name", "CAB-dependency"}},
                                       {{"name", "CAB-dependency.resS"}}})}};
        write_json(fixture / "catalog.json",
                   {{"bundles", J::array({active})},
                    {"dependencyBundles", J::array({older})}});
        Source dependencies(fixture / "catalog.json", fixture);
        require(dependencies.nodeBundles.at("cab-active") == "active.pak" &&
                    dependencies.nodeBundles.at("cab-dependency") ==
                        "older.pak" &&
                    dependencies.nodeBundles.at("cab-dependency.ress") ==
                        "older.pak",
                "Historical dependency either replaced an active CAB or lost "
                "an exact stream identity");
        write_json(fixture / "catalog.json", {{"bundles", J::array()}});
      }
      {
        TypeNode nameField{"string", "m_Name"};
        TypeNode tail{"int", "payload"};
        TypeNode named{"Fixture", "Base", -1, 0, {nameField, tail}};
        source.types[43] = named;
        SerializedFile file;
        file.name = "name-fixture";
        file.schemas.resize(1);
        file.data = {3, 0, 0, 0, 'g', 'u', 'n', 0, 255};
        Object object{&file, 1, 43, 0, 0, file.data.size()};
        require(source.name(object) == "gun" && !object.decoded,
                "Name discovery decoded the full asset payload");
        bool rejected = false;
        try {
          source.tree(object);
        } catch (const std::exception &) {
          rejected = true;
        }
        require(
            rejected && !object.decoded,
            "Name discovery bypassed full-object validation on subsequent use");
        file.data.insert(file.data.end(), 3, 0);
        object.size = file.data.size();
        require(source.tree(object).at("payload") == 255 &&
                    source.name(object) == "gun",
                "Full decoding changed the discovery name");

        // Embedded layouts take precedence. A name after another field
        // must still be located by the full schema, not by a prefix guess.
        object.decoded.reset();
        auto unknown = tail;
        unknown.name = "$unknown_common_string_1073";
        file.schemas[0] = TypeNode{"Fixture", "Base", -1, 0, {nameField, unknown}};
        require(source.name(object) == "gun" && !object.decoded,
                "An unused unknown field prevented safe name discovery");
        rejected = false;
        try { source.tree(object); } catch (const std::exception &) { rejected = true; }
        require(rejected && !object.decoded, "An unknown selected layout was silently decoded");
        object.decoded.reset();
        file.schemas[0] = TypeNode{"Fixture", "Base", -1, 0, {tail, nameField}};
        file.data = {7, 0, 0, 0, 3, 0, 0, 0, 'r', 'i', 'g', 0};
        object.size = file.data.size();
        require(
            source.name(object) == "rig" && object.decoded,
            "Name discovery ignored the embedded schema or a leading field");
        object.decoded.reset();
        file.schemas[0] = named;
        file.data = {255, 255, 255, 127};
        object.size = file.data.size();
        require(source.name(object) == object.id() && !object.decoded,
                "Malformed name escaped its object bounds");
      }
      source.installedData = fixture;
      auto write = [&](const char *name) {
        std::ofstream output(fixture / name, std::ios::binary);
        output.write("abcdef", 6);
      };
      write("resources.assets.resS");
      write("sharedassets12.assets.resS");
      write("unrelated.resS");
      require(source.resource("archive:/ignored/resources.assets.resS", 2, 3) ==
                  Bytes({'c', 'd', 'e'}),
              "Loose texture stream range was not resolved");
      require(source.resource("sharedassets12.assets.resS", 5, 1) ==
                  Bytes({'f'}),
              "Numbered player resource files were hardcoded to the initial "
              "installation");
      require(source.dependency_paths().size() == 2 &&
                  source.dependency_paths().contains(
                      pathstr(fixture / "resources.assets.ress")),
              "Loose texture source was omitted from cache dependencies");
      auto rejects = [&](const char *name, uint64_t offset, uint64_t count) {
        try {
          source.resource(name, offset, count);
          return false;
        } catch (...) {
          return true;
        }
      };
      require(rejects("resources.assets.resS", 5, 2) &&
                  rejects("resources.assets.resS", UINT64_MAX - 1, 8) &&
                  rejects("../unrelated.resS", 0, 1),
              "Loose resource fallback escaped its file or accepted an invalid "
              "range");
      source.nodeBundles["resources.assets.ress"] =
          pathstr(fixture / "missing.pak");
      require(rejects("resources.assets.resS", 0, 1),
              "Loose fallback silently replaced the catalog-selected bundle");
      source.clear();
      require(source.dependency_paths().empty(),
              "Cleared source retained stale dependencies");
      auto originalState = source.installed_data_state();
      require(originalState.at("files").size() == 2,
              "Unrelated files polluted installation cache state");
      write("sharedassets13.assets.resS");
      auto addedState = source.installed_data_state();
      require(addedState != originalState,
              "Newly available player dependency did not invalidate library "
              "cache state");
      {
        std::ofstream changed(fixture / "sharedassets13.assets.resS",
                              std::ios::binary);
        changed << "changed resource";
      }
      require(
          source.installed_data_state() != addedState,
          "Changed player dependency did not invalidate library cache state");
      fs::remove(fixture / "sharedassets13.assets.resS");
      require(source.installed_data_state() == originalState,
              "Deleted player dependency remained in library cache state");
    }
    remove_owned_tree(test, directory);
    require(!fs::exists(test), "Owned scratch cleanup failed");
    std::cout << "Overlay, parser bounds, categories, cancellation and output "
                 "protection passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
