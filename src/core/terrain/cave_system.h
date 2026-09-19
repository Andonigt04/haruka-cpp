#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * @file cave_system.h
 * @brief Cuevas: volúmenes de aire BAJO la superficie, definidos por MAPAS EN FICHEROS.
 *
 * Ver `docs/guides/PLAN_CUEVAS_ISLAS.md` §A. Aquel plan (2026-07-05) es anterior a la reescritura
 * del 2 de agosto y da por existentes cosas que ya no están —`sampleTerrainV2` es hoy un stub y la
 * altura dejó de ser una fórmula para ser un CAMPO horneado (`PlanetFields`)—, así que esto se
 * escribe de cero. Se conservan sus decisiones (heightfield = suelo; determinismo; colisión contra
 * el campo y no contra la malla; todos los huecos conectados) y se cambia UNA, por indicación del
 * usuario:
 *
 * ⚠️ **LA CUEVA LA DEFINEN MAPAS EN DISCO, NO UNA FÓRMULA.** Igual que el terreno: una función pura
 * hornea, el resultado se persiste (`bakes/`) con una clave que incluye todo lo que lo afecta, y a
 * partir de ahí **el fichero es la fuente de verdad**. Tres cosas salen gratis de hacerlo así:
 *
 *   1. **La paridad CPU↔GPU deja de ser un acto de fe.** No hay dos copias de la fórmula que puedan
 *      divergir (el riesgo que el propio plan listaba primero): las dos partes muestrean LOS MISMOS
 *      TEXELS. Es la misma razón por la que la altura pasó a ser un campo y no una fórmula.
 *   2. **La conectividad se hornea.** El tallado de túneles (§A.3) es un proceso con memoria sobre
 *      una rejilla —union-find, componentes, unir bolsas—, y eso no se re-evalúa en un punto. En el
 *      fichero ya va el aire CONECTADO, así que nadie tiene que volver a garantizarlo en runtime.
 *   3. **Se pueden EDITAR.** Un mapa es un PNG: el editor (o tú) puede pintar una galería, y el
 *      juego la lee. Una fórmula sólo se puede reprogramar.
 *
 * Los tres ficheros de un sistema de cuevas, todos en `bakes/`:
 *
 *   `cave_<clave>_planta.png`  (8 bits gris)  — M_top: la huella de los túneles vista DESDE ARRIBA.
 *   `cave_<clave>_perfil.png`  (8 bits gris)  — M_side: qué profundidades tienen hueco (eje tangente
 *                                               × profundidad). Los dos son "cómo es" la cueva, y son
 *                                               los que tiene sentido pintar a mano.
 *   `cave_<clave>_vol.png`     (8 bits gris)  — el VOLUMEN resuelto: los dos mapas combinados, la
 *                                               conectividad ya garantizada, guardado como atlas de
 *                                               rodajas. Es lo que muestrean la malla, la colisión y
 *                                               la GPU. Derivado: si falta, se rehornea de los otros.
 *
 * La clave es un hash de (versión, seed, celda, resolución, parámetros de la celda): cambiar
 * cualquier cosa que altere el resultado invalida el fichero, como en `bakeCacheKey` del planeta.
 */
namespace Haruka {

    /** @brief Marco local de un sistema de cuevas: una caja bajo un parche de superficie.
     *
     *  Coordenadas locales `(x, y, d)` en METROS: `x`,`y` tangentes (base determinista de la
     *  vertical), `d` = profundidad bajo la superficie, positiva hacia abajo. Todo lo que se hornea
     *  vive en esta caja; fuera de ella un sistema no existe (no hay efecto a distancia). */
    struct CaveBox {
        glm::dvec3 centerDir{0.0, 1.0, 0.0};  ///< dirección unitaria del centro de la celda
        glm::dvec3 tangentU{1.0, 0.0, 0.0};   ///< x local
        glm::dvec3 tangentV{0.0, 0.0, 1.0};   ///< y local
        float  radiusM   = 0.0f;   ///< medio lado tangencial de la caja (m)
        float  depthM    = 0.0f;   ///< profundidad de la caja (m)
        /// Radio del planeta + elevación en el centro (m) = techo de la caja. ⚠️ DOUBLE: a 6,37e6 m
        /// un float tiene 0,5 m de ulp, y eso es medio metro de indeterminación en dónde empieza el
        /// suelo — ya hizo que el primer vóxel de aire cayera "fuera de la caja".
        double surfaceRM = 0.0;
        /// Identidad. Para un BORRADOR sembrado, la clave de celda; para una cueva de la escena, el
        /// hash de su nombre (`caveIdFromName`). Es lo que indexa `VoxWorld::m_caves` y `caveCells`.
        uint32_t cell    = 0;
        /// Semilla del BORRADOR (la forma que hornea `caveBakeMaps`). Una cueva de la escena ya
        /// tiene sus PNG: la semilla sólo se usa si faltan.
        uint32_t seed    = 0;
        /// La boca, en fracción de la caja: (u, v) ∈ [0,1]² y radio como fracción del lado.
        /// Explícita (va en la escena); el borrador la saca de la semilla.
        glm::vec3 mouth{0.5f, 0.5f, 0.06f};

        /// Mundo (relativo al CENTRO del planeta, m) → local (m). `.z` es la profundidad.
        glm::vec3 toLocal(const glm::dvec3& p) const;
        /// Local → mundo (relativo al centro del planeta, m).
        glm::dvec3 toWorld(const glm::vec3& local) const;
        /// ¿Cae dentro de la caja?
        bool contains(const glm::dvec3& p) const;
    };

    /** @brief Un mapa horneado: gris, [0,1] por texel, con su tamaño. El formato de disco es PNG. */
    struct CaveMap {
        int w = 0, h = 0;
        std::vector<float> v;            ///< w*h, fila mayor, de arriba a abajo
        float at(float u, float t) const;  ///< bilineal, u,t ∈ [0,1] (bordes sujetados)
        bool  empty() const { return v.empty(); }
    };

    /** @brief El volumen resuelto de un sistema: el campo firmado, ya conectado.
     *
     *  `n³` vóxeles sobre la caja. `d[i]` > 0 = roca, < 0 = aire, en metros aproximados (es una
     *  distancia con signo aproximada, lo que hace que el gradiente sirva para empujar al jugador). */
    struct CaveVolume {
        int n = 0;
        std::vector<float> d;
        bool empty() const { return d.empty(); }
    };

    /** @brief Un sistema de cuevas cargado: su caja y sus mapas. */
    struct CaveSystem {
        CaveBox    box;
        CaveMap    planta;   ///< M_top
        CaveMap    perfil;   ///< M_side
        CaveVolume vol;      ///< resuelto (lo que se muestrea)
        CaveMap    boca;     ///< EL ACOPLAMIENTO con el terreno (ver `caveBakeEntrance`)
        /// Estadística del horneado, para que el banco pueda medir en vez de suponer:
        /// x = componentes de aire ANTES de tallar · y = DESPUÉS · z = vóxeles de aire.
        glm::ivec3 connectStats{0, 0, 0};
        bool valid() const { return !vol.empty(); }
    };

    /// Lado de los mapas 2D horneados (planta y perfil).
    inline constexpr int    kCaveMapRes = 256;
    /// Lado de la rejilla del volumen (n³). 96 sobre una caja de 400 m → vóxel de ~4 m.
    inline constexpr int    kCaveVolRes = 96;

    // ── LA DEFINICIÓN DE UNA CUEVA DE LA ESCENA ──────────────────────────────────────────────────
    //
    // ⚠️ SIN SEMILLA (decisión de Andoni, 13-09): una cueva colocada es una ENTRADA DEL `.scene`
    // (`"type": "Cave"`), con su centro, su caja y sus mapas como ASSETS por ruta, igual que el
    // `elevationMap` del planeta. El juego la reproduce leyendo esos ficheros; la semilla sólo
    // sirve para hornear el BORRADOR de los PNG la primera vez (y se guardan en esas rutas).
    struct CaveDef {
        std::string name;                        ///< identidad (nombre del objeto de escena)
        glm::dvec3  centerDir{0.0, 1.0, 0.0};    ///< dirección del centro de la caja
        float       radiusM = 300.0f, depthM = 150.0f;
        glm::vec3   mouth{0.5f, 0.5f, 0.06f};    ///< boca: (u, v, radio) en fracción de la caja
        std::string planta, perfil;              ///< rutas de los PNG (relativas al proyecto)
        uint32_t    draftSeed = 0;               ///< sólo si faltan los PNG
    };
    uint32_t caveIdFromName(const std::string& name);
    /** @brief La caja de una definición de escena (`surfaceRM` = radio + cota en el centro). */
    CaveBox caveBoxFromDef(const CaveDef& def, double planetRadiusM,
                           float (*surfaceElevM)(const glm::dvec3&, void*) = nullptr, void* user = nullptr);
    /** @brief Carga una cueva de la escena: mapas de sus rutas (o borrador guardado en ellas si
     *  faltan), volumen y boca derivados en `bakes/` con clave = hash(caja + TEXELS de los mapas):
     *  pintar un texel rehace el volumen; un PNG idéntico, no. */
    bool caveLoad(const CaveDef& def, CaveSystem& out, double planetRadiusM, const std::string& bakesDir = std::string(),
                  int mapRes = kCaveMapRes, int volRes = kCaveVolRes,
                  float (*surfaceElevM)(const glm::dvec3&, void*) = nullptr, void* user = nullptr);
    /** @brief Ídem con la caja ya calculada (para hornear en hilo sin tocar la cota desde él). */
    bool caveLoadBox(const CaveDef& def, const CaveBox& box, CaveSystem& out, const std::string& bakesDir = std::string(),
                     int mapRes = kCaveMapRes, int volRes = kCaveVolRes);
    /** @brief Deriva volumen y boca de los mapas YA cargados en `out` (caja + planta + perfil): lo
     *  que hace el editor tras pintar. Cache en `bakes/` por hash de caja + texels. */
    bool caveDeriveFromMaps(CaveSystem& out, const std::string& bakesDir = std::string(), int volRes = kCaveVolRes);
    /** @brief Resuelve una ruta de asset: tal cual si existe, si no bajo la raíz del proyecto. */
    std::string caveResolveAssetPath(const std::string& path);

    // ── La caja: qué celdas siembran cueva (BORRADORES) ──────────────────────────────────────────

    /// Lado angular de la celda de siembra (rad). 0,0016 rad ≈ 10 km en la Tierra.
    inline constexpr double kCaveCellRad = 0.0016;
    /// Fracción de celdas que siembran cueva AUTOMÁTICAMENTE. ⚠️ Es un ajuste de ejecución y su
    /// defecto es CERO: Andoni quiere las cuevas puestas A MANO de momento, no sembradas como el
    /// terreno. `caveSetAutoProbability(0.14f)` devuelve la siembra (una cada ~7 celdas), y los
    /// tests la ponen ellos mismos. Con 0, `caveBoxAt`/`caveBoxesNear` sólo devuelven cuevas
    /// colocadas con `caveBoxForCell`.
    void  caveSetAutoProbability(float p);
    float caveAutoProbability();

    /** @brief La caja de la celda que contiene `dir`, EXISTA O NO cueva sembrada: es cómo se pone
     *  una cueva a mano (la forma sigue saliendo de (seed, celda), determinista). */
    void caveBoxForCell(uint32_t seed, const glm::dvec3& dir, double planetRadiusM, CaveBox& out,
                        float (*surfaceElevM)(const glm::dvec3&, void*) = nullptr, void* user = nullptr);

    /** @brief ¿Siembra cueva la celda que contiene `dir`? Si sí, devuelve su caja.
     *  @param surfaceElevM cota del terreno en una dirección (m sobre el nivel del mar); nulo = 0. */
    bool caveBoxAt(uint32_t seed, const glm::dvec3& dir, double planetRadiusM, CaveBox& out,
                   float (*surfaceElevM)(const glm::dvec3&, void*) = nullptr, void* user = nullptr);

    /** @brief Dirección (unitaria) de la BOCA de una cueva sembrada: donde el pozo rompe el suelo. */
    glm::dvec3 caveEntranceDir(const CaveBox& box);
    /** @brief Radio (m) del pozo de entrada de esa cueva. */
    float caveEntranceRadiusM(const CaveBox& box);

    /** @brief Las cajas cuyo centro cae a menos de `searchRadiusM` de `nearDir`. Determinista. */
    int caveBoxesNear(uint32_t seed, const glm::dvec3& nearDir, double searchRadiusM,
                      double planetRadiusM, CaveBox* out, int maxOut,
                      float (*surfaceElevM)(const glm::dvec3&, void*) = nullptr, void* user = nullptr);

    // ── El horneado (funciones puras) ────────────────────────────────────────────────────────────

    /** @brief Hornea los dos mapas que DEFINEN la cueva. Pura: misma caja → mismos texels. */
    void caveBakeMaps(const CaveBox& box, int res, CaveMap& planta, CaveMap& perfil);

    /** @brief Combina los dos mapas en el volumen y GARANTIZA que el aire sea una sola pieza.
     *
     *  Rasteriza `aire = planta·perfil > τ`, busca componentes conexas (26 vecinos), se queda con la
     *  mayor, talla un túnel de la mayor a cada bolsa que merezca la pena y rellena de roca las que
     *  no. Deja en `outStats` {componentes antes, después, vóxeles de aire}: ese número es la MEDIDA
     *  de si el AND de dos mapas necesitaba post-proceso o no.
     *
     *  @param minVoxels bolsas menores que esto se descartan (huecos-punto, inalcanzables). */
    void caveBakeVolume(const CaveBox& box, const CaveMap& planta, const CaveMap& perfil,
                        int n, CaveVolume& out, glm::ivec3* outStats = nullptr, int minVoxels = 12);

    // ── El cache en disco (mismo patrón que `loadOrBake` del planeta) ────────────────────────────

    /** @brief Clave del sistema: hash de (versión, seed, celda, caja, resoluciones). */
    std::string caveCacheKey(const CaveBox& box, int mapRes, int volRes);

    /** @brief Carga el sistema de `bakes/` o, si no está (o la clave cambió), lo hornea y lo guarda.
     *  @param dir carpeta de horneados; vacío = `bakes/` del proyecto. */
    bool caveLoadOrBake(const CaveBox& box, CaveSystem& out, const std::string& dir = std::string(),
                        int mapRes = kCaveMapRes, int volRes = kCaveVolRes);

    // ── Lo que consumen malla, colisión y render ─────────────────────────────────────────────────

    /** @brief El campo firmado en un punto: >0 roca, <0 aire (m aprox.). Trilineal sobre el volumen.
     *  @param p posición relativa al CENTRO del planeta (m). Fuera de la caja: roca. */
    float caveDensityAt(const CaveSystem& sys, const glm::dvec3& p);
    /** @brief Ídem, DRAPEADA sobre el terreno: la profundidad se mide desde `surfaceR` (radio del
     *  planeta + cota del terreno en la dirección de `p`), no desde el techo plano de la caja.
     *
     *  ⚠️ Con el techo plano, donde el terreno baja por debajo de la cota del centro de la caja las
     *  galerías quedaban POR ENCIMA del suelo: el terreno se recortaba (aire bajo la superficie) y
     *  la pared no se dibujaba (roca sobre el suelo no es de la cueva) — un boquete sin nada dentro
     *  ("el terreno en el nodo vecino y la cueva no aparecen"). Drapeada, la cueva sigue al relieve
     *  como una capa: la misma galería está siempre a la misma profundidad bajo el suelo. */
    float caveDensityAtDraped(const CaveSystem& sys, const glm::dvec3& p, double surfaceR);

    /** @brief Gradiente de `caveDensityAt` (diferencias centrales): la normal de la pared. */
    glm::vec3 caveDensityGradient(const CaveSystem& sys, const glm::dvec3& p, float hM = 1.0f);

    /** @brief Malla de las paredes de una cueva (posiciones RELATIVAS al centro del planeta, m). */
    struct CaveMesh {
        std::vector<glm::vec3>    positions;   ///< relativas a `origin` (m), para no perder precisión
        std::vector<glm::vec3>    normals;
        std::vector<unsigned int> indices;
        glm::dvec3 origin{0.0};                ///< a qué punto del mundo van referidas (m)
        size_t triangleCount() const { return indices.size() / 3; }
        bool   empty() const { return indices.empty(); }
    };

    /**
     * @brief Extrae la superficie `caveDensity = 0` como malla (surface nets / dual contouring).
     *
     * ⚠️ NO son marching cubes, y es a propósito: MC necesita una tabla de 256 casos —cientos de
     * líneas que no se pueden auditar leyéndolas— y da triángulos finísimos en las esquinas. El dual
     * pone UN vértice por celda, en la media de los cortes del campo con sus aristas, y cose un quad
     * por cada arista que cambia de signo. Sale menos malla, más regular, y el vértice lo coloca el
     * CAMPO (que es el que manda, ver `cave_system.h`), no una tabla.
     *
     * @param maxTris cota dura: por encima de esto devuelve lo que lleve (una malla desbocada es un
     *                cuelgue, no un fallo visible).
     */
    void caveBuildMesh(const CaveSystem& sys, CaveMesh& out, size_t maxTris = 200000);

    /**
     * @brief Surface nets sobre una rejilla `n³` cualquiera. Es lo que usan `caveBuildMesh` (una
     *        cueva entera, para el banco) y `VoxWorld::buildMesh` (un chunk con delantal): UNA
     *        implementación, o las dos mallas divergirían en la costura.
     * @param d          n³ valores, índice `z·n·n + y·n + x`; > 0 roca, < 0 aire
     * @param gridLocal  posición local (m) del nodo de rejilla (x,y,z) — puede ser anisótropa
     * @param own        opcional, n³, tres bits por nodo: bit 0 = el aire aquí es de la cueva ·
     *                   bit 1 = el nodo está bajo el terreno (o es roca añadida) · bit 2 = la roca
     *                   la pone el canal AÑADIDO (isla, construido). Un quad se cose si su extremo
     *                   de ROCA tiene el bit 1 y además su extremo de AIRE tiene el bit 0 o el de
     *                   roca el bit 2. Es lo que deja fuera el suelo (que dibuja el pase de nodos)
     *                   y las paredes que asomarían por una ladera.
     */
    void harukaSurfaceNets(int n, const float* d,
                           const std::function<glm::vec3(float, float, float)>& gridLocal,
                           CaveMesh& out, size_t maxTris, const uint8_t* own = nullptr);

    // ── TRAZOS: la ÚNICA forma de modificar el mundo ─────────────────────────────────────────────
    //
    // ⚠️ NO HAY "deformar terreno" POR UN LADO Y "cavar" POR OTRO. Quitar roca (picar, una galería)
    // y ponerla (levantar un parapeto, tapar un hueco) son el MISMO gesto con el signo cambiado, y
    // caen sobre el MISMO campo que ya usan las cuevas horneadas. Una cueva del generador no es otra
    // cosa que estos mismos trazos, puestos de antemano porque son deterministas.
    //
    // Lo que sí es distinto es DÓNDE VIVE cada cosa: el horneado se deriva de la seed y es caché
    // desechable (`bakes/`); un trazo del jugador no se puede recalcular y es estado de la PARTIDA.
    // Por eso los trazos se guardan aparte y se aplican ENCIMA al cargar — y por eso se guardan como
    // trazos (20 bytes, mandables por red) y no como el volumen entero (~900 KB por sistema).

    /** @brief Una modificación puntual del campo. */
    struct CaveStroke {
        glm::vec3 centerLocal{0.0f};  ///< centro, en coordenadas LOCALES de la caja (m)
        float     radiusM = 0.0f;
        bool      remove  = true;     ///< true = quita roca (abre) · false = la pone (rellena)
        /// FORMA (v2 del fichero): 0 esfera · 1 cilindro radial (eje = la vertical local) · 2 caja
        /// alineada al tangente. `halfM` = medio alto a lo largo de la vertical (cilindro y caja).
        uint8_t   shape   = 0;
        float     halfM   = 0.0f;
        /// v3: la OPERACIÓN (0 quitar · 1 poner · 2 allanar · 3 subir · 4 bajar · 5 suavizar), su
        /// cantidad (m, o fuerza 0..1), la dureza del borde y el segundo extremo (cápsula).
        uint8_t   op      = 0;
        float     amountM = 4.0f;
        float     hardness = 0.5f;
        glm::vec3 endLocal{0.0f};
        /// v4: esculpir (curva de caída, patrón, paso del patrón, lados de la huella).
        uint8_t   falloff = 0, pattern = 0, sides = 0;
        float     patternM = 8.0f;
    };

    /** @brief Aplica un trazo al volumen ya cargado. Devuelve cuántos vóxeles cambiaron de medio. */
    int caveApplyStroke(CaveSystem& sys, const CaveStroke& stroke);

    /** @brief Ídem, con el centro en coordenadas de MUNDO (relativas al centro del planeta).
     *  @return -1 si el punto no cae en la caja de este sistema. */
    int caveApplyStrokeWorld(CaveSystem& sys, const glm::dvec3& worldPos, float radiusM, bool remove);

    /** @brief Guarda / carga la lista de trazos de una partida (texto, una línea por trazo). */
    bool caveSaveStrokes(const std::string& path, const std::vector<CaveStroke>& strokes);
    bool caveLoadStrokes(const std::string& path, std::vector<CaveStroke>& out);

    /**
     * @brief Hornea el MAPA DE BOCA: dónde el aire de la cueva llega a la superficie.
     *
     * ⚠️ ES EL ÚNICO PUNTO DE CONTACTO ENTRE CUEVA Y TERRENO, y por eso es un mapa y no una consulta
     * al volumen: el shader del terreno tiene que decidir por PÍXEL si hay suelo, y no se le puede
     * pedir que recorra un atlas de 96 rodajas para eso. Aquí se resuelve una vez, en el horneado:
     * un mapa 2D en el plano tangente de la caja, 1 = aquí la cueva rompe el suelo (el terreno se
     * recorta) · 0 = aquí hay suelo. El borde es SUAVE a propósito: el recorte por píxel con un
     * escalón duro deja un filo dentado de téxel, y la del terreno es una malla teselada que no
     * puede seguir ese filo.
     *
     * `metrosDeTecho` = cuánta roca bajo la superficie tiene que faltar para llamarlo boca.
     */
    void caveBakeEntrance(const CaveBox& box, const CaveVolume& vol, int res, CaveMap& out,
                          float metrosDeTecho = 6.0f);

    /** @brief El recorte del terreno en una dirección: 1 = no dibujes suelo aquí (hay boca). */
    float caveSurfaceCut(const CaveSystem& sys, const glm::dvec3& dir);

    /** @brief Cuánto aire toca la superficie en esta dirección: 1 = la cueva ROMPE el suelo aquí.
     *
     *  Único acoplamiento cueva→terreno (§A.4): el shader del terreno recorta lo que esto marca y el
     *  labio de la malla tapa el borde. 0 donde no hay sistema. */
    float caveEntranceMask(const CaveSystem& sys, const glm::dvec3& dir);

} // namespace Haruka
