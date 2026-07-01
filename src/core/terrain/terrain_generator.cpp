#include "terrain_generator.h"
#include "tools/planetary_types.h"
#include "core/noise_generator.h"
#include "core/terrain/terrain_sampler_v2.h"
#include "core/terrain/gpu_heightfield.h"
#include "core/terrain/shared_index_table.h" // índices compartidos por res (ahorro RAM caché)
#include <algorithm>
#include <cmath>     // std::lround (#4 empaquetado de posiciones)
#include <functional>
#include <mutex>
#include <glm/gtc/packing.hpp> // #4 packSnorm3x10_1x2 (empaquetar normales)
#include "core/modules.h"
#ifdef HARUKA_MOD_DEFORM
#include "core/terrain/deformation_field.h"
#endif
#include <cstdio>


namespace Haruka {

TerrainGenerator::~TerrainGenerator() = default;

// Cachea los WorldGenParams del v2 por seed (deriveWorldParams hace una calibración
// CDF de ~2k muestras; no queremos repetirla en cada chunk). Recalcula solo si la
// seed cambia. Protegido por mutex porque generateChunk corre en hilos worker.
static WorldGenParams getWorldParamsCached(uint32_t seed, double planetRadius) {
    static std::mutex     mtx;
    static bool           valid = false;
    static uint32_t       cachedSeed = 0;
    static WorldGenParams cached;
    std::lock_guard<std::mutex> lock(mtx);
    if (!valid || cachedSeed != seed) {
        cached = deriveWorldParams(seed, planetRadius);
        cachedSeed = seed;
        valid = true;
    }
    return cached;
}

// Global (thread-local) seed used by calculateHeight when the JSON node
// passed doesn't provide direct access to the parent config object.
namespace {
    thread_local int g_current_seed = 42;
}

    glm::dvec3 TerrainGenerator::getLocalPosition(const PlanetChunkKey& key, int x, int y, int chunkSize) {
        // u, v in [0,1] for the whole face.
        // At LOD d there are 2^d chunks per axis; chunk (key.x, key.y) covers
        // u in [key.x/2^d, (key.x+1)/2^d], v in [key.y/2^d, (key.y+1)/2^d].
        double numChunks = double(1 << key.lod);
        double invChunks = 1.0 / numChunks;
        double invSize   = 1.0 / double(chunkSize);

        double u = (double(key.x) + double(x) * invSize) * invChunks;
        double v = (double(key.y) + double(y) * invSize) * invChunks;

        // Map [0,1] → [-1,1] for face projection
        double localX = u * 2.0 - 1.0;
        double localY = v * 2.0 - 1.0;

        // 2. Proyectar según la cara (Face: 0-5)
        glm::dvec3 p;
        switch (key.face) {
            case PlanetFace::FRONT: p = { 1.0,    localY, -localX }; break;
            case PlanetFace::BACK: p = { -1.0,   localY,  localX }; break;
            case PlanetFace::TOP: p = { localX,  1.0,   -localY }; break;
            case PlanetFace::BOTTOM: p = { localX, -1.0,    localY }; break;
            case PlanetFace::RIGHT: p = { localX,  localY,  1.0 };    break;
            case PlanetFace::LEFT: p = { -localX, localY, -1.0 };    break;
        }

        // 3. Convertir cubo a esfera (Spherify) para evitar distorsión en las esquinas
        double x2 = p.x * p.x;
        double y2 = p.y * p.y;
        double z2 = p.z * p.z;
        
        glm::dvec3 spherePos;
        spherePos.x = p.x * sqrt(1.0 - y2 / 2.0 - z2 / 2.0 + y2 * z2 / 3.0);
        spherePos.y = p.y * sqrt(1.0 - z2 / 2.0 - x2 / 2.0 + z2 * x2 / 3.0);
        spherePos.z = p.z * sqrt(1.0 - x2 / 2.0 - y2 / 2.0 + x2 * y2 / 3.0);

        return glm::dvec3(spherePos);
    }

    std::shared_ptr<ChunkData> TerrainGenerator::generateChunk(const PlanetChunkKey& key, const nlohmann::json& settings, double planetRadius, bool mainThread,
                                                               const std::vector<float>* preElev, const std::vector<glm::vec3>* preNormal,
                                                               const std::vector<float>* preWater)
    {
        auto chunk = std::make_shared<ChunkData>();
        static const nlohmann::json kEmptyObj = nlohmann::json::object();
        const auto& config = settings.contains("config") ? settings["config"] : kEmptyObj;
        int res = config.value("chunkSize", 24); // 24 (antes 32): ~53% de RAM por chunk
        g_current_seed = config.value("seed", 42);
        
        const int vtxCount  = (res + 1) * (res + 1);
        const int skirtVtxCount = 4 * (res + 1);
        chunk->vertices.reserve(vtxCount + skirtVtxCount);
        chunk->morphTargets.reserve(vtxCount + skirtVtxCount);
        chunk->normals.reserve(vtxCount + skirtVtxCount);
        chunk->uvs.reserve(vtxCount + skirtVtxCount);
        chunk->planetRadius = planetRadius;

        // Determine generation mode from settings ("type" field, default Procedural)
        const std::string modeStr = settings.value("type", "ProceduralPlanetary");
        const bool isManual = (modeStr == "Manual");
        chunk->terrainMode = isManual ? TerrainMode::Manual : TerrainMode::Procedural;

        // Chunk center: sphere direction of the face-center vertex, scaled to planet radius.
        // Stored as double for the model-matrix translation (keeps per-vertex floats small).
        // chunkCenter_local: posición PLANET-LOCAL del centro del chunk (para restar a
        // los vértices). chunk->chunkCenter se guarda TAMBIÉN planet-local (sin offset):
        // el renderer le suma el centro ACTUAL del planeta → el cuerpo puede MOVERSE
        // (orbitar) sin regenerar chunks. (Los vértices ya son planet-local.)
        glm::dvec3 chunkCenter_local;
        {
            glm::dvec3 centerDir = glm::dvec3(getLocalPosition(key, res / 2, res / 2, res));
            chunkCenter_local    = glm::normalize(centerDir) * planetRadius;
            chunk->chunkCenter   = chunkCenter_local; // PLANET-LOCAL
        }

        const auto& layers = config.contains("layers") ? config["layers"] : kEmptyObj;
        float invRes = 1.0f / float(res);
        const float kmToFraction = float(1000.0 / planetRadius);

        // World offset of this planet (to place vertices in absolute world space
        // for sampling the deformation field, which lives in world coordinates).
        const glm::dvec3 planetOffset(
            settings.value("planetOffsetX", 0.0),
            settings.value("planetOffsetY", 0.0),
            settings.value("planetOffsetZ", 0.0));
        (void)planetOffset; // only used by the DEFORM module

        // Generación v2 (rediseño A→B con normal analítica). Opt-in por escena:
        // terrainSettings.config.genVersion >= 2. Por defecto = v1 (sin cambios).
        const int  genVersion = config.value("genVersion", 1);
        const bool useV2 = (genVersion >= 2) && !isManual;
        WorldGenParams wgp = useV2
            ? getWorldParamsCached((uint32_t)g_current_seed, planetRadius)
            : WorldGenParams{};
        wgp.reliefStrength = config.value("reliefStrength", 1.0f); // parámetro de escena
        { const std::string pr = config.value("profile", std::string("terran"));
          wgp.profile = (pr == "moon") ? 1 : (pr == "gas") ? 2 : 0; }
        // ¿Está activo el deform? Si lo está, la normal vuelve a diferencias finitas
        // (para que los cráteres tengan normal correcta); si no, normal analítica v2.
        bool deformActive = false;
#ifdef HARUKA_MOD_DEFORM
        deformActive = (m_deform && !m_deform->empty());
#endif
        const bool useAnalyticNormal = useV2 && !deformActive;

        // === Camino GPU (opt-in: config.gpuTerrain) — incremento de la migración ===
        // El streaming asíncrono ya calculó elevación+normal del grid en compute
        // (Perlin portado, paridad ✓) y los pasa AQUÍ (preElev/preNormal). Las ramas
        // de abajo los USAN en vez de muestrear el ruido en CPU. Todo guardado tras
        // useGpu → sin GPU (preElev=null) el camino CPU es idéntico.
        (void)mainThread;
        const int vtxN = (res + 1) * (res + 1);
        const bool useGpu = (useV2 && !deformActive && wgp.profile == 0 &&
                             preElev && preNormal && preWater &&
                             (int)preElev->size() == vtxN && (int)preNormal->size() == vtxN &&
                             (int)preWater->size() == vtxN);
        static const std::vector<float>     s_emptyF;
        static const std::vector<glm::vec3> s_emptyN;
        const std::vector<float>&     gpuElevKm   = useGpu ? *preElev   : s_emptyF;
        const std::vector<glm::vec3>& gpuNormal   = useGpu ? *preNormal : s_emptyN;
        const std::vector<float>&     gpuWaterKm  = useGpu ? *preWater  : s_emptyF;
        auto gridIdx = [res](int x, int y) { return (size_t)(x + y * (res + 1)); };

        // Procedural elevation (km) along a sphere direction, INCLUDING player edits
        // (the deformation field). Single source of truth so render and collision
        // agree (getTerrainHeightAt uses the same path via sampleHeightAt).
        auto elevKmAt = [&](const glm::vec3& sph) -> float {
            float e = isManual ? 0.0f
                    : (useV2 ? sampleTerrainV2(sph, wgp, planetRadius).elevKm
                             : calculateHeight(sph, layers)); // km
#ifdef HARUKA_MOD_DEFORM
            if (m_deform && !m_deform->empty()) {
                // Vertex world pos at the *procedural* surface, then sample edits.
                glm::dvec3 surfW = glm::dvec3(sph) * (planetRadius * (1.0 + double(e * kmToFraction))) + planetOffset;
                e = float(m_deform->applyHeight(surfW, double(e) * 1000.0) * 0.001); // m→km (aditivo + nivelado)
            }
#endif
            return e;
        };

        // Posición local (relativa a chunkCenter_local) para CUALQUIER índice de
        // rejilla i,j — incluso fuera de [0,res]. Se usa para los vértices y para
        // muestrear el anillo vecino al calcular normales en los bordes (sin costuras).
        auto localPosAt = [&](int i, int j) -> glm::vec3 {
            glm::dvec3 dir = getLocalPosition(key, i, j, res);     // dir en DOUBLE (sin escalones)
            float h = elevKmAt(glm::vec3(dir)) * kmToFraction;
            glm::dvec3 absP = dir * (planetRadius * (1.0 + double(h)));
            return glm::vec3(absP - chunkCenter_local);
        };

        // Hundir bajo el mar los ISLOTES sub-celda a LOD grueso → no asoman como motas
        // marrones/huecos en el mar a distancia (aliasing del terreno). Decisión SOLO
        // por posición (igual que la normal analítica) para que dos chunks vecinos —de
        // cualquier LOD— tomen EXACTAMENTE la misma decisión en el borde compartido (sin
        // costuras): si la elevación REGIONAL (baja frecuencia, sin detalle fino) es
        // oceánica pero el detalle fino apenas emerge, es un islote sub-celda → al mar.
        // 'seaBiasKm' (cuánto detalle fino se perdona) escala con el espaciado del chunk
        // → a ras de suelo ≈0 (no toca nada) y el islote emerge al acercarte.
        const double nodeSizeM_   = planetRadius * 2.0 / double(1u << key.lod);
        const float  vtxSpacingM_ = float(nodeSizeM_ / double(res));
        const float  seaBiasKm    = glm::min(0.15f, vtxSpacingM_ * 0.03f / 1000.0f);
        auto adjElevKm = [&](int gx, int gy) -> float {
            float e = gpuElevKm[gridIdx(gx, gy)];
            if (e <= 0.0f || e >= seaBiasKm) return e;     // ya en el mar o claramente tierra
            glm::vec3 sph = glm::vec3(getLocalPosition(key, gx, gy, res));
            // Continente de baja frecuencia (misma fBm que el sampler): si la región
            // está por DEBAJO del umbral de mar, el detalle fino que asoma es un islote
            // sub-celda → al mar. Posición-only → idéntico en chunks vecinos (sin costuras).
            float cont = NoiseGenerator::fBm(sph, (int)wgp.seed, 6, 0.5f, 2.0f, wgp.continentFreqA);
            return (cont <= wgp.seaThreshold) ? -0.001f : e;
        };

        float minElevKm = 1e30f;
        for (int y = 0; y <= res; ++y) {
            for (int x = 0; x <= res; ++x) {
                glm::dvec3 dir = getLocalPosition(key, x, y, res); // dir en DOUBLE (sin escalones)
                glm::vec3 posOnSphere = glm::vec3(dir);            // float: ruido/normales/uv (les basta)

                // Elevación: GPU (compute, con hundido de islotes) si useGpu, si no CPU.
                float elevKm = useGpu ? adjElevKm(x, y) : elevKmAt(posOnSphere);
                float height = elevKm * kmToFraction;          // fracción del radio
                if (elevKm < minElevKm) minElevKm = elevKm;

                glm::dvec3 absPos = dir * (planetRadius * (1.0 + double(height)));
                chunk->vertices.push_back(glm::vec3(absPos - chunkCenter_local));
                chunk->normals.push_back(posOnSphere); // placeholder; recomputado abajo
                // uv.x = shoreFactor (cercanía a CUALQUIER agua: mar y lagos), para
                // pintar arena en las orillas. uv.y = v (libre). v1 → uv normal.
                float shore = 0.0f;
                if (useGpu) {
                    // Orilla relativa al agua aplicable (mar 0 o lago L). Playa ~0–16 m.
                    float wl = std::max(gpuWaterKm[gridIdx(x, y)], 0.0f);
                    shore = 1.0f - glm::clamp((elevKm - wl) / 0.016f, 0.0f, 1.0f);
                } else if (useV2) {
                    TerrainSample ts = sampleTerrainV2(posOnSphere, wgp, planetRadius);
                    float aboveKm = ts.elevKm - ts.waterLevelKm; // km sobre el agua aplicable
                    shore = 1.0f - glm::clamp(aboveKm / 0.016f, 0.0f, 1.0f); // playa ~0–16 m
                }
                chunk->uvs.push_back(glm::vec2(useV2 ? shore : float(x) * invRes, float(y) * invRes));
            }
        }
        chunk->minElevation = (minElevKm < 1e29f) ? minElevKm : 0.0f;
        chunk->hasOcean     = (chunk->minElevation < 0.0f); // nivel del mar = elevación 0

        // Normales ANALÍTICAS con epsilon FIJO en espacio mundial. Clave para no
        // tener costuras de iluminación entre chunks de distinto LOD: la normal es
        // función solo de la posición esférica (no del espaciado de vértices del
        // chunk), así dos chunks vecinos evalúan EXACTAMENTE la misma normal en el
        // borde compartido. (Antes se usaba el spacing del chunk → fino y grueso
        // discrepaban → línea oscura en cada borde.)
        {
            // eps angular fijo (~ pocos metros sobre la esfera), igual para todo LOD.
            const float epsAng = float(2.0 / planetRadius); // ~2 m de arco
            // Posición desplazada (planet-local) usando elevKmAt → incluye las
            // deformaciones, así las paredes de un cráter tienen normal correcta.
            auto dispAt = [&](const glm::vec3& dir) -> glm::dvec3 {
                glm::dvec3 d = glm::normalize(glm::dvec3(dir));
                float h = elevKmAt(glm::vec3(d)) * kmToFraction;
                return d * (planetRadius * (1.0 + double(h))) - chunkCenter_local;
            };
            for (int y = 0; y <= res; ++y) {
                for (int x = 0; x <= res; ++x) {
                    // GPU: normal ya calculada en el compute (eps fijo → seam-safe).
                    if (useGpu) { chunk->normals[gridIdx(x, y)] = gpuNormal[gridIdx(x, y)]; continue; }
                    glm::vec3 sph = getLocalPosition(key, x, y, res); // dir unitaria
                    // v2 sin deform: normal ANALÍTICA (gradiente exacto) → sin pinchos
                    // y seam-safe (función solo de la dirección, igual que la FD fija).
                    if (useAnalyticNormal) {
                        chunk->normals[x + y * (res + 1)] = sampleTerrainV2(sph, wgp, planetRadius).normal;
                        continue;
                    }
                    // Dos tangentes ortogonales a la dirección esférica.
                    glm::vec3 ref = (std::abs(sph.y) < 0.9f) ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
                    glm::vec3 t1 = glm::normalize(glm::cross(ref, sph));
                    glm::vec3 t2 = glm::normalize(glm::cross(sph, t1));
                    // Muestreo central a ±epsAng a lo largo de cada tangente.
                    glm::dvec3 px = dispAt(sph + t1 * epsAng) - dispAt(sph - t1 * epsAng);
                    glm::dvec3 py = dispAt(sph + t2 * epsAng) - dispAt(sph - t2 * epsAng);
                    glm::dvec3 nd = glm::cross(px, py);
                    double len = glm::length(nd);
                    glm::vec3 n = (len > 1e-12) ? glm::vec3(nd / len) : sph;
                    if (glm::dot(n, sph) < 0.0f) n = -n; // hacia afuera
                    chunk->normals[x + y * (res + 1)] = n;
                }
            }
        }

        // 1b. Morph targets (CDLOD): la posición que cada vértice tendría en el LOD
        // padre = malla decimada a media resolución. Se obtiene interpolando
        // bilinealmente entre los vértices de índice PAR (los que sobreviven al
        // decimado). En el shader se mezcla pos↔morphTarget según distancia, así
        // el chunk fino "se aplana" hacia la forma del padre antes del cambio de
        // LOD → sin popping.
        {
            auto vidx = [res](int x, int y) { return x + y * (res + 1); };
            chunk->morphTargets.resize(chunk->vertices.size());
            chunk->morphNormals.resize(chunk->vertices.size());
            for (int y = 0; y <= res; ++y) {
                for (int x = 0; x <= res; ++x) {
                    int x0 = x & ~1, x1 = std::min(x0 + 2, res);
                    int y0 = y & ~1, y1 = std::min(y0 + 2, res);
                    float fx = (x1 > x0) ? float(x - x0) / float(x1 - x0) : 0.0f;
                    float fy = (y1 > y0) ? float(y - y0) / float(y1 - y0) : 0.0f;
                    glm::vec3 v00 = chunk->vertices[vidx(x0, y0)];
                    glm::vec3 v10 = chunk->vertices[vidx(x1, y0)];
                    glm::vec3 v01 = chunk->vertices[vidx(x0, y1)];
                    glm::vec3 v11 = chunk->vertices[vidx(x1, y1)];
                    glm::vec3 a = glm::mix(v00, v10, fx);
                    glm::vec3 b = glm::mix(v01, v11, fx);
                    chunk->morphTargets[vidx(x, y)] = glm::mix(a, b, fy);

                    // Normal decimada (mismo esquema) → al aplanar el chunk hacia el padre
                    // su normal también va a la del padre, así el sombreado casa con la
                    // geometría morfada (sin esto: rectángulo plano con normales "bumpy").
                    glm::vec3 n00 = chunk->normals[vidx(x0, y0)];
                    glm::vec3 n10 = chunk->normals[vidx(x1, y0)];
                    glm::vec3 n01 = chunk->normals[vidx(x0, y1)];
                    glm::vec3 n11 = chunk->normals[vidx(x1, y1)];
                    glm::vec3 na = glm::mix(n00, n10, fx);
                    glm::vec3 nb = glm::mix(n01, n11, fx);
                    chunk->morphNormals[vidx(x, y)] = glm::normalize(glm::mix(na, nb, fy));
                }
            }
        }

        // 2. Generar Índices (Triángulos)
        for (int y = 0; y < res; ++y) {
            for (int x = 0; x < res; ++x) {
                 int i = x + y * (res + 1);
                chunk->indices.push_back(i);
                chunk->indices.push_back(i + 1);
                chunk->indices.push_back(i + res + 1);

                chunk->indices.push_back(i + 1);
                chunk->indices.push_back(i + res + 2);
                chunk->indices.push_back(i + res + 1);
            }
        }

        // 3. Skirts: falda de geometría que cuelga por los 4 bordes del chunk.
        // Tapa las grietas (T-junctions) entre chunks de distinto nivel LOD.
        // La profundidad cubre la máxima variación de altura (~2x el max height en metros).
        // Skirt depth: enough to cover the maximum terrain height variation so
        // adjacent chunks at different LODs don't leave visible cracks, but not
        // so deep that they become visible "walls" at ground level.
        // Skirt depth CAPPED to a small absolute value. Scaling with vtxSpacing
        // made low-LOD chunks (huge nodeSize) hang skirts hundreds of metres deep,
        // which look like a "mountain wall" at chunk borders while a neighbour is
        // still streaming in. The morph already aligns neighbour edges, so the
        // skirt only needs to cover sub-pixel cracks: a few metres is plenty, and
        // we clamp so it never dominates the view.
        const double nodeSize = planetRadius * 2.0 / double(1u << key.lod);
        const float  vtxSpacing = float(nodeSize / double(res));
        // Skirt ADAPTATIVO: la grieta T-junction entre un chunk y su vecino de 1 LOD de
        // diferencia ≈ variación de altura sobre un espaciado de vértice → escala con el
        // espaciado Y con el relieve local del chunk (en pendientes fuertes una grieta supera
        // de largo cualquier tope fijo). Antes era clamp(spacing*0.5, 1, 30) → al bajar
        // chunkSize (spacing mayor) y en laderas, 30 m se quedaba corto → AGUJEROS (cielo
        // colándose). Ahora: el mayor de (spacing) y (15% del relieve del chunk), cap alto.
        // El faldón cuelga HACIA DENTRO, tapado por el chunk vecino → más profundo no se ve.
        // Profundidad MODESTA: la grieta entre vecinos (≤1 LOD por el balance 2:1) ≈ 1
        // espaciado de vértice de variación. Cae radialmente (oculto), así que no hace falta
        // pasarse: 200 m exponía MUROS cuando un borde quedaba al aire. Escala con el
        // espaciado (cubre el aumento de chunkSize) con tope bajo.
        const float skirtDepth = glm::clamp(vtxSpacing * 0.8f, 2.0f, 50.0f);

        const int mainCount = (res + 1) * (res + 1);

        // Helper: añade un vértice de falda (empuja hacia dentro de la esfera).
        // Todas las lecturas se copian ANTES del push_back para evitar UB por
        // revalidación de referencias cuando el vector realloca.
        auto addSkirt = [&](int mainIdx) {
            glm::vec3 v  = chunk->vertices[mainIdx];
            glm::vec3 n  = chunk->normals[mainIdx];
            glm::vec2 uv = chunk->uvs[mainIdx];
            // El faldón cae RADIALMENTE (hacia el centro del planeta), NO por la normal del
            // vértice: en una pendiente la normal apunta de lado → con profundidad grande el
            // skirt asomaba como aletas. El radial siempre queda por DEBAJO del terreno → oculto.
            glm::vec3 radial = glm::vec3(glm::normalize(chunkCenter_local + glm::dvec3(v)));
            glm::vec3 sv = v - radial * skirtDepth;
            chunk->vertices.push_back(sv);
            chunk->morphTargets.push_back(sv); // skirts don't morph
            chunk->normals.push_back(n);
            chunk->morphNormals.push_back(n);  // skirts no morphan → su propia normal
            chunk->uvs.push_back(uv);
        };

        // Índice base de los vértices de falda para cada borde.
        const int sB = mainCount;            // bottom (y=0),   res+1 verts
        const int sT = sB + (res + 1);      // top    (y=res), res+1 verts
        const int sL = sT + (res + 1);      // left   (x=0),   res+1 verts
        const int sR = sL + (res + 1);      // right  (x=res), res+1 verts

        for (int x = 0; x <= res; ++x) addSkirt(x);                        // bottom
        for (int x = 0; x <= res; ++x) addSkirt(res * (res + 1) + x);     // top
        for (int y = 0; y <= res; ++y) addSkirt(y * (res + 1));            // left
        for (int y = 0; y <= res; ++y) addSkirt(y * (res + 1) + res);     // right

        // Quads de falda — winding: la cara exterior de la falda debe ser CCW vista
        // desde fuera de la esfera. Los skirts van hacia dentro (normal negativa), por
        // lo que el winding se invierte respecto al terreno principal.
        auto addSkirtQuad = [&](int a, int b, int sa, int sb) {
            // a, b: vértices del borde principal (orden a lo largo del borde)
            // sa, sb: vértices de falda correspondientes
            chunk->indices.push_back(a);  chunk->indices.push_back(sa); chunk->indices.push_back(b);
            chunk->indices.push_back(b);  chunk->indices.push_back(sa); chunk->indices.push_back(sb);
        };

        // Bottom (y=0): main verts 0..res, skirt at sB
        for (int x = 0; x < res; ++x)
            addSkirtQuad(x, x + 1, sB + x, sB + x + 1);

        // Top (y=res): main verts res*(res+1)..res*(res+1)+res, skirt at sT
        // Reversed order so the outward face stays CCW from outside
        for (int x = 0; x < res; ++x)
            addSkirtQuad(res*(res+1)+x+1, res*(res+1)+x, sT+x+1, sT+x);

        // Left (x=0): main verts 0, res+1, 2*(res+1)…, skirt at sL (y is index)
        for (int y = 0; y < res; ++y)
            addSkirtQuad((y+1)*(res+1), y*(res+1), sL+y+1, sL+y);

        // Right (x=res): main verts res, (res+1)+res…, skirt at sR
        for (int y = 0; y < res; ++y)
            addSkirtQuad(y*(res+1)+res, (y+1)*(res+1)+res, sR+y, sR+y+1);

        // Skirt de agua: faldón vertical en los bordes MOJADOS del chunk, colgando
        // hacia el centro del planeta. Tapa las grietas T-junction entre LODs vecinos
        // (donde un chunk fino tiene más vértices en el borde que el grueso) y las que
        // abre el oleaje del shader al desplazar de forma distinta cada LOD. Solo en
        // bordes con agua → no cuelga "paredes" de agua en tierra seca. El agua se
        // dibuja two-sided (cull off), así que el winding del faldón es indiferente.
        // El faldón es ~mitad del espaciado (suelo 8 m para cubrir el desplazamiento
        // de las olas, techo 40 m para no colgar muros en LODs gruesos).
        auto buildWaterSkirt = [&](int side, const std::function<bool(int,int)>& cellWet) {
            // Skirt del agua MÍNIMO: el recorte por profundidad con signo tapa la costa y el
            // morph CDLOD alinea los bordes entre LODs → el faldón grande (antes 8–40m) solo
            // colgaba visible en cada borde de chunk = streaks verticales a distancia. Ahora
            // tope bajo: solo tapa grietas sub-píxel sin asomar.
            const float wSkirt = glm::clamp(vtxSpacing * 0.15f, 0.5f, 6.0f);
            const bool  hasParams = !chunk->waterParams.empty();
            const int   mainCount = (int)chunk->waterVertices.size();
            std::vector<int> skirtOf((size_t)mainCount, -1);
            const bool hasMorph = !chunk->waterMorphTargets.empty();
            auto skirtVtx = [&](int mainIdx) -> int {
                if (skirtOf[mainIdx] >= 0) return skirtOf[mainIdx];
                int s = (int)chunk->waterVertices.size();
                glm::vec3 n = chunk->waterNormals[mainIdx];
                glm::vec3 sv = chunk->waterVertices[mainIdx] - n * wSkirt;
                chunk->waterVertices.push_back(sv);
                chunk->waterNormals.push_back(n);
                if (hasParams) chunk->waterParams.push_back(chunk->waterParams[mainIdx]);
                if (hasMorph)  chunk->waterMorphTargets.push_back(sv); // el faldón no morfa
                skirtOf[mainIdx] = s;
                return s;
            };
            auto edge = [&](int a, int b) {
                int sa = skirtVtx(a), sb = skirtVtx(b);
                chunk->waterIndices.push_back(a);  chunk->waterIndices.push_back(b);  chunk->waterIndices.push_back(sb);
                chunk->waterIndices.push_back(a);  chunk->waterIndices.push_back(sb); chunk->waterIndices.push_back(sa);
            };
            const int r = side - 1; // = res
            for (int x = 0; x < r; ++x) if (cellWet(x, 0))     edge(x, x + 1);                     // borde inferior
            for (int x = 0; x < r; ++x) if (cellWet(x, r - 1)) edge(r*side + x, r*side + x + 1);   // borde superior
            for (int y = 0; y < r; ++y) if (cellWet(0, y))     edge(y*side, (y + 1)*side);         // borde izquierdo
            for (int y = 0; y < r; ++y) if (cellWet(r - 1, y)) edge(y*side + r, (y + 1)*side + r); // borde derecho
        };

        // Morph CDLOD del AGUA: para cada vértice del grid, su posición en el LOD padre
        // (malla decimada a media resolución, bilineal entre índices PAR) → el shader
        // mezcla pos↔morph según distancia, igual que el terreno, así la lámina no
        // "salta" de teselación al cambiar de LOD. Llamar ANTES del skirt (que apila su
        // propio morph identidad). 'side' = res+1; el grid tiene side² vértices.
        auto buildWaterMorph = [&](int side) {
            const int rr = side - 1; // res
            const int n  = side * side;
            chunk->waterMorphTargets.resize((size_t)n);
            auto wvidx = [side](int x, int y) { return x + y * side; };
            for (int y = 0; y <= rr; ++y)
            for (int x = 0; x <= rr; ++x) {
                int x0 = x & ~1, x1 = std::min(x0 + 2, rr);
                int y0 = y & ~1, y1 = std::min(y0 + 2, rr);
                float fx = (x1 > x0) ? float(x - x0) / float(x1 - x0) : 0.0f;
                float fy = (y1 > y0) ? float(y - y0) / float(y1 - y0) : 0.0f;
                glm::vec3 a = glm::mix(chunk->waterVertices[wvidx(x0,y0)], chunk->waterVertices[wvidx(x1,y0)], fx);
                glm::vec3 b = glm::mix(chunk->waterVertices[wvidx(x0,y1)], chunk->waterVertices[wvidx(x1,y1)], fx);
                chunk->waterMorphTargets[wvidx(x,y)] = glm::mix(a, b, fy);
            }
        };

        // --- Malla de agua v2: océano (nivel 0) + lagos (nivel local L) ---
        // Nivel POR VÉRTICE: océano a 0, lago a su L. Se propaga el nivel del lago
        // un anillo hacia la orilla para que la lámina quede plana (sin cuña).
        if (useV2) {
            const int side = res + 1;
            const int wcount = side * side;
            const float NOLVL = -1e30f;
            std::vector<float>   levelKm((size_t)wcount, NOLVL);
            std::vector<float>   elevKmArr((size_t)wcount, 0.0f); // elev terreno (para profundidad)
            std::vector<uint8_t> wet((size_t)wcount, 0);
            bool anyWet = false;
            for (int y = 0; y <= res; ++y) {
                for (int x = 0; x <= res; ++x) {
                    size_t i = (size_t)(x + y * side);
                    float e, wl; bool isW;
                    if (useGpu) {
                        e = adjElevKm(x, y);            // misma elevación (con hundido de islotes) que el terreno
                        float gw = gpuWaterKm[gridIdx(x, y)];
                        wl = (gw > 0.0f) ? gw : 0.0f;   // mar 0, lago L (= waterLevelKm)
                        isW = e < wl;                   // mar: e<0; lago: e<L (== isWater())
                    } else {
                        glm::vec3 sph = getLocalPosition(key, x, y, res);
                        TerrainSample ts = sampleTerrainV2(sph, wgp, planetRadius);
                        e = ts.elevKm; wl = ts.waterLevelKm; isW = ts.isWater();
                    }
                    // Un vértice BAJO el nivel del mar es OCÉANO (nivel 0) aunque caiga
                    // dentro de un sello de lago (gw>0): el lago solo aplica en TIERRA
                    // (e>=0). Sin esto, mar+lago solapados marcaban el punto como lago
                    // (nivel L>0) → parche de agua ELEVADO sobre el mar y huecos en la
                    // lámina (el seabed marrón asomaba) = "mar roto" en océano abierto.
                    if (e < 0.0f) { wl = 0.0f; isW = true; }
                    elevKmArr[i] = e;
                    if (isW) { wet[i] = 1; anyWet = true; levelKm[i] = wl; } // océano 0, lago L
                }
            }
            if (anyWet) {
                // Propagar el nivel a vértices secos vecinos de agua (orilla plana).
                std::vector<float> lvl = levelKm;
                for (int y = 0; y <= res; ++y)
                for (int x = 0; x <= res; ++x) {
                    size_t i = (size_t)(x + y * side);
                    if (levelKm[i] > NOLVL) continue;
                    float best = NOLVL;
                    auto consider = [&](int xx, int yy){
                        if (xx<0||yy<0||xx>res||yy>res) return;
                        float v = levelKm[(size_t)(xx + yy*side)];
                        if (v > best) best = v;
                    };
                    consider(x-1,y); consider(x+1,y); consider(x,y-1); consider(x,y+1);
                    lvl[i] = (best > NOLVL) ? best : 0.0f;
                }
                chunk->waterVertices.reserve(wcount);
                chunk->waterNormals.reserve(wcount);
                chunk->waterParams.reserve(wcount);
                for (int y = 0; y <= res; ++y) {
                    for (int x = 0; x <= res; ++x) {
                        glm::dvec3 dir = getLocalPosition(key, x, y, res); // DOUBLE
                        size_t i = (size_t)(x + y * side);
                        float L = (lvl[i] > NOLVL) ? lvl[i] : 0.0f;
                        glm::dvec3 wp = dir * (planetRadius * (1.0 + double(L) * double(kmToFraction)));
                        chunk->waterVertices.push_back(glm::vec3(wp - chunkCenter_local));
                        chunk->waterNormals.push_back(glm::vec3(dir));
                        // profundidad del agua (m): nivel − terreno. CON SIGNO (negativa sobre
                        // tierra) → el shader descarta donde ≤0 y la lámina se recorta en el
                        // contorno EXACTO del nivel del mar (sub-celda, por píxel) SIN depender
                        // del depth buffer (que falla a distancia). Antes clampeada a ≥0 +
                        // any-corner = agua sobre tierra "tapada por el terreno" (dependía del
                        // depth → se colaba lejos). El interp lineal de la profundidad con signo
                        // cruza 0 en la costa → contorno suave a CUALQUIER LOD.
                        float depthM = (L - elevKmArr[i]) * 1000.0f;
                        chunk->waterParams.push_back(glm::vec2(L, depthM));
                    }
                }
                // Emisión de celdas de agua, limpia en TODO el rango de LOD:
                //   · OCÉANO (nivel 0): emite si CUALQUIER esquina está bajo el mar →
                //     costa sin huecos (el terreno ocluye el agua que sobra).
                //   · LAGO (nivel L>0): los lagos son features FINOS (0.1–2 km). A LOD
                //     grueso un lago es sub-celda → emitirlo da confeti/charcos falsos o,
                //     si exiges celda completa, bordes DENTADOS (triángulos). Solución:
                //     los lagos SOLO se dibujan cuando el chunk es lo bastante fino para
                //     resolverlos (vtxSpacing < umbral), y ahí con any-corner → bordes
                //     SUAVES. A LOD grueso no hay lago (terreno limpio); aparece, ya
                //     suave, al acercarte. levelKm: 0=océano, >0=lago, NOLVL=seco.
                const bool lakesAllowed = (vtxSpacingM_ < 250.0f);
                auto isOcean = [&](int idx){ return wet[idx] && levelKm[(size_t)idx] == 0.0f; };
                auto cellEmits = [&](int i)->bool {
                    bool anyOcean = isOcean(i) || isOcean(i+1) || isOcean(i+side) || isOcean(i+side+1);
                    if (anyOcean) return true;                     // océano: any-corner (costa sin huecos)
                    if (!lakesAllowed) return false;               // LOD grueso: sin lagos → terreno limpio
                    return wet[i] || wet[i+1] || wet[i+side] || wet[i+side+1]; // lago fino: any-corner (suave)
                };
                for (int y = 0; y < res; ++y)
                for (int x = 0; x < res; ++x) {
                    int i = x + y * side;
                    if (!cellEmits(i)) continue;
                    chunk->waterIndices.push_back(i);
                    chunk->waterIndices.push_back(i + 1);
                    chunk->waterIndices.push_back(i + side);
                    chunk->waterIndices.push_back(i + 1);
                    chunk->waterIndices.push_back(i + side + 1);
                    chunk->waterIndices.push_back(i + side);
                }
                buildWaterMorph(side);  // CDLOD del agua (antes del skirt)
                // Faldón en los mismos bordes que sí emiten agua.
                buildWaterSkirt(side, [&](int cx, int cy) { return cellEmits(cx + cy * side); });
                chunk->hasOcean = true; // reusa el flag: el renderer sube la malla de agua
            }
        }
        // --- Malla de agua v1 (cascarón a nivel del mar) ---
        // Solo si el chunk toca océano. Rejilla (res+1)² sobre la esfera al radio
        // del planeta (elevación 0), normales radiales. El oleaje se hace en el
        // shader, así que la base es lisa. Sin skirts (la esfera es suave).
        else if (chunk->hasOcean) {
            const int side = res + 1;
            const int wcount = side * side;
            chunk->waterVertices.reserve(wcount);
            chunk->waterNormals.reserve(wcount);
            // Per-vertex "below sea level" flag so we emit water ONLY where the
            // terrain dips under sea level — not a full sheet over the whole chunk.
            // That kills both the chunk-aligned "square" coastline AND the sea
            // bleeding through the land (z-fighting). The coastline now follows the
            // terrain at vertex resolution.
            // Emisión con MARGEN sobre el nivel del mar: emitimos las celdas cuya esquina está
            // bajo el mar O hasta +kEmitMarginKm por encima → la geometría de agua es CONTINUA a
            // través de la costa (no celdas-sliver sueltas que a distancia salen como PUNTOS/
            // halftone). El recorte EXACTO del contorno lo hace el discard por-píxel (water.frag,
            // Depth con signo); la geometría del margen sobre tierra se descarta ahí. Sin gen nueva
            // NO se ve (la malla de agua está cacheada → mundo nuevo para regenerar).
            const float kEmitMarginKm = 0.0f; // solo celdas mojadas (el margen daba geometría continua
                                              // PERO cerca, con el recorte per-vértice, hacía círculos
                                              // → peor trato que los puntos lejanos. Sin margen.)
            std::vector<uint8_t> nearSea((size_t)wcount, 0);
            for (int y = 0; y <= res; ++y) {
                for (int x = 0; x <= res; ++x) {
                    glm::dvec3 dir = getLocalPosition(key, x, y, res); // DOUBLE
                    glm::vec3 sph = glm::vec3(dir);
                    glm::dvec3 wp = dir * planetRadius;                // nivel del mar
                    chunk->waterVertices.push_back(glm::vec3(wp - chunkCenter_local));
                    chunk->waterNormals.push_back(sph);
                    float ek = useGpu ? gpuElevKm[gridIdx(x, y)] : elevKmAt(sph);
                    nearSea[(size_t)(x + y * side)] = (ek < kEmitMarginKm) ? 1 : 0;
                }
            }
            for (int y = 0; y < res; ++y) {
                for (int x = 0; x < res; ++x) {
                    int i = x + y * side;
                    // Emit the cell if ANY corner is near/below sea level (con margen) → costa
                    // continua; el discard por-píxel recorta el contorno exacto del nivel del mar.
                    bool wet = nearSea[i] || nearSea[i + 1]
                            || nearSea[i + side] || nearSea[i + side + 1];
                    if (!wet) continue;
                    chunk->waterIndices.push_back(i);
                    chunk->waterIndices.push_back(i + 1);
                    chunk->waterIndices.push_back(i + side);
                    chunk->waterIndices.push_back(i + 1);
                    chunk->waterIndices.push_back(i + side + 1);
                    chunk->waterIndices.push_back(i + side);
                }
            }
            buildWaterMorph(side);  // CDLOD del agua (antes del skirt)
            // Faldón en bordes mojados (celda mojada si cualquier esquina < nivel del mar).
            buildWaterSkirt(side, [&](int cx, int cy) {
                int i = cx + cy * side;
                return nearSea[i] || nearSea[i+1] || nearSea[i+side] || nearSea[i+side+1];
            });
        }

        // #4 EMPAQUETADO: normales y morphNormals → INT_2_10_10_10 (4B vs 12B). Se hace al FINAL
        // (ya rellenas, incluidos skirts) y se LIBERAN las vec3 → la cache y la VRAM guardan lo
        // pequeño (−16 B/vértice). 10 bits/comp normalizado = sobra para normales.
        {
            chunk->normalsPacked.resize(chunk->normals.size());
            for (size_t i = 0; i < chunk->normals.size(); ++i)
                chunk->normalsPacked[i] = glm::packSnorm3x10_1x2(glm::vec4(chunk->normals[i], 0.0f));
            chunk->morphNormalsPacked.resize(chunk->morphNormals.size());
            for (size_t i = 0; i < chunk->morphNormals.size(); ++i)
                chunk->morphNormalsPacked[i] = glm::packSnorm3x10_1x2(glm::vec4(chunk->morphNormals[i], 0.0f));
            std::vector<glm::vec3>().swap(chunk->normals);      // libera la RAM de las vec3
            std::vector<glm::vec3>().swap(chunk->morphNormals);

            chunk->uvsPacked.resize(chunk->uvs.size());          // uv → half-float ×2
            for (size_t i = 0; i < chunk->uvs.size(); ++i)
                chunk->uvsPacked[i] = glm::packHalf2x16(chunk->uvs[i]);
            std::vector<glm::vec2>().swap(chunk->uvs);
        }

        // Índices = topología fija por res → se REGISTRAN una vez en la tabla compartida y se
        // LIBERA la copia por-chunk (la caché RAM no duplica ~3456 índices/chunk). El render los
        // lee de SharedIndexTable vía indexCount. (waterIndices NO: son condicionales por celda.)
        chunk->indexCount = (uint32_t)chunk->indices.size();
        SharedIndexTable::get().registerOnce(chunk->indexCount, chunk->indices);
        std::vector<unsigned int>().swap(chunk->indices);

        return chunk;
    }

    TerrainSample TerrainGenerator::sampleSurfaceAt(const glm::vec3& sphereDir,
                                                    const nlohmann::json& settings,
                                                    double planetRadius)
    {
        static const nlohmann::json kEmpty = nlohmann::json::object();
        const auto& config = settings.contains("config") ? settings["config"] : kEmpty;
        g_current_seed = config.value("seed", 42);
        const auto& layers = config.contains("layers") ? config["layers"] : kEmpty;

        // Muestreo procedural (mismo sampler que el render v2 → colisión = geometría).
        const int genVersion = config.value("genVersion", 1);
        TerrainSample s;
        if (genVersion >= 2) {
            WorldGenParams wgp = getWorldParamsCached((uint32_t)g_current_seed, planetRadius);
            wgp.reliefStrength = config.value("reliefStrength", 1.0f); // parámetro de escena
            { const std::string pr = config.value("profile", std::string("terran"));
              wgp.profile = (pr == "moon") ? 1 : (pr == "gas") ? 2 : 0; }
            s = sampleTerrainV2(sphereDir, wgp, planetRadius);
        } else {
            s.elevKm = calculateHeight(sphereDir, layers); // v1: ya en km (·1000 = metros)
        }

        // Ediciones del jugador (deform): MISMA fuente que la malla → cavar un agujero y
        // caer en él. Se aplica a la elevación final (única verdad: render/colisión/agua).
#ifdef HARUKA_MOD_DEFORM
        if (m_deform && !m_deform->empty()) {
            glm::dvec3 offset(settings.value("planetOffsetX", 0.0),
                              settings.value("planetOffsetY", 0.0),
                              settings.value("planetOffsetZ", 0.0));
            float metres = s.elevKm * 1000.0f;
            glm::dvec3 surfW = glm::dvec3(glm::normalize(sphereDir)) * (planetRadius + double(metres)) + offset;
            s.elevKm = float(m_deform->applyHeight(surfW, double(metres))) * 0.001f; // m→km
        }
#endif
        return s;
    }

    // Wrapper: altura en metros sobre la esfera de referencia (= sampleSurfaceAt().elevKm·1000).
    float TerrainGenerator::sampleHeightAt(const glm::vec3& sphereDir,
                                            const nlohmann::json& settings,
                                            double planetRadius)
    {
        return sampleSurfaceAt(sphereDir, settings, planetRadius).elevKm * 1000.0f;
    }

    // smoothstep auxiliar (Hermite) usado por el modelo de elevación.
    static inline float sstep(float e0, float e1, float x) {
        float t = glm::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float TerrainGenerator::calculateHeight(const glm::vec3& pos, const nlohmann::json& layers) {
        // Modelo de elevación REALISTA tipo Tierra. Devuelve km.
        // TOTALMENTE data-driven: cada amplitud/frecuencia/octava sale de la escena
        // (layers["..."]) con DEFAULTS = los valores afinados que dan el terreno
        // bueno actual. Una escena vacía → terreno por defecto correcto; una escena
        // que define parámetros → los respeta. Unidades de altura/profundidad en km.
        int seed = layers.value("seed", g_current_seed);
        const float A = 0.45f; // amplitud típica del fBm normalizado

        // Helper: lee layers[name][key] o devuelve def. Tolera capa ausente.
        auto P = [&](const char* name, const char* key, float def) -> float {
            if (layers.contains(name) && layers[name].is_object())
                return layers[name].value(key, def);
            return def;
        };
        auto Pi = [&](const char* name, const char* key, int def) -> int {
            if (layers.contains(name) && layers[name].is_object())
                return layers[name].value(key, def);
            return def;
        };

        // --- Parámetros (escena → default) ---
        // continents: forma base tierra/océano.
        const float cFreq      = P("continents", "freq", 1.2f);
        const int   cOct       = Pi("continents", "octaves", 6);
        const float landHeight = P("continents", "landHeight", 1.0f); // km tierra
        const float oceanDepth = P("continents", "oceanDepth", 5.0f); // km fondo
        // mountains: cordilleras. freq alta = cordilleras estrechas y visibles.
        float       mFreq      = P("mountains", "freq", 800.0f);
        if (mFreq < 40.0f) mFreq = 800.0f; // guarda: <40 = escala continental (plano).
                                           // 800 ≈ cordilleras de ~50 km, pendiente ~27°
                                           // (visibles desde el suelo). 200 daba domos
                                           // continentales de ~9° = se percibían como llano.
        const int   mOct       = std::max(2, (int)(Pi("mountains", "octaves", 6) * s_detailScale));
        // 6 octavas (no 8): las 2 octavas ridged más finas (~270 m) creaban un campo
        // de conos afilados ("pinchos") al diferenciar la normal a 2 m. Con 6 la octava
        // más fina es ~1,2 km → cordilleras suaves sin pinchos.
        const float mHeight    = P("mountains", "strength", 6.0f);    // km pico
        const float mSharp     = P("mountains", "sharpness", 2.0f);   // exp ridge
        // belt: dónde se agrupan las cordilleras (cobertura).
        const float beltFreq   = P("belt", "freq", 2.0f);
        const float beltLo     = P("belt", "lo", -0.15f);
        const float beltHi     = P("belt", "hi",  0.20f);
        // trench: fosas oceánicas.
        const float trDepth    = P("trench", "strength", 6.0f);       // km
        // detail: relieve fino de superficie.
        const float dFreq      = P("detail", "freq", 20000.0f);
        const float dStr       = P("detail", "strength", 0.05f);
        const int   dOct       = std::max(2, (int)(Pi("detail", "octaves", 8) * s_detailScale));

        // 1. Continentes → elevación base asimétrica con plataforma continental C1.
        float cont = NoiseGenerator::fBm(pos, seed, cOct, 0.5f, 2.0f, cFreq);
        float s = cont / A;                                  // ~[-1, 1]
        float elev;
        if (s >= 0.0f) {
            elev = s * landHeight;
        } else {
            // Hermite C1: misma pendiente que la tierra en la costa, plano al fondo.
            float t  = glm::clamp(-s, 0.0f, 1.0f);
            float d  = oceanDepth * (3.0f*t*t - 2.0f*t*t*t)
                     + landHeight * (t - 2.0f*t*t + t*t*t);
            elev = -d;
        }

        float landMask  = glm::clamp(cont / 0.1f, 0.0f, 1.0f);
        float oceanMask = glm::clamp(-cont / 0.1f, 0.0f, 1.0f);

        // 2. Cinturones tectónicos.
        float belt     = NoiseGenerator::fBm(pos, seed + 55, 4, 0.5f, 2.0f, beltFreq);
        float beltMask = sstep(beltLo, beltHi, belt);

        // 3. Cordilleras (ridged) sobre tierra.
        float mn    = NoiseGenerator::fBm(pos, seed + 123, mOct, 0.5f, 2.1f, mFreq);
        float ridge = glm::clamp(1.0f - std::fabs(mn / A), 0.0f, 1.0f);
        ridge = std::pow(ridge, mSharp);
        elev += ridge * mHeight * landMask * beltMask;

        // 4. Fosas oceánicas (ridged) en cinturones.
        float tr     = NoiseGenerator::fBm(pos, seed + 99, 6, 0.5f, 2.1f, 3.0f);
        float trench = std::pow(glm::clamp(1.0f - std::fabs(tr / A), 0.0f, 1.0f), 6.0f);
        elev -= trench * trDepth * oceanMask * beltMask;

        // 5. Detalle de superficie. SOLO ruge la tierra YA emergida: el fade por
        // elevación lo lleva a 0 en la línea de costa, así el ruido fino nunca
        // empuja celdas bajo el nivel del mar → no hay "mar" disperso en la costa.
        float detail     = NoiseGenerator::fBm(pos, seed + 777, dOct, 0.5f, 2.0f, dFreq);
        float detailFade = sstep(0.0f, 0.15f, elev); // 0 en/bajo el mar, pleno sobre ~150 m
        elev += detail * dStr * landMask * detailFade;

        return elev; // km
    }

    // ---- Orquestación GPU asíncrona (hilo principal) ----
    // Saca de la config los parámetros del compute. Devuelve false si el chunk NO es
    // elegible para GPU (no gpuTerrain / no v2 / perfil != terran).
    static bool gpuParamsFromSettings(const nlohmann::json& settings, double radius,
                                      GpuHeightfield::Params& p, int& res) {
        static const nlohmann::json kE = nlohmann::json::object();
        const auto& config = settings.contains("config") ? settings["config"] : kE;
        if (config.value("genVersion", 1) < 2)        return false;
        if (!config.value("gpuTerrain", false))       return false;
        if (config.value("profile", std::string("terran")) != "terran") return false;
        p.seed   = config.value("seed", 42);
        res      = config.value("chunkSize", 24); // 24 (antes 32): ~53% de RAM por chunk
        p.reliefStrength = config.value("reliefStrength", 1.0f);
        WorldGenParams wgp = getWorldParamsCached((uint32_t)p.seed, radius);
        p.continentFreqA = wgp.continentFreqA;
        p.seaThreshold   = wgp.seaThreshold;
        p.voronoiDensity = wgp.voronoiDensity;
        p.lakeDensity    = wgp.lakeDensity;
        p.lakeMaxProb    = wgp.lakeMaxProb;
        p.radius         = radius;
        return true;
    }

    bool TerrainGenerator::gpuHasFreeSlot() {
        if (!m_gpu) m_gpu = std::make_unique<GpuHeightfield>();
        return m_gpu->hasFreeSlot();
    }

    int TerrainGenerator::gpuDispatch(const PlanetChunkKey& key, const nlohmann::json& settings, double planetRadius) {
        if (!m_gpu) m_gpu = std::make_unique<GpuHeightfield>();
        GpuHeightfield::Params p; int res;
        if (!gpuParamsFromSettings(settings, planetRadius, p, res)) return -1;
        std::vector<glm::vec3> dirs((size_t)(res + 1) * (res + 1));
        for (int y = 0; y <= res; ++y)
            for (int x = 0; x <= res; ++x)
                dirs[(size_t)(x + y * (res + 1))] = glm::vec3(getLocalPosition(key, x, y, res));
        return m_gpu->dispatchAsync(dirs, p);
    }

    bool TerrainGenerator::gpuHarvestData(int slot, std::vector<float>& outElev, std::vector<glm::vec3>& outNormal,
                                          std::vector<float>& outWater) {
        return m_gpu && m_gpu->tryHarvest(slot, outElev, outNormal, outWater);
    }
}