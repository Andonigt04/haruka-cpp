/**
 * @file rhitest_watershape.vert
 * @brief Aplica la ola del MAR a una forma cualquiera: un anillo cilindrico, un cubo.
 *
 * Existe para responder a una pregunta de diseno con una imagen y no con una opinion: ¿la ola depende
 * de que haya un planeta debajo? El modelo solo necesita tres cosas —posicion, normal de la
 * superficie y profundidad— y construye su marco tangente del propio `up`, asi que en teoria vale
 * para cualquier geometria. Aqui se comprueba: la misma `harukaGerstner` sobre un cilindro (donde
 * `up` es radial en el plano XZ) y sobre un cubo (donde `up` salta de cara en cara).
 *
 * El cubo es el caso duro a proposito: en una arista la normal cambia de golpe, asi que el marco
 * tangente tambien, y si la ola dependiera del marco de forma inestable se veria una costura.
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

#include "lib/ocean_wave.glsl"

layout(location = 0) in vec3 aPos;    // posicion en el espacio de la forma
layout(location = 1) in vec3 aUp;     // normal de la superficie en ese punto

layout(std140, binding = 0) uniform ShapeParams {
    mat4  uViewProj;
    vec4  uMisc;      // x = tiempo · y = profundidad del agua · z = amplitud de la escala · w = sin uso
};

layout(location = 0) out vec3 vNormal;
layout(location = 1) out float vFoam;

void main() {
    vec3  up = normalize(aUp);
    vec3  n; float foam;
    // La posicion en metros que ve la ola: la forma se escala para que las longitudes de onda del
    // mar (decenas de metros) den varias crestas sobre la pieza y no una sola ondulacion.
    vec3  wp = aPos * uMisc.z;
    vec3  disp = harukaGerstner(wp, up, uMisc.x, uMisc.y, HARUKA_FETCH_UNLIMITED, 4.0, 1.0, n, foam);

    vNormal = n;
    vFoam   = foam;
    // ⚠️ EL DESPLAZAMIENTO VUELVE A UNIDADES DE FORMA. `disp` sale en METROS (porque `wp` va en
    // metros) y sumarlo tal cual a `aPos`, que esta en unidades de la pieza, mezcla dos escalas: con
    // 22 m por unidad una ola de 0,35 m movia 0,175 unidades sobre un radio de 0,62 — un 28% del
    // radio. El anillo dejaba de ser un anillo. Dividido por la escala, las proporciones son las
    // REALES: la ola se ve tan grande respecto a la pieza como lo seria en el mar.
    gl_Position = uViewProj * vec4(aPos + disp / uMisc.z, 1.0);
}
