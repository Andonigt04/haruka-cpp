#include "lod_system.h"
#include "chunk_cache.h" // Para usar keyToHash
#include "tools/profiler.h" // sub-scopes de lod.recompute: tree (recursión) vs balance (2:1)

namespace Haruka {

LODSystem::LODSystem(double splitFactor, int maxLOD)
    : m_splitFactor(splitFactor), m_maxLOD(maxLOD) {}

void LODSystem::setCullMatrix(const glm::mat4& m) {
    // Gribb-Hartmann: extrae los 6 planos del frustum del cam-rel VP (igual que el renderer).
    for (int i = 0; i < 4; ++i) m_cullPlanes[0][i] = m[i][3] + m[i][0]; // left
    for (int i = 0; i < 4; ++i) m_cullPlanes[1][i] = m[i][3] - m[i][0]; // right
    for (int i = 0; i < 4; ++i) m_cullPlanes[2][i] = m[i][3] + m[i][1]; // bottom
    for (int i = 0; i < 4; ++i) m_cullPlanes[3][i] = m[i][3] - m[i][1]; // top
    for (int i = 0; i < 4; ++i) m_cullPlanes[4][i] = m[i][3] + m[i][2]; // near
    for (int i = 0; i < 4; ++i) m_cullPlanes[5][i] = m[i][3] - m[i][2]; // far
    for (int p = 0; p < 6; ++p) m_cullPlanes[p] /= glm::length(glm::vec3(m_cullPlanes[p]));
    m_hasCull = true;
}

void LODSystem::forgetChunk(const PlanetChunkKey& key) {
    m_lastFrameChunks.erase(ChunkCache::keyToHash(key));
}

bool LODSystem::isVisible(const PlanetChunkKey& key) const {
    return m_currentFrameChunks.count(ChunkCache::keyToHash(key)) > 0;
}

LODUpdate LODSystem::updatePlanetLOD(const std::shared_ptr<SceneObject>& planet, const glm::dvec3& cameraPos,
                                    const ResidencyFn& isResident, uint16_t bodyId) {
    LODUpdate update;
    update.planetName = planet->name;
    m_currentBody = bodyId;  // se estampa en todas las claves de esta pasada
    m_currentFrameChunks.clear();

    double radius = planet->scale.x; // Asumimos escala uniforme para el radio

    const glm::dvec3 planetPos = planet->position;
    m_curCamPos = cameraPos; m_curPlanetPos = planetPos; m_curRadius = radius; // para balanceLeaves

    // RESIDENCIA DEL PLANETA ENTERO: procesamos las 6 caras del cubo-esfera SIN
    // descartar ninguna. El quadtree mantiene las caras lejanas gruesas (no se
    // subdividen porque están lejos), así que TODO el planeta queda representado
    // como hojas finas-cerca + gruesas-lejos, una por zona (sin solape). Qué se
    // DIBUJA lo decide por chunk el horizon+frustum cull preciso del render
    // (TerrainRenderer/WaterRenderer), y el caché LRU lo mantiene residente
    // mientras haya presupuesto de memoria. Así girar o mirar a la cara opuesta
    // NUNCA recarga terreno (antes el face-cull la descargaba y regeneraba).
    {
        HARUKA_PROFILE("tree"); // recursión top-down por las 6 caras (frustum/horizonte/split)
        for (int face = 0; face < 6; ++face) {
            const PlanetFace planetFace = static_cast<PlanetFace>(face);
            PlanetChunkKey rootKey{ planetFace, 0, 0, 0, m_currentBody };
            glm::dvec3 center = getCubeToSpherePos(planetFace, 0.5, 0.5, radius) + planetPos;

            recursiveProcess(rootKey, center, radius * 2.0, cameraPos, update, radius, planetPos, isResident);
        }
    }

    // 2:1 balance: ningún chunk vecino puede diferir en más de 1 nivel de LOD.
    // Sin esto, dos chunks adyacentes con diferencia ≥2 niveles dejan un escalón
    // recto que el morph (que solo salva 1 nivel) no puede cerrar.
    { HARUKA_PROFILE("balance"); balanceLeaves(); } // while 2:1 (findCoveringLeaf O(lod) por vecino)

    // Diff contra el frame anterior para load/keep/unload.
    for (auto& [hash, key] : m_currentFrameChunks) {
        if (m_lastFrameChunks.count(hash)) update.chunksToKeep.push_back(key);
        else                               update.chunksToLoad.push_back(key);
    }
    for (auto& [hash, key] : m_lastFrameChunks) {
        if (!m_currentFrameChunks.count(hash))
            update.chunksToUnload.push_back(key);
    }

    m_lastFrameChunks = m_currentFrameChunks;
    return update;
}

// Encuentra la hoja (en m_currentFrameChunks) que cubre la celda (face,lod,x,y),
// subiendo por los ancestros hasta la raíz. Devuelve false si ninguna existe.
bool LODSystem::findCoveringLeaf(PlanetFace face, int lod, uint32_t x, uint32_t y,
                                 PlanetChunkKey& out) const {
    for (int l = lod; l >= 0; --l) {
        uint32_t ax = x >> (lod - l);
        uint32_t ay = y >> (lod - l);
        PlanetChunkKey k{ face, (uint8_t)l, ax, ay, m_currentBody };
        auto it = m_currentFrameChunks.find(ChunkCache::keyToHash(k));
        if (it != m_currentFrameChunks.end()) { out = it->second; return true; }
    }
    return false;
}

// Sustituye una hoja por sus 4 hijos en el conjunto de hojas.
void LODSystem::subdivideLeafKey(const PlanetChunkKey& k) {
    m_currentFrameChunks.erase(ChunkCache::keyToHash(k));
    int nl = k.lod + 1;
    for (int i = 0; i < 4; ++i) {
        PlanetChunkKey c{ k.face, (uint8_t)nl, k.x * 2 + (i % 2), k.y * 2 + (i / 2), k.body };
        m_currentFrameChunks[ChunkCache::keyToHash(c)] = c;
    }
}

bool LODSystem::leafVisibleForBalance(const PlanetChunkKey& k) const {
    if (!m_screenSpace || !m_hasCull) return true; // baseline: balance global (como antes)
    const double cpa = std::pow(2.0, (double)k.lod);
    const double u = (double(k.x) + 0.5) / cpa, v = (double(k.y) + 0.5) / cpa;
    const glm::dvec3 center = getCubeToSpherePos(k.face, u, v, m_curRadius) + m_curPlanetPos;
    const glm::vec3  c = glm::vec3(center - m_curCamPos);
    const float r = (float)(m_curRadius * 2.0 / cpa); // tamaño del nodo
    for (int p = 0; p < 6; ++p) {
        const glm::vec4& pl = m_cullPlanes[p];
        if (pl.x*c.x + pl.y*c.y + pl.z*c.z + pl.w < -r) return false; // fuera del frustum
    }
    return true;
}

void LODSystem::balanceLeaves() {
    const int dx[4] = { -1, 1, 0, 0 };
    const int dy[4] = { 0, 0, -1, 1 };

    bool changed = true;
    int  guard   = 0;
    while (changed && guard++ < 128) {
        changed = false;
        // Snapshot de las hojas actuales (vamos a modificar el mapa al subdividir).
        std::vector<PlanetChunkKey> leaves;
        leaves.reserve(m_currentFrameChunks.size());
        for (auto& [h, k] : m_currentFrameChunks) leaves.push_back(k);

        for (const auto& L : leaves) {
            // Puede haber dejado de ser hoja en esta misma pasada.
            if (!m_currentFrameChunks.count(ChunkCache::keyToHash(L))) continue;

            const int n = 1 << L.lod;
            for (int d = 0; d < 4; ++d) {
                long nx = (long)L.x + dx[d];
                long ny = (long)L.y + dy[d];
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue; // cross-face: omitido

                PlanetChunkKey cover;
                if (!findCoveringLeaf(L.face, L.lod, (uint32_t)nx, (uint32_t)ny, cover)) continue;

                if ((int)L.lod - (int)cover.lod > 1) {
                    // NO cascadear hacia hojas invisibles (fuera del frustum): no se dibujan,
                    // así que su "grieta" con el vecino fino no se ve → evita la explosión de
                    // chunks en el borde del frustum (el acantilado de LOD = pico de ms).
                    if (!leafVisibleForBalance(cover)) continue;
                    subdivideLeafKey(cover); // acerca el vecino un nivel; el while repite hasta ≤1
                    changed = true;
                }
            }
        }
    }
}

// Altitude-based LOD ceiling: far from the planet (orbit / space) we DON'T subdivide to fine
// detail — the whole planet loads as a few coarse chunks (mountain-level, cheap). Near the
// surface the full m_maxLOD is allowed. `alt` is the camera altitude in planet RADII.
static int altitudeMaxLOD(double camDist, double radius, int hardMax) {
    double alt = (radius > 1e-9) ? std::max(0.0, (camDist - radius) / radius) : 0.0;
    // Tope RELATIVO a hardMax en vuelo bajo-medio (antes había topes FIJOS a 8/7 → costa a
    // bloques al volar/mirar en picado). Relativo = escala con la calidad y mantiene detalle
    // de costa desde altura, sin reventar el espacio profundo (sigue grueso, gratis).
    if (alt < 0.02) return hardMax;                    // ~superficie: detalle completo
    if (alt < 0.08) return std::max(hardMax - 2, 11);  // vuelo bajo
    if (alt < 0.20) return std::max(hardMax - 4, 10);  // vuelo medio (antes 8 fijo → bloques)
    if (alt < 0.60) return std::max(hardMax - 6, 9);   // vuelo alto
    if (alt < 2.0)  return 7;                           // órbita baja
    if (alt < 6.0)  return 5;                           // órbita
    return 4;                                            // espacio profundo: el más grueso
}

void LODSystem::recursiveProcess(const PlanetChunkKey& key, const glm::dvec3& center, double size,
                                 const glm::dvec3& cameraPos, LODUpdate& update, double radius, const glm::dvec3& planetPos, const ResidencyFn& isResident) {
    // Use the distance from camera to the SURFACE of the planet in the chunk's
    // direction rather than to the chunk center.  This allows side faces to
    // subdivide near the horizon even though their centers are far away.
    const glm::dvec3 chunkDir    = glm::normalize(center - planetPos);
    const glm::dvec3 surfacePoint = planetPos + chunkDir * radius;
    double dist = glm::distance(cameraPos, surfacePoint);
    uint64_t hash = ChunkCache::keyToHash(key);

    // Cap subdivision by camera altitude so a distant view shows the WHOLE planet cheaply.
    const double camDist = glm::distance(cameraPos, planetPos);
    const int camMaxLOD = altitudeMaxLOD(camDist, radius, m_maxLOD);

    // SUELO de LOD: el planeta ENTERO se subdivide hasta m_minLOD (todas las ~1536 piezas
    // base cargadas), PERO solo cuando estás CERCA de él (alt < ~1.5 radios). Sin este
    // gate, los planetas LEJANOS (Luna/Júpiter, simples puntos en el cielo) también se
    // forzarían a 1536 chunks cada uno → 3×1536 satura generación/memoria y no carga nada.
    const double camAlt = (radius > 1e-9) ? (camDist / radius - 1.0) : 1e9; // en radios
    const bool belowMin = (key.lod < m_minLOD) && (camAlt < 1.5);

    // RESIDENCIA DE LOS 4 HIJOS: la MISMA consulta la necesitan la HISTÉRESIS de fusión (screenSpace)
    // Y la CARGA PROGRESIVA (más abajo, ambos modos). Antes se calculaba DOS veces (hasta 8 lookups
    // hash/nodo); ahora UNA sola vez → 4. Output-idéntico (mismas claves). isResident nulo = sin gate.
    bool childAny = false;
    if (isResident) {
        const uint8_t  cl = (uint8_t)(key.lod + 1);
        const uint32_t bx = key.x * 2, by = key.y * 2;
        const uint16_t bd = key.body;
        childAny = isResident({ key.face, cl, bx,     by,     bd })
                || isResident({ key.face, cl, bx + 1, by,     bd })
                || isResident({ key.face, cl, bx,     by + 1, bd })
                || isResident({ key.face, cl, bx + 1, by + 1, bd });
    }

    bool distSplit;
    if (m_screenSpace) {
        // F1: subdivide mientras el chunk PROYECTE más de m_targetPx px en pantalla. SIN tope
        // por altitud → a distancia se vuelve grueso PORQUE proyecta poco, no por un hack.
        // F2: pero SOLO si el nodo es VISIBLE (frustum + horizonte). Si no, no refinar → acota
        // el recompute a la vista (sin esto subdivide la esfera entera = cientos de ms en CPU).
        bool visible = true;
        if (m_hasCull) {
            // Frustum (cam-rel): esfera del nodo (centro = center, radio ≈ size).
            const glm::vec3 c = glm::vec3(center - cameraPos);
            const float r = (float)size;
            // GUARD-BAND: para el REFINAMIENTO/residencia (no el dibujo) ensanchamos el frustum un
            // halo angular (~12°) → los chunks justo fuera de la vista SIGUEN refinados y residentes.
            // Sin esto, al GIRAR EN SITIO los chunks que sales de vista se evictan (12 frames) y al
            // volver a mirarlos hay que regenerarlos → "el terreno desaparece al girar". El dibujo
            // sigue estricto (terrain_renderer), así que no se dibuja nada de más: solo se mantiene vivo.
            const float guard = glm::length(c) * 0.22f; // ≈12° de halo alrededor del frustum
            for (int p = 0; p < 6; ++p) {
                const glm::vec4& pl = m_cullPlanes[p];
                if (pl.x*c.x + pl.y*c.y + pl.z*c.z + pl.w < -(r + guard)) { visible = false; break; }
            }
            // Horizonte: nodo tras la curvatura del planeta (occR = radio − margen para picos).
            if (visible) {
                const double occR = radius - 15000.0;
                if (occR > 1.0 && camDist > occR + 2000.0) {
                    const glm::dvec3 camDir = (cameraPos - planetPos) / camDist;
                    const double cosChunk   = glm::dot(camDir, chunkDir); // chunkDir = dir del nodo
                    const double cosHorizon = occR / camDist;
                    const double nodeDist   = glm::length(center - cameraPos);
                    const double horizonDist = std::sqrt(camDist*camDist - occR*occR);
                    // margen por tamaño angular del nodo (no descartar gruesos parciales)
                    const double angMargin = size / radius;
                    if (cosChunk < cosHorizon - angMargin && nodeDist > horizonDist + size)
                        visible = false;
                }
            }
        }
        const double screenSize = (dist > 1.0) ? size * m_screenK / dist : 1e30;
        // HISTÉRESIS (banda muerta) para matar el POPPING al saltar/jitear/moverte un poco cerca
        // de una frontera de LOD: si los hijos finos YA están residentes (el nodo se dibuja
        // PARTIDO), no lo fusionamos hasta que proyecte NOTABLEMENTE menos (0.72·targetPx). Sin
        // esto, un cambio mínimo de distancia (un salto) cruza el umbral una y otra vez → el chunk
        // fino se DESCARGA y se RECARGA en bucle = "aparecen cosas diferentes". Estado = residencia
        // (persiste entre frames en el renderer; el árbol de nodos se reconstruye cada frame).
        double thrPx = m_targetPx;
        if (childAny) thrPx = m_targetPx * 0.72;   // ya partido → banda muerta al fusionar
        // BIAS DE COSTA: si la línea de mar cae dentro de la HUELLA del chunk (|distToCoast| < ~size),
        // baja el umbral → se subdivide más en la orilla = triángulos finos, sin la línea de agua
        // dentada. La banda encoge con el chunk (size) → el refinamiento se concentra en la costa y
        // se detiene solo. Gate de distancia REDUCIDO 60km→20km (2026-07-05): mirando A LO LARGO
        // de la costa (vista al mar), 60 km de orilla refinada 2.5× reventaba el recompute (29ms) +
        // buildDrawSet (6.6ms) + streaming → "el terreno desaparece al mirar al mar/girar". A distancia
        // el recorte de la línea de agua es PER-PÍXEL en el shader (oceanElevKm), NO depende del LOD de
        // la malla → bajar el refinamiento lejano NO dienta la costa (sigue exacta). Órbita/coste intactos.
        // OPTIMIZACIÓN (sin cambio de salida): el sesgo de costa solo puede VOLTEAR la decisión de
        // split cuando screenSize cae en la banda (thrPx·coastRefine, thrPx]. Si screenSize > thrPx el
        // nodo se subdivide igual; si screenSize ≤ thrPx·coastRefine no se subdivide ni con el sesgo →
        // en ambos casos m_coastFn (hasta 5 fBm/nodo) es INÚTIL. Cerca del mar hay MILES de nodos
        // dentro de 20km → gatearlo a la banda mata el grueso de lod.recompute (era el pico de 13-27ms
        // al mirar a lo largo de la costa). También exige `visible` (invisible ⇒ distSplit=false igual).
        if (m_coastFn && visible && key.lod < m_maxLOD && dist < 20000.0
            && screenSize > thrPx * m_coastRefine && screenSize <= thrPx) {
            const double dc = std::abs(m_coastFn(chunkDir));   // |dist a la costa| (m)
            if (dc < size * 1.2 + 100.0) thrPx *= m_coastRefine;
        }
        distSplit = visible && (screenSize > thrPx) && (key.lod < m_maxLOD);
    } else {
        distSplit = (dist < size * m_splitFactor && key.lod < camMaxLOD);
    }
    bool wantSplit = belowMin || distSplit;

    // CARGA PROGRESIVA (grueso→fino, sin huecos negros): solo bajamos un nivel si este
    // nodo YA está residente (dibujándose) o ya tenía hijos residentes de un frame
    // previo. Así nunca pedimos chunks finos sin tener antes el grueso encima: siempre
    // hay algo dibujado mientras el detalle llega, y refina nivel a nivel. Sin predicado
    // (isResident=nullptr) → comportamiento clásico (subdivide directo al objetivo).
    if (wantSplit && isResident) {
        // childAny (residencia de los 4 hijos) ya se calculó arriba → reutilizado sin re-hashear.
        if (!isResident(key) && !childAny) {
            wantSplit = false;             // aún no está el grueso → espera
            update.residencyLimited = true; // el llamador debe recalcular hasta refinar
        }
    }

    if (wantSplit) {
        // SIN ALLOCS: en vez de crear 4 LODNode con make_unique (el árbol se tiraba entero cada
        // recompute → ~miles de alloc/free = el muro de planetary.update al moverte), calculamos
        // cada hijo (key/center/size) al vuelo y recursamos directo. El árbol vive solo en la pila.
        const int      nextLOD = key.lod + 1;
        const double   childSize = size * 0.5;
        const uint32_t baseX = key.x * 2, baseY = key.y * 2;
        const double   chunksPerAxis = std::pow(2.0, nextLOD);
        for (int i = 0; i < 4; ++i) {
            uint32_t cx = baseX + (i % 2);
            uint32_t cy = baseY + (i / 2);
            double u = (double(cx) + 0.5) / chunksPerAxis;
            double v = (double(cy) + 0.5) / chunksPerAxis;
            glm::dvec3 childCenter = getCubeToSpherePos(key.face, u, v, radius) + planetPos;
            PlanetChunkKey childKey = { key.face, (uint8_t)nextLOD, cx, cy, key.body };
            recursiveProcess(childKey, childCenter, childSize, cameraPos, update, radius, planetPos, isResident);
        }
    } else {
        // Nodo hoja: solo registrar. El load/keep/unload se calcula tras el
        // balanceo 2:1 en updatePlanetLOD (las hojas pueden subdividirse luego).
        m_currentFrameChunks[hash] = key;
    }
}

glm::dvec3 LODSystem::getCubeToSpherePos(PlanetFace face, double u, double v, double radius) {
    // 1. Convertir UV [0, 1] a Espacio Local del Cubo [-1, 1]
    double localX = (u * 2.0) - 1.0;
    double localY = (v * 2.0) - 1.0;

    // 2. Proyectar en la cara del cubo (Misma convención que en TerrainGenerator)
    glm::dvec3 p;
    switch (face) {
        case PlanetFace::FRONT: p = {  1.0,    localY, -localX }; break;
        case PlanetFace::BACK: p = { -1.0,    localY,  localX }; break;
        case PlanetFace::TOP: p = {  localX,  1.0,   -localY }; break;
        case PlanetFace::BOTTOM: p = {  localX, -1.0,    localY }; break;
        case PlanetFace::RIGHT: p = {  localX,  localY,  1.0   }; break;
        case PlanetFace::LEFT: p = { -localX,  localY, -1.0   }; break;
    }

    // 3. Esferificación (Spherify) - Mapeo matemático para evitar distorsión
    double x2 = p.x * p.x;
    double y2 = p.y * p.y;
    double z2 = p.z * p.z;

    glm::dvec3 spherePos;
    spherePos.x = p.x * std::sqrt(1.0 - (y2 * 0.5) - (z2 * 0.5) + (y2 * z2 / 3.0));
    spherePos.y = p.y * std::sqrt(1.0 - (z2 * 0.5) - (x2 * 0.5) + (z2 * x2 / 3.0));
    spherePos.z = p.z * std::sqrt(1.0 - (x2 * 0.5) - (y2 * 0.5) + (x2 * y2 / 3.0));

    // 4. Escalar al radio del planeta
    return spherePos * radius;
}

} // namespace Haruka