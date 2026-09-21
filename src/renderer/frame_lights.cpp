#include "renderer/frame_lights.h"

#include "core/camera.h"
#include "core/logger.h"
#include "core/scene/scene_manager.h"
#include "core/sky_ambient.h"   // ambiente integrado del MISMO cielo que se dibuja
#include "tools/object_types.h"
#include "world/planet/planetary_system.h"
#include "world/world_system.h"

#include <cmath>

namespace Haruka::Renderer {

void FrameLights::fill(PerFrameUBOData& fd, const Frame& f) {
    // Sun direction and color come from the WorldSystem (supports multiple stars).
    // Lazy sync in case the scene was loaded before the WorldSystem was ready.
    if (f.world && f.scene && f.world->getBodies().empty())
        f.world->syncFromScene(*f.scene);

    if (f.world && !f.world->getBodies().empty())
        fd.sunDirection  = f.world->getDominantLightDirection(glm::dvec3(f.cameraOrigin));
    else
        fd.sunDirection  = glm::vec3(-0.55f, 0.69f, -0.41f); // reasonable fallback
    fd.sunLightColor = (f.world && !f.world->getBodies().empty())
        ? f.world->getDominantLightColor(glm::dvec3(f.cameraOrigin))
        : glm::vec3(1.0f, 0.98f, 0.95f);
    // Ambiente por ATMÓSFERA: de día luz ambiental clara, de noche casi nada (la
    // luna aporta el relleno). Suaviza la transición con la elevación solar.
    //
    // El ambiente EN COLOR que sale del cielo integrado vive aquí, en el ámbito de la función:
    // lo calcula el bloque de abajo y lo consume el planeta un poco más allá.
    static glm::vec3 s_skyAmbient{0.15f, 0.18f, 0.24f};
    static glm::vec3 s_shCoef[9]{};   // los 9 coeficientes, para el fragment
    {
        float el  = f.world ? f.world->getSunElevation(glm::dvec3(f.cameraOrigin)) : 1.0f;
        float day = glm::smoothstep(-0.10f, 0.25f, el);
        // Suelo nocturno subido (0.04→0.11): el lado noche era casi negro → parecía "media
        // planeta sin dibujar". Ahora se ve TENUE (luz de estrellas/cielo), sigue siendo noche.
        fd.ambientStrength = glm::mix(0.11f, 0.28f, day);

        // ── EL AMBIENTE SALE DEL CIELO QUE SE DIBUJA ───────────────────────────────────────
        //
        // ⚠️ `ambientStrength` era un escalar cosido a mano, y MEDIDO estaba entre 5 y 8 veces
        // por debajo de la luz que el cielo entrega de verdad: con sol alto daba
        // (0.154, 0.182, 0.238) contra (0.714, 1.372, 2.611) del cielo. Por eso cualquier
        // superficie sin sol directo caía a 0,043 de luminancia bajo un cielo de 0,60 y se leía
        // como un agujero negro — no era un problema de sombras, era el ambiente.
        //
        // `sky_ambient.cpp` integra la MISMA paleta que dibuja `sky.frag` en armónicos
        // esféricos. Estaba escrito, documentado y sin llamar por nadie.
        //
        // ⚠️ Devuelve IRRADIANCIA (pasa de 1 en el azul), no un multiplicador de albedo: meterla
        // tal cual quemaría el planeta. La normalización lambertiana es dividir por π — así
        // `albedo · ambiente` vuelve a ser la radiancia saliente correcta.
        //
        // Se recalcula por tramos de elevación solar: la cuadratura esférica no es cara, pero
        // rehacerla cada frame no aporta nada cuando el sol se mueve en minutos.
        // ⚠️ NO va en `PerFrameUBOData`: ese bloque tiene layout std140 fijado por un
        // `static_assert` y por su gemelo en GLSL. Añadirle un campo desalinea el UBO de TODOS
        // los shaders que lo leen. El ambiente del cielo lo consume el planeta por su propia
        // vía (`setSunLight` → `SimplePlanetUBO`), así que vive fuera de ese struct.
        {
            static Haruka::SkySH s_sh;
            static float         s_shElev = -999.0f;
            if (std::abs(el - s_shElev) > 0.01f) {
                s_shElev = el;
                s_sh = Haruka::skyAmbientSH(el, 0.0f);
                for (int i = 0; i < 9; ++i) s_shCoef[i] = s_sh.coef[i];
            }
            const glm::vec3 up(0.0f, 1.0f, 0.0f);
            s_skyAmbient = Haruka::skyAmbientEval(s_sh, up, up)
                         * (1.0f / 3.14159265358979323846f);
        }
    }
    // El terreno del planeta usa la MISMA luz que el cielo: sin esto el SimplePlanet
    // iluminaba con una dirección fija y no respondía al sol que se ve en el cielo.
    if (f.planets)
        f.planets->setSunLight(fd.sunDirection, fd.sunLightColor,
                                      s_skyAmbient);
        // Y los 9 coeficientes, para que el fragment evalúe el ambiente POR NORMAL. Sin esto el
        // ambiente es una constante y todo lo que está en sombra sale plano (ver `sky_sh.glsl`).
        f.planets->setSkyAmbientSH(s_shCoef);
    // Luz de luna (2ª luz): dirección + brillo por fase (WorldSystem), color azulado.
    {
        glm::vec3 moonDir(0.0f, 1.0f, 0.0f); float moonI = 0.0f;
        if (f.world) f.world->getMoonLight(glm::dvec3(f.cameraOrigin), moonDir, moonI);
        fd.moonDirection  = moonDir;
        fd.moonIntensity  = moonI;
        fd.moonLightColor = glm::vec3(0.55f, 0.62f, 0.85f); // azul plateado
    }
}

void FrameLights::starDiagnostics(const PerFrameUBOData& fd, const Frame& f) {
    if (!f.camera) return;
    // ── DIAGNÓSTICO DE LOS CUERPOS EMISORES ─────────────────────────────────────────────────
    //
    // "¿El sol no se renderiza?" no se contesta leyendo el código: una estrella es un objeto de
    // escena como cualquier otro (esfera emisiva, `castLight` → exenta del cull por distancia) y
    // el cielo solo pinta el resplandor, no el disco. Si no se ve, la causa está en uno de tres
    // sitios y los tres son números: detrás de la cámara, fuera del frustum, o subpíxel.
    //
    // ⚠️ Recorre TODAS las estrellas, sin `break`. El motor es explícitamente multi-estrella
    // (`getDominantLightDirection` suma la contribución de cada una pesada por 1/dist²), así que
    // un diagnóstico que se pare en la primera mentiría justo en el caso que importa: un sistema
    // binario donde la que no sale es la segunda.
        if (f.world && f.diagClock - m_lastStarLog > 5.0) {
        const glm::dvec3 camD = glm::dvec3(f.camera->position);
        for (const auto& b : f.world->getBodies()) {
            if (b.type != Haruka::ObjectType::STAR) continue;
            const glm::dvec3 rel  = b.worldPos - camD;
            const double     dist = glm::length(rel);
            if (dist < 1.0) continue;
            // Semidiámetro aparente y su tamaño en PÍXELES con el FOV vertical actual.
            const double angRad = std::atan2((double)b.radius, dist);
            const double pxDiam = 2.0 * angRad / glm::radians((double)f.camera->zoom) * (double)f.heightPx;
            // ¿Dónde cae en pantalla? MISMO camino que el draw: view rotation-only + proj.
            const glm::vec4 clip = fd.projection * fd.view
                                 * glm::vec4(glm::vec3(rel), 1.0f);
            const bool      front = clip.w > 0.0f;
            const glm::vec3 ndc   = front ? glm::vec3(clip) / clip.w : glm::vec3(0.0f);
            // Elevación de ESTA estrella sobre el horizonte local, no la del conjunto.
            double elev = 0.0;
            if (f.planets) {
                glm::dvec3 pc; double pr = 0.0;
                if (f.planets->getActivePlanet(pc, pr)) {
                    const glm::dvec3 upv = camD - pc;
                    const double ul = glm::length(upv);
                    if (ul > 1e-9) elev = glm::dot(rel / dist, upv / ul);
                }
            }
            HARUKA_LOGDIAG("Star",
                "'%s' dist=%.3e m radio=%.3e m diam=%.1f px | delante=%d ndc=(%.2f,%.2f) "
                "depth=%.3e | elev=%.2f",
                b.name.c_str(), dist, (double)b.radius, pxDiam, (int)front,
                ndc.x, ndc.y, (double)ndc.z, elev);
        }
        m_lastStarLog = f.diagClock;
    }
}

} // namespace Haruka::Renderer
