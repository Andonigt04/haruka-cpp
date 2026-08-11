#version 460 core
#extension GL_GOOGLE_include_directive : require
layout(vertices = 4) out;
layout(location = 0) in vec3 cpPos[]; layout(location = 1) in vec3 cpColor[]; layout(location = 2) in vec3 cpClimate[];
layout(location = 0) out vec3 ePos[]; layout(location = 1) out vec3 eColor[]; layout(location = 2) out vec3 eClimate[];
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
    vec4 uTexAnchor;   // ancla planetaria de las UV de terreno (la usa biome.frag; ver planet.cpp)
};
// El MISMO bake que usa terrain.tese (binding 16) para la banda costera del edgeFactor.
layout(binding = 16) uniform sampler2D uHeightTex;
// ClipParams (binding 13) para el SEMI-LADO real del clipmap: los anillos concéntricos cogen su
// anillo interior de ESE valor, el mismo con el que terrain.tese recorta la base y el clipmap pinta.
layout(std140, binding = 13) uniform ClipParams {
    vec4 uClipOrigin; vec4 uClipTanU; vec4 uClipTanV; vec4 uClipCover;
};
#include "lib/terrain_detail.glsl"

// ANILLOS CONCÉNTRICOS (Fase 1 del plan §9 de TERRENO.md): cota superior del factor de teselación
// por RANGO DE DISTANCIA a la cámara, en vez de "lo que pida el tamaño en pantalla" en todo el
// planeta.
//
// Por qué: hoy la malla base tesela a tope 128 hasta ~50 km (los 3,9 M de vértices del §8) cuando
// el detalle fino a esa distancia ya está MUERTO por el Nyquist de `triM` (terrain.tese): la octava
// de 625 m a d=20 km pide triM>200 m y `octaveWeight` la excluye. Esos vértices son puro desperdicio.
//
// Dónde recorta y dónde no (esto es lo delicado, y por eso va por anillos):
//   - Hasta ~clipHalf·6 (≈12 km): tope 128, IGUAL que hoy. Aquí vive el recorte del clipmap (`inClip`
//     en terrain.tese recorta por VÉRTICE) y el sesgo de profundidad: si la teselación colapsara
//     dentro del clipmap, el borde del recorte se volvería un serrucho de quads de kilómetros que
//     se asoman por el sesgo. La densidad cercana NO se toca.
//   - De ~clipHalf·6 a ~clipHalf·24 (≈12-48 km): bajada SUAVE de 128 → 24. Sin escalones: una
//     transición de LOD dura se ve como una línea; el `smoothstep` hace la densidad continua con
//     la distancia.
//   - Más allá: 24. El relieve fino lo recuperará el per-pixel (Fase 2) por shading, sin vértices.
//
// La cota es función de la distancia del PUNTO MEDIO de la arista —el mismo dato compartido que usa
// el factor base— así que dos parches vecinos obtienen la MISMA cota y no puede haber grietas (§2.2).
// El refino de costa multiplica por encima de la cota (y se clampa a 128 al final): es una banda
// localizada en el nivel del mar y en órbita no debe perder su subdivisión.
// ⚠️ El techo es 64, NO 128. `GL_MAX_TESS_GEN_LEVEL` vale 64 en todo el hardware de escritorio (es
// el teselador de función fija; el mínimo del spec y lo que reportan NVIDIA, AMD e Intel — y también
// Vulkan y D3D, porque es el mismo silicio). Pedir 128 NO da error: el driver recorta en SILENCIO,
// así que el shader hacía algo distinto de lo que decía su comentario. Y tenía un segundo efecto:
// `mix(128, 24, s)` recortado a 64 se quedaba PLANO en 64 hasta s≈0.62 (d ≈ clipHalf·18), o sea que
// la bajada por anillos no empezaba a los ~12 km sino a los ~32 km — 20 km de franja pagando
// teselación plena que el diseño creía ya recortada. Con el techo en 64 la rampa es la que dice ser.
// Gemelo de TERRAIN_MESH_TESS_CAP / TERRAIN_MAX_TESS_GEN_LEVEL (core/planet/terrain_lod.h).
float ringCap(float d) {
    float clipHalf = uDebug.w > 0.5 ? uClipCover.x : 2000.0;
    return mix(64.0, 24.0, smoothstep(clipHalf * 6.0, clipHalf * 24.0, d));
}

// Factor de una arista, calculado SOLO a partir de la arista.
//
// Esta es la propiedad que sustituye a toda la maquinaria de cosido del sistema de chunks: dos
// parches vecinos comparten arista, le pasan los mismos dos extremos y obtienen el MISMO número,
// así que no puede haber grietas por construcción. Meter cualquier dato del parche aquí —su
// centro, su normal, su nivel— rompe la simetría y las grietas vuelven.
float edgeFactor(vec3 a, vec3 b) {
    // Las posiciones llegan relativas al centro del planeta; uCenter.xyz = centro − cámara.
    vec3  mid = (a + b) * 0.5 + uCenter.xyz;     // punto medio, relativo a la CÁMARA
    float d   = max(length(mid), 1.0);
    float arc = length(a - b);                   // longitud de la arista en metros
    // Objetivo: ~kTargetPx radianes por segmento. Antes 0.012: la costa quedaba en bloques de
    // kilómetros a media distancia; 0.004 dejaba triángulos de ~3 px en pantalla (se veían las
    // facetas); 0.002 los deja ~1.5 px, por debajo del umbral visible.
    // PERO con 0.002 el factor caía a 1 (la malla base de 256, bloques de ~38 km) nada más pasar
    // de ~1.2×radio: desde órbita la costa volvía a salir POLIGONAL. 0.0007 mantiene la
    // subdivisión cuando te acercas al planeta (LOD ~2-4 a 1.5-2×radio) SIN tocar la malla base.
    const float kTargetPx = 0.0007;               // radianes por segmento ≈ tamaño en pantalla
    // El tope efectivo es 64 y lo pone `ringCap` (Fase 1, §9).
    //
    // ⚠️ Aquí hubo un intento de subirlo a 128 para que el quad cercano bajara de ~305 m a ~152 m y
    // la octava de 625 m (±35 m) dejara de muestrearse a 2 muestras por onda — el suelo FACETEADO
    // del borde del clipmap. NUNCA OCURRIÓ: 128 está por encima de `GL_MAX_TESS_GEN_LEVEL` (64) y el
    // driver lo recortaba sin avisar, así que el quad siguió en ~305 m y el faceteado con él. Si se
    // quiere de verdad, la vía NO es pedir más nivel: es partir el parche (más vértices base) o
    // mover esa banda al clipmap. Cualquier tope que se escriba aquí tiene que ser ≤ 64.
    float raw = arc / (d * kTargetPx);
    float f = clamp(raw, 1.0, ringCap(d));

    // REFINADO DE COSTA — arista que toca o cruza el nivel del mar.
    //
    // La costa es el único sitio donde la teselación gruesa se ve desde lejos: es una línea (el
    // cruce del terreno por el nivel del mar) sobre la malla, y en órbita (factor base 1-4, quads
    // de 10-38 km) salía POLIGONAL mientras el resto del planeta no acusaba los bloques. Aquí se
    // sube el factor hasta ×5 en el margen ±150 m del nivel del mar (con desvanecido a ±400 m);
    // el resto del planeta queda igual de grueso, así que el coste solo crece en una banda.
    //
    // La banda se lee del MISMO bake que pinta la malla (uHeightTex, R32F en metros), para que el
    // refino siga la costa REAL que dibuja terrain.tese, no una aproximación. Y es función SOLO de
    // los dos extremos de la arista —la propiedad de §2.2 de TERRENO.md—, así que dos parches
    // vecinos comparten el mismo boost y no puede haber grietas.
    //
    // El coste está acotado: solo se muestrean los 2 extremos cuando el factor GEOMÉTRICO crudo ya
    // es pequeño (raw < 12 ≈ cámara más lejos de ~4 600 km), que es exactamente cuando la costa se
    // ve gruesa. Se comprueba `raw`, no `f`, para que la cota del anillo (ringCap) no impida refinar
    // la costa: en órbita ringCap es baja pero la costa debe conservar su subdivisión.
    // A pie y a baja órbita el factor ya es alto (o lo cubre el clipmap) y no se paga nada.
    if (uDebug.z > 0.5 && raw < 12.0) {
        ivec2 sz = textureSize(uHeightTex, 0);
        float hA = harukaSampleHeightField(uHeightTex, sz, harukaEquirectUV(normalize(a)));
        float hB = harukaSampleHeightField(uHeightTex, sz, harukaEquirectUV(normalize(b)));
        float hi = max(hA, hB), lo = min(hA, hB);
        float band = 1.0 - smoothstep(150.0, 400.0, max(hi, -lo));
        f *= 1.0 + 4.0 * band;
    }
    return clamp(f, 1.0, 128.0);
}

// Culling de LIMBO del planeta. Un parche tras el limbo es invisible: la línea de visión hasta él
// atraviesa la esfera. Sin esto, a pie se teselan los 6×512² parches de TODO el planeta cada frame
// (media esfera tras la espalda + casi todo el hemisferio "delantero" bajo el horizonte) — trabajo
// muerto que es el grueso del coste GPU a pie.
//
// El test es el de segmento↔esfera EXACTO, no un ángulo de horizonte con margen fijo: se cull
// solo cuando hasta el punto MÁS favorable del parche (su rincón más cercano a la cámara, elevado
// a la envolvente de relieve R+relief) está detrás de la esfera. Así un pico alto que asoma por el
// limbo no se recorta. La misma matemática vale desde órbita (recorta el hemisferio oculto) y a pie
// (recorta todo lo que la curvatura oculta).
bool patchHidden(vec3 camRel, vec3 camDir, float R, vec3 cp0, vec3 cp1, vec3 cp2, vec3 cp3) {
    const float kMaxRelief = 12000.0;             // techo del relieve (m): placa + mapa de autor

    // Rincón del parche más cercano a la cámara: es el que puede asomar por el limbo.
    vec3  corner = cp0;
    float best   = dot(corner, camDir);
    float d1 = dot(cp1, camDir); if (d1 > best) { best = d1; corner = cp1; }
    float d2 = dot(cp2, camDir); if (d2 > best) { best = d2; corner = cp2; }
    float d3 = dot(cp3, camDir); if (d3 > best) { best = d3; corner = cp3; }
    vec3 P     = normalize(corner) * (R + kMaxRelief);
    vec3 CtoP  = P - camRel;
    float dCP2 = dot(CtoP, CtoP);

    // Pie de la perpendicular del centro sobre la recta cámara→P. Si cae FUERA del segmento, el
    // punto más cercano del segmento al centro es uno de sus extremos (ambos a radio > R) → visible.
    float foot = dot(camRel, camRel - P);
    if (foot < 0.0 || foot > dCP2) return false;
    // Si cae dentro: oculto si la recta atraviesa la esfera (distancia al centro < R).
    float dist = length(cross(camRel, P)) / sqrt(dCP2);
    return dist < R;
}

// Culling de FRUSTUM CONSERVADOR vía ESFERA envolvente por parche. Un parche cuyos 4 vértices
// quedan fuera por la misma cara NO siempre es invisible: con la envolvente de +12 km de relieve
// la malla real puede asomar por una esquina. El intento anterior probaba los 4 vértices elevados
// contra el cubo NDC y esa envolvente, mayor que la distancia local al frustum, empujaba TODOS los
// vértices de un parche visible al otro lado de la MISMA cara → se recortaba terreno que se veía
// (huecos en las esquinas / "no renderiza todo el planeta").
//
// Aquí la envolvente es la ESFERA MÍNIMA del parche (centroide + distancia al vértice más lejano
// + relief): un parche se cull SOLO si la esfera entera queda fuera de una cara del cubo NDC —
// la condición exacta de invisibilidad para un conjunto convexo. No puede recortar nada visible.
bool frustumHidden(vec3 cp0, vec3 cp1, vec3 cp2, vec3 cp3) {
    const float kMaxRelief = 12000.0;
    vec3  center = (cp0 + cp1 + cp2 + cp3) * 0.25;
    float r = kMaxRelief;
    r = max(r, distance(center, cp0)); r = max(r, distance(center, cp1));
    r = max(r, distance(center, cp2)); r = max(r, distance(center, cp3));
    // Planos del frustum en el espacio de entrada de uMVP (cámara-relativo), por FILAS. Cubo NDC:
    // x,y∈[-w,w]; para las caras laterales el exterior es:
    //   left  (x>=-w): (r0+r3)·p < 0   right (x<= w): (r0-r3)·p > 0
    //   bottom(y>=-w): (r1+r3)·p < 0   top   (y<= w): (r1-r3)·p > 0
    //   near/z< -w  : (r2+r3)·p < 0    far/z>  w : (r2-r3)·p > 0
    // (con ZERO_TO_ONE+reversed-Z estos dos últimos son el plano del ojo y el infinito; el test del
    // infinito queda inerte, el del ojo cull solo lo que está detrás de la cámara: seguro).
    vec4 r0 = vec4(uMVP[0][0], uMVP[1][0], uMVP[2][0], uMVP[3][0]);
    vec4 r1 = vec4(uMVP[0][1], uMVP[1][1], uMVP[2][1], uMVP[3][1]);
    vec4 r2 = vec4(uMVP[0][2], uMVP[1][2], uMVP[2][2], uMVP[3][2]);
    vec4 r3 = vec4(uMVP[0][3], uMVP[1][3], uMVP[2][3], uMVP[3][3]);
    vec4 C4 = vec4(center + uCenter.xyz, 1.0);
    // Por cara: distancia con signo de la esfera; cull solo si TODA la esfera queda fuera del radio.
    float s = dot(r0 + r3, C4); { float L = length((r0 + r3).xyz); if (s < -r * L) return true; } // left
    s       = dot(r0 - r3, C4); { float L = length((r0 - r3).xyz); if (s >  r * L) return true; } // right
    s       = dot(r1 + r3, C4); { float L = length((r1 + r3).xyz); if (s < -r * L) return true; } // bottom
    s       = dot(r1 - r3, C4); { float L = length((r1 - r3).xyz); if (s >  r * L) return true; } // top
    s       = dot(r2 + r3, C4); { float L = length((r2 + r3).xyz); if (s < -r * L) return true; } // near (ojo)
    s       = dot(r2 - r3, C4); { float L = length((r2 - r3).xyz); if (s >  r * L) return true; } // far (infinito)
    return false;
}

void main() {
    ePos[gl_InvocationID]     = cpPos[gl_InvocationID];
    eColor[gl_InvocationID]   = cpColor[gl_InvocationID];
    eClimate[gl_InvocationID] = cpClimate[gl_InvocationID];
    if (gl_InvocationID == 0) {
        vec3 camRel = -uCenter.xyz;               // cámara relativa al centro del planeta
        float D     = length(camRel);
        vec3 camDir = camRel / D;
        if (patchHidden(camRel, camDir, uExtra.w,
                        cpPos[0], cpPos[1], cpPos[2], cpPos[3])) {
            // Niveles de tesela 0 → el parche no se genera. Es la vía estándar de culling en tesc.
            gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = gl_TessLevelOuter[3] = 0.0;
            gl_TessLevelInner[0] = gl_TessLevelInner[1] = 0.0;
            return;
        }
        if (frustumHidden(cpPos[0], cpPos[1], cpPos[2], cpPos[3])) {
            // Esfera envolvente entera fuera del frustum → invisible. Esfera conservadora (radio =
            // centroide→vértice + relief), así que solo cull cuando de verdad no se ve.
            gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = gl_TessLevelOuter[3] = 0.0;
            gl_TessLevelInner[0] = gl_TessLevelInner[1] = 0.0;
            return;
        }
        // ── EL PARCHE ESTÁ ENTERO BAJO EL CLIPMAP → DESCARTARLO AQUÍ ────────────────────────────
        //
        // ⚠️ ESTE ES EL ARREGLO DE LAS FRANJAS QUE CRUZABAN LA PANTALLA.
        //
        // El recorte vivía en `terrain.tese`, que empujaba el vértice fuera del NDC:
        //     gl_Position = inClip ? vec4(2.0, 2.0, 2.0, 1.0) : ...
        // y eso NO descarta nada. Un triángulo con parte de sus vértices dentro y parte expulsados
        // se sigue rasterizando: la GPU lo recorta contra el frustum y pinta lo que queda, ESTIRADO
        // entre los vértices reales y los de fuera. Con parches de ~39 km el resultado es una franja
        // que cruza la pantalla, y había una por cada parche que cruzara el borde de la caja — de ahí
        // que se vieran "muchísimas". Verificado: quitando la teselación de la base desaparecían.
        //
        // Un parche se descarta o no se descarta, ENTERO. Ese es el único sitio donde el recorte es
        // correcto, y es la misma vía (niveles a 0) que ya usan `patchHidden` y `frustumHidden`.
        //
        // Se exige que las CUATRO esquinas estén dentro: un parche a caballo del borde sigue
        // dibujándose y se solapa con el clipmap. Solapar cuesta algo de sobredibujo; el otro
        // extremo —recortar de más— deja agujeros, que es peor. El clipmap gana por sesgo de
        // profundidad, así que el solape no se ve.
        if (uDebug.w > 0.5) {
            vec3 upv = normalize(-uCenter.xyz);          // MISMO marco que el tese
            vec3 tu  = cross(vec3(0, 1, 0), upv);
            if (length(tu) < 1e-6) tu = cross(vec3(1, 0, 0), upv);
            tu = normalize(tu);
            vec3 tv = cross(upv, tu);
            float cover = uClipCover.x;
            bool allIn = true;
            for (int i = 0; i < 4; ++i) {
                vec3 rel = cpPos[i] + uCenter.xyz;       // esquina relativa a la cámara
                if (abs(dot(rel, tu)) > cover || abs(dot(rel, tv)) > cover) { allIn = false; break; }
            }
            if (allIn) {
                gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = 0.0;
                gl_TessLevelOuter[2] = gl_TessLevelOuter[3] = 0.0;
                gl_TessLevelInner[0] = gl_TessLevelInner[1] = 0.0;
                return;
            }
        }

        // Orden de gl_TessLevelOuter en un quad: 0 = arista v0-v3, 1 = v0-v1, 2 = v1-v2, 3 = v2-v3.
        // Equivocarlo no da grietas —los factores siguen siendo simétricos— pero sí subdivide en la
        // dirección contraria, y el síntoma es "se ve mal" sin nada que lo explique.
        float e0 = edgeFactor(cpPos[3], cpPos[0]);
        float e1 = edgeFactor(cpPos[0], cpPos[1]);
        float e2 = edgeFactor(cpPos[1], cpPos[2]);
        float e3 = edgeFactor(cpPos[2], cpPos[3]);
        gl_TessLevelOuter[0] = e0; gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2; gl_TessLevelOuter[3] = e3;
        // El interior es el máximo de las aristas: si fuera menor, el parche se rompería por dentro
        // al no poder conectar aristas más finas que su relleno.
        gl_TessLevelInner[0] = max(e1, e3);
        gl_TessLevelInner[1] = max(e0, e2);
    }
}
