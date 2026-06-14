#include "world_system.h"
#include "game/planetary_system.h"
#include "core/scene/scene_manager.h"
#include "tools/object_types.h"
#include <cmath>

namespace Haruka {

// Duración de un día completo (s). Showcase: ~4 min para que se note el ciclo sin
// marear. (Futuro: exponer como ajuste.)
static constexpr double kDayLengthSeconds = 240.0;
// Periodo orbital de la Luna (s). Más lento que el día → se mueve por el cielo.
static constexpr double kMoonPeriodSeconds = 600.0;

    WorldSystem::WorldSystem() : m_worldOrigin(0.0, 0.0, 0.0) {}

    WorldSystem::~WorldSystem() = default;

    void WorldSystem::init() {
        m_planetarySystem = std::make_unique<PlanetarySystem>();
        m_planetarySystem->init();
        
        // Aquí podrías cargar tu base de datos de planetas inicial
    }

    void WorldSystem::update(double dt, const glm::dvec3& cameraPos) {
        // 1. GESTIÓN DE PRECISIÓN (Floating Origin)
        // Si la cámara se aleja más de 5000km del origen actual, reseteamos el origen.
        if (glm::length(cameraPos) > 5000.0) {
            shiftOrigin(cameraPos);
        }

        // 2. ACTUALIZAR SUBSISTEMAS
        // Pasamos la cámara y una matriz de vista-proyección (calculada con toLocal)
        m_planetarySystem->update(dt, cameraPos);

        // 3. CICLO DÍA/NOCHE: orbitar el Sol alrededor del planeta.
        updateDayNight(dt);
        // 4. LUNA: orbita alrededor del planeta (si el juego la creó).
        updateMoon(dt);
        // 5. MAREA: alineación Sol–Luna → amplitud del oleaje.
        updateTide();
        // 6. ATMÓSFERA: avanza el tiempo (deriva de dirección + ráfagas de viento).
        m_atmoTime += dt;
    }

    float WorldSystem::getSunElevation(const glm::dvec3& observer) const {
        if (!m_planetarySystem) return 1.0f;
        glm::dvec3 pc; double pr; uint32_t sd; float rl;
        if (!m_planetarySystem->getActivePlanet(pc, pr, sd, rl)) return 1.0f;
        glm::dvec3 up = observer - pc; double ul = glm::length(up);
        if (ul < 1e-9) return 1.0f;
        up /= ul;
        glm::vec3 sunDir = getDominantLightDirection(observer); // hacia el Sol
        return (float)glm::dot(glm::dvec3(sunDir), up);
    }

    glm::vec3 WorldSystem::getSkyColor(const glm::dvec3& observer) const {
        if (!m_planetarySystem) return glm::vec3(0.005f);
        glm::dvec3 pc; double pr; uint32_t sd; float rl;
        if (!m_planetarySystem->getActivePlanet(pc, pr, sd, rl)) return glm::vec3(0.005f);
        glm::dvec3 up = observer - pc; double ul = glm::length(up);
        if (ul < 1e-9) return glm::vec3(0.005f);
        up /= ul;
        float el = (float)glm::dot(glm::dvec3(getDominantLightDirection(observer)), up);

        // Cielo por elevación solar: noche → día → resplandor cálido en el horizonte.
        float day = glm::smoothstep(-0.10f, 0.25f, el);
        glm::vec3 night = glm::vec3(0.015f, 0.02f, 0.05f);
        glm::vec3 dayC  = glm::vec3(0.40f, 0.60f, 0.92f);
        glm::vec3 sky   = glm::mix(night, dayC, day);
        float sunset = std::exp(-(el * el) / 0.02f) * glm::smoothstep(-0.25f, 0.05f, el);
        sky = glm::mix(sky, glm::vec3(0.85f, 0.42f, 0.22f), sunset * 0.7f);

        // Fade a NEGRO espacial al subir (la atmósfera tiene grosor finito).
        double alt  = ul - pr;
        float  atmo = 1.0f - glm::smoothstep(0.0f, (float)(pr * 0.02), (float)alt);
        return glm::mix(glm::vec3(0.004f, 0.004f, 0.008f), sky, atmo);
    }

    glm::vec3 WorldSystem::getWind(const glm::dvec3& worldPos) const {
        if (!m_planetarySystem) return glm::vec3(0.0f);
        glm::dvec3 pc; double pr; uint32_t sd; float rl;
        if (!m_planetarySystem->getActivePlanet(pc, pr, sd, rl)) return glm::vec3(0.0f);
        glm::dvec3 up = worldPos - pc; double ul = glm::length(up);
        if (ul < 1e-9) return glm::vec3(0.0f);
        up /= ul;
        // Sin viento fuera de la atmósfera (espacio).
        float atmo = 1.0f - glm::smoothstep(0.0f, (float)(pr * 0.02), (float)(ul - pr));
        if (atmo <= 0.001f) return glm::vec3(0.0f);

        // Dirección tangente que gira lento + ráfagas (suma de senos).
        glm::dvec3 ref = (std::abs(up.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        glm::dvec3 T = glm::normalize(glm::cross(ref, up));
        glm::dvec3 B = glm::cross(up, T);
        double a = m_atmoTime * 0.03;
        glm::dvec3 dir = T * std::cos(a) + B * std::sin(a);
        float gust = 4.0f + 2.5f * (float)std::sin(m_atmoTime * 0.7)
                          + 1.5f * (float)std::sin(m_atmoTime * 0.23 + 1.0);
        return glm::vec3(dir) * (gust * atmo); // m/s
    }

    void WorldSystem::updateTide() {
        if (!m_planetarySystem) { m_tideFactor = 1.0f; return; }
        glm::dvec3 pc; double pr; uint32_t sd; float rl;
        if (!m_planetarySystem->getActivePlanet(pc, pr, sd, rl)) { m_tideFactor = 1.0f; return; }

        // Dirección al Sol (primera estrella) y a la Luna (objeto de escena).
        const CelestialBody* sun = nullptr;
        for (const auto& b : m_bodies) { if (b.type == ObjectType::STAR) { sun = &b; break; } }
        std::shared_ptr<SceneObject> moon = m_scene ? m_scene->getObject("Moon") : nullptr;
        if (!sun || !moon) { m_tideFactor = 1.0f; return; }

        glm::dvec3 sunD  = sun->worldPos - pc;
        glm::dvec3 moonD = glm::dvec3(moon->position) - pc;
        double sl = glm::length(sunD), ml = glm::length(moonD);
        if (sl < 1e-9 || ml < 1e-9) { m_tideFactor = 1.0f; return; }

        // Alineación (0..1): 1 cuando Sol y Luna están en la misma línea (viva),
        // 0 en cuadratura (muerta). |cos| porque luna nueva Y llena dan marea viva.
        double align = std::fabs(glm::dot(sunD / sl, moonD / ml));
        m_tideFactor = (float)glm::mix(0.55, 1.35, align); // amplitud del oleaje
    }

    void WorldSystem::updateMoon(double dt) {
        if (!m_scene || !m_planetarySystem) return;
        auto moon = m_scene->getObject("Moon");
        if (!moon) return;
        glm::dvec3 pc; double pr; uint32_t sd; float rl;
        if (!m_planetarySystem->getActivePlanet(pc, pr, sd, rl)) return;

        if (!m_moonInit) { m_moonOffset0 = glm::dvec3(moon->position) - pc; m_moonInit = true; }
        m_moonAngle += dt * (2.0 * M_PI / kMoonPeriodSeconds);

        // Órbita en el plano XZ (rotación en Y) manteniendo la altura del offset.
        const double c = std::cos(m_moonAngle), s = std::sin(m_moonAngle);
        const glm::dvec3 o = m_moonOffset0;
        const glm::dvec3 rot(o.x * c - o.z * s, o.y, o.x * s + o.z * c);
        moon->position = pc + rot;
    }

    void WorldSystem::updateDayNight(double dt) {
        if (!m_planetarySystem) return;
        glm::dvec3 pc; double pr; uint32_t sd; float rl;
        if (!m_planetarySystem->getActivePlanet(pc, pr, sd, rl)) return;

        // Localiza el Sol (primera estrella). Guarda su offset inicial al planeta.
        CelestialBody* sun = nullptr;
        for (auto& b : m_bodies) { if (b.type == ObjectType::STAR) { sun = &b; break; } }
        if (!sun) return;
        if (!m_celestialInit) {
            // Offset inicial desde el OBJETO DE ESCENA (absoluto, nunca desplazado por
            // el floating-origin), no desde el body (que sí se desplaza) → frame correcto.
            glm::dvec3 sunAbs = sun->worldPos;
            if (m_scene) if (auto so = m_scene->getObject(sun->name)) sunAbs = glm::dvec3(so->position);
            m_sunOffset0 = sunAbs - pc;
            m_celestialInit = true;
        }

        m_dayAngle += dt * (2.0 * M_PI / kDayLengthSeconds);

        // Rotación del offset del Sol alrededor del eje Y del planeta (sale por el
        // este, se pone por el oeste). La luz = normalize(sol - observador) sigue sola.
        const double c = std::cos(m_dayAngle), s = std::sin(m_dayAngle);
        const glm::dvec3 o = m_sunOffset0;
        const glm::dvec3 rot(o.x * c - o.z * s, o.y, o.x * s + o.z * c);
        sun->worldPos = pc + rot;

        // Mueve también el Sol VISIBLE (objeto de escena) para que cruce el cielo.
        if (m_scene) {
            if (auto so = m_scene->getObject(sun->name)) so->position = sun->worldPos;
        }
    }

    void WorldSystem::shiftOrigin(const glm::dvec3& newOrigin) {
        // Calculamos cuánto nos hemos movido
        glm::dvec3 offset = newOrigin - m_worldOrigin;
        
        // Actualizamos el origen global
        m_worldOrigin = newOrigin;

        // Movemos todos los objetos del universo para compensar
        for (auto& body : m_bodies) {
            body.worldPos -= offset;
        }
    }

    void WorldSystem::syncFromScene(const SceneManager& scene) {
        // Guardamos la escena para poder mover el Sol/Luna VISIBLES en updateDayNight.
        m_scene = const_cast<SceneManager*>(&scene);
        m_bodies.clear();
        for (const auto& objPtr : scene.getAllObjects()) {
            if (!objPtr) continue;
            ObjectType ot = stringToObjectType(objPtr->type);
            if (ot != ObjectType::STAR && !objPtr->flags.castLight) continue;

            CelestialBody body;
            body.name     = objPtr->name;
            body.type     = ObjectType::STAR;
            body.worldPos = glm::dvec3(objPtr->position);
            body.radius   = std::max({(float)objPtr->scale.x, (float)objPtr->scale.y, (float)objPtr->scale.z});
            body.mass     = 1.989e30; // default solar mass
            body.color    = glm::length(glm::vec3(objPtr->color)) > 0.001f
                            ? glm::vec3(objPtr->color)
                            : glm::vec3(1.0f, 0.95f, 0.85f);
            m_bodies.push_back(body);
        }
    }

    glm::vec3 WorldSystem::getDominantLightDirection(const glm::dvec3& observerPos) const {
        glm::dvec3 sum(0.0);
        for (const auto& body : m_bodies) {
            if (body.type != ObjectType::STAR) continue;
            glm::dvec3 delta = body.worldPos - observerPos;
            double dist2 = glm::dot(delta, delta);
            if (dist2 < 1e-12) continue;
            sum += delta / dist2; // direction weighted by 1/dist²
        }
        if (glm::length(sum) < 1e-9)
            return glm::vec3(0.0f, 1.0f, 0.0f);
        return glm::vec3(glm::normalize(sum));
    }

    bool WorldSystem::getMoonLight(const glm::dvec3& observer, glm::vec3& outDir, float& outIntensity) const {
        std::shared_ptr<SceneObject> moon = m_scene ? m_scene->getObject("Moon") : nullptr;
        if (!moon) return false;
        const CelestialBody* sun = nullptr;
        for (const auto& b : m_bodies) { if (b.type == ObjectType::STAR) { sun = &b; break; } }

        glm::dvec3 moonPos = glm::dvec3(moon->position);
        glm::dvec3 toMoon  = moonPos - observer;
        double ml = glm::length(toMoon);
        if (ml < 1e-6) return false;
        outDir = glm::vec3(toMoon / ml);            // dirección HACIA la Luna

        // Fase: fracción iluminada de la Luna vista desde el observador. Luna llena
        // (Sol detrás del observador) ≈ 1; nueva ≈ 0.
        float phase = 0.5f;
        if (sun) {
            glm::dvec3 moonToSun = glm::normalize(sun->worldPos - moonPos);
            glm::dvec3 moonToObs = glm::normalize(observer - moonPos);
            phase = (float)((glm::dot(moonToSun, moonToObs) + 1.0) * 0.5);
        }
        // Brillo base tenue (la luna ilumina ~1/400000 del sol; aquí, estilizado para
        // que la noche no sea negra pero siga siendo noche).
        outIntensity = phase * 0.22f;
        return true;
    }

    glm::vec3 WorldSystem::getDominantLightColor(const glm::dvec3& observerPos) const {
        glm::dvec3 colorSum(0.0);
        double weightSum = 0.0;
        for (const auto& body : m_bodies) {
            if (body.type != ObjectType::STAR) continue;
            glm::dvec3 delta = body.worldPos - observerPos;
            double dist2 = glm::dot(delta, delta);
            if (dist2 < 1e-12) continue;
            double w = 1.0 / dist2;
            colorSum   += glm::dvec3(body.color) * w;
            weightSum  += w;
        }
        if (weightSum < 1e-12)
            return glm::vec3(1.0f);
        return glm::vec3(colorSum / weightSum);
    }

    glm::vec3 WorldSystem::toLocal(const glm::dvec3& worldPos) const {
        // Restamos la posición de la cámara en alta precisión (double)
        // y el resultado lo bajamos a float para la GPU.
        return glm::vec3(worldPos - m_worldOrigin);
    }
}