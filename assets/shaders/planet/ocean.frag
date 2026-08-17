#version 460 core
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec3 vNorm;
layout(location = 1) in vec3 vFragPos;
layout(location = 2) in float vDepth;
layout(location = 3) in float vFoam;
layout(location = 4) in vec2  vLoc;
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
    vec4 uTexAnchor;
};
layout(std140, binding = 13) uniform ClipParams {
    vec4 uClipOrigin; vec4 uClipTanU; vec4 uClipTanV; vec4 uClipCover;
};
layout(binding = 16) uniform sampler2D uHeightTex;
layout(location = 0) out vec4 fragColor;

#include "lib/terrain_detail.glsl"
#include "lib/ocean_shade.glsl"
#include "lib/inland_water.glsl"

void main() {
    // ── EL HUECO DEL ANILLO, POR PÍXEL ──────────────────────────────────────────────────────────
    //
    // ⚠️ ESTO ES LO QUE EVITA LAS "LÁMINAS CUADRADAS" PEGADAS AL TERRENO. El descarte del TCS es por
    // PARCHE ENTERO, así que un parche a caballo del borde del hueco lo dibujan los DOS anillos. Al
    // terreno le da igual —es opaco y el de delante tapa al de detrás—, pero el agua va con ALFA:
    // cada solape suma una segunda capa de agua sobre la misma escena, y con varios anillos salen
    // cuadrados anidados cada vez más turbios, con los lados rectos del parche.
    //
    // Recortando aquí, con la coordenada del plano tangente del propio anillo, la frontera cae donde
    // dice el ClipParams y no donde acaba un parche de 128 m: los anillos TESELAN el plano sin
    // solaparse ni dejar hueco, y el agua se mezcla exactamente una vez por píxel.
    float hole = uClipCover.w;
    if (hole > 0.0 && max(abs(vLoc.x), abs(vLoc.y)) < hole) discard;

    vec3 wp = vFragPos - uCenter.xyz;
    // LA ORILLA, POR PÍXEL. El vértice trae la profundidad del bake (10,7 km por téxel); aquí se
    // recalcula con el DETALLE procedural —la misma función que dibuja el terreno y que pisa la
    // física— para que el borde del agua siga el suelo real y no la retícula del horneado.
    vec3  dir   = normalize(wp);
    float baseH = harukaSampleHeightField(uHeightTex, textureSize(uHeightTex, 0),
                                          harukaEquirectUV(dir));
    // ⚠️ La profundidad se calcula AQUÍ, no se hereda del vértice: el mar lejano (la esfera) no la
    // trae, y si se heredara su océano profundo saldría con profundidad 0 y se descartaría entero.
    // Nivel del agua en este píxel: el mar (0) o el lago/río que publique la sim. Ver inland_water.
    float level = max(0.0, harukaInlandWaterAt(vFragPos));
    float depth = level - baseH;

    // ── EL DETALLE FINO SOLO DONDE CAMBIA ALGO ──────────────────────────────────────────────────
    //
    // ⚠️ ESTO COSTABA 180 ms. `harukaTerrainDetail` son 5 octavas × 8 hashes (~200 operaciones) y se
    // evaluaba POR PÍXEL en toda la banda |baseH| < 400 m — o sea la plataforma continental entera —
    // sobre una superficie que llena la pantalla, con alfa (sin early-Z) y con sobredibujo.
    //
    // Y no decidía la orilla, que era su justificación. Bajo el agua la amplitud del detalle está
    // atenuada por profundidad (`harukaSeaLevelAttenuation`): su máximo es 173,85·(prof/200) =
    // **0,87 × profundidad**, así que NO PUEDE cruzar el nivel del mar por mucho que se afine. Y en
    // tierra `det = max(det, -baseH)` fuerza `shoreH ≥ 0`. El recorte lo decide `baseH` solo.
    //
    // Lo único que aportaba era afinar el tinte del agua somera, y eso solo se lee en los primeros
    // metros de profundidad y de cerca. Fuera de esa franja son 200 operaciones por píxel para un
    // resultado idéntico.
    if (baseH > -60.0 && baseH < 60.0 && length(vFragPos) < 3000.0) {
        float baseR = uExtra.w + baseH;
        float triM  = max(length(vFragPos) * 0.012, 4.0);   // gemelo de `terrainTriM`
        float det   = harukaTerrainDetail(dir, baseR, triM) * harukaSeaLevelAttenuation(baseH);
        if (baseH > 0.0) det = max(det, -baseH);
        depth = level - (baseH + det);
    }
    if (depth <= 0.0) discard;              // el fondo asoma: aquí no hay mar

    fragColor = harukaOceanShade(wp, vFragPos, normalize(vNorm), normalize(uLightDir.xyz),
                                 uLightColor.xyz, uAmbient.xyz, depth, vFoam);
}
