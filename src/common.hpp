#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
namespace codm {
namespace fs = std::filesystem;
using J = nlohmann::json;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
inline void require(bool test, const char *message) {
    if (!test)
        throw std::runtime_error(message);
}
inline void require(bool test, const std::string &message) {
    if (!test)
        throw std::runtime_error(message);
}
inline std::string lower(std::string s) {
    for (auto &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline std::string pathstr(const fs::path &p) {
    auto s = p.u8string();
    return {reinterpret_cast<const char *>(s.data()), s.size()};
}
inline fs::path pathof(const std::string &s) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}
inline std::string basename(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s.substr(s.find_last_of('/') + 1);
}
inline Bytes read_bytes(const fs::path &p, uint64_t offset = 0, uint64_t count = UINT64_MAX) {
    std::ifstream in(p, std::ios::binary);
    require(bool(in), "Cannot open " + pathstr(p));
    auto size = fs::file_size(p);
    require(offset <= size, "File offset outside " + pathstr(p));
    if (count == UINT64_MAX)
        count = size - offset;
    require(count <= size - offset && count <= 1024ull * 1024 * 1024,
            "File range outside allowed bounds: " + pathstr(p));
    Bytes b(static_cast<size_t>(count));
    in.seekg(offset);
    in.read(reinterpret_cast<char *>(b.data()), count);
    require(uint64_t(in.gcount()) == count, "Short read: " + pathstr(p));
    return b;
}
inline J read_json(const fs::path &p) {
    std::ifstream in(p);
    require(bool(in), "Cannot read " + pathstr(p));
    return J::parse(in);
}
void write_json(const fs::path &p, const J &j);
inline double seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
struct JobContext {
    std::atomic_bool cancel{false};
    std::atomic<float> progress{0};
    std::mutex mutex;
    std::string label, stage;
    void set_stage(std::string s) {
        check();
        std::lock_guard lock(mutex); stage=std::move(s);
    }
    void check() const { require(!cancel.load(), "Cancelled"); }
    void update(float p, std::string s) {
        check();
        progress = p;
        std::lock_guard lock(mutex);
        label = std::move(s);
    }
};
struct Reader {
    std::span<const uint8_t> data;
    size_t pos = 0;
    bool big = false;
    explicit Reader(std::span<const uint8_t> d, bool be = false) : data(d), big(be) {}
    void seek(size_t p) {
        require(p <= data.size(), "Seek outside buffer");
        pos = p;
    }
    std::span<const uint8_t> take(size_t n) {
        if (pos > data.size() || n > data.size() - pos)
            throw std::runtime_error("Truncated input at " + std::to_string(pos) + " requesting " +
                                     std::to_string(n) + " of " + std::to_string(data.size()));
        auto out = data.subspan(pos, n);
        pos += n;
        return out;
    }
    void skip(size_t n) { take(n); }
    void align(size_t n = 4) { skip((n - pos % n) % n); }
    template <class T> T get() {
        auto b = take(sizeof(T));
        std::array<uint8_t, sizeof(T)> a;
        std::copy(b.begin(), b.end(), a.begin());
        if (big == (std::endian::native == std::endian::little))
            std::reverse(a.begin(), a.end());
        return std::bit_cast<T>(a);
    }
    std::string cstr() {
        std::string s;
        for (size_t i = 0; i < 4096; ++i) {
            auto c = get<uint8_t>();
            if (!c)
                return s;
            s += char(c);
        }
        throw std::runtime_error("Unterminated string");
    }
    std::string str() {
        auto n = get<int32_t>();
        require(n >= 0 && n <= 16 * 1024 * 1024, "Invalid string size");
        auto b = take(n);
        std::string s(b.begin(), b.end());
        align();
        return s;
    }
    int32_t count(size_t minBytes = 1, size_t limit = 10000000) {
        auto n = get<int32_t>();
        require(n >= 0 && size_t(n) <= limit && size_t(n) <= (data.size() - pos) / minBytes,
                "Invalid array count at " + std::to_string(pos - 4));
        return n;
    }
};
inline uint64_t hash64(std::span<const uint8_t> b) {
    uint64_t h = 14695981039346656037ull;
    for (auto c : b) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}
inline uint64_t hash64(const std::string &s) {
    return hash64(std::span(reinterpret_cast<const uint8_t *>(s.data()), s.size()));
}
inline std::string hex64(uint64_t v) {
    std::ostringstream s;
    s << std::hex << v;
    return s.str();
}
inline uint32_t crc32(const std::string &s) {
    uint32_t c = ~0u;
    for (uint8_t b : s) {
        c ^= b;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}
inline bool natural_less(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(uint8_t(a[i])) && std::isdigit(uint8_t(b[j]))) {
            size_t ia = i, jb = j;
            while (i < a.size() && std::isdigit(uint8_t(a[i])))
                i++;
            while (j < b.size() && std::isdigit(uint8_t(b[j])))
                j++;
            auto x = a.substr(ia, i - ia), y = b.substr(jb, j - jb);
            auto trim = [](std::string v) {
                auto p = v.find_first_not_of('0');
                return p == v.npos ? std::string("0") : v.substr(p);
            };
            auto xx = trim(x), yy = trim(y);
            if (xx.size() != yy.size())
                return xx.size() < yy.size();
            if (xx != yy)
                return xx < yy;
            if (x.size() != y.size())
                return x.size() < y.size();
        } else {
            auto x = std::tolower(uint8_t(a[i++])), y = std::tolower(uint8_t(b[j++]));
            if (x != y)
                return x < y;
        }
    }
    return i == a.size() && j != b.size();
}
inline void remove_owned_tree(const fs::path &path, const fs::path &parent) {
    std::error_code ec;
    auto root = fs::weakly_canonical(parent, ec);
    if (ec)
        return;
    auto target = fs::weakly_canonical(path, ec);
    if (ec || target == root || target.parent_path() != root)
        return;
    if (fs::is_symlink(fs::symlink_status(path, ec)))
        return;
    fs::remove_all(target, ec);
}
inline bool wildcard(std::string pattern, std::string name) {
    pattern = lower(pattern);
    name = lower(name);
    if (pattern.find_first_of("*?") == pattern.npos)
        pattern = "*" + pattern + "*";
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
            p++;
            n++;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++mark;
        } else
            return false;
    }
    while (p < pattern.size() && pattern[p] == '*')
        p++;
    return p == pattern.size();
}
} // namespace codm
