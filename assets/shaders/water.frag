/**
 * @file water.frag
 * @brief Planetary ocean shading — Fase 3.
 *
 * Procedural sky reflection (no cubemap dependency: the engine's IBL is not
 * populated) + Fresnel + sun specular + Gerstner-Jacobian foam.
 *
 * u_waterQuality (settings hook, default 1):
 *   0 = sun specular only (cheapest)
 *   1 = + procedural sky reflection (default)
 *   2 = reserved for screen-space reflections (future)
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in float WaveHeight;
layout(location = 3) in float Foam;
layout(location = 4) in float IsLake;   // 1 = lago (agua dulce), 0 = océano
layout(location = 5) in float Depth;    // profundidad del agua (m): orilla≈0
layout(location = 6) in vec3  WorldDir; // radial (planeta→frag), preciso — para la costa per-píxel

layout(location = 20) uniform int u_waterQuality;

// COSTA PER-PÍXEL (océano): evalúa la elevación del terreno EN EL PÍXEL (mismo ruido que el
// generador → paridad) para recortar la orilla exacta a cualquier distancia, sin el aliasing de
// celda de la malla. Gateado por DISTANCIA: cerca usa el Depth per-vértice (refleja el cavar, LOD
// fino); lejos usa esto (nítido, donde no hay deformación visible). Params del planeta activo.
layout(location = 22) uniform vec3  u_planetRelCam;   // cameraPos - planetCenter
layout(location = 23) uniform int   u_seed;
layout(location = 24) uniform float u_continentFreqA;
layout(location = 25) uniform float u_reliefStrength;
layout(location = 26) uniform float u_invRadius;
layout(location = 27) uniform float u_seaThreshold;
layout(location = 28) uniform int   u_pxCoast;        // 1 = costa per-píxel activa (params válidos)
layout(location = 29) uniform int   u_oceanSurface;   // 1 = superficie única (recorte per-píxel SIEMPRE, sin Depth)
layout(location = 30) uniform float u_coastWidth;     // factor de ancho de costa (seed): escala rampas distToCoast

layout(std140, binding = 0) uniform PerFrameData {
    mat4  view;
    mat4  projection;
    vec3  _cameraPos;   float _pad0;
    vec3  sunDirection; float _pad1;
    vec3  sunLightColor; float ambientStrength;
    int   enableHDR;
    int   _enableBloom; int _enableSSAO; int _enableIBL; int _enableShadows;
    int   _pad3a; int _pad3b; int _pad3c;
    vec3  moonDirection;  float moonIntensity;   // 2ª luz (luna)
    vec3  moonLightColor; float _pad4;
};

// ===== Ruido Perlin 3D — PORT EXACTO de terrain_gen.comp (paridad costa↔terreno) =====
int  nHash(int x, int y, int z, int seed) {
    int h = seed; h ^= 61 ^ x; h += (h << 3); h ^= (h >> 4);
    h += (h << 3) ^ y; h += (h << 3) ^ z; h ^= (h >> 4); h += (h << 3); return h & 0x7fffffff;
}
float nGrad(int hash, float x, float y, float z) {
    int h = hash & 15; float u = h < 8 ? x : y; float v = h < 8 ? y : z;
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}
float nFade(float t) { return t * t * (3.0 - 2.0 * t); }
float nLerp(float a, float b, float t) { return a + (b - a) * t; }
float perlin3D(vec3 pos, int seed, float scale) {
    vec3 p = pos * scale;
    int x0 = int(floor(p.x)), y0 = int(floor(p.y)), z0 = int(floor(p.z));
    float xf = p.x - float(x0), yf = p.y - float(y0), zf = p.z - float(z0);
    float u = nFade(xf), v = nFade(yf), w = nFade(zf);
    int xi = x0 & 255, yi = y0 & 255, zi = z0 & 255;
    int xj = (x0 + 1) & 255, yj = (y0 + 1) & 255, zj = (z0 + 1) & 255;
    float g000 = nGrad(nHash(xi, yi, zi, seed), xf,       yf,       zf);
    float g100 = nGrad(nHash(xj, yi, zi, seed), xf - 1.0, yf,       zf);
    float g010 = nGrad(nHash(xi, yj, zi, seed), xf,       yf - 1.0, zf);
    float g110 = nGrad(nHash(xj, yj, zi, seed), xf - 1.0, yf - 1.0, zf);
    float g001 = nGrad(nHash(xi, yi, zj, seed), xf,       yf,       zf - 1.0);
    float g101 = nGrad(nHash(xj, yi, zj, seed), xf - 1.0, yf,       zf - 1.0);
    float g011 = nGrad(nHash(xi, yj, zj, seed), xf,       yf - 1.0, zf - 1.0);
    float g111 = nGrad(nHash(xj, yj, zj, seed), xf - 1.0, yf - 1.0, zf - 1.0);
    float n00 = nLerp(g000, g100, u), n10 = nLerp(g010, g110, u);
    float n0  = nLerp(n00, n10, v);
    float n01 = nLerp(g001, g101, u), n11 = nLerp(g011, g111, u);
    float n1  = nLerp(n01, n11, v);
    return nLerp(n0, n1, w);
}
float fBmW(vec3 pos, int seed, int octaves, float persistence, float lacunarity, float scale) {
    if (octaves <= 0) return 0.0;
    float value = 0.0, amplitude = 1.0, frequency = 1.0, maxValue = 0.0;
    for (int i = 0; i < octaves; ++i) {
        value += perlin3D(pos, seed + i, scale * frequency) * amplitude;
        maxValue += amplitude; amplitude *= persistence; frequency *= lacunarity;
    }
    return value / maxValue;
}
float ssfW(float e0, float e1, float x) {
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0); return t * t * (3.0 - 2.0 * t);
}

// ===== FASE 4: Perlin con GRADIENTE ANALÍTICO — PORT EXACTO (igual que terrain_gen.comp / C++) =====
float nGradD(int hash, float x, float y, float z, out vec3 coef) {
    int h = hash & 15;
    float u = h < 8 ? x : y;
    float v = h < 8 ? y : z;
    float su = (h & 1) == 0 ? 1.0 : -1.0;
    float sv = (h & 2) == 0 ? 1.0 : -1.0;
    coef = vec3(0.0);
    if (h < 8) { coef.x = su; coef.y = sv; } else { coef.y = su; coef.z = sv; }
    return su * u + sv * v;
}
float perlin3D_d(vec3 pos, out vec3 outGrad, int seed, float scale) {
    vec3 p = pos * scale;
    int x0 = int(floor(p.x)), y0 = int(floor(p.y)), z0 = int(floor(p.z));
    float xf = p.x - float(x0), yf = p.y - float(y0), zf = p.z - float(z0);
    float su = nFade(xf), sv = nFade(yf), sw = nFade(zf);
    float dsu = 6.0 * xf * (1.0 - xf), dsv = 6.0 * yf * (1.0 - yf), dsw = 6.0 * zf * (1.0 - zf);
    int cxA[2] = int[2](x0 & 255, (x0 + 1) & 255);
    int cyA[2] = int[2](y0 & 255, (y0 + 1) & 255);
    int czA[2] = int[2](z0 & 255, (z0 + 1) & 255);
    float wx[2] = float[2](1.0 - su, su), dwx[2] = float[2](-dsu, dsu);
    float wy[2] = float[2](1.0 - sv, sv), dwy[2] = float[2](-dsv, dsv);
    float wz[2] = float[2](1.0 - sw, sw), dwz[2] = float[2](-dsw, dsw);
    float n = 0.0; vec3 dn = vec3(0.0);
    for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j)
    for (int k = 0; k < 2; ++k) {
        vec3 gc;
        float V = nGradD(nHash(cxA[i], cyA[j], czA[k], seed), xf - float(i), yf - float(j), zf - float(k), gc);
        float wxyz = wx[i] * wy[j] * wz[k];
        n    += V * wxyz;
        dn.x += gc.x * wxyz + V * dwx[i] * wy[j] * wz[k];
        dn.y += gc.y * wxyz + V * wx[i] * dwy[j] * wz[k];
        dn.z += gc.z * wxyz + V * wx[i] * wy[j] * dwz[k];
    }
    outGrad = dn * scale;
    return n;
}
vec3 fBmGradW(vec3 pos, int seed, int octaves, float persistence, float lacunarity, float scale) {
    float amplitude = 1.0, frequency = 1.0, maxValue = 0.0;
    vec3 grad = vec3(0.0);
    for (int i = 0; i < octaves; ++i) {
        vec3 gn;
        perlin3D_d(pos, gn, seed + i, scale * frequency);
        grad += gn * amplitude;
        maxValue += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }
    return grad / maxValue;
}

// Elevación TERRAN oceánica EXACTA (km, <0 mar / >0 tierra) = PORT LITERAL de terranElevKm de
// terrain_gen.comp SIN lagos ni deformación (los lagos van por su malla; la deformación/cavar la
// refleja el Depth per-vértice cerca). Como fBmW≡fBm y ssfW≡ssf, esto da la MISMA costa que el
// terreno → costa per-píxel EXACTA, sin "discos" (el fallo antiguo era usar (seaThreshold+0.013)−c,
// una versión simplificada que no casaba). Coste: 6 fBm/píxel → gateado a la franja de costa.
const float A_NORM = 0.45;
// distToCoast (m, con signo) — MISMA fórmula/epsilon que terrain_gen.comp y terrain_sampler_v2.cpp →
// paridad EXACTA con el terreno (si no, la costa per-píxel discreparía = "discos"). Ver PLAN v2 mar.
float distToCoastW(vec3 dir, float c, float c0) {
    // FASE 4: gradiente ANALÍTICO (fBmGradW) — 1 evaluación, paridad con terreno CPU/GPU.
    vec3 g3   = fBmGradW(dir, u_seed, 6, 0.5, 2.0, u_continentFreqA);
    vec3 gtan = g3 - dot(g3, dir) * dir;
    float gmag = length(gtan);
    float ddir = (c - c0) / max(gmag, 1e-5);
    return ddir / max(u_invRadius, 1e-20);
}
float oceanElevKm(vec3 dir) {
    int seed = u_seed;
    float c  = fBmW(dir, seed, 6, 0.5, 2.0, u_continentFreqA);
    float c0 = u_seaThreshold;
    float landMask = ssfW(c0 - 0.04, c0 + 0.04, c);
    // EARLY-OUT (perf, mar abierto): elev = mix(seaDepth<0, landRelief>0, landMask) → el contorno
    // elev=0 SOLO cae dentro de la banda 0<landMask<1. Fuera de ella el signo ya está decidido, así
    // que devolvemos un valor hondo/alto BARATO y nos saltamos distToCoast (4 fBm) + relieve (5 fBm):
    // ~1 fBm en vez de ~10 por píxel de mar abierto (el grueso de water.draw). No mueve la orilla.
    if (landMask <= 0.0) return -5.0;   // océano hondo (signo correcto; magnitud aprox, sin efecto de orilla)
    if (landMask >= 1.0) return  5.0;   // tierra firme (la ocluye el terreno opaco de todas formas)
    float dc = distToCoastW(dir, c, c0);          // metros, >0 tierra <0 mar (paridad con el terreno)
    // BATIMETRÍA (paridad con terrain_gen.comp / terrain_sampler_v2): plataforma → talud → abisal.
    float off    = -dc;                                        // metros mar adentro (>0)
    float shelfD = ssfW(0.0,                    3000.0 * u_coastWidth, off);
    float slopeD = ssfW(3000.0 * u_coastWidth,  12000.0 * u_coastWidth, off);
    float abyssD = ssfW(12000.0 * u_coastWidth, 45000.0 * u_coastWidth, off);
    float seaDepth = shelfD * (-0.18) + slopeD * (-3.6) + abyssD * (-1.0);
    float oreg  = fBmW(dir, seed + 211, 4, 0.5, 2.0, 3.0);
    float oMask = ssfW(0.05, 0.30, oreg);
    float omn   = fBmW(dir, seed + 311, 5, 0.5, 2.1, 600.0);
    float ona   = omn * (1.0 / A_NORM);
    float oform = pow(clamp(1.0 - abs(ona), 0.0, 1.0), 1.5);
    float oceanRelief = (oform - 0.5) * oMask * 1.6;
    seaDepth = min(seaDepth + oceanRelief * slopeD, -0.02);
    float landRamp = ssfW(0.0, 1500.0 * u_coastWidth, dc);
    float landBase = landRamp * 1.0;
    float hmn   = fBmW(dir, seed + 77, 5, 0.5, 2.0, 300.0);
    float hillRelief = (hmn * (1.0 / A_NORM)) * landRamp * 1.3;
    float reg     = fBmW(dir, seed + 55, 4, 0.5, 2.0, 2.0);
    float mtnMask = ssfW(0.12, 0.36, reg);
    float mn      = fBmW(dir, seed + 123, 5, 0.5, 2.1, 800.0);
    float form    = pow(clamp(1.0 - abs(mn * (1.0 / A_NORM)), 0.0, 1.0), 1.3);
    float mountains  = form * mtnMask * (6.5 * u_reliefStrength);
    float landRelief = max(landBase + hillRelief + mountains, 0.01);
    float elev = mix(seaDepth, landRelief, landMask);
    // COSTA PURA POR landMask (continente de baja freq, ESTABLE entre LODs) → clamp de AMBOS lados:
    // landMask>0.5 = tierra (≥+5mm), landMask<0.5 = océano (≤−5mm). Elimina islotes/charcos sub-celda
    // JUSTO al nivel del mar que el morph CDLOD hacía flipar tierra↔agua entre LODs → SIN panales/
    // flicker. La costa queda en la línea landMask=0.5 (suave, 6-oct). Lagos = sistema aparte.
    elev = (landMask > 0.5) ? max(elev, 0.005) : min(elev, -0.005);
    return elev;
}

const vec3 DEEP_COLOR    = vec3(0.015, 0.07, 0.14);
const vec3 SHALLOW_COLOR = vec3(0.05, 0.28, 0.40);
// Lago (agua dulce): más turquesa/verde y menos profundo que el océano.
const vec3 LAKE_DEEP     = vec3(0.04, 0.16, 0.18);
const vec3 LAKE_SHALLOW  = vec3(0.12, 0.40, 0.42);

// Cheap analytic sky: horizon haze → zenith blue, plus a soft sun halo.
vec3 skyColor(vec3 dir, vec3 sunDir) {
    float up   = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3  zen  = vec3(0.20, 0.42, 0.82);            // cenit azul profundo
    vec3  hor  = vec3(0.68, 0.80, 0.94);            // horizonte pálido
    vec3  base = mix(hor, zen, pow(up, 0.55));
    // Reflejo del sol en DOS lóbulos: HALO atmosférico ancho (calidez alrededor del sol) + DISCO
    // estrecho brillante → el cielo reflejado se ve atmosférico, no un degradado plano.
    float sd   = max(dot(dir, sunDir), 0.0);
    float halo = pow(sd, 8.0) * 0.35;
    float disk = pow(sd, 220.0);
    base = mix(base, vec3(1.00, 0.94, 0.82), halo); // tinte cálido cerca del sol
    return base + sunLightColor * disk * 0.8;
}

void main() {
    // Recorte de costa por PROFUNDIDAD con signo (negativa sobre tierra), SIN depender del depth
    // buffer. En vez de un discard DURO en Depth=0 —que a distancia aliasa la orilla en un patrón
    // de puntos/halftone (una celda de agua ≈ 1 píxel entra/sale de golpe)— usamos una RAMPA de
    // alpha suave en los primeros decímetros: el agua se FUNDE con el lecho → orilla anti-aliasada.
    // Descartamos solo lo claramente seco (tierra) para ahorrar relleno.
    // Recorte de costa. CERCA = Depth per-vértice (refleja el cavar/deformación, y el LOD ya es
    // fino). LEJOS y solo en OCÉANO = elevación del terreno evaluada POR PÍXEL (orilla exacta, sin
    // el aliasing de celda de la malla). Mezcla por distancia → seguro con el cavar (cerca nunca
    // usa el ruido base) y nítido en órbita (donde no hay deformación visible).
    // Costa per-píxel. Peso SUAVE por profundidad (coastW) en vez de un gate duro → sin círculos
    // (el gate duro creaba isóbatas circulares visibles). coastW llega a 0 a ~2 km de profundidad,
    // ANTES del borde de evaluación (3 km) → ese borde es INVISIBLE (peso ya 0) y el agua profunda
    // SIEMPRE se ve (coastW=0 → clipDepth=Depth). Solo lejos (>4km) y solo océano (cerca/lagos =
    // per-vértice, refleja el cavar). Continente 6-oct = barato. Params verificados correctos.
    // COSTA PER-PÍXEL EXACTA (opción B). CERCA = Depth per-vértice (refleja el cavar/deformación).
    // A partir de ~150 m = elevación del terreno EVALUADA POR PÍXEL con la MISMA función que el
    // terreno (oceanElevKm) → la orilla es la curva REAL elev=0, no la malla facetada por celda.
    // Como la paridad es EXACTA, la mezcla per-vértice↔per-píxel es continua (sin discos, sin
    // escalones). Solo se evalúa en la franja de costa (|Depth| pequeño) por coste (6 fBm/píxel).
    float clipDepth = Depth;
    if (u_oceanSurface == 1) {
        // SUPERFICIE ÚNICA de océano (test): no hay Depth por-vértice del terreno → recorte per-píxel
        // EXACTO en TODOS los píxeles (la orilla = curva real elev=0). El terreno opaco ocluye por
        // depth lo que quede sobre tierra; esto descarta el resto → el mar solo se ve donde hay mar.
        clipDepth = -oceanElevKm(normalize(WorldDir)) * 1000.0;
    } else if (u_pxCoast == 1 && IsLake < 0.5 && abs(Depth) < 1200.0) { // banda estrecha (perf: menos 6-fBm/píxel)
        vec3  dir     = normalize(WorldDir);              // radial PRECISO (no float32 a escala planeta)
        float pxDepth = -oceanElevKm(dir) * 1000.0;       // profundidad con signo (>0 mar, <0 tierra)
        // farW APRETADO (40→220 m): a partir de ~220 m manda el per-píxel exacto → descarta el agua
        // GRUESA sobre tierra (celdas de agua a LOD grueso sobre terreno fino) que dejaba blobs. Muy
        // cerca (<40 m, donde cavas) manda la malla per-vértice. Con dir preciso, paridad exacta.
        float farW    = smoothstep(40.0, 220.0, length(FragPos));
        clipDepth = mix(Depth, pxDepth, farW);
    }
    if (clipDepth <= 0.0) discard;
    // Profundidad real en la orilla: en el océano único Depth es constante (50 m) → no había bajío
    // turquesa ni espuma de orilla y el filo terreno↔agua se veía dentado (malla facetada). Usamos
    // la profundidad per-píxel (clipDepth) → bajío + espuma + AA del borde enmascaran el faceteado.
    float shoreDepthM = (u_oceanSurface == 1) ? clipDepth : Depth;
    // AA del filo del agua: fade suave en los últimos ~0.6 m antes de la orilla (mata el escalón duro).
    float shoreAlpha = smoothstep(0.0, 0.6, clipDepth);

    vec3 N = normalize(Normal);
    vec3 V = normalize(-FragPos);   // camera at origin in cam-relative space
    vec3 L = normalize(sunDirection);
    vec3 H = normalize(L + V);

    float ndv = max(dot(N, V), 0.0);

    // Fresnel (Schlick) — more reflective at grazing angles.
    float fresnel = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);

    // Body colour: deeper looking straight down, lighter at grazing angle.
    // Lago vs océano: paleta de agua dulce (turquesa) si IsLake.
    vec3 deep    = mix(DEEP_COLOR,    LAKE_DEEP,    IsLake);
    vec3 shallow = mix(SHALLOW_COLOR, LAKE_SHALLOW, IsLake);
    // Color por PROFUNDIDAD real: turquesa en el bajío → azul oscuro en lo hondo.
    // (+ un poco de aclarado por ángulo rasante para el brillo del horizonte.)
    float shallowF = exp(-shoreDepthM * 0.10);      // 1 en la orilla, →0 a ~20-30 m
    vec3 body = mix(deep, shallow, max(shallowF, ndv * 0.35));

    vec3 color = body;

    // Reflection (quality-gated).
    if (u_waterQuality >= 1) {
        vec3 R = reflect(-V, N);
        vec3 sky = skyColor(R, L);
        color = mix(body, sky, fresnel);
    } else {
        // Low: just a flat sky tint via fresnel.
        color = mix(body, vec3(0.45, 0.62, 0.85), fresnel);
    }

    // Diffuse lift so the night side isn't pure black.
    float ndl = max(dot(N, L), 0.0);
    color += sunLightColor * body * ndl * 0.25;

    // Luz de luna: aporte difuso azulado + un brillo especular suave (reflejo lunar
    // en el agua) → el mar nocturno no queda negro y tiene un destello plateado.
    vec3  ML       = normalize(moonDirection);
    float ndlMoon  = max(dot(N, ML), 0.0);
    vec3  HM       = normalize(ML + V);
    float specMoon = pow(max(dot(N, HM), 0.0), 300.0);
    color += moonLightColor * moonIntensity * (body * ndlMoon + vec3(specMoon) * 0.3);

    // Sun specular glint.
    // Sun specular glint — más cerrado y tenue (el brillo anterior era demasiado
    // drástico/irreal): reflejo de sol puntual, no un fogonazo en todo el mar.
    // Glint del sol ACOTADO: a distancia la normal se aplana a radial → el glint se ensancha a una
    // región grande; con sunLightColor HDR reventaba a BLANCO + bloom (mancha). Bajado 0.45→0.18 y
    // CLAMP a 0.6 → destello visible sin fogonazo blanco.
    float spec = pow(max(dot(N, H), 0.0), 400.0);
    color += min(sunLightColor * spec * 0.18, vec3(0.6));
    // Lóbulo ANCHO (sheen): además del glint puntual, un brillo suave y extenso da al reflejo del sol
    // una caída natural (sparkle + sheen) en vez de un único punto duro. Gateado por fresnel (borde).
    float specSheen = pow(max(dot(N, H), 0.0), 60.0);
    color += min(sunLightColor * specSheen * 0.05, vec3(0.15)) * fresnel;

    // SUBSURFACE SCATTERING: la luz atraviesa la cresta de la ola y sale hacia el ojo → brillo
    // turquesa TRANSLÚCIDO, intenso a CONTRALUZ (sol tras la ola) y en crestas altas (agua fina).
    // Es lo que separa un mar "vivo" de un espejo plano con espuma. Solo ALU → barato, sin texturas.
    vec3  sssTint  = mix(vec3(0.05, 0.34, 0.30), vec3(0.10, 0.46, 0.40), IsLake); // teal océano/lago
    float backLit  = max(dot(-V, L), 0.0);                        // cámara mira hacia el sol a través del agua
    float crestLit = clamp(WaveHeight * 0.5 + 0.30, 0.0, 1.0);    // más en crestas (agua fina)
    float sss      = pow(backLit, 3.0) * crestLit * (0.35 + 0.65 * fresnel);
    color += sssTint * sunLightColor * sss * 0.7;

    color += ambientStrength * body;

    // Espuma de ORILLA: banda blanca donde el agua es muy somera (Depth→0),
    // modulada por el oleaje (las crestas empujan la espuma). Vale para mar y lago.
    float shoreFoam = (1.0 - smoothstep(0.0, 2.2, shoreDepthM))
                    * (0.55 + 0.45 * smoothstep(-0.4, 1.0, WaveHeight));
    // Foam de cresta (Gerstner-Jacobian + crestas altas): casi nula en lagos calmos.
    float crestFoam = smoothstep(1.2, 2.2, WaveHeight) * (1.0 - 0.9 * IsLake);
    float foam = clamp(max(max(Foam * (1.0 - 0.9 * IsLake), crestFoam), shoreFoam), 0.0, 1.0);
    color = mix(color, vec3(0.85, 0.92, 0.97), foam * 0.75);

    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    // El AGUA es el medio TRANSLÚCIDO (no la cámara): se ve el LECHO A TRAVÉS, teñido por el color
    // del agua (alpha-blend sobre el terreno ya dibujado). Confinado a CERCA + SOMERO:
    //  · nearF: solo a < ~150 m (donde estás nadando/en la orilla); a distancia OPACA para que el
    //    lecho grueso NO asome en halftone (el bug de los círculos marrones era translucidez lejana).
    //  · shallow: aguas someras claras (ves la arena); lo HONDO opaco (azul denso, realista).
    // Así "ves a través del agua" donde importa, sin ensuciar el mar lejano.
    float camDistW = length(FragPos);
    float nearF    = 1.0 - smoothstep(150.0, 800.0, camDistW);   // 1 muy cerca → 0 lejos
    float shallowW = 1.0 - smoothstep(0.0, 12.0, shoreDepthM);    // 1 somero → 0 hondo
    float minA     = mix(1.0, 0.55, nearF * shallowW);           // ves el fondo, pero el agua sigue leyéndose como MAR (no barro)
    float alpha    = mix(minA, 1.0, max(fresnel, foam)) * shoreAlpha;
    FragColor = vec4(color, alpha);
}
