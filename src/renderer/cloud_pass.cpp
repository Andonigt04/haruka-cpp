#include "renderer/cloud_pass.h"

#include "core/camera.h"
#include "core/logger.h"
#include "world/weather_system.h"
#include "world/world_system.h"
#include "world/planet/planetary_system.h"
#include "renderer/shader.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_gpu_scope.h"
#include "tools/profiler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace Haruka::Renderer {

namespace {

/** @brief UBO del pase VOLUMÉTRICO de nubes (binding 5). Declaración gemela de la de
 *  `cloud_vol.vert`/`cloud_vol.frag` — GLSL exige el bloque IDÉNTICO en las dos etapas.
 *
 *  Lleva DOS inversas y no una: `invViewProjRot` (solo rotación) para reconstruir la dirección del
 *  rayo, e `invViewProj` (completa, cámara-relativa) para deshacer la profundidad de la escena y
 *  saber a qué distancia hay geometría. El pase de cielo solo necesitaba la primera porque, al ser
 *  fondo, no tenía que respetar nada de lo que hubiera delante. */
struct CloudParams {
    glm::mat4 invViewProjRot; //   0..64
    glm::vec4 planetC;        //  64..80   xyz=centro del planeta RELATIVO a la cámara · w=radio(m)
    glm::vec4 slab;           //  80..96   x=base(m) · y=techo(m) · z=cobertura · w=precipitación
    glm::vec4 sun;            //  96..112  xyz=hacia el Sol · w=elevación
    glm::vec4 sunColor;       // 112..128  rgb=color del sol · a=día[0,1]
    glm::vec4 wind;           // 128..144  xy=deriva del campo · z=tiempo(s) · w=altitud del ojo(m)
    glm::vec4 misc;           // 144..160  x=atmósfera · y=pasos · z=escala · w=EXTINCIÓN por metro
    // ── EMBUDOS (tornados y trombas) ──────────────────────────────────────────────────────────
    // Se dibujan en ESTE pase porque un embudo es nube condensada: mismo color, misma luz, mismo
    // corte contra la escena. Un pase aparte habría que componerlo contra la nube madre, y el punto
    // donde el embudo entra en la base es justo donde se notaría la costura.
    glm::vec4 vortexPos[4];   // 160..224  xyz = punto de SUELO del eje, RELATIVO a la cámara · w = radio del núcleo (m)
    glm::vec4 vortexInfo[4];  // 224..288  x = techo (m) · y = viento (m/s) · z = giro · w = 1 tromba / 0 tornado
    glm::vec4 vortexN;        // 288..304  x = cuántos · y = tiempo (s)
    glm::vec4 aerial;         // 304..320  perspectiva aérea (lib/aerial.glsl): x = 1/L · y = día
    glm::vec4 blend;          // 320..336  x = peso del horneado NUEVO frente al anterior (bindings 3/4)
};
static_assert(sizeof(CloudParams) == 336, "CloudParams std140 size mismatch");
/** @brief Techo de embudos que el shader dibuja a la vez. Cuatro en pantalla ya es el fin del mundo. */
constexpr int kCloudMaxVortices = 4;

/** @brief EXTINCIÓN por metro del volumen de nube: el mando de la DENSIDAD.
 *
 *  Empezó en 0.004 y la nube se veía translúcida — se leía el cielo a través de ella, que es lo que
 *  delata que no hay cuerpo. Subir esto hace que la profundidad óptica llegue antes a saturación
 *  (Beer-Lambert), o sea que atravesar el mismo espesor tape más.
 *
 *  ⚠️ BAJÓ de 0.014 a 0.008 y NO es que la nube tape menos: cambió lo que multiplica. Antes la
 *  densidad era `max(campo − umbral, 0)`, que sobre el campo real vale 0,05-0,21 — un factor de
 *  escala accidental que había que compensar aquí. Ahora `harukaCloudStrength` normaliza a [0,1], el
 *  núcleo de una nube vale 1 y esto vuelve a ser un coeficiente por metro de verdad. Medido con la
 *  fórmula nueva: con 0.008 un cúmulo de 1060 m sale opaco (alpha 0,96 en el cénit) y los bordes
 *  siguen suaves porque la suavidad la da la rampa del umbral, no la transparencia global. */
constexpr float kCloudExtinction = 0.008f;

/** @brief Pasos del raymarch de nubes (el presupuesto de la losa convectiva; las capas altas son
 *  fijas en el shader). Es el primer número a tocar si el pase pesa. */
/// Historia: 24 → 64 (18-09: con 24 pasos UNIFORMES las nubes lejanas salían planas a 4,3 km) → 24
/// (20-09). Lo que cambió entre medias es la marcha: ahora el paso es adaptativo y el presupuesto se
/// reparte sobre la distancia que queda, así que el alcance no depende de los pasos (rasante, con 16,
/// 0 píxeles agotan el presupuesto). Medido en el banco contra 256 pasos (VK, 256²): a 25° sobre el
/// horizonte 64 = 1,2/255 de diferencia media y 24 = 2,4/255 (0,3 % de píxeles > 24/255); desde
/// 4,5 km mirando abajo 0,4 → 1,8. En el juego (volcado de GPU, misma escena) el pase baja de
/// 11-18 ms a 8-11. Y la luz del Sol NO cambia con los pasos: el banco (6-L) mide la correlación con
/// la cara al Sol y el factor de fase a 64 y a 24 y exige que coincidan. Andoni: "no quiero que el
/// sol se muestre diferente" — por eso se bajan los pasos y no las muestras hacia el Sol.
/// ⚠️ VUELTO A 64 el mismo dia: con 24 en el juego "las nubes se ven con rayas horizontales". El
/// banco no lo vio porque midio a 25° y desde arriba, no RASANTE desde el suelo, que es donde el
/// presupuesto repartido sobre decenas de km da pasos de cientos de metros y los planos de muestreo
/// (t constante) se ven como bandas. Ahora el banco mide tambien esa vista; hasta que 24 la pase
/// alli, se queda 48 (A/B 24-09: 64 marcaba 9,7 ms de GPU mirando al cielo en 1080p; 48 lo deja en
/// ~6 ms con la caja calibrada, y 32 —el piso de la muestra— en ~4,9). `HARUKA_CLOUD_STEPS` sigue
/// siendo el mando.
constexpr int kCloudStepsDefault = 48;

/// `HARUKA_CLOUD_STEPS=N` sobreescribe los pasos (8..128) sin recompilar: para el A/B en el juego
/// con la misma sonda (`HARUKA_PROF_LOG`). Nació cuando el banco (`nubes.pase_64pasos_ms_1080p`)
/// marcó 567,8 ms equivalentes contra 302,9 aprobados tras la luz física del 19-09 (marcha al Sol +
/// octavas de dispersión); `HARUKA_CLOUD_STEPS=64` es el antes.
int cloudStepsImpl() {
    static const int s_steps = [] {
        const char* e = std::getenv("HARUKA_CLOUD_STEPS");
        int n = e ? std::atoi(e) : kCloudStepsDefault;
        return std::clamp(n, 8, 128);
    }();
    return s_steps;
}

/// `HARUKA_CLOUD_VOL=0` apaga el pase VOLUMÉTRICO de cúmulos. Vive aquí y no dentro del pase porque
/// lo consultan DOS sitios que tienen que estar de acuerdo: el propio pase y el conmutador que le
/// dice a `sky.frag` si debe pintar el cúmulo plano de fondo. Si solo lo mirara el pase, apagarlo
/// dejaría el cielo SIN cúmulos —ni volumétricos ni planos— y el A/B no compararía lo que dice.
bool cloudVolumetricOffImpl() {
    static const bool s_off = [] { const char* e = std::getenv("HARUKA_CLOUD_VOL");
                                   return e && std::atoi(e) == 0; }();
    return s_off;
}
} // namespace

int  CloudPass::steps()  { return cloudStepsImpl(); }
bool CloudPass::envOff() { return cloudVolumetricOffImpl(); }

CloudPass::~CloudPass() { shutdown(); }

void CloudPass::shutdown() {
    if (m_skyBakeJob.valid()) m_skyBakeJob.wait();   // el hilo del horneado no puede sobrevivir al objeto
    RHI::Device* dev = RHI::device();
    if (dev) {
        for (RHI::TextureHandle* t : { &m_cloudCoverTex, &m_cloudHiTex, &m_cloudCoverTexPrev, &m_cloudHiTexPrev, &m_cloudCoverDummy })
            if (RHI::valid(*t)) dev->destroy(*t);
        if (RHI::valid(m_cloudRT))      dev->destroy(m_cloudRT);
        if (RHI::valid(m_cloudDepthRT)) dev->destroy(m_cloudDepthRT);
        if (RHI::valid(m_cloudUBO))     dev->destroy(m_cloudUBO);
        if (RHI::valid(m_cloudPSO))     dev->destroy(m_cloudPSO);
        if (RHI::valid(m_cloudUpPSO))   dev->destroy(m_cloudUpPSO);
    }
    m_cloudCoverTex = m_cloudHiTex = m_cloudCoverTexPrev = m_cloudHiTexPrev = m_cloudCoverDummy = {};
    m_cloudRT = m_cloudDepthRT = {}; m_cloudUBO = {}; m_cloudPSO = m_cloudUpPSO = {};
}

void CloudPass::draw(const Frame& f) {
    // A/B SIN RECOMPILAR: `HARUKA_CLOUD_VOL=0` apaga el pase volumétrico. Con él apagado, `sky.frag`
    // vuelve a pintar el cúmulo PLANO como fondo (ver su conmutador `u_planet.w`). Si el cielo se ve
    // IGUAL con y sin, lo que estás mirando no es el pase volumétrico: son el cirro (8 km) y el
    // altocúmulo (4 km), que son planos A PROPÓSITO —están tan alto que nunca se cruzan— y solo el
    // CÚMULO es volumétrico.
    // ⚠️ POR QUE NO SE DIBUJAN, EN UNA LINEA. "No se ven las nubes" tiene seis causas posibles y
    // cinco de ellas son puertas de este bloque, no del shader — que ademas se comprobo en el banco
    // que dibuja bien (`cloud_volume_draws`, incluso desde 250 km y con el planeta delante). Sin esta
    // linea, distinguirlas pedia leer el codigo; ya costo dos rondas de adivinar.
    {
        static int s_why = -2;
        int why = 0;                                   // 0 = el pase corre
        if (!m_enabled)                                    why = 1;
        else if (envOff())                              why = 2;
        else if (!f.camera || !f.planets || !f.world)    why = 3;
        else {
            glm::dvec3 c(0.0); double r = 0.0;
            if (!RHI::device())                                     why = 4;
            else if (!f.planets->getActivePlanet(c, r) || r <= 0.0) why = 5;
        }
        // ⚠️ Y EL CONMUTADOR DEL CÚMULO PLANO EN LA MISMA LÍNEA. "Siguen saliendo las nubes planas
        // antiguas" no se distingue de "son el cirro y el altocúmulo, que son planos a propósito"
        // sin saber qué vale `planet.w`: >0 significa que `sky.frag` está pintando el CÚMULO PLANO
        // porque nadie va a pintar el volumétrico. Las dos cosas se deciden aquí, así que se dicen
        // aquí — mirarlo pedía leer dos ficheros.
        const bool cumuloPlano = !(m_enabled && !envOff());
        static int s_flat = -1;
        if (why != s_why || (int)cumuloPlano != s_flat) {
            s_flat = (int)cumuloPlano;
            static const char* kPor[] = {
                "CORRE (si aun no se ven, mira la linea de cobertura)",
                "APAGADO en ajustes (m_enabled = false)",
                "APAGADO por HARUKA_CLOUD_VOL=0",
                "falta camara / sistema planetario / sistema de mundo",
                "no hay dispositivo RHI",
                "NO HAY PLANETA ACTIVO (getActivePlanet): en orbita lejana el cuerpo deja de serlo"
            };
            s_why = why;
            HARUKA_LOGI("Clouds", "pase volumetrico -> %s · cumulo PLANO de `sky.frag`: %s "
                        "(el cirro a 8 km y el altocumulo a 4 km son planos A PROPOSITO)",
                        kPor[why], cumuloPlano ? "SI (planet.w > 0)" : "no (planet.w = 0)");
        }
    }

    if (m_enabled && !envOff() && f.camera && f.planets && f.world) {
        HARUKA_PROFILE("scene.clouds.volumetric"); HARUKA_GPU_SCOPE("scene.clouds.volumetric");
        RHI::Device* dev = RHI::device();
        glm::dvec3 pcD; double prD = 0.0;
        if (dev && f.planets->getActivePlanet(pcD, prD) && prD > 0.0) {
            const int cw = f.width;
            const int ch = f.height;

            if (!RHI::valid(m_cloudPSO)) {
                const std::string vs = Shader::baseDir() + "shaders/cloud_vol.vert";
                const std::string fs = Shader::baseDir() + "shaders/cloud_vol.frag";
                RHI::PipelineDesc pd;
                pd.vertexPath   = vs.c_str();
                pd.fragmentPath = fs.c_str();
                pd.topology     = RHI::PrimitiveTopology::Triangles;
                // NO escribe profundidad: la nube es medio participativo, no una superficie. Si
                // escribiera z, lo que se dibujara después quedaría recortado por una "cáscara" que
                // no existe.
                pd.depth.test   = false;  pd.depth.write = false;
                // ⚠️ SIN MEZCLA: ahora la marcha va a un target PROPIO que se limpia a transparente,
                // no encima de la escena. Mezclar aqui compondria la nube contra el vacio del target.
                pd.blend.enable = false;
                m_cloudPSO = dev->createPipeline(pd);
                m_cloudUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(CloudParams),
                                               nullptr, RHI::BufferMemory::Dynamic);

                const std::string uvs = Shader::baseDir() + "shaders/cloud_upsample.vert";
                const std::string ufs = Shader::baseDir() + "shaders/cloud_upsample.frag";
                RHI::PipelineDesc up;
                up.vertexPath   = uvs.c_str();
                up.fragmentPath = ufs.c_str();
                up.topology     = RHI::PrimitiveTopology::Triangles;
                up.depth.test   = false;  up.depth.write = false;
                up.blend.enable = true;   // ESTE si: es el que compone la nube sobre la escena
                m_cloudUpPSO = dev->createPipeline(up);

                HARUKA_LOGI("Clouds", "pase volumetrico: %s · composicion: %s",
                            RHI::valid(m_cloudPSO)   ? "ok" : "FALLO (se sigue viendo el cielo de fondo)",
                            RHI::valid(m_cloudUpPSO) ? "ok" : "FALLO (no se vera la nube)");
            }

            // ── EL TARGET REDUCIDO ──────────────────────────────────────────────────────────────
            // El divisor es de LADO: 4 son 16 veces menos pixeles. La nube es de frecuencia baja y ya
            // va con jitter, asi que aguanta el reescalado; lo que no aguanta el motor es pagarla a
            // resolucion completa (ver la nota del miembro en application.h).
            // ⚠️ DEFAULT: 4 → 6 → 8 (22-09, A/B del FPS cerrado en el juego). Medido con
            // `HARUKA_PROF_LOG` en la 3050 Laptop: la marcha a 1/4 (480x270x64) costaba 38 ms de un
            // frame de 62 —el 60 % del frame GPU—; a 1/6, 16 ms; a 1/8 (240x135) oscila 5-6 ms y el
            // upsample + jitter la dejan igual. `HARUKA_CLOUD_RESDIV` vuelve a 6 sin recompilar.
            int resDiv = 8;
            if (const char* e = std::getenv("HARUKA_CLOUD_RESDIV")) {
                const int v = std::atoi(e);
                if (v >= 1 && v <= 8) resDiv = v;
            }
            const int rw = std::max(64, cw / resDiv), rh = std::max(64, ch / resDiv);
            if (RHI::valid(m_cloudPSO) && (!RHI::valid(m_cloudRT) ||
                                           m_cloudRTW != rw || m_cloudRTH != rh)) {
                if (RHI::valid(m_cloudRT)) dev->destroy(m_cloudRT);
                RHI::RenderTargetDesc rd;
                rd.width = rw; rd.height = rh;
                rd.colorFormats = { RHI::Format::RGBA8 };
                rd.colorFilter  = RHI::Filter::Linear;
                rd.hasDepth     = false;   // la nube no escribe z; la oclusion sale de la copia de depth
                m_cloudRT = dev->createRenderTarget(rd);
                m_cloudRTW = rw; m_cloudRTH = rh;
                HARUKA_LOGI("Clouds", "target de marcha %dx%d (1/%d de %dx%d · %.1fx menos pixeles)",
                            rw, rh, resDiv, cw, ch,
                            (double)(cw * ch) / (double)(rw * rh));
            }

            // Copia del DEPTH de la escena. ⚠️ No se puede samplear la profundidad del MISMO target
            // al que se dibuja (realimentación); el fluido ya resolvía esto igual, con `blitDepth`.
            // ⚠️ EL FORMATO DE PROFUNDIDAD LO DICTA LA FUENTE, no este bloque. Un blit de profundidad
            // exige formatos IDÉNTICOS. Con el post-proceso apagado, `f.sceneTargetPass` es inválido y la
            // escena vive en el BACKBUFFER, cuya profundidad la elige SDL (punto fijo, no D32F): pedir
            // D32F aquí daba `GL_INVALID_OPERATION: Depth formats do not match`, el blit se caía en
            // silencio y esta textura se quedaba SIN ESCRIBIR — o sea las nubes ocluyendo contra basura,
            // que es el "se ven a través del terreno" reportado.
            const RHI::Format srcDepthFmt = RHI::valid(f.sceneTargetPass) ? RHI::Format::D32F
                                                                        : dev->backbufferDepthFormat();
            if (RHI::valid(m_cloudPSO) && (!RHI::valid(m_cloudDepthRT) ||
                                           m_cloudDepthW != cw || m_cloudDepthH != ch ||
                                           m_cloudDepthFmt != srcDepthFmt)) {
                if (RHI::valid(m_cloudDepthRT)) dev->destroy(m_cloudDepthRT);
                RHI::RenderTargetDesc dd;
                dd.width = cw; dd.height = ch;
                dd.colorFormats = { RHI::Format::R32F };   // no se usa; el target necesita un color
                dd.colorFilter  = RHI::Filter::Nearest;
                dd.hasDepth     = true;
                dd.depthFormat  = srcDepthFmt;
                // ⚠️ SIN ESTO LA PROFUNDIDAD ES UN RENDERBUFFER Y NO SE PUEDE MUESTREAR:
                // `getDepthTexture` devuelve un handle INVALIDO, `bindTexture(0, ...)` no ata nada y
                // en Vulkan un descriptor sin atar es INDEFINIDO, no ceros. El shader leia basura
                // como profundidad de escena, `min(tExit, sceneT)` vaciaba el recorrido y el pase no
                // pintaba una sola nube — mientras el cielo, que no lee profundidad, se seguia
                // viendo. Solo `shadow.cpp` ponia este flag; aqui faltaba desde el principio.
                dd.depthAsTexture = true;
                m_cloudDepthRT = dev->createRenderTarget(dd);
                m_cloudDepthW = cw; m_cloudDepthH = ch; m_cloudDepthFmt = srcDepthFmt;
                // Una línea, solo al (re)crear: dice si el blit de profundidad puede ser legal. Si
                // aquí sale un formato y la escena tiene otro, las nubes ocluyen contra basura y el
                // síntoma es "se ven a través del terreno" — sin más aviso que un GL_INVALID_OPERATION
                // fácil de pasar por alto.
                HARUKA_LOGI("Clouds", "copia de profundidad %dx%d · formato %s (escena: %s)",
                            cw, ch, srcDepthFmt == RHI::Format::D32F ? "D32F" : "D24S8",
                            RHI::valid(f.sceneTargetPass) ? "target de post" : "BACKBUFFER");
                if (!RHI::valid(dev->getDepthTexture(m_cloudDepthRT))) {
                    HARUKA_LOGW("Clouds", "la copia de profundidad NO es muestreable: el pase "
                                          "ocluiria contra basura. Se desactiva.");
                    dev->destroy(m_cloudDepthRT);
                    m_cloudDepthRT = {};
                }
            }

            if (RHI::valid(m_cloudPSO) && RHI::valid(m_cloudDepthRT) &&
                RHI::valid(m_cloudRT)  && RHI::valid(m_cloudUpPSO)) {
                const glm::dvec3 camD = glm::dvec3(f.camera->position);
                const Haruka::WeatherSample wx = f.planets->weatherAt(camD);
                float coverC = wx.cloudCover;
                float precC  = wx.precip;
                if (f.rainOverride >= 0.0f) { precC = f.rainOverride; coverC = glm::max(coverC, precC); }

                // SONDA: sin esto, "las nubes no son volumétricas" no se puede separar de "no hay
                // nubes". El pase solo dibuja con cobertura > 1 %, así que un cielo con cirros y sin
                // cúmulos se ve plano y el pase ni se ejecuta. Solo al cambiar de forma apreciable.
                {
                    static float s_lastCov = -1.0f;
                    if (std::abs(coverC - s_lastCov) > 0.05f) {
                        s_lastCov = coverC;
                        HARUKA_LOGI("Clouds", "cobertura=%.2f · precip=%.2f · base=%.0f m · techo=%.0f m"
                                    " · pasos=%d -> el cumulo volumetrico %s",
                                    coverC, precC, wx.cloudBaseM, wx.cloudTopM, steps(),
                                    coverC > 0.01f ? "SE DIBUJA" : "NO se dibuja (cielo plano: cirro+altocumulo)");
                    }
                }
                // ── LA COBERTURA DEL PLANETA, HORNEADA ──────────────────────────────────────
                //
                // ⚠️ El pase recibia UN escalar (`cover` del punto bajo la camara) y lo aplicaba a
                // todo lo visible. Desde orbita eso son miles de km con el tiempo de un solo sitio.
                // Se hornea una equirect pequeña del MISMO `WeatherSystem` —el shader sigue sin
                // decidir cuanta nube hay— y el escalar pasa a ser el MAXIMO, solo para la salida
                // rapida. `cloudCoverAt` existe justo para esto: "solo la cobertura, mas barato".
                //
                // 128x64 = 8192 muestras, y NO por frame: el tiempo se mueve en minutos, asi que se
                // rehornea cada 2 s. A 60 fps son 8192 muestras cada 120 frames.
                // ⚠️ A/B SIN RECOMPILAR: `HARUKA_CLOUD_COVER=<0..1>` fuerza una cobertura UNIFORME y
                // salta el horneado del campo. Es lo unico que separa en UNA ejecucion las dos causas
                // posibles de "no hay nubes" con el pase corriendo:
                //   · con el forzado SE VEN  -> el campo (humedad o cobertura) esta mal;
                //   · con el forzado TAMPOCO -> es el shader o el compositado.
                // Sin esto hay que deducirlo, y llevo demasiadas deducciones equivocadas hoy sobre el
                // origen de un pixel.
                static const float s_forced = [] {
                    const char* e = std::getenv("HARUKA_CLOUD_COVER");
                    const float v = e ? (float)std::atof(e) : -1.0f;
                    if (v >= 0.0f)
                        HARUKA_LOGW("Clouds", "HARUKA_CLOUD_COVER=%.2f: cobertura UNIFORME forzada, "
                                    "el campo del clima se ignora (diagnostico)", v);
                    return v;
                }();

                float coverMax = coverC;
                if (s_forced >= 0.0f) {
                    coverMax = s_forced;
                    if (RHI::valid(m_cloudCoverTex))     { dev->destroy(m_cloudCoverTex);     m_cloudCoverTex = {}; }
                    if (RHI::valid(m_cloudCoverTexPrev)) { dev->destroy(m_cloudCoverTexPrev); m_cloudCoverTexPrev = {}; }
                } else {
                    // ⚠️ ESTA CADENCIA SE MEDIA EN UN RELOJ QUE EL MUNDO YA NO SIGUE, y con el reloj
                    // del cluster acelerado eso se ve. El campo se horneaba cada 2 segundos REALES;
                    // con `WORLD_TIME_SCALE=120` el mundo avanza 240 s entre horneados, o sea el 13 %
                    // de una vuelta completa de frente (`kFrontRevolutionS` = 1800 s). El cielo se
                    // queda congelado dos segundos y luego SALTA — y como un tercio de los frentes
                    // giran al reves por diseno (`sign = hashf < 0.35`), dos saltos seguidos pueden
                    // ir en sentidos opuestos. Se ve exactamente como lo que es: rapido, se para, y
                    // vuelve para atras.
                    //
                    // La cadencia se mide ahora en tiempo de MUNDO, que es lo que el campo dibuja, con
                    // un suelo en tiempo real para que acelerar el reloj no multiplique el coste: este
                    // horneado es CPU (128x64 muestras del campo de humedad) y a x120 pedirlo 120 veces
                    // mas costaria 120 veces mas. Ese suelo es el limite real de lo rapido que el cielo
                    // puede cambiar de forma continua, y decirlo aqui es mejor que fingir que no existe.
                    static double s_lastBakeWorld = -1e18;
                    static auto   s_lastBake = std::chrono::steady_clock::now() - std::chrono::hours(1);
                    const auto   now = std::chrono::steady_clock::now();
                    const double worldNow = f.planets ? f.planets->simulationTime() : 0.0;
                    const double sinceRealS  = std::chrono::duration<double>(now - s_lastBake).count();
                    const double sinceWorldS = worldNow - s_lastBakeWorld;
                    if (!RHI::valid(m_cloudCoverTex) ||
                        (sinceWorldS > 2.0 && sinceRealS > 0.10)) {
                        s_lastBake = now;
                        s_lastBakeWorld = worldNow;
                        // ⚠️ 256x128, no 128x64: un texel de 128x64 son ~310 km. Con la base y el techo
                        // por direccion (abajo) esa rejilla no distingue la tormenta del claro de al
                        // lado, que es justo lo que se quiere ver. 32768 muestras cada 2 s de mundo.
                        const int CW = 256, CH = 128;
                        std::vector<float> sky((size_t)CW * CH * 4, 0.0f);
                        // ⚠️⚠️ ESTO ERA UN `static` HORNEADO UNA SOLA VEZ Y SIN VALIDAR, Y APAGABA LAS
                        // NUBES ENTERAS. La humedad sale de `fieldSampleAt`, que necesita el planeta
                        // con su campo de clima YA horneado; el pase de nubes corre desde el primer
                        // frame, asi que si se adelanta a eso el campo sale a CERO — y al ser `static`
                        // se cachea PARA SIEMPRE. Cobertura 0 en todo el planeta, umbral 0,58, cielo
                        // vacio. Y el pase sigue corriendo (su puerta usa el maximo, que incluye la
                        // cobertura bajo la camara), asi que el log sale limpio: "SE DIBUJA", atmosfera
                        // intacta, ni una nube. Reportado como "no hay nubes".
                        //
                        // Ahora: miembro (no `static`, que ademas se lo llevaba de un mundo a otro) y
                        // **no se acepta un horneado degenerado** — si la media sale ~0 el campo no
                        // estaba listo, no se cachea, y se reintenta en la siguiente pasada.
                        std::vector<float>& humField  = m_cloudHumField;
                        std::vector<float>& tempField = m_cloudTempField;
                        std::vector<float>& waterField = m_cloudWaterField;
                        std::vector<float>& groundField = m_cloudGroundField;
                        if (humField.size() != (size_t)CW * CH) {
                            const auto tHum = std::chrono::steady_clock::now();
                            humField.assign((size_t)CW * CH, 0.5f);
                            tempField.assign((size_t)CW * CH, 15.0f);
                            waterField.assign((size_t)CW * CH, 0.0f);
                            groundField.assign((size_t)CW * CH, 0.0f);
                            const double kPiH = 3.14159265358979323846;
                            for (int y = 0; y < CH; ++y) {
                                const double lat = (0.5 - ((double)y + 0.5) / CH) * kPiH;
                                for (int x = 0; x < CW; ++x) {
                                    const double lon = (((double)x + 0.5) / CW - 0.5) * 2.0 * kPiH;
                                    const glm::dvec3 dd(std::cos(lat) * std::cos(lon), std::sin(lat),
                                                        std::cos(lat) * std::sin(lon));
                                    const Haruka::TerrainSample ts =
                                        f.planets->sampleSurface(pcD + dd * (prD + 1.0));
                                    humField[(size_t)y * CW + x]   = ts.humidity;
                                    tempField[(size_t)y * CW + x]  = ts.tempC;
                                    // ⚠️ NO `landMask`: es la puerta del continente y aqui salia 1 en todo el
                                    // planeta (MAR 0.000 en el log). El mar es la cota bajo el nivel del mar.
                                    waterField[(size_t)y * CW + x] = (ts.waterType == Haruka::WaterType::Ocean) ? 1.0f : 0.0f;
                                    groundField[(size_t)y * CW + x] = std::max(ts.elevKm, 0.0f) * 1000.0f;   // la nube nace SOBRE el suelo
                                }
                            }
                            double sh = 0.0; float hmn = 1.0f, hmx = 0.0f;
                            for (float v : humField) { sh += v; hmn = std::min(hmn, v); hmx = std::max(hmx, v); }
                            const double hmean = sh / (double)humField.size();
                            HARUKA_LOGI("Clouds", "campo de HUMEDAD+TEMP %dx%d en %.0f ms · humedad min %.3f · media "
                                        "%.3f · max %.3f%s", CW, CH,
                                        std::chrono::duration<double, std::milli>(
                                            std::chrono::steady_clock::now() - tHum).count(),
                                        hmn, hmean, hmx,
                                        (hmean < 0.02) ? "  <- DEGENERADO: el clima aun no estaba listo,"
                                                         " se reintenta" : "");
                            // ⚠️ Un campo plano a cero no se cachea: significa que el clima del planeta
                            // todavia no existia. Cachearlo dejaba el cielo vacio el resto de la partida.
                            if (hmean < 0.02) { humField.clear(); tempField.clear(); waterField.clear(); groundField.clear(); }
                            // EL CICLO DEL AGUA arranca con los mismos campos: el clima guarda su copia
                            // de temperatura y máscara de agua y desde ahí evapora, llueve y difunde.
                            else f.planets->weatherMut().setMoistureFields(tempField.data(), waterField.data(), CW, CH);
                        }
                        if (humField.size() != (size_t)CW * CH) {
                            // Sin humedad valida todavia no se hornea el cielo: mejor esperar un
                            // par de frames que publicar un campo a cero y apagar el cielo.
                            m_cloudCoverMax = std::max(m_cloudCoverMax, coverC);
                        } else {
                        // ── EL CIELO ENTERO, POR DIRECCION, EN UN HILO ──────────────────────
                        // ⚠️ Antes aqui solo se horneaba la COBERTURA, y base/techo/lluvia salian
                        // del punto bajo la camara para todo el planeta: una sola losa, y el pase
                        // rellenaba con capas fijas. Ver `WeatherSystem::bakeSky`.
                        // ⚠️ Y EN UN HILO: 134 ms medidos a 256x128, que en el hilo de render son
                        // ocho frames parados cada dos segundos de mundo. Se lanza con copias y se
                        // recoge cuando este; entre medias se ve el cielo anterior, que a 2 s de
                        // mundo de distancia es indistinguible.
                        // El ciclo del agua avanza con el reloj del CLIMA (el mismo que mueve los
                        // frentes), no con el del render.
                        {
                            const double tw = f.planets->weather().time();
                            if (m_lastWeatherT >= 0.0 && tw > m_lastWeatherT)
                                f.planets->weatherMut().stepMoisture(tw - m_lastWeatherT, m_lastSkyRGBA.size() == (size_t)CW * CH * 4 ? m_lastSkyRGBA.data() : nullptr);
                            m_lastWeatherT = tw;
                            static double s_lastLog = -1e9;
                            if (tw - s_lastLog > 120.0) {   // cada 2 min de clima: la medida del ciclo
                                s_lastLog = tw;
                                const auto& st = f.planets->weather().moistureStats();
                                HARUKA_LOGI("Clima", "ciclo del agua: vapor medio %.3f · max %.3f · evaporado acumulado %.4f m · consumido por lluvia %.3f · agua de lluvia en el parche %.0f m3",
                                            st.meanM, st.maxM, st.evapTotal, st.rainTotal, f.dynamicWaterM3);
                            }
                        }
                        if (!m_skyBakeJob.valid()) {
                            Haruka::WeatherSystem wcopy = f.planets->weather();
                            std::vector<float> tcopy = tempField, hcopy = humField, wcp = waterField, gcp = groundField;
                            // Humedad EFECTIVA: la estática del bioma + el vapor del ciclo del agua.
                            wcopy.effectiveHumidityField(humField.data(), hcopy.data(), CW, CH);
                            m_skyBakeJob = std::async(std::launch::async,
                                [wcopy, tcopy = std::move(tcopy), hcopy = std::move(hcopy), wcp = std::move(wcp), gcp = std::move(gcp), CW, CH]() {
                                    SkyBake r; r.w = CW; r.h = CH;
                                    r.rgba.assign((size_t)CW * CH * 4, 0.0f);
                                    r.hi.assign((size_t)CW * CH * 4, 0.0f);
                                    const auto t0 = std::chrono::steady_clock::now();
                                    r.coverMax = wcopy.bakeSky(tcopy.data(), hcopy.data(), CW, CH,
                                                               r.rgba.data(), &r.baseMin, &r.topMax,
                                                               wcp.data(), r.hi.data(),
                                                               gcp.size() == (size_t)CW * CH ? gcp.data() : nullptr);
                                    r.ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - t0).count();
                                    return r;
                                });
                        }
                        }   // fin del `else` de "hay humedad valida"
                    }
                    // Recoger el horneado que haya terminado (en cualquier frame, no solo en los de
                    // lanzamiento: si no, un horneado de 134 ms se recogeria 2 s tarde).
                    if (m_skyBakeJob.valid() &&
                        m_skyBakeJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                        SkyBake r = m_skyBakeJob.get();
                        const int CW = r.w, CH = r.h;
                        const std::vector<float>& sky = r.rgba;
                        m_lastSkyRGBA = r.rgba;   // lo que el ciclo del agua lee como "dónde llueve"
                        // Media por AREA (un texel polar cubre cos(lat) veces menos superficie).
                        const double kPi = 3.14159265358979323846;
                        double sum = 0.0, wsum = 0.0, sumTower = 0.0; int nStorm = 0;
                        double sumSea = 0.0, wSea = 0.0, sumLand = 0.0, wLand = 0.0, sumHi = 0.0;
                        for (int y = 0; y < CH; ++y) {
                            const double w = std::cos((0.5 - ((double)y + 0.5) / CH) * kPi);
                            for (int x = 0; x < CW; ++x) {
                                const size_t i = (size_t)y * CW + x;
                                sum += sky[i * 4] * w; wsum += w;
                                sumHi += r.hi[i * 4] * w;
                                // ⚠️ MAR Y TIERRA POR SEPARADO: "el mar totalmente cubierto" no se ve
                                // en la media global, que puede dar 0,67 con el mar a 0,95 y la tierra
                                // a 0,4. Es la cifra que hay que mirar para ese sintoma.
                                const double isSea = (m_cloudWaterField.size() == sky.size() / 4) ? m_cloudWaterField[i] : 0.0;
                                sumSea += sky[i * 4] * w * isSea;  wSea  += w * isSea;
                                sumLand += sky[i * 4] * w * (1.0 - isSea); wLand += w * (1.0 - isSea);
                                if (sky[i * 4 + 3] > 0.3f) { ++nStorm; sumTower += sky[i * 4 + 2] - sky[i * 4 + 1]; }
                            }
                        }
                        // HARUKA_DIAG: cuanto CAMBIA el horneado entre dos seguidos (cada 2 s de
                        // mundo): un salto grande aqui es un tiron del cielo, porque la textura se
                        // cambia de golpe, sin fundido.
                        {
                            static std::vector<float> s_prev; static double s_prevT = 0.0;
                            static const bool s_diag = std::getenv("HARUKA_DIAG") != nullptr;
                            if (s_diag && s_prev.size() == sky.size()) {
                                double mCov = 0, mBase = 0, mTop = 0, sCov = 0; int n = 0;
                                for (size_t i = 0; i < sky.size(); i += 4) {
                                    mCov  = std::max(mCov,  (double)std::fabs(sky[i] - s_prev[i]));
                                    mBase = std::max(mBase, (double)std::fabs(sky[i + 1] - s_prev[i + 1]));
                                    mTop  = std::max(mTop,  (double)std::fabs(sky[i + 2] - s_prev[i + 2]));
                                    sCov += std::fabs(sky[i] - s_prev[i]); ++n;
                                }
                                HARUKA_LOGI("Clouds", "horneado vs anterior (%.1f s de mundo): cobertura max |d| %.3f (media %.4f) · base max |d| %.0f m · techo max |d| %.0f m",
                                            worldNow - s_prevT, mCov, n ? sCov / n : 0.0, mBase, mTop);
                            }
                            if (s_diag) { s_prev = sky; s_prevT = worldNow; }
                        }
                        // ⚠️ EL HORNEADO ANTERIOR NO SE TIRA: SE FUNDE. Cada 2 s de mundo la textura
                        // cambiaba de golpe, y medido (HARUKA_DIAG) entre dos horneados seguidos el
                        // techo de algun texel salta hasta 1,4 km y la cobertura 0,05: la torre que
                        // tienes encima crece 1,4 km en un frame — "las nubes van a tirones". El shader
                        // lee las dos (bindings 1/2 = nuevo, 3/4 = anterior) y mezcla con `blend.x`,
                        // que va de 0 a 1 durante los 2 s hasta el siguiente horneado.
                        if (RHI::valid(m_cloudCoverTexPrev)) dev->destroy(m_cloudCoverTexPrev);
                        if (RHI::valid(m_cloudHiTexPrev))    dev->destroy(m_cloudHiTexPrev);
                        m_cloudCoverTexPrev = m_cloudCoverTex; m_cloudHiTexPrev = m_cloudHiTex;
                        m_cloudBlend.onBake(worldNow);   // (core/cloud_motion.h; medido en test_cloud_motion)
                        RHI::TextureDesc ct;
                        ct.width = CW; ct.height = CH; ct.format = RHI::Format::RGBA32F;
                        ct.filter = RHI::Filter::Linear; ct.wrap = RHI::Wrap::Repeat;
                        ct.mipmaps = false; ct.initialData = sky.data();
                        m_cloudCoverTex = dev->createTexture(ct);
                        ct.initialData = r.hi.data();
                        m_cloudHiTex = dev->createTexture(ct);
                        m_cloudCoverMax = r.coverMax;
                        m_cloudBaseMin  = r.baseMin;
                        // La marcha es UNA por la columna de aire: hasta el techo de la atmosfera util
                        // (13 km: el hielo del vapor alto en el tropico esta a 10-11 km).
                        m_cloudTopMax   = std::max(r.topMax, 13000.0f);
                        static int s_bakeLog = 0;
                        if ((s_bakeLog++ % 30) == 0)   // cada minuto de mundo, no cada horneado
                            HARUKA_LOGI("Clouds", "cielo horneado %dx%d en %.1f ms (en hilo) · cobertura media por AREA %.3f "
                                        "(MAR %.3f · TIERRA %.3f) · cirro medio %.3f · MAX %.3f "
                                        "· base min %.0f m · techo max %.0f m · %d texeles con lluvia (torre media %.0f m) "
                                        "· bajo la camara %.3f",
                                        CW, CH, r.ms, wsum > 0.0 ? sum / wsum : 0.0,
                                        wSea > 0.0 ? sumSea / wSea : 0.0, wLand > 0.0 ? sumLand / wLand : 0.0,
                                        wsum > 0.0 ? sumHi / wsum : 0.0, r.coverMax, r.baseMin, r.topMax,
                                        nStorm, nStorm ? sumTower / nStorm : 0.0, coverC);
                    }
                    {
                        if (!RHI::valid(m_cloudCoverDummy)) {
                            const float one[4] = { 1.0f, 700.0f, 1800.0f, 0.0f };
                            RHI::TextureDesc dd;
                            dd.width = 1; dd.height = 1; dd.format = RHI::Format::RGBA32F;
                            dd.filter = RHI::Filter::Nearest; dd.wrap = RHI::Wrap::ClampToEdge;
                            dd.mipmaps = false; dd.initialData = one;
                            m_cloudCoverDummy = dev->createTexture(dd);
                        }
                        // (La traza con min/media/max va DENTRO del bloque que las calcula: sacarla
                        //  fuera la dejaba fuera de ambito y ademas mentia cuando no se horneo nada.)
                    }
                    coverMax = std::max(m_cloudCoverMax, coverC);
                }

                // ⚠️ LA SALIDA RAPIDA VA POR EL MAXIMO DEL PLANETA, no por el punto de la camara.
                // Con el escalar, estando sobre un claro el pase se saltaba entero y no se dibujaba
                // la tormenta que tenias a 300 km — que desde orbita es media pantalla.
                if (coverMax > 0.01f || (f.planets && !f.planets->activeVortices().empty())) {
                    glm::dvec3 upD = camD - pcD; const double ul = glm::length(upD);
                    upD = (ul > 1e-9) ? upD / ul : glm::dvec3(0, 1, 0);
                    const float altEye = (float)(ul - prD);

                    const float aspectC = (ch > 0) ? (float)cw / (float)ch : 1.0f;
                    const glm::mat4 proj = f.camera->getProjectionMatrix(aspectC);
                    const glm::mat4 view = f.camera->getViewMatrix();

                    CloudParams cp{};
                    // ⚠️ SOLO ROTACIÓN, y es la clave del corte contra la escena. Toda la geometría
                    // se rasteriza cámara-relativa (`frameData.view = mat4(mat3(view))`), así que
                    // deshacer el depth con la vista COMPLETA situaba el punto a la distancia
                    // ABSOLUTA de la cámara al origen —millones de metros en un planeta—, el rayo no
                    // se recortaba nunca y la nube se dibujaba POR ENCIMA DEL TERRENO.
                    cp.invViewProjRot = glm::inverse(proj * glm::mat4(glm::mat3(view)));
                    // El centro del planeta RELATIVO a la cámara: en doble y solo el resultado a
                    // float. La resta directa de magnitudes de ~6,37e6 en float se cuantiza a medio
                    // metro, que sobre la base de la nube se ve como que la capa "respira".
                    cp.planetC = glm::vec4(glm::vec3(pcD - camD), (float)prD);
                    // x/y = la BANDA GLOBAL que hay que marchar (base mínima y techo máximo del
                    // planeta): dentro, cada punto lee SU base y SU techo de la textura. Con el
                    // forzado uniforme (`HARUKA_CLOUD_COVER`) no hay textura y valen los locales.
                    cp.slab    = (s_forced >= 0.0f)
                               ? glm::vec4(wx.cloudBaseM, wx.cloudTopM, coverMax, precC)
                               : glm::vec4(m_cloudBaseMin, m_cloudTopMax, coverMax, precC);

                    const glm::vec3 sunDir = f.world->getDominantLightDirection(camD);
                    const glm::vec3 sunCol = f.world->getDominantLightColor(camD);
                    const float sunElev = (float)glm::dot(glm::dvec3(sunDir), upD);
                    cp.sun      = glm::vec4(sunDir, sunElev);
                    cp.sunColor = glm::vec4(sunCol, glm::smoothstep(-0.12f, 0.18f, sunElev));

                    static const auto s_cloudT0 = std::chrono::steady_clock::now();
                    const float ct = std::chrono::duration<float>(
                        std::chrono::steady_clock::now() - s_cloudT0).count();
                    glm::vec3 eastC = glm::normalize(glm::cross(glm::vec3(0, 1, 0), glm::vec3(upD)));
                    if (!std::isfinite(eastC.x)) eastC = glm::vec3(1, 0, 0);
                    const glm::vec3 northC = glm::cross(glm::vec3(upD), eastC);
                    // ⚠️ LA DERIVA IBA CON EL VIENTO DE SUPERFICIE Y CON EL RELOJ REAL. Dos fallos:
                    // (1) a 1-2 km el viento es 2-3 veces el de los 10 m del suelo, y con los 3,5 m/s
                    // del spawn un cumulo de 1,4 km tardaba 7 minutos en recorrer su ancho —"las nubes
                    // no se mueven"—; (2) el reloj real no es el del mundo: con el tiempo acelerado los
                    // frentes corrian y la forma fina se quedaba atras. Va en tiempo de MUNDO y en
                    // unidades del campo (metros x escala), calculado en doble para no perder el
                    // desplazamiento en la suma con un `ct` grande.
                    const double simT = f.planets ? f.planets->simulationTime() : (double)ct;
                    // ⚠️ ERA `viento(ahora) x simT`, Y ESO DA TIRONES. El viento que devuelve el clima
                    // cambia cada frame (rachas, y con la camara, porque se lee en `camD`); multiplicado
                    // por un simT de horas, una variacion de 0,1 m/s movia el campo cientos de km de
                    // golpe: "las nubes van a tirones" (Andoni). La deriva es la INTEGRAL del viento,
                    // asi que se acumula en doble frame a frame con el dt del mundo; el viento de hoy
                    // solo decide cuanto avanza HOY. El primer frame arranca en viento x simT para que
                    // el cielo no empiece en el origen del campo en cada partida.
                    {
                        const glm::dvec3 wcl = glm::dvec3(f.windVec) * (double)Haruka::WeatherSystem::kCloudLevelWindMul
                                             * (double)Haruka::WeatherSystem::kFieldScale;
                        const glm::dvec2 wEN(glm::dot(wcl, glm::dvec3(eastC)), glm::dot(wcl, glm::dvec3(northC)));
                        const glm::dvec2 dAdv = m_cloudDrift.advance(wEN, simT);   // core/cloud_motion.h
                        // HARUKA_DIAG: cuanto habria SALTADO el campo con la formula vieja (viento x simT)
                        // frente a lo que avanza integrando. En unidades del campo (1 = ~1,4 km).
                        static const bool s_diag = std::getenv("HARUKA_DIAG") != nullptr;
                        if (s_diag) {
                            static glm::dvec2 s_oldPrev(0.0); static double s_maxOld = 0.0, s_maxNew = 0.0; static int s_n = 0;
                            const glm::dvec2 oldNow = wEN * simT;
                            if (s_n > 0) { s_maxOld = std::max(s_maxOld, glm::length(oldNow - s_oldPrev)); s_maxNew = std::max(s_maxNew, glm::length(dAdv)); }
                            s_oldPrev = oldNow;
                            if (++s_n % 300 == 0)
                                HARUKA_LOGI("Clouds", "deriva: salto maximo por frame en 300 frames — formula vieja (viento x simT) %.4f · integrada %.4f unidades del campo (viento %.2f m/s)",
                                            s_maxOld, s_maxNew, (double)glm::length(f.windVec)), s_maxOld = s_maxNew = 0.0;
                        }
                    }
                    cp.wind = glm::vec4((float)m_cloudDrift.offset.x, (float)m_cloudDrift.offset.y, ct, altEye);

                    // ⚠️⚠️ ESTO APAGABA LAS NUBES DESDE ORBITA, Y SU PREMISA ERA FALSA POR LOS DOS
                    // LADOS. Era `1 - smoothstep(0, radio*0.02, altitud)`: en la Tierra eso vale CERO
                    // a partir de **127 km**, y el shader hace `alpha * u_misc.x`, o sea que a 249 km
                    // (donde Andoni lo reporto) las nubes se multiplicaban por cero. El comentario
                    // decia *"en orbita no hay nube que atravesar (y el fondo ya se encarga)"*:
                    //   · desde fuera no se ATRAVIESAN, pero se VEN — la capa de nubes sobre el
                    //     planeta es lo mas reconocible de un mundo visto desde el espacio;
                    //   · y el fondo NO se encarga: `sky.frag` con `u_atmo -> 0` pinta ESPACIO, no
                    //     nubes, asi que no habia nadie dibujandolas.
                    //
                    // La geometria del pase ya soportaba mirar desde ARRIBA de la capa —`cloud_vol.frag`
                    // tiene el caso `if (hitBase && b0 > 0.0)` que invierte el orden de entrada— y el
                    // corte contra la profundidad de la escena recorta la cara lejana de la cascara.
                    // O sea que lo unico que faltaba era dejar de multiplicar por cero.
                    //
                    // El campo se conserva (esta en el UBO y el shader lo lee) por si alguna vez hace
                    // falta atenuar por otra razon; hoy no hay ninguna.
                    const float atmoC = 1.0f;
                    (void)altEye;
                    // Pasos: pocos. Esto corre a pantalla completa y lo que hace falta es que el
                    // ESPESOR exista, no que la integral sea exacta.
                    // z = ESCALA del campo horizontal, en 1/metros; su inversa es el ANCHO del
                    // cúmulo. Sale de `WeatherSystem` y no de un literal aquí porque forma pareja
                    // con el GROSOR que calcula `sampleAt`: los dos juntos deciden si la nube se lee
                    // como cuerpo o como lámina, y el test `cloud_shape` fija su relación.
                    cp.misc = glm::vec4(atmoC, (float)steps(),
                                        Haruka::WeatherSystem::kFieldScale, kCloudExtinction);

                    // ── LOS EMBUDOS QUE HAY CERCA ───────────────────────────────────────────
                    // Los más cercanos primero: si hay más de `kCloudMaxVortices` (no debería
                    // pasar nunca; ver la frecuencia medida en `weather_severe`), se quedan fuera
                    // los lejanos, que son los que menos píxeles ocupan.
                    int nv = 0;
                    if (f.planets) {
                        auto vs = f.planets->activeVortices();   // copia: se ordena
                        std::sort(vs.begin(), vs.end(),
                            [&](const Haruka::WeatherSystem::Vortex& a,
                                const Haruka::WeatherSystem::Vortex& b) {
                                return glm::dot(a.dir, upD) > glm::dot(b.dir, upD);
                            });
                        for (const auto& v : vs) {
                            if (nv >= kCloudMaxVortices) break;
                            // Punto de SUELO del eje: el terreno bajo el vórtice, no el nivel del
                            // mar. Un tornado sobre una meseta de 800 m que naciera a cota 0
                            // sería una columna enterrada.
                            const glm::dvec3 gW = pcD + v.dir * prD;
                            const double hM = f.planets->sampleTerrainHeight(gW);
                            const glm::dvec3 groundW = pcD + v.dir * (prD + std::max(hM, 0.0));
                            cp.vortexPos[nv]  = glm::vec4(glm::vec3(groundW - camD), (float)v.coreRadiusM);
                            cp.vortexInfo[nv] = glm::vec4(v.topM, v.windMS, v.spin,
                                                          v.overWater ? 1.0f : 0.0f);
                            ++nv;
                        }
                    }
                    // z = ángulo de un píxel del target REDUCIDO (el que marcha): el LOD del shader
                    // se calibra con lo que de verdad resuelve, no con una constante.
                    const float fovYc = glm::radians(f.camera ? f.camera->zoom : 60.0f);
                    // w = máscara de capas (diagnóstico): HARUKA_CLOUD_MODES=1 cúmulo · 2 nivel medio · 4 cirro.
                    static const float s_modes = [] {
                        const char* e = std::getenv("HARUKA_CLOUD_MODES");
                        return e ? (float)std::atoi(e) : 7.0f;
                    }();
                    cp.vortexN = glm::vec4((float)nv, ct, fovYc / (float)std::max(m_cloudRTH, 1), s_modes);
                    cp.aerial  = f.aerial;
                    {   // fundido del horneado (core/cloud_motion.h): 0 al llegar el nuevo, 1 antes del siguiente
                        const double wnow = f.planets ? f.planets->simulationTime() : 0.0;
                        const double f = RHI::valid(m_cloudCoverTexPrev) ? m_cloudBlend.factor(wnow) : 1.0;
                        cp.blend = glm::vec4((float)f, 0.0f, 0.0f, 0.0f);
                    }

                    dev->updateBuffer(m_cloudUBO, 0, sizeof(cp), &cp);

                    RHI::Context* cctx = dev->beginFrame();
                    if (cctx) {
                        cctx->blitDepth(f.sceneTargetPass, m_cloudDepthRT, cw, ch);

                        // ── LA MARCHA, EN EL TARGET REDUCIDO ────────────────────────────────────
                        // Aqui SI se limpia (a transparente): el target es solo de la nube, y si se
                        // conservara lo del frame anterior la nube se acumularia sobre si misma.
                        RHI::ClearValues cl;
                        cl.clearColor = true;  cl.clearDepth = false;
                        cl.color[0] = cl.color[1] = cl.color[2] = cl.color[3] = 0.0f;
                        cctx->beginRenderPass(m_cloudRT, cl);
                        cctx->setViewport(0, 0, m_cloudRTW, m_cloudRTH);
                        cctx->bindPipeline(m_cloudPSO);
                        cctx->bindUniformBuffer(5, m_cloudUBO);
                        cctx->bindTexture(0, dev->getDepthTexture(m_cloudDepthRT));
                        // ⚠️ INCONDICIONAL: en Vulkan un sampler sin atar es INDEFINIDO, no ceros.
                        // El shader distingue "sin campo" por `textureSize <= 1` y cae al escalar.
                        cctx->bindTexture(1, RHI::valid(m_cloudCoverTex) ? m_cloudCoverTex
                                                                         : m_cloudCoverDummy);
                        cctx->bindTexture(2, RHI::valid(m_cloudHiTex) ? m_cloudHiTex
                                                                      : m_cloudCoverDummy);
                        cctx->bindTexture(3, RHI::valid(m_cloudCoverTexPrev) ? m_cloudCoverTexPrev
                                          : (RHI::valid(m_cloudCoverTex) ? m_cloudCoverTex : m_cloudCoverDummy));
                        cctx->bindTexture(4, RHI::valid(m_cloudHiTexPrev) ? m_cloudHiTexPrev
                                          : (RHI::valid(m_cloudHiTex) ? m_cloudHiTex : m_cloudCoverDummy));
                        cctx->draw(3);
                        cctx->endRenderPass();

                        // ── Y LA COMPOSICION SOBRE LA ESCENA, A RESOLUCION COMPLETA ─────────────
                        RHI::ClearValues keep;
                        keep.clearColor = false; keep.clearDepth = false;
                        cctx->beginRenderPass(f.sceneTargetPass, keep);
                        // ⚠️ INCONDICIONAL, y no solo cuando no hay target de post: el pase anterior
                        // dejo el viewport en el tamaño REDUCIDO y en OpenGL `beginRenderPass` no lo
                        // restablece (Vulkan si). Sin esta linea la nube se componia solo en la
                        // esquina de 1/4 de lado — medido en el banco: 5,8 % del cuadro en GL contra
                        // 96,4 % en Vulkan, con el mismo shader y los mismos datos.
                        cctx->setViewport(0, 0, cw, ch);
                        cctx->bindPipeline(m_cloudUpPSO);
                        cctx->bindTexture(0, dev->getColorTexture(m_cloudRT, 0));
                        // ⚠️ Y LA PROFUNDIDAD DE LA ESCENA, la misma copia que leyo el pase: con ella
                        // la composicion descarta las muestras de nube que se marcharon contra OTRA
                        // escena (el otro lado de la silueta del terreno). Sin esto el recorte sale en
                        // escalera de bloques de 4 px — ver la nota larga de `cloud_upsample.frag`.
                        cctx->bindTexture(1, dev->getDepthTexture(m_cloudDepthRT));
                        cctx->draw(3);
                        cctx->endRenderPass();
                    }
                }
            }
        }
    }

}

} // namespace Haruka::Renderer
