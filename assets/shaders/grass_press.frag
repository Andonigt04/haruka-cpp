#version 460 core
/**
 * @file grass_press.frag
 * @brief EL MAPA DE PRESION: donde y hacia donde esta aplastada la hierba, y cuanto le queda para
 *        levantarse. Un texel = 25 cm de suelo; el mapa sigue a la camara a saltos de texel entero
 *        (ver GrassRenderer::updatePressure) para que el contenido no se emborrone al recentrar.
 *
 * Cada frame: se lee el mapa ANTERIOR (recolocado por el desplazamiento del ancla), se decae hacia
 * cero (la hierba se recupera en ~`uDecay.x` s) y se estampan hasta 64 objetos en movimiento: un
 * disco de radio `r` con el vector de avance dentro, ponderado por el peso del objeto. La brizna
 * lee `rg` en su raiz (grass.vert) y se tumba en esa direccion. Sin geometria de sellos ni
 * blending: un bucle sobre 64 entradas por texel son 16 M operaciones a 512², nada.
 */
layout(std140, binding = 0) uniform Press {
    vec4 uShift;         // xy = desplazamiento del ancla en texeles (nuevo − viejo) · z = texel (m) · w = lado (texeles)
    vec4 uDecay;         // x = segundos para recuperarse · y = dt (s) · zw = (libre)
    vec4 uStamp[64];     // xy = centro (m, en el marco E/N del ancla NUEVA) · z = radio (m) · w = fuerza 0..1
    vec4 uStampDir[64];  // xy = direccion de avance (unitaria) · z = (libre) · w = 1 si el sello esta activo
};
layout(binding = 5) uniform sampler2D uPrev;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec2 outPress;

void main() {
    const float side = uShift.w;
    // El texel de este pixel en el mapa VIEJO: desplazado por el salto del ancla.
    const vec2 prevUv = vUv + uShift.xy / side;
    vec2 p = vec2(0.0);
    if (all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0))))
        p = texture(uPrev, prevUv).rg;
    // Recuperacion: la magnitud baja linealmente; la direccion se conserva.
    const float m = length(p);
    if (m > 1e-4) p *= max(m - uDecay.y / max(uDecay.x, 0.1), 0.0) / m;

    // Posicion de este texel en metros respecto al centro del mapa nuevo.
    const vec2 pos = (vUv - 0.5) * side * uShift.z;
    vec2 best = p;
    for (int i = 0; i < 64; ++i) {
        if (uStampDir[i].w < 0.5) continue;
        const vec2  d = pos - uStamp[i].xy;
        const float r = uStamp[i].z;
        const float w = uStamp[i].w * (1.0 - smoothstep(r * 0.6, r, length(d)));
        if (w <= 0.0) continue;
        // Bajo el objeto la hierba se tumba hacia donde va; en el borde, hacia fuera (la pisada abre).
        const vec2 dir = normalize(uStampDir[i].xy * 0.7 + normalize(d + vec2(1e-4)) * 0.5);
        const vec2 cand = dir * w;
        if (dot(cand, cand) > dot(best, best)) best = cand;
    }
    outPress = best;
}
