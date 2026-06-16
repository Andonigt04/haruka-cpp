/**
 * @file mapc.cpp
 * @brief Map compiler: packs a readable .scene (JSON) into an obfuscated .hmap.
 *
 * Usage:  mapc <input.scene> [output.hmap]
 * If the output is omitted, it is the input with the extension replaced by .hmap.
 *
 * The .hmap is what ships in release builds; the engine loads it via
 * SceneLoader::loadFromFile (which decodes zstd+XOR). Run this manually whenever
 * you want to repack a map.
 */
#include "core/scene/scene_loader.h"
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: mapc <input.scene> [output.hmap]\n");
        return 2;
    }
    std::string in = argv[1];
    std::string out = (argc >= 3) ? argv[2] : std::string{};
    if (out.empty()) {
        // Replace a trailing .scene with .hmap, or just append .hmap.
        const std::string ext = ".scene";
        if (in.size() >= ext.size() && in.compare(in.size()-ext.size(), ext.size(), ext) == 0)
            out = in.substr(0, in.size()-ext.size()) + ".hmap";
        else
            out = in + ".hmap";
    }

    if (!Haruka::SceneLoader::packToFile(in, out)) {
        std::fprintf(stderr, "mapc: failed to pack '%s' -> '%s'\n", in.c_str(), out.c_str());
        return 1;
    }
    std::printf("mapc: packed '%s' -> '%s'\n", in.c_str(), out.c_str());
    return 0;
}
