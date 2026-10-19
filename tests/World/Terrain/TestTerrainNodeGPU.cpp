#include "RHI/Common/RHITestCommon.h"

// ================================================================================================
// F1 del PLAN TERRENO v5 — ¿genera la GPU EL MISMO nodo que la CPU?
//
// ⚠️ ESTE ES EL TEST QUE DECIDE SI EL v5 ES VIABLE, y por eso vive aquí y no en `haruka_tests`: hace
// falta un device real.
//
// El v5 apuesta a que el heightmap de cada nodo se genera en GPU y se cachea. Cliente y servidor
// corren el MISMO compute (decisión: GPU obligatoria en el servidor), así que basta con que el
// resultado sea reproducible. Pero "reproducible" hay que demostrarlo, y el sitio donde puede
// romperse no es el ruido —su hash es aritmética entera, bit-exacta por diseño— sino:
//
//   · la CONTRACCIÓN `a*b+c` → FMA, que el compilador del driver puede aplicar donde quiera,
//   · el orden de operaciones al reconstruir la coordenada de cara,
//   · una divergencia entre `harukaCubeFaceToDir` (GLSL) y `cubeFaceToDir` (C++), que son gemelos
//     escritos a mano.
//
// Se compara contra `nodeFillHeights`, la referencia de CPU, sobre el MISMO nodo. Primero por hash
// (¿son idénticos bit a bit?) y, si no lo son, por diferencia máxima en metros — porque "no son
// idénticos" y "difieren 3 cm" llevan a decisiones distintas: lo primero puede ser aceptable con una
// tolerancia declarada, lo segundo no.
// ================================================================================================
void testTerrainNodeGpuParity()
{
    BEGIN("v5 F1: el nodo generado en GPU coincide con la referencia de CPU");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const Haruka::World::Terrain::NodeId node{ Haruka::Tools::PlanetFace::FRONT, 14, 4200, 3100 };   // el mismo del golden de CPU
    const uint32_t N = Haruka::World::Terrain::TERRAIN_NODE_TEXELS;
    const size_t   count = (size_t)N * N;

    // --- referencia de CPU ---
    std::vector<float> cpu(count);
    Haruka::World::Terrain::nodeFillHeights(node, R, cpu.data());
    const uint32_t cpuHash = Haruka::World::Terrain::nodeContentHash(cpu.data(), count);

    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string cs   = base + "shaders/terrain_node.comp";
    PipelineDesc pd;
    pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de terrain_node.comp creado");
    if (!valid(cp)) return;

    // Gemelo del bloque `NodeParams` del shader. std140: cada ivec4/vec4 ocupa 16 B, sin relleno.
    struct NodeParamsUBO { int32_t node[4]; int32_t grid[4]; float misc[4]; } up{};
    up.node[0] = (int32_t)node.face; up.node[1] = (int32_t)node.level;
    up.node[2] = (int32_t)node.i;    up.node[3] = (int32_t)node.j;
    up.grid[0] = (int32_t)N;         up.grid[1] = (int32_t)Haruka::World::Terrain::TERRAIN_NODE_CELLS;
    up.misc[0] = (float)R;           up.misc[1] = (float)Haruka::World::Terrain::nodeTexelM(node, R);

    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(up), &up, BufferMemory::Dynamic);
    // Readback: mapeo persistente, se lee tras esperar la fence del dispatch.
    // ⚠️ SE INICIALIZA CON UN CENTINELA, no con `nullptr`. Un buffer de readback sin inicializar
    // contiene basura, y leer basura donde el shader no escribió se confunde con "la GPU calculó
    // mal": el primer intento de este test reportó una diferencia de 1,7e38 m y una media NaN, que
    // no era el shader sino los téxeles que nadie había tocado todavía.
    std::vector<float> sentinel(count, -12345.0f);
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, count * sizeof(float),
                                           sentinel.data(), BufferMemory::Readback);
    CHECK(valid(ubo) && valid(out), "buffers de parametros y de salida creados");
    if (!valid(ubo) || !valid(out)) { g_dev->destroy(cp); return; }

    // El dispatch va FUERA de un render pass (ver `testDispatchInsideRenderPass`: dentro es ilegal
    // en Vulkan). Grupos de 8x8, redondeando hacia arriba.
    FenceHandle fence{};
    if (Context* c = g_dev->beginFrame()) {
        c->bindPipeline(cp);
        c->bindUniformBuffer(0, ubo);
        c->bindStorageBuffer(1, out);
        c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
        c->memoryBarrier();
        // ⚠️ Y HAY QUE ESPERAR LA FENCE. El `memoryBarrier` ordena los accesos DENTRO de la GPU; no
        // dice nada sobre cuándo la CPU puede leer el mapeo. El contrato de `mappedData` es explícito
        // ("solo válido tras esperar la fence del trabajo que lo escribió") y saltárselo es una
        // carrera que en una GPU rápida a veces pasa — la peor clase de fallo.
        fence = c->signalFence();
        g_dev->endFrame();
        pumpWindowEvents();
        if (Context* c2 = g_dev->beginFrame()) {
            const bool signalled = c2->waitFence(fence, 5000000000ull);   // 5 s de tope
            CHECK(signalled, "la fence del dispatch senala (el compute ha terminado)");
            c2->deleteFence(fence);
            g_dev->endFrame();
        }
    }

    // ── BISECCIÓN: ¿en qué etapa empieza a diverger? ────────────────────────────────────────────
    // Sin esto, un hash que no casa solo dice "difieren". Estos tres modos comparan la coordenada de
    // cara, la dirección y la entrada del ruido, así que señalan la etapa exacta.
    {
        struct Stage { int mode; const char* name; };
        const Stage stages[3] = { {1, "lx (coordenada de cara)"}, {2, "dir.x (proyeccion Cobb)"},
                                  {3, "entrada del ruido (dir.x*R*0.00035)"} };
        for (const Stage& st : stages) {
            NodeParamsUBO sp = up; sp.grid[2] = st.mode;
            g_dev->updateBuffer(ubo, 0, sizeof(sp), &sp);
            FenceHandle f2{};
            if (Context* c = g_dev->beginFrame()) {
                c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
                c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
                c->memoryBarrier(); f2 = c->signalFence();
                g_dev->endFrame();
                if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(f2, 5000000000ull); c2->deleteFence(f2); g_dev->endFrame(); }
            }
            const float* g = (const float*)g_dev->mappedData(out);
            if (!g) continue;
            double worstS = 0.0;
            for (uint32_t v = 0; v < N; ++v)
                for (uint32_t u = 0; u < N; ++u) {
                    double lx, ly; Haruka::World::Terrain::nodeTexelFaceCoord(node, u, v, lx, ly);
                    const glm::dvec3 d = Haruka::World::Terrain::nodeTexelDir(node, u, v);
                    const double ref = (st.mode == 1) ? lx : (st.mode == 2) ? d.x : d.x * R * 0.00035;
                    worstS = std::max(worstS, std::abs((double)g[(size_t)v * N + u] - (float)ref));
                }
            std::printf("    etapa %-38s diferencia peor %.3e\n", st.name, worstS);
        }
        // Volver al modo de produccion para la comparacion final.
        NodeParamsUBO sp = up; sp.grid[2] = 0;
        g_dev->updateBuffer(ubo, 0, sizeof(sp), &sp);
        FenceHandle f3{};
        if (Context* c = g_dev->beginFrame()) {
            c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
            c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
            c->memoryBarrier(); f3 = c->signalFence();
            g_dev->endFrame();
            if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(f3, 5000000000ull); c2->deleteFence(f3); g_dev->endFrame(); }
        }
    }

    const float* gpu = (const float*)g_dev->mappedData(out);
    CHECK(gpu != nullptr, "el buffer de salida es legible");
    if (gpu) {
        const uint32_t gpuHash = Haruka::World::Terrain::nodeContentHash(gpu, count);
        std::printf("    hash CPU 0x%08X · hash GPU 0x%08X\n", cpuHash, gpuHash);

        // La diferencia en METROS, que es la que decide qué hacer si los hashes no casan.
        double worst = 0.0, sum = 0.0; size_t nz = 0;
        for (size_t k = 0; k < count; ++k) {
            const double d = std::abs((double)gpu[k] - (double)cpu[k]);
            worst = std::max(worst, d); sum += d;
            if (gpu[k] != -12345.0f) ++nz;   // escrito por el shader (no quedo el centinela)
        }
        std::printf("    diferencia GPU↔CPU: peor %.6f m · media %.6f m · texeles escritos por la GPU %zu/%zu\n",
                    worst, sum / (double)count, nz, count);

        // Que la GPU haya escrito ALGO. Sin esto, un buffer de ceros daría "diferencia pequeña" si el
        // nodo fuese plano, y el test pasaría sin haber ejecutado nada.
        CHECK(nz == count, "la GPU ha escrito TODOS los texeles del nodo");

        // ── QUÉ SE EXIGE, Y POR QUÉ NO ES IDENTIDAD CPU↔GPU ─────────────────────────────────
        //
        // La bisección de arriba localiza la divergencia: `lx` coincide EXACTO (0.000e+00, el
        // direccionamiento por enteros funciona) y entra en `harukaCubeFaceToDir`, a nivel ~1e-8
        // relativo.
        //
        // ⚠️⚠️ LA EXPLICACION QUE HABIA AQUI ERA FALSA, Y DECIDIA ARQUITECTURA. Decia: "eso es `sqrt`
        // sobre `double`; GLSL no exige redondeo correcto para dobles, así que el driver puede
        // resolverlo con iteraciones y no coincidir con la CPU — y por eso el v5 exige **GPU también
        // en el servidor**, porque la identidad alcanzable es GPU↔GPU y no CPU↔GPU".
        //
        // Medido el 2026-08-31 en las cuatro combinaciones de este equipo (`testGpuFp64` con las
        // entradas por SSBO, y `testBakeStageBisect` sobre los 16 641 téxeles):
        //
        //     AMD RENOIR + OpenGL   bake peor 0,0253 m · 85 % de téxeles > 1 mm   <- el unico roto
        //     AMD RENOIR + Vulkan   bake peor 0,0001 m · 0 téxeles > 1 mm
        //     NVIDIA 3050 + OpenGL  bake peor 0,0002 m · 0 téxeles > 1 mm
        //     NVIDIA 3050 + Vulkan  bake peor 0,0001 m · 0 téxeles > 1 mm
        //
        // El `sqrt` de doble NO es el problema: en tres de las cuatro celdas `harukaCubeFaceToDir`
        // casa con `cubeFaceToDir` de C++ **hasta el ultimo bit** (residuo sub-float identico). Lo
        // que rompia era el compilador de GLSL del driver de OpenGL de AMD, que degrada
        // `inversesqrt(double)` a precision de float. Ver [[amd-opengl-degrada-fp64]].
        //
        // ⚠️ CONSECUENCIA PARA EL PLAN: **el servidor NO necesita GPU.** Lo que queda entre CPU y GPU
        // son 0,0001-0,0002 m, y eso es el redondeo en float de la suma de octavas, no la geometria.
        // La premisa de "GPU obligatoria en el servidor" se apoyaba en una medida de una sola maquina
        // con el unico backend roto de los cuatro.
        //
        // Así que se exige (a) que la GPU se reproduzca a sí misma —eso sí es requisito— y (b) que
        // la desviación contra el oráculo esté DENTRO DE UNA TOLERANCIA DECLARADA.
        constexpr double kTolM = 0.05;   // tolerancia DECLARADA, no derivada de ninguna medida viva
        std::printf("    tolerancia declarada %.3f m\n", kTolM);
        CHECK(worst < kTolM, "la desviacion contra el oraculo de CPU cabe en la tolerancia declarada");

        // REPRODUCIBILIDAD EN LA MISMA GPU. Ésta sí es la propiedad que el plan necesita: si la GPU
        // no se reproduce ni a sí misma, cliente y servidor no pueden acordar nada.
        FenceHandle f4{};
        if (Context* c = g_dev->beginFrame()) {
            c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
            c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
            c->memoryBarrier(); f4 = c->signalFence();
            g_dev->endFrame();
            if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(f4, 5000000000ull); c2->deleteFence(f4); g_dev->endFrame(); }
        }
        const float* gpu2 = (const float*)g_dev->mappedData(out);
        if (gpu2) {
            const uint32_t again = Haruka::World::Terrain::nodeContentHash(gpu2, count);
            std::printf("    reproducibilidad en la MISMA GPU: 0x%08X vs 0x%08X\n", gpuHash, again);
            CHECK(again == gpuHash, "la GPU genera el mismo nodo dos veces, BIT A BIT");
        }

        // ⚠️ LO QUE ESTE TEST NO PUEDE CONTESTAR: si dos GPU DISTINTAS coinciden. Es la pregunta que
        // de verdad decide el determinismo cliente↔servidor del v5, y necesita otra máquina. Anotar
        // el hash `0x%08X` y compararlo en otro equipo es la forma barata de cerrarla.
        std::printf("    PENDIENTE (necesita otra GPU): hash de este nodo en este equipo = 0x%08X\n",
                    gpuHash);
    }

    g_dev->destroy(out);
    g_dev->destroy(ubo);
    g_dev->destroy(cp);
}


// ================================================================================================
// F1 (coste) — ¿cuánto cuesta generar un nodo, y cuántos caben en un frame?
//
// Es la otra mitad de F1: sin esta cifra no se puede dimensionar el pool (F2) ni saber si el v5 cabe
// en el presupuesto. Y es la pregunta que decide si "generar una vez y cachear" gana de verdad al
// "evaluar por vértice y por píxel cada frame" que cuesta hoy 60 ms.
//
// ⚠️ SE MIDE CON FENCE, no con el reloj alrededor del `dispatch`. Un dispatch es asíncrono: cronometrar
// la llamada mide lo que tarda la CPU en ENCOLARLO (microsegundos) y no lo que tarda la GPU en
// hacerlo. Se encolan N nodos y se espera a que la GPU los termine; el tiempo dividido entre N es el
// coste real amortizado, que además es el régimen en el que trabajará el pool.
// ================================================================================================
void testTerrainNodeGpuCost()
{
    BEGIN("v5 F1: coste de generar nodos en GPU");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const uint32_t N = Haruka::World::Terrain::TERRAIN_NODE_TEXELS;
    const size_t   count = (size_t)N * N;

    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string cs   = base + "shaders/terrain_node.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    if (!valid(cp)) { CHECK(false, "pipeline de compute creado"); return; }

    struct NodeParamsUBO { int32_t node[4]; int32_t grid[4]; float misc[4]; } up{};
    up.grid[0] = (int32_t)N; up.grid[1] = (int32_t)Haruka::World::Terrain::TERRAIN_NODE_CELLS; up.grid[2] = 0;
    up.misc[0] = (float)R;

    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(up), &up, BufferMemory::Dynamic);
    // Salida NO de readback: aquí se mide la generación, no la descarga. Un buffer de readback vive
    // en memoria visible por el host y mediría también el coste de escribir ahí, que el pool no paga
    // (los nodos se quedan en VRAM).
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, count * 3 * sizeof(float), nullptr,
                                           BufferMemory::Dynamic);
    if (!valid(ubo) || !valid(out)) { CHECK(false, "buffers creados"); g_dev->destroy(cp); return; }

    // Se miden dos niveles: uno grueso (pocas octavas activas por su `triM`) y uno fino (todas). El
    // coste NO es uniforme —cada octava está gateada por `triM`— así que un solo número engañaría.
    struct Case { uint32_t level; const char* what; };
    const Case cases[2] = { { 8, "grueso (nivel 8)" }, { 18, "fino (nivel 18)" } };

    for (const Case& cse : cases) {
        const Haruka::World::Terrain::NodeId node{ Haruka::Tools::PlanetFace::FRONT, cse.level, 100, 100 };
        up.node[0] = (int32_t)node.face; up.node[1] = (int32_t)node.level;
        up.node[2] = (int32_t)node.i;    up.node[3] = (int32_t)node.j;
        up.misc[1] = (float)Haruka::World::Terrain::nodeTexelM(node, R);

        // Calentamiento: la primera ejecución paga compilación de shader y asignaciones del driver.
        // Medirla daría un número que no se repite nunca en producción.
        for (int warm = 0; warm < 2; ++warm) {
            if (Context* c = g_dev->beginFrame()) {
                g_dev->updateBuffer(ubo, 0, sizeof(up), &up);
                c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
                c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
                FenceHandle fw = c->signalFence();
                g_dev->endFrame();
                if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(fw, 5000000000ull); c2->deleteFence(fw); g_dev->endFrame(); }
            }
        }

        const int kNodes = 64;
        const auto t0 = std::chrono::high_resolution_clock::now();
        FenceHandle f{};
        if (Context* c = g_dev->beginFrame()) {
            c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
            for (int k = 0; k < kNodes; ++k) c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
            f = c->signalFence();
            g_dev->endFrame();
            if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(f, 10000000000ull); c2->deleteFence(f); g_dev->endFrame(); }
        }
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - t0).count();
        const double perNode = ms / kNodes;
        std::printf("    %-18s %.3f ms/nodo  ·  %.0f nodos en 1 ms  ·  %u texeles (%.3f m/texel)\n",
                    cse.what, perNode, perNode > 0 ? 1.0 / perNode : 0.0,
                    (unsigned)count, Haruka::World::Terrain::nodeTexelM(node, R));
        // Lo que decide si el pool es viable: en un presupuesto de 2 ms/frame para generación,
        // ¿cuántos nodos nuevos caben? Al descender en picado es cuando más se piden de golpe.
        std::printf("      -> con 2 ms/frame de presupuesto caben %.0f nodos nuevos por frame\n",
                    perNode > 0 ? 2.0 / perNode : 0.0);
        CHECK(perNode > 0.0 && perNode < 50.0, "el nodo se genera en un tiempo razonable (<50 ms)");
    }

    // ── DE DONDE SALE EL TIEMPO: DESPACHO, PROYECCION Y RELIEVE POR SEPARADO ────────────────────
    //
    // "0,056 ms/nodo" no dice que optimizar. Aqui se parte en tres con dos medidas:
    //   · barrido de N: si el ms/nodo baja al encolar mas, hay COSTE FIJO por dispatch y la palanca
    //     es agrupar; si no baja, el coste es todo trabajo por texel y agrupar no compra nada.
    //   · `uGrid.z = 2` (modo de biseccion que ya existia): hace la misma proyeccion y la misma
    //     escritura pero NO evalua el relieve. La diferencia con el modo 0 es el relieve, exacta.
    {
        const Haruka::World::Terrain::NodeId fine{ Haruka::Tools::PlanetFace::FRONT, 18, 100, 100 };
        up.node[0] = (int32_t)fine.face; up.node[1] = (int32_t)fine.level;
        up.node[2] = (int32_t)fine.i;    up.node[3] = (int32_t)fine.j;
        up.misc[1] = (float)Haruka::World::Terrain::nodeTexelM(fine, R);

        auto run = [&](int kNodes, int mode) -> double {
            up.grid[2] = mode;
            for (int warm = 0; warm < 2; ++warm)
                if (Context* c = g_dev->beginFrame()) {
                    g_dev->updateBuffer(ubo, 0, sizeof(up), &up);
                    c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
                    c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
                    FenceHandle fw = c->signalFence();
                    g_dev->endFrame();
                    if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(fw, 5000000000ull); c2->deleteFence(fw); g_dev->endFrame(); }
                }
            // ⚠️ EL MINIMO DE CINCO TANDAS, NO UNA. Es reloj de pared con un frame entero alrededor, y
            // una sola muestra se llevaba cualquier hipo del sistema: se midio "sin relieve 0,130 ms,
            // con relieve 0,036" en la misma pasada, y la CONTRAPRUEBA de abajo caia una vez de cada
            // pocas tandas. El minimo es la cota del coste real; el ruido solo suma.
            double best = 1e30;
            for (int rep = 0; rep < 5; ++rep) {
                const auto t0 = std::chrono::high_resolution_clock::now();
                if (Context* c = g_dev->beginFrame()) {
                    c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
                    for (int k = 0; k < kNodes; ++k) c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
                    FenceHandle f = c->signalFence();
                    g_dev->endFrame();
                    if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(f, 10000000000ull); c2->deleteFence(f); g_dev->endFrame(); }
                }
                best = std::min(best, std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() - t0).count());
            }
            return best / (double)kNodes;
        };

        std::printf("\n    nivel 18 · %u texeles por nodo · reparto del coste\n", (unsigned)count);
        std::printf("      nodos por tanda   ms/nodo TOTAL   ms/nodo SIN relieve   relieve\n");
        double perFull1 = 0.0, perFull256 = 0.0, perBare256 = 0.0;
        for (int k : { 1, 4, 16, 64, 256 }) {
            const double full = run(k, 0);
            const double bare = run(k, 2);
            std::printf("      %13d   %13.4f   %19.4f   %6.1f %%\n",
                        k, full, bare, full > 0.0 ? 100.0 * (full - bare) / full : 0.0);
            if (k == 1)   perFull1   = full;
            if (k == 256) { perFull256 = full; perBare256 = bare; }
        }
        up.grid[2] = 0;
        const double fixedMs = perFull1 - perFull256;      // lo que se ahorra al amortizar la tanda
        std::printf("      -> coste FIJO por dispatch %.4f ms/nodo · relieve %.4f · resto %.4f\n",
                    fixedMs > 0.0 ? fixedMs : 0.0, perFull256 - perBare256, perBare256);

        CHECK(perFull256 > 0.0, "la medida amortizada existe");
        CHECK(perBare256 < perFull256,
              "CONTRAPRUEBA: sin evaluar el relieve cuesta MENOS (el modo de biseccion hace algo)");
    }

    g_dev->destroy(out); g_dev->destroy(ubo); g_dev->destroy(cp);
}


// ================================================================================================
// F2 — EL CICLO COMPLETO: seleccionar -> pedir al pool -> generar en GPU -> publicar
//
// Es la primera vez que las piezas del v5 corren JUNTAS. Cada una estaba probada por separado
// (direccionamiento exacto, compute que coincide con la CPU, pool con LRU y presupuesto), y lo que
// esto verifica es lo único que ninguna podía: que el hueco que el pool asigna es el hueco que la
// GPU llena. Un desfase ahí no rompe nada visible en los tests unitarios y en pantalla sale como un
// trozo de terreno de OTRO SITIO.
// ================================================================================================
void testTerrainNodePoolGpu()
{
    BEGIN("v5 F2: ciclo completo selector -> pool -> compute -> publicacion");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const size_t kCap = 256;

    Haruka::World::Terrain::TerrainNodeGpu gpu;
    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/terrain_node.comp";
    if (!gpu.init(g_dev, cs.c_str(), kCap)) { CHECK(false, "TerrainNodeGpu::init"); return; }
    std::printf("    pool de %zu huecos = %.1f MB en VRAM (%.1f KB por nodo)\n",
                gpu.capacity(), gpu.bytes() / (1024.0 * 1024.0),
                Haruka::World::Terrain::TerrainNodeGpu::kBytesPerNode / 1024.0);

    Haruka::World::Terrain::TerrainNodePool pool(kCap, 32);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / 1080.0;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, 1920.0 / 1080.0);
    const glm::dvec3 camDir = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 fwd    = glm::normalize(glm::cross(camDir, glm::dvec3(0, 1, 0)));

    // Varios frames: es como se comporta de verdad — el presupuesto reparte la carga.
    std::vector<Haruka::World::Terrain::NodeId> sel;
    size_t totalIssued = 0;
    for (int frame = 0; frame < 6; ++frame) {
        const glm::dvec3 cam = pc + camDir * (R + 2.0) + fwd * (48.0 * frame);
        pool.beginFrame();
        Haruka::World::Terrain::nodeSelectVisible(R, cam, pc, radPerPx, sel, 4096, 8.0, &fwd, cone, 5000.0,
                          &Haruka::World::Terrain::TerrainNodePool::rangeFnAdapter, &pool);
        for (const Haruka::World::Terrain::NodeId& n : sel) pool.request(n);
        // ⚠️ EL `endFrame` VA AQUI AHORA, Y ES EXPLICITO A PROPOSITO. `generatePending` lo hacia por
        // dentro, y en GL `endFrame` es `SDL_GL_SwapWindow`: el generador presentaba la ventana a
        // media escena cada vez que habia nodos que generar. En el juego eso era el parpadeo; aqui
        // era lo que hacia visible el resultado al readback, asi que se conserva, a la vista.
        totalIssued += gpu.generatePending(g_dev->beginFrame(), pool, R);
        g_dev->endFrame();
    }
    const auto st = pool.stats();
    std::printf("    6 frames · %zu nodos dispatchados · residentes %zu/%zu · desalojos %zu\n",
                totalIssued, st.resident, st.capacity, st.evicted);
    CHECK(totalIssued > 0, "se han generado nodos en GPU");
    CHECK(st.resident <= kCap, "el pool nunca excede su capacidad");

    // ── LO QUE SOLO ESTE TEST PUEDE VER: ¿está el nodo EN SU HUECO? ─────────────────────────────
    // Se descarga el pool entero y se compara cada hueco residente contra la referencia de CPU del
    // nodo que el pool DICE tener ahí. Si el índice se desfasara, el contenido sería el de otro nodo
    // y la diferencia saldría enorme — no un ulp.
    BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, gpu.bytes(), nullptr,
                                               BufferMemory::Readback);
    if (!valid(rb)) { CHECK(false, "buffer de readback creado"); gpu.shutdown(); return; }
    FenceHandle f{};
    if (Context* c = g_dev->beginFrame()) {
        g_dev->copyBuffer(gpu.heights(), rb, 0, 0, gpu.bytes());
        c->memoryBarrier();
        f = c->signalFence();
        g_dev->endFrame();
        if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(f, 10000000000ull); c2->deleteFence(f); g_dev->endFrame(); }
    }
    const float* all = (const float*)g_dev->mappedData(rb);
    CHECK(all != nullptr, "el pool es legible");
    if (all) {
        // Se comprueban unos cuantos huecos, no los 256: cada uno cuesta 16 641 evaluaciones de
        // ruido en CPU y lo que se audita es el INDEXADO, que falla igual con 8 que con 256.
        std::vector<float> ref(Haruka::World::Terrain::TerrainNodeGpu::kTexelsPerNode);
        int checked = 0, wrong = 0; double worst = 0.0;
        for (size_t slot = 0; slot < kCap && checked < 8; ++slot) {
            Haruka::World::Terrain::NodeId n; if (!pool.nodeAtSlot((int)slot, n)) continue;
            Haruka::World::Terrain::nodeFillHeights(n, R, ref.data());
            // ⚠️ `kFloatsPerNode`, no `kTexelsPerNode`: cada hueco guarda DOS mapas (el propio y el
            // del padre, para el geomorph). Con el índice viejo se leía el mapa del padre del hueco
            // anterior y la auditoría lo cantaba como "contenido ajeno".
            const float* got = all + slot * Haruka::World::Terrain::TerrainNodeGpu::kFloatsPerNode;
            double d = 0.0;
            for (size_t k = 0; k < Haruka::World::Terrain::TerrainNodeGpu::kTexelsPerNode; ++k)
                d = std::max(d, std::abs((double)got[k] - (double)ref[k]));
            worst = std::max(worst, d);
            if (d > 1.0) ++wrong;      // 1 m: muy por encima del ruido GPU/CPU (0,022 m medido) y
            ++checked;                 // muy por debajo de lo que daría un nodo equivocado
        }
        std::printf("    %d huecos auditados contra la referencia de CPU · %d con contenido AJENO · peor %.4f m\n",
                    checked, wrong, worst);
        CHECK(checked > 0, "hay huecos residentes que auditar");
        CHECK(wrong == 0, "cada hueco contiene EL nodo que el pool dice (el indexado no se desfasa)");
        CHECK(worst < 0.05, "y su contenido cabe en la tolerancia GPU<->CPU declarada");
    }
    g_dev->destroy(rb);
    gpu.shutdown();
}


void testTerrainBakeAcrossLevels()
{
    BEGIN("v5 F1: el bake contra la CPU, NIVEL A NIVEL (¿escala la divergencia?)");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const uint32_t N = Haruka::World::Terrain::TERRAIN_NODE_TEXELS;

    Haruka::World::Terrain::TerrainNodeGpu gpu;
    const std::string base = Haruka::Renderer::Shader::baseDir();
    if (!gpu.init(g_dev, (base + "shaders/terrain_node.comp").c_str(), 64)) {
        CHECK(false, "TerrainNodeGpu::init"); return;
    }
    std::printf("    nivel   texel      peor       media     (bake GPU vs referencia CPU)\n");
    double worstAll = 0.0, worstFine = 0.0;
    for (uint32_t lvl : { 9u, 11u, 13u, 15u, 17u }) {
        // El mismo punto del planeta a cada nivel: el nodo de nivel 17 dividido por potencias de dos.
        const Haruka::World::Terrain::NodeId node{ Haruka::Tools::PlanetFace::FRONT, lvl, 33600u >> (17 - lvl), 24800u >> (17 - lvl) };
        Haruka::World::Terrain::TerrainNodePool pool(64, 8);
        pool.beginFrame();
        pool.request(node);
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
        if (slot < 0) { CHECK(false, "el nodo del nivel esta residente"); continue; }

        std::vector<float> ref((size_t)N * N);
        Haruka::World::Terrain::nodeFillHeights(node, R, ref.data());
        BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, ref.size() * sizeof(float),
                                              nullptr, BufferMemory::Readback);
        copyThenWait(gpu.heights(), rb, (size_t)slot * Haruka::World::Terrain::TerrainNodeGpu::kBytesPerNode,
                     ref.size() * sizeof(float));
        const float* got = (const float*)g_dev->mappedData(rb);
        double worst = 0.0, sum = 0.0;
        if (got) {
            for (size_t k = 0; k < ref.size(); ++k) {
                const double e = std::fabs((double)got[k] - (double)ref[k]);
                worst = std::max(worst, e); sum += e;
            }
        }
        std::printf("    %5u  %7.3f m  %8.4f m  %8.4f m\n", lvl, Haruka::World::Terrain::nodeTexelM(node, R),
                    worst, sum / (double)ref.size());
        worstAll = std::max(worstAll, worst);
        if (lvl == 17u) worstFine = worst;
        g_dev->destroy(rb);
    }
    // ⚠️ SIN NUMERO FIJO: lo que se afirma es que el bake CASA con su gemelo de CPU en TODOS los
    // niveles, no solo en el que se miraba. La tolerancia es la del test que ya existia (0,05 m).
    std::printf("    peor de todos los niveles: %.4f m · solo del 17: %.4f m\n", worstAll, worstFine);
    CHECK(worstAll < 0.05, "el bake casa con la CPU en TODOS los niveles, no solo en el 17");
    gpu.shutdown();
}


// ================================================================================================
// ¿EN QUE ETAPA DEL BAKE SE PIERDE? La biseccion que faltaba, sobre los 16 641 TEXELES.
//
// ⚠️ LO QUE HABIA NO PODIA CONTESTAR ESTO, POR DOS MOTIVOS INDEPENDIENTES:
//
//   · `octave_probe.comp` mira UN SOLO PUNTO. Da 1,5e-5 m en las dos GPU y pasa — pero el bake, en
//     la misma AMD, tiene una MEDIA de 0,003 m sobre el nodo entero. Un punto de 16 641 no es una
//     muestra, es una anecdota, y esa anecdota decia "todo casa" mientras el nodo no casaba.
//   · Los modos de biseccion 1-3 del propio `terrain_node.comp` emiten floats de `lx` y `dir.x`,
//     cuyo ulp vale 0,76 m de superficie. Se escribieron cuando el sintoma eran METROS. Hoy el
//     sintoma son 0,02 m y esos modos son ciegos: habrian dicho "casa" con toda seguridad.
//
// Aqui se usan los modos 6 y 7 (residuo sub-float amplificado, ver el shader) para preguntar por
// SEPARADO y en TODOS los texeles: ¿es identica la direccion? ¿lo es la posicion `dir·R`? Y luego
// la altura, con su histograma — porque "peor 0,02 · media 0,003" no dice lo mismo si es un texel
// suelto (un umbral cruzado) que si son los 16 641 (precision).
// ================================================================================================
void testBakeStageBisect()
{
    BEGIN("v5 F1: ¿en QUE etapa del bake diverge la GPU? (biseccion por texel)");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const uint32_t N = Haruka::World::Terrain::TERRAIN_NODE_TEXELS;
    const size_t   T = (size_t)N * N;

    Haruka::World::Terrain::TerrainNodeGpu gpu;
    if (!gpu.init(g_dev, (Haruka::Renderer::Shader::baseDir() + "shaders/terrain_node.comp").c_str(), 8)) {
        CHECK(false, "TerrainNodeGpu::init"); return;
    }

    // Genera el nodo con el modo pedido y devuelve el heightmap leido. Pool NUEVO cada vez: el
    // contenido se cachea por hueco, asi que reutilizarlo devolveria el bake del modo anterior.
    auto bake = [&](const Haruka::World::Terrain::NodeId& node, int mode, std::vector<float>& out) -> bool {
        gpu.setDebugMode(mode);
        Haruka::World::Terrain::TerrainNodePool pool(8, 4);
        pool.beginFrame();
        pool.request(node);
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
        if (slot < 0) return false;
        out.assign(T, 0.0f);
        BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, T * sizeof(float), nullptr,
                                              BufferMemory::Readback);
        copyThenWait(gpu.heights(), rb, (size_t)slot * Haruka::World::Terrain::TerrainNodeGpu::kBytesPerNode,
                     T * sizeof(float));
        const float* got = (const float*)g_dev->mappedData(rb);
        const bool ok = (got != nullptr);
        if (ok) std::memcpy(out.data(), got, T * sizeof(float));
        g_dev->destroy(rb);
        return ok;
    };

    bool dirOk = true, posOk = true;
    for (uint32_t lvl : { 9u, 17u }) {
        const Haruka::World::Terrain::NodeId node{ Haruka::Tools::PlanetFace::FRONT, lvl, 33600u >> (17 - lvl), 24800u >> (17 - lvl) };
        std::printf("    ── nivel %u (texel %.3f m) ─────────────────────────────\n",
                    lvl, Haruka::World::Terrain::nodeTexelM(node, R));

        // ── ETAPA 1: la DIRECCION, hasta el ultimo bit del double ────────────────────────────────
        std::vector<float> g6;
        if (bake(node, 6, g6)) {
            double worst = 0.0, sum = 0.0;
            for (uint32_t v = 0; v < N; ++v)
                for (uint32_t u = 0; u < N; ++u) {
                    const glm::dvec3 d = Haruka::World::Terrain::nodeTexelDir(node, u, v);
                    const double ref = (d.x - (double)(float)d.x) * 1.0e9;
                    const double e = std::fabs((double)g6[(size_t)v * N + u] - ref);
                    worst = std::max(worst, e); sum += e;
                }
            // El residuo esta en unidades de 1e-9 de direccion; a radio terrestre, 1 unidad = 6,4 mm.
            std::printf("      dir.x  (residuo sub-float) peor %.4f · media %.4f  [1 ud = %.4f m]\n",
                        worst, sum / (double)T, 1.0e-9 * R);
            if (worst > 0.01) dirOk = false;
        } else CHECK(false, "el nodo del modo 6 esta residente");

        // ── ETAPA 2: la POSICION `dir·R`, en metros ──────────────────────────────────────────────
        std::vector<float> g7;
        if (bake(node, 7, g7)) {
            double worst = 0.0, sum = 0.0;
            for (uint32_t v = 0; v < N; ++v)
                for (uint32_t u = 0; u < N; ++u) {
                    const double px  = Haruka::World::Terrain::nodeTexelDir(node, u, v).x * R;
                    const double ref = px - (double)(float)px;
                    const double e = std::fabs((double)g7[(size_t)v * N + u] - ref);
                    worst = std::max(worst, e); sum += e;
                }
            std::printf("      dir.x*R (residuo, metros)  peor %.9f m · media %.9f m\n",
                        worst, sum / (double)T);
            if (worst > 1.0e-6) posOk = false;
        } else CHECK(false, "el nodo del modo 7 esta residente");

        // ── ETAPA 3: la ALTURA, con su forma espacial ────────────────────────────────────────────
        std::vector<float> g0;
        std::vector<float> ref(T);
        Haruka::World::Terrain::nodeFillHeights(node, R, ref.data());
        if (bake(node, 0, g0)) {
            double worst = 0.0, sum = 0.0; uint32_t wu = 0, wv = 0;
            size_t over1mm = 0, over1cm = 0;
            for (uint32_t v = 0; v < N; ++v)
                for (uint32_t u = 0; u < N; ++u) {
                    const size_t k = (size_t)v * N + u;
                    const double e = std::fabs((double)g0[k] - (double)ref[k]);
                    if (e > worst) { worst = e; wu = u; wv = v; }
                    sum += e;
                    if (e > 0.001) ++over1mm;
                    if (e > 0.01)  ++over1cm;
                }
            std::printf("      altura                     peor %.4f m en (%u,%u) · media %.4f m\n",
                        worst, wu, wv, sum / (double)T);
            // ⚠️ EL HISTOGRAMA ES EL DIAGNOSTICO, NO EL PEOR. Un peor alto con casi ningun texel por
            // encima del milimetro seria un UMBRAL cruzado en un punto; miles de texeles por encima
            // es PRECISION perdida en todas partes. Son dos bugs distintos y el "peor" no los separa.
            std::printf("      texeles > 1 mm: %zu de %zu (%.1f %%) · > 1 cm: %zu (%.1f %%)\n",
                        over1mm, T, 100.0 * (double)over1mm / (double)T,
                        over1cm, 100.0 * (double)over1cm / (double)T);
        } else CHECK(false, "el nodo del modo 0 esta residente");
    }

    // ⚠️ ESTAS DOS ASERCIONES SON LAS QUE LOCALIZAN EL BUG, Y POR ESO SON DURAS. Si la direccion y la
    // posicion casan bit a bit con la CPU y la ALTURA no, entonces lo que diverge esta AGUAS ABAJO
    // —dentro de `harukaTerrainDetail`, que es hash entero y lerps escritos a mano en float— y eso
    // acota el problema a una sola funcion. Si alguna de las dos falla, el problema es la geometria.
    CHECK(dirOk, "la direccion del texel es la MISMA que en la CPU, hasta el ulp del double");
    CHECK(posOk, "la posicion `dir*R` es la MISMA que en la CPU, hasta el ulp del double");
    gpu.shutdown();
}


// ================================================================================================
// ¿QUE OCTAVA DIVERGE? Localiza el bug CRITICO de "el suelo depende de la GPU".
//
// `testTerrainBakeAcrossLevels` dice CUANTO (0,02 m en AMD, 0,0001 en NVIDIA, plano en todo nivel).
// Esto dice DONDE: emite cada octava por separado y su fraccion de celda, y lo compara con el gemelo
// de CPU. Si solo divergen las octavas finas -> precision de la posicion. Si divergen todas por
// igual -> el hash o la interpolacion.
// ================================================================================================
void testOctaveDivergence()
{
    BEGIN("¿que octava del ruido diverge entre GPUs? (bug critico del suelo)");

    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/tests/octave_probe.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de octave_probe.comp creado");
    if (!valid(cp)) return;

    const size_t N = 17;
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, N * sizeof(float), nullptr,
                                           BufferMemory::Static);
    BufferHandle rb  = g_dev->createBuffer(BufferUsage::Storage, N * sizeof(float), nullptr,
                                           BufferMemory::Readback);
    if (Context* ctx = g_dev->beginFrame()) {
        ctx->bindPipeline(cp);
        ctx->bindStorageBuffer(0, out);
        ctx->dispatch(1, 1, 1);
        ctx->memoryBarrier();
        g_dev->endFrame();
    }
    copyThenWait(out, rb, 0, N * sizeof(float));
    const float* v = (const float*)g_dev->mappedData(rb);
    if (!v) { CHECK(false, "readback"); g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(cp); return; }

    // El gemelo de CPU, con la MISMA direccion y las MISMAS frecuencias.
    const glm::dvec3 dir = glm::normalize(glm::dvec3(0.31, 0.62, 0.72));
    const glm::dvec3 p   = dir * 6371000.0;
    const double freq[7] = { 0.0000833, 0.0001667, 0.00035, 0.0016, 0.0090, 0.0450, 0.2200 };
    const double amp[7]  = { 969.3, 513.4, 260.0, 70.0, 14.0, 3.0, 0.7 };
    std::printf("    oct  frecuencia   ruido GPU   ruido CPU     delta     x amplitud\n");
    double worstM = 0.0; int worstOct = -1;
    for (int i = 0; i < 7; ++i) {
        const glm::dvec3 x = p * freq[i];
        const double cpu = Haruka::World::Terrain::detailNoise(x);
        const double d   = std::fabs((double)v[i] - cpu);
        const double dm  = d * amp[i];
        if (dm > worstM) { worstM = dm; worstOct = i; }
        std::printf("    %3d  %10.7f  %10.7f  %10.7f  %9.2e  %8.4f m\n",
                    i, freq[i], (double)v[i], cpu, d, dm);
    }
    std::printf("    |p| - R en la GPU: %.6f m  ·  peor octava: %d (%.4f m)\n",
                (double)v[14], worstOct, worstM);
    // El ultimo eslabon: la direccion calculada por `harukaCubeFaceToDir`, que es la que usa el bake.
    const glm::dvec3 d2  = Haruka::World::Terrain::cubeFaceToDir(Haruka::Tools::PlanetFace::FRONT, -0.4, 0.25);
    const double     cpu2 = Haruka::World::Terrain::terrainDetail(d2, 6371000.0, 0.596f);
    const double     dd   = std::fabs((double)v[15] - cpu2);
    std::printf("    con la direccion del BAKE (cubeFaceToDir): GPU %.6f m · CPU %.6f m · delta %.6f m\n",
                (double)v[15], cpu2, dd);
    std::printf("    (|dir| - 1) en ulps de double: %.2f\n", (double)v[16]);
    CHECK(dd < 0.001, "el detalle con la direccion del bake casa GPU<->CPU");
    // ⚠️ SIN TOLERANCIA INVENTADA: el ruido es la MISMA funcion en los dos lados, con la misma
    // entrada en double. Cualquier diferencia por encima del ulp de un float ya es el bug.
    CHECK(worstM < 0.001, "el ruido de la GPU casa con el de la CPU en TODAS las octavas");
    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(cp);
}


// Diagnóstico directo del pase completo: `init` falló en el juego y hay que saber en qué paso.
void testTerrainNodeRendererInit()
{
    BEGIN("v5 F3: init del pase de nodos (diagnostico)");
    using namespace Haruka::World::Terrain;
    const std::string dir = Haruka::Renderer::Shader::baseDir() + "shaders/";
    for (size_t cap : { (size_t)64, (size_t)256, (size_t)1024 }) {
        Haruka::World::Terrain::TerrainNodeRenderer r;
        const bool ok = r.init(g_dev, dir, cap);
        std::printf("    capacidad %4zu (%6.1f MB de SSBO): init %s\n",
                    cap, (double)(cap * Haruka::World::Terrain::TerrainNodeGpu::kBytesPerNode) / 1048576.0,
                    ok ? "OK" : "FALLO");
        if (cap == 64) CHECK(ok, "el pase arranca con una capacidad pequena");
        r.shutdown();
    }
}




// ================================================================================================
// EL BAKE, QUE ES LO QUE LE DA CONTINENTES AL NODO.
//
// ⚠️ El generador calculaba SOLO `harukaTerrainDetail`, o sea `R + detalle`. El clipmap compone
// `R + baseH + detalle·atenuacion` con recorte al nivel del mar, y el bake llega a ±4 km: el pase de
// nodos estaba dibujando un planeta distinto —sin continentes, sin oceanos, sin costa— y a otra
// altitud, lo que pone un escalon en la transicion entre los dos.
//
// El test de paridad de F1 no lo veia porque corre SIN bake, o sea por la rama `uMisc.z == 0`. Este
// ata un campo base SINTETICO y conocido, y exige que GPU y CPU sigan siendo gemelos POR ESE CAMINO.
// ================================================================================================
// El campo sintetico que se sube a la GPU. El muestreo de CPU NO se reimplementa aqui: se usa
// `baseFieldHeightAt`, que es el gemelo de verdad que usara la fisica. Asi este test no valida una
// copia privada del test — valida la funcion del motor.
struct TestBaseField {
    int res = 0;                      // lado de la reticula = res+1 texeles
    std::vector<float> heights;       // [face][j][i], metros
};

static float g_testBaseElevM(const glm::dvec3& dir, void* ctx)
{
    const TestBaseField& bf = *(const TestBaseField*)ctx;
    return Haruka::World::Terrain::baseFieldHeightAt(bf.heights.data(), bf.res, dir);
}

void testTerrainNodeBaseField()
{
    BEGIN("v5 F1: el nodo incluye el BAKE (continentes), gemelo CPU<->GPU");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const Haruka::World::Terrain::NodeId node{ Haruka::Tools::PlanetFace::FRONT, 14, 4200, 3100 };
    const uint32_t N = Haruka::World::Terrain::TERRAIN_NODE_TEXELS;
    const size_t count = (size_t)N * N;

    // Campo base sintetico: una rampa de ±3 km, del orden del bake real (±4 km), con estructura en
    // las dos direcciones para que un error de indexado o de cara se note.
    TestBaseField bf; bf.res = 64;
    const int N1 = bf.res + 1;
    bf.heights.assign((size_t)6 * N1 * N1, 0.0f);
    std::vector<float> rgba((size_t)6 * N1 * N1 * 4, 0.0f);   // lo que ve la GPU: RGBA32F
    for (int f = 0; f < 6; ++f)
        for (int y = 0; y < N1; ++y)
            for (int x = 0; x < N1; ++x) {
                const double u = (double)x / bf.res, v = (double)y / bf.res;
                const float e = (float)(3000.0 * std::sin(u * 6.0 + f) * std::cos(v * 4.0 + f * 0.7));
                const size_t k = ((size_t)f * N1 * N1) + (size_t)y * N1 + x;
                bf.heights[k] = e;
                rgba[k * 4]   = e;
            }

    TextureDesc td;
    td.width = td.height = (uint32_t)N1; td.layers = 6;
    td.format = Format::RGBA32F; td.filter = Filter::Nearest; td.wrap = Wrap::ClampToEdge;
    td.initialData = rgba.data();
    TextureHandle tex = g_dev->createTexture(td);
    CHECK(valid(tex), "textura del campo base creada");
    if (!valid(tex)) return;

    Haruka::World::Terrain::TerrainNodePool pool(4);
    Haruka::World::Terrain::TerrainNodeGpu  gpu;
    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/terrain_node.comp";
    if (!gpu.init(g_dev, cs.c_str(), 4)) { CHECK(false, "init del generador"); return; }
    gpu.setBaseField(tex);

    pool.beginFrame(); pool.request(node);
    const size_t genOk = gpu.generatePending(g_dev->beginFrame(), pool, R);
    g_dev->endFrame();
    // Ver la nota de `issued` en F3: con la cadena de ancestros se generan varios, no uno.
    CHECK(genOk >= 1, "el nodo se genera con el bake atado");
    CHECK(pool.isResident(node), "y el nodo PEDIDO esta entre los generados");
    const int slot = pool.request(node).slot;

    std::vector<float> ref(count);
    Haruka::World::Terrain::nodeFillHeights(node, R, ref.data(), nullptr, &g_testBaseElevM, &bf);

    BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, count * sizeof(float), nullptr,
                                          BufferMemory::Readback);
    // ⚠️ FUERA de todo frame: `copyBuffer` hace `submitOneShot` y meterlo dentro de uno abierto
    // pierde el dispositivo (paso hoy, y envenenó los ~17 tests siguientes).
    copyThenWait(gpu.heights(), rb, (size_t)slot * Haruka::World::Terrain::TerrainNodeGpu::kBytesPerNode, count * sizeof(float));

    const float* got = (const float*)g_dev->mappedData(rb);
    if (got) {
        double worst = 0.0, refMin = 1e30, refMax = -1e30;
        for (size_t k = 0; k < count; ++k) {
            worst  = std::max(worst, (double)std::fabs(got[k] - ref[k]));
            refMin = std::min(refMin, (double)ref[k]);
            refMax = std::max(refMax, (double)ref[k]);
        }
        std::printf("    la referencia abarca %.1f .. %.1f m  ·  diferencia GPU<->CPU peor %.4f m\n",
                    refMin, refMax, worst);
        {
            CHECK(worst < 0.05, "GPU y CPU siguen siendo gemelos CON el bake (0,05 m declarados)");
        }


        // CONTRAPRUEBA: sin el bake, el mismo nodo tiene que salir MUY distinto. Si no, el campo
        // base no estaria llegando al shader y el test de arriba pasaria sin medir nada.
        std::vector<float> noBase(count);
        Haruka::World::Terrain::nodeFillHeights(node, R, noBase.data());
        double worstVsNoBase = 0.0;
        for (size_t k = 0; k < count; ++k)
            worstVsNoBase = std::max(worstVsNoBase, (double)std::fabs(ref[k] - noBase[k]));
        std::printf("    CONTRAPRUEBA: contra el mismo nodo SIN bake, hasta %.1f m de diferencia\n",
                    worstVsNoBase);
        CHECK(worstVsNoBase > 100.0, "CONTRAPRUEBA: el bake cambia el nodo de verdad (no es un no-op)");
    } else {
        CHECK(false, "readback del nodo");
    }

    g_dev->destroy(rb); g_dev->destroy(tex);
    gpu.shutdown();
}


// ================================================================================================
// BUG 9 — BISECCION: ¿el compute, o el ADAPTADOR? Los dos caminos, mismo nodo, misma ejecucion.
//
// El dato que abrio esto: para el MISMO nodo y el MISMO backend (OpenGL),
//
//     F1 (pipeline y UBO propios, dispatch directo)   0.024231 m   <- bien
//     F3 (via TerrainNodeGpu::generatePending)       399.5896 m    <- roto
//
// O sea que el compute de GL NO esta roto: lo que difiere es el adaptador. Este test corre los dos
// caminos seguidos, sobre el mismo nodo, y compara cada uno con la referencia de CPU **y entre si**.
// Si A casa y B no, el fallo esta en lo que B hace de mas: el anillo de UBOs por dispatch, el hueco
// del pool, o el numero de grupos de trabajo.
// ================================================================================================
void testNodeGpuAdapterBisect()
{
    BEGIN("bug 9: bisecar compute directo contra TerrainNodeGpu");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const Haruka::World::Terrain::NodeId node{ Haruka::Tools::PlanetFace::FRONT, 14, 4200, 3100 };   // el mismo de F1 y F3
    const uint32_t N = Haruka::World::Terrain::TERRAIN_NODE_TEXELS;
    const size_t count = (size_t)N * N;

    std::vector<float> ref(count);
    Haruka::World::Terrain::nodeFillHeights(node, R, ref.data());

    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/terrain_node.comp";

    // ── CAMINO A: dispatch directo, UBO propio (lo que hace F1) ─────────────────────────────────
    std::vector<float> a(count, -1.0f);
    {
        PipelineDesc pd; pd.computePath = cs.c_str();
        PipelineHandle cp = g_dev->createPipeline(pd);
        if (!valid(cp)) { CHECK(false, "pipeline directo"); return; }
        struct ParamsUBO { int32_t node[4]; int32_t grid[4]; float misc[4]; } up{};
        up.node[0] = (int32_t)node.face; up.node[1] = (int32_t)node.level;
        up.node[2] = (int32_t)node.i;    up.node[3] = (int32_t)node.j;
        up.grid[0] = (int32_t)N; up.grid[1] = (int32_t)Haruka::World::Terrain::TERRAIN_NODE_CELLS;
        up.grid[2] = 0; up.grid[3] = 0;                       // modo produccion, hueco 0
        up.misc[0] = (float)R; up.misc[1] = (float)Haruka::World::Terrain::nodeTexelM(node, R);
        BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(up), &up,
                                               BufferMemory::Dynamic);
        // ⚠️ EL PATRON DE LECTURA ES EL DE F1, Y NO ES INTERCAMBIABLE: SSBO de salida creado ya como
        // `Readback` (mapeo persistente) + fence + `mappedData` DIRECTO. Sin `copyBuffer`.
        //
        // Con `Storage/Dynamic` + `copyBuffer` + `mappedData` de otro buffer, en OpenGL se lee BASURA
        // (1,7e38): el destino nunca se llena. Ese era el patron de mi repro del bug 9 y el de
        // `testTerrainNodeBaseField` — o sea que sus dos veredictos sobre OpenGL median el readback,
        // no el shader.
        std::vector<float> sentinel(count, -1.0f);
        BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, count * sizeof(float),
                                               sentinel.data(), BufferMemory::Readback);
        FenceHandle f{};
        if (Context* c = g_dev->beginFrame()) {
            c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
            c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
            c->memoryBarrier();
            f = c->signalFence();
            g_dev->endFrame();
            if (Context* c2 = g_dev->beginFrame()) {
                c2->waitFence(f, 10000000000ull); c2->deleteFence(f); g_dev->endFrame();
            }
        }
        if (const float* g = (const float*)g_dev->mappedData(out)) std::memcpy(a.data(), g, count * sizeof(float));
        g_dev->destroy(out); g_dev->destroy(ubo); g_dev->destroy(cp);
    }

    // ── CAMINO B: el adaptador de verdad ────────────────────────────────────────────────────────
    std::vector<float> b(count, -1.0f);
    int slot = -1;
    {
        // Capacidad 1: el nodo cae en el hueco 0 y el buffer tiene el tamaño de UN nodo, igual que
        // el camino directo. Si con esto casa, el fallo esta en el desplazamiento por hueco.
        Haruka::World::Terrain::TerrainNodePool pool(1);
        Haruka::World::Terrain::TerrainNodeGpu  gpu;
        if (!gpu.init(g_dev, cs.c_str(), 1)) { CHECK(false, "init del adaptador"); return; }
        pool.beginFrame(); pool.request(node);
        gpu.generatePending(g_dev->beginFrame(), pool, R);
        g_dev->endFrame();
        slot = pool.request(node).slot;
        // ⚠️ FENCE ANTES DE COPIAR. `generatePending` acaba con `memoryBarrier` + `endFrame`, y eso
        // ordena los comandos, pero NO dice cuando la copia puede leer lo escrito. Sin esto, en
        // OpenGL salian 399 m de "divergencia" que eran datos a medio escribir.
        {
            FenceHandle fb{};
            if (Context* c = g_dev->beginFrame()) {
                c->memoryBarrier(); fb = c->signalFence();
                g_dev->endFrame();
                if (Context* c2 = g_dev->beginFrame()) {
                    c2->waitFence(fb, 10000000000ull); c2->deleteFence(fb); g_dev->endFrame();
                }
            }
        }
        BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, count * sizeof(float), nullptr,
                                              BufferMemory::Readback);
        g_dev->copyBuffer(gpu.heights(), rb, (size_t)slot * Haruka::World::Terrain::TerrainNodeGpu::kBytesPerNode, 0,
                          count * sizeof(float));
        // ⚠️ LA FENCE VA **DESPUES** DE LA COPIA, NO ANTES. Esta es LA causa de todo el enredo del
        // "bug 9": con la fence antes, `mappedData` leia el destino a medio llenar y salian 399 m de
        // divergencia contra la CPU — solo en OpenGL. La generacion SIEMPRE fue correcta.
        {
            FenceHandle fc{};
            if (Context* c = g_dev->beginFrame()) {
                c->memoryBarrier(); fc = c->signalFence();
                g_dev->endFrame();
                if (Context* c2 = g_dev->beginFrame()) {
                    c2->waitFence(fc, 10000000000ull); c2->deleteFence(fc); g_dev->endFrame();
                }
            }
        }
        if (const float* g = (const float*)g_dev->mappedData(rb)) std::memcpy(b.data(), g, count * sizeof(float));
        g_dev->destroy(rb);
        gpu.shutdown();
    }

    double wA = 0.0, wB = 0.0, wAB = 0.0;
    for (size_t k = 0; k < count; ++k) {
        wA  = std::max(wA,  (double)std::fabs(a[k] - ref[k]));
        wB  = std::max(wB,  (double)std::fabs(b[k] - ref[k]));
        wAB = std::max(wAB, (double)std::fabs(a[k] - b[k]));
    }
    std::printf("    el nodo cayo en el hueco %d\n", slot);
    std::printf("    A  dispatch DIRECTO   vs CPU: %10.4f m\n", wA);
    std::printf("    B  TerrainNodeGpu     vs CPU: %10.4f m\n", wB);
    std::printf("    A vs B (los dos caminos entre si): %10.4f m\n", wAB);

    CHECK(wA < 0.05, "el camino DIRECTO casa con la CPU (la tolerancia declarada)");
    CHECK(wB < 0.05, "el camino del ADAPTADOR casa con la CPU");
    CHECK(wAB < 0.05, "los dos caminos producen LO MISMO (si no, el fallo esta en el adaptador)");
}
