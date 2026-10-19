#include "Haruka/Renderer/SkyLight.h"

#include "Haruka/Core/Logger.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/World/Planet/Atmosphere.h"
#include "Haruka/World/Planet/SkyRadiometry.h"

#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <vector>

namespace Haruka::Renderer {

namespace P = Haruka::World::Planet;

namespace {
/// Albedo medio del suelo para la luz que devuelve hacia arriba (hierba, roca, arena: ~0,2).
constexpr double kGroundAlbedo = 0.2;

/// Irradiancia del cielo sobre una cara horizontal: `E = (1/π)·∫ R·cosθ dω` sobre el hemisferio de
/// arriba (R ya es `π·L`). Cuadratura por anillos: 8 en altura x 16 en acimut.
glm::dvec3 skyIrradianceUp(const glm::dvec3& eye, const glm::dvec3& up, double R,
                           const glm::dvec3& sunDir, const glm::dvec3& sunE,
                           const glm::dvec3& moonDir, const glm::dvec3& moonE,
                           const P::AtmoMsLut& ms) {
    const glm::dvec3 e1 = glm::normalize(glm::cross(up, std::abs(up.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
    const glm::dvec3 e2 = glm::cross(up, e1);
    constexpr int NT = 8, NP = 16;
    const double dT = 0.5 * P::kAtmoPi / NT, dP = 2.0 * P::kAtmoPi / NP;
    glm::dvec3 E(0.0);
    for (int i = 0; i < NT; ++i) {
        const double th = (i + 0.5) * dT;                 // desde el cenit
        const double w = std::cos(th) * std::sin(th) * dT * dP;
        for (int j = 0; j < NP; ++j) {
            const double ph = (j + 0.5) * dP;
            const glm::dvec3 d = up * std::cos(th) + (e1 * std::cos(ph) + e2 * std::sin(ph)) * std::sin(th);
            E += P::atmoInscatter(eye, d, -1.0, R, sunDir, sunE, moonDir, moonE, nullptr, &ms) * w;
        }
    }
    return E / P::kAtmoPi;
}
} // namespace

void SkyLight::update(const Input& in) {
    SkyLightUBO u{};
    const glm::dvec3 rel = in.eye - in.planetCenter;
    const double     dist = glm::length(rel);
    const glm::dvec3 up = (dist > 1e-6) ? rel / dist : glm::dvec3(0, 1, 0);
    const double     R = in.planetRadius;
    const double     alt = (R > 0.0) ? dist - R : 1e9;
    const glm::dvec3 eyeC = (R > 0.0) ? rel : glm::dvec3(0.0, 1e12, 0.0);   // sin planeta: lejos de todo

    // ── EL SOL: color de cuerpo negro, irradiancia por luminosidad y distancia ─────────────────────
    glm::dvec3 sunDir = up, sunE(0.0);
    double sunAng = 0.0;
    if (in.hasSun) {
        const glm::dvec3 toSun = in.sunPos - in.eye;
        const double ds = glm::length(toSun);
        if (ds > 1.0) {
            sunDir = toSun / ds;
            sunE   = P::solarColor() * P::solarIrradiance(ds);
            sunAng = std::asin(std::min(in.sunRadius / ds, 1.0));
        }
    }
    // ── LA LUNA: lo que refleja de ese mismo Sol ────────────────────────────────────────────────
    glm::dvec3 moonDir = -up, moonE(0.0), sunAtMoon(0.0);
    double moonAng = 0.0;
    if (in.hasMoon && in.moonRadius > 0.0) {
        const glm::dvec3 toMoon = in.moonPos - in.eye;
        const double dm = glm::length(toMoon);
        if (dm > in.moonRadius) {
            moonDir = toMoon / dm;
            moonAng = std::asin(std::min(in.moonRadius / dm, 1.0));
            if (in.hasSun) {
                const glm::dvec3 moonToSun = in.sunPos - in.moonPos;
                const double dms = glm::length(moonToSun);
                const double eAtMoon = P::solarIrradiance(dms);
                const double phase = std::acos(std::clamp(glm::dot(moonToSun / dms, -moonDir), -1.0, 1.0));
                moonE     = P::solarColor() * P::moonIrradiance(eAtMoon, in.moonRadius, dm, phase);
                sunAtMoon = P::solarColor() * eAtMoon;
            }
        }
    }

    // ── EL AIRE: lo que llega al ojo y el cielo que lo rodea ────────────────────────────────────
    glm::dvec3 sunGround = sunE, moonGround = moonE;
    if (R > 0.0 && std::abs(m_ms.R - R) > 1.0) {   // la tabla de dispersion multiple, una vez por planeta
        m_ms = P::atmoBuildMsLut(R);
        m_msDirty = true;
        m_cacheAlt = -1e9;                          // el cielo integrado cambia con ella
    }
    if (R > 0.0) {
        sunGround  = sunE  * P::atmoTransmittanceToSpace(eyeC, sunDir,  R);
        const glm::dvec3 moonT = P::atmoTransmittanceToSpace(eyeC, moonDir, R);
        moonGround = moonE * moonT;
        sunAtMoon *= moonT;   // la cara iluminada se VE a traves del mismo aire
        const double sunEl = glm::dot(sunDir, up), moonEl = glm::dot(moonDir, up);
        const double moonLum = P::luminance(moonE);
        if (std::abs(sunEl - m_cacheSunEl) > 0.003 || std::abs(moonEl - m_cacheMoonEl) > 0.01 ||
            std::abs(moonLum - m_cacheMoonE) > 0.05 * std::max(m_cacheMoonE, 1e-12) ||
            std::abs(alt - m_cacheAlt) > 200.0) {
            m_cacheSunEl = sunEl; m_cacheMoonEl = moonEl; m_cacheMoonE = moonLum; m_cacheAlt = alt;
            m_skyUp = skyIrradianceUp(eyeC, up, R, sunDir, sunE, moonDir, moonE, m_ms);
            // Cenit y horizonte (media en 8 acimuts a 3°): los reflejos (agua) y el ambiente de las nubes.
            m_skyZen = P::atmoInscatter(eyeC, up, -1.0, R, sunDir, sunE, moonDir, moonE, nullptr, &m_ms);
            const glm::dvec3 e1 = glm::normalize(glm::cross(up, std::abs(up.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
            const glm::dvec3 e2 = glm::cross(up, e1);
            m_skyHor = glm::dvec3(0.0);
            const double el = 3.0 * P::kAtmoPi / 180.0;
            for (int j = 0; j < 8; ++j) {
                const double ph = j * P::kAtmoPi / 4.0;
                const glm::dvec3 d = up * std::sin(el) + (e1 * std::cos(ph) + e2 * std::sin(ph)) * std::cos(el);
                m_skyHor += P::atmoInscatter(eyeC, d, -1.0, R, sunDir, sunE, moonDir, moonE, nullptr, &m_ms) / 8.0;
            }
        }
    } else {
        m_skyUp = m_skyZen = m_skyHor = glm::dvec3(0.0);
    }
    const glm::dvec3 direct = sunGround * std::max(glm::dot(sunDir, up), 0.0)
                            + moonGround * std::max(glm::dot(moonDir, up), 0.0);
    const glm::dvec3 groundUp = (R > 0.0 && alt < 20000.0) ? kGroundAlbedo * (direct + m_skyUp) : glm::dvec3(0.0);

    // ── LA EXPOSICION: cuanto abre la camara para la luz que hay ────────────────────────────────
    // ⚠️ FOTOMETRO DE LUZ INCIDENTE APUNTADO A LA FUENTE, no la luz sobre el suelo horizontal. Con el
    // Sol a 20° la cara horizontal recibe un tercio, pero lo que MIRA al Sol (nubes, laderas) la
    // recibe entera: medir en horizontal abria x2,6 y quemaba todo lo iluminado (captura 17-10).
    const double horizontalE = P::luminance(sunGround + moonGround + m_skyUp);
    const double target = (m_fixedExposure > 0.0) ? m_fixedExposure : P::exposureTarget(horizontalE);
    m_exposure = (m_fixedExposure > 0.0) ? m_fixedExposure : P::exposureAdapt(m_exposure, target, in.dtS);
    const double k = m_exposure;
    // ── EL BALANCE DE BLANCOS, el otro mando de la camara ───────────────────────────────────────
    // La luz del Sol es amarillenta de verdad en cuanto baja (a 20° ya es (0,80, 0,65, 0,45) tras el
    // aire) y una nube iluminada salia color melocoton. Una camara (y el ojo) equilibra el blanco; aqui
    // con el ajuste "luz de dia" fijo: el del Sol a 60° sobre el mar, normalizado a luminancia 1. Asi
    // el mediodia es blanco y el ocaso sigue naranja (el preset no lo corrige, como en una foto).
    glm::dvec3 wb(1.0);
    if (R > 0.0) {
        const glm::dvec3 refEye(0.0, R + 2.0, 0.0);
        const double e60 = 60.0 * P::kAtmoPi / 180.0;
        const glm::dvec3 D = P::solarColor() * P::atmoTransmittanceToSpace(refEye, glm::dvec3(std::cos(e60), std::sin(e60), 0.0), R);
        wb = glm::dvec3(P::luminance(D)) / D;
    }
    const glm::dvec3 kw = wb * k;
    {   // Diagnostico (HARUKA_DIAG): la luz medida y lo que abre la camara, cada ~5 s.
        static int s_n = 0;
        if ((s_n++ % 300) == 0)
            HARUKA_LOGDIAG("SkyLight", "Sol a %.1f° · Luna a %.1f° · luz medida %.3e (cielo %.3e) · exposicion %.1f (objetivo %.1f)",
                           glm::degrees(std::asin(std::clamp(glm::dot(sunDir, up), -1.0, 1.0))),
                           glm::degrees(std::asin(std::clamp(glm::dot(moonDir, up), -1.0, 1.0))),
                           horizontalE, P::luminance(m_skyUp), k, target);
    }

    u.sunDir      = glm::vec4(glm::vec3(sunDir), (float)sunAng);
    u.sunE        = glm::vec4(glm::vec3(sunE * kw), (float)k);
    u.moonDir     = glm::vec4(glm::vec3(moonDir), (float)moonAng);
    u.moonE       = glm::vec4(glm::vec3(moonE * kw), (float)(P::starIrradiance(0.0) * k));
    u.planet      = glm::vec4(glm::vec3(in.planetCenter - in.eye), (float)R);
    u.sunGround   = glm::vec4(glm::vec3(sunGround * kw), 0.0f);
    u.moonGround  = glm::vec4(glm::vec3(moonGround * kw), 0.0f);
    u.ambientUp   = glm::vec4(glm::vec3(m_skyUp * kw), 0.0f);
    u.ambientDown = glm::vec4(glm::vec3(groundUp * kw), 0.0f);
    u.up          = glm::vec4(glm::vec3(up), (float)alt);
    u.sunAtMoon   = glm::vec4(glm::vec3(sunAtMoon * kw), 0.0f);
    u.skyZenith   = glm::vec4(glm::vec3(m_skyZen * kw), 0.0f);
    u.skyHorizon  = glm::vec4(glm::vec3(m_skyHor * kw), 0.0f);
    m_ubo = u;
}

void SkyLight::bind(RHI::Context* ctx) {
    RHI::Device* dev = RHI::device();
    if (!dev || !ctx) return;
    if (!RHI::valid(m_buffer))
        m_buffer = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(SkyLightUBO), nullptr, RHI::BufferMemory::Dynamic);
    if (!RHI::valid(m_buffer)) return;
    dev->updateBuffer(m_buffer, 0, sizeof(SkyLightUBO), &m_ubo);
    ctx->bindUniformBuffer(kBinding, m_buffer);
    // La tabla de dispersion multiple. Se ata SIEMPRE (con ceros si aun no hay planeta): en Vulkan un
    // sampler sin atar es indefinido, y la leen el cielo, el aire de en medio y las nubes.
    if (!RHI::valid(m_msTex) || m_msDirty) {
        std::vector<glm::vec4> px((size_t)P::kAtmoMsSize * P::kAtmoMsSize, glm::vec4(0.0f));
        for (size_t i = 0; i < m_ms.data.size() && i < px.size(); ++i) px[i] = glm::vec4(m_ms.data[i], 1.0f);
        if (RHI::valid(m_msTex)) dev->destroy(m_msTex);
        RHI::TextureDesc td;
        td.width = td.height = P::kAtmoMsSize;
        td.format = RHI::Format::RGBA32F;
        td.filter = RHI::Filter::Nearest;
        td.wrap = RHI::Wrap::ClampToEdge;
        td.initialData = px.data();
        m_msTex = dev->createTexture(td);
        m_msDirty = false;
    }
    if (RHI::valid(m_msTex)) ctx->bindTexture(kMsLutBinding, m_msTex);
}

void SkyLight::shutdown() {
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_buffer)) dev->destroy(m_buffer);
        if (RHI::valid(m_msTex))  dev->destroy(m_msTex);
    }
    m_buffer = {}; m_msTex = {};
}

} // namespace Haruka::Renderer
