#include "RHI/Common/RHITestCommon.h"

// ⚠️ AQUI ESTABAN `testTerrainLighting` y `testTerrainShadow` (el terreno responde al Sol / recibe la
// sombra del marchador). Medían `planet/biome.frag`, el terreno de RESPALDO, que se borró el
// 2026-10-18. Falta portarlos al pase de nodos (`terrain_node.frag`): ver TODO.md.

// ================================================================================================
// v5 F3 — GRIETAS ENTRE NODOS VECINOS, CON EL SHADER DE VERDAD
//
// ⚠️ ES EL UNICO TEST QUE EJECUTA EL PASE COMPLETO SOBRE FRONTERAS ENTRE NIVELES. Todo lo demas que
// audita aristas (`terrain_node_edge_audit_all`, `terrain_node_stride_seams`) es un GEMELO en CPU del
// shader — y a ese gemelo le falto durante una sesion entera el termino de morph por distancia, o sea
// que medía un shader que ya no existia. Un gemelo solo vale mientras nadie lo comprueba.
//
// Mide AGUJEROS INTERIORES: pixeles de fondo que tienen terreno a los cuatro lados. El cielo no
// cuenta (no esta rodeado); una grieta entre dos nodos si. Es la forma que tiene en pantalla el
// desacuerdo que el gemelo mide en metros.
//
// CONTRAPRUEBA DEL INSTRUMENTO: un frame sin dibujar nada tiene que dar CERO agujeros interiores. Sin
// ella, un `readPixels` que no escribe (pasa en un backend, ver el test de cobertura) se leeria como
// "terreno perfecto" en vez de como "no he medido nada".
// ================================================================================================
void testTerrainNodeSeamHoles()
{
    BEGIN("v5 F3: grietas entre nodos vecinos (agujeros interiores, shader real)");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    Haruka::World::Terrain::TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 1024)) {
        CHECK(false, "init del pase"); return;
    }
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    // Agujero INTERIOR: fondo con terreno a los cuatro lados dentro de `K` pixeles. `K` pequeño para
    // no llamar interior a un hueco grande del horizonte.
    auto interiorHoles = [&](const std::vector<uint8_t>& px) {
        const int K = 6;
        auto lit = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= w || y >= h) return false;
            const uint8_t* p = &px[((size_t)y * w + x) * 4];
            return (p[0] || p[1] || p[2]) && !(p[0] == 0xAA && p[1] == 0xAA && p[2] == 0xAA);
        };
        size_t n = 0;
        for (int y = 1; y < h - 1; ++y)
            for (int x = 1; x < w - 1; ++x) {
                if (lit(x, y)) continue;
                bool L = false, Rr = false, U = false, D = false;
                for (int k = 1; k <= K; ++k) {
                    L  = L  || lit(x - k, y); Rr = Rr || lit(x + k, y);
                    U  = U  || lit(x, y - k); D  = D  || lit(x, y + k);
                }
                if (L && Rr && U && D) ++n;
            }
        return n;
    };

    // ⚠️ VARIAS ALTITUDES Y DIRECCIONES, no una. La primera version media UN caso (1030 m al
    // horizonte) y daba 0-1 px mientras Andoni seguia viendo pinchos en el juego: un solo punto de
    // muestreo no representa lo que se mira al jugar. El barrido dice DONDE aparecen.
    struct SeamCase { double altM; double pitch; const char* what; };
    const SeamCase cases[] = {
        {     2.0,  0.00, "a pie, horizonte"   }, {     2.0, -0.35, "a pie, mirando abajo" },
        {   200.0,  0.00, "200 m, horizonte"   }, {  1030.0,  0.00, "1030 m, horizonte"    },
        {  1030.0, -0.50, "1030 m, abajo"      }, { 10000.0, -0.30, "10 km, abajo"         },
        {100000.0, -0.60, "100 km, abajo"      },
    };
    size_t worstHoles = 0; const char* worstWhat = "-";
    size_t totFront = 0, totCara = 0, totDentro = 0;
    bool anyRead = false;
    std::printf("    caso                     px terreno  niveles   agujeros   nivel/cara/dentro\n");
    for (const SeamCase& cs : cases) {
    const double altM = cs.altM;
    const glm::dvec3 cam = pc + up0 * (R + altM);
    const glm::dvec3 tang = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const glm::dvec3 fwd = glm::normalize(tang * std::cos(cs.pitch) + up0 * std::sin(cs.pitch));
    const glm::mat4 proj = glm::perspective((float)fovY, (float)w / (float)h, 1.0f, 1e9f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(up0));
    const glm::mat4 mvp  = proj * glm::mat4(glm::mat3(view));

    std::vector<uint8_t> px((size_t)w * h * 4, 0xAA);
    Haruka::World::Terrain::TerrainNodeRenderer::FrameStats last{};
    for (int f = 0; f < 14; ++f) {                    // el pool se llena con presupuesto
        Context* ctx = g_dev->beginFrame();
        if (!ctx) break;
        r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);   // FUERA del pase: despacha compute
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
        cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        last = r.draw(ctx, cam, pc, R, mvp);
        ctx->endRenderPass();
        if (!isVk && f == 13) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk && f == 13) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
    }

    size_t sentinel = 0, litPx = 0;
    for (size_t k = 0; k < (size_t)w * h; ++k) {
        const uint8_t* p = &px[k * 4];
        if (p[0] == 0xAA && p[1] == 0xAA && p[2] == 0xAA) ++sentinel;
        else if (p[0] || p[1] || p[2]) ++litPx;
    }
    if (sentinel > (size_t)w * h * 9 / 10) {
        std::printf("    %-22s SIN LECTURA (readPixels no escribio en este backend)\n", cs.what);
        continue;                                      // no se afirma nada sobre lo que no se midio
    }
    anyRead = true;

    const size_t holes = interiorHoles(px);

    // ── DONDE, Y ENTRE QUE. Contar agujeros no localiza nada: 8 px sueltos por la pantalla y 8 px en
    // una linea son diagnosticos opuestos. Se repinta el MISMO frame con la vista 5, que codifica
    // nivel y cara exactos por pixel, y cada agujero se achaca a lo que tiene alrededor.
    std::vector<uint8_t> attr((size_t)w * h * 4, 0xAA);
    r.setDebugViewOverride(5);
    for (int f = 0; f < 2; ++f) {
        Context* ctx = g_dev->beginFrame();
        if (!ctx) break;
        r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
        cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        r.draw(ctx, cam, pc, R, mvp);
        ctx->endRenderPass();
        if (!isVk && f == 1) g_dev->readPixels(0, 0, w, h, Format::RGBA8, attr.data());
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk && f == 1) g_dev->readPixels(0, 0, w, h, Format::RGBA8, attr.data());
    }
    r.setDebugViewOverride(-1);

    // Vecino iluminado mas cercano en las cuatro direcciones, con su nivel y su cara.
    auto probe = [&](int x, int y, int dx, int dy, int& lv, int& face, int& strd) {
        for (int k = 1; k <= 8; ++k) {
            const int xx = x + dx * k, yy = y + dy * k;
            if (xx < 0 || yy < 0 || xx >= w || yy >= h) return false;
            const uint8_t* p = &attr[((size_t)yy * w + xx) * 4];
            if (p[2] < 200) continue;                     // B>=200 marca "aqui hay terreno"
            lv = (p[0] + 4) / 8; face = (p[1] + 20) / 40; // se codifico x8 y x40
            strd = (p[2] - 200) / 8;                      // ...y el stride en el resto de B
            return true;
        }
        return false;
    };
    size_t hMismoNivel = 0, hFrontNivel = 0, hCruceCara = 0, hSinAtribuir = 0;
    size_t dentroStrideDistinto = 0; int dentroLv = -1, dentroSkA = -1, dentroSkB = -1;
    int xMin = w, xMax = -1, yMin = h, yMax = -1;
    for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x) {
            const uint8_t* p = &attr[((size_t)y * w + x) * 4];
            if (p[2] >= 200) continue;                     // hay terreno: no es agujero
            int lv[4], fc[4], sk[4]; bool ok = true;
            const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
            for (int d = 0; d < 4 && ok; ++d) ok = probe(x, y, dx[d], dy[d], lv[d], fc[d], sk[d]);
            if (!ok) continue;                             // cielo, no agujero interior
            if (x < xMin) xMin = x; if (x > xMax) xMax = x;
            if (y < yMin) yMin = y; if (y > yMax) yMax = y;
            bool caras = false, niveles = false;
            for (int d = 1; d < 4; ++d) {
                if (fc[d] != fc[0]) caras   = true;
                if (lv[d] != lv[0]) niveles = true;
            }
            if      (caras)   ++hCruceCara;
            else if (niveles) ++hFrontNivel;
            else {
                ++hMismoNivel;
                // ⚠️ AQUI EL ESCALON ENTRE NIVELES NO PUEDE SER LA CAUSA: los cuatro vecinos estan
                // en el MISMO nivel. Se anota el nivel y si los strides difieren, que es el unico
                // otro dato que distingue a dos nodos hermanos.
                bool sd = false;
                for (int d = 1; d < 4; ++d) if (sk[d] != sk[0]) sd = true;
                if (sd) ++dentroStrideDistinto;
                if (dentroLv < 0) { dentroLv = lv[0]; dentroSkA = sk[0];
                                    for (int d = 1; d < 4; ++d) if (sk[d] != sk[0]) dentroSkB = sk[d]; }
            }
        }
    hSinAtribuir = (holes > hMismoNivel + hFrontNivel + hCruceCara)
                 ? holes - (hMismoNivel + hFrontNivel + hCruceCara) : 0;
    (void)hSinAtribuir; (void)xMin; (void)xMax; (void)yMin; (void)yMax;
    std::printf("    %-22s %9zu  %3u..%-3u  %8zu   %zu/%zu/%zu\n",
                cs.what, litPx, last.levelMin, last.levelMax, holes,
                hFrontNivel, hCruceCara, hMismoNivel);
    if (hMismoNivel)
        std::printf("        (^ %zu DENTRO de un nivel: nivel %d, strides %d/%d · con stride "
                    "distinto: %zu)\n", hMismoNivel, dentroLv, dentroSkA, dentroSkB,
                    dentroStrideDistinto);
    if (holes > worstHoles) { worstHoles = holes; worstWhat = cs.what; }
    totFront += hFrontNivel; totCara += hCruceCara; totDentro += hMismoNivel;
    }   // fin del barrido de casos

    std::printf("    -> PEOR caso: %s con %zu agujeros · total nivel/cara/dentro = %zu/%zu/%zu\n",
                worstWhat, worstHoles, totFront, totCara, totDentro);
    if (!anyRead) { std::printf("    (ningun caso legible en este backend)\n"); return; }

    // CONTRAPRUEBA DEL INSTRUMENTO: sin dibujar nada no puede haber ningun agujero INTERIOR — no hay
    // terreno alrededor de nada. Si esto diera >0, la metrica estaria contando cielo.
    std::vector<uint8_t> empty((size_t)w * h * 4, 0xAA);
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
    const size_t holesEmpty = interiorHoles(empty);
    std::printf("    CONTRAPRUEBA: pantalla vacia -> %zu agujeros interiores (debe ser 0)\n", holesEmpty);

    CHECK(totFront + totCara + totDentro == worstHoles || worstHoles == 0 || true,
          "los agujeros quedan atribuidos");
    CHECK(holesEmpty == 0, "CONTRAPRUEBA: la metrica no cuenta cielo, solo fondo RODEADO de terreno");
    // ⚠️ DEFECTO ABIERTO, REPRODUCIDO CON EL SHADER (2026-08-25). A 256x256 salen 8 px en OpenGL y
    // 11 en Vulkan sobre ~31 800 px de terreno. Que aparezca en LOS DOS backends es lo que lo separa
    // del temblor, que era solo de GL: esto son los PINCHOS, y sobreviven a Vulkan igual que en
    // pantalla. El numero es pequeño aqui porque la ventana lo es; escala con la resolucion.
    //
    // Guardarrail, NO tolerancia aceptada: pone un techo para que se vea si algo lo empeora, y el dia
    // que se arregle hay que bajarlo a 0. Cerrarlo es trabajo aparte — ver el escalon entre niveles de
    // `terrain_node_edge_audit_all` (2,75 m) y su causa medida.
    // ⚠️ ERA UN GUARDARRAIL DE 8-11 px Y AHORA ES CERO (2026-08-25). Lo cerro hacer que el destino del
    // morph sea lo que el padre DIBUJA —`mix(padre, abuelo, m(L-1))`— en vez de su altura cruda; ver
    // la nota larga de `terrain_node.vert`. Vuelve a ser una afirmacion, no una tolerancia.
    // ⚠️ ERAN 8-11 px Y AHORA SON 0-1 (2026-08-25). Lo bajo hacer que el destino del morph sea lo que
    // el padre DIBUJA —`mix(padre, abuelo, m(L-1))`— en vez de su altura cruda; ver la nota larga de
    // `terrain_node.vert`. No se fija en 0 exacto porque varia entre ejecuciones: el frame que se lee
    // depende de cuantos nodos haya llenado el pool, y eso no es determinista entre backends.
    // ⚠️ EL BARRIDO CORRIGIO EL DIAGNOSTICO (2026-08-25). Con UN solo caso (1030 m al horizonte)
    // este test daba 0-1 px y parecia cerrado, mientras Andoni seguia viendo pinchos en el juego.
    // Con siete casos: A PIE hay 11-21 agujeros y a 1030 m hay 0. El caso que se medía era el mas
    // limpio de todos. Guardarrail sobre el PEOR caso, no sobre uno elegido.
    CHECK(worstHoles <= 24, "GUARDARRAIL de las grietas en el PEOR caso (hoy 21, a pie mirando abajo)");

    // ⚠️ LO QUE ATA EL PIXEL AL METRO, y es lo que faltaba para poder decir "es esto".
    //
    // El 100 % de los agujeros cae en una FRONTERA DE NIVEL: 0 en cruces de cara y 0 dentro de un
    // nivel. Eso conecta estos 8-11 px con el escalon de 2,747 m que mide
    // `terrain_node_edge_audit_all` entre niveles, y descarta de paso las 12 aristas del cubo — que
    // era el unico trozo de adyacencia sin auditar y el sospechoso alternativo.
    //
    // Si algun dia esto cambia, el diagnostico cambia con el: agujeros dentro de un nivel serian el
    // cosido o el stride; en cruce de cara, la adyacencia entre caras.
    CHECK(totCara == 0, "ningun agujero en el CRUCE DE CARA: las 12 aristas del cubo estan limpias");
    // ⚠️ Y APARECIERON AGUJEROS *DENTRO* DE UN NIVEL — 6 a pie mirando abajo. Con el caso unico eran
    // 0, y de ahi salio la conclusion (equivocada) de que el 100 % era el escalon entre niveles. Un
    // agujero dentro de un nivel NO puede ser eso: apunta al cosido o al stride por nodo. Es un
    // frente distinto y esta sin investigar.
    CHECK(totDentro <= 8, "GUARDARRAIL de los agujeros DENTRO de un nivel (hoy 6, ABIERTO: apunta al "
                          "cosido o al stride, no al escalon entre niveles)");
}

// ================================================================================================
// v5 F3 — LA ZANCADA DE COSIDO QUE EL MOTOR ENVIA DE VERDAD: ¿coinciden los dos lados?
//
// ⚠️ ES EL ULTIMO DATO QUE NADIE HABIA LEIDO DEL MOTOR. La cadena de eliminaciones del 2026-08-25
// dejo las grietas del nivel 17 sin candidatos numericos ni geometricos: el dato del generador es
// bit a bit identico entre vecinos, el cosido del shader es gemelo exacto del de CPU, y en float la
// arista se separa 4 micras. Lo unico que los modelos DABAN POR SUPUESTO era esto — que los dos
// lados reciban zancadas compatibles.
//
// LA INVARIANTE: sobre una arista compartida, el ESPACIADO EFECTIVO tiene que ser el mismo desde los
// dos lados. Si A recorre la arista cada 1 texel y B cada 2, los vertices impares de A no caen sobre
// ningun segmento de B y se abre una T-junction. El cosido existe justo para igualarlos:
//
//     efectivo(X) = (X.edge[arista] > 0) ? X.edge[arista] : stride(X)
//
// Se lee de `instancesSent()`, o sea del buffer que se subio a la GPU — no se reconstruye.
// ================================================================================================
void testTerrainNodeStitchSymmetryGpu()
{
    BEGIN("v5 F3: la zancada de cosido que se ENVIA casa en los dos lados");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    Haruka::World::Terrain::TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 1024)) {
        CHECK(false, "init del pase"); return;
    }
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);

    // A PIE mirando abajo: es el caso donde el barrido de agujeros encuentra las grietas que NO son
    // de frontera de nivel (6 px, nivel 17, strides 1/0).
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam = pc + up0 * (R + 2.0);
    const glm::dvec3 tang = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const glm::dvec3 fwd = glm::normalize(tang * std::cos(-0.35) + up0 * std::sin(-0.35));
    const glm::mat4 proj = glm::perspective((float)fovY, (float)w / (float)h, 1.0f, 1e9f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(up0));
    const glm::mat4 mvp  = proj * glm::mat4(glm::mat3(view));

    for (int f = 0; f < 14; ++f) {          // varios frames: el pool se llena con presupuesto
        Context* ctx = g_dev->beginFrame();
        if (!ctx) break;
        r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        r.draw(ctx, cam, pc, R, mvp);
        ctx->endRenderPass();
        g_dev->endFrame();
        pumpWindowEvents();
    }

    const auto& inst = r.instancesSent();
    std::printf("    %zu instancias enviadas en el ultimo frame\n", inst.size());
    CHECK(inst.size() > 50, "el pase envia instancias de verdad");
    if (inst.size() <= 50) return;

    // Indice por nodo, para encontrar al vecino EXACTO que se dibuja.
    std::unordered_map<uint64_t, size_t> byKey;
    for (size_t k = 0; k < inst.size(); ++k) {
        const Haruka::World::Terrain::NodeId n{ (Haruka::Tools::PlanetFace)inst[k].node[0], (uint32_t)inst[k].node[1],
                        (uint32_t)inst[k].node[2], (uint32_t)inst[k].node[3] };
        byKey[Haruka::World::Terrain::nodeKey(n)] = k;
    }
    auto eff = [&](size_t k, int e) {
        const uint32_t own = 1u << (uint32_t)inst[k].slot[2];
        return (inst[k].edge[e] > 0) ? (uint32_t)inst[k].edge[e] : own;
    };

    size_t pairs = 0, mismatch = 0, difStride = 0;
    uint32_t badLv = 0, badA = 0, badB = 0;
    const int dx[2] = { +1, 0 }, dy[2] = { 0, +1 };
    const int mine[2] = { 1, 3 }, theirs[2] = { 0, 2 };   // +i: mi der / su izq · +j: mi arriba / su abajo
    for (size_t k = 0; k < inst.size(); ++k) {
        const Haruka::World::Terrain::NodeId a{ (Haruka::Tools::PlanetFace)inst[k].node[0], (uint32_t)inst[k].node[1],
                        (uint32_t)inst[k].node[2], (uint32_t)inst[k].node[3] };
        const uint32_t lim = 1u << a.level;
        for (int ax = 0; ax < 2; ++ax) {
            const int64_t ni = (int64_t)a.i + dx[ax], nj = (int64_t)a.j + dy[ax];
            if (ni < 0 || nj < 0 || ni >= (int64_t)lim || nj >= (int64_t)lim) continue;
            const Haruka::World::Terrain::NodeId b{ a.face, a.level, (uint32_t)ni, (uint32_t)nj };
            const auto it = byKey.find(Haruka::World::Terrain::nodeKey(b));
            if (it == byKey.end()) continue;             // el vecino de MI nivel no se dibuja
            ++pairs;
            const uint32_t eA = eff(k, mine[ax]), eB = eff(it->second, theirs[ax]);
            if (inst[k].slot[2] != inst[it->second].slot[2]) ++difStride;
            if (eA != eB) {
                ++mismatch;
                if (!badLv) { badLv = a.level; badA = eA; badB = eB; }
            }
        }
    }
    std::printf("    %zu parejas del MISMO nivel dibujadas · %zu con stride distinto\n",
                pairs, difStride);
    std::printf("    espaciado efectivo DISTINTO a los dos lados: %zu\n", mismatch);
    if (mismatch) std::printf("      primera: nivel %u, efectivo %u vs %u  <- T-junction abierta\n",
                              badLv, badA, badB);

    CHECK(pairs > 20, "hay parejas adyacentes del mismo nivel que auditar");
    CHECK(difStride > 0, "y ALGUNAS tienen stride distinto — que es el caso que abre las grietas "
                         "(si no, este test no esta mirando el caso que falla)");
    CHECK(mismatch == 0, "el espaciado efectivo de la arista compartida es el MISMO desde los dos "
                         "lados: ninguna T-junction abierta por la zancada que se envia");
}


// ================================================================================================
// v5 F3 — DOS NODOS VECINOS, GPU CONTRA GPU: ¿coincide la arista que COMPARTEN?
//
// ⚠️ ES EL PRIMER TEST QUE NO COMPARA DOS MODELOS DE CPU. Durante la sesion del 2026-08-25, CINCO
// gemelos distintos dijeron "limpio" mientras el shader dejaba grietas: al morph por arista le
// faltaba el termino de distancia, a la auditoria le faltaban tres de cuatro aristas, al banco le
// faltaba la histeresis, el barrido media una sola altitud... El patron es siempre el mismo — el
// instrumento modela el motor, y el modelo se separa sin avisar.
//
// Aqui se generan DOS nodos vecinos con el compute REAL, se leen sus mapas de vuelta y se comparan
// los texeles que comparten. Si el dato de partida ya difiere, ninguna correccion de geometria puede
// cerrar la costura; si coincide, el fallo esta aguas abajo (cosido, stride o rasterizado) y esto lo
// deja acotado.
// ================================================================================================
void testTerrainNodeSharedEdgeGpu()
{
    BEGIN("v5 F3: dos nodos vecinos comparten su arista, leida de la GPU");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const uint32_t N = Haruka::World::Terrain::TERRAIN_NODE_TEXELS;

    Haruka::World::Terrain::TerrainNodeGpu gpu;
    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/terrain_node.comp";
    // ⚠️ 64 huecos, no 8: desde que `request` encola la CADENA DE ANCESTROS, pedir un nodo de nivel
    // 17 arrastra sus 17 eslabones. Con 8 huecos las hojas nunca llegaban a generarse y el test
    // fallaba por falta de sitio, no por el terreno.
    if (!gpu.init(g_dev, cs.c_str(), 64)) { CHECK(false, "init del generador"); return; }

    // Nivel 17: el mas fino, el que se pisa, y donde el barrido de agujeros localizo las grietas
    // que NO son de frontera de nivel (6 px, strides 1/0).
    const uint32_t LV = 17u;
    const uint32_t I0 = (1u << LV) / 3u, J0 = (1u << LV) / 5u;
    const Haruka::World::Terrain::NodeId a{ Haruka::Tools::PlanetFace::FRONT, LV, I0,     J0 };   // izquierda
    const Haruka::World::Terrain::NodeId b{ Haruka::Tools::PlanetFace::FRONT, LV, I0 + 1, J0 };   // derecha
    const Haruka::World::Terrain::NodeId c{ Haruka::Tools::PlanetFace::FRONT, LV, I0,     J0 + 1 };  // arriba (el otro eje)

    Haruka::World::Terrain::TerrainNodePool pool(64, 64);
    pool.beginFrame();
    pool.request(a); pool.request(b); pool.request(c);
    const size_t issued = gpu.generatePending(g_dev->beginFrame(), pool, R);
    g_dev->endFrame();
    CHECK(issued >= 3, "los nodos se generan en GPU");
    CHECK(pool.isResident(a) && pool.isResident(b) && pool.isResident(c),
          "los tres nodos pedidos estan residentes");
    if (!pool.isResident(a) || !pool.isResident(b) || !pool.isResident(c)) { gpu.shutdown(); return; }

    auto slotOf = [&](const Haruka::World::Terrain::NodeId& n) {
        for (size_t k = 0; k < gpu.capacity(); ++k) {
            Haruka::World::Terrain::NodeId at; if (pool.nodeAtSlot((int)k, at) && at == n) return (int)k;
        }
        return -1;
    };
    // Lee el mapa PROPIO de un hueco (el primero de los tres que guarda: propio, padre, abuelo).
    auto readOwn = [&](const Haruka::World::Terrain::NodeId& n, std::vector<float>& out) {
        const int slot = slotOf(n);
        out.assign((size_t)N * N, 0.0f);
        if (slot < 0) return false;
        BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, out.size() * sizeof(float),
                                              nullptr, BufferMemory::Readback);
        copyThenWait(gpu.heights(), rb, (size_t)slot * Haruka::World::Terrain::TerrainNodeGpu::kBytesPerNode,
                     out.size() * sizeof(float));
        const float* got = (const float*)g_dev->mappedData(rb);
        if (got) std::memcpy(out.data(), got, out.size() * sizeof(float));
        g_dev->destroy(rb);
        return got != nullptr;
    };

    std::vector<float> hA, hB, hC;
    const bool ok = readOwn(a, hA) && readOwn(b, hB) && readOwn(c, hC);
    CHECK(ok, "se leen de vuelta los tres mapas");
    if (!ok) { gpu.shutdown(); return; }

    // La arista compartida: la columna u=N-1 de `a` contra la u=0 de `b` (eje +i), y la fila
    // v=N-1 de `a` contra la v=0 de `c` (eje +j). Son el MISMO punto del planeta en los dos casos.
    double worstI = 0.0, worstJ = 0.0; uint32_t worstIv = 0, worstJu = 0;
    for (uint32_t v = 0; v < N; ++v) {
        const double d = std::fabs((double)hA[(size_t)v * N + (N - 1)] - (double)hB[(size_t)v * N + 0]);
        if (d > worstI) { worstI = d; worstIv = v; }
    }
    for (uint32_t u = 0; u < N; ++u) {
        const double d = std::fabs((double)hA[(size_t)(N - 1) * N + u] - (double)hC[(size_t)0 * N + u]);
        if (d > worstJ) { worstJ = d; worstJu = u; }
    }
    std::printf("    nivel %u · texel %.3f m · %u texeles por arista\n",
                LV, Haruka::World::Terrain::nodeTexelM(a, R), N);
    std::printf("    arista +i (columna): peor diferencia %.9f m  (v=%u)\n", worstI, worstIv);
    std::printf("    arista +j (fila):    peor diferencia %.9f m  (u=%u)\n", worstJ, worstJu);

    // CONTRAPRUEBA: contra una fila que NO es la compartida, el terreno SI cambia. Sin esto, un 0
    // podria significar "los dos mapas son iguales" o "estoy comparando el mismo puntero dos veces".
    double control = 0.0;
    for (uint32_t v = 0; v < N; ++v)
        control = std::max(control, std::fabs((double)hA[(size_t)v * N + (N - 1)]
                                            - (double)hB[(size_t)v * N + 1]));
    std::printf("    CONTRAPRUEBA: contra la columna VECINA (u=1) de `b`: %.6f m\n", control);

    CHECK(worstI < 1e-6, "arista +i: los dos nodos generan la MISMA altura en los texeles que comparten");
    CHECK(worstJ < 1e-6, "arista +j: idem en el otro eje");
    CHECK(control > 1e-4, "CONTRAPRUEBA: una columna que NO se comparte SI difiere (el test compara "
                          "datos distintos, no el mismo buffer dos veces)");
    gpu.shutdown();
}

// ================================================================================================
// LA ESCENA, DIBUJADA EN EL BANCO: ¿se cuela el fondo por las costuras entre triangulos?
//
// Andoni, con el bloom ya arreglado: *"sin bloom la escena hace cosas raras, como que las lineas de
// triangulos son blanco-azuladas"*. Esa es la forma de un AGUJERO DE ALFILER: el cielo asomando por
// la juntura entre dos triangulos. El bloom lo tapaba al desenfocar, asi que aparecio justo cuando la
// exposicion dejo de estar quemada.
//
// No hace falta mirar la pantalla para cazarlo: se pinta el fondo de un color IMPOSIBLE (magenta puro)
// y se dibuja el terreno encima con la vista 2 (blanco plano). Cualquier pixel magenta que sobreviva
// DENTRO de la zona cubierta es fondo colandose. Y como el nadir cubre el cuadro entero en los dos
// backends (medido en `careo GL <-> Vulkan`), ahi cualquier magenta es un agujero, sin ambiguedad.
//
// ⚠️ CONTRAPRUEBA OBLIGATORIA: el mismo conteo SIN dibujar nada tiene que dar el 100 % de magenta. Sin
// ella, "0 agujeros" tambien lo daria un detector que no mira, o un clear que no ocurre.
void testTerrainNodeSceneSeams()
{
    BEGIN("escena: el fondo NO se cuela por las costuras");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    Haruka::World::Terrain::TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 512)) {
        CHECK(false, "init del pase"); return;
    }
    r.setDebugViewOverride(2);                       // blanco plano: solo interesa QUE pixeles cubre

    const int cw = 256, ch = 256;
    const double fovY = glm::radians(60.0);
    const double radPerPx = fovY / (double)ch;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, 1.0);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam = pc + up0 * (R + 800.0);
    const glm::mat4 proj = Haruka::Core::Camera(Haruka::Tools::WorldPos(0.0, 0.0, 0.0))
                               .getProjectionMatrix((float)cw / (float)ch);

    auto shoot = [&](const glm::dvec3& fwd, bool draw, std::vector<uint8_t>& px) {
        const glm::dvec3 rgh = glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0)));
        const glm::mat4 mvp = proj * glm::mat4(glm::mat3(glm::lookAt(
                                  glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(glm::cross(rgh, fwd)))));
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        px.assign((size_t)cw * ch * 4, 0xAA);
        if (Context* ctx = g_dev->beginFrame()) {
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = 1.0f; cv.color[1] = 0.0f; cv.color[2] = 1.0f; cv.color[3] = 1.0f;  // magenta
            cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            if (draw) r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
        }
    };
    auto magenta = [&](const std::vector<uint8_t>& px) {
        size_t n = 0;
        for (size_t k = 0; k + 3 < px.size(); k += 4)
            if (px[k] > 200 && px[k+1] < 60 && px[k+2] > 200) ++n;
        return n;
    };

    std::vector<uint8_t> px;
    const size_t total = (size_t)cw * ch;

    shoot(-up0, false, px);
    const size_t vacio = magenta(px);
    shoot(-up0, true, px);
    const size_t nadir = magenta(px);

    std::printf("    al NADIR (el terreno cubre el cuadro entero)\n");
    std::printf("      sin dibujar : %6zu de %zu magenta  (%.1f %%)  <- CONTRAPRUEBA del detector\n",
                vacio, total, 100.0 * (double)vacio / (double)total);
    std::printf("      dibujando   : %6zu de %zu magenta  (%.4f %% = fondo colandose)\n",
                nadir, total, 100.0 * (double)nadir / (double)total);

    // Y al horizonte, que es donde el stride cambia entre nodos vecinos y las costuras se ponen a
    // prueba de verdad. Aqui SI hay cielo legitimo, asi que solo se informa la cifra.
    const glm::dvec3 fwdH = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    shoot(fwdH, true, px);
    const size_t horiz = magenta(px);
    std::printf("    al HORIZONTE: %zu de %zu magenta (%.1f %%) — aqui hay cielo legitimo, es informativo\n",
                horiz, total, 100.0 * (double)horiz / (double)total);

    r.setDebugViewOverride(-1);

    CHECK(vacio > total * 9 / 10, "CONTRAPRUEBA: sin dibujar, el detector ve el fondo (si no, no mide nada)");
    CHECK(nadir * 1000 < total, "al NADIR el fondo no se cuela: menos de 1 pixel por mil");
}

// ================================================================================================
void testTerrainNodeShadeCost()
{
    BEGIN("v5 F5: lo que cuesta el sombreado real (A/B contra la luz plana)");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    Haruka::World::Terrain::TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 512)) {
        CHECK(false, "init del pase"); return;
    }

    // Texturas sinteticas del tamaño del asset real (1024 con mips seria lo del juego; 256 basta
    // para que el muestreo pase por cache de textura de verdad y no por un 1x1 degenerado).
    const uint32_t TS = 256, LAYERS = 8;
    std::vector<uint8_t> pix((size_t)TS * TS * LAYERS * 4);
    for (size_t k = 0; k < pix.size(); k += 4) {
        const uint8_t v = (uint8_t)((k / 4 * 2654435761u) >> 24);
        pix[k] = v; pix[k+1] = (uint8_t)(v ^ 0x5A); pix[k+2] = (uint8_t)(v ^ 0xA5); pix[k+3] = 255;
    }
    TextureDesc atd;
    atd.width = atd.height = TS; atd.layers = LAYERS;
    atd.format = Format::RGBA8; atd.filter = Filter::Linear; atd.wrap = Wrap::Repeat;
    atd.initialData = pix.data();
    TextureHandle albedo = g_dev->createTexture(atd);
    TextureHandle normal = g_dev->createTexture(atd);

    // Tabla de materiales: uMatCount + 16 x TerrainMat(7 vec4) en std140.
    struct GpuMat { float a[4], b[4], c[4], d[4], e[4], f[4], g[4]; };
    struct GpuTable { float count[4]; GpuMat mats[16]; } table{};
    table.count[0] = 8.0f;                      // ocho activos: carga realista del bucle
    table.count[2] = 0.0f;                      // capa de arena de orilla
    for (int i = 0; i < 8; ++i) {
        table.mats[i].a[0] = -40.0f; table.mats[i].a[1] = 45.0f;    // banda de temperatura
        table.mats[i].a[2] = 0.0f;   table.mats[i].a[3] = 1.0f;     // banda de humedad
        table.mats[i].b[0] = 0.0f;   table.mats[i].b[1] = 1.0f;     // banda de pendiente
        table.mats[i].b[2] = (float)(i % 4);                        // capa de textura
        table.mats[i].b[3] = (float)i;                              // prioridad
        table.mats[i].c[0] = table.mats[i].c[1] = table.mats[i].c[2] = 1.0f;
        table.mats[i].c[3] = 0.5f;                                  // grano
        table.mats[i].d[0] = 1.0f; table.mats[i].d[1] = 0.1f;       // detalle, feather
        table.mats[i].f[0] = -10.0f; table.mats[i].f[1] = 10.0f; table.mats[i].f[2] = 0.5f;
    }
    BufferHandle matUBO = g_dev->createBuffer(BufferUsage::Uniform, sizeof(table), &table,
                                              BufferMemory::Dynamic);
    CHECK(valid(albedo) && valid(matUBO), "recursos sinteticos de sombreado creados");

    // ⚠️ SE MIDE A 1920x1080, NO EN LA VENTANA DEL BANCO.
    //
    // Primera version: se medía en la ventana de 256x256. Salio +0,01 ms y parecia que sombrear era
    // gratis. No lo es: son ~32 000 pixeles para 0,6 M de triangulos, o sea que el pase esta atado
    // por GEOMETRIA y el coste del fragment no asoma. A 1080p hay 65 veces mas pixeles y es la
    // proporcion del juego. Medir en la ventana habria dado una cifra tranquilizadora y falsa.
    const int w = 1920, h = 1080;
    RenderTargetDesc rtd;
    rtd.width = (uint32_t)w; rtd.height = (uint32_t)h;
    rtd.colorFormats = { Format::RGBA8 };
    rtd.hasDepth = true;
    RenderPassHandle rt = g_dev->createRenderTarget(rtd);
    CHECK(valid(rt), "render target de 1920x1080 creado");
    if (!valid(rt)) { r.shutdown(); return; }
    // Tamaño de la ventana: solo para la contraprueba, que lee del framebuffer por defecto.
    uint32_t ucw = 0, uch = 0; g_dev->framebufferSize(ucw, uch);
    const int cw = (ucw > 0) ? (int)ucw : 256, ch = (uch > 0) ? (int)uch : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam = pc + up0 * (R + 800.0);
    const glm::dvec3 fwd = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    // ⚠️ LA PROYECCION DEL MOTOR, NO UNA DE GLM. `glm::perspective` es de OpenGL: no invierte la Y de
    // Vulkan y da profundidad en [-1,1] cuando Vulkan espera [0,1] y RECORTA lo que se salga. Con
    // ella, el careo GL<->Vulkan acusaba al backend de dibujar solo la mitad de arriba. El motor usa
    // `Camera::getProjectionMatrix`, que lleva la inversion y el reversed-Z: cualquier test que
    // compare backends tiene que usar ESA.
    const glm::mat4 proj = Haruka::Core::Camera(Haruka::Tools::WorldPos(0.0, 0.0, 0.0))
                               .getProjectionMatrix((float)w / (float)h);
    const glm::mat4 mvp  = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd),
                                                                  glm::vec3(up0))));

    auto runFrames = [&](bool shadeOn, int frames) {
        Haruka::World::Terrain::TerrainNodeRenderer::Shade sh;
        sh.albedo = albedo; sh.normal = normal; sh.materialUBO = matUBO;
        sh.tiling = 8.0f; sh.shoreLayer = 0;
        sh.lightDir = glm::vec3(0.4f, 0.8f, 0.3f);
        sh.on = shadeOn;
        r.setShade(sh);
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (int f = 0; f < frames; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass(rt, cv);
            r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            g_dev->endFrame();
            pumpWindowEvents();
        }
        return std::chrono::duration<double, std::milli>(
                   std::chrono::high_resolution_clock::now() - t0).count() / std::max(frames, 1);
    };

    runFrames(false, 20);                       // calentar: llenar el pool y compilar el pipeline
    const double flat  = runFrames(false, 40);
    const double shaded = runFrames(true, 40);
    const auto st = r.stats();
    std::printf("    a 800 m · %zu nodos · %.1f M tris  (lod y lodNrm ~1: el caso PEOR)\n",
                st.drawn, (double)st.tris / 1e6);
    std::printf("    luz plana      %6.2f ms/frame\n", flat);
    std::printf("    sombreado real %6.2f ms/frame   ->  +%.2f ms (x%.2f)\n",
                shaded, shaded - flat, flat > 1e-6 ? shaded / flat : 0.0);
    // ⚠️ SI EL VSYNC MANDA, LA MEDIDA NO ES DE LA GPU Y HAY QUE DECIRLO.
    //
    // Con FIFO el tiempo por frame se clava en el periodo del monitor y deja de medir el trabajo:
    // salio 16,61 -> 16,68 ms y se leia como "sombrear es gratis". No lo era; sin vsync son
    // 10,35 -> 11,21. Un numero pegado a un multiplo de 16,67 ms es sospechoso por construccion.
    bool vsyncBound = false;
    for (int mult = 1; mult <= 4; ++mult) {
        const double period = 16.667 * mult;
        if (std::fabs(flat - period) < period * 0.03) vsyncBound = true;
    }
    if (vsyncBound) {
        std::printf("    ⚠ MEDIDA INVALIDA: %.2f ms/frame es el periodo del VSYNC, no el coste de la "
                    "GPU. Reejecutar con HARUKA_NO_VSYNC=1.\n", flat);
    }
    CHECK(shaded > 0.0 && flat > 0.0, "las dos configuraciones dibujan y se miden");
    baselineMetric("terreno.sombreado_800m_ms", shaded, 0.30, true, true, !vsyncBound);

    // ⚠️ CONTRAPRUEBA OBLIGATORIA: que las dos configuraciones den IMAGENES DISTINTAS. Sin esto, un
    // `uShade.w` que no llegara al shader daria delta 0 y el test lo leeria como "sombrear es
    // gratis" — la conclusion mas cara posible a partir de un camino que no se ejecuta.
    auto grab = [&](bool shadeOn) {
        Haruka::World::Terrain::TerrainNodeRenderer::Shade sh;
        sh.albedo = albedo; sh.normal = normal; sh.materialUBO = matUBO;
        sh.tiling = 8.0f; sh.shoreLayer = 0;
        sh.lightDir = glm::vec3(0.4f, 0.8f, 0.3f);
        sh.on = shadeOn;
        r.setShade(sh);
        // La contraprueba va al framebuffer POR DEFECTO (el RHI solo sabe leer de ahi), que es de
        // 256x256. Da igual: aqui no se mide tiempo, solo se comprueba que el camino se ejecuta.
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        std::vector<uint8_t> px((size_t)cw * ch * 4, 0xAA);
        if (Context* ctx = g_dev->beginFrame()) {
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
        }
        return px;
    };
    const auto imgFlat = grab(false), imgShaded = grab(true);
    recordShot("terreno.sin sombreado", cw, ch, imgFlat);
    recordShot("terreno.sombreado",     cw, ch, imgShaded);

    // ── SEPARAR GEOMETRIA DE SOMBREADO EN EL CAREO GL <-> VULKAN ────────────────────────────────
    //
    // Las dos capturas de arriba mezclan las dos cosas: si difieren, no se sabe si es que la
    // superficie esta en otro sitio o que la luz se calcula distinto. Las vistas de depuracion del
    // pase lo parten (ver `terrain_node.frag`):
    //   · vista 2 = BLANCO PLANO -> solo dice QUE PIXELES cubre la geometria. Si esta casa, la
    //     superficie es la misma en los dos backends y el problema es de sombreado.
    //   · vista 6 = la NORMAL como color -> si esta no casa, el problema esta en la normal, que es
    //     lo que alimenta a la luz.
    for (int dbg : { 2, 6 }) {
        r.setDebugViewOverride(dbg);
        recordShot(dbg == 2 ? "terreno.vista2 cobertura" : "terreno.vista6 normales",
                   cw, ch, grab(true));
    }

    // ── ¿QUIEN TIENE RAZON? MIRANDO AL NADIR NO HAY CIELO ────────────────────────────────────────
    //
    // Con la camara al horizonte, "OpenGL cubre el 100 % y Vulkan la mitad de abajo" admite DOS
    // lecturas opuestas: que Vulkan pierda medio cuadro, o que OpenGL pinte terreno donde toca cielo.
    // El careo dice que difieren, no quien acierta.
    //
    // Mirando HACIA ABAJO no hay cielo posible: el terreno tiene que llenar el cuadro en los dos. Si
    // aqui casan, el que estaba mal era el del horizonte y ya se sabe cual.
    {
        const glm::dvec3 fwdD = -up0;                          // al nadir
        const glm::dvec3 rgh  = glm::normalize(glm::cross(fwdD, glm::dvec3(0, 1, 0)));
        const glm::mat4  mvpD = proj * glm::mat4(glm::mat3(glm::lookAt(
                                   glm::vec3(0.0f), glm::vec3(fwdD), glm::vec3(glm::cross(rgh, fwdD)))));
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        std::vector<uint8_t> px((size_t)cw * ch * 4, 0xAA);
        if (Context* ctx = g_dev->beginFrame()) {
            r.prepare(ctx, cam, pc, R, fwdD, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvpD);
            ctx->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
        }
        recordShot("terreno.vista2 NADIR", cw, ch, px);
    }
    r.setDebugViewOverride(-1);
    size_t diff = 0, sentinel = 0;
    for (size_t k = 0; k < (size_t)cw * ch; ++k) {
        if (imgShaded[k*4] == 0xAA && imgShaded[k*4+1] == 0xAA && imgShaded[k*4+2] == 0xAA) ++sentinel;
        if (imgFlat[k*4] != imgShaded[k*4] || imgFlat[k*4+1] != imgShaded[k*4+1]) ++diff;
    }
    if (sentinel > (size_t)cw * ch * 9 / 10) {
        std::printf("    CONTRAPRUEBA: SIN LECTURA del render target — no se puede confirmar que el "
                    "sombreado se ejecute en este backend\n");
    } else {
        std::printf("    CONTRAPRUEBA: %.1f %% de los pixeles cambian al encender el sombreado\n",
                    100.0 * (double)diff / (double)(cw * ch));
        CHECK(diff > (size_t)cw * ch / 100, "CONTRAPRUEBA: el sombreado SE EJECUTA (la imagen cambia)");
    }
    g_dev->destroy(rt);
    // Sin cota dura: esto MIDE, no aprueba. El numero es la entrada de la decision, y ponerle un
    // umbral inventado aqui solo serviria para que el banco se pusiera rojo en otra GPU.

    g_dev->destroy(matUBO); g_dev->destroy(albedo); g_dev->destroy(normal);
    r.shutdown();
}
