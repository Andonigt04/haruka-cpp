#version 460 core
#extension GL_GOOGLE_include_directive : require
/**
 * @file terrain_node_water.frag
 * @brief El agua del pase de nodos. Comparte el SOMBREADO del mar, no su geometria.
 *
 * ⚠️ NO REUSA `ocean.frag` Y ES DELIBERADO. Aquel lee `ClipParams` (binding 13) y un `vLoc` en
 * coordenadas de ANILLO para descartar el hueco — o sea, depende de la rejilla del clipmap, que es
 * justo lo que esta migracion viene a quitar. Reusarlo habria dejado el resto vivo por la puerta de
 * atras. Lo que si se comparte es `harukaOceanShade`, que es el aspecto y no el sitio.
 */
#include "lib/ocean_params.glsl"
#include "lib/ocean_wave.glsl"       // harukaWindRipple: el rizo de viento, solo luz
#include "lib/aerial.glsl"           // el aire de en medio, por fisica
#include "lib/ocean_shade.glsl"      // harukaOceanShade: absorcion, Fresnel, espuma
#include "lib/paint.glsl"            // harukaPaintValue: el ruido que rompe la espuma por pixel

layout(std140, binding = 0) uniform NodeDraw {
    mat4  uMVP; vec4 uCenter; vec4 uCenterLo; vec4 uLod; ivec4 uGrid; ivec4 uEdgeUnused;
    vec4  uMisc; vec4 uShade; vec4 uTexAnchor; vec4 uLightDir; vec4 uAerial; vec4 uStrideRamp;
};

layout(location = 0) in vec3  vNormal;
layout(location = 1) in vec3  vFragPos;
layout(location = 2) in float vDepth;
layout(location = 9) in float vBed;
layout(location = 3) in float vFoam;
layout(location = 4) flat in int vWStride;
layout(location = 5) flat in int vWLevel;
layout(location = 6) in float vSheet;
layout(location = 7) flat in float vQuadM;   // el quad del nodo (m): lo que la malla puede llevar
layout(location = 8) in float vFetch;

// Lo que solo lee el pase de AGUA (el UBO 0 lo comparte con el terreno): x = 1 si las olas que la malla
// no lleva se ven por píxel (`TerrainNodeRenderer::Water::pixelWaves`).
// y = 1: el LÍQUIDO 3D manda en su parche (origen relativo al ojo, ejes, lados zw) y aquí no se dibuja.
layout(std140, binding = 30) uniform WaterDraw { vec4 uWaterDraw; vec4 uLiquidOrigin; vec4 uLiquidAxisX; vec4 uLiquidAxisZ; };

layout(location = 0) out vec4 fragColor;

void main() {
    // Aqui no hay agua: el fondo asoma. Mismo criterio que `ocean.frag` — la orilla la decide la
    // PROFUNDIDAD, no un recorte de geometria, asi que sube y baja con la marea sola.
    // A lake's sheet is not discarded on its shore: the terrain's depth cuts it per pixel (see the
    // vert). Where the interpolated depth says "dry" but the sheet is visible, the ground there is
    // below the lake: shallow water.
    // Only a triangle that is ALL sheet skips the depth rule. One with a dry vertex (the band's end)
    // keeps it: that vertex lies on its own ground, maybe metres below the lake, and with `vSheet <
    // 0.5` half of such a triangle was drawn as a sloping wall from the lake down to it (12-10).
    if (vDepth <= 0.0 && vSheet < 0.999) discard;
    // UN AGUA EN CADA SITIO: dentro del parche del líquido 3D la dibuja él (su física, su superficie).
    if (uWaterDraw.y > 0.5) {
        const vec3 lp = vFragPos - uLiquidOrigin.xyz;
        const float lu = dot(lp, uLiquidAxisX.xyz), lv = dot(lp, uLiquidAxisZ.xyz);
        if (lu > 0.0 && lv > 0.0 && lu < uWaterDraw.z && lv < uWaterDraw.w) discard;
    }
    const float depthShade = (vSheet > 0.5) ? max(vDepth, 0.3) : vDepth;
    // Vistas de depuracion (`HARUKA_TERRAIN_V5_DEBUG`, uMisc.z), las mismas que el terreno: 4 = color
    // por ZANCADA, 1 = por nivel. Existen por los "bloques negros en la linea del mar" (captura,
    // 18-09): si se colorean, son nodos de agua; si no, no es el agua.
    const int dbg = int(uMisc.z);
    if (dbg == 4) { fragColor = vec4(fract(vec3(0.31, 0.57, 0.83) * float(vWStride * 3 + 1)), 1.0); return; }
    if (dbg == 1) { fragColor = vec4(fract(vec3(0.31, 0.57, 0.83) * float(vWLevel + 1)), 1.0); return; }
    const vec3 wp = vFragPos - uCenter.xyz - uCenterLo.xyz;

    // ── EL MARCO DEL PLANO TANGENTE, el MISMO que construye `harukaGerstner` ─────────────────────
    // Asi el rumbo de los trenes (`harukaWaveAt(i).zw`) se lee en las mismas dos tangentes.
    const vec3 up = normalize(wp);
    const vec3 t1 = normalize(abs(up.y) < 0.99 ? cross(up, vec3(0, 1, 0)) : cross(up, vec3(1, 0, 0)));
    const vec3 t2 = cross(up, t1);
    // Posicion en ese plano, medida desde el ANCLA DEL MAR: numeros de km como mucho, no de 6e6 m.
    const vec3 pA = vFragPos - uOceanAnchor.xyz;
    const vec2 uv = vec2(dot(pA, t1), dot(pA, t2));
    const vec2 w0 = harukaWaveAt(0).zw;
    const vec2 windDir = (dot(w0, w0) > 1e-6) ? normalize(w0) : vec2(1.0, 0.0);
    const float t     = harukaOceanTime();
    const float footM = max(length(fwidth(vFragPos)), 1e-4);   // metros por pixel aqui

    // ── LA NORMAL SUAVE: el oleaje de la malla + el rizo de viento por pixel ────────────────────
    // Ver `harukaWindRipple`: el rizo existe en calma tambien, y es lo que hace que el agua se mueva
    // a la vista aunque el clima de 35 cm de ola.
    const vec2 gPix  = harukaWindRipple(uv, windDir, t, footM);
    // Las olas que la malla ya no lleva (más cortas que dos quads), por píxel: solo luz. Sin esto,
    // lejos el mar era un espejo (ver `harukaWaveSlopeResidual`).
    vec3 nGeo = normalize(vNormal);
    // ⚠️ Con el interruptor (`uWaterDraw.x`, `Water::pixelWaves`): sin él la rama de abajo quedaba
    // muerta y apagar las olas por píxel no hacía nada (`NodeWaterFarWaves`).
    if (uWaterDraw.x > 0.5 && vDepth > 0.0) {
        // ANIME, LISO: la normal del oleaje entero POR PÍXEL (ver `harukaWaveSlopePixel`), no la del
        // vértice: con ella el reflejo y la espuma dibujaban los triángulos de la malla. La de la malla
        // solo queda donde la lámina se PLIEGA (el labio: su vientre mira abajo y eso no lo sabe la
        // pendiente de un seno).
        const float footSide = max(min(length(dFdx(vFragPos)), length(dFdy(vFragPos))), 1e-4);
        const vec2 gAll = harukaWaveSlopePixel(pA, t1, t2, t, vDepth, vFetch, vQuadM, footSide, vBed);
        const vec3 nPix = normalize(up - t1 * gAll.x - t2 * gAll.y);
        nGeo = (dot(nGeo, up) > 0.2) ? nPix : nGeo;
    }
    const vec3 nBase = nGeo;
    const vec3 nSmooth = normalize(nBase - t1 * gPix.x - t2 * gPix.y);

    // ── LA FACETA, en una rejilla FIJA DEL MUNDO ────────────────────────────────────────────────
    // ⚠️ AQUI ESTABA EL CORTE DE NODOS. La faceta salia de `harukaFaceNormal` —el triangulo de la
    // malla—, y la malla del agua cambia de densidad de un nodo a otro (zancada por LOD, un vertice
    // cada `waveVertexPx` pixeles): en la arista de dos nodos las caras doblaban de tamano y la luz
    // por cara lo pintaba como una costura. Con la rejilla, la faceta mide `kWaterFacetM` en todos
    // los nodos y a cualquier LOD. El rizo se evalua en el CENTRO de la faceta, asi que cada
    // triangulo tiene una luz y esa luz cambia con el tiempo: facetas que destellan y se mueven.
    // El oleaje de la malla entra con su normal suave (no hay forma de evaluarlo en otro punto sin
    // repetir Gerstner entero por pixel), que es la escala larga; el facetado es del rizo.
    const float kWaterFacetM = 1.5;
    const vec2  cUv   = harukaLatticeCenter(uv, kWaterFacetM);
    const vec2  gFac  = harukaWindRipple(cUv, windDir, t, 0.0);
    const vec3  nFacet = normalize(nBase - t1 * gFac.x - t2 * gFac.y);
    // De lejos la faceta cabe en pocos pixeles y seria ruido: se funde a la suave, igual que el
    // suelo, y ademas por TAMANO EN PANTALLA (menos de ~6 px por faceta, fuera).
    // ⚠️ SIN FACETAS (18-10): Andoni, "quita el low-poly y pon uno tipo más anime". La rejilla queda
    // por si se quiere volver (facet > 0), pero el agua es LISA y el estilo lo ponen el destello
    // nítido y la espuma recortada de `harukaOceanShade`.
    const float facet = 0.0 * harukaTerrainFacet(length(vFragPos))
                      * smoothstep(3.0, 6.0, kWaterFacetM / footM);
    const vec3  nLight = normalize(mix(nSmooth, nFacet, facet));

    // LA LUZ DEL FRAME (lib/sky_light.glsl): el Sol tras el aire, el cielo y el suelo como ambiente y
    // la Luna. Aquí había `vec3(1.0)` y `(0,10, 0,13, 0,18)` fijos.
    const vec3 ambE = harukaSkyAmbient(nSmooth, up)
                    + uSkyMoonGround.rgb * max(dot(nLight, uSkyMoonDir.xyz), 0.0);
    // El cielo que REFLEJA esta faceta: el de la direccion reflejada (horizonte claro, cenit oscuro).
    // Es lo que hace visibles las olas lejos: cada cara refleja un trozo de cielo distinto.
    const vec3 Rv = reflect(normalize(vFragPos), nSmooth);
    // LA ESPUMA, POR PÍXEL. `vFoam` viene del VÉRTICE y se interpola por el triángulo: recortada con
    // borde (el estilo anime de `harukaOceanShade`) dibujaba CADA TRIÁNGULO de la malla en blanco
    // (Andoni, 18-10: "muchos polígonos de espuma, sigue siendo low-poly"). Un ruido del mar, anclado
    // al ancla del oleaje y que se mueve despacio, decide la forma de la mancha; el vértice solo dice
    // cuánta espuma toca aquí. De lejos (manchas de menos de unos píxeles) el ruido se apaga a su media.
    float foamPix = vFoam;
    if (vFoam > 0.0) {
        const float fine = 1.0 - smoothstep(0.4, 1.2, footM / 1.2);
        const float n = 0.6 * harukaPaintValue(vec3(uv / 3.0, t * 0.10), 11u)
                      + 0.4 * harukaPaintValue(vec3(uv / 1.2, t * 0.18), 12u);
        foamPix = clamp(vFoam * mix(1.0, 0.25 + 1.5 * n, fine) - 0.12 * fine, 0.0, 1.0);
    }
    vec4 c = harukaOceanShade(wp, vFragPos, nSmooth, nLight, normalize(uLightDir.xyz),
                              uSkySunGround.rgb, ambE, harukaSkyRadiance(Rv, up), depthShade, foamPix, facet);
    // El aire de en medio y a pantalla.
    c.rgb = harukaAerialSkyLinear(c.rgb, vFragPos);
    fragColor = vec4(harukaDisplay(c.rgb), c.a);
}
