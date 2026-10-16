/**
 * @file instance.glsl
 * @brief El stream de INSTANCIA compacto (40 B) y cómo se aplica. Gemelo de `InstanceDataFloat` y
 *        `GPUInstancing::appendInstanceLayout` (Renderer/GPUInstancing.h/.cpp).
 *
 * Antes era una mat4 entera por instancia (96 B) y cada vértice invertía su 3×3 para la normal. Una
 * transformación de prop o pieza es traslación · giro · escala, así que viaja así y se aplica así:
 *   posición = T + R·(s ⊙ p)        normal = R·(n / s)
 * La normal sale exacta (la inversa traspuesta de R·S es R·S⁻¹) y sin `inverse()`.
 *
 * Locations: 3 posición · 4 cuaternión · 5 escala · 7 tinte · 10 máscara de partes rotas.
 * (6 y 8 quedan libres: eran las columnas de la mat4 y la escala suelta.)
 */
#ifndef HARUKA_INSTANCE_GLSL
#define HARUKA_INSTANCE_GLSL

layout(location = 3)  in vec3  iPos;        // traslación, relativa al origen del pase
layout(location = 4)  in vec4  iRot;        // cuaternión (x,y,z,w), snorm16: se renormaliza
layout(location = 5)  in vec3  iScale;      // escala por eje (X negativa = espejo)
layout(location = 7)  in vec4  iColorPk;    // RGBA8: rgb × (a·4) = tinte
layout(location = 10) in float iBreakMask;  // partes ROTAS (bit p = parte p), valor entero en float

/// Gira `v` por el cuaternión unitario `q` (forma de Rodrigues, 2 productos cruz).
vec3 harukaQuatRotate(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

vec4 harukaInstanceQuat() { return normalize(iRot); }

/// Punto del prototipo → espacio del pase (sin la traslación del origen relativo).
vec3 harukaInstancePoint(vec4 q, vec3 p) { return iPos + harukaQuatRotate(q, iScale * p); }

/// Normal del prototipo → espacio del pase. Exacta con escala NO uniforme.
vec3 harukaInstanceNormal(vec4 q, vec3 n) { return normalize(harukaQuatRotate(q, n / iScale)); }

/// El tinte desempaquetado.
vec3 harukaInstanceTint() { return iColorPk.rgb * (iColorPk.a * 4.0); }

/// ¿Está rota la parte `part` de esta instancia?
bool harukaInstancePartBroken(float part) {
    return (uint(iBreakMask) & (1u << uint(part + 0.5))) != 0u;
}

#endif // HARUKA_INSTANCE_GLSL
