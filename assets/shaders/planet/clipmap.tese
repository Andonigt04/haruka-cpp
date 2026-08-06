#version 460 core
#extension GL_GOOGLE_include_directive : require
layout(quads, fractional_odd_spacing, ccw) in;
in vec2 eLocal[];
out vec3 vNorm; out vec3 vFragPos; out vec3 vColor; out vec2 vUv; out vec3 vClimate;
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
};
layout(std140, binding = 13) uniform ClipParams {
    vec4 uClipOrigin; vec4 uClipTanU; vec4 uClipTanV; vec4 uClipCover;
};
// La retícula base del planeta, 6 capas (una por cara del cubo): (elev m, tempC, humedad).
layout(binding = 15) uniform sampler2DArray uBaseField;
// Campo base horneado (R32F, metros): la ELEVACIÓN ya no sale de la retícula de 39 km sino del bake.
layout(binding = 16) uniform sampler2D uHeightTex;

#include "lib/terrain_detail.glsl"
// Descomposición dirección → cara + (lx,ly). Tiene que ser EXACTAMENTE la inversa de la proyección
// con la que se construyó la malla; ver la nota larga del propio fichero.
#include "lib/cube_face.glsl"

// Bilineal A MANO con texelFetch, no con el filtrado del hardware: el bilineal de GL usa pesos de
// precisión limitada (8 bits en varias GPU) y eso bastaría para que el suelo del clipmap y el de la
// malla difieran en centímetros. Aquí se replica exactamente la bilineal de la CPU.
vec3 sampleBase(vec3 dir) {
    int f; vec2 uv;
    harukaDirToCubeFace(dir, f, uv);
    int N = textureSize(uBaseField, 0).x - 1;      // lado de la retícula = faceRes
    vec2 fxy = (uv * 0.5 + 0.5) * float(N);
    ivec2 i0 = ivec2(floor(fxy));
    i0 = clamp(i0, ivec2(0), ivec2(N - 1));
    vec2 t = fxy - vec2(i0);
    vec3 h00 = texelFetch(uBaseField, ivec3(i0 + ivec2(0,0), f), 0).rgb;
    vec3 h10 = texelFetch(uBaseField, ivec3(i0 + ivec2(1,0), f), 0).rgb;
    vec3 h01 = texelFetch(uBaseField, ivec3(i0 + ivec2(0,1), f), 0).rgb;
    vec3 h11 = texelFetch(uBaseField, ivec3(i0 + ivec2(1,1), f), 0).rgb;
    vec3 a = h00 + (h10 - h00) * t.x;
    vec3 b = h01 + (h11 - h01) * t.x;
    return a + (b - a) * t.y;
}

void main() {
    float R = uExtra.w;
    vec2 l01 = mix(eLocal[0], eLocal[1], gl_TessCoord.x);
    vec2 l32 = mix(eLocal[3], eLocal[2], gl_TessCoord.x);
    vec2 loc = mix(l01, l32, gl_TessCoord.y);

    // Del plano tangente a la esfera. Normalizar es lo que curva la rejilla: sin ello sería un
    // plano y a 4 km ya se separaría del planeta varios metros.
    vec3 dir = normalize(uClipOrigin.xyz + uClipTanU.xyz * loc.x / R + uClipTanV.xyz * loc.y / R);

    // La ELEVACIÓN sale del bake (binding 16), la MISMA retícula que leen la malla, el agua y la
    // física — el clipmap y la malla son coplanares por construcción. La retícula de 39 km (15) solo
    // sigue aportando temp/humedad para la selección de material.
    vec3  fld   = sampleBase(dir);
    float baseH = uDebug.z > 0.5
                ? harukaSampleHeightField(uHeightTex, textureSize(uHeightTex, 0), harukaEquirectUV(dir))
                : fld.x;
    float baseR = R + baseH;
    float att   = harukaSeaLevelAttenuation(baseH);

    // Tamaño de triángulo con el que se atenúan las octavas (Nyquist). Dos valores y una mezcla:
    //
    //   clipM  lo que de verdad mide un triángulo de la rejilla aquí → todo el detalle fino.
    //   meshM  lo que usa el eval de la MALLA a esta misma distancia (MISMA fórmula que terrain.tese:
    //          camD·0.012 — antes 0.006, y el borde de la rejilla y la malla usaban octavas distintas).
    //
    // La mezcla es lo que COSE las dos superficies. En el borde exterior de la rejilla el peso es 1,
    // así que allí el clipmap evalúa exactamente las mismas octavas que la malla y las dos alturas
    // coinciden: no hay escalón donde acaba la rejilla. Hacia dentro pasa a `clipM` y aparecen las
    // octavas de 22 m y 4,5 m, que es el relieve que se camina.
    //
    // Por DISTANCIA y no por el factor de teselación del parche: el factor es discreto por parche,
    // y usarlo dibujaba una rejilla de 128 m sobre el terreno (el salto del peso en la frontera).
    //
    // Se mezclan las ALTURAS, no el tamaño de triángulo. `harukaOctaveWeight` es por piezas (una
    // octava entra al 100 % hasta su umbral de Nyquist y se apaga justo después), así que mezclar
    // `triM` deja un CODO de ±1,2 m en el anillo 1400-1900 m — la "rampa" que se ve desde el suelo.
    // Mezclando los dos desniveles (el fino y el que usará la malla fuera) el relieve se desvanece
    // LINEALMENTE y el suelo no rompe en el borde. Fuera del anillo solo se evalúa un lado.
    float rad   = length(loc);
    float clipM = max(rad * 0.002, 2.0);
    float meshM = max(length(dir * baseR + uCenter.xyz) * 0.012, 2.0);
    // Anillo de mezcla dinámico (70-95 % del semi-lado): escala con la cobertura del clipmap.
    float blendStart_ = uClipCover.y;
    float blendEnd   = uClipCover.z;
    float blend = smoothstep(blendStart_, blendEnd, rad);
    float triM  = mix(clipM, meshM, blend);
    float h;
    if (rad <= blendStart_) {
        h = harukaTerrainDetail(dir, baseR, clipM) * att;
    } else if (rad >= blendEnd) {
        h = harukaTerrainDetail(dir, baseR, meshM) * att;
    } else {
        h = mix(harukaTerrainDetail(dir, baseR, clipM) * att,
                harukaTerrainDetail(dir, baseR, meshM) * att, blend);
    }
    // La tierra no baja del nivel del mar (paridad con terrain.tese y sampleHeight): el agua es una
    // esfera en R y solo rellena océanos.
    if (baseH > 0.0) h = max(h, -baseH);
    vec3  world = dir * (baseR + h);

    vec3 t1 = normalize(abs(dir.y) < 0.99 ? cross(dir, vec3(0,1,0)) : cross(dir, vec3(1,0,0)));
    vec3 t2 = cross(dir, t1);
    // EPS de la normal con la MISMA mezcla que las alturas: cerca del jugador la rejilla tiene
    // pasos de 2 m (eps=3 captura su relieve fino); en el borde exterior la rejilla tiene que
    // sombrearse IGUAL que la malla base, que usa eps=12 — si no, las dos superficies se iluminan
    // distinto y la rejilla se ve como una CAPA aparte flotando sobre el terreno (los "dos
    // terrenos" al subir: el fino que sigue al jugador y el gordo que se revela alrededor).
    float eps = mix(3.0, 12.0, blend);
    float hu = harukaTerrainDetail(normalize(dir * baseR + t1 * eps), baseR, triM) * att;
    float hv = harukaTerrainDetail(normalize(dir * baseR + t2 * eps), baseR, triM) * att;
    vNorm = normalize(dir - (t1 * (hu - h) + t2 * (hv - h)) / eps);

    // vFragPos relativo a la CÁMARA (igual que simple.vert): biome.frag resta uCenter para la
    // radial y las UV. Pasarlo planetario deformaba `up` y los colores giraban con el jugador.
    vFragPos = world + uCenter.xyz; vUv = loc * 0.01; vColor = vec3(1.0);
    vClimate = vec3(baseH * 0.001, fld.y, fld.z);
    gl_Position = uMVP * vec4(world + uCenter.xyz, 1.0);
}
