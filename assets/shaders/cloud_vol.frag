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

/// Cobertura en una direccion (desde el centro del planeta). Convencion equirect del motor.
float coverAtDir(vec3 dirFromCenter) {
    const ivec2 sz = textureSize(u_coverTex, 0);
    if (sz.x <= 1 || sz.y <= 1) return u_slab.z;    // sin textura: el escalar de siempre
    const vec3 d = normalize(dirFromCenter);
    const vec2 uv = vec2(0.5 + atan(d.z, d.x) * 0.1591549,
                         0.5 - asin(clamp(d.y, -1.0, 1.0)) * 0.3183099);
    return texture(u_coverTex, uv).r;
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

void harukaMarchShell(float baseA, float topA, float fscale, float extMul, float covMul, int steps,
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

    float t = tEnter + g_jit * dt;
    for (int i = 0; i < steps && t < tExit; ++i, t += dt, dt *= growth) {
        const vec3  p = g_ro + g_dir * t;
        const float alt = length(p) - R;
        const float f = (alt - baseA) / thick;
        if (f <= 0.0 || f >= 1.0) continue;

        const float coverP = clamp(coverAtDir(p) * covMul, 0.0, 1.0);
        const float lo = mix(0.58, 0.20, coverP);

        // ⚠️ EL DOMINIO ES LA POSICION, NO UNA PROYECCION SOBRE EL MARCO DE LA CAMARA. Aquí estaba
        // el "las nubes se mueven contigo": ver la nota larga en `lib/cloud_volume.glsl`.
        const vec3  pf = p * fscale;
        const float sN = harukaCloudStrength(harukaCloudField3(pf, harukaWindAt(p / (R + alt))), lo);
        if (sN <= 0.0) continue;

        // El detalle a la MISMA cadencia que antes: eran `11.4` unidades por unidad de rasgo en
        // horizontal y `0.004` por metro en vertical, y con `fscale = 1/2857` (el cúmulo)
        // `11.4 * fscale = 0.00399` — el mismo número. O sea que esto no cambia el grano, sólo lo
        // deja de anclar al ojo.
        const vec3  q    = pf * 11.4;
        const float dens = harukaCloudDensity(sN, f, q);
        if (dens <= 0.0) continue;

        const float ext = u_misc.w * extMul;   // el velo alto extingue MUCHO menos que el cumulo
        optical += dens * dt * ext;

        const float localTopM = baseA + thick * mix(HARUKA_CLOUD_MINTOP, 1.0, sN);
        const float widthM = 1.0 / fscale;
        const float toSun  = min(max(localTopM - alt, 0.0) / g_sunUp, widthM);
        const float shadow = exp(-toSun * ext * 0.25 * sN);

        lit  += (0.18 + 0.82 * shadow) * dens;
        wsum += dens;
        if (optical > 4.0) return;
    }
}

void main()
{
    const float coverMax = u_slab.z;
    if (coverMax <= 0.01) { FragColor = vec4(0.0); return; }

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
    harukaMarchShell(u_slab.x, u_slab.y, u_misc.z, 1.00, 1.00,
                     int(clamp(u_misc.y, 16.0, 96.0)), optical, lit, wsum);
    harukaMarchShell(4000.0,   4600.0,   1.0/1200.0, 0.55, 0.45, 12, optical, lit, wsum);
    harukaMarchShell(8000.0,   8900.0,   1.0/6000.0, 0.22, 0.30, 12, optical, lit, wsum);

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

    // La tormenta oscurece: un cumulonimbo cargado no es blanco.
    col *= mix(1.0, 0.55, clamp(u_slab.w, 0.0, 1.0));

    FragColor = vec4(col, alpha * clamp(u_misc.x, 0.0, 1.0));
}
