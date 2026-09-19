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
#include "lib/aerial.glsl"
#include "lib/cloud_column.glsl"

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
    vec4  u_aerial;         // perspectiva aérea (lib/aerial.glsl): x = 1/L · y = día
    vec4  u_blend;          // x = peso del horneado NUEVO (bindings 1/2) frente al ANTERIOR (3/4)
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
/// El horneado ANTERIOR de las dos. Cada 2 s de mundo llega uno nuevo y entre dos seguidos un
/// texel puede cambiar el techo 1,4 km y la cobertura 0,05 (medido): cambiarlo de golpe era "las
/// nubes van a tirones". Se mezcla con `u_blend.x`, que sube de 0 a 1 hasta el siguiente.
layout(binding = 3) uniform sampler2D u_coverTexPrev;
layout(binding = 4) uniform sampler2D u_skyHiTexPrev;

vec4 skyHiAtDir(vec3 dirFromCenter) {
    const ivec2 sz = textureSize(u_skyHiTex, 0);
    if (sz.x <= 1 || sz.y <= 1) return vec4(0.0, 0.0, 0.5, 15.0);   // sin horneado: 15 C al nivel del mar
    const vec3 d = normalize(dirFromCenter);
    const vec2 uv = vec2(0.5 + atan(d.z, d.x) * 0.1591549,
                         0.5 - asin(clamp(d.y, -1.0, 1.0)) * 0.3183099);
    return mix(texture(u_skyHiTexPrev, uv), texture(u_skyHiTex, uv), clamp(u_blend.x, 0.0, 1.0));
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
    return mix(texture(u_coverTexPrev, uv), texture(u_coverTex, uv), clamp(u_blend.x, 0.0, 1.0));
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
float g_tsum = 0.0;   // Σ distancia·densidad: la distancia MEDIA de la nube, para el aire de en medio
vec3  g_rad  = vec3(0.0);   // RADIANCIA integrada a lo largo del rayo (premultiplicada por la opacidad)

// ═══════════════════════════════════════════════════════════════════════════════════════════════
//  LA LUZ DE LA NUBE ES TRANSPORTE, NO UN PROMEDIO.
//
//  Antes: `lit/wsum` (una media de "cuanto Sol ve cada muestra") elegia entre un color de sombra y
//  uno de luz escritos a mano, con la sombra hacia el Sol multiplicada por un 0,25 a ojo y un AO por
//  bultos. Ahora la radiancia se INTEGRA por el rayo: cada muestra aporta
//      dα · albedo · [ E_sol · Σᵢ aⁱ·T_sol(bⁱ)·p(cⁱ·g) + E_cielo·Σᵢ aⁱ·T_cima(bⁱ) ]
//  donde dα es lo que esa muestra añade a la opacidad (Beer-Lambert por segmentos, la misma α que se
//  compone), `T_sol` la transmitancia hacia el Sol MARCHADA por el campo (3 muestras), `p` la fase de
//  Henyey-Greenstein de doble lobulo, y la suma en octavas (a = b = c = 0,5, Hillaire 2016) la
//  aproximacion de dispersion multiple: cada octava es luz que ya se disperso una vez mas, con menos
//  extincion y una fase mas isotropa. El ambiente es la radiancia del cielo (la misma paleta que
//  dibuja el domo), atenuada por la nube que hay encima de la muestra. El color final es la
//  radiancia dividida por α (la composicion espera color sin premultiplicar).
//  Lo que sigue sin ser fisico: la extincion `u_misc.w` es una constante (una nube real varia de
//  0,01 a 0,1/m), el albedo se toma 0,98 y la paleta del cielo no es un modelo de atmosfera.
// ═══════════════════════════════════════════════════════════════════════════════════════════════
const float HARUKA_CLOUD_ALBEDO = 0.98;
const float HARUKA_CLOUD_SUN_E  = 0.35;   ///< irradiancia solar en las unidades del resto del render (con 0,55 la cara al Sol ya tocaba la rodilla y la fase no se veia: x1,04)
const float HARUKA_CLOUD_HG_G   = 0.72;   ///< lobulo delantero (agua en gotas, ~0,7-0,85)
const float HARUKA_CLOUD_HG_B   = -0.30;  ///< lobulo trasero
const float HARUKA_CLOUD_HG_MIX = 0.30;   ///< peso del lobulo trasero

/// Henyey-Greenstein normalizada a 1 para isotropa (multiplicada por 4π).
float harukaHG(float c, float g) {
    const float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * c, 1.0e-4), 1.5);
}
float harukaCloudPhase(float c, float g) {
    return mix(harukaHG(c, g), harukaHG(c, HARUKA_CLOUD_HG_B), HARUKA_CLOUD_HG_MIX);
}
/// Σᵢ aⁱ·exp(−τ·bⁱ)·p(cⁱ·g), tres octavas (a = b = c = 0,5).
float harukaMultiScatterSun(float tau, float cosTh) {
    float sum = 0.0, a = 1.0, b = 1.0, c = 1.0;
    for (int i = 0; i < 3; ++i) {
        sum += a * exp(-tau * b) * harukaCloudPhase(cosTh, HARUKA_CLOUD_HG_G * c);
        a *= 0.5; b *= 0.5; c *= 0.5;
    }
    return sum;
}
/// Transmitancia DIFUSA de una losa de albedo ~1 a profundidad optica τ (dos flujos, Meador-Weaver):
/// `1 / (1 + 0,75·(1−g)·τ)`. ⚠️ No es `e^{−τ}`: la luz del cielo entra por toda la superficie y se
/// dispersa muchas veces; con las octavas del haz (`e^{−τ}`, `e^{−τ/2}`...) la panza de un cumulo de
/// 600 m salia al 7 % del cielo — negra en pantalla ("las nubes oscuras", capturas de Andoni). Con
/// dos flujos, τ = 5 → 0,64, que es lo que se mide en una base de cumulo real (0,4-0,7 del cielo).
float harukaDiffuseT(float tau) {
    return 1.0 / (1.0 + 0.75 * (1.0 - HARUKA_CLOUD_HG_G) * max(tau, 0.0));
}
float g_exhaust = 0.0; // 1 si alguna marcha agoto el presupuesto antes de salir de la losa (diagnostico)

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

// ═══════════════════════════════════════════════════════════════════════════════════════════════
//  UNA SOLA MARCHA POR LA COLUMNA DE AIRE
//
//  ⚠️ AQUI HABIA TRES `harukaMarchShell` (cumulo, altocumulo en 3,4-5,8 km, cirro en 6,8-10 km), cada
//  una con su funcion de densidad y su `mode`. Se veian como lo que eran: tres sabanas a tres cotas
//  ("no deberian de mostrarse nubes por capas"). Ahora hay UNA marcha de la base minima al techo de
//  la atmosfera util (13 km) y en cada punto la nube es la FRACCION que da la columna de aire de ese
//  punto (`lib/cloud_column.glsl`): lo convectivo entre su base (nivel de condensacion sobre el
//  suelo) y su techo, y el vapor en altura donde el aire tiene la temperatura a la que condensa.
//  El ASPECTO sale de la fase (agua/hielo por temperatura), no de "que capa es".
//
//  `HARUKA_CLOUD_MODES` (diagnostico): 1 = lo convectivo · 2 = vapor medio · 4 = vapor alto.
// ═══════════════════════════════════════════════════════════════════════════════════════════════
/// Cierra un SEGMENTO de la marcha (ver la nota "COBERTURA Y DENSIDAD" dentro del bucle): la
/// transmitancia del segmento es `1 − c·(1 − e^{−τ})` con `c` la fraccion de nube del segmento y `τ`
/// la profundidad optica DENTRO de la nube. Con `c = 1` (de cerca) es Beer-Lambert de siempre.
/// Longitud de CORRELACION de la cobertura a lo largo del rayo, en celdas de la fundamental: cada
/// tanto se "sortea" nube nueva. 1 = celdas independientes (cierra el horizonte con un 20 % de
/// cobertura: 0,8^32 = 0,001 en la banda de 0,5-3 grados). El campo es un fbm con warp y su
/// correlacion es mayor que una celda; ver la medida en `rhi_test_main` (6-H).
const float HARUKA_CLOUD_CORR_CELLS = 3.0;
/// Transmitancia de la marcha con cobertura CORRELADA, en forma CONTINUA:
///     T = 1 − (1 − P_hueco)·(1 − e^{−τ_in}),   log P_hueco = Σ log(1 − cᵢ)·(sorteosᵢ)
/// donde cada paso aporta `sorteos = ds_h/L + ds_v/H` (una celda horizontal L o el espesor de la
/// capa H en vertical = un sorteo nuevo) y `τ_in` es la profundidad optica DENTRO de la nube.
/// ⚠️ ANTES ERAN SEGMENTOS con frontera cada L (`harukaCloseSeg`): al cerrar cada uno la
/// transmitancia saltaba, y como todos los rayos de una fila entran en la capa a la misma distancia
/// los saltos caian en las mismas distancias → BANDAS RECTAS paralelas al horizonte donde la nube
/// cambiaba de golpe ("se recorta con cortes rectos y parecen mas cerca", Andoni). Sin fronteras no
/// hay bandas; al nadir (ds_h = 0, ds_v = H) es un sorteo: T = 1 − c·(1 − e^{−τ}); de cerca
/// (c = 1) es Beer-Lambert; rasante la union crece con la distancia recorrida.
float harukaCorrT(float logClear, float tauIn)
{
    return 1.0 - (1.0 - exp(logClear)) * (1.0 - exp(-tauIn));
}

void harukaMarchColumn(float baseA, float topA, float fscale, int steps,
                       inout float optical, inout float lit, inout float wsum)
{
    const float R = u_planetC.w;
    if (topA - baseA <= 1.0) return;
    const int mask = (u_vortexN.w > 0.5) ? int(u_vortexN.w) : 7;

    // Tramo dentro de la cascara [baseA, topA], incluido mirar desde ENCIMA (el orden se invierte).
    float b0, b1, t0, t1;
    const bool hitBase = harukaRaySphere(g_ro, g_dir, R + baseA, b0, b1);
    const bool hitTop  = harukaRaySphere(g_ro, g_dir, R + topA,  t0, t1);
    if (!hitTop) return;
    float tEnter = hitBase ? max(b1, 0.0) : max(t0, 0.0);
    float tExit  = max(t1, 0.0);
    if (hitBase && b0 > 0.0) { tEnter = max(t0, 0.0); tExit = max(b0, 0.0); }
    if (g_sceneT < 1.0e7) tExit = min(tExit, g_sceneT);   // el terreno tapa lo que hay detras
    if (tExit <= tEnter) return;

    const float kNearStep = 50.0, kGrowth = 1.09, kGrowth0 = 0.09;
    float dt = (tExit - tEnter) / float(steps);
    float growth = 1.0;
    if (dt > kNearStep) { dt = kNearStep; growth = kGrowth; }
    // Tope al paso: un tercio de la fundamental del campo convectivo (ver la nota historica en git:
    // sin tope, mirando bajo, la fundamental se muestreaba una vez por paso y salia alias por pixel).
    const float dtMax = (1.0 / (1.3 * fscale)) / 3.0;
    dt = min(dt, dtMax);
    float t = tEnter + g_jit * dt;
    // La columna entera desde dentro de ella: el rayo rasante va decenas de km por nube. x6.
    const int budget = steps * 6;
    float fineK = 1.0;
    // La marcha llega SIEMPRE a `tExit`: el paso nunca baja del reparto del presupuesto que queda
    // sobre la distancia que queda (si no, rasante se agotaba a 20-30 km y salia una recta en el
    // horizonte: 8 % de los pixeles, medido en rojo con dbg 64). Y mas alla de 4 L de perspectiva
    // aerea (transmitancia 1,8 %) no se marcha.
    if (u_aerial.x > 1.0e-7) tExit = min(tExit, tEnter + 4.0 / u_aerial.x);
    // Escalas del vapor en altura (el campo de capa, banda limitada): ~1,6 km de rasgo.
    const float fscaleA = 1.0 / 1600.0;
    const float finoC = (1.0 / (fscale * 11.4)) * 0.6;              // detalle del cumulo (~75 m)
    const float finoA = (1.0 / (fscaleA * 1.3 * 2.0)) / 5.0;         // un quinto de la 2a octava del vapor
    // El SEGMENTO de cobertura correlada (ver "COBERTURA Y DENSIDAD" abajo): τ dentro de la nube,
    // fraccion de nube y camino HORIZONTAL recorrido desde que se abrio.
    float logClear = 0.0, tauIn = 0.0;   // ver `harukaCorrT`
    bool  drewConv = false, drewAloft = false;   // ENTRAR en una capa es un sorteo (uno por capa)
    float opticalOld = 0.0;              // (dbg & 1024): el modelo viejo por paso, como contraprueba
    int i = 0;
    for (; i < budget && t < tExit; ++i) {
        const float stepMin = (tExit - t) / float(budget - i);
        float stepNow = max(dt, stepMin);
        const float kPixelRad = (u_vortexN.z > 1.0e-5) ? u_vortexN.z : 0.0045;
        const vec3  p = g_ro + g_dir * t;
        const float alt = length(p) - R;

        // EL CLIMA DE ESTE PUNTO: la columna de aire de su texel.
        const vec4 sky = harukaStormImprint(skyAtDir(p), p);   // x cobertura · y base · z techo · w lluvia
        const vec4 hi  = skyHiAtDir(p);                         // x vapor alto · y vapor medio · z humedad · w T al nivel del mar
        const vec3 upS = p / (R + alt);

        // ── LO CONVECTIVO: cada cumulo con su base y su desarrollo ───────────────────────────
        // Un ruido a escala de CELDA (unas pocas nubes) mueve la base ±100 m y el desarrollo x0,7-2:
        // la base de un cielo es comun (es una cota) pero varia algo; el desarrollo no lo es nada.
        const float cv = harukaCloudVNoise3(p * fscale * 0.31 + vec3(7.1, 3.3, 9.7));
        // LA BASE NO ES UN PLANO. El nivel de condensacion es una cota, pero la base de un cumulo
        // tiene bultos de 100-200 m y cuelga mas bajo su nucleo (mas gotas, mas peso, la corriente
        // ascendente se hace notar): con `base = LCL` exacta se veia una lamina desde abajo
        // (Andoni, dos veces). Ondula con el mismo campo de bultos que la cima (a otra escala) y
        // con la celda (`cv`); el domo/erosion siguen dando el borde.
        const vec3  pfB  = harukaCloudSquashedPos(p, R, alt, fscale);   // (ver la nota en cloud_volume.glsl)
        const float bumpsB = harukaCloudTopBumps(pfB * 0.6 + vec3(2.7, 5.1, 8.3));                  // ~440 m
        const float lBase = sky.y + (cv - 0.5) * 200.0 - (bumpsB - 1.0) * 120.0;   // bultos de ±50 m: la base de un cumulo es CASI plana
        // ⚠️ El DESARROLLO ya no es un ruido por celda (x0,7-2 con `cv²`): "hay nubes grandes que
        // salen planas" — una nube grande cruza varias celdas y la celda con x0,7 le ponia el techo
        // a 0,7 de la losa hiciera lo que hiciera su nucleo. La losa marchada es el doble del espesor
        // convectivo del clima (el maximo que puede subir) y la ALTURA la decide la nube: el domo
        // (`harukaCloudDensity`) sube con la fuerza y con el exceso del nucleo (`harukaCloudTowerMul`).
        const float lTop  = min(lBase + max((sky.z - sky.y) * 2.0, HARUKA_CLOUD_MIN_THICK_M * 1.6), topA);
        const float fracC = ((mask & 1) != 0) ? harukaColConvective(sky.x, lBase, lTop, alt) : 0.0;

        // ── EL VAPOR EN ALTURA: condensa a su temperatura ────────────────────────────────────
        // La temperatura del punto lleva una perturbacion de celda (±3 °C a ~6 km): asi la cota a
        // la que condensa ondula, cada nube a su altura, sin bandas planas.
        const float tCell = (harukaCloudVNoise3(p * fscaleA * 0.19 + vec3(51.0, 13.0, 27.0)) - 0.5) * 6.0;
        const float tC    = harukaColAirTempC(hi.w, alt) + tCell;
        // Borreguillos de buen tiempo: humedad en altura sin frente, a escala de ~150 km (no va con
        // la humedad de superficie: atarla dejaba la llanura seca sin una nube alta). Se los come
        // la conveccion profunda.
        const float manchas  = smoothstep(0.50, 0.76, harukaCloudVNoise3(p * 0.00003 + vec3(31.0, 5.0, 17.0)));
        const float vaporMid = ((mask & 2) != 0) ? max(hi.y * 0.6, 0.55 * manchas * (0.6 + 0.4 * hi.z)) * (1.0 - smoothstep(3500.0, 6000.0, sky.z - sky.y)) : 0.0;
        // Vapor alto: la antesala del frente, el YUNQUE (la cima de la tormenta que se desparrama:
        // va con la lluvia) y jirones sueltos a escala de cientos de km.
        const float manchasH = smoothstep(0.50, 0.78, harukaCloudVNoise3(p * 0.000012 + vec3(3.0, 41.0, 9.0)));
        const float vaporHigh = ((mask & 4) != 0) ? max(max(hi.x * 0.7, smoothstep(0.30, 0.75, sky.w) * 0.85), 0.45 * manchasH * (0.6 + 0.4 * hi.z)) : 0.0;
        const float fracA = harukaColAloft(vaporMid, vaporHigh, tC);

        const bool convective = fracC >= fracA;
        const float coverP = clamp(convective ? fracC : fracA, 0.0, 1.0);
        // La marcha entera a saltos donde no hay nube posible, fino donde la hay.
        const float lThick = convective ? (lTop - lBase) : 900.0;
        const float finoM  = convective ? finoC : finoA;
        const float refK = (((u_vortexN.w > 0.5) ? int(u_vortexN.w) : 0) & 32) != 0 ? 0.25 : 1.0;
        stepNow = min(dt, max(max(min(lThick / 4.0, finoM), 20.0), t * kPixelRad * 2.0) * refK) * fineK;
        stepNow = max(stepNow, stepMin);
        if (convective) stepNow = min(stepNow, max(t * 0.06, 6.0) * refK * fineK);   // de cerca, proporcional
        stepNow = max(stepNow, max(4.0, stepMin));
        t += stepNow;
        dt = min(dt * growth, dtMax);
        // Camino HORIZONTAL de este paso (respecto a la vertical local): es lo que decorrelaciona la
        // cobertura. Un rayo vertical no avanza en horizontal y ve UNA columna; uno rasante cruza
        // una celda nueva cada `widthM`. Se acumula tambien en aire limpio: un hueco cierra el segmento.
        const float cosV  = dot(g_dir, upS);
        const float stepH = stepNow * sqrt(max(1.0 - cosV * cosV, 0.0));
        const float stepV = stepNow * abs(cosV);

        const int dbg = (u_vortexN.w > 0.5) ? int(u_vortexN.w) : 0;
        if ((dbg & 8) != 0) {   // DIAGNOSTICO: pintar la FRACCION, sin campo
            if (coverP > 0.01) { lit += coverP; wsum += 1.0; g_tsum += t; optical += 0.2 * coverP; }
            continue;
        }
        if (coverP <= 0.01) continue;

        // ── EL CAMPO 3D REPARTE LA FRACCION EN NUBES ─────────────────────────────────────────
        const float lo = harukaCloudThreshold(coverP, convective ? HARUKA_CLOUD_SIGMA_CUMULO : HARUKA_CLOUD_SIGMA_CAPA);
        const vec3  wv = harukaWindAt(upS);
        // Huella de ESTA muestra para el LOD: el pixel a esta distancia y el paso NOMINAL (continuo
        // en t; con `stepNow`, que salta, el campo cambiaba de golpe donde el paso cambiaba).
        const float fpPix = t * kPixelRad;
        float fpNom = min(min(kNearStep + kGrowth0 * (t - tEnter), dtMax),
                          max(max(min(lThick / 4.0, finoM), 20.0), t * kPixelRad * 2.0));
        if (convective) fpNom = min(fpNom, max(t * 0.06, 6.0));
        const float fpM   = max(fpNom, fpPix);
        float sN, dens;
        float sNd = 0.0;      // la fuerza con la que se DIBUJA (dentro de la parte nublada)
        float wFund = 1.0;    // 1 = el pixel resuelve la fundamental; 0 = orbita (sN es la fraccion)
        const float ice = harukaColIce(tC);
        float ext = u_misc.w;
        if (convective) {
            const vec3  pf  = p * fscale;
            const float fp0U = fpPix * fscale, fpU = fpM * fscale;
            const vec3  pfC = harukaCloudSquashedPos(p, R, alt, fscale);
            const float fieldC = harukaCloudField3LOD(pfC, wv, fp0U * HARUKA_CLOUD_HORIZ_SCALE, fpU * HARUKA_CLOUD_HORIZ_SCALE);
            const float sNraw = harukaCloudStrengthCumulo(fieldC, lo);
            // Lo que el LOD se ha llevado del campo: con ello, la FRACCION LOCAL de nube de esta
            // muestra y la fuerza media dentro de ella (ver `harukaCloudLocalFraction`). De cerca
            // (nada filtrado) es la fuerza de siempre; desde orbita, la probabilidad local.
            // ⚠️ `sSub` va con la huella de la MUESTRA (pixel Y paso): el campo se filtra con las dos
            // y la fraccion tiene que describir el MISMO campo. Se probo con el pixel solo, para que
            // el paso no apagara la forma en los rayos rasantes: el grano del nivel medio sube de 6 a
            // 12 % y su presencia cae de 32 a 15 % (el campo filtrado por el paso casi no cruza el
            // umbral). Medido y descartado: el manchurron lejano no se arregla por aqui.
            const float sSub = HARUKA_CLOUD_SIGMA_CUMULO * harukaCloudLodSigmaFrac(fp0U * 1.3 * HARUKA_CLOUD_HORIZ_SCALE, fpU * 1.3 * HARUKA_CLOUD_HORIZ_SCALE, 4);
            wFund = 1.0 - smoothstep(0.10, 0.50, sSub / HARUKA_CLOUD_SIGMA_CUMULO);
            if (wFund >= 1.0) { sN = sNraw; sNd = sNraw; }
            else {
                const vec2 lf = harukaCloudLocalFraction(fieldC, lo, sSub);
                sN  = mix(lf.x, sNraw, wFund);
                sNd = mix(harukaCloudStrengthCumulo(lf.y, lo), sNraw, wFund);
            }
            if ((dbg & 1024) != 0) {   // el modelo viejo (contraprueba): la cobertura MEDIA del texel
                const float wOld = 1.0 - smoothstep(HARUKA_CLOUD_LOD0_LO, HARUKA_CLOUD_LOD0_HI, fp0U * 1.3);
                sN = mix(coverP, sNraw, wOld); sNd = sN;
            }
            if (sN <= 0.0) { fineK = min(fineK * 1.6, 1.0); continue; }
            const float f = (alt - lBase) / max(lThick, 1.0);
            // Bultos (coliflor) por la TORRE del nucleo (ver harukaCloudTowerMul): la nube grande sube.
            const float bumps = harukaCloudTopBumps(pfC) * harukaCloudTowerMul(fieldC - lo);
            dens = harukaCloudDensity(sNd, f, pf * 11.4, HARUKA_CLOUD_MIN_THICK_M / lThick, 1.0 / (fscale * 11.4), fpM, bumps);
            // (el AO por bultos y el sombreado por pendiente hacia el Sol se fueron: la cara al Sol y
            // la panza salen ahora de la transmitancia MARCHADA hacia el Sol, ver `g_rad`)
        } else {
            const vec3  pf  = p * fscaleA;
            const float fp0U = fpPix * fscaleA, fpU = fpM * fscaleA;
            const float fieldL = harukaCloudFieldLayer(pf, wv, fp0U, fpU);
            const float sNraw = harukaCloudStrength(fieldL, lo);
            // (el campo de capa devuelve estadisticas de 2 octavas; la sigma sub-huella va con ellas)
            const float sSub = HARUKA_CLOUD_SIGMA_CAPA * harukaCloudLodSigmaFrac(fp0U * 1.3, fpU * 1.3, 2);
            wFund = 1.0 - smoothstep(0.10, 0.50, sSub / HARUKA_CLOUD_SIGMA_CAPA);
            if (wFund >= 1.0) { sN = sNraw; sNd = sNraw; }
            else {
                const vec2 lf = harukaCloudLocalFraction(fieldL, lo, sSub);
                sN  = mix(lf.x, sNraw, wFund);
                sNd = mix(harukaCloudStrength(lf.y, lo), sNraw, wFund);
            }
            if ((dbg & 1024) != 0) {
                const float wOld = 1.0 - smoothstep(HARUKA_CLOUD_LOD0_LO, HARUKA_CLOUD_LOD0_HI, fp0U * 1.3);
                sN = mix(coverP, sNraw, wOld); sNd = sN;
            }
            if (sN <= 0.0) { fineK = min(fineK * 1.6, 1.0); continue; }
            // Agua: celdas con textura (el altocumulo). Hielo: velo fibroso peinado por el viento
            // de altura (el cirro). Entre medias, mezcla. La fase la decide la temperatura.
            const vec3 dn = upS;
            const float lonM = atan(dn.z, dn.x) * R, latM = asin(clamp(dn.y, -1.0, 1.0)) * R;
            const vec2 wz = vec2(lonM, latM) * 2.5e-5;
            const float along  = lonM + 20000.0 * (harukaCloudVNoise3(vec3(wz, 2.0)) - 0.5) * 2.0;
            const float across = latM + 20000.0 * (harukaCloudVNoise3(vec3(wz, 8.0)) - 0.5) * 2.0;
            const float dWater = harukaLayerDensity(sNd, 0.0, 1.0, pf * 11.4);
            const float dIce   = harukaCirrusDensity(sNd, 0.0, 1.0, along, across, fpPix);
            // La campana en temperatura es el perfil vertical (normalizada: el vapor ya entro en el umbral).
            const float shapeA = fracA / max(max(vaporMid, vaporHigh), 1.0e-3);
            dens = mix(dWater, dIce, ice) * shapeA;
            ext *= mix(0.80, 0.22, ice);              // el hielo extingue mucho menos: velo
        }
        fineK = (dens > 0.0) ? 0.4 : min(fineK * 1.6, 1.0);
        if (dens <= 0.0) continue;
        const float widthM = 1.0 / (convective ? fscale : fscaleA);   // la fundamental del campo (m)

        // ── COBERTURA Y DENSIDAD NO SON LO MISMO EN UN PASO GRUESO ──────────────────────────
        // La transmitancia de una muestra que la nube llena en una fraccion `c` es 1 − c·(1 − e^{−τ}).
        // ⚠️ SOLO CUANDO EL PASO ES GRUESO. `sN` es una FUERZA (cuanto pasa el campo del umbral), y
        // tratarla siempre como "fraccion de la muestra con nube" topaba el alfa de una nube floja
        // en `sN` a cualquier distancia: con sN 0,3 la nube nunca pasaba de 30 % de opacidad, y como
        // la cima de una nube floja es la parte mas debil, de lado se veia solo la base: una TORTA
        // translucida ("hay nubes grandes que salen planas"; medido en el banco de siluetas: de lado,
        // desde la base, solo una raya de 160 m). Un cumulo real es opaco a los 300 m. De cerca (la
        // huella de la muestra por debajo de 1/16 de la fundamental) la nube llena la muestra:
        // Beer-Lambert de siempre. Lejos, cuando la muestra abarca nube y aire, vuelve la fraccion.
        // ⚠️ Y LA FRACCION NO ES INDEPENDIENTE PASO A PASO. Aplicarla por paso (`Π(1 − c·a_i)`)
        // trata cada paso como una nube distinta: con 10 pasos a traves de una capa al 30 % la
        // transmitancia salia 0,7^10 = 0,03 — desde orbita, y desde cualquier sitio alto, una capa
        // ROTA se veia como una sabana blanca ("en orbita esta todo en blanco"; "las nubes tapan el
        // mar"). Un cumulo es una COLUMNA: un rayo vertical ve la misma celda de arriba abajo y su
        // transmitancia es `1 − c·(1 − e^{−τ})` ENTERA (0,7 con c = 0,3). Solo al avanzar una celda
        // en horizontal (`1/fscale`) el rayo sortea una nube nueva — y rasante, con muchas celdas, la
        // union tiende a 1, que es lo que se ve de verdad al mirar una capa rota al horizonte.
        // Asi que la fraccion y la τ interna se acumulan por SEGMENTO (`harukaCloseSeg`).
        const float wCoarse = smoothstep(widthM / 16.0, widthM / 4.0, fpM);
        const float frac  = mix(1.0, clamp(sN, 0.0, 1.0), wCoarse);
        // Con la fundamental resuelta, `dens` lleva la fuerza `sN` de ESTE punto y se normaliza por la
        // fraccion; sin ella (orbita) `dens` ya es la de la parte nublada (`sNd`) y no se divide.
        const float inner = dens / max(((dbg & 1024) != 0) ? frac : mix(1.0, frac, wFund), 1.0e-3);
        // Lo que esta muestra AÑADE a la opacidad: la transmitancia correlada antes y despues.
        const float Tb = harukaCorrT(logClear, tauIn);
        // Sorteos: uno al ENTRAR en cada capa (una columna se ve entera de arriba abajo: al nadir
        // es UN sorteo, no uno por paso) y luego uno por cada L de avance horizontal.
        float draws = stepH / (HARUKA_CLOUD_CORR_CELLS / fscale);
        if (convective && !drewConv)  { draws += 1.0; drewConv  = true; }
        if (!convective && !drewAloft) { draws += 1.0; drewAloft = true; }
        logClear += log(max(1.0 - frac, 1.0e-4)) * draws;
        tauIn    += inner * stepNow * ext;
        float Ta = harukaCorrT(logClear, tauIn);
        // (dbg & 1024): EL MODELO VIEJO, como contraprueba del banco — cada paso su propia nube.
        if ((dbg & 1024) != 0) {
            opticalOld += -log(max(1.0 - frac * (1.0 - exp(-inner * stepNow * ext)), 1.0e-4));
            Ta = exp(-opticalOld);
        }
        const float dAlpha = max(Tb - Ta, 0.0);
        optical = ((dbg & 1024) != 0) ? opticalOld : -log(max(Ta, 1.0e-4));

        // ── TRANSMITANCIA HACIA EL SOL: marchada por el campo, no estimada ───────────────────
        // Tres muestras hacia el Sol dentro de la banda de esta nube, con el campo a huella gruesa
        // (1-2 octavas: la forma, no la erosion). Es lo que pone la cara al Sol clara y la panza en
        // sombra por la NUBE QUE HAY ENTRE MEDIAS, en vez de por la pendiente de un ruido.
        const float localTopM = convective ? (lBase + lThick * mix(HARUKA_CLOUD_MINTOP, 1.0, sNd)) : (alt + 300.0);
        float tauSun = 0.0;
        {
            const float span = clamp(max(localTopM - alt, 30.0) / g_sunUp, 60.0, widthM);
            const float ds   = span / 3.0;
            for (int k = 1; k <= 3; ++k) {
                const vec3  q    = p + u_sun.xyz * (ds * (float(k) - 0.5));
                const float altQ = length(q) - R;
                float dq;
                if (convective) {
                    const float fq = harukaColConvective(sky.x, lBase, lTop, altQ);
                    if (fq <= 0.0) continue;
                    const vec3  pq  = harukaCloudSquashedPos(q, R, altQ, fscale);
                    const float fpq = ds * fscale * HARUKA_CLOUD_HORIZ_SCALE;
                    const float fieldQ = harukaCloudField3LOD(pq, wv, fpPix * fscale * HARUKA_CLOUD_HORIZ_SCALE, fpq);
                    const float sq  = harukaCloudStrengthCumulo(fieldQ, lo);
                    const float fh  = (altQ - lBase) / max(lThick, 1.0);
                    // Con los bultos de la cima: es lo que pone en sombra el flanco que NO mira al Sol
                    // (sin ellos la nube marchada hacia el Sol es lisa y las dos caras salen iguales).
                    const float bq  = harukaCloudTopBumps(pq) * harukaCloudTowerMul(fieldQ - lo);
                    dq = harukaCloudDensity(sq, fh, q * fscale * 11.4, HARUKA_CLOUD_MIN_THICK_M / lThick, 1.0 / (fscale * 11.4), ds, bq);
                } else {
                    dq = dens;   // el velo: se toma homogeneo en su banda (es fino y translucido)
                }
                tauSun += dq * ds * ext;
            }
        }
        const float cosTh = dot(g_dir, u_sun.xyz);
        const float day   = u_sunColor.a;
        const vec3  sunE  = u_sunColor.rgb * HARUKA_CLOUD_SUN_E * mix(0.02, 1.0, day);
        // Ambiente: el cielo por encima (cenit) y el horizonte por los lados, atenuado por la nube
        // que queda hasta la cima; y algo de suelo por debajo. La misma paleta que el domo.
        const float tauTop = max(localTopM - alt, 0.0) * ext * inner;
        const float fH     = convective ? clamp((alt - lBase) / max(lThick, 1.0), 0.0, 1.0) : 0.5;
        const vec3  skyZ   = harukaSkyBase(1.0, day), skyH = harukaSkyBase(0.0, day);
        // Cielo por arriba a traves de la nube que queda hasta la cima (difusa), horizonte por los
        // lados (a traves de media anchura de nube) y suelo por abajo (albedo ~0,2, a traves de la
        // nube que queda hasta la base).
        const float tauBot = convective ? max(alt - lBase, 0.0) * ext * inner : 150.0 * ext * inner;
        const float tauSide = 0.5 * widthM * ext * inner * 0.5;
        const vec3  amb    = skyZ * 0.5 * harukaDiffuseT(tauTop)
                           + skyH * 0.5 * harukaDiffuseT(tauSide)
                           + skyH * 0.2 * harukaDiffuseT(tauBot);
        const float dark   = mix(1.0, 0.55, clamp(sky.w, 0.0, 1.0));   // la tormenta: mas agua = mas extincion, menos luz
        const vec3  L      = HARUKA_CLOUD_ALBEDO * (sunE * harukaMultiScatterSun(tauSun, cosTh) + amb) * dark;
        g_rad  += dAlpha * L;
        lit    += dAlpha;          // (diagnostico: la opacidad repartida)
        wsum   += dens;
        g_tsum += t * dens;
        if (optical > 4.0) return;
    }
    if (i >= budget && t < tExit) g_exhaust = 1.0;
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
            g_tsum += t * dens;
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
    // ⚠️ UNA SOLA MUESTRA, Y ES UN PROBLEMA CONOCIDO. La copia de profundidad es de resolucion
    // COMPLETA y este pase corre a 1/4 de lado: cada pixel de nube tapa 4x4 de escena y lee UNO, asi
    // que en el borde del terreno el "cortar o no cortar" se decide a cara o cruz y el recorte de la
    // capa sale en ESCALERA de bloques (Andoni: "recortes rectos"; su hipotesis, correcta: el recorte
    // lo hace el relieve del terreno). Se probo tomar la MAS LEJANA de cuatro muestras repartidas por
    // la huella: deja de cortar del todo (la cordillera pasa de 0 % a 99,3 % de nube delante), porque
    // cualquier pixel vecino de cielo gana. Y tomar la mas CERCANA agranda el escalon. El arreglo de
    // verdad no esta aqui: es que la COMPOSICION elija, a resolucion completa, la muestra de nube
    // cuya profundidad casa con la del pixel (upsample guiado por profundidad). PENDIENTE.
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
    // ⚠️ ERA `fract(sin(dot(xy, (12.9898, 78.233))) * 43758.5)`, y ese hash tiene el jitter
    // CORRELACIONADO POR FILAS (el seno de un producto escalar casi lineal en x): en un velo fino
    // las muestras de una fila caen todas en las mismas alturas y salen ESTRIAS HORIZONTALES por
    // todo el cielo (captura desde 3 km, 16-09). Ruido de gradiente entrelazado (Jimenez 2014): sin
    // estructura por filas ni columnas, y mas barato (sin seno).
    // ⚠️ Y TAMPOCO el ruido de gradiente entrelazado (Jimenez): es un patron ORDENADO, y en un velo
    // fino los planos de la marcha salian como ANILLOS concentricos alrededor del cenit (captura).
    // Un hash entero de verdad por pixel (lowbias32 de Wellons): blanco, sin filas ni anillos.
    {
        uint hsh = uint(gl_FragCoord.x) * 1664525u ^ uint(gl_FragCoord.y) * 1013904223u;
        hsh ^= hsh >> 16; hsh *= 0x7feb352du; hsh ^= hsh >> 15; hsh *= 0x846ca68bu; hsh ^= hsh >> 16;
        g_jit = float(hsh & 0xffffffu) / 16777216.0;
    }
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
    // ⚠️ LA BANDA QUE SE MARCHA VA MAS ALTA QUE EL TECHO DEL CLIMA. Cada nube tiene su techo
    // (`lTop` = base + grosor x0,7-2 por celda), pero se recortaba al techo GLOBAL `u_slab.y`, y
    // como casi todas lo superan, casi todas quedaban con la cima PLANA a la misma cota: desde 1 km
    // por encima, a 10-20 km, cada cumulo era una RAYA horizontal (captura de Andoni desde el cine
    // `cielo3k`; banco "encima del cumulo, -5"). Un 60 % mas de banda por arriba deja que la cima
    // de cada nube sea la suya; el aire de mas se salta con el paso adaptativo.
    // UNA marcha por la columna de aire: de la base minima del planeta al techo de la atmosfera util
    // (`u_slab.y` lo pone el motor: max(techo convectivo, 13 km)). Los pasos los manda el motor.
    harukaMarchColumn(u_slab.x, u_slab.y, u_misc.z, int(clamp(u_misc.y, 16.0, 96.0)), optical, lit, wsum);

    // Y los embudos, si hay alguno cerca (casi nunca: la salida por recinto es inmediata).
    float dust = 0.0, manga = 0.0;
    harukaMarchFunnels(optical, lit, wsum, dust, manga);

    // (dbg & 64): PINTA EN ROJO los pixeles cuya marcha se quedo sin presupuesto antes de salir de la
    // losa: ahi la nube se CORTA por donde se acabaron los pasos (bordes rectos, Andoni con captura
    // desde 4 km). Diagnostico del banco.
    const int dbgMain = (u_vortexN.w > 0.5) ? int(u_vortexN.w) : 0;
    if ((dbgMain & 64) != 0 && g_exhaust > 0.5) { FragColor = vec4(1.0, 0.0, 0.0, 1.0); return; }
    if ((dbgMain & 512) != 0) {   // SONDA: la distancia MEDIA a lo atravesado (g_tsum/wsum) en 16 bits sobre 20 km
        const float tm = (wsum > 0.0) ? clamp((g_tsum / wsum) / 20000.0, 0.0, 1.0) : 0.0;
        const float hi16 = floor(tm * 255.0) / 255.0, lo16 = fract(tm * 255.0);
        // B: 1 = nube OPACA (optica > 0,7, alfa > 0,5) · 0,5 = jiron · 0 = nada
        FragColor = vec4(hi16, lo16, (optical > 0.7) ? 1.0 : ((wsum > 0.0) ? 0.5 : 0.0), 1.0);
        return;
    }
    if ((dbgMain & 256) != 0) {   // SONDA de profundidad: antes de la salida "sin nube"
        FragColor = (g_sceneT < 1.0e7) ? vec4(fract(g_sceneT / 1000.0), min(g_sceneT / 30000.0, 1.0), 0.0, 1.0)
                                       : vec4(0.0, 0.0, 1.0, 1.0);
        return;
    }
    if (optical <= 0.0001) { FragColor = vec4(0.0); return; }

    // Beer-Lambert: la opacidad sale de la profundidad óptica, no de una media. Atravesar más nube
    // tapa MÁS — que es lo que distingue un volumen de una calcomanía.
    const float alpha = 1.0 - exp(-optical);
    const float day   = u_sunColor.a;
    // La radiancia integrada, sin premultiplicar (ver la nota de `g_rad`). Los embudos siguen
    // aportando por `lit` a la antigua: se funden con su propia nube madre.
    vec3 col = (alpha > 1.0e-4) ? g_rad / alpha : vec3(0.0);
    // ⚠️ RODILLA DE EXPOSICION, no un tope. La dispersion hacia delante es fisica y HDR: un borde
    // fino contra el Sol da 2-4 veces la radiancia isotropa (fase de HG a 0,72 por 4π), y este render
    // no tiene pipeline HDR ni tonemapping: todo lo demas vive en [0,1]. Por debajo de 0,8 no se toca
    // (ahi esta la forma de la nube); lo que sobra se comprime suavemente en [0,8, 1,0] para que el
    // brillo del borde se vea sin quemarse a blanco plano ("quemados" en el banco).
    col = mix(col, 0.8 + 0.2 * (1.0 - exp(-(col - 0.8) / 0.2)), step(0.8, col));
    if (wsum > 0.0 && (dust > 0.0 || manga > 0.0)) {
        const vec3 cFunnel = mix(vec3(0.28, 0.31, 0.38), vec3(0.55, 0.57, 0.63), day);
        col = mix(col, cFunnel, clamp((dust + manga) / wsum, 0.0, 1.0));
    }

    // (el oscurecido de tormenta va POR PUNTO dentro de la marcha, ya no es un escalar global)
    // El polvo que levanta un tornado es tierra, no nube: tinte terroso en proporción a cuánto de lo
    // atravesado era polvo. En el mar `dust` es 0 y la tromba queda del color de la nube.
    if (wsum > 0.0) {
        // La MANGA es oscura: un cuerpo grueso que se ve a contraluz de su propia nube. Con el color
        // de sombra de la nube (0,55 de gris a pleno día) salía blanca contra el cielo en la captura.
        col *= mix(1.0, 0.38, clamp(manga / wsum, 0.0, 1.0));
        col  = mix(col, vec3(0.42, 0.34, 0.24) * (0.5 + 0.5 * day), clamp(dust / wsum, 0.0, 0.8));
    }

    // EL AIRE DE EN MEDIO: una nube a 100 km se ve a través de 100 km de aire, y con L = 18-60 km
    // eso es casi todo aire. Es lo que quita las "tiras" del horizonte: la capa vista de canto, con
    // su ruido aliasado a esa distancia, se funde con el cielo en vez de recortarse en blanco. El
    // mismo `harukaAerial` que usan el terreno y los props, a la distancia media de lo atravesado.
    if (wsum > 0.0) {
        const vec3  upC   = normalize(-u_planetC.xyz);
        const float tMean = g_tsum / wsum;
        // Con la altitud de lo atravesado: desde orbita una nube a 3 km se ve a traves de unos km de
        // aire, no de 250 (ver lib/aerial.glsl). `g_ro` es relativo al CENTRO del planeta.
        const float hCloud = length(g_ro + g_dir * tMean) - u_planetC.w;
        col = harukaAerialAlt(col, tMean, dot(g_dir, upC), hCloud, u_aerial);
    }
    // (dbg & 128): SONDA — `light` en gris, `wsum` en alfa, para bisecar un negro opaco.
    if ((((u_vortexN.w > 0.5) ? int(u_vortexN.w) : 0) & 128) != 0) {
        FragColor = vec4(lit / 10.0, wsum / 10.0, min(optical, 8.0) / 8.0, 1.0);
        if (isinf(wsum)) FragColor.a = 0.0;
        return;
    }
    FragColor = vec4(col, alpha * clamp(u_misc.x, 0.0, 1.0));
}
