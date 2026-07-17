/**
 * @file planet.frag
 * @brief Fragment shader for planetary terrain chunks.
 *
 * Checkerboard pattern from per-face UVs. Both Procedural and Manual modes
 * use the same visual for now (different tint to distinguish them).
 * u_terrainMode: 0 = Procedural, 1 = Manual
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec2 TexCoord;
// El modo lo fija el VERTEX shader (draw normal: uniform loc 11; batched/MultiDraw: SSBO por
// gl_DrawID). Así el frag es idéntico en ambos caminos sin depender de gl_DrawID (no disponible
// aquí). #define para no tocar cada uso de u_terrainMode del cuerpo.
layout(location = 3) flat in int vMode;
#define u_terrainMode vMode

// Parámetros del PASE de terreno (antes uniforms sueltos: fog, sombras, u_lightSpace, u_hasTex).
// glUniform no existe en Vulkan → van en un UBO. Los FLAGS son float a propósito: el empaquetado
// std140 de int/bool difiere entre GL y Vulkan.
// Binding 8, NO 7: el 7 lo usa el SSBO del DrawItem. En GL son espacios de binding distintos y no
// chocarían, pero en un descriptor set de Vulkan sí → no dejamos la mina puesta.
layout(std140, binding = 8) uniform TerrainParams {
    mat4  u_lightSpace;   // sombras del sol (matriz luz)
    float u_fogEnabled;   // >0.5 = niebla on (consola: fog)
    float u_shadowsOn;    // >0.5 = hay shadow map bindeado
    float u_hasTex;       // >0.5 = texturas de bioma disponibles (si no, color procedural)
    float u_fieldRes;     // (F5) lado de cara del cube-sphere del campo/clima (0 = sin campo)
};

// (F5) EL CAMPO DEL PLANETA — el MISMO que generó el relieve y que usa el agua.
//  · binding 9  = (elevKm, caudal, cotaLago, orogenia)
//  · binding 10 = (tempC, humedad, -, -)
// El bioma se decide con ESTO, no con la altura: así no puede contradecir al terreno (un desierto
// a sotavento de la cordillera sale porque el campo dice que ahí no llueve, no porque esté a tal cota).
layout(std430, binding = 9)  readonly buffer PlanetField   { vec4 pFieldCells[]; };
layout(std430, binding = 10) readonly buffer PlanetClimate { vec4 pClimateCells[]; };

void pDirToFaceUV(vec3 d, out int face, out float u, out float v) {
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) {
        if (d.x > 0.0) { face = 0; u = -d.z / a.x; v = -d.y / a.x; }
        else           { face = 1; u =  d.z / a.x; v = -d.y / a.x; }
    } else if (a.y >= a.z) {
        if (d.y > 0.0) { face = 2; u =  d.x / a.y; v =  d.z / a.y; }
        else           { face = 3; u =  d.x / a.y; v = -d.z / a.y; }
    } else {
        if (d.z > 0.0) { face = 4; u =  d.x / a.z; v = -d.y / a.z; }
        else           { face = 5; u = -d.x / a.z; v = -d.y / a.z; }
    }
}
// Devuelve (caudal, tempC, humedad). Bilineal, igual que el sampler de CPU → mismo bioma en ambos.
vec3 pSampleClimate(vec3 dir) {
    int R = int(u_fieldRes);
    if (R <= 0) return vec3(0.0, 15.0, 0.5);      // sin campo: templado y medio húmedo (respaldo)
    vec3 d = normalize(dir);
    int f; float u, v;
    pDirToFaceUV(d, f, u, v);
    float fx = clamp((u * 0.5 + 0.5) * float(R) - 0.5, 0.0, float(R) - 1.001);
    float fy = clamp((v * 0.5 + 0.5) * float(R) - 0.5, 0.0, float(R) - 1.001);
    int i0 = int(fx), j0 = int(fy);
    int i1 = min(i0 + 1, R - 1), j1 = min(j0 + 1, R - 1);
    float tx = fx - float(i0), ty = fy - float(j0);
    int base = f * R * R;
    vec4 fl0 = mix(pFieldCells[base + j0 * R + i0], pFieldCells[base + j0 * R + i1], tx);
    vec4 fl1 = mix(pFieldCells[base + j1 * R + i0], pFieldCells[base + j1 * R + i1], tx);
    vec4 cl0 = mix(pClimateCells[base + j0 * R + i0], pClimateCells[base + j0 * R + i1], tx);
    vec4 cl1 = mix(pClimateCells[base + j1 * R + i0], pClimateCells[base + j1 * R + i1], tx);
    return vec3(mix(fl0, fl1, ty).y, mix(cl0, cl1, ty).x, mix(cl0, cl1, ty).y);
}

// Texturas de bioma (Fase 4A). Sin ellas (u_hasTex=0) → color procedural (fallback).
// El binding = la unidad de textura que ata el RHI (bindTexture) → sin glUniform1i de sampler.
layout(binding = 0) uniform sampler2D u_sandAlbedo;
layout(binding = 1) uniform sampler2D u_sandNormal;
layout(binding = 2) uniform sampler2D u_grassAlbedo;
layout(binding = 3) uniform sampler2D u_landAlbedo;
layout(binding = 4) uniform sampler2D u_landNormal;
layout(binding = 5) uniform sampler2D u_shadowMap;

// Only the fields actually used from the per-frame UBO.
// Must preserve std140 offsets: view(0), projection(64),
// cameraPos+pad(128), sunDirection+pad(144), sunLightColor+ambientStrength(160), enableHDR(176).
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

// Per-object UBO (binding 1). Para el terreno: u_planetRelCam.xyz = (cameraPos −
// planetCenter) en double→float (preciso) → relPos del fragmento = FragPos + eso.
// u_baseColorAndRadius.a = radio del planeta (m). Permite altura/pendiente/clima.
layout(std140, binding = 1) uniform PerObjectData {
    mat4 u_model;
    vec4 u_baseColorAndRadius;
    vec4 u_planetRelCam;
};

// --- Detalle fino 1–2 m como NORMAL MAP por píxel (Fase 4) ---
// El relieve de < ~8 m NO va en la geometría (aliasaría las normales del vértice
// = pinchos); aquí vive POR PÍXEL en la iluminación → detalle sin pinchos.
// DETAIL_STRENGTH: súbelo/bájalo si el relieve fino se ve flojo/exagerado.
const float DETAIL_STRENGTH = 0.10; // relieve fino por píxel. BAJADO de 0.22: a 0.22 la normal se
                                    // perturbaba tanto que de noche daba MANCHAS NEGRAS (normal lejos
                                    // de la Luna) y "desnivel disparado". 0.10 = definición sutil sin
                                    // eso. Gateado por distancia (transición ancha → sin anillo).

float hash1(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Value noise 3D con GRADIENTE analítico (iq). Devuelve (valor, ∂/∂x,∂/∂y,∂/∂z).
vec4 noised(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    vec3 u  = f * f * (3.0 - 2.0 * f);
    vec3 du = 6.0 * f * (1.0 - f);
    float a = hash1(i + vec3(0,0,0));
    float b = hash1(i + vec3(1,0,0));
    float c = hash1(i + vec3(0,1,0));
    float d = hash1(i + vec3(1,1,0));
    float e = hash1(i + vec3(0,0,1));
    float f1= hash1(i + vec3(1,0,1));
    float g = hash1(i + vec3(0,1,1));
    float h = hash1(i + vec3(1,1,1));
    float k0=a, k1=b-a, k2=c-a, k3=e-a, k4=a-b-c+d, k5=a-c-e+g, k6=a-b-e+f1,
          k7=-a+b+c-d+e-f1-g+h;
    vec3 grad = du * vec3(
        k1 + k4*u.y + k6*u.z + k7*u.y*u.z,
        k2 + k5*u.z + k4*u.x + k7*u.z*u.x,
        k3 + k6*u.x + k5*u.y + k7*u.x*u.y);
    float val = k0 + k1*u.x + k2*u.y + k3*u.z
              + k4*u.x*u.y + k5*u.y*u.z + k6*u.z*u.x + k7*u.x*u.y*u.z;
    return vec4(val, grad);
}

// Perturba la normal con relieve fino (2 octavas ~2 m y ~0.8 m). Mismo marco
// que el damero (FragPos en metros) → escala real, sin geometría.
vec3 applyDetailNormal(vec3 N, vec3 wp, float camDist) {
    // Gate por DISTANCIA: relieve fino solo CERCA. Lejos las celdas de ~1-2 m son sub-píxel →
    // aliasarían (shimmer) — por eso estaba OFF; con el gate se puede tener sin ese problema.
    // wp = posición MUNDO (relativa al centro del planeta) → estable al moverse (no "nada").
    // camDist se pasa aparte (length(FragPos)) para el gate de distancia.
    float nearW = 1.0 - smoothstep(40.0, 600.0, camDist); // transición ANCHA → sin anillo visible
    if (nearW < 0.01) return N;
    vec3 g = noised(wp * 0.5).yzw            // celdas de ~2 m
           + noised(wp * 1.25).yzw * 0.5;    // celdas de ~0.8 m (media amplitud)
    vec3 gTan = g - dot(g, N) * N;           // solo la parte tangencial inclina la normal
    return normalize(N - DETAIL_STRENGTH * nearW * gTan);
}

// --- Material de bioma (Etapa 4A) — paleta dieselpunk sombría (TERROSA) ---
// Para variante FRÍA: tira los verdes a grises/azules ceniza y sube la nieve.
const vec3 BIOME_SAND  = vec3(0.44, 0.39, 0.27); // costa / tierra batida
const vec3 BIOME_GRASS = vec3(0.26, 0.34, 0.16); // hierba (verde tierra, no gris)
const vec3 BIOME_DRY   = vec3(0.40, 0.35, 0.21); // hierba seca / tierra parda
const vec3 BIOME_ROCK  = vec3(0.33, 0.29, 0.25); // roca parda
const vec3 BIOME_SNOW  = vec3(0.74, 0.74, 0.71); // nieve sucia (no blanco puro)

// Paleta por BIOMA (Whittaker). No son "colores por altura": cada uno es un ecosistema.
const vec3 BIOME_DESERT = vec3(0.62, 0.53, 0.35); // desierto cálido (arena/gravilla)
const vec3 BIOME_STEPPE = vec3(0.47, 0.44, 0.26); // estepa / sabana seca
const vec3 BIOME_FOREST = vec3(0.18, 0.30, 0.13); // bosque templado
const vec3 BIOME_JUNGLE = vec3(0.13, 0.31, 0.11); // selva (cálido + muy húmedo)
const vec3 BIOME_TAIGA  = vec3(0.20, 0.28, 0.21); // conífera fría
const vec3 BIOME_TUNDRA = vec3(0.42, 0.42, 0.34); // tundra (musgo/roca)
const vec3 BIOME_RIPAR  = vec3(0.22, 0.38, 0.15); // ribera: verde intenso junto al cauce

// BIOMA = f(temperatura, humedad, pendiente, caudal) — la tabla de Whittaker.
//
// Esto SUSTITUYE al color por bandas de altura. La diferencia no es estética: la altura no sabe si
// llueve, así que producía el MISMO paisaje a ambos lados de una cordillera. Ahora el desierto sale
// donde el aire llega seco (sombra orográfica), el bosque donde llueve, y la ribera acompaña al río
// → **el río se ve porque la vegetación lo delata**, que es como se lee un paisaje de verdad.
vec3 biomeColor(float tempC, float humidity, float slope, float flow, float altitude, vec3 wp, float camDist) {
    // Ruido de borde: rompe la frontera limpia entre biomas (las transiciones reales son sucias).
    float n = (noised(wp * 0.004).x - 0.5);
    float T = tempC    + n * 6.0;      // ±3 °C
    float H = clamp(humidity + n * 0.12, 0.0, 1.0);

    // Eje húmedo: desierto → estepa → bosque → selva (la selva solo si además hace calor).
    vec3 c = BIOME_DESERT;
    c = mix(c, BIOME_STEPPE, smoothstep(0.18, 0.38, H));
    c = mix(c, BIOME_FOREST, smoothstep(0.40, 0.60, H));
    c = mix(c, BIOME_JUNGLE, smoothstep(0.72, 0.92, H) * smoothstep(18.0, 24.0, T));

    // Eje frío: conífera y, más al norte/arriba, tundra. La temperatura YA lleva dentro la altitud
    // (−6.5 °C/km en el campo) → la montaña se enfría sola, sin una regla aparte por cota.
    c = mix(c, BIOME_TAIGA,  smoothstep(8.0, 2.0, T) * smoothstep(0.25, 0.5, H));
    c = mix(c, BIOME_TUNDRA, smoothstep(2.0, -6.0, T));

    // RIBERA: el cauce y su entorno verdean aunque el bioma sea seco (el agua está AHÍ).
    c = mix(c, BIOME_RIPAR, smoothstep(0.30, 0.55, flow) * (1.0 - smoothstep(0.35, 0.6, slope)));

    // ROCA por PENDIENTE: donde supera el ángulo de reposo no se sostiene el suelo (lo dice la
    // erosión térmica del campo) → pared desnuda, en cualquier bioma.
    c = mix(c, BIOME_ROCK, smoothstep(0.55, 0.80, slope));

    // NIEVE por temperatura REAL, y menos en pendiente fuerte (no se acumula en la pared).
    float snow = smoothstep(-1.0, -8.0, T) * (1.0 - smoothstep(0.55, 0.85, slope));
    c = mix(c, BIOME_SNOW, snow);

    float v = noised(wp * 0.4).x * (1.0 - smoothstep(2500.0, 9000.0, camDist));
    return c * (0.94 + 0.10 * v);
}

// (F7) MEZCLA POR ALTURA (height-based blending) — el estándar del sector para splatmapping.
//
// Un `mix(a, b, t)` lineal FUNDE los dos materiales: a mitad de camino no ves ni tierra ni hierba,
// ves el promedio (una papilla parda). Lo que hace la naturaleza es otra cosa: la hierba ocupa
// primero los HUECOS y la tierra sigue asomando entre las matas. Eso se consigue comparando la
// ALTURA de cada capa y quedándose con la que sobresale:
//
//   peso_i = altura_i + t_i ;  gana la mayor, con una banda estrecha de transición.
//
// LA ALTURA SALE DE LA PROPIA TEXTURA (su luminancia), no de un ruido.
//
// ⚠️ Usar ruido aquí era un error de precisión, y se veía: la altura se muestreaba con `relPos`
// (posición relativa al CENTRO DEL PLANETA, ~6.4e6 m) reconstruida en float32 → a esa magnitud el
// float solo resuelve ~0.5 m, así que un ruido de frecuencia 0.35 salía CUANTIZADO = sal y pimienta
// por todo el terreno. Además era innecesario: los guijarros de la tierra y las matas de la hierba
// YA están en el albedo — sus zonas oscuras son los huecos y las claras lo que sobresale.
vec3 heightBlend(vec3 colA, vec3 colB, float t) {
    float hA = dot(colA, vec3(0.299, 0.587, 0.114));  // "relieve" de la tierra (sus guijarros)
    float hB = dot(colB, vec3(0.299, 0.587, 0.114));  // el de la hierba (sus matas)
    const float kBand = 0.16;                     // ancho de la transición (0 = corte duro)
    float wA = hA * (1.0 - t);
    float wB = hB * t;
    float m  = max(wA, wB) - kBand;
    wA = max(wA - m, 0.0);
    wB = max(wB - m, 0.0);
    return (colA * wA + colB * wB) / max(wA + wB, 1e-5);
}

// Triplanar: muestrea una textura tileable proyectando desde los 3 ejes del
// mundo y mezclando por la normal → sin estiramiento ni costuras en la esfera.
// scale = 1/metros_por_tile. (wp = FragPos; nota: aún relativo a cámara → "swim"
// al moverse; se cambiará por una UV estable del generador.)
vec3 triplanarAlbedo(sampler2D tex, vec3 wp, vec3 n, float scale) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= (bw.x + bw.y + bw.z + 1e-5);
    vec3 cx = texture(tex, wp.zy * scale).rgb;
    vec3 cy = texture(tex, wp.xz * scale).rgb;
    vec3 cz = texture(tex, wp.xy * scale).rgb;
    return cx * bw.x + cy * bw.y + cz * bw.z;
}

void main() {
    // Posición del fragmento RELATIVA al centro del planeta (precisa) → altura,
    // dirección radial (clima/pendiente). u_planetRelCam.xyz = cameraPos−planetCenter.
    vec3  relPos   = FragPos + u_planetRelCam.xyz;
    float radius   = u_baseColorAndRadius.a;
    float altitude = length(relPos) - radius;             // m sobre el nivel del mar
    vec3  up       = normalize(relPos);                    // radial (planeta sin rotación)
    vec3  Ngeo     = normalize(Normal);                    // normal geométrica (para pendiente)
    float slope    = clamp(1.0 - dot(Ngeo, up), 0.0, 1.0); // 0 llano … 1 vertical

    // (F5) CLIMA REAL, del campo del planeta: temperatura (latitud + altitud) y humedad (traída por
    // el viento desde el mar, descargada al subir las montañas → sombra orográfica). Antes esto era
    // `1 - |latitud|`, o sea: el mismo paisaje a los dos lados de una cordillera.
    vec3  clim     = pSampleClimate(up);
    float flow     = clim.x;       // caudal normalizado [0,1] (>0.45 = río)
    float tempC    = clim.y;       // °C
    float humidity = clim.z;       // [0,1]

    // Factor de ORILLA per-PÍXEL: arena en la costa por ALTITUD (suave, continua) + ruido que
    // ROMPE la línea → costa ONDULADA. Antes la arena se gateaba con TexCoord.x (shoreFactor
    // POR-VÉRTICE): a baja resolución (chunk grueso) se interpolaba RECTO entre vértices →
    // borde poligonal "que parte el terreno". Por píxel + ruido = orilla natural a cualquier LOD.
    float aShore  = altitude + (noised(FragPos * 0.004).x - 0.5) * 55.0; // ±~27 m de ruido (bajado de 90 → menos moteado arena/hierba)
    float shoreF  = 1.0 - smoothstep(0.0, 22.0, aShore);                 // banda de arena MÁS FINA (35→22 m): menos "manchas" en llanos costeros

    float bodyProfile = u_planetRelCam.w;  // 0 terran · 1 luna · 2 gas
    vec3 baseColor;
    if (bodyProfile > 1.5) {
        // GAS (Júpiter): bandas horizontales por latitud, tonos cálidos.
        float n    = noised(FragPos * 0.0015).x;
        float band = sin(up.y * 16.0 + n * 4.0) * 0.5 + 0.5;
        baseColor  = mix(vec3(0.60, 0.46, 0.34), vec3(0.86, 0.77, 0.61), band);
    } else if (bodyProfile > 0.5) {
        // LUNA: regolito gris oscuro (albedo realista ~0.12-0.3).
        float g   = 0.30 + 0.05 * noised(FragPos * 0.02).x;
        g        *= mix(0.78, 1.06, slope);
        baseColor = vec3(g, g, g * 1.03);
    } else if (u_terrainMode == 1) {
        baseColor = vec3(0.30, 0.30, 0.38);                // modo manual: tinte neutro
    } else if (u_hasTex > 0.5) {
        // ⚠️ El bioma manda el COLOR; la textura, solo el GRANO. Antes este camino IGNORABA el bioma
        // Whittaker: mezclaba tierra/hierba/roca/nieve genéricas, así que un desierto (sin hierba, sin
        // roca, sin nieve) quedaba como la textura de tierra pura = GRIS. Todo el trabajo de biomas
        // (desierto dorado, estepa, taiga, selva, ribera) era invisible en cuanto había texturas.
        // biomeColor() ya resuelve el bioma COMPLETO (incluidas roca por pendiente y nieve por temp),
        // así que se usa como color y la textura solo aporta variación de brillo de cerca.
        vec3 biome = biomeColor(tempC, humidity, slope, flow, altitude, relPos, length(FragPos));
        vec3 tex   = triplanarAlbedo(u_landAlbedo, FragPos, Ngeo, 0.5);   // grano de superficie
        float grain = dot(tex, vec3(0.333)) / 0.45;                       // ~1.0 (brillo de la textura)
        float near  = 1.0 - smoothstep(2500.0, 9000.0, length(FragPos));  // detalle solo cerca
        // Arena de orilla: única textura que SÍ pinta color propio (la playa no es "bioma").
        float sandW = shoreF * (1.0 - smoothstep(0.45, 0.7, slope));
        vec3 col    = biome * mix(1.0, clamp(grain, 0.75, 1.25), near);
        baseColor   = mix(col, triplanarAlbedo(u_sandAlbedo, FragPos, Ngeo, 0.7), sandW);
    } else {
        baseColor = biomeColor(tempC, humidity, slope, flow, altitude, relPos, length(FragPos)); // por CLIMA
        float sandW = shoreF * (1.0 - smoothstep(0.45, 0.7, slope));
        baseColor = mix(baseColor, BIOME_SAND, sandW);
    }

    // Normal del vértice (analítica, sin pinchos) + detalle fino 1–2 m por píxel.
    vec3 N = applyDetailNormal(Ngeo, relPos, length(FragPos));
    vec3 L = normalize(sunDirection);
    vec3 V = normalize(-FragPos);
    vec3 H = normalize(L + V);

    float ndl  = max(dot(N, L), 0.0);
    // SIN ESPECULAR: el terreno es MATE. El `pow(dot(N,H),32)*0.12` daba un borde brillante que no
    // existe en roca/tierra/hierba y delataba las facetas de la malla. (Lo pidió el usuario.)

    // Sombra del sol (PCF 3×3) — los props proyectan en u_shadowMap.
    float shadow = 0.0;
    if (u_shadowsOn > 0.5) {
        vec4 lp = u_lightSpace * vec4(FragPos, 1.0);
        // Con glClipControl(ZERO_TO_ONE) la Z de clip YA sale en [0,1] (solo x/y van en [-1,1]).
        // Antes se remapeaba xyz*0.5+0.5 asumiendo [-1,1]: eso ahora hundiría la z y el terreno
        // saldría todo sombreado. El shadow map NO es reversed-Z (usa ortho estándar).
        vec3 pc = vec3(lp.xy / lp.w * 0.5 + 0.5, lp.z / lp.w);
        if (pc.z <= 1.0 && pc.x > 0.0 && pc.x < 1.0 && pc.y > 0.0 && pc.y < 1.0) {
            float bias = max(0.0025 * (1.0 - ndl), 0.0008);
            vec2 texel = 1.0 / vec2(textureSize(u_shadowMap, 0));
            for (int x = -2; x <= 2; ++x)      // PCF 5×5 → bordes más suaves
            for (int y = -2; y <= 2; ++y) {
                float d = texture(u_shadowMap, pc.xy + vec2(x, y) * texel).r;
                shadow += (pc.z - bias > d) ? 1.0 : 0.0;
            }
            shadow /= 25.0;
        }
    }

    // Luz de luna (2ª luz): difusa tenue azulada → la noche no es negra cuando la
    // Luna está alta. Sin sombras (es luz suave de relleno).
    float ndlMoon = max(dot(N, normalize(moonDirection)), 0.0);

    vec3 color = ambientStrength * baseColor
               + 0.7 * ndl * sunLightColor * baseColor * (1.0 - 0.85 * shadow)
               + moonLightColor * moonIntensity * ndlMoon * baseColor;

    // Niebla atmosférica: da profundidad y un horizonte claro, y disimula el LOD
    // lejano. Color grisáceo (dieselpunk sombrío). FOG_DENSITY = qué tan pronto cierra.
    const vec3  FOG_COLOR   = vec3(0.60, 0.63, 0.68);
    const float FOG_DENSITY = 0.00035; // conocido-bueno: oculta el horizonte de bajo LOD (tiras)
    float fog = (u_fogEnabled > 0.5) ? (1.0 - exp(-length(FragPos) * FOG_DENSITY)) : 0.0;
    color = mix(color, FOG_COLOR, fog);

    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    FragColor = vec4(color, 1.0);
}
