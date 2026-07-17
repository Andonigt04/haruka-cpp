#include "planetary_system.h"
#include "tools/profiler.h"   // HARUKA_PROFILE (sub-scopes de planetary.update: lod.recompute / lod.stream)

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <utility>
#if defined(_WIN32)
  #include <windows.h>
#else
  #include <unistd.h>
#endif
#include <unordered_map>
#include <unordered_set>

namespace Haruka {
    // Umbrales del vigilante de memoria (ver update()). Por debajo de kLowMemoryMB el sistema está a
    // punto de swapear (y con él, de congelar el escritorio) → la caché de terreno suelta lastre.
    static constexpr size_t kLowMemoryMB   = 3072;  // por debajo: encoger la caché
    // Recuperar en cuanto haya una HISTÉRESIS de 512 MB sobre el suelo, no 2 GB por encima. Con el gap
    // anterior (5120) una máquina con 3-5 GB libres caía en una ZONA MUERTA: la caché ni encogía ni
    // crecía, se congelaba en el techo bajo con el que arrancó y no podía recuperar sitio para el LOD
    // fino → manta gruesa permanente. La protección anti-swap NO cambia (sigue encogiendo bajo 3072);
    // el crecimiento es lento (+256 MB/2 s) y se autolimita: al crecer, `avail` baja y frena.
    static constexpr size_t kAmpleMemoryMB = kLowMemoryMB + 512;  // 3584: por encima, recuperar poco a poco
    static size_t systemAvailableRAMMB();   // definido más abajo (junto a systemTotalRAMMB)
}
#include "core/modules.h"
#include "core/chunk_cache.h"
#include "core/lod_system.h"
#include "core/terrain/terrain_generator.h"
#include "core/terrain/terrain_streaming_system.h"
#ifdef HARUKA_MOD_DEFORM
#include "core/terrain/deformation_field.h"
#endif
#include "renderer/terrain_renderer.h"
#include "renderer/water_renderer.h"
#include "renderer/floating_island_renderer.h"
#include "renderer/texture.h"
#include "renderer/primitive_shapes.h"              // esfera base de la Luna
#include "core/components/mesh_renderer_component.h" // malla procedural de cuerpos
#include "core/terrain/gpu_heightfield.h"            // generador GPU (compute) + test paridad
#include "core/terrain/floating_islands.h"
#include "core/terrain/terrain_sampler_v2.h"
#include "settings/settings_manager.h"
#include <glad/glad.h>
#include <filesystem>
#include <chrono>

namespace Haruka {

PlanetarySystem::PlanetarySystem() {}
PlanetarySystem::~PlanetarySystem() { stopLODWorker(); }

void PlanetarySystem::init() {
    // Inicializamos los subsistemas una sola vez
    // 768 MB: el suelo de LOD mantiene el planeta entero (~1536 chunks base) residente
    // SIEMPRE; con 384 MB se evictaba y entraba en bucle recarga. Override por GraphicsSettings.
    m_cache = std::make_unique<ChunkCache>(768);
    // CACHÉ EN DISCO: evictar deja de condenar a REGENERAR. El directorio se fija con la seed del
    // primer planeta (ver addPlanet) → un mundo nuevo nunca lee chunks de otro.
    m_generator = std::make_unique<TerrainGenerator>();
    m_renderer = std::make_unique<TerrainRenderer>();
    m_waterRenderer = std::make_unique<WaterRenderer>();
    m_waterRenderer->setTerrainRenderer(m_renderer.get()); // agua sincronizada con el terreno
    // El agua espeja al terreno: cuando el terreno borra un chunk (descarga o purga
    // de stale), borramos su agua → agua y terreno cubren SIEMPRE lo mismo.
    m_renderer->setOnChunkRemoved([this](const PlanetChunkKey& k) {
        if (m_waterRenderer) m_waterRenderer->removeFromScene("", k);
    });
    m_islandRenderer = std::make_unique<FloatingIslandRenderer>();
#ifdef HARUKA_MOD_DEFORM
    m_deform = std::make_unique<DeformationField>();
    m_generator->setDeformationField(m_deform.get());
#endif
    
    // El streaming necesita a los otros tres
    m_streaming = std::make_unique<TerrainStreamingSystem>(*m_cache, *m_generator, *m_renderer);
    
    // El LOD es el que toma decisiones.
    // splitFactor 1.0: subdivide cuando la distancia a la superficie es menor que
    //   el tamaño del nodo (más detalle cerca de la cámara).
    // maxLOD: cada +1 nivel = chunks la mitad de grandes = 2x más definición
    //   cerca del jugador. 20 → chunk ~12 m (vs ~190 m a LOD16) = 16x más fino.
    //   Es barato porque solo subdivide donde está la cámara (rings concéntricos).
    //   Para más detalle súbelo (22 ≈ 64x, 23 ≈ 128x), pero el ruido debe tener
    //   frecuencia suficiente o los chunks pequeños salen lisos.
    m_lod = std::make_unique<LODSystem>(1.0, 20);
    // Instancia GEMELA para el worker async (misma config inicial). El worker es el único que
    // la toca; el main le espeja la config por job. Ver setAsyncLOD / lodWorkerLoop.
    m_lodAsync = std::make_unique<LODSystem>(1.0, 20);
    // Si el modo async viene ACTIVADO por defecto, hay que arrancar el worker AQUÍ: setAsyncLOD(true)
    // no lo haría (early-return al no cambiar el flag) y el main esperaría resultados eternamente.
    // HARUKA_LODASYNC=1 fuerza el modo async (para reproducir/depurar sin recompilar).
    if (getenv("HARUKA_LODASYNC")) m_asyncLOD.store(true, std::memory_order_relaxed);
    if (m_asyncLOD.load(std::memory_order_relaxed)) startLODWorker();
}

// ---------------------------------------------------------------------------------------------
// LOD asíncrono: hilo worker + arranque/cierre.
// ---------------------------------------------------------------------------------------------
void PlanetarySystem::startLODWorker() {
    if (m_lodThread.joinable()) return; // ya arrancado
    m_lodStop = false;
    m_lodThread = std::thread([this] { lodWorkerLoop(); });
}

void PlanetarySystem::stopLODWorker() {
    if (!m_lodThread.joinable()) return;
    { std::lock_guard<std::mutex> lk(m_lodMx); m_lodStop = true; }
    m_lodCv.notify_all();
    m_lodThread.join();
}

void PlanetarySystem::setAsyncLOD(bool on) {
    if (on == m_asyncLOD.load(std::memory_order_relaxed)) return;
    if (on) {
        startLODWorker();
        m_asyncLOD.store(true, std::memory_order_relaxed);
    } else {
        // Apagar: drena el worker (espera a que termine el job en vuelo) y descarta su
        // resultado, para que el siguiente frame vuelva al recompute inline limpio.
        m_asyncLOD.store(false, std::memory_order_relaxed);
        std::unique_lock<std::mutex> lk(m_lodMx);
        m_lodCv.wait(lk, [this] { return !m_lodBusy; });
        m_lodResults.clear();
        m_lodResultReady = false;
        m_lodJobReady = false;
    }
    m_forceLOD = true; // fuerza un recompute limpio tras el cambio de modo
}

// Bucle del worker: duerme hasta que el main deja un job (m_lodJobReady), lo copia bajo el
// mutex (para no retener el lock durante el cálculo), recomputa CADA planeta sobre m_lodAsync
// y publica los LODUpdate. TODO es CPU puro (el LOD no llama a GL) y opera sobre snapshots
// (residencia/cámara/config) → sin tocar nada del main mientras calcula.
void PlanetarySystem::lodWorkerLoop() {
    for (;;) {
        LODAsyncJob job;
        {
            std::unique_lock<std::mutex> lk(m_lodMx);
            m_lodCv.wait(lk, [this] { return m_lodStop || m_lodJobReady; });
            if (m_lodStop) return;
            job = std::move(m_lodJob);       // toma el job y libera el slot
            m_lodJobReady = false;
            m_lodBusy = true;
        }

        // Espeja la config del job en la instancia del worker (sin locks: solo el worker la toca).
        m_lodAsync->setParams(job.splitFactor, job.maxLOD);
        m_lodAsync->setMinLOD(job.minLOD);
        m_lodAsync->setScreenSpaceLOD(job.screenSpace);
        m_lodAsync->setScreenK(job.screenK);
        m_lodAsync->setTargetPx(job.targetPx);
        if (job.hasCull) m_lodAsync->setCullMatrix(job.cullVP);
        for (const auto& k : job.forgetKeys) m_lodAsync->forgetChunk(k);

        // Residencia: snapshot lock-free (mismo patrón que el main). Copiado en el job.
        auto residentFn = [&job](const PlanetChunkKey& k) {
            return job.resident.count(ChunkCache::keyToHash(k)) != 0;
        };

        std::vector<std::pair<size_t, LODUpdate>> results;
        results.reserve(job.planets.size());
        for (const auto& p : job.planets) {
            // Bias de costa PER-PLANET (cada cuerpo tiene su seed → su línea de mar).
            if (p.coastFn) m_lodAsync->setCoastBias(p.coastFn, p.coastRefine);
            else           m_lodAsync->setCoastBias(nullptr);
            SceneObject proxy;
            proxy.name = p.name;
            proxy.position = p.position;
            proxy.scale = glm::dvec3(p.radius);
            LODUpdate up = m_lodAsync->updatePlanetLOD(
                std::shared_ptr<SceneObject>(&proxy, [](SceneObject*){}), p.camPos, residentFn, p.bodyId);
            results.emplace_back(p.index, std::move(up));
        }

        {
            std::lock_guard<std::mutex> lk(m_lodMx);
            m_lodResults = std::move(results);
            m_lodResultReady = true;
            m_lodBusy = false;
        }
        m_lodCv.notify_all(); // por si setAsyncLOD(false) espera a que drene
    }
}

void PlanetarySystem::update(double dt, const glm::dvec3& cameraPos) {
    m_simulationTime += dt;

    // LOD ADAPTATIVO: ajusta el targetPx hacia la mejor calidad que el frame sostiene. Afina
    // mientras hay holgura de tiempo, engorda solo si se pasa del presupuesto → el LOD degrada
    // SOLO bajo carga (petición del usuario: "mejor calidad según hardware"). Reacciona al tiempo
    // de frame TOTAL, así se auto-corrige aunque el cuello no sea el terreno.
    if (m_adaptiveLOD && m_lodBudgetMs > 0.0 && m_lod && dt > 1e-5) {
        double frameMs = glm::clamp(dt * 1000.0, 0.5, 200.0);          // recorta outliers (hitches/pausa)
        // EWMA LENTA (0.04): promedia ~varios segundos → NO reacciona a picos transitorios (un giro,
        // un frame de streaming). Solo la carga SOSTENIDA mueve el LOD → el detalle no "respira" al girar.
        m_lodEwmaMs = (m_lodEwmaMs <= 0.0) ? frameMs : glm::mix(m_lodEwmaMs, frameMs, 0.04);
        m_lodAdjustAccum += dt;
        if (m_lodAdjustAccum >= 1.0) {                                 // ajusta 1×/s
            m_lodAdjustAccum = 0.0;
            double px = m_lod->getTargetPx();
            const double b = m_lodBudgetMs;
            // Histéresis ANCHA (±30%): solo ajusta con desvío claro y sostenido → sin oscilación fina.
            if      (m_lodEwmaMs > b * 1.30) px *= 1.06;               // muy pasado → engorda un poco
            else if (m_lodEwmaMs < b * 0.70) px *= 0.95;               // holgura clara → afina
            px = glm::clamp(px, m_lodMinPx, m_lodMaxPx);
            if (std::abs(px - m_lod->getTargetPx()) > 0.5) { m_lod->setTargetPx(px); m_forceLOD = true; }
        }
    }

    // PREDICCIÓN DE MOVIMIENTO (lookahead): estima la velocidad de la cámara y evalúa
    // el LOD/streaming en un punto ADELANTADO → los chunks se generan ANTES de llegar
    // (menos pop-in al moverse/volar rápido y en el fly-in cinemático). El render usa
    // la cámara REAL; solo el LOD mira 'lodCamPos'. Tope de distancia para no
    // sobre-subdividir lejísimos; a baja velocidad (caminando) el adelanto es ~0.
    glm::dvec3 lodCamPos = cameraPos;
    if (m_havePrevCam && dt > 1e-5) {
        glm::dvec3 inst = (cameraPos - m_prevCamPos) / dt;       // m/s instantánea
        m_camVel = glm::mix(m_camVel, inst, 0.25);               // EMA (suaviza ruido/saltos)
        glm::dvec3 ahead = m_camVel * 0.4;                       // 0.4 s de anticipación
        // Tope ABSOLUTO y pequeño (4 km): el lookahead es para suavizar el pop-in al
        // moverse a ras de suelo, NO para descensos orbitales. Escalarlo con la altitud
        // hacía que en la órbita/descenso pidiera detalle a cientos de km de donde estás
        // → sobrecarga y HUECOS. Para el fly-in rápido, la solución es el .cine
        // escalonado (pausa baja), no adelantar el LOD.
        const double maxAhead = 4000.0;
        double al = glm::length(ahead);
        if (al > maxAhead) ahead *= (maxAhead / al);
        lodCamPos = cameraPos + ahead;
    }
    m_prevCamPos = cameraPos; m_havePrevCam = true;

    // 1. Mover los planetas en sus órbitas
    { HARUKA_PROFILE("lod.orbits"); updateOrbits(dt); }

    // Keep the per-planet LOD-throttle state sized to the planet list.
    if (m_lastLODCamPos.size() != m_planets.size())
        m_lastLODCamPos.assign(m_planets.size(), glm::dvec3(1e300));
    if (m_lastUpdates.size() != m_planets.size())
        m_lastUpdates.assign(m_planets.size(), LODUpdate{});
    if (m_lastLODFrame.size() != m_planets.size())
        m_lastLODFrame.assign(m_planets.size(), -1000);

    // Islas flotantes (genVersion>=2): streaming-lite. Regenera el casquete de
    // islas alrededor de la cámara cuando esta se mueve lo bastante → islas
    // descubribles al explorar, sin generar millones en todo el planeta.
    if (m_islandRenderer) {
        HARUKA_PROFILE("lod.islands");
        for (const auto& planet : m_planets) {
            const auto& cfg = planet.terrainSettings.contains("config")
                            ? planet.terrainSettings["config"] : planet.terrainSettings;
            if (cfg.value("genVersion", 1) < 2) continue;
            glm::dvec3 up = cameraPos - glm::dvec3(planet.position);
            double ul = glm::length(up);
            if (ul < 1.0) break;
            glm::dvec3 camDir = up / ul;
            const float angRadius = 0.03f; // ~190 km de casquete
            if (m_islandsGenerated &&
                glm::dot(camDir, m_islandGenCamDir) > std::cos(angRadius * 0.4)) break; // apenas se movió
            uint32_t seed = (uint32_t)cfg.value("seed", 42);
            WorldGenParams W = deriveWorldParams(seed, planet.radius);
            W.reliefStrength = cfg.value("reliefStrength", 1.0f);
            auto islands = generateFloatingIslandsNear(seed, glm::dvec3(planet.position),
                                                       planet.radius, W, camDir, angRadius);
            m_islandRenderer->setIslands(std::move(islands));
            m_islandGenCamDir  = camDir;
            m_islandsGenerated = true;
            break; // un planeta con islas por ahora
        }
    }

    // (Los props/recursos los genera ahora el JUEGO — Survival ResourceSystem — usando
    //  getActivePlanet() + el sampler de terreno del motor.)

    // 2. Para cada planeta, actualizar su terreno
    static int s_lf = 0; ++s_lf; // contador de frames (para escalonar el catch-up)

    // VIGILANTE DE MEMORIA (cada ~2 s). El presupuesto de la caché se fija al arrancar, pero lo que
    // el SISTEMA tiene libre cambia MIENTRAS JUEGAS (el navegador crece, otro programa arranca). Sin
    // esto, la caché sigue llenándose hasta su techo, el sistema entra en swap y **se congela el PC
    // entero**, no solo el juego. El terreno es REGENERABLE: ante la duda, se tira caché.
    // (m_cacheBudgetMB == 0 = nadie fijó el techo → no hay nada a lo que volver: sin vigilante.)
    if (m_cache && m_cacheBudgetMB > 0 && (s_lf % 120) == 0) {
        const size_t avail = systemAvailableRAMMB();
        if (avail) {
            const size_t cur  = m_cache->getMaxMemoryMB();
            const size_t used = m_cache->getMemoryUsageMB();

            // ⚠️ SUELO = EL CONJUNTO DE TRABAJO, no una constante. Los chunks que el LOD tiene
            // residentes AHORA no son lastre: si el techo baja de ahí, la caché los desaloja y el
            // streaming los regenera acto seguido → thrash permanente (visto: `mem 1359/512 MB`, la
            // caché 2.6× por encima de su techo, el frame a 27 ms y `stream.sort` a 9 ms). Encoger
            // solo tiene sentido por ENCIMA de lo que el jugador está mirando.
            const size_t chunks   = std::max<size_t>(1, m_cache->getChunkCount());
            const size_t avgKB    = (used * 1024) / chunks;
            const size_t resident = (size_t)std::max(0, getGPUChunkCount());
            const size_t workingMB = (resident * avgKB) / 1024;
            const size_t hi       = std::max<size_t>(512, m_cacheBudgetMB); // el techo puede ser < 512 (manual)
            const size_t floorMB  = std::clamp<size_t>(workingMB * 5 / 4, 512, hi);

            if (avail < kLowMemoryMB && cur > floorMB) {              // apretando → soltar lastre
                const size_t next = std::max(floorMB, cur * 3 / 4);
                m_cache->setMaxMemory(next);                          // desaloja al bajar el techo
                fprintf(stderr, "[ChunkCache] memoria del sistema baja (%zu MB libres) → cache %zu→%zu MB "
                                "(conjunto de trabajo ~%zu MB)\n", avail, cur, next, workingMB);
            } else if (avail > kAmpleMemoryMB && cur < m_cacheBudgetMB) { // holgura → recuperar
                m_cache->setMaxMemory(std::min<size_t>(m_cacheBudgetMB, cur + 256));
            }
        }
    }
    // F7 — PRESUPUESTO DE SUBIDAS POR FRAME: subir un chunk (terreno o agua) = copia
    // pesada + buffers GPU. En una ráfaga (carga inicial / fly-in) eran CIENTOS por frame
    // → pico de >1500 ms. Limitamos cuántos se suben por frame (terreno + agua comparten
    // este presupuesto); el resto entra en frames siguientes → carga repartida, sin picos.
    // 24 (no 48): cada subida = VAO+5 VBOs terreno (+ malla de agua) ≈ 1–3 ms en mesa/AMD.
    // 24/frame mantiene el peor frame del fly-in bajo control (~1 frame) sin que se note la
    // carga repartida (24×60 = 1440 chunks/s → la Tierra ~1000 entra en <1 s).
    // TIME-BOX de subidas (antes: count=40): cada subida cuesta 2–3 ms (VAO+VBOs+malla de
    // agua), así que un count de 40 producía picos de ~100 ms (los stalls de 112 ms que medía
    // el profiler en planetary.update al entrar en zona nueva). Ahora acotamos por TIEMPO de
    // pared: hasta 5 ms de subidas por frame (y un techo de count alto como salvaguarda). El
    // resto del backlog entra en frames siguientes → sin picos, carga repartida. 5 ms mantiene
    // el frame por debajo de ~16 ms incluso subiendo; el agua comparte el mismo presupuesto.
    if (m_streaming) m_streaming->beginUploadFrame(/*count*/256, /*msBudget*/5.0);
    // Si hay chunks generándose/encolados, mantén la ventana de catch-up abierta (~1 s).
    if (m_streaming && (m_streaming->getPendingCount() + m_streaming->getQueuedCount()) > 0)
        m_catchupGrace = 60;
    else if (m_catchupGrace > 0) --m_catchupGrace;
    // ¿Recalculó el LOD de cada planeta ESTE frame? En throttle (cámara quieta o entre recomputes)
    // las hojas deseadas NO cambian → no hace falta re-enviarlas al renderer (evita invalidar el
    // draw-set cacheado y reconstruirlo idéntico). Solo re-enviamos donde recompute realmente corrió.
    std::vector<char> leavesChanged(m_planets.size(), 0);
    // Snapshot de la residencia del AGUA UNA vez/frame (un solo lock) → los catch-up consultan miles
    // de chunks SIN re-bloquear el mutex por chunk (era el grueso de lod.catchup: 5.89ms). addToScene
    // es idempotente, así que el snapshot puede quedar algo obsoleto dentro del frame sin problema.
    static std::unordered_set<uint64_t> s_waterResident, s_waterNoOcean;
    if (m_waterRenderer) {
        m_waterRenderer->residentHashes(s_waterResident);
        m_waterRenderer->noWaterHashes(s_waterNoOcean); // chunks de tierra → saltar (no re-copiar)
    }
    // El catch-up salta un chunk si su agua YA está en GPU o si NO tiene agua (tierra). Sin el 2º,
    // reintentaba getChunkCopy+addToScene sobre cada chunk de tierra cada 4 frames (churn = lod.catchup).
    auto waterSkip = [&](const PlanetChunkKey& k) {
        uint64_t h = ChunkCache::keyToHash(k);
        return s_waterResident.count(h) != 0 || s_waterNoOcean.count(h) != 0;
    };
    // Catch-up de throttle: sube a GPU el backlog de terreno/agua ya generado SIN recomputar el
    // LOD (idempotente, filtra por residencia). Reutilizado por el modo async cuando el worker
    // aún no ha publicado un resultado para este planeta.
    // CATCH-UP del agua. ANTES: recorría los MILES de `chunksToKeep` de CADA planeta cada 4 frames
    // solo para encontrar los pocos que faltaban por subir → era el cuello del frame (5.5 ms ×3).
    // AHORA: una COLA por planeta que se rellena cuando cambian las hojas y se DRENA con presupuesto
    // → coste O(lo que subes), no O(todo el mundo). Y los cuerpos SIN OCÉANO (Luna, Júpiter) ni
    // siquiera entran: no tienen agua que subir.
    auto planetHasOcean = [](const Planet& p) {
        const auto& cfg = p.terrainSettings.contains("config") ? p.terrainSettings["config"]
                                                               : p.terrainSettings;
        return cfg.value("profile", std::string("terran")) == "terran";
    };
    auto lodCatchUp = [&](size_t pi, const Planet& planet) {
        if (m_catchupGrace > 0 && (s_lf % 3) == 0)
            m_streaming->processLODUpdate(m_lastUpdates[pi]); // terreno (filtra por residencia + presupuesto)
        if (!m_waterRenderer || !m_cache) return;
        if (!planetHasOcean(planet)) return;                  // sin océano → nada que poner al día
        if (pi >= m_waterBacklog.size()) m_waterBacklog.resize(m_planets.size());
        auto& q = m_waterBacklog[pi];
        if (q.empty()) return;                                // nada pendiente → coste CERO
        int budget = 24;                                      // tope por frame (no barre el mundo)
        while (!q.empty() && budget-- > 0) {
            const PlanetChunkKey k = q.front(); q.pop_front();
            if (waterSkip(k)) continue;                       // ya en GPU (snapshot, sin lock)
            if (!m_streaming->tryConsumeUpload()) { q.push_front(k); break; } // presupuesto agotado
            ChunkData wd;
            if (m_cache->getChunkCopy(k, wd)) m_waterRenderer->addToScene(planet.name, k, wd);
        }
    };
    // BIAS DE COSTA de un planeta (la orilla se subdivide más → sin línea de agua dentada). Solo
    // para genVersion>=2 perfil terran; devuelve función vacía si no aplica. W cacheada por seed.
    // Factorizado para reusarlo en el path SÍNCRONO (aplica a m_lod) y en el ASYNC (va al job).
    auto computeCoastFn = [&](const Planet& planet) -> std::function<double(const glm::dvec3&)> {
        const auto& cfg = planet.terrainSettings.contains("config") ? planet.terrainSettings["config"] : planet.terrainSettings;
        if (cfg.value("genVersion", 1) >= 2 && cfg.value("profile", std::string("terran")) == "terran") {
            const uint32_t seed = (uint32_t)cfg.value("seed", 42);
            const double   rad  = planet.radius;
            static std::unordered_map<uint32_t, WorldGenParams> s_wpCache;
            auto it = s_wpCache.find(seed);
            if (it == s_wpCache.end()) {
                WorldGenParams W = deriveWorldParams(seed, rad);
                W.reliefStrength = cfg.value("reliefStrength", 1.0f); W.profile = 0;
                it = s_wpCache.emplace(seed, W).first;
            }
            const WorldGenParams W = it->second;
            return [W, rad](const glm::dvec3& dir){ return (double)coastDistanceMeters(glm::vec3(dir), W, rad); };
        }
        return {};
    };
    // MODO ASYNC: recoge de una vez el lote de LODUpdate que el worker publicó (de un kick
    // anterior). Se consume por planeta más abajo; lo que toque recomputar se acumula en
    // asyncKick y se despacha como UN job tras el loop (si el worker está libre).
    const bool asyncOn = m_asyncLOD.load(std::memory_order_relaxed);
    std::unordered_map<size_t, LODUpdate> asyncReady;
    std::vector<LODAsyncPlanet>           asyncKick;
    if (asyncOn) {
        std::lock_guard<std::mutex> lk(m_lodMx);
        if (m_lodResultReady) {
            for (auto& r : m_lodResults) asyncReady.emplace(r.first, std::move(r.second));
            m_lodResults.clear();
            m_lodResultReady = false;
        }
    }
    for (size_t pi = 0; pi < m_planets.size(); ++pi) {
        auto& planet = m_planets[pi];

        // Throttle: el quadtree solo cambia al cruzar una frontera de split, así
        // que saltamos el rebuild (con allocs) cuando la cámara apenas se movió
        // desde el último recompute. Los chunks ya en GPU siguen renderizándose.
        const double finest = (planet.radius * 2.0) / double(1u << 16);
        const double moveThresh = std::max(0.5, finest * 0.125);
        const double moved = glm::length(lodCamPos - m_lastLODCamPos[pi]);
        // Si la última pasada quedó limitada por residencia (carga progresiva a medias),
        // NO aplicamos throttle: hay que recalcular para subdividir el siguiente nivel a
        // medida que el grueso llega. Sin esto, con la cámara quieta el planeta se queda
        // en grueso para siempre (p.ej. 6 caras raíz al spawn).
        // Mientras la carga progresiva esté a medias, recalcula para afinar — pero solo
        // cada 4 frames (no cada uno) para repartir el coste del rebuild del quadtree.
        // TAMBIÉN refinamos mientras haya chunks EN VUELO (catchupGrace>0, se pone a 60 cuando
        // hay pendientes/encolados): sin esto, la cascada grueso→fino se ESTANCABA entre oleadas
        // con la cámara quieta (residencyLimited parpadea a false al llegar un nivel, antes de
        // pedir el siguiente) → al spawnear estático aparecías con COLISIÓN pero SIN VISUAL hasta
        // que el cine movía la cámara y forzaba el recompute. Ahora la cascada se completa sola.
        const bool refining = (pi < m_lastUpdates.size() && m_lastUpdates[pi].residencyLimited)
                           || (m_catchupGrace > 0);
        const bool refineNow = refining && ((s_lf % 4) == 0);
        // THROTTLE TEMPORAL: al moverte a ritmo normal el LOD recalculaba CADA frame (rebuild del
        // quadtree con allocs por nodo = hasta ~14ms → el muro de planetary.update moviéndote).
        // Recalcula como mucho cada 2 frames; en un SALTO grande (teleport/descenso) recalcula ya.
        // Los chunks residentes se siguen dibujando entre recomputes → pop-in mínimo, ~mitad de coste.
        const bool bigJump  = moved > moveThresh * 16.0;
        const bool frameDue = (s_lf - m_lastLODFrame[pi]) >= 2;
        // GIRO DE CÁMARA: el LOD refina SOLO dentro del frustum (F2), así que al ROTAR (sin
        // trasladar) la zona recién revelada se queda con el set del recompute viejo → grueso/
        // vacío = "el terreno desaparece al mover la cámara". El recompute ya es baratísimo
        // (~1ms, sin allocs), así que recalculamos también cuando la vista cambia lo bastante.
        float dVP = 0.0f;
        for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b)
            dVP += std::fabs(m_curCullVP[a][b] - m_lastRecomputeCullVP[a][b]);
        const bool viewChanged = dVP > 0.02f;
        // THROTTLE del recompute (incluido el GIRO): antes `viewChanged` disparaba recompute CADA
        // frame (el comentario asumía ~1ms). Con miles de chunks de costa el recompute es ~11ms →
        // rotar/moverte reconstruía el quadtree ENTERO cada frame = el muro de planetary.update
        // (31ms/32fps). Ahora TAMBIÉN el giro respeta el intervalo de 2 frames (frameDue); el
        // guard-band de 12° (recursiveProcess) ya mantiene residentes los chunks justo fuera de vista,
        // así que 1 frame de latencia al girar no revela terreno grueso/vacío. bigJump/forceLOD siguen
        // recalculando de inmediato (teleport/descenso).
        const bool doRecompute = m_forceLOD || refineNow || bigJump
                              || ((viewChanged || moved >= moveThresh) && frameDue);
        // El recompute del LOD (updatePlanetLOD) es el pico de CPU del frame. En modo ASYNC va al
        // worker; aquí solo consumimos su resultado y decidimos si encolar el siguiente. En modo
        // SÍNCRONO se ejecuta inline (comportamiento previo, idéntico).
        LODUpdate update;
        if (asyncOn) {
            // (1) ¿Toca recompute? → acumula este planeta para el JOB (se despacha tras el loop si
            //     el worker está libre). El marcado de throttle se hace AL DESPACHAR (no aquí): si
            //     el worker estaba ocupado no lo despachamos y hay que reintentarlo el frame siguiente.
            if (doRecompute)
                asyncKick.push_back(LODAsyncPlanet{
                    pi, planet.name, planet.position, planet.radius, lodCamPos, (uint16_t)pi,
                    computeCoastFn(planet), 0.4 });
            // (2) ¿El worker publicó hojas nuevas para este planeta?
            auto itR = asyncReady.find(pi);
            if (itR == asyncReady.end()) {
                // Aún no → sube el backlog ya generado (idempotente) y sigue, como en throttle. Los
                // chunks residentes se siguen dibujando con el último set → sin huecos entre kicks.
                HARUKA_PROFILE("lod.catchup");
                lodCatchUp(pi, planet);
                continue;
            }
            update = std::move(itR->second); // resultado del worker → cae al streaming (GL) de abajo
        } else {
            if (!doRecompute) {
                HARUKA_PROFILE("lod.catchup"); // subida de backlog de agua (crea mallas GL) en throttle
                // Cámara quieta: NO recalculamos el LOD (caro). Solo subimos lo recién generado
                // (idempotente, filtra por residencia). Sin esto los chunks no aparecen hasta moverte;
                // haciéndolo cada frame costaba ~45 ms (copiaba todo el agua siempre).
                lodCatchUp(pi, planet);
                continue;
            }
            m_lastLODCamPos[pi] = lodCamPos;
            m_lastLODFrame[pi]  = s_lf;   // marca el frame de este recompute (throttle temporal)
            m_lastRecomputeCullVP = m_curCullVP; // vista con la que se refinó → detecta giros posteriores

            SceneObject planetProxy;                 // pila, sin make_shared por frame
            planetProxy.name = planet.name;
            planetProxy.position = planet.position;
            planetProxy.scale = glm::dvec3(planet.radius);

            // A. ¿Qué chunks deben verse? Predicado de residencia → carga PROGRESIVA grueso→fino
            //    (sin huecos negros). SNAPSHOT del set residente UNA vez (un solo lock) → el LOD lo
            //    consulta miles de veces sin re-bloquear el mutex por nodo (antes: ~900 ms por contención).
            static std::unordered_set<uint64_t> s_resident; // reusa buffer entre frames
            if (m_renderer) m_renderer->residentHashes(s_resident);
            auto residentFn = [](const PlanetChunkKey& k) { return s_resident.count(ChunkCache::keyToHash(k)) != 0; };
            // BIAS DE COSTA del LOD para ESTE planeta (orilla más fina, sin línea de agua dentada).
            auto coastFn = computeCoastFn(planet);
            if (coastFn) m_lod->setCoastBias(coastFn, 0.4); else m_lod->setCoastBias(nullptr);
            // bodyId = índice del planeta (estable en la sesión) → estampado en todas las claves del
            // cuerpo para que Tierra/Luna/Júpiter no colisionen en caché/renderer.
            HARUKA_PROFILE("lod.recompute"); // recursiveProcess + balanceo 2:1 (explota si hay mucha costa)
            update = m_lod->updatePlanetLOD(
                std::shared_ptr<SceneObject>(&planetProxy, [](SceneObject*){}), lodCamPos, residentFn, (uint16_t)pi);
        }
        m_lastUpdates[pi] = update; // para el catch-up en throttle
        // Hojas nuevas → RELLENA la cola de agua de este planeta (en vez de re-escanear cada frame).
        if (m_waterRenderer && planetHasOcean(planet)) {
            if (pi >= m_waterBacklog.size()) m_waterBacklog.resize(m_planets.size());
            auto& q = m_waterBacklog[pi];
            q.clear();
            for (const auto& k : update.chunksToKeep)
                if (!waterSkip(k)) q.push_back(k);
        }
        leavesChanged[pi] = 1;      // hojas nuevas → re-enviar
        HARUKA_PROFILE("lod.stream"); // resto de la iteración: sort + setDesiredChunks + processLODUpdate + agua + unload

        // B. Generar chunks nuevos (async) pasando settings del planeta
        nlohmann::json streamSettings = planet.terrainSettings;
        streamSettings["radius"]      = planet.radius;
        streamSettings["planetName"]  = planet.name;
        streamSettings["planetOffsetX"] = planet.position.x;
        streamSettings["planetOffsetY"] = planet.position.y;
        streamSettings["planetOffsetZ"] = planet.position.z;

        // Ordenar los chunks deseados de MÁS CERCANO a más lejano al jugador, para
        // que el streaming los genere/cargue en ese orden (cercanos primero).
        {
            HARUKA_PROFILE("stream.sort");
            const glm::dvec3 pPos = planet.position;
            const double     pRad = planet.radius;
            std::vector<std::pair<double, PlanetChunkKey>> byDist;
            byDist.reserve(update.chunksToLoad.size());
            for (const auto& k : update.chunksToLoad) {
                const double cpa = std::pow(2.0, (double)k.lod); // chunks por eje en este LOD
                const double u = (double(k.x) + 0.5) / cpa;
                const double v = (double(k.y) + 0.5) / cpa;
                const glm::dvec3 c = m_lod->getCubeToSpherePos(k.face, u, v, pRad) + pPos;
                const glm::dvec3 d = c - lodCamPos; // orden respecto al punto adelantado
                byDist.emplace_back(glm::dot(d, d), k);
            }
            std::sort(byDist.begin(), byDist.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            std::vector<PlanetChunkKey> sorted;
            sorted.reserve(byDist.size());
            for (auto& p : byDist) sorted.push_back(p.second);
            { HARUKA_PROFILE("stream.setDesired"); m_streaming->setDesiredChunks(std::move(sorted), streamSettings); }
        }

        // C. Cargar desde caché a GPU (la descarga es DIFERIDA, abajo)
        { HARUKA_PROFILE("stream.processLOD"); m_streaming->processLODUpdate(update); }

        // El streaming solo sube el TERRENO desde caché. El AGUA debe seguir la
        // MISMA vida que el terreno (subirse desde caché aquí también), o al
        // recargar un chunk vuelve el terreno pero no el agua → mallas de agua
        // desincronizadas/huérfanas que se solapan (los "blobs"). addToScene se
        // auto-salta si ya está, así que reañadir es barato.
        if (m_waterRenderer && m_cache) {
            HARUKA_PROFILE("stream.water");
            for (const auto& k : update.chunksToLoad) {
                if (waterSkip(k)) continue;                     // ya en GPU → gratis (snapshot, sin lock)
                if (!m_streaming->tryConsumeUpload()) break;         // presupuesto de frame agotado
                ChunkData wd;
                if (m_cache->getChunkCopy(k, wd))
                    m_waterRenderer->addToScene(planet.name, k, wd);
            }
        }

        // Lo que sale de vista NO se borra: se marca STALE (sigue dibujándose). El
        // renderer lo retira solo cuando su área queda cubierta por el reemplazo
        // (padre al fusionar, o los 4 hijos al subdividir) vía purgeStaleCoveredBy.
        // Así nunca hay un agujero entre quitar el viejo y subir el nuevo — el mismo
        // mecanismo "sin hueco" que ya usaba el dig, ahora general para todo el LOD.
        {
            HARUKA_PROFILE("stream.unload");
            for (const auto& k : update.chunksToUnload) {
                m_renderer->markStale(k);
                if (m_waterRenderer) m_waterRenderer->markStale(k);
            }
        }
    }

    // === Despacho del JOB async: si hay planetas a recomputar y el worker está LIBRE, le pasamos
    //     el lote (config espejo de m_lod + snapshot de residencia + forgets). El main NUNCA bloquea
    //     esperando al worker: si está ocupado, los kicks se reintentan el frame siguiente. El estado
    //     de throttle se marca SOLO para los planetas que entran al job (cámara = AHORA).
    bool asyncDispatched = false;
    if (asyncOn && !asyncKick.empty()) {
        std::unique_lock<std::mutex> lk(m_lodMx);
        if (!m_lodBusy && !m_lodJobReady) {
            for (const auto& p : asyncKick) {
                m_lastLODCamPos[p.index] = p.camPos;
                m_lastLODFrame[p.index]  = s_lf;
            }
            m_lastRecomputeCullVP = m_curCullVP;
            m_lodJob = LODAsyncJob{};
            m_lodJob.planets     = std::move(asyncKick);
            m_lodJob.splitFactor = m_lod->getSplitFactor();
            m_lodJob.maxLOD      = m_lod->getMaxLOD();
            m_lodJob.minLOD      = m_lod->getMinLOD();
            m_lodJob.screenSpace = m_lod->getScreenSpaceLOD();
            m_lodJob.screenK     = m_curScreenK;
            m_lodJob.targetPx    = m_lod->getTargetPx();
            m_lodJob.cullVP      = m_curCullVP; m_lodJob.hasCull = true;
            m_lodJob.forgetKeys  = std::move(m_pendingForget); m_pendingForget.clear();
            if (m_renderer) m_renderer->residentHashes(m_lodJob.resident); // snapshot COPIADO al job (worker lo lee lock-free)
            m_lodJobReady = true;
            asyncDispatched = true;
        }
    }
    if (asyncDispatched) m_lodCv.notify_one();
    // m_forceLOD: en async solo se limpia si el kick forzado LLEGÓ a despacharse (worker ocupado →
    // reintentar el frame siguiente). En sync (o sin nada que encolar) se limpia siempre.
    if (!asyncOn || asyncDispatched || asyncKick.empty()) m_forceLOD = false;

    // Pasa al renderer el set de HOJAS deseadas de cada planeta (keep ∪ load del último LOD).
    // El render dibuja, por hoja, el chunk residente más fino (hoja o ancestro) → desacopla el
    // detalle de tener la cadena entera residente. Se hace CADA frame (también en throttle, con
    // el último update) para que el set esté siempre vigente.
    if (m_renderer) {
        HARUKA_PROFILE("lod.desired"); // setDesiredLeaves (+ closure/leaf/int sets) por planeta
        for (size_t pi = 0; pi < m_lastUpdates.size(); ++pi) {
            const auto& up = m_lastUpdates[pi];
            if (up.planetName.empty()) continue;
            // Solo re-enviar si el LOD recalculó este frame (hojas nuevas). En throttle las hojas
            // son idénticas a las ya enviadas → re-enviarlas invalidaría el draw-set cacheado del
            // renderer para reconstruirlo IGUAL. El renderer ya conserva el último set.
            if (pi < leavesChanged.size() && !leavesChanged[pi]) continue;
            std::vector<PlanetChunkKey> leaves;
            leaves.reserve(up.chunksToKeep.size() + up.chunksToLoad.size());
            leaves.insert(leaves.end(), up.chunksToKeep.begin(), up.chunksToKeep.end());
            leaves.insert(leaves.end(), up.chunksToLoad.begin(), up.chunksToLoad.end());
            { HARUKA_PROFILE("desired.water");
              if (m_waterRenderer) m_waterRenderer->setDesiredLeaves(up.planetName, leaves); } // copia
            { HARUKA_PROFILE("desired.terrain");
              m_renderer->setDesiredLeaves(up.planetName, std::move(leaves)); }
        }
    }

    // Despachar generación pendiente (cercanos primero) CADA frame, aunque el LOD
    // esté en throttle por estar quieto → los chunks lejanos siguen entrando.
    { HARUKA_PROFILE("lod.pump"); m_streaming->pump(); }

    // 3. Recoger chunks terminados y subirlos a la GPU — SOLO si siguen deseados.
    //    Un chunk cuya generación async terminó DESPUÉS de salir de la vista ya fue
    //    "descargado" por el diff del LOD mientras se generaba; si lo subiéramos
    //    igual quedaría en GPU para siempre (terreno apilado + coste creciente).
    //    Se queda en la caché y reaparece vía processLODUpdate si vuelves.
    //
    //    OJO: NO usamos m_lod->isVisible aquí. Ese set (m_currentFrameChunks) lo
    //    limpia y reconstruye updatePlanetLOD POR PLANETA, así que tras procesar
    //    los N planetas solo contiene los del ÚLTIMO → los chunks recién generados
    //    de los demás planetas (p.ej. la Tierra) se descartaban y NO aparecían
    //    hasta moverte (recompute del LOD). En su lugar comprobamos el set deseado
    //    del planeta CORRECTO (chunksToLoad ∪ chunksToKeep de su último LODUpdate).
    HARUKA_PROFILE("lod.upload"); // subida a GPU de chunks recién generados (crea buffers GL = caro)
    std::unordered_map<std::string, std::unordered_set<uint64_t>> wantedByPlanet;
    for (const auto& up : m_lastUpdates) {
        if (up.planetName.empty()) continue;
        auto& set = wantedByPlanet[up.planetName];
        for (const auto& k : up.chunksToLoad) set.insert(ChunkCache::keyToHash(k));
        for (const auto& k : up.chunksToKeep) set.insert(ChunkCache::keyToHash(k));
    }
    auto readyChunks = m_streaming->getReadyChunks();
    for (auto& chunk : readyChunks) {
        auto it = wantedByPlanet.find(chunk->planetName);
        if (it != wantedByPlanet.end() &&
            !it->second.count(ChunkCache::keyToHash(chunk->key))) continue; // ya no se desea
        if (m_renderer->isResident(chunk->key)) continue; // ya subido (idempotente) → no gastar presupuesto
        // F7: este camino (integrar chunks RECIÉN generados) también cuenta contra el
        // presupuesto de subidas/frame. En un fly-in terminan ~m_maxInFlight+kSlots a la
        // vez → volcarlos TODOS clavaba 150–200 ms (creación de buffers GL terreno+agua).
        // Lo que no entre queda en CACHÉ (addChunk ya corrió en el worker) → processLODUpdate
        // lo sube en frames siguientes (filtra por residencia). Cap compartido = sin picos.
        if (!m_streaming->tryConsumeUpload()) break;
        m_renderer->addToScene(chunk->planetName, chunk->key, *chunk);
        if (m_waterRenderer) m_waterRenderer->addToScene(chunk->planetName, chunk->key, *chunk);
    }

    // Modo vigilancia del LOD: valida cada frame y avisa SOLO al volverse inválido (o al
    // recuperarse), throttled, para no spamear. Activar con `lodcheck watch`.
    if (m_lodWatch && m_renderer) {
        static bool s_wasInvalid = false;
        auto rep = m_renderer->validateCoverage();
        if (!rep.valid() && !s_wasInvalid) {
            fprintf(stderr, "[LODwatch] INVALIDO: dibujados=%d overlaps=%d holes=%d balance=%d"
                            " (muestra hole face=%d lod=%d)\n",
                    rep.drawn, rep.overlaps, rep.holes, rep.balance,
                    rep.hasHole ? (int)rep.sampleHole.face : -1,
                    rep.hasHole ? (int)rep.sampleHole.lod  : -1);
        } else if (rep.valid() && s_wasInvalid) {
            fprintf(stderr, "[LODwatch] OK de nuevo (dibujados=%d)\n", rep.drawn);
        }
        s_wasInvalid = !rep.valid();
    }
}

std::string PlanetarySystem::validateLOD() const {
    if (!m_renderer) return "LOD: renderer no listo";
    const auto r = m_renderer->validateCoverage();
    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "[LOD] %s | dibujados=%d  overlaps=%d  holes=%d  balance=%d",
        r.valid() ? "OK" : "INVALIDO", r.drawn, r.overlaps, r.holes, r.balance);
    auto append = [&](const char* tag, bool has, const PlanetChunkKey& k) {
        if (has && n < (int)sizeof(buf))
            n += snprintf(buf + n, sizeof(buf) - n,
                          "\n  %s: body=%d face=%d lod=%d x=%u y=%u",
                          tag, (int)k.body, (int)k.face, (int)k.lod, k.x, k.y);
    };
    append("overlap", r.hasOverlap, r.sampleOverlap);
    append("hole",    r.hasHole,    r.sampleHole);
    append("balance", r.hasBalance, r.sampleBalance);
    std::string s(buf);
    // Desglose por cuerpo: distancia/radio/dibujados → revela si un cuerpo lejano (Júpiter)
    // se está sobre-subdividiendo y aparece como "bolas" flotando. body index = pi.
    for (size_t pi = 0; pi < m_planets.size() && pi < 16; ++pi) {
        const auto& p = m_planets[pi];
        const double distKm   = glm::length(m_prevCamPos - p.position) / 1000.0;
        const double radKm    = p.radius / 1000.0;
        const double altKm    = (glm::length(m_prevCamPos - p.position) - p.radius) / 1000.0;
        char bb[192];
        snprintf(bb, sizeof(bb),
                 "\n  body %zu '%s': R=%.0fkm dist=%.0fkm alt=%.0fkm dibujados=%d overlaps=%d",
                 pi, p.name.c_str(), radKm, distKm, altKm,
                 (pi < 16 ? r.drawnByBody[pi] : 0), (pi < 16 ? r.overlapsByBody[pi] : 0));
        s += bb;
    }
    return s;
}

double PlanetarySystem::benchDrawSet(int iters, int* outDrawn) const {
    if (!m_renderer || iters < 1) { if (outDrawn) *outDrawn = 0; return 0.0; }
    std::vector<double> ns; ns.reserve(iters);
    std::unordered_set<uint64_t> out;
    for (int i = 0; i < iters; ++i) {
        out.clear();
        auto t0 = std::chrono::high_resolution_clock::now();
        m_renderer->buildDrawSet("TestPlanet", out, nullptr);
        auto t1 = std::chrono::high_resolution_clock::now();
        ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    if (outDrawn) *outDrawn = (int)out.size();
    std::sort(ns.begin(), ns.end());
    return ns[ns.size() / 2]; // mediana ns/llamada
}

bool PlanetarySystem::checkDrawSetMatchesReference(int iters, double* outNsOpt, double* outNsRef,
                                                   int* outDrawn) const {
    if (!m_renderer) return false;
    std::unordered_set<uint64_t> opt, ref;
    m_renderer->buildDrawSet("TestPlanet", opt, nullptr);
    m_renderer->buildDrawSetReference("TestPlanet", ref, nullptr);
    if (outDrawn) *outDrawn = (int)opt.size();
    if (iters > 0) {
        std::vector<double> a, b; a.reserve(iters); b.reserve(iters);
        std::unordered_set<uint64_t> tmp;
        for (int i = 0; i < iters; ++i) {
            tmp.clear(); auto t0 = std::chrono::high_resolution_clock::now();
            m_renderer->buildDrawSet("TestPlanet", tmp, nullptr);
            auto t1 = std::chrono::high_resolution_clock::now();
            a.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
            tmp.clear(); auto t2 = std::chrono::high_resolution_clock::now();
            m_renderer->buildDrawSetReference("TestPlanet", tmp, nullptr);
            auto t3 = std::chrono::high_resolution_clock::now();
            b.push_back(std::chrono::duration<double, std::nano>(t3 - t2).count());
        }
        std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
        if (outNsOpt) *outNsOpt = a[a.size() / 2];
        if (outNsRef) *outNsRef = b[b.size() / 2];
    }
    return opt == ref; // MISMO conjunto exacto
}

bool PlanetarySystem::checkCachedDrawSetFresh() const {
    return m_renderer ? m_renderer->debugCachedDrawSetIsFresh("TestPlanet") : true;
}

void PlanetarySystem::residencyGatingStats(uint64_t& changes, uint64_t& bumps) const {
    changes = bumps = 0;
    if (m_renderer) m_renderer->debugResidencyStats(changes, bumps);
}

void PlanetarySystem::drawSetDeltaStats(uint64_t& applied, uint64_t& full) const {
    applied = full = 0;
    if (m_renderer) m_renderer->debugDeltaStats(applied, full);
}

void PlanetarySystem::benchDrawSetSplice(int iters, double* nsFull, double* nsSplice, size_t* drawn) const {
    if (m_renderer) m_renderer->benchDrawSetSplice("TestPlanet", iters, nsFull, nsSplice, drawn);
}

void PlanetarySystem::updateOrbits(double /*dt*/) {
    // ÓRBITA KEPLER ANALÍTICA: la posición es función del TIEMPO ABSOLUTO (m_simulationTime), no una
    // integración incremental → SIN deriva ni inestabilidad (la órbita cierra exacta cada periodo).
    // Resolvemos la CADENA DE PADRES RECURSIVAMENTE (memoizada) → el orden en m_planets da igual y
    // funciona multinivel: Luna orbita Tierra que orbita Sol, sin lag. `done` marca lo ya resuelto
    // este frame (y corta ciclos: un padre en su propio linaje usa la posición actual).
    const size_t n = m_planets.size();
    std::vector<char> done(n, 0);
    std::function<glm::dvec3(size_t)> resolve = [&](size_t i) -> glm::dvec3 {
        Planet& p = m_planets[i];
        if (done[i]) return glm::dvec3(p.position);
        done[i] = 1; // marca ANTES de recursar (guard de ciclos)
        if (p.orbitParent >= 0 && (size_t)p.orbitParent < n && p.orbitPeriod > 0.0 && (size_t)p.orbitParent != i) {
            const glm::dvec3 focus = resolve((size_t)p.orbitParent); // el padre PRIMERO
            const double e = glm::clamp(p.orbitEcc, 0.0, 0.99);
            const double M = p.orbitPhase + 2.0 * 3.14159265358979323846 * (m_simulationTime / p.orbitPeriod);
            double E = M; // Kepler M = E − e·sinE por Newton
            for (int it = 0; it < 8; ++it) E -= (E - e * std::sin(E) - M) / (1.0 - e * std::cos(E));
            const double x = p.orbitA * (std::cos(E) - e);
            const double y = p.orbitA * std::sqrt(std::max(0.0, 1.0 - e * e)) * std::sin(E);
            p.position = focus + p.orbitU * x + p.orbitV * y;
        }
        return glm::dvec3(p.position);
    };
    for (size_t i = 0; i < n; ++i) resolve(i);
}

void PlanetarySystem::addPlanet(const Planet& planet) {
    // La caché de DISCO se ata a la seed del PRIMER planeta (el mundo). Sin separar por seed, un
    // mundo nuevo leería los chunks del anterior → terreno de otro planeta.
    // ⚠️ DETRÁS DE INTERRUPTOR (HARUKA_DISKCACHE=1), aún NO por defecto.
    // Funciona (genera una vez, luego lee; escrituras en hilo aparte; versionada por generación),
    // PERO con la caché poblada 1 test de streaming se cae: leer es mucho más rápido que generar y
    // el orden/ritmo de llegada de los chunks cambia → el LOD no se asienta igual. Hay que entender
    // esa interacción antes de encenderla. Con la caché vacía o apagada: 57 tests en verde.
    if (m_cache && m_planets.empty() && getenv("HARUKA_DISKCACHE")) {
        const auto& cfg = planet.terrainSettings.contains("config")
                        ? planet.terrainSettings["config"] : planet.terrainSettings;
        m_cache->setDiskCache("cache/terrain", (uint32_t)cfg.value("seed", 42));
    }
    m_planets.push_back(planet);
    m_forceLOD = true; // recompute LOD next update so the new planet appears at once
}

bool PlanetarySystem::setPlanetOrbit(const std::string& planetName, const std::string& parentName,
                                     double period, double ecc) {
    int pi = -1, par = -1;
    for (size_t k = 0; k < m_planets.size(); ++k) {
        if (m_planets[k].name == planetName) pi  = (int)k;
        if (m_planets[k].name == parentName) par = (int)k;
    }
    if (pi < 0) return false;
    Planet& pl = m_planets[(size_t)pi];
    if (parentName.empty() || period <= 0.0 || par < 0 || par == pi) { // DETENER
        pl.orbitParent = -1; pl.orbitPeriod = 0.0; return true;
    }
    const glm::dvec3 focus = m_planets[(size_t)par].position;
    glm::dvec3 rel = glm::dvec3(pl.position) - focus;
    double dist = glm::length(rel);
    if (dist < 1e-6) return false;
    const double e = glm::clamp(ecc, 0.0, 0.9);
    glm::dvec3 u = rel / dist;
    glm::dvec3 axis = (std::abs(u.y) < 0.95) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    glm::dvec3 v = glm::normalize(glm::cross(axis, u));
    pl.orbitParent = par; pl.orbitEcc = e; pl.orbitA = dist / std::max(1.0 - e, 1e-3);
    pl.orbitPeriod = period;
    // fase para que M=0 (periastro) AHORA, arrancando desde la posición actual sin salto.
    pl.orbitPhase = -2.0 * 3.14159265358979323846 * (m_simulationTime / period);
    pl.orbitU = u; pl.orbitV = v;
    return true;
}

// Malla de Luna: esfera base (unidad) + desplazamiento de cráteres deterministas
// (cuenca + borde elevado) y ruido fino; normales recalculadas de la malla
// deformada para que los cráteres tengan sombreado real. El SceneObject la escala.
static void buildCrateredMoonMesh(std::vector<glm::vec3>& verts,
                                  std::vector<glm::vec3>& norms,
                                  std::vector<glm::vec3>& cols,
                                  std::vector<unsigned int>& idx) {
    std::vector<glm::vec3> base, bn;
    Haruka::Renderer::PrimitiveShapes::createSphereLOD(1.0f, 72, 54, base, bn, idx);

    struct Crater { glm::vec3 dir; float ang; float depth; };
    std::vector<Crater> craters;
    uint32_t s = 0xC0FFEEu;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return float(s >> 8) * (1.0f / 16777216.0f); };
    for (int i = 0; i < 48; ++i) {
        glm::vec3 d = glm::normalize(glm::vec3(rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1));
        float ang   = glm::mix(0.04f, 0.30f, rnd() * rnd());
        float depth = ang * glm::mix(0.25f, 0.55f, rnd());
        craters.push_back({ d, ang, depth });
    }

    verts.resize(base.size());
    cols.resize(base.size());
    for (size_t i = 0; i < base.size(); ++i) {
        glm::vec3 v = glm::normalize(base[i]);
        float disp  = 0.012f * std::sin(v.x * 40.0f) * std::sin(v.y * 37.0f) * std::sin(v.z * 41.0f);
        float shade = 1.0f;
        for (const auto& c : craters) {
            float a = std::acos(glm::clamp(glm::dot(v, c.dir), -1.0f, 1.0f));
            if (a < c.ang) {
                float t    = a / c.ang;
                float bowl = -c.depth * (1.0f - t * t);
                float rim  = c.depth * 0.5f * std::exp(-((t - 0.92f) * (t - 0.92f)) / 0.004f);
                disp  += bowl + rim;
                shade *= glm::mix(0.80f, 1.0f, t);
            }
        }
        verts[i] = v * (1.0f + disp);
        float g  = glm::clamp((0.56f + 0.10f * std::sin(v.y * 19.0f)) * shade, 0.15f, 0.95f);
        cols[i]  = glm::vec3(g, g, g * 1.03f);
    }

    norms.assign(verts.size(), glm::vec3(0.0f));
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        unsigned a = idx[t], b = idx[t + 1], c = idx[t + 2];
        glm::vec3 fn = glm::cross(verts[b] - verts[a], verts[c] - verts[a]);
        norms[a] += fn; norms[b] += fn; norms[c] += fn;
    }
    for (auto& n : norms) n = (glm::length(n) > 1e-6f) ? glm::normalize(n) : glm::vec3(0, 1, 0);
}

void PlanetarySystem::buildFromScene(SceneManager& scene) {
    // Un planeta ES un cuerpo celeste: un solo recorrido decide, por objeto, qué
    // representación necesita — terreno por chunks (si trae terrainSettings) y/o
    // una malla procedural propia (si trae properties.proceduralMesh). No se separan.
    // ÓRBITAS (opt-in): un objeto con la propiedad "orbitParent" (nombre de otro cuerpo) y
    // "orbitPeriod" (s) orbitará con Kepler analítico. a/u/v/fase se derivan de su posición INICIAL
    // en la escena (arranca ahí, en el periastro). Sin esas props → estático (retrocompatible).
    struct OrbitIntent { size_t planetIdx; std::string parent; double period, ecc; };
    std::vector<OrbitIntent> orbitIntents;
    for (auto& objPtr : scene.getObjectsMutable()) {
        if (!objPtr) continue;
        auto& obj = *objPtr;

        // (1) PLANETA: cuerpo con terreno procedural → terreno por chunks/LOD.
        if (obj.terrainSettings && !obj.terrainSettings->layers.empty()) {
            const auto& ts = *obj.terrainSettings;
            Planet planet;
            planet.name     = obj.name;
            planet.position = obj.position;
            planet.radius   = std::max({obj.scale.x, obj.scale.y, obj.scale.z});
            planet.isHome   = obj.flags.originShiftingTarget; // el planeta del jugador

            // Config COMPLETA sin filtrar (todos los parámetros que la escena escribió);
            // cae al subconjunto tipado si faltara rawConfig (guardados antiguos).
            if (!ts.rawConfig.is_null() && ts.rawConfig.is_object()) {
                planet.terrainSettings["config"] = ts.rawConfig;
                if (!planet.terrainSettings["config"].contains("chunkSize"))
                    planet.terrainSettings["config"]["chunkSize"] = ts.chunkSize > 0 ? ts.chunkSize : 24;
            } else {
                planet.terrainSettings["config"]["chunkSize"] = ts.chunkSize > 0 ? ts.chunkSize : 24;
                planet.terrainSettings["config"]["seed"]      = ts.seed;
                for (const auto& [name, layer] : ts.layers) {
                    planet.terrainSettings["config"]["layers"][name]["freq"]     = layer.freq;
                    planet.terrainSettings["config"]["layers"][name]["octaves"]  = layer.octaves;
                    planet.terrainSettings["config"]["layers"][name]["strength"] = layer.strength;
                }
            }
            addPlanet(planet);
            // ¿Orbita? (propiedades de la escena) → intención a resolver cuando estén TODOS.
            std::string op = obj.getProperty<std::string>("orbitParent", std::string{});
            double per = (double)obj.getProperty<float>("orbitPeriod", 0.0f);
            if (!op.empty() && per > 0.0)
                orbitIntents.push_back({ m_planets.size() - 1, op, per,
                                         (double)obj.getProperty<float>("orbitEcc", 0.0f) });
        }

        // (2) MALLA procedural propia (cuerpo sin terreno): p.ej. la Luna con
        //     cráteres. Esfera + cráteres; se ilumina por el Sol (cráteres y fases).
        if (!obj.meshRenderer &&
            obj.getProperty<std::string>("proceduralMesh", std::string{}) == "cratered_moon") {
            auto mrc = std::make_shared<MeshRendererComponent>();
            std::vector<glm::vec3> v, n, c; std::vector<unsigned int> idx;
            buildCrateredMoonMesh(v, n, c, idx);
            mrc->setMesh(v, n, c, idx);
            obj.meshRenderer = mrc;
            fprintf(stderr, "[PlanetarySystem] Procedural mesh attached to celestial body '%s'\n", obj.name.c_str());
        }
    }

    // Resuelve las órbitas: nombre del padre → índice; deriva a/u/v/fase de la posición INICIAL de
    // la escena (arranca en el periastro a lo largo del radial inicial; plano ~horizontal del padre).
    for (const auto& oi : orbitIntents) {
        int parentIdx = -1;
        for (size_t k = 0; k < m_planets.size(); ++k)
            if (m_planets[k].name == oi.parent) { parentIdx = (int)k; break; }
        if (parentIdx < 0 || parentIdx == (int)oi.planetIdx) continue; // padre no hallado o self
        Planet& pl = m_planets[oi.planetIdx];
        const glm::dvec3 focus = m_planets[(size_t)parentIdx].position;
        glm::dvec3 rel = glm::dvec3(pl.position) - focus;
        double dist = glm::length(rel);
        if (dist < 1e-6) continue;
        const double e = glm::clamp(oi.ecc, 0.0, 0.9);
        glm::dvec3 u = rel / dist;                                   // radial inicial = eje del periastro
        glm::dvec3 axis = (std::abs(u.y) < 0.95) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        glm::dvec3 v = glm::normalize(glm::cross(axis, u));          // perpendicular (plano ~horizontal)
        pl.orbitParent = parentIdx;
        pl.orbitEcc    = e;
        pl.orbitA      = dist / std::max(1.0 - e, 1e-3);            // periastro (E=0) = dist inicial
        pl.orbitPeriod = oi.period;
        pl.orbitPhase  = 0.0;
        pl.orbitU = u; pl.orbitV = v;
        fprintf(stderr, "[orbit] '%s' orbita '%s': a=%.3e e=%.2f T=%.1fs\n",
                pl.name.c_str(), oi.parent.c_str(), pl.orbitA, e, pl.orbitPeriod);
    }

    // (Órbitas por defecto: NO se auto-cablean aquí — WorldSystem ya gestiona Luna+día/noche. Ver notas.)

    // --- TEST DE PARIDAD GPU↔CPU (una vez) ---
    // Antes de meter el generador GPU en el pipeline hay que confirmar que el Perlin
    // portado a GLSL da la MISMA altura que la CPU (si no, mundo/colisión corruptos).
    // Comparamos elevación GPU (terran, sin lagos) vs sampleTerrainV2 (con lagos) en
    // miles de direcciones: mediana≈0 ⇒ port correcto; el máximo grande son los lagos.
    fprintf(stderr, "[buildFromScene] planetas añadidos a m_planets: %zu\n", m_planets.size());
    for (const auto& p : m_planets)
        fprintf(stderr, "[buildFromScene]   - '%s' r=%.0f\n", p.name.c_str(), p.radius);

    // Test de paridad GPU↔CPU: DIAGNÓSTICO, no se necesita en producción. Hace 4096
    // muestras CPU con sampleTerrainV2 completo (lagos+autodiff) en el hilo principal
    // → ~700 ms de pico al cargar la escena. Ya está validado (mediana 0), así que por
    // defecto OFF. Para re-ejecutarlo: HARUKA_GPU_PARITY=1 ./juego.
    static bool s_parityDone = (std::getenv("HARUKA_GPU_PARITY") == nullptr);
    if (!s_parityDone) {
        for (const auto& p : m_planets) {
            const auto& cfg = p.terrainSettings.contains("config") ? p.terrainSettings["config"] : p.terrainSettings;
            if (cfg.value("genVersion", 1) < 2) continue;
            if (cfg.value("profile", std::string("terran")) != "terran") continue;
            s_parityDone = true;   // solo se marca cuando ENCONTRAMOS un planeta terran v2
            uint32_t seed  = (uint32_t)cfg.value("seed", 42);
            float    relief = cfg.value("reliefStrength", 1.0f);
            WorldGenParams W = deriveWorldParams(seed, p.radius);
            W.reliefStrength = relief; W.profile = 0;

            std::vector<glm::vec3> dirs; dirs.reserve(4096);
            uint32_t s = 12345u;
            auto rnd = [&]() { s = s * 1664525u + 1013904223u; return float(s >> 8) * (1.0f / 16777216.0f); };
            for (int i = 0; i < 4096; ++i)
                dirs.push_back(glm::normalize(glm::vec3(rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1)));

            GpuHeightfield gpu;
            GpuHeightfield::Params gp;
            gp.seed = (int)seed; gp.continentFreqA = W.continentFreqA; gp.reliefStrength = relief;
            gp.seaThreshold = W.seaThreshold; gp.voronoiDensity = W.voronoiDensity;
            gp.lakeDensity = W.lakeDensity; gp.lakeMaxProb = W.lakeMaxProb; gp.coastWidth = W.coastWidth; gp.radius = p.radius;
            std::vector<float> ge, gw; std::vector<glm::vec3> gn;
            if (gpu.generate(dirs, gp, ge, gn, gw)
                && ge.size() == dirs.size()) {
                std::vector<float> diffs; diffs.reserve(dirs.size());
                double sumd = 0.0, maxd = 0.0;
                for (size_t i = 0; i < dirs.size(); ++i) {
                    float cpu = sampleTerrainV2(dirs[i], W, p.radius).elevKm; // incluye lagos
                    float d = std::fabs(cpu - ge[i]);
                    diffs.push_back(d); sumd += d; if (d > maxd) maxd = d;
                }
                std::sort(diffs.begin(), diffs.end());
                fprintf(stderr, "[GPU parity] '%s': N=%zu  mediana|Δ|=%.4f km  media=%.4f km  max=%.4f km\n",
                       p.name.c_str(), dirs.size(), diffs[diffs.size() / 2], sumd / dirs.size(), maxd);
                fprintf(stderr, "[GPU parity] mediana≈0 => Perlin portado OK; max grande = lagos (aún solo en CPU).\n");
            } else {
                fprintf(stderr, "[GPU parity] no se pudo ejecutar el compute (¿sin contexto GL?).\n");
            }
            break;
        }
    }
}

void PlanetarySystem::syncFromScene(const SceneManager& scene) {
    HARUKA_PROFILE("lod.sync");
    // Antes: O(planetas × TODOS los objetos de escena) con std::string-compares CADA frame → con
    // miles de props colocados escaneaba decenas de miles de strings/frame (parte del self-time de
    // planetary.update). Ahora O(planetas) vía el registro por nombre de SceneManager (hash O(1)).
    for (auto& planet : m_planets) {
        auto obj = scene.getObject(planet.name);
        if (!obj) continue;
        planet.position = obj->position;
        planet.radius   = std::max({obj->scale.x, obj->scale.y, obj->scale.z});
    }
    // (Las islas flotantes se generan en update(), cerca de la cámara: streaming-lite.)
}

void PlanetarySystem::renderPlanetIslands(const std::string& planetName, const glm::dvec3& cameraPos) {
    (void)planetName;
    if (m_islandRenderer) m_islandRenderer->render(cameraPos);
}

bool PlanetarySystem::getActivePlanet(glm::dvec3& center, double& radius,
                                     uint32_t& seed, float& reliefStrength) const {
    // El planeta ACTIVO es el HOGAR del jugador (flags.originShiftingTarget), no el
    // primero por orden — si no, con varios planetas (Luna, Júpiter) el activo podría
    // ser otro. Si ninguno está marcado, cae al primer v2.
    const Planet* fallback = nullptr;
    for (const auto& planet : m_planets) {
        const auto& cfg = planet.terrainSettings.contains("config")
                        ? planet.terrainSettings["config"] : planet.terrainSettings;
        if (cfg.value("genVersion", 1) < 2) continue;   // solo planetas v2 tienen recursos
        if (planet.isHome) { fallback = &planet; break; }
        if (!fallback) fallback = &planet;
    }
    if (!fallback) return false;
    const auto& cfg = fallback->terrainSettings.contains("config")
                    ? fallback->terrainSettings["config"] : fallback->terrainSettings;
    center         = glm::dvec3(fallback->position);
    radius         = fallback->radius;
    seed           = (uint32_t)cfg.value("seed", 42);
    reliefStrength = cfg.value("reliefStrength", 1.0f);
    return true;
}

void PlanetarySystem::setWaterPassParams(const WaterRenderer::PassParams& p) {
    if (m_waterRenderer) m_waterRenderer->setPassParams(p);
}

// Parámetros del PASE de terreno (van al UBO TerrainParams del renderer, no a uniforms sueltos).
void PlanetarySystem::setTerrainFog(bool on) {
    if (m_renderer) m_renderer->setFog(on);
}
void PlanetarySystem::setTerrainShadow(Haruka::RHI::TextureHandle map, const glm::mat4& lightSpace, bool on) {
    if (m_renderer) m_renderer->setShadow(map, lightSpace, on);
}

void PlanetarySystem::setTerrainCullMatrix(const glm::mat4& camRelViewProj) {
    m_curCullVP = camRelViewProj; // guardado para detectar GIRO de cámara (recompute del LOD al rotar)
    if (m_renderer)      m_renderer->setCullMatrix(camRelViewProj);
    if (m_waterRenderer) m_waterRenderer->setCullMatrix(camRelViewProj);
    if (m_lod)           m_lod->setCullMatrix(camRelViewProj); // F2: acota el recompute del LOD a la vista
    // Sincroniza el splitFactor del morph cada frame (independiente del orden de init y del
    // early-return de setLODParams) → la banda de morph siempre casa con el LOD real.
    if (m_lod) {
        const double sf = m_lod->getSplitFactor();
        if (m_renderer)      m_renderer->setSplitFactor(sf);
        if (m_waterRenderer) m_waterRenderer->setSplitFactor(sf);
    }
}

// Texturas de bioma OPCIONALES, declaradas en la escena:
//   terrainSettings.config.textures = { "dir": "assets/textures/terrain" }
// Convención de tiers (la genera tools/gen_texture_tiers.sh):
//   <dir>/512, <dir>/1024 (base) y <dir>_hd/2048 (pack HD). Tier por TextureQuality,
//   con fallback a 1024. Sin config → procedural (u_hasTex=0). El engine core no
//   sabe de texturas; esto vive en el sistema de terreno.
void PlanetarySystem::bindTerrainTextures(const Planet& planet) {
    namespace fs = std::filesystem;
    const auto& cfg = planet.terrainSettings.contains("config")
                    ? planet.terrainSettings["config"] : planet.terrainSettings;

    std::string dir;
    if (cfg.contains("textures") && cfg["textures"].is_object())
        dir = cfg["textures"].value("dir", std::string{});

    if (dir.empty()) { m_renderer->setBiomeTextures({}, false); return; } // sin texturas → procedural

    const int q    = (int)Haruka::SettingsManager::get().graphics().textureQuality;
    const int tier = (q <= 0) ? 512 : (q >= 3) ? 2048 : 1024;
    if (tier != m_texTier || dir != m_texDir) {
        std::string sub = (tier == 2048) ? (dir + "_hd/2048/")
                                         : (dir + "/" + std::to_string(tier) + "/");
        if (!fs::exists(sub + "sand_albedo.png")) sub = dir + "/1024/"; // fallback
        auto load = [&](const char* n) -> std::unique_ptr<Texture> {
            std::string p = sub + n;
            return fs::exists(p) ? std::make_unique<Texture>(p.c_str()) : nullptr;
        };
        m_texSandAlbedo  = load("sand_albedo.png");
        m_texSandNormal  = load("sand_normal.png");
        m_texGrassAlbedo = load("grass_albedo.png");
        m_texLandAlbedo  = load("land_albedo.png");
        m_texLandNormal  = load("land_normal.png");
        m_texTier = tier; m_texDir = dir;
    }
    bool has = m_texSandAlbedo && m_texGrassAlbedo && m_texLandAlbedo;
    // Las unidades de textura las fija el PSO (layout(binding=N) en planet.frag) → aquí solo
    // pasamos los HANDLES; el TerrainRenderer las ata con Context::bindTexture.
    TerrainRenderer::BiomeTextures t{};
    if (has) {
        t.sandAlbedo  = m_texSandAlbedo->handle();
        t.grassAlbedo = m_texGrassAlbedo->handle();
        t.landAlbedo  = m_texLandAlbedo->handle();
        if (m_texSandNormal) t.sandNormal = m_texSandNormal->handle();
        if (m_texLandNormal) t.landNormal = m_texLandNormal->handle();
    }
    m_renderer->setBiomeTextures(t, has);
}

void PlanetarySystem::renderPlanetTerrain(const std::string& planetName, const glm::dvec3& cameraPos) {
    if (!m_renderer) return;
    const Planet* planet = nullptr;
    for (const auto& p : m_planets)
        if (p.name == planetName) { m_renderer->setPlanetCenter(p.position); planet = &p; break; }
    if (planet) {
        bindTerrainTextures(*planet);
        // (F5) El BIOMA sale del campo (temperatura + humedad + pendiente + caudal), no de la
        // altura: mismo campo que generó el relieve → no pueden contradecirse.
        const auto& cfg = planet->terrainSettings.contains("config")
                        ? planet->terrainSettings["config"] : planet->terrainSettings;
        const uint32_t pseed = (uint32_t)cfg.value("seed", 42);
        int fres = 0, cres = 0;
        const unsigned fieldSSBO   = planetFieldsSSBO(pseed, fres);
        const unsigned climateSSBO = planetClimateSSBO(pseed, cres);
        m_renderer->setPlanetFields(fieldSSBO, climateSSBO, fres);
    }
    m_renderer->renderPlanet(planetName, cameraPos);
}

void PlanetarySystem::renderPlanetWater(const std::string& planetName, const glm::dvec3& cameraPos) {
    if (!m_waterRenderer) return;
    for (const auto& p : m_planets)
        if (p.name == planetName) {
            m_waterRenderer->setPlanetCenter(p.position);
            m_waterRenderer->setPlanetRadius(p.radius);
            break;
        }
    if (m_waterRenderer->getSingleOcean())
        m_waterRenderer->renderSingleOcean(planetName, cameraPos); // superficie única (test)
    else
        m_waterRenderer->renderPlanet(planetName, cameraPos);      // mallas de agua por-chunk
}
void PlanetarySystem::setWaterSingleOcean(bool on) { if (m_waterRenderer) m_waterRenderer->setSingleOcean(on); }
bool PlanetarySystem::getWaterSingleOcean() const  { return m_waterRenderer && m_waterRenderer->getSingleOcean(); }
void PlanetarySystem::setWaterFluidSampler(std::function<bool(const glm::dvec3&, float&, float&)> f) {
    if (m_waterRenderer) m_waterRenderer->setFluidSampler(std::move(f)); // F5.1: océano sigue a la sim
}

int PlanetarySystem::getGPUWaterChunkCount() const {
    return m_waterRenderer ? m_waterRenderer->getGPUMeshCount() : 0;
}

int PlanetarySystem::getGPUChunkCount()    const { return m_renderer  ? m_renderer->getGPUMeshCount()              : 0; }
int PlanetarySystem::getPendingChunks()    const { return m_streaming ? m_streaming->getPendingCount()              : 0; }
int PlanetarySystem::getQueuedChunks()     const { return m_streaming ? m_streaming->getQueuedCount()               : 0; }
int PlanetarySystem::getCachedChunks()     const { return m_cache     ? (int)m_cache->getChunkCount()               : 0; }
int PlanetarySystem::getCacheMemoryMB()    const { return m_cache     ? (int)m_cache->getMemoryUsageMB()            : 0; }
int PlanetarySystem::getCacheMaxMemoryMB() const { return m_cache     ? (int)m_cache->getMaxMemoryMB()              : 0; }
// RAM DISPONIBLE de verdad en MB (0 si no se puede determinar). NO es "libre": MemAvailable es lo
// que el kernel puede dar sin swapear (libre + caché reclamable). Es lo único que importa aquí: la
// RAM total no dice nada si el usuario ya tiene medio sistema ocupado con el navegador.
static size_t systemAvailableRAMMB() {
#if defined(_WIN32)
    MEMORYSTATUSEX s{}; s.dwLength = sizeof(s);
    if (GlobalMemoryStatusEx(&s)) return (size_t)(s.ullAvailPhys / (1024ull * 1024ull));
    return 0;
#else
    if (FILE* f = fopen("/proc/meminfo", "r")) {
        char line[256];
        size_t kb = 0;
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "MemAvailable: %zu kB", &kb) == 1) break;
        fclose(f);
        if (kb) return kb / 1024;
    }
    // Sin MemAvailable (kernels viejos / otro Unix): pág. libres × tamaño de página.
    long pages = sysconf(_SC_AVPHYS_PAGES), psize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && psize > 0) return (size_t)((double)pages * (double)psize / (1024.0 * 1024.0));
    return 0;
#endif
}

// RAM física total del sistema en MB (0 si no se puede determinar).
static size_t systemTotalRAMMB() {
#if defined(_WIN32)
    MEMORYSTATUSEX s{}; s.dwLength = sizeof(s);
    if (GlobalMemoryStatusEx(&s)) return (size_t)(s.ullTotalPhys / (1024ull * 1024ull));
    return 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && psize > 0)
        return (size_t)((double)pages * (double)psize / (1024.0 * 1024.0));
    return 0;
#endif
}

void PlanetarySystem::setCacheMaxMemoryMB(int mb) {
    if (!m_cache) return;
    int resolved = mb;
    int initialMB = mb;   // tamaño de arranque (auto: conservador según RAM libre; manual: = techo)
    if (mb <= 0) {
        // AUTO (mb<=0): el presupuesto de la cache de terreno = fracción de la RAM TOTAL,
        // acotado. El terreno es regenerable, así que puede ocupar bastante, pero hay que
        // dejar sitio al SO, driver GPU, caches de modelos/texturas y la lógica del juego.
        // 33% de la RAM, suelo 512 MB (que cargue algo en equipos pequeños), techo 16384 MB
        // (en equipos grandes la fracción sí compensa para explorar sin regenerar; el techo solo
        // evita que un servidor con cientos de GB dedique decenas de GB a terreno).
        // ⚠️ La RAM TOTAL no basta: en una máquina de 14 GB con 7 GB ya en uso (navegador, etc.),
        // el 25% del total = 3.4 GB de terreno + el resto del juego + el driver GPU metían al SISTEMA
        // ENTERO en swap → se congelaba el PC, no solo el juego. El terreno es REGENERABLE: la caché
        // es una optimización, jamás un motivo para empujar la máquina al swap.
        //
        // Se toma el MENOR de: 25% del total, y lo que quede de la RAM DISPONIBLE dejando una reserva
        // (para el driver GPU, texturas/modelos, el SO y lo que el usuario ya tenga abierto).
        // Los LÍMITES importan tanto como la fracción:
        //  - SUELO 1 GB: el conjunto de trabajo (los chunks que el LOD tiene residentes) ya ronda
        //    ~1 GB. Un techo por DEBAJO de eso no ahorra memoria: desaloja lo que el streaming
        //    regenera acto seguido → thrash permanente (la caché acaba MUY por encima de su techo).
        //    Si una máquina no puede con eso, lo que hay que bajar es el detalle (targetPx), no esto.
        //  - TECHO 3 GB: acumular más solo evita regenerar al VOLVER a un sitio — y para eso está la
        //    caché de DISCO. No merece la pena arriesgar el swap del sistema por ello.
        const size_t ram   = systemTotalRAMMB();
        const size_t avail = systemAvailableRAMMB();
        const size_t reserve = 2048;                              // aire para SO + driver GPU + resto
        // TECHO = fracción de la RAM TOTAL (no de la libre). Es el máximo al que el vigilante puede
        // recuperar cuando hay holgura. ⚠️ ANTES se acotaba por `avail` EN EL ARRANQUE: si abrías el
        // juego con el navegador abierto, el techo quedaba clavado bajo (p.ej. 1728) y NUNCA subía →
        // manta gruesa permanente. El techo debe venir de la RAM total; la protección la da el
        // vigilante en vivo (encoge si `avail` baja), no un techo tímido de arranque.
        size_t ceiling = ram ? (ram / 4) : 1024;
        ceiling = std::clamp<size_t>(ceiling, 1024, 3072);
        resolved = (int)ceiling;
        // TAMAÑO INICIAL = conservador según la RAM LIBRE ahora (no empujar al swap al arrancar). El
        // vigilante lo hace CRECER hacia el techo si `avail` da holgura, y encoger si aprieta.
        size_t initial = ceiling;
        if (avail) initial = std::min<size_t>(ceiling, avail > reserve ? avail - reserve : 1024);
        initialMB = (int)std::clamp<size_t>(initial, 1024, ceiling);
        fprintf(stderr, "[ChunkCache] Auto: RAM total=%zu MB, disponible=%zu MB → techo=%d MB, inicial=%d MB\n",
                ram, avail, resolved, initialMB);
    }
    // SEGURIDAD (memoria): la cache de terreno NUNCA debe pasar de ~50% de la RAM total, aunque el
    // ajuste manual lo pida (p.ej. 24 GB en una máquina de 32 GB llenaba TODA la RAM → thrashing).
    // Hay que dejar sitio al SO, driver GPU (los pools de malla), modelos/texturas y la lógica.
    {
        const size_t ram = systemTotalRAMMB();
        if (ram > 0) resolved = std::min<int>(resolved, (int)(ram * 3 / 10)); // ≤30% RAM (deja sitio a VRAM/SO)
    }
    m_cacheBudgetMB = (size_t)std::max(16, resolved); // techo al que el vigilante puede REGRESAR
    // Arranca en el tamaño INICIAL (conservador en auto), no en el techo → no empuja al swap al abrir.
    // El vigilante lo sube hacia el techo si hay holgura. Manual (initialMB=mb) = techo, salvo si la
    // seguridad bajó el techo: en ese caso, capado al techo.
    if (initialMB <= 0 || initialMB > (int)m_cacheBudgetMB) initialMB = (int)m_cacheBudgetMB;
    m_cache->setMaxMemory((size_t)std::max(16, initialMB));
}
void PlanetarySystem::setLODParams(double splitFactor, int maxLOD) {
    if (!m_lod) return;
    if (m_lod->getSplitFactor() == splitFactor && m_lod->getMaxLOD() == maxLOD) return;
    m_lod->setParams(splitFactor, maxLOD);
    // El renderer calcula la banda de morph CDLOD con el splitFactor → debe ser el MISMO que
    // el LOD o los chunks se suavizan a la distancia equivocada (parches lisos / popping).
    if (m_renderer)      m_renderer->setSplitFactor(splitFactor);
    if (m_waterRenderer) m_waterRenderer->setSplitFactor(splitFactor);
    m_forceLOD = true; // re-evaluate the quadtree next update
}

void PlanetarySystem::setTerrainBatching(bool on) {
    if (m_renderer)      m_renderer->setBatching(on);
    if (m_waterRenderer) m_waterRenderer->setBatching(on); // el toggle cubre terreno + agua
}
bool PlanetarySystem::getTerrainBatching() const  { return m_renderer && m_renderer->getBatching(); }

void PlanetarySystem::setLODScreenSpace(bool on) { if (m_lod) { m_lod->setScreenSpaceLOD(on); m_forceLOD = true; } }
bool PlanetarySystem::getLODScreenSpace() const { return m_lod && m_lod->getScreenSpaceLOD(); }
void PlanetarySystem::setLODScreenK(double k) {
    if (!m_lod) return;
    m_curScreenK = k; // espejo para snapshotear al job async (LODSystem no tiene getter)
    m_lod->setScreenK(k);
    // CDLOD morph EN SCREEN-SPACE: el split decide por targetPx (screenK·size/dist), así que la BANDA
    // de morph del renderer debe usar el MISMO criterio o se desalinea. splitFactor efectivo del morph
    // = screenK/targetPx → la banda [nodeSize·sf, 2·nodeSize·sf] coincide con la frontera de merge REAL.
    // Antes el morph usaba el splitFactor fijo (0.70, criterio de DISTANCIA) mientras el split era
    // screen-space → el chunk quedaba morphado al PADRE (hundido) sobre casi todo su rango visible →
    // la malla se hundía bajo la colisión analítica = "doble terreno / props flotando".
    if (m_lod->getScreenSpaceLOD()) {
        const double tpx = m_lod->getTargetPx();
        const double eff = (tpx > 1.0 && k > 1.0) ? (k / tpx) : m_lod->getSplitFactor();
        if (m_renderer)      m_renderer->setSplitFactor(eff);
        if (m_waterRenderer) m_waterRenderer->setSplitFactor(eff);
    }
}
void PlanetarySystem::setLODTargetPx(double px) { if (m_lod) { m_lod->setTargetPx(px); m_forceLOD = true; } }
double PlanetarySystem::getLODTargetPx() const { return m_lod ? m_lod->getTargetPx() : 0.0; }
void PlanetarySystem::setLODBudget(double budgetMs, double minPx, double maxPx) {
    m_lodBudgetMs = budgetMs;
    m_lodMinPx = std::min(minPx, maxPx);
    m_lodMaxPx = std::max(minPx, maxPx);
    // Arranca en la cota GRUESA (segura): el controlador afina hacia abajo si hay holgura. Así
    // nunca empezamos con un frame reventado por pedir demasiado detalle de golpe.
    if (m_lod && budgetMs > 0.0) { m_lod->setTargetPx(m_lodMaxPx); m_forceLOD = true; }
    m_lodEwmaMs = 0.0; m_lodAdjustAccum = 0.0;
}

PlanetarySystem::TerrainDrawStats PlanetarySystem::getTerrainDrawStats() const {
    if (!m_renderer) return {};
    const auto s = m_renderer->getDrawStats();
    return { s.draws, s.vertices, s.triangles };
}

PlanetarySystem::TerrainPoolStats PlanetarySystem::getTerrainPoolStats() const {
    if (!m_renderer) return {};
    const auto s = m_renderer->poolStats();
    return { s.poolCount, s.totalSlots, s.usedSlots, s.vramBytes, s.maxPoolCap, s.growCount };
}

bool PlanetarySystem::getSeaSurface(const glm::dvec3& worldPos, glm::dvec3& outCenter, double& outSeaRadius) const {
    if (m_planets.empty()) return false;
    const Planet* nearest = nullptr;
    double bestDist = 1e300;
    for (const auto& p : m_planets) {
        double d = glm::length(p.position - worldPos);
        if (d < bestDist) { bestDist = d; nearest = &p; }
    }
    if (!nearest) return false;
    outCenter    = nearest->position;
    outSeaRadius = nearest->radius; // sea level = elevation 0 = planet radius
    return true;
}

Haruka::TerrainSample PlanetarySystem::sampleSurface(const glm::dvec3& worldPos) const {
    Haruka::TerrainSample s; // por defecto: elev 0
    if (!m_generator || m_planets.empty()) return s;

    // Planeta más cercano al punto.
    const Planet* nearest = nullptr;
    double bestDist = 1e300;
    for (const auto& p : m_planets) {
        double d = glm::length(p.position - worldPos);
        if (d < bestDist) { bestDist = d; nearest = &p; }
    }
    if (!nearest) return s;

    glm::dvec3 dir = worldPos - nearest->position;
    double len = glm::length(dir);
    if (len < 1e-9) return s;
    glm::vec3 sphereDir = glm::vec3(dir / len);

    // Settings como en el streaming (config bajo terrainSettings).
    nlohmann::json settings = nearest->terrainSettings;
    settings["planetOffsetX"] = nearest->position.x;
    settings["planetOffsetY"] = nearest->position.y;
    settings["planetOffsetZ"] = nearest->position.z;
    return m_generator->sampleSurfaceAt(sphereDir, settings, nearest->radius);
}

double PlanetarySystem::sampleWaterLevel(const glm::dvec3& worldPos) const {
    const Haruka::TerrainSample s = sampleSurface(worldPos);
    if (s.waterType == Haruka::WaterType::None) return kNoWater;   // aquí NO hay agua
    return double(s.waterLevelKm) * 1000.0;                        // m sobre el radio de referencia
}

double PlanetarySystem::sampleTerrainHeight(const glm::dvec3& worldPos) const {
    // (F10) LA MALLA MANDA. Es lo que el jugador ve y sobre lo que se dibujan los props: si la
    // colisión usara la fórmula analítica (que difiere del meso donde el chunk es grueso), caerías a
    // través del suelo o los props flotarían. Se pregunta a la malla; solo si no hay chunk residente
    // ahí (spawn, IA, lejos) se cae al sampler analítico — donde, además, no hay malla que contradiga.
    const Planet* nearest = nullptr; double best = 1e300;
    for (const auto& p : m_planets) {
        const double d = glm::length(p.position - worldPos);
        if (d < best) { best = d; nearest = &p; }
    }
    if (nearest) {
        const glm::dvec3 rel = worldPos - nearest->position;
        const double len = glm::length(rel);
        if (len > 1e-9) {
            float meshKm = 0.0f;
            if (meshHeightKmAt(nearest->name, glm::vec3(rel / len), meshKm))
                return (double)meshKm * 1000.0;
        }
    }
    return double(sampleSurface(worldPos).elevKm) * 1000.0; // respaldo analítico (sin malla aquí)
}

std::string PlanetarySystem::getActivePlanetName() const {
    const Planet* fallback = nullptr;
    for (const auto& p : m_planets) {
        const auto& cfg = p.terrainSettings.contains("config") ? p.terrainSettings["config"] : p.terrainSettings;
        if (cfg.value("genVersion", 1) < 2) continue;
        if (p.isHome) return p.name;
        if (!fallback) fallback = &p;
    }
    return fallback ? fallback->name : std::string{};
}

bool PlanetarySystem::getActivePlanetParams(Haruka::WorldGenParams& out, double& outRadius) const {
    // MISMA selección que getActivePlanet (home, si no el primer v2) y MISMA construcción
    // de params que el sampler del motor → el juego usa estos en vez de re-derivar.
    const Planet* home = nullptr;
    for (const auto& p : m_planets) {
        const auto& cfg = p.terrainSettings.contains("config") ? p.terrainSettings["config"] : p.terrainSettings;
        if (cfg.value("genVersion", 1) < 2) continue;
        if (p.isHome) { home = &p; break; }
        if (!home) home = &p;
    }
    if (!home) return false;
    const auto& cfg = home->terrainSettings.contains("config") ? home->terrainSettings["config"] : home->terrainSettings;
    uint32_t seed = (uint32_t)cfg.value("seed", 42);
    out = Haruka::deriveWorldParams(seed, home->radius);
    out.reliefStrength = cfg.value("reliefStrength", 1.0f);
    { const std::string pr = cfg.value("profile", std::string("terran"));
      out.profile = (pr == "moon") ? 1 : (pr == "gas") ? 2 : 0; }
    outRadius = home->radius;
    return true;
}

void PlanetarySystem::invalidateAllChunks() {
    // Drop every GPU chunk + cache entry + LOD memory so the next update fully
    // regenerates the terrain (applying whatever brushes are now in the field).
    if (!m_renderer) return;
    // A sphere centred on each planet large enough to cover the whole planet.
    for (const auto& p : m_planets) {
        auto keys = m_renderer->invalidateSphere(p.position, p.radius * 4.0);
        for (const auto& k : keys) {
            if (m_cache) m_cache->removeChunk(k);
            if (m_waterRenderer) m_waterRenderer->removeFromScene("", k);
            if (m_lod) m_lod->forgetChunk(k);
            // En async el recompute corre sobre m_lodAsync (del worker): encola el forget para
            // aplicarlo en el próximo job (no podemos tocar m_lodAsync desde el hilo de render).
            if (m_asyncLOD.load(std::memory_order_relaxed)) m_pendingForget.push_back(k);
        }
    }
    m_forceLOD = true;
}

void PlanetarySystem::editTerrain(const glm::dvec3& worldPos, double radius, double strength, bool dig) {
#ifdef HARUKA_MOD_DEFORM
    if (!m_deform) return;

    DeformationField::Brush b;
    b.center   = worldPos;
    b.radius   = radius;
    b.strength = strength;
    b.type     = dig ? DeformationField::Type::Subtract : DeformationField::Type::Add;
    m_deform->addBrush(b);
    invalidateEditedChunks(worldPos, radius);
#else
    (void)worldPos; (void)radius; (void)strength; (void)dig;
#endif
}

void PlanetarySystem::levelTerrain(const glm::dvec3& worldPos, double radius, double targetHeightM) {
#ifdef HARUKA_MOD_DEFORM
    if (!m_deform) return;

    DeformationField::Brush b;
    b.center       = worldPos;
    b.radius       = radius;
    b.type         = DeformationField::Type::Flatten;
    b.targetHeight = targetHeightM;   // el área se aplana hacia esta elevación
    m_deform->addBrush(b);
    invalidateEditedChunks(worldPos, radius);
#else
    (void)worldPos; (void)radius; (void)targetHeightM;
#endif
}

void PlanetarySystem::levelTerrainBox(const glm::dvec3& center, const glm::dvec3& halfExtents,
                                      const glm::dmat3& rot, double targetHeightM, double band) {
#ifdef HARUKA_MOD_DEFORM
    if (!m_deform) return;

    DeformationField::Brush b;
    b.center       = center;
    b.type         = DeformationField::Type::Flatten;
    b.targetHeight = targetHeightM;
    b.box          = true;            // huella del objeto (no un círculo)
    b.halfExtents  = halfExtents;
    b.rot          = rot;
    b.radius       = band;            // ancho de transición del borde
    m_deform->addBrush(b);
    invalidateEditedChunks(center, glm::length(halfExtents) + band);
#else
    (void)center; (void)halfExtents; (void)rot; (void)targetHeightM; (void)band;
#endif
}

void PlanetarySystem::invalidateEditedChunks(const glm::dvec3& center, double radius) {
    // Marca los chunks GPU que toca la edición (+margen) como STALE (NO los borra → la malla
    // vieja se sigue viendo, sin agujero) y los quita de la caché para regenerarlos con la
    // edición. Al llegar el chunk regenerado, addToScene REEMPLAZA la malla vieja sin parón.
    const double margin = radius * 1.5 + 8.0;
    if (m_renderer) {
        auto keys = m_renderer->markStaleSphere(center, margin);
        for (const auto& k : keys) {
            if (m_cache) m_cache->removeChunk(k);
            if (m_waterRenderer) m_waterRenderer->removeFromScene("", k);
            // Olvídalo en el LOD: el próximo update lo re-pide (LOAD) → regenera y reemplaza.
            if (m_lod) m_lod->forgetChunk(k);
            // En async, el worker recomputa sobre m_lodAsync: encola el forget para el próximo job.
            if (m_asyncLOD.load(std::memory_order_relaxed)) m_pendingForget.push_back(k);
        }
    }
    m_forceLOD = true; // recomputa el LOD el próximo update → re-pide los chunks liberados
}


// ===================== F10 — LA ALTURA SALE DE LA MALLA ========================================
bool PlanetarySystem::meshHeightKmAt(const std::string& planetName, const glm::vec3& dirIn,
                                     float& outElevKm) const {
    if (!m_cache || !m_renderer) return false;

    // Índice del cuerpo: las claves de chunk lo llevan estampado (Tierra/Luna no colisionan).
    uint16_t body = 0; double radius = 0.0; bool found = false;
    for (size_t i = 0; i < m_planets.size(); ++i)
        if (m_planets[i].name == planetName) { body = (uint16_t)i; radius = m_planets[i].radius; found = true; break; }
    if (!found || radius <= 0.0) return false;

    const glm::dvec3 dir = glm::normalize(glm::dvec3(dirIn));

    // 1) Dirección → (cara, lx, ly): la INVERSA del cubo→esfera (Newton; ver TerrainGenerator).
    PlanetFace face; double lx, ly;
    TerrainGenerator::dirToFaceLocal(dir, face, lx, ly);
    const double u = glm::clamp(lx * 0.5 + 0.5, 0.0, 1.0);   // [0,1] dentro de la cara
    const double v = glm::clamp(ly * 0.5 + 0.5, 0.0, 1.0);

    // 2) De FINO a GRUESO: el primer chunk RESIDENTE que cubre el punto es el que el render dibuja.
    const int maxLod = m_lod ? m_lod->getMaxLOD() : 12;
    for (int lod = maxLod; lod >= 0; --lod) {
        const double cpa = (double)(1u << lod);              // chunks por eje en este LOD
        PlanetChunkKey key;
        key.face = face;
        key.lod  = (uint8_t)lod;
        key.x    = (uint32_t)glm::clamp((int)(u * cpa), 0, (int)cpa - 1);
        key.y    = (uint32_t)glm::clamp((int)(v * cpa), 0, (int)cpa - 1);
        key.body = body;
        if (!m_renderer->isResident(key)) continue;

        const ChunkData* cd = m_cache->getChunk(key);
        if (!cd || cd->vertices.empty()) continue;

        // 3) Resolución de la rejilla: n = (res+1)² + 4·(res+1) (los 4 faldones). Se despeja en vez
        //    de guardarla: ChunkData ya carga bastante y esto es exacto.
        int res = 0;
        for (int r = 4; r <= 256; ++r)
            if ((size_t)(r + 1) * (r + 5) == cd->vertices.size()) { res = r; break; }
        if (res == 0) continue;

        // 4) Bilineal sobre la celda. La malla es lineal a trozos ENTRE vértices: interpolar igual
        //    que el rasterizador es lo que hace que la altura sea la MISMA que ves.
        const double fx = glm::clamp((u * cpa - key.x) * res, 0.0, (double)res - 1e-9);
        const double fy = glm::clamp((v * cpa - key.y) * res, 0.0, (double)res - 1e-9);
        const int x0 = (int)fx, y0 = (int)fy;
        const int x1 = std::min(x0 + 1, res), y1 = std::min(y0 + 1, res);
        const double tx = fx - x0, ty = fy - y0;

        auto elevAt = [&](int x, int y) -> double {
            const glm::dvec3 p = cd->chunkCenter + glm::dvec3(cd->vertices[(size_t)(x + y * (res + 1))]);
            return (glm::length(p) - radius) * 0.001;        // km sobre el nivel del mar
        };
        const double e0 = elevAt(x0, y0) * (1 - tx) + elevAt(x1, y0) * tx;
        const double e1 = elevAt(x0, y1) * (1 - tx) + elevAt(x1, y1) * tx;
        outElevKm = (float)(e0 * (1 - ty) + e1 * ty);
        return true;
    }
    return false;   // sin malla aquí (lejos del jugador) → el llamador usa el sampler analítico
}

}
