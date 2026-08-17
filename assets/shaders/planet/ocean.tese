#version 460 core
#extension GL_GOOGLE_include_directive : require
layout(quads, fractional_odd_spacing, cw) in;
layout(location = 0) in vec2 eLocal[];
layout(location = 0) out vec3 vNorm;     // normal de la superficie ONDULADA
layout(location = 1) out vec3 vFragPos;  // posición relativa a la CÁMARA
layout(location = 2) out float vDepth;   // profundidad del agua bajo el punto (m)
layout(location = 3) out float vFoam;    // 0..1 de rompiente
layout(location = 4) out vec2  vLoc;     // posición en el plano tangente del anillo (m)

layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
    vec4 uTexAnchor;
};
layout(std140, binding = 13) uniform ClipParams {
    vec4 uClipOrigin; vec4 uClipTanU; vec4 uClipTanV; vec4 uClipCover;
};
// El bake de altura: de aquí sale la PROFUNDIDAD, que es quien manda en todo el oleaje.
layout(binding = 16) uniform sampler2D uHeightTex;

#include "lib/terrain_detail.glsl"
#include "lib/ocean_wave.glsl"
#include "lib/inland_water.glsl"

void main() {
    float R = uExtra.w;
    vec2 l01 = mix(eLocal[0], eLocal[1], gl_TessCoord.x);
    vec2 l32 = mix(eLocal[3], eLocal[2], gl_TessCoord.x);
    vec2 loc = mix(l01, l32, gl_TessCoord.y);

    // Del plano tangente a la esfera, igual que el clipmap del terreno: normalizar es lo que curva
    // la rejilla. Sin ello, a 4 km el "mar" se separaría varios metros de la superficie del planeta.
    vec3 dir = normalize(uClipOrigin.xyz + uClipTanU.xyz * loc.x / R + uClipTanV.xyz * loc.y / R);

    // ── NIVEL DEL AGUA: mar o lago, un solo campo ───────────────────────────────────────────────
    // El mar está en la cota 0 (el bake ya viene desplazado). Un lago o un río están donde diga la
    // simulación. `max` de los dos: donde no hay agua interior el centinela es -1e9 y gana el mar;
    // donde hay lago por encima del mar, gana el lago. La misma superficie sirve para los dos.
    float inland = harukaInlandWaterAt(dir * R + uCenter.xyz);
    float level  = max(0.0, inland);
    vec3  wp     = dir * (R + level);         // superficie del agua EN REPOSO

    // PROFUNDIDAD: el mismo bake que dibuja el terreno y que pisa la física. `baseH` negativo = fondo
    // bajo el nivel del mar, o sea agua; positivo = tierra, y ahí no hay mar que dibujar.
    float baseH = harukaSampleHeightField(uHeightTex, textureSize(uHeightTex, 0),
                                          harukaEquirectUV(dir));
    float depth = max(level - baseH, 0.0);    // profundidad = nivel del agua − cota del suelo

    // DESVANECIDO EN EL BORDE DEL CLIPMAP. La ola se apaga antes de que acabe la rejilla, para que el
    // último anillo entregue exactamente la MISMA superficie que la esfera lisa que lo releva: una
    // esfera a cota 0. Sin esto, el relevo sería un escalón de la altura de la ola.
    float rad  = length(loc);
    // ⚠️ Iba de 0.60 a 0.92 del semi-lado: en el anillo 0 (±1 984 m) la ola se apagaba ya a 1,2 km,
    // y por eso "solo se veían muy cerca". Ahora ocupa el último 15 % del anillo, que es lo que hace
    // falta para casar con la esfera lisa sin regalar los 800 m de en medio.
    float fade = 1.0 - smoothstep(uClipCover.x * 0.85, uClipCover.x * 0.98, rad);

    vec3  n;
    float foam;
    vec3  disp = harukaGerstner(wp, dir, uDebug.y, depth, fade, n, foam);
    // Sin agua no hay ola: en tierra el desplazamiento se anula (y el fragmento se descarta luego).
    if (depth <= 0.0) { disp = vec3(0.0); n = dir; foam = 0.0; }

    vec3 posPlanet = wp + disp;
    vFragPos = posPlanet + uCenter.xyz;       // uCenter = centro − cámara ⇒ relativo al ojo
    vNorm    = n;
    vDepth   = depth;
    vFoam    = foam;
    vLoc     = loc;
    gl_Position = uMVP * vec4(vFragPos, 1.0);
}
