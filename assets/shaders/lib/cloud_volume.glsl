/**
 * @file lib/cloud_volume.glsl
 * @brief LA NUBE COMO VOLUMEN: campo de densidad y perfil vertical, en un solo sitio.
 *
 * ── POR QUÉ EXISTE ──────────────────────────────────────────────────────────────────────────────
 * El cúmulo se pintaba dentro de `sky.frag`, en el pase de FONDO: sin profundidad, antes que la
 * escena, con un triángulo a pantalla completa. Eso lo convertía en un telón — cualquier objeto lo
 * tapaba aunque estuviera detrás, y sobre todo **no había un "dentro"**: era imposible atravesar
 * una nube porque un fondo no tiene interior. La única forma de fingir el interior habría sido un
 * efecto de pantalla aparte ("blanqueo"), que es un parche sobre el síntoma.
 *
 * La nube no puede ser una malla: si le pones superficie, desde dentro ves caras traseras y al
 * cruzarla hay un salto. Una nube es MEDIO PARTICIPATIVO — un campo de densidad que se recorre.
 * Con un raymarch, estar dentro deja de ser un caso especial: si el rayo empieza dentro, marcha
 * desde dentro, y la pérdida de visibilidad SALE SOLA en vez de programarse.
 *
 * ── UNA SOLA FUENTE ─────────────────────────────────────────────────────────────────────────────
 * ⚠️ El perfil vertical de aquí es el MISMO que `WeatherSystem::cloudDensityAt` en C++. Están
 * atados por `test_weather_3d` (`kProfileRise`/`kProfileFall`). Si divergen, el jugador entraría en
 * la niebla a una altura distinta de la que se ve la nube — que es exactamente el fallo que tenía
 * el clima cuando la CPU y el shader calculaban la nubosidad cada uno por su cuenta.
 */
#ifndef HARUKA_CLOUD_VOLUME_GLSL
#define HARUKA_CLOUD_VOLUME_GLSL

// Bordes de la losa, en fracción del grosor. GEMELOS de los de weather_system.cpp.
// La base es un plano bastante definido (el nivel de condensación es una cota, y por eso todas las
// nubes de un cielo tienen la panza a la misma altura); el techo se desfleca.
const float HARUKA_CLOUD_RISE = 0.28;
const float HARUKA_CLOUD_FALL = 0.62;

/** @brief Perfil vertical dentro de la losa. `t` = 0 en la base, 1 en el techo. */
float harukaCloudProfile(float t) {
    if (t <= 0.0 || t >= 1.0) return 0.0;
    float rise = smoothstep(0.0, HARUKA_CLOUD_RISE, t);
    float fall = 1.0 - smoothstep(HARUKA_CLOUD_FALL, 1.0, t);
    return rise * fall;
}

// ── RUIDO ───────────────────────────────────────────────────────────────────────────────────────
// Es el mismo value-noise + fBm que ya usaba el cielo. Vive aquí para que el fondo (cirro,
// altocúmulo) y el volumen (cúmulo) compartan la forma: si usaran ruidos distintos, la capa alta y
// la baja parecerían de dos cielos diferentes.
float harukaCloudHash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

/** @brief Ruido de valor 3D (trilineal). Un solo hash para todo el cielo: la capa alta y la baja
 *  tienen que parecer del mismo. */
float harukaCloudVNoise3(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(harukaCloudHash13(i + vec3(0, 0, 0)), harukaCloudHash13(i + vec3(1, 0, 0)), f.x);
    float b = mix(harukaCloudHash13(i + vec3(0, 1, 0)), harukaCloudHash13(i + vec3(1, 1, 0)), f.x);
    float c = mix(harukaCloudHash13(i + vec3(0, 0, 1)), harukaCloudHash13(i + vec3(1, 0, 1)), f.x);
    float d = mix(harukaCloudHash13(i + vec3(0, 1, 1)), harukaCloudHash13(i + vec3(1, 1, 1)), f.x);
    return mix(mix(a, b, f.y), mix(c, d, f.y), f.z);
}

// ⚠️ EL CAMPO ERA 2D Y POR ESO LAS NUBES IBAN PEGADAS AL JUGADOR (2026-09-07) ────────────────────
//
// Un fBm 2D necesita DOS COORDENADAS, y sobre una esfera eso obliga a elegir un marco tangente.
// `cloud_vol.frag` lo sacaba del OJO:
//
//     up0 = normalize(-u_planetC.xyz);            // la vertical DE LA CAMARA
//     e1  = normalize(cross(vec3(0,1,0), up0));
//     e2  = cross(up0, e1);
//     uv  = vec2(dot(p, e1), dot(p, e2)) * fscale;
//
// La prueba de que eso ata el cielo a quien mira cabe en una linea: e1 y e2 son PERPENDICULARES a
// up0, asi que el punto del cenit —el que esta justo encima de la camara, p = up0*(R+alt)— da
// `uv = (0,0)` SIEMPRE, estes donde estes en el planeta. O sea que el mismo rasgo del campo se
// queda clavado sobre tu cabeza mientras caminas: las nubes no se quedan atras nunca. Reportado
// como "las nubes se mueven con la posicion del personaje", y es literalmente eso.
//
// No se arregla con otro marco tangente: por el teorema de la bola peluda NO EXISTE un campo
// tangente continuo y no nulo sobre una esfera, asi que cualquier eleccion 2D tiene una
// singularidad (equirect la pone en los polos y ademas parte una costura en el antimeridiano; una
// ortografica fija estira las nubes en bandas por medio planeta). El dominio correcto de un campo
// sobre una esfera es el ESPACIO, no un plano: se indexa por la posicion 3D relativa al CENTRO del
// planeta y deja de haber marco que elegir.
//
// ⚠️ COSTE, dicho con numeros y no con adjetivos. El ruido 3D cuesta 8 hashes por octava contra 4
// del 2D, asi que la traduccion ingenua (mismas 5 octavas, mismas 4 llamadas) seria 160 hashes por
// muestra contra los 80 de antes: DOBLE en el bucle mas caliente del motor. No hace falta pagarlo:
//   · el WARP es un desplazamiento suave del dominio — dos octavas ya lo dan, las tres de arriba
//     solo le ponen grano a un offset;
//   · la octava 5 de la FORMA pesa 1/32 y esa banda ya la cubre `harukaCloudDetail`, que sigue ahi.
// Quedan 3*(2*8) + 2*(4*8) = 112 hashes contra 80: +40 %, no +100 %. Sin medir en maquina — el
// pase se dibuja a 1/4 de lado y se compone, asi que el numero a mirar es el del frame, con `perf`.
float harukaCloudFbm3(vec3 p, int octaves) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < octaves; ++i) { v += a * harukaCloudVNoise3(p); p *= 2.0; a *= 0.5; }
    return v;
}

/** @brief Forma de la nube en un punto, con el dominio FIJO AL PLANETA.
 *
 *  @param p     posicion relativa al CENTRO del planeta, ya en unidades de rasgo (`* fscale`).
 *  @param wind  deriva del campo con el tiempo, ya en el marco del punto (ver `g_wind3`).
 *
 *  Misma receta que tenia la version 2D —domain warp para que salgan formas organicas y no
 *  manchas, mas una erosion de detalle— solo que sobre un dominio de tres dimensiones. */
float harukaCloudField3(vec3 p, vec3 wind) {
    vec3 warp = vec3(harukaCloudFbm3(p * 0.6 + wind * 0.5,        2),
                     harukaCloudFbm3(p * 0.6 + 5.2 - wind * 0.5,  2),
                     harukaCloudFbm3(p * 0.6 + 11.7 + wind * 0.3, 2));
    float d = harukaCloudFbm3(p * 1.3 + warp * 1.4 + wind, 4);
    d -= 0.20 * harukaCloudFbm3(p * 4.0 - wind * 2.0, 4);
    return d;
}

// ── DE LÁMINA A CUERPO ──────────────────────────────────────────────────────────────────────────
// Lo de arriba es un campo HORIZONTAL (varía muchísimo más de lado que en vertical, porque la escala
// del rasgo son kilómetros y el espesor cientos de metros). Multiplicado por un perfil que solo depende de la altura RELATIVA,
// toda nube sale un PRISMA: la misma sección de la base al techo. Medido sobre el campo real, el
// rasgo horizontal típico son 2857 m contra 440-980 m de espesor con buen tiempo — o sea entre 6,5:1
// y 2,9:1. Eso no se ve como un cúmulo por mucho que se recorra: se ve como una lámina, que es justo
// lo que se reportó. Las dos piezas que siguen son las que le dan volumen, y ninguna toca el perfil
// compartido con la CPU: las dos trabajan DENTRO de esa envolvente.

/** Anchura del borde del umbral, en unidades del campo. La cobertura decide QUÉ FRACCIÓN del cielo
 *  tiene nube (por el umbral); esta constante decide cómo de rápido se pasa de aire a nube. */
const float HARUKA_CLOUD_SOFT = 0.085;
/** Cuánto erosiona el detalle 3D. 0 = prisma liso; alto = nube deshilachada sin cuerpo. */
const float HARUKA_CLOUD_ERODE = 0.55;
/** Altura MÍNIMA de la nube más débil, en fracción de la losa. Sin este suelo, un punto apenas por
 *  encima del umbral daría una nube de espesor ~0 y el borde del cúmulo se afilaría a un plano. */
const float HARUKA_CLOUD_MINTOP = 0.22;

/**
 * @brief Fuerza de la nube en [0,1] a partir del campo y el umbral.
 *
 * ⚠️ ESTO ES LA MITAD DE "no hay casi nubes". Antes la densidad era `max(d - lo, 0)` — un número
 * diminuto: medido sobre el campo, lo que supera el umbral lo hace por 0,054-0,21 de media. Con la
 * cobertura MEDIANA del planeta (0,111) la opacidad resultante en el cénit era **0,016**: el pase se
 * ejecutaba y no se veía. Normalizando a [0,1], el núcleo de una nube es 1 y la extinción vuelve a
 * ser lo que dice ser —un coeficiente por metro— en vez de un factor de escala accidental.
 */
float harukaCloudStrength(float fieldValue, float threshold) {
    return smoothstep(threshold, threshold + HARUKA_CLOUD_SOFT, fieldValue);
}

/** @brief Erosión de detalle: 3 octavas de ruido 3D. Solo se llama donde YA hay nube (ver
 *  `harukaCloudDensity`), que es lo que la hace asequible: el 3D no se paga en el cielo vacío, y
 *  cuesta 8 hashes por octava contra los 80 del campo 2D — subir octavas AQUÍ es barato. */
float harukaCloudDetail(vec3 q) {
    return 0.55 * harukaCloudVNoise3(q)
         + 0.30 * harukaCloudVNoise3(q * 2.7 + 11.3)
         + 0.15 * harukaCloudVNoise3(q * 6.9 + 41.7);
}

/**
 * @brief Densidad de la nube en un punto, en [0,1].
 *
 * @param strength  salida de `harukaCloudStrength`: cuánta nube hay en esta VERTICAL.
 * @param f         altura relativa en la losa [0,1] (0 = base del clima, 1 = techo del clima).
 * @param q         punto para el detalle 3D, ya en unidades de detalle.
 *
 * ── EL REMAPEO DE ALTURA, que es lo que crea la forma ────────────────────────────────────────────
 * La losa del clima (base→techo) es el TECHO POSIBLE, no la altura de cada nube. Aquí la nube ocupa
 * solo la fracción de abajo proporcional a su fuerza: un punto flojo del campo da un jirón pegado a
 * la base, uno fuerte llena la losa entera. Esa es la razón de que un cielo de cúmulos tenga las
 * panzas TODAS a la misma altura y las cimas a alturas distintas — y de que el resultado se lea como
 * un cuerpo abombado en vez de como un tablón recortado.
 *
 * ⚠️ La nube sigue siendo un SUBCONJUNTO de la envolvente que evalúa `WeatherSystem::cloudDensityAt`
 * en C++: el remapeo solo puede ACORTAR (`localTop <= 1`). La CPU responde "¿puede haber nube a esta
 * altura?" y el shader "¿la hay aquí exactamente?". Por eso el perfil compartido no se toca.
 */
float harukaCloudDensity(float strength, float f, vec3 q) {
    if (strength <= 0.0 || f <= 0.0 || f >= 1.0) return 0.0;

    // Techo LOCAL de esta nube dentro de la losa. El suelo evita el filo de espesor cero.
    const float localTop = mix(HARUKA_CLOUD_MINTOP, 1.0, strength);
    if (f >= localTop) return 0.0;

    const float body = harukaCloudProfile(f / localTop) * strength;
    if (body <= 0.0) return 0.0;

    // El detalle muerde SOBRE TODO el borde, pero ya no deja el núcleo liso.
    //
    // ⚠️ Antes el peso era `1 - smoothstep(0.30, 0.85, body)`, o sea EXACTAMENTE CERO en el cuerpo
    // de la nube: el núcleo salía como una mancha de color plano, y con el cielo cubierto —donde
    // casi todo el campo es núcleo— eso es una pared gris sin un solo rasgo. Es la "poca definición
    // de nube" que se reportó. Ahora el núcleo conserva un 35 % del detalle: suficiente para que se
    // lea el grano y la nube tenga superficie, sin convertirla en un encaje que deje pasar el sol.
    const float edge = mix(0.35, 1.0, 1.0 - smoothstep(0.30, 0.85, body));
    return max(body - HARUKA_CLOUD_ERODE * edge * harukaCloudDetail(q), 0.0);
}

/**
 * @brief Intersección de un rayo con un casquete esférico de radio `R`, con el origen del rayo en
 *        `ro` relativo al CENTRO de la esfera. Devuelve las dos raíces en `t0`/`t1`.
 * @return false si el rayo no corta.
 *
 * Se usa para entrar y salir de la losa: la nube es una cáscara entre R+base y R+techo, así que la
 * franja que hay que recorrer es la diferencia entre las dos esferas. Sobre un planeta esto NO es
 * lo mismo que dos planos horizontales: mirando al horizonte el rayo atraviesa muchísima más nube
 * que mirando al cénit, y eso es lo que hace que el cielo se cierre hacia el horizonte.
 */
bool harukaRaySphere(vec3 ro, vec3 rd, float R, out float t0, out float t1) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - R * R;
    float disc = b * b - c;
    if (disc < 0.0) { t0 = 0.0; t1 = 0.0; return false; }
    float s = sqrt(disc);
    t0 = -b - s;
    t1 = -b + s;
    return true;
}

#endif // HARUKA_CLOUD_VOLUME_GLSL
