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

    // ⚠️ EL DEFECTO ES `Medium`, NO `Low`, Y ES DELIBERADO. Este test esperaba `Low` "conservador"
    // desde antes de la deduplicación de capas del array de terreno: pasó de 9 capas a 4 (las 9
    // contenían 4 imágenes distintas — `grass_albedo` cargada CUATRO veces), y eso bajó el coste de
    // 4096 con mipmaps de ~1,6 GB a ~716 MB entre albedo y normal. Con esa cifra, 4k por defecto sí
    // cabe — y es además la única calidad cuyos assets VIENEN con el juego: 8k y 16k son paquetes
    // opcionales (ver `game_settings.h`).
    //
    // El test llevaba fallando desde el 2026-08-18 y era el único rojo del banco headless. No era un
    // bug del código: era una expectativa que el código había dejado atrás con motivo escrito.
    GraphicsSettings defaults;
    CHECK(defaults.terrainQuality == TerrainQuality::Medium,
          "el defecto es Medium (4k): la unica calidad cuyos assets vienen con el juego");
    // Y el preset de portátil SI baja a Low, que es donde vive lo "conservador".
    GraphicsSettings low; applyLowPreset(low);
    CHECK(low.terrainQuality == TerrainQuality::Low, "el preset Low si baja la calidad de terreno");
}
