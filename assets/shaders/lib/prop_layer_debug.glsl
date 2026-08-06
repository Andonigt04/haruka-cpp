// ===========================================================================================
// CAPAS DE PROPS — definición ÚNICA para la VISTA DE SPAWN (depuración del editor).
//
// La incluye `biome.frag`. Es el espejo GLSL de `Haruka::Planet::PropLayerTable` (prop_layer.h):
// la misma tabla que el placer recorre para instalar los objetos (árboles, rocas, casas), subida
// a un UBO para que el shader pueda pintar DÓNDE instalaría cada capa en el planeta — las
// "áreas de spawn" que el editor quería ver al seleccionar una capa.
//
// Al contrario que los materiales (cuyo `terrain_material.glsl` solo rige el ASPECTO), aquí se
// replica la REGLA de colocación: la misma `harukaBandWeight` sobre humedad/temperatura/pendiente
// multiplicada por el densityMap de la capa. Pintar `coverage` en el planeta es exactamente
// "aquí nacería un árbol de esta capa". No comparte struct con el placer (ese corre en CPU,
// GL-free, sin GLSL); este layout solo tiene que CASAR con el UBO que sube `TerrestrialPlanet::render`.
//
// Layout (debe coincidir con el struct GpuPropLayer de planet.cpp y con su subida a la GPU):
//   a = (humMin, humMax, tempMin, tempMax)
//   b = (slopeMin, slopeMax, _, feather)
//   c = (hasDensityMap, _, _, _)     hasDensityMap > 0.5 → multiplicar por uPropDensityMap
//   d = (density, _, _, _)           peso de volumen de la capa (huecos sin patrón)
// ===========================================================================================
#ifndef HARUKA_PROP_LAYER_DEBUG_GLSL
#define HARUKA_PROP_LAYER_DEBUG_GLSL

#define MAX_DEBUG_PROP_LAYERS 16

struct PropLayerDbg { vec4 a; vec4 b; vec4 c; vec4 d; };

layout(std140, binding = 14) uniform PropLayerTableUBO {
    vec4 uPropCount;                                   // x = capas activas
    PropLayerDbg uPropLayers[MAX_DEBUG_PROP_LAYERS];
};

// densityMap de la capa ACTIVA (el motor bindea aquí el de la capa que el editor seleccionó,
// o una textura blanca si la capa no lo tiene). Muestrear por píxel el mapa "manda sobre las
// bandas" igual que en el placer: así la vista de spawn coincide con la colocación real.
layout(binding = 17) uniform sampler2D uPropDensityMap;

/** @brief Cuánto cumple la capa `i` en este píxel, [0,1] = bandas × densityMap (si lo tiene). */
float harukaPropCoverage(int i, float humid, float tempC, float slope, vec2 uv) {
    float f = max(uPropLayers[i].b.w, 1e-4);
    float w = harukaBandWeight(humid, uPropLayers[i].a.x, uPropLayers[i].a.y, f)
            * harukaBandWeight(tempC,  uPropLayers[i].a.z, uPropLayers[i].a.w, max(f * 12.0, 0.5))
            * harukaBandWeight(slope,  uPropLayers[i].b.x, uPropLayers[i].b.y, f);
    float mapD = 1.0;
    if (uPropLayers[i].c.x > 0.5) mapD = texture(uPropDensityMap, uv).r;
    return w * clamp(mapD, 0.0, 1.0) * clamp(uPropLayers[i].d.x, 0.0, 1.0);
}

#endif // HARUKA_PROP_LAYER_DEBUG_GLSL
