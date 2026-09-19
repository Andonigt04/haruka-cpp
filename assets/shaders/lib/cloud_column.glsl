#ifndef HARUKA_CLOUD_COLUMN_GLSL
#define HARUKA_CLOUD_COLUMN_GLSL
// ═══════════════════════════════════════════════════════════════════════════════════════════════
//  LA COLUMNA DE AIRE — gemelo GLSL de `src/core/cloud_column.h`. LEE AQUEL PRIMERO.
//
//  Una nube es donde el aire esta saturado, no una capa a una cota. Aqui la columna llega ya
//  horneada por texel: base y techo de lo convectivo (m sobre el nivel del mar; la base es el
//  nivel de condensacion SOBRE EL SUELO, calculada en CPU), la cobertura de los frentes, el vapor
//  en altura (medio, alto) y la temperatura de superficie REDUCIDA AL NIVEL DEL MAR, con la que
//  la temperatura de cualquier cota sale sin conocer el suelo: T(z) = tSea − 0,0065·z.
//
//  `test_cloud_formation` mide la CPU; el banco RHI (`nubes: FORMACION`) mide que esto da lo mismo.
// ═══════════════════════════════════════════════════════════════════════════════════════════════

const float HARUKA_COL_LAPSE      = 0.0065;   // °C/m (gemelo de kLapseEnvC)
const float HARUKA_COL_TOP_FADE   = 400.0;    // m    (kBlTopFadeM)
const float HARUKA_COL_MID_C      = -10.0;    // °C   (kMidCloudC)
const float HARUKA_COL_MID_W      = 9.0;      //      (kMidCloudWidthC)
const float HARUKA_COL_HIGH_C     = -40.0;    //      (kHighCloudC)
const float HARUKA_COL_HIGH_W     = 10.0;     //      (kHighCloudWidthC)

float harukaColAirTempC(float tSeaC, float zM) { return tSeaC - HARUKA_COL_LAPSE * max(zM, 0.0); }

/// Fraccion CONVECTIVA a la cota z: 0 bajo la base, `cover` en la capa convectiva, desflecada
/// `HARUKA_COL_TOP_FADE` por encima del techo. Gemelo de `convectiveFractionAt`.
float harukaColConvective(float cover, float baseM, float topM, float zM) {
    if (cover <= 0.0 || topM <= baseM + 1.0) return 0.0;
    const float inB  = smoothstep(baseM, baseM + 60.0, zM);
    const float outT = 1.0 - smoothstep(topM, topM + HARUKA_COL_TOP_FADE, zM);
    return cover * inB * outT;
}

/// Fraccion del vapor EN ALTURA a la cota z: campanas en temperatura alrededor de −10 °C (medio)
/// y −40 °C (hielo), cortadas a 2,5 anchuras. Gemelo de `aloftFractionAt`. `tC` es la temperatura
/// del punto (ya con la perturbacion de celda, si la hay).
float harukaColAloft(float vaporMid, float vaporHigh, float tC) {
    const float dm = (tC - HARUKA_COL_MID_C)  / HARUKA_COL_MID_W;
    const float dh = (tC - HARUKA_COL_HIGH_C) / HARUKA_COL_HIGH_W;
    const float gm = (abs(dm) < 2.5) ? exp(-dm * dm) : 0.0;
    const float gh = (abs(dh) < 2.5) ? exp(-dh * dh) : 0.0;
    return max(vaporMid * gm, vaporHigh * gh);
}

/// Fase del agua: 0 agua (cumulo denso) · 1 hielo (velo, fibras). Gemelo de `iceFractionAt`.
float harukaColIce(float tC) { return smoothstep(-15.0, -38.0, tC); }

#endif
