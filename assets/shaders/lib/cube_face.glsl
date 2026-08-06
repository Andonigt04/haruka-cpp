// ===========================================================================================
// INVERSA cubo↔esfera — gemelo GLSL de `dirToCubeFaceClosed` en src/core/terrain/cube_sphere.cpp.
//
// La malla del planeta se construye recorriendo (lx,ly) en [-1,1] por cara y llamando a
// `cubeFaceToDir`. Cualquier shader que quiera leer esa retícula desde una DIRECCIÓN tiene que
// deshacer exactamente esa proyección. Hacerlo "casi" no vale: la versión anterior de este código
// usaba la proyección gnómica a secas (dividir por la componente dominante) y encima con el signo
// de v invertido, así que el clipmap leía la altura de OTRO PUNTO DEL PLANETA — normalmente fondo
// oceánico, con lo que la rejilla se hundía kilómetros y no se dibujaba ni un píxel. El síntoma no
// era un error de GL ni un shader que no compila: era simplemente que no se veía nada.
//
// La derivación de la forma cerrada está en cube_sphere.h.
// ⚠️ CUALQUIER CAMBIO VA EN LOS DOS FICHEROS A LA VEZ.
// ===========================================================================================
#ifndef HARUKA_CUBE_FACE_GLSL
#define HARUKA_CUBE_FACE_GLSL

// dir unitaria → (cara 0..5, uv=(lx,ly) en [-1,1]). Las caras son el enum PlanetFace:
// 0 FRONT(+X) · 1 BACK(-X) · 2 TOP(+Y) · 3 BOTTOM(-Y) · 4 RIGHT(+Z) · 5 LEFT(-Z).
void harukaDirToCubeFace(vec3 d, out int face, out vec2 uv) {
    vec3 ad = abs(d);

    // (sj,sk) = las dos componentes NO dominantes, en el orden en que cubeFaceToDir las coloca.
    int   dom;
    float sj, sk;
    if (ad.x >= ad.y && ad.x >= ad.z) { dom = 0; sj = d.y; sk = d.z; }
    else if (ad.y >= ad.z)            { dom = 1; sj = d.x; sk = d.z; }
    else                              { dom = 2; sj = d.x; sk = d.y; }

    float j2 = sj * sj, k2 = sk * sk;
    float T  = j2 + k2;
    float D  = 2.0 * (j2 - k2);                       // = A - B
    float Q  = max(9.0 - 12.0 * T + D * D, 0.0);
    // Forma CONJUGADA de `3 - sqrt(Q)`: en el centro de la cara T→0 y sqrt(Q)→3, y la resta directa
    // se lleva por delante los dígitos significativos (en float32, todos).
    float S  = (12.0 * T - D * D) / (3.0 + sqrt(Q));

    float a = sign(sj) * sqrt(max((S + D) * 0.5, 0.0));
    float b = sign(sk) * sqrt(max((S - D) * 0.5, 0.0));

    if (dom == 0) {
        if (d.x > 0.0) { face = 0; uv = vec2(-b,  a); }   // p = ( 1, ly, -lx)
        else           { face = 1; uv = vec2( b,  a); }   // p = (-1, ly,  lx)
    } else if (dom == 1) {
        if (d.y > 0.0) { face = 2; uv = vec2( a, -b); }   // p = (lx,  1, -ly)
        else           { face = 3; uv = vec2( a,  b); }   // p = (lx, -1,  ly)
    } else {
        if (d.z > 0.0) { face = 4; uv = vec2( a,  b); }   // p = (lx, ly,  1)
        else           { face = 5; uv = vec2(-a,  b); }   // p = (-lx,ly, -1)
    }
}

#endif // HARUKA_CUBE_FACE_GLSL
