/**
 * @file terrain_node_gpu.h
 * @brief ADAPTADOR GPU del pool de nodos (v5, F2). Pega dos piezas ya probadas por separado.
 *
 * `TerrainNodePool` decide QUÉ nodos y en qué hueco (política pura, testeada headless).
 * `terrain_node.comp` sabe LLENAR un nodo (probado y medido: 0,046 ms el caso fino).
 * Esto es lo único que faltaba entre los dos, y a propósito es lo más fino posible: si el pegamento
 * acumula decisiones, la política deja de estar donde se puede probar.
 *
 * ── EL POOL ES **UN** BUFFER ────────────────────────────────────────────────────────────────────
 *
 * Todos los nodos viven en un SSBO contiguo: el hueco `k` ocupa `[k·TEXELS², (k+1)·TEXELS²)`.
 * Generar un nodo distinto solo cambia un entero del UBO de parámetros — sin crear recursos, sin
 * rebindear buffers y sin sincronizar nada entre nodos, porque cada dispatch escribe en un rango
 * disjunto. Es lo que permite encolar los 43 del presupuesto en una sola tanda.
 *
 * ⚠️ SSBO y no array de texturas porque el RHI tiene `bindStorageBuffer` pero NO binding de storage
 * image. Cuando el render lo consuma habrá que decidir si se muestrea como buffer o si se añade esa
 * capacidad; para F2 —que no dibuja— el buffer basta y no obliga a tocar el RHI.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "terrain_node.h"
#include "terrain_node_pool.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_resources.h"

namespace Haruka { namespace Terrain {

class TerrainNodeGpu {
public:
    /// Téxeles de un nodo (TEXELS²) y bytes que ocupa su hueco.
    static constexpr size_t kTexelsPerNode = (size_t)TERRAIN_NODE_TEXELS * TERRAIN_NODE_TEXELS;
    /// ⚠️ TRES alturas por téxel: la propia, la del PADRE y la del ABUELO. El hueco `k` ocupa
    /// `[k·3·TEXELS², …)` en ese orden.
    ///
    /// La del padre cierra el escalón de 2,4 m al cambiar de nivel (geomorphing). La del ABUELO se
    /// añadió el 2026-08-25 y cierra el que quedaba: el nodo fino morfeaba hacia la altura CRUDA de
    /// su padre, pero el vecino grueso —que ESTÁ en ese nivel— se morfea a su vez hacia el suyo, así
    /// que los dos lados dibujaban superficies distintas sobre la arista compartida. Medido: 2,747 m
    /// de escalón, y el 100 % de las grietas en pantalla caían en fronteras de nivel
    /// (`v5 F3: grietas entre nodos vecinos`). Con el abuelo, el fino puede apuntar a lo que el
    /// grueso DIBUJA —`mix(padre, abuelo, m(L−1))`— en vez de a su altura cruda.
    ///
    /// ⚠️ Cuesta un 50 % más de VRAM: 130 → 195 KB por nodo, o sea 260 → 390 MB con 2048 huecos.
    /// Se probaron antes DOS alternativas sin memoria extra —estrechar las rampas del morph por
    /// arista, y apagar el morph del grueso junto a un vecino fino— y las dos cambiaron un pincho por
    /// otro. La razón es estructural y está en `terrain_node.vert`: el morph tiene que ser función
    /// SOLO de (nivel, posición del vértice), y toda solución sin el abuelo depende de los vecinos.
    static constexpr size_t kFloatsPerNode = kTexelsPerNode * 3;   ///< propias + padre + abuelo
    static constexpr size_t kBytesPerNode  = kFloatsPerNode * sizeof(float);

    /** @param computePath ruta a `shaders/terrain_node.comp`. @return false si no hay device o pipeline. */
    bool init(RHI::Device* dev, const char* computePath, size_t capacity) {
        m_dev = dev; m_capacity = capacity;
        if (!m_dev || capacity == 0) return false;
        RHI::PipelineDesc pd; pd.computePath = computePath;
        m_pipe = m_dev->createPipeline(pd);
        if (!RHI::valid(m_pipe)) return false;
        m_heights = m_dev->createBuffer(RHI::BufferUsage::Storage, capacity * kBytesPerNode,
                                        nullptr, RHI::BufferMemory::Dynamic);
        ParamsUBO p{};
        m_params = m_dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(p), &p,
                                       RHI::BufferMemory::Dynamic);
        // ⚠️ EL RELLENO SE CREA AQUI, NO EN EL CAMINO DE DIBUJO. Estaba creandose perezosamente
        // dentro de `draw()`, que corre DENTRO del render pass: crear recursos ahi es pedirle
        // problemas al backend, y en OpenGL rompio el readback de otro test del banco.
        RHI::TextureDesc dd;
        dd.width = dd.height = 1; dd.layers = 6;
        dd.format = RHI::Format::RGBA32F;
        dd.filter = RHI::Filter::Nearest; dd.wrap = RHI::Wrap::ClampToEdge;
        static const float zeros[6 * 4] = {};
        dd.initialData = zeros;
        m_baseDummy = m_dev->createTexture(dd);
        RHI::TextureDesc hd;                       // 1x1 de relleno para el binding 16
        hd.width = hd.height = 1; hd.layers = 1;
        hd.format = RHI::Format::R32F;
        hd.filter = RHI::Filter::Nearest; hd.wrap = RHI::Wrap::ClampToEdge;
        static const float zero1 = 0.0f;
        hd.initialData = &zero1;
        m_heightDummy = m_dev->createTexture(hd);
        return RHI::valid(m_heights) && RHI::valid(m_params);
    }

    void shutdown() {
        if (!m_dev) return;
        if (RHI::valid(m_params))  { m_dev->destroy(m_params);  m_params  = {}; }
        for (auto& b : m_paramRing) if (RHI::valid(b)) m_dev->destroy(b);
        m_paramRing.clear();
        if (RHI::valid(m_baseDummy)) { m_dev->destroy(m_baseDummy); m_baseDummy = {}; }
        if (RHI::valid(m_heightDummy)) { m_dev->destroy(m_heightDummy); m_heightDummy = {}; }
        if (RHI::valid(m_heights)) { m_dev->destroy(m_heights); m_heights = {}; }
        if (RHI::valid(m_pipe))    { m_dev->destroy(m_pipe);    m_pipe    = {}; }
        m_dev = nullptr;
    }

    /**
     * @brief Genera en GPU los nodos que el pool tiene pendientes y los publica.
     *
     * ⚠️ El hueco se reserva ANTES del dispatch (`publish`), no después. Si se hiciera al revés, un
     * dispatch podría escribir en un hueco que el LRU acaba de dar a otro nodo — y el resultado sería
     * un nodo con el contenido de otro sitio, que en pantalla es un trozo de terreno ajeno. Reservar
     * primero también deja que `publish` RECHACE (devuelve −1) cuando todo está en uso este frame, y
     * entonces simplemente no se dispatcha: el nodo entra en el frame siguiente.
     *
     * ⚠️ El RANGO se publica INVÁLIDO. El min/max real solo se conoce leyendo el contenido, y hacer
     * readback por nodo mataría la asincronía entera. Hasta que el compute lo calcule por su cuenta
     * (una reducción, o atómicos sobre dos floats), el selector usa la herencia del padre — que ya
     * está medida: los hijos se salen 0,1 % de la amplitud del padre.
     *
     * @return cuántos nodos se han dispatchado.
     */
    /**
     * @brief Muestreador de la altura de superficie (m) para estimar el rango de cada nodo.
     *
     * Sin el, `nodeEstimateRange` devuelve invalido y todo vuelve a medirse al nivel del mar.
     */
    void setHeightSampler(NodeBaseHeightFn fn, void* ctx) { m_heightFn = fn; m_heightCtx = ctx; }

    /**
     * @brief Genera los nodos pendientes. **El `ctx` lo pone el llamador, y no es un detalle.**
     *
     * ⚠️ ESTO ABRIA SU PROPIO FRAME (`m_dev->beginFrame()` / `endFrame()`), y en OpenGL
     * `GLDevice::endFrame()` es **`SDL_GL_SwapWindow`**. O sea que cada vez que habia nodos que
     * generar, el generador de terreno PRESENTABA LA VENTANA a media escena, antes de que el propio
     * terreno se dibujara. Y solo pasa cuando hay cola de generacion — al andar o girar—, que es
     * exactamente cuando se reportaron el parpadeo y los pinchos.
     *
     * Ningun test headless podia verlo: la aritmetica esta bien (reconstruida vertice a vertice, el
     * pase no anade ni 0,0001 m de discontinuidad sobre el campo). El fallo no estaba en QUE se
     * dibuja sino en CUANDO se presenta.
     *
     * `prepare` ya corre **fuera del render pass** —esta documentado justo para poder despachar
     * compute ahi—, asi que su `ctx` vale y no hace falta abrir nada.
     */
    size_t generatePending(RHI::Context* ctx, TerrainNodePool& pool, double planetRadiusM) {
        if (!m_dev || !RHI::valid(m_pipe) || !ctx) return 0;
        const std::vector<NodeId> pending = pool.takePending();   // copia: `publish` toca el pool
        if (pending.empty()) return 0;

        ctx->bindPipeline(m_pipe);
        ctx->bindStorageBuffer(1, m_heights);
        const bool hasBase = RHI::valid(m_baseField);
        ctx->bindTexture(15, baseFieldOrDummy());
        ctx->bindTexture(16, heightTexOrDummy());

        // ⚠️ EL TOPE SE APLICA AQUI TAMBIEN. `prioritisePending` lo recorta antes, pero si alguien
        // genera sin ordenar (un test, otro llamador) el presupuesto no puede saltarse en silencio:
        // 170 nodos a 0,27 ms serian 45 ms de frame.
        const size_t budget = pool.genPerFrame();
        size_t issued = 0;
        for (const NodeId& n : pending) {
            if (issued >= budget) break;
            if (!nodeIndicesValid(n)) continue;          // petición mal formada: no se dispatcha
            // ⚠️ EL RANGO, NO `NodeRange{}`. Publicar vacio dejaba a `rangeOf` devolviendo invalido
            // SIEMPRE (medido en el juego: `SIN RANGO 3070` de 3070), y de eso viven el criterio de
            // subdivision, el stride por nodo y la envolvente del frustum. Ver `nodeEstimateRange`.
            const int slot = pool.publish(n, nodeEstimateRange(n, planetRadiusM,
                                                               m_heightFn, m_heightCtx));
            if (slot < 0) continue;                      // pool lleno de visibles: el frame que viene

            ParamsUBO p{};
            p.node[0] = (int32_t)n.face; p.node[1] = (int32_t)n.level;
            p.node[2] = (int32_t)n.i;    p.node[3] = (int32_t)n.j;
            p.grid[0] = (int32_t)TERRAIN_NODE_TEXELS; p.grid[1] = (int32_t)TERRAIN_NODE_CELLS;
            p.grid[2] = m_debugMode;                     // 0 = producción; ver `setDebugMode`
            p.grid[3] = slot;
            p.misc[0] = (float)planetRadiusM;
            p.misc[1] = (float)nodeTexelM(n, planetRadiusM);
            p.misc[2] = hasBase ? 1.0f : 0.0f;         // ¿hay campo del cubo? (ver `setBaseField`)
            p.misc[3] = hasHeightTex() ? 1.0f : 0.0f;  // ¿hay bake EQUIRECT? Es el preferido.
            // ⚠️ UN UBO POR DISPATCH. REESCRIBIR UNO SOLO ENTRE DISPATCHES NO FUNCIONA EN VULKAN.
            //
            // Esto era `updateBuffer(m_params, ...)` seguido de `dispatch`, en bucle. En OpenGL el
            // driver hace ghosting del buffer y cada dispatch ve sus propios parámetros. En Vulkan
            // `updateBuffer` sobre memoria mapeada es un `memcpy` en tiempo de GRABACIÓN: los N
            // dispatches quedan grabados apuntando al MISMO buffer y, al ejecutarse, todos leen el
            // ÚLTIMO valor escrito. Todos los huecos del pool acaban con el contenido del último nodo.
            //
            // Medido por la auditoría de F2, que compara cada hueco con la referencia de CPU:
            //   OpenGL  → 0 de 8 huecos con contenido ajeno · peor 0,0150 m
            //   Vulkan  → 7 de 8 huecos con contenido ajeno · peor 107,2421 m
            //
            // Es la MISMA enfermedad que tenía el pase de dibujo (allí se curó con un draw instanciado
            // y los datos por nodo en un SSBO). Aquí basta con que cada dispatch tenga su propio UBO:
            // son 48 B, se reutilizan entre frames y no obliga a tocar el shader — que importa porque
            // los tests de bisección de F1 montan su propio UBO contra el mismo binding.
            // El buffer se crea con los datos ya dentro cuando es nuevo; los reutilizados van por
            // `updateBuffer`. (Se probó como hipótesis para una divergencia de 399 m en OpenGL y NO
            // era la causa — se conserva porque evita un `updateBuffer` en el estreno, no porque
            // arregle nada.)
            RHI::BufferHandle pb;
            if (m_paramRing.size() <= issued) {
                pb = m_dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(p), &p,
                                         RHI::BufferMemory::Dynamic);
                m_paramRing.push_back(pb);
            } else {
                pb = m_paramRing[issued];
                m_dev->updateBuffer(pb, 0, sizeof(p), &p);
            }
            ctx->bindUniformBuffer(0, pb);
            const uint32_t g = (TERRAIN_NODE_TEXELS + 7u) / 8u;
            ctx->dispatch(g, g, 1);
            ++issued;
        }
        // UNA barrera para toda la tanda: los dispatch escriben en rangos disjuntos del mismo buffer,
        // así que no necesitan ordenarse entre sí — solo frente a quien lea después.
        ctx->memoryBarrier();
        return issued;
    }

    /**
     * @brief El bake del planeta (elev, temp, humedad) como array de 6 caras. Sin él, el nodo
     *        describe SOLO el detalle procedural: un planeta sin continentes ni costa.
     *
     * ⚠️ Se ata SIEMPRE, aunque no se use. En Vulkan un descriptor que nadie escribe es INDEFINIDO
     * —no ceros— y muestrearlo no da negro: da basura o pierde el dispositivo. Cuando no hay bake se
     * ata un 1x1 de relleno y el shader lo salta por `uMisc.z`.
     */
    void setBaseField(RHI::TextureHandle t) { m_baseField = t; }

    /// El bake EQUIRECT de altura (binding 16). Es la fuente de elevación que usan el clipmap y la
    /// física; el campo del cubo es el respaldo. Ver la nota de `terrain_node.comp`.
    void setHeightTex(RHI::TextureHandle t) { m_heightTex = t; }
    RHI::TextureHandle heightTexOrDummy() const {
        return RHI::valid(m_heightTex) ? m_heightTex : m_heightDummy;
    }
    bool hasHeightTex() const { return RHI::valid(m_heightTex); }

    /// Modo de salida del compute para BISECAR (0 = producción). Ver `uGrid.z` en el shader.
    void setDebugMode(int m) { m_debugMode = m; }

    /// El bake, o el 1x1 de relleno si no lo hay. **Siempre válido**: el que dibuja tiene que atar
    /// algo a la unidad 15 aunque no vaya a leerlo (ver `setBaseField`).
    RHI::TextureHandle baseFieldOrDummy() const {
        return RHI::valid(m_baseField) ? m_baseField : m_baseDummy;
    }

    RHI::BufferHandle heights() const { return m_heights; }
    size_t capacity() const { return m_capacity; }
    size_t bytes()    const { return m_capacity * kBytesPerNode; }

private:
    NodeBaseHeightFn m_heightFn = nullptr;
    void*            m_heightCtx = nullptr;
    /// Gemelo del bloque `NodeParams` de `terrain_node.comp`. std140: tres vec4 de 16 B, sin relleno.
    struct ParamsUBO { int32_t node[4]; int32_t grid[4]; float misc[4]; };

    RHI::Device*       m_dev = nullptr;
    RHI::PipelineHandle m_pipe{};
    RHI::BufferHandle   m_heights{};
    int                 m_debugMode = 0;
    RHI::TextureHandle  m_baseField{}, m_baseDummy{};
    RHI::TextureHandle  m_heightTex{}, m_heightDummy{};
    RHI::BufferHandle   m_params{};
    /// Un UBO por dispatch de la tanda (ver la nota en `generatePending`). Crece y no encoge.
    std::vector<RHI::BufferHandle> m_paramRing;
    size_t              m_capacity = 0;
};

}} // namespace Haruka::Terrain
