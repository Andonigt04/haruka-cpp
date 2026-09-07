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

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 FragColor;

void main()
{
    const vec2  sz = vec2(textureSize(u_cloudLo, 0));
    if (sz.x < 2.0 || sz.y < 2.0) { FragColor = vec4(0.0); return; }

    // Bilineal a mano sobre los cuatro téxeles que rodean al píxel.
    const vec2 t  = vUV * sz - 0.5;
    const vec2 i  = floor(t);
    const vec2 f  = t - i;
    const vec2 uv0 = (i + 0.5) / sz;
    const vec2 duv = 1.0 / sz;

    const vec4 c00 = texture(u_cloudLo, uv0);
    const vec4 c10 = texture(u_cloudLo, uv0 + vec2(duv.x, 0.0));
    const vec4 c01 = texture(u_cloudLo, uv0 + vec2(0.0, duv.y));
    const vec4 c11 = texture(u_cloudLo, uv0 + duv);

    const float w00 = (1.0 - f.x) * (1.0 - f.y);
    const float w10 =        f.x  * (1.0 - f.y);
    const float w01 = (1.0 - f.x) *        f.y;
    const float w11 =        f.x  *        f.y;

    // Alfa: bilineal normal. Es una cobertura, y promediarla es lo correcto.
    const float a = w00 * c00.a + w10 * c10.a + w01 * c01.a + w11 * c11.a;
    if (a <= 0.002) { FragColor = vec4(0.0); return; }

    // Color: bilineal PONDERADA POR ALFA. Un téxel sin nube no tiene color que aportar.
    const float wa = w00 * c00.a + w10 * c10.a + w01 * c01.a + w11 * c11.a;
    const vec3  c  = (w00 * c00.a * c00.rgb + w10 * c10.a * c10.rgb +
                      w01 * c01.a * c01.rgb + w11 * c11.a * c11.rgb) / max(wa, 1e-4);

    FragColor = vec4(c, a);
}
