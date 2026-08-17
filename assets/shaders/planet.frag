/**
 * @file planet.frag
 * @brief Fragment shader for planetary terrain chunks.
 *
 * Checkerboard pattern from per-face UVs. Both Procedural and Manual modes
 * use the same visual for now (different tint to distinguish them).
 * u_terrainMode: 0 = Procedural, 1 = Manual
 */
#version 450 core
// `#include` no existe en GLSL de serie. glslc/glslangValidator lo aceptan bajo esta extensión, y
// el RHI lo resuelve por texto al cargar (y RETIRA esta línea, porque el driver de GL no la
// reconoce). Así el mismo fichero valida en el build y compila en runtime.
#extension GL_GOOGLE_include_directive : require

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
    // (CLIMA) La MÁSCARA DE EXPOSICIÓN AL CIELO y el estado del agua en el suelo.
    mat4  u_skySpace;     // matriz del pase CENITAL (qué tienes ENCIMA)
    float u_skyMaskOn;    // >0.5 = hay máscara cenital bindeada
    float u_wetness;      // [0,1] agua ACUMULADA (sube lloviendo, baja secándose: permanece tras la lluvia)
    float u_rainNow;      // [0,1] lluvia CAYENDO ahora (las salpicaduras solo existen mientras cae)
    float u_timeS;        // reloj (anima las ondas de salpicadura)
    // (CAPA GRANULAR) La nieve NO es un bioma: es una capa que se ACUMULA sobre lo que haya.
    float u_snowAccum;    // [0,1] espesor de nieve acumulada
    float u_stampOn;      // >0.5 = hay campo de HUELLAS bindeado
    float u_hasRock;      // >0.5 = textura de roca disponible (si no, cae a u_landAlbedo)
    float _padG1;
    mat4  u_stampSpace;   // ventana de huellas (cenital, alrededor del jugador)
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
// (CLIMA) Profundidad de lo que hay ENCIMA (pase CENITAL). No es el shadow map del sol: la sombra
// se mueve con el sol, la cubierta no. Bajo el mismo árbol, la sombra viaja durante el día y la zona
// SECA se queda quieta — por eso hacen falta dos máscaras y no una.
layout(binding = 6) uniform sampler2D u_skyMask;
// (CAPA GRANULAR) Huellas: R = hundido, G = reborde levantado. Ventana cenital ±24 m del jugador.
layout(binding = 7) uniform sampler2D u_stampField;
// Texturas de bioma ROCA (Etapa 4A). Opcionales: si no están, el bioma rocoso usa u_landAlbedo.
layout(binding = 8) uniform sampler2D u_rockAlbedo;
layout(binding = 9) uniform sampler2D u_rockNormal;
// Macro variation texture (equirectangular, optional) — breaks tile repetition.
layout(binding = 10) uniform sampler2D u_macroVar;
// Biome map texture (equirectangular, generated by ProcGraph) — replaces GLSL biomeColor()
// RGB = biome color, A = biome index (0=desert..1=jungle)
layout(binding = 11) uniform sampler2D u_biomeMap;

// Equirectangular UV from direction.
vec2 equirectUV(vec3 dir) {
    vec3 d = normalize(dir);
    return vec2(0.5 + atan(d.z, d.x) * 0.1591549,
                0.5 - asin(clamp(d.y, -1.0, 1.0)) * 0.3183099);
}

// Sample macro variation texture from direction (equirectangular projection).
vec4 sampleMacroVariation(vec3 dir) {
    return texture(u_macroVar, equirectUV(dir));
}

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

// --- Material de bioma — paleta dieselpunk sombría ---
// BIOME_ROCK y BIOME_RIPAR se aplican como override PER-PÍXEL sobre el color
// de bioma que viene de la textura equirectangular generada por ProcGraph.
const vec3 BIOME_SAND  = vec3(0.44, 0.39, 0.27);
const vec3 BIOME_ROCK  = vec3(0.33, 0.29, 0.25);
const vec3 BIOME_RIPAR = vec3(0.22, 0.38, 0.15);

#include "lib/terrain_material.glsl"

// Sample biome map texture (equirectangular) — climate-scale color from ProcGraph.
// RGB = biome color, A = biome index (0=desert … 1=jungle).
vec4 sampleBiomeMap(vec3 dir) {
    return texture(u_biomeMap, equirectUV(dir));
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
// scale = 1/metros_por_tile. wp debe ser la posición RELATIVA AL PLANETA (relPos):
// FragPos es relativo a la cámara y hace que la textura se deslice al moverse.
vec3 triplanarAlbedo(sampler2D tex, vec3 wp, vec3 n, float scale) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= (bw.x + bw.y + bw.z + 1e-5);
    vec3 cx = texture(tex, wp.zy * scale).rgb;
    vec3 cy = texture(tex, wp.xz * scale).rgb;
    vec3 cz = texture(tex, wp.xy * scale).rgb;
    return cx * bw.x + cy * bw.y + cz * bw.z;
}

// Triplanar de NORMAL MAP, en "whiteout blend": se suman las tangentes de las tres proyecciones
// en vez de mezclar los vectores ya girados. Mezclar normales de mundo directamente las promedia
// hacia la normal geométrica y aplana el relieve justo en las caras diagonales, que son la mitad
// de una esfera.
// Devuelve un vector en espacio MUNDO listo para mezclar con la normal de sombreado.
vec3 triplanarNormal(sampler2D tex, vec3 wp, vec3 n, float scale) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= (bw.x + bw.y + bw.z + 1e-5);
    // Tangentes del normal map en cada plano (OpenGL Y+).
    vec3 tx = texture(tex, wp.zy * scale).xyz * 2.0 - 1.0;
    vec3 ty = texture(tex, wp.xz * scale).xyz * 2.0 - 1.0;
    vec3 tz = texture(tex, wp.xy * scale).xyz * 2.0 - 1.0;
    // Reorientar cada una a su eje y sumar la desviación tangencial.
    vec3 nx = vec3(0.0, tx.y, tx.x) * sign(n.x);
    vec3 ny = vec3(ty.x, 0.0, ty.y) * sign(n.y);
    vec3 nz = vec3(tz.x, tz.y, 0.0) * sign(n.z);
    return nx * bw.x + ny * bw.y + nz * bw.z;
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
    float aShore  = altitude + (noised(relPos * 0.004).x - 0.5) * 55.0; // ±~27 m de ruido (bajado de 90 → menos moteado arena/hierba)
    float shoreF  = 1.0 - smoothstep(0.0, 22.0, aShore);                 // banda de arena MÁS FINA (35→22 m): menos "manchas" en llanos costeros

    float bodyProfile = u_planetRelCam.w;  // 0 terran · 1 luna · 2 gas
    vec3 baseColor;
    // Desviación de normal que aporta el NORMAL MAP del material del terreno. Se rellena en la
    // rama con texturas y la consume el bloque de iluminación de más abajo; a cero significa
    // "sin relieve de textura" (modo manual, sin tiles, o lejos).
    vec3 matNormalDelta = vec3(0.0);
    if (bodyProfile > 1.5) {
        // GAS (Júpiter): bandas horizontales por latitud, tonos cálidos.
        float n    = noised(relPos * 0.0015).x;
        float band = sin(up.y * 16.0 + n * 4.0) * 0.5 + 0.5;
        baseColor  = mix(vec3(0.60, 0.46, 0.34), vec3(0.86, 0.77, 0.61), band);
    } else if (bodyProfile > 0.5) {
        // LUNA: regolito gris oscuro (albedo realista ~0.12-0.3).
        float g   = 0.30 + 0.05 * noised(relPos * 0.02).x;
        g        *= mix(0.78, 1.06, slope);
        baseColor = vec3(g, g, g * 1.03);
    } else if (u_terrainMode == 1) {
        baseColor = vec3(0.30, 0.30, 0.38);                // modo manual: tinte neutro
    } else if (u_hasTex > 0.5) {
        // ⚠️ El bioma manda el COLOR; la textura, solo el GRANO. Antes este camino IGNORABA el bioma
        // Whittaker: mezclaba tierra/hierba/roca/nieve genéricas, así que un desierto (sin hierba, sin
        // roca, sin nieve) quedaba como la textura de tierra pura = GRIS. Todo el trabajo de biomas
        // (desierto dorado, estepa, taiga, selva, ribera) era invisible en cuanto había texturas.
        // Ahora el color de bioma viene de la textura equirectangular generada por ProcGraph
        // (BiomeClassifyNode), y solo los overrides per-píxel (roca, ribera) quedan en GLSL.
        // Per-pixel edge noise — rompe fronteras de bioma a resolución completa
        float n = (noised(relPos * 0.004).x - 0.5);
        // Perturbar UV de la biome map con el mismo edge noise → fronteras rotas
        vec2  bUV = equirectUV(up) + n * 0.003;
        vec4  biomeSample = texture(u_biomeMap, bUV);
        vec3  biomeCol    = biomeSample.rgb;
        float H = clamp(humidity + n * 0.12, 0.0, 1.0);
        // ROCA — override por pendiente + afloramiento
        float rockZone = smoothstep(0.62, 0.80, noised(relPos * 0.0016).x + n * 0.6);
        float rock     = max(smoothstep(0.55, 0.80, slope), rockZone);
        vec3  c        = mix(biomeCol, BIOME_ROCK, rock);
        // RIBERA — override por caudal
        c = mix(c, BIOME_RIPAR, smoothstep(0.30, 0.55, flow) * (1.0 - smoothstep(0.35, 0.6, slope)));
        // Variación de cerca. El alcance del detalle pasa de 2.5–9 km a 12–60 km: con el corte
        // anterior, TODO lo que se veía desde el aire era color plano sin una sola textura, que es
        // buena parte de por qué el planeta se leía liso desde lejos.
        float near  = 1.0 - smoothstep(12000.0, 60000.0, length(FragPos));
        float v = noised(relPos * 0.4).x * near;
        biomeCol = c * (0.94 + 0.10 * v);

        // --- MATERIAL: los dos ejes del clima, no solo la humedad --------------------------
        // `rock` (pendiente + afloramiento) entra como PENDIENTE efectiva: así el material de roca
        // se elige con la misma regla de la tabla que todos los demás, en vez de con un caso aparte.
        vec3 matTint; float grainAmt, detailAmt; int tile; int matIdx;
        // Sin mapa de zonas en esta ruta (el terreno V3 por chunks aún no lo bindea): reglas de clima.
        vec4 matColor;
        // ⚠️ Cuarto eje (altura) INERTE en este camino: es el terreno V3 por chunks, y su fragmento
        // solo recibe Normal/FragPos/TexCoord — no tiene la cota. Se pasa 0 para que los materiales
        // sin banda de altura declarada (el rango por defecto ±1000 km) sigan funcionando igual.
        // El camino VIVO es `planet/biome.frag`, que sí lee la cota del bake por píxel.
        // `tileBed`/`coverW` se reciben y se IGNORAN a propósito: este es el terreno V3 por chunks,
        // un camino muerto (el vivo es `planet/biome.frag`). Mezclar aquí las dos capas de la columna
        // sería trabajo sobre código que no se dibuja.
        int   tileBed_unused; float coverW_unused;
        harukaSelectMaterial(H, tempC, max(slope, rock), 0.0, vec3(0.0), false,
                             matTint, grainAmt, detailAmt, tile, tileBed_unused, coverW_unused,
                             matColor, matIdx);
        biomeCol = mix(biomeCol, matColor.rgb, matColor.a);

        // Tile del material. Con solo cuatro disponibles (sand/grass/land/rock), tundra y desierto
        // comparten `land` y taiga comparte `grass`: se distinguen por tinte y por cuánto grano y
        // relieve dejan pasar, no por tener PNG propio. El HIELO no usa tile: la nieve es lisa, y
        // darle el grano de tierra era justo lo que la delataba como "tierra pintada de blanco".
        vec3 tex = vec3(0.45);       // sin tile: luminancia neutra → grano ≈ 1, superficie lisa
        vec3 tileNrm = vec3(0.0);
        const float kTileScale = 0.5;      // 1/metros por tile
        // GLSL no indexa samplers dinámicamente (son handles, no valores): la tabla da un ÍNDICE y
        // aquí se ramifica la llamada. El día que haya `sampler2DArray`, esto es una línea.
        if (tile == 3) {
            if (u_hasRock > 0.5) {
                tex     = triplanarAlbedo(u_rockAlbedo, relPos, Ngeo, kTileScale);
                tileNrm = triplanarNormal(u_rockNormal, relPos, Ngeo, kTileScale);
            } else {
                tex     = triplanarAlbedo(u_landAlbedo, relPos, Ngeo, kTileScale);
                tileNrm = triplanarNormal(u_landNormal, relPos, Ngeo, kTileScale);
            }
        } else if (tile == 1) {
            tex     = triplanarAlbedo(u_grassAlbedo, relPos, Ngeo, kTileScale);
            tileNrm = triplanarNormal(u_landNormal,  relPos, Ngeo, kTileScale);
        } else if (tile == 0) {
            tex     = triplanarAlbedo(u_sandAlbedo, relPos, Ngeo, kTileScale);
            tileNrm = triplanarNormal(u_sandNormal, relPos, Ngeo, kTileScale);
        } else if (tile == 2) {
            tex     = triplanarAlbedo(u_landAlbedo, relPos, Ngeo, kTileScale);
            tileNrm = triplanarNormal(u_landNormal, relPos, Ngeo, kTileScale);
        }

        // El PNG aporta GRANO (luminancia) y RELIEVE (normal), no color: el color es el del bioma,
        // que sale del clima. `grainAmt` decide cuánto se nota por material.
        float grain = dot(tex, vec3(0.333)) / 0.45;
        grain = mix(1.0, grain, grainAmt);
        float sandW = shoreF * (1.0 - smoothstep(0.45, 0.7, slope));

        // NORMAL del tile — hasta ahora `u_*Normal` se cargaban y NUNCA se muestreaban. Es el
        // aporte real de la textura bajo una luz toon: relieve, no tono. Se acumula en `matNormal`
        // y `main` la mezcla con la normal geométrica más abajo.
        matNormalDelta = tileNrm * (detailAmt * near);

        // Macro variation: sample equirectangular texture to break tile repetition
        vec4 macro = sampleMacroVariation(up);
        float macroBrightness = macro.r;
        float macroHueShift   = (macro.g - 0.5) * 2.0;  // convert [-0.5,0.5] → approx [-1,1]
        // El ALBEDO SE VE, no solo su luminancia (mismo fix que biome.frag): el color del PNG se
        // mezcla con el del bioma para que el patrón (hierba, arena, roca) sea visible. `tex * 2.0`
        // compensa el brillo medio (~0.45); `near` atenúa el peso con la distancia.
        float texW = clamp(grainAmt * 0.55, 0.0, 0.75) * near;
        vec3 col = mix(biomeCol, biomeCol * tex * 2.0, texW) * matTint * mix(1.0, clamp(grain, 0.75, 1.25), near);
        col *= 0.85 + 0.30 * macroBrightness;           // macro brightness modulation
        baseColor   = mix(col, triplanarAlbedo(u_sandAlbedo, relPos, Ngeo, 0.7), sandW);
    } else {
        float n = (noised(relPos * 0.004).x - 0.5);
        vec2 bUV = equirectUV(up) + n * 0.003;
        baseColor = texture(u_biomeMap, bUV).rgb;
        float rockZone = smoothstep(0.62, 0.80, noised(relPos * 0.0016).x + n * 0.6);
        float rock = max(smoothstep(0.55, 0.80, slope), rockZone);
        baseColor = mix(baseColor, BIOME_ROCK, rock);
        baseColor = mix(baseColor, BIOME_RIPAR, smoothstep(0.30, 0.55, flow) * (1.0 - smoothstep(0.35, 0.6, slope)));
        float sandW = shoreF * (1.0 - smoothstep(0.45, 0.7, slope));
        baseColor = mix(baseColor, BIOME_SAND, sandW);
    }

    // Normal del vértice (analítica, sin pinchos) + detalle fino 1–2 m por píxel.
    vec3 N = applyDetailNormal(Ngeo, relPos, length(FragPos));
    // + RELIEVE DEL MATERIAL: la desviación del normal map del tile. Va DESPUÉS del detalle
    // procedural y solo su parte tangencial, por la misma razón que allí: la componente a lo largo
    // de N no inclina nada y sí desnormaliza. La fuerza es baja (0.35) a propósito — con el
    // sombreado cel un relieve fuerte pica el terminador y saca manchas oscuras, que es justo lo
    // que obligó a bajar DETAIL_STRENGTH de 0.22 a 0.10 en su día.
    if (dot(matNormalDelta, matNormalDelta) > 1e-8) {
        vec3 d = matNormalDelta - dot(matNormalDelta, N) * N;
        N = normalize(N + 0.35 * d);
    }
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

    // ── EL SUELO MOJADO (clima) ─────────────────────────────────────────────────────────────────
    // Dos cantidades distintas, y la diferencia se ve: `u_wetness` es el agua ACUMULADA (tarda en
    // subir y en secarse: el suelo sigue oscuro un rato después de escampar) y `u_rainNow` es la
    // lluvia que cae AHORA (las salpicaduras existen solo mientras cae).
    //
    // La MÁSCARA CENITAL es lo que lo convierte en "marca según collider": donde hay algo encima, el
    // agua no llega y queda una silueta SECA — bajo el árbol, bajo el alero, bajo la roca. Ojo: NO es
    // el shadow map. La sombra del sol viaja a lo largo del día; el sitio seco no se mueve.
    // Fuera del alcance de la máscara (±48 m del jugador) se asume EXPUESTO: es el caso mayoritario
    // (el mundo es cielo abierto) y el error se va a donde no se mira de cerca. Ampliar el alcance
    // cuesta resolución por metro, que es justo lo que hace creíble el borde del alero.
    float exposure = 1.0;
    if (u_skyMaskOn > 0.5 && (u_wetness > 0.002 || u_rainNow > 0.002)) {
        vec4 sp = u_skySpace * vec4(FragPos, 1.0);
        vec3 sc = vec3(sp.xy / sp.w * 0.5 + 0.5, sp.z / sp.w);   // z ya en [0,1] (ZERO_TO_ONE)
        if (sc.z <= 1.0 && sc.x > 0.0 && sc.x < 1.0 && sc.y > 0.0 && sc.y < 1.0) {
            // PCF 3×3: el borde de la zona seca es difuso (el viento mete agua bajo el alero), y sin
            // filtrar se verían los téxeles de la máscara como un recorte de cartón.
            vec2  tx = 1.0 / vec2(textureSize(u_skyMask, 0));
            float cov = 0.0;
            for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                cov += (sc.z - 0.0015 > texture(u_skyMask, sc.xy + vec2(x, y) * tx).r) ? 1.0 : 0.0;
            exposure = 1.0 - cov / 9.0;
        }
    }
    float wet = clamp(u_wetness * exposure, 0.0, 1.0);

    // CHARCOS: el agua se acumula donde puede QUEDARSE — llano y, con más razón, en las vaguadas por
    // donde ya corre el agua (`flow` del campo hidrológico). En pendiente escurre y no hay charco.
    float pond = smoothstep(0.30, 0.02, slope)
               * smoothstep(0.10, 0.45, flow * 0.5 + noised(relPos * 0.07).x * 0.6)
               * wet;

    // El agua OSCURECE y SATURA (el índice de refracción hace que el suelo mojado refleje menos
    // difusa): tierra parda → tierra chocolate. Es el 80 % del efecto; el brillo es el otro 20 %.
    baseColor = mix(baseColor, baseColor * vec3(0.52, 0.50, 0.55), wet * 0.85);
    baseColor = mix(baseColor, baseColor * vec3(0.34, 0.38, 0.46), pond * 0.7);

    // ══ LA CAPA GRANULAR: NIEVE ENCIMA, Y LAS HUELLAS QUE LA ROMPEN ═════════════════════════════
    // La nieve NO sustituye al material del suelo (eso era el bioma por temperatura que se quitó):
    // se ACUMULA encima de lo que haya —tierra, hierba, roca— y deja asomar lo de debajo donde no
    // cuaja. Dónde cuaja lo deciden tres cosas, ninguna arbitraria:
    //   · la MÁSCARA CENITAL: bajo el alero o la copa del árbol no cae nieve, igual que no cae agua;
    //   · la PENDIENTE: en la pared vertical la nieve resbala y no se sujeta;
    //   · un ruido de borde, para que la línea entre cubierto y descubierto no sea un recorte limpio.
    float snowCover = u_snowAccum * exposure
                    * (1.0 - smoothstep(0.42, 0.78, slope));
    snowCover = clamp(snowCover * 1.35 - 0.12 + noised(relPos * 0.9).x * 0.22, 0.0, 1.0);

    // ARENA — la otra capa granular, y NO depende del clima del día: el desierto es de arena llueva o
    // no. Sale de la ARIDEZ del campo (el mismo eje de humedad que decide el bioma) y de la ORILLA,
    // ambas limitadas por la pendiente: en la pared no se sostiene un manto suelto, aflora la roca.
    float sandCover = (1.0 - smoothstep(0.08, 0.26, humidity)) * (1.0 - smoothstep(0.30, 0.62, slope));
    sandCover = max(sandCover, shoreF * (1.0 - smoothstep(0.45, 0.70, slope)));   // la playa
    sandCover = clamp(sandCover * (1.0 - snowCover), 0.0, 1.0);  // si ha nevado encima, manda la nieve

    // La capa PISABLE es la que haya: nieve o arena. El campo de huellas es el mismo (hundido+reborde);
    // lo que cambia por material es cuánto DURA la huella, y eso lo decide la CPU (`GroundLayer`).
    float granular = max(snowCover, sandCover);

    float trod = 0.0, rim = 0.0;     // pisado / reborde levantado
    vec2  tuv  = vec2(-1.0);         // <0 = fuera de la ventana de huellas
    if (u_stampOn > 0.5 && granular > 0.01) {
        vec4 tp = u_stampSpace * vec4(FragPos, 1.0);
        vec2 uvq = tp.xy / tp.w * 0.5 + 0.5;
        if (uvq.x > 0.0 && uvq.x < 1.0 && uvq.y > 0.0 && uvq.y < 1.0) {
            tuv = uvq;
            vec2 st = texture(u_stampField, tuv).rg;
            trod = clamp(st.r, 0.0, 1.0);
            rim  = clamp(st.g, 0.0, 1.0);
        }
    }

    if (snowCover > 0.002) {
        // Nieve LIMPIA arriba, más gris y azulada en el hueco de la pisada (ahí ves nieve compactada
        // y algo del suelo de debajo). La huella se ve sobre todo por su SOMBRA, no por su color:
        // el hueco recibe menos luz y el reborde más — de ahí la perturbación de la normal, abajo.
        vec3 snowClean = vec3(0.90, 0.93, 0.98);
        vec3 snowTrod  = mix(snowClean, mix(vec3(0.62, 0.66, 0.74), baseColor, 0.25), 0.75);
        vec3 snowCol   = mix(snowClean, snowTrod, trod);
        baseColor = mix(baseColor, snowCol, snowCover * (1.0 - 0.35 * trod));
        // La nieve fresca NO está mojada (el agua está congelada): donde cuaja, se apaga el brillo
        // de lluvia, o la nevada dejaría el suelo con charcos y reflejos, que es lo contrario.
        wet  *= 1.0 - snowCover * 0.9;
        pond *= 1.0 - snowCover;
    }

    if (sandCover > 0.002) {
        // ARENA: la capa no recolorea el desierto (su albedo YA es de arena, por bioma) — lo que
        // aporta es el GRANO suelto y cómo reacciona a que lo pisen. La huella en arena es al revés
        // que en nieve: al hundir sale material de DEBAJO, que está más húmedo y compacto → más
        // OSCURO; y el reborde, arena seca removida → más CLARO. Por eso no se puede usar el mismo
        // tinte para las dos capas aunque el campo de huellas sea el mismo.
        vec3 sandLoose = baseColor * 1.06;
        vec3 sandTrod  = baseColor * 0.80;
        vec3 sandRim   = baseColor * 1.14;
        vec3 sandCol   = mix(mix(sandLoose, sandTrod, trod), sandRim, rim * 0.7);
        baseColor = mix(baseColor, sandCol, sandCover);
        // Ondulación fina del manto suelto (rizos de viento) donde NO se ha pisado. Se apaga con la
        // pisada: por eso una huella en duna se lee tan bien — rompe un patrón regular.
        float ripple = sin(dot(relPos.xz, vec2(3.1, 1.7)) * 0.9 + noised(relPos * 0.12).x * 5.0);
        baseColor *= 1.0 + ripple * 0.025 * sandCover * (1.0 - trod);
    }

    // ⚠️ LA HUELLA SE VE POR SU SOMBRA, NO POR SU COLOR. Sobre nieve blanca (o arena clara) casi no
    // hay contraste de albedo: si solo se oscureciera el hueco, la pisada parecería una MANCHA. Lo
    // que la lee como hueco es la NORMAL — el hundido se inclina hacia dentro y el reborde hacia
    // fuera, y la luz rasante hace el resto. De ahí el gradiente del campo (hundido − reborde).
    if (granular > 0.002 && tuv.x >= 0.0 && (trod > 0.002 || rim > 0.002)) {
        vec2  tx = 1.0 / vec2(textureSize(u_stampField, 0));
        float hL = texture(u_stampField, tuv - vec2(tx.x, 0)).r - texture(u_stampField, tuv - vec2(tx.x, 0)).g;
        float hR = texture(u_stampField, tuv + vec2(tx.x, 0)).r - texture(u_stampField, tuv + vec2(tx.x, 0)).g;
        float hD = texture(u_stampField, tuv - vec2(0, tx.y)).r - texture(u_stampField, tuv - vec2(0, tx.y)).g;
        float hU = texture(u_stampField, tuv + vec2(0, tx.y)).r - texture(u_stampField, tuv + vec2(0, tx.y)).g;
        vec3 t1 = normalize(cross(Ngeo, abs(Ngeo.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0)));
        vec3 t2 = cross(Ngeo, t1);
        // La arena se hunde MENOS que la nieve: mismo campo, relieve más suave (0.6 frente a 1.0).
        float relief = mix(0.6, 1.0, snowCover / max(granular, 1e-3));
        N = normalize(N + (t1 * (hR - hL) + t2 * (hU - hD)) * 2.2 * granular * relief);
    }

    // --- Iluminación TOON (cel suave + sombra FRÍA) — MISMO idioma que escena/props/follaje. El terreno
    //     es MATE (sin especular). El shadow map oscurece el lado iluminado; la luna rellena la noche. ---
    float band = smoothstep(-0.04, 0.24, dot(N, L));         // terminador SUAVE (no duro)
    band *= (1.0 - 0.9 * shadow);                            // los props proyectan sombra sobre el suelo
    vec3  litCol    = baseColor * (0.80 + 0.25 * sunLightColor);  // sin sobre-brillar albedos claros (nieve)
    vec3  shadowCol = baseColor * vec3(0.42, 0.48, 0.60);    // sombra fría, algo desaturada (crudeza mística)
    vec3  color = mix(shadowCol, litCol, band)
                + moonLightColor * moonIntensity * smoothstep(0.0, 0.6, ndlMoon) * baseColor * 0.6;

    // BRILLO DEL AGUA. El terreno es MATE a propósito (decisión de estilo: es tierra, no plástico),
    // pero el agua encima SÍ es especular — es lo que hace leer "mojado" y no "más oscuro". Va aquí
    // y no en el albedo porque depende de la vista. Un charco brilla mucho más que la tierra húmeda.
    if (wet > 0.002) {
        vec3  V = normalize(-FragPos);                 // FragPos es relativo a la cámara
        vec3  H = normalize(L + V);
        float glossy = mix(28.0, 160.0, pond);         // charco = reflejo más apretado
        float spec   = pow(max(dot(N, H), 0.0), glossy) * (0.12 + 0.55 * pond) * wet;
        spec        *= band;                           // sin sol no hay reflejo (ni en sombra)
        color += sunLightColor * spec;
        // Fresnel: a rasante el agua refleja el CIELO. Da el destello de la carretera mojada.
        float fres = pow(1.0 - max(dot(N, V), 0.0), 5.0);
        color = mix(color, mix(color, vec3(0.62, 0.72, 0.86), 0.55), fres * wet * (0.25 + 0.6 * pond));
    }

    // SALPICADURAS: anillos que se abren donde la gota golpea. Solo mientras LLUEVE (no quedan tras
    // escampar), solo a cielo abierto (bajo el alero no cae nada) y solo cerca — a 40 m un anillo de
    // 20 cm es sub-píxel y solo produciría ruido de aliasing.
    float splashNear = 1.0 - smoothstep(12.0, 34.0, length(FragPos));
    if (u_rainNow > 0.01 && splashNear > 0.01 && exposure > 0.3) {
        vec2  cellS = floor(relPos.xz * 3.0);                       // celdas de ~33 cm
        float ph    = fract(sin(dot(cellS, vec2(41.7, 289.1))) * 43758.5453);
        float t     = fract(u_timeS * 1.7 + ph);                    // cada gota, su fase
        vec2  cen   = (fract(relPos.xz * 3.0) - 0.5) / 3.0;
        float r     = length(cen);
        float ring  = smoothstep(0.02, 0.0, abs(r - t * 0.16)) * (1.0 - t);  // se abre y se apaga
        color += vec3(0.55, 0.62, 0.72) * ring * u_rainNow * splashNear * exposure * 0.35;
    }

    // --- ACORDE DE COLOR POR BIOMA (firma): tiñe la LUZ y la bruma según el clima ya calculado. Eje
    //     TEMPERATURA (frío-azul ↔ cálido-dorado) + HUMEDAD (seco-polvo ↔ verde-lush). Sutil: el albedo ya
    //     es por bioma; esto es el "acorde atmosférico" que Genshin/SAO (mundos cerrados) no tienen. ---
    float warm    = smoothstep(6.0, 26.0, tempC);
    vec3  tempTint = mix(vec3(0.84, 0.92, 1.07), vec3(1.09, 1.01, 0.85), warm);   // frío → cálido
    vec3  humTint  = mix(vec3(1.07, 1.00, 0.85), vec3(0.93, 1.06, 0.93), humidity); // seco → húmedo(verde)
    vec3  biomeMood = tempTint * humTint;
    color *= mix(vec3(1.0), biomeMood, 0.35);   // 0.35 = fuerza del acorde (sutil)

    // Niebla atmosférica: da profundidad y un horizonte claro, y disimula el LOD
    // lejano. BRUMA anime: azul-cielo suave que se CALIENTA hacia el color del sol → horizonte luminoso y
    // cálido que cambia con la hora del día (firma "atmósfera del planeta"), en vez del gris dieselpunk.
    // Además se tiñe con el acorde del bioma → la bruma del bosque es distinta a la del desierto.
    vec3        FOG_COLOR   = mix(vec3(0.66, 0.78, 0.90), sunLightColor, 0.35) * mix(vec3(1.0), biomeMood, 0.5);
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
