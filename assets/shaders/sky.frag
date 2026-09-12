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
#include "lib/sky_clouds.glsl"   // hash13/fbm/cloudField: el campo, para poder SONDEARLO

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
// ── LA ATMOSFERA VISTA DESDE FUERA ──────────────────────────────────────────────────────────────
//
// ⚠️ ESTO NO EXISTIA. `sky.frag` solo sabe pintar el DOMO en el que estas metido: fuera, `u_atmo`
// tiende a 0 y `mix(spaceC, sky, u_atmo)` devuelve espacio liso. O sea que un planeta visto desde
// orbita no tenia atmosfera — ni halo en el limbo, ni la linea azul sobre el horizonte, que es lo
// que hace que un mundo parezca un mundo desde el espacio. No es que se apagara con la altitud: no
// estaba escrito. Reportado mirando la pantalla ("el atmosfera segun donde estas se deja de ver").
//
// Es el caso geometrico complementario del domo: el rayo entra en la cascara de aire (R .. R+H),
// recorre un tramo, y sale — o lo corta el planeta. Se integra la densidad por el camino con la
// escala de altura real, y lo que se dispersa es Rayleigh: el azul ~5 veces mas que el rojo.
//
// ⚠️ SOLO SALE FUERA, y no por un interruptor sino por la misma mezcla que ya habia: se suma a
// `spaceC`, asi que a ras de suelo `mix(..., sky, u_atmo)` la tapa entera. El aire visto desde dentro
// ya lo modela el domo; tener los dos sumandose seria contar la atmosfera dos veces.
//
// ⚠️ LA BRUMA SOBRE EL DISCO SE CALCULA BIEN, PERO EL ORDEN DE PASES LA PIERDE. Medido: sobre el
// disco esta funcion da luminancia **117** (el rayo cruza aire antes de llegar al suelo, y eso es lo
// correcto). Lo que pasa es que `sky.frag` se dibuja ANTES de la escena, asi que donde hay terreno el
// terreno la sobreescribe. El limbo —los rayos que pasan de largo— si sobrevive porque ahi no hay
// nada delante, y es la mitad que se ve desde orbita.
//
// O sea que NO falta fisica: falta un pase DESPUES de la escena con la copia de profundidad, como el
// de nubes, que componga esta misma integral sobre lo ya dibujado. ABIERTO.
//
// ⚠️ Y una correccion a lo que llegue a afirmar: dije que el halo se salia 4 grados de la cascara,
// leyendo un perfil por angulo que estaba ESPEJADO (ver `detectRowFlip` en el test). Con el orden de
// filas bien calibrado, el espacio vale 1,1 con atmosfera y 1,1 sin ella — no se sale nada.

/// Interseccion rayo-esfera centrada en el origen. `t0 <= t1`.
bool raySphereSky(vec3 ro, vec3 rd, float rad, out float t0, out float t1) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - rad * rad;
    float h = b * b - c;
    if (h < 0.0) { t0 = t1 = 0.0; return false; }
    h = sqrt(h);
    t0 = -b - h; t1 = -b + h;
    return true;
}

vec3 harukaAtmoLimb(vec3 dir) {
    float R = u_planet.x;
    if (R <= 1.0) return vec3(0.0);
    // Espesor VISIBLE de la atmosfera y escala de altura. Los dos de la Tierra: la capa azul se
    // acaba hacia los 80 km y la densidad cae a 1/e cada 8,5 km.
    const float kAtmoM = 80000.0;
    const float kScaleH = 8500.0;

    vec3 ro = u_up * (R + max(u_planet.y, 0.0));     // el ojo, respecto al CENTRO del planeta
    float a0, a1;
    if (!raySphereSky(ro, dir, R + kAtmoM, a0, a1)) return vec3(0.0);
    float tEnter = max(a0, 0.0);
    float tExit  = max(a1, 0.0);
    if (tExit <= tEnter) return vec3(0.0);
    // El planeta corta el rayo: mas alla de su superficie no hay aire que sumar.
    float p0, p1;
    if (raySphereSky(ro, dir, R, p0, p1) && p0 > 0.0) tExit = min(tExit, p0);
    if (tExit <= tEnter) return vec3(0.0);

    const int N = 8;                                  // fondo a pantalla completa: pocos, y bastan
    float dt = (tExit - tEnter) / float(N);
    float dens = 0.0, litSum = 0.0;
    for (int i = 0; i < N; ++i) {
        vec3  p   = ro + dir * (tEnter + (float(i) + 0.5) * dt);
        float alt = length(p) - R;
        float d   = exp(-max(alt, 0.0) / kScaleH) * dt;
        dens += d;
        // ¿Le da el sol a ESE trozo de aire? Es lo que pone el terminador y deja el lado nocturno
        // oscuro en vez de un halo azul rodeando el planeta entero.
        litSum += d * smoothstep(-0.15, 0.10, dot(normalize(p), u_sunDir));
    }
    if (dens <= 0.0) return vec3(0.0);
    float lit = litSum / dens;

    // ⚠️ COEFICIENTES DE RAYLEIGH REALES, no una proporcion inventada. Estaban como
    // `beta = (0.20, 0.50, 1.00)` por un `kTau = 2.0e-6` que me saque de la manga para que "se viera
    // bien": eso deja el azul en 2,0e-6 por metro cuando la dispersion de Rayleigh al nivel del mar
    // es **33,1e-6** — dieciseis veces mas. Por eso la atmosfera salia FINA: solo los ultimos
    // kilometros de aire llegaban a ser visibles y el resto de la capa se quedaba por debajo del
    // umbral del ojo. Reportado mirando la pantalla ("se ve desde el espacio pero un poco fina").
    //
    // Los valores son los medidos para el aire a nivel del mar, en m^-1 (R, G, B):
    const vec3 beta = vec3(5.8e-6, 13.5e-6, 33.1e-6);
    // Con esto, un rayo rasante a la superficie (columna equivalente ~5,8e5 m) satura los tres
    // canales y da el blanco-azulado de la base del limbo; a 30 km de altura tangente cae a
    // tau = 0,56 en azul contra 0,098 en rojo, o sea el azul profundo de arriba. Esa graduacion
    // —blanca abajo, azul arriba, negra al final— es la que hace que la capa se lea GRUESA.
    vec3 scat = (1.0 - exp(-dens * beta)) * lit;
    return scat * u_sunColor;
}

vec2 cloudPlane(vec3 dir, vec3 e1, vec3 e2, float h, out bool outOk, out float outDistKm) {
    outOk = false; outDistKm = 0.0;
    float R = u_planet.x;
    if (R <= 0.0) {
        // Sin planeta (menús, tests): se conserva el domo de siempre para no dejar el cielo vacío.
        float t = max(dot(dir, u_up), 0.0);
        outOk = true; outDistKm = 4.0;
        return vec2(dot(dir, e1), dot(dir, e2)) / (t + 0.22) * 4.0;
    }
    float ro    = R + max(u_planet.y, 0.0);     // radio del ojo
    float rc    = R + h;                        // radio de la capa
    float mu    = dot(dir, u_up);               // coseno respecto al cénit
    float disc  = ro * ro * (mu * mu - 1.0) + rc * rc;
    if (disc < 0.0) return vec2(0.0);           // el rayo no llega a la capa
    float dist  = -ro * mu + sqrt(disc);        // raíz positiva: la capa está arriba
    if (dist <= 0.0) return vec2(0.0);
    // ⚠️⚠️ AQUI ESTABA EL ANILLO. Esto era `dist = min(dist, 6.0*h)`, un tope DURO, y el efecto no es
    // "la nube se ve borrosa a lo lejos" sino que **por debajo de cierta elevacion la capa desaparece
    // de golpe**. Con la capa a 4 km y el ojo a 300 m, alcanzar 6·h = 24 km pide asin(3700/24000) =
    // 8,9 grados; por debajo, TODOS los rayos se clavan en la misma distancia y —siendo casi
    // paralelos cerca del horizonte— muestrean el ruido practicamente en el MISMO punto. El campo
    // sale constante en toda esa franja y se va a todo-o-nada.
    //
    // Medido con `sky_layers_have_depth` antes de tocarlo: 0 % de nube por debajo de 10 grados,
    // 6,95 % y 12,73 % en 10-20. Un anillo de nube a altura fija de pantalla es exactamente lo que se
    // lee como "las nubes son un plano 2D".
    //
    // Se sustituye por una saturacion SUAVE: identica hasta `D` (el campo cercano no cambia ni un
    // bit) y a partir de ahi crece cada vez menos hasta `2·D`. Sigue acotada —que es lo que pedia el
    // aviso de aliasing— pero es MONOTONA y estrictamente creciente, asi que dos rayos vecinos nunca
    // caen en el mismo punto del campo y la capa sigue teniendo estructura hasta el horizonte.
    // La derivada vale 1 justo en `D`, asi que no hay codo visible en la transicion.
    {
        float D = 6.0 * max(h, 1.0);
        dist = (dist <= D) ? dist : D * (2.0 - D / dist);
    }
    vec3  hit   = dir * dist;                   // punto en la capa, relativo al ojo
    outOk = true; outDistKm = dist * 0.001;     // para el footprint del pixel (ver `fbmAA`)
    return vec2(dot(hit, e1), dot(hit, e2)) * 0.001;   // a km
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

    // ── LAS NUBES NO SE PINTAN AQUI. NINGUNA. ───────────────────────────────────────────────────
    //
    // ⚠️ AQUI VIVIAN 286 LINEAS: las tres capas (cumulo, altocumulo, cirro) dibujadas como FONDO, y
    // un camino de RESPALDO que las repintaba planas cuando el pase volumetrico estaba apagado. Se
    // van enteras, y el motivo es el que ya estaba escrito en este mismo fichero unas lineas mas
    // abajo de donde empezaban:
    //
    //   un pase de FONDO no tiene profundidad, no se compone sobre la escena, y su puerta (`t > 0.02`)
    //   solo deja mirar hacia arriba: pasabas de 4 km y el altocumulo se esfumaba, de 8 y el cirro, y
    //   nunca se veian por debajo de ti.
    //
    // Las tres viven en `cloud_vol.frag` como cascaras marchadas de verdad — con interior, que se
    // cruzan al volar y que se componen con la profundidad de la escena. Mantener ademas una copia
    // plana aqui significaba DOS respuestas a "que nube hay en esa direccion", y la que se veia
    // dependia de un conmutador. Es la misma enfermedad que las tres aguas y las dos miniaturas.
    //
    // ⚠️ CONSECUENCIA, Y ES DELIBERADA: con `HARUKA_CLOUD_VOL=0` o `m_volumetricClouds = false` el
    // cielo se queda SIN NUBES. Antes el respaldo las repintaba planas y apagar el pase no se notaba
    // — que es justo lo que hacia imposible saber cual de los dos caminos estabas mirando. Ahora el
    // interruptor dice la verdad: apagado es apagado.

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
    // La atmosfera VISTA DESDE FUERA se suma al espacio, no al cielo: dentro, el domo ya la modela y
    // `mix` la tapa entera. Ver `harukaAtmoLimb`.
    spaceC += harukaAtmoLimb(dir);
    vec3 outC   = mix(spaceC, sky, u_atmo);
    // (El cuerpo del Sol = corona-objeto emisiva, se dibuja en el pase de objetos sobre este
    //  fondo, igual dentro y fuera de la atmósfera → sin "dos soles".)

    FragColor = vec4(outC, 1.0);
}
