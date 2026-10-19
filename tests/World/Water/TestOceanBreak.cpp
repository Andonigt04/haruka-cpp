// El mar en la costa: bajio, rompiente y pliegue, espuma y borreguillos, profundidad finita, refraccion, resaca y marea.
// (Partido de TestOcean.cpp; el registro de todos sigue allí, en el orden de siempre.)
#include "World/Water/OceanTestCommon.h"

// ----------------------------------------------- TEST 3: bajío y rompiente (la costa, en números)
void test_ocean_shoaling() {
    beginTest("ocean_shoaling");

    const float ampDeep = Haruka::World::Water::OCEAN_WAVE[0][1];   // 0.85 m, el swell que da la silueta

    // Ley de Green: al subir el fondo la ola CRECE. 50 m -> 3 m debe engordarla claramente.
    const float aDeep  = oceanShoalAmp(50.0f, ampDeep);
    const float aShelf = oceanShoalAmp(10.0f, ampDeep);
    const float a3     = oceanShoalAmp(3.0f,  ampDeep);
    std::printf("    amplitud del swell de %.2f m por profundidad:\n", ampDeep);
    std::printf("      50 m -> %.3f m · 10 m -> %.3f m · 3 m -> %.3f m\n", aDeep, aShelf, a3);
    CHECK(aShelf > aDeep, "la ola CRECE al entrar en agua somera (ley de Green)");

    // Límite de rompiente: pase lo que pase, la amplitud nunca supera 0.55·profundidad. Es lo que
    // impide que la orilla sea un pico absurdo — pasado el límite la ola no crece, ROMPE.
    bool capOk = true;
    float worstRatio = 0.0f;
    for (float d = 0.05f; d < 60.0f; d += 0.05f) {
        const float a = oceanShoalAmp(d, ampDeep);
        worstRatio = std::max(worstRatio, a / d);
        if (a > 0.55f * d + 1e-5f) capOk = false;
    }
    std::printf("      razon amplitud/profundidad peor: %.3f (tope de rompiente 0.550)\n", worstRatio);
    CHECK(capOk, "la amplitud nunca supera 0.55x la profundidad (limite de rompiente)");

    // Y LO QUE HACE QUE LA COSTA FUNCIONE: con el fondo a cero la ola se anula, así que la superficie
    // del agua se junta con el suelo EXACTAMENTE en la línea de costa. Sin esto la orilla sería una
    // frontera entre dos superficies que hay que mantener de acuerdo.
    std::printf("      en la orilla (d=0.01 m): amplitud %.4f m\n", oceanShoalAmp(0.01f, ampDeep));
    CHECK(oceanShoalAmp(0.01f, ampDeep) < 0.01f, "la ola se anula en la linea de costa");

    // CONTRAPRUEBA: sin el tope, la ley de Green sola daría una amplitud ABSURDA en 10 cm de agua.
    // Es la cifra que justifica que el tope exista.
    const float greenOnly = ampDeep * std::pow(std::min(50.0f / 0.1f, 40.0f), 0.25f);
    std::printf("      CONTRAPRUEBA sin tope, en 0.1 m de agua: %.3f m (%.0fx la profundidad)\n",
                greenOnly, greenOnly / 0.1f);
    CHECK(greenOnly > 1.0f, "CONTRAPRUEBA: sin el tope la ola en la orilla seria absurda");
}

// -------------------------------------------------- TEST 7: la marea tiene DOS abultamientos (P2)
void test_ocean_tide() {
    beginTest("ocean_tide");
    using Haruka::World::Water::oceanTideAt;

    const double R = 6371000.0;
    // Una luna en +X, a distancia lunar y con el GM de la Luna.
    const glm::dvec3 moon(3.844e8, 0, 0);
    const double gm = 4.9048695e12;

    const double under = Haruka::World::Water::oceanTideAt(glm::dvec3( 1, 0, 0), R, &moon, &gm, 1);  // bajo la luna
    const double anti  = Haruka::World::Water::oceanTideAt(glm::dvec3(-1, 0, 0), R, &moon, &gm, 1);  // antipodas
    const double side  = Haruka::World::Water::oceanTideAt(glm::dvec3( 0, 1, 0), R, &moon, &gm, 1);  // cuadratura
    std::printf("    marea de una luna: bajo ella %+.3f m · antipodas %+.3f m · a 90 grados %+.3f m\n",
                under, anti, side);

    // LAS DOS PLEAMARES. El término P₂ = (3cos²θ − 1)/2 vale +1 en θ=0 Y en θ=180: por eso hay dos
    // mareas altas al día y no una. Que las antípodas suban es la prueba de que esto es el potencial
    // de marea y no "atracción hacia la luna", que daría un solo abultamiento.
    CHECK(under > 0.0, "hay pleamar BAJO la luna");
    CHECK(anti  > 0.0, "y tambien en las ANTIPODAS (la segunda pleamar del dia)");
    CHECK(side  < 0.0, "y bajamar a 90 grados");
    // Los dos abultamientos son casi iguales: la pequeña asimetría es el término de distancia (1/D³
    // evaluado en la superficie, no en el centro), que es real.
    std::printf("    asimetria entre los dos abultamientos: %.2f %%\n",
                100.0 * std::abs(under - anti) / std::max(under, 1e-9));
    CHECK(std::abs(under - anti) / std::max(under, 1e-9) < 0.15, "los dos abultamientos son casi iguales");

    // CONTRAPRUEBA: sin cuerpos no hay marea. Si devolviera algo, el mar tendría un nivel inventado.
    const double none = Haruka::World::Water::oceanTideAt(glm::dvec3(1, 0, 0), R, nullptr, nullptr, 0);
    std::printf("    CONTRAPRUEBA: sin cuerpos masivos la marea es %.3f m\n", none);
    CHECK(none == 0.0, "CONTRAPRUEBA: sin cuerpos la marea es exactamente 0");
}

// --------------------------------------------------- TEST 8: el SWASH mueve la línea de costa
void test_ocean_swash() {
    beginTest("ocean_swash");
    using Haruka::World::Water::oceanSwash;
    const auto st = Haruka::World::Water::oceanDefaultState();
    const glm::vec3 up = upAt(0.3, 0.15);
    const glm::vec3 wp = up * (float)kR;

    // (a) EN MAR ABIERTO NO EXISTE. Es la condición que hace el cambio seguro: el oleaje de siempre
    // no se toca, la trepada solo vive en la franja somera.
    float deepMax = 0.0f;
    for (int s = 0; s < 400; ++s)
        deepMax = std::max(deepMax, std::abs(Haruka::World::Water::oceanSwash(60.0f, wp, up, (float)s * 0.05f, st)));
    std::printf("    swash a 60 m de fondo: |max| %.4f m (mar abierto: debe ser ~0)\n", deepMax);
    CHECK(deepMax < 0.01f, "en mar abierto el swash no existe");

    // (b) EN LA ORILLA OSCILA, y con amplitud del orden de la ola que rompe. Ése es el movimiento
    // que antes NO había: la banda de espuma era un smoothstep fijo sobre la profundidad.
    float lo = 1e9f, hi = -1e9f;
    for (int s = 0; s < 2000; ++s) {
        const float v = Haruka::World::Water::oceanSwash(0.3f, wp, up, (float)s * 0.02f, st);
        lo = std::min(lo, v); hi = std::max(hi, v);
    }
    std::printf("    swash en 0.3 m de fondo: recorrido %.3f m  (de %+.3f a %+.3f)\n", hi - lo, lo, hi);
    CHECK(hi - lo > 0.3f, "en la orilla la lamina SUBE Y BAJA de verdad");
    CHECK(hi > 0.0f && lo < 0.0f, "trepa y se retira (la orilla respira en los dos sentidos)");

    // (c) SOBRE ARENA SECA la trepada sigue siendo POSITIVA en su fase alta — es lo que permite que
    // el agua moje terreno que estaba fuera del mar. ⚠️ Es el caso que rompía la primera versión:
    // `oceanShoalAmp` acota por `0.55·d` y con `d` negativa devolvía amplitud NEGATIVA, o sea que la
    // lámina se hundía justo donde tenía que subir.
    float dryHi = -1e9f;
    for (int s = 0; s < 2000; ++s)
        dryHi = std::max(dryHi, Haruka::World::Water::oceanSwash(-0.5f, wp, up, (float)s * 0.02f, st));
    std::printf("    swash sobre arena a -0.5 m: pico %+.3f m (debe ser >0: moja tierra)\n", dryHi);
    CHECK(dryHi > 0.1f, "sobre tierra la trepada es positiva (moja arena seca)");

    // (d) ESCALA CON EL ESTADO DEL MAR — pero NO en el último palmo de agua, y eso es física, no un
    // defecto. En 1 m de fondo el límite de rompiente (`0.55·d`) satura por igual a la brisa y a la
    // galerna: una ola no puede ser más alta que el fondo que la sostiene, venga del temporal que
    // venga. Lo que sí distingue a los dos mares es (1) la trepada MÁS ADENTRO, donde el tope aún no
    // muerde, y (2) hasta dónde llega la zona de rompientes.
    const auto calm  = Haruka::World::Water::oceanStateFromWind(5.0f,  1.0f, 0.0f, 0.0f);
    const auto storm = Haruka::World::Water::oceanStateFromWind(18.0f, 1.0f, 0.0f, 0.0f);
    auto swingAt = [&](const Haruka::World::Water::OceanState& s0, float depth) {
        float a = 1e9f, b = -1e9f;
        for (int s = 0; s < 2000; ++s) {
            const float v = Haruka::World::Water::oceanSwash(depth, wp, up, (float)s * 0.02f, s0);
            a = std::min(a, v); b = std::max(b, v);
        }
        return b - a;
    };
    const float sc = swingAt(calm, 3.0f), ss = swingAt(storm, 3.0f);
    std::printf("    a 3 m de fondo: brisa (5 m/s) %.3f m vs galerna (18 m/s) %.3f m -> x%.1f\n",
                sc, ss, ss / std::max(sc, 1e-4f));
    CHECK(ss > sc * 2.0f, "mas adentro, la trepada crece con el estado del mar");

    // El ALCANCE de la zona de rompientes: a qué profundidad el swash ya se ha apagado. Con galerna
    // la rompiente empieza mucho más lejos de la playa, que es lo que se ve en una costa de verdad.
    auto reachOf = [&](const Haruka::World::Water::OceanState& s0) {
        float d = 0.5f;
        for (; d < 60.0f; d += 0.25f) if (swingAt(s0, d) < 0.05f) break;
        return d;
    };
    const float rc = reachOf(calm), rs = reachOf(storm);
    std::printf("    alcance de la rompiente: brisa hasta %.1f m de fondo · galerna hasta %.1f m\n", rc, rs);
    CHECK(rs > rc * 1.5f, "con galerna la rompiente empieza mucho mas lejos de la playa");

    // CONTRAPRUEBA: el recorrido en la orilla tiene que ser MUCHO mayor que en mar abierto. Sin esto
    // el test pasaría con un swash constante aplicado en todas partes.
    std::printf("    CONTRAPRUEBA: orilla %.3f m vs mar abierto %.4f m\n", hi - lo, deepMax);
    CHECK((hi - lo) > deepMax * 20.0f, "CONTRAPRUEBA: el swash es un fenomeno de ORILLA");
}

// ------------------------------------------------------- TEST 10: el tope de rompiente es de la OLA
//
// La ola no puede ser mas alta que el agua que la sostiene. Es el indice de rompiente clasico: una
// ola revienta cuando su altura se acerca a la profundidad. Suena obvio y el motor lo incumplia:
// `oceanShoalAmp` acotaba CADA uno de los 4 trenes a `0.55·d` y luego `oceanWaveHeight` los sumaba,
// asi que el total podia llegar a `4 x 0.55·d = 2,2·d`. Medido antes del arreglo: un lago de 3,21 m
// de fondo daba 5,93 m de ola pico a pico, casi el doble que en mar abierto (3,16 m).
//
// Se veia en TODA la costa —el bajio es un fenomeno de agua somera— pero saltaba a la vista en los
// lagos de montana, que son someros enteros. Este test es el guardian: si alguien vuelve a mover el
// tope al bucle, el barrido de profundidades lo caza.
void test_ocean_break_limit() {
    beginTest("ocean_break_limit");

    const glm::vec3 up = upAt(0.4, 0.3);
    const glm::vec3 wp = up * (float)kR;

    // Altura pico a pico barriendo tiempo y espacio: una sola muestra podria caer en un nodo.
    auto peakToPeak = [&](float depth) {
        float lo = 1e9f, hi = -1e9f;
        for (int s = 0; s < 20; ++s) {
            const glm::vec3 p = wp + glm::vec3(1.0f, 0.0f, 0.3f) * (float)(s * 11);
            for (int k = 0; k < 48; ++k) {
                const float h = oceanWaveHeight(p, up, 0.29f * k, depth);
                lo = std::min(lo, h); hi = std::max(hi, h);
            }
        }
        return hi - lo;
    };

    // El tope es sobre la AMPLITUD (0.55·d), asi que la altura pico a pico no puede pasar de 1,1·d.
    const float kMaxRatio = 1.1f;
    std::printf("    altura de ola frente a profundidad (el tope es %.2f x la profundidad):\n", kMaxRatio);

    int worstIdx = -1; float worstRatio = 0.0f;
    const float depths[] = { 0.5f, 1.0f, 2.0f, 3.21f, 5.0f, 8.0f, 15.0f, 30.0f };
    for (int i = 0; i < 8; ++i) {
        const float d  = depths[i];
        const float pp = peakToPeak(d);
        const float r  = pp / d;
        std::printf("      fondo %5.2f m -> ola %5.2f m  (%.2f x la profundidad)%s\n",
                    d, pp, r, r > kMaxRatio ? "   <-- MAS ALTA QUE EL AGUA" : "");
        if (r > worstRatio) { worstRatio = r; worstIdx = i; }
        CHECK(r <= kMaxRatio + 0.02f, "la ola no supera el limite de rompiente a esta profundidad");
    }
    std::printf("    peor caso: %.2f x a %.2f m de fondo (antes del arreglo se llegaba a ~1,85 x)\n",
                worstRatio, worstIdx >= 0 ? depths[worstIdx] : 0.0f);

    // CONTRAPRUEBA 1: en agua HONDA el tope no debe morder, o el test estaria pasando simplemente
    // porque la ola es pequena en todas partes y no porque el limite funcione.
    const float ppDeep = peakToPeak(400.0f);
    const float ppAbyss = peakToPeak(4000.0f);
    std::printf("    CONTRAPRUEBA: a 400 m %.3f m y a 4000 m %.3f m — el tope no muerde (iguales)\n",
                ppDeep, ppAbyss);
    CHECK(std::abs(ppDeep - ppAbyss) < 0.05f * ppAbyss,
          "en agua honda el tope no toca la ola (no esta aplastando el mar entero)");

    // CONTRAPRUEBA 2: el tope tiene que DEPENDER de la profundidad. Si diera lo mismo a 1 m que a
    // 400 m, no seria un limite de rompiente sino una constante disfrazada.
    const float pp1 = peakToPeak(1.0f);
    std::printf("    CONTRAPRUEBA: a 1 m la ola es %.3f m y a 400 m %.3f m (%.1fx)\n",
                pp1, ppDeep, ppDeep / std::max(pp1, 1e-3f));
    CHECK(pp1 < 0.6f * ppDeep, "el limite depende de la profundidad (no es una constante)");
}

// ------------------------------------------------------- TEST 11: la ola PUEDE plegarse al romper
//
// Una ola real rompe volcando: la cresta se echa hacia delante hasta que la superficie se pliega
// sobre si misma. En Gerstner eso ocurre cuando `Σ Q·A·k > 1`, porque ahi el jacobiano del
// desplazamiento se vuelve negativo — y el propio shader lo usa como senal de rompiente.
//
// ⚠️ CON LA FORMULA ANTERIOR NO PODIA PASAR NUNCA. Era `Q = 0.75/(k·A·N)` acotada a 1, lo que deja
// `Q·A·k ≤ 0.75/N` por tren y `Σ ≤ 0.75`: el jacobiano se quedaba en 0,25 como suelo. El comentario
// de `harukaGerstner` decia "si pasa de 1, la pliega" describiendo algo que su propia formula
// impedia, y la espuma por plegado no se encendia jamas. Este test fija las dos mitades: que en mar
// abierto NO se pliegue (el oleaje de fondo no vuelca solo) y que al romper SI.
void test_ocean_break_fold() {
    beginTest("ocean_break_fold");

    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
    const glm::vec3 up = upAt(0.2, -0.4);
    const glm::vec3 wp = up * (float)kR;
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0,1,0))
                                                               : glm::cross(up, glm::vec3(1,0,0)));
    const glm::vec3 t2 = glm::cross(up, t1);

    // ⚠️ ESTE TEST REIMPLEMENTABA EL JACOBIANO A MANO Y SU COPIA SE QUEDO ANCLADA A LA CADENA VIEJA:
    // `oceanSteepness` como `Q` y `k = 2π/λ` de aguas profundas, sin `oceanWaveNumber`, sin
    // `oceanHorizAmp`, sin `oceanSteepScale` y sin `oceanCurlGain`. Seguia VERDE midiendo un motor que
    // ya no existe, y sus comentarios afirmaban "la voluta se revirtio" y "aqui lambda es FIJA" —
    // las dos cosas dejaron de ser ciertas el 2026-09-01. Es el mismo modo de fallo que
    // `terrain_two_bakes_disagree`: un numero de test se cita con la confianza de una medida.
    //
    // Ahora llama a `oceanJacobian`, que es LO QUE EJECUTA EL MOTOR (y el gemelo de la `jac` que
    // acumula `harukaGerstner`). Si la formula cambia, este test se entera.
    auto minJacobian = [&](float depth) {
        float worst = 1e9f;
        for (int s = 0; s < 30; ++s) {
            const glm::vec3 p = wp + glm::vec3(1.0f, 0.0f, 0.4f) * (float)(s * 5);
            for (int n = 0; n < 60; ++n) {
                const float t = 0.21f * n;
                worst = std::min(worst, Haruka::World::Water::oceanJacobian(p, up, t, depth, 1.0f, st));
            }
        }
        return worst;
    };

    const float jacDeep  = minJacobian(60.0f);
    const float jacMid   = minJacobian(6.0f);
    const float jacBreak = minJacobian(1.2f);

    std::printf("    jacobiano minimo (negativo = superficie PLEGADA = la cresta vuelca):\n");
    std::printf("      mar abierto 60,0 m -> %+.3f %s\n", jacDeep,  jacDeep  < 0 ? "(pliega)" : "(no pliega)");
    std::printf("      transicion   6,0 m -> %+.3f %s\n", jacMid,   jacMid   < 0 ? "(pliega)" : "(no pliega)");
    std::printf("      rompiendo    1,2 m -> %+.3f %s\n", jacBreak, jacBreak < 0 ? "(pliega)" : "(no pliega)");

    // LA VOLUTA YA SALE, y lo que la desbloqueo no fue mas compresion sino CONCENTRARLA. Historia
    // corta, porque las dos hipotesis intermedias eran falsas:
    //  1. "el adelanto vive en `sin φ = 0`" — FALSO: la derivada de `H·cos φ·(1+g·cos φ)` es
    //     `−H·sin φ·(1+2g·cos φ)`, o sea que entra por el producto.
    //  2. habia un error de derivada de verdad (se acumulaba `(1+g·cos φ)`, la mitad), arreglado y
    //     comprobado abajo contra la derivada NUMERICA — y aun asi no plegaba.
    // La causa real: el escarpado por divergencia ya pasaba de 1, pero se REPARTIA entre las dos
    // direcciones del mapa horizontal, y un determinante no se anula asi. `oceanBreakerGain` concentra
    // el refuerzo en el tren 0 REFRACTADO —crestas paralelas a la orilla, como un surf real— y con eso
    // un autovalor cruza el cero.
    //
    // ⚠️ El fondo de 1,2 m NO pliega, y es correcto: ahi el limite de rompiente ya ha aplastado la
    // amplitud (`Σamp ≤ 0,55·d`), asi que queda poca ola que volcar. La voluta vive en la BARRA, no en
    // la orilla — que es donde rompe una ola de verdad.
    // ⚠️ SE PROBO A CERRAR EL PLIEGUE Y SE REVIRTIO, CON LOS NUMEROS DELANTE. `ocean_mesh_stretch`
    // midio lo que este test no puede ver —el avance entre vertices VECINOS— y el vuelco sale como
    // **87 celdas del reves de 48 000 (0,18 %)**. Se implemento un tope del vaiven horizontal
    // (`sum k·horiz` acotada) y funcionaba:
    //
    //   sin tope          jac -0,006 ·  87 celdas invertidas · estiramiento 2,33x
    //   tope 1,80         jac +0,095 ·  58 celdas invertidas · 2,27x
    //   tope 0,97         jac +0,512 ·   0 celdas invertidas · 1,74x   <- sin vuelco
    //
    // **Andoni eligio el vuelco**, y es defendible: en un mapa de altura con desplazamiento
    // horizontal, "la cresta vuelca" y "la malla se cruza" son LA MISMA COSA, asi que exigir cero
    // celdas cruzadas es exigir que no haya rompiente. Lo que se dibuja con `cull = None` y la
    // normal analitica es una cresta que sobresale, que es lo que se queria.
    //
    // ⚠️ Lo que NO puede pasar es que el pliegue se EXTIENDA sin que nadie se entere: en mar abierto
    // no debe cruzarse una sola celda, y la fraccion que cruza en la rompiente esta acotada por
    // `ocean_mesh_stretch`. El desastre de 2026-08-29 (la charca con metros de vaiven) esta cerrado
    // por otra via — la elipse escala con la amplitud — y su caso sigue medido aqui abajo.
    CHECK(jacDeep  > 0.0f, "en mar abierto la ola NO vuelca sola");
    CHECK(jacMid   < 0.0f, "en la franja de rompiente la cresta SI se pliega");

    // ── EL ORACULO: ¿es `oceanJacobian` la derivada del desplazamiento que el motor APLICA? ───────
    //
    // Hasta ahora la formula era su propio juez: la unica referencia del jacobiano era su propia
    // acumulacion analitica. Con `oceanDisplacement` (el desplazamiento COMPLETO, gemelo del valor que
    // devuelve `harukaGerstner`) se puede medir por diferencias finitas y comparar.
    //
    // ⚠️ Y HAY DOS COSAS DISTINTAS QUE MEDIR, porque con varios trenes NO son la misma:
    //  · el escalar que acumula el motor, `1 − Σ k·horiz·sin φ`, que es la traza;
    //  · el DETERMINANTE del mapa horizontal 2x2, `det(I + ∂disp_h/∂x)`, que es el criterio real de
    //    que la superficie deje de ser inyectiva. Solo coinciden si todos los trenes van paralelos.
    {
        // ⚠️ LA PROFUNDIDAD SALE DEL INDICE DE ROMPIENTE, NO DE UN NUMERO (17-10). Eran 5,15 m fijos,
        // elegidos cuando el pliegue lo hacia el adelanto de cresta en la ESPALDA de la ola (el signo
        // estaba al reves, ver `coast_lip_front_steep`). Con el signo bueno quien pliega es el LABIO
        // (`oceanLipGain`), que vive en el indice 0,78-1,7 — y a 5,15 m esta mar ya va por 1,69: espuma,
        // labio apagado. Se mide donde el labio esta entero (indice ~1,15).
        float d = 5.15f;
        for (float dd = 30.0f, bestErr = 1e9f; dd > 0.5f; dd -= 0.05f) {
            const float e = std::abs(Haruka::World::Water::oceanBreakIndex(dd, st) - 1.15f);
            if (e < bestErr) { bestErr = e; d = dd; }
        }
        // ⚠️ SIN PENDIENTE NO HAY REFRACCION, y en una playa de verdad la hay: al perder fondo los
        // trenes GIRAN hacia la orilla y se alinean entre si. Ocho trenes en ocho direcciones no
        // pliegan porque nunca coinciden en fase; ocho trenes casi paralelos son otra cosa. Se mide
        // con y sin, porque la diferencia es justo la hipotesis.
        const glm::vec3 haciaLaOrilla = t1;      // solo cuenta la direccion: `oceanRefract` normaliza
        double peorTraza = 0.0, minDet = 1e9, minAnalitico = 1e9;
        double minDetRefr = 1e9, minAnaRefr = 1e9;
        // ⚠️ LAS DIFERENCIAS FINITAS NO SE PUEDEN TOMAR EN COORDENADAS DE PLANETA. A radio terrestre
        // (6,4e6 m) un float32 tiene 1 ulp ≈ 0,5 m, asi que `p + t1*0.02f` **es exactamente `p`**: la
        // derivada sale 0 y el determinante un +1,000 perfecto que parece "no hay deformacion" y es
        // "no hay medida". Paso primero por aqui. La ola solo depende de la posicion a traves de
        // `k·dot(D, wp)`, asi que evaluar cerca del origen es la MISMA ola con otra fase — y ahi el
        // paso de 2 cm si existe.
        for (int s = 0; s < 40; ++s) {
            const glm::vec3 p = glm::vec3(1.0f, 0.0f, 0.4f) * (float)(s * 3);
            for (int n = 0; n < 40; ++n) {
                const float tt = 0.17f * n;
                // Derivada numerica del desplazamiento sobre el plano tangente. `h` en metros: lo
                // bastante fino para la onda mas corta (8,7 m) y lo bastante grueso para no ahogarse
                // en el ruido del float.
                const float h = 0.02f;
                const glm::vec3 d1 = (Haruka::World::Water::oceanDisplacement(p + t1*h, up, tt, d, 1.0f, st)
                                    - Haruka::World::Water::oceanDisplacement(p - t1*h, up, tt, d, 1.0f, st))
                                   / (2.0f * h);
                const glm::vec3 d2 = (Haruka::World::Water::oceanDisplacement(p + t2*h, up, tt, d, 1.0f, st)
                                    - Haruka::World::Water::oceanDisplacement(p - t2*h, up, tt, d, 1.0f, st))
                                   / (2.0f * h);
                // Mapa horizontal: I + [∂disp/∂t1 , ∂disp/∂t2] proyectado en (t1,t2).
                const float a11 = 1.0f + glm::dot(d1, t1), a12 = glm::dot(d2, t1);
                const float a21 = glm::dot(d1, t2),        a22 = 1.0f + glm::dot(d2, t2);
                const double det   = (double)a11 * a22 - (double)a12 * a21;
                // ⚠️ `1 + divergencia`, NO la media de la diagonal. `a11 + a22 = 2 + div`, asi que la
                // media es `1 + div/2` y comparar contra ella mete un factor 2 — me paso, y el
                // "error" que salia era el de la metrica, no el del motor.
                const double traza = (double)a11 + a22 - 1.0;
                const double ana   = Haruka::World::Water::oceanJacobian(p, up, tt, d, 1.0f, st);
                minDet = std::min(minDet, det);
                minAnalitico = std::min(minAnalitico, ana);
                peorTraza = std::max(peorTraza, std::abs(ana - traza));

                // Lo mismo CON refraccion: los trenes giran hacia la orilla y se alinean.
                const glm::vec3 r1 = (Haruka::World::Water::oceanDisplacement(p + t1*h, up, tt, d, 1.0f, st,
                                          Haruka::World::Water::WATER_FETCH_UNLIMITED, haciaLaOrilla)
                                    - Haruka::World::Water::oceanDisplacement(p - t1*h, up, tt, d, 1.0f, st,
                                          Haruka::World::Water::WATER_FETCH_UNLIMITED, haciaLaOrilla))
                                   / (2.0f * h);
                const glm::vec3 r2 = (Haruka::World::Water::oceanDisplacement(p + t2*h, up, tt, d, 1.0f, st,
                                          Haruka::World::Water::WATER_FETCH_UNLIMITED, haciaLaOrilla)
                                    - Haruka::World::Water::oceanDisplacement(p - t2*h, up, tt, d, 1.0f, st,
                                          Haruka::World::Water::WATER_FETCH_UNLIMITED, haciaLaOrilla))
                                   / (2.0f * h);
                const float b11 = 1.0f + glm::dot(r1, t1), b12 = glm::dot(r2, t1);
                const float b21 = glm::dot(r1, t2),        b22 = 1.0f + glm::dot(r2, t2);
                minDetRefr = std::min(minDetRefr, (double)b11 * b22 - (double)b12 * b21);
                minAnaRefr = std::min(minAnaRefr,
                    (double)Haruka::World::Water::oceanJacobian(p, up, tt, d, 1.0f, st,
                                Haruka::World::Water::WATER_FETCH_UNLIMITED, haciaLaOrilla));
            }
        }
        std::printf("    ORACULO a %.2f m (40 puntos x 40 instantes, derivada numerica):\n", d);
        std::printf("      jacobiano ANALITICO minimo   %+.3f\n", minAnalitico);
        std::printf("      DETERMINANTE 2x2 minimo      %+.3f   <- el criterio real de pliegue\n", minDet);
        std::printf("      peor |analitico - traza|     %.4f   <- si no es ~0, la formula no es su derivada\n",
                    peorTraza);
        std::printf("      CON REFRACCION (playa):  analitico %+.3f · DETERMINANTE %+.3f\n",
                    minAnaRefr, minDetRefr);

        // EL ORACULO, y es lo que impide que la formula vuelva a ser su propio juez: el jacobiano que
        // acumula el motor tiene que ser la divergencia del desplazamiento que el motor APLICA.
        CHECK(peorTraza < 0.01, "`oceanJacobian` ES la divergencia de `oceanDisplacement` (derivada numerica)");

        // EL CRITERIO REAL DE PLIEGUE, y se cruza: con refraccion el determinante es NEGATIVO, o sea
        // que la superficie deja de ser inyectiva. Eso es una ola volcando — y es lo que se quiere.
        CHECK(minDetRefr < 0.0, "CON REFRACCION la superficie SE PLIEGA (determinante < 0)");
        // ⚠️ Y SIN REFRACCION YA TAMBIEN PLIEGA, y es correcto: aqui se pedia `minDet > 0` ("sin alinear
        // los trenes no pliega: el mecanismo es concentrar"). Eso medía el refuerzo del tren 0, que solo
        // concentra si la refraccion alinea los demas. El LABIO concentra por construccion: va entero
        // en el rumbo del tren que rompe. Lo que sigue siendo cierto —y es lo que se mide— es que
        // alinear los trenes pliega MAS.
        CHECK(minDetRefr < minDet, "la refraccion (trenes alineados hacia la orilla) pliega MAS");
        std::printf("      CONTRAPRUEBA: la refraccion pliega MAS (%.3f -> %.3f)\n",
                    minDet, minDetRefr);

        // ⚠️ LA GUARDA QUE DE VERDAD IMPORTA: EL AGUA INTERIOR. El intento de voluta de 2026-08-29 se
        // revirtio porque metia metros de desplazamiento horizontal en una charca y la lamina se
        // plegaba sobre celdas de 4,4 m. Aqui el refuerzo esta cerrado por el indice de rompiente
        // (arranca en 0,78) y es una FRACCION del semieje, que ya escala con la amplitud. Medido: una
        // charca de 30 cm con fetch de 100 m sale IDENTICA a como estaba antes de todo esto.
        {
            const float dCharca = 0.30f, fetchCharca = 100.0f;
            float horizMax = 0.0f; double detMin = 1e9;
            for (int a = 0; a < 60; ++a) for (int b = 0; b < 60; ++b) {
                const glm::vec3 pc((float)a * 1.7f, 0.0f, (float)b * 1.3f);
                const float tt = 0.11f * b;
                const glm::vec3 dsp = Haruka::World::Water::oceanDisplacement(pc, up, tt, dCharca, 1.0f, st,
                                                                        fetchCharca, haciaLaOrilla);
                horizMax = std::max(horizMax, glm::length(dsp - up * glm::dot(dsp, up)));
                const float e = 0.01f;
                const glm::vec3 q1 = (Haruka::World::Water::oceanDisplacement(pc + t1*e, up, tt, dCharca, 1.0f, st, fetchCharca, haciaLaOrilla)
                                    - Haruka::World::Water::oceanDisplacement(pc - t1*e, up, tt, dCharca, 1.0f, st, fetchCharca, haciaLaOrilla)) / (2.0f*e);
                const glm::vec3 q2 = (Haruka::World::Water::oceanDisplacement(pc + t2*e, up, tt, dCharca, 1.0f, st, fetchCharca, haciaLaOrilla)
                                    - Haruka::World::Water::oceanDisplacement(pc - t2*e, up, tt, dCharca, 1.0f, st, fetchCharca, haciaLaOrilla)) / (2.0f*e);
                const float c11 = 1.0f + glm::dot(q1,t1), c12 = glm::dot(q2,t1);
                const float c21 = glm::dot(q1,t2),        c22 = 1.0f + glm::dot(q2,t2);
                detMin = std::min(detMin, (double)c11*c22 - (double)c12*c21);
            }
            std::printf("    CHARCA de %.2f m (fetch %.0f m): horizontal max %.3f m · determinante min %+.3f\n",
                        dCharca, fetchCharca, horizMax, detMin);
            CHECK(detMin > 0.5, "el AGUA INTERIOR no se pliega (la regresion de 2026-08-29 no vuelve)");
            CHECK(horizMax < 1.5f, "el vaiven de una charca sigue por debajo de su celda de 4,4 m");
        }
    }

    // REPARTO POR TREN en la franja de rompiente: quien lleva el escarpado y quien es el DOMINANTE.
    // Un rompiente real es UNA ola que vuelca, no ocho repartiendose el presupuesto.
    for (float d : { 5.15f, 2.0f }) {
        const float green = Haruka::World::Water::oceanGreenGain(d);
        const float scale = Haruka::World::Water::oceanBreakScale(d, st);
        const float steep = Haruka::World::Water::oceanSteepScale(d, st);
        std::printf("    reparto a %.2f m (lambda0 -> lambda local · amp · k*horiz):\n", d);
        float total = 0.0f;
        for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) {
            const float k0 = 6.2831853f / st.wave[i][0];
            const float k  = Haruka::World::Water::oceanWaveNumber(k0, d);
            const float amp = st.wave[i][1] * green * scale * Haruka::World::Water::oceanShortWaveFade(k);
            const float kh = k * Haruka::World::Water::oceanHorizAmp(k, amp, d) * steep;
            total += kh;
            std::printf("      tren %d: %6.1f -> %5.1f m · amp %.3f · k*horiz %.3f\n",
                        i, st.wave[i][0], 6.2831853f / k, amp, kh);
        }
        std::printf("      TOTAL k*horiz %.3f  (un tren SOLO pliega si supera ~0,54 con el adelanto)\n",
                    total);
    }

    // La cuenta que separa las dos magnitudes, para que no haya que fiarse del parrafo: en la franja
    // de rompiente se mide el escarpado maximo (sobre la cresta, `cos φ = 1`) y el jacobiano minimo.
    // Si el primero pasa de 1 y el segundo no baja de 0, es exactamente el desajuste de fase.
    {
        const float d = 5.15f;   // donde la memoria situa el pico de escarpado
        const float green = Haruka::World::Water::oceanGreenGain(d);
        const float scale = Haruka::World::Water::oceanBreakScale(d, st);
        const float steep = Haruka::World::Water::oceanSteepScale(d, st);
        const float curl  = Haruka::World::Water::oceanCurlGain(d, st);
        float sumCrest = 0.0f;   // Σ k·horiz con el adelanto puesto (cos φ = 1)
        for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) {
            const float k0 = 6.2831853f / st.wave[i][0];
            const float k  = Haruka::World::Water::oceanWaveNumber(k0, d);
            const float amp = st.wave[i][1] * green * scale * Haruka::World::Water::oceanShortWaveFade(k);
            if (amp <= 1e-4f) continue;
            sumCrest += k * Haruka::World::Water::oceanHorizAmp(k, amp, d) * steep * (1.0f + curl);
        }
        std::printf("    a %.2f m: escarpado en la CRESTA (cos=1) %.3f · jacobiano minimo %+.3f\n",
                    d, sumCrest, minJacobian(d));
        std::printf("      el escarpado pasa de 1 y aun asi no pliega: los OCHO trenes no coinciden en fase\n");
    }

    // CONTRAPRUEBA: la cadena VIEJA (Q = 0,75/(k·A·N) acotada a 1, con `k` de aguas profundas) no
    // podia bajar de 1−0,75 = 0,25 a NINGUNA profundidad — el pliegue estaba prohibido por
    // construccion, valga lo que valga el fondo. Se recalcula aqui para dejar constancia de que lo que
    // habilita la voluta es el cambio de formula, y no que el barrido haya tenido suerte.
    float worstOld = 1e9f;
    for (float depth : { 60.0f, 6.0f, 1.2f }) {
        const float green = Haruka::World::Water::oceanGreenGain(depth);
        const float scale = Haruka::World::Water::oceanBreakScale(depth, st);
        for (int s = 0; s < 30; ++s) {
            const glm::vec3 p = wp + glm::vec3(1.0f, 0.0f, 0.4f) * (float)(s * 5);
            for (int n = 0; n < 60; ++n) {
                const float t = 0.21f * n;
                float jac = 1.0f;
                for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) {
                    const float k   = 6.2831853f / st.wave[i][0];
                    const float amp = st.wave[i][1] * green * scale;
                    if (amp <= 1e-4f) continue;
                    const glm::vec3 D = glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]);
                    float Q = 0.75f / (k * amp * float(Haruka::World::Water::OCEAN_WAVES) + 1e-4f);
                    Q = std::min(Q, 1.0f);                     // el tope viejo
                    const float ph = k * glm::dot(D, p) - std::sqrt(Haruka::World::Water::OCEAN_G * k) * t;
                    jac -= Q * amp * k * std::sin(ph);
                }
                worstOld = std::min(worstOld, jac);
            }
        }
    }
    std::printf("    CONTRAPRUEBA: la cadena vieja (Q acotado a 1, k de aguas profundas) da %+.3f en el mejor caso\n",
                worstOld);
    CHECK(worstOld > 0.0f, "la cadena vieja NO podia plegar: el pliegue lo trae la formula nueva");
}

// ------------------------------------------------ TEST: BORREGUILLOS en mar abierto, contra Monahan
//
// El oceano abierto salia LISO Y OSCURO hasta la costa: la unica espuma era la de orilla y la de
// plegado, y esta ultima pedia un escarpado local de 0,45 que en mar abierto se alcanza el 0,000 % de
// las veces. Un mar de 11,4 m/s tiene crestas blancas.
//
// ⚠️ EL ORACULO NO SOY YO: es Monahan & O'Muircheartaigh (1980), `W = 3,84e-6·U^3,41`, con el viento
// que el propio estado del mar DEDUCE (no uno que yo elija). El umbral de espuma se calibro contra
// esa cobertura; este test comprueba que sigue casando, que es lo que impide que alguien lo mueva
// "porque se ve mejor" sin enterarse de que se ha salido de la fisica.
void test_ocean_whitecaps() {
    beginTest("ocean_whitecaps");
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;

    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
    const glm::vec3 up = upAt(0.2, -0.4);

    auto muestra = [&](int i, int c) {
        uint32_t h = (uint32_t)i * 2654435761u + (uint32_t)c * 40503u;
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
    };
    const int N = 200000;
    auto cobertura = [&](float depth, const Haruka::World::Water::OceanState& s) {
        int blancos = 0;
        for (int i = 0; i < N; ++i) {
            const glm::vec3 p(muestra(i, 0) * 800.0f, 0.0f, muestra(i, 1) * 800.0f);
            if (Haruka::World::Water::oceanFoam(p, up, muestra(i, 2) * 200.0f, depth, 1.0f, s) > 0.5f) ++blancos;
        }
        return (float)blancos / (float)N;
    };

    const float U = Haruka::World::Water::oceanWindSpeed(st);
    const float W = Haruka::World::Water::oceanWhitecapCoverage(st);
    std::printf("    viento DEDUCIDO del estado %.3f m/s · Hs %.4f m\n", U, Haruka::World::Water::oceanSignificantHeight(st));
    std::printf("    cobertura de Monahan: %.4f%%\n", 100.0f * W);

    const float c400 = cobertura(400.0f, st);
    const float c60  = cobertura(60.0f,  st);
    const float c20  = cobertura(20.0f,  st);
    const float c8   = cobertura(8.0f,   st);
    std::printf("    cobertura medida: 400 m %.3f%% · 60 m %.3f%% · 20 m %.3f%% · 8 m %.3f%%\n",
                100.0f * c400, 100.0f * c60, 100.0f * c20, 100.0f * c8);

    // 1. HAY BORREGUILLOS. Antes esto era 0,000 % exacto.
    CHECK(c400 > 0.002f, "en mar abierto HAY crestas blancas (antes: ninguna)");
    // 2. Y SON LOS QUE TOCAN. El margen es amplio a proposito: Monahan es una correlacion empirica con
    //    dispersion de un factor ~2 entre campanas, asi que exigir mas seria fingir precision. Lo que
    //    se afirma es el ORDEN DE MAGNITUD correcto, y eso si lo fija.
    CHECK(c400 > 0.5f * W && c400 < 2.0f * W, "la cobertura casa con Monahan (mismo orden)");
    // 3. SUBE HACIA LA COSTA: al perder fondo la ola se peralta y rompe mas.
    CHECK(c20 > c60 && c8 > c20, "la cobertura crece al perder fondo");
    // 4. Y NO SATURA: si el bajio saliera al 100 % la costa seria una sabana blanca.
    CHECK(c8 < 0.60f, "ni siquiera la rompiente satura la superficie entera");

    // CONTRAPRUEBA: MAS VIENTO, MAS BORREGUILLOS. Es lo que separa "espuma calibrada" de "una
    // constante que casualmente daba 1,5 %". Monahan va como U^3,41, asi que subir el mar tiene que
    // subir la cobertura MUCHO.
    // ⚠️ LA CONTRAPRUEBA ANTERIOR NO ERA UN CAMBIO DE VIENTO. Multiplicaba las amplitudes por 1,6
    // dejando las longitudes de onda quietas: eso es un mar MAS ESCARPADO, no uno mas ventoso, y
    // Monahan no habla de eso. Un viento mayor sube `Hs` Y `lambda` a la vez (los dos con `U²`), que
    // es justo lo que hace `oceanStateFromWind` — el instrumento correcto, y ya existia.
    const Haruka::World::Water::OceanState fuerte = Haruka::World::Water::oceanStateFromWind(15.0f, 1.0f, 0.0f);
    const float Ufuerte = Haruka::World::Water::oceanWindSpeed(fuerte);
    const float cFuerte = cobertura(400.0f, fuerte);
    const float Wfuerte = Haruka::World::Water::oceanWhitecapCoverage(fuerte);
    std::printf("    CONTRAPRUEBA (mar de PM a otro viento): %.2f -> %.2f m/s · cobertura %.3f%% -> %.3f%%\n",
                U, Ufuerte, 100.0f * c400, 100.0f * cFuerte);
    std::printf("      Monahan a ese viento: %.3f%% · el modelo da %.3f%%\n",
                100.0f * Wfuerte, 100.0f * cFuerte);
    CHECK(cFuerte > c400 * 1.5f, "CONTRAPRUEBA: mas viento da mas borreguillos");
    CHECK(cFuerte > 0.5f * Wfuerte && cFuerte < 2.0f * Wfuerte,
          "la cobertura sigue a Monahan TAMBIEN a otro viento (el umbral se deriva de ella)");
}

// --------------------------------------------------------------- TEST: la ESPUMA existe en CPU
//
// La espuma era la UNICA magnitud de la ola que se calculaba solo en el shader. Sin gemelo en CPU no
// habia con que carearla (eso lo hace ahora `rhi_test_main`, canal alfa de `rhitest_waveprobe`), y —
// mas importante— ni la fisica ni el juego podian preguntar "¿esto es agua blanca?".
//
// Este test no compara contra la GPU (eso es del banco RHI): fija las PROPIEDADES que la espuma tiene
// que cumplir para significar algo, cada una con su contraprueba.
void test_ocean_foam() {
    beginTest("ocean_foam");
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;

    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
    const glm::vec3 up = upAt(0.2, -0.4);
    const glm::vec3 wp = up * (float)kR;

    // ⚠️ EL MAXIMO DEJO DE SERVIR AL LLEGAR LOS BORREGUILLOS. Este test medía la espuma MAXIMA de un
    // barrido; con espuma de cresta calibrada, la cresta mas escarpada del barrido llega a 1,000 a
    // CUALQUIER profundidad, asi que el maximo salia 1,000 en todas partes y dejaba de distinguir el
    // mar abierto de la orilla. El estadistico que significa algo es la MEDIA (cuanta superficie hay
    // blanca), que es tambien la que se carea contra Monahan en `ocean_whitecaps`.
    auto meanFoam = [&](float depth, float fade, const Haruka::World::Water::OceanState& s) {
        double acc = 0.0; int n = 0;
        for (int i = 0; i < 30; ++i) {
            const glm::vec3 p = wp + glm::vec3(1.0f, 0.0f, 0.4f) * (float)(i * 5);
            for (int j = 0; j < 60; ++j) { acc += Haruka::World::Water::oceanFoam(p, up, 0.21f * j, depth, fade, s); ++n; }
        }
        return (float)(acc / n);
    };

    std::printf("    espuma MEDIA por profundidad (barrido de 30 puntos x 60 instantes):\n");
    const float fDeep  = meanFoam(200.0f, 1.0f, st);
    const float fMid   = meanFoam(20.0f,  1.0f, st);
    const float fBreak = meanFoam(2.0f,   1.0f, st);
    const float fShore = meanFoam(0.4f,   1.0f, st);
    std::printf("      mar abierto 200,0 m -> %.4f\n", fDeep);
    std::printf("      transicion   20,0 m -> %.4f\n", fMid);
    std::printf("      rompiendo     2,0 m -> %.4f\n", fBreak);
    std::printf("      orilla        0,4 m -> %.4f\n", fShore);

    // LA GRADACION es lo que hace que la espuma signifique algo: si fuera un tinte uniforme, o si
    // saltara de 0 a 1 en la orilla, no estaria describiendo un mar.
    // ⚠️ NO "> 0,9", QUE ERA EL DEFECTO: con la espuma de rompiente atada solo a la profundidad la
    // orilla salia blanca opaca y quieta en el juego (17-10). Ahora sigue a la cresta: de media, entre
    // el resto de espuma gastada y el blanco entero.
    CHECK(fShore > 0.35f && fShore < 0.85f, "la orilla es mas blanca que el mar, pero no una mancha opaca");
    CHECK(fDeep  < 0.05f, "el mar abierto tiene borreguillos, no espuma (poca superficie)");
    // ⚠️ A 20 m NO tiene por que haber mas: los borreguillos son de VIENTO y su cobertura es Monahan a
    // cualquier profundidad (el umbral se deriva de ella). Lo que crece hacia la costa es la espuma de
    // ROMPIENTE, y esa aparece cuando el indice cruza 0,78 — no a 20 m, donde vale 0,31.
    CHECK(std::fabs(fMid - fDeep) < 0.02f, "a 20 m la ola aun no rompe: espuma de viento, como en profundo");
    CHECK(fBreak > fMid,   "y en la franja de rompiente mas todavia");
    CHECK(fShore >= fBreak - 0.02f, "y la orilla al menos tanto como la rompiente");

    // LA ESPUMA DE ROMPIENTE VIAJA CON LA OLA. En un punto fijo de la rompiente, a lo largo del tiempo:
    // blanca cuando pasa la cresta, el resto de espuma gastada en el seno. Se mide la ELEVACION con
    // `oceanJacobian` (la misma fase que usa `oceanFoam`) y se compara la espuma en cresta y en seno.
    {
        double crestFoam = 0.0, troughFoam = 0.0; int nc = 0, nt = 0;
        float fMin = 1.0f, fMax = 0.0f;
        for (int i = 0; i < 30; ++i) {
            const glm::vec3 p = wp + glm::vec3(1.0f, 0.0f, 0.4f) * (float)(i * 5);
            for (int j = 0; j < 60; ++j) {
                const float tt = 0.21f * j;
                float slope = 0.0f, sigma = 0.0f, e = 0.0f;
                Haruka::World::Water::oceanJacobian(p, up, tt, 0.4f, 1.0f, st, WATER_FETCH_UNLIMITED,
                                                    glm::vec3(0.0f), &slope, &sigma, OCEAN_MIN_QUAD_M, &e);
                const float f = Haruka::World::Water::oceanFoam(p, up, tt, 0.4f, 1.0f, st);
                fMin = std::min(fMin, f); fMax = std::max(fMax, f);
                if (e > 0.6f)  { crestFoam  += f; ++nc; }
                if (e < -0.2f) { troughFoam += f; ++nt; }
            }
        }
        const float fc = nc ? (float)(crestFoam / nc) : 0.0f, ft = nt ? (float)(troughFoam / nt) : 1.0f;
        std::printf("    orilla 0,4 m: espuma en CRESTA %.3f (%d) · en SENO %.3f (%d) · rango %.3f..%.3f\n",
                    fc, nc, ft, nt, fMin, fMax);
        CHECK(nc > 0 && nt > 0, "el barrido pasa por crestas y por senos");
        CHECK(fc > 0.95f, "la cresta que revienta es agua blanca");
        CHECK(ft < 0.40f, "en el seno queda espuma gastada, no blanco opaco");
        CHECK(fMax - fMin > 0.6f, "la espuma de la orilla CAMBIA al pasar la ola (antes: constante)");
    }

    // `fade` apaga la ola fuera del alcance del clipmap, y tiene que apagar la espuma con ella: si no,
    // quedaria un cerco de blanco flotando sobre un mar que ya esta liso.
    const float fFaded = meanFoam(0.4f, 0.0f, st);
    std::printf("    con fade = 0 (fuera del alcance de la ola): %.3f\n", fFaded);
    CHECK(fFaded == 0.0f, "sin ola no hay espuma (el fade apaga las dos)");

    // CONTRAPRUEBA: con un mar MAS GRUESO la espuma tiene que llegar mas adentro. Si `oceanFoam`
    // ignorase el estado y mirase solo la profundidad, esto no se moveria — que es exactamente el
    // error que tendria un gemelo escrito a medias.
    Haruka::World::Water::OceanState alt = st;
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) alt.wave[i][1] *= 2.0f;
    const float fMidAlt = meanFoam(20.0f, 1.0f, alt);
    std::printf("    CONTRAPRUEBA: con el mar al doble de altura, a 20,0 m de fondo %.4f -> %.4f\n",
                fMid, fMidAlt);
    CHECK(fMidAlt > fMid * 1.5f, "un mar mas grueso espuma mas adentro (la espuma depende de la OLA)");
}

/**
 * @brief DISPERSIÓN EN PROFUNDIDAD FINITA: la ola se acorta al perder fondo.
 *
 * Todo el oleaje usaba `ω = √(g·k)` —aguas profundas— también en 30 cm de agua. La relación correcta
 * es `ω² = g·k·tanh(k·d)`, y lo que se conserva al entrar en el bajío es la FRECUENCIA, no la
 * longitud de onda.
 *
 * ⚠️ EL ORÁCULO ES LA PROPIA ECUACIÓN, no una tabla de números que yo haya elegido. Se resuelve `k`
 * y se comprueba que satisface `g·k·tanh(k·d) = ω²`. Eso no se puede hacer trampa: o la cumple o no.
 */
void test_ocean_finite_depth() {
    beginTest("ocean_finite_depth");
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;
    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();

    // (1) EL RESIDUO DE LA ECUACIÓN, sobre los cuatro trenes y toda la franja interesante.
    double worstRel = 0.0; float worstD = 0.0f; int worstI = -1;
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) {
        const float k0 = 6.2831853f / st.wave[i][0];
        const float w2 = Haruka::World::Water::OCEAN_G * k0;
        for (float d : { 0.1f, 0.3f, 1.0f, 3.0f, 10.0f, 30.0f, 100.0f, 400.0f }) {
            const float k   = Haruka::World::Water::oceanWaveNumber(k0, d);
            const double res = std::fabs((double)(Haruka::World::Water::OCEAN_G * k * std::tanh(k * d)) - (double)w2)
                             / (double)w2;
            if (res > worstRel) { worstRel = res; worstD = d; worstI = i; }
        }
    }
    std::printf("    residuo peor de `g k tanh(kd) = w2`: %.3e (tren %d a %.1f m)\n",
                worstRel, worstI, worstD);

    // (2) LA TABLA, para que se vea lo que hace.
    std::printf("    longitud de onda LOCAL por profundidad (la de la tabla es la PROFUNDA):\n");
    std::printf("      tren      prof:   0.3 m    1 m     3 m    10 m    40 m   400 m\n");
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) {
        const float k0 = 6.2831853f / st.wave[i][0];
        std::printf("      %6.1f m         ", st.wave[i][0]);
        for (float d : { 0.3f, 1.0f, 3.0f, 10.0f, 40.0f, 400.0f })
            std::printf("%7.2f", 6.2831853f / Haruka::World::Water::oceanWaveNumber(k0, d));
        std::printf("\n");
    }

    // (3) EL MAR PROFUNDO NO CAMBIA, Y ES BIT A BIT. Con `k0·d >= 10` la salida rápida devuelve `k0`
    //     sin tocar nada, y eso es lo que garantiza que este cambio no toca el mar abierto.
    bool deepExact = true;
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) {
        const float k0 = 6.2831853f / st.wave[i][0];
        const float d  = 10.0f / k0 + 1.0f;           // por encima del umbral de la salida rápida
        if (Haruka::World::Water::oceanWaveNumber(k0, d) != k0) deepExact = false;
    }
    // (4) Y EN SOMERO SÍ CAMBIA, MUCHO. Sin esto, (3) se leería como "la función no hace nada".
    const float k0Long  = 6.2831853f / st.wave[0][0];
    const float kShallow = Haruka::World::Water::oceanWaveNumber(k0Long, 0.3f);
    const float ratio    = kShallow / k0Long;
    std::printf("    tren de %.0f m: k x%.2f en 30 cm de agua (lambda %.1f -> %.2f m)\n",
                st.wave[0][0], ratio, st.wave[0][0], 6.2831853f / kShallow);

    // (5) EL LÍMITE SOMERO ANALÍTICO: `k -> sqrt(k0/d)`. Es otra comprobación independiente de (1),
    //     y con una fórmula distinta: si las dos casan, el solver está resolviendo lo que dice.
    const float kAsym = std::sqrt(k0Long / 0.05f);
    const float kNum  = Haruka::World::Water::oceanWaveNumber(k0Long, 0.05f);
    std::printf("    limite somero a 5 cm: numerico %.4f vs analitico sqrt(k0/d) %.4f (%.2f %%)\n",
                kNum, kAsym, 100.0 * std::fabs(kNum - kAsym) / kAsym);

    CHECK(worstRel < 1e-5, "el `k` resuelto satisface la relacion de dispersion de profundidad finita");
    CHECK(deepExact, "en agua profunda devuelve el `k` de la tabla EXACTO: el mar abierto no cambia");
    CHECK(ratio > 4.0f, "CONTRAPRUEBA: en agua somera el numero de onda cambia MUCHO (si no, (3) no "
                        "demostraria nada)");
    CHECK(std::fabs(kNum - kAsym) / kAsym < 0.05f,
          "y casa con el limite somero analitico, que es una formula distinta");
}

// ---------------------------------- TEST: REFRACCION y ROMPIENTE (los dos que faltaban) ----------
// ⚠️ LOS DOS AGUJEROS QUE QUEDABAN DE "OLAS REALES", y los dos estaban medidos antes de tocarlos:
//
//   · REFRACCION: no existia en ninguna parte del motor. `D = normalize(t1*W.z + t2*W.w)` — la
//     direccion del tren NO dependia del fondo, ni en CPU ni en GLSL. Las olas llegaban a la playa
//     con el angulo del mar abierto; en el mar real giran al perder fondo y entran casi paralelas a
//     la orilla, que es por lo que las crestas se ven paralelas a la playa desde cualquier sitio.
//   · ROMPIENTE: el escarpado total llegaba a 0,818 y plegar pide pasar de 1 — y QUITANDO EL TOPE
//     ENTERO se quedaba igual. O sea que no era el tope: una ola simetrica con la altura que permite
//     el limite de rompiente no se pliega por mucho escarpado que se le eche. Falta ASIMETRIA.
void test_ocean_refraction_and_break() {
    beginTest("ocean_refraction_and_break");
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;
    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
    const glm::vec3 up(0, 1, 0);
    const glm::vec3 costa(1, 0, 0);      // hacia agua MENOS profunda (la orilla esta al +X)
    const glm::vec3 isobata(0, 0, 1);    // a lo largo de la playa

    // ── (1) EN AGUA PROFUNDA NO PASA NADA. Si esto fallara, el arreglo habria cambiado el mar
    //        abierto entero, que es el 99 % de lo que se ve.
    {
        const float k0 = 6.2831853f / st.wave[0][0];
        const float k  = Haruka::World::Water::oceanWaveNumber(k0, 4000.0f);
        const glm::vec3 d0 = glm::normalize(costa * 0.5f + isobata * 0.866f);   // 60 grados oblicuo
        const glm::vec3 d  = Haruka::World::Water::oceanRefract(d0, costa, up, k0, k);
        const double giro = std::acos(glm::clamp((double)glm::dot(d, d0), -1.0, 1.0)) * 180.0 / 3.14159265;
        std::printf("    a 4000 m de fondo la ola gira %.4f grados (k/k0 = %.6f)\n", giro, k / k0);
        CHECK(giro < 0.01, "en agua PROFUNDA la refraccion no toca la direccion");
    }

    // ── (2) AL PERDER FONDO, LA OLA SE ENDEREZA. Se barre la profundidad y se mide el angulo que
    //        forma la cresta con la orilla. Tiene que ir a cero.
    {
        const float k0 = 6.2831853f / st.wave[0][0];
        const glm::vec3 d0 = glm::normalize(costa * 0.342f + isobata * 0.940f);   // 70 grados oblicuo
        const double ang0 = std::acos(glm::clamp((double)glm::dot(d0, costa), -1.0, 1.0)) * 180.0/3.14159265;
        std::printf("    fondo(m)   angulo con la NORMAL a la costa   (empieza en %.1f grados)\n", ang0);
        double prev = 1e9, ultimo = 0.0;
        bool monotono = true;
        for (float d : { 200.0f, 50.0f, 20.0f, 10.0f, 5.0f, 2.0f, 1.0f, 0.5f }) {
            const float k = Haruka::World::Water::oceanWaveNumber(k0, d);
            const glm::vec3 dd = Haruka::World::Water::oceanRefract(d0, costa, up, k0, k);
            const double a = std::acos(glm::clamp((double)glm::dot(dd, costa), -1.0, 1.0)) * 180.0/3.14159265;
            std::printf("      %7.1f   %10.2f\n", d, a);
            if (a > prev + 1e-3) monotono = false;
            prev = a; ultimo = a;
        }
        CHECK(monotono, "el angulo con la normal a la costa NO crece al perder fondo (se endereza)");
        CHECK(ultimo < ang0 * 0.5, "y a 0,5 m de fondo se ha enderezado a menos de la mitad");
        // ⚠️ CONTRAPRUEBA: la LEY, no solo la tendencia. Snell conserva `k·sinθ`; si esto no se
        // comprobara, cualquier funcion que gire la ola hacia la costa pasaria el test de arriba.
        double peorSnell = 0.0;
        for (float d : { 200.0f, 20.0f, 5.0f, 1.0f }) {
            const float k = Haruka::World::Water::oceanWaveNumber(k0, d);
            const glm::vec3 dd = Haruka::World::Water::oceanRefract(d0, costa, up, k0, k);
            const double kAlong0 = (double)k0 * glm::dot(d0, isobata);
            const double kAlong  = (double)k  * glm::dot(dd, isobata);
            peorSnell = std::max(peorSnell, std::fabs(kAlong - kAlong0) / std::fabs(kAlong0));
        }
        std::printf("    LEY DE SNELL: peor desvio de `k·sin(θ)` conservado: %.3e relativo\n", peorSnell);
        CHECK(peorSnell < 1e-5, "se conserva `k·sin(θ)` — es Snell, no una interpolacion hacia la costa");
    }

    // ── (3) SIN PENDIENTE NO REFRACTA. Un fondo llano no gira nada, y el centinela lo dice.
    {
        const float k0 = 6.2831853f / st.wave[0][0];
        const float k  = Haruka::World::Water::oceanWaveNumber(k0, 3.0f);
        const glm::vec3 d0 = glm::normalize(costa * 0.5f + isobata * 0.866f);
        const glm::vec3 d  = Haruka::World::Water::oceanRefract(d0, glm::vec3(0.0f), up, k0, k);
        CHECK(glm::length(d - d0) < 1e-6f,
              "CONTRAPRUEBA: sin dato de pendiente la direccion queda INTACTA (no se inventa una costa)");
    }

    // ── (4) LA ROMPIENTE: ¿pliega ahora la cresta? Se barre el fondo y se mira el escarpado total,
    //        que es `1 − jacobiano minimo`. Pasar de 1 ES el pliegue.
    {
        double peor = 0.0; float peorD = 0.0f; int pliegan = 0;
        float dMin = 1e9f, dMax = 0.0f;
        for (int i = 0; i < 400; ++i) {
            const float d = 0.05f + (float)i * 0.05f;
            const float green = Haruka::World::Water::oceanGreenGain(d) * Haruka::World::Water::oceanFetchFactor(st, Haruka::World::Water::WATER_FETCH_UNLIMITED);
            const float brk   = Haruka::World::Water::oceanBreakScale(d, st);
            const float ss    = Haruka::World::Water::oceanSteepScale(d, st);
            const float curl  = Haruka::World::Water::oceanCurlGain(d, st);
            double sum = 0.0;
            for (int j = 0; j < Haruka::World::Water::OCEAN_WAVES; ++j) {
                const float k0 = 6.2831853f / st.wave[j][0];
                const float k  = Haruka::World::Water::oceanWaveNumber(k0, d);
                const float amp = st.wave[j][1] * green * brk * Haruka::World::Water::oceanShortWaveFade(k);
                // En la cresta `cos φ = 1`, que es donde el adelanto es maximo.
                sum += (double)k * Haruka::World::Water::oceanHorizAmp(k, amp, d) * ss * (1.0 + curl);
            }
            if (sum > 1.0) { ++pliegan; dMin = std::min(dMin, d); dMax = std::max(dMax, d); }
            if (sum > peor) { peor = sum; peorD = d; }
        }
        std::printf("    escarpado total en la CRESTA, 5 cm..20 m de fondo: maximo %.3f (a %.2f m)\n"
                    "    LA FRANJA QUE ROMPE: de %.2f m a %.2f m de fondo (%d de 400 muestras)\n",
                    peor, peorD, dMin, dMax, pliegan);
        // La ola de referencia mide 2,8 m de Hs, y una ola rompe con H/d ~ 0,78 → alrededor de 3,6 m
        // de fondo. Si la franja cayera en centimetros, romperia en la orilla misma y no se veria.
        // Y donde dice el INDICE que deberia romper, que es contra lo que se compara la franja.
        float dGamma = 0.0f;
        for (int i = 400; i >= 1; --i) {
            const float d = (float)i * 0.05f;
            if (Haruka::World::Water::oceanBreakIndex(d, st) >= 0.78f) { dGamma = d; break; }
        }
        std::printf("    el INDICE de rompiente cruza 0,78 a %.2f m de fondo — ahi es donde debe romper\n", dGamma);
        CHECK(dMax > dGamma * 0.5f && dMin < dGamma * 1.5f,
              "la franja que pliega cae DONDE el indice dice que rompe, no en la orilla");
        CHECK(peor > 1.0, "la cresta AHORA se pliega: el escarpado pasa de 1 (antes el maximo era 0,818)");
        CHECK(pliegan > 0 && pliegan < 400,
              "y pliega en una FRANJA, no en todas partes (una ola que rompe siempre no es una ola)");
    }

    // ── (5) EL MAR ABIERTO NO CAMBIA. Es la mitad del arreglo: si el adelanto tocara el agua honda,
    //        el oceano entero tendria la cresta torcida.
    {
        const float curlHondo = Haruka::World::Water::oceanCurlGain(400.0f, st);
        const float ssHondo   = Haruka::World::Water::oceanSteepScale(400.0f, st);
        std::printf("    a 400 m de fondo: adelanto de cresta %.6f · escala de escarpado %.6f\n",
                    curlHondo, ssHondo);
        CHECK(curlHondo < 1e-4f, "en agua honda NO hay adelanto de cresta (el mar abierto no cambia)");
        CHECK(std::fabs(ssHondo - 1.0f) < 1e-4f, "ni recorte de escarpado");
    }

    // ── (6) ⚠️ EL AGUA INTERIOR, que es lo que rompio el intento anterior. Una charca de 30 cm sobre
    //        celdas de 4,4 m: el adelanto es una FRACCION del semieje, y el semieje escala con la
    //        amplitud, asi que una ola de centimetros se adelanta centimetros.
    {
        const float d = 0.30f;
        const float green = Haruka::World::Water::oceanGreenGain(d) * Haruka::World::Water::oceanFetchFactor(st, 200.0f);
        const float brk   = Haruka::World::Water::oceanBreakScale(d, st, 200.0f);
        const float ss    = Haruka::World::Water::oceanSteepScale(d, st, 200.0f);
        const float curl  = Haruka::World::Water::oceanCurlGain(d, st, 200.0f);
        float total = 0.0f;
        for (int j = 0; j < Haruka::World::Water::OCEAN_WAVES; ++j) {
            const float k0 = 6.2831853f / st.wave[j][0];
            const float k  = Haruka::World::Water::oceanWaveNumber(k0, d);
            const float amp = st.wave[j][1] * green * brk * Haruka::World::Water::oceanShortWaveFade(k);
            total += Haruka::World::Water::oceanHorizAmp(k, amp, d) * ss * (1.0f + curl);
        }
        std::printf("    charca de 30 cm CON la rompiente puesta: vaiven horizontal %.3f m (celda 4,4 m)\n",
                    total);
        CHECK(total < 4.4f,
              "la charca sigue sin desplazarse mas que su celda (el adelanto es fraccion de la amplitud)");
    }

    // ── (7) Y que la altura sigue siendo finita y acotada por el fondo en todo el barrido.
    {
        bool ok = true; float peorH = 0.0f, peorD = 0.0f;
        for (int i = 0; i < 200; ++i) {
            const float d = 0.05f + (float)i * 0.1f;
            const glm::vec3 wp(37.0f, 0.0f, 11.0f);
            const float h = oceanWaveHeight(wp, up, 5.5f, d, 1.0f, st, Haruka::World::Water::WATER_FETCH_UNLIMITED, costa);
            if (!std::isfinite(h)) ok = false;
            if (std::fabs(h) > peorH) { peorH = std::fabs(h); peorD = d; }
        }
        std::printf("    altura maxima en el barrido: %.3f m (a %.2f m de fondo) · toda finita: %s\n",
                    peorH, peorD, ok ? "si" : "NO");
        CHECK(ok, "la altura es finita en todo el barrido (con refraccion y rompiente puestas)");
    }
}
