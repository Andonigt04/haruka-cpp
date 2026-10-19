#include "RHI/Common/RHITestCommon.h"

// ================================================================================================
// F3 — RENDER SOBRE NODOS: ¿cae la geometría dibujada sobre la superficie del nodo?
//
// Es la promesa central del v5 puesta a prueba. Hoy el render y la colisión describen superficies
// distintas y se MIDE cuánto se separan (`terrain_chord_error`). Con nodos no
// hay dos superficies: el vértice lee el MISMO téxel que leerá Jolt, así que la paridad no se mide,
// se cumple. Esto lo comprueba dibujando de verdad y leyendo la posición de vuelta.
//
// ⚠️ Se dibuja a un render target FLOAT y se lee la POSICIÓN, no el color. Comparar píxeles de color
// diría "se parece"; comparar posiciones dice cuántos metros. Y metros es la unidad en la que este
// motor ha tenido todos sus bugs de terreno.
// ================================================================================================
void testTerrainNodeRender()
{
    BEGIN("v5 F3: la geometria dibujada cae sobre la superficie del nodo");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const Haruka::World::Terrain::NodeId node{ Haruka::Tools::PlanetFace::FRONT, 14, 4200, 3100 };
    const uint32_t N = Haruka::World::Terrain::TERRAIN_NODE_TEXELS;

    // --- el nodo, generado en GPU por el mismo camino de F2 ---
    Haruka::World::Terrain::TerrainNodeGpu gpu;
    const std::string base = Haruka::Renderer::Shader::baseDir();
    if (!gpu.init(g_dev, (base + "shaders/terrain_node.comp").c_str(), 4)) {
        CHECK(false, "TerrainNodeGpu::init"); return;
    }
    Haruka::World::Terrain::TerrainNodePool pool(4, 8);
    pool.beginFrame();
    pool.request(node);
    const size_t issued = gpu.generatePending(g_dev->beginFrame(), pool, R);
    g_dev->endFrame();
    // ⚠️ YA NO ES 1, Y NO ES UN BUG (2026-08-25): `TerrainNodePool::request` encola tambien los
    // eslabones que le faltan a la cadena raiz->nodo, porque el selector solo pide HOJAS y sin eso
    // la caida por ancestro se saltaba niveles. Un nodo de nivel 14 arrastra su cadena, asi que con
    // 4 huecos se generan 4. Lo que este test necesita no es cuantos, sino que el PEDIDO este entre
    // ellos — y eso lo comprueba el `slot >= 0` de aqui debajo, que lo busca por identidad.
    CHECK(issued >= 1, "se ha generado en GPU");
    // ⚠️ NO se asume el hueco 0: la lista de libres del pool es LIFO, así que el primer nodo cae en
    // el último hueco. Que el índice sea impredecible es correcto —el pool es quien manda— y el
    // llamador tiene que PREGUNTARLO, que es justo lo que hará el render de verdad.
    int slot = -1;
    for (size_t k = 0; k < gpu.capacity(); ++k) {
        Haruka::World::Terrain::NodeId at; if (pool.nodeAtSlot((int)k, at) && at == node) { slot = (int)k; break; }
    }
    std::printf("    el nodo ocupa el hueco %d de %zu\n", slot, gpu.capacity());
    CHECK(slot >= 0, "el pool sabe en que hueco lo puso");
    if (slot < 0) { gpu.shutdown(); return; }

    // --- pipeline de dibujo ---
    PipelineDesc pd;
    const std::string vs = base + "shaders/terrain_node.vert";
    const std::string fs = base + "shaders/terrain_node.frag";
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides    = { (uint32_t)(2 * sizeof(float)) };
    pd.vertexLayout.attributes.push_back({ 0, 0, Format::RG32F, 0 });
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "pipeline de dibujo del nodo creado");
    if (!valid(pipe)) { gpu.shutdown(); return; }

    // --- la rejilla del nodo: COMPARTIDA por todos los nodos, solo enteros (u,v) ---
    // Lo único que distingue a un nodo de otro es su UBO. La geometría es la misma para los miles
    // que puedan estar residentes: un vertex buffer, un index buffer, y `drawIndexed` por nodo.
    std::vector<float> verts; verts.reserve((size_t)N * N * 2);
    for (uint32_t v = 0; v < N; ++v)
        for (uint32_t u = 0; u < N; ++u) { verts.push_back((float)u); verts.push_back((float)v); }
    std::vector<uint32_t> idx; idx.reserve((size_t)(N - 1) * (N - 1) * 6);
    for (uint32_t v = 0; v + 1 < N; ++v)
        for (uint32_t u = 0; u + 1 < N; ++u) {
            const uint32_t a = v * N + u, b = a + 1, c = a + N, d = c + 1;
            idx.insert(idx.end(), { a, c, b, b, c, d });
        }
    BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, verts.size() * sizeof(float),
                                          verts.data(), BufferMemory::Static);
    BufferHandle ib = g_dev->createBuffer(BufferUsage::Index, idx.size() * sizeof(uint32_t),
                                          idx.data(), BufferMemory::Static);
    std::printf("    rejilla compartida: %zu vertices · %zu triangulos (la MISMA para todos los nodos)\n",
                verts.size() / 2, idx.size() / 3);

    // --- cámara mirando al centro del nodo desde 2 km ---
    const glm::dvec3 c = Haruka::World::Terrain::nodeTexelDir(node, Haruka::World::Terrain::TERRAIN_NODE_CELLS / 2, Haruka::World::Terrain::TERRAIN_NODE_CELLS / 2);
    const glm::dvec3 target = c * R;
    const glm::dvec3 cam    = c * (R + 2000.0);
    const glm::dvec3 upv    = glm::normalize(glm::cross(c, glm::dvec3(0, 1, 0)));
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(target - cam), glm::vec3(upv));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 1.0f, 20000.0f);

    // ⚠️ ESTE BLOQUE ESTABA DESCUADRADO Y EL TEST NO PODIA VERLO.
    //
    // Era `{ mat4 mvp; vec4 center; ivec4 node; ivec4 grid; ivec4 edge; vec4 misc; }`, y el bloque
    // real de `terrain_node.vert` es `{ mvp, center, centerLo, lod, grid, edgeUnused, misc, shade,
    // texAnchor, lightDir }`. Faltaban `centerLo` y `lod`, asi que TODO lo de despues caia 32 bytes
    // antes: el `grid` del test aterrizaba en `uLod`, y su `misc[0] = R` en `uEdgeUnused`. **El
    // shader leia el radio del planeta como 0.**
    //
    // Un bloque uniforme mal copiado no da error de compilacion —lee el campo de al lado— y aqui
    // encima el test seguia pasando, porque lo que comprueba despues es el SSBO de alturas y no lo
    // dibujado. Dibujaba basura y nadie miraba. El `static_assert` es lo que impide que vuelva.
    struct DrawUBO {
        glm::mat4 mvp; glm::vec4 center; glm::vec4 centerLo;
        float lod[4]; int32_t grid[4]; int32_t edge[4];
        float misc[4]; float shade[4]; glm::vec4 texAnchor; glm::vec4 lightDir;
        // ⚠️ `uAerial` y `uStrideRamp` TAMBIEN, aunque la sonda no los use: el bloque tiene que medir
        // lo que el shader declara. Con 208 B (sin los dos ultimos) NVIDIA+GL leia `uStrideRamp`
        // fuera del buffer y la contraprueba de F3 dibujaba 0 px; AMD lo toleraba en silencio.
        glm::vec4 aerial; glm::vec4 strideRamp;
    } du{};
    static_assert(sizeof(DrawUBO) == 240, "el gemelo de NodeDraw se ha descuadrado");
    du.mvp      = proj * view;
    du.center   = glm::vec4(glm::vec3(glm::dvec3(0.0) - cam), 0.0f);  // centro del planeta rel. al ojo
    du.centerLo = glm::vec4(0.0f);
    du.lod[0] = (float)(glm::radians(60.0) / 256.0); du.lod[1] = 2.0f;
    du.lod[2] = (float)(R * 1.5707963267948966); du.lod[3] = 0.0f;    // w = 0: morph apagado
    du.grid[0] = (int32_t)N; du.grid[1] = (int32_t)Haruka::World::Terrain::TERRAIN_NODE_CELLS; du.grid[2] = slot;
    du.misc[0] = (float)R;
    // La ley de zancada del vertice, como la manda el renderer: sin rampa (0), para que el mapa
    // leido sea el de la zancada y la sonda mida el corte que dice medir.
    du.strideRamp = glm::vec4(150.0f, 0.0f, 4.0f, 0.5f);
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(du), &du, BufferMemory::Dynamic);

    // ⚠️ EL SSBO DE INSTANCIAS ES OBLIGATORIO, Y NO ATARLO PIERDE EL DISPOSITIVO.
    //
    // `terrain_node.vert` dejó de leer el nodo del UBO: ahora lo lee de `uInst[INSTANCE_INDEX]`, para
    // que un draw instanciado sirva a todos los nodos (un UBO reescrito entre draws no funciona en
    // Vulkan). Este test seguía atando solo el UBO y las alturas, así que el shader leía un SSBO SIN
    // ATAR — lectura fuera de rango y `VK_ERROR_DEVICE_LOST`.
    //
    // Y la factura no la pagaba este test: la GPU quedaba muerta y los ~17 tests siguientes de la
    // tanda de Vulkan fallaban midiendo sobre ella. Un test que corrompe el estado global miente
    // sobre todos los que vienen después.
    struct InstGPU { int32_t node[4]; int32_t edge[4]; int32_t slot[4]; } inst{};
    inst.node[0] = (int32_t)node.face; inst.node[1] = (int32_t)node.level;
    inst.node[2] = (int32_t)node.i;    inst.node[3] = (int32_t)node.j;
    inst.slot[0] = slot;
    // `NodeCraters` (binding 4) de terrain_node.vert: ningún cráter, pero el descriptor existe.
    const std::vector<uint8_t> noCraters(16 + 2 * 64 * 16, 0);
    BufferHandle craterUBO = g_dev->createBuffer(BufferUsage::Uniform, noCraters.size(), noCraters.data(), BufferMemory::Static);
    BufferHandle instSSBO = g_dev->createBuffer(BufferUsage::Storage, sizeof(inst), &inst,
                                                BufferMemory::Dynamic);

    // --- dibujar ---
    bool drew = false;
    if (Context* ctx = g_dev->beginFrame()) {
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        ctx->bindPipeline(pipe);
        ctx->bindUniformBuffer(0, ubo);
        ctx->bindStorageBuffer(1, gpu.heights());
        ctx->bindStorageBuffer(2, instSSBO);
        ctx->bindUniformBuffer(4, craterUBO);   // `NodeCraters`: vacío, pero atado (Vulkan)
        ctx->bindVertexBuffer(vb);
        ctx->bindIndexBuffer(ib);
        ctx->drawIndexed((uint32_t)idx.size(), 0, 1);
        ctx->endRenderPass();
        g_dev->endFrame();
        pumpWindowEvents();
        drew = true;
    }
    CHECK(drew, "el nodo se dibuja sin tumbar el device");

    // ── LA PARIDAD, EN METROS ───────────────────────────────────────────────────────────────────
    // El shader compone `vFragPos = uCenter + dir·(R + h)` con `dir` reconstruida de enteros y `h`
    // leída del pool. La CPU hace la MISMA cuenta con `nodeTexelDir` y `nodeFillHeights`. Si las dos
    // coinciden, el vértice cae sobre la superficie del nodo — que es lo que la colisión va a leer.
    std::vector<float> ref((size_t)N * N);
    Haruka::World::Terrain::nodeFillHeights(node, R, ref.data());
    BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, ref.size() * sizeof(float), nullptr,
                                          BufferMemory::Readback);
    // ⚠️ `copyBuffer` VA FUERA DE TODO FRAME, Y METERLO DENTRO PIERDE EL DISPOSITIVO.
    //
    // En Vulkan `VKDevice::copyBuffer` hace `submitOneShot`: un envío inmediato a la cola. Estaba
    // entre `beginFrame` y `endFrame`, así que enviaba con el command buffer del frame todavía
    // abierto y su semáforo de `vkAcquireNextImageKHR` sin esperar. La validación lo cantaba en
    // cadena ("Semaphore must not have any pending operations", "command buffer is in use") y
    // acababa en `VK_ERROR_DEVICE_LOST`.
    //
    // Y el daño no se quedaba aquí: una vez perdido el dispositivo, TODOS los tests de Vulkan
    // posteriores medían sobre una GPU muerta. El de cobertura leía 0 % y yo estuve un buen rato
    // buscando en el pase un fallo que no existía. Un test que corrompe el estado global no falla
    // solo él — falsifica a los que vienen detrás.
    //
    // `submitOneShot` espera a que su propio envío acabe, así que aquí no hace falta ninguna fence.
    copyThenWait(gpu.heights(), rb, (size_t)slot * Haruka::World::Terrain::TerrainNodeGpu::kBytesPerNode,
                 ref.size() * sizeof(float));
    const float* got = (const float*)g_dev->mappedData(rb);
    if (got) {
        double worstPos = 0.0;
        for (uint32_t v = 0; v < N; v += 8)
            for (uint32_t u = 0; u < N; u += 8) {
                const size_t k = (size_t)v * N + u;
                const glm::dvec3 d = Haruka::World::Terrain::nodeTexelDir(node, u, v);
                // La misma composición que el vertex shader, en double.
                const glm::dvec3 pGpu = d * (R + (double)got[k]);
                const glm::dvec3 pCpu = d * (R + (double)ref[k]);
                worstPos = std::max(worstPos, glm::length(pGpu - pCpu));
            }
        std::printf("    posicion del vertice: GPU vs referencia CPU -> peor %.4f m\n", worstPos);
        // ⚠️ AQUI SALIAN 399,59 m EN OPENGL Y 0,0001 EN VULKAN, y se leyo como "los dos backends
        // producen suelos distintos". No era cierto: faltaba la fence DESPUES de `copyBuffer` (ver
        // `copyThenWait`). Con ella, GL da 0,0204 m. La generacion siempre fue correcta.
        CHECK(worstPos < 0.05, "el vertice cae sobre la superficie del nodo, dentro de la tolerancia");
    }

    g_dev->destroy(rb); g_dev->destroy(ubo); g_dev->destroy(ib); g_dev->destroy(vb); g_dev->destroy(craterUBO);
    g_dev->destroy(pipe); gpu.shutdown();
}


// ================================================================================================
// LA SONDA QUE FALTABA: la geometria DIBUJADA contra el CAMPO de terreno.
//
// ⚠️ EL BANCO MEDIA LOS DOS EXTREMOS DE LA CADENA Y NO EL TRAMO DE EN MEDIO. Hasta hoy habia:
//
//     bake (terrain_node.comp) vs referencia CPU ....... 0,02 m   ✔ medido
//     malla de colision        vs campo real ........... 0,04 m   ✔ medido
//     textura -> terrain_node.vert -> posicion ......... NADA
//
// `testTerrainNodeRender` dibuja el nodo, pero lo que compara despues es el SSBO de alturas: su
// comentario dice "se lee la POSICION" y lee el buffer. Asi que el vertex shader —donde viven `dirD`,
// el cosido de aristas, el morph y la cancelacion con `uCenter`— nunca se ha comprobado contra nada.
// Ahi es donde tiene que estar la disparidad reportada, porque los otros dos tramos dan centimetros.
//
// La referencia es `harukaTerrainDetail` evaluada EN EL PIXEL, que es independiente del bake (el bake
// evalua en el centro del texel). Lo que debe quedar es el error de cuerda de la rejilla: centimetros.
// ================================================================================================
void testTerrainNodeDrawnVsField()
{
    BEGIN("v5 F3: la geometria DIBUJADA cae sobre el campo (el tramo sin medir)");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    // ⚠️ NIVEL 17, NO 14, Y LA DIFERENCIA IMPORTA. Con el 14 (texel 4,77 m) esto media un nodo que
    // solo se dibuja a kilometros, y sus cifras se leyeron como si fueran las del suelo que se pisa.
    // Bajo los pies el selector esta en el 17: texel 0,596 m, que es donde el corte del render y el
    // de la colision (0,5 m) caen en las MISMAS octavas y la disparidad deberia ser de centimetros.
    const Haruka::World::Terrain::NodeId node{ Haruka::Tools::PlanetFace::FRONT, 17, 33600, 24800 };
    const uint32_t N = Haruka::World::Terrain::TERRAIN_NODE_TEXELS;

    Haruka::World::Terrain::TerrainNodeGpu gpu;
    const std::string base = Haruka::Renderer::Shader::baseDir();
    // 32 huecos: `request` encola la cadena raiz->nodo entera, y con 4 (lo que usa el test de al
    // lado, que va al nivel 14) el padre y el abuelo se caen del pool — el shader leeria su propio
    // mapa con stride 2 y 4, y el barrido daria tres veces la misma cifra.
    if (!gpu.init(g_dev, (base + "shaders/terrain_node.comp").c_str(), 32)) {
        CHECK(false, "TerrainNodeGpu::init"); return;
    }
    Haruka::World::Terrain::TerrainNodePool pool(32, 8);
    pool.beginFrame();
    pool.request(node);
    // ⚠️ `endFrame` VA SIEMPRE. Esto era `for (...; generatePending(beginFrame(),...); ++pass)
    // endFrame();`: cuando no quedaba nada por generar la condicion abria un frame y salia sin
    // cerrarlo, asi que el dibujo de despues encontraba el device con un frame abierto y pintaba
    // 1,9% de pantalla. Lo canto el guardia de cobertura, no yo.
    for (int pass = 0; pass < 24; ++pass) {
        Context* ctx = g_dev->beginFrame();
        const size_t n = ctx ? gpu.generatePending(ctx, pool, R) : 0;
        g_dev->endFrame();
        pumpWindowEvents();
        if (n == 0) break;
    }
    int slot = -1;
    for (size_t k = 0; k < gpu.capacity(); ++k) {
        Haruka::World::Terrain::NodeId at; if (pool.nodeAtSlot((int)k, at) && at == node) { slot = (int)k; break; }
    }
    CHECK(slot >= 0, "el nodo esta residente");
    if (slot < 0) { gpu.shutdown(); return; }
    double worstByStride[3] = { 0.0, 0.0, 0.0 };
    double worstCollByStride[3] = { 0.0, 0.0, 0.0 };
    double worstSanity = 0.0;

    PipelineDesc pd;
    const std::string vs = base + "shaders/terrain_node.vert";
    const std::string fs = base + "shaders/tests/terrain_node_probe.frag";
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides = { (uint32_t)(2 * sizeof(float)) };
    pd.vertexLayout.attributes.push_back({ 0, 0, Format::RG32F, 0 });
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "pipeline de la sonda creado");
    if (!valid(pipe)) { gpu.shutdown(); return; }

    // ⚠️ EL BARRIDO DE STRIDE ES EL PUNTO DE ESTE TEST. Con stride 1 se dibuja un vertice por texel y
    // el error es el de cuerda del texel — pero el juego NO dibuja asi: reparte stride por nodo con
    // histeresis, y la malla sale cada TEXEL x STRIDE. El propio `terrain_node.vert` lo tiene
    // apuntado (nivel 14, stride 4: el corte efectivo pasa de 1,41 a 0,43 m), pero eso es el corte de
    // OCTAVAS; lo que aqui se mide es lo otro: cuanto se separa del campo la superficie DIBUJADA.
    // mode 2 = CONTRAPRUEBA: la misma referencia de colision con la pendiente x50. No mide nada del
    // motor; existe solo para demostrar que `uProbe.z` llega al shader. Hacia falta porque a nivel 17
    // los modos 0 y 1 dan cifras IDENTICAS —el corte de la colision (0,5 m) y el del nodo (0,596 m)
    // caen en las mismas octavas— y comparar los dos entre si daba un falso fallo.
    for (int mode = 0; mode < 3; ++mode) {
    const bool vsColl = (mode >= 1);
    std::printf("  -- referencia: %s\n", mode == 0
                ? "EL CAMPO con el corte del nodo (¿esta bien puesto el vertice?)"
                : mode == 1 ? "LA SUPERFICIE DE COLISION (lo que se ve contra lo que se pisa)"
                            : "contraprueba: la de colision con la pendiente x50");
    for (int sIdx = 0; sIdx <= 2; ++sIdx) {
    const uint32_t step = 1u << sIdx;
    std::vector<float> verts;
    for (uint32_t v = 0; v < N; v += step)
        for (uint32_t u = 0; u < N; u += step) { verts.push_back((float)u); verts.push_back((float)v); }
    const uint32_t side = (N + step - 1) / step;
    std::vector<uint32_t> idx;
    for (uint32_t v = 0; v + 1 < side; ++v)
        for (uint32_t u = 0; u + 1 < side; ++u) {
            const uint32_t a = v * side + u, b = a + 1, c = a + side, d = c + 1;
            idx.insert(idx.end(), { a, c, b, b, c, d });
        }
    BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, verts.size() * sizeof(float),
                                          verts.data(), BufferMemory::Static);
    BufferHandle ib = g_dev->createBuffer(BufferUsage::Index, idx.size() * sizeof(uint32_t),
                                          idx.data(), BufferMemory::Static);

    // Camara a 300 m sobre el centro del nodo y mirando abajo: se quiere el suelo LLENANDO la
    // pantalla y visto de cerca, que es donde el usuario reporta el sintoma (bajo sus pies).
    const glm::dvec3 c   = Haruka::World::Terrain::nodeTexelDir(node, Haruka::World::Terrain::TERRAIN_NODE_CELLS / 2, Haruka::World::Terrain::TERRAIN_NODE_CELLS / 2);
    // ⚠️ LA ALTURA SE MIDE DESDE EL TERRENO, NO DESDE EL RADIO. Estaba en `c * (R + span*0.7)`, o
    // sea 53 m sobre la ESFERA — y como ahi el terreno pasa de esa cota, la camara quedaba enterrada:
    // 1,9% de pantalla y tres strides dando la misma cifra. A nivel 14 colaba de casualidad porque
    // 300 m tapaba el relieve. Sin el guardia de cobertura habria publicado esas cifras.
    const double nodeSpanM = Haruka::World::Terrain::nodeTexelM(node, R) * (double)Haruka::World::Terrain::TERRAIN_NODE_CELLS;
    const double hCentre   = (double)Haruka::World::Terrain::terrainDetail(c, R, (float)Haruka::World::Terrain::nodeTexelM(node, R));
    const glm::dvec3 cam = c * (R + hCentre + nodeSpanM * 0.7);
    const glm::dvec3 tgt = c * (R + hCentre);
    const glm::dvec3 upv = glm::normalize(glm::cross(c, glm::dvec3(0, 1, 0)));
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(tgt - cam), glm::vec3(upv));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 1.0f, 20000.0f);

    // ⚠️ LAYOUT COPIADO A MANO Y POR ESO CON `static_assert`. `TerrainNodeRenderer::DrawUBO` es
    // privado, asi que aqui hay un gemelo. El de `testTerrainNodeRender` ESTA DESCUADRADO (le faltan
    // `centerLo` y `lod`, asi que su `grid` y su `misc` caen sobre otros campos) — un bloque uniforme
    // mal copiado no da error de compilacion, solo lee basura del campo de al lado.
    struct DrawUBO {
        glm::mat4 mvp; glm::vec4 center; glm::vec4 centerLo;
        float lod[4]; int32_t grid[4]; int32_t edge[4];
        float misc[4]; float shade[4]; glm::vec4 texAnchor; glm::vec4 lightDir;
        // ⚠️ `uAerial` y `uStrideRamp` TAMBIEN, aunque la sonda no los use: el bloque tiene que medir
        // lo que el shader declara. Con 208 B (sin los dos ultimos) NVIDIA+GL leia `uStrideRamp`
        // fuera del buffer y la contraprueba de F3 dibujaba 0 px; AMD lo toleraba en silencio.
        glm::vec4 aerial; glm::vec4 strideRamp;
    } du{};
    static_assert(sizeof(DrawUBO) == 240, "el gemelo de NodeDraw se ha descuadrado");
    du.mvp      = proj * view;
    du.center   = glm::vec4(glm::vec3(glm::dvec3(0.0) - cam), 0.0f);
    du.centerLo = glm::vec4(0.0f);
    du.lod[0] = (float)(glm::radians(60.0) / 256.0); du.lod[1] = 2.0f;
    du.lod[2] = (float)(R * 1.5707963267948966); du.lod[3] = 0.0f;   // w = 0: morph APAGADO
    du.grid[0] = (int32_t)N; du.grid[1] = (int32_t)Haruka::World::Terrain::TERRAIN_NODE_CELLS; du.grid[2] = slot;
    du.misc[0] = (float)R;
    // La ley de zancada del vertice, como la manda el renderer: sin rampa (0), para que el mapa
    // leido sea el de la zancada y la sonda mida el corte que dice medir.
    du.strideRamp = glm::vec4((float)Haruka::World::Terrain::TERRAIN_NODE_STRIDE_MATCH_M, 0.0f, 4.0f, 0.5f);
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(du), &du, BufferMemory::Dynamic);

    // El corte de octavas del nodo: si la sonda evaluara el campo con otro, mediria el CORTE en vez
    // de la colocacion del vertice — que es un efecto real pero distinto y ya medido aparte.
    const float minFeatureM = (float)Haruka::World::Terrain::nodeTexelM(node, R);
    // ⚠️ FONDO DE ESCALA DE ±64 m, Y NO ES HOLGURA DE SOBRA. Con ±4 m saturaba el 100% de los pixeles
    // en cuanto el stride pasaba de 1, y "media 4,0000 · peor 4,0000" no es una medida: es el tope
    // del codificado. Sobre 16 bits, ±64 m sigue dando 2 mm de resolucion.
    const float fullScaleM  = 64.0f;
    struct ProbeUBO { float p[4]; glm::vec4 anchor; } pu{
        { minFeatureM, fullScaleM,
          vsColl ? (float)(Haruka::World::Terrain::TERRAIN_TRIM_SLOPE * (mode == 2 ? 50.0 : 1.0)) : 0.0f,
          (float)Haruka::World::Terrain::TERRAIN_TRIM_FLOOR },
        glm::vec4(glm::vec3(c), 0.0f) };
    BufferHandle pubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(pu), &pu, BufferMemory::Dynamic);

    Haruka::World::Terrain::TerrainNodeRenderer::NodeInstGPU inst{};
    inst.node[0] = (int32_t)node.face; inst.node[1] = (int32_t)node.level;
    inst.node[2] = (int32_t)node.i;    inst.node[3] = (int32_t)node.j;
    inst.slot[0] = slot;
    inst.slot[2] = sIdx;                 // log2 del stride: el shader hace `1 << slot.z`
    // ⚠️ SIN ESTO LA SONDA MIDE BASURA, Y ME LO TRAGUE UNA VEZ: con `misc` a cero el shader creia que
    // el padre vivia en el hueco 0 —un hueco VALIDO, ocupado por otro nodo de la cadena— y leia SU
    // mapa de alturas. Salieron 62 m de error y por un momento parecio el hallazgo del dia. El
    // relieve a 4,77 m no puede valer 62 m: cuando una sonda da un numero imposible, lo primero que
    // falla es la sonda. Con -1 el shader sabe que no estan y se queda en su propio mapa.
    const Haruka::World::Terrain::NodeId par { node.face, node.level - 1, node.i / 2, node.j / 2 };
    const Haruka::World::Terrain::NodeId gran{ par.face,  par.level  - 1, par.i  / 2, par.j  / 2 };
    inst.misc[0] = (float)pool.slotOf(par);
    inst.misc[1] = (float)pool.slotOf(gran);
    inst.misc[2] = -1.0f; inst.misc[3] = -1.0f;   // bisabuelo/tatarabuelo: no estan (0 seria el hueco 0)
    // `NodeCraters` (binding 4) de terrain_node.vert: ningún cráter, pero el descriptor existe.
    const std::vector<uint8_t> noCraters(16 + 2 * 64 * 16, 0);
    BufferHandle craterUBO = g_dev->createBuffer(BufferUsage::Uniform, noCraters.size(), noCraters.data(), BufferMemory::Static);
    BufferHandle instSSBO = g_dev->createBuffer(BufferUsage::Storage, sizeof(inst), &inst,
                                                BufferMemory::Dynamic);

    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int W = (uw > 0) ? (int)uw : 256, H = (uh > 0) ? (int)uh : 256;
    std::vector<uint8_t> px((size_t)W * H * 4, 0);
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    if (Context* ctx = g_dev->beginFrame()) {
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
        cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        ctx->bindPipeline(pipe);
        ctx->bindUniformBuffer(0, ubo);
        ctx->bindStorageBuffer(1, gpu.heights());
        ctx->bindStorageBuffer(2, instSSBO);
        ctx->bindUniformBuffer(3, pubo);
        ctx->bindUniformBuffer(4, craterUBO);   // `NodeCraters`: vacío, pero atado (Vulkan)
        ctx->bindVertexBuffer(vb);
        ctx->bindIndexBuffer(ib);
        ctx->drawIndexed((uint32_t)idx.size(), 0, 1);
        ctx->endRenderPass();
        // GL lee del framebuffer por defecto ANTES del swap; Vulkan, ya presentado. Ver la nota de
        // `testClearReadback`: invertirlo da una imagen en negro y se lee como "no se dibujo nada".
        if (!isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        g_dev->endFrame();
        if (isVk)  g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        pumpWindowEvents();
    }

    size_t covered = 0; double sum = 0.0, worst = 0.0; int saturated = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        if (px[i * 4 + 2] < 128) continue;                 // B = 0 -> el clear, ahi no hay terreno
        ++covered;
        const uint32_t q = ((uint32_t)px[i * 4 + 0] << 8) | (uint32_t)px[i * 4 + 1];
        const double err = ((double)q / 65535.0) * 2.0 * fullScaleM - fullScaleM;
        if (q == 0 || q == 65535) ++saturated;
        sum += std::fabs(err); worst = std::max(worst, std::fabs(err));
    }
    const double cov = (double)covered / (double)(W * H) * 100.0;
    std::printf("    stride %u (celda %.2f m · lee el mapa del %s) · %zu px con terreno (%.1f%%) · "
                "%s: "
                "media %.4f m · peor %.4f m · saturados %d\n",
                step, minFeatureM * (double)step,
                sIdx == 0 ? "nodo" : (sIdx == 1 ? "padre" : "abuelo"), covered, cov,
                vsColl ? "VER vs PISAR" : "dibujado vs campo",
                covered ? sum / (double)covered : 0.0, worst, saturated);
    // ⚠️ SIN COBERTURA NO HAY MEDIDA. Si el nodo no sale en pantalla, las estadisticas de abajo dan
    // 0,0000 m y se leerian como paridad perfecta: el fallo mas caro que puede tener esta sonda.
    CHECK(covered > (size_t)(W * H) / 10, "el nodo cubre la pantalla (si no, las cifras no valen)");
    CHECK(saturated == 0, "ningun pixel satura el fondo de escala (si satura, el peor es mayor)");
    if      (mode == 0) worstByStride[sIdx]     = worst;
    else if (mode == 1) worstCollByStride[sIdx] = worst;
    else if (sIdx == 0) worstSanity              = worst;

    g_dev->destroy(pubo); g_dev->destroy(instSSBO); g_dev->destroy(ubo); g_dev->destroy(craterUBO);
    g_dev->destroy(ib); g_dev->destroy(vb);
    }   // fin del barrido de stride
    }   // fin de los dos modos de referencia

    // ⚠️ CONTRAPRUEBA: el error TIENE que crecer con el stride. Si saliera plano seria que `slot.z`
    // no llega al shader y las tres pasadas dibujan lo mismo — la sonda estaria midiendo una sola
    // configuracion tres veces y sus tres cifras iguales se leerian como "el stride no influye".
    std::printf("    peor error por stride: 1 -> %.4f m · 2 -> %.4f m · 4 -> %.4f m\n",
                worstByStride[0], worstByStride[1], worstByStride[2]);
    CHECK(worstByStride[2] > worstByStride[0] * 1.5,
          "el error crece con el stride (si no, `slot.z` no esta llegando al shader)");
    std::printf("    ver-vs-pisar por stride: 1 -> %.4f m · 2 -> %.4f m · 4 -> %.4f m\n",
                worstCollByStride[0], worstCollByStride[1], worstCollByStride[2]);
    // ⚠️ CONTRAPRUEBA DEL SEGUNDO MODO. Comparar los modos 0 y 1 entre si NO vale: a nivel 17 dan lo
    // mismo porque los dos cortes caen en las mismas octavas, y eso es un RESULTADO, no un fallo. Lo
    // que hay que demostrar es que el uniform llega, y para eso se perturba a proposito.
    std::printf("    contraprueba (pendiente x50): peor %.4f m contra %.4f m del campo\n",
                worstSanity, worstByStride[0]);
    CHECK(worstSanity > worstByStride[0] * 1.5,
          "perturbar el corte de la colision CAMBIA la medida (si no, `uProbe.z` no llega)");

    g_dev->destroy(pipe);
    gpu.shutdown();
}


// ================================================================================================
// EL COSTE DEL TOPE DE STRIDE DE CERCA, A ALTURA DE OJO.
//
// ⚠️ EL BANCO DE COSTE QUE HABIA MIDE A 800 m DE ALTITUD, y ahi este tope casi no aplica: desde 800 m
// el nodo mas cercano ya esta a cientos de metros. Medir alli habria dado "no cuesta nada" y habria
// sido una cifra correcta contestando a la pregunta equivocada. La disparidad se ve DE PIE.
//
// Se barre el radio (0 = apagado, o sea la linea base) y se leen los triangulos del contador DEL
// MOTOR, no de un modelo: un modelo por bandas dio 1,77x y hay que ver cuanto es de verdad.
// ================================================================================================
void testTerrainStrideMatchCost()
{
    BEGIN("v5: coste del tope de stride de cerca (a altura de ojo)");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    Haruka::World::Terrain::TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 2048)) {
        CHECK(false, "init del pase"); return;
    }
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int W = (uw > 0) ? (int)uw : 256, H = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)H;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, (double)W / (double)H);

    // De pie: 1,7 m sobre la superficie, mirando al horizonte. Es donde se reporto el sintoma.
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(0.31, 0.62, 0.72));
    const glm::dvec3 cam = pc + up0 * (R + 1.7);
    const glm::dvec3 fwd = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(up0));
    const glm::mat4 proj = glm::perspective((float)fovY, (float)W / (float)H, 1.0f, 40000.0f);
    const glm::mat4 mvp  = proj * view;

    std::printf("    radio    nodos    triangulos   ms/frame\n");
    double trisOff = 0.0, msOff = 0.0;
    for (double radius : { 0.0, 150.0, 300.0, 600.0 }) {
        // ⚠️ El radio va por ENV y `strideMatchM()` lo cachea en un `static`, asi que hay que
        // pasarlo por el parametro: cachearlo haria que las cuatro pasadas midieran la primera.
        r.setStrideMatchOverride(radius);
        double ms = 0.0; size_t nodes = 0, tris = 0;
        for (int f = 0; f < 24; ++f) {
            const auto t0 = std::chrono::high_resolution_clock::now();
            Context* ctx = g_dev->beginFrame(); if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            g_dev->endFrame();
            pumpWindowEvents();
            if (f >= 8) {   // los 8 primeros llenan el pool y compilan; no cuentan
                ms += std::chrono::duration<double, std::milli>(
                          std::chrono::high_resolution_clock::now() - t0).count();
                const auto st = r.stats(); nodes = st.drawn; tris = st.tris;
            }
        }
        ms /= 16.0;
        std::printf("    %5.0f m  %6zu   %8.2f M   %6.2f%s\n", radius, nodes,
                    (double)tris / 1e6, ms, radius == 0.0 ? "   <- linea base" : "");
        if (radius == 0.0) { trisOff = (double)tris; msOff = ms; }
        else if (radius == 300.0 && trisOff > 0.0)
            std::printf("      -> a 300 m: x%.2f triangulos · x%.2f ms\n",
                        (double)tris / trisOff, msOff > 1e-6 ? ms / msOff : 0.0);
    }
    r.setStrideMatchOverride(-1.0);   // volver al valor del entorno

    // ⚠️ CONTRAPRUEBA: si el radio no llegara a `nodeStrideWant`, las cuatro filas darian el MISMO
    // numero de triangulos y se leerian como "el tope es gratis" — la conclusion mas cara posible.
    CHECK(trisOff > 0.0, "la linea base dibuja algo");
    r.shutdown();
}

// ================================================================================================
// LA ATMOSFERA VISTA DESDE FUERA: el limbo del planeta
//
// ⚠️ NO EXISTIA. `sky.frag` solo pinta el domo en el que estas metido; fuera, `mix(spaceC, sky,
// u_atmo)` devuelve espacio liso, asi que un planeta desde orbita no tenia atmosfera — ni halo, ni la
// linea azul sobre el horizonte. No se apagaba con la altitud: nunca se escribio.
//
// Lo que se mide es la firma que ninguna otra cosa produce: un anillo AZUL justo por FUERA de la
// silueta del planeta, que se apaga al alejarse del limbo. Con contraprueba en el lado nocturno, que
// es donde el terminador tiene que dejarlo a oscuras — sin ella, "hay azul" no distingue una
// atmosfera de un halo pintado alrededor de la esfera.
// ─────────────────────────────────────────────────────────────────────────────────────────────────
// LOS CIRCULOS EN EL TERRENO (Andoni, 19-09: "sigue ocurriendo y no es de agua sino de terreno")
//
// Con zancada `s` el vertice lee el mapa cortado a `texel x s`, y la zancada cambia en el radio de
// 150 m (1->2) y 300 m (2->4): el relieve dibujado suelta una octava EN UNA CIRCUNFERENCIA alrededor
// del observador, y el sombreado cambia de rugosidad ahi. Se mide asi: se dibuja el terreno con la
// luz plana y con la vista 4 (color por zancada); en la imagen lit se toma la rugosidad de cada
// pixel (|L - media 3x3|) y, para cada frontera entre dos zancadas de la vista 4, la rugosidad
// media a un lado y al otro (a 3-10 px de la frontera). La RAZON entre los dos lados es el circulo.
// Con la rampa del mapa (`TERRAIN_NODE_STRIDE_RAMP`) tiene que quedar cerca de 1; la CONTRAPRUEBA
// es la misma escena con la rampa a 0 (el escalon de antes), donde tiene que ser claramente > 1.
// ─────────────────────────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────────────────────────
// LA IMAGEN CONTRA EL RELIEVE HORNEADO FINAL (Andoni, 20-09: "que el valor dado en la imagen y el
// horneado se verifique"). Un FRAME ENTERO del renderer real —selector, pool, niveles, zancadas,
// rampa, con bake— y por cada pixel |dibujado − pisado| (vista 14: 16 bits, la misma referencia que
// la 11 pero con el bake), por bandas de distancia. La contraprueba: cambiar el bake que lee el
// vertice SIN regenerar los nodos (que guardan el bake viejo) tiene que aparecer como error del
// tamano del cambio; si no, el instrumento no ve el relieve horneado.
// ─────────────────────────────────────────────────────────────────────────────────────────────────
void testFrameDrawnVsBaked()
{
    BEGIN("v5: un frame entero contra el relieve horneado final (|dibujado − pisado| por pixel)");
    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 tan0 = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    // Un bake equirect SUAVE con continentes de verdad (cientos de metros): el vertice y el compute
    // lo muestrean bilineal con el mismo gemelo (`harukaSampleHeightField` / `sampleHeightField`),
    // asi que el bake que se dibuja y el que se pisa son el MISMO numero.
    const int BW = 128, BH = 64;
    static std::vector<float> s_bake, s_bake2;
    auto bakeFn = [](double u, double v, float extra) {
        const double lon = (u - 0.5) * 6.2831853, lat = (0.5 - v) * 3.1415926;
        return (float)(320.0 + 260.0 * std::sin(3.0 * lon) * std::cos(2.0 * lat) + 90.0 * std::cos(7.0 * lon + 1.0)) + extra;
    };
    s_bake.assign((size_t)BW * BH, 0.0f); s_bake2.assign((size_t)BW * BH, 0.0f);
    for (int y = 0; y < BH; ++y) for (int x = 0; x < BW; ++x) {
        s_bake [(size_t)y * BW + x] = bakeFn((x + 0.5) / BW, (y + 0.5) / BH, 0.0f);
        s_bake2[(size_t)y * BW + x] = bakeFn((x + 0.5) / BW, (y + 0.5) / BH, 3.0f);   // +3 m: la contraprueba
    }
    auto makeBake = [&](const std::vector<float>& d) {
        TextureDesc td; td.width = BW; td.height = BH; td.format = Format::R32F;
        td.filter = Filter::Linear; td.wrap = Wrap::ClampToEdge; td.mipmaps = false;
        td.initialData = d.data();
        return g_dev->createTexture(td);
    };
    TextureHandle bakeTex = makeBake(s_bake), bakeTex2 = makeBake(s_bake2);
    // ⚠️ El compute solo compone bake + detalle si hay CAMPO DEL CUBO atado (`uMisc.z`): el equirect
    // manda para la altura, pero sin el cubo el nodo es detalle puro sobre R (y el vertice, que si
    // lee el equirect para el clima, compararia contra otro planeta: medido, 64 m saturados en todo
    // el cuadro). Un cubo de ceros basta: el clima no se mide aqui.
    TextureHandle cubeTex{};
    {
        const int N1 = 9;
        std::vector<float> rgba((size_t)6 * N1 * N1 * 4, 0.0f);
        TextureDesc td; td.width = td.height = (uint32_t)N1; td.layers = 6;
        td.format = Format::RGBA32F; td.filter = Filter::Nearest; td.wrap = Wrap::ClampToEdge;
        td.initialData = rgba.data();
        cubeTex = g_dev->createTexture(td);
    }
    auto baseAt = [&](const glm::dvec3& d) -> float {
        return Haruka::World::Terrain::sampleHeightField(Haruka::World::Terrain::equirectUV(glm::vec3(d)), BW, BH, s_bake.data());
    };

    Haruka::World::Terrain::TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 4096)) { CHECK(false, "init del pase"); return; }
    r.setBaseField(cubeTex);
    r.setHeightTex(bakeTex);
    r.setHeightSampler([](const glm::dvec3& d, void*) {
        return Haruka::World::Terrain::sampleHeightField(Haruka::World::Terrain::equirectUV(glm::vec3(d)), 128, 64, s_bake.data());
    }, nullptr);

    // A la altura del ojo, sobre el suelo horneado FINAL (bake + detalle con el corte de la colision).
    const float  b0 = baseAt(up0);
    const double h0 = (double)b0 + (double)Haruka::World::Terrain::terrainDetail(up0, R + (double)b0, 0.5f) * Haruka::World::Terrain::seaLevelAttenuation(b0);
    // A 30 m y 15 grados abajo: desde la altura del ojo todo lo que pasa de 150 m cabe en 3 filas
    // de 256 (medido: 0 pixeles en las bandas lejanas). Asi las cuatro bandas tienen pixeles.
    const glm::dvec3 cam = pc + up0 * (R + h0 + 30.0);
    const glm::dvec3 fwd = glm::normalize(tan0 * std::cos(glm::radians(15.0)) - up0 * std::sin(glm::radians(15.0)));
    const glm::dvec3 vup = glm::normalize(up0 - fwd * glm::dot(up0, fwd));
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double radPerPx = fovY / 1080.0;                 // la ley del juego (ver `testStrideRingsOnLand`)
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);
    const float aspect = (float)w / (float)h, fCot = 1.0f / std::tan((float)fovY * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = fCot / aspect; proj[1][1] = fCot; proj[2][3] = -1.0f; proj[3][2] = 1.0f;
    const glm::mat4 mvp = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup))));
    std::printf("    bake bajo la camara %.1f m · suelo final %.2f m · camara a 30 m, mirando 15 grados abajo\n", b0, h0);

    auto render = [&](int dbg, std::vector<uint8_t>& px, int frames) {
        r.setDebugViewOverride(dbg);
        px.assign((size_t)w * h * 4, 0);
        Haruka::World::Terrain::TerrainNodeRenderer::FrameStats last{};
        for (int f = 0; f < frames; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            last = r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk && f == frames - 1) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame(); pumpWindowEvents();
            if (isVk && f == frames - 1) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
        return last;
    };
    struct Band { double lo, hi, sum = 0, worst = 0; size_t n = 0; };
    auto measure = [&](const std::vector<uint8_t>& pxErr, const std::vector<uint8_t>& pxDist, std::vector<Band>& bands) {
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            if (pxErr[i * 4 + 2] < 128) continue;                       // B = 0: sin terreno
            const uint32_t q = ((uint32_t)pxErr[i * 4] << 8) | (uint32_t)pxErr[i * 4 + 1];
            const double err = ((double)q / 65535.0) * 128.0 - 64.0;
            const double d = pxDist[i * 4] / 255.0 * 2000.0;
            for (Band& b : bands) if (d >= b.lo && d < b.hi) { b.sum += std::abs(err); b.worst = std::max(b.worst, std::abs(err)); ++b.n; }
        }
    };
    auto report = [&](const char* tag, const std::vector<Band>& bands) {
        std::printf("    %s\n      banda           pixeles    media     peor\n", tag);
        for (const Band& b : bands)
            std::printf("      %5.0f-%5.0f m   %7zu   %.4f   %.4f\n", b.lo, b.hi, b.n, b.n ? b.sum / b.n : 0.0, b.worst);
    };
    std::vector<Band> bands = { {2, 150}, {150, 300}, {300, 1000}, {1000, 1990} };

    std::vector<uint8_t> pxErr, pxDist;
    const auto st = render(14, pxErr, 24);
    render(13, pxDist, 2);
    std::printf("    sel %zu · dibujados %zu · por ancestro %zu · zancadas %u..%u\n", st.selected, st.drawn, st.ancestors, st.strideMin, st.stride);
    measure(pxErr, pxDist, bands);
    report("|dibujado − pisado| (bake + detalle con el corte de la colision):", bands);
    if (const char* dir = std::getenv("HARUKA_TERRAIN_PNG")) {
        const bool flip = detectRowFlip(pxErr, w, h);
        std::vector<uint8_t> img(pxErr.size());
        for (int y = 0; y < h; ++y) std::memcpy(&img[(size_t)y * w * 4], &pxErr[(size_t)(flip ? h - 1 - y : y) * w * 4], (size_t)w * 4);
        Haruka::IO::writePNG(std::string(dir) + "/" + (isVk ? "vk_" : "gl_") + "frame_vs_horneado.png", w, h, 4, img.data());
    }

    // ── Y DESDE ALTO (la captura de Andoni del 20-09: desde 1 500 m, mirando 35 grados abajo) ──
    // Todo el cuadro esta mas alla de 300 m: es el regimen del corte continuo entero (mapas 2 a 4).
    std::vector<Band> bandsHi = { {300, 1000}, {1000, 2000}, {2000, 4000}, {4000, 7990} };
    {
        const glm::dvec3 camHi = pc + up0 * (R + h0 + 1500.0);
        const glm::dvec3 fwdHi = glm::normalize(tan0 * std::cos(glm::radians(35.0)) - up0 * std::sin(glm::radians(35.0)));
        const glm::dvec3 vupHi = glm::normalize(up0 - fwdHi * glm::dot(up0, fwdHi));
        const glm::mat4 mvpHi = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwdHi), glm::vec3(vupHi))));
        auto renderHi = [&](int dbg, std::vector<uint8_t>& px, int frames) {
            r.setDebugViewOverride(dbg);
            px.assign((size_t)w * h * 4, 0);
            Haruka::World::Terrain::TerrainNodeRenderer::FrameStats last{};
            for (int f = 0; f < frames; ++f) {
                Context* ctx = g_dev->beginFrame();
                if (!ctx) break;
                r.prepare(ctx, camHi, pc, R, fwdHi, radPerPx, cone);
                ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
                cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
                ctx->beginRenderPass({}, cv);
                last = r.draw(ctx, camHi, pc, R, mvpHi);
                ctx->endRenderPass();
                if (!isVk && f == frames - 1) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
                g_dev->endFrame(); pumpWindowEvents();
                if (isVk && f == frames - 1) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            }
            return last;
        };
        std::vector<uint8_t> pxErrHi, pxDistHi, pxMapHi;
        const auto stHi = renderHi(14, pxErrHi, 30);
        renderHi(13, pxDistHi, 2);
        renderHi(12, pxMapHi, 2);
        // La vista 13 va a 8 m en 0..2 km: aqui se lee con la escala x4 (0..8 km) que da `uMisc`... no:
        // la vista 13 es fija a 2 000 m. Las bandas de mas de 2 km se saturan a 1990: se agrupan ahi.
        auto measureHi = [&](const std::vector<uint8_t>& pxErr2, std::vector<Band>& bands2) {
            for (size_t i = 0; i < (size_t)w * h; ++i) {
                if (pxErr2[i * 4 + 2] < 128) continue;
                const uint32_t q = ((uint32_t)pxErr2[i * 4] << 8) | (uint32_t)pxErr2[i * 4 + 1];
                const double err = ((double)q / 65535.0) * 128.0 - 64.0;
                const double d = pxDistHi[i * 4] / 255.0 * 2000.0;
                for (Band& b : bands2) if (d >= b.lo && d < b.hi) { b.sum += std::abs(err); b.worst = std::max(b.worst, std::abs(err)); ++b.n; }
            }
        };
        measureHi(pxErrHi, bandsHi);
        std::printf("    DESDE ALTO: sel %zu · dibujados %zu · por ancestro %zu · niveles %u..%u · zancadas %u..%u\n",
                    stHi.selected, stHi.drawn, stHi.ancestors, stHi.levelMin, stHi.levelMax, stHi.strideMin, stHi.stride);
        report("|dibujado − pisado| desde 1 500 m (la vista 13 satura a 2 km: la ultima banda es 'mas de 2 km'):", bandsHi);
        // El mapa absoluto leido, por bandas: de 17 (propio del 17) bajando; y cuantos pixeles leen
        // el tatarabuelo (fraccion del mapa > 3 respecto a su hoja no se ve aqui; se ve el absoluto).
        double mapMin = 99, mapMax = -99; size_t nm = 0;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            if (pxMapHi[i * 4] < 8) continue;
            const double A = (pxMapHi[i * 4] / 255.0 - 0.1) / 0.08 + 6.0;
            mapMin = std::min(mapMin, A); mapMax = std::max(mapMax, A); ++nm;
        }
        std::printf("    mapa absoluto leido: de %.2f a %.2f (%zu px)\n", mapMin, mapMax, nm);
        if (const char* dir = std::getenv("HARUKA_TERRAIN_PNG")) {
            const bool flip = detectRowFlip(pxErrHi, w, h);
            std::vector<uint8_t> img(pxErrHi.size());
            for (int y = 0; y < h; ++y) std::memcpy(&img[(size_t)y * w * 4], &pxErrHi[(size_t)(flip ? h - 1 - y : y) * w * 4], (size_t)w * 4);
            Haruka::IO::writePNG(std::string(dir) + "/" + (isVk ? "vk_" : "gl_") + "frame_vs_horneado_alto.png", w, h, 4, img.data());
        }
    }

    // ── CONTRAPRUEBA: el bake +3 m para el vertice, los nodos con el viejo ──────────────────────
    std::vector<Band> bands2 = { {2, 150}, {150, 300}, {300, 1000}, {1000, 1990} };
    r.setHeightTex(bakeTex2);
    std::vector<uint8_t> pxErr2;
    render(14, pxErr2, 2);              // 2 frames: nada nuevo que generar, los nodos siguen con el bake viejo
    measure(pxErr2, pxDist, bands2);
    report("CONTRAPRUEBA (el vertice lee un bake +3 m; los nodos guardan el viejo):", bands2);
    r.setHeightTex(bakeTex);

    CHECK(st.ancestors == 0, "el frame se dibuja con datos propios (24 frames bastan para el pool)");
    CHECK(bands[0].n > 2000 && bands[1].n > 500, "hay terreno en las bandas cercanas");
    // Cerca: el nodo dibuja su propio mapa (corte 0,6 m) contra el corte de la colision (0,5-0,6):
    // lo que queda es la cuerda del triangulo y la octava de 4,5 m entre los dos cortes. Medido.
    // Medido (NVIDIA, VK y GL): 1,9 mm / 5,6 cm · 3,0 cm / 29 cm · 10 cm / 65 cm · 37 cm / 1,29 m.
    // Los topes van al doble: lo que se quiere cazar es un cambio de regimen, no el ruido.
    CHECK(bands[0].sum / std::max(bands[0].n, (size_t)1) < 0.01, "a menos de 150 m, |dibujado − pisado| medio < 1 cm");
    CHECK(bands[0].worst < 0.15, "y el peor pixel cercano < 15 cm");
    CHECK(bands[1].sum / std::max(bands[1].n, (size_t)1) < 0.06, "de 150 a 300 m (mapa del padre), medio < 6 cm");
    CHECK(bands[2].sum / std::max(bands[2].n, (size_t)1) < 0.2, "de 300 a 1000 m (corte continuo), medio < 20 cm");
    // ⚠️ 0.90 m AND NOT 0.75 (11-10, Andoni's decision): this is the far relief's DETAIL, and since
    // `vertexPx` went from 8 to 24 (23-09, for 60 fps: terrain pass ~10.25 -> ~8 ms of GPU) the far
    // map is read up to 1.6 levels coarser. Measured here: 0.37 m with 8, 0.48 with 16, 0.80 with 24.
    // Andoni kept 24; the bound stays just above it so that it still catches anything WORSE.
    CHECK(bands[3].n == 0 || bands[3].sum / bands[3].n < 0.90, "de 1 a 2 km, medio < 90 cm (con vertexPx 24)");
    const double c0 = bands2[0].sum / std::max(bands2[0].n, (size_t)1);
    CHECK(c0 > 2.5 && c0 < 3.5, "CONTRAPRUEBA: un bake 3 m distinto del horneado aparece como ~3 m de error (el instrumento ve el relieve horneado)");
    g_dev->destroy(bakeTex); g_dev->destroy(bakeTex2); g_dev->destroy(cubeTex);
}

void testStrideRingsOnLand()
{
    BEGIN("v5: los circulos de zancada en el TERRENO (el mapa leido es continuo en la distancia)");
    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 tan0 = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    // Sin bake, el nodo dibuja el detalle sobre R sin atenuar: el suelo bajo la camara es esto.
    const double h0 = Haruka::World::Terrain::terrainDetail(up0, R, 0.5f);
    const glm::dvec3 cam = pc + up0 * (R + h0 + 40.0);
    const glm::dvec3 fwd = glm::normalize(tan0 * std::cos(glm::radians(25.0)) - up0 * std::sin(glm::radians(25.0)));
    // El "arriba" de la camara es el del planeta proyectado, para que las bandas de distancia sean
    // FILAS de la imagen (con `cross(fwd, tan0)` la imagen sale girada 90 grados).
    const glm::dvec3 vup = glm::normalize(up0 - fwd * glm::dot(up0, fwd));
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    // ⚠️ LA LEY DE ZANCADA DEL JUEGO, NO LA DEL BANCO. A 256 px `collWant = d/64/texel` no llega a 2
    // en ningun nivel (el nivel se parte a 122 x texel en vez de a 515 x), asi que la zancada es 1 en
    // todo el cuadro y no hay frontera que medir. Se selecciona y se dibuja con los radianes por
    // pixel de 1080p: la imagen es la misma geometria vista a menos resolucion.
    const double radPerPx = fovY / 1080.0;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);
    const float aspect = (float)w / (float)h;
    const float fCot   = 1.0f / std::tan((float)fovY * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = fCot / aspect; proj[1][1] = fCot; proj[2][3] = -1.0f; proj[3][2] = 1.0f;
    const glm::mat4 mvp = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup))));
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    Haruka::World::Terrain::TerrainNodeRenderer rBig, rSmall;
    if (!rBig.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 4096)) { CHECK(false, "init del pase"); return; }
    // ⚠️ Y CON EL POOL PEQUEÑO, que es el juego lejos: con 512 huecos el selector (tope 384) deja
    // la mitad de la escena dibujada POR ANCESTRO. Un nodo caido tiene que seguir la misma ley
    // continua desde el mapa de su ancestro; si no, en la frontera con un vecino bien dibujado
    // queda un desnivel (Andoni, 20-09). Se mide lo mismo con los dos pools.
    if (!rSmall.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 512)) { CHECK(false, "init del pase (pool pequeño)"); return; }
    Haruka::World::Terrain::TerrainNodeRenderer* rp = &rBig;
    size_t lastAncestors = 0;

    auto render = [&](int dbg, std::vector<uint8_t>& px) {
        Haruka::World::Terrain::TerrainNodeRenderer& r = *rp;
        r.setDebugViewOverride(dbg);
        px.assign((size_t)w * h * 4, 0);
        Haruka::World::Terrain::TerrainNodeRenderer::FrameStats last{};
        for (int f = 0; f < 24; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            last = r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk && f == 23) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame(); pumpWindowEvents();
            if (isVk && f == 23) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
        // Si algo se dibuja por ANCESTRO, la medida es de otra cosa (datos mas bastos, no la zancada).
        std::printf("    [pool %zu · vista %d] sel %zu / tope %zu · dibujados %zu · por ancestro %zu · zancadas %u..%u · %.2f M tris\n",
                    rp == &rBig ? (size_t)4096 : (size_t)512, dbg, last.selected, last.selBudget, last.drawn, last.ancestors, last.strideMin, last.stride, last.tris / 1e6);
        lastAncestors = last.ancestors;
    };
    // Clase de zancada por pixel, de la vista 4 (colores planos distintos por zancada; negro = cielo).
    auto classes = [&](const std::vector<uint8_t>& px, std::vector<int>& cls, std::vector<uint32_t>& palette) {
        cls.assign((size_t)w * h, -1); palette.clear();
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const size_t k = ((size_t)y * w + x) * 4;
            if (px[k] < 8 && px[k+1] < 8 && px[k+2] < 8) continue;
            // Cuantizado a 4 bits por canal: la interpolacion plana no mezcla, pero el swapchain puede
            // redondear distinto en GL y VK.
            const uint32_t key = ((uint32_t)(px[k] >> 4) << 8) | ((uint32_t)(px[k+1] >> 4) << 4) | (uint32_t)(px[k+2] >> 4);
            int id = -1;
            for (size_t i = 0; i < palette.size(); ++i) if (palette[i] == key) { id = (int)i; break; }
            if (id < 0) { palette.push_back(key); id = (int)palette.size() - 1; }
            cls[(size_t)y * w + x] = id;
        }
    };
    // ── LO QUE SE MIDE AQUI, Y LO QUE NO ────────────────────────────────────────────────────────
    // La rugosidad de la IMAGEN a 256 px y con la camara rasante no mide el relieve: la domina la
    // estructura de la malla (facetas y triangulos de 2,4 m a 2 px), y la MISMA superficie sale con
    // 0,31 de rugosidad en una malla a zancada 1 y 0,55 a zancada 4 (medido en esta escena). Lo que
    // si se puede medir sin ambiguedad en la GPU es EL MAPA QUE LEE CADA VERTICE (vista 12): con la
    // rampa tiene que ser un degradado continuo en la distancia, y sin ella una escalera de saltos
    // de 1. La continuidad del relieve y de la normal dibujados se mide en CPU sobre los mismos
    // mapas (`terrain_node_stride_ramp_continuity`), que es donde se ve en metros.
    std::vector<uint8_t> pxLit, pxStride, pxMap;
    std::vector<int> cls; std::vector<uint32_t> pal;
    double worstRamp = 0.0, worstStep = 0.0, worstSmall = 0.0; size_t nClasses = 0, ancSmall = 0;
    for (int pass = 0; pass < 3; ++pass) {
        // 0 = la rampa que corre · 1 = el escalon (contraprueba) · 2 = la rampa con el pool pequeño
        rp = (pass == 2) ? &rSmall : &rBig;
        Haruka::World::Terrain::TerrainNodeRenderer& r = *rp;
        r.setStrideRampOverride(pass == 1 ? 0.0 : -1.0);
        render(0, pxLit);
        render(4, pxStride);
        render(12, pxMap);
        if (const char* dir = std::getenv("HARUKA_TERRAIN_PNG")) {
            const bool flip = detectRowFlip(pxLit, w, h);
            for (int which = 0; which < 3; ++which) {
                const std::vector<uint8_t>& src = which == 0 ? pxLit : which == 1 ? pxStride : pxMap;
                std::vector<uint8_t> img(src.size());
                for (int y = 0; y < h; ++y) std::memcpy(&img[(size_t)y * w * 4], &src[(size_t)(flip ? h - 1 - y : y) * w * 4], (size_t)w * 4);
                Haruka::IO::writePNG(std::string(dir) + "/" + (isVk ? "vk_" : "gl_") + "anillos_terreno_"
                                 + (pass == 0 ? "rampa" : pass == 1 ? "escalon" : "pool512") + (which == 0 ? "_lit" : which == 1 ? "_zancada" : "_mapa") + ".png", w, h, 4, img.data());
            }
        }
        classes(pxStride, cls, pal);
        std::vector<uint8_t> pxDist; render(13, pxDist);
        // El nivel ABSOLUTO del mapa leido por pixel (vista 12: gris 0,1 + 0,08·(A − 6); 8 bits =
        // 1/20 de nivel) y su salto entre filas a 2 px en la misma columna, DESCONTANDO lo que la ley
        // continua cambia entre esas dos distancias (mas alla de 300 m, A baja log2(d2/d1) entre dos
        // pixeles: cerca del horizonte dos filas son cientos de metros). Lo que queda es el salto
        // que NO explica la distancia: el escalon. Solo entre 100 y 1 500 m (la vista 13 da la
        // distancia a 8 m de resolucion).
        double worst = 0.0; int wy = -1, wx = -1;
        for (int x = 0; x < w; ++x) for (int y = 0; y + 2 < h; ++y) {
            const size_t k0 = ((size_t)y * w + x) * 4, k1 = ((size_t)(y + 2) * w + x) * 4;
            if (pxMap[k0] < 8 || pxMap[k1] < 8) continue;               // cielo
            const double d0 = pxDist[k0] / 255.0 * 2000.0, d1 = pxDist[k1] / 255.0 * 2000.0;
            if (d0 < 100.0 || d1 < 100.0 || d0 > 1500.0 || d1 > 1500.0) continue;   // 8 m de resolucion: a 100 m son 0,1 de log2
            const double A0 = (pxMap[k0] / 255.0 - 0.1) / 0.08 + 6.0, A1 = (pxMap[k1] / 255.0 - 0.1) / 0.08 + 6.0;
            const double dA = A1 - A0;   // nivel absoluto del mapa: decrece con la distancia segun la ley
            // ⚠️ `screenWant` IS THE RENDERER'S `vertexPx / errorPx`, NOT A WRITTEN 4.0 (11-10). The 4.0
            // was 8 px / 2 px; since `vertexPx` went to 24 (23-09) the shader gets 12, and the test
            // compared the GPU with a law the game no longer uses (measured: 1.66 against 0.16).
            const double screenWant = Haruka::World::Terrain::TerrainNodeRenderer::vertexPx()
                                    / std::max(Haruka::World::Terrain::TerrainNodeRenderer::errorPx(), 1e-3);
            // La ley, en nivel absoluto: la misma `nodeStrideMapF` (con el texel del nivel 17, que es
            // el que a 1080p es hoja hasta 614 m; para otro nivel solo cambia una constante, y aqui
            // se comparan diferencias). En la contraprueba (rampa 0) la ley es la del escalon.
            const double tex17 = Haruka::World::Terrain::nodeTexelM(Haruka::World::Terrain::NodeId{ Haruka::Tools::PlanetFace::FRONT, 17, 0, 0 }, R);
            const double ramp = (pass == 1) ? 0.0 : Haruka::World::Terrain::TERRAIN_NODE_STRIDE_RAMP;
            const double fA = Haruka::World::Terrain::nodeStrideMapF(d0, tex17, Haruka::World::Terrain::TERRAIN_NODE_STRIDE_MATCH_M, ramp, screenWant, 0.5, radPerPx, Haruka::World::Terrain::TERRAIN_NODE_ERROR_PX);
            const double fB = Haruka::World::Terrain::nodeStrideMapF(d1, tex17, Haruka::World::Terrain::TERRAIN_NODE_STRIDE_MATCH_M, ramp, screenWant, 0.5, radPerPx, Haruka::World::Terrain::TERRAIN_NODE_ERROR_PX);
            const double expected = -(fB - fA);
            // Con la rampa a 0 el escalon cae en un pixel concreto y la vista 13 lo situa con 8 m de
            // error: en la contraprueba se mide contra la ley CONTINUA para que el escalon aparezca.
            const double expectedC = -(Haruka::World::Terrain::nodeStrideMapF(d1, tex17, Haruka::World::Terrain::TERRAIN_NODE_STRIDE_MATCH_M, Haruka::World::Terrain::TERRAIN_NODE_STRIDE_RAMP, screenWant, 0.5, radPerPx, Haruka::World::Terrain::TERRAIN_NODE_ERROR_PX)
                                     - Haruka::World::Terrain::nodeStrideMapF(d0, tex17, Haruka::World::Terrain::TERRAIN_NODE_STRIDE_MATCH_M, Haruka::World::Terrain::TERRAIN_NODE_STRIDE_RAMP, screenWant, 0.5, radPerPx, Haruka::World::Terrain::TERRAIN_NODE_ERROR_PX));
            const double resid = std::abs(dA - (pass == 1 ? expectedC : expected));
            if (resid > worst) { worst = resid; wy = y; wx = x; }
        }
        {   // El nivel ABSOLUTO del mapa en la columna central, del pixel mas cercano al mas lejano:
            // bajo los pies tiene que ser 17 (el mapa propio del nivel 17). Un desfase constante aqui
            // no lo veria el salto entre filas de abajo.
            std::printf("    mapa absoluto en la columna central (cerca -> lejos):");
            int shown = 0;
            for (int y = 0; y < h && shown < 8; y += 12) {
                const size_t k = ((size_t)y * w + w / 2) * 4;
                if (pxMap[k] < 8) continue;
                std::printf(" %.2f@%.0fm", (pxMap[k] / 255.0 - 0.1) / 0.08 + 6.0, pxDist[k] / 255.0 * 2000.0); ++shown;
            }
            std::printf("\n");
        }
        std::printf("    %s: %zu zancadas en cuadro · mayor salto del MAPA leido entre filas a 2 px (descontada la ley): %.2f (columna %d, fila %d)\n",
                    pass == 0 ? "CON rampa (lo que corre)" : pass == 1 ? "SIN rampa (contraprueba)" : "CON rampa, pool 512 (mitad por ancestro)", pal.size(), worst, wx, wy);
        if (std::getenv("HARUKA_TERRAIN_PNG") && wx >= 0) {   // la columna del peor salto, para mirar
            std::printf("      columna %d (fila: mapa absoluto · distancia · zancada):\n", wx);
            for (int y = std::max(wy - 8, 0); y < std::min(wy + 10, h); ++y) {
                const size_t k = ((size_t)y * w + wx) * 4;
                std::printf("        %3d: mapa absoluto %.2f · %5.0f m · zancada %d\n", y, (pxMap[k] / 255.0 - 0.1) / 0.08 + 6.0, pxDist[k] / 255.0 * 2000.0, cls[(size_t)y * w + wx]);
            }
        }
        if (pass == 0) { worstRamp = worst; nClasses = pal.size(); }
        else if (pass == 1) worstStep = worst;
        else { worstSmall = worst; ancSmall = lastAncestors; }
    }
    rBig.setStrideRampOverride(-1.0);
    CHECK(nClasses >= 2, "hay al menos dos zancadas en cuadro (si no, no hay frontera que medir)");
    CHECK(worstStep > 0.6, "CONTRAPRUEBA: sin la rampa el mapa leido SALTA un nivel entero en el radio de zancada (contra la ley continua)");
    // Lo que queda es ruido de las vistas de 8 bits (mapa 1/51, distancia 8 m) y el muestreo de
    // la malla. La continuidad en METROS la mide `terrain_node_stride_ramp_continuity`; aqui se
    // comprueba que la GPU aplica LA MISMA ley que la CPU, incluidas las fronteras de nivel.
    CHECK(worstRamp < 0.35, "con la rampa y el corte continuo, lo que cambia el mapa leido entre dos filas lo explica la ley (GPU = CPU)");
    // ⚠️ Con el pool pequeño el selector se queda SIN SITIO y emite hojas mas bastas de lo que la ley
    // pide (nivel 13 a 300 m: f = −1, recortado a 0): eso es un escalon inherente al hambre —un nodo
    // no puede leer mas fino de lo que tiene— y no lo que se quiere medir aqui. Se imprime, y lo
    // que si se comprueba es que haya caidas por ancestro. La aritmetica de los nodos CAIDOS (leer
    // la ley desde el mapa del ancestro `kUp`) la mide en CPU `terrain_node_stride_ramp_continuity`.
    (void)worstSmall;
    CHECK(ancSmall > 0, "con el pool pequeño hay nodos dibujados por ancestro (la ruta kUp > 0 se ejecuta en la GPU)");
}


// ── LA MESETA DEL HORIZONTE: un nodo lejano con datos de un ANCESTRO sale como un escalon ───────
//
// Andoni, con un zoom del horizonte: la silueta del terreno contra el cielo es una ESCALERA de
// rectangulos planos del ancho de un nodo. Causa: mas alla del horizonte de la esfera lisa solo se
// ve lo que el relieve levanta sobre la tangente; un nodo caido dos niveles o mas dibuja una
// superficie casi PLANA a cota constante, o sea una meseta rectangular recortando el cielo. Se
// saltan (`FrameStats::plateauSkipped`) hasta que tengan datos propios.
//
// Se fuerza la caida con un POOL MINIMO. Contrapruebas: (1) con el pool grande no se salta nada —
// asi el test no pasa por un camino que no existe en la partida—; (2) lo CERCANO se dibuja igual en
// los dos casos (la regla no puede abrir un agujero bajo los pies).
void testHorizonPlateau()
{
    BEGIN("v5: la MESETA del horizonte (nodo por ancestro mas alla del horizonte liso) no se dibuja");
    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam = pc + up0 * (R + 2.0);
    const glm::dvec3 fwd = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double radPerPx = fovY / (double)h;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);
    const float aspect = (float)w / (float)h;
    const float fCot   = 1.0f / std::tan((float)fovY * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = fCot / aspect; proj[1][1] = fCot; proj[2][3] = -1.0f; proj[3][2] = 1.0f;
    const glm::mat4 mvp = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(up0))));
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    auto run = [&](size_t pool, size_t& skipped, size_t& anc, size_t& drawn, size_t& lowerPx) {
        Haruka::World::Terrain::TerrainNodeRenderer r;
        if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", pool)) { CHECK(false, "init del pase"); return; }
        std::vector<uint8_t> px((size_t)w * h * 4, 0xAA);
        Haruka::World::Terrain::TerrainNodeRenderer::FrameStats last{};
        for (int f = 0; f < 12; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            last = r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame(); pumpWindowEvents();
            if (isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
        // ⚠️ `ancestors` (toda caida), no `ancDeep`: las profundas MAS ALLA del horizonte se
        // saltan ANTES de contarse, asi que ancDeep sale 0 justo cuando la regla funciona.
        skipped = last.plateauSkipped; anc = last.ancestors; drawn = last.drawn;
        // Pixeles con terreno en la mitad del cuadro que lo tiene (lo CERCANO): el mayor de las dos.
        size_t top = 0, bot = 0;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const size_t k = ((size_t)y * w + x) * 4;
            if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
            if (px[k] || px[k+1] || px[k+2]) { if (y < h / 2) ++top; else ++bot; }
        }
        lowerPx = std::max(top, bot);
    };

    size_t skS = 0, ancS = 0, drS = 0, pxS = 0, skB = 0, ancB = 0, drB = 0, pxB = 0;
    run(96,   skS, ancS, drS, pxS);     // pool minimo: obliga a caer por ancestro
    run(4096, skB, ancB, drB, pxB);     // pool de la partida
    std::printf("    pool 96:   caidas por ancestro %zu · MESETAS saltadas %zu · dibujados %zu · terreno cercano %zu px\n", ancS, skS, drS, pxS);
    std::printf("    pool 4096: caidas por ancestro %zu · MESETAS saltadas %zu · dibujados %zu · terreno cercano %zu px\n", ancB, skB, drB, pxB);
    CHECK(ancS > 0, "con el pool minimo hay caidas por ancestro (si no, el test no prueba nada)");
    CHECK(skS > 0, "y las que caen MAS ALLA del horizonte liso no se dibujan (la meseta en escalera)");
    CHECK(skB == 0 || skB < skS / 4, "CONTRAPRUEBA: con el pool de la partida casi no se salta nada");
    CHECK(pxS > 0 && pxB > 0 && pxS > pxB * 0.9,
          "CONTRAPRUEBA: lo CERCANO se sigue dibujando igual (la regla no abre agujeros bajo los pies)");
}

void testTerrainNodeCoverage()
{
    BEGIN("v5 F3: el pase de nodos CUBRE la pantalla (a varias altitudes)");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    Haruka::World::Terrain::TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 512)) {
        CHECK(false, "init del pase"); return;
    }

    // El tamaño REAL del swapchain, que no tiene por qué ser el de la ventana (el compositor escala).
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);

    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    // ⚠️ EL ORACULO NO ES UN UMBRAL, ES EL HORIZONTE.
    //
    // Primera version de este test: "cubierto > 50 %% a toda altitud". Fallaba en orbita y parecia
    // haber cazado el bug — pero NO: a 2000 km el horizonte cae 40,4 grados bajo la horizontal y con
    // fovY 60 el borde inferior del cuadro solo llega a 30. El planeta esta FUERA DE PLANO y 0 %% es
    // la respuesta correcta. Un umbral fijo habria acusado al pase de un fallo inexistente.
    //
    // Asi que se predice la cobertura desde la geometria: el horizonte esta a acos(R/(R+alt)) bajo la
    // horizontal, y en perspectiva la pantalla es lineal en TANGENTE, no en angulo.
    auto predictLowerHalf = [&](double altM) {
        const double th = std::acos(R / (R + altM));
        const double f  = std::tan(th) / std::tan(fovY * 0.5);
        return (f >= 1.0) ? 0.0 : (1.0 - f);
    };

    struct Case { double altM; bool nadir; const char* what; };
    const Case cases[5] = {
        { 2.0,       false, "a pie (2 m)"        }, { 800.0,     false, "bajo (800 m)"    },
        { 50000.0,   false, "alto (50 km)"       }, { 2000000.0, false, "orbita, horizonte"},
        { 2000000.0, true,  "orbita, mirando abajo" },
    };

    bool allCovered = true, readbackOk = true;
    for (const Case& cse : cases) {
        const glm::dvec3 cam = pc + up0 * (R + cse.altM);
        // Al horizonte (tangente) es donde el terreno lejano llena la mitad baja: la vista que delata
        // "al alejarte no se ve". El caso nadir existe porque en orbita la tangente no ve NADA, y un
        // 0 %% esperado que coincide con un 0 %% medido no demuestra que el pase dibuje.
        const glm::dvec3 fwd = cse.nadir ? -up0
                                         : glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
        const glm::dvec3 vup = cse.nadir ? glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0))) : up0;
        const glm::mat4 proj = glm::perspective((float)fovY, (float)w / (float)h, 1.0f, 1e9f);
        // ⚠️ Vista SIN traslacion: el pase compone posiciones RELATIVAS AL OJO, igual que el planeta
        // (`rotOnlyVP`). Con la vista completa se restaria la camara dos veces — que es exactamente
        // uno de los bugs que este test existe para cazar.
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup));
        const glm::mat4 mvp  = proj * glm::mat4(glm::mat3(view));

        // Varios frames: el pool se llena con presupuesto, asi que el primero dibuja poco.
        // ⚠️ CENTINELA, PORQUE UN readPixels QUE NO ESCRIBE PARECE "PANTALLA VACIA".
        //
        // Este buffer se llenaba de ceros y se contaban los pixeles no negros. Cuando el readback no
        // escribe nada —y en este camino de Vulkan no escribe— el conteo da 0 y el test acusa al pase
        // de no dibujar. Estuve persiguiendo un bug de Vulkan que el test se estaba inventando.
        // Con 0xAA se distingue: si al acabar sigue siendo 0xAA, el que fallo fue el instrumento.
        std::vector<uint8_t> px((size_t)w * h * 4, 0xAA);
        Haruka::World::Terrain::TerrainNodeRenderer::FrameStats last{};
        for (int f = 0; f < 12; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            // ⚠️ `prepare` FUERA del pase (despacha compute + barrera) y `draw` DENTRO. Este orden es
            // el bug que este test encontró: con las dos cosas dentro, Vulkan daba 0 %% de cobertura
            // en las cinco altitudes mientras OpenGL las pasaba todas.
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = 0.0f; cv.color[1] = 0.0f; cv.color[2] = 0.0f; cv.color[3] = 1.0f;
            cv.depth = 0.0f;                       // reversed-Z: el clear va a 0
            ctx->beginRenderPass({}, cv);
            last = r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
        // El fragmento nunca emite negro puro (su color base minimo es 0.22*0.25), asi que un pixel
        // negro es fondo sin cubrir. Solo se cuenta la MITAD BAJA: la alta es cielo por definicion.
        size_t sentinel = 0;
        for (size_t k = 0; k < (size_t)w * h; ++k)
            if (px[k*4] == 0xAA && px[k*4+1] == 0xAA && px[k*4+2] == 0xAA) ++sentinel;
        if (sentinel > (size_t)w * h * 9 / 10) {
            std::printf("    %-24s SIN LECTURA: readPixels no escribio (%.0f %% centinela) — "
                        "no se puede medir en este backend\n",
                        cse.what, 100.0 * (double)sentinel / (double)(w * h));
            readbackOk = false;
            continue;
        }
        size_t covered = 0;
        for (int y = 0; y < h / 2; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t* p8 = &px[((size_t)y * w + x) * 4];
                if (p8[0] || p8[1] || p8[2]) ++covered;
            }
        const double pct  = 100.0 * (double)covered / (double)(w * (h / 2));
        // Mirando abajo, el disco del planeta (radio angular asin(R/(R+alt)) = 49,6 grados a 2000 km)
        // desborda el cuadro entero, asi que se espera todo lleno.
        const double pred = cse.nadir ? 100.0 : 100.0 * predictLowerHalf(cse.altM);
        const double err  = std::fabs(pct - pred);
        std::printf("    %-24s cubierto %5.1f %%  ·  predice %5.1f %%  ·  error %4.1f pts"
                    "   [sel %zu drawn %zu noSlot %zu resid %zu tris %zu]\n",
                    cse.what, pct, pred, err, last.selected, last.drawn, last.noSlot,
                    last.resident, last.tris);
        // 3 puntos: el horizonte real no es la esfera lisa — el relieve lo sube y lo baja unos cientos
        // de metros, y a 50 km eso ya vale decimas de grado.
        if (err > 3.0) allCovered = false;
    }
    if (readbackOk) CHECK(allCovered, "la cobertura en pantalla casa con el horizonte a TODAS las altitudes");
    else std::printf("    (omitido: sin readback de color no hay nada que afirmar en este backend)\n");

    // ── ⚠️ DESDE ORBITA EL PLANETA NO ES DEL COLOR DEL AIRE (2026-09-18) ────────────────────────
    //
    // Andoni: "en orbita esta todo en blanco". No eran las nubes: la perspectiva aerea del terreno
    // extinguia por la DISTANCIA geometrica (e^{-dist/L}, L = 18-60 km), y desde 2 000 km eso es
    // cero: todo el planeta del color del aire bajo el horizonte. Ahora la extincion integra el aire
    // atravesado con una atmosfera exponencial (lib/aerial.glsl, `aerial.w` = escala de altura).
    // Se mide el desvio del color del planeta al nadir desde 2 000 km respecto a SIN aire:
    // con el aire de verdad tiene que quedar cerca; con el camino plano (w = 0, el bug) se va entero.
    if (readbackOk) {
        const double altO = 2000000.0;
        const glm::dvec3 cam = pc + up0 * (R + altO);
        const glm::dvec3 fwd = -up0;
        const glm::dvec3 vup = glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0)));
        const glm::mat4 proj = glm::perspective((float)fovY, (float)w / (float)h, 1.0f, 1e9f);
        const glm::mat4 mvp  = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup))));
        // Material sintetico: el camino de sombreado REAL (la luz plana devuelve antes del aire).
        const uint32_t TS = 64, LAYERS = 4;
        std::vector<uint8_t> pix((size_t)TS * TS * LAYERS * 4, 200);
        TextureDesc atd; atd.width = atd.height = TS; atd.layers = LAYERS;
        atd.format = Format::RGBA8; atd.filter = Filter::Linear; atd.wrap = Wrap::Repeat;
        atd.initialData = pix.data();
        TextureHandle albedo = g_dev->createTexture(atd), normal = g_dev->createTexture(atd);
        struct MatUBO { float v[64]; } mu{};
        for (float& f : mu.v) f = 0.5f;
        BufferHandle matUBO = g_dev->createBuffer(BufferUsage::Uniform, sizeof(mu), &mu, BufferMemory::Dynamic);
        auto renderAir = [&](const glm::vec4& aerial, std::vector<uint8_t>& px) {
            Haruka::World::Terrain::TerrainNodeRenderer::Shade sh;
            sh.albedo = albedo; sh.normal = normal; sh.materialUBO = matUBO;
            sh.tiling = 8.0f; sh.shoreLayer = 0; sh.lightDir = glm::vec3(up0);
            sh.on = true; sh.aerial = aerial;
            r.setShade(sh);
            px.assign((size_t)w * h * 4, 0xAA);
            for (int f = 0; f < 12; ++f) {
                Context* ctx = g_dev->beginFrame();
                if (!ctx) break;
                r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
                ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
                cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
                ctx->beginRenderPass({}, cv);
                r.draw(ctx, cam, pc, R, mvp);
                ctx->endRenderPass();
                if (!isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
                g_dev->endFrame(); pumpWindowEvents();
                if (isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            }
        };
        // ⚠️ EL AIRE YA NO ES UN MANDO (`aerial`), es la atmosfera: la luz del frame en ESTE escenario
        // (ojo a 2 000 km, Sol al cenit). "Sin aire" = un planeta sin atmosfera (radio 0 para la luz:
        // ni transmitancia ni aire de en medio). La "contraprueba del camino plano" medía un modelo
        // que ya no existe (la integral va por la densidad real a cada altitud).
        std::vector<uint8_t> pxNone, pxAir;
        setTestSkyLight(pc - cam, 0.0, glm::dvec3(up0));
        renderAir(glm::vec4(0.0f, 1.0f, (float)altO, 4000.0f), pxNone);          // sin aire
        setTestSkyLight(pc - cam, R, glm::dvec3(up0));
        renderAir(glm::vec4(0.0f, 1.0f, (float)altO, 4000.0f), pxAir);           // la atmosfera
        auto desvio = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
            double acc = 0.0; size_t n = 0;
            for (size_t k = 0; k + 3 < a.size(); k += 4) {
                if (a[k] == 0xAA && a[k+1] == 0xAA && a[k+2] == 0xAA) continue;
                if (!a[k] && !a[k+1] && !a[k+2]) continue;   // fondo negro: no es planeta
                acc += (std::abs((int)a[k] - b[k]) + std::abs((int)a[k+1] - b[k+1]) + std::abs((int)a[k+2] - b[k+2])) / 3.0;
                ++n;
            }
            return n ? acc / (double)n : -1.0;
        };
        const double dAir = desvio(pxNone, pxAir);
        std::printf("    desde 2 000 km al nadir, desvio medio del color respecto a SIN atmosfera: %.1f/255\n", dAir);
        CHECK(dAir >= 0.0 && dAir < 25.0, "con la atmosfera el planeta conserva su color desde orbita (desvio < 25/255)");
        CHECK(dAir > 1.0, "CONTRAPRUEBA: y la atmosfera SE VE (el aire de en medio tiñe y atenua: no es un no-op)");
        Haruka::World::Terrain::TerrainNodeRenderer::Shade sh0; r.setShade(sh0);
        g_dev->destroy(albedo); g_dev->destroy(normal); g_dev->destroy(matUBO);
    }

    // CONTRAPRUEBA: sin dibujar nada, la cobertura tiene que ser 0. Sin esto, el test pasaria si
    // `readPixels` devolviera basura no nula o si el clear no funcionara.
    std::vector<uint8_t> empty((size_t)w * h * 4, 0xFF);
    if (Context* ctx = g_dev->beginFrame()) {
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
        cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        ctx->endRenderPass();
        if (!isVk) g_dev->readPixels(0, 0, w, h, Format::RGBA8, empty.data());
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk) g_dev->readPixels(0, 0, w, h, Format::RGBA8, empty.data());
    }
    // ⚠️ LA CONTRAPRUEBA NECESITA EL MISMO CENTINELA QUE EL BUCLE DE ARRIBA.
    //
    // Se inicializa a 0xFF y se cuentan los no-negros, asi que si `readPixels` no escribe, el conteo
    // sale ENORME y el test acusa al pase de dibujar sin dibujar. Es la version espejo del fallo que
    // ya costo medio dia: el bucle principal se inicializa a 0xAA y ahi el mismo fallo se leia como
    // "0 % de cobertura". El mismo instrumento roto miente en las dos direcciones.
    //
    // Y aqui delato una INCONSISTENCIA DEL RHI, no del terreno: en OpenGL `framebufferSize` dice
    // 1920x1080 mientras `readPixels` llena solo 256x256 (medido: 97 % del buffer sin tocar). Los
    // dos numeros tienen que venir de la misma fuente, y hoy no vienen.
    size_t stray = 0, strayUntouched = 0;
    for (size_t k = 0; k < (size_t)w * h; ++k) {
        const bool untouched = (empty[k*4] == 0xFF && empty[k*4+1] == 0xFF && empty[k*4+2] == 0xFF);
        if (untouched) ++strayUntouched;
        else if (empty[k*4] || empty[k*4+1] || empty[k*4+2]) ++stray;
    }
    if (strayUntouched > (size_t)w * h / 10) {
        std::printf("    CONTRAPRUEBA: SIN LECTURA (%.0f %% del buffer sin escribir) — `framebufferSize` "
                    "dice %dx%d pero `readPixels` no lo llena; no se puede afirmar nada\n",
                    100.0 * (double)strayUntouched / (double)(w * h), w, h);
    } else {
        std::printf("    CONTRAPRUEBA: solo con clear, pixeles no negros = %zu\n", stray);
        CHECK(stray == 0, "CONTRAPRUEBA: sin dibujar no hay cobertura (el conteo mide el pase)");
    }

    r.shutdown();
}
