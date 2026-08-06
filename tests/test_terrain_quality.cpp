#include "test_common.h"
#include "settings/game_settings.h"

using namespace Haruka::Settings;

static const char* kResLabels[] = { "16k", "8k", "4k", "hd" };

static int qualityStartIndex(TerrainQuality q) {
    switch (q) {
        case TerrainQuality::Ultra:  return 0;
        case TerrainQuality::High:   return 1;
        case TerrainQuality::Medium: return 2;
        default:
        case TerrainQuality::Low:    return 3;
    }
}

void test_terrain_quality_mapping() {
    beginTest("terrain_quality_mapping");

    CHECK(qualityStartIndex(TerrainQuality::Low)    == 3, "Low → hd");
    CHECK(qualityStartIndex(TerrainQuality::Medium) == 2, "Medium → 4k");
    CHECK(qualityStartIndex(TerrainQuality::High)   == 1, "High → 8k");
    CHECK(qualityStartIndex(TerrainQuality::Ultra)  == 0, "Ultra → 16k");

    CHECK(std::string(kResLabels[qualityStartIndex(TerrainQuality::Low)])    == "hd",  "Low label");
    CHECK(std::string(kResLabels[qualityStartIndex(TerrainQuality::Medium)]) == "4k",  "Medium label");
    CHECK(std::string(kResLabels[qualityStartIndex(TerrainQuality::High)])   == "8k",  "High label");
    CHECK(std::string(kResLabels[qualityStartIndex(TerrainQuality::Ultra)])  == "16k", "Ultra label");

    // Default quality should be Low (conservador)
    GraphicsSettings defaults;
    CHECK(defaults.terrainQuality == TerrainQuality::Low, "default quality is Low");
}
