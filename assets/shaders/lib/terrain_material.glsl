// ===========================================================================================
// MATERIALES DEL TERRENO — definición ÚNICA, compartida por los dos shaders de terreno.
//
// La incluyen `planet.frag` (terreno V3 por chunks) y el shader inline del SimplePlanet
// (`kBiomeFragSrc` en planetary_system.cpp). Antes cada uno tenía su copia del mismo look y
// cualquier ajuste había que hacerlo dos veces — la misma clase de duplicación que en este motor
// ya produjo "tres suelos distintos" y "dos climas describiendo el mismo planeta". Si el color de
// un bioma se ve distinto en el terreno de cerca y en el planeta de lejos, es que alguien tocó
// una copia.
//
// Un material NO es una textura: es una REGLA sobre el clima y la forma (humedad, temperatura,
// pendiente) más cómo se ve lo que cae dentro de ella. La regla la declara el JSON de la escena
// (`surface.materials`) y llega aquí en un UBO, así que añadir un material no toca GLSL.
//
// El COLOR no está aquí: lo pone el mapa de biomas, que sale del clima. El material aporta el
// matiz (`tint`), cuánto grano deja pasar su textura y cuánto relieve saca de su normal map.
//
// Layout (debe coincidir con `Haruka::Planet::TerrainMaterial` y con su subida a la GPU):
//   a = (humMin, humMax, tempMin, tempMax)
//   b = (slopeMin, slopeMax, tile, priority)   tile: -1 ninguno · 0 sand · 1 grass · 2 land · 3 rock
//   c = (tint.rgb, grain)
//   d = (detail, feather, zonePacked, flags)
//   e = (baseColor.rgb, colorWeight)   colorWeight <= 0 → este material no tiñe, manda el bioma
//       zonePacked = r*65536 + g*256 + b con r,g,b en 0..255. Cabe EXACTO en un float32 (mantisa de
//       24 bits → enteros exactos hasta 2^24 = 16777216, y el máximo aquí es 16777215). Se empaqueta
//       para no crecer el UBO por tres floats.
//       flags: bit0 = este material tiene zona pintada
// ===========================================================================================
#ifndef HARUKA_TERRAIN_MATERIAL_GLSL
#define HARUKA_TERRAIN_MATERIAL_GLSL

#define MAX_TERRAIN_MATERIALS 16

struct TerrainMat { vec4 a; vec4 b; vec4 c; vec4 d; vec4 e; };

layout(std140, binding = 12) uniform TerrainMaterials {
    vec4 uMatCount;                              // x = cuántos hay activos
    TerrainMat uMat[MAX_TERRAIN_MATERIALS];
};

// Pertenencia SUAVE a una banda [lo,hi]: 1 dentro, 0 fuera, con un degradado de anchura `f` en los
// bordes. Con un corte duro, dos materiales contiguos dejan una línea recta sobre el terreno que se
// ve a kilómetros; el degradado los funde.
float harukaBandWeight(float x, float lo, float hi, float f) {
    f = max(f, 1e-4);
    return smoothstep(lo - f, lo + f, x) * (1.0 - smoothstep(hi - f, hi + f, x));
}

// Recorre la tabla y devuelve el aspecto resultante en este píxel.
//   · El ASPECTO (tint/grain/detail) se MEZCLA por peso → transiciones suaves.
//   · El TILE lo gana la PRIORIDAD: mezclar dos texturas por píxel costaría el doble de muestreos
//     para algo que solo aporta grano, y el tile se nota mucho menos que el color.
// Desempaqueta el color de zona de un material (ver el layout de arriba).
vec3 harukaZoneColor(int i) {
    float p = uMat[i].d.z;
    float r = floor(p / 65536.0);
    float g = floor(mod(p, 65536.0) / 256.0);
    float b = mod(p, 256.0);
    return vec3(r, g, b);
}
bool harukaHasZone(int i) { return mod(uMat[i].d.w, 2.0) >= 1.0; }

/**
 * Material en este píxel.
 *
 * `zoneRGB` es el color leído del mapa de zonas (0..255) y `hasZoneMap` dice si ese mapa existe.
 * Cuando existe, la ZONA MANDA: se elige el material cuyo color de zona está más cerca y se ignoran
 * las reglas de clima. Es lo que hace que un mapa pintado a mano signifique algo — si el clima
 * pudiera contradecirlo, pintar una isla no garantizaría que ahí hubiera isla.
 *
 * Sin mapa (o en un material sin zona declarada) se cae a las reglas de humedad/temperatura/pendiente.
 */
void harukaSelectMaterial(float humid, float tempC, float slope,
                          vec3 zoneRGB, bool hasZoneMap,
                          out vec3 tint, out float grainAmt, out float detailAmt, out int tile,
                          out vec4 baseColor, out int matIdx)
{
    tint = vec3(1.0); grainAmt = 1.0; detailAmt = 1.0; tile = 2;   // land por defecto
    baseColor = vec4(0.0);                                          // a = peso; 0 = manda el bioma
    matIdx = -1;
    int count = int(uMatCount.x);

    if (hasZoneMap) {
        // VECINO MÁS CERCANO en el espacio de color. Sin umbral: un píxel que no case exacto con
        // ninguna entrada (antialias del pincel, recompresión del PNG) debe caer en el material más
        // parecido, no quedarse sin material y salir como un agujero.
        int best = -1; float bestD = 0.0;
        for (int i = 0; i < count && i < MAX_TERRAIN_MATERIALS; ++i) {
            if (!harukaHasZone(i)) continue;
            vec3 d = harukaZoneColor(i) - zoneRGB;
            float dist = dot(d, d);
            if (best < 0 || dist < bestD) { best = i; bestD = dist; }
        }
        if (best >= 0) {
            tint      = uMat[best].c.rgb;
            grainAmt  = uMat[best].c.a;
            detailAmt = uMat[best].d.x;
            tile      = int(uMat[best].b.z);
            baseColor = uMat[best].e;
            matIdx    = best;
            return;
        }
    }

    float wSum = 0.0, bestScore = -1.0;
    vec3  tintAcc = vec3(0.0), colAcc = vec3(0.0);
    float grainAcc = 0.0, detailAcc = 0.0, colW = 0.0;

    for (int i = 0; i < count && i < MAX_TERRAIN_MATERIALS; ++i) {
        float f = uMat[i].d.y;
        // El feather de temperatura va en GRADOS, no en la escala [0,1] de humedad/pendiente: medio
        // grado de degradado sería un corte duro, y 0.08 °C ni se ve.
        float w = harukaBandWeight(humid, uMat[i].a.x, uMat[i].a.y, f)
                * harukaBandWeight(tempC, uMat[i].a.z, uMat[i].a.w, max(f * 12.0, 0.5))
                * harukaBandWeight(slope, uMat[i].b.x, uMat[i].b.y, f);
        if (w <= 0.0) continue;

        float score = w * uMat[i].b.w;
        wSum      += w;
        tintAcc   += uMat[i].c.rgb * w;
        grainAcc  += uMat[i].c.a   * w;
        detailAcc += uMat[i].d.x   * w;
        colAcc    += uMat[i].e.rgb * w;
        colW      += uMat[i].e.a   * w;
        if (score > bestScore) { bestScore = score; tile = int(uMat[i].b.z); matIdx = i; }
    }

    if (wSum > 1e-4) {
        tint = tintAcc / wSum; grainAmt = grainAcc / wSum; detailAmt = detailAcc / wSum;
        baseColor = vec4(colAcc / wSum, colW / wSum);
    }
}

#endif // HARUKA_TERRAIN_MATERIAL_GLSL
