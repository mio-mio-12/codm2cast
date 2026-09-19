#include "materials.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <texture2ddecoder/src/Texture2DDecoder/astc.h>
#include <texture2ddecoder/src/Texture2DDecoder/atc.h>
#include <texture2ddecoder/src/Texture2DDecoder/bcn.h>
#include <texture2ddecoder/src/Texture2DDecoder/crunch.h>
#include <texture2ddecoder/src/Texture2DDecoder/etc.h>
#include <texture2ddecoder/src/Texture2DDecoder/pvrtc.h>
#include <texture2ddecoder/src/Texture2DDecoder/unitycrunch.h>
namespace codm {
std::string sha256(std::span<const uint8_t> bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
            "Cannot open SHA256 provider");
    std::array<uint8_t, 32> digest{};
    auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
                             ULONG(bytes.size()), digest.data(), ULONG(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    require(status >= 0, "SHA256 failed");
    static const char *hex = "0123456789abcdef";
    std::string out;
    for (auto c : digest) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}
static size_t level_size(int fmt, int w, int h) {
    if (fmt == 1 || fmt == 63)
        return size_t(w) * h;
    if (fmt == 3)
        return size_t(w) * h * 3;
    if (fmt == 4 || fmt == 5 || fmt == 14)
        return size_t(w) * h * 4;
    if (fmt == 2 || fmt == 7 || fmt == 9 || fmt == 13 || fmt == 62)
        return size_t(w) * h * 2;
    if (fmt >= 48 && fmt <= 59) {
        int blocks[] = {4, 5, 6, 8, 10, 12};
        int b = blocks[(fmt - 48) % 6];
        return size_t((w + b - 1) / b) * ((h + b - 1) / b) * 16;
    }
    if (fmt >= 30 && fmt <= 33) {
        bool two = fmt <= 31;
        return size_t(std::max(w, two ? 16 : 8)) * std::max(h, 8) * (two ? 2 : 4) / 8;
    }
    bool small = fmt == 10 || fmt == 26 || fmt == 34 || fmt == 35 || fmt == 41 || fmt == 42 ||
                 fmt == 45 || fmt == 46;
    return size_t((w + 3) / 4) * ((h + 3) / 4) * (small ? 8 : 16);
}
Image decode_texture(Source &source, Object &o, int maxDimension) {
    require(o.cid == 28, "Expected Texture2D");
    const auto &t = source.tree(o);
    int w = t.at("m_Width"), h = t.at("m_Height"), fmt = t.at("m_TextureFormat"),
        mips = t.value("m_MipCount", 1);
    require(w > 0 && h > 0 && w <= 16384 && h <= 16384 && uint64_t(w) * h <= 67108864,
            "Texture dimensions exceed bound: " + o.id());
    require(t.value("m_ImageCount", 1) == 1 && t.value("m_TextureDimension", 2) == 2,
            "Only ordinary 2D textures are supported");
    int level = 0;
    size_t offset = 0;
    bool crunch = fmt == 28 || fmt == 29 || fmt == 64 || fmt == 65;
    while (maxDimension > 0 && std::max(w, h) > maxDimension && level + 1 < mips) {
        offset += level_size(fmt, w, h);
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
        level++;
    }
    Bytes bytes;
    const auto &stream = t.at("m_StreamData");
    auto streamPath = stream.at("path").get<std::string>();
    size_t need = level_size(fmt, w, h);
    if (!streamPath.empty()) {
        uint64_t size = stream.at("size"), base = stream.at("offset");
        require(crunch || (offset <= size && need <= size - offset),
                "Texture mip exceeds streamed size");
        bytes = source.resource(streamPath, base + (crunch ? 0 : offset), crunch ? size : need);
    } else {
        auto &raw = t.at("image data").get_binary();
        require(crunch || (offset <= raw.size() && need <= raw.size() - offset),
                "Texture mip exceeds inline data");
        if (crunch)
            bytes.assign(raw.begin(), raw.end());
        else
            bytes.assign(raw.begin() + offset, raw.begin() + offset + need);
    }
    if (crunch) {
        void *raw = nullptr;
        uint32_t count = 0;
        bool ok =
            (fmt == 64 || fmt == 65)
                ? unity_crunch_unpack_level(bytes.data(), uint32_t(bytes.size()), level, &raw, &count)
                : crunch_unpack_level(bytes.data(), uint32_t(bytes.size()), level, &raw, &count);
        std::unique_ptr<uint8_t[]> decoded(static_cast<uint8_t *>(raw));
        require(ok && raw, "Crunch decompression failed");
        bytes.assign(decoded.get(), decoded.get() + count);
        fmt = fmt == 28 ? 10 : fmt == 29 ? 12 : fmt == 64 ? 34 : 47;
        need = level_size(fmt, w, h);
        require(bytes.size() >= need, "Truncated Crunch output");
    }
    Image im{w, h, Bytes(size_t(w) * h * 4)};
    bool bgr = false;
    auto *out = reinterpret_cast<uint32_t *>(im.pixels.data());
    int ok = 1;
    switch (fmt) {
    case 10:
        ok = decode_bc1(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 12:
        ok = decode_bc3(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 24:
        ok = decode_bc6(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 25:
        ok = decode_bc7(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 26:
        ok = decode_bc4(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 27:
        ok = decode_bc5(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 34:
        ok = decode_etc1(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 45:
        ok = decode_etc2(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 46:
        ok = decode_etc2a1(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 47:
        ok = decode_etc2a8(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 41:
        ok = decode_eacr(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 42:
        ok = decode_eacr_signed(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 43:
        ok = decode_eacrg(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 44:
        ok = decode_eacrg_signed(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 35:
        ok = decode_atc_rgb4(bytes.data(), w, h, out);
        bgr = true;
        break;
    case 36:
        ok = decode_atc_rgba8(bytes.data(), w, h, out);
        bgr = true;
        break;
    default:
        if (fmt >= 48 && fmt <= 59) {
            int blocks[] = {4, 5, 6, 8, 10, 12};
            int b = blocks[(fmt - 48) % 6];
            ok = decode_astc(bytes.data(), w, h, b, b, out);
            bgr = true;
        } else if (fmt >= 30 && fmt <= 33) {
            require((w & (w - 1)) == 0 && (h & (h - 1)) == 0, "PVRTC dimensions must be powers of two");
            ok = decode_pvrtc(bytes.data(), w, h, out, fmt <= 31);
            bgr = true;
        } else {
            require(fmt == 1 || fmt == 2 || fmt == 3 || fmt == 4 || fmt == 5 || fmt == 7 || fmt == 9 ||
                        fmt == 13 || fmt == 14 || fmt == 62 || fmt == 63,
                    "Unsupported texture format " + std::to_string(fmt) + " for " + o.id());
            for (size_t i = 0; i < size_t(w) * h; i++) {
                auto *p = &im.pixels[i * 4];
                p[3] = 255;
                if (fmt == 4)
                    std::memcpy(p, &bytes[i * 4], 4);
                else if (fmt == 14) {
                    p[0] = bytes[i * 4 + 2];
                    p[1] = bytes[i * 4 + 1];
                    p[2] = bytes[i * 4];
                    p[3] = bytes[i * 4 + 3];
                } else if (fmt == 5) {
                    p[0] = bytes[i * 4 + 1];
                    p[1] = bytes[i * 4 + 2];
                    p[2] = bytes[i * 4 + 3];
                    p[3] = bytes[i * 4];
                } else if (fmt == 3)
                    std::memcpy(p, &bytes[i * 3], 3);
                else if (fmt == 1) {
                    p[0] = p[1] = p[2] = 255;
                    p[3] = bytes[i];
                } else if (fmt == 63) {
                    p[0] = bytes[i];
                    p[1] = p[2] = 0;
                } else if (fmt == 62) {
                    p[0] = bytes[i * 2];
                    p[1] = bytes[i * 2 + 1];
                    p[2] = 0;
                } else {
                    uint16_t v = bytes[i * 2] | (uint16_t(bytes[i * 2 + 1]) << 8);
                    if (fmt == 7) {
                        p[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
                        p[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
                        p[2] = uint8_t((v & 31) * 255 / 31);
                    } else if (fmt == 9) {
                        p[0] = uint8_t(v >> 8);
                        p[1] = p[2] = 0;
                    } else if (fmt == 13) {
                        p[0] = uint8_t((v & 15) * 17);
                        p[1] = uint8_t(((v >> 4) & 15) * 17);
                        p[2] = uint8_t(((v >> 8) & 15) * 17);
                        p[3] = uint8_t(((v >> 12) & 15) * 17);
                    } else {
                        p[0] = uint8_t(((v >> 8) & 15) * 17);
                        p[1] = uint8_t(((v >> 4) & 15) * 17);
                        p[2] = uint8_t((v & 15) * 17);
                        p[3] = uint8_t(((v >> 12) & 15) * 17);
                    }
                }
            }
        }
    }
    require(ok != 0, "Texture codec failed: " + o.id());
    if (bgr)
        for (size_t i = 0; i < im.pixels.size(); i += 4)
            std::swap(im.pixels[i], im.pixels[i + 2]);
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w * 4; x++)
            std::swap(im.pixels[size_t(y) * w * 4 + x], im.pixels[size_t(h - 1 - y) * w * 4 + x]);
    if (maxDimension > 0 && std::max(w, h) > maxDimension) {
        float ratio = float(maxDimension) / std::max(w, h);
        return resize_image(im, std::max(1, int(w * ratio)), std::max(1, int(h * ratio)));
    }
    return im;
}
Image resize_image(const Image &im, int w, int h) {
    if (w == im.width && h == im.height)
        return im;
    require(w > 0 && h > 0, "Invalid resize dimensions");
    struct Kernel {
        int first;
        std::vector<int32_t> weights;
    };
    auto kernels = [](int from, int to) {
        std::vector<Kernel> out;
        const double scale = double(from) / to, support = std::max(1., scale);
        for (int x = 0; x < to; x++) {
            double center = (x + .5) * scale;
            int first = std::max(0, int(center - support + .5)),
                end = std::min(from, int(center + support + .5));
            std::vector<double> weights;
            double sum = 0;
            for (int k = first; k < end; k++) {
                double v = std::max(0., 1 - std::abs((k + .5 - center) / support));
                weights.push_back(v);
                sum += v;
            }
            require(sum > 0, "Empty resize kernel");
            Kernel kernel{first, {}};
            for (double v : weights)
                kernel.weights.push_back(int32_t(v / sum * (1 << 22) + .5));
            out.push_back(std::move(kernel));
        }
        return out;
    };
    // Filter packed channels independently. Alpha is data, never a premultiplication mask.
    auto kx = kernels(im.width, w), ky = kernels(im.height, h);
    Image horizontal{w, im.height, Bytes(size_t(w) * im.height * 4)},
        out{w, h, Bytes(size_t(w) * h * 4)};
    for (int y = 0; y < im.height; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 4; c++) {
                int64_t acc = 1 << 21;
                const auto &k = kx[x];
                for (size_t j = 0; j < k.weights.size(); j++)
                    acc +=
                        int64_t(k.weights[j]) * im.pixels[(size_t(y) * im.width + k.first + j) * 4 + c];
                horizontal.pixels[(size_t(y) * w + x) * 4 + c] =
                    uint8_t(std::clamp(acc >> 22, int64_t(0), int64_t(255)));
            }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 4; c++) {
                int64_t acc = 1 << 21;
                const auto &k = ky[y];
                for (size_t j = 0; j < k.weights.size(); j++)
                    acc += int64_t(k.weights[j]) *
                           horizontal.pixels[(size_t(k.first + j) * w + x) * 4 + c];
                out.pixels[(size_t(y) * w + x) * 4 + c] =
                    uint8_t(std::clamp(acc >> 22, int64_t(0), int64_t(255)));
            }
    return out;
}
} // namespace codm
