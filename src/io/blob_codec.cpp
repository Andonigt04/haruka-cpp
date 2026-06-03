#include "blob_codec.h"
#include <zstd.h>
#include <cstring>

namespace Haruka::codec {

static const uint8_t kMagic[4] = { 'H', 'B', '1', 0 };

// Cheap keystream from the key (xorshift seeded by a hash of the key). Not crypto;
// just spreads the key over the whole blob so bytes aren't trivially recoverable.
static void xorKeystream(Bytes& data, const std::string& key) {
    uint64_t s = 1469598103934665603ULL; // FNV offset basis
    for (char c : key) { s ^= (uint8_t)c; s *= 1099511628211ULL; }
    if (s == 0) s = 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < data.size(); ++i) {
        // xorshift64*
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        uint64_t r = s * 0x2545F4914F6CDD1DULL;
        data[i] ^= (uint8_t)(r >> 33);
    }
}

Bytes encode(const Bytes& raw, const std::string& key) {
    size_t bound = ZSTD_compressBound(raw.size());
    Bytes comp(bound);
    size_t n = ZSTD_compress(comp.data(), bound,
                             raw.data(), raw.size(), /*level*/ 9);
    if (ZSTD_isError(n)) return {};
    comp.resize(n);
    xorKeystream(comp, key);

    Bytes out;
    out.reserve(8 + comp.size());
    out.insert(out.end(), kMagic, kMagic + 4);
    uint32_t rawSize = (uint32_t)raw.size();
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(rawSize >> (i * 8)));
    out.insert(out.end(), comp.begin(), comp.end());
    return out;
}

bool decode(const Bytes& blob, const std::string& key, Bytes& out) {
    if (blob.size() < 8) return false;
    if (std::memcmp(blob.data(), kMagic, 4) != 0) return false;
    uint32_t rawSize = 0;
    for (int i = 0; i < 4; ++i) rawSize |= (uint32_t)blob[4 + i] << (i * 8);

    Bytes comp(blob.begin() + 8, blob.end());
    xorKeystream(comp, key);

    out.resize(rawSize);
    size_t n = ZSTD_decompress(out.data(), rawSize, comp.data(), comp.size());
    if (ZSTD_isError(n) || n != rawSize) { out.clear(); return false; }
    return true;
}

Bytes encodeString(const std::string& text, const std::string& key) {
    Bytes raw(text.begin(), text.end());
    return encode(raw, key);
}

bool decodeString(const Bytes& blob, const std::string& key, std::string& out) {
    Bytes raw;
    if (!decode(blob, key, raw)) return false;
    out.assign(raw.begin(), raw.end());
    return true;
}

} // namespace Haruka::codec
