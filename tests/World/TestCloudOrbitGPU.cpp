// Las nubes desde ORBITA: la vista desde arriba, la configuracion exacta de la partida y la vista
// orbital con el clima de verdad.
#include "World/CloudPassBench.h"

void CloudPassBench::orbit() {
    BEGIN("nubes: el pase VOLUMETRICO dibuja, y aguanta el angulo rasante");
    // ── (6) ⚠️ DESDE ORBITA. Reportado mirando la pantalla: *"desde orbita deberian de verse"*, y no
    //        se veian. `ApplicationRender.cpp` calculaba `atmoC = 1 - smoothstep(0, radio*0.02,
    //        altitud)` y el shader hace `alpha * u_misc.x`: en la Tierra eso es CERO a partir de
    //        **127 km**, asi que a 249 km las nubes se multiplicaban por cero. Su comentario decia
    //        "en orbita no hay nube que atravesar (y el fondo ya se encarga)" — pero desde fuera no
    //        se atraviesan, se VEN, y el fondo pinta espacio, no nubes.
    //
    //        La geometria ya lo soportaba (`cloud_vol.frag` invierte el orden de entrada al mirar
    //        desde encima de la capa). Este caso lo fija para que no vuelva a apagarse en silencio.
    {
        ojoA = 250000.0f;          // 250 km: la altitud del reporte
        mira = -up0;               // mirando al planeta
        std::vector<uint8_t> pxOrb;
        render(0.60f, pxOrb);

        // ⚠️ Y AHORA CON EL PLANETA DELANTE, que es la diferencia entre este test y el juego. Hasta
        // aqui la profundidad de escena era un 1x1 a 0,0 (reversed-Z: "vacio, lejisimos"), o sea
        // CIELO DESPEJADO. En orbita el planeta llena la pantalla y el pase recorta la marcha contra
        // esa profundidad (`tExit = min(tExit, sceneT)`). Si ese recorte esta mal a escala orbital, en
        // el banco se ven nubes y en el juego no — que es exactamente lo reportado.
        //
        // Se pone la profundidad que escribiria el terreno a 249 km con la proyeccion reversed-Z
        // infinita del motor: `z_ndc = near/dist`, con near = 1 m.
        const float dSuelo = 1.0f / (float)(ojoA);          // el suelo, a `ojoA` metros
        TextureDesc sd; sd.width = 1; sd.height = 1; sd.format = Format::R32F;
        sd.filter = Filter::Nearest; sd.wrap = Wrap::ClampToEdge; sd.mipmaps = false;
        sd.initialData = &dSuelo;
        TextureHandle sueloTex = g_dev->createTexture(sd);
        TextureHandle vacio = depthTex;
        depthTex = sueloTex;                                 // `render` ata `depthTex` al binding 0
        std::vector<uint8_t> pxSuelo;
        render(0.60f, pxSuelo);
        depthTex = vacio;
        size_t litS = 0, nS = 0;
        for (size_t k = 0; k < pxSuelo.size(); k += 4) {
            if (pxSuelo[k] == 0xAA && pxSuelo[k+1] == 0xAA && pxSuelo[k+2] == 0xAA) continue;
            if (pxSuelo[k] > 6 || pxSuelo[k+1] > 6 || pxSuelo[k+2] > 6) ++litS;
            ++nS;
        }
        std::printf("    DESDE ORBITA con el PLANETA delante (profundidad de escena a %.0f km): "
                    "%zu px de nube de %zu (%.1f %%)\n",
                    ojoA / 1000.0f, litS, nS, nS ? 100.0 * (double)litS / (double)nS : 0.0);
        CHECK(litS > nS / 20,
              "las nubes sobreviven al recorte contra el planeta (si no, en el juego no se verian)");
        g_dev->destroy(sueloTex);
        size_t lit = 0, n = 0;
        for (size_t k = 0; k < pxOrb.size(); k += 4) {
            if (pxOrb[k] == 0xAA && pxOrb[k+1] == 0xAA && pxOrb[k+2] == 0xAA) continue;
            if (pxOrb[k] > 6 || pxOrb[k+1] > 6 || pxOrb[k+2] > 6) ++lit;
            ++n;
        }
        std::printf("    DESDE ORBITA (250 km, mirando al planeta): %zu px de nube de %zu (%.1f %%)\n",
                    lit, n, n ? 100.0 * (double)lit / (double)n : 0.0);
        CHECK(n > 0, "el readback de la vista orbital escribio");
        CHECK(lit > n / 20, "las nubes SE VEN desde orbita (el fade por altitud las borraba enteras)");

        // ── (6-T) ⚠️ Y CUANTO TAPAN. Contar pixeles "con nube" no distingue una capa rota de una
        //        sabana: con el 30 % de cobertura los dos dan 100 % de pixeles tocados. Andoni:
        //        "en orbita esta todo en blanco". La causa era la fraccion de cobertura aplicada POR
        //        PASO como si cada paso fuera una nube independiente: 0,7^10 = 0,03 de transmitancia
        //        a traves de una capa al 30 %. Ahora la fraccion se correlaciona por columna
        //        (`harukaCloseSeg`), y al nadir la transmitancia media tiene que quedar cerca de
        //        `1 − cobertura`. Sonda dbg 128: B = optica/8.
        {
            const int modosAntes = modosCur;
            modosCur = 7 | 128;
            auto transmit = [&](float cover) {
                std::vector<uint8_t> pxT;
                render(cover, pxT);
                double sumT = 0.0; size_t nT = 0;
                for (size_t k = 0; k < pxT.size(); k += 4) {
                    if (pxT[k] == 0xAA && pxT[k+1] == 0xAA && pxT[k+2] == 0xAA) continue;
                    const double optical = (double)pxT[k+2] / 255.0 * 8.0;
                    sumT += std::exp(-optical); ++nT;
                }
                return nT ? sumT / (double)nT : -1.0;
            };
            const double t30 = transmit(0.30f), t60 = transmit(0.60f), t90 = transmit(0.90f);
            // Y DESDE UN CINE (4 km, mirando 45 grados abajo al mar bajo una capa al 50 %): el rayo
            // cruza la capa en oblicuo, una o dos celdas -> T ~ (1 − c)^1..2. Antes, con 10 pasos
            // independientes, 0,5^10 = 0,001: "las nubes tapan el mar".
            ojoA = 4030.0f;
            mira = glm::normalize(-up0 * std::sin(glm::radians(45.0)) + fwd * std::cos(glm::radians(45.0)));
            const double tCine = transmit(0.50f);
            // CONTRAPRUEBA: el modelo viejo (dbg 1024: cada paso una nube independiente) en las dos vistas.
            modosCur = 7 | 128 | 1024;
            const double tCineOld = transmit(0.50f);
            ojoA = 250000.0f; mira = -up0;
            const double t30Old = transmit(0.30f), t60Old = transmit(0.60f);
            modosCur = modosAntes;
            std::printf("    DESDE 4 km a -45 grados, capa al 50 %%: transmitancia media %.3f (modelo viejo: %.3f)\n", tCine, tCineOld);
            // ⚠️ MEDIDO, Y NO ERA LO QUE YO HABIA DEDUCIDO. Con la fraccion por paso deduje "0,7^10
            // = 0,03, sabana blanca"; el modelo viejo da 0,936 con cobertura 0,3 (DEMASIADO transparente:
            // la fuerza de la nube lejana era la fraccion misma, floja) y 0,315 con 0,6 (demasiado
            // opaco). El de segmentos queda mas cerca de 1 − c en los dos, que es lo que se exige.
            std::printf("    modelo viejo desde orbita: cobertura 0,30 -> %.3f · 0,60 -> %.3f  (nuevo %.3f · %.3f; ideal 0,70 · 0,40)\n", t30Old, t60Old, t30, t60);
            CHECK(tCine > 0.15, "desde un cine sobre una capa al 50 % se ve el suelo/mar a traves (T > 0,15)");
            CHECK(std::abs(t30 - 0.70) < std::abs(t30Old - 0.70) && std::abs(t60 - 0.40) < std::abs(t60Old - 0.40),
                  "CONTRAPRUEBA: el modelo de segmentos queda mas cerca de 1 − cobertura que el de fraccion por paso, en 0,3 y en 0,6");

            // ── (6-H) ⚠️ EL HORIZONTE NO SE CIERRA CON POCA NUBE. Andoni: "las nubes tapan el
            //        horizonte cuando no deberia; igual con la cordillera". Desde el suelo, la banda
            //        de 0,5-3 grados sobre el horizonte cruza 30-50 celdas de la capa; si cada celda
            //        sortea nube con la cobertura MEDIA del texel, 0,8^32 = 0,001 y con el 20 % se
            //        cierra del todo. Con la fraccion LOCAL (donde el campo filtrado queda bajo el
            //        umbral no hay nube) quedan huecos. Se mide la fraccion de pixeles OPACOS
            //        (optica > 0,7) en esa banda, con cobertura 0,2 y 0,5, nuevo contra viejo.
            {
                auto opaqueBand = [&](float cover, int modos, double eLo, double eHi) {
                    modosCur = modos; ojoA = 2.0f; mira = fwd;
                    std::vector<uint8_t> pxH; render(cover, pxH);
                    size_t op = 0, n = 0;
                    const double tanHalf = std::tan(fovY * 0.5);
                    for (int y = 0; y < h; ++y) {
                        // fila -> elevacion (proyeccion lineal en tangente); el readback puede venir volteado,
                        // asi que se mira en las dos mitades por simetria: banda |e| en [eLo, eHi] grados
                        const double e = std::atan(((h * 0.5 - (y + 0.5)) / (h * 0.5)) * tanHalf) * 180.0 / 3.14159265358979;
                        if (std::abs(e) < eLo || std::abs(e) > eHi) continue;
                        for (int x = 0; x < w; ++x) {
                            const size_t k = ((size_t)y * w + x) * 4;
                            if (pxH[k] == 0xAA && pxH[k+1] == 0xAA && pxH[k+2] == 0xAA) continue;
                            if ((double)pxH[k+2] / 255.0 * 8.0 > 0.7) ++op;
                            ++n;
                        }
                    }
                    return n ? (double)op / (double)n : -1.0;
                };
                const double h20 = opaqueBand(0.20f, 7 | 128, 0.5, 3.0), h20Old = opaqueBand(0.20f, 7 | 128 | 1024, 0.5, 3.0);
                const double h50 = opaqueBand(0.50f, 7 | 128, 0.5, 3.0), h50Old = opaqueBand(0.50f, 7 | 128 | 1024, 0.5, 3.0);
                const double h20hi = opaqueBand(0.20f, 7 | 128, 5.0, 15.0);
                modosCur = modosAntes; ojoA = eyeA; mira = fwd;
                std::printf("    HORIZONTE desde 2 m, banda 0,5-3 grados: OPACO con cobertura 0,20 -> %.1f %% (modelo viejo %.1f %%) · 0,50 -> %.1f %% (viejo %.1f %%) · 0,20 a 5-15 grados %.1f %%\n",
                            100 * h20, 100 * h20Old, 100 * h50, 100 * h50Old, 100 * h20hi);
                // Medido (correlacion 3 celdas): 57 % con 0,20 (con 1 celda era 92 %) · 99,6 % con 0,50.
                // ⚠️ Con el 50 % la banda SE CIERRA y es fisica (una capa vista de canto: 10 sorteos a
                // 0,5 = 0,1 % de hueco), no un defecto: lo que no debe pasar es tapar lo que esta
                // DELANTE de la nube o por DEBAJO de su base, y eso lo mide (6-M).
                CHECK(h20 < 0.7, "con el 20 % de cobertura el horizonte NO se cierra (menos del 70 % opaco en 0,5-3 grados)");
                CHECK(h50 > h20, "y con mas cobertura se cierra mas");
                std::printf("    (el modelo viejo daba menos porque dibujaba la nube lejana floja y translucida, no porque dejara huecos)\n");
            }

            // ── (6-M) ⚠️ LA CORDILLERA DELANTE DE LA NUBE SE VE. "Pasa igual con la cordillera de los
            //        montes". Config de la partida (ojo 1 030 m, base 2 470 m, cobertura 0,50): un rayo a
            //        0,3-1,0 grados sube como mucho 1 330 m en 60 km (con la curvatura), o sea que a 60 km
            //        sigue BAJO la base. Con montes a 60 km (profundidad de escena) en esa banda no puede
            //        haber nube delante: el corte contra la escena tiene que dejarlos limpios.
            //        CONTRAPRUEBA: sin la profundidad, esa misma banda (por encima del horizonte) si
            //        lleva la pared de nube que entra a partir de ~70 km.
            {
                const float baseJ = 2470.0f, topJ = 3116.0f, altJ = 1030.0f;
                TextureHandle skyJ = makeSkyTex(std::vector<float>{0.5f}, 1, 1, baseJ, topJ, Filter::Nearest, Wrap::ClampToEdge);
                // Profundidad: montes a 60 km en las filas con |elevacion| en [0,3, 1,0] grados (simetrico
                // arriba y abajo para no depender de la orientacion del readback ni de la textura).
                std::vector<float> dep((size_t)w * h, 0.0f);
                const double tanHalf = std::tan(fovY * 0.5);
                auto elevOfRow = [&](int y) { return std::atan(((h * 0.5 - (y + 0.5)) / (h * 0.5)) * tanHalf) * 180.0 / 3.14159265358979; };
                for (int y = 0; y < h; ++y) {
                    const double e = std::abs(elevOfRow(y));
                    if (e >= 0.3 && e <= 1.0) for (int x = 0; x < w; ++x) dep[(size_t)y * w + x] = 1.0f / 60000.0f;   // reversed-Z, near 1 m
                }
                TextureDesc dd; dd.width = w; dd.height = h; dd.format = Format::R32F;
                dd.filter = Filter::Nearest; dd.wrap = Wrap::ClampToEdge; dd.mipmaps = false; dd.initialData = dep.data();
                TextureHandle depMontes = g_dev->createTexture(dd);
                auto opaqueMontes = [&](bool conMontes) {
                    const TextureHandle depAnt = depthTex; const TextureHandle covAnt = coverBound;
                    const float baseAnt = slabBase, topAnt = slabTop;
                    if (conMontes) depthTex = depMontes;
                    coverBound = skyJ; slabBase = baseJ; slabTop = topJ;
                    modosCur = 7 | 128; ojoA = altJ; mira = fwd;
                    std::vector<uint8_t> pxM; render(0.5f, pxM);
                    depthTex = depAnt; coverBound = covAnt; slabBase = baseAnt; slabTop = topAnt;
                    modosCur = modosAntes; ojoA = eyeA; mira = fwd;
                    size_t op = 0, n = 0;
                    for (int y = 0; y < h; ++y) {
                        const double e = std::abs(elevOfRow(y));
                        if (e < 0.3 || e > 1.0) continue;
                        for (int x = 0; x < w; ++x) {
                            const size_t k = ((size_t)y * w + x) * 4;
                            if (pxM[k] == 0xAA && pxM[k+1] == 0xAA && pxM[k+2] == 0xAA) continue;
                            if ((double)pxM[k+2] / 255.0 * 8.0 > 0.7) ++op;
                            ++n;
                        }
                    }
                    return n ? (double)op / (double)n : -1.0;
                };
                const double mCon = opaqueMontes(true), mSin = opaqueMontes(false);
                std::printf("    CORDILLERA a 60 km bajo la base (banda 0,3-1,0 grados, cobertura 0,50): nube opaca delante %.1f %% · sin la cordillera (solo cielo) %.1f %%\n",
                            100 * mCon, 100 * mSin);
                CHECK(mCon >= 0.0 && mCon < 0.05, "los montes que estan DELANTE de la nube (y bajo su base) se ven limpios (< 5 % de nube opaca encima)");
                CHECK(mSin > 0.25, "CONTRAPRUEBA: sin los montes, esa banda si lleva la pared de nube del horizonte (> 25 %)");
                g_dev->destroy(skyJ); g_dev->destroy(depMontes);
            }

            // ── (6-L) ⚠️ LA LUZ ES TRANSPORTE (2026-09-18). Andoni: "el visual tambien 100 %". La luz
            //        de la nube pasa de un promedio con colores a mano a la integral de dispersion
            //        (Beer-Lambert marchado hacia el Sol, fase HG de doble lobulo, octavas de dispersion
            //        multiple, ambiente del cielo). Dos consecuencias medibles y sus contrapruebas:
            //        (a) DIRECCION: desde encima, con el Sol de un lado, la luz de la nube se carga
            //            hacia ese lado (centroide de luminancia); con el Sol en el cenit, no.
            //        (b) FASE: mirando HACIA el Sol a traves de nube fina, mas radiancia (dispersion
            //            hacia delante) que mirando en contra.
            // ⚠️ A 64 Y A 24 PASOS (20-09). El juego baja a 24 por coste y Andoni: "no quiero que el
            //    sol se muestre diferente". Los pasos son el muestreo de la DENSIDAD a lo largo del
            //    rayo; la luz (marcha hacia el Sol, fase, octavas) se evalua por muestra y no depende
            //    de cuantas hay. Se mide igualmente: las dos cifras de la luz a cada presupuesto, y se
            //    exige que coincidan (Δr ≤ 0,05, Δfase ≤ 0,05). Si un dia el paso entra en la luz
            //    (p. ej. una sombra por pasos), esto lo dice.
            double corrAt[2] = {}, faseAt[2] = {};
            const float pasosLuz[2] = { 64.0f, 24.0f };
            for (int ip = 0; ip < 2; ++ip) {
                pasos = pasosLuz[ip];
                std::printf("    LUZ a %.0f pasos:\n", pasos);
                const glm::vec3 sunAnt = sunCur;
                auto shot = [&](const glm::vec3& sun, std::vector<uint8_t>& pxS) {
                    sunCur = sun; ojoA = 4030.0f;
                    mira = glm::normalize(-up0 * std::sin(glm::radians(60.0)) + fwd * std::cos(glm::radians(60.0)));
                    modosCur = 7;
                    render(0.5f, pxS);
                };
                auto lumOf = [&](const std::vector<uint8_t>& px, int x, int y) {
                    const size_t k = ((size_t)y * w + x) * 4;
                    if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) return -1.0;
                    return (px[k] + px[k+1] + px[k+2]) / 3.0;
                };
                const glm::dvec3 right = glm::normalize(glm::cross(glm::normalize(-up0 * std::sin(glm::radians(60.0)) + fwd * std::cos(glm::radians(60.0))), up0));
                std::vector<uint8_t> pxR, pxL, pxZ, pxO;
                shot(glm::normalize(glm::vec3(right * 0.8 + up0 * 0.45)), pxR);
                shot(glm::normalize(glm::vec3(-right * 0.8 + up0 * 0.45)), pxL);
                shot(glm::vec3(up0), pxZ);
                // Hacia donde "mira" cada trozo de nube: el lado en que la OPTICA cae (sonda dbg 128,
                // B = optica/8: la nube se acaba por ahi). Con el Sol de un lado, el cambio R−L de cada
                // pixel tiene que ir con ese lado: correlacion entre (L_R − L_L) y (opt(x−3) − opt(x+3)).
                modosCur = 7 | 128; render(0.5f, pxO); modosCur = 7;
                auto optOf = [&](int x, int y) {
                    const size_t k = ((size_t)y * w + x) * 4;
                    if (pxO[k] == 0xAA && pxO[k+1] == 0xAA && pxO[k+2] == 0xAA) return -1.0;
                    return (double)pxO[k+2] / 255.0 * 8.0;
                };
                double num = 0.0, dd = 0.0, ee = 0.0, numY = 0.0, eeY = 0.0;
                for (int y = 3; y < h - 3; ++y) for (int x = 3; x < w - 3; ++x) {
                    const double lr = lumOf(pxR, x, y), ll = lumOf(pxL, x, y), lz = lumOf(pxZ, x, y);
                    const double om = optOf(x - 3, y), op = optOf(x + 3, y);
                    const double oyM = optOf(x, y - 3), oyP = optOf(x, y + 3);
                    if (lr < 8.0 || ll < 8.0 || lz < 8.0 || om < 0.0 || op < 0.0 || oyM < 0.0 || oyP < 0.0) continue;
                    const double d = lr - ll, e = om - op, ey = oyM - oyP;
                    num += d * e; dd += d * d; ee += e * e; numY += d * ey; eeY += ey * ey;
                }
                const double corr  = (dd > 0.0 && ee  > 0.0) ? num  / std::sqrt(dd * ee)  : 0.0;
                const double corrY = (dd > 0.0 && eeY > 0.0) ? numY / std::sqrt(dd * eeY) : 0.0;
                {   // cuanto cambia la imagen al mover el Sol (si ~0, el Sol no llega a la marcha)
                    double aRL = 0.0, aRZ = 0.0, mR = 0.0; size_t n = 0;
                    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                        const double lr = lumOf(pxR, x, y), ll = lumOf(pxL, x, y), lz = lumOf(pxZ, x, y);
                        if (lr < 8.0 || ll < 8.0 || lz < 8.0) continue;
                        aRL += std::abs(lr - ll); aRZ += std::abs(lr - lz); mR += lr; ++n;
                    }
                    std::printf("    LUZ (a): |R−L| medio %.2f · |R−cenit| medio %.2f · luminancia media con el Sol a la derecha %.1f (%zu px)\n",
                                n ? aRL / n : 0.0, n ? aRZ / n : 0.0, n ? mR / n : 0.0, n);
                }
                std::printf("    LUZ (a): correlacion entre (Sol derecha − Sol izquierda) y el lado al que mira cada trozo de nube: %+.3f (con la orientacion VERTICAL, contraprueba: %+.3f)\n", corr, corrY);
                // El signo depende de la orientacion horizontal del readback; lo que tiene que haber es
                // correlacion con el lado (medido 0,22 a nivel de pixel, con toda la textura de la
                // nube como ruido) y NINGUNA con la orientacion vertical (el Sol se movio en horizontal).
                CHECK(std::abs(corr) > 0.12, "la cara que mira al Sol es la clara: el cambio al mover el Sol correlaciona con la orientacion horizontal de cada trozo (|r| > 0,12; medido 0,22)");
                CHECK(std::abs(corrY) < 0.5 * std::abs(corr), "CONTRAPRUEBA: con la orientacion vertical no correlaciona (el Sol se movio en horizontal)");
                sunCur = sunAnt;
                // (b) la fase: desde el suelo, 25 grados de elevacion, hacia el Sol (a 30 grados de la mira)
                // y en contra, con velo fino (cobertura 0,25).
                auto meanLum = [&](const glm::vec3& sun, const glm::dvec3& look) {
                    sunCur = sun; ojoA = 2.0f; mira = look; modosCur = 7;
                    std::vector<uint8_t> pxP; render(0.25f, pxP);
                    double sl = 0.0; size_t n = 0;
                    for (size_t k = 0; k + 3 < pxP.size(); k += 4) {
                        if (pxP[k] == 0xAA && pxP[k+1] == 0xAA && pxP[k+2] == 0xAA) continue;
                        const double l = (pxP[k] + pxP[k+1] + pxP[k+2]) / 3.0;
                        if (l < 8.0) continue;
                        // En RADIANCIA (lineal): la salida va codificada para pantalla (gamma 2,2), y una
                        // proporcion de valores de pantalla no es una proporcion de luz.
                        sl += std::pow(l / 255.0, 2.2); ++n;
                    }
                    return n ? sl / n : 0.0;
                };
                const glm::dvec3 look25 = glm::normalize(fwd * std::cos(glm::radians(25.0)) + up0 * std::sin(glm::radians(25.0)));
                const glm::vec3 sunFront = glm::normalize(glm::vec3(fwd * std::cos(glm::radians(55.0)) + up0 * std::sin(glm::radians(55.0))));
                const glm::vec3 sunBack  = glm::normalize(glm::vec3(-fwd * std::cos(glm::radians(55.0)) + up0 * std::sin(glm::radians(55.0))));
                const double lF = meanLum(sunFront, look25), lB = meanLum(sunBack, look25);
                std::printf("    LUZ (b): radiancia media (lineal) de la nube fina mirando HACIA el Sol %.4f · en CONTRA %.4f (x%.2f)\n", lF, lB, lB > 0 ? lF / lB : 0.0);
                CHECK(lF > lB * 1.10, "dispersion hacia delante: la nube fina es mas clara mirando hacia el Sol (x1,10 o mas, en radiancia)");
                sunCur = sunAnt; modosCur = modosAntes; ojoA = eyeA; mira = fwd;
                corrAt[ip] = std::abs(corr); faseAt[ip] = lB > 0 ? lF / lB : 0.0;
            }
            pasos = 64.0f;
            std::printf("    LUZ 64 vs 24 pasos: |r| %.3f vs %.3f (Δ %.3f) · fase x%.2f vs x%.2f (Δ %.2f)\n",
                        corrAt[0], corrAt[1], std::abs(corrAt[0] - corrAt[1]),
                        faseAt[0], faseAt[1], std::abs(faseAt[0] - faseAt[1]));
            CHECK(std::abs(corrAt[0] - corrAt[1]) <= 0.05, "el Sol se ve IGUAL a 24 pasos que a 64: la correlacion con la cara al Sol no cambia (Δ|r| ≤ 0,05)");
            CHECK(std::abs(faseAt[0] - faseAt[1]) <= 0.05, "...ni la fase (Δ ≤ 0,05): los pasos muestrean la densidad, no la luz");

            // ── (6-C) ⚠️ LA CAPA NO SE ACABA DE GOLPE. Andoni: "se recorta con cortes rectos y parece
            //        que estan mas cerca". La marcha se topaba a 4·L de perspectiva aerea (72 km con
            //        aire humedo), puesto cuando la extincion era plana (a 4L la nube quedaba al 1,8 %).
            //        Con el aire exponencial una nube alta a 4L se ve al 20-25 %: la capa TERMINABA en
            //        un borde recto a distancia fija. Se mide el perfil de optica por elevacion desde 2 m
            //        (aire humedo, L = 18 km): el mayor salto entre filas vecinas del cielo es el corte.
            {
                // ⚠️ CON LA BASE DE LA PARTIDA (2 470 m), no la del banco: un rayo a 1 grado no llega
                // a esa base hasta ~105 km, y ahi es donde un tope a 72 km la corta.
                const float aerialAnt = aerialLm, baseAnt = slabBase, topAnt = slabTop;
                TextureHandle skyC = makeSkyTex(std::vector<float>{0.5f}, 1, 1, 2470.0f, 3116.0f, Filter::Nearest, Wrap::ClampToEdge);
                const TextureHandle covAnt = coverBound; coverBound = skyC; slabBase = 2470.0f; slabTop = 3116.0f;
                aerialLm = 18000.0f;
                modosCur = 7 | 128; ojoA = 2.0f; mira = fwd;
                std::vector<uint8_t> pxC; render(0.5f, pxC);
                modosCur = modosAntes; aerialLm = aerialAnt; ojoA = eyeA;
                coverBound = covAnt; slabBase = baseAnt; slabTop = topAnt; g_dev->destroy(skyC);
                const double tanHalf = std::tan(fovY * 0.5);
                // perfil: optica media por fila, en la mitad del cuadro que tiene nube (arriba)
                std::vector<double> prof(h, 0.0);
                for (int y = 0; y < h; ++y) {
                    double acc = 0.0; int n = 0;
                    for (int x = 0; x < w; ++x) {
                        const size_t k = ((size_t)y * w + x) * 4;
                        if (pxC[k] == 0xAA && pxC[k+1] == 0xAA && pxC[k+2] == 0xAA) continue;
                        acc += (double)pxC[k+2] / 255.0 * 8.0; ++n;
                    }
                    prof[y] = n ? acc / n : 0.0;
                }
                // el salto mayor entre filas vecinas (dentro de |e| < 6 grados, donde vive la capa de canto)
                // ⚠️ EL HORIZONTE MISMO NO CUENTA: debajo está el PLANETA y la nube se acaba ahí, que es
                // un borde de verdad (a 2 m el horizonte cae 0,05°). Este caso medía hasta 2026-10-17 un
                // perfil suave a través del horizonte porque bajo él la marcha atravesaba el planeta y
                // recogía las nubes de las antípodas (el bug de "las nubes encima del mar"). Lo que se
                // vigila es un corte RECTO por encima del horizonte (el tope de la marcha), así que se
                // salta el par de filas que lo cruza. (El sentido de las filas cambia entre GL y Vulkan:
                // por eso se mira el signo de los dos lados, no "arriba" o "abajo".)
                double worstJump = 0.0; int worstRow = 0; double eWorst = 0.0;
                for (int y = 1; y < h; ++y) {
                    const double e = std::atan(((h * 0.5 - (y + 0.5)) / (h * 0.5)) * tanHalf) * 180.0 / 3.14159265358979;
                    const double ePrev = std::atan(((h * 0.5 - (y - 0.5)) / (h * 0.5)) * tanHalf) * 180.0 / 3.14159265358979;
                    if (std::abs(e) > 6.0) continue;
                    if (e * ePrev <= 0.0) continue;   // el par que cruza el horizonte
                    const double j = std::abs(prof[y] - prof[y - 1]);
                    if (j > worstJump) { worstJump = j; worstRow = y; eWorst = e; }
                }
                std::printf("    CORTE DE LA MARCHA (aire humedo, L 18 km, desde 2 m): mayor salto de optica entre filas vecinas %.3f (fila %d, %.2f grados; optica ahi %.2f -> %.2f)\n",
                            worstJump, worstRow, eWorst, prof[worstRow - 1], prof[worstRow]);
                std::printf("      perfil (grados: optica media):");
                for (int y = 0; y < h; y += 4) {
                    const double e = std::atan(((h * 0.5 - (y + 0.5)) / (h * 0.5)) * tanHalf) * 180.0 / 3.14159265358979;
                    if (std::abs(e) <= 8.0) std::printf(" %+.1f:%.2f", e, prof[y]);
                }
                std::printf("\n");
                // Medido sin ningun tope: 0,30 (ruido entre filas dentro de la pared, 3,2 -> 3,5). Un
                // corte recto es una caida de la optica a ~0 en una fila: > 1,0.
                CHECK(worstJump < 1.0, "la capa se funde con el aire: ningun salto de optica > 1 entre filas vecinas (un tope recto da un escalon a cero; medido 0,30 de ruido)");

                // ── ⚠️ A QUE DISTANCIA PIERDE LA NUBE SU FORMA (19-09) ──────────────────────
                // Andoni: "si bajas de cierta altura se crea un fantasma de una nube, un manchurron
                // como si estuviera lejos". Es el LOD: cuando la huella del pixel tapa la
                // FUNDAMENTAL del campo (1,4 km) la nube pierde el relieve y queda su cobertura. Al
                // bajar, la capa se ve mas rasante y su entrada se aleja, asi que cruza ese umbral.
                // Se mide el contraste (desviacion de luminancia) de la nube por banda de distancia
                // con la sonda de distancia media (dbg 512): si la forma sobrevive, el contraste no
                // se desploma.
                {
                    const int modosAnt2 = modosCur;
                    modosCur = 7; ojoA = 2.0f; mira = fwd;
                    std::vector<uint8_t> pxL; render(0.5f, pxL);
                    modosCur = 7 | 512;
                    std::vector<uint8_t> pxD; render(0.5f, pxD);
                    modosCur = modosAnt2; ojoA = eyeA;
                    // banda de distancia -> desviacion de luminancia de la nube
                    double s1[4] = {}, s2[4] = {}; size_t nB[4] = {};
                    for (size_t k = 0; k + 3 < pxL.size(); k += 4) {
                        if (pxD[k+2] < 100) continue;                       // sin nube opaca
                        const double dist = ((double)pxD[k] + (double)pxD[k+1] / 255.0) / 255.0 * 20000.0;
                        const int b = (dist < 20000.0) ? 0 : 1;             // la sonda satura en 20 km
                        (void)b;
                        const double l = (pxL[k] + pxL[k+1] + pxL[k+2]) / 3.0;
                        const int bb = (dist < 5000.0) ? 0 : (dist < 10000.0 ? 1 : (dist < 16000.0 ? 2 : 3));
                        s1[bb] += l; s2[bb] += l * l; ++nB[bb];
                    }
                    std::printf("    FORMA por distancia (contraste de la nube, sd de luminancia): ");
                    double sdFar = -1.0, sdNear = -1.0;
                    for (int b = 0; b < 4; ++b) {
                        if (!nB[b]) continue;
                        const double m = s1[b] / nB[b], sd = std::sqrt(std::max(s2[b] / nB[b] - m * m, 0.0));
                        std::printf("%s%.0f-%.0f km: %.1f (%zu px)", b ? " · " : "", b * 5.0, (b + 1) * 5.0, sd, nB[b]);
                        if (b == 0) sdNear = sd;
                        if (b == 3) sdFar = sd;
                    }
                    std::printf("\n");
                    CHECK(sdFar < 0.0 || sdNear < 0.0 || sdFar > 0.35 * sdNear,
                          "la nube LEJANA conserva forma: su contraste no cae por debajo del 35 % del de la cercana (el 'manchurron')");
                }
            }

            // ── (6-A) ⚠️ BAJANDO: a que altura se tapa el horizonte. Andoni: "sigue pasando; ocurre a
            //        partir de cierta altura". Barrido de altitud del ojo mirando al horizonte, con el
            //        SUELO ESFERICO como profundidad de escena (los rayos bajo el horizonte cortan en el
            //        suelo, como en el juego). Config de partida (base 2 470 m, techo 3 116, cobertura
            //        0,50). Por altura: nube opaca en la banda de CIELO justo sobre el horizonte
            //        geometrico (0-2 grados) y en la banda de SUELO justo debajo (0-2 grados bajo el).
            //        Lo que la fisica dice: por DEBAJO de la base, el suelo se ve limpio (el rayo que
            //        baja nunca entra en la capa) y el cielo se cierra solo a lo lejos; ENTRE la base y
            //        el techo el ojo esta dentro de la capa y el horizonte es pared; por ENCIMA, los rayos
            //        rasantes vuelven a cruzarla al bajar hacia el horizonte (curvatura).
            {
                const float baseJ = 2470.0f, topJ = 3116.0f;
                TextureHandle skyJ = makeSkyTex(std::vector<float>{0.5f}, 1, 1, baseJ, topJ, Filter::Nearest, Wrap::ClampToEdge);
                const double tanHalf = std::tan(fovY * 0.5);
                auto elevOfRow = [&](int y) { return std::atan(((h * 0.5 - (y + 0.5)) / (h * 0.5)) * tanHalf); };   // rad, fila del READBACK
                // 1) ¿que mitad del readback es ARRIBA? Sin profundidad, desde 2 m y con cobertura 1,0
                //    solo los rayos que SUBEN encuentran la capa.
                bool upIsLowRow = false;
                {
                    modosCur = 7 | 128; ojoA = 2.0f; mira = fwd;
                    std::vector<uint8_t> pxU; render(1.0f, pxU);
                    size_t opTop = 0, opBot = 0;
                    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                        const size_t k = ((size_t)y * w + x) * 4;
                        if ((double)pxU[k+2] / 255.0 * 8.0 > 0.7) { if (y < h / 2) ++opTop; else ++opBot; }
                    }
                    upIsLowRow = opTop > opBot;   // filas bajas del readback = arriba del cuadro
                }
                auto elevRow = [&](int y) { return upIsLowRow ? elevOfRow(y) : -elevOfRow(y); };
                // 2) ¿como se orienta la textura de profundidad respecto al readback? Sonda dbg 256 con
                //    el suelo solo en el primer cuarto de filas de la textura.
                bool depthRowsMatchReadback = true;
                {
                    std::vector<float> dq((size_t)w * h, 0.0f);
                    for (int y = 0; y < h / 4; ++y) for (int x = 0; x < w; ++x) dq[(size_t)y * w + x] = 1.0f / 10000.0f;
                    TextureDesc dd; dd.width = w; dd.height = h; dd.format = Format::R32F;
                    dd.filter = Filter::Nearest; dd.wrap = Wrap::ClampToEdge; dd.mipmaps = false; dd.initialData = dq.data();
                    TextureHandle dqTex = g_dev->createTexture(dd);
                    const TextureHandle depAnt = depthTex; depthTex = dqTex;
                    modosCur = 7 | 256; ojoA = 2.0f; mira = fwd;
                    std::vector<uint8_t> pxQ; render(1.0f, pxQ);
                    depthTex = depAnt; g_dev->destroy(dqTex);
                    size_t hitTop = 0, hitBot = 0;
                    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                        const size_t k = ((size_t)y * w + x) * 4;
                        const bool hit = !(pxQ[k] == 0 && pxQ[k+1] == 0 && pxQ[k+2] > 200);   // azul = sin escena
                        if (hit) { if (y < h / 4) ++hitTop; else ++hitBot; }
                    }
                    depthRowsMatchReadback = hitTop > hitBot;
                }
                std::printf("    (orientacion: arriba = filas %s del readback · profundidad %s con el readback)\n",
                            upIsLowRow ? "bajas" : "altas", depthRowsMatchReadback ? "alineada" : "INVERTIDA");
                // 3) El barrido.
                const double alts[] = { 2.0, 300.0, 1000.0, 2000.0, 2470.0, 2800.0, 3116.0, 4000.0, 6000.0, 10000.0 };
                double skyAt[10] = {}, gndAt[10] = {};
                std::printf("    altura del ojo   horizonte a   NUBE OPACA en el cielo 0-2 gr sobre el horizonte   en el suelo 0-2 gr bajo el\n");
                for (int ai = 0; ai < 10; ++ai) {
                    const double hEye = alts[ai];
                    const double eHor = -std::acos(R / (R + hEye));   // depresion del horizonte geometrico (rad)
                    // profundidad: el suelo esferico para los rayos bajo el horizonte
                    std::vector<float> dep((size_t)w * h, 0.0f);
                    for (int y = 0; y < h; ++y) {
                        const int ty = depthRowsMatchReadback ? y : (h - 1 - y);
                        const double e = elevRow(y);
                        if (e >= eHor) continue;
                        const double o = R + hEye, b = o * std::sin(e), c = o * o - R * R;
                        const double disc = b * b - c;
                        if (disc < 0.0) continue;
                        const double t = -b - std::sqrt(disc);
                        if (t <= 1.0) continue;
                        for (int x = 0; x < w; ++x) dep[(size_t)ty * w + x] = (float)(1.0 / t);
                    }
                    TextureDesc dd; dd.width = w; dd.height = h; dd.format = Format::R32F;
                    dd.filter = Filter::Nearest; dd.wrap = Wrap::ClampToEdge; dd.mipmaps = false; dd.initialData = dep.data();
                    TextureHandle depTex = g_dev->createTexture(dd);
                    const TextureHandle depAnt = depthTex, covAnt = coverBound;
                    const float baseAnt = slabBase, topAnt = slabTop;
                    depthTex = depTex; coverBound = skyJ; slabBase = baseJ; slabTop = topJ;
                    modosCur = 7 | 128; ojoA = (float)hEye; mira = fwd;
                    std::vector<uint8_t> pxA; render(0.5f, pxA);
                    depthTex = depAnt; coverBound = covAnt; slabBase = baseAnt; slabTop = topAnt;
                    g_dev->destroy(depTex);
                    size_t opS = 0, nS = 0, opG = 0, nG = 0;
                    const double two = 2.0 * 3.14159265358979 / 180.0;
                    for (int y = 0; y < h; ++y) {
                        const double e = elevRow(y);
                        const bool sky = (e >= eHor && e < eHor + two), gnd = (e < eHor && e >= eHor - two);
                        if (!sky && !gnd) continue;
                        for (int x = 0; x < w; ++x) {
                            const size_t k = ((size_t)y * w + x) * 4;
                            const bool op = (double)pxA[k+2] / 255.0 * 8.0 > 0.7;
                            if (sky) { ++nS; if (op) ++opS; } else { ++nG; if (op) ++opG; }
                        }
                    }
                    skyAt[ai] = nS ? (double)opS / nS : -1.0; gndAt[ai] = nG ? (double)opG / nG : -1.0;
                    std::printf("    %8.0f m       %5.1f km          %5.1f %%                                        %5.1f %%%s\n",
                                hEye, std::sqrt(2.0 * R * hEye + hEye * hEye) / 1000.0, 100.0 * skyAt[ai], 100.0 * gndAt[ai],
                                (hEye >= baseJ && hEye <= topJ) ? "   <- dentro de la capa" : "");
                }
                modosCur = modosAntes; ojoA = eyeA; mira = fwd;
                g_dev->destroy(skyJ);
                // Lo que se afirma: por debajo de la base el SUELO bajo el horizonte esta limpio (el
                // rayo que baja no entra en la capa); a 2 m el cielo sobre el horizonte no esta cerrado
                // del todo (la pared empieza a 166 km, mas alla del alcance de la perspectiva aerea);
                // dentro de la capa el horizonte es pared (fisica).
                CHECK(gndAt[0] < 0.05 && gndAt[1] < 0.05 && gndAt[2] < 0.05, "por debajo de la base, el suelo bajo el horizonte se ve limpio a 2, 300 y 1000 m");
                CHECK(skyAt[4] > 0.9 || skyAt[5] > 0.9, "dentro de la capa el horizonte es pared (contraprueba del instrumento)");
            }
            std::printf("    DESDE ORBITA, al nadir, TRANSMITANCIA media: cobertura 0,30 -> %.3f · 0,60 -> %.3f · 0,90 -> %.3f  (una capa rota deja pasar ~1 − cobertura)\n",
                        t30, t60, t90);
            CHECK(t30 > 0.45 && t30 < 0.80, "con el 30 % de cobertura se ve el planeta a traves (T entre 0,45 y 0,80; antes ~0)");
            CHECK(t60 > 0.20 && t60 < 0.55, "con el 60 %, T entre 0,20 y 0,55");
            CHECK(t30 > t60 + 0.1 && t60 > t90 + 0.05, "y la transmitancia BAJA con la cobertura (contraprueba del instrumento)");
        }
        ojoA = eyeA; mira = fwd;
    }

    // ── (6b) ⚠️ LA CONFIGURACION EXACTA DE LA PARTIDA. El banco decia que el pase dibuja y la
    //        pantalla decia que no hay nubes; con `HARUKA_CLOUD_COVER=0.80` (que salta el campo del
    //        clima) SEGUIA sin verse, o sea que el fallo no es el campo. La unica diferencia que
    //        quedaba entre banco y juego eran los NUMEROS: aqui se usaban ojo a 300 m y losa
    //        1200-2600 m, y la partida corre con ojo a 1030 m y losa **2470-3116 m** (646 m de
    //        espesor, 1440 m por encima del ojo). Se reproduce tal cual.
    {
        const float baseJuego = 2470.0f, topJuego = 3116.0f, altJuego = 1030.0f;
        ojoA = altJuego; mira = fwd;                 // mirando al horizonte, como se juega
        const float baseAnt = slabBase, topAnt = slabTop;
        slabBase = baseJuego; slabTop = topJuego;
        std::vector<uint8_t> pxJ;
        render(0.80f, pxJ);
        size_t litJ = 0, nJ = 0;
        for (size_t k = 0; k + 3 < pxJ.size(); k += 4) {
            if (pxJ[k] == 0xAA && pxJ[k+1] == 0xAA && pxJ[k+2] == 0xAA) continue;
            if (pxJ[k] > 6 || pxJ[k+1] > 6 || pxJ[k+2] > 6) ++litJ;
            ++nJ;
        }
        std::printf("    CONFIG DE PARTIDA (ojo %.0f m · losa %.0f-%.0f m · cobertura 0,80): "
                    "%.1f %% del cuadro con nube\n",
                    altJuego, baseJuego, topJuego, nJ ? 100.0 * (double)litJ / (double)nJ : 0.0);
        slabBase = baseAnt; slabTop = topAnt; ojoA = eyeA;
        CHECK(litJ > nJ / 50,
              "con los numeros EXACTOS de la partida se dibuja nube (si falla, el banco reproduce el bug)");
    }
}

void CloudPassBench::orbitView() {
    BEGIN("nubes: el pase VOLUMETRICO dibuja, y aguanta el angulo rasante");
    // ── (8) LA VISTA ORBITAL CON EL CLIMA DE VERDAD, para poder MIRARLA ─────────────────────────
    //
    // `HARUKA_CLOUD_PNG=<dir>` vuelca el planeta visto desde 250 km con el campo de cobertura
    // horneado del `WeatherSystem` REAL — el mismo `cloudCoverAt` que usa el motor. Sirve para lo que
    // ningun numero contesta: "¿esto parece un planeta desde arriba, o son motas?".
    {
        const char* dir = std::getenv("HARUKA_CLOUD_PNG");
        const int CW = 128, CH = 64;
        std::vector<float> cov((size_t)CW * CH, 0.0f);
        // ⚠️ CONFIGURADO. La primera version lo dejaba recien construido, y `cloudCoverAt` empieza con
        // `if (!m_configured) return 0.0f` — el campo salia TODO A CERO y la vista orbital, negra. Lo
        // descarte como "fallo del test"... y resulta que ese negro es exactamente el sintoma que
        // Andoni reporta en partida ("se ve la atmosfera pero no las nubes"). El mecanismo es real y
        // hay que cubrirlo: con el campo a cero el pase SIGUE corriendo —la puerta usa el maximo, que
        // incluye la cobertura bajo la camara— pero cada muestra lee 0, el umbral se va a 0,58 y no
        // dibuja nada. Pase vivo, cielo vacio.
        Haruka::World::WeatherSystem wsys;
        wsys.configure(1234u);
        const double kPi2 = 3.14159265358979;
        float mn = 1.0f, mx = 0.0f; double acc = 0.0;
        for (int y = 0; y < CH; ++y) {
            const double lat = (0.5 - ((double)y + 0.5) / CH) * kPi2;
            for (int x = 0; x < CW; ++x) {
                const double lon = (((double)x + 0.5) / CW - 0.5) * 2.0 * kPi2;
                const glm::dvec3 d(std::cos(lat) * std::cos(lon), std::sin(lat),
                                   std::cos(lat) * std::sin(lon));
                const float c = wsys.cloudCoverAt(d, 0.5f);
                cov[(size_t)y * CW + x] = c;
                mn = std::min(mn, c); mx = std::max(mx, c); acc += c;
            }
        }
        std::printf("    campo de cobertura del CLIMA REAL: min %.3f · medio %.3f · max %.3f\n",
                    mn, acc / (double)(CW * CH), mx);
        TextureHandle wTex = makeSkyTex(cov, CW, CH, slabBase, slabTop, Filter::Linear, Wrap::Repeat);

        ojoA = 250000.0f; mira = -up0;
        coverBound = wTex;
        std::vector<uint8_t> pxO;
        render(mx, pxO);                       // el escalar = el maximo, como hace el motor
        coverBound = coverTex; ojoA = eyeA; mira = fwd;

        // El volcado a disco es OPCIONAL (para poder mirarlo); la AFIRMACION de abajo corre siempre.
        if (dir) {
            const bool flip8 = detectRowFlip(pxO, w, h);
            std::vector<uint8_t> img(pxO.size());
            for (int y = 0; y < h; ++y) {
                const int src = flip8 ? (h - 1 - y) : y;
                std::memcpy(&img[(size_t)y * w * 4], &pxO[(size_t)src * w * 4], (size_t)w * 4);
            }
            const std::string path = std::string(dir) + "/"
                                   + ((g_dev->backend() == Backend::Vulkan) ? "vk_" : "gl_")
                                   + "nubes_orbita.png";
            Haruka::IO::writePNG(path, w, h, 4, img.data());
            std::printf("      -> %s\n", path.c_str());
        }
        size_t litO = 0, nO = 0;
        for (size_t k = 0; k + 3 < pxO.size(); k += 4) {
            if (pxO[k] == 0xAA && pxO[k+1] == 0xAA && pxO[k+2] == 0xAA) continue;
            if (pxO[k] > 6 || pxO[k+1] > 6 || pxO[k+2] > 6) ++litO;
            ++nO;
        }
        std::printf("    con el campo del CLIMA REAL, desde orbita: %.1f %% del cuadro con nube\n",
                    nO ? 100.0 * (double)litO / (double)nO : 0.0);
        CHECK(litO > nO / 20,
              "con el campo de cobertura REAL se dibujan nubes desde orbita (el campo a cero las apaga "
              "sin apagar el pase)");
        // ⚠️ Y CUANTO TAPA el campo real desde orbita (Andoni: "en orbita esta todo en blanco"). La
        // transmitancia media al nadir tiene que quedar cerca de 1 − cobertura media; sonda dbg 128,
        // y el modelo viejo (dbg 1024) como contraprueba.
        {
            const int modosAntes = modosCur;
            auto transmitO = [&](int modos) {
                modosCur = modos; ojoA = 250000.0f; mira = -up0; coverBound = wTex;
                std::vector<uint8_t> pxT; render(mx, pxT);
                coverBound = coverTex; ojoA = eyeA; mira = fwd;
                double sumT = 0.0; size_t nT = 0;
                for (size_t k = 0; k + 3 < pxT.size(); k += 4) {
                    if (pxT[k] == 0xAA && pxT[k+1] == 0xAA && pxT[k+2] == 0xAA) continue;
                    sumT += std::exp(-(double)pxT[k+2] / 255.0 * 8.0); ++nT;
                }
                return nT ? sumT / (double)nT : -1.0;
            };
            const double tNew = transmitO(7 | 128), tOld = transmitO(7 | 128 | 1024);
            modosCur = modosAntes;
            std::printf("    con el campo del CLIMA REAL, desde orbita: transmitancia media %.3f (1 − cobertura media = %.3f) · modelo viejo %.3f\n",
                        tNew, 1.0 - acc / (double)(CW * CH), tOld);
            CHECK(tNew > 0.6 * (1.0 - acc / (double)(CW * CH)), "desde orbita se ve el planeta a traves del campo real (T >= 0,6·(1 − cobertura media))");
        }
        g_dev->destroy(wTex);
    }
}

// Un test por caso, cada uno con su banco.
void testCloudPassOrbit() { CloudPassBench b; if (b.ok) b.orbit(); }
void testCloudOrbitView() { CloudPassBench b; if (b.ok) b.orbitView(); }
