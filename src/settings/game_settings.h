#pragma once

#include <string>

namespace Haruka::Settings {

enum class TextureQuality : int { Low = 0, Medium = 1, High = 2, Ultra = 3 };
enum class ShadowQuality  : int { Off = 0, Low = 1, Medium = 2, High = 3 };
enum class AntialiasingMode : int { None = 0, FXAA = 1, TAA = 2 };
// Drives simulated-water mesh density (river / shallow-water grid resolution).
enum class WaterQuality   : int { Low = 0, Medium = 1, High = 2, Ultra = 3 };
// Resolución de texturas de terreno: Low=hd(2048) Medium=4k(4096) High=8k(8192) Ultra=16k(16384).
// Carga la primera disponible desde la calidad seleccionada hacia abajo.
enum class TerrainQuality : int { Low = 0, Medium = 1, High = 2, Ultra = 3 };
// Densidad/alcance de props (árboles/rocas). Lower = menos/más cerca = menos CPU.
enum class FoliageQuality : int { Low = 0, Medium = 1, High = 2, Ultra = 3 };

// Modo de ventana. Windowed = ventana normal con borde; Borderless = sin borde a
// pantalla completa (windowed fullscreen); Fullscreen = pantalla completa (SDL).
enum class WindowMode : int { Windowed = 0, Borderless = 1, Fullscreen = 2 };

// API gráfica (backend del RHI). Se elige en el ARRANQUE (el device se crea entonces):
// cambiarla requiere REINICIAR el juego. Vulkan cae a OpenGL si no está disponible
// (ver el fallback en rhi_device.cpp).
enum class RenderBackend : int { OpenGL = 0, Vulkan = 1 };

struct GraphicsSettings {
    WindowMode      windowMode      = WindowMode::Fullscreen;
    RenderBackend   renderBackend   = RenderBackend::OpenGL; // API gráfica (requiere reinicio)
    TextureQuality  textureQuality  = TextureQuality::High;
    ShadowQuality   shadowQuality   = ShadowQuality::Medium;
    AntialiasingMode antialiasing   = AntialiasingMode::TAA;
    WaterQuality    waterQuality    = WaterQuality::Medium;
    TerrainQuality  terrainQuality  = TerrainQuality::Low; // Low = hd (2048px); sube a 4k/8k/16k según VRAM
    FoliageQuality  foliageQuality  = FoliageQuality::Medium; // densidad/alcance de árboles/rocas
    float           fov             = 90.0f;
    float           renderScale     = 1.0f;
    bool            vsync           = false;
    bool            ssao            = true;
    bool            bloom           = true;
    float           bloomThreshold  = 0.95f; // luma above which pixels bloom. OJO: el color llega ya
                                            // tonemapeado+gamma (0..1), así que 0.8 hacía brillar el 20%
                                            // MÁS CLARO de la imagen — ladrillo al sol, nubes… todo
                                            // "reluciente". 0.95 deja el bloom para lo casi blanco (sol,
                                            // emisivos, destellos), que es para lo que está.
    float           bloomStrength   = 0.7f; // additive bloom intensity
    bool            motionBlur      = false;
    bool            fog             = false;  // niebla atmosférica del terreno (consola: fog 0|1)
    int             chunkMemoryMB   = 0;     // terrain chunk cache budget (MB). 0 = AUTO (25% de la RAM del sistema, acotado 512–4096). Presets pueden fijar un valor explícito.
    int             maxFps          = 60;    // frame-rate cap (0 = uncapped)
    // LOD adaptativo (calidad máxima que aguante el HW; degrada solo bajo carga). El usuario puede
    // SOBREPONERSE: adaptiveLOD=false + lodTargetPx>0 fija un detalle manual (px del split screen-space).
    bool            adaptiveLOD     = true;  // false = targetPx fijo (manual o el del preset)
    int             lodTargetPx     = 0;     // 0 = automático (preset/adaptativo); >0 = override manual del usuario (px)
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
    g.terrainQuality = TerrainQuality::Medium; // 4k (4096px); High=8k/Ultra=16k son paquetes opcionales
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
