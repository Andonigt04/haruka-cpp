/**
 * @file blob_codec.h
 * @brief Compress + obfuscate byte blobs (zstd + keystream XOR).
 *
 * Used by saves and (later) asset packs so on-disk data is small and not human
 * readable. Honest scope: the XOR key ships in the binary, so this stops casual
 * inspection/editing — it is NOT strong DRM (impossible for local single-player,
 * where the key must travel with the game). zstd already makes the payload
 * unreadable; the XOR pass adds a cheap second layer and a format marker.
 *
 * Layout produced by encode():
 *   [4]  magic 'H','B','1', 0
 *   [4]  rawSize  (uint32 LE)   — uncompressed length
 *   [N]  zstd(payload) XOR keystream(key)
 */
#pragma once

#include <vector>
#include <string>
#include <cstdint>

namespace Haruka::codec {

using Bytes = std::vector<uint8_t>;

/** @brief Compress (zstd) then obfuscate (XOR keystream from `key`). */
Bytes encode(const Bytes& raw, const std::string& key);

/** @brief Inverse of encode(). Returns false on bad magic / corrupt data. */
bool decode(const Bytes& blob, const std::string& key, Bytes& out);

// String convenience wrappers (UTF-8 text payloads like JSON).
Bytes encodeString(const std::string& text, const std::string& key);
bool  decodeString(const Bytes& blob, const std::string& key, std::string& out);

} // namespace Haruka::codec
