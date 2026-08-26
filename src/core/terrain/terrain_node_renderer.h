/**
 * @file terrain_node_renderer.h
 * @brief El pase de terreno por NODOS, entero (v5, F3). Opt-in con `HARUKA_TERRAIN_V5=1`.
 *
 * Junta las cinco piezas que ya están probadas por separado —selector con horizonte y frustum, pool
 * con LRU, generador en GPU, cosido entre niveles y el shader que lee el pool— y las corre por frame.
 *
 * ── POR QUÉ VIVE AQUÍ Y NO EN `planet.cpp` ──────────────────────────────────────────────────────
 *
 * `planet.cpp` es el fichero con más cicatrices del repo: tres superficies coplanares peleándose por
 * el z-buffer, un sesgo de profundidad que decide quién gana, y una vista de depuración que existe
 * solo para saber cuál pintó un píxel. Meter un cuarto camino ahí dentro sería repetir la historia.
 *
 * Encapsulado, el diff en `planet.cpp` es un miembro y dos líneas, y se puede BORRAR de un tirón si
 * no funciona. Esa reversibilidad es la que permite probarlo en el juego sin apostar nada.
 *
 * ⚠️ Y por eso mismo es EXCLUYENTE, no aditivo: cuando está activo, la malla base y el clipmap NO se
 * dibujan. Dos superficies coplanares más serían exactamente el bug que el v5 viene a quitar.
 */
#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/planet/terrain_lod.h"   // TERRAIN_RING_FINE_CELL: la celda de la malla que se PISA
#include "terrain_node.h"
#include "terrain_node_pool.h"
#include "terrain_node_gpu.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_resources.h"
#include "core/logger.h"

namespace Haruka { namespace Terrain {

class TerrainNodeRenderer {
public:
    /** @brief ¿Está activado el camino v5? Se lee UNA vez (getenv por frame es búsqueda lineal). */
    /// ⚠️ ACTIVO POR DEFECTO desde el 2026-08-24: el pase de nodos ES el terreno. `HARUKA_TERRAIN_V5=0`
    /// lo apaga, pero ya no hay clipmap detrás — sin él no se dibuja suelo.
    static bool enabled() {
        static const bool s_on = [] {
            const char* e = std::getenv("HARUKA_TERRAIN_V5");
            return !(e && e[0] == '0');
        }();
        return s_on;
    }

    /**
     * @brief Presupuesto de error en píxeles, ajustable sin recompilar (`HARUKA_TERRAIN_V5_ERRPX`).
     *
     * ⚠️ EXISTE PORQUE EL VALOR CORRECTO NO SE PUEDE DEDUCIR, HAY QUE MEDIRLO, y cada medida cuesta
     * reconstruir el juego. Con 1 px el selector pidió **1 011 nodos** y el pase se fue a 53 ms
     * (contra 26 del clipmap): 1 011 draws de 32 768 triángulos son 33 M de triángulos por frame.
     * El error por téxel y el coste no son la misma curva, y sin poder barrer el parámetro la
     * pregunta "¿es el diseño o es la constante?" no tiene respuesta.
     */
    static double errorPx() {
        static const double s_px = []() {
            const char* v = std::getenv("HARUKA_TERRAIN_V5_ERRPX");
            return v ? std::atof(v) : (double)TERRAIN_NODE_ERROR_PX;
        }();
        return s_px;
    }

    /**
     * @brief Píxeles que se quieren entre VÉRTICES al dibujar (`HARUKA_TERRAIN_V5_VERTPX`).
     *
     * ⚠️ ES OTRA COSA QUE `errorPx`, Y CONFUNDIRLAS ERA EL BUG. `errorPx` decide cuánto DETALLE
     * necesita el heightmap (a qué nivel se subdivide); esto decide cuántos TRIÁNGULOS se dibujan de
     * él. Estaban acoplados —un vértice por téxel— así que con un téxel de 1 px salía **un vértice
     * por píxel**: 1 011 nodos × 32 768 triángulos = 33 M por frame, y el pase a 53 ms contra los 26
     * del clipmap.
     *
     * El clipmap dibuja quads de 4,1 px, y por eso 4 es el valor que hace la comparación justa.
     */
    /** @brief Vista de depuración del pase (`HARUKA_TERRAIN_V5_DEBUG`). Ver `terrain_node.frag`. */
    /// Fuerza la vista de depuración desde código (>=0). `debugView()` la lee de una variable de
    /// entorno UNA vez, así que un test no puede cambiarla: esto es la puerta para hacerlo.
    void setDebugViewOverride(int v) { m_debugOverride = v; }

    static int debugView() {
        static const int s_v = []() {
            const char* v = std::getenv("HARUKA_TERRAIN_V5_DEBUG");
            return v ? std::atoi(v) : 0;
        }();
        return s_v;
    }

    /**
     * @brief INTERRUPTORES DE BISECCION (`HARUKA_TERRAIN_V5_*`). Existen para responder "¿desde
     *        cuando?" en una ejecucion en vez de en una tarde de hipotesis.
     *
     * ⚠️ Se anadieron el 2026-08-25 porque los pinchos reportados sobrevivieron a CUATRO causas
     * medidas y descartadas (caida profunda a ancestro, hambre del selector, rango vacio, el
     * `SwapWindow` del generador). Reconstruir los 3,07 M de vertices en CPU da **+0,0000 m** de
     * exceso sobre el campo, asi que la aritmetica no es. Lo que queda es aislar POR SUBSISTEMA.
     *
     *   · `STRIDE=n`  fija el stride de TODOS los nodos a n (1,2,4…), como antes de que fuera por
     *                 nodo. Si los pinchos se van, son del stride por nodo o de su cosido.
     *   · `NOMORPH=1` apaga el geomorph entero (ni por distancia ni por arista). Si se van, es el
     *                 morph. ⚠️ Sin morph REAPARECEN los escalones entre niveles (2,4 m medidos):
     *                 es un diagnostico, no un modo de juego.
     */
    static uint32_t forcedStride() {
        static const uint32_t s_v = []() -> uint32_t {
            const char* v = std::getenv("HARUKA_TERRAIN_V5_STRIDE");
            if (!v) return 0;
            const long n = std::strtol(v, nullptr, 10);
            return (n >= 1 && n <= 64) ? (uint32_t)n : 0u;
        }();
        return s_v;
    }
    static bool noMorph() {
        static const bool s_v = []() {
            const char* v = std::getenv("HARUKA_TERRAIN_V5_NOMORPH");
            return v && v[0] == '1';
        }();
        return s_v;
    }

    static double vertexPx() {
        static const double s_px = []() {
            const char* v = std::getenv("HARUKA_TERRAIN_V5_VERTPX");
            return v ? std::atof(v) : 4.0;
        }();
        return s_px;
    }

    /**
     * @param capacity huecos del pool. ⚠️ **El selector solo usa el 75 %**: el resto es la cache de
     *        lo que acaba de salir de cuadro. Con 1024 el juego medía `sel 1008 / residentes
     *        1024/1024` — sin margen, y el nivel más fino se quedaba en 14 en vez de 17.
     *        `HARUKA_TERRAIN_V5_POOL` lo cambia sin recompilar: cada hueco son 66,6 KB de VRAM.
     */
    /**
     * ⚠️ **EL 1024 DE ANTES SE QUEDO CORTO Y NO ERA OBVIO.** Venia de cuando el pool publicaba rangos
     * vacios y el criterio media la distancia al NIVEL DEL MAR, o sea subdividiendo de menos. Con el
     * rango de verdad la demanda REAL —ya recortada por el cono del frustum— es de **1 013 nodos a
     * pie**, contra un presupuesto de 768 (el 75 % de 1024). Saturado permanentemente, y de un
     * selector hambriento salen las dos cosas que se reportaron juntas: vecinos con mas de un nivel
     * de diferencia (pinchos) y reparto que cambia con la camara (parpadeo). El log lo decia:
     * `tope del selector 768 SATURADO · por ancestro 180` de 767.
     *
     * Medido en `terrain_node_demand_with_range`:
     *
     *     altura    demanda con cono    pool necesario (demanda / 0,75)
     *        2 m         1 013                1 350
     *      200 m           955                1 273
     *    2 000 m           528                  704
     *
     * 2048 deja el presupuesto en 1 536, un 50 % por encima del pico medido — margen para girar (que
     * mete nodos nuevos de golpe) y para mirar a una ladera, que pide mas. Son 399 MB a 195 KB/nodo (tres mapas: propio, padre y abuelo).
     */
    bool init(RHI::Device* dev, const std::string& shaderDir, size_t capacity = 2048) {
        if (const char* e = std::getenv("HARUKA_TERRAIN_V5_POOL")) {
            const long v = std::strtol(e, nullptr, 10);
            if (v >= 64 && v <= 65536) capacity = (size_t)v;
        }
        if (m_ready || !dev) return m_ready;
        m_dev = dev;
        // El presupuesto de generación escala con el pool: con más huecos que llenar, el mismo
        // número por frame tardaría proporcionalmente más en converger.
        m_pool = TerrainNodePool(capacity, std::max<size_t>(43, capacity / 24));
        const std::string comp = shaderDir + "terrain_node.comp";
        if (!m_gpu.init(dev, comp.c_str(), capacity)) {
            HARUKA_LOGW("TerrenoV5", "fallo el generador (compute '%s' o el SSBO de %.1f MB)",
                        comp.c_str(), (double)(capacity * TerrainNodeGpu::kBytesPerNode) / 1048576.0);
            return false;
        }

        RHI::PipelineDesc pd;
        const std::string vs = shaderDir + "terrain_node.vert";
        const std::string fs = shaderDir + "terrain_node.frag";
        pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
        pd.vertexLayout.strides = { (uint32_t)(2 * sizeof(float)) };
        pd.vertexLayout.attributes.push_back({ 0, 0, RHI::Format::RG32F, 0 });
        pd.depth.test = true; pd.depth.write = true;
        // ⚠️ SIGUE EN `None`, Y NO POR DESCUIDO — ESTÁ PROBADO Y REVERTIDO (2026-08-25).
        //
        // Con `None` cada nodo rasteriza SUS DOS CARAS: en el limbo y a distancia rasante la de
        // delante y la de detrás del mismo relieve compiten por el depth (parpadeo) y se pagan el
        // doble de fragmentos sobre 9,8 M de triángulos. O sea que activarlo interesa.
        //
        // El bobinado NO es el problema: `terrain_node_winding` lo verifica CCW-desde-fuera en las
        // seis caras, 612/612, con contraprueba. Aun así, con `CullMode::Back` medido en el banco:
        //     OpenGL  cobertura en pantalla 97,4 %   (correcto)
        //     Vulkan  cobertura en pantalla  3,4 %   (se descarta la cara BUENA)
        // `vk_pipeline.cpp` fija `VK_FRONT_FACE_COUNTER_CLOCKWISE`, y otros pipelines con
        // `cull = Back` (props, malla base, `nearground.vert`) sí se ven bien en Vulkan — así que la
        // causa es específica de este pase y no un fallo general del RHI. Sin entender ESO, activarlo
        // deja el planeta casi vacío en un backend. Es la primera pregunta del hilo `vulkan-is-opengl-assumed`:
        // ¿qué está asumiendo de GL este pase que los otros no?
        m_pipe = dev->createPipeline(pd);
        if (!RHI::valid(m_pipe)) {
            HARUKA_LOGW("TerrenoV5", "fallo el pipeline de dibujo ('%s' + '%s')", vs.c_str(), fs.c_str());
            return false;
        }

        // LA REJILLA ES ÚNICA para todos los nodos: solo enteros (u,v). Lo que distingue a un nodo de
        // otro es su UBO, así que miles de nodos residentes comparten un vertex y un index buffer.
        const uint32_t N = TERRAIN_NODE_TEXELS;
        std::vector<float> verts; verts.reserve((size_t)N * N * 2);
        for (uint32_t v = 0; v < N; ++v)
            for (uint32_t u = 0; u < N; ++u) { verts.push_back((float)u); verts.push_back((float)v); }
        // ── UN ÍNDICE POR STRIDE, todos sobre el MISMO vertex buffer ────────────────────────────
        //
        // El stride `s` dibuja uno de cada `s` téxeles: 129², 65², 33²… hasta 3². Como los vértices
        // gruesos son un SUBCONJUNTO exacto de los finos (la propiedad de F1), cambiar de stride no
        // mueve ningún vértice — solo deja de dibujar los de en medio. Por eso basta con cambiar de
        // índice: la geometría sigue describiendo la misma superficie.
        //
        // Se precalculan los 7 y se elige por frame. Un vertex buffer, siete index buffers, y el
        // coste de dibujo baja con el cuadrado del stride.
        std::vector<uint32_t> idx;
        for (uint32_t k = 0; k < kStrides; ++k) {
            const uint32_t st = 1u << k;
            if (TERRAIN_NODE_CELLS % st != 0u) break;
            idx.clear();
            for (uint32_t v = 0; v + st < N; v += st)
                for (uint32_t u = 0; u + st < N; u += st) {
                    const uint32_t a = v * N + u, b = a + st, c = a + st * N, d = c + st;
                    // Bobinado (a,b,c): con `p = (1, ly, -lx)` en la cara FRONT, `∂lx × ∂ly` apunta
                    // hacia FUERA, así que recorrer +u antes que +v da CCW visto desde el exterior —
                    // lo que espera el convenio del motor. El orden anterior era (a,c,b), o sea el
                    // inverso. Hoy no descartaba nada porque el pipeline va con `CullMode::None`,
                    // pero dejarlo mal impide activar el culling y esconde el error hasta entonces.
                    idx.insert(idx.end(), { a, b, c, c, b, d });
                }
            m_indexCount[k] = (uint32_t)idx.size();
            m_ibs[k] = dev->createBuffer(RHI::BufferUsage::Index, idx.size() * sizeof(uint32_t),
                                         idx.data(), RHI::BufferMemory::Static);
            if (!RHI::valid(m_ibs[k])) return false;
            ++m_strideCount;
        }
        m_vb = dev->createBuffer(RHI::BufferUsage::Vertex, verts.size() * sizeof(float),
                                 verts.data(), RHI::BufferMemory::Static);
        DrawUBO du{};
        m_ubo = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(du), &du, RHI::BufferMemory::Dynamic);
        m_ready = RHI::valid(m_vb) && m_strideCount > 0 && RHI::valid(m_ubo);
        // ⚠️ LA VRAM DEL POOL NO ES UN DETALLE: con el geomorph cada hueco guarda DOS mapas, así que
        // son 195 KB por nodo (propio + padre + abuelo). 4096 huecos = 798 MB, encima de los
        // 720 MB del array de terreno — a partir de 2048 hay que mirar la VRAM de verdad.
        if (m_ready)
            HARUKA_LOGI("TerrenoV5", "pool de %zu huecos = %.0f MB de VRAM (%.1f KB/nodo: propio "
                                     "+ padre + abuelo) · generacion %zu nodos/frame",
                        capacity, (double)m_gpu.bytes() / (1024.0 * 1024.0),
                        (double)TerrainNodeGpu::kBytesPerNode / 1024.0,
                        std::max<size_t>(43, capacity / 24));
        return m_ready;
    }

    void shutdown() {
        if (!m_dev) return;
        if (RHI::valid(m_ubo)) { m_dev->destroy(m_ubo); m_ubo = {}; }
        for (uint32_t k = 0; k < kStrides; ++k) {
            if (RHI::valid(m_ibs[k]))      { m_dev->destroy(m_ibs[k]);      m_ibs[k] = {}; }
            if (RHI::valid(m_instSSBO[k])) { m_dev->destroy(m_instSSBO[k]); m_instSSBO[k] = {}; m_instCap[k] = 0; }
        }
        m_strideCount = 0;
        if (RHI::valid(m_vb))  { m_dev->destroy(m_vb);  m_vb  = {}; }
        if (RHI::valid(m_pipe)){ m_dev->destroy(m_pipe);m_pipe= {}; }
        m_gpu.shutdown();
        m_dev = nullptr; m_ready = false;
    }

    struct FrameStats {
        size_t drawn = 0, generated = 0, resident = 0, ancestors = 0, tris = 0;
        size_t covered = 0;   ///< instancias quitadas por tener un ANCESTRO dibujado encima
        uint32_t coveredDrop = 0;  ///< y a cuantos NIVELES estaba el ancestro mas lejano
        size_t evicted = 0;   ///< desalojos ACUMULADOS del pool (si sube, hay thrash)
        size_t pubFailed = 0; ///< publicaciones rechazadas por no haber hueco desalojable
        uint32_t stride = 1;      ///< el MAS GRUESO del frame (los nodos ya no comparten stride)
        uint32_t strideMin = 1;   ///< el mas fino: es el que hay bajo tus pies
        uint32_t drawCalls = 0;   ///< uno por grupo de stride con nodos, <= 7
        /// ⚠️ EL TOPE REAL, no la capacidad. El log imprimia `capacity()` (4096) mientras el selector
        /// corria con `capacity - capacity/4` (3072), asi que "sel 3053 · tope 4096" parecia holgado
        /// cuando estaba a 19 del limite. Saturar significa dibujar nodos BASTOS, y quien se queda
        /// basto lo decide el orden de recorrido, no la cercania (`terrain_node_budget_starvation`).
        size_t   selBudget = 0;
        /// ⚠️ Nodos dibujados SIN rango publicado por el pool. No es contabilidad: sin rango,
        /// `nodeStrideIndex` cree que la superficie esta en R y mide la distancia al NIVEL DEL MAR.
        /// A 1026 m de cota eso convierte 2 m en 1028 y el stride se queda en 4 donde tocaba 1 —
        /// el sintoma es `stride 4..4` en vez de `1..4`. Ver `terrain_node_stride_per_node`.
        size_t   rangeMissing = 0;
        // Diagnóstico: sin esto, "falta terreno" no se puede separar de "no se selecciona",
        // "no cabe en el pool" o "se recorta". Cada causa se arregla en un sitio distinto.
        size_t   selected = 0, culledHorizon = 0, culledFrustum = 0, noSlot = 0;
        uint32_t levelMin = 99, levelMax = 0;
        double   farthestKm = 0.0;
    };

    // ── POR QUÉ ESTO SON DOS FUNCIONES Y NO UNA ─────────────────────────────────────────────────
    //
    // ⚠️ EN VULKAN, DESPACHAR COMPUTE DENTRO DE UN RENDER PASS NO DIBUJA NADA. No es lento: es
    // ILEGAL. `generatePending` emite un `vkCmdPipelineBarrier`, y una barrera dentro de un subpass
    // sin self-dependency INVALIDA el command buffer entero — la capa de validación lo dice literal
    // ("Barriers cannot be set during subpass 0 ... with no self-dependency"). Todo lo que venga
    // detrás, incluido el dibujo del terreno, se descarta en silencio.
    //
    // Esto era una sola `render()` que hacía las dos cosas, llamada desde dentro del pase. En GL
    // funcionaba —GL no tiene render passes— y en Vulkan el terreno no salía. Es EL MISMO patrón que
    // los otros ocho bugs de "Vulkan asumido como OpenGL": la API tolerante enseña una costumbre que
    // la estricta prohíbe.
    //
    // Partirlo obliga al orden correcto en el sitio de llamada, que es la única forma de que no
    // vuelva a pasar. `render()` ya no existe a propósito: no se puede escribir el orden malo.

    /**
     * @brief Fase 1 — seleccionar, pedir al pool y GENERAR en compute. **FUERA del render pass.**
     *
     * @param viewDir    eje del cono de visión, para el recorte de frustum.
     */
    /// El bake del planeta, que el pase necesita para que el nodo tenga continentes. Ver
    /// `TerrainNodeGpu::setBaseField`. Lo usan el compute (altura) y el vértice (clima).
    /// @brief Muestreador de altura para el rango de los nodos. Ver `TerrainNodeGpu::setHeightSampler`.
    void setHeightSampler(NodeBaseHeightFn fn, void* ctx) { m_gpu.setHeightSampler(fn, ctx); }

    void setBaseField(RHI::TextureHandle t) { m_gpu.setBaseField(t); m_baseField = t; }

    /// El bake EQUIRECT de altura: la MISMA fuente de elevación que el clipmap y la física.
    void setHeightTex(RHI::TextureHandle t) { m_gpu.setHeightTex(t); }

    /**
     * @brief Lo que hace falta para sombrear como el clipmap: mismas texturas, mismo UBO de
     *        materiales, misma ancla de patrón. Sin esto el pase pinta la luz plana de geometría.
     *
     * ⚠️ Son EXACTAMENTE los recursos que ata `biome.frag`, y a propósito: el color del nodo sale de
     * `lib/terrain_shade.glsl`, la misma función. Si divergieran, la transición nodo↔clipmap se
     * vería como una costura.
     */
    struct Shade {
        RHI::TextureHandle albedo{}, normal{}, macro{}, biome{}, zone{};
        RHI::BufferHandle  materialUBO{};
        float     tiling     = 1.0f;    ///< metros por tile
        int       shoreLayer = -1;      ///< capa de arena de orilla (−1 = no hay)
        glm::vec3 texAnchor{0.0f};      ///< ancla planetaria, reducida módulo el tile en doubles
        glm::vec3 lightDir{0.0f, 1.0f, 0.0f};
        bool      on = false;           ///< false = luz plana (el lado A del A/B de coste)
    };
    void setShade(const Shade& sh) { m_shade = sh; }

    /// Las cifras del último frame dibujado (nodos, triángulos, niveles…).
    const FrameStats& stats() const { return m_stats; }

    /// Huecos del pool. Si `selected` se acerca a esto, el selector deja de afinar (no abre agujeros).
    size_t capacity() const { return m_gpu.capacity(); }

    FrameStats prepare(RHI::Context* ctx, const glm::dvec3& camPos, const glm::dvec3& planetCenter,
                       double planetRadiusM, const glm::dvec3& viewDir,
                       double radPerPx, double coneHalfAngle) {
        FrameStats fs;
        m_stats = fs;
        if (!m_ready || !ctx) return fs;

        // ── LAS RAÍCES SE FIJAN, Y SIN ESTO HAY AGUJEROS ────────────────────────────────────────
        //
        // ⚠️ SÍNTOMA REPORTADO: "dentro del clipmap correcto, fuera no hay terreno". La causa no era
        // el selector ni el recorte — era esto. El fallback por ancestro solo puede devolver algo si
        // hay un ancestro RESIDENTE, y con el pool saturado (1 024 huecos, y el campo cercano pide
        // ~1 010) los nodos lejanos no encontraban ninguno: `slot = -1`, no se dibujan, agujero.
        //
        // Las seis raíces se generan una vez y se marcan `pin`, así que el LRU no puede tocarlas.
        // Cuestan seis huecos de 1 024 y garantizan que SIEMPRE haya algo que dibujar en cualquier
        // dirección — la propiedad que el pool documenta y que el renderer no estaba cumpliendo.
        if (!m_rootsPinned) {
            m_pool.beginFrame();
            for (int f = 0; f < 6; ++f) m_pool.request(NodeId{ (PlanetFace)f, 0, 0, 0 });
            if (m_gpu.generatePending(ctx, m_pool, planetRadiusM) > 0) {
                for (int f = 0; f < 6; ++f) m_pool.pin(NodeId{ (PlanetFace)f, 0, 0, 0 });
                m_rootsPinned = true;
            }
        }

        m_pool.beginFrame();
        // ⚠️ EL SELECTOR NO PUEDE PEDIR TODO EL POOL: SI LO HACE, LA CACHE DEJA DE SER UNA CACHE.
        //
        // Medido en el juego a 1030 m: `sel 1008 · tope 1024 · residentes 1024/1024`. El selector iba
        // al 98 % de su tope y el pool al 100 %, así que NO quedaba un solo hueco para lo que se
        // acababa de dejar de ver — el LRU desaloja y regenera en bucle, y como el tope frena la
        // subdivisión, el terreno se queda en el nivel 14 (4,77 m/téxel) en vez de bajar al 17 (0,6).
        //
        // El pool es una CACHE: tiene que ser mayor que el conjunto visible, no igual. Se reserva un
        // 25 % para lo que sale de cuadro y va a volver (medido: al andar 48 m sobreviven el 89,3 %
        // de los nodos, así que ese margen se aprovecha de verdad).
        const size_t selBudget = m_gpu.capacity() - m_gpu.capacity() / 4;
        nodeSelectVisible(planetRadiusM, camPos, planetCenter, radPerPx, m_sel,
                          selBudget, errorPx(), &viewDir, coneHalfAngle,
                          5000.0, &TerrainNodePool::rangeFnAdapter, &m_pool,
                          &fs.culledHorizon, &fs.culledFrustum);
        fs.selected  = m_sel.size();
        fs.selBudget = selBudget;
        // El desglose lo cuenta el propio selector (ver `nodeSelectVisible`): aqui se hacia
        // re-ejecutandolo dos veces con los MISMOS argumentos y restando, o sea siempre 0 para el
        // horizonte, y costaba dos pasadas por frame.
        for (const NodeId& n : m_sel) {
            fs.levelMin = std::min(fs.levelMin, n.level);
            fs.levelMax = std::max(fs.levelMax, n.level);
            const glm::dvec3 p = planetCenter + nodeTexelDir(n, TERRAIN_NODE_CELLS/2, TERRAIN_NODE_CELLS/2) * planetRadiusM;
            fs.farthestKm = std::max(fs.farthestKm, glm::length(p - camPos) / 1000.0);
        }

        // Resolver ANTES de generar: así el frame dibuja lo que hay (con fallback por ancestro) y la
        // generación de lo que falta se paga en paralelo, no bloqueando el dibujo.
        m_resolved.clear(); m_resolved.reserve(m_sel.size());
        for (const NodeId& n : m_sel) m_resolved.push_back(m_pool.request(n));
        // ⚠️ ORDENAR ANTES DE GENERAR. El presupuesto es el mismo; lo que cambia es CUÁLES entran.
        m_pool.prioritisePending(camPos, planetCenter, planetRadiusM);
        fs.generated = m_gpu.generatePending(ctx, m_pool, planetRadiusM);

        // ── VOLVER A RESOLVER CON LO QUE SE ACABA DE GENERAR ────────────────────────────────────
        //
        // Esto era el "aparece un segundo terreno encima al girar la cámara". La resolución de arriba
        // es ANTERIOR a la generación, así que una hoja que entra nueva por el borde del frustum se
        // resuelve contra un pool que todavía no la tiene, cae a un ancestro muy grueso, y **el
        // ancestro se dibuja ENTERO**: cubre a sus cuatro hijos, no solo al que falta, y a 7 niveles
        // eso es una sábana sobre toda la vista. Un frame después ya está generada y desaparece.
        // Medido con la cámara girando y el pool CALIENTE (`terrain_node_overlap_on_turn`):
        //
        //     giro f1  tapados  59  caida 4 niveles      giro f4  tapados 274  caida 7 niveles
        //     giro f2  tapados  18  caida 2 niveles      giro f5  tapados  35  caida 2 niveles
        //
        // El destello de un frame, cada dos frames, mientras giras. Y NO era falta de recursos: 0
        // desalojos, 364 residentes de 2048, y 8-20 fallos por frame contra un presupuesto de 170.
        // Lo que faltaba era usar lo que ya se había generado ANTES de dibujar.
        //
        //     girando, como estaba    499 emitidos · 457 tapados (91,6 %)
        //     girando, re-resolviendo 252 emitidos ·   0 tapados     <- igual que estando quieto
        //
        // De regalo, casi la mitad de instancias: los ancestros gordos ya no se emiten.
        if (fs.generated > 0) {
            m_resolved.clear();
            for (const NodeId& n : m_sel) m_resolved.push_back(m_pool.request(n));
        }

        // ── Y AUN ASI, EL CONJUNTO DIBUJADO TIENE QUE SER UNA PARTICION ─────────────────────────
        //
        // Re-resolver cierra el hueco cuando el presupuesto da de sí; si no da (descenso rápido desde
        // órbita, arranque en frío), el fallback vuelve a emitir ancestros que tapan a sus hijos. Esto
        // deja solo los MAXIMALES: sin duplicados y sin nadie dibujado debajo de otro. No abre
        // agujeros —lo que se quita estaba tapado por algo que sigue— y no empeora el LOD: medido, el
        // peor error en pantalla es el MISMO (31,9 px), porque el ancestro ya se estaba dibujando.
        //
        // ⚠️ Y SOLO SE PAGA CUANDO PUEDE HABER ALGO QUE QUITAR. El selector emite HOJAS de un quadtree,
        // que por construcción no son ancestros unas de otras: sin ninguna caída por ancestro no puede
        // haber un solo nodo tapado. En régimen eso es el caso (medido en el juego: `por ancestro 0` en
        // doce ventanas seguidas), así que la máscara no corre y el coste es una comparación.
        {
            bool anyAncestor = false;
            for (const auto& r : m_resolved) if (r.slot >= 0 && !r.exact) { anyAncestor = true; break; }
            m_drawn.clear();
            if (anyAncestor) {
                m_drawn.reserve(m_resolved.size());
                for (const auto& r : m_resolved) if (r.slot >= 0) m_drawn.push_back(r.node);
            }
            uint32_t covDrop = 0;
            const size_t nCov = anyAncestor ? nodeCoveredMask(m_drawn, m_drop, &covDrop) : 0;
            if (nCov > 0) {
                size_t w = 0, k = 0;
                for (size_t i = 0; i < m_resolved.size(); ++i) {
                    if (m_resolved[i].slot < 0) { m_resolved[w++] = m_resolved[i]; continue; }
                    if (!m_drop[k++]) m_resolved[w++] = m_resolved[i];
                }
                m_resolved.resize(w);
            }
            fs.covered = nCov; fs.coveredDrop = covDrop;
        }

        // ── EL CONJUNTO DIBUJADO NO CUMPLE 2:1, Y EQUILIBRARLO AQUI NO ES LA SOLUCION ───────────
        //
        // El SELECTOR sale 2:1 (medido: 0 saltos de mas de un nivel a cualquier presupuesto). Lo que
        // se DIBUJA no: un nodo sin hueco cae a un ancestro y aparecen saltos de hasta 4 niveles —
        // 191 parejas de 11 304 con solo 23 nodos caidos. El cosido coloca bien la arista pero el
        // geomorph apunta al PADRE, y a 4 niveles el padre no es el vecino: quedan **1,372 m** de
        // escalon (0,339 m ya a dos niveles). Es una causa real de pinchos.
        //
        // ⚠️ SE PROBO A EQUILIBRARLO AQUI, BAJANDO DE NIVEL AL FINO, Y ES PEOR. Bajar un nodo lo
        // convierte en ancestro de otros que siguen dibujandose, asi que hay que quitar tambien sus
        // descendientes — y esa onda se propaga: medido, **2 902 nodos se quedan en 277**. Veintitres
        // nodos caidos arrastran al 90 % del terreno. `terrain_node_level_balance` guarda la medida
        // para que nadie lo vuelva a intentar por este lado.
        //
        // La direccion correcta es que no haya caidas profundas: que el pool garantice el PADRE de lo
        // que se selecciona. Eso es politica del pool, no del renderer, y esta sin hacer.
        m_drawn.clear(); m_drawn.reserve(m_resolved.size());
        for (const auto& r : m_resolved) if (r.slot >= 0) m_drawn.push_back(r.node);
        const auto index = nodeDrawnIndex(m_drawn);

        // ── STRIDE POR NODO ─────────────────────────────────────────────────────────────────────
        //
        // Era global, con el argumento de que el selector ya iguala el tamaño en pantalla de todos
        // los nodos. Cierto para la pantalla; falso para la disparidad ver↔pisar, que solo importa
        // donde pisas. Ver `nodeStrideIndex` para las cifras.
        //
        // ⚠️ CON HISTERESIS, Y POR ESO HACE FALTA MEMORIA. Sin ella el stride es funcion pura de la
        // distancia y un nodo parado en un umbral parpadea CADA FRAME con el micro-temblor del
        // personaje (medido: 119 de 120 frames), moviendo la superficie ~1 cm cada vez. Eso era el
        // "el terreno tiembla al moverse".
        //
        // ⚠️ Y el mapa se consulta TAMBIEN para el vecino al coser (ver `draw`). Recalcular alli el
        // stride del vecino daria el valor SIN historia, que puede no ser el que el vecino esta
        // usando de verdad — y coser contra una zancada que el otro lado no tiene es una grieta.
        m_skOf.clear(); m_skOf.reserve(m_resolved.size());
        m_skNow.clear(); m_skNow.reserve(m_resolved.size() * 2);
        for (const auto& r : m_resolved) {
            if (r.slot < 0) { m_skOf.push_back(0); continue; }
            double elevM = 0.0;
            { const NodeRange rg = m_pool.rangeOf(r.node); if (rg.valid()) elevM = rg.maxM; }
            if (elevM == 0.0) ++fs.rangeMissing;
            const uint64_t key = nodeKey(r.node);
            const auto it = m_skPrev.find(key);
            const uint32_t prev = (it == m_skPrev.end()) ? kNoPrevStride : it->second;
            const double want = nodeStrideWant(r.node, planetRadiusM, camPos, planetCenter,
                                               errorPx(), vertexPx(),
                                               Planet::TERRAIN_RING_FINE_CELL, elevM);
            uint32_t sk = nodeStrideQuantise(want, m_strideCount - 1, prev);
            if (const uint32_t f = forcedStride()) {          // biseccion: stride global, como antes
                sk = 0; while ((1u << sk) < f && sk + 1 < m_strideCount) ++sk;
            }
            m_skOf.push_back(sk);
            m_skNow[key] = sk;
        }
        m_skPrev.swap(m_skNow);   // lo de este frame pasa a ser la historia del siguiente
        fs.stride    = m_skOf.empty() ? 1u : (1u << *std::max_element(m_skOf.begin(), m_skOf.end()));
        fs.strideMin = m_skOf.empty() ? 1u : (1u << *std::min_element(m_skOf.begin(), m_skOf.end()));
        m_lastRadPerPx = radPerPx;
        m_index = index;
        m_stats = fs;
        return fs;
    }

    /**
     * @brief Fase 2 — dibujar lo que `prepare` dejó resuelto. **DENTRO del render pass.**
     *
     * @param mvp  proyección·vista, con la vista SIN traslación (todo va relativo al ojo).
     */
    FrameStats draw(RHI::Context* ctx, const glm::dvec3& camPos, const glm::dvec3& planetCenter,
                    double planetRadiusM, const glm::mat4& mvp) {
        FrameStats fs = m_stats;
        if (!m_ready || !ctx) return fs;
        const auto& index = m_index;

        // ── UN SOLO DRAW INSTANCIADO. No es una optimización: es lo único correcto en Vulkan. ──
        //
        // ⚠️ Antes esto era un bucle de un draw por nodo con `m_dev->updateBuffer(m_ubo, ...)` entre
        // medias. En Vulkan `updateBuffer` sobre memoria mapeada es un `memcpy` inmediato, así que
        // los draws GRABADOS leen todos el ÚLTIMO valor: se dibuja N veces el mismo nodo. El test de
        // cobertura lo midió — OpenGL 100 %, Vulkan 0,2 % (un parche) con las MISMAS cifras de CPU:
        // 365 seleccionados, 365 dibujados, 747 520 triángulos emitidos y la pantalla negra.
        //
        // Los datos por nodo se suben de una vez a un SSBO y el shader los indexa por instancia.
        // ⚠️ AGRUPADO POR STRIDE. Cada stride tiene su propio index buffer, así que un draw
        // instanciado solo puede cubrir nodos que compartan stride. Se ordenan por grupo y se emite
        // un draw por grupo — como mucho `m_strideCount` (7), no uno por nodo.
        m_inst.clear(); m_inst.reserve(m_resolved.size());
        m_group.assign(m_strideCount + 1, 0);
        for (uint32_t pass = 0; pass < m_strideCount; ++pass) {
        for (size_t ri = 0; ri < m_resolved.size(); ++ri) {
            const auto& r = m_resolved[ri];
            const uint32_t skOwn = (ri < m_skOf.size()) ? m_skOf[ri] : 0u;
            if (skOwn != pass) continue;
            if (r.slot < 0) { if (pass == 0) ++fs.noSlot; continue; }
            if (!r.exact) ++fs.ancestors;
            NodeInstGPU g{};
            g.node[0] = (int32_t)r.node.face; g.node[1] = (int32_t)r.node.level;
            g.node[2] = (int32_t)r.node.i;    g.node[3] = (int32_t)r.node.j;
            // ── COSIDO CON STRIDES DISTINTOS ────────────────────────────────────────────────────
            //
            // La zancada del cosido ya no la fija sola la diferencia de nivel: un vecino del MISMO
            // nivel pero con el doble de stride tiene la mitad de vértices en la arista, y eso abre
            // exactamente la misma grieta que un nivel de diferencia. La arista compartida tiene que
            // usar la MÁS GRUESA de las dos densidades, y el lado fino es el que cede.
            NodeId nb[4];
            nodeNeighbourLevels(r.node, index, g.edge, nb);
            const uint32_t sOwn = 1u << skOwn;
            int32_t fineMask = 0;
            for (int e = 0; e < 4; ++e) {
                // El stride que el vecino esta usando DE VERDAD este frame, historia incluida. Si no
                // esta en el mapa es que no se dibuja, y `nodeNeighbourLevels` ya devolvio `r.node`
                // como vecino: entonces vale el propio y no hay nada que coser.
                const auto itNb = m_skPrev.find(nodeKey(nb[e]));
                const uint32_t sNb = 1u << ((itNb == m_skPrev.end()) ? skOwn : itNb->second);
                // ⚠️ LA DIFERENCIA DE NIVEL VA CON SIGNO, Y AQUI ESTABA EL PINCHO.
                //
                // `nodeNeighbourLevels` recorta `outCoarser` a 0 cuando el vecino es MAS FINO —
                // correcto para el cosido por nivel, porque en ese caso cose el otro—. Pero la
                // zancada del stride necesita la diferencia REAL: un vecino un nivel mas fino tiene
                // texeles la mitad de grandes, asi que su stride en MIS unidades es `sNb/2`, no
                // `sNb`. Usando el 0 recortado se sobreestima la zancada y se cose una arista que no
                // habia que coser: yo mismo abro la T-junction que el cosido venia a cerrar.
                //
                // Y con stride por nodo el caso NO es raro, es el comun: el vecino mas fino esta mas
                // cerca, su `collWant` dobla y le toca `sNb = 2·sOwn` — que en mis unidades es
                // exactamente mi propia malla, o sea nada que coser.
                //
                // Con stride GLOBAL esto era invisible: `sNb == sOwn` siempre, y `sOwn << 0` da
                // `sOwn`, que no supera a `sOwn` y no cose. Por eso los pinchos aparecieron con el
                // stride por nodo y desaparecen con `HARUKA_TERRAIN_V5_STRIDE=4`.
                const int lvDiff = (int)r.node.level - (int)nb[e].level;
                const uint32_t sNbMine = (lvDiff >= 0) ? (sNb << (uint32_t)lvDiff)
                                                       : (sNb >> (uint32_t)(-lvDiff));
                const uint32_t step = std::max(sOwn, std::max(sNbMine, 1u));
                // ⚠️ DOS COSAS DISTINTAS QUE ANTES ERAN LA MISMA. La zancada del cosido y el
                // geomorph hacia el padre se leían los dos de `edge`, porque mientras `edge` era
                // "niveles más grueso" las dos preguntas tenían la misma respuesta. Con stride por
                // nodo ya no: un vecino del MISMO nivel y otro stride necesita cosido pero NO
                // morph —evalúa la misma escalera de octavas, no la del padre—, y morfear ahí
                // levanta mi arista hacia el padre mientras la suya se queda. Eso es una costura,
                // y la introduje yo al cambiar el significado de `edge`.
                // (la mascara de morph por arista se borro: no cerraba nada. Ver terrain_node.vert)
                // ⚠️ Y LA MASCARA ESPEJO: aristas donde el vecino es MAS FINO.
                //
                // El vecino fino morfea hacia la altura CRUDA de su padre —o sea la MIA—, pero yo
                // estoy a mi vez morfeando hacia el mio, asi que los dos lados apuntan a superficies
                // distintas. Medido en `terrain_node_edge_audit_all`: de los 2,75 m de escalon entre
                // niveles, **2,25 m son esto** (quitandome el morph aqui baja a 0,495 m).
                //
                // No se puede arreglar por el otro lado: el fino necesitaria `mix(padre, abuelo, ...)`
                // y su hueco solo guarda dos mapas. Asi que lo cede el GRUESO: junto a esa arista deja
                // de morfear y vuelve a su propia altura, que es justo a la que el fino apunta.
                // (la mascara de vecino FINO se calcula fuera del bucle: hay que mirar hacia
                //  ABAJO, y `nodeNeighbourLevels` solo sabe subir — ver `nodeNeighbourFinerMask`.)
                g.edge[e] = (step > sOwn) ? (int32_t)step : 0;
            }
            g.slot[0] = r.slot;
            g.slot[1] = 0;   // libre: era la mascara del morph por arista, borrado por medida
            // Ver la nota larga de `terrain_node.vert`: probado, medido y revertido.
            fineMask = 0;
            g.slot[3] = fineMask;      // aristas con vecino MAS FINO: ahi no morfeo (ver arriba)
            g.slot[2] = (int32_t)skOwn;   // solo para la vista 4: color por STRIDE
            // ⚠️ EL MORPH POR DISTANCIA YA NO VIAJA POR AQUI. Era `nodeParentMorph`, un escalar por
            // NODO, y por eso mismo no podía casar en las aristas: dos vecinos a distancias distintas
            // evaluaban el mismo punto compartido con morphs distintos (medido: hasta 10,07 m). Ahora
            // el vértice lo calcula con SU distancia, a partir de `du.lod` — ver `terrain_node.vert`.
            g.misc[0] = 0.0f;
            m_inst.push_back(g);
            ++fs.drawn;
        }
        m_group[pass + 1] = m_inst.size();       // fin del grupo `pass`, inicio del siguiente
        }
        if (m_inst.empty()) { m_stats = fs; return fs; }

        // ⚠️ UN SSBO POR GRUPO, NO UNO CON DESPLAZAMIENTO. `drawIndexed` no tiene `baseInstance`, así
        // que cada draw empieza a contar instancias en 0 y necesita ver SOLO las suyas. Meter el
        // desplazamiento en el UBO y reescribirlo entre draws es exactamente el fallo que documenta
        // el bloque de arriba: en Vulkan los draws GRABADOS leerían todos el último valor. Cambiar
        // de HANDLE sí se graba bien — es una escritura de descriptor, no un memcpy.
        for (uint32_t k = 0; k < m_strideCount; ++k) {
            const size_t n = m_group[k + 1] - m_group[k];
            if (n == 0) continue;
            const size_t needB = n * sizeof(NodeInstGPU);
            if (needB > m_instCap[k]) {                // crece y no encoge: evita recrear cada frame
                if (RHI::valid(m_instSSBO[k])) m_dev->destroy(m_instSSBO[k]);
                m_instCap[k]  = needB * 2;
                m_instSSBO[k] = m_dev->createBuffer(RHI::BufferUsage::Storage, m_instCap[k],
                                                    nullptr, RHI::BufferMemory::Dynamic);
            }
            m_dev->updateBuffer(m_instSSBO[k], 0, needB, m_inst.data() + m_group[k]);
        }

        DrawUBO du{};
        du.mvp     = mvp;
        // ⚠️ PARTIDO EN DOS, y la resta en double antes de partir. Un float de 6,37e6 tiene un ulp de
        // 0,5 m, asi que meter esto entero en `vec4` hacia saltar el terreno medio metro cada vez que
        // la camara cruzaba un escalon — 0,53 m entre frames andando a 5 m/s, medido. El trozo gordo
        // se cuantiza a 64 m (exacto en float a esta magnitud) y el fino viaja aparte.
        const glm::dvec3 rel = planetCenter - camPos;
        const double kQ = 64.0;
        const glm::dvec3 hi(std::round(rel.x / kQ) * kQ, std::round(rel.y / kQ) * kQ,
                            std::round(rel.z / kQ) * kQ);
        du.center   = glm::vec4(glm::vec3(hi), 0.0f);
        du.centerLo = glm::vec4(glm::vec3(rel - hi), 0.0f);
        // ── LO QUE EL VERTICE NECESITA PARA SU PROPIO MORPH ─────────────────────────────────────
        // Antes esto se calculaba en CPU por nodo (`nodeParentMorph` -> `g.misc[0]`) y salia UN
        // escalar constante en toda su superficie: dos vecinos daban valores distintos sobre la
        // arista que comparten y eso abria hasta 10,07 m de grieta. Ahora se manda la REGLA y la
        // aplica cada vertice con su propia distancia, que es lo unico que los dos lados comparten.
        du.lod[0] = (float)m_lastRadPerPx;
        du.lod[1] = (float)errorPx();
        du.lod[2] = (float)(planetRadiusM * 1.5707963267948966);   // gemelo de `nodeSpanM` en nivel 0
        du.lod[3] = noMorph() ? 0.0f : 1.0f;
        du.grid[0] = (int32_t)TERRAIN_NODE_TEXELS;
        du.grid[1] = (int32_t)TERRAIN_NODE_CELLS;
        du.misc[0] = (float)planetRadiusM;
        du.misc[2] = (float)((m_debugOverride >= 0) ? m_debugOverride : debugView());
        du.misc[1] = m_gpu.hasHeightTex() ? 1.0f : 0.0f;      // ¿hay bake EQUIRECT? (el preferido)
        du.misc[3] = RHI::valid(m_baseField) ? 1.0f : 0.0f;   // ¿hay campo del cubo? (clima + respaldo)

        const bool shadeOn = m_shade.on && RHI::valid(m_shade.albedo) &&
                             RHI::valid(m_shade.materialUBO);
        du.shade[0] = m_shade.tiling;
        du.shade[1] = (float)m_shade.shoreLayer;
        du.shade[2] = (float)((RHI::valid(m_shade.macro) ? 1 : 0) |
                              (RHI::valid(m_shade.biome) ? 2 : 0) |
                              (RHI::valid(m_shade.zone)  ? 4 : 0));
        du.shade[3] = shadeOn ? 1.0f : 0.0f;
        du.texAnchor = glm::vec4(m_shade.texAnchor, 0.0f);
        du.lightDir  = glm::vec4(m_shade.lightDir, 0.0f);
        m_dev->updateBuffer(m_ubo, 0, sizeof(du), &du);   // UNA vez, antes de cualquier draw

        ctx->bindPipeline(m_pipe);
        ctx->bindUniformBuffer(0, m_ubo);
        ctx->bindStorageBuffer(1, m_gpu.heights());
        // La ranura 2 la reata cada grupo justo antes de su draw (abajo).
        // ⚠️ TODOS los samplers que el shader DECLARA se atan, se usen o no. En Vulkan un descriptor
        // sin escribir es INDEFINIDO —no ceros— y muestrearlo puede perder el dispositivo. Los flags
        // de `uShade.z` deciden si se LEEN; atarlos es obligatorio igual.
        // ⚠️ SIEMPRE, aunque `uMisc.w` diga que no se lee. En Vulkan un descriptor sin escribir es
        // INDEFINIDO; en OpenGL la unidad 15 conserva lo que dejara otro pase, y si lo que hay ahi
        // es de OTRO TIPO (un sampler2D donde el shader declara sampler2DArray) el draw entero pasa
        // a ser invalido. Costó dos tests del banco que no tenian nada que ver con esto.
        ctx->bindTexture(15, m_gpu.baseFieldOrDummy());
        ctx->bindTexture(16, m_gpu.heightTexOrDummy());
        if (shadeOn) {
            ctx->bindUniformBuffer(12, m_shade.materialUBO);
            ctx->bindTexture(12, m_shade.albedo);
            ctx->bindTexture(13, RHI::valid(m_shade.normal) ? m_shade.normal : m_shade.albedo);
            if (RHI::valid(m_shade.macro)) ctx->bindTexture(10, m_shade.macro);
            if (RHI::valid(m_shade.biome)) ctx->bindTexture(11, m_shade.biome);
            if (RHI::valid(m_shade.zone))  ctx->bindTexture(14, m_shade.zone);
        }
        ctx->bindVertexBuffer(m_vb);
        fs.tris = 0;
        for (uint32_t k = 0; k < m_strideCount; ++k) {
            const size_t n = m_group[k + 1] - m_group[k];
            if (n == 0 || !RHI::valid(m_instSSBO[k])) continue;
            ctx->bindStorageBuffer(2, m_instSSBO[k]);
            ctx->bindIndexBuffer(m_ibs[k]);
            ctx->drawIndexed(m_indexCount[k], 0, (uint32_t)n);
            fs.tris += n * (size_t)(m_indexCount[k] / 3);
            ++fs.drawCalls;
        }
        fs.resident = m_pool.residentCount();
        { const auto ps = m_pool.stats(); fs.evicted = ps.evicted; fs.pubFailed = ps.publishFailed; }
        m_stats = fs;
        return fs;
    }

    /// Gemelo de `NodeInst` de `terrain_node.vert`. std430: 4 ivec4/vec4 = 64 B, alineado a 16.
    /// `node` = cara/nivel/i/j · `edge` = zancada del cosido por arista (0 = no coser) ·
    /// `slot` = hueco / (libre) / índice de stride / (libre) · `misc` = (libre).
    struct NodeInstGPU { int32_t node[4]; int32_t edge[4]; int32_t slot[4]; float misc[4]; };
    static_assert(sizeof(NodeInstGPU) == 64, "NodeInst std430 descuadrado");

    /**
     * @brief Lo que el pase ENVÍA de verdad este frame, para auditarlo.
     *
     * ⚠️ Es público a propósito. Durante la caza de grietas del 2026-08-25, SEIS modelos de CPU
     * dijeron "limpio" mientras el shader dejaba costura, y el último dato que nadie había leído del
     * motor era precisamente éste: la zancada de cosido que cada nodo recibe. Un test que la
     * reconstruye vuelve a ser un gemelo; uno que la LEE, no.
     *
     * Válido después de `draw()` y hasta el siguiente. No se copia: es el buffer que se subió.
     */
    const std::vector<NodeInstGPU>& instancesSent() const { return m_inst; }

private:

    /// Gemelo del bloque `NodeDraw` de `terrain_node.vert`. std140: mat4 + 4 vec4 de 16 B.
    struct DrawUBO {
        glm::mat4 mvp{1.0f};
        glm::vec4 center{0.0f};      ///< parte GRUESA, multiplo de 64 m: exacta en float
        glm::vec4 centerLo{0.0f};    ///< el resto. Gemelo de `uCenterLo`; ver la nota del .vert
        /// x = radianes por píxel · y = errorPx · z = arco del nivel 0 (m) · w = ¿morph encendido?
        /// Es lo que el vértice necesita para decidir su PROPIO morph; ver `terrain_node.vert`.
        float   lod[4]{};
        int32_t grid[4]{}; int32_t edge[4]{};
        float   misc[4]{};
        float   shade[4]{};
        glm::vec4 texAnchor{0.0f};
        glm::vec4 lightDir{0.0f, 1.0f, 0.0f, 0.0f};
    };


    Shade                    m_shade;
    RHI::TextureHandle       m_baseField{};
    std::vector<NodeInstGPU> m_inst;      ///< ORDENADO por grupo de stride; `m_group` los delimita
    std::vector<size_t>      m_group;     ///< `m_group[k]` = primera instancia del stride `k`
    std::vector<uint32_t>    m_skOf;      ///< stride de cada `m_resolved[i]`, de `prepare`
    /// Stride por nodo del frame ANTERIOR: es lo que da histeresis y quita el parpadeo. Tras el
    /// `swap` de `prepare` contiene el de ESTE frame, que es lo que `draw` consulta para el vecino.
    std::unordered_map<uint64_t, uint32_t> m_skPrev, m_skNow;

    // Estado que cruza de `prepare` a `draw` (ver la nota de arriba sobre por qué son dos).
    FrameStats                          m_stats;
    int                                 m_debugOverride = -1;   ///< ver setDebugViewOverride
    double                              m_lastRadPerPx = 1.0;   ///< de `prepare`, lo usa el morph
    std::unordered_map<uint64_t, uint32_t> m_index;

    RHI::Device*        m_dev = nullptr;
    RHI::PipelineHandle m_pipe{};
    static constexpr uint32_t kStrides = 7;   // 1,2,4,…,64 (129 téxeles -> hasta 3x3)
    RHI::BufferHandle   m_vb{}, m_ubo{};
    RHI::BufferHandle   m_ibs[kStrides]{};
    uint32_t            m_indexCount[kStrides]{};
    RHI::BufferHandle   m_instSSBO[kStrides]{};   ///< uno por grupo: `drawIndexed` no tiene baseInstance
    size_t              m_instCap[kStrides]{};
    uint32_t            m_strideCount = 0;
    bool                m_ready = false;
    bool                m_rootsPinned = false;
    TerrainNodeGpu      m_gpu;
    // ⚠️ SE REDIMENSIONA EN `init` CON LA MISMA CAPACIDAD QUE EL SSBO. Estaba a fuego en 1024
    // mientras `m_gpu` sí recibía el parámetro, así que subir la capacidad agrandaba el buffer y el
    // tope del selector pero NO los huecos: medido en el juego con 4096, `residentes 1024/4096` y
    // **2 035 de 3 053 nodos dibujados por ancestro** — dos de cada tres más gruesos de lo pedido.
    TerrainNodePool     m_pool{ 1024, 43 };
    std::vector<NodeId> m_sel, m_drawn;
    std::vector<uint8_t> m_drop;   // mascara de `nodeCoveredMask`
    std::vector<TerrainNodePool::Resolved> m_resolved;

};

}} // namespace Haruka::Terrain
