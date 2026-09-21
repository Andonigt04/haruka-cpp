#include "renderer/shadow_pass.h"

#include "core/camera.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_gpu_scope.h"
#include "settings/settings_manager.h"
#include "tools/profiler.h"
#include "world/planet/planetary_system.h"
#include "world/props/prop_system.h"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace Haruka::Renderer {

RHI::TextureHandle ShadowPass::skyMaskTexture() const {
    if (!m_skyMaskOn || !m_skyMask) return {};
    RHI::Device* dev = RHI::device();
    return dev ? dev->getDepthTexture(m_skyMask->pass()) : RHI::TextureHandle{};
}

void ShadowPass::draw(RHI::Context* ctx, const Frame& f) {
    if (!ctx || !f.weather) return;
    // --- SHADOW PASS (sun): the game's casters (props) project depth into a
    // depth map; the terrain receives it. All camera-relative (camera at origin).
    m_lightSpace = glm::mat4(1.0f);
    m_shadowsOn = false;
    glm::mat4& lightSpace = m_lightSpace;
    // Read the LIVE setting (config panel + save): 0=Off,1=Low,2=Medium,3=High.
    int sq = (int)Haruka::SettingsManager::get().graphics().shadowQuality;
    if (f.camera && f.onRenderShadow && sq > 0) {
        unsigned res = (sq == 1) ? 1024u : (sq == 2) ? 2048u : 4096u; // resolution per quality
        if (!m_shadow || m_shadow->shadowWidth != res) m_shadow = std::make_unique<Shadow>(res, res);
        glm::vec3 sunDir = glm::normalize(f.sunDirection); // hacia el sol
        glm::vec3 lup = (std::abs(sunDir.y) < 0.95f) ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
        // TIGHT area around the player → more resolution per metre (sharpness).
        // ±42 m: at 4096 ≈ 2 cm/texel (was ±75 → 3.7 cm, pixels were visible).
        const float D = 90.0f, S = 42.0f;
        glm::mat4 lView = glm::lookAt(sunDir * D, glm::vec3(0.0f), lup); // luz desde el sol
        glm::mat4 lProj = glm::ortho(-S, S, -S, S, 1.0f, 2.0f * D);
        lightSpace = lProj * lView;

        RHI::ClearValues shadowClear;
        shadowClear.clearColor = false; shadowClear.clearDepth = true; shadowClear.depth = 1.0f;
        { HARUKA_PROFILE("shadow.pass"); HARUKA_GPU_SCOPE("shadow.pass");
        ctx->beginRenderPass(m_shadow->pass(), shadowClear);
        f.onRenderShadow(lightSpace, glm::vec3(f.camera->position));
        f.props->drawShadows(ctx, lightSpace, f.camera, f.planets);   // los props del motor
        m_shadowsOn = true;
        ctx->endRenderPass();
        }
    }

    // --- MÁSCARA DE EXPOSICIÓN AL CIELO (pase CENITAL) -----------------------------------
    // El mismo truco que el shadow map, pero con la "luz" en el CÉNIT: lo que aparece aquí
    // es lo que tienes ENCIMA. Es la pieza que el depth buffer no puede dar — la gota que cae
    // entre tu cara y el tejado no tiene nada DELANTE, así que el depth no la descarta; lo que
    // la descarta es saber que hay tejado ARRIBA. Una sola máscara sirve para las tres cosas:
    // nada de lluvia bajo cubierto, la silueta SECA bajo cualquier collider, y dónde cuaja la
    // nieve. Reusa el hook `onRenderShadow` del juego (props, edificios, piezas colocadas):
    // no hay que registrar casters nuevos, ya son los mismos.
    // Se hace SOLO cuando precipita: sin lluvia ni nieve no la lee nadie y sería un pase de
    // profundidad regalado cada frame.
    m_skyMaskOn = false;
    if (f.camera && f.onRenderShadow &&
        (f.weather->rainAmount > 0.01f || f.weather->snowAmount > 0.01f || f.weather->groundWetness > 0.01f)) {
        if (!m_skyMask) m_skyMask = std::make_unique<Shadow>(kSkyMaskRes, kSkyMaskRes);
        const glm::dvec3 camD2 = glm::dvec3(f.camera->position);
        glm::dvec3 upD(0, 1, 0);
        {                   glm::dvec3 pc2; double pr2;
            if (f.planets->getActivePlanet(pc2, pr2)) {
                const glm::dvec3 r = camD2 - pc2; const double rl = glm::length(r);
                if (rl > 1e-9) upD = r / rl;
            } }
        const glm::vec3 zen = glm::vec3(upD);
        glm::vec3 zup = (std::abs(zen.y) < 0.95f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        // Ortográfica MIRANDO HACIA ABAJO desde muy arriba (D), cubriendo ±S alrededor del
        // jugador. D generoso: un tejado o la copa de un árbol a 40 m tiene que entrar, o
        // "sin cubierta" saldría falso justo bajo lo que más tapa.
        const float D = 140.0f, S = (float)kSkyMaskExtentM;
        const glm::mat4 zView = glm::lookAt(zen * D, glm::vec3(0.0f), zup);
        const glm::mat4 zProj = glm::ortho(-S, S, -S, S, 1.0f, 2.0f * D);
        m_skySpace = zProj * zView;

        {
        RHI::ClearValues maskClear;
        maskClear.clearColor = false; maskClear.clearDepth = true; maskClear.depth = 1.0f;
        { HARUKA_PROFILE("skymask.pass"); HARUKA_GPU_SCOPE("skymask.pass");
        ctx->beginRenderPass(m_skyMask->pass(), maskClear);
        f.onRenderShadow(m_skySpace, glm::vec3(f.camera->position));
        m_skyMaskOn = true;
        ctx->endRenderPass();
        }
        }
    }

    // El SUELO ya sabe que está mojado (`f.weather->groundWetness` se integra arriba) pero hasta ahora
    // eso no llegaba al shader del terreno: el suelo se mojaba en la simulación y no cambiaba
    // de aspecto. Se reparte aquí, DESPUÉS del pase de máscara cenital, porque la silueta seca
    // bajo los árboles sale de esa misma textura — la que la lluvia ya usa por gota.
    if (f.planets) {
        f.planets->setGroundWet(
            f.weather->groundWetness, f.weather->snowAccum,
            (m_skyMaskOn && m_skyMask) ? RHI::device()->getDepthTexture(m_skyMask->pass())
                                       : RHI::TextureHandle{},
            m_skySpace);
    }
}

} // namespace Haruka::Renderer
