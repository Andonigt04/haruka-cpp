/**
 * @file rhitest_watershape.frag
 * @brief Sombreado del banco para VER la geometria de la ola sobre una forma. Ver watershape.vert.
 *
 * ⚠️ ESTE SOMBREADO NO ES EL DEL MAR y no pretende serlo (el del mar es `lib/ocean_shade.glsl`).
 * Aqui se audita la GEOMETRIA de la ola sobre formas arbitrarias, asi que el sombreado tiene un solo
 * trabajo: revelar la normal. La primera version era un difuso plano y no servia — la pendiente de
 * una ola de mar abierto es de unos 6 grados, que en difuso puro es invisible, y el toro salia liso
 * como si no hubiera ola. Con un especular duro esas mismas pendientes saltan a la vista.
 *
 * Y la ESPUMA va como tinte, no como sustitucion. Tal cual la calcula el motor, a 1,5 m de fondo vale
 * 1,0 en todo el cuadro (la manda el termino de ORILLA, que solo mira la profundidad), asi que
 * mezclarla a saco pintaba la pieza de blanco liso y tapaba justo lo que se venia a mirar. El valor
 * crudo se sigue reportando como numero en el test.
 */
#version 450 core

layout(location = 0) in vec3 vNormal;
layout(location = 1) in float vFoam;
layout(location = 0) out vec4 FragColor;

void main() {
    vec3  N = normalize(vNormal);
    vec3  L = normalize(vec3(0.35, 0.75, 0.55));
    vec3  V = normalize(vec3(0.55, 0.62, 0.56));   // la misma direccion desde la que mira la camara
    vec3  Hv = normalize(L + V);

    float dif = max(dot(N, L), 0.0);
    // Especular duro: es lo que dibuja las crestas. Con exponente alto, un cambio de pocos grados en
    // la normal enciende o apaga el brillo, que es exactamente la sensibilidad que hace falta.
    float spec = pow(max(dot(N, Hv), 0.0), 90.0);
    float rim  = pow(1.0 - max(dot(N, V), 0.0), 3.0);

    vec3 agua = vec3(0.05, 0.22, 0.34) * (0.30 + 0.70 * dif)
              + vec3(0.85, 0.93, 1.00) * spec
              + vec3(0.10, 0.20, 0.28) * rim;

    // Tinte, no lavado: hasta un 45% para que la forma siga leyendose debajo.
    FragColor = vec4(mix(agua, vec3(0.92, 0.95, 0.97), clamp(vFoam, 0.0, 1.0) * 0.45), 1.0);
}
