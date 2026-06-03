/**
 * @file asset_paths.h
 * @brief Resolves where game data (shaders, maps) lives, for dev vs release.
 *
 *  DEV (default):     assets are flat next to the executable —
 *                       <exe>/shaders/  <exe>/scenes/
 *  RELEASE (-DHARUKA_RELEASE): assets live under an assets/ root —
 *                       <exe>/assets/shaders/  <exe>/assets/maps/
 *
 * The executable cd's to its own dir at startup (main.cpp), so relative paths
 * resolve correctly either way. These helpers centralise the dev/release
 * difference so loaders don't hard-code layout.
 */
#pragma once
#include <string>

namespace Haruka {

class AssetPaths {
public:
    /** @brief Root prefix for shader files (with trailing separator). */
    static std::string shaders() {
#ifdef HARUKA_RELEASE
        return "assets/shaders/";
#else
        return "shaders/";
#endif
    }

    /** @brief Root prefix + directory for map/scene files. */
    static std::string maps() {
#ifdef HARUKA_RELEASE
        return "assets/maps/";
#else
        return "scenes/";
#endif
    }

    /** @brief True when built for distribution (packed assets, no sources). */
    static constexpr bool isRelease() {
#ifdef HARUKA_RELEASE
        return true;
#else
        return false;
#endif
    }
};

} // namespace Haruka
