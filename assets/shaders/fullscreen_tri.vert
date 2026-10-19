#version 450 core
#extension GL_GOOGLE_include_directive : require
/**
 * @file fullscreen_tri.vert
 * @brief EL vertex de cualquier pase que recorra el destino texel a texel: post (bloom, present),
 *        fluidos en pantalla, composicion de nubes, LUTs y campos cenitales (`ground_field.frag`).
 *
 * Sin VBO ni layout de vertices: el pipeline va con `vertexLayout` vacio, topologia de triangulos y
 * `draw(3)`. Ver lib/fullscreen.glsl.
 *
 * Out: vUv (location 0), (0,0) abajo a la izquierda. Los fragment lo leen con el nombre que quieran
 * (`TexCoords`, `vUV`…): lo que casa es la location.
 */
#include "lib/backend.glsl"
#include "lib/fullscreen.glsl"

layout(location = 0) out vec2 vUv;

void main() {
    const vec2 ndc = harukaFullscreenNdc(int(VERTEX_INDEX));
    vUv = ndc * 0.5 + 0.5;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
