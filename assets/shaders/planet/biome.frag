#version 460 core
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec3 vNorm; layout(location = 1) in vec3 vFragPos; layout(location = 2) in vec3 vColor; layout(location = 3) in vec2 vUv; layout(location = 4) in vec3 vClimate;
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra;
    vec4 uDebug; // x = vista de depuración: 0=normal, 1=elev, 2=zonas, 3=bioma, 4=temp, 5=humedad
    // xyz = posición de la CÁMARA en el marco del planeta, REDUCIDA módulo `uExtra.y` (el tile) en
    // doubles por la CPU. Sirve para anclar el patrón de textura al planeta sin reconstruir nunca una
    // coordenada de 6,37e6 en el fragmento: ver la nota larga de `planet.cpp`.
    vec4 uTexAnchor;
};
// ⚠️ LAS OCHO TEXTURAS SUELTAS DE BIOMA SE FUERON (bindings 1-8).
//
// Eran el camino anterior a los arrays de material: un sampler por bioma clásico (sand/grass/land/rock
// × albedo/normal). Desde que el terreno elige su textura por CAPA DEL ARRAY, ninguna se muestreaba —
// comprobado por grep — pero seguían declarándose, cargándose y enlazándose: 8 cargas de ~3 s y
// ~176 MB de VRAM por planeta. Residuo de una migración a medias.
//
// La última en caer fue `uSandAlbedo`, y era el caso más claro: cargaba OTRA VEZ el mismo
// `sand_albedo.png` que ya vive en el array. Dos rutas al mismo fichero y dos verdades sobre cuál es
// la arena. Ahora la orilla usa `uMatCount.z`, la capa que la TABLA declara como orilla — el índice
// sale de quien asigna las capas, no de un literal que se rompería al reordenar los materiales.
layout(binding = 10) uniform sampler2D uMacroVar;
layout(binding = 11) uniform sampler2D uBiomeMap;
// ARRAYS de terreno: una capa por material. El `tile` de la tabla es el ÍNDICE DE CAPA, así que
// elegir textura deja de ser una cadena de `if` sobre samplers (GLSL no los indexa dinámicamente).
layout(binding = 12) uniform sampler2DArray uTerrainAlbedo;
layout(binding = 13) uniform sampler2DArray uTerrainNormal;
// MAPA DE ZONAS equirectangular (paleta). uExtra.z > 0.5 = existe.
layout(binding = 14) uniform sampler2D uZoneMap;

// Bake de altura (R32F, metros, binding 16): lo leen terrain.tese, clipmap.tese y water.frag con la
// bilineal manual compartida (harukaSampleHeightField). Es el MISMO sustrato que pinta la malla y que
// pisa la física, así que el per-pixel se re-ancla sobre él (paridad CPU↔GPU, §5).
layout(binding = 16) uniform sampler2D uHeightTex;
// ClipParams (UBO binding 13): el MISMO marco tangente y semi-lado que cálcula la CPU para el clipmap.
// El per-pixel se descarta DENTRO de esa caja: allí la rejilla fina ya da el detalle y repintarlo
// descosería el sesgo de profundidad que la hace ganar (§2.5). Fuera quedan todos los píxeles de la
// malla base, que es exactamente el rango que la Fase 2 quiere llenar. El espacio de binding de UBO
// y de textura es distinto en GL, así que no choca con el sampler2DArray de la unidad 13.
layout(std140, binding = 13) uniform ClipParams {
    vec4 uClipOrigin; vec4 uClipTanU; vec4 uClipTanV; vec4 uClipCover;
};

#include "lib/terrain_material.glsl"
#include "lib/prop_layer_debug.glsl"
// La función de altura COMPARTIDA con C++ (harukaTerrainDetail y compañía): el per-pixel evalúa
// exactamente lo mismo que la malla y la física, solo que con un triM de PÍXEL (§9, Fase 2).
#include "lib/terrain_detail.glsl"
layout(location = 0) out vec4 fragColor;

vec2 equirectUV(vec3 dir) {
    vec3 d = normalize(dir);
    return vec2(0.5 + atan(d.z, d.x) * 0.1591549,
                0.5 - asin(clamp(d.y, -1.0, 1.0)) * 0.3183099);
}

vec3 triplanar(sampler2D tex, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    return texture(tex, wp.zy * s).rgb * bw.x
         + texture(tex, wp.xz * s).rgb * bw.y
         + texture(tex, wp.xy * s).rgb * bw.z;
}

// Triplanar de NORMAL MAP en "whiteout blend": se suman las desviaciones tangenciales de las tres
// proyecciones, no los vectores ya girados. Mezclar normales de mundo las promedia hacia la normal
// geometrica y aplana el relieve justo en las caras diagonales, que son media esfera.
// Los `u*Normal` estaban DECLARADOS y no se muestreaban nunca: media VRAM de terreno sin usar.
// Triplanar sobre una CAPA del array: idéntico al de sampler2D, con la capa en la 3ª coordenada.
vec3 triplanarArr(sampler2DArray tex, float layer, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    return texture(tex, vec3(wp.zy * s, layer)).rgb * bw.x
         + texture(tex, vec3(wp.xz * s, layer)).rgb * bw.y
         + texture(tex, vec3(wp.xy * s, layer)).rgb * bw.z;
}

vec3 triplanarArrNrm(sampler2DArray tex, float layer, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    vec3 tx = texture(tex, vec3(wp.zy * s, layer)).xyz * 2.0 - 1.0;
    vec3 ty = texture(tex, vec3(wp.xz * s, layer)).xyz * 2.0 - 1.0;
    vec3 tz = texture(tex, vec3(wp.xy * s, layer)).xyz * 2.0 - 1.0;
    return vec3(0.0, tx.y, tx.x) * sign(n.x) * bw.x
         + vec3(ty.x, 0.0, ty.y) * sign(n.y) * bw.y
         + vec3(tz.x, tz.y, 0.0) * sign(n.z) * bw.z;
}

vec3 triplanarNrm(sampler2D tex, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    vec3 tx = texture(tex, wp.zy * s).xyz * 2.0 - 1.0;
    vec3 ty = texture(tex, wp.xz * s).xyz * 2.0 - 1.0;
    vec3 tz = texture(tex, wp.xy * s).xyz * 2.0 - 1.0;
    return vec3(0.0, tx.y, tx.x) * sign(n.x) * bw.x
         + vec3(ty.x, 0.0, ty.y) * sign(n.y) * bw.y
         + vec3(tz.x, tz.y, 0.0) * sign(n.z) * bw.z;
}

// Rampa de color para las vistas de depuración: azul → cian → verde → amarillo → rojo.
vec3 heatmap(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 c = mix(vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), smoothstep(0.00, 0.25, t));
    c = mix(c, vec3(0.0, 1.0, 0.0), smoothstep(0.25, 0.50, t));
    c = mix(c, vec3(1.0, 1.0, 0.0), smoothstep(0.50, 0.75, t));
    c = mix(c, vec3(1.0, 0.0, 0.0), smoothstep(0.75, 1.00, t));
    return c;
}

// Color CATEGÓRICO por capa de textura (tile). Fijo para que "hierba" sea siempre la misma
// mancha verde en cualquier planeta; capas sin asignar de la paleta no se usan.
vec3 layerColor(int l) {
    vec3 pal[16] = vec3[16](
        vec3(0.95, 0.80, 0.35),   // 0 arena
        vec3(0.30, 0.65, 0.25),   // 1 hierba
        vec3(0.55, 0.40, 0.25),   // 2 tierra
        vec3(0.55, 0.55, 0.55),   // 3 roca
        vec3(0.70, 0.30, 0.20),   // 4
        vec3(0.80, 0.60, 0.90),   // 5
        vec3(0.20, 0.60, 0.70),   // 6
        vec3(0.90, 0.45, 0.55),   // 7
        vec3(0.45, 0.55, 0.35),   // 8
        vec3(0.85, 0.70, 0.50),   // 9
        vec3(0.55, 0.35, 0.60),   // 10
        vec3(0.65, 0.70, 0.30),   // 11
        vec3(0.80, 0.50, 0.30),   // 12
        vec3(0.40, 0.40, 0.55),   // 13
        vec3(0.75, 0.65, 0.40),   // 14
        vec3(0.60, 0.80, 0.80)    // 15
    );
    return pal[clamp(l, 0, 15)];
}

// Color de un MATERIAL en las vistas de capas. El matIdx es la posición en `surface.materials`
// (NO el tile: incluye agua/hielo, que no tienen textura). Se prefiere el color DECLARADO por el
// material (agua azul, hielo casi blanco, arena…) y sin él se cae a la paleta categórica.
vec3 matDebugColor(int i) {
    if (i < 0) return vec3(0.10, 0.10, 0.12);
    vec4 mc = uMat[i].e;
    return mc.a > 0.001 ? mc.rgb : layerColor(i);
}

// Curva del fragmento hacia la superficie FINA (Fase 2 del §9 de TERRENO.md).
//
// La malla base (pasada A) dibuja su relieve fino según el triM del TRIÁNGULO (§3.1): fuera del
// clipmap, triM = camD·0.012 mata las octavas de 22 m y 4,5 m por encima de alguna distancia. Aquí
// el fragmento vuelve a evaluar la MISMA `harukaTerrainDetail` pero con el triM del PÍXEL, así que
// el tamaño de muestra deja de ser el triángulo (que no puede bajar de cierto valor) y pasa a ser
// lo que abarca un píxel: el freno de Nyquist deja de aplicarse y el relieve fino vuelve al shading.
//
// No se generan vértices: el re-anclado es un raymarch CORTO (sphere-trace) a lo largo del rayo de
// la vista, desde la posición gruesa del triángulo hasta cruzar la superficie fina
// `R + baseH + detail(pixelTriM)`. El nº de pasos es LOD por distancia (8-16 cerca de la cámara,
// 2-4 lejos, según el §9), con salida temprana cuando se converge (dR < 5 cm). El clipmap NO se
// toca: dentro de su caja la rejilla fina ya da el relieve real, y volver a entrar descosería el
// sesgo de profundidad que la hace ganar (§2.5).
// triM del PÍXEL: lo que abarca un píxel en el suelo a esta distancia. GEMELO de `terrainTriM`
// (core/planet/terrain_lod.h) — misma pendiente y mismo piso que usan el clipmap y la colisión.
//
// ⚠️ El piso era 0.5 "para que el coste no explote en el píxel de 0 m", y era código MUERTO: este
// camino solo corre FUERA de la caja del clipmap, o sea con t0 ≥ 1984 m, donde t0·0.002 ≥ 3.97. El
// piso nunca se alcanzaba. Puesto en el valor compartido, el comportamiento es idéntico y deja de
// haber un cuarto número de LOD escrito a mano.
float harukaPixelTriM(float t0) { return max(t0 * 0.002, 4.0); }

// ── MARCO DEL PLANETA SIN CANCELACIÓN CATASTRÓFICA ──────────────────────────────────────────────
//
// Todo este shader necesita dos cosas del punto que está sombreando: su DIRECCIÓN desde el centro del
// planeta y su COTA sobre la esfera. La forma evidente de sacarlas —`p = fragP - uCenter`, luego
// `normalize(p)` y `length(p) - R`— es la que producía el ruido del suelo, y no por poco:
//
//   · `fragP - uCenter` resta dos floats de ~6,37e6, así que el resultado se REDONDEA a un ulp de
//     0,76 m. Y el redondeo depende de los bits bajos de `fragP`, o sea que cambia de un píxel al de
//     al lado sin ninguna relación con la geometría: no es un error, es DITHER.
//   · `length(p) - R` es cancelación catastrófica: 6,37e6 − 6,371e6. La cota hereda el ulp del
//     length(), ~0,8 m, otra vez dithered por píxel.
//
// Nada de eso se ve mientras el resultado solo se compare con cosas grandes. Se ve en cuanto entra en
// una COMPARACIÓN FINA, y aquí hay dos: la penumbra de la sombra de terreno (12 m → 0,8 m de dither
// son un 7 % de brillo aleatorio por píxel) y el gradiente del detalle procedural, que con octavas de
// 8 m de longitud de onda convierte 0,4 m de dither en una normal completamente distinta. De ahí la
// sal y pimienta: es ruido de SOMBREADO, no de textura. (Y por eso mi primer arreglo, el de las UV,
// no lo tocó: las texturas del suelo son lisas, no pueden aliasear así.)
//
// La cura es no formar nunca la posición planetaria. Se pasa a trabajar con:
//   Ahat  dirección centro→cámara, UNITARIA (su error es constante del frame, no varía por píxel)
//   r0    radio de la cámara            } vienen de la CPU en double: exactos y constantes
//   camAlt altitud de la cámara         }
// y el punto se describe con su desplazamiento `rel` respecto a la cámara, que es pequeño y preciso.
//
//   dirección:  p/|p| = normalize(Ahat + rel/r0)      ← todo de magnitud ~1, y SUAVE entre píxeles
//   cota:       |p|-R = (|p|²-R²)/(|p|+R)
//               |p|²-R² = camAlt·(r0+R) + 2·dot(A,rel) + |rel|²   ← ni una resta de números grandes
//
// El truco de la cota es la identidad a²-b² = (a-b)(a+b) usada al revés: convierte la resta que se
// cancela en un producto que no. Cada término se calcula con error relativo de float sobre su propia
// magnitud, así que la cota sale con precisión de MICRAS en vez de metros.
//
// Lo que queda es un residuo irreducible: `normalize` devuelve un vector unitario con ulp de 6e-8, y
// eso son 0,38 m sobre la superficie de la Tierra (el tope del §5). Pero al venir de una expresión
// suave, el redondeo es una ESCALERA suave de 0,38 m, no dither — y una escalera sub-píxel no se ve.
struct PlanetFrame { vec3 ahat; float r0; float camAlt; float R; };

PlanetFrame planetFrame() {
    PlanetFrame f;
    f.R      = uExtra.w;
    vec3 A   = -uCenter.xyz;             // cámara − centro del planeta
    f.r0     = length(A);                // constante del frame: su ulp NO es ruido por píxel
    f.ahat   = A / f.r0;
    f.camAlt = uTexAnchor.w;             // en DOUBLE desde la CPU: `r0 - R` aquí se cancelaría
    return f;
}

/// Dirección desde el centro del planeta del punto a `rel` de la cámara. Suave por construcción.
vec3 frameDir(PlanetFrame f, vec3 rel) { return normalize(f.ahat + rel / f.r0); }

/// Cota sobre la esfera de referencia del punto a `rel` de la cámara, sin cancelación.
float frameAlt(PlanetFrame f, vec3 rel) {
    float num = f.camAlt * (f.r0 + f.R) + 2.0 * dot(f.ahat * f.r0, rel) + dot(rel, rel);
    // |p|+R = 2R+alt. Dos pasadas: la primera estima `alt` para cerrar el denominador. El error de la
    // estimación entra dividido por 2R, o sea que la segunda pasada ya da micras.
    float alt0 = num / (2.0 * f.R + f.camAlt);
    return num / (2.0 * f.R + alt0);
}

vec3 reanchorToFine(vec3 rayDir, float t0) {
    float pixelTriM = harukaPixelTriM(t0);
    int steps = clamp(int(round(mix(16.0, 2.0, smoothstep(3000.0, 25000.0, t0)))), 2, 16);
    PlanetFrame f = planetFrame();
    float t = t0;
    for (int i = 0; i < 16; ++i) {
        if (i >= steps) break;
        vec3  rel = rayDir * t;
        vec3  c   = frameDir(f, rel);            // suave; antes: normalize(p - uCenter) → dither
        float alt = frameAlt(f, rel);            // exacta;  antes: length(p - uCenter) - R
        float baseH = harukaSampleHeightField(uHeightTex, textureSize(uHeightTex, 0), harukaEquirectUV(c));
        float baseR = f.R + baseH;
        float att = harukaSeaLevelAttenuation(baseH);
        float h   = harukaTerrainDetail(c, baseR, pixelTriM) * att;
        if (baseH > 0.0) h = max(h, -baseH);           // la tierra no baja del nivel del mar (paridad)
        float grad  = dot(rayDir, c);
        // El residuo del sphere-trace es ahora una resta entre COTAS (metros), no entre radios
        // (6,37e6). Sin eso el residuo estaba dominado por el ulp de `length` (~0,8 m) y la
        // condición de convergencia de 5 cm era INALCANZABLE: el bucle gastaba siempre los 16 pasos
        // y aterrizaba en una `t` con metros de dither, que es lo que reventaba la normal.
        float delta = ((baseH + h) - alt) / (abs(grad) > 1e-3 ? grad : 0.05);
        delta = clamp(delta, -t0 * 0.1, t0 * 0.1);      // sin saltos grandes
        t += delta;
        if (abs(delta) < 0.05) break;
    }
    return rayDir * t;
}

// ── SOMBRAS DE TERRENO POR RAY-MARCH ────────────────────────────────────────────────────────────
//
// El mapa de sombras del motor cubre una caja de ±42 m alrededor del jugador y solo contiene lo que
// dibuja el hook del juego: el TERRENO no está ahí. Por eso una loma no proyecta sombra y solo se
// notan las sombras cuando el sol está bajo (las de los pocos objetos que sí entran, alargadas).
//
// Meter el terreno en ese mapa no vale: habría que redibujar el clipmap en el pase de sombras, y una
// caja de 84 m no puede contener la montaña que proyecta la sombra. Aquí sobra el mapa: el fragmento
// ya tiene la FUNCIÓN de altura, así que se marcha hacia el sol y se pregunta si el terreno tapa.
// Sin límite de caja — es lo que da las sombras largas de una cordillera al amanecer.
//
// El paso crece geométricamente: cerca importa la resolución, lejos importa el alcance. Con 24 pasos
// y ×1.35 se cubren ~4 km desde 8 m. Y se sale en cuanto el rayo se eleva por encima del techo de
// relieve local, que es la salida que hace esto barato en terreno llano.
// ⚠️ EL RAYO ARRANCA EN EL CAMPO BASE, no en la superficie con detalle.
//
// Esto fue un bug mío y se veía como MEDIO PLANETA EN NEGRO con bordes duros. Al recortar el coste
// hice que la sombra muestreara solo el campo base (`th`), pero seguía comparándolo con la altura de
// la superficie REAL, que lleva el detalle. Donde el detalle es negativo —la mitad de los puntos— la
// resta `th − pAlt` sale positiva hasta +151 m, y con un smoothstep de 12 m eso es oclusión TOTAL:
// cada valle se sombreaba a sí mismo.
//
// Los dos lados tienen que vivir en el mismo campo. Arrancando en `R + baseH` la comparación es
// coherente y la sombra es lo que dice ser: la que proyecta el RELIEVE GRANDE (montañas y crestas),
// que es justo lo que un mapa de sombras local no puede dar.
float terrainShadow(vec3 dirRad, vec3 up, vec3 L, float R, float baseH) {
    // De espaldas al sol no hace falta marchar: ya está en penumbra por el difuso.
    float ndl = dot(up, L);
    if (ndl <= 0.02) return 1.0;

    // ⚠️ COSTE. Esto corre POR PÍXEL y a pie el suelo es casi toda la pantalla. La primera versión
    // hacía 24 pasos × `harukaTerrainDetail` (5 octavas × 8 hashes) = ~1000 operaciones por píxel, y
    // medido con vsync APAGADO eso puso `present.swap` en 42 ms: la GPU pasó a ser el cuello del
    // frame. Dos recortes, los dos sin pérdida visible:
    //
    //  1. La sombra usa SOLO EL CAMPO BASE (una bilineal del bake), no el detalle. Lo que proyecta una
    //     sombra a cientos de metros es la montaña, no la octava de 22 m: el detalle aporta metros de
    //     relieve sobre un rayo que recorre kilómetros. Pasa de ~45 operaciones por paso a 4 lecturas.
    //  2. 16 pasos en vez de 24, con más crecimiento para conservar el alcance (~4 km).
    const int   kSteps = 16;
    const float kStep0 = 10.0;     // primer paso (m): por debajo, el ruido de la propia superficie
    const float kGrow  = 1.45;     // crecimiento geométrico → ~4 km de alcance con 16 pasos
    // ⚠️ SESGO PROPORCIONAL A LA DISTANCIA, no fijo. El campo base se muestrea a ~4,9 km por téxel,
    // así que a 4 km de marcha la incertidumbre de la altura es de decenas de metros: un sesgo de 2 m
    // convierte cualquier ondulación del propio terreno en sombra. El 1,2% de la distancia da 2 m al
    // empezar y 48 m a 4 km, que es del orden del error del propio campo.
    const float kBias0 = 2.0, kBiasK = 0.012;

    // Punto de partida en el campo BASE, en la dirección radial del fragmento.
    //
    // ⚠️ La cota del punto del rayo NO se calcula como `length(surfW + L*t) - R`. Ese era el segundo
    // foco del ruido del suelo, y el peor de los dos porque esta función corre para TODOS los
    // fragmentos (dentro y fuera del clipmap): `surfW` mide 6,37e6, así que la suma se redondea a
    // 0,76 m según los bits bajos del píxel, y el `length()−R` de después es cancelación catastrófica.
    // El resultado entraba directo en `smoothstep(0, 12 m, ...)`: 0,8 m de dither sobre una penumbra
    // de 12 m = un 7 % de brillo aleatorio por píxel. Sal y pimienta en toda la pantalla.
    //
    // Aquí la geometría es un rayo desde un punto conocido, así que la cota sale en cerrado:
    //   |p|² − R² = (r0² − R²) + 2·r0·t·cos + t²   con r0 = R + baseH, cos = dot(dirRad, L)
    //   r0² − R²  = baseH·(2R + baseH)             ← la resta que se cancelaba, factorizada
    // Todos los términos son productos de magnitudes moderadas: la cota sale con error de micras.
    float cosRL = dot(dirRad, L);
    float r0    = R + baseH;
    float base2 = baseH * (2.0 * R + baseH);       // r0² − R², sin cancelación
    float t = kStep0, step = kStep0;
    float occ = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        float num  = base2 + 2.0 * r0 * t * cosRL + t * t;
        float alt0 = num / (2.0 * R + baseH);      // estimación para cerrar el denominador |p|+R
        float pAlt = num / (2.0 * R + alt0);
        // Solo la DIRECCIÓN, y sacada de números de magnitud ~1: `p = r0·(dirRad + L·t/r0)`, y el
        // factor r0 no afecta a normalize. Así el muestreo del campo tampoco va dithered.
        vec3  pd   = normalize(dirRad + L * (t / r0));
        // Solo el campo base: ver el recorte (1) de arriba.
        float th = harukaSampleHeightField(uHeightTex, textureSize(uHeightTex, 0),
                                           harukaEquirectUV(pd));
        th = max(th, 0.0);   // la tierra no baja del nivel del mar (paridad con el resto)
        // Si el rayo va por debajo del terreno, está tapado. Penumbra suave con la profundidad para
        // que el borde de la sombra no sea un escalón de un solo paso.
        // La penumbra también escala con la distancia: 12 m fijos son un escalón binario a escala
        // kilométrica, y el borde de la sombra sale como un filo recortado.
        float bias  = kBias0 + t * kBiasK;
        float penum = 12.0 + t * 0.05;
        occ = max(occ, smoothstep(0.0, penum, th - pAlt + bias));
        if (occ >= 0.999) break;
        // Salida barata: por encima del techo de relieve del entorno ya no puede tapar nada.
        if (pAlt - th > 900.0) break;
        step *= kGrow;
        t    += step;
    }
    return 1.0 - occ;
}

void main() {
    vec3 n = normalize(vNorm);
    float diff = max(dot(n, normalize(uLightDir.xyz)), 0.0);
    float tiling = uExtra.y;

    // `vFragPos` es entrada (solo lectura); el per-pixel re-ancla la posición a la superficie fina,
    // así que el resto del main trabaja sobre la copia local `fragP`.
    vec3 fragP = vFragPos;

    // Radial direction (from planet center to surface point). Por el marco, no restando `uCenter`:
    // esta `up` es la que va a `terrainShadow` como dirección radial y a `equirectUV` para los mapas.
    PlanetFrame pf = planetFrame();
    vec3 up = frameDir(pf, fragP);
    float elev = vClimate.x; // km
    {
        // PER-PIXEL (Fase 2, §9): re-ancla el fragmento a la superficie fina ANTES de triplanar e
        // iluminar. Solo fuera de la caja del clipmap (`!inClip`, Mismo test que terrain.tese) y con
        // bake de altura presente. El resultado: el grano fino (octavas de 22 m / 4,5 m) llega hasta
        // el horizonte, sin la línea de corte a 2 km de donde acaba el clipmap. Coste por píxel de
        // la base, no por vértice teselado; los pasos del sphere-trace son LOD por distancia (§9).
        // ⚠️ El corte de distancia NO elimina la línea donde acaba el relieve fino: la MUEVE del
        // borde del clipmap (~2 km) hasta aquí. A altura de ojo el horizonte está a 4,65 km, así que
        // no se ve; desde altitud sí (a 2 km de altura el horizonte está a ~160 km). Es el límite
        // aceptado de la Fase 2, y el número está atado a eso, no elegido al azar: 20 km es el
        // horizonte desde ~31 m de altura.
        if (uDebug.z > 0.5 && length(fragP) < 20000.0) {
            vec3 rel = fragP;
            // Marco tangente: MISMA cuenta que `terrainClipFrame` (core/planet/terrain_lod.h), que
            // es la que llena ClipParams y la que construye la malla de colisión. Aquí había una
            // tercera copia a mano; el eje auxiliar se elige ANTES de normalizar porque en el polo
            // el producto vectorial es cero y normalizarlo daría NaN, no un vector corto.
            vec3 upv = normalize(-uCenter.xyz);
            vec3 tu = cross(vec3(0, 1, 0), upv);
            if (length(tu) < 1e-6) tu = cross(vec3(1, 0, 0), upv);
            tu = normalize(tu);
            vec3 tv = cross(upv, tu);
            // Solo se lee uClipCover si el clipmap está activo este frame (uDebug.w): si no, el UBO de
            // binding 13 puede no estar enlazado y leerlo es indefinido.
            bool inClip = false;
            if (uDebug.w > 0.5) {
                float cover = uClipCover.x;
                inClip = abs(dot(rel, tu)) <= cover && abs(dot(rel, tv)) <= cover;
            }
            if (!inClip) {
                float dist0 = length(fragP);
                vec3 rayDir = fragP / dist0;
                vec3 p = reanchorToFine(rayDir, dist0);
                // Por el marco: esta `dir` es la que alimenta el GRADIENTE del detalle, y es donde
                // 0,4 m de dither se convierten en una normal aleatoria (octavas de 8 m).
                vec3 dir = frameDir(pf, p);
                float baseH = harukaSampleHeightField(uHeightTex, textureSize(uHeightTex, 0), harukaEquirectUV(dir));
                float baseR = uExtra.w + baseH;
                float att = harukaSeaLevelAttenuation(baseH);
                // UN solo triM para la altura Y para el paso de las diferencias finitas. Estaban
                // escritos por separado (piso 0.5 la altura, 2.0 el paso): coincidían de milagro,
                // porque fuera de la caja ninguno de los dos pisos se alcanza. Con dos expresiones
                // distintas, tocar una habría dado una normal que no describe la altura que sombrea.
                // Altura Y GRADIENTE en UNA evaluación, igual que los tess eval (§8). Aquí el ahorro
                // pesa el triple: esto corre POR PÍXEL de la malla base, no por vértice.
                float triM = harukaPixelTriM(dist0);
                vec3 grad;
                float hFine = harukaTerrainDetailGrad(dir, baseR, triM, grad) * att;
                grad *= att;
                if (baseH > 0.0 && hFine < -baseH) { hFine = -baseH; grad = vec3(0.0); }
                vec3 t1 = normalize(abs(dir.y) < 0.99 ? cross(dir, vec3(0,1,0)) : cross(dir, vec3(1,0,0)));
                vec3 t2 = cross(dir, t1);
                n = normalize(dir - t1 * dot(grad, t1) - t2 * dot(grad, t2));
                diff = max(dot(n, normalize(uLightDir.xyz)), 0.0);
                // Re-anclamos fragP y `up` para que el triplanar siga el relieve FINO.
                //
                // ⚠️ `elev` NO se re-ancla: se queda en la base del bake, igual que en los dos tess
                // eval. Es lo que decide la costa, y la costa es del mapa base. Sobrescribirlo aquí
                // con `baseH + hFine` hacía que el material cambiara al cruzar el borde del clipmap
                // (dentro se usaba la base, fuera la superficie fina), que es el salto hierba/arena.
                fragP = p;
                up = dir;
            }
        }
    }

    // ── COTA POR PÍXEL, no interpolada del vértice. ─────────────────────────────────────────────
    //
    // ⚠️ `vClimate.x` se calcula por VÉRTICE TESELADO y se interpola por el triángulo. Eso hace que la
    // misma cota se muestree con dos resoluciones muy distintas:
    //   · dentro de la caja del clipmap → vértices cada 4 m: sigue el bake fielmente.
    //   · fuera                         → vértices cada ~305-650 m: el bake ESTIRADO linealmente.
    // La banda de arena de la orilla es estrecha (−30 … +12 m), así que esa diferencia decide arena
    // sí/no a cada lado del borde — y el borde del clipmap es un CUADRADO, de ahí la línea RECTA de
    // arena. Leyendo el bake por píxel el valor es idéntico en las dos pasadas, la costura desaparece
    // y de paso la costa deja de estar difuminada sobre cientos de metros.
    //
    // Es el mismo sustrato que leen los dos tess eval, el agua y la física: una sola descripción.
    if (uDebug.z > 0.5)
        elev = harukaSampleHeightField(uHeightTex, textureSize(uHeightTex, 0),
                                       harukaEquirectUV(up)) * 0.001;

    // LOD POR DISTANCIA DEL FRAGMENTO: el detalle de textura (triplanar, normal map, arena) es
    // subpíxel a más de ~500 m — muestrearlo es coste sin resultado. Se apaga suavemente; el color
    // del bioma (mapa horneado) queda, que es lo que se ve de lejos. El rango (300 m – 2,5 km) está
    // pensado para el JUEGO A PIE, que es donde el fragmento pesaba: casi toda la pantalla es suelo
    // a 0-4 km, y el triplanar en la mitad lejana era subpíxel pero se pagaba entero. En órbita todo
    // supera 2,5 km → lod 0 → solo el color del bioma, que es lo único visible a esa distancia.
    float lod = 1.0 - smoothstep(300.0, 2500.0, length(fragP));

    // Sample biome map for classification + color
    vec2 bUV = equirectUV(up);
    vec4 biomeSample = texture(uBiomeMap, bUV);
    vec3 biomeCol = biomeSample.rgb;
    float H = biomeSample.a; // biome index / humidity [0,1]

    // Rock override by slope
    float slope = 1.0 - dot(n, up);
    float rock = smoothstep(0.55, 0.80, slope) + smoothstep(2.0, 5.0, elev);

    // --- SELECCION DE MATERIAL: se recorre la tabla -------------------------------------
    // La humedad y la TEMPERATURA llegan por vertice (`vClimate.yz`) y hasta ahora se ignoraban:
    // el shader leia la humedad del alfa del mapa horneado y no miraba la temperatura en absoluto,
    // asi que no podia distinguir una taiga de una pradera.
    float humid = clamp(vClimate.z, 0.0, 1.0);
    float tempC = vClimate.y;

    // El mapa de zonas se lee en el MISMO UV equirectangular que el mapa de biomas: los dos
    // describen el mismo planeta y si divergieran, el color diría una cosa y el material otra.
    // SIN ruido: la geometría (sampleHeight, nearest sin jitter) usa el mismo texel, así que el
    // material y el borde del agua coinciden. Perturbar aquí con ruido descasaba el material del
    // terreno (un texel ≈ 20 km a 2048) y pintaba agua azul sobre tierra en la franja de la costa.
    bool hasZoneMap = uExtra.z > 0.5;
    vec3 zoneRGB = hasZoneMap ? texture(uZoneMap, bUV).rgb * 255.0 : vec3(0.0);

    vec3  tint; float grainAmt, detailAmt; int tile; vec4 matColor; int matIdx;
    // `elev` es la cota por PÍXEL leída del bake (km): el cuarto eje de la selección.
    harukaSelectMaterial(humid, tempC, slope, elev, zoneRGB, hasZoneMap,
                         tint, grainAmt, detailAmt, tile, matColor, matIdx);
    // El color PROPIO del material sustituye al del bioma según su peso. Con peso 0 (o sin color
    // declarado) manda el clima, que es el comportamiento de siempre.
    biomeCol = mix(biomeCol, matColor.rgb, matColor.a);

    // EL SUELO DEL OCÉANO NO SE PINTA DE MATERIAL DE AGUA. El agua es la ESFERA 3D (water.frag); el
    // material 0 de la tabla siempre gana bajo el nivel del mar y dibujaba una "capa de agua" azul
    // sobre la textura del planeta — un segundo mar horneado que ni se ve (la esfera lo tapa) ni se
    // quita al generar otro planeta, y que asoma con HARUKA_NOWATER. Donde `elev` (km) < 0 el
    // fragmento está bajo el nivel del mar y debe leerse como FONDO MARINO (arena), no como agua. La
    // transición suaviza justo en la orilla, donde la playa de arena (`sandW`) ya se encarga del
    // borde; el `col` normal sigue pasando por textura/iluminación como el resto del suelo.
    if (elev < 0.0)
        biomeCol = mix(biomeCol, vec3(0.42, 0.38, 0.31), 1.0 - smoothstep(-0.4, 0.0, elev));

    // `tiling` es METROS POR TILE. Estaba usándose al revés (`vFragPos * tiling` con un scale de
    // 0.5 encima) → un tile cada 2 cm: muy por debajo del píxel a cualquier distancia, así que la
    // textura se promediaba a gris plano y no aportaba ni grano ni relieve. De ahí que el terreno
    // se viera liso por mucha resolución que tuvieran los PNG.
    // Anclado al PLANETA, no a la cámara: vFragPos es relativo a la cámara (aPos + uCenter), así que
    // usarlo como coordenada de textura hacía que el patrón se deslizara con el jugador.
    //
    // ⚠️ Y NO se reconstruye la posición planetaria (`fragP - uCenter`). Esa resta es entre dos
    // floats de ~6,37e6 y su resultado queda cuantizado a un ulp ≈ 0,76 m POR PÍXEL, así que las
    // derivadas que la GPU usa para elegir el mip salen 0 ó 0,76 m en vez del valor real: cada píxel
    // muestreaba un mip arbitrario y el suelo entero era sal y pimienta (ver `planet.cpp`,
    // `uTexAnchor`). `fragP` es relativo a la cámara (metros a km → precisión de milímetros) y el
    // ancla ya viene reducida módulo el tile en doubles: la suma es exacta, el patrón sigue pegado al
    // planeta y las derivadas vuelven a describir el movimiento real de la textura.
    //
    // ⚠️ Todo patrón que use `wp` tiene que tener PERIODO DIVISOR de `tiling`, o el módulo del ancla
    // dejaría de ser invisible y el patrón saltaría al cruzar un múltiplo. De ahí que el grano y la
    // arena de abajo se deriven de `tiling` en vez de llevar su propio número en metros.
    vec3 wp = fragP + uTexAnchor.xyz;
    float tileScale = 1.0 / max(tiling, 0.01);
    // Una sola llamada: el material dice QUE CAPA, no que sampler. tile < 0 = sin textura (hielo,
    // sal, lava vidriada): luminancia neutra -> grano ~1 y superficie lisa.
    // ⚠️ LUMINANCIA MEDIA AUTORIZADA A 0.5. `tools/gen_terrain_textures.py` normaliza cada PNG a esa
    // media exacta con estos MISMOS pesos, así que `tex / kTexMean` tiene media 1.0 y multiplicar por
    // él es neutro en promedio. Antes el número era 0.45, medido a ojo sobre unos ficheros concretos:
    // una constante del shader que dependía del asset, y que se desajustaba al cambiar una textura.
    const float kTexMean = 0.5;
    const vec3  kLumaW   = vec3(0.2126, 0.7152, 0.0722);
    vec3 tex = (tile < 0 || lod < 0.01) ? vec3(kTexMean)
                                       : triplanarArr(uTerrainAlbedo, float(tile), wp, n, tileScale);

    // Macro variation brightness modulation
    vec4 macro = texture(uMacroVar, bUV);
    // El PNG aporta GRANO, no color: el color es el del bioma, que sale del clima. `grainAmt`
    // decide cuanto se nota por material (el hielo casi nada, la roca mas que nadie).
    float grain = mix(1.0, dot(tex, kLumaW) / kTexMean, grainAmt);
    // El ALBEDO SE VE, no solo su luminancia: se mezcla el COLOR de la textura con el del bioma
    // para que el patron (hierba, arena, roca) sea visible. Antes el color plano del material
    // lo tapaba y la textura solo aportaba grano, que es como "no tener texturas".
    float texW = clamp(grainAmt * 0.55, 0.0, 0.75) * lod;
    vec3 col = mix(biomeCol, biomeCol * tex / kTexMean, texW) * tint * clamp(grain, 0.75, 1.25);

    // ── EL GRANO YA NO SE FABRICA AQUÍ: ESTÁ EN LA TEXTURA ──────────────────────────────────────
    //
    // Aquí había una SEGUNDA capa triplanar de la misma imagen muestreada a ~2 m por tile, aplicada
    // como modulación multiplicativa en los primeros 40 m. Existía porque las texturas del proyecto no
    // tenían detalle fino: medido, su contraste a la escala más pequeña era del **0,15-0,32 %** y su
    // espectro se aplanaba a 1,1 por octava — la firma de una imagen reescalada. El suelo salía liso y
    // el shader lo compensaba inventándose la rugosidad.
    //
    // Compensar por código es inestable, y por eso se va: el resultado dependía de la distancia (una
    // banda de fundido de 8-40 m que se ve pasar), del mip que eligiera el driver para un patrón de
    // 2 m, y de la precisión con la que se reconstruyera `wp` — las tres cosas ya han dado problemas
    // en este shader. Un material no cambia de aspecto porque el jugador se acerque.
    //
    // Ahora el detalle lo trae el asset: `tools/gen_terrain_textures.py` genera cada material con un
    // espectro en ley de potencias sostenido hasta Nyquist (contraste fino del 1,0-2,6 %, ~10× más) y
    // un normal map derivado de SU propia altura con la pendiente de cada material. Eso se muestrea
    // UNA vez, con mipmaps y anisotropía, y se ve igual a un metro que a cincuenta.
    //
    // La variación de macro solo en TIERRA: en el mar (liso, sin tiles) su patrón de manchas se
    // veía como círculos grises. step(0,elev) = 1 en tierra, 0 en mar.
    col *= 0.85 + 0.30 * macro.r * step(0.0, elev);

    // Sand overlay at shoreline — a los DOS lados del nivel del mar. `elev` está en km. La rampa del
    // lado del agua sube al acercarse a la superficie (de -30 m a -2 m: arena mojada en la orilla) y
    // la del lado de tierra baja al alejarse (0 a +12 m: playa seca). Antes solo se pintaba bajo el
    // agua y además con la rampa invertida, así que la orilla seca salía con el color del bioma de
    // esa latitud y la costa no se leía como costa; ahora la línea del agua tiene playa en sus dos
    // lados y se ve como costa incluso desde lejos.
    float shoreF = smoothstep(-0.030, -0.002, elev) * (1.0 - smoothstep(0.0, 0.012, elev));
    float sandW = shoreF * (1.0 - smoothstep(0.45, 0.7, slope));
    // ×2 (periodo `tiling`/2), no ×1.4: el factor tiene que DIVIDIR el tiling para que el módulo del
    // ancla de `wp` no se note. Con 1.4 el periodo era 71,4 m contra un ancla de 100 m.
    // La arena de la orilla, desde la CAPA del array que declara la tabla. Con -1 (ninguna declarada)
    // la orilla se queda con su color sin textura: degradación visible, no basura.
    int shoreLayer = int(uMatCount.z);
    if (lod > 0.01 && sandW > 0.001 && shoreLayer >= 0)
        col = mix(col, triplanarArr(uTerrainAlbedo, float(shoreLayer), wp, n, tileScale * 2.0), sandW);

    // RELIEVE del material: la desviacion de su normal map. Solo la parte tangencial (la componente
    // a lo largo de n no inclina nada y si desnormaliza) y con fuerza baja: con sombreado cel un
    // relieve fuerte pica el terminador y saca manchas oscuras.
    vec3 nrmDelta = (tile < 0 || lod < 0.01) ? vec3(0.0)
                                             : triplanarArrNrm(uTerrainNormal, float(tile), wp, n, tileScale);
    if (dot(nrmDelta, nrmDelta) > 1e-8) {
        vec3 dTan = nrmDelta - dot(nrmDelta, n) * n;
        // El relieve del normal map se apaga con la distancia igual que la textura (`lod`): a
        // 500-2500 m es subpíxel y muestrear su desviación a plena fuerza solo mete temblor y paga
        // el mismo coste. Cerca de los pies (`lod`≈1) queda entero; `(0.5 + 0.5·lod)` mantiene al
        // menos la mitad en el plano medio en vez de un corte brusco.
        n = normalize(n + 0.35 * detailAmt * (0.5 + 0.5 * lod) * dTan);
        diff = max(dot(n, normalize(uLightDir.xyz)), 0.0);   // reiluminar con la normal nueva
    }

    // Luz del SOL + CIELO, MISMA respuesta que el agua: `sunD` satura antes (clamp(diff*1.6), igual
    // que water.frag) y con multiplicador más alto, más un destello especular tenue para que el
    // terreno "atrape" el sol igual que el agua. Antes el sol era lineal (×1.3*diff) y a ángulos
    // rasantes la cara de día salía apagada: terreno oscuro y "el sol apenas lo toca" en
    // comparación con el agua, que sí brillaba.
    vec3 sky = vec3(0.09, 0.14, 0.22);
    vec3 L = normalize(uLightDir.xyz);
    vec3 V = -fragP;
    V = length(V) > 1e-6 ? normalize(V) : vec3(0.0, 0.0, 1.0);
    vec3 hh = normalize(L + V);
    float sunD = clamp(diff * 1.6, 0.0, 1.0);

    // ── EL DESTELLO, SOLO DONDE HAY MOTIVO FÍSICO ───────────────────────────────────────────────
    //
    // ⚠️ ANTES SE APLICABA A TODO EL TERRENO (`pow(dot(n,hh), 24) * 0.30`, sin más) y eso es brillo
    // de material MOJADO repartido por el planeta entero. Tierra seca, hierba y arena de playa son
    // casi lambertianas: no tienen un lóbulo especular estrecho.
    //
    // Y no se leía como "el suelo atrapa el sol", que era la intención: `hh` depende de la VISTA, así
    // que el realce se mueve con la cámara, y un lóbulo de exponente 24 sobre un suelo casi plano no
    // cae como una mancha sino como una BANDA — una línea de luz cerca del personaje que barría al
    // girar. Un reflejo que sigue al jugador por el barro no lo produce nada real.
    //
    // Ahora se pesa por los dos materiales que SÍ reflejan así, y los dos ya están calculados arriba:
    //
    //   roca  — pendiente alta, donde el material rocoso domina (misma rampa que usa `sandW` para
    //           excluirse, así que roca y arena no se solapan).
    //   arena MOJADA — solo el lado de AGUA de la orilla. La playa seca (`elev` > 0) queda fuera a
    //           propósito: es lo que la hacía brillar como charco.
    //
    // El sol directo (×1.6) no se toca: el terreno no se apaga, solo deja de estar barnizado.
    float rockW = smoothstep(0.45, 0.75, slope);
    float wetW  = shoreF * (1.0 - smoothstep(-0.004, 0.002, elev));
    float sheenW = clamp(max(rockW, wetW), 0.0, 1.0);
    float sheen = pow(max(dot(n, hh), 0.0), 24.0) * 0.30 * sheenW;

    // SOMBRA DEL TERRENO (uAmbient.w > 0.5 = activada por la CPU; ver HARUKA_TERRAIN_SHADOW).
    // Multiplica solo la contribución DIRECTA del sol: el ambiente y el cielo siguen llegando a una
    // ladera en sombra, que es lo que hace que una sombra parezca sombra y no un agujero negro.
    float shadow = 1.0;
    if (uAmbient.w > 0.5 && uDebug.z > 0.5) {
        float bh = harukaSampleHeightField(uHeightTex, textureSize(uHeightTex, 0),
                                           harukaEquirectUV(up));
        shadow = terrainShadow(up, up, L, uExtra.w, bh);
    }
    sunD  *= shadow;
    sheen *= shadow;
    col = col * (uAmbient.xyz + sky * (0.35 + 0.65 * sunD) + uLightColor.xyz * sunD * 1.6)
        + uLightColor.xyz * sheen;

    // VISTAS DE DEPURACIÓN del editor: colorean el planeta para "ver cómo funciona" el terreno.
    // Se aplican DESPUÉS de la iluminación y SIN texturas: lo que importa es el dato, no el grano.
    int dbg = int(uDebug.x);
    if (dbg == 1) {                       // elevación (km): ±5 km normalizado
        // La elevación REAL del fragmento (radio desde el centro del planeta), no la base del
        // vértice: así la vista de elevación refleja el relieve fino que la malla/clipmap dibujan
        // (el `vClimate.x` es la base del bake, sin las octavas — la vista salía lisa donde la
        // geometría tenía relieve, y eso despistaba al autor).
        float realElevKm = frameAlt(pf, fragP) * 0.001;   // sin la cancelación de `length(...) - R`
        col = heatmap(realElevKm * 0.1 + 0.5);
    } else if (dbg == 2) {                // zonas del autor (paleta del mapa)
        col = hasZoneMap ? zoneRGB / 255.0 : vec3(0.3, 0.3, 0.35);
    } else if (dbg == 3) {                // bioma clasificado (mapa horneado)
        col = biomeCol;
    } else if (dbg == 4) {                // temperatura (°C): ±40 °C normalizado
        col = heatmap(tempC / 80.0 + 0.5);
    } else if (dbg == 5) {                // humedad [0,1]
        col = heatmap(humid);
    } else if (dbg == 6) {                // CAPAS: TODAS a la vez, cada material con su color
        col = matDebugColor(matIdx);
    } else if (dbg >= 10 && dbg < 40) {    // CAPA i: solo donde manda ese material (máscara).
        // El índice es la POSICIÓN en `surface.materials` (0=agua, 1=arena…), no el tile: así
        // los materiales sin textura (agua, hielo) también se pueden aislar.
        int layer = dbg - 10;
        col = (matIdx == layer) ? matDebugColor(layer) : vec3(0.05, 0.06, 0.09);
    } else if (dbg >= 40) {               // ÁREA DE SPAWN de la capa de prop (dbg-40).
        // Pinta dónde INSTALARÍA la capa: bandas de clima/forma × densityMap (uPropDensityMap se
        // bindea con el mapa de la capa seleccionada). Es la MISMA regla que el placer, así la
        // vista coincide con los objetos que aparecerían. Sin capas, planeta apagado.
        int layer = dbg - 40;
        if (layer >= 0 && layer < int(uPropCount.x)) {
            col = heatmap(harukaPropCoverage(layer, humid, tempC, slope, bUV));
        } else {
            col = vec3(0.05, 0.06, 0.09);
        }
    }
    fragColor = vec4(col, 1.0);
}
