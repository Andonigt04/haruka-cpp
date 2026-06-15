#pragma once

#include <string>

namespace Haruka::Settings {

enum class TextureQuality : int { Low = 0, Medium = 1, High = 2, Ultra = 3 };
enum class ShadowQuality  : int { Off = 0, Low = 1, Medium = 2, High = 3 };
enum class AntialiasingMode : int { None = 0, FXAA = 1, TAA = 2 };
// Drives simulated-water mesh density (river / shallow-water grid resolution).
enum class WaterQuality   : int { Low = 0, Medium = 1, High = 2, Ultra = 3 };
// Terrain LOD detail: lower = fewer/larger chunks = cheaper (CPU/GPU/RAM).
enum class TerrainQuality : int { Low = 0, Medium = 1, High = 2, Ultra = 3 };
// Densidad/alcance de props (árboles/rocas). Lower = menos/más cerca = menos CPU.
enum class FoliageQuality : int { Low = 0, Medium = 1, High = 2, Ultra = 3 };

struct GraphicsSettings {
    TextureQuality  textureQuality  = TextureQuality::High;
    ShadowQuality   shadowQuality   = ShadowQuality::Medium;
    AntialiasingMode antialiasing   = AntialiasingMode::TAA;
    WaterQuality    waterQuality    = WaterQuality::Medium;
    TerrainQuality  terrainQuality  = TerrainQuality::High; // High = current detail
    FoliageQuality  foliageQuality  = FoliageQuality::Medium; // densidad/alcance de árboles/rocas
    float           fov             = 90.0f;
    float           renderScale     = 1.0f;
    bool            vsync           = false;
    bool            ssao            = true;
    bool            bloom           = true;
    float           bloomThreshold  = 0.8f; // luma above which pixels bloom
    float           bloomStrength   = 0.7f; // additive bloom intensity
    bool            motionBlur      = false;
    bool            fog             = false;  // niebla atmosférica del terreno (consola: fog 0|1)
    int             chunkMemoryMB   = 2048;  // terrain chunk cache memory budget (MB). 512 en preset High.
    int             maxFps          = 60;    // frame-rate cap (0 = uncapped)
};

// One-click presets. "Low/Laptop" trades quality for frame time + battery/heat;
// "High" restores the desktop defaults.
inline void applyLowPreset(GraphicsSettings& g) {
    g.renderScale    = 0.75f;
    g.terrainQuality = TerrainQuality::Low;
    g.foliageQuality = FoliageQuality::Low;
    g.waterQuality   = WaterQuality::Low;
    g.textureQuality = TextureQuality::Medium;
    g.antialiasing   = AntialiasingMode::None;
    g.bloom          = false;
    g.ssao           = false;
    g.motionBlur     = false;
    g.vsync          = true;   // cap to refresh; avoids 1000fps burning the GPU
    g.maxFps         = 60;
    g.chunkMemoryMB  = 256;
}
inline void applyHighPreset(GraphicsSettings& g) {
    g.renderScale    = 1.0f;
    g.terrainQuality = TerrainQuality::High;
    g.foliageQuality = FoliageQuality::High;
    g.waterQuality   = WaterQuality::Medium;
    g.textureQuality = TextureQuality::High;
    g.antialiasing   = AntialiasingMode::FXAA;
    g.bloom          = true;
    g.vsync          = false;
    g.maxFps         = 0;
    g.chunkMemoryMB  = 512;
}

struct AudioSettings {
    float masterVolume = 1.0f;
    float musicVolume  = 0.6f;
    float sfxVolume    = 1.0f;
    // Dispositivos elegidos (NOMBRE; vacío = predeterminado del sistema). input = micro
    // (voz/conjuros), output = altavoces (efectos/propagación).
    std::string inputDevice;
    std::string outputDevice;
};

} // namespace Haruka::Settings
