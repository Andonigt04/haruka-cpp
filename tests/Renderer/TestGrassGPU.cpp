#include "RHI/Common/RHITestCommon.h"

/**
 * @brief La ola sobre FORMAS que no son un planeta: un anillo cilindrico y un cubo.
 *
 * Responde a una pregunta de diseno con una medida en vez de con una opinion: ¿el mar depende de que
 * haya una esfera debajo? El modelo solo pide posicion, normal y profundidad, y construye su marco
 * tangente del propio `up` — asi que deberia valer para cualquier superficie. Aqui se dibuja sobre
 * dos que no tienen nada que ver con un planeta.
 *
 * El CUBO es el caso duro a proposito: en una arista la normal salta de golpe y con ella el marco
 * tangente. Si la ola dependiera del marco de manera inestable, ahi se veria una costura.
 *
 * Se mide cobertura y color medio en los dos backends. La imagen queda ademas guardada en `g_shots`
 * para que el careo GL<->Vulkan de `compareBackends()` la incluya como una escena mas.
 */
// ═══════════════════════════════════════════════════════════════════════════════════════════════
// HIERBA DENSA: generada en GPU sobre los nodos dibujados, aplastada por el mapa de presion.
// ═══════════════════════════════════════════════════════════════════════════════════════════════
void testGrassGen()
{
    BEGIN("hierba: la GPU genera briznas sobre el suelo dibujado, las recorta al frustum y las aplasta");

    using namespace Haruka::World::Terrain;
    using Haruka::Renderer::GrassRenderer;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    Haruka::World::Terrain::TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 256)) { CHECK(false, "init del pase de nodos"); return; }
    Haruka::Renderer::GrassRenderer grass;
    if (!grass.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/")) { CHECK(false, "init de la hierba"); return; }

    // Suelo: SIN bake (ni equirect ni campo del cubo), o sea solo el relieve procedural sobre R —
    // el mismo camino que `nodeFillHeights` sin `baseFn`. Asi el muestreador de CPU que publica los
    // rangos y el mapa que genera la GPU describen el MISMO suelo (±100 m de relieve, no plano), que
    // es lo que hace falta para que "apoyada en el mapa del nodo" signifique algo. El nivel del mar
    // se manda a −2 000 m para que la orilla no descarte nada.
    static const double s_R = R;
    static const float  s_floor = 0.0f;
    r.setHeightSampler([](const glm::dvec3& d, void*) {
        return Haruka::World::Terrain::terrainDetail(d, s_R, 0.6f);
    }, nullptr);

    // Tabla de materiales: UNO, bandas abiertas, y `g.w` = cuanta hierba (1 en la prueba, 0 en la contraprueba).
    struct GpuMat { float a[4], b[4], c[4], d[4], e[4], f[4], g[4]; };
    struct GpuTable { float count[4]; GpuMat mats[16]; };
    auto makeMat = [&](float grassAmt) {
        GpuTable t{};
        t.count[0] = 1.0f;
        t.mats[0].a[0] = 0.0f; t.mats[0].a[1] = 1.0f; t.mats[0].a[2] = -1000.0f; t.mats[0].a[3] = 1000.0f;
        t.mats[0].b[0] = 0.0f; t.mats[0].b[1] = 1.0f; t.mats[0].b[2] = -1.0f; t.mats[0].b[3] = 0.0f;
        t.mats[0].c[0] = t.mats[0].c[1] = t.mats[0].c[2] = 1.0f; t.mats[0].c[3] = 1.0f;
        t.mats[0].d[0] = 1.0f; t.mats[0].d[1] = 0.08f;
        t.mats[0].f[0] = -1000.0f; t.mats[0].f[1] = 1000.0f; t.mats[0].f[2] = 0.25f;
        t.mats[0].g[0] = -1000.0f; t.mats[0].g[1] = 1000.0f; t.mats[0].g[2] = 0.01f; t.mats[0].g[3] = grassAmt;
        return g_dev->createBuffer(BufferUsage::Uniform, sizeof(t), &t, BufferMemory::Dynamic);
    };
    BufferHandle matGrass = makeMat(1.0f), matBare = makeMat(0.0f);

    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 east = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    // Camara a 1,7 m sobre el suelo DIBUJADO (el bake + el relieve procedural, que aqui vale −130 m:
    // puesta sobre el bake a secas quedaba a 130 m del suelo y toda la hierba fuera del radio),
    // mirando al horizonte. La cota sale del mapa del nodo, leido de vuelta, tras calentar el pool.
    glm::dvec3 cam = pc + up0 * (R + (double)s_floor + 1.7);
    const glm::dvec3 fwd = east;
    const glm::dvec3 vup = up0;
    const float aspect = (float)w / (float)h, fCot = 1.0f / std::tan((float)fovY * 0.5f);
    glm::mat4 proj(0.0f); proj[0][0] = fCot / aspect; proj[1][1] = fCot; proj[2][3] = -1.0f; proj[3][2] = 1.0f;
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup));
    const glm::mat4 rotVP = proj * glm::mat4(glm::mat3(view));

    std::vector<Haruka::World::Terrain::TerrainNodeRenderer::NearNode> nodes;
    // Un frame: nodos (compute) + hierba (presion + compute) fuera del pase; terreno + hierba dentro.
    auto frame = [&](BufferHandle mat, const glm::dvec3& viewDir, bool drawGrass, float dt,
                     const std::vector<glm::dvec3>* stamps, const glm::vec3& stampVel, bool noNodes = false) {
        Context* ctx = g_dev->beginFrame();
        if (!ctx) return false;
        r.prepare(ctx, cam, pc, R, viewDir, radPerPx, cone);
        Haruka::Renderer::GrassRenderer::Frame gf;
        gf.finestTexelM = (float)r.nearNodes(cam, pc, R, (double)grass.config().radiusM, nodes);
        if (noNodes) nodes.clear();
        gf.nodes = &nodes; gf.camPos = cam; gf.planetCenter = pc; gf.planetRadiusM = R;
        gf.viewDir = glm::vec3(viewDir); gf.coneHalfAngle = (float)cone; gf.seaLevelM = -2000.0f;
        gf.viewUp = glm::vec3(up0); gf.tanHalfV = (float)std::tan(fovY * 0.5); gf.aspect = (float)w / (float)h;
        gf.sunDir = glm::vec3(up0); gf.wind = glm::vec3(0.0f); gf.time = 0.0f; gf.dt = dt;
        gf.heights = r.heightsBuffer(); gf.baseField = r.baseFieldOrDummy(); gf.materialUBO = mat;
        if (stamps) for (const glm::dvec3& sp : *stamps) grass.addStamp(sp, 0.5f, stampVel, 1.0f);
        grass.prepare(ctx, gf);
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
        cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f;
        ctx->beginRenderPass({}, cv);
        r.draw(ctx, cam, pc, R, rotVP);
        if (drawGrass) grass.draw(ctx, rotVP);
        ctx->endRenderPass();
        g_dev->endFrame(); pumpWindowEvents();
        return true;
    };
    // La bilineal en CPU del mapa del nodo, sobre una copia leida de vuelta: es la MISMA lectura que
    // hace el compute (`drawnHeightAt`), y sirve para poner la camara en el suelo y para auditar raices.
    const size_t N1 = Haruka::World::Terrain::TERRAIN_NODE_TEXELS, texels = N1 * N1;
    BufferHandle heightsRb = g_dev->createBuffer(BufferUsage::Storage, 256 * texels * sizeof(float), nullptr, BufferMemory::Readback);
    auto readHeights = [&]() -> const float* {
        FenceHandle fh{};
        if (Context* c = g_dev->beginFrame()) {
            g_dev->copyBuffer(r.heightsBuffer(), heightsRb, 0, 0, 256 * texels * sizeof(float));
            c->memoryBarrier(); fh = c->signalFence(); g_dev->endFrame();
            if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(fh, 10000000000ull); c2->deleteFence(fh); g_dev->endFrame(); }
        }
        return (const float*)g_dev->mappedData(heightsRb);
    };
    auto mapHeightAt = [&](const float* H, const glm::dvec3& dir, double& hOut) {
        Haruka::Tools::PlanetFace face; double lx, ly;
        Haruka::World::Terrain::dirToCubeFaceClosed(dir, face, lx, ly);
        const double fx = (lx + 1.0) * 0.5, fy = (ly + 1.0) * 0.5;
        for (const auto& nd : nodes) {
            if (nd.node[0] != (int)face) continue;
            const double cells = (double)(1u << nd.node[1]);
            if ((int)std::floor(fx * cells) != nd.node[2] || (int)std::floor(fy * cells) != nd.node[3]) continue;
            const double tu = (fx * cells - nd.node[2]) * Haruka::World::Terrain::TERRAIN_NODE_CELLS, tv = (fy * cells - nd.node[3]) * Haruka::World::Terrain::TERRAIN_NODE_CELLS;
            const double inv = 1.0 / (double)(1 << nd.slot[1]);
            const double au = std::clamp((nd.slot[2] * (double)Haruka::World::Terrain::TERRAIN_NODE_CELLS + tu) * inv, 0.0, (double)Haruka::World::Terrain::TERRAIN_NODE_CELLS);
            const double av = std::clamp((nd.slot[3] * (double)Haruka::World::Terrain::TERRAIN_NODE_CELLS + tv) * inv, 0.0, (double)Haruka::World::Terrain::TERRAIN_NODE_CELLS);
            const size_t base = (size_t)nd.slot[0] * texels;
            const int iu = (int)std::floor(au), iv = (int)std::floor(av);
            const int ju = std::min(iu + 1, (int)N1 - 1), jv = std::min(iv + 1, (int)N1 - 1);
            const double fu = au - iu, fv = av - iv;
            const double h00 = H[base + iv * N1 + iu], h10 = H[base + iv * N1 + ju], h01 = H[base + jv * N1 + iu], h11 = H[base + jv * N1 + ju];
            hOut = (h00 * (1 - fu) + h10 * fu) * (1 - fv) + (h01 * (1 - fu) + h11 * fu) * fv;
            return true;
        }
        return false;
    };
    // Calentar el pool: los nodos bajo la camara tienen que existir antes de pedirles altura. Dos
    // vueltas: la primera para saber donde esta el suelo, la segunda ya con la camara sobre el.
    for (int pass = 0; pass < 2; ++pass) {
        for (int f = 0; f < 10; ++f) frame(matGrass, fwd, false, 1.0f / 60.0f, nullptr, glm::vec3(0.0f));
        if (pass == 0) {
            double hGround = (double)s_floor;
            // Con radio ancho: la camara aun esta a decenas de metros del suelo real y la tabla de
            // 60 m (que mide contra el rango del nodo) no la tendria dentro.
            r.nearNodes(cam, pc, R, 600.0, nodes);
            const float* H = readHeights();
            const bool ok = H && mapHeightAt(H, up0, hGround);
            std::printf("    suelo dibujado bajo la camara: %s%.2f m (bake %.0f m)\n", ok ? "" : "SIN NODO, ", hGround, (double)s_floor);
            CHECK(ok, "el mapa del nodo da la cota del suelo bajo la camara");
            cam = pc + up0 * (R + hGround + 1.7);
        }
    }
    const auto st0 = r.stats();
    std::printf("    nodos dibujados %zu · en la tabla de la hierba %zu (texel mas fino %.2f m)\n",
                st0.drawn, nodes.size(), r.nearNodes(cam, pc, R, (double)grass.config().radiusM, nodes));
    CHECK(!nodes.empty(), "hay nodos hoja dibujados alrededor de la camara (la tabla de la hierba no esta vacia)");

    // ── 0. SE VE: la imagen con hierba difiere de la imagen sin ella ────────────────────────────
    // Es la comprobacion que no depende de ninguna lectura de vuelta: si el compute escribe y el
    // draw indirecto dibuja, hay pixeles distintos; si cualquiera de los dos falla en silencio, no.
    {
        auto capture = [&](bool withGrass, std::vector<uint8_t>& px, const std::vector<glm::dvec3>* stamps = nullptr) {
            px.assign((size_t)w * h * 4, 0);
            Context* ctx = g_dev->beginFrame();
            if (!ctx) return;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            if (stamps) for (const glm::dvec3& sp : *stamps) grass.addStamp(sp, 0.6f, glm::vec3(glm::cross(fwd, up0)) * 2.0f, 1.0f);
            Haruka::Renderer::GrassRenderer::Frame gf;
            gf.finestTexelM = (float)r.nearNodes(cam, pc, R, (double)grass.config().radiusM, nodes);
            gf.nodes = &nodes; gf.camPos = cam; gf.planetCenter = pc; gf.planetRadiusM = R;
            gf.viewDir = glm::vec3(fwd); gf.coneHalfAngle = (float)cone; gf.sunDir = glm::vec3(up0); gf.seaLevelM = -2000.0f;
            gf.viewUp = glm::vec3(up0); gf.tanHalfV = (float)std::tan(fovY * 0.5); gf.aspect = (float)w / (float)h;
            gf.heights = r.heightsBuffer(); gf.baseField = r.baseFieldOrDummy(); gf.materialUBO = matGrass;
            grass.prepare(ctx, gf);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, rotVP);
            if (withGrass) grass.draw(ctx, rotVP);
            ctx->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame(); pumpWindowEvents();
            if (isVk) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        };
        std::vector<uint8_t> a, b;
        capture(false, a); capture(true, b);
        // `HARUKA_GRASS_PNG=<dir>`: las dos imagenes, para MIRARLAS (fila 0 = abajo en GL, arriba en VK).
        if (const char* dir = std::getenv("HARUKA_GRASS_PNG")) {
            for (int k = 0; k < 2; ++k) {
                const std::vector<uint8_t>& px = k ? b : a;
                std::vector<uint8_t> img(px.size());
                for (int y = 0; y < h; ++y) {
                    const int src = isVk ? y : (h - 1 - y);
                    std::memcpy(&img[(size_t)y * w * 4], &px[(size_t)src * w * 4], (size_t)w * 4);
                }
                const std::string path = std::string(dir) + "/" + (isVk ? "vk_" : "gl_") + (k ? "hierba_con.png" : "hierba_sin.png");
                Haruka::IO::writePNG(path, w, h, 4, img.data());
                std::printf("      -> %s\n", path.c_str());
            }
            // Y una PISADA: un rastro de sellos que CRUZA por delante de la camara (de izquierda a
            // derecha, a 5 m), por el suelo dibujado. Cruzando y no alejandose: una brizna tumbada
            // en la direccion de la vista se ve como una raya; tumbada de lado se ve tumbada.
            std::vector<glm::dvec3> trail;
            const float* H = readHeights();
            const glm::dvec3 right = glm::normalize(glm::cross(fwd, up0));
            for (double d = -7.0; d < 7.0; d += 0.6) {
                const glm::dvec3 p = cam + fwd * 5.0 + right * d;
                double hg = 0.0; const glm::dvec3 dirP = glm::normalize(p - pc);
                if (H && mapHeightAt(H, dirP, hg)) trail.push_back(pc + dirP * (R + hg));
            }
            std::vector<uint8_t> c;
            for (int f = 0; f < 3; ++f) capture(true, c, &trail);
            std::vector<uint8_t> img(c.size());
            for (int y = 0; y < h; ++y) {
                const int src = isVk ? y : (h - 1 - y);
                std::memcpy(&img[(size_t)y * w * 4], &c[(size_t)src * w * 4], (size_t)w * 4);
            }
            const std::string path = std::string(dir) + "/" + (isVk ? "vk_" : "gl_") + "hierba_pisada.png";
            Haruka::IO::writePNG(path, w, h, 4, img.data());
            std::printf("      -> %s (%zu sellos)\n", path.c_str(), trail.size());
        }
        size_t diff = 0, lowerDiff = 0;
        for (size_t i = 0; i + 4 <= a.size(); i += 4) {
            const int d = std::abs((int)a[i] - b[i]) + std::abs((int)a[i + 1] - b[i + 1]) + std::abs((int)a[i + 2] - b[i + 2]);
            if (d > 24) {
                ++diff;
                const size_t row = (i / 4) / (size_t)w;
                const bool bottom = isVk ? (row >= (size_t)h / 2) : (row < (size_t)h / 2);   // fila 0 = abajo en GL, arriba en VK
                if (bottom) ++lowerDiff;
            }
        }
        std::printf("    SE VE: %zu de %d pixeles cambian con la hierba (%.1f %%), %zu en la mitad inferior\n",
                    diff, w * h, 100.0 * diff / (w * h), lowerDiff);
        CHECK(diff > (size_t)(w * h) / 50, "la hierba cambia mas del 2 %% de los pixeles de la imagen");
    }

    // ── 1. GENERACION: cuantas y donde ──────────────────────────────────────────────────────────
    std::vector<float> bl;
    frame(matGrass, fwd, true, 1.0f / 60.0f, nullptr, glm::vec3(0.0f));
    const size_t nGrass = grass.readBack(4096, bl);
    const double areaM2 = 3.14159 * grass.config().radiusM * grass.config().radiusM;
    std::printf("    briznas: %zu de %zu hilos (%.1f por m² de disco, cono de %.0f°)\n",
                nGrass, grass.lastDispatched(), (double)nGrass / areaM2, cone * 180.0 / 3.14159);
    CHECK(nGrass > 2000, "con material de hierba se generan briznas (mas de 2 000)");
    CHECK(nGrass < grass.config().maxBlades, "y no se desborda el tope del buffer");
    // Cada brizna leida: dentro del radio, por delante de la camara (cono), normal sana, altura sana.
    size_t bad = 0, farN = 0, behind = 0, badUp = 0;
    double maxAbove = -1e9, minBelow = 1e9;   // cota de la raiz respecto al bake, en metros
    const float cosCone = std::cos((float)cone + 0.15f);
    for (size_t i = 0; i + 8 <= bl.size(); i += 8) {
        const glm::vec3 p(bl[i], bl[i + 1], bl[i + 2]);
        const glm::vec3 n(bl[i + 4], bl[i + 5], bl[i + 6]);
        const float dist = glm::length(p);
        const bool inRadius = dist <= grass.config().radiusM + 0.01f;
        const bool inCone   = dist <= 3.0f || glm::dot(p / std::max(dist, 1e-3f), glm::vec3(fwd)) >= cosCone - 0.02f;
        const bool upOk     = std::abs(glm::length(n) - 1.0f) < 0.05f && glm::dot(n, glm::vec3(up0)) > 0.7f;
        if (!inRadius) ++farN; if (!inCone) ++behind; if (!upOk) ++badUp;
        if (!inRadius || !inCone || !upOk || !(bl[i + 3] > 0.05f && bl[i + 3] < 2.0f)) ++bad;
        // Cota de la raiz sobre el bake (50 m): el relieve procedural es de metros, la raiz va 4 cm bajo el mapa.
        const double hRoot = glm::length(glm::dvec3(p) + cam - pc) - R;
        maxAbove = std::max(maxAbove, hRoot - (double)s_floor); minBelow = std::min(minBelow, hRoot - (double)s_floor);
    }
    std::printf("    muestra de %zu: fuera del radio %zu · fuera del cono %zu · normal rara %zu · cota de raiz sobre el bake [%.2f, %.2f] m\n",
                bl.size() / 8, farN, behind, badUp, minBelow, maxAbove);
    CHECK(bl.size() >= 8 * 100, "se leen briznas de vuelta");
    CHECK(bad == 0, "todas las briznas leidas estan en el radio, en el cono, con normal unitaria hacia arriba y altura sana");

    // ── 2. LAS RAICES ESTAN EN EL SUELO DIBUJADO ────────────────────────────────────────────────
    // El mapa del nodo se lee de vuelta y se compara, brizna a brizna, con la bilineal en CPU del
    // MISMO mapa (la raiz va 4 cm por debajo). Si la busqueda del nodo o el sub-rectangulo del
    // ancestro estuvieran mal, la raiz caeria a metros del suelo, no a centimetros.
    {
        double worst = 0.0; size_t tested = 0, missing = 0;
        const float* H = readHeights();
        for (size_t i = 0; H && i + 8 <= bl.size() && tested < 512; i += 8) {
            const glm::dvec3 pW = glm::dvec3(bl[i], bl[i + 1], bl[i + 2]) + cam - pc;
            double hMap = 0.0;
            if (!mapHeightAt(H, glm::normalize(pW), hMap)) { ++missing; continue; }
            worst = std::max(worst, std::abs((glm::length(pW) - R) - (hMap - 0.04)));
            ++tested;
        }
        std::printf("    raiz vs mapa del nodo (bilineal en CPU del MISMO mapa): %zu briznas · peor %.4f m · sin nodo %zu\n", tested, worst, missing);
        CHECK(tested >= 100, "se comprobaron al menos 100 raices contra el mapa");
        CHECK(worst < 0.02, "la raiz esta a menos de 2 cm de donde el mapa del nodo pone el suelo (menos 4 cm de hundimiento)");
    }

    // ── 3. CONTRAPRUEBAS ────────────────────────────────────────────────────────────────────────
    frame(matBare, fwd, true, 1.0f / 60.0f, nullptr, glm::vec3(0.0f));
    const size_t nBare = grass.readBack(0, bl);
    std::printf("    CONTRAPRUEBA material sin hierba (g.w = 0): %zu briznas\n", nBare);
    CHECK(nBare == 0, "con `grass = 0` en el material no se genera ninguna brizna");
    frame(matGrass, fwd, true, 1.0f / 60.0f, nullptr, glm::vec3(0.0f), true);
    const size_t nNoNodes = grass.readBack(0, bl);
    std::printf("    CONTRAPRUEBA sin tabla de nodos: %zu briznas\n", nNoNodes);
    CHECK(nNoNodes == 0, "sin nodos dibujados no hay suelo donde apoyarse: cero briznas");
    // Mirando hacia atras, el cono recorta las de delante: la cuenta cae a las de los 3 m de gracia.
    frame(matGrass, -fwd, true, 1.0f / 60.0f, nullptr, glm::vec3(0.0f));
    const size_t nBack = grass.readBack(0, bl);
    frame(matGrass, fwd, true, 1.0f / 60.0f, nullptr, glm::vec3(0.0f));
    const size_t nFwd = grass.readBack(4096, bl);
    std::printf("    CONTRAPRUEBA del cono: mirando al este %zu · al oeste %zu (conjuntos distintos, tamano parecido)\n", nFwd, nBack);
    CHECK(nBack > 1000 && (double)nBack < 1.5 * (double)nFwd && (double)nFwd < 1.5 * (double)nBack,
          "girar la camara 180 grados da otro conjunto del mismo tamano (el cono recorta, no la densidad)");

    // ── 4. LA PRESION: un sello tumba la hierba y se levanta sola ──────────────────────────────
    // Se estampa en el pie de la camara con velocidad E+N (asi las dos componentes del mapa son
    // positivas y se leen en un backbuffer RGBA8) y se muestrea el mapa en su centro con el
    // pipeline de muestreo del banco.
    {
        glm::dvec3 anchor, E, N; grass.pressureFrame(anchor, E, N);
        const std::vector<glm::dvec3> foot = { cam - up0 * 1.7 };
        const glm::vec3 vel = glm::vec3(E + N) * 2.0f;
        frame(matGrass, fwd, true, 1.0f / 60.0f, &foot, vel);
        const std::string vs = Haruka::Renderer::Shader::baseDir() + "shaders/tests/rhitest_fullscreen.vert";
        const std::string fs = Haruka::Renderer::Shader::baseDir() + "shaders/tests/rhitest_tex.frag";
        PipelineDesc pd; pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
        pd.topology = PrimitiveTopology::Triangles; pd.depth.test = false; pd.depth.write = false;
        PipelineHandle pipe = g_dev->createPipeline(pd);
        auto sampleCentre = [&](float& rOut, float& gOut) {
            // El sello cae en (cam − ancla)·E/N, que tras el primer anclado es ~0 → el centro del mapa.
            grass.pressureFrame(anchor, E, N);
            const glm::dvec3 d = foot[0] - anchor;
            const float side = (float)(grass.config().pressRes * grass.config().pressTexelM);
            const float uv[4] = { (float)glm::dot(d, E) / side + 0.5f, (float)glm::dot(d, N) / side + 0.5f, 0.0f, 0.0f };
            BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, 16, uv, BufferMemory::Dynamic);
            unsigned char px[4] = { 0, 0, 0, 0 };
            if (Context* c = g_dev->beginFrame()) {
                ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
                c->beginRenderPass({}, cv);
                c->bindPipeline(pipe); c->bindUniformBuffer(0, ubo); c->bindTexture(0, grass.pressureTexture());
                c->draw(3, 0, 1);
                c->endRenderPass();
                if (!isVk) g_dev->readPixels(w / 2, h / 2, 1, 1, Format::RGBA8, px);
                g_dev->endFrame(); pumpWindowEvents();
                if (isVk) g_dev->readPixels(w / 2, h / 2, 1, 1, Format::RGBA8, px);
            }
            g_dev->destroy(ubo);
            rOut = px[0] / 255.0f; gOut = px[1] / 255.0f;
        };
        float pr = 0, pg = 0; sampleCentre(pr, pg);
        std::printf("    presion bajo el pie tras el sello: E %.2f · N %.2f\n", pr, pg);
        CHECK(valid(pipe) && pr > 0.25f && pg > 0.25f, "el sello deja la hierba tumbada hacia donde va (las dos componentes > 0,25)");
        // Se levanta: 6 s de mundo sin sellos con `recoverS` = 4 → cero.
        for (int f = 0; f < 6; ++f) frame(matGrass, fwd, true, 1.0f, nullptr, glm::vec3(0.0f));
        float qr = 0, qg = 0; sampleCentre(qr, qg);
        std::printf("    ... y 6 s despues: E %.2f · N %.2f\n", qr, qg);
        CHECK(qr < 0.02f && qg < 0.02f, "sin sellos la hierba se levanta sola (presion a 0 en 6 s)");
        // Y un sello LEJOS del pie no toca el centro (el mapa no es un escalar global).
        const std::vector<glm::dvec3> farStamp = { cam - up0 * 1.7 + E * 20.0 };
        frame(matGrass, fwd, true, 1.0f / 60.0f, &farStamp, vel);
        float fr = 0, fg = 0; sampleCentre(fr, fg);
        std::printf("    CONTRAPRUEBA sello a 20 m: bajo el pie E %.2f · N %.2f\n", fr, fg);
        CHECK(fr < 0.02f && fg < 0.02f, "un sello a 20 m no aplasta la hierba bajo el pie");
        if (valid(pipe)) g_dev->destroy(pipe);
    }

    // ── 5. COSTE ───────────────────────────────────────────────────────────────────────────────
    {
        std::vector<uint8_t> one(4, 0);
        auto timeFrames = [&](bool withGrass, int n) {
            for (int f = 0; f < 4; ++f) frame(matGrass, fwd, withGrass, 1.0f / 60.0f, nullptr, glm::vec3(0.0f));
            g_dev->readPixels(0, 0, 1, 1, Format::RGBA8, one.data());
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int f = 0; f < n; ++f) frame(matGrass, fwd, withGrass, 1.0f / 60.0f, nullptr, glm::vec3(0.0f));
            g_dev->readPixels(0, 0, 1, 1, Format::RGBA8, one.data());
            return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count() / n;
        };
        const double tOff = timeFrames(false, 30), tOn = timeFrames(true, 30);
        const bool vs = (tOff > 15.5 && tOff < 18.0);
        std::printf("    COSTE (256², %zu briznas): sin hierba %.3f ms · con %.3f ms · hierba %.3f ms%s\n",
                    nFwd, tOff, tOn, tOn - tOff, vs ? "  (VSYNC: no vale)" : "");
        baselineMetric("hierba.pase_ms", tOn - tOff, 0.30, true, true, !vs);
    }

    grass.shutdown();
    g_dev->destroy(heightsRb);
    g_dev->destroy(matGrass); g_dev->destroy(matBare);
}
