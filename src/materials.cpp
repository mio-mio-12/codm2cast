#include "materials.hpp"
#include "eye_material.hpp"
#include "hair_coverage.hpp"
#include "effect_alpha.hpp"
#include "uv_bake.hpp"
#include <regex>
namespace codm {
std::vector<std::pair<std::string, std::string>>
MaterialResolver::containers(SerializedFile &f) {
  if (containerCache.contains(f.name))
    return containerCache.at(f.name);
  std::vector<std::pair<std::string, std::string>> out;
  for (auto &[pid, o] : f.objects)
    if (o.cid == 142) {
      for (auto &p : source.tree(o).at("m_Container")) {
        auto *asset = source.ref(o, p.at("second").at("asset"));
        if (asset)
          out.push_back({lower(p.at("first")), asset->id()});
      }
    }
  containerCache[f.name] = out;
  return out;
}
J MaterialResolver::candidates(Object &mesh, Object *context) {
  std::set<std::string> roots, fileNames;
  std::vector<Object *> targets{&mesh};
  if (context)
    targets.push_back(context);
  if (related.empty())
    for (auto &b : source.catalog.at("bundles")) {
      auto logical = hint(b.at("path"));
      auto at = logical.find("cod_models/");
      if (at != logical.npos)
        related[logical.substr(at + 11)].push_back(b);
    }
  for (auto *obj : targets) {
    fileNames.insert(obj->file->name);
    bool rooted = false;
    for (auto &[path, id] : containers(*obj->file))
      if (id == obj->id()) {
        auto root = path.substr(0, path.find_last_of('/'));
        // Packaging directories are siblings of materials, not a skin identity.
        auto leaf = root.substr(root.find_last_of('/') + 1);
        if (leaf == "uncompress" || leaf == "meshes" || leaf == "mesh" ||
            leaf == "fbx")
          root = root.substr(0, root.find_last_of('/'));
        roots.insert(root);
        rooted = true;
      }
    if (!rooted)
      continue;
    auto logical = hint(obj->file->bundle);
    auto at = logical.find("cod_models/");
    if (at == logical.npos)
      continue;
    for (auto &b : related[logical.substr(at + 11)]) {
      auto path = b.at("path").get<std::string>();
      source.load(path);
      for (auto &node : b.at("nodes")) {
        auto key = lower(basename(node.at("name")));
        if (source.files.contains(key))
          fileNames.insert(source.files.at(key)->name);
      }
    }
  }
  // Materials may live in an explicitly referenced shared bundle. Keep the
  // asset-directory boundary when inspecting those dependencies.
  auto ownerFiles = fileNames;
  for (auto &name : ownerFiles)
    for (auto &external : source.file(name).externals) {
      auto key = lower(basename(external));
      if (source.nodeBundles.contains(key))
        fileNames.insert(source.file(external).name);
    }
  J result = J::array();
  std::set<std::string> seen;
  for (auto &name : fileNames) {
    auto &file = source.file(name);
    for (auto &[path, id] : containers(file)) {
      bool inside = false;
      for (auto &root : roots)
        inside |= path.starts_with(root + "/");
      if (!inside)
        continue;
      auto &o = source.object(id);
      if (o.cid == 21 && !placeholder(source, &o) && seen.insert(id).second)
        result.push_back(
            {{"id", id}, {"name", source.name(o)}, {"assetPath", path}});
    }
  }
  return result;
}
static std::string equivalent_material(Source &source,
                                       const std::set<std::string> &ids) {
  if (ids.empty())
    return {};
  if (ids.size() == 1)
    return *ids.begin();
  std::string key;
  for (auto &id : ids) {
    auto current = material_content_key(source, source.object(id));
    if (key.empty())
      key = current;
    else if (current != key)
      return {};
  }
  return *ids.begin();
}
static std::string material_name_key(std::string name) {
  name = lower(name);
  name.erase(std::remove(name.begin(), name.end(), '_'), name.end());
  return name;
}
static std::vector<std::string> choose_names(Source &source,
                                             const std::string &meshName,
                                             int count, const J &options,
                                             bool &roleFallback) {
  auto stem = std::regex_replace(
      lower(meshName), std::regex("(_(1p|3p|pov|ui|lod[0-9]+))+$"), "");
  std::smatch m;
  std::vector<std::string> out(count);
  bool component = std::regex_match(
      stem, m,
      std::regex("(wea|mag|sto|rai|bar|sig|gri|grip|iro|muz|sco|guide|tri|sra)("
                 "[0-9]+(?:up)?)_(.+)"));
  std::string kind = component ? m[1].str() : "wea",
              number = component ? m[2].str() : "01",
              variant = component ? std::regex_replace(
                                        m[3].str(), std::regex("^front_"), "")
                                  : stem;
  std::map<std::string, std::set<std::string>> names;
  for (auto &o : options) {
    auto name =
        std::regex_replace(lower(o.at("name")), std::regex("_(mtl|mat)$"), "");
    names[material_name_key(name)].insert(o.at("id"));
  }
  auto unique = [&](const std::string &name) {
    auto ids = names[material_name_key(name + "_noflowmap")];
    auto &aliases = names[material_name_key(name + "_noflomap")];
    ids.insert(aliases.begin(), aliases.end());
    if (ids.empty())
      ids = names[material_name_key(name)];
    return equivalent_material(source, ids);
  };
  if (count > 1) {
    bool all = true;
    for (int i = 0; i < count; i++) {
      auto n = std::to_string(i + 1);
      out[i] =
          unique(kind + "_" + variant + "_" + (n.size() < 2 ? "0" : "") + n);
      all &= !out[i].empty();
    }
    if (!all) {
      out.assign(count, "");
      // Multiple submeshes need not use different finishes. A single
      // unambiguous receiver material can be shared across its slots.
      // Competing receiver finishes remain unresolved for user selection.
      if (component && kind == "wea") {
        std::set<std::string> family;
        auto prefix = material_name_key(kind + "_" + variant);
        for (auto &[name, ids] : names)
          if (name.starts_with(prefix) || name == material_name_key(stem))
            family.insert(ids.begin(), ids.end());
        auto shared = equivalent_material(source, family);
        if (!shared.empty() && !unique(kind + "_" + variant).empty())
          out.assign(count, shared);
      }
    }
    return out;
  }
  for (auto &n :
       std::vector<std::string>{stem, kind + "_" + variant, "wea_" + variant,
                                variant, "wea_" + variant + "_" + number}) {
    auto id = unique(n);
    if (!id.empty()) {
      out[0] = id;
      break;
    }
  }
  if (out[0].empty() && component) {
    // Asset revisions sometimes retain an older material family spelling.
    // Resolve only a unique component role within the already scoped source
    // directory, and only for a single slot. Never infer a multi-slot order.
    auto role = [&](const std::string &wanted, bool numbered) {
      std::set<std::string> ids;
      for (auto &option : options) {
        std::smatch match;
        auto name = lower(option.at("name"));
        if (std::regex_search(
                name, match,
                std::regex("^(wea|mag|sto|rai|bar|sig|gri|grip|iro|muz|sco|"
                           "guide|tri|sra)([0-9]+(?:up)?)?_")) &&
            match[1].str() == wanted && (!numbered || match[2].str() == number))
          ids.insert(option.at("id"));
      }
      return std::pair{!ids.empty(), equivalent_material(source, ids)};
    };
    for (auto tier : std::vector<std::pair<std::string,bool>>{{kind,true},{kind,false},{"wea",false}}) {
      auto [present, id] = role(tier.first,tier.second);
      if (present) { out[0]=id; break; }
    }
    roleFallback = !out[0].empty();
  }
  return out;
}
J MaterialResolver::resolve(Surface &surface, const J &overrides) {
  auto &mesh = source.object(surface.meshId);
  Object *context = surface.materialContextMesh.empty()
                        ? nullptr
                        : &source.object(surface.materialContextMesh);
  Object *renderer =
      surface.rendererId.empty() ? nullptr : &source.object(surface.rendererId);
  int count = int(source.tree(mesh).at("m_SubMeshes").size());
  auto options = candidates(mesh, context);
  bool roleFallback = false;
  auto named =
      choose_names(source, source.name(mesh), count, options, roleFallback);
  int slot = surface.submesh;
  require(slot >= 0 && slot < count, "Invalid surface slot");
  Object *material = nullptr;
  std::string method = "unresolved", evidence;
  if (renderer) {
    auto mats = renderer_materials(source, *renderer);
    if (slot < int(mats.size()) && !placeholder(source, mats[slot])) {
      material = mats[slot];
      method = "authored renderer";
    }
  }
  if (overrides.contains(surface.meshId) &&
      overrides.at(surface.meshId).contains(std::to_string(slot))) {
    auto id = overrides.at(surface.meshId)
                  .at(std::to_string(slot))
                  .get<std::string>();
    material = &source.object(id);
    require(material->cid == 21 && !placeholder(source, material),
            "Override is not a usable Material");
    method = "explicit surface selection";
  }
  if (!material && renderer) {
    auto key =
        renderer->file->name + ":" + mesh.id() + ":" + std::to_string(slot);
    if (!peers.contains(key)) {
      std::vector<Object *> matches;
      std::set<std::string> ids;
      for (auto &[pid, o] : renderer->file->objects)
        if (o.cid == 23 || o.cid == 137) {
          try {
            auto mats = renderer_materials(source, o);
            if (slot >= int(mats.size()) || placeholder(source, mats[slot]))
              continue;
            auto r = renderer_info(source, o);
            if (r.mesh->id() == mesh.id() &&
                ids.insert(mats[slot]->id()).second)
              matches.push_back(mats[slot]);
          } catch (const std::exception &) {
          }
        }
      peers[key] = matches;
    }
    std::set<std::string> peerIds;
    for (auto *peer : peers[key])
      peerIds.insert(peer->id());
    auto peerId = equivalent_material(source, peerIds);
    if (!peerId.empty()) {
      material = &source.object(peerId);
      method = "authored same-mesh renderer";
    }
  }
  if (!material && fs::exists(dbPath)) {
    Database db(dbPath);
    std::set<std::string> ids;
    for (auto &row :
         db.query("SELECT materials FROM donors WHERE mesh=?", {mesh.id()})) {
      auto mats = J::parse(row[0]);
      if (slot < int(mats.size()) && !mats[slot].is_null())
        ids.insert(mats[slot]);
    }
    auto equivalent = equivalent_material(source, ids);
    if (!equivalent.empty()) {
      auto &candidate = source.object(equivalent);
      if (!placeholder(source, &candidate)) {
        material = &candidate;
        method = "indexed authored same-mesh renderer";
      }
    }
  }
  if (!material && !named[slot].empty()) {
    material = &source.object(named[slot]);
    method = "authored asset directory and component material name";
    if (roleFallback)
      method = "authored asset directory and unique component material role; "
               "name differs, assignment inferred";
    if (count > 1 &&
        std::all_of(named.begin(), named.end(),
                    [&](const auto &id) { return id == named[slot]; }))
      method = "authored asset directory and shared receiver material; slot "
               "assignment inferred";
  }
  if (material)
    surface.materialId = material->id();
  else
    surface.materialId.clear();
  return {{"mesh", mesh.id()},
          {"slot", slot},
          {"material", material ? J(material->id()) : J()},
          {"method", method},
          {"candidates", options},
          {"resolved", material != nullptr},
          {"verifiedRuntimeBinding",
           method == "authored renderer" ||
               method == "authored same-mesh renderer" ||
               method == "indexed authored same-mesh renderer"}};
}
static float linear(float c) {
  return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
}
static float srgb(float c) {
  c = std::clamp(c, 0.f, 1.f);
  return c <= .0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - .055f;
}
static uint8_t quant(double v) {
  require(std::isfinite(v), "Nonfinite surface value");
  return uint8_t(std::clamp(std::nearbyint(v * 255), 0., 255.));
}
static std::string safe_name(std::string s) {
  for (auto &c : s)
    if (!(std::isalnum(uint8_t(c)) || c == '_' || c == '-' || c == '.'))
      c = '_';
  if (s.size() > 100)
    s.resize(100);
  return s.empty() ? "material" : s;
}
static V4 rgba(const J &j) {
  return {j.at("r").get<float>(), j.at("g").get<float>(),
          j.at("b").get<float>(), j.at("a").get<float>()};
}
static std::string bake_context(const Surface &s, bool projected = false) {
  auto bytes = std::span(reinterpret_cast<const uint8_t *>(s.indices.data()),
                         s.indices.size() * sizeof(uint32_t));
  auto uv = std::span(reinterpret_cast<const uint8_t *>(s.uv1.data()),
                      s.uv1.size() * sizeof(V2));
  if (!projected)
    return s.meshId + ":" + std::to_string(s.submesh) + ":" + sha256(bytes) +
           ":" + sha256(uv);
  auto positions =
      std::span(reinterpret_cast<const uint8_t *>(s.sourcePositions.data()),
                s.sourcePositions.size() * sizeof(V3));
  auto normals =
      std::span(reinterpret_cast<const uint8_t *>(s.sourceNormals.data()),
                s.sourceNormals.size() * sizeof(V3));
  auto primaryUV = std::span(reinterpret_cast<const uint8_t *>(s.uv0.data()),
                             s.uv0.size() * sizeof(V2));
  return s.meshId + ":" + std::to_string(s.submesh) + ":" + sha256(bytes) +
         ":" + sha256(uv) + ":" + sha256(positions) + ":" + sha256(normals) +
         ":" + sha256(primaryUV);
}
static bool needs_bake_context(Source &source, Object &material,
                               Object &shader) {
  const auto &parsed = source.tree(shader).at("m_ParsedForm");
  if (parsed.value("m_Name", std::string()) == "Special Weapon/Flowmap2")
    return source.tree(material)
               .value("m_ShaderKeywords", std::string())
               .find("EFX_FLIPBOOK_1") != std::string::npos;
  if (parsed.value("m_Name", std::string()) != "Special Weapon/Flowmap1")
    return false;
  float contribution = 1, mode = 0;
  for (auto &p : parsed.at("m_PropInfo").at("m_Props")) {
    if (p.at("m_Name") == "_Flowmap1Contribution")
      contribution = p.value("m_DefValue[0]", 1.f);
    if (p.at("m_Name") == "_ProjectAxis_U")
      mode = p.value("m_DefValue[3]", 0.f);
  }
  for (auto &p : source.tree(material).at("m_SavedProperties").at("m_Colors")) {
    if (p.at("first") == "_Flowmap1Contribution")
      contribution = p.at("second").at("r");
    if (p.at("first") == "_ProjectAxis_U")
      mode = p.at("second").at("a");
  }
  return contribution != 0 && mode == 2;
}
Material convert_material(Source &source, Object &o, const Surface &context,
                          int maxDimension,
                          std::map<std::string, Image> &textureCache,
                          bool preview) {
  require(source.project_color_space() == 1,
          "Gamma project requires a separately verified material adapter");
  require(o.cid == 21 && !placeholder(source, &o),
          "Missing authored material for " + context.name);
  const auto &t = source.tree(o);
  auto *shader = source.ref(o, t.at("m_Shader"));
  require(shader && shader->cid == 48, "Missing material shader");
  const auto &parsed = source.tree(*shader).at("m_ParsedForm");
  std::string shaderName = parsed.at("m_Name");
  if (shaderName == "CODMStandard Eye")
    return convert_eye_material(source, o, maxDimension, textureCache, preview);
  if (shaderName == "CODStandard Hair Optimization" || shaderName == "CODStandard Hair") {
    const auto fingerprint = shaderName == "CODStandard Hair"
        ? "eca696a8dd79d77862c84441247c69493bf2a6842d00d2eb4a9fd475206e70a0"
        : "257d4e7254fbca4c11ad45bf34ebebc3ce7080cf460836b3f952f51b9ae82d3b";
    require(sha256(shader->raw()) == fingerprint,
            "Shader revision differs from verified adapter: " + shaderName);
    const auto &saved = t.at("m_SavedProperties");
    auto scalar = [&](const std::string &key, float fallback) {
      for (const auto &p : saved.at("m_Floats")) if (p.at("first") == key) return p.at("second").get<float>();
      for (const auto &p : parsed.at("m_PropInfo").at("m_Props")) if (p.at("m_Name") == key) return p.value("m_DefValue[0]", fallback);
      return fallback;
    };
    // Unity retains keywords/properties from previous shaders. Only these
    // compiled branches alter static hair color; wind is vertex animation.
    auto keywords = t.value("m_ShaderKeywords", std::string());
    require(keywords.find("_MASK_ON") == std::string::npos && keywords.find("_COVERTEXTURE_UV1") == std::string::npos,
            "Unsupported active hair color overlay");
    J records=J::object();
    auto texture = [&](const std::string &key) {
      for (const auto &p : saved.at("m_TexEnvs")) if (p.at("first") == key) {
        const auto &env=p.at("second");
        if (env.at("m_Texture").at("m_PathID") == 0) break;
        require(env.at("m_Scale").at("x")==1 && env.at("m_Scale").at("y")==1 && env.at("m_Offset").at("x")==0 && env.at("m_Offset").at("y")==0,"Unsupported hair texture UV transform");
        auto *asset=source.ref(o,env.at("m_Texture"));require(asset && asset->cid==28,"Missing authored hair texture: "+key);
        auto cacheKey=asset->id()+":"+std::to_string(maxDimension);
        if(!textureCache.contains(cacheKey)) textureCache[cacheKey]=decode_texture(source,*asset,maxDimension);
        records[key]={{"source",asset->id()},{"name",source.name(*asset)}};
        return textureCache.at(cacheKey);
      }
      if (key == "_AmbientOcclusionRootMap")
        for (const auto &p : parsed.at("m_PropInfo").at("m_Props"))
          if (p.at("m_Name") == key && p.at("m_DefTexture").at("m_DefaultName") == "white") {
            records[key]={{"shaderDefault","white"},{"source",nullptr}};
            return Image{1,1,Bytes{255,255,255,255}};
          }
      throw std::runtime_error("Missing authored hair texture: "+key);
    };
    auto base=texture("_MainTex"), rootMap=texture("_AmbientOcclusionRootMap");
    rootMap=resize_image(rootMap,base.width,base.height);
    V4 tint(1);for(const auto &p:saved.at("m_Colors")) if(p.at("first")=="_Color") tint=rgba(p.at("second"));
    const float cutoff=scalar("_Cutoff",.5f),tips=scalar("_CutoffTips",1);
    std::array<std::array<uint8_t,256>,3> colors;
    for(int k=0;k<3;++k) for(int v=0;v<256;++v) colors[k][v]=quant(srgb(linear(v/255.f)*linear(tint[k])));
    for(size_t i=0;i<base.pixels.size();i+=4) {
      for(int k=0;k<3;++k) base.pixels[i+k]=colors[k][base.pixels[i+k]];
      base.pixels[i+3]=quant(hair_coverage(base.pixels[i+3]/255.f,rootMap.pixels[i+1]/255.f,cutoff,tips));
    }
    Material result;result.id=o.id();result.name=safe_name(source.name(o))+"_"+hex64(hash64(o.id()));result.shader=shaderName;result.maps["albedo"]=std::move(base);
    if(preview) result.previewBaseColor=result.maps.at("albedo");
    result.provenance={{"shader",shaderName},{"shaderSHA256",fingerprint},{"textures",records},{"alphaMode","blend"},{"workflow","Authored hair color with combined depth and forward strand coverage"},{"opacitySource","Opaque above lerp(Cutoff, CutoffTips, RootMap.g); otherwise saturate(MainTex.a / (threshold + .001))"},{"limitations","Anisotropic lighting is not portable to standard CAST. TangentFlowMap is not a normal map and is intentionally omitted."}};
    return result;
  }
  static const std::map<std::string, std::string> verified = {
      {"CODStandard Dynamic Weapon_Pupil",
       "cf528fb4a474c84d31672b881da9146f81c4d7ba4c772bdff7be247f4f144699"},
      {"CODStandard Dynamic",
       "51dc2d86a86b41ed06c2c00fdc13e36133145ff3ec306dce767c331b92fa6ff4"},
      {"CODStandard Skin",
       "3429ea7f0d5ff158cb8496cb0dbf36f0f1e5738c140454c1d7bcecb024c68f40"},
      {"CODM/Character/LobbyHair_BR(Legacy)",
       "6f0131aa2207ac978bf0b078fef03999c25ee8ac85b80d9b68fa86e5fb20fead"},
      {"Special Weapon/Flowmap1",
       "4d1b39693b84cd768a5318ce235a1a1955ec2d34e3224c24c7e184ec5d5fae8a"},
      {"Special Weapon/Flowmap2",
       "6700bb8aa2bf477c31b98c46a1e2c1217c4f2975ed008c3ce7bfa30bc0c9fcef"}};
  require(verified.contains(shaderName),
          "No verified static surface adapter: " + shaderName);
  require(sha256(shader->raw()) == verified.at(shaderName),
          "Shader revision differs from verified adapter: " + shaderName);
  bool skin = shaderName == "CODStandard Skin",
       hair = shaderName == "CODM/Character/LobbyHair_BR(Legacy)",
       pupil = shaderName == "CODStandard Dynamic Weapon_Pupil",
       flow = shaderName == "Special Weapon/Flowmap1",
       flow2 = shaderName == "Special Weapon/Flowmap2";
  std::set<std::string> allowed = {"_DYNAMIC_OPAQUE",
                                   "_DYNAMIC_TRANSPARENT",
                                   "_DYNAMIC_CUTOUT",
                                   "_DYNAMIC_OPAQUE_CHARACTER_ADDLIGHT",
                                   "_DYNAMIC_CUTOUT_CHARACTER_ADDLIGHT",
                                   "_DYNAMIC_EMISSION",
                                   "_GPU_SKINNING"};
  if (skin)
    allowed = {"_NORMALMAP", "_GPU_SKINNING", "_EMISSION"}; // Emitted light is intentionally omitted.
  if (hair)
    allowed = {"_GPU_SKINNING"};
  if (flow2)
    allowed = {"EFX_FLIPBOOK_1"};
  if (pupil)
    allowed = {"_DYNAMIC_TRANSPARENT"};
  if (flow)
    allowed = {"_DYNAMIC_OPAQUE", "_DYNAMIC_OPAQUE_CHARACTER_ADDLIGHT",
               "_DYNAMIC_EMISSION", "_GPU_SKINNING"};
  if (shaderName == "CODStandard Dynamic")
    allowed.insert("_DYNAMIC_TRANSPARENT_EMISSION");
  std::set<std::string> keywords;
  std::set<std::string> ignoredKeywords;
  std::istringstream split(t.value("m_ShaderKeywords", std::string()));
  std::string keyword;
  while (split >> keyword) {
    if (!allowed.contains(keyword)) {
      bool declared = false;
      for (auto &subshader : parsed.at("m_SubShaders"))
        for (auto &pass : subshader.at("m_Passes")) {
          std::set<int> indices;
          for (auto &name : pass.at("m_NameIndices"))
            if (name.at("first") == keyword)
              indices.insert(name.at("second").get<int>());
          for (auto stage : {"progVertex", "progFragment"})
            for (auto &program : pass.at(stage).at("m_SubPrograms"))
              for (auto &index : program.at("m_KeywordIndices"))
                declared |= indices.contains(index.get<int>());
        }
      if (!declared) {
        ignoredKeywords.insert(keyword);
        continue;
      }
    }
    require(allowed.contains(keyword), "Unverified shader keyword: " + keyword);
    keywords.insert(keyword);
  }
  const bool transparentEmission =
      keywords.contains("_DYNAMIC_TRANSPARENT_EMISSION");
  if (transparentEmission)
    require(keywords.size() == 1 + size_t(keywords.contains("_GPU_SKINNING")),
            "Conflicting keywords for transparent-emission surface");
  std::map<std::string, V4> values;
  std::set<std::string> colors;
  for (auto &p : parsed.at("m_PropInfo").at("m_Props")) {
    V4 v;
    for (int i = 0; i < 4; i++)
      v[i] = p.value("m_DefValue[" + std::to_string(i) + "]", 0.f);
    std::string name = p.at("m_Name");
    values[name] = v;
    if (p.at("m_Type") == 0)
      colors.insert(name);
  }
  const auto &saved = t.at("m_SavedProperties");
  for (auto &p : saved.at("m_Floats"))
    values[p.at("first")] = V4(p.at("second").get<float>());
  for (auto &p : saved.at("m_Colors"))
    values[p.at("first")] = rgba(p.at("second"));
  for (auto &name : colors)
    for (int k = 0; k < 3; k++)
      values[name][k] = linear(values[name][k]);
  auto val = [&](const std::string &name, V4 fallback) {
    return values.contains(name) ? values.at(name) : fallback;
  };
  auto scalar = [&](const std::string &name, float fallback) {
    return val(name, V4(fallback)).x;
  };
  auto albedo = skin ? "_MainTex2" : "_MainTex",
       normal = skin   ? "_BumpMap"
                : hair ? "_DefalutNormal"
                       : "_BumpMapPakced",
       mask = skin ? "_mask" : "_MetallicRoughnessMap";
  V4 contribution = val("_Flowmap1Contribution", V4(1, 0, 0, 0));
  const float uniformMetallic = scalar("_Metallic", 0);
  const bool flow2Flipbook = flow2 && keywords.contains("EFX_FLIPBOOK_1");
  const bool flow2Standard = flow2 && keywords.empty();
  const V4 contribution2 = val("_Flowmap2Contribution", V4(0, 1, 0, 0));
  const bool flow2Inactive =
      flow2Standard && contribution.x == 0 && contribution2.x == 0 &&
      scalar("_UseOriginalMetalGloss", 1) == 1 &&
      scalar("_MetalGlossUseFlow1Alpha", 0) == 0 &&
      scalar("_MetalGlossUseFlow2Alpha", 0) == 0 &&
      scalar("_MetalGlossNoMaskSwitch", 0) == 0 &&
      scalar("_UseAlbedoAlphaMetalGlossSwitch", 0) == 0 &&
      val("_UseOriginalAlbedoSetting", V4(0)).x == 0 &&
      val("_UseOriginalAlbedoSetting", V4(0)).y == 0;
  if (transparentEmission) {
    require(std::isfinite(uniformMetallic) && uniformMetallic >= 0 &&
                uniformMetallic <= 1,
            "Transparent-emission metallic value is outside [0,1]");
    require(scalar("_SrcBlend", 5) == 5 && scalar("_DstBlend", 10) == 10,
            "Unsupported transparent-emission blend factors");
  }
  std::map<std::string, Image> images;
  V2 flow2Scale(1), flow2Offset(0);
  V2 flow1Scale(1), flow1Offset(0);
  J textureRecords = J::object();
  for (auto &p : saved.at("m_TexEnvs")) {
    std::string prop = p.at("first");
    const auto &env = p.at("second");
    // Saved properties may retain textures from an inactive shader branch.
    // Resolve only samplers used by the verified static surface conversion.
    bool used = prop == albedo || prop == normal ||
                (!transparentEmission && !hair && prop == mask) ||
                (flow && contribution.x != 0 && prop == "_Flowmap1") ||
                (flow2 && !flow2Inactive &&
                 (prop == "_Flowmap1" || prop == "_Flowmap2")) ||
                (transparentEmission && prop == "_EmissionMap");
    if (!used)
      continue;
    auto *texture = source.ref(o, env.at("m_Texture"));
    if (!texture || texture->cid != 28)
      continue;
    const int colorSpace = source.tree(*texture).at("m_ColorSpace").get<int>();
    if (flow2Standard && (prop == "_Flowmap1" || prop == "_Flowmap2"))
      require(colorSpace == 1, "Unverified flow color texture space: " + prop);
    if (transparentEmission)
      require(colorSpace == (prop == normal ? 0 : 1),
              "Unverified transparent-emission texture color space: " + prop);
    if ((flow || flow2) && prop == "_Flowmap1") {
      flow1Scale = V2(env.at("m_Scale").at("x"), env.at("m_Scale").at("y"));
      flow1Offset = V2(env.at("m_Offset").at("x"), env.at("m_Offset").at("y"));
    } else if (flow2 && prop == "_Flowmap2") {
      flow2Scale = V2(env.at("m_Scale").at("x"), env.at("m_Scale").at("y"));
      flow2Offset = V2(env.at("m_Offset").at("x"), env.at("m_Offset").at("y"));
    } else
      require(env.at("m_Scale").at("x") == 1 &&
                  env.at("m_Scale").at("y") == 1 &&
                  env.at("m_Offset").at("x") == 0 &&
                  env.at("m_Offset").at("y") == 0,
              "Unverified texture UV transform: " + prop);
    auto key = texture->id() + ":" + std::to_string(maxDimension);
    if (!textureCache.contains(key))
      textureCache[key] = decode_texture(source, *texture, maxDimension);
    images[prop] = textureCache.at(key);
    textureRecords[prop] = {{"source", texture->id()},
                            {"name", source.name(*texture)},
                            {"contentSHA256", sha256(images[prop].pixels)},
                            {"width", images[prop].width},
                            {"height", images[prop].height},
                            {"sourceColorSpace", colorSpace},
                            {"scale", env.at("m_Scale")},
                            {"offset", env.at("m_Offset")}};
  }
  for (auto key : {albedo, normal,
                   transparentEmission ? "_EmissionMap"
                   : hair              ? normal
                                       : mask}) {
    if (!images.contains(key)) {
      bool assigned = false;
      for (auto &env : saved.at("m_TexEnvs"))
        if (env.at("first") == key)
          assigned = env.at("second").at("m_Texture").at("m_PathID") != 0;
      // A null sampler uses its shader-declared constant texture. Never
      // substitute for an assigned but missing/unsupported texture asset.
      if (!assigned)
        for (auto &property : parsed.at("m_PropInfo").at("m_Props"))
          if (property.at("m_Name") == key &&
              property.contains("m_DefTexture")) {
            auto name = property.at("m_DefTexture")
                            .value("m_DefaultName", std::string());
            if (name == "white" || name == "black") {
              uint8_t color = name == "white" ? 255 : 0;
              images[key] = Image{1, 1, Bytes{color, color, color, 255}};
              textureRecords[key] = {{"source", nullptr},
                                     {"shaderDefault", name},
                                     {"width", 1},
                                     {"height", 1},
                                     {"evidence",
                                      "Unassigned sampler; declared default of "
                                      "fingerprinted shader"}};
            }
          }
    }
    require(images.contains(key),
            shaderName + ": required texture missing " + key);
  }
  int w = images.at(albedo).width, h = images.at(albedo).height;
  auto packed = resize_image(images.at(normal), w, h);
  Image masks;
  if (!transparentEmission && !hair)
    masks = resize_image(images.at(mask), w, h);
  auto &baseImage = images.at(albedo);
  std::vector<V4> flow2Pixels;
  if (flow2Flipbook) {
    require(keywords == std::set<std::string>{"EFX_FLIPBOOK_1"},
            "Unverified Flowmap2 variant");
    for (const auto &[key, expected] :
         std::map<std::string, float>{{"_EffectOption", 0},
                                      {"_AddScale", 1},
                                      {"_MulScale", 0},
                                      {"_CoverScale", 0},
                                      {"_UseOriginalMetalGloss", 1},
                                      {"_SplitMask", 1},
                                      {"_MetalGlossNoMaskSwitch", 0},
                                      {"_UseAlbedoAlphaMetalGlossSwitch", 0},
                                      {"_MetalGlossUseFlow1Alpha", 0},
                                      {"_MetalGlossUseFlow2Alpha", 1},
                                      {"_Flowmap1UseNoMask", 0},
                                      {"_FlowmapNormalDistort1", 0},
                                      {"_FlowmapNormalDistort2", 0},
                                      {"_SrcBlend", 1},
                                      {"_DstBlend", 0}})
      require(scalar(key, expected) == expected,
              "Unverified Flowmap2 branch: " + key);
    for (const auto &[key, expected] :
         std::map<std::string, V4>{{"_Flowmap2Contribution", V4(1, 0, 0, 0)},
                                   {"_ChannelIntensity0", V4(1, 0, 0, 0)},
                                   {"_ChannelIntensity1", V4(0, 1, 0, 0)},
                                   {"_MultiSwitch", V4(0)},
                                   {"_FlowRotationSinCos", V4(0, 1, 0, 1)},
                                   {"_ProjectAxis_U", V4(1, 0, 0, 0)},
                                   {"_ProjectAxis_U2", V4(1, 0, 0, 1)},
                                   {"_ProjectAxis_V2", V4(0, 0, 1, 0)},
                                   {"_ChannelR2", V4(1, 0, 0, 1)},
                                   {"_ChannelG2", V4(0, 1, 0, 1)},
                                   {"_ChannelB2", V4(0, 0, 1, 1)},
                                   {"_FlipbookParam1", V4(4, 3, .125f, 0)}})
      require(val(key, expected) == expected,
              "Unverified Flowmap2 vector: " + key);
    require(contribution.x == 0 && contribution.z == 0 &&
                val("_UseOriginalAlbedoSetting", V4(0)).x == 0 &&
                val("_UseOriginalAlbedoSetting", V4(0)).y == 0,
            "Unverified Flowmap2 original-color blend");
    require(images.contains("_Flowmap1") && images.contains("_Flowmap2"),
            "Missing Flowmap2 layers");
    require(context.sourcePositions.size() == context.positions.size() &&
                context.sourceNormals.size() == context.positions.size(),
            "Missing object-space projection data");
    Surface projection = context;
    projection.uv1.clear();
    std::vector<float> facing;
    for (size_t i = 0; i < context.positions.size(); i++) {
      auto p = context.sourcePositions[i];
      projection.uv1.push_back(V2(p.x, p.z) * flow2Scale + flow2Offset);
      facing.push_back(std::clamp(
          1 + contribution.w * (std::abs(context.sourceNormals[i].y) - 1), 0.f,
          1.f));
    }
    std::vector<float> importance(size_t(w) * h);
    for (size_t i = 0; i < importance.size(); i++)
      importance[i] = masks.pixels[i * 4 + 3] / 255.f;
    // The fingerprinted flipbook shader at t=0 selects column 0, row 1.
    // Clamp projected UVs before tile selection, then convert Unity's V axis.
    flow2Pixels =
        bake_overlay(images.at("_Flowmap2"), projection, w, h, importance,
                     facing, V2(.25f, -1.f / 3), V2(0, 2.f / 3), true);
  }
  if (flow2Standard && !flow2Inactive) {
    require(contribution.z == 0 && contribution.w == 0 &&
                contribution2.z == 0 && contribution2.w == 0 &&
                (scalar("_SplitMask", 0) == 0 || scalar("_SplitMask", 0) == 1),
            "Unverified Flowmap2 directional mask or split mask");
    require(scalar("_EffectOption", 0) == 0 && scalar("_AddScale", 1) == 1 &&
                scalar("_MulScale", 0) == 0 && scalar("_CoverScale", 0) == 0,
            "Unverified Flowmap2 composition mode");
    require(val("_ProjectAxis_U", V4(0)).w == 0 &&
                val("_ProjectAxis_U2", V4(0)).w == 0 &&
                val("_MultiSwitch", V4(0)) == V4(0) &&
                val("_FlowRotationSinCos", V4(0, 1, 0, 1)) == V4(0, 1, 0, 1) &&
                val("_UseOriginalAlbedoSetting", V4(0)).x == 0 &&
                val("_UseOriginalAlbedoSetting", V4(0)).y == 0,
            "Unverified Flowmap2 UV or albedo mode");
    for (auto key :
         {"_MetalGlossNoMaskSwitch", "_MetalGlossUseFlow1Alpha",
          "_MetalGlossUseFlow2Alpha", "_UseAlbedoAlphaMetalGlossSwitch"})
      require(scalar(key, 0) == 0,
              std::string("Unverified Flowmap2 branch: ") + key);
    require(images.contains("_Flowmap1") && images.contains("_Flowmap2"),
            "Missing Flowmap2 layers");
  }
  float uvMode = val("_ProjectAxis_U", V4(0)).w;
  std::vector<V4> overlayPixels;
  if (flow && contribution.x != 0) {
    for (auto &[key, expected] :
         std::map<std::string, float>{{"_MetalGlossNoMaskSwitch", 0},
                                      {"_MetalGlossUseFlow1Alpha", 0},
                                      {"_UseAlbedoAlphaMetalGlossSwitch", 0},
                                      {"_FlowmapNormalDistort1", 0}})
      require(scalar(key, expected) == expected,
              "Unverified Flowmap1 branch: " + key);
    require(contribution.z == 0 && (uvMode == 0 || uvMode == 2) &&
                val("_MultiSwitch", V4(0)).x == 0,
            "Unverified Flowmap contribution or projected UV branch");
    auto speed = val("_FlowmapSpeed1", V4(0)),
         rot = val("_FlowRotationSinCos", V4(0, 1, 0, 1)),
         orig = val("_UseOriginalAlbedoSetting", V4(0));
    require(rot.x == 0 && rot.y == 1 && orig.x == 0 && orig.y == 0,
            "Animated Flowmap UVs cannot be baked as a static CAST surface");
    require(images.contains("_Flowmap1"), "Missing Flowmap1 texture");
    if (uvMode == 2) {
      require(flow1Scale == V2(1) && flow1Offset == V2(0),
              "Unverified projected Flowmap1 UV transform");
      std::vector<float> importance(size_t(w) * h);
      float split = scalar("_SplitMask", 0);
      require(split == 0 || split == 1, "Unverified Flowmap split mask");
      float extra = val("_ChannelIntensity0", V4(1, 0, 0, 0)).y;
      for (size_t i = 0; i < importance.size(); i++) {
        auto pm = &masks.pixels[i * 4];
        float g = pm[1] / 255.f;
        if (split)
          g = std::clamp((g <= .5f ? g : g - .6f) * 2.5f, 0.f, 1.f);
        importance[i] = g + pm[3] / 255.f * extra;
      }
      overlayPixels =
          bake_overlay(images.at("_Flowmap1"), context, w, h, importance);
    }
  }
  Material out;
  out.id =
      o.id() + (flow2Flipbook || (flow && contribution.x != 0 && uvMode == 2)
                    ? ":" + bake_context(context, flow2)
                    : "");
  out.name = safe_name(source.name(o)) + "_" + hex64(hash64(out.id));
  out.shader = shaderName;
  if (!preview) for (auto key : {"albedo", "specular", "normal", "gloss", "ao"})
    out.maps[key] = Image{w, h, Bytes(size_t(w) * h * 4)};
  if (preview)
    out.previewBaseColor = Image{w, h, Bytes(size_t(w) * h * 4)};
  auto *diffuseOut = preview ? nullptr : out.maps.at("albedo").pixels.data();
  auto *specularOut = preview ? nullptr : out.maps.at("specular").pixels.data();
  auto *normalOut = preview ? nullptr : out.maps.at("normal").pixels.data();
  auto *glossOut = preview ? nullptr : out.maps.at("gloss").pixels.data();
  auto *aoOut = preview ? nullptr : out.maps.at("ao").pixels.data();
  const auto colorValue = val("_Color", skin ? V4(.25f) : V4(1));
  const auto colorTint = V3(colorValue);
  const bool transparent =
      !pupil &&
      (keywords.contains("_DYNAMIC_TRANSPARENT") || transparentEmission);
  const Image *emissionImage =
      transparentEmission ? &images.at("_EmissionMap") : nullptr;
  const V3 emissionRed(val("_EmissionColor", V4(0))),
      emissionGreen(val("_EmissionColor_1", V4(0))),
      emissionBlue(val("_EmissionColor_2", V4(0))),
      emissionTint(val("_ChangeEmissionColor", V4(1)));
  const auto skinMultiplier = 2.f * glm::sqrt(glm::max(colorTint, V3(0)));
  const auto specularTint = V3(val("_skinSpecTint", V4(1)));
  const float pupilAlpha = std::clamp(scalar("_AimTransparency", 1), 0.f, 1.f);
  const float smoothScale = scalar("_smoothScale", 1),
              aoStrength = scalar("_aoStrengh", 1);
  const bool cutout = keywords.contains("_DYNAMIC_CUTOUT");
  const float cutoff = scalar("_Cutoff", .5f);
  const bool flowActive = flow && contribution.x != 0;
  const Image *flowImage = flowActive ? &images.at("_Flowmap1") : nullptr;
  // Material uniforms are constant across the image. Looking them up in the
  // string-keyed property map for every pixel dominated active overlay export.
  const V3 flowRed(val("_ChannelR1", V4(1, 0, 0, 1))),
      flowGreen(val("_ChannelG1", V4(0, 1, 0, 1))),
      flowBlue(val("_ChannelB1", V4(0, 0, 1, 1)));
  const float flowScale = scalar("_FlowmapScale1", 1),
              flowSplit = scalar("_SplitMask", 0),
              flowEnable = scalar("_EnableFlowSetting", 1),
              flowNoMask = scalar("_Flowmap1UseNoMask", 0),
              flowBlend = scalar("_BlendScale", 1);
  const V4 flowIntensity = val("_ChannelIntensity0", V4(1, 0, 0, 0));
  require(!flowActive || flowSplit == 0 || flowSplit == 1,
          "Unverified Flowmap split mask");
  const auto pixelsStart = Clock::now();
  const float flow2Blend = scalar("_BlendScale", 1),
              flow2ColorScale = scalar("_FlowmapScale2", 1),
              flow2Metal = val("_UseFlowAlphaMetallic", V4(1)).y,
              flow2Gloss = val("_UseFlowAlphaGlossiness", V4(1)).y;
  const float hairBumpFactor = scalar("_BumpFactor", 1);
  const float originalMetalGloss = scalar("_UseOriginalMetalGloss", 1);
  const V2 blendMetal(val("_blendMetallic", V4(0))),
      blendGloss(val("_blendGlossiness", V4(.5f)));
  const V2 secondIntensity(val("_ChannelIntensity1", V4(0, 1, 0, 0)));
  const V3 secondRed(val("_ChannelR2", V4(1, 0, 0, 1))),
      secondGreen(val("_ChannelG2", V4(0, 1, 0, 1))),
      secondBlue(val("_ChannelB2", V4(0, 0, 1, 1)));
  const float distort1 = scalar("_FlowmapNormalDistort1", 0),
              distort2 = scalar("_FlowmapNormalDistort2", 0);
  auto transformedUV = [](V2 uv, V2 scale, V2 offset, V2 distortion) {
    V2 sourceUV(uv.x, 1 - uv.y);
    sourceUV = sourceUV * scale + offset + distortion;
    return V2(sourceUV.x, 1 - sourceUV.y);
  };
  for (size_t i = 0; i < size_t(w) * h; i++) {
    auto *color = &baseImage.pixels[i * 4];
    auto *pn = &packed.pixels[i * 4];
    auto *pm = transparentEmission || hair ? nullptr : &masks.pixels[i * 4];
    V3 base(linear(color[0] / 255.f), linear(color[1] / 255.f),
            linear(color[2] / 255.f));
    double nx = pn[0] / 127.5 - 1, ny = pn[1] / 127.5 - 1,
           nz = std::sqrt(std::max(.001, 1 - std::min(1., nx * nx + ny * ny)));
    float metal = transparentEmission ? uniformMetallic
                  : hair              ? 0
                                      : pm[2] / 255.f,
          gloss = pn[2] / 255.f,
          ao = transparentEmission ? float(nz)
               : hair              ? 1
                                   : pm[0] / 255.f,
          alpha = 1;
    if (hair) {
      base *= colorTint;
      nx *= hairBumpFactor;
      ny *= hairBumpFactor;
      nz = std::sqrt(std::max(0., 1 - std::min(1., nx * nx + ny * ny)));
      const float coverage =
          std::clamp(color[3] / 255.f * colorValue.w, 0.f, 1.f);
      // Installed DEPTH pass writes opaque coverage above _Cutoff;
      // FORWARD blends remaining coverage at alpha / 0.7 below 0.7.
      alpha = coverage >= cutoff ? 1.f : coverage <= .7f ? coverage / .7f : 0.f;
    }
    if (transparent) {
      base *= colorTint;
      alpha = std::clamp(color[3] / 255.f * colorValue.w, 0.f, 1.f);
    }
    if (transparentEmission) {
      // Fingerprinted Dynamic fragment programs use scalar metallic and
      // reconstructed tangent-normal Z for ambient occlusion. Emission RGB
      // also raises opacity; retain that alpha contribution without adding
      // emitted light to the exported albedo or writing an emissive map.
      V3 e;
      if (emissionImage->width == w && emissionImage->height == h) {
        auto *pixel = &emissionImage->pixels[i * 4];
        e = V3(linear(pixel[0] / 255.f), linear(pixel[1] / 255.f),
               linear(pixel[2] / 255.f));
      } else
        e = V3(sample_linear_overlay(*emissionImage,
                                     V2((i % w + .5f) / w, (i / w + .5f) / h)));
      auto emission =
          (e.r * emissionRed + e.g * emissionGreen + e.b * emissionBlue) *
          emissionTint;
      alpha = std::clamp(
          std::max(alpha, std::min(glm::dot(emission, V3(1.f / 3.f)), 1.f)),
          0.f, 1.f);
    }
    if (pupil) {
      base *= colorTint;
      alpha = pupilAlpha;
    }
    if (skin) {
      base *= skinMultiplier;
      gloss *= smoothScale;
      ao = 1 + (ao - 1) * aoStrength;
    }
    if (flowActive) {
      V2 uv((i % w + .5f) / w, (i / w + .5f) / h);
      auto flowPixel =
          uvMode == 2 ? overlayPixels[i]
                      : sample_linear_overlay(
                            *flowImage,
                            transformedUV(uv, flow1Scale, flow1Offset, V2(0)));
      V3 fc(flowPixel);
      fc = fc.r * flowRed + fc.g * flowGreen + fc.b * flowBlue;
      fc *= flowScale;
      float g = pm[1] / 255.f;
      if (flowSplit)
        g = std::clamp((g <= .5f ? g : g - .6f) * 2.5f, 0.f, 1.f);
      g *= flowEnable;
      g += flowNoMask * (1 - g);
      float strength = flowBlend *
                       (g * flowIntensity.x + pm[3] / 255.f * flowIntensity.y) *
                       flowPixel.a;
      float amount = std::clamp(strength, 0.f, 1.f) * contribution.x;
      base += (glm::clamp(fc, V3(0), V3(1)) - base) * amount;
      float surfaceBlend =
          std::clamp(strength, 0.f, 1.f) * (1 - originalMetalGloss);
      V2 materialMask(g, pm[3] / 255.f);
      metal = glm::mix(metal,
                       std::clamp(glm::dot(blendMetal, materialMask), 0.f, 1.f),
                       surfaceBlend);
      gloss = glm::mix(gloss,
                       std::clamp(glm::dot(blendGloss, materialMask), 0.f, 1.f),
                       surfaceBlend);
    }
    if (flow2Standard && !flow2Inactive) {
      V2 uv((i % w + .5f) / w, (i / w + .5f) / h);
      auto first = sample_linear_overlay(
          images.at("_Flowmap1"),
          transformedUV(uv, flow1Scale, flow1Offset, V2(nx, ny) * distort1));
      auto second = sample_linear_overlay(
          images.at("_Flowmap2"),
          transformedUV(uv, flow2Scale, flow2Offset, V2(nx, ny) * distort2));
      float g = pm[1] / 255.f;
      if (flowSplit)
        g = std::clamp((g <= .5f ? g : g - .6f) * 2.5f, 0.f, 1.f);
      g += flowNoMask * (1 - g);
      V2 maskWeights(g, pm[3] / 255.f);
      float w1 = glm::dot(maskWeights, V2(flowIntensity)),
            w2 = glm::dot(maskWeights, secondIntensity);
      V3 c1 = (first.r * flowRed + first.g * flowGreen + first.b * flowBlue) *
              flowScale * w1;
      V3 c2 = (second.r * secondRed + second.g * secondGreen +
               second.b * secondBlue) *
              flow2ColorScale * w2;
      V3 overlay = glm::clamp(
          c1 * contribution.x + c2 * contribution2.x * second.a, V3(0), V3(1));
      float strength = std::clamp(
          glm::dot(maskWeights, glm::max(V2(flowIntensity) * first.a,
                                         secondIntensity * second.a)) *
              flowBlend,
          0.f, 1.f);
      float coverage = std::clamp(std::max(w1 * contribution.x * first.a,
                                           w2 * contribution2.x * second.a),
                                  0.f, 1.f) *
                       strength;
      base = glm::mix(base, overlay, coverage);
      float materialBlend = strength * (1 - originalMetalGloss);
      metal = glm::mix(metal,
                       std::clamp(glm::dot(blendMetal, maskWeights), 0.f, 1.f),
                       materialBlend);
      gloss = glm::mix(gloss,
                       std::clamp(glm::dot(blendGloss, maskWeights), 0.f, 1.f),
                       materialBlend);
    }
    if (flow2Flipbook) {
      V2 uv((i % w + .5f) / w, (i / w + .5f) / h);
      float a1 = sample_linear_overlay(images.at("_Flowmap1"), uv).a;
      const auto f2 = flow2Pixels[i];
      float g = pm[1] / 255.f, maskAlpha = pm[3] / 255.f;
      g = std::clamp((g <= .5f ? g : g - .6f) * 2.5f, 0.f, 1.f);
      float strength =
          std::clamp((g * a1 + maskAlpha * f2.a) * flow2Blend, 0.f, 1.f);
      float coverage = std::clamp(maskAlpha * f2.a, 0.f, 1.f) * strength;
      auto overlay =
          glm::clamp(V3(f2) * flow2ColorScale * maskAlpha * f2.a, V3(0), V3(1));
      base = glm::mix(base, overlay, coverage);
      float materialBlend = maskAlpha * maskAlpha * f2.a;
      metal = glm::mix(metal, flow2Metal, materialBlend);
      gloss = glm::mix(gloss, flow2Gloss, materialBlend);
    }
    if (preview) {
      if (cutout) alpha = color[3] / 255.f >= cutoff ? 1.f : 0.f;
      for (int k = 0; k < 3; ++k) out.previewBaseColor.pixels[i * 4 + k] = quant(srgb(base[k]));
      out.previewBaseColor.pixels[i * 4 + 3] = quant(alpha);
      continue;
    }
    V3 spec = .04f * (1 - metal) + base * metal;
    if (skin)
      spec *= specularTint;
    V3 diff = base * (1 - metal);
    if (cutout)
      alpha = color[3] / 255.f >= cutoff ? 1.f : 0.f;
    auto normal3 = glm::normalize(DV3(nx, ny, nz));
    for (int k = 0; k < 3; k++) {
      if (preview)
        out.previewBaseColor.pixels[i * 4 + k] = quant(srgb(base[k]));
      diffuseOut[i * 4 + k] = quant(srgb(diff[k]));
      specularOut[i * 4 + k] = quant(srgb(spec[k]));
      normalOut[i * 4 + k] = quant((normal3[k] + 1) / 2);
      glossOut[i * 4 + k] = quant(gloss);
      aoOut[i * 4 + k] = quant(ao);
    }
    if (preview)
      out.previewBaseColor.pixels[i * 4 + 3] = quant(alpha);
    diffuseOut[i * 4 + 3] = quant(alpha);
    specularOut[i * 4 + 3] = quant(gloss);
    normalOut[i * 4 + 3] = glossOut[i * 4 + 3] = aoOut[i * 4 + 3] = 255;
  }
  if (preview) out.maps["albedo"] = out.previewBaseColor;
  J effective = J::object();
  const double pixelsSeconds = seconds(pixelsStart);
  for (auto &[key, v] : values)
    effective[key] = {v.x, v.y, v.z, v.w};
  out.provenance = {
      {"source", o.id()},
      {"material", t},
      {"shaderName", shaderName},
      {"ignoredUndeclaredKeywords", ignoredKeywords},
      {"shaderSource", shader->id()},
      {"shaderSHA256", verified.at(shaderName)},
      {"textures", textureRecords},
      {"effectiveUniforms", effective},
      {"sourceMesh", context.meshId},
      {"sourceSlot", context.submesh},
      {"workflow", "linear specular/gloss"},
      {"specularAlpha",
       "Gloss duplicated in alpha for Cadence; separate gloss map retained"},
      {"normalConvention",
       "OpenGL +Y; reconstructed positive Z with source clamp 0.001"},
      {"emission", false},
      {"previewMaximumDimension", maxDimension},
      {"pixelConversionSeconds", pixelsSeconds},
      {"limitations",
       J::array({"Static surface channels only; game lighting and runtime "
                 "shader effects are not reproduced."})}};
  if (pupil)
    out.provenance["limitations"].push_back(
        "Camera-dependent pupil masking and aim fade are not baked.");
  if (flow2Flipbook) {
    out.provenance["surfaceBranch"] =
        "Flowmap2 / EFX_FLIPBOOK_1, static frame at time zero";
    out.provenance["limitations"].push_back(
        "Projected flipbook artwork is baked at time zero; "
        "texture animation and emitted RGB are omitted.");
  }
  if (flow2Standard || flow) {
    out.provenance["surfaceBranch"] =
        flow2Inactive ? "Inactive flow layers; authored base surface"
                      : "Static flow surface at time zero";
    out.provenance["limitations"].push_back(
        "Flow motion is sampled at time zero; emission remains disabled.");
  }
  if (transparentEmission) {
    out.provenance["surfaceBranch"] =
        "CODStandard Dynamic / _DYNAMIC_TRANSPARENT_EMISSION";
    out.provenance["metallicSource"] =
        "_Metallic scalar; metallic/AO texture is not sampled";
    out.provenance["aoSource"] =
        "Reconstructed tangent-normal Z before normalization";
    out.provenance["opacitySource"] =
        "max(MainTex.a * Color.a, min(mean(emission-channel tint sum "
        "* ChangeEmissionColor.rgb),1))";
    out.provenance["emissionTextureUsage"] =
        "Opacity only; emission color output remains disabled";
    out.provenance["limitations"].push_back(
        "Source emission contributes to opacity, but emitted light is omitted. "
        "Source blend "
        "factors are SrcAlpha / OneMinusSrcAlpha; the CAST consumer must honor "
        "albedo alpha.");
  } else if (transparent)
    out.provenance["limitations"].push_back(
        "Static transparency uses MainTex alpha times Color alpha. Runtime "
        "one-pass fading and "
        "lighting are not baked.");
  out.provenance["alphaMode"] = (transparent || pupil) ? "blend"
                                : cutout               ? "mask"
                                                       : "opaque";
  out.provenance["projectColorSpace"] =
      "Linear; read from installed PlayerSettings";
  if (skin)
    out.provenance["limitations"].push_back(
        "Skin SSS and the second specular lobe require consumer support.");
  if (hair) {
    out.maps.erase("specular");
    out.maps.erase("gloss");
    out.maps.erase("ao");
    out.provenance["workflow"] = "Static hair albedo and tangent normal";
    out.provenance["alphaMode"] = "blend";
    out.provenance["opacitySource"] =
        "Combined source depth cutoff and forward alpha/0.7 coverage";
    out.provenance.erase("specularAlpha");
    out.provenance["normalConvention"] =
        "OpenGL +Y; source XY times _BumpFactor, reconstructed positive Z";
    out.provenance["limitations"].push_back(
        "Hair anisotropic lobes, strand noise and game lighting are not "
        "represented by "
        "specular/gloss maps.");
  }
  return out;
}
// Only adapter failures permit approximation. Missing references, corrupt textures
// and unresolved material assignments must still fail normally.
static bool unsupported_shader(const std::string &reason) {
  return reason.starts_with("No verified static surface adapter:") ||
         reason.starts_with("Shader revision differs from verified adapter:") ||
         reason.starts_with("Unverified shader keyword:") ||
         reason.starts_with("Unsupported active hair") ||
         reason.starts_with("Unsupported hair texture UV") ||
         reason.find(": required texture missing ") != std::string::npos ||
         reason.starts_with("Unverified Flowmap") ||
         reason.starts_with("Unverified projected Flowmap");
}

// An unassigned extra draw slot can still display its component's base atlas.
// This is deliberately reported as an approximation, not a verified slot binding.
std::set<std::string> component_material_candidates(const std::string &meshName,
                                                   const J &candidates) {
  auto stem = std::regex_replace(lower(meshName),
      std::regex("(_(1p|3p|pov|ui|lod[0-9]+))+$"), "");
  // Component numbering is sometimes absent from the material family. Strip
  // only that prefix's number, never digits belonging to the weapon/variant.
  static const std::regex componentNumber(
      "^(wea|mag|sto|rai|bar|sig|gri|grip|iro|muz|sco|guide|tri|sra)[0-9]+(up)?_");
  auto family = [&](const std::string &name) {
    return material_name_key(std::regex_replace(name, componentNumber, "$1$2_"));
  };
  std::set<std::string> matches, familyMatches;
  for (const auto &candidate : candidates) {
    auto name = std::regex_replace(lower(candidate.at("name")),
                                   std::regex("_(mtl|mat)$"), "");
    if (material_name_key(name) == material_name_key(stem))
      matches.insert(candidate.at("id"));
    if (family(name) == family(stem))
      familyMatches.insert(candidate.at("id"));
  }
  return matches.empty() ? familyMatches : matches;
}
static std::string component_base_candidate(Source &source, Object &mesh,
                                             const J &candidates) {
  return equivalent_material(source, component_material_candidates(source.name(mesh), candidates));
}

static Material base_texture_fallback(Source &source, Object &material,
                                      int maxDimension,
                                      std::map<std::string, Image> &cache,
                                      const std::string &reason, bool character = false) {
  const auto &tree = source.tree(material);
  auto *shader = source.ref(material, tree.at("m_Shader"));
  require(shader && shader->cid == 48, "Missing material shader");
  const auto &parsed = source.tree(*shader).at("m_ParsedForm");
  const auto &saved = tree.at("m_SavedProperties");
  const int additive=additive_source_factor(parsed,saved);
  const J *base = nullptr;
  std::string baseProperty = "_MainTex";
  for (auto name : {"_MainTex", "_BaseMap", "_BaseColorMap", "_MainTex2", "_AlbedoMap"}) {
    if (!character && std::string(name) != "_MainTex") break;
    bool declared = !character;
    for (const auto &p : parsed.at("m_PropInfo").at("m_Props")) declared |= p.at("m_Name") == name;
    if (!declared) continue;
    for (const auto &property : saved.at("m_TexEnvs"))
      if (property.at("first") == name && property.at("second").at("m_Texture").at("m_PathID") != 0) {
        base = &property.at("second"); baseProperty = name; break;
      }
    if (base) break;
  }
  require(base, "Shader fallback has no authored base-color texture: " + source.name(material));
  auto *texture = source.ref(material, base->at("m_Texture"));
  require(texture && texture->cid == 28,
          "Shader fallback has no available base texture: " + source.name(material));
  require(lower(source.name(*texture)).find("emptydiffuse") == std::string::npos,
          "Shader fallback base texture is a placeholder: " + source.name(material));
  require(character || additive || (base->at("m_Scale").at("x") == 1 && base->at("m_Scale").at("y") == 1 &&
              base->at("m_Offset").at("x") == 0 && base->at("m_Offset").at("y") == 0),
          "Shader fallback requires an identity base texture UV transform");
  const auto cacheKey = texture->id() + ":" + std::to_string(maxDimension);
  if (!cache.contains(cacheKey))
    cache[cacheKey] = decode_texture(source, *texture, maxDimension);
  Material out;
  out.id = material.id();
  out.name = safe_name(source.name(material)) + "_" + hex64(hash64(out.id));
  out.shader = source.tree(*shader).at("m_ParsedForm").at("m_Name");
  auto image = cache.at(cacheKey);
  // Custom shaders often pack non-opacity data in base alpha.
  for (size_t i = 3; i < image.pixels.size(); i += 4)
    image.pixels[i] = 255;
  out.maps["albedo"] = image;
  out.previewBaseColor = image;
  out.provenance = {
      {"fallback", "authored base texture only"}, {"fallbackReason", reason},
      {"sourceMaterial", material.id()}, {"shader", out.shader},
      {"shaderSource", shader->id()}, {"alphaMode", "opaque"}, {"emission", false},
      {"textures", {{"_MainTex", {{"source", texture->id()},
                                    {"name", source.name(*texture)},
                                    {"width", image.width}, {"height", image.height}}}}},
      {"limitations", J::array({"Unsupported shader: using authored base RGB only. "
          "Shader effects, tint, transparency, normal and specular channels are omitted."})}};
  if (character || additive) {
    V4 tint(1); float cutoff=.5f;
    std::string tintProperty;bool hasCutoff=false;
    const auto &properties=parsed.at("m_PropInfo").at("m_Props");
    for (auto name : {"_MainColor","_TintColor","_BaseColor","_Color"}) {
      for (const auto &p : properties) if (p.at("m_Name")==name && p.at("m_Type")==0) {
        tintProperty=name;
        for(int k=0;k<4;++k) tint[k]=p.value("m_DefValue["+std::to_string(k)+"]",1.f);
        break;
      }
      if (!tintProperty.empty()) break;
    }
    for (const auto &p : properties) if(p.at("m_Name")=="_Cutoff") {hasCutoff=true;cutoff=p.value("m_DefValue[0]",.5f);}
    // Ignore saved colors/floats inherited from a previous shader.
    for (const auto &p : saved.at("m_Colors")) if (p.at("first")==tintProperty) tint=rgba(p.at("second"));
    for (const auto &p : saved.at("m_Floats")) if (hasCutoff && p.at("first")=="_Cutoff") cutoff=p.at("second");
    std::string renderType;
    auto tags = [&](const J &rows) {
      for (const auto &p : rows) {
        auto name=p.is_array()?p.at(0):p.at("first");
        if (name=="RenderType") renderType=(p.is_array()?p.at(1):p.at("second")).get<std::string>();
      }
    };
    if (!parsed.at("m_SubShaders").empty()) tags(parsed.at("m_SubShaders").at(0).at("m_Tags").at("tags"));
    if (tree.contains("stringTagMap")) tags(tree.at("stringTagMap"));
    auto keys=tree.value("m_ShaderKeywords",std::string());
    bool cutout=!additive && (renderType=="TransparentCutout" || keys.find("_DYNAMIC_CUTOUT")!=std::string::npos);
    bool blend=additive || (!cutout && (renderType=="Transparent" || keys.find("_DYNAMIC_TRANSPARENT")!=std::string::npos));
    V2 scale(base->at("m_Scale").at("x"),base->at("m_Scale").at("y"));
    V2 offset(base->at("m_Offset").at("x"),base->at("m_Offset").at("y"));
    auto original=cache.at(cacheKey);
    auto scalar=[&](const std::string &name,float fallback){for(const auto &p:saved.at("m_Floats"))if(p.at("first")==name)return p.at("second").get<float>();return fallback;};
    bool particleAdd=additive && parsed.at("m_Name")=="CODM/FX/MSParticle_Add" && sha256(shader->raw())=="87b43dbd218c95dbda5cad72155a0273f503346aee5efd648b39ac587625902e";
    const Image *mask=nullptr,*twist=nullptr;V2 maskScale(1),maskOffset(0),twistScale(1),twistOffset(0);
    auto sampler=[&](const char *name,V2 &st,V2 &offset)->const Image* {
      for(const auto &p:saved.at("m_TexEnvs"))if(p.at("first")==name) {
        const auto &env=p.at("second");
        if(env.at("m_Texture").at("m_PathID")==0) {
          for(const auto &prop:properties)if(prop.at("m_Name")==name && prop.value("m_DefTexture",J::object()).value("m_DefaultName",std::string())=="white") {
            cache.try_emplace("effect-default-white",Image{1,1,Bytes{255,255,255,255}});return &cache.at("effect-default-white");
          }
        }
        auto *tex=source.ref(material,env.at("m_Texture"));require(tex && tex->cid==28,std::string("Missing active effect texture: ")+name);
        auto key=tex->id()+":"+std::to_string(maxDimension);if(!cache.contains(key))cache[key]=decode_texture(source,*tex,maxDimension);
        st=V2(env.at("m_Scale").at("x"),env.at("m_Scale").at("y"));offset=V2(env.at("m_Offset").at("x"),env.at("m_Offset").at("y"));
        out.provenance["textures"][name]={{"source",tex->id()},{"name",source.name(*tex)}};return &cache.at(key);
      }
      throw std::runtime_error(std::string("Missing active effect sampler: ")+name);
    };
    if(particleAdd && keys.find("_MASK_ON")!=std::string::npos)mask=sampler("_MaskTex",maskScale,maskOffset);
    if(particleAdd && keys.find("_TWIST")!=std::string::npos)twist=sampler("_TwistTex",twistScale,twistOffset);
    float brightness=particleAdd?scalar("_Brightness",2):1;
    V2 twistAmounts(scalar("_TwistScaleX",0),scalar("_TwistScale",0));
    V3 linearTint;for(int k=0;k<3;++k) linearTint[k]=linear(tint[k]);
    for(int y=0;y<image.height;++y) for(int x=0;x<image.width;++x) {
      V2 uv((x+.5f)/image.width,(y+.5f)/image.height);auto sampleUV=uv*scale+offset;
      if(twist){auto t=sample_effect_data(*twist,uv*twistScale+twistOffset);sampleUV+=V2(t)*twistAmounts;}
      auto value=sample_linear_overlay(original,sampleUV);
      if(mask)value.a*=sample_effect_data(*mask,uv*maskScale+maskOffset).r;
      size_t i=(size_t(y)*image.width+x)*4;
      V3 color=V3(value)*linearTint*brightness;
      if(additive) {
        auto straight=straight_alpha_from_additive(color*(additive==5?value.a*tint.a:1.f));
        for(int k=0;k<3;++k)image.pixels[i+k]=quant(srgb(straight[k]));image.pixels[i+3]=quant(straight.a);
      } else {
        for(int k=0;k<3;++k) image.pixels[i+k]=quant(srgb(color[k]));
        image.pixels[i+3]=cutout ? (value.a>=cutoff ? 255:0) : blend ? quant(value.a*tint.a):255;
      }
    }
    if(additive)bleed_transparent_rgb(image);
    out.maps["albedo"]=image;out.previewBaseColor=image;
    out.provenance["textures"][baseProperty]={{"source",texture->id()},{"name",source.name(*texture)},{"width",image.width},{"height",image.height}};
    out.provenance["alphaMode"]=cutout?"mask":blend?"blend":"opaque";
    out.provenance["fallback"]="Authored character base texture, tint and surface opacity";
    out.provenance["limitations"]="Static base approximation: custom effects and unavailable normal/specular channels are omitted; texture masks are never substituted for base color.";
    if(additive) {
      out.provenance["fallback"]="Authored additive color converted to soft straight alpha; black matte removed";
      out.provenance["sourceBlend"]={{"source",additive},{"destination",1}};
      out.provenance["limitations"]="Soft alpha approximation of additive light; background-independent additive brightness cannot be exactly represented by standard alpha blending. Animated UV/time, vertex colors, scene depth and exposure are not baked.";
      out.provenance["staticParticleMaskAndTwist"]=particleAdd;
    }
  }
  return out;
}

static Material particle_surface(Source &source,Object &material,int maxDimension,
                                 std::map<std::string,Image> &cache) {
  const auto &tree=source.tree(material);auto *shader=source.ref(material,tree.at("m_Shader"));
  require(shader && sha256(shader->raw())=="8e61e7407997d59083da4a4c4441ef7eba12d42acb3cd23519032e74c9d044a2",
          "Shader revision differs from verified particle base adapter");
  std::istringstream keys(tree.value("m_ShaderKeywords",std::string()));std::string key;
  while(keys>>key) require(key=="_DYNAMIC_TRANSPARENT" || key=="_FOG_ON","Unsupported particle surface keyword: "+key);
  const auto &saved=tree.at("m_SavedProperties");
  auto scalar=[&](const std::string &name,float fallback) {for(const auto &p:saved.at("m_Floats")) if(p.at("first")==name) return p.at("second").get<float>();return fallback;};
  for(auto name:{"_SoftParticle","_NearCameraFade","_TimeScale","_SubModeUV","_NoiseIntensity"}) require(scalar(name,0)==0,"Unsupported active particle effect: "+std::string(name));
  auto out=base_texture_fallback(source,material,maxDimension,cache,"Static particle surface approximation");
  std::string textureId=out.provenance.at("textures").at("_MainTex").at("source");
  auto image=cache.at(textureId+":"+std::to_string(maxDimension));
  V4 tint(1);for(const auto &p:saved.at("m_Colors")) if(p.at("first")=="_TintColor") tint=rgba(p.at("second"));
  float brightness=scalar("_Brightness",1);
  for(size_t i=0;i<image.pixels.size();i+=4) {
    for(int k=0;k<3;++k) image.pixels[i+k]=quant(srgb(linear(image.pixels[i+k]/255.f)*linear(tint[k])*brightness));
    image.pixels[i+3]=quant(image.pixels[i+3]/255.f*tint.a);
  }
  out.maps["albedo"]=image;out.previewBaseColor=image;
  out.provenance["fallback"]="Authored particle texture, tint and transparency";
  out.provenance["alphaMode"]="blend";
  out.provenance["limitations"]="Static approximation; vertex colors, scene lighting and fog are not baked. No normal/specular sampler is invented.";
  return out;
}

MaterialSet prepare_materials(Source &source, Model &model, const fs::path &db,
                              const J &overrides, int maxDimension,
                              JobContext *job, bool preview) {
  MaterialSet result;
  MaterialResolver resolver(source, db);
  std::map<std::string, Image> textures;
  size_t done = 0;
  for (size_t surfaceIndex = 0; surfaceIndex < model.surfaces.size();
       surfaceIndex++) {
    auto &s = model.surfaces[surfaceIndex];
    if (job)
      job->update(float(done++) / std::max(size_t(1), model.surfaces.size()),
                  "Preparing textures: " + s.name);
    try {
      auto r = resolver.resolve(s, overrides);
      bool approximateSlot = false;
      if (s.materialId.empty() ||
          (source.tree(source.object(s.meshId)).at("m_SubMeshes").size() > 1 &&
           r.value("method", std::string())
               .starts_with("authored asset directory"))) {
        auto recovered =
            resolver.recover(source.object(s.meshId),
                             s.materialContextMesh.empty()
                                 ? nullptr
                                 : &source.object(s.materialContextMesh),
                             job);
        for (auto &candidate : recovered.value("candidates", J::array()))
          if (candidate.value("slot", -1) == s.submesh)
            r["candidates"].push_back(candidate);
        auto ids = recovered.value("materials", J::array());
        if (recovered.value("hasUnpartitionedDonorPasses", false) &&
            r.value("method", std::string()).find("shared receiver material") !=
                std::string::npos) {
          s.materialId.clear();
          r["material"] = nullptr;
          r["resolved"] = false;
          r["method"] = "unresolved: donor contains additional material passes "
                        "without a matching submesh partition";
        }
        if (s.submesh < int(ids.size()) && !ids[s.submesh].is_null()) {
          s.materialId = ids[s.submesh];
          r["material"] = s.materialId;
          r["resolved"] = true;
          r["method"] = "scoped source mesh and UV correspondence";
          r["correspondence"] = recovered.at("matches");
        }
        r["recoveryStatus"] = recovered.value("status", std::string());
      }
      if (s.materialId.empty() &&
          model.report.at("source").value("category", std::string()) == "Weapon") {
        auto candidate = component_base_candidate(source, source.object(s.meshId),
                                                   r.at("candidates"));
        if (!candidate.empty()) {
          s.materialId = candidate;
          approximateSlot = true;
          r["material"] = candidate;
          r["resolved"] = true;
          r["method"] = "base texture approximation from unique scoped component material; slot binding unverified";
        }
      }
      result.surfaces.push_back(r);
      require(!s.materialId.empty(), "Unresolved material: " + s.name +
                                         " slot " + std::to_string(s.submesh));
      auto &m = source.object(s.materialId);
      auto key = m.id();
      if (approximateSlot)
        key += ":base-slot-fallback";
      auto *shader = source.ref(m, source.tree(m).at("m_Shader"));
      bool quickPreview = preview && model.report.at("source").value("category", std::string()) == "Weapon";
      bool flow = shader && needs_bake_context(source, m, *shader);
      quickPreview &= !flow; // Preserve authored UV/layer baking where base RGB is insufficient.
      if (flow)
        key +=
            ":" + bake_context(
                      s, source.tree(*shader).at("m_ParsedForm").at("m_Name") ==
                             "Special Weapon/Flowmap2");
      if (!result.materials.contains(key)) {
        try {
          bool quickLoaded=false;
          if(quickPreview) {
            try {
              result.materials[key]=base_texture_fallback(source,m,maxDimension,textures,
                  "Fast preview: authored base RGB; full material processing is performed at export");
              quickLoaded=true;
            } catch(const std::exception &) { /* Unsupported base UVs use the full path below. */ }
          }
          if(!quickLoaded) result.materials[key] = approximateSlot
              ? base_texture_fallback(source, m, maxDimension, textures,
                    "Unassigned material slot; unique scoped component base texture approximation")
              : convert_material(source, m, s, maxDimension, textures, preview);
          if (approximateSlot) {
            result.materials[key].id = key;
            result.materials[key].name += "_base_slot";
          }
        } catch (const UVLayers &) {
          throw;
        } catch (const std::exception &e) {
          if(shader && source.tree(*shader).at("m_ParsedForm").at("m_Name")=="CODM/FX/MSParticle") {
            result.materials[key]=particle_surface(source,m,maxDimension,textures);
          } else {
          if (!unsupported_shader(e.what()))
            throw;
          result.materials[key] = base_texture_fallback(source, m, maxDimension,
              textures, e.what(), model.report.at("source").value("category", std::string()) != "Weapon");
          }
        }
      }
      const auto &provenance = result.materials.at(key).provenance;
      if (provenance.contains("fallback")) {
        if (!model.report.contains("materialWarnings"))
          model.report["materialWarnings"] = J::array();
        model.report["materialWarnings"].push_back(
            {{"surface", s.name}, {"material", m.id()},
             {"fallback", provenance.at("fallback")},
             {"reason", provenance.at("fallbackReason")}});
      }
      s.materialId = key;
      s.materialFailed = false;
    } catch (const UVLayers &split) {
      auto original = s;
      if (!model.report.contains("uvLayerSplits"))
        model.report["uvLayerSplits"] = J::array();
      J triangleCounts = J::array();
      for (auto &group : split.groups)
        triangleCounts.push_back(group.size() / 3);
      model.report["uvLayerSplits"].push_back(
          {{"sourceMesh", original.meshId},
           {"sourceSlot", original.submesh},
           {"sourceTriangles", original.indices.size() / 3},
           {"layerTriangles", triangleCounts}});
      for (size_t layer = 0; layer < split.groups.size(); layer++) {
        auto part = subset_surface(original, split.groups[layer]);
        part.name += "_uv_layer_" + std::to_string(layer + 1);
        if (layer == 0)
          model.surfaces[surfaceIndex] = std::move(part);
        else
          model.surfaces.push_back(std::move(part));
      }
      if (!result.surfaces.empty())
        result.surfaces.erase(result.surfaces.size() - 1);
      --surfaceIndex;
    } catch (const std::exception &e) {
      s.materialFailed = true;
      result.errors.push_back({{"surface", s.name},
                               {"mesh", s.meshId},
                               {"slot", s.submesh},
                               {"error", e.what()}});
    }
  }
  model.report["materialResolutions"] = result.surfaces;
  model.report["materialErrors"] = result.errors;
  if (model.report.contains("uvLayerSplits")) {
    model.report["surfaces"] = model.surfaces.size();
    size_t vertices = 0, triangles = 0;
    for (auto &surface : model.surfaces) {
      vertices += surface.positions.size();
      triangles += surface.indices.size() / 3;
    }
    model.report["vertices"] = vertices;
    model.report["triangles"] = triangles;
  }
  return result;
}
void omit_unresolved_surfaces(Model &model, MaterialSet &materials) {
  J omitted=J::array();std::set<std::string> used;
  model.surfaces.erase(std::remove_if(model.surfaces.begin(),model.surfaces.end(),[&](const Surface &s){
    if(!s.materialFailed && materials.materials.contains(s.materialId)){used.insert(s.materialId);return false;}
    omitted.push_back({{"surface",s.name},{"mesh",s.meshId},{"renderer",s.rendererId},{"slot",s.submesh},{"material",s.materialId}});return true;
  }),model.surfaces.end());
  model.report["omittedSurfaces"]=omitted;
  require(!model.surfaces.empty(),"No surfaces with resolved materials remain; no empty model was exported");
  for(auto it=materials.materials.begin();it!=materials.materials.end();)if(!used.contains(it->first))it=materials.materials.erase(it);else ++it;
  materials.errors=J::array();
  size_t vertices=0,triangles=0;for(const auto &s:model.surfaces){vertices+=s.positions.size();triangles+=s.indices.size()/3;}
  model.report["surfaces"]=model.surfaces.size();model.report["vertices"]=vertices;model.report["triangles"]=triangles;
}
} // namespace codm
