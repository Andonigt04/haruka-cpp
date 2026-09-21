#include "core/modules.h"   // HARUKA_MOD_FLUIDS
#ifdef HARUKA_MOD_FLUIDS
#include "world/water/fluid_bridge.h"

#include "core/camera.h"
#include "renderer/fluid_host.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_gpu_scope.h"
#include "tools/profiler.h"
#include "world/planet/biomes.h"
#include "world/planet/planet.h"
#include "world/planet/planetary_system.h"
#include "world/water/water_fill.h"   // WATER_FILL_DRY: el centinela del mapa de lagos
#include "world/weather_system.h"

#include <cmath>
#include <vector>

namespace Haruka::World {

void FluidBridge::run(std::unique_ptr<FluidHost>& host, const Frame& f) {
    PlanetarySystem* ps = f.planets;
    // --- HITO 2: RÍOS/LAGOS + SPLASH (FluidHost) -------------------------------------------
    // Dibuja en el pase de escena DESPUÉS del planeta y la precipitación: el color/depth de la
    // escena ya están completos, que es lo que el modo superficie del fluido necesita para sembrar
    // refracción y oclusión (blit de escena). El binding 0 se restaura al UBO per-frame del
    // engine (el planeta lo pisa con su SimplePlanetUBO; softbody/fluido esperan PerFrameData).
    // ── PUERTA RÁPIDA DEL CUERPO SECO ───────────────────────────────────────────────────────────
    //
    // ⚠️ ESTE STACK CORRÍA SIEMPRE, HUBIERA AGUA O NO. `ensurePatch` re-muestrea 9 216 celdas del
    // terreno procedural cada 160 m de marcha, `seedLakes` hace un priority-flood sobre ellas y
    // `sim.step`/`pbf.step`/`hybrid.update` van cada frame — en una luna sin una gota. Y peor que el
    // coste: `seedLakes` llena las hondonadas mire lo que mire, así que un cuerpo seco tenía LAGOS.
    //
    // `hasWater()` sale del campo horneado (`bakeWaterMap`), no de la configuración: sin téxeles bajo
    // el nivel del mar no hay semilla para el relleno y el cuerpo es seco por construcción. Andoni lo
    // pidió explícitamente: *"quiero que haya planetas sin agua... igual se gasta en que espera agua
    // cuando no tiene nunca"*.
    //
    // ⚠️ Y NO SE CONSTRUYE EL HOST SIQUIERA. Crearlo y no usarlo reservaría el solver PBF y la malla
    // del heightfield para nada; el `unique_ptr` sigue vacío hasta que se pise un mundo con agua.
    const bool bodyHasWater = ps && ps->activeTerrestrial()
                            ? ps->activeTerrestrial()->hasWater()
                            : true;   // sin planeta terrestre resuelto, no se decide aquí
    if (!host && bodyHasWater) host = std::make_unique<FluidHost>();
    if (host && bodyHasWater && ps && f.camera) {
        HARUKA_PROFILE("fluid.setup+render(rios/lagos)"); HARUKA_GPU_SCOPE("fluid.setup+render(rios/lagos)");
        glm::dvec3 pc; double pr;
            if (ps->getActivePlanet(pc, pr)) {
                host->planetCenter = pc;
                host->planetRadius = pr;
                host->hasPlanet    = true;
            }
            host->terrainHeightFn = [ps](const glm::dvec3& wp) -> double {
                // El LECHO, no el heightfield: donde hay boca de cueva el agua cae hasta el fondo
                // del foso y se ACUMULA allí (Andoni: "el agua es que se acumule en esa parte").
                const double h = ps->sampleTerrainHeight(wp);
                if (const auto* t = ps->activeTerrestrial()) {
                    glm::dvec3 pc; double pr = 0.0;
                    if (ps->getActivePlanet(pc, pr) && pr > 0.0) {
                        const glm::dvec3 rel = wp - pc;
                        if (glm::length(rel) > 1e-9) return h - (double)t->vox().floorDepthM(rel / glm::length(rel));
                    }
                }
                return h;
            };
            // El campo de lagos HORNEADO: con él, la siembra del parche deja de depender de su borde.
            host->bakedWaterFn = [ps](const glm::dvec3& wp) -> double {
                const auto* t = ps ? ps->activeTerrestrial() : nullptr;
                if (!t) return (double)Haruka::Planet::WATER_FILL_DRY;
                glm::dvec3 pc; double pr = 0.0;
                if (!ps->getActivePlanet(pc, pr) || pr <= 0.0)
                    return (double)Haruka::Planet::WATER_FILL_DRY;
                const glm::dvec3 rel = wp - pc;
                const double len = glm::length(rel);
                if (len < 1e-9) return (double)Haruka::Planet::WATER_FILL_DRY;
                return (double)t->lakeLevelAt(rel / len);
            };
            // LA RUGOSIDAD DEL LECHO, del bioma del punto (el mismo clasificador que pinta el suelo
            // y reparte los props): con ella la fricción del agua interior es del sitio — sobre roca
            // o hielo la lámina corre, en un bosque se frena — y no un número único para el parche.
            host->bedRoughnessFn = [ps](const glm::dvec3& wp) -> double {
                const auto* t = ps ? ps->activeTerrestrial() : nullptr;
                if (!t) return 0.03;
                glm::dvec3 pc; double pr = 0.0;
                if (!ps->getActivePlanet(pc, pr) || pr <= 0.0) return 0.03;
                const glm::dvec3 rel = wp - pc;
                const double len = glm::length(rel);
                if (len < 1e-9) return 0.03;
                const glm::dvec3 dir = rel / len;
                const Haruka::FieldSample f = t->fieldSampleAt(glm::vec3(dir));
                const double hM = ps->sampleTerrainHeight(wp);
                // Pendiente por diferencias sobre 8 m, en el plano tangente: basta para separar
                // "acantilado" (0,55 rad) de lo demás, que es lo único que el bioma le pide.
                const glm::dvec3 t1 = glm::normalize(std::abs(dir.y) < 0.99 ? glm::cross(dir, glm::dvec3(0, 1, 0))
                                                                             : glm::cross(dir, glm::dvec3(1, 0, 0)));
                const glm::dvec3 t2 = glm::cross(dir, t1);
                const double e = 4.0;
                const double dh1 = ps->sampleTerrainHeight(wp + t1 * e) - ps->sampleTerrainHeight(wp - t1 * e);
                const double dh2 = ps->sampleTerrainHeight(wp + t2 * e) - ps->sampleTerrainHeight(wp - t2 * e);
                const float slope = (float)std::atan(std::sqrt(dh1 * dh1 + dh2 * dh2) / (2.0 * e));
                const Haruka::Planet::Biome b = Haruka::Planet::classifyBiome(
                    Haruka::Planet::BiomeSample{f.tempC, f.humidity, (float)hM, slope});
                return (double)Haruka::Planet::manningForBiome(b);
            };
            const auto* tp = ps->activeTerrestrial();
            if (tp) {
                host->tidalSeaAlongUpFn = [ps](const glm::dvec3& dir) -> double {
                    if (!ps) return 0.0;
                    const auto* t = ps->activeTerrestrial();
                    if (!t) return 0.0;
                    const auto& bodies = t->tidalBodies();
                    if (bodies.empty()) return 0.0;
                    glm::dvec3 pc2; double R;
                    if (!ps->getActivePlanet(pc2, R)) return 0.0;
                    const double scale  = (R * R / 9.8) * 15.0;   // misma escala que planet.cpp:2686
                    const double floorD = R * 0.5;                 // mismo suelo que uTide.z
                    double h = 0.0;
                    for (const auto& b : bodies) {
                        const glm::dvec3 surf = dir * R;
                        const glm::dvec3 to   = b.posCenter - surf;
                        const double D = glm::max(glm::length(to), floorD);
                        const double L = glm::max(glm::length(b.posCenter), 1.0);
                        const double c = glm::dot(dir, b.posCenter) / L;
                        h += b.gm * (3.0 * c * c - 1.0) / (2.0 * D * D * D);
                    }
                    return h * scale;
                };
            } else {
                host->tidalSeaAlongUpFn = nullptr;
            }
            // El agua interior la DIBUJA EL MAR: el fluido publica su campo y el planeta lo consume.
            // Sin este enganche la sim sigue corriendo (cauces, charcos, lluvia) pero no se ve nada.
            host->publishInlandWater =
                [ps](const std::vector<float>& surf, int n, const glm::vec3& anchorRelEye,
                       const glm::vec3& tan, const glm::vec3& bit, const glm::vec3& up, float span) {
                    if (!ps) return;
                    if (auto* tp = ps->activeTerrestrialMut())
                        tp->setInlandWater(&surf, n, anchorRelEye, tan, bit, up, span);
                };
            // ⚠️ LA INTENSIDAD DEL CLIMA NO ES UN CAUDAL, Y ASI ESTABA ENCHUFADA. `rainPerSec` son
            // METROS DE LAMINA POR SEGUNDO (lo dice `fluid_host.h`) y aqui se le pasaba `f.rainAmount`
            // tal cual, que es un 0..1 de "cuanto llueve". O sea que una lluvia de intensidad 0,5
            // vertia **medio metro de agua por segundo** sobre las 9 216 celdas del parche.
            //
            // Medido antes de esto, con `HARUKA_RAIN=0.06` y 60 s de partida: **8 602 celdas de 9 216
            // encharcadas** y el jugador AHOGANDOSE dentro de un bloque de agua de 420 m que le
            // seguia — el "cubo de agua dinamica que aparece en terreno seco y se mueve". Con
            // `HARUKA_RAIN=1`, sumergido entero antes de los 70 s.
            //
            // La referencia fisica: una lluvia torrencial son ~100 mm/h = 2,8e-5 m/s. Aquello eran
            // 0,5 m/s, o sea **~18 000 veces** un aguacero. Aqui se usa 1 mm/s a intensidad plena
            // (3,6 m/h, todavia 36x lo torrencial) porque a escala real un charco tarda horas en
            // verse y el gate de `addRain` es de 2 cm; es un numero de JUEGO, y es el que hay que
            // tocar si se quiere mas o menos encharcamiento.
            constexpr float kRainFullIntensityMPerSec = 1.0e-3f;
            host->rainPerSec = (f.rainAmount > 0.01f)
                                   ? f.rainAmount * kRainFullIntensityMPerSec : 0.0f;
            // EL CICLO, en el parche: evapora con la temperatura y el sol (misma escala de juego
            // que la lluvia: 1e-5 m/s a 25 °C y cielo abierto = 36x lo real) y se infiltra despacio
            // en el suelo. Lo evaporado vuelve al clima como vapor en la celda del jugador.
            {
                const Haruka::WeatherSample wxl = ps ? ps->weatherAt(glm::dvec3(f.camera->position)) : Haruka::WeatherSample{};
                const float fT  = glm::clamp((wxl.tempC + 5.0f) / 35.0f, 0.0f, 1.5f);
                const float sun = 1.0f - 0.8f * glm::clamp(wxl.cloudCover, 0.0f, 1.0f);
                const float speed = (float)Haruka::WeatherSystem::cycleSpeed();
                host->evapPerSec  = 1.0e-5f * fT * sun * speed;
                host->infilPerSec = 0.5e-5f * speed;
            }
            host->update(f.dt > 0.0f ? f.dt : 0.016f,
                               glm::dvec3(f.camera->position));
            if (host->evaporatedM3 > 0.0 && ps) {
                glm::dvec3 pcw; double prw;
                if (ps->getActivePlanet(pcw, prw))
                    ps->weatherMut().addMoisture(glm::dvec3(f.camera->position) - pcw, host->evaporatedM3);
            }

            const int fw = f.renderW, fh = f.renderH;
            // Binding 0 = PerFrameData (f.perFrameUBO): las binds de UBO son por contexto GL
            // (globales al mismo device), así que un contexto local recién creado lo deja puesto
            // para los draws de softbody/fluido de abajo.
            if (RHI::Device* dev = RHI::device()) {
                RHI::Context* fluidCtx = dev->beginFrame();
                if (fluidCtx) fluidCtx->bindUniformBuffer(0, f.perFrameUBO);
            }
            host->render(glm::dvec3(f.camera->position), fw, fh, f.sceneTargetPass);
        }
}

} // namespace Haruka::World
#endif // HARUKA_MOD_FLUIDS
