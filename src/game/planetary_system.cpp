#include "planetary_system.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <unordered_map>
#include <unordered_set>
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
PlanetarySystem::~PlanetarySystem() {}

void PlanetarySystem::init() {
    // Inicializamos los subsistemas una sola vez
    // 768 MB: el suelo de LOD mantiene el planeta entero (~1536 chunks base) residente
    // SIEMPRE; con 384 MB se evictaba y entraba en bucle recarga. Override por GraphicsSettings.
    m_cache = std::make_unique<ChunkCache>(768);
    m_generator = std::make_unique<TerrainGenerator>();
    m_renderer = std::make_unique<TerrainRenderer>();
    m_waterRenderer = std::make_unique<WaterRenderer>();
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
}

void PlanetarySystem::update(double dt, const glm::dvec3& cameraPos) {
    m_simulationTime += dt;

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
    updateOrbits(dt);

    // Keep the per-planet LOD-throttle state sized to the planet list.
    if (m_lastLODCamPos.size() != m_planets.size())
        m_lastLODCamPos.assign(m_planets.size(), glm::dvec3(1e300));
    if (m_lastUpdates.size() != m_planets.size())
        m_lastUpdates.assign(m_planets.size(), LODUpdate{});

    // Islas flotantes (genVersion>=2): streaming-lite. Regenera el casquete de
    // islas alrededor de la cámara cuando esta se mueve lo bastante → islas
    // descubribles al explorar, sin generar millones en todo el planeta.
    if (m_islandRenderer) {
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
    // Si hay chunks generándose/encolados, mantén la ventana de catch-up abierta (~1 s).
    if (m_streaming && (m_streaming->getPendingCount() + m_streaming->getQueuedCount()) > 0)
        m_catchupGrace = 60;
    else if (m_catchupGrace > 0) --m_catchupGrace;
    for (size_t pi = 0; pi < m_planets.size(); ++pi) {
        auto& planet = m_planets[pi];

        // DIAG (throttled ~2 s, también en reposo): deseados(load+keep) del ÚLTIMO
        // update vs gpu/cache/pend/queued. Si deseados >> gpu con pend=0 y queued=0
        // estando quieto → algo no se sube / se queda fuera (no completa el planeta).
        if ((s_lf % 120) == 0 && pi < m_lastUpdates.size()) {
            const auto& u = m_lastUpdates[pi];
            const double alt = (glm::length(cameraPos - glm::dvec3(planet.position)) - planet.radius)
                             / std::max(1.0, planet.radius);
            fprintf(stderr, "[LOD] '%s' alt=%.3f deseados=%zu (load%zu+keep%zu) gpu=%d cache=%d pend=%d queued=%d\n",
                    planet.name.c_str(), alt,
                    u.chunksToLoad.size() + u.chunksToKeep.size(),
                    u.chunksToLoad.size(), u.chunksToKeep.size(),
                    m_renderer ? m_renderer->getGPUMeshCount() : -1,
                    m_cache ? (int)m_cache->getChunkCount() : -1,
                    m_streaming ? m_streaming->getPendingCount() : -1,
                    m_streaming ? m_streaming->getQueuedCount() : -1);
        }

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
        const bool refining = (pi < m_lastUpdates.size() && m_lastUpdates[pi].residencyLimited);
        const bool refineNow = refining && ((s_lf % 4) == 0);
        if (!m_forceLOD && !refineNow && moved < moveThresh) {
            // Cámara quieta: NO recalculamos el LOD (caro). Solo de vez en cuando
            // (cada ~6 frames) reprocesamos el último set para SUBIR lo recién
            // generado (idempotente, comprobando residencia → sin recopiar lo que ya
            // está). Sin esto los chunks no aparecen hasta moverte; haciéndolo cada
            // frame costaba ~45 ms (copiaba todo el agua siempre).
            if (m_catchupGrace > 0 && (s_lf % 3) == 0) {
                m_streaming->processLODUpdate(m_lastUpdates[pi]); // terreno (ya filtra por residencia)
                if (m_waterRenderer && m_cache)
                    for (const auto& k : m_lastUpdates[pi].chunksToKeep) {
                        if (m_waterRenderer->isResident(k)) continue; // ya en GPU → no recopiar
                        ChunkData wd;
                        if (m_cache->getChunkCopy(k, wd)) m_waterRenderer->addToScene(planet.name, k, wd);
                    }
            }
            continue;
        }
        m_lastLODCamPos[pi] = lodCamPos;

        SceneObject planetProxy;                 // pila, sin make_shared por frame
        planetProxy.name = planet.name;
        planetProxy.position = planet.position;
        planetProxy.scale = glm::dvec3(planet.radius);

        // A. ¿Qué chunks deben verse? (firma toma shared_ptr → deleter no-op)
        //    Usa la posición ADELANTADA (lookahead) → prefetch en la dirección de marcha.
        //    Predicado de residencia → carga PROGRESIVA grueso→fino (sin huecos negros).
        //    SNAPSHOT del set residente UNA vez (un solo lock) → el LOD lo consulta miles
        //    de veces sin re-bloquear el mutex por nodo (antes: ~900 ms por contención).
        static std::unordered_set<uint64_t> s_resident; // reusa buffer entre frames
        if (m_renderer) m_renderer->residentHashes(s_resident);
        auto residentFn = [](const PlanetChunkKey& k) { return s_resident.count(ChunkCache::keyToHash(k)) != 0; };
        LODUpdate update = m_lod->updatePlanetLOD(
            std::shared_ptr<SceneObject>(&planetProxy, [](SceneObject*){}), lodCamPos, residentFn);
        m_lastUpdates[pi] = update; // para el catch-up en throttle

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
            m_streaming->setDesiredChunks(std::move(sorted), streamSettings);
        }

        // C. Cargar desde caché a GPU (la descarga es DIFERIDA, abajo)
        m_streaming->processLODUpdate(update);

        // El streaming solo sube el TERRENO desde caché. El AGUA debe seguir la
        // MISMA vida que el terreno (subirse desde caché aquí también), o al
        // recargar un chunk vuelve el terreno pero no el agua → mallas de agua
        // desincronizadas/huérfanas que se solapan (los "blobs"). addToScene se
        // auto-salta si ya está, así que reañadir es barato.
        if (m_waterRenderer && m_cache)
            for (const auto& k : update.chunksToLoad) {
                ChunkData wd;
                if (m_cache->getChunkCopy(k, wd))
                    m_waterRenderer->addToScene(planet.name, k, wd);
            }

        // Lo que sale de vista NO se borra: se marca STALE (sigue dibujándose). El
        // renderer lo retira solo cuando su área queda cubierta por el reemplazo
        // (padre al fusionar, o los 4 hijos al subdividir) vía purgeStaleCoveredBy.
        // Así nunca hay un agujero entre quitar el viejo y subir el nuevo — el mismo
        // mecanismo "sin hueco" que ya usaba el dig, ahora general para todo el LOD.
        for (const auto& k : update.chunksToUnload) {
            m_renderer->markStale(k);
            if (m_waterRenderer) m_waterRenderer->markStale(k);
        }
    }
    m_forceLOD = false;

    // Despachar generación pendiente (cercanos primero) CADA frame, aunque el LOD
    // esté en throttle por estar quieto → los chunks lejanos siguen entrando.
    m_streaming->pump();

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
        m_renderer->addToScene(chunk->planetName, chunk->key, *chunk);
        if (m_waterRenderer) m_waterRenderer->addToScene(chunk->planetName, chunk->key, *chunk);
    }
}

void PlanetarySystem::updateOrbits(double dt) {
    // Aquí mueves las posiciones de m_planets usando Kepler o integración Euler
    // Por ahora, podrías dejarlos estáticos o con una rotación simple
}

void PlanetarySystem::addPlanet(const Planet& planet) {
    m_planets.push_back(planet);
    m_forceLOD = true; // recompute LOD next update so the new planet appears at once
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
                    planet.terrainSettings["config"]["chunkSize"] = ts.chunkSize > 0 ? ts.chunkSize : 32;
            } else {
                planet.terrainSettings["config"]["chunkSize"] = ts.chunkSize > 0 ? ts.chunkSize : 32;
                planet.terrainSettings["config"]["seed"]      = ts.seed;
                for (const auto& [name, layer] : ts.layers) {
                    planet.terrainSettings["config"]["layers"][name]["freq"]     = layer.freq;
                    planet.terrainSettings["config"]["layers"][name]["octaves"]  = layer.octaves;
                    planet.terrainSettings["config"]["layers"][name]["strength"] = layer.strength;
                }
            }
            addPlanet(planet);
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
            gp.lakeDensity = W.lakeDensity; gp.lakeMaxProb = W.lakeMaxProb; gp.radius = p.radius;
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
    for (auto& planet : m_planets) {
        for (const auto& objPtr : scene.getAllObjects()) {
            if (!objPtr || objPtr->name != planet.name) continue;
            planet.position = objPtr->position;
            planet.radius   = std::max({objPtr->scale.x, objPtr->scale.y, objPtr->scale.z});
            break;
        }
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

void PlanetarySystem::setTerrainCullMatrix(const glm::mat4& camRelViewProj) {
    if (m_renderer)      m_renderer->setCullMatrix(camRelViewProj);
    if (m_waterRenderer) m_waterRenderer->setCullMatrix(camRelViewProj);
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

    if (dir.empty()) { glUniform1i(23, 0); return; } // sin texturas → procedural

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
    if (has) {
        m_texSandAlbedo->use(4);  glUniform1i(21, 4);
        if (m_texSandNormal) { m_texSandNormal->use(5); glUniform1i(22, 5); }
        m_texGrassAlbedo->use(6); glUniform1i(24, 6);
        m_texLandAlbedo->use(7);  glUniform1i(25, 7);
        if (m_texLandNormal) { m_texLandNormal->use(8); glUniform1i(26, 8); }
    }
    glUniform1i(23, has ? 1 : 0);
}

void PlanetarySystem::renderPlanetTerrain(const std::string& planetName, const glm::dvec3& cameraPos) {
    if (!m_renderer) return;
    const Planet* planet = nullptr;
    for (const auto& p : m_planets)
        if (p.name == planetName) { m_renderer->setPlanetCenter(p.position); planet = &p; break; }
    if (planet) bindTerrainTextures(*planet);
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
    m_waterRenderer->renderPlanet(planetName, cameraPos);
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
void PlanetarySystem::setCacheMaxMemoryMB(int mb) { if (m_cache) m_cache->setMaxMemory((size_t)std::max(16, mb)); }
void PlanetarySystem::setLODParams(double splitFactor, int maxLOD) {
    if (!m_lod) return;
    if (m_lod->getSplitFactor() == splitFactor && m_lod->getMaxLOD() == maxLOD) return;
    m_lod->setParams(splitFactor, maxLOD);
    m_forceLOD = true; // re-evaluate the quadtree next update
}

PlanetarySystem::TerrainDrawStats PlanetarySystem::getTerrainDrawStats() const {
    if (!m_renderer) return {};
    const auto s = m_renderer->getDrawStats();
    return { s.draws, s.vertices, s.triangles };
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

double PlanetarySystem::sampleTerrainHeight(const glm::dvec3& worldPos) const {
    if (!m_generator || m_planets.empty()) return 0.0;

    // Find the nearest planet to the query position.
    const Planet* nearest = nullptr;
    double bestDist = 1e300;
    for (const auto& p : m_planets) {
        double d = glm::length(p.position - worldPos);
        if (d < bestDist) { bestDist = d; nearest = &p; }
    }
    if (!nearest) return 0.0;

    // Direction from planet center to the query point (unit sphere direction).
    glm::dvec3 dir = worldPos - nearest->position;
    double len = glm::length(dir);
    if (len < 1e-9) return 0.0;
    glm::vec3 sphereDir = glm::vec3(dir / len);

    // Build settings JSON the same way streaming does (config lives under terrainSettings).
    nlohmann::json settings = nearest->terrainSettings;
    settings["planetOffsetX"] = nearest->position.x;
    settings["planetOffsetY"] = nearest->position.y;
    settings["planetOffsetZ"] = nearest->position.z;
    return double(m_generator->sampleHeightAt(sphereDir, settings, nearest->radius));
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
        }
    }
    m_forceLOD = true; // recomputa el LOD el próximo update → re-pide los chunks liberados
}

}