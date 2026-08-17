#include "physics_engine.h"

#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdlib>   // std::getenv — Jolt es opt-in (HARUKA_JOLT=1) mientras se integra
#include <future>    // refresco ASÍNCRONO de la malla de colisión (no bloquear el frame)
#include <atomic>
#include <mutex>
#include <chrono>

#ifdef HARUKA_HAS_JOLT
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCylinderShape.h>   // troncos y ramas: cono truncado
#include <Jolt/Physics/Collision/Shape/MeshShape.h>              // props con su malla exacta
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>            // escala por instancia
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include "core/planet/terrain_lod.h"
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
// RAÍLES (docs/guides/PLAN_PUERTOS.md §4): puertas, rampas y suspensiones son MECANISMOS, no clips.
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <unordered_map>
#include <cstdarg>
#include <cstring>   // memcpy — checksum de las muestras publicadas al render
#include "core/logger.h"

// Boilerplate de Jolt (capas + filtros), aislado en este .cpp — el header NO ve Jolt.
namespace {
    void jphTrace(const char* fmt, ...) { va_list a; va_start(a, fmt); char b[512]; vsnprintf(b, sizeof(b), fmt, a); va_end(a); HARUKA_LOGI("Jolt", "%s", b); }
    namespace JLayers { static constexpr JPH::ObjectLayer NON_MOVING = 0, MOVING = 1, NUM = 2; }
    namespace JBP     { static constexpr JPH::BroadPhaseLayer NON_MOVING{0}, MOVING{1}; static constexpr JPH::uint NUM = 2; }
    class BPImpl final : public JPH::BroadPhaseLayerInterface {
    public: BPImpl() { m[JLayers::NON_MOVING] = JBP::NON_MOVING; m[JLayers::MOVING] = JBP::MOVING; }
        JPH::uint GetNumBroadPhaseLayers() const override { return JBP::NUM; }
        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return m[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "L"; }
#endif
    private: JPH::BroadPhaseLayer m[JLayers::NUM];
    };
    class OVBImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
    public: bool ShouldCollide(JPH::ObjectLayer o, JPH::BroadPhaseLayer b) const override { return o == JLayers::NON_MOVING ? (b == JBP::MOVING) : true; } };
    class OVOImpl : public JPH::ObjectLayerPairFilter {
    public: bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a == JLayers::NON_MOVING ? (b == JLayers::MOVING) : true; } };
}
#endif // HARUKA_HAS_JOLT

namespace Haruka { namespace Physics {

#ifdef HARUKA_HAS_JOLT
// Fase 1: wrapper de Jolt. Simula los cuerpos DINÁMICOS (los kinemáticos los dirige el juego). El
// terreno es de momento una ESFERA (planeta+altura); la malla local llega en la Fase 2. Gravedad
// RADIAL aplicada por cuerpo como fuerza (Jolt no la sabe curva) → a = F/m = g, independiente de la masa.
struct PhysicsEngine::JoltImpl {
    static void globalInit() {
        static bool done = false; if (done) return; done = true;
        JPH::RegisterDefaultAllocator();
        JPH::Trace = jphTrace;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    JPH::TempAllocatorImpl        temp{16 * 1024 * 1024};
    JPH::JobSystemSingleThreaded  jobs{JPH::cMaxPhysicsJobs};   // 1 hilo → determinista (DGS)
    BPImpl  bpli; OVBImpl ovbp; OVOImpl ovo;
    JPH::PhysicsSystem            system;
    std::unordered_map<RigidBody*, JPH::BodyID> map;

    // --- RAÍLES ---------------------------------------------------------------------------------
    // Un raíl es una restricción de Jolt más el poco estado que el juego necesita leer. `stall` cuenta
    // pasos SIN AVANZAR con el motor pedido: es lo que distingue "va despacio" de "está BLOQUEADO",
    // que es justo el caso que una animación resuelve mal (atravesaría lo que estorba).
    struct RailEntry {
        JPH::Ref<JPH::TwoBodyConstraint> c;
        PhysicsEngine::RailSpec  spec;
        PhysicsEngine::RailInfo  info;
        double target = 0.0;
        double lastValue = 0.0;
        int    stall = 0;
        bool   alive = false;
    };
    std::vector<RailEntry> rails;
    // MARCO LOCAL: Jolt usa float; a escala planetaria (Tierra a ~1.5e8 m en la escena) eso da ~16 m de
    // precisión → inservible. Simulamos TODO relativo a `origin` (posición del primer cuerpo) → floats
    // pequeños. El mundo exterior sigue en double. (Recentrar al alejarse mucho = refinamiento posterior.)
    glm::dvec3 origin{0.0}; bool originSet = false;
    double simTime = 0.0, lastRebuildT = -1e9;   // reloj de simulación para rate-limitar la reconstrucción
    double lastParityLogT = -1e9;                // rate-limit de la sonda de paridad suelo↔render

    // ── EL SUELO CERCANO Y EL LEJANO SON DOS CUERPOS, CON DOS CADENCIAS ─────────────────────────
    //
    // Antes eran uno solo, reconstruido cada 48 m de deriva. Eso deja el suelo que se PISA anclado
    // donde estabas hace hasta 48 m, mientras el clipmap que se DIBUJA se re-ancla cada 4 m (el paso
    // de `terrainClipFrame`): las dos superficies nacen de anclas distintas casi todo el rato, y como
    // ningún nodo sobrevive a un cambio de ancla (medido: 8 de 257 049) la disparidad no puede bajar
    // de la sagita de la celda por mucho que se afine la aritmética. Era un desfase ESTRUCTURAL, no
    // un error numérico.
    //
    // El anillo 0 (±256 m, 16 900 nodos, ~20 ms) puede seguir al anclaje sin coste apreciable; los
    // diez de fuera (213 000 nodos, ~280 ms) no, y tampoco hace falta — a >256 m no se camina.
    JPH::BodyID nearId, farId;
    bool   haveNear = false, haveFar = false, groundIsMesh = false;
    glm::dvec3 meshCenter{0.0};
    glm::dvec3 nearAnchor{0.0};      // `up` ANCLADO con el que se construyó el anillo 0 vigente
    bool       nearAnchorSet = false;
    double meshBuildElev = 0.0;      // altura del terreno en meshCenter al construir el suelo lejano
    // Compatibilidad con el camino de MALLA (servidor/tests, sin anillos) y con la esfera de respaldo:
    // esos siguen siendo UN cuerpo, el "lejano".
    bool  haveGround() const { return haveNear || haveFar; }

    // REFRESCO ASÍNCRONO de la malla de colisión. Construir el parche (muestrear el terreno + montar la
    // MeshShape) cuesta ms; hacerlo en el hilo de física daba un TIRÓN al avanzar 48 m. La Shape NO toca
    // el PhysicsSystem, así que se construye en un worker; solo el intercambio del cuerpo (barato) va en
    // el hilo de física. La malla VIEJA sigue colisionando hasta que la nueva está lista → nunca hay
    // hueco (no se cae). El campo de altura interpolado (world_system_provider) hace el worker barato.
    // Copia de la malla de colisión para el alambre de depuración. `mutable`/atómico porque
    // `buildGroundShape` es const y corre en un WORKER, mientras el render lee desde el hilo principal.
    // ⚠️ SE ARMA DESDE EL ENTORNO, no solo desde `setCollisionWireframe`. La captura ocurre DENTRO de
    // la construcción del suelo, así que si el interruptor se enciende desde el render (primer frame)
    // el suelo ya está construido y no se captura nada hasta que el jugador camina 48 m y la física
    // reconstruye. Un instrumento que solo se arma después del suceso que observa no sirve para
    // arrancar: había que caminar a ciegas para que apareciera el alambre.
    std::atomic<bool>       debugCaptureGround{
        [] { const char* e = std::getenv("HARUKA_COLLISION_WIRE"); return e && e[0] == '1'; }() };
    mutable std::mutex      debugMeshMx;
    // Dos slots: 0 = anillo CERCANO, 1 = los lejanos. Se construyen en jobs distintos y con cadencias
    // distintas, así que no pueden compartir buffer — el que llegara segundo borraría al primero y el
    // alambre enseñaría medio suelo. El lector los concatena.
    mutable std::vector<glm::dvec3> debugMeshVerts[2];
    mutable std::vector<uint32_t>   debugMeshTris[2];
    mutable glm::dvec3      debugMeshCenter{0.0};
    mutable uint64_t        debugMeshRevision = 0;

    // EL ANILLO CERCANO PUBLICADO AL RENDER (ver `PhysicsEngine::getNearGroundRing`). Se guarda
    // SIEMPRE, no solo con la depuración encendida: no es un instrumento, es la fuente del suelo que
    // se dibuja. Mismo mutex que el alambre — los dos los escribe el worker y los lee el render.
    mutable PhysicsEngine::NearGroundRing nearRing;
    mutable bool                          nearRingValid = false;

    // Reconstrucciones de medida pendientes (`HARUKA_GROUND_BENCH=N`, ver `step`). 0 = apagado.
    int groundBenchLeft = [] { const char* e = std::getenv("HARUKA_GROUND_BENCH");
                               return e ? std::atoi(e) : 0; }();

    std::future<JPH::Ref<JPH::Shape>> groundJob;      // suelo LEJANO (o el respaldo de malla/esfera)
    glm::dvec3 groundJobCenter{0.0};
    double     groundJobElev = 0.0;
    std::future<JPH::Ref<JPH::Shape>> nearJob;        // anillo 0, cadencia del ANCLAJE
    glm::dvec3 nearJobAnchor{0.0};

    // Construye la MeshShape del parche alrededor de `near` (SIN tocar el PhysicsSystem → seguro fuera
    // del hilo de física). Devuelve null si no hay malla. `terrainMesh` es thread-safe (caché con mutex).
    // §9 Fase 3: radio del parche ~200 km (200000 m); la retícula de world_system_provider es no-uniforme
    // (densa 3 m al pie, kilométrica en el borde) → la colisión cubre TODO el terreno visible como pisa.
    // El suelo como PILA DE ANILLOS de heightfield. Devuelve null si el provider no los da (servidor,
    // tests) → el llamante cae a la malla. Ver `terrain_lod.h` para por qué esto sustituye a medio
    // millón de triángulos: el árbol AABB de la MeshShape son 802-1274 ms medidos, y aquí no hay.
    JPH::Ref<JPH::Shape> buildGroundRings(IWorldProvider* w, const glm::dvec3& near,
                                          size_t firstRing, double halfExtent, int slot) const {
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        std::vector<IWorldProvider::TerrainRing> rings;
        glm::dvec3 org(0.0), rx(0.0), rup(0.0), rz(0.0);
        if (!w || !w->terrainHeightFieldRings(near, halfExtent, rings, org, rx, rup, rz, firstRing)
            || rings.empty())
            return {};
        const auto tSample = Clock::now();

        // Rotación anillo→cuerpo: los ejes del heightfield (X, altura, Z) son (rx, rup, rz). El
        // provider ya entrega la terna DEXTRÓGIRA; con una levógira el determinante sería -1 y Jolt
        // recibiría el relieve espejado.
        const glm::dmat3 M(rx, rup, rz);
        JPH::Mat44 jm = JPH::Mat44::sIdentity();
        for (int c = 0; c < 3; ++c)
            jm.SetColumn3(c, JPH::Vec3((float)M[c].x, (float)M[c].y, (float)M[c].z));
        const glm::dvec3 lp = org - origin;
        const JPH::Vec3  jp((float)lp.x, (float)lp.y, (float)lp.z);
        const JPH::Quat  jq = jm.GetQuaternion().Normalized();

        // ── LA SUPERFICIE QUE JOLT DE VERDAD COLISIONA ──────────────────────────────────────────
        //
        // Un heightfield es PLANO: Jolt coloca el nodo en `origen + X·x + arriba·muestra + Z·z`, sin
        // volver a la esfera. No es el punto con el que se generó la muestra (ese es
        // `pc + dir·(R+h)`), y la diferencia es TANGENCIAL: `x·h/R`, o sea 3 cm en el borde del
        // bloque de 256 m y menos de 1 mm a 5 m. Se reconstruye asi y no "como se genero" porque lo
        // que hay que poder mirar es lo que se PISA, no lo que se pretendia.
        auto ringPoint = [&](const IWorldProvider::TerrainRing& r, uint32_t i, uint32_t j) {
            const double x = Haruka::Planet::terrainRingNode(i, r.spec);
            const double z = Haruka::Planet::terrainRingNode(j, r.spec);
            return org + rx * x + rz * z + rup * (double)r.samples[(size_t)j * r.spec.samples + i];
        };

        // ── EL TWIST DEL QUAD: la disparidad que NINGUNA comparación de nodos puede ver ──────────
        //
        // Los cuatro nodos de un quad los comparten render y colisión EXACTAMENTE (eso ya está
        // demostrado: `clipmap_vertex_lattice`, 0 de 59 785 fuera de la retícula). Pero un quad no es
        // plano: para dibujarlo hay que partirlo en dos triángulos, y las dos diagonales posibles dan
        // superficies DISTINTAS que solo coinciden en los cuatro nodos. En el centro del quad se
        // separan por `|h00 + h11 − h10 − h01| / 4`.
        //
        // Y las dos diagonales NO tienen por qué coincidir: `clipmap.tese` declara
        // `layout(quads, equal_spacing, ccw)`, y qué diagonal usa el teselador para partir un quad
        // NO lo fija el spec de OpenGL — lo elige la implementación. Jolt sí la fija: parte por
        // `(x,y)→(x+1,y+1)` (ver `HeightFieldShape::GetSurfacePosition`).
        //
        // Por eso este número: es la disparidad MÁXIMA atribuible a que las diagonales no casen, y
        // existe a CUALQUIER distancia — a 2 m del jugador igual que a 200 m. Toda la paridad medida
        // hasta ahora es estructuralmente ciega a ella, porque compara valores EN los nodos.
        double twistNear = 0.0, twistBlock = 0.0;
        if (firstRing == 0) {
            const auto& r0 = rings[0];
            const uint32_t n = r0.spec.samples;
            for (uint32_t j = 1; j + 1 < n; ++j) for (uint32_t i = 1; i + 1 < n; ++i) {
                const float h00 = r0.samples[(size_t)j * n + i],       h10 = r0.samples[(size_t)j * n + i + 1];
                const float h01 = r0.samples[(size_t)(j + 1) * n + i], h11 = r0.samples[(size_t)(j + 1) * n + i + 1];
                const double d = std::abs((double)h00 + h11 - h10 - h01) * 0.25;
                if (!(d == d)) continue;
                if (d > twistBlock) twistBlock = d;
                const double x = Haruka::Planet::terrainRingNode(i, r0.spec);
                const double z = Haruka::Planet::terrainRingNode(j, r0.spec);
                if (std::abs(x) <= 8.0 && std::abs(z) <= 8.0 && d > twistNear) twistNear = d;
            }
        }

        // Alambre de depuración: la MISMA geometría que se le entrega a Jolt y con SU triangulación
        // (diagonal `(i,j)→(i+1,j+1)`), no una reconstrucción. Dibujarla con la otra diagonal
        // enseñaría una superficie que nadie pisa — justo el error que se está intentando ver.
        // ── MONTAR UN ANILLO EN TRIÁNGULOS ──────────────────────────────────────────────────────
        //
        // UNA sola implementación, usada por el alambre de depuración Y por lo que se le publica al
        // render. Si fueran dos, el suelo que se dibuja y el que se inspecciona podrían diferir — y
        // entonces el instrumento dejaría de poder auditar precisamente lo que hay que auditar.
        // `limit` (0 = sin límite) recorta la emisión a un cuadrado tangente. Lo necesita la copia
        // que va al RENDER: el clipmap solo puede abrir el hueco en un borde de PARCHE, y el mayor
        // que cabe en el anillo son 192 m (ver `TERRAIN_CLIP_HOLE_M`). Emitir hasta 256 dejaría la
        // banda 192-256 dibujada por los dos — dos superficies sobre el mismo suelo. El alambre de
        // depuración sí se emite entero: ahí lo que interesa es lo que COLISIONA, no lo que se pinta.
        auto emitRing = [&](const IWorldProvider::TerrainRing& r, double limit,
                            std::vector<glm::dvec3>& dv, std::vector<uint32_t>& dt) {
                const uint32_t n = r.spec.samples;
                auto solid = [&](uint32_t i, uint32_t j) {
                    if (Haruka::Planet::terrainRingNoSample(r.samples[(size_t)j * n + i])) return false;
                    if (limit <= 0.0) return true;
                    const double x = Haruka::Planet::terrainRingNode(i, r.spec);
                    const double z = Haruka::Planet::terrainRingNode(j, r.spec);
                    return std::abs(x) <= limit + 1e-9 && std::abs(z) <= limit + 1e-9;
                };
                // ⚠️ SOLO los nodos CON superficie. Emitir también los del hueco metía NaN en la lista
                // de vértices: no los referencia ningún triángulo, pero el autotest del alambre
                // recorre VÉRTICES y su media salía `nan` — un instrumento envenenado por lo que
                // dibuja. Se compacta con un remapeo en vez de indexar por `j·n+i`.
                std::vector<uint32_t> remap((size_t)n * n, UINT32_MAX);
                for (uint32_t j = 0; j < n; ++j) for (uint32_t i = 0; i < n; ++i)
                    if (solid(i, j)) {
                        remap[(size_t)j * n + i] = (uint32_t)dv.size();
                        dv.push_back(ringPoint(r, i, j));
                    }
                for (uint32_t j = 0; j + 1 < n; ++j) for (uint32_t i = 0; i + 1 < n; ++i) {
                    // Jolt tira el quad entero si un solo nodo no tiene superficie.
                    const uint32_t a = remap[(size_t)j * n + i],       b = remap[(size_t)j * n + i + 1];
                    const uint32_t c = remap[(size_t)(j + 1) * n + i], d = remap[(size_t)(j + 1) * n + i + 1];
                    if (a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX || d == UINT32_MAX)
                        continue;
                    dt.insert(dt.end(), { a, c, d,  a, d, b });   // diagonal a-d, la de Jolt
                }
        };

        // Publicar el anillo 0 al RENDER: vértices e índices ya montados (ver `getNearGroundRing`).
        if (firstRing == 0) {
            std::vector<glm::dvec3> rv; std::vector<uint32_t> rt;
            emitRing(rings[0], Haruka::Planet::TERRAIN_CLIP_HOLE_M, rv, rt);
            uint64_t ck = 1469598103934665603ull;
            for (const auto& v : rv) {
                const glm::vec3 f(v - org);      // el checksum va sobre lo que se DIBUJA (float, relativo)
                for (int c = 0; c < 3; ++c) { uint32_t b; std::memcpy(&b, &f[c], 4); ck = (ck ^ b) * 1099511628211ull; }
            }
            std::lock_guard<std::mutex> lk(debugMeshMx);
            nearRing.verts = std::move(rv);
            nearRing.tris  = std::move(rt);
            nearRing.anchor = org;
            nearRing.cell   = rings[0].spec.cell;
            // El alcance que se DIBUJA, no el del anillo: fuera de aquí manda el clipmap.
            nearRing.extent = Haruka::Planet::TERRAIN_CLIP_HOLE_M;
            ++nearRing.revision;
            nearRingValid = true;
            HARUKA_LOGI("Physics", "  anillo cercano publicado: rev %llu · %zu vert · %zu tris · "
                        "checksum %016llx", (unsigned long long)nearRing.revision,
                        nearRing.verts.size(), nearRing.tris.size() / 3, (unsigned long long)ck);
        }

        if (debugCaptureGround.load(std::memory_order_relaxed)) {
            std::vector<glm::dvec3> dv; std::vector<uint32_t> dt;
            for (const auto& r : rings) emitRing(r, 0.0, dv, dt);
            std::lock_guard<std::mutex> lk(debugMeshMx);
            debugMeshVerts[slot] = std::move(dv);
            debugMeshTris[slot]  = std::move(dt);
            debugMeshCenter = near;
            ++debugMeshRevision;
        }

        JPH::StaticCompoundShapeSettings cs;
        size_t samples = 0;
        for (size_t k = 0; k < rings.size(); ++k) {
            const auto& r = rings[k];
            JPH::HeightFieldShapeSettings hs;
            hs.mHeightSamples.resize(r.samples.size());
            for (size_t i = 0; i < r.samples.size(); ++i)
                hs.mHeightSamples[i] = Haruka::Planet::terrainRingNoSample(r.samples[i])
                    ? JPH::HeightFieldShapeConstants::cNoCollisionValue : r.samples[i];
            hs.mSampleCount = r.spec.samples;
            // ⚠️ 16 BITS POR MUESTRA, no los 8 de fábrica. Jolt cuantiza las alturas dentro de cada
            // bloque, y en los anillos lejanos el rango dentro de un bloque no lo marca el relieve
            // sino la CAÍDA POR CURVATURA (x²/2R), que a celda de 4 km son cientos de metros. Error
            // peor en el nodo, medido: celda 1024 m → 0,217 m con 8 bits y 0,0009 m con 16; celda
            // 4096 m → 1,718 m con 8 y 0,0068 m con 16. Con 8 bits el suelo lejano se desplazaría
            // más de un metro por pura cuantización, en una superficie cuyo objetivo es 0.
            // Cuesta el doble de memoria: 360 KB en total. No es un ajuste libre.
            hs.mBitsPerSample = 16;
            const double lo = Haruka::Planet::terrainRingNode(0, r.spec);
            hs.mOffset = JPH::Vec3((float)lo, 0.0f, (float)lo);
            hs.mScale  = JPH::Vec3((float)r.spec.cell, 1.0f, (float)r.spec.cell);
            JPH::ShapeSettings::ShapeResult hr = hs.Create();
            if (!hr.IsValid()) {
                HARUKA_LOGW("Physics", "anillo %zu (celda %.0f m) invalido: %s", k, r.spec.cell,
                            hr.GetError().c_str());
                return {};
            }
            samples += r.samples.size();
            cs.AddShape(jp, jq, hr.Get());
        }
        JPH::ShapeSettings::ShapeResult cres = cs.Create();
        if (!cres.IsValid()) return {};
        auto ms_of = [](Clock::time_point a, Clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        // El PRECIO del diseño, medido sobre las muestras que se acaban de entregar a Jolt. Va en la
        // misma línea que el tiempo a propósito: la ganancia y lo que cuesta no deben poder leerse
        // por separado.
        int seamK = -1; std::vector<double> seamPer;
        const double seam = IWorldProvider::terrainRingSeamStep(rings, &seamK, &seamPer);
        HARUKA_LOGI("Physics", "suelo %s: %zu anillos (alcance +-%.0f m, %zu KB de muestras)"
                    "  ·  %.0f ms = muestreo %.0f + shapes %.0f",
                    firstRing == 0 ? "CERCANO" : "lejano",
                    rings.size(), rings.back().spec.extent,
                    samples * sizeof(float) / 1024, ms_of(t0, Clock::now()),
                    ms_of(t0, tSample), ms_of(tSample, Clock::now()));
        // El escalón POR NIVEL, con la distancia a la que cae cada costura. El máximo suelto no dice
        // nada útil: crece con el cuadrado de la celda y su peor valor está a cientos de km.
        std::string seams;
        for (size_t k = 1; k < seamPer.size(); ++k) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s%.1f km:%.2f m (celda %.0f m)", k > 1 ? "  " : "",
                          rings[k].spec.hole / 1000.0, seamPer[k], rings[k].spec.cell);
            seams += buf;
        }
        if (firstRing == 0)
            HARUKA_LOGI("Physics", "  twist del quad de 4 m (disparidad en el CENTRO del quad si las "
                        "diagonales no casan): %.3f m a <8 m del jugador · %.3f m peor en el bloque",
                        twistNear, twistBlock);
        if (rings.size() > 1)
            HARUKA_LOGI("Physics", "  escalon en costuras (distancia:escalon): %s  ·  peor %.2f m (nivel %d)",
                        seams.c_str(), seam, seamK);
        return cres.Get();
    }

    JPH::Ref<JPH::Shape> buildGroundShape(IWorldProvider* w, const glm::dvec3& near) const {
        // Camino preferente: los ANILLOS (todos, en un cuerpo). La malla de abajo sigue existiendo
        // como respaldo para los providers que no los implementan (servidor, tests), no como
        // alternativa que se elija.
        if (JPH::Ref<JPH::Shape> r = buildGroundRings(w, near, 0, 200000.0, 0)) return r;

        // ── SONDA DE COSTE ──────────────────────────────────────────────────────────────────────
        //
        // Esto corre en un worker cada vez que el jugador deriva 48 m, y reconstruye la rejilla
        // ENTERA (257 049 nodos, 479 814 triángulos) aunque el jugador solo se haya movido 48 m de
        // 190 km de alcance. La pregunta de si eso hay que trocear en parches no se contesta
        // razonando: hace falta el reparto real entre MUESTREO del terreno (una llamada a
        // `sampleTerrainHeight` por nodo) y CONSTRUCCIÓN de las Shape de Jolt (árbol AABB sobre medio
        // millón de triángulos). Son dos remedios distintos y solo uno de los dos vale la pena.
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        std::vector<glm::dvec3> verts; std::vector<uint32_t> tris;
        if (!w || !w->terrainMesh(near, 200000.0, verts, tris) || tris.size() < 3 || verts.size() < 3)
            return {};
        const auto tMesh = Clock::now();
        // ── CAPTURA PARA DEPURACIÓN ─────────────────────────────────────────────────────────────
        //
        // Se guarda una copia de la MISMA geometría que se le entrega a Jolt, no una reconstrucción:
        // el render la dibuja en alambre encima del terreno y así "se ve disparidad" pasa a ser algo
        // que se mira. Reconstruirla en el render no valdría — parte de lo que hay que poder ver es si
        // la malla que Jolt TIENE se ha quedado vieja respecto a lo que se dibuja.
        //
        // Solo se copia si alguien la ha pedido: el parche de 200 km son ~88 k vértices (2 MB), y
        // pagarlos por si acaso en cada reconstrucción no tiene sentido.
        if (debugCaptureGround.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(debugMeshMx);
            debugMeshVerts[1] = verts;
            debugMeshTris[1]  = tris;
            debugMeshVerts[0].clear();
            debugMeshTris[0].clear();
            debugMeshCenter = near;
            ++debugMeshRevision;
        }
        JPH::VertexList jv; jv.reserve(verts.size());
        for (const auto& p : verts) { const glm::dvec3 lp = p - origin;
            jv.push_back(JPH::Float3((float)lp.x, (float)lp.y, (float)lp.z)); }
        JPH::IndexedTriangleList jt; jt.reserve(tris.size() / 3);
        for (size_t i = 0; i + 2 < tris.size(); i += 3)
            jt.push_back(JPH::IndexedTriangle(tris[i], tris[i + 1], tris[i + 2], 0));
        JPH::MeshShapeSettings ms(jv, jt);
        JPH::ShapeSettings::ShapeResult res = ms.Create();
        if (!res.IsValid()) return {};
        const auto tShape = Clock::now();

        // ── EL BLOQUE CERCANO COMO HEIGHTFIELD ──────────────────────────────────────────────────
        //
        // La malla de arriba viene con un HUECO donde va esto (`terrainHeightFieldCovers`): las dos
        // geometrías se tocan pero no se solapan, porque solapadas Jolt generaría contactos DOBLES
        // sobre el mismo suelo y el personaje rebotaría en la frontera.
        //
        // Es el mismo terreno, misma función, mismo marco anclado y los mismos vértices que tendría la
        // malla — pero `HeightFieldShape` no guarda posiciones ni índices ni construye árbol AABB sobre
        // triángulos: solo las alturas cuantizadas sobre una rejilla implícita.
        std::vector<float> hf; uint32_t hfN = 0; double hfCell = 0.0;
        glm::dvec3 hfO(0.0), hfX(0.0), hfUp(0.0), hfZ(0.0);
        const bool haveHF = w->terrainHeightField(near, hf, hfN, hfCell, hfO, hfX, hfUp, hfZ);
        const auto tHF = Clock::now();
        auto ms_of = [](Clock::time_point a, Clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        if (haveHF && hfN >= 2) {
            JPH::HeightFieldShapeSettings hs;
            hs.mHeightSamples.assign(hf.begin(), hf.end());
            hs.mSampleCount = hfN;
            // La superficie de Jolt es `mOffset + mScale·(x, muestra, y)` con x,y enteros: el offset
            // lleva la esquina a su coordenada tangente y la escala convierte índice→metros.
            hs.mOffset = JPH::Vec3((float)Haruka::Planet::TERRAIN_HF_LO, 0.0f,
                                   (float)Haruka::Planet::TERRAIN_HF_LO);
            hs.mScale  = JPH::Vec3((float)hfCell, 1.0f, (float)hfCell);
            JPH::ShapeSettings::ShapeResult hres = hs.Create();
            if (hres.IsValid()) {
                // Rotación heightfield→cuerpo: sus ejes (X, altura, Z) son (hfX, hfUp, hfZ). El provider
                // ya devuelve la terna DEXTRÓGIRA (por eso su Z es `-t2`); si no lo fuera, la matriz
                // tendría determinante -1 y Jolt recibiría el relieve espejado.
                const glm::dmat3 M(hfX, hfUp, hfZ);          // columnas
                JPH::Mat44 jm = JPH::Mat44::sIdentity();
                for (int c = 0; c < 3; ++c)
                    jm.SetColumn3(c, JPH::Vec3((float)M[c].x, (float)M[c].y, (float)M[c].z));
                const glm::dvec3 lp = hfO - origin;          // ancla en coordenadas del cuerpo
                JPH::StaticCompoundShapeSettings cs;
                cs.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), res.Get());
                cs.AddShape(JPH::Vec3((float)lp.x, (float)lp.y, (float)lp.z),
                            jm.GetQuaternion().Normalized(), hres.Get());
                JPH::ShapeSettings::ShapeResult cres = cs.Create();
                if (cres.IsValid()) {
                    HARUKA_LOGI("Physics", "suelo: malla %zu tris (con hueco) + heightfield %ux%u "
                                "(%zu KB de muestras)  ·  %.0f ms = muestreo %.0f + MeshShape %.0f "
                                "+ heightfield %.0f + compound %.0f",
                                tris.size() / 3, hfN, hfN, hf.size() * sizeof(float) / 1024,
                                ms_of(t0, Clock::now()), ms_of(t0, tMesh), ms_of(tMesh, tShape),
                                ms_of(tShape, tHF), ms_of(tHF, Clock::now()));
                    return cres.Get();
                }
            }
        }
        return res.Get();
    }

    // Cambia un cuerpo de suelo por `shape` (en el hilo de física). El viejo se quita justo aquí, no
    // antes: hasta este instante seguía colisionando, así que nunca hay un frame sin suelo.
    /**
     * @brief Cambia la FORMA del suelo conservando el CUERPO.
     *
     * ⚠️ ANTES DESTRUÍA Y RECREABA EL CUERPO (`RemoveBody` + `DestroyBody` + `CreateAndAddBody`), y
     * eso le daba un `BodyID` NUEVO en cada reconstrucción. El `CharacterVirtual` arrastra sus
     * contactos entre updates identificados por `BodyID` (`mActiveContacts`, `mGroundBodyID`): al
     * cambiar el identificador, el personaje deja de reconocer el suelo que ya pisaba y ese update
     * empieza de cero. Un frame sin el apoyo que tenía, EN CUESTA, es resbalar hacia abajo — que es
     * exactamente el "pelea al subir por una ladera" reportado.
     *
     * Y ocurría a menudo: el anillo cercano se reconstruye cada vez que salta el anclaje, o sea cada
     * pocos metros caminando (y, antes de la banda muerta de `terrainAnchorShouldJump`, en CADA
     * FRAME estando quieto sobre un borde de celda).
     *
     * `SetShape` mantiene el `BodyID`, invalida la caché de contactos de ese cuerpo y avisa al
     * broadphase del nuevo AABB — sin ventana en la que el suelo no exista.
     */
    void swapBody(JPH::BodyID& id, bool& have, JPH::Ref<JPH::Shape> shape) {
        if (!shape) return;
        JPH::BodyInterface& bi = system.GetBodyInterface();
        if (have) {
            bi.SetShape(id, shape, /*updateMassProperties*/false, JPH::EActivation::DontActivate);
            return;
        }
        JPH::BodyCreationSettings s(shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static, JLayers::NON_MOVING);
        id = bi.CreateAndAddBody(s, JPH::EActivation::DontActivate);
        have = true;
    }

    // El `up` ANCLADO en `p`: la retícula del clipmap salta en escalones discretos del tamaño del
    // quad, y entre salto y salto los vértices están quietos en coordenadas del mundo. Comparar este
    // vector es la forma exacta de preguntar "¿se ha movido la retícula que dibuja el render?", que
    // es lo que tiene que disparar la reconstrucción del anillo cercano — no la distancia recorrida.
    static glm::dvec3 anchorOf(IWorldProvider* w, const glm::dvec3& p) {
        glm::dvec3 up(0.0, 1.0, 0.0), t1, t2;
        if (w && w->hasActivePlanet())
            Haruka::Planet::terrainClipFrame(p, w->activePlanetCenter(), up, t1, t2,
                                             w->activePlanetRadius());
        return up;
    }

    JoltImpl() {
        globalInit();
        system.Init(4096, 0, 16384, 16384, bpli, ovbp, ovo);
        system.SetGravity(JPH::Vec3::sZero());   // la gravedad radial la aplicamos nosotros
    }
    // Construye/REFRESCA el suelo alrededor de `near`. La MALLA (si el provider la da) se reconstruye al
    // moverse el jugador (sigue la región); la esfera (fallback: servidor/test) es fija → una vez basta.
    void rebuildGround(IWorldProvider* w, const glm::dvec3& near) {
        if (!w || !w->hasActivePlanet()) return;
        // ⚠️ LA MISMA GEOMETRÍA QUE EL REFRESCO ASÍNCRONO, no una segunda versión. Aquí había una copia
        // del montaje de la MeshShape, así que el suelo del SPAWN y el de después de caminar 48 m eran
        // dos códigos distintos que había que mantener a la vez — y el del spawn se quedó sin los
        // anillos y sin el heightfield cercano. Ahora los dos pasan por `buildGroundShape`.
        // Camino de ANILLOS: dos cuerpos, dos cadencias (ver los miembros `nearId`/`farId`).
        JPH::Ref<JPH::Shape> nearShape =
            buildGroundRings(w, near, 0, Haruka::Planet::TERRAIN_COLLIDE_UNIFORM_M, 0);
        if (nearShape) {
            JPH::Ref<JPH::Shape> farShape = buildGroundRings(w, near, 1, 200000.0, 1);
            swapBody(nearId, haveNear, nearShape);
            swapBody(farId,  haveFar,  farShape);
            groundIsMesh = true; meshCenter = near; meshBuildElev = w->terrainHeightAt(near);
            nearAnchor = anchorOf(w, near); nearAnchorSet = true;
            return;
        }
        // Sin anillos: el respaldo (malla o esfera) es UN cuerpo, el "lejano".
        if (JPH::Ref<JPH::Shape> shape = buildGroundShape(w, near)) {
            swapBody(farId, haveFar, shape);
            groundIsMesh = true; meshCenter = near; meshBuildElev = w->terrainHeightAt(near);
            return;
        }
        JPH::BodyInterface& bi = system.GetBodyInterface();
        if (haveGround() && !groundIsMesh) return;   // sigue en modo esfera fija → nada que hacer
        if (haveFar) { bi.RemoveBody(farId); bi.DestroyBody(farId); haveFar = false; }
        // Fallback: esfera (planeta + altura de un punto). Fija.
        const glm::dvec3 c = w->activePlanetCenter();
        const double R = w->activePlanetRadius();
        const double h = w->terrainHeightAt(c + glm::dvec3(R, 0, 0));
        const glm::dvec3 lc = c - origin;
        JPH::BodyCreationSettings s(new JPH::SphereShape((float)(R + h)),
            JPH::RVec3((float)lc.x, (float)lc.y, (float)lc.z), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static, JLayers::NON_MOVING);
        farId = bi.CreateAndAddBody(s, JPH::EActivation::DontActivate);
        haveFar = true; groundIsMesh = false;
    }
    // --- CONTROLADOR DE PERSONAJE (Jolt CharacterVirtual) ---
    // Un personaje NO es un rígido dinámico: quiere subir escalones, no resbalar por pendientes suaves,
    // pararse en seco y saber si pisa suelo. Emular eso a mano sobre una esfera rígida sale mal pieza a
    // pieza (rodaba, se enterraba, no saltaba, deslizaba). `CharacterVirtual` lo trae de fábrica.
    JPH::Ref<JPH::CharacterVirtual> charCtrl;
    RigidBody*                      charBody = nullptr;

    void addCharacter(RigidBody* b) {
        charBody = b;
        if (!originSet) { origin = b->position; originSet = true; }
        JPH::Ref<JPH::CharacterVirtualSettings> cs = new JPH::CharacterVirtualSettings();
        // ESFERA del mismo radio que el cuerpo: así `position` sigue siendo el CENTRO y el juego puede
        // seguir sacando el pie como `centro − arriba·radio`, sin tocar su matemática.
        cs->mShape         = new JPH::SphereShape((float)b->radius);
        cs->mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);   // más empinado que esto = resbalas
        const glm::dvec3 lp = b->position - origin;
        charCtrl = new JPH::CharacterVirtual(cs, JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z),
                                             JPH::Quat::sIdentity(), &system);
    }

    // Quitar un cuerpo TAMBIÉN de Jolt. Si no, su BodyID sigue vivo (colisionador fantasma que nadie ve)
    // y la clave `RigidBody*` del mapa queda COLGANDO: si un cuerpo nuevo se aloja en esa misma dirección
    // coincide con la entrada vieja y acaba movido por el cuerpo equivocado.
    void removeBody(RigidBody* b) {
        if (b == charBody) { charCtrl = nullptr; charBody = nullptr; return; }
        auto it = map.find(b);
        if (it == map.end()) return;
        JPH::BodyInterface& bi = system.GetBodyInterface();
        bi.RemoveBody(it->second);
        bi.DestroyBody(it->second);
        map.erase(it);
    }

    void addBody(RigidBody* b) {
        if (b->isCharacter) { addCharacter(b); return; }
        if (!originSet) { origin = b->position; originSet = true; }   // el 1er cuerpo fija el marco local
        JPH::Ref<JPH::Shape> shape;
        if (b->shape == RigidBody::Shape::Compound && !b->compound.empty()) {
            // Una caja por pieza. El hueco del pasillo aparece solo: es donde no hay hija.
            JPH::StaticCompoundShapeSettings cs;
            for (const auto& c : b->compound) {
                cs.AddShape(JPH::Vec3((float)c.center.x, (float)c.center.y, (float)c.center.z),
                            JPH::Quat((float)c.rotation.x, (float)c.rotation.y,
                                      (float)c.rotation.z, (float)c.rotation.w),
                            new JPH::BoxShape(JPH::Vec3((float)c.halfExtents.x,
                                                        (float)c.halfExtents.y,
                                                        (float)c.halfExtents.z)));
            }
            auto res = cs.Create();
            if (res.IsValid()) shape = res.Get();
        }
        if (!shape) {
            shape = (b->shape == RigidBody::Shape::Box || b->shape == RigidBody::Shape::Compound)
                ? (JPH::Shape*)new JPH::BoxShape(JPH::Vec3((float)b->halfExtents.x, (float)b->halfExtents.y, (float)b->halfExtents.z))
                : (JPH::Shape*)new JPH::SphereShape((float)b->radius);
        }
        const glm::dvec3 lp = b->position - origin;                   // posición en el marco local
        JPH::BodyCreationSettings s(shape,
            JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z),
            JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, JLayers::MOVING);
        s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        s.mMassPropertiesOverride.mMass = (float)b->mass;
        // Un PERSONAJE no rueda: se le permite trasladar pero no girar. Si no, la esfera acumula
        // velocidad angular y el contacto la convierte en avance → sigue deslizando aunque se le ponga
        // la velocidad lineal a cero (justo el "no tiene fricción" que se veía).
        if (b->lockRotation)
            s.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY
                           | JPH::EAllowedDOFs::TranslationZ;
        JPH::BodyInterface& bi = system.GetBodyInterface();
        JPH::BodyID id = bi.CreateAndAddBody(s, JPH::EActivation::Activate);
        bi.SetLinearVelocity(id, JPH::Vec3((float)b->velocity.x, (float)b->velocity.y, (float)b->velocity.z));
        map[b] = id;
    }
    // --- Mundo ESTÁTICO en Jolt: props colocados, piezas de construcción y muros de edificio. ---
    // Sin esto, Jolt solo conoce el terreno: un cuerpo dinámico (el jugador) ATRAVIESA todo lo demás,
    // porque esas cajas solo existían para el resolutor a mano. Se reconstruyen cuando cambia la lista
    // (`version`), no cada frame: son estáticas y cambiarlas es raro (colocar/romper algo).
    std::vector<JPH::BodyID> staticIds;
    uint64_t staticVersion = ~0ull;

    /// Mallas de colisión de props, UNA por prototipo/parte y compartidas por todas sus instancias.
    /// El `MeshShape` monta un árbol AABB (lo caro); por instancia sería inviable.
    std::vector<JPH::Ref<JPH::Shape>> propMeshShapes;

    int addMeshShape(const float* verts, size_t vertCount, const uint32_t* idx, size_t idxCount) {
        if (!verts || !idx || vertCount < 3 || idxCount < 3) return -1;
        JPH::VertexList vl;
        vl.reserve(vertCount);
        for (size_t i = 0; i < vertCount; ++i)
            vl.push_back(JPH::Float3(verts[i * 3 + 0], verts[i * 3 + 1], verts[i * 3 + 2]));
        JPH::IndexedTriangleList tl;
        tl.reserve(idxCount / 3);
        for (size_t t = 0; t + 2 < idxCount; t += 3) {
            const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
            if (a >= vertCount || b >= vertCount || c >= vertCount) continue;
            if (a == b || b == c || a == c) continue;            // degenerado: Jolt lo rechaza
            tl.push_back(JPH::IndexedTriangle(a, b, c, 0));
        }
        if (tl.empty()) return -1;
        JPH::MeshShapeSettings ms(vl, tl);
        ms.SetEmbedded();
        JPH::ShapeSettings::ShapeResult r = ms.Create();
        if (r.HasError()) {
            HARUKA_LOGE("Physics", "malla de prop rechazada por Jolt: %s", r.GetError().c_str());
            return -1;
        }
        propMeshShapes.push_back(r.Get());
        return (int)propMeshShapes.size() - 1;
    }

    static bool finite3(const glm::dvec3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }
    void syncStatics(PhysicsEngine* eng) {
        const uint64_t version = eng->staticsVersion();
        if (version == staticVersion || !originSet) return;
        staticVersion = version;
        JPH::BodyInterface& bi = system.GetBodyInterface();
        for (JPH::BodyID id : staticIds) { bi.RemoveBody(id); bi.DestroyBody(id); }
        staticIds.clear();

        auto addBox = [&](const glm::dvec3& center, const glm::dvec3& he, const glm::dquat& q0) {
            if (he.x < 1e-4 || he.y < 1e-4 || he.z < 1e-4) return;   // caja degenerada → Jolt la rechaza
            const glm::dvec3 lp = center - origin;                   // al marco local (Jolt va en float)
            // Jolt ASERTA (SIGTRAP) si el cuaternión no está normalizado o la posición no es finita. Las
            // rotaciones vienen de matrices hechas con productos vectoriales que, tras pasar por float,
            // se salen de la tolerancia de Jolt → NORMALIZAR aquí y descartar lo no-finito. Es la frontera
            // con Jolt: se saneia una vez, no se confía en cada productor.
            const double qlen = glm::length(q0);
            if (!finite3(lp) || !finite3(he) || qlen < 1e-9 || !std::isfinite(qlen)) return;
            const glm::dquat q = q0 / qlen;                          // normalizado
            JPH::BodyCreationSettings s(
                new JPH::BoxShape(JPH::Vec3((float)he.x, (float)he.y, (float)he.z)),
                JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z),
                JPH::Quat((float)q.x, (float)q.y, (float)q.z, (float)q.w).Normalized(),
                JPH::EMotionType::Static, JLayers::NON_MOVING);
            staticIds.push_back(bi.CreateAndAddBody(s, JPH::EActivation::DontActivate));
        };

        // TODO el mundo estático, de una: objetos colocados + piezas/muros (placed), RECURSOS (árboles/
        // rocas, prop) y colisionadores de escena (staticBox, AABB → OBB sin girar). Si alguna faltara,
        // el jugador —que ahora es un cuerpo de Jolt— la ATRAVESARÍA (colisión "sin terminar").
        const auto& placed = eng->getPlacedOBBs();
        const auto& props  = eng->getPropOBBs();
        const auto& boxes  = eng->getStaticBoxes();
        staticIds.reserve(placed.size() + props.size() + boxes.size());
        for (const StaticOBB& o : placed) addBox(o.center, o.halfExtents, glm::quat_cast(o.rot));
        for (const StaticOBB& o : props)  addBox(o.center, o.halfExtents, glm::quat_cast(o.rot));

        // CONOS TRUNCADOS: la forma real de troncos y ramas. Con cajas, las esquinas sobresalen de
        // la madera visible y se choca con aire.
        auto addCone = [&](const StaticCone& c) {
            // ⚠️ Jolt exige que el radio de convexidad quepa: con un radio menor que el suyo por
            // defecto (0,05 m) la forma es inválida. Las ramas finas caen por debajo, así que el
            // radio de convexidad se acota al propio cono en vez de descartar la rama.
            const double rMin = std::min(c.rTop, c.rBottom);
            const double rMax = std::max(c.rTop, c.rBottom);
            if (c.halfHeight < 1e-3 || rMax < 1e-3) return;
            const glm::dvec3 lp = c.center - origin;
            const glm::dquat q0 = glm::quat_cast(c.rot);
            const double qlen = glm::length(q0);
            if (!finite3(lp) || qlen < 1e-9 || !std::isfinite(qlen)) return;
            const glm::dquat q = q0 / qlen;
            const float conv = (float)std::min(0.05, std::max(1e-3, rMin * 0.9));
            JPH::TaperedCylinderShapeSettings cs((float)c.halfHeight, (float)c.rTop,
                                                 (float)c.rBottom, conv);
            JPH::ShapeSettings::ShapeResult res = cs.Create();
            if (res.HasError()) {
                // ⚠️ Sin este aviso el fallo es MUDO y los árboles se quedan sin collider: peor que
                // con cajas, y sin nada en el log que lo diga.
                static int s_warned = 0;
                if (s_warned++ < 3)
                    HARUKA_LOGE("Physics", "cono de prop rechazado por Jolt: hh=%.3f rT=%.3f rB=%.3f conv=%.3f -> %s",
                                c.halfHeight, c.rTop, c.rBottom, (double)conv, res.GetError().c_str());
                return;
            }
            JPH::BodyCreationSettings s(res.Get(),
                JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z),
                JPH::Quat((float)q.x, (float)q.y, (float)q.z, (float)q.w).Normalized(),
                JPH::EMotionType::Static, JLayers::NON_MOVING);
            staticIds.push_back(bi.CreateAndAddBody(s, JPH::EActivation::DontActivate));
        };
        const size_t idsBeforeCones = staticIds.size();
        for (const StaticCone& c : eng->getPropCones()) addCone(c);

        // MALLAS EXACTAS: la geometría real del prop. La forma se comparte; lo único por instancia
        // es la transformación y la escala (`ScaledShape`, que no reconstruye el árbol AABB).
        size_t meshMade = 0;
        for (const StaticMeshInstance& mi : eng->getPropMeshInstances()) {
            if (mi.shapeId < 0 || mi.shapeId >= (int)propMeshShapes.size()) continue;
            const glm::dvec3 lp = mi.center - origin;
            const glm::dquat q0 = glm::quat_cast(mi.rot);
            const double qlen = glm::length(q0);
            if (!finite3(lp) || qlen < 1e-9 || !std::isfinite(qlen) || mi.scale < 1e-4) continue;
            const glm::dquat q = q0 / qlen;
            JPH::Ref<JPH::Shape> shape = propMeshShapes[(size_t)mi.shapeId];
            if (std::abs(mi.scale - 1.0) > 1e-4) {
                JPH::Ref<JPH::Shape> sc = new JPH::ScaledShape(
                    shape, JPH::Vec3((float)mi.scale, (float)mi.scale, (float)mi.scale));
                shape = sc;
            }
            JPH::BodyCreationSettings s(shape,
                JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z),
                JPH::Quat((float)q.x, (float)q.y, (float)q.z, (float)q.w).Normalized(),
                JPH::EMotionType::Static, JLayers::NON_MOVING);
            staticIds.push_back(bi.CreateAndAddBody(s, JPH::EActivation::DontActivate));
            ++meshMade;
        }
        {
            static size_t s_last = ~(size_t)0;
            if (meshMade != s_last) {
                s_last = meshMade;
                HARUKA_LOGD("Physics", "mallas de props -> Jolt: %zu de %zu",
                            meshMade, eng->getPropMeshInstances().size());
            }
        }


        {
            static size_t s_lastReport = ~(size_t)0;
            const size_t made = staticIds.size() - idsBeforeCones;
            const size_t want = eng->getPropCones().size();
            if (made != s_lastReport) {
                s_lastReport = made;
                HARUKA_LOGD("Physics", "conos de props -> Jolt: %zu de %zu creados", made, want);
            }
        }
        for (const StaticBox& b : boxes)
            addBox((b.bmin + b.bmax) * 0.5, (b.bmax - b.bmin) * 0.5, glm::dquat(1, 0, 0, 0));
    }

    // Gravedad en un punto: SUMA de todos los cuerpos masivos, con su masa real.
    //
    // ⚠️ Esto era `9.81·R²/r²` hacia el planeta activo, con este comentario: «`gravBodies()` solo trae
    // los cuerpos que EMITEN luz (estrellas) → el planeta sobre el que andas NO está ahí». El parche
    // era correcto sobre un dato que no lo era, y tenía dos costes: TODO cuerpo tenía la gravedad de
    // la Tierra (una luna te frenaba como un planeta) y la regla de gravedad existía por duplicado —
    // aquí y en el camino de los cuerpos dinámicos. Arreglada la lista (ver world_system_provider.h),
    // los dos parches se van y queda una sola función.
    glm::dvec3 gravityAtPoint(IWorldProvider* w, const glm::dvec3& pos) const {
        if (!w) return glm::dvec3(0.0);
        const glm::dvec3 g = Haruka::Planet::gravityAt(w->gravBodies(), pos);
        // Red de seguridad: si la escena aún no ha registrado ningún cuerpo masivo (carga a medias),
        // sin esto el jugador se queda sin "arriba" y el controlador de personaje pierde su eje.
        if (glm::dot(g, g) > 1e-18 || !w->hasActivePlanet()) return g;
        const glm::dvec3 toC = w->activePlanetCenter() - pos;
        const double r = glm::length(toC);
        if (r < 1e-6) return glm::dvec3(0.0);
        const double R = w->activePlanetRadius();
        return (toC / r) * (Haruka::Planet::surfaceGravity(R) * (R * R) / (r * r));
    }

    void stepCharacter(double dt, PhysicsEngine* eng, IWorldProvider* w) {
        if (!charCtrl || !charBody) return;
        RigidBody* b = charBody;
        const glm::dvec3 lp = b->position - origin;

        // VOLANDO/NADANDO manda el juego: se lleva el controlador a su posición y no se simula.
        if (b->isKinematic) {
            if (finite3(b->position))
                charCtrl->SetPosition(JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z));
            if (finite3(b->velocity))
                charCtrl->SetLinearVelocity(JPH::Vec3((float)b->velocity.x, (float)b->velocity.y, (float)b->velocity.z));
            b->onGround = false;
            return;
        }

        const glm::dvec3 gvec = gravityAtPoint(w, b->position);
        const double gl = glm::length(gvec);
        const glm::dvec3 up = (gl > 1e-9) ? -gvec / gl : glm::dvec3(0.0, 1.0, 0.0);
        charCtrl->SetUp(JPH::Vec3((float)up.x, (float)up.y, (float)up.z));   // "arriba" es RADIAL y cambia al moverte
        if (finite3(b->position))
            charCtrl->SetPosition(JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z));
        // La GRAVEDAD la integra el LLAMADOR: `ExtendedUpdate` no la aplica a la velocidad (su parámetro
        // `inGravity` solo sirve para empujar hacia abajo cuando estás encima de algo). Sin esto el
        // personaje anda y choca pero NUNCA CAE.
        if (finite3(b->velocity)) {
            const glm::dvec3 vel = b->velocity + gvec * dt;
            charCtrl->SetLinearVelocity(JPH::Vec3((float)vel.x, (float)vel.y, (float)vel.z));
        }

        // Escalones y "pegado al suelo" en el marco RADIAL (por defecto Jolt los da en ±Y del mundo,
        // que en una esfera solo vale en el polo).
        JPH::CharacterVirtual::ExtendedUpdateSettings us;
        us.mStickToFloorStepDown = JPH::Vec3((float)(-up.x * 0.5), (float)(-up.y * 0.5), (float)(-up.z * 0.5));
        us.mWalkStairsStepUp     = JPH::Vec3((float)( up.x * 0.4), (float)( up.y * 0.4), (float)( up.z * 0.4));

        charCtrl->ExtendedUpdate((float)dt,
            JPH::Vec3((float)gvec.x, (float)gvec.y, (float)gvec.z), us,
            system.GetDefaultBroadPhaseLayerFilter(JLayers::MOVING),
            system.GetDefaultLayerFilter(JLayers::MOVING),
            JPH::BodyFilter{}, JPH::ShapeFilter{}, temp);

        // ── SONDA DE CONTACTOS ──────────────────────────────────────────────────────────────────
        //
        // Convierte "no colisiona" en un número. `CharacterVirtual` lleva sus contactos activos: si
        // al caminar contra un árbol esto sigue diciendo 1 contacto (el suelo), el collider NO se
        // está probando; si sube a 2, se toca y el problema es de respuesta (deslizamiento), no de
        // detección. Es la medida que separa los dos casos, y sin ella solo se puede opinar.
        // Se reporta ~1 vez por segundo para no ensuciar el log.
        {
            static double s_lastLog = -1e9;
            if (simTime - s_lastLog > 1.0) {
                s_lastLog = simTime;
                const auto& contacts = charCtrl->GetActiveContacts();
                int nGround = 0, nOther = 0;
                for (const auto& c : contacts) {
                    // Contacto "de suelo": su normal apunta contra la gravedad local.
                    const glm::dvec3 n(c.mContactNormal.GetX(), c.mContactNormal.GetY(),
                                       c.mContactNormal.GetZ());
                    if (glm::dot(n, up) > 0.5) ++nGround; else ++nOther;
                }
                HARUKA_LOGDIAG("CharContacts", "contactos=%zu (suelo=%d, laterales=%d) · onGround=%d",
                            contacts.size(), nGround, nOther,
                            charCtrl->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround);
            }
        }

        const JPH::RVec3 p = charCtrl->GetPosition();
        const JPH::Vec3  v = charCtrl->GetLinearVelocity();
        b->position = origin + glm::dvec3(p.GetX(), p.GetY(), p.GetZ());
        b->velocity = glm::dvec3(v.GetX(), v.GetY(), v.GetZ());
        b->onGround = (charCtrl->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround);
    }

    // --- RAÍLES: crear, pedir y leer ------------------------------------------------------------
    // ⚠️ El ancla y el eje llegan en MUNDO (double) y aquí se rebajan al MARCO LOCAL (`origin`), que es
    // donde vive Jolt. Sin esto, un raíl en un planeta a 1.5e8 m se crearía con coordenadas que el
    // float no representa — el mismo motivo por el que todo lo demás se simula relativo a `origin`.
    int addRail(RigidBody* a, RigidBody* b, const PhysicsEngine::RailSpec& spec) {
        auto ia = map.find(a), ib = map.find(b);
        if (ia == map.end() || ib == map.end()) return -1;

        const glm::dvec3 la = spec.anchor - origin;
        glm::dvec3 ax = spec.axis;
        const double axLen = glm::length(ax);
        if (axLen < 1e-9) return -1;
        ax /= axLen;
        // ⚠️ EL CERO DEL ÁNGULO ES LA POSTURA DE MONTAJE. Jolt necesita una normal perpendicular al
        // eje para fijar dónde está el 0, y si se elige una cualquiera, la hoja NACE a un ángulo
        // arbitrario de su propio cero: con la puerta montada a 90° de esa normal, unos límites de
        // ±90° dejaban de significar nada (medido: pedía 90 y leía 123.8). Se toma la dirección
        // ancla→hoja, que es lo que el diseñador ve al colocarla: "cerrada" es 0 y los límites son
        // relativos al reposo, como dice `game/ports/port.h`.
        glm::dvec3 n = b->position - spec.anchor;
        n -= ax * glm::dot(n, ax);                       // perpendicular al eje
        if (glm::length(n) < 1e-6)                       // hoja SOBRE el eje: no hay referencia
            n = std::abs(ax.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
        n = glm::normalize(n - ax * glm::dot(n, ax));

        const JPH::Vec3 jp((float)la.x, (float)la.y, (float)la.z);
        const JPH::Vec3 ja((float)ax.x, (float)ax.y, (float)ax.z);
        const JPH::Vec3 jn((float)n.x,  (float)n.y,  (float)n.z);

        // ⚠️ DOS `BodyLockWrite` sueltos NO valen: Jolt asserta por orden de bloqueo (es su defensa
        // contra interbloqueos). `BodyLockMultiWrite` ordena los ids él mismo.
        const JPH::BodyID ids[2] = { ia->second, ib->second };
        JPH::BodyLockMultiWrite lock(system.GetBodyLockInterface(), ids, 2);
        JPH::Body* jba = lock.GetBody(0);
        JPH::Body* jbb = lock.GetBody(1);
        if (!jba || !jbb) return -1;

        RailEntry e;
        e.spec = spec;
        if (spec.dof == PhysicsEngine::RailSpec::Dof::Hinge) {
            JPH::HingeConstraintSettings st;
            st.mPoint1 = st.mPoint2 = JPH::RVec3(jp);
            st.mHingeAxis1 = st.mHingeAxis2 = ja;
            st.mNormalAxis1 = st.mNormalAxis2 = jn;
            st.mLimitsMin = (float)glm::radians(std::min(spec.limits.x, spec.limits.y));
            st.mLimitsMax = (float)glm::radians(std::max(spec.limits.x, spec.limits.y));
            if (spec.drive == PhysicsEngine::RailSpec::Drive::Motor) {
                st.mMotorSettings.SetTorqueLimit((float)spec.motorForce);
                // ⚠️ El muelle por defecto de Jolt (2 Hz) es de SUSPENSIÓN, no de mecanismo: una
                // rampa tardaba decenas de segundos en acercarse al ángulo pedido y se quedaba corta.
                // Un actuador de puerta es RÍGIDO — lo que lo frena tiene que ser la carga y el tope
                // de par, no la blandura del muelle.
                st.mMotorSettings.mSpringSettings.mFrequency = 20.0f;
                st.mMotorSettings.mSpringSettings.mDamping   = 1.0f;
            }
            e.c = st.Create(*jba, *jbb);
        } else {
            JPH::SliderConstraintSettings st;
            st.mPoint1 = st.mPoint2 = JPH::RVec3(jp);
            st.SetSliderAxis(ja);
            st.mLimitsMin = (float)std::min(spec.limits.x, spec.limits.y);
            st.mLimitsMax = (float)std::max(spec.limits.x, spec.limits.y);
            if (spec.drive == PhysicsEngine::RailSpec::Drive::Motor) {
                st.mMotorSettings.SetForceLimit((float)spec.motorForce);
                st.mMotorSettings.mSpringSettings.mFrequency = 20.0f;   // ver la nota del Hinge
                st.mMotorSettings.mSpringSettings.mDamping   = 1.0f;
            }
            e.c = st.Create(*jba, *jbb);
        }
        if (!e.c) return -1;
        system.AddConstraint(e.c);
        e.alive = true;
        rails.push_back(e);
        return (int)rails.size() - 1;
    }

    void railTarget(int id, double t) {
        if (id < 0 || id >= (int)rails.size()) return;
        RailEntry& e = rails[(size_t)id];
        if (!e.alive || e.info.broken) return;
        const double lo = std::min(e.spec.limits.x, e.spec.limits.y);
        const double hi = std::max(e.spec.limits.x, e.spec.limits.y);
        e.target = std::clamp(t, lo, hi);
        e.stall = 0; e.lastValue = e.info.value; e.info.blocked = false;
        // Solo un motor OBEDECE una posición pedida. Manual lo empuja un personaje y Spring tira solo.
        if (e.spec.drive != PhysicsEngine::RailSpec::Drive::Motor) return;
        if (e.spec.dof == PhysicsEngine::RailSpec::Dof::Hinge) {
            auto* h = static_cast<JPH::HingeConstraint*>(e.c.GetPtr());
            h->SetMotorState(JPH::EMotorState::Position);
            h->SetTargetAngle((float)glm::radians(e.target));
        } else {
            auto* sl = static_cast<JPH::SliderConstraint*>(e.c.GetPtr());
            sl->SetMotorState(JPH::EMotorState::Position);
            sl->SetTargetPosition((float)e.target);
        }
    }

    void updateRails(double dt) {
        if (rails.empty() || dt <= 0.0) return;
        for (auto& e : rails) {
            if (!e.alive) continue;
            double force = 0.0;
            if (e.spec.dof == PhysicsEngine::RailSpec::Dof::Hinge) {
                auto* h = static_cast<JPH::HingeConstraint*>(e.c.GetPtr());
                e.info.value = glm::degrees((double)h->GetCurrentAngle());
                force = (double)h->GetTotalLambdaPosition().Length() / dt;
            } else {
                auto* sl = static_cast<JPH::SliderConstraint*>(e.c.GetPtr());
                e.info.value = (double)sl->GetCurrentPosition();
                force = (double)std::abs(sl->GetTotalLambdaPositionLimits()) / dt;
            }
            e.info.force = force;

            // (a) ¿CEDE la unión? Se quita la restricción: la hoja queda suelta y se cae sola. Una
            //     animación no puede caerse — o se reproduce o no (PLAN_PUERTOS §4, razón 2).
            if (e.spec.breakForce > 0.0 && force > e.spec.breakForce) {
                system.RemoveConstraint(e.c);
                e.c = nullptr; e.alive = false;
                e.info.broken = true; e.info.blocked = false;
                continue;
            }
            // (b) ¿BLOQUEADO? Con el motor pidiendo posición y sin avanzar durante varios pasos, algo
            //     estorba. Se mide por avance real, no por la fuerza: así vale igual para una caja en
            //     medio que para una rampa con demasiado peso encima.
            // "Ha llegado" necesita TOLERANCIA: un motor de posición converge con error residual, y
            // con un umbral de 1e-3 una puerta perfectamente abierta se declaraba BLOQUEADA para
            // siempre. Bloqueado = le falta camino DE VERDAD y no lo recorre.
            const double arriveEps =
                (e.spec.dof == PhysicsEngine::RailSpec::Dof::Hinge) ? 0.5 : 2e-3;
            if (e.spec.drive == PhysicsEngine::RailSpec::Drive::Motor &&
                std::abs(e.target - e.info.value) > arriveEps) {
                // ⚠️ "No avanza" hay que medirlo sobre una VENTANA, no paso a paso. Un mecanismo
                // atascado no se queda quieto: tiembla unas milésimas por la elasticidad del solver,
                // y con un umbral por paso el contador se reiniciaba constantemente y NUNCA se
                // declaraba bloqueado (medido: una rampa de 589 N·m con motor de 1 N·m reportaba
                // blocked=0 mientras estaba claramente vencida). Media vuelta de segundo sin recorrer
                // medio grado = bloqueada, tiemble lo que tiemble.
                const double blockEps =
                    (e.spec.dof == PhysicsEngine::RailSpec::Dof::Hinge) ? 0.5 : 2e-3;
                if (++e.stall >= 30) {
                    e.info.blocked = std::abs(e.info.value - e.lastValue) < blockEps;
                    e.lastValue = e.info.value;
                    e.stall = 0;
                }
            } else {
                e.stall = 0; e.info.blocked = false; e.lastValue = e.info.value;
            }
        }
    }

    void step(double dt, PhysicsEngine* eng, IWorldProvider* w,
              const std::vector<std::shared_ptr<RigidBody>>& bodies) {
        // SIN cuerpos dinámicos NI personaje no hay nada que simular: no construir malla (muestrear el
        // terreno es CARO) ni pisar Jolt. Evita el build en (0,0,0) y deja el frame libre.
        if (map.empty() && !charCtrl) return;
        simTime += dt;
        // Región de la malla del terreno = alrededor del primer cuerpo dinámico. Se REFRESCA cuando el
        // cuerpo se aleja >48 m del centro con el que se construyó, pero como MUCHO ~4×/s (rate-limit):
        // rehacer la malla muestrea el terreno (caro) → un cuerpo veloz no debe dispararlo cada paso.
        // La región del suelo sigue al PERSONAJE si lo hay (es quien camina); si no, al 1er dinámico.
        glm::dvec3 near(0.0);
        if (charBody) near = charBody->position;
        else for (auto& sp : bodies) if (map.count(sp.get())) { near = sp->position; break; }
        // Primera vez (spawn): SÍNCRONO — hay que tener suelo ya, no se puede caer un frame.
        if (!haveGround()) { rebuildGround(w, near); lastRebuildT = simTime; }

        // ── BANCO DE PRUEBAS DEL SUELO (`HARUKA_GROUND_BENCH=N`) ────────────────────────────────
        //
        // El desglose de coste de `buildGroundShape` solo se imprime cuando el jugador DERIVA 48 m,
        // o sea que medirlo exigía arrancar el juego y caminar — y un número que hay que ir a buscar
        // a mano no se mide, se supone. Con esto se fuerzan N reconstrucciones nada más existir el
        // suelo, cada una desde un centro distinto (48 m de separación, la deriva real) para que
        // ninguna se aproveche de cachés calientes de la anterior.
        //
        // Va aquí y no en un test porque lo caro es `sampleTerrainHeight`, que necesita el planeta
        // BAKEADO: fuera del juego no existe la función cuyo coste se quiere conocer.
        if (haveGround() && groundBenchLeft > 0) {
            const int n = groundBenchLeft--;
            HARUKA_LOGI("Physics", "banco del suelo: reconstruccion %d", n);
            buildGroundShape(w, near + glm::dvec3((double)n * 48.0, 0.0, 0.0));
        }

        // ¿Terminó un refresco en curso? Intercambia el cuerpo (barato, hilo de física). La malla vieja
        // colisionó hasta este instante → cero hueco.
        if (groundJob.valid() && groundJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            swapBody(farId, haveFar, groundJob.get());
            meshCenter = groundJobCenter; meshBuildElev = groundJobElev;
        }
        if (nearJob.valid() && nearJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            swapBody(nearId, haveNear, nearJob.get());
            nearAnchor = nearJobAnchor; nearAnchorSet = true;
        }

        // ── EL ANILLO CERCANO SIGUE AL ANCLAJE DEL RENDER, no a la deriva ───────────────────────
        //
        // El disparador NO es "me he movido X metros": es "la retícula del clipmap ha saltado". Son
        // cosas distintas — el anclaje se cuantiza en (lat, lon) y su paso en metros depende de la
        // latitud (`quad·cos(lat)`, 3,30 m a 0,6 rad). Preguntarlo por el `up` anclado es exacto y no
        // hay que replicar la cuantización aquí.
        //
        // Sin esto, el suelo que se PISA se quedaba anclado donde estabas hace hasta 48 m mientras el
        // que se DIBUJA se re-ancla cada 4 m: dos retículas distintas casi todo el rato, y como ningún
        // nodo sobrevive a un cambio de ancla (medido: 8 de 257 049) la disparidad no podía bajar de
        // la sagita de la celda por mucho que se afinara la aritmética.
        //
        // ⚠️ CON BANDA MUERTA. Este `if` comparaba las anclas por IGUALDAD EXACTA (`> 1e-12`), y como
        // la cuantización es un `round` duro, un jugador PARADO justo sobre un borde de celda hacía
        // saltar el ancla en cada frame: medido en el juego, 69 reconstrucciones en 8 s alternando
        // entre dos mallas de 18 432 triángulos, a 5-6 ms cada una — y el suelo cambiando bajo los
        // pies con ellas. Ver `terrainAnchorShouldJump` para el porqué del umbral y de por qué la
        // banda muerta vive aquí y no en `terrainClipFrame`.
        if (groundIsMesh && haveNear && !nearJob.valid()) {
            const glm::dvec3 a = anchorOf(w, near);
            const bool moved = nearAnchorSet && glm::length(a - nearAnchor) > 1e-12 &&
                               Haruka::Planet::terrainAnchorShouldJump(
                                   nearAnchor, near - w->activePlanetCenter(),
                                   w->activePlanetRadius());
            if (!nearAnchorSet || moved) {
                nearJobAnchor = a;
                nearJob = std::async(std::launch::async, [this, w, near] {
                    return buildGroundRings(w, near, 0, Haruka::Planet::TERRAIN_COLLIDE_UNIFORM_M, 0);
                });
            }
        }

        // ¿Hace falta refrescar? (movido >48 m — el radio fino del nuevo parche abandona al jugador —,
        // o el terreno bajo los pies cambió >0.5 m por refinado del LOD / deform, aunque el jugador esté
        // QUIETO). Se lanza en un WORKER: el parche (muestrear + montar la MeshJolt) se construye FUERA
        // del frame; el frame solo paga el intercambio cuando esté listo (arriba). Un solo job en vuelo.
        // El parche es el de 200 km (§9 Fase 3); por eso el 48 m es conservador pero barato en worker.
        // SONDA DE PARIDAD (cada ~2 s): cuánto se separa el suelo que se PISA del que describe la
        // función que dibuja el render. Son dos superficies distintas: la malla es lineal dentro de
        // cada celda y la función no, así que entre vértices divergen. Ninguna comparación de la
        // función consigo misma puede verlo — por eso se mide aquí, con la malla real.
        if (groundIsMesh && simTime - lastParityLogT > 2.0) {
            lastParityLogT = simTime;
            double funcH = 0.0, meshH = 0.0, cellM = 0.0;
            if (w->terrainParityProbe(near, meshCenter, funcH, meshH, cellM)) {
                const double drift = glm::length(near - meshCenter);
                // ⚠️ Y DÓNDE ACABA EL JUGADOR, que es lo que de verdad se nota.
                //
                // `funcion` vs `malla` compara dos descripciones del CAMPO DE ALTURA, las dos en CPU. Da
                // 0.000 m —comparten campo, bilineal, triM, radio base y recorte del nivel del mar— y
                // sin embargo se ve disparidad al caminar. Es que esa comparación no puede verla: lo que
                // se pisa no es el campo, es la forma de colisión de Jolt tal como la resuelve el
                // controlador de personaje, con su cápsula, su holgura de penetración y una malla que
                // solo se reconstruye al derivar 48 m.
                //
                // `pies` es la altura del cuerpo sobre la esfera de referencia menos su radio: si el
                // jugador flota o se hunde respecto al suelo que describe la función, sale AQUÍ y no en
                // ninguna comparación de campos.
                // ⚠️ HAY QUE RESTAR EL RADIO DEL CUERPO. La posición de un cuerpo con cápsula es el
                // centro de su esfera inferior, así que en reposo está EXACTAMENTE `radius` por encima
                // del suelo — el juego lo coloca así a propósito (`player.cpp`: `spawnPos.y + radius`).
                // La primera versión de esta sonda no lo restaba y reportaba +0.400 m constantes con un
                // radio de 0,4: un falso positivo que parecía justo el bug que se estaba buscando.
                double feetOffset = 0.0;
                if (w->hasActivePlanet()) {
                    const glm::dvec3 pc = w->activePlanetCenter();
                    const double alt = glm::length(near - pc) - w->activePlanetRadius();
                    const double bodyR = (charBody && charBody->radius > 0.0) ? charBody->radius : 0.0;
                    feetOffset = alt - bodyR - funcH;
                }
                HARUKA_LOGDIAG("TerrainParity",
                    "funcion=%.3f m  malla=%.3f m  |delta|=%.3f m  ·  celda=%.1f m  deriva=%.1f m"
                    "  ·  pies-suelo=%+.3f m (radio ya restado)",
                    funcH, meshH, std::fabs(funcH - meshH), cellM, drift, feetOffset);
            }
        }

        const bool drifted = groundIsMesh && glm::length(near - meshCenter) > 48.0;
        const bool refined = groundIsMesh && std::abs(w->terrainHeightAt(near) - meshBuildElev) > 0.5;
        // onSphere: el primer build cayó al fallback de ESFERA (terrainMesh falló ese instante, p.ej. el
        // planeta aún no estaba listo). La esfera es PLANA a la altura de un punto → el terreno aparece
        // decenas de m desplazado. Hay que SEGUIR intentando la malla (si no, se queda clavado en la
        // esfera para siempre, porque drifted/refined exigen que YA haya malla). Rate-limit lo acota.
        const bool onSphere = haveGround() && !groundIsMesh;
        if (((groundIsMesh && (drifted || refined)) || onSphere)
            && simTime - lastRebuildT > 0.25 && !groundJob.valid()) {
            groundJobCenter = near;
            groundJobElev   = w->terrainHeightAt(near);
            groundJob = std::async(std::launch::async, [this, w, near] {
                // Solo los anillos de FUERA: el 0 lo lleva `nearJob` con su propia cadencia. Si el
                // provider no da anillos, `buildGroundShape` cae a la malla/esfera de respaldo.
                if (JPH::Ref<JPH::Shape> r = buildGroundRings(w, near, 1, 200000.0, 1)) return r;
                return buildGroundShape(w, near);
            });
            lastRebuildT = simTime;
        }
        syncStatics(eng);   // TODO el mundo estático (colocados + recursos + escena) → cuerpos de Jolt
        stepCharacter(dt, eng, w);
        JPH::BodyInterface& bi = system.GetBodyInterface();
        for (auto& sp : bodies) {
            auto it = map.find(sp.get()); if (it == map.end()) continue;
            // ¿Lo manda el JUEGO ahora mismo (volar, nadar)? Entonces Jolt NO debe integrarlo: pasa a
            // KINEMÁTICO y se le lleva a la posición que dicta el juego. Antes el juego ponía
            // `isKinematic=true` pero Jolt lo ignoraba y lo seguía simulando con gravedad → los dos
            // peleaban por la posición y volar no funcionaba. Kinemático en Jolt además EMPUJA a los
            // cuerpos dinámicos con los que choque, que es justo lo que quieres al volar contra algo.
            if (sp->isKinematic) {
                if (bi.GetMotionType(it->second) != JPH::EMotionType::Kinematic)
                    bi.SetMotionType(it->second, JPH::EMotionType::Kinematic, JPH::EActivation::Activate);
                if (finite3(sp->position)) {
                    const glm::dvec3 lp = sp->position - origin;
                    bi.SetPositionAndRotation(it->second,
                        JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z),
                        JPH::Quat::sIdentity(), JPH::EActivation::Activate);
                }
                if (finite3(sp->velocity))
                    bi.SetLinearVelocity(it->second, JPH::Vec3((float)sp->velocity.x, (float)sp->velocity.y, (float)sp->velocity.z));
                continue;   // sin gravedad y sin lectura de vuelta: la posición la posee el juego
            }
            if (bi.GetMotionType(it->second) != JPH::EMotionType::Dynamic)
                bi.SetMotionType(it->second, JPH::EMotionType::Dynamic, JPH::EActivation::Activate);
            // Un cuerpo LIBRE lo POSEE el motor: NO se le re-impone su velocidad cada paso. Hacerlo
            // re-aplicaba la velocidad de SEPARACIÓN que el solver genera al resolver una penetración
            // (normalmente transitoria) una y otra vez → el objeto acumulaba energía y salía volando,
            // como si no pesara. Solo se le pasa cuando el juego la cambia A PROPÓSITO (lanzar, empujar).
            if (sp->velocityDirty) {
                if (finite3(sp->velocity))   // Jolt aserta con NaN → nunca inyectar basura
                    bi.SetLinearVelocity(it->second, JPH::Vec3((float)sp->velocity.x, (float)sp->velocity.y, (float)sp->velocity.z));
                bi.ActivateBody(it->second);
                sp->velocityDirty = false;
            }
            // MISMA función que el personaje. Antes eran dos: la suma de `gravBodies()` (que no tenía
            // planetas, así que solo aportaba el Sol y era «insignificante» como decía el comentario)
            // más un parche radial de 9.81 hacia el planeta activo. Un escombro y el jugador caían por
            // reglas distintas, y ninguna de las dos usaba la masa del cuerpo.
            const glm::dvec3 F = gravityAtPoint(w, sp->position) * sp->mass;
            if (finite3(F))
                bi.AddForce(it->second, JPH::Vec3((float)F.x, (float)F.y, (float)F.z));
        }
        system.Update((float)dt, 1, &temp, &jobs);
        updateRails(dt);
        for (auto& sp : bodies) {
            auto it = map.find(sp.get()); if (it == map.end()) continue;
            if (sp->isKinematic) continue;   // lo mueve el juego (vuelo/nado): no le pisemos la posición
            const JPH::RVec3 p = bi.GetPosition(it->second);
            const JPH::Vec3  v = bi.GetLinearVelocity(it->second);
            const JPH::Quat  q = bi.GetRotation(it->second);
            const JPH::Vec3  a = bi.GetAngularVelocity(it->second);
            sp->position    = origin + glm::dvec3(p.GetX(), p.GetY(), p.GetZ());   // local → mundo (double)
            sp->velocity    = glm::dvec3(v.GetX(), v.GetY(), v.GetZ());
            sp->orientation = glm::dquat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
            sp->angularVel  = glm::dvec3(a.GetX(), a.GetY(), a.GetZ());
        }
    }
};
#endif // HARUKA_HAS_JOLT

PhysicsEngine::PhysicsEngine() {
#ifdef HARUKA_HAS_JOLT
    // Jolt es el motor de rígidos por defecto. Escape de depuración: HARUKA_JOLT=0 fuerza el solver a mano.
    const char* v = std::getenv("HARUKA_JOLT");
    if (!(v && v[0] == '0')) {
        JoltImpl::globalInit();                 // registra el allocator de Jolt ANTES de construir los
        m_jolt = std::make_unique<JoltImpl>();  // miembros de JoltImpl (el TempAllocator ya lo necesita)
    }
#endif
}

PhysicsEngine::~PhysicsEngine() {}

void PhysicsEngine::addBody(std::shared_ptr<RigidBody> body) {
    bodies.push_back(body);
    if (octree) octree->insert(body);
#ifdef HARUKA_HAS_JOLT
    // Los DINÁMICOS los simula Jolt; los kinemáticos los dirige el juego (posición directa).
    if (m_jolt && !body->isKinematic) m_jolt->addBody(body.get());
#endif
}

void PhysicsEngine::removeBody(const std::string& name) {
    auto it = std::find_if(bodies.begin(), bodies.end(),
        [&name](const std::shared_ptr<RigidBody>& b) { return b->name == name; });
    if (it != bodies.end()) {
#ifdef HARUKA_HAS_JOLT
        if (m_jolt) m_jolt->removeBody(it->get());   // si no, queda colisionador fantasma + clave colgante
#endif
        bodies.erase(it);
    }
}

std::shared_ptr<RigidBody> PhysicsEngine::getBody(const std::string& name) {
    auto it = std::find_if(bodies.begin(), bodies.end(),
        [&name](const std::shared_ptr<RigidBody>& b) { return b->name == name; });
    return (it != bodies.end()) ? *it : nullptr;
}

// ── RAÍLES (docs/guides/PLAN_PUERTOS.md §4) ─────────────────────────────────────────────────────
// Sin Jolt no hay mecanismo: se devuelve -1 en vez de fingir uno. Un raíl simulado a mano sería justo
// la animación que este sistema existe para no tener.
int PhysicsEngine::addRail(const std::string& bodyA, const std::string& bodyB, const RailSpec& spec) {
#ifdef HARUKA_HAS_JOLT
    if (!m_jolt) return -1;
    auto a = getBody(bodyA), b = getBody(bodyB);
    if (!a || !b) return -1;
    return m_jolt->addRail(a.get(), b.get(), spec);
#else
    (void)bodyA; (void)bodyB; (void)spec; return -1;
#endif
}

bool PhysicsEngine::railSetTarget(int railId, double target) {
#ifdef HARUKA_HAS_JOLT
    if (!m_jolt || railId < 0 || railId >= (int)m_jolt->rails.size()) return false;
    m_jolt->railTarget(railId, target);
    return true;
#else
    (void)railId; (void)target; return false;
#endif
}

bool PhysicsEngine::railGet(int railId, RailInfo& out) const {
#ifdef HARUKA_HAS_JOLT
    if (!m_jolt || railId < 0 || railId >= (int)m_jolt->rails.size()) return false;
    out = m_jolt->rails[(size_t)railId].info;
    return true;
#else
    (void)railId; (void)out; return false;
#endif
}

void PhysicsEngine::removeRail(int railId) {
#ifdef HARUKA_HAS_JOLT
    if (!m_jolt || railId < 0 || railId >= (int)m_jolt->rails.size()) return;
    auto& e = m_jolt->rails[(size_t)railId];
    if (!e.alive) return;
    // Se marca muerto pero NO se borra del vector: los ids que tenga el juego seguirían siendo válidos
    // y apuntarían a otro raíl. Un hueco cuesta unos bytes; un id reutilizado cuesta un bug mudo.
    m_jolt->system.RemoveConstraint(e.c);
    e.c = nullptr; e.alive = false;
#else
    (void)railId;
#endif
}

void PhysicsEngine::update(double deltaTime) {
#ifdef HARUKA_HAS_JOLT
    // Fase 1: con Jolt disponible, los cuerpos DINÁMICOS los simula él (gravedad radial + suelo +
    // colisión estables). Los kinemáticos, OBB estáticos y resolveSphere siguen su camino (no aquí).
    if (m_jolt) { m_jolt->step(deltaTime, this, m_world, bodies); return; }
#endif
    integrateForces(deltaTime);
    broadPhaseAABB();
    detectCollisions();
    resolveCollisions();
    resolveStaticCollisions();
}

void PhysicsEngine::advance(double frameDt) {
    // TIMESTEP FIJO con acumulador. El motor SIEMPRE avanza en pasos de kFixedDt, sea cual sea el
    // dt de reloj. Es REQUISITO de determinismo cliente↔servidor: con dt variable, el mismo input
    // da trayectorias distintas según los fps → cliente y DGS discreparían y la validación fallaría.
    // El resto (frameDt no múltiplo exacto) se acumula para el frame siguiente.
    m_accum += frameDt;
    // Cota anti-espiral: si el frame se congela (breakpoint, hitch), no intentar recuperar segundos
    // de simulación de golpe (bloquearía más) — se descarta el exceso.
    if (m_accum > kMaxAccum) m_accum = kMaxAccum;
    while (m_accum >= kFixedDt) {
        update(kFixedDt);
        m_accum -= kFixedDt;
    }
}

void PhysicsEngine::integrateForces(double dt) {
    // Nivel del mar del planeta activo (esfera de radio seaR). La TIERRA siempre está por
    // encima del nivel del mar → un cuerpo por debajo de esta esfera está en una cuenca
    // oceánica = sumergido. Se usa para el empuje de Arquímedes de los cuerpos dinámicos.
    bool       haveSea = false;
    glm::dvec3 seaCenter(0.0);
    double     seaR = 0.0;
    if (m_world && m_world->hasActivePlanet()) {
        haveSea   = true;
        seaCenter = m_world->activePlanetCenter();
        seaR      = m_world->activePlanetRadius();
    }

    for (auto& body : bodies) {
        if (body->isKinematic) continue;

        // Gravedad RADIAL hacia el planeta (antes era plana -Y, que solo valía cerca del polo). La
        // suma newtoniana de los cuerpos vía calculateGravityAtPosition da la dirección y magnitud
        // reales; sin m_world cae a (0,-1,0)·9.81. Es lo que permite que un cuerpo dinámico "caiga
        // hacia el suelo" en cualquier punto del planeta.
        glm::dvec3 gDir;
        double gMag = calculateGravityAtPosition(body->position, gDir);
        body->acceleration = gDir * gMag;

        // Velocity Verlet
        body->velocity += body->acceleration * dt;

        // Arrastre AERODINÁMICO (solo cuerpos dinámicos = conjunto acotado cerca del
        // juego → O(cuerpos), sin coste global). Resistencia del aire (amortigua hacia
        // 0) + empuje del viento (cuadrático con la velocidad relativa). Suave.
        body->velocity *= (1.0 - std::min(m_airDamp * dt, 0.5));
        glm::dvec3 vRel = m_wind - body->velocity;
        double rel = glm::length(vRel);
        if (rel > 1e-6)
            body->velocity += vRel * std::min(m_windCoef * rel * dt, 0.20);

        // --- BUOYANCY (empuje de Arquímedes) + arrastre de agua para cuerpos SUMERGIDOS ---
        // f = fracción sumergida: 0 cuando el cuerpo apenas toca la superficie por arriba
        // (dist = seaR+radio), 1 cuando está totalmente bajo el agua (dist ≤ seaR−radio).
        // Empuje hacia el radial +up escala con f y con la razón de densidades (buoyRatio>1 →
        // flota); arrastre fuerte amortigua la velocidad → el objeto se ASIENTA flotando en la
        // superficie (equilibrio a f≈1/buoyRatio) en vez de oscilar. up radial ≈ world-up cerca
        // del jugador (donde la gravedad plana -Y ya apunta hacia el planeta).
        if (haveSea && body->radius > 1e-4) {
            glm::dvec3 rel2 = body->position - seaCenter;
            double dist = glm::length(rel2);
            double r    = body->radius;
            if (dist > 1e-6 && dist < seaR + r) {
                glm::dvec3 up = rel2 / dist;
                double f = glm::clamp((seaR + r - dist) / (2.0 * r), 0.0, 1.0);
                // SPLASH: al CRUZAR la superficie hacia dentro (aire→agua) con velocidad de entrada
                // apreciable → dispara el callback UNA vez (no cada frame, por el flag inWater). El
                // juego emite partículas PBF ahí → el impacto salpica.
                if (f > 0.05 && !body->inWater) {
                    double vDown = -glm::dot(body->velocity, up); // componente hacia el agua (m/s)
                    if (vDown > 1.5 && m_onWaterEntry)
                        m_onWaterEntry(seaCenter + up * seaR, body->velocity, r);
                    body->inWater = true;
                }
                if (f > 0.0) {
                    double g = glm::length(gravity);
                    const double buoyRatio = 1.1;                 // agua/objeto (>1 = flota)
                    body->velocity += up * (f * g * buoyRatio * dt);   // Arquímedes
                    body->velocity -= body->velocity * (1.0 - std::exp(-3.0 * f * dt)); // arrastre
                }
            } else {
                body->inWater = false; // fuera del agua → rearma el splash para la próxima entrada
            }
        }

        body->position += body->velocity * dt;

        // --- GROUND-SNAP contra el TERRENO ------------------------------------------------------
        // El suelo está en radio = planetRadius + altura del terreno + radio del cuerpo. Si el cuerpo
        // cayó por debajo, se sube a la superficie y se le quita la componente de velocidad que
        // apunta HACIA DENTRO (la tangencial se conserva → puede deslizar/andar). El terreno lo da
        // m_world (cliente: la malla F10; servidor: el sampler analítico) → misma superficie que la
        // que se ve/valida. Sustituye al "surface constraint" que el juego hacía a mano.
        if (haveSea && body->radius > 1e-4) {
            const glm::dvec3 rel = body->position - seaCenter;
            const double dist = glm::length(rel);
            if (dist > 1e-6) {
                const glm::dvec3 up = rel / dist;
                if (body->shape == RigidBody::Shape::Box && m_world) {
                    // COLISIÓN CONTRA EL TERRENO POR LOS VÉRTICES DE LA FORMA REAL. Si el cuerpo define
                    // sus `points` (casco convexo del objeto), se prueba cada uno; si no, la caja usa sus
                    // 8 esquinas. Así la colisión sigue la FORMA del objeto (no una esfera/caja fija). El
                    // cuerpo se apoya en su cara/vértices y VUELCA sobre la arista si el centro de masa se
                    // sale del apoyo. ⚠️ ESTABILIDAD (antes "flotaba/giraba"): posición corregida por la
                    // penetración MÁXIMA una vez (no sumada → no se autolanza), impulsos REPARTIDOS entre
                    // contactos, y amortiguación angular fuerte + asentamiento al descansar lento.
                    const glm::dvec3 comW = body->orientation * body->comOffset;
                    const double invM = 1.0 / body->mass;
                    const double havg = (body->halfExtents.x + body->halfExtents.y + body->halfExtents.z) / 3.0;
                    const double invI = 1.0 / ((1.0 / 6.0) * body->mass * (2.0 * havg) * (2.0 * havg) + 1e-9);
                    glm::dvec3 hit[32]; int nh = 0; double maxPen = 0.0;
                    auto test = [&](const glm::dvec3& local) {
                        const glm::dvec3 corner = body->position + body->orientation * local;
                        const double cdist = glm::length(corner - seaCenter);
                        if (cdist < 1e-6) return;
                        const double pen = (seaR + m_world->terrainHeightAt(corner)) - cdist;
                        if (pen > 0.0 && nh < 32) { hit[nh++] = corner; maxPen = std::max(maxPen, pen); }
                    };
                    if (!body->points.empty()) {
                        for (const glm::dvec3& p : body->points) test(p);
                    } else {
                        for (int sx = -1; sx <= 1; sx += 2)
                        for (int sy = -1; sy <= 1; sy += 2)
                        for (int sz = -1; sz <= 1; sz += 2)
                            test(glm::dvec3(sx * body->halfExtents.x, sy * body->halfExtents.y, sz * body->halfExtents.z));
                    }
                    if (nh > 0) {
                        body->position += up * (maxPen * 0.8);                    // sacar del suelo UNA vez
                        const double relax = 1.0 / (double)nh;                    // repartir entre esquinas
                        for (int k = 0; k < nh; ++k) {
                            const glm::dvec3 rc = hit[k] - (body->position + comW);
                            const glm::dvec3 vc = body->velocity + glm::cross(body->angularVel, rc);
                            const double vn = glm::dot(vc, up);
                            if (vn >= 0.0) continue;
                            const glm::dvec3 rcxn = glm::cross(rc, up);
                            const double kn = invM + glm::dot(up, glm::cross(rcxn * invI, rc));
                            const double e = (vn < -1.5) ? 0.3 : 0.0;              // BOTE solo en impactos fuertes
                            const double jn = (-(1.0 + e) * vn / std::max(kn, 1e-9)) * relax;   // relajado
                            const glm::dvec3 P = up * jn;
                            body->velocity   += P * invM;
                            body->angularVel += glm::cross(rc, P) * invI;
                            const glm::dvec3 vt = vc - up * vn;                     // rozamiento tangencial
                            const double vtl = glm::length(vt);
                            if (vtl > 1e-6) {
                                const glm::dvec3 t = vt / vtl;
                                const glm::dvec3 rcxt = glm::cross(rc, t);
                                const double kt = invM + glm::dot(t, glm::cross(rcxt * invI, rc));
                                const double jt = glm::clamp((-vtl / std::max(kt, 1e-9)) * relax, -0.7 * jn, 0.7 * jn);
                                const glm::dvec3 Pt = t * jt;
                                body->velocity   += Pt * invM;
                                body->angularVel += glm::cross(rc, Pt) * invI;
                            }
                        }
                        // Amortiguación angular: fuerte, y AÚN más cuando descansa girando lento →
                        // ASIENTA en vez de rotar sobre sí mismo eternamente (lo que veías).
                        const double slow = glm::length(body->angularVel) < 0.6 ? 8.0 : 3.0;
                        body->angularVel *= std::max(0.0, 1.0 - slow * dt);
                    }
                } else {
                const double terrainM = m_world ? m_world->terrainHeightAt(body->position) : 0.0;
                const double floorDist = seaR + terrainM + body->radius;
                if (dist < floorDist) {
                    body->position = seaCenter + up * floorDist;          // sube al suelo
                    const double vIn = glm::dot(body->velocity, up);      // <0 = cayendo hacia dentro
                    if (vIn < 0.0) {
                        const double e = (vIn < -1.5) ? 0.3 : 0.0;        // BOTE solo en impactos fuertes (no en reposo)
                        body->velocity -= up * vIn * (1.0 + e);           // anula lo radial + rebota una fracción
                    }

                    // DESLIZAMIENTO POR PENDIENTE. La componente de la gravedad TANGENTE al terreno
                    // empuja cuesta abajo, pero solo en cuestas más empinadas que el ÁNGULO DE REPOSO
                    // (kReposeCos): así los objetos sueltos ruedan por las laderas fuertes y se quedan
                    // en lo llano — y el jugador NO resbala en pendientes suaves. La normal sale de
                    // muestrear el MISMO campo del suelo (terrainHeightAt) en dos tangentes → coincide
                    // con la superficie que se ve/colisiona. + rozamiento tangencial (los sueltos frenan).
                    constexpr double kReposeCos      = 0.82;   // cos(~35°): más empinado → desliza
                    constexpr double kGroundFriction = 2.5;    // 1/s de rozamiento tangencial en el suelo
                    glm::dvec3 t1 = glm::cross(up, std::abs(up.y) < 0.99 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
                    const double t1len = glm::length(t1);
                    if (m_world && t1len > 1e-9) {
                        t1 /= t1len;
                        const glm::dvec3 t2 = glm::cross(up, t1);
                        const double e  = std::max(0.25, body->radius);
                        const double dh1 = (m_world->terrainHeightAt(body->position + t1 * e)
                                          - m_world->terrainHeightAt(body->position - t1 * e)) / (2.0 * e);
                        const double dh2 = (m_world->terrainHeightAt(body->position + t2 * e)
                                          - m_world->terrainHeightAt(body->position - t2 * e)) / (2.0 * e);
                        const glm::dvec3 N = glm::normalize(up - t1 * dh1 - t2 * dh2);
                        if (glm::dot(N, up) < kReposeCos) {              // cuesta > reposo → deslizar
                            const glm::dvec3 gvec = gDir * gMag;         // gravedad (radial)
                            const glm::dvec3 tang = gvec - glm::dot(gvec, N) * N;   // proyección cuesta abajo
                            body->velocity += tang * dt;
                        }
                        const glm::dvec3 vTang = body->velocity - glm::dot(body->velocity, up) * up;
                        body->velocity -= vTang * std::min(kGroundFriction * dt, 1.0);
                    }
                    // VUELCO por BALANCE: si el centro de masa está DESCENTRADO (comOffset≠0), apoyado
                    // la gravedad hace PALANCA → un torque que gira el cuerpo hacia donde pesa (vuelca
                    // por un lado y no por otro). Inercia ∝ masa·radio² → los más pesados/grandes
                    // vuelcan más despacio. comOffset=0 (equilibrado) → torque nulo, no vuelca.
                    if (glm::dot(body->comOffset, body->comOffset) > 1e-12) {
                        const glm::dvec3 comW   = body->orientation * body->comOffset;   // offset → mundo
                        const glm::dvec3 torque = glm::cross(comW, gDir * (gMag * body->mass));
                        const double     I      = 0.4 * body->mass * body->radius * body->radius + 1e-6;
                        body->angularVel += (torque / I) * dt;
                        body->angularVel *= std::max(0.0, 1.0 - 1.5 * dt);              // rozamiento angular
                    }
                }
                }   // cierra el else (rama ESFERA)
            }
        }

        // Integrar la ORIENTACIÓN desde la velocidad angular (cualquier cuerpo que gire; con angularVel=0
        // no hay cambio). Cuaternión: q ← normalize(q + ½·ω·q·dt).
        if (glm::dot(body->angularVel, body->angularVel) > 1e-14) {
            const glm::dquat w(0.0, body->angularVel.x, body->angularVel.y, body->angularVel.z);
            body->orientation = glm::normalize(body->orientation + 0.5 * w * body->orientation * dt);
        }
    }
}

namespace {
    // Vértices del mundo de un cuerpo con forma: su casco `points` si lo tiene, si no las 8 esquinas.
    inline int shapeVertices(const RigidBody* b, glm::dvec3 out[32]) {
        int n = 0;
        if (!b->points.empty()) {
            for (const auto& p : b->points) if (n < 32) out[n++] = b->position + b->orientation * p;
        } else {
            for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) for (int sz = -1; sz <= 1; sz += 2)
                out[n++] = b->position + b->orientation * glm::dvec3(sx * b->halfExtents.x, sy * b->halfExtents.y, sz * b->halfExtents.z);
        }
        return n;
    }
    // ¿Punto p (mundo) DENTRO de la caja? outN = normal de cara SALIENTE (mundo), pen = profundidad (eje mínimo).
    inline bool pointInBox(const glm::dvec3& p, const RigidBody* box, glm::dvec3& outN, double& pen) {
        const glm::dvec3 lp = glm::conjugate(box->orientation) * (p - box->position);
        const glm::dvec3 h = box->halfExtents;
        const double dx = h.x - std::abs(lp.x), dy = h.y - std::abs(lp.y), dz = h.z - std::abs(lp.z);
        if (dx < 0.0 || dy < 0.0 || dz < 0.0) return false;               // fuera
        glm::dvec3 ln;
        if (dx <= dy && dx <= dz)      { ln = glm::dvec3(lp.x < 0 ? -1 : 1, 0, 0); pen = dx; }
        else if (dy <= dz)             { ln = glm::dvec3(0, lp.y < 0 ? -1 : 1, 0); pen = dy; }
        else                           { ln = glm::dvec3(0, 0, lp.z < 0 ? -1 : 1); pen = dz; }
        outN = box->orientation * ln;                                     // normal saliente en el mundo
        return true;
    }
}

void PhysicsEngine::detectCollisions() {
    collisions.clear();

    // NARROW-PHASE POR FORMA (nada de radio envolvente para la colisión, salvo esfera↔esfera, que ES
    // su forma real). El radio solo lo usa el BROAD-PHASE para descartar pares.
    //  · esfera↔esfera: 1 contacto por radios.
    //  · caja↔caja: los VÉRTICES de cada una dentro de la otra → un contacto por vértice (apila real).
    //  · esfera↔caja: el punto más cercano de la caja al centro de la esfera.
    auto narrow = [&](RigidBody* a, RigidBody* b) {
        const bool aBox = (a->shape == RigidBody::Shape::Box);
        const bool bBox = (b->shape == RigidBody::Shape::Box);
        if (!aBox && !bBox) {                                   // esfera ↔ esfera
            const glm::dvec3 d = b->position - a->position;
            const double dist = glm::length(d), minD = a->radius + b->radius;
            if (dist < minD && dist > 1e-9) {
                CollisionInfo c; c.bodyA = a; c.bodyB = b; c.penetration = minD - dist;
                c.normal = d / dist; c.point = a->position + c.normal * a->radius;
                collisions.push_back(c);
            }
            return;
        }
        if (aBox && bBox) {                                    // caja ↔ caja (por vértices, ambos sentidos)
            glm::dvec3 v[32], nOut; double pen;
            const int na = shapeVertices(a, v);
            for (int i = 0; i < na; ++i) if (pointInBox(v[i], b, nOut, pen)) {
                CollisionInfo c; c.bodyA = a; c.bodyB = b; c.penetration = pen;
                c.normal = -nOut; c.point = v[i]; collisions.push_back(c);   // saca el vértice de A fuera de B
            }
            const int nb = shapeVertices(b, v);
            for (int i = 0; i < nb; ++i) if (pointInBox(v[i], a, nOut, pen)) {
                CollisionInfo c; c.bodyA = a; c.bodyB = b; c.penetration = pen;
                c.normal = nOut; c.point = v[i]; collisions.push_back(c);    // saca el vértice de B fuera de A
            }
            return;
        }
        // esfera ↔ caja
        RigidBody* s = aBox ? b : a;                            // la esfera
        RigidBody* x = aBox ? a : b;                            // la caja
        const glm::dvec3 lc = glm::conjugate(x->orientation) * (s->position - x->position);
        const glm::dvec3 cl = glm::clamp(lc, -x->halfExtents, x->halfExtents);
        glm::dvec3 nOut; double pen;
        if (cl == lc) {                                        // centro DENTRO de la caja
            if (!pointInBox(s->position, x, nOut, pen)) return;
            pen += s->radius;
        } else {
            const glm::dvec3 wc = x->position + x->orientation * cl;
            const glm::dvec3 delta = s->position - wc;
            const double dist = glm::length(delta);
            if (dist >= s->radius || dist < 1e-9) return;
            nOut = delta / dist; pen = s->radius - dist;
        }
        CollisionInfo c; c.bodyA = a; c.bodyB = b; c.penetration = pen;
        c.point = x->position + x->orientation * cl;
        c.normal = (s == a) ? -nOut : nOut;                    // nOut va de la caja hacia la esfera
        collisions.push_back(c);
    };

    if (!octree) {
        for (size_t i = 0; i < bodies.size(); ++i)
            for (size_t j = i + 1; j < bodies.size(); ++j)
                narrow(bodies[i].get(), bodies[j].get());
    } else {
        for (auto& body : bodies) {
            std::vector<std::shared_ptr<RigidBody>> nearby;
            octree->getNearbodies(body, nearby);
            for (auto& other : nearby)
                if (body.get() < other.get())                  // cada par UNA vez (orden estable por puntero)
                    narrow(body.get(), other.get());
        }
    }
}

void PhysicsEngine::resolveCollisions() {
    // Resolución por IMPULSOS en el PUNTO DE CONTACTO real (col.point): separa el solape, intercambia
    // MOMENTO (lineal) y aplica impulso ANGULAR con el brazo al centro de masa → los cuerpos se empujan
    // y GIRAN al chocar según DÓNDE se tocan (una caja golpeada en una esquina vuelca). Inercia POR
    // FORMA (caja vs esfera). Rozamiento tangencial en el cono de Coulomb.
    constexpr double kFriction = 0.5;
    auto invInertia = [](const RigidBody* b) -> double {
        if (b->isKinematic) return 0.0;
        double I;
        if (b->shape == RigidBody::Shape::Box) {
            const double havg = (b->halfExtents.x + b->halfExtents.y + b->halfExtents.z) / 3.0;
            I = (1.0 / 6.0) * b->mass * (2.0 * havg) * (2.0 * havg);
        } else {
            I = 0.4 * b->mass * b->radius * b->radius;
        }
        return 1.0 / (I + 1e-9);
    };
    for (auto& col : collisions) {
        RigidBody* A = col.bodyA; RigidBody* B = col.bodyB;
        const bool aKin = A->isKinematic, bKin = B->isKinematic;
        if (aKin && bKin) continue;
        const glm::dvec3 n = col.normal;                        // de A hacia B
        const double invMa = aKin ? 0.0 : 1.0 / A->mass;
        const double invMb = bKin ? 0.0 : 1.0 / B->mass;
        const double invSum = invMa + invMb;
        if (invSum <= 0.0) continue;

        // (1) Separación del solape (Baumgarte 0.8: suave, para no explotar con varios contactos/par).
        const double corr = (col.penetration * 0.8) / invSum;
        A->position -= n * (corr * invMa);
        B->position += n * (corr * invMb);

        // (2) Impulso en el punto de contacto REAL, con inercia por forma.
        const double invIa = invInertia(A), invIb = invInertia(B);
        const glm::dvec3 rA = col.point - (A->position + A->orientation * A->comOffset);
        const glm::dvec3 rB = col.point - (B->position + B->orientation * B->comOffset);
        const glm::dvec3 vRel = (B->velocity + glm::cross(B->angularVel, rB))
                              - (A->velocity + glm::cross(A->angularVel, rA));
        const double vn = glm::dot(vRel, n);
        if (vn < 0.0) {                                         // se acercan → responder
            const double e = (vn < -1.5) ? 0.2 : 0.0;          // bote solo en impactos fuertes
            const glm::dvec3 rAxn = glm::cross(rA, n), rBxn = glm::cross(rB, n);
            const double kn = invSum + glm::dot(n, glm::cross(rAxn * invIa, rA))
                                     + glm::dot(n, glm::cross(rBxn * invIb, rB));
            const double jn = -(1.0 + e) * vn / std::max(kn, 1e-9);
            const glm::dvec3 P = n * jn;
            A->velocity   -= P * invMa;                B->velocity   += P * invMb;
            A->angularVel -= glm::cross(rA, P) * invIa; B->angularVel += glm::cross(rB, P) * invIb;

            // (3) Rozamiento tangencial (cono de Coulomb) → frena el deslizamiento y da GIRO al chocar.
            const glm::dvec3 vt = vRel - n * vn;
            const double vtl = glm::length(vt);
            if (vtl > 1e-6) {
                const glm::dvec3 t = vt / vtl;
                const glm::dvec3 rAxt = glm::cross(rA, t), rBxt = glm::cross(rB, t);
                const double kt = invSum + glm::dot(t, glm::cross(rAxt * invIa, rA))
                                         + glm::dot(t, glm::cross(rBxt * invIb, rB));
                const double jt = glm::clamp(-vtl / std::max(kt, 1e-9), -kFriction * jn, kFriction * jn);
                const glm::dvec3 Pt = t * jt;
                A->velocity   -= Pt * invMa;                B->velocity   += Pt * invMb;
                A->angularVel -= glm::cross(rA, Pt) * invIa; B->angularVel += glm::cross(rB, Pt) * invIb;
            }
        }
    }
}

void PhysicsEngine::addStaticBox(const glm::dvec3& center, const glm::dvec3& halfExtents) {
    staticBoxes.push_back({ center - halfExtents, center + halfExtents });
    ++m_staticsVersion;
}

void PhysicsEngine::clearStaticBoxes() {
    staticBoxes.clear();
    ++m_staticsVersion;
}

void PhysicsEngine::addPlacedOBB(const glm::dvec3& center, const glm::dvec3& halfExtents, const glm::dmat3& rot) {
    placedOBBs.push_back({ center, halfExtents, rot });
    ++m_staticsVersion;   // Jolt reconstruye sus cuerpos estáticos en el siguiente paso
}

void PhysicsEngine::clearPlacedOBBs() {
    placedOBBs.clear();
    ++m_staticsVersion;
}

void PhysicsEngine::addPropOBB(const glm::dvec3& center, const glm::dvec3& halfExtents, const glm::dmat3& rot) {
    propOBBs.push_back({ center, halfExtents, rot });
    ++m_staticsVersion;   // los recursos se regeneran al moverse → Jolt refresca sus estáticos
}

void PhysicsEngine::clearPropOBBs() {
    propOBBs.clear();
    ++m_staticsVersion;
}

void PhysicsEngine::addPropCone(const glm::dvec3& center, double halfHeight, double rTop,
                                double rBottom, const glm::dmat3& rot) {
    propCones.push_back({ center, halfHeight, rTop, rBottom, rot });
    ++m_staticsVersion;
}

int PhysicsEngine::registerPropMesh(const float* verts, size_t vertCount,
                                    const uint32_t* idx, size_t idxCount) {
#ifdef HARUKA_HAS_JOLT
    if (m_jolt) return m_jolt->addMeshShape(verts, vertCount, idx, idxCount);
#endif
    (void)verts; (void)vertCount; (void)idx; (void)idxCount;
    return -1;
}

void PhysicsEngine::addPropMeshInstance(int shapeId, const glm::dvec3& center,
                                        const glm::dmat3& rot, double scale) {
    if (shapeId < 0) return;
    propMeshes.push_back({ shapeId, center, rot, scale });
    ++m_staticsVersion;
}

void PhysicsEngine::clearPropMeshInstances() {
    propMeshes.clear();
    ++m_staticsVersion;
}

void PhysicsEngine::clearPropCones() {
    propCones.clear();
    ++m_staticsVersion;
}

glm::dvec3 PhysicsEngine::resolveSphere(const glm::dvec3& center0, double radius,
                                        const glm::dvec3& up, bool& grounded) const {
    glm::dvec3 center = center0;
    auto process = [&](const std::vector<StaticOBB>& list) {
        for (const auto& b : list) {
            // Broad-phase: salta cajas lejanas (clave con cientos de props).
            glm::dvec3 dd = center - b.center;
            double maxR = radius + glm::length(b.halfExtents) + 0.5;
            if (glm::dot(dd, dd) > maxR * maxR) continue;

            // Centro de la esfera al espacio LOCAL del OBB (rot ortonormal → inv = transpose).
            glm::dvec3 lp = glm::transpose(b.rot) * (center - b.center);
            const glm::dvec3& he = b.halfExtents;
            glm::dvec3 cp = glm::clamp(lp, -he, he);
            glm::dvec3 d  = lp - cp;
            double dist2  = glm::dot(d, d);

            glm::dvec3 newlp = lp;
            if (dist2 <= 1e-12) {                       // centro DENTRO → empuja por la cara más cercana
                double px = he.x - std::abs(lp.x);
                double py = he.y - std::abs(lp.y);
                double pz = he.z - std::abs(lp.z);
                double m  = std::min({ px, py, pz });
                if      (m == px) newlp.x = (lp.x >= 0.0 ? he.x + radius : -he.x - radius);
                else if (m == py) newlp.y = (lp.y >= 0.0 ? he.y + radius : -he.y - radius);
                else              newlp.z = (lp.z >= 0.0 ? he.z + radius : -he.z - radius);
            } else if (dist2 < radius * radius) {       // la esfera roza la caja → empuja por la normal
                double dist = std::sqrt(dist2);
                newlp = cp + (dist > 1e-9 ? d / dist : glm::dvec3(0, 1, 0)) * radius;
            } else {
                continue;                               // no toca
            }

            glm::dvec3 newCenter = b.center + b.rot * newlp;
            if (glm::dot(newCenter - center, up) > 0.3 * radius) grounded = true; // apoyado encima
            center = newCenter;
        }
    };
    process(placedOBBs);
    process(propOBBs);
    // El solver a mano (para cuerpos que no son de Jolt) no sabe de conos: se aproximan por su caja
    // envolvente, que es lo que había antes para todo. Jolt sí usa la forma exacta.
    {
        std::vector<StaticOBB> coneBoxes;
        coneBoxes.reserve(propCones.size());
        for (const StaticCone& c : propCones) {
            const double r = std::max(c.rTop, c.rBottom);
            coneBoxes.push_back({ c.center, glm::dvec3(r, c.halfHeight, r), c.rot });
        }
        process(coneBoxes);

    }
    return center;
}

void PhysicsEngine::broadPhaseAABB() {
    if (!octree || bodies.empty()) return;
    // Rebuild the octree each step so moved bodies are found correctly.
    // Each body is re-inserted using its updated position from integrateForces().
    for (auto& body : bodies) {
        octree->remove(body);
        octree->insert(body);
    }
}

void PhysicsEngine::resolveStaticCollisions() {
    for (auto& body : bodies) {
        if (body->isKinematic) continue;

        for (const auto& box : staticBoxes) {
            // Sphere center (body->position IS the sphere center)
            const glm::dvec3& c = body->position;
            const double      r = body->radius;

            // Closest point on AABB to sphere center
            glm::dvec3 closest = glm::clamp(c, box.bmin, box.bmax);
            glm::dvec3 diff    = c - closest;
            double dist = glm::length(diff);

            double penetration = r - dist;
            if (penetration <= 0.0) continue;

            glm::dvec3 normal;
            if (dist < 1e-9) {
                // Center is inside the box — push out on the minimum-penetration axis
                double px = std::min(c.x - box.bmin.x, box.bmax.x - c.x);
                double py = std::min(c.y - box.bmin.y, box.bmax.y - c.y);
                double pz = std::min(c.z - box.bmin.z, box.bmax.z - c.z);
                if (px <= py && px <= pz)
                    normal = (c.x < (box.bmin.x + box.bmax.x) * 0.5) ? glm::dvec3(-1,0,0) : glm::dvec3(1,0,0);
                else if (py <= px && py <= pz)
                    normal = (c.y < (box.bmin.y + box.bmax.y) * 0.5) ? glm::dvec3(0,-1,0) : glm::dvec3(0,1,0);
                else
                    normal = (c.z < (box.bmin.z + box.bmax.z) * 0.5) ? glm::dvec3(0,0,-1) : glm::dvec3(0,0,1);
                penetration = r + std::min({px, py, pz});
            } else {
                normal = diff / dist;
            }

            // Push body out of the box
            body->position += normal * penetration;

            // Cancel velocity component directed into the surface
            double vn = glm::dot(body->velocity, normal);
            if (vn < 0.0) body->velocity -= normal * vn;
        }
    }
}

double PhysicsEngine::calculateGravityAtPosition(const glm::dvec3& worldPos, glm::dvec3& outGravityDir) {
    if (!m_world) {
        outGravityDir = glm::dvec3(0.0, -1.0, 0.0);
        return 9.81;  // Default Earth gravity
    }

    // UNA sola implementación de la suma (core/planet/soi.h). Este bucle era la tercera copia de la
    // regla de gravedad del motor —con las dos de `stepCharacter` y los cuerpos dinámicos— y además la
    // única que no trataba el interior del cuerpo: `GM/r²` con r → 0 dispara a un objeto que atraviese
    // el suelo un frame, en vez de frenarlo como hace una esfera uniforme.
    //
    // ⚠️ `gravitationalConstant` (el miembro, con su setter público) deja de intervenir aquí. Hoy vale
    // exactamente `Planet::kGravConstant` y nadie llama al setter, así que el resultado es idéntico;
    // si algún día alguien lo cambia, el sitio a cambiar es la constante compartida — tener G en dos
    // sitios es exactamente el tipo de duplicado que este cambio viene a quitar.
    const glm::dvec3 totalGravity = Haruka::Planet::gravityAt(m_world->gravBodies(), worldPos);


    double magnitude = glm::length(totalGravity);
    if (magnitude > 1e-6) {
        outGravityDir = glm::normalize(totalGravity);
    } else {
        outGravityDir = glm::dvec3(0.0, -1.0, 0.0);
        magnitude = 0.0;
    }
    
    return magnitude;
}

glm::dvec3 PhysicsEngine::calculateGravityContribution(const GravBody& body, const glm::dvec3& worldPos) {
    // Calculate vector from test position to body
    glm::dvec3 delta = body.pos - worldPos;
    double distance = glm::length(delta);
    
    // Avoid division by zero
    if (distance < 1e-6) {
        return glm::dvec3(0.0);
    }
    
    // Newton's universal law of gravitation: F = G * m1 * m2 / r²
    // Acceleration: a = F / m1 = G * m2 / r²
    double acceleration = gravitationalConstant * body.mass / (distance * distance);
    glm::dvec3 direction = glm::normalize(delta);
    
    return direction * acceleration;
}

bool PhysicsEngine::checkTerrainCollision(
    const glm::dvec3& worldPos,
    glm::dvec3* outCollisionPoint,
    glm::dvec3* outCollisionNormal) {
    // (Fase 1) El ground-snap real irá aquí, usando m_world->terrainHeightAt. Hoy es un stub
    // (nunca se usaba: el ground se hacía analíticamente en el juego). Desacoplado ya del engine.
    (void)worldPos; (void)outCollisionPoint; (void)outCollisionNormal;
    return false;
}

void PhysicsEngine::applyGravity(const glm::dvec3& worldPos, double deltaTime, glm::dvec3& inOutVelocity) {
    glm::dvec3 gravityDir;
    double gravityMagnitude = calculateGravityAtPosition(worldPos, gravityDir);
    
    // Apply gravitational acceleration: v += a * dt
    glm::dvec3 gravityAcceleration = gravityDir * gravityMagnitude;
    inOutVelocity += gravityAcceleration * deltaTime;
}

// ── ALAMBRE DE LA MALLA DE COLISIÓN ─────────────────────────────────────────────────────────────
// Delegan en el JoltImpl. Sin Jolt compilado no hay malla que enseñar y devuelven false, que es la
// respuesta honesta: el solver a mano no construye una MeshShape.
void PhysicsEngine::setCollisionMeshDebug(bool on) {
#ifdef HARUKA_HAS_JOLT
    if (m_jolt) m_jolt->debugCaptureGround.store(on, std::memory_order_relaxed);
#else
    (void)on;
#endif
}

bool PhysicsEngine::isCollisionMeshDebug() const {
#ifdef HARUKA_HAS_JOLT
    return m_jolt && m_jolt->debugCaptureGround.load(std::memory_order_relaxed);
#else
    return false;
#endif
}

bool PhysicsEngine::getNearGroundRing(NearGroundRing& out) const {
#ifdef HARUKA_HAS_JOLT
    if (!m_jolt) return false;
    std::lock_guard<std::mutex> lk(m_jolt->debugMeshMx);
    if (!m_jolt->nearRingValid || m_jolt->nearRing.tris.size() < 3) return false;
    out = m_jolt->nearRing;
    return true;
#else
    (void)out; return false;
#endif
}

bool PhysicsEngine::getCollisionMeshDebug(std::vector<glm::dvec3>& outVerts,
                                          std::vector<uint32_t>& outTris,
                                          glm::dvec3& outCenter, uint64_t& outRevision) const {
#ifdef HARUKA_HAS_JOLT
    if (!m_jolt) return false;
    std::lock_guard<std::mutex> lk(m_jolt->debugMeshMx);
    // Los dos slots (cercano y lejano) se construyen por separado; aquí se unen en una sola malla,
    // reindexando el segundo. El consumidor ve un alambre, no dos.
    if (m_jolt->debugMeshTris[0].size() + m_jolt->debugMeshTris[1].size() < 3) return false;
    outVerts.clear(); outTris.clear();
    outVerts.reserve(m_jolt->debugMeshVerts[0].size() + m_jolt->debugMeshVerts[1].size());
    outTris.reserve(m_jolt->debugMeshTris[0].size() + m_jolt->debugMeshTris[1].size());
    for (int slot = 0; slot < 2; ++slot) {
        const uint32_t base = (uint32_t)outVerts.size();
        outVerts.insert(outVerts.end(), m_jolt->debugMeshVerts[slot].begin(),
                        m_jolt->debugMeshVerts[slot].end());
        for (uint32_t i : m_jolt->debugMeshTris[slot]) outTris.push_back(base + i);
    }
    outCenter   = m_jolt->debugMeshCenter;
    outRevision = m_jolt->debugMeshRevision;
    return true;
#else
    (void)outVerts; (void)outTris; (void)outCenter; (void)outRevision;
    return false;
#endif
}

}} // namespace Haruka::Physics
