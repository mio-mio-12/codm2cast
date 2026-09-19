#include "source.hpp"
#include <lz4/lz4.h>
#include <regex>
namespace codm {
TypeNode make_schema(const J &nodes) {
  size_t at = 0;
  std::function<TypeNode()> read = [&] {
    const auto &n = nodes.at(at++);
    TypeNode t{n.at("m_Type"), n.at("m_Name"), n.value("m_ByteSize", -1),
               n.value("m_MetaFlag", 0)};
    int level = n.at("m_Level");
    while (at < nodes.size() && int(nodes[at].at("m_Level")) > level)
      t.children.push_back(read());
    return t;
  };
  return read();
}
J parse_tree(Reader &r, const TypeNode &n, size_t depth) {
  require(depth < 128, "Type tree nesting exceeds limit");
  const auto &t = n.type;
  J v;
  bool align = n.flags & 0x4000;
  auto start = r.pos;
  try {
    if (t == "bool")
      v = bool(r.get<uint8_t>());
    else if (t == "char" || t == "UInt8")
      v = r.get<uint8_t>();
    else if (t == "SInt8")
      v = r.get<int8_t>();
    else if (t == "short" || t == "SInt16")
      v = r.get<int16_t>();
    else if (t == "UInt16" || t == "unsigned short")
      v = r.get<uint16_t>();
    else if (t == "int" || t == "SInt32")
      v = r.get<int32_t>();
    else if (t == "UInt32" || t == "unsigned int")
      v = r.get<uint32_t>();
    else if (t == "SInt64" || t == "long long")
      v = r.get<int64_t>();
    else if (t == "UInt64" || t == "unsigned long long" || t == "FileSize")
      v = r.get<uint64_t>();
    else if (t == "float")
      v = r.get<float>();
    else if (t == "double")
      v = r.get<double>();
    else if (t == "string")
      v = r.str();
    else if (t == "TypelessData") {
      auto b = r.take(r.count());
      v = J::binary(Bytes(b.begin(), b.end()));
    } else if (!n.children.empty() && n.children[0].type == "Array") {
      const auto &arr = n.children[0];
      require(arr.children.size() == 2, "Invalid array schema");
      const auto &elem = arr.children[1];
      auto count = r.count();
      if ((elem.type == "UInt8" || elem.type == "char") &&
          !(elem.flags & 0x4000)) {
        auto b = r.take(count);
        v = J::binary(Bytes(b.begin(), b.end()));
      } else {
        v = J::array();
        v.get_ref<J::array_t &>().reserve(count);
        for (int i = 0; i < count; ++i)
          v.push_back(parse_tree(r, elem, depth + 1));
      }
      align |= bool(arr.flags & 0x4000);
    } else {
      v = J::object();
      require(!n.children.empty() || n.size == 0, "Unknown leaf type: " + t);
      for (auto &child : n.children)
        v[child.name] = parse_tree(r, child, depth + 1);
    }
    if (align)
      r.align();
    return v;
  } catch (const std::exception &e) {
    throw std::runtime_error(n.name + " at " + std::to_string(start) + ": " +
                             e.what());
  }
}
static Bytes decompress(std::span<const uint8_t> data, size_t size, int flags) {
  require(size <= 512ull * 1024 * 1024, "Unpacked block exceeds 512 MiB");
  int kind = flags & 63;
  if (kind == 0) {
    require(data.size() == size, "Uncompressed size mismatch");
    return Bytes(data.begin(), data.end());
  }
  require(kind == 2 || kind == 3,
          "Unsupported UnityFS compression " + std::to_string(kind));
  Bytes out(size);
  auto got = LZ4_decompress_safe(reinterpret_cast<const char *>(data.data()),
                                 reinterpret_cast<char *>(out.data()),
                                 int(data.size()), int(size));
  require(got == int(size), "Invalid LZ4 block");
  return out;
}
Bundle::Bundle(const fs::path &p) : path(p) {
  uint64_t size = fs::file_size(p);
  auto header = read_bytes(p, 0, std::min(size, uint64_t(16384)));
  Reader r(header, true);
  require(r.cstr() == "UnityFS", "Not a UnityFS bundle");
  auto version = r.get<uint32_t>();
  require(version >= 6 && version <= 8, "Unsupported UnityFS version");
  r.cstr();
  engine = r.cstr();
  require(r.get<uint64_t>() == size, "Bundle length differs from header");
  auto compressed = r.get<uint32_t>(), unpacked = r.get<uint32_t>();
  flags = r.get<uint32_t>();
  require(!(flags & 0x200), "Unsupported encrypted bundle flags");
  require(unpacked <= 64 * 1024 * 1024 && compressed <= size,
          "Invalid directory bounds");
  if (version >= 7)
    r.align(16);
  auto dirOffset = (flags & 128) ? size - compressed : r.pos;
  auto packed = read_bytes(p, dirOffset, compressed);
  auto bytes = decompress(packed, unpacked, flags);
  Reader d(bytes, true);
  d.skip(16);
  int blockCount = d.count(10);
  uint64_t off = 0, fpos = (flags & 128) ? r.pos : dirOffset + compressed;
  for (int i = 0; i < blockCount; i++) {
    Block b;
    b.unpacked = d.get<uint32_t>();
    b.packed = d.get<uint32_t>();
    b.flags = d.get<uint16_t>();
    b.offset = off;
    b.fileOffset = fpos;
    require(off <= UINT64_MAX - b.unpacked, "Expanded block offsets overflow");
    off += b.unpacked;
    require(fpos <= size && b.packed <= size - fpos,
            "Block exceeds bundle bounds");
    fpos += b.packed;
    blocks.push_back(b);
  }
  require(fpos <= ((flags & 128) ? dirOffset : size),
          "Blocks overlap directory");
  auto count = d.count(21);
  for (int i = 0; i < count; i++) {
    BundleNode n;
    n.offset = d.get<uint64_t>();
    n.size = d.get<uint64_t>();
    n.flags = d.get<uint32_t>();
    n.name = d.cstr();
    require(n.offset <= off && n.size <= off - n.offset,
            "Node outside expanded blocks");
    nodes.push_back(n);
  }
  require(d.pos == bytes.size(), "Unconsumed directory bytes");
}
Bytes Bundle::read(const BundleNode &n, uint64_t offset, uint64_t size) {
  require(offset <= n.size, "Node offset outside range");
  if (size == UINT64_MAX)
    size = n.size - offset;
  require(size <= n.size - offset && size <= 512ull * 1024 * 1024,
          "Node range exceeds bounds");
  Bytes out(static_cast<size_t>(size));
  uint64_t begin = n.offset + offset, end = begin + size, copied = 0;
  for (size_t i = 0; i < blocks.size(); i++) {
    const auto &b = blocks[i];
    if (b.offset >= end || b.offset + b.unpacked <= begin)
      continue;
    if (!cache.contains(i)) {
      auto packed = read_bytes(path, b.fileOffset, b.packed);
      auto decoded = decompress(packed, b.unpacked, b.flags);
      if (cacheBytes + decoded.size() > 128ull * 1024 * 1024) {
        cache.clear();
        cacheBytes = 0;
      }
      cacheBytes += decoded.size();
      cache[i] = std::move(decoded);
    }
    uint64_t lo = std::max(begin, b.offset),
             hi = std::min(end, b.offset + b.unpacked);
    std::memcpy(out.data() + lo - begin, cache.at(i).data() + lo - b.offset,
                hi - lo);
    copied += hi - lo;
  }
  require(copied == size, "Incomplete node range");
  return out;
}
std::tuple<int, int, int64_t, std::string> priority(const J &b) {
  auto p = lower(b.at("path").get<std::string>());
  std::replace(p.begin(), p.end(), '\\', '/');
  int rank = 0, patch = 0;
  auto pos = p.find("/persistentdata/");
  if (pos != p.npos) {
    rank = 1;
    auto ex = p.find("/extract/", pos);
    if (ex != p.npos) {
      rank = 2;
      auto s = p.substr(ex + 9);
      s = s.substr(0, s.find('/'));
      if (!s.empty() && s.find_first_not_of("0123456789") == s.npos) {
        try {
          patch = std::stoi(s);
        } catch (...) {
        }
      }
    }
  }
  return {rank, patch, b.value("mtime", int64_t(0)), p};
}
std::string hint(const std::string &p) {
  std::string s = basename(p), from = "PKLJMSHGZDBCEVUAYTFRONXWQI",
              to = "abcdefghijklmnopqrstuvwxyz";
  for (auto &c : s) {
    auto at = from.find(c);
    if (at != from.npos)
      c = to[at];
    else if (c == '$')
      c = '/';
    else if (c == '!')
      c = '.';
  }
  return s;
}
J scan_catalog(const fs::path &root, const fs::path &dest, JobContext *job) {
  require(fs::is_directory(root),
          "Game scan folder does not exist: " + pathstr(root));
  J previous = J::object();
  if (fs::exists(dest)) {
    auto old = read_json(dest);
    for (auto group : {"bundles", "dependencyBundles"})
      for (auto &b : old.value(group, J::array()))
        previous[b.at("path").get<std::string>()] = b;
  }
  std::map<std::string, J> chosen;
  std::vector<J> dependencies;
  J errors = J::array();
  std::error_code ec;
  for (fs::recursive_directory_iterator
           it(root, fs::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (job)
      job->check();
    if (ec) {
      errors.push_back(
          {{"path", pathstr(it->path())}, {"error", ec.message()}});
      ec.clear();
      continue;
    }
    if (!it->is_regular_file())
      continue;
    auto p = it->path();
    auto ext = lower(pathstr(p.extension()));
    if (ext != ".pak" && ext != ".assetbundle")
      continue;
    auto ticks = fs::last_write_time(p).time_since_epoch();
    int64_t mtime =
        std::chrono::duration_cast<std::chrono::nanoseconds>(ticks).count();
    J b = {{"path", pathstr(p)}, {"size", fs::file_size(p)}, {"mtime", mtime}};
    auto key = lower(pathstr(p.filename()));
    if (!chosen.contains(key) || priority(b) > priority(chosen[key])) {
      if (chosen.contains(key))
        dependencies.push_back(chosen[key]);
      chosen[key] = b;
    } else
      dependencies.push_back(b);
  }
  J result = {{"version", 3},
              {"root", pathstr(root)},
              {"bundles", J::array()},
              {"dependencyBundles", J::array()},
              {"errors", J::array()}};
  size_t i = 0;
  std::vector<std::pair<J, bool>> inventory;
  for (auto &[key, b] : chosen)
    inventory.push_back({b, false});
  for (auto &b : dependencies)
    inventory.push_back({b, true});
  for (auto &[b, dependencyOnly] : inventory) {
    if (job)
      job->update(float(i++) / std::max(size_t(1), inventory.size()),
                  "Reading bundle directories: " + std::to_string(i) + " / " +
                      std::to_string(inventory.size()));
    try {
      auto p = b.at("path").get<std::string>();
      if (previous.contains(p) && previous[p].at("size") == b.at("size") &&
          previous[p].at("mtime") == b.at("mtime"))
        b = previous[p];
      else {
        Bundle bundle(pathof(p));
        b["engine"] = bundle.engine;
        b["nodes"] = J::array();
        for (auto &n : bundle.nodes)
          b["nodes"].push_back(
              {{"name", n.name}, {"size", n.size}, {"flags", n.flags}});
      }
      result[dependencyOnly ? "dependencyBundles" : "bundles"].push_back(b);
    } catch (const std::exception &e) {
      errors.push_back({{"path", b["path"]}, {"error", e.what()}});
    }
  }
  result["errors"] = errors;
  if (job)
    job->check();
  auto tmp = dest;
  tmp += ".partial";
  write_json(tmp, result);
  if (fs::exists(dest)) {
    auto bak = dest;
    bak += ".previous";
    fs::copy_file(dest, bak, fs::copy_options::overwrite_existing);
    fs::remove(dest);
  }
  fs::rename(tmp, dest);
  return result;
}
std::string Object::id() const {
  return file->name + ":" + std::to_string(pid);
}
std::span<const uint8_t> Object::raw() const {
  return std::span(file->data).subspan(offset, size);
}
Source::Source(const fs::path &cp, const fs::path &dp)
    : catalog(read_json(cp)) {
  catalogDirectory = fs::absolute(cp).parent_path();
  auto schema = read_json(dp / "codm_types.json");
  for (auto it = schema.begin(); it != schema.end(); it++)
    types.emplace(std::stoi(it.key()), make_schema(it.value().at("nodes")));
  if (fs::exists(dp / "common_strings.json"))
    commonStrings = read_json(dp / "common_strings.json");
  std::unordered_map<std::string, const J *> selected;
  for (auto &b : catalog.at("bundles"))
    for (auto &n : b.at("nodes")) {
      auto key = lower(basename(n.at("name")));
      if (!selected.contains(key) || priority(b) > priority(*selected[key]))
        selected[key] = &b;
    }
  for (auto &[key, b] : selected)
    nodeBundles[key] = b->at("path");
  // An older package can contain a unique CAB still referenced by current
  // materials. Keep those exact identities reachable without browsing old
  // assets or replacing any CAB supplied by the active package selection.
  std::unordered_map<std::string, const J *> fallback;
  if (catalog.contains("dependencyBundles"))
    for (auto &b : catalog.at("dependencyBundles"))
      for (auto &n : b.at("nodes")) {
        auto key = lower(basename(n.at("name")));
        if (!selected.contains(key) &&
            (!fallback.contains(key) || priority(b) > priority(*fallback[key])))
          fallback[key] = &b;
      }
  for (auto &[key, b] : fallback)
    nodeBundles[key] = b->at("path");
  std::set<fs::path> roots;
  for (auto &b : catalog.at("bundles")) {
    auto path = pathof(b.at("path"));
    for (auto parent = path.parent_path();
         !parent.empty() && parent != parent.root_path();
         parent = parent.parent_path())
      if (lower(pathstr(parent.filename())) == "codm_data") {
        roots.insert(parent);
        break;
      }
  }
  if (roots.size() == 1)
    installedData = *roots.begin();
}
Bundle &Source::bundle(const std::string &p) {
  auto it = bundles.find(p);
  if (it == bundles.end())
    it = bundles.emplace(p, std::make_unique<Bundle>(pathof(p))).first;
  return *it->second;
}
std::unique_ptr<SerializedFile>
Source::parse_file(std::string name, std::string bundlePath, Bytes bytes) {
  auto f = std::make_unique<SerializedFile>();
  f->name = std::move(name);
  f->bundle = std::move(bundlePath);
  f->data = std::move(bytes);
  Reader r(f->data, true);
  auto metadata = r.get<uint32_t>();
  auto length = r.get<uint32_t>();
  auto version = r.get<uint32_t>();
  auto offset = r.get<uint32_t>();
  require(version == 17, "Unsupported serialized version " +
                             std::to_string(version) + " in " + f->name);
  f->big = r.get<uint8_t>() != 0;
  r.skip(3);
  require(length == f->data.size() && offset <= length && metadata <= length,
          "Invalid serialized header bounds");
  r.big = f->big;
  f->version = r.cstr();
  require(f->version.starts_with("5.6.4p4"),
          "Unverified Unity schema version " + f->version);
  r.get<int32_t>();
  bool embedded = r.get<uint8_t>() != 0;
  auto nt = r.count(23, 10000);
  for (int i = 0; i < nt; i++) {
    int cid = r.get<int32_t>();
    r.get<uint8_t>();
    r.get<int16_t>();
    if (cid == 114)
      r.skip(16);
    r.skip(16);
    f->classes.push_back(cid);
    f->schemas.emplace_back();
    if (embedded) {
      auto count = r.count(1, 100000);
      auto size = r.get<int32_t>();
      require(size >= 0, "Invalid type strings size");
      struct Raw {
        int level, flags, bytes, meta;
        uint32_t type, name;
      };
      std::vector<Raw> raw;
      for (int k = 0; k < count; k++) {
        r.get<int16_t>();
        Raw n;
        n.level = r.get<uint8_t>();
        n.flags = r.get<uint8_t>();
        n.type = r.get<uint32_t>();
        n.name = r.get<uint32_t>();
        n.bytes = r.get<int32_t>();
        r.get<int32_t>();
        n.meta = r.get<int32_t>();
        raw.push_back(n);
      }
      auto strings = r.take(size);
      auto str = [&](uint32_t off) {
        if (off & 0x80000000u) {
          auto key = std::to_string(off & 0x7fffffffu);
          // Preserve unknown names in unused layouts so readable objects in
          // the same file remain discoverable. tree() still rejects any
          // requested object whose layout contains an unresolved string.
          if (!commonStrings.contains(key))
            return std::string("$unknown_common_string_") + key;
          return commonStrings[key].get<std::string>();
        }
        require(off < strings.size(), "Type string offset out of bounds");
        Reader s(strings);
        s.seek(off);
        return s.cstr();
      };
      J nodes = J::array();
      for (auto &n : raw)
        nodes.push_back({{"m_Level", n.level},
                         {"m_Type", str(n.type)},
                         {"m_Name", str(n.name)},
                         {"m_ByteSize", n.bytes},
                         {"m_MetaFlag", n.meta}});
      f->schemas.back() = make_schema(nodes);
    }
  }
  int no = r.count(20);
  for (int i = 0; i < no; i++) {
    r.align();
    Object o;
    o.file = f.get();
    o.pid = r.get<int64_t>();
    o.offset = uint64_t(offset) + r.get<uint32_t>();
    o.size = r.get<uint32_t>();
    o.typeIndex = r.get<int32_t>();
    require(o.typeIndex >= 0 && o.typeIndex < int(f->classes.size()),
            "Invalid serialized type index");
    o.cid = f->classes[o.typeIndex];
    require(o.offset <= length && o.size <= length - o.offset,
            "Object outside serialized data");
    require(!f->objects.contains(o.pid), "Duplicate path ID");
    f->objects.emplace(o.pid, std::move(o));
  }
  int ns = r.count(12);
  for (int i = 0; i < ns; i++) {
    r.get<int32_t>();
    r.align();
    r.get<int64_t>();
  }
  int ne = r.count(22, 100000);
  for (int i = 0; i < ne; i++) {
    r.cstr();
    r.skip(16);
    r.get<int32_t>();
    f->externals.push_back(r.cstr());
  }
  r.cstr();
  require(r.pos <= offset, "Metadata overlaps object data");
  return f;
}
fs::path Source::installed_file(const std::string &key) const {
  if (installedData.empty())
    return {};
  // Resolve only recognized player data files under this installation. Paths
  // embedded in assets are never used as arbitrary filesystem destinations.
  auto base = key.ends_with(".ress") ? key.substr(0, key.size() - 5) : key;
  if (base == "unity default resources" || base == "unity_builtin_extra")
    return installedData / "Resources" / pathof(key);
  bool shared = base.starts_with("sharedassets") && base.ends_with(".assets");
  if (shared) {
    auto number = base.substr(12, base.size() - 12 - 7);
    shared = !number.empty() &&
             std::all_of(number.begin(), number.end(),
                         [](char c) { return c >= '0' && c <= '9'; });
  }
  if (base == "resources.assets" || base == "globalgamemanagers.assets" ||
      shared)
    return installedData / pathof(key);
  return {};
}
SerializedFile &Source::file(const std::string &name) {
  auto key = lower(basename(name));
  if (files.contains(key))
    return *files.at(key);
  if (!nodeBundles.contains(key) && !installedData.empty()) {
    auto path = installed_file(key);
    if (!key.ends_with(".ress") && !path.empty() && fs::is_regular_file(path)) {
      auto f = parse_file(key, pathstr(path), read_bytes(path));
      auto *out = f.get();
      files.emplace(key, std::move(f));
      looseFiles.insert(pathstr(path));
      return *out;
    }
  }
  require(nodeBundles.contains(key), "Missing serialized dependency: " + name);
  auto p = nodeBundles.at(key);
  auto &b = bundle(p);
  for (auto &n : b.nodes)
    if (lower(basename(n.name)) == key) {
      auto f = parse_file(n.name, p, b.read(n));
      auto *out = f.get();
      files.emplace(key, std::move(f));
      return *out;
    }
  throw std::runtime_error("CAB missing in selected bundle: " + name);
}
void Source::load(const std::string &p) {
  auto &b = bundle(p);
  for (auto &n : b.nodes)
    if (n.flags & 4)
      file(n.name);
}
Object *Source::ref(Object &owner, const J &p) {
  if (p.is_null() || !p.value("m_PathID", int64_t(0)))
    return nullptr;
  int fid = p.at("m_FileID");
  auto *f = owner.file;
  require(fid >= 0 && fid <= int(f->externals.size()),
          "External file ID outside bounds for " + owner.id());
  if (fid)
    f = &file(f->externals[fid - 1]);
  int64_t pid = p.at("m_PathID");
  auto it = f->objects.find(pid);
  require(it != f->objects.end(),
          "Missing object " + f->name + ":" + std::to_string(pid));
  return &it->second;
}
Object &Source::object(const std::string &id) {
  auto colon = id.find_last_of(':');
  require(colon != id.npos, "Expected CAB:path-id");
  auto &f = file(id.substr(0, colon));
  auto it = f.objects.find(std::stoll(id.substr(colon + 1)));
  require(it != f.objects.end(), "Missing asset " + id);
  return it->second;
}
const J &Source::tree(Object &o) {
  if (!o.decoded) {
    Reader r(o.raw(), o.file->big);
    auto &embedded = o.file->schemas[o.typeIndex];
    require(embedded.has_value() || types.contains(o.cid),
            "No schema for class " + std::to_string(o.cid));
    const auto &schema = embedded ? *embedded : types.at(o.cid);
    std::function<void(const TypeNode &)> validate = [&](const TypeNode &node) {
      require(!node.type.starts_with("$unknown_common_string_") &&
                  !node.name.starts_with("$unknown_common_string_"),
              "Unresolved embedded type string in selected asset " + o.id());
      for (const auto &child : node.children)
        validate(child);
    };
    validate(schema);
    auto v = parse_tree(r, schema);
    require(r.pos == o.size || (o.cid == 142 && r.pos + 4 == o.size),
            "Schema size mismatch for " + o.id() + ": read " +
                std::to_string(r.pos) + " / " + std::to_string(o.size));
    o.decoded = std::move(v);
  }
  return *o.decoded;
}
std::string Source::name(Object &o) {
  try {
    if (!o.decoded) {
      const auto &embedded = o.file->schemas.at(o.typeIndex);
      const TypeNode *schema = embedded                ? &*embedded
                               : types.contains(o.cid) ? &types.at(o.cid)
                                                       : nullptr;
      if (schema && !schema->children.empty()) {
        const auto &first = schema->children.front();
        if (first.name == "m_Name" && first.type == "string") {
          // Names are discovery metadata. Do not decode mesh buffers,
          // pixels or shader payloads merely to identify an object.
          // tree() still validates the entire selected object later.
          Reader r(o.raw(), o.file->big);
          return r.str();
        }
      }
    }
    return tree(o).value("m_Name", o.id());
  } catch (...) {
    Reader r(o.raw(), o.file->big);
    try {
      return r.str();
    } catch (...) {
      return o.id();
    }
  }
}
Bytes Source::resource(const std::string &name, uint64_t offset,
                       uint64_t size) {
  auto key = lower(basename(name));
  if (!nodeBundles.contains(key) && key.ends_with(".ress")) {
    auto path = installed_file(key);
    if (!path.empty() && fs::is_regular_file(path)) {
      auto bytes = read_bytes(path, offset, size);
      looseFiles.insert(pathstr(path));
      return bytes;
    }
  }
  require(nodeBundles.contains(key), "Missing streamed resource: " + name);
  auto &b = bundle(nodeBundles.at(key));
  for (auto &n : b.nodes)
    if (lower(basename(n.name)) == key)
      return b.read(n, offset, size);
  throw std::runtime_error("Missing streamed node: " + name);
}
std::set<std::string> Source::dependency_paths() const {
  auto paths = looseFiles;
  for (auto &[path, bundle] : bundles)
    paths.insert(path);
  return paths;
}
J Source::installed_data_state() const {
  J paths = J::object();
  if (!installedData.empty())
    for (auto directory : {installedData, installedData / "Resources"}) {
      if (!fs::is_directory(directory))
        continue;
      for (const auto &entry : fs::directory_iterator(directory)) {
        auto key = lower(pathstr(entry.path().filename()));
        if (!entry.is_regular_file() ||
            (installed_file(key).empty() &&
             !(directory == installedData && key == "globalgamemanagers")))
          continue;
        paths[pathstr(entry.path())] =
            J::array({entry.file_size(),
                      entry.last_write_time().time_since_epoch().count()});
      }
    }
  return {{"root", pathstr(installedData)}, {"files", paths}};
}
int Source::project_color_space() {
  if (colorSpace)
    return *colorSpace;
  require(
      !installedData.empty(),
      "Expected one installed PlayerSettings file for material color space");
  auto path = installedData / "globalgamemanagers";
  auto file = parse_file("globalgamemanagers", pathstr(path), read_bytes(path));
  looseFiles.insert(pathstr(path));
  for (auto &[pid, obj] : file->objects)
    if (obj.cid == 129) {
      Reader r(obj.raw(), file->big);
      for (auto &c : types.at(129).children) {
        auto value = parse_tree(r, c);
        if (c.name == "m_ActiveColorSpace") {
          int v = value;
          require(v == 0 || v == 1, "Invalid project color space");
          colorSpace = v;
          return v;
        }
      }
    }
  throw std::runtime_error("PlayerSettings contains no active color space");
}
} // namespace codm
