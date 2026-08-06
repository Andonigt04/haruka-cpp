#version 460 core
#extension GL_GOOGLE_include_directive : require
layout(quads, fractional_odd_spacing, ccw) in;
in vec3 ePos[]; in vec3 eColor[]; in vec3 eClimate[];
out vec3 vNorm; out vec3 vFragPos; out vec3 vColor; out vec2 vUv; out vec3 vClimate;
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
};
// Campo base horneado (R32F, metros): la altura de la malla ya no viaja en el clima del vértice.
layout(binding = 16) uniform sampler2D uHeightTex;

layout(std140, binding = 13) uniform ClipParams {
    vec4 uClipOrigin; vec4 uClipTanU; vec4 uClipTanV; vec4 uClipCover;
};

// El detalle fino sale de la librería COMPARTIDA con C++ (`terrain_detail.h`). Es lo que hace que
// el suelo que se ve y el que se pisa sean el mismo: los dos evalúan la misma función, con las
// mismas operaciones en el mismo orden.
#include "lib/terrain_detail.glsl"

void main() {
    float u = gl_TessCoord.x, v = gl_TessCoord.y;
    float R = uExtra.w;                       // radio del planeta (m)

    // DIRECCIÓN y ALTURA se interpolan por SEPARADO, no la posición 3D.
    //
    // Interpolar la posición metía la sagita de la cuerda —a 39 km de parche sobre 6371 km de radio
    // son ~30 m de hundimiento en el centro— dentro de la altura del terreno. Eso no es relieve, es
    // un artefacto de la malla base, y además obligaría a la CPU a replicar exactamente la misma
    // cuerda para que el suelo que se pisa coincida con el que se ve. Separándolos, la superficie es
    // `R + bilineal(alturas)` en los dos lados y la CPU puede reproducirla con una bilineal normal.
    vec3 d01 = mix(normalize(ePos[0]), normalize(ePos[1]), u);
    vec3 d32 = mix(normalize(ePos[3]), normalize(ePos[2]), u);
    vec3 dir = normalize(mix(d01, d32, v));

    // ALTURA BASE: la del bake horneado (R32F, binding 16), no la del clima del vértice (39 km).
    // Es el mismo dato que leen el clipmap, el agua y la física, así que los tres suelos nacen de la
    // misma retícula y no pueden divergir. uDebug.z > 0.5 = hay bake; si no, se cae a la antigua
    // interpolación de `elev` (km) para no dejar la malla sin altura base.
    float baseH;
    if (uDebug.z > 0.5) {
        baseH = harukaSampleHeightField(uHeightTex, textureSize(uHeightTex, 0), harukaEquirectUV(dir));
    } else {
        float e01 = mix(eClimate[0].x, eClimate[1].x, u);
        float e32 = mix(eClimate[3].x, eClimate[2].x, u);
        baseH = mix(e01, e32, v) * 1000.0;
    }
    float baseR = R + baseH;

    // Tamaño del triángulo que lleva este vértice, DERIVADO DE LA DISTANCIA, no del factor de
    // teselación del parche.
    //
    // ⚠️ Usar `gl_TessLevelOuter` aquí es un error que se ve: el factor es DISCRETO por parche, así
    // que dos parches vecinos dan pesos de octava distintos, la altura salta en la frontera y sale
    // una REJILLA del tamaño del parche dibujada sobre el terreno. La distancia es continua, así
    // que el peso también, y la costura desaparece. Es el mismo principio que el factor por arista:
    // lo que se comparte entre parches tiene que salir de datos compartidos.
    float camD  = length(dir * (baseR) + uCenter.xyz);
    float triM  = max(camD * 0.012, 2.0);

    // EL CLIPMAP CUBRE 0-2 km: ahí la malla base queda OCULTA por la rejilla fina. Si además se
    // dibuja, sus parches de ~610 m (cuerdas de las octavas, hasta ~14 m por encima de la superficie)
    // se asoman por el sesgo de profundidad del clipmap y salen PINCOS a los pies — y pinta el suelo
    // dos veces (coste de fragmento). Se recorta la base dentro de la MISMA caja que cubre el clipmap
    // (±1984 m en el plano tangente de la cámara, con el MISMO marco tangente que calcula la CPU en
    // planet.cpp). Los vértices recortados se salen del NDC → no se rasterizan, y se salta el detalle
    // (el muestreo de ruido caro) para ellos. La FÍSICA no recorta: sigue consultando el campo fino.
    vec3 rel = dir * baseR + uCenter.xyz;
    vec3 upv = normalize(-uCenter.xyz);
    vec3 tu  = cross(vec3(0, 1, 0), upv);
    if (length(tu) < 1e-6) tu = cross(vec3(1, 0, 0), upv);
    tu = normalize(tu);
    vec3 tv = cross(upv, tu);
    // uDebug.w = el clipmap dibuja este frame (CPU). Solo entonces se recorta: si no hay pipeline de
    // clipmap, no hay bake o la cámara está en órbita, la malla base tiene que dibujarse entera.
    // El semi-lado sale de ClipParams (uClipCover.x), el mismo que usa el clipmap para dibujar.
    float cover = uClipCover.x;
    bool inClip = uDebug.w > 0.5 && abs(dot(rel, tu)) <= cover && abs(dot(rel, tv)) <= cover;

    float h = inClip ? 0.0 : harukaTerrainDetail(dir, baseR, triM) * harukaSeaLevelAttenuation(baseH);
    // La tierra nunca baja del nivel del mar: el océano es una esfera en R, y si el detalle hundiera
    // el suelo en tierra, el agua lo taparía. Paridad con planet.cpp sampleHeight.
    if (baseH > 0.0 && !inClip) h = max(h, -baseH);
    vec3  world = dir * (baseR + h);

    // Normal del detalle por diferencias finitas sobre la MISMA función: pedirle a la GPU los
    // vértices vecinos dentro del eval no es posible sin geometry shader. Se salta para los vértices
    // recortados (no se rasterizan).
    vec3 n = normalize(dir);
    if (!inClip) {
        vec3 t1 = normalize(abs(dir.y) < 0.99 ? cross(dir, vec3(0,1,0)) : cross(dir, vec3(1,0,0)));
        vec3 t2 = cross(dir, t1);
        const float eps = 12.0;                      // metros; por debajo el ruido no cambia
        float att = harukaSeaLevelAttenuation(baseH);
        float hu = harukaTerrainDetail(normalize(dir * baseR + t1 * eps), baseR, triM) * att;
        float hv = harukaTerrainDetail(normalize(dir * baseR + t2 * eps), baseR, triM) * att;
        n = normalize(dir - (t1 * (hu - h) + t2 * (hv - h)) / eps);
    }

    // vFragPos RELATIVO A LA CÁMARA, igual que simple.vert (aPos + uCenter): biome.frag hace
    // `vFragPos - uCenter` para la dirección radial y las UV. Si se pasara el `world` planetario,
    // `up` saldría deformado y los mapas de color girarían con el jugador.
    vNorm = n; vFragPos = world + uCenter.xyz; vUv = vec2(u, v);
    vColor   = mix(mix(eColor[0], eColor[1], u), mix(eColor[3], eColor[2], u), v);
    // vClimate.x = ALTURA base en km, la del bake (igual que clipmap.tese): la selección de material
    // por elevación (nieve/roca en biome.frag) sigue el MISMO suelo que dibuja esta malla.
    vec3 c01 = mix(eClimate[0], eClimate[1], u);
    vec3 c32 = mix(eClimate[3], eClimate[2], u);
    vClimate = vec3(baseH * 0.001, mix(c01.yz, c32.yz, v));
    gl_Position = inClip ? vec4(2.0, 2.0, 2.0, 1.0)
                         : uMVP * vec4(world + uCenter.xyz, 1.0);
}
