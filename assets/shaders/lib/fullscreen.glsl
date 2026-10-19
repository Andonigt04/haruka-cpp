/**
 * @file fullscreen.glsl
 * @brief EL TRIANGULO QUE CUBRE EL DESTINO, sin VBO: `draw(3)` con topologia de triangulos.
 *
 * Los vertices salen del indice: NDC (-1,-1) (3,-1) (-1,3), que recortado da el [-1,1]² justo y un
 * solo triangulo (sin la diagonal de un quad, que rasteriza dos veces la misma linea de pixeles).
 * UV = NDC·0,5+0,5: (0,0) abajo a la izquierda, igual que el quad de antes.
 *
 * Lo usan `fullscreen_tri.vert` (el vertex de cualquier pase de texel a texel) y los pases que
 * necesitan algo mas que la UV pero comparten el triangulo (`sky.vert`, `cloud_vol.vert`).
 */
#ifndef HARUKA_FULLSCREEN_GLSL
#define HARUKA_FULLSCREEN_GLSL

/// NDC del vertice `vid` (0, 1, 2) del triangulo de pantalla completa.
vec2 harukaFullscreenNdc(int vid) {
    return vec2((vid == 1) ? 3.0 : -1.0, (vid == 2) ? 3.0 : -1.0);
}

/// Direccion de vista en mundo de un punto NDC, con la inversa de (proyeccion · vista sin traslacion).
vec3 harukaFullscreenRay(mat4 invViewProjRot, vec2 ndc) {
    const vec4 far = invViewProjRot * vec4(ndc, 1.0, 1.0);
    return far.xyz / far.w;
}

#endif // HARUKA_FULLSCREEN_GLSL
