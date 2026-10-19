/**
 * @file shell_clumps.glsl
 * @brief RACIMOS DE BULTOS SOBRE UNA CÁSCARA: GEMELO EXACTO de `src/Haruka/World/ShellClumps.h` (léelo
 *        allí: la retícula y la ley que conserva la cobertura). Lo usa `shell_scatter.comp`. Solo
 *        DÓNDE y CUÁNTO MIDE cada racimo: sus bultos se hornean en CPU (`BlobSurface`), una vez.
 *
 * Si se toca una constante aquí, se toca allí: los tests de CPU miden la cobertura y la base plana con
 * la ley de C++, y el banco de GPU compara lo que siembra el compute con ella.
 */
#ifndef HARUKA_SHELL_CLUMPS_GLSL
#define HARUKA_SHELL_CLUMPS_GLSL

const float HARUKA_CLUMP_JITTER        = 0.80;
const float HARUKA_CLUMP_PRES_PER_COVER = 2.5;
const float HARUKA_CLUMP_PRES_FLOOR    = 0.25;
const float HARUKA_CLUMP_BIRTH         = 0.15;
const float HARUKA_CLUMP_SIZE_MIN      = 0.55;
const float HARUKA_CLUMP_SIZE_SPAN     = 0.90;
const float HARUKA_CLUMP_FILL          = 0.69;
const float HARUKA_CLUMP_LATTICE_FIX[11] = float[11](4.36, 1.067, 0.886, 0.807, 0.747, 0.718, 0.694, 0.686, 0.713, 0.795, 0.85);
const float HARUKA_CLUMP_COVER_MAX     = 0.97;
const float HARUKA_CLUMP_MIN_HEIGHT_M  = 120.0;
/// Alto de las variantes horneadas con semiancho 1 (`kClumpBakeH`): la escala vertical es alto / esto.
const float HARUKA_CLUMP_BAKE_H       = 1.6;
const float HARUKA_CLUMP_BASE_JITTER_M = 80.0;

uint harukaClumpHash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float harukaClumpHashF(uint h, uint salt) {
    return float(harukaClumpHash(h ^ (salt * 0x85EBCA6Bu)) & 0xFFFFFFu) / 16777216.0;
}
uint harukaClumpCellHash(int face, uint i, uint j, uint seed) {
    return harukaClumpHash((i * 73856093u) ^ (j * 19349663u) ^ (uint(face) * 83492791u) ^ seed);
}

vec3 harukaClumpFaceDir(int face, float lx, float ly) {
    vec3 p;
    if      (face == 0) p = vec3( 1.0,   ly,  -lx);
    else if (face == 1) p = vec3(-1.0,   ly,   lx);
    else if (face == 2) p = vec3(  lx,  1.0,  -ly);
    else if (face == 3) p = vec3(  lx, -1.0,   ly);
    else if (face == 4) p = vec3(  lx,   ly,  1.0);
    else                p = vec3( -lx,   ly, -1.0);
    const float x2 = p.x * p.x, y2 = p.y * p.y, z2 = p.z * p.z;
    return vec3(p.x * sqrt(1.0 - y2 * 0.5 - z2 * 0.5 + y2 * z2 / 3.0),
                p.y * sqrt(1.0 - z2 * 0.5 - x2 * 0.5 + z2 * x2 / 3.0),
                p.z * sqrt(1.0 - x2 * 0.5 - y2 * 0.5 + x2 * y2 / 3.0));
}

vec3 harukaClumpSiteDir(int face, uint i, uint j, uint N, uint h) {
    const float cell = 2.0 / float(N);
    const float jx = (1.0 - HARUKA_CLUMP_JITTER) * 0.5 + HARUKA_CLUMP_JITTER * harukaClumpHashF(h, 1u);
    const float jy = (1.0 - HARUKA_CLUMP_JITTER) * 0.5 + HARUKA_CLUMP_JITTER * harukaClumpHashF(h, 2u);
    return harukaClumpFaceDir(face, -1.0 + (float(i) + jx) * cell, -1.0 + (float(j) + jy) * cell);
}

/// (Sin los bultos: en la GPU solo hace falta dónde está y cuánto mide; la forma va horneada.)
struct HarukaClump { bool present; float widthM; float heightM; float strength; float baseOffsetM; };

float harukaClumpSizeE2(float pres) {
    const float s0 = 1.0 - pres / max(pres, HARUKA_CLUMP_PRES_FLOOR);
    const float a = HARUKA_CLUMP_SIZE_MIN, b = HARUKA_CLUMP_SIZE_SPAN;
    const float hi = (a + b) * (a + b) * (a + b), lo = (a + b * s0) * (a + b * s0) * (a + b * s0);
    return (hi - lo) / (3.0 * b * max(1.0 - s0, 1e-4));
}
float harukaClumpLatticeFix(float c) {
    const float x = clamp(c, 0.0, 1.0) * 10.0;
    const int i = min(int(x), 9);
    return HARUKA_CLUMP_LATTICE_FIX[i] + (HARUKA_CLUMP_LATTICE_FIX[i + 1] - HARUKA_CLUMP_LATTICE_FIX[i]) * (x - float(i));
}
float harukaClumpWidthCells(float cover) {
    const float c = clamp(cover, 0.0, HARUKA_CLUMP_COVER_MAX);
    const float pres = min(1.0, HARUKA_CLUMP_PRES_PER_COVER * c);
    const float x = -log(1.0 - c) * harukaClumpLatticeFix(c);
    if (pres <= 0.0 || x <= 0.0) return 0.0;
    return sqrt(x * 4.0 / (3.14159265 * pres * harukaClumpSizeE2(pres) * HARUKA_CLUMP_FILL));
}
float harukaClumpSizeOf(float u, float pres) {
    if (pres <= 0.0 || u >= pres) return 0.0;
    const float s = 1.0 - u / max(pres, HARUKA_CLUMP_PRES_FLOOR);
    const float t = clamp((pres - u) / HARUKA_CLUMP_BIRTH, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t) * (HARUKA_CLUMP_SIZE_MIN + HARUKA_CLUMP_SIZE_SPAN * s);
}

/// `field` = (cobertura, base m, techo m).
HarukaClump harukaClumpAt(vec3 field, uint h, float cellM, float aspectMul) {
    HarukaClump c = HarukaClump(false, 0.0, 0.0, 0.0, 0.0);
    const float cover = clamp(field.x, 0.0, HARUKA_CLUMP_COVER_MAX);
    const float pres = min(1.0, HARUKA_CLUMP_PRES_PER_COVER * cover);
    const float u = harukaClumpHashF(h, 3u);
    const float w = harukaClumpWidthCells(cover) * cellM * harukaClumpSizeOf(u, pres);
    if (w <= 1.0) return c;
    const float s = 1.0 - u / max(pres, HARUKA_CLUMP_PRES_FLOOR);
    const float depth = max(field.z - field.y, 0.0);
    const float aspect = (0.6 + 1.8 * smoothstep(0.75, 1.0, s)) * aspectMul;
    const float want  = min(max(depth * (0.30 + 0.90 * s), 0.30 * w), aspect * w);
    const float tb = clamp((pres - u) / HARUKA_CLUMP_BIRTH, 0.0, 1.0);
    c.heightM  = clamp(want, HARUKA_CLUMP_MIN_HEIGHT_M, max(1.2 * depth, HARUKA_CLUMP_MIN_HEIGHT_M)) * (tb * tb * (3.0 - 2.0 * tb));
    c.widthM   = w;
    c.strength = s;
    c.present  = true;
    c.baseOffsetM = (harukaClumpHashF(h, 6u) - 0.5) * HARUKA_CLUMP_BASE_JITTER_M;
    return c;
}

#endif // HARUKA_SHELL_CLUMPS_GLSL
