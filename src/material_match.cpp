#include "materials.hpp"
namespace codm {
// Material identity follows UV triangle topology, even when the authored
// renderer is posed, rescaled, or combines several detachable parts. Compare
// every triangle within the quantization error of the serialized UV streams.
std::optional<double> material_uv_correspondence(const RawMesh &target,
                                                 size_t ts,
                                                 const RawMesh &donor,
                                                 size_t ds) {
  const auto &tf = target.faces.at(ts), &df = donor.faces.at(ds);
  if (tf.size() < 6 || tf.size() > df.size() || df.size() > 300000 ||
      target.uv0.size() != target.positions.size() ||
      donor.uv0.size() != donor.positions.size())
    return {};
  double eps = target.uvPrecision + donor.uvPrecision + 1e-7;
  if (!std::isfinite(eps) || eps > .004)
    return {};
  using Cell = std::pair<int64_t, int64_t>;
  auto centroid = [](const RawMesh &m, const auto &f, size_t i) {
    return (glm::dvec2(m.uv0[f[i]]) + glm::dvec2(m.uv0[f[i + 1]]) +
            glm::dvec2(m.uv0[f[i + 2]])) /
           3.;
  };
  auto cell = [&](glm::dvec2 p) {
    return Cell{int64_t(std::floor(p.x / eps)), int64_t(std::floor(p.y / eps))};
  };
  std::map<Cell, std::vector<size_t>> grid;
  for (size_t i = 0; i < df.size(); i += 3)
    grid[cell(centroid(donor, df, i))].push_back(i);
  std::set<size_t> consumed;
  double worst = 0;
  size_t substantial = 0;
  for (size_t i = 0; i < tf.size(); i += 3) {
    auto a = glm::dvec2(target.uv0[tf[i]]),
         b = glm::dvec2(target.uv0[tf[i + 1]]),
         c = glm::dvec2(target.uv0[tf[i + 2]]);
    substantial += std::abs((b.x - a.x) * (c.y - a.y) -
                            (b.y - a.y) * (c.x - a.x)) > eps * eps * 4;
    auto center = cell(centroid(target, tf, i));
    double best = INFINITY;
    size_t bestTri = 0;
    for (int y = -1; y <= 1; y++)
      for (int x = -1; x <= 1; x++) {
        auto found = grid.find({center.first + x, center.second + y});
        if (found == grid.end())
          continue;
        for (auto tri : found->second) {
          if (consumed.contains(tri))
            continue;
          std::array<int, 3> order{0, 1, 2};
          do {
            double error = 0;
            for (int k = 0; k < 3; k++) {
              auto delta = glm::abs(glm::dvec2(target.uv0[tf[i + k]]) -
                                    glm::dvec2(donor.uv0[df[tri + order[k]]]));
              error = std::max(error, std::max(delta.x, delta.y));
            }
            if (error < best) {
              best = error;
              bestTri = tri;
            }
          } while (std::next_permutation(order.begin(), order.end()));
        }
      }
    if (best > eps)
      return {};
    consumed.insert(bestTri);
    worst = std::max(worst, best);
  }
  if (substantial < 2)
    return {};
  return worst;
}
using UVKey = uint64_t;
static UVKey uvkey(V2 uv) {
  float x = uv.x == 0 ? 0 : uv.x, y = uv.y == 0 ? 0 : uv.y;
  return uint64_t(std::bit_cast<uint32_t>(x)) << 32 |
         std::bit_cast<uint32_t>(y);
}
using TriUV = std::array<UVKey, 3>;
static std::vector<TriUV> uv_triangles(const RawMesh &m, size_t slot) {
  std::vector<TriUV> out;
  auto &f = m.faces.at(slot);
  for (size_t i = 0; i < f.size(); i += 3) {
    TriUV tri = {uvkey(m.uv0.at(f[i])), uvkey(m.uv0.at(f[i + 1])),
                 uvkey(m.uv0.at(f[i + 2]))};
    std::sort(tri.begin(), tri.end());
    out.push_back(tri);
  }
  std::sort(out.begin(), out.end());
  return out;
}
struct Match {
  std::vector<int> slots;
  glm::dmat3 rotation{1};
  double scale = 1, error = 0;
  DV3 translation{0};
  double tolerance = 0;
  bool lod = false;
};
static std::optional<Match> match_geometry(const RawMesh &target,
                                           const RawMesh &donor) {
  if (target.uv0.size() != target.positions.size() ||
      donor.uv0.size() != donor.positions.size() ||
      target.faces.size() != donor.faces.size() ||
      target.positions.size() > 60000 || donor.positions.size() > 60000)
    return {};
  Match out;
  std::vector<std::vector<TriUV>> targetUV, donorUV;
  for (size_t i = 0; i < target.faces.size(); i++)
    targetUV.push_back(uv_triangles(target, i));
  for (size_t i = 0; i < donor.faces.size(); i++)
    donorUV.push_back(uv_triangles(donor, i));
  std::set<int> occupied;
  for (auto &uv : targetUV) {
    std::vector<int> candidates;
    for (size_t i = 0; i < donorUV.size(); i++)
      if (uv == donorUV[i])
        candidates.push_back(int(i));
    if (candidates.size() != 1 || !occupied.insert(candidates[0]).second)
      return {};
    out.slots.push_back(candidates[0]);
  }
  auto groups = [](const RawMesh &m) {
    std::map<UVKey, std::pair<DV3, size_t>> sums;
    for (size_t i = 0; i < m.positions.size(); i++) {
      auto &p = sums[uvkey(m.uv0[i])];
      p.first += DV3(m.positions[i]);
      p.second++;
    }
    std::map<UVKey, DV3> out;
    for (auto &[k, p] : sums)
      out[k] = p.first / double(p.second);
    return out;
  };
  auto a = groups(donor), b = groups(target);
  if (a.size() != b.size() || a.size() < 8)
    return {};
  std::vector<DV3> from, to;
  for (auto &[key, pos] : a) {
    if (!b.contains(key))
      return {};
    from.push_back(pos);
    to.push_back(b.at(key));
  }
  size_t second = 0, third = 0;
  double longest = 0, area = 0;
  for (size_t i = 1; i < from.size(); i++) {
    double d = glm::length(from[i] - from[0]);
    if (d > longest) {
      longest = d;
      second = i;
    }
  }
  if (longest < 1e-8)
    return {};
  auto x = glm::normalize(from[second] - from[0]);
  for (size_t i = 1; i < from.size(); i++) {
    double d = glm::length(glm::cross(from[i] - from[0], x));
    if (d > area) {
      area = d;
      third = i;
    }
  }
  if (area < 1e-8)
    return {};
  auto basis = [&](const std::vector<DV3> &v) {
    auto x = glm::normalize(v[second] - v[0]);
    auto y = v[third] - v[0];
    y -= x * glm::dot(x, y);
    if (glm::length(y) < 1e-9)
      throw std::runtime_error("Degenerate material correspondence anchors");
    y = glm::normalize(y);
    return glm::dmat3(x, y, glm::cross(x, y));
  };
  out.rotation = basis(to) * glm::transpose(basis(from));
  out.scale = glm::length(to[second] - to[0]) / longest;
  if (out.scale < .1 || out.scale > 10)
    return {};
  out.translation = to[0] - out.scale * out.rotation * from[0];
  DV3 lo(1e30), hi(-1e30);
  for (auto &p : target.positions) {
    lo = glm::min(lo, DV3(p));
    hi = glm::max(hi, DV3(p));
  }
  double tolerance = std::max(2e-6, glm::length(hi - lo) * 3e-5);
  for (size_t i = 0; i < from.size(); i++)
    if (glm::length(out.scale * out.rotation * from[i] + out.translation -
                    to[i]) > tolerance)
      return {};
  // Match every triangle with its UV association; the fit cannot transfer one
  // matching part's skin to a different assembly.
  for (size_t slot = 0; slot < target.faces.size(); slot++) {
    auto &tf = target.faces[slot];
    auto &df = donor.faces[out.slots[slot]];
    std::map<TriUV, std::vector<std::array<uint32_t, 3>>> lookup;
    for (size_t i = 0; i < df.size(); i += 3) {
      TriUV uv = {uvkey(donor.uv0[df[i]]), uvkey(donor.uv0[df[i + 1]]),
                  uvkey(donor.uv0[df[i + 2]])};
      std::sort(uv.begin(), uv.end());
      lookup[uv].push_back({df[i], df[i + 1], df[i + 2]});
    }
    for (size_t i = 0; i < tf.size(); i += 3) {
      TriUV uv = {uvkey(target.uv0[tf[i]]), uvkey(target.uv0[tf[i + 1]]),
                  uvkey(target.uv0[tf[i + 2]])};
      std::sort(uv.begin(), uv.end());
      auto &candidates = lookup.at(uv);
      bool matched = false;
      for (auto it = candidates.begin(); it != candidates.end(); ++it) {
        double error = 0;
        std::set<int> used;
        bool ok = true;
        for (int k = 0; k < 3; k++) {
          double best = INFINITY;
          int chosen = -1;
          for (int d = 0; d < 3; d++)
            if (!used.contains(d) &&
                uvkey(target.uv0[tf[i + k]]) == uvkey(donor.uv0[(*it)[d]])) {
              double e = glm::length(
                  DV3(target.positions[tf[i + k]]) -
                  (out.scale * out.rotation * DV3(donor.positions[(*it)[d]]) +
                   out.translation));
              if (e < best) {
                best = e;
                chosen = d;
              }
            }
          if (chosen < 0 || best > tolerance) {
            ok = false;
            break;
          }
          used.insert(chosen);
          error = std::max(error, best);
        }
        if (ok) {
          out.error = std::max(out.error, error);
          candidates.erase(it);
          matched = true;
          break;
        }
      }
      if (!matched)
        return {};
    }
  }
  return out;
}
static std::optional<Match> match_lod(const RawMesh &target,
                                      const RawMesh &donor) {
  if (target.faces.size() != donor.faces.size() ||
      target.uv0.size() != target.positions.size() ||
      donor.uv0.size() != donor.positions.size())
    return {};
  using Key = std::pair<int64_t, int64_t>;
  auto anchors = [](const RawMesh &m) {
    std::map<Key, std::vector<DV3>> points;
    for (size_t i = 0; i < m.positions.size(); i++) {
      auto uv = m.uv0[i];
      auto &group =
          points[{std::llround(uv.x * 100000), std::llround(uv.y * 100000)}];
      auto p = DV3(m.positions[i]);
      if (group.empty() || glm::length(group[0] - p) > 1e-7)
        group.push_back(p);
    }
    std::map<Key, DV3> unique;
    for (auto &[key, group] : points)
      if (group.size() == 1)
        unique[key] = group[0];
    return unique;
  };
  auto a = anchors(donor), b = anchors(target);
  std::vector<DV3> from, to;
  for (auto &[key, p] : a)
    if (b.contains(key)) {
      from.push_back(p);
      to.push_back(b.at(key));
    }
  if (from.size() < 8)
    return {};
  size_t second = 0, third = 0;
  double span = 0, area = 0;
  for (size_t i = 1; i < from.size(); i++) {
    double d = glm::length(from[i] - from[0]);
    if (d > span) {
      span = d;
      second = i;
    }
  }
  if (span < 1e-8)
    return {};
  auto x = glm::normalize(from[second] - from[0]);
  for (size_t i = 1; i < from.size(); i++) {
    double v = glm::length(glm::cross(from[i] - from[0], x));
    if (v > area) {
      area = v;
      third = i;
    }
  }
  if (area < 1e-8)
    return {};
  auto frame = [&](auto &v) {
    auto x = glm::normalize(v[second] - v[0]), y = v[third] - v[0];
    y -= x * glm::dot(x, y);
    require(glm::length(y) > 1e-9, "Degenerate LOD anchors");
    y = glm::normalize(y);
    return glm::dmat3(x, y, glm::cross(x, y));
  };
  Match out;
  out.lod = true;
  out.rotation = frame(to) * glm::transpose(frame(from));
  out.scale = glm::length(to[second] - to[0]) / span;
  if (out.scale < .1 || out.scale > 10)
    return {};
  out.translation = to[0] - out.scale * out.rotation * from[0];
  DV3 lo(1e30), hi(-1e30);
  for (auto &p : target.positions) {
    lo = glm::min(lo, DV3(p));
    hi = glm::max(hi, DV3(p));
  }
  out.tolerance = std::max(2e-6, glm::length(hi - lo) * .005);
  for (size_t i = 0; i < from.size(); i++)
    if (glm::length(out.scale * out.rotation * from[i] + out.translation -
                    to[i]) > out.tolerance)
      return {};
  auto compare = [&](const RawMesh &sampled, size_t sampleSlot,
                     const RawMesh &surface, size_t surfaceSlot,
                     bool reverse) -> double {
    const auto &sf = sampled.faces[sampleSlot],
               &df = surface.faces[surfaceSlot];
    if (sf.size() > 60000 || df.size() > 60000)
      return INFINITY;
    V2 uvLo(1e20), uvHi(-1e20);
    for (auto index : df) {
      uvLo = glm::min(uvLo, surface.uv0[index]);
      uvHi = glm::max(uvHi, surface.uv0[index]);
    }
    auto extent = glm::max(uvHi - uvLo, V2(1e-6f));
    auto cell = [&](V2 uv) {
      auto q = (uv - uvLo) / extent * 31.f;
      return std::pair{std::clamp(int(q.x), 0, 31),
                       std::clamp(int(q.y), 0, 31)};
    };
    std::map<std::pair<int, int>, std::vector<size_t>> grid;
    for (size_t i = 0; i < df.size(); i += 3) {
      auto a = surface.uv0[df[i]], b = surface.uv0[df[i + 1]],
           c = surface.uv0[df[i + 2]];
      auto low = cell(glm::min(a, glm::min(b, c))),
           high = cell(glm::max(a, glm::max(b, c)));
      for (int y = low.second; y <= high.second; y++)
        for (int x = low.first; x <= high.first; x++)
          grid[{x, y}].push_back(i);
    }
    double worst = 0;
    const std::array<V3, 7> weights = {
        V3(1, 0, 0),   V3(0, 1, 0),   V3(0, 0, 1), V3(.5, .5, 0),
        V3(0, .5, .5), V3(.5, 0, .5), V3(1.f / 3)};
    for (size_t i = 0; i < sf.size(); i += 3)
      for (auto weight : weights) {
        V2 uv(0);
        DV3 point(0);
        for (int k = 0; k < 3; k++) {
          uv += weight[k] * sampled.uv0[sf[i + k]];
          point += double(weight[k]) * DV3(sampled.positions[sf[i + k]]);
        }
        if (reverse)
          point = out.scale * out.rotation * point + out.translation;
        double best = INFINITY;
        auto found = grid.find(cell(uv));
        if (found == grid.end())
          return double(INFINITY);
        for (auto tri : found->second) {
          auto a = surface.uv0[df[tri]], b = surface.uv0[df[tri + 1]],
               c = surface.uv0[df[tri + 2]];
          auto u = b - a, v = c - a, p = uv - a;
          double det = u.x * v.y - u.y * v.x;
          if (std::abs(det) < 1e-12)
            continue;
          double s = (p.x * v.y - p.y * v.x) / det,
                 t = (u.x * p.y - u.y * p.x) / det;
          if (s < -.0001 || t < -.0001 || s + t > 1.0001)
            continue;
          DV3 expected = (1 - s - t) * DV3(surface.positions[df[tri]]) +
                         s * DV3(surface.positions[df[tri + 1]]) +
                         t * DV3(surface.positions[df[tri + 2]]);
          if (!reverse)
            expected = out.scale * out.rotation * expected + out.translation;
          best = std::min(best, glm::length(point - expected));
        }
        if (best > out.tolerance)
          return double(INFINITY);
        worst = std::max(worst, best);
      }
    return worst;
  };
  std::set<int> used;
  for (size_t i = 0; i < target.faces.size(); i++) {
    std::vector<std::pair<int, double>> valid;
    for (size_t j = 0; j < donor.faces.size(); j++) {
      double forward = compare(target, i, donor, j, false);
      if (!std::isfinite(forward))
        continue;
      double reverse = compare(donor, j, target, i, true);
      if (std::isfinite(reverse))
        valid.push_back({int(j), std::max(forward, reverse)});
    }
    if (valid.size() != 1 || !used.insert(valid[0].first).second)
      return {};
    out.slots.push_back(valid[0].first);
    out.error = std::max(out.error, valid[0].second);
  }
  return out;
}
static J stamp(const fs::path &path) {
  return J::array({fs::file_size(path),
                   fs::last_write_time(path).time_since_epoch().count()});
}
static bool dependencies_current(const J &deps) {
  try {
    for (auto it = deps.begin(); it != deps.end(); it++)
      if (stamp(pathof(it.key())) != it.value())
        return false;
    return true;
  } catch (...) {
    return false;
  }
}
J texture_content_fingerprint(const J &texture,
                              std::span<const uint8_t> payload) {
  J key = {{"payloadSHA256", sha256(payload)}};
  for (auto field : {"m_TextureFormat", "m_Width", "m_Height", "m_MipCount",
                     "m_ImageCount", "m_TextureDimension", "m_ColorSpace",
                     "m_TextureSettings", "m_LightmapFormat"})
    if (texture.contains(field))
      key[field] = texture.at(field);
  return key;
}
std::string material_content_key(Source &source, Object &material) {
  const auto &t = source.tree(material);
  J key = {{"keywords", t.value("m_ShaderKeywords", std::string())},
           {"properties", t.at("m_SavedProperties")}};
  auto *shader = source.ref(material, t.at("m_Shader"));
  require(shader, "Missing shader in material alias check");
  key["shader"] = sha256(shader->raw());
  for (auto &p : key["properties"]["m_TexEnvs"]) {
    auto &env = p.at("second");
    auto *texture = source.ref(material, env.at("m_Texture"));
    if (!texture) {
      env["m_Texture"] = nullptr;
      continue;
    }
    if (texture->cid != 28) {
      env["m_Texture"] = texture->id();
      continue;
    }
    const auto &tt = source.tree(*texture);
    auto stream = tt.at("m_StreamData");
    Bytes payload;
    if (!stream.at("path").get<std::string>().empty())
      payload = source.resource(stream.at("path"), stream.at("offset"),
                                stream.at("size"));
    else
      payload = tt.at("image data").get_binary();
    env["m_Texture"] = texture_content_fingerprint(tt, payload);
  }
  auto canonical = key.dump();
  for (auto &properties : key["properties"])
    if (properties.is_array())
      std::sort(
          properties.begin(), properties.end(),
          [](const J &a, const J &b) { return a.at("first") < b.at("first"); });
  std::istringstream tokens(t.value("m_ShaderKeywords", std::string()));
  std::set<std::string> keywords;
  for (std::string word; tokens >> word;)
    keywords.insert(word);
  key["keywords"] = keywords;
  canonical = key.dump();
  return sha256(std::span(reinterpret_cast<const uint8_t *>(canonical.data()),
                          canonical.size()));
}
J MaterialResolver::recover(Object &mesh, Object *context, JobContext *job) {
  auto memoryKey = mesh.id() + ":" + (context ? context->id() : "");
  if (recoveryResults.contains(memoryKey))
    return recoveryResults.at(memoryKey);
  if (!fs::exists(dbPath))
    return {{"status", "no donor index"},
            {"materials", J::array()},
            {"candidates", J::array()}};
  Database db(dbPath);
  std::vector<int> counts;
  for (auto &s : source.tree(mesh).at("m_SubMeshes"))
    counts.push_back(s.at("indexCount").get<int>() / 3);
  std::sort(counts.begin(), counts.end());
  auto rows = db.query("SELECT mesh,renderer,bundle,materials FROM donors "
                       "WHERE counts=? ORDER BY mesh,renderer",
                       {J(counts).dump()});
  std::set<std::string> exactDirectory;
  for (auto &c : candidates(mesh, context))
    exactDirectory.insert(c.at("id"));
  if (!exactDirectory.empty() && exactDirectory.size() <= 80) {
    std::string query = "SELECT mesh,renderer,bundle,materials FROM donors "
                        "WHERE EXISTS(SELECT 1 "
                        "FROM json_each(donors.materials) WHERE value IN(";
    std::vector<std::string> args(exactDirectory.begin(), exactDirectory.end());
    for (size_t i = 0; i < args.size(); i++)
      query += (i ? ",?" : "?");
    query += ")) ORDER BY mesh,renderer";
    auto contextual = db.query(query, args);
    rows.insert(rows.end(), contextual.begin(), contextual.end());
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  }
  J indexRows = rows;
  auto key = hex64(hash64(mesh.id() + ":" + (context ? context->id() : "") +
                          ":" + sha256(mesh.raw()) + ":" + indexRows.dump()));
  auto cache =
      dbPath.parent_path() / "cache/material-matches" / (key + ".json");
  if (fs::exists(cache))
    try {
      auto j = read_json(cache);
      if (j.value("version", 0) == 8 &&
          dependencies_current(j.at("dependencies")))
        return recoveryResults[memoryKey] = j;
    } catch (...) {
    }
  J matches = J::array(), errors = J::array();
  bool unpartitionedPasses = false;
  auto target = decode_mesh(source, mesh);
  std::map<std::string, std::optional<Match>> fits;
  size_t done = 0;
  for (auto &row : rows) {
    if (job)
      job->update(float(done++) / std::max(size_t(1), rows.size()),
                  "Matching geometry and UVs: " + source.name(mesh));
    if (fits.size() > 2000) {
      errors.push_back("Candidate bound reached; refine source context or "
                       "select a material");
      break;
    }
    try {
      auto ids = J::parse(row[3]);
      bool scoped = false;
      for (auto &id : ids)
        if (id.is_string())
          scoped |= exactDirectory.contains(id.get<std::string>());
      if (scoped) {
        auto donor = decode_mesh(source, source.object(row[0]));
        // Extra materials on the final donor submesh are additional draw
        // passes, not a material partition for a different mesh.
        if (ids.size() > donor.faces.size()) {
          scoped = false;
          unpartitionedPasses = true;
        }
        if (scoped)
          for (size_t ti = 0; ti < target.faces.size(); ti++)
            for (size_t di = 0; di < donor.faces.size() && di < ids.size();
                 di++) {
              if (!ids[di].is_string() ||
                  !exactDirectory.contains(ids[di].get<std::string>()))
                continue;
              auto error = material_uv_correspondence(target, ti, donor, di);

              if (!error)
                continue;
              J materials = J::array();
              for (size_t k = 0; k < target.faces.size(); k++)
                materials.push_back(k == ti ? ids[di] : J());
              matches.push_back(
                  {{"donorMesh", row[0]},
                   {"renderer", row[1]},
                   {"materials", materials},
                   {"maximumUVError", *error},
                   {"confidence", 2},
                   {"uvTolerance",
                    target.uvPrecision + donor.uvPrecision + 1e-7},
                   {"evidence",
                    "All target slot UV triangles occur in scoped authored "
                    "donor; serialized UV precision respected"}});
            }
      }
      if (!fits.contains(row[0])) {
        auto donor = decode_mesh(source, source.object(row[0]));
        fits[row[0]] = match_geometry(target, donor);
        if (!fits[row[0]])
          fits[row[0]] = match_lod(target, donor);
      }
      auto &fit = fits.at(row[0]);
      if (!fit)
        continue;
      J mats = J::array();
      for (auto slot : fit->slots)
        mats.push_back(slot < int(ids.size()) ? ids[slot] : J());
      matches.push_back(
          {{"donorMesh", row[0]},
           {"renderer", row[1]},
           {"materials", mats},
           {"maximumError", fit->error},
           {"tolerance", fit->tolerance},
           {"uniformScale", fit->scale},
           {"evidence",
            fit->lod
                ? "bidirectional UV surface correspondence with bounded LOD "
                  "deviation under one proper similarity transform"
                : "all triangle positions and UV associations agree under one "
                  "proper similarity transform"}});
    } catch (const std::exception &e) {
      errors.push_back({{"mesh", row[0]}, {"error", e.what()}});
    }
  }
  J chosen = J::array(), alternatives = J::array();
  std::map<std::string, std::string> content;
  for (size_t slot = 0; slot < target.faces.size(); slot++) {
    std::set<std::string> ids;
    int strongest = 0;
    for (auto &m : matches)
      if (!m.at("materials")[slot].is_null())
        strongest = std::max(strongest, m.value("confidence", 3));
    for (auto &m : matches)
      if (!m.at("materials")[slot].is_null() &&
          m.value("confidence", 3) == strongest)
        ids.insert(m.at("materials")[slot]);
    std::set<std::string> scoped;
    for (auto &id : ids)
      if (exactDirectory.contains(id))
        scoped.insert(id);
    if (!scoped.empty())
      ids = scoped;
    std::map<std::string, std::string> distinct;
    for (auto &id : ids) {
      auto &obj = source.object(id);
      if (!content.contains(id))
        content[id] = material_content_key(source, obj);
      distinct.try_emplace(content[id], id);
      alternatives.push_back({{"slot", slot},
                              {"id", id},
                              {"name", source.name(obj)},
                              {"contentKey", content[id]}});
    }
    chosen.push_back(distinct.size() == 1 ? J(distinct.begin()->second) : J());
  }
  J deps = J::object();
  for (auto &path : source.dependency_paths())
    deps[path] = stamp(pathof(path));
  J result = {{"version", 8},
              {"mesh", mesh.id()},
              {"hasUnpartitionedDonorPasses", unpartitionedPasses},
              {"matches", matches},
              {"materials", chosen},
              {"candidates", alternatives},
              {"errors", errors},
              {"dependencies", deps},
              {"status", std::all_of(chosen.begin(), chosen.end(),
                                     [](auto &v) { return !v.is_null(); })
                             ? "resolved"
                             : "ambiguous or absent"}};
  write_json(cache, result);
  return recoveryResults[memoryKey] = result;
}
} // namespace codm
