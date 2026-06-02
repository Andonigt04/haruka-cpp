#include "terrain_generator.h"
#include "tools/planetary_types.h"
#include "core/noise_generator.h"
#include <cstdio>


namespace Haruka {

// Global (thread-local) seed used by calculateHeight when the JSON node
// passed doesn't provide direct access to the parent config object.
namespace {
    thread_local int g_current_seed = 42;
}

    glm::vec3 TerrainGenerator::getLocalPosition(const PlanetChunkKey& key, int x, int y, int chunkSize) {
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

        return glm::vec3(spherePos);
    }

    std::shared_ptr<ChunkData> TerrainGenerator::generateChunk(const PlanetChunkKey& key, const nlohmann::json& settings, double planetRadius) 
    {
        auto chunk = std::make_shared<ChunkData>();
        static const nlohmann::json kEmptyObj = nlohmann::json::object();
        const auto& config = settings.contains("config") ? settings["config"] : kEmptyObj;
        int res = config.value("chunkSize", 32);
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
        // chunkCenter_local: planet-local position of chunk center (for vertex subtraction).
        // chunk->chunkCenter: world-space (adds planet offset), used by renderer for u_chunkOffset.
        glm::dvec3 chunkCenter_local;
        {
            glm::dvec3 centerDir = glm::dvec3(getLocalPosition(key, res / 2, res / 2, res));
            chunkCenter_local    = glm::normalize(centerDir) * planetRadius;
            chunk->chunkCenter   = chunkCenter_local + glm::dvec3(
                settings.value("planetOffsetX", 0.0),
                settings.value("planetOffsetY", 0.0),
                settings.value("planetOffsetZ", 0.0)
            );
        }

        const auto& layers = config.contains("layers") ? config["layers"] : kEmptyObj;
        float invRes = 1.0f / float(res);
        const float kmToFraction = float(1000.0 / planetRadius);

        // Posición local (relativa a chunkCenter_local) para CUALQUIER índice de
        // rejilla i,j — incluso fuera de [0,res]. Se usa para los vértices y para
        // muestrear el anillo vecino al calcular normales en los bordes (sin costuras).
        auto localPosAt = [&](int i, int j) -> glm::vec3 {
            glm::vec3 sph = getLocalPosition(key, i, j, res);
            float h = 0.0f;
            if (!isManual) h = calculateHeight(sph, layers) * kmToFraction;
            glm::dvec3 absP = glm::dvec3(sph) * (planetRadius * (1.0 + double(h)));
            return glm::vec3(absP - chunkCenter_local);
        };

        float minElevKm = 1e30f;
        for (int y = 0; y <= res; ++y) {
            for (int x = 0; x <= res; ++x) {
                glm::vec3 posOnSphere = getLocalPosition(key, x, y, res);

                float elevKm = isManual ? 0.0f : calculateHeight(posOnSphere, layers); // km
                float height = elevKm * kmToFraction;                                   // fracción del radio
                if (elevKm < minElevKm) minElevKm = elevKm;

                glm::dvec3 absPos = glm::dvec3(posOnSphere) * (planetRadius * (1.0 + double(height)));
                chunk->vertices.push_back(glm::vec3(absPos - chunkCenter_local));
                chunk->normals.push_back(posOnSphere); // placeholder; recomputado abajo
                chunk->uvs.push_back(glm::vec2(float(x) * invRes, float(y) * invRes));
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
            // Altura (fracción del radio) en una dirección esférica unitaria.
            auto heightAt = [&](const glm::vec3& dir) -> float {
                return isManual ? 0.0f : calculateHeight(dir, layers) * kmToFraction;
            };
            // Posición desplazada (planet-local, relativa a chunkCenter_local).
            auto dispAt = [&](const glm::vec3& dir) -> glm::dvec3 {
                glm::dvec3 d = glm::normalize(glm::dvec3(dir));
                return d * (planetRadius * (1.0 + double(heightAt(glm::vec3(d))))) - chunkCenter_local;
            };
            for (int y = 0; y <= res; ++y) {
                for (int x = 0; x <= res; ++x) {
                    glm::vec3 sph = getLocalPosition(key, x, y, res); // dir unitaria
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
        const float  skirtDepth = glm::clamp(vtxSpacing * 0.5f, 1.0f, 30.0f);

        const int mainCount = (res + 1) * (res + 1);

        // Helper: añade un vértice de falda (empuja hacia dentro de la esfera).
        // Todas las lecturas se copian ANTES del push_back para evitar UB por
        // revalidación de referencias cuando el vector realloca.
        auto addSkirt = [&](int mainIdx) {
            glm::vec3 v  = chunk->vertices[mainIdx];
            glm::vec3 n  = chunk->normals[mainIdx];
            glm::vec2 uv = chunk->uvs[mainIdx];
            glm::vec3 sv = v - n * skirtDepth;
            chunk->vertices.push_back(sv);
            chunk->morphTargets.push_back(sv); // skirts don't morph
            chunk->normals.push_back(n);
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

        // --- Malla de agua (cascarón a nivel del mar) ---
        // Solo si el chunk toca océano. Rejilla (res+1)² sobre la esfera al radio
        // del planeta (elevación 0), normales radiales. El oleaje se hace en el
        // shader, así que la base es lisa. Sin skirts (la esfera es suave).
        if (chunk->hasOcean) {
            const int wcount = (res + 1) * (res + 1);
            chunk->waterVertices.reserve(wcount);
            chunk->waterNormals.reserve(wcount);
            for (int y = 0; y <= res; ++y) {
                for (int x = 0; x <= res; ++x) {
                    glm::vec3 sph = getLocalPosition(key, x, y, res); // dir unitaria
                    glm::dvec3 wp = glm::dvec3(sph) * planetRadius;   // nivel del mar
                    chunk->waterVertices.push_back(glm::vec3(wp - chunkCenter_local));
                    chunk->waterNormals.push_back(sph);
                }
            }
            for (int y = 0; y < res; ++y) {
                for (int x = 0; x < res; ++x) {
                    int i = x + y * (res + 1);
                    chunk->waterIndices.push_back(i);
                    chunk->waterIndices.push_back(i + 1);
                    chunk->waterIndices.push_back(i + res + 1);
                    chunk->waterIndices.push_back(i + 1);
                    chunk->waterIndices.push_back(i + res + 2);
                    chunk->waterIndices.push_back(i + res + 1);
                }
            }
        }

        return chunk;
    }

    float TerrainGenerator::sampleHeightAt(const glm::vec3& sphereDir,
                                            const nlohmann::json& settings,
                                            double planetRadius)
    {
        static const nlohmann::json kEmpty = nlohmann::json::object();
        const auto& config = settings.contains("config") ? settings["config"] : kEmpty;
        g_current_seed = config.value("seed", 42);
        const auto& layers = config.contains("layers") ? config["layers"] : kEmpty;
        const float kmToFraction = float(1000.0 / planetRadius);
        return calculateHeight(sphereDir, layers) * kmToFraction * float(planetRadius);
    }

    // smoothstep auxiliar (Hermite) usado por el modelo de elevación.
    static inline float sstep(float e0, float e1, float x) {
        float t = glm::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float TerrainGenerator::calculateHeight(const glm::vec3& pos, const nlohmann::json& layers) {
        // Modelo de elevación REALISTA tipo Tierra. Devuelve km.
        // Rango ~[-11, +11] km. Verificado numéricamente: picos >6km ~2% (cordilleras
        // concentradas, no por todo el planeta), fosas <-8km ~0.4%, ~50% océano.
        // Las amplitudes (km) están hardcodeadas para dar escala real; la escena
        // controla freq/octaves/seed.
        int seed = layers.value("seed", g_current_seed);
        const float A = 0.45f; // amplitud típica del fBm normalizado

        float cFreq = 1.2f; int cOct = 6;
        if (layers.contains("continents")) {
            cFreq = layers["continents"].value("freq", cFreq);
            cOct  = layers["continents"].value("octaves", cOct);
        }
        // Mountain frequency must be HIGH for visible relief: at freq 4.5 a ridge
        // spans ~8900 km (wavelength = circumference/freq) → a 6 km bump over
        // 8900 km is a 0.04% slope, i.e. flat. freq 200 → ~200 km ridges (Andes/
        // Himalaya scale) with real, climbable slopes. Default raised accordingly.
        float mFreq = 200.0f; int mOct = 8;
        if (layers.contains("mountains")) {
            // Honour an explicit scene value, but reject the old tiny defaults that
            // produce flat terrain (anything < 40 is continental-scale, not mountains).
            float v = layers["mountains"].value("freq", mFreq);
            mFreq = (v < 40.0f) ? 200.0f : v;
            mOct  = layers["mountains"].value("octaves", mOct);
        }

        // 1. Continentes → elevación base asimétrica (océano profundo / tierra baja).
        float cont = NoiseGenerator::fBm(pos, seed, cOct, 0.5f, 2.0f, cFreq);
        float s = cont / A;                                  // ~[-1, 1]
        const float landHeight = 1.0f;   // km, altura continental
        const float oceanDepth = 5.0f;   // km, fondo oceánico
        float elev;
        if (s >= 0.0f) {
            elev = s * landHeight;       // tierra: sube suave
        } else {
            // Plataforma continental: transición C1 (sin quiebre de pendiente en la
            // costa). Hermite en t=-s∈[0,1] con d(0)=0, d'(0)=landHeight (igual
            // gradiente que la tierra → sin acantilado costero) y d(1)=oceanDepth,
            // d'(1)=0 (llanura abisal plana). Antes la pendiente saltaba 1→5 en la
            // costa creando un "borde de continente" abrupto.
            float t  = glm::clamp(-s, 0.0f, 1.0f);
            float d  = oceanDepth * (3.0f*t*t - 2.0f*t*t*t)
                     + landHeight * (t - 2.0f*t*t + t*t*t);
            elev = -d;
        }

        float landMask  = glm::clamp(cont / 0.1f, 0.0f, 1.0f);
        float oceanMask = glm::clamp(-cont / 0.1f, 0.0f, 1.0f);

        // 2. Cinturones tectónicos (baja frecuencia): dónde se concentran
        //    cordilleras. Umbral relajado (-0.15..0.20) → ~45% del planeta tiene
        //    relieve montañoso, no solo franjas estrechas. Antes (0.05..0.30) solo
        //    cubría 16% → la mayoría de la tierra salía lisa y no se veían montañas.
        float belt     = NoiseGenerator::fBm(pos, seed + 55, 4, 0.5f, 2.0f, 2.0f);
        float beltMask = sstep(-0.15f, 0.20f, belt);

        // 3. Cordilleras (ridged) sobre tierra. Exponente 2 (antes 4): crestas más
        //    anchas y visibles, no picos afilados aislados. +6 km de altura.
        float mn    = NoiseGenerator::fBm(pos, seed + 123, mOct, 0.5f, 2.1f, mFreq);
        float ridge = glm::clamp(1.0f - std::fabs(mn / A), 0.0f, 1.0f);
        ridge = std::pow(ridge, 2.0f);                        // crestas anchas
        elev += ridge * 6.0f * landMask * beltMask;

        // 4. Fosas oceánicas (ridged) en cinturones → hasta ~-11km.
        float tr     = NoiseGenerator::fBm(pos, seed + 99, 6, 0.5f, 2.1f, 3.0f);
        float trench = std::pow(glm::clamp(1.0f - std::fabs(tr / A), 0.0f, 1.0f), 6.0f);
        elev -= trench * 6.0f * oceanMask * beltMask;

        // 5. Detalle de alta frecuencia (relieve fino a ras de superficie).
        float dFreq = 20000.0f, dStr = 0.05f; int dOct = 10;
        if (layers.contains("detail")) {
            const auto& d = layers["detail"];
            dFreq = d.value("freq", dFreq);
            dStr  = d.value("strength", dStr);
            dOct  = d.value("octaves", dOct);
        }
        float detail = NoiseGenerator::fBm(pos, seed + 777, dOct, 0.5f, 2.0f, dFreq);
        elev += detail * dStr * glm::clamp(cont + 0.3f, 0.0f, 1.0f);

        return elev; // km
    }
}