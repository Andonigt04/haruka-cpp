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

#include <cstdlib>
#include <string>
#include <vector>

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
    static bool enabled() {
        static const bool s_on = std::getenv("HARUKA_TERRAIN_V5") != nullptr;
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
    static int debugView() {
        static const int s_v = []() {
            const char* v = std::getenv("HARUKA_TERRAIN_V5_DEBUG");
            return v ? std::atoi(v) : 0;
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

    bool init(RHI::Device* dev, const std::string& shaderDir, size_t capacity = 1024) {
        if (m_ready || !dev) return m_ready;
        m_dev = dev;
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
        return m_ready;
    }

    void shutdown() {
        if (!m_dev) return;
        if (RHI::valid(m_ubo)) { m_dev->destroy(m_ubo); m_ubo = {}; }
        for (uint32_t k = 0; k < m_strideCount; ++k)
            if (RHI::valid(m_ibs[k])) { m_dev->destroy(m_ibs[k]); m_ibs[k] = {}; }
        m_strideCount = 0;
        if (RHI::valid(m_vb))  { m_dev->destroy(m_vb);  m_vb  = {}; }
        if (RHI::valid(m_pipe)){ m_dev->destroy(m_pipe);m_pipe= {}; }
        m_gpu.shutdown();
        m_dev = nullptr; m_ready = false;
    }

    struct FrameStats {
        size_t drawn = 0, generated = 0, resident = 0, ancestors = 0, tris = 0;
        uint32_t stride = 1;
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
    void setBaseField(RHI::TextureHandle t) { m_gpu.setBaseField(t); m_baseField = t; }

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
            if (m_gpu.generatePending(m_pool, planetRadiusM) > 0) {
                for (int f = 0; f < 6; ++f) m_pool.pin(NodeId{ (PlanetFace)f, 0, 0, 0 });
                m_rootsPinned = true;
            }
        }

        m_pool.beginFrame();
        nodeSelectVisible(planetRadiusM, camPos, planetCenter, radPerPx, m_sel,
                          m_gpu.capacity(), errorPx(), &viewDir, coneHalfAngle,
                          5000.0, &TerrainNodePool::rangeFnAdapter, &m_pool);
        fs.selected = m_sel.size();
        // Cuánto descarta cada recorte, por separado: sin el desglose no se sabe cuál se pasa.
        {
            std::vector<NodeId> noCull, horizOnly;
            nodeSelectVisible(planetRadiusM, camPos, planetCenter, radPerPx, noCull,
                              m_gpu.capacity(), errorPx());
            nodeSelectVisible(planetRadiusM, camPos, planetCenter, radPerPx, horizOnly,
                              m_gpu.capacity(), errorPx());
            fs.culledHorizon = noCull.size() > horizOnly.size() ? noCull.size() - horizOnly.size() : 0;
            fs.culledFrustum = horizOnly.size() > m_sel.size() ? horizOnly.size() - m_sel.size() : 0;
        }
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
        fs.generated = m_gpu.generatePending(m_pool, planetRadiusM);

        // Niveles de vecino para el cosido, sobre el conjunto que DE VERDAD se va a dibujar (que no
        // es el seleccionado: los que cayeron a un ancestro dibujan el ancestro).
        m_drawn.clear(); m_drawn.reserve(m_resolved.size());
        for (const auto& r : m_resolved) if (r.slot >= 0) m_drawn.push_back(r.node);
        const auto index = nodeDrawnIndex(m_drawn);

        // STRIDE GLOBAL, no por nodo: el selector ya iguala el tamaño en pantalla de los nodos (eso
        // es lo que hace su métrica de error), así que todos quieren el mismo. Uno global además NO
        // crea T-junctions nuevas — dos nodos vecinos dibujan la misma densidad.
        uint32_t sk = 0;
        {
            const double want = vertexPx() / std::max(errorPx(), 1e-3);   // téxeles por vértice
            while (sk + 1 < m_strideCount && (double)(1u << (sk + 1)) <= want) ++sk;
        }
        fs.stride = 1u << sk;
        m_sk = sk; m_index = index;
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
        const uint32_t sk = m_sk;
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
        m_inst.clear(); m_inst.reserve(m_resolved.size());
        for (const auto& r : m_resolved) {
            if (r.slot < 0) { ++fs.noSlot; continue; }
            if (!r.exact) ++fs.ancestors;
            NodeInstGPU g{};
            g.node[0] = (int32_t)r.node.face; g.node[1] = (int32_t)r.node.level;
            g.node[2] = (int32_t)r.node.i;    g.node[3] = (int32_t)r.node.j;
            nodeNeighbourLevels(r.node, index, g.edge);
            g.slot[0] = r.slot;
            m_inst.push_back(g);
            ++fs.drawn;
        }
        if (m_inst.empty()) { m_stats = fs; return fs; }

        const size_t needB = m_inst.size() * sizeof(NodeInstGPU);
        if (needB > m_instCap) {                       // crece y no encoge: evita recrear cada frame
            if (RHI::valid(m_instSSBO)) m_dev->destroy(m_instSSBO);
            m_instCap  = needB * 2;
            m_instSSBO = m_dev->createBuffer(RHI::BufferUsage::Storage, m_instCap,
                                             nullptr, RHI::BufferMemory::Dynamic);
        }
        m_dev->updateBuffer(m_instSSBO, 0, needB, m_inst.data());

        DrawUBO du{};
        du.mvp     = mvp;
        du.center  = glm::vec4(glm::vec3(planetCenter - camPos), 0.0f);
        du.grid[0] = (int32_t)TERRAIN_NODE_TEXELS;
        du.grid[1] = (int32_t)TERRAIN_NODE_CELLS;
        du.misc[0] = (float)planetRadiusM;
        du.misc[2] = (float)debugView();
        du.misc[3] = RHI::valid(m_baseField) ? 1.0f : 0.0f;   // ¿hay bake que muestrear en el vértice?

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
        ctx->bindStorageBuffer(2, m_instSSBO);
        // ⚠️ TODOS los samplers que el shader DECLARA se atan, se usen o no. En Vulkan un descriptor
        // sin escribir es INDEFINIDO —no ceros— y muestrearlo puede perder el dispositivo. Los flags
        // de `uShade.z` deciden si se LEEN; atarlos es obligatorio igual.
        // ⚠️ SIEMPRE, aunque `uMisc.w` diga que no se lee. En Vulkan un descriptor sin escribir es
        // INDEFINIDO; en OpenGL la unidad 15 conserva lo que dejara otro pase, y si lo que hay ahi
        // es de OTRO TIPO (un sampler2D donde el shader declara sampler2DArray) el draw entero pasa
        // a ser invalido. Costó dos tests del banco que no tenian nada que ver con esto.
        ctx->bindTexture(15, m_gpu.baseFieldOrDummy());
        if (shadeOn) {
            ctx->bindUniformBuffer(12, m_shade.materialUBO);
            ctx->bindTexture(12, m_shade.albedo);
            ctx->bindTexture(13, RHI::valid(m_shade.normal) ? m_shade.normal : m_shade.albedo);
            if (RHI::valid(m_shade.macro)) ctx->bindTexture(10, m_shade.macro);
            if (RHI::valid(m_shade.biome)) ctx->bindTexture(11, m_shade.biome);
            if (RHI::valid(m_shade.zone))  ctx->bindTexture(14, m_shade.zone);
        }
        ctx->bindVertexBuffer(m_vb);
        ctx->bindIndexBuffer(m_ibs[sk]);
        ctx->drawIndexed(m_indexCount[sk], 0, (uint32_t)m_inst.size());

        fs.tris = fs.drawn * (size_t)(m_indexCount[sk] / 3);
        fs.resident = m_pool.residentCount();
        m_stats = fs;
        return fs;
    }

private:
    /// Gemelo de `NodeInst` de `terrain_node.vert`. std430: 3 ivec4 = 48 B, alineado a 16.
    struct NodeInstGPU { int32_t node[4]; int32_t edge[4]; int32_t slot[4]; };
    static_assert(sizeof(NodeInstGPU) == 48, "NodeInst std430 descuadrado");

    /// Gemelo del bloque `NodeDraw` de `terrain_node.vert`. std140: mat4 + 4 vec4 de 16 B.
    struct DrawUBO {
        glm::mat4 mvp{1.0f};
        glm::vec4 center{0.0f};
        int32_t node[4]{}; int32_t grid[4]{}; int32_t edge[4]{};
        float   misc[4]{};
        float   shade[4]{};
        glm::vec4 texAnchor{0.0f};
        glm::vec4 lightDir{0.0f, 1.0f, 0.0f, 0.0f};
    };


    Shade                    m_shade;
    RHI::TextureHandle       m_baseField{};
    std::vector<NodeInstGPU> m_inst;
    RHI::BufferHandle        m_instSSBO{};
    size_t                   m_instCap = 0;

    // Estado que cruza de `prepare` a `draw` (ver la nota de arriba sobre por qué son dos).
    FrameStats                          m_stats;
    uint32_t                            m_sk = 0;
    std::unordered_map<uint64_t, uint32_t> m_index;

    RHI::Device*        m_dev = nullptr;
    RHI::PipelineHandle m_pipe{};
    static constexpr uint32_t kStrides = 7;   // 1,2,4,…,64 (129 téxeles -> hasta 3x3)
    RHI::BufferHandle   m_vb{}, m_ubo{};
    RHI::BufferHandle   m_ibs[kStrides]{};
    uint32_t            m_indexCount[kStrides]{};
    uint32_t            m_strideCount = 0;
    bool                m_ready = false;
    bool                m_rootsPinned = false;
    TerrainNodeGpu      m_gpu;
    TerrainNodePool     m_pool{ 1024, 43 };
    std::vector<NodeId> m_sel, m_drawn;
    std::vector<TerrainNodePool::Resolved> m_resolved;

};

}} // namespace Haruka::Terrain
