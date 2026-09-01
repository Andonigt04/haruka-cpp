#pragma once

#include <vector>
#include <string>

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

    /// Resolucion de la ventana, en pixeles.
    ///
    /// ⚠️ **0x0 SIGNIFICA "AUN NO RESUELTA", Y NUNCA SE ENSENA COMO OPCION.** El combo lista solo
    /// resoluciones reales del monitor; el valor por defecto es la NATIVA, no una entrada "Auto".
    /// La diferencia importa: con un "Auto" en la lista, el usuario que enchufa otro monitor no sabe
    /// a que resolucion esta jugando, y el que elige una concreta no puede volver a la nativa sin
    /// adivinar cual era. Aqui siempre hay un numero, y el primer arranque lo rellena solo.
    ///
    /// No se puede resolver al cargar los ajustes: `SDL_Init(SDL_INIT_VIDEO)` ocurre DENTRO de
    /// `Window::init`, que va despues. Lo rellena `Application` justo tras crear la ventana.
    int             resolutionW     = 0;
    int             resolutionH     = 0;
    /// ── VULKAN POR DEFECTO desde el 2026-08-31. OpenGL SIGUE SOPORTADO ──────────────────────────
    ///
    /// No es por milisegundos: medido a altura de ojo son parejos, y en algunos casos Vulkan sale un
    /// pelin peor. Es por FIDELIDAD, y la razon es concreta y esta en el log del propio motor:
    ///
    ///     [RHI] GPU preferida por ajuste: 'NVIDIA' (IGNORADA: OpenGL no permite elegir adaptador)
    ///
    /// **OpenGL no deja elegir GPU.** En un portatil hibrido eso significa que el contexto cae en la
    /// integrada salvo que el usuario sepa exportar las variables de PRIME. Y la diferencia entre
    /// tarjetas NO es cosmetica — medido con la misma sonda, el mismo shader y el mismo dato:
    ///
    ///     colocacion del vertice dibujado   AMD integrada 0,089 m   ·   NVIDIA 0,011 m    27x
    ///     bake contra la referencia CPU     AMD integrada 0,020 m   ·   NVIDIA 0,0001 m  200x
    ///
    /// Con Vulkan la seleccion automatica coge la dedicada, asi que el backend por defecto es lo que
    /// hace efectiva la GPU por defecto. Por eso NO se fija aqui una `preferredGpus` con un nombre:
    /// un nombre hardcodeado no existe en otra maquina, y la automatica ya prefiere la discreta.
    ///
    /// ⚠️ OpenGL no se retira: `HARUKA_BACKEND=opengl` o el panel de ajustes lo devuelven, y el banco
    /// sigue pasando los dos. Es cobertura de hardware viejo y un segundo par de ojos sobre el RHI —
    /// varios bugs de esta sesion salieron justo de comparar los dos backends.
    RenderBackend   renderBackend   = RenderBackend::Vulkan; // API gráfica (requiere reinicio)

    /// GPUs preferidas, por NOMBRE y en orden. Vacío = elección automática (discreta > integrada).
    ///
    /// ⚠️ Es una LISTA con una sola entrada hoy, no una cadena suelta. Un reparto multi-GPU (dibujar
    /// en una, computar en otra) necesita expresar orden, y cambiar la forma del ajuste más adelante
    /// obligaría a migrar la configuración ya guardada de los usuarios. Se guarda como `Gpu0`, `Gpu1`…
    ///
    /// ⚠️ Y por NOMBRE, no por índice: el orden en que el sistema enumera las GPUs cambia al
    /// actualizar drivers, al conectar un eGPU o al arrancar en otra máquina. Un índice guardado
    /// apuntaría mañana a otra tarjeta sin que el usuario entienda por qué; un nombre que ya no está
    /// simplemente no casa y se cae a la automática.
    ///
    /// Requiere REINICIO: el device se crea una vez en el arranque.
    std::vector<std::string> preferredGpus;
    TextureQuality  textureQuality  = TextureQuality::High;
    ShadowQuality   shadowQuality   = ShadowQuality::Medium;
    AntialiasingMode antialiasing   = AntialiasingMode::TAA;
    WaterQuality    waterQuality    = WaterQuality::Medium;
    // ⚠️ MEDIUM, NO LOW. Low = 2048, y los PNG del terreno son 4096: estaba tirando la MITAD de la
    // resolución de cada textura antes de subirla, que es la pixelación del suelo que se veía de
    // cerca. Medium = 4096 = el tamaño nativo del asset; por encima no hay nada que ganar (el
    // reescalado nunca SUBE de resolución, así que High/Ultra con estos assets dan lo mismo).
    //
    // Y cabe porque la deduplicación de capas lo hizo posible: el array pasó de 9 capas a 4 (las 9
    // contenían 4 imágenes distintas — `grass_albedo` cargada CUATRO veces). A 4096 con mipmaps son
    // ~358 MB por array, ~716 MB entre albedo y normal. Con las 9 capas de antes habrían sido 1,6 GB.
    TerrainQuality  terrainQuality  = TerrainQuality::Medium;
    FoliageQuality  foliageQuality  = FoliageQuality::Medium; // densidad/alcance de árboles/rocas
    float           fov             = 90.0f;
    float           renderScale     = 1.0f;
    bool            vsync           = false;
    bool            ssao            = true;
    bool            bloom           = true;
    /**
     * @brief TONEMAPPING. Sin él, lo que pasa de 1 se RECORTA y sale blanco puro.
     *
     * ⚠️ No es un efecto decorativo: `prop_inst.frag` y `pbr.frag` deciden dentro del shader si
     * tonemapean (`color/(color+1)` + gamma) o si hacen `clamp(color, 0, 1)`. Con esto apagado, un
     * prop bien iluminado se quema — que es exactamente el síntoma que se persiguió durante horas
     * creyendo que era un fallo del backend de Vulkan.
     *
     * Apagarlo tiene un uso legítimo: ver los valores CRUDOS para diagnosticar qué se está pasando
     * de rango. Como opción visible es honesto; como estado accidental era un bug invisible.
     */
    bool            hdr             = true;
    float           bloomThreshold  = 0.95f; // luma above which pixels bloom. OJO: el color llega ya
                                            // tonemapeado+gamma (0..1), así que 0.8 hacía brillar el 20%
                                            // MÁS CLARO de la imagen — ladrillo al sol, nubes… todo
                                            // "reluciente". 0.95 deja el bloom para lo casi blanco (sol,
                                            // emisivos, destellos), que es para lo que está.
    float           bloomStrength   = 0.7f; // additive bloom intensity
    /** @brief Iteraciones del gaussiano separable del bloom. Es el RADIO del halo.
     *
     *  Estaba hardcodeado a 5 en `renderBloom`. El bloom corre a media resolución con un kernel de
     *  5 taps: σ por pase ≈ 1,75 téxeles, y al iterar σ crece como √N. Con 5 → σ ≈ 3,9 téxeles = 7,8 px
     *  de pantalla, y el halo visible (≈3σ) llega a **~23 px de radio**. Alrededor de un sol que mide
     *  9,5 px (0,533°, el tamaño real), eso es el "foco diluido".
     *
     *  Con 3 → σ ≈ 3,0 téxeles: el halo baja a ~18 px. Bajar de 3 empieza a dejar ver la cruz del
     *  kernel de 5 taps, que es peor que un halo ancho. */
    int             bloomIterations = 3;

    /** @brief Culling de PARCHES de la malla base en GPU (compute + drawIndexedIndirect).
     *
     *  La malla base envía 393 216 parches por frame y el TCS mata casi todos: a altura de ojo el
     *  horizonte está a 4,65 km y un parche mide 39,1 km, así que se ve parte de UNO. El compute los
     *  descarta antes, con una invocación por parche en vez de cuatro.
     *
     *  ⚠️ Un fallo aquí se ve como AGUJEROS en el planeta. `HARUKA_GPU_CULL=0` lo apaga sin tocar los
     *  ajustes ni recompilar, que es la salida que uno quiere a las 3 de la mañana. */
    bool            gpuPatchCull    = true;

    /** @brief Sombras del TERRENO por ray-march contra la función de altura, en el fragment.
     *
     *  El mapa de sombras cubre ±42 m alrededor del jugador, así que no puede contener la montaña que
     *  proyecta la sombra: sin esto, una loma no proyecta nada. El fragmento marcha hacia el sol y
     *  pregunta si el terreno tapa — sin caja y sin límite de alcance.
     *
     *  ⚠️ Es coste de FRAGMENTO, y a pie el suelo es casi toda la pantalla. `HARUKA_TERRAIN_SHADOW=0`
     *  lo apaga en caliente para medir el coste real en una escena concreta. */
    bool            terrainShadows  = true;
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
