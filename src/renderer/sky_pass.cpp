#include "renderer/sky_pass.h"

#include "core/camera.h"
#include "renderer/shader.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_gpu_scope.h"
#include "tools/profiler.h"
#include "world/planet/planetary_system.h"
#include "world/world_system.h"

#include <chrono>
#include <cmath>
#include <string>

namespace Haruka::Renderer {

namespace {
/// Escala de altura del aire para la perspectiva aerea (m). Ver lib/aerial.glsl.
constexpr float kAerialScaleHeightM = 4000.0f;

// UBO del cielo (binding 5) — compartido por sky.vert (la mat4) y sky.frag (el resto). Antes eran
// uniforms sueltos (glUniformMatrix4fv/3fv/1f). Truco std140: un vec3 tiene alineación 16 pero
// tamaño 12 → el float que le SIGUE cabe en el mismo slot de 16 B. Por eso los pares vec3+float.
struct SkyParams {
    glm::mat4 invViewProjRot; //   0..64
    glm::vec3 sunDir;         //  64..76
    float     sunElev;        //  76..80  (relleno del vec3 anterior)
    glm::vec3 up;             //  80..92
    float     atmo;           //  92..96
    glm::vec3 sunColor;       //  96..108
    float     time;           // 108..112  (segundos; para el MOVIMIENTO de las nubes)
    glm::vec4 weather;        // 112..128  x=humedad[0,1] · y=tempC · z=precipitación[0,1] · w=COBERTURA de nube[0,1]
    // El VIENTO del clima, en el plano tangente del observador (este/norte). Antes las nubes derivaban
    // con una constante hardcodeada (0.012, 0.006) y el follaje con su propio reloj → cada cosa soplaba
    // por su lado. Un solo vector para todos. w = 1 si lo que cae es NIEVE (nube gris ≠ nube de nieve).
    glm::vec4 wind;           // 128..144  x=este(m/s) · y=norte(m/s) · z=racha · w=esNieve
    // GEOMETRÍA DEL PLANETA para las capas de nube. Sin esto el cielo solo puede proyectar las nubes
    // sobre un plano falso (`tangente / (t + 0.22)`), que no tiene altitud: todas las capas se
    // comprimen igual hacia el horizonte y ninguna hace paralaje respecto a otra. Con radio y
    // altitud se puede intersecar el rayo con una CAPA REAL a `R + h`, que es lo que hace que una
    // nube baja pase por encima deprisa y un cirro apenas se mueva.
    glm::vec4 planet;         // 144..160  x=radio(m) · y=altitud de la cámara(m) · z=base de nube(m) · w libre
};
static_assert(sizeof(SkyParams) == 160, "SkyParams std140 size mismatch");
} // namespace

SkyPass::~SkyPass() { shutdown(); }

void SkyPass::shutdown() {
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_skyPSO)) dev->destroy(m_skyPSO);
        if (RHI::valid(m_skyUBO)) dev->destroy(m_skyUBO);
    }
    m_skyPSO = {}; m_skyUBO = {};
}

void SkyPass::draw(const Frame& f) {
    // Pase de cielo procedural (gradiente + sol + estrellas) como FONDO: triángulo
    // fullscreen SIN escribir profundidad → el terreno/objetos se pintan encima.
    if (f.world && f.camera && f.planets) {
        HARUKA_PROFILE("frame.sky+clima+sol(UBO)");
        glm::dvec3 pc; double pr;
        if (f.planets->getActivePlanet(pc, pr)) {
            // === PASE 3 MIGRADO A PSO/Context ===
            // Sin VBO: el triángulo fullscreen sale de gl_VertexID → pipeline SIN vertex layout
            // (createPipeline crea igualmente el VAO vacío que bindPipeline ata) y draw(3).
            RHI::Device* skyDev = RHI::device();
            if (!RHI::valid(m_skyPSO)) {
                const std::string vsPath = Shader::baseDir() + "shaders/sky.vert";
                const std::string fsPath = Shader::baseDir() + "shaders/sky.frag";
                RHI::PipelineDesc pd;
                pd.vertexPath   = vsPath.c_str();
                pd.fragmentPath = fsPath.c_str();
                pd.topology     = RHI::PrimitiveTopology::Triangles;  // vertexLayout vacío a propósito
                // Fondo: sin depth test → GL tampoco ESCRIBE depth (con el test off no se actualiza
                // el z-buffer), así que el terreno/objetos se pintan encima sin más.
                pd.depth.test   = false;  pd.depth.write = false;
                pd.blend.enable = false;
                m_skyPSO = skyDev->createPipeline(pd);
                m_skyUBO = skyDev->createBuffer(RHI::BufferUsage::Uniform, sizeof(SkyParams), nullptr,
                                                RHI::BufferMemory::Dynamic);
            }
            // Si el pipeline no se creó (shader roto), saltar el cielo: el clearColor deja un fondo
            // de respaldo y el resto de la escena renderiza normal.
            if (RHI::valid(m_skyPSO)) {
            const glm::dvec3 camD = glm::dvec3(f.camera->position);
            glm::dvec3 up = camD - pc; double ul = glm::length(up);
            up = (ul > 1e-9) ? up / ul : glm::dvec3(0, 1, 0);
            glm::vec3 sunDir  = f.world->getDominantLightDirection(camD);
            glm::vec3 sunCol  = f.world->getDominantLightColor(camD);
            float     sunElev = (float)glm::dot(glm::dvec3(sunDir), up);
            float     alt     = (float)(ul - pr);
            float     atmo    = 1.0f - glm::smoothstep(0.0f, (float)(pr * 0.02), alt);

            float aspectS = (float)f.renderW
                          / (float)f.renderH;
            glm::mat4 viewRot  = glm::mat4(glm::mat3(f.camera->getViewMatrix()));
            glm::mat4 invVPRot = glm::inverse(f.camera->getProjectionMatrix(aspectS) * viewRot);

            static const auto s_skyT0 = std::chrono::steady_clock::now();
            const float skyTime = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_skyT0).count();

            // ── CLIMA: se LEE del mundo, no se inventa aquí ─────────────────────────────────────
            // Antes este bloque calculaba la lluvia con `0.5+0.5·sin(t·0.05)` mientras sky.frag
            // calculaba SUS nubes con `fbm(t·0.008)`. Dos fórmulas sin relación → llovía con el
            // cielo despejado. Ahora ambos leen el MISMO `WeatherSample`: la cobertura que el shader
            // dibuja es la que produjo la lluvia, y la precipitación existe solo bajo esa nube.
            Haruka::WeatherSample wx;
            if (f.planets) wx = f.planets->weatherAt(camD);
            float precip = wx.precip;               // lluvia O nieve: las dos encapotan el cielo
            float rain   = wx.rainAmount();         // solo LLUVIA: es lo que dibuja el pase de gotas
            float cover  = wx.cloudCover;
            if (f.rainOverride >= 0.0f) {           // consola `rain` (pruebas): fuerza el temporal
                rain   = f.rainOverride;            // ...y CON él la nube que lo justifica, o volvería
                precip = f.rainOverride;            //    a verse lluvia con cielo azul (el bug de antes)
                cover  = glm::max(cover, rain);
            }
            m_wx.rainAmount = rain;                                    // gotas
            m_wx.snowAmount = (f.rainOverride >= 0.0f) ? 0.0f : wx.snowAmount();   // copos
            m_wx.windVec    = wx.wind;                                 // el ÚNICO viento del mundo

            // MOJADO DEL SUELO: se INTEGRA, no se copia de la lluvia. Mojarse cuesta ~30 s de
            // chaparrón; secarse, minutos. Esa asimetría es la que hace que el suelo siga oscuro
            // (y brillante) un rato después de escampar, en vez de apagarse con la última gota.
            {
                static auto s_wetT = std::chrono::steady_clock::now();
                const auto  now    = std::chrono::steady_clock::now();
                const float dtW    = glm::clamp(std::chrono::duration<float>(now - s_wetT).count(), 0.0f, 0.25f);
                s_wetT = now;
                m_wx.groundWetness += dtW * (rain > 0.01f ? rain / 30.0f : -1.0f / 240.0f);
                m_wx.groundWetness  = glm::clamp(m_wx.groundWetness, 0.0f, 1.0f);

                // NIEVE ACUMULADA: cuaja mientras nieva y FUNDE con la temperatura del sitio. No es
                // "hace frío ⇒ suelo blanco": si no ha nevado no hay nieve, y si sube la temperatura
                // se va aunque siga siendo invierno. Fundir es MUCHO más lento que cuajar.
                const float melt = glm::smoothstep(0.0f, 6.0f, wx.tempC) / 420.0f;
                m_wx.snowAccum += dtW * (m_wx.snowAmount > 0.01f ? m_wx.snowAmount / 45.0f : -melt);
                m_wx.snowAccum  = glm::clamp(m_wx.snowAccum, 0.0f, 1.0f);
            }

            // Viento del clima proyectado al marco del observador (este/norte) → nubes y lluvia
            // inclinan HACIA EL MISMO LADO. `wind` viene en el marco local del planeta.
            glm::vec3 eastW = glm::normalize(glm::cross(glm::vec3(0, 1, 0), glm::vec3(up)));
            if (!std::isfinite(eastW.x)) eastW = glm::vec3(1, 0, 0);
            const glm::vec3 northW = glm::cross(glm::vec3(up), eastW);
            const glm::vec2 windEN(glm::dot(wx.wind, eastW), glm::dot(wx.wind, northW));
            m_wx.windSlant = glm::clamp(windEN.x * 0.02f, -0.45f, 0.45f);   // para el pase de lluvia

            // ── EL AIRE DEL FRAME (perspectiva aérea) ─────────────────────────────────────────
            // Visibilidad L: ~60 km con aire seco, ~18 km húmedo, y la lluvia la recorta más. `día`
            // es el mismo `harukaSkyDay` del cielo (smoothstep(-0,12, 0,22) de la elevación solar),
            // para que el color del aire sea el del domo y no otro.
            {
                const float hum = glm::clamp(wx.humidity, 0.0f, 1.0f);
                float L = glm::mix(60000.0f, 18000.0f, hum);
                L *= glm::mix(1.0f, 0.35f, glm::clamp(precip, 0.0f, 1.0f));
                const float day = glm::smoothstep(-0.12f, 0.22f, sunElev);
                // z = altitud del ojo · w = escala de altura del aire (m). Con ellas la extincion
                // integra el AIRE atravesado y no los metros: desde orbita el planeta se veia al 1,5 %
                // de su color (e^{-250/60}) — "todo en blanco". Ver lib/aerial.glsl. 4 km: entre los
                // 8 km del Rayleigh y los ~1,5-2 km del aerosol de la capa limite, que es lo que la
                // visibilidad L describe.
                m_wx.aerial = glm::vec4(1.0f / L, day, std::max(alt, 0.0f), kAerialScaleHeightM);
                if (f.planets) f.planets->setAerial(m_wx.aerial);
            }

            SkyParams sp{};
            sp.invViewProjRot = invVPRot;
            sp.sunDir         = sunDir;
            sp.sunElev        = sunElev;
            sp.up             = glm::vec3(up);
            sp.atmo           = atmo;
            sp.sunColor       = sunCol;
            sp.time           = skyTime;   // para el desplazamiento de las nubes
            sp.weather        = glm::vec4(wx.humidity, wx.tempC, precip, cover);
            sp.wind           = glm::vec4(windEN.x, windEN.y, glm::length(wx.wind),
                                          wx.isSnow() ? 1.0f : 0.0f);
            // Radio del planeta y altitud del ojo sobre el nivel del mar. Se calculan en DOUBLE y
            // solo el resultado baja a float: la resta `|cam − centro| − R` es entre magnitudes de
            // ~6,37e6, y en float el resultado quedaría cuantizado a medio metro.
            {
                glm::dvec3 pcD(0.0); double prD = 0.0;
                float rad = 0.0f, alt = 0.0f;
                if (f.planets && f.planets->getActivePlanet(pcD, prD) && prD > 0.0) {
                    rad = (float)prD;
                    alt = (float)(glm::length(glm::dvec3(f.camera->position) - pcD) - prD);
                }
                // z = BASE DE LA NUBE del clima (`WeatherSample::cloudBaseM`), no una constante del
                // shader: es la misma altura que ya define el techo de la lluvia, así que la panza
                // de los cúmulos y el punto donde nacen las gotas coinciden por construcción.
                // ⚠️ `w` ERA EL CONMUTADOR DEL CÚMULO PLANO Y YA NO EXISTE TAL COSA. Decía "0 = lo
                // dibuja el pase volumétrico; > 0 = píntalo tú plano, y este valor es su techo".
                // `sky.frag` ya no pinta NINGUNA nube: las tres capas viven en `cloud_vol.frag` como
                // cáscaras marchadas. Se deja el campo a 0 y no se recicla para otra cosa — un
                // hueco de UBO reutilizado es cómo un shader acaba leyendo el dato de otro.
                //
                // CONSECUENCIA DELIBERADA: apagar el pase (ajustes o `HARUKA_CLOUD_VOL=0`) deja el
                // cielo SIN nubes. Antes el respaldo las repintaba planas y apagar no se notaba, que
                // es justo lo que impedía saber cuál de los dos caminos estabas mirando.
                sp.planet = glm::vec4(rad, alt, wx.cloudBaseM, 0.0f);
            }
            skyDev->updateBuffer(m_skyUBO, 0, sizeof(sp), &sp);

            RHI::Context* ctx = skyDev->beginFrame();
            // SIN clear: el frame ya se limpió arriba (color de cielo + depth), y este triángulo
            // fullscreen cubre la pantalla entera de todas formas.
            // f.sceneTargetPass ya resuelve _editorTarget (viewport del IDE) o _postScene; si
            // ninguno está activo queda inválido = backbuffer de pantalla. Usarlo aquí es lo
            // que hace que la escena caiga DENTRO del RenderTarget del editor.
            RHI::ClearValues sceneClear;
            sceneClear.clearColor = false; sceneClear.clearDepth = false;
            ctx->beginRenderPass(f.sceneTargetPass, sceneClear);
            if (!RHI::valid(f.sceneTargetPass))                        // el pass a pantalla NO fija viewport
                ctx->setViewport(0, 0, (int)f.width, (int)f.height);
            { HARUKA_PROFILE("sky.draw"); HARUKA_GPU_SCOPE("sky.draw");
            ctx->bindPipeline(m_skyPSO);                             // programa + depth off (no escribe z)
            ctx->bindUniformBuffer(5, m_skyUBO);
            ctx->draw(3);
            }                                            // sin vertex buffer: gl_VertexID
            ctx->endRenderPass();

            } // if (RHI::valid(m_skyPSO))
        }
    }
}

} // namespace Haruka::Renderer
