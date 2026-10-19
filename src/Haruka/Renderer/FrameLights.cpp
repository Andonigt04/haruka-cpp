#include "Haruka/Renderer/FrameLights.h"

#include "Haruka/Core/Camera.h"
#include "Haruka/Core/Logger.h"
#include "Haruka/Core/SkyAmbient.h"   // el ambiente cielo/suelo en SH, para el planeta lejano
#include "Haruka/Core/Scene/SceneManager.h"
#include "Haruka/Tools/ObjectTypes.h"
#include "Haruka/World/Planet/PlanetarySystem.h"
#include "Haruka/World/WorldSystem.h"

#include <cmath>

namespace Haruka::Renderer {

void FrameLights::fill(PerFrameUBOData& fd, const Frame& f) {
    // La DIRECCION del Sol sale del WorldSystem (multi-estrella); la LUZ, de `SkyLight` (fisica).
    // Lazy sync in case the scene was loaded before the WorldSystem was ready.
    if (f.world && f.scene && f.world->getBodies().empty())
        f.world->syncFromScene(*f.scene);

    if (f.world && !f.world->getBodies().empty())
        fd.sunDirection  = f.world->getDominantLightDirection(glm::dvec3(f.cameraOrigin));
    else
        fd.sunDirection  = glm::vec3(-0.55f, 0.69f, -0.41f); // reasonable fallback

    // ⚠️ AQUI VIVIAN TRES NUMEROS ELEGIDOS: el color del Sol era el de su objeto de escena, el
    // ambiente un `mix(0.11, 0.28, dia)` ("suelo nocturno subido para que no fuera negro") y la Luna
    // un 0,22 por la fase, "estilizado". Ahora es la misma luz que pintan el cielo y el terreno
    // (`SkyLight`: cuerpo negro, 1/d², aire, exposicion), y estos campos solo la reparten.
    glm::vec3 sunG(1.0f), moonG(0.0f), ambUp(0.2f), ambDown(0.05f);
    if (f.sky) {
        sunG = glm::vec3(f.sky->sunGround); moonG = glm::vec3(f.sky->moonGround);
        ambUp = glm::vec3(f.sky->ambientUp); ambDown = glm::vec3(f.sky->ambientDown);
    }
    fd.sunLightColor   = sunG;
    fd.ambientStrength = 0.2126f * ambUp.x + 0.7152f * ambUp.y + 0.0722f * ambUp.z;
    {
        glm::vec3 moonDir(0.0f, 1.0f, 0.0f); float moonI = 0.0f;
        if (f.world) f.world->getMoonLight(glm::dvec3(f.cameraOrigin), moonDir, moonI);
        fd.moonDirection  = f.sky ? glm::vec3(f.sky->moonDir) : moonDir;
        fd.moonIntensity  = 1.0f;
        fd.moonLightColor = moonG;
    }

    // El planeta lejano (SimplePlanet, biome.frag): la misma luz. Su ambiente en SH: el cielo arriba y
    // el suelo abajo caben EXACTOS en los dos primeros terminos, `E(n) = (arriba+abajo)/2 +
    // (arriba-abajo)/2 · (n·up)`. biome.frag divide por π (lee irradiancia y la pasa a radiancia),
    // asi que se sube por π para que `albedo · E` salga en la misma escala que el terreno.
    if (f.planets) {
        const float pi = 3.14159265358979323846f;
        const Core::SkySH sh = Core::skyHemisphereSH(pi * ambUp, pi * ambDown);
        f.planets->setSunLight(fd.sunDirection, sunG, ambUp);
        f.planets->setSkyAmbientSH(sh.coef);
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
            if (b.type != Tools::ObjectType::STAR) continue;
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
