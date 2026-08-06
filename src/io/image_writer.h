/**
 * @file image_writer.h
 * @brief Minimal, dependency-free 8-bit PNG writer (used for screenshots).
 *
 * Self-contained encoder (uncompressed zlib "stored" blocks + CRC32/Adler32) so
 * the engine can save standard PNGs without pulling in stb_image_write or zlib.
 * Files are valid PNGs (viewable everywhere); not size-optimised.
 */
#ifndef HARUKA_IMAGE_WRITER_H
#define HARUKA_IMAGE_WRITER_H

#include <string>

namespace Haruka {

/**
 * @brief Writes an 8-bit PNG. `channels` = 3 (RGB) or 4 (RGBA).
 * @param pixels row-major, **top-to-bottom**, `w*h*channels` bytes.
 * @return false on bad args or file-open failure.
 */
bool writePNG(const std::string& path, int w, int h, int channels, const unsigned char* pixels);

/**
 * @brief Writes a 16-bit GRAYSCALE PNG (color type 0, bit depth 16, big-endian).
 * @param gray row-major, **top-to-bottom**, `w*h` samples in HOST byte order.
 * @return false on bad args or file-open failure.
 */
bool writePNG16(const std::string& path, int w, int h, const unsigned short* gray);

} // namespace Haruka

#endif // HARUKA_IMAGE_WRITER_H
