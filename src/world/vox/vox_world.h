#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <future>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "world/vox/cave_system.h"
#include "world/vox/island_system.h"

/**
 * @file vox_world.h
 * @brief EL CAMPO VOLUMÉTRICO DEL MUNDO: chunks uniformes, creados bajo demanda, donde vive todo lo
 *        que le ha pasado al relieve — las cuevas que trajo el generador y lo que pique el jugador.
 *
 * ⚠️ POR QUÉ CHUNKS Y NO LAS CAJAS DE LAS CUEVAS. Las cajas (`CaveBox`) miden 300-800 m dentro de
 * celdas de 10 km: NO tejan el planeta. Con el campo guardado por caja, picar en una ladera sin cueva
 * no tiene dónde escribir, y picar al borde de una caja corta el túnel contra una pared invisible —
 * la costura. Andoni pidió una sola forma de tocar el mundo y fidelidad sin fallos; las dos cosas
 * exigen que el ALMACENAMIENTO sea uniforme y bajo demanda. Así que:
 *
 *   · `CaveBox` + `planta/perfil/vol` siguen siendo la DEFINICIÓN de una cueva sembrada (lo que se
 *     hornea y lo que se puede pintar a mano).
 *   · Los CHUNKS son el MUNDO: la cueva sembrada es sólo lo que se rasteriza en los chunks que cubre,
 *     y un trazo del jugador escribe en los chunks que toca, existan o no (se crean todo roca).
 *
 * Un chunk es un cubo de `kVoxChunkM` de lado (128 m) con `kVoxN³` vóxeles (32³ → 4 m). Se indexa
 * por (cara del cubo, i, j) en la retícula angular de la cara y por `k` radial desde el radio del
 * planeta, así que dos jugadores que miren el mismo sitio hablan del MISMO chunk sin coordinarse.
 *
 * La costura entre chunks vecinos no existe por construcción: todos los vóxeles salen de la MISMA
 * definición global (cuevas + trazos), y la malla de cada chunk se extrae con un vóxel de delantal
 * muestreado del vecino. `test_vox_seam` pica una esfera a caballo de dos chunks y mide que el campo
 * sea continuo a través del plano.
 */
namespace Haruka {

    inline constexpr double kVoxChunkM = 128.0;  ///< lado del chunk (m), radial exacto
    inline constexpr int    kVoxN      = 32;     ///< vóxeles por lado
    inline constexpr float  kVoxRockM  = 24.0f;  ///< "roca lejos de todo": el mismo tope que satura el PNG

    struct VoxKey {
        int face = 0, i = 0, j = 0, k = 0;
        bool operator==(const VoxKey& o) const { return face == o.face && i == o.i && j == o.j && k == o.k; }
    };
    struct VoxKeyHash {
        size_t operator()(const VoxKey& k) const {
            return ((size_t)k.face * 73856093u) ^ ((size_t)(uint32_t)k.i * 19349663u)
                 ^ ((size_t)(uint32_t)k.j * 83492791u) ^ ((size_t)(uint32_t)k.k * 2654435761u);
        }
    };

    /** @brief Un chunk del campo. `d` > 0 roca, < 0 aire, en metros (distancia con signo aprox.).
     *
     *  ⚠️ DOS CANALES, PORQUE EL TERRENO ES LA BASE. El campo del mundo es
     *  `max(min(terreno, d), a)`: `d` es lo que se QUITA a la roca del terreno (cuevas, picar) y `a`
     *  lo que se le AÑADE (islas, levantar un parapeto). Con un solo canal y `min(terreno, d)` no
     *  hay forma de poner roca donde el terreno dice aire —construir sobre el suelo no hacía nada, y
     *  una isla flotante es exactamente eso—; con un solo canal y un sentinela "neutro" el campo
     *  deja de ser una distancia (la pared se pega a los nodos). Los dos canales son distancias con
     *  signo saturadas, se interpolan bien, y un trazo escribe en los dos (picar quita también lo
     *  añadido; rellenar tapa también lo cavado). `a` vacío = "todo aire" (−kVoxRockM): el caso de
     *  casi todos los chunks, y así no duplica memoria.
     *
     *  ⚠️ LA REJILLA ES LA CELDA (u,v,r), SIN MARCO TANGENTE. La primera versión ponía un marco
     *  `tU/tV` "determinista" en el centro y una rejilla cuadrada de `lado` metros encima: el banco
     *  midió la costura real a 79 m del centro con la rejilla acabando a 63 m — el marco era una
     *  rotación arbitraria y la celda (u,v) de la cara del cubo es un paralelogramo, no un cuadrado.
     *  Cualquier aproximación tangente deja metros de hueco o solape entre vecinos, o sea COSTURA.
     *  Ahora las coordenadas locales SON (u, v, radio): la clave y la rejilla se calculan con la
     *  misma fórmula y coinciden por construcción, hasta el último bit. El vóxel mide 4 m en el
     *  centro de una cara y ~2,8-5,7 m junto a sus aristas; se mide con `metresPerVoxel`. */
    struct VoxChunk {
        VoxKey     key;
        double     u0 = 0.0, u1 = 0.0, v0 = 0.0, v1 = 0.0;   ///< la celda en la cara del cubo
        double     baseR = 0.0;                              ///< radio (m) de la cara INFERIOR
        glm::dvec3 centerDir{0.0, 1.0, 0.0};                 ///< dirección del centro (para culls)
        glm::dvec3 origin{0.0};                              ///< centerDir·baseR: a qué van referidas las mallas
        float      sideU = 0.0f, sideV = 0.0f;               ///< metros reales de la celda en u y en v (medidos)
        /// ⚠️ LOS DOS CANALES SE RESERVAN SÓLO SI HAY ALGO QUE GUARDAR. Vacío = valor neutro (`d`:
        /// roca, `a`: aire). Antes `d` se reservaba siempre: 492 chunks cargados alrededor del
        /// jugador × 131 KB = 64 MB, casi todo roca maciza diciendo "+24" 32 768 veces. Ahora un
        /// chunk sin cueva ni trazo ni isla pesa lo que su cabecera.
        std::vector<float> d;                                ///< kVoxN³ o vacío: lo quitado (cuevas, picar)
        std::vector<float> a;                                ///< kVoxN³ o vacío: lo añadido (islas, construir)
        bool edited = false;                                 ///< tiene trazos (se guarda en la partida)
        bool dirty  = true;                                  ///< la malla no refleja `d`
        bool pendingCave = false;                            ///< una cueva que lo cruza se está horneando en hilo: `d` es provisional
        /// Las cuevas (celdas) y pozos con los que se horneó: con ellos el delantal de la malla puede
        /// evaluar el campo en un vecino NO cargado en vez de leer "roca" y coser una pared.
        std::vector<uint32_t> caveCells;
        struct Shaft { glm::dvec3 dir; double rM, topR; };
        std::vector<Shaft>    shafts;
        std::vector<uint32_t> islandCells;                   ///< islas con las que se horneó `a`

        /// Posición en el MUNDO (relativa al centro del planeta) del nodo de rejilla (x,y,z), fraccional.
        glm::dvec3 worldOfGrid(double x, double y, double z) const;
        /// Coordenadas de rejilla (fraccionales) de un punto del mundo. NO comprueba que caiga dentro.
        glm::dvec3 gridOf(const glm::dvec3& p) const;
        /// Metros por vóxel en cada eje (aprox. constantes dentro del chunk).
        glm::vec3  metresPerVoxel() const;
        /// Escritura en el canal quitado: SÓLO con `d` reservado (`reserveCut`).
        float& at(int x, int y, int z) { return d[(size_t)z * kVoxN * kVoxN + (size_t)y * kVoxN + x]; }
        /// El canal quitado; +kVoxRockM (roca) si no está reservado.
        float cutAt(int x, int y, int z) const { return d.empty() ? kVoxRockM : d[(size_t)z * kVoxN * kVoxN + (size_t)y * kVoxN + x]; }
        /// Reserva los canales a su valor neutro (roca / aire) si aún no lo están.
        void reserveCut()   { if (d.empty()) d.assign((size_t)kVoxN * kVoxN * kVoxN,  kVoxRockM); }
        void reserveAdded() { if (a.empty()) a.assign((size_t)kVoxN * kVoxN * kVoxN, -kVoxRockM); }
        /// El canal añadido; −kVoxRockM (aire) si no está reservado.
        float addedAt(int x, int y, int z) const { return a.empty() ? -kVoxRockM : a[(size_t)z * kVoxN * kVoxN + (size_t)y * kVoxN + x]; }
    };

    /** @brief Cota del terreno (m sobre el nivel del mar) en una dirección. */
    using VoxElevFn = std::function<float(const glm::dvec3&)>;

    class VoxWorld {
    public:
        /**
         * @param seed        semilla del mundo (siembra de cuevas)
         * @param planetRadiusM radio del planeta (m)
         * @param bakesDir    caché de horneados (derivable, desechable); vacío = `bakes/` del proyecto
         * @param saveDir     carpeta de la PARTIDA para los trazos (autoritativa); vacío = no persiste
         * @param elev        cota del terreno; nula = nivel del mar
         */
        void configure(uint32_t seed, double planetRadiusM, const std::string& bakesDir,
                       const std::string& saveDir, VoxElevFn elev);

        bool configured() const { return m_radius > 0.0; }
        /** @brief Hornear las cuevas en el hilo que llama (tests: resultados deterministas y sin esperas). */
        void setSynchronousBake(bool on) { m_syncBake = on; }
        /** @brief Cambia la carpeta de la partida en caliente (el juego la conoce después de que el
         *  planeta arranque): carga las cuevas colocadas de ahí y guardará ahí los trazos. */
        void setSaveDir(const std::string& dir);
        uint32_t seed() const { return m_seed; }
        const std::string& bakesDir() const { return m_bakes; }
        double   planetRadius() const { return m_radius; }

        /** @brief La clave del chunk que contiene un punto (relativo al centro del planeta). */
        VoxKey keyAt(const glm::dvec3& p) const;
        /** @brief El lado MENOR (m) de la celda de chunk en una dirección. No es 128: la retícula
         *  (u,v) de la cara del cubo se aprieta hacia las esquinas y allí las celdas miden ~50 m. */
        double chunkSideAt(const glm::dvec3& dir) const;
        /** @brief Celdas de chunk por lado de cara del cubo (los índices i, j van de 0 a esto − 1). */
        int chunksPerFace() const;
        /** @brief Dirección del centro de la celda de una clave (sin cargar nada). */
        glm::dvec3 chunkCenterDir(const VoxKey& key) const;
        /**
         * @brief Las COLUMNAS (claves con k = 0) a menos de `radiusM` del pie de `dir`, sin saltarse
         *        ninguna.
         *
         * ⚠️ NO SE ITERA A PASOS DE 100 M. El streaming del juego barría el disco a `128·0,8` m de
         * paso, dando por hecho que un chunk mide 128 m; cerca de una esquina del cubo mide ~50 m y
         * el barrido SALTABA columnas enteras: una rodaja de cueva o de isla sin chunk, y por tanto
         * sin malla — "entre las dos partes hay un hueco como si una parte estuviera desplazada".
         * Medido en el juego a 31°N 131°E: de las columnas 4352..4355 se cargaban 4352, 4354 y 4355.
         * Aquí el paso es la mitad del lado menor de la celda en ese sitio, y `test_vox_columns`
         * compara con la enumeración por índice.
         */
        std::vector<VoxKey> columnsNear(const glm::dvec3& dir, double radiusM) const;

        // ── CUEVAS E ISLAS: OBJETOS DE LA ESCENA ─────────────────────────────────────────────────
        // ⚠️ SIN SEMILLA (Andoni, 13-09): una cueva o isla colocada es una entrada del `.scene` con
        // sus mapas como assets (`CaveDef` / `IslandDef`); el juego la reproduce leyendo ficheros.
        // La siembra automática (`caveAutoProbability`, a 0) y `placeCave(dir)` producen BORRADORES
        // (forma de la semilla, mapas sin guardar): son para probar sin editor, no definen el mundo.
        /** @brief Sustituye la lista de cuevas por las de la escena (los chunks afectados se rehacen). */
        void setCaves(const std::vector<CaveDef>& defs);
        void setIslands(const std::vector<IslandDef>& defs);
        const std::vector<CaveDef>&   caveDefs()   const { return m_caveDefs; }
        const std::vector<IslandDef>& islandDefs() const { return m_islandDefs; }
        /** @brief Añade UNA definición (el editor al colocar). @return false si ya hay una con ese nombre. */
        bool placeCave(const CaveDef& def);
        bool placeIsland(const IslandDef& def);
        /** @brief Borrador en la celda de `dir` (forma de la semilla del mundo). @return false si ya existe. */
        bool placeCave(const glm::dvec3& dir);
        bool placeIsland(const glm::dvec3& dir);
        /** @brief Quita por nombre (los chunks afectados se rehornean). */
        bool removeCave(const std::string& name);
        bool removeIsland(const std::string& name);
        /** @brief Quita la que tenga el centro más cerca de `dir` (a menos de su radio). */
        bool removeCave(const glm::dvec3& dir);
        bool removeIsland(const glm::dvec3& dir);
        /** @brief La boca de cada cueva (para llevarte a ella). */
        std::vector<glm::dvec3> placedCaveMouths() const;
        /** @brief Las cajas (para cargar sus chunks enteros). */
        std::vector<CaveBox>   placedCaveBoxes() const;
        std::vector<IslandBox> placedIslandBoxes() const;

        // ── TRAZOS DEL DISEÑADOR ─────────────────────────────────────────────────────────────────
        // Dos listas por chunk, en este orden al cargar: las del MUNDO (carpeta `voxEdits` que
        // declara el planeta en la escena; las escribe el editor) y las de la PARTIDA (`saveDir`;
        // las escribe el jugador). El mismo fichero de texto por chunk en las dos.
        void setDesignerDir(const std::string& dir);
        /** @brief Con `true`, los trazos nuevos son del MUNDO (editor); con `false`, de la partida. */
        void setDesignerMode(bool on) { m_designerMode = on; }
        bool designerMode() const { return m_designerMode; }

        /** @brief Cuánto aire hay bajo la superficie en esta columna, en metros: 0 si el suelo está
         *  entero; si hay boca, la profundidad hasta el primer suelo de roca (hasta `maxM`). Es el
         *  LECHO que ve el agua: la simulación de fluidos y la lámina se apoyan aquí, no en el
         *  heightfield, y así el agua se ACUMULA en el foso en vez de flotar sobre él. */
        float floorDepthM(const glm::dvec3& dir, float maxM = 300.0f) const;

        /** @brief Recoge los horneados de cueva que corren en hilo y rehace los chunks que los
         *  esperaban. Llamar una vez por frame (lo hace `streamVox`). Devuelve cuántos llegaron. */
        int pollCaveJobs();
        /** @brief Espera a que terminen todos los horneados en hilo (tests, cierre). */
        void waitCaveJobs();
        /** @brief `true` mientras algún chunk cargado esté esperando una cueva (todo roca provisional). */
        bool cavesPending() const;

        /** @brief El chunk, cargado/creado si hace falta: hornea las cuevas que lo cruzan y reaplica
         *  los trazos guardados de la partida. Los vecinos ya cargados se marcan `dirty` (su delantal
         *  cambia). */
        VoxChunk& ensure(const VoxKey& key);
        /** @brief El chunk si está cargado; nulo si no. NO crea. */
        VoxChunk* get(const VoxKey& key);
        const VoxChunk* get(const VoxKey& key) const;

        /**
         * @brief EL CAMPO en un punto: `max(min(terreno, quitado), añadido)`.
         *
         * ⚠️ EL TERRENO ES PARTE DEL CAMPO. El chunk sólo guarda la parte VOLUMÉTRICA (`d`: cuevas y
         * trazos, roca por defecto); el heightfield entra aquí como distancia radial a su superficie
         * (`cota − r`: positivo bajo tierra, negativo en el aire). Sin esto, el campo decía "roca"
         * por encima del suelo y el pozo de una cueva salía TAPADO con un techo flotando sobre la
         * ladera; y la boca sólo rompía el suelo donde el terreno casualmente coincidía con el techo
         * plano de la caja (medido en el juego: 5 téxeles abiertos de 65 536).
         * Sin chunk cargado, sólo el terreno.
         */
        float density(const glm::dvec3& p) const;
        /** @brief Sólo el canal QUITADO (sin el terreno). Roca donde no hay chunk. */
        float caveDensity(const glm::dvec3& p) const;
        /** @brief Sólo el canal AÑADIDO. Aire donde no hay chunk (o no se reservó). */
        float addedDensity(const glm::dvec3& p) const;
        /** @brief La parte del terreno: `cota(dir) − |p|` (m). */
        float terrainDensity(const glm::dvec3& p) const;
        /** @brief Gradiente por diferencias centrales (la normal de la pared, lo que empuja). */
        glm::vec3 gradient(const glm::dvec3& p, float hM = 1.0f) const;

        /** @brief La FORMA de un trazo: una distancia con signo alrededor del centro, en metros, en
         *  el marco local (vertical = radial en el centro; tangentes deterministas). */
        struct Brush {
            enum Shape : uint8_t { Sphere = 0, Cylinder = 1, Box = 2, Capsule = 3 };
            /// QUÉ HACE dentro de la forma:
            ///   Remove/Add: quita/pone roca (unión de distancias, lo de siempre).
            ///   Flatten: allana a la cota del punto de impacto (roca debajo, aire encima).
            ///   Raise/Lower: sube/baja el suelo `amountM` metros dentro de la huella.
            ///   Smooth: suaviza los canales editados (media 3x3x3, fuerza `amountM` ∈ [0,1]).
            ///   Sculpt: ESCULPIR como en Blender: desplaza la superficie ±`amountM` a lo largo de su
            ///     normal, con una CURVA de caída (`falloff`), una HUELLA poligonal (`sides`: 0 círculo,
            ///     4 cuadrado, 6 hexágono…) y un PATRÓN opcional (`pattern`: olas, anillos, hexágonos,
            ///     damero, ruido) de paso `patternM`.
            enum Op : uint8_t { Remove = 0, Add = 1, Flatten = 2, Raise = 3, Lower = 4, Smooth = 5, Sculpt = 6 };
            enum Falloff : uint8_t { FSmooth = 0, FSphere = 1, FRoot = 2, FSharp = 3, FLinear = 4, FConstant = 5, FInvSquare = 6 };
            enum Pattern : uint8_t { PNone = 0, PWaves = 1, PRings = 2, PHex = 3, PChecker = 4, PNoise = 5 };
            Shape shape   = Sphere;
            float radiusM = 6.0f;   ///< radio (esfera, cilindro, cápsula) o medio lado tangencial (caja)
            float halfM   = 0.0f;   ///< medio alto a lo largo de la vertical (cilindro, caja)
            Op    op      = Remove;
            float amountM = 4.0f;   ///< Raise/Lower: metros · Smooth: fuerza 0..1
            float hardness = 0.5f;  ///< Flatten/Raise/Lower/Smooth: 1 = borde duro, 0 = cae desde el centro
            glm::dvec3 capsuleEnd{0.0};   ///< Capsule: el otro extremo (mundo, relativo al centro del planeta)
            Falloff falloff  = FSmooth;   ///< Sculpt: curva de caída del centro al borde
            Pattern pattern  = PNone;     ///< Sculpt: patrón sobre la huella
            float   patternM = 8.0f;      ///< Sculpt: paso del patrón (m)
            uint8_t sides    = 0;         ///< Sculpt: lados de la huella (0 = círculo)
        };
        /**
         * @brief EL ÚNICO GESTO: un trazo. Toca TODOS los chunks que la forma cruza (se crean si no
         *        existen), lo registra para la partida (o el mundo, en modo diseñador), lo apunta en
         *        el historial de deshacer y marca las mallas afectadas.
         * @return vóxeles que cambiaron de medio.
         */
        int stroke(const glm::dvec3& worldPos, const Brush& brush);
        /// Compatibilidad: `remove` manda sobre `brush.op` (Remove/Add).
        int stroke(const glm::dvec3& worldPos, const Brush& brush, bool remove) { Brush b = brush; b.op = remove ? Brush::Remove : Brush::Add; return stroke(worldPos, b); }
        /// Esfera (lo que hace el pico del jugador).
        int stroke(const glm::dvec3& worldPos, float radiusM, bool remove) { Brush b; b.shape = Brush::Sphere; b.radiusM = radiusM; b.op = remove ? Brush::Remove : Brush::Add; return stroke(worldPos, b); }

        /** @brief Deshace el último trazo de ESTA sesión (los cargados de fichero no se deshacen):
         *  se quita de todos los chunks que tocó, que se rehornean y reaplican lo que queda. El
         *  campo vuelve BIT A BIT a como estaba (`test_vox_brush`). @return false si no hay nada. */
        bool undo();
        bool redo();
        size_t undoCount() const { std::shared_lock<std::shared_mutex> lk(m_mx); return m_history.size(); }
        size_t redoCount() const { std::shared_lock<std::shared_mutex> lk(m_mx); return m_redo.size(); }

        /** @brief Rayo contra el CAMPO (no contra la malla): marcha a medio vóxel y afina por
         *  bisección. `o` relativo al centro del planeta. @return true con `outHit` en el primer
         *  punto con roca; false si `o` ya está en roca o no hay nada en `maxM`. */
        bool raycast(const glm::dvec3& o, const glm::dvec3& dir, double maxM, glm::dvec3& outHit) const;

        /** @brief Cota del terreno (m sobre el nivel del mar) en una dirección; 0 sin función. */
        float elevAt(const glm::dvec3& dir) const { return m_elev ? m_elev(glm::normalize(dir)) : 0.0f; }

        /** @brief El recorte del terreno en una dirección: 1 = no dibujes suelo (hay aire debajo). */
        float surfaceCut(const glm::dvec3& dir) const;

        /** @brief Malla de las paredes de un chunk (surface nets, con delantal del vecino). */
        void buildMesh(const VoxKey& key, CaveMesh& out, size_t maxTris = 100000);

        /** @brief Descarga los chunks a más de `keepRadiusM` de `nearDir` (los editados se guardan antes). */
        void unloadFar(const glm::dvec3& nearDir, double keepRadiusM);

        /** @brief Guarda los trazos de todos los chunks editados (llamar al guardar la partida). */
        bool saveEdits() const;

        size_t loadedCount() const { std::shared_lock<std::shared_mutex> lk(m_mx); return m_chunks.size(); }
        /// Bytes de vóxel reservados (los dos canales de todos los chunks): la MEDIDA de la memoria.
        size_t voxelBytes() const;
        /// Chunks con algún canal reservado (cueva, trazo o isla dentro).
        size_t loadedWithContent() const;
        /// Estadística del último `ensure` que horneó: cuántas cuevas cruzaban el chunk.
        int lastBakeCaves() const { return m_lastBakeCaves; }

        /** @brief MEDIDA DE COSTURA: para cada par de chunks vecinos cargados compara los valores del
         *  campo en la cara que comparten (mismos puntos del mundo). Devuelve el peor |Δ| en metros y
         *  cuántas caras se compararon. Un campo bien horneado da 0; un "desplazamiento" entre
         *  mitades da metros. */
        float seamMismatch(int* outFaces = nullptr) const;

        /// Claves de todos los chunks cargados (para el render y para medir).
        std::vector<VoxKey> loadedKeys() const;
        /// Cambia cada vez que el campo cambia (ensure con cueva, trazo, descarga): lo que mira el
        /// render para saber si su ventana de recorte sigue valiendo.
        uint64_t version() const { return m_version; }

    private:
        /// Un trazo tal como se guarda por chunk: el pincel entero, en coordenadas de REJILLA del chunk.
        struct Stroke { glm::vec3 local; float r; bool remove; bool designer = false; uint8_t shape = 0; float halfM = 0.0f; uint64_t seq = 0;
                        uint8_t op = 0; float amount = 4.0f; float hardness = 0.5f; glm::vec3 localB{0.0f};
                        uint8_t falloff = 0, pattern = 0, sides = 0; float patternM = 8.0f; };
        /// Un trazo de la sesión, tal como se dio (para rehacer y para saber qué chunks tocó).
        struct HistoryEntry { glm::dvec3 worldPos; Brush brush; bool remove; bool designer; uint64_t seq; std::vector<VoxKey> keys; };
        std::vector<HistoryEntry> m_history, m_redo;
        uint64_t m_seq = 0;
        void rebakeNoLock(VoxChunk& c, const VoxKey& key);

        // ⚠️ LO LEE LA FÍSICA DESDE SUS HILOS (los anillos de colisión se muestrean en workers) y lo
        // escribe el hilo principal (streaming, trazos). Sin esto, `unordered_map` bajo un `ensure`
        // mientras un worker hace `find` es un cuelgue esporádico imposible de reproducir. Las
        // consultas toman el cerrojo compartido UNA vez y usan los `*NoLock`; las mutaciones, el
        // exclusivo. Ningún método público se llama a sí mismo con el cerrojo tomado.
        mutable std::shared_mutex m_mx;
        const VoxChunk* findNoLock(const VoxKey& key) const;
        /// El campo HORNEADO (sin trazos) en un punto, con las cuevas y pozos de `c`.
        float bakedFieldNoLock(const VoxChunk& c, const glm::dvec3& p) const;
        /// El canal añadido HORNEADO en un punto, con las islas de `c`.
        float bakedAddedNoLock(const VoxChunk& c, const glm::dvec3& p) const;
        VoxChunk& ensureNoLock(const VoxKey& key);
        float caveDensityNoLock(const glm::dvec3& p) const;
        float addedDensityNoLock(const glm::dvec3& p) const;
        /// Trilineal de un canal de un chunk en coordenadas de rejilla (sujetadas).
        static float sampleChannel(const VoxChunk& c, const std::vector<float>& ch, const glm::dvec3& g);



        uint32_t    m_seed = 0;
        double      m_radius = 0.0;
        std::string m_bakes, m_save;
        VoxElevFn   m_elev;
        int         m_lastBakeCaves = 0;
        uint64_t    m_version = 1;
        std::unordered_map<VoxKey, VoxChunk, VoxKeyHash>          m_chunks;
        std::unordered_map<VoxKey, std::vector<Stroke>, VoxKeyHash> m_strokes;   // por chunk, en local
        std::unordered_map<uint32_t, CaveSystem>                    m_caves;     // por celda de cueva
        // ⚠️ EL HORNEADO DE UNA CUEVA VA EN HILO. Medido en el juego: 1,1 s (mapas + volumen +
        // conectividad + PNGs) la primera vez que te acercas a una — un parón. Mientras llega, el
        // chunk se hornea "todo roca" con `pendingCave`, y al recogerlo (`pollCaveJobs`) se rehace.
        std::unordered_map<uint32_t, std::future<CaveSystem>>       m_caveJobs;
        bool                                                         m_syncBake = false;   // tests: sin hilo
        std::vector<CaveDef>    m_caveDefs;                                      // las de la escena (+ borradores)
        std::vector<CaveBox>    m_placedBoxes;                                   // sus cajas (misma posición)
        std::vector<IslandDef>  m_islandDefs;
        std::vector<IslandBox>  m_placedIslandBoxes;
        std::unordered_map<uint32_t, IslandSystem>                  m_islands;   // por id de isla
        std::string             m_designerDir;
        bool                    m_designerMode = false;
        const IslandSystem* islandFor(const IslandBox& box);
        const CaveDef*   caveDefById(uint32_t id) const;
        const IslandDef* islandDefById(uint32_t id) const;
        CaveBox   boxFor(const CaveDef& d) const;
        IslandBox boxFor(const IslandDef& d) const;
        std::string strokePathIn(const std::string& dir, const VoxKey& key) const;
        void saveChunkStrokes(const VoxKey& key) const;
        /// Descarga (para rehornear) los chunks no editados a menos de `radiusM` de `dir`.
        void dropChunksNearNoLock(const glm::dvec3& dir, double radiusM);

        void frameFor(const VoxKey& key, VoxChunk& c) const;
        void bake(VoxChunk& c);
        const CaveSystem* caveFor(const CaveBox& box);
        int  applyLocal(VoxChunk& c, const Stroke& st);
        int  applyLocal(VoxChunk& c, const glm::vec3& local, float r, bool remove) { Stroke st; st.local = local; st.r = r; st.remove = remove; st.op = remove ? 0 : 1; return applyLocal(c, st); }
        static Stroke strokeFrom(const VoxChunk& c, const glm::dvec3& worldPos, const Brush& b, bool designer, uint64_t seq);
        static Stroke strokeFrom(const CaveStroke& s, bool designer);
        static CaveStroke caveStrokeFrom(const Stroke& s);
        std::string strokePath(const VoxKey& key) const;
        void markNeighboursDirty(const VoxKey& key);
    };

} // namespace Haruka
