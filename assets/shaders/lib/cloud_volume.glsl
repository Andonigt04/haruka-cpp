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
// ⚠️ BASE PLANA, TECHO REDONDO. Eran 0,28 / 0,62: la nube entraba en 670 m y se deshilachaba desde
// 1 500 m, simetrica arriba y abajo, y con el campo 3D isotropo cada nube salia como una BOLA
// (medido en `cloud_shape_3d`: variacion de altura de la base 374 m = la del techo 374 m). Un cumulo
// real nace en el nivel de condensacion, que es PLANO: la base se forma en ~150 m (0,06 de 2,4 km)
// y todo lo redondo va arriba. Gemelos en `WeatherSystem::kProfileRise/Fall` (los ata `weather_3d`).
const float HARUKA_CLOUD_RISE = 0.06;
const float HARUKA_CLOUD_FALL = 0.50;
/// Compresion VERTICAL de la coordenada del campo 3D del cumulo: la estructura horizontal del campo
/// se mantiene y a lo alto de la losa casi no cambia, asi que la nube es una COLUMNA con la base del
/// perfil y el techo de la erosion, no una bola. 1,0 = isotropo (lo de antes).
const float HARUKA_CLOUD_VERT_SQUASH = 0.15;
/// Escala HORIZONTAL del campo del cumulo: 1,0 daba nubes de ~1 km de ancho (el fundamental del fbm
/// a 1,43 km/unidad); a 0,6 salen de ~1,7 km, el tamano de un cumulo de buen tiempo.
const float HARUKA_CLOUD_HORIZ_SCALE = 0.6;

/// La posicion del campo del cumulo con la vertical COMPRIMIDA, sobre la esfera. `p` relativo al
/// centro del planeta (m), `fscale` 1/m. Horizontal: la posicion sobre la esfera a radio R (o sea,
/// d·fscale por metro andado); vertical: la altura sobre R comprimida por SQUASH.
/// ⚠️ ERA `pf − up·(dot(pf,up)·(1−SQUASH))`, Y ESO NO COMPRIME NADA: `p` es RADIAL desde el centro,
/// asi que `pf − 0,85·|pf|·up = 0,15·pf` — un escalado UNIFORME de todo el campo (x0,15 tambien en
/// horizontal). El cumulo se muestreaba con celdas de ~12 km e isotropo: bolas enormes recortadas
/// por la losa = tortas planas por arriba y por abajo ("nubes grandes planas", "la base es plana").
/// La sonda de densidad (`cloud_density_probe.comp`) usaba la version plana correcta, por eso sus
/// medidas (3,3:1, coliflor) no eran las del pase. Medido al arreglarlo: la base dibujada pasa de
/// 66 m de ondulacion a ver los bultos, y las nubes al tamano de la sonda.
vec3 harukaCloudSquashedPos(vec3 p, float R, float alt, float fscale) {
    const vec3 upS = p / (R + alt);
    return upS * ((R + alt * HARUKA_CLOUD_VERT_SQUASH) * fscale) * HARUKA_CLOUD_HORIZ_SCALE;
}

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
/// ⚠️ ERA `p = fract(p*0.1031); p += dot(p, p.yzx+33.33); fract((p.x+p.y)*p.z)` (el "hash 13" de
/// Hoskins), y en coma flotante es CAOTICO: con p ~ 3 000 (las coordenadas que ve el pase) un ulp
/// de diferencia en `p*0.1031` se multiplica por ~100 dos veces y el hash cambia del todo. Los
/// compiladores no redondean igual: con el MISMO shader y la MISMA GPU, OpenGL y Vulkan daban
/// hashes distintos en el 39 % de los nudos (sonda `cloud_noise_probe`), y de ahi un cirro al
/// cenit del 53,7 % en GL contra 67,3 % en Vulkan, con facetas rectas en Vulkan (dos esquinas de
/// una celda con hashes "de otro compilador" = una arista recta). O sea: no existia UN campo de
/// nubes, existia uno por compilador.
/// Ademas `fract(p·0,1031)` es casi lineal entre nudos vecinos: el ruido viejo estaba
/// correlacionado por los ejes de la malla (hecho exacto en enteros salia un cumulo alargado 2,2:1
/// desde el nadir), y lo unico que lo disimulaba era el caos de redondeo. Los nudos son ENTEROS y
/// se mezclan como enteros (pcg3d, Jarzynski & Olano 2020): exacto en cualquier compilador y GPU,
/// blanco e isotropo. El tamano de las celdas del cumulo se recalibro para este ruido
/// (HARUKA_CLOUD_HORIZ_SCALE).
float harukaCloudPcg3d(ivec3 ip) {
    uvec3 v = uvec3(ip + ivec3(0x40000000));
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return float(v.x & 0xffffffu) / 16777216.0;
}
/// El hash de las nubes, EXACTO y con la misma estadistica que tenia:
///   · la primera etapa del "hash 13" (`fract(p·0,1031)`) se hace en enteros: `(p·1031 mod 10000)/10000`,
///     igual en cualquier compilador; las dos etapas restantes van con entradas en [0,1) y un ulp
///     ahi son 1e-4 de salida, no otro hash;
///   · ese hash es casi LINEAL entre nudos vecinos (por eso el ruido tenia energia de baja frecuencia
///     y los cumulos salian coherentes) y por lo mismo tiende a rampas por los ejes de la malla.
///     En coma flotante el caos de redondeo revolvia ~18 % de los nudos y rompia las rampas (hecho
///     exacto sin mas, el cumulo salia alargado 2,2:1 desde el nadir). Aqui ese 18 % se revuelve A
///     PROPOSITO con pcg3d, decidido por el propio nudo: determinista, y el mismo aspecto.
/// ⚠️ Se probo el hash blanco puro (pcg3d) y recalibrar: sin la correlacion el cumulo sale en
/// jirones (presencia 7,4 -> 1,8 %), y un hash suavizado (media de 8) cuesta x9 el pase (1 790 ms).
/// El "hash 13" de Hoskins de siempre, hecho DETERMINISTA:
///   · el nudo se envuelve a 8 192 (`& 8191`: el ruido se repite cada 8 192 nudos = 11 000 km en el
///     cumulo, invisible) para que `nudo · 0,1031` sea pequeño y UN solo producto IEEE;
///   · ese producto va `precise`: sin eso el compilador lo fundia con el `floor` de `fract` en un
///     FMA (o no), y el error de redondeo (1e-5) se amplificaba x100 dos veces: hashes distintos en
///     el 39 % de los nudos entre OpenGL y Vulkan EN LA MISMA GPU. Las dos etapas siguientes tienen
///     entradas en [0,1): un ulp ahi son 1e-4 en la salida, no otro hash.
/// ⚠️ NO se cambia por un hash "bueno" (pcg3d): con la misma media y sigma del campo (medido:
/// 0,375 / 0,109 los dos) el cumulo salia en jirones grises (presencia 7,4 -> 1,8 %). La razon esta
/// en las OCTAVAS: `fract(2i·0,1031) = fract(2·fract(i·0,1031))`, asi que en este hash la octava
/// k+1 sigue a la k y los picos se alinean — es lo que da cuerpos coherentes. Con nudos
/// independientes por octava no hay cuerpos. Todo el cumulo esta calibrado sobre ESTO.
float harukaCloudHash13(vec3 p) {
    const ivec3 ip = ivec3(floor(p + 0.5)) & ivec3(8191);
    // TODO `precise` y con el orden de las operaciones escrito: `precise` obliga a evaluar el arbol
    // tal cual (sin FMA ni reasociacion), y asi los dos compiladores hacen las mismas operaciones
    // IEEE en el mismo orden. Con un `dot()` suelto quedaban flips en el 2 % de los nudos.
    precise vec3 t = vec3(ip) * 0.1031;
    precise vec3 q = fract(t);
    precise float d = ((q.x * (q.y + 33.33)) + (q.y * (q.z + 33.33))) + (q.z * (q.x + 33.33));
    precise vec3 r = q + d;
    precise float m = (r.x + r.y) * r.z;
    return fract(m);
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

/** @brief fBm con LOD: cada octava se funde a su MEDIA (0,5·a) cuando su longitud de onda no cubre
 *  varias veces la huella de la muestra (`fp`, en las unidades de `p`). Fundir a la media en vez de
 *  quitar la octava mantiene el nivel del campo —y con él la cobertura— igual a cualquier distancia.
 *
 *  ⚠️ POR QUÉ. El campo del cúmulo tiene una octava a `p·4` con cuatro suboctavas: rasgos de 45 m.
 *  La marcha muestrea a 50-360 m con jitter por píxel, así que dos píxeles vecinos ven valores
 *  distintos de un rasgo que no pueden resolver: GRANO. En una nube densa no se nota (satura en dos
 *  muestras); en un cúmulo de buen tiempo, translúcido, se ve entero (27 % de píxeles a más de
 *  10/255 de su media 3x3, banco con horneado real). La regla es la de siempre: nada por debajo
 *  del paso ni del píxel. */
///  ⚠️ `fp0` es la huella para la octava FUNDAMENTAL y `fp` para el resto, y no son la misma: la
///  fundamental es la FORMA de la nube (1,1 km) y sólo puede fundirse cuando el PÍXEL no la resuelve
///  (desde órbita). Fundirla también por el paso de la marcha —que crece hasta 430 m a 2,3 km
///  mirando al horizonte— dejaba el cielo entero como una lámina uniforme de la cobertura media,
///  moteada por la erosión: "todo el cielo lleno de textura". Las octavas finas sí van por el paso.
/// ⚠️ LA BANDA DE DESVANECIDO DEL LOD ESTABA 3x POR ENCIMA DE NYQUIST. Era `(wl/6, wl/3)`: una
/// octava se apagaba DEL TODO con tres muestras por longitud de onda, cuando Nyquist pide dos. Como
/// el pase corre a 1/4 de lado, la fundamental del cumulo (1,4 km) empezaba a fundirse a 77 km y
/// desaparecia a 154 km: a esas distancias la nube perdia la FORMA y quedaba su cobertura, o sea un
/// manchurron — "si bajas de cierta altura se crea un fantasma de una nube" (Andoni), porque al
/// bajar la capa se ve mas rasante y su entrada se aleja. Con `(wl/3, wl/1,5)` la octava vive hasta
/// 1,5 muestras por periodo y se apaga en el limite: la forma llega al doble de distancia. El precio
/// es alias, y eso el banco lo mide (`grano %`): si sube, este es el sitio.
/// Las octavas FINAS se quedan donde estaban (medido: aflojarlas a Nyquist sube el grano del
/// nivel medio de 6,4 a 16 % y el del cumulo de 55 a 63 % — el muestreo de la marcha lleva jitter
/// por pixel y pasos irregulares, asi que su Nyquist efectivo es peor que el teorico).
const float HARUKA_CLOUD_LOD_LO = 1.0 / 6.0;
const float HARUKA_CLOUD_LOD_HI = 1.0 / 3.0;
/// La FUNDAMENTAL no: es la FORMA de la nube (1,4 km), su alias es de baja frecuencia (bultos
/// grandes, no moteado) y es lo que se pierde a 77 km con la banda conservadora. Se lleva a Nyquist.
const float HARUKA_CLOUD_LOD0_LO = 1.0 / 4.0;
const float HARUKA_CLOUD_LOD0_HI = 1.0 / 2.0;
float harukaCloudFbm3LOD(vec3 p, int octaves, float fp0, float fp) {
    float v = 0.0, a = 0.5, wl = 1.0;
    for (int i = 0; i < octaves; ++i) {
        const float w = (i == 0) ? (1.0 - smoothstep(wl * HARUKA_CLOUD_LOD0_LO, wl * HARUKA_CLOUD_LOD0_HI, fp0))
                                 : (1.0 - smoothstep(wl * HARUKA_CLOUD_LOD_LO,  wl * HARUKA_CLOUD_LOD_HI,  fp));
        // Una octava que el LOD descarta del todo no se evalua: es lo que hace asequible la marcha
        // hacia el Sol (huella gruesa, 1-2 octavas) y las muestras lejanas.
        v += (w > 0.0) ? a * mix(0.5, harukaCloudVNoise3(p), w) : a * 0.5;
        p *= 2.0; a *= 0.5; wl *= 0.5;
    }
    return v;
}

/** @brief `harukaCloudField3` con LOD (ver `harukaCloudFbm3LOD`). `fp` = huella de la muestra en
 *  unidades de `p` (metros × fscale): el mayor entre el paso de la marcha y el píxel. */
float harukaCloudField3LOD(vec3 p, vec3 wind, float fp0, float fp) {
    // ⚠️ Se probo con las mallas giradas por octava (`harukaCloudFbm3LODRot`, como las capas) y el
    // banco dio MAS cortes rectos en el cumulo (107 contra 53) y lo saco alargado desde arriba: una
    // malla girada sigue siendo una malla, y el cumulo ya lleva warp, cuatro octavas y erosion.
    // Se queda como estaba; la rotacion es solo para las capas, que no tienen nada de eso.
    vec3 warp = vec3(harukaCloudFbm3LOD(p * 0.6 + wind * 0.5,        2, fp0 * 0.6, fp * 0.6),
                     harukaCloudFbm3LOD(p * 0.6 + 5.2 - wind * 0.5,  2, fp0 * 0.6, fp * 0.6),
                     harukaCloudFbm3LOD(p * 0.6 + 11.7 + wind * 0.3, 2, fp0 * 0.6, fp * 0.6));
    float d = harukaCloudFbm3LOD(p * 1.3 + warp * 1.4 + wind, 4, fp0 * 1.3, fp * 1.3);
    d -= 0.20 * harukaCloudFbm3LOD(p * 4.0 - wind * 2.0, 4, fp * 4.0, fp * 4.0);   // esta es detalle entera
    return d;
}

/** @brief El campo de las CAPAS (altocúmulo, cirro): el mismo warp, pero LIMITADO EN BANDA.
 *
 *  ⚠️ `harukaCloudField3` lleva una octava a `p·4` con cuatro suboctavas: rasgos de 190 m a la
 *  escala del cirro y de 50 m a la del altocúmulo. La marcha da pasos de 50-360 m con jitter por
 *  píxel, así que cada píxel muestrea ese ruido en sitios distintos y sale un valor distinto: MOTAS
 *  de 1 px por todo el cielo (medido a resolución completa; con el recorte de escena apagado
 *  seguían, con el perfil de capa seguían — era el campo). El cúmulo lo tolera porque satura en dos
 *  pasos y la varianza se esconde bajo el alfa; una capa translúcida la enseña entera. Aquí, dos
 *  octavas: el rasgo más fino queda en 2,3 km (cirro) y 600 m (altocúmulo), varias veces el paso. */
/// ⚠️ LAS CAPAS SALIAN COMO POLIGONOS. El ruido de valor es una malla cubica interpolada: umbralizado
/// con un borde de 0,085 (`harukaCloudStrength`), sus contornos son TRAMOS RECTOS entre nudos de la
/// malla, y en las capas —dos octavas, sin detalle que lo rompa— cada hueco del cirro era un rombo
/// de 4,6 km de lados rectos (captura desde 3 km, 16-09: "una difuminada con cortes"). El cumulo no
/// lo ensena porque lleva 4 octavas, warp y erosion. Aqui cada octava va sobre una malla GIRADA
/// respecto a la anterior: los tramos rectos de una no coinciden con los de la otra y el contorno
/// deja de ser un poligono. Cuesta lo mismo (una multiplicacion por matriz por octava).
const mat3 HARUKA_CLOUD_ROT_A = mat3( 0.80, -0.36,  0.48,
                                      0.48,  0.80, -0.36,
                                     -0.36,  0.48,  0.80);   // ~37 grados sobre (1,1,1)
const mat3 HARUKA_CLOUD_ROT_B = mat3( 0.36,  0.48,  0.80,
                                      0.80,  0.36, -0.48,
                                     -0.48,  0.80, -0.36);   // otra, sin eje en comun con la anterior
float harukaCloudFbm3LODRot(vec3 p, int octaves, float fp0, float fp) {
    float v = 0.0, a = 0.5, wl = 1.0;
    for (int i = 0; i < octaves; ++i) {
        const float w = (i == 0) ? (1.0 - smoothstep(wl * HARUKA_CLOUD_LOD0_LO, wl * HARUKA_CLOUD_LOD0_HI, fp0))
                                 : (1.0 - smoothstep(wl * HARUKA_CLOUD_LOD_LO,  wl * HARUKA_CLOUD_LOD_HI,  fp));
        v += (w > 0.0) ? a * mix(0.5, harukaCloudVNoise3(p), w) : a * 0.5;
        p = ((i & 1) == 0 ? HARUKA_CLOUD_ROT_A : HARUKA_CLOUD_ROT_B) * p * 2.0 + 17.3;
        a *= 0.5; wl *= 0.5;
    }
    return v;
}

float harukaCloudFieldLayer(vec3 p, vec3 wind, float fp0, float fp) {
    vec3 warp = vec3(harukaCloudFbm3LODRot(p * 0.6 + wind * 0.5,        2, fp0 * 0.6, fp * 0.6),
                     harukaCloudFbm3LODRot(p * 0.6 + 5.2 - wind * 0.5,  2, fp0 * 0.6, fp * 0.6),
                     harukaCloudFbm3LODRot(p * 0.6 + 11.7 + wind * 0.3, 2, fp0 * 0.6, fp * 0.6));
    // Tres octavas, no dos: la tercera (1,1 km en el cirro, 300 m en el altocumulo) es la que
    // redondea el contorno; queda por encima del paso dentro de la losa (ver `finoM`).
    const float v = harukaCloudFbm3LODRot(HARUKA_CLOUD_ROT_B * (p * 1.3) + warp * 1.8 + wind, 3, fp0 * 1.3, fp * 1.3);
    // El umbral (`harukaCloudThreshold`) esta calibrado para DOS octavas: media 0,375 y sigma
    // HARUKA_CLOUD_SIGMA_CAPA. Con tres la media es 0,4375 y la sigma x1,10 — sin recolocar, el
    // cirro a 0,45 de cobertura salia como un velo casi cerrado (captura). Se devuelve el campo con
    // las estadisticas de dos octavas, y la tercera solo aporta FORMA.
    return 0.375 + (v - 0.4375) * 0.91;
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
/** Espesor MÍNIMO de una nube (m): lo que la marcha puede resolver con sus pasos de 50-360 m. Una
 *  nube más fina que esto no se dibuja como nube, se dibuja como grano. */
const float HARUKA_CLOUD_MIN_THICK_M = 260.0;
float harukaCloudDensity(float strength, float f, vec3 q, float minTop, float wavelengthM, float footprintM, float topMul);
float harukaCloudDensity(float strength, float f, vec3 q, float minTop, float wavelengthM, float footprintM) {
    return harukaCloudDensity(strength, f, q, minTop, wavelengthM, footprintM, 1.0);
}
float harukaCloudDensity(float strength, float f, vec3 q) { return harukaCloudDensity(strength, f, q, HARUKA_CLOUD_MINTOP, 1.0e9, 0.0, 1.0); }

/** @brief COLIFLOR: multiplicador del techo por columna, de un ruido de ~250 m evaluado en la
 *  posicion HORIZONTAL (la comprimida, `pfC`, que apenas cambia con la altura). 0,7..1,3: cada bulto
 *  sube o baja el techo hasta un 30 %. Es lo que hace que el techo tenga bultos del tamano de los de
 *  un cumulo real, y no solo el grano de 125 m de la erosion. */
/// LA TORRE: cuanto sube una nube, ademas de los bultos, segun lo FUERTE que sea su nucleo.
/// ⚠️ "Hay nubes grandes que salen planas" (Andoni). `harukaCloudStrengthCumulo` satura en 0,22
/// unidades por encima del umbral: en un cumulo grande todo el nucleo esta saturado, todos sus
/// puntos alcanzan el MISMO techo (el 60 % de la losa) y la cima es una MESETA con bultos de 130-265
/// m. En la atmosfera real es al reves: la nube grande es la del ascenso fuerte, y sube mas. Aqui el
/// EXCESO del campo sobre el umbral —que no satura, sigue creciendo hacia el centro de la nube—
/// levanta el techo: 0 de exceso, x0,75 (una nube floja es baja y ancha); 0,40 de exceso, x1,9 (el
/// nucleo llega al techo de su losa). Con eso la cima de una nube grande es un domo que crece hacia
/// dentro, no una bandeja.
float harukaCloudTowerMul(float excess) {
    return mix(0.75, 1.9, smoothstep(0.0, 0.40, excess));
}
float harukaCloudTopBumps(vec3 pfC) {
    const float b = harukaCloudVNoise3(pfC * 9.0 + vec3(3.3, 8.1, 5.7)) * 0.6
                  + harukaCloudVNoise3(pfC * 18.0 + vec3(9.2, 1.4, 6.6)) * 0.4;
    return 0.6 + 0.8 * b;
}

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
/// Banda de transicion del CUMULO, mas ancha que la de las capas: con 0,085 la opacidad vista desde
/// abajo pasaba de 0,1 a 0,9 en 125 m (dos celdas de la sonda) — el "corta de repente". Las capas
/// altas se quedan con la estrecha: con la ancha sus fuerzas bajan y desaparecen (medido).
const float HARUKA_CLOUD_SOFT_CUMULO = 0.22;
float harukaCloudStrengthCumulo(float fieldValue, float threshold) {
    // Centrada en el umbral (no empezando en el): el umbral sale de la cobertura pedida y con la
    // banda ancha empezando ahi el cielo tapado caia al 43 % de lo pedido (medido). Centrada, lo
    // que el adelgazado de la periferia quita lo devuelve el medio umbral de mas.
    const float t0 = threshold - 0.25 * HARUKA_CLOUD_SOFT_CUMULO;
    return smoothstep(t0, t0 + HARUKA_CLOUD_SOFT_CUMULO, fieldValue);
}

/**
 * @brief El UMBRAL que deja por encima una fraccion `cover` del campo.
 *
 * ⚠️ ERA `lo = mix(0.58, 0.20, cover)`, una recta a ojo. Medida la distribucion de los campos en
 * CPU (400 000 puntos, mismo hash y mismo fBm): los dos son ~gaussianos con media 0,375 y sigma
 * 0,109 (cumulo) / 0,103 (capa). Con esa recta, cobertura 0,08 daba umbral 0,55 y por encima
 * quedaba el 5 % del cumulo y el 0,7-2 % de la capa: el altocumulo de buen tiempo (cobertura
 * 0,08-0,14) NO EXISTIA —0,0 % de pixeles en el banco desde el suelo, desde dentro de su banda y
 * desde encima— y el cirro salia a la mitad. El umbral de un cuantil es media + sigma·probit(1-c);
 * el probit va por Abramowitz-Stegun 26.2.23 (error < 4,5e-4), que son diez operaciones.
 * A partir de aqui `cover` SIGNIFICA fraccion del cielo, y la mezcla a cobertura de lejos
 * (`wFund`) deja de cambiar de nivel al entrar.
 */
float harukaCloudThreshold(float cover, float sigma) {
    const float c  = clamp(cover, 0.005, 0.995);
    const float pp = min(c, 1.0 - c);
    const float t  = sqrt(-2.0 * log(pp));
    float z = t - (2.515517 + 0.802853 * t + 0.010328 * t * t)
                / (1.0 + 1.432788 * t + 0.189269 * t * t + 0.001308 * t * t * t);
    z = (c < 0.5) ? z : -z;
    return 0.375 + sigma * z;
}
const float HARUKA_CLOUD_SIGMA_CUMULO = 0.109;
const float HARUKA_CLOUD_SIGMA_CAPA   = 0.103;

/**
 * @brief Cuanto queda el campo POR ENCIMA del umbral, de media, DENTRO de la parte nublada:
 *        `E[campo | campo > umbral] − umbral = σ·(φ(z)/c − z)` con `z = (umbral − μ)/σ` (el cociente
 *        de Mills inverso de la gaussiana). Es lo que hay que meter en la funcion de fuerza cuando
 *        el pixel ya no resuelve la fundamental (desde orbita): ahi `sN` pasa a ser la FRACCION de
 *        cielo con nube (`cover`), y si esa misma cifra se usa como fuerza de la nube, con cobertura
 *        0,3 cada nube sale como una nube al 30 % — floja, baja, translucida — y desde orbita el
 *        planeta se ve a traves de una capa que deberia tapar el 30 % en firme (medido: T 0,94 con
 *        cobertura 0,3, cuando toca ~0,7). Con esto la nube lejana tiene la fuerza MEDIA de las
 *        nubes de cerca (cobertura 0,3 -> fuerza 0,63; 0,9 -> 1,0).
 */
/**
 * @brief Fraccion de la sigma del campo que el LOD se ha LLEVADO (la varianza sub-huella), en [0,1]:
 *        `sqrt(Σ(1−wᵢ²)·aᵢ² / Σaᵢ²)` con los mismos pesos `wᵢ` que `harukaCloudFbm3LOD` aplica a cada
 *        octava (aᵢ = 0,5^(i+1), λᵢ = 0,5^i). 0 de cerca (todo resuelto), 1 desde orbita.
 *        `fp0`/`fp`: las huellas que recibio el fbm (ya multiplicadas por su 1,3).
 */
float harukaCloudLodSigmaFrac(float fp0, float fp, int octaves) {
    float lost = 0.0, total = 0.0, a = 0.5, wl = 1.0;
    for (int i = 0; i < octaves; ++i) {
        const float w = (i == 0) ? (1.0 - smoothstep(wl * HARUKA_CLOUD_LOD0_LO, wl * HARUKA_CLOUD_LOD0_HI, fp0))
                                 : (1.0 - smoothstep(wl * HARUKA_CLOUD_LOD_LO,  wl * HARUKA_CLOUD_LOD_HI,  fp));
        lost += (1.0 - w * w) * a * a; total += a * a;
        a *= 0.5; wl *= 0.5;
    }
    return sqrt(max(lost, 0.0) / max(total, 1.0e-6));
}
/// Φ(z) de la normal estandar (Abramowitz-Stegun 7.1.26, error < 1,5e-7).
float harukaCloudPhi(float z) {
    const float x = abs(z) * 0.7071068;
    const float t = 1.0 / (1.0 + 0.3275911 * x);
    const float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);
    return 0.5 * (1.0 + ((z < 0.0) ? -y : y));
}
/**
 * @brief La FRACCION de nube de una muestra cuya huella no resuelve el campo, y el campo MEDIO dentro
 *        de esa fraccion. El LOD devuelve el campo FILTRADO `f`; lo que se quedo por debajo de la
 *        huella es ~gaussiano con sigma `sSub`, asi que P(campo > umbral) = 1 − Φ((lo − f)/sSub) y
 *        E[campo | campo > lo] = f + sSub·φ(z)/P.
 *
 * ⚠️ ESTO ES LO QUE ABRE HUECOS EN EL HORIZONTE. Antes, sin la fundamental resuelta, la fraccion era
 * la COBERTURA MEDIA DEL TEXEL (78 km): la misma en toda la region, asi que un rayo rasante que cruza
 * 30-50 celdas de la banda de la capa "sorteaba" nube en todas y el horizonte se cerraba con un 20 %
 * de cobertura (0,8^32 = 0,001): las cordilleras lejanas y el mar desaparecian tras una pared. Con la
 * fraccion LOCAL, donde el campo filtrado queda bajo el umbral no hay nube, y los huecos regionales
 * del campo (que existen: es un fbm con warp) dejan ver.
 */
vec2 harukaCloudLocalFraction(float f, float lo, float sSub) {
    const float z = (lo - f) / max(sSub, 1.0e-4);
    const float P = clamp(1.0 - harukaCloudPhi(z), 0.0, 1.0);
    const float phi = exp(-0.5 * z * z) * 0.3989423;
    const float fIn = f + sSub * phi / max(P, 1.0e-3);
    return vec2(P, fIn);
}

float harukaCloudInFieldAbove(float cover, float sigma) {
    const float c  = clamp(cover, 0.005, 0.995);
    const float lo = harukaCloudThreshold(c, sigma);
    const float z  = (lo - 0.375) / sigma;
    const float phi = exp(-0.5 * z * z) * 0.3989423;
    return sigma * (phi / c - z);
}

/* ⚠️ AQUÍ HUBO DOS `harukaCloudStrengthAA` (asimétrico y simétrico) que ensanchaban el borde del
 * umbral con la huella. Los dos INFLAN la cobertura a distancia cuando el umbral cae en la cola
 * del campo (cobertura baja): medido en el juego como neblina gris y como cielo blanco al
 * horizonte. Retirados. El antialiasing correcto de un umbral es la esperanza del escalón bajo la
 * huella, y eso no sale de ensanchar un smoothstep. */

/** @brief Erosión de detalle: 3 octavas de ruido 3D. Solo se llama donde YA hay nube (ver
 *  `harukaCloudDensity`), que es lo que la hace asequible: el 3D no se paga en el cielo vacío, y
 *  cuesta 8 hashes por octava contra los 80 del campo 2D — subir octavas AQUÍ es barato. */
float harukaCloudDetail(vec3 q) {
    return 0.55 * harukaCloudVNoise3(q)
         + 0.30 * harukaCloudVNoise3(q * 2.7 + 11.3)
         + 0.15 * harukaCloudVNoise3(q * 6.9 + 41.7);
}

/** @brief El detalle CON LOD: cada octava se apaga cuando su longitud de onda no llega a unos
 *  píxeles en pantalla.
 *
 *  ⚠️ SIN ESTO LA NUBE DÉBIL ES UNA CELOSÍA. Las tres octavas miden 125, 46 y 18 m a la escala del
 *  cúmulo; a 1,5 km de la cámara y a 1/4 de resolución eso son 1-3 píxeles. Sobre un cuerpo flojo
 *  (~0,3) la erosión ×0,55 manda, y lo que queda es la RETÍCULA del ruido de valor pintada a
 *  Nyquist: en el banco (`cloud_volume_draws`, horneado real) el 27 % de los píxeles del cúmulo se
 *  separaban más de 10/255 de su media 3x3, y en el juego eran "motas" y "píxeles". No era muestreo
 *  —se probó con paso adaptativo y espesor mínimo, sin cambio—: era señal real por encima de la
 *  frecuencia del píxel. Cada octava entra sólo cuando su longitud de onda cubre 4-8 píxeles, y los
 *  pesos se renormalizan para que la densidad media no cambie con la distancia.
 *
 *  @param wavelengthM longitud de onda de la PRIMERA octava, en metros (1/(fscale·11,4))
 *  @param footprintM  lo que mide un píxel a la distancia de la muestra, en metros */
float harukaCloudDetailLOD(vec3 q, float wavelengthM, float footprintM) {
    const float w0 = 1.0 - smoothstep(wavelengthM / 8.0,       wavelengthM / 4.0,       footprintM);
    const float w1 = 1.0 - smoothstep(wavelengthM / 8.0 / 2.7, wavelengthM / 4.0 / 2.7, footprintM);
    const float w2 = 1.0 - smoothstep(wavelengthM / 8.0 / 6.9, wavelengthM / 4.0 / 6.9, footprintM);
    const float a0 = 0.55 * w0, a1 = 0.30 * w1, a2 = 0.15 * w2;
    const float sum = a0 + a1 + a2;
    if (sum <= 0.001) return 0.5;                     // sin detalle resoluble: el valor medio
    return (a0 * harukaCloudVNoise3(q)
          + a1 * harukaCloudVNoise3(q * 2.7 + 11.3)
          + a2 * harukaCloudVNoise3(q * 6.9 + 41.7)) / sum;
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
float harukaCloudDensity(float strength, float f, vec3 q, float minTop, float wavelengthM, float footprintM, float topMul) {
    if (strength <= 0.0 || f <= 0.0 || f >= 1.0) return 0.0;

    // Techo LOCAL de esta nube dentro de la losa. El suelo evita el filo de espesor cero.
    // ⚠️ `minTop` lo pone el LLAMADOR a partir del espesor de la losa en metros: una nube floja no
    // puede quedar por debajo de lo que la marcha resuelve (ver `HARUKA_CLOUD_MIN_THICK_M`). Con el
    // 22 % fijo y una losa de 280 m salían jirones de 62 m, muestreados a 50-360 m por paso con
    // jitter por píxel: GRANO (22,8 % de píxeles a más de 10/255 de su media 3x3, medido en el
    // banco con el horneado real; 0 % en las capas altas, que llenan su losa).
    // ⚠️ EL TECHO SUBE CON LA FUERZA AL CUADRADO, no linealmente. Con la vertical del campo comprimida
    // (columnas) y el techo lineal, cualquier nube que pasaba el umbral llegaba al techo de la losa:
    // torres de 2,4 km y 1 km de ancho (medido 1,01:1). Solo los nucleos fuertes suben; el resto son
    // cumulos bajos y anchos, que es lo que hay en un cielo de buen tiempo.
    // ── DOMO: la seccion se ESTRECHA con la altura ─────────────────────────────────────────────
    // Antes: `techo = f(fuerza)` y dentro `perfil(f/techo)·fuerza`. Con fuerza baja eso es una LAMINA
    // fina (medido en el juego: parches planos con banding). Un cumulo es lo contrario: en la base
    // TODA la huella es nube (base plana) y segun se sube solo sobreviven los picos del campo — asi
    // salen los domos, la coliflor y los bordes que se afinan hacia arriba. Es el "gradiente de
    // altura" clasico: el umbral de fuerza sube con la altura.
    // El techo que puede alcanzar la columna: hasta ~el 60 % de la losa por los bultos (topMul 0,6..1,4),
    // no la losa entera — con toda la losa salian torres de 2,4 km (1,7:1 medido).
    // ⚠️ Se probo 0,75 (17-09) contra las "rayas" de cumulos lejanos vistos desde 1 km por encima: el
    // banco ("encima del cumulo, -5") no cambio nada visible —esas lentes son la cima de cada nube en
    // escorzo, a 10-20 km, y no dependen de la altura del domo— y subian los cortes del cumulo (72).
    // ⚠️ SIN TOPE DURO. Era `min(MINTOP + 0,38·topMul, 1,0)`: con cobertura alta el exceso del campo
    // es grande en todas partes, la torre (x1,9) llevaba `topMul` por encima de 2 y el `min` dejaba
    // TODA la nube en el techo de la losa — un deck PLANO ("hay nubes grandes que salen planas").
    // Ahora el techo satura suave: 1 − e^(−0,55·topMul), que con topMul 0,45..2,7 da 0,39..0,82 de la
    // losa, y los bultos (±40 %) siguen moviendo la cima ±0,08 (200-280 m) tambien en el deck: es lo
    // que hace celular un estratocumulo en vez de una bandeja.
    const float tmin = max(HARUKA_CLOUD_MINTOP, minTop);
    const float topMax = tmin + (1.0 - tmin) * (1.0 - exp(-0.55 * topMul));
    if (f >= topMax) return 0.0;
    const float g     = pow(clamp(f / topMax, 0.0, 1.0), 1.4);            // 0 en la base, 1 en el techo maximo
    const float need  = 0.92 * g;                                           // fuerza que hace falta a esta altura
    const float sEff  = clamp((strength - need) / max(1.0 - need, 0.05), 0.0, 1.0);
    if (sEff <= 0.0) return 0.0;
    const float body = smoothstep(0.0, HARUKA_CLOUD_RISE, f) * sEff * (1.0 - smoothstep(0.85, 1.0, f / topMax));
    if (body <= 0.0) return 0.0;
    const float localTop = topMax;   // para la erosion de abajo

    // El detalle muerde SOBRE TODO el borde, pero ya no deja el núcleo liso.
    //
    // ⚠️ Antes el peso era `1 - smoothstep(0.30, 0.85, body)`, o sea EXACTAMENTE CERO en el cuerpo
    // de la nube: el núcleo salía como una mancha de color plano, y con el cielo cubierto —donde
    // casi todo el campo es núcleo— eso es una pared gris sin un solo rasgo. Es la "poca definición
    // de nube" que se reportó. Ahora el núcleo conserva un 35 % del detalle: suficiente para que se
    // lea el grano y la nube tenga superficie, sin convertirla en un encaje que deje pasar el sol.
    const float edge = mix(0.35, 1.0, 1.0 - smoothstep(0.30, 0.85, body));
    // La erosion muerde ARRIBA (coliflor) y apenas abajo (base plana): sube con la altura en la nube.
    const float topBias = 0.20 + 0.80 * clamp(f / localTop, 0.0, 1.0);
    const float dens = max(body - HARUKA_CLOUD_ERODE * edge * topBias * harukaCloudDetailLOD(q, wavelengthM, footprintM), 0.0);
    // El BORDE se adelgaza: donde la fuerza es baja (periferia de la nube) la densidad baja con ella,
    // asi la opacidad sube en cientos de metros y no en dos celdas ("corta de repente": medido 125 m
    // de 0,1 a 0,9 de opacidad, ahora >= 190).
    // Y las MOTAS no existen: por debajo de 0,2 de fuerza (picos sueltos del campo que solo dan una
    // lamina de un paso) no hay nube; de 0,2 a 0,5 entra en rampa.
    return dens * smoothstep(0.1, 0.55, strength);
}

/**
 * @brief Densidad de una nube EN CAPA (altocúmulo, cirro): ocupa su losa entera, sin remapeo.
 *
 * ⚠️ Usar `harukaCloudDensity` para las capas altas las convertía en MOTAS: el remapeo por fuerza
 * deja a una nube floja en el 22 % inferior de su losa — es lo que hace crecer un cúmulo — y un
 * cirro a 0,26 de cobertura quedaba en ~250 m de espesor contra pasos de 130 m: uno o cero pasos
 * dentro, decididos por el jitter de cada píxel. Medido con A/B en el juego (parches a 0 → sin
 * motas). Un cirro o un altocúmulo no crecen en vertical: son capas, y llenan su losa.
 */
/** @brief Perfil vertical de una CAPA (altocumulo, cirro): parabola 4f(1-f), maximo a media losa,
 *  y su MEDIA EXACTA sobre el tramo [fLo, fHi] que el paso recorre dentro de la losa.
 *
 *  ⚠️ ESTRIAS HORIZONTALES POR TODO EL VELO (captura desde 3 km, 16-09; el banco las ensena al cenit).
 *  El perfil del cumulo (subida en el 6 % de la losa: 20-50 m de rampa) se evaluaba en UN punto por
 *  paso —la altura media del solape— con pasos de 80-200 m dentro de una capa de 320-800 m: dos o
 *  cuatro muestras por travesia, y si la rampa caia dentro del paso o no lo decidia el angulo del
 *  rayo, no el jitter (que solo mueve el arranque, 0-50 m). Bandas a elevacion constante. Sin
 *  fibras (dbg 16, sin perfil) no habia bandas; con la marcha de referencia (x4 fina) eran MAS
 *  fuertes, no menos — la firma de un perfil aliasado. Integrar el perfil sobre el tramo es exacto
 *  con cualquier paso: F(f) = 2f^2 - 4f^3/3. */
float harukaLayerProfileMean(float fLo, float fHi) {
    fLo = clamp(fLo, 0.0, 1.0); fHi = clamp(fHi, 0.0, 1.0);
    if (fHi - fLo < 1.0e-5) { const float f = clamp(0.5 * (fLo + fHi), 0.0, 1.0); return 4.0 * f * (1.0 - f); }
    const float FLo = 2.0 * fLo * fLo - 4.0 * fLo * fLo * fLo / 3.0;
    const float FHi = 2.0 * fHi * fHi - 4.0 * fHi * fHi * fHi / 3.0;
    return (FHi - FLo) / (fHi - fLo);
}

float harukaLayerDensity(float strength, float fLo, float fHi, vec3 q) {
    if (strength <= 0.0 || fHi <= 0.0 || fLo >= 1.0) return 0.0;
    const float body = harukaLayerProfileMean(fLo, fHi) * strength;
    if (body <= 0.0) return 0.0;
    // ⚠️ NADA DE RESTAR EL DETALLE FINO. `harukaCloudDetail` tiene octavas hasta 76 m a la escala
    // del cirro y restarlo (x0,55) a un cuerpo de 0,3 deja solo los PICOS del ruido: a 100-200 m por
    // paso y con jitter por pixel eso es una mota por pixel — medido a resolucion completa, motas
    // de 1 px por todo el cielo. Aqui el detalle MODULA (nunca resta hasta cero) y a baja
    // frecuencia (jirones de km, que es lo que tiene un cirro).
    const float tex = harukaCloudVNoise3(q * 0.12) * 0.7 + harukaCloudVNoise3(q * 0.20 + 3.1) * 0.3;
    return body * (0.45 + 0.75 * tex);
}

/**
 * @brief Densidad del CIRRO: una lamina fina, translucida y FIBROSA a lo largo del viento.
 *
 * ⚠️ El cirro era `harukaLayerDensity` sobre 1,1-2,2 km de losa con extincion 0,00176/m: espesor
 * optico 2-4, o sea OPACO, y desde dentro de su banda el banco daba el 100 % del cuadro blanco (una
 * niebla). Un cirro real tiene espesor optico 0,1-1 y es un peine de fibras tendidas con el viento
 * de altura. `along` y `across` son las coordenadas en el marco tangente del punto (metros): el
 * ruido va estirado 6:1 a lo largo, y la fibra que cae por debajo del cuerpo se apaga.
 */
float harukaCirrusDensity(float strength, float fLo, float fHi, float along, float across, float footprintM) {
    if (strength <= 0.0 || fHi <= 0.0 || fLo >= 1.0) return 0.0;
    const float body = harukaLayerProfileMean(fLo, fHi) * strength;   // ver harukaLayerProfileMean
    if (body <= 0.0) return 0.0;
    // Las fibras llevan LOD como todo lo demas, o a 30 km son una mota por pixel (medido en el banco: la mitad alta del cuadro en sal y pimienta).
    // ⚠️ El patron es HORIZONTAL (no depende de la altura dentro de la lamina) y por eso su LOD va
    // por la huella del PIXEL, no del paso: con el paso (80-200 m dentro de la lamina) las fibras
    // se fundian enteras y el cirro salia como algodon liso desde encima.
    // 1,7 km de traves (720 m la segunda octava) por 4 km a lo largo: a 1/4 de resolucion desde
    // el suelo, una fibra de 290 m eran 3-4 px y el banco la contaba como grano (51 % al cenit).
    // ⚠️ Eran 10 km a lo largo (6:1) y en el juego se veian "nubes mucho mas largas que anchas":
    // barras. 2,4:1, y un tercer ruido ISOTROPO que las corta en jirones para que no sean barras.
    // ⚠️ Con la malla del ruido alineada con (along, across) y umbralizada, cada jiron salia como un
    // ROMBO de lados rectos (captura desde 3 km, 17-09, en cuanto se quito la palanca que lo
    // escondia). La malla de las fibras va girada 33 grados en el plano y la del ruido isotropo
    // otros 33 al reves: los tramos rectos de una no coinciden con los de la otra.
    const vec2  ax = vec2(along * 0.00025, across * 0.0006);
    const vec2  ar = vec2(ax.x * 0.8387 - ax.y * 0.5446, ax.x * 0.5446 + ax.y * 0.8387);
    const vec2  ix = vec2(along * 0.0004, across * 0.0004);
    const vec2  ir = vec2(ix.x * 0.8387 + ix.y * 0.5446, -ix.x * 0.5446 + ix.y * 0.8387);
    const vec3 qa = vec3(ar, 3.7);
    const vec3 qi = vec3(ir, 9.1);
    const float w0 = 1.0 - smoothstep(1667.0 / 8.0, 1667.0 / 4.0, footprintM);
    const float w1 = 1.0 - smoothstep(725.0 / 8.0, 725.0 / 4.0, footprintM);
    // Las mallas de las tres, giradas entre si (ver HARUKA_CLOUD_ROT_*): con la misma malla la fibra
    // umbralizada era una BARRA de lados rectos, no un jiron.
    const float fibra = mix(0.5, harukaCloudVNoise3(qa), w0) * 0.45
                      + mix(0.5, harukaCloudVNoise3(HARUKA_CLOUD_ROT_A * (qa * 2.3) + 7.7), w1) * 0.25
                      + mix(0.5, harukaCloudVNoise3(HARUKA_CLOUD_ROT_B * qi), w1) * 0.30;
    // Borde blando: la fibra MODULA (0,15-1) y no recorta; con smoothstep(0,30, 0,70) salian
    // placas de canto duro.
    return body * mix(0.15, 1.0, smoothstep(0.20, 0.80, fibra));   // borde mas ancho: menos poligono
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
