/**
 * @file cloud_vol.frag
 * @brief NUBES VOLUMÉTRICAS: el cúmulo bajo como medio participativo, no como telón.
 *
 * ── QUÉ CAMBIA RESPECTO AL CIELO ────────────────────────────────────────────────────────────────
 * El cúmulo se pintaba en `sky.frag`, que es un pase de FONDO: sin profundidad, antes que la
 * escena. Eso tenía dos consecuencias que no se arreglan retocando el fondo:
 *   · cualquier objeto de la escena tapaba la nube, estuviera donde estuviera;
 *   · **no existía un "dentro"** — un telón no tiene interior, así que atravesar una nube era
 *     imposible por construcción.
 * Aquí la nube se RECORRE. Si el rayo empieza dentro de la losa, la marcha empieza dentro y la
 * pérdida de visibilidad sale de la propia integración: no hay ningún efecto de "blanqueo" que
 * programar ni activar.
 *
 * ── CÓMO ────────────────────────────────────────────────────────────────────────────────────────
 * La nube es la cáscara entre las esferas de radio R+base y R+techo. Se corta el rayo contra las
 * dos, se recorre el tramo y se acumula PROFUNDIDAD ÓPTICA; la opacidad sale de Beer-Lambert. La
 * marcha se corta contra la profundidad de la escena, así que una montaña delante tapa la nube de
 * detrás — que es justo lo que el pase de fondo no podía hacer.
 *
 * ⚠️ La profundidad se lee de una COPIA del depth de la escena (`u_sceneDepth`, volcada con
 * `blitDepth`), no del target donde se escribe: samplear la misma imagen a la que se dibuja es un
 * bucle de realimentación. Es el mismo patrón que ya usa el fluido para ocluir sus partículas.
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require
#include "lib/cloud_volume.glsl"

layout(location = 0) in vec3 vRayDir;
layout(location = 1) in vec2 vNdc;
layout(location = 0) out vec4 FragColor;

layout(std140, binding = 5) uniform CloudParams {
    // ⚠️ UNA SOLA INVERSA, Y ES LA DE ROTACIÓN. La escena entera se dibuja CÁMARA-RELATIVA: el UBO
    // de frame lleva `view = mat4(mat3(view))`, sin traslación. Reconstruir la profundidad con la
    // vista COMPLETA daba un punto desplazado la posición absoluta de la cámara —~6,37e6 m sobre un
    // planeta—, la distancia salía siempre enorme, el rayo no se recortaba nunca contra la escena y
    // **la nube se pintaba por encima del terreno**. Con la misma matriz que rasterizó el depth,
    // el punto reconstruido está en el mismo espacio y la comparación tiene sentido.
    mat4  u_invViewProjRot;
    vec4  u_planetC;   // xyz = centro del planeta relativo a la cámara · w = radio
    vec4  u_slab;      // x = base · y = techo · z = cobertura · w = precipitación
    vec4  u_sun;       // xyz = hacia el Sol · w = elevación
    vec4  u_sunColor;  // rgb = color · a = día[0,1]
    vec4  u_wind;      // xy = deriva · z = tiempo · w = altitud del ojo
    vec4  u_misc;      // x = atmósfera · y = pasos · z = escala del campo · w = EXTINCIÓN por metro
    // EMBUDOS. Mismo layout que `CloudParams` en application_render.cpp (hay static_assert allí).
    vec4  u_vortexPos[4];   // xyz = punto de SUELO del eje, relativo a la cámara · w = radio del núcleo
    vec4  u_vortexInfo[4];  // x = techo (m) · y = viento (m/s) · z = giro · w = 1 tromba / 0 tornado
    vec4  u_vortexN;        // x = cuántos · y = tiempo · z = ángulo de UN PÍXEL del target (rad) · w = máscara de capas (diagnóstico)
};

layout(binding = 0) uniform sampler2D u_sceneDepth;

/// ── LA COBERTURA, POR DIRECCION ────────────────────────────────────────────────────────────────
///
/// ⚠️ ERA UN ESCALAR PARA TODA LA PANTALLA (`u_slab.z`), muestreado en el punto que la camara tiene
/// DEBAJO. A ras de suelo se nota poco: lo que ves cabe en unas decenas de km y el tiempo no cambia
/// tanto. Desde orbita es otra cosa — a 557 km ves miles de kilometros de planeta, y todos recibian
/// el tiempo de UN punto. Por eso no podia parecer un planeta desde el espacio: no hay frentes, hay
/// un valor. Reportado mirando la pantalla ("desde orbita deberian de verse") y confirmado con la
/// linea del log: `cobertura=0.10 -> SE DIBUJA`, o sea que el pase corria y pintaba un 10 % de nube
/// uniforme sobre el hemisferio entero.
///
/// Ahora la cobertura viaja como una equirect pequeña (`u_coverTex`) horneada del MISMO
/// `WeatherSystem` que ya decide el tiempo — el shader sigue sin inventar cuanta nube hay. `u_slab.z`
/// pasa a ser el MAXIMO del planeta, y solo se usa para la salida rapida: si en ningun sitio hay
/// nube, no hay nada que marchar.
layout(binding = 1) uniform sampler2D u_coverTex;
/// LAS CAPAS ALTAS, horneadas aparte y DESFASADAS del frente: x = cirro (frentes adelantados
/// `kCirrusLeadS`), y = nivel medio (frentes atrasados), z = humedad que ve el cielo. Ver
/// `WeatherSystem::bakeSky`. Sin textura (1x1) no hay capas altas: el escalar no las conoce.
layout(binding = 2) uniform sampler2D u_skyHiTex;

vec4 skyHiAtDir(vec3 dirFromCenter) {
    const ivec2 sz = textureSize(u_skyHiTex, 0);
    if (sz.x <= 1 || sz.y <= 1) return vec4(0.0, 0.0, 0.5, 0.0);
    const vec3 d = normalize(dirFromCenter);
    const vec2 uv = vec2(0.5 + atan(d.z, d.x) * 0.1591549,
                         0.5 - asin(clamp(d.y, -1.0, 1.0)) * 0.3183099);
    return texture(u_skyHiTex, uv);
}

/// EL CIELO en una direccion (desde el centro del planeta): (cobertura, base m, techo m, precip).
/// Convencion equirect del motor. Sin textura (forzado de diagnostico) valen los escalares locales.
///
/// ⚠️ ANTES ERA SOLO LA COBERTURA. Base, techo y lluvia venian de UN punto (el de la camara) para
/// todo lo visible: una unica losa en todo el planeta, y las capas altas a alturas FIJAS con una
/// fraccion de la cobertura. O sea, nubes por CAPAS. Ahora cada punto lee la nube que ESE punto
/// tiene segun el clima: fina y baja en buen tiempo, torre de 6 km donde descarga.
vec4 skyAtDir(vec3 dirFromCenter) {
    const ivec2 sz = textureSize(u_coverTex, 0);
    if (sz.x <= 1 || sz.y <= 1) return vec4(u_slab.z, u_slab.x, u_slab.y, u_slab.w);
    const vec3 d = normalize(dirFromCenter);
    const vec2 uv = vec2(0.5 + atan(d.z, d.x) * 0.1591549,
                         0.5 - asin(clamp(d.y, -1.0, 1.0)) * 0.3183099);
    return texture(u_coverTex, uv);
}

/// LA HUELLA DEL EMBUDO EN SU NUBE. Un tornado no cuelga del cielo azul: cuelga de un cumulonimbo,
/// y justo encima tiene la NUBE DE PARED, una base rebajada y oscura de un par de km. Aqui se
/// imprime eso sobre lo que diga el clima: dentro de `kStormR` la cobertura se cierra y la torre
/// sube; dentro de `kWallR` la base baja. Es lo que hace que el embudo "utilice" las nubes en vez
/// de convivir con ellas — y que se vea la tormenta ANTES que el embudo, que es como se ve venir.
/// `p` es la posicion respecto al CENTRO del planeta (el marco de la marcha).
vec4 harukaStormImprint(vec4 sky, vec3 p) {
    const int n = int(u_vortexN.x);
    for (int k = 0; k < 4; ++k) {
        if (k >= n) break;
        const vec3  G = u_vortexPos[k].xyz - u_planetC.xyz;
        const float top = u_vortexInfo[k].x;
        if (top <= 1.0) continue;
        const vec3  a  = normalize(G);
        const vec3  q  = p - G;
        const float dH = length(q - a * dot(q, a));      // distancia horizontal al eje
        const float kStormR = 6000.0, kWallR = 1800.0;
        const float storm = 1.0 - smoothstep(kStormR * 0.5, kStormR, dH);
        if (storm <= 0.0) continue;
        const float wall = 1.0 - smoothstep(kWallR * 0.3, kWallR, dH);
        sky.x = max(sky.x, storm);                                    // cielo cerrado
        sky.z = max(sky.z, mix(sky.z, sky.y + 6500.0, storm));        // la torre
        sky.y = mix(sky.y, top * 0.55, wall);                         // la nube de pared
        sky.w = max(sky.w, 0.8 * storm);                              // y descarga
    }
    return sky;
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// UNA CASCARA DE NUBE, MARCHADA. Se saco a funcion para poder recorrer VARIAS.
//
// ⚠️ POR QUE. El cirro (8 km) y el altocumulo (4 km) los pintaba `sky.frag`, que es un pase de FONDO:
// sin profundidad, sin poder componerse sobre la escena, y con la puerta `t > 0.02` que solo deja
// mirar HACIA ARRIBA. Consecuencia medida: al pasar de 4 km el altocumulo **se esfuma**, al pasar de
// 8 el cirro, y nunca se ven por debajo de ti. Reportado como "se desvanecen con la subida de altura
// como si estuvieran pegados al fondo" — porque lo estaban.
//
// Aqui las tres capas son cascaras de verdad: se cortan contra el rayo, se recortan contra la
// profundidad de la escena y se componen encima. Se pueden atravesar y se ven desde arriba, que es lo
// que `cloud_vol.frag` ya sabia hacer para el cumulo (tiene el caso `hitBase && b0 > 0` que invierte
// el orden de entrada al mirar desde encima de la capa).
//
// ⚠️ EL PRESUPUESTO DE PASOS NO SUBE: los 64 de antes se REPARTEN. El cumulo es el cuerpo y se lleva
// la mayoria; las dos altas son velos finos y con pocos pasos ya quedan resueltas.
//
// Las globales evitan una funcion con quince parametros; se fijan una vez en `main`.
vec3  g_ro, g_dir;
vec2  g_wind;
float g_sunUp, g_jit, g_sceneT;

/// La deriva del viento, EN EL MARCO DEL PUNTO y no en el de la cámara.
///
/// ⚠️ Sacarla del ojo (que es de donde salían `g_e1`/`g_e2`) volvería a atar el cielo al jugador por
/// la puerta de atrás: sería un desplazamiento pequeño, pero del mismo tipo que el bug que se acaba
/// de quitar. Aquí el este/norte se construyen a partir de `dn` —la vertical DEL PUNTO—, así que
/// dependen sólo de dónde está la nube. Cerca de los polos el `cross` con Y degenera y se cambia de
/// eje de referencia; ahí la dirección de la deriva da un giro, pero es una animación, no la forma.
vec3 harukaWindAt(vec3 dn) {
    const vec3 ref  = abs(dn.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    const vec3 east = normalize(cross(ref, dn));
    return east * g_wind.x + cross(dn, east) * g_wind.y;
}

/// `mode`: 0 = la losa CONVECTIVA (base y techo POR PUNTO, de la textura) · 1 = altocumulo ·
///         2 = cirro/yunque. Los modos 1 y 2 no son "capas": son nubes que salen de CONDICIONES
///         del punto (ver `harukaCoverFor`) y se apagan del todo donde no se dan.
/// Devuelve LA COBERTURA que usa cada modo en ese punto. Los modos 1 y 2 no son "capas": leen su
/// propio campo, desfasado del frente, y se apagan del todo donde no se dan las condiciones.
///
/// ⚠️ ANTES SALIAN DE LA MISMA COBERTURA QUE EL CUMULO (una fraccion de ella, con puertas). Con
/// cielo poco cubierto solo existia la capa baja — "solo hay una capa". El cirro real va POR
/// DELANTE del frente y el nivel medio POR DETRAS: son otros sitios del mapa, no otra fraccion.
float harukaCoverFor(int mode, vec4 sky, vec4 hi, vec3 p) {
    const int mask = (u_vortexN.w > 0.5) ? int(u_vortexN.w) : 7;   // HARUKA_CLOUD_MODES (diagnóstico)
    if ((mask & (1 << mode)) == 0) return 0.0;
    const float cover = sky.x, thick = sky.z - sky.y, precip = sky.w;
    if (mode == 1) {
        // NIVEL MEDIO (altocumulo/altostrato): el aire que el frente deja detras, mas PARCHES de
        // buen tiempo (los borreguillos de una tarde cualquiera: no hacen falta frentes, solo
        // humedad en altura). Se lo come la conveccion profunda (con torre grande hay cumulonimbo).
        const float manchas = smoothstep(0.50, 0.76, harukaCloudVNoise3(p * 0.00003 + vec3(31.0, 5.0, 17.0)));
        // ⚠️ NO va con la humedad de SUPERFICIE (`hi.z`): la humedad en altura no es la del suelo,
        // y atarlo a ella dejaba el cielo de una llanura seca sin una sola nube alta. 0,55 y no
        // 0,40: con el remapeo por fuerza (celda floja = celda baja y pequeña) una cobertura de
        // 0,1 deja un 2 % de cielo con altocumulo desde encima; un campo de borreguillos es 0,3-0,5.
        const float fair  = 0.55 * manchas * (0.6 + 0.4 * hi.z);
        return max(hi.y * 0.6, fair) * (1.0 - smoothstep(3500.0, 6000.0, thick));
    }
    if (mode == 2) {
        // CIRRO: (a) la antesala del frente, cientos de km por delante; (b) el YUNQUE, la cima de
        // la tormenta que se desparrama — va CON la lluvia; (c) bandas sueltas de buen tiempo,
        // a escala de cientos de km, que es la escala real del cirro (jirones de un chorro).
        const float ahead = hi.x * 0.7;
        const float anvil = smoothstep(0.30, 0.75, precip) * 0.85;
        const float manchas = smoothstep(0.50, 0.78, harukaCloudVNoise3(p * 0.000012 + vec3(3.0, 41.0, 9.0)));
        const float fair  = 0.45 * manchas * (0.6 + 0.4 * hi.z);
        return max(max(ahead, anvil), fair);
    }
    return cover;
}

void harukaMarchShell(float baseA, float topA, float fscale, float extMul, int mode, int steps,
                      inout float optical, inout float lit, inout float wsum)
{
    const float R = u_planetC.w;
    const float thick = topA - baseA;
    if (thick <= 1.0) return;

    // Tramo dentro de la cascara: entre la esfera de la base y la del techo. Mismo criterio que
    // tenia el cumulo, incluido el caso de mirar desde ENCIMA (ahi el orden se invierte).
    float b0, b1, t0, t1;
    const bool hitBase = harukaRaySphere(g_ro, g_dir, R + baseA, b0, b1);
    const bool hitTop  = harukaRaySphere(g_ro, g_dir, R + topA,  t0, t1);
    if (!hitTop) return;
    float tEnter = hitBase ? max(b1, 0.0) : max(t0, 0.0);
    float tExit  = max(t1, 0.0);
    if (hitBase && b0 > 0.0) { tEnter = max(t0, 0.0); tExit = max(b0, 0.0); }
    if (g_sceneT < 1.0e7) tExit = min(tExit, g_sceneT);   // el terreno tapa lo que hay detras
    if (tExit <= tEnter) return;

    const float kNearStep = 50.0, kGrowth = 1.09;
    float dt = (tExit - tEnter) / float(steps);
    float growth = 1.0;
    if (dt > kNearStep) { dt = kNearStep; growth = kGrowth; }

    // ── PASO ADAPTATIVO: grande en el aire, fino en la nube ─────────────────────────────────────
    // ⚠️ Con el paso creciendo un 9 % por muestra, a 2,5 km ya mide 280 m. Un cúmulo de buen tiempo
    // tiene 300 m y es TRANSLÚCIDO (alfa 0,3-0,5): por él pasan entre una y cinco muestras según
    // el jitter de cada píxel, y el alfa sale distinto en cada uno — GRANO por todo el cuerpo (27 %
    // de píxeles a más de 10/255 de su media 3x3, medido en el banco con el horneado real). Las
    // nubes densas lo disimulan porque saturan en dos muestras; las finas lo enseñan entero.
    // Dentro de una losa el paso se acorta a una fracción de SU espesor; fuera sigue creciendo. El
    // presupuesto de iteraciones sube (x3) pero el coste real no: en el aire se salta igual que
    // antes y en la nube densa `optical > 4` corta enseguida.
    // ⚠️ TOPE AL PASO: un tercio de la longitud de onda FUNDAMENTAL del campo (1,1 km en el cúmulo,
    // 4,6 km en el cirro). Sin tope, mirando bajo el paso pasaba de 400 m a los 3 km y seguía
    // creciendo a kilómetros; la fundamental —que no puede fundirse por paso sin convertir el cielo
    // en neblina— se muestreaba una vez por paso y salía alias por píxel en las TRES capas (banco a
    // 25° sobre el horizonte: 58/71/48 % de grano por capa). El presupuesto de iteraciones sube (x3)
    // sólo para los rayos que se quedan dentro de la banda, que son los del horizonte.
    const float dtMax = (1.0 / (1.3 * fscale)) / 3.0;
    dt = min(dt, dtMax);
    float t = tEnter + g_jit * dt;
    // Las capas altas, vistas desde DENTRO de su banda, tienen el rayo entero dentro de la losa: con
    // pasos de 120 m el presupuesto x3 se agotaba a 9 km y mas alla no habia nada. x6 para ellas.
    const int budget = steps * ((mode == 0) ? 3 : 6);
    for (int i = 0; i < budget && t < tExit; ++i) {
        float stepNow = dt;
        // Lo que abarca UN píxel del target que se está dibujando, en radianes. Viene del UBO porque
        // depende de la resolución del target (1/4 en el juego, 64 px en el banco) y una constante
        // aquí mentía en cuanto cambiaba: el LOD y el paso adaptativo se calibran con esto.
        const float kPixelRad = (u_vortexN.z > 1.0e-5) ? u_vortexN.z : 0.0045;
        const vec3  p = g_ro + g_dir * t;
        const float alt = length(p) - R;

        // EL CLIMA DE ESTE PUNTO, no el de la camara. La losa convectiva usa SU base y SU techo;
        // las altas usan su banda fija pero se apagan por condicion.
        const vec4 sky = harukaStormImprint(skyAtDir(p), p);
        const vec4 hi  = (mode == 0) ? vec4(0.0) : skyHiAtDir(p);
        float lBase = baseA, lTop = topA;
        // ── CADA NUBE A SU ALTURA, EN LAS TRES CAPAS ────────────────────────────────────────
        // ⚠️ Reportado desde el aire: "todos en la misma capa, sin variación de altura" y luego
        // "las de arriba se aplanan". La base y el techo salen de la textura del clima, que es la
        // MISMA para ~150 km, y las capas altas eran bandas FIJAS: campos de nubes con la cima
        // cortada a la misma cota, como bandejas. En un cielo real la base del cúmulo es común (el
        // nivel de condensación es una cota) pero varía algo, y el DESARROLLO no lo es en absoluto:
        // al lado de un cúmulo de 300 m hay uno que sube 2 km; y el altocúmulo y el cirro viven en
        // bandas de kilómetros, no en una losa de 600 m. Un ruido de baja frecuencia a escala de
        // CELDA (unas pocas nubes) mueve base y grosor de cada una. Suave a propósito: un hash por
        // celda dejaría escalones donde dos nubes se tocan.
        const float cv = harukaCloudVNoise3(p * fscale * 0.31 + vec3(7.1, 3.3, 9.7));
        if (mode == 0) {
            lBase = sky.y * (0.85 + 0.30 * cv);
            // ⚠️ El factor bajo era 0,45 y dejaba losas de 280 m: con el 22 % de techo local de una
            // nube floja, 62 m de nube — GRANO. El desarrollo varía entre 0,7x y 2x, y la losa no
            // baja del espesor mínimo resoluble.
            lTop  = min(lBase + max((sky.z - sky.y) * (0.70 + 1.3 * cv * cv), HARUKA_CLOUD_MIN_THICK_M * 1.6), topA);
        } else {
            // Bandas altas: la nube ocupa un tramo VARIABLE dentro de la banda [baseA, topA], no
            // la banda entera. `span` = grosor de esta nube; `pos` = dónde cae dentro de la banda.
            const float band = topA - baseA;
            // ⚠️ El cirro tenia 0,18-0,40 de banda (576-1280 m) con 12 pasos sobre 3,2 km: cada rayo
            // lo cruzaba en cero o un paso y salia como MOTAS blancas por todo el cielo. Grosor y
            // pasos van juntos: la capa tiene que tener varios pasos dentro.
            // ⚠️ El cirro tenia 0,35-0,70 de banda (1,1-2,2 km) y con extincion 0,00176/m es un
            // muro (espesor optico 2-4): desde dentro de su banda el banco daba el 100 % del cuadro
            // blanco. Ahora 0,10-0,25 (320-800 m), que con la densidad fibrosa da 0,2-1 de espesor
            // optico. Los pasos ya no van con el grosor de la losa sino con el rasgo del campo.
            const float span = band * ((mode == 1) ? (0.30 + 0.35 * cv) : (0.10 + 0.15 * cv));
            const float pos  = harukaCloudVNoise3(p * fscale * 0.19 + vec3(51.0, 13.0, 27.0));
            lBase = baseA + (band - span) * pos;
            lTop  = lBase + span;
        }
        const float lThick = lTop - lBase;
        if (lThick <= 1.0) { t += stepNow; dt = min(dt * growth, dtMax); continue; }

        // ── EL SOLAPE ANALÍTICO DEL PASO CON LA LOSA ─────────────────────────────────────────
        // ⚠️ Antes: `f = (alt − base)/espesor; si f ∉ (0,1) siguiente`. Con un paso de 400 m y una
        // capa de 700 m mirando bajo, que la MUESTRA caiga dentro es cara o cruz por píxel (el
        // jitter decide): moteado por todo el cielo en el nivel medio y el cirro, medido en el juego
        // con `HARUKA_CLOUD_MODES` (el cúmulo, denso y grueso, salía limpio). Una capa no se
        // muestrea: se INTEGRA. Cada paso aporta exactamente la longitud que recorre dentro de la
        // losa —el solape del tramo de altitudes que cubre con [base, techo]— y la densidad se
        // evalúa a media altura de ese solape. Con eso, dónde caiga la muestra deja de importar.
        const float dAlt = dot(g_dir, p / (R + alt));               // cuánto sube el rayo por metro
        const float a1 = alt + dAlt * stepNow;
        const float aLo = min(alt, a1), aHi = max(alt, a1);
        const float ovLo = max(aLo, lBase), ovHi = min(aHi, lTop);
        if (ovHi <= ovLo) { t += stepNow; dt = min(dt * growth, dtMax); continue; }
        const float pathIn = min((abs(dAlt) > 1.0e-3) ? (ovHi - ovLo) / abs(dAlt) : stepNow, stepNow);
        const float f = ((ovLo + ovHi) * 0.5 - lBase) / lThick;
        // Dentro de la losa el paso se acorta a una fracción de su espesor (la densidad varía dentro),
        // sin bajar de dos píxeles de huella; lejos, afinar más de lo que el píxel resuelve sólo
        // agota el presupuesto y deja el resto del rayo sin marchar.
        // ⚠️ Y NUNCA MAS LARGO QUE UN QUINTO DE LA SEGUNDA OCTAVA del campo: el LOD funde a la media
        // toda octava que no cubra 3-6 pasos, y con el paso a un cuarto de la losa (180-390 m en el
        // altocumulo, cuya segunda octava mide 615 m) esa octava se fundia ENTERA: el campo quedaba
        // en una octava, sd 0,092 en vez de 0,103, y con el umbral de entonces el altocumulo no
        // existia. El cumulo tiene cuatro octavas y su detalle aparte; a el no le hace falta.
        // ⚠️ Y EL CUMULO TAMBIEN, a la escala de su DETALLE (125 m → ~75 m): desde el suelo el paso
        // dentro de la losa era 50 m (nunca crecia), pero viniendo de ARRIBA llegaba a la losa ya
        // crecido a 250-366 m: el perfil se evaluaba en 3-4 alturas por nube y el detalle (LOD por
        // paso) se apagaba entero — cada nube salia como TERRAZAS apiladas ("capas de cada nube
        // cuando estas subiendo"). Banco, cumulo de lado desde 5,5 km: coherencia de bordes 1,37.
        const float finoM = (mode == 0) ? (1.0 / (fscale * 11.4)) * 0.6 : (1.0 / (fscale * 1.3 * 2.0)) / 5.0;
        stepNow = min(dt, max(max(min(lThick / 4.0, finoM), 20.0), t * kPixelRad * 2.0));
        // ⚠️ Y DE CERCA, PROPORCIONAL A LA DISTANCIA: 50 m por paso a 200 m de una nube son cuatro
        // rodajas por nube, y con el jitter por pixel y la B-spline del compuesto salen como capas
        // apiladas ("cuando estas subiendo, cada nube se ve en capas"). Un 6 % de la distancia: a
        // 200 m, 12 m; a 1 km, 60 m (el tope de antes). Solo cuesta a quien vuela entre nubes.
        if (mode == 0) stepNow = min(stepNow, max(t * 0.06, 6.0));
        t += stepNow;
        // ⚠️ EL PASO BASE CRECE TAMBIEN AQUI. Solo crecia en las salidas tempranas (fuera de la losa),
        // asi que un rayo que iba POR DENTRO de una banda —desde dentro de ella, o rasante desde el
        // suelo— se quedaba en 50 m por paso y agotaba el presupuesto a 3-7 km: mas alla, nada. Es
        // el "contra mas arriba se ocultan": desde la banda media el banco daba 0,0 % de altocumulo
        // mirando a 10 grados. El acortamiento dentro de la losa sigue siendo `stepNow`, por paso.
        if (mode != 0) dt = min(dt * growth, dtMax);

        const float coverP = clamp(harukaCoverFor(mode, sky, hi, p), 0.0, 1.0);
        // DIAGNOSTICO (HARUKA_CLOUD_MODES | 8): pinta la LOSA entera donde hay cobertura, sin campo.
        // (| 16): el campo sin remapeo ni fibras. Para bisecar "la capa no sale" sin adivinar: asi
        // se vio que el altocumulo a 0,0 % era el POLO del banco (una celda de manchas), no la marcha.
        const int dbg = (u_vortexN.w > 0.5) ? int(u_vortexN.w) : 0;
        if ((dbg & 8) != 0 && mode != 0) {
            if (coverP > 0.01) { lit += coverP; wsum += 1.0; optical += 0.2 * coverP; }
            continue;
        }
        if (coverP <= 0.01) continue;
        const float lo = harukaCloudThreshold(coverP, (mode == 0) ? HARUKA_CLOUD_SIGMA_CUMULO : HARUKA_CLOUD_SIGMA_CAPA);

        // ⚠️ EL DOMINIO ES LA POSICION, NO UNA PROYECCION SOBRE EL MARCO DE LA CAMARA. Aquí estaba
        // el "las nubes se mueven contigo": ver la nota larga en `lib/cloud_volume.glsl`.
        const vec3  pf = p * fscale;
        // El cúmulo con el campo completo; las capas con el limitado en banda (ver `harukaCloudFieldLayer`).
        const vec3  wv = harukaWindAt(p / (R + alt));
        // Huella de ESTA muestra: el mayor entre el paso dado y el píxel a esta distancia. Nada del
        // campo por debajo de eso (ver `harukaCloudFbm3LOD`).
        const float fpPix = t * kPixelRad;              // sólo el píxel: para la FORMA
        const float fpM   = max(stepNow, fpPix);        // píxel o paso: para el detalle
        const float fp0U  = fpPix * fscale, fpU = fpM * fscale;
        // ⚠️ SIN antialiasing del umbral. Se probaron dos (asimétrico y simétrico) y los dos inflan
        // la cobertura lejos: con cobertura 0,09 el umbral (0,55) está en la COLA del campo (media
        // ~0,4), y ensanchar el borde admite mucho más por debajo de lo que quita por encima —
        // cielo blanco al horizonte, medido en el juego. El antialiasing correcto de un umbral es
        // la esperanza de la función escalón bajo la huella, y eso no sale de un smoothstep.
        float sN = harukaCloudStrength((mode == 0) ? harukaCloudField3LOD(pf, wv, fp0U, fpU)
                                                   : harukaCloudFieldLayer(pf, wv, fp0U, fpU), lo);
        // Cuando ni la FUNDAMENTAL cabe en el píxel (desde órbita: 1 km por píxel contra rasgos de
        // 1,4 km) el campo ya es su media, y umbralizar la media no es la media de lo umbralizado:
        // el planeta salía sin nubes. Lo que ese píxel ve es la COBERTURA. Va por huella de PÍXEL,
        // no de paso: desde el suelo no entra hasta ~60 km (por paso entraba a 2 km y dejaba el
        // cielo entero en neblina).
        const float wFund = 1.0 - smoothstep(1.0 / 6.0, 1.0 / 3.0, fp0U * 1.3);
        sN = mix(coverP, sN, wFund);
        if (sN <= 0.0) continue;

        const vec3  q    = pf * 11.4;
        // Cúmulo: crece con su fuerza. Capas altas: llenan su losa (ver `harukaLayerDensity`).
        // LOD del detalle: la misma huella (ver `harukaCloudDetailLOD`).
        // ⚠️ El ALTOCUMULO ya no es lamina. `harukaLayerDensity` es un perfil vertical por un campo
        // horizontal: la misma seccion de la base al techo, un PRISMA, y visto de canto o desde
        // encima una bandeja. El altocumulo es un campo de CELDAS con panza y cima, como el cumulo
        // pero pequeño: el mismo remapeo por fuerza (una celda floja es baja, una fuerte llena su
        // losa), con el suelo de espesor minimo que quita el grano. El cirro es lo unico que sigue
        // siendo lamina, porque LO ES: fina, translucida y con fibras (ver `harukaCirrusDensity`).
        float dens;
        if ((dbg & 16) != 0 && mode != 0) {
            dens = sN;
        } else if (mode == 2) {
            // ⚠️ Las fibras van en coordenadas SOBRE LA ESFERA (longitud y latitud por R, en
            // metros), no en un marco tangente del punto: en el marco del punto, el punto esta en el
            // origen —`dot(p, este(p))` es 0 SIEMPRE— y el primer intento daba un cirro sin una
            // sola fibra (banco desde encima: algodon liso, sd de luminancia 38). Zonales, como los
            // chorros que las peinan; cerca de los polos la parametrizacion se aprieta, y da igual.
            const vec3 dn = p / (R + alt);
            const float lonM = atan(dn.z, dn.x) * R;
            const float latM = asin(clamp(dn.y, -1.0, 1.0)) * R;
            // ⚠️ NO TODAS EN LA MISMA DIRECCION. Zonales puras, desde el suelo convergen al punto de
            // fuga como railes: "una flecha, muy artificial". El rumbo de las fibras gira con un
            // ruido a escala de ~150 km (±60 grados): cada region tiene el suyo, como los chorros.
            const float ang = (harukaCloudVNoise3(vec3(lonM, latM, 0.0) * 6.7e-6 + vec3(2.0, 8.0, 5.0)) - 0.5) * 2.1;
            const float ca = cos(ang), sa = sin(ang);
            dens = harukaCirrusDensity(sN, f, lonM * ca + latM * sa, -lonM * sa + latM * ca, fpPix);
        } else {
            dens = harukaCloudDensity(sN, f, q, HARUKA_CLOUD_MIN_THICK_M / lThick, 1.0 / (fscale * 11.4), fpM);
        }
        if (dens <= 0.0) continue;

        const float ext = u_misc.w * extMul;   // el velo alto extingue MUCHO menos que el cumulo
        // ── COBERTURA Y DENSIDAD NO SON LO MISMO EN UN PASO GRUESO ──────────────────────────
        // ⚠️ Era `optical += dens · paso · ext`. Con paso de 300 m (lejos, mirando al horizonte)
        // eso satura aunque la nube ocupe el 10 % de la muestra: `exp(-0,1·300·0,008)` = 0,79 por
        // paso, y en cuatro pasos blanco puro. Cada píxel lejano salía "toca nube / no toca", sin
        // gradiente: 54 % de grano a 25° sobre el horizonte, medido. La transmitancia de una muestra
        // que la nube llena en una fracción `c` es `1 − c·(1 − e^{−τ})`, no `e^{−c·τ}`: la parte
        // vacía deja pasar la luz entera. `sN` es esa fracción y `dens/sN` la densidad de la nube
        // donde la hay. Cerca, con `sN` en 0 o 1, es Beer-Lambert de siempre.
        const float frac  = clamp(sN, 0.0, 1.0);
        const float inner = dens / max(frac, 1.0e-3);
        const float tStep = 1.0 - frac * (1.0 - exp(-inner * min(pathIn, stepNow) * ext));
        optical += -log(max(tStep, 1.0e-4));

        const float localTopM = lBase + lThick * mix(HARUKA_CLOUD_MINTOP, 1.0, sN);
        const float widthM = 1.0 / fscale;
        const float toSun  = min(max(localTopM - alt, 0.0) / g_sunUp, widthM);
        const float shadow = exp(-toSun * ext * 0.25 * sN);

        // La tormenta oscurece POR PUNTO: un cumulonimbo cargado no es blanco, y el claro de al
        // lado si lo es. Antes esto era un escalar de la camara aplicado a todo el cielo.
        const float dark = mix(1.0, 0.55, clamp(sky.w, 0.0, 1.0));
        lit  += (0.18 + 0.82 * shadow) * dens * dark;
        wsum += dens;
        if (optical > 4.0) return;
    }
}

/// ── EL EMBUDO ──────────────────────────────────────────────────────────────────────────────────
///
/// Un tornado se VE porque el aire que gira condensa: la presión cae en el núcleo y el vapor se hace
/// nube. Así que el embudo es nube, se marcha con las mismas cuentas y se compone en el mismo pase.
/// Su forma sale de la física que ya está en `WeatherSystem::vortexWindAt`: estrecho abajo (donde
/// la succión es fuerte y la presión más baja) y abierto arriba, hasta fundirse con la base de la
/// nube madre. Y en la base del suelo, lo que levanta: polvo (tornado) o agua (tromba).
///
/// ⚠️ ES UNA REGIÓN APARTE de las cáscaras: vive ENTRE el suelo y la base de la nube, donde
/// `harukaMarchShell` no pisa nunca. Por eso tiene su propio marchado y su propio recinto.
void harukaMarchFunnels(inout float optical, inout float lit, inout float wsum, inout float dust, inout float manga)
{
    const int n = int(u_vortexN.x);
    const float time = u_vortexN.y;
    for (int k = 0; k < 4; ++k) {
        if (k >= n) break;
        const vec3  G    = u_vortexPos[k].xyz - u_planetC.xyz;  // suelo del eje, respecto al CENTRO
        const float core = u_vortexPos[k].w;
        const float top  = u_vortexInfo[k].x;
        const float wind = u_vortexInfo[k].y;
        const float spin = u_vortexInfo[k].z;
        const float water = u_vortexInfo[k].w;
        if (core <= 0.0 || top <= 1.0 || wind <= 0.0) continue;

        const vec3  a  = normalize(G);          // el eje es la vertical LOCAL del punto
        const float Rb = core * 2.4;            // radio del recinto: la manga abre hasta ~1,6 núcleos y el polvo hasta ~2,2

        // Recinto: la esfera que envuelve el cilindro (radio Rb, altura top). Un rayo que no la toca
        // no paga nada — que es casi todos los píxeles de casi todos los frames.
        float s0, s1;
        const vec3  cen = G + a * (top * 0.5);
        const float rad = sqrt(top * top * 0.25 + Rb * Rb);
        if (!harukaRaySphere(g_ro - cen, g_dir, rad, s0, s1)) continue;
        float tEnter = max(s0, 0.0);
        float tExit  = max(s1, 0.0);
        if (g_sceneT < 1.0e7) tExit = min(tExit, g_sceneT);
        if (tExit <= tEnter) continue;

        // ⚠️ PASOS SEGÚN EL NÚCLEO, no un número fijo. Con 28 pasos sobre el recinto (~1,3 km) el
        // paso era ~90 m contra una manga de 100 m: con jitter salía grano, sin jitter salían ANILLOS
        // (la marcha en pantalla). Ninguna de las dos es la nube. El paso tiene que ser una fracción
        // del núcleo, y el jitter entero para que lo que quede sea ruido y no estructura.
        const int   steps = int(clamp((tExit - tEnter) / (core * 0.22), 24.0, 96.0));
        const float dt = (tExit - tEnter) / float(steps);
        float t = tEnter + g_jit * dt;
        const float ext = u_misc.w * 4.0;       // el embudo es DENSO: se lee como cuerpo, no como velo
        const float strength = clamp(wind / 70.0, 0.35, 1.3);

        for (int i = 0; i < steps; ++i, t += dt) {
            const vec3  p  = g_ro + g_dir * t;
            const vec3  q  = p - G;
            const float h  = dot(q, a);
            if (h < 0.0 || h > top) continue;
            const vec3  rv = q - a * h;
            const float d  = length(rv);
            const float hn = h / top;

            // La MANGA: radio que crece con la altura (estrecho en el suelo, abierto en la nube).
            // Cuadrático porque un embudo se ensancha despacio abajo y deprisa cerca de la base.
            const float rF = core * mix(0.35, 1.6, hn * hn);
            // Giro: el ruido se evalúa en coordenadas que ROTAN con el vórtice, así la manga se ve
            // girar en vez de ser un cono quieto. La velocidad angular sale del viento en el núcleo.
            const float ang  = atan(dot(rv, cross(a, vec3(0.0, 0.0, 1.0))), dot(rv, cross(cross(a, vec3(0.0, 0.0, 1.0)), a)) + 1e-6);
            const float omega = spin * wind / max(core, 1.0);
            const vec3  swirl = vec3(d * 0.05, h * 0.004 - time * 0.6, ang * 3.0 - time * omega * 0.25);
            const float noise = harukaCloudDetail(swirl);
            float dens = smoothstep(rF, rF * 0.45, d) * (0.85 + 0.25 * noise) * strength;
            // Se funde con la nube madre por arriba: sin esto hay un corte seco justo en la base.
            dens *= smoothstep(0.0, 0.08, 1.0 - hn) + smoothstep(0.92, 1.0, hn) * 0.6;

            // LO QUE LEVANTA. Un anillo BAJO y algo más ancho que la manga: polvo/escombros en tierra,
            // rociones en el mar.
            // ⚠️ ERA `hn < 0,22` (540 m de alto con la base a 2470) y 3,2 núcleos de radio: un bloque
            // de 770 × 540 m, MÁS GRANDE QUE LA MANGA, que en la captura tapaba medio cielo y —al
            // ir teñido de tierra— hacía que el tornado entero pareciera de arena. La nube de
            // escombros real tiene decenas de metros de alto, no cientos: va en METROS, no en
            // fracción de la columna, porque la altura de la nube madre no tiene nada que ver.
            const float ringH = clamp(core * 0.9, 40.0, 140.0);
            const float ring = smoothstep(ringH, ringH * 0.15, h) * smoothstep(core * 2.2, core * 0.6, d)
                             * (0.45 + 0.55 * noise) * strength * 0.45;
            dens += ring;
            if (dens <= 0.0) continue;

            optical += dens * dt * ext;
            // Luz: la manga es un cuerpo grueso y está a la sombra de su propia nube → oscura. El
            // polvo del suelo recibe luz de lado y se ve más claro y más cálido.
            // (la manga sale OSCURA: es un cuerpo grueso bajo su propia nube; en la captura con
            //  0,10 + 0,25·ruido se leía blanca contra el cielo)
            lit  += (0.03 + 0.12 * noise) * dens + 0.30 * ring;
            wsum += dens;
            dust  += ring * (1.0 - water) * 0.6;   // tinte terroso sólo en tierra
            manga += dens - ring;                  // cuánto de lo atravesado era la manga (oscura)
            if (optical > 4.0) return;
        }
    }
}

void main()
{
    const float coverMax = u_slab.z;
    // (con un embudo cerca hay nube aunque el clima diga cielo limpio: la imprime el propio vortice)
    if (coverMax <= 0.01 && u_vortexN.x < 1.0) { FragColor = vec4(0.0); return; }

    const vec3  dir   = normalize(vRayDir);
    const vec3  pc    = u_planetC.xyz;
    const float R     = u_planetC.w;
    if (R <= 1.0) { FragColor = vec4(0.0); return; }

    // ── CORTE CONTRA LA ESCENA, UNA VEZ PARA LAS TRES CAPAS ─────────────────────────────────────
    // Sin esto la nube se pintaria por delante del terreno. Se deshace la profundidad a posicion de
    // mundo (camara-relativa) y su modulo es la distancia a la que hay geometria.
    float sceneT = 1.0e30;
    {
        const float d = texture(u_sceneDepth, vNdc * 0.5 + 0.5).r;
        vec4 wp = u_invViewProjRot * vec4(vNdc, d, 1.0);
        if (abs(wp.w) > 1e-9) {
            const float st = length(wp.xyz / wp.w);
            // Con reversed-Z el "vacio" es 0.0 y reproyecta lejisimos: se descarta por su tamaño.
            if (st < 1.0e7) sceneT = st;
        }
    }

    const vec3 ro  = -pc;                       // el ojo, respecto al CENTRO del planeta
    const vec3 up0 = normalize(ro);

    // JITTER: con pocos pasos, arrancar todos los rayos en el mismo sitio pone la estructura de la
    // marcha EN PANTALLA (anillos concentricos). Desplazar el arranque lo convierte en ruido.
    g_jit = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    g_ro = ro; g_dir = dir;
    // ⚠️ AQUI SE CONSTRUIA EL MARCO TANGENTE DE LA CAMARA (`g_e1`/`g_e2`) Y ERA EL BUG: el campo se
    // indexaba proyectando cada punto sobre él, así que el cielo viajaba con el jugador. Ya no hay
    // marco que construir — el campo se indexa por la posición. Ver `lib/cloud_volume.glsl`.
    g_wind  = u_wind.xy;
    g_sunUp = max(dot(up0, u_sun.xyz), 0.10);
    g_sceneT = sceneT;

    float optical = 0.0, lit = 0.0, wsum = 0.0;

    // ── LAS TRES CAPAS, TODAS VOLUMETRICAS ──────────────────────────────────────────────────────
    //
    // ⚠️ EL CIRRO Y EL ALTOCUMULO ESTABAN EN `sky.frag`, EN EL PASE DE FONDO. Eso los hacia un telon:
    // sin profundidad, sin componerse sobre la escena, y con una puerta que solo permitia mirar hacia
    // arriba — asi que al subir por encima de 4 km (o de 8) desaparecian y nunca se veian por debajo.
    // Aqui son cascaras de verdad y se pueden atravesar.
    //
    // Alturas y caracter de la atmosfera real. El TAMAÑO de elemento es lo que las distingue tanto
    // como la altura: un cirro son jirones de kilometros, un altocumulo borreguillos de ~1 km.
    // Y la EXTINCION es lo que las hace velo o cuerpo: un cirro deja pasar el sol, un cumulo no.
    //
    //   capa          altura        elemento   extincion   pasos
    //   cumulo        del clima     1/u_misc.z    1.00       40   <- el cuerpo, se lleva la mayoria
    //   altocumulo    4,0-4,6 km    1,2 km        0.55       12
    //   cirro         8,0-8,9 km    6,0 km        0.22       12   <- velo
    //
    // El presupuesto total sigue siendo 64 pasos: se reparte, no se suma.
    // ⚠️ LA COBERTURA SE REPARTE, NO SE REPITE. `cloudCover` del clima dice "que fraccion de cielo
    // tiene nube", UNA vez. Dandole 0,85 y 0,75 a las capas altas (los valores que traian de
    // `sky.frag`, donde se dibujaban por separado), lo tapado sale `1-(1-c1)(1-c2)(1-c3)` — con 0,10
    // pedido el cielo se cerraba al triple, y `cloud_volume_draws` lo casco. En un cielo de verdad
    // las capas altas cubren bastante menos que el cumulo: son velos, no cuerpos.
    // Los pasos de la capa principal los MANDA EL MOTOR (`kCloudSteps`). Fijarlos a 40 aqui dejaba
    // el alcance rasante en 50·(1,09^40−1)/0,09 ≈ 17 km en vez de los ≈137 km de 64 pasos: al mirar
    // al horizonte la capa se cortaba a media distancia.
    // La losa convectiva marcha la BANDA GLOBAL (base minima..techo maximo del planeta) y dentro
    // cada punto se queda con su base y su techo. Las otras dos no son capas: son condiciones.
    harukaMarchShell(u_slab.x, u_slab.y, u_misc.z, 1.00, 0,
                     int(clamp(u_misc.y, 16.0, 96.0)), optical, lit, wsum);
    // ⚠️ Las bandas son ANCHAS (2,4 km y 3,2 km) y cada nube ocupa un tramo variable de ellas; con
    // 600 m y 900 m fijos las capas altas salían como sábanas a una cota. El altocúmulo es un
    // CUERPO (extinción 0,80, elemento de 1,6 km): antes iba a 0,55 y se leía como velo, "distinto
    // de las nubes normales". El cirro sí es velo y se queda en 0,22.
    harukaMarchShell(3400.0,   5800.0,   1.0/1600.0, 0.80, 1, 24, optical, lit, wsum);
    harukaMarchShell(6800.0,  10000.0,   1.0/6000.0, 0.22, 2, 24, optical, lit, wsum);

    // Y los embudos, si hay alguno cerca (casi nunca: la salida por recinto es inmediata).
    float dust = 0.0, manga = 0.0;
    harukaMarchFunnels(optical, lit, wsum, dust, manga);

    if (optical <= 0.0001) { FragColor = vec4(0.0); return; }

    // Beer-Lambert: la opacidad sale de la profundidad óptica, no de una media. Atravesar más nube
    // tapa MÁS — que es lo que distingue un volumen de una calcomanía.
    const float alpha = 1.0 - exp(-optical);
    const float light = (wsum > 0.0) ? lit / wsum : 0.5;

    // Color: sombra fría en la panza, luz cálida del sol en la cima.
    const float day    = u_sunColor.a;
    const vec3  cShad  = mix(vec3(0.28, 0.31, 0.38), vec3(0.55, 0.57, 0.63), day);
    const vec3  cLit   = mix(vec3(0.55, 0.58, 0.66), u_sunColor.rgb * 1.05 + 0.35, day);
    vec3 col = mix(cShad, cLit, light);

    // (el oscurecido de tormenta va POR PUNTO dentro de la marcha, ya no es un escalar global)
    // El polvo que levanta un tornado es tierra, no nube: tinte terroso en proporción a cuánto de lo
    // atravesado era polvo. En el mar `dust` es 0 y la tromba queda del color de la nube.
    if (wsum > 0.0) {
        // La MANGA es oscura: un cuerpo grueso que se ve a contraluz de su propia nube. Con el color
        // de sombra de la nube (0,55 de gris a pleno día) salía blanca contra el cielo en la captura.
        col *= mix(1.0, 0.38, clamp(manga / wsum, 0.0, 1.0));
        col  = mix(col, vec3(0.42, 0.34, 0.24) * (0.5 + 0.5 * day), clamp(dust / wsum, 0.0, 0.8));
    }

    FragColor = vec4(col, alpha * clamp(u_misc.x, 0.0, 1.0));
}
