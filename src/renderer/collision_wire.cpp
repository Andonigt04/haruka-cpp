#include "renderer/collision_wire.h"

#include "core/camera.h"
#include "core/logger.h"
#include "world/terrain/terrain_lod.h"   // TERRAIN_COLLIDE_UNIFORM_M (radio del alambre)
#include "world/planet/planetary_system.h"
#include "physics/physics_engine.h"
#include "renderer/shader.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_device.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>

namespace Haruka::Renderer {

CollisionWire::~CollisionWire() { shutdown(); }

void CollisionWire::shutdown() {
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_linePSO)) dev->destroy(m_linePSO);
        if (RHI::valid(m_lineVB))  dev->destroy(m_lineVB);
        if (RHI::valid(m_lineUBO)) dev->destroy(m_lineUBO);
    }
    m_linePSO = {}; m_lineVB = {}; m_lineUBO = {};
    m_lineVerts = 0; m_lineRev = m_propVer = ~0ull;
}

void CollisionWire::setEnabled(bool on, Physics::PhysicsEngine* physics) {
    m_on = on;
#ifdef HARUKA_MOD_PHYSICS
    if (physics) physics->setCollisionMeshDebug(on);
#endif
    m_lineRev = ~0ull;   // fuerza re-subir los buffers al encender
}

void CollisionWire::draw(RHI::Context* ctx, const glm::mat4& viewProjRotOnly, const Frame& f) {
#ifdef HARUKA_MOD_PHYSICS
    // Se puede encender sin tocar el juego: `HARUKA_COLLISION_WIRE=1`. Es una herramienta de
    // diagnóstico y el juego no tiene por qué exponerla en su UI para poder usarla.
    {
        static int s_env = -1;
        if (s_env < 0) {
            const char* e = std::getenv("HARUKA_COLLISION_WIRE");
            s_env = (e && e[0] == '1') ? 1 : 0;
            if (s_env == 1) {
                HARUKA_LOGI("CollisionWire", "activado por HARUKA_COLLISION_WIRE=1");
                setEnabled(true, f.physics);
            }
        }
    }
    if (!m_on || !ctx || !f.physics || !f.camera) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;

    static std::vector<glm::dvec3> s_verts;
    static std::vector<uint32_t>   s_tris;
    glm::dvec3 center(0.0);
    uint64_t rev = 0;
    if (!f.physics->getCollisionMeshDebug(s_verts, s_tris, center, rev)) return;

    // ── LA MALLA QUE DE VERDAD ESTA EN JOLT, CONTRA LA SUPERFICIE QUE DICE LA FUNCION ────────────
    //
    // ⚠️ ES LO UNICO QUE NUNCA SE HABIA COMPROBADO, y por eso merece estar aqui. Todas las sondas del
    // motor comparan FUNCIONES (`sampleHeight` con dos cortes de octava) o DATOS (el heightmap del
    // nodo). Ninguna mira si los vertices que Jolt colisiona de verdad estan donde la funcion dice.
    // Un desplazamiento ahi seria invisible para todas ellas — y explicaria una diferencia
    // render/colision que ninguna medida encuentra. Se reporta una vez por revision de la malla, que
    // es cuando puede cambiar.
    if (f.planets) {
        static uint64_t s_lastRev = ~0ull;
        if (rev != s_lastRev) {
            s_lastRev = rev;
            glm::dvec3 pc; double pr = 0.0;
            if (f.planets->getActivePlanet(pc, pr) && !s_verts.empty()) {
                double worst = 0.0, sum = 0.0; size_t n = 0;
                double worstN = 0.0, sumN = 0.0; size_t nN = 0;   // solo el entorno CERCANO
                // ⚠️ SIN MUESTREAR EN EL ENTORNO INMEDIATO. Un paso de 1 de cada N reparte 500
                // puntos por 256 m: demasiado disperso para ver un problema LOCAL, que es justo
                // donde se reporta ("bajo mis pies"). Cerca se miran TODOS los vertices.
                const glm::dvec3 eye = glm::dvec3(f.camera->position);
                double worstF = 0.0, sumF = 0.0; size_t nF = 0;   // < 10 m del jugador
                for (size_t vi = 0; vi < s_verts.size(); ++vi) {
                    if (glm::length(s_verts[vi] - eye) < 10.0) {
                        const glm::dvec3 wf = s_verts[vi];
                        const double rf = glm::length(wf - pc);
                        if (rf > 1.0) {
                            const double df = (rf - pr) - f.planets->sampleTerrainHeight(wf);
                            worstF = std::max(worstF, std::abs(df)); sumF += df; ++nF;
                        }
                    }
                }
                const size_t stepV = std::max<size_t>(1, s_verts.size() / 500);
                for (size_t vi = 0; vi < s_verts.size(); vi += stepV) {
                    // ⚠️ `s_verts` YA ESTA EN MUNDO. `center` es el ANCLA y el dibujo la RESTA (ver
                    // abajo); sumarla aqui daba desvios de 136 000 km — el instrumento, no la malla.
                    const glm::dvec3 w = s_verts[vi];
                    const double r = glm::length(w - pc);
                    if (r < 1.0) continue;
                    const double alt  = r - pr;                     // cota del vertice de Jolt
                    const double real = f.planets->sampleTerrainHeight(w);
                    const double d = alt - real;
                    worst = std::max(worst, std::abs(d)); sum += d; ++n;
                    // Cerca es donde se pisa y donde se compara con lo dibujado; lejos las celdas son
                    // kilometricas y un desvio grande ahi no significa lo mismo.
                    if (glm::length(w - center) < Haruka::Planet::TERRAIN_COLLIDE_UNIFORM_M) {
                        worstN = std::max(worstN, std::abs(d)); sumN += d; ++nN;
                    }
                }
                HARUKA_LOGI("Sonda", "BAJO LOS PIES (<10 m): %zu vertices · desvio medio %+.4f m "
                            "· peor %.4f m", nF, nF ? sumF / (double)nF : 0.0, worstF);
                if (n) HARUKA_LOGI("Sonda", "malla de JOLT vs funcion de altura: TODO %zu vert "
                                   "medio %+.4f m peor %.4f m · CERCA (<%.0f m) %zu vert "
                                   "medio %+.4f m peor %.4f m",
                                   n, sum / (double)n, worst,
                                   Haruka::Planet::TERRAIN_COLLIDE_UNIFORM_M,
                                   nN, nN ? sumN / (double)nN : 0.0, worstN);
            }
        }
    }

    // Solo se re-suben los buffers cuando la física reconstruye el parche (una vez cada 48 m de
    // deriva), no cada frame: son ~88 k vértices.
    // ⚠️ La recarga mira TAMBIÉN la versión de estáticos: los OBB de props se rehacen cada ~30 m
    // (mucho más a menudo que la malla del suelo), y sin esto el alambre de los props se quedaba
    // congelado en las cajas del primer refresco — que es exactamente la duda que vino a resolver.
    const uint64_t propVer = f.physics->staticsVersion();
    if (rev != m_lineRev || propVer != m_propVer) {
        m_lineRev = rev;
        m_propVer = propVer;
        // ⚠️ SOLO EL ENTORNO CERCANO. El parche de colisión llega a 200 km y sus celdas del borde son
        // kilométricas: dibujarlo entero son 175 k triángulos de alambre que además tapan la pantalla.
        // Lo que hay que poder comparar es el suelo que se pisa, así que se recorta a lo que el bloque
        // uniforme cubre (±256 m, `TERRAIN_COLLIDE_UNIFORM_M`) con algo de margen.
        const double kShowM = Haruka::Planet::TERRAIN_COLLIDE_UNIFORM_M * 1.25;
        const glm::dvec3 camD = glm::dvec3(f.camera->position);
        std::vector<glm::vec3> lines;
        lines.reserve(4096);
        for (size_t i = 0; i + 2 < s_tris.size(); i += 3) {
            const glm::dvec3& a = s_verts[s_tris[i]];
            const glm::dvec3& b = s_verts[s_tris[i + 1]];
            const glm::dvec3& c = s_verts[s_tris[i + 2]];
            // Centroide dentro del radio: recortar por triángulo (no por vértice) evita aristas
            // sueltas que salen del borde y despistan.
            // Recorte alrededor del ANCLA, no de la cámara: el buffer se construye una vez por
            // reconstrucción, así que un recorte centrado en la cámara dejaba el disco visible
            // congelado contra una posición vieja y su borde derivaba con el jugador.
            const glm::dvec3 mid = (a + b + c) / 3.0;
            if (glm::length(mid - center) > kShowM) continue;
            // ⚠️ La resta cámara→vértice va en DOUBLE y solo el offset baja a float. Con las
            // posiciones de mundo (~6,37e6) en float, el alambre temblaría casi un metro respecto al
            // terreno — el mismo problema de cancelación que se arregló en el propio terreno, y aquí
            // haría que el instrumento inventara la disparidad que viene a medir.
            // ⚠️ RELATIVO AL CENTRO DE LA MALLA, NO A LA CÁMARA. Este buffer se construye una sola vez
            // por reconstrucción de la física (cada 48 m de deriva) y el shader dibuja con una matriz
            // SIN traslación, o sea que lo que se suba aquí queda rígidamente pegado a la cámara: al
            // caminar, la lámina verde caminaba contigo y se deslizaba sobre el terreno hasta 48 m,
            // cortando el suelo en diagonal. El instrumento inventaba justo la disparidad que venía a
            // medir — y el autotest de abajo no podía verlo, porque audita `s_verts` en coordenadas de
            // mundo, que están bien; lo que estaba mal era el DIBUJO.
            //
            // `center` es el ancla de la malla y no se mueve entre reconstrucciones, así que el offset
            // es estable. La traslación al ojo se aplica por frame, en la matriz (ver más abajo).
            const glm::vec3 fa = glm::vec3(a - center), fb = glm::vec3(b - center), fc = glm::vec3(c - center);
            lines.push_back(fa); lines.push_back(fb);
            lines.push_back(fb); lines.push_back(fc);
            lines.push_back(fc); lines.push_back(fa);
        }
        // ── CAJAS DE LOS PROPS (árboles y rocas) ────────────────────────────────────────────────
        //
        // Sin esto el alambre solo enseñaba el SUELO, así que "¿los árboles tienen collider y dónde?"
        // no era una pregunta que se pudiera mirar — solo deducir. Cada OBB se dibuja con sus 12
        // aristas, en el mismo marco relativo a `center` que el resto del buffer.
        {
            const auto& obbs = f.physics->getPropOBBs();
            for (const auto& o : obbs) {
                if (glm::length(o.center - center) > kShowM) continue;
                // Los 8 vértices de la caja: centro ± cada semieje, en el marco del OBB.
                glm::vec3 v[8];
                for (int k = 0; k < 8; ++k) {
                    const glm::dvec3 sgn((k & 1) ? 1.0 : -1.0,
                                         (k & 2) ? 1.0 : -1.0,
                                         (k & 4) ? 1.0 : -1.0);
                    const glm::dvec3 local = o.halfExtents * sgn;
                    v[k] = glm::vec3((o.center + o.rot * local) - center);
                }
                // 12 aristas del cubo por índices de vértice.
                static const int E[12][2] = {
                    {0,1},{2,3},{4,5},{6,7},   // en X
                    {0,2},{1,3},{4,6},{5,7},   // en Y
                    {0,4},{1,5},{2,6},{3,7}    // en Z
                };
                for (const auto& e : E) { lines.push_back(v[e[0]]); lines.push_back(v[e[1]]); }
            }

            // ── MALLAS DE COLISIÓN (lo que la física usa de verdad) ─────────────────────────────
            //
            // ⚠️ Sin esto el alambre no enseñaba NADA de los props: al pasar a mallas dejaron de
            // existir las cajas y los conos que dibujaba. Un instrumento que se queda ciego al
            // cambiar lo que mide no sirve — es la segunda vez que pasa hoy con este mismo alambre.
            //
            // Se acota a un radio CORTO: son ~500 triángulos por prop y dibujarlos todos serían
            // cientos de miles de líneas. Lo que hace falta es ver la forma alrededor del jugador.
            {
                const double kMeshShowM = 40.0;
                const glm::dvec3 camNow = glm::dvec3(f.camera->position);
                for (const auto& mi : f.physics->getPropMeshInstances()) {
                    if (glm::length(mi.center - camNow) > kMeshShowM) continue;
                    // ¿De qué prototipo/parte es esta forma? Se busca por id en el cache.
                    const std::vector<glm::vec3>* tris = nullptr;
                    if (!f.meshShapes || !f.meshCpu) break;
                    for (const auto& [proto, ids] : (*f.meshShapes)) {
                        for (size_t part = 0; part < ids.size(); ++part) {
                            if (ids[part] != mi.shapeId) continue;
                            auto it = f.meshCpu->find(proto);
                            if (it != f.meshCpu->end() && part < it->second.size())
                                tris = &it->second[part];
                            break;
                        }
                        if (tris) break;
                    }
                    if (!tris) continue;
                    for (size_t t = 0; t + 2 < tris->size(); t += 3) {
                        glm::vec3 w[3];
                        for (int k = 0; k < 3; ++k) {
                            const glm::dvec3 lp = glm::dvec3((*tris)[t + k]) * mi.scale;
                            w[k] = glm::vec3((mi.center + mi.rot * lp) - center);
                        }
                        lines.push_back(w[0]); lines.push_back(w[1]);
                        lines.push_back(w[1]); lines.push_back(w[2]);
                        lines.push_back(w[2]); lines.push_back(w[0]);
                    }
                }
            }

            // ── CONOS (troncos y ramas) ─────────────────────────────────────────────────────────
            //
            // ⚠️ Se dibujan APARTE porque no están en `propOBBs`: al darles su forma real pasaron a
            // `propCones`, y el alambre —que solo leía las cajas— dejó de enseñar los árboles justo
            // cuando cambiaron de forma. Un instrumento que se queda ciego al tocar lo que mide es
            // peor que no tenerlo.
            //
            // Se pinta el contorno de verdad: dos anillos (base y punta, con SUS radios) y unas
            // generatrices que los unen. Así se ve el taper, que es lo que distingue el cono de la
            // caja que había antes.
            for (const auto& c : f.physics->getPropCones()) {
                if (glm::length(c.center - center) > kShowM) continue;
                const int kSeg = 8;
                glm::vec3 ringBot[kSeg], ringTop[kSeg];
                for (int k = 0; k < kSeg; ++k) {
                    const double a = 6.283185307179586 * (double)k / (double)kSeg;
                    const glm::dvec3 offB(std::cos(a) * c.rBottom, -c.halfHeight, std::sin(a) * c.rBottom);
                    const glm::dvec3 offT(std::cos(a) * c.rTop,     c.halfHeight, std::sin(a) * c.rTop);
                    ringBot[k] = glm::vec3((c.center + c.rot * offB) - center);
                    ringTop[k] = glm::vec3((c.center + c.rot * offT) - center);
                }
                for (int k = 0; k < kSeg; ++k) {
                    const int k2 = (k + 1) % kSeg;
                    lines.push_back(ringBot[k]); lines.push_back(ringBot[k2]);   // anillo de la base
                    lines.push_back(ringTop[k]); lines.push_back(ringTop[k2]);   // anillo de la punta
                    if ((k % 2) == 0) {                                          // generatrices (la mitad)
                        lines.push_back(ringBot[k]); lines.push_back(ringTop[k]);
                    }
                }
            }
        }

        m_lineVerts = (uint32_t)lines.size();
        if (RHI::valid(m_lineVB)) { dev->destroy(m_lineVB); m_lineVB = {}; }
        if (m_lineVerts > 0)
            m_lineVB = dev->createBuffer(RHI::BufferUsage::Vertex,
                                            lines.size() * sizeof(glm::vec3), lines.data());
        // ── AUTOVERIFICACIÓN DEL INSTRUMENTO ────────────────────────────────────────────────────
        //
        // Antes de creerse lo que el alambre enseña hay que saber si el alambre está bien puesto. Y
        // hay una comprobación EXACTA disponible: los vértices de la malla de colisión se generaron
        // muestreando la función de altura en esos puntos, así que **en un vértice la malla ES la
        // función**, sin sagita ni interpolación de por medio. La desviación tiene que ser ~0.
        //
        // Si sale ~0, el alambre está donde dice y cualquier separación que se vea en pantalla es del
        // render. Si NO sale 0, el instrumento está desplazado y todo lo que sugiera es falso — que es
        // exactamente lo que hay que descartar antes de seguir buscando.
        if (f.planets) {
            glm::dvec3 pc; double pr = 0.0;
            if (f.planets->getActivePlanet(pc, pr)) {
                // ⚠️ DESCOMPUESTO EN TRES, porque la versión de una sola cifra decía «0,55 m» y no
                // permitía saber de cuál de los tres supuestos venía. Cada columna aísla uno:
                //
                //   triM0 : compara contra `sampleTerrainHeight(v)`, que usa `terrainTriM(0)` FIJO.
                //   triMr : contra el triM que el vértice usó DE VERDAD, `terrainTriM(radio tangente)`.
                //           Si esta baja a ~0 y la anterior no, el desajuste es de LOD, no de posición.
                //   radio : el rango de radios tangentes muestreados, para saber si el filtro de 320 m
                //           está cogiendo lo que se cree que coge (con triM plano ambos han de coincidir).
                double worst = 0.0, sum = 0.0, worstR = 0.0, sumR = 0.0;
                double radMin = 1e300, radMax = 0.0; int n = 0;
                double worstD = 0.0, worstRad = 0.0, worstAlt = 0.0, worstFunc = 0.0;
                int    over05 = 0, over02 = 0;
                // ⚠️ LOS OUTLIERS, CON SUS COORDENADAS. Con solo "el peor" no se puede decidir nada:
                // cuatro puntos con su radio y su signo dicen enseguida si comparten anillo, si caen
                // en una frontera de hueco o si estan repartidos. Es lo que cerro los dos bugs
                // anteriores de esta caza.
                struct Outlier { double rad, delta, alt; };
                std::vector<Outlier> outliers;
                const size_t stride = std::max<size_t>(1, s_verts.size() / 400);
                for (size_t i = 0; i < s_verts.size(); i += stride) {
                    const glm::dvec3& v = s_verts[i];
                    if (glm::length(v - center) > kShowM) continue;
                    const double alt  = glm::length(v - pc) - pr;         // cota del vértice capturado
                    const double func = f.planets->sampleTerrainHeight(v);
                    // Radio TANGENTE del vértice respecto al ancla de la malla: es el argumento con el
                    // que se eligió su triM al construirlo.
                    const glm::dvec3 up = glm::normalize(center - pc);
                    const glm::dvec3 rel = v - center;
                    const double radM = glm::length(rel - up * glm::dot(rel, up));
                    const double funcR = f.planets->sampleTerrainHeight(
                                             v, Haruka::Planet::terrainTriM(radM));
                    const double d = std::abs(alt - func), dR = std::abs(alt - funcR);
                    // ⚠️ DONDE, no solo cuanto. La media (3,7 mm) y el pico (0,104 m) dicen que son
                    // OUTLIERS, no una desviacion repartida: unos pocos vertices fuera de sitio. Un
                    // numero suelto no permite ir a mirarlos; su radio y su altura si.
                    if (d > worstD) { worstD = d; worstRad = radM; worstAlt = alt; worstFunc = func; }
                    worst = std::max(worst, d); sum += d;
                    if (d > 0.05) ++over05;
                    if (d > 0.02) { ++over02; if (outliers.size() < 12) outliers.push_back({ radM, alt - func, alt }); }
                    worstR = std::max(worstR, dR); sumR += dR;
                    radMin = std::min(radMin, radM); radMax = std::max(radMax, radM);
                    ++n;
                }
                if (n > 0) {
                    HARUKA_LOGI("CollisionWire", "AUTOTEST del alambre: %d vertices · triM0: peor %.4f "
                                "media %.4f · triMr: peor %.4f media %.4f · radio tangente %.0f..%.0f m "
                                "(deberia ser ~0: en un vertice la malla ES la funcion)",
                                n, worst, sum / n, worstR, sumR / n, radMin, radMax);
                    // La DISTRIBUCION separa "todo un poco mal" de "casi todo bien y unos pocos
                    // fuera": son dos causas distintas y el pico solo no lo dice.
                    HARUKA_LOGI("CollisionWire", "  distribucion: %d de %d por encima de 2 cm · %d por "
                                "encima de 5 cm  ->  %s",
                                over02, n, over05,
                                (over02 * 20 < n) ? "OUTLIERS (pocos vertices fuera de sitio)"
                                                  : "DESVIACION REPARTIDA (toda la malla)");
                    HARUKA_LOGI("CollisionWire", "  el PEOR: radio tangente %.1f m · malla %.3f m vs "
                                "funcion %.3f m (delta %+.4f m)",
                                worstRad, worstAlt, worstFunc, worstAlt - worstFunc);
                    for (const Outlier& o : outliers) {
                        // El anillo al que pertenece el radio: si todos caen en el mismo, o en una
                        // frontera entre dos, la causa es del anillo y no del terreno.
                        const auto lay = Haruka::Planet::terrainRingLayout(o.rad * 1.5 + 8.0);
                        int ring = -1; double cellM = 0.0;
                        for (size_t k = 0; k < lay.size(); ++k)
                            if (o.rad <= lay[k].extent) { ring = (int)k; cellM = lay[k].cell; break; }
                        HARUKA_LOGI("CollisionWire", "    outlier: radio %7.1f m · delta %+.4f m · "
                                    "alt %.2f m · anillo %d (celda %.2f m)",
                                    o.rad, o.delta, o.alt, ring, cellM);
                    }
                }
            }
        }
        HARUKA_LOGI("CollisionWire", "malla de colision: %zu triangulos en total, %u vertices de "
                    "alambre dentro de %.0f m del jugador", s_tris.size() / 3, m_lineVerts, kShowM);
    }
    if (m_lineVerts == 0 || !RHI::valid(m_lineVB)) return;

    // ⚠️ Una sola oportunidad: si el pipeline no se crea, no se reintenta cada frame. La primera
    // versión lo reintentaba y llenó el log con "FALLO" 60 veces por segundo.
    static bool s_dbgLineTried = false;
    if (!RHI::valid(m_linePSO) && !s_dbgLineTried) {
        s_dbgLineTried = true;
        // ⚠️ La ruta va por `Shader::baseDir()`, no "assets/shaders/..." a pelo. Es el mismo error que
        // el comentario del bloque de bloom advierte: sin la base, el fichero no se encuentra, el shader
        // queda en 0 y el programa no enlaza — que es exactamente lo que pasó al primer intento.
        const std::string vsP = Shader::baseDir() + "shaders/debug_lines.vert";
        const std::string fsP = Shader::baseDir() + "shaders/debug_lines.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath   = vsP.c_str();
        pd.fragmentPath = fsP.c_str();
        pd.vertexLayout.strides    = { (uint32_t)sizeof(glm::vec3) };
        pd.vertexLayout.attributes = { { 0, 0, RHI::Format::RGB32F, 0 } };
        pd.topology   = RHI::PrimitiveTopology::Lines;
        // Profundidad ACTIVA pero sin escribir: el alambre se oculta tras el relieve, que es lo que
        // permite ver si pasa por encima o por debajo del suelo dibujado. Con el test apagado flotaría
        // siempre sobre todo y no diría nada.
        pd.depth.test = true;  pd.depth.write = false;
        pd.cull       = RHI::CullMode::None;
        pd.blend.enable = false;
        m_linePSO = dev->createPipeline(pd);
        HARUKA_LOGI("CollisionWire", "pipeline de alambre: %s",
                    RHI::valid(m_linePSO) ? "ok" : "FALLO");
    }
    if (!RHI::valid(m_linePSO)) return;
    if (!RHI::valid(m_lineUBO))
        m_lineUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(glm::mat4) + sizeof(glm::vec4),
                                         nullptr, RHI::BufferMemory::Dynamic);
    // La traslación ancla→ojo se recalcula CADA FRAME y se dobla en la matriz. Es la mitad que faltaba:
    // los vértices son relativos al ancla (fijo) y esto los lleva al ojo (móvil). Antes los vértices ya
    // venían relativos a la cámara de hace hasta 48 m y esta matriz no tenía traslación, así que nada
    // corregía el desplazamiento. La resta va en DOUBLE y solo el resultado baja a float — con las
    // posiciones de mundo (~6,37e6) en float el alambre temblaría casi un metro.
    const glm::vec3 anchorRelEye = glm::vec3(center - glm::dvec3(f.camera->position));
    const glm::mat4 vp = glm::translate(viewProjRotOnly, anchorRelEye);
    struct { glm::mat4 vp; glm::vec4 color; } u{ vp, glm::vec4(0.15f, 1.0f, 0.35f, 1.0f) };
    dev->updateBuffer(m_lineUBO, 0, sizeof(u), &u);

    ctx->bindPipeline(m_linePSO);
    ctx->bindUniformBuffer(0, m_lineUBO);
    ctx->bindVertexBuffer(m_lineVB, 0);
    ctx->draw(m_lineVerts);
#else
    (void)ctx; (void)viewProjRotOnly;
#endif
}







} // namespace Haruka::Renderer
