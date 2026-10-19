#pragma once
/**
 * @file World/ShellClumps.h
 * @brief RACIMOS DE BULTOS SOBRE UNA CÁSCARA: dónde hay uno, qué tamaño tiene y de qué bultos está
 *        hecho, a partir de un campo por dirección (cuánto, desde qué cota, hasta qué cota). GL-free.
 *        GEMELO EXACTO de `assets/shaders/lib/shell_clumps.glsl`: el compute que los siembra
 *        (`shell_scatter.comp`) y los tests usan la MISMA ley. Si se toca una constante aquí, se toca
 *        allí.
 *
 * Lo usa el cielo (un cúmulo es un racimo: domos de panza PLANA sobre la base y esferas encima), pero
 * la ley no sabe de nubes: un racimo es "cobertura c, base b, techo t" en una celda de una retícula.
 *
 * ── LA RETÍCULA ──────────────────────────────────────────────────────────────────────────────────
 * Las caras del cubo (`cubeFaceToDir`, spherify de Cobb) partidas en N×N celdas: un sitio por celda,
 * con su azar sacado de (cara, i, j, semilla). Es una retícula de la ESFERA, no de la cámara: al moverse
 * nadie se recoloca. Quien la usa puede girarla entera (las nubes van con el viento: `CloudSpin`).
 *
 * ── LA COBERTURA SE CONSERVA ─────────────────────────────────────────────────────────────────────
 * El campo dice qué FRACCIÓN de cielo cubre (c). Cada sitio tiene un rango u ∈ [0,1): hay racimo si
 * u < pres(c) = min(1, 2,5·c), y su FUERZA es s = 1 − u/max(pres, 0,25) (los de rango bajo son los
 * fuertes: los más grandes, los que más suben). El ancho es w = k(c)·celda·f, con f naciendo en CERO
 * cuando pres pasa su rango (un racimo nuevo crece: no aparece de golpe). k(c) sale de pedir que la
 * fracción cubierta sea c con el modelo booleano (discos al azar que se solapan):
 *     1 − exp(−ρ·E[área]) = c    con ρ = pres/celda² y E[área] = π/4·k²·celda²·E[f²]·fill
 * corregido por lo que la retícula es más regular que el azar (`kClumpLatticeFix`). `fill` es la parte
 * del disco de ancho w que tapan de verdad sus bultos. Las dos se MIDEN en `clump_coverage`.
 *
 * ── LA FORMA ─────────────────────────────────────────────────────────────────────────────────────
 * Bulto 0: el DOMO central (panza plana en la base). Bulto 1: la CORONA (esfera en lo alto). Luego
 * 5-6 hombros alrededor, esferas que suben (la coliflor) y bolitas sobre ellos. Una nube se dibuja
 * ENTERA, horneada como una sola malla (`World/Props/BlobSurface`): la distancia baja la malla, no
 * la forma.
 * Ningún bulto baja de la base: la panza de una nube es el nivel de condensación, una cota plana (los
 * domos apoyan sus tapas escalonadas unos metros, `kClumpDomeStep`, para que no se peleen en el depth).
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace Haruka::World {

// ── constantes de la ley (GEMELAS de lib/shell_clumps.glsl) ──────────────────────────────────────
constexpr float kClumpJitter      = 0.80f;   ///< el sitio cae en [0,1; 0,9] de su celda
constexpr float kClumpPresPerCover = 2.5f;   ///< pres(c) = min(1, 2,5·c)
/// La FUERZA de un sitio: s = 1 − u/max(pres, 0,25). Con poca cobertura NO depende de ella: los que
/// hay son los de rango bajo y su tamaño no cambia porque el clima suba un poco (sin el suelo, con
/// c = 0,02 todos los racimos crecían a la vez 500 m por cada 0,002 de cobertura: `clump_no_pop`).
constexpr float kClumpPresFloor   = 0.25f;
/// NACER: el racimo pasa de 0 a su tamaño mientras pres sube 0,15 por encima de su rango (segundos de
/// fundido del clima). Así entra creciendo, no de golpe; con 0,04 crecía 1,5 km en un segundo.
constexpr float kClumpBirth       = 0.15f;
constexpr float kClumpSizeMin     = 0.55f;   ///< tamaño relativo = nacer·(0,55 + 0,9·s)
constexpr float kClumpSizeSpan    = 0.90f;
/// Parte del disco de ancho w que tapan sus bultos vista desde arriba (MEDIDA en `clump_coverage`).
constexpr float kClumpFill        = 0.69f;
/// La retícula es MÁS REGULAR que el modelo booleano (discos al azar): con la misma área media se
/// solapan menos y tapan más; y con poca cobertura muchos racimos están naciendo (más pequeños que
/// su ley). Corrección MEDIDA del término −ln(1−c) (`clump_coverage` la calibra por bisección y
/// comprueba que la tabla la sigue), en c = 0; 0,1; …; 1.
constexpr float kClumpLatticeFix[11] = { 4.36f, 1.067f, 0.886f, 0.807f, 0.747f, 0.718f, 0.694f, 0.686f, 0.713f, 0.795f, 0.85f };
constexpr float kClumpCoverMax    = 0.97f;   ///< c = 1 pediría racimos infinitos
constexpr float kClumpMinHeightM  = 120.0f;
/// Bultos PRINCIPALES (cuerpo, corona, hombros, coliflor) y en total con las BOLITAS: el borde de un
/// cúmulo es fractal, bultos sobre bultos (lo que hace que se lea como nube y no como un montón de
/// bolas). Las bolitas van al FINAL del orden: de lejos no se dibujan.
constexpr int   kClumpMaxMain     = 14;
constexpr int   kClumpMaxBlobs    = 40;
/// Las nubes se HORNEAN como mallas (`World/Props/BlobSurface`) con semiancho 1 y este alto; cada
/// instancia se escala a su ancho y su alto. Gemelo de HARUKA_CLUMP_BAKE_H.
constexpr float kClumpBakeH       = 1.6f;
/// La base de cada racimo se mueve ±40 m del nivel de condensación (la de un cielo real varía eso) y,
/// dentro del racimo, cada domo apoya su tapa un poco más arriba que el anterior (0,1 % del alto). Con
/// todas las tapas en el MISMO plano el depth se peleaba entre ellas: discos rotos en la panza. Con
/// 0,4 % (6 m por domo) se veían desde abajo como platos apilados.
constexpr float kClumpBaseJitterM = 80.0f;
constexpr float kClumpDomeStep    = 0.001f;

/// El hash entero de la siembra (el mismo de `ground_scatter.comp`): exacto en cualquier GPU.
inline uint32_t clumpHash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
inline float clumpHashF(uint32_t h, uint32_t salt) {
    return (float)(clumpHash(h ^ (salt * 0x85EBCA6Bu)) & 0xFFFFFFu) / 16777216.0f;
}
inline uint32_t clumpCellHash(int face, uint32_t i, uint32_t j, uint32_t seed) {
    return clumpHash((i * 73856093u) ^ (j * 19349663u) ^ ((uint32_t)face * 83492791u) ^ seed);
}

/// (cara, lx, ly) → dirección unitaria, en FLOAT: gemelo de `harukaCubeFaceToDirF` (lib/cube_face.glsl).
/// Una nube no necesita el doble: un ulp de dirección son 0,5 m a radio terrestre.
inline glm::vec3 clumpFaceDir(int face, float lx, float ly) {
    glm::vec3 p;
    if      (face == 0) p = glm::vec3( 1.0f,   ly,  -lx);
    else if (face == 1) p = glm::vec3(-1.0f,   ly,   lx);
    else if (face == 2) p = glm::vec3(  lx,  1.0f,  -ly);
    else if (face == 3) p = glm::vec3(  lx, -1.0f,   ly);
    else if (face == 4) p = glm::vec3(  lx,   ly,  1.0f);
    else                p = glm::vec3( -lx,   ly, -1.0f);
    const float x2 = p.x * p.x, y2 = p.y * p.y, z2 = p.z * p.z;
    return glm::vec3(p.x * std::sqrt(1.0f - y2 * 0.5f - z2 * 0.5f + y2 * z2 / 3.0f),
                     p.y * std::sqrt(1.0f - z2 * 0.5f - x2 * 0.5f + z2 * x2 / 3.0f),
                     p.z * std::sqrt(1.0f - x2 * 0.5f - y2 * 0.5f + x2 * y2 / 3.0f));
}

/// El SITIO de la celda (i, j) de la cara con N celdas por lado: dirección en el marco de la retícula.
inline glm::vec3 clumpSiteDir(int face, uint32_t i, uint32_t j, uint32_t N, uint32_t h) {
    const float cell = 2.0f / (float)N;
    const float jx = (1.0f - kClumpJitter) * 0.5f + kClumpJitter * clumpHashF(h, 1u);
    const float jy = (1.0f - kClumpJitter) * 0.5f + kClumpJitter * clumpHashF(h, 2u);
    return clumpFaceDir(face, -1.0f + ((float)i + jx) * cell, -1.0f + ((float)j + jy) * cell);
}

/// Lo que dice el campo en el sitio.
struct ClumpField {
    float cover = 0.0f;   ///< fracción de cielo [0,1]
    float baseM = 0.0f;   ///< cota de la base (m sobre el radio)
    float topM  = 0.0f;   ///< cota del techo (m sobre el radio)
};

/// Un racimo: si hay, su ancho, su alto y su fuerza.
struct Clump {
    bool  present = false;
    float widthM  = 0.0f;
    float heightM = 0.0f;
    float strength = 0.0f;
    float baseOffsetM = 0.0f;   ///< su base respecto a la del campo (±kClumpBaseJitterM/2)
    int   blobs   = 0;      ///< cuántos bultos tiene a detalle completo
    int   main    = 0;      ///< de ellos, los principales (el resto son bolitas sobre ellos)
};

/// E[(min + span·s)²] con s uniforme en [s0, 1] (los presentes): cerrada.
inline float clumpSizeE2(float pres) {
    const float s0 = 1.0f - pres / std::max(pres, kClumpPresFloor);
    const float a = kClumpSizeMin, b = kClumpSizeSpan;
    const float hi = (a + b) * (a + b) * (a + b), lo = (a + b * s0) * (a + b * s0) * (a + b * s0);
    return (hi - lo) / (3.0f * b * std::max(1.0f - s0, 1e-4f));
}
/// La corrección de la retícula, interpolada en la tabla.
inline float clumpLatticeFix(float c) {
    const float x = std::clamp(c, 0.0f, 1.0f) * 10.0f;
    const int i = std::min((int)x, 9);
    return kClumpLatticeFix[i] + (kClumpLatticeFix[i + 1] - kClumpLatticeFix[i]) * (x - (float)i);
}
/// k: el ancho medio en celdas para que el término de cobertura sea `x` (−ln(1−c) corregido).
inline float clumpWidthCellsFor(float pres, float x) {
    if (pres <= 0.0f || x <= 0.0f) return 0.0f;
    return std::sqrt(x * 4.0f / (3.14159265f * pres * clumpSizeE2(pres) * kClumpFill));
}
/// k(c): el ancho medio en celdas para que la fracción cubierta sea c (ver la cabecera).
inline float clumpWidthCells(float cover) {
    const float c = std::clamp(cover, 0.0f, kClumpCoverMax);
    const float pres = std::min(1.0f, kClumpPresPerCover * c);
    return clumpWidthCellsFor(pres, -std::log(1.0f - c) * clumpLatticeFix(c));
}
/// Tamaño relativo de un sitio de rango u con presencia pres (0 si no está).
inline float clumpSizeOf(float u, float pres) {
    if (pres <= 0.0f || u >= pres) return 0.0f;
    const float s = 1.0f - u / std::max(pres, kClumpPresFloor);
    const float t = std::clamp((pres - u) / kClumpBirth, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t) * (kClumpSizeMin + kClumpSizeSpan * s);
}

/// De QUÉ BULTOS se hace un racimo de ancho y alto dados (cuántos hombros, cuánta coliflor, cuántas
/// bolitas). Aparte de `clumpAt` para poder hornear racimos de forma fija (`World/Props/BlobSurface`).
inline void clumpSetShape(Clump& c, uint32_t h) {
    const int nd  = 5 + (int)((h >> 4) & 1u);
    const float rw = 0.5f * c.widthM;
    const int nUp = std::clamp((int)(2.0f + 3.0f * c.heightM / std::max(rw, 1e-6f)), 2, kClumpMaxMain - 2 - nd);
    c.main  = 2 + nd + nUp;
    c.blobs = c.main + std::min(kClumpMaxBlobs - c.main, 2 * c.main);
}

/// El racimo del sitio con hash `h`, en una celda de `cellM` metros. `widthCells` < 0 = la ley (k(c));
/// la calibración de `clump_coverage` lo fija a mano para buscar el que da la cobertura pedida.
/// `aspectMul` escala el alto máximo respecto al ancho: 1 = cúmulo; menos = elementos más planos
/// (altocúmulo, lámina de cirro). Es ASPECTO del tipo de nube, no dónde ni cuánta: eso es el campo.
inline Clump clumpAt(const ClumpField& f, uint32_t h, float cellM, float widthCells = -1.0f, float aspectMul = 1.0f) {
    Clump c;
    const float cover = std::clamp(f.cover, 0.0f, kClumpCoverMax);
    const float pres = std::min(1.0f, kClumpPresPerCover * cover);
    const float u = clumpHashF(h, 3u);
    const float k = (widthCells >= 0.0f) ? widthCells : clumpWidthCells(cover);
    const float w = k * cellM * clumpSizeOf(u, pres);
    if (w <= 1.0f) return c;
    const float s = 1.0f - u / std::max(pres, kClumpPresFloor);
    // ALTO: la capa convectiva del campo, más cuanto más fuerte el núcleo (un cúmulo grande es el del
    // ascenso fuerte, y sube). Nunca más fino que 0,3 del ancho salvo que la capa no dé para más:
    // ahí sale una torta ancha, que es lo que es un estratocúmulo.
    const float depth = std::max(f.topM - f.baseM, 0.0f);
    // Y NO MÁS ALTO QUE ANCHO salvo los más fuertes: un cúmulo de buen tiempo es un MONTÓN (más ancho
    // que alto); solo el ascenso fuerte hace torre. Sin este tope, con una capa de 2 km todos los
    // racimos salían torres estrechas y el cielo era un campo de campanas.
    const float aspect = (0.6f + 1.8f * glm::smoothstep(0.75f, 1.0f, s)) * aspectMul;   // alto máximo, en anchos
    const float want  = std::min(std::max(depth * (0.30f + 0.90f * s), 0.30f * w), aspect * w);
    // Y NACE BAJO, como nace estrecho: con el mínimo de 120 m a secas, un racimo de 1 m de ancho salía
    // ya con 120 m de alto (una aguja que aparece de golpe: `clump_no_pop`).
    const float tb = std::clamp((pres - u) / kClumpBirth, 0.0f, 1.0f);
    c.heightM  = std::clamp(want, kClumpMinHeightM, std::max(1.2f * depth, kClumpMinHeightM)) * (tb * tb * (3.0f - 2.0f * tb));
    c.widthM   = w;
    c.strength = s;
    c.present  = true;
    c.baseOffsetM = (clumpHashF(h, 6u) - 0.5f) * kClumpBaseJitterM;
    clumpSetShape(c, h);
    return c;
}

/// Un bulto: posición de su CENTRO en el marco del racimo (x, z horizontales; y = altura sobre la
/// base) y sus semiejes. `dome` = medio elipsoide con la tapa plana EN la base (y = 0).
struct ClumpBlob {
    glm::vec3 center{0.0f};
    glm::vec3 radii{0.0f};
    float     yaw  = 0.0f;
    bool      dome = false;
};

/// El bulto k de un racimo (0 ≤ k < c.blobs).
inline ClumpBlob clumpBlob(const Clump& c, uint32_t h, int k) {
    ClumpBlob b;
    const float rw = 0.5f * c.widthM, H = c.heightM;
    const uint32_t hk = clumpHash(h ^ ((uint32_t)(k + 1) * 0x9E3779B9u));
    const float a = clumpHashF(hk, 1u), bb = clumpHashF(hk, 2u), r3 = clumpHashF(hk, 3u);
    b.yaw = clumpHashF(hk, 4u) * 6.2831853f;
    const int nd = 5 + (int)((h >> 4) & 1u);
    const float theta0 = clumpHashF(h, 5u) * 6.2831853f;
    // El CUERPO: el domo central, del que salen los lóbulos.
    // El cuerpo es el CENTRO y no la silueta: si domina (un medio elipsoide liso de casi todo el
    // ancho) el racimo se lee como una campana. La silueta la hacen los 5-6 hombros que salen de él.
    const float coreH = 0.55f * rw, coreV = 0.58f * H;
    if (k == 0) {
        b.dome = true;
        b.radii = glm::vec3(coreH * (0.9f + 0.2f * a), coreV, coreH * (0.9f + 0.2f * bb));
        return b;
    }
    if (k == 1) {   // la corona: arriba del todo
        const float r = std::min(0.45f * rw, 0.40f * H);
        const float ang = a * 6.2831853f, rho = 0.12f * rw * bb;
        b.center = glm::vec3(std::cos(ang) * rho, H - r, std::sin(ang) * rho);
        b.radii  = glm::vec3(r);
        if (b.center.y - r < 0.0f) { b.dome = true; b.center.y = kClumpDomeStep * H; b.radii = glm::vec3(r, H, r); }
        return b;
    }
    if (k < 2 + nd) {   // hombros: lóbulos GRANDES en los flancos, apoyados en la base: el borde y la panza abultada
        const float ang = theta0 + (float)(k - 2) * 6.2831853f / (float)nd + (a - 0.5f) * 0.7f;
        const float rho = rw * (0.45f + 0.18f * bb);
        const float r = std::min(rw * (0.32f + 0.18f * r3), 0.48f * H);
        const float y = std::clamp(H * (0.22f + 0.20f * a), r, std::max(H - r, r));   // ni cuelga bajo la base ni pasa del techo
        b.center = glm::vec3(std::cos(ang) * rho, y, std::sin(ang) * rho);
        b.radii  = glm::vec3(r);
        return b;
    }
    // Las BOLITAS: sobre la mitad de arriba de un bulto principal (no el cuerpo: dentro de él no se
    // verían), asomando. Repartidas en orden entre todos, para que el detalle crezca por igual.
    if (k >= c.main) {
        const int j = k - c.main;
        const ClumpBlob par = clumpBlob(c, h, 1 + j % std::max(c.main - 1, 1));
        const float phi = 1.35f * std::sqrt(bb);          // desde la vertical: más en los flancos que en la cima
        const float ang = a * 6.2831853f;
        const glm::vec3 u(std::sin(phi) * std::cos(ang), std::cos(phi), std::sin(phi) * std::sin(ang));
        const float rp = std::min(par.radii.x, par.radii.z);
        const float r  = std::min(rp * (0.30f + 0.22f * r3), 0.45f * H);   // en una torta baja, bolitas bajas
        glm::vec3 p = par.center + u * glm::vec3(par.radii.x, std::min(par.radii.y, rp * 1.2f), par.radii.z) * 0.88f;
        p.y = std::clamp(p.y, r, std::max(H - r, r));      // ni cuelga bajo la base ni pasa del techo
        b.center = p;
        b.radii  = glm::vec3(r);
        return b;
    }
    // La COLIFLOR: lóbulos con el centro SOBRE la superficie del cuerpo, asomando a medias (dentro de
    // él no se verían). Cuanto más arriba, más cerca del eje y más pequeños.
    const int nUp = c.main - 2 - nd;
    const int j = k - 2 - nd;
    const float t = ((float)j + 0.5f) / (float)std::max(nUp, 1);
    const float phi = 1.25f - 1.0f * t + (bb - 0.5f) * 0.3f;   // ángulo desde la vertical (rad)
    const float ang = a * 6.2831853f;
    const float r = std::min(rw * (0.30f - 0.08f * t) * (0.8f + 0.4f * r3), H * 0.38f);
    glm::vec3 p(std::sin(phi) * std::cos(ang) * coreH, std::cos(phi) * coreV, std::sin(phi) * std::sin(ang) * coreH);
    p.y = std::clamp(p.y, r, std::max(H - r, r));   // ni cuelga bajo la base ni pasa del techo
    b.center = p;
    b.radii  = glm::vec3(r);
    return b;
}

} // namespace Haruka::World
