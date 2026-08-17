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

void main()
{
    const float cover = u_slab.z;
    if (cover <= 0.01) { FragColor = vec4(0.0); return; }

    const vec3  dir   = normalize(vRayDir);
    const vec3  pc    = u_planetC.xyz;          // centro del planeta, relativo a la cámara
    const float R     = u_planetC.w;
    const float baseA = u_slab.x, topA = u_slab.y;
    if (topA <= baseA + 1.0 || R <= 1.0) { FragColor = vec4(0.0); return; }

    // Origen del rayo respecto al CENTRO del planeta. La cámara está en el origen del mundo
    // relativo, así que el vector al centro es -pc.
    const vec3 ro = -pc;

    // Tramo dentro de la losa: entre la esfera de la base y la del techo.
    float b0, b1, t0, t1;
    bool hitBase = harukaRaySphere(ro, dir, R + baseA, b0, b1);
    bool hitTop  = harukaRaySphere(ro, dir, R + topA,  t0, t1);
    if (!hitTop) { FragColor = vec4(0.0); return; }   // ni siquiera roza la capa

    // El interior de la cáscara. Desde debajo de la base se entra por `b1` y se sale por `t1`;
    // desde dentro se arranca en 0. Se acota a lo positivo: lo que queda detrás no se pinta.
    float tEnter = hitBase ? max(b1, 0.0) : max(t0, 0.0);
    float tExit  = max(t1, 0.0);
    // Mirando hacia abajo desde encima de la capa el orden se invierte.
    if (hitBase && b0 > 0.0) { tEnter = max(t0, 0.0); tExit = max(b0, 0.0); }
    if (tExit <= tEnter) { FragColor = vec4(0.0); return; }

    // ── CORTE CONTRA LA ESCENA ──────────────────────────────────────────────────────────────────
    // Sin esto la nube se pintaría por delante del terreno. Se deshace la profundidad a posición de
    // mundo (cámara-relativa) y su módulo es la distancia a la que hay geometría.
    {
        const float d = texture(u_sceneDepth, vNdc * 0.5 + 0.5).r;
        // La MISMA inversa con la que se rasterizó el depth (rotación, cámara-relativa). El clip es
        // ZERO_TO_ONE (`gl_device.cpp` llama a `glClipControl`), así que el valor del depth ES la z
        // de NDC y no hay que remapearlo.
        vec4 wp = u_invViewProjRot * vec4(vNdc, d, 1.0);
        if (abs(wp.w) > 1e-9) {
            const float sceneT = length(wp.xyz / wp.w);
            // Con reversed-Z el "vacío" es 0.0 y reproyecta a una distancia enorme: se descarta por
            // el propio tamaño en vez de comparar con un valor de clear que depende de la convención.
            if (sceneT < 1.0e7) tExit = min(tExit, sceneT);
        }
    }
    if (tExit <= tEnter) { FragColor = vec4(0.0); return; }

    // ── MARCHA ──────────────────────────────────────────────────────────────────────────────────
    const int   STEPS = int(u_misc.y);
    const float span  = tExit - tEnter;
    const vec2  wind  = u_wind.xy;
    const float scale = u_misc.z;
    const float thick = topA - baseA;

    // JITTER: con 24 pasos, empezar todos los rayos en el mismo sitio pone la estructura de la
    // marcha EN PANTALLA — anillos concéntricos alrededor del punto de fuga. Desplazar el primer
    // paso una fracción aleatoria del intervalo convierte esas bandas en ruido, que el ojo perdona.
    const float jit = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);

    // ── PASO QUE CRECE CON LA DISTANCIA ─────────────────────────────────────────────────────────
    // ⚠️ ESTO ERA "LA NUBE NO TIENE ALTURA", y no se arregla en el campo de densidad: el campo mide
    // 0,82:1 (más alta que ancha). Era el MUESTREO. Repartir el recorrido en 24 pasos iguales
    // funciona mirando arriba y se desmorona mirando al horizonte, que es justo donde se ven las
    // nubes DE PERFIL y donde está casi toda el área de cielo. Medido sobre la losa real
    // (1303-1887 m) contra una nube de 332 m:
    //
    //      cenit     0,6 km de recorrido ->  24 m de paso -> 13,6 muestras por nube
    //      70 grados 1,7 km               ->  71 m        ->  4,7
    //      85 grados 6,5 km               -> 271 m        ->  1,2
    //      88 grados 14,1 km              -> 587 m        ->  0,6   <- MENOS DE UNA
    //
    // Con menos de una muestra por nube no queda relieve que ver: se promedia todo a un manchón
    // uniforme. Un paso que CRECE resuelve fino lo cercano (donde la nube ocupa muchos píxeles) y
    // grueso lo lejano (donde ya es subpíxel), con los MISMOS 24 pasos y por tanto el mismo coste.
    // ⚠️ SEGUNDA VUELTA: con 24 pasos y crecimiento 1,32 lo cercano quedaba bien y **lo lejano
    // seguía plano**, que es lo que se reportó. El paso iguala al tamaño de la nube a los 4,3 km:
    // más allá de ahí no hay estructura que resolver. El presupuesto lo dio el propio HUD del juego
    // (`renderFrameContent` 1,42 ms con este pase POR DEBAJO del umbral de 0,5 ms del panel), así
    // que se pueden pagar muchos más pasos y crecer MÁS DESPACIO:
    //
    //      24 pasos x1,32  ->  resuelto hasta  4,3 km   (lo de antes: lejanas planas)
    //      64 pasos x1,09  ->  resuelto hasta 15,3 km   <- esto
    //
    // Crecer más despacio importa tanto como el número de pasos: con x1,32 los últimos pasos miden
    // kilómetros aunque haya 64.
    const float kNearStep = 50.0;    // paso al entrar, en metros
    const float kGrowth   = 1.09;    // 64 pasos desde 50 m alcanzan ~137 km (el tramo rasante
                                     // extremo son 91 km: por debajo de eso, las nubes del ultimo
                                     // trecho hacia el horizonte se quedaban SIN marchar)
    float dt     = span / float(STEPS);
    float growth = 1.0;
    if (dt > kNearStep) { dt = kNearStep; growth = kGrowth; }

    // Base tangente para proyectar el punto de la marcha a coordenadas horizontales del campo.
    const vec3 up0 = normalize(ro);
    vec3 e1 = normalize(cross(abs(up0.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0), up0));
    vec3 e2 = cross(up0, e1);

    // Elevación del Sol sobre la vertical local. Manda en cuánta nube tiene que atravesar la luz
    // para llegar a un punto: con el Sol bajo el camino es largo y el cúmulo se enciende de lado.
    const float sunUp = max(dot(up0, u_sun.xyz), 0.10);

    // Umbral del campo: más cobertura, menos umbral, más CIELO CUBIERTO. ⚠️ Ahora el umbral decide
    // sólo QUÉ FRACCIÓN del cielo tiene nube; lo maciza que es cada una la decide la extinción. Antes
    // decidía las dos cosas a la vez, y por eso con la cobertura mediana del planeta (0,111) las
    // nubes existían pero eran transparentes.
    const float lo = mix(0.58, 0.20, clamp(cover, 0.0, 1.0));

    float optical = 0.0;   // profundidad óptica acumulada
    float lit     = 0.0;   // iluminación acumulada, ponderada por lo que aporta cada tramo
    float wsum    = 0.0;

    float t = tEnter + jit * dt;    // el jitter desplaza el ARRANQUE, no cada muestra

    for (int i = 0; i < STEPS && t < tExit; ++i, t += dt, dt *= growth) {
        const vec3  p = ro + dir * t;                 // punto respecto al centro del planeta
        const float r = length(p);
        const float alt = r - R;

        // Altura relativa dentro de la losa. El perfil se aplica dentro de `harukaCloudDensity`,
        // sobre la altura remapeada al techo LOCAL de esta nube.
        const float f = (alt - baseA) / thick;
        if (f <= 0.0 || f >= 1.0) continue;

        // Coordenadas horizontales: proyección del punto sobre la base tangente. Es lo que hace que
        // la nube se quede quieta en el mundo mientras la cámara se mueve por debajo.
        const vec2  uv = vec2(dot(p, e1), dot(p, e2)) * scale;
        const float s  = harukaCloudStrength(harukaCloudField(uv, wind), lo);
        if (s <= 0.0) continue;                       // aire: el detalle 3D no se paga aquí

        // Detalle 3D en las MISMAS unidades del campo (uv) más la altura. Mantenerlo en este marco
        // evita meter coordenadas de ~6,4e6 m en un `fract`, donde el ruido se cuantiza a escalones.
        const vec3  q    = vec3(uv * 11.4, (alt - baseA) * 0.004);
        const float dens = harukaCloudDensity(s, f, q);
        if (dens <= 0.0) continue;

        // EXTINCIÓN por metro. Es EL mando de la densidad y por eso viaja en el UBO en vez de estar
        // cableado: sube y la nube se vuelve opaca antes, baja y se ve el cielo a través.
        optical += dens * dt * u_misc.w;

        // ── LUZ ─────────────────────────────────────────────────────────────────────────────────
        // ⚠️ ESTO ERA LA OTRA MITAD DE "es una lámina". La versión anterior sombreaba con
        // `dot(normalize(p), sol)`, pero `p` va referido al CENTRO DEL PLANETA: sobre una nube de
        // 3 km, `normalize(p)` gira 4,7e-4 rad, así que ese producto escalar era CONSTANTE en toda
        // la nube. No había sombreado que ver — un volumen iluminado con un valor único se ve
        // exactamente igual que una calcomanía.
        //
        // Ahora se mide el CAMINO ÓPTICO HACIA EL SOL de forma analítica: cuánta nube queda por
        // encima hasta salir por el techo local, dividido por la elevación solar. No cuesta ni una
        // muestra extra del campo, y varía en horizontal porque el techo local varía con `s`: eso
        // es lo que da cimas encendidas, panzas oscuras y relieve entre nube y nube.
        const float localTopM = baseA + thick * mix(HARUKA_CLOUD_MINTOP, 1.0, s);
        // ⚠️ TOPADO AL ANCHO DE LA NUBE. Con el Sol bajo, `1/sunUp` dispara el camino a decenas de
        // km y la nube entera se apagaría justo al amanecer y al atardecer, que es cuando más se
        // mira. Pero la luz no atraviesa 20 km de nube: SALE POR EL COSTADO en cuanto el camino
        // supera el ancho del cúmulo (~1/escala del campo). Con el tope, el Sol rasante enciende el
        // borde superior y deja la panza oscura, en vez de apagarlo todo.
        const float widthM = 1.0 / scale;
        const float toSun  = min(max(localTopM - alt, 0.0) / sunUp, widthM);
        // La extinción de la LUZ es una fracción de la de la VISTA. No es un ajuste a ojo: un solo
        // rebote (esto es `exp(-x)`, dispersión simple) deja negro cualquier interior de nube,
        // mientras que una nube real es blanca PORQUE la luz rebota muchas veces dentro. Bajar el
        // coeficiente del camino de luz es la forma barata y estándar de no perder esa energía sin
        // integrar el multiscattering entero.
        const float shadow = exp(-toSun * u_misc.w * 0.25 * s);

        lit  += (0.18 + 0.82 * shadow) * dens;   // 0.18 = luz del cielo; sin ella la panza es negra
        wsum += dens;

        if (optical > 4.0) break;       // ya es opaco: seguir no cambia el píxel
    }

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
