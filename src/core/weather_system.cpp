#include "core/weather_system.h"

#include <glm/gtc/constants.hpp>
#include <cmath>

namespace Haruka {

namespace {

// Hash entero → [0,1). Determinista y barato; los frentes se siembran con esto, no con rand().
inline float h01(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return (float)(x & 0x00FFFFFFu) / (float)0x01000000u;
}
inline float hashf(uint32_t seed, int i, int k) {
    return h01(seed * 747796405u + (uint32_t)i * 2891336453u + (uint32_t)k * 2654435761u);
}

// Punto uniforme en la esfera a partir de dos uniformes [0,1). Uniforme DE VERDAD (z uniforme, no
// el ángulo): sembrar con (lat,lon) uniformes amontonaría todos los frentes en los polos.
inline glm::dvec3 spherePoint(float u1, float u2) {
    const double z   = 2.0 * (double)u1 - 1.0;
    const double r   = std::sqrt(std::max(0.0, 1.0 - z * z));
    const double phi = 2.0 * glm::pi<double>() * (double)u2;
    return glm::dvec3(r * std::cos(phi), r * std::sin(phi), z);
}

// Rodrigues: gira `v` un ángulo `ang` alrededor del eje unitario `axis`.
inline glm::dvec3 rotateAround(const glm::dvec3& v, const glm::dvec3& axis, double ang) {
    const double c = std::cos(ang), s = std::sin(ang);
    return v * c + glm::cross(axis, v) * s + axis * glm::dot(axis, v) * (1.0 - c);
}

// El NORTE local del planeta es +Y: es el mismo eje con el que `PlanetFields::computeClimate`
// calcula la latitud para la temperatura. Si aquí se usara otro, los vientos zonales (alisios,
// poniente de latitudes medias) no casarían con las bandas de temperatura y se vería raro.
constexpr glm::dvec3 kNorth{0.0, 1.0, 0.0};

} // namespace

void WeatherSystem::configure(uint32_t seed) {
    if (m_configured && m_seed == seed) return;   // idempotente: reconfigurar movería las tormentas
    m_seed = seed;

    for (int i = 0; i < kFronts; ++i) {
        Front& f = m_fronts[i];

        // Eje de giro y punto de partida, independientes → el frente describe un círculo máximo
        // inclinado cualquiera. Con el eje SIEMPRE en +Y todos los frentes irían en paralelo por
        // latitudes fijas y el planeta tendría "carriles" de tormenta.
        f.axis  = glm::normalize(spherePoint(hashf(seed, i, 0), hashf(seed, i, 1)));
        glm::dvec3 s = spherePoint(hashf(seed, i, 2), hashf(seed, i, 3));
        s -= f.axis * glm::dot(f.axis, s);                       // al plano perpendicular al eje
        const double sl = glm::length(s);
        f.start = (sl > 1e-9) ? s / sl : glm::normalize(glm::cross(f.axis, glm::dvec3(1, 0, 0)));

        // Velocidad: unos van más rápido que otros y ~1/3 van al revés → los frentes se alcanzan,
        // se cruzan y se separan. Con todos a la misma velocidad el patrón sería rígido.
        const double rate = 0.55 + 0.9 * (double)hashf(seed, i, 4);
        const double sign = (hashf(seed, i, 5) < 0.35f) ? -1.0 : 1.0;
        f.omega = sign * rate * 2.0 * glm::pi<double>() / kFrontRevolutionS;

        // Tamaño y fuerza. El radio angular manda en la ESCALA: 0.25 rad sobre la Tierra son ~1600 km
        // de radio = una borrasca de verdad; 0.72 tapa medio hemisferio (un sistema de gran escala).
        // ⚠️ SE INTENTO ENSANCHARLOS A 0,32-0,89 rad Y SE REVIRTIO, con la medida delante. La idea era
        // subir la cobertura media por el termino que SI varia en espacio y tiempo, en vez de por el
        // fondo (que es plano y actua como suelo). Pero un casquete de 0,89 rad tapa el 18,5 % de la
        // esfera y con 14 frentes no quedan huecos: `weather_fronts` bajo a **111 muestras despejadas
        // de 9600** y algunas horas se quedaban sin una sola — el cielo permanentemente cubierto que
        // este fichero lleva dos parrafos avisando de no hacer.
        //
        // O sea que los frentes NO tienen holgura para levantar la media: ya cubren casi todo lo que
        // pueden cubrir sin cerrar el planeta. Lo que de verdad limita la cobertura de este mundo es
        // su HUMEDAD (media 0,286 contra la terrestre, mucho mas alta), y eso se decide en el bioma,
        // no aqui.
        f.radius   = 0.25 + 0.47 * (double)hashf(seed, i, 6);
        f.strength = 0.55f + 0.45f * hashf(seed, i, 7);
    }
    m_configured = true;
}

glm::dvec3 WeatherSystem::frontCenter(int i) const {
    const Front& f = m_fronts[i];
    return rotateAround(f.start, f.axis, f.omega * m_time * m_timeScale);
}

float WeatherSystem::cloudCoverAt(const glm::dvec3& dir, float humidity) const {
    if (!m_configured) return 0.0f;
    const glm::dvec3 d = glm::normalize(dir);

    // Cobertura = lo que aportan los frentes que tapan el punto. Se suma en vez de tomar el máximo
    // para que dos frentes solapados den cielo REALMENTE cerrado (el clamp final lo acota).
    float cover = 0.0f;
    for (int i = 0; i < kFronts; ++i) {
        const Front& f = m_fronts[i];
        const double cosang = glm::clamp(glm::dot(d, frontCenter(i)), -1.0, 1.0);
        const double ang    = std::acos(cosang);
        if (ang >= f.radius) continue;
        // Perfil suave del casquete: 1 en el núcleo → 0 en el borde. `t·t·(3−2t)` = smoothstep.
        const double t = 1.0 - ang / f.radius;
        cover += f.strength * (float)(t * t * (3.0 - 2.0 * t));
    }

    // La humedad del CAMPO modula: el mismo frente descarga sobre la selva y cruza el desierto casi
    // vacío. Y un fondo de nube proporcional a la humedad (la selva rara vez está del todo despejada).
    const float H = glm::clamp(humidity, 0.0f, 1.0f);

    // ⚠️ EL FONDO ERA `0.12·H` Y ESO DEJABA EL PLANETA DESPEJADO. Los frentes cubren una fracción
    // pequeña de la esfera, así que fuera de ellos la cobertura caía al fondo: con humedad 0,65 eran
    // **0,078**. Medido sobre 24 000 muestras, la MEDIANA del planeta salía 0,111 y el 60 % estaba
    // por debajo de 0,20 — la Tierra real ronda 0,67 de media. Con esos números NINGUNO de los dos
    // sistemas de nube dibuja nada: el umbral del cúmulo volumétrico queda en 0,54 y el del cirro de
    // fondo en 0,68, contra un campo cuyo máximo es 0,836. O sea que el cielo salía vacío por
    // aritmética, y cualquier ajuste del render se estaba probando sobre un planeta sin nubes.
    //
    // El fondo pasa a ser el término DOMINANTE y los frentes lo que lo cierra del todo. Sigue
    // habiendo días despejados (aire seco) y sigue mandando la humedad, que es lo que hacía que el
    // desierto y la selva no tuvieran el mismo cielo.
    // ⚠️ El fondo es CONSTANTE para una humedad dada, así que actúa como un SUELO plano: por debajo
    // de él no baja nadie. Por eso se deja bajo y se refuerzan los FRENTES en su lugar — ellos sí
    // varían en espacio y tiempo, que es de donde tiene que salir "hoy está despejado y mañana no".
    // Un fondo alto daba cielo permanentemente cubierto, que aburre igual que el cielo vacío.
    // ⚠️⚠️ Y SEGUIA CORTO, PORQUE SE CALIBRO CON UNA HUMEDAD QUE EL MUNDO NO TIENE. El arreglo de
    // arriba se valido con `cloud_shape`, que muestrea con `humidity = 0.65` escrita a mano. La
    // humedad REAL del planeta la imprime el propio motor al cargarlo:
    //
    //     clima del planeta: humedad [0.059, 0.882] media 0.286
    //
    // O sea que la calibracion se hizo a 2,3 veces la humedad que hay. Con `0.06 + 0.30·H` y la media
    // real, el fondo vale **0,146** — y la Tierra ronda **0,67** de cobertura media. De ahi que
    // Andoni viera "motas" desde orbita: fuera de los frentes el planeta esta practicamente
    // despejado, igual que antes del arreglo anterior pero por otra via.
    //
    // ⚠️ NO SE PONE LINEAL. Con `a + b·H` ajustado para dar 0,67 en la media (H = 0,286) hace falta
    // b ≈ 2,07, y entonces la mitad humeda del mundo (H hasta 0,882) satura a 1,0: cielo cubierto
    // PERMANENTE, que es justo lo que el parrafo de arriba dice que aburre igual que el vacio. La
    // humedad esta sesgada a la baja (media 0,286 con maximo 0,882), asi que una recta que acierte en
    // la media falla en la cola.
    //
    // Con raiz cuadrada la curva sube deprisa donde hay poca humedad y se aplana arriba:
    //     H = 0,059 (desierto)  -> 0,19    sigue habiendo cielos despejados
    //     H = 0,286 (la MEDIA)  -> 0,42    contra los 0,146 de antes
    //     H = 0,882 (selva)     -> 0,73    sin saturar, los frentes aun tienen donde crecer
    // Con los frentes encima, la media del planeta queda cerca de la terrestre sin cerrar el cielo.
    //
    // ⚠️ El 0,67 de la Tierra es una referencia REAL y por eso se usa; donde exactamente entre 0,42 y
    // 0,67 debe quedar este mundo es una decision de diseño, no un dato. Queda escrito el numero para
    // que se pueda mover a sabiendas.
    // ⚠️ Y LA RAIZ TAMPOCO VALIA: dejaba el planeta SIN NINGUN SITIO DESPEJADO. `weather_fronts`
    // barre humedad desde 0,15 y llama "despejado" a `cover < 0.15`; con `0.78·sqrt(0.15) = 0.302` no
    // habia un solo punto limpio a ninguna hora, y el test lo casco (24 fallos). Es exactamente lo que
    // el parrafo de arriba avisa: "un fondo alto daba cielo permanentemente cubierto, que aburre igual
    // que el cielo vacio". La raiz sube demasiado deprisa en la parte baja, que es donde vive casi
    // todo este mundo (humedad media 0,286).
    //
    // Lineal y SIN suelo constante: con poca humedad el cielo puede limpiarse de verdad
    // (H = 0,15 -> 0,128, por debajo del umbral de despejado), y con mucha se cierra
    // (H = 0,882 -> 0,75). La pendiente es 2,8 veces la que habia (0,30), que es lo que sube la
    // cobertura sin romper el invariante.
    //
    // ⚠️ Lo que de verdad levanta la MEDIA sin tocar ese suelo son los FRENTES —ellos varian en
    // espacio y tiempo— y por eso se ensanchan abajo, en `configure`. El fondo solo pone el minimo.
    // ⚠️ ERA 0,85 Y SE CALIBRO CONTRA UNA MEDIA MAL PONDERADA. La sonda de cobertura promediaba por
    // TEXEL sobre una rejilla equirectangular, donde un texel polar cubre `cos(lat)` veces menos
    // superficie: sobrepondera los polos. Al ponderar por AREA, la cobertura real del planeta salio
    // **0,732**, no los 0,641 que se leian — o sea que la calibracion apuntaba a 0,67 y el mundo
    // estaba en 0,73, mas cubierto que la Tierra. El coeficiente baja para cerrar esa diferencia.
    // Ver la nota de `application_render.cpp`, que ahora imprime las dos medias.
    // El valor sale de DOS medidas, no de una: con 0,85 la media por area era 0,732 y con 0,63 bajaba a
    // 0,614, o sea 0,536 de cobertura por unidad de coeficiente. Para los 0,67 de la Tierra toca 0,73.
    const float background = 0.73f * H;
    return glm::clamp(cover * (0.70f + 1.00f * H) + background, 0.0f, 1.0f);
}

float WeatherSystem::cloudDensityAt(const WeatherSample& w, float altM) {
    if (w.cloudCover <= 0.0f) return 0.0f;
    const float thick = w.cloudTopM - w.cloudBaseM;
    if (thick <= 1.0f) return 0.0f;

    // Altura RELATIVA dentro de la losa: 0 en la base, 1 en el techo. Fuera de [0,1] no hay nube, y
    // ese cero es literal — es lo que permite volar por debajo de la capa y salir por encima.
    const float t = (altM - w.cloudBaseM) / thick;
    if (t <= 0.0f || t >= 1.0f) return 0.0f;

    // Perfil: entra rápido por abajo y se deshilacha por arriba. Asimétrico a propósito — la base de
    // una nube es un plano bastante definido (el nivel de condensación es una cota, y por eso todas
    // las nubes de un cielo tienen la base a la misma altura) mientras que el techo se desfleca.
    // Bordes SUAVES: con un corte duro, atravesar la base se leería como cruzar una pared de niebla.
    // Los bordes viven en la cabecera (`kProfileRise`/`kProfileFall`) porque un TEST los compara
    // con los del shader: es la misma losa evaluada en dos sitios y no pueden separarse.
    const float rise = glm::smoothstep(0.0f, kProfileRise, t);
    const float fall = 1.0f - glm::smoothstep(kProfileFall, 1.0f, t);
    return glm::clamp(w.cloudCover * rise * fall, 0.0f, 1.0f);
}

WeatherSample WeatherSystem::sampleAt(const glm::dvec3& dir, float tempC, float humidity) const {
    WeatherSample w;
    w.tempC    = tempC;
    w.humidity = glm::clamp(humidity, 0.0f, 1.0f);
    if (!m_configured) return w;

    const glm::dvec3 d = glm::normalize(dir);
    w.cloudCover = cloudCoverAt(d, humidity);

    // ── LA PRECIPITACIÓN SALE DE LA COBERTURA ───────────────────────────────────────────────────
    // Esta línea ES el sistema: no hay ningún otro camino por el que `precip` pueda subir. Sin nube
    // encima (cover < kPrecipCover) el resultado es exactamente 0, y el aire seco no descarga
    // aunque el cielo esté cerrado (una capa alta sobre el desierto tapa el sol y no moja).
    const float heavy = glm::smoothstep(kPrecipCover, 0.94f, w.cloudCover);
    w.precip = glm::clamp(heavy * (0.30f + 0.70f * w.humidity), 0.0f, 1.0f);
    if (w.precip > 0.01f) w.type = (tempC <= kSnowTempC) ? Precip::Snow : Precip::Rain;
    else                  { w.precip = 0.0f; w.type = Precip::None; }

    // Base de la nube ≈ nivel de condensación: cuanto más seco el aire, más alto condensa. Es el
    // TECHO desde el que cae la precipitación (la fase B la dibuja entre esta cota y el suelo), así
    // que no es decorativo: si está mal, la lluvia nace dentro de la nube o muy por debajo.
    // ⚠️ EL SUELO SUBIÓ de 200 a 700 m (y el término constante de 320 a 700). Con aire húmedo y
    // cálido la fórmula daba bases de 200-400 m, y una capa a esa altura se lee como un TECHO
    // encima de la cabeza — el "la gris está muy cercana" que se reportó. El estratocúmulo real de
    // un día cubierto vive entre 600 y 2000 m. Sigue siendo el techo de la lluvia, así que subirlo
    // sube también de dónde nacen las gotas, que es lo coherente.
    w.cloudBaseM = glm::clamp(700.0f + (1.0f - w.humidity) * 2100.0f
                              - glm::smoothstep(25.0f, -5.0f, tempC) * 260.0f,
                              700.0f, 2900.0f);

    // ── TECHO: el DESARROLLO VERTICAL ───────────────────────────────────────────────────────────
    // Aquí es donde la nube deja de ser una superficie y pasa a ser un cuerpo. El grosor NO es una
    // constante, y la razón es la misma por la que llueve: una nube descarga porque se ha
    // desarrollado en vertical. Una capa de buen tiempo son unos cientos de metros de estrato; un
    // cumulonimbo de tormenta sube kilómetros. Poner un grosor fijo daría el mismo cielo a las dos
    // y perdería justo lo que hace reconocible una tormenta al verla venir de lejos.
    //
    // Dos sumandos, y cada uno responde a algo distinto:
    //   · la COBERTURA da el cuerpo base (más nube encima = capa más gruesa);
    //   · la PRECIPITACIÓN dispara la torre (es el término convectivo, el que hace el yunque).
    // La temperatura tapona: en aire frío la convección no sube igual, así que la tormenta polar es
    // más baja que la tropical.
    // ⚠️ EL CUERPO SUBIÓ (era 260 + 900·cover) y no es un retoque estético. Con el remapeo de altura
    // del shader (`harukaCloudDensity`), esta losa dejó de ser "la altura de la nube" para pasar a
    // ser el TECHO POSIBLE: cada nube ocupa la fracción de abajo que le toca por su fuerza, así que
    // con la losa vieja hasta el cúmulo más fuerte se quedaba en 620 m de alto contra ~1400 m de
    // ancho. Dando margen aquí, el shader tiene sitio donde levantar las torres.
    const float convect = glm::smoothstep(-10.0f, 24.0f, tempC);   // 0 = gélido, 1 = tropical
    const float body    = 500.0f + 1400.0f * w.cloudCover;
    // ⚠️ LA TORRE VA CON `precip²`, NO LINEAL. Con el termino lineal una lluvia MODERADA (precip 0,44)
    // levantaba 2,3 km de torre sobre 1,7 km de cuerpo: la losa iba de 700 a 4490 m, casi 4 km de
    // espesor. Eso es un cumulonimbo, no una lluvia; y con la camara a 3350 m se queda DENTRO de la
    // nube, que es el blanco total que se reporto. El desarrollo vertical de una nube crece mucho mas
    // deprisa que la tasa de lluvia — un nimbostrato de lluvia continua tiene 2-3 km en total, y los
    // 5 km de torre son del temporal. Con el cuadrado, `precip = 1` da lo mismo que antes (la tormenta
    // sigue haciendo yunque) y la lluvia moderada baja a ~1,0 km.
    const float tower   = 5200.0f * w.precip * w.precip * (0.35f + 0.65f * convect);
    const float thick   = glm::clamp(body + tower, 180.0f, 11000.0f);
    w.cloudTopM = w.cloudBaseM + thick;

    // ── VIENTO ──────────────────────────────────────────────────────────────────────────────────
    // Marco tangente local (este/norte) para poder hablar de vientos zonales como en la Tierra.
    glm::dvec3 east = glm::cross(kNorth, d);
    const double el = glm::length(east);
    east = (el > 1e-6) ? east / el : glm::dvec3(1, 0, 0);       // en el polo el "este" degenera
    const glm::dvec3 north = glm::cross(d, east);

    // (1) Circulación de fondo por bandas de latitud: alisios cerca del ecuador, poniente en
    //     latitudes medias. `sin(2.6·lat)` cambia de signo hacia los 35° → las dos bandas.
    const double lat   = std::asin(glm::clamp(glm::dot(d, kNorth), -1.0, 1.0));
    const double zonal = -5.5 * std::sin(2.6 * lat) * std::cos(lat);
    glm::dvec3 wind = east * zonal;

    // (2) Cada frente arrastra el aire EN SU DIRECCIÓN DE AVANCE, con el peso de cuánto tapa. Así el
    //     viento gira cuando la tormenta pasa por encima, que es lo que se nota al jugar: primero
    //     cambia el viento, luego llega la nube. La velocidad del frente en el punto es ω × r.
    for (int i = 0; i < kFronts; ++i) {
        const Front& f = m_fronts[i];
        const glm::dvec3 c = frontCenter(i);
        const double ang = std::acos(glm::clamp(glm::dot(d, c), -1.0, 1.0));
        if (ang >= f.radius) continue;
        const double t = 1.0 - ang / f.radius;
        const double wgt = f.strength * t * t * (3.0 - 2.0 * t);
        glm::dvec3 adv = glm::cross(f.axis, d);                  // dirección de avance en ESTE punto
        const double al = glm::length(adv);
        if (al < 1e-9) continue;
        wind += (adv / al) * (f.omega * 6371000.0 * 0.35) * wgt; // ω·R escalado a m/s plausibles
    }

    // (3) Racha: variación lenta y determinista (nada de rand()). ±25 % sobre el vector.
    const float gust = 0.75f + 0.5f * h01((uint32_t)(m_time * 0.35) * 2654435761u + m_seed);
    wind *= (double)gust;

    // La lluvia arrecia con el viento (la borrasca sopla y descarga a la vez): +30 % como techo.
    const double speed = glm::length(wind);
    w.precip = glm::clamp(w.precip * (float)(1.0 + 0.02 * glm::min(speed, 15.0)), 0.0f, 1.0f);
    w.wind   = glm::vec3(wind);
    return w;
}

} // namespace Haruka
