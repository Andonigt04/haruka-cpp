/**
 * @file water.frag
 * @brief Planetary ocean shading — Fase 3.
 *
 * Procedural sky reflection (no cubemap dependency: the engine's IBL is not
 * populated) + Fresnel + sun specular + Gerstner-Jacobian foam.
 *
 * u_waterQuality (settings hook, default 1):
 *   0 = sun specular only (cheapest)
 *   1 = + procedural sky reflection (default)
 *   2 = reserved for screen-space reflections (future)
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in float WaveHeight;
layout(location = 3) in float Foam;
layout(location = 4) in float IsLake;   // 1 = lago (agua dulce), 0 = océano
layout(location = 5) in float Depth;    // profundidad del agua (m): orilla≈0
layout(location = 6) in vec3  WorldDir; // radial (planeta→frag), preciso — para la costa per-píxel

// Parámetros del PASE. MISMO bloque que water.vert (binding 8) — el 7 lo usa el SSBO por-draw.
// (F1) El bloque ADELGAZÓ: fuera seed / continentFreqA / seaThreshold / invRadius / coastWidth /
// pxCoast / oceanSurface. Existían solo para re-derivar el terreno per-píxel; ya no hace falta.
layout(std140, binding = 8) uniform WaterParams {
    vec4  u_waveUpTime;      // xyz = radial up en el punto de superficie · w = tiempo (s)
    vec4  u_waveTanWind;     // xyz = tangente (base global por frame)    · w = fuerza del viento
    vec4  u_waveBitTide;     // xyz = bitangente                          · w = nivel de marea (m)
    vec4  u_relCamNear;      // xyz = cameraPos − centro del planeta · w = plano cercano
    vec2  u_windDir;         // dirección viento/corriente en el plano tangente (unitaria)
    float u_waterQuality;    // >=0.5 → reflejo de cielo (si no, solo especular)
    float u_fieldRes;        // lado de cara del cube-sphere del CAMPO (0 = sin campo)
};

// CAMPO EROSIONADO del planeta: el MISMO que usa el generador de terreno (una sola fuente de verdad).
// El agua lo necesita para saber DÓNDE HAY AGUA: sin esto, la lámina del océano existe también BAJO
// la tierra y la ves en cuanto cavas ("mar bajo el terreno").
layout(std430, binding = 9) readonly buffer WaterFields { vec4 wFieldCells[]; };
// xyz: ANCLA la espuma per-píxel al MUNDO (si el ruido se evaluara sobre FragPos —relativo a la
// cámara— la espuma "nadaría" al moverse). NO es la fórmula del terreno: solo un origen estable.
#define u_relCam u_relCamNear.xyz
#define u_near   u_relCamNear.w

// PROFUNDIDAD DE LA ESCENA (copia del depth del target de escena). Con reversed-Z basta esto para
// saber cuánta agua atraviesa el rayo → adiós al port del ruido del terreno.
layout(binding = 6) uniform sampler2D u_sceneDepth;

layout(std140, binding = 0) uniform PerFrameData {
    mat4  view;
    mat4  projection;
    vec3  _cameraPos;   float _pad0;
    vec3  sunDirection; float _pad1;
    vec3  sunLightColor; float ambientStrength;
    int   enableHDR;
    int   _enableBloom; int _enableSSAO; int _enableIBL; int _enableShadows;
    int   _pad3a; int _pad3b; int _pad3c;
    vec3  moonDirection;  float moonIntensity;   // 2ª luz (luna)
    vec3  moonLightColor; float _pad4;
};

// (F1) Aquí vivían ~150 líneas de Perlin/fBm portadas A MANO desde terrain_gen.comp (`oceanElevKm`,
//      `distToCoastW`) para re-derivar el terreno per-píxel y saber dónde estaba la costa.
//      BORRADAS: con reversed-Z el depth buffer da la profundidad real del agua y el terreno
//      ocluye solo → se acabó mantener la MISMA fórmula en tres sitios (era la fuente del bug
//      del agua inundando la tierra). Ver docs/guides/PLAN_TERRENO_V3.md §1.1.

const vec3 DEEP_COLOR    = vec3(0.015, 0.07, 0.14);
const vec3 SHALLOW_COLOR = vec3(0.05, 0.28, 0.40);
// Lago (agua dulce): más turquesa/verde y menos profundo que el océano.
const vec3 LAKE_DEEP     = vec3(0.04, 0.16, 0.18);
const vec3 LAKE_SHALLOW  = vec3(0.12, 0.40, 0.42);

// --- Ruido de valor 3D barato, para la ESPUMA per-píxel -------------------------------------
float fHash(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float fNoise(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(fHash(i + vec3(0,0,0)), fHash(i + vec3(1,0,0)), f.x),
                   mix(fHash(i + vec3(0,1,0)), fHash(i + vec3(1,1,0)), f.x), f.y),
               mix(mix(fHash(i + vec3(0,0,1)), fHash(i + vec3(1,0,1)), f.x),
                   mix(fHash(i + vec3(0,1,1)), fHash(i + vec3(1,1,1)), f.x), f.y), f.z);
}
float foamNoise(vec3 wp, float t) {   // 2 octavas, arrastradas por el tiempo
    return fNoise(wp * 0.45 + vec3(0.0, t * 0.35, 0.0)) * 0.65
         + fNoise(wp * 1.30 - vec3(0.0, t * 0.55, 0.0)) * 0.35;
}

// --- Cube-sphere: dirección -> (cara,u,v). PORT del mismo mapeo que planet_fields.cpp ---------
void wDirToFaceUV(vec3 d, out int face, out float u, out float v) {
    float ax = abs(d.x), ay = abs(d.y), az = abs(d.z);
    if (ax >= ay && ax >= az) {
        if (d.x > 0.0) { face = 0; u = -d.z / ax; v = -d.y / ax; }
        else           { face = 1; u =  d.z / ax; v = -d.y / ax; }
    } else if (ay >= az) {
        if (d.y > 0.0) { face = 2; u =  d.x / ay; v =  d.z / ay; }
        else           { face = 3; u =  d.x / ay; v = -d.z / ay; }
    } else {
        if (d.z > 0.0) { face = 4; u =  d.x / az; v = -d.y / az; }
        else           { face = 5; u = -d.x / az; v = -d.y / az; }
    }
}
vec3 wSampleField(vec3 dirIn) {   // -> (elev km, caudal, lago km)
    int R = int(u_fieldRes);
    if (R <= 0) return vec3(-1.0, 0.0, 0.0);   // sin campo: asumimos mar (comportamiento previo)
    vec3 d = normalize(dirIn);
    int f; float u, v;
    wDirToFaceUV(d, f, u, v);
    float fx = clamp((u * 0.5 + 0.5) * float(R) - 0.5, 0.0, float(R) - 1.001);
    float fy = clamp((v * 0.5 + 0.5) * float(R) - 0.5, 0.0, float(R) - 1.001);
    int i0 = int(fx), j0 = int(fy);
    int i1 = min(i0 + 1, R - 1), j1 = min(j0 + 1, R - 1);
    float tx = fx - float(i0), ty = fy - float(j0);
    int base = f * R * R;
    vec4 e0 = mix(wFieldCells[base + j0 * R + i0], wFieldCells[base + j0 * R + i1], tx);
    vec4 e1 = mix(wFieldCells[base + j1 * R + i0], wFieldCells[base + j1 * R + i1], tx);
    return mix(e0, e1, ty).xyz;
}

// --- OLAS CORTAS PER-PÍXEL --------------------------------------------------------------------
// Las olas de 7-23 m NO caben en la rejilla del océano (192²): si se meten en la geometría, lo que
// ves son las FACETAS de la malla, no olas. Así que la malla se queda con el oleaje LARGO (la mar de
// fondo, que sí resuelve y da la silueta del horizonte) y las cortas se evalúan AQUÍ, perturbando la
// normal por píxel. Mismo principio que la espuma: geometría per-vértice, sombreado per-píxel.
const int   SW_N = 3;
const float SW_LEN[SW_N]   = float[](23.0, 13.0, 7.0);
const float SW_AMP[SW_N]   = float[](0.25, 0.13, 0.08);
const float SW_ANG[SW_N]   = float[](1.1, -1.3,  0.9);
vec3 shortWaveNormal(vec3 camRelPos, vec3 N) {
    vec3 T = normalize(u_waveTanWind.xyz);
    vec3 B = normalize(u_waveBitTide.xyz);
    vec2 horiz = vec2(dot(camRelPos, T), dot(camRelPos, B));
    float baseAng = atan(u_windDir.y, u_windDir.x);
    float windAmp = clamp(u_waveTanWind.w, 0.05, 4.0);
    float t = u_waveUpTime.w;
    // La normal per-píxel RECOGE lo que la geometría suelta: donde la malla ya no resuelve la ola,
    // la ola sigue existiendo en la ILUMINACIÓN. Por eso llega mucho más lejos que el desplazamiento
    // (una normal no aliasa como una faceta). Solo se apaga cuando la ola es sub-píxel de verdad.
    float fade = 1.0 - smoothstep(2000.0, 9000.0, length(camRelPos));
    if (fade < 0.01) return N;
    vec2 slope = vec2(0.0);
    for (int i = 0; i < SW_N; ++i) {
        float ang = baseAng + SW_ANG[i];
        vec2  D   = vec2(cos(ang), sin(ang));
        float k   = 6.28318530718 / SW_LEN[i];
        float A   = SW_AMP[i] * windAmp * fade;
        float w   = sqrt(9.81 * k) * 0.55;
        float phase = k * dot(D, horiz) - w * t;
        slope += D * (A * k * cos(phase));       // ∂altura/∂(dirección D)
    }
    // Inclinamos la normal con la pendiente de las olas cortas (en el plano tangente del mar).
    return normalize(N - (T * slope.x + B * slope.y));
}

// Cheap analytic sky: horizon haze → zenith blue, plus a soft sun halo.
vec3 skyColor(vec3 dir, vec3 sunDir) {
    float up   = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3  zen  = vec3(0.20, 0.42, 0.82);            // cenit azul profundo
    vec3  hor  = vec3(0.68, 0.80, 0.94);            // horizonte pálido
    vec3  base = mix(hor, zen, pow(up, 0.55));
    // Reflejo del sol en DOS lóbulos: HALO atmosférico ancho (calidez alrededor del sol) + DISCO
    // estrecho brillante → el cielo reflejado se ve atmosférico, no un degradado plano.
    float sd   = max(dot(dir, sunDir), 0.0);
    float halo = pow(sd, 8.0) * 0.35;
    float disk = pow(sd, 220.0);
    base = mix(base, vec3(1.00, 0.94, 0.82), halo); // tinte cálido cerca del sol
    return base + sunLightColor * disk * 0.8;
}

void main() {
    // --- PROFUNDIDAD DEL AGUA LEÍDA DEL DEPTH BUFFER (F1) --------------------------------------
    // ANTES: el agua RE-DERIVABA el terreno en cada píxel (`oceanElevKm`, un port a mano de la
    // fórmula del generador, ~150 líneas de Perlin) solo para saber dónde acababa la costa. Hacía
    // falta porque el z-buffer no tenía precisión para ocluir el mar (todo el terreno caía entre
    // 0.99996 y 1.0). Eso obligaba a mantener la MISMA fórmula en tres sitios a la vez.
    //
    // AHORA (reversed-Z): el terreno opaco ocluye el agua ÉL SOLO por depth test. Aquí ya solo hace
    // falta saber CUÁNTA agua atraviesa el rayo, y eso lo dice el propio depth buffer:
    //     proyección reversed-Z de far infinito  ⇒  z_ndc = near / dist  ⇒  dist = near / z_ndc
    // (z_ndc = 0 ⇒ no se escribió nada ahí = cielo/infinito.)
    vec2  uv        = gl_FragCoord.xy / vec2(textureSize(u_sceneDepth, 0));
    float zScene    = texture(u_sceneDepth, uv).r;
    float distScene = (zScene > 0.0) ? (u_near / zScene) : 1e12;   // el FONDO bajo este píxel
    float distWater = u_near / gl_FragCoord.z;                     // la propia superficie del agua

    // Espesor de agua ATRAVESADA por el rayo de visión. Es lo correcto también físicamente: la
    // absorción depende del camino que recorre la luz, no de la profundidad vertical.
    float shoreDepthM = max(distScene - distWater, 0.0);

    // Orilla: el agua se FUNDE con el lecho en los últimos decímetros → borde anti-aliasado, sin
    // el escalón/halftone del discard duro. Donde el terreno está DELANTE del agua, el depth test
    // ya descartó el fragmento → aquí no hace falta ningún discard.
    float shoreAlpha = smoothstep(0.0, 0.6, shoreDepthM);

    // --- ¿HAY AGUA AQUÍ? (F3) -----------------------------------------------------------------
    // El océano es una lámina a nivel del mar en TODO el planeta: también existe BAJO la tierra, y
    // se ve en cuanto cavas. Preguntamos al CAMPO (el mismo que genera el terreno): si aquí el
    // terreno procedural está sobre el mar y no hay lago, NO hay agua → fuera el fragmento.
    vec3  wpDir = normalize(FragPos + u_relCam);      // dirección desde el centro del planeta
    vec3  fld   = wSampleField(wpDir);
    bool  lake  = (fld.z > 0.0 && fld.z > fld.x);
    if (fld.x > 0.0 && !lake) discard;                // tierra firme: aquí no hay mar

    vec3 N = normalize(Normal);
    // Detalle de oleaje CORTO: per-píxel, no en la malla (si no, ves los triángulos de la rejilla).
    N = shortWaveNormal(FragPos, N);
    vec3 V = normalize(-FragPos);   // camera at origin in cam-relative space
    vec3 L = normalize(sunDirection);
    vec3 H = normalize(L + V);

    float ndv = max(dot(N, V), 0.0);

    // Fresnel (Schlick) — more reflective at grazing angles.
    float fresnel = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);

    // Body colour: deeper looking straight down, lighter at grazing angle.
    // Lago vs océano: paleta de agua dulce (turquesa) si IsLake.
    vec3 deep    = mix(DEEP_COLOR,    LAKE_DEEP,    IsLake);
    vec3 shallow = mix(SHALLOW_COLOR, LAKE_SHALLOW, IsLake);
    // Color por PROFUNDIDAD real: turquesa en el bajío → azul oscuro en lo hondo.
    // (+ un poco de aclarado por ángulo rasante para el brillo del horizonte.)
    float shallowF = exp(-shoreDepthM * 0.10);      // 1 en la orilla, →0 a ~20-30 m
    vec3 body = mix(deep, shallow, max(shallowF, ndv * 0.35));

    vec3 color = body;

    // Reflection (quality-gated).
    if (u_waterQuality >= 0.5) {
        vec3 R = reflect(-V, N);
        vec3 sky = skyColor(R, L);
        color = mix(body, sky, fresnel);
    } else {
        // Low: just a flat sky tint via fresnel.
        color = mix(body, vec3(0.45, 0.62, 0.85), fresnel);
    }

    // Diffuse lift so the night side isn't pure black.
    float ndl = max(dot(N, L), 0.0);
    color += sunLightColor * body * ndl * 0.25;

    // Luz de luna: aporte difuso azulado + un brillo especular suave (reflejo lunar
    // en el agua) → el mar nocturno no queda negro y tiene un destello plateado.
    vec3  ML       = normalize(moonDirection);
    float ndlMoon  = max(dot(N, ML), 0.0);
    vec3  HM       = normalize(ML + V);
    float specMoon = pow(max(dot(N, HM), 0.0), 300.0);
    color += moonLightColor * moonIntensity * (body * ndlMoon + vec3(specMoon) * 0.3);

    // Sun specular glint.
    // Sun specular glint — más cerrado y tenue (el brillo anterior era demasiado
    // drástico/irreal): reflejo de sol puntual, no un fogonazo en todo el mar.
    // Glint del sol ACOTADO: a distancia la normal se aplana a radial → el glint se ensancha a una
    // región grande; con sunLightColor HDR reventaba a BLANCO + bloom (mancha). Bajado 0.45→0.18 y
    // CLAMP a 0.6 → destello visible sin fogonazo blanco.
    float spec = pow(max(dot(N, H), 0.0), 400.0);
    color += min(sunLightColor * spec * 0.18, vec3(0.6));
    // Lóbulo ANCHO (sheen): además del glint puntual, un brillo suave y extenso da al reflejo del sol
    // una caída natural (sparkle + sheen) en vez de un único punto duro. Gateado por fresnel (borde).
    float specSheen = pow(max(dot(N, H), 0.0), 60.0);
    color += min(sunLightColor * specSheen * 0.05, vec3(0.15)) * fresnel;

    // SUBSURFACE SCATTERING: la luz atraviesa la cresta de la ola y sale hacia el ojo → brillo
    // turquesa TRANSLÚCIDO, intenso a CONTRALUZ (sol tras la ola) y en crestas altas (agua fina).
    // Es lo que separa un mar "vivo" de un espejo plano con espuma. Solo ALU → barato, sin texturas.
    vec3  sssTint  = mix(vec3(0.05, 0.34, 0.30), vec3(0.10, 0.46, 0.40), IsLake); // teal océano/lago
    float backLit  = max(dot(-V, L), 0.0);                        // cámara mira hacia el sol a través del agua
    float crestLit = clamp(WaveHeight * 0.5 + 0.30, 0.0, 1.0);    // más en crestas (agua fina)
    float sss      = pow(backLit, 3.0) * crestLit * (0.35 + 0.65 * fresnel);
    color += sssTint * sunLightColor * sss * 0.7;

    color += ambientStrength * body;

    // --- ESPUMA PER-PÍXEL (F1) ---------------------------------------------------------------
    // ANTES: la espuma de cresta se calculaba PER-VÉRTICE (Jacobiano de Gerstner) sobre la rejilla
    // del océano (192²) y se interpolaba en triángulos ENORMES → manchas/"triángulos" blancos sin
    // relación con el oleaje. Era una señal de ALTA frecuencia metida en un canal de BAJA frecuencia;
    // el fade por distancia solo lo TAPABA. (Ver PLAN_TERRENO_V3 §0: la geometría va per-vértice, el
    // sombreado va per-píxel.)
    // AHORA: lo per-vértice aporta solo la MÁSCARA (dónde hay cresta / cuán somero es), y el DETALLE
    // lo pone un ruido per-píxel anclado al mundo → la espuma se rompe en grumos, no en triángulos.
    vec3  wp    = FragPos + u_relCam;                 // posición MUNDO estable (no nada con la cámara)
    float fN    = foamNoise(wp, u_waveUpTime.w);      // 0..1

    // Orilla: ahora shoreDepthM es la profundidad REAL (del depth buffer) → banda de espuma exacta.
    float shoreMask = 1.0 - smoothstep(0.0, 2.2, shoreDepthM);
    float shoreFoam = shoreMask * smoothstep(0.30, 0.85, fN * (0.75 + 0.35 * shoreMask));

    // Cresta: máscara per-vértice (Jacobiano + altura) × ruido per-píxel. El fade por distancia se
    // mantiene, pero ya no es un parche: es LOD honesto (las olas cortas no se resuelven lejos).
    float waveFoamFade = 1.0 - smoothstep(400.0, 2000.0, length(FragPos));
    float crestMask = clamp(max(Foam, smoothstep(0.9, 2.0, WaveHeight)), 0.0, 1.0)
                    * (1.0 - 0.9 * IsLake) * waveFoamFade;
    float crestFoam = smoothstep(0.42, 0.90, crestMask * (0.55 + 0.85 * fN));

    float foam = clamp(max(crestFoam, shoreFoam), 0.0, 1.0);
    color = mix(color, vec3(0.85, 0.92, 0.97), foam * 0.75);

    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    // El AGUA es el medio TRANSLÚCIDO (no la cámara): se ve el LECHO A TRAVÉS, teñido por el color
    // del agua (alpha-blend sobre el terreno ya dibujado). Confinado a CERCA + SOMERO:
    //  · nearF: solo a < ~150 m (donde estás nadando/en la orilla); a distancia OPACA para que el
    //    lecho grueso NO asome en halftone (el bug de los círculos marrones era translucidez lejana).
    //  · shallow: aguas someras claras (ves la arena); lo HONDO opaco (azul denso, realista).
    // Así "ves a través del agua" donde importa, sin ensuciar el mar lejano.
    float camDistW = length(FragPos);
    float nearF    = 1.0 - smoothstep(150.0, 800.0, camDistW);   // 1 muy cerca → 0 lejos
    float shallowW = 1.0 - smoothstep(0.0, 12.0, shoreDepthM);    // 1 somero → 0 hondo
    float minA     = mix(1.0, 0.55, nearF * shallowW);           // ves el fondo, pero el agua sigue leyéndose como MAR (no barro)
    float alpha    = mix(minA, 1.0, max(fresnel, foam)) * shoreAlpha;
    FragColor = vec4(color, alpha);
}
