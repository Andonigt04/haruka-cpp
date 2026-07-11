#pragma once

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "rhi/rhi_types.h"
#include "core/chunk_cache.h"
#include "tools/math_types.h"

namespace Haruka {

    class TerrainRenderer {
    public:
        struct RenderMesh {
            GLuint vao = 0;
            GLuint vbo = 0;
            GLuint mbo = 0;   // morph-target buffer (CDLOD)
            GLuint nbo = 0;
            GLuint nmbo = 0;  // morph-NORMAL buffer (CDLOD): normal del LOD padre
            GLuint uvo = 0;   // UV buffer
            GLuint ebo = 0;
            // Handles RHI de los buffers propios (legacy). El ebo es compartido (m_sharedEBO).
            Haruka::RHI::BufferHandle hVbo, hMbo, hNbo, hNmbo, hUvo;
            uint32_t indexCount  = 0;
            uint32_t vertexCount = 0;
            bool isReady = false;
            int  framesUndrawn = 0;       // frames seguidos sin dibujarse → purga GPU al pasar el margen
            std::string planetName;
            PlanetChunkKey key{};         // identity, for invalidation on terrain edit
            glm::dvec3  chunkCenter{0.0};
            int         terrainMode = 0;  // 0=Procedural, 1=Manual
            int         lod = 0;          // chunk LOD level, for morph-band calc
            double      planetRadius = 1.0;
            // Real bounding sphere computed from the vertices (relative to
            // chunkCenter). Used for accurate frustum culling — the chunk's actual
            // geometry sits at terrain elevation, NOT at the sea-level chunkCenter,
            // so a sea-level estimate wrongly culls visible chunks.
            glm::vec3   bsCenter{0.0f};   // sphere centre, relative to chunkCenter
            float       bsRadius = 0.0f;  // sphere radius (metres)
            // BATCHING (pool): si poolSlot>=0, la geometría vive en un POOL compartido
            // (un VAO por vertexCount) y se dibuja con glDrawElementsBaseVertex → sin
            // rebind de VAO por chunk (el coste dominante de terrain.draw). Si poolSlot<0,
            // usa sus buffers propios (vao/vbo…) — camino legacy, intacto.
            int32_t     poolSlot = -1;
            uint32_t    poolKey  = 0;     // = vertexCount (clave del pool)
        };

        TerrainRenderer() = default;
        ~TerrainRenderer();

        // Llamado por el StreamingSystem cuando un chunk está listo en RAM
        void addToScene(const std::string& planetName, const PlanetChunkKey& key, const ChunkData& data);

        // Llamado por el StreamingSystem cuando el LOD decide ocultar un chunk
        void removeFromScene(const std::string& planetName, const PlanetChunkKey& key);

        // Render completo (sin modelo por planeta)
        void render(const Haruka::WorldPos& cameraPos);

        // Render de un planeta específico (el caller ya subió el UBO model matrix)
        void renderPlanet(const std::string& planetName, const Haruka::WorldPos& cameraPos);

        /**
         * @brief Sets the camera-relative view-projection used for frustum culling.
         * Geometry is drawn camera-relative (projection * mat3(view)), so pass that
         * same matrix here. Chunks whose bounding sphere is fully outside the
         * frustum are skipped — large win when >200 chunks exist but only a
         * fraction are on screen. Call once per frame before renderPlanet.
         */
        void setCullMatrix(const glm::mat4& camRelViewProj) { m_cullVP = camRelViewProj; m_cullEnabled = true; }

        /** @brief Planet center (world, double) for horizon culling. Per planet, before renderPlanet. */
        void setPlanetCenter(const glm::dvec3& c) { m_planetCenter = c; m_hasPlanetCenter = true; }

        /** @brief Conjunto de HOJAS deseadas del LOD para un planeta (partición balanceada que
         *  calcula LODSystem = chunksToKeep ∪ chunksToLoad). El render dibuja, por cada hoja, el
         *  chunk residente más fino disponible (la hoja si está, si no su ancestro más cercano).
         *  Así NO se necesita la cadena entera residente: cerca se llega al fino en cuanto su
         *  hoja carga, y los intermedios sobrantes no se dibujan (sin "láminas" de overlap).
         *  Llamar cada frame antes de renderPlanet. */
        void setDesiredLeaves(const std::string& planet, std::vector<PlanetChunkKey> leaves) {
            std::lock_guard<std::mutex> lock(m_renderMutex);
            m_desiredLeaves[planet] = std::move(leaves);
            m_drawSetDirty[planet] = 1;   // invalida el cache del draw set (hojas nuevas)
            rebuildClosureForPlanet(planet); // clausura nueva → gate de m_residentVersion al día
        }

        /** @brief splitFactor REAL del LODSystem. La banda de morph CDLOD debe terminar (=1)
         *  justo en la frontera de fusión (2·nodeSize·splitFactor); si el renderer asume otro
         *  splitFactor que el LOD, los chunks se morphan (suavizan) a la distancia equivocada
         *  → parches lisos donde el terreno fino debería verse, o popping al fusionar. */
        void setSplitFactor(double sf) { if (sf > 1e-3) m_splitFactor = sf; }

        /** @brief Removes GPU meshes whose bounding sphere overlaps (center,radius)
         *  and returns their chunk keys, so the caller can drop them from the cache
         *  and let streaming regenerate them (with the new terrain edit applied). */
        std::vector<PlanetChunkKey> invalidateSphere(const glm::dvec3& center, double radius);

        /** @brief Como invalidateSphere pero SIN borrar las mallas: las marca "stale". La malla
         *  vieja se sigue dibujando hasta que el chunk regenerado llega a addToScene, que entonces
         *  la REEMPLAZA (en vez de saltarla). Evita el agujero al deformar terreno. */
        std::vector<PlanetChunkKey> markStaleSphere(const glm::dvec3& center, double radius);

        /** @brief Marks a chunk STALE without removing it: it keeps DRAWING until its
         *  replacement is ready (same key via addToScene, or its area gets covered by
         *  finer/coarser resident chunks). This is the dig "no-hole" swap generalised
         *  to all streaming/LOD reloads — the old mesh never disappears before the new
         *  one exists. No-op if the chunk isn't currently resident. */
        void markStale(const PlanetChunkKey& key);

        /** @brief True si la malla del chunk ya está subida y lista en GPU. */
        bool isResident(const PlanetChunkKey& key) const;
        /** @brief Copia (bajo UN solo lock) los hashes de los chunks residentes+listos.
         *  Para que el LOD consulte residencia miles de veces SIN re-bloquear el mutex
         *  por nodo (evita la contención que disparaba el update a ~900 ms). */
        void residentHashes(std::unordered_set<uint64_t>& out) const;

        /** @brief Hashes que el terreno DIBUJÓ para un planeta el último renderPlanet (hojas +
         *  fallbacks). El agua se sincroniza con esto (dibuja el agua de los MISMOS chunks) →
         *  nunca pinta agua de un ancestro grueso sobre tierra seca (diamantes flotantes).
         *  Sin lock: solo se toca en el hilo de render (terreno antes que agua, secuencial). */
        const std::unordered_set<uint64_t>* drawnHashesFor(const std::string& planet) const {
            auto it = m_drawnByPlanet.find(planet);
            return it != m_drawnByPlanet.end() ? &it->second : nullptr;
        }

        /** @brief Callback invoked whenever a terrain mesh is actually DELETED from the
         *  GPU (unload or stale-purge). The water renderer mirrors terrain exactly by
         *  removing its matching chunk here → water never diverges (no holes, no blobs). */
        void setOnChunkRemoved(std::function<void(const PlanetChunkKey&)> cb) { m_onRemoved = std::move(cb); }

        int getGPUMeshCount() const { return static_cast<int>(m_gpuMeshes.size()); }
        int getLastDrawnCount() const { return m_lastDrawn; }

        /** @brief Activa/desactiva el BATCHING por pool (glDrawElementsBaseVertex sin rebind de
         *  VAO por chunk). Afecta a las SUBIDAS nuevas: los chunks ya residentes siguen en su
         *  modo hasta que se recargan (muévete para que la zona se repueble). OFF = camino legacy
         *  por chunk (fallback seguro si el pool renderizara mal). */
        void setBatching(bool b) { std::lock_guard<std::mutex> lk(m_renderMutex); m_batching = b; }
        bool getBatching() const { return m_batching; }

        // --- Validación de invariantes del LOD (diagnóstico) ------------------------------
        // Recomputa el conjunto de chunks que CUBREN el planeta (la misma selección de
        // cobertura que el dibujo, pero SIN culling → independiente de hacia dónde mires) y
        // comprueba que forman una teselación correcta del quadtree. Reporta:
        //  - overlaps: un chunk dibujado tiene un ANCESTRO también dibujado (doble malla).
        //  - holes:    un área de una cara no la cubre ningún chunk (hueco).
        //  - balance:  dos chunks vecinos difieren en MÁS de 1 nivel de LOD (escalón).
        // Todo a 0 = LOD correcto este frame. Las muestras señalan UN caso de cada tipo.
        struct LODReport {
            int drawn = 0, overlaps = 0, holes = 0, balance = 0;
            int overlapsByBody[16] = {0}; // overlaps por cuerpo (body 0 = planeta principal)
            int drawnByBody[16]    = {0};
            bool hasOverlap = false, hasHole = false, hasBalance = false;
            PlanetChunkKey sampleOverlap{}, sampleHole{}, sampleBalance{};
            bool valid() const { return overlaps == 0 && holes == 0 && balance == 0; }
        };
        LODReport validateCoverage() const;

        // TEST del gating de m_residentVersion: ¿el draw-set CACHEADO (el que de verdad se dibuja,
        // actualizado solo cuando sube m_residentVersion) coincide con un buildDrawSet FRESCO? Si el
        // gate se saltara un bump necesario, el cache quedaría rancio y esto lo delataría. true si aún
        // no hay nada cacheado. Llamar en punto quiescente (sin async en vuelo).
        bool debugCachedDrawSetIsFresh(const std::string& planet) const;
        // Micro-bench NO destructivo: cronometra un buildDrawSet COMPLETO vs un applyDrawSetDelta con
        // un cambio de 1 nodo, sobre el draw-set actual. Salva y restaura cache/pending. Devuelve los
        // ns/iter en *nsFull/*nsSplice y el tamaño del draw-set en *drawn. Para cuantificar el ahorro.
        void benchDrawSetSplice(const std::string& planet, int iters,
                                double* nsFull, double* nsSplice, size_t* drawn);
        void debugResidencyStats(uint64_t& changes, uint64_t& bumps) const { changes = m_dbgResChanges; bumps = m_dbgResBumps; }

        // Selección de dibujo TOP-DOWN sin solapes: parte de la raíz de cada cara y baja a los
        // hijos SOLO si todo el subárbol hasta las hojas deseadas está residente; si falta algún
        // fino, dibuja el nodo grueso residente (coarsest fallback). Partición exacta → 0 overlaps,
        // 0 holes (con la raíz residente). La usan renderPlanet Y validateCoverage (miden lo mismo).
        void buildDrawSet(const std::string& planet,
                          std::unordered_set<uint64_t>& outHashes,
                          std::vector<PlanetChunkKey>* outKeys,
                          std::unordered_set<uint16_t>* outBodies = nullptr) const;

        // Implementación de REFERENCIA (algoritmo original, más lento) — solo para el banco de
        // pruebas: buildDrawSet (optimizado) debe producir EXACTAMENTE el mismo conjunto que este.
        void buildDrawSetReference(const std::string& planet,
                                   std::unordered_set<uint64_t>& outHashes,
                                   std::vector<PlanetChunkKey>* outKeys,
                                   std::unordered_set<uint16_t>* outBodies = nullptr) const;

        struct DrawStats { int draws = 0; int vertices = 0; int triangles = 0; };
        DrawStats getDrawStats() const;
        DrawStats getDrawStatsForPlanet(const std::string& planet) const;

        // --- Telemetría de los pools de geometría (blindaje del fix "buffer fijo") ------------
        // Expuesto para el banco de pruebas (y `terrainpool` en consola): permite ASERTAR que la
        // VRAM del terreno está ACOTADA y que growPool (el realloc gigante que causaba el stall
        // "el terreno desaparece al girar") NO dispara en el caso normal.
        struct PoolStats {
            int      poolCount = 0;    // nº de topologías distintas residentes (= nº de pools)
            size_t   totalSlots = 0;   // suma de capacidades reservadas
            size_t   usedSlots = 0;    // slots ocupados (capacity - freeSlots) sumados
            size_t   vramBytes = 0;    // VRAM reservada por TODOS los pools (5 VBOs, sin EBO compartido)
            uint32_t maxPoolCap = 0;   // capacidad del pool más grande (debe ser <= kMaxResidentChunks)
            uint64_t growCount = 0;    // veces que growPool ha disparado en toda la vida (DEBE ser 0)
        };
        PoolStats poolStats() const;

        // --- Constantes de dimensionado de los pools (públicas: tests + consola) --------------
        // Tope duro de mallas residentes → acota RAM/VRAM ante picos de streaming (fly-in/órbita).
        // Generoso sobre el set visible normal (~1500-4000); más allá se dibuja el ancestro grueso.
        static constexpr int      kMaxResidentChunks = 12000;
        static constexpr size_t   kPoolBytesPerVertex = 12 + 12 + 4 + 4 + 4; // pos+morph(vec3) + norm+morphNorm+uv(u32) = 36 B
        // Capacidad de un pool SECUNDARIO (topología no-dominante: otro planeta con distinto chunkSize,
        // islas flotantes, cuevas...). El pool DOMINANTE (el primero creado) recibe kMaxResidentChunks.
        static constexpr uint32_t kSecondaryPoolSlots = 4096;

        // Cota TEÓRICA de VRAM de pools (bytes) para asertar en tests: el pool dominante a capacidad
        // completa + (n-1) pools secundarios acotados, todos con `vc` vértices. Con vcMax = el mayor
        // vertexCount observado, es un techo válido para cualquier mezcla de topologías.
        static size_t poolVramCeil(int poolCount, uint32_t vcMax) {
            const size_t bpv = kPoolBytesPerVertex;
            size_t ceil = (size_t)kMaxResidentChunks * vcMax * bpv;                 // pool dominante
            if (poolCount > 1) ceil += (size_t)(poolCount - 1) * kSecondaryPoolSlots * vcMax * bpv;
            return ceil;
        }

    private:
        // Usamos el hash de la llave para identificar la malla en la GPU
        std::unordered_map<uint64_t, RenderMesh> m_gpuMeshes;
        // EBO COMPARTIDO por nº de índices: los índices son topología fija por res → idénticos
        // en todos los chunks de ese res. Un EBO por indexCount, reusado → ahorra VRAM (no un
        // EBO por chunk). Clave = indexCount; valor = handle GL. Se liberan en el destructor.
        std::unordered_map<uint32_t, GLuint>     m_sharedEBO;
        std::unordered_map<uint32_t, Haruka::RHI::BufferHandle> m_sharedEBOHandle;  // handles RHI paralelos

        // --- BATCHING: pool de geometría por vertexCount ---------------------------------
        // Todos los chunks del MISMO vertexCount comparten un VAO + 5 buffers (pos/morph/
        // normal/morphNormal/uv), en SLOTS de tamaño fijo (vertexCount vértices cada uno).
        // Dibujar = glDrawElementsBaseVertex(baseVertex = slot*vertexCount) → un solo VAO
        // bind para todos → mata el coste de rebind por chunk. Free-list de slots (sin
        // fragmentación: todos los slots son del mismo tamaño). CAPACIDAD FIJA reservada UNA
        // vez (ver getOrCreatePool): NO crece en runtime en el caso normal (1 topología). growPool
        // queda solo como VÁLVULA DE SEGURIDAD ruidosa (ver m_poolGrowCount) para transiciones raras.
        struct ChunkPool {
            GLuint   vao = 0;
            GLuint   posVBO = 0, morphVBO = 0, normVBO = 0, morphNormVBO = 0, uvVBO = 0;
            GLuint   ebo = 0;            // EBO compartido por indexCount (reusa m_sharedEBO)
            // Handles RHI de los 5 VBOs del pool + los buffers indirectos.
            Haruka::RHI::BufferHandle hPos, hMorph, hNorm, hMorphNorm, hUv, hCmd, hSSBO;
            uint32_t vertexCount = 0;
            uint32_t indexCount  = 0;
            uint32_t capacity    = 0;   // nº de slots
            std::vector<uint32_t> freeSlots;
            // MultiDrawIndirect: buffers de comandos + datos por-draw (se rellenan cada frame con
            // los chunks visibles de ESTE pool → una sola glMultiDrawElementsIndirect por pool).
            GLuint   cmdBuf   = 0;       // GL_DRAW_INDIRECT_BUFFER
            GLuint   drawSSBO = 0;       // DrawItem[] (binding 7), 1 por comando (gl_DrawID)
            uint32_t drawBufCap = 0;     // capacidad actual (en elementos) de cmdBuf/drawSSBO
        };
        // std430 (coincide con el SSBO de planet.vert) y comando indirecto estándar.
        struct GpuDrawItem { float offMorph[4]; int32_t mode[4]; };                 // 32 B
        struct GpuDrawCmd  { uint32_t count, instanceCount, firstIndex, baseVertex, baseInstance; }; // 20 B
        // Scratch reutilizado entre frames (por pool) → sin allocs por frame.
        std::unordered_map<uint32_t, std::vector<GpuDrawCmd>>  m_scratchCmds;
        std::unordered_map<uint32_t, std::vector<GpuDrawItem>> m_scratchItems;
        std::unordered_map<uint32_t, ChunkPool>  m_pools;   // clave = vertexCount
        bool m_batching = true;   // ON por defecto (validado por captura: pool renderiza correcto);
                                  // consola `terrainpool off` = fallback per-chunk legacy si hiciera falta

        /** @brief Devuelve (creando si hace falta) el pool para ese vertexCount/indexCount. */
        ChunkPool& getOrCreatePool(uint32_t vertexCount, uint32_t indexCount);
        /** @brief Duplica la capacidad del pool preservando los slots ya subidos (copia GPU→GPU). */
        void growPool(ChunkPool& p);
        /** @brief (Re)configura los atributos del VAO del pool tras crear/recrear sus buffers. */
        void setupPoolVAO(ChunkPool& p);

        std::unordered_set<uint64_t>             m_stale;   // chunks a REEMPLAZAR en addToScene
        std::function<void(const PlanetChunkKey&)> m_onRemoved; // espejo del agua
        mutable std::mutex m_renderMutex;

        double     m_splitFactor = 1.0; // sincronizado con el LODSystem vía setSplitFactor
        std::unordered_map<std::string, std::vector<PlanetChunkKey>> m_desiredLeaves;  // hojas LOD por planeta
        std::unordered_map<std::string, std::unordered_set<uint64_t>> m_drawnByPlanet; // hashes dibujados (para el agua)

        // CACHE del draw set (buildDrawSet es CARO: partición del quadtree con recursión + sets
        // hash → decenas de miles de ops/frame). Solo cambia si cambian las hojas deseadas
        // (setDesiredLeaves → m_drawSetDirty) o el conjunto residente (add/remove → m_residentVersion).
        // Reusado entre frames cuando nada cambió → terrain.draw baja mucho quieto/asentado.
        std::unordered_map<std::string, std::unordered_set<uint64_t>> m_drawSetCache;
        std::unordered_map<std::string, uint64_t>                     m_drawSetCacheVer; // m_residentVersion del build
        std::unordered_map<std::string, char>                         m_drawSetDirty;    // hojas cambiadas
        uint64_t m_residentVersion = 0; // ++ SOLO cuando cambia la residencia de un nodo de la CLAUSURA

        // --- DELTA-SPLICE del draw-set (evita rebuild completo al moverte+stream) -----------------
        // buildDrawSet completo es O(hojas): re-camina TODAS las hojas cada frame en que el streaming
        // entregue algún chunk. Pero un cambio de residencia de un nodo K SOLO puede alterar el
        // draw-set en el subárbol del nodo DIBUJADO que cubre K (las celdas del quadtree anidan o son
        // disjuntas → re-emitir ese subárbol y sustituir sus celdas viejas preserva EXACTO la
        // partición). Así, en frames de solo-streaming (hojas sin cambiar), re-emitimos únicamente los
        // pocos subárboles tocados en vez de todo. Correcto-por-construcción: emit(subárbol) depende
        // solo de la residencia bajo ese subárbol (demostrado igual que la clausura). Fallback a
        // rebuild completo si el delta es grande o no hay ancestro residente.
        // Conjuntos PRECOMPUTADOS por planeta (estáticos entre cambios de hojas → los usa emitSubtree
        // sin reconstruir el trie cada frame): hojas deseadas y nodos internos (ancestros de hojas).
        std::unordered_map<std::string, std::unordered_set<uint64_t>> m_leafSet;  // hojas deseadas (hash)
        std::unordered_map<std::string, std::unordered_set<uint64_t>> m_intSet;   // nodos internos (ancestros)
        // Cambios de residencia PENDIENTES de aplicar por planeta (nodos de la clausura que cambiaron
        // desde el último build). Se consumen en renderPlanet (splice) y se vacían.
        std::unordered_map<std::string, std::unordered_set<uint64_t>> m_pendingDelta;
        std::unordered_map<uint16_t, std::string> m_bodyToPlanet; // routing de noteResidencyChange (body→planeta)
        static constexpr size_t kMaxDeltaSplice = 96; // más cambios que esto en un frame → rebuild completo
        // Emite (partición top-down coarsest-fallback) el subárbol de `root` usando los sets
        // precomputados; añade los nodos DIBUJADOS a out. `memo` cachea canCover entre raíces.
        void emitSubtree(const std::unordered_set<uint64_t>& leafSet,
                         const std::unordered_set<uint64_t>& intSet,
                         const PlanetChunkKey& root,
                         std::unordered_set<uint64_t>& outHashes,
                         std::vector<PlanetChunkKey>* outKeys,
                         std::unordered_map<uint64_t, uint8_t>& memo) const;
        // Aplica los cambios pendientes por splice de subárboles. Devuelve false si hay que recurrir
        // al rebuild completo (delta grande / sin ancestro residente). CALLER tiene lock.
        bool applyDrawSetDelta(const std::string& planet);

        // CLAUSURA del draw-set = hojas deseadas ∪ todos sus ancestros ∪ raíces de cara. buildDrawSet
        // SOLO lee la residencia de nodos de la clausura (demostrado: canCover solo consulta nodos que
        // la recursión visita, y todos están en la clausura) → un cambio de residencia FUERA de la
        // clausura NO puede alterar el draw-set. Por eso m_residentVersion solo sube si el chunk que
        // cambió está en m_closureAll → evita reconstruir por evicciones lejanas/purgas/cargas no
        // deseadas al moverse. Se reconstruye cuando cambian las hojas (setDesiredLeaves, throttleado).
        std::unordered_map<std::string, std::unordered_set<uint64_t>> m_closureByPlanet;
        std::unordered_set<uint64_t> m_closureAll; // unión de todas las clausuras (test de pertenencia O(1))
        void rebuildClosureForPlanet(const std::string& planet); // clausura del planeta + m_closureAll (CALLER tiene lock)
        void rebuildClosureAll();                  // recomputa m_closureAll = ∪ m_closureByPlanet (CALLER tiene lock)
        void noteResidencyChange(uint64_t hash) {  // ++version SOLO si el nodo está en la clausura
            ++m_dbgResChanges;
            if (m_closureAll.count(hash)) {
                ++m_residentVersion; ++m_dbgResBumps;
                // Ruta el nodo cambiado a su planeta (por body) → pendiente de splice. face(3)|lod(5)|
                // x(23)|y(23)|body(10): el body está en los 10 bits altos (>>54).
                uint16_t body = (uint16_t)((hash >> 54) & 0x3FF);
                auto pit = m_bodyToPlanet.find(body);
                if (pit != m_bodyToPlanet.end()) m_pendingDelta[pit->second].insert(hash);
            }
        }
        // Diagnóstico del gating: cambios de residencia TOTALES vs los que sí invalidaron (en clausura).
        mutable uint64_t m_dbgResChanges = 0, m_dbgResBumps = 0;
        // Diagnóstico del delta-splice: frames en que se aplicó por splice vs los que cayeron al
        // rebuild completo (delta grande / sin fallback / hojas nuevas).
        mutable uint64_t m_dbgDeltaApplied = 0, m_dbgDeltaFull = 0;
    public:
        void debugDeltaStats(uint64_t& applied, uint64_t& full) const { applied = m_dbgDeltaApplied; full = m_dbgDeltaFull; }
    private:
        glm::mat4  m_cullVP{1.0f};
        bool       m_cullEnabled = false;
        glm::dvec3 m_planetCenter{0.0};
        bool       m_hasPlanetCenter = false;
        int        m_lastDrawn = 0;
        int        m_ageTick   = 0; // contador para la pasada de eviction (envejecer+purgar residentes)
        uint64_t   m_poolGrowCount = 0; // válvula de seguridad: veces que growPool ha reasignado (debe ser 0)

        void cleanupMesh(RenderMesh& mesh);

        /** @brief After adding chunk `key`, drop any STALE mesh whose area is now
         *  covered: finer descendants of `key` (merge), or the stale parent of `key`
         *  once all 4 of its children are resident (subdivide). Caller holds the lock. */
        void purgeStaleCoveredBy(const PlanetChunkKey& key);
    };

} // namespace Haruka
