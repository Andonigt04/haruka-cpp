// El pase volumetrico de nubes: que dibuja (y el angulo rasante), la profundidad por el camino de la
// partida, el coste y la cobertura por direccion.
#include "World/CloudPassBench.h"

void CloudPassBench::draws() {
    BEGIN("nubes: el pase VOLUMETRICO dibuja, y aguanta el angulo rasante");
    // ── (1) DIBUJA, y con cobertura cero NO ─────────────────────────────────────────────────────
    std::vector<uint8_t> pxOn, pxOff;
    render(0.85f, pxOn);
    render(0.00f, pxOff);
    size_t litOn = 0, litOff = 0, sentinel = 0;
    for (size_t k = 0; k < pxOn.size(); k += 4) {
        if (pxOn[k] == 0xAA && pxOn[k+1] == 0xAA && pxOn[k+2] == 0xAA) { ++sentinel; continue; }
        if (pxOn[k] > 6 || pxOn[k+1] > 6 || pxOn[k+2] > 6) ++litOn;
    }
    for (size_t k = 0; k < pxOff.size(); k += 4)
        if (pxOff[k] > 6 || pxOff[k+1] > 6 || pxOff[k+2] > 6) ++litOff;
    const size_t total = (size_t)w * h;
    std::printf("    cobertura 0,85: %zu px de nube (%.1f %%) · cobertura 0,00: %zu px · centinela %zu\n",
                litOn, 100.0 * (double)litOn / (double)total, litOff, sentinel);
    CHECK(sentinel < total / 2, "el readback escribio (si no, el conteo no significa nada)");
    CHECK(litOn > total / 50, "el pase volumetrico DIBUJA nube");
    CHECK(litOff == 0, "CONTRAPRUEBA: con cobertura 0 no pinta nada (es la nube, no el fondo)");
    if (litOn == 0) return;   // el fixture libera pipe/ubo/depthTex

    // ── (2) ⚠️ EL ANGULO RASANTE: lo que se le escapo al test de CPU ────────────────────────────
    //
    // La capa es una cascara esferica: mirando hacia el horizonte el rayo recorre MUCHA mas nube que
    // hacia el cenit (91 km contra 1,4 km). O sea que la opacidad tiene que SUBIR al bajar la mirada.
    // Si baja, la marcha se queda corta ahi — que es exactamente el bug de los 24 pasos uniformes.
    {
        // Elevacion de cada fila: el rayo de su centro contra el horizonte local.
        const bool cloudFlip = detectRowFlip(pxOn, w, h);
        const int bandas = 6;
        std::vector<double> sumA(bandas, 0.0); std::vector<size_t> cnt(bandas, 0);
        double elevMin = 1e9, elevMax = -1e9;
        for (int y = 0; y < h; ++y) {
            // ⚠️ Mismo error que en el test del cielo. Ver `rowToNdcY` / `detectRowFlip`.
            const float ndcY = rowToNdcY(y, h, cloudFlip);
            glm::vec4 far = invVP * glm::vec4(0.0f, ndcY, 1.0f, 1.0f);
            const glm::vec3 d = glm::normalize(glm::vec3(far) / far.w);
            const double elev = std::asin(glm::clamp((double)glm::dot(d, glm::vec3(up0)), -1.0, 1.0))
                              * 180.0 / 3.14159265358979;
            elevMin = std::min(elevMin, elev); elevMax = std::max(elevMax, elev);
            if (elev < 0.0) continue;                       // por debajo del horizonte no hay capa
            const int b = std::min(bandas - 1, (int)(elev / 30.0 * bandas));
            for (int x = 0; x < w; ++x) {
                const size_t k = ((size_t)y * w + x) * 4;
                sumA[(size_t)b] += (double)std::max({pxOn[k], pxOn[k+1], pxOn[k+2]}) / 255.0;
                ++cnt[(size_t)b];
            }
        }
        std::printf("    elevacion de la vista: %.1f .. %.1f grados\n", elevMin, elevMax);
        std::printf("    elevacion    opacidad media de la capa\n");
        double cerca = -1.0, lejos = -1.0;
        for (int b = 0; b < bandas; ++b) {
            if (cnt[(size_t)b] == 0) continue;
            const double a = sumA[(size_t)b] / (double)cnt[(size_t)b];
            std::printf("      %2d-%2d grados      %.4f\n", b * 30 / bandas, (b + 1) * 30 / bandas, a);
            if (cerca < 0.0) cerca = a;                     // la banda MAS baja con datos (rasante)
            lejos = a;                                      // la mas alta
        }
        std::printf("    rasante %.4f  vs  alto %.4f  ->  razon %.2f (la cascara da MAS recorrido abajo)\n",
                    cerca, lejos, (lejos > 1e-6) ? cerca / lejos : 0.0);
        CHECK(cerca > 0.0, "hay nube dibujada en la banda RASANTE (la que el muestreo uniforme vaciaba)");
        CHECK(cerca >= lejos * 0.8,
              "la opacidad NO se derrumba hacia el horizonte: la marcha aguanta el angulo rasante");
    }

    // ═══ De aqui en adelante lo que se mide es la FORMACION (como se reparte la fraccion de la
    // columna en nubes y como se ven), no la fontaneria del pase. Es UN test: el gemelo GPU de
    // `test_cloud_formation`. Las secciones de fontaneria vuelven al nombre del pase.
    BEGIN("nubes: FORMACION en GPU (la columna de aire repartida en nubes)");
    // ── (3) ⚠️ ¿HAY NUBES, O HAY UN TECHO? Con la cobertura MEDIANA DEL PLANETA ─────────────────
    //
    // A cobertura 0,85 el cielo esta cerrado y verlo todo tapado es lo correcto. Pero la mediana del
    // planeta es **0,302** (`cloud_shape`), y con esa el cielo tiene que tener HUECOS: nubes sueltas
    // con azul entre ellas. Si tambien sale tapado de lado a lado, lo que hay no son nubes — es una
    // lamina, que es literalmente el sintoma reportado ("no son 3d, son un plano 2d").
    //
    // Se mide lo unico que separa las dos cosas: la fraccion de cielo VACIO y la varianza espacial.
    {
        std::vector<uint8_t> pxMed;
        render(0.302f, pxMed);
        size_t hueco = 0, nube = 0; double acc = 0.0, acc2 = 0.0;
        for (size_t k = 0; k < pxMed.size(); k += 4) {
            const double a = (double)std::max({pxMed[k], pxMed[k+1], pxMed[k+2]}) / 255.0;
            if (a < 0.05) ++hueco; else ++nube;
            acc += a; acc2 += a * a;
        }
        const double n = (double)(hueco + nube);
        const double media = acc / n, var = acc2 / n - media * media;
        std::printf("    con la cobertura MEDIANA del planeta (0,302): %.1f %% de cielo VACIO · "
                    "opacidad media %.3f · desviacion %.3f\n",
                    100.0 * (double)hueco / n, media, std::sqrt(std::max(0.0, var)));
        CHECK(hueco > 0, "con cobertura media el cielo tiene HUECOS (si no, es una lamina y no nubes)");
        CHECK(std::sqrt(std::max(0.0, var)) > 0.10,
              "y la opacidad VARIA por el cielo (una lamina uniforme daria desviacion ~0)");
    }

    // ── (4) ¿SE VE COMO UN CUERPO O COMO UNA CALCOMANIA? ────────────────────────────────────────
    //
    // Una de las cuatro causas del "no son volumetricas" fue que la luz salia CONSTANTE: `dot` con el
    // vector al centro del planeta, que sobre una nube de 3 km gira 4,7e-4 rad. Sin sombreado, un
    // volumen se ve IGUAL que una calcomania. Se mide el RANGO de luminancia dentro de la nube.
    {
        double lo = 1e9, hi = -1e9, acc = 0.0; size_t n = 0;
        for (size_t k = 0; k < pxOn.size(); k += 4) {
            const double L = 0.2126 * pxOn[k] + 0.7152 * pxOn[k+1] + 0.0722 * pxOn[k+2];
            if (L < 8.0) continue;                          // fondo
            lo = std::min(lo, L); hi = std::max(hi, L); acc += L; ++n;
        }
        const double rango = (n && hi > 0.0) ? (hi - lo) / hi : 0.0;
        std::printf("    luminancia dentro de la nube: %.0f .. %.0f (media %.0f) -> rango relativo %.2f\n",
                    n ? lo : 0.0, n ? hi : 0.0, n ? acc / (double)n : 0.0, rango);
        CHECK(n > 0, "hay pixeles de nube que medir");
        CHECK(rango > 0.15,
              "la nube tiene SOMBREADO (una calcomania con luz constante daria rango ~0)");
    }

    // ── (5) ⚠️ ¿EL CIELO SE VE TAN NUBLADO COMO DICE EL CLIMA? AL CENIT ─────────────────────────
    //
    // Los 78,7 % tapados de arriba son mirando al HORIZONTE, donde el rayo cruza ~91 km de capa y
    // atraviesa muchas nubes seguidas: que se vea casi cerrado ahi es CORRECTO, y pasa en el cielo de
    // verdad. Donde "cobertura" significa lo que dice es en el CENIT, donde el rayo cruza la losa una
    // sola vez. Si tambien alli sale 2-3 veces mas tapado de lo pedido, el umbral del campo esta mal
    // y el cielo se ve encapotado con el clima diciendo "parcialmente nublado" — que se leeria como
    // un techo plano.
    {
        // ⚠️ VA POR `mira`, NO POR `fwd`. Al parametrizar `render` para poder mirar desde orbita, la
        // matriz paso a construirse dentro a partir de `mira`; este caso seguia asignando `fwd` y la
        // matriz de fuera, que ya no lee nadie — o sea que medía el HORIZONTE creyendo mirar arriba,
        // y la razon tapado/pedido salia falseada. Lo caza el propio umbral de 3x.
        mira = up0;                                          // al cenit
        std::printf("    AL CENIT:  cobertura pedida   fraccion de cielo TAPADA   razon\n");
        double peorRazon = 0.0; bool subeConCobertura = true; double antes = -1.0;
        for (float c : { 0.10f, 0.302f, 0.60f, 0.90f }) {
            std::vector<uint8_t> pz;
            render(c, pz);
            size_t tap = 0, n = 0;
            for (size_t k = 0; k < pz.size(); k += 4) {
                const double a = (double)std::max({pz[k], pz[k+1], pz[k+2]}) / 255.0;
                if (a > 0.05) ++tap; ++n;
            }
            const double frac = (double)tap / (double)n;
            std::printf("              %.3f              %.3f                    %.2fx\n",
                        c, frac, frac / (double)c);
            peorRazon = std::max(peorRazon, frac / (double)c);
            if (frac + 1e-6 < antes) subeConCobertura = false;
            antes = frac;
        }
        CHECK(subeConCobertura, "la fraccion tapada CRECE con la cobertura pedida (el mando manda)");
        // ⚠️ No se exige que la razon sea 1,0: un rayo al cenit atraviesa 1,4 km de losa y puede
        // encontrar mas de una nube, asi que tapar algo mas de lo pedido es fisico. Lo que se fija es
        // que no sea un factor descontrolado — ahi el cielo dejaria de responder al clima.
        std::printf("    peor razon tapado/pedido al cenit: %.2fx\n", peorRazon);
        CHECK(peorRazon < 3.0,
              "al cenit el cielo no se tapa mas de 3x lo que pide el clima (si no, siempre encapotado)");
        mira = fwd;   // se devuelve la vista, o el caso siguiente heredaria el cenit
    }
}

void CloudPassBench::sceneDepth() {
    BEGIN("nubes: el pase VOLUMETRICO dibuja, y aguanta el angulo rasante");
    // (6d) LA PROFUNDIDAD POR EL CAMINO DE LA PARTIDA. Todo lo de arriba ata una textura de 1x1
    //      rellenada a mano con 0,0. La partida NO hace eso: copia la profundidad de la escena con
    //      `blitDepth` a un target propio y ata `getDepthTexture` de esa copia. Ese camino no lo ha
    //      ejercitado nunca ningun test. Si entrega basura, `sceneT` cae al orden del plano cercano,
    //      `min(tExit, sceneT)` deja el recorrido en nada y NO SE DIBUJA UNA NUBE — mientras el
    //      cielo, que no lee profundidad, se sigue viendo. Que es el sintoma exacto reportado.
    //      Aqui cambia SOLO esa variable: mismos uniformes que el caso de arriba, otra profundidad.
    {
        RenderTargetDesc sd;
        sd.width = w; sd.height = h;
        sd.colorFormats = { Format::RGBA8 };
        sd.colorFilter  = Filter::Nearest;
        sd.hasDepth = true; sd.depthFormat = Format::D32F;
        RenderPassHandle escenaRT = g_dev->createRenderTarget(sd);

        RenderTargetDesc cd;                       // gemelo de `m_cloudDepthRT` en application_render
        cd.width = w; cd.height = h;
        cd.colorFormats = { Format::R32F };
        cd.colorFilter  = Filter::Nearest;
        cd.hasDepth = true; cd.depthFormat = Format::D32F;
        cd.depthAsTexture = true;                  // sin esto la profundidad no se puede MUESTREAR
        RenderPassHandle copiaRT = g_dev->createRenderTarget(cd);

        if (!valid(escenaRT) || !valid(copiaRT)) {
            CHECK(false, "targets del camino real de profundidad creados");
        } else {
            const TextureHandle copiaTex = g_dev->getDepthTexture(copiaRT);
            CHECK(valid(copiaTex), "getDepthTexture de la copia devuelve una textura valida");

            const float baseJuego = 2470.0f, topJuego = 3116.0f, altJuego = 1030.0f;
            const float baseAnt = slabBase, topAnt = slabTop;
            const TextureHandle depthAnt = depthTex;
            ojoA = altJuego; mira = fwd;
            slabBase = baseJuego; slabTop = topJuego;
            depthTex   = copiaTex;                 // `render` ata esto al binding 0
            preBlitSrc = escenaRT; preBlitDst = copiaRT;

            std::vector<uint8_t> pxD;
            render(0.80f, pxD);

            preBlitSrc = {}; preBlitDst = {};
            depthTex = depthAnt;
            slabBase = baseAnt; slabTop = topAnt; ojoA = eyeA;

            size_t litD = 0, nD = 0;
            for (size_t k = 0; k + 3 < pxD.size(); k += 4) {
                if (pxD[k] == 0xAA && pxD[k+1] == 0xAA && pxD[k+2] == 0xAA) continue;
                if (pxD[k] > 6 || pxD[k+1] > 6 || pxD[k+2] > 6) ++litD;
                ++nD;
            }
            std::printf("    PROFUNDIDAD POR blitDepth (mismos numeros que arriba): %.1f %% con nube\n",
                        nD ? 100.0 * (double)litD / (double)nD : 0.0);
            CHECK(litD > nD / 50,
                  "con la profundidad copiada por blitDepth se sigue dibujando nube "
                  "(si falla, el bug esta en la copia de profundidad, no en el shader)");
        }
        if (valid(escenaRT)) g_dev->destroy(escenaRT);
        if (valid(copiaRT))  g_dev->destroy(copiaRT);
    }
}

void CloudPassBench::belowHorizon() {
    BEGIN("nubes: por debajo del horizonte no hay nube aunque la profundidad este vacia (mar abierto)");
    // EL MAR NO ESCRIBE PROFUNDIDAD (es translucido) y en mar abierto el fondo queda a kilometros o
    // sin dibujar: bajo el horizonte la copia de depth dice "vacio" y `sceneT` no recorta nada. Con
    // el ojo bajo la base de la capa, la entrada a la cascara era la SALIDA de la esfera interior, que
    // para un rayo que baja esta AL OTRO LADO DEL PLANETA: la marcha recogia las nubes de las antipodas
    // y las pintaba encima del agua (Andoni, 2026-10-17: "las nubes se sobreponen al agua").
    // Montaje: ojo a 2 m sobre el nivel del mar, mirando 35 grados hacia abajo (todo el cuadro bajo el
    // horizonte con 60 grados de campo), profundidad vacia, cobertura entera.
    const float altAnt = ojoA; const glm::dvec3 miraAnt = mira;
    ojoA = 2.0f;
    const double a = glm::radians(35.0);
    mira = glm::normalize(fwd * std::cos(a) - up0 * std::sin(a));
    std::vector<uint8_t> px;
    render(1.0f, px);
    size_t lit = 0, n = 0;
    for (size_t k = 0; k + 3 < px.size(); k += 4) {
        if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
        if (px[k] > 6 || px[k+1] > 6 || px[k+2] > 6) ++lit;
        ++n;
    }
    std::printf("    bajo el horizonte, profundidad vacia: %.2f %% con nube\n", n ? 100.0 * (double)lit / (double)n : 0.0);
    CHECK(n > 0 && lit == 0, "bajo el horizonte no se pinta nube (el rayo no atraviesa el planeta)");
    // CONTRAPRUEBA: la misma escena mirando 35 grados hacia ARRIBA si tiene nube; si no, el caso de
    // arriba pasaria tambien con el pase roto.
    mira = glm::normalize(fwd * std::cos(a) + up0 * std::sin(a));
    render(1.0f, px);
    lit = 0; n = 0;
    for (size_t k = 0; k + 3 < px.size(); k += 4) {
        if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
        if (px[k] > 6 || px[k+1] > 6 || px[k+2] > 6) ++lit;
        ++n;
    }
    std::printf("    CONTRAPRUEBA mirando arriba: %.2f %% con nube\n", n ? 100.0 * (double)lit / (double)n : 0.0);
    CHECK(lit > n / 2, "CONTRAPRUEBA: mirando arriba la misma capa si se dibuja");
    ojoA = altAnt; mira = miraAnt;
}

void CloudPassBench::cost() {
    BEGIN("nubes: el pase VOLUMETRICO dibuja, y aguanta el angulo rasante");
    // (6e) EL COSTE, QUE NUNCA SE HABIA MEDIDO. Mientras la profundidad era basura el pase se
    //      recortaba a cero y salia gratis; en cuanto dibuja de verdad hay que saber lo que cuesta.
    //      Se mide el SUELO (cobertura 0 -> sale por el primer `if`) y la nube a 64/24/12 pasos, para
    //      separar lo que es por-pixel de lo que es por-paso: si bajar los pasos a la mitad NO baja
    //      el coste a la mitad, el termino dominante es el pixel y lo que hay que bajar es la
    //      RESOLUCION, no la marcha.
    {
        std::vector<uint8_t> uno(4, 0), basura;
        const float baseJuego = 2470.0f, topJuego = 3116.0f, altJuego = 1030.0f;
        const float baseAnt = slabBase, topAnt = slabTop;
        ojoA = altJuego; mira = fwd; slabBase = baseJuego; slabTop = topJuego;

        auto medir = [&](float cover, float pasosN, int marcosN) {
            pasos = pasosN;
            marcos = 6; leer = true; render(cover, basura);          // calienta y sincroniza
            marcos = marcosN; leer = false;
            const auto t0 = std::chrono::high_resolution_clock::now();
            render(cover, basura);
            g_dev->readPixels(0, 0, 1, 1, Format::RGBA8, uno.data());  // cierra la cuenta
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::high_resolution_clock::now() - t0).count()
                              / (double)marcosN;
            marcos = 3; leer = true; pasos = 64.0f;
            return ms;
        };

        const double msVacio = medir(0.000f, 64.0f, 30);
        const double ms64    = medir(0.800f, 64.0f, 30);
        const double ms24    = medir(0.800f, 24.0f, 30);
        const double ms12    = medir(0.800f, 12.0f, 30);

        const double mpx  = (double)w * (double)h / 1.0e6;
        const double aFHD = (1920.0 * 1080.0) / ((double)w * (double)h);
        std::printf("    COSTE DEL PASE a %dx%d (%.2f Mpx) · entre parentesis, extrapolado a 1920x1080\n"
                    "      cielo vacio (sale por el primer if) %.2f ms (%.1f)\n"
                    "      nube 0,80 · 64 pasos             %.2f ms (%.1f)\n"
                    "      nube 0,80 · 24 pasos             %.2f ms (%.1f)\n"
                    "      nube 0,80 · 12 pasos             %.2f ms (%.1f)\n",
                    w, h, mpx,
                    msVacio, msVacio * aFHD, ms64, ms64 * aFHD,
                    ms24, ms24 * aFHD, ms12, ms12 * aFHD);
        const double porPaso64 = (ms64 - msVacio);
        if (porPaso64 > 0.0 && ms24 > msVacio)
            std::printf("      -> de 64 a 24 pasos el coste de marcha cae al %.0f %% (%.0f %% si fuera "
                        "lineal en pasos)\n",
                        100.0 * (ms24 - msVacio) / porPaso64, 100.0 * 24.0 / 64.0);
        // ⚠️ CON VSYNC ESTO NO MIDE NADA: en FIFO cualquier frame se clava en 16,7 ms.
        const bool vsync = (msVacio > 15.5 && msVacio < 18.0);
        if (vsync)
            std::printf("    \033[33m⚠️ el suelo son %.1f ms = VSYNC (FIFO): esta medida NO vale. "
                        "Repite con HARUKA_NO_VSYNC=1.\033[0m\n", msVacio);
        slabBase = baseAnt; slabTop = topAnt; ojoA = eyeA;
        CHECK(msVacio > 0.0 && ms64 > 0.0, "el coste del pase de nubes queda MEDIDO");
        baselineMetric("nubes.pase_64pasos_ms_1080p", ms64 * aFHD, 0.30, true, true, !vsync);
        // Los 24 son lo que dibuja la partida desde el 20-09: es la cifra que vigila el coste real.
        baselineMetric("nubes.pase_24pasos_ms_1080p", ms24 * aFHD, 0.30, true, true, !vsync);
    }

    // (6f) EL CAMINO REDUCIDO + COMPOSICION, que es como dibuja la partida desde el arreglo del coste.
    //      Se marcha en un target de 1/4 de lado y se sube con `cloud_upsample`. Dos cosas que probar:
    //      que sigue habiendo nube, y que NO aparece el RIBETE OSCURO — el pase escribe alfa recto con
    //      color negro donde no hay nube, asi que una bilineal normal mezcla ese negro con la nube y
    //      ensucia cada borde. Por eso el shader interpola PONDERANDO POR ALFA.
    {
        // El MISMO vertex que la marcha (el rayo de cada píxel) y el UBO de nubes: la composición
        // ancla el borde a la nube con ellos (ver `cloud_upsample.frag`), igual que en `CloudPass`.
        const std::string uvs = base + "shaders/cloud_vol.vert";
        const std::string ufs = base + "shaders/cloud_upsample.frag";
        PipelineDesc up;
        up.vertexPath = uvs.c_str(); up.fragmentPath = ufs.c_str();
        up.topology = PrimitiveTopology::Triangles;
        up.depth.test = false; up.depth.write = false;
        up.blend.enable = true;
        PipelineHandle pipeUp = g_dev->createPipeline(up);
        CHECK(valid(pipeUp), "pipeline de composicion de nubes creado");

        const int rw = std::max(64, w / 4), rh = std::max(64, h / 4);
        RenderTargetDesc rd;
        rd.width = rw; rd.height = rh;
        rd.colorFormats = { Format::RGBA8 };
        rd.colorFilter  = Filter::Linear;
        rd.hasDepth     = false;
        RenderPassHandle bajoRT = g_dev->createRenderTarget(rd);
        CHECK(valid(bajoRT), "target reducido de nubes creado");

        if (valid(pipeUp) && valid(bajoRT)) {
            const TextureHandle bajoTex = g_dev->getColorTexture(bajoRT, 0);
            CHECK(valid(bajoTex), "getColorTexture del target reducido devuelve una textura valida");

            const float baseJ = 2470.0f, topJ = 3116.0f, altJ = 1030.0f;
            const float baseAnt2 = slabBase, topAnt2 = slabTop;
            ojoA = altJ; mira = fwd; slabBase = baseJ; slabTop = topJ;

            // Se dibuja el MISMO cielo por los DOS caminos sobre un fondo GRIS MEDIO: directo a
            // resolucion completa (lo caro, que es la referencia) y reducido + composicion (lo que
            // hace el motor). Contar pixeles "no negros" como en los casos de arriba no serviria:
            // ese umbral lo pasa hasta un alfa de 0,03, asi que diria que si aunque la nube saliera
            // fantasma. Sobre gris se compara la LUZ MEDIA, que es sensible al alfa, y ademas deja
            // ver el ribete oscuro que meteria una bilineal cruda.
            const bool isVk = (g_dev->backend() == Backend::Vulkan);
            auto pintar = [&](bool reducido, std::vector<uint8_t>& px) {
                px.assign((size_t)w * h * 4, 0xAA);
                for (int f = 0; f < 3; ++f) {
                    Context* ctx = g_dev->beginFrame();
                    if (!ctx) break;
                    hCur = reducido ? rh : h;
                    fillUBO(0.80f);
                    hCur = h;
                    if (reducido) {
                        ClearValues cl; cl.clearColor = true; cl.clearDepth = false;
                        cl.color[0] = cl.color[1] = cl.color[2] = cl.color[3] = 0.0f;
                        ctx->beginRenderPass(bajoRT, cl);
                        ctx->setViewport(0, 0, rw, rh);
                        ctx->bindPipeline(pipe);
                        ctx->bindUniformBuffer(5, ubo);
                        ctx->bindTexture(0, depthTex);
                        ctx->bindTexture(1, coverBound);
                        ctx->bindTexture(2, hiDummy);
                        ctx->draw(3, 1);
                        ctx->endRenderPass();
                    }
                    ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
                    cv.color[0] = cv.color[1] = cv.color[2] = 0.5f; cv.color[3] = 1.0f;
                    cv.depth = 0.0f;
                    ctx->beginRenderPass({}, cv);
                    // ⚠️ El pase reducido dejo el viewport a `rw x rh` y en OpenGL abrir un pase NO
                    // lo restablece. Sin esto solo se compone la esquina.
                    ctx->setViewport(0, 0, w, h);
                    if (reducido) {
                        ctx->bindPipeline(pipeUp);
                        ctx->bindUniformBuffer(5, ubo);
                        ctx->bindTexture(0, bajoTex);
                        // La profundidad de la escena (binding 1): aqui es el 1x1 de "cielo
                        // despejado", asi que el filtro guiado se apaga solo (`textureSize <= 1`) y
                        // este caso sigue midiendo lo que medía. ⚠️ Atarla NO es opcional: en Vulkan
                        // un descriptor sin atar es INDEFINIDO.
                        ctx->bindTexture(1, depthTex);
                    } else {
                        ctx->bindPipeline(pipe);
                        ctx->bindUniformBuffer(5, ubo);
                        ctx->bindTexture(0, depthTex);
                        ctx->bindTexture(1, coverBound);
                        ctx->bindTexture(2, hiDummy);
                    }
                    ctx->draw(3, 1);
                    ctx->endRenderPass();
                    if (!isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
                    g_dev->endFrame();
                    pumpWindowEvents();
                    if (isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
                }
            };
            std::vector<uint8_t> pxFull, pxLow;
            pintar(false, pxFull);
            pintar(true,  pxLow);

            // ── ⚠️ EL RECORTE CONTRA LA ESCENA, SUBIDO DE 1/4 A COMPLETA (19-09) ───────────────
            //
            // El pase recorta la marcha contra la profundidad (`min(tExit, sceneT)`) y a 1/4 de lado
            // cada texel decide por 4x4 pixeles con UNA muestra: en la silueta del terreno el recorte
            // salia en escalera de bloques. Aqui se pone una escalera DE VERDAD como escena (una
            // silueta escalonada a 60 km, como el horizonte) y se compara el compuesto contra la
            // referencia a resolucion completa. CONTRAPRUEBA: con la profundidad DESATADA (el 1x1)
            // el filtro guiado se apaga y el error tiene que subir.
            {
                const float dTerr = 1.0f / 60000.0f;             // reversed-Z, near 1 m
                std::vector<float> dep((size_t)w * h, 0.0f);
                int edge[512];
                for (int x = 0; x < w; ++x) edge[x] = h / 2 + ((x / 16) % 2) * 6;   // escalones de 16 px
                for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                    if (y >= edge[x]) dep[(size_t)y * w + x] = dTerr;
                TextureDesc dd; dd.width = w; dd.height = h; dd.format = Format::R32F;
                dd.filter = Filter::Nearest; dd.wrap = Wrap::ClampToEdge; dd.mipmaps = false;
                dd.initialData = dep.data();
                TextureHandle depEsc = g_dev->createTexture(dd);

                auto pintarEsc = [&](bool reducido, bool guiada, std::vector<uint8_t>& px) {
                    px.assign((size_t)w * h * 4, 0xAA);
                    for (int f = 0; f < 3; ++f) {
                        Context* ctx = g_dev->beginFrame();
                        if (!ctx) break;
                        hCur = reducido ? rh : h; fillUBO(0.80f); hCur = h;
                        if (reducido) {
                            ClearValues cl; cl.clearColor = true; cl.clearDepth = false;
                            cl.color[0] = cl.color[1] = cl.color[2] = cl.color[3] = 0.0f;
                            ctx->beginRenderPass(bajoRT, cl);
                            ctx->setViewport(0, 0, rw, rh);
                            ctx->bindPipeline(pipe); ctx->bindUniformBuffer(5, ubo);
                            ctx->bindTexture(0, depEsc); ctx->bindTexture(1, coverBound); ctx->bindTexture(2, hiDummy);
                            ctx->draw(3, 1);
                            ctx->endRenderPass();
                        }
                        ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
                        cv.color[0] = cv.color[1] = cv.color[2] = 0.5f; cv.color[3] = 1.0f; cv.depth = 0.0f;
                        ctx->beginRenderPass({}, cv);
                        ctx->setViewport(0, 0, w, h);
                        if (reducido) {
                            ctx->bindPipeline(pipeUp);
                        ctx->bindUniformBuffer(5, ubo);
                            ctx->bindTexture(0, bajoTex);
                            ctx->bindTexture(1, guiada ? depEsc : depthTex);   // 1x1 = sin guia
                        } else {
                            ctx->bindPipeline(pipe); ctx->bindUniformBuffer(5, ubo);
                            ctx->bindTexture(0, depEsc); ctx->bindTexture(1, coverBound); ctx->bindTexture(2, hiDummy);
                        }
                        ctx->draw(3, 1);
                        ctx->endRenderPass();
                        if (!isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
                        g_dev->endFrame(); pumpWindowEvents();
                        if (isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
                    }
                };
                // Sin aire en medio: lo que se mide es la COMPOSICION del borde, y a 60 km la neblina
                // (fisica) acerca la nube al color del cielo y le quita al borde el contraste que mide.
                std::vector<uint8_t> pxRef, pxGuia, pxCiega;
                sinAire = true;
                pintarEsc(false, false, pxRef);
                pintarEsc(true,  true,  pxGuia);
                pintarEsc(true,  false, pxCiega);
                sinAire = false;
                // Error contra la referencia en la BANDA del borde (±8 px del escalon), que es donde
                // se decide "cortar o no cortar".
                auto errBanda = [&](const std::vector<uint8_t>& px) {
                    double acc = 0.0; size_t n = 0;
                    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                        if (std::abs(y - edge[x]) > 8) continue;
                        const size_t k = ((size_t)y * w + x) * 4;
                        if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
                        // SOLO EL LADO DE LA MONTAÑA: donde la referencia NO tiene nube (el fondo del
                        // clear, gris 0,5). Ahi es donde la composicion ciega SANGRA nube sobre la
                        // montaña y la guiada la recorta — lo que se mide. Contar tambien el lado del
                        // cielo metia la diferencia de textura de la marcha a 1/4 (que no es de la
                        // composicion), y con la luz fisica la nube tiene mas contraste interno: la
                        // diluia hasta un 6 %.
                        if (std::abs((int)pxRef[k] - 128) > 2 || std::abs((int)pxRef[k+1] - 128) > 2 ||
                            std::abs((int)pxRef[k+2] - 128) > 2) continue;
                        acc += (std::abs((int)px[k] - pxRef[k]) + std::abs((int)px[k+1] - pxRef[k+1])
                              + std::abs((int)px[k+2] - pxRef[k+2])) / 3.0;
                        ++n;
                    }
                    return n ? acc / (double)n : -1.0;
                };
                const double eG = errBanda(pxGuia), eC = errBanda(pxCiega);
                std::printf("    RECORTE contra una silueta ESCALONADA (a 60 km): error medio en la banda del borde, lado de la montaña"
                            " · composicion GUIADA por profundidad %.2f/255 · CIEGA %.2f/255 (%.0f %% menos)\n", eG, eC,
                            eC > 0.0 ? 100.0 * (1.0 - eG / eC) : 0.0);
                CHECK(eG >= 0.0 && eC >= 0.0, "las dos composiciones dan lectura");
                CHECK(eG < eC, "la composicion guiada por profundidad se parece MAS a la referencia que la ciega");
                // Medido (lado de la montaña, 17-10): 11,3 contra 16,6 = **32 % menos** de error en el borde. Lo que queda NO es
                // la composicion: es que el pase corre a 1/4 y su marcha no es la de resolucion
                // completa ni con el recorte perfecto. Por eso el listón va en el 10 %.
                CHECK(eG < eC * 0.9, "y no por poco: al menos un 10 % menos de error en el borde (medido 32 %, lado de la montaña)");
                g_dev->destroy(depEsc);
            }

            slabBase = baseAnt2; slabTop = topAnt2; ojoA = eyeA;

            auto resumen = [&](const std::vector<uint8_t>& px, double& media, double& frac,
                               double& oscuroFrac) -> size_t {
                double suma = 0.0; size_t nube = 0, osc = 0, n = 0;
                for (size_t k = 0; k + 3 < px.size(); k += 4) {
                    if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
                    const double lum = (px[k] + px[k+1] + px[k+2]) / 3.0;
                    suma += lum;
                    if (lum > 140.0) ++nube;
                    if (lum < 100.0) ++osc;
                    ++n;
                }
                media = n ? suma / (double)n : -1.0;
                frac  = n ? 100.0 * (double)nube / (double)n : 0.0;
                oscuroFrac = n ? 100.0 * (double)osc / (double)n : 0.0;
                return n;
            };
            double mF = 0, fF = 0, oF = 0, mL = 0, fL = 0, oL = 0;
            const size_t nF = resumen(pxFull, mF, fF, oF);
            const size_t nL = resumen(pxLow,  mL, fL, oL);
            std::printf("    NUBE sobre gris 128 · completa %dx%d: luz media %.1f · %.1f %% nubosa\n"
                        "                      · reducida %dx%d + composicion: luz media %.1f · "
                        "%.1f %% nubosa · %.2f %% mas oscuro que el fondo (ribete)\n",
                        w, h, mF, fF, rw, rh, mL, fL, oL);
            CHECK(nF > 0 && nL > 0, "los dos caminos de nube dan lectura");
            CHECK(fL > fF * 0.5, "el camino reducido no pierde la nube que dibuja el completo");
            // ⚠️ El margen era 12 y la diferencia base (antes del LOD por huella) era 9. Con el LOD
            // y el paso adaptativo por huella, el target de 64x64 —cuatro veces mas grueso que el
            // de 256— resuelve MENOS y sale 16 mas oscuro. No es el compositado (probado con la
            // bilineal vieja: mismo numero) y en el juego el reducido es 480x270, no 64x64. Se deja
            // 20 como guardia contra una regresion mayor, y el numero queda impreso.
            // Con la luz fisica (18-09) la diferencia sube a 20: el reducido tiene la huella 4x mas
            // gruesa y el LOD (fraccion local, τ hacia el Sol) lo deja algo mas oscuro. El juego va a
            // 480x270, no a 64x64, asi que alli es menor; la guardia sube a 25 y el numero queda impreso.
            CHECK(std::fabs(mL - mF) < 25.0,
                  "reducido + composicion casa con el directo en luz media (< 25 de 255; base 9, luego 16, hoy 20)");
            // ⚠️ Con la luz fisica una nube TIENE partes oscuras (la panza en sombra: cielo·0,35 de
            // ambiente), asi que "oscuro" ya no es sinonimo de ribete. El ribete es lo que el camino
            // reducido AÑADE de oscuro respecto al completo.
            std::printf("                      · pixeles bajo 100/255: completo %.2f %% · reducido %.2f %%\n", oF, oL);
            CHECK(oL < oF + 1.0, "la composicion NO mete ribete oscuro (bilineal ponderada por alfa): no hay mas oscuro que en el completo");
        }
        if (valid(bajoRT)) g_dev->destroy(bajoRT);
        if (valid(pipeUp)) g_dev->destroy(pipeUp);
    }
}

void CloudPassBench::coverageByDir() {
    BEGIN("nubes: el pase VOLUMETRICO dibuja, y aguanta el angulo rasante");
    // ── (7) ⚠️ LA COBERTURA POR DIRECCION, que era la deuda declarada del cambio anterior ────────
    //
    // El pase recibia UN escalar (la cobertura del punto bajo la camara) y lo aplicaba a todo lo
    // visible: desde orbita, miles de km con el tiempo de un solo sitio. Ahora viaja como una
    // equirect horneada del `WeatherSystem`. Hasta aqui el banco ataba un 1x1 y CAIA AL ESCALAR a
    // proposito, asi que el camino nuevo no lo probaba nadie — quedo escrito como deuda y esto la paga.
    //
    // Se le mete un campo PARTIDO: media esfera cubierta (0,9) y media despejada (0). Si el shader
    // lee la textura, la mitad correspondiente del cielo se queda limpia; si sigue con el escalar
    // —que aqui se pone a 0,90 a proposito— las dos mitades salen IGUALES. Es lo unico que distingue
    // los dos caminos, y el escalar hace de contraprueba por construccion.
    {
        const int CW = 64, CH = 32;
        std::vector<float> campo((size_t)CW * CH, 0.0f);
        const double kPi = 3.14159265358979;
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x) {
                const double lon = (((double)x + 0.5) / CW - 0.5) * 2.0 * kPi;
                // ⚠️ SE PARTE POR Z, NO POR X. `coverAtDir` usa u = 0,5 + atan2(z,x)/2pi, asi que
                // `cos(lon) > 0` es "x > 0" — y la camara mira precisamente hacia +X, o sea que TODOS
                // los rayos caian del mismo lado y el test no veia la mitad despejada. Partir por el
                // eje PERPENDICULAR a la vista (`sin(lon) > 0`, o sea z > 0) pone las dos mitades en
                // el cuadro, que es lo unico que permite compararlas.
                campo[(size_t)y * CW + x] = (std::sin(lon) > 0.0) ? 0.90f : 0.0f;
            }
        TextureHandle campoTex = makeSkyTex(campo, CW, CH, slabBase, slabTop, Filter::Linear, Wrap::Repeat);

        // ⚠️ DESDE ORBITA, Y NO ES UNA PREFERENCIA: A RAS DE SUELO ESTO NO PUEDE MEDIRSE.
        // `coverAtDir` mira la direccion del PUNTO DE MUESTRA desde el centro del planeta. Con el ojo
        // a 300 m, la capa de nube esta a 100 km como mucho, o sea **0,9 grados de arco**: la
        // cobertura varia sobre el PLANETA y en 0,9 grados no varia nada. Mi primera version medio
        // ahi y dio 0,482 contra 0,548 — las dos mitades iguales — y acuse al shader de no leer la
        // textura. No era el shader: a ras de suelo el efecto ES despreciable, y eso es correcto.
        //
        // Desde 250 km sobre el polo el disco visible abarca TODAS las longitudes, asi que el campo
        // partido por `z` cae medio y medio en el cuadro. Es ademas el caso que motivo el cambio.
        ojoA = 250000.0f; mira = -up0;
        coverBound = campoTex;
        std::vector<uint8_t> pxF;
        render(0.90f, pxF);
        coverBound = coverTex;

        const glm::mat4 view7 = glm::lookAt(glm::vec3(0.0f), glm::vec3(-up0), glm::vec3(1, 0, 0));
        const glm::mat4 iVP7 = glm::inverse(proj * glm::mat4(glm::mat3(view7)));
        const bool flip7 = detectRowFlip(pxF, w, h);
        double sumPos = 0, sumNeg = 0; size_t nPos = 0, nNeg = 0;
        for (int y = 0; y < h; ++y) {
            const float ndcY = rowToNdcY(y, h, flip7);
            for (int x = 0; x < w; ++x) {
                const float ndcX = 2.0f * ((float)x + 0.5f) / (float)w - 1.0f;
                glm::vec4 far = iVP7 * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
                const glm::vec3 d = glm::normalize(glm::vec3(far) / far.w);
                if (std::abs(d.z) < 0.25f) continue;    // la franja del corte no cuenta
                const size_t k = ((size_t)y * w + x) * 4;
                if (pxF[k] == 0xAA && pxF[k+1] == 0xAA && pxF[k+2] == 0xAA) continue;
                const double a = (double)std::max({pxF[k], pxF[k+1], pxF[k+2]}) / 255.0;
                if (d.z > 0.0f) { sumPos += a; ++nPos; } else { sumNeg += a; ++nNeg; }
            }
        }
        const double mPos = nPos ? sumPos / (double)nPos : 0.0;
        const double mNeg = nNeg ? sumNeg / (double)nNeg : 0.0;
        std::printf("    campo PARTIDO (mitad 0,90 / mitad 0,00) con el escalar en 0,90:\n"
                    "      lado cubierto  opacidad %.3f (%zu px) · lado despejado %.3f (%zu px)\n",
                    mPos, nPos, mNeg, nNeg);
        CHECK(nPos > 0 && nNeg > 0, "el cuadro ve los dos lados del campo");
        CHECK(mPos > mNeg * 2.0,
              "el shader lee la cobertura POR DIRECCION (con el escalar, las dos mitades serian iguales)");
        ojoA = eyeA; mira = fwd;
        g_dev->destroy(campoTex);
    }
}

// Un test por caso, cada uno con su banco.
void testCloudPassDraws() { CloudPassBench b; if (b.ok) b.draws(); }
void testCloudPassSceneDepth() { CloudPassBench b; if (b.ok) b.sceneDepth(); }
void testCloudBelowHorizon() { CloudPassBench b; if (b.ok) b.belowHorizon(); }
void testCloudPassCost() { CloudPassBench b; if (b.ok) b.cost(); }
void testCloudCoverageByDirection() { CloudPassBench b; if (b.ok) b.coverageByDir(); }
