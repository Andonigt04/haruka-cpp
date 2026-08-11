/**
 * @file sky.frag
 * @brief Cielo procedural: gradiente cénit→horizonte por elevación solar,
 *        resplandor de amanecer/atardecer, disco + halo del sol, y estrellas
 *        que aparecen de noche y en el espacio. Se mezcla a negro espacial
 *        según el grosor de atmósfera (u_atmo: 1 en superficie, 0 en órbita).
 *
 * Pase de fondo: se dibuja tras limpiar, SIN escribir profundidad, de modo
 * que el terreno/objetos se pintan encima.
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require
#include "lib/sky_palette.glsl"

layout(location = 0) in vec3 vRayDir;
layout(location = 0) out vec4 FragColor;

// UBO compartido con sky.vert (binding 5) — antes uniforms sueltos (glUniform3fv/1f), que NO
// existen en Vulkan → van por UBO (ruta PSO/RHI). Declaración IDÉNTICA a la de sky.vert (GLSL lo
// exige entre etapas del mismo programa). Truco std140: vec3 (align 16, size 12) + float siguiente
// comparten el mismo slot de 16 B → por eso van emparejados.
layout(std140, binding = 5) uniform SkyParams {
    mat4  u_invViewProjRot; // inv(proj * mat4(mat3(view))) — lo usa el vertex
    vec3  u_sunDir;         // hacia el Sol (mundo, normalizado)
    float u_sunElev;        // dot(sunDir, up): elevación del sol
    vec3  u_up;             // cénit local del observador (mundo)
    float u_atmo;           // 1=superficie ... 0=espacio
    vec3  u_sunColor;       // color de la luz solar
    float u_time;           // segundos (movimiento de nubes)
    vec4  u_weather;        // x=humedad · y=tempC · z=precipitación · w=COBERTURA de nube (del mundo)
    vec4  u_wind;           // x=este(m/s) · y=norte(m/s) · z=racha · w=1 si es NIEVE
    vec4  u_planet;         // x=radio(m) · y=altitud del ojo(m) · z=base de nube(m)
};

// ── DÓNDE CORTA EL RAYO UNA CAPA DE NUBE A ALTITUD `h` ──────────────────────────────────────────
//
// Devuelve las coordenadas sobre la capa, en KILÓMETROS, y en `outOk` si el rayo la alcanza.
//
// ⚠️ Esto es lo que sustituye a `tangente / (t + 0.22)`, que era un plano SIN altitud: todas las
// capas se comprimían igual hacia el horizonte, así que dos capas a alturas distintas se movían
// idénticas y no había paralaje entre ellas. Con la intersección real, una capa baja barre deprisa
// sobre tu cabeza y un cirro apenas se mueve — que es lo que hace que el cielo tenga PROFUNDIDAD.
//
// Geometría: esfera de radio `R + h` centrada en el planeta, ojo a `R + camAlt` sobre el eje `up`.
// Se resuelve la cuadrática y se toma la raíz positiva (la capa está por encima del observador).
// Al ras del horizonte la distancia se dispara, así que se acota: más allá la nube es una franja
// borrosa y seguir estirando la coordenada solo produce aliasing.
vec2 cloudPlane(vec3 dir, vec3 e1, vec3 e2, float h, out bool outOk) {
    outOk = false;
    float R = u_planet.x;
    if (R <= 0.0) {
        // Sin planeta (menús, tests): se conserva el domo de siempre para no dejar el cielo vacío.
        float t = max(dot(dir, u_up), 0.0);
        outOk = true;
        return vec2(dot(dir, e1), dot(dir, e2)) / (t + 0.22) * 4.0;
    }
    float ro    = R + max(u_planet.y, 0.0);     // radio del ojo
    float rc    = R + h;                        // radio de la capa
    float mu    = dot(dir, u_up);               // coseno respecto al cénit
    float disc  = ro * ro * (mu * mu - 1.0) + rc * rc;
    if (disc < 0.0) return vec2(0.0);           // el rayo no llega a la capa
    float dist  = -ro * mu + sqrt(disc);        // raíz positiva: la capa está arriba
    if (dist <= 0.0) return vec2(0.0);
    dist = min(dist, 6.0 * max(h, 1.0));        // acotar el rasante (ver arriba)
    vec3  hit   = dir * dist;                   // punto en la capa, relativo al ojo
    outOk = true;
    return vec2(dot(hit, e1), dot(hit, e2)) * 0.001;   // a km
}

// Hash 3D barato para el campo de estrellas.
float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// Value-noise + fBm para las NUBES pintadas (anime).
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash13(vec3(i, 0.0));
    float b = hash13(vec3(i + vec2(1.0, 0.0), 0.0));
    float c = hash13(vec3(i + vec2(0.0, 1.0), 0.0));
    float d = hash13(vec3(i + vec2(1.0, 1.0), 0.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) { v += a * vnoise(p); p *= 2.0; a *= 0.5; }
    return v;
}

// Densidad de nube: DOMAIN WARP (deforma las coords con otro ruido → formas orgánicas billowy, no blobs) +
// erosión de detalle en los bordes (desgarrados). `wind` = deriva temporal.
float cloudField(vec2 p, vec2 wind) {
    vec2  warp = vec2(fbm(p * 0.6 + wind * 0.5), fbm(p * 0.6 + 5.2 - wind * 0.5));
    float d    = fbm(p * 1.3 + warp * 1.4 + wind);
    d -= 0.20 * fbm(p * 4.0 - wind * 2.0);
    return d;
}

void main()
{
    // Estado POR PÍXEL: depende de la entrada de vértice y del UBO, así que vive aquí y no en
    // lib/sky_palette.glsl (ese fichero es gemelo del de CPU y no puede leer uniforms).
    vec3  dir    = normalize(vRayDir);
    float t      = dot(dir, u_up);                    // 1=cénit, 0=horizonte, <0 bajo el horizonte
    float sunAmt = max(dot(dir, u_sunDir), 0.0);

    // --- Paleta + gradiente base: COMPARTIDOS con el ambiente por SH (lib/sky_palette.glsl) ---
    // Todo lo que viene después (resplandor, halo, nubes, estrellas) es solo del dibujado.
    float day = harukaSkyDay(u_sunElev);
    vec3  sky = harukaSkyBase(t, day);

    // --- Resplandor cálido de amanecer/atardecer ---
    // Máximo con el sol cerca del horizonte; concentrado en su acimut y abajo.
    float twilight = exp(-(u_sunElev * u_sunElev) / 0.020)
                   * smoothstep(-0.25, 0.05, u_sunElev);
    float glowAz   = pow(sunAmt, 4.0);                 // pegado al acimut del sol
    float glowLow  = 1.0 - smoothstep(0.0, 0.35, abs(t)); // ceñido al horizonte
    sky = mix(sky, vec3(0.95, 0.45, 0.22), twilight * glowAz * glowLow * 0.9);

    // Halo difuso alrededor del sol — SOLO dentro de la atmósfera (dispersión de Mie). En el
    // espacio (atmo=0) no hay halo → así el Sol se ve IGUAL dentro y fuera (su cuerpo es la
    // corona-objeto emisiva; el cielo ya NO dibuja un disco solar aparte que se veía distinto
    // contra cielo azul vs negro = "dos soles/diferentes").
    sky += u_sunColor * pow(sunAmt, 8.0) * 0.35 * day * u_atmo;

    // Con LLUVIA el cielo se CUBRE y agrisa (overcast) → el azul soleado desaparece.
    sky = mix(sky, vec3(0.55, 0.58, 0.63) * (0.65 + 0.35 * day), u_weather.z * 0.7 * u_atmo);

    // --- NUBES cinematográficas: VOLUMEN (domain warp + auto-sombra en varios pasos hacia el sol = gradiente
    //     luz→sombra "de película"), ESCALA por zonas (un campo de masa de baja frecuencia → cúmulos grandes,
    //     zonas despejadas y puffs pequeños), CICLO del planeta y SILVER-LINING a contraluz. Plano tangente
    //     LOCAL (es un planeta → "arriba" = u_up). ---
    if (u_atmo > 0.5 && t > 0.02) {
        vec3 e1 = normalize(cross(u_up, abs(u_up.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
        vec3 e2 = cross(u_up, e1);
        // DERIVA = el viento del CLIMA (m/s en este/norte), no una constante. Es el mismo vector que
        // inclina la lluvia y ondea el follaje: la nube va hacia donde sopla.
        vec2 windBase = u_wind.xy * 0.00035 * u_time;
        float rain    = u_weather.z;
        float snowy   = u_wind.w;

        // ⚠️ LA COBERTURA VIENE DEL MUNDO (`WeatherSystem`), NO de un fbm(u_time) local. El shader
        // no decide CUÁNTA nube hay —eso lo dice el frente que está pasando— solo qué FORMA tiene.
        float coverW = clamp(u_weather.w, 0.0, 1.0);

        // ── TRES CAPAS A ALTITUDES REALES ───────────────────────────────────────────────────────
        //
        // Antes había UNA sola, proyectada sobre un plano sin altitud. Con capas a alturas distintas
        // la intersección rayo–esfera les da PARALAJE: la baja barre deprisa sobre tu cabeza, la
        // alta apenas se mueve, y entre ellas se ve profundidad en vez de un solo papel pintado.
        //
        // Las alturas y el carácter son los de la atmósfera real, que es de donde sale que se lean
        // como nubes distintas y no como el mismo ruido tres veces:
        //   cúmulo    1,5 km — grande, compacto, muy contrastado. Es el que da el "pop" de anime.
        //   altocúmulo 4 km  — borreguillos medianos, contraste medio.
        //   cirro      8 km  — velo fino y estirado, casi sin sombra propia.
        //
        // El bucle está DESENROLLADO a propósito: cada capa tiene su escala, su umbral y su color, y
        // un bucle con arrays de constantes se compila peor y se lee peor.
        vec3  sunT = normalize(u_sunDir - dot(u_sunDir, u_up) * u_up);
        vec2  ls2  = vec2(dot(sunT, e1), dot(sunT, e2));

        // ⚠️ EL CÚMULO (capa 2) YA NO SE PINTA AQUÍ. Lo dibuja el pase VOLUMÉTRICO
        // (`cloud_vol.frag`), después de la escena y con la profundidad a mano. El motivo no es
        // estético: aquí era FONDO —sin depth, antes que todo— y por tanto un telón que cualquier
        // objeto tapaba y que **no tenía interior**, así que atravesar una nube era imposible por
        // construcción. El cirro (8 km) y el altocúmulo (4 km) se quedan: están tan alto que nunca
        // se cruzan, y como fondo cuestan una fracción de lo que costaría marcharlos.
        // `u_planet.w` es el CONMUTADOR, y su convención es explícita: 0 = el pase volumétrico se
        // encarga del cúmulo (aquí solo se pintan las dos capas altas); > 0 = no hay pase
        // volumétrico y ese valor ES el techo del cúmulo, que se pinta plano como antes. Así apagar
        // `m_volumetricClouds` no deja el cielo sin cúmulos.
        int nLayers = (u_planet.w > 1.0) ? 3 : 2;
        for (int layer = 0; layer < nLayers; ++layer)
        {
            float h, scale, sharp, thick, windMul, shadeMul, covMul;
            if (layer == 0) {        // CIRRO (se pinta primero: está detrás de todo)
                h = 8000.0; scale = 0.085; sharp = 0.30; thick = 0.55;
                windMul = 0.35; shadeMul = 0.25; covMul = 0.75;
            } else if (layer == 1) { // ALTOCÚMULO
                h = 4000.0; scale = 0.16;  sharp = 0.14; thick = 0.80;
                windMul = 0.65; shadeMul = 0.70; covMul = 0.85;
            } else {                 // CÚMULO (delante, el protagonista)
                h = 1500.0; scale = 0.30;  sharp = 0.07; thick = 1.00;
                windMul = 1.00; shadeMul = 1.00; covMul = 1.00;
            }

            // La base de los CÚMULOS la pone el clima (la misma que el techo de la lluvia), no una
            // constante: así la panza de la nube y el punto donde nacen las gotas coinciden.
            if (layer == 2 && u_planet.z > 1.0) h = u_planet.z;

            bool ok;
            vec2 pk = cloudPlane(dir, e1, e2, h, ok);
            if (!ok) continue;
            vec2 base = pk * scale;
            vec2 wind = windBase * windMul;

            // MASA a baja frecuencia: varía cobertura por zonas (grandes cúmulos ↔ despejado).
            float mass = fbm(base * 0.30 + wind * 0.25);
            float cloudiness = coverW * covMul
                             * smoothstep(0.16, 0.72, mass + 0.26 + 0.5 * rain);

            float lo   = mix(0.70, 0.32, cloudiness);

            // ── ESPESOR REAL: LOSA EN VEZ DE LÁMINA (solo el cúmulo) ────────────────────────────
            //
            // ⚠️ ESTE ERA EL MOTIVO DE QUE SE VIERAN PLANAS. La intersección rayo–esfera da paralaje
            // ENTRE capas, pero dentro de cada una el campo seguía siendo 2D: una calcomanía sobre
            // un cascarón. Sin espesor no hay auto-oclusión, ni silueta que se hinche, ni ese borde
            // que se engorda al mirarlo de canto. Se veía plano porque era plano.
            //
            // Ahora la capa baja se muestrea a VARIAS ALTITUDES entre la base y la cima. Como el rayo
            // es oblicuo, cada altitud corta la losa en un punto horizontal distinto —eso es paralaje
            // DENTRO de la nube— y la densidad acumulada a lo largo de la vista es la que decide
            // cuánto tapa. Mirando hacia arriba se atraviesa poco espesor y la nube es tenue; hacia
            // el horizonte se atraviesan kilómetros y se vuelve sólida, que es como se comportan.
            //
            // Cuatro pasos, no dieciséis: esto corre a pantalla completa. Lo que hace falta para que
            // deje de leerse como calcomanía es que el espesor EXISTA, no que sea exacto.
            float dens;
            if (layer == 2) {
                const int  STEPS = 4;
                // CIMA DEL CÚMULO: la trae el CLIMA (`WeatherSample::cloudTopM`), no una constante.
                // Estaba cableada a `h + 1700`, y eso daba el mismo desarrollo vertical a una capa
                // de buen tiempo que a una tormenta — justo lo que distingue a las dos. Medido en el
                // modelo: ~430 m sin lluvia, ~3850 m descargando (8,9x). El fallback se conserva por
                // si el UBO llega sin clima (planeta sin `WeatherSystem` configurado).
                float top = (u_planet.w > h + 1.0) ? u_planet.w : h + 1700.0;
                float acc = 0.0, wsum = 0.0;
                for (int sI = 0; sI < STEPS; ++sI) {
                    float f  = (float(sI) + 0.5) / float(STEPS);
                    float hs = mix(h, top, f);
                    bool  ok2;
                    vec2  ps = cloudPlane(dir, e1, e2, hs, ok2);
                    if (!ok2) continue;
                    // Perfil vertical: densa en el medio, deshilachada en la panza y en la cima. Es
                    // lo que da la forma de coliflor en vez de un ladrillo.
                    float prof = smoothstep(0.0, 0.28, f) * (1.0 - smoothstep(0.62, 1.0, f));
                    acc  += cloudField(ps * scale, wind) * prof;
                    wsum += prof;
                }
                // ⚠️ NO SE PROMEDIA. La primera versión hacía `acc / wsum`, o sea la MEDIA de las
                // cuatro altitudes, y eso aplana los picos: el campo se suaviza, muchos menos puntos
                // cruzan el umbral y salen POCAS nubes y TENUES. Promediar una losa da lo contrario
                // de lo que se busca — atravesar más nube tiene que tapar MÁS, no menos.
                //
                // Lo que acumula una losa es PROFUNDIDAD ÓPTICA: cada tramo suma lo que le sobra al
                // umbral, y la cobertura sale de Beer-Lambert. Así el borde fino sigue siendo fino y
                // el centro grueso se vuelve sólido, que es como se lee el volumen.
                //
                // `wsum` sigue existiendo solo para no depender del número de pasos: la densidad se
                // normaliza por el perfil, no por el conteo.
                dens = (wsum > 0.0) ? acc / wsum : 0.0;
            } else {
                dens = cloudField(base, wind);
            }

            // ⚠️ BORDE DURO = el look de anime. La rampa era `smoothstep(lo, lo + 0.15, dens)` para
            // todas; con `sharp` la nube baja corta en 0,07 y su silueta se lee como una FORMA
            // recortada, no como un degradado de niebla. La alta conserva rampa ancha (0,30) porque
            // un cirro con borde duro no parece un cirro, parece un recorte.
            float cov = smoothstep(lo, lo + sharp, dens);
            cov = cov * cov * (3.0 - 2.0 * cov);
            // CAMINO ÓPTICO: cuanto más rasante es la vista, más losa se atraviesa → más opaca.
            // `t` es el coseno respecto al cénit, así que 1/t es el camino relativo. Se aplica a la
            // COBERTURA y no a la densidad: aplicado a la densidad desplazaba el umbral y cambiaba la
            // FORMA de la nube con el ángulo de vista, que es justo lo que no debe pasar.
            if (layer == 2) {
                float path = clamp(1.0 / max(t, 0.14), 1.0, 2.6);
                cov = 1.0 - pow(max(1.0 - cov, 0.0), path);
            }

            // Ni pegadas al horizonte ni en el cénit exacto. La capa ALTA puede acercarse más al
            // horizonte que la baja: está más lejos, así que su franja visible es más ancha.
            float hb   = mix(0.02, 0.05, thick);
            float band = smoothstep(hb, hb + 0.22, t) * (1.0 - smoothstep(0.62, 1.01, t));
            float cloud = cov * band * day * u_atmo;
            if (cloud <= 0.001) continue;

            // Iluminación volumétrica (Beer): densidad acumulada HACIA el sol → base oscura y cara
            // al sol brillante. El paso se escala con la altura de la capa para que la sombra tenga
            // el tamaño que le toca a su tamaño de nube.
            vec2  ls = ls2 * (0.20 * scale / 0.30);
            float sh = 0.0;
            for (int i = 1; i <= 3; ++i)
                sh += smoothstep(lo, lo + 0.26, cloudField(base + ls * float(i), wind));
            // ⚠️ Beer MÁS AGRESIVO en las capas densas (1,9 → 3,2): es lo que separa el blanco de la
            // cima del azul de la panza. El claroscuro duro es la mitad del look; la otra mitad es
            // el borde. Un cirro no debe tener sombra propia marcada, de ahí `shadeMul`.
            float light = exp(-(sh / 3.0) * (1.9 + 1.3 * shadeMul));

            // Cima blanca-MATE (no 1.0 → no se sobreexpone), base FRÍA en sombra. Con lluvia, gris
            // plomo; con nieve, gris claro y plano (la nevada apaga el relieve del cielo).
            vec3  cLit  = mix(vec3(0.86, 0.88, 0.92), u_sunColor, 0.10 + 0.18 * pow(sunAmt, 2.0));
            vec3  cShad = vec3(0.42, 0.50, 0.68);
            vec3  pLit  = mix(vec3(0.55, 0.57, 0.61), vec3(0.78, 0.79, 0.83), snowy);
            vec3  pShad = mix(vec3(0.30, 0.32, 0.36), vec3(0.63, 0.65, 0.70), snowy);
            cLit  = mix(cLit,  pLit,  rain);
            cShad = mix(cShad, pShad, rain);
            // El contraste luz↔sombra se atenúa con la altura: un cirro es casi plano.
            vec3 cloudCol = mix(cShad, cLit, mix(0.75, light, shadeMul));
            sky = mix(sky, cloudCol, cloud * mix(0.55, 0.95, thick));
        }
    }

    // disc solo para ATENUAR estrellas cerca del Sol (el cuerpo del Sol lo dibuja la corona-objeto).
    float disc = smoothstep(0.9994, 0.9998, dot(dir, u_sunDir));

    // --- Estrellas (noche + espacio) ---
    float starVis = (1.0 - day) * (1.0 - 0.6 * disc); // ocultas por el día y el sol
    if (starVis > 0.001) {
        vec3  cell = floor(dir * 350.0);
        float r    = hash13(cell);
        float star = smoothstep(0.9975, 1.0, r);       // pocas celdas encienden
        float tw   = 0.6 + 0.4 * sin(hash13(cell + 7.0) * 100.0); // titileo
        sky += vec3(star * tw * starVis);
    }

    // --- Mezcla a espacio negro según grosor de atmósfera ---
    vec3 spaceC = vec3(0.004, 0.004, 0.010);
    vec3 outC   = mix(spaceC, sky, u_atmo);
    // (El cuerpo del Sol = corona-objeto emisiva, se dibuja en el pase de objetos sobre este
    //  fondo, igual dentro y fuera de la atmósfera → sin "dos soles".)

    FragColor = vec4(outC, 1.0);
}
