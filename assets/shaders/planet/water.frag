#version 460 core
#extension GL_GOOGLE_include_directive : require
in vec3 vNorm; in vec3 vFragPos; in vec3 vColor; in vec2 vUv;
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
};
// Campo base del terreno (elev del nivel del mar, 6 capas): lo usa la costa per-pixel.
layout(binding = 15) uniform sampler2DArray uBaseField;
// Campo base horneado (R32F): la costa lee de aquí la MISMA altura que pinta la malla.
layout(binding = 16) uniform sampler2D uHeightTex;
out vec4 fragColor;

// La costa per-pixel evalúa el MISMO suelo que pinta la malla: descomposición de cara compartida
// (gemela de dirToCubeFaceClosed en cube_sphere.cpp) y función de detalle compartida con C++.
#include "lib/cube_face.glsl"
#include "lib/terrain_detail.glsl"
// Cuerpos masivos que atraen/mueven el mar (mismo bloque que water.vert; uTide.x = 0 ⇒ neutro).
#include "lib/tidal.glsl"

// Bilineal a MANO del campo base, idéntica a clipmap.tese (misma retícula, mismo peso: si la
// orilla del agua y la costa de la malla difirieran en centímetros se vería un escalón).
vec3 sampleBase(vec3 dir) {
    int f; vec2 uv;
    harukaDirToCubeFace(dir, f, uv);
    int N = textureSize(uBaseField, 0).x - 1;
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

// ── OLEAJE ─────────────────────────────────────────────────────────────────────────────────
// Rizo fino + oleaje grande (olas que dan vida): una marea que viaja despacio por la esfera
// (gravedad de una "luna") abulta el swell; sobre él corren olas de longitud de onda enorme.
// Perturban la NORMAL, no la geometría: las olas son luz, no relieve — el océano es una esfera
// fija y no merece una malla de altura solo para esto.
//
// Dos guardas contra artefactos:
//  1. La normal de las olas se construye en el marco TANGENTE de la superficie (up = radial del
//     planeta), no con el "up" de mundo. Con up de mundo, la perturbación no coincidía con la
//     esfera salvo en los polos y la onda se deformaba en ESPIRALES GIGANTES que giraban con el
//     oleaje — los "círculos grandes que aparecen y desaparecen" que se veían desde lejos.
//  2. Antialiasing por longitud de onda: una ola solo se dibuja si el píxel es más pequeño que su
//     longitud de onda. Si no, la fase se muestrea en puntos dispersos y el seno se convierte en
//     muaré que brilla al moverse (mismo criterio que las octavas del terreno).
float waveVisibility(float lambdaM, float pxM) {
    return clamp(1.0 - pxM / lambdaM, 0.0, 1.0);
}

vec3 waterNormal(vec3 wp, float t) {
    vec2 p = wp.xz * 0.001;                        // metros → km
    float dist = length(vFragPos);                 // distancia a la cámara (camera-relative)
    float pxM  = dist * 0.0007;                    // ≈ tamaño de píxel en metros
    // Rizo fino (escala de cientos de m)
    vec2 k1 = vec2(0.47, 0.83), k2 = vec2(-0.62, 0.35);
    float f1 = dot(p, k1) * 0.8 + t * 0.9;
    float f2 = dot(p, k2) * 1.6 - t * 1.3;
    float w1 = waveVisibility(8200.0, pxM);        // λ ≈ 8,2 km
    float w2 = waveVisibility(5500.0, pxM);        // λ ≈ 5,5 km
    // Marea de "luna": un abultamiento que rueda alrededor del planeta y hace crecer el oleaje
    float moonAz = t * 0.018;
    vec2  kmoon  = vec2(cos(moonAz), sin(moonAz));
    float tide   = 0.5 + 0.5 * sin(dot(p, kmoon) * 0.28 - t * 0.05);
    float swell  = 0.40 + 0.45 * tide;             // olas grandes donde pasa la marea
    // Swell oceánico: olas de longitud de onda enorme y muy lentas
    vec2 k3 = vec2(0.085, 0.11), k4 = vec2(-0.045, 0.03);
    float f3 = dot(p, k3) - t * 0.28;
    float f4 = dot(p, k4) + t * 0.16;
    float w3 = waveVisibility(45000.0, pxM);       // λ ≈ 45 km
    float w4 = waveVisibility(116000.0, pxM);      // λ ≈ 116 km
    float h  = (sin(f1) * w1 + sin(f2) * w2) * 0.20
             + (sin(f3) * w3 + sin(f4) * w4) * swell;
    // Gradiente analítico de la suma, con los mismos pesos de visibilidad (si el peso apaga la
    // ola, su gradiente también tiene que apagarse o la normal seguiría "hirviendo").
    vec2 g  = k1 * 0.8 * 0.20 * cos(f1) * w1 + k2 * 1.6 * 0.20 * cos(f2) * w2
            + k3 * swell * cos(f3) * w3 + k4 * swell * cos(f4) * w4;
    // Normal en el marco TANGENTE de la esfera: se proyecta el gradiente (que vive en el plano XZ
    // de la parametrización) sobre el plano tangente a la superficie y se perturba la RADIAL.
    vec3 radial = normalize(wp);
    vec3 g3     = vec3(g.x, 0.0, g.y);
    vec3 gtan   = g3 - dot(g3, radial) * radial;
    return normalize(radial - gtan);
}

void main() {
    vec3 n = normalize(vNorm);
    vec3 wp = vFragPos - uCenter.xyz;
    // LIMPIA EL MAR BAJO TIERRA. Donde el bake dice que hay tierra (baseH > 0,5 m) la superficie del
    // terreno está DELANTE del océano y este fragmento lo habría ocultado el depth test de todos
    // modos: descartarlo ahorra la mezcla del agua bajo el continente (y cualquier asomo por
    // grietas). Es el recorte CORRECTO frente al que intentaba water.vert —empujar el vértice fuera
    // del NDC estiraba los triángulos que compartían un vértice de costa con uno recortado en bandas
    // hacia la esquina del clip (las "láminas de azul")— así que water.vert se revirtió a una
    // transformación limpia y el recorte vive aquí, por píxel. Umbral 0,5 m: solo tierra clara.
    if (uDebug.z > 0.5 && texture(uHeightTex, harukaEquirectUV(normalize(wp))).r > 0.5) discard;
    float t = uDebug.y;
    // Mezcla la normal de la esfera con la de las olas: sin mezcla, la onda dominaría el borde
    // del terminador; con 0.5 la superficie ondula pero la esfera sigue siendo legible. Cerca de un
    // cuerpo masivo, sobre cada CRESTA del tren de anillos (`tidalRings`) la mezcla sube → las olas
    // se agitan y el movimiento se lee (la marea geométrica sola no se percibe).
    float rings = tidalRings(wp, t);
    n = normalize(mix(n, waterNormal(wp, t), 0.5 + 0.30 * clamp(rings, 0.0, 1.0)));

    vec3 L = normalize(uLightDir.xyz);
    vec3 V = normalize(-vFragPos);          // vFragPos es relativo a la cámara
    vec3 H = normalize(L + V);
    float diff  = max(dot(n, L), 0.0);
    float fres  = pow(1.0 - max(dot(n, V), 0.0), 3.0);
    float spec  = pow(max(dot(n, H), 0.0), 96.0);
    vec3 deep    = vec3(0.01, 0.10, 0.32);   // fondo del océano
    vec3 shallow = vec3(0.03, 0.34, 0.72);   // superficie iluminada
    // Luz propia de cielo: el ambiente del UBO es casi nulo (espacio exterior), y sin más el océano
    // es una lámina negra fuera del parche del sol. Este término no depende de uAmbient — es la
    // dispersión Rayleigh azulada, más fuerte en el horizonte (fresnel).
    vec3 sky   = vec3(0.09, 0.15, 0.24);
    vec3 amb   = uAmbient.xyz + sky * (0.55 + 0.45 * fres);
    float sunD = clamp(diff * 1.6, 0.0, 1.0);
    vec3 base = mix(deep, shallow, sunD);
    // MAR DINÁMICO POR MASA: donde el bulto de marea de los cuerpos masivos levanta/hunde el agua,
    // la superficie queda más tensa y "atrapa" mejor el sol → un destello tenue que sigue al pozo
    // mientras éste barre el océano. Neutro (0) sin cuerpos.
    float tideW = tidalHeight(wp);
    float tideGlint = clamp(abs(tideW) * 0.5 + rings * 0.5, 0.0, 1.0);
    vec3 col = base * (amb + uLightColor.xyz * sunD * 1.1)
             + uLightColor.xyz * spec * (0.45 + 0.55 * fres) * (1.6 + 2.4 * tideGlint)
             + uLightColor.xyz * spec * 0.6 * tideGlint;

    // COSTA PER-PIXEL: el borde del agua lo recorta la superficie del TERRENO, y desde órbita esa
    // superficie sale poligonal (la teselación cae a LOD 1 y la malla base de 256 manda). Aquí el
    // agua computa la altura REAL del suelo en cada píxel —bake horneado + detalle compartido, el
    // mismo que pinta la malla— y dibuja la orilla con un degradado somero y una línea de espuma
    // en el cruce exacto. El detalle solo se evalúa cerca de la costa (|baseH| < 400 m); en mar
    // abierto son 4 texelFetch + un exp, casi gratis.
    vec3  wdir   = normalize(wp);
    // La altura REAL del suelo: el bake horneado (binding 16), el MISMO que leen la malla, el clipmap
    // y la física. Sin él (uDebug.z ≤ 0.5) se cae al campo de 39 km, la retícula antigua.
    // Aquí NO hace falta la bilineal manual: la paridad bit a bit solo importa donde se PINTA el
    // terreno (teselación y clipmap) y donde se PISA (física). El agua es un efecto de fragmento, y
    // el bilineal de hardware hace 1 fetch donde la manual hace 4 — la costura de longitud ±π no se
    // ve en mar abierto.
    float baseH  = uDebug.z > 0.5
                 ? texture(uHeightTex, harukaEquirectUV(wdir)).r
                 : sampleBase(wdir).x;
    float shoreH = baseH;
    if (baseH > -400.0 && baseH < 400.0) {
        float baseR = uExtra.w + baseH;
        float triM  = max(length(wdir * baseR + uCenter.xyz) * 0.012, 2.0);
        float det   = harukaTerrainDetail(wdir, baseR, triM) * harukaSeaLevelAttenuation(baseH);
        if (baseH > 0.0) det = max(det, -baseH);   // la tierra no baja del nivel del mar (paridad)
        shoreH = baseH + det;
    }
    float shallowW = smoothstep(-35.0, -2.0, shoreH);        // agua somera junto a la costa
    float foamW    = exp(-abs(shoreH) * 0.45);               // espuma justo en el cruce (H≈0)
    col = mix(col, vec3(0.14, 0.55, 0.72) * (amb + uLightColor.xyz * sunD * 1.1), shallowW * 0.55);
    col = mix(col, vec3(0.90, 0.94, 0.97) * (amb + uLightColor.xyz * sunD), foamW * 0.40);
    // ESPUMA DE MAREA: sobre las crestas del tren de anillos de los cuerpos masivos se pinta una
    // línea blanca que las sigue al propagarse — la estela espumosa de una "ola real". Se apaga
    // con la distancia al pozo (ya está en `rings`). Neutro (0) sin cuerpos.
    col = mix(col, vec3(0.90, 0.94, 0.97) * (amb + uLightColor.xyz * sunD),
              clamp(rings, 0.0, 1.0) * 0.55);

    fragColor = vec4(col, 0.85);
}
