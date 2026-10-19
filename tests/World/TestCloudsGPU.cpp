#include "RHI/Common/RHITestCommon.h"

// ================================================================================================
// F3 — ¿HAY TERRENO EN PANTALLA? El test que faltaba, y por eso se escaparon tres bugs.
//
// ⚠️ TODOS los demás tests del v5 miran DATOS: el direccionamiento, el contenido del nodo, la
// política del pool, el cosido. Ninguno mira la IMAGEN. Y eso dejó pasar tres fallos que no rompen
// ningún dato y sí vacían la pantalla:
//
//   · las raíces sin fijar -> los nodos lejanos no tenían ancestro y no se dibujaban
//   · el SSBO instanciado  -> las 1 010 instancias leían la misma y se pintaba UN nodo mil veces
//   · la matriz equivocada -> posiciones relativas al ojo con una vista que YA resta la cámara
//
// Los tres pasaron las 100 comprobaciones del banco. El síntoma solo existía en pantalla, así que
// aquí se DIBUJA y se cuentan los píxeles cubiertos, a varias altitudes — porque el fallo reportado
// era "al alejarte no se ve el terreno", o sea que una sola altura no lo habría cazado.
// ================================================================================================
// ================================================================================================
// EL AGUA DEL PASE DE NODOS **DIBUJA**. El test que separa "compila" de "se ve".
//
// ⚠️ EL AGUA SE MIGRO ENTERA DE LA REJILLA DEL CLIPMAP AL QUADTREE Y NADIE LA HABIA VISTO PINTAR. El
// motor tenia sondas del CAMPO (donde hay agua), de la OLA (que altura) y de la PARIDAD GPU/CPU —
// todas de DATOS. Ninguna miraba la IMAGEN, que es exactamente lo que dejo pasar los tres bugs del
// pase de terreno (raices sin fijar, SSBO instanciado, matriz equivocada): los tres pasaban el banco
// entero y vaciaban la pantalla.
//
// ⚠️ Y HACE FALTA UN FONDO DE MAR FALSO. El agua se recorta por PROFUNDIDAD (`nivel - baseH`), y sin
// planeta horneado `baseH` vale 0: la profundidad sale 0 y el fragmento se descarta ENTERO. Con una
// textura de altura sintetica a -500 m hay oceano en todas partes; con +500 m, tierra. Ese par es el
// test y su contraprueba a la vez.
// ================================================================================================
// ================================================================================================
// NUBES: EL PASE VOLUMETRICO, MEDIDO SOBRE LO QUE DIBUJA
//
// ⚠️ ESTE TEST NO EXISTIA, Y ES EXACTAMENTE EL QUE FALTO LA VEZ ANTERIOR. `cloud_shape` mide el CAMPO
// (relacion ancho/alto de la losa, opacidad, pasos) y paso en VERDE mientras el muestreo estaba roto:
// con 24 pasos repartidos por igual, mirando al horizonte el paso superaba el tamaño de la nube
// —0,38 muestras por nube a 88 grados— y las nubes sencillamente no se dibujaban ahi. La leccion
// quedo escrita: *"el test va contra lo que se DIBUJA, no contra la geometria ideal"*. Y aun asi
// `cloud_vol.frag` solo aparecia en la lista de pipelines que COMPILAN. Nadie lo habia dibujado.
//
// Aqui se dibuja de verdad y se mide en el eje donde ese bug se escondia: **el angulo de elevacion**.
// La capa es una cascara esferica, asi que mirando hacia el horizonte el rayo recorre MAS nube, no
// menos — la opacidad tiene que SUBIR hacia el horizonte. Si baja, es que la marcha se queda corta.
// ================================================================================================
// CIELO: LAS CAPAS ALTAS, ¿LOSAS O CALCOMANIAS?
//
// ⚠️ EL BUG QUE ENCONTRO ESTE TEST, Y POR QUE NADIE LO VIO. `sky.frag` tenia un bloque titulado
// *"ESPESOR REAL: LOSA EN VEZ DE LAMINA"* con esta nota: *"ESTE ERA EL MOTIVO DE QUE SE VIERAN
// PLANAS... Se veia plano porque era plano"*. Estaba bajo `if (layer == 2)` — solo el CUMULO. Y desde
// que el cumulo lo dibuja el pase volumetrico, `nLayers` vale 2 y **la capa 2 no se recorre nunca**:
// el arreglo entero estaba aplicado a la unica capa que ya no se pinta. Las dos que si se ven —cirro
// (8 km) y altocumulo (4 km)— seguian siendo campos 2D sobre un cascaron. Igual que `cloud_vol.frag`,
// `sky.frag` solo aparecia en la lista de pipelines que COMPILAN: nadie lo habia dibujado nunca.
//
// La firma que separa una losa de una calcomania es el CAMINO OPTICO: al mirar hacia el horizonte se
// atraviesa mas capa, asi que tapa mas. Una calcomania se ve igual de opaca en el cenit que en el
// horizonte. Eso es lo que se mide aqui, sobre pixeles.

// ================================================================================================
// LA DISTRIBUCION DE `cloudField`: el dato que faltaba para calibrar el cielo
//
// ⚠️ El cielo se tapa ~10 veces menos de lo que pide el clima (cobertura 0,80 -> 6,6 % tapado al
// cenit). Quien lo decide es `lo = mix(0.70, 0.32, cloudiness)` de `sky.frag`, y un umbral solo
// significa algo contra la DISTRIBUCION del campo. Nunca se habia medido: `cloudField` vivia dentro
// del fragment y no se podia llamar desde el banco. Sacada a `lib/sky_clouds.glsl`, esto la sondea.
//
// Lo que se busca son los CUANTILES: "para tapar el X % del cielo, el umbral tiene que valer Y".
void testCloudFieldDistribution()
{
    BEGIN("nubes: FORMACION en GPU (la columna de aire repartida en nubes)");   // distribucion del campo

    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/tests/cloud_field_probe.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de cloud_field_probe.comp creado");
    if (!valid(cp)) return;

    const int N = 256;                       // 65 536 muestras
    const size_t bytes = (size_t)N * N * sizeof(float);
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, bytes, nullptr, BufferMemory::Static);
    BufferHandle rb  = g_dev->createBuffer(BufferUsage::Storage, bytes, nullptr, BufferMemory::Readback);

    // Paso de la rejilla en las unidades del campo. `sky.frag` evalua `base = pk * scale` con
    // `scale` 0,085..0,30 y `pk` en km: cubrir decenas de unidades barre muchas nubes.
    const float in4[4] = { (float)N, 0.08f, 3.0f, 1.0f };
    BufferHandle in = g_dev->createBuffer(BufferUsage::Storage, sizeof(in4), in4, BufferMemory::Static);

    if (Context* ctx = g_dev->beginFrame()) {
        ctx->bindPipeline(cp);
        ctx->bindStorageBuffer(0, out);
        ctx->bindStorageBuffer(1, in);
        ctx->dispatch((uint32_t)((N + 15) / 16), (uint32_t)((N + 15) / 16), 1);
        ctx->memoryBarrier();
        g_dev->endFrame();
    }
    copyThenWait(out, rb, 0, bytes);
    const float* v = (const float*)g_dev->mappedData(rb);
    if (!v) { CHECK(false, "readback del campo"); g_dev->destroy(rb); g_dev->destroy(out);
              g_dev->destroy(in); g_dev->destroy(cp); return; }

    std::vector<float> d(v, v + (size_t)N * N);
    std::sort(d.begin(), d.end());
    auto q = [&](double f) { return d[(size_t)(f * (double)(d.size() - 1))]; };
    double sum = 0.0; for (float x : d) sum += x;
    const double media = sum / (double)d.size();

    std::printf("    %zu muestras · min %.4f · max %.4f · media %.4f\n",
                d.size(), (double)d.front(), (double)d.back(), media);
    std::printf("    cuantiles (el umbral que deja ese %% de cielo POR ENCIMA):\n");
    for (double f : { 0.10, 0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90 })
        std::printf("      tapar el %2.0f %% -> umbral %.4f\n", 100.0 * (1.0 - f), (double)q(f));
    std::printf("    ⚠️ el rango que usa hoy `sky.frag` es lo = 0,70 (sin nube) .. 0,32 (todo nube)\n"
                "       -> con 0,70 tapa el %.1f %% · con 0,32 tapa el %.1f %%\n",
                100.0 * (double)(std::upper_bound(d.begin(), d.end(), 0.70f) - d.begin()) / (double)d.size() * 0.0
                + 100.0 * (1.0 - (double)(std::lower_bound(d.begin(), d.end(), 0.70f) - d.begin()) / (double)d.size()),
                100.0 * (1.0 - (double)(std::lower_bound(d.begin(), d.end(), 0.32f) - d.begin()) / (double)d.size()));

    CHECK(d.back() > d.front(), "el campo VARIA (si fuera constante, no habria nada que calibrar)");
    CHECK(std::isfinite(media), "la media es finita");

    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(in); g_dev->destroy(cp);
}

// ================================================================================================
// FORMA DEL CUMULO EN 3D: ¿es un cuerpo con base y techo, o una bola que corta de golpe?
//
// Andoni (16-09): "son redondas y cortan de repente". Se mide sobre la densidad volumetrica real
// (`cloud_density_probe.comp`, la misma cadena que el pase) en una rejilla de 8 x 8 km y todo el
// espesor de la losa:
//   1. RELACION ANCHO/ALTO de cada nube (componentes conexas de densidad > 0,05). Un cumulo real es
//      mas ancho que alto; una bola da ~1.
//   2. BASE contra TECHO: en un cumulo la base es plana (poca variacion de altura) y el techo es
//      abollado (mucha). En una bola las dos varian igual.
//   3. DUREZA DEL BORDE: al mirar la nube desde abajo, la opacidad (Beer-Lambert con la extincion
//      del motor) pasa de 0,1 a 0,9 en cuantos metros. Un corte "de repente" son unas decenas.
// ================================================================================================
// ── LA MITAD GPU DEL UNICO TEST DE FORMACION ───────────────────────────────────────────────────
// `test_cloud_formation` (CPU) mide el modelo de la columna de aire; aqui se mide que el gemelo GLSL
// (`lib/cloud_column.glsl`) da LO MISMO en 4 096 (columna, altura) al azar. Sin esto el jugador
// podria estar "dentro de la nube" segun la CPU donde el cielo no la dibuja.
void testCloudColumnParity()
{
    BEGIN("nubes: FORMACION en GPU (la columna de aire repartida en nubes)");   // paridad CPU/GPU
    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/tests/cloud_column_probe.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de cloud_column_probe.comp creado");
    if (!valid(cp)) return;
    const int n = 4096;
    std::vector<float> in(8 + (size_t)n * 8, 0.0f);
    std::vector<Haruka::World::AirColumn> cols((size_t)n);
    std::vector<float> zs((size_t)n);
    uint32_t seed = 20260917u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f; };
    in[0] = (float)n;
    for (int i = 0; i < n; ++i) {
        Haruka::World::AirColumn& c = cols[i];
        c.groundM = rnd() * 2500.0f; c.tSurfC = -10.0f + 40.0f * rnd(); c.rhSurf = 0.1f + 0.9f * rnd();
        c.cover = rnd(); c.blDepthM = 500.0f + 7500.0f * rnd(); c.vaporMid = rnd(); c.vaporHigh = rnd();
        zs[i] = rnd() * 13000.0f;
        const size_t b = 8 + (size_t)i * 8;
        in[b + 0] = c.cover; in[b + 1] = Haruka::World::cloudBaseM(c); in[b + 2] = c.groundM + c.blDepthM;
        in[b + 3] = c.vaporMid; in[b + 4] = c.vaporHigh;
        in[b + 5] = c.tSurfC + Haruka::World::kLapseEnvC * c.groundM;   // reducida al nivel del mar, como el horneado
        in[b + 6] = zs[i];
    }
    const size_t bytes = (size_t)n * 3 * sizeof(float);
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, bytes, nullptr, BufferMemory::Static);
    BufferHandle rb  = g_dev->createBuffer(BufferUsage::Storage, bytes, nullptr, BufferMemory::Readback);
    BufferHandle inB = g_dev->createBuffer(BufferUsage::Storage, in.size() * sizeof(float), in.data(), BufferMemory::Static);
    if (Context* ctx = g_dev->beginFrame()) {
        ctx->bindPipeline(cp);
        ctx->bindStorageBuffer(0, out);
        ctx->bindStorageBuffer(1, inB);
        ctx->dispatch((uint32_t)((n + 63) / 64), 1, 1);
        ctx->memoryBarrier();
        g_dev->endFrame();
    }
    copyThenWait(out, rb, 0, bytes);
    const float* v = (const float*)g_dev->mappedData(rb);
    if (!v) { CHECK(false, "readback de la sonda de columna"); }
    else {
        double worst[3] = {0, 0, 0}; int nCloud = 0;
        for (int i = 0; i < n; ++i) {
            const Haruka::World::AirColumn& c = cols[i];
            // ⚠️ La CPU toma la cota del suelo en `airTempC` (max(z − suelo, 0)); el shader, sin suelo,
            // la temperatura reducida al nivel del mar: bajo tierra divergen, y bajo tierra no hay
            // nube que comparar. Solo se compara por encima del suelo.
            if (zs[i] < c.groundM) continue;
            const float cC = Haruka::World::convectiveFractionAt(c, zs[i]);
            const float cA = Haruka::World::aloftFractionAt(c, zs[i]);
            const float cI = Haruka::World::iceFractionAt(c, zs[i]);
            worst[0] = std::max(worst[0], (double)std::fabs(cC - v[i * 3 + 0]));
            worst[1] = std::max(worst[1], (double)std::fabs(cA - v[i * 3 + 1]));
            worst[2] = std::max(worst[2], (double)std::fabs(cI - v[i * 3 + 2]));
            if (cC > 0.05f || cA > 0.05f) ++nCloud;
        }
        std::printf("    paridad CPU/GPU de la columna en %d muestras (%d con nube): peor |dif| convectiva %.2e · vapor en altura %.2e · hielo %.2e\n",
                    n, nCloud, worst[0], worst[1], worst[2]);
        CHECK(nCloud > n / 10, "hay nube en al menos el 10 % de las muestras (si no, la paridad no mide nada)");
        CHECK(worst[0] < 1e-3 && worst[1] < 1e-3 && worst[2] < 1e-3, "la columna de aire da LO MISMO en CPU y en GPU (|dif| < 1e-3)");
    }
    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(inB); g_dev->destroy(cp);
}

// ── ¿GL Y VULKAN CALCULAN EL MISMO RUIDO? ──────────────────────────────────────────────────────
// Con el MISMO shader y la MISMA GPU (GL forzado a la NVIDIA con PRIME), el cirro al cenit salia
// 53,7 % en OpenGL y 67,3 % en Vulkan, y en Vulkan con facetas rectas. La imagen no dice donde;
// esto si: cada pieza del campo (hash, ruido de valor, fbm, fbm con mallas giradas, campo de capa,
// fibras) evaluada en los mismos 4 096 puntos en los dos backends. El primero que corre guarda; el
// segundo compara.
static std::vector<float> g_noiseProbeFirst; static std::string g_noiseProbeFirstBackend;
void testCloudNoiseParity()
{
    BEGIN("nubes: FORMACION en GPU (la columna de aire repartida en nubes)");   // ruido GL vs VK
    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/tests/cloud_noise_probe.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de cloud_noise_probe.comp creado");
    if (!valid(cp)) return;
    const int n = 4096;
    std::vector<float> in(4 + (size_t)n * 4, 0.0f);
    uint32_t seed = 4242u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f; };
    in[0] = (float)n;
    for (int i = 0; i < n; ++i) {   // puntos del orden de los que ve el pase: cientos a miles de unidades
        const float mag = (i % 4 == 0) ? 10.0f : (i % 4 == 1) ? 300.0f : (i % 4 == 2) ? 3000.0f : 4500.0f;
        in[4 + (size_t)i * 4 + 0] = (rnd() - 0.5f) * 2.0f * mag;
        in[4 + (size_t)i * 4 + 1] = (rnd() - 0.5f) * 2.0f * mag;
        in[4 + (size_t)i * 4 + 2] = (rnd() - 0.5f) * 2.0f * mag;
    }
    const size_t bytes = (size_t)n * 6 * sizeof(float);
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, bytes, nullptr, BufferMemory::Static);
    BufferHandle rb  = g_dev->createBuffer(BufferUsage::Storage, bytes, nullptr, BufferMemory::Readback);
    BufferHandle inB = g_dev->createBuffer(BufferUsage::Storage, in.size() * sizeof(float), in.data(), BufferMemory::Static);
    if (Context* ctx = g_dev->beginFrame()) {
        ctx->bindPipeline(cp);
        ctx->bindStorageBuffer(0, out);
        ctx->bindStorageBuffer(1, inB);
        ctx->dispatch((uint32_t)((n + 63) / 64), 1, 1);
        ctx->memoryBarrier();
        g_dev->endFrame();
    }
    copyThenWait(out, rb, 0, bytes);
    const float* v = (const float*)g_dev->mappedData(rb);
    if (!v) { CHECK(false, "readback de la sonda de ruido"); }
    else {
        std::vector<float> cur(v, v + (size_t)n * 6);
        const char* piezas[6] = { "hash13", "ruido de valor", "campo del cumulo", "fbm mallas giradas", "campo de capa", "fibras del cirro" };
        // La ESTADISTICA del campo, que es lo que calibra el umbral (`harukaCloudThreshold`: media +
        // sigma·z). Un hash distinto cambia la sigma (uno correlacionado da mas varianza que uno
        // blanco interpolado) y con ella cuanta nube sale para la misma cobertura.
        for (int k = 2; k < 5; ++k) {
            double m = 0.0, m2 = 0.0;
            for (int i = 0; i < n; ++i) { const double x = cur[(size_t)i * 6 + k]; m += x; m2 += x * x; }
            m /= n; m2 /= n;
            std::printf("    %-20s media %.4f · sigma %.4f\n", piezas[k], m, std::sqrt(std::max(m2 - m * m, 0.0)));
        }
        if (g_noiseProbeFirst.empty()) {
            g_noiseProbeFirst = cur; g_noiseProbeFirstBackend = (g_curBackend == Backend::Vulkan) ? "Vulkan" : "OpenGL";
            std::printf("    ruido evaluado en %d puntos (%s): se compara cuando corra el otro backend\n", n, g_noiseProbeFirstBackend.c_str());
        } else {
            for (int k = 0; k < 6; ++k) {
                double worst = 0.0, mean = 0.0; int nBad = 0;
                for (int i = 0; i < n; ++i) {
                    const double d = std::fabs((double)cur[(size_t)i * 6 + k] - (double)g_noiseProbeFirst[(size_t)i * 6 + k]);
                    worst = std::max(worst, d); mean += d; if (d > 1e-3) ++nBad;
                }
                mean /= n;
                std::printf("    %-20s GL vs VK: |dif| media %.2e · peor %.2e · %d de %d puntos con > 1e-3\n", piezas[k], mean, worst, nBad, n);
                char msg[160];
                // Lo que se caza es el hash que CAMBIA (|dif| ~ 1: el 39 % de los nudos antes del
                // arreglo); las dos etapas en coma flotante que quedan dan ulps amplificados (< 0,05).
                std::snprintf(msg, sizeof msg, "%s: GL y Vulkan dan lo mismo (peor |dif| < 0,1; era 1,0)", piezas[k]);
                CHECK(worst < 0.1, msg);
            }
        }
    }
    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(inB); g_dev->destroy(cp);
}

static void cloudShapeAt(float cover, bool deck)
{
    BEGIN("nubes: FORMACION en GPU (la columna de aire repartida en nubes)");   // forma 3D del cumulo
    if (deck) std::printf("    -- el DECK: cobertura %.2f (\"hay nubes grandes que salen planas\") --\n", cover);
    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/tests/cloud_density_probe.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de cloud_density_probe.comp creado");
    if (!valid(cp)) return;

    const int   N = 192, Ny = 48;                 // 6 km a 31 m · 48 alturas (50 m) en la losa
    const float stepM = 31.25f, thick = 2400.0f, fscale = 0.0007f;
    const size_t cnt = (size_t)N * N * Ny, bytes = cnt * sizeof(float);
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, bytes, nullptr, BufferMemory::Static);
    BufferHandle rb  = g_dev->createBuffer(BufferUsage::Storage, bytes, nullptr, BufferMemory::Readback);
    const float in8[8] = { (float)N, stepM, (float)Ny, thick, cover, fscale, 0.0f, 0.0f };
    BufferHandle in = g_dev->createBuffer(BufferUsage::Storage, sizeof(in8), in8, BufferMemory::Static);
    if (Context* ctx = g_dev->beginFrame()) {
        ctx->bindPipeline(cp);
        ctx->bindStorageBuffer(0, out);
        ctx->bindStorageBuffer(1, in);
        ctx->dispatch((uint32_t)((N + 7) / 8), (uint32_t)((N + 7) / 8), (uint32_t)((Ny + 3) / 4));
        ctx->memoryBarrier();
        g_dev->endFrame();
    }
    copyThenWait(out, rb, 0, bytes);
    const float* v = (const float*)g_dev->mappedData(rb);
    if (!v) { CHECK(false, "readback de la densidad"); g_dev->destroy(rb); g_dev->destroy(out);
              g_dev->destroy(in); g_dev->destroy(cp); return; }
    std::vector<float> d(v, v + cnt);
    auto at = [&](int x, int z, int y) { return d[((size_t)y * N + z) * N + x]; };

    // 1+2. Componentes conexas en 3D (6-vecinos) sobre densidad > 0,05.
    const float kOn = 0.05f;
    std::vector<int> label(cnt, -1);
    struct Comp { int minX = 1 << 30, maxX = -1, minZ = 1 << 30, maxZ = -1, minY = 1 << 30, maxY = -1; size_t vox = 0;
                  std::vector<int> baseY, topY; };
    std::vector<Comp> comps;
    std::vector<size_t> stack;
    for (size_t i = 0; i < cnt; ++i) {
        if (d[i] <= kOn || label[i] >= 0) continue;
        const int id = (int)comps.size(); comps.push_back(Comp{});
        stack.clear(); stack.push_back(i); label[i] = id;
        while (!stack.empty()) {
            const size_t c = stack.back(); stack.pop_back();
            const int y = (int)(c / ((size_t)N * N)), z = (int)((c / N) % N), x = (int)(c % N);
            Comp& C = comps[(size_t)id];
            C.minX = std::min(C.minX, x); C.maxX = std::max(C.maxX, x);
            C.minZ = std::min(C.minZ, z); C.maxZ = std::max(C.maxZ, z);
            C.minY = std::min(C.minY, y); C.maxY = std::max(C.maxY, y);
            ++C.vox;
            const int nb[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
            for (const auto& o : nb) {
                const int nx = x + o[0], nz = z + o[1], ny = y + o[2];
                if (nx < 0 || nz < 0 || ny < 0 || nx >= N || nz >= N || ny >= Ny) continue;
                const size_t j = ((size_t)ny * N + nz) * N + nx;
                if (d[j] > kOn && label[j] < 0) { label[j] = id; stack.push_back(j); }
            }
        }
    }
    // Base y techo por columna dentro de cada componente (y en mapa 2D, para la rugosidad).
    std::vector<int> topMap((size_t)N * N, -1), baseMap((size_t)N * N, -1), idMap((size_t)N * N, -1);
    for (int z = 0; z < N; ++z) for (int x = 0; x < N; ++x) {
        int lo = -1, hi = -1;
        for (int y = 0; y < Ny; ++y) if (at(x, z, y) > kOn) { if (lo < 0) lo = y; hi = y; }
        if (lo < 0) continue;
        const int id = label[((size_t)lo * N + z) * N + x];
        if (id >= 0) { comps[(size_t)id].baseY.push_back(lo); comps[(size_t)id].topY.push_back(hi);
                       topMap[(size_t)z * N + x] = hi; baseMap[(size_t)z * N + x] = lo; idMap[(size_t)z * N + x] = id; }
    }
    // RUGOSIDAD (coliflor): alta frecuencia del techo = techo menos su media 3x3, solo donde los
    // nueve vecinos son de la misma nube. En metros. La base, igual, para contrastar.
    auto roughness = [&](const std::vector<int>& m, int wantId, double& acc, int& cnt) {
        const int R2 = 3;   // ventana 7x7 = 220 m: la escala de un bulto de coliflor
        for (int z = R2; z < N - R2; ++z) for (int x = R2; x < N - R2; ++x) {
            if (idMap[(size_t)z * N + x] != wantId) continue;
            double sum = 0; bool ok = true;
            for (int dz = -R2; dz <= R2 && ok; ++dz) for (int dx = -R2; dx <= R2; ++dx) {
                const size_t k = (size_t)(z + dz) * N + (x + dx);
                if (idMap[k] != wantId) { ok = false; break; }
                sum += m[k];
            }
            if (!ok) continue;
            const double d = (double)m[(size_t)z * N + x] - sum / (double)((2 * R2 + 1) * (2 * R2 + 1));
            acc += d * d; ++cnt;
        }
    };
    const float dyM = thick / (float)Ny;
    double sumRatio = 0.0, sumBaseSd = 0.0, sumTopSd = 0.0, sumTopRough = 0.0, sumBaseRough = 0.0; int nBig = 0; float worstRatio = 1e9f;
    float flattestRatio = 0.0f, flattestW = 0.0f, flattestH = 0.0f;   // la nube mas APLANADA (ancho/alto mayor)
    for (const Comp& C : comps) {
        if (C.vox < 800 || C.baseY.size() < 120) continue;      // nubes de verdad (>= ~0,1 km2), no motas
        const float w = (float)(std::max(C.maxX - C.minX, C.maxZ - C.minZ) + 1) * stepM;
        // Alto TIPICO = espesor medio de sus columnas (la caja englobaria la torre mas alta, y un
        // cumulo con un nucleo que sube no es una torre).
        double th = 0.0; for (size_t i = 0; i < C.baseY.size(); ++i) th += (C.topY[i] - C.baseY[i] + 1);
        const float h = (float)(th / (double)C.baseY.size()) * dyM;
        const float ratio = w / h;
        auto sd = [&](const std::vector<int>& ys) {
            double m = 0; for (int y : ys) m += y; m /= (double)ys.size();
            double s2 = 0; for (int y : ys) s2 += (y - m) * (y - m);
            return std::sqrt(s2 / (double)ys.size()) * dyM;
        };
        sumRatio += ratio; worstRatio = std::min(worstRatio, ratio);
        if (ratio > flattestRatio) { flattestRatio = ratio; flattestW = w; flattestH = h; }
        sumBaseSd += sd(C.baseY); sumTopSd += sd(C.topY); ++nBig;
        double accT = 0, accB = 0; int cT = 0, cB = 0;
        roughness(topMap, (int)(&C - comps.data()), accT, cT);
        roughness(baseMap, (int)(&C - comps.data()), accB, cB);
        sumTopRough  += cT ? std::sqrt(accT / cT) * dyM : 0.0;
        sumBaseRough += cB ? std::sqrt(accB / cB) * dyM : 0.0;
    }
    // ── MESETA: "hay nubes grandes que salen planas" (Andoni) ─────────────────────────────────
    // En una nube grande el nucleo saturaba la fuerza y todos sus puntos llegaban al MISMO techo:
    // una bandeja. Se mide (a) la fraccion de columnas de cada nube grande cuyo techo esta en la
    // capa mas alta de esa nube (o la de debajo): una meseta tiene muchas, un domo pocas; (b) que la
    // nube MAS grande sea tambien mas alta que la mediana (la nube grande es la del ascenso fuerte).
    double plateauMax = 0.0, plateauMean = 0.0; int nPl = 0;
    float hBiggest = 0.0f; size_t voxBiggest = 0; std::vector<float> hAll;
    for (const Comp& C : comps) {
        if (C.vox < 800 || C.topY.size() < 120) continue;
        int top = -1; for (int y : C.topY) top = std::max(top, y);
        int flat = 0; for (int y : C.topY) if (y >= top - 1) ++flat;
        const double fr = (double)flat / (double)C.topY.size();
        plateauMax = std::max(plateauMax, fr); plateauMean += fr; ++nPl;
        const float hTop = (float)(top - C.minY + 1) * dyM;
        hAll.push_back(hTop);
        if (C.vox > voxBiggest) { voxBiggest = C.vox; hBiggest = hTop; }
    }
    if (nPl) plateauMean /= nPl;
    // ── LA CIMA QUE SE VE: donde la opacidad desde arriba llega a 0,63 (optica 1 con 0,008/m) ──
    // La densidad > 0,05 incluye jirones; lo que un ojo ve desde encima es la superficie OPACA, y
    // esa puede ser plana aunque el jiron ondule. Por nube grande: sd de esa cota y su meseta.
    double visSdSum = 0.0, visPlateauMax = 0.0; int nVis = 0;
    for (const Comp& C : comps) {
        if (C.vox < 800 || C.topY.size() < 120) continue;
        const int id = (int)(&C - comps.data());
        std::vector<int> yv;
        for (int z = 0; z < N; ++z) for (int x = 0; x < N; ++x) {
            if (idMap[(size_t)z * N + x] != id) continue;
            float opt = 0.0f; int yy = -1;
            for (int y = Ny - 1; y >= 0; --y) { opt += at(x, z, y) * dyM * 0.008f; if (opt >= 1.0f) { yy = y; break; } }
            if (yy >= 0) yv.push_back(yy);
        }
        if (yv.size() < 60) continue;
        double m = 0; for (int y : yv) m += y; m /= yv.size();
        double s2 = 0; for (int y : yv) s2 += (y - m) * (y - m);
        int top = -1; for (int y : yv) top = std::max(top, y);
        int flat = 0; for (int y : yv) if (y >= top - 1) ++flat;
        visSdSum += std::sqrt(s2 / yv.size()) * dyM; visPlateauMax = std::max(visPlateauMax, (double)flat / yv.size()); ++nVis;
    }
    const double visSd = nVis ? visSdSum / nVis : 0.0;
    std::printf("    cima VISIBLE (opacidad 0,63 desde arriba): ondula %.0f m (sd) · meseta peor %.0f %% en %d nubes\n", visSd, 100.0 * visPlateauMax, nVis);
    std::sort(hAll.begin(), hAll.end());
    const float hMedian = hAll.empty() ? 0.0f : hAll[hAll.size() / 2];
    std::printf("    meseta: columnas en la capa mas alta de su nube, media %.0f %% · peor nube %.0f %%  ·  la nube mas grande sube %.0f m contra %.0f m la mediana\n",
                100.0 * plateauMean, 100.0 * plateauMax, hBiggest, hMedian);
    const double topRough = nBig ? sumTopRough / nBig : 0.0, baseRough = nBig ? sumBaseRough / nBig : 0.0;
    const double ratioMean = nBig ? sumRatio / nBig : 0.0;
    const double baseSd = nBig ? sumBaseSd / nBig : 0.0, topSd = nBig ? sumTopSd / nBig : 0.0;
    std::printf("    cobertura %.2f · %zu componentes (%d de mas de 800 voxeles)\n", cover, comps.size(), nBig);
    std::printf("    ancho/alto: media %.2f:1 · peor %.2f:1   (una bola es 1:1) · la mas APLANADA %.1f:1 (%.0f m de ancho, %.0f de alto)\n",
                ratioMean, worstRatio, flattestRatio, flattestW, flattestH);
    std::printf("    variacion de altura: base %.0f m · techo %.0f m   (cumulo: base plana << techo abollado)\n", baseSd, topSd);
    std::printf("    rugosidad local (alta frecuencia, 7x7 = %.0f m): techo %.0f m · base %.0f m   (coliflor: techo >> base)\n",
                7.0f * stepM, topRough, baseRough);

    // 3. Borde visto desde abajo: opacidad por columna con la extincion del motor (0,008/m).
    std::vector<float> alpha((size_t)N * N, 0.0f);
    for (int z = 0; z < N; ++z) for (int x = 0; x < N; ++x) {
        float opt = 0.0f;
        for (int y = 0; y < Ny; ++y) opt += at(x, z, y) * dyM * 0.008f;
        alpha[(size_t)z * N + x] = 1.0f - std::exp(-opt);
    }
    // Anchura media del borde: recorriendo filas, metros entre alpha 0,1 y 0,9 en cada subida.
    std::vector<float> edges;
    for (int z = 0; z < N; ++z) {
        int start = -1;
        for (int x = 1; x < N; ++x) {
            const float a0 = alpha[(size_t)z * N + x - 1], a1 = alpha[(size_t)z * N + x];
            if (a0 < 0.1f && a1 >= 0.1f) start = x;
            if (start >= 0 && a1 >= 0.9f) { edges.push_back((float)(x - start + 1) * stepM); start = -1; }
            if (a1 < 0.1f) start = -1;
        }
    }
    std::sort(edges.begin(), edges.end());
    const float edgeMed = edges.empty() ? 0.0f : edges[edges.size() / 2];
    std::printf("    borde (opacidad 0,1 -> 0,9 visto desde abajo): mediana %.0f m en %zu bordes · rejilla %.1f m\n",
                edgeMed, edges.size(), stepM);

    if (deck) {
        // Con cobertura alta el campo pasa el umbral casi en todas partes y sale UNA nube grande: la
        // que Andoni veia plana. Con el tope duro del techo (min(...,1)) el exceso grande la llevaba
        // entera al techo de la losa: una bandeja. Un estratocumulo real es CELULAR.
        CHECK(nBig >= 1, "con cobertura alta hay al menos una nube grande");
        CHECK(plateauMax < 0.25, "el DECK no es una bandeja: menos del 25 % de sus columnas en la capa mas alta");
        CHECK(topSd > 150.0, "la cima del deck ondula mas de 150 m (celular, no plana)");
        CHECK(topRough >= 40.0, "y tiene coliflor (bultos locales >= 40 m) tambien con cobertura alta");
        CHECK(nVis > 0 && visSd > 120.0, "la cima OPACA del deck (la que se ve desde encima) ondula mas de 120 m");
        CHECK(visPlateauMax < 0.25, "...y no es una meseta (menos del 25 % de columnas en su capa mas alta)");
        g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(in); g_dev->destroy(cp);
        return;
    }
    CHECK(nBig >= 3, "hay nubes que medir");
    CHECK(plateauMax < 0.25, "ninguna nube grande es una MESETA: menos del 25 % de sus columnas en la capa mas alta (con la fuerza saturada eran bandejas)");
    // "Hay nubes grandes que salen planas": la nube mas ancha no puede ser una torta. Un cumulo
    // mediocris real anda en 2-4:1; a 6:1 se lee como una lamina. Con el techo al 60 % de la losa
    // para TODAS las nubes, la mas ancha del banco salia a 6-8:1 (medido antes de la torre).
    CHECK(flattestRatio < 6.0, "la nube mas ancha no es una torta: ancho/alto < 6 (la nube grande sube con su nucleo; medido 5,2 con torre, 5,5 sin ella)");
    CHECK(hAll.size() < 2 || hBiggest >= hMedian, "la nube mas grande no es mas baja que la mediana (la grande es la del ascenso fuerte)");
    CHECK(ratioMean >= 1.8, "un cumulo es mas ancho que alto (media >= 1,8:1)");
    CHECK(baseSd * 2.0 < topSd, "la base es plana y el techo abollado (variacion de la base < mitad de la del techo)");
    CHECK(topRough >= 45.0 && topRough > 3.0 * baseRough, "el techo es COLIFLOR: bultos locales de >= 45 m y al menos 3x los de la base");
    CHECK(edgeMed >= 4.0f * stepM, "el borde no es un corte: la opacidad tarda al menos 4 celdas (125 m) en subir (un cumulo real: 100-200 m)");
    // Contraprueba del metodo: una bola perfecta da 1:1, base y techo iguales, y borde de una celda.
    {
        std::vector<float> ball(cnt, 0.0f);
        const float rM = 900.0f, cy = 0.5f * thick;
        for (int y = 0; y < Ny; ++y) for (int z = 0; z < N; ++z) for (int x = 0; x < N; ++x) {
            const float px = ((float)x - 0.5f * N) * stepM, pz = ((float)z - 0.5f * N) * stepM, py = ((float)y + 0.5f) * dyM - cy;
            ball[((size_t)y * N + z) * N + x] = (px * px + pz * pz + py * py < rM * rM) ? 1.0f : 0.0f;
        }
        int lo = Ny, hi = -1, minX = N, maxX = -1;
        std::vector<int> by, ty;
        for (int z = 0; z < N; ++z) for (int x = 0; x < N; ++x) {
            int b = -1, t = -1;
            for (int y = 0; y < Ny; ++y) if (ball[((size_t)y * N + z) * N + x] > 0.5f) { if (b < 0) b = y; t = y; }
            if (b < 0) continue;
            by.push_back(b); ty.push_back(t); lo = std::min(lo, b); hi = std::max(hi, t); minX = std::min(minX, x); maxX = std::max(maxX, x);
        }
        double thb = 0.0; for (size_t i = 0; i < by.size(); ++i) thb += (ty[i] - by[i] + 1);
        const float ratioBall = (float)(maxX - minX + 1) * stepM / ((float)(thb / (double)by.size()) * dyM);
        double mb = 0, mt = 0; for (size_t i = 0; i < by.size(); ++i) { mb += by[i]; mt += ty[i]; } mb /= by.size(); mt /= ty.size();
        double sb = 0, st = 0; for (size_t i = 0; i < by.size(); ++i) { sb += (by[i] - mb) * (by[i] - mb); st += (ty[i] - mt) * (ty[i] - mt); }
        std::printf("    contraprueba (bola de 900 m): ancho/alto %.2f:1 · base %.0f m · techo %.0f m\n",
                    ratioBall, std::sqrt(sb / by.size()) * dyM, std::sqrt(st / ty.size()) * dyM);
        CHECK(ratioBall < 1.6f && std::abs(sb - st) < 1e-6, "contraprueba: la bola da ~1,5:1 (ancho / espesor medio) y base = techo");
    }
    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(in); g_dev->destroy(cp);
}
void testCloudShape3D()
{
    cloudShapeAt(0.35f, false);   // buen tiempo: cumulos sueltos
    cloudShapeAt(0.85f, true);    // el deck: una nube grande que no puede ser una bandeja
}

void testSkyLayersHaveDepth()
{
    BEGIN("cielo: las capas altas tienen ESPESOR (no son calcomanias)");

    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;

    const std::string base = Haruka::Renderer::Shader::baseDir();
    PipelineDesc pd;
    const std::string vs = base + "shaders/sky.vert", fs = base + "shaders/sky.frag";
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.topology = PrimitiveTopology::Triangles;
    pd.depth.test = false; pd.depth.write = false; pd.blend.enable = false;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "pipeline del cielo creado");
    if (!valid(pipe)) return;

    struct SkyUBO {
        float invVP[16];
        float sunDir[3]; float sunElev;
        float up[3];     float atmo;
        float sunColor[3]; float time;
        float weather[4]; float wind[4]; float planet[4];
    };
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(SkyUBO), nullptr,
                                           BufferMemory::Dynamic);

    const double R = 6371000.0;
    const glm::dvec3 up0(0.0, 1.0, 0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const float aspect = (float)w / (float)h;
    const float fCot = 1.0f / std::tan((float)fovY * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = fCot / aspect; proj[1][1] = fCot;
    proj[2][3] = -1.0f;         proj[3][2] = 1.0f;      // reversed-Z, lejano infinito

    float g_skyTime = 5.0f;
    // `u_planet.w > 1` = "no hay pase volumetrico, pinta tu el cumulo": el camino de RESPALDO.
    bool respaldo = false;
    auto shootRaw = [&](const glm::dvec3& fwd, const glm::dvec3& vup, float cover,
                        std::vector<uint8_t>& px) {
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup));
        const glm::mat4 invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));
        SkyUBO u{};
        std::memcpy(u.invVP, &invVP[0][0], sizeof(u.invVP));
        const glm::vec3 sun = glm::normalize(glm::vec3(0.35f, 0.62f, 0.70f));
        u.sunDir[0]=sun.x; u.sunDir[1]=sun.y; u.sunDir[2]=sun.z; u.sunElev = sun.y;
        u.up[0]=(float)up0.x; u.up[1]=(float)up0.y; u.up[2]=(float)up0.z; u.atmo = 1.0f;
        u.sunColor[0]=u.sunColor[1]=u.sunColor[2]=1.0f; u.time = g_skyTime;
        u.weather[0]=0.5f; u.weather[1]=15.0f; u.weather[2]=0.0f; u.weather[3]=cover;
        u.wind[0]=4.0f; u.wind[1]=1.0f; u.wind[2]=0.0f; u.wind[3]=0.0f;
        // ⚠️ `planet.w = 0` = "del cumulo se encarga el pase volumetrico", que es el camino REAL del
        // motor. Con w > 0 se pintarian tres capas y se estaria probando el camino de respaldo.
        u.planet[0]=(float)R; u.planet[1]=300.0f; u.planet[2]=1500.0f;
        u.planet[3]= respaldo ? 3200.0f : 0.0f;   // >1 = techo del cumulo -> camino de respaldo
        g_dev->updateBuffer(ubo, 0, sizeof(u), &u);

        px.assign((size_t)w * h * 4, 0xAA);
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        for (int f = 0; f < 3; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0]=cv.color[1]=cv.color[2]=0.0f; cv.color[3]=1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            ctx->bindPipeline(pipe); ctx->bindUniformBuffer(5, ubo);
            ctx->draw(3, 1);
            ctx->endRenderPass();
            if (!isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame(); pumpWindowEvents();
            if (isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
    };

    // Calibracion del orden de filas, con el cielo despejado (el sol es la referencia). Ver
    // `detectRowFlip`: atarlo al backend salio espejado en uno de los dos.
    bool rowFlip = false;
    {
        std::vector<uint8_t> cal;
        shootRaw(glm::dvec3(1, 0, 0), up0, 0.0f, cal);
        rowFlip = detectRowFlip(cal, w, h);
        std::printf("    orden de filas del readback: %s (calibrado con la posicion del sol)\n",
                    rowFlip ? "invertido" : "directo");
    }

    // ⚠️ EL DISCRIMINANTE, A LA TERCERA — LAS DOS PRIMERAS FALLARON POR EL INSTRUMENTO.
    // "Poco azul y claro" (`B-R < 40 && suma > 150`) dio **0 % en las cuatro vistas**; bajar el
    // umbral a `B-R < 55` dio 0,5 %. No es que no hubiera nubes: el cielo de este motor las tiñe de
    // azul y ninguna regla sobre la paleta acertaba. Lo que NO depende de la paleta es **que pixeles
    // CAMBIAN al mover la cobertura** — eso es la nube, sea del color que sea. Es la misma tecnica
    // con la que se cazo que los dos backends sombreaban distinto (99,8 % contra 48,8 %).
    auto cloudFraction = [&](const glm::dvec3& fwd, const glm::dvec3& vup, float cover) {
        std::vector<uint8_t> a, b;
        shootRaw(fwd, vup, 0.0f, a);
        shootRaw(fwd, vup, cover, b);
        size_t dif = 0, n = 0;
        for (size_t k = 0; k + 3 < a.size() && k + 3 < b.size(); k += 4) {
            if (a[k] == 0xAA && a[k+1] == 0xAA && a[k+2] == 0xAA) continue;
            const int d = std::abs((int)a[k] - (int)b[k]) + std::abs((int)a[k+1] - (int)b[k+1])
                        + std::abs((int)a[k+2] - (int)b[k+2]);
            if (d > 12) ++dif;
            ++n;
        }
        return n ? (double)dif / (double)n : 0.0;
    };

    // ⚠️ Y UNA CUARTA TRAMPA DEL INSTRUMENTO, esta geometrica: comparar la vista al CENIT con la del
    // HORIZONTE no vale, porque la segunda tiene **la mitad inferior por debajo del horizonte**,
    // donde `sky.frag` no pinta cielo (`t > 0.02`). Salio razon 0,47, que es exactamente esa mitad y
    // no una propiedad de la nube. La comparacion honesta es entre BANDAS DE ELEVACION DE LA MISMA
    // IMAGEN: mismo render, mismos pixeles validos, solo cambia cuanta capa atraviesa el rayo.
    const glm::dvec3 horiz(1.0, 0.0, 0.0);
    auto bandFractions = [&](float cover, double loDeg, double hiDeg, double& outLow, double& outHigh) {
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(horiz), glm::vec3(up0));
        const glm::mat4 invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));
        std::vector<uint8_t> a, b;
        shootRaw(horiz, up0, 0.0f, a);
        shootRaw(horiz, up0, cover, b);
        size_t dLo = 0, nLo = 0, dHi = 0, nHi = 0;
        for (int y = 0; y < h; ++y) {
            const float ndcY = rowToNdcY(y, h, rowFlip);
            glm::vec4 far = invVP * glm::vec4(0.0f, ndcY, 1.0f, 1.0f);
            const glm::vec3 dd = glm::normalize(glm::vec3(far) / far.w);
            const double el = std::asin(glm::clamp((double)glm::dot(dd, glm::vec3(up0)), -1.0, 1.0))
                            * 180.0 / 3.14159265358979;
            const bool low  = (el > 1.0 && el < loDeg);
            const bool high = (el > hiDeg);
            if (!low && !high) continue;
            for (int x = 0; x < w; ++x) {
                const size_t k = ((size_t)y * w + x) * 4;
                if (a[k] == 0xAA && a[k+1] == 0xAA && a[k+2] == 0xAA) continue;
                const int df = std::abs((int)a[k] - (int)b[k]) + std::abs((int)a[k+1] - (int)b[k+1])
                             + std::abs((int)a[k+2] - (int)b[k+2]);
                if (low)  { if (df > 12) ++dLo; ++nLo; }
                else      { if (df > 12) ++dHi; ++nHi; }
            }
        }
        outLow  = nLo ? (double)dLo / (double)nLo : 0.0;
        outHigh = nHi ? (double)dHi / (double)nHi : 0.0;
    };

    // ⚠️ EL PERFIL ENTERO, no dos bandas elegidas a ojo. Con 1-10 y >20 grados salieron las DOS a
    // cero mientras la imagen completa daba 1,6 %, o sea que la nube estaba justo en el hueco que
    // deje sin mirar. Se imprime la fraccion por banda y la afirmacion se hace sobre lo que salga.
    // ⚠️ SOBRE VARIOS INSTANTES, no una foto. Una sola pasada no distingue "esta banda esta MUERTA"
    // de "en este momento ahi no hay nube": el campo se mueve con el viento. Se promedian cuatro
    // tiempos separados, que es lo que convierte el perfil en una propiedad del sistema.
    double fLow = 0.0, fHigh = 0.0, fRing = 0.0, fAny = 0.0, elLo = 1e9, elHi = -1e9;
    {
        const int NB = 6;
        std::vector<double> accF((size_t)NB, 0.0);
        int tomas = 0;
        double accAny = 0.0;
        for (float tm : { 5.0f, 240.0f, 900.0f, 1800.0f }) {
            g_skyTime = tm;
            const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(horiz), glm::vec3(up0));
            const glm::mat4 invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));
            std::vector<uint8_t> a, b;
            shootRaw(horiz, up0, 0.0f, a);
            shootRaw(horiz, up0, 0.45f, b);
            std::vector<size_t> dif((size_t)NB, 0), cnt((size_t)NB, 0);
            size_t dTot = 0, nTot = 0;
            for (int y = 0; y < h; ++y) {
                const float ndcY = rowToNdcY(y, h, rowFlip);
                glm::vec4 far = invVP * glm::vec4(0.0f, ndcY, 1.0f, 1.0f);
                const glm::vec3 dd = glm::normalize(glm::vec3(far) / far.w);
                const double el = std::asin(glm::clamp((double)glm::dot(dd, glm::vec3(up0)), -1.0, 1.0))
                                * 180.0 / 3.14159265358979;
                const bool inBand = (el > 0.0 && el < 30.0);
                const int bnd = inBand ? std::min(NB - 1, (int)(el / 30.0 * NB)) : -1;
                for (int x = 0; x < w; ++x) {
                    const size_t k = ((size_t)y * w + x) * 4;
                    if (a[k] == 0xAA && a[k+1] == 0xAA && a[k+2] == 0xAA) continue;
                    const int df = std::abs((int)a[k] - (int)b[k]) + std::abs((int)a[k+1] - (int)b[k+1])
                                 + std::abs((int)a[k+2] - (int)b[k+2]);
                    if (df > 12) { ++dTot; elLo = std::min(elLo, el); elHi = std::max(elHi, el); }
                    ++nTot;
                    if (bnd >= 0) { if (df > 12) ++dif[(size_t)bnd]; ++cnt[(size_t)bnd]; }
                }
            }
            for (int bnd = 0; bnd < NB; ++bnd)
                accF[(size_t)bnd] += cnt[(size_t)bnd] ? (double)dif[(size_t)bnd] / (double)cnt[(size_t)bnd] : 0.0;
            accAny += nTot ? (double)dTot / (double)nTot : 0.0;
            ++tomas;
        }
        g_skyTime = 5.0f;
        std::printf("    cobertura 0,45, media de %d instantes · fraccion de cielo CON NUBE por banda:\n", tomas);
        for (int bnd = 0; bnd < NB; ++bnd) {
            const double f = accF[(size_t)bnd] / (double)tomas;
            std::printf("      %2d-%2d grados   %6.2f %%\n", bnd * 5, (bnd + 1) * 5, 100.0 * f);
            if (bnd == 0) fLow = f;
            fHigh = f;
            fRing = std::max(fRing, f);
        }
        fAny = accAny / (double)tomas;
        if (elLo > elHi) { elLo = 0.0; elHi = 0.0; }
    }
    (void)fRing;
    // La afirmacion va sobre la IMAGEN ENTERA, no sobre una banda: en que franja cae es justo lo que
    // varia y lo que el perfil de arriba deja escrito. Afirmar una banda concreta seria clavar el
    // defecto en el test.
    // ⚠️⚠️ ESTE TEST YA NO PRUEBA LO QUE PROBABA, Y ES A PROPOSITO. Media el cirro y el altocumulo
    // pintados por `sky.frag`; ahora esas dos capas viven en `cloud_vol.frag` como cascaras
    // volumetricas (se atraviesan y se ven desde arriba, que como fondo era imposible). Asi que el
    // cielo de fondo NO debe dibujar ni una nube en el camino normal — y eso es justo lo que hay que
    // fijar, o alguien las devuelve ahi sin querer y vuelven a ser un telon.
    //
    // Lo que MIDE las capas altas ahora es `cloud_volume_draws`. Aqui queda la otra mitad: que el
    // camino de RESPALDO (pase volumetrico apagado, `u_planet.w > 1`) siga pintando cielo con nubes,
    // porque apagarlas no debe dejar el cielo pelado.
    std::printf("    con el pase volumetrico ACTIVO, el cielo de fondo dibuja %.2f %% de nube\n",
                100.0 * fAny);
    CHECK(fAny < 0.02,
          "el cielo de fondo YA NO pinta nubes: las tres capas son volumetricas (no un telon)");
    {
        // ⚠️ ESTO AFIRMABA LO CONTRARIO, Y EL CAMBIO ES DELIBERADO. Habia un camino de RESPALDO
        // (`u_planet.w > 1` = "no hay pase volumetrico, pinta tu el cumulo plano") y este bloque
        // exigia que siguiera pintando nubes. Se borro a peticion de Andoni: mantener una copia
        // PLANA de las nubes aqui significaba DOS respuestas a "que nube hay en esa direccion", y
        // cual se veia dependia de un conmutador — la misma enfermedad que las tres aguas.
        //
        // Ahora el test es la GUARDIA de que nadie la resucite: ni con el conmutador del respaldo
        // encendido se pinta una sola nube en el fondo. Y de paso hace honesto el interruptor:
        // apagar el pase volumetrico deja el cielo SIN nubes, que es lo que significa apagarlo.
        std::vector<uint8_t> a, b;
        respaldo = true;  shootRaw(horiz, up0, 0.0f, a);
        shootRaw(horiz, up0, 0.45f, b);
        respaldo = false;
        size_t dif = 0, n = 0;
        for (size_t k = 0; k + 3 < a.size() && k + 3 < b.size(); k += 4) {
            if (a[k] == 0xAA && a[k+1] == 0xAA && a[k+2] == 0xAA) continue;
            const int d2 = std::abs((int)a[k] - (int)b[k]) + std::abs((int)a[k+1] - (int)b[k+1])
                         + std::abs((int)a[k+2] - (int)b[k+2]);
            if (d2 > 12) ++dif;
            ++n;
        }
        std::printf("    con el conmutador del respaldo ENCENDIDO: %.1f %% de cielo con nube "
                    "(tiene que ser ~0: el camino plano ya no existe)\n",
                    n ? 100.0 * (double)dif / (double)n : 0.0);
        CHECK(n && dif < n / 50,
              "el camino PLANO de respaldo esta borrado: el fondo no pinta nubes ni con w > 0");
    }
    std::printf("    la nube del cielo vive entre %.1f y %.1f grados de elevacion\n", elLo, elHi);

    // ⚠️ NO HAY ANILLO, Y ESTO ES LO QUE LO FIJA. La capa vivia entre 5,8 y 21,1 grados con CERO por
    // encima de 25 — un anillo a altura fija de pantalla, que es lo que se lee como "las nubes son un
    // plano 2D". Tres causas, las tres medidas:
    //   · `band` apagaba la capa desde 38 grados hasta el cenit (valia 0,026 mirando arriba);
    //   · `cloudPlane` acotaba la distancia con un `min` DURO, lo que vaciaba la franja rasante;
    //   · `scale` daba elementos de 4,8 km a una capa que esta a 4 km, asi que el cielo alto entero
    //     cabia dentro de UNA nube y salia todo o nada.
    // El test exige nube en las dos puntas del barrido: es lo unico que un anillo no puede cumplir.
    // ⚠️ NO SE EXIGE NUBE EN LA BANDA MAS RASANTE, y es a proposito. Ahi el pixel abarca varias
    // celdas de la reticula del ruido, asi que `fbmAA` apaga las octavas que no se pueden resolver
    // (ver `lib/sky_clouds.glsl`) y lo que queda es bruma lisa — que es lo que hace el cielo de
    // verdad y lo que quita los "cuadrados deformes". Pedir nube contrastada ahi seria pedir alias.
    // Lo que fija que NO hay anillo es el ALCANCE: nube desde cerca del horizonte hasta lo alto del
    // barrido. Un anillo, por definicion, no puede cubrir las dos cosas.
    // (El perfil por bandas se conserva impreso: con el fondo ya sin nubes debe salir plano a cero,
    //  y es la forma de ver de un vistazo si alguien las devuelve al telon.)

    const double fZen = fHigh;   // para la contraprueba de abajo
    (void)fZen;

    {
        const double f0 = cloudFraction(horiz, up0, 0.0f);
        std::printf("    CONTRAPRUEBA cobertura 0,00 contra si misma: %.2f %% de pixeles cambian\n",
                    100.0 * f0);
        CHECK(f0 < 0.01, "CONTRAPRUEBA: dos veces el mismo cielo despejado dan la MISMA imagen");
    }
    // ⚠️ AQUI SE MEDIA "mas cobertura pedida = mas cielo tapado" SOBRE EL RESPALDO, y ese camino ya
    // no existe: con el fondo sin nubes comparaba 0 contra 0. La invariante no se pierde — la mide
    // `cloud_volume_draws` sobre el pase que de verdad tiene las capas, con su tabla de
    // "cobertura pedida -> fraccion tapada" al cenit. Un test no puede quedarse midiendo un camino
    // borrado: o se mueve a donde vive lo que afirma, o miente en verde.

    // HARUKA_SKY_PNG=<dir> vuelca el cielo a disco. Igual que el volcado del agua y por lo mismo: si
    // las nubes se ven bien o no es un juicio visual, y hace falta poder mirarlas.
    if (const char* dir = std::getenv("HARUKA_SKY_PNG")) {
        const bool isVk2 = (g_dev->backend() == Backend::Vulkan);
        struct Vista { glm::dvec3 fwd, up; const char* nombre; };
        const Vista vistas[2] = { { glm::dvec3(1,0,0), up0, "horizonte" },
                                  { up0, glm::dvec3(1,0,0), "cenit" } };
        for (const Vista& vi : vistas) {
            std::vector<uint8_t> px;
            shootRaw(vi.fwd, vi.up, 0.45f, px);
            std::vector<uint8_t> img(px.size());
            for (int y = 0; y < h; ++y) {
                const int src = isVk2 ? y : (h - 1 - y);
                std::memcpy(&img[(size_t)y * w * 4], &px[(size_t)src * w * 4], (size_t)w * 4);
            }
            const std::string path = std::string(dir) + "/" + (isVk2 ? "vk_" : "gl_")
                                   + "cielo_" + vi.nombre + ".png";
            Haruka::IO::writePNG(path, w, h, 4, img.data());
            std::printf("      -> %s\n", path.c_str());
        }
    }

    g_dev->destroy(pipe); g_dev->destroy(ubo);
}

void testAtmosphereLimb()
{
    BEGIN("atmosfera: el planeta tiene LIMBO visto desde fuera");

    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;

    const std::string base = Haruka::Renderer::Shader::baseDir();
    PipelineDesc pd;
    const std::string vs = base + "shaders/sky.vert", fs = base + "shaders/sky.frag";
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.topology = PrimitiveTopology::Triangles;
    pd.depth.test = false; pd.depth.write = false; pd.blend.enable = false;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "pipeline del cielo creado");
    if (!valid(pipe)) return;

    struct SkyUBO {
        float invVP[16];
        float sunDir[3]; float sunElev;
        float up[3];     float atmo;
        float sunColor[3]; float time;
        float weather[4]; float wind[4]; float planet[4];
    };
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(SkyUBO), nullptr,
                                           BufferMemory::Dynamic);

    // ⚠️ NO SE ENCUADRA EL PLANETA ENTERO, SE ENCUADRA EL LIMBO. Mi primer intento miraba al centro
    // desde 3000 km con 60 grados de fov: el planeta subtiende **42,83 grados de SEMIANGULO** y el
    // fov solo 30, asi que el cuadro entero caia dentro del disco — ni anillo ni espacio que medir,
    // y las cuatro afirmaciones fallaron por el encuadre, no por la atmosfera.
    //
    // Y alejarse tampoco vale: a 20 000 km el planeta cabe, pero la capa de 80 km subtiende 0,18
    // grados, o sea MENOS DE UN PIXEL. La atmosfera vista de lejos es una linea de pelo; para medirla
    // hay que acercar el ojo y estrechar el fov, que es lo que hace cualquiera al fotografiarla.
    const double R = 6371000.0, ALT = 3.0e6;
    const glm::dvec3 up0(0.0, 1.0, 0.0);
    const double fovY = 10.0 * 3.14159265358979 / 180.0;   // estrecho: el limbo ocupa ~17 px
    const float aspect = (float)w / (float)h;
    const float fCot = 1.0f / std::tan((float)fovY * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = fCot / aspect; proj[1][1] = fCot;
    proj[2][3] = -1.0f;         proj[3][2] = 1.0f;

    // El nadir (hacia el centro del planeta) y el limbo: una direccion a `angPlaneta` del nadir.
    const glm::dvec3 nadir = -up0;
    const double aLimb = std::asin(R / (R + ALT));
    const glm::dvec3 fwd = glm::normalize(nadir * std::cos(aLimb) + glm::dvec3(1, 0, 0) * std::sin(aLimb));
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(0, 1, 0));
    const glm::mat4 invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));
    // El sol hacia +X: el limbo encuadrado queda ILUMINADO, y el opuesto en noche.
    const glm::vec3 sun = glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f));

    bool sinPlaneta = false;   // R = 0 -> `harukaAtmoLimb` sale por su primera linea
    auto shootVP = [&](float atmo, const glm::mat4& iVP, std::vector<uint8_t>& px) {
        // ⚠️ "DENTRO" Y "FUERA" YA NO SON UN MANDO (`atmo`): el cielo sale de donde esta el OJO (la luz
        // del frame, `lib/sky_light.glsl`). Fuera = a 3 000 km; dentro = a 2 m del suelo.
        const double eyeAlt = (atmo > 0.5f) ? 2.0 : ALT;
        setTestSkyLight(-up0 * (R + eyeAlt), sinPlaneta ? 0.0 : R, glm::dvec3(sun));
        SkyUBO u{};
        std::memcpy(u.invVP, &iVP[0][0], sizeof(u.invVP));
        u.sunDir[0]=sun.x; u.sunDir[1]=sun.y; u.sunDir[2]=sun.z; u.sunElev = 0.0f;
        u.up[0]=(float)up0.x; u.up[1]=(float)up0.y; u.up[2]=(float)up0.z; u.atmo = atmo;
        u.sunColor[0]=u.sunColor[1]=u.sunColor[2]=1.0f; u.time = 3.0f;
        u.weather[0]=0.5f; u.weather[1]=15.0f; u.weather[2]=0.0f; u.weather[3]=0.3f;
        u.wind[0]=2.0f; u.wind[1]=0.0f; u.wind[2]=0.0f; u.wind[3]=0.0f;
        u.planet[0]= sinPlaneta ? 0.0f : (float)R;
        u.planet[1]=(float)ALT; u.planet[2]=1500.0f; u.planet[3]=0.0f;
        g_dev->updateBuffer(ubo, 0, sizeof(u), &u);
        px.assign((size_t)w * h * 4, 0xAA);
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        for (int f = 0; f < 3; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0]=cv.color[1]=cv.color[2]=0.0f; cv.color[3]=1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            ctx->bindPipeline(pipe); ctx->bindUniformBuffer(5, ubo);
            ctx->draw(3, 1);
            ctx->endRenderPass();
            if (!isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame(); pumpWindowEvents();
            if (isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
    };

    auto shoot = [&](float atmo, std::vector<uint8_t>& px) { shootVP(atmo, invVP, px); };

    std::vector<uint8_t> px;
    shoot(0.0f, px);                                     // FUERA de la atmosfera

    // ⚠️ EL ORDEN DE FILAS **NO** SE PUEDE CALIBRAR CON ESTA IMAGEN, y eso me costo una deduccion
    // entera. `detectRowFlip` busca la fila mas brillante suponiendo que es "arriba" — vale para un
    // cielo con el sol en lo alto, pero aqui el encuadre esta CENTRADO en el limbo y la banda
    // brillante cae en MEDIO: la heuristica sale a cara o cruz. Con el signo invertido, el perfil por
    // angulo salia espejado y parecia que el halo se extendia 4 grados fuera de la cascara — cuando
    // lo que habia en esos pixeles era la BRUMA sobre el disco del planeta, que es correcta.
    //
    // Se calibra con un encuadre donde la respuesta se sabe: mirando al nadir con el eje +X como
    // "arriba" de la camara y el sol en +X, la mitad de arriba TIENE que ser la brillante.
    bool rowFlip = false;
    {
        const glm::mat4 vCal = glm::lookAt(glm::vec3(0.0f), glm::vec3(nadir), glm::vec3(1, 0, 0));
        const glm::mat4 iCal = glm::inverse(proj * glm::mat4(glm::mat3(vCal)));
        std::vector<uint8_t> pxCal;
        shootVP(0.0f, iCal, pxCal);
        rowFlip = detectRowFlip(pxCal, w, h);
        std::printf("    orden de filas: %s (calibrado con el sol en un encuadre que si lo delata)\n",
                    rowFlip ? "invertido" : "directo");
    }

    // El angulo de cada pixel respecto al eje de vista dice a que distancia angular del centro del
    // planeta esta. El planeta subtiende asin(R/(R+ALT)); el borde de la atmosfera, asin((R+80km)/…).
    const double angPlaneta = aLimb * 180.0 / 3.14159265358979;
    const double angAtmo    = std::asin((R + 80000.0) / (R + ALT)) * 180.0 / 3.14159265358979;
    std::printf("    desde %.0f km: el planeta subtiende %.2f grados · la atmosfera llega a %.2f"
                " · fov %.0f grados -> el anillo son ~%.0f px\n",
                ALT / 1000.0, angPlaneta, angAtmo, fovY * 180.0 / 3.14159265358979,
                (angAtmo - angPlaneta) / (fovY * 180.0 / 3.14159265358979) * h);

    // Se miden tres coronas: DENTRO del disco, el ANILLO de atmosfera, y FUERA (espacio vacio).
    double sumIn = 0, sumRing = 0, sumOut = 0; size_t nIn = 0, nRing = 0, nOut = 0;
    double ringB = 0.0, ringR = 0.0;
    for (int y = 0; y < h; ++y) {
        const float ndcY = rowToNdcY(y, h, rowFlip);
        for (int x = 0; x < w; ++x) {
            const float ndcX = 2.0f * ((float)x + 0.5f) / (float)w - 1.0f;
            glm::vec4 far = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
            const glm::vec3 d = glm::normalize(glm::vec3(far) / far.w);
            // ⚠️ DESDE EL NADIR, no desde el eje de vista: asi el criterio no depende de hacia
            // donde se encuadre, y el disco/anillo/espacio salen de la geometria del planeta.
            const double ang = std::acos(glm::clamp((double)glm::dot(d, glm::vec3(nadir)), -1.0, 1.0))
                             * 180.0 / 3.14159265358979;
            const size_t k = ((size_t)y * w + x) * 4;
            if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
            const double L = 0.2126*px[k] + 0.7152*px[k+1] + 0.0722*px[k+2];
            // Solo el lado ILUMINADO (el sol esta en +X): en el nocturno no debe haber halo.
            const bool dia = (d.x > 0.3f);
            if (ang < angPlaneta * 0.995)                 { sumIn += L; ++nIn; }
            else if (dia && ang > angPlaneta && ang < angAtmo) {
                sumRing += L; ++nRing; ringB += px[k+2]; ringR += px[k];
            }
            else if (ang > angAtmo * 1.02)                { sumOut += L; ++nOut; }
        }
    }
    const double mIn   = nIn   ? sumIn   / (double)nIn   : 0.0;
    const double mRing = nRing ? sumRing / (double)nRing : 0.0;
    const double mOut  = nOut  ? sumOut  / (double)nOut  : 0.0;
    std::printf("    luminancia media: dentro del disco %.1f · ANILLO de atmosfera %.1f · espacio %.1f\n",
                mIn, mRing, mOut);
    // ⚠️ EL PERFIL POR ANGULO, porque "el espacio marca 69" no distingue "el halo se sale de la
    // cascara" de "mi banda de espacio esta mal puesta". La atmosfera acaba en `angAtmo`: mas alla el
    // rayo NO corta la cascara y la luz tiene que caer a cero. Si no cae, el halo bordea el planeta
    // como una burbuja y se veria.
    {
        std::printf("    perfil por angulo desde el nadir (el planeta acaba en %.2f, la atmosfera en %.2f):\n",
                    angPlaneta, angAtmo);
        const int NB = 10;
        std::vector<double> acc((size_t)NB, 0.0); std::vector<size_t> cnt((size_t)NB, 0);
        const double a0 = angPlaneta - 1.0, a1 = angPlaneta + 5.0;
        for (int y = 0; y < h; ++y) {
            const float ndcY = rowToNdcY(y, h, rowFlip);
            for (int x = 0; x < w; ++x) {
                const float ndcX = 2.0f * ((float)x + 0.5f) / (float)w - 1.0f;
                glm::vec4 far = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
                const glm::vec3 d = glm::normalize(glm::vec3(far) / far.w);
                if (d.x < 0.3f) continue;                     // solo el lado iluminado
                const double ang = std::acos(glm::clamp((double)glm::dot(d, glm::vec3(nadir)), -1.0, 1.0))
                                 * 180.0 / 3.14159265358979;
                if (ang < a0 || ang >= a1) continue;
                const int b = std::min(NB - 1, (int)((ang - a0) / (a1 - a0) * NB));
                const size_t k = ((size_t)y * w + x) * 4;
                if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
                acc[(size_t)b] += 0.2126*px[k] + 0.7152*px[k+1] + 0.0722*px[k+2];
                ++cnt[(size_t)b];
            }
        }
        for (int b = 0; b < NB; ++b) {
            const double lo2 = a0 + (a1 - a0) * b / NB, hi2 = a0 + (a1 - a0) * (b + 1) / NB;
            std::printf("      %5.2f-%5.2f grados  luminancia %6.1f  (%zu px)%s\n", lo2, hi2,
                        cnt[(size_t)b] ? acc[(size_t)b] / (double)cnt[(size_t)b] : 0.0,
                        cnt[(size_t)b],
                        (lo2 >= angAtmo) ? "   <- espacio: deberia ser ~0"
                                         : ((hi2 <= angPlaneta) ? "   <- sobre el DISCO: es bruma, correcta" : ""));
        }
    }
    // ⚠️ ¿ESE BRILLO DE MAS ES MIO? Se aisla en vez de razonarlo: con `u_planet.x = 0`,
    // `harukaAtmoLimb` sale por su primera linea y no aporta NADA. Lo que quede iluminado en el mismo
    // encuadre no es la atmosfera. Cinco veces hoy he deducido mal la causa de un pixel; esto lo zanja.
    {
        sinPlaneta = true;
        std::vector<uint8_t> pxOff;
        shoot(0.0f, pxOff);
        sinPlaneta = false;
        double sOn = 0.0, sOff = 0.0; size_t n = 0;
        for (int y = 0; y < h; ++y) {
            const float ndcY = rowToNdcY(y, h, rowFlip);
            for (int x = 0; x < w; ++x) {
                const float ndcX = 2.0f * ((float)x + 0.5f) / (float)w - 1.0f;
                glm::vec4 far = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
                const glm::vec3 d = glm::normalize(glm::vec3(far) / far.w);
                const double ang = std::acos(glm::clamp((double)glm::dot(d, glm::vec3(nadir)), -1.0, 1.0))
                                 * 180.0 / 3.14159265358979;
                if (ang < angAtmo * 1.02) continue;             // solo la zona que deberia ser espacio
                const size_t k = ((size_t)y * w + x) * 4;
                if (px[k] == 0xAA || pxOff[k] == 0xAA) continue;
                sOn  += 0.2126*px[k]    + 0.7152*px[k+1]    + 0.0722*px[k+2];
                sOff += 0.2126*pxOff[k] + 0.7152*pxOff[k+1] + 0.0722*pxOff[k+2];
                ++n;
            }
        }
        std::printf("    AISLAMIENTO en la zona de espacio (%zu px): con atmosfera %.1f · SIN ella %.1f\n",
                    n, n ? sOn / (double)n : 0.0, n ? sOff / (double)n : 0.0);
    }
    std::printf("    color del anillo: R %.1f · B %.1f  (Rayleigh pide B >> R)\n",
                nRing ? ringR / (double)nRing : 0.0, nRing ? ringB / (double)nRing : 0.0);
    CHECK(nRing > 0 && nOut > 0, "el cuadro contiene anillo y espacio (si no, la geometria esta mal)");
    CHECK(mRing > mOut + 3.0, "hay LIMBO: el anillo de atmosfera brilla mas que el espacio vacio");
    CHECK(nRing && ringB > ringR * 1.5, "y es AZUL (Rayleigh), no un halo blanco cualquiera");

    // ── CONTRAPRUEBA 1: el lado NOCTURNO no tiene halo. Sin esto, "hay azul alrededor" no distingue
    //    una atmosfera de un anillo pintado sobre la esfera.
    {
        // ⚠️ EL LIMBO NOCTURNO ES OTRO ENCUADRE. Con 10 grados de fov apuntando al limbo diurno, el
        // opuesto no esta ni cerca del cuadro: mi primera version lo buscaba en la misma imagen y
        // encontraba CERO pixeles, o sea que la contraprueba no comparaba nada. Se renderiza aparte,
        // apuntando al limbo del lado contrario al sol.
        const glm::dvec3 fwdN = glm::normalize(nadir * std::cos(aLimb)
                                             - glm::dvec3(1, 0, 0) * std::sin(aLimb));
        const glm::mat4 viewN = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwdN), glm::vec3(0, 1, 0));
        const glm::mat4 invVPN = glm::inverse(proj * glm::mat4(glm::mat3(viewN)));
        std::vector<uint8_t> pxN;
        shootVP(0.0f, invVPN, pxN);
        double sumNoche = 0.0; size_t nNoche = 0;
        for (int y = 0; y < h; ++y) {
            const float ndcY = rowToNdcY(y, h, rowFlip);
            for (int x = 0; x < w; ++x) {
                const float ndcX = 2.0f * ((float)x + 0.5f) / (float)w - 1.0f;
                glm::vec4 far = invVPN * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
                const glm::vec3 d = glm::normalize(glm::vec3(far) / far.w);
                const double ang = std::acos(glm::clamp((double)glm::dot(d, glm::vec3(nadir)), -1.0, 1.0))
                                 * 180.0 / 3.14159265358979;
                if (ang < angPlaneta || ang > angAtmo) continue;
                const size_t k = ((size_t)y * w + x) * 4;
                if (pxN[k] == 0xAA && pxN[k+1] == 0xAA && pxN[k+2] == 0xAA) continue;
                sumNoche += 0.2126*pxN[k] + 0.7152*pxN[k+1] + 0.0722*pxN[k+2];
                ++nNoche;
            }
        }
        const double mN = nNoche ? sumNoche / (double)nNoche : 0.0;
        std::printf("    CONTRAPRUEBA limbo NOCTURNO (%zu px): %.1f · el diurno da %.1f\n",
                    nNoche, mN, mRing);
        CHECK(nNoche > 0, "el limbo nocturno TIENE pixeles que medir (si no, no se compara nada)");
        CHECK(nNoche > 0 && mN < mRing * 0.6,
              "el terminador funciona: el limbo del lado nocturno esta MUCHO mas oscuro");
    }

    // ── CONTRAPRUEBA 2: desde DENTRO no se suma nada. El domo ya modela el aire; sumar las dos cosas
    //    seria contar la atmosfera dos veces, y el cielo a ras de suelo cambiaria sin querer.
    {
        std::vector<uint8_t> pxSuelo;
        SkyUBO probe{};
        (void)probe;
        // `u_atmo = 1` = a ras de suelo. La mezcla debe devolver el domo, con el limbo tapado.
        shoot(1.0f, pxSuelo);
        size_t dif = 0, n = 0;
        for (size_t k = 0; k + 3 < px.size() && k + 3 < pxSuelo.size(); k += 4) {
            if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
            const int dd = std::abs((int)px[k] - (int)pxSuelo[k])
                         + std::abs((int)px[k+1] - (int)pxSuelo[k+1])
                         + std::abs((int)px[k+2] - (int)pxSuelo[k+2]);
            if (dd > 12) ++dif;
            ++n;
        }
        std::printf("    con u_atmo=1 (a ras de suelo) cambia el %.1f %% del cuadro respecto a u_atmo=0\n",
                    n ? 100.0 * (double)dif / (double)n : 0.0);
        CHECK(n && dif > n / 10,
              "dentro y fuera dan cielos DISTINTOS (el limbo es del caso de fuera, no de los dos)");
    }

    g_dev->destroy(pipe); g_dev->destroy(ubo);
}

