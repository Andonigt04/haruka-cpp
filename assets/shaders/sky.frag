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

    // --- NUBES cinematográficas: VOLUMEN (domain warp + auto-sombra en varios pasos hacia el sol = gradiente
    //     luz→sombra "de película"), ESCALA por zonas (un campo de masa de baja frecuencia → cúmulos grandes,
    //     zonas despejadas y puffs pequeños), CICLO del planeta y SILVER-LINING a contraluz. Plano tangente
    //     LOCAL (es un planeta → "arriba" = u_up). ---
    if (u_atmo > 0.5 && t > 0.02) {
        // ⚠️ EL FOOTPRINT DEL PIXEL, MEDIDO AQUI Y NO DENTRO DEL BUCLE. `fwidth` en control de flujo
        // NO UNIFORME es comportamiento indefinido, y el bucle de capas tiene un `continue` (cuando el
        // rayo no alcanza la capa) que lo vuelve no uniforme. Se mide sobre `dir`, que es uniforme, y
        // dentro del bucle se escala por la distancia y por `scale` de cada capa.
        float dirPx = length(fwidth(dir));   // radianes por pixel, aproximadamente
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
        // ⚠️⚠️ CUANDO EL PASE VOLUMETRICO ESTA ACTIVO, AQUI NO SE PINTA NINGUNA NUBE. Antes eran 2
        // (cirro y altocumulo) y se quedaban porque "estan tan alto que nunca se cruzan" — premisa
        // falsa en un juego con vuelo y orbita. Al ser un pase de FONDO no tienen profundidad, no se
        // componen sobre la escena, y su puerta (`t > 0.02`) solo deja mirar hacia arriba: pasabas de
        // 4 km y el altocumulo se esfumaba, de 8 y el cirro, y nunca se veian por debajo de ti.
        // Reportado como "se desvanecen con la subida como si estuvieran pegados al fondo".
        //
        // Las tres capas viven ahora en `cloud_vol.frag` como cascaras marchadas de verdad. Aqui solo
        // se conserva el camino de RESPALDO (`u_planet.w > 1.0`, o sea el pase volumetrico apagado),
        // donde se pintan las tres planas como siempre: apagar las nubes no debe dejar el cielo pelado.
        int nLayers = (u_planet.w > 1.0) ? 3 : 0;
        for (int layer = 0; layer < nLayers; ++layer)
        {
            // ⚠️⚠️ `slabM`/`slabSteps`/`pathMax` SON NUEVOS Y ARREGLAN EL BUG QUE ESTE MISMO
            // FICHERO CREIA RESUELTO. El bloque de "ESPESOR REAL: LOSA EN VEZ DE LAMINA" de abajo
            // decia `if (layer == 2)` — o sea que solo el CUMULO tenia espesor. Y desde que el
            // cumulo lo dibuja el pase volumetrico, `nLayers` vale 2 y **la capa 2 no se recorre
            // nunca**: el arreglo entero de la planitud estaba aplicado a la unica capa que ya no se
            // pinta aqui. Las dos que SI se ven —cirro y altocumulo— seguian siendo calcomanias 2D
            // sobre un cascaron, que es literalmente lo que aquel comentario describia como el
            // motivo de que se vieran planas: *"Se veía plano porque era plano"*.
            //
            // Espesores de la atmosfera real: altocumulo 200-1000 m (se toman 600), cirro mucho mas
            // extenso pero tenue (900 m con dos muestras basta para que deje de ser una pegatina).
            // El COSTE no sube respecto a antes: se pasa de "2 capas planas + 1 losa de 4 pasos que
            // ya no corre" a "una losa de 2 + una de 4", o sea las mismas 6 evaluaciones de campo.
            float h, scale, sharp, thick, windMul, shadeMul, covMul, slabM, pathMax;
            int   slabSteps;
            // ⚠️ `scale` ES EL TAMAÑO DE LA NUBE, Y ESTABA DE 4 A 5 VECES PASADO. `base = pk·scale`
            // con `pk` en km, y la frecuencia dominante de `cloudField` es 1,3, asi que el elemento
            // mide ~0,77/scale km. Con `scale = 0,16` el altocumulo tenia elementos de **4,8 km** —
            // un altocumulo de verdad son borreguillos de ~1 km. Y eso no es solo estetico: la capa
            // esta a 4 km de altura, asi que MIRANDO HACIA ARRIBA solo se ve un parche de unos pocos
            // km de ella. Con elementos de 4,8 km, el cielo alto entero cae dentro de UN elemento y
            // sale todo nube o todo despejado. Medido: 0,00 % de nube en 20-30 grados, en las cuatro
            // tomas. No era un umbral mal puesto, era que ahi solo cabe una nube.
            //
            // Ahora sale del tamaño FISICO de cada tipo: cirro en jirones de ~6 km, altocumulo en
            // borreguillos de ~1,2 km, cumulo de ~1 km.
            if (layer == 0) {        // CIRRO (se pinta primero: está detrás de todo)
                h = 8000.0; scale = 0.128; sharp = 0.30; thick = 0.55;   // jirones de ~6 km
                windMul = 0.35; shadeMul = 0.25; covMul = 0.75;
                slabM = 900.0; slabSteps = 2; pathMax = 1.6;   // velo: poco espesor efectivo
            } else if (layer == 1) { // ALTOCÚMULO
                h = 4000.0; scale = 0.64;  sharp = 0.14; thick = 0.80;   // borreguillos de ~1,2 km
                windMul = 0.65; shadeMul = 0.70; covMul = 0.85;
                slabM = 600.0; slabSteps = 4; pathMax = 2.2;
            } else {                 // CÚMULO (delante, el protagonista)
                h = 1500.0; scale = 0.77;  sharp = 0.07; thick = 1.00;   // cumulos de ~1 km
                windMul = 1.00; shadeMul = 1.00; covMul = 1.00;
                slabM = 1700.0; slabSteps = 4; pathMax = 2.6;  // el techo real lo trae el clima
            }

            // La base de los CÚMULOS la pone el clima (la misma que el techo de la lluvia), no una
            // constante: así la panza de la nube y el punto donde nacen las gotas coinciden.
            if (layer == 2 && u_planet.z > 1.0) h = u_planet.z;

            bool ok; float distKm;
            vec2 pk = cloudPlane(dir, e1, e2, h, ok, distKm);
            if (!ok) continue;
            // Lado del pixel EN UNIDADES DEL CAMPO: radianes/pixel x distancia (km) x `scale`.
            float fieldPx = dirPx * distKm * scale;
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
            // TODAS las capas son losas ahora, cada una con su espesor y sus pasos (ver arriba).
            float dens;
            {
                const int  STEPS = slabSteps;
                // CIMA DEL CÚMULO: la trae el CLIMA (`WeatherSample::cloudTopM`), no una constante.
                // Estaba cableada a `h + 1700`, y eso daba el mismo desarrollo vertical a una capa
                // de buen tiempo que a una tormenta — justo lo que distingue a las dos. Medido en el
                // modelo: ~430 m sin lluvia, ~3850 m descargando (8,9x). El fallback se conserva por
                // si el UBO llega sin clima (planeta sin `WeatherSystem` configurado).
                // El CUMULO trae su cima del clima; las otras dos usan su espesor tipico.
                float top = (layer == 2) ? ((u_planet.w > h + 1.0) ? u_planet.w : h + slabM)
                                         : (h + slabM);

                // ⚠️ Y EL ESPESOR QUE SE MUESTREA SE ACOTA, QUE ES LA RAIZ DEL "SE VEN COMO CAPAS".
                // La separacion HORIZONTAL entre la muestra de la base y la del techo crece como
                // `1/sin(elevacion)`: mirando en oblicuo, 600 m de espesor se convierten en KILOMETROS
                // de recorrido lateral. Pasado ~un elemento de nube, las muestras dejan de ser "la
                // misma nube a distinta altura" y pasan a ser NUBES DISTINTAS — y lo que aportan no es
                // espesor, son copias fantasma. Con 4 pasos eso se ve como franjas paralelas.
                //
                // El elemento mide ~0,77/scale km (ver la nota de `scale`), asi que se limita el
                // espesor muestreado a la altura que produce esa separacion lateral. La opacidad del
                // camino largo NO se pierde: la pone `path` analiticamente, unas lineas mas abajo.
                {
                    float latPorM   = 1.0 / max(t, 0.05);            // m laterales por m de altura
                    float elementoM = (0.77 / max(scale, 1e-3)) * 1000.0;
                    top = min(top, h + max(elementoM / latPorM, 60.0));
                }
                // ⚠️ JITTER, Y NO ES OPCIONAL CON TAN POCOS PASOS. Con las alturas fijas en
                // `(sI + 0.5)/STEPS`, los 2-4 puntos de muestreo son los MISMOS para todos los
                // pixeles: mirando en oblicuo, cada altura corta la losa en un sitio horizontal
                // distinto y lo que se ve no es una nube gruesa sino la MISMA nube repetida 4 veces
                // en franjas paralelas. Reportado mirando la pantalla: *"se ven como con capas en vez
                // de como un bloque de gas"*. Es exactamente el artefacto que `cloud_vol.frag` ya
                // describe para su marcha ("empezar todos los rayos en el mismo sitio pone la
                // estructura de la marcha EN PANTALLA") y se cura igual: desplazar el arranque una
                // fraccion distinta por pixel convierte las franjas en ruido, que el ojo perdona.
                // Cuesta cero: son los mismos pasos, movidos.
                float jit = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
                float acc = 0.0, wsum = 0.0;
                for (int sI = 0; sI < STEPS; ++sI) {
                    float f  = (float(sI) + jit) / float(STEPS);
                    float hs = mix(h, top, f);
                    bool  ok2; float d2;
                    vec2  ps = cloudPlane(dir, e1, e2, hs, ok2, d2);
                    if (!ok2) continue;
                    // Perfil vertical: densa en el medio, deshilachada en la panza y en la cima. Es
                    // lo que da la forma de coliflor en vez de un ladrillo.
                    float prof = smoothstep(0.0, 0.28, f) * (1.0 - smoothstep(0.62, 1.0, f));
                    // PROFUNDIDAD OPTICA: cada tramo suma lo que le SOBRA al umbral, no su valor.
                    // Un tramo por debajo del umbral no es "poca nube", es NADA — y promediarlo con
                    // el nucleo es lo que aplanaba los picos.
                    acc  += max(cloudField(ps * scale, wind, fieldPx) - lo, 0.0) * prof;
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
                //
                // ⚠️⚠️ Y ESTO ES LO QUE FALTABA APLICAR. El comentario de arriba lleva escrito desde el
                // principio *"NO SE PROMEDIA... salen POCAS nubes y TENUES"* — y la linea era
                // literalmente `acc / wsum` sobre el valor CRUDO del campo, o sea la media que el
                // propio comentario declaraba mal. El sintoma estaba medido y coincidia: con
                // cobertura pedida 0,80 el cielo salia tapado un **5,9 %** (`sky_layers_have_depth`).
                // Ahora `acc` acumula lo que sobra del umbral, asi que `dens` ES profundidad optica.
                dens = (wsum > 0.0) ? acc / wsum : 0.0;
            }

            // ⚠️ BORDE DURO = el look de anime. La rampa era `smoothstep(lo, lo + 0.15, dens)` para
            // todas; con `sharp` la nube baja corta en 0,07 y su silueta se lee como una FORMA
            // recortada, no como un degradado de niebla. La alta conserva rampa ancha (0,30) porque
            // un cirro con borde duro no parece un cirro, parece un recorte.
            // BEER-LAMBERT sobre la profundidad optica, que es lo que el comentario de `dens` pide.
            // `sharp` deja de ser el ancho de una rampa y pasa a ser la ESCALA DE EXTINCION: cuanto
            // campo por encima del umbral hace falta para tapar. El cirro (0,30) necesita mucho y se
            // queda en velo; el cumulo (0,07) tapa en cuanto asoma, que es su borde duro de siempre.
            // El umbral ya esta DENTRO de `dens`, asi que no se resta dos veces.
            float cov = 1.0 - exp(-dens / max(sharp, 1e-3));
            cov = cov * cov * (3.0 - 2.0 * cov);   // el mismo afilado de silueta
            // CAMINO ÓPTICO: cuanto más rasante es la vista, más losa se atraviesa → más opaca.
            // `t` es el coseno respecto al cénit, así que 1/t es el camino relativo. Se aplica a la
            // COBERTURA y no a la densidad: aplicado a la densidad desplazaba el umbral y cambiaba la
            // FORMA de la nube con el ángulo de vista, que es justo lo que no debe pasar.
            // ⚠️ TAMBIEN ESTABA BAJO `if (layer == 2)`, o sea aplicado solo a la capa que ya no se
            // dibuja. Sin el, una capa se ve IGUAL de opaca sobre tu cabeza que en el horizonte, que
            // es la firma de una calcomania: una losa de verdad tapa mas cuanto mas la atraviesas.
            // El tope es por capa — un cirro no debe cerrarse del todo en el horizonte.
            {
                float path = clamp(1.0 / max(t, 0.14), 1.0, pathMax);
                cov = 1.0 - pow(max(1.0 - cov, 0.0), path);
            }

            // ⚠️⚠️ AQUI ESTABA EL ANILLO ENTERO, Y ERA UNA VENTANA DURA DE ELEVACION.
            //
            //     band = smoothstep(hb, hb+0.22, t) * (1.0 - smoothstep(0.62, 1.01, t));
            //
            // Con `hb ≈ 0,044`, el primer factor no llega a 1 hasta `t = 0,264`, o sea **15,3 grados**:
            // por debajo la capa esta a medias o apagada. Y el segundo la APAGA desde `t = 0,62`
            // (38,3 grados) hasta el cenit, donde vale **0,026** — el 97 % borrado. Justo donde mejor
            // se ve una capa de nubes, que es mirando hacia arriba.
            //
            // Medido antes de tocarlo (`sky_layers_have_depth`): la nube vivia entre 5,8 y 21,1 grados
            // con 0 % por encima de 25, y al cenit la cobertura pedida 0,80 daba **6,6 %** de cielo
            // tapado. Las dos cifras salen de esta linea. Un anillo a altura fija de pantalla es
            // exactamente lo que se lee como "las nubes son un plano 2D".
            //
            // El desvanecido del cenit NO tiene por que existir: se puso cuando la capa era un plano
            // sin altitud y la formula reventaba mirando arriba; con la interseccion rayo-esfera de
            // `cloudPlane` no hay nada que tapar ahi. Se retira entero.
            //
            // Del horizonte se conserva un desvanecido MUCHO mas corto (0,6 a 2,9 grados) y solo por
            // lo que de verdad pasa ahi: la coordenada de la capa se satura (ver `cloudPlane`) y el
            // ultimo grado es una franja donde ya no hay estructura que dibujar.
            float hb   = mix(0.010, 0.018, thick);
            float band = smoothstep(hb, hb + 0.040, t);
            float cloud = cov * band * day * u_atmo;
            if (cloud <= 0.001) continue;

            // Iluminación volumétrica (Beer): densidad acumulada HACIA el sol → base oscura y cara
            // al sol brillante. El paso se escala con la altura de la capa para que la sombra tenga
            // el tamaño que le toca a su tamaño de nube.
            vec2  ls = ls2 * (0.20 * scale / 0.30);
            float sh = 0.0;
            for (int i = 1; i <= 3; ++i)
                sh += smoothstep(lo, lo + 0.26, cloudField(base + ls * float(i), wind, fieldPx));
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
    // La atmosfera VISTA DESDE FUERA se suma al espacio, no al cielo: dentro, el domo ya la modela y
    // `mix` la tapa entera. Ver `harukaAtmoLimb`.
    spaceC += harukaAtmoLimb(dir);
    vec3 outC   = mix(spaceC, sky, u_atmo);
    // (El cuerpo del Sol = corona-objeto emisiva, se dibuja en el pase de objetos sobre este
    //  fondo, igual dentro y fuera de la atmósfera → sin "dos soles".)

    FragColor = vec4(outC, 1.0);
}
