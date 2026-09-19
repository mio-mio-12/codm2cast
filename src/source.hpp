#pragma once
#include "common.hpp"
namespace codm {
struct TypeNode {
    std::string type, name;
    int size = -1, flags = 0;
    std::vector<TypeNode> children;
};
TypeNode make_schema(const J &nodes);
J parse_tree(Reader &r, const TypeNode &node, size_t depth = 0);
struct BundleNode {
    std::string name;
    uint64_t offset = 0, size = 0;
    uint32_t flags = 0;
};
struct Block {
    uint32_t unpacked = 0, packed = 0;
    uint16_t flags = 0;
    uint64_t offset = 0, fileOffset = 0;
};
struct Bundle {
    fs::path path;
    std::string engine;
    uint32_t flags = 0;
    std::vector<BundleNode> nodes;
    std::vector<Block> blocks;
    std::unordered_map<size_t, Bytes> cache;
    size_t cacheBytes = 0;
    explicit Bundle(const fs::path &path);
    Bytes read(const BundleNode &node, uint64_t offset = 0, uint64_t size = UINT64_MAX);
};
struct SerializedFile;
struct Object {
    SerializedFile *file = nullptr;
    int64_t pid = 0;
    int cid = 0, typeIndex = 0;
    size_t offset = 0, size = 0;
    std::optional<J> decoded;
    std::string id() const;
    std::span<const uint8_t> raw() const;
};
struct SerializedFile {
    std::string name, bundle, version;
    Bytes data;
    bool big = false;
    std::vector<int> classes;
    std::vector<std::optional<TypeNode>> schemas;
    std::vector<std::string> externals;
    std::map<int64_t, Object> objects;
};
std::tuple<int, int, int64_t, std::string> priority(const J &b);
std::string hint(const std::string &path);
J scan_catalog(const fs::path &root, const fs::path &destination, JobContext *job = nullptr);
class Source {
  public:
    J catalog;
    std::unordered_map<int, TypeNode> types;
    J commonStrings;
    fs::path installedData;
    fs::path catalogDirectory;
    std::unordered_map<std::string, std::string> nodeBundles;
    std::unordered_map<std::string, std::unique_ptr<Bundle>> bundles;
    std::unordered_map<std::string, std::unique_ptr<SerializedFile>> files;
    explicit Source(const fs::path &catalogPath, const fs::path &dataRoot);
    Bundle &bundle(const std::string &p);
    SerializedFile &file(const std::string &name);
    void load(const std::string &p);
    Object *ref(Object &owner, const J &p);
    Object &object(const std::string &id);
    const J &tree(Object &object);
    std::string name(Object &o);
    Bytes resource(const std::string &name, uint64_t offset, uint64_t size);
    std::set<std::string> dependency_paths() const;
    J installed_data_state() const;
    int project_color_space();
    void clear() {
        files.clear();
        bundles.clear();
        looseFiles.clear();
        colorSpace.reset();
    }

  private:
    std::optional<int> colorSpace;
    std::set<std::string> looseFiles;
    fs::path installed_file(const std::string &key) const;
    std::unique_ptr<SerializedFile> parse_file(std::string name, std::string bundle, Bytes data);
};
} // namespace codm
