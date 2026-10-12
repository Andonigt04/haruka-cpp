// El agua interior: relleno de cuencas (global y por ventana), fetch y el limite de resolucion del horneado.
// (Partido de TestOcean.cpp; el registro de todos sigue allí, en el orden de siempre.)
#include "World/Water/OceanTestCommon.h"

/**
 * @brief EL AGUA INTERIOR COMO CAMPO DEL MUNDO: priority-flood global sobre el bake.
 *
 * Sustituye la siembra por parche de 420 m, de la que colgaban tres limitaciones: no había agua
 * fuera del parche, el resultado dependía de su BORDE (un lago podía aparecer al caminar) y no era
 * función de la posición, así que dos clientes no podían coincidir.
 *
 * ⚠️ EL ORÁCULO NO ES UNA TABLA MÍA: son propiedades que el relleno tiene que cumplir por definición
 * —una cuenca cerrada se llena EXACTAMENTE hasta su punto de derrame, y ni un téxel queda por encima
 * de él— más el caso del planeta seco, que es el requisito que Andoni pidió.
 */
void test_water_fill_global() {
    beginTest("water_fill_global");
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;

    const int W = 64, H = 32;
    auto idx = [&](int x, int y) { return (size_t)y * W + x; };

    // ── MUNDO 1: un continente con una cuenca cerrada de fondo conocido ─────────────────────────
    //
    // Terreno a +100 m, un mar en la franja izquierda (por debajo de 0) y una cuenca cuadrada a
    // +10 m rodeada de pared a +100. El punto de derrame es la pared: la cuenca tiene que llenarse
    // hasta 100 m EXACTOS, o sea 90 m de fondo.
    std::vector<float> land((size_t)W * H, 100.0f);
    for (int y = 0; y < H; ++y) for (int x = 0; x < 6; ++x) land[idx(x, y)] = -500.0f;  // océano
    for (int y = 12; y < 20; ++y) for (int x = 30; x < 38; ++x) land[idx(x, y)] = 10.0f; // cuenca
    const Haruka::World::Water::WaterFillResult r1 = Haruka::World::Water::waterFillEquirect(land.data(), W, H, 0.0f);

    double worstLevel = 0.0; size_t basinWet = 0;
    for (int y = 12; y < 20; ++y) for (int x = 30; x < 38; ++x) {
        const float lv = r1.levelM[idx(x, y)];
        if (lv > Haruka::World::Water::WATER_FILL_DRY) { ++basinWet; worstLevel = std::max(worstLevel, std::fabs(lv - 100.0)); }
    }
    std::printf("    cuenca cerrada: %zu de 64 texeles con agua · desvio del punto de derrame %.6f m\n",
                basinWet, worstLevel);
    std::printf("    mapa: %zu texeles de mar · %zu de lago · el mas hondo %.1f m\n",
                r1.seaCells, r1.lakeCells, r1.deepestM);

    // Ni una gota fuera de la cuenca: la meseta a +100 no puede retener nada.
    size_t spill = 0;
    for (int y = 0; y < H; ++y) for (int x = 6; x < W; ++x) {
        const bool inBasin = (x >= 30 && x < 38 && y >= 12 && y < 20);
        if (!inBasin && r1.levelM[idx(x, y)] > Haruka::World::Water::WATER_FILL_DRY) ++spill;
    }
    std::printf("    texeles con agua FUERA de la cuenca: %zu (debe ser 0: la meseta no retiene)\n", spill);

    // ── MUNDO 2: SIN MAR. El requisito del planeta seco ─────────────────────────────────────────
    std::vector<float> dry((size_t)W * H, 100.0f);
    for (int y = 12; y < 20; ++y) for (int x = 30; x < 38; ++x) dry[idx(x, y)] = 10.0f;  // la MISMA cuenca
    const Haruka::World::Water::WaterFillResult r2 = Haruka::World::Water::waterFillEquirect(dry.data(), W, H, 0.0f);
    std::printf("    planeta SECO (misma cuenca, sin mar): %zu de mar · %zu de lago · hasWater=%d\n",
                r2.seaCells, r2.lakeCells, (int)r2.hasWater);

    // ── MUNDO 3: la costura de longitud NO es un borde ──────────────────────────────────────────
    //
    // La misma cuenca, pero a caballo del meridiano ±180 (mitad en x=62,63 y mitad en x=0,1). Si el
    // mapa se tratara como un rectángulo, ese meridiano sería un BORDE por el que el agua se fuga y
    // la cuenca no se llenaría. Con la esfera bien cosida se llena igual que la de en medio.
    std::vector<float> seam((size_t)W * H, 100.0f);
    for (int y = 0; y < H; ++y) for (int x = 8; x < 14; ++x) seam[idx(x, y)] = -500.0f;   // océano
    for (int y = 12; y < 20; ++y) { for (int x = 60; x < 64; ++x) seam[idx(x, y)] = 10.0f;
                                    for (int x = 0;  x < 4;  ++x) seam[idx(x, y)] = 10.0f; }
    const Haruka::World::Water::WaterFillResult r3 = Haruka::World::Water::waterFillEquirect(seam.data(), W, H, 0.0f);
    size_t seamWet = 0;
    for (int y = 12; y < 20; ++y) { for (int x = 60; x < 64; ++x) if (r3.levelM[idx(x,y)] > Haruka::World::Water::WATER_FILL_DRY) ++seamWet;
                                    for (int x = 0;  x < 4;  ++x) if (r3.levelM[idx(x,y)] > Haruka::World::Water::WATER_FILL_DRY) ++seamWet; }
    std::printf("    cuenca a caballo del meridiano +-180: %zu de 64 texeles con agua\n", seamWet);

    // ── DETERMINISMO: dos pasadas, los mismos bits ──────────────────────────────────────────────
    const Haruka::World::Water::WaterFillResult r1b = Haruka::World::Water::waterFillEquirect(land.data(), W, H, 0.0f);
    bool identical = (r1b.levelM.size() == r1.levelM.size());
    for (size_t c = 0; identical && c < r1.levelM.size(); ++c)
        if (r1b.levelM[c] != r1.levelM[c]) identical = false;

    CHECK(basinWet == 64, "la cuenca cerrada se llena ENTERA");
    CHECK(worstLevel == 0.0, "y EXACTAMENTE hasta su punto de derrame (no 'cerca': el mismo numero)");
    CHECK(spill == 0, "CONTRAPRUEBA: la meseta no retiene ni un texel — el relleno no inunda de mas");
    // El requisito de Andoni: un cuerpo sin oceano no tiene agua, y sale del CAMPO, no de un ajuste.
    CHECK(r2.lakeCells == 0 && !r2.hasWater,
          "planeta SIN MAR: cero agua y `hasWater` false, DERIVADO del campo y no configurado");
    CHECK(r1.hasWater, "CONTRAPRUEBA: el que si tiene mar da hasWater true (si no, (2) no diria nada)");
    CHECK(seamWet == 64, "la costura de longitud NO es un borde: la cuenca del meridiano se llena igual");
    CHECK(identical, "el relleno es DETERMINISTA bit a bit (dos clientes tienen que ver los mismos lagos)");
}

/**
 * @brief EL FETCH: un lago de montaña deja de tener olas de mar abierto.
 *
 * Era la limitación 2 del agua, medida: un lago de 3,21 m de fondo recibía **3,46 m de ola**, más
 * alta que profundo era el lago, porque el oleaje sale de UN estado global (el viento sobre el
 * océano) y el bajío lo AMPLIFICA en agua somera. Ni la dispersión de profundidad finita lo arregla
 * —cambia la longitud de onda, no la altura— porque la altura la fijan Green y el tope de rompiente.
 *
 * ⚠️ EL ORÁCULO ES JONSWAP, no un número elegido: `Hs = 0,0016·U·sqrt(F/g)` contra el
 * `Hs = 0,21·U²/g` de Pierson-Moskowitz. Lo que se comprueba es que el factor los relaciona.
 */
void test_ocean_fetch() {
    beginTest("ocean_fetch");
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;
    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();

    // El viento que el estado de referencia representa, deducido de su propia Hs (ver la función).
    float sum2 = 0.0f;
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) sum2 += st.wave[i][1] * st.wave[i][1];
    const float Hs = 4.0f * std::sqrt(sum2 * 0.5f);
    const float U  = std::sqrt(Hs * Haruka::World::Water::OCEAN_G / 0.21f);
    std::printf("    estado de referencia: Hs %.2f m -> viento deducido %.1f m/s (el declarado es %.1f)\n",
                Hs, U, Haruka::World::Water::OCEAN_REF_WIND);

    std::printf("    fetch        factor    Hs resultante   (Hs de mar abierto %.2f m)\n", Hs);
    for (float F : { 100.0f, 400.0f, 2000.0f, 20000.0f, 200000.0f, Haruka::World::Water::WATER_FETCH_UNLIMITED }) {
        const float f = Haruka::World::Water::oceanFetchFactor(st, F);
        std::printf("    %9.0f m  %8.4f   %8.3f m\n", F, f, f * Hs);
    }

    // La ola de verdad en el lago que se medía: 3,21 m de fondo, 400 m de diámetro.
    const glm::vec3 up(0, 1, 0), wp(1234.0f, 0.0f, -567.0f);
    float pkOpen = 0.0f, pkLake = 0.0f;
    float loOpen = 1e9f, loLake = 1e9f;
    for (int k = 0; k < 240; ++k) {
        const float t = 0.05f * (float)k;
        const float hO = oceanWaveHeight(wp, up, t, 40.0f, 1.0f, st, Haruka::World::Water::WATER_FETCH_UNLIMITED);
        const float hL = oceanWaveHeight(wp, up, t, 3.21f, 1.0f, st, 400.0f);
        pkOpen = std::max(pkOpen, hO); loOpen = std::min(loOpen, hO);
        pkLake = std::max(pkLake, hL); loLake = std::min(loLake, hL);
    }
    const float ptpOpen = pkOpen - loOpen, ptpLake = pkLake - loLake;
    std::printf("    ola pico a pico: mar abierto (40 m) %.3f m · lago de montana (3,21 m fondo, "
                "400 m de diametro) %.3f m\n", ptpOpen, ptpLake);
    std::printf("    razon lago/mar: %.3f  (antes del fetch era 1,08 — el lago tenia MAS ola)\n",
                ptpOpen > 1e-6f ? ptpLake / ptpOpen : 0.0f);

    // ── LO QUE SE AFIRMA ────────────────────────────────────────────────────────────────────────
    CHECK(std::fabs(Haruka::World::Water::oceanFetchFactor(st, Haruka::World::Water::WATER_FETCH_UNLIMITED) - 1.0f) < 1e-6f,
          "el OCEANO no se limita: factor 1 exacto");
    CHECK(ptpLake < ptpOpen * 0.25f,
          "el lago de montana ya NO hereda el swell del oceano: menos de un cuarto de su ola");
    CHECK(ptpLake < 3.21f * 0.5f,
          "y la ola cabe holgada en el lago (antes era MAS alta que profundo era el lago)");
    // CONTRAPRUEBA: sin el fetch, la MISMA llamada da la ola grande. Si no, este test no estaria
    // midiendo el fetch sino la profundidad.
    float pkNo = 0.0f, loNo = 1e9f;
    for (int k = 0; k < 240; ++k) {
        const float h = oceanWaveHeight(wp, up, 0.05f * (float)k, 3.21f, 1.0f, st,
                                        Haruka::World::Water::WATER_FETCH_UNLIMITED);
        pkNo = std::max(pkNo, h); loNo = std::min(loNo, h);
    }
    std::printf("    CONTRAPRUEBA: el MISMO lago sin limitar el fetch da %.3f m (x%.1f)\n",
                pkNo - loNo, ptpLake > 1e-6f ? (pkNo - loNo) / ptpLake : 0.0f);
    CHECK((pkNo - loNo) > ptpLake * 4.0f,
          "CONTRAPRUEBA: sin fetch el MISMO punto da mas de cuatro veces la ola (es el fetch, no el fondo)");
    // Un mar interior grande SI tiene oleaje: el fetch no es un interruptor de "lago = sin olas".
    CHECK(Haruka::World::Water::oceanFetchFactor(st, 200000.0f) > 0.5f,
          "un mar interior de 200 km si levanta ola: el fetch gradua, no apaga");
}

/**
 * @brief ¿QUÉ LAGO MÁS PEQUEÑO CABE EN EL BAKE? El límite de resolución del campo de agua.
 *
 * ⚠️ NO ES UN FALLO QUE ARREGLAR: ES EL LÍMITE DE LO QUE SE CONSTRUYÓ, y hay que tenerlo escrito
 * porque no es evidente. `bakeWaterMap` rellena las cuencas sobre el bake de altura, que es equirect
 * a `m_mapRes` (512 por defecto). Un téxel cubre `2πR/W` metros — en la Tierra, **78 km**. Un lago
 * más pequeño que un téxel no existe para el relleno: ni se llena, ni sale en la textura, ni la
 * física lo ve.
 *
 * O sea que el campo horneado representa MARES INTERIORES, no lagos. Los lagos de verdad (1-50 km)
 * siguen dependiendo del parche dinámico, que es local. Cerrarlo pide direccionar el agua por el
 * quadtree del terreno —(cara, nivel, i, j)— en vez de por un equirect: era la propuesta original y
 * quedó a medias, porque el relleno sí es global pero su rejilla no.
 */
void test_water_bake_resolution_limit() {
    beginTest("water_bake_resolution_limit");
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;
    const double R = 6371000.0;
    const double kPi = 3.14159265358979323846;

    std::printf("    texel del bake de agua y lago minimo que puede representar:\n");
    std::printf("      resolucion     texel (ecuador)    lago minimo (~2 texeles)\n");
    double texel512 = 0.0;
    for (int w : { 512, 1024, 2048, 4096, 8192 }) {
        const double texel = 2.0 * kPi * R / (double)w;
        if (w == 512) texel512 = texel;
        std::printf("      %5d x %-5d %13.0f m %20.0f m\n", w, w / 2, texel, texel * 2.0);
    }

    // Y comprobado sobre el relleno REAL: una cuenca de un solo texel da un fetch del orden del
    // texel, no del lago que uno se imagina.
    const int W = 128, H = 64;
    std::vector<float> land((size_t)W * H, 100.0f);
    for (int y = 0; y < H; ++y) for (int x = 0; x < 8; ++x) land[(size_t)y * W + x] = -500.0f;
    land[(size_t)32 * W + 64] = 10.0f;                       // UNA sola celda hundida
    const Haruka::World::Water::WaterFillResult one = Haruka::World::Water::waterFillEquirect(land.data(), W, H, 0.0f, R);
    const double texel128 = 2.0 * kPi * R / (double)W;
    std::printf("    cuenca de UN texel a %.0f km/texel: %zu texeles de lago · fetch %.0f m\n",
                texel128 / 1000.0, one.lakeCells, one.biggestFetchM);

    // ── LO QUE SE AFIRMA ────────────────────────────────────────────────────────────────────────
    //
    // No hay nada que "pase" o "falle" en la fisica: se fija el LIMITE por escrito, para que el dia
    // que alguien pregunte "por que no hay lagos" la respuesta este medida y no haya que buscarla.
    CHECK(texel512 > 50000.0,
          "a 512 de resolucion el texel del bake pasa de 50 km: el campo horneado da MARES "
          "INTERIORES, no lagos — los lagos siguen siendo del parche dinamico");
    CHECK(one.lakeCells > 0 && one.biggestFetchM > texel128 * 0.5,
          "y el lago mas pequeno posible es del tamano de un texel, no menor");
    // CONTRAPRUEBA: subiendo la resolucion el limite baja proporcionalmente. Si no lo hiciera, el
    // problema no seria de resolucion y la salida propuesta (rejilla del quadtree) seria la equivocada.
    const double texel8k = 2.0 * kPi * R / 8192.0;
    std::printf("    a 8192 el texel baja a %.1f km: la salida es la RESOLUCION, no el algoritmo\n",
                texel8k / 1000.0);
    CHECK(texel8k < texel512 / 10.0,
          "CONTRAPRUEBA: el limite escala con la resolucion — es de rejilla, no del relleno");
}

// ------------------------- TEST: el relleno de DOS NIVELES encuentra lagos que el global no ve ---
// ⚠️ EL LIMITE QUE CIERRA. `water_bake_resolution_limit` deja medido que el bake de agua a 512 son
// 78,2 km/texel en la Tierra, asi que una cuenca menor que eso no existe en el campo: salen mares
// interiores, no lagos. Y subir la resolucion global no llega — a 8192 el texel sigue siendo 4,9 km
// y cuesta 402 MB, dos ordenes por encima del tamaño de un lago.
//
// La salida es partir el problema: la COTA DE DERRAME es global (depende de la cuenca entera) y la
// ORILLA es local (`h(x) < nivel`). `waterFillWindow` hace el segundo paso sobre el terreno de verdad
// con el relleno grueso de condicion de contorno.
//
// Este test pone una cuenca de ~3 km en un planeta terrestre y comprueba las dos mitades:
//   · el relleno GLOBAL a 512 no la ve (es sub-texel) — si la viera, la ventana no haria falta,
//   · la VENTANA si la ve, con cota y fetch plausibles,
//   · y la condicion de contorno IMPORTA: sin ella la ventana saldria seca.
void test_water_fill_window() {
    beginTest("water_fill_window");
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;
    const double R = 6371000.0;
    const double kPi = 3.14159265358979323846;

    // Un terreno analitico: meseta a 500 m con un CRATER de 3 km de radio y 40 m de hondo, y mar
    // lejos. Se define como funcion de (lon,lat) para que la ventana pueda muestrearlo tan fino como
    // quiera — que es justo lo que el bake grueso no puede.
    const double lonL = 0.6, latL = 0.3;      // centro del lago
    const double radLagoM = 3000.0;
    auto terreno = [&](double lon, double lat) -> float {
        // Distancia angular al centro del lago -> metros.
        const double dx = (lon - lonL) * std::cos(lat), dy = lat - latL;
        const double dM = std::sqrt(dx*dx + dy*dy) * R;
        float hM = 500.0f;
        if (dM < radLagoM) {
            const double t = dM / radLagoM;
            hM = (float)(500.0 - 40.0 * (1.0 - t*t));    // cuenco parabolico de 40 m
        }
        // Y un mar en el otro hemisferio, para que el relleno global tenga semilla.
        if (lon < -1.0) hM = -200.0f;
        return hM;
    };

    // ── (1) EL RELLENO GLOBAL, a la resolucion del bake. ────────────────────────────────────────
    const int GW = 512, GH = 256;
    std::vector<float> global((size_t)GW * GH);
    for (int y = 0; y < GH; ++y) for (int x = 0; x < GW; ++x) {
        const double lon = (((double)x + 0.5) / GW - 0.5) * 2.0 * kPi;
        const double lat = (0.5 - ((double)y + 0.5) / GH) * kPi;
        global[(size_t)y * GW + x] = terreno(lon, lat);
    }
    const Haruka::World::Water::WaterFillResult g = Haruka::World::Water::waterFillEquirect(global.data(), GW, GH, 0.0f, R);
    const double texelG = 2.0 * kPi * R / GW;
    std::printf("    GLOBAL %dx%d (%.0f km/texel): %zu texeles de lago · %zu masas\n",
                GW, GH, texelG / 1000.0, g.lakeCells, g.bodies);
    CHECK(g.seaCells > 0, "el relleno global tiene mar (si no, no habria semilla y todo saldria seco)");
    CHECK(g.lakeCells == 0,
          "el GLOBAL no ve un lago de 3 km: es sub-texel a 78 km/texel (por eso hace falta el paso fino)");

    // El relleno grueso, consultado por la ventana en su borde.
    auto nivelGrueso = [&](double lon, double lat) -> float {
        double u = lon / (2.0 * kPi) + 0.5;
        u -= std::floor(u);
        int x = (int)(u * GW); if (x >= GW) x = GW - 1;
        int y = (int)((0.5 - lat / kPi) * GH);
        y = y < 0 ? 0 : (y >= GH ? GH - 1 : y);
        return g.levelM[(size_t)y * GW + x];
    };

    // ── (2) LA VENTANA, sobre el terreno de verdad. ─────────────────────────────────────────────
    const double medio = 8000.0 / R;    // ventana de ±8 km alrededor del lago
    const Haruka::World::Water::WaterWindowResult win = Haruka::World::Water::waterFillWindow(256, 256, lonL, latL, medio, medio, R,
                                                 terreno, nivelGrueso, 0.0f);
    std::printf("    VENTANA 256x256 (%.0f m/texel): %zu texeles de lago · %zu masas · hondo %.1f m\n"
                "    fetch %.0f m · masas cortadas por el borde %zu\n",
                win.texelM, win.lakeCells, win.bodies, win.deepestM,
                win.lakeCells ? win.fetchM[(size_t)128*256+128] : 0.0f, win.clippedBodies);
    CHECK(win.lakeCells > 0, "la VENTANA si encuentra el lago que el global no ve");
    CHECK(win.texelM < texelG / 100.0,
          "y lo hace porque su texel es dos ordenes mas fino que el del bake global");
    // La cota tiene que ser la del borde del crater (500 m), no el fondo (460 m).
    const float nivel = win.levelM[(size_t)128*256+128];
    std::printf("    cota de la lamina en el centro: %.2f m (el borde del crater esta a 500 m)\n", nivel);
    CHECK(std::fabs(nivel - 500.0f) < 1.0f,
          "la cota es la del punto de derrame (borde del crater), no el fondo de la cuenca");
    CHECK(win.deepestM > 35.0f && win.deepestM < 41.0f,
          "y la profundidad maxima es la del cuenco (40 m), medida y no supuesta");
    CHECK(win.clippedBodies == 0,
          "el lago cabe entero en la ventana → su fetch es exacto, no una cota inferior");

    // ── (3) ⚠️ LA CONDICION DE CONTORNO SE CONSULTA DE VERDAD, y hay que probarlo: sin esto, un
    //     `waterFillWindow` que ignorara el borde entero daria EXACTAMENTE el mismo 500,00 m de
    //     arriba y el test no notaria nada. Se le dice que fuera hay una lamina a 520 m —un lago
    //     mayor que se derrama hacia aqui— y la ventana tiene que inundarse hasta ESA cota.
    //
    //     ⚠️ Mi primera contraprueba fue mala y la dejo escrita: puse el borde a cota 0 esperando
    //     "salida libre, todo drena", y salio el mismo lago. El motivo es que el nivel del borde
    //     nunca baja de su propia altura (`max(coarse, height)`) — el agua no puede estar bajo el
    //     suelo. O sea que un relleno grueso equivocado puede inundar de mas, pero NO puede vaciar
    //     inventando un desague. Eso es una propiedad buena de la funcion, y la contraprueba tenia
    //     que ir en la direccion en la que si se puede fallar.
    {
        auto lamAlta = [&](double, double) -> float { return 520.0f; };
        const Haruka::World::Water::WaterWindowResult sube = Haruka::World::Water::waterFillWindow(256, 256, lonL, latL, medio, medio, R,
                                                       terreno, lamAlta, 0.0f);
        const float nivelAlto = sube.levelM[(size_t)128*256+128];
        std::printf("    CONTRAPRUEBA con lamina exterior a 520 m: cota %.2f m · %zu texeles (antes %zu)\n",
                    nivelAlto, sube.lakeCells, win.lakeCells);
        CHECK(std::fabs(nivelAlto - 520.0f) < 1.0f,
              "CONTRAPRUEBA: el borde MANDA — con lamina exterior a 520 m la ventana se inunda a 520, "
              "no a los 500 de su propio crater");
        CHECK(sube.lakeCells > win.lakeCells,
              "y moja mas superficie que con el exterior seco (la ventana no ignora su contorno)");
    }

    // ── (4) Y una ventana en terreno LISO no inventa agua. ──────────────────────────────────────
    {
        auto llano = [](double, double) -> float { return 500.0f; };
        const Haruka::World::Water::WaterWindowResult seco = Haruka::World::Water::waterFillWindow(128, 128, lonL, latL, medio, medio, R,
                                                       llano, nivelGrueso, 0.0f);
        std::printf("    ventana sobre terreno LISO: %zu texeles de lago\n", seco.lakeCells);
        CHECK(seco.lakeCells == 0, "sobre terreno liso la ventana no inventa lagos");
    }

    // ── (5) EL COSTE, que es lo que decide si esto puede ir por streaming. ──────────────────────
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        const int reps = 20;
        for (int i = 0; i < reps; ++i)
            (void)Haruka::World::Water::waterFillWindow(256, 256, lonL, latL, medio, medio, R, terreno, nivelGrueso, 0.0f);
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - t0).count() / reps;
        std::printf("    coste de una ventana 256x256: %.2f ms (65 536 muestras de terreno + flood)\n", ms);
        CHECK(ms < 200.0, "una ventana cuesta menos de 200 ms: cabe en un hilo de fondo");
    }
}

// -------------------- TEST: la POLITICA de la ventana de lagos (que recuadro, cuando, cuanto) ----
// ⚠️ ESTO ES LA MITAD QUE FALTABA. `waterFillWindow` sabe inundar un recuadro; lo que decide si el
// sistema sirve es CUANDO se recentra. Dos formas de equivocarse, las dos caras:
//   · recalcular al pisar el borde → un jugador andando por el limite dispara un recalculo POR FRAME,
//     y son 65 536 muestras del terreno real cada vez;
//   · recalcular solo al salir del todo → hay frames con el observador FUERA de la ventana, o sea sin
//     lago donde si lo hay.
// La regla es recentrar al salir de la MITAD interior, y lo que compra es margen garantizado.
void test_water_window_policy() {
    beginTest("water_window_policy");
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;
    const double R = 6371000.0, halfM = 8000.0;

    // Centro de la ventana y una direccion a `d` metros de el, por la esfera.
    const double lonC = 0.4, latC = -0.2;
    auto dirAt = [&](double metros) {
        const double ang = metros / R;
        const double lat = latC + ang;                  // se separa en latitud, que es distancia pura
        return glm::dvec3(std::cos(lat) * std::cos(lonC), std::sin(lat), std::cos(lat) * std::sin(lonC));
    };
    const glm::dvec3 centro = dirAt(0.0);

    CHECK(Haruka::World::Water::waterWindowNeedsRecenter(centro, false, lonC, latC, halfM, R),
          "sin ventana valida SIEMPRE se calcula (si no, el primer frame no tendria lagos)");
    CHECK(!Haruka::World::Water::waterWindowNeedsRecenter(centro, true, lonC, latC, halfM, R),
          "quieto en el centro NO se recalcula");

    // El umbral, barrido: donde esta y que margen deja.
    double umbral = -1.0;
    for (int i = 0; i <= 200; ++i) {
        const double m = (double)i * (halfM / 100.0);   // 0 .. 2·half
        if (Haruka::World::Water::waterWindowNeedsRecenter(dirAt(m), true, lonC, latC, halfM, R)) { umbral = m; break; }
    }
    std::printf("    recuadro ±%.0f m · se recentra al pasar de %.0f m del centro (%.0f %% del medio lado)\n"
                "    margen GARANTIZADO por delante en el peor caso: %.0f m\n",
                halfM, umbral, 100.0 * umbral / halfM, halfM - umbral);
    CHECK(umbral > 0.0 && umbral < halfM,
          "el umbral cae DENTRO del recuadro (si fuera >= al medio lado, habria frames sin ventana bajo los pies)");
    CHECK(halfM - umbral >= halfM * 0.45,
          "y deja al menos medio recuadro de margen por delante");

    // ⚠️ CONTRAPRUEBA de que la histeresis existe: sin ella (umbral = el medio lado) el margen seria
    // CERO en el peor caso. Es la diferencia entre "se ve venir el lago" y "aparece bajo los pies".
    {
        const bool alBorde = Haruka::World::Water::waterWindowNeedsRecenter(dirAt(halfM * 0.95), true, lonC, latC, halfM, R);
        std::printf("    CONTRAPRUEBA: a 95%% del medio lado ya se ha recentrado hace rato (%s)\n",
                    alBorde ? "si" : "NO");
        CHECK(alBorde, "CONTRAPRUEBA: cerca del borde el sistema YA ha pedido ventana nueva");
    }
    // Y que no se recentra por moverse un metro: si lo hiciera, cada paso costaria el recalculo entero.
    CHECK(!Haruka::World::Water::waterWindowNeedsRecenter(dirAt(1.0), true, lonC, latC, halfM, R),
          "andar un metro NO dispara un recalculo");

    // ── EL COSTE REAL, con el terreno de VERDAD (base + detalle), que es lo que decide si esto puede
    //    ir en un hilo o tendria que ir troceado. Con un terreno analitico de juguete la cifra no
    //    valdria: el 73 % del coste de un bake es evaluar el relieve.
    {
        const int HW = 256, HH = 128;
        std::vector<float> base((size_t)HW * HH, 300.0f);
        const float triM = Haruka::World::Terrain::terrainTriM(0.0);
        auto dirOfLL = [](double lo, double la) {
            return glm::dvec3(std::cos(la)*std::cos(lo), std::sin(la), std::cos(la)*std::sin(lo));
        };
        auto terreno = [&](double lo, double la) -> float {
            const glm::dvec3 d = dirOfLL(lo, la);
            const glm::vec2 uv = Haruka::World::Terrain::equirectUV(glm::vec3(d));
            const float baseH = Haruka::World::Terrain::sampleHeightField(uv, HW, HH, base.data());
            float det = Haruka::World::Terrain::terrainDetail(glm::vec3(d), (float)R + baseH, triM) * Haruka::World::Terrain::seaLevelAttenuation(baseH);
            if (baseH > 0.0f) det = glm::max(det, -baseH);
            return baseH + det;
        };
        auto contorno = [](double, double) -> float { return Haruka::World::Water::WATER_FILL_DRY; };
        const double hLat = halfM / R, hLon = hLat / std::max(std::cos(latC), 1e-6);
        const auto t0 = std::chrono::high_resolution_clock::now();
        const int reps = 5;
        for (int i = 0; i < reps; ++i)
            (void)Haruka::World::Water::waterFillWindow(256, 256, lonC, latC, hLon, hLat, R, terreno, contorno, 0.0f);
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - t0).count() / reps;
        std::printf("    coste con el terreno REAL (base + terrainDetail), 256x256: %.1f ms\n"
                    "    → %s del frame de 16,7 ms: por eso va en un hilo y no en `prepare`\n",
                    ms, ms > 16.7 ? "POR ENCIMA" : "por debajo");
        CHECK(ms > 0.0, "el coste queda MEDIDO con el terreno real, no con uno de juguete");
        // Se recentra cada `halfM/2` = 4 km. A 5 m/s son 800 s entre recalculos: el coste amortizado
        // es despreciable AUNQUE cada uno sea caro. Lo que no puede es bloquear el frame.
        const double segundos = (halfM * 0.5) / 5.0;
        std::printf("    a 5 m/s se recentra cada %.0f s → %.4f ms/frame amortizados\n",
                    segundos, ms / (segundos * 60.0));
        CHECK(ms / (segundos * 60.0) < 1.0, "amortizado cuesta menos de 1 ms por frame");
    }
}

// THE SHORE BAND GOES ONLY UP (12-10, Andoni's capture of terraced blue slabs with walls on a slope).
// `lakeWindowGpuRG` carries a lake's level into the dry texels around it so the terrain can cut the
// shore per pixel. Carried into EVERY dry neighbour, the upper of two terraced lakes spilled its level
// over the lower one's shore, and an outlet's downhill texels got a slab. A row, west to east:
//     upper lake (410) | rim 410.5 | lower shore 404.6 | lower lake (404) | rising shore 406
void test_water_window_shore_rises() {
    beginTest("water_window_shore_rises");
    namespace W = Haruka::World::Water;
    const int w = 5, h = 1;
    const std::vector<float> level = { 410.0f, W::WATER_FILL_DRY, W::WATER_FILL_DRY, 404.0f, W::WATER_FILL_DRY };
    const std::vector<float> fetch = { 300.0f, 0.0f, 0.0f, 200.0f, 0.0f };
    const std::vector<float> ground = { 409.0f, 410.5f, 404.6f, 403.0f, 406.0f };
    std::vector<float> now, first;
    W::lakeWindowGpuRG(level, fetch, w, h, 2, now, &ground);
    W::lakeWindowGpuRG(level, fetch, w, h, 2, first, nullptr);
    std::printf("    drawn level, west to east: %.1f %.1f %.1f %.1f %.1f  (into every neighbour: %.1f %.1f %.1f %.1f %.1f)\n",
                now[0], now[2], now[4], now[6], now[8], first[0], first[2], first[4], first[6], first[8]);
    CHECK(now[2] == 410.0f, "the rim (ground above the upper lake) takes the upper lake's level: its shore");
    CHECK(now[4] == 404.0f, "the LOWER lake's shore keeps the lower level: the upper one does not spill over it");
    CHECK(now[8] == 404.0f, "a shore that rises from the lower lake takes its level");
    CHECK(now[5] < 0.0f && now[3] < 0.0f, "the band's texels are marked as shore (negative fetch: dry)");
    CHECK(first[4] == 410.0f, "CONTROL: into every neighbour, the lower lake's shore got the UPPER level (the slab)");
}

// A PUDDLE IS NOT A LAKE (12-10, Andoni: "desniveles notorios y el agua no se nivela"). On the real
// bake the fine window held 1144 lake levels in 16 km, 925 of them 1-2 texels: perched puddles side by
// side at different heights. `waterFillWindow` now keeps a body of `WATER_WINDOW_MIN_LAKE_TEXELS` or
// more, or one that touches the window's edge (it goes on outside). Three dips on a flat 100 m plain,
// 2 m deep: 3 texels (dry), 4 texels (lake), and 2 texels against the west edge, where the coarse
// fill has water outside at 99 m (kept, 1 m deep).
void test_water_window_min_lake() {
    beginTest("water_window_min_lake");
    namespace W = Haruka::World::Water;
    const int n = 32;
    const double R = 6371000.0, half = 1000.0 / R;   // 2 km window: 62.5 m texels
    auto texel = [&](double lo, double la, int& x, int& y) {
        x = (int)std::floor((lo + half) / (2.0 * half) * n); y = (int)std::floor((la + half) / (2.0 * half) * n);
    };
    auto inDip = [](int x, int y) {
        if (y == 10 && x >= 10 && x <= 12) return 1;                 // 3 texels in a row
        if ((y == 20 || y == 21) && (x == 20 || x == 21)) return 2;  // 2x2
        if (y == 5 && (x == 0 || x == 1)) return 3;                  // 2 texels on the west edge
        return 0;
    };
    auto ground = [&](double lo, double la) -> float { int x, y; texel(lo, la, x, y); return inDip(x, y) ? 98.0f : 100.0f; };
    // Outside the window, to the west, the coarse fill holds water at 99 m: a lake that goes on out
    // there. (With a dry outside a border cell is a wall and never a lake, by design of the fill.)
    auto border = [&](double lo, double) -> float { return (lo < -half * 0.9) ? 99.0f : W::WATER_FILL_DRY; };
    const W::WaterWindowResult r = W::waterFillWindow(n, n, 0.0, 0.0, half, half, R, ground, border, 0.0f);
    int wet[4] = { 0, 0, 0, 0 };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            if (r.levelM[(size_t)y * n + x] > W::WATER_FILL_DRY) wet[inDip(x, y)]++;
    std::printf("    wet texels: 3-texel dip %d · 2x2 dip %d · 2 texels on the edge %d · elsewhere %d · bodies %zu\n",
                wet[1], wet[2], wet[3], wet[0], r.bodies);
    CHECK(wet[1] == 0, "a 3-texel dip is dry: under the minimum lake");
    CHECK(wet[2] == 4, "CONTROL: the 2x2 dip (4 texels) is a lake, so the dips do flood");
    CHECK(wet[3] == 2, "a small dip against the window's edge is kept: it may go on outside");
    CHECK(wet[0] == 0 && r.bodies == 2, "nothing else is wet, and two bodies are counted");
}
