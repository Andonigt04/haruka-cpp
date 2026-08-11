#version 460 core
#extension GL_GOOGLE_include_directive : require
layout(quads, fractional_odd_spacing, ccw) in;
layout(location = 0) in vec3 ePos[]; layout(location = 1) in vec3 eColor[]; layout(location = 2) in vec3 eClimate[];
layout(location = 0) out vec3 vNorm; layout(location = 1) out vec3 vFragPos; layout(location = 2) out vec3 vColor; layout(location = 3) out vec2 vUv; layout(location = 4) out vec3 vClimate;
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
    vec4 uTexAnchor;   // ancla planetaria de las UV de terreno (la usa biome.frag; ver planet.cpp)
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
    // ⚠️ Piso GEMELO de `terrainTriM` (core/planet/terrain_lod.h) = lado real del quad del clipmap.
    float triM  = max(camD * 0.012, 4.0);

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

    // Altura Y GRADIENTE en UNA evaluación (§8). Antes eran TRES —la altura y dos puntos desplazados
    // para la normal por diferencias finitas—, o sea 15 ruidos por vértice para un dato que la
    // función ya conoce. El ruido es trilineal con pesos smoothstep: su derivada es exacta y sale de
    // los mismos 8 hashes. `terrain_detail_gradient` verifica que gradiente y función coinciden.
    float att = harukaSeaLevelAttenuation(baseH);
    vec3  grad = vec3(0.0);
    float h = 0.0;
    if (!inClip) {
        h = harukaTerrainDetailGrad(dir, baseR, triM, grad) * att;
        grad *= att;
        // La tierra nunca baja del nivel del mar: el océano es una esfera en R, y si el detalle
        // hundiera el suelo en tierra, el agua lo taparía. Paridad con planet.cpp sampleHeight.
        // Donde el recorte MUERDE, la superficie es plana → el gradiente también.
        if (baseH > 0.0 && h < -baseH) { h = -baseH; grad = vec3(0.0); }
    }
    vec3  world = dir * (baseR + h);

    // La normal sale del gradiente: las derivadas tangenciales son sus proyecciones sobre el
    // triedro. Además de barata es MÁS FIEL que la diferencia finita, que medía la pendiente media
    // sobre 12 m y aplanaba el relieve que curvara dentro de ese paso.
    vec3 n = normalize(dir);
    if (!inClip) {
        vec3 t1 = normalize(abs(dir.y) < 0.99 ? cross(dir, vec3(0,1,0)) : cross(dir, vec3(1,0,0)));
        vec3 t2 = cross(dir, t1);
        n = normalize(dir - t1 * dot(grad, t1) - t2 * dot(grad, t2));
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
    vClimate = vec3(baseH * 0.001, mix(c01.yz, c32.yz, v));
    // ⚠️ AQUÍ SE EMPUJABA EL VÉRTICE FUERA DEL NDC (`inClip ? vec4(2,2,2,1) : ...`) y era el origen
    // de las franjas que cruzaban la pantalla: expulsar VÉRTICES no descarta TRIÁNGULOS, los estira.
    // El descarte se hace ahora por parche entero en `terrain.tesc`, que es donde se puede hacer bien.
    // `inClip` se conserva para saltarse el DETALLE (el muestreo de ruido caro) donde el clipmap tapa.
    gl_Position = uMVP * vec4(world + uCenter.xyz, 1.0);
}
