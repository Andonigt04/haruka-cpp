/**
 * @file world_system_provider.h
 * @brief Adaptador CLIENTE de IWorldProvider sobre WorldSystem + PlanetarySystem.
 *
 * La física (HarukaPhysics) solo conoce `IWorldProvider` (sin GL). En el CLIENTE, el mundo real vive
 * en `WorldSystem` (cuerpos celestes, mar) y `PlanetarySystem` (altura del terreno). Este adaptador
 * traduce uno a otro. En el SERVIDOR (DGS) habrá otro adaptador sobre su estado autoritativo — misma
 * interfaz, sin este archivo (que sí depende del engine).
 */
#pragma once

#include "physics/world_provider.h"
#include "core/world_system.h"
#include "game/planetary_system.h"
#include "core/terrain/cube_sphere.h"
#include "core/planet/terrain_lod.h"
#include "core/logger.h"

#include <unordered_map>
#include <mutex>
#include <cmath>
#include <utility>   // std::pair — coordenadas (x,z) de un nodo de anillo
#include <limits>       // quiet_NaN — marca de "sin superficie" en los anillos de heightfield
#include <algorithm>
#include <thread>       // muestreo de anillos en paralelo (ver terrainHeightFieldRings)
#include <atomic>
#include <utility>
#include "tools/profiler.h"   // HARUKA_PROFILE: el coste de llenar los anillos no se medía

namespace Haruka {

class WorldSystemProvider : public Physics::IWorldProvider {
public:
    WorldSystemProvider(WorldSystem* world, PlanetarySystem* planetary)
        : m_world(world), m_planetary(planetary) {}

    bool       hasActivePlanet()    const override { return m_world && m_world->hasActivePlanet(); }
    glm::dvec3 activePlanetCenter() const override { return m_world ? m_world->getActivePlanetCenter() : glm::dvec3(0.0); }
    double     activePlanetRadius() const override { return m_world ? m_world->getActivePlanetRadius() : 0.0; }

    const std::vector<Physics::GravBody>& gravBodies() const override {
        // ⚠️ ANTES ESTA LISTA NO TENÍA PLANETAS.
        //
        // Se llenaba solo desde `WorldSystem::getBodies()`, y `syncFromScene` filtra ahí
        // `if (!flags.castLight) continue`: la lista de gravedad contenía ÚNICAMENTE las estrellas. El
        // planeta sobre el que caminas no estaba, así que la física tenía gravedad ~0 y los dos
        // caminos (personaje y cuerpos dinámicos) lo parcheaban por separado sumando a mano
        // `9.81·R²/r²` hacia el planeta activo. Eso hacía que TODO cuerpo tuviera la gravedad de la
        // Tierra —una luna te frenaba igual que un planeta— y que "cuánto tiras hacia abajo" tuviera
        // dos definiciones en vez de una.
        //
        // Ahora la lista lleva todos los cuerpos masivos con su jerarquía, y la gravedad sale de UNA
        // función de sus masas (core/planet/soi.h). Los parches desaparecen.
        m_grav.clear();
        if (m_world) {
            const auto& bodies = m_world->getBodies();
            m_grav.reserve(bodies.size() + (m_planetary ? m_planetary->getPlanets().size() : 0));
            for (const auto& b : bodies)
                m_grav.push_back({ b.worldPos, b.mass, (double)b.radius, 0.0, -1 });
        }
        if (m_planetary) {
            // La estrella más masiva es el padre por defecto de los planetas: es lo que hace que la
            // SOI de un planeta esté ACOTADA. Sin padre su influencia sería infinita y un planeta
            // secuestraría el marco de referencia desde el otro extremo del sistema.
            int starIdx = -1;
            for (size_t i = 0; i < m_grav.size(); ++i)
                if (starIdx < 0 || m_grav[i].mass > m_grav[(size_t)starIdx].mass) starIdx = (int)i;
            const double starMass = (starIdx >= 0) ? m_grav[(size_t)starIdx].mass : 0.0;

            const auto& planets = m_planetary->getPlanets();
            for (const auto& p : planets) {
                // Masa por el proxy COMPARTIDO (densidad media terrestre): un cuerpo de radio
                // terrestre da 9,82 m/s² en superficie, o sea que el jugador en la Tierra no nota el
                // cambio respecto al 9.81 que estaba a mano. Lo que cambia es que una luna pesa como
                // una luna.
                const double mass = Haruka::Planet::kEarthMeanDensity * (4.0 / 3.0)
                                  * 3.14159265358979323846 * p.radius * p.radius * p.radius;
                // El radio de la SOI necesita el semieje: sin órbita declarada no hay `a`, y se usa la
                // distancia actual a la estrella, que es lo mismo para una órbita circular.
                double a = p.orbit.a;
                if (a <= 0.0 && starIdx >= 0)
                    a = glm::length(glm::dvec3(p.position) - m_grav[(size_t)starIdx].pos);
                const double soi = Haruka::Planet::laplaceSoiRadius(a, mass, starMass);
                m_grav.push_back({ glm::dvec3(p.position), mass, p.radius, soi, starIdx });
            }
        }
        return m_grav;
    }

    /** @brief Índice en `gravBodies()` del cuerpo cuya gravedad domina en `worldPos` (-1 si ninguno).
     *  Es quien define el marco local, el terreno con el que colisionas y el cielo que ves. */
    int dominantBody(const glm::dvec3& worldPos) const override {
        return Haruka::Planet::dominantBodyIndex(gravBodies(), worldPos);
    }

    double terrainHeightAt(const glm::dvec3& worldPos) const override {
        // LA SUPERFICIE DE REFERENCIA, la misma que leen props, scripts y el render (ver
        // PlanetarySystem::sampleTerrainHeight). La física NO puede tener su propio suelo: cuando lo
        // tuvo —analítico puntual sobre una retícula de 8 m inventada— caminabas sobre una superficie
        // que no era la dibujada (7.81 m de pico con meso) y los props flotaban.
        if (!m_planetary || !hasActivePlanet()) return 0.0;
        return m_planetary->sampleTerrainHeight(worldPos);
    }

    /** @brief La superficie del agua CON LA OLA, en las mismas unidades que `terrainHeightAt`.
     *
     *  Misma disciplina que el suelo: la física NO tiene un mar propio. `sampleWaterLevel` es la única
     *  respuesta a "¿qué cota tiene el agua aquí?", y evalúa el gemelo CPU del shader con el reloj
     *  compartido — así lo que se flota es lo que se dibuja. */
    double waterSurfaceAt(const glm::dvec3& worldPos) const override {
        if (!m_planetary || !hasActivePlanet()) return kNoWater;
        const double lvl = m_planetary->sampleWaterLevel(worldPos);
        return (lvl > PlanetarySystem::kNoWater * 0.5) ? lvl : kNoWater;
    }

    /** @brief Velocidad orbital del agua en la superficie (m/s). Ver `oceanWaveVelocity`. */
    glm::dvec3 waterVelocityAt(const glm::dvec3& worldPos) const override {
        if (!m_planetary || !hasActivePlanet()) return glm::dvec3(0.0);
        return m_planetary->sampleWaterVelocity(worldPos);
    }

// (Fase 2b/3) Malla LOCAL del terreno para la colisión de Jolt: se muestrea la altura de la MALLA en
    // una rejilla TANGENTE alrededor de `center` (parche localmente plano sobre la esfera). Sigue el
    // terreno REAL (mismo `sampleTerrainHeight` que la altura) sin tener que iterar el set de chunks.
    // §9 Fase 3: la rejilla pasa de uniforme (32×32, ±96 m, paso 6 m) a una NO-UNIFORME geométrica:
    // paso ~3 m en el centro (lo que se pisa) que crece ×1.12 por anillo hasta cubrir ±radius (~200-300
    // km, el horizonte lejano), de modo que la colisión alcanza todo el terreno visible igual que lo
    // pisa el render. Por ser un rectángulo estricto no hay T-junctions ni costuras; la densidad se
    // agota donde el render ya no distingue triángulos (rango de anillos del §9).
    bool terrainMesh(const glm::dvec3& center, double radius,
                     std::vector<glm::dvec3>& outVerts, std::vector<uint32_t>& outTris) const override {
        if (!m_world || !m_planetary || !m_world->hasActivePlanet()) return false;
        const glm::dvec3 pc = m_world->getActivePlanetCenter();
        const double     R  = m_world->getActivePlanetRadius();
        const glm::dvec3 rel = center - pc;
        const double d = glm::length(rel);
        if (d < 1e-6 || R <= 0.0) return false;
        // Marco tangente: el MISMO que llena ClipParams y que reconstruyen terrain.tese/biome.frag
        // (`terrainClipFrame`). Antes esto tenía su propia copia con el producto vectorial al revés
        // — inocuo por la simetría de la rejilla, pero eran dos convenciones para un solo marco.
        // MISMO marco ANCLADO que el clipmap: si la colisión usara un marco sin anclar, las dos
        // retículas nacerían desfasadas y la disparidad no podría bajar de la cuerda de la celda.
        glm::dvec3 up, t1, t2;
        Haruka::Planet::terrainClipFrame(center, pc, up, t1, t2, R);
        // ── Rejilla NO-uniforme: paso ~innerStep al pie, ×growth por anillo hasta ±radius. Barata de
        // construir (una entrada por anillo de distancia) y estrictamente rectangular → sin costuras
        // entre densidades. Un rectángulo de ~200-300 km cuesta así una pocas decenas de miles de
        // vértices: las celdas lejanas son kilométricas, justo lo que la colisión necesita a ese rango.
        // La genera `terrainRingGrid` (terrain_lod.h), que es también la que auditan los tests.
        const double halfR = std::max(radius, 200.0);
        const std::vector<double> xs = Haruka::Planet::terrainRingGrid(halfR);
        const std::vector<double> zs = xs;              // misma retícula en X y Z (cuadrado)
        Haruka::WorldGenParams W; double Rp;
        if (!m_planetary->getActivePlanetParams(W, Rp)) return false;
        const size_t N = xs.size();
        outVerts.clear(); outVerts.reserve(N * N);
        // triM POR PUNTO, IGUAL que el clipmap. La física no puede usar un triM FIJO: el render
        // evalúa la rejilla con un tamaño de triángulo que crece con la distancia, y la malla de
        // colisión debe pisar EXACTAMENTE esa misma superficie (regla de paridad §5/§9, no se
        // negocia). Ya no se escribe la fórmula aquí: `terrainTriM` es la única definición, y es la
        // que llaman también el test y (como gemelo declarado) los shaders.
        // ── LA DIRECCIÓN SE CALCULA COMO LA CALCULA EL SHADER, no "equivalente" ──────────────────
        //
        // Aquí había DOS fallos que juntos separaban el suelo que se pisa del que se ve (los medía
        // `clipmap_dir_parity`, borrado junto con el clipmap: las cifras son históricas):
        //
        //  1. ORIGEN SIN ANCLAR (0,58 m de altura). La línea era
        //         normalize(rel + t1*x + t2*z)
        //     con `rel = center - pc`, la dirección SIN anclar — mientras `t1`/`t2` venían del marco
        //     ANCLADO. Ejes de una retícula y origen de otra. El comentario de arriba dice la intención
        //     («MISMO marco ANCLADO … si la colisión usara un marco sin anclar, las dos retículas
        //     nacerían desfasadas») y la línea hacía justo lo contrario: el anclaje se calculaba y se
        //     tiraba. El desfase es de hasta medio quad TANGENCIAL, así que las dos retículas no
        //     comparten vértices y la separación pasa de la sagita a la pendiente local por 2 m.
        //
        //  2. PRECISIÓN DISTINTA (0,145 m). El tese llega a su dirección en FLOAT desde el marco
        //     (`normalize(uClipOrigin + uClipTanU*loc.x/R + uClipTanV*loc.y/R)`, y el marco viaja en el
        //     UBO como `vec3`). Calcularla en double da OTRA dirección: un ulp de un unitario son
        //     ~0,4-0,9 m sobre la superficie, y la altura se muestrea EN ese punto.
        //
        // Se replica el shader exactamente: origen = `up` ANCLADO y unitario, marco recortado a float
        // como lo recorta el UBO, y el MISMO orden de operaciones —`(tan*loc)/R`, no `tan*(loc/R)`—
        // porque en float no son la misma cuenta. Así la colisión no se aproxima a la superficie
        // dibujada: es la superficie dibujada.
        const glm::vec3 fUp(up), fT1(t1), fT2(t2);
        const float     fR = (float)R;
        for (double z : zs) for (double x : xs) {
            const glm::vec3 dirF = glm::normalize(fUp + (fT1 * (float)x) / fR + (fT2 * (float)z) / fR);
            // ⚠️ RENORMALIZAR EN DOUBLE, y no es redundante aunque `dirF` ya venga de un `normalize`.
            //
            // `normalize` en float deja el módulo en 1 ± 1,2e-7, no en 1. Da igual para la DIRECCIÓN
            // —que es lo que hay que compartir bit a bit con el shader, y por eso el cálculo de `dirF`
            // sigue siendo float con el orden de operaciones del tese— pero aquí ese vector se
            // MULTIPLICA por el radio de la Tierra: 6,37e6 · 1,2e-7 = **0,76 m de error radial**. El
            // vértice acababa hasta 76 cm por encima o por debajo de su altitud, en una malla cuyo
            // objetivo declarado es 0.
            //
            // Costó encontrarlo porque la comprobación obvia no lo ve: reevaluar la función EN el
            // vértice devuelve exactamente la altura con la que se colocó (medido: 0.0000 m sobre
            // 128 949 vértices), porque el error es RADIAL y la función solo depende de la dirección.
            // Lo destapó el autotest del alambre, que mide `length(v - centro) - radio` y por eso sí lo
            // ve: 0,55 m peor, del orden exacto que predice el ulp.
            //
            // Renormalizar en double NO rompe la paridad: conserva la dirección (cuya cuantización a
            // float es lo que se comparte con la GPU) y solo corrige el módulo.
            const glm::dvec3 dir = glm::normalize(glm::dvec3(dirF));
            const double radM = std::sqrt(x * x + z * z);                     // distancia tangente al centro
            const float  triM = Haruka::Planet::terrainTriM(radM);            // MISMO triM que el clipmap
            const double h = m_planetary->sampleTerrainHeight(pc + dir * R, triM);  // física = render
            const glm::dvec3 P = pc + dir * (R + h);
            outVerts.push_back(P);                                            // punto de superficie (mundo)
            // ⚠️ AUTOCOMPROBACIÓN EN EL SITIO. El autotest del alambre (application_render.cpp) mide
            // 0,55 m donde tiene que dar 0, y todas las hipótesis externas están descartadas por
            // medición: mismo centro de planeta en las dos listas, mismo radio, mismo triM. Aquí están
            // TODOS los datos a la vez, así que la pregunta se hace donde no hay intermediarios:
            // ¿reevaluar la función EN el vértice devuelve la altura con la que se colocó?
        }
        // ── HUECO PARA EL HEIGHTFIELD ───────────────────────────────────────────────────────────
        //
        // Las celdas que caen ENTERAS dentro del bloque no se emiten: ese trozo lo aporta
        // `terrainHeightField` como `HeightFieldShape`. Si se emitieran las dos, Jolt tendría DOS
        // superficies sobre el mismo suelo y generaría contactos dobles — el personaje rebota o se
        // engancha en la frontera, que es peor que el coste que se quería ahorrar.
        //
        // El hueco es exacto porque `TERRAIN_HF_LO`/`HI` son nodos de esta misma rejilla (múltiplos del
        // quad dentro del bloque uniforme): ninguna celda queda a caballo.
        outTris.clear(); outTris.reserve((size_t)(N - 1) * (N - 1) * 6);
        size_t skipped = 0;
        for (size_t j = 0; j < N - 1; ++j) for (size_t i = 0; i < N - 1; ++i) {
            if (Haruka::Planet::terrainHeightFieldCovers(xs[i], xs[i + 1]) &&
                Haruka::Planet::terrainHeightFieldCovers(zs[j], zs[j + 1])) { ++skipped; continue; }
            const uint32_t a = (uint32_t)(j * N + i), b = a + 1, c = a + (uint32_t)N, dd = c + 1;
            outTris.insert(outTris.end(), { a, b, c, b, dd, c });   // winding → normal saliente
        }
        (void)skipped;
        return true;
    }

    // EL BLOQUE CERCANO COMO HEIGHTFIELD (ver IWorldProvider). Comparte marco, quad y función con
    // `terrainMesh`: son la misma superficie descrita de dos maneras, no dos aproximaciones.
    bool terrainHeightField(const glm::dvec3& center, std::vector<float>& outSamples, uint32_t& outN,
                            double& outCell, glm::dvec3& outOrigin,
                            glm::dvec3& outT1, glm::dvec3& outUp, glm::dvec3& outT2) const override {
        if (!m_world || !m_planetary || !m_world->hasActivePlanet()) return false;
        const glm::dvec3 pc = m_world->getActivePlanetCenter();
        const double     R  = m_world->getActivePlanetRadius();
        if (glm::length(center - pc) < 1e-6 || R <= 0.0) return false;

        glm::dvec3 up, t1, t2;
        Haruka::Planet::terrainClipFrame(center, pc, up, t1, t2, R);   // MISMO marco anclado que la malla
        // ⚠️ EL EJE Z DEL HEIGHTFIELD ES `-t2`, NO `t2`. `terrainClipFrame` entrega `(t1, t2, up)` con
        // `cross(t1,t2) == up`, o sea que la terna `(t1, up, t2)` —el orden en que un heightfield espera
        // sus ejes (X, altura, Z)— tiene determinante **-1**: es levógira. Construir la rotación con
        // ella entrega a Jolt una superficie ESPEJADA respecto a la malla que la rodea, con el relieve
        // invertido en una diagonal. Negar `t2` la vuelve dextrógira, y se hace AQUÍ para que quien
        // genera las muestras y quien las coloca usen literalmente el mismo eje.
        const glm::dvec3 hz = -t2;
        outT1 = t1; outUp = up; outT2 = hz;
        outOrigin = pc + up * R;                    // punto de superficie del ancla (altura 0 del campo)
        outCell = Haruka::Planet::TERRAIN_CLIP_QUAD_M;
        outN    = Haruka::Planet::TERRAIN_HF_SAMPLES;

        // MISMA reconstrucción de `dir` que la malla y que `clipmap.tese`: marco recortado a float y el
        // orden `(tan*loc)/R`. No es pedantería — un ulp de un `dir` unitario son 0,38-0,76 m sobre la
        // superficie terrestre, así que hacerlo "equivalente pero distinto" separa las dos superficies.
        const glm::vec3 fUp(up), fT1(t1), fHz(hz);
        const float     fR = (float)R;
        outSamples.clear(); outSamples.resize((size_t)outN * outN);
        for (uint32_t j = 0; j < outN; ++j) {
            const double z = Haruka::Planet::TERRAIN_HF_LO + (double)j * outCell;
            for (uint32_t i = 0; i < outN; ++i) {
                const double x = Haruka::Planet::TERRAIN_HF_LO + (double)i * outCell;
                const glm::vec3 dirF = glm::normalize(fUp + (fT1 * (float)x) / fR + (fHz * (float)z) / fR);
                // ⚠️ RENORMALIZAR EN DOUBLE, y no es redundante aunque `dirF` ya venga de un `normalize`.
            //
            // `normalize` en float deja el módulo en 1 ± 1,2e-7, no en 1. Da igual para la DIRECCIÓN
            // —que es lo que hay que compartir bit a bit con el shader, y por eso el cálculo de `dirF`
            // sigue siendo float con el orden de operaciones del tese— pero aquí ese vector se
            // MULTIPLICA por el radio de la Tierra: 6,37e6 · 1,2e-7 = **0,76 m de error radial**. El
            // vértice acababa hasta 76 cm por encima o por debajo de su altitud, en una malla cuyo
            // objetivo declarado es 0.
            //
            // Costó encontrarlo porque la comprobación obvia no lo ve: reevaluar la función EN el
            // vértice devuelve exactamente la altura con la que se colocó (medido: 0.0000 m sobre
            // 128 949 vértices), porque el error es RADIAL y la función solo depende de la dirección.
            // Lo destapó el autotest del alambre, que mide `length(v - centro) - radio` y por eso sí lo
            // ve: 0,55 m peor, del orden exacto que predice el ulp.
            //
            // Renormalizar en double NO rompe la paridad: conserva la dirección (cuya cuantización a
            // float es lo que se comparte con la GPU) y solo corrige el módulo.
            const glm::dvec3 dir = glm::normalize(glm::dvec3(dirF));
                // ⚠️ EL CORTE VA CON LA CELDA DE ESTE ANILLO, NO CON EL TÉXEL DEL RENDER. NO REINTENTARLO.
        //
        // Se probó bajar el piso a 0,596 m (el téxel del nodo más fino) para que el suelo que se pisa
        // y el que se ve coincidieran: la disparidad bajaba de 0,2539 m a 0. **Y era un error**, del
        // tipo exacto que `TERRAIN_TRIM_FLOOR` documenta: la celda del anillo cercano mide 4 m, así
        // que meterle octavas de 0,6 m es SUB-NYQUIST — el "hervido". Medido en el juego:
        //
        //     twist del quad de 4 m:  0.035 m -> 0.108 m a <8 m del jugador  (0.082 -> 0.225 peor)
        //
        // Tres veces peor justo donde se camina, y encima inestable al moverse. La disparidad de
        // 0,2539 m es ESTRUCTURAL: la colisión es una rejilla de 4 m y el render de 0,596 m. La única
        // forma de cerrarla es afinar la rejilla de colisión — 45× más muestras, ~135 ms — no mover
        // el corte de octavas.
        const float triM = Haruka::Planet::terrainTriM(std::sqrt(x * x + z * z));
                const double h = m_planetary->sampleTerrainHeight(pc + dir * R, triM);
                // ⚠️ Componente RADIAL respecto al plano tangente, no la altitud. Un heightfield es
                // PLANO y el terreno está sobre una esfera: el nodo a 256 m está 5,1 mm por debajo del
                // plano tangente por pura curvatura (x²/2R). Si se guardara la altitud a secas, el
                // borde del bloque quedaría 5 mm por encima de la malla que lo rodea — un escalón
                // pequeño pero REAL justo en la costura, y las costuras es donde el personaje se
                // engancha. Proyectar sobre `up` lo incluye exactamente, sin fórmula aparte.
                const glm::dvec3 P = pc + dir * (R + h);
                outSamples[(size_t)j * outN + i] = (float)glm::dot(P - outOrigin, up);
            }
        }
        return true;
    }

    // LA MUESTRA DE UN NODO DE ANILLO. Existe como función y no escrita dos veces porque la usan la
    // CONSTRUCCIÓN de los anillos y la SONDA que los audita: si fueran dos copias, la sonda auditaría
    // su propia copia y daría 0 pase lo que pase (es literalmente el fallo de `detail_triM_parity`).
    //
    // Devuelve la componente RADIAL respecto al plano tangente, no la altitud: un heightfield es plano
    // y el terreno está sobre una esfera. Proyectar sobre `up` mete la caída por curvatura (x²/2R)
    // exactamente — a 262 km son 5,4 km, así que no es un detalle.
    float ringSample(const PlanetarySystem::TerrainSampler& src,
                     const glm::dvec3& pc, double R, const glm::dvec3& up,
                     const glm::dvec3& t1d, const glm::dvec3& hzd,
                     const glm::dvec3& org, double x, double z) const {
        // ⚠️ LA DIRECCION VA EN DOUBLE. ANTES SE CALCULABA EN FLOAT A PROPOSITO, Y ESE ERA EL SUELO
        // QUE SE PISABA.
        //
        // El comentario original decía: *"se calcula EN FLOAT con el orden de operaciones del tese"*,
        // para casar bit a bit con `clipmap.tese`. **Ese shader se borró el 2026-08-24**: hoy el
        // render es el pase de nodos, que saca la dirección de ENTEROS en double y es exacta. Así que
        // la razón para bajar a float desapareció, y lo que quedó fue solo su coste.
        //
        // Y el coste no es teórico: un ulp de un vector unitario en float son **0,38-0,76 m de
        // superficie** (`float-ulp-precision-wall`), y el redondeo es INDEPENDIENTE muestra a
        // muestra. Sobre una rejilla de colisión de 0,5-4 m eso no es una deformación suave: es ruido
        // a la frecuencia de la rejilla — micro-pendientes que el controlador del personaje pelea
        // frame a frame. Reportado como "me atasco, me deslizo y va pesado", con el terreno REAL
        // midiendo 13,8° de pendiente máxima entre celdas (`terrain_collision_walkability`).
        //
        // Es el MISMO fallo que se acaba de arreglar en `terrain_node.vert`, por el otro lado: allí
        // deformaba lo que se ve, aquí lo que se pisa.
        const glm::dvec3 dir = glm::normalize(up + (t1d * x) / R + (hzd * z) / R);
        // ⚠️ EL CORTE VA CON LA CELDA DE ESTE ANILLO, NO CON EL TÉXEL DEL RENDER. NO REINTENTARLO.
        //
        // Se probó bajar el piso a 0,596 m (el téxel del nodo más fino) para que el suelo que se pisa
        // y el que se ve coincidieran: la disparidad bajaba de 0,2539 m a 0. **Y era un error**, del
        // tipo exacto que `TERRAIN_TRIM_FLOOR` documenta: la celda del anillo cercano mide 4 m, así
        // que meterle octavas de 0,6 m es SUB-NYQUIST — el "hervido". Medido en el juego:
        //
        //     twist del quad de 4 m:  0.035 m -> 0.108 m a <8 m del jugador  (0.082 -> 0.225 peor)
        //
        // Tres veces peor justo donde se camina, y encima inestable al moverse. La disparidad de
        // 0,2539 m es ESTRUCTURAL: la colisión es una rejilla de 4 m y el render de 0,596 m. La única
        // forma de cerrarla es afinar la rejilla de colisión — 45× más muestras, ~135 ms — no mover
        // el corte de octavas.
        const float triM = Haruka::Planet::terrainTriM(std::sqrt(x * x + z * z));
        // ⚠️ `src` YA RESUELTO por el llamador. Antes esto era `m_planetary->sampleTerrainHeight(...)`,
        // que por dentro rehacía la búsqueda del planeta —dos bucles y una comparación de `std::string`—
        // en cada una de las 235 564 muestras, para un puntero que no cambia. `heightAt` hace las mismas
        // cuentas que hacía aquella, así que el resultado es idéntico bit a bit.
        const glm::dvec3 wp = pc + dir * R;
        const double h = src ? src.heightAt(wp, triM) : m_planetary->sampleTerrainHeight(wp, triM);
        return (float)glm::dot(pc + dir * (R + h) - org, up);
    }

    // EL TERRENO COMO PILA DE ANILLOS (ver IWorldProvider). Nivel 0 = el bloque cercano de siempre;
    // cada nivel dobla celda y alcance con el centro agujereado donde llega el de dentro.
    //
    // Comparte con `terrainMesh` y `terrainHeightField` lo único que no se puede duplicar: el marco
    // anclado, la reconstrucción de `dir` en float con el orden del tese, la renormalización en
    // double y el `triM` por punto. Si alguna de esas cuatro cosas se calculara "equivalente pero
    // distinto", los anillos dejarían de ser la superficie que dibuja el render.
    bool terrainHeightFieldRings(const glm::dvec3& center, double halfExtent,
                                 std::vector<Haruka::Physics::IWorldProvider::TerrainRing>& outRings,
                                 glm::dvec3& outOrigin, glm::dvec3& outT1,
                                 glm::dvec3& outUp, glm::dvec3& outT2,
                                 size_t firstRing = 0) const override {
        if (!m_world || !m_planetary || !m_world->hasActivePlanet()) return false;
        const glm::dvec3 pc = m_world->getActivePlanetCenter();
        const double     R  = m_world->getActivePlanetRadius();
        if (glm::length(center - pc) < 1e-6 || R <= 0.0) return false;

        glm::dvec3 up, t1, t2;
        Haruka::Planet::terrainClipFrame(center, pc, up, t1, t2, R);
        // ⚠️ EJE Z NEGADO, por lo mismo que en `terrainHeightField`: `(t1, up, t2)` es LEVÓGIRA y un
        // heightfield espera sus ejes como (X, altura, Z) dextrógiros. Sin negar, Jolt recibe el
        // relieve espejado en una diagonal.
        const glm::dvec3 hz = -t2;
        outT1 = t1; outUp = up; outT2 = hz;
        outOrigin = pc + up * R;

        const glm::vec3 fUp(up), fT1(t1), fHz(hz);
        const float     fR = (float)R;
        const std::vector<Haruka::Planet::TerrainRingSpec> layout =
            Haruka::Planet::terrainRingLayout(halfExtent);
        if (firstRing >= layout.size()) return false;
        outRings.clear(); outRings.resize(layout.size() - firstRing);

        // EL PLANETA SE RESUELVE UNA VEZ, no por muestra. Ver `PlanetarySystem::TerrainSampler`.
        const PlanetarySystem::TerrainSampler src = m_planetary->terrainSampler(center);

        // ── EL COSTE VIVE AQUÍ, Y HASTA AHORA NO SE MEDÍA ───────────────────────────────────────
        //
        // Este bucle son ~235 564 muestras del terreno procedural. `terrain_lod.h` documenta el
        // coste de la MALLA que estos anillos sustituyeron, pero el muestreo de los anillos nunca
        // tuvo scope propio: se sabía que los 11 `HeightFieldShape` cuestan 18,3 ms y NO cuánto
        // cuesta llenarlos. Sin este número, cualquier rediseño del muestreo se justifica con una
        // extrapolación. El scope es THREAD-LOCAL, así que va en el hilo que llama, no dentro de
        // los workers.
        HARUKA_PROFILE("terrain.rings.sample");

        // ── PARALELO POR FILAS ──────────────────────────────────────────────────────────────────
        //
        // Las muestras son INDEPENDIENTES: cada una escribe su propio hueco y ninguna lee el de otra,
        // así que el reparto no cambia un solo bit del resultado — el determinismo cliente↔servidor
        // que exige el paso fijo se mantiene por construcción, no por convenio.
        //
        // `sampleHeight` es seguro desde varios hilos: lee solo `m_heightCPU`, inmutable tras el bake
        // (es la misma propiedad en la que ya se apoyan `nearJob`/`groundJob` para muestrear en un
        // worker). Se reparte por FILAS y no por anillos porque los anillos son muy desiguales — el
        // de ±2048 m tiene 258² muestras contra las 130² de los demás, o sea que un hilo por anillo
        // dejaría a uno con el 35 % del trabajo y a los otros esperándolo.
        std::vector<std::pair<size_t, uint32_t>> rows;   // (índice de anillo, fila)
        for (size_t k = firstRing; k < layout.size(); ++k) {
            auto& ring = outRings[k - firstRing];
            ring.spec = layout[k];
            ring.samples.assign((size_t)layout[k].samples * layout[k].samples, 0.0f);
            for (uint32_t j = 0; j < layout[k].samples; ++j) rows.emplace_back(k - firstRing, j);
        }
        auto doRow = [&](size_t r) {
            auto& ring = outRings[rows[r].first];
            const Haruka::Planet::TerrainRingSpec& spec = ring.spec;
            const uint32_t n = spec.samples, j = rows[r].second;
            const double z = Haruka::Planet::terrainRingNode(j, spec);
            for (uint32_t i = 0; i < n; ++i) {
                const double x = Haruka::Planet::terrainRingNode(i, spec);
                float& out = ring.samples[(size_t)j * n + i];
                // Sin superficie: el nodo sobrante del borde (índice 0, ver TERRAIN_RING_SAMPLES)
                // y el hueco central que cubre el nivel de dentro. Emitir los dos sería darle a
                // Jolt DOS superficies sobre el mismo suelo → contactos dobles en la frontera.
                if (i == 0 || j == 0 || Haruka::Planet::terrainRingHole(spec, x, z)) {
                    out = std::numeric_limits<float>::quiet_NaN();
                    continue;
                }
                out = ringSample(src, pc, R, up, t1, hz, outOrigin, x, z);
            }
        };
        const unsigned hw = std::thread::hardware_concurrency();
        // Un solo hilo si la máquina no lo dice o si hay tan poco que repartir que el arranque de los
        // hilos costaría más que el trabajo.
        const unsigned nThreads = (hw > 1 && rows.size() >= 64) ? std::min(hw, 16u) : 1u;
        if (nThreads <= 1) {
            for (size_t r = 0; r < rows.size(); ++r) doRow(r);
        } else {
            std::atomic<size_t> next{0};
            std::vector<std::thread> pool;
            pool.reserve(nThreads - 1);
            // Reparto DINÁMICO (cada hilo coge la siguiente fila libre) y no en bloques: las filas
            // cuestan muy distinto — las del hueco central salen por el `continue` sin muestrear nada
            // y las de fuera evalúan hasta 4 octavas. Con bloques fijos, el hilo que pillara el anillo
            // interior acabaría enseguida y se quedaría mirando.
            auto worker = [&] { for (size_t r; (r = next++) < rows.size(); ) doRow(r); };
            for (unsigned t = 1; t < nThreads; ++t) pool.emplace_back(worker);
            worker();
            for (auto& th : pool) th.join();
        }

        // ── COSER LAS COSTURAS ENTRE ANILLOS ────────────────────────────────────────────────────
        //
        // ⚠️ MEDIDO EN EL JUEGO: hasta **122,65 m de escalón** a 131 km (celda de 4 km). Es la peor
        // disparidad entre lo que se ve y lo que se pisa que queda en el motor — 500 veces la del
        // campo cercano.
        //
        // Es la MISMA T-junction que el pase de nodos ya resuelve: el vértice del anillo fino que no
        // tiene gemelo en el grueso no cae sobre la recta que une a sus dos vecinos gruesos, así que
        // el suelo se parte en la costura. `terrainRingSeamStep` lo mide exactamente así.
        //
        // Aquí se cierra POR CONSTRUCCIÓN: ese vértice se mueve a la recta. Adapta el FINO al grueso,
        // que es la dirección correcta —el grueso no puede representar lo que el fino ve— y es lo
        // mismo que hace `nodeStitch`.
        {
            HARUKA_PROFILE("terrain.rings.stitch");
            auto at = [](Haruka::Physics::IWorldProvider::TerrainRing& r, int i, int j) -> float& {
                return r.samples[(size_t)j * r.spec.samples + (size_t)i];
            };
            for (size_t k = 1; k < outRings.size(); ++k) {
                auto& F = outRings[k - 1];
                auto& C = outRings[k];
                const double e = F.spec.extent;
                if (std::abs(C.spec.hole - e) > 1e-9) continue;      // no comparten costura
                const double rho = C.spec.cell / F.spec.cell;
                if (!(rho > 1.0)) continue;
                const int cq = (int)std::llround(e / C.spec.cell);
                // ⚠️ LOS MISMOS INDICES QUE `terrainRingSeamStep`, literalmente. Los escribí a ojo
                // como {0, 2·half} y la medida usa {half+1-half, half+1+half} = {1, 2·half+1}: habría
                // cosido la fila de al lado y el escalón habría seguido ahí, con el test en verde.
                const int fineEdge[2]   = { F.spec.half + 1 - F.spec.half, F.spec.half + 1 + F.spec.half };
                const int coarseEdge[2] = { C.spec.half + 1 - cq, C.spec.half + 1 + cq };
                for (int side = 0; side < 2; ++side) {
                    if (coarseEdge[side] < 0 || coarseEdge[side] >= (int)C.spec.samples) continue;
                    for (int m = -F.spec.half; m <= F.spec.half; ++m) {
                        const double u  = (double)m / rho;
                        const double u0 = std::floor(u);
                        if (std::abs(u - u0) < 1e-9) continue;       // tiene gemelo grueso: ya coincide
                        const double w  = u - u0;
                        const int jf = m + F.spec.half + 1;
                        const int c0 = (int)u0 + C.spec.half + 1, c1 = c0 + 1;
                        if (c0 < 0 || c1 >= (int)C.spec.samples) continue;
                        if (jf < 0 || jf >= (int)F.spec.samples) continue;
                        auto lerp = [&](float a, float b) { return (float)(a + (b - a) * w); };
                        // La costura es un CUADRADO: hay columnas (x fijo) y filas (z fijo).
                        const float colC = lerp(at(C, coarseEdge[side], c0), at(C, coarseEdge[side], c1));
                        const float rowC = lerp(at(C, c0, coarseEdge[side]), at(C, c1, coarseEdge[side]));
                        // NaN = nodo sin superficie (hueco): no se toca, o se rellenaría el agujero.
                        float& colF = at(F, fineEdge[side], jf);
                        float& rowF = at(F, jf, fineEdge[side]);
                        if (colF == colF && colC == colC) colF = colC;
                        if (rowF == rowF && rowC == rowC) rowF = rowC;
                    }
                }
            }
        }
        return true;
    }

    // SONDA DE PARIDAD (ver IWorldProvider): qué suelo hay bajo `worldPos` según la función continua
    // y según la superficie que Jolt DE VERDAD colisiona.
    //
    // ⚠️ REESCRITA SOBRE LOS ANILLOS (2026-08-10). Reconstruía una celda de `terrainRingGrid` e
    // interpolaba BILINEAL — o sea medía la malla de triángulos, que ya no colisiona nada desde que el
    // suelo son heightfields. Su columna `malla=` describía una superficie inexistente, que es peor
    // que no tener sonda: un instrumento que sigue dando números después de que su objeto desaparezca.
    //
    // Ahora reproduce la aritmética de `HeightFieldShape::GetSurfacePosition`, que NO es bilineal:
    // Jolt parte cada celda en dos triángulos por la diagonal (x,y)→(x+1,y+1) y elige según
    // `y_frac >= x_frac`. La diferencia entre bilineal y triangulado es justo el twist del quad
    // (medido: 2-4 cm bajo los pies), o sea precisamente lo que esta sonda tiene que poder ver.
    bool terrainParityProbe(const glm::dvec3& worldPos, const glm::dvec3& meshCenter,
                            double& outFuncH, double& outMeshH, double& outCellM) const override {
        if (!m_world || !m_planetary || !m_world->hasActivePlanet()) return false;
        const glm::dvec3 pc = m_world->getActivePlanetCenter();
        const double     R  = m_world->getActivePlanetRadius();
        if (R <= 0.0) return false;

        outFuncH = terrainHeightAt(worldPos);

        // Marco EXACTAMENTE el de los anillos: anclado, centrado donde se construyeron, y con el eje
        // Z NEGADO (`hz`), que es el que se le entrega a Jolt.
        glm::dvec3 up, t1, t2;
        Haruka::Planet::terrainClipFrame(meshCenter, pc, up, t1, t2, R);
        const glm::dvec3 hz = -t2;
        const glm::dvec3 org = pc + up * R;
        const glm::vec3 fUp(up), fT1(t1), fHz(hz);
        const float     fR = (float)R;

        // Offset tangente del punto en el marco del heightfield.
        const glm::dvec3 d = worldPos - org;
        const double px = glm::dot(d, t1), pz = glm::dot(d, hz);

        // El anillo que lo cubre: el MÁS INTERNO cuya celda no cae en el hueco. Recorrer de dentro
        // a fuera reproduce el reparto real — dentro del hueco manda el anillo de dentro.
        const std::vector<Haruka::Planet::TerrainRingSpec> layout =
            Haruka::Planet::terrainRingLayout(200000.0);
        for (const auto& spec : layout) {
            if (std::abs(px) >= spec.extent || std::abs(pz) >= spec.extent) continue;
            // Índice de celda y fracción, como hace Jolt: `(local - offset) / scale`.
            const double lo = Haruka::Planet::terrainRingNode(0, spec);
            double xf = (px - lo) / spec.cell, zf = (pz - lo) / spec.cell;
            const int ix = (int)std::floor(xf), iz = (int)std::floor(zf);
            xf -= ix; zf -= iz;
            if (ix < 0 || iz < 0 || ix + 1 >= (int)spec.samples || iz + 1 >= (int)spec.samples)
                continue;
            auto nodeXZ = [&](int i, int j) {
                return std::pair<double, double>(Haruka::Planet::terrainRingNode((uint32_t)i, spec),
                                                 Haruka::Planet::terrainRingNode((uint32_t)j, spec));
            };
            // ¿Tiene superficie la celda? Basta con que un nodo esté en el hueco (o sea el sobrante
            // del índice 0) para que Jolt tire el quad entero.
            bool solid = true;
            for (int dx = 0; dx <= 1 && solid; ++dx) for (int dz = 0; dz <= 1 && solid; ++dz) {
                const auto p = nodeXZ(ix + dx, iz + dz);
                if (ix + dx == 0 || iz + dz == 0 ||
                    Haruka::Planet::terrainRingHole(spec, p.first, p.second)) solid = false;
            }
            if (!solid) continue;

            // La sonda de paridad es puntual (4 muestras), así que resolver el planeta aquí no está
            // en ningún camino caliente — pero se usa el MISMO `TerrainSampler` que el bucle masivo
            // para que las dos rutas no puedan describir suelos distintos.
            const PlanetarySystem::TerrainSampler src = m_planetary->terrainSampler(worldPos);
            auto S = [&](int i, int j) {
                const auto p = nodeXZ(i, j);
                return (double)ringSample(src, pc, R, up, t1, hz, org, p.first, p.second);
            };
            // La MISMA elección de triángulo y la MISMA interpolación que Jolt. No es bilineal.
            double s;
            if (zf >= xf) s = S(ix, iz) + zf * (S(ix, iz + 1) - S(ix, iz))
                              + xf * (S(ix + 1, iz + 1) - S(ix, iz + 1));
            else          s = S(ix, iz) + zf * (S(ix + 1, iz + 1) - S(ix + 1, iz))
                              + xf * (S(ix + 1, iz) - S(ix, iz));

            // ALTITUD del punto de la superficie de colisión, para poder compararla con la función.
            // Se reconstruye PLANA (`org + t1·x + hz·z + up·s`) porque así es como Jolt la coloca: el
            // desplazamiento tangencial de un heightfield plano (`x·h/R`, 3 cm a 256 m) es parte de
            // la disparidad real y tiene que salir aquí, no esconderse en una reconstrucción ideal.
            const glm::dvec3 P = org + t1 * px + hz * pz + up * s;
            outMeshH  = glm::length(P - pc) - R;
            outCellM  = spec.cell;
            return true;
        }
        return false;
    }

private:
    // Sin caché ni retícula PROPIAS: las tenía y ese era el bug. La altura la define
    // `PlanetarySystem::sampleTerrainHeight` → `ReferenceSurface`, que ya cachea por nudo de retícula.
    WorldSystem*     m_world;
    PlanetarySystem* m_planetary;
    mutable std::vector<Physics::GravBody> m_grav;   // buffer reusado (gravBodies devuelve referencia)
};

} // namespace Haruka
