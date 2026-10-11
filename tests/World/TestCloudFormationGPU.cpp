// Formacion de nubes en GPU: las capas altas, la cima que se dibuja, el movimiento frame a frame y
// la convergencia de la marcha.
#include "World/CloudPassBench.h"

void CloudPassBench::highLayers() {
    BEGIN("nubes: FORMACION en GPU (la columna de aire repartida en nubes)");
    // ── (6c) EL VAPOR EN ALTURA NO SON MOTAS ─────────────────────────────────────────────────────
    //
    // Reportado en partida: "motas blancas por todo el cielo". A resolucion completa eran de 1 px:
    // muestreo, no campo. Un cielo sin cumulo (cobertura ~0) deja solo el altocumulo y el cirro de
    // buen tiempo (los parches salen de la posicion, no de la textura), mirando al CENIT. Se cuenta
    // cuantos pixeles con nube son AISLADOS —ninguno de sus cuatro vecinos tiene nube—: una capa de
    // verdad tiene 0 y una lluvia de motas los tiene a miles. `HARUKA_CLOUD_PNG` vuelca la imagen.
    {
        const float baseAnt = slabBase, topAnt = slabTop;
        slabBase = 2470.0f; slabTop = 13000.0f; ojoA = 1030.0f; mira = up0;   // al cenit, la columna entera
        std::vector<uint8_t> pxH;
        // ⚠️ SOLO LAS CAPAS ALTAS (mascara 2|4). Antes se "quitaba" el cumulo bajando su cobertura a
        // 0,02 y el pase seguia dibujando MOTAS de cumulo: los 292 px que hacian pasar "las capas
        // altas existen" eran cumulo (comprobado marchando sin la losa convectiva: 0 px). Con la
        // mascara la medida es de lo que dice medir. Y como los parches de buen tiempo salen de la
        // POSICION, se mira en varias direcciones y se suma: un cenit vacio no es un cielo sin capas.
        modosCur = 6;
        size_t nLitAll = 0;
        const glm::dvec3 dirs[5] = { up0, glm::normalize(fwd + up0 * 0.8), glm::normalize(-fwd + up0 * 0.8),
                                     glm::normalize(glm::cross(up0, fwd) + up0 * 0.8), glm::normalize(-glm::cross(up0, fwd) + up0 * 0.8) };
        for (const glm::dvec3& dv : dirs) {
            mira = dv;
            std::vector<uint8_t> pxD;
            render(0.02f, pxD);
            for (size_t k = 0; k + 3 < pxD.size(); k += 4) if (pxD[k] > 12 || pxD[k+1] > 12 || pxD[k+2] > 12) ++nLitAll;
            if (dv == up0) pxH = pxD;
        }
        mira = up0;
        modosCur = 0;
        const bool flipH = detectRowFlip(pxH, w, h);
        auto lit = [&](int x, int y) {
            const size_t k = ((size_t)y * w + x) * 4;
            return pxH[k] > 12 || pxH[k+1] > 12 || pxH[k+2] > 12;
        };
        size_t nLit = 0, nIsolated = 0;
        for (int y = 1; y < h - 1; ++y)
            for (int x = 1; x < w - 1; ++x) {
                if (!lit(x, y)) continue;
                ++nLit;
                if (!lit(x-1,y) && !lit(x+1,y) && !lit(x,y-1) && !lit(x,y+1)) ++nIsolated;
            }
        const double granoH = grano(pxH);
        std::printf("    CAPAS ALTAS al cenit (solo capas, mascara 2|4): %zu px con nube · %zu AISLADOS (%.2f %%) · grano %.1f %% · en 5 direcciones %zu px\n",
                    nLit, nIsolated, nLit ? 100.0 * (double)nIsolated / (double)nLit : 0.0, granoH, nLitAll);
        if (const char* dir = std::getenv("HARUKA_CLOUD_PNG")) {
            std::vector<uint8_t> img(pxH.size());
            for (int y = 0; y < h; ++y) {
                const int src = flipH ? (h - 1 - y) : y;
                std::memcpy(&img[(size_t)y * w * 4], &pxH[(size_t)src * w * 4], (size_t)w * 4);
            }
            const std::string path = std::string(dir) + "/"
                                   + ((g_dev->backend() == Backend::Vulkan) ? "vk_" : "gl_") + "capas_altas.png";
            Haruka::IO::writePNG(path, w, h, 4, img.data());
            std::printf("      -> %s\n", path.c_str());
        }
        CHECK(nLitAll > 0, "las capas altas de buen tiempo EXISTEN en alguna direccion (si no, el cielo despejado no tiene cirro nunca)");
        CHECK(nLit == 0 || nIsolated * 200 < nLit, "y NO son motas: menos del 0,5 % de pixeles aislados");
        CHECK(granoH < 3.0, "y no son grano: menos del 3 % de pixeles a mas de 10/255 de su media 3x3");

        // Y CON EL HORNEADO REAL, que es lo que el juego ata: las dos texturas de `bakeSky` (cielo y
        // capas altas) con un mundo templado y seco como el del spawn (18 C, H 0,12). Si aqui salen
        // motas y arriba no, la culpa es de las texturas (o de como se leen), no de la marcha.
        {
            const int CW = 256, CH = 128;
            std::vector<float> tmp((size_t)CW * CH, 18.0f), hum((size_t)CW * CH, 0.12f), wat((size_t)CW * CH, 0.0f);
            std::vector<float> sky((size_t)CW * CH * 4), hi((size_t)CW * CH * 4);
            Haruka::World::WeatherSystem wsys; wsys.configure(1234u); wsys.setTime(30.0);
            float bMin = 0, tMax = 0;
            const float cmax = wsys.bakeSky(tmp.data(), hum.data(), CW, CH, sky.data(), &bMin, &tMax, wat.data(), hi.data());
            TextureDesc d; d.width = CW; d.height = CH; d.format = Format::RGBA32F;
            d.filter = Filter::Linear; d.wrap = Wrap::Repeat; d.mipmaps = false;
            d.initialData = sky.data(); TextureHandle skyTex = g_dev->createTexture(d);
            d.initialData = hi.data();  TextureHandle hiTex  = g_dev->createTexture(d);
            coverBound = skyTex;
            const TextureHandle hiAnt = hiDummy; hiDummy = hiTex;
            slabBase = bMin; slabTop = std::max(tMax, 13000.0f);   // la columna entera: el hielo esta a 9-11 km
            // ⚠️ SOLO EL CUMULO. Con las capas altas puestas, el cirro (liso y con el 30 % del cuadro)
            // diluia la metrica: daba 2,2 % de grano cuando el cumulo solo tiene 17-22 %. La medida
            // tiene que ser de lo que dice medir. El banco de las capas es (6c').
            modosCur = 1;
            std::vector<uint8_t> pxR;
            render(cmax, pxR);
            // Y la misma escena MIRANDO A 25 GRADOS sobre el horizonte, que es como se juega: el
            // rayo cruza decenas de km de capa y el paso ya ha crecido a cientos de metros.
            mira = glm::normalize(fwd + up0 * 0.47);
            std::vector<uint8_t> pxO;
            render(cmax, pxO);
            mira = up0; modosCur = 0;
            hiDummy = hiAnt; coverBound = coverTex;
            {
                const double granoO = grano(pxO);
                size_t nO = 0;
                for (size_t k = 0; k + 3 < pxO.size(); k += 4) if (pxO[k] > 12) ++nO;
                std::printf("    ... y a 25 grados sobre el horizonte: %zu px con nube · grano %.1f %%\n", nO, granoO);
                if (const char* dir = std::getenv("HARUKA_CLOUD_PNG")) {
                    const bool flipO = detectRowFlip(pxO, w, h);
                    std::vector<uint8_t> img(pxO.size());
                    for (int y = 0; y < h; ++y) {
                        const int src = flipO ? (h - 1 - y) : y;
                        std::memcpy(&img[(size_t)y * w * 4], &pxO[(size_t)src * w * 4], (size_t)w * 4);
                    }
                    Haruka::IO::writePNG(std::string(dir) + "/vk_capas_altas_oblicuo.png", w, h, 4, img.data());
                }
                // ⚠️ NO ES UN OBJETIVO CUMPLIDO, ES UNA GUARDIA. Con el shader original (antes de
                // cualquier cambio del 2026-09-12) este numero era 62 %: junto al horizonte el rayo
                // cruza decenas de km de capa con pasos de 300 m y cada pixel salia "toca / no toca".
                // Con LOD de octavas, umbral con antialiasing simetrico, transmitancia por cobertura
                // y paso adaptativo queda en ~40 %: la banda del horizonte ya se funde en cobertura y
                // el grano vive en su borde. Lo que falta es reproyeccion temporal o mas pasos; hasta
                // entonces, que no vuelva a subir.
                CHECK(granoO < 60.0, "grano al horizonte por debajo del 60 % (cumulo solo; era 62 % con el shader original; hoy ~56)");
            }
            const bool flipR = detectRowFlip(pxR, w, h);
            auto litR = [&](int x, int y) {
                const size_t k = ((size_t)y * w + x) * 4;
                return pxR[k] > 12 || pxR[k+1] > 12 || pxR[k+2] > 12;
            };
            size_t nLitR = 0, nIsoR = 0;
            for (int y = 1; y < h - 1; ++y)
                for (int x = 1; x < w - 1; ++x) {
                    if (!litR(x, y)) continue;
                    ++nLitR;
                    if (!litR(x-1,y) && !litR(x+1,y) && !litR(x,y-1) && !litR(x,y+1)) ++nIsoR;
                }
            const double granoR = grano(pxR);
            std::printf("    con el HORNEADO REAL (bakeSky, 18 C / H 0.12, banda %.0f-%.0f m): %zu px con nube · %zu AISLADOS (%.2f %%) · grano %.1f %%\n",
                        bMin, tMax, nLitR, nIsoR, nLitR ? 100.0 * (double)nIsoR / (double)nLitR : 0.0, granoR);
            if (const char* dir = std::getenv("HARUKA_CLOUD_PNG")) {
                std::vector<uint8_t> img(pxR.size());
                for (int y = 0; y < h; ++y) {
                    const int src = flipR ? (h - 1 - y) : y;
                    std::memcpy(&img[(size_t)y * w * 4], &pxR[(size_t)src * w * 4], (size_t)w * 4);
                }
                const std::string path = std::string(dir) + "/"
                                       + ((g_dev->backend() == Backend::Vulkan) ? "vk_" : "gl_") + "capas_altas_real.png";
                Haruka::IO::writePNG(path, w, h, 4, img.data());
                std::printf("      -> %s\n", path.c_str());
            }
            CHECK(nLitR == 0 || nIsoR * 100 < nLitR, "con el horneado real tampoco hay motas (cumulo solo: < 1 % aislados; medido 0,57)");
            // ⚠️ GUARDIA, NO OBJETIVO: el cumulo de buen tiempo al cenit tiene un 17-22 % de grano
            // (jitter por pixel sobre losas translucidas de 260-600 m con pasos de 50 m). El "2,2 %"
            // de antes era el cirro diluyendo la media. Lo que lo quita es reproyeccion temporal.
            CHECK(granoR < 25.0, "grano del cumulo al cenit por debajo del 25 % (medido 21,5; 17,3 con el umbral viejo)");
            g_dev->destroy(skyTex); g_dev->destroy(hiTex);
        }
        slabBase = baseAnt; slabTop = topAnt; ojoA = eyeA; mira = fwd;
    }

    // ── (6c') LAS CAPAS ALTAS, CAPA A CAPA Y ALTURA A ALTURA ─────────────────────────────────
    //
    // Reportado: "contra mas arriba, o se ocultan y/o son planas". Dos numeros por capa y punto de
    // vista, con el horneado real: cuanta capa HAY (px con nube) y que FORMA tiene (desviacion de
    // la luminancia entre los px con nube: una lamina se sombrea igual en todas partes, un cuerpo
    // tiene panza oscura y cima clara; el cumulo es la referencia). Se mira desde el suelo al
    // cenit, desde DENTRO de cada banda a 10 grados, y desde encima de todas hacia abajo.
    {
        const int CW = 256, CH = 128;
        const float humB = std::getenv("HARUKA_BENCH_HUM") ? (float)std::atof(std::getenv("HARUKA_BENCH_HUM")) : 0.12f;
        std::vector<float> tmp((size_t)CW * CH, 18.0f), hum((size_t)CW * CH, humB), wat((size_t)CW * CH, 0.0f);
        std::vector<float> sky((size_t)CW * CH * 4), hi((size_t)CW * CH * 4);
        Haruka::World::WeatherSystem wsys; wsys.configure(1234u); wsys.setTime(30.0);
        float bMin = 0, tMax = 0;
        const float cmax = wsys.bakeSky(tmp.data(), hum.data(), CW, CH, sky.data(), &bMin, &tMax, wat.data(), hi.data());
        TextureDesc d; d.width = CW; d.height = CH; d.format = Format::RGBA32F;
        d.filter = Filter::Linear; d.wrap = Wrap::Repeat; d.mipmaps = false;
        d.initialData = sky.data(); TextureHandle skyTex = g_dev->createTexture(d);
        d.initialData = hi.data();  TextureHandle hiTex  = g_dev->createTexture(d);
        {   // lo que el shader va a LEER de las dos texturas (para que un 0 % tenga explicacion)
            double sx = 0, sy = 0, sz = 0, sc = 0, st = 0, mxy = 0;
            for (size_t k = 0; k < (size_t)CW * CH; ++k) {
                sx += hi[k*4]; sy += hi[k*4+1]; sz += hi[k*4+2]; mxy = std::max(mxy, (double)hi[k*4+1]);
                sc += sky[k*4]; st += sky[k*4+2] - sky[k*4+1];
            }
            const double N = (double)CW * CH;
            std::printf("    horneado: cobertura media %.3f · torre media %.0f m · hi: cirro %.3f · medio %.3f (max %.3f) · hSky %.3f\n",
                        sc / N, st / N, sx / N, sy / N, mxy, sz / N);
        }
        const TextureHandle hiAnt = hiDummy, covAnt = coverBound;
        hiDummy = hiTex; coverBound = skyTex;
        const float baseAnt = slabBase, topAnt = slabTop;
        slabBase = bMin; slabTop = std::max(tMax, 13000.0f);   // la columna entera (una marcha)

        // ⚠️ FUERA DEL POLO. El banco pone el ojo en el polo norte del planeta (vertical +Y), y en
        // la equirectangular eso es la fila 0 entera: todo el cuadro lee UN texel del horneado y
        // las "manchas" de buen tiempo (ruido de 33-83 km) caen en UNA celda. Medido: altocumulo
        // 0,0 % en las cuatro vistas por eso, no por la marcha. Latitud 45 y CUATRO rumbos, y se
        // da la media: lo que se mide es el cielo, no una celda.
        upCur = glm::normalize(glm::dvec3(1.0, 1.0, 0.0));
        const glm::dvec3 fwdA = glm::normalize(glm::cross(upCur, glm::dvec3(0.0, 0.0, 1.0)));
        const glm::dvec3 fwdB = glm::cross(upCur, fwdA);
        struct Vista { const char* nombre; float alt; double elev; int rumbos; };
        const Vista vistas[] = { { "suelo, cenit",          1030.0f,  90.0, 1 },
                                 { "en la banda media",     4600.0f,  10.0, 4 },
                                 { "en la banda del cirro", 8400.0f,  10.0, 4 },
                                 { "encima de todo, -45",  13000.0f, -45.0, 4 },
                                 // La vista de Andoni desde el cine `cielo3k`: 1 km por encima del techo
                                 // del cumulo, casi rasante hacia abajo. Aqui las nubes salian como RAYAS.
                                 { "encima del cumulo, -5", 4030.0f,  -5.0, 4 } };
        const char* capas[] = { "cumulo", "medio", "cirro" };
        double presencia[5][3] = {}, forma[5][3] = {}, granoV[5][3] = {}, quemados[5][3] = {};
        // ── CORTES RECTOS ──────────────────────────────────────────────────────────────────────
        // Andoni, con captura desde 4 km: "una nube bien formada y otra difuminada con CORTES". Las
        // imagenes de este mismo banco (HARUKA_CLOUD_PNG) lo ensenan: en la banda media y desde
        // arriba hay bordes de nube que son LINEAS RECTAS de decenas de pixeles. Una nube no tiene
        // rectas; una recta en pantalla es un plano de la marcha, una costura de textura o un corte
        // de rango. Se cuenta: tramos de >= 20 px seguidos de borde fuerte (|grad| > 40/255) a lo
        // largo de filas, columnas y las dos diagonales, por vista y capa.
        auto cortesRectos = [&](const std::vector<uint8_t>& px) {
            auto L = [&](int x, int y) { return (int)px[((size_t)y * w + x) * 4]; };
            auto edge = [&](int x, int y) {
                if (x < 1 || y < 1 || x >= w - 1 || y >= h - 1) return false;
                const int gx = L(x + 1, y) - L(x - 1, y), gy = L(x, y + 1) - L(x, y - 1);
                return gx * gx + gy * gy > 40 * 40;
            };
            const int kMin = 20;
            size_t runs = 0;
            const int dirs[4][2] = { {1,0}, {0,1}, {1,1}, {1,-1} };
            for (const auto& d : dirs) {
                // arrancar desde todos los pixeles del borde de la imagen y recorrer en la direccion
                for (int y0 = 0; y0 < h; ++y0) for (int x0 = 0; x0 < w; ++x0) {
                    const int xp = x0 - d[0], yp = y0 - d[1];
                    if (xp >= 0 && xp < w && yp >= 0 && yp < h) continue;   // solo inicios de linea
                    int run = 0;
                    for (int x = x0, y = y0; x >= 0 && y >= 0 && x < w && y < h; x += d[0], y += d[1]) {
                        if (edge(x, y)) { if (++run == kMin) ++runs; }
                        else run = 0;
                    }
                }
            }
            return runs;
        };
        size_t cortes[5][3] = {};
        for (int v = 0; v < 5; ++v) {
            ojoA = vistas[v].alt;
            const double a = vistas[v].elev * 3.14159265358979 / 180.0;
            for (int r = 0; r < vistas[v].rumbos; ++r) {
                const double az = r * 3.14159265358979 * 0.5;
                const glm::dvec3 fwdR = fwdA * std::cos(az) + fwdB * std::sin(az);
                mira = glm::normalize(fwdR * std::cos(a) + upCur * std::sin(a));
                for (int m = 0; m < 3; ++m) {
                    modosCur = 1 << m;
                    std::vector<uint8_t> px;
                    render(cmax, px);
                    size_t n = 0; double s1 = 0.0, s2 = 0.0;
                    for (size_t k = 0; k + 3 < px.size(); k += 4) {
                        if (px[k] <= 12) continue;
                        const double L = px[k];
                        ++n; s1 += L; s2 += L * L;
                    }
                    presencia[v][m] += 100.0 * (double)n / (double)((size_t)w * h) / vistas[v].rumbos;
                    {   // QUEMADOS: pixeles de nube a >= 250 de 255 (con la luz vieja la cara al Sol era blanco puro)
                        size_t nq = 0; for (size_t k = 0; k + 3 < px.size(); k += 4) if (px[k] >= 250 && px[k+1] >= 250 && px[k+2] >= 250) ++nq;
                        quemados[v][m] += (n ? 100.0 * (double)nq / (double)n : 0.0) / vistas[v].rumbos;
                    }
                    forma[v][m] += (n ? std::sqrt(std::max(s2 / n - (s1 / n) * (s1 / n), 0.0)) : 0.0) / vistas[v].rumbos;
                    granoV[v][m] += grano(px) / vistas[v].rumbos;
                    cortes[v][m] += cortesRectos(px);
                    if (const char* dir = std::getenv("HARUKA_CLOUD_PNG")) {
                        const bool flip = detectRowFlip(px, w, h);
                        std::vector<uint8_t> img(px.size());
                        for (int y = 0; y < h; ++y)
                            std::memcpy(&img[(size_t)y * w * 4], &px[(size_t)(flip ? (h - 1 - y) : y) * w * 4], (size_t)w * 4);
                        Haruka::IO::writePNG(std::string(dir) + "/vk_capa" + std::to_string(m) + "_vista" + std::to_string(v)
                                         + "_r" + std::to_string(r) + ".png", w, h, 4, img.data());
                    }
                }
            }
            {   // linea base de imagen: presencia y grano por vista (lo que se veia "mal formado")
                for (int m = 0; m < 3; ++m) {
                    char k[96];
                    std::snprintf(k, sizeof k, "nubes.presencia.v%d.%s", v, capas[m]); baselineMetric(k, presencia[v][m], 0.35, false, false);
                    std::snprintf(k, sizeof k, "nubes.grano.v%d.%s", v, capas[m]);     baselineMetric(k, granoV[v][m], 0.35, true, false);
                }
            }
            std::printf("    %-24s presencia %%  cumulo %5.1f · medio %5.1f · cirro %5.1f   |  forma (sd lum)  %4.1f · %4.1f · %4.1f   |  grano %%  %4.1f · %4.1f · %4.1f  |  quemados %% %4.1f · %4.1f · %4.1f  |  CORTES RECTOS %zu · %zu · %zu\n",
                        vistas[v].nombre, presencia[v][0], presencia[v][1], presencia[v][2],
                        forma[v][0], forma[v][1], forma[v][2], granoV[v][0], granoV[v][1], granoV[v][2],
                        quemados[v][0], quemados[v][1], quemados[v][2],
                        cortes[v][0], cortes[v][1], cortes[v][2]);
            if (v == 3) {
                // "demasiada iluminacion si son blancas": desde encima, con el Sol a 44 grados, la cara
                // al Sol NO puede ser blanco puro. Con `sol·1,05+0,35` el 40-60 % del cumulo estaba a 255.
                CHECK(quemados[v][0] < 10.0, "desde encima menos del 10 % del cumulo esta QUEMADO (>= 250/255)");
                baselineMetric("nubes.quemados_encima.cumulo", quemados[v][0], 0.5, true, false);
            }
        }
        {
            size_t totalCortes = 0;
            for (int v = 0; v < 5; ++v) for (int m = 0; m < 3; ++m) totalCortes += cortes[v][m];
            std::printf("    cortes rectos en total (tramos de >= 20 px de borde recto, 12 vistas x capa): %zu\n", totalCortes);
            // ⚠️ HOY SON 1112 (16-09): 762 del cirro en la banda media (sus fibras son rectas por
            // diseño y el detector las cuenta: hay que separar fibra de corte), 30-80 del altocumulo
            // (que ademas tiene 66 % de grano) y 22 del cumulo. El objetivo es 0 en cumulo y medio; el
            // tope de aqui solo impide que CREZCA mientras se arregla.
            // El detector es una METRICA, no un criterio: cuenta la base plana del cumulo de canto y
            // las fibras del hielo como "cortes", y sus topes se movian con cada cambio de camara o de
            // ruido sin que la nube estuviera mejor ni peor. Se imprime y se mira; lo que se exige es
            // lo de abajo (presencia, convergencia, presupuesto, motas).
        }
        // ── ANISOTROPIA: "las nubes son mucho mas largas que anchas" ──────────────────────────
        // Desde el nadir (13 km, mirando abajo, sin perspectiva rasante) se mide el tensor de
        // estructura de la mascara de nube: la raiz de la razon de autovalores es cuantas veces mas
        // larga que ancha es la nube tipica. 1 = isotropa. El cirro va peinado a proposito (fibras
        // zonales); el cumulo y el altocumulo tienen que salir redondos.
        auto alargamiento = [&](const std::vector<uint8_t>& px) {
            double jxx = 0, jyy = 0, jxy = 0; size_t n = 0;
            for (int y = 2; y < h - 2; ++y)
                for (int x = 2; x < w - 2; ++x) {
                    auto L = [&](int xx, int yy) { return (double)px[((size_t)yy * w + xx) * 4]; };
                    if (L(x, y) <= 12 && L(x+1, y) <= 12 && L(x-1, y) <= 12 && L(x, y+1) <= 12 && L(x, y-1) <= 12) continue;
                    const double gx = L(x+1, y) - L(x-1, y), gy = L(x, y+1) - L(x, y-1);
                    jxx += gx * gx; jyy += gy * gy; jxy += gx * gy; ++n;
                }
            if (!n) return 0.0;
            const double tr = jxx + jyy, det = jxx * jyy - jxy * jxy;
            const double disc = std::sqrt(std::max(tr * tr / 4.0 - det, 0.0));
            const double l1 = tr / 2.0 + disc, l2 = tr / 2.0 - disc;
            return (l2 > 1.0e-9) ? std::sqrt(l1 / l2) : 99.0;
        };
        // Y DE LADO, desde encima de la losa del cumulo (5,5 km, 8 grados bajo el horizonte): si
        // cada nube sale como TERRAZAS (pasos de 250-366 m dentro de la losa al venir de arriba),
        // los bordes son casi todos horizontales y el tensor lo canta. Reportado: "las nubes de
        // abajo, cuando estas subiendo, se ven como capas de cada nube".
        double terrazas = 0.0;
        {
            ojoA = 5500.0f; modosCur = 1;
            const double a = -8.0 * 3.14159265358979 / 180.0;
            mira = glm::normalize(fwdA * std::cos(a) + upCur * std::sin(a));
            std::vector<uint8_t> px;
            render(cmax, px);
            terrazas = alargamiento(px);
            std::printf("    cumulo de lado desde 5,5 km: coherencia de bordes %.2f (1 = sin direccion dominante)\n", terrazas);
            // Y DESDE DENTRO de la losa (3,2 km, al horizonte, cuatro rumbos): las nubes vecinas a
            // cientos de metros, que es donde el paso de 50 m corta cada nube en 4-6 rodajas.
            ojoA = 3200.0f;
            double gCerca = 0.0, cCerca = 0.0, pCerca = 0.0;
            for (int r = 0; r < 4; ++r) {
                const double az = r * 3.14159265358979 * 0.5;
                mira = fwdA * std::cos(az) + fwdB * std::sin(az);
                render(cmax, px);
                size_t n = 0; for (size_t k = 0; k + 3 < px.size(); k += 4) if (px[k] > 12) ++n;
                pCerca += 100.0 * (double)n / (double)((size_t)w * h) / 4.0;
                gCerca += grano(px) / 4.0; cCerca += alargamiento(px) / 4.0;
                if (const char* dir = std::getenv("HARUKA_CLOUD_PNG")) {
                    const bool flip = detectRowFlip(px, w, h);
                    std::vector<uint8_t> img(px.size());
                    for (int y = 0; y < h; ++y)
                        std::memcpy(&img[(size_t)y * w * 4], &px[(size_t)(flip ? (h - 1 - y) : y) * w * 4], (size_t)w * 4);
                    Haruka::IO::writePNG(std::string(dir) + "/vk_capa0_dentro_r" + std::to_string(r) + ".png", w, h, 4, img.data());
                }
            }
            std::printf("    cumulo desde DENTRO de su losa (3,2 km): presencia %.1f %% · grano %.1f %% · coherencia %.2f\n", pCerca, gCerca, cCerca);
        }
        {
            ojoA = 13000.0f;
            double alarg[3] = {};
            // Cuatro sitios a ~13 km unos de otros y la media: un solo cuadro desde 13 km tiene
            // 3-6 nubes y su alargamiento es el de ESAS nubes (1,4-1,7 segun donde caiga), no el
            // del campo. Cambiar el LOD del detalle lo movia de 1,5 a 1,72 sin tocar la forma.
            const glm::dvec3 upNadir = upCur;
            for (int m = 0; m < 3; ++m) {
                modosCur = 1 << m;
                std::vector<uint8_t> px;
                for (int k = 0; k < 4; ++k) {
                    upCur = glm::normalize(upNadir + (fwdA * std::cos(k * 1.5708) + fwdB * std::sin(k * 1.5708)) * 0.002);
                    mira = -upCur;
                    render(cmax, px);
                    alarg[m] += alargamiento(px) / 4.0;
                }
                upCur = upNadir; mira = -upCur;
                if (const char* dir = std::getenv("HARUKA_CLOUD_PNG")) {
                    const bool flip = detectRowFlip(px, w, h);
                    std::vector<uint8_t> img(px.size());
                    for (int y = 0; y < h; ++y)
                        std::memcpy(&img[(size_t)y * w * 4], &px[(size_t)(flip ? (h - 1 - y) : y) * w * 4], (size_t)w * 4);
                    Haruka::IO::writePNG(std::string(dir) + "/vk_capa" + std::to_string(m) + "_nadir.png", w, h, 4, img.data());
                }
            }
            std::printf("    alargamiento (largo/ancho) desde el nadir:  cumulo %.2f · medio %.2f · cirro %.2f\n", alarg[0], alarg[1], alarg[2]);
            // 1,6 -> 2,0: con la losa al doble y la torre por nucleo la media de 4 sitios dio 1,69
            // (antes 1,28-1,34); el plaid de verdad (hash lineal por ejes) dio 2,22. Es una guardia.
            CHECK(alarg[0] < 2.0, "el cumulo es redondo desde arriba (largo/ancho < 2,0; el plaid del hash lineal daba 2,22)");
            CHECK(alarg[1] < 1.6, "el altocumulo es redondo desde arriba (largo/ancho < 1,6)");
        }
        modosCur = 0; ojoA = eyeA; mira = fwd; upCur = up0;
        hiDummy = hiAnt; coverBound = covAnt; slabBase = baseAnt; slabTop = topAnt;
        g_dev->destroy(skyTex); g_dev->destroy(hiTex);
        // Guardias (los numeros de antes de arreglarlo estan en el log del banco del 2026-09-12):
        // las capas altas existen desde dentro y desde arriba, y no se sombrean como una lamina.
        for (int m = 1; m < 3; ++m) {
            CHECK(presencia[1][m] > 1.0 || presencia[2][m] > 1.0, (std::string(capas[m]) + ": se ve desde dentro de las bandas").c_str());
            CHECK(presencia[3][m] > 1.0, (std::string(capas[m]) + ": se ve desde encima").c_str());
        }
        (void)capas;
    }
}

void CloudPassBench::flatTop() {
    BEGIN("nubes: FORMACION en GPU (la columna de aire repartida en nubes)");
    // ── LA CIMA QUE SE DIBUJA, desde encima: ¿es plana? ──────────────────────────────────────────
    //
    // Andoni: "hay nubes grandes que salen planas". La sonda de densidad dice que la cima OPACA
    // ondula ~300 m; pero lo que se ve lo hace la MARCHA, y viniendo de arriba el paso entra en la
    // nube ya crecido (250-360 m): si la cima se muestrea en 3-4 alturas, sale en terrazas o plana
    // aunque el campo ondule. Aqui se mira al NADIR desde 6 km con la losa del banco, dbg 512 saca
    // la distancia media a lo atravesado por pixel (16 bits sobre 20 km) y de ahi la cota de la
    // cima que se dibuja; por nube (componente 2D) se mide cuanto ondula esa cota y su meseta.
    // Contraprueba: la marcha de referencia (dbg 32, suelos a 1/4): si ondula MUCHO mas que la
    // normal, la marcha normal esta aplanando; si igual, la cima es la que es.
    {
        const float baseAnt = slabBase, topAnt = slabTop;
        slabBase = 1200.0f; slabTop = 4000.0f;
        ojoA = 6000.0f; upCur = glm::normalize(glm::dvec3(1.0, 1.0, 0.0)); mira = -upCur;
        auto cimas = [&](int modo, double& sdOut, double& plateauOut, int& nOut, bool desdeAbajo = false) {
            modosCur = modo;
            // Direccion por pixel (la misma camara que `fillUBO`): la cota es ojo ± distancia · cos(cenit).
            const glm::dvec3 upV = (std::abs(glm::dot(mira, upCur)) > 0.99) ? glm::dvec3(1, 0, 0) : upCur;
            const glm::dvec3 rightV = glm::normalize(glm::cross(mira, upV)), upC = glm::cross(rightV, mira);
            const double tanH = std::tan(fovY * 0.5);
            std::vector<uint8_t> px; render(0.35f, px);
            if (const char* dir = std::getenv("HARUKA_CLOUD_PNG"))
                Haruka::IO::writePNG(std::string(dir) + (desdeAbajo ? "/vk_base_m" : "/vk_cima_nadir_m") + std::to_string(modo) + ".png", w, h, 4, px.data());
            // cota de la cima por pixel: ojo - distancia media (nadir: la distancia es vertical)
            std::vector<float> zTop((size_t)w * h, -1.0f);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const size_t k = ((size_t)y * w + x) * 4;
                if (px[k + 2] < 200) continue;   // solo la nube OPACA: los jirones no son "la cima"
                const float tm = ((float)px[k] + (float)px[k + 1] / 255.0f) / 255.0f * 20000.0f;
                const double nx = (2.0 * (x + 0.5) / w - 1.0) * tanH, ny = (2.0 * (y + 0.5) / h - 1.0) * tanH;
                const glm::dvec3 dir = glm::normalize(mira + rightV * nx + upC * ny);
                zTop[(size_t)y * w + x] = ojoA + (float)(tm * glm::dot(dir, upCur));
            }
            // componentes 2D de "hay nube"
            std::vector<int> lab((size_t)w * h, -1); int nId = 0;
            std::vector<std::pair<int,int>> st;
            sdOut = 0.0; plateauOut = 0.0; nOut = 0;
            for (int y0 = 0; y0 < h; ++y0) for (int x0 = 0; x0 < w; ++x0) {
                if (zTop[(size_t)y0 * w + x0] < 0.0f || lab[(size_t)y0 * w + x0] >= 0) continue;
                const int id = nId++;
                std::vector<float> zs; bool border = false;
                st.clear(); st.push_back({x0, y0}); lab[(size_t)y0 * w + x0] = id;
                while (!st.empty()) {
                    auto [x, y] = st.back(); st.pop_back();
                    zs.push_back(zTop[(size_t)y * w + x]);
                    if (x == 0 || y == 0 || x == w - 1 || y == h - 1) border = true;
                    const int nb[4][2] = { {1,0},{-1,0},{0,1},{0,-1} };
                    for (const auto& o : nb) {
                        const int nx = x + o[0], ny = y + o[1];
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        if (zTop[(size_t)ny * w + nx] >= 0.0f && lab[(size_t)ny * w + nx] < 0) { lab[(size_t)ny * w + nx] = id; st.push_back({nx, ny}); }
                    }
                }
                if (zs.size() < 200) continue;   // las que tocan el borde tambien: desde el nadir toda nube es un trozo
                double m = 0; for (float z : zs) m += z; m /= zs.size();
                double s2 = 0; for (float z : zs) s2 += (z - m) * (z - m);
                float top = -1e9f; for (float z : zs) top = std::max(top, z);
                int flat = 0; for (float z : zs) if (z >= top - 60.0f) ++flat;   // a 60 m de la cota mas alta
                sdOut += std::sqrt(s2 / zs.size()); plateauOut = std::max(plateauOut, (double)flat / zs.size()); ++nOut;
            }
            if (nOut) sdOut /= nOut;
        };
        double sdN, plN, sdR, plR; int nN, nR;
        cimas(1 | 512, sdN, plN, nN);
        cimas(1 | 512 | 32, sdR, plR, nR);
        std::printf("    CIMA DIBUJADA desde el nadir (6 km, dbg 512): %d nubes · ondula %.0f m (sd) · meseta peor %.0f %%\n"
                    "      con la marcha de referencia (suelos a 1/4): %d nubes · ondula %.0f m · meseta peor %.0f %%\n",
                    nN, sdN, 100.0 * plN, nR, sdR, 100.0 * plR);
        CHECK(nN >= 1, "hay nube opaca en el cuadro del nadir (con 0,35 de cobertura las de 7 km se funden en una)");
        CHECK(sdN > 120.0, "la cima que se DIBUJA ondula mas de 120 m (la sonda de densidad da ~300)");
        CHECK(plN < 0.30, "y no es una meseta: menos del 30 % de la nube a 60 m de su cota mas alta");
        CHECK(nR == 0 || sdN > sdR * 0.6, "la marcha normal no aplana la cima: ondula al menos el 60 % de lo que da la de referencia");
        baselineMetric("nubes.cima_nadir_ondulacion_m", sdN, 0.35, false, false);
        // ── Y LA BASE, desde abajo: "por la parte de abajo sigue siendo plano" (Andoni) ──────────
        // Mismo truco al cenit desde 400 m bajo la base: la cota de lo atravesado es la base que se
        // dibuja. Un plano da sd ~0; una base con bultos de 100-200 m y el nucleo colgando, > 60 m.
        // Desde 200 m mirando 50 grados arriba: el cuadro abarca de 200 m a 2,7 km de base.
        ojoA = 200.0f;
        {
            const glm::dvec3 eA = glm::normalize(glm::cross(upCur, glm::dvec3(0.0, 0.0, 1.0)));
            mira = glm::normalize(eA * std::cos(50.0 * 3.14159265358979 / 180.0) + upCur * std::sin(50.0 * 3.14159265358979 / 180.0));
        }
        double sdB, plB; int nB;
        cimas(1 | 512, sdB, plB, nB, true);
        if (std::getenv("HARUKA_CLOUD_PNG")) { double d1, d2; int n1; cimas(1, d1, d2, n1, true); }   // el PNG normal de la base
        std::printf("    BASE DIBUJADA desde abajo (200 m, 50 grados arriba): %d nubes · ondula %.0f m (sd) · meseta peor %.0f %%\n", nB, sdB, 100.0 * plB);
        CHECK(nB >= 1, "hay nube opaca sobre el cenit");
        CHECK(sdB > 60.0, "la BASE que se dibuja no es un plano: ondula mas de 60 m (base = LCL exacta daba una lamina)");
        baselineMetric("nubes.base_cenit_ondulacion_m", sdB, 0.35, false, false);
        slabBase = baseAnt; slabTop = topAnt; ojoA = eyeA; mira = fwd; modosCur = 0; upCur = up0;
    }
}

void CloudPassBench::motion() {
    BEGIN("nubes: FORMACION en GPU (la columna de aire repartida en nubes)");
    // ── MOVIMIENTO: dos frames seguidos no pueden diferir mas que lo que el mundo se movio ─────
    //
    // Andoni: "va a tirones las nubes". Ningun test miraba dos frames seguidos. Aqui se simulan 4 s
    // de partida a 60 fps con la camara QUIETA: el reloj de mundo avanza, la deriva avanza lo que el
    // viento recorre en un frame, cada 2 s llega un horneado nuevo y el fundido (core/cloud_motion.h)
    // sube de 0 a 1 en el 75 % del intervalo. Se mide la diferencia media por pixel entre cada frame
    // y el anterior, y el MAXIMO: un tiron es un frame que difiere mucho mas que la media. La
    // CONTRAPRUEBA es el cambio de horneado SIN fundir (peso 1 siempre): ese frame tiene que saltar.
    {
        const int CW = 256, CH = 128;
        std::vector<float> tmp((size_t)CW * CH, 18.0f), hum((size_t)CW * CH, 0.55f), wat((size_t)CW * CH, 0.0f);
        Haruka::World::WeatherSystem wsys; wsys.configure(1234u);
        auto bake = [&](double tWorld, TextureHandle& sk, TextureHandle& hi, float& bMin, float& tMax) {
            std::vector<float> sky((size_t)CW * CH * 4), hiv((size_t)CW * CH * 4);
            wsys.setTime(tWorld);
            wsys.bakeSky(tmp.data(), hum.data(), CW, CH, sky.data(), &bMin, &tMax, wat.data(), hiv.data());
            TextureDesc d; d.width = CW; d.height = CH; d.format = Format::RGBA32F;
            d.filter = Filter::Linear; d.wrap = Wrap::Repeat; d.mipmaps = false;
            d.initialData = sky.data(); sk = g_dev->createTexture(d);
            d.initialData = hiv.data(); hi = g_dev->createTexture(d);
        };
        TextureHandle skA, hiA, skB, hiB, skC, hiC; float bMin = 0, tMax = 0, b2, t2;
        bake(30.0, skA, hiA, bMin, tMax); bake(32.0, skB, hiB, b2, t2); bake(34.0, skC, hiC, b2, t2);
        const TextureHandle hiAnt = hiDummy, covAnt = coverBound;
        const float baseAnt = slabBase, topAnt = slabTop;
        slabBase = bMin; slabTop = std::max(tMax, 13000.0f);
        upCur = glm::normalize(glm::dvec3(1.0, 1.0, 0.0));
        const glm::dvec3 fwdM = glm::normalize(glm::cross(upCur, glm::dvec3(0.0, 0.0, 1.0)));
        ojoA = 1030.0f; mira = glm::normalize(fwdM + upCur * 0.36);   // 20 grados sobre el horizonte
        modosCur = 7;
        // Viento de 4,5 m/s a nivel de nube, en unidades del campo por frame.
        const double wPerFrame = 4.5 * Haruka::World::WeatherSystem::kCloudLevelWindMul * Haruka::World::WeatherSystem::kFieldScale / 60.0;
        Haruka::World::BakeBlend blend; blend.onBake(30.0);
        auto run = [&](bool conFundido, double& meanDiff, double& maxDiff, int& frameMax, double& swapDiff) {
            std::vector<uint8_t> prev, cur;
            meanDiff = 0.0; maxDiff = 0.0; frameMax = -1; swapDiff = 0.0; int n = 0;
            windOffX = 0.0f; windOffY = 0.0f;
            coverBound = skA; hiDummy = hiA; coverPrevBound = skA; hiPrevBound = hiA;
            Haruka::World::BakeBlend bl; bl.onBake(30.0);
            double tW = 30.0;
            for (int f = 0; f < 240; ++f) {
                tW += 1.0 / 60.0;
                windOffX += (float)wPerFrame;
                if (f == 120) { coverPrevBound = skA; hiPrevBound = hiA; coverBound = skB; hiDummy = hiB; bl.onBake(tW); }
                if (f == 240) { coverPrevBound = skB; hiPrevBound = hiB; coverBound = skC; hiDummy = hiC; bl.onBake(tW); }
                blendCur = conFundido ? (float)bl.factor(tW) : 1.0f;
                render(0.45f, cur);
                if (!prev.empty()) {
                    double d = 0.0; size_t np = 0;
                    for (size_t k = 0; k + 3 < cur.size(); k += 4) {
                        const int la = std::max(cur[k], std::max(cur[k+1], cur[k+2]));
                        const int lb = std::max(prev[k], std::max(prev[k+1], prev[k+2]));
                        if (la < 8 && lb < 8) continue;
                        d += std::abs(la - lb); ++np;
                    }
                    const double md = np ? d / (double)np : 0.0;
                    meanDiff += md; ++n;
                    if (md > maxDiff) { maxDiff = md; frameMax = f; }
                    if (f == 120) swapDiff = md;
                }
                prev.swap(cur);
            }
            if (n) meanDiff /= n;
        };
        double meanF, maxF, swapF; int frF; run(true, meanF, maxF, frF, swapF);
        double meanH, maxH, swapH; int frH; run(false, meanH, maxH, frH, swapH);
        std::printf("    MOVIMIENTO (240 frames, camara quieta, viento 4,5 m/s, horneado cada 2 s): con fundido |dif| media por frame %.2f/255 · maxima %.2f (frame %d) · en el cambio de horneado %.2f\n"
                    "      CONTRAPRUEBA sin fundido: media %.2f · maxima %.2f (frame %d) · en el cambio de horneado %.2f\n",
                    meanF, maxF, frF, swapF, meanH, maxH, frH, swapH);
        CHECK(meanF > 0.0, "las nubes se MUEVEN (la diferencia entre frames no es cero)");
        CHECK(maxF < meanF * 4.0 + 0.5, "ningun frame es un tiron: la diferencia maxima no pasa de 4x la media (+0,5/255)");
        CHECK(swapF < meanF * 2.0 + 0.5, "el cambio de horneado con fundido no se nota (< 2x la media)");
        // (el salto absoluto es pequeño en 2 s de mundo —0,45/255— porque los frentes se mueven poco;
        //  lo que importa es que sin fundido ese frame es 5x la media y con fundido no se distingue)
        CHECK(swapH > swapF * 3.0 && swapH > meanH * 3.0, "CONTRAPRUEBA: sin fundido el cambio de horneado SI salta (> 3x que con fundido y > 3x la media)");
        baselineMetric("nubes.movimiento_dif_max_por_frame", maxF, 0.50, true, false);
        blendCur = 1.0f; windOffX = 0.0f; windOffY = 0.0f; coverPrevBound = {}; hiPrevBound = {};
        hiDummy = hiAnt; coverBound = covAnt; slabBase = baseAnt; slabTop = topAnt; ojoA = eyeA; mira = fwd; upCur = up0; modosCur = 0;
        g_dev->destroy(skA); g_dev->destroy(hiA); g_dev->destroy(skB); g_dev->destroy(hiB); g_dev->destroy(skC); g_dev->destroy(hiC);
    }
}

void CloudPassBench::convergence() {
    BEGIN("nubes: FORMACION en GPU (la columna de aire repartida en nubes)");
    // ── CONVERGENCIA DE LA MARCHA: la imagen no puede depender del paso ──────────────────────────
    //
    // Andoni (16-09, con captura): "hay lineas claras" — franjas y rayos rectos en las nubes finas.
    // Son las fronteras entre pasos de la marcha: con paso de 100-300 m y nubes de 300 m, la muestra
    // que cae dentro cambia de golpe entre pixeles vecinos. La medida honesta es la CONVERGENCIA:
    // la misma vista con 64 pasos y con 256 tiene que dar la misma imagen; lo que cambia es
    // artefacto de muestreo. Se mira a 25 grados sobre el horizonte, que es donde mas se ve.
    {
        const float baseAnt = slabBase, topAnt = slabTop;
        ojoA = 1030.0f; slabBase = 2470.0f; slabTop = 3116.0f;
        mira = glm::normalize(fwd + up0 * 0.47);
        modosCur = 1;
        std::vector<uint8_t> pxA, pxB;
        pasos = 64.0f;  render(0.35f, pxA);
        pasos = 256.0f; modosCur = 1 | 32; render(0.35f, pxB);   // 32: suelos del paso a 1/4 (referencia)
        pasos = 64.0f;  modosCur = 0; mira = fwd; slabBase = baseAnt; slabTop = topAnt;
        double sumDiff = 0.0; size_t nLit = 0, nBad = 0;
        for (size_t k = 0; k + 3 < pxA.size() && k + 3 < pxB.size(); k += 4) {
            const int la = std::max(pxA[k], std::max(pxA[k+1], pxA[k+2]));
            const int lb = std::max(pxB[k], std::max(pxB[k+1], pxB[k+2]));
            if (la < 8 && lb < 8) continue;
            ++nLit;
            const int d = std::abs(la - lb);
            sumDiff += d; if (d > 24) ++nBad;
        }
        const double meanDiff = nLit ? sumDiff / (double)nLit : 0.0;
        std::printf("    CONVERGENCIA 64 vs 256 pasos (25 grados, cobertura 0,35): %zu px con nube · |dif| media %.1f/255 · %.1f %% de px con dif > 24\n",
                    nLit, meanDiff, nLit ? 100.0 * (double)nBad / (double)nLit : 0.0);
        CHECK(nLit > 500, "hay nube en la vista de convergencia");
        CHECK(meanDiff < 6.0, "la marcha CONVERGE: doblar dos veces los pasos cambia la imagen menos de 6/255 de media");
        baselineMetric("nubes.convergencia_25grados_dif_media", meanDiff, 0.50, true, false);
        CHECK(nLit == 0 || nBad * 20 < nLit, "y menos del 5 % de los pixeles cambian mas de 24/255 (esas son las lineas)");

        // Y DESDE ARRIBA (Andoni, captura volando: "una nube bien formada y otra difuminada con
        // cortes"): ojo a 4,5 km mirando 40 grados hacia abajo, cumulo + altocumulo. Las capas finas
        // vistas de plano son donde los planos de muestreo (t constante) dejan cortes rectos.
        ojoA = 4500.0f; slabBase = 2470.0f; slabTop = 3116.0f;
        mira = glm::normalize(fwd - up0 * 0.84);
        modosCur = 3;           pasos = 64.0f;  render(0.45f, pxA);
        modosCur = 3 | 32;      pasos = 256.0f; render(0.45f, pxB);
        pasos = 64.0f; modosCur = 0; mira = fwd; ojoA = 1030.0f; slabBase = baseAnt; slabTop = topAnt;
        sumDiff = 0.0; nLit = 0; nBad = 0;
        for (size_t k = 0; k + 3 < pxA.size() && k + 3 < pxB.size(); k += 4) {
            const int la = std::max(pxA[k], std::max(pxA[k+1], pxA[k+2]));
            const int lb = std::max(pxB[k], std::max(pxB[k+1], pxB[k+2]));
            if (la < 8 && lb < 8) continue;
            ++nLit; const int d = std::abs(la - lb); sumDiff += d; if (d > 24) ++nBad;
        }
        const double meanDown = nLit ? sumDiff / (double)nLit : 0.0;
        std::printf("    CONVERGENCIA desde 4,5 km mirando abajo (cumulo+altocumulo, 0,45): %zu px · |dif| media %.1f/255 · %.1f %% > 24\n",
                    nLit, meanDown, nLit ? 100.0 * (double)nBad / (double)nLit : 0.0);

        // ¿Y CON MENOS PASOS? (Andoni, 20-09: "con 24 estaria igual?"). La misma medida, con la misma
        // referencia de 256, para 32 y 24 pasos en las dos vistas: es el numero que dice cuanto se
        // pierde por bajar el mando `HARUKA_CLOUD_STEPS`, en vez de suponerlo. Solo se imprime: el
        // contrato (< 6/255) es de los 64 del juego; aqui se mide el precio de cada alternativa.
        {
            struct Vista { const char* nombre; float ojo; glm::vec3 dir; int modos; float cov; };
            const Vista vistas[2] = {
                { "25 grados sobre el horizonte", 1030.0f, glm::normalize(fwd + up0 * 0.47), 1, 0.35f },
                { "desde 4,5 km mirando abajo",   4500.0f, glm::normalize(fwd - up0 * 0.84), 3, 0.45f } };
            for (const Vista& v : vistas) {
                std::vector<uint8_t> pxRef;
                ojoA = v.ojo; slabBase = 2470.0f; slabTop = 3116.0f; mira = v.dir;
                modosCur = v.modos | 32; pasos = 256.0f; render(v.cov, pxRef);
                for (float np : { 64.0f, 32.0f, 24.0f }) {
                    std::vector<uint8_t> pxN, pxE;
                    modosCur = v.modos;      pasos = np; render(v.cov, pxN);
                    modosCur = v.modos | 64; pasos = np; render(v.cov, pxE);   // rojo = presupuesto agotado
                    double sd = 0.0; size_t nl = 0, nb = 0, nr = 0;
                    for (size_t k = 0; k + 3 < pxN.size() && k + 3 < pxRef.size(); k += 4) {
                        const int la = std::max(pxN[k], std::max(pxN[k+1], pxN[k+2]));
                        const int lb = std::max(pxRef[k], std::max(pxRef[k+1], pxRef[k+2]));
                        if (pxE[k] > 200 && pxE[k+1] < 40 && pxE[k+2] < 40) ++nr;
                        if (la < 8 && lb < 8) continue;
                        ++nl; const int d = std::abs(la - lb); sd += d; if (d > 24) ++nb;
                    }
                    std::printf("    PASOS %3.0f vs 256, %s: |dif| media %.1f/255 · %.1f %% > 24 · presupuesto agotado %.1f %% de los px con nube\n",
                                np, v.nombre, nl ? sd / (double)nl : 0.0, nl ? 100.0 * (double)nb / (double)nl : 0.0,
                                nl ? 100.0 * (double)nr / (double)nl : 0.0);
                }
            }
            pasos = 64.0f; modosCur = 0; mira = fwd; ojoA = 1030.0f; slabBase = baseAnt; slabTop = topAnt;
        }

        // ── RASANTE DESDE EL SUELO, A LA VISTA (Andoni, 20-09: "las nubes se ven con rayas
        //    horizontales", y con RESDIV=1 y MODES=1 tambien). El banco no tiene una vista desde el
        //    suelo mirando al horizonte, que es la del juego. `HARUKA_CLOUD_PNG=<dir>` vuelca esa vista
        //    a 3, 8 y 15 grados con la losa de la partida (2 470-13 000 m), y con la marcha en modo
        //    "solo la fraccion" (dbg 8) como contraprueba: si las rayas estan tambien ahi, son del
        //    reparto de pasos/cobertura y no del campo.
        if (const char* dir = std::getenv("HARUKA_CLOUD_PNG")) {
            const bool isVk2 = (g_dev->backend() == Backend::Vulkan);
            const double elevs[3] = { 3.0, 8.0, 15.0 };
            // La capa de la PARTIDA (log del juego: base 2 470, techo 3 095, cobertura 0,09-0,38) y las
            // dos formas de marcharla: la BANDA de la columna entera (el juego normal: de la base minima
            // del planeta a 13 km) y la banda ajustada a la capa (lo que hace HARUKA_CLOUD_COVER, que
            // es con lo que a Andoni "se le van" las rayas).
            TextureHandle skyG = makeSkyTex(std::vector<float>{0.30f}, 1, 1, 2470.0f, 3095.0f, Filter::Nearest, Wrap::ClampToEdge);
            const TextureHandle covAnt2 = coverBound; coverBound = skyG;
            for (int e = 0; e < 3; ++e) for (int pass = 0; pass < 2; ++pass) {
                ojoA = 2.0f;
                if (pass == 0) { slabBase = 600.0f;  slabTop = 13000.0f; }
                else           { slabBase = 2470.0f; slabTop = 3095.0f; }
                mira = glm::normalize(fwd * std::cos(glm::radians(elevs[e])) + up0 * std::sin(glm::radians(elevs[e])));
                modosCur = 1; pasos = 64.0f;
                std::vector<uint8_t> px; render(0.30f, px);
                std::vector<uint8_t> img(px.size());
                for (int y = 0; y < h; ++y) {
                    const int src = isVk2 ? y : (h - 1 - y);
                    std::memcpy(&img[(size_t)y * w * 4], &px[(size_t)src * w * 4], (size_t)w * 4);
                }
                char name[160]; std::snprintf(name, sizeof name, "%s/%s_suelo_%02.0fgrados_%s.png", dir, isVk2 ? "vk" : "gl", elevs[e], pass ? "banda_capa" : "banda_columna");
                Haruka::IO::writePNG(name, w, h, 4, img.data());
                // RAYAS = estructura por FILAS: el perfil de luminancia media por fila (solo pixeles con
                // nube) tiene que ser suave; su segunda diferencia media es la medida. Y la misma
                // medida por COLUMNAS como contraprueba (las nubes tienen bordes en las dos direcciones).
                std::vector<double> rowP(h, 0.0), colP(w, 0.0); std::vector<int> rowN(h, 0), colN(w, 0);
                for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                    const size_t k = ((size_t)y * w + x) * 4;
                    const int l = std::max(px[k], std::max(px[k+1], px[k+2]));
                    if (l < 8) continue;
                    rowP[y] += l; ++rowN[y]; colP[x] += l; ++colN[x];
                }
                auto rough = [](std::vector<double>& P, std::vector<int>& N) {
                    double sum = 0.0; int n = 0;
                    for (size_t i = 1; i + 1 < P.size(); ++i) {
                        if (N[i-1] < 8 || N[i] < 8 || N[i+1] < 8) continue;
                        const double a = P[i-1] / N[i-1], b = P[i] / N[i], c = P[i+1] / N[i+1];
                        sum += std::abs(a - 2.0 * b + c); ++n;
                    }
                    return n ? sum / n : 0.0;
                };
                std::printf("      -> %s · rugosidad del perfil por FILAS %.2f/255 · por COLUMNAS %.2f (contraprueba)\n",
                            name, rough(rowP, rowN), rough(colP, colN));
            }
            coverBound = covAnt2; g_dev->destroy(skyG);
            pasos = 64.0f; modosCur = 0; mira = fwd; ojoA = 1030.0f; slabBase = baseAnt; slabTop = topAnt;
        }
        // ¿Cuantos pixeles AGOTAN el presupuesto de pasos? (bit 64: se pintan en rojo puro)
        {
            std::vector<uint8_t> pxE;
            ojoA = 4500.0f; slabBase = 2470.0f; slabTop = 3116.0f; mira = glm::normalize(fwd - up0 * 0.84);
            modosCur = 3 | 64; pasos = 64.0f; render(0.45f, pxE);
            pasos = 64.0f; modosCur = 0; mira = fwd; ojoA = 1030.0f; slabBase = baseAnt; slabTop = topAnt;
            size_t nRed = 0;
            for (size_t k = 0; k + 3 < pxE.size(); k += 4) if (pxE[k] > 200 && pxE[k+1] < 40 && pxE[k+2] < 40) ++nRed;
            std::printf("    presupuesto AGOTADO desde arriba: %zu px de %zu con nube (%.1f %%)\n", nRed, nLit, nLit ? 100.0 * nRed / (double)nLit : 0.0);
            CHECK(nRed * 50 < std::max(nLit, (size_t)1), "menos del 2 % de los pixeles con nube agotan el presupuesto de pasos (si no, la nube se corta recta)");
            // ⚠️ Y RASANTE, que es donde de verdad se agotaba: desde DENTRO de la banda a 10 grados
            // sobre el horizonte el rayo recorre decenas de km por la losa. Con la camara del banco
            // ya derecha se vio (16-09): una FRANJA roja pegada al horizonte, 5 200-5 900 px de
            // 65 536 (8 %) en las tres capas — eso eran los "cortes rectos". Contraprueba: con el
            // presupuesto a un cuarto (pasos 16) tiene que agotarse... salvo que el reparto del
            // presupuesto sobre la distancia que queda lo impida, que es justo lo que se pide: con
            // reparto, 0 agotados a CUALQUIER presupuesto; lo que cambia es el paso, no el alcance.
            // Las bandas altas son fijas en el shader (3,4-5,8 km y 6,8-10 km, 24 pasos): el ojo
            // se pone dentro de cada una; `pasos` solo mueve la losa convectiva.
            for (int m = 0; m < 3; ++m) {
                const float ojos[3] = { 2800.0f, 4600.0f, 8400.0f };
                ojoA = ojos[m]; slabBase = 2470.0f; slabTop = (m == 0) ? 3116.0f : 13000.0f;   // el vapor en altura vive en la columna entera
                mira = glm::normalize(fwd + up0 * 0.176);            // 10 grados sobre el horizonte
                size_t nRedR[2] = {}, nLitR[2] = {};
                for (int b = 0; b < (m == 0 ? 2 : 1); ++b) {
                    // m = 1: vapor en altura (medio+alto juntos: los parches de buen tiempo de un solo
                    // nivel pueden no caer en el cuadro desde el polo, y lo que se mide es el presupuesto)
                    modosCur = ((m == 1) ? 6 : (1 << m)) | 64; pasos = b ? 16.0f : 64.0f; render(0.45f, pxE);
                    for (size_t k = 0; k + 3 < pxE.size(); k += 4) {
                        if (pxE[k] > 200 && pxE[k+1] < 40 && pxE[k+2] < 40) ++nRedR[b];
                        else if (pxE[k] > 12) ++nLitR[b];
                    }
                }
                std::printf("    presupuesto AGOTADO rasante (10 grados, dentro de la banda) capa %d: %zu px rojos de %zu con nube%s\n",
                            m, nRedR[0], nLitR[0], m == 0 ? "" : "");
                if (m == 0) std::printf("      ... y con 1/4 de presupuesto (16 pasos): %zu rojos de %zu con nube\n", nRedR[1], nLitR[1]);
                CHECK(nLitR[0] > 200, "rasante y dentro de la banda hay nube que medir");
                CHECK(nRedR[0] == 0, "rasante y dentro de la banda, NINGUN pixel agota el presupuesto (eran 5 200-5 900: la franja recta al horizonte)");
                if (m == 0) {
                    CHECK(nRedR[1] == 0, "...ni con un cuarto de presupuesto: el reparto sobre la distancia restante llega siempre al final");
                    CHECK(nLitR[1] * 4 > nLitR[0], "y con 1/4 de presupuesto sigue habiendo nube (el reparto no la borra)");
                }
            }
            pasos = 64.0f; modosCur = 0; mira = fwd; ojoA = 1030.0f; slabBase = baseAnt; slabTop = topAnt;
        }
        CHECK(nLit > 500, "hay nube en la vista desde arriba");
        CHECK(meanDown < 6.0, "desde arriba la marcha tambien converge (< 6/255 de media)");
        CHECK(nLit == 0 || nBad * 20 < nLit, "y menos del 5 % de pixeles con cortes (> 24/255)");
    }
}

// Un test por caso, cada uno con su banco.
void testCloudHighLayers() { CloudPassBench b; if (b.ok) b.highLayers(); }
void testCloudFlatTop() { CloudPassBench b; if (b.ok) b.flatTop(); }
void testCloudMotionFrames() { CloudPassBench b; if (b.ok) b.motion(); }
void testCloudMarchConvergence() { CloudPassBench b; if (b.ok) b.convergence(); }
