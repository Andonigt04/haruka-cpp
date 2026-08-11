#version 460 core
/**
 * @file debug_lines.vert
 * @brief Líneas de depuración en coordenadas relativas al ANCLA de la malla de colisión.
 *
 * Existe para dibujar la malla de COLISIÓN encima del terreno. Hacía falta un shader propio y no
 * reutilizar el de alambre del planeta por dos razones: aquel lee el UBO del planeta (que es privado
 * del planeta y trae 7 vec4 de estado que aquí no significan nada), y atar el dibujo de la física al
 * UBO del render habría creado justo la dependencia que este overlay viene a auditar.
 *
 * Las posiciones llegan relativas al ANCLA DE LA MALLA (no a la cámara) y en float: la malla vive en
 * coordenadas de mundo (~6,37e6) y restar en double ANTES de subir es lo único que evita que el alambre
 * tiemble un metro respecto al terreno — el mismo problema de cancelación que se arregló en el terreno.
 *
 * ⚠️ Relativas al ANCLA, y la distinción costó una sesión de diagnóstico. El buffer se sube una vez por
 * reconstrucción de la física (cada 48 m de deriva), así que si las posiciones fueran relativas a la
 * cámara quedarían congeladas contra una cámara vieja; con esta matriz sin traslación el alambre se
 * pegaba al ojo y se deslizaba sobre el terreno al caminar. El ancla no se mueve entre reconstrucciones,
 * y `uViewProjRotOnly` trae doblada la traslación ancla→ojo recalculada cada frame.
 */
layout(location = 0) in vec3 aPosRelAnchor;

layout(std140, binding = 0) uniform DebugLinesUBO {
    mat4 uViewProjRotOnly;   // proyección × vista sin traslación, × traslación ancla→ojo (por frame)
    vec4 uColor;
};

void main() {
    gl_Position = uViewProjRotOnly * vec4(aPosRelAnchor, 1.0);
}
