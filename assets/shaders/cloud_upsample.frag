/**
 * @file cloud_upsample.frag
 * @brief Compone el pase volumétrico de nubes, dibujado a resolución REDUCIDA, sobre la escena.
 *
 * ── POR QUÉ EXISTE ──────────────────────────────────────────────────────────────────────────────
 * El pase volumétrico cuesta ~80 hashes de ruido POR PASO y ~88 pasos por píxel. Medido en el banco
 * (`cloud_volume_draws`), eso son 8,76 ms a 256x256 — del orden de 150-280 ms a 1920x1080, y bajar
 * los pasos no lo arregla: de 64 a 24 pasos el coste solo cae al 58 %. El término que manda es el
 * PÍXEL, así que lo que hay que bajar es la resolución del pase, no la marcha.
 *
 * ── POR QUÉ NO VALE LA BILINEAL DEL SAMPLER ─────────────────────────────────────────────────────
 * El pase escribe alfa RECTO (`vec4(col, alpha)`), y donde no hay nube el color es NEGRO con alfa 0.
 * Un `texture()` bilineal mezcla ese negro con el blanco de la nube vecina y deja un ribete OSCURO
 * en cada borde. El RHI solo ofrece `BlendMode::Alpha`/`Additive`, así que tampoco se puede pasar a
 * alfa premultiplicado y componer con `One`. La salida: interpolar el color PONDERADO POR ALFA —los
 * téxeles vacíos no aportan color, solo bajan la opacidad— que es lo mismo que haría el premultiplicado
 * pero hecho a mano en cuatro lecturas.
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

#include "lib/cloud_volume.glsl"   // harukaCloudBillows: la coliflor

layout(binding = 0) uniform sampler2D u_cloudLo;
/// ⚠️ LA PROFUNDIDAD DE LA ESCENA, A RESOLUCION COMPLETA — la MISMA copia que leyo el pase de nubes.
/// Con ella la composicion sabe, para cada texel de nube, CONTRA QUE recorto su marcha: basta
/// muestrearla en el centro del texel, que es exactamente donde la leyo el pase. No hace falta que
/// el pase escriba nada mas.
layout(binding = 1) uniform sampler2D u_sceneDepth;

// El rayo de vista de cada píxel (`cloud_vol.vert`) y el UBO de la marcha: con ellos el borde y el
// pincel se anclan a la NUBE (el punto de la capa donde cae el píxel, con la deriva del viento).
layout(location = 0) in vec3 vRayDir;
layout(location = 1) in vec2 vNdc;
layout(location = 0) out vec4 FragColor;

layout(std140, binding = 5) uniform CloudParams {   // ⚠️ el MISMO bloque que `cloud_vol.vert/.frag`
    mat4  u_invViewProjRot;
    vec4  u_planetC;   // xyz = centro del planeta relativo a la cámara · w = radio
    vec4  u_slab;      // x = base · y = techo · z = cobertura · w = precipitación
    vec4  u_sun;       // xyz = hacia el Sol · w = elevación
    vec4  u_sunColor;
    vec4  u_wind;      // xy = deriva · z = tiempo · w = altitud del ojo
    vec4  u_misc;      // z = escala del campo
    vec4  u_vortexPos[4];
    vec4  u_vortexInfo[4];
    vec4  u_vortexN;
    vec4  u_aerial;
    vec4  u_blend;
    vec4  u_vortexEmit[4];
};

/// La dirección del CENTRO de la pantalla (hacia dónde mira la cámara), con la misma inversa.
vec3 harukaFullscreenRayC(mat4 invViewProjRot) {
    const vec4 far = invViewProjRot * vec4(0.0, 0.0, 1.0, 1.0);
    return far.xyz / far.w;
}

/// El punto de la capa de cúmulos donde cae el rayo, relativo al CENTRO DEL PLANETA y anclado al campo
/// (con la deriva del viento, la misma que suma la marcha), y lo que mide un píxel ahí (m).
/// La capa es una esfera a la base + 800 m: el borde y el pincel solo necesitan un punto que se
/// mueva con la nube; la distancia exacta no la tiene la subida (la marcha no la escribe).
bool cloudAnchor(out vec3 pm, out vec3 pc, out vec3 upv, out float mpp) {
    const vec3 rd = normalize(vRayDir);
    const vec3 C = u_planetC.xyz;
    const float Rs = u_planetC.w + u_slab.x + 800.0;
    const float b = dot(rd, C), c = dot(C, C) - Rs * Rs;
    const float disc = b * b - c;
    if (disc < 0.0) return false;
    const float sq = sqrt(disc);
    const float t = (c < 0.0) ? b + sq : b - sq;   // dentro de la esfera: la salida · fuera: la entrada
    if (t <= 0.0) return false;
    const vec3 p = rd * t - C;
    upv = normalize(p);
    const vec3 ref = abs(upv.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    const vec3 east = normalize(cross(ref, upv));
    const vec3 wv = east * u_wind.x + cross(upv, east) * u_wind.y;   // `harukaWindAt` de la marcha
    const float fs = max(u_misc.z, 1.0e-7);
    pm = p + wv / (fs * 1.3);                                          // metros, fijos al campo
    pc = rd * t + wv / (fs * 1.3);                                     // lo mismo, relativo a la CÁMARA (números pequeños)
    mpp = t * max(length(fwidth(rd)), 1.0e-6);
    return true;
}

// ── POR QUE HAY QUE MIRAR LA PROFUNDIDAD AL SUBIR LA RESOLUCION ────────────────────────────────
//
// El pase corre a 1/4 de lado y recorta la marcha contra la escena (`tExit = min(tExit, sceneT)`),
// asi que cada texel de nube tapa 4x4 pixeles de pantalla y decide "cortar o no cortar" con UNA
// muestra de profundidad. En el borde del terreno esa decision es cara o cruz, y como la silueta del
// horizonte es casi horizontal, el recorte de la capa salia en ESCALERA de bloques de 4 px, rectos y
// paralelos al horizonte (Andoni: "se recorta con recortes rectos y parece que estan mas cerca"; su
// hipotesis, correcta: lo recorta el relieve del terreno).
//
// El arreglo es de la COMPOSICION, no del pase: cada pixel de pantalla se queda con las muestras de
// nube que se marcharon contra SU MISMA escena y descarta las del otro lado del borde. Se probo
// arreglarlo en el pase (tomar la profundidad mas lejana o la mas cercana de la huella) y las dos
// fallan por construccion: la lejana deja de cortar (la cordillera pasa de 0 % a 99,3 % de nube
// delante) y la cercana agranda el escalon.
//
// `d` es reversed-Z (`near/dist`), asi que `dTap/dPix` es la razon de DISTANCIAS: comparar asi es
// relativo y no depende de la escala de la escena.
float depthAffinity(float dTap, float dPix) {
    const bool skyTap = (dTap < 1.0e-7), skyPix = (dPix < 1.0e-7);
    if (skyTap != skyPix) return 0.0;   // uno es cielo y el otro escena: lados distintos del borde
    if (skyTap) return 1.0;             // los dos cielo: nada que distinguir
    const float rel = abs(dTap / dPix - 1.0);
    return 1.0 - smoothstep(0.05, 0.35, rel);
}

// ── POR QUÉ CÚBICA Y NO BILINEAL ────────────────────────────────────────────────────────────────
// Reportado: "las nubes se ven como píxeles". A 1/4 de lado el borde de la nube pasa de 0 a 1 en UN
// téxel (la extinción satura Beer-Lambert en dos o tres pasos), y el jitter de arranque es ruido
// blanco por téxel: al subirlo x4 salen bloques de 4 px en el borde y grano de 4 px dentro. La
// bilineal une cuatro téxeles con pesos lineales y deja la escalera intacta. Una B-spline cúbica de
// 4x4 es un filtro PASO BAJO de verdad: cada píxel promedia 16 téxeles con pesos suaves, la
// escalera se convierte en un degradado y el grano del jitter baja ~4x. Lo que se pierde (el
// detalle por debajo de ~4 px) no existe a esa resolución de todos modos.
float bspline(float x) {
    x = abs(x);
    if (x < 1.0) return (4.0 - 6.0 * x * x + 3.0 * x * x * x) / 6.0;
    if (x < 2.0) { const float t = 2.0 - x; return t * t * t / 6.0; }
    return 0.0;
}

void main()
{
    const vec2  vUV = vNdc * 0.5 + 0.5;
    const vec2  sz = vec2(textureSize(u_cloudLo, 0));
    if (sz.x < 2.0 || sz.y < 2.0) { FragColor = vec4(0.0); return; }

    const vec2 t  = vUV * sz - 0.5;
    const vec2 i  = floor(t);
    const vec2 f  = t - i;
    const vec2 duv = 1.0 / sz;

    float wx[4], wy[4];
    for (int k = 0; k < 4; ++k) {
        wx[k] = bspline(f.x - float(k - 1));
        wy[k] = bspline(f.y - float(k - 1));
    }

    // Color PONDERADO POR ALFA: los téxeles vacíos (negros, alfa 0) solo bajan la opacidad, no
    // oscurecen el color — el ribete oscuro del borde que tenía la bilineal a secas.
    // La profundidad de ESTE pixel de pantalla: contra ella se juzga cada muestra de nube.
    const bool haveDepth = (textureSize(u_sceneDepth, 0).x > 1);
    const float dPix = haveDepth ? texture(u_sceneDepth, vUV).r : 0.0;

    float a = 0.0, wa = 0.0, wSum = 0.0;          // con el peso de profundidad
    float aP = 0.0, waP = 0.0;                    // sin el: respaldo si ninguna muestra casa
    vec3  c = vec3(0.0), cP = vec3(0.0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const float wb = wx[x] * wy[y];
            const vec2  uv = (i + vec2(float(x - 1), float(y - 1)) + 0.5) * duv;
            const vec4  s  = texture(u_cloudLo, uv);
            aP  += wb * s.a; waP += wb * s.a; cP += wb * s.a * s.rgb;
            // El texel de nube leyo la profundidad en SU centro, que es este mismo `uv`.
            const float w = haveDepth ? wb * depthAffinity(texture(u_sceneDepth, uv).r, dPix) : wb;
            a  += w * s.a;
            wa += w * s.a;
            wSum += w;
            c  += w * s.a * s.rgb;
        }
    // Si ninguna muestra se marcho contra la misma escena (un pixel aislado del otro lado del borde),
    // se cae al filtro de siempre: mejor la nube de al lado que un agujero.
    if (wSum < 0.02) { a = aP; wa = waP; c = cP; wSum = 1.0; }
    a /= max(wSum, 1.0e-4);
    if (a <= 0.002) { FragColor = vec4(0.0); return; }
    vec3 col = c / max(wa, 1e-4);

    // EL VOLUMEN, EN MANCHAS (anime "sólido"): tres tonos planos — luz, medio tono y sombra azulada —
    // con un paso corto entre ellos. Las fronteras las pone la LUZ de la marcha (la sombra de un bulto
    // sobre otro, la panza, el núcleo a contraluz), así que dibujan la forma de la nube.
    // ⚠️ Antes: una S suave (se leía plana) y pinceladas encima (ruido estirado y luego sellos: "como
    // estirado", "sin trazos"). En las mallas el posterizado dibujaba ANILLOS porque su luz era un
    // degradado liso en círculo; aquí sale de la sombra marchada de cada bulto.
    {
        const float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
        const float t   = clamp((lum - 0.25) / 0.65, 0.0, 1.0);          // 0 sombra honda · 1 luz plena
        // Pasos ANCHOS y la mancha a medias con el degradado: con pasos de 0,08 era "muy agresivo y
        // no progresivo", y en las partes pequeñas (donde la luz no cruza ninguna frontera) no había
        // sombreado ninguno. El degradado mantiene el volumen de lo pequeño; la mancha, el estilo.
        const float qc  = 0.5 * smoothstep(0.22, 0.44, t) + 0.5 * smoothstep(0.60, 0.84, t);
        const float q   = mix(t, qc, 0.55);
        const float want = mix(0.36, 0.92, q);                            // sombra · medio · luz (techo 0,92)
        col *= want / max(lum, 1.0e-3);
        col  = mix(col, col * vec3(0.80, 0.89, 1.10), (1.0 - q) * 0.6);   // la sombra, al azul del cielo
    }

    // ── EL ACABADO, A RESOLUCIÓN COMPLETA ───────────────────────────────────────────────────────
    // La marcha va a 1/8: un bulto de 400 m a 20 km le cabe en 2-3 de sus píxeles y la subida lo
    // difumina — la nube era una MANCHA LISA. Aquí, anclado a la nube, el borde se recorta en FESTÓN:
    // las cúpulas de la coliflor mueven el umbral solo donde la opacidad es de borde; el interior
    // opaco no cambia.
    vec3 pm, pc, upv; float mpp;
    float edgeShift = 0.0;
    if (cloudAnchor(pm, pc, upv, mpp)) {
        // EL PLANO DEL FESTÓN: el que pasa por el punto de la nube MIRANDO A LA CÁMARA (la horizontal
        // perpendicular a la vista y su vertical). ⚠️ Sobre la capa HORIZONTAL, cerca del horizonte un
        // píxel de pantalla en vertical son cientos de metros de capa: el ruido se comprimía en filas
        // y el borde salía en RAYAS horizontales. En este plano un píxel es un píxel en las dos
        // direcciones, y el plano solo gira con el rumbo de la cámara (el festón no nada).
        // ⚠️ EL PLANO ES EL MISMO PARA TODA LA PANTALLA (hacia dónde mira la cámara y su vertical), y
        // el punto va RELATIVO A LA CÁMARA. Con el rayo de cada píxel y el punto referido al centro
        // del planeta (6 400 km), girar el plano lo que gira un píxel movía sus coordenadas ~6 km: el
        // ruido salía distinto en cada píxel — GRANO de arena en todo el borde.
        const vec3 fwd = normalize(harukaFullscreenRayC(u_invViewProjRot));
        const vec3 upc = normalize(-u_planetC.xyz);
        const vec3 s1 = normalize(cross(upc, fwd)), s2 = cross(fwd, s1);
        const vec3 pf = vec3(dot(pc, s1), dot(pc, s2), 0.0);
        // Lo que mide un píxel EN ESTE PLANO (no en la capa): cerca del horizonte la distancia a la
        // capa es enorme pero el plano apenas avanza por píxel, y con `mpp` de la capa el relieve
        // salía gigante en vertical — manchas corridas. El plano es suave en toda la pantalla, así
        // que su derivada es fiable.
        mpp = max(length(fwidth(pf.xy)) * 0.7071, 1.0e-3);
        const float B0 = 380.0, B1 = 150.0;                       // bultos de la coliflor (m)
        const float w0 = smoothstep(2.0, 6.0, B0 / mpp), w1 = smoothstep(2.0, 6.0, B1 / mpp);
        const float bil = (w0 + w1 > 0.001)
            ? (0.65 * w0 * harukaCloudBillows(pf / B0) + 0.35 * w1 * harukaCloudBillows(pf / B1 + 23.7)) / (0.65 * w0 + 0.35 * w1)
            : 0.55;
        // Solo donde YA hay borde de nube: el festón recorta, no crea nube. ⚠️ Sin esto, delante de un
        // objeto cercano (el hacha en la mano) el respaldo del filtro deja una opacidad pequeña de la
        // nube de detrás, y el festón la subía por encima del umbral: el hacha salía pintada de nube.
        edgeShift = 0.50 * (bil - 0.55) * smoothstep(0.08, 0.22, a);
    }

    // EL BORDE NÍTIDO (estilo anime / Tenki no Ko): la opacidad suave de la marcha se ENDURECE (con
    // el festón de arriba). A 1/8 de lado el borde era un degradado de 8-16 px; con la
    // curva pasa a ~2 px. Endurecer aquí cuesta una curva por píxel; marchar a más resolución, 4-16
    // veces más.
    a = smoothstep(0.18, 0.55, a + edgeShift);
    if (a <= 0.002) { FragColor = vec4(0.0); return; }
    FragColor = vec4(col, a);
}
