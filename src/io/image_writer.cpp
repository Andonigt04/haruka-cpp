#include "io/image_writer.h"

#include <cstdint>
#include <vector>
#include <fstream>
#include <filesystem>

namespace Haruka {
namespace {

uint32_t crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) { a = (a + data[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

void putBE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF);  v.push_back(x & 0xFF);
}

void chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    putBE32(out, (uint32_t)data.size());
    std::vector<uint8_t> tc(type, type + 4);
    tc.insert(tc.end(), data.begin(), data.end());
    out.insert(out.end(), tc.begin(), tc.end());
    putBE32(out, crc32(tc.data(), tc.size()));
}

// zlib stream wrapping DEFLATE "stored" (uncompressed) blocks of `raw`.
std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);              // zlib header (no dict)
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t block = std::min<size_t>(65535, raw.size() - pos);
        bool last = (pos + block >= raw.size());
        z.push_back(last ? 1 : 0);                     // BFINAL, BTYPE=00 (stored)
        uint16_t len = (uint16_t)block, nlen = (uint16_t)~len;
        z.push_back(len & 0xFF); z.push_back((len >> 8) & 0xFF);
        z.push_back(nlen & 0xFF); z.push_back((nlen >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + block);
        pos += block;
    }
    putBE32(z, adler32(raw.data(), raw.size()));       // Adler32 (big-endian)
    return z;
}

} // namespace

bool writePNG(const std::string& path, int w, int h, int channels, const unsigned char* pixels) {
    if (w <= 0 || h <= 0 || (channels != 3 && channels != 4) || !pixels) return false;

    // Filtered scanlines: each row prefixed with filter byte 0 (None).
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (1 + (size_t)w * channels));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const unsigned char* row = pixels + (size_t)y * w * channels;
        raw.insert(raw.end(), row, row + (size_t)w * channels);
    }

    std::vector<uint8_t> ihdr;
    putBE32(ihdr, (uint32_t)w); putBE32(ihdr, (uint32_t)h);
    ihdr.push_back(8);                                 // bit depth
    ihdr.push_back(channels == 4 ? 6 : 2);             // color type: 6=RGBA, 2=RGB
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0); // compression/filter/interlace

    std::vector<uint8_t> out = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", zlibStored(raw));
    chunk(out, "IEND", {});

    std::error_code ec;
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
    return (bool)f;
}

} // namespace Haruka
