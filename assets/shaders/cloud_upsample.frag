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

layout(binding = 0) uniform sampler2D u_cloudLo;
/// ⚠️ LA PROFUNDIDAD DE LA ESCENA, A RESOLUCION COMPLETA — la MISMA copia que leyo el pase de nubes.
/// Con ella la composicion sabe, para cada texel de nube, CONTRA QUE recorto su marcha: basta
/// muestrearla en el centro del texel, que es exactamente donde la leyo el pase. No hace falta que
/// el pase escriba nada mas.
layout(binding = 1) uniform sampler2D u_sceneDepth;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 FragColor;

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
    FragColor = vec4(c / max(wa, 1e-4), a);
}
