#version 460 core
#extension GL_GOOGLE_include_directive : require
/**
 * @file ground_field.frag
 * @brief UN CAMPO CENITAL alrededor de un ancla (la camara): lo que pasa por el suelo deja marca, y
 *        la marca se borra con el tiempo —o no se guarda—. No sabe QUIEN lo lee.
 *
 * Usos: hierba tumbada (lee el empuje), huellas en nieve o arena (hundido + reborde), estela en agua
 * (empuje + marca), suelo mojado o quemado (solo la marca)... Cada lector decide que significan los
 * canales; aqui solo se desplaza, se decae y se estampa. Vertex: `fullscreen_tri.vert`.
 *
 * ── LOS CANALES (destino rgba16f) ────────────────────────────────────────────────────────────────
 *   rg = EMPUJE: direccion · fuerza (|rg| ≤ 1). Se queda el mas fuerte, no se suma: dos objetos no
 *        tumban el doble. Decae en magnitud y conserva la direccion.
 *   b  = MARCA / HUNDIDO 0..1, con el borde blando de `uShape.x`.
 *   a  = REBORDE 0..1: el anillo que levanta una pisada (el material desplazado tiene que ir a alguna
 *        parte). Es lo que hace que una huella se lea como huella y no como mancha: se ve por su
 *        sombra propia. `uShape.z` = 0 lo apaga.
 *   `b` y `a` se ACUMULAN (`uShape.w` = 1: un camino muy pisado se hunde mas que una pisada) o se
 *   quedan con el MAXIMO (`uShape.w` = 0: un objeto que se estampa cada frame no satura).
 *
 * ── CADA FRAME ───────────────────────────────────────────────────────────────────────────────────
 * Se lee el campo ANTERIOR desplazado por el salto del ancla (el ancla se mueve a texeles enteros
 * para que el contenido no se emborrone al recentrar), se decae cada canal con su tiempo y se
 * estampan hasta 64 discos. Sin geometria de sellos ni blending: 64 entradas por texel a 512² son
 * 16 M operaciones, nada.
 *
 * Con MEMORIA (tiempo > 0) un sello es un EVENTO: se estampa una vez (la pisada nueva) y el campo lo
 * recuerda y lo borra solo. Sin memoria (tiempo = 0) el campo se repinta entero desde la lista cada
 * frame, y el que llama decide la edad de cada sello por su fuerza.
 */
// Bindings PLANOS, como el resto de shaders: el RHI de Vulkan ya desplaza cada tipo a su rango al
// compilar (`--shift-*-binding`, VKPipeline.cpp). Con `BIND_TEX` de lib/backend.glsl el desplazamiento
// se aplicaba DOS veces (glslang define `VULKAN` solo) y el sampler caía en el binding 133, fuera del
// layout compartido: RADV se caía al crear el pipeline en vez de dar un error.
layout(std140, binding = 0) uniform GroundField {
    vec4 uShift;         // xy = salto del ancla en texeles (nuevo − viejo) · z = texel (m) · w = lado (texeles)
    vec4 uDecay;         // x = s en recuperar el EMPUJE · y = dt (s) · z = s en borrar MARCA y REBORDE · w = (libre)
    vec4 uStamp[64];     // xy = centro (m, marco E/N del ancla NUEVA) · z = radio (m) · w = fuerza 0..1
    vec4 uStampDir[64];  // xy = avance (unitario, 0 = quieto) · z = peso en la MARCA 0..1 · w = 1 si activo
    vec4 uShape;         // x = borde blando (fraccion del radio) · y = cuanto abre hacia fuera el empuje ·
                         // z = ganancia del REBORDE · w = 1 acumular marca/reborde, 0 maximo
};
layout(binding = 5) uniform sampler2D uPrev;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outField;

// Decaimiento lineal hacia 0 en `secs` (0 = sin memoria).
float fade(float v, float secs, float dt) {
    return (secs <= 0.0) ? 0.0 : max(v - dt / secs, 0.0);
}
float combine(float acc, float v, bool add) { return add ? min(acc + v, 1.0) : max(acc, v); }

void main() {
    const float side = uShift.w;
    const vec2 prevUv = vUv + uShift.xy / side;
    vec4 f = vec4(0.0);
    if (all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0))))
        f = texture(uPrev, prevUv);

    vec2  push = f.rg;
    const float m = length(push);
    push = (m > 1e-4) ? push * (fade(m, uDecay.x, uDecay.y) / m) : vec2(0.0);
    float mark = fade(f.b, uDecay.z, uDecay.y);
    float rim  = fade(f.a, uDecay.z, uDecay.y);

    const vec2  pos  = (vUv - 0.5) * side * uShift.z;     // m respecto al centro del campo nuevo
    const float soft = clamp(uShape.x, 0.0, 1.0);
    const bool  add  = uShape.w > 0.5;
    for (int i = 0; i < 64; ++i) {
        if (uStampDir[i].w < 0.5) continue;
        const vec2  d = pos - uStamp[i].xy;
        const float r = max(uStamp[i].z, 1e-4);
        const float t = length(d) / r;                     // 0 en el centro · 1 en el borde
        if (t >= 1.0) continue;
        const float w = uStamp[i].w * (1.0 - smoothstep(1.0 - soft, 1.0, t));
        // Bajo el objeto se empuja hacia donde va; en el borde, ademas, hacia fuera (la pisada abre).
        const vec2 v = uStampDir[i].xy + normalize(d + vec2(1e-4)) * uShape.y;
        if (w > 0.0 && dot(v, v) > 1e-8) {
            const vec2 cand = normalize(v) * w;
            if (dot(cand, cand) > dot(push, push)) push = cand;
        }
        mark = combine(mark, w * uStampDir[i].z, add);
        // El reborde: un anillo en 0,82 del radio, fuera del hueco.
        const float ring = (1.0 - smoothstep(0.16, 0.42, abs(t - 0.82))) * uStamp[i].w * uStampDir[i].z;
        rim  = combine(rim, ring * uShape.z, add);
    }
    outField = vec4(push, mark, rim);
}
