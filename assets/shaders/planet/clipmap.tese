#version 460 core
#extension GL_GOOGLE_include_directive : require
// ⚠️ `equal_spacing`, NO `fractional_odd_spacing`, y el motivo es la paridad con la colisión.
//
// El espaciado fraccionario reparte el parche en tramos DESIGUALES —redondea el nivel al impar
// siguiente y encoge los dos tramos de los extremos— y además el nivel es una función CONTINUA de la
// distancia, así que los vértices se deslizan al caminar. Resultado: los vértices del render no caen
// en la retícula de 4 m y no dejan de moverse, mientras la malla de colisión es una retícula fija
// anclada al mundo. No son dos aproximaciones de la misma superficie con distinto error: son dos
// superficies muestreadas en puntos distintos, y una se mueve. Ninguna cantidad de afinado las junta.
//
// Con `equal_spacing` y el nivel en potencias de dos (clipmap.tesc), el vértice cae en `128/n` exacto
// —4, 8, 16 m…— así que el conjunto del render es siempre un subconjunto de la retícula de colisión.
//
// Lo que se pierde es la transición continua entre niveles: al subir de nivel aparecen vértices de
// golpe y la superficie salta de la cuerda al valor real. MEDIDO sobre esta función de terreno, ese
// salto son 6-11 cm de mundo y **0,07-0,09 px** a la distancia donde ocurre (el nivel está topado a 32
// por debajo de 1 km, así que solo cambia más allá). La fórmula del nivel mantiene la celda a un
// tamaño angular constante y la sagita escala con la celda al cuadrado: en pantalla el salto se
// encoge justo donde el nivel cambia. Si algún día el terreno se vuelve mucho más abrupto, la salida
// es geomorphing con el factor atado a la distancia AL JUGADOR (no a la cámara, o el servidor del DGS
// no podría reproducir la superficie).
layout(quads, equal_spacing, ccw) in;
layout(location = 0) in vec2 eLocal[];
layout(location = 0) out vec3 vNorm; layout(location = 1) out vec3 vFragPos; layout(location = 2) out vec3 vColor; layout(location = 3) out vec2 vUv; layout(location = 4) out vec3 vClimate;
layout(location = 5) out float vSurfKind;   // 0 = no es la malla base (ver terrain.tese)
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
    vec4 uTexAnchor;   // ancla planetaria de las UV de terreno (la usa biome.frag; ver planet.cpp)
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
    vSurfKind = 0.0;
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
    // ⚠️ GEMELO de `terrainTriM` (core/planet/terrain_lod.h). El piso NO es 2.0: es el lado REAL del
    // quad del clipmap (parche 128 m / tope 32 de clipmap.tesc = 4 m). Con el piso en 2.0 la octava
    // de 4,5 m entraba al 12,5 % sobre vértices separados 4 m — sub-Nyquist, el hervido de §3.1.
    float rad   = length(loc);
    float clipM = max(rad * 0.002, 4.0);
    float meshM = max(length(dir * baseR + uCenter.xyz) * 0.012, 4.0);
    // Anillo de mezcla dinámico (70-95 % del semi-lado): escala con la cobertura del clipmap.
    float blendStart_ = uClipCover.y;
    float blendEnd   = uClipCover.z;
    float blend = smoothstep(blendStart_, blendEnd, rad);
    // (Ya no se mezcla el `triM`: lo que se mezcla son las ALTURAS y su GRADIENTE, abajo. Mezclar
    // el triM era justo lo que dejaba el codo de ±1,2 m que describe el comentario de arriba.)
    // Altura Y GRADIENTE juntos (§8): una evaluación fuera de la banda de mezcla, dos dentro. Antes
    // eran 3 y 6 — la altura más dos puntos desplazados por cada lado para la normal. El gradiente
    // se mezcla con el MISMO peso que las alturas, así que sigue describiendo la superficie que de
    // verdad se dibuja en la transición.
    float h;
    vec3  grad, gA, gB;
    if (rad <= blendStart_) {
        h = harukaTerrainDetailGrad(dir, baseR, clipM, grad) * att;  grad *= att;
    } else if (rad >= blendEnd) {
        h = harukaTerrainDetailGrad(dir, baseR, meshM, grad) * att;  grad *= att;
    } else {
        float hA = harukaTerrainDetailGrad(dir, baseR, clipM, gA) * att;
        float hB = harukaTerrainDetailGrad(dir, baseR, meshM, gB) * att;
        h = mix(hA, hB, blend);
        grad = mix(gA * att, gB * att, blend);
    }
    // La tierra no baja del nivel del mar (paridad con terrain.tese y sampleHeight): el agua es una
    // esfera en R y solo rellena océanos. Donde el recorte muerde, la superficie es plana.
    if (baseH > 0.0 && h < -baseH) { h = -baseH; grad = vec3(0.0); }
    vec3  world = dir * (baseR + h);

    vec3 t1 = normalize(abs(dir.y) < 0.99 ? cross(dir, vec3(0,1,0)) : cross(dir, vec3(1,0,0)));
    vec3 t2 = cross(dir, t1);
    // La normal sale del gradiente ya calculado con la altura: sus proyecciones sobre el triedro.
    //
    // ⚠️ Esto RESUELVE de raíz lo que el `eps` mezclado (3 → 12 m) intentaba parchear. Aquel truco
    // existía porque la rejilla y la malla base sombreaban con pasos de diferencia finita distintos,
    // y entonces la rejilla se veía como una CAPA aparte flotando sobre el terreno (los "dos
    // terrenos" al subir). Con el gradiente no hay paso que igualar: las dos superficies derivan la
    // MISMA función con el MISMO triM en el borde, así que se iluminan igual por construcción.
    vNorm = normalize(dir - t1 * dot(grad, t1) - t2 * dot(grad, t2));

    // vFragPos relativo a la CÁMARA (igual que simple.vert): biome.frag resta uCenter para la
    // radial y las UV. Pasarlo planetario deformaba `up` y los colores giraban con el jugador.
    vFragPos = world + uCenter.xyz; vUv = loc * 0.01; vColor = vec3(1.0);
    // ⚠️ ELEVACIÓN DE LA BASE, sin el detalle — y esto es deliberado.
    //
    // `biome.frag` decide con esto la arena de ORILLA (banda −30 m … +12 m) y la roca por altura, o
    // sea DÓNDE ESTÁ LA COSTA. Y la costa la define el mapa base, no el ruido procedural: el detalle
    // es relieve local, no estructura de continente.
    //
    // Sumarle el detalle (lo intenté) rompe justo aquí: el recorte del nivel del mar pinza `h` a
    // `-baseH` donde el detalle hundiría el suelo bajo el mar, así que `baseH + h` sale EXACTAMENTE 0
    // — el centro de la banda de arena. Resultado: arena a pleno brillo en cada zona recortada, y
    // solo DENTRO del clipmap, porque ahí el triM de 4 m da amplitud suficiente para que el recorte
    // muerda mientras la malla base (triM = camD·0.012) casi nunca llega.
    vClimate = vec3(baseH * 0.001, fld.y, fld.z);
    gl_Position = uMVP * vec4(world + uCenter.xyz, 1.0);
}
